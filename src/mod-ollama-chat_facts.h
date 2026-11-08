#ifndef MOD_OLLAMA_CHAT_FACTS_H
#define MOD_OLLAMA_CHAT_FACTS_H

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Real names from the game's own data, for conversation premises.
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

// "orcs, tauren, trolls and draenei" for "shamans"; empty when unknown.
std::string Facts_ClassRaces(const std::string& cls);

#endif // MOD_OLLAMA_CHAT_FACTS_H
