#include "mod-ollama-chat_raregear.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_drops.h"
#include "mod-ollama-chat_facts.h"
#include "mod-ollama-chat_knowledge.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat-utilities.h"

#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    std::unordered_map<uint32_t, Clock::time_point>    g_itemSaid;    // a thing -> when it was last talked of
    std::unordered_map<std::string, Clock::time_point> g_placeSaid;   // a city's General -> when it last heard one
    std::unordered_map<uint64_t, Clock::time_point>    g_botSaid;     // a bot -> when it last said one
    std::unordered_map<uint32_t, bool>                 g_rare;        // a thing -> really rare, worked out once
    std::vector<uint32_t>                              g_pool;        // every really rare thing: what bots ask after
    bool                                               g_poolBuilt = false;
    RareGearStats                                      g_stats;

    // What "really rare" was worked out against: a reload that changes it
    // starts over.
    struct Rules
    {
        float    maxDrop = 0.0f, minDrop = 0.0f, maxBest = 0.0f;
        uint32_t quality = 0;

        bool operator==(Rules const&) const = default;
    };
    Rules g_rules;

    void Fresh()
    {
        const Rules now{ g_RareGearMaxDropPct, g_RareGearMinDropPct, g_RareGearMaxBestPct, g_RareGearMinQuality };
        if (now == g_rules)
            return;
        g_rules = now;
        g_rare.clear();
        g_pool.clear();
        g_poolBuilt  = false;
        g_stats.pool = 0;
    }

    template <typename Map, typename Key>
    bool Within(const Map& map, const Key& key, uint32_t minutes, Clock::time_point now)
    {
        auto it = map.find(key);
        return it != map.end() && now - it->second < std::chrono::minutes(minutes);
    }

    template <typename Map>
    void Prune(Map& map, uint32_t minutes, Clock::time_point now)
    {
        if (map.size() < 1024)
            return;
        for (auto it = map.begin(); it != map.end();)
            it = now - it->second >= std::chrono::minutes(minutes) ? map.erase(it) : std::next(it);
    }

    bool Rare(ItemTemplate const* p)
    {
        if (!p)
            return false;
        auto known = g_rare.find(p->ItemId);
        if (known != g_rare.end())
            return known->second;
        bool rare = false;
        if ((p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR) && p->InventoryType != INVTYPE_NON_EQUIP &&
            p->Quality >= g_RareGearMinQuality && !p->HasFlag(ITEM_FLAG_DEPRECATED))
        {
            const double pct = Drops_Pct(p->ItemId);
            DropsFrom from;
            rare = pct > 0.0 && pct < g_RareGearMaxDropPct && pct >= g_RareGearMinDropPct &&
                   Drops_Sources(p->ItemId, from) && from.bestPct < g_RareGearMaxBestPct && from.spawns > 0 &&
                   Facts_SaneName(p->Name1);
        }
        g_rare[p->ItemId] = rare;
        return rare;
    }

    uint32_t WeightOf(const std::unordered_map<std::string, uint32_t>& styles, uint32_t fallback,
                      const std::string& style)
    {
        auto it = styles.find(style);
        return it == styles.end() ? fallback : it->second;
    }

    // Stats for its class: no strength sword for a mage, no intellect robe
    // for a rogue. Only the clear cases: a hybrid takes anything, and so does
    // a piece with neither kind of stat.
    bool StatsSuit(Player* bot, ItemTemplate const* p)
    {
        bool body = false, mind = false;
        for (uint32 i = 0; i < p->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
        {
            if (!p->ItemStat[i].ItemStatValue)
                continue;
            switch (p->ItemStat[i].ItemStatType)
            {
                case ITEM_MOD_STRENGTH:
                case ITEM_MOD_AGILITY:
                case ITEM_MOD_ATTACK_POWER:
                case ITEM_MOD_RANGED_ATTACK_POWER:
                    body = true;
                    break;
                case ITEM_MOD_INTELLECT:
                case ITEM_MOD_SPIRIT:
                case ITEM_MOD_SPELL_POWER:
                case ITEM_MOD_SPELL_HEALING_DONE:
                case ITEM_MOD_SPELL_DAMAGE_DONE:
                case ITEM_MOD_MANA_REGENERATION:
                    mind = true;
                    break;
                default:
                    break;
            }
        }
        switch (bot->getClass())
        {
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
                return !body || mind;
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_DEATH_KNIGHT:
            case CLASS_HUNTER:
                return !mind || body;
            default:
                return true;
        }
    }

    // A thing the bot could wear: its class and race may, it has the weapon
    // skill, the heaviest armour it wears for a piece of armour of a kind (a
    // cloak, a ring or a trinket is any), stats for its class.
    bool Wearable(Player* bot, ItemTemplate const* p)
    {
        const InventoryResult can = bot->CanUseItem(p);
        if (can != EQUIP_ERR_OK && can != EQUIP_ERR_CANT_EQUIP_LEVEL_I)
            return false;
        const uint32 skill = p->GetSkill();
        if (p->Class == ITEM_CLASS_WEAPON && (!skill || !bot->HasSkill(skill)))
            return false;
        if (p->Class == ITEM_CLASS_ARMOR)
        {
            if (p->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH && p->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE &&
                p->InventoryType != INVTYPE_CLOAK)
            {
                const uint32 heaviest = bot->HasSkill(SKILL_PLATE_MAIL) ? ITEM_SUBCLASS_ARMOR_PLATE
                                      : bot->HasSkill(SKILL_MAIL)       ? ITEM_SUBCLASS_ARMOR_MAIL
                                      : bot->HasSkill(SKILL_LEATHER)    ? ITEM_SUBCLASS_ARMOR_LEATHER
                                                                        : ITEM_SUBCLASS_ARMOR_CLOTH;
                if (p->SubClass != heaviest)
                    return false;
            }
            else if (skill && !bot->HasSkill(skill))
                return false;   // a shield
        }
        return StatsSuit(bot, p);
    }

    // A really rare thing it has on, not talked of lately.
    ItemTemplate const* PickWorn(Player* bot, Clock::time_point now)
    {
        std::vector<ItemTemplate const*> worn;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (ItemTemplate const* p = item->GetTemplate())
                    if (Rare(p) && !Within(g_itemSaid, p->ItemId, g_RareGearItemMinutes, now))
                        worn.push_back(p);
        if (worn.empty())
            return nullptr;
        return worn[urand(0, uint32(worn.size() - 1))];
    }

    void BuildPool()
    {
        if (g_poolBuilt)
            return;
        g_poolBuilt = true;
        for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
            if (Rare(&proto))
                g_pool.push_back(id);
        std::sort(g_pool.begin(), g_pool.end());
        g_stats.pool = uint32_t(g_pool.size());
    }

    // A really rare thing it would want: one it could wear, about its level,
    // that it does not have, not talked of lately.
    ItemTemplate const* PickWanted(Player* bot, Clock::time_point now)
    {
        BuildPool();
        const uint32 level = bot->GetLevel();
        const uint32 lo = level > g_RareGearAskLevelsBelow ? level - g_RareGearAskLevelsBelow : 1;
        const uint32 hi = level + g_RareGearAskLevelsAbove;
        std::vector<ItemTemplate const*> wanted;
        for (uint32_t id : g_pool)
        {
            ItemTemplate const* p = sObjectMgr->GetItemTemplate(id);
            if (!p)
                continue;
            const uint32 need = std::max<uint32>(1, p->RequiredLevel);
            if (need < lo || need > hi || Within(g_itemSaid, id, g_RareGearItemMinutes, now))
                continue;
            if (!Wearable(bot, p) || bot->HasItemCount(id, 1, true))
                continue;
            wanted.push_back(p);
        }
        if (wanted.empty())
            return nullptr;
        return wanted[urand(0, uint32(wanted.size() - 1))];
    }

    std::string Pick(const std::vector<std::string>& list)
    {
        return list.empty() ? std::string() : list[urand(0, uint32(list.size() - 1))];
    }

    template <typename... Args>
    std::string Fill(const std::string& templ, Args&&... args)
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ, std::forward<Args>(args)...);
        return out == "[Format Error]" ? std::string() : out;
    }

    // What it is, after its name: "an epic one-handed mace, item level 62,
    // for level 57 and up, binds when equipped, a rare drop: about once in
    // 18,000 kills of anything that can drop it".
    std::string WhatItIs(ItemTemplate const* p)
    {
        const std::string all = Links_DescribeItem(p);
        const std::string head = p->Name1 + " is an item, not a person or a creature: ";
        return all.rfind(head, 0) == 0 ? all.substr(head.size()) : all;
    }

    bool Write(Player* bot, ItemTemplate const* p, RareGearKind kind, RareGearLine& out)
    {
        const bool show = kind == RareGearKind::Show;
        const std::string what  = WhatItIs(p);
        const std::string facts = Knowledge_DropFacts(p);
        out.kind   = kind;
        out.itemId = p->ItemId;
        out.name   = p->Name1;
        out.environment = Fill(show ? g_RareGearShowText : g_RareGearAskText, fmt::arg("item", p->Name1),
                               fmt::arg("what", what),
                               fmt::arg("angle", Pick(show ? g_RareGearShowAngles : g_RareGearAskAngles)));
        out.premise = Fill(show ? g_RareGearShowPremise : g_RareGearAskPremise, fmt::arg("name", bot->GetName()),
                           fmt::arg("item", p->Name1), fmt::arg("what", what), fmt::arg("facts", facts));
        const std::string stock = Fill(Pick(show ? g_RareGearShowLines : g_RareGearAskLines),
                                       fmt::arg("item", p->Name1));
        out.fallback = stock.empty() ? std::string() : Personality_StyleCanned(bot, stock);
        return !out.environment.empty();
    }
}

