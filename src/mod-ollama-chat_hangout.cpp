#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "Timer.h"

#include "Event.h"
#include "NewRpgInfo.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <list>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr float    kTwoPi      = 6.2831853f;
    constexpr uint32_t kPointId    = 0x48474F;   // MovePoint id for the last few steps
    constexpr uint32_t kHoldMs     = 5000;       // the bot's own AI waits this long; renewed every tick, so a slow tick is no gap
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
        Arriving,   // fetched from elsewhere: teleported in, not landed yet
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
        uint8_t           seat = 0;           // the stand state sitting left it in (a chair's is its own)
        Clock::time_point started{};
        Clock::time_point approachStart{};
        Clock::time_point settledAt{};
        Clock::time_point until{};
        Clock::time_point nextLook{};
        Clock::time_point cameBack{};         // was AFK, then talked to
        Clock::time_point landedAt{};         // fetched: when it was first seen in the city
        Clock::time_point nextTry{};          // ...and when it next looks for room
        bool              broughtIn = false;  // fetched from elsewhere (Hangout.BringIn)...
        uint32_t          fromMap   = 0;      // ...from here
        float             fromX = 0.0f, fromY = 0.0f, fromZ = 0.0f, fromO = 0.0f;
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

    // Where the bots fetched for a spot arrive while someone is in the city:
    // an inn near it, the way people come in by hearthstone. Worked out once
    // per spot from the creature spawns.
    struct Inn
    {
        float    x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
        float    dist    = 0.0f;
        uint32_t faction = 0;   // the innkeeper's faction template
    };
    struct SpotInns
    {
        uint32_t         map = 0;
        float            x = 0.0f, y = 0.0f;   // the spot they were found for
        bool             known = false;
        std::vector<Inn> inns;                 // nearest first
    };
    std::unordered_map<std::string, SpotInns> g_spotInns;

    std::deque<Clock::time_point>  g_fetchTimes;   // bots fetched in the last minute
    std::unordered_map<std::string, Clock::time_point> g_spotNextFetch;   // a spot with no room fetches again from then
    std::unordered_map<uint64_t, Clock::time_point>    g_cityNextFetch;   // a city someone is in takes its next arrival from then
    std::unordered_set<uint64_t>   g_watched;      // (map << 32 | zone) with a real player in it, as of the last look

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

    uint64_t WatchKey(uint32_t map, uint32_t zone)
    {
        return (uint64_t(map) << 32) | zone;
    }

    // A city whose side the bot is on: an Alliance capital takes Alliance
    // bots, a Horde one Horde bots, Shattrath and Dalaran anyone.
    bool TeamFits(Player* bot, uint32_t zone)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
        if (!area)
            return false;
        if (area->team == AREATEAM_ALLY)
            return bot->GetTeamId() == TEAM_ALLIANCE;
        if (area->team == AREATEAM_HORDE)
            return bot->GetTeamId() == TEAM_HORDE;
        return true;
    }

    // Outland's and Northrend's cities are for the levels that go there:
    // Shattrath from 58, Dalaran from 68. (Silvermoon and the Exodar are on
    // the Outland map too, but they are starting cities.)
    uint32_t MinLevelFor(const HangoutSpotConfig& s, uint32_t zone)
    {
        constexpr uint32_t kSilvermoonCity = 3487, kTheExodar = 3557;
        uint32_t level = g_HangoutBringInMinLevel;
        if (s.map == 571)
            level = std::max<uint32_t>(level, 68);
        else if (s.map == 530 && zone != kSilvermoonCity && zone != kTheExodar)
            level = std::max<uint32_t>(level, 58);
        return level;
    }

    // The inns within Hangout.ArrivalRange of a spot, in its zone, nearest
    // first. Empty until the spot's map is up.
    const std::vector<Inn>& InnsFor(const HangoutSpotConfig& s, uint32_t zone)
    {
        SpotInns& known = g_spotInns[s.key];
        if (known.known && known.map == s.map && known.x == s.x && known.y == s.y)
            return known.inns;
        known = SpotInns{};
        Map* base = sMapMgr->FindBaseMap(s.map);
        if (!base || !zone)
            return known.inns;   // looked for again once it is up
        known.map   = s.map;
        known.x     = s.x;
        known.y     = s.y;
        known.known = true;
        if (g_HangoutArrivalRange <= 0.0f)
            return known.inns;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (data.mapid != s.map || std::fabs(data.posZ - s.z) > kMaxClimb)
                continue;
            const float dist = std::hypot(data.posX - s.x, data.posY - s.y);
            if (dist > g_HangoutArrivalRange)
                continue;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(data.id);
            if (!info || !((info->npcflag | data.npcflag) & UNIT_NPC_FLAG_INNKEEPER))
                continue;
            if (base->GetZoneId(PHASEMASK_NORMAL, data.posX, data.posY, data.posZ) != zone)
                continue;
            Inn inn;
            inn.x       = data.posX;
            inn.y       = data.posY;
            inn.z       = data.posZ;
            inn.o       = data.orientation;
            inn.dist    = dist;
            inn.faction = info->faction;
            known.inns.push_back(inn);
        }
        std::sort(known.inns.begin(), known.inns.end(), [](const Inn& a, const Inn& b) { return a.dist < b.dist; });
        if (known.inns.empty())
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] City hangouts: no inn within {:.0f} yards of {}; bots are fetched for it only "
                     "while nobody is in the city.",
                     g_HangoutArrivalRange, s.name);
        return known.inns;
    }

    // The nearest of a spot's inns that is friendly to this bot (else one
    // that is at least not hostile): nobody hearths into the other side's inn.
    Inn const* InnFor(Player* bot, const std::vector<Inn>& inns)
    {
        FactionTemplateEntry const* mine = bot->GetFactionTemplateEntry();
        if (!mine)
            return nullptr;
        Inn const* fallback = nullptr;
        for (const Inn& inn : inns)
        {
            FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(inn.faction);
            if (!theirs || mine->IsHostileTo(*theirs) || theirs->IsHostileTo(*mine))
                continue;
            if (mine->IsFriendlyTo(*theirs) && theirs->IsFriendlyTo(*mine))
                return &inn;
            if (!fallback)
                fallback = &inn;
        }
        return fallback;
    }

    // A real player anywhere near a place, on its map: they would see a bot
    // appear there, or vanish.
    bool RealPlayerNear(uint32_t map, float x, float y, float z, float range)
    {
        for (Player* p : sRandomPlayerbotMgr.GetPlayers())
            if (p && p->IsInWorld() && p->GetMapId() == map && OllamaIsRealPlayer(p) &&
                p->GetExactDistSq(x, y, z) < range * range)
                return true;
        return false;
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
            if (!(type & PATHFIND_NORMAL) ||
                (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)))
                continue;
            // Where the path really ends: beside a crate, not on it.
            G3D::Vector3 const& end = path.GetActualEndPosition();
            if (std::hypot(end.x - x, end.y - y) > 1.0f || std::fabs(end.z - z) > 0.5f)
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

    // In a queue (a dungeon, a battleground): a proposal or an invite could
    // come at any moment, and its AI has to answer it.
    bool Queued(Player* bot)
    {
        return bot->InBattlegroundQueue() || sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE;
    }

    // Something wants the bot's own AI right now: an invite to answer (a
    // group, a guild, an arena team), a trade, a duel, a queue, a fight. A
    // held AI would see none of it.
    bool Wanted(Player* bot)
    {
        return !bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() || bot->GetGroupInvite() ||
               bot->GetTradeData() || bot->duel || bot->GetGuildIdInvited() || bot->GetArenaTeamIdInvited() ||
               Queued(bot);
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

    // A fetched bot goes back where it came from once it is done, if nobody
    // would see it vanish here or turn up there: so a city nobody is in does
    // not fill up with bots that are done hanging out in it. False: it stays,
    // and goes about its business from here like anyone else.
    bool SendBack(Player* bot, PlayerbotAI* ai, const Visit& v)
    {
        if (!ai || !bot->IsInWorld() || bot->IsBeingTeleported() || Wanted(bot) || bot->GetVehicle() ||
            RealPlayerNear(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 150.0f) ||
            RealPlayerNear(v.fromMap, v.fromX, v.fromY, v.fromZ, 150.0f))
            return false;
        bot->GetMotionMaster()->Clear();
        ai->Reset(true);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        if (!bot->TeleportTo(v.fromMap, v.fromX, v.fromY, v.fromZ, v.fromO))
            return false;
        ++g_stats.sentBack;
        return true;
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

    // ---- bots fetched from elsewhere (Hangout.BringIn) --------------------------

    bool StillFree(Player* bot, const Visit& v);
    void Arrived(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now);

    // A bot that may be fetched from wherever it is, for this spot: one of
    // playerbots' random bots, on its own, wanted by nobody, of the city's
    // side and of a level that goes there, with no real player near enough to
    // see it go. (The cheap checks -- alive, out of a fight, not grouped, not
    // in an instance, on a boat or a flight -- are made before.)
    bool CanBringIn(Player* bot, PlayerbotAI* ai, const HangoutSpotConfig& s, uint32_t zone)
    {
        if (!ai || !ai->IsBotAI() || ai->GetMaster() || !sRandomPlayerbotMgr.IsRandomBot(bot) ||
            !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) || Wanted(bot))
            return false;
        if (bot->GetLevel() < MinLevelFor(s, zone) || !TeamFits(bot, zone))
            return false;
        return !RealPlayerNear(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 150.0f);
    }

    // Off it goes: to an inn in the city while someone is in it (it walks
    // over from there, like anyone who has just hearthed in), straight to the
    // spot while nobody is.
    bool BringIn(Player* bot, PlayerbotAI* ai, const HangoutSpotConfig& s, uint32_t zone, bool watched,
                 const Clock::time_point now)
    {
        float x = s.x, y = s.y, z = s.z, o = Frand(0.0f, kTwoPi);
        Map* base = sMapMgr->FindBaseMap(s.map);
        if (!base)
            return false;
        if (watched)
        {
            Inn const* inn = InnFor(bot, InnsFor(s, zone));
            if (!inn)
                return false;   // nowhere to come in that a player would not see it appear
            // A step out from behind the counter, if there is floor there.
            x = inn->x;
            y = inn->y;
            z = inn->z;
            o = inn->o;
            const float sx = inn->x + 1.5f * std::cos(inn->o), sy = inn->y + 1.5f * std::sin(inn->o);
            const float sz = base->GetHeight(PHASEMASK_NORMAL, sx, sy, inn->z + 2.0f, true, 6.0f);
            if (sz > INVALID_HEIGHT && std::fabs(sz - inn->z) < 0.6f)
            {
                x = sx;
                y = sy;
                z = sz;
            }
        }
        else
        {
            if (RealPlayerNear(s.map, x, y, z, 150.0f))
                return false;   // over the zone line from someone after all
            const float ground = base->GetHeight(PHASEMASK_NORMAL, x, y, z + 2.0f, true, 6.0f);
            if (ground > INVALID_HEIGHT && std::fabs(ground - z) < 3.0f)
                z = ground;
        }
        z += 0.05f;

        Visit v;
        v.spotKey   = s.key;
        v.spotName  = s.name;
        v.map       = s.map;
        v.cx        = s.x;
        v.cy        = s.y;
        v.cz        = s.z;
        v.radius    = s.radius;
        v.phase     = Phase::Arriving;
        v.started   = now;
        v.broughtIn = true;
        v.fromMap   = bot->GetMapId();
        v.fromX     = bot->GetPositionX();
        v.fromY     = bot->GetPositionY();
        v.fromZ     = bot->GetPositionZ();
        v.fromO     = bot->GetOrientation();
        // Back on the ground (or the water) it was over, not in mid-air.
        const float floor = bot->GetMapWaterOrGroundLevel(v.fromX, v.fromY, v.fromZ);
        if (floor > INVALID_HEIGHT && v.fromZ - floor > 1.0f && v.fromZ - floor < 50.0f)
            v.fromZ = floor + 0.05f;

        // The way playerbots sends a random bot somewhere itself.
        bot->GetMotionMaster()->Clear();
        ai->Reset(true);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        if (!bot->TeleportTo(s.map, x, y, z, o))
            return false;
        g_visits[bot->GetGUID().GetRawValue()] = std::move(v);
        g_fetchTimes.push_back(now);
        ++g_stats.broughtIn;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} is fetched to hang out at {} ({}).", bot->GetName(), s.name,
                     watched ? "at the inn" : "nobody about");
        return true;
    }

    // Landed: to its place, walking over from the inn while someone is in the
    // city, straight there while nobody is. False: it is not staying after all.
    bool Arrive(Player* bot, PlayerbotAI* ai, Visit& v, HangoutSpotConfig const& s, const Clock::time_point now)
    {
        const uint32_t zone = ZoneOf(s);
        if (bot->GetMapId() != v.map || !zone || bot->GetZoneId() != zone || !StillFree(bot, v))
        {
            ++g_stats.gaveUp;
            Let(bot, ai, v, false, now);
            return false;
        }
        if (v.landedAt == Clock::time_point{})
            v.landedAt = now;
        if (now - v.landedAt < std::chrono::seconds(1) || now < v.nextTry)
            return true;   // a moment to land, or to look again
        if (!PickPlace(bot, s, v.x, v.y, v.z))
        {
            v.nextTry = now + std::chrono::seconds(1);
            if (now - v.landedAt < std::chrono::seconds(20))
                return true;   // everywhere taken just now: another look in a moment
            ++g_stats.gaveUp;
            g_spotNextFetch[v.spotKey] = now + std::chrono::minutes(2);   // nowhere to put them: not for a while
            Let(bot, ai, v, false, now);
            if (!SendBack(bot, ai, v) && g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} found no room at {}.", bot->GetName(), v.spotName);
            return false;
        }
        if (g_watched.count(WatchKey(v.map, zone)))
        {
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            ai->rpgInfo.ChangeToGoCamp(WorldPosition(v.map, v.x, v.y, v.z));
            v.phase   = Phase::Travel;
            v.started = now;
            return true;
        }
        Arrived(bot, ai, v, now);
        return true;
    }

    // Spots short of bots (Hangout.BringInFill of their room) get one more
    // each, from elsewhere, up to Hangout.BringInPerMinute in all -- the
    // cities someone is in first.
    void BringInSome(const Clock::time_point now,
                     const std::vector<std::pair<HangoutSpotConfig const*, uint32_t>>& open,
                     std::vector<Player*>& fetchable)
    {
        while (!g_fetchTimes.empty() && now - g_fetchTimes.front() > std::chrono::seconds(60))
            g_fetchTimes.pop_front();
        if (g_fetchTimes.size() >= g_HangoutBringInPerMinute || fetchable.empty())
            return;
        uint32_t budget = g_HangoutBringInPerMinute - static_cast<uint32_t>(g_fetchTimes.size());

        std::unordered_map<std::string, uint32_t> there;
        for (const auto& [guid, v] : g_visits)
            ++there[v.spotKey];
        std::vector<std::pair<HangoutSpotConfig const*, uint32_t>> wanting, others;
        for (const auto& [s, zone] : open)
        {
            const uint32_t target = (s->capacity * g_HangoutBringInFill + 99) / 100;
            if (there[s->key] >= target)
                continue;
            auto next = g_spotNextFetch.find(s->key);
            if (next != g_spotNextFetch.end() && now < next->second)
                continue;
            (g_watched.count(WatchKey(s->map, zone)) ? wanting : others).emplace_back(s, zone);
        }
        wanting.insert(wanting.end(), others.begin(), others.end());

        for (const auto& [s, zone] : wanting)
        {
            if (!budget || fetchable.empty())
                break;
            const bool watched = g_watched.count(WatchKey(s->map, zone)) != 0;
            if (watched && InnsFor(*s, zone).empty())
                continue;   // no inn to come in at: only while nobody is there
            if (watched)
            {
                auto next = g_cityNextFetch.find(WatchKey(s->map, zone));
                if (next != g_cityNextFetch.end() && now < next->second)
                    continue;   // someone just came in at the inn there
            }
            for (int tries = 0; tries < 12 && !fetchable.empty(); ++tries)
            {
                const size_t i = std::uniform_int_distribution<size_t>(0, fetchable.size() - 1)(Rng());
                Player* bot = fetchable[i];
                fetchable[i] = fetchable.back();
                fetchable.pop_back();
                if (g_visits.count(bot->GetGUID().GetRawValue()))
                    continue;   // just sent to a spot in its own town
                PlayerbotAI* ai = AiOf(bot);
                if (!CanBringIn(bot, ai, *s, zone))
                    continue;
                if (BringIn(bot, ai, *s, zone, watched, now))
                {
                    --budget;
                    // Someone in the city sees them come in at its inns: not a
                    // stream of them, one every so often, whichever spot.
                    if (watched)
                        g_cityNextFetch[WatchKey(s->map, zone)] = now + std::chrono::seconds(8);
                    break;
                }
            }
        }
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
        g_watched.clear();
        for (Player* p : real)
            g_watched.insert(WatchKey(p->GetMapId(), p->GetZoneId()));
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

        const bool fetching = g_HangoutBringIn && g_HangoutBringInFill > 0 && g_HangoutBringInPerMinute > 0;
        std::vector<Player*> fetchable;   // could be fetched from where they are, as far as the cheap checks go
        std::unordered_map<std::string, std::vector<Player*>> willing;
        for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
        {
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() ||
                bot->IsInFlight() || bot->GetVehicle() || bot->IsBeingTeleported() ||
                bot->InBattleground() || bot->InArena())
                continue;
            if (fetching && !bot->GetTransport() && bot->GetMap() && !bot->GetMap()->Instanceable() &&
                !bot->IsFlying() && !bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING) &&
                !g_visits.count(bot->GetGUID().GetRawValue()))
            {
                auto cool = g_cooldown.find(bot->GetGUID().GetRawValue());
                if (cool == g_cooldown.end() || now >= cool->second)
                    fetchable.push_back(bot);
            }
            if (bot->isAFK())
                continue;   // (one fetched is no matter: that flag is playerbots', for a bot nobody is near)

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
            // Wanted() covers a queue too: one waiting on a dungeon or a
            // battleground is not parked where it cannot answer the call.
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

        // Not enough idle bots in town: some from elsewhere.
        if (fetching)
            BringInSome(now, open, fetchable);
    }

    // Still a bot that may be hanging out at all.
    bool StillFree(Player* bot, const Visit& v)
    {
        return !Wanted(bot) && bot->GetMapId() == v.map && !bot->IsInFlight() && !bot->GetVehicle();
    }

    void Arrived(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now);

    // On its way. False: it is not going there after all.
    bool Travel(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        const bool free = StillFree(bot, v);
        if (!free || now - v.started > std::chrono::seconds(g_HangoutTravelSeconds))
        {
            ++g_stats.gaveUp;
            Let(bot, ai, v, free, now);   // timed out: on with something else
            if (free && v.broughtIn)
                SendBack(bot, ai, v);     // (fetched, and stuck on the way: back where it was, if nobody sees)
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
        if (there)
            Arrived(bot, ai, v, now);
        return true;
    }

    // Here (or in the city, for one fetched while nobody is about). It rests
    // for as long as it hangs out, its own AI held from now on, and how it
    // spends it is settled where it can see the place.
    void Arrived(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
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
    }

    void Settle(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        if (v.posture != Posture::Mounted && bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);   // off the horse: it is sitting or standing here
        else if (v.posture == Posture::Mounted && !bot->IsMounted())
            // Up on its best mount now, by playerbots' own pick: a bot fetched
            // for a city nobody is in has an AI too idle to do it by itself.
            ai->DoSpecificAction("check mount state", Event(), true);

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
        v.seat = bot->getStandState();
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
            if (v.cameBack == Clock::time_point{})
            {
                if (v.wantsAfk)
                {
                    // (Already flagged: playerbots marks a bot nobody is near
                    // AFK. Ours now, to take off when it is done.)
                    if (!bot->isAFK())
                        bot->ToggleAFK();
                    v.afkSet = bot->isAFK();
                }
                else if (bot->isAFK())
                    bot->ToggleAFK();   // that flag, on one fetched from where nobody was: not AFK here
            }
        }
        else
        {
            // A world tick long enough for its AI to slip through once stands
            // it up and takes its AFK off: both go back.
            if ((v.posture == Posture::Sit || v.posture == Posture::Bench) && !bot->IsSitState() && !bot->isMoving() &&
                v.seat)
                bot->SetStandState(v.seat);
            if (v.afkSet && !bot->isAFK())
                bot->ToggleAFK();
        }
        ai->SetNextCheckDelay(kHoldMs);
        return true;
    }

    // Every Hangout.CheckSeconds while it is there. False: it is done here.
    bool Stay(Player* bot, PlayerbotAI* ai, Visit& v, const Clock::time_point now)
    {
        const bool  onBench = v.posture == Posture::Bench;
        const float fromOwn = onBench ? bot->GetExactDist2d(v.bx, v.by) : bot->GetExactDist2d(v.x, v.y);
        const bool  moved   = bot->GetMapId() != v.map ||
                              (bot->GetExactDist2d(v.cx, v.cy) > v.radius + 15.0f && fromOwn > 10.0f);
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
            const bool back = v.broughtIn && SendBack(bot, ai, v);
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} is done hanging out at {}{}.", bot->GetName(),
                         v.spotName, back ? ", and goes back where it came from" : "");
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
        if (v.phase == Phase::Arriving && now - v.started > std::chrono::seconds(60))
        {
            // Never landed (or not where it was sent): nothing of this to undo.
            ++g_stats.gaveUp;
            g_cooldown[it->first] = now + std::chrono::minutes(g_HangoutCooldownMinutes);
            it = g_visits.erase(it);
            continue;
        }
        if (!bot->IsInWorld() || bot->IsBeingTeleported())
        {
            ++it;   // between maps: looked at again once it lands
            continue;
        }

        HangoutSpotConfig const* spot = SpotByKey(v.spotKey);
        bool keep = spot != nullptr;   // a spot taken out of the conf: everyone there goes
        if (!keep)
            Let(bot, ai, v, true, now);
        else if (v.phase == Phase::Arriving)
            keep = Arrive(bot, ai, v, *spot, now);
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
    for (auto it = g_spotNextFetch.begin(); it != g_spotNextFetch.end();)
        it = (now >= it->second) ? g_spotNextFetch.erase(it) : std::next(it);
    for (auto it = g_cityNextFetch.begin(); it != g_cityNextFetch.end();)
        it = (now >= it->second) ? g_cityNextFetch.erase(it) : std::next(it);
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

bool Hangout_WhereAndDoing(Player* bot, std::string& spot, std::string& doing)
{
    if (!bot || g_visits.empty())
        return false;
    auto it = g_visits.find(bot->GetGUID().GetRawValue());
    if (it == g_visits.end() || it->second.phase != Phase::Settled)
        return false;
    spot  = it->second.spotName;
    doing = Doing(it->second, bot);
    return true;
}

HangoutStats Hangout_GetStats()
{
    HangoutStats s = g_stats;
    s.spots = static_cast<uint32_t>(g_HangoutSpots.size());
    for (const auto& [key, known] : g_spotInns)
        if (known.known && !known.inns.empty())
            ++s.withInn;
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
