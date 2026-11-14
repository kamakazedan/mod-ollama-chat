#ifndef MOD_OLLAMA_CHAT_HAGGLE_H
#define MOD_OLLAMA_CHAT_HAGGLE_H

// --------------------------------------------------------------------------
// What a bot pays, or asks, in Trade chat, and how it haggles.
//
// Pure arithmetic on copper, with no game types, so it can be checked on its
// own. mod-ollama-chat_tradechat does the rest: who answers, and saying it.
//
// The idea is the one a real player goes by:
//   - something is worth what the auction house has it for, or else a
//     multiple of what a vendor pays for it, more for better quality;
//   - a buyer pays up to its own share of that (its chat style's `pay`), and
//     never more than it can spare; a seller takes no less than its share;
//   - a haggler (`haggle`) opens well under, or over, and meets them part of
//     the way each time, `rounds` times before it says that is its last;
//   - an ask far over what it is worth (`ripOffPct`) is taking the mickey,
//     and a bid far under it (`lowballPct`) a lowball: either is called out
//     (`callout`), the way its style would; one well under what it is worth
//     (`bargainPct`) is snapped up, not haggled.
// --------------------------------------------------------------------------

#include <array>
#include <cstdint>

namespace Haggle
{
    constexpr uint32_t kQualities = 7;   // poor .. heirloom

    struct Settings
    {
        // What something is worth with nothing on the auction house to go by,
        // as a percentage of what a vendor pays for it: gear by quality,
        // everything else in one figure.
        std::array<uint32_t, kQualities> gearValuePct{ 100, 250, 500, 1000, 2500, 4000, 4000 };
        uint32_t goodsValuePct = 300;

        uint32_t ripOffPct  = 250;   // an ask this far over what it is worth: taking the mickey
        uint32_t bargainPct = 50;    // an ask this far under: a bargain, snapped up
        uint32_t lowballPct = 40;    // a bid this far under: a lowball
    };

    // A chat style's way with money (OllamaChat.Trade.Style.<KEY>).
    struct Style
    {
        uint32_t pay     = 100;   // percent of what it is worth it pays at most (selling: takes at least 10000/pay)
        uint32_t haggle  = 40;    // percent chance it haggles a price it would take anyway; how far it opens off
        uint32_t rounds  = 2;     // counter-offers before its last word
        uint32_t callout = 40;    // percent chance it calls out a rip-off or a lowball
    };

    // One of it, by itself: `vendor` is what a vendor pays for one.
    uint64_t Value(Settings const& s, uint64_t vendor, uint32_t quality, bool gear);

    // A price the way people say it, near `copper`: rounded down for an offer,
    // up for an ask ("1g 20s", not "1g 23s 7c").
    uint64_t Nice(uint64_t copper, bool up);

    enum class Move : uint8_t
    {
        Ask,       // buying, no price named: how much?
        Take,      // takes the price named
        Counter,   // offers another price
        Final,     // ...as far as it goes
        Pass       // no deal
    };

    struct Decision
    {
        Move     move    = Move::Pass;
        uint64_t price   = 0;       // Take: theirs; Counter, Final: its own; Pass: the most (least) it would
        bool     ripOff  = false;   // their ask is far over what it is worth
        bool     bargain = false;   // ...far under
        bool     lowball = false;   // their bid is far under what it is worth
    };

    // A bot buying from someone. `worth` and `limit` are for the whole lot.
    struct Buyer
    {
        uint64_t worth    = 0;
        uint64_t limit    = 0;   // the most it pays
        uint64_t last     = 0;   // its last offer, 0 for none yet
        uint32_t counters = 0;
    };

    // What it says to their asking price (`ask` 0: none named). `roll` is a
    // number in [0, 100): under the style's `haggle`, it haggles.
    Decision BuyerAnswer(Settings const& s, Style const& style, Buyer& b, uint64_t ask, uint32_t roll);

    // They said no to its offer, with no new price: it comes up, or that was
    // its last.
    Decision BuyerRaise(Style const& style, Buyer& b);

    // A bot selling to someone. `worth` and `floor` are for the whole lot.
    struct Seller
    {
        uint64_t worth    = 0;
        uint64_t floor    = 0;   // the least it takes
        uint64_t last     = 0;   // its last asking price, 0 for none yet
        uint32_t counters = 0;
    };

    // Its asking price, before anyone has bid.
    Decision SellerOpen(Style const& style, Seller& sl);

    // What it says to their bid (0: none named, which is SellerOpen).
    Decision SellerAnswer(Settings const& s, Style const& style, Seller& sl, uint64_t bid, uint32_t roll);

    // They said no to its price, with no bid: it comes down, or that was its
    // last.
    Decision SellerLower(Style const& style, Seller& sl);

    // The most a buyer of this style pays for something worth `worth`, with
    // `spare` to spend; the least a seller of it takes (never under what a
    // vendor pays, `vendor` for the lot).
    uint64_t BuyLimit(Style const& style, uint64_t worth, uint64_t spare);
    uint64_t SellFloor(Style const& style, uint64_t worth, uint64_t vendor);
}

#endif // MOD_OLLAMA_CHAT_HAGGLE_H
