#ifndef MOD_OLLAMA_CHAT_ANSWER_H
#define MOD_OLLAMA_CHAT_ANSWER_H

#include "mod-ollama-chat_crowd.h"     // CrowdLineKind
#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// A bot answering a person's line about something it is doing for them -- the
// food it is trading over, the order it is following -- in its own voice,
// steered by `hint`.
//
// In party, raid and /say that is a one-voice crowd plan; when a plan is
// already answering the same line ("mb, water pls": the tank first), the bot
// is one more voice in it. A whisper is answered by whisper.
//
// `seq` is the line's transcript number. World thread only.
// --------------------------------------------------------------------------
void OllamaBotAnswer(Player* player, Player* bot, const std::string& msg, ChatChannelSourceLocal source,
                     const std::string& scopeKey, uint64_t seq, std::string hint, CrowdLineKind kind);

#endif // MOD_OLLAMA_CHAT_ANSWER_H
