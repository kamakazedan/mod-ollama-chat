#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
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

namespace
{
    PartyCmdStats g_stats;

#if OLLAMA_CHAT_DUNGEON_CLEAR
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

        std::string hint = SafeFormat(*templ,
                                      fmt::arg("player_name", player->GetName()),
                                      fmt::arg("player_message", msg),
                                      fmt::arg("reason", detail),
                                      fmt::arg("missing", detail),
                                      fmt::arg("pass_token", g_ContextPassToken));
        if (hint == "[Format Error]")
            return;

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

PartyCmdStats PartyCmd_GetStats()
{
    return g_stats;
}
