#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_orders.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "ChatHelper.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <chrono>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// mod-dungeon-clear is optional: when it is part of the same build its
// headers are on the module include path, and the bridge is compiled in.
// Only its two lightest headers are used, so nothing else of it leaks in.
#if __has_include("DcLeaderSignal.h") && __has_include("DcModuleEnable.h")
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#if __has_include("DcFollowerLifecycle.h")
#include "DcFollowerLifecycle.h"   // HoldsWithStay, where this mod-dungeon-clear has it
#endif
#define OLLAMA_CHAT_DUNGEON_CLEAR 1
#else
#define OLLAMA_CHAT_DUNGEON_CLEAR 0
#endif

// What the run is doing, for bots asked "which boss is next?". Needs a
// mod-dungeon-clear recent enough to keep its run in DcRunState.
#if OLLAMA_CHAT_DUNGEON_CLEAR && __has_include("DcRun.h") && __has_include("DcStatusPublisher.h") && \
    __has_include("DcAnchorDone.h") && __has_include("DungeonBossInfo.h")
#include "DcAnchorDone.h"
#include "DcRun.h"
#include "DcStatusPublisher.h"
#include "DungeonBossInfo.h"
#include "InstanceScript.h"
#define OLLAMA_CHAT_DC_RUN_STATE 1
#else
#define OLLAMA_CHAT_DC_RUN_STATE 0
#endif

namespace
{
    PartyCmdStats g_stats;

#if OLLAMA_CHAT_DUNGEON_CLEAR
    // The tank's last few answers, per group (by the group's guid counter).
    std::unordered_map<uint32_t, std::deque<std::string>> g_recentAcks;

    uint32_t GroupKey(Player* player)
    {
        Group* group = player ? player->GetGroup() : nullptr;
        return group ? group->GetGUID().GetCounter() : 0;
    }

    // " You already said "on it", "lets go" in this group, ..." -- or empty.
    std::string RecentAcksText(Player* tank)
    {
        if (g_PartyCmdRecentAcks == 0 || g_PartyCmdRecentAcksText.empty())
            return "";
        auto it = g_recentAcks.find(GroupKey(tank));
        if (it == g_recentAcks.end() || it->second.empty())
            return "";

        std::string lines;
        for (const std::string& line : it->second)
            lines += (lines.empty() ? "" : ", ") + ("\"" + line + "\"");
        const std::string out = SafeFormat(g_PartyCmdRecentAcksText, fmt::arg("lines", lines));
        return out == "[Format Error]" ? std::string() : out;
    }

#if OLLAMA_CHAT_DC_RUN_STATE
    // mod-dungeon-clear's status codes, in words a bot can use.
    const char* StateWords(const std::string& code)
    {
        static const std::pair<const char*, const char*> table[] = {
            { "paused",         "paused, waiting for someone to say they are ready to go on" },
            { "pulling",        "pulling a pack back to the group" },
            { "fighting_boss",  "fighting the boss" },
            { "fighting_trash", "fighting trash" },
            { "looting",        "looting" },
            { "resting",        "resting or waiting for someone" },
            { "door_blocked",   "stuck at a closed door" },
            { "stalled",        "stuck" },
            { "clearing_room",  "clearing the room before the boss" },
            { "recovering",     "finding a new way after getting stuck" },
            { "pursuing",       "closing in on the boss" },
            { "pathing",        "working out the route" },
            { "moving",         "moving on" },
            { "idle",           "standing by" },
        };
        for (const auto& [key, words] : table)
            if (code == key)
                return words;
        return "";
    }

    // "Waiting on Brelac (out of range)." -> "waiting on Brelac (out of range)"
    std::string DetailWords(std::string detail)
    {
        while (!detail.empty() && (detail.back() == '.' || detail.back() == ' '))
            detail.pop_back();
        if (!detail.empty() && detail[0] >= 'A' && detail[0] <= 'Z')
            detail[0] = static_cast<char>(detail[0] - 'A' + 'a');
        return detail;
    }

