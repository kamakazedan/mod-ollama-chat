#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

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
            { "paused",         "paused" },
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

    if (TextRules::MatchPartyIntent(clean, g_PartyCmdRules) == TextRules::PartyIntent::None)
        return PartyCmdLine::None;

    Player* tank = DcLeaderSignal::FindLeaderTank(player);
    if (!tank || !tank->IsInWorld())
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

    // The run's own values. LazyGet only computes one that never has been;
    // mod-dungeon-clear refreshes them itself while the run is on.
    uint32_t killed = 0, left = 0;
    if (auto* value = ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses))
    {
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

PartyCmdStats PartyCmd_GetStats()
{
    return g_stats;
}
