#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_textrules.h"

#include "Bag.h"
#include "Hyperlinks.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"

#include "ChatHelper.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <unordered_map>

namespace
{
    // --- links people posted ----------------------------------------------

    struct Seen
    {
        std::string markup;
        time_t      at = 0;
    };
    std::unordered_map<std::string, Seen> g_seen;   // by TextRules::NameKey
    constexpr size_t kMaxSeen = 400;

    // Only the kinds a bot would talk about; player and channel links are
    // part of how chat itself works, not something anyone "mentions".
    bool Rememberable(const std::string& type)
    {
        return type == "item" || type == "quest" || type == "spell" || type == "enchant" ||
               type == "achievement" || type == "trade" || type == "talent";
    }

    void Remember(const std::string& key, const std::string& markup, time_t now)
    {
        if (key.empty() || markup.empty())
            return;
        g_seen[key] = Seen{ markup, now };

        if (g_seen.size() <= kMaxSeen)
            return;
        auto oldest = g_seen.begin();
        for (auto it = g_seen.begin(); it != g_seen.end(); ++it)
            if (it->second.at < oldest->second.at)
                oldest = it;
        g_seen.erase(oldest);
    }

    // --- the item and quest tables ------------------------------------------

    uint64_t Hash(const std::string& key)
    {
        uint64_t h = 1469598103934665603ull;   // FNV-1a
        for (unsigned char c : key)
        {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    bool StartsWith(const std::string& s, const char* prefix)
    {
        const size_t n = std::char_traits<char>::length(prefix);
        return s.size() >= n && s.compare(0, n, prefix) == 0;
    }

    // Placeholder and developer entries nobody would talk about. Only the
    // unmistakable ones: "Test of Strength" and "Old Crafty" are real.
    bool JunkName(const std::string& key)
    {
        return key.empty() || key[0] == '<' || key[0] == '[' || StartsWith(key, "monster -") ||
               StartsWith(key, "zz") || key.find("deprecated") != std::string::npos ||
               key.find("unused") != std::string::npos || key.find("(old)") != std::string::npos ||
               key.find("[ph]") != std::string::npos || key.find("(ph)") != std::string::npos;
    }

    struct NameIndex
    {
        bool built = false;
        std::unordered_multimap<uint64_t, uint32_t> items;
        std::unordered_multimap<uint64_t, uint32_t> quests;
    };
    NameIndex g_index;

    void BuildIndex()
    {
        if (g_index.built)
            return;
        g_index.built = true;

        if (ItemTemplateContainer const* store = sObjectMgr->GetItemTemplateStore())
        {
            g_index.items.reserve(store->size());
            for (auto const& [id, proto] : *store)
            {
                if (proto.HasFlag(ITEM_FLAG_DEPRECATED))
                    continue;
                const std::string key = TextRules::NameKey(proto.Name1);
                if (!JunkName(key))
                    g_index.items.emplace(Hash(key), id);
            }
        }

        ObjectMgr::QuestMap const& quests = sObjectMgr->GetQuestTemplates();
        g_index.quests.reserve(quests.size());
        for (auto const& [id, quest] : quests)
        {
            if (!quest)
                continue;
            const std::string key = TextRules::NameKey(quest->GetTitle());
            if (!JunkName(key))
                g_index.quests.emplace(Hash(key), id);
        }

        LOG_INFO("module.ollamachat", "[Ollama Chat] Link names indexed: {} items, {} quests.",
                 g_index.items.size(), g_index.quests.size());
    }

    uint32_t LevelGap(uint32_t a, uint32_t b)
    {
        return a > b ? a - b : b - a;
    }

    // Several items share a name: the one nearest the bot's level, then the
    // oldest entry (usually the real one).
    ItemTemplate const* BestItem(const std::string& key, Player* bot)
    {
        ItemTemplate const* best = nullptr;
        uint32_t bestGap = 0;
        auto range = g_index.items.equal_range(Hash(key));
        for (auto it = range.first; it != range.second; ++it)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(it->second);
            if (!proto || TextRules::NameKey(proto->Name1) != key)
                continue;
            const uint32_t gap = proto->RequiredLevel ? LevelGap(proto->RequiredLevel, bot->GetLevel()) : 0;
            if (!best || gap < bestGap || (gap == bestGap && proto->ItemId < best->ItemId))
            {
                best = proto;
                bestGap = gap;
            }
        }
        return best;
    }

    // Quests share titles across factions and chains: one this bot's race
    // can take, nearest its level, then the oldest.
    Quest const* BestQuest(const std::string& key, Player* bot)
    {
        Quest const* best = nullptr;
        bool bestRace = false;
        uint32_t bestGap = 0;
        auto range = g_index.quests.equal_range(Hash(key));
        for (auto it = range.first; it != range.second; ++it)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(it->second);
            if (!quest || TextRules::NameKey(quest->GetTitle()) != key)
                continue;
            const uint32_t races = quest->GetAllowableRaces();
            const bool race = races == 0 || (races & bot->getRaceMask()) != 0;
            const uint32_t level = quest->GetQuestLevel() > 0 ? uint32_t(quest->GetQuestLevel())
                                                                : quest->GetMinLevel();
            const uint32_t gap = LevelGap(level, bot->GetLevel());
            const bool better = !best || (race && !bestRace) ||
                                (race == bestRace && (gap < bestGap ||
                                                      (gap == bestGap && quest->GetQuestId() < best->GetQuestId())));
            if (better)
            {
                best = quest;
                bestRace = race;
                bestGap = gap;
            }
        }
        return best;
    }

