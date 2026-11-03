#ifndef MOD_OLLAMA_CHAT_HANGOUT_H
#define MOD_OLLAMA_CHAT_HANGOUT_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// City hangouts: idle bots in a capital gather where players do.
//
// On a real server everyone parks in the same places -- the Trade District,
// the Commons, the Valley of Strength, outside the bank in Dalaran -- mounted
// on their best mount, sat on a bench, or stood there AFK. Playerbots never
// sends a bot anywhere in particular: one with nothing to do in a city walks
// from NPC to NPC, and a "rest" is 30 seconds sat wherever it happens to be.
//
// Every Hangout.CheckSeconds, a spot (OllamaChat.Hangout.Spot.*) with room
// may take one more of the idle random bots in its zone (and on its level of
// it) -- not grouped, not questing, flying or fighting, not one a real player
// has targeted. It walks over (playerbots' own "go camp" movement) to a free
// place within the radius (on the ground, in sight of the middle, out of the
// water, one you can walk to), and settles there for StayMinutesMin..Max:
//
//   * on a bench within Hangout.BenchRange that is free;
//   * mounted, if it can ride and it is outdoors (playerbots puts an idle
//     random bot on its best mount);
//   * sat on the ground, or just standing about;
//   * and now and then AFK (the flag, so it shows <AFK>): it stays quiet,
//     and when someone talks to it, it is back.
//
// From the moment it gets there its playerbots AI is held (renewed every
// world tick, as marks and orders do), the way a parked player does nothing:
// otherwise playerbots stands a sitting bot up and takes its AFK off on its
// very next tick. An invite, a trade, a duel or a fight lets it go at once.
// When its time is up it stands and goes about its business, and does not
// hang out again for Hangout.CooldownMinutes. If this stops running, the
// hold runs out by itself within a second.
//
// World thread only: Hangout_Update runs in OnWorldUpdate, while the map
// threads (and every bot's AI) are idle.
// --------------------------------------------------------------------------

void Hangout_Update(uint32_t diff);

// Hanging out AFK (the flag this set). Not for a bot that has since joined a
// group: that one is let go here and now.
bool Hangout_IsAway(Player* bot);

// Someone talked to it, or it is about to talk: back at the keyboard.
void Hangout_ComeBack(Player* bot);

// True while a hanging-out bot is sitting down on a bench here, so the
// module's "used an object" event chatter does not take it for news. Any
// thread (it is thread_local: only the world thread ever sets it).
bool Hangout_UsingBench();

// For a reply prompt: where it is hanging out and how ("sitting on a bench
// in the Trade District"), and that it has just come back if it was AFK.
// Empty when it is not hanging out.
std::string Hangout_StateTextFor(Player* bot);

struct HangoutStats
{
    uint64_t sent      = 0;   // set off for a spot
    uint64_t settled   = 0;   // got there and settled
    uint64_t gaveUp    = 0;   // never got there
    uint64_t movedOn   = 0;   // time up, off again
    uint64_t cutShort  = 0;   // a group, a fight, moved away...
    uint64_t cameBack  = 0;   // AFK, then talked to
    uint32_t hanging   = 0;   // there now
    uint32_t away      = 0;   // ...of whom AFK
    uint32_t onTheWay  = 0;
    uint32_t spots     = 0;
};
HangoutStats Hangout_GetStats();

#endif // MOD_OLLAMA_CHAT_HANGOUT_H
