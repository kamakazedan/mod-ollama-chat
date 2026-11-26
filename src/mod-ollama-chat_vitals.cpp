#include "mod-ollama-chat_vitals.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"

#include "PlayerbotAI.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t kSlotSeconds = 10;

    // Ten seconds of someone: their health and mana at their lowest, and
    // added up over the looks they were fighting at, for an average.
    struct Slot
    {
        int64_t  index      = 0;     // which ten seconds
        uint8_t  healthLow  = 100;
        uint32_t healthSum  = 0;
        uint8_t  manaLow    = 100;
        uint32_t manaSum    = 0;
        uint16_t fightLooks = 0;     // looks while fighting
        uint16_t manaLooks  = 0;     // ...of them with mana to look at
        bool     hasMana    = false;
        uint8_t  deaths     = 0;
    };

    struct Member
    {
        std::deque<Slot>  slots;
        bool              wasDead = false;
        Clock::time_point seen{};
    };
    std::unordered_map<uint64_t, Member> g_members;   // by player
    // By group: when its healer's "oom" was last weighed.
    std::unordered_map<uint32_t, Clock::time_point> g_oomWeighed;
    VitalsStats                          g_stats;
    const Clock::time_point              g_epoch = Clock::now();

    int64_t SlotNow()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_epoch).count() / kSlotSeconds;
    }

    int64_t WindowSlots()
    {
        return std::max<int64_t>(1, int64_t(g_VitalsMinutes) * 60 / kSlotSeconds);
    }

    Slot& SlotOf(Member& m)
    {
        const int64_t index = SlotNow();
        if (m.slots.empty() || m.slots.back().index != index)
        {
            m.slots.emplace_back();
            m.slots.back().index = index;
        }
        while (!m.slots.empty() && m.slots.front().index <= index - WindowSlots())
            m.slots.pop_front();
        return m.slots.back();
    }

    bool UsesMana(Player* p)
    {
        return p->getPowerType() == POWER_MANA && p->GetMaxPower(POWER_MANA) > 0;
    }

    uint8_t Pct(float value)
    {
        return uint8_t(std::clamp(value + 0.5f, 0.0f, 100.0f));
    }

    void Look(Player* p, Clock::time_point now)
    {
        Member& m = g_members[p->GetGUID().GetRawValue()];
        m.seen = now;
        Slot& s = SlotOf(m);
        if (!p->IsAlive())
        {
            if (!m.wasDead)
                ++s.deaths;
            m.wasDead = true;
            return;
        }
        m.wasDead = false;
        const uint8_t health = Pct(p->GetHealthPct());
        s.healthLow = std::min(s.healthLow, health);
        const bool mana = UsesMana(p);
        uint8_t manaPct = 100;
        if (mana)
        {
            manaPct   = Pct(100.0f * float(p->GetPower(POWER_MANA)) / float(p->GetMaxPower(POWER_MANA)));
            s.hasMana = true;
            s.manaLow = std::min(s.manaLow, manaPct);
        }
        if (p->IsInCombat())
        {
            ++s.fightLooks;
            s.healthSum += health;
            if (mana)
            {
                ++s.manaLooks;
                s.manaSum += manaPct;
            }
        }
    }

    // The last Vitals.Minutes of someone, added up.
    struct Sum
    {
        bool     known     = false;
        bool     fought    = false;
        uint32_t healthLow = 100;
        uint32_t healthAvg = 100;
        bool     hasMana   = false;
        uint32_t manaLow   = 100;
        uint32_t manaAvg   = 100;
        uint32_t deaths    = 0;
    };

    Sum SumOf(Player* p)
    {
        Sum out;
        auto it = g_members.find(p->GetGUID().GetRawValue());
        if (it == g_members.end())
            return out;
        const int64_t oldest = SlotNow() - WindowSlots();
        uint64_t healthSum = 0, manaSum = 0, fightLooks = 0, manaLooks = 0;
        for (const Slot& s : it->second.slots)
        {
            if (s.index <= oldest)
                continue;
            out.known     = true;
            out.healthLow = std::min<uint32_t>(out.healthLow, s.healthLow);
            out.deaths   += s.deaths;
            healthSum    += s.healthSum;
            fightLooks   += s.fightLooks;
            if (s.hasMana)
            {
                out.hasMana = true;
                out.manaLow = std::min<uint32_t>(out.manaLow, s.manaLow);
                manaSum    += s.manaSum;
                manaLooks  += s.manaLooks;
            }
        }
        out.fought = fightLooks > 0 || out.deaths > 0;
        if (fightLooks)
            out.healthAvg = uint32_t((healthSum + fightLooks / 2) / fightLooks);
        else
            out.healthAvg = out.healthLow;
        if (manaLooks)
            out.manaAvg = uint32_t((manaSum + manaLooks / 2) / manaLooks);
        else
            out.manaAvg = out.manaLow;
        return out;
    }

    bool GroupChat(ChatChannelSourceLocal source)
    {
        return source == SRC_PARTY_LOCAL || source == SRC_RAID_LOCAL;
    }

    // The words of `list` in `text`, names in [brackets] aside.
    bool TalksOf(const std::string& text, const std::vector<std::vector<std::string>>& list, std::string& which)
    {
        which.clear();
        if (list.empty() || text.empty())
            return false;
        const std::vector<size_t> found =
            TextRules::FindPhraseIndexes(TextRules::WithoutBracketNames(TextRules::StripChatMarkup(text)), list);
        if (found.empty())
            return false;
        which = TextRules::PhraseKey(list[found.front()]);
        return true;
    }

    std::string Fill(const std::string& templ, uint32_t pct)
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ, fmt::arg("minutes", g_VitalsMinutes), fmt::arg("pct", pct));
        return out == "[Format Error]" ? std::string() : out;
    }
}

