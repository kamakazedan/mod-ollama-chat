#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "Timer.h"

#include "NewRpgInfo.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <list>
#include <random>
#include <unordered_map>
#include <variant>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr float    kTwoPi   = 6.2831853f;
    constexpr uint32_t kPointId = 0x48474F;   // MovePoint id for the last few steps

    enum class Posture : uint8_t
    {
        Mounted,
        Bench,
        Sit,
        Stand
    };

    enum class Phase : uint8_t
    {
        Travel,     // playerbots' "go camp" is taking it there
        Approach,   // the last few steps, to its own place in the crowd
        Settled
    };

    struct Visit
    {
        std::string       spotKey;
        std::string       spotName;
        uint32_t          map    = 0;
        float             x      = 0.0f;   // its place (a bench's, for a bench)
        float             y      = 0.0f;
        float             z      = 0.0f;
        float             cx     = 0.0f;   // the spot's middle
        float             cy     = 0.0f;
        float             cz     = 0.0f;
        float             radius = 0.0f;
        Phase             phase      = Phase::Travel;
        Posture           posture    = Posture::Stand;
        bool              wantsAfk   = false;
        bool              afkSet     = false;   // the AFK flag is this module's
        bool              mountTaken = false;   // this took playerbots' "mount" strategy off
        ObjectGuid        bench;
        Clock::time_point started{};
        Clock::time_point approachStart{};
        Clock::time_point until{};
        Clock::time_point nextLook{};
        Clock::time_point cameBack{};           // was AFK, then talked to
    };

    std::unordered_map<uint64_t, Visit>             g_visits;     // by bot guid
    std::unordered_map<uint64_t, Clock::time_point> g_cooldown;   // bot -> may hang out again from
    HangoutStats                                    g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    float Frand(float lo, float hi)
    {
        return std::uniform_real_distribution<float>(lo, hi)(Rng());
    }

    bool Roll(uint32_t percent)
    {
        return percent > 0 && std::uniform_int_distribution<uint32_t>(1, 100)(Rng()) <= percent;
    }

    PlayerbotAI* AiOf(Player* p)
    {
        return p ? PlayerbotsMgr::instance().GetPlayerbotAI(p) : nullptr;
    }

    HangoutSpotConfig const* SpotByKey(const std::string& key)
    {
        for (const HangoutSpotConfig& spot : g_HangoutSpots)
            if (spot.key == key)
                return &spot;
        return nullptr;
    }

    // Two bots on one spot of ground would stand inside each other.
    bool Taken(const std::string& spotKey, float x, float y, float minDist)
    {
        for (const auto& [guid, v] : g_visits)
            if (v.spotKey == spotKey && std::hypot(v.x - x, v.y - y) < minDist)
                return true;
        return false;
    }

    // A place within the spot: on the ground near the middle's height, in
    // sight of the middle (so not inside a building or behind a wall), out
    // of the water, and not where another bot stands.
    bool PickPlace(Map* map, uint32_t phaseMask, const HangoutSpotConfig& s, float& outX, float& outY, float& outZ)
    {
        for (int tries = 0; tries < 16; ++tries)
        {
            const float angle = Frand(0.0f, kTwoPi);
            const float dist  = s.radius * std::sqrt(Frand(0.0f, 1.0f));
            const float x = s.x + dist * std::cos(angle);
            const float y = s.y + dist * std::sin(angle);
            const float z = map->GetHeight(phaseMask, x, y, s.z + 3.0f, true, g_HangoutHeightTolerance + 6.0f);
            if (z <= INVALID_HEIGHT || std::fabs(z - s.z) > g_HangoutHeightTolerance)
                continue;
            if (map->IsInWater(phaseMask, x, y, z, 2.0f))
                continue;
            if (!map->isInLineOfSight(s.x, s.y, s.z + 2.0f, x, y, z + 2.0f, phaseMask, LINEOFSIGHT_ALL_CHECKS,
                                      VMAP::ModelIgnoreFlags::Nothing))
                continue;
            if (Taken(s.key, x, y, 2.5f))
                continue;
            outX = x;
            outY = y;
            outZ = z;
            return true;
        }
        return false;
    }

    bool IsBench(GameObject* go)
    {
        if (!go || !go->isSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_CHAIR || g_HangoutBenchWords.empty())
            return false;
        std::string name = go->GetName();
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const std::string& word : g_HangoutBenchWords)
            if (!word.empty() && name.find(word) != std::string::npos)
                return true;
        return false;
    }

    // A free bench near the bot, in sight of the spot's middle: fewer of this
    // module's bots on it than it has seats.
    GameObject* FindBench(Player* bot, const Visit& v)
    {
        if (g_HangoutBenchRange <= 0.0f)
            return nullptr;
        std::list<GameObject*> objects;
        Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                            g_HangoutBenchRange);
        Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, g_HangoutBenchRange);

        std::vector<GameObject*> free;
        for (GameObject* go : objects)
        {
            if (!IsBench(go) || std::fabs(go->GetPositionZ() - v.cz) > g_HangoutHeightTolerance)
                continue;
            if (!bot->GetMap()->isInLineOfSight(v.cx, v.cy, v.cz + 2.0f, go->GetPositionX(), go->GetPositionY(),
                                                go->GetPositionZ() + 1.5f, bot->GetPhaseMask(),
                                                LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                continue;
            uint32_t seats = 1;
            if (GameObjectTemplate const* info = go->GetGOInfo())
                seats = std::max<uint32_t>(1, info->chair.slots);
            uint32_t used = 0;
            for (const auto& [guid, other] : g_visits)
                if (other.bench == go->GetGUID())
                    ++used;
            if (used < seats)
                free.push_back(go);
        }
        if (free.empty())
            return nullptr;
        return free[std::uniform_int_distribution<size_t>(0, free.size() - 1)(Rng())];
    }

    bool CanRide(Player* bot, PlayerbotAI* ai)
    {
        return bot->GetLevel() >= sPlayerbotAIConfig.UseGroundMountAtMinLevel &&
               ai->HasStrategy("mount", BOT_STATE_NON_COMBAT) && bot->IsOutdoors();
    }

    const char* Doing(const Visit& v, Player* bot)
    {
        switch (v.posture)
        {
            case Posture::Mounted: return bot->IsMounted() ? "sat on your mount" : "standing about";
            case Posture::Bench:   return "sitting on a bench";
            case Posture::Sit:     return "sitting on the ground";
            default:               return "standing about";
        }
    }

    // Done with it: whatever this changed goes back. `moveOn`: its time there
    // is up, so it goes and does something else now.
    void Let(Player* bot, PlayerbotAI* ai, Visit& v, bool moveOn, const Clock::time_point now)
    {
        if (bot)
        {
            if (v.afkSet && bot->isAFK())
                bot->ToggleAFK();
            v.afkSet = false;
            if (ai && v.mountTaken && !ai->HasStrategy("mount", BOT_STATE_NON_COMBAT))
                ai->ChangeStrategy("+mount", BOT_STATE_NON_COMBAT);
            v.mountTaken = false;
            if (v.phase == Phase::Settled && (v.posture == Posture::Sit || v.posture == Posture::Bench) &&
                bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            if (ai && moveOn)
            {
                NewRpgInfo& info = ai->rpgInfo;
                const bool ours = std::holds_alternative<NewRpgInfo::Rest>(info.data) ||
                                  std::holds_alternative<NewRpgInfo::GoCamp>(info.data);
                if (ours)
                    info.ChangeToIdle();
            }
        }
        if (bot)
            g_cooldown[bot->GetGUID().GetRawValue()] = now + std::chrono::minutes(g_HangoutCooldownMinutes);
    }

    // On a map with no real player anywhere, nobody would see it.
    bool AnyRealPlayer(std::vector<Player*>& real)
    {
        for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
            if (p && p->IsInWorld() && OllamaIsRealPlayer(p))
                real.push_back(p);
        return !real.empty();
    }

    bool IdleInRpg(PlayerbotAI* ai)
    {
        const auto& data = ai->rpgInfo.data;
        return std::holds_alternative<NewRpgInfo::Idle>(data) || std::holds_alternative<NewRpgInfo::Rest>(data) ||
               std::holds_alternative<NewRpgInfo::WanderNpc>(data);
    }

    void Send(Player* bot, PlayerbotAI* ai, const HangoutSpotConfig& s, const Clock::time_point now)
    {
        Visit v;
        v.spotKey  = s.key;
        v.spotName = s.name;
        v.map      = s.map;
        v.cx       = s.x;
        v.cy       = s.y;
        v.cz       = s.z;
        v.radius   = s.radius;
        v.started  = now;
        v.x = s.x;
        v.y = s.y;
        v.z = s.z;
        if (!PickPlace(bot->GetMap(), bot->GetPhaseMask(), s, v.x, v.y, v.z))
        {
            // Nowhere checks out: the middle, a step or two off it.
            v.x = s.x + Frand(-2.0f, 2.0f);
            v.y = s.y + Frand(-2.0f, 2.0f);
            v.z = s.z;
        }

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        ai->rpgInfo.ChangeToGoCamp(WorldPosition(s.map, v.x, v.y, v.z));
        g_visits[bot->GetGUID().GetRawValue()] = std::move(v);
        ++g_stats.sent;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} heads over to hang out at {}.", bot->GetName(), s.name);
    }

    void Recruit(const Clock::time_point now)
    {
        if (g_HangoutSpots.empty())
            return;
        std::vector<Player*> real;
        if (!AnyRealPlayer(real))
            return;

        std::unordered_map<std::string, uint32_t> there;   // spot -> bots there or on the way
        for (const auto& [guid, v] : g_visits)
            ++there[v.spotKey];

        std::unordered_map<std::string, std::vector<Player*>> willing;
        for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
        {
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() ||
                bot->IsInFlight() || bot->GetVehicle() || bot->IsBeingTeleported() || bot->isAFK() ||
                bot->InBattleground() || bot->InArena())
                continue;
            const uint64_t raw = bot->GetGUID().GetRawValue();
            if (g_visits.count(raw))
                continue;
            auto cool = g_cooldown.find(raw);
            if (cool != g_cooldown.end() && now < cool->second)
                continue;

            // The nearest spot with room, on this map.
            HangoutSpotConfig const* best = nullptr;
            float bestDist = g_HangoutRecruitRange;
            for (const HangoutSpotConfig& s : g_HangoutSpots)
            {
                if (s.map != bot->GetMapId() || s.capacity == 0 || there[s.key] >= s.capacity)
                    continue;
                const float d = bot->GetExactDist2d(s.x, s.y);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = &s;
                }
            }
            if (!best)
                continue;

            Map* map = bot->GetMap();
            PlayerbotAI* ai = AiOf(bot);
            if (!map || map->Instanceable() || !ai || !ai->IsBotAI() || ai->GetMaster() ||
                !sRandomPlayerbotMgr.IsRandomBot(bot) || !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) ||
                !IdleInRpg(ai))
                continue;
            // Someone has it targeted: they may be about to talk to it.
            if (std::any_of(real.begin(), real.end(), [&](Player* p) { return p->GetTarget() == bot->GetGUID(); }))
                continue;
            willing[best->key].push_back(bot);
        }

        for (auto& [key, bots] : willing)
        {
            HangoutSpotConfig const* s = SpotByKey(key);
            if (!s || bots.empty() || !Roll(g_HangoutRecruitChance))
                continue;
            Player* bot = bots[std::uniform_int_distribution<size_t>(0, bots.size() - 1)(Rng())];
            Send(bot, AiOf(bot), *s, now);
        }
    }

    // Still a bot that may be hanging out at all.
    bool StillFree(Player* bot, const Visit& v)
    {
        return bot->IsAlive() && !bot->IsInCombat() && !bot->GetGroup() && bot->GetMapId() == v.map &&
               !bot->IsInFlight() && !bot->GetVehicle();
    }

    // On its way. False: it is not going there after all.
    bool Travel(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        if (!StillFree(bot, v) || now - v.started > std::chrono::seconds(g_HangoutTravelSeconds))
        {
            ++g_stats.gaveUp;
            Let(bot, ai, v, StillFree(bot, v), now);   // timed out: on with something else
            return false;
        }

        const float dist = bot->GetExactDist(v.x, v.y, v.z);
        bool there = false;
        const auto& data = ai->rpgInfo.data;
        if (auto* camp = std::get_if<NewRpgInfo::GoCamp>(&data))
        {
            const bool ours = std::fabs(camp->pos.GetPositionX() - v.x) < 0.5f &&
                              std::fabs(camp->pos.GetPositionY() - v.y) < 0.5f;
            if (!ours)
            {
                ++g_stats.gaveUp;
                Let(bot, ai, v, false, now);
                return false;
            }
            there = dist < 10.0f;
        }
        else if (std::holds_alternative<NewRpgInfo::WanderNpc>(data) && dist < 14.0f)
            there = true;   // playerbots' "go camp" arrived, and turned into wandering between NPCs
        else
        {
            ++g_stats.gaveUp;   // something gave it other things to do
            Let(bot, ai, v, false, now);
            return false;
        }
        if (!there)
            return true;

        // Here. It rests (for as long as it hangs out), and how it spends it
        // is settled now, where it can see the place.
        ai->rpgInfo.ChangeToRest();
        GameObject* bench = Roll(g_HangoutBenchChance) ? FindBench(bot, v) : nullptr;
        if (bench)
        {
            v.posture = Posture::Bench;
            v.bench   = bench->GetGUID();
            v.x       = bench->GetPositionX();
            v.y       = bench->GetPositionY();
            v.z       = bench->GetPositionZ();
        }
        else if (CanRide(bot, ai) && Roll(g_HangoutMountedChance))
            v.posture = Posture::Mounted;
        else if (Roll(g_HangoutSitChance))
            v.posture = Posture::Sit;
        else
            v.posture = Posture::Stand;
        v.wantsAfk      = Roll(g_HangoutAfkChance);
        v.phase         = Phase::Approach;
        v.approachStart = now;
        bot->GetMotionMaster()->MovePoint(kPointId, v.x, v.y, v.z);
        return true;
    }

    void Settle(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        if (v.posture != Posture::Mounted)
        {
            // Off the horse, and kept off it while it sits or stands here:
            // playerbots puts an idle random bot back on its mount.
            if (ai->HasStrategy("mount", BOT_STATE_NON_COMBAT))
            {
                ai->ChangeStrategy("-mount", BOT_STATE_NON_COMBAT);
                v.mountTaken = true;
            }
            if (bot->IsMounted())
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }

        if (v.posture == Posture::Bench)
        {
            GameObject* go = ObjectAccessor::GetGameObject(*bot, v.bench);
            if (go && go->isSpawned() && bot->GetExactDist2d(go) < 6.0f)
                go->Use(bot);   // the nearest free seat, the way the client does it
            if (!bot->IsSitState())
            {
                v.posture = Posture::Sit;   // every seat taken: the ground will do
                v.bench   = ObjectGuid::Empty;
            }
        }
        if (v.posture == Posture::Sit)
            bot->SetStandState(UNIT_STAND_STATE_SIT);
        if (v.posture != Posture::Bench)
        {
            // Facing the middle, more or less: people stand about facing each other.
            const float angle = bot->GetExactDist2d(v.cx, v.cy) > 2.0f ? bot->GetAngle(v.cx, v.cy) + Frand(-0.9f, 0.9f)
                                                                     : Frand(0.0f, kTwoPi);
            bot->SetFacingTo(angle);
        }
        if (v.wantsAfk && !bot->isAFK())
        {
            bot->ToggleAFK();
            v.afkSet = bot->isAFK();
        }

        const uint32_t lo = g_HangoutStayMinutesMin * 60, hi = std::max(lo, g_HangoutStayMinutesMax * 60);
        v.phase    = Phase::Settled;
        v.until    = now + std::chrono::seconds(std::uniform_int_distribution<uint32_t>(lo, hi)(Rng()));
        v.nextLook = now + std::chrono::seconds(g_HangoutCheckSeconds);
        ++g_stats.settled;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} hangs out at {}, {}{}.", bot->GetName(), v.spotName,
                     Doing(v, bot), v.afkSet ? ", AFK" : "");
    }

    // The last steps. False: something else came up.
    bool Approach(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        if (!StillFree(bot, v) || !std::holds_alternative<NewRpgInfo::Rest>(ai->rpgInfo.data))
        {
            ++g_stats.cutShort;
            Let(bot, ai, v, false, now);
            return false;
        }
        const float reach   = v.posture == Posture::Bench ? 3.0f : 1.5f;
        const auto  elapsed = now - v.approachStart;
        const bool  stopped = elapsed > std::chrono::seconds(2) && (!bot->movespline || bot->movespline->Finalized());
        if (bot->GetExactDist2d(v.x, v.y) <= reach || stopped || elapsed > std::chrono::seconds(12))
            Settle(bot, ai, v, now);
        return true;
    }

    // Every Hangout.CheckSeconds while it is there. False: it is done here.
    bool Stay(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        const bool moved = bot->GetExactDist2d(v.cx, v.cy) > v.radius + 15.0f && bot->GetExactDist2d(v.x, v.y) > 10.0f;
        if (!StillFree(bot, v) || moved || !std::holds_alternative<NewRpgInfo::Rest>(ai->rpgInfo.data))
        {
            ++g_stats.cutShort;
            Let(bot, ai, v, false, now);
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} stops hanging out at {}: something came up.",
                         bot->GetName(), v.spotName);
            return false;
        }
        if (now >= v.until)
        {
            ++g_stats.movedOn;
            Let(bot, ai, v, true, now);
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} is done hanging out at {}.", bot->GetName(),
                         v.spotName);
            return false;
        }
        // Playerbots' rest is 30 seconds: kept going while it is here.
        ai->rpgInfo.startT = getMSTime();
        // Back on its mount after all (strategies reset): so be it, rather
        // than pull it off again and again.
        if (v.posture != Posture::Mounted && bot->IsMounted())
            v.posture = Posture::Mounted;
        return true;
    }

    void LetAllGo(const Clock::time_point now)
    {
        for (auto& [guid, v] : g_visits)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            if (bot && bot->IsInWorld())
                Let(bot, AiOf(bot), v, true, now);
        }
        g_visits.clear();
    }
}

