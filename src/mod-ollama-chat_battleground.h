#ifndef MOD_OLLAMA_CHAT_BATTLEGROUND_H
#define MOD_OLLAMA_CHAT_BATTLEGROUND_H

#include "mod-ollama-chat_handler.h"

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Battlegrounds (OllamaChat.Battleground.*): callouts, and a team that
// answers.
//
// A battleground's chat is where a team talks: "inc farm 3", "they have our
// flag", "nice cap", "gg". With bots for a team it was silent. What a bot did
// say there was a reaction to a kill, in party chat -- and to a line typed in
// battleground chat nobody answered: the module never heard it.
//
// So, in a battleground with someone playing in it, for that someone's team:
//
// - What is typed in battleground chat is heard and answered like a line in
//   a raid's chat, by the bots of the team, in battleground chat. A reply is
//   told which battleground it is, how it is won and how it stands right
//   now (Battleground.ReplyText).
// - The bots call out what happens, each line a team's: the match about to
//   start; in Warsong Gulch a flag picked up, got back or captured; in
//   Arathi Basin a base under attack, taken or lost; in Eye of the Storm a
//   tower taken or lost and the flag; the match nearly over; and "inc farm
//   3", typed by a bot standing at a base its team holds with the enemy
//   coming (Battleground.IncomingLines). At the end a few say gg
//   (Battleground.GgLines), typed at once: a bot leaves a battleground the
//   moment it is over.
//
// Everything said is read from the battleground itself -- the score, who
// holds what, who carries which flag and where on the field they are -- at
// the moment the line is written, and looked at again when it goes out: a
// line takes seconds to write, and "they have our flag" after the flag is
// home again is not said (OllamaChatRequest::battleground,
// Battleground_StillSo).
//
// Warsong Gulch, Arathi Basin and Eye of the Storm are read in full. Alterac
// Valley, Isle of Conquest and Strand of the Ancients keep their state where
// a module cannot read it, so there the bots only wish luck, answer (told
// what the battleground is and how it is won, nothing of how it stands) and
// say gg. Arenas are left alone.
//
// In a battleground a bot's group is its team, so a line of its to its group
// -- these, a reaction to a kill, an answer -- goes out in battleground
// chat, and the small talk a bot makes elsewhere is not made there.
//
// World thread only, and no hooks: the battleground is looked at from the
// world tick, while the map threads are idle.
// --------------------------------------------------------------------------

void Battleground_LoadConfig();             // startup and .ollama reload
void Battleground_Update(uint32_t diff);    // world tick

// `who` is in a battleground (not an arena) with its team for a group, and
// battleground chat is this module's to hear and speak in.
bool Battleground_TeamChat(Player* who);

// Says `text` in `bot`'s battleground chat: to those playing on its team.
// False when nobody is there to read it.
bool Battleground_Say(Player* bot, const std::string& text);

// For a reply of `bot`'s to its group in a battleground: which one it is,
// how it is won and how it stands. Empty anywhere else.
std::string Battleground_PromptFor(Player* bot, ChatChannelSourceLocal source);

// Whether what a battleground line was written on (`claim`,
// OllamaChatRequest::battleground) is still so, now that it is to go out.
bool Battleground_StillSo(Player* bot, uint64_t claim);

struct BattlegroundStats
{
    uint32_t watched  = 0;   // battlegrounds with someone playing in them now
    uint64_t matches  = 0;   // ...since startup
    uint64_t lines    = 0;   // lines sent to be written
    uint64_t typed    = 0;   // "inc farm 3", typed
    uint64_t gg       = 0;   // "gg", typed at the end
    uint64_t stale    = 0;   // lines not said: no longer so by the time they were written
    uint64_t passed   = 0;   // things that happened and got no line: the chance, the gap, nobody free
    uint64_t answered = 0;   // replies told how the battleground stands
};
BattlegroundStats Battleground_GetStats();

#endif // MOD_OLLAMA_CHAT_BATTLEGROUND_H
