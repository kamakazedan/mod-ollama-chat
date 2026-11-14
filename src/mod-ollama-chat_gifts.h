#ifndef MOD_OLLAMA_CHAT_GIFTS_H
#define MOD_OLLAMA_CHAT_GIFTS_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Player;
struct ItemTemplate;

// --------------------------------------------------------------------------
// A real player giving away, or selling, something they carry.
//
// "anyone want my [Woolen Bag]?" used to read to the bots as talk about items,
// so every one of them was shown what it could spare: one said it had no bags
// to spare, one offered its own vest, and "im giving it away, not asking for
// one" only confused them more. Now a line that links something the player
// has on them, with one of Gifts.OfferWords ("anyone want", "free", "if anyone
// needs") or Gifts.SellWords ("wts", "selling", or just a price: "5g"), is
// read as what it is. Each bot in the group judges the thing the way
// playerbots does when it loots -- an upgrade, a quest item it is collecting,
// something for its profession, a bag bigger than its smallest one or a bag
// slot it has free (all the more with its bags nearly full) -- and:
//
//   * Given away: the one it is most use to says yes please (Gifts.HintWant),
//     and that it can be traded over. Sometimes a second voice: another who
//     could use it too, or a no thanks. When nobody can use it, maybe one no
//     thanks (Gifts.NobodyChance). Playerbots takes it when it is put in a
//     trade window, as it takes any gift it has a use for.
//   * Sold, with no price named: the one who could use it asks how much.
//     At a price it would pay -- no more than playerbots itself values the
//     thing at (its vendor price times the bot's own buy rate) and no more
//     than Gifts.MaxSpendPercent of its money -- it says it will buy, and when
//     the player puts the thing in a trade window with it, it puts the money
//     in on its side. Asked too much, it offers what it would pay, and that
//     price stands if they trade at it. A later "5g then" from the player is
//     answered by the same bot.
//   * Named in the line, or whispered, only that bot answers.
//   * Soulbound (and past its loot trade window): one bot says it cannot be
//     traded.
//
// Afterwards the bot remembers for a while what it said yes to, or agreed to
// pay (Gifts_PromptFor), and the replies to the line do not list their own
// spare things (Gifts_IsOfferLine).
//
// World thread only.
// --------------------------------------------------------------------------

// A real player's line. True when it offered or sold something they carry and
// the group is answering: the caller sends no other replies to it. `seq` is
// the line's transcript number, 0 when it is not recorded yet.
bool Gifts_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                           const std::string& channelName, Player* receiver, const std::string& scopeKey,
                           uint64_t seq);

// Is `msg` a line `player` just gave away or sold something with?
bool Gifts_IsOfferLine(Player* player, const std::string& msg);

// For a reply prompt: what this bot said yes to, offered or agreed to pay
// lately (Gifts.Said*Text). Empty otherwise.
std::string Gifts_PromptFor(Player* bot);

// How much use `proto` is to `bot`, the way playerbots judges what it loots,
// and why, in its own words: 0 none, 40 and up it would take it as a gift, 50
// and up it would pay for it. World thread.
int Gifts_Judge(Player* bot, ItemTemplate const* proto, int32_t suffix, std::string& why);

// A deal struck in Trade chat (mod-ollama-chat_tradechat): `bot` pays exactly
// `price` for exactly `count` of `entry` (`suffix`, when not 0, its random
// property too) from `player`, who is outside its group. Its AI is held when
// they are close, it asks them to trade once they are within reach, puts the
// money in when they put exactly that in, and accepts its side only then.
// Kept for `seconds`. World thread.
void Gifts_AgreeDeal(Player* bot, Player* player, uint32_t entry, int32_t suffix, uint32_t count, uint32_t price,
                     uint32_t seconds);

// A deal between them still stands (not paid, not forgotten).
bool Gifts_HasDeal(Player* bot, Player* player);

// Called off, unless the money is in a window between them right now.
void Gifts_DropDeal(Player* bot, Player* player);

void Gifts_Tick();     // world tick: a bot that agreed a price puts the money in the trade window
void Gifts_Update();   // maintenance: old offers and deals are forgotten

struct GiftsStats
{
    uint64_t offers   = 0;   // things given away in chat that the bots answered
    uint64_t sales    = 0;   // things sold in chat that the bots answered
    uint64_t wanted   = 0;   // a bot said yes please
    uint64_t nobody   = 0;   // nobody could use it
    uint64_t asked    = 0;   // a bot asked the price
    uint64_t agreed   = 0;   // a bot agreed a price
    uint64_t countered = 0;  // a bot offered less
    uint64_t paid     = 0;   // a bot paid in a trade window
    uint64_t received = 0;   // a gift from someone outside its group, taken in a trade
    uint64_t marketPaid = 0; // of the paid: deals struck in Trade chat
    uint32_t deals    = 0;   // prices agreed and not traded yet
};
GiftsStats Gifts_GetStats();

#endif // MOD_OLLAMA_CHAT_GIFTS_H
