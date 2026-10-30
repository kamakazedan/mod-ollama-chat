#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_bags.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_conversation.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_events.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_phrases.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_world.h"

#include "CellImpl.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    enum class TaskType : uint8_t { ChatReply, Sentiment, Condense, Relationship };

    struct Task
    {
        TaskType          type = TaskType::ChatReply;
        OllamaChatRequest request;

        // Sentiment-only payload.
        uint64_t    sentimentBotGuid    = 0;
        uint64_t    sentimentPlayerGuid = 0;
        std::string sentimentMessage;
        std::string sentimentPrompt;

        // Memory / relationship payload.
        uint64_t    memoryBotGuid   = 0;
        uint64_t    memoryOtherGuid = 0;
        std::string memoryOtherName;
        std::string memoryPrompt;
    };

    struct Completion
    {
        OllamaChatRequest request;
        std::string       text;
        uint32_t          emoteId = 0;
        Clock::time_point deliverAt;
        bool              failed = false;   // nothing to say: only the world thread's bookkeeping runs
    };

    // --- shared state -----------------------------------------------------

    std::mutex              g_queueMutex;
    std::condition_variable g_queueCv;
    std::deque<Task>        g_queue;
    bool                    g_running = false;

    std::mutex             g_doneMutex;
    std::deque<Completion> g_done;

    std::vector<std::thread> g_workers;

    std::atomic<uint32_t> g_inFlight{ 0 };
    std::atomic<uint64_t> g_totalSubmitted{ 0 };
    std::atomic<uint64_t> g_totalDelivered{ 0 };
    std::atomic<uint64_t> g_droppedQueueFull{ 0 };
    std::atomic<uint64_t> g_droppedEmpty{ 0 };
    std::atomic<uint64_t> g_droppedGovernor{ 0 };
    std::atomic<uint64_t> g_droppedPassed{ 0 };
    std::atomic<uint64_t> g_droppedPhrase{ 0 };
    std::atomic<uint64_t> g_totalFailed{ 0 };

    std::mutex  g_errorMutex;
    std::string g_lastError;

    void RecordError(const std::string& what)
    {
        ++g_totalFailed;
        std::lock_guard<std::mutex> lock(g_errorMutex);
        g_lastError = what;
    }

    // A conversation's opening line that came to nothing still has to reach
    // the world thread, which frees the place the conversation was holding
    // for it. (A crowd voice reports through Crowd_ReportWorkerFailure.)
    void ReportOpenerFailure(const OllamaChatRequest& request)
    {
        if (request.openerKey.empty())
            return;
        Completion completion;
        completion.request   = request;
        completion.failed    = true;
        completion.deliverAt = Clock::now();
        std::lock_guard<std::mutex> lock(g_doneMutex);
        g_done.push_back(std::move(completion));
    }

    // --- worker -----------------------------------------------------------

    void RunChatTask(const Task& task)
    {
        // A canned line ("gz") needs no generation, only the same delivery.
        if (!task.request.fixedText.empty())
        {
            Completion completion;
            completion.request = task.request;
            completion.text    = task.request.fixedText;

            uint32_t delayMs = task.request.extraDelayMs;
            if (g_EnableTypingSimulation)
            {
                uint32_t typing = g_TypingSimulationBaseDelay +
                                  static_cast<uint32_t>(completion.text.length()) * g_TypingSimulationDelayPerChar;
                if (g_TypingSimulationMaxDelay > 0 && typing > g_TypingSimulationMaxDelay)
                    typing = g_TypingSimulationMaxDelay;
                delayMs += typing;
            }
            completion.deliverAt = Clock::now() + std::chrono::milliseconds(delayMs);

            std::lock_guard<std::mutex> lock(g_doneMutex);
            g_done.push_back(std::move(completion));
            return;
        }

        OllamaApiResult api = QueryOllama(task.request.prompt, task.request.kind);

        if (!api.ok)
        {
            RecordError(api.error);
            Crowd_ReportWorkerFailure(task.request.crowdPlanId, task.request.crowdSlot);
            ReportOpenerFailure(task.request);
            return;
        }

        uint32_t emoteId = 0;
        std::string text = ProcessLlmResponse(api.text, task.request.botName, &emoteId,
                                              task.request.maxReplyLength);

        // Roleplay mode rejects lines carrying out-of-world vocabulary rather
        // than mangling the sentence around the offending word.
        if (!text.empty())
        {
            std::string filtered = Roleplay_FilterMetaTerms(text);
            if (filtered.empty() && !text.empty() && g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} reply rejected by roleplay filter: '{}'",
                         task.request.botName, text);
            }
            text = std::move(filtered);
        }

        if (text.empty())
        {
            ++g_droppedEmpty;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} produced nothing usable after cleanup.",
                         task.request.botName);
            Crowd_ReportWorkerFailure(task.request.crowdPlanId, task.request.crowdSlot);
            ReportOpenerFailure(task.request);
            return;
        }

        Completion completion;
        completion.request = task.request;
        completion.text    = std::move(text);
        completion.emoteId = emoteId;

        uint32_t delayMs = task.request.extraDelayMs;
        if (g_EnableTypingSimulation)
        {
            uint32_t typing = g_TypingSimulationBaseDelay +
                              static_cast<uint32_t>(completion.text.length()) * g_TypingSimulationDelayPerChar;
            if (g_TypingSimulationMaxDelay > 0 && typing > g_TypingSimulationMaxDelay)
                typing = g_TypingSimulationMaxDelay;
            delayMs += typing;
        }

        // A delay, not a sleep. The old code held a whole thread hostage here.
        completion.deliverAt = Clock::now() + std::chrono::milliseconds(delayMs);

        std::lock_guard<std::mutex> lock(g_doneMutex);
        g_done.push_back(std::move(completion));
    }

    void RunSentimentTask(const Task& task)
    {
        // Sentiment touches only mutex-guarded in-memory state and async DB
        // writes, so it completes here rather than round-tripping to the
        // world thread.
        ApplySentimentAnalysis(task.sentimentBotGuid, task.sentimentPlayerGuid,
                               task.sentimentMessage, task.sentimentPrompt);
    }

    void WorkerLoop()
    {
        for (;;)
        {
            Task task;

            {
                std::unique_lock<std::mutex> lock(g_queueMutex);
                g_queueCv.wait(lock, [] { return !g_running || !g_queue.empty(); });

                if (!g_running && g_queue.empty())
                    return;

                task = std::move(g_queue.front());
                g_queue.pop_front();
            }

            ++g_inFlight;

            try
            {
                switch (task.type)
                {
                    case TaskType::Sentiment:
                        RunSentimentTask(task);
                        break;
                    case TaskType::Condense:
                        Memory_RunCondensation(task.memoryBotGuid, task.memoryPrompt);
                        break;
                    case TaskType::Relationship:
                        Memory_RunRelationshipUpdate(task.memoryBotGuid, task.memoryOtherGuid,
                                                     task.memoryOtherName, task.memoryPrompt);
                        break;
                    default:
                        RunChatTask(task);
                        break;
                }
            }
            catch (const std::exception& e)
            {
                RecordError(e.what());
                LOG_ERROR("module.ollamachat", "[Ollama Chat] Worker exception: {}", e.what());
                if (task.type == TaskType::ChatReply)
                {
                    Crowd_ReportWorkerFailure(task.request.crowdPlanId, task.request.crowdSlot);
                    ReportOpenerFailure(task.request);
                }
            }
            catch (...)
            {
                RecordError("unknown exception");
                LOG_ERROR("module.ollamachat", "[Ollama Chat] Unknown worker exception.");
                if (task.type == TaskType::ChatReply)
                {
                    Crowd_ReportWorkerFailure(task.request.crowdPlanId, task.request.crowdSlot);
                    ReportOpenerFailure(task.request);
                }
            }

            --g_inFlight;
        }
    }

    // --- delivery (world thread only) -------------------------------------

    Channel* ResolveChannel(Player* bot, const std::string& channelName)
    {
        if (!bot || channelName.empty())
            return nullptr;

        ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (!mgr)
            return nullptr;

        return mgr->GetChannel(channelName, bot);
    }

    bool AnyoneInRange(Player* bot, float distance)
    {
        if (!bot || !bot->IsInWorld() || distance <= 0.0f)
            return false;

        // Grid search: this runs on every Say/Yell delivery, and walking every
        // online character to answer "is anyone standing near me" is the kind
        // of thing that adds up on a bot-heavy realm.
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, distance, false, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, distance);

        for (Player* other : found)
            if (other && other != bot && other->IsInWorld())
                return true;

        return false;
    }

    // Returns true when the line actually went out.
    bool RouteMessage(Player* bot, PlayerbotAI* botAI, const Completion& c,
                      const OllamaWorldSnapshot& world,
                      Channel*& outChannel)
    {
        outChannel = nullptr;

        switch (c.request.source)
        {
            case SRC_GENERAL_LOCAL:
            {
                Channel* channel = ResolveChannel(bot, c.request.channelName);
                if (!channel || !OllamaIsOnChannel(channel, bot->GetGUID()))
                    return false;

                // Checked before generating too; re-checked because the only
                // human in the channel can leave during the LLM round trip.
                if (!world.RealPlayerInChannel(channel))
                    return false;

                channel->Say(bot->GetGUID(), c.text, LANG_UNIVERSAL);
                outChannel = channel;
                return true;
            }

            // Guild, party and raid are re-checked here for the same reason
            // say and yell always were: the audience is validated at submit
            // time, and an LLM round trip is seconds long. Whoever the bot was
            // talking to can log out, leave the guild or drop group in that
            // window, and without this the bot announces to an empty channel.
            case SRC_GUILD_LOCAL:
            case SRC_OFFICER_LOCAL:
                if (g_DisableForGuild || !bot->GetGuild())
                    return false;
                if (!world.GuildHasRealPlayer(bot->GetGuildId()))
                    return false;
                return botAI->SayToGuild(c.text);

            case SRC_PARTY_LOCAL:
                if (g_DisableForParty || !bot->GetGroup())
                    return false;
                if (!OllamaGroupHasRealPlayer(bot))
                    return false;
                return botAI->SayToParty(c.text);

            case SRC_RAID_LOCAL:
                if (g_DisableForParty || !bot->GetGroup())
                    return false;
                if (!OllamaGroupHasRealPlayer(bot))
                    return false;
                return botAI->SayToRaid(c.text);

            case SRC_YELL_LOCAL:
                if (g_DisableForSayYell || !AnyoneInRange(bot, g_YellDistance))
                    return false;
                return botAI->Yell(c.text);

            case SRC_WHISPER_LOCAL:
            {
                Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.targetGuid));
                if (!target)
                    return false;
                return botAI->Whisper(c.text, target->GetName());
            }

            case SRC_SAY_LOCAL:
            default:
                if (g_DisableForSayYell || !AnyoneInRange(bot, g_SayDistance))
                    return false;
                return botAI->Say(c.text);
        }
    }

    // Tells a crowd plan how its slot went. Every way out of Deliver that does
    // not put the line in chat counts as "nothing said", so the plan moves on
    // to its next voice instead of waiting out the slot timeout.
    struct CrowdSlotReport
    {
        uint64_t planId = 0;
        uint32_t slot   = 0;
        bool     done   = false;

        void Landed(uint64_t botGuid, const std::string& botName)
        {
            if (planId && !done)
            {
                done = true;
                Crowd_OnSlotFinished(planId, slot, true, botGuid, botName);
            }
        }

        ~CrowdSlotReport()
        {
            if (planId && !done)
                Crowd_OnSlotFinished(planId, slot, false, 0, std::string());
        }
    };

    // Likewise for a conversation's opening line: if it does not go out, the
    // conversation waiting for it lets the place go at once.
    struct OpenerReport
    {
        std::string key;
        uint64_t    botGuid = 0;
        bool        done    = false;

        ~OpenerReport()
        {
            if (!key.empty() && !done)
                Conversation_OpenerLost(key, botGuid);
        }
    };

    // By value: the line can still be trimmed here before it is spoken.
    void Deliver(Completion c, const OllamaWorldSnapshot& world)
    {
        // A worker failure has already told the crowd; only the opener needs
        // to hear about it here.
        CrowdSlotReport crowdReport{ c.failed ? 0 : c.request.crowdPlanId, c.request.crowdSlot };
        OpenerReport    openerReport{ c.request.openerKey, c.request.botGuid };
        if (c.failed)
            return;

        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.botGuid));
        if (!bot || !bot->IsInWorld())
            return;

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI)
            return;

        const ObjectGuid botGuid = bot->GetGUID();

        // Direct address: the bot owes this person an answer, so it skips the
        // suppressions that exist to pace ambient chatter. Decided at submit
        // time in ProcessChat, where the Group was live to consult.
        const bool directAddress = c.request.directAddress;

        // The model never writes real chat markup; whatever looks like it is
        // noise, and a stray '|' would be read by the client. Taken out first,
        // so every check below reads words, and the only links that go out are
        // the ones Links_Resolve makes.
        c.text = TextRules::StripChatMarkup(c.text);

        // What they said, as they saw it: a link in it is its "[Name]".
        const std::string origin = TextRules::StripChatMarkup(c.request.originMessage);

        // The prompt lets a bot that overheard someone else's exchange decline
        // to butt in. Checked here, on the world thread, because the token is
        // a config string that a reload can reassign.
        if (Context_IsPassReply(c.text))
        {
            ++g_droppedPassed;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} chose not to reply.",
                         bot->GetName());
            return;
        }

        // lol, omg and xD are fine now and then; every line is too many.
        const std::string rationed =
            Context_RationFillers(bot, c.request.source, c.request.channelName, c.text);
        if (rationed.empty())
        {
            ++g_droppedEmpty;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} reply was only filler words: '{}'",
                         bot->GetName(), c.text);
            return;
        }
        if (g_DebugEnabled && rationed != c.text)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} fillers rationed: '{}' -> '{}'",
                     bot->GetName(), c.text, rationed);
        c.text = rationed;

        // Players do not offer help like a help desk ("How can I help you?").
        if (!g_ResponseAssistantPhrases.empty())
        {
            const std::string plain = TextRules::StripAssistantSpeak(c.text, g_ResponseAssistantPhrases);
            if (plain.empty())
            {
                ++g_droppedEmpty;
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply was only assistant talk: '{}'",
                             bot->GetName(), c.text);
                return;
            }
            if (g_DebugEnabled && plain != c.text)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} assistant talk removed: '{}' -> '{}'",
                         bot->GetName(), c.text, plain);
            c.text = plain;
        }

        // "just some cave mobs kid": nobody here calls anyone kid.
        if (!g_ResponseDropVocatives.empty())
        {
            const std::string kept = TextRules::StripVocatives(c.text, g_ResponseDropVocatives);
            if (kept.empty())
            {
                ++g_droppedEmpty;
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply was only a form of address: '{}'",
                             bot->GetName(), c.text);
                return;
            }
            if (g_DebugEnabled && kept != c.text)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} form of address dropped: '{}' -> '{}'",
                         bot->GetName(), c.text, kept);
            c.text = kept;
        }

        // Only the player who levelled says ding.
        if (c.request.noSelfDing && TextRules::IsSelfDing(c.text))
        {
            // (A plain "gz" would break character in roleplay mode: dropped.)
            const std::string gz = g_RoleplayEnable ? std::string()
                                 : Events_PickCongrats(bot, c.request.scopeKey, c.request.congratsName);
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} said '{}' for someone else's ding; {}.",
                         bot->GetName(), c.text, gz.empty() ? "dropped" : "'" + gz + "' instead");
            if (gz.empty())
            {
                ++g_droppedEmpty;
                return;
            }
            c.text = gz;
        }

        // Wrath has no keystones, a level 6 has no repair bills, and nobody
        // needs to hear "this is taking forever" from a different bot every
        // few minutes.
        {
            std::string which;
            const PhraseCheck verdict = Phrases_Check(c.text, origin, directAddress, bot->GetLevel(), which);
            if (verdict != PhraseCheck::Ok)
            {
                ++g_droppedPhrase;
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply dropped, {} '{}': '{}'",
                             bot->GetName(),
                             verdict == PhraseCheck::OffEra   ? "not in Wrath:"
                           : verdict == PhraseCheck::OffLevel ? "not at its level:"
                           : verdict == PhraseCheck::Rare     ? "hobby horse said lately:"
                                                              : "said lately:",
                             which, c.text);
                return;
            }
        }

        // Repetition is checked at delivery rather than at submission, because
        // we only know what the model actually said now.
        //
        // Skipped for direct address. Ask a bot the same question twice and
        // the same answer is correct -- suppressing it leaves the asker
        // staring at silence, which reads as the bot being broken rather than
        // as anti-repetition working.
        if (!directAddress && Governor_IsRepetitive(botGuid, c.request.scopeKey, c.text))
        {
            ++g_droppedGovernor;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} reply suppressed as repetitive: '{}'",
                         bot->GetName(), c.text);
            return;
        }

        // A crowd voice is paced by its crowd -- but only while that crowd is
        // still running. A late line from one that has ended or been replaced
        // gets the ordinary cooldowns like any other line. A conversation's
        // opening line is paced by the conversation (one per place, and
        // Conversation.CooldownSeconds between them).
        const bool paced = (c.request.crowdPlanId && Crowd_FindById(c.request.crowdPlanId)) ||
                           !c.request.openerKey.empty();
        const bool allowed = paced
            ? Governor_TryConsumePaced(botGuid, c.request.scopeKey, directAddress)
            : Governor_TryConsumeSend(botGuid, c.request.scopeKey, directAddress);
        if (!allowed)
        {
            ++g_droppedGovernor;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} reply suppressed by cooldown/rate limit.",
                         bot->GetName());
            return;
        }

        // "[Linen Cloth]" becomes a real item link. Last, so every check above
        // saw words rather than markup; `plain` is the line as a reader sees
        // it, for the transcript, the history and the bots that hear it.
        const LinkedLine line = Links_Resolve(bot, c.text);
        c.text = line.text;
        const std::string& plain = line.plain;

        Channel* channel = nullptr;
        if (!RouteMessage(bot, botAI, c, world, channel))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} had nowhere to send its reply ({}).",
                         bot->GetName(), ChatChannelSourceLocalStr[c.request.source]);
            return;
        }

        openerReport.done = true;   // it is out: the conversation hears it below
        Links_NoteSent(line);
        Bags_NoteOffersIn(bot, line.text, c.request.scopeKey);   // a spare thing linked is on offer
        Governor_RecordUtterance(botGuid, c.request.scopeKey, plain);
        Phrases_NoteSaid(plain);
        if (c.request.partyAck)
            PartyCmd_NoteAck(bot, plain);
        ++g_totalDelivered;

        // Into the shared transcript before other bots hear it below, so the
        // ones that answer can see what they are answering.
        const uint64_t seq = Context_RecordLine(bot, c.request.source,
                                                channel ? channel->GetName() : c.request.channelName,
                                                plain, ContextLineKind::Bot);

        // ...and a conversation running here hears it too, before anyone
        // reacts to it below.
        if (c.request.source == SRC_GENERAL_LOCAL || c.request.source == SRC_SAY_LOCAL)
            Conversation_NoteLine(Crowd_KeyFor(c.request.scopeKey, c.request.source, uint32(bot->GetTeamId())),
                                  bot, true, plain, seq, c.request.crowdPlanId, 0);

        // This bot is now in a conversation with whoever it just answered, so
        // their next line in this scope is a turn in it rather than ambient
        // chatter. Only against a real person: an open conversation bypasses
        // pacing, and letting two bots open one with each other is how a
        // bot-to-bot loop would escape every brake in the governor.
        if (c.request.targetGuid)
        {
            Player* addressee = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.targetGuid));
            if (OllamaIsRealPlayer(addressee))
                Governor_NoteConversation(botGuid, ObjectGuid(c.request.targetGuid),
                                          c.request.scopeKey);
        }

        // Body language. Safe here and only here: this is the world thread.
        ScheduleBotExpression(bot, ObjectGuid(c.request.targetGuid), c.emoteId,
                              g_BotExpressionDelayMs);

        if (c.request.recordHistory && c.request.targetGuid)
        {
            AppendBotConversation(c.request.botGuid, c.request.targetGuid, origin, plain);

            // Counts name mentions and, past a threshold, queues a
            // condensation or relationship revision. World thread.
            Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.targetGuid));
            Memory_NoteExchange(c.request.botGuid, c.request.targetGuid,
                                target ? target->GetName() : std::string(),
                                origin, plain);
        }

        if (c.request.updateSentiment && c.request.targetGuid && !origin.empty())
            OllamaDispatch_SubmitSentiment(c.request.botGuid, c.request.targetGuid, origin);

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} ({}, depth {}): {}",
                     bot->GetName(), ChatChannelSourceLocalStr[c.request.source],
                     c.request.chainDepth, c.text);

        // The crowd hears it landed before other bots do, so an answer-back
        // to this line is queued after it rather than in front of it.
        crowdReport.Landed(c.request.botGuid, bot->GetName());

        // Let other bots hear it -- with the chain depth advanced, which is
        // what stops the reply loop that had no brakes before.
        if (c.request.triggerBotReplies &&
            c.request.source != SRC_WHISPER_LOCAL)
        {
            ProcessBotChatMessage(bot, plain, c.request.source, channel,
                                  static_cast<uint8_t>(c.request.chainDepth + 1),
                                  c.request.crowdPlanId);
        }
    }
}

