#include "mod-ollama-chat_lately.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_world.h"

#include "Creature.h"
#include "DBCStores.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // A stretch of killing things in one zone.
    struct Run
    {
        uint32_t zone  = 0;
        time_t   first = 0;
        time_t   last  = 0;
        uint32_t count = 0;
        std::vector<std::pair<std::string, uint32_t>> kinds;   // what it killed, and how many
    };

    struct Lately
    {
        Run         kills;
        std::string quest;
        uint32_t    questId   = 0;
        uint32_t    questZone = 0;
        time_t      questAt   = 0;
        uint32_t    item        = 0;   // the best thing it got lately
        uint32_t    itemQuality = 0;
        uint32_t    itemLevel   = 0;
        time_t      itemAt      = 0;
        uint32_t    level     = 0;
        time_t      levelAt   = 0;
        time_t      touched   = 0;
    };

    std::mutex g_mutex;
    std::unordered_map<uint64_t, Lately> g_lately;

    constexpr size_t kMaxKinds  = 6;
    constexpr size_t kMaxBots   = 50000;
    constexpr time_t kRunBreak  = 20 * 60;   // this long without a kill and the next is a new run
    constexpr time_t kForget    = 3 * 3600;

    // Under the lock: one entry, room made when there are far too many.
    Lately& Entry(uint64_t guid, time_t now)
    {
        if (g_lately.size() >= kMaxBots && !g_lately.contains(guid))
            for (auto it = g_lately.begin(); it != g_lately.end();)
                it = now - it->second.touched > kForget ? g_lately.erase(it) : std::next(it);
        Lately& l = g_lately[guid];
        l.touched = now;
        return l;
    }

    std::string ZoneName(uint32_t id)
    {
        AreaTableEntry const* a = id ? sAreaTableStore.LookupEntry(id) : nullptr;
        return (a && a->area_name[0] && *a->area_name[0]) ? std::string(a->area_name[0]) : std::string();
    }

    std::string Span(time_t seconds)
    {
        if (seconds < 6 * 60)
            return "a few minutes";
        if (seconds < 55 * 60)
            return "about " + std::to_string((seconds + 150) / 300 * 5) + " minutes";
        return "over an hour";
    }

    std::string Ago(time_t seconds)
    {
        if (seconds < 2 * 60)
            return "just now";
        if (seconds < 8 * 60)
            return "a few minutes ago";
        return "about " + std::to_string((seconds + 150) / 300 * 5) + " minutes ago";
    }
}

void Lately_NoteKill(Player* killer, Creature* victim)
{
    if (!g_LatelyEnable || !killer || !victim || !OllamaIsBotPlayer(killer))
        return;
    CreatureTemplate const* t = victim->GetCreatureTemplate();
    if (!t || t->type == CREATURE_TYPE_CRITTER || victim->IsPet() || victim->IsTotem())
        return;
    // A beast by its kind ("Crab"), anything else by its name ("Murloc Coastrunner").
    std::string kind = victim->GetName();
    if (t->type == CREATURE_TYPE_BEAST && t->family)
        if (CreatureFamilyEntry const* f = sCreatureFamilyStore.LookupEntry(t->family); f && f->Name[0] && *f->Name[0])
            kind = f->Name[0];
    if (kind.empty())
        return;
    const uint32_t zone = killer->GetZoneId();
    const time_t   now  = time(nullptr);

    std::lock_guard<std::mutex> lock(g_mutex);
    Run& run = Entry(killer->GetGUID().GetRawValue(), now).kills;
    if (run.zone != zone || now - run.last > kRunBreak)
    {
        run       = Run{};
        run.zone  = zone;
        run.first = now;
    }
    run.last = now;
    ++run.count;
    auto it = std::find_if(run.kinds.begin(), run.kinds.end(),
                           [&kind](const std::pair<std::string, uint32_t>& k) { return k.first == kind; });
    if (it != run.kinds.end())
        ++it->second;
    else if (run.kinds.size() < kMaxKinds)
        run.kinds.emplace_back(std::move(kind), 1);
}

void Lately_NoteQuest(Player* player, Quest const* quest)
{
    if (!g_LatelyEnable || !player || !quest || quest->GetTitle().empty() || !OllamaIsBotPlayer(player))
        return;
    const uint32_t zone = player->GetZoneId();
    const time_t   now  = time(nullptr);
    std::lock_guard<std::mutex> lock(g_mutex);
    Lately& l   = Entry(player->GetGUID().GetRawValue(), now);
    l.quest     = quest->GetTitle();
    l.questId   = quest->GetQuestId();
    l.questZone = zone;
    l.questAt   = now;
}

