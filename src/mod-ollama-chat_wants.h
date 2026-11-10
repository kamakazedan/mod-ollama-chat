#ifndef MOD_OLLAMA_CHAT_WANTS_H
#define MOD_OLLAMA_CHAT_WANTS_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// "I need a wand": where one really comes from.
//
// In Wailing Caverns someone said they still needed a wand, and a bot sent
// them to kill a few more murlocs in the south, past the big bridge in the
// east: none of it real, and asked "south of where?" it made up more. A line
// that says its speaker is after a kind of item -- one of Wants.Words before
// a wand, a shield, a cloak, boots (TextRules::FindItemWant) -- now has each
// reply prompt told what is really so for the speaker's level and class,
// from the game's own data (Wants.Text):
//
//   * in a dungeon, which of its bosses can drop one they could use, and
//     whether that boss is down already this run -- or that none does;
//   * a dungeon or two around their level where a boss can drop one;
//   * what a profession makes of the kind around their level;
//   * and whether the bot answering could use one itself.
//
// It answers from that in its own way: the boss here, another dungeon, a
// crafter, hoping one drops, needing one too. For Wants.FollowSeconds after,
// the speaker's next lines ("south of where?") and the replies of their
// group carry the same facts.
//
// Boss loot is read from the world database once, on the first such line:
// the dungeon finder's dungeons, their bosses as the instance encounters
// credit them, and the gear those can drop -- their own loot tables, not the
// world-drop tables half the world refers to.
//
// World thread only.
// --------------------------------------------------------------------------

// For a reply prompt of `bot` answering `speaker`'s `msg`: where what the
// speaker is after really comes from, when the line says they are after
// something, or comes a moment after it did. Empty otherwise.
std::string Wants_PromptFor(Player* bot, Player* speaker, const std::string& msg);

void Wants_Update();   // maintenance: wants told long ago are forgotten

struct WantsStats
{
    uint64_t heard     = 0;   // lines saying their speaker is after a kind of item
    uint64_t prompts   = 0;   // reply prompts told where one comes from
    uint64_t followUps = 0;   // ...of those, for a line after the one that said it
    uint32_t dungeons  = 0;   // dungeons with bosses known to drop gear (0 until the first such line)
    uint32_t bosses    = 0;   // ...and their bosses
};
WantsStats Wants_GetStats();

#endif // MOD_OLLAMA_CHAT_WANTS_H
