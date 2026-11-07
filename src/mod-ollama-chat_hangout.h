#ifndef MOD_OLLAMA_CHAT_HANGOUT_H
#define MOD_OLLAMA_CHAT_HANGOUT_H

#include <string>

class Player;

// --------------------------------------------------------------------------
// City hangouts, as this module sees them.
//
// mod-playerbots-plus parks idle bots where players stand about in the
// capitals -- the Trade District, the Commons, outside the bank in Dalaran --
// mounted, sat on a bench, or AFK. This is what the chat needs of that: a bot
// that is AFK there stays quiet in public, comes back to the keyboard when it
// talks, and knows where it is and what it is doing when someone asks
// (Hangout.StateText, Hangout.AwayText).
//
// Without mod-playerbots-plus nobody hangs out, and all of this says so.
// World thread only, except Hangout_UsingBench.
// --------------------------------------------------------------------------

// Hanging out AFK (the flag the hangout set).
bool Hangout_IsAway(Player* bot);

// Someone talked to it, or it is about to talk: back at the keyboard.
void Hangout_ComeBack(Player* bot);

// True while a hanging-out bot is sitting down on a bench on this thread, so
// the "used an object" event chatter does not take it for news.
bool Hangout_UsingBench();

// For a reply prompt: where it is hanging out and how ("sitting on a bench
// in the Trade District"), and that it has just come back if it was AFK.
// Empty when it is not hanging out.
std::string Hangout_StateTextFor(Player* bot);

// Where it is hanging out and how, for a prompt: "the Trade District in
// Stormwind", "sitting on a bench". False when it is not settled at a spot.
bool Hangout_WhereAndDoing(Player* bot, std::string& spot, std::string& doing);

#endif // MOD_OLLAMA_CHAT_HANGOUT_H
