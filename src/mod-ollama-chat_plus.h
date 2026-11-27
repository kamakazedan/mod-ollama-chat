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
// bot looted itself (OllamaLootedByBot): the only things a bot ever sells, 10
// for group quests two bots talk each other into (OllamaGroupQuestFor, in
// mod-ollama-chat_mingle), 11 for what a bot made with its professions being
// its own too, 12 for disenchanting (OllamaCanDisenchant: an enchanter buying
// gear in Trade chat for its dust).
// --------------------------------------------------------------------------

#if __has_include("PlayerbotsPlus.h")
#include "PlayerbotsPlus.h"
#endif

#if defined(PLAYERBOTS_PLUS_API_VERSION)
#define OLLAMA_CHAT_PLAYERBOTS_PLUS PLAYERBOTS_PLUS_API_VERSION
#else
#define OLLAMA_CHAT_PLAYERBOTS_PLUS 0
#endif

#include <cstdint>
#include <string>

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

// A group or elite quest of the zone `a` and `b` stand in that they could go
// and do together now (mod-playerbots-plus's group quests: both holding it,
// up to it, by each other and free), the one nearest their level. 0 for none,
// and always 0 without it. World thread.
inline uint32_t OllamaGroupQuestFor(Player* a, Player* b)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 10
    return PlayerbotsPlus::GroupQuestFor(a, b);
#else
    (void)a;
    (void)b;
    return 0;
#endif
}

// ...and, having talked each other into it, they go and do it: a party for
// it there and then, `a` leading. False when they cannot after all. World
// thread.
inline bool OllamaGroupQuestTogether(Player* a, Player* b, uint32_t quest)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 10
    return PlayerbotsPlus::GroupQuestTogether(a, b, quest);
#else
    (void)a;
    (void)b;
    (void)quest;
    return false;
#endif
}

// Could `bot` disenchant one of these: an enchanter with the skill for it,
// where the game's own loot tables say what it would give
// (mod-playerbots-plus's crafting)? Never without it. World thread.
inline bool OllamaCanDisenchant(Player* bot, uint32_t itemId)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 12
    return PlayerbotsPlus::CanDisenchant(bot, itemId);
#else
    (void)bot;
    (void)itemId;
    return false;
#endif
}

// What disenchanting one gives is worth on average, in copper, by the same
// sums as everything else in Trade chat. 0: nothing known.
inline uint64_t OllamaDisenchantWorth(uint32_t itemId)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 12
    return PlayerbotsPlus::DisenchantWorth(itemId);
#else
    (void)itemId;
    return 0;
#endif
}

// `item`, now in `bot`'s bags, was bought to be disenchanted: it may be,
// though the bot neither looted nor made it, and what comes out is its own.
// World thread.
inline void OllamaBoughtToDisenchant(Player* bot, Item* item)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 12
    PlayerbotsPlus::BoughtToDisenchant(bot, item);
#else
    (void)bot;
    (void)item;
#endif
}

// How `bot`'s two professions came of the leaning its personality is
// (mod-playerbots-plus, Professions.Leanings): "own" a pair the leaning
// names, "armour" the one that makes the armour it wears, "race" its race's
// knack, "rare" what the fewest of its class take, "class" one of its
// class's pairs. Empty without it, with no leaning, or for a pair the
// leaning would not have. World thread.
inline std::string OllamaLeaningMatch(Player* bot)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 13
    return PlayerbotsPlus::LeaningMatch(bot);
#else
    (void)bot;
    return {};
#endif
}

// The profession a race is better at than the rest, by its racial
// ("Engineering" for a gnome). Empty for the rest, and without it.
inline std::string OllamaRaceKnack(uint8_t race)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 13
    return PlayerbotsPlus::RaceKnack(race);
#else
    (void)race;
    return {};
#endif
}

#endif // MOD_OLLAMA_CHAT_PLUS_H
