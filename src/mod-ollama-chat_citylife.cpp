#include "mod-ollama-chat_citylife.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"

#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <string>
#include <utility>

namespace
{
    CityLifeStats g_stats;

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 5
    using PlayerbotsPlus::BotEventType;

    // Who came over to look, `n` of them counting the one saying it.
    std::string Who(uint32_t n)
    {
        if (n <= 1)
            return "you";
        if (n == 2)
            return "you and one other person";
        return "you and " + std::to_string(n - 1) + " others";
    }

    // A few hanging out came over to look at someone's rare mount: the first
    // of them says something to the rider about it.
    void MountShown(PlayerbotsPlus::BotEvent const& event)
    {
        ++g_stats.mounts;
        if (!g_Enable || !g_CityLifeEnable || g_DisableForSayYell || g_CityLifeMountPrompt.empty() ||
            event.what.empty())
        {
            ++g_stats.quiet;
            return;
        }

        Player* bot   = ObjectAccessor::FindPlayer(event.bot);
        Player* rider = ObjectAccessor::FindPlayer(event.other);
        PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !rider || bot == rider || !bot->IsInWorld() || !rider->IsInWorld() ||
            bot->GetMap() != rider->GetMap() || !ai || !ai->GetChatHelper())
        {
            ++g_stats.quiet;
            return;
        }

        // The other side's /say arrives as gibberish (a neutral city).
        // A line nobody asked for: not over someone real who has just spoken
        // nearby. And the usual pacing of /say there.
        std::string const scopeKey =
            Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, "", 0, bot->GetZoneId());
        if (Roleplay_IsLanguageBarrier(bot, rider) ||
            (g_ContextAmbientHoldSeconds &&
             Context_HumanSpokeSince(bot, SRC_SAY_LOCAL, std::string(), 0, g_ContextAmbientHoldSeconds)) ||
            !Governor_CanSend(bot->GetGUID(), scopeKey))
        {
            ++g_stats.quiet;
            return;
        }

        std::string prompt = SafeFormat(g_CityLifeMountPrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality",
                                                 GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("rider_name", rider->GetName()),
                                        fmt::arg("mount", event.what),
                                        fmt::arg("rarity", event.detail),
                                        fmt::arg("who", Who(event.have)),
                                        fmt::arg("zone", event.zone));
        if (prompt.empty() || prompt == "[Format Error]")
        {
            ++g_stats.quiet;
            return;
        }
        prompt += Roleplay_BuildVoicePrompt(bot);
        prompt += Outlook_ForBot(bot);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = rider->GetGUID().GetRawValue();
        request.source            = SRC_SAY_LOCAL;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = true;   // the rider, or another who came over, may answer
        request.gentle            = true;   // a stranger, as often as not: nothing rude goes with it
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        // Nobody asked for it: a real player who speaks before it is out was
        // not heard by it.
        request.writtenAtSeq      = Context_NextSeq();
        // Once it has walked over and had a look.
        request.extraDelayMs      = urand(1500, 4000);
        if (!OllamaDispatch_Submit(std::move(request)))
        {
            ++g_stats.quiet;
            return;
        }
        ++g_stats.said;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] City life: {} to say something about {}'s {} ({}).",
                     bot->GetName(), rider->GetName(), event.what, event.detail);
    }

    // A bot started dancing (nobody with it), or joined someone dancing.
    void Danced(PlayerbotsPlus::BotEvent const& event)
    {
        ++g_stats.dances;
        if (!g_Enable || !g_CityLifeEnable || !g_CityLifeDanceEmotes || !g_EnableBotEmotes)
            return;
        Player* bot = ObjectAccessor::FindPlayer(event.bot);
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            return;
        Player* with = nullptr;
        if (event.other)
        {
            with = ObjectAccessor::FindPlayer(event.other);
            if (!with || with == bot || !with->IsInWorld() || with->GetMap() != bot->GetMap())
                return;   // gone already: plus ends the dance
        }
        // The dance is plus's, and so is its end: the emote says it and
        // leaves it on.
        BotPlayTextEmote(bot, TEXT_EMOTE_DANCE, with ? with->GetGUID() : ObjectGuid::Empty, true);
        if (with)
            Expression_NoteEmote(bot, with, TEXT_EMOTE_DANCE);   // "why are you dancing with me?" is its to answer
        ++g_stats.emoted;
    }

    void Heard(PlayerbotsPlus::BotEvent const& event)
    {
        switch (event.type)
        {
            case BotEventType::CityMountShown: MountShown(event); break;
            case BotEventType::CityDance:      Danced(event);     break;
            default:                                              break;
        }
    }
#endif
}

void CityLife_Init()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 5
    PlayerbotsPlus::AddBotEventListener(Heard);
#endif
}

CityLifeStats CityLife_GetStats()
{
    return g_stats;
}
