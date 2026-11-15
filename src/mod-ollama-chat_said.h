#ifndef MOD_OLLAMA_CHAT_SAID_H
#define MOD_OLLAMA_CHAT_SAID_H

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
// So every line a bot says with something behind it -- a topic, a premise, a
// thing it asked for, something it remarked on, the facts a reply was given
// (OllamaChatRequest::basis) -- is kept a while with what it was told, and
// its replies are given the last few: what it said, where, and what it went
// on. Asked about it, it answers from that, and makes up nothing more.
//
// World thread only.
// --------------------------------------------------------------------------

// `bot` just said `line` in `place` ("General", "/say", "party chat"), going
// on `basis`. Nothing is kept for an empty basis.
void Said_Note(Player* bot, const std::string& line, const std::string& place, const std::string& basis);

// For a reply prompt of `bot`: what it said lately and what it went on
// (Said.Text). Empty when it said nothing with anything behind it lately.
std::string Said_PromptFor(Player* bot);

void Said_Forget(ObjectGuid botGuid);   // logged out
void Said_Update();                     // maintenance: old lines forgotten

struct SaidStats
{
    uint64_t noted = 0;   // lines kept with what they were said on
    uint64_t told  = 0;   // reply prompts given them
    uint32_t bots  = 0;   // bots with lines kept now
};
SaidStats Said_GetStats();

#endif // MOD_OLLAMA_CHAT_SAID_H
