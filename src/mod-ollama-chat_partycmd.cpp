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
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "ChatHelper.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
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

// Skipping a boss someone asks to skip: a mod-dungeon-clear whose `dc skip`
// takes the boss (DcCommandApi.h, DC_COMMAND_API 1).
#if OLLAMA_CHAT_DC_RUN_STATE && __has_include("DcCommandApi.h")
#include "DcCommandApi.h"
#endif
#if OLLAMA_CHAT_DC_RUN_STATE && defined(DC_COMMAND_API) && DC_COMMAND_API >= 1
#define OLLAMA_CHAT_DC_SKIPS 1
#else
#define OLLAMA_CHAT_DC_SKIPS 0
#endif

namespace
{
    PartyCmdStats g_stats;

    // Members a group got while it was a dungeon-finder group, by group, and
    // who last invited whom into a group (the group's leader at the time),
    // by invitee. Raw guids. Written by the group hooks on any thread.
    struct Invite
    {
        uint64_t                              by = 0;
        std::chrono::steady_clock::time_point at{};
    };
    std::mutex g_finderMutex;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> g_finderJoins;
    std::unordered_map<uint64_t, Invite>                        g_invites;

#if OLLAMA_CHAT_DC_RUN_STATE
    // A bot the finder matched into this group -- not one somebody in it
    // invited lately and queued with (the finder rebuilds a part group around
    // the players it adds, so they all join it afresh).
    bool JoinedThroughFinder(Group* group, ObjectGuid member)
    {
        std::lock_guard<std::mutex> lock(g_finderMutex);
        auto it = g_finderJoins.find(group->GetGUID().GetRawValue());
        if (it == g_finderJoins.end() || !it->second.count(member.GetRawValue()))
            return false;
        auto inv = g_invites.find(member.GetRawValue());
        if (inv != g_invites.end() && std::chrono::steady_clock::now() - inv->second.at < std::chrono::hours(6) &&
            group->IsMember(ObjectGuid(inv->second.by)))
            return false;   // invited by someone it is still grouped with
        return true;
    }
#endif

    // "just start the dingeon": the words a line about the run hangs on,
    // read right however they are typed.
    const std::vector<std::string> kTypoKeys = { "dungeon", "instance" };

#if OLLAMA_CHAT_DUNGEON_CLEAR
    // The tank's last few answers, per group (by the group's guid counter).
    std::unordered_map<uint32_t, std::deque<std::string>> g_recentAcks;

    uint32_t GroupKey(Player* player)
    {
        Group* group = player ? player->GetGroup() : nullptr;
        return group ? group->GetGUID().GetCounter() : 0;
    }

    // When the tank last asked the group whether it is ready (the greeting as
    // they come in), by group: for a few minutes after, a plain "yes" or
    // "sure" answers it and starts the run. With nobody saying anything at
    // all, the tank takes it they are ready (Party.ReadyTimeout*).
    struct ReadyAsk
    {
        std::chrono::steady_clock::time_point at{};
        bool                                  spoken = false;   // a real player said something in the group since
        std::chrono::steady_clock::time_point leadCalled{};     // the tank's "ok, pulling" went to be written
    };
    std::unordered_map<uint32_t, ReadyAsk> g_readyAsked;

    bool AnswersReadyAsk(Player* player, const std::string& clean)
    {
        if (g_PartyReadyYesPhrases.empty() || g_PartyReadyWindowSeconds == 0)
            return false;
        auto it = g_readyAsked.find(GroupKey(player));
        if (it == g_readyAsked.end() ||
            std::chrono::steady_clock::now() - it->second.at > std::chrono::seconds(g_PartyReadyWindowSeconds))
            return false;

        // "yep Gruk": the names in the group are not part of the answer.
        std::vector<std::string> nameWords;
        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    for (std::string& w : CrowdText::Words(member->GetName()))
                        nameWords.push_back(std::move(w));
        std::vector<std::string> words;
        for (std::string& w : CrowdText::Words(clean))
            if (std::find(nameWords.begin(), nameWords.end(), w) == nameWords.end())
                words.push_back(std::move(w));
        return TextRules::IsBareYes(words, g_PartyReadyYesPhrases, g_PartyCmdRules.resumeFillers);
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

    // The bosses and stops of `owner`'s run, in the order it takes them: its
    // own list, which LazyGet only works out when it never has been and
    // mod-dungeon-clear refreshes itself while the run is on. A list from the
    // last dungeon this bot was in is worked out again, once, for this one.
    std::vector<DungeonBossInfo> RunBossList(Player* owner, AiObjectContext* ctx)
    {
        auto* value = (owner && ctx) ? ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses) : nullptr;
        if (!value)
            return {};
        static std::unordered_map<uint64_t, uint32_t> listFor;   // bot -> instance its list is for
        if (listFor.size() > 5000)
            listFor.clear();
        uint32_t& instance = listFor[owner->GetGUID().GetRawValue()];
        if (instance != owner->GetInstanceId())
        {
            value->Reset();
            instance = owner->GetInstanceId();
        }
        return value->LazyGet();
    }

    // The bosses skipped in `ctx`'s run (`dc skip`), by entry.
    const std::unordered_set<uint32>& RunSkipped(AiObjectContext* ctx)
    {
        static const std::unordered_set<uint32> none;
        auto* value = ctx ? ctx->GetValue<std::unordered_set<uint32>&>(DcKey::Skipped) : nullptr;
        return value ? value->Get() : none;
    }

    // Is this boss of the run dead, or its encounter done?
    bool BossDone(const DungeonBossInfo& b, InstanceScript const* inst)
    {
        const uint32_t mask = inst ? inst->GetCompletedEncounterMask() : 0u;
        return (b.encounterIndex < 32 && (mask & (1u << b.encounterIndex))) || DcAnchorDoneByInstanceScript(b, inst);
    }