    // How many of the dungeon's bosses are down, and how many the run still
    // means to kill (not the ones it skips). The run's own values: LazyGet only
    // computes one that never has been, and mod-dungeon-clear refreshes them
    // itself while the run is on. A list from the last dungeon this bot was in
    // is worked out again, once, for the copy it is in now.
    void BossCounts(Player* owner, AiObjectContext* ctx, uint32_t& killed, uint32_t& left)
    {
        killed = left = 0;
        if (!owner || !ctx)
            return;
        auto* value = ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses);
        if (!value)
            return;
        static std::unordered_map<uint64_t, uint32_t> listFor;   // bot -> instance its list is for
        if (listFor.size() > 5000)
            listFor.clear();
        uint32_t& instance = listFor[owner->GetGUID().GetRawValue()];
        if (instance != owner->GetInstanceId())
        {
            value->Reset();
            instance = owner->GetInstanceId();
        }
        InstanceScript const* inst = owner->GetInstanceScript();
        const uint32_t mask = inst ? inst->GetCompletedEncounterMask() : 0u;
        static const std::unordered_set<uint32> noneSkipped;
        auto* skippedValue = ctx->GetValue<std::unordered_set<uint32>&>(DcKey::Skipped);
        const std::unordered_set<uint32>& skipped = skippedValue ? skippedValue->Get() : noneSkipped;
        for (const DungeonBossInfo& b : value->LazyGet())
        {
            if (b.kind != DungeonAnchorKind::Boss)
                continue;
            if ((b.encounterIndex < 32 && (mask & (1u << b.encounterIndex))) || DcAnchorDoneByInstanceScript(b, inst))
                ++killed;
            else if (!b.skipByDesign && !skipped.count(b.entry))
                ++left;
        }
    }

    std::vector<std::string> SplitTabs(const std::string& s)
    {
        std::vector<std::string> out;
        size_t start = 0;
        for (;;)
        {
            const size_t tab = s.find('\t', start);
            out.push_back(s.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos)
                return out;
            start = tab + 1;
        }
    }
#endif

    enum class Outcome : uint8_t { Started, Paused, Resumed, Waiting };

    // What the pause is for, in words the tank can use.
    std::string PauseReason(const std::string& clean)
    {
        const std::vector<std::string> words = CrowdText::Words(clean);
        auto has = [&](std::initializer_list<const char*> list)
        {
            for (const std::string& w : words)
                for (const char* x : list)
                    if (w == x)
                        return true;
            return false;
        };

        if (has({ "mb", "mana", "manabreak", "drink", "drinking", "oom" }))
            return "a mana break (mb means mana break: they need to drink)";
        if (has({ "brb", "afk", "bio" }))
            return "a short break (they will be right back)";
        if (has({ "eat", "food", "rest" }))
            return "a rest to eat and drink";
        return "a moment";
    }

    // Real players in the group who are not inside, and near the tank, yet.
    std::vector<std::string> MissingPeople(Player* tank)
    {
        std::vector<std::string> missing;
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group)
            return missing;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || OllamaIsBotPlayer(member))
                continue;
            if (!member->IsInWorld() || member->GetMap() != tank->GetMap() ||
                (g_PartyCmdMaxDistance > 0.0f && !member->IsWithinDistInMap(tank, g_PartyCmdMaxDistance)))
                missing.push_back(member->GetName());
        }
        return missing;
    }

    bool Dispatch(Player* issuer, Player* tank, const std::string& action, const std::string& param)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(tank);
        if (!ai)
            return false;
        // Runs the action now; mod-dungeon-clear checks the issuer is a real
        // player in the tank's group, and whispers its reason when it refuses.
        ai->DoSpecificAction(action, Event("dc", param, issuer), true);
        return true;
    }

    // The tank says what happened, in party, as itself.
    void Acknowledge(Player* player, Player* tank, const std::string& msg, uint64_t seq,
                     ChatChannelSourceLocal source, const std::string& scopeKey,
                     Outcome outcome, const std::string& detail)
    {
        const std::string* templ = nullptr;
        switch (outcome)
        {
            case Outcome::Started: templ = &g_PartyCmdHintStart;   break;
            case Outcome::Paused:  templ = &g_PartyCmdHintPause;   break;
            case Outcome::Resumed: templ = &g_PartyCmdHintResume;  break;
            case Outcome::Waiting: templ = &g_PartyCmdHintWaiting; break;
        }
        if (!templ || templ->empty())
            return;

        const std::string recent = RecentAcksText(tank);
        std::string hint = SafeFormat(*templ,
                                      fmt::arg("player_name", player->GetName()),
                                      fmt::arg("player_message", TextRules::StripChatMarkup(msg)),
                                      fmt::arg("reason", detail),
                                      fmt::arg("missing", detail),
                                      fmt::arg("recent_acks", recent),
                                      fmt::arg("pass_token", g_ContextPassToken));
        if (hint == "[Format Error]")
            return;
        // A hint written before {recent_acks} existed still gets it.
        if (templ->find("{recent_acks}") == std::string::npos)
            hint += recent;

        CrowdPlan plan;
        plan.key          = Crowd_KeyFor(scopeKey, source, uint32(player->GetTeamId()));
        plan.scopeKey     = scopeKey;
        plan.source       = source;
        plan.speakerGuid  = player->GetGUID().GetRawValue();
        plan.speakerIsBot = false;
        plan.speakerName  = player->GetName();
        plan.message      = msg;
        plan.messageSeq   = seq;
        plan.kind         = CrowdLineKind::Command;
        plan.listeners    = 1;
        plan.closed       = true;   // one answer, no follow-ups

        CrowdSlot slot;
        slot.botGuid   = tank->GetGUID().GetRawValue();
        slot.addressed = true;     // owed an answer: no pacing cooldowns
        slot.hint      = std::move(hint);
        plan.slots.push_back(std::move(slot));

        Crowd_Start(std::move(plan), 0);
    }

    // Groups whose staying bots are still to be taken along: the run started
    // while dungeon clear held the party at camp (below). The run watch does it.
    std::unordered_set<uint32_t> g_unstayLater;

