#ifndef MOD_OLLAMA_CHAT_REVIVE_H
#define MOD_OLLAMA_CHAT_REVIVE_H

#include <cstdint>

// --------------------------------------------------------------------------
// Dead bots get themselves back up.
//
// Playerbots leaves a bot that dies near a real player lying where it fell
// until it is told what to do, and mod-dungeon-clear keeps every dungeon
// corpse for a rez. People do not wait to be told: when nobody still standing
// can resurrect them -- no living priest, paladin, shaman or druid who knows
// the spell -- they release, run back, get up and rejoin the fight.
//
//   * Someone alive who can rez: wait for it. OllamaChat.Revive.WaitForRezSeconds
//     (counted only while nobody is fighting) is how long; 0 waits for good.
//   * Nobody: release after OllamaChat.Revive.ReleaseDelaySeconds.
//   * A ghost whose body is in a dungeon or raid runs from the graveyard to
//     the entrance and walks in, which brings it back there as it does for
//     anyone (OllamaChat.Revive.RunBackSeconds bounds the run).
//   * In the open world the ghost walks back to its body, as playerbots does.
//
// Only bots in a group with a real player; never in battlegrounds or arenas,
// and never while a mod-dungeon-clear run is deciding how to bring its party
// back. World thread only.
// --------------------------------------------------------------------------

void Revive_Update(uint32_t diff);

class Player;

// A mod-dungeon-clear run owns this bot's group, so the run decides how its
// dead come back: nothing else releases them.
bool Revive_RunDecides(Player* bot);

struct ReviveStats
{
    uint64_t released = 0;   // told to release, nobody could rez them
    uint64_t returned = 0;   // ghosts that walked back into their dungeon
    uint64_t gaveUp   = 0;   // a rezzer was there but never did it
};
ReviveStats Revive_GetStats();

#endif // MOD_OLLAMA_CHAT_REVIVE_H
