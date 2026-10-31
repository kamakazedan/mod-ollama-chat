#include "mod-ollama-chat_marks.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Creature.h"
#include "DBCStructure.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"

#include "AiObjectContext.h"
#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The sap's step-by-step progress goes to INFO with OllamaChat.DebugEnabled,
// to DEBUG otherwise. How each one ended -- sapped, gave up and why -- is
// always INFO (MARKS_SAP_RESULT), so a sap that fails can be read back from
// the log without the debug flag.
#define MARKS_SAP_RESULT(...) LOG_INFO("module.ollamachat", __VA_ARGS__)
#define MARKS_SAP_LOG(...)                                       \
    do                                                           \
    {                                                            \
        if (g_DebugEnabled)                                      \
            LOG_INFO("module.ollamachat", __VA_ARGS__);          \
        else                                                     \
            LOG_DEBUG("module.ollamachat", __VA_ARGS__);         \
    } while (0)

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
    constexpr uint8  SAP_LEVEL   = 10;

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

    // A rogue on its way to sap the star. Stealth first: a rogue walking up
    // without it pulls the mob, so with Stealth still cooling down it keeps
    // following until it can go in. Then round to the mob's back, since a mob
    // only spots stealth in front of it; then stop, face it and sap.
    enum class SapPhase : uint8
    {
        Stealth,    // waiting to go into stealth; the bot's own AI runs meanwhile
        Approach,   // sneaking over
        Settle,     // in reach and stopped: facing it, then the cast
        Check,      // cast: did it take?
    };

    struct Sap
    {
        uint64_t          rogue  = 0;
        uint64_t          target = 0;
        uint64_t          asker  = 0;        // the real player in the group, for what the rogue says
        SapPhase          phase  = SapPhase::Stealth;
        Clock::time_point until{};           // gives up at this point; each phase sets its own
        Clock::time_point nextMove{};        // the walk is planned again at this point
        Clock::time_point moved{};           // last time it was given somewhere to walk
        Clock::time_point castAt{};
        Clock::time_point lastTick{};        // the last update, for how long it has waited
        uint8             tries  = 0;        // facing and range retries, and missed casts
        uint8             shortOf = 0;       // got to where it was going in, and still short of reach
        bool              closer = false;    // the cast said out of range: go right up to it
        bool              waiting = false;   // the mob is wandering: hanging back until it stops
        uint32_t          waitedMs = 0;      // ...this long so far
        const char*       noStealth = "you could not go into stealth";   // why, the last time it could not
        const char*       how = "";          // the last plan, for the log
    };
    std::vector<Sap> g_saps;

    // What the rogue tells the group when the star asks for something it
    // cannot do, at most once per reason every SayCooldownSeconds.
    enum class SapWhy : uint8
    {
        NotLearned,
        WrongType,
        NoStealth,
        Failed,
    };
    std::unordered_map<uint64_t, Clock::time_point> g_said;   // rogue guid * 4 + reason

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

    // Why Sap will not work on this mob, for "can't sap that, it's undead";
    // empty when it does.
    std::string NotSappable(uint32 creatureType)
    {
        const char* what = nullptr;
        switch (creatureType)
        {
            case CREATURE_TYPE_HUMANOID:
            case CREATURE_TYPE_BEAST:
            case CREATURE_TYPE_DEMON:
            case CREATURE_TYPE_DRAGONKIN:
                return std::string();
            case CREATURE_TYPE_UNDEAD:     what = "undead";       break;
            case CREATURE_TYPE_ELEMENTAL:  what = "an elemental"; break;
            case CREATURE_TYPE_MECHANICAL: what = "a machine";    break;
            case CREATURE_TYPE_GIANT:      what = "a giant";      break;
            case CREATURE_TYPE_CRITTER:    what = "a critter";    break;
            case CREATURE_TYPE_TOTEM:      what = "a totem";      break;
            default:                       break;
        }
        const char* rule = "Sap only works on humanoids, beasts, demons and dragonkin";
        return what ? fmt::format("it is {}, and {}", what, rule) : std::string(rule);
    }

    // Why a sap did not go out, in words the rogue can use, and for the log.
    const char* FailWords(SpellCastResult result)
    {
        switch (result)
        {
            case SPELL_FAILED_OUT_OF_RANGE:            return "you could not get close enough";
            case SPELL_FAILED_LINE_OF_SIGHT:           return "something was in the way";
            case SPELL_FAILED_UNIT_NOT_INFRONT:        return "you could not get facing it";
            case SPELL_FAILED_NO_POWER:                return "you were out of energy";
            case SPELL_FAILED_ONLY_STEALTHED:          return "you dropped out of stealth";
            case SPELL_FAILED_TARGET_AFFECTING_COMBAT: return "it was already in a fight";
            case SPELL_FAILED_IMMUNE:                  return "it is immune to it";
            case SPELL_FAILED_BAD_TARGETS:             return "it was not a target you could sap";
            default:                                   return "it just did not work";
        }
    }

    // Close enough for the core to let the sap go. A melee-range spell is
    // checked the way melee is -- the old test used the spell's range as a
    // plain distance, which stops a couple of yards short of melee reach, and
    // that is the sap that "walked over, did nothing and came back".
    bool InSapReach(Player* rogue, Unit* target, SpellInfo const* info)
    {
        if (!info || !info->RangeEntry || (info->RangeEntry->Flags & SPELL_RANGE_MELEE))
            return rogue->IsWithinMeleeRange(target);
        const float range = info->GetMaxRange(false, rogue);
        return rogue->IsWithinCombatRange(target, std::max(0.0f, range - 1.0f));
    }

    // How near in front a mob spots a rogue in stealth, centre to centre:
    // the core's own sum (WorldObject::CanDetectStealthOf -- its level against
    // the rogue's stealth), with the margin at which it only grows suspicious
    // and turns to look. From behind it never does.
    float SpotRange(Creature* mob, Player* rogue)
    {
        if (mob->HasAuraTypeWithMiscvalue(SPELL_AURA_DETECT_STEALTH, STEALTH_GENERAL))
            return 40.0f;
        const int32 value = 30 + (int32(mob->getLevelForTarget(rogue)) - 1) * 5 +
                            mob->m_stealthDetect.GetValue(STEALTH_GENERAL) - rogue->m_stealth.GetValue(STEALTH_GENERAL);
        const float range = float(std::max<int32>(value, 0)) * 0.3f + mob->GetCombatReach();
        return range * 1.08f + 1.5f;
    }

    // The farthest out the sap goes from, centre to centre: what InSapReach
    // allows (a melee-range sap: melee range; any other: its range, less a
    // yard to spare).
    float SapCentreReach(Player* rogue, Unit* target, SpellInfo const* info)
    {
        if (!info || !info->RangeEntry || (info->RangeEntry->Flags & SPELL_RANGE_MELEE))
            return rogue->GetMeleeRange(target);
        return std::max(0.0f, info->GetMaxRange(false, rogue) - 1.0f) + rogue->GetCombatReach() + target->GetCombatReach();
    }

    // Can the rogue walk there? A path that ends short of it (a tree, a rock,
    // a ledge) is no good; a map with no navigation mesh walks straight.
    bool Walkable(Player* rogue, float x, float y, float z)
    {
        PathGenerator path(rogue);
        path.CalculatePath(x, y, z, false);
        const uint32 type = path.GetPathType();
        if (type & PATHFIND_NOPATH)
            return false;
        if (type & PATHFIND_INCOMPLETE)
        {
            const G3D::Vector3& end = path.GetActualEndPosition();
            const float dx = end.x - x, dy = end.y - y;
            return dx * dx + dy * dy <= 2.0f * 2.0f;
        }
        return true;
    }

    // Where the rogue walks next. The sap reaching farther than the mob can
    // see: straight in, to between the two, from whatever side it is on.
    // Otherwise it has to come from behind, the only side a mob never spots
    // stealth from: in front, round it a step at a time, out of sight;
    // behind, in towards its back. A mob wandering about: hang back out of
    // sight until it stops (`restless`). Right up to it after a cast that
    // said out of range (`closer`). Each point is tried for a walkable path,
    // a little to either side and further out, before settling for it.
    void PlanSapPoint(Player* rogue, Creature* target, SpellInfo const* info, bool closer, bool restless,
                      float& x, float& y, float& z, const char*& how)
    {
        constexpr float kDeg = static_cast<float>(M_PI) / 180.0f;
        const float face  = target->GetOrientation();
        float rel = Position::NormalizeOrientation(target->GetAngle(rogue) - face);   // 0 .. 2pi
        if (rel > static_cast<float>(M_PI))
            rel -= 2.0f * static_cast<float>(M_PI);                                   // -pi .. pi
        const float side = rel >= 0.0f ? 1.0f : -1.0f;
        const float off  = std::fabs(rel);

        const float spot    = SpotRange(target, rogue);
        const float reach   = SapCentreReach(rogue, target, info);
        const float nearest = rogue->GetCombatReach() + target->GetCombatReach() + 0.3f;

        float angle = target->GetAngle(rogue);   // as it is: straight in
        float dist  = 0.0f;
        bool  round = false;
        if (closer)
        {
            dist = nearest + 0.2f;
            how  = "right up to it";
        }
        else if (reach > spot + 1.0f)
        {
            dist = restless ? std::max(spot + 3.0f, reach + 1.0f) : (spot + 1.0f + reach - 0.5f) / 2.0f;
            how  = restless ? "hanging back" : "straight in";
        }
        else if (off < 100.0f * kDeg)
        {
            angle = face + side * std::min(off + 60.0f * kDeg, 130.0f * kDeg);
            dist  = std::max(spot + 1.5f, reach + 1.0f);
            round = true;
            how   = "round the side";
        }
        else
        {
            angle = face + side * std::min(off + 30.0f * kDeg, static_cast<float>(M_PI));
            dist  = restless ? std::max(spot + 1.5f, reach + 1.0f)
                             : std::clamp(reach - 1.0f, nearest, std::max(nearest, reach - 0.3f));
            how   = restless ? "hanging back" : "in from behind";
        }

        auto at = [&](float a, float d, float& px, float& py, float& pz)
        {
            a  = Position::NormalizeOrientation(a);
            px = target->GetPositionX() + d * std::cos(a);
            py = target->GetPositionY() + d * std::sin(a);
            pz = target->GetPositionZ();
            rogue->UpdateAllowedPositionZ(px, py, pz);
        };
        at(angle, dist, x, y, z);
        if (Walkable(rogue, x, y, z))
            return;

        // Blocked: a little to either side, further out, the other way round.
        struct Try
        {
            float da, dd;
        };
        const Try tries[] = { { 20.0f, 0.0f }, { -20.0f, 0.0f }, { 0.0f, 2.5f }, { 35.0f, 2.5f }, { -35.0f, 2.5f } };
        for (const Try& t : tries)
        {
            // Behind it, never round to its front for a better path.
            const float a = angle + side * t.da * kDeg;
            float r = Position::NormalizeOrientation(a - face);
            if (r > static_cast<float>(M_PI))
                r -= 2.0f * static_cast<float>(M_PI);
            if (!round && reach <= spot + 1.0f && !closer && std::fabs(r) < 95.0f * kDeg)
                continue;
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            at(a, dist + t.dd, px, py, pz);
            if (Walkable(rogue, px, py, pz))
            {
                x = px;
                y = py;
                z = pz;
                return;
            }
        }
        if (round)
        {
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            at(face - side * std::min(off + 60.0f * kDeg, 130.0f * kDeg), dist, px, py, pz);
            if (Walkable(rogue, px, py, pz))
            {
                x = px;
                y = py;
                z = pz;
                how = "round the other side";
            }
        }
    }

    // The rogue says, in party, why it is not sapping the star -- in its own
    // words (OllamaChat.Marks.SayPrompt with one of the Sap* situations).
    void Explain(Player* rogue, uint64_t askerGuid, Unit* target, SapWhy why, const std::string& detail,
                 Clock::time_point now)
    {
        const std::string* situation = nullptr;
        switch (why)
        {
            case SapWhy::NotLearned: situation = &g_MarksSapNotLearned; break;
            case SapWhy::WrongType:  situation = &g_MarksSapWrongType;  break;
            case SapWhy::NoStealth:  situation = &g_MarksSapNoStealth;  break;
            case SapWhy::Failed:     situation = &g_MarksSapFailed;     break;
        }
        if (!g_MarksSayEnable || !situation || situation->empty() || g_MarksSayPrompt.empty() || !rogue || !target)
            return;

        const uint64_t key = rogue->GetGUID().GetRawValue() * 4 + static_cast<uint8>(why);
        if (auto it = g_said.find(key);
            it != g_said.end() && now - it->second < std::chrono::seconds(g_MarksSayCooldownSeconds))
            return;

        Group* group = rogue->GetGroup();
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(rogue);
        if (!group || !ai || !ai->GetChatHelper())
            return;
        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           group->GetGUID().GetCounter());
        if (!Governor_CanSend(rogue->GetGUID(), scopeKey, true))
            return;

        Player* asker = Find(askerGuid);
        const std::string askerName = asker ? asker->GetName() : std::string("someone");
        const std::string what = SafeFormat(*situation,
                                            fmt::arg("player_name", askerName),
                                            fmt::arg("target", target->GetName()),
                                            fmt::arg("detail", detail),
                                            fmt::arg("bot_level", rogue->GetLevel()),
                                            fmt::arg("sap_level", SAP_LEVEL));
        if (what.empty() || what == "[Format Error]")
            return;
        std::string prompt = SafeFormat(g_MarksSayPrompt,
                                        fmt::arg("bot_name", rogue->GetName()),
                                        fmt::arg("bot_level", rogue->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(rogue->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(rogue->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(GetBotPersonality(rogue))),
                                        fmt::arg("player_name", askerName),
                                        fmt::arg("situation", what));
        if (prompt.empty() || prompt == "[Format Error]")
            return;
        prompt += Roleplay_BuildVoicePrompt(rogue);

        OllamaChatRequest request;
        request.botGuid           = rogue->GetGUID().GetRawValue();
        request.source            = source;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = rogue->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.directAddress     = true;    // an answer to the mark, not ambient chatter
        request.triggerBotReplies = false;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(rogue);
        if (OllamaDispatch_Submit(std::move(request)))
        {
            g_said[key] = now;
            ++g_stats.explained;
        }
    }

    // Hand the rogue back to its own AI, which walks it back to the group (or
    // into the fight) on its next update.
    void FinishSap(Player* rogue, PlayerbotAI* ai, bool held)
    {
        if (held && rogue && ai)
        {
            rogue->GetMotionMaster()->Clear();
            ai->SetNextCheckDelay(0);
        }
    }

    // The star went on a mob nobody is fighting: the nearest rogue bot that
    // can, sneaks over and saps it. One that cannot -- no Sap yet, or a mob
    // Sap does not work on -- leaves it alone and says so.
    void StartSap(Player* ref, Group* group, ObjectGuid starGuid, Clock::time_point now)
    {
        Creature* target = ObjectAccessor::GetCreature(*ref, starGuid);
        if (!target || !target->IsAlive() || target->IsInCombat() || IsSapped(target))
            return;

        Player* rogue = nullptr;     // the nearest one with Sap
        Player* without = nullptr;   // the nearest one without
        float bestDist = 40.0f, bestWithout = 40.0f;
        for (GroupReference* r = group->GetFirstMember(); r; r = r->next())
        {
            Player* member = r->GetSource();
            if (!member || member->getClass() != CLASS_ROGUE || !OllamaIsBotPlayer(member) || !member->IsInWorld() ||
                !member->IsAlive() || member->IsInCombat() || member->GetMap() != target->GetMap() ||
                !member->IsValidAttackTarget(target))
                continue;
            const float dist = member->GetDistance(target);
            if (!SapSpell(member))
            {
                if (dist < bestWithout)
                {
                    without = member;
                    bestWithout = dist;
                }
                continue;
            }
            if (member->IsMounted())
                continue;
            if (dist < bestDist)
            {
                rogue = member;
                bestDist = dist;
            }
        }

        const uint64_t asker = ref->GetGUID().GetRawValue();
        if (!rogue)
        {
            if (without)
            {
                const std::string why = without->GetLevel() < SAP_LEVEL
                    ? fmt::format("rogues learn Sap at level {} and you are level {}", SAP_LEVEL, without->GetLevel())
                    : std::string("you have not trained Sap");
                Explain(without, asker, target, SapWhy::NotLearned, why, now);
                ++g_stats.sapCant;
                MARKS_SAP_RESULT("[Ollama Chat] {} cannot sap {} (star): no Sap.", without->GetName(), target->GetName());
            }
            return;
        }

        SpellInfo const* info = sSpellMgr->GetSpellInfo(SapSpell(rogue));
        std::string cant = NotSappable(target->GetCreatureType());
        if (cant.empty() && info && target->IsImmunedToSpell(info))
            cant = "it is immune to Sap";
        if (!cant.empty())
        {
            Explain(rogue, asker, target, SapWhy::WrongType, cant, now);
            ++g_stats.sapCant;
            MARKS_SAP_RESULT("[Ollama Chat] {} cannot sap {} (star): {}.", rogue->GetName(), target->GetName(), cant);
            return;
        }

        // A new star while it was on its way to the last one: let go of that
        // walk first.
        const uint64_t rogueGuid = rogue->GetGUID().GetRawValue();
        for (const Sap& s : g_saps)
            if (s.rogue == rogueGuid && s.phase != SapPhase::Stealth)
                FinishSap(rogue, PlayerbotsMgr::instance().GetPlayerbotAI(rogue), true);
        g_saps.erase(std::remove_if(g_saps.begin(), g_saps.end(), [&](const Sap& s) { return s.rogue == rogueGuid; }),
                     g_saps.end());
        Sap sap;
        sap.rogue  = rogueGuid;
        sap.target = target->GetGUID().GetRawValue();
        sap.asker  = asker;
        sap.phase  = SapPhase::Stealth;
        sap.until  = now + std::chrono::seconds(g_MarksSapStealthWaitSeconds);
        g_saps.push_back(sap);
        MARKS_SAP_RESULT("[Ollama Chat] {} goes to sap {} (star, level {} against {}){}.", rogue->GetName(),
                         target->GetName(), target->GetLevel(), rogue->GetLevel(),
                         rogue->HasStealthAura() ? "" : ", stealth first");
    }

    void UpdateSaps(Clock::time_point now)
    {
        for (auto it = g_saps.begin(); it != g_saps.end();)
        {
            Sap& s = *it;
            Player* rogue = Find(s.rogue);
            PlayerbotAI* ai = rogue ? PlayerbotsMgr::instance().GetPlayerbotAI(rogue) : nullptr;
            Creature* target = (rogue && rogue->IsInWorld()) ? ObjectAccessor::GetCreature(*rogue, ObjectGuid(s.target))
                                                             : nullptr;
            Group* group = rogue ? rogue->GetGroup() : nullptr;
            const uint32 sap = rogue ? SapSpell(rogue) : 0;
            const bool held = s.phase != SapPhase::Stealth;   // walking it ourselves

            // Landed, whoever did it.
            if (target && IsSapped(target))
            {
                if (s.phase == SapPhase::Check)
                {
                    ++g_stats.saps;
                    MARKS_SAP_RESULT("[Ollama Chat] {} sapped {}.", rogue ? rogue->GetName() : "?", target->GetName());
                }
                FinishSap(rogue, ai, held);
                it = g_saps.erase(it);
                continue;
            }

            // A fight, the star moved, somebody gone: nothing to say about it.
            if (!rogue || !ai || !target || !group || !sap || !rogue->IsInWorld() || !rogue->IsAlive() ||
                !target->IsAlive() || rogue->IsInCombat() || target->IsInCombat() ||
                rogue->GetMap() != target->GetMap() || group->GetTargetIcon(ICON_STAR) != target->GetGUID())
            {
                if (rogue && target && (rogue->IsInCombat() || target->IsInCombat()))
                    MARKS_SAP_RESULT("[Ollama Chat] {} stopped going to sap {}: a fight started.", rogue->GetName(),
                                     target->GetName());
                FinishSap(rogue, ai, held && rogue && rogue->IsInWorld());
                it = g_saps.erase(it);
                continue;
            }

            SpellInfo const* info = sSpellMgr->GetSpellInfo(sap);
            bool done = false;
            switch (s.phase)
            {
                case SapPhase::Stealth:
                {
                    // Its own AI runs meanwhile: it keeps following, and does
                    // not walk up to the mob in plain sight. Tried twice a
                    // second, not every update.
                    if (!rogue->HasStealthAura() && now >= s.nextMove)
                    {
                        s.nextMove = now + std::chrono::milliseconds(500);
                        if (!rogue->HasSpell(SPELL_STEALTH))
                            s.noStealth = "you do not know Stealth";
                        else if (rogue->IsMounted())
                            s.noStealth = "you were mounted";
                        else if (rogue->HasSpellCooldown(SPELL_STEALTH))
                            s.noStealth = "your Stealth was still on cooldown";
                        else if (rogue->getStandState() != UNIT_STAND_STATE_STAND)
                            s.noStealth = "you were sitting down";
                        else
                        {
                            const SpellCastResult result = rogue->CastSpell(rogue, SPELL_STEALTH, false);
                            if (result != SPELL_CAST_OK)
                                MARKS_SAP_LOG("[Ollama Chat] {} could not go into stealth to sap {} (cast result {}).",
                                              rogue->GetName(), target->GetName(), static_cast<uint32>(result));
                        }
                    }
                    if (rogue->HasStealthAura())
                    {
                        s.phase    = SapPhase::Approach;
                        s.until    = now + std::chrono::seconds(g_MarksSapApproachSeconds);
                        s.nextMove = now;
                        ai->SetNextCheckDelay(1000);
                    }
                    else if (now >= s.until)
                    {
                        ++g_stats.sapGaveUp;
                        MARKS_SAP_RESULT("[Ollama Chat] {} gave up on sapping {}: no stealth in {}s ({}).",
                                         rogue->GetName(), target->GetName(), g_MarksSapStealthWaitSeconds, s.noStealth);
                        Explain(rogue, s.asker, target, SapWhy::NoStealth, s.noStealth, now);
                        done = true;
                    }
                    break;
                }

                case SapPhase::Approach:
                {
                    ai->SetNextCheckDelay(1000);   // the walk is this, not its own idea of where to be
                    if (!rogue->HasStealthAura())
                    {
                        ++g_stats.sapGaveUp;
                        MARKS_SAP_RESULT("[Ollama Chat] {} lost stealth on the way to sap {}.",
                                         rogue->GetName(), target->GetName());
                        done = true;
                        break;
                    }

                    // A mob wandering about: the time spent hanging back for it
                    // to stop does not count, up to Marks.SapWaitSeconds.
                    const bool restless = target->isMoving();
                    const uint32_t tick = s.lastTick == Clock::time_point{} ? 0u
                        : uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - s.lastTick).count());
                    s.lastTick = now;
                    if (restless && s.waitedMs < g_MarksSapWaitSeconds * 1000)
                    {
                        s.waitedMs += tick;
                        s.until += std::chrono::milliseconds(tick);
                    }
                    if (restless != s.waiting)
                    {
                        s.waiting  = restless;
                        s.nextMove = now;   // plan again for it: hang back, or go in
                    }

                    if (now >= s.until)
                    {
                        ++g_stats.sapGaveUp;
                        const char* why = s.waitedMs >= g_MarksSapWaitSeconds * 1000
                                              ? "it would not stop wandering about"
                                              : "you could not get round to it in time";
                        MARKS_SAP_RESULT("[Ollama Chat] {} could not reach {} to sap it ({}; last plan: {}; {:.1f} yd "
                                         "off, it spots stealth at {:.1f} yd in front, sap reaches {:.1f} yd).",
                                         rogue->GetName(), target->GetName(), why, s.how, rogue->GetExactDist(target),
                                         SpotRange(target, rogue), SapCentreReach(rogue, target, info));
                        Explain(rogue, s.asker, target, SapWhy::Failed, why, now);
                        done = true;
                        break;
                    }

                    // In reach, and where it cannot be seen -- behind, or
                    // further off than it sees in front: stop, face it, sap.
                    // (After a cast that said out of range: right up to it, or
                    // as close as the walk gets it.)
                    const bool arrived = !rogue->isMoving() && now - s.moved >= std::chrono::milliseconds(300);
                    float rel = Position::NormalizeOrientation(target->GetAngle(rogue) - target->GetOrientation());
                    if (rel > static_cast<float>(M_PI))
                        rel -= 2.0f * static_cast<float>(M_PI);
                    const bool unseen = std::fabs(rel) > static_cast<float>(M_PI) * 95.0f / 180.0f ||
                                        rogue->GetExactDist(target) > SpotRange(target, rogue);
                    const bool inReach = s.closer
                        ? (rogue->IsWithinDistInMap(target, 1.0f) || (arrived && InSapReach(rogue, target, info)))
                        : (InSapReach(rogue, target, info) && unseen);
                    if (inReach && !restless)
                    {
                        rogue->GetMotionMaster()->Clear();
                        rogue->StopMoving();
                        rogue->SetFacingToObject(target);
                        s.phase  = SapPhase::Settle;
                        s.castAt = now + std::chrono::milliseconds(250);   // the turn lands on the next update
                        MARKS_SAP_LOG("[Ollama Chat] {} is in reach of {} ({:.1f} yd, {:.0f} deg round): sapping.",
                                      rogue->GetName(), target->GetName(), rogue->GetExactDist(target),
                                      std::fabs(rel) * 180.0f / static_cast<float>(M_PI));
                        break;
                    }
                    // Got where it was going in and still short (a slope, the
                    // mob's size): right up to it.
                    if (arrived && !restless && !s.closer && std::strcmp(s.how, "in from behind") == 0 &&
                        ++s.shortOf >= 2)
                    {
                        s.closer   = true;
                        s.nextMove = now;
                    }
                    // Planned again every second (the mob turns, wanders on)
                    // and as soon as it stands still at a step.
                    if (now >= s.nextMove || arrived)
                    {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        PlanSapPoint(rogue, target, info, s.closer, restless, x, y, z, s.how);
                        rogue->GetMotionMaster()->MovePoint(0, x, y, z);
                        s.moved    = now;
                        s.nextMove = now + std::chrono::seconds(1);
                        MARKS_SAP_LOG("[Ollama Chat] {} to sap {}: {} ({:.1f} yd off, {:.0f} deg round, spots at "
                                      "{:.1f}, reach {:.1f}{}).",
                                      rogue->GetName(), target->GetName(), s.how, rogue->GetExactDist(target),
                                      std::fabs(rel) * 180.0f / static_cast<float>(M_PI), SpotRange(target, rogue),
                                      SapCentreReach(rogue, target, info), restless ? ", it is moving" : "");
                    }
                    break;
                }

                case SapPhase::Settle:
                {
                    ai->SetNextCheckDelay(1000);
                    if (now < s.castAt)
                        break;
                    if (!InSapReach(rogue, target, info))
                    {
                        s.phase    = SapPhase::Approach;   // it moved off (a patrol)
                        s.nextMove = now;
                        break;
                    }
                    if (!rogue->HasInArc(static_cast<float>(M_PI), target) && s.tries < 4)
                    {
                        ++s.tries;
                        rogue->SetFacingToObject(target);
                        s.castAt = now + std::chrono::milliseconds(250);
                        break;
                    }

                    const SpellCastResult result = rogue->CastSpell(target, sap, false);
                    if (result == SPELL_CAST_OK)
                    {
                        s.phase  = SapPhase::Check;
                        s.castAt = now + std::chrono::milliseconds(700);
                        break;
                    }
                    g_stats.lastFail = static_cast<uint32>(result);
                    MARKS_SAP_RESULT("[Ollama Chat] {} could not sap {}: {} (cast result {}, {:.1f} yd off).",
                                     rogue->GetName(), target->GetName(), FailWords(result), static_cast<uint32>(result),
                                     rogue->GetExactDist(target));
                    if (s.tries < 4 &&
                        (result == SPELL_FAILED_OUT_OF_RANGE || result == SPELL_FAILED_LINE_OF_SIGHT))
                    {
                        ++s.tries;
                        s.closer = true;
                        s.phase  = SapPhase::Approach;
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        PlanSapPoint(rogue, target, info, true, false, x, y, z, s.how);
                        rogue->GetMotionMaster()->MovePoint(0, x, y, z);
                        s.moved    = now;
                        s.nextMove = now + std::chrono::seconds(1);
                        break;
                    }
                    if (s.tries < 4 && result == SPELL_FAILED_UNIT_NOT_INFRONT)
                    {
                        ++s.tries;
                        rogue->SetFacingToObject(target);
                        s.castAt = now + std::chrono::milliseconds(250);
                        break;
                    }
                    if (result == SPELL_FAILED_NO_POWER || result == SPELL_FAILED_NOT_READY ||
                        result == SPELL_FAILED_SPELL_IN_PROGRESS)
                    {
                        s.castAt = now + std::chrono::milliseconds(500);   // energy, the global cooldown
                        if (now < s.until)
                            break;
                    }
                    ++g_stats.sapFails;
                    Explain(rogue, s.asker, target, SapWhy::Failed, FailWords(result), now);
                    done = true;
                    break;
                }

                case SapPhase::Check:
                {
                    ai->SetNextCheckDelay(1000);
                    if (now < s.castAt)
                        break;
                    // Cast, but no sap on it and no fight either: it shrugged
                    // it off. Once more, then say so.
                    ++g_stats.sapMissed;
                    MARKS_SAP_RESULT("[Ollama Chat] {} cast Sap on {} but it did not take.", rogue->GetName(),
                                     target->GetName());
                    if (++s.tries < 5 && now < s.until)
                    {
                        s.phase  = SapPhase::Settle;
                        s.castAt = now;
                        break;
                    }
                    Explain(rogue, s.asker, target, SapWhy::Failed, "it shrugged it off", now);
                    done = true;
                    break;
                }
            }

            if (done)
            {
                FinishSap(rogue, ai, s.phase != SapPhase::Stealth);
                it = g_saps.erase(it);
            }
            else
                it = std::next(it);
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
    for (auto it = g_said.begin(); it != g_said.end();)
        it = (now - it->second > std::chrono::seconds(g_MarksSayCooldownSeconds)) ? g_said.erase(it) : std::next(it);
}

MarksStats Marks_GetStats()
{
    return g_stats;
}
