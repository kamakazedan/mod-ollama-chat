#ifndef MOD_OLLAMA_CHAT_OUTLOOK_H
#define MOD_OLLAMA_CHAT_OUTLOOK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// What a bot would plausibly know and talk about, given who its character is.
//
// The system prompt says every bot "has played for years", so without this a
// level 6 gnome in Dun Morogh grumbles about repair bills and a mage offers to
// heal. Up to three short sentences go into each prompt:
//
//   level     -- what fills a player's days at this level (Outlook.LevelBands):
//                first quests at 6, Outland at 64, heroics and raids at 80;
//   class     -- which roles the class can fill, from the game's rules: a mage
//                only does damage, so it never offers to heal or tank. A class
//                with more than one role also says which one it plays now;
//   knowledge -- what anyone can see of another player (Outlook.KnowledgeText),
//                so nobody comments on someone's talents or bags.
//
// Conversation premises can carry a level tag, "[70+]" or "[10-39]", and are
// only picked by bots that level fits.
//
// World thread only.
// --------------------------------------------------------------------------

void   Outlook_Load();        // after the config is read
size_t Outlook_BandCount();

// The sentences for this bot, with a leading space; empty when
// OllamaChat.Outlook.Enable is 0.
std::string Outlook_ForBot(Player* bot);

// Just the level part, for a level (also used by the tests).
std::string Outlook_LevelText(uint32_t level);

// Does a premise's optional level tag ("[70+]", "[10-39]", "[80]") allow this
// level? Strips the tag from `premise` either way; no tag fits every level.
bool Outlook_PremiseFits(std::string& premise, uint32_t level);

// A random premise whose tag fits `level`, tag stripped. Empty when none does.
std::string Outlook_PickPremise(const std::vector<std::string>& premises, uint32_t level);

#endif // MOD_OLLAMA_CHAT_OUTLOOK_H
