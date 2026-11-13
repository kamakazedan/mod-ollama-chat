#include "mod-ollama-chat_facts.h"
#include "mod-ollama-chat-utilities.h"

#include "DBCStores.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    // Not a test, placeholder or retired entry: the data has plenty of those.
    // Whole words, so "Testament" and "Old Murk-Eye" stay.
    bool Sane(const std::string& name)
    {
        if (name.size() < 3 || std::isdigit(static_cast<unsigned char>(name[0])))
            return false;
        const std::string low = AsciiLower(name);
        static const char* const kBadParts[] = { "[ph]", "(ph)", "[old]", "(old)", "[dnd]", "(dnd)", "do not use",
                                                 "monster -", "(test)", "[test]" };
        for (const char* bad : kBadParts)
            if (low.find(bad) != std::string::npos)
                return false;
        static const char* const kBadStarts[] = { "zz", "qa ", "test " };
        for (const char* bad : kBadStarts)
            if (low.rfind(bad, 0) == 0)
                return false;
        static const std::unordered_set<std::string> kBadWords = { "test", "testing", "tester", "deprecated", "unused",
                                                                    "debug", "placeholder", "dnd", "nyi", "ph" };
        std::string word;
        for (size_t i = 0; i <= low.size(); ++i)
        {
            if (i < low.size() && std::isalnum(static_cast<unsigned char>(low[i])))
                word += low[i];
            else
            {
                if (kBadWords.contains(word))
                    return false;
                word.clear();
            }
        }
        return true;
    }

    // Names with the level they go with.
    using Pool = std::vector<std::pair<uint32_t, std::string>>;

    // One of `pool` whose level is within [lo, hi], not in `taken`.
    std::string PickBand(const Pool& pool, uint32_t lo, uint32_t hi, const std::vector<std::string>& taken)
    {
        std::vector<const std::string*> fits;
        for (const auto& [lvl, name] : pool)
            if (lvl >= lo && lvl <= hi && std::find(taken.begin(), taken.end(), name) == taken.end())
                fits.push_back(&name);
        if (fits.empty())
            return {};
        return *fits[std::uniform_int_distribution<size_t>(0, fits.size() - 1)(Rng())];
    }

    const Pool& Trinkets()
    {
        static Pool pool;
        static bool built = false;
        if (!built)
        {
            built = true;
            if (ItemTemplateContainer const* items = sObjectMgr->GetItemTemplateStore())
                for (const auto& [id, t] : *items)
                    if (t.InventoryType == INVTYPE_TRINKET && t.Quality >= ITEM_QUALITY_RARE &&
                        t.Quality <= ITEM_QUALITY_EPIC && t.RequiredLevel > 0 &&
                        !t.HasFlag(ITEM_FLAG_DEPRECATED) && Sane(t.Name1))
                        pool.emplace_back(t.RequiredLevel, t.Name1);
        }
        return pool;
    }

    const Pool& ItemSets()
    {
        static Pool pool;
        static bool built = false;
        if (!built)
        {
            built = true;
            std::unordered_set<std::string> seen;
            for (uint32 i = 0; i < sItemSetStore.GetNumRows(); ++i)
            {
                ItemSetEntry const* set = sItemSetStore.LookupEntry(i);
                if (!set || !set->name[0] || !*set->name[0] || !Sane(set->name[0]) || !seen.insert(set->name[0]).second)
                    continue;
                // Its pieces are real, rare or better, and say what level it is for.
                for (uint32 itemId : set->itemId)
                {
                    ItemTemplate const* t = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
                    if (!t)
                        continue;
                    if (t->Quality >= ITEM_QUALITY_RARE && t->RequiredLevel > 0 && !t->HasFlag(ITEM_FLAG_DEPRECATED))
                        pool.emplace_back(t->RequiredLevel, set->name[0]);
                    break;
                }
            }
        }
        return pool;
    }

    const Pool& Rares()
    {
        static Pool pool;
        static bool built = false;
        if (!built)
        {
            built = true;
            // Only the ones the world actually spawns.
            std::unordered_set<uint32> spawned;
            for (const auto& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                spawned.insert(data.id);
            std::unordered_set<std::string> seen;
            if (CreatureTemplateContainer const* creatures = sObjectMgr->GetCreatureTemplates())
                for (const auto& [entry, t] : *creatures)
                    if ((t.rank == CREATURE_ELITE_RARE || t.rank == CREATURE_ELITE_RAREELITE) && spawned.contains(entry) &&
                        t.minlevel > 0 && Sane(t.Name) && seen.insert(t.Name).second)
                        pool.emplace_back(t.minlevel, t.Name);
        }
        return pool;
    }

    // "the Deadmines" -> "deadmines"; "Ahn'kahet: The Old Kingdom" -> "ahnkahet the old kingdom".
    std::string Key(const std::string& name)
    {
        std::string out;
        for (char c : AsciiLower(name))
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
                out += c;
            else if (c == ' ' || c == '-' || c == ':')
            {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
            }
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        if (out.rfind("the ", 0) == 0)
            out = out.substr(4);
        return out;
    }

    // The dungeon finder's name for each dungeon and raid, as a key, to its map.
    const std::vector<std::pair<std::string, uint32>>& Instances()
    {
        static std::vector<std::pair<std::string, uint32>> list;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
            {
                LFGDungeonEntry const* d = sLFGDungeonStore.LookupEntry(i);
                if (!d || !d->Name[0] || !*d->Name[0] || (d->TypeID != 1 && d->TypeID != 2))
                    continue;
                list.emplace_back(Key(d->Name[0]), d->MapID);
            }
        }
        return list;
    }
}

