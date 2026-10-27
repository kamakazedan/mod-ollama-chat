#include "mod-ollama-chat_revive.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"

#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include "Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

// While a mod-dungeon-clear run is on, it decides how its party is brought
// back (a rez, a wait, or calling the run off), so nothing here releases a
// body it is keeping.
#if __has_include("DcLeaderSignal.h") && __has_include("DcModuleEnable.h")
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#define OLLAMA_REVIVE_DUNGEON_CLEAR 1
#else
#define OLLAMA_REVIVE_DUNGEON_CLEAR 0
#endif

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct DeadBot
    {
        Clock::time_point since{};        // first seen dead
        Clock::time_point lastSeen{};
        uint32_t          quietMs = 0;    // dead with nobody fighting, waiting for a rez
        bool              gaveUp  = false;
        Clock::time_point nextRelease{};  // when to try releasing (again)
        bool              walking = false;  // a ghost on its way to the dungeon
        Clock::time_point walkUntil{};
        Clock::time_point nextEnter{};
        uint32_t          enterTries = 0;
        bool              abandoned  = false;   // the dungeon kept saying no: playerbots' own way now
    };
    std::unordered_map<uint64_t, DeadBot> g_dead;
    ReviveStats g_stats;

    // Who knows a resurrection, looked up once a minute per character.
    struct RezCache
    {
        bool              can = false;
        Clock::time_point until{};
    };
    std::unordered_map<uint64_t, RezCache> g_rezCache;

    bool IsResurrect(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_RESURRECT || effect.Effect == SPELL_EFFECT_RESURRECT_NEW)
                return true;
        return false;
    }

    // Priests, paladins, shamans and druids once they have learned the spell:
    // the spellbook is asked, not the class, so a level 8 priest is no rezzer.
    bool KnowsResurrect(Player* p)
    {
        switch (p->getClass())
        {
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_HUNTER:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
            case CLASS_DEATH_KNIGHT:
                return false;
            default:
                break;
        }

        RezCache& cache = g_rezCache[p->GetGUID().GetRawValue()];
        const Clock::time_point now = Clock::now();
        if (now < cache.until)
            return cache.can;
        cache.until = now + std::chrono::seconds(60);
        cache.can = false;
        for (auto const& [spellId, spell] : p->GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (info && !info->IsPassive() && IsResurrect(info))
            {
                cache.can = true;
                break;
            }
        }
        return cache.can;
    }

    // Someone in the group, alive and on the map the body is on, who could
    // bring it back.
    bool SomeoneCanRez(Player* dead, Group* group, uint32_t mapId)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != dead && member->IsInWorld() && member->IsAlive() &&
                member->GetMapId() == mapId && KnowsResurrect(member))
                return true;
        }
        return false;
    }

    bool AnyoneFighting(Group* group, uint32_t mapId)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->IsAlive() && member->GetMapId() == mapId &&
                member->IsInCombat())
                return true;
        }
        return false;
    }

    bool InDungeonClearRun(Player* bot)
    {
#if OLLAMA_REVIVE_DUNGEON_CLEAR
        return DcModule::IsEnabled() && DcLeaderSignal::FindRunOwner(bot) != nullptr;
#else
        (void)bot;
        return false;
#endif
    }

    // Dead and not released yet: release when nobody can rez.
    void LookAtBody(Player* bot, PlayerbotAI* ai, Group* group, DeadBot& d, Clock::time_point now)
    {
        if (InDungeonClearRun(bot))
        {
            d.quietMs = 0;
            return;
        }

        const uint32_t mapId = bot->GetMapId();
        if (SomeoneCanRez(bot, group, mapId))
        {
            if (!AnyoneFighting(group, mapId))
                d.quietMs += 1000;
            if (g_ReviveWaitForRezSeconds == 0 || d.quietMs < g_ReviveWaitForRezSeconds * 1000)
                return;
            if (!d.gaveUp)
            {
                d.gaveUp = true;
                ++g_stats.gaveUp;
            }
        }
        else if (now - d.since < std::chrono::seconds(g_ReviveReleaseDelaySeconds))
            return;

        if (now < d.nextRelease)
            return;
        d.nextRelease = now + std::chrono::seconds(10);
        if (ai->DoSpecificAction("release", Event("ollama revive"), true))
        {
            ++g_stats.released;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} released: nobody in the group can resurrect.",
                         bot->GetName());
        }
    }

    // A ghost. Its body in a dungeon or raid it is not in: run from the
    // graveyard to the entrance and walk in, which brings it back there.
    // Returns true once it has gone in.
    bool LookAtGhost(Player* bot, PlayerbotAI* ai, DeadBot& d, Clock::time_point now)
    {
        if (d.abandoned)
            return false;

        const uint32_t corpseMap = bot->GetCorpseLocation().GetMapId();
        MapEntry const* entry = sMapStore.LookupEntry(corpseMap);
        if (!entry || !entry->IsDungeon() || entry->IsBattlegroundOrArena() || corpseMap == bot->GetMapId())
            return false;   // the body is out here: playerbots walks it back

        AreaTriggerTeleport const* portal = sObjectMgr->GetGoBackTrigger(corpseMap);
        if (portal && portal->target_mapId != bot->GetMapId())
            portal = nullptr;   // not on this continent: nowhere to walk to

        if (!d.walking)
        {
            d.walking   = true;
            d.walkUntil = now + std::chrono::seconds(std::max<uint32_t>(10, g_ReviveRunBackSeconds));
            if (portal)
            {
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, portal->target_X, portal->target_Y, portal->target_Z);
            }
        }

        // Its own corpse run cannot reach a body inside, and gives up at the
        // spirit healer instead. Keep it to this walk.
        ai->SetNextCheckDelay(1500);

        const bool there = portal && bot->GetDistance(portal->target_X, portal->target_Y, portal->target_Z) < 10.0f;
        if ((!there && now < d.walkUntil) || now < d.nextEnter)
            return false;

        AreaTriggerTeleport const* inside = sObjectMgr->GetMapEntranceTrigger(corpseMap);
        if (inside && bot->TeleportTo(inside->target_mapId, inside->target_X, inside->target_Y, inside->target_Z,
                                      inside->target_Orientation))
        {
            ++g_stats.returned;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} ran back into {}.", bot->GetName(),
                         entry->name[0] ? entry->name[0] : "its dungeon");
            return true;
        }

        // Shut while a raid boss is being fought, for one: try again shortly.
        d.nextEnter = now + std::chrono::seconds(10);
        if (++d.enterTries >= 12)
            d.abandoned = true;
        return false;
    }
}

