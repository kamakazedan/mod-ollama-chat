#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_clock.h"
#include "mod-ollama-chat_aside.h"
#include "mod-ollama-chat_bags.h"
#include "mod-ollama-chat_citylife.h"
#include "mod-ollama-chat_congrats.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_conversation.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_events.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_tradechat.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_joined.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_mute.h"
#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_phrases.h"
#include "mod-ollama-chat_progress.h"
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
#include <unordered_map>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    enum class TaskType : uint8_t { ChatReply, Sentiment, Condense, Relationship, Script };

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

        // A /say exchange between bots (mod-ollama-chat_mingle).
        uint64_t    scriptId         = 0;
        std::string scriptPrompt;
        uint32_t    scriptNumPredict = 0;
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

    // Lines other lines wait on (OllamaChatRequest::saysKey): when each was
    // sent to be written, and what became of it. Submitted and delivered on
    // the world thread; the lock is only so a stray submit elsewhere is safe.
    struct Awaited
    {
        enum class State : uint8_t
        {
            Writing,
            Out,
            Gone
        };
        Clock::time_point sent;
        Clock::time_point done;
        State             state = State::Writing;
    };
    std::mutex                            g_awaitedMutex;
    std::unordered_map<uint64_t, Awaited> g_awaited;
    constexpr auto kAwaitAtMost = std::chrono::seconds(90);   // still being written after this: given up on
    constexpr auto kAwaitGap    = std::chrono::seconds(3);    // an answer never in the same breath as its ask
    constexpr auto kAwaitKeep   = std::chrono::minutes(10);
    std::atomic<uint64_t> g_droppedUnasked{ 0 };

    void AwaitedSent(uint64_t key)
    {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(g_awaitedMutex);
        for (auto it = g_awaited.begin(); it != g_awaited.end();)
            it = now - it->second.sent > kAwaitKeep ? g_awaited.erase(it) : std::next(it);
        g_awaited[key] = Awaited{ now, {}, Awaited::State::Writing };
    }

    void AwaitedDone(uint64_t key, bool out)
    {
        std::lock_guard<std::mutex> lock(g_awaitedMutex);
        auto it = g_awaited.find(key);
        if (it == g_awaited.end() || it->second.state != Awaited::State::Writing)
            return;
        it->second.state = out ? Awaited::State::Out : Awaited::State::Gone;
        it->second.done  = Clock::now();
    }

    // What a line waiting on `key` does now: goes, waits until `until`, or
    // is dropped -- what it answers never went out.
    enum class AfterIs : uint8_t
    {
        Go,
        Wait,
        Drop
    };
    AfterIs After(uint64_t key, Clock::time_point now, Clock::time_point& until)
    {
        std::lock_guard<std::mutex> lock(g_awaitedMutex);
        auto it = g_awaited.find(key);
        if (it == g_awaited.end())
            return AfterIs::Drop;
        Awaited const& a = it->second;
        switch (a.state)
        {
            case Awaited::State::Out:
                if (now >= a.done + kAwaitGap)
                    return AfterIs::Go;
                until = a.done + kAwaitGap;
                return AfterIs::Wait;
            case Awaited::State::Gone:
                return AfterIs::Drop;
            case Awaited::State::Writing:
            default:
                if (now - a.sent >= kAwaitAtMost)
                    return AfterIs::Drop;
                until = now + std::chrono::seconds(2);
                return AfterIs::Wait;
        }
    }

    // However a delivery ends, a line others wait on is noted out or gone.
    struct AwaitedOutcome
    {
        uint64_t key = 0;
        bool     out = false;
        ~AwaitedOutcome()
        {
            if (key)
                AwaitedDone(key, out);
        }
    };

    std::mutex                      g_scriptMutex;
    std::vector<OllamaScriptResult> g_scripts;

    std::vector<std::thread> g_workers;

    std::atomic<uint32_t> g_inFlight{ 0 };
    std::atomic<uint64_t> g_totalSubmitted{ 0 };
    std::atomic<uint64_t> g_totalDelivered{ 0 };
    std::atomic<uint64_t> g_droppedQueueFull{ 0 };
    std::atomic<uint64_t> g_droppedEmpty{ 0 };

    // When each bot last let an insult through (world thread, at delivery).
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> g_insultUse;

    // A pet-name style's lines out since its last pet name, while fewer than
    // Response.PetNameEveryLines (world thread, at delivery).
    std::unordered_map<uint64_t, uint32_t> g_petNameGap;

    bool PetNameStyle(Player* bot)
    {
        const std::string style = GetBotPersonality(bot);
        return std::find(g_ResponsePetNameStyles.begin(), g_ResponsePetNameStyles.end(), style) !=
               g_ResponsePetNameStyles.end();
    }
    std::atomic<uint64_t> g_droppedGovernor{ 0 };
    std::atomic<uint64_t> g_droppedPassed{ 0 };
    std::atomic<uint64_t> g_droppedPhrase{ 0 };
    std::atomic<uint64_t> g_droppedOvertaken{ 0 };
    std::atomic<uint64_t> g_retried{ 0 };
    std::atomic<uint64_t> g_totalFailed{ 0 };

    std::mutex  g_errorMutex;
    std::string g_lastError;
    std::chrono::steady_clock::time_point g_lastErrorAt{};
    std::deque<std::chrono::steady_clock::time_point> g_errorTimes;   // the last hour's failures

    void RecordError(const std::string& what)
    {
        ++g_totalFailed;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_errorMutex);
        g_lastError = what;
        g_lastErrorAt = now;
        g_errorTimes.push_back(now);
        while (now - g_errorTimes.front() > std::chrono::hours(1))
            g_errorTimes.pop_front();
    }

    // A line that came to nothing still has to reach the world thread when
    // something waits on it there: a conversation's opening line frees the
    // place the conversation was holding for it, and a line that has to go
    // out goes out in its stock words. (A crowd voice reports through
    // Crowd_ReportWorkerFailure.)
    void ReportFailure(const OllamaChatRequest& request)
    {
        if (request.openerKey.empty() && request.fallbackText.empty() && !request.saysKey)
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
            ReportFailure(task.request);
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
            ReportFailure(task.request);
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

    void ReportScriptFailure(uint64_t scriptId)
    {
        OllamaScriptResult result;
        result.scriptId = scriptId;
        std::lock_guard<std::mutex> lock(g_scriptMutex);
        if (g_scripts.size() < 64)
            g_scripts.push_back(std::move(result));
    }

    // A whole /say exchange: the model's text as it came, for the world
    // thread to read (mod-ollama-chat_mingle).
    void RunScriptTask(const Task& task)
    {
        OllamaScriptResult result;
        result.scriptId = task.scriptId;
        OllamaApiResult api = QueryOllama(task.scriptPrompt, OllamaRequestKind::RandomChatter, task.scriptNumPredict);
        if (api.ok && !api.text.empty())
        {
            result.ok   = true;
            result.text = std::move(api.text);
        }
        else if (!api.ok)
            RecordError(api.error);

        std::lock_guard<std::mutex> lock(g_scriptMutex);
        if (g_scripts.size() < 64)
            g_scripts.push_back(std::move(result));
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
                    case TaskType::Script:
                        RunScriptTask(task);
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
                    ReportFailure(task.request);
                }
                else if (task.type == TaskType::Script)
                    ReportScriptFailure(task.scriptId);
            }
            catch (...)
            {
                RecordError("unknown exception");
                LOG_ERROR("module.ollamachat", "[Ollama Chat] Unknown worker exception.");
                if (task.type == TaskType::ChatReply)
                {
                    Crowd_ReportWorkerFailure(task.request.crowdPlanId, task.request.crowdSlot);
                    ReportFailure(task.request);
                }
                else if (task.type == TaskType::Script)
                    ReportScriptFailure(task.scriptId);
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
                if (!botAI->Say(c.text))
                    return false;
                // Heads turn to whoever speaks, and it does not walk off
                // mid-sentence (mod-playerbots-plus's city life).
                Hangout_Spoke(bot);
                Hangout_Busy(bot, 8);
                return true;
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
        // A line others wait on: noted out, or gone, however this ends.
        AwaitedOutcome awaited{ c.request.saysKey };

        // A line that has to go out goes out in its stock words when the model
        // could not be reached.
        if (c.failed && !c.request.fallbackText.empty())
        {
            c.failed = false;
            c.text   = c.request.fallbackText;
        }

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

        // An answer: only after what it answers is out, and never if that
        // never goes out.
        if (c.request.afterKey)
        {
            Clock::time_point until;
            switch (After(c.request.afterKey, Clock::now(), until))
            {
                case AfterIs::Wait:
                {
                    awaited.key = 0;   // not done with: back to wait
                    c.deliverAt = until;
                    std::lock_guard<std::mutex> lock(g_doneMutex);
                    g_done.push_back(std::move(c));
                    return;
                }
                case AfterIs::Drop:
                    ++g_droppedUnasked;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat",
                                 "[Ollama Chat] {}'s answer dropped: what it answers never went out.", bot->GetName());
                    return;
                case AfterIs::Go:
                    break;
            }
        }

        // "right, lets go" after a rest: moot once somebody said ready while
        // it was being written.
        if (c.request.breakEndCall && !PartyCmd_BreakEndStillDue(bot))
            return;

        // "ok, taking that as a yes, pulling": moot once somebody has said anything.
        if (c.request.leadOffCall && !PartyCmd_LeadOffStillDue(bot))
            return;

        // An unprompted line written before a real player said something
        // there: it would talk straight over them, about something else.
        if (c.request.writtenAtSeq &&
            Context_HumanSpokeSince(bot, c.request.source, c.request.channelName, c.request.writtenAtSeq, 0))
        {
            ++g_droppedOvertaken;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {}'s unprompted line was overtaken by a real "
                         "player's; dropped: '{}'", bot->GetName(), c.text);
            return;
        }

        const ObjectGuid botGuid = bot->GetGUID();

        // Direct address: the bot owes this person an answer, so it skips the
        // suppressions that exist to pace ambient chatter. Decided at submit
        // time in ProcessChat, where the Group was live to consult.
        const bool directAddress = c.request.directAddress;

        // A dungeon run by bots with nobody playing among them is silent:
        // only an answer owed to someone who spoke to it goes out.
        if (!directAddress && c.request.source != SRC_WHISPER_LOCAL && OllamaInBotOnlyDungeon(bot))
        {
            ++g_droppedPassed;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} is in a dungeon run by bots; line dropped: '{}'",
                         bot->GetName(), c.text);
            return;
        }

        // Away from the keyboard, hanging out in a city (mod-ollama-chat_hangout):
        // nothing to say unless someone talks to it -- and then it is back.
        if (Hangout_IsAway(bot) && !directAddress && c.request.source != SRC_WHISPER_LOCAL)
        {
            ++g_droppedPassed;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} is AFK; line dropped: '{}'", bot->GetName(),
                         c.text);
            return;
        }

        // The model never writes real chat markup; whatever looks like it is
        // noise, and a stray '|' would be read by the client. Taken out first,
        // so every check below reads words, and the only links that go out are
        // the ones Links_Resolve makes.
        c.text = TextRules::StripChatMarkup(c.text);

        // What they said, as they saw it: a link in it is its "[Name]".
        const std::string origin = TextRules::StripChatMarkup(c.request.originMessage);

        // About something that is not there -- "nice mount" to someone on
        // foot: not said at all.
        if (!c.request.mustNotSay.empty() &&
            CrowdText::ContainsPhrase(CrowdText::Words(c.text), c.request.mustNotSay))
        {
            ++g_droppedPhrase;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} named something that is not there; line dropped: "
                         "'{}'", bot->GetName(), c.text);
            return;
        }

        // A line about a deal names the price agreed or offered, and no price
        // nobody named: the model's numbers are no deal.
        bool priceWrong = false;
        if (c.request.mustPrice || !c.request.okPrices.empty())
        {
            const std::vector<uint32_t> prices = TextRules::PricesIn(TextRules::WithoutBracketNames(c.text));
            bool named = !c.request.mustPrice;
            for (uint32_t p : prices)
            {
                if (p == c.request.mustPrice)
                    named = true;
                else if (std::find(c.request.okPrices.begin(), c.request.okPrices.end(), p) ==
                         c.request.okPrices.end())
                    priceWrong = true;
            }
            priceWrong = priceWrong || !named;
        }

        // A line that has to say something in particular: the model's, if it
        // did say it, else the stock one.
        if (!c.request.fallbackText.empty() && c.text != c.request.fallbackText &&
            (Context_IsPassReply(c.text) || CrowdText::Words(c.text).empty() || priceWrong ||
             (!c.request.mustSay.empty() &&
              !CrowdText::ContainsPhrase(CrowdText::Words(c.text), c.request.mustSay))))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} did not say what it had to ('{}'): '{}' instead.",
                         bot->GetName(), c.text, c.request.fallbackText);
            if (priceWrong)
                TradeChat_NoteFallback();
            c.text = c.request.fallbackText;
        }
        else if (priceWrong && c.request.fallbackText.empty())
        {
            // ...and with no stock line to say instead, nothing: a price
            // nobody named is never said.
            ++g_droppedPhrase;
            TradeChat_NoteFallback();
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} named a price nobody gave it: '{}' dropped.",
                         bot->GetName(), c.text);
            return;
        }

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

        // :) and <3 now and then, not in every line, nor in every bot's.
        {
            const std::string faces = Context_RationEmoticons(bot, c.request.source, c.request.channelName, c.text);
            if (faces.empty())
            {
                ++g_droppedEmpty;
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply was only a smiley: '{}'",
                             bot->GetName(), c.text);
                return;
            }
            if (g_DebugEnabled && faces != c.text)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} smiley rationed: '{}' -> '{}'", bot->GetName(),
                         c.text, faces);
            c.text = faces;
        }

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

        // "the ah is a nightmare sugar": pet names are one style's
        // (Response.PetNameStyles), and even that one does not call someone
        // something every line. A bot that picked one up from the chat above
        // -- one says "sugar", and soon the whole channel does -- or a
        // pet-name style that used one in its last few lines loses it.
        if (!g_ResponsePetNames.empty())
        {
            auto gap = g_petNameGap.find(botGuid.GetRawValue());
            const bool rested = gap == g_petNameGap.end() || g_ResponsePetNameEveryLines <= 1;
            if (!rested || !PetNameStyle(bot))
            {
                const std::string kept = TextRules::StripVocatives(c.text, g_ResponsePetNames);
                if (kept.empty())
                {
                    ++g_droppedEmpty;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply was only a pet name: '{}'",
                                 bot->GetName(), c.text);
                    return;
                }
                if (g_DebugEnabled && kept != c.text)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} pet name dropped: '{}' -> '{}'",
                             bot->GetName(), c.text, kept);
                c.text = kept;
            }
        }

        // "far too quiet for a Tuesday" -- on a Saturday. The model has no
        // calendar: a weekday it uses for today becomes today's.
        if (g_ClockFixWeekdays)
        {
            const std::string dated = TextRules::FixWeekday(c.text, Clock_Weekday());
            if (g_DebugEnabled && dated != c.text)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} weekday put right: '{}' -> '{}'",
                         bot->GetName(), c.text, dated);
            c.text = dated;
        }

        // "fair point" is just "fair" now: a phrase people have stopped
        // saying, the way they say it now (Response.Swaps).
        if (!g_ResponseSwaps.empty())
        {
            const std::string now = TextRules::SwapPhrases(c.text, g_ResponseSwaps, [](size_t n)
                                                           { return size_t(urand(0, uint32(n - 1))); });
            if (g_DebugEnabled && now != c.text)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} said it the way people say it now: '{}' -> '{}'",
                         bot->GetName(), c.text, now);
            c.text = now;
        }

        // "i'm the spider king now. avengers.": the binger naming what it
        // quoted, however often it is told not to. The name goes, the line
        // stays (Response.QuoteSources, .QuoteSourceStyles).
        if (!g_ResponseQuoteSourceStyles.empty() && !g_ResponseQuoteSources.names.empty())
        {
            std::string style;
            for (char ch : GetBotPersonality(bot))
                style += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (g_ResponseQuoteSourceStyles.contains(style))
            {
                const std::string unsourced = TextRules::StripQuoteSource(c.text, g_ResponseQuoteSources, origin);
                if (g_DebugEnabled && unsourced != c.text)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} named what it quoted: '{}' -> '{}'",
                             bot->GetName(), c.text, unsourced);
                c.text = unsourced;
            }
        }

        // "nah just chilling here you knob": an insult out of nowhere. One
        // fits banter someone started, or a line that was rude to it, or a
        // bot whose style is just rude -- and even then not every time (the
        // time counts from a line that really went out with one).
        bool insultKept = false;
        const std::string unInsulted = (!g_ResponseInsultWords.empty() &&
                                        CrowdText::ContainsPhrase(CrowdText::Words(c.text), g_ResponseInsultWords))
                                           ? TextRules::StripInsults(c.text, g_ResponseInsultWords)
                                           : c.text;
        if (unInsulted != c.text)   // one thrown at someone; "dont be a knob" is only talk
        {
            const std::string& kept = unInsulted;
            const std::vector<std::string> said = CrowdText::Words(origin);
            const bool provoked = CrowdText::ContainsPhrase(said, g_ResponseInsultWords) ||
                                  CrowdText::ContainsPhrase(said, g_ResponseProvokeWords);
            const std::string style = GetBotPersonality(bot);
            const bool rude = std::find(g_ResponseInsultPersonalities.begin(), g_ResponseInsultPersonalities.end(),
                                        style) != g_ResponseInsultPersonalities.end();
            auto last = g_insultUse.find(botGuid.GetRawValue());
            const bool rested = last == g_insultUse.end() || std::chrono::steady_clock::now() - last->second >=
                                                                 std::chrono::seconds(g_ResponseInsultCooldownSeconds);
            if ((provoked || rude) && rested)
                insultKept = true;
            else
            {
                if (kept.empty())
                {
                    ++g_droppedEmpty;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply was only an insult: '{}'",
                                 bot->GetName(), c.text);
                    return;
                }
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} insult out of nowhere dropped: '{}' -> '{}'",
                             bot->GetName(), c.text, kept);
                c.text = kept;
                // ...and the rude gesture it was going to go with.
                switch (c.emoteId)
                {
                    case TEXT_EMOTE_RUDE:  case TEXT_EMOTE_SPIT:  case TEXT_EMOTE_THREATEN: case TEXT_EMOTE_INSULT:
                    case TEXT_EMOTE_MOCK:  case TEXT_EMOTE_TAUNT: case TEXT_EMOTE_RASP:     case TEXT_EMOTE_CHICKEN:
                        c.emoteId = 0;
                        break;
                    default:
                        break;
                }
            }
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

        // A line owed to someone that came back with a stock phrase gets one
        // more go, told what to leave out, rather than leaving them without an
        // answer. Its crowd slot or conversation place waits for the second go.
        auto retryWith = [&](const std::string& note, const std::string& phrase) -> bool
        {
            if (!g_ResponseRetryOwed || !directAddress || c.request.retried || c.request.prompt.empty() ||
                !c.request.fixedText.empty() || note.empty() || note == "[Format Error]")
                return false;
            OllamaChatRequest again = c.request;
            again.retried = true;
            again.prompt += " " + note;
            if (!OllamaDispatch_Submit(std::move(again)))
                return false;
            crowdReport.done  = true;   // the second go reports, however it goes
            openerReport.done = true;
            ++g_retried;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} asked again without '{}': '{}'", bot->GetName(),
                         phrase, c.text);
            return true;
        };
        auto retry = [&](const std::string& phrase) -> bool
        {
            return !g_ResponseRetryNote.empty() &&
                   retryWith(SafeFormat(g_ResponseRetryNote, fmt::arg("phrase", phrase)), phrase);
        };

        // Wrath has no keystones, a level 6 has no repair bills, and nobody
        // needs to hear "this is taking forever" from a different bot every
        // few minutes.
        {
            std::string which;
            const bool answeringPlayer = c.request.chainDepth == 0;   // not a bot's line
            PhraseCheck verdict =
                Phrases_Check(c.text, origin, answeringPlayer, directAddress, bot->GetLevel(), which);
            if (verdict != PhraseCheck::Ok && !c.request.fallbackText.empty() && c.text != c.request.fallbackText)
            {
                // It has to go out: in its stock words, then.
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} line had '{}' in it: '{}' instead.",
                             bot->GetName(), which, c.request.fallbackText);
                c.text  = c.request.fallbackText;
                verdict = PhraseCheck::Ok;
            }
            if (verdict != PhraseCheck::Ok)
            {
                ++g_droppedPhrase;
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply dropped, {} '{}': '{}'",
                             bot->GetName(),
                             verdict == PhraseCheck::FourthWall ? "breaks the fourth wall:"
                           : verdict == PhraseCheck::OffEra   ? "not in Wrath:"
                           : verdict == PhraseCheck::OffLevel ? "not at its level:"
                           : verdict == PhraseCheck::Rare     ? "hobby horse said lately:"
                                                              : "said lately:",
                             which, c.text);
                retry(which);
                return;
            }
        }

        // The same phrase from bot after bot, all over the realm: an ambient
        // line goes, one owed to someone gets another go.
        if (!c.request.noEcho)
        {
            std::string echo;
            if (Phrases_IsEcho(botGuid.GetRawValue(), c.text, origin, echo))
            {
                if (directAddress && !c.request.retried && retry(echo))
                    return;
                if (!directAddress)
                {
                    ++g_droppedPhrase;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} reply dropped, echoes other bots ('{}'): '{}'",
                                 bot->GetName(), echo, c.text);
                    return;
                }
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

        // ...but owed or not, not a line it said itself a moment ago to
        // something else: "i'm going to make him an offer he can't refuse",
        // to one bot and then the next. It gets one more go, told so
        // (Repetition.OwnRepeatNote). Missed twice, it is held back where a
        // bot was owed it, and goes out where a person was: better the same
        // line than none.
        if (directAddress)
        {
            std::string said;
            if (Governor_RepeatsOwnLine(botGuid, c.text, origin, said))
            {
                const std::string note = g_RepetitionOwnRepeatNote.empty()
                                             ? std::string()
                                             : SafeFormat(g_RepetitionOwnRepeatNote, fmt::arg("line", said));
                if (retryWith(note, said))
                    return;
                if (c.request.chainDepth > 0)
                {
                    ++g_droppedGovernor;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat",
                                 "[Ollama Chat] Bot {} reply to a bot suppressed, it said that already: '{}'",
                                 bot->GetName(), c.text);
                    return;
                }
            }
        }

        // A crowd voice is paced by its crowd -- but only while that crowd is
        // still running. A late line from one that has ended or been replaced
        // gets the ordinary cooldowns like any other line. A conversation's
        // opening line is paced by the conversation (one per place, and
        // Conversation.CooldownSeconds between them).
        const bool paced = (c.request.crowdPlanId && Crowd_FindById(c.request.crowdPlanId)) ||
                           !c.request.openerKey.empty();
        // A line of a /say exchange between bots was written with the rest of
        // it, in one request, and its exchange paces it: it only notes that
        // the bot spoke (mod-ollama-chat_mingle).
        bool allowed = true;
        if (c.request.mingle)
            Governor_NoteSpoke(botGuid, c.request.scopeKey);
        else
            allowed = paced ? Governor_TryConsumePaced(botGuid, c.request.scopeKey, directAddress)
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

        // "[Linen Cloth]" becomes a real item link -- unless this chat had it
        // linked a moment ago, in the line being answered or by another bot.
        // Last, so every check above saw words rather than markup; `plain` is
        // the line as a reader sees it, for the transcript, the history and
        // the bots that hear it.
        const LinkedLine line = Links_Resolve(bot, c.text, c.request.scopeKey, c.request.originMessage);
        c.text = line.text;
        const std::string& plain = line.plain;

        Hangout_ComeBack(bot);   // AFK and talked to: back at the keyboard, so no <AFK> on the line

        Channel* channel = nullptr;
        bool routed = false;
        {
            MuteOwnLine own;   // what the module says for a bot is never muted, "Hi" or not
            routed = RouteMessage(bot, botAI, c, world, channel);
        }
        if (!routed)
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} had nowhere to send its reply ({}).",
                         bot->GetName(), ChatChannelSourceLocalStr[c.request.source]);
            return;
        }

        openerReport.done = true;   // it is out: the conversation hears it below
        if (insultKept)
            g_insultUse[botGuid.GetRawValue()] = std::chrono::steady_clock::now();
        if (!g_ResponsePetNames.empty() && g_ResponsePetNameEveryLines > 1 && PetNameStyle(bot))
        {
            // Its pet name is out: the next few lines go without one.
            const uint64_t key = botGuid.GetRawValue();
            if (TextRules::StripVocatives(plain, g_ResponsePetNames) != plain)
                g_petNameGap[key] = 0;
            else if (auto gap = g_petNameGap.find(key); gap != g_petNameGap.end() &&
                                                        ++gap->second + 1 >= g_ResponsePetNameEveryLines)
                g_petNameGap.erase(gap);
        }
        Links_NoteSent(line);
        if (c.request.offerEntry)   // "anyone want [Linen Cloth]?": out, so "me!" claims it
            Bags_NoteOffer(bot, c.request.offerEntry, c.request.scopeKey);
        else                        // "just have some [Ankh] if you want", asked what it has
            Bags_NoteReply(bot, plain, line.text, c.request.scopeKey, c.request.targetGuid);
        Governor_RecordUtterance(botGuid, c.request.scopeKey, plain, origin);
        Phrases_NoteSaid(plain);
        Phrases_NoteEcho(botGuid.GetRawValue(), plain);
        Context_NoteEmoticonSent(bot, plain);
        if (c.request.partyAck)
            PartyCmd_NoteAck(bot, plain);
        if (c.request.readyAsk)   // "everyone ready?" is out: a "yep" now answers it
            PartyCmd_NoteReadyAsked(bot);
        if (c.request.doneCall)   // "that's the dungeon cleared": the rest say gg
            PartyCmd_NoteDoneCalled(bot);
        if (c.request.breakEndCall)   // "right, lets go": off they go
            PartyCmd_NoteBreakEndCalled(bot);
        if (c.request.leadOffCall)    // "looks like everyones set, lets go": off they go
            PartyCmd_NoteLeadOffCalled(bot);
        if (c.request.progressKey)
            Progress_NoteDelivered(c.request.progressKey);
        if (c.request.showOffTold)   // "can you stop that": the show-off laughs it off
            CityLife_NoteToldOff(bot, c.request.showOffTold);
        awaited.out = true;          // an ask: its answers may follow
        if (c.request.noSelfDing)   // a "gz": the one it is for hears it, and thanks once for the lot
            Congrats_NoteBotCongrats(bot, c.request.congratsName, c.request.source,
                                     channel ? channel->GetName() : c.request.channelName, c.request.channelId,
                                     c.request.scopeKey);
        ++g_totalDelivered;

        // Into the shared transcript before other bots hear it below, so the
        // ones that answer can see what they are answering.
        const uint64_t seq = Context_RecordLine(bot, c.request.source,
                                                channel ? channel->GetName() : c.request.channelName,
                                                plain, ContextLineKind::Bot);

        // A gossip in the group may whisper a real player about it, now and then.
        if (c.request.source == SRC_PARTY_LOCAL || c.request.source == SRC_RAID_LOCAL)
            Aside_NotePartyLine(bot, plain, c.request.source);

        // ...and a conversation running here hears it too, before anyone
        // reacts to it below.
        if ((c.request.source == SRC_GENERAL_LOCAL || c.request.source == SRC_SAY_LOCAL) && !c.request.mingle)
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
            {
                Governor_NoteConversation(botGuid, ObjectGuid(c.request.targetGuid),
                                          c.request.scopeKey);
                // An invite that follows carries on from this (mod-ollama-chat_joined).
                Joined_NoteExchange(bot, addressee, c.request.source,
                                    channel ? channel->GetName() : c.request.channelName, origin, plain);
            }
        }

        // Body language. Safe here and only here: this is the world thread.
        // A passer-by gets nothing rude or gross with it.
        if (c.request.gentle && g_PresenceSkipEmotes.contains(c.emoteId))
            c.emoteId = 0;
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

    // Nothing to write for a bot in a dungeon run by bots alone: it would be
    // dropped at delivery (Deliver says why).
    if (!request.directAddress && request.source != SRC_WHISPER_LOCAL)
        if (Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(request.botGuid));
            bot && bot->IsInWorld() && OllamaInBotOnlyDungeon(bot))
            return false;

    // A line others wait on is on its way from now: noted before the worker
    // can have it, and gone again if it is not queued after all.
    const uint64_t says = request.saysKey;
    if (says)
        AwaitedSent(says);

    Task task;
    task.type    = TaskType::ChatReply;
    task.request = std::move(request);

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);

        if (!g_running)
        {
            if (says)
                AwaitedDone(says, false);
            return false;
        }

        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth)
        {
            ++g_droppedQueueFull;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Queue full ({}); dropping request for {}.",
                         g_queue.size(), task.request.botName);
            if (says)
                AwaitedDone(says, false);
            return false;
        }

        g_queue.push_back(std::move(task));
    }

    ++g_totalSubmitted;
    g_queueCv.notify_one();
    return true;
}

