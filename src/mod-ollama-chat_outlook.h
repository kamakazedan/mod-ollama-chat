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
// heal. A few short sentences go into each prompt:
//
//   level     -- what fills a player's days at this level (Outlook.LevelBands):
//                first quests at 6, Outland at 64, heroics and raids at 80;
//   class     -- which roles the class can fill, from the game's rules: a mage
//                only does damage, so it never offers to heal or tank. A class
//                with more than one role also says which one it plays now;
//   knowledge -- what anyone can see of another player (Outlook.KnowledgeText),
//                so nobody comments on someone's talents or bags;
//   side      -- which side it plays and its own side's cities
//                (Outlook.SideText), and, standing in one of them, that it is
//                home: safe, and the guards are its side's
//                (Outlook.HomeCityText). Never told, a Horde bot in Orgrimmar
//                said "these grunts are too high level" and asked who was
//                going AFK in Stormwind.
//
// Conversation premises can carry a level tag, "[70+]" or "[10-39]", and are
// only picked by bots that level fits -- and a side, "[horde]" or
// "[alliance]", picked only by that side's bots.
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

// Just the side part: "You play Horde: your own side's cities are ..." and,
// in one of them, that it is home. Empty when both texts are.
std::string Outlook_SideText(Player* bot);

// A side's own cities as one phrase (Outlook.Cities.Horde/.Alliance):
// "Orgrimmar, Thunder Bluff, the Undercity and Silvermoon City".
std::string Outlook_SideCities(bool alliance);

// The city `bot` stands in, by the game's name for it, when that is one of
// its own side's capitals; empty anywhere else (another side's, a neutral
// one, an instance).
std::string Outlook_HomeCity(Player* bot);

// Prompts that said a bot was in its own side's city.
uint64_t Outlook_HomeCityPrompts();

// Does a premise's optional level tag ("[70+]", "[10-39]", "[80]") allow this
// level? Strips the tag from `premise` either way; no tag fits every level.
bool Outlook_PremiseFits(std::string& premise, uint32_t level);

// A random premise whose tag fits `level`, tag stripped. Empty when none does.
std::string Outlook_PickPremise(const std::vector<std::string>& premises, uint32_t level);

#endif // MOD_OLLAMA_CHAT_OUTLOOK_H