    // How many of the dungeon's bosses are down, and how many the run still
    // means to kill (not the ones it skips, whose names go in `skippedNames`).
    void BossCounts(Player* owner, AiObjectContext* ctx, uint32_t& killed, uint32_t& left,
                    std::vector<std::string>* skippedNames = nullptr)
    {
        killed = left = 0;
        if (!owner || !ctx)
            return;
        InstanceScript const* inst = owner->GetInstanceScript();
        const std::unordered_set<uint32>& skipped = RunSkipped(ctx);
        for (const DungeonBossInfo& b : RunBossList(owner, ctx))
        {
            if (b.kind != DungeonAnchorKind::Boss)
                continue;
            if (BossDone(b, inst))
                ++killed;
            else if (skipped.count(b.entry))
            {
                if (skippedNames && !b.skipByDesign)
                    skippedNames->push_back(b.name);
            }
            else if (!b.skipByDesign)
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

    // Paused is a stop the tank waits out until someone says they are ready;
    // Break is a rest that ends by itself once everyone has had it.
    enum class Outcome : uint8_t { Started, Paused, Break, Resumed, Waiting };

    // What a pause is for. A rest ("mb", "need to drink", "lets eat") is over
    // when everyone has their health and mana back; someone stepping away
    // ("brb", even "brb need mana") or a plain "wait" is over when they say so.
    enum class BreakKind : uint8_t { Mana, Rest, Away, Moment };

    BreakKind KindOfBreak(const std::string& clean)
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

        if (has({ "brb", "afk", "bio" }))
            return BreakKind::Away;
        if (has({ "mb", "mana", "manas", "manabreak", "drink", "drinking", "oom", "regen" }))
            return BreakKind::Mana;
        if (has({ "eat", "food", "rest" }))
            return BreakKind::Rest;
        return BreakKind::Moment;
    }

    // What the pause is for, in words the tank can use.
    const char* PauseReason(BreakKind kind)
    {
        switch (kind)
        {
            case BreakKind::Mana: return "a mana break (mb means mana break: they need to drink)";
            case BreakKind::Rest: return "a rest to eat and drink";
            case BreakKind::Away: return "a short break (they will be right back)";
            default:              return "a moment";
        }
    }

    // A rest someone called in party, while it is watched for its end
    // (Party.BreakEnd*): by the group's guid counter. Gone once the run moves
    // again however that came about, once someone says "brb" or "wait" in
    // it, or once it has gone on too long to end by itself.
    struct Break
    {
        uint64_t                              caller     = 0;   // who asked for it
        uint32_t                              instanceId = 0;
        std::chrono::steady_clock::time_point startedAt{};
        std::chrono::steady_clock::time_point calledAt{};       // the tank's "lets go" went to be written
    };
    std::unordered_map<uint32_t, Break> g_breaks;

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

    // The tank answers `msg`, in party, as itself, and nobody else does: `hint`
    // is what it is told.
    void TankAnswers(Player* player, Player* tank, const std::string& msg, uint64_t seq,
                     ChatChannelSourceLocal source, const std::string& scopeKey, std::string hint)
    {
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
            case Outcome::Break:
                templ = g_PartyCmdHintBreak.empty() ? &g_PartyCmdHintPause : &g_PartyCmdHintBreak;
                break;
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

        TankAnswers(player, tank, msg, seq, source, scopeKey, std::move(hint));
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

#if OLLAMA_CHAT_DC_SKIPS
    // ---- skipping a boss -----------------------------------------------------------

    // One boss of a run, the way a skip ask sees it.
    struct RunBoss
    {
        uint32_t    entry   = 0;
        std::string name;
        size_t      at      = 0;       // where it is in the run's list of bosses and stops
        bool        done    = false;   // dead, its encounter done
        bool        skipped = false;   // skipped this run, or to be once it starts
    };

    // The run a skip ask is about: whose it is -- the tank's, before it starts
    // -- its bosses in the order it takes them, and what it is heading for.
    struct RunView
    {
        Player*              owner   = nullptr;
        AiObjectContext*     ctx     = nullptr;
        bool                 running = false;
        std::vector<RunBoss> bosses;          // bosses only, in the order the run takes them
        int                  current = -1;    // the boss it is heading for (an index into bosses), -1 none
        bool                 atBoss  = false; // ...for that boss itself, not a stop or an event on the way
    };

    // Skips asked for before the run starts, by group: done once it does.
    struct PendingSkips
    {
        uint32_t                              instanceId = 0;
        std::vector<uint32_t>                 entries;
        std::chrono::steady_clock::time_point at{};
    };
    std::unordered_map<uint32_t, PendingSkips> g_pendingSkips;

    // Each group's last skip, for "no dont skip", "unskip him".
    std::unordered_map<uint32_t, PendingSkips> g_lastSkips;

    bool Has(const std::vector<uint32_t>& entries, uint32_t entry)
    {
        return std::find(entries.begin(), entries.end(), entry) != entries.end();
    }

    // The run in `player`'s copy of the dungeon, or the one the tank would
    // lead. `withCurrent`: what it is heading for too (it asks the run).
    bool ViewRun(Player* player, Player* tank, RunView& view, bool withCurrent = true)
    {
        Map* map = player->GetMap();
        Player* owner = DcLeaderSignal::FindRunOwner(player);
        view.running = owner && owner->IsInWorld() && owner->GetMap() == map;
        view.owner   = view.running ? owner : tank;
        PlayerbotAI* ai = view.owner ? PlayerbotsMgr::instance().GetPlayerbotAI(view.owner) : nullptr;
        view.ctx = ai ? ai->GetAiObjectContext() : nullptr;
        if (!map || !view.ctx)
            return false;

        const std::vector<DungeonBossInfo> list = RunBossList(view.owner, view.ctx);
        InstanceScript const* inst = view.owner->GetInstanceScript();
        const std::unordered_set<uint32>& skipped = RunSkipped(view.ctx);
        static const std::vector<uint32_t> nonePending;
        const std::vector<uint32_t>* pending = &nonePending;
        if (!view.running)
        {
            auto it = g_pendingSkips.find(GroupKey(player));
            if (it != g_pendingSkips.end() && it->second.instanceId == map->GetInstanceId())
                pending = &it->second.entries;
        }
        for (size_t i = 0; i < list.size(); ++i)
        {
            const DungeonBossInfo& b = list[i];
            if (b.kind != DungeonAnchorKind::Boss || b.skipByDesign)
                continue;
            RunBoss boss;
            boss.entry   = b.entry;
            boss.name    = b.name;
            boss.at      = i;
            boss.done    = BossDone(b, inst);
            boss.skipped = view.running ? skipped.count(b.entry) > 0 : Has(*pending, b.entry);
            view.bosses.push_back(std::move(boss));
        }
        if (view.bosses.empty() || !withCurrent)
            return !view.bosses.empty();

        // What it heads for: the run's own next stop, and the first boss at or
        // after it still to do (the first one of all, before it starts).
        size_t from = 0;
        view.atBoss = true;
        if (view.running)
        {
            auto* value = view.ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss);
            const std::optional<DungeonBossInfo> next =
                value ? (view.owner->IsAlive() ? value->Get() : value->LazyGet()) : std::nullopt;
            if (!next)
                return true;   // nothing left
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].entry == next->entry)
                {
                    from = i;
                    break;
                }
            view.atBoss = next->kind == DungeonAnchorKind::Boss;
        }
        for (int pass = 0; pass < 2 && view.current < 0; ++pass)
            for (size_t k = 0; k < view.bosses.size(); ++k)
            {
                const RunBoss& b = view.bosses[k];
                if (b.done || b.skipped || (pass == 0 && b.at < from))
                    continue;
                view.current = int(k);
                break;
            }
        if (view.running && view.current >= 0 && view.bosses[view.current].at != from)
            view.atBoss = false;
        return true;
    }

    // The run's last boss still to do, or its very last one with `evenDone`.
    int LastBoss(const RunView& view, bool evenDone = false)
    {
        for (size_t k = view.bosses.size(); k-- > 0;)
            if (evenDone || (!view.bosses[k].done && !view.bosses[k].skipped))
                return int(k);
        return -1;
    }

    // Someone in `player`'s group is fighting this boss right now.
    bool Fighting(Player* player, uint32_t entry)
    {
        Group* group = player->GetGroup();
        if (!group)
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && member->GetMap() == player->GetMap() && member->IsInCombat())
                    if (Unit* victim = member->GetVictim())
                        if (victim->GetEntry() == entry && victim->IsAlive())
                            return true;
        return false;
    }

    // The tank answers a skip: `templ` with what it is told.
    void SkipAck(Player* player, Player* tank, const std::string& msg, uint64_t seq, ChatChannelSourceLocal source,
                 const std::string& scopeKey, const std::string& templ, const std::string& skipped,
                 const std::string& next, const std::string& plan, const std::string& reason)
    {
        if (templ.empty())
            return;
        const std::string recent = RecentAcksText(tank);
        std::string hint = SafeFormat(templ,
                                      fmt::arg("player_name", player->GetName()),
                                      fmt::arg("player_message", TextRules::StripChatMarkup(msg)),
                                      fmt::arg("skipped", skipped),
                                      fmt::arg("next", next.empty() ? std::string("nothing: the run is over") : next),
                                      fmt::arg("plan", plan),
                                      fmt::arg("reason", reason),
                                      fmt::arg("recent_acks", recent),
                                      fmt::arg("pass_token", g_ContextPassToken));
        if (hint == "[Format Error]")
            return;
        if (templ.find("{recent_acks}") == std::string::npos)
            hint += recent;
        TankAnswers(player, tank, msg, seq, source, scopeKey, std::move(hint));
    }

    void NoteLastSkip(Player* player, const std::vector<uint32_t>& entries)
    {
        if (g_lastSkips.size() > 500)
            g_lastSkips.clear();
        PendingSkips& last = g_lastSkips[GroupKey(player)];
        last.instanceId = player->GetMap() ? player->GetMap()->GetInstanceId() : 0;
        last.entries    = entries;
        last.at         = std::chrono::steady_clock::now();
    }

    // The run has started: what was asked to be skipped before it did is.
    void ApplyPendingSkips(Player* player, Player* tank)
    {
        auto it = g_pendingSkips.find(GroupKey(player));
        if (it == g_pendingSkips.end())
            return;
        const PendingSkips pending = std::move(it->second);
        g_pendingSkips.erase(it);
        Map* map = player->GetMap();
        if (!tank || !map || pending.instanceId != map->GetInstanceId() || !DcLeaderSignal::FindRunOwner(player))
            return;
        for (uint32_t entry : pending.entries)
            Dispatch(player, tank, "dc skip", std::to_string(entry));
        NoteLastSkip(player, pending.entries);
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] The run in {} has started: {} boss(es) {} asked to skip "
                     "before it did skipped now.", map->GetMapName(), pending.entries.size(), player->GetName());
    }

    // The boss names a skip ask is read against: the run's, or the one the
    // tank would lead.
    std::vector<std::string> SkipNames(Player* player, Player* tank)
    {
        RunView view;
        std::vector<std::string> names;
        if (ViewRun(player, tank, view, false))
            for (const RunBoss& b : view.bosses)
                names.push_back(b.name);
        return names;
    }

    enum class SkipResult : uint8_t
    {
        NotOne,    // no skip ask, or one about nothing: ordinary chat
        Handled,   // done, or said no to, and answered
        Refused    // mod-dungeon-clear said no, and whispered why
    };

    // "skip the next boss", "skip oggleflint", "dont skip him": done and
    // answered by the tank. `intent`: "ready, skip oggleflint" starts the run
    // as well, and "ok im done, skip him" carries on a paused one.
    SkipResult HandleSkip(Player* player, Player* tank, const std::string& msg, const std::string& clean,
                          uint64_t seq, ChatChannelSourceLocal source, const std::string& scopeKey,
                          TextRules::PartyIntent intent)
    {
        using TextRules::SkipKind;
        RunView view;
        if (!ViewRun(player, tank, view))
            return SkipResult::NotOne;
        std::vector<std::string> names;
        for (const RunBoss& b : view.bosses)
            names.push_back(b.name);
        const TextRules::SkipAsk ask = TextRules::MatchSkipAsk(clean, g_PartySkipRules, names);
        if (ask.kind == SkipKind::None)
            return SkipResult::NotOne;

        Map* map = player->GetMap();
        const uint32_t key = GroupKey(player);
        const auto now = std::chrono::steady_clock::now();
        auto nameOf = [&](size_t k) { return view.bosses[k].name; };

        if (ask.kind == SkipKind::Unskip || ask.kind == SkipKind::Do)
        {
            // Only one that is skipped comes back: "lets do jergosh" about one
            // that is not is ordinary chat.
            std::vector<size_t> targets;
            auto add = [&](size_t k)
            {
                if (view.bosses[k].skipped && std::find(targets.begin(), targets.end(), k) == targets.end())
                    targets.push_back(k);
            };
            for (size_t k : ask.bosses)
                add(k);
            if (ask.kind == SkipKind::Unskip && ask.next)
            {
                auto it = g_lastSkips.find(key);
                if (it != g_lastSkips.end() && it->second.instanceId == map->GetInstanceId() &&
                    now - it->second.at < std::chrono::minutes(15))
                    for (size_t k = 0; k < view.bosses.size(); ++k)
                        if (Has(it->second.entries, view.bosses[k].entry))
                            add(k);
            }
            if (targets.empty())
                return SkipResult::NotOne;

            const bool later = CrowdText::ContainsPhrase(CrowdText::Words(clean), g_PartySkipLaterWords);
            std::vector<std::string> back;
            bool turnBack = false;
            if (!view.running)
            {
                auto it = g_pendingSkips.find(key);
                for (size_t k : targets)
                {
                    if (it != g_pendingSkips.end())
                        it->second.entries.erase(std::remove(it->second.entries.begin(), it->second.entries.end(),
                                                             view.bosses[k].entry),
                                                 it->second.entries.end());
                    back.push_back(nameOf(k));
                }
            }
            else
            {
                // One behind the run is gone back for now ("...later": once
                // everything ahead is done); one ahead comes in its turn.
                for (size_t k : targets)
                {
                    const RunBoss& b = view.bosses[k];
                    const bool behind = view.current >= 0 && b.at < view.bosses[size_t(view.current)].at;
                    const bool goNow  = behind && !later && !turnBack;
                    Dispatch(player, tank, goNow ? "dc go" : "dc unskip", std::to_string(b.entry));
                    if (!RunSkipped(view.ctx).count(b.entry))
                    {
                        back.push_back(b.name);
                        turnBack = turnBack || goNow;
                    }
                }
                if (back.empty())
                {
                    ++g_stats.refused;
                    return SkipResult::Refused;
                }
            }
            if (auto it = g_lastSkips.find(key); it != g_lastSkips.end())
                for (size_t k : targets)
                    it->second.entries.erase(std::remove(it->second.entries.begin(), it->second.entries.end(),
                                                         view.bosses[k].entry),
                                             it->second.entries.end());
            g_stats.unskips += back.size();

            RunView after;
            ViewRun(player, tank, after);
            const std::string next = after.current >= 0 ? after.bosses[size_t(after.current)].name : std::string();
            const char* it = back.size() > 1 ? "them" : "it";
            std::string plan;
            if (!view.running)
                plan = fmt::format("{} back in the run, which has not started yet.",
                                   back.size() > 1 ? "they are" : "it is");
            else if (turnBack)
                plan = fmt::format("you turn back for {} now.", back.front());
            else if (later && !next.empty())
                plan = fmt::format("you will go back for {} once everything ahead is done; for now you are heading for "
                                   "{}.", it, next);
            else if (!next.empty())
                plan = fmt::format("you will get to {} in {} turn; for now you are heading for {}.", it,
                                   back.size() > 1 ? "their" : "its", next);
            else
                plan = fmt::format("you will get to {} next.", it);
            SkipAck(player, tank, msg, seq, source, scopeKey, g_PartyCmdHintUnskip, CrowdText::JoinNames(back), next,
                    plan, std::string());
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} took a skip back in {}: {} ('{}').", player->GetName(),
                         map->GetMapName(), CrowdText::JoinNames(back), clean);
            return SkipResult::Handled;
        }

        // What to skip: the bosses named, what the run heads for, the last
        // one -- or every boss before the one to skip to.
        std::vector<size_t> targets;
        std::string why;   // the first reason one of them cannot be
        auto add = [&](size_t k)
        {
            if (std::find(targets.begin(), targets.end(), k) == targets.end())
                targets.push_back(k);
        };
        std::string asked;   // what they asked to skip, in words
        int dest = -1;       // skipping to: where
        if (ask.kind == SkipKind::Skip)
        {
            for (size_t k : ask.bosses)
                add(k);
            if (ask.next)
            {
                if (view.current < 0)
                    why = "there is no boss left to skip.";
                else if (!view.atBoss && !ask.nextSaysBoss)
                    return SkipResult::NotOne;   // "skip it" on the way to an event: not about a boss
                else
                    add(size_t(view.current));
            }
            if (ask.last)
            {
                if (const int last = LastBoss(view, true); last >= 0)
                    add(size_t(last));
            }
            std::vector<std::string> said;
            for (size_t k : targets)
                said.push_back(nameOf(k));
            asked = said.empty() ? std::string("the next boss") : CrowdText::JoinNames(said);
        }
        else
        {
            dest = !ask.bosses.empty() ? int(ask.bosses.front()) : LastBoss(view, true);
            if (dest < 0)
                return SkipResult::NotOne;
            asked = "everything before " + nameOf(size_t(dest));
            if (view.bosses[size_t(dest)].done)
                why = nameOf(size_t(dest)) + " is already dead.";
            else
            {
                for (size_t k = 0; k < size_t(dest); ++k)
                    if (!view.bosses[k].done && !view.bosses[k].skipped)
                        add(k);
                if (targets.empty())
                    why = nameOf(size_t(dest)) + " is next anyway, so there is nothing to skip.";
            }
        }

        std::vector<size_t> skip;
        for (size_t k : targets)
        {
            const RunBoss& b = view.bosses[k];
            std::string no;
            if (b.done)
                no = b.name + " is already dead.";
            else if (b.skipped)
                no = "you are skipping " + b.name + " already.";
            else if (view.running && int(k) == view.current && Fighting(player, b.entry))
                no = "you are fighting " + b.name + " right now, so it is too late to skip it.";
            if (no.empty())
                skip.push_back(k);
            else if (why.empty())
                why = no;
        }
        if (skip.empty())
        {
            // "ready, skip oggleflint" with him skipped already: the ready is
            // what is left of it.
            if (why.empty() || intent == TextRules::PartyIntent::Start || intent == TextRules::PartyIntent::Resume)
                return SkipResult::NotOne;
            ++g_stats.skipsRefused;
            SkipAck(player, tank, msg, seq, source, scopeKey, g_PartyCmdHintSkipNo, asked,
                    view.current >= 0 ? nameOf(size_t(view.current)) : std::string(), std::string(), why);
            return SkipResult::Handled;
        }

        // "ready, skip oggleflint": start the run as well; "im back, skip
        // him": carry on a paused one.
        bool started = false;
        std::vector<std::string> missing;
        if (!view.running && intent == TextRules::PartyIntent::Start)
        {
            missing = MissingPeople(tank);
            if (!missing.empty())
                ++g_stats.waiting;
            else
            {
                Dispatch(player, tank, "dc on", std::string());
                if (DcLeaderSignal::FindRunOwner(player))
                {
                    started = true;
                    ++g_stats.started;
                    g_readyAsked.erase(key);
                    ReleaseStaysFor(player);
                }
            }
        }
        else if (view.running && intent != TextRules::PartyIntent::None &&
                 intent != TextRules::PartyIntent::Pause && DcLeaderSignal::IsInPausedDungeonClearRun(player) &&
                 MissingPeople(tank).empty())
        {
            Dispatch(player, tank, "dc pause", "resume");
            if (!DcLeaderSignal::IsInPausedDungeonClearRun(player))
            {
                ++g_stats.resumed;
                g_breaks.erase(key);
                ReleaseStaysFor(player);
            }
        }

        std::vector<std::string> skipped;
        std::vector<uint32_t> entries;
        bool turnedAway = false;
        const bool running = view.running || started;
        if (running)
        {
            if (started)
                ApplyPendingSkips(player, tank);   // asked for before, in other lines
            RunView live;
            if (!ViewRun(player, tank, live) || !live.running)
                return SkipResult::Refused;
            const uint32_t heading = live.current >= 0 ? live.bosses[size_t(live.current)].entry : 0;
            for (size_t k : skip)
            {
                const RunBoss& b = view.bosses[k];
                if (!RunSkipped(live.ctx).count(b.entry))   // or it was, as the run started
                    Dispatch(player, tank, "dc skip", std::to_string(b.entry));
                if (RunSkipped(live.ctx).count(b.entry))
                {
                    skipped.push_back(b.name);
                    entries.push_back(b.entry);
                    turnedAway = turnedAway || b.entry == heading;
                }
            }
            if (skipped.empty())
            {
                ++g_stats.refused;
                return SkipResult::Refused;
            }
            // "skip to jergosh" when he was skipped too: he is where they go.
            if (dest >= 0 && RunSkipped(live.ctx).count(view.bosses[size_t(dest)].entry))
                Dispatch(player, tank, "dc unskip", std::to_string(view.bosses[size_t(dest)].entry));
        }
        else
        {
            PendingSkips& pending = g_pendingSkips[key];
            if (dest >= 0)
                pending.entries.erase(std::remove(pending.entries.begin(), pending.entries.end(),
                                                  view.bosses[size_t(dest)].entry),
                                      pending.entries.end());
            if (pending.instanceId != map->GetInstanceId())
                pending = PendingSkips{};
            pending.instanceId = map->GetInstanceId();
            pending.at         = now;
            for (size_t k : skip)
                if (!Has(pending.entries, view.bosses[k].entry))
                {
                    pending.entries.push_back(view.bosses[k].entry);
                    skipped.push_back(nameOf(k));
                    entries.push_back(view.bosses[k].entry);
                }
            g_stats.skipsBefore += skipped.size();
        }
        g_stats.skips += skipped.size();
        NoteLastSkip(player, entries);

        // Where it heads now. A run with nothing left to do ends by itself,
        // and the tank calls it done in a moment: that is its answer.
        const char* it = skipped.size() > 1 ? "them" : "it";
        std::string next, plan;
        if (running && !DcLeaderSignal::FindRunOwner(player))
        {
            uint32_t killed = 0, left = 0;
            BossCounts(view.owner, view.ctx, killed, left);
            if (killed > 0 && g_PartyDoneEnable)
            {
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] {} skipped the last boss of {}: the tank calls the "
                             "run done.", player->GetName(), map->GetMapName());
                return SkipResult::Handled;
            }
            plan = "with that, there is no boss left to do here, so the run is over.";
        }
        else
        {
            RunView after;
            ViewRun(player, tank, after);
            next = after.current >= 0 ? after.bosses[size_t(after.current)].name : std::string();
            if (!running)
                plan = missing.empty()
                           ? fmt::format("the run has not started yet, so you will leave {} out once it does.", it)
                           : fmt::format("you will start as soon as {} {} here, and leave {} out.",
                                         CrowdText::JoinNames(missing), missing.size() > 1 ? "are" : "is", it);
            else if (started)
                plan = fmt::format("you start the run now and leave {} out, so you head for {} first.", it,
                                   next.empty() ? std::string("the next boss") : next);
            else if (DcLeaderSignal::IsInPausedDungeonClearRun(player))
                plan = fmt::format("you leave {} out, and once everyone is ready you head for {}.", it,
                                   next.empty() ? std::string("the next boss") : next);
            else if (turnedAway)
                plan = fmt::format("you turn away from {} and head for {} instead.", it,
                                   next.empty() ? std::string("the next boss") : next);
            else
                plan = fmt::format("you will pass {} by when you get there, and for now you are still heading for "
                                   "{}.", it, next.empty() ? std::string("the next boss") : next);
        }
        SkipAck(player, tank, msg, seq, source, scopeKey, g_PartyCmdHintSkip, CrowdText::JoinNames(skipped), next,
                plan, std::string());
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked to skip {} in {}{}: '{}'.", player->GetName(),
                     CrowdText::JoinNames(skipped), map->GetMapName(), running ? "" : " (once the run starts)", clean);
        return SkipResult::Handled;
    }
