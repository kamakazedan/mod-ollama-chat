#include "mod-ollama-chat_mounts.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_mountstext.h"
#include "mod-ollama-chat_pets.h"
#include "mod-ollama-chat_textrules.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <list>
#include <set>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    MountStats g_stats;

    // ---- the game's mounts, by name --------------------------------------------

    // Read once from the spells that put someone on a mount, the first time a
    // line is checked (the world thread, the spells long loaded): {chestnut,
    // mare}, {swift, brown, horse} (MountsText::NameWords).
    MountsText::Phrases g_names;
    bool g_namesRead = false;

    void ReadNames()
    {
        if (g_namesRead)
            return;
        g_namesRead = true;

        std::set<std::vector<std::string>> seen;
        const uint32 count = sSpellMgr->GetSpellInfoStoreSize();
        for (uint32 id = 1; id < count; ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || !info->SpellName[0] || !*info->SpellName[0] || !info->HasAura(SPELL_AURA_MOUNTED))
                continue;
            std::vector<std::string> words = MountsText::NameWords(TextRules::MountFromSpell(info->SpellName[0]));
            if (!words.empty() && seen.insert(words).second)
                g_names.push_back(std::move(words));
        }
        g_stats.names = static_cast<uint32_t>(g_names.size());
        LOG_INFO("module.ollamachat", "[Ollama Chat] Mounts: {} of the game's mounts known by name.", g_names.size());
    }

    MountsText::Lists Lists()
    {
        ReadNames();
        MountsText::Lists lists;
        lists.names       = &g_names;
        lists.kinds       = &g_MountsKinds;
        lists.words       = &g_MountsWords;
        lists.remarkWords = &g_MountsRemarkWords;
        lists.notRemarks  = &g_MountsNotRemarks;
        lists.mountWords  = &g_PetsMountWords;
        return lists;
    }

    // ---- who is riding what, in sight ------------------------------------------

    struct Rider
    {
        ObjectGuid               guid;
        std::vector<std::string> words;   // its mount's name, as words
        std::string              kind;    // "horse"
    };

    Rider RiderOf(Player* p)
    {
        Rider r;
        r.guid  = p->GetGUID();
        r.words = CrowdText::Words(Pets_MountOf(p));
        r.kind  = MountsText::KindOf(r.words, g_MountsKinds);
        return r;
    }

    std::list<Player*> PlayersInSight(Player* bot)
    {
        const float range = std::max(1.0f, g_MountsSightYards);
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, range, true, false);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, range);
        found.remove_if([bot](Player* p) { return !p || !p->IsInWorld() || (p != bot && !bot->IsWithinLOSInMap(p)); });
        if (std::find(found.begin(), found.end(), bot) == found.end())
            found.push_back(bot);
        return found;
    }

    // Everyone riding a mount within Mounts.SightYards of `bot` and in its
    // sight, the bot itself included -- not one flying overhead on a flight path.
    std::vector<Rider> RidersInSight(Player* bot)
    {
        std::vector<Rider> out;
        for (Player* p : PlayersInSight(bot))
            if (p->IsMounted() && !p->IsInFlight())
                out.push_back(RiderOf(p));
        return out;
    }

    // Anyone in sight with a hunter's pet, a demon or a companion out, the bot
    // included: "cute cat" may be about that.
    bool PetInSight(Player* bot)
    {
        for (Player* p : PlayersInSight(bot))
        {
            const TextRules::Animals a = Pets_Seen(p, std::max(1.0f, g_MountsSightYards)).animals;
            if (!a.petKind.empty() || !a.companion.empty())
                return true;
        }
        return false;
    }

    // ---- remarks on someone's mount, theirs to answer ----------------------------

    struct Remark
    {
        uint64_t                 speaker = 0;
        std::vector<uint64_t>    riders;
        bool                     voice = true;   // /say or /yell; else a party or a raid
        uint64_t                 group = 0;
        uint32_t                 map   = 0;
        float                    x = 0.0f, y = 0.0f, z = 0.0f;
        Clock::time_point        at;
        std::vector<std::string> keys;           // the line's words: as people read it, and as written
    };
    std::deque<Remark> g_remarks;   // oldest first

    bool Voice(ChatChannelSourceLocal s)     { return s == SRC_SAY_LOCAL || s == SRC_YELL_LOCAL; }
    bool GroupChat(ChatChannelSourceLocal s) { return s == SRC_PARTY_LOCAL || s == SRC_RAID_LOCAL; }

    void Prune(Clock::time_point now)
    {
        const auto window = std::chrono::seconds(g_MountsAnswerSeconds);
        while (!g_remarks.empty() && (g_remarks.size() > 64 || now - g_remarks.front().at >= window))
            g_remarks.pop_front();
    }

    // Said where `bot` would be heard saying something in `source`: /say in
    // earshot of where the remark was made, the same party or raid.
    bool Here(const Remark& r, Player* bot, ChatChannelSourceLocal source)
    {
        if (Voice(source))
            return r.voice && bot->GetMapId() == r.map &&
                   bot->GetExactDist(r.x, r.y, r.z) <= std::max(g_SayDistance, g_MountsSightYards) + 10.0f;
        if (GroupChat(source))
        {
            Group* group = bot->GetGroup();
            return !r.voice && group && group->GetGUID().GetRawValue() == r.group;
        }
        return false;
    }

    bool Rides(const Remark& r, uint64_t guid)
    {
        return std::find(r.riders.begin(), r.riders.end(), guid) != r.riders.end();
    }
}

