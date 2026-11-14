#ifndef MOD_OLLAMA_CHAT_DROPS_H
#define MOD_OLLAMA_CHAT_DROPS_H

#include <cstdint>
#include <string>

// --------------------------------------------------------------------------
// How rarely things drop, and from what: mod-playerbots-plus's sums from the
// world's own loot tables (PlayerbotsPlus::DropChance, API 7, and
// DropSourcesOf, API 8), so every part of this module that talks of a drop
// says the same as its market prices by. Without it -- or with an older one
// -- nothing is known, and these say so.
//
// World thread.
// --------------------------------------------------------------------------

// How rarely it drops, in percent per kill of anything that can drop it
// (over every one the world has of them), or for a thing only chests give,
// the best chest's. 0 when nothing drops it, or nothing is known.
double Drops_Pct(uint32_t itemId);

// Where it drops from. False when nothing drops it, or nothing is known.
struct DropsFrom
{
    uint32_t kinds    = 0;        // kinds of creature that drop it
    uint32_t spawns   = 0;        // how many of them the world has
    uint32_t minLevel = 0;        // the levels most of them are; 0 when none is out in the world
    uint32_t maxLevel = 0;
    uint32_t creature[3]  = {};   // creature entries, the likeliest first; 0: no more
    double   chancePct[3] = {};   // ...the chance a kill of each gives it, percent
    double   bestPct  = 0.0;      // the most any one creature or chest gives it, percent
    int32_t  onlyMap  = -1;       // the one map all of those creatures are on (a dungeon's); -1: several, none
    bool     chests   = false;    // a chest or a fishing hole gives it too
};
bool Drops_Sources(uint32_t itemId, DropsFrom& out);

// Is where things drop known at all (mod-playerbots-plus API 8 in the build)?
bool Drops_SourcesKnown();

// "20,800": once in how many, the way people say it -- two figures. For a
// chance in percent above 0.
std::string Drops_OnceIn(double pct);

#endif // MOD_OLLAMA_CHAT_DROPS_H