#endif
#endif
}

bool PartyCmd_Available()
{
    return OLLAMA_CHAT_DUNGEON_CLEAR != 0;
}

bool PartyCmd_SkipsAvailable()
{
    return OLLAMA_CHAT_DC_SKIPS != 0;
}

Player* PartyCmd_RunVoiceFor(Player* player, const std::string& msg, ChatChannelSourceLocal source)
{
#if !OLLAMA_CHAT_DUNGEON_CLEAR
    (void)player; (void)msg; (void)source;
    return nullptr;
#else
    if (!g_PartyCmdEnable || g_PartyRunQuestionPhrases.empty() || !player ||
        (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL))
        return nullptr;
    if (OllamaIsBotPlayer(player) || !player->GetGroup() || !DcModule::IsEnabled())
        return nullptr;
    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())
        return nullptr;

    const std::vector<std::string> words =
        CrowdText::Words(TextRules::FixKeyTypos(Context_CleanText(msg), kTypoKeys));
    if (words.empty() || words.size() > g_PartyRunQuestionMaxWords ||
        !CrowdText::ContainsPhrase(words, g_PartyRunQuestionPhrases))
        return nullptr;

    // No bot tank -- the player tanks, or there is no run -- and anyone may answer.
    Player* tank = DcLeaderSignal::FindLeaderTank(player);
    return (tank && tank->IsInWorld() && OllamaIsBotPlayer(tank)) ? tank : nullptr;
