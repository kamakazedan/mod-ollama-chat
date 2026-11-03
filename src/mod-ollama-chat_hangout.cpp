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
#include "MapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
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

    constexpr float    kTwoPi      = 6.2831853f;
    constexpr uint32_t kPointId    = 0x48474F;   // MovePoint id for the last few steps
    constexpr uint32_t kHoldMs     = 1000;       // the bot's own AI waits this long; renewed every tick
    constexpr float    kMaxClimb   = 30.0f;      // a bot this far above or below a spot is on another level
    constexpr auto     kMountGrace = std::chrono::seconds(10);

    // Set while a bench is used here, so the module's own "used an object"
    // event chatter (mod-ollama-chat_events) leaves it alone.
    thread_local bool t_usingBench = false;

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
        float             x      = 0.0f;   // its place in the spot
        float             y      = 0.0f;
        float             z      = 0.0f;
        float             cx     = 0.0f;   // the spot's middle
        float             cy     = 0.0f;
        float             cz     = 0.0f;
        float             radius = 0.0f;
        Phase             phase    = Phase::Travel;
        Posture           posture  = Posture::Stand;
        bool              wantsAfk = false;
        bool              afkSet   = false;   // the AFK flag is this module's
        bool              holding  = false;   // its playerbots AI is held: nothing undoes the sitting or the AFK
        ObjectGuid        bench;              // a bench it is going to, or sat on
        float             bx = 0.0f, by = 0.0f, bz = 0.0f;
        Clock::time_point started{};
        Clock::time_point approachStart{};
        Clock::time_point settledAt{};
        Clock::time_point until{};
        Clock::time_point nextLook{};
        Clock::time_point cameBack{};         // was AFK, then talked to
    };

    std::unordered_map<uint64_t, Visit>             g_visits;     // by bot guid
    std::unordered_map<uint64_t, Clock::time_point> g_cooldown;   // bot -> may hang out again from
    HangoutStats                                    g_stats;

    // Which zone each spot is in, worked out once from the map.
    struct SpotZone
    {
        uint32_t map  = 0;
        float    x    = 0.0f;
        float    y    = 0.0f;
        uint32_t zone = 0;
    };
    std::unordered_map<std::string, SpotZone> g_spotZones;

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

    // The zone a spot is in (the Undercity, not the Ruins of Lordaeron over
    // it; Dalaran, not Crystalsong under it). 0 while its map is not up.
    uint32_t ZoneOf(const HangoutSpotConfig& s)
    {
        SpotZone& known = g_spotZones[s.key];
        if (known.zone && known.map == s.map && known.x == s.x && known.y == s.y)
            return known.zone;
        Map* map = sMapMgr->FindBaseMap(s.map);
        const uint32_t zone = map ? map->GetZoneId(PHASEMASK_NORMAL, s.x, s.y, s.z) : 0;
        known = SpotZone{ s.map, s.x, s.y, zone };
        return zone;
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
    // of the water, not where another bot stands -- and one you can walk to
    // from the middle, so not the top of a crate or a market stall.
    bool PickPlace(Player* bot, const HangoutSpotConfig& s, float& outX, float& outY, float& outZ)
    {
        Map* map = bot->GetMap();
        const uint32_t phaseMask = bot->GetPhaseMask();
        int paths = 0;
        for (int tries = 0; tries < 16 && paths < 4; ++tries)
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

            ++paths;
            PathGenerator path(bot);
            path.CalculatePath(s.x, s.y, s.z, x, y, z, false);
            const uint32_t type = path.GetPathType();
            if (!(type & PATHFIND_NORMAL) || (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT)))
                continue;
            G3D::Vector3 const& end = path.GetEndPosition();
            if (std::hypot(end.x - x, end.y - y) > 1.0f || std::fabs(end.z - z) > 1.5f)
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

    // Something wants the bot's own AI right now: an invite to answer, a
    // trade, a duel, a fight. A held AI would see none of it.
    bool Wanted(Player* bot)
    {
        return !bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() || bot->GetGroupInvite() ||
               bot->GetTradeData() || bot->duel;
    }

    // Done with it: whatever this changed goes back, and its AI runs again
    // now. `moveOn`: its time there is up, so it goes and does something else.
    void Let(Player* bot, PlayerbotAI* ai, Visit& v, bool moveOn, const Clock::time_point now)
    {
        if (!bot)
            return;
        if (v.afkSet && bot->isAFK())
            bot->ToggleAFK();
        v.afkSet = false;
        if (v.phase == Phase::Settled && (v.posture == Posture::Sit || v.posture == Posture::Bench) &&
            bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        if (ai)
        {
            if (v.holding || v.phase != Phase::Travel)
                ai->SetNextCheckDelay(0);
            if (moveOn)
            {
                NewRpgInfo& info = ai->rpgInfo;
                if (std::holds_alternative<NewRpgInfo::Rest>(info.data) ||
                    std::holds_alternative<NewRpgInfo::GoCamp>(info.data))
                    info.ChangeToIdle();
            }
        }
        v.holding = false;
        g_cooldown[bot->GetGUID().GetRawValue()] = now + std::chrono::minutes(g_HangoutCooldownMinutes);
    }

    bool IdleInRpg(PlayerbotAI* ai)
    {
        const auto& data = ai->rpgInfo.data;
        return std::holds_alternative<NewRpgInfo::Idle>(data) || std::holds_alternative<NewRpgInfo::Rest>(data) ||
               std::holds_alternative<NewRpgInfo::WanderNpc>(data);
    }

    bool Send(Player* bot, PlayerbotAI* ai, const HangoutSpotConfig& s, const Clock::time_point now)
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
        if (!PickPlace(bot, s, v.x, v.y, v.z))
            return false;   // nowhere that checks out just now: another time

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        ai->rpgInfo.ChangeToGoCamp(WorldPosition(s.map, v.x, v.y, v.z));
        g_visits[bot->GetGUID().GetRawValue()] = std::move(v);
        ++g_stats.sent;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} heads over to hang out at {}.", bot->GetName(), s.name);
        return true;
    }

    void Recruit(const Clock::time_point now)
    {
        if (g_HangoutSpots.empty())
            return;

        // Nobody online to see it: nothing to do.
        std::vector<Player*> real;
        for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
            if (p && p->IsInWorld() && OllamaIsRealPlayer(p))
                real.push_back(p);
        if (real.empty())
            return;

        std::unordered_map<std::string, uint32_t> there;   // spot -> bots there or on the way
        for (const auto& [guid, v] : g_visits)
            ++there[v.spotKey];
        std::vector<std::pair<HangoutSpotConfig const*, uint32_t>> open;   // spots with room, and their zone
        for (const HangoutSpotConfig& s : g_HangoutSpots)
            if (s.capacity > there[s.key])
                if (const uint32_t zone = ZoneOf(s))
                    open.emplace_back(&s, zone);
        if (open.empty())
            return;

        std::unordered_map<std::string, std::vector<Player*>> willing;
        for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
        {
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() ||
                bot->IsInFlight() || bot->GetVehicle() || bot->IsBeingTeleported() || bot->isAFK() ||
                bot->InBattleground() || bot->InArena())
                continue;

            // The nearest spot with room, in this zone and on this level of it.
            HangoutSpotConfig const* best = nullptr;
            float bestDist = g_HangoutRecruitRange;
            for (const auto& [s, zone] : open)
            {
                if (s->map != bot->GetMapId() || zone != bot->GetZoneId() ||
                    std::fabs(bot->GetPositionZ() - s->z) > kMaxClimb)
                    continue;
                const float d = bot->GetExactDist2d(s->x, s->y);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = s;
                }
            }
            if (!best)
                continue;

            const uint64_t raw = bot->GetGUID().GetRawValue();
            if (g_visits.count(raw))
                continue;
            auto cool = g_cooldown.find(raw);
            if (cool != g_cooldown.end() && now < cool->second)
                continue;
            PlayerbotAI* ai = AiOf(bot);
            if (!ai || !ai->IsBotAI() || ai->GetMaster() || !sRandomPlayerbotMgr.IsRandomBot(bot) ||
                !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) || !IdleInRpg(ai) || Wanted(bot))
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
            std::shuffle(bots.begin(), bots.end(), Rng());
            for (size_t i = 0; i < bots.size() && i < 3; ++i)   // the first that has somewhere to stand
                if (Send(bots[i], AiOf(bots[i]), *s, now))
                    break;
        }
    }

    // Still a bot that may be hanging out at all.
    bool StillFree(Player* bot, const Visit& v)
    {
        return !Wanted(bot) && bot->GetMapId() == v.map && !bot->IsInFlight() && !bot->GetVehicle();
    }

    // On its way. False: it is not going there after all.
    bool Travel(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        const bool free = StillFree(bot, v);
        if (!free || now - v.started > std::chrono::seconds(g_HangoutTravelSeconds))
        {
            ++g_stats.gaveUp;
            Let(bot, ai, v, free, now);   // timed out: on with something else
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

        // Here. It rests for as long as it hangs out, its own AI held from now
        // on, and how it spends it is settled where it can see the place.
        ai->rpgInfo.ChangeToRest();
        ai->SetNextCheckDelay(kHoldMs);
        GameObject* bench = Roll(g_HangoutBenchChance) ? FindBench(bot, v) : nullptr;
        if (bench)
        {
            v.posture = Posture::Bench;
            v.bench   = bench->GetGUID();
            v.bx      = bench->GetPositionX();
            v.by      = bench->GetPositionY();
            v.bz      = bench->GetPositionZ();
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
        if (v.posture == Posture::Bench)
            bot->GetMotionMaster()->MovePoint(kPointId, v.bx, v.by, v.bz);
        else
            bot->GetMotionMaster()->MovePoint(kPointId, v.x, v.y, v.z);
        return true;
    }

    void Settle(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        if (v.posture != Posture::Mounted && bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);   // off the horse: it is sitting or standing here

        if (v.posture == Posture::Bench)
        {
            GameObject* go = ObjectAccessor::GetGameObject(*bot, v.bench);
            if (go && go->isSpawned() && bot->GetExactDist2d(go) < 6.0f)
            {
                t_usingBench = true;
                go->Use(bot);   // the nearest free seat, the way the client does it
                t_usingBench = false;
            }
            if (!bot->IsSitState())
            {
                // Every seat taken after all: back to its own place, to sit there.
                v.posture       = Posture::Sit;
                v.bench         = ObjectGuid::Empty;
                v.approachStart = now;
                bot->GetMotionMaster()->MovePoint(kPointId, v.x, v.y, v.z);
                return;
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

        ai->rpgInfo.startT = getMSTime();
        const uint32_t lo = g_HangoutStayMinutesMin * 60, hi = std::max(lo, g_HangoutStayMinutesMax * 60);
        v.phase     = Phase::Settled;
        v.settledAt = now;
        v.until     = now + std::chrono::seconds(std::uniform_int_distribution<uint32_t>(lo, hi)(Rng()));
        v.nextLook  = now + std::chrono::seconds(g_HangoutCheckSeconds);
        ++g_stats.settled;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} hangs out at {}, {}{}.", bot->GetName(), v.spotName,
                     Doing(v, bot), v.wantsAfk ? ", going AFK" : "");
    }

    // The last steps, its AI held. False: something else came up.
    bool Approach(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        if (!StillFree(bot, v) || !std::holds_alternative<NewRpgInfo::Rest>(ai->rpgInfo.data))
        {
            ++g_stats.cutShort;
            Let(bot, ai, v, false, now);
            return false;
        }
        ai->SetNextCheckDelay(kHoldMs);
        ai->rpgInfo.startT = getMSTime();
        const bool  toBench = v.posture == Posture::Bench;
        const float reach   = toBench ? 3.0f : 1.5f;
        const auto  elapsed = now - v.approachStart;
        const bool  stopped = elapsed > std::chrono::seconds(2) && (!bot->movespline || bot->movespline->Finalized());
        const float dist    = toBench ? bot->GetExactDist2d(v.bx, v.by) : bot->GetExactDist2d(v.x, v.y);
        if (dist <= reach || stopped || elapsed > std::chrono::seconds(12))
            Settle(bot, ai, v, now);
        return true;
    }

    // Every world tick once it is there: its AI held, so playerbots does not
    // stand it up, take its AFK off, or put it back on its horse. Let go at
    // once for anything its AI has to answer. False: it is done here.
    bool Hold(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        if (Wanted(bot))
        {
            ++g_stats.cutShort;
            Let(bot, ai, v, false, now);
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} stops hanging out at {}: wanted.", bot->GetName(),
                         v.spotName);
            return false;
        }
        if (!v.holding)
        {
            // Mounted is left to playerbots, which puts an idle random bot on
            // its best mount; held once it is up there.
            if (v.posture == Posture::Mounted && !bot->IsMounted() && now - v.settledAt < kMountGrace)
                return true;
            v.holding = true;
            if (v.wantsAfk && !bot->isAFK() && v.cameBack == Clock::time_point{})
            {
                bot->ToggleAFK();
                v.afkSet = bot->isAFK();
            }
        }
        ai->SetNextCheckDelay(kHoldMs);
        return true;
    }

    // Every Hangout.CheckSeconds while it is there. False: it is done here.
    bool Stay(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        const bool moved = bot->GetMapId() != v.map ||
                           (bot->GetExactDist2d(v.cx, v.cy) > v.radius + 15.0f && bot->GetExactDist2d(v.x, v.y) > 10.0f);
        if (moved || !std::holds_alternative<NewRpgInfo::Rest>(ai->rpgInfo.data))
        {
            ++g_stats.cutShort;
            Let(bot, ai, v, false, now);
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} stops hanging out at {}: moved or busy.",
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
        ai->rpgInfo.startT = getMSTime();   // playerbots' rest is 30 seconds; it is still resting
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

    for (auto it = g_visits.begin(); it != g_visits.end();)
    {
        Visit& v = it->second;
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->first));
        PlayerbotAI* ai = bot ? AiOf(bot) : nullptr;
        if (!ai)
        {
            it = g_visits.erase(it);   // logged out: nothing to put back
            continue;
        }
        if (!bot->IsInWorld() || bot->IsBeingTeleported())
        {
            ++it;   // between maps: looked at again once it lands
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
            keep = Hold(bot, ai, v, now);
            if (keep && now >= v.nextLook)
            {
                v.nextLook = now + std::chrono::seconds(g_HangoutCheckSeconds);
                keep = Stay(bot, ai, v, now);
            }
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
    if (Wanted(bot))
    {
        // Invited, traded with, attacked: back at the keyboard, and done here.
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

bool Hangout_UsingBench()
{
    return t_usingBench;
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
    const bool away     = v.afkSet && bot->isAFK();
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
