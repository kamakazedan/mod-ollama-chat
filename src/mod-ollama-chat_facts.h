#ifndef MOD_OLLAMA_CHAT_FACTS_H
#define MOD_OLLAMA_CHAT_FACTS_H

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Real names from the game's own data, for conversation premises and topics.
//
// "a would-you-rather about trinkets" got "Trinket of the Hidden Sun or
// Trinket of the Hidden Moon?": two trinkets that do not exist. "which race
// makes the best shaman" got humans, who cannot be one. A premise that needs
// something specific is given the real thing instead, from the item, creature
// and DBC data the server loaded: {trinket}, {itemset} and {rare} (a rare
// spawn) near the bot's level, {bosses} after a {dungeon} or {raid} (" (its
// bosses: ...)"), {class_races} after a {class} (the races that can be one).
//
// World thread: the pools are built on first use.
// --------------------------------------------------------------------------

// A rare or epic trinket, an armour set, a rare spawn, for a bot of `level`;
// not one of `taken`. Empty when there is none.
std::string Facts_Trinket(uint32_t level, const std::vector<std::string>& taken);
std::string Facts_ItemSet(uint32_t level, const std::vector<std::string>& taken);
std::string Facts_Rare(uint32_t level, const std::vector<std::string>& taken);

// " (its bosses: Rhahk'Zor, Sneed and Edwin VanCleef)" for a dungeon or raid
// as the premise pools name it ("the Deadmines"); a single space when the
// game's data does not say, so the premise still fills.
std::string Facts_Bosses(const std::string& instance);

// Is it a name a player could have come across: not a placeholder, a test
// or a deprecated thing ("[PH] ...", "Monster - ...", "Deprecated ...").
bool Facts_SaneName(const std::string& name);

// The races of one side that can be the class: "orcs, tauren and trolls"
// for "shamans" on the Horde's (`alliance` false). Empty when unknown, and
// when only one of that side's races can be it: there is nothing to compare.
std::string Facts_ClassRaces(const std::string& cls, bool alliance);

// A dungeon the dungeon finder suggests for about `level`, with what is so
// of it: "Drak'Tharon Keep, a dungeon in Grizzly Hills for levels 74 to 76
// (its bosses: ...)" -- the zone its way in is in, the levels the dungeon
// finder lists it for, its bosses (not for one wing of several, whose map's
// bosses are every wing's). Empty when none suits the level. Told only "a
// dungeon that suits your level", a bot asked its guild to run "The
// Whispering Taverns", "a dungeon in the northern wastes".
std::string Facts_Dungeon(uint32_t level);

// The dungeon finder's dungeons whose way in is in the zone called `zone`,
// with the levels it lists each for: "The Deadmines (levels 15 to 21)". No
// more than three. Empty when there is none.
std::string Facts_DungeonsIn(const std::string& zone);

// The levels most of the quests of `zone` (an area id) are for: the middle
// four fifths of them, so one stray quest of another bracket does not stretch
// it. False for a zone with too few quests to say.
bool Facts_ZoneQuestLevels(uint32_t zone, uint32_t& lo, uint32_t& hi);

#endif // MOD_OLLAMA_CHAT_FACTS_H