void Lately_NoteItem(Player* player, ItemTemplate const* item)
{
    if (!g_LatelyEnable || !player || !item)
        return;
    // Something to show off: a green or better piece of gear, or anything
    // rare and up (a recipe, a mount, a companion). Not quest items.
    const bool gear = item->Class == ITEM_CLASS_WEAPON || item->Class == ITEM_CLASS_ARMOR;
    if (item->Class == ITEM_CLASS_QUEST || item->Quality < (gear ? ITEM_QUALITY_UNCOMMON : ITEM_QUALITY_RARE) ||
        !OllamaIsBotPlayer(player))
        return;
    const time_t now = time(nullptr);
    std::lock_guard<std::mutex> lock(g_mutex);
    Lately& l = Entry(player->GetGUID().GetRawValue(), now);
    // The best of the last while: a blue over a green, then the higher item
    // level, then the newer.
    const bool stale = now - l.itemAt > time_t(g_LatelyMinutes) * 60;
    if (stale || item->Quality > l.itemQuality || (item->Quality == l.itemQuality && item->ItemLevel >= l.itemLevel))
    {
        l.item        = item->ItemId;
        l.itemQuality = item->Quality;
        l.itemLevel   = item->ItemLevel;
        l.itemAt      = now;
    }
}

void Lately_NoteLevel(Player* player)
{
    if (!g_LatelyEnable || !player || !OllamaIsBotPlayer(player))
        return;
    const uint32_t level = player->GetLevel();
    const time_t   now   = time(nullptr);
    std::lock_guard<std::mutex> lock(g_mutex);
    Lately& l = Entry(player->GetGUID().GetRawValue(), now);
    l.level   = level;
    l.levelAt = now;
}

std::string Lately_Text(Player* bot)
{
    if (!g_LatelyEnable || !bot)
        return {};
    Lately l;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_lately.find(bot->GetGUID().GetRawValue());
        if (it == g_lately.end())
            return {};
        l = it->second;
    }
    const time_t now    = time(nullptr);
    const time_t window = time_t(g_LatelyMinutes) * 60;

    std::vector<std::string> facts;
    if (l.kills.count >= 4 && now - l.kills.last <= window)
    {
        std::vector<std::pair<std::string, uint32_t>> kinds = l.kills.kinds;
        std::stable_sort(kinds.begin(), kinds.end(),
                         [](const std::pair<std::string, uint32_t>& a, const std::pair<std::string, uint32_t>& b)
                         { return a.second > b.second; });
        std::string what;
        for (size_t i = 0; i < kinds.size() && i < 2; ++i)
            what += (i ? " and " : "") + kinds[i].first + " (" + std::to_string(kinds[i].second) + ")";
        const std::string zone = ZoneName(l.kills.zone);
        facts.push_back("out" + (zone.empty() ? std::string() : " in " + zone) + " for " +
                        Span(l.kills.last - l.kills.first) + " killing " + what + ", until " +
                        Ago(now - l.kills.last));
    }
    if (!l.quest.empty() && now - l.questAt <= window)
    {
        const std::string zone = ZoneName(l.questZone);
        facts.push_back("handed in the quest " + l.quest + (zone.empty() ? std::string() : " in " + zone) + " " +
                        Ago(now - l.questAt));
    }
    if (l.item && now - l.itemAt <= window)
        if (ItemTemplate const* t = sObjectMgr->GetItemTemplate(l.item))
            facts.push_back("got " + t->Name1 + " " + Ago(now - l.itemAt));
    if (l.level && l.level == bot->GetLevel() && now - l.levelAt <= window)
        facts.push_back("reached level " + std::to_string(l.level) + " " + Ago(now - l.levelAt));

    std::string out;
    for (const std::string& f : facts)
        out += (out.empty() ? "" : "; ") + f;
    return out;
}

LatelyNamed Lately_Named(Player* bot)
{
    LatelyNamed out;
    if (!g_LatelyEnable || !bot)
        return out;
    const time_t now    = time(nullptr);
    const time_t window = time_t(g_LatelyMinutes) * 60;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_lately.find(bot->GetGUID().GetRawValue());
    if (it == g_lately.end())
        return out;
    const Lately& l = it->second;
    if (l.questId && !l.quest.empty() && now - l.questAt <= window)
        out.quest = l.questId;
    if (l.item && now - l.itemAt <= window)
        out.item = l.item;
    return out;
}

std::string Lately_StateTextFor(Player* bot, const std::string& message)
{
    if (!g_LatelyEnable || !bot || g_LatelyStateText.empty() || g_LatelyWords.empty() || message.empty())
        return {};
    if (!CrowdText::ContainsPhrase(CrowdText::Words(message), g_LatelyWords))
        return {};
    const std::string lately = Lately_Text(bot);
    if (lately.empty())
        return {};
    const std::string out = SafeFormat(g_LatelyStateText, fmt::arg("lately", lately));
    return out == "[Format Error]" ? std::string() : out;
}