void Revive_Update(uint32_t diff)
{
    if (!g_ReviveEnable)
        return;
    static uint32_t timer = 0;
    timer += diff;
    if (timer < 1000)
        return;
    timer = 0;

    const Clock::time_point now = Clock::now();
    std::unordered_set<uint32_t> looked;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* human = entry.second;
        if (!human || !human->IsInWorld() || OllamaIsBotPlayer(human))
            continue;
        Group* group = human->GetGroup();
        if (!group || !looked.insert(group->GetGUID().GetCounter()).second)
            continue;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* bot = ref->GetSource();
            if (!bot || !OllamaIsBotPlayer(bot))
                continue;
            const uint64_t guid = bot->GetGUID().GetRawValue();
            if (bot->IsAlive())
            {
                g_dead.erase(guid);
                continue;
            }
            if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->InBattleground() || bot->InArena())
                continue;
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (!ai)
                continue;

            DeadBot& d = g_dead[guid];
            if (d.since == Clock::time_point{})
                d.since = now;
            d.lastSeen = now;

            if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                LookAtBody(bot, ai, group, d, now);
            else if (LookAtGhost(bot, ai, d, now))
                g_dead.erase(guid);
        }
    }

    for (auto it = g_dead.begin(); it != g_dead.end();)
        it = (now - it->second.lastSeen > std::chrono::minutes(2)) ? g_dead.erase(it) : std::next(it);
    if (g_rezCache.size() > 2000)
        g_rezCache.clear();
}

bool Revive_RunDecides(Player* bot)
{
    return bot && InDungeonClearRun(bot);
}

ReviveStats Revive_GetStats()
{
    return g_stats;
}
