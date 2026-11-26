#include "mod-ollama-chat_arrive.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_facts.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_weather.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // What the hook saw: someone playing, and the zone they are now in.
    struct Noted
    {
        uint64_t who  = 0;
        uint32_t zone = 0;
    };
    std::mutex         g_mutex;
    std::vector<Noted> g_noted;   // under g_mutex
    constexpr size_t   kMaxNoted = 256;

    // Where someone playing has been, and the zone they have just crossed
    // into, if that is still to be looked at.
    struct Trip
    {
        uint32_t          zone    = 0;   // where they are, by the last the hook said
        uint32_t          arrived = 0;   // a zone crossed into and not yet looked at; 0: none
        Clock::time_point due{};         // when to look
        Clock::time_point giveUp{};      // still on the way by then: let it go
        Clock::time_point lastSaid{};
        bool              saidEver = false;
        std::unordered_map<uint32_t, Clock::time_point> seen;   // zone -> when they were last in it
    };
    std::unordered_map<uint64_t, Trip> g_trips;   // by who; the world thread's
    ArriveStats g_stats;

    // Settings: the world thread's (the hook reads only g_enable).
    bool        g_enable = true;
    uint32_t    g_chance = 40;
    uint32_t    g_delayMin = 20;
    uint32_t    g_delayMax = 45;
    uint32_t    g_zoneHours = 6;
    uint32_t    g_minMinutes = 15;
    float       g_nearYards = 60.0f;
    std::string g_prompt;
    std::string g_levelsText;
    std::string g_dungeonsText;
    std::string g_weatherText;

    constexpr auto kOnTheWay = std::chrono::seconds(10);   // flying, sailing or fighting: look again then
    constexpr auto kGiveUp   = std::chrono::minutes(5);

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    std::string Filled(const std::string& out)
    {
        return out == "[Format Error]" ? std::string() : out;
    }

    // What is so of the zone, for the prompt: nothing it does not have.
    std::string FactsOf(Player* bot, uint32_t zone, const std::string& name)
    {
        std::string out;
        auto add = [&out](const std::string& part)
        {
            if (!part.empty())
                out += (out.empty() ? "" : " ") + part;
        };
        uint32_t lo = 0, hi = 0;
        if (!g_levelsText.empty() && Facts_ZoneQuestLevels(zone, lo, hi))
            add(Filled(SafeFormat(g_levelsText, fmt::arg("lo", lo), fmt::arg("hi", hi))));
        if (const std::string dungeons = Facts_DungeonsIn(name); !dungeons.empty() && !g_dungeonsText.empty())
            add(Filled(SafeFormat(g_dungeonsText, fmt::arg("dungeons", dungeons))));
        if (const std::string sky = Weather_NameFor(bot); !sky.empty() && !g_weatherText.empty())
            add(Filled(SafeFormat(g_weatherText, fmt::arg("weather", sky))));
        return out;
    }

    // One of the group's bots standing by `player`, free to say something.
    Player* SomeoneBy(Player* player, Group* group)
    {
        std::vector<Player*> free;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player || !member->IsInWorld() || !member->IsAlive() || member->IsInCombat() ||
                !OllamaIsBotPlayer(member) || member->FindMap() != player->FindMap() ||
                !member->IsWithinDistInMap(player, g_nearYards))
                continue;
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(member);
            if (ai && ai->GetChatHelper())
                free.push_back(member);
        }
        return free.empty() ? nullptr : free[urand(0, uint32_t(free.size() - 1))];
    }

    // True when done with this arrival (said, or not to be); false to look again.
    bool Consider(uint64_t who, Trip& trip, Clock::time_point now)
    {
        const uint32_t zone = trip.arrived;
        Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(who));
        if (!g_enable || g_DisableForParty || !player || !player->IsInWorld() || !player->IsAlive() ||
            player->GetZoneId() != zone)
            return true;
        Map* map = player->FindMap();
        Group* group = player->GetGroup();
        if (!map || map->Instanceable() || !group || OllamaInCity(player))
            return true;
        // Still on the way: in the air, on a boat, or fighting its way in.
        if (player->IsInFlight() || player->GetTransport() || player->IsInCombat())
        {
            trip.due = now + kOnTheWay;
            return now >= trip.giveUp;
        }

        Player* bot = SomeoneBy(player, group);
        if (!bot)
        {
            ++g_stats.passed;
            return true;
        }
        ++g_stats.crossed;
        if (trip.saidEver && now - trip.lastSaid < std::chrono::minutes(g_minMinutes))
        {
            ++g_stats.again;
            return true;
        }
        if (!g_chance || urand(1, 100) > g_chance)
        {
            ++g_stats.passed;
            return true;
        }

        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        // Someone has just said something there: not over them.
        if (Context_HumanSpokeSince(bot, source, std::string(), 0, g_ContextAmbientHoldSeconds))
        {
            trip.due = now + kOnTheWay;
            return now >= trip.giveUp;
        }
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           group->GetGUID().GetCounter());
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!area || !area->area_name[0] || !*area->area_name[0] || !ai || g_prompt.empty() ||
            !Governor_CanSend(bot->GetGUID(), scopeKey))
        {
            ++g_stats.passed;
            return true;
        }
        const std::string name = area->area_name[0];
        const std::string facts = FactsOf(bot, zone, name);

        // "{~the look of the place|what the plan is|...}": one each time.
        std::string prompt = SafeFormat(TextRules::ExpandChoices(g_prompt, Rng()),
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality",
                                                 GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("zone", name),
                                        fmt::arg("facts", facts));
        if (prompt.empty() || prompt == "[Format Error]")
        {
            ++g_stats.passed;
            return true;
        }
        prompt += Roleplay_BuildVoicePrompt(bot);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.source            = source;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = true;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        // Nobody asked for it: one written before someone spoke is not sent.
        request.writtenAtSeq      = Context_NextSeq();
        // Asked "what?", it knows what it was on about (mod-ollama-chat_said).
        request.basis = "your group had just arrived in " + name + (facts.empty() ? std::string() : "; " + facts);
        if (OllamaDispatch_Submit(std::move(request)))
        {
            trip.lastSaid = now;
            trip.saidEver = true;
            ++g_stats.said;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} has a word on the group getting to {}.",
                         bot->GetName(), name);
        }
        return true;
    }
}