bool OllamaDispatch_DeliverNow(OllamaChatRequest request, uint32_t emoteId, const OllamaWorldSnapshot& world)
{
    if (request.fixedText.empty() || request.botGuid == 0)
        return false;

    Completion c;
    c.text      = request.fixedText;
    c.emoteId   = emoteId;
    c.deliverAt = Clock::now();
    c.request   = std::move(request);

    const uint64_t before = g_totalDelivered.load();
    try
    {
        Deliver(std::move(c), world);
    }
    catch (const std::exception& e)
    {
        RecordError(e.what());
        LOG_ERROR("module.ollamachat", "[Ollama Chat] Delivery exception: {}", e.what());
        return false;
    }
    return g_totalDelivered.load() > before;
}

std::vector<OllamaScriptResult> OllamaDispatch_TakeScripts()
{
    std::vector<OllamaScriptResult> out;
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    out.swap(g_scripts);
    return out;
}

uint32_t OllamaDispatch_QueueDepth()
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    return static_cast<uint32_t>(g_queue.size());
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

bool OllamaDispatch_SubmitScript(uint64_t scriptId, std::string prompt, uint32_t numPredict)
{
    if (scriptId == 0 || prompt.empty())
        return false;

    Task task;
    task.type             = TaskType::Script;
    task.scriptId         = scriptId;
    task.scriptPrompt     = std::move(prompt);
    task.scriptNumPredict = numPredict;

    // Nobody is owed one: with the queue half full it is not asked for at
    // all, as for memories.
    if (!SubmitBackground(std::move(task)))
        return false;
    ++g_totalSubmitted;
    g_queueCv.notify_one();
    return true;
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
        const auto now = std::chrono::steady_clock::now();
        if (!g_lastError.empty())
            stats.lastErrorAgoS = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now - g_lastErrorAt).count());
        for (auto it = g_errorTimes.rbegin(); it != g_errorTimes.rend() && now - *it <= std::chrono::minutes(10); ++it)
            ++stats.failedLast10Min;
    }

    stats.inFlight              = g_inFlight.load();
    stats.totalSubmitted        = g_totalSubmitted.load();
    stats.totalDelivered        = g_totalDelivered.load();
    stats.totalDroppedQueueFull = g_droppedQueueFull.load();
    stats.totalDroppedEmpty     = g_droppedEmpty.load();
    stats.totalDroppedGovernor  = g_droppedGovernor.load();
    stats.totalDroppedPassed    = g_droppedPassed.load();
    stats.totalDroppedPhrase    = g_droppedPhrase.load();
    stats.totalDroppedOvertaken = g_droppedOvertaken.load();
    stats.totalDroppedUnasked   = g_droppedUnasked.load();
    stats.totalRetried          = g_retried.load();
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
