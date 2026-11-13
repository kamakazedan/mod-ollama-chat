#include "mod-ollama-chat_wants.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_knowledge.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "InstanceScript.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // The most creatures a loot table may be shared by and still count as a
    // boss's own: a world-drop table is shared by thousands.
    constexpr uint64_t kOwnTableMaxSharers = 12;

    struct Boss
    {
        std::string           name;
        uint32_t              encounterIndex = 0;   // its bit in the instance's done encounters
        std::vector<uint32_t> items;                // the gear it can drop
    };

    struct Dungeon
    {
        uint32_t          map        = 0;
        uint8_t           difficulty = 0;
        std::string       name;
        uint32_t          minLevel   = 0;
        uint32_t          maxLevel   = 0;
        std::vector<Boss> bosses;
    };

    struct Index
    {
        bool                 built = false;
        std::vector<Dungeon> dungeons;
    };
    Index g;

    struct Want
    {
        const TextRules::ItemKindWords* kind = nullptr;
        std::string       facts;           // what is so, for the one after it
        std::string       name;            // who is after it
        uint32_t          level = 0;
        std::string       cls;
        uint32_t          group = 0;       // their group then (its low guid), or 0
        Clock::time_point until{};
    };
    std::unordered_map<uint64_t, Want> g_wants;   // speaker -> what they are after
    WantsStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    std::string JoinIds(const std::vector<uint32_t>& ids)
    {
        std::string out;
        for (uint32_t id : ids)
        {
            if (!out.empty())
                out += ',';
            out += std::to_string(id);
        }
        return out;
    }

    std::string JoinWords(const std::vector<std::string>& parts, const char* last = " and ")
    {
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i)
            out += (i == 0 ? "" : (i + 1 == parts.size() ? last : ", ")) + parts[i];
        return out;
    }

    // ---- what a kind is, and who can use one ------------------------------------

    bool OfKind(ItemTemplate const* p, const std::string& kind)
    {
        const bool   weapon = p->Class == ITEM_CLASS_WEAPON;
        const bool   armor  = p->Class == ITEM_CLASS_ARMOR;
        const uint32 sub    = p->SubClass;
        const uint32 inv    = p->InventoryType;
        if (kind == "wand")          return weapon && sub == ITEM_SUBCLASS_WEAPON_WAND;
        if (kind == "staff")         return weapon && sub == ITEM_SUBCLASS_WEAPON_STAFF;
        if (kind == "bow")           return weapon && sub == ITEM_SUBCLASS_WEAPON_BOW;
        if (kind == "gun")           return weapon && sub == ITEM_SUBCLASS_WEAPON_GUN;
        if (kind == "crossbow")      return weapon && sub == ITEM_SUBCLASS_WEAPON_CROSSBOW;
        if (kind == "thrown weapon") return weapon && sub == ITEM_SUBCLASS_WEAPON_THROWN;
        if (kind == "ranged weapon")
            return weapon && (sub == ITEM_SUBCLASS_WEAPON_BOW || sub == ITEM_SUBCLASS_WEAPON_GUN ||
                              sub == ITEM_SUBCLASS_WEAPON_CROSSBOW || sub == ITEM_SUBCLASS_WEAPON_THROWN);
        if (kind == "fist weapon")   return weapon && sub == ITEM_SUBCLASS_WEAPON_FIST;
        if (kind == "two-hander")    return weapon && inv == INVTYPE_2HWEAPON;
        if (kind == "sword")
            return weapon && (sub == ITEM_SUBCLASS_WEAPON_SWORD || sub == ITEM_SUBCLASS_WEAPON_SWORD2);
        if (kind == "axe")
            return weapon && (sub == ITEM_SUBCLASS_WEAPON_AXE || sub == ITEM_SUBCLASS_WEAPON_AXE2);
        if (kind == "mace")
            return weapon && (sub == ITEM_SUBCLASS_WEAPON_MACE || sub == ITEM_SUBCLASS_WEAPON_MACE2);
        if (kind == "dagger")        return weapon && sub == ITEM_SUBCLASS_WEAPON_DAGGER;
        if (kind == "polearm")       return weapon && sub == ITEM_SUBCLASS_WEAPON_POLEARM;
        if (kind == "weapon")
            return weapon && (inv == INVTYPE_WEAPON || inv == INVTYPE_2HWEAPON || inv == INVTYPE_WEAPONMAINHAND ||
                              inv == INVTYPE_WEAPONOFFHAND);
        if (kind == "shield")        return armor && sub == ITEM_SUBCLASS_ARMOR_SHIELD;
        if (kind == "off-hand")      return inv == INVTYPE_HOLDABLE;
        if (kind == "cloak")         return inv == INVTYPE_CLOAK;
        if (kind == "ring")          return inv == INVTYPE_FINGER;
        if (kind == "necklace")      return inv == INVTYPE_NECK;
        if (kind == "trinket")       return inv == INVTYPE_TRINKET;
        if (kind == "helm")          return armor && inv == INVTYPE_HEAD;
        if (kind == "shoulders")     return armor && inv == INVTYPE_SHOULDERS;
        if (kind == "chest piece")   return armor && (inv == INVTYPE_CHEST || inv == INVTYPE_ROBE);
        if (kind == "bracers")       return armor && inv == INVTYPE_WRISTS;
        if (kind == "gloves")        return armor && inv == INVTYPE_HANDS;
        if (kind == "belt")          return armor && inv == INVTYPE_WAIST;
        if (kind == "leggings")      return armor && inv == INVTYPE_LEGS;
        if (kind == "boots")         return armor && inv == INVTYPE_FEET;
        if (kind == "relic")         return inv == INVTYPE_RELIC;
        return false;
    }

    // A piece worn on the body: cloth, leather, mail or plate.
    bool BodyArmor(const std::string& kind)
    {
        return kind == "helm" || kind == "shoulders" || kind == "chest piece" || kind == "bracers" ||
               kind == "gloves" || kind == "belt" || kind == "leggings" || kind == "boots";
    }

    // Strength and agility, or intellect and spell power: whose gear it is.
    enum class Lean : uint8_t
    {
        Any,
        Body,
        Mind
    };

    Lean LeanOf(ItemTemplate const* p)
    {
        bool body = false, mind = false;
        for (uint32 i = 0; i < p->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
            switch (p->ItemStat[i].ItemStatType)
            {
                case ITEM_MOD_STRENGTH:
                case ITEM_MOD_AGILITY:
                case ITEM_MOD_ATTACK_POWER:
                case ITEM_MOD_RANGED_ATTACK_POWER:
                case ITEM_MOD_ARMOR_PENETRATION_RATING:
                case ITEM_MOD_EXPERTISE_RATING:
                    body = true;
                    break;
                case ITEM_MOD_INTELLECT:
                case ITEM_MOD_SPIRIT:
                case ITEM_MOD_SPELL_POWER:
                case ITEM_MOD_SPELL_HEALING_DONE:
                case ITEM_MOD_SPELL_DAMAGE_DONE:
                case ITEM_MOD_MANA_REGENERATION:
                case ITEM_MOD_SPELL_PENETRATION:
                    mind = true;
                    break;
                default:
                    break;
            }
        return body == mind ? Lean::Any : (body ? Lean::Body : Lean::Mind);
    }

    // Paladins, shamans and druids go either way.
    Lean LeanOf(Player* p)
    {
        switch (p->getClass())
        {
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_HUNTER:
            case CLASS_DEATH_KNIGHT:
                return Lean::Body;
            case CLASS_MAGE:
            case CLASS_WARLOCK:
            case CLASS_PRIEST:
                return Lean::Mind;
            default:
                return Lean::Any;
        }
    }

    // The heaviest armour they have learned to wear.
    uint32 BestArmor(Player* p)
    {
        if (p->GetSkillValue(SKILL_PLATE_MAIL))
            return ITEM_SUBCLASS_ARMOR_PLATE;
        if (p->GetSkillValue(SKILL_MAIL))
            return ITEM_SUBCLASS_ARMOR_MAIL;
        if (p->GetSkillValue(SKILL_LEATHER))
            return ITEM_SUBCLASS_ARMOR_LEATHER;
        return ITEM_SUBCLASS_ARMOR_CLOTH;
    }

    // One they could use: their class, race and side, a weapon or armour
    // they have learned, the armour they wear, stats for what they do.
    bool CanUse(Player* p, ItemTemplate const* proto, const std::string& kind)
    {
        if (!(uint32(proto->AllowableClass) & p->getClassMask()) || !(uint32(proto->AllowableRace) & p->getRaceMask()))
            return false;
        if ((proto->Flags2 & ITEM_FLAG2_FACTION_HORDE) && p->GetTeamId() != TEAM_HORDE)
            return false;
        if ((proto->Flags2 & ITEM_FLAG2_FACTION_ALLIANCE) && p->GetTeamId() != TEAM_ALLIANCE)
            return false;
        if (const uint32 skill = proto->GetSkill(); skill && !p->GetSkillValue(skill))
            return false;
        if (BodyArmor(kind) && proto->SubClass != BestArmor(p))
            return false;
        const Lean mine = LeanOf(p), its = LeanOf(proto);
        return mine == Lean::Any || its == Lean::Any || mine == its;
    }

    // The level it is worn from (an item with none, about its item level's).
    uint32 NeedsLevel(ItemTemplate const* p)
    {
        return p->RequiredLevel ? p->RequiredLevel : (p->ItemLevel > 5 ? p->ItemLevel - 5 : 1);
    }

    // Whether `bot` could use one at all: the weapon or shield is one it has
    // learned; anything else, anyone could.
    bool CouldUseOne(Player* bot, const std::string& kind)
    {
        static const std::vector<std::pair<std::string, std::vector<uint32>>> kSkills = {
            { "wand", { SKILL_WANDS } },
            { "staff", { SKILL_STAVES } },
            { "bow", { SKILL_BOWS } },
            { "gun", { SKILL_GUNS } },
            { "crossbow", { SKILL_CROSSBOWS } },
            { "thrown weapon", { SKILL_THROWN } },
            { "ranged weapon", { SKILL_BOWS, SKILL_GUNS, SKILL_CROSSBOWS, SKILL_THROWN } },
            { "fist weapon", { SKILL_FIST_WEAPONS } },
            { "two-hander", { SKILL_2H_AXES, SKILL_2H_MACES, SKILL_2H_SWORDS, SKILL_POLEARMS, SKILL_STAVES } },
            { "sword", { SKILL_SWORDS, SKILL_2H_SWORDS } },
            { "axe", { SKILL_AXES, SKILL_2H_AXES } },
            { "mace", { SKILL_MACES, SKILL_2H_MACES } },
            { "dagger", { SKILL_DAGGERS } },
            { "polearm", { SKILL_POLEARMS } },
            { "shield", { SKILL_SHIELD } },
        };
        for (const auto& [key, skills] : kSkills)
            if (kind == key)
                return std::any_of(skills.begin(), skills.end(), [bot](uint32 s) { return bot->GetSkillValue(s) != 0; });
        if (kind == "relic")
            return bot->getClass() == CLASS_PALADIN || bot->getClass() == CLASS_DRUID ||
                   bot->getClass() == CLASS_SHAMAN || bot->getClass() == CLASS_DEATH_KNIGHT;
        return true;
    }

    // ---- the dungeons and what their bosses drop -----------------------------------

    // Gear worth naming: a weapon or a piece of armour, green or better.
    bool Gear(uint32 itemId)
    {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(itemId);
        return p && (p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR) &&
               p->Quality >= ITEM_QUALITY_UNCOMMON && !(p->Flags & ITEM_FLAG_DEPRECATED);
    }

    void Build()
    {
        if (g.built)
            return;
        g.built = true;
        const auto started = Clock::now();

        // The dungeon finder's dungeons, by map and difficulty, with the
        // levels it takes them for.
        std::map<std::pair<uint32_t, uint8_t>, size_t> at;
        for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
        {
            LFGDungeonEntry const* d = sLFGDungeonStore.LookupEntry(i);
            if (!d || d->TypeID != 1)   // 1: a dungeon (2 a raid, 6 a random one)
                continue;
            const auto key = std::make_pair(uint32_t(d->MapID), uint8_t(d->Difficulty));
            auto [it, fresh] = at.emplace(key, g.dungeons.size());
            if (fresh)
            {
                Dungeon dungeon;
                dungeon.map        = d->MapID;
                dungeon.difficulty = uint8_t(d->Difficulty);
                dungeon.minLevel   = d->MinLevel;
                dungeon.maxLevel   = d->MaxLevel;
                MapEntry const* m  = sMapStore.LookupEntry(d->MapID);
                dungeon.name = (m && m->name[0] && *m->name[0]) ? std::string(m->name[0])
                                                                : std::string(d->Name[0] ? d->Name[0] : "");
                g.dungeons.push_back(std::move(dungeon));
            }
            else
            {
                Dungeon& dungeon = g.dungeons[it->second];
                dungeon.minLevel = std::min<uint32_t>(dungeon.minLevel, d->MinLevel);
                dungeon.maxLevel = std::max<uint32_t>(dungeon.maxLevel, d->MaxLevel);
            }
        }

        // Their bosses, as the instance encounters credit them -- a heroic
        // one by its heroic self's loot.
        std::unordered_map<uint32_t, std::vector<std::pair<size_t, size_t>>> byLoot;   // loot id -> (dungeon, boss)
        for (size_t di = 0; di < g.dungeons.size(); ++di)
        {
            Dungeon& dungeon = g.dungeons[di];
            DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(dungeon.map,
                                                                                   Difficulty(dungeon.difficulty));
            if (!list)
                continue;
            for (DungeonEncounter const* e : *list)
            {
                if (!e || !e->dbcEntry || e->creditType != ENCOUNTER_CREDIT_KILL_CREATURE)
                    continue;
                CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(e->creditEntry);
                if (!t)
                    continue;
                CreatureTemplate const* looted = t;
                if (dungeon.difficulty > 0 && dungeon.difficulty <= MAX_DIFFICULTY - 1 &&
                    t->DifficultyEntry[dungeon.difficulty - 1])
                    if (CreatureTemplate const* hard = sObjectMgr->GetCreatureTemplate(t->DifficultyEntry[dungeon.difficulty - 1]))
                        looted = hard;
                if (!looted->lootid)
                    continue;
                Boss boss;
                boss.name           = t->Name;
                boss.encounterIndex = e->dbcEntry->encounterIndex;
                dungeon.bosses.push_back(std::move(boss));
                byLoot[looted->lootid].emplace_back(di, dungeon.bosses.size() - 1);
            }
        }

        if (!byLoot.empty())
        {
            std::vector<uint32_t> lootIds;
            for (const auto& [id, where] : byLoot)
                lootIds.push_back(id);

            // Their own rows: gear dropped outright, and the tables they
            // refer to. A row with a small chance of its own, outside any
            // group, is a world drop listed by hand: not theirs to name.
            std::unordered_map<uint32_t, std::vector<uint32_t>> direct, refs;
            std::unordered_set<uint32_t> allRefs;
            if (QueryResult r = WorldDatabase.Query(
                    "SELECT Entry, Item, Reference, Chance, GroupId FROM creature_loot_template WHERE Entry IN ({})",
                    JoinIds(lootIds)))
                do
                {
                    Field* f = r->Fetch();
                    const uint32 entry  = f[0].Get<uint32>();
                    const uint32 item   = f[1].Get<uint32>();
                    const int32  ref    = f[2].Get<int32>();
                    const float  chance = f[3].Get<float>();
                    const uint8  group  = f[4].Get<uint8>();
                    if (ref > 0)
                    {
                        refs[entry].push_back(uint32(ref));
                        allRefs.insert(uint32(ref));
                    }
                    else if (group || chance == 0.0f || chance >= 3.0f)
                        direct[entry].push_back(item);
                } while (r->NextRow());

            // Of those tables, the ones that are theirs: a world-drop table
            // is shared by thousands of creatures.
            std::unordered_set<uint32_t> own;
            if (!allRefs.empty())
                if (QueryResult r = WorldDatabase.Query(
                        "SELECT Reference, COUNT(*) FROM creature_loot_template WHERE Reference IN ({}) GROUP BY "
                        "Reference",
                        JoinIds(std::vector<uint32_t>(allRefs.begin(), allRefs.end()))))
                    do
                    {
                        Field* f = r->Fetch();
                        if (f[1].Get<uint64>() <= kOwnTableMaxSharers)
                            own.insert(uint32(f[0].Get<int32>()));
                    } while (r->NextRow());

            // What is in them, a few levels down.
            std::unordered_map<uint32_t, std::vector<uint32_t>> tableItems, tableRefs;
            std::unordered_set<uint32_t> read;
            std::vector<uint32_t> next(own.begin(), own.end());
            for (int depth = 0; depth < 3 && !next.empty(); ++depth)
            {
                for (uint32_t id : next)
                    read.insert(id);
                std::vector<uint32_t> deeper;
                if (QueryResult r = WorldDatabase.Query(
                        "SELECT Entry, Item, Reference FROM reference_loot_template WHERE Entry IN ({})", JoinIds(next)))
                    do
                    {
                        Field* f = r->Fetch();
                        const uint32 entry = f[0].Get<uint32>();
                        const int32  ref   = f[2].Get<int32>();
                        if (ref > 0)
                        {
                            tableRefs[entry].push_back(uint32(ref));
                            if (!read.contains(uint32(ref)))
                                deeper.push_back(uint32(ref));
                        }
                        else
                            tableItems[entry].push_back(f[1].Get<uint32>());
                    } while (r->NextRow());
                std::sort(deeper.begin(), deeper.end());
                deeper.erase(std::unique(deeper.begin(), deeper.end()), deeper.end());
                next = std::move(deeper);
            }

            std::function<void(uint32_t, std::vector<uint32_t>&, int)> collect =
                [&](uint32_t table, std::vector<uint32_t>& out, int depth)
            {
                if (depth > 3)
                    return;
                for (uint32_t item : tableItems[table])
                    out.push_back(item);
                for (uint32_t ref : tableRefs[table])
                    collect(ref, out, depth + 1);
            };

            for (const auto& [lootId, where] : byLoot)
            {
                std::vector<uint32_t> items = direct[lootId];
                for (uint32_t ref : refs[lootId])
                    if (own.contains(ref))
                        collect(ref, items, 0);
                items.erase(std::remove_if(items.begin(), items.end(), [](uint32_t id) { return !Gear(id); }),
                            items.end());
                std::sort(items.begin(), items.end());
                items.erase(std::unique(items.begin(), items.end()), items.end());
                for (const auto& [di, bi] : where)
                    g.dungeons[di].bosses[bi].items = items;
            }
        }

        // Only bosses with gear to drop, and dungeons with such bosses.
        for (Dungeon& d : g.dungeons)
            d.bosses.erase(std::remove_if(d.bosses.begin(), d.bosses.end(),
                                          [](const Boss& b) { return b.items.empty(); }),
                           d.bosses.end());
        g.dungeons.erase(std::remove_if(g.dungeons.begin(), g.dungeons.end(),
                                        [](const Dungeon& d) { return d.bosses.empty(); }),
                         g.dungeons.end());

        g_stats.dungeons = uint32_t(g.dungeons.size());
        g_stats.bosses   = 0;
        for (const Dungeon& d : g.dungeons)
            g_stats.bosses += uint32_t(d.bosses.size());
        LOG_INFO("module.ollamachat", "[Ollama Chat] Wants: what {} bosses in {} dungeons drop, read in {} ms.",
                 g_stats.bosses, g_stats.dungeons,
                 std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    }

    struct Drop
    {
        const Boss*         boss = nullptr;
        ItemTemplate const* item = nullptr;
    };

    std::vector<Drop> DropsIn(const Dungeon& d, Player* who, const std::string& kind, uint32 lo, uint32 hi)
    {
        std::vector<Drop> out;
        for (const Boss& b : d.bosses)
            for (uint32_t id : b.items)
                if (ItemTemplate const* p = sObjectMgr->GetItemTemplate(id))
                    if (OfKind(p, kind) && CanUse(who, p, kind))
                    {
                        const uint32 need = NeedsLevel(p);
                        if (need >= lo && need <= hi)
                            out.push_back(Drop{ &b, p });
                    }
        return out;
    }

    // What is so about getting one, for `who`: sentences for the prompt.
    std::string FactsFor(Player* who, const TextRules::ItemKindWords& kind)
    {
        Build();
        const uint32 level = who->GetLevel();
        std::vector<std::string> parts;

        // Here: which of this dungeon's bosses can drop one, and whether it
        // is down already.
        Map* map = who->GetMap();
        const Dungeon* here = nullptr;
        if (map && map->IsDungeon())
            for (const Dungeon& d : g.dungeons)
                if (d.map == map->GetId() && d.difficulty == uint8_t(map->GetDifficulty()))
                    here = &d;
        std::string hereName;
        if (map && map->IsDungeon())
        {
            MapEntry const* m = sMapStore.LookupEntry(map->GetId());
            hereName = here ? here->name : (m && m->name[0] ? std::string(m->name[0]) : std::string());
        }
        if (here)
        {
            std::vector<Drop> drops = DropsIn(*here, who, kind.key, 1, level + 6);
            if (drops.empty())
                parts.push_back("None of the bosses here in " + here->name + " drops " + kind.phrase +
                                " they could use.");
            else
            {
                const InstanceScript* script = who->GetInstanceScript();
                const uint32 done = script ? script->GetCompletedEncounterMask() : 0;
                std::shuffle(drops.begin(), drops.end(), Rng());
                std::vector<std::string> said;
                std::unordered_set<const Boss*> named;
                for (const Drop& d : drops)
                {
                    if (named.size() >= 2)
                        break;
                    if (!named.insert(d.boss).second)
                        continue;
                    const bool down = d.boss->encounterIndex < 32 && (done & (1u << d.boss->encounterIndex));
                    said.push_back(d.boss->name + " can drop [" + d.item->Name1 + "]" +
                                   (down ? " (already killed this run)" : ""));
                }
                parts.push_back("Here in " + here->name + ", " + JoinWords(said) + ".");
            }
        }
        else if (!hereName.empty())
            parts.push_back("What the bosses here in " + hereName + " drop is not something you know.");

        // Elsewhere: a dungeon or two around their level where a boss can.
        std::vector<const Dungeon*> near;
        for (const Dungeon& d : g.dungeons)
            if (d.difficulty == 0 && (!map || d.map != map->GetId()) && level + 3 >= d.minLevel &&
                level <= d.maxLevel + 3)
                near.push_back(&d);
        std::shuffle(near.begin(), near.end(), Rng());
        std::vector<std::string> elsewhere;
        for (const Dungeon* d : near)
        {
            if (elsewhere.size() >= 2)
                break;
            std::vector<Drop> drops = DropsIn(*d, who, kind.key, level > 8 ? level - 8 : 1, level + 4);
            if (drops.empty())
                continue;
            const Drop& pick = drops[std::uniform_int_distribution<size_t>(0, drops.size() - 1)(Rng())];
            elsewhere.push_back(d->name + " (" + pick.boss->name + " can drop [" + pick.item->Name1 + "])");
        }
        if (!elsewhere.empty())
            parts.push_back("Dungeons around their level where a boss can drop one: " + JoinWords(elsewhere) + ".");
        else
            parts.push_back("No other dungeon around their level has a boss that drops one.");

        // Made: what a profession makes of the kind around their level.
        std::vector<std::string> made;
        for (const auto& [item, skills] : Knowledge_MadeBy())
        {
            ItemTemplate const* p = sObjectMgr->GetItemTemplate(item);
            if (!p || skills.empty() || p->Quality < ITEM_QUALITY_NORMAL || (p->Flags & ITEM_FLAG_DEPRECATED) ||
                !OfKind(p, kind.key) || !CanUse(who, p, kind.key))
                continue;
            const uint32 need = NeedsLevel(p);
            if (need > level + 2 || need + 10 < level)
                continue;
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(skills.front());
            if (!line || !line->name[0] || !*line->name[0])
                continue;
            made.push_back(std::string(line->name[0]) + " makes [" + p->Name1 + "]");
        }
        std::shuffle(made.begin(), made.end(), Rng());
        if (made.size() > 2)
            made.resize(2);
        if (!made.empty())
            parts.push_back("Around their level, " + JoinWords(made) + ".");

        std::string out;
        for (const std::string& p : parts)
            out += (out.empty() ? "" : " ") + p;
        return out;
    }

    std::string ClassName(Player* p)
    {
        ChrClassesEntry const* c = sChrClassesStore.LookupEntry(p->getClass());
        std::string name = (c && c->name[0]) ? std::string(c->name[0]) : std::string("adventurer");
        for (char& ch : name)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return name;
    }

    uint32_t GroupOf(Player* p)
    {
        Group* group = p ? p->GetGroup() : nullptr;
        return group ? group->GetGUID().GetCounter() : 0;
    }

    // `templ`: Wants.Text for the line that says it, Wants.FollowText for
    // one a moment after.
    std::string Prompt(Player* bot, const Want& w, const std::string& templ)
    {
        if (templ.empty())
            return {};
        const std::string self = CouldUseOne(bot, w.kind->key) ? "You could use one yourself."
                                                               : "You could not use one yourself.";
        const std::string out = SafeFormat(templ, fmt::arg("speaker", w.name), fmt::arg("kind", w.kind->phrase),
                                           fmt::arg("level", w.level), fmt::arg("class", w.cls),
                                           fmt::arg("facts", w.facts), fmt::arg("self", self));
        return out == "[Format Error]" ? std::string() : out;
    }
}

std::string Wants_PromptFor(Player* bot, Player* speaker, const std::string& msg)
{
    if (!g_Enable || !g_WantsEnable || !bot || !speaker || bot == speaker)
        return {};
    const auto now = Clock::now();
    const uint64_t key = speaker->GetGUID().GetRawValue();

    // A line saying its speaker is after something: what is so about it,
    // worked out once for everyone who answers.
    if (const TextRules::ItemKindWords* kind =
            TextRules::FindItemWant(CrowdText::Words(Context_CleanText(msg)), g_WantsWords))
    {
        Want& w = g_wants[key];
        if (w.kind != kind || now >= w.until)
        {
            w.kind  = kind;
            w.facts = FactsFor(speaker, *kind);
            w.name  = speaker->GetName();
            w.level = speaker->GetLevel();
            w.cls   = ClassName(speaker);
            ++g_stats.heard;
        }
        w.group = GroupOf(speaker);
        w.until = now + std::chrono::seconds(g_WantsFollowSeconds);
        ++g_stats.prompts;
        return Prompt(bot, w, g_WantsText);
    }

    if (g_wants.empty() || g_WantsFollowSeconds == 0)
        return {};

    // A moment later: their own next line ("south of where?"), or one of
    // their group answering them.
    auto it = g_wants.find(key);
    if (it == g_wants.end() || now >= it->second.until)
    {
        it = g_wants.end();
        const uint32_t group = GroupOf(speaker);
        if (group)
            for (auto w = g_wants.begin(); w != g_wants.end(); ++w)
                if (w->second.group == group && now < w->second.until)
                {
                    it = w;
                    break;
                }
    }
    if (it == g_wants.end())
        return {};
    ++g_stats.prompts;
    ++g_stats.followUps;
    return Prompt(bot, it->second, g_WantsFollowText);
}

void Wants_Update()
{
    const auto now = Clock::now();
    for (auto it = g_wants.begin(); it != g_wants.end();)
        it = now >= it->second.until ? g_wants.erase(it) : std::next(it);
}

WantsStats Wants_GetStats()
{
    return g_stats;
}
