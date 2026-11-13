#include "mod-ollama-chat_knowledge.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "CreatureData.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "GossipDef.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "NPCHandler.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // One place a town's guards give directions to.
    struct Direction
    {
        std::string label;                // "Leatherworking", "Bank of Stormwind"
        std::string text;                 // what the guard says about it
        std::string mark;                 // what the guard marks on the minimap; empty when nothing
        std::vector<std::string> keys;    // the words of the label a question has to have
        bool trainer      = false;        // under a "... Trainer" heading: "the leatherworking trainer"
        bool classTrainer = false;        // a class's trainer: only when a trainer is asked for
    };

    // A heading of the menu: "Class Trainer", "Profession Trainer"; in
    // Shattrath "Bank", which leads to the Aldor bank and the Scryers bank.
    struct Heading
    {
        std::string label;
        std::vector<std::string> keys;
        std::vector<size_t> children;      // its own places, into Menu::dirs: "Druid", "Hunter"...
        std::vector<std::string> more;     // headings under it, by name: Dalaran's inns go on to "Sewers"
    };

    struct Menu
    {
        std::vector<Direction> dirs;
        std::vector<Heading>   headings;
    };

    struct Spawn
    {
        uint32_t entry = 0;
        uint32_t map   = 0;
        float    x = 0.0f, y = 0.0f, z = 0.0f;
        uint64_t id    = 0;
    };

    // A zone's rectangle on the world map, in its map's coordinates.
    struct ZoneRect
    {
        uint32_t zone = 0;
        float    xMin = 0.0f, xMax = 0.0f, yMin = 0.0f, yMax = 0.0f;
        float    size = 0.0f;
    };

    // A town, city, district or zone a question could name.
    struct NamedArea
    {
        uint32_t zone = 0;                // the zone it is, or is in
        std::vector<std::string> words;
    };

    struct Index
    {
        bool built = false;
        std::unordered_map<uint32_t, Menu>     menus;       // gossip menu -> the directions in it
        std::unordered_map<uint32_t, uint32_t> giverMenu;   // creature entry -> its directions
        std::vector<Spawn>                     giverSpawns;
        std::unordered_map<uint32_t, uint32_t> zoneMenu;    // zone -> the directions given there
        std::unordered_map<uint32_t, std::vector<uint32_t>> vendorsOf;      // item -> vendors in the world
        std::unordered_map<uint32_t, std::vector<Spawn>>    vendorSpawns;   // vendor -> where they stand
        std::unordered_map<uint32_t, std::vector<uint32_t>> madeBy;         // item -> professions
        std::unordered_map<uint32_t, std::vector<ZoneRect>> rects;          // map -> its zones
        std::vector<NamedArea> areas;
    };
    Index g;

    std::unordered_map<uint64_t, std::string> g_placeOf;   // spawn -> "Trade District, Stormwind City"
    KnowledgeStats g_stats;

    // Who is on a question a moment after it was asked: the one who asked
    // it, and the bots that were given the answer for it. A line from either
    // is a follow-up -- "turn what in?", or the troll's "near the entrance by
    // the forge" -- and a reply to it gets the same answer.
    struct OnTopic
    {
        std::string question;
        uint64_t    asker = 0;
        std::string askerName;
        std::chrono::steady_clock::time_point at;
    };
    std::unordered_map<uint64_t, OnTopic> g_onTopic;   // speaker -> the question

    template <typename... Args>
    std::string Say(const std::string& templ, Args&&... args)
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ, std::forward<Args>(args)...);
        return out == "[Format Error]" ? std::string() : out;
    }

    std::string AreaName(uint32_t id)
    {
        AreaTableEntry const* a = id ? sAreaTableStore.LookupEntry(id) : nullptr;
        return (a && a->area_name[0] && *a->area_name[0]) ? std::string(a->area_name[0]) : std::string();
    }

    // "trainers" asks about a trainer, "weapon" about the weapons trainer.
    bool Same(const std::string& a, const std::string& b)
    {
        auto plural = [](const std::string& longer, const std::string& shorter)
        {
            return longer.size() == shorter.size() + 1 && longer.back() == 's' &&
                   longer.compare(0, shorter.size(), shorter) == 0;
        };
        return a == b || plural(a, b) || plural(b, a);
    }

    bool HasWord(const std::vector<std::string>& words, const std::string& key)
    {
        return std::any_of(words.begin(), words.end(), [&](const std::string& w) { return Same(w, key); });
    }

    bool HasAll(const std::vector<std::string>& words, const std::vector<std::string>& keys)
    {
        return !keys.empty() &&
               std::all_of(keys.begin(), keys.end(), [&](const std::string& k) { return HasWord(words, k); });
    }

    // The words of a label a question about it has to have: "Bank of
    // Stormwind" is the bank, wherever you are asking.
    std::vector<std::string> KeysOf(const std::string& label)
    {
        static const std::unordered_set<std::string> skip = {
            "the", "of", "a", "an", "and", "to", "in", "at", "for", "s", "city", "stormwind", "ironforge",
            "darnassus", "exodar", "orgrimmar", "undercity", "thunder", "bluff", "silvermoon", "shattrath", "dalaran"
        };
        std::vector<std::string> keys;
        for (const std::string& w : CrowdText::Words(label))
            if (!skip.contains(w) && std::find(keys.begin(), keys.end(), w) == keys.end())
                keys.push_back(w);
        return keys;
    }

    // A place's name as people say it: "Stormwind City" is "stormwind".
    std::vector<std::string> AreaWords(const std::string& name)
    {
        std::vector<std::string> out;
        for (const std::string& w : CrowdText::Words(name))
            if (w != "the" && w != "of" && w != "city")
                out.push_back(w);
        return out;
    }

    std::string Broadcast(uint32_t id)
    {
        BroadcastText const* bt = id ? sObjectMgr->GetBroadcastText(id) : nullptr;
        return bt ? bt->GetText(DEFAULT_LOCALE, GENDER_MALE) : std::string();
    }

    // What a gossip menu says: the first of its texts there is.
    std::string MenuText(uint32_t menuId)
    {
        auto menus = sObjectMgr->GetGossipMenusMapBounds(menuId);
        for (auto it = menus.first; it != menus.second; ++it)
        {
            GossipText const* text = sObjectMgr->GetGossipText(it->second.TextID);
            if (!text)
                continue;
            for (const GossipTextOption& o : text->Options)
            {
                if (!o.Text_0.empty())
                    return o.Text_0;
                if (!o.Text_1.empty())
                    return o.Text_1;
                const std::string said = Broadcast(o.BroadcastTextID);
                if (!said.empty())
                    return said;
            }
        }
        return {};
    }

    bool HasOptions(uint32_t menuId)
    {
        auto items = sObjectMgr->GetGossipMenuItemsMapBounds(menuId);
        return items.first != items.second;
    }

    // A guard's menu, a heading deep ("Profession Trainer" -> "Leatherworking"):
    // each place, what the guard says about it and what it marks on the map.
    // Returns the places under `menuId` (into menu.dirs); a submenu two
    // headings share -- Dalaran's arena and locksmith are both down in the
    // sewers -- is read once and given to both.
    std::vector<size_t> Walk(uint32_t menuId, const std::string& heading, int depth, Menu& menu, uint32_t& marked,
                             std::unordered_map<uint32_t, std::vector<size_t>>& walked)
    {
        auto known = walked.find(menuId);
        if (known != walked.end())
            return known->second;
        walked[menuId];   // a menu leading back to itself adds nothing more
        std::vector<size_t> found;
        if (depth > 2)
            return found;
        auto items = sObjectMgr->GetGossipMenuItemsMapBounds(menuId);
        for (auto it = items.first; it != items.second; ++it)
        {
            const GossipMenuItems& o = it->second;
            if (o.OptionType != GOSSIP_OPTION_GOSSIP || o.BoxCoded || o.BoxMoney)
                continue;
            const std::string label = !o.OptionText.empty() ? o.OptionText : Broadcast(o.OptionBroadcastTextID);
            if (label.empty())
                continue;

            if (o.ActionMenuID && o.ActionMenuID != menuId && HasOptions(o.ActionMenuID))
            {
                const std::vector<size_t> under =
                    Walk(o.ActionMenuID, depth > 0 ? heading : label, depth + 1, menu, marked, walked);
                found.insert(found.end(), under.begin(), under.end());
                if (depth > 0)
                    continue;
                // What the heading offers: its own places, and any further
                // headings under it by name.
                Heading h;
                h.label = label;
                h.keys  = KeysOf(label);
                auto offered = sObjectMgr->GetGossipMenuItemsMapBounds(o.ActionMenuID);
                for (auto c = offered.first; c != offered.second; ++c)
                {
                    const GossipMenuItems& choice = c->second;
                    if (choice.OptionType != GOSSIP_OPTION_GOSSIP || choice.BoxCoded || choice.BoxMoney)
                        continue;
                    const std::string name = !choice.OptionText.empty() ? choice.OptionText
                                                                        : Broadcast(choice.OptionBroadcastTextID);
                    if (name.empty())
                        continue;
                    if (choice.ActionMenuID && choice.ActionMenuID != o.ActionMenuID && HasOptions(choice.ActionMenuID))
                    {
                        h.more.push_back(name);
                        continue;
                    }
                    for (size_t i : under)
                        if (menu.dirs[i].label == name &&
                            std::find(h.children.begin(), h.children.end(), i) == h.children.end())
                        {
                            h.children.push_back(i);
                            break;
                        }
                }
                if (!h.keys.empty() && (!h.children.empty() || !h.more.empty()))
                    menu.headings.push_back(std::move(h));
                continue;
            }
            if (!o.ActionMenuID && !o.ActionPoiID)
                continue;

            PointOfInterest const* poi = o.ActionPoiID ? sObjectMgr->GetPointOfInterest(o.ActionPoiID) : nullptr;
            Direction d;
            d.label = label;
            d.text  = o.ActionMenuID ? MenuText(o.ActionMenuID) : std::string();
            d.mark  = poi ? poi->Name : std::string();
            if (d.text.empty())
                d.text = d.mark;
            d.keys = KeysOf(label);
            if (!heading.empty())
            {
                const std::vector<std::string> words = CrowdText::Words(heading);
                d.trainer      = HasWord(words, "trainer");
                d.classTrainer = d.trainer && HasWord(words, "class");
            }
            if (d.text.empty() || d.keys.empty())
                continue;
            marked += poi ? 1 : 0;
            menu.dirs.push_back(std::move(d));
            found.push_back(menu.dirs.size() - 1);
        }
        walked[menuId] = found;
        return found;
    }

    uint32_t ZoneByRect(uint32_t map, float x, float y)
    {
        auto it = g.rects.find(map);
        if (it == g.rects.end())
            return 0;
        ZoneRect const* best = nullptr;
        for (const ZoneRect& r : it->second)
            if (x >= r.xMin && x <= r.xMax && y >= r.yMin && y <= r.yMax && (!best || r.size < best->size))
                best = &r;
        return best ? best->zone : 0;
    }

    // The zone and area of a spot: exact where the map around it is loaded
    // already, else the smallest zone whose world map rectangle holds it.
    // Nothing is loaded for this. True when exact.
    bool ZoneAt(uint32_t map, float x, float y, float z, uint32_t& zone, uint32_t& area)
    {
        zone = area = 0;
        if (Map* base = sMapMgr->FindBaseMap(map))
            if (base->IsGridCreated(x, y))
            {
                uint32 z0 = 0, a0 = 0;
                base->GetZoneAndAreaId(PHASEMASK_NORMAL, z0, a0, x, y, z);
                if (z0)
                {
                    zone = z0;
                    area = a0;
                    return true;
                }
            }
        zone = ZoneByRect(map, x, y);
        return false;
    }

    // Where a spawn stands: "Trade District, Stormwind City".
    std::string PlaceOf(const Spawn& s)
    {
        auto known = g_placeOf.find(s.id);
        if (known != g_placeOf.end())
            return known->second;
        uint32_t zone = 0, area = 0;
        const bool exact = ZoneAt(s.map, s.x, s.y, s.z, zone, area);
        const std::string z = AreaName(zone), a = AreaName(area);
        const std::string out = (!a.empty() && area != zone && !z.empty()) ? a + ", " + z : (z.empty() ? a : z);
        if (exact)
            g_placeOf[s.id] = out;
        return out;
    }

    void Build()
    {
        if (g.built)
            return;
        g.built = true;
        const auto started = std::chrono::steady_clock::now();

        // Zones' world map rectangles, and the names of towns, cities, their
        // districts and zones.
        for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
        {
            AreaTableEntry const* a = sAreaTableStore.LookupEntry(i);
            if (!a)
                continue;
            MapEntry const* map = sMapStore.LookupEntry(a->mapid);
            if (!map || map->Instanceable())
                continue;   // "the stockade" in a question is not a town to give directions in
            if (!a->zone)
            {
                float x1 = 0.0f, y1 = 0.0f, x2 = 100.0f, y2 = 100.0f;
                Zone2MapCoordinates(x1, y1, a->ID);
                Zone2MapCoordinates(x2, y2, a->ID);
                const bool listed = !(x1 == 0.0f && y1 == 0.0f && x2 == 100.0f && y2 == 100.0f);
                ZoneRect r;
                r.zone = a->ID;
                r.xMin = std::min(x1, x2);
                r.xMax = std::max(x1, x2);
                r.yMin = std::min(y1, y2);
                r.yMax = std::max(y1, y2);
                r.size = (r.xMax - r.xMin) * (r.yMax - r.yMin);
                if (listed && r.size > 0.0f)
                    g.rects[a->mapid].push_back(r);
            }
            const bool town = !a->zone || (a->flags & (AREA_FLAG_SLAVE_CAPITAL | AREA_FLAG_SLAVE_CAPITAL2 |
                                                       AREA_FLAG_CAPITAL | AREA_FLAG_TOWN));
            if (!town || !a->area_name[0])
                continue;
            NamedArea n;
            n.zone  = a->zone ? a->zone : a->ID;
            n.words = AreaWords(a->area_name[0]);
            if (n.words.empty() || (n.words.size() == 1 && n.words[0].size() < 5))
                continue;   // "Inn", "Park": too easily a word of the question
            g.areas.push_back(std::move(n));
        }

        // Who gives directions: a menu of a few places, some of them marked.
        std::unordered_set<uint32_t> vendorEntries;
        std::unordered_set<uint32_t> walked;
        for (auto const& [entry, t] : *sObjectMgr->GetCreatureTemplates())
        {
            if (t.GossipMenuId)
            {
                if (walked.insert(t.GossipMenuId).second)
                {
                    Menu menu;
                    uint32_t marked = 0;
                    std::unordered_map<uint32_t, std::vector<size_t>> seen;
                    Walk(t.GossipMenuId, std::string(), 0, menu, marked, seen);
                    if (menu.dirs.size() >= 3 && marked >= 2)
                        g.menus.emplace(t.GossipMenuId, std::move(menu));
                }
                if (g.menus.contains(t.GossipMenuId))
                    g.giverMenu[entry] = t.GossipMenuId;
            }
            if (sObjectMgr->GetNpcVendorItemList(entry))
                vendorEntries.insert(entry);
        }

        // A place whose name is only the kind of place it is -- a district
        // called "Auction House" -- is not a town a question names: "where is
        // the auction house" asked in Stormwind is not about Orgrimmar's.
        {
            std::unordered_set<std::string> kinds;
            for (auto const& [id, menu] : g.menus)
            {
                for (const Direction& d : menu.dirs)
                    kinds.insert(d.keys.begin(), d.keys.end());
                for (const Heading& h : menu.headings)
                    kinds.insert(h.keys.begin(), h.keys.end());
            }
            g.areas.erase(std::remove_if(g.areas.begin(), g.areas.end(), [&](const NamedArea& a)
            {
                return std::all_of(a.words.begin(), a.words.end(), [&](const std::string& w)
                {
                    return kinds.contains(w) || kinds.contains(w + "s") ||
                           (w.size() > 1 && w.back() == 's' && kinds.contains(w.substr(0, w.size() - 1)));
                });
            }), g.areas.end());
        }

        // Where they stand -- not in an instance, nor only for a holiday.
        std::unordered_set<uint64_t> eventOnly;
        const size_t events = sGameEventMgr->GetEventMap().size();
        for (size_t i = events; i < sGameEventMgr->GameEventCreatureGuids.size(); ++i)
            for (ObjectGuid::LowType guid : sGameEventMgr->GameEventCreatureGuids[i])
                eventOnly.insert(guid);
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            const bool giver = g.giverMenu.contains(data.id);
            if ((!giver && !vendorEntries.contains(data.id)) || eventOnly.contains(spawnId))
                continue;
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            if (!map || map->Instanceable())
                continue;
            Spawn s;
            s.entry = data.id;
            s.map   = data.mapid;
            s.x     = data.posX;
            s.y     = data.posY;
            s.z     = data.posZ;
            s.id    = spawnId;
            if (giver)
                g.giverSpawns.push_back(s);
            if (vendorEntries.contains(data.id))
            {
                uint32 npcflag = 0, unitFlags = 0, dynamicFlags = 0;
                ObjectMgr::ChooseCreatureFlags(sObjectMgr->GetCreatureTemplate(data.id), npcflag, unitFlags,
                                               dynamicFlags, &data);
                if (npcflag & UNIT_NPC_FLAG_VENDOR)
                    g.vendorSpawns[data.id].push_back(s);
            }
        }

        // Each zone's directions: the menu most of its guards give.
        std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> given;   // zone -> menu -> guards
        for (const Spawn& s : g.giverSpawns)
            if (const uint32_t zone = ZoneByRect(s.map, s.x, s.y))
                ++given[zone][g.giverMenu[s.entry]];
        for (auto const& [zone, menus] : given)
        {
            uint32_t best = 0, most = 0;
            for (auto const& [menu, count] : menus)
                if (count > most || (count == most && g.menus[menu].dirs.size() > g.menus[best].dirs.size()))
                {
                    best = menu;
                    most = count;
                }
            if (best)
                g.zoneMenu[zone] = best;
        }

        // What the vendors out in the world sell.
        for (auto const& [entry, spawns] : g.vendorSpawns)
            if (VendorItemData const* stock = sObjectMgr->GetNpcVendorItemList(entry))
                for (VendorItem const* item : stock->m_items)
                    if (item)
                    {
                        std::vector<uint32_t>& sellers = g.vendorsOf[item->item];
                        if (std::find(sellers.begin(), sellers.end(), entry) == sellers.end())
                            sellers.push_back(entry);
                    }

        // Which professions make what.
        for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
            if (!spell)
                continue;
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            {
                const uint32 effect = spell->Effects[e].Effect;
                const uint32 made   = spell->Effects[e].ItemType;
                if ((effect != SPELL_EFFECT_CREATE_ITEM && effect != SPELL_EFFECT_CREATE_ITEM_2) || !made)
                    continue;
                auto skills = sSpellMgr->GetSkillLineAbilityMapBounds(id);
                for (auto it = skills.first; it != skills.second; ++it)
                {
                    SkillLineEntry const* line = sSkillLineStore.LookupEntry(it->second->SkillLine);
                    if (!line || (line->categoryId != SKILL_CATEGORY_PROFESSION &&
                                  line->categoryId != SKILL_CATEGORY_SECONDARY))
                        continue;
                    std::vector<uint32_t>& by = g.madeBy[made];
                    if (std::find(by.begin(), by.end(), line->id) == by.end())
                        by.push_back(line->id);
                }
            }
        }

        g_stats.menus = uint32_t(g.menus.size());
        g_stats.sold  = uint32_t(g.vendorsOf.size());
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Knowledge indexed in {} ms: {} guard direction menus ({} guards, {} zones), {} "
                 "items sold by {} vendors, {} items made by a profession, {} place names.",
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                     .count(),
                 g.menus.size(), g.giverSpawns.size(), g.zoneMenu.size(), g.vendorsOf.size(), g.vendorSpawns.size(),
                 g.madeBy.size(), g.areas.size());
    }

    // The words of a question, and what its shorthand stands for: "ah" asks
    // about the auction house, "fp" about whichever flight master it is.
    std::vector<std::string> Expanded(const std::vector<std::string>& words)
    {
        std::vector<std::string> out = words;
        for (const KnowledgeSynonym& s : g_KnowledgeSynonyms)
            if (CrowdText::ContainsPhrase(words, s.triggers))
                for (const std::string& w : s.adds)
                    if (std::find(out.begin(), out.end(), w) == out.end())
                        out.push_back(w);
        return out;
    }

    // Whose directions a question wants: the guards of a place it names --
    // "in ironforge", "in goldshire", "the trade district" -- else of the
    // nearest guard to the asker. Naming somewhere with no such guards gets
    // none, rather than another town's answer.
    Menu const* MenuFor(Player* asker, const std::vector<std::string>& words, std::string& town)
    {
        uint32_t chosen = 0;
        size_t bestScore = 0;
        bool named = false;
        for (const NamedArea& a : g.areas)
        {
            if (!HasAll(words, a.words))
                continue;
            named = true;
            if (!g.zoneMenu.contains(a.zone))
                continue;
            // Where the asker is first, then the longest name.
            const size_t score = a.words.size() + (asker && asker->GetZoneId() == a.zone ? 100 : 0);
            if (!chosen || score > bestScore)
            {
                chosen = a.zone;
                bestScore = score;
            }
        }
        if (chosen)
        {
            town = AreaName(chosen);
            return &g.menus[g.zoneMenu[chosen]];
        }
        if (named || !asker)
            return nullptr;

        Spawn const* nearest = nullptr;
        float best = g_KnowledgeGuardRange;
        for (const Spawn& s : g.giverSpawns)
        {
            if (s.map != asker->GetMapId())
                continue;
            const float d = std::hypot(s.x - asker->GetPositionX(), s.y - asker->GetPositionY());
            if (d <= best)
            {
                best = d;
                nearest = &s;
            }
        }
        if (!nearest)
            return nullptr;
        uint32_t zone = 0, area = 0;
        ZoneAt(nearest->map, nearest->x, nearest->y, nearest->z, zone, area);
        town = AreaName(zone);
        return &g.menus[g.giverMenu[nearest->entry]];
    }

    // The place a question asks about: every word of its label (or most of a
    // long one), the one with more of them first. A class's trainer only when
    // a trainer is asked for ("the mage district" is not the mage trainer);
    // a profession's, without one, only by its own name ("where do i buy
    // fish" is not the fishing trainer, "where is leatherworking" is).
    Direction const* Match(const Menu& menu, const std::vector<std::string>& words,
                           const std::vector<std::string>& said, bool trainerWanted)
    {
        Direction const* best = nullptr;
        size_t bestHits = 0;
        bool bestAll = false;
        for (const Direction& d : menu.dirs)
        {
            if (d.classTrainer && !trainerWanted)
                continue;
            const std::vector<std::string>& pool = (d.trainer && !trainerWanted) ? said : words;
            size_t hits = 0;
            for (const std::string& k : d.keys)
                hits += HasWord(pool, k) ? 1 : 0;
            const bool all = hits == d.keys.size();
            if (!all && (hits < 2 || hits * 2 < d.keys.size()))
                continue;
            if (!best || (all && !bestAll) || (all == bestAll && hits > bestHits))
            {
                best = &d;
                bestHits = hits;
                bestAll = all;
            }
        }
        return best;
    }

    // "Leatherworking" under "Profession Trainer" is the leatherworking
    // trainer; "The inn" is the inn, "Bank of Stormwind" stays as it is.
    std::string PlaceName(const Direction& d)
    {
        if (d.trainer)
            return "the " + AsciiLower(d.label) + " trainer";
        std::string name = d.label;
        if (name.size() > 4 && name.compare(0, 4, "The ") == 0)
            name[0] = 't';
        return name;
    }

    // What a guard says, to the one asking: "$N" is their name.
    std::string GuardSays(const std::string& text, Player* asker, size_t maxLen)
    {
        std::string name, cls, race;
        bool female = false;
        if (asker)
        {
            name   = asker->GetName();
            female = asker->getGender() == GENDER_FEMALE;
            if (ChrClassesEntry const* c = sChrClassesStore.LookupEntry(asker->getClass()))
                cls = c->name[0];
            if (ChrRacesEntry const* r = sChrRacesStore.LookupEntry(asker->getRace()))
                race = r->name[0];
        }
        return TextRules::QuestText(text, name.empty() ? std::string("friend") : name, cls, race, female, maxLen);
    }

    std::string Marked(const std::string& marks)
    {
        const std::string out = marks.empty() ? std::string() : Say(g_KnowledgeMarkText, fmt::arg("mark", marks));
        return out.empty() ? out : " " + out;
    }

    std::string PlaceFacts(Player* asker, const std::vector<std::string>& said)
    {
        std::vector<std::string> words = Expanded(said);
        const bool trainerWanted = CrowdText::ContainsPhrase(words, g_KnowledgeTrainerWords);
        if (trainerWanted && !HasWord(words, "trainer"))
            words.push_back("trainer");   // "where can i learn a profession" asks for the trainers

        std::string town;
        Menu const* menu = MenuFor(asker, words, town);
        if (!menu)
            return {};
        if (town.empty())
            town = "town";

        if (Direction const* d = Match(*menu, words, said, trainerWanted))
        {
            const std::string out = Say(g_KnowledgePlaceText, fmt::arg("place", PlaceName(*d)),
                                        fmt::arg("town", town),
                                        fmt::arg("guard", GuardSays(d->text, asker, g_KnowledgeMaxGuardChars)),
                                        fmt::arg("marked", Marked(d->mark)));
            if (!out.empty())
                ++g_stats.places;
            return out;
        }

        for (const Heading& h : menu->headings)
        {
            if (!HasAll(words, h.keys))
                continue;
            std::string heading = AsciiLower(h.label);
            if (heading.compare(0, 4, "the ") == 0)
                heading.erase(0, 4);

            // A few places -- the Aldor bank and the Scryers bank: what is
            // said of each.
            if (h.children.size() + h.more.size() <= 3)
            {
                std::string guard, marks;
                for (size_t i : h.children)
                {
                    const Direction& d = menu->dirs[i];
                    guard += (guard.empty() ? "" : " / ") + d.label + ": " +
                             GuardSays(d.text, asker, g_KnowledgeMaxGuardChars / 2);
                    if (!d.mark.empty() && marks.find(d.mark) == std::string::npos)
                        marks += (marks.empty() ? "" : ", ") + d.mark;
                }
                for (const std::string& name : h.more)
                    guard += (guard.empty() ? "" : " / ") + name;
                const std::string out = Say(g_KnowledgePlaceText, fmt::arg("place", "the " + heading),
                                            fmt::arg("town", town), fmt::arg("guard", guard),
                                            fmt::arg("marked", Marked(marks)));
                if (!out.empty())
                    ++g_stats.places;
                return out;
            }

            // Many -- every class's trainer: that a guard knows them all.
            std::vector<std::string> choices;
            for (size_t i : h.children)
                choices.push_back(menu->dirs[i].label);
            choices.insert(choices.end(), h.more.begin(), h.more.end());
            const std::string out = Say(g_KnowledgeHeadingText, fmt::arg("heading", heading), fmt::arg("town", town),
                                        fmt::arg("choices", CrowdText::JoinNames(choices)));
            if (!out.empty())
                ++g_stats.headings;
            return out;
        }
        return {};
    }

    // Words that neither start nor end an item's name in a question.
    bool EdgeNoise(const std::string& w)
    {
        static const std::unordered_set<std::string> noise = {
            "where", "wheres", "do", "does", "i", "you", "u", "we", "get", "got", "to", "the", "a", "an", "can",
            "could", "buy", "find", "anyone", "anybody", "any1", "know", "is", "are", "of", "for", "from", "in", "at",
            "on", "my", "your", "some", "any", "how", "who", "sells", "sell", "what", "which", "need", "needs",
            "looking", "with", "and", "or", "it", "them", "they", "those", "these", "this", "that", "please", "pls",
            "plz", "ty", "more", "new", "good", "best", "cheap", "wtb", "learn", "make", "craft", "one", "ones"
        };
        return noise.contains(w);
    }

    // The item a question is about: linked, in brackets, or the longest run
    // of its words that is an item's name ("where i get rune of teleportation").
    ItemTemplate const* ItemAsked(Player* bot, const std::string& msg)
    {
        for (const TextRules::ChatLink& link : TextRules::ExtractChatLinks(msg))
            if (link.type == "item" && link.id)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(link.id))
                    return proto;
        const std::string plain = TextRules::StripChatMarkup(msg);
        for (const TextRules::BracketName& n : TextRules::FindBracketNames(plain))
            if (ItemTemplate const* proto = Links_ItemNamed(n.name, bot))
                return proto;

        // Words as typed, apostrophes kept ("mage's"), punctuation at the ends off.
        std::vector<std::string> tokens;
        std::string token;
        auto flush = [&]()
        {
            size_t b = 0, e = token.size();
            while (b < e && !std::isalnum(static_cast<unsigned char>(token[b])))
                ++b;
            while (e > b && !std::isalnum(static_cast<unsigned char>(token[e - 1])))
                --e;
            if (e > b)
                tokens.push_back(AsciiLower(token.substr(b, e - b)));
            token.clear();
        };
        for (char c : plain)
        {
            if (std::isspace(static_cast<unsigned char>(c)))
                flush();
            else
                token += c;
        }
        flush();

        for (size_t n = std::min<size_t>(6, tokens.size()); n >= 2; --n)
            for (size_t i = 0; i + n <= tokens.size(); ++i)
            {
                if (EdgeNoise(tokens[i]) || EdgeNoise(tokens[i + n - 1]))
                    continue;
                std::string name = tokens[i];
                for (size_t k = 1; k < n; ++k)
                    name += " " + tokens[i + k];
                if (ItemTemplate const* proto = Links_ItemNamed(name, bot))
                    return proto;
            }
        return nullptr;
    }

    bool Hostile(Player* asker, uint32_t entry)
    {
        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry);
        FactionTemplateEntry const* mine = asker ? asker->GetFactionTemplateEntry() : nullptr;
        FactionTemplateEntry const* theirs = t ? sFactionTemplateStore.LookupEntry(t->faction) : nullptr;
        return mine && theirs && theirs->IsHostileTo(*mine);
    }

    // "Jarel Moor (Reagents)".
    std::string Who(uint32_t entry)
    {
        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry);
        if (!t)
            return {};
        return t->SubName.empty() ? t->Name : t->Name + " (" + t->SubName + ")";
    }

    // The kinds of vendor most of them are, by what their titles share:
    // "Reagents" covers "Poisons & Reagents" and "Reagent Supplies" too.
    std::string KindsOf(const std::vector<uint32_t>& vendors)
    {
        static const std::unordered_set<std::string> generic = {
            "supplies", "supply", "goods", "vendor", "vendors", "merchant", "and", "the", "of"
        };
        struct Titled
        {
            std::string title;
            std::vector<std::string> words;
        };
        std::vector<Titled> left;
        for (uint32_t entry : vendors)
        {
            CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry);
            if (!t || t->SubName.empty())
                continue;
            Titled v;
            v.title = t->SubName;
            for (std::string w : CrowdText::Words(t->SubName))
            {
                if (generic.contains(w))
                    continue;
                if (w.size() > 3 && w.back() == 's')
                    w.pop_back();
                if (std::find(v.words.begin(), v.words.end(), w) == v.words.end())
                    v.words.push_back(w);
            }
            if (!v.words.empty())
                left.push_back(std::move(v));
        }
        const size_t total = left.size();
        if (total == 0)
            return {};

        std::vector<std::string> kinds;
        size_t covered = 0;
        while (kinds.size() < 2 && !left.empty())
        {
            std::unordered_map<std::string, size_t> count;
            for (const Titled& v : left)
                for (const std::string& w : v.words)
                    ++count[w];
            std::string word;
            size_t most = 0;
            for (auto const& [w, n] : count)
                if (n > most || (n == most && w < word))
                {
                    word = w;
                    most = n;
                }
            if (most * 100 < total * 15)
                break;
            std::unordered_map<std::string, size_t> titles;
            std::string title;
            size_t usual = 0;
            for (const Titled& v : left)
                if (std::find(v.words.begin(), v.words.end(), word) != v.words.end())
                    if (const size_t n = ++titles[v.title]; n > usual)
                    {
                        title = v.title;
                        usual = n;
                    }
            kinds.push_back(title);
            covered += most;
            left.erase(std::remove_if(left.begin(), left.end(), [&](const Titled& v)
                       { return std::find(v.words.begin(), v.words.end(), word) != v.words.end(); }),
                       left.end());
        }
        if (kinds.empty() || covered * 100 < total * 40)
            return {};
        return CrowdText::JoinNames(kinds);
    }

    std::string ItemFacts(ItemTemplate const* proto, Player* asker)
    {
        const std::string item = proto->Name1;

        std::vector<uint32_t> vendors;   // the ones that would sell to the asker
        bool enemyOnly = false;          // only the other faction's sell it
        auto sold = g.vendorsOf.find(proto->ItemId);
        if (sold != g.vendorsOf.end())
        {
            for (uint32_t entry : sold->second)
                if (!Hostile(asker, entry))
                    vendors.push_back(entry);
            enemyOnly = vendors.empty() && !sold->second.empty();
        }

        std::vector<std::string> parts;
        parts.push_back(Say(g_KnowledgeItemText, fmt::arg("item", item)));

        if (!vendors.empty())
        {
            std::unordered_map<uint32_t, uint32_t> zones;   // zone -> vendors standing there
            Spawn const* nearest = nullptr;
            Spawn const* any = nullptr;
            float best = 0.0f;
            size_t spawns = 0;
            bool gold = false;
            for (uint32_t entry : vendors)
            {
                for (const Spawn& s : g.vendorSpawns[entry])
                {
                    ++spawns;
                    any = &s;
                    if (const uint32_t zone = ZoneByRect(s.map, s.x, s.y))
                        ++zones[zone];
                    if (!asker || s.map != asker->GetMapId())
                        continue;
                    const float d = std::hypot(s.x - asker->GetPositionX(), s.y - asker->GetPositionY());
                    if (!nearest || d < best)
                    {
                        nearest = &s;
                        best = d;
                    }
                }
                if (VendorItemData const* stock = sObjectMgr->GetNpcVendorItemList(entry))
                    for (VendorItem const* v : stock->m_items)
                        if (v && v->item == proto->ItemId && v->IsGoldRequired(proto))
                            gold = true;
            }

            std::string spread;
            if (spawns == 1 && any)
                spread = Say(g_KnowledgeSpreadOneText, fmt::arg("vendor", Who(any->entry)),
                             fmt::arg("place", PlaceOf(*any)));
            else if (zones.size() >= g_KnowledgeManyZones)
                spread = g_KnowledgeSpreadManyText;
            else
            {
                // The asker's zone first, then where most of them are.
                std::vector<std::pair<uint32_t, uint32_t>> order(zones.begin(), zones.end());
                const uint32_t here = asker ? asker->GetZoneId() : 0;
                std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b)
                {
                    if ((a.first == here) != (b.first == here))
                        return a.first == here;
                    return a.second != b.second ? a.second > b.second : a.first < b.first;
                });
                std::vector<std::string> names;
                for (const auto& [zone, count] : order)
                {
                    if (names.size() == 3 && order.size() > 4)
                    {
                        names.push_back("elsewhere");
                        break;
                    }
                    const std::string name = AreaName(zone);
                    if (!name.empty())
                        names.push_back(name);
                }
                spread = Say(g_KnowledgeSpreadSomeText,
                             fmt::arg("zones", names.empty() ? std::string("a few places") : CrowdText::JoinNames(names)));
            }
            const std::string kinds = spawns > 1 ? KindsOf(vendors) : std::string();
            const std::string kind  = kinds.empty() ? std::string() : Say(g_KnowledgeKindText, fmt::arg("kinds", kinds));
            if (!kind.empty() && !spread.empty())
                spread += " -- " + kind;
            if (!spread.empty())
                parts.push_back(spread + ".");
            if (!gold)
                parts.push_back(g_KnowledgeTokensText);
            if (nearest && spawns > 1)
                parts.push_back(Say(g_KnowledgeNearestText, fmt::arg("vendor", Who(nearest->entry)),
                                    fmt::arg("place", PlaceOf(*nearest)),
                                    fmt::arg("asker", asker ? asker->GetName() : std::string("them"))));
        }

        std::vector<std::string> crafts;
        auto made = g.madeBy.find(proto->ItemId);
        if (made != g.madeBy.end())
            for (uint32_t skill : made->second)
                if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
                    if (line->name[0] && *line->name[0])
                        crafts.push_back(line->name[0]);
        if (enemyOnly)
            parts.push_back(g_KnowledgeEnemyOnlyText);
        if (!crafts.empty())
            parts.push_back(Say(vendors.empty() ? g_KnowledgeCraftText : g_KnowledgeCraftAlsoText,
                                fmt::arg("professions", CrowdText::JoinNames(crafts))));
        else if (vendors.empty() && !enemyOnly)
            parts.push_back(g_KnowledgeNoVendorText);

        if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
            parts.push_back(g_KnowledgeBoundText);
        else if (vendors.empty())
            parts.push_back(g_KnowledgeTradeText);

        std::string out;
        for (const std::string& p : parts)
            if (!p.empty())
                out += (out.empty() ? "" : " ") + p;
        return out;
    }

    // What the game says to a question: an item first ("where do i get a
    // fishing pole" is not the fishing trainer), then a place.
    std::string FactsFor(Player* bot, Player* asker, const std::string& question)
    {
        if (ItemTemplate const* proto = ItemAsked(bot, question))
        {
            const std::string out = ItemFacts(proto, asker);
            if (!out.empty())
                ++g_stats.items;
            return out;
        }
        return PlaceFacts(asker, CrowdText::Words(Context_CleanText(question)));
    }
}