// --------------------------------------------------------------------------

void OllamaDispatch_Start()
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_running)
        return;

    g_running = true;

    uint32_t count = g_DispatchWorkerThreads;
    if (count == 0)
        count = 4;
    if (count > 64)
        count = 64;

    g_workers.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        g_workers.emplace_back(WorkerLoop);

    LOG_INFO("module.ollamachat", "[Ollama Chat] Dispatcher started with {} worker threads.", count);
}

void OllamaDispatch_Stop()
{
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running)
            return;
        g_running = false;
        g_queue.clear();
    }

    g_queueCv.notify_all();

    for (auto& worker : g_workers)
        if (worker.joinable())
            worker.join();

    g_workers.clear();

    {
        std::lock_guard<std::mutex> lock(g_doneMutex);
        g_done.clear();
    }

    LOG_INFO("module.ollamachat", "[Ollama Chat] Dispatcher stopped.");
}

bool OllamaDispatch_Submit(OllamaChatRequest request)
{
    if ((request.prompt.empty() && request.fixedText.empty()) || request.botGuid == 0)
        return false;

    Task task;
    task.type    = TaskType::ChatReply;
    task.request = std::move(request);

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);

        if (!g_running)
            return false;

        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth)
        {
            ++g_droppedQueueFull;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Queue full ({}); dropping request for {}.",
                         g_queue.size(), task.request.botName);
            return false;
        }

        g_queue.push_back(std::move(task));
    }

    ++g_totalSubmitted;
    g_queueCv.notify_one();
    return true;
}