#endif
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

    const std::string clean = TextRules::FixKeyTypos(Context_CleanText(msg), kTypoKeys);

    // Its commands work anywhere, not only inside.
    if (TextRules::IsDungeonClearCommand(clean))
        return PartyCmdLine::DcCommand;

    if (!g_PartyCmdEnable)
        return PartyCmdLine::None;

    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())   // dungeons and raids
        return PartyCmdLine::None;

    TextRules::PartyIntent intent = TextRules::MatchPartyIntent(clean, g_PartyCmdRules);
    if (intent == TextRules::PartyIntent::None && AnswersReadyAsk(player, clean) &&
        !DcLeaderSignal::FindRunOwner(player))
        intent = TextRules::PartyIntent::Start;   // "yep", to the tank's "everyone ready?"

    Player* tank = DcLeaderSignal::FindLeaderTank(player);
    if (!tank || !tank->IsInWorld())
        return PartyCmdLine::None;

#if OLLAMA_CHAT_DC_SKIPS
    // "skip the next boss", "dont skip jergosh".
    if (g_PartySkipEnable &&
        TextRules::MatchSkipAsk(clean, g_PartySkipRules, SkipNames(player, tank)).kind != TextRules::SkipKind::None)
        return PartyCmdLine::Instruction;
#endif

    if (intent == TextRules::PartyIntent::None)
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

    const std::string clean = TextRules::FixKeyTypos(Context_CleanText(msg), kTypoKeys);
    TextRules::PartyIntent intent = TextRules::MatchPartyIntent(clean, g_PartyCmdRules);
    if (intent == TextRules::PartyIntent::None && AnswersReadyAsk(player, clean) &&
        !DcLeaderSignal::FindRunOwner(player))
        intent = TextRules::PartyIntent::Start;   // "yep", to the tank's "everyone ready?"

    // No dungeon-clear tank in this group: just chat.
    Player* tank = DcLeaderSignal::FindLeaderTank(player);
    if (!tank || !tank->IsInWorld())
        return false;

#if OLLAMA_CHAT_DC_SKIPS
    // "skip the next boss", "skip oggleflint", "dont skip him" -- with
    // "ready" in it, the run starts as well.
    if (g_PartySkipEnable)
    {
        const SkipResult skip = HandleSkip(player, tank, msg, clean, seq, source, scopeKey, intent);
        if (skip == SkipResult::Handled)
            return true;
        if (skip == SkipResult::Refused)
            return false;   // dungeon clear whispered why
    }
#endif

    if (intent == TextRules::PartyIntent::None)
        return false;

    const bool running = DcLeaderSignal::FindRunOwner(player) != nullptr;
    const bool paused  = running && DcLeaderSignal::IsInPausedDungeonClearRun(player);

    // "ok im done", "full": the end of a break, and nothing else.
    if (intent == TextRules::PartyIntent::Resume && !paused)
        return false;

    if (intent == TextRules::PartyIntent::Pause)
    {
        const BreakKind kind = KindOfBreak(clean);

        // Nothing running to pause: ordinary chat (with the glossary, so a
        // "mb" still reads as a mana break).
        if (!running || paused)
        {
            // In the middle of a rest: "brb" or "wait" leaves its end to
            // somebody saying so; another "mb" starts it over (and takes back
            // a "lets go" still being written).
            auto it = paused ? g_breaks.find(GroupKey(player)) : g_breaks.end();
            if (it != g_breaks.end())
            {
                if (kind == BreakKind::Mana || kind == BreakKind::Rest)
                {
                    it->second.startedAt = std::chrono::steady_clock::now();
                    it->second.calledAt  = {};
                }
                else
                {
                    g_breaks.erase(it);
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] {}: '{}', so the break waits for someone to "
                                 "say ready.", player->GetName(), clean);
                }
            }
            return false;
        }

        Dispatch(player, tank, "dc pause", "pause");
        if (!DcLeaderSignal::IsInPausedDungeonClearRun(player))
        {
            ++g_stats.refused;
            return false;
        }

        ++g_stats.paused;

        // A rest, with a bot leading: the tank gets going again by itself once
        // everyone has rested (PartyCmd_Update), so it only says ok now.
        bool endsByItself = false;
