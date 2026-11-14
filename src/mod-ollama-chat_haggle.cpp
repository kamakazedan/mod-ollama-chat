#include "mod-ollama-chat_haggle.h"

#include <algorithm>

namespace Haggle
{
    namespace
    {
        // How far it opens off its limit, or its floor: half its `haggle`, so
        // a style that always haggles (100) opens at half its limit, or half
        // as much again over its floor.
        uint64_t Off(uint64_t copper, uint32_t haggle, bool up)
        {
            const uint64_t h = std::min<uint32_t>(haggle, 100);
            return up ? copper * (200 + h) / 200 : copper * (200 - h) / 200;
        }

        Move CounterMove(Style const& style, uint32_t counters)
        {
            return counters >= style.rounds ? Move::Final : Move::Counter;
        }
    }

    uint64_t Value(Settings const& s, uint64_t vendor, uint32_t quality, bool gear)
    {
        const uint32_t pct = gear ? s.gearValuePct[std::min(quality, kQualities - 1)] : s.goodsValuePct;
        return vendor * pct / 100;
    }

    uint64_t Nice(uint64_t copper, bool up)
    {
        uint64_t step = 1;
        if (copper >= 1000000)
            step = 50000;   // 5g steps from 100g
        else if (copper >= 100000)
            step = 10000;   // whole gold from 10g
        else if (copper >= 10000)
            step = 1000;    // 10s steps from 1g
        else if (copper >= 1000)
            step = 100;     // whole silver from 10s
        else if (copper >= 100)
            step = 10;      // 10c steps from 1s
        return up ? (copper + step - 1) / step * step : copper / step * step;
    }

    uint64_t BuyLimit(Style const& style, uint64_t worth, uint64_t spare)
    {
        return std::min<uint64_t>(worth * style.pay / 100, spare);
    }

    uint64_t SellFloor(Style const& style, uint64_t worth, uint64_t vendor)
    {
        const uint64_t own = worth * 100 / std::max<uint32_t>(style.pay, 1);
        return std::max<uint64_t>(own, vendor * 11 / 10);   // under that, a vendor pays more
    }

    Decision BuyerAnswer(Settings const& s, Style const& style, Buyer& b, uint64_t ask, uint32_t roll)
    {
        Decision d;
        if (ask && b.worth)
        {
            d.ripOff  = ask * 100 >= b.worth * s.ripOffPct;
            d.bargain = ask * 100 <= b.worth * s.bargainPct;
        }
        const bool haggles = roll < style.haggle;
        const uint64_t top = Nice(b.limit, false);

        if (!ask)
        {
            // Asked again with nothing named: its offer stands.
            if (b.last)
            {
                d.move  = CounterMove(style, b.counters);
                d.price = b.last;
                return d;
            }
            // Nothing named: a haggler names its own, the rest ask.
            if (haggles && style.rounds > 0 && top)
            {
                const uint64_t p = style.rounds > 1 ? Nice(Off(b.limit, style.haggle, false), false) : top;
                if (p)
                {
                    b.last = p;
                    ++b.counters;
                    d.move  = CounterMove(style, b.counters);
                    d.price = p;
                    return d;
                }
            }
            d.move = Move::Ask;
            return d;
        }

        // No more than it offered already: taken, never haggled down again.
        if (b.last && ask <= b.last)
        {
            d.move  = Move::Take;
            d.price = ask;
            return d;
        }

        if (ask <= b.limit)
        {
            // A price it would pay: taken, unless it haggles first -- once,
            // and never over a bargain.
            if (!d.bargain && haggles && b.counters == 0 && style.rounds > 0)
            {
                const uint64_t p = Nice(Off(ask, style.haggle, false), false);
                if (p > b.last && p < ask)
                {
                    b.last = p;
                    ++b.counters;
                    d.move  = CounterMove(style, b.counters);
                    d.price = p;
                    return d;
                }
            }
            d.move  = Move::Take;
            d.price = ask;
            return d;
        }

        // More than it pays: it offers what it would, coming up a step at a
        // time to its limit, which is its last word.
        if (b.counters < style.rounds && top > b.last)
        {
            const uint32_t counters = b.counters + 1;
            uint64_t p = 0;
            if (counters >= style.rounds)
                p = top;
            else if (!b.last)
                p = Nice(Off(b.limit, style.haggle, false), false);
            else
                p = Nice(b.last + (b.limit - b.last) / 2, false);
            if (p <= b.last)
                p = top;
            if (p > b.last && p)
            {
                b.last     = p;
                b.counters = counters;
                d.move     = p >= top ? Move::Final : CounterMove(style, b.counters);
                d.price    = p;
                return d;
            }
        }
        d.move  = Move::Pass;
        d.price = b.last ? b.last : top;
        return d;
    }

