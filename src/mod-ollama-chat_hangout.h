#ifndef MOD_OLLAMA_CHAT_HANGOUT_H
#define MOD_OLLAMA_CHAT_HANGOUT_H

#include <cstdint>
#include <string>
#include <vector>

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
// Two bots that talk themselves into a duel walk out to the city's gates and
// fight it out there (mod-ollama-chat_mingle asks first whether they can).
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

// Whether two bots could go out to the gates and duel there now
// (mod-playerbots-plus's Duels.*: of a side, about a level, free, a duel
// spot they can walk out to), and where that is, for a prompt: "outside the
// gates of Stormwind". Without mod-playerbots-plus, never.
bool Hangout_CanGoDuel(Player* a, Player* b, std::string& where);

// ...and they go: both walk out there, and fight it out once they are
// there. False when they cannot after all.
bool Hangout_GoDuel(Player* a, Player* b);

// City life (mod-playerbots-plus's CityLife.*: the crowd shifting about,
// looking round, dancing...), told what this module has bots doing: one is
// talking or emoting for `seconds`, so it is not walked off mid-sentence (0:
// done); someone said something in /say, so some hanging out near them look
// round; these are about to talk in /say, so those hanging out walk over to
// the first to talk face to face. Without mod-playerbots-plus (API 5), none
// of it happens.
void Hangout_Busy(Player* bot, uint32_t seconds);
void Hangout_Spoke(Player* speaker);
void Hangout_Gather(const std::vector<Player*>& bots, uint32_t seconds);

#endif // MOD_OLLAMA_CHAT_HANGOUT_H
