#include "mod-ollama-chat_answer.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"

#include "Player.h"

void OllamaBotAnswer(Player* player, Player* bot, const std::string& msg, ChatChannelSourceLocal source,
                     const std::string& scopeKey, uint64_t seq, std::string hint, CrowdLineKind kind)
{
    if (!player || !bot || hint.empty() || hint == "[Format Error]")
        return;

    if (source == SRC_WHISPER_LOCAL)
    {
        if ((g_DisableRepliesInCombat && bot->IsInCombat()) || !Governor_CanSend(bot->GetGUID(), scopeKey, true))
            return;
        std::string prompt = GenerateBotPrompt(bot, msg, player, SRC_WHISPER_LOCAL, std::string(), seq, hint);
        if (prompt.empty())
            return;

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = player->GetGUID().GetRawValue();
        request.source            = SRC_WHISPER_LOCAL;
        request.directAddress     = true;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.originMessage     = msg;
        request.kind              = (g_RoleplayEnable && g_RoleplayStrictness >= 1)
                                        ? OllamaRequestKind::RoleplayReply
                                        : OllamaRequestKind::ChatReply;
        request.triggerBotReplies = false;
        request.recordHistory     = true;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        OllamaDispatch_Submit(std::move(request));
        return;
    }

    CrowdSlot slot;
    slot.botGuid   = bot->GetGUID().GetRawValue();
    slot.addressed = true;   // owed an answer: no pacing cooldowns
    slot.hint      = std::move(hint);

    const std::string key = Crowd_KeyFor(scopeKey, source, uint32(player->GetTeamId()));
    if (seq)
    {
        if (CrowdPlan* running = Crowd_Find(key))
        {
            if (running->messageSeq == seq && running->speakerGuid == player->GetGUID().GetRawValue())
            {
                running->slots.push_back(std::move(slot));
                return;
            }
        }
    }

    CrowdPlan plan;
    plan.key          = key;
    plan.scopeKey     = scopeKey;
    plan.source       = source;
    plan.speakerGuid  = player->GetGUID().GetRawValue();
    plan.speakerIsBot = false;
    plan.speakerName  = player->GetName();
    plan.message      = msg;
    plan.messageSeq   = seq;
    plan.kind         = kind;
    plan.listeners    = 1;
    plan.closed       = true;   // one answer, no follow-ups
    plan.slots.push_back(std::move(slot));
    Crowd_Start(std::move(plan), 0);
}