namespace
{
    // The healer out of mana says so (Vitals.OomCall*): the one thing a group
    // has to be told, and what a healer types. A typed line in its own chat
    // style; weighed once in a while for a group, said or not.
    void MaybeOomCall(Group* group, Player* healer, uint8_t manaPct, Clock::time_point now)
    {
        const uint32_t key = group->GetGUID().GetCounter();
        auto weighed = g_oomWeighed.find(key);
        if (weighed != g_oomWeighed.end() && now - weighed->second < std::chrono::seconds(g_VitalsOomCallSeconds))
            return;
        g_oomWeighed[key] = now;
        if (g_RoleplayEnable || g_DisableForParty || g_VitalsOomLines.empty() || g_VitalsOomCallChance == 0 ||
            urand(1, 100) > g_VitalsOomCallChance)
            return;
        const std::string line = Personality_StyleCanned(
            healer, g_VitalsOomLines[urand(0, uint32_t(g_VitalsOomLines.size() - 1))]);
        if (line.empty())
            return;

        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        OllamaChatRequest request;
        request.botGuid           = healer->GetGUID().GetRawValue();
        request.source            = source;
        request.scopeKey          = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0, key);
        request.botName           = healer->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.fixedText         = line;
        request.directAddress     = true;    // the moment calls for it: no pacing cooldown
        request.noEcho            = true;    // "oom" is what everyone says
        request.triggerBotReplies = false;   // nobody needs to answer it
        // Asked "how low?", it knows (mod-ollama-chat_said).
        request.basis = fmt::format("you, the healer, were down to {}% mana", uint32_t(manaPct));
        if (OllamaDispatch_Submit(std::move(request)))
            ++g_stats.oomCalls;
    }
}

void Vitals_Update(uint32_t diff)
{
    if (!g_VitalsEnable)
    {
        g_members.clear();
        return;
    }
    static uint32_t timer = 0;
    timer += diff;
    if (timer < 1000)
        return;
    timer = 0;

    const Clock::time_point now = Clock::now();
    std::unordered_set<uint32_t> looked;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* human = entry.second;
        if (!human || !human->IsInWorld() || OllamaIsBotPlayer(human))
            continue;
        Group* group = human->GetGroup();
        if (!group || group->isBGGroup() || group->isBFGroup() || human->InBattleground() || human->InArena())
            continue;
        if (!looked.insert(group->GetGUID().GetCounter()).second)
            continue;
        Player* dry = nullptr;
        uint8_t dryPct = 100;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld())
                continue;
            Look(member, now);
            // Its healer, when that is a bot: how little mana it has left.
            if (g_VitalsOomCall && member->IsAlive() && UsesMana(member) && OllamaIsBotPlayer(member) &&
                PlayerbotAI::IsHeal(member))
            {
                const uint8_t pct =
                    Pct(100.0f * float(member->GetPower(POWER_MANA)) / float(member->GetMaxPower(POWER_MANA)));
                if (pct <= g_VitalsOomCallPct && pct < dryPct)
                {
                    dry    = member;
                    dryPct = pct;
                }
            }
        }
        if (dry)
            MaybeOomCall(group, dry, dryPct, now);
    }

    // Out of such a group, or offline, for longer than is kept: forgotten.
    const auto keep = std::chrono::minutes(g_VitalsMinutes) + std::chrono::seconds(kSlotSeconds);
    for (auto it = g_members.begin(); it != g_members.end();)
        it = now - it->second.seen > keep ? g_members.erase(it) : std::next(it);
    const auto forget = std::chrono::seconds(g_VitalsOomCallSeconds) + std::chrono::minutes(1);
    for (auto it = g_oomWeighed.begin(); it != g_oomWeighed.end();)
        it = now - it->second > forget ? g_oomWeighed.erase(it) : std::next(it);
}

void Vitals_NoteLow(Player* who, int32_t healthPct, int32_t manaPct)
{
    if (!g_VitalsEnable || !who)
        return;
    auto it = g_members.find(who->GetGUID().GetRawValue());
    if (it == g_members.end())
        return;   // not someone being looked at
    Slot& s = SlotOf(it->second);
    if (healthPct >= 0)
        s.healthLow = std::min<uint8_t>(s.healthLow, uint8_t(std::min(healthPct, 100)));
    if (manaPct >= 0)
    {
        s.hasMana = true;
        s.manaLow = std::min<uint8_t>(s.manaLow, uint8_t(std::min(manaPct, 100)));
    }
    ++g_stats.dips;
}

