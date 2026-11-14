#ifndef MOD_OLLAMA_CHAT_RAREGEAR_H
#define MOD_OLLAMA_CHAT_RAREGEAR_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Rare gear: in a city, now and then, a bot shows off a really rare thing it
// has on in General -- "check out my [Hand of Edward the Odd]" -- or asks
// where one is found, and the people about talk it over: where it drops and
// how rare it is, from the world's own loot tables (mod-ollama-chat_drops,
// with mod-playerbots-plus API 8), never a creature or a place made up.
//
// Really rare is a weapon or armour of RareGear.MinQuality or better that
// drops less often than RareGear.MaxDropPercent per kill of anything that
// can drop it (and not so seldom that it is a leftover in the tables,
// RareGear.MinDropPercent), no more often than RareGear.MaxBestPercent from
// any one creature or chest -- a boss's loot is no rare find -- and from a
// creature out in the world. A bot asks after one it could wear: its armour
// type, a weapon it can use, stats for its class, about its level.
//
// Which bots do it is their chat style's (RareGear.ShowStyles,
// RareGear.AskStyles). The same thing is not talked of again on the realm
// for RareGear.ItemMinutes, one city's General hears one such line in
// RareGear.PlaceMinutes, and a bot says one in RareGear.BotMinutes. Its line
// may open a conversation (RareGear.ConversationChance) about it, the facts
// in its premise; replies to it are told what the thing is and how rare
// (Links_DescribeItem) and, asked where it is found, where it drops
// (Knowledge_DropFacts).
//
// World thread only.
// --------------------------------------------------------------------------

enum class RareGearKind : uint8_t
{
    Show,
    Ask
};

struct RareGearLine
{
    RareGearKind kind   = RareGearKind::Show;
    uint32_t     itemId = 0;
    std::string  name;          // the thing's name
    std::string  environment;   // what the line is to be, for its prompt
    std::string  premise;       // what a conversation about it is about, with the facts
    std::string  fallback;      // a stock line in the bot's style, for when the model's names no thing
};

// For `bot`, about to say something unprompted in a city's General
// (`placeKey`): by its chat style, a really rare thing it has on to show
// off, or one it would want to ask where to find. False: neither, now.
bool RareGear_Pick(Player* bot, const std::string& placeKey, RareGearLine& out);

// Could a line like that be said at `placeKey` now (RareGear.PlaceMinutes)?
bool RareGear_PlaceFree(const std::string& placeKey);

// The line was handed to the dispatcher: the thing, the place and the bot
// are not heard of again for a while.
void RareGear_NoteSaid(Player* bot, const RareGearLine& line, const std::string& placeKey, bool conversation);

// Is it really rare, as above?
bool RareGear_IsRare(uint32_t itemId);

struct RareGearStats
{
    uint64_t shown         = 0;   // lines showing one off
    uint64_t asked         = 0;   // lines asking where one is found
    uint64_t conversations = 0;   // ...that opened a conversation
    uint64_t none          = 0;   // rolled for, and nothing to say this time
    uint32_t pool          = 0;   // really rare things in the world (0 until the first ask)
};
RareGearStats RareGear_GetStats();

#endif // MOD_OLLAMA_CHAT_RAREGEAR_H