#if OLLAMA_CHAT_DC_RUN_STATE
        endsByItself = g_PartyBreakEndEnable && (kind == BreakKind::Mana || kind == BreakKind::Rest) &&
                       OllamaIsBotPlayer(tank);
        if (endsByItself)
        {
            Break& b     = g_breaks[GroupKey(player)];
            b            = Break{};
            b.caller     = player->GetGUID().GetRawValue();
            b.instanceId = map->GetInstanceId();
            b.startedAt  = std::chrono::steady_clock::now();
            ++g_stats.breaks;
        }
#endif
        Acknowledge(player, tank, msg, seq, source, scopeKey, endsByItself ? Outcome::Break : Outcome::Paused,
                    PauseReason(kind));
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} paused the dungeon clear{}: '{}'", player->GetName(),
                     endsByItself ? " for a rest that ends by itself" : "", clean);
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
        g_breaks.erase(GroupKey(player));   // said ready before everyone had rested: over all the same
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
        g_readyAsked.erase(GroupKey(player));   // answered
        ReleaseStaysFor(player);
#if OLLAMA_CHAT_DC_SKIPS
        ApplyPendingSkips(player, tank);   // "skip the first boss", said before
#endif
        Acknowledge(player, tank, msg, seq, source, scopeKey, Outcome::Started, std::string());
    }

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} {} the dungeon clear: '{}'", player->GetName(),
                 paused ? "resumed" : "started", clean);
    return true;
#endif
}