    // --- what the bot itself has ------------------------------------------

    Quest const* QuestInLog(Player* bot, const std::string& key)
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            const uint32_t id = bot->GetQuestSlotQuestId(slot);
            if (!id)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (quest && TextRules::NameKey(quest->GetTitle()) == key)
                return quest;
        }
        return nullptr;
    }

    ItemTemplate const* ItemCarried(Player* bot, const std::string& key)
    {
        auto matches = [&key](Item* item) -> ItemTemplate const*
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            return (proto && TextRules::NameKey(proto->Name1) == key) ? proto : nullptr;
        };

        // Equipped, then the backpack.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (ItemTemplate const* proto = matches(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
                return proto;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = bot->GetBagByPos(bagSlot);
            if (!bag)
                continue;
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                if (ItemTemplate const* proto = matches(bag->GetItemByPos(static_cast<uint8>(i))))
                    return proto;
        }
        return nullptr;
    }

    // The link for one name, or empty.
    std::string Find(Player* bot, const std::string& key, time_t now)
    {
        auto seen = g_seen.find(key);
        if (seen != g_seen.end())
        {
            if (g_LinksRememberMinutes == 0 ||
                now - seen->second.at <= static_cast<time_t>(g_LinksRememberMinutes) * 60)
                return seen->second.markup;
            g_seen.erase(seen);
        }

        if (Quest const* quest = QuestInLog(bot, key))
            return ChatHelper::FormatQuest(quest);
        if (ItemTemplate const* proto = ItemCarried(bot, key))
            return ChatHelper::FormatItem(proto);

        if (!g_LinksUseGameData)
            return "";
        BuildIndex();
        if (ItemTemplate const* proto = BestItem(key, bot))
            return ChatHelper::FormatItem(proto);
        if (Quest const* quest = BestQuest(key, bot))
            return ChatHelper::FormatQuest(quest);
        return "";
    }

    // The 3.3.5 client takes at most 255 bytes from a chat line.
    constexpr size_t kMaxLineBytes = 250;

    uint64_t g_linked     = 0;
    uint64_t g_unresolved = 0;
}

void Links_NoteMessage(const std::string& message)
{
    if (!g_LinksEnable || message.find("|H") == std::string::npos)
        return;
    const time_t now = time(nullptr);
    for (const TextRules::ChatLink& link : TextRules::ExtractChatLinks(message))
        if (Rememberable(link.type) && Acore::Hyperlinks::CheckAllLinks(link.markup))
            Remember(TextRules::NameKey(link.name), link.markup, now);
}

std::string Links_PromptHint()
{
    if (!g_LinksEnable || g_LinksPromptHint.empty())
        return "";
    return " " + g_LinksPromptHint;
}

LinkedLine Links_Resolve(Player* bot, const std::string& text)
{
    LinkedLine out;
    const std::vector<TextRules::BracketName> names = TextRules::FindBracketNames(text);
    if (names.empty())
    {
        out.text = out.plain = text;
        return out;
    }

    const time_t now = time(nullptr);

    // What the line will cost the client with the links so far.
    size_t bytes = text.size();
    size_t from = 0;
    for (const TextRules::BracketName& n : names)
    {
        out.text.append(text, from, n.begin - from);
        out.plain.append(text, from, n.begin - from);
        from = n.end;

        // Only links players posted are remembered: one a bot looked up for
        // itself (its own faction's version of a quest, say) must not become
        // everyone else's.
        std::string markup;
        if (g_LinksEnable && bot && out.linked < g_LinksMaxPerLine)
        {
            markup = Find(bot, TextRules::NameKey(n.name), now);
            if (markup.empty())
                ++out.unresolved;
            else if (bytes - (n.end - n.begin) + markup.size() > kMaxLineBytes)
                markup.clear();   // too long for one chat line: leave it as text
        }

        if (markup.empty())
        {
            out.text  += n.name;
            out.plain += n.name;
            continue;
        }

        bytes = bytes - (n.end - n.begin) + markup.size();
        ++out.linked;
        out.text  += markup;
        out.plain += TextRules::StripChatMarkup(markup);   // "[Name]", as a reader sees it
    }
    out.text.append(text, from, std::string::npos);
    out.plain.append(text, from, std::string::npos);
    return out;
}

void Links_NoteSent(const LinkedLine& line)
{
    g_linked     += line.linked;
    g_unresolved += line.unresolved;
}

void Links_Update()
{
    if (g_LinksRememberMinutes == 0)
        return;
    const time_t cutoff = time(nullptr) - static_cast<time_t>(g_LinksRememberMinutes) * 60;
    for (auto it = g_seen.begin(); it != g_seen.end(); )
        it = (it->second.at < cutoff) ? g_seen.erase(it) : std::next(it);
}

LinksStats Links_GetStats()
{
    LinksStats s;
    s.linked     = g_linked;
    s.unresolved = g_unresolved;
    s.remembered = static_cast<uint32_t>(g_seen.size());
    s.indexed    = g_index.built;
    return s;
}
