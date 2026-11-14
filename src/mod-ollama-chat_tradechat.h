#ifndef MOD_OLLAMA_CHAT_TRADECHAT_H
#define MOD_OLLAMA_CHAT_TRADECHAT_H

#include <cstdint>
#include <string>

class Channel;
class Player;

// --------------------------------------------------------------------------
// Trade chat: buying and selling, and nothing else (OllamaChat.Trade.*).
//
// Trade is where people buy and sell, so that is all bots do there: no
// chatter, no banter, no conversations, no answers to anything that is not
// an offer. What they do there they mean, and carry out:
//
//   * A real player selling something linked ("WTS [Lil Timmy's Peashooter]
//     6g", "anyone want [Woolen Bag]?") is answered by the bot in that city
//     it is most use to -- the way playerbots judges loot: an upgrade it can
//     wear at its level, a quest item it needs, something for its
//     profession -- and only one with the money for it. It is told what the
//     thing really is (a blue gun, item level 21, for level 16; hunters,
//     warriors and rogues use guns) and what it is worth: what the auction
//     house has it for, else a multiple of what a vendor pays.
//   * A real player buying ("WTB [Linen Cloth] x20") is answered by a bot
//     there that has some it can spare.
//   * "pc [item]", "how much is [item] worth": a bot says what it goes for.
//   * Each haggles the way its chat style does (Trade.Style.<KEY>): one pays
//     the asking price, one haggles hard, one makes its offer and walks.
//     An ask far over what it is worth is called out ("6g? its 1g 40s on the
//     ah"), a lowball too -- by the bot that wants it, or, when nobody does,
//     now and then by someone who has had enough of it.
//   * The numbers are decided here, not by the model: a line that names any
//     other price than the one offered (and the ones it is told: their ask,
//     what it is worth) goes out as a plain stock line instead.
//   * A deal struck -- they take its offer, or it takes theirs -- is carried
//     out: close by, the bot walks over and opens the trade; further off, it
//     says where it is and waits for them. In the window it puts in exactly
//     the money agreed when they put in exactly the thing agreed (and no
//     more of it, and nothing else), or the thing when they put in exactly
//     the money; its side is accepted only then, and theirs finishes it.
//     Playerbots, which turns down any trade with someone outside the bot's
//     group, is held off it meanwhile (mod-ollama-chat_favor).
//   * Now and then a bot in a city with a real player about puts up
//     something it can spare: "WTS [Pagan Vest] 1g 50s". A whisper or a line
//     about it is a haggle like any other.
//
// The talk afterwards -- "its a gun", "deal, come trade me", whispered or in
// Trade -- is the same bot's, told how the haggle stands.
//
// World thread only.
// --------------------------------------------------------------------------

// Is `channel` the Trade channel ("Trade - City")?
bool TradeChat_IsTrade(Channel const* channel);

// Trade chat is for buying and selling only (Trade.Enable and
// Trade.OnlyCommerce): nothing but this module says anything there.
bool TradeChat_OnlyCommerce();

// A real player's line in Trade. True when nothing else is to answer it: it
// was about a deal (answered here, or deliberately not), or Trade is for
// buying and selling only. False: ordinary chat, for the rest of the handler.
bool TradeChat_HandleHumanLine(Player* player, const std::string& msg, Channel* channel,
                               const std::string& scopeKey);

// A real player's whisper to a bot. True when it is about a deal or a haggle
// with that bot -- one it has up for sale, or one it is in the middle of --
// and has been answered: the caller sends nothing else.
bool TradeChat_HandleWhisper(Player* player, Player* bot, const std::string& msg, const std::string& scopeKey);

// For any other reply prompt of `bot` to `player`: the deal or haggle between
// them, so it is not forgotten. Empty when there is none.
std::string TradeChat_PromptFor(Player* bot, Player* player);

void TradeChat_Tick();     // world tick: walking over to trade, deals done, things put up for sale
void TradeChat_Update();   // maintenance: old haggles and listings forgotten

struct TradeChatStats
{
    uint64_t heard     = 0;   // real players' lines in Trade
    uint64_t ignored   = 0;   // ...not buying or selling anything, so nobody answered
    uint64_t sales     = 0;   // things they put up for sale
    uint64_t wants     = 0;   // things they wanted to buy
    uint64_t checks    = 0;   // prices they asked about
    uint64_t nobody    = 0;   // nobody wanted it, or had one to spare
    uint64_t answered  = 0;   // lines a bot said about a deal
    uint64_t counters  = 0;   // counter-offers
    uint64_t callouts  = 0;   // rip-offs and lowballs called out
    uint64_t passed    = 0;   // haggles a bot walked away from
    uint64_t agreed    = 0;   // deals struck
    uint64_t walked    = 0;   // bots that walked over to trade
    uint64_t done      = 0;   // deals that changed hands
    uint64_t lapsed    = 0;   // deals struck that nobody came to trade
    uint64_t ads       = 0;   // things bots put up for sale
    uint64_t fallbacks = 0;   // the model named another price: a stock line instead, or nothing
    uint32_t talking   = 0;   // haggles going on now
};
TradeChatStats TradeChat_GetStats();

// The model's line about a deal named a price nobody agreed to, so a stock
// line went instead (mod-ollama-chat_dispatch).
void TradeChat_NoteFallback();

#endif // MOD_OLLAMA_CHAT_TRADECHAT_H
