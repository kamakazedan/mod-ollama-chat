#ifndef MOD_OLLAMA_CHAT_PLUS_H
#define MOD_OLLAMA_CHAT_PLUS_H

// --------------------------------------------------------------------------
// mod-playerbots-plus, when it is part of the same build.
//
// What bots do without a language model lives there: hanging out in cities,
// getting unstuck, getting back up, raid marks, muting playerbots' stock
// lines, trading on the auction house. This module only has them talk about
// it. Modules are compiled into one worldserver with each other's src/ on the
// include path, so it is found without depending on it; without it, all of
// that is simply not there, and nothing here needs it.
//
// OLLAMA_CHAT_PLAYERBOTS_PLUS is its API version: 0 when it is not in the
// build, 1 for the market only, 2 for hangouts, bot events and muting, 3 for
// duels talked into as well, 4 for group quests (mod-ollama-chat_groupquests),
// 5 for city life (mod-ollama-chat_citylife, and mod-ollama-chat_hangout's
// Hangout_Busy, Hangout_Spoke and Hangout_Gather), 6 for city life by
// temperament and showing off, 7 for what things are worth and how rarely
// they drop (mod-ollama-chat_tradechat), 8 for where things drop
// (mod-ollama-chat_drops: knowledge's answers and rare gear), 9 for what each
// bot looted itself (OllamaLootedByBot): the only things a bot ever sells.
// --------------------------------------------------------------------------

#if __has_include("PlayerbotsPlus.h")
#include "PlayerbotsPlus.h"
#endif

#if defined(PLAYERBOTS_PLUS_API_VERSION)
#define OLLAMA_CHAT_PLAYERBOTS_PLUS PLAYERBOTS_PLUS_API_VERSION
#else
#define OLLAMA_CHAT_PLAYERBOTS_PLUS 0
#endif

class Item;
class Player;

// Did `bot` loot this stack in its bags itself, with nothing but more loot
// added to it since (mod-playerbots-plus's record)? A bot sells only that:
// never what playerbots hands it for nothing, nor anything it bought, was
// given, made or got from a quest. Without mod-playerbots-plus nothing is
// known to be its loot, so nothing is sold. World thread.
inline bool OllamaLootedByBot(Player* bot, Item* item)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 9
    return PlayerbotsPlus::LootedByBot(bot, item);
#else
    (void)bot;
    (void)item;
    return false;
#endif
}

#endif // MOD_OLLAMA_CHAT_PLUS_H
