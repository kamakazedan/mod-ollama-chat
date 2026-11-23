#ifndef MOD_OLLAMA_CHAT_SAID_H
#define MOD_OLLAMA_CHAT_SAID_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class ObjectGuid;
class Player;

// --------------------------------------------------------------------------
// What a bot said lately, and what it had to go on (OllamaChat.Said.*).
//
// A bot in Duskwood said "still havent seen that kodo drop yet" -- it had
// been handed the Great Brewfest Kodo, from Coren Direbrew in Blackrock Depths
// during Brewfest, to talk about -- and asked "kodo? where is it supposed to
// drop?" answered "no idea": nothing of what it had been told was left. A
// player asking about something a bot said has to get the real answer, from
// what it said it on.
//
// So every line a bot says is kept a while: where, to whom, and what it went
// on, if anything -- a topic, a premise, a thing it asked for, something it
// remarked on, the facts a reply was given (OllamaChatRequest::basis). Its
// replies are given the last few that had something behind them: what it
// said, where, and what it went on. Asked about it, it answers from that,
// and makes up nothing more.
//
// And asked back about one -- "lol who?", "what leggings?", "what do you
// mean?" -- it is told which line they mean and to say exactly who or what
// it meant (Said.AskedText). A bot whispered "those leggings she's rocking
// are a bit much arent they?", was asked "lol who?", and answered "the one
// in the group with the weird gear".
//
// A line said to someone alone (a whisper) is told only to them, and one said
// in a group's chat only to that group: an aside is not repeated to the one
// it was about.
//
// World thread only.
// --------------------------------------------------------------------------

// Where a line was said, as these lines name it: "a whisper", "party chat",
// "General", "/say nearby".
std::string Said_Place(ChatChannelSourceLocal source, const std::string& channelName);

// `bot` just said `line` in `place` (Said_Place), going on `basis` (empty:
// nothing in particular). `to`: whom it was said to alone (a whisper), else 0;
// `group`: the group whose chat it was said in (party or raid), else 0.
void Said_Note(Player* bot, const std::string& line, const std::string& place, const std::string& basis,
               uint64_t to, uint32_t group);

// For a reply prompt of `bot` to `asker`, who said `message` in `place`:
// what it said lately where `asker` heard it, and what it went on
// (Said.Text); and when `message` asks back about one of those lines, which
// one and that it is to say exactly who or what it meant (Said.AskedText).
// `askedBasis` gets what that line went on (for the reply's own basis).
// Empty when there is nothing to tell.
std::string Said_PromptFor(Player* bot, Player* asker, const std::string& message, const std::string& place,
                           std::string* askedBasis = nullptr);

void Said_Forget(ObjectGuid botGuid);   // logged out
void Said_Update();                     // maintenance: old lines forgotten

struct SaidStats
{
    uint64_t lines     = 0;   // lines kept
    uint64_t noted     = 0;   // ...with what they were said on
    uint64_t told      = 0;   // reply prompts given them
    uint64_t askedBack = 0;   // replies to someone asking back about a line ("lol who?")
    uint32_t bots      = 0;   // bots with lines kept now
};
SaidStats Said_GetStats();

#endif // MOD_OLLAMA_CHAT_SAID_H