void PartyCmd_NoteReadyAsked(Player* bot)
{
#if OLLAMA_CHAT_DUNGEON_CLEAR
    const uint32_t key = GroupKey(bot);
    if (!key || g_PartyReadyWindowSeconds == 0)
        return;
    if (g_readyAsked.size() > 500)
        g_readyAsked.clear();
    g_readyAsked[key] = ReadyAsk{ std::chrono::steady_clock::now() };
#else
    (void)bot;
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

std::string PartyCmd_TogetherFor(Player* bot, ChatChannelSourceLocal source)
{
    if (!bot || g_PartyTogetherText.empty() || (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL))
        return {};
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon() || map->IsBattlegroundOrArena() || !bot->GetGroup())
        return {};
    std::string tank;
#if OLLAMA_CHAT_DUNGEON_CLEAR
    if (DcModule::IsEnabled())
        if (Player* t = DcLeaderSignal::FindLeaderTank(bot))
            tank = (t == bot) ? std::string(", with you tanking") : ", with " + t->GetName() + " tanking";
#endif
    const std::string out = SafeFormat(g_PartyTogetherText, fmt::arg("dungeon", std::string(map->GetMapName())),
                                       fmt::arg("tank", tank));
    return out == "[Format Error]" ? std::string() : out;
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
    std::vector<std::string> skippedNames;
    BossCounts(owner, ctx, killed, left, &skippedNames);
    std::string skipped;
    if (!skippedNames.empty() && !g_PartyRunStateSkippedText.empty())
    {
        skipped = SafeFormat(g_PartyRunStateSkippedText, fmt::arg("skipped", CrowdText::JoinNames(skippedNames)));
        if (skipped == "[Format Error]")
            skipped.clear();
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
                                       fmt::arg("state", state),
                                       fmt::arg("skipped", skipped));
    if (out == "[Format Error]")
        return std::string();
    // A text written before {skipped} existed still says what is skipped.
    return g_PartyRunStateText.find("{skipped}") == std::string::npos ? out + skipped : out;
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
        WatchClock::time_point doneTried{};             // the end of the run first tried to call
    };
    std::unordered_map<uint32_t, RunWatch> g_watches;   // by the group's guid counter

    // What a line that has to say something carries besides its situation.
    struct MustSay
    {
        std::vector<std::vector<std::string>> phrases;   // one of these in the model's line...
        std::string                           fallback;  // ...or this goes out instead
        bool                                  doneCall     = false;
        bool                                  breakEndCall = false;
        bool                                  leadOffCall  = false;
    };

    // The tank says something to the whole group without being asked, in its
    // own chat style: OllamaChat.Party.AnnouncePrompt around `situation`.
    bool SubmitTankLine(Player* tank, const std::string& situation, bool readyAsk = false,
                        MustSay const* must = nullptr)
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
        request.readyAsk          = readyAsk;
        request.noEcho            = true;    // due now, and kept fresh by the line above instead
        request.maxReplyLength    = OllamaMaxReplyLengthFor(tank);
        if (must)
        {
            request.mustSay      = must->phrases;
            request.fallbackText = must->fallback;
            request.doneCall     = must->doneCall;
            request.breakEndCall = must->breakEndCall;
            request.leadOffCall  = must->leadOffCall;
        }
        return OllamaDispatch_Submit(std::move(request));
    }

    // ---- the end of a run ----------------------------------------------------------

    std::mt19937& WrapRng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint32_t Between(uint32_t lo, uint32_t hi)
    {
        return std::uniform_int_distribution<uint32_t>(lo, std::max(lo, hi))(WrapRng());
    }

    // "all 6 bosses are down", "the boss is down".
    std::string BossesText(uint32_t killed)
    {
        return killed > 1 ? fmt::format("all {} bosses are down", killed) : std::string("the boss is down");
    }

    // One group's end of a run, in one copy of a dungeon.
    struct Wrapup
    {
        uint32_t               map        = 0;
        uint32_t               instanceId = 0;
        uint64_t               tank       = 0;
        std::string            dungeon;
        WatchClock::time_point calledAt{};
        WatchClock::time_point nextLeave{};   // the next one goes then; zero while nobody is to go
        bool                   cheered    = false;
        bool                   stay       = false;   // asked to stay
        std::vector<uint64_t>  leavers;              // the random bots that came in through the finder
        std::string            skipped;              // bosses skipped on purpose, for the gg: no full clear
    };
    std::unordered_map<uint32_t, Wrapup> g_wrapups;   // by the group's guid counter

    // The tank calls it: in its own words if they say the dungeon is cleared
    // (Party.DoneWords, or the dungeon's name), else in stock ones. With bosses
    // skipped on purpose (`skipped`) it is done but no full clear, and says so
    // (Party.DoneSkipped*).
    bool SubmitDoneCall(Player* tank, Map* map, uint32_t killed, const std::vector<std::string>& skipped)
    {
        const bool partial = !skipped.empty() && !g_PartyDoneSkippedSituation.empty();
        const std::string dungeon = map ? std::string(map->GetMapName()) : std::string("the dungeon");
        const std::string bosses  = !partial ? BossesText(killed)
                                  : killed == 1 ? std::string("one boss is down")
                                                : fmt::format("{} bosses are down", killed);
        const std::string& templ = partial ? g_PartyDoneSkippedSituation : g_PartyDoneSituation;
        std::string situation = SafeFormat(templ, fmt::arg("dungeon", dungeon), fmt::arg("bosses", bosses),
                                           fmt::arg("skipped", CrowdText::JoinNames(skipped)),
                                           fmt::arg("was_were", skipped.size() > 1 ? "were" : "was"));
        if (situation.empty() || situation == "[Format Error]")
            situation = templ;

        MustSay must;
        must.doneCall = true;
        must.phrases  = partial ? g_PartyDoneSkippedWords : g_PartyDoneWords;
        if (std::vector<std::string> name = CrowdText::Words(dungeon); !name.empty())
            must.phrases.push_back(std::move(name));
        const std::vector<std::string>& lines = partial ? g_PartyDoneSkippedLines : g_PartyDoneLines;
        if (!lines.empty())
        {
            const std::string& pick = lines[Between(0, uint32_t(lines.size() - 1))];
            must.fallback = SafeFormat(pick, fmt::arg("dungeon", dungeon), fmt::arg("bosses", bosses));
            if (must.fallback == "[Format Error]")
                must.fallback.clear();
            must.fallback = Personality_StyleCanned(tank, must.fallback);
        }
        return SubmitTankLine(tank, situation, false, &must);
    }

    // The tank has called it: who is to leave (the random bots, in a group the
    // dungeon finder made), and from when.
    void StartWrapup(Player* tank, Group* group, Map* map, uint32_t key, WatchClock::time_point now,
                     const std::vector<std::string>& skipped)
    {
        Wrapup& w   = g_wrapups[key];
        w           = Wrapup{};
        w.map        = map->GetId();
        w.instanceId = map->GetInstanceId();
        w.tank       = tank->GetGUID().GetRawValue();
        w.dungeon    = map->GetMapName();
        w.calledAt   = now;
        w.skipped    = CrowdText::JoinNames(skipped);
        if (!g_PartyDoneLeave || !group->isLFGGroup())
            return;
        // Only the random bots the finder matched: not ones invited before the
        // group queued together, nor anyone's own bots (nor the bots
        // mod-dungeon-clear's queue fill logs in for a player, which it sees
        // out itself).
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && member->GetMap() == map && OllamaIsBotPlayer(member) &&
                    sRandomPlayerbotMgr.IsRandomBot(member) && JoinedThroughFinder(group, member->GetGUID()))
                    w.leavers.push_back(member->GetGUID().GetRawValue());
        std::shuffle(w.leavers.begin(), w.leavers.end(), WrapRng());
        if (!w.leavers.empty())
            w.nextLeave = now + std::chrono::seconds(g_PartyDoneLeaveSeconds);
    }

    std::string CheerPrompt(Player* bot, PlayerbotAI* ai, const Wrapup& w, const std::string& tankName)
    {
        if (g_PartyCheerPrompt.empty() || !ai->GetChatHelper())
            return "";
        std::string prompt = SafeFormat(g_PartyCheerPrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("dungeon", w.dungeon),
                                        fmt::arg("tank", tankName));
        if (prompt.empty() || prompt == "[Format Error]")
            return "";
        if (!w.skipped.empty() && !g_PartyDoneSkippedText.empty())
        {
            const std::string note = SafeFormat(g_PartyDoneSkippedText, fmt::arg("skipped", w.skipped));
            if (note != "[Format Error]")
                prompt += note;
        }
        return prompt + Roleplay_BuildVoicePrompt(bot);
    }

    // Everyone but the tank says gg, once each, a moment apart, to nobody in
    // particular: no bot answers another's.
    void Cheer(Player* tank, Wrapup& w)
    {
        w.cheered = true;
        if (!tank || !g_PartyCheerEnable || g_PartyCheerChance == 0)
            return;
        Group* group = tank->GetGroup();
        if (!group)
            return;
        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           group->GetGUID().GetCounter());

        std::vector<Player*> bots;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != tank && member->IsInWorld() && member->GetMap() == tank->GetMap() &&
                    OllamaIsBotPlayer(member))
                    bots.push_back(member);
        std::shuffle(bots.begin(), bots.end(), WrapRng());

        // Typed ones never twice in a group; none in roleplay, where a bare
        // "gg" would break character.
        std::vector<std::string> typed = g_RoleplayEnable ? std::vector<std::string>() : g_PartyCheerLines;
        std::shuffle(typed.begin(), typed.end(), WrapRng());
        size_t   nextTyped = 0;
        uint32_t delay     = 0;
        for (Player* bot : bots)
        {
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (!ai || Between(1, 100) > g_PartyCheerChance)
                continue;
            delay += Between(1200, 4500);

            OllamaChatRequest request;
            request.botGuid           = bot->GetGUID().GetRawValue();
            request.source            = source;
            request.chainDepth        = 0;
            request.scopeKey          = scopeKey;
            request.botName           = bot->GetName();
            request.kind              = OllamaRequestKind::EventChatter;
            request.directAddress     = true;    // a moment everyone speaks at: no pacing cooldown
            request.triggerBotReplies = false;   // nobody answers a gg
            request.noEcho            = true;    // "gg" from everyone is the point
            request.extraDelayMs      = delay;
            request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
            if (nextTyped < typed.size() && Between(1, 100) <= g_PartyCheerCannedChance)
                request.fixedText = Personality_StyleCanned(bot, typed[nextTyped++]);
            else
            {
                request.prompt = CheerPrompt(bot, ai, w, tank->GetName());
                if (request.prompt.empty())
                {
                    if (nextTyped >= typed.size())
                        continue;
                    request.fixedText = Personality_StyleCanned(bot, typed[nextTyped++]);
                }
            }
            if (OllamaDispatch_Submit(std::move(request)))
                ++g_stats.cheers;
        }
    }

    // Every couple of seconds: the gg, if the tank's call never got out, and
    // the next one to leave.
    void UpdateWrapups(WatchClock::time_point now)
    {
        for (auto it = g_wrapups.begin(); it != g_wrapups.end();)
        {
            Wrapup& w = it->second;
            if (!w.cheered && now - w.calledAt > std::chrono::seconds(25))
                Cheer(ObjectAccessor::FindConnectedPlayer(ObjectGuid(w.tank)), w);

            bool over = now - w.calledAt > std::chrono::minutes(30);
            if (!over && !w.stay && w.nextLeave != WatchClock::time_point{} && now >= w.nextLeave)
            {
                w.nextLeave = WatchClock::time_point{};
                while (!w.leavers.empty())
                {
                    const uint64_t raw = w.leavers.back();
                    w.leavers.pop_back();
                    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(raw));
                    Group* group = bot ? bot->GetGroup() : nullptr;
                    PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
                    if (!group || group->GetGUID().GetCounter() != it->first || !ai || !bot->IsInWorld())
                        continue;   // gone already
                    if (bot->IsInCombat())
                    {
                        w.leavers.push_back(raw);   // not in the middle of a fight
                        w.nextLeave = now + std::chrono::seconds(5);
                        break;
                    }
                    ai->LeaveOrDisbandGroup();
                    ++g_stats.left;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] {} leaves the group: {} is cleared.",
                                 bot->GetName(), w.dungeon);
                    w.nextLeave = now + std::chrono::seconds(Between(3, 9));
                    break;
                }
                if (w.leavers.empty() && w.nextLeave == WatchClock::time_point{})
                    over = true;   // everyone who was going has gone
            }
            it = over ? g_wrapups.erase(it) : std::next(it);
        }
    }

    // ---- a rest that ends by itself ------------------------------------------------

    // Everyone has rested: in here and alive, out of combat, health and mana
    // back (Party.BreakEnd*), and every real player close by, at the keyboard
    // and done drinking -- one sat down still is done only when full. A real
    // player's and a healer's mana has to be all but full; any other bot's
    // less, as it tops up on the way like on the rest of a run, and a hunter's
    // or a shaman's (BreakEndLowManaClasses) least: it does its damage anyway.
    bool GroupRested(Player* tank, Group* group)
    {
        Map* map = tank->GetMap();
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;
            const bool real = !OllamaIsBotPlayer(member);
            if (!member->IsInWorld() || member->GetMap() != map)
            {
                if (real)
                    return false;   // not in here (yet)
                continue;           // a bot somewhere else is no part of it
            }
            if (!member->IsAlive() || member->IsInCombat())
                return false;
            if (real && (member->isAFK() ||
                         (g_PartyCmdMaxDistance > 0.0f && !member->IsWithinDistInMap(tank, g_PartyCmdMaxDistance))))
                return false;

            const float health  = member->GetHealthPct();
            const bool  hasMana = member->GetMaxPower(POWER_MANA) > 0;
            const float mana    = hasMana ? member->GetPowerPct(POWER_MANA) : 100.0f;
            if (health < float(g_PartyBreakEndHealthPct))
                return false;
            const bool lowManaClass =
                (member->getClassMask() & g_PartyBreakEndLowManaClasses) && !PlayerbotAI::IsTank(member);
            const uint32_t bar = (real || PlayerbotAI::IsHeal(member)) ? g_PartyBreakEndManaPct
                                 : lowManaClass                        ? g_PartyBreakEndLowManaPct
                                                                       : g_PartyBreakEndOthersManaPct;
            if (hasMana && mana < float(bar))
                return false;
            if (real && member->IsSitState() && (health < 100.0f || mana < 100.0f))
                return false;   // still drinking: they get to finish it
        }
        return true;
    }

    // The break is over: the run goes on, as if whoever called it had said
    // they were ready -- or another real player in the group, if they are gone.
    void EndBreak(Player* tank, uint32_t key)
    {
        auto it = g_breaks.find(key);
        if (it == g_breaks.end())
            return;
        const uint64_t caller = it->second.caller;
        g_breaks.erase(it);

        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group)
            return;
        Player* issuer = ObjectAccessor::FindConnectedPlayer(ObjectGuid(caller));
        if (!issuer || !issuer->IsInWorld() || issuer->GetGroup() != group)
            issuer = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref && !issuer; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && !OllamaIsBotPlayer(member))
                    issuer = member;
        if (!issuer || !DcLeaderSignal::IsInPausedDungeonClearRun(issuer))
            return;   // nobody to give the word, or going again already
        Player* leader = DcLeaderSignal::FindLeaderTank(issuer);
        if (!leader || !leader->IsInWorld())
            return;

        Dispatch(issuer, leader, "dc pause", "resume");
        if (DcLeaderSignal::IsInPausedDungeonClearRun(issuer))
        {
            ++g_stats.refused;   // dungeon clear whispered why
            return;
        }
        ++g_stats.breaksEnded;
        ReleaseStaysFor(issuer);
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Everyone has rested: {} carries on with the dungeon clear.",
                     leader->GetName());
    }

    // "right, lets go": the tank calls the break over, in its own chat style
    // and in words that plainly say they are moving, or else in stock ones
    // (Party.BreakEndLines). The run goes on once the line is out
    // (PartyCmd_NoteBreakEndCalled).
    bool SubmitBreakEndLine(Player* tank)
    {
        if (g_PartyBreakEndSituation.empty())
            return false;
        MustSay must;
        must.phrases      = g_PartyBreakEndWords;
        must.breakEndCall = true;
        if (!g_PartyBreakEndLines.empty())
            must.fallback = Personality_StyleCanned(
                tank, g_PartyBreakEndLines[Between(0, uint32_t(g_PartyBreakEndLines.size() - 1))]);
        return SubmitTankLine(tank, TextRules::ExpandChoices(g_PartyBreakEndSituation, WrapRng()), false, &must);
    }

    // One group's rest, every couple of seconds while it lasts.
    void WatchBreak(Player* tank, Group* group, Map* map, uint32_t key, bool pausedRun, WatchClock::time_point now)
    {
        auto it = g_breaks.find(key);
        if (it == g_breaks.end())
            return;
        Break& b = it->second;
        if (!g_PartyBreakEndEnable || !pausedRun || b.instanceId != map->GetInstanceId())
        {
            g_breaks.erase(it);   // going again, however that came about
            return;
        }

        if (b.calledAt != WatchClock::time_point{})
        {
            // The line is on its way, and the run goes when it is out. One
            // that never makes it out does not leave them standing there.
            if (now - b.calledAt > std::chrono::seconds(20))
                EndBreak(tank, key);
            return;
        }
        if (now - b.startedAt > std::chrono::minutes(g_PartyBreakEndMaxMinutes))
        {
            g_breaks.erase(it);   // a long one: over when somebody says so
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] A break in {} has gone on {} minutes: it waits for "
                         "someone to say ready.", map->GetMapName(), g_PartyBreakEndMaxMinutes);
            return;
        }
        if (now - b.startedAt < std::chrono::seconds(g_PartyBreakEndMinSeconds) || !tank->IsAlive() ||
            !GroupRested(tank, group))
            return;

        b.calledAt = now;
        if (!SubmitBreakEndLine(tank))
            EndBreak(tank, key);   // nothing to say it with: it just goes
    }

    // ---- nobody answers "everyone ready?" -------------------------------------------

    // The whole group is here and ready to go: every member online and in this
    // copy of the dungeon, and the rest as at the end of a rest (GroupRested:
    // alive, out of combat, real players near the tank and not AFK, health and
    // mana back).
    bool EveryoneHere(Player* tank, Group* group)
    {
        Map* map = tank->GetMap();
        uint32_t here = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && member->GetMap() == map)
                    ++here;
        return here == group->GetMembersCount() && GroupRested(tank, group);
    }

    // The tank leads off: the run starts as if a real player in the group had
    // said ready.
    void LeadOff(Player* tank, uint32_t key)
    {
        g_readyAsked.erase(key);
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group)
            return;
        Player* issuer = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref && !issuer; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && !OllamaIsBotPlayer(member))
                    issuer = member;
        if (!issuer || DcLeaderSignal::FindRunOwner(issuer))
            return;   // nobody to give the word, or going already
        Player* leader = DcLeaderSignal::FindLeaderTank(issuer);
        if (!leader || !leader->IsInWorld())
            return;

        Dispatch(issuer, leader, "dc on", std::string());
        if (!DcLeaderSignal::FindRunOwner(issuer))
        {
            ++g_stats.refused;   // dungeon clear whispered why
            return;
        }
        ++g_stats.started;
        ++g_stats.ledOff;
        ReleaseStaysFor(issuer);
