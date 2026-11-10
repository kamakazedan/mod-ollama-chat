#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_conversation.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_phrases.h"
#include "mod-ollama-chat_presence.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_equip.h"
#include "mod-ollama-chat_congrats.h"
#include "mod-ollama-chat_joined.h"
#include "mod-ollama-chat_marks.h"
#include "mod-ollama-chat_bags.h"
#include "mod-ollama-chat_needs.h"
#include "mod-ollama-chat_orders.h"
#include "mod-ollama-chat_premises.h"
#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat_stuck.h"
#include "mod-ollama-chat_gifts.h"
#include "mod-ollama-chat_wipe.h"
#include "mod-ollama-chat_gear.h"
#include "mod-ollama-chat_aside.h"
#include "mod-ollama-chat_knowledge.h"
#include "mod-ollama-chat_wants.h"
#include "mod-ollama-chat_mingle.h"
#include "mod-ollama-chat_rolls.h"
#include "mod-ollama-chat_market.h"
#include "mod-ollama-chat_plus.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include <thread>
#include "Chat.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include <fmt/core.h>

using namespace Acore::ChatCommands;

OllamaChatConfigCommand::OllamaChatConfigCommand()
    : CommandScript("OllamaChatConfigCommand")
{
}

ChatCommandTable OllamaChatConfigCommand::GetCommands() const
{
    static ChatCommandTable ollamaSentimentCommandTable =
    {
        { "view",  HandleOllamaSentimentViewCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "set",   HandleOllamaSentimentSetCommand,   SEC_ADMINISTRATOR, Console::Yes },
        { "reset", HandleOllamaSentimentResetCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaPersonalityCommandTable =
    {
        { "get",  HandleOllamaPersonalityGetCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "set",  HandleOllamaPersonalitySetCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "list", HandleOllamaPersonalityListCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaReloadCommandTable =
    {
        { "reload",      HandleOllamaReloadCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "status",      HandleOllamaStatusCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "test",        HandleOllamaTestCommand,    SEC_ADMINISTRATOR, Console::Yes },
        { "sentiment",   ollamaSentimentCommandTable },
        { "personality", ollamaPersonalityCommandTable }
    };

    static ChatCommandTable commandTable =
    {
        { "ollama", ollamaReloadCommandTable }
    };

    return commandTable;
}

bool OllamaChatConfigCommand::HandleOllamaReloadCommand(ChatHandler* handler)
{
    sConfigMgr->Reload();
    LoadOllamaChatConfig();
    Roleplay_Load();

    // Re-probe: the operator may have just pointed the module at a different
    // model, and think-mode support is per-model.
    OllamaCapability_Init(true);

    // Clear personality assignments if RP personalities are disabled
    // This ensures that when re-enabled later, bots get fresh random assignments
    if (!g_EnableRPPersonalities)
    {
        ClearAllBotPersonalities();
    }

    LoadBotPersonalityList();
    LoadBotConversationHistoryFromDB();
    InitializeSentimentTracking();
    handler->SendSysMessage("OllamaChat: Configuration reloaded from conf!");
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentViewCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat: Sentiment tracking is disabled.");
        return true;
    }

    if (!botName && !playerName)
    {
        // Show all sentiment data
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        if (g_BotPlayerSentiments.empty())
        {
            handler->SendSysMessage("OllamaChat: No sentiment data found.");
            return true;
        }

        handler->SendSysMessage("OllamaChat: All sentiment data:");
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
            std::string botNameStr = bot ? bot->GetName() : std::to_string(botGuid);
            
            for (const auto& [playerGuid, sentiment] : playerMap)
            {
                Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
                std::string playerNameStr = player ? player->GetName() : std::to_string(playerGuid);
                
                handler->SendSysMessage(fmt::format("  Bot '{}' -> Player '{}': {:.3f}", 
                                        botNameStr, playerNameStr, sentiment));
            }
        }
        return true;
    }

    // Find specific bot or player
    Player* targetBot = nullptr;
    Player* targetPlayer = nullptr;

    if (botName)
    {
        targetBot = ObjectAccessor::FindPlayerByName(*botName);
        if (!targetBot)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", *botName));
            return true;
        }
        if (!PlayerbotsMgr::instance().GetPlayerbotAI(targetBot))
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", *botName));
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", *playerName));
            return true;
        }
    }

    // Show sentiment for specific bot-player pair or all pairs involving a specific bot/player
    if (targetBot && targetPlayer)
    {
        float sentiment = GetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue());
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' -> Player '{}': {:.3f}", 
                                targetBot->GetName(), targetPlayer->GetName(), sentiment));
    }
    else if (targetBot)
    {
        // Show all sentiments for this bot
        uint64_t botGuid = targetBot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        auto botIt = g_BotPlayerSentiments.find(botGuid);
        if (botIt == g_BotPlayerSentiments.end() || botIt->second.empty())
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found for bot '{}'.", targetBot->GetName()));
            return true;
        }

        handler->SendSysMessage(fmt::format("OllamaChat: Sentiment data for bot '{}':", targetBot->GetName()));
        for (const auto& [playerGuid, sentiment] : botIt->second)
        {
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
            std::string playerNameStr = player ? player->GetName() : std::to_string(playerGuid);
            handler->SendSysMessage(fmt::format("  -> Player '{}': {:.3f}", playerNameStr, sentiment));
        }
    }
    else if (targetPlayer)
    {
        // Show all sentiments involving this player
        uint64_t playerGuid = targetPlayer->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        bool found = false;
        handler->SendSysMessage(fmt::format("OllamaChat: Sentiment data involving player '{}':", targetPlayer->GetName()));
        
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            auto playerIt = playerMap.find(playerGuid);
            if (playerIt != playerMap.end())
            {
                Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                std::string botNameStr = bot ? bot->GetName() : std::to_string(botGuid);
                handler->SendSysMessage(fmt::format("  Bot '{}' -> {:.3f}", botNameStr, playerIt->second));
                found = true;
            }
        }
        
        if (!found)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found involving player '{}'.", targetPlayer->GetName()));
        }
    }

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentSetCommand(ChatHandler* handler, std::string botName, std::string playerName, float sentimentValue)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat: Sentiment tracking is disabled.");
        return true;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }

    Player* player = ObjectAccessor::FindPlayerByName(playerName);
    if (!player)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", playerName));
        return true;
    }

    if (sentimentValue < 0.0f || sentimentValue > 1.0f)
    {
        handler->SendSysMessage("OllamaChat: Sentiment value must be between 0.0 and 1.0.");
        return true;
    }

    SetBotPlayerSentiment(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(), sentimentValue);
    handler->SendSysMessage(fmt::format("OllamaChat: Set sentiment between bot '{}' and player '{}' to {:.3f}.", 
                            botName, playerName, sentimentValue));
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentResetCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat: Sentiment tracking is disabled.");
        return true;
    }

    if (!botName && !playerName)
    {
        // Reset all sentiment data
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        uint32_t count = 0;
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            count += playerMap.size();
        }
        g_BotPlayerSentiments.clear();
        g_DirtySentiments.clear();
        CharacterDatabase.Execute("DELETE FROM mod_ollama_chat_bot_player_sentiments");
        handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data ({} records).", count));
        return true;
    }

    Player* targetBot = nullptr;
    Player* targetPlayer = nullptr;

    if (botName)
    {
        targetBot = ObjectAccessor::FindPlayerByName(*botName);
        if (!targetBot)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", *botName));
            return true;
        }
        if (!PlayerbotsMgr::instance().GetPlayerbotAI(targetBot))
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", *botName));
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", *playerName));
            return true;
        }
    }

    if (targetBot && targetPlayer)
    {
        // Reset specific bot-player sentiment
        SetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue(), g_SentimentDefaultValue);
        handler->SendSysMessage(fmt::format("OllamaChat: Reset sentiment between bot '{}' and player '{}' to default ({:.3f}).", 
                                targetBot->GetName(), targetPlayer->GetName(), g_SentimentDefaultValue));
    }
    else if (targetBot)
    {
        // Reset all sentiments for this bot
        uint64_t botGuid = targetBot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        auto botIt = g_BotPlayerSentiments.find(botGuid);
        if (botIt != g_BotPlayerSentiments.end())
        {
            uint32_t count = botIt->second.size();
            g_BotPlayerSentiments.erase(botIt);

            for (auto it = g_DirtySentiments.begin(); it != g_DirtySentiments.end(); )
            {
                if (it->first == botGuid)
                    it = g_DirtySentiments.erase(it);
                else
                    ++it;
            }

            CharacterDatabase.Execute(SafeFormat(
                "DELETE FROM mod_ollama_chat_bot_player_sentiments WHERE bot_guid = {}", botGuid));
            handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data for bot '{}' ({} records).", 
                                    targetBot->GetName(), count));
        }
        else
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found for bot '{}'.", targetBot->GetName()));
        }
    }
    else if (targetPlayer)
    {
        // Reset all sentiments involving this player
        uint64_t playerGuid = targetPlayer->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        uint32_t count = 0;
        for (auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            auto playerIt = playerMap.find(playerGuid);
            if (playerIt != playerMap.end())
            {
                playerMap.erase(playerIt);
                count++;
            }
        }

        for (auto it = g_DirtySentiments.begin(); it != g_DirtySentiments.end(); )
        {
            if (it->second == playerGuid)
                it = g_DirtySentiments.erase(it);
            else
                ++it;
        }

        CharacterDatabase.Execute(SafeFormat(
            "DELETE FROM mod_ollama_chat_bot_player_sentiments WHERE player_guid = {}", playerGuid));

        handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data involving player '{}' ({} records).", 
                                targetPlayer->GetName(), count));
    }

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalityGetCommand(ChatHandler* handler, std::string botName)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }
    
    std::string personality = GetBotPersonality(bot);
    std::string prompt = GetPersonalityPromptAddition(personality);
    
    handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' has personality '{}'", botName, personality));
    handler->SendSysMessage(fmt::format("  Prompt: {}", prompt));
    
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalitySetCommand(ChatHandler* handler, std::string botName, std::string personality)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }
    
    if (!PersonalityExists(personality))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Personality '{}' does not exist. Use '.ollama personality list' to see available personalities.", personality));
        return true;
    }
    
    if (SetBotPersonality(bot, personality))
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        handler->SendSysMessage(fmt::format("OllamaChat: Set bot '{}' personality to '{}'", botName, personality));
        handler->SendSysMessage(fmt::format("  Prompt: {}", prompt));
    }
    else
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Failed to set personality for bot '{}'.", botName));
    }
    
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalityListCommand(ChatHandler* handler)
{
    std::vector<std::string> personalities = GetAllPersonalityKeys();
    
    if (personalities.empty())
    {
        handler->SendSysMessage("OllamaChat: No personalities loaded.");
        return true;
    }
    
    handler->SendSysMessage(fmt::format("OllamaChat: Available personalities ({} total, {} random-assignable):", 
                            personalities.size(), g_PersonalityKeysRandomOnly.size()));
    
    for (const auto& personality : personalities)
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        
        // Check if this personality is manual-only
        bool isManualOnly = (std::find(g_PersonalityKeysRandomOnly.begin(), g_PersonalityKeysRandomOnly.end(), personality) 
                            == g_PersonalityKeysRandomOnly.end());
        
        std::string manualTag = isManualOnly ? " [MANUAL ONLY]" : "";
        
        handler->SendSysMessage(fmt::format("  - {}{}", personality, manualTag));
        handler->SendSysMessage(fmt::format("    {}", prompt));
    }
    
    return true;
}


// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// The few lines worth seeing every time: what it talks to, whether it is
// keeping up, what is going on right now, and the last thing that went wrong.

// Hangouts, stuck mounts, getting back up, raid marks and muted lines are
// mod-playerbots-plus's: where to look for them.
static const char* PlusStatus()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 2
    return ".pbp status";
#else
    return "need mod-playerbots-plus, which is not in this build";
#endif
}

static bool StatusSummary(ChatHandler* handler)
{
    const OllamaDispatchStats dispatch = OllamaDispatch_GetStats();
    const GovernorStats       gov      = Governor_GetStats();
    const CrowdStats          crowd    = Crowd_GetStats();
    const ConversationStats   conv     = Conversation_GetStats();
    const MingleStats         mingle   = Mingle_GetStats();

    handler->PSendSysMessage("|cff00ff00[Ollama Chat]|r {} - {} ({}) at {}",
                             g_Enable ? "on" : "|cffff0000OFF|r", g_OllamaModel,
                             OllamaProvider_Name(g_OllamaProvider), g_OllamaUrl);
    handler->PSendSysMessage("Requests: {} waiting, {} being written by {} workers; {} asked for, {} said, {} failed",
                             dispatch.queuedRequests, dispatch.inFlight, dispatch.workers,
                             (unsigned long long)dispatch.totalSubmitted,
                             (unsigned long long)dispatch.totalDelivered,
                             (unsigned long long)dispatch.totalFailed);
    handler->PSendSysMessage("Held back: {} by pacing, {} by the bot, {} stock phrases; {} lines in the last minute",
                             (unsigned long long)dispatch.totalDroppedGovernor,
                             (unsigned long long)dispatch.totalDroppedPassed,
                             (unsigned long long)dispatch.totalDroppedPhrase, gov.sendsLastMinute);
    handler->PSendSysMessage("Right now: {} crowd replies, {} conversations, {} bot chats in /say",
                             crowd.active, conv.active, mingle.running);
    if (!dispatch.lastError.empty())
        handler->PSendSysMessage("|cffff0000Last error:|r {}", dispatch.lastError);
    else
        handler->PSendSysMessage("Last error: none");
    handler->PSendSysMessage("More: .ollama status chat | city | party | bots | all   Hangouts, marks, the auction "
                             "house: {}", PlusStatus());
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaStatusCommand(ChatHandler* handler, Optional<std::string> section)
{
    // ".ollama status" is a few lines; ".ollama status chat|city|party|bots|all"
    // the counts behind them. Every line at once filled the chat window.
    std::string want = section ? *section : std::string();
    for (char& c : want)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (want.empty())
        return StatusSummary(handler);
    if (want != "chat" && want != "city" && want != "party" && want != "bots" && want != "all")
    {
        handler->SendSysMessage("Usage: .ollama status [chat|city|party|bots|all]");
        handler->SetSentErrorMessage(true);
        return false;
    }
    auto show = [&want](const char* name) { return want == "all" || want == name; };
    const OllamaDispatchStats dispatch = OllamaDispatch_GetStats();
    const GovernorStats       gov      = Governor_GetStats();

    handler->PSendSysMessage("|cff00ff00[Ollama Chat] Status: {}|r", want);
    // The API key is deliberately not shown here.
    if (show("chat"))
        handler->PSendSysMessage("Module: {}   Provider: {}   Endpoint: {}   Model: {}   ApiKey: {}   HTTPS: {}",
                             g_Enable ? "enabled" : "DISABLED",
                             OllamaProvider_Name(g_OllamaProvider),
                             g_OllamaUrl, g_OllamaModel,
                             g_OllamaApiKey.empty() ? "not set" : "set",
                             OllamaHttp_TlsAvailable() ? "available" : "NOT BUILT (no OpenSSL)");
    if (show("chat"))
        handler->PSendSysMessage("Think: {}", OllamaCapability_StatusText());

    if (show("chat"))
        handler->PSendSysMessage("Dispatcher: {} workers, {} queued, {} in flight, {} awaiting delivery",
                             dispatch.workers, dispatch.queuedRequests,
                             dispatch.inFlight, dispatch.pendingDeliveries);
    if (show("chat"))
        handler->PSendSysMessage("Totals: {} submitted, {} delivered, {} failed",
                             (unsigned long long)dispatch.totalSubmitted,
                             (unsigned long long)dispatch.totalDelivered,
                             (unsigned long long)dispatch.totalFailed);
    if (show("chat"))
        handler->PSendSysMessage("Dropped: {} queue-full, {} empty-after-cleanup, {} by governor, {} passed by the bot, "
                             "{} stock or off-era phrases",
                             (unsigned long long)dispatch.totalDroppedQueueFull,
                             (unsigned long long)dispatch.totalDroppedEmpty,
                             (unsigned long long)dispatch.totalDroppedGovernor,
                             (unsigned long long)dispatch.totalDroppedPassed,
                             (unsigned long long)dispatch.totalDroppedPhrase);

    if (show("chat"))
        handler->PSendSysMessage("Governor: {} bots, {} scopes tracked, {} sends in the last minute",
                             gov.trackedBots, gov.trackedScopes, gov.sendsLastMinute);
    if (show("chat"))
        handler->PSendSysMessage("Blocked: {} cooldown, {} rate, {} repetition, {} chain-depth, {} no-audience",
                             gov.blockedCooldown, gov.blockedRate, gov.blockedRepetition,
                             gov.blockedChainDepth, gov.blockedNoAudience);

    const CrowdStats crowd = Crowd_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Crowds: {} ({} running, {} started) - {} voices asked, {} spoke ({} late), "
                             "{} said nothing, {} skipped, {} answer-backs",
                             (g_CrowdEnable && g_ContextEnable) ? "on" : "off", crowd.active,
                             (unsigned long long)crowd.plansStarted,
                             (unsigned long long)crowd.slotsFired,
                             (unsigned long long)crowd.slotsLanded,
                             (unsigned long long)crowd.slotsLate,
                             (unsigned long long)crowd.slotsDropped,
                             (unsigned long long)crowd.slotsSkipped,
                             (unsigned long long)crowd.replyBacks);

    const ConversationStats conv = Conversation_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Conversations: {} ({} running) - {} started ({} by players, {} by bots), "
                             "{} turns, {} lines, {} joined, {} wound down, {} fizzled",
                             (g_ConversationEnable && g_CrowdEnable && g_ContextEnable) ? "on" : "off",
                             conv.active,
                             (unsigned long long)conv.started,
                             (unsigned long long)conv.byPlayers,
                             (unsigned long long)conv.byBots,
                             (unsigned long long)conv.turns,
                             (unsigned long long)conv.lines,
                             (unsigned long long)conv.joins,
                             (unsigned long long)conv.woundDown,
                             (unsigned long long)conv.fizzled);

    const PresenceStats presence = Presence_GetStats();
    if (show("city"))
        handler->PSendSysMessage("Cities: {} (chatter x{})   Presence: {} - {} gestures ({} at pets), {} remarks",
                             g_CityEnable ? "on" : "off", g_CityChatterMultiplier,
                             g_PresenceEnable ? "on" : "off",
                             (unsigned long long)presence.emotes,
                             (unsigned long long)presence.petEmotes,
                             (unsigned long long)presence.remarks);

    const PartyCmdStats party = PartyCmd_GetStats();
    if (show("party"))
        handler->PSendSysMessage("Party instructions: {} (dungeon clear {}) - {} started, {} paused ({} rests, "
                             "{} the tank called over itself), {} resumed, "
                             "{} waited for players, {} refused; {} runs called done, {} gg, {} bots left after, "
                             "{} groups asked to stay",
                             g_PartyCmdEnable ? "on" : "off",
                             PartyCmd_Available() ? "found" : "not in this build",
                             (unsigned long long)party.started,
                             (unsigned long long)party.paused,
                             (unsigned long long)party.breaks,
                             (unsigned long long)party.breaksEnded,
                             (unsigned long long)party.resumed,
                             (unsigned long long)party.waiting,
                             (unsigned long long)party.refused,
                             (unsigned long long)party.done,
                             (unsigned long long)party.cheers,
                             (unsigned long long)party.left,
                             (unsigned long long)party.stayed);

    const ConjureStats conjure = Conjure_GetStats();
    if (show("bots"))
        handler->PSendSysMessage("Conjured food and water: {} - {} asked, {} trades offered, {} traded, {} waited "
                             "first, {} refused, {} lapsed, {} pending",
                             g_ConjureEnable ? "on" : "off",
                             (unsigned long long)conjure.asked,
                             (unsigned long long)conjure.offered,
                             (unsigned long long)conjure.completed,
                             (unsigned long long)conjure.waited,
                             (unsigned long long)conjure.refused,
                             (unsigned long long)conjure.lapsed,
                             conjure.pending);

    const OrdersStats orders = Orders_GetStats();
    if (show("bots"))
        handler->PSendSysMessage("Orders: {} - {} given, {} commands sent, {} playerbots had already, {} put right "
                             "after playerbots read them otherwise, {} could not, {} trades offered, {} unstuck, "
                             "{} playerbots commands left alone",
                             g_OrdersEnable ? "on" : "off",
                             (unsigned long long)orders.orders,
                             (unsigned long long)orders.sent,
                             (unsigned long long)orders.already,
                             (unsigned long long)orders.redone,
                             (unsigned long long)orders.refused,
                             (unsigned long long)orders.trades,
                             (unsigned long long)orders.unstuck,
                             (unsigned long long)orders.raw);

    const MarksStats marks = Marks_GetStats();
    if (show("party"))
        handler->PSendSysMessage("Saps that could not be done: {}, the rogue said why {} times ({})   Raid marks, "
                             "getting back up, muted lines: {}",
                             (unsigned long long)marks.heard,
                             (unsigned long long)marks.explained,
                             g_MarksSayEnable ? "on" : "off",
                             PlusStatus());

    const BagsStats  bags  = Bags_GetStats();
    const NeedsStats needs = Needs_GetStats();
    if (show("bots"))
        handler->PSendSysMessage("Bags: {} - told what they carry {} times, {} asks for something ({} traded over, {} "
                             "kept, {} told to invite first), {} spare things offered",
                             g_BagsEnable ? "on" : "off",
                             (unsigned long long)bags.described,
                             (unsigned long long)bags.asked,
                             (unsigned long long)bags.given,
                             (unsigned long long)bags.kept,
                             (unsigned long long)bags.invite,
                             (unsigned long long)bags.offers);
    const JoinedStats joined = Joined_GetStats();
    if (show("party"))
        handler->PSendSysMessage("Joining a group: {} - {} joins with a real player, {} said hi, {} carried on a "
                             "conversation, {} said nothing",
                             g_JoinedEnable ? "on" : "off",
                             (unsigned long long)joined.joins,
                             (unsigned long long)joined.greeted,
                             (unsigned long long)joined.carriedOn,
                             (unsigned long long)joined.quiet);
    const CongratsStats cheers = Congrats_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Congratulations: {} - {} dings noted ({} achievements too common for a gz, {} "
                             "dings guild chat was not told of), {} gz answered only by the one they were for, {} "
                             "thank-yous, {} more gz after it ({} for nobody in particular)",
                             g_CongratsEnable ? "on" : "off",
                             (unsigned long long)cheers.celebrated,
                             (unsigned long long)cheers.common,
                             (unsigned long long)cheers.guildQuiet,
                             (unsigned long long)cheers.heard,
                             (unsigned long long)cheers.thanked,
                             (unsigned long long)cheers.repeat,
                             (unsigned long long)cheers.nobody);
    if (show("chat"))
        handler->PSendSysMessage("Asks with a reason: {} ({}%) - {} quest groups, {} errands, {} spare things, {} for a "
                             "few silver; {} tries found nothing; {} replies told what it is really doing",
                             g_NeedsEnable ? "on" : "off", g_NeedsChance,
                             (unsigned long long)needs.asked[size_t(NeedKind::Quest)],
                             (unsigned long long)needs.asked[size_t(NeedKind::Errand)],
                             (unsigned long long)needs.asked[size_t(NeedKind::Spare)],
                             (unsigned long long)needs.asked[size_t(NeedKind::Short)],
                             (unsigned long long)needs.none,
                             (unsigned long long)needs.plans);

    const LinksStats   links   = Links_GetStats();
    const PhrasesStats phrases = Phrases_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Level outlook: {} ({} bands)   Links: {} - {} linked, {} not found, {} remembered{}",
                             g_OutlookEnable ? "on" : "off", Outlook_BandCount(),
                             g_LinksEnable ? "on" : "off",
                             (unsigned long long)links.linked,
                             (unsigned long long)links.unresolved,
                             links.remembered, links.indexed ? ", names indexed" : "");
    if (show("chat"))
        handler->PSendSysMessage("Stock phrases: {} dropped as said lately, {} as a hobby horse said lately, {} as "
                             "not in Wrath, {} as not at the bot's level, {} as breaking the fourth wall ({} winks "
                             "at it let through), {} cooling down",
                             (unsigned long long)phrases.droppedOverused,
                             (unsigned long long)phrases.droppedRare,
                             (unsigned long long)phrases.droppedOffEra,
                             (unsigned long long)phrases.droppedOffLevel,
                             (unsigned long long)phrases.droppedFourthWall,
                             (unsigned long long)phrases.fourthWallWinks,
                             phrases.coolingDown);
    const PremiseStats premises = Premises_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Echoes: {} - {} lines echoed other bots, {} phrases counted; {} owed lines asked for "
                             "again   Premises: {} fixed, {} from templates ({} templates, {} pools), {} times none fitted",
                             g_EchoEnable ? "on" : "off",
                             (unsigned long long)phrases.echoes, phrases.phrasesTracked,
                             (unsigned long long)dispatch.totalRetried,
                             (unsigned long long)premises.fixed,
                             (unsigned long long)premises.templated,
                             g_PremiseTemplates.size(), g_PremisePools.size(),
                             (unsigned long long)premises.none);
    const StuckStats    stuck    = Stuck_GetStats();
    const ProgressStats progress = Progress_GetStats();
    if (show("bots"))
        handler->PSendSysMessage("Stuck on a mount: {} bots taken off what they were stuck on, {} said so   The "
                             "rest: {}",
                             (unsigned long long)stuck.heard,
                             (unsigned long long)stuck.remarks,
                             PlusStatus());
    if (show("party"))
        handler->PSendSysMessage("Quest progress: {} - {} objectives and {} quests finished by grouped bots, {} said in "
                             "party, {} left to whoever said it first, {} let go",
                             g_ProgressEnable ? "on" : "off",
                             (unsigned long long)progress.objectives,
                             (unsigned long long)progress.quests,
                             (unsigned long long)progress.said,
                             (unsigned long long)progress.shared,
                             (unsigned long long)progress.lapsed);
    const EquipStats equip = Equip_GetStats();
    if (show("party"))
        handler->PSendSysMessage("New gear: {} - {} rares or epics put on, {} by a grouped bot that just got it, {} "
                             "said so in party",
                             g_EquipEnable ? "on" : "off",
                             (unsigned long long)equip.noted,
                             (unsigned long long)equip.fresh,
                             (unsigned long long)equip.said);
    if (show("city"))
        handler->PSendSysMessage("City hangouts: {}", PlusStatus());
    const GiftsStats gifts = Gifts_GetStats();
    if (show("bots"))
        handler->PSendSysMessage("Things given away and sold: {} - {} given away, {} sold; {} said yes please, {} nobody "
                             "could use; {} asked the price, {} agreed one, {} offered less, {} paid ({} agreed and "
                             "not traded yet)",
                             g_GiftsEnable ? "on" : "off",
                             (unsigned long long)gifts.offers,
                             (unsigned long long)gifts.sales,
                             (unsigned long long)gifts.wanted,
                             (unsigned long long)gifts.nobody,
                             (unsigned long long)gifts.asked,
                             (unsigned long long)gifts.agreed,
                             (unsigned long long)gifts.countered,
                             (unsigned long long)gifts.paid,
                             gifts.deals);
    const WipeStats wipe = Wipe_GetStats();
    if (show("party"))
        handler->PSendSysMessage("Deaths in groups: {} - {} uh-ohs for the tank, {} wipes ({} sorries from the tank, {} "
                             "calls to run back), {} running back on their own",
                             g_WipeEnable ? "on" : "off",
                             (unsigned long long)wipe.tankDown,
                             (unsigned long long)wipe.wipes,
                             (unsigned long long)wipe.apologies,
                             (unsigned long long)wipe.noRez,
                             (unsigned long long)wipe.runBack);
    const GearStats gear = Gear_GetStats();
    const AsideStats aside = Aside_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Gear talk: {} - {} prompts told what people wear, {} players who felt picked on   "
                             "Asides: {} - {} whispered ({} about something said, {} about something else)",
                             g_GearEnable ? "on" : "off",
                             (unsigned long long)gear.described,
                             (unsigned long long)gear.backOffs,
                             g_AsideEnable ? "on" : "off",
                             (unsigned long long)aside.whispered,
                             (unsigned long long)aside.onLines,
                             (unsigned long long)aside.idle);
    const KnowledgeStats known = Knowledge_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Knowledge: {} - answers put in prompts: {} how to get an item, {} where a place is, {} "
                             "ask a guard for any trainer, {} a question just before ({} told to get it wrong on "
                             "purpose); {} guard direction menus, {} items sold by vendors (0 until the first "
                             "question)",
                             g_KnowledgeEnable ? "on" : "off",
                             (unsigned long long)known.items,
                             (unsigned long long)known.places,
                             (unsigned long long)known.headings,
                             (unsigned long long)known.followUps,
                             (unsigned long long)known.wrong,
                             known.menus,
                             known.sold);
    const WantsStats wants = Wants_GetStats();
    if (show("chat"))
        handler->PSendSysMessage("Wanted gear: {} - {} lines saying someone is after a kind of item, {} reply prompts "
                             "told where one comes from ({} follow-ups); {} bosses in {} dungeons with gear known (0 "
                             "until the first such line)",
                             g_WantsEnable ? "on" : "off",
                             (unsigned long long)wants.heard,
                             (unsigned long long)wants.prompts,
                             (unsigned long long)wants.followUps,
                             wants.bosses, wants.dungeons);
    const RollsStats rolls = Rolls_GetStats();
    if (show("party"))
        handler->PSendSysMessage("Loot rolls: {} - {} put in a group's chat, {} very low or high ones remarked on",
                             g_RollsEnable ? "on" : "off",
                             (unsigned long long)rolls.noted,
                             (unsigned long long)rolls.reactions);
    const MingleStats mingle = Mingle_GetStats();
    if (show("city"))
        handler->PSendSysMessage("Mingling: {} - {} running, {} asked for, {} written ({} unusable), {} ran to the end, "
                             "{} cut short, {} lines, {} emotes at each other; {} greetings between bots; {} pairs "
                             "talked themselves into a duel and went out to the gates",
                             g_MingleEnable ? "on" : "off", mingle.running,
                             (unsigned long long)mingle.asked,
                             (unsigned long long)mingle.written,
                             (unsigned long long)mingle.unusable,
                             (unsigned long long)mingle.finished,
                             (unsigned long long)mingle.cut,
                             (unsigned long long)mingle.lines,
                             (unsigned long long)mingle.emotes,
                             (unsigned long long)mingle.greetings,
                             (unsigned long long)mingle.duels);

    if (show("chat"))
        handler->PSendSysMessage("Roleplay: {} (strictness {})   Emote reactions: {}",
                             g_RoleplayEnable ? "on" : "off",
                             (uint32)g_RoleplayStrictness,
                             g_EnableEmoteReactions ? "on" : "off");
    if (show("chat"))
        handler->PSendSysMessage("Topic weights: people {} / world {} / activity {} / self {} / guild {} / the game {} / the auction house {}{}",
                             g_TopicWeightPeople, g_TopicWeightWorld, g_TopicWeightActivity,
                             g_TopicWeightSelf, g_TopicWeightGuild, g_TopicWeightTalk, g_TopicWeightMarket,
                             OllamaMarket_Available() ? fmt::format(" ({} dealings heard of)", OllamaMarket_Heard())
                                                      : std::string(" (no mod-playerbots-plus)"));

    if (!dispatch.lastError.empty())
        handler->PSendSysMessage("|cffff0000Last error:|r {}", dispatch.lastError);
    else
        handler->PSendSysMessage("Last error: none");

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaTestCommand(ChatHandler* handler, Acore::ChatCommands::Tail prompt)
{
    std::string text(prompt);
    if (text.empty())
    {
        handler->SendSysMessage("Usage: .ollama test <prompt>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("[Ollama Chat] Sending test prompt, please wait...");

    // Blocking HTTP must not run on the world thread, so do the round trip on
    // a scratch thread and report from there. Turns "the bots are quiet" into
    // a one-command diagnosis: you see the raw output and the cleaned output
    // side by side.
    std::thread([text]()
    {
        OllamaApiResult api = QueryOllama(text, OllamaRequestKind::ChatReply);

        if (!api.ok)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST FAILED after {}ms: {}",
                     api.latencyMs, api.error.empty() ? "unknown error" : api.error);
            return;
        }

        uint32_t emote = 0;
        const std::string cleaned = ProcessLlmResponse(api.text, "Tester", &emote);

        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST ok in {}ms (think={}).",
                 api.latencyMs, api.thinkUsed ? "yes" : "no");
        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST raw     : {}", api.text);
        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST cleaned : {}", cleaned);
        if (emote)
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST emote   : {}", emote);
        if (!api.thinking.empty())
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST thinking: {}", api.thinking);
    }).detach();

    handler->PSendSysMessage("[Ollama Chat] Result will appear in the server log (module.ollamachat).");
    return true;
}
