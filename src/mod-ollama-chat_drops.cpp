#include "mod-ollama-chat_drops.h"
#include "mod-ollama-chat_plus.h"

#include <cmath>

double Drops_Pct(uint32_t itemId)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 7
    return PlayerbotsPlus::DropChance(itemId);
#else
    (void)itemId;
    return 0.0;
#endif
}

bool Drops_Sources(uint32_t itemId, DropsFrom& out)
{
    out = {};
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 8
    PlayerbotsPlus::DropSources from;
    if (!PlayerbotsPlus::DropSourcesOf(itemId, from))
        return false;
    out.kinds    = from.kinds;
    out.spawns   = from.spawns;
    out.minLevel = from.minLevel;
    out.maxLevel = from.maxLevel;
    for (size_t i = 0; i < 3; ++i)
    {
        out.creature[i]  = from.creature[i];
        out.chancePct[i] = from.chancePct[i];
    }
    out.bestPct = from.bestPct;
    out.onlyMap = from.onlyMap;
    out.chests  = from.chests;
    return true;
#else
    (void)itemId;
    return false;
#endif
}

bool Drops_SourcesKnown()
{
    return OLLAMA_CHAT_PLAYERBOTS_PLUS >= 8;
}

std::string Drops_OnceIn(double pct)
{
    if (!(pct > 0.0))
        return "0";
    double n = 100.0 / pct;
    if (n >= 100.0)
    {
        const double step = std::pow(10.0, std::floor(std::log10(n)) - 1.0);
        n = std::round(n / step) * step;
    }
    std::string digits = std::to_string(uint64_t(std::llround(n)));
    for (int i = int(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(size_t(i), ",");
    return digits;
}
