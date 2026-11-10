#ifndef MOD_OLLAMA_CHAT_PREMISES_H
#define MOD_OLLAMA_CHAT_PREMISES_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// What a conversation in town is about.
//
// It used to be one of a fixed list, picked at random every time: "would you
// rather fight one huge murloc or a hundred tiny ones" was a delight the first
// time and a script by the fourth. Now:
//
//   * a premise from the fixed list (Conversation.Premises), or a template
//     (Conversation.PremiseTemplates, PremiseTemplateChance of the time) with
//     slots filled from pools -- "a silly would-you-rather about {creature}",
//     "which of {class} and {class2} has it easier levelling" -- so the same
//     idea comes back about something else (Conversation.Pool.<name>; a
//     number on the end, {class2}, is another pick from the same pool);
//   * none of them, fixed or template, comes round again across the realm
//     for Conversation.PremiseCooldownMinutes (when all have, the one used
//     longest ago); a pool's last PremiseValueMemory picks are not picked
//     again;
//   * both may say "{~this|that}" for wording that changes each time, and be
//     level-tagged ("[15+] ..."), pool entries too;
//   * a premise or pool entry about one side's places and people is tagged
//     with that side, "[horde]" or "[alliance]", and only that side's bots
//     pick it: "the best spot in {city} to stand around in" is Orgrimmar for
//     a Horde bot, never Stormwind.
//
// {zone} is where the bot is, {faction} and {other_faction} its side and the
// other. World thread only.
// --------------------------------------------------------------------------

// A premise for `bot` to open a conversation with, filled in; empty when
// none fits.
std::string Premises_Pick(Player* bot);

struct PremiseStats
{
    uint64_t fixed     = 0;   // from the fixed list
    uint64_t templated = 0;   // from a template
    uint64_t none      = 0;   // nothing fitted
};
PremiseStats Premises_GetStats();

#endif // MOD_OLLAMA_CHAT_PREMISES_H