#if defined(DC_FOLLOWER_LIFECYCLE_HOLDS_WITH_STAY)
    // A healer dungeon clear holds at camp for a pull: its stay is the run's.
    bool HeldByRun(Player* bot)
    {
        return DcFollowerLifecycle::HoldsWithStay(bot->GetGUID());
    }
#endif

    // The run is moving again: bots told to stay go along with it
    // (Orders_ReleaseStays), all but a healer dungeon clear holds at camp with
    // a stay of its own, which it takes off when the pull is done. A
    // mod-dungeon-clear that cannot say which bots those are gets none of it
    // while it holds the party at camp; the run watch tries again after.
    bool ReleaseStaysWhenFree(Player* player)
    {
        if (!g_PartyCmdEnable && !g_OrdersEnable)
            return true;
#if defined(DC_FOLLOWER_LIFECYCLE_HOLDS_WITH_STAY)
        Orders_ReleaseStays(player, &HeldByRun);
#else
        uint32 phase = 0;
        Position camp;
        bool passive = false;
        if ((DcLeaderSignal::GetLeaderPullInfo(player, phase, camp) && DcLeaderSignal::IsPullPhaseHolding(phase)) ||
            (DcLeaderSignal::GetLeaderCampHold(player, camp, passive) && passive))
            return false;
        Orders_ReleaseStays(player);
#endif
        return true;
    }

    // From a line here: now, or by the run watch once it can be.
    void ReleaseStaysFor(Player* player)
    {
        if (!ReleaseStaysWhenFree(player))
            if (Group* group = player->GetGroup())
                g_unstayLater.insert(group->GetGUID().GetCounter());
    }
#endif
}

bool PartyCmd_Available()
{
    return OLLAMA_CHAT_DUNGEON_CLEAR != 0;
}

PartyCmdLine PartyCmd_Classify(Player* player, const std::string& msg, ChatChannelSourceLocal source)
{
#if !OLLAMA_CHAT_DUNGEON_CLEAR
    (void)player; (void)msg; (void)source;
    return PartyCmdLine::None;
#else
    if (!player || (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL))
        return PartyCmdLine::None;
    if (OllamaIsBotPlayer(player) || !player->GetGroup() || !DcModule::IsEnabled())
        return PartyCmdLine::None;

    const std::string clean = Context_CleanText(msg);

    // Its commands work anywhere, not only inside.
    if (TextRules::IsDungeonClearCommand(clean))
        return PartyCmdLine::DcCommand;

    if (!g_PartyCmdEnable)
        return PartyCmdLine::None;

    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())   // dungeons and raids
        return PartyCmdLine::None;

    const TextRules::PartyIntent intent = TextRules::MatchPartyIntent(clean, g_PartyCmdRules);
    if (intent == TextRules::PartyIntent::None)
        return PartyCmdLine::None;

    Player* tank = DcLeaderSignal::FindLeaderTank(player);
    if (!tank || !tank->IsInWorld())
        return PartyCmdLine::None;

    // "ok im done" only means go on when there is a break to end.
    if (intent == TextRules::PartyIntent::Resume && !DcLeaderSignal::IsInPausedDungeonClearRun(player))
        return PartyCmdLine::None;

    return PartyCmdLine::Instruction;
