#include "mod-ollama-chat_needs.h"
#include "mod-ollama-chat_bags.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"

#include "Bag.h"
#include "DBCStores.h"
#include "Item.h"
#include "Map.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct NeedsBotState
    {
        Clock::time_point lastAsk{};
        std::unordered_map<std::string, Clock::time_point> asked;   // key -> when
        std::string       reason;
        Clock::time_point reasonAt{};
    };
    std::unordered_map<uint64_t, NeedsBotState> g_bots;
    std::unordered_map<uint64_t, Clock::time_point> g_zones;   // zone * 8 + kind -> last ask there
    NeedsStats g_stats;

    std::string Fmt(const std::string& templ, auto&&... args)
    {
        if (templ.empty())
            return {};
        std::string out = SafeFormat(templ, args...);
        return out == "[Format Error]" ? std::string() : out;
    }

    std::string MoneyText(uint32_t copper)
    {
        const uint32_t gold = copper / 10000, silver = (copper / 100) % 100, rest = copper % 100;
        if (gold)
            return silver ? fmt::format("{}g {}s", gold, silver) : fmt::format("{}g", gold);
        if (silver)
            return rest ? fmt::format("{}s {}c", silver, rest) : fmt::format("{}s", silver);
        return fmt::format("{}c", rest);
    }

    std::string AreaName(uint32_t id)
    {
        AreaTableEntry const* area = id ? sAreaTableStore.LookupEntry(id) : nullptr;
        return (area && area->area_name[0] && *area->area_name[0]) ? std::string(area->area_name[0]) : std::string();
    }

    uint32_t FreeSlots(Player* bot)
    {
        uint32_t free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++free;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                free += bag->GetFreeSlots();
        return free;
    }

    bool HasPotion(Player* bot)
    {
        auto isPotion = [](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            return proto && proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_POTION;
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (isPotion(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
                return true;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    if (isPotion(bag->GetItemByPos(uint8(i))))
                        return true;
        return false;
    }

    bool Cooling(const NeedsBotState* state, const std::string& key, Clock::time_point now)
    {
        if (!state)
            return false;
        auto it = state->asked.find(key);
        return it != state->asked.end() && now - it->second < std::chrono::minutes(g_NeedsRepeatMinutes);
    }

    void GatherQuests(Player* bot, bool hub, const NeedsBotState* state, Clock::time_point now, std::vector<BotNeed>& out)
    {
        const int32 level = int32(bot->GetLevel());
        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            const uint32 type = quest->GetType();
            const bool dungeon = type == QUEST_TYPE_DUNGEON || type == QUEST_TYPE_HEROIC;
            const bool group   = type == QUEST_TYPE_ELITE || quest->GetSuggestedPlayers() > 1;
            if (!dungeon && !group)
                continue;
            const int32 questLevel = quest->GetQuestLevel();
            if (questLevel > 0 && (level > questLevel + 4 || questLevel > level + 3))
                continue;   // soloable by now, or out of its league

            const int32 zoneOrSort = quest->GetZoneOrSort();
            const std::string where = zoneOrSort > 0 ? AreaName(uint32(zoneOrSort)) : std::string();
            const bool here = zoneOrSort > 0 && uint32(zoneOrSort) == bot->GetZoneId();
            if (!here && !hub)
                continue;   // out in the world: what is around here
            if (dungeon && where.empty())
                continue;

            const std::string key = "quest:" + std::to_string(questId);
            if (Cooling(state, key, now))
                continue;

            std::string target;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT && target.empty(); ++i)
                if (quest->RequiredNpcOrGo[i] > 0)
                    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(uint32(quest->RequiredNpcOrGo[i])))
                        target = ct->Name;

            const std::string questName = "[" + quest->GetTitle() + "]";
            const std::string kind = dungeon ? std::string("a dungeon quest")
                                   : quest->GetSuggestedPlayers() > 1
                                       ? fmt::format("a group quest for {} players", quest->GetSuggestedPlayers())
                                       : std::string("an elite quest");
            BotNeed n;
            n.kind  = NeedKind::Quest;
            n.key   = key;
            n.party = true;
            const std::string& askTempl = dungeon ? g_NeedsAskDungeon : g_NeedsAskQuest;
            const std::string targetText = target.empty() ? std::string()
                                                          : fmt::format(" (you have to kill {})", target);
            const std::string whereText = where.empty() ? std::string("around here") : where;
            n.ask = Fmt(askTempl,
                        fmt::arg("quest", questName),
                        fmt::arg("target", targetText),
                        fmt::arg("where", whereText),
                        fmt::arg("kind", kind));
            n.reason = Fmt(g_NeedsReasonQuest,
                           fmt::arg("quest", questName),
                           fmt::arg("target", targetText),
                           fmt::arg("where", whereText),
                           fmt::arg("kind", kind));
            if (!n.ask.empty())
                out.push_back(std::move(n));
        }
    }

    void GatherErrands(Player* bot, const NeedsBotState* state, Clock::time_point now, std::vector<BotNeed>& out)
    {
        const std::string city = AreaName(bot->GetZoneId());
        auto add = [&](const char* id, std::string why, std::string what)
        {
            const std::string key = std::string("errand:") + id;
            if (Cooling(state, key, now))
                return;
            BotNeed n;
            n.kind = NeedKind::Errand;
            n.key  = key;
            n.ask  = Fmt(g_NeedsAskErrand, fmt::arg("city", city), fmt::arg("why", why), fmt::arg("what", what));
            n.reason = Fmt(g_NeedsReasonErrand, fmt::arg("city", city), fmt::arg("why", why), fmt::arg("what", what));
            if (!n.ask.empty())
                out.push_back(std::move(n));
        };

        // Gear nearly broken.
        Item* worst = nullptr;
        float worstPct = 1.0f;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            const uint32 max = item ? item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) : 0;
            if (!max)
                continue;
            const float pct = float(item->GetUInt32Value(ITEM_FIELD_DURABILITY)) / float(max);
            if (pct < worstPct)
            {
                worst = item;
                worstPct = pct;
            }
        }
        if (worst && worstPct < 0.25f && worst->GetTemplate())
            add("repair", fmt::format("your [{}] is nearly broken", worst->GetTemplate()->Name1),
                "where you can get your gear repaired here");

        if (FreeSlots(bot) <= 2)
            add("bags", "your bags are full", "where the nearest vendor or the auction house is");

        if (bot->GetLevel() >= 8 && !HasPotion(bot))
            add("potions", "you have no healing potions left", "where you can buy healing potions here");

        if (bot->GetLevel() >= 20 && !bot->HasSkill(SKILL_RIDING))
            add("riding", fmt::format("you are level {} and still cannot ride", bot->GetLevel()),
                "where the riding trainer is");

        // A profession at the top of what it has trained, with the next rank
        // open to its level: 150 at 10, 225 at 20, 300 at 35, 375 at 50, 450 at 65.
        static const uint32 professions[] = { SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                                              SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING,
                                              SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING,
                                              SKILL_COOKING, SKILL_FIRST_AID, SKILL_FISHING };
        for (uint32 skill : professions)
        {
            if (!bot->HasSkill(skill))
                continue;
            const uint16 value = bot->GetPureSkillValue(skill);
            const uint16 max   = bot->GetPureMaxSkillValue(skill);
            if (!max || value < max || max >= 450)
                continue;
            const uint32 next = max < 150 ? 10 : max < 225 ? 20 : max < 300 ? 35 : max < 375 ? 50 : 65;
            if (bot->GetLevel() < next)
                continue;
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
            if (!line || !line->name[0] || !*line->name[0])
                continue;
            const std::string name = line->name[0];
            add(("trainer:" + std::to_string(skill)).c_str(),
                fmt::format("your {} is stuck at {} until you train the next rank", name, value),
                fmt::format("where the {} trainer is here", name));
            break;
        }
    }

    void GatherSpare(Player* bot, const NeedsBotState* state, Clock::time_point now, std::vector<BotNeed>& out)
    {
        for (const BagEntry& e : Bags_Scan(bot))
        {
            if (!e.spare)
                break;   // spare first
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(e.entry);
            const bool worth = e.quality >= ITEM_QUALITY_UNCOMMON ||
                               (proto && proto->Class == ITEM_CLASS_TRADE_GOODS && e.count >= 5) ||
                               (e.recent && e.quality >= ITEM_QUALITY_NORMAL);
            if (!worth)
                continue;
            const std::string key = "spare:" + std::to_string(e.entry);
            if (Cooling(state, key, now))
                continue;
            const std::string item = e.count > 1 ? fmt::format("[{}] x{}", e.name, e.count) : "[" + e.name + "]";
            BotNeed n;
            n.kind      = NeedKind::Spare;
            n.key       = key;
            n.itemEntry = e.entry;
            n.party     = true;
            n.ask = Fmt(g_NeedsAskSpare, fmt::arg("item", item),
                        fmt::arg("looted", e.recent ? std::string(", which you just looted,") : std::string()));
            n.reason = Fmt(g_NeedsReasonSpare, fmt::arg("item", item));
            if (!n.ask.empty())
                out.push_back(std::move(n));
            return;   // one is plenty
        }
    }

    void GatherShort(Player* bot, const NeedsBotState* state, Clock::time_point now, std::vector<BotNeed>& out)
    {
        if (bot->GetLevel() > g_NeedsShortMaxLevel || bot->GetLevel() < 3)
            return;
        const uint32 money = bot->GetMoney();
        if (money >= bot->GetLevel() * g_NeedsShortCopperPerLevel || Cooling(state, "short", now))
            return;
        const std::string want = FreeSlots(bot) <= 4 ? std::string("a bigger bag")
                                                     : std::string("your next skills at the trainer");
        BotNeed n;
        n.kind   = NeedKind::Short;
        n.key    = "short";
        n.ask    = Fmt(g_NeedsAskShort, fmt::arg("money", MoneyText(money)), fmt::arg("want", want));
        n.reason = Fmt(g_NeedsReasonShort, fmt::arg("money", MoneyText(money)), fmt::arg("want", want));
        if (!n.ask.empty())
            out.push_back(std::move(n));
    }

    uint64_t ZoneKey(Player* bot, NeedKind kind)
    {
        return uint64_t(bot->GetZoneId()) * 8 + uint64_t(kind);
    }
}