bool Knowledge_IsQuestion(const std::string& msg)
{
    if (!g_KnowledgeEnable || msg.empty())
        return false;
    const std::vector<std::string> words = CrowdText::Words(Context_CleanText(msg));
    return CrowdText::ContainsPhrase(words, g_KnowledgeQuestionWords) &&
           !CrowdText::ContainsPhrase(words, g_KnowledgeNotAskingWords);
}

std::string Knowledge_PromptFor(Player* bot, Player* speaker, const std::string& msg)
{
    if (!g_KnowledgeEnable || !bot)
        return {};

    const auto now = std::chrono::steady_clock::now();
    const auto follow = std::chrono::seconds(g_KnowledgeFollowSeconds);
    const uint64_t speakerGuid = speaker ? speaker->GetGUID().GetRawValue() : 0;
    const uint64_t botGuid = bot->GetGUID().GetRawValue();
    if (g_onTopic.size() > 512)
        for (auto it = g_onTopic.begin(); it != g_onTopic.end();)
            it = now - it->second.at > follow ? g_onTopic.erase(it) : std::next(it);

    std::string facts;
    bool followUp = false;
    if (Knowledge_IsQuestion(msg))
    {
        Build();
        facts = FactsFor(bot, speaker ? speaker : bot, msg);
        if (!facts.empty() && g_KnowledgeFollowSeconds)
        {
            const OnTopic topic{ msg, speakerGuid, speaker ? speaker->GetName() : bot->GetName(), now };
            if (speakerGuid)
                g_onTopic[speakerGuid] = topic;
            g_onTopic[botGuid] = topic;
        }
    }
    else if (g_KnowledgeFollowSeconds && speakerGuid)
    {
        // A line from someone on a question that was just asked -- its asker,
        // or a bot answering it: the same answer, so a wrong one can be put
        // right -- or, by the troll, doubled down on.
        auto it = g_onTopic.find(speakerGuid);
        if (it != g_onTopic.end() && now - it->second.at <= follow)
        {
            const OnTopic topic = it->second;
            Build();
            Player* asker = topic.asker ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(topic.asker)) : nullptr;
            const std::string answer = FactsFor(bot, asker ? asker : bot, topic.question);
            if (!answer.empty())
            {
                const std::string who = topic.asker == botGuid ? std::string("You") : topic.askerName;
                const std::string earlier =
                    Say(g_KnowledgeEarlierText, fmt::arg("asker", who),
                        fmt::arg("question", TextRules::QuestText(TextRules::StripChatMarkup(topic.question), "",
                                                                  "", "", false, 160)));
                facts = earlier.empty() ? answer : earlier + " " + answer;
                followUp = true;
                ++g_stats.followUps;
                g_onTopic[botGuid] = topic;   // answering it too, now
            }
        }
    }
    if (facts.empty())
        return {};

    const std::string style = GetBotPersonality(bot);
    const bool wrong = std::find(g_KnowledgeWrongOnPurpose.begin(), g_KnowledgeWrongOnPurpose.end(), style) !=
                       g_KnowledgeWrongOnPurpose.end();
    if (wrong)
        ++g_stats.wrong;
    std::string out = facts;
    const std::string& how = wrong ? g_KnowledgeWrongText : g_KnowledgeAnswerText;
    if (!how.empty())
        out += " " + how;
    if (followUp && !wrong && !g_KnowledgeFollowText.empty())
        out += " " + g_KnowledgeFollowText;
    return out;
}

KnowledgeStats Knowledge_GetStats()
{
    return g_stats;
}

const std::unordered_map<uint32_t, std::vector<uint32_t>>& Knowledge_MadeBy()
{
    Build();
    return g.madeBy;
}