void Hangout_Update(uint32_t /*diff*/)
{
    const auto now = Clock::now();
    if (!g_Enable || !g_HangoutEnable)
    {
        if (!g_visits.empty())
            LetAllGo(now);
        return;
    }

    // Every tick for the ones on their way: playerbots turns an arrival into
    // wandering between NPCs on the bot's next AI tick, which runs between
    // these. The ones settled are looked at every Hangout.CheckSeconds.
    for (auto it = g_visits.begin(); it != g_visits.end();)
    {
        Visit& v = it->second;
        if (v.phase == Phase::Settled && now < v.nextLook)
        {
            ++it;
            continue;
        }
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->first));
        PlayerbotAI* ai = (bot && bot->IsInWorld()) ? AiOf(bot) : nullptr;
        if (!ai)
        {
            it = g_visits.erase(it);   // logged out: nothing to put back
            continue;
        }
        bool keep = SpotByKey(v.spotKey) != nullptr;   // a spot taken out of the conf: everyone there goes
        if (!keep)
            Let(bot, ai, v, true, now);
        else if (v.phase == Phase::Travel)
            keep = Travel(bot, ai, v, now);
        else if (v.phase == Phase::Approach)
            keep = Approach(bot, ai, v, now);
        else
        {
            v.nextLook = now + std::chrono::seconds(g_HangoutCheckSeconds);
            keep = Stay(bot, ai, v, now);
        }
        it = keep ? std::next(it) : g_visits.erase(it);
    }

    static Clock::time_point nextRecruit{};
    if (now < nextRecruit)
        return;
    nextRecruit = now + std::chrono::seconds(g_HangoutCheckSeconds);
    Recruit(now);

    for (auto it = g_cooldown.begin(); it != g_cooldown.end();)
        it = (now >= it->second) ? g_cooldown.erase(it) : std::next(it);
}

