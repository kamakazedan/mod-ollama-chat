#include "mod-ollama-chat_groupquests.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CharacterCache.h"
#include "Channel.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <utility>

namespace
{
    GroupQuestsStats g_stats;

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 4
    using PlayerbotsPlus::BotEventType;

    std::string const* SituationFor(BotEventType type)
    {
        switch (type)
        {
            case BotEventType::GroupQuestAsk:    return &g_GroupQuestsAskText;
            case BotEventType::GroupQuestAnswer: return &g_GroupQuestsAnswerText;
            case BotEventType::GroupQuestInvite: return &g_GroupQuestsInviteText;
            case BotEventType::GroupQuestSolo:   return &g_GroupQuestsSoloText;
            default:                             return nullptr;
        }
    }

    std::string People(uint32_t n)
    {
        return n == 1 ? std::string("one more person") : std::to_string(n) + " more people";
    }

    // Asks sent to be written, by who asked and for which quest. An answer or
    // an invite goes out only after an ask that did (one held back, or with
    // nobody real there to read it, has none), and going alone only after
    // asks someone could have read.
    using Clock = std::chrono::steady_clock;
    std::map<std::pair<uint64_t, uint32_t>, Clock::time_point> g_asked;
    constexpr auto kAnswerWithin = std::chrono::minutes(2);
    constexpr auto kAskedFor     = std::chrono::hours(2);

    bool Asked(ObjectGuid asker, uint32_t quest, Clock::duration within, Clock::time_point now)
    {
        auto it = g_asked.find({ asker.GetRawValue(), quest });
        return it != g_asked.end() && now - it->second < within;
    }

    // mod-playerbots-plus had a bot ask for help with a group quest, answer
    // one, tell one who asked to come along, or go at it alone.
    void Heard(PlayerbotsPlus::BotEvent const& event)
    {
        std::string const* situation = SituationFor(event.type);
        if (!situation)
            return;
        ++g_stats.heard;
        if (!g_Enable || !g_GroupQuestsEnable || situation->empty() || g_GroupQuestsPrompt.empty())
            return;

        Player* bot = ObjectAccessor::FindPlayer(event.bot);
        PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        Quest const* quest = sObjectMgr->GetQuestTemplate(event.quest);
        if (!bot || !bot->IsInWorld() || !ai || !ai->GetChatHelper() || !quest)
            return;

        // Only where a real player would read it.
        Channel* channel = OllamaResolveZoneChannel(bot, uint32_t(ChatChannelId::GENERAL));
        OllamaWorldSnapshot world;
        if (channel && OllamaIsOnChannel(channel, bot->GetGUID()))
            world.Build();
        if (!channel || world.Empty() || !world.RealPlayerInChannel(channel))
        {
            ++g_stats.quiet;
            return;
        }
        bool const unprompted = event.type == BotEventType::GroupQuestAsk || event.type == BotEventType::GroupQuestSolo;
        if (unprompted && g_ContextAmbientHoldSeconds &&
            Context_HumanSpokeSince(bot, SRC_GENERAL_LOCAL, channel->GetName(), 0, g_ContextAmbientHoldSeconds))
        {
            ++g_stats.quiet;
            return;
        }
        Clock::time_point const now = Clock::now();
        for (auto it = g_asked.begin(); it != g_asked.end();)
            it = now - it->second >= kAskedFor ? g_asked.erase(it) : std::next(it);
        bool const answer =
            event.type == BotEventType::GroupQuestAnswer || event.type == BotEventType::GroupQuestInvite;
        if ((answer && !Asked(event.other, event.quest, kAnswerWithin, now)) ||
            (event.type == BotEventType::GroupQuestSolo && !Asked(event.bot, event.quest, kAskedFor, now)))
        {
            ++g_stats.quiet;
            return;
        }

        std::string other;
        if (event.other)
        {
            if (Player* p = ObjectAccessor::FindConnectedPlayer(event.other))
                other = p->GetName();
            else
                sCharacterCache->GetCharacterNameByGuid(event.other, other);
        }
        std::string const text = SafeFormat(*situation,
                                            fmt::arg("quest", "[" + quest->GetTitle() + "]"),
                                            fmt::arg("need", event.need),
                                            fmt::arg("need_people", People(std::max<uint32_t>(1, event.need))),
                                            fmt::arg("have", event.have),
                                            fmt::arg("other_name", other),
                                            fmt::arg("zone", event.zone));
        if (text.empty() || text == "[Format Error]")
            return;

        std::string prompt = SafeFormat(g_GroupQuestsPrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality",
                                                 GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("situation", text),
                                        fmt::arg("zone", event.zone));
        if (prompt.empty() || prompt == "[Format Error]")
            return;
        prompt += Roleplay_BuildVoicePrompt(bot);
        prompt += Links_PromptHint();

        std::string const scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_GENERAL_LOCAL],
                                                           channel->GetChannelId(), channel->GetName(), 0,
                                                           bot->GetZoneId());
        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = event.other.GetRawValue();
        request.source            = SRC_GENERAL_LOCAL;
        request.channelName       = channel->GetName();
        request.channelId         = channel->GetChannelId();
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = false;   // who answers is mod-playerbots-plus's
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        // Nobody asked for any of these: a real player who speaks before one
        // is out was not heard by it (and an answer goes with the ask it
        // answers).
        request.writtenAtSeq      = Context_NextSeq();
        if (answer)
        {
            // After the ask it answers, as someone reads it and types back.
            uint32_t const low = std::min(g_GroupQuestsAnswerDelayMinSeconds, g_GroupQuestsAnswerDelayMaxSeconds);
            request.extraDelayMs = urand(low, g_GroupQuestsAnswerDelayMaxSeconds) * 1000;
        }
        if (!OllamaDispatch_Submit(std::move(request)))
        {
            ++g_stats.quiet;
            return;
        }
        ++g_stats.said;
        if (event.type == BotEventType::GroupQuestAsk)
            g_asked[{ event.bot.GetRawValue(), event.quest }] = now;
    }
#endif
}

void GroupQuests_Init()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 4
    PlayerbotsPlus::AddBotEventListener(Heard);
#endif
}

GroupQuestsStats GroupQuests_GetStats()
{
    return g_stats;
}