void OllamaDispatch_SubmitSentiment(uint64_t botGuid, uint64_t playerGuid,
                                    const std::string& message)
{
    if (!g_EnableSentimentTracking || message.empty())
        return;

    // Built here, on the world thread: the template is config state that
    // reload rewrites, so a worker must never read it.
    std::string prompt = BuildSentimentPrompt(message);
    if (prompt.empty())
        return;

    Task task;
    task.type                = TaskType::Sentiment;
    task.sentimentBotGuid    = botGuid;
    task.sentimentPlayerGuid = playerGuid;
    task.sentimentMessage    = message;
    task.sentimentPrompt     = std::move(prompt);

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running)
            return;

        // Sentiment is best-effort; never let it crowd out actual chat.
        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth / 2)
            return;

        g_queue.push_back(std::move(task));
    }

    g_queueCv.notify_one();
}

void OllamaDispatch_Update(uint32_t /*diff*/)
{
    const auto now = Clock::now();

    // Move due completions out under the lock, then deliver without it: the
    // delivery path re-enters ProcessBotChatMessage, which submits new work.
    std::vector<Completion> due;

    {
        std::lock_guard<std::mutex> lock(g_doneMutex);

        for (auto it = g_done.begin(); it != g_done.end(); )
        {
            if (it->deliverAt <= now)
            {
                due.push_back(std::move(*it));
                it = g_done.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (due.empty())
        return;

    // One pass over the online player list for the whole tick, rather than one
    // per delivery.
    OllamaWorldSnapshot world;
    world.Build();

    for (const Completion& c : due)
    {
        try
        {
            Deliver(c, world);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] Delivery exception: {}", e.what());
        }
    }
}

void OllamaChat_DispatchEmoteReaction(Player* bot, Player* player, uint32_t textEmote)
{
    if (!bot || !player)
        return;

    OllamaChatRequest request;
    request.botGuid    = bot->GetGUID().GetRawValue();
    request.targetGuid = player->GetGUID().GetRawValue();
    request.source     = SRC_SAY_LOCAL;
    request.chainDepth = 0;
    request.botName    = bot->GetName();
    request.kind       = OllamaRequestKind::EventChatter;
    request.scopeKey   = Governor_MakeScopeKey("Say", 0, "", 0, bot->GetZoneId());
    request.triggerBotReplies = false;
    // Someone emoted at this bot by name. Its own debounce paces this;
    // the ambient say cooldown has no business also silencing it.
    request.directAddress = true;

    request.prompt = BuildEmoteReactionPrompt(bot, player, textEmote);
    if (request.prompt.empty())
        return;

    OllamaDispatch_Submit(std::move(request));
}

namespace
{
    // Background upkeep must never crowd out actual chat, so it gets a
    // stricter queue allowance than a reply does.
    bool SubmitBackground(Task&& task)
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running)
            return false;
        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth / 2)
            return false;

        g_queue.push_back(std::move(task));
        return true;
    }
}