    Decision BuyerRaise(Style const& style, Buyer& b)
    {
        Decision d;
        const uint64_t top = Nice(b.limit, false);
        if (b.counters < style.rounds && top > b.last)
        {
            const uint32_t counters = b.counters + 1;
            uint64_t p = counters >= style.rounds ? top : Nice(b.last + (b.limit - b.last) / 2, false);
            if (p <= b.last)
                p = top;
            if (p > b.last)
            {
                b.last     = p;
                b.counters = counters;
                d.move     = p >= top ? Move::Final : CounterMove(style, b.counters);
                d.price    = p;
                return d;
            }
        }
        d.move  = Move::Pass;
        d.price = b.last ? b.last : top;
        return d;
    }

    Decision SellerOpen(Style const& style, Seller& sl)
    {
        Decision d;
        if (!sl.floor)
            return d;   // no idea what to ask: no sale
        const uint64_t p = std::max(Nice(Off(sl.floor, style.haggle, true), true), Nice(sl.floor, true));
        sl.last = p;
        d.move  = Move::Counter;
        d.price = p;
        return d;
    }

    Decision SellerAnswer(Settings const& s, Style const& style, Seller& sl, uint64_t bid, uint32_t roll)
    {
        if (!bid)
            return SellerOpen(style, sl);
        Decision d;
        if (!sl.floor)
            return d;
        if (!sl.last)
            SellerOpen(style, sl);
        d.lowball = sl.worth && bid * 100 <= sl.worth * s.lowballPct;
        const bool haggles = roll < style.haggle;
        const uint64_t bottom = Nice(sl.floor, true);

        if (bid >= sl.last)
        {
            d.move  = Move::Take;
            d.price = bid;
            return d;
        }
        if (bid >= sl.floor)
        {
            // A bid it would take: taken, unless it haggles first -- once.
            if (haggles && sl.counters == 0 && style.rounds > 0)
            {
                const uint64_t p = Nice(bid + (sl.last - bid) / 2, true);
                if (p > bid && p < sl.last)
                {
                    sl.last = p;
                    ++sl.counters;
                    d.move  = CounterMove(style, sl.counters);
                    d.price = p;
                    return d;
                }
            }
            d.move  = Move::Take;
            d.price = bid;
            return d;
        }

        // Under the least it takes: it comes down a step at a time to its
        // floor, which is its last word.
        if (sl.counters < style.rounds && sl.last > bottom)
        {
            const uint32_t counters = sl.counters + 1;
            uint64_t p = counters >= style.rounds ? bottom : Nice(sl.last - (sl.last - sl.floor) / 2, true);
            if (p >= sl.last)
                p = bottom;
            if (p < sl.last)
            {
                sl.last     = p;
                sl.counters = counters;
                d.move      = p <= bottom ? Move::Final : CounterMove(style, sl.counters);
                d.price     = p;
                return d;
            }
        }
        d.move  = Move::Pass;
        d.price = sl.last;
        return d;
    }

    Decision SellerLower(Style const& style, Seller& sl)
    {
        Decision d;
        if (!sl.floor)
            return d;
        if (!sl.last)
            return SellerOpen(style, sl);
        const uint64_t bottom = Nice(sl.floor, true);
        if (sl.counters < style.rounds && sl.last > bottom)
        {
            const uint32_t counters = sl.counters + 1;
            uint64_t p = counters >= style.rounds ? bottom : Nice(sl.last - (sl.last - sl.floor) / 2, true);
            if (p >= sl.last)
                p = bottom;
            if (p < sl.last)
            {
                sl.last     = p;
                sl.counters = counters;
                d.move      = p <= bottom ? Move::Final : CounterMove(style, sl.counters);
                d.price     = p;
                return d;
            }
        }
        d.move  = Move::Pass;
        d.price = sl.last;
        return d;
    }
}