std::string Vitals_PromptFor(Player* bot, ChatChannelSourceLocal source, const std::vector<std::string>& heard)
{
    if (!g_VitalsEnable || !bot || !GroupChat(source) || g_VitalsText.empty() || g_VitalsLineText.empty())
        return {};
    Group* group = bot->GetGroup();
    if (!group)
        return {};
    std::string which;
    if (std::none_of(heard.begin(), heard.end(),
                     [&which](const std::string& line) { return TalksOf(line, g_VitalsTalkWords, which); }))
        return {};

    // The bot first, as "you"; then the rest.
    std::vector<Player*> members{ bot };
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* member = ref->GetSource())
            if (member != bot)
                members.push_back(member);

    std::string lines;
    bool anyone = false, fought = false;
    for (Player* member : members)
    {
        const Sum sum = SumOf(member);
        if (!sum.known)
            continue;
        anyone = true;
        if (!sum.fought)
            continue;
        fought = true;
        const char* role = PlayerbotAI::IsTank(member) ? "the tank" : PlayerbotAI::IsHeal(member) ? "the healer" : "";
        std::string name = member == bot ? std::string("you") : member->GetName();
        if (*role)
            name += std::string(" (") + role + ")";
        std::string mana, died;
        if (sum.hasMana && !g_VitalsManaText.empty())
            mana = SafeFormat(g_VitalsManaText, fmt::arg("mana_low", sum.manaLow), fmt::arg("mana_avg", sum.manaAvg));
        if (sum.deaths && !g_VitalsDiedText.empty())
            died = SafeFormat(g_VitalsDiedText, fmt::arg("deaths", sum.deaths),
                              fmt::arg("times", sum.deaths == 1 ? "once" : std::to_string(sum.deaths) + " times"));
        const std::string line = SafeFormat(g_VitalsLineText,
                                            fmt::arg("name", name),
                                            fmt::arg("health_low", sum.healthLow),
                                            fmt::arg("health_avg", sum.healthAvg),
                                            fmt::arg("mana", mana == "[Format Error]" ? std::string() : mana),
                                            fmt::arg("died", died == "[Format Error]" ? std::string() : died));
        if (!line.empty() && line != "[Format Error]")
            lines += (lines.empty() ? "" : "; ") + line;
    }
    if (!anyone)
        return {};
    std::string out;
    if (!fought || lines.empty())
        out = Fill(g_VitalsQuietText, 0);
    else
    {
        out = SafeFormat(g_VitalsText, fmt::arg("minutes", g_VitalsMinutes), fmt::arg("lines", lines));
        if (out == "[Format Error]")
            out.clear();
    }
    if (!out.empty())
        ++g_stats.told;
    return out;
}

bool Vitals_MayGoOut(Player* bot, ChatChannelSourceLocal source, const std::string& text,
                     const std::string& answering, bool byPerson, std::string& which, std::string& note)
{
    which.clear();
    note.clear();
    if (!g_VitalsEnable || !bot || !GroupChat(source))
        return true;
    Group* group = bot->GetGroup();
    if (!group)
        return true;
    std::string scare, dry;
    const bool saysScare = TalksOf(text, g_VitalsScareWords, scare);
    const bool saysDry   = TalksOf(text, g_VitalsDryWords, dry);
    if (!saysScare && !saysDry)
        return true;

    // How it really was, for everyone in the group being looked at.
    bool known = false, scared = false, dried = false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* member = ref->GetSource())
        {
            const Sum sum = SumOf(member);
            if (!sum.known)
                continue;
            known  = true;
            scared = scared || sum.deaths > 0 || sum.healthLow <= g_VitalsScarePct || !member->IsAlive();
            dried  = dried || (sum.hasMana && sum.manaLow <= g_VitalsDryPct);
        }
    if (!known)
        return true;   // a group nobody is looked at in: nothing to hold it to

    // A person who said so themselves is answered: it was told how it was.
    std::string theirs;
    const bool theySaidScare = byPerson && TalksOf(answering, g_VitalsScareWords, theirs);
    const bool theySaidDry   = byPerson && TalksOf(answering, g_VitalsDryWords, theirs);

    if (saysScare && !scared && !theySaidScare)
    {
        which = scare;
        note  = Fill(g_VitalsScareNote, g_VitalsScarePct);
        ++g_stats.keptIn;
        return false;
    }
    if (saysDry && !dried && !theySaidDry)
    {
        which = dry;
        note  = Fill(g_VitalsDryNote, g_VitalsDryPct);
        ++g_stats.keptIn;
        return false;
    }
    return true;
}

VitalsStats Vitals_GetStats()
{
    VitalsStats s = g_stats;
    s.members = uint32_t(g_members.size());
    return s;
}