bool Needs_Pick(Player* bot, bool hub, BotNeed& out)
{
    if (!g_NeedsEnable || !bot || !bot->IsInWorld())
        return false;
    const auto now = Clock::now();
    const uint64_t guid = bot->GetGUID().GetRawValue();
    auto found = g_bots.find(guid);
    const NeedsBotState* state = found == g_bots.end() ? nullptr : &found->second;
    if (state && state->lastAsk != Clock::time_point{} &&
        now - state->lastAsk < std::chrono::minutes(g_NeedsBotCooldownMinutes))
        return false;

    auto zoneFree = [&](NeedKind kind)
    {
        auto it = g_zones.find(ZoneKey(bot, kind));
        return it == g_zones.end() || now - it->second >= std::chrono::seconds(g_NeedsZoneCooldownSeconds);
    };

    // Each kind by its weight, only those that have something to ask.
    std::vector<BotNeed> candidates;
    std::vector<uint32_t> weights;
    auto gather = [&](NeedKind kind, auto&& fn)
    {
        const uint32_t weight = g_NeedsWeight[static_cast<size_t>(kind)];
        if (!weight || !zoneFree(kind))
            return;
        std::vector<BotNeed> found;
        fn(found);
        if (found.empty())
            return;
        candidates.push_back(std::move(found[urand(0, uint32(found.size() - 1))]));
        weights.push_back(weight);
    };
    // Inside a dungeon only a spare thing is worth saying: nobody asks their
    // own party to run the dungeon they are in.
    const bool instance = bot->GetMap() && bot->GetMap()->Instanceable();
    if (!instance)
        gather(NeedKind::Quest, [&](std::vector<BotNeed>& v) { GatherQuests(bot, hub, state, now, v); });
    if (hub && !instance)
        gather(NeedKind::Errand, [&](std::vector<BotNeed>& v) { GatherErrands(bot, state, now, v); });
    gather(NeedKind::Spare, [&](std::vector<BotNeed>& v) { GatherSpare(bot, state, now, v); });
    if (!instance)
        gather(NeedKind::Short, [&](std::vector<BotNeed>& v) { GatherShort(bot, state, now, v); });

    if (candidates.empty())
    {
        ++g_stats.none;
        return false;
    }
    uint32_t total = 0;
    for (uint32_t w : weights)
        total += w;
    uint32_t roll = urand(1, total);
    size_t pick = 0;
    while (pick + 1 < weights.size() && roll > weights[pick])
        roll -= weights[pick++];
    out = std::move(candidates[pick]);
    ++g_stats.picked;
    return true;
}