#endif
}

bool PartyCmd_HandleHumanLine(Player* player, const std::string& msg,
                              ChatChannelSourceLocal source, const std::string& scopeKey,
                              uint64_t seq)
{
#if !OLLAMA_CHAT_DUNGEON_CLEAR
    (void)player; (void)msg; (void)source; (void)scopeKey; (void)seq;
    return false;
#else
    if (!g_PartyCmdEnable || !player || !DcModule::IsEnabled())
        return false;
    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL)
        return false;
    if (OllamaIsBotPlayer(player) || !player->GetGroup())
        return false;

    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())   // dungeons and raids
        return false;

    const std::string clean = Context_CleanText(msg);
    const TextRules::PartyIntent intent = TextRules::MatchPartyIntent(clean, g_PartyCmdRules);
    if (intent == TextRules::PartyIntent::None)
        return false;

    // No dungeon-clear tank in this group: just chat.
    Player* tank = DcLeaderSignal::FindLeaderTank(player);
    if (!tank || !tank->IsInWorld())
        return false;

    const bool running = DcLeaderSignal::FindRunOwner(player) != nullptr;
    const bool paused  = running && DcLeaderSignal::IsInPausedDungeonClearRun(player);

    // "ok im done", "full": the end of a break, and nothing else.
    if (intent == TextRules::PartyIntent::Resume && !paused)
        return false;

    if (intent == TextRules::PartyIntent::Pause)
    {
        // Nothing running to pause: ordinary chat (with the glossary, so a
        // "mb" still reads as a mana break).
        if (!running || paused)
            return false;

        Dispatch(player, tank, "dc pause", "pause");
        if (!DcLeaderSignal::IsInPausedDungeonClearRun(player))
        {
            ++g_stats.refused;
            return false;
        }

        ++g_stats.paused;
        Acknowledge(player, tank, msg, seq, source, scopeKey, Outcome::Paused, PauseReason(clean));
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} paused the dungeon clear: '{}'", player->GetName(), clean);
        return true;
    }

    // Start or resume. Already going: ordinary chat.
    if (running && !paused)
        return false;

    const std::vector<std::string> missing = MissingPeople(tank);
    if (!missing.empty())
    {
        ++g_stats.waiting;
        Acknowledge(player, tank, msg, seq, source, scopeKey, Outcome::Waiting, CrowdText::JoinNames(missing));
        return true;
    }

    if (paused)
    {
        Dispatch(player, tank, "dc pause", "resume");
        if (DcLeaderSignal::IsInPausedDungeonClearRun(player))
        {
            ++g_stats.refused;
            return false;
        }
        ++g_stats.resumed;
        ReleaseStaysFor(player);
        Acknowledge(player, tank, msg, seq, source, scopeKey, Outcome::Resumed, std::string());
    }
    else
    {
        Dispatch(player, tank, "dc on", std::string());
        if (!DcLeaderSignal::FindRunOwner(player))
        {
            ++g_stats.refused;
            return false;
        }
        ++g_stats.started;
        ReleaseStaysFor(player);
        Acknowledge(player, tank, msg, seq, source, scopeKey, Outcome::Started, std::string());
    }

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} {} the dungeon clear: '{}'", player->GetName(),
                 paused ? "resumed" : "started", clean);
    return true;
#endif
}

void PartyCmd_NoteAck(Player* bot, const std::string& line)
{
#if OLLAMA_CHAT_DUNGEON_CLEAR
    const uint32_t key = GroupKey(bot);
    if (!key || g_PartyCmdRecentAcks == 0 || line.empty())
        return;
    if (g_recentAcks.size() > 500)
        g_recentAcks.clear();   // groups come and go; a few hundred stale ones are not worth tracking
    std::deque<std::string>& acks = g_recentAcks[key];
    acks.push_back(line);
    while (acks.size() > g_PartyCmdRecentAcks)
        acks.pop_front();
#else
    (void)bot; (void)line;
#endif
}

std::string PartyCmd_GlossaryFor(Player* bot, ChatChannelSourceLocal source)
{
    if (!bot || g_PartyDungeonGlossary.empty())
        return "";
    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL)
        return "";
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return "";
    return g_PartyDungeonGlossary;
}