#if OLLAMA_CHAT_DC_SKIPS
        ApplyPendingSkips(issuer, leader);   // "skip the first boss", said before
#endif
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Nobody answered the ready check in {}: {} leads off.",
                     leader->GetMap() ? leader->GetMap()->GetMapName() : "a dungeon", leader->GetName());
    }

    // "ok, taking that as a yes, pulling": in the tank's own chat
    // style and in words that say it is starting, or else stock ones
    // (Party.ReadyTimeoutLines). The run starts once the line is out
    // (PartyCmd_NoteLeadOffCalled).
    bool SubmitLeadOffLine(Player* tank)
    {
        if (g_PartyReadyTimeoutSituation.empty())
            return false;
        MustSay must;
        must.phrases     = g_PartyReadyTimeoutWords;
        must.leadOffCall = true;
        if (!g_PartyReadyTimeoutLines.empty())
            must.fallback = Personality_StyleCanned(
                tank, g_PartyReadyTimeoutLines[Between(0, uint32_t(g_PartyReadyTimeoutLines.size() - 1))]);
        return SubmitTankLine(tank, TextRules::ExpandChoices(g_PartyReadyTimeoutSituation, WrapRng()), false, &must);
    }

    // The tank's are-you-ready, every couple of seconds until the run starts:
    // nobody has said anything for Party.ReadyTimeoutSeconds and the whole
    // group is here, so it takes that as a yes.
    void WatchReadyAsk(Player* tank, Group* group, uint32_t key, WatchClock::time_point now)
    {
        if (g_PartyReadyTimeoutSeconds == 0)
            return;
        auto it = g_readyAsked.find(key);
        if (it == g_readyAsked.end())
            return;
        ReadyAsk& ask = it->second;
        if (ask.spoken)
            return;   // someone said something: when to start is theirs to say
        if (ask.leadCalled != WatchClock::time_point{})
        {
            // The line is on its way, and the run starts when it is out. One
            // that never makes it out does not leave them standing there.
            if (now - ask.leadCalled > std::chrono::seconds(20))
                LeadOff(tank, key);
            return;
        }
        if (now - ask.at < std::chrono::seconds(g_PartyReadyTimeoutSeconds) || !tank->IsAlive() ||
            !EveryoneHere(tank, group))
            return;

        ask.leadCalled = now;
        if (!SubmitLeadOffLine(tank))
            LeadOff(tank, key);   // nothing to say it with: it just goes
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
    if (!g_PartyGreetEnable && !g_PartyDoneEnable && !unstayWanted && g_breaks.empty())
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
        std::vector<std::string> skippedNames;
        BossCounts(counted, countedAi ? countedAi->GetAiObjectContext() : nullptr, killed, left, &skippedNames);

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
        const bool pausedRun = owner && DcLeaderSignal::IsInPausedDungeonClearRun(player);
        const bool moving    = owner && !pausedRun;
        if (g_unstayLater.erase(key))
            w.unstay = unstayWanted;
        if (moving && !w.moving)
            w.unstay = unstayWanted;
        w.moving = moving;
        if (!moving)
            w.unstay = false;
        else if (w.unstay && ReleaseStaysWhenFree(player))
            w.unstay = false;

        // A rest someone called ("mb"): over once everyone has had it.
        WatchBreak(tank, group, map, key, pausedRun, now);

#if OLLAMA_CHAT_DC_SKIPS
        // Skips asked for before the run, which "dc on" or the addon started.
        if (owner && g_pendingSkips.count(key))
            ApplyPendingSkips(player, tank);
#endif

        // "let me know when you're ready": once, when they first come in.
        if (!w.greeted)
        {
            if (owner || killed > 0)
                w.greeted = true;   // under way already
            else if (g_PartyGreetEnable && now - w.firstSeen >= std::chrono::seconds(g_PartyGreetDelaySeconds) &&
                     tank->IsAlive() && !tank->IsInCombat() && !player->IsInCombat())
            {
                // A tank that spoke a moment ago gets a little longer, then
                // the moment has passed. It asks whether they are ready: once
                // that is out, a plain "yes" answers it (PartyCmd_NoteReadyAsked).
                static std::mt19937 rng{ std::random_device{}() };
                if (SubmitTankLine(tank, TextRules::ExpandChoices(g_PartyGreetSituation, rng), true))
                    w.greeted = true;
                else if (now - w.firstSeen >= std::chrono::seconds(g_PartyGreetDelaySeconds + 30))
                    w.greeted = true;
            }
        }
        // Asked, and nobody has said a word since while everyone is here: the
        // tank takes it they are ready, says so, and leads off.
        else if (!owner && killed == 0)
            WatchReadyAsk(tank, group, key, now);

        // Every boss down: the tank says they are done -- plainly -- and the
        // rest of the end of a run follows from that (UpdateWrapups). A call
        // that cannot go out just now is tried again for a minute.
        if (!w.announced && g_PartyDoneEnable && killed > 0 && left == 0 && (w.fresh || w.sawRun) &&
            tank->IsAlive() && !tank->IsInCombat() && !player->IsInCombat())
        {
            if (w.doneTried == WatchClock::time_point{})
                w.doneTried = now;
            if (SubmitDoneCall(tank, map, killed, skippedNames))
            {
                w.announced = true;
                ++g_stats.done;
                StartWrapup(tank, group, map, key, now, skippedNames);
            }
            else if (now - w.doneTried > std::chrono::seconds(60))
                w.announced = true;
        }
    }

    UpdateWrapups(now);

    // Now and then: the finder groups that are gone (bots' own come and go).
    static WatchClock::time_point nextSweep{};
    if (now >= nextSweep)
    {
        nextSweep = now + std::chrono::minutes(5);
        std::lock_guard<std::mutex> lock(g_finderMutex);
        for (auto it = g_finderJoins.begin(); it != g_finderJoins.end();)
            it = sGroupMgr->GetGroupByGUID(ObjectGuid(it->first).GetCounter()) ? std::next(it)
                                                                                 : g_finderJoins.erase(it);
        for (auto it = g_invites.begin(); it != g_invites.end();)
            it = (now - it->second.at > std::chrono::hours(6)) ? g_invites.erase(it) : std::next(it);
    }

    for (auto it = g_watches.begin(); it != g_watches.end();)
        it = (now - it->second.lastSeen > std::chrono::minutes(30)) ? g_watches.erase(it) : std::next(it);
    // Rests of groups no longer looked at (nobody real inside, a player
    // tanking now): long over either way.
    for (auto it = g_breaks.begin(); it != g_breaks.end();)
        it = (now - it->second.startedAt > std::chrono::minutes(g_PartyBreakEndMaxMinutes + 1)) ? g_breaks.erase(it)
                                                                                                 : std::next(it);
    // Ready checks long asked -- but not one whose "ok, pulling" is on
    // its way: the run starts when that is out.
    for (auto it = g_readyAsked.begin(); it != g_readyAsked.end();)
    {
        const ReadyAsk& ask = it->second;
        const bool leading =
            ask.leadCalled != WatchClock::time_point{} && now - ask.leadCalled < std::chrono::seconds(30);
        it = (now - ask.at > std::chrono::seconds(g_PartyReadyWindowSeconds) && !leading) ? g_readyAsked.erase(it)
                                                                                          : std::next(it);
    }