bool RareGear_PlaceFree(const std::string& placeKey)
{
    return !Within(g_placeSaid, placeKey, g_RareGearPlaceMinutes, Clock::now());
}

bool RareGear_Pick(Player* bot, const std::string& placeKey, RareGearLine& out)
{
    out = {};
    if (!g_RareGearEnable || !bot || !Drops_SourcesKnown())
        return false;
    Fresh();
    const auto now = Clock::now();
    if (Within(g_botSaid, bot->GetGUID().GetRawValue(), g_RareGearBotMinutes, now) ||
        Within(g_placeSaid, placeKey, g_RareGearPlaceMinutes, now))
        return false;

    // Its style decides whether it would show one off, or ask after one --
    // either first, so neither always wins.
    const std::string style = GetBotPersonality(bot);
    const bool showFirst = urand(0, 1) == 0;
    bool wanted = false;
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool show = (pass == 0) == showFirst;
        const uint32_t weight = show ? WeightOf(g_RareGearShowStyles, g_RareGearShowDefault, style)
                                     : WeightOf(g_RareGearAskStyles, g_RareGearAskDefault, style);
        if (!weight || urand(1, 100) > weight)
            continue;
        wanted = true;
        ItemTemplate const* p = show ? PickWorn(bot, now) : PickWanted(bot, now);
        if (p && Write(bot, p, show ? RareGearKind::Show : RareGearKind::Ask, out))
            return true;
    }
    if (wanted)
        ++g_stats.none;
    return false;
}

void RareGear_NoteSaid(Player* bot, const RareGearLine& line, const std::string& placeKey, bool conversation)
{
    const auto now = Clock::now();
    g_itemSaid[line.itemId] = now;
    g_placeSaid[placeKey]   = now;
    if (bot)
        g_botSaid[bot->GetGUID().GetRawValue()] = now;
    if (line.kind == RareGearKind::Show)
        ++g_stats.shown;
    else
        ++g_stats.asked;
    if (conversation)
        ++g_stats.conversations;
    Prune(g_itemSaid, g_RareGearItemMinutes, now);
    Prune(g_placeSaid, g_RareGearPlaceMinutes, now);
    Prune(g_botSaid, g_RareGearBotMinutes, now);
}

bool RareGear_IsRare(uint32_t itemId)
{
    Fresh();
    return Rare(sObjectMgr->GetItemTemplate(itemId));
}

RareGearStats RareGear_GetStats()
{
    return g_stats;
}