std::string PartyCmd_RunStateFor(Player* bot, ChatChannelSourceLocal source)
{
#if !OLLAMA_CHAT_DC_RUN_STATE
    (void)bot; (void)source;
    return "";
#else
    if (!bot || g_PartyRunStateText.empty() || !DcModule::IsEnabled())
        return "";
    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL)
        return "";
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon() || !bot->GetGroup())
        return "";

    // The run lives on whoever owns it, alive or not -- in this instance.
    Player* owner = DcLeaderSignal::FindRunOwner(bot);
    if (!owner || !owner->IsInWorld() || owner->GetMap() != map)
        return "";
    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(owner);
    if (!ai)
        return "";
    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx || !DcRun::Of(ctx).enabled)
        return "";

    uint32_t killed = 0, left = 0;
    BossCounts(owner, ctx, killed, left);

    // STATUS \t on \t entry \t name \t stall \t skipped \t state \t detail \t ...
    // The status line is built the way mod-dungeon-clear builds it for its
    // own panel, which also refreshes the next boss -- so a boss that just
    // died is not still "next".
    std::string state, next;
    if (!owner->IsAlive())
    {
        state = "waiting for " + owner->GetName() + " to be brought back";
        if (auto* value = ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss))
            if (const std::optional<DungeonBossInfo> boss = value->LazyGet())
                next = boss->name;
    }
    else
    {
        const std::vector<std::string> f = SplitTabs(DcStatusPublisher::BuildStatusPayload(ai));
        if (f.size() > 3 && f[2] != "0")
            next = f[3];
        state = f.size() > 6 ? StateWords(f[6]) : "";
        const std::string detail = f.size() > 7 ? DetailWords(f[7]) : "";
        if (state.empty())
            state = "on the move";
        if (!detail.empty())
            state += " (" + detail + ")";
    }

    const uint32_t total = killed + left;
    const std::string progress = killed == 0 ? std::string("no bosses are down yet")
                               : fmt::format("{} of {} bosses are down", killed, total);

    const std::string out = SafeFormat(g_PartyRunStateText,
                                       fmt::arg("tank", owner->GetName()),
                                       fmt::arg("progress", progress),
                                       fmt::arg("killed", killed),
                                       fmt::arg("left", left),
                                       fmt::arg("total", total),
                                       fmt::arg("next", !next.empty() ? next
                                                        : (left == 0 && killed > 0) ? std::string("none, they are all down")
                                                                                    : std::string("not picked yet")),
                                       fmt::arg("state", state));
    return out == "[Format Error]" ? std::string() : out;
#endif
}

#if OLLAMA_CHAT_DC_RUN_STATE
namespace
{
    using WatchClock = std::chrono::steady_clock;

    // One group in one copy of a dungeon.
    struct RunWatch
    {
        uint32_t               instanceId = 0;
        WatchClock::time_point firstSeen{};
        WatchClock::time_point lastSeen{};
        bool                   fresh     = false;   // no boss down when they came in
        bool                   greeted   = false;
        bool                   sawRun    = false;
        bool                   announced = false;
        bool                   moving    = false;   // run under way and not paused
        bool                   unstay    = false;   // staying bots still to take along
    };
    std::unordered_map<uint32_t, RunWatch> g_watches;   // by the group's guid counter

    // The tank says something to the whole group without being asked, in its
    // own chat style: OllamaChat.Party.AnnouncePrompt around `situation`.
    bool SubmitTankLine(Player* tank, const std::string& situation)
    {
        if (!tank || situation.empty() || g_PartyAnnouncePrompt.empty())
            return false;
        Group* group = tank->GetGroup();
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(tank);
        if (!group || !ai || !ai->GetChatHelper())
            return false;

        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           group->GetGUID().GetCounter());
        if (!Governor_CanSend(tank->GetGUID(), scopeKey, true))
            return false;

