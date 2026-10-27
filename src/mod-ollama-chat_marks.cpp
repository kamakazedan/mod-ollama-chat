#include "mod-ollama-chat_marks.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"

#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"

#include "AiObjectContext.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr uint8 ICON_STAR  = 0;
    constexpr uint8 ICON_CROSS = 6;
    constexpr uint8 ICON_SKULL = 7;
    constexpr uint8 ICON_COUNT = 8;
    constexpr uint8 ICON_LAST_CC = 5;   // star .. square: marked to be controlled

    constexpr uint32 SPELL_STEALTH = 1784;
    constexpr uint32 SAP_RANKS[] = { 51724, 11297, 2070, 6770 };   // best first

    MarksStats g_stats;

    struct GroupMarks
    {
        ObjectGuid        icons[ICON_COUNT];
        bool              known = false;
        Clock::time_point marking{};    // last change somebody else made
        Clock::time_point lastSeen{};
        ObjectGuid        liveSkull;    // the skull's mob, last time it was alive
    };
    std::unordered_map<uint32_t, GroupMarks> g_groups;   // by the group's guid counter

    // A rogue on its way to sap the star.
    struct Sap
    {
        uint64_t          rogue  = 0;
        uint64_t          target = 0;
        Clock::time_point until{};
        Clock::time_point nextMove{};
    };
    std::vector<Sap> g_saps;

    Player* Find(uint64_t guid)
    {
        return guid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)) : nullptr;
    }

    bool HasCcStrategy(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_HUNTER:
            case CLASS_PALADIN:
            case CLASS_ROGUE:
            case CLASS_DRUID:
            case CLASS_WARLOCK:
                return true;
            default:
                return false;
        }
    }

    // The mark this bot's class controls, and playerbots' crowd control on.
    void AssignClassIcon(Player* bot)
    {
        const uint8 cls = bot->getClass();
        if (cls >= MAX_CLASSES || g_MarksClassIcon[cls].empty())
            return;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        AiObjectContext* ctx = ai ? ai->GetAiObjectContext() : nullptr;
        if (!ctx)
            return;
        if (Value<std::string>* rti = ctx->GetValue<std::string>("rti cc"))
        {
            if (rti->Get() != g_MarksClassIcon[cls])
            {
                rti->Set(g_MarksClassIcon[cls]);
                ++g_stats.assigned;
            }
        }
        // Never for a tank: a bear dropping out of form to hibernate something
        // is why playerbots leaves crowd control off for them.
        if (g_MarksAddCcStrategy && HasCcStrategy(cls) && !PlayerbotAI::IsTank(bot) &&
            !ai->HasStrategy("cc", BOT_STATE_COMBAT))
            ai->ChangeStrategy("+cc", BOT_STATE_COMBAT);
    }

    Unit* LiveMark(Player* ref, ObjectGuid guid)
    {
        if (guid.IsEmpty())
            return nullptr;
        Unit* unit = ObjectAccessor::GetUnit(*ref, guid);
        return (unit && unit->IsInWorld() && unit->IsAlive()) ? unit : nullptr;
    }

    bool MarkedToControl(const GroupMarks& m, ObjectGuid guid)
    {
        for (uint8 i = 0; i <= ICON_LAST_CC; ++i)
            if (m.icons[i] == guid)
                return true;
        return false;
    }

    // Is this mob in a fight with someone in the group (or their pet)?
    bool FightingGroup(Unit* mob, Group* group, Player* ref)
    {
        if (!mob->IsInCombat())
            return false;
        for (GroupReference* r = group->GetFirstMember(); r; r = r->next())
        {
            Player* member = r->GetSource();
            if (!member || !member->IsInWorld() || member->GetMap() != ref->GetMap())
                continue;
            if (mob->IsEngagedBy(member))
                return true;
            if (Pet* pet = member->GetPet())
                if (mob->IsEngagedBy(pet))
                    return true;
        }
        return false;
    }

    // The nearest mob still fighting the group that nobody marked to control
    // and nothing is controlling.
    Unit* NextLooseMob(Player* ref, Group* group, const GroupMarks& m)
    {
        Player* anchor = ref;
        for (GroupReference* r = group->GetFirstMember(); r; r = r->next())
            if (Player* member = r->GetSource())
                if (member->IsAlive() && member->GetMap() == ref->GetMap() && PlayerbotAI::IsTank(member))
                {
                    anchor = member;
                    break;
                }

        Unit* best = nullptr;
        float bestDist = 0.0f;
        std::unordered_set<ObjectGuid> seen;
        for (GroupReference* r = group->GetFirstMember(); r; r = r->next())
        {
            Player* member = r->GetSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMap() != ref->GetMap())
                continue;
            for (auto const& [guid, threat] : member->GetThreatMgr().GetThreatenedByMeList())
            {
                Unit* mob = threat ? threat->GetOwner() : nullptr;
                if (!mob || !seen.insert(mob->GetGUID()).second)
                    continue;
                if (!mob->IsInWorld() || !mob->IsAlive() || !mob->IsInCombat() || mob->GetTypeId() != TYPEID_UNIT ||
                    MarkedToControl(m, mob->GetGUID()) || mob->HasBreakableByDamageCrowdControlAura() ||
                    mob->IsPolymorphed() || mob->HasAuraType(SPELL_AURA_SCHOOL_IMMUNITY) ||
                    !ref->IsValidAttackTarget(mob))
                    continue;
                const float dist = anchor->GetDistance(mob);
                if (!best || dist < bestDist)
                {
                    best = mob;
                    bestDist = dist;
                }
            }
        }
        return best;
    }

    uint32 SapSpell(Player* rogue)
    {
        for (uint32 id : SAP_RANKS)
            if (rogue->HasSpell(id))
                return id;
        return 0;
    }

    bool IsSapped(Unit* target)
    {
        for (uint32 id : SAP_RANKS)
            if (target->HasAura(id))
                return true;
        return false;
    }

    // The star went on a mob nobody is fighting: the nearest rogue bot that
    // can, sneaks over and saps it.
    void StartSap(Player* ref, Group* group, ObjectGuid starGuid, Clock::time_point now)
    {
        Creature* target = ObjectAccessor::GetCreature(*ref, starGuid);
        if (!target || !target->IsAlive() || target->IsInCombat() || IsSapped(target))
            return;
        switch (target->GetCreatureType())
        {
            case CREATURE_TYPE_HUMANOID:
            case CREATURE_TYPE_BEAST:
            case CREATURE_TYPE_DEMON:
            case CREATURE_TYPE_DRAGONKIN:
                break;
            default:
                return;   // sap does not work on it
        }

        Player* rogue = nullptr;
        float bestDist = 40.0f;
        for (GroupReference* r = group->GetFirstMember(); r; r = r->next())
        {
            Player* member = r->GetSource();
            if (!member || member->getClass() != CLASS_ROGUE || !OllamaIsBotPlayer(member) || !member->IsInWorld() ||
                !member->IsAlive() || member->IsInCombat() || member->IsMounted() ||
                member->GetMap() != target->GetMap() || !SapSpell(member))
                continue;
            const float dist = member->GetDistance(target);
            if (dist < bestDist)
            {
                rogue = member;
                bestDist = dist;
            }
        }
        if (!rogue)
            return;

        const uint64_t rogueGuid = rogue->GetGUID().GetRawValue();
        g_saps.erase(std::remove_if(g_saps.begin(), g_saps.end(), [&](const Sap& s) { return s.rogue == rogueGuid; }),
                     g_saps.end());
        g_saps.push_back(Sap{ rogueGuid, target->GetGUID().GetRawValue(), now + std::chrono::seconds(15), {} });
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} goes to sap {} (star).", rogue->GetName(), target->GetName());
    }

    void UpdateSaps(Clock::time_point now)
    {
        for (auto it = g_saps.begin(); it != g_saps.end();)
        {
            Player* rogue = Find(it->rogue);
            PlayerbotAI* ai = rogue ? PlayerbotsMgr::instance().GetPlayerbotAI(rogue) : nullptr;
            Creature* target = (rogue && rogue->IsInWorld()) ? ObjectAccessor::GetCreature(*rogue, ObjectGuid(it->target))
                                                             : nullptr;
            Group* group = rogue ? rogue->GetGroup() : nullptr;
            const uint32 sap = rogue ? SapSpell(rogue) : 0;

            bool done = !rogue || !ai || !target || !group || !sap || now >= it->until || !rogue->IsAlive() ||
                        !target->IsAlive() || rogue->IsInCombat() || target->IsInCombat() ||
                        rogue->GetMap() != target->GetMap() || group->GetTargetIcon(ICON_STAR) != target->GetGUID() ||
                        IsSapped(target);
            if (!done)
            {
                // The walk is this, not its own idea of where to be.
                ai->SetNextCheckDelay(1000);

                if (!rogue->HasStealthAura() && rogue->HasSpell(SPELL_STEALTH) && !rogue->HasSpellCooldown(SPELL_STEALTH))
                    rogue->CastSpell(rogue, SPELL_STEALTH, false);

                SpellInfo const* info = sSpellMgr->GetSpellInfo(sap);
                const float reach = info ? std::max(2.0f, info->GetMaxRange(false, rogue) - 1.0f) : 4.0f;
                if (rogue->IsWithinDistInMap(target, reach))
                {
                    rogue->GetMotionMaster()->Clear();
                    rogue->StopMoving();
                    rogue->SetFacingToObject(target);
                    if (rogue->CastSpell(target, sap, false) == SPELL_CAST_OK)
                        ++g_stats.saps;
                    else
                        ++g_stats.sapFails;
                    done = true;
                }
                else if (now >= it->nextMove)
                {
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    target->GetNearPoint(rogue, x, y, z, rogue->GetObjectSize(), std::max(0.5f, reach - 1.5f),
                                         target->GetAngle(rogue));
                    rogue->GetMotionMaster()->MovePoint(0, x, y, z);
                    it->nextMove = now + std::chrono::seconds(1);
                }
            }
            it = done ? g_saps.erase(it) : std::next(it);
        }
    }

    void LookAtGroup(Player* ref, Group* group, Clock::time_point now)
    {
        GroupMarks& m = g_groups[group->GetGUID().GetCounter()];
        m.lastSeen = now;

        // Somebody marking? A change this did not make.
        bool changed = false;
        ObjectGuid newStar;
        for (uint8 i = 0; i < ICON_COUNT; ++i)
        {
            const ObjectGuid icon = group->GetTargetIcon(i);
            if (m.known && icon != m.icons[i])
            {
                changed = true;
                if (i == ICON_STAR && !icon.IsEmpty())
                    newStar = icon;
            }
            m.icons[i] = icon;
        }
        if (m.known && changed)
            m.marking = now;
        m.known = true;
        const bool marking = m.marking != Clock::time_point{} &&
                             now - m.marking <= std::chrono::seconds(g_MarksInUseSeconds);

        for (GroupReference* r = group->GetFirstMember(); r; r = r->next())
            if (Player* member = r->GetSource())
                if (member->IsInWorld() && OllamaIsBotPlayer(member))
                    AssignClassIcon(member);

        if (g_MarksSapStar && !newStar.IsEmpty())
            StartSap(ref, group, newStar, now);

        // The skull moves on when its mob dies, while somebody is marking: to
        // the cross, else to the nearest loose mob -- each only when it is in
        // the fight already. A cross on the next pack is "that one next", not
        // "pull it": nothing starts a fight but somebody putting the skull on.
        if (Unit* skull = LiveMark(ref, m.icons[ICON_SKULL]))
        {
            m.liveSkull = skull->GetGUID();
            return;
        }
        if (m.liveSkull.IsEmpty())
            return;
        Unit* was = ObjectAccessor::GetUnit(*ref, m.liveSkull);
        const bool died = !was || !was->IsAlive();
        m.liveSkull.Clear();
        if (!died || !marking || !ref->IsAlive())
            return;   // taken off a living mob: theirs to put somewhere

        Unit* next = LiveMark(ref, m.icons[ICON_CROSS]);
        if (next && !FightingGroup(next, group, ref))
            next = nullptr;
        if (next)
            ++g_stats.moved;
        else if (g_MarksAutoSkull)
        {
            next = NextLooseMob(ref, group, m);
            if (next)
                ++g_stats.picked;
        }
        if (!next)
            return;
        group->SetTargetIcon(ICON_SKULL, ref->GetGUID(), next->GetGUID());
        for (uint8 i = 0; i < ICON_COUNT; ++i)
            m.icons[i] = group->GetTargetIcon(i);   // ours: not somebody marking
    }
}

void Marks_Update(uint32_t diff)
{
    if (!g_MarksEnable)
        return;

    const Clock::time_point now = Clock::now();
    if (!g_saps.empty())
        UpdateSaps(now);

    static uint32_t timer = 0;
    timer += diff;
    if (timer < 500)
        return;
    timer = 0;

    std::unordered_set<uint32_t> looked;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld() || OllamaIsBotPlayer(player))
            continue;
        Group* group = player->GetGroup();
        if (!group || !looked.insert(group->GetGUID().GetCounter()).second)
            continue;
        Map* map = player->GetMap();
        if (!map || map->IsBattlegroundOrArena())
            continue;
        LookAtGroup(player, group, now);
    }

    for (auto it = g_groups.begin(); it != g_groups.end();)
        it = (now - it->second.lastSeen > std::chrono::minutes(10)) ? g_groups.erase(it) : std::next(it);
}

MarksStats Marks_GetStats()
{
    return g_stats;
}
