#ifndef MOD_OLLAMA_CHAT_WIPE_H
#define MOD_OLLAMA_CHAT_WIPE_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// When it goes wrong: the tank down, a wipe, nobody left to rez.
//
// A group wiped and its bots had no idea: "how do i keep my health up here?",
// "shes coming back soon i think", "just waiting for urdrudes to get back
// then" -- with every one of them lying dead. Now, once a second, each group
// with a real player in it is looked at:
//
//   * The tank -- mod-dungeon-clear's tank when there is one, else whoever
//     tanks by spec -- dies in the middle of a fight with others still up: one
//     bot still standing says so the way people do, "uh oh", "oh no"
//     (Wipe.TankDown*), once a fight.
//   * Everyone where the group was fighting is dead -- a wipe: a bot tank says
//     sorry (Wipe.Apology*), and then someone says nobody is left to rez, so
//     it is release and run back (Wipe.NoRez*) -- unless one of them has a
//     soulstone or a reincarnation ready, which gets them up without anyone.
//   * After a fight with some of them dead and nobody alive who can resurrect
//     (the real players count), one of the dead bots says it is running back
//     (Wipe.RunBack*), as mod-ollama-chat_revive then has it do.
//
// Each is the model's line, in the bot's own voice, or now and then a typed
// one; a typed one goes out instead when the model cannot be reached.
// And every reply prompt of a bot in such a group says who is dead, whether
// it was a wipe and who went down first, and who can still bring them back
// (Wipe.State*), so "we wiped, we need to respawn" is understood.
//
// World thread only.
// --------------------------------------------------------------------------

void Wipe_Update(uint32_t diff);

// For a reply prompt: who in this bot's group is dead and what happens next.
// Empty when nobody is.
std::string Wipe_StateTextFor(Player* bot);

// How many times `member` has died in its current group (for asides).
uint32_t Wipe_DeathsOf(Player* member);

struct WipeStats
{
    uint64_t tankDown  = 0;   // "uh oh": the tank died mid-fight
    uint64_t wipes     = 0;   // groups wiped
    uint64_t apologies = 0;   // a bot tank said sorry
    uint64_t noRez     = 0;   // "nobody to rez, run back" after a wipe
    uint64_t runBack   = 0;   // a dead bot said it is running back
};
WipeStats Wipe_GetStats();

#endif // MOD_OLLAMA_CHAT_WIPE_H