#if OLLAMA_CHAT_DC_SKIPS
    // Skips for a run that never started, and the last skips, long gone.
    for (auto* skips : { &g_pendingSkips, &g_lastSkips })
        for (auto it = skips->begin(); it != skips->end();)
            it = (now - it->second.at > std::chrono::minutes(30)) ? skips->erase(it) : std::next(it);
#endif
#endif
}

void PartyCmd_NoteDoneCalled(Player* bot)
{
#if OLLAMA_CHAT_DC_RUN_STATE
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return;
    auto it = g_wrapups.find(group->GetGUID().GetCounter());
    if (it != g_wrapups.end() && !it->second.cheered)
        Cheer(bot, it->second);
#else
    (void)bot;
#endif
}

bool PartyCmd_BreakEndStillDue(Player* bot)
{
#if OLLAMA_CHAT_DC_RUN_STATE
    auto it = g_breaks.find(GroupKey(bot));
    return it != g_breaks.end() && it->second.calledAt != std::chrono::steady_clock::time_point{} &&
           DcLeaderSignal::IsInPausedDungeonClearRun(bot);
#else
    (void)bot;
    return false;
#endif
}

bool PartyCmd_LeadOffStillDue(Player* bot)
{
#if OLLAMA_CHAT_DC_RUN_STATE
    auto it = g_readyAsked.find(GroupKey(bot));
    return it != g_readyAsked.end() && !it->second.spoken &&
           it->second.leadCalled != std::chrono::steady_clock::time_point{} && !DcLeaderSignal::FindRunOwner(bot);
#else
    (void)bot;
    return false;
#endif
}

void PartyCmd_NoteLeadOffCalled(Player* bot)
{
#if OLLAMA_CHAT_DC_RUN_STATE
    const uint32_t key = GroupKey(bot);
    auto it = g_readyAsked.find(key);
    if (it != g_readyAsked.end() && !it->second.spoken)
        LeadOff(bot, key);
#else
    (void)bot;
#endif
}

void PartyCmd_NoteBreakEndCalled(Player* bot)
{
#if OLLAMA_CHAT_DC_RUN_STATE
    const uint32_t key = GroupKey(bot);
    if (key && g_breaks.count(key))
        EndBreak(bot, key);
#else
    (void)bot;
#endif
}

void PartyCmd_NoteHumanPartyLine(Player* player, const std::string& msg, ChatChannelSourceLocal source)
{
#if OLLAMA_CHAT_DC_RUN_STATE
    if (!player || (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL))
        return;
    // Whatever they said after the tank's "everyone ready?", when to start is
    // theirs to say now: the tank does not take the quiet for a yes.
    if (auto ask = g_readyAsked.find(GroupKey(player)); ask != g_readyAsked.end())
        ask->second.spoken = true;
    if (g_wrapups.empty())
        return;
    Group* group = player->GetGroup();
    auto it = group ? g_wrapups.find(group->GetGUID().GetCounter()) : g_wrapups.end();
    if (it == g_wrapups.end())
        return;
    Wrapup& w = it->second;
    if (w.leavers.empty())
        return;   // nobody was going anyway
    // An ask ("ty for that - can you help me with one more?") keeps them
    // even with a goodbye in the same line; the stay list holds only asks,
    // so "gg, thanks for the help" or "bye, see you next time" keeps nobody.
    const std::vector<std::string> words = CrowdText::Words(msg);
    const bool stays = CrowdText::ContainsPhrase(words, g_PartyDoneStayWords);
    const bool goes  = !stays && CrowdText::ContainsPhrase(words, g_PartyDoneGoWords);
    if (!w.stay && stays)
    {
        w.stay = true;
        ++g_stats.stayed;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked the group to stay after {}: '{}'.",
                     player->GetName(), w.dungeon, msg);
    }
    else if (w.stay && goes)
    {
        w.stay      = false;
        w.nextLeave = WatchClock::now() + std::chrono::seconds(Between(4, 10));
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} let the group go after {}: '{}'.", player->GetName(),
                     w.dungeon, msg);
    }
#else
    (void)player; (void)msg; (void)source;
#endif
}

std::string PartyCmd_DoneStateFor(Player* bot, ChatChannelSourceLocal source)
{
#if OLLAMA_CHAT_DC_RUN_STATE
    if (g_wrapups.empty() || !bot || g_PartyDoneStateText.empty() ||
        (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL))
        return "";
    Group* group = bot->GetGroup();
    auto it = group ? g_wrapups.find(group->GetGUID().GetCounter()) : g_wrapups.end();
    if (it == g_wrapups.end())
        return "";
    const Wrapup& w = it->second;
    Map* map = bot->GetMap();
    if (!map || map->GetId() != w.map || map->GetInstanceId() != w.instanceId)
        return "";   // a run since, or somewhere else
    const std::string plan = w.stay ? g_PartyDoneStayingText
                                    : (!w.leavers.empty() ? g_PartyDoneLeavingText : std::string());
    const std::string out = SafeFormat(g_PartyDoneStateText, fmt::arg("dungeon", w.dungeon), fmt::arg("plan", plan));
    return out == "[Format Error]" ? std::string() : out;
#else
    (void)bot; (void)source;
    return "";
#endif
}

PartyCmdStats PartyCmd_GetStats()
{
    return g_stats;
}

void OllamaChatFinderJoinScript::OnInviteMember(Group* group, ObjectGuid guid)
{
    // Any thread: note it, nothing more.
    if (!group || !guid.IsPlayer() || group->isLFGGroup() || group->isBGGroup() || group->isBFGroup())
        return;
    std::lock_guard<std::mutex> lock(g_finderMutex);
    if (g_invites.size() > 20000)
        g_invites.clear();   // only if the sweep has stopped running
    g_invites[guid.GetRawValue()] = Invite{ group->GetLeaderGUID().GetRawValue(), std::chrono::steady_clock::now() };
}

void OllamaChatFinderJoinScript::OnAddMember(Group* group, ObjectGuid guid)
{
    // Any thread: note it, nothing more.
    if (!group || !guid.IsPlayer() || !group->isLFGGroup())
        return;
    std::lock_guard<std::mutex> lock(g_finderMutex);
    if (g_finderJoins.size() > 20000)
        g_finderJoins.clear();   // only if the sweep below has stopped running
    g_finderJoins[group->GetGUID().GetRawValue()].insert(guid.GetRawValue());
}
