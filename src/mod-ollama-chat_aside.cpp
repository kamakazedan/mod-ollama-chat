#include "mod-ollama-chat_aside.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_gear.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_wipe.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"

#include "AiFactory.h"
#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    std::unordered_map<uint64_t, Clock::time_point> g_playerNext;   // real player -> no aside to them before
    std::unordered_map<uint64_t, Clock::time_point> g_botNext;      // bot -> no aside from it before
    std::unordered_map<uint64_t, uint64_t>          g_lastSubject;  // real player -> whom the last one was about
    AsideStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    bool Chance(uint32_t percent)
    {
        if (percent == 0)
            return false;
        return percent >= 100 || std::uniform_int_distribution<uint32_t>(1, 100)(Rng()) <= percent;
    }

    uint32_t WeightOf(Player* bot)
    {
        auto it = g_AsidePersonalities.find(GetBotPersonality(bot));
        return it == g_AsidePersonalities.end() ? 0 : it->second;
    }

    // Someone who would take a whisper now.
    bool Listening(Player* p)
    {
        return p && p->IsInWorld() && !p->IsInCombat() && !p->isAFK() && !p->isDND() && !p->IsBeingTeleported();
    }

    // The pieces a fact is made of; each template uses the ones it needs.
    struct FactArgs
    {
        std::string line, race, role, duty, gap, dir, item, family, pet, deaths;
    };

    std::string FillFact(const std::string& templ, Player* subject, const FactArgs& f)
    {
        if (templ.empty() || !subject)
            return {};
        const std::string out = SafeFormat(templ,
                                           fmt::arg("subject", subject->GetName()),
                                           fmt::arg("level", subject->GetLevel()),
                                           fmt::arg("line", f.line),
                                           fmt::arg("race", f.race),
                                           fmt::arg("role", f.role),
                                           fmt::arg("duty", f.duty),
                                           fmt::arg("gap", f.gap),
                                           fmt::arg("dir", f.dir),
                                           fmt::arg("item", f.item),
                                           fmt::arg("family", f.family),
                                           fmt::arg("pet", f.pet),
                                           fmt::arg("deaths", f.deaths));
        return out == "[Format Error]" ? std::string() : out;
    }

    // Something real about `x` worth a remark, other than what they just said.
    std::string IdleFact(Player* x, Group* group)
    {
        std::vector<std::string> facts;

        // Their class, spec and job in the group: always there, so the plainer
        // pick when there is anything more particular to go on.
        std::string plain;
        {
            FactArgs f;
            f.race = ChatHelper::FormatRace(x->getRace());
            f.role = CleanRoleForPrompt(ChatHelper::FormatClass(x, AiFactory::GetPlayerSpecTab(x)));
            f.duty = PlayerbotAI::IsTank(x) ? ", the group's tank"
                   : PlayerbotAI::IsHeal(x) ? ", the group's healer"
                                            : "";
            plain = FillFact(g_AsideFactClass, x, f);
        }

        // A level well off the rest of the group's.
        uint32_t sum = 0, n = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* m = ref->GetSource())
                if (m != x)
                {
                    sum += m->GetLevel();
                    ++n;
                }
        if (n)
        {
            const int gap = int(x->GetLevel()) - int(std::lround(double(sum) / n));
            if (std::abs(gap) >= 4)
            {
                FactArgs f;
                f.gap = std::to_string(std::abs(gap));
                f.dir = gap < 0 ? "below" : "above";
                facts.push_back(FillFact(g_AsideFactLevel, x, f));
            }
        }

        // Something they wear.
        const std::string piece = Gear_OnePiece(x);
        if (!piece.empty())
        {
            FactArgs f;
            f.item = piece;
            facts.push_back(FillFact(g_AsideFactGear, x, f));
        }

        // Their pet.
        if (Pet* pet = x->GetPet())
        {
            std::string family;
            if (CreatureTemplate const* t = pet->GetCreatureTemplate())
                if (CreatureFamilyEntry const* f = sCreatureFamilyStore.LookupEntry(t->family))
                    if (f->Name[0] && *f->Name[0])
                        family = f->Name[0];
            if (!family.empty())
            {
                std::transform(family.begin(), family.end(), family.begin(),
                               [](unsigned char ch) { return char(std::tolower(ch)); });
                FactArgs f;
                f.family = family;
                f.pet    = pet->GetName();
                facts.push_back(FillFact(g_AsideFactPet, x, f));
            }
        }

        // How often they have died with this group.
        if (const uint32_t deaths = Wipe_DeathsOf(x); deaths >= 2)
        {
            FactArgs f;
            f.deaths = std::to_string(deaths);
            facts.push_back(FillFact(g_AsideFactDeaths, x, f));
        }

        facts.erase(std::remove(facts.begin(), facts.end(), std::string()), facts.end());
        if (facts.empty() || (!plain.empty() && Chance(25)))
            return plain;
        return facts[std::uniform_int_distribution<size_t>(0, facts.size() - 1)(Rng())];
    }

    // One aside to `to`, about `about`, from a bot of theirs that does this.
    bool TryAside(Player* to, Group* group, Player* about, const std::string& fact, bool onLine)
    {
        const auto now = Clock::now();
        const uint64_t toGuid = to->GetGUID().GetRawValue();
        if (fact.empty() || about == to)
            return false;
        if (auto it = g_playerNext.find(toGuid); it != g_playerNext.end() && now < it->second)
            return false;
        if (auto it = g_lastSubject.find(toGuid); it != g_lastSubject.end() && it->second == about->GetGUID().GetRawValue())
            return false;

        // Who could say it, by how much their style goes in for it.
        std::vector<std::pair<Player*, uint32_t>> who;
        uint32_t total = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* bot = ref->GetSource();
            if (!bot || bot == to || bot == about || !OllamaIsBotPlayer(bot) || !bot->IsInWorld() ||
                bot->IsInCombat() || bot->GetMap() != to->GetMap())
                continue;
            if (auto it = g_botNext.find(bot->GetGUID().GetRawValue()); it != g_botNext.end() && now < it->second)
                continue;
            const uint32_t weight = WeightOf(bot);
            if (!weight)
                continue;
            who.emplace_back(bot, weight);
            total += weight;
        }
        if (who.empty())
            return false;
        uint32_t at = std::uniform_int_distribution<uint32_t>(1, total)(Rng());
        Player* bot = who.back().first;
        uint32_t weight = who.back().second;
        for (const auto& [b, w] : who)
        {
            if (at <= w)
            {
                bot = b;
                weight = w;
                break;
            }
            at -= w;
        }
        // A style that only now and then goes in for it lets the rest go by.
        if (!Chance(weight))
            return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->GetChatHelper() || g_AsidePrompt.empty())
            return false;
        std::string prompt = SafeFormat(g_AsidePrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("player_name", to->GetName()),
                                        fmt::arg("subject", about->GetName()),
                                        fmt::arg("fact", fact));
        if (prompt.empty() || prompt == "[Format Error]")
            return false;
        prompt += Roleplay_BuildVoicePrompt(bot);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = toGuid;
        request.source            = SRC_WHISPER_LOCAL;
        request.scopeKey          = Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_WHISPER_LOCAL], 0, std::string(), 0,
                                                          to->GetZoneId());
        request.directAddress     = true;    // paced here, not by the reply cooldowns
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.originMessage     = g_AsideHistoryNote;
        request.kind              = OllamaRequestKind::ChatReply;
        request.triggerBotReplies = false;
        request.recordHistory     = true;    // a whisper back carries on from it
        request.extraDelayMs      = std::uniform_int_distribution<uint32_t>(2000, 7000)(Rng());
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        if (!OllamaDispatch_Submit(std::move(request)))
            return false;

        g_playerNext[toGuid] = now + std::chrono::minutes(g_AsidePlayerMinutes);
        g_botNext[bot->GetGUID().GetRawValue()] = now + std::chrono::minutes(g_AsideBotMinutes);
        g_lastSubject[toGuid] = about->GetGUID().GetRawValue();
        ++g_stats.whispered;
        ++(onLine ? g_stats.onLines : g_stats.idle);
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} whispers {} an aside about {}: {}", bot->GetName(),
                     to->GetName(), about->GetName(), fact);
        return true;
    }
}

