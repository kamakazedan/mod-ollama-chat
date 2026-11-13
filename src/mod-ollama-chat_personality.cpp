#include "mod-ollama-chat_personality.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "Log.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_textrules.h"
#include "DatabaseEnv.h"
#include <random>
#include <vector>

// The way a personality types a canned line (OllamaChat.Personality.CannedStyles).
std::string Personality_StyleCanned(Player* bot, const std::string& line)
{
    if (!bot || line.empty() || g_PersonalityCannedStyles.empty())
        return line;
    auto it = g_PersonalityCannedStyles.find(GetBotPersonality(bot));
    return it == g_PersonalityCannedStyles.end() ? line : TextRules::StyleCanned(line, it->second);
}

// Internal personality map. Any thread: the map is behind a lock, since a bot
// emoted at answers from the emote's map thread.
std::string GetBotPersonality(Player* bot)
{
    // RP personalities off: everyone is "default". The assignments are kept,
    // not overwritten, for when they are switched back on.
    if (!g_EnableRPPersonalities)
        return "default";

    const uint64_t botGuid = bot->GetGUID().GetRawValue();

    std::unique_lock<std::mutex> lock(g_BotPersonalityMutex);

    // Already assigned: earlier this run, or saved in the table and loaded.
    auto it = g_BotPersonalityList.find(botGuid);
    if (it != g_BotPersonalityList.end())
    {
        std::string found = it->second;
        lock.unlock();
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Using existing personality '{}' for bot {}", found, bot->GetName());
        }
        return found;
    }

    // None that may be handed out at random (config not loaded): "default",
    // and nothing kept, so a reload that adds some gives this bot one.
    if (g_PersonalityKeysRandomOnly.empty())
        return "default";

    // Otherwise, assign randomly from config (only from non-manual personalities)
    uint32 newIdx = urand(0, uint32(g_PersonalityKeysRandomOnly.size() - 1));
    std::string chosenPersonality = g_PersonalityKeysRandomOnly[newIdx];
    g_BotPersonalityList[botGuid] = chosenPersonality;
    lock.unlock();

    // Saved for next time, when the table is there (asked once, at load: this
    // used to ask the database again, and wait for it, for every new bot).
    if (!g_PersonalityTableExists)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Please source the required database table first");
    }
    else
    {
        CharacterDatabase.Execute("INSERT INTO mod_ollama_chat_personality (guid, personality) VALUES ({}, '{}')", botGuid, chosenPersonality);
    }

    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Assigned new personality '{}' to bot {}", chosenPersonality, bot->GetName());
    }
    return chosenPersonality;
}


std::string GetPersonalityPromptAddition(const std::string& personality)
{
    auto it = g_PersonalityPrompts.find(personality);
    if (it != g_PersonalityPrompts.end())
        return it->second;
    return g_DefaultPersonalityPrompt;
}

bool SetBotPersonality(Player* bot, const std::string& personality)
{
    if (!bot)
        return false;
    
    uint64_t botGuid = bot->GetGUID().GetRawValue();
    
    // Check if personality exists
    if (g_PersonalityPrompts.find(personality) == g_PersonalityPrompts.end() && personality != "default")
    {
        return false;
    }
    
    // Update in memory
    {
        std::lock_guard<std::mutex> lock(g_BotPersonalityMutex);
        g_BotPersonalityList[botGuid] = personality;
    }
    
    // Update in database
    CharacterDatabase.Execute("REPLACE INTO mod_ollama_chat_personality (guid, personality) VALUES ({}, '{}')", 
                             botGuid, personality);
    
    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Set personality '{}' for bot {}", personality, bot->GetName());
    }
    
    return true;
}

std::vector<std::string> GetAllPersonalityKeys()
{
    return g_PersonalityKeys;
}

bool PersonalityExists(const std::string& personality)
{
    if (personality == "default")
        return true;
    return g_PersonalityPrompts.find(personality) != g_PersonalityPrompts.end();
}

void ClearAllBotPersonalities()
{
    {
        std::lock_guard<std::mutex> lock(g_BotPersonalityMutex);
        g_BotPersonalityList.clear();
    }
    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Cleared all bot personality assignments due to RP personalities being disabled");
    }
}