bool Hangout_IsAway(Player* bot)
{
    if (!bot || g_visits.empty())
        return false;
    auto it = g_visits.find(bot->GetGUID().GetRawValue());
    if (it == g_visits.end() || !it->second.afkSet)
        return false;
    if (bot->GetGroup())
    {
        // Invited: back at the keyboard, and done hanging out.
        ++g_stats.cutShort;
        Let(bot, AiOf(bot), it->second, false, Clock::now());
        g_visits.erase(it);
        return false;
    }
    return bot->isAFK();
}

void Hangout_ComeBack(Player* bot)
{
    if (!bot || g_visits.empty())
        return;
    auto it = g_visits.find(bot->GetGUID().GetRawValue());
    if (it == g_visits.end() || !it->second.afkSet)
        return;
    if (bot->isAFK())
        bot->ToggleAFK();
    it->second.afkSet   = false;
    it->second.cameBack = Clock::now();
    ++g_stats.cameBack;
}

std::string Hangout_StateTextFor(Player* bot)
{
    if (!bot || g_visits.empty() || g_HangoutStateText.empty())
        return {};
    auto it = g_visits.find(bot->GetGUID().GetRawValue());
    if (it == g_visits.end() || it->second.phase != Phase::Settled)
        return {};
    const Visit& v = it->second;
    std::string out = SafeFormat(g_HangoutStateText, fmt::arg("spot", v.spotName), fmt::arg("doing", Doing(v, bot)));
    if (out == "[Format Error]")
        out.clear();
    const bool away = v.afkSet && bot->isAFK();
    const bool justBack = v.cameBack != Clock::time_point{} && Clock::now() - v.cameBack < std::chrono::minutes(3);
    if ((away || justBack) && !g_HangoutAwayText.empty())
        out += (out.empty() ? "" : " ") + g_HangoutAwayText;
    return out;
}

HangoutStats Hangout_GetStats()
{
    HangoutStats s = g_stats;
    s.spots = static_cast<uint32_t>(g_HangoutSpots.size());
    for (const auto& [guid, v] : g_visits)
    {
        if (v.phase == Phase::Settled)
        {
            ++s.hanging;
            if (v.afkSet)
                ++s.away;
        }
        else
            ++s.onTheWay;
    }
    return s;
}
