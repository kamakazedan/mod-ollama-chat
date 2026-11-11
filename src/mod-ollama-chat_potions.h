#ifndef MOD_OLLAMA_CHAT_POTIONS_H
#define MOD_OLLAMA_CHAT_POTIONS_H

#include "ScriptMgr.h"

#include <cstdint>

class Player;
class Spell;

// --------------------------------------------------------------------------
// "That pot saved my ass."
//
// A bot in a dungeon that drinks a healing potion with its health nearly gone
// (Potions.HealthPct), or a mana potion with its mana nearly gone
// (Potions.ManaPct) -- the tank on its last legs, the healer running dry --
// and lives through the fight may say so in party once the fight is over, in
// its own words and style: that the pot saved it, how close it was, a joke
// (Potions.HealPrompt, Potions.ManaPrompt). How likely goes by its chat style
// (Potions.Personalities, or Potions.Chance for one not listed): at most once
// every Potions.BotCooldownMinutes, and one bot in a group every
// Potions.GroupCooldownSeconds. Not when it died anyway, nor in a group
// without a real player.
//
// What counts as a potion is what the game says: a consumable whose spell
// heals (a healing potion, a healthstone) or gives back mana (a mana potion,
// a mana gem).
//
// The cast hook runs on a map thread: it only notes the potion, the bot's
// health or mana and the time. The rest is on the world tick.
// --------------------------------------------------------------------------

class OllamaChatPotionScript : public PlayerScript
{
public:
    OllamaChatPotionScript() : PlayerScript("OllamaChatPotionScript", { PLAYERHOOK_ON_SPELL_CAST }) {}
    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override;
};

// World tick: a potion drunk in a pinch, looked at once the fight is over.
void Potions_Update();

struct PotionStats
{
    uint64_t noted = 0;   // potions drunk in a pinch in a dungeon, by anyone
    uint64_t lived = 0;   // ...by a bot with a real player in its group that came through the fight
    uint64_t said  = 0;   // lines about it
};
PotionStats Potions_GetStats();

#endif // MOD_OLLAMA_CHAT_POTIONS_H
