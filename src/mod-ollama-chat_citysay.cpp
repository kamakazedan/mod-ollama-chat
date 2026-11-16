#include "mod-ollama-chat_citysay.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"

#include <chrono>
#include <list>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    std::unordered_map<uint64_t, Clock::time_point> g_heardAt;   // real player -> the last such line they heard
    CitySayStats g_stats;

    bool Applies(Player* bot)
    {
        return g_CitySayGapSeconds > 0 && !g_DisableForSayYell && bot && bot->IsInWorld() && OllamaInCity(bot);
    }

    // The real players who would hear `bot` in /say.
    std::vector<Player*> InEarshot(Player* bot)
    {
        std::vector<Player*> out;
        if (g_SayDistance <= 0.0f)
            return out;
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, g_SayDistance, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, g_SayDistance);
        for (Player* p : found)
            if (p && p != bot && p->IsInWorld() && OllamaIsRealPlayer(p))
                out.push_back(p);
        return out;
    }

    bool FreeFor(const std::vector<Player*>& near, Clock::time_point now)
    {
        const auto gap = std::chrono::seconds(g_CitySayGapSeconds);
        for (Player* p : near)
        {
            auto it = g_heardAt.find(p->GetGUID().GetRawValue());
            if (it != g_heardAt.end() && now - it->second < gap)
                return false;
        }
        return true;
    }

    void Mark(const std::vector<Player*>& near, Clock::time_point now)
    {
        if (g_heardAt.size() >= 256)
        {
            const auto gap = std::chrono::seconds(g_CitySayGapSeconds);
            for (auto it = g_heardAt.begin(); it != g_heardAt.end();)
                it = now - it->second >= gap ? g_heardAt.erase(it) : std::next(it);
        }
        for (Player* p : near)
            g_heardAt[p->GetGUID().GetRawValue()] = now;
    }
}

bool CitySay_Free(Player* bot)
{
    if (!Applies(bot))
        return true;
    return FreeFor(InEarshot(bot), Clock::now());
}

bool CitySay_Claim(Player* bot)
{
    if (!Applies(bot))
        return true;
    const auto now = Clock::now();
    const std::vector<Player*> near = InEarshot(bot);
    if (!FreeFor(near, now))
    {
        ++g_stats.held;
        return false;
    }
    Mark(near, now);
    ++g_stats.started;
    return true;
}

void CitySay_Heard(Player* bot)
{
    if (!Applies(bot))
        return;
    const std::vector<Player*> near = InEarshot(bot);
    if (near.empty())
        return;
    Mark(near, Clock::now());
    ++g_stats.heard;
}

CitySayStats CitySay_GetStats()
{
    return g_stats;
}