std::string Mounts_KindOfWord(const std::string& word)
{
    return MountsText::KindOfWord(word, g_MountsKinds);
}

std::string Mounts_KindOf(const std::string& mountName)
{
    return MountsText::KindOf(CrowdText::Words(mountName), g_MountsKinds);
}

bool Mounts_TalksOf(const std::string& text)
{
    return g_MountsEnable && MountsText::TalkOf(text, Lists()).Any();
}

MountLineVerdict Mounts_CheckLine(Player* bot, const OllamaChatRequest& request, const std::string& text,
                                  const std::string& origin)
{
    MountLineVerdict v;
    if (!g_Enable || !g_MountsEnable || !bot || !(Voice(request.source) || GroupChat(request.source)))
        return v;

    const uint64_t self = bot->GetGUID().GetRawValue();
    Prune(Clock::now());
    const MountsText::Lists lists = Lists();

    // Someone who spoke to the bot is owed its answer, whatever was said
    // before; the rest wait on a remark on someone's mount.
    const bool waits = !(request.chainDepth == 0 && request.directAddress) && g_MountsAnswerSeconds;

    // An answer to a remark on a mount that is not its own: "she's a beauty,
    // i suppose", from a paladin on a Charger.
    if (waits && !origin.empty())
    {
        const std::string key = MountsText::Join(CrowdText::Words(origin));
        for (auto it = g_remarks.rbegin(); it != g_remarks.rend(); ++it)
        {
            if (!Here(*it, bot, request.source) ||
                std::find(it->keys.begin(), it->keys.end(), key) == it->keys.end())
                continue;
            if (it->speaker != self && !Rides(*it, self))
            {
                v.check = MountLineCheck::NotTheirs;
                ++g_stats.notTheirs;
                return v;
            }
            break;
        }
    }

    // A remark on a mount: on one somebody in sight is riding, now -- unless
    // a person brought mounts up and is answered about them, or the talk it
    // was given is of mounts in general.
    const MountsText::Talk talk = MountsText::TalkOf(text, lists);
    const bool remark = MountsText::IsRemark(talk) &&
                        !(request.chainDepth == 0 && !origin.empty() && MountsText::TalkOf(origin, lists).Any()) &&
                        !MountsText::GeneralTalk(request.basis, lists);
    if (remark)
    {
        v.what = talk.said.empty() ? std::string("a mount") : TextRules::WithArticle(talk.said);

        const std::vector<Rider> riders = RidersInSight(bot);
        Player* target =
            request.targetGuid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(request.targetGuid)) : nullptr;
        const bool toTarget = talk.second && target && target != bot;

        // Said to "you": the one it is said to. Else anyone in sight but the
        // bot. Its own only when it says so ("my charger is slow"), and that
        // is nobody else's to answer: "nice chestnut mare" from one riding
        // one is about somebody else's.
        for (const Rider& r : riders)
        {
            if (r.guid == bot->GetGUID() || (toTarget && r.guid != target->GetGUID()))
                continue;
            if (MountsText::Fits(talk, r.words, r.kind))
                v.riders.push_back(r.guid);
        }
        bool own = false;
        if (v.riders.empty() && talk.first)
            for (const Rider& r : riders)
                own = own || (r.guid == bot->GetGUID() && MountsText::Fits(talk, r.words, r.kind));
        // "cute cat", with a hunter's cat about: a pet, not a mount.
        if (v.riders.empty() && !own && !(MountsText::OnlyPetKinds(talk, g_MountsPetKinds) && PetInSight(bot)))
        {
            v.check = MountLineCheck::NotThere;
            ++g_stats.notThere;
            return v;
        }
    }

    // Something nobody asked for, written before the remark or after it:
    // talking over its answer, or answering it for them. Another remark on
    // the same one's mount is not that (city life: a few wander over to look).
    if (waits && request.writtenAtSeq)
        for (const Remark& r : g_remarks)
        {
            if (r.speaker == self || Rides(r, self) || !Here(r, bot, request.source))
                continue;
            const bool same = std::any_of(v.riders.begin(), v.riders.end(),
                                          [&r](ObjectGuid g) { return Rides(r, g.GetRawValue()); });
            if (!same)
            {
                v.check = MountLineCheck::Held;
                v.riders.clear();
                ++g_stats.held;
                return v;
            }
        }
    return v;
}

