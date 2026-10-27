#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_events.h"
#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_rag.h"
#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_mute.h"
#include "Log.h"

void Addmod_ollama_chatScripts()
{
    LOG_INFO("server.loading", "[Ollama Chat] Registering mod-ollama-chat scripts.");
    new OllamaChatConfigWorldScript();
    new PlayerBotChatHandler();
    new OllamaChatMaintenance();
    new OllamaBotRandomChatter();

    LOG_INFO("server.loading", "[Ollama Chat] Registering mod-ollama-chat events.");
    new ChatOnKill();
    new ChatOnLoot();
    new ChatOnDeath();
    new ChatOnQuest();
    new ChatOnLearn();
    new ChatOnDuel();
    new ChatOnLevelUp();
    new ChatOnAchievement();
    new ChatOnGameObjectUse();

    // Guild events. ChatOnGuildMemberChange used to exist but was never
    // registered here, and its hooks were not AzerothCore hooks in any case.
    new ChatOnGuild();
    new ChatOnGuildLogin();

    // Bots react when a player emotes at them.
    new ChatOnEmote();

    new OllamaChatConfigCommand();

    // Mages filling the trade window they offered as soon as it opens.
    new OllamaChatConjureScript();

    // Stock bot lines nobody needs: "Using [Hearthstone]", "AI was reset to defaults".
    new OllamaChatMuteScript();
}