void OllamaChatArriveScript::OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/)
{
    // A map thread: note who and where, nothing more.
    if (!g_Enable || !g_enable || !player || !newZone || OllamaIsBotPlayer(player))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_noted.size() < kMaxNoted)
        g_noted.push_back({ player->GetGUID().GetRawValue(), newZone });
}

void Arrive_LoadConfig()
{
    auto text = [](const char* key, const char* def)
    {
        return sConfigMgr->GetOption<std::string>(key, def);
    };
    auto number = [](const char* key, uint32_t def, uint32_t lo, uint32_t hi)
    {
        return std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>(key, def), lo, hi);
    };

    g_enable     = sConfigMgr->GetOption<bool>("OllamaChat.Arrive.Enable", true);
    g_chance     = number("OllamaChat.Arrive.Chance", 40, 0, 100);
    g_delayMin   = number("OllamaChat.Arrive.DelayMinSeconds", 20, 1, 600);
    g_delayMax   = std::max(g_delayMin, number("OllamaChat.Arrive.DelayMaxSeconds", 45, 1, 600));
    g_zoneHours  = number("OllamaChat.Arrive.ZoneHours", 6, 1, 168);
    g_minMinutes = number("OllamaChat.Arrive.MinMinutes", 15, 1, 1440);
    g_nearYards  = float(number("OllamaChat.Arrive.NearYards", 60, 10, 200));
    g_prompt = text("OllamaChat.Arrive.Prompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in a group. Your chat style: "
        "{bot_personality} Your group has just got to {zone}. {facts} Say something about getting here in party "
        "chat the way players do -- {~the look of the place|asking what the plan is|whether the place suits your "
        "level|being glad the trip is over|a quick remark in passing} -- in your own style. Say nothing of whether "
        "you have been here before, and make up no reason for coming. One short line, 3 to 12 words, no quotes, no "
        "name in front.");
    g_levelsText = text("OllamaChat.Arrive.LevelsText", "Most of the quests here are for levels {lo} to {hi}.");
    g_dungeonsText = text("OllamaChat.Arrive.DungeonsText", "The way into {dungeons} is in this zone.");
    g_weatherText = text("OllamaChat.Arrive.WeatherText", "Right now there is {weather} here.");
}

void Arrive_Update()
{
    std::vector<Noted> fresh;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fresh.swap(g_noted);
    }
    if (fresh.empty() && g_trips.empty())
        return;
    const auto now = Clock::now();

    for (const Noted& n : fresh)
    {
        Trip& trip = g_trips[n.who];
        if (trip.zone == n.zone)
            continue;
        const bool first = trip.zone == 0;   // the first heard of them: they logged in there
        if (!first)
            trip.seen[trip.zone] = now;      // ...they were in the last one until just now
        trip.zone    = n.zone;
        trip.arrived = 0;
        const auto been = trip.seen.find(n.zone);
        if (first)
            continue;
        if (been != trip.seen.end() && now - been->second < std::chrono::hours(g_zoneHours))
        {
            ++g_stats.again;   // back where they had just been: a border, not a journey
            continue;
        }
        trip.arrived = n.zone;
        trip.due     = now + std::chrono::seconds(urand(g_delayMin, g_delayMax));
        trip.giveUp  = trip.due + kGiveUp;
    }

    for (auto& [who, trip] : g_trips)
        if (trip.arrived && now >= trip.due && Consider(who, trip, now))
            trip.arrived = 0;
}

ArriveStats Arrive_GetStats()
{
    ArriveStats s = g_stats;
    for (const auto& [who, trip] : g_trips)
        if (trip.arrived)
            ++s.pending;
    return s;
}