        Map* map = tank->GetMap();
        const std::string personality = GetBotPersonality(tank);
        const std::string recent = RecentAcksText(tank);
        std::string prompt = SafeFormat(g_PartyAnnouncePrompt,
                                        fmt::arg("bot_name", tank->GetName()),
                                        fmt::arg("bot_level", tank->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(tank->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(tank->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(personality)),
                                        fmt::arg("dungeon", map ? std::string(map->GetMapName()) : std::string("the dungeon")),
                                        fmt::arg("situation", situation),
                                        fmt::arg("recent_acks", recent));
        if (prompt.empty() || prompt == "[Format Error]")
            return false;
        if (g_PartyAnnouncePrompt.find("{recent_acks}") == std::string::npos)
            prompt += recent;
        prompt += Roleplay_BuildVoicePrompt(tank);

        OllamaChatRequest request;
        request.botGuid           = tank->GetGUID().GetRawValue();
        request.source            = source;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = tank->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.directAddress     = true;    // nobody else is talking: no pacing cooldown
        request.triggerBotReplies = false;
        request.partyAck          = true;    // remembered, so the next one is worded differently
        request.maxReplyLength    = OllamaMaxReplyLengthFor(tank);
        return OllamaDispatch_Submit(std::move(request));
    }
}
#endif

void PartyCmd_Update(uint32_t diff)
{
#if !OLLAMA_CHAT_DC_RUN_STATE
    (void)diff;
#else
    static uint32_t timer = 0;
    timer += diff;
    if (timer < 2000)
        return;
    timer = 0;
    if (!DcModule::IsEnabled())
        return;
    const bool unstayWanted = g_PartyCmdEnable || g_OrdersEnable;
    if (!g_PartyGreetEnable && !g_PartyDoneEnable && !unstayWanted)
        return;

    const WatchClock::time_point now = WatchClock::now();
    std::unordered_set<uint32_t> looked;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld() || OllamaIsBotPlayer(player))
            continue;
        Group* group = player->GetGroup();
        Map* map = player->GetMap();
        if (!group || !map || !map->IsDungeon() || map->IsBattlegroundOrArena())
            continue;
        const uint32_t key = group->GetGUID().GetCounter();
        if (!looked.insert(key).second)
            continue;   // one look per group

        // Only a bot tank greets the group and calls the finish.
        Player* tank = DcLeaderSignal::FindLeaderTank(player);
        if (!tank || !OllamaIsBotPlayer(tank) || tank->GetMap() != map)
            continue;

        Player* owner = DcLeaderSignal::FindRunOwner(player);
        Player* counted = (owner && owner->GetMap() == map) ? owner : tank;
        PlayerbotAI* countedAi = PlayerbotsMgr::instance().GetPlayerbotAI(counted);
        uint32_t killed = 0, left = 0;
        BossCounts(counted, countedAi ? countedAi->GetAiObjectContext() : nullptr, killed, left);

        RunWatch& w = g_watches[key];
        if (w.instanceId != map->GetInstanceId() || w.firstSeen == WatchClock::time_point{})
        {
            w = RunWatch{};
            w.instanceId = map->GetInstanceId();
            w.firstSeen  = now;
            w.fresh      = killed == 0;
        }
        w.lastSeen = now;
        if (owner)
            w.sawRun = true;

        // The run has just got going, however it was started or carried on:
        // a line here (which took them along already), "dc on", the addon, a
        // door it was waiting at. Whoever is still on a stay goes along.
        const bool moving = owner && !DcLeaderSignal::IsInPausedDungeonClearRun(player);
        if (g_unstayLater.erase(key))
            w.unstay = unstayWanted;
        if (moving && !w.moving)
            w.unstay = unstayWanted;
        w.moving = moving;
        if (!moving)
            w.unstay = false;
        else if (w.unstay && ReleaseStaysWhenFree(player))
            w.unstay = false;

        // "let me know when you're ready": once, when they first come in.
        if (!w.greeted)
        {
            if (owner || killed > 0)
                w.greeted = true;   // under way already
            else if (g_PartyGreetEnable && now - w.firstSeen >= std::chrono::seconds(g_PartyGreetDelaySeconds) &&
                     tank->IsAlive() && !tank->IsInCombat() && !player->IsInCombat())
            {
                // A tank that spoke a moment ago gets a little longer, then
                // the moment has passed.
                if (SubmitTankLine(tank, g_PartyGreetSituation) ||
                    now - w.firstSeen >= std::chrono::seconds(g_PartyGreetDelaySeconds + 30))
                    w.greeted = true;
            }
        }

        // Every boss down: the tank says they are done.
        if (!w.announced && g_PartyDoneEnable && killed > 0 && left == 0 && (w.fresh || w.sawRun) &&
            tank->IsAlive() && !tank->IsInCombat() && !player->IsInCombat())
        {
            SubmitTankLine(tank, g_PartyDoneSituation);
            w.announced = true;
        }
    }

    for (auto it = g_watches.begin(); it != g_watches.end();)
        it = (now - it->second.lastSeen > std::chrono::minutes(30)) ? g_watches.erase(it) : std::next(it);
#endif
}

PartyCmdStats PartyCmd_GetStats()
{
    return g_stats;
}