std::string Facts_Trinket(uint32_t level, const std::vector<std::string>& taken)
{
    const uint32_t lo = level > 15 ? level - 15 : 1;
    return PickBand(Trinkets(), lo, std::max<uint32_t>(level, 10), taken);
}

std::string Facts_ItemSet(uint32_t level, const std::vector<std::string>& taken)
{
    const uint32_t lo = level > 25 ? level - 25 : 1;
    return PickBand(ItemSets(), lo, std::max<uint32_t>(level, 20), taken);
}

std::string Facts_Rare(uint32_t level, const std::vector<std::string>& taken)
{
    const uint32_t lo = level > 10 ? level - 10 : 1;
    return PickBand(Rares(), lo, level + 5, taken);
}

std::string Facts_Bosses(const std::string& instance)
{
    // The few the dungeon finder calls something else.
    static const std::unordered_map<std::string, std::string> kAlias = {
        { "mount hyjal", "hyjal summit" },           { "tempest keep", "eye" },
        { "temple of ahnqiraj", "ahnqiraj temple" }, { "sunwell", "sunwell plateau" },
    };
    std::string key = Key(instance);
    if (auto it = kAlias.find(key); it != kAlias.end())
        key = it->second;
    if (key.empty())
        return " ";

    std::unordered_set<uint32> maps;
    for (const auto& [name, map] : Instances())
        if (name == key || name.rfind(key + " ", 0) == 0)   // "scarlet monastery" is four wings of one map
            maps.insert(map);
    if (maps.empty())
        return " ";

    std::vector<std::pair<uint32, std::string>> found;   // by encounter index
    std::unordered_set<std::string> seen;
    for (uint32 i = 0; i < sDungeonEncounterStore.GetNumRows(); ++i)
    {
        DungeonEncounterEntry const* e = sDungeonEncounterStore.LookupEntry(i);
        if (!e || e->difficulty != 0 || !maps.contains(e->mapId) || !e->encounterName[0] || !*e->encounterName[0])
            continue;
        if (seen.insert(e->encounterName[0]).second)
            found.emplace_back(e->encounterIndex, e->encounterName[0]);
    }
    if (found.empty())
        return " ";
    std::sort(found.begin(), found.end());
    if (found.size() > 8)
        found.resize(8);
    std::string out = " (its bosses: ";
    for (size_t i = 0; i < found.size(); ++i)
        out += (i ? (i + 1 == found.size() ? " and " : ", ") : "") + found[i].second;
    return out + ")";
}

std::string Facts_ClassRaces(const std::string& cls)
{
    static const std::pair<const char*, uint8> kClasses[] = {
        { "warrior", CLASS_WARRIOR }, { "paladin", CLASS_PALADIN }, { "hunter", CLASS_HUNTER },
        { "rogue", CLASS_ROGUE },     { "priest", CLASS_PRIEST },   { "death knight", CLASS_DEATH_KNIGHT },
        { "shaman", CLASS_SHAMAN },   { "mage", CLASS_MAGE },       { "warlock", CLASS_WARLOCK },
        { "druid", CLASS_DRUID },
    };
    static const std::pair<uint8, const char*> kRaces[] = {
        { RACE_HUMAN, "humans" },   { RACE_ORC, "orcs" },         { RACE_DWARF, "dwarves" },
        { RACE_NIGHTELF, "night elves" }, { RACE_UNDEAD_PLAYER, "undead" }, { RACE_TAUREN, "tauren" },
        { RACE_GNOME, "gnomes" },   { RACE_TROLL, "trolls" },     { RACE_BLOODELF, "blood elves" },
        { RACE_DRAENEI, "draenei" },
    };
    const std::string low = AsciiLower(cls);
    uint8 classId = 0;
    for (const auto& [name, id] : kClasses)
        if (low.find(name) != std::string::npos)
            classId = id;
    if (!classId)
        return {};
    std::vector<std::string> races;
    for (const auto& [race, name] : kRaces)
        if (sObjectMgr->GetPlayerInfo(race, classId))
            races.emplace_back(name);
    if (races.empty())
        return {};
    std::string out;
    for (size_t i = 0; i < races.size(); ++i)
        out += (i ? (i + 1 == races.size() ? " and " : ", ") : "") + races[i];
    return out;
}