void OllamaDispatch_SubmitCondensation(uint64_t botGuid, const std::string& prompt)
{
    if (botGuid == 0 || prompt.empty())
        return;

    Task task;
    task.type          = TaskType::Condense;
    task.memoryBotGuid = botGuid;
    task.memoryPrompt  = prompt;

    if (SubmitBackground(std::move(task)))
        g_queueCv.notify_one();
}

void OllamaDispatch_SubmitRelationship(uint64_t botGuid, uint64_t otherGuid,
                                       const std::string& otherName,
                                       const std::string& prompt)
{
    if (botGuid == 0 || otherGuid == 0 || prompt.empty())
        return;

    Task task;
    task.type            = TaskType::Relationship;
    task.memoryBotGuid   = botGuid;
    task.memoryOtherGuid = otherGuid;
    task.memoryOtherName = otherName;
    task.memoryPrompt    = prompt;

    if (SubmitBackground(std::move(task)))
        g_queueCv.notify_one();
}

OllamaDispatchStats OllamaDispatch_GetStats()
{
    OllamaDispatchStats stats{};

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        stats.queuedRequests = static_cast<uint32_t>(g_queue.size());
        stats.workers        = static_cast<uint32_t>(g_workers.size());
    }
    {
        std::lock_guard<std::mutex> lock(g_doneMutex);
        stats.pendingDeliveries = static_cast<uint32_t>(g_done.size());
    }
    {
        std::lock_guard<std::mutex> lock(g_errorMutex);
        stats.lastError = g_lastError;
    }

    stats.inFlight              = g_inFlight.load();
    stats.totalSubmitted        = g_totalSubmitted.load();
    stats.totalDelivered        = g_totalDelivered.load();
    stats.totalDroppedQueueFull = g_droppedQueueFull.load();
    stats.totalDroppedEmpty     = g_droppedEmpty.load();
    stats.totalDroppedGovernor  = g_droppedGovernor.load();
    stats.totalDroppedPassed    = g_droppedPassed.load();
    stats.totalDroppedPhrase    = g_droppedPhrase.load();
    stats.totalFailed           = g_totalFailed.load();

    return stats;
}