void Aside_NotePartyLine(Player* speaker, const std::string& text, ChatChannelSourceLocal source)
{
    if (!g_AsideEnable || !speaker || (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL))
        return;
    Group* group = speaker->GetGroup();
    if (!group || !speaker->IsInWorld())
        return;
    const std::string clean = Context_CleanText(text);
    if (CrowdText::Words(clean).size() < g_AsideMinWords || !Chance(g_AsideLineChance))
        return;
    FactArgs said;
    said.line = clean;
    const std::string fact = FillFact(g_AsideFactSaid, speaker, said);
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* to = ref->GetSource();
        if (to != speaker && OllamaIsRealPlayer(to) && Listening(to) && TryAside(to, group, speaker, fact, true))
            return;
    }
}

void Aside_Update(uint32_t diff)
{
    if (!g_AsideEnable)
        return;
    static uint32_t timer = 0;
    timer += diff;
    if (timer < std::max<uint32_t>(10, g_AsideCheckSeconds) * 1000)
        return;
    timer = 0;

    const auto now = Clock::now();
    if (g_AsideIdleChance)
    {
        std::unordered_set<uint32_t> looked;
        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            Player* to = entry.second;
            if (!OllamaIsRealPlayer(to) || !Listening(to))
                continue;
            Group* group = to->GetGroup();
            if (!group || group->isBGGroup() || group->isBFGroup() || to->InBattleground() || to->InArena())
                continue;
            if (auto it = g_playerNext.find(to->GetGUID().GetRawValue()); it != g_playerNext.end() && now < it->second)
                continue;
            if (!Chance(g_AsideIdleChance))
                continue;

            std::vector<Player*> others;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    if (m != to && m->IsInWorld() && m->GetMap() == to->GetMap())
                        others.push_back(m);
            std::shuffle(others.begin(), others.end(), Rng());
            for (Player* about : others)
            {
                // Said by whoever would say it: the fact is the same for all.
                const std::string fact = IdleFact(about, group);
                if (!fact.empty())
                {
                    TryAside(to, group, about, fact, false);
                    break;
                }
            }
        }
    }

    // Housekeeping.
    for (auto it = g_playerNext.begin(); it != g_playerNext.end();)
        it = now >= it->second + std::chrono::hours(1) ? g_playerNext.erase(it) : std::next(it);
    for (auto it = g_botNext.begin(); it != g_botNext.end();)
        it = now >= it->second ? g_botNext.erase(it) : std::next(it);
    if (g_lastSubject.size() > 5000)
        g_lastSubject.clear();
}

AsideStats Aside_GetStats()
{
    return g_stats;
}