void Needs_NoteAsked(Player* bot, const BotNeed& need)
{
    if (!bot)
        return;
    const auto now = Clock::now();
    NeedsBotState& state = g_bots[bot->GetGUID().GetRawValue()];
    state.lastAsk = now;
    state.asked[need.key] = now;
    state.reason   = need.reason;
    state.reasonAt = now;
    g_zones[ZoneKey(bot, need.kind)] = now;
    ++g_stats.asked[static_cast<size_t>(need.kind)];
}

std::string Needs_PromptFor(Player* bot)
{
    if (!g_NeedsEnable || !bot || g_NeedsReasonText.empty())
        return {};
    auto it = g_bots.find(bot->GetGUID().GetRawValue());
    if (it == g_bots.end() || it->second.reason.empty() ||
        Clock::now() - it->second.reasonAt > std::chrono::minutes(g_NeedsRememberMinutes))
        return {};
    return Fmt(g_NeedsReasonText, fmt::arg("reason", it->second.reason));
}

void Needs_Update()
{
    const auto now = Clock::now();
    const auto keep = std::chrono::minutes(std::max<uint32_t>(g_NeedsRepeatMinutes,
                                                              std::max(g_NeedsBotCooldownMinutes, g_NeedsRememberMinutes)));
    for (auto it = g_bots.begin(); it != g_bots.end();)
    {
        NeedsBotState& s = it->second;
        for (auto a = s.asked.begin(); a != s.asked.end();)
            a = (now - a->second > keep) ? s.asked.erase(a) : std::next(a);
        if (!s.reason.empty() && now - s.reasonAt > std::chrono::minutes(g_NeedsRememberMinutes))
            s.reason.clear();
        it = (s.asked.empty() && s.reason.empty() && now - s.lastAsk > keep) ? g_bots.erase(it) : std::next(it);
    }
    for (auto it = g_zones.begin(); it != g_zones.end();)
        it = (now - it->second > std::chrono::seconds(g_NeedsZoneCooldownSeconds)) ? g_zones.erase(it) : std::next(it);
}

NeedsStats Needs_GetStats()
{
    return g_stats;
}