// Explicit template instantiations are exempt from access checking
// ([temp.spec.general]), so instantiating this with &Channel::IsOn is legal
// and hands the member pointer out through a friend function. If the core
// ever changes IsOn's signature, this stops compiling rather than misbehaving.
namespace
{
    using ChannelIsOnFn = bool (Channel::*)(ObjectGuid) const;

    ChannelIsOnFn ChannelIsOnPtr();

    template <ChannelIsOnFn Fn>
    struct ChannelIsOnAccess
    {
        friend ChannelIsOnFn ChannelIsOnPtr() { return Fn; }
    };

    template struct ChannelIsOnAccess<&Channel::IsOn>;
}

bool OllamaIsOnChannel(Channel const* channel, ObjectGuid guid)
{
    return channel && (channel->*ChannelIsOnPtr())(guid);
}

Channel* OllamaResolveZoneChannel(Player* bot, uint32_t chatChannelId)
{
    if (!bot)
        return nullptr;

    ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
    if (!mgr)
        return nullptr;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    std::string zoneName;
    if (botAI)
        if (AreaTableEntry const* zone = botAI->GetCurrentZone())
            zoneName = PlayerbotAI::GetLocalizedAreaName(zone);

    for (auto const& [key, channel] : mgr->GetChannels())
    {
        if (!channel || channel->GetName().empty())
            continue;
        if (channel->GetChannelId() != chatChannelId)
            continue;

        // Global channels (LFG, WorldDefense) are not zone-scoped.
        const bool zoneScoped = (chatChannelId != uint32_t(ChatChannelId::LOOKING_FOR_GROUP) &&
                                 chatChannelId != uint32_t(ChatChannelId::WORLD_DEFENSE));

        if (zoneScoped)
        {
            if (zoneName.empty())
                continue;
            if (channel->GetName().find(zoneName) == std::string::npos)
                continue;
        }

        return channel;
    }

    return nullptr;
}
