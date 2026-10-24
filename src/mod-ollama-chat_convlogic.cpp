#include "mod-ollama-chat_convlogic.h"

#include <algorithm>

namespace
{
    uint32_t Roll(std::mt19937& rng)
    {
        return std::uniform_int_distribution<uint32_t>(0, 99)(rng);
    }

    uint32_t Between(uint32_t lo, uint32_t hi, std::mt19937& rng)
    {
        if (hi < lo)
            std::swap(lo, hi);
        return std::uniform_int_distribution<uint32_t>(lo, hi)(rng);
    }

    const ConvLogic::Seat* FindSeat(const std::vector<ConvLogic::Seat>& seats, uint64_t guid)
    {
        if (!guid)
            return nullptr;
        for (const ConvLogic::Seat& s : seats)
            if (s.guid == guid)
                return &s;
        return nullptr;
    }
}

namespace ConvLogic
{
    PickResult PickNextSpeaker(const PickInput& in, std::mt19937& rng)
    {
        PickResult out;

        auto usable = [&](const Seat* s)
        {
            return s && s->guid != in.lastSpeaker && s->misses < in.maxMisses;
        };

        // Spoken to by name: they answer.
        if (const Seat* s = FindSeat(in.seats, in.named); usable(s) && Roll(rng) < 85)
        {
            out.guid = s->guid;
            return out;
        }

        // A person answered one of the bots: that bot answers back.
        if (in.lastWasHuman)
        {
            if (const Seat* s = FindSeat(in.seats, in.humanAddressee); usable(s) && Roll(rng) < 85)
            {
                out.guid = s->guid;
                return out;
            }
        }

        // Somebody new pipes up.
        if (in.canJoin && in.joinChancePct > 0 && Roll(rng) < in.joinChancePct)
        {
            out.join = true;
            return out;
        }

        std::vector<const Seat*> pool;
        std::vector<uint32_t> weights;
        for (const Seat& s : in.seats)
        {
            if (!usable(&s))
                continue;

            uint32_t w = 10;
            if (s.guid == in.beforeLast)
                w += 15;                       // back and forth
            if (s.lines == 0)
                w += 10;                       // has not had a say yet
            if (s.lastMs < 0)
                w += 5;
            else if (in.nowMs > s.lastMs)
                w += uint32_t(std::min<int64_t>(10, (in.nowMs - s.lastMs) / 10000));   // quiet for a while
            if (s.misses > 0)
                w = std::max<uint32_t>(2, w / 3);  // just passed: less keen

            pool.push_back(&s);
            weights.push_back(w);
        }

        if (pool.empty())
        {
            out.join = in.canJoin;   // nobody left to answer: someone new, or the end
            return out;
        }

        std::discrete_distribution<size_t> pick(weights.begin(), weights.end());
        out.guid = pool[pick(rng)]->guid;
        return out;
    }

    uint32_t NextGapMs(uint32_t minMs, uint32_t maxMs, double progress,
                       uint32_t lullChancePct, uint32_t lullMinMs, uint32_t lullMaxMs,
                       std::mt19937& rng)
    {
        if (lullChancePct > 0 && lullMaxMs > 0 && Roll(rng) < lullChancePct)
            return Between(lullMinMs, lullMaxMs, rng);

        uint32_t gap = Between(minMs, maxMs, rng);

        // Up to half as long again by the end: an old conversation slows down.
        progress = std::clamp(progress, 0.0, 1.0);
        return uint32_t(double(gap) * (1.0 + 0.5 * progress));
    }
}
