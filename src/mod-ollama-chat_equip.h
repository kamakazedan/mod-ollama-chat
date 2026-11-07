#ifndef MOD_OLLAMA_CHAT_EQUIP_H
#define MOD_OLLAMA_CHAT_EQUIP_H

#include "ScriptMgr.h"

#include <cstdint>

class Item;
class Player;

// --------------------------------------------------------------------------
// A bot putting on something good it just got, and now and then saying so.
//
// Playerbots whispers its master "Equipping [Lorekeeper's Ring]" for every
// piece it puts on (mod-playerbots-plus mutes that). A player who gets a
// nice drop does not announce every ring; with a rare or an epic
// (Equip.MinQuality) they might say so in party, or might not: it depends
// who they are. So a bot in a group with a real player that puts on a rare
// or epic it got in the last Equip.LootedMinutes -- loot, a roll it won, a
// quest reward; never gear playerbots handed it -- may say so in party, in
// its own words: how likely by its chat style (Equip.Personalities, or
// Equip.Chance for one not listed), at most once every
// Equip.BotCooldownMinutes, and one bot in a group every
// Equip.GroupCooldownSeconds.
//
// The equip hook runs on whatever thread the bot's AI does (a map thread):
// it only notes the item. The rest is on the world tick.
// --------------------------------------------------------------------------

class OllamaChatEquipScript : public PlayerScript
{
public:
    OllamaChatEquipScript() : PlayerScript("OllamaChatEquipScript", { PLAYERHOOK_ON_EQUIP }) {}
    void OnPlayerEquip(Player* player, Item* item, uint8 bag, uint8 slot, bool update) override;
};

// World tick: what was put on, looked at a few seconds later.
void Equip_Update();

struct EquipStats
{
    uint64_t noted = 0;   // rares and epics put on by anyone
    uint64_t fresh = 0;   // ...by a bot with a real player in its group, just got
    uint64_t said  = 0;   // lines about it
};
EquipStats Equip_GetStats();

#endif // MOD_OLLAMA_CHAT_EQUIP_H
