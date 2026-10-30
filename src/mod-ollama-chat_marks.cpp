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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Debug builds of the sap's progress go to INFO with OllamaChat.DebugEnabled,
// to DEBUG otherwise, so a failing sap can be read back without the debug flag.
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
        uint8             tries  = 0;        // facing and range retries, and missed casts
        bool              closer = false;    // the cast said out of range: go right up to it
        const char*       noStealth = "you could not go into stealth";   // why, the last time it could not
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

    // How far from the mob's edge the rogue stops to sap: right up to it for a
    // melee-range sap (or after the cast said out of range), a few yards off
    // otherwise.
    float SapStandOff(Player* rogue, SpellInfo const* info, bool closer)
    {
        if (closer || !info || !info->RangeEntry || (info->RangeEntry->Flags & SPELL_RANGE_MELEE))
            return 0.5f;
        return std::clamp(info->GetMaxRange(false, rogue) - 4.0f, 0.5f, 4.0f);
    }

    // Where the rogue walks next. In front of the mob it goes round it
    // first, a step of the circle at a time, far enough out not to be seen
    // (a mob spots stealth only in front of it, and closer for a lower-level
    // one); once it is round the side, straight in to the mob's back. Close
    // already and only short of reach (`closer`): straight up to it from
    // where it stands.
    void NextSapPoint(Player* rogue, Creature* target, float standOff, bool closer, float& x, float& y, float& z)
    {
        if (closer)
        {
            target->GetNearPoint(rogue, x, y, z, rogue->GetObjectSize(), standOff, target->GetAngle(rogue));
            return;
        }

        constexpr float kRoundEnough = static_cast<float>(M_PI) * 100.0f / 180.0f;   // past the front half
        constexpr float kStep        = static_cast<float>(M_PI) / 3.0f;              // 60 degrees at a time
        constexpr float kSide        = static_cast<float>(M_PI) * 115.0f / 180.0f;

        const float face = target->GetOrientation();
        float rel = Position::NormalizeOrientation(target->GetAngle(rogue) - face);   // 0 .. 2pi
        if (rel > static_cast<float>(M_PI))
            rel -= 2.0f * static_cast<float>(M_PI);                                   // -pi .. pi
        const float side = rel >= 0.0f ? 1.0f : -1.0f;
        const float off  = std::fabs(rel);

        float angle = face + static_cast<float>(M_PI);   // its back
        float dist  = standOff;
        if (off < kRoundEnough)
        {
            const float edge = rogue->GetExactDist2d(target) - rogue->GetCombatReach() - target->GetCombatReach();
            angle = face + side * std::min(off + kStep, kSide);
            dist  = std::clamp(edge, 8.0f, 14.0f);
        }
        target->GetNearPoint(rogue, x, y, z, rogue->GetObjectSize(), dist, Position::NormalizeOrientation(angle));
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
                MARKS_SAP_LOG("[Ollama Chat] {} cannot sap {} (star): no Sap.", without->GetName(), target->GetName());
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
            MARKS_SAP_LOG("[Ollama Chat] {} cannot sap {} (star): {}.", rogue->GetName(), target->GetName(), cant);
            return;
        }

        const uint64_t rogueGuid = rogue->GetGUID().GetRawValue();
        g_saps.erase(std::remove_if(g_saps.begin(), g_saps.end(), [&](const Sap& s) { return s.rogue == rogueGuid; }),
                     g_saps.end());
        Sap sap;
        sap.rogue  = rogueGuid;
        sap.target = target->GetGUID().GetRawValue();
        sap.asker  = asker;
        sap.phase  = SapPhase::Stealth;
        sap.until  = now + std::chrono::seconds(g_MarksSapStealthWaitSeconds);
        g_saps.push_back(sap);
        MARKS_SAP_LOG("[Ollama Chat] {} goes to sap {} (star){}.", rogue->GetName(), target->GetName(),
                      rogue->HasStealthAura() ? "" : ", stealth first");
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
                    MARKS_SAP_LOG("[Ollama Chat] {} sapped {}.", rogue ? rogue->GetName() : "?", target->GetName());
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
                    MARKS_SAP_LOG("[Ollama Chat] {} stopped going to sap {}: a fight started.", rogue->GetName(),
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
                        MARKS_SAP_LOG("[Ollama Chat] {} gave up on sapping {}: no stealth in {}s ({}).",
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
                        MARKS_SAP_LOG("[Ollama Chat] {} lost stealth on the way to sap {}.",
                                      rogue->GetName(), target->GetName());
                        done = true;
                        break;
                    }
                    if (now >= s.until)
                    {
                        ++g_stats.sapGaveUp;
                        MARKS_SAP_LOG("[Ollama Chat] {} could not reach {} to sap it in {}s.",
                                      rogue->GetName(), target->GetName(), g_MarksSapApproachSeconds);
                        Explain(rogue, s.asker, target, SapWhy::Failed, "you could not get round to it in time", now);
                        done = true;
                        break;
                    }
                    // After a cast that said out of range: right up to it, or
                    // as close as the walk gets it.
                    const bool arrived = !rogue->isMoving() && now - s.moved >= std::chrono::milliseconds(300);
                    const bool inReach = s.closer
                        ? (rogue->IsWithinDistInMap(target, 1.0f) || (arrived && InSapReach(rogue, target, info)))
                        : InSapReach(rogue, target, info);
                    if (inReach)
                    {
                        rogue->GetMotionMaster()->Clear();
                        rogue->StopMoving();
                        rogue->SetFacingToObject(target);
                        s.phase  = SapPhase::Settle;
                        s.castAt = now + std::chrono::milliseconds(250);   // the turn lands on the next update
                        break;
                    }
                    // Planned again every second (the mob turns, a patrol
                    // walks on) and as soon as it stands still at a step.
                    if (now >= s.nextMove || arrived)
                    {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        NextSapPoint(rogue, target, SapStandOff(rogue, info, s.closer), s.closer, x, y, z);
                        rogue->GetMotionMaster()->MovePoint(0, x, y, z);
                        s.moved    = now;
                        s.nextMove = now + std::chrono::seconds(1);
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
                    MARKS_SAP_LOG("[Ollama Chat] {} could not sap {}: {} (cast result {}).", rogue->GetName(),
                                  target->GetName(), FailWords(result), static_cast<uint32>(result));
                    if (s.tries < 4 &&
                        (result == SPELL_FAILED_OUT_OF_RANGE || result == SPELL_FAILED_LINE_OF_SIGHT))
                    {
                        ++s.tries;
                        s.closer = true;
                        s.phase  = SapPhase::Approach;
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        NextSapPoint(rogue, target, SapStandOff(rogue, info, true), true, x, y, z);
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
                    MARKS_SAP_LOG("[Ollama Chat] {} cast Sap on {} but it did not take.", rogue->GetName(),
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
