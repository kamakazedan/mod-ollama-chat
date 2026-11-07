#include "mod-ollama-chat_marks.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <chrono>
#include <unordered_map>

// The marks themselves -- whose is whose, the sap, the skull moving on -- are
// mod-playerbots-plus's (Marks.* there). This is the rogue saying why it is
// not sapping the star.

namespace
{
    using Clock = std::chrono::steady_clock;

    MarksStats g_stats;

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 2
    constexpr uint8_t SAP_LEVEL = 10;   // rogues learn Sap at this level

    // When the rogue last said each reason: rogue guid * 4 + reason.
    std::unordered_map<uint64_t, Clock::time_point> g_said;

    // The rogue says, in party, why it is not sapping the star -- in its own
    // words (Marks.SayPrompt with one of the Marks.Sap* situations).
    void Heard(PlayerbotsPlus::BotEvent const& event)
    {
        const std::string* situation = nullptr;
        uint8_t reason = 0;
        switch (event.type)
        {
            case PlayerbotsPlus::BotEventType::SapNotLearned: situation = &g_MarksSapNotLearned; reason = 0; break;
            case PlayerbotsPlus::BotEventType::SapWrongType:  situation = &g_MarksSapWrongType;  reason = 1; break;
            case PlayerbotsPlus::BotEventType::SapNoStealth:  situation = &g_MarksSapNoStealth;  reason = 2; break;
            case PlayerbotsPlus::BotEventType::SapFailed:     situation = &g_MarksSapFailed;     reason = 3; break;
            default: return;
        }
        ++g_stats.heard;

        const Clock::time_point now = Clock::now();
        const auto cooldown = std::chrono::seconds(g_MarksSayCooldownSeconds);
        if (g_said.size() > 256)
            for (auto it = g_said.begin(); it != g_said.end();)
                it = (now - it->second > cooldown) ? g_said.erase(it) : std::next(it);

        if (!g_Enable || !g_MarksSayEnable || situation->empty() || g_MarksSayPrompt.empty())
            return;
        Player* rogue = ObjectAccessor::FindConnectedPlayer(event.bot);
        if (!rogue || !rogue->IsInWorld())
            return;

        const uint64_t key = rogue->GetGUID().GetRawValue() * 4 + reason;
        if (auto it = g_said.find(key); it != g_said.end() && now - it->second < cooldown)
            return;

        Group* group = rogue->GetGroup();
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(rogue);
        if (!group || !ai || !ai->GetChatHelper())
            return;
        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           group->GetGUID().GetCounter());
        if (!Governor_CanSend(rogue->GetGUID(), scopeKey, true))
            return;

        Player* asker = event.other ? ObjectAccessor::FindConnectedPlayer(event.other) : nullptr;
        const std::string askerName = asker ? asker->GetName() : std::string("someone");
        const std::string what = SafeFormat(*situation,
                                            fmt::arg("player_name", askerName),
                                            fmt::arg("target", event.what),
                                            fmt::arg("detail", event.detail),
                                            fmt::arg("bot_level", rogue->GetLevel()),
                                            fmt::arg("sap_level", SAP_LEVEL));
        if (what.empty() || what == "[Format Error]")
            return;
        std::string prompt = SafeFormat(g_MarksSayPrompt,
                                        fmt::arg("bot_name", rogue->GetName()),
                                        fmt::arg("bot_level", rogue->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(rogue->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(rogue->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(GetBotPersonality(rogue))),
                                        fmt::arg("player_name", askerName),
                                        fmt::arg("situation", what));
        if (prompt.empty() || prompt == "[Format Error]")
            return;
        prompt += Roleplay_BuildVoicePrompt(rogue);

        OllamaChatRequest request;
        request.botGuid           = rogue->GetGUID().GetRawValue();
        request.source            = source;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = rogue->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.directAddress     = true;    // an answer to the mark, not ambient chatter
        request.triggerBotReplies = false;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(rogue);
        if (OllamaDispatch_Submit(std::move(request)))
        {
            g_said[key] = now;
            ++g_stats.explained;
        }
    }
#endif
}

void Marks_Init()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 2
    PlayerbotsPlus::AddBotEventListener(Heard);
#endif
}

MarksStats Marks_GetStats()
{
    return g_stats;
}
