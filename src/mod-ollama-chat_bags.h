#ifndef MOD_OLLAMA_CHAT_BAGS_H
#define MOD_OLLAMA_CHAT_BAGS_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// What a bot carries: what it could part with, and what it keeps and why.
//
// The judgement is playerbots' own item usage -- the one it loots, rolls and
// sells by -- so a bot never offers the upgrade it is about to put on or the
// quest item it is collecting. Spare is anything tradeable it has no use for:
// cloth it does not work, greens for another class or worse than what it
// wears, things it would sell. Kept: an upgrade, a quest item, its trade
// goods, the food and potions it uses, and anything soulbound.
//
//   * Asked what it has ("got anything to trade?", "what did you loot?"),
//     its reply prompt gets the list (Bags.PromptText): spare things as
//     links, what it just looted first, and a few things it keeps, with why.
//   * Asked for something ("can I have your [Linen Cloth]", "ill take the
//     leggings", or just "me!" after it offered one), a spare thing goes over
//     in a trade -- the way a mage's water does -- and a kept one gets a no
//     and the reason. Playerbots only trades inside a group, so someone
//     outside it is told to invite the bot first. Only what a player would
//     give away free counts as spare: not a blue, not a stack worth real
//     gold (Bags.MaxGiftQuality, Bags.MaxGiftCopper).
//
// World thread only, except Bags_NoteLoot.
// --------------------------------------------------------------------------

struct BagEntry
{
    uint32_t    entry   = 0;
    uint32_t    count   = 0;       // all its stacks together
    uint32_t    quality = 0;
    std::string name;
    bool        spare   = false;   // tradeable, and no use to it
    bool        worn    = false;   // on it, not in its bags
    std::string why;               // kept: why ("you need it for [Wanted: Hogger]")
    bool        recent  = false;   // looted in the last Bags.RecentMinutes
};

// What it carries (and wears, as kept), spare first: what it just looted,
// then the better things.
std::vector<BagEntry> Bags_Scan(Player* bot);

// For a reply prompt: what it could spare and what it keeps
// (OllamaChat.Bags.PromptText), or empty when there is nothing to say.
std::string Bags_Describe(Player* bot);

// Is this line about trading or about what someone has ("anything to
// trade?", "what did you loot", a linked item)?
bool Bags_IsItemTalk(const std::string& msg);

// A bot just looted something. Any thread: it only records the item.
void Bags_NoteLoot(Player* bot, uint32_t entry);

// A bot just offered something in `scopeKey` (OllamaChat.Needs: "anyone
// want [Linen Cloth]?", recorded when the line goes out): for a few minutes
// "me!" there claims it.
void Bags_NoteOffer(Player* bot, uint32_t entry, const std::string& scopeKey);

// A real player asking a bot for something it carries. True when it was
// that and has been answered (and the trade started, where it could be).
// `seq` is the line's transcript number, 0 when it is not recorded yet.
bool Bags_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                          const std::string& channelName, Player* receiver, const std::string& scopeKey,
                          uint64_t seq);

void Bags_Tick();     // world tick: a trade promised once they invited it starts when they have
void Bags_Update();   // maintenance tick: old loot and offers are forgotten

struct BagsStats
{
    uint64_t described = 0;   // reply prompts told what is in the bags
    uint64_t asked     = 0;   // asks for something it carries
    uint64_t given     = 0;   // trades started for it
    uint64_t kept      = 0;   // said no: it needs it
    uint64_t invite    = 0;   // told to invite it first
    uint64_t offers    = 0;   // spare things linked in its own lines
};
BagsStats Bags_GetStats();

#endif // MOD_OLLAMA_CHAT_BAGS_H