void Mounts_NoteDelivered(Player* bot, const OllamaChatRequest& request, const std::string& plain,
                          const MountLineVerdict& verdict)
{
    if (!g_MountsEnable || !bot || verdict.check != MountLineCheck::Ok || verdict.riders.empty())
        return;

    // Their mount is not handed to another bot to remark on for a while
    // (Pets.MountRemarkMinutes): "nice mount" from all who walk by.
    for (ObjectGuid rider : verdict.riders)
        Pets_NoteMountRemarked(rider);
    ++g_stats.remarks;
    if (!g_MountsAnswerSeconds)
        return;

    Remark r;
    r.speaker = bot->GetGUID().GetRawValue();
    for (ObjectGuid rider : verdict.riders)
        r.riders.push_back(rider.GetRawValue());
    r.voice = Voice(request.source);
    if (!r.voice)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return;
        r.group = group->GetGUID().GetRawValue();
    }
    r.map = bot->GetMapId();
    r.x   = bot->GetPositionX();
    r.y   = bot->GetPositionY();
    r.z   = bot->GetPositionZ();
    r.at  = Clock::now();
    // What an answer to it carries as the line it answers: as people read it
    // (a crowd's, a conversation's), and as it was written (a /say exchange's).
    r.keys.push_back(MountsText::Join(CrowdText::Words(plain)));
    if (!request.fixedText.empty())
        r.keys.push_back(MountsText::Join(CrowdText::Words(request.fixedText)));
    g_remarks.push_back(std::move(r));
    Prune(Clock::now());
}

std::vector<ObjectGuid> Mounts_RemarkedOn(Player* bot, const std::string& line)
{
    std::vector<ObjectGuid> out;
    if (!g_MountsEnable || !bot || line.empty())
        return out;
    const uint64_t self = bot->GetGUID().GetRawValue();
    const std::string key = MountsText::Join(CrowdText::Words(line));
    const auto now = Clock::now();
    for (auto it = g_remarks.rbegin(); it != g_remarks.rend(); ++it)
    {
        if (now - it->at > std::chrono::seconds(10))
            break;
        if (it->speaker != self || std::find(it->keys.begin(), it->keys.end(), key) == it->keys.end())
            continue;
        for (uint64_t guid : it->riders)
            out.push_back(ObjectGuid(guid));
        break;
    }
    return out;
}

MountStats Mounts_GetStats()
{
    return g_stats;
}

void Mounts_NoteRetried()
{
    ++g_stats.retried;
}
