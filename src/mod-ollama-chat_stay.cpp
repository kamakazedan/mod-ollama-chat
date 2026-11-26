#include "mod-ollama-chat_stay.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_world.h"

#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t kHoldMs = 1500;   // its AI held this long, again every world tick it stays

    struct Hold
    {
        uint64_t          to = 0;
        Clock::time_point until{};
        bool              spoke = false;
    };
    std::unordered_map<uint64_t, Hold> g_holds;   // by bot
    StayStats                          g_stats;

    // Who is about to answer whom: noted from whichever thread sent the line
    // to be written, acted on by the world tick.
    std::mutex                                 g_askedMutex;
    std::vector<std::pair<uint64_t, uint64_t>> g_asked;   // (bot, to), under g_askedMutex
    constexpr size_t                           kMaxAsked = 256;

    float Reach()
    {
        return g_SayDistance > 0.0f ? g_SayDistance : 30.0f;
    }

    // On its own and free to stand still: not fighting, travelling, in the
    // air or in anyone's group.
    bool CanStay(Player* bot)
    {
        return bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() && !bot->duel && !bot->IsInFlight() &&
               !bot->IsFalling() && !bot->GetVehicle() && !bot->GetTransport() && !bot->IsBeingTeleported() &&
               !bot->GetGroup() && !bot->InBattleground() && !bot->InArena();
    }

    void Begin(Player* bot, Player* to)
    {
        if (!bot || !to || bot == to || !OllamaIsBotPlayer(bot) || OllamaIsBotPlayer(to))
            return;
        if (!CanStay(bot) || !to->IsInWorld() || bot->GetMap() != to->GetMap() || !bot->IsWithinDistInMap(to, Reach()))
            return;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || ai->GetMaster())
            return;   // someone's own bot does as it is told

        const uint64_t key = bot->GetGUID().GetRawValue();
        const bool fresh = !g_holds.contains(key);
        Hold& h = g_holds[key];
        h.to    = to->GetGUID().GetRawValue();
        h.until = Clock::now() + std::chrono::seconds(g_StayMaxSeconds);
        h.spoke = false;
        if (fresh)
            ++g_stats.stayed;

        if (bot->isMoving())
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }
        if (g_StayFace && g_EnableBotFacing && IsSafeForFacing(bot))
            bot->SetFacingToObject(to);
        ai->YieldThread(bot, kHoldMs);
        Hangout_Busy(bot, g_StayMaxSeconds);
    }
}

void Stay_Begin(uint64_t bot, uint64_t to)
{
    if (!g_Enable || !g_StayEnable || !bot || !to || bot == to)
        return;
    std::lock_guard<std::mutex> lock(g_askedMutex);
    if (g_asked.size() < kMaxAsked)
        g_asked.emplace_back(bot, to);
}

void Stay_Spoke(Player* bot)
{
    if (!bot)
        return;
    auto it = g_holds.find(bot->GetGUID().GetRawValue());
    if (it == g_holds.end())
        return;
    if (!it->second.spoke)
        ++g_stats.spoke;
    it->second.spoke = true;
    // A moment more, in case they say something back.
    it->second.until = Clock::now() + std::chrono::seconds(g_StayLingerSeconds);
    Hangout_Busy(bot, g_StayLingerSeconds);
}

void Stay_Update()
{
    std::vector<std::pair<uint64_t, uint64_t>> asked;
    {
        std::lock_guard<std::mutex> lock(g_askedMutex);
        asked.swap(g_asked);
    }
    if (g_StayEnable)
        for (const auto& [bot, to] : asked)
            Begin(ObjectAccessor::FindConnectedPlayer(ObjectGuid(bot)),
                  ObjectAccessor::FindConnectedPlayer(ObjectGuid(to)));

    if (g_holds.empty())
        return;
    const auto now = Clock::now();
    for (auto it = g_holds.begin(); it != g_holds.end();)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->first));
        Player* to  = ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->second.to));
        PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        const bool fighting = bot && bot->IsInWorld() && bot->IsInCombat();
        const bool keep = g_StayEnable && ai && now < it->second.until && to && to->IsInWorld() && CanStay(bot) &&
                          bot->GetMap() == to->GetMap() && bot->IsWithinDistInMap(to, Reach() * 1.5f);
        if (!keep)
        {
            if (fighting)
            {
                ++g_stats.fights;
                if (ai)
                    ai->SetNextCheckDelay(0);   // it has a fight to see to, now
            }
            else if (!it->second.spoke)
                ++g_stats.gaveUp;
            it = g_holds.erase(it);
            continue;
        }
        ai->YieldThread(bot, kHoldMs);
        ++it;
    }
}

StayStats Stay_GetStats()
{
    StayStats s = g_stats;
    s.now = uint32_t(g_holds.size());
    return s;
}
