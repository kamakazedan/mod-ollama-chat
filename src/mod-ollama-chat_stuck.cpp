#include "mod-ollama-chat_stuck.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <list>
#include <unordered_map>

// Finding a bot stuck, and getting it unstuck, is mod-playerbots-plus's
// (Stuck.* there). This is what the bot makes of it.

namespace
{
    using Clock = std::chrono::steady_clock;

    // What it was stuck on, for its replies a while after.
    struct Memory
    {
        std::string       what;    // "a Copper Vein"
        std::string       verb;    // "mine"
        std::string       zone;
        Clock::time_point when{};
    };
    std::unordered_map<uint64_t, Memory> g_memory;

    StuckStats g_stats;

    Clock::duration Remember()
    {
        return std::chrono::minutes(std::max<uint32_t>(1, g_StuckRememberMinutes));
    }

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 2
    Player* RealPlayerNear(Player* bot, float range)
    {
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, range, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, range);
        for (Player* p : found)
            if (p && p != bot && p->IsInWorld() && OllamaIsRealPlayer(p))
                return p;
        return nullptr;
    }

    // Grouped with a real player, or with one in earshot: it may say so.
    void Remark(Player* bot, Memory const& m)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (g_StuckRemarkChance == 0 || g_StuckRemarkPrompt.empty() || !ai || !ai->GetChatHelper())
            return;

        ChatChannelSourceLocal source;
        uint32_t chance = g_StuckRemarkChance;
        Group* group = bot->GetGroup();
        if (group && OllamaGroupHasRealPlayer(bot) && !g_DisableForParty)
            source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        else if (!g_DisableForSayYell && g_SayDistance > 0.0f && RealPlayerNear(bot, g_SayDistance))
        {
            source = SRC_SAY_LOCAL;
            chance /= 2;   // talking to nobody in particular
        }
        else
            return;
        if (urand(1, 100) > chance)
            return;

        const std::string scopeKey =
            (source == SRC_SAY_LOCAL)
                ? Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, "", 0, bot->GetZoneId())
                : Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                        group->GetGUID().GetCounter());
        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
            return;

        const std::string personality = GetBotPersonality(bot);
        std::string prompt = SafeFormat(g_StuckRemarkPrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(personality)),
                                        fmt::arg("what", m.what),
                                        fmt::arg("verb", m.verb),
                                        fmt::arg("zone", m.zone),
                                        fmt::arg("channel", source == SRC_SAY_LOCAL ? "/say, to nobody in particular"
                                                                                    : "party chat"));
        if (prompt.empty() || prompt == "[Format Error]")
            return;
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
        if (OllamaDispatch_Submit(std::move(request)))
            ++g_stats.remarks;
    }

    // mod-playerbots-plus found a bot stuck on something, and took it off it.
    void Heard(PlayerbotsPlus::BotEvent const& event)
    {
        if (event.type != PlayerbotsPlus::BotEventType::StuckMount)
            return;
        ++g_stats.heard;

        const Clock::time_point now = Clock::now();
        for (auto it = g_memory.begin(); it != g_memory.end();)
            it = (now - it->second.when > Remember()) ? g_memory.erase(it) : std::next(it);
        if (!g_Enable)
            return;

        Player* bot = ObjectAccessor::FindConnectedPlayer(event.bot);
        if (!bot || !bot->IsInWorld())
            return;
        Memory& m = g_memory[event.bot.GetRawValue()];
        m.what = event.what;
        m.verb = event.verb;
        m.zone = event.zone;
        m.when = now;
        Remark(bot, m);
    }
#endif
}

void Stuck_Init()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 2
    PlayerbotsPlus::AddBotEventListener(Heard);
#endif
}

std::string Stuck_StateTextFor(Player* bot)
{
    if (!bot || g_memory.empty() || g_StuckStateText.empty())
        return {};
    auto it = g_memory.find(bot->GetGUID().GetRawValue());
    if (it == g_memory.end() || Clock::now() - it->second.when > Remember())
        return {};
    const Memory& m = it->second;
    const std::string out = SafeFormat(g_StuckStateText,
                                       fmt::arg("what", m.what),
                                       fmt::arg("verb", m.verb),
                                       fmt::arg("zone", m.zone.empty() ? std::string("here") : m.zone));
    return out == "[Format Error]" ? std::string() : out;
}

StuckStats Stuck_GetStats()
{
    return g_stats;
}
