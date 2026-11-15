#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_drops.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_tradechat.h"
#include "mod-ollama-chat-utilities.h"

#include "Bag.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "ItemTemplate.h"
#include "SharedDefines.h"
#include "Hyperlinks.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"

#include "ChatHelper.h"

#include <fmt/core.h>

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

    // --- what was linked lately, chat by chat -------------------------------
    // A player linked [Rune of Teleportation] asking where to get it, and
    // every answer linked it again: a wall of the same link. Once a thing has
    // been linked in a chat -- in the line being answered, or by a bot -- it is
    // named plainly there for Links.RepeatSeconds.

    std::unordered_map<std::string, time_t> g_recentLinks;   // scope '\x1f' name key -> when
    constexpr size_t kMaxRecentLinks = 2000;

    std::string RecentKey(const std::string& scope, const std::string& key)
    {
        return scope + '\x1f' + key;
    }

    bool LinkedLately(const std::string& scope, const std::string& key, time_t now)
    {
        if (scope.empty() || g_LinksRepeatSeconds == 0)
            return false;
        auto it = g_recentLinks.find(RecentKey(scope, key));
        return it != g_recentLinks.end() && now - it->second < static_cast<time_t>(g_LinksRepeatSeconds);
    }

    void NoteLinked(const std::string& scope, const std::string& key, time_t now)
    {
        if (scope.empty() || key.empty() || g_LinksRepeatSeconds == 0)
            return;
        g_recentLinks[RecentKey(scope, key)] = now;
        if (g_recentLinks.size() <= kMaxRecentLinks)
            return;
        const time_t cutoff = now - static_cast<time_t>(g_LinksRepeatSeconds);
        for (auto it = g_recentLinks.begin(); it != g_recentLinks.end(); )
            it = it->second < cutoff ? g_recentLinks.erase(it) : std::next(it);
        if (g_recentLinks.size() > kMaxRecentLinks)
            g_recentLinks.clear();
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

ItemTemplate const* Links_ItemNamed(const std::string& name, Player* bot)
{
    if (!bot)
        return nullptr;
    const std::string key = TextRules::NameKey(name);
    if (key.empty() || JunkName(key))
        return nullptr;
    BuildIndex();
    return BestItem(key, bot);
}

std::string Links_PromptHint()
{
    if (!g_LinksEnable || g_LinksPromptHint.empty())
        return "";
    return " " + g_LinksPromptHint;
}

LinkedLine Links_Resolve(Player* bot, const std::string& text, const std::string& scopeKey,
                         const std::string& replyingTo)
{
    LinkedLine out;
    const std::vector<TextRules::BracketName> names = TextRules::FindBracketNames(text);
    if (names.empty())
    {
        out.text = out.plain = text;
        return out;
    }

    const time_t now = time(nullptr);

    // What the line being answered linked is linked in this chat already.
    for (const TextRules::ChatLink& link : TextRules::ExtractChatLinks(replyingTo))
        NoteLinked(scopeKey, TextRules::NameKey(link.name), now);
    for (const TextRules::BracketName& n : TextRules::FindBracketNames(TextRules::StripChatMarkup(replyingTo)))
        NoteLinked(scopeKey, TextRules::NameKey(n.name), now);

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
        // everyone else's. One linked in this chat a moment ago is just named.
        const std::string key = TextRules::NameKey(n.name);
        std::string markup;
        if (g_LinksEnable && bot && out.linked < g_LinksMaxPerLine && !LinkedLately(scopeKey, key, now))
        {
            markup = Find(bot, key, now);
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
        NoteLinked(scopeKey, key, now);
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

// --------------------------------------------------------------------------
// What a named thing is.
// --------------------------------------------------------------------------

namespace
{
    const char* QualityWord(uint32_t q)
    {
        switch (q)
        {
            case ITEM_QUALITY_POOR:      return "grey (junk)";
            case ITEM_QUALITY_NORMAL:    return "white";
            case ITEM_QUALITY_UNCOMMON:  return "green";
            case ITEM_QUALITY_RARE:      return "blue";
            case ITEM_QUALITY_EPIC:      return "epic";
            case ITEM_QUALITY_LEGENDARY: return "legendary";
            case ITEM_QUALITY_HEIRLOOM:  return "heirloom";
            default:                     return "";
        }
    }

    const char* SlotWords(uint32_t inv)
    {
        switch (inv)
        {
            case INVTYPE_HEAD:           return "worn on the head";
            case INVTYPE_NECK:           return "a necklace";
            case INVTYPE_SHOULDERS:      return "shoulders";
            case INVTYPE_BODY:           return "a shirt";
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:           return "worn on the chest";
            case INVTYPE_WAIST:          return "a belt";
            case INVTYPE_LEGS:           return "worn on the legs";
            case INVTYPE_FEET:           return "boots, worn on the feet";
            case INVTYPE_WRISTS:         return "bracers, worn on the wrists";
            case INVTYPE_HANDS:          return "gloves";
            case INVTYPE_FINGER:         return "a ring";
            case INVTYPE_TRINKET:        return "a trinket";
            case INVTYPE_WEAPON:         return "one-handed";
            case INVTYPE_SHIELD:         return "a shield";
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:    return "a ranged weapon";
            case INVTYPE_CLOAK:          return "a cloak";
            case INVTYPE_2HWEAPON:       return "two-handed";
            case INVTYPE_BAG:            return "a bag";
            case INVTYPE_TABARD:         return "a tabard";
            case INVTYPE_WEAPONMAINHAND: return "main hand";
            case INVTYPE_WEAPONOFFHAND:  return "off hand";
            case INVTYPE_HOLDABLE:       return "held in the off hand";
            case INVTYPE_AMMO:           return "ammo";
            case INVTYPE_THROWN:         return "a throwing weapon";
            case INVTYPE_QUIVER:         return "a quiver";
            case INVTYPE_RELIC:          return "a relic";
            default:                     return "";
        }
    }

    const char* WeaponWords(uint32_t sub)
    {
        switch (sub)
        {
            case ITEM_SUBCLASS_WEAPON_AXE:
            case ITEM_SUBCLASS_WEAPON_AXE2:         return "an axe";
            case ITEM_SUBCLASS_WEAPON_BOW:          return "a bow";
            case ITEM_SUBCLASS_WEAPON_GUN:          return "a gun";
            case ITEM_SUBCLASS_WEAPON_MACE:
            case ITEM_SUBCLASS_WEAPON_MACE2:        return "a mace";
            case ITEM_SUBCLASS_WEAPON_POLEARM:      return "a polearm";
            case ITEM_SUBCLASS_WEAPON_SWORD:
            case ITEM_SUBCLASS_WEAPON_SWORD2:       return "a sword";
            case ITEM_SUBCLASS_WEAPON_STAFF:        return "a staff";
            case ITEM_SUBCLASS_WEAPON_FIST:         return "a fist weapon";
            case ITEM_SUBCLASS_WEAPON_DAGGER:       return "a dagger";
            case ITEM_SUBCLASS_WEAPON_THROWN:       return "a throwing weapon";
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:     return "a crossbow";
            case ITEM_SUBCLASS_WEAPON_WAND:         return "a wand";
            case ITEM_SUBCLASS_WEAPON_FISHING_POLE: return "a fishing pole";
            default:                                return "a weapon";
        }
    }

    const char* ArmorWords(uint32_t sub)
    {
        switch (sub)
        {
            case ITEM_SUBCLASS_ARMOR_CLOTH:   return "cloth armour";
            case ITEM_SUBCLASS_ARMOR_LEATHER: return "leather armour";
            case ITEM_SUBCLASS_ARMOR_MAIL:    return "mail armour";
            case ITEM_SUBCLASS_ARMOR_PLATE:   return "plate armour";
            case ITEM_SUBCLASS_ARMOR_SHIELD:  return "a shield";
            case ITEM_SUBCLASS_ARMOR_LIBRAM:  return "a paladin libram";
            case ITEM_SUBCLASS_ARMOR_IDOL:    return "a druid idol";
            case ITEM_SUBCLASS_ARMOR_TOTEM:   return "a shaman totem";
            case ITEM_SUBCLASS_ARMOR_SIGIL:   return "a death knight sigil";
            default:                          return "";   // rings, necks, trinkets, cloaks: the slot says it
        }
    }

    // What kind of thing it is, in a few words.
    std::string KindOf(ItemTemplate const* p)
    {
        switch (p->Class)
        {
            case ITEM_CLASS_WEAPON:
            {
                std::string out = WeaponWords(p->SubClass);
                const std::string slot = SlotWords(p->InventoryType);
                if (p->InventoryType == INVTYPE_WEAPON || p->InventoryType == INVTYPE_2HWEAPON ||
                    p->InventoryType == INVTYPE_WEAPONMAINHAND || p->InventoryType == INVTYPE_WEAPONOFFHAND)
                    out = slot + " " + out.substr(out.find(' ') + 1);   // "a sword" -> "one-handed sword"
                return out;
            }
            case ITEM_CLASS_ARMOR:
            {
                if (p->InventoryType == INVTYPE_CLOAK)
                    return "a cloak";   // cloth by subclass, but nobody calls it cloth armour
                std::string kind = ArmorWords(p->SubClass);
                const std::string slot = SlotWords(p->InventoryType);
                if (kind.empty())
                    return slot.empty() ? std::string("a piece of gear") : slot;
                if (slot.empty() || p->SubClass >= ITEM_SUBCLASS_ARMOR_SHIELD)
                    return kind;
                return kind + ", " + slot;
            }
            case ITEM_CLASS_CONSUMABLE:
                switch (p->SubClass)
                {
                    case 1:  return "a potion";
                    case 2:  return "an elixir";
                    case 3:  return "a flask";
                    case 4:  return "a scroll";
                    case 5:  return "food or drink";
                    case 6:  return "something to enchant or improve gear with";
                    case 7:  return "a bandage";
                    default: return "something you use up";
                }
            case ITEM_CLASS_CONTAINER:  return p->ContainerSlots ? fmt::format("a {}-slot bag", p->ContainerSlots)
                                                                 : std::string("a bag");
            case ITEM_CLASS_GEM:        return "a gem for socketing gear";
            case ITEM_CLASS_REAGENT:    return "a reagent for spells";
            case ITEM_CLASS_PROJECTILE: return "ammo";
            case ITEM_CLASS_TRADE_GOODS:
                switch (p->SubClass)
                {
                    case 1:  return "an engineering part";
                    case 2:  return "an explosive";
                    case 3:  return "an engineering device";
                    case 4:  return "a jewelcrafting material";
                    case 5:  return "cloth";
                    case 6:  return "leather";
                    case 7:  return "ore, metal or stone";
                    case 8:  return "meat for cooking";
                    case 9:  return "a herb";
                    case 10: return "an elemental material";
                    case 12: return "an enchanting material";
                    default: return "a crafting material";
                }
            case ITEM_CLASS_RECIPE:     return "a recipe that teaches a profession something";
            case ITEM_CLASS_QUIVER:     return "a quiver or ammo pouch";
            case ITEM_CLASS_QUEST:      return "a quest item";
            case ITEM_CLASS_KEY:        return "a key";
            case ITEM_CLASS_GLYPH:      return "a glyph";
            case ITEM_CLASS_MISC:
                switch (p->SubClass)
                {
                    case 0:  return p->Quality == ITEM_QUALITY_POOR ? "junk to sell" : "a miscellaneous item";
                    case 2:  return "a companion pet";
                    case 3:  return "a holiday item";
                    case 5:  return "a mount";
                    default: return "a miscellaneous item";
                }
            default:                    return "an item";
        }
    }

    const char* QuestKind(Quest const* q)
    {
        switch (q->GetType())
        {
            case QUEST_TYPE_ELITE:   return "an elite quest";
            case QUEST_TYPE_DUNGEON: return "a dungeon quest";
            case QUEST_TYPE_HEROIC:  return "a heroic dungeon quest";
            case QUEST_TYPE_RAID:
            case QUEST_TYPE_RAID_10:
            case QUEST_TYPE_RAID_25: return "a raid quest";
            case QUEST_TYPE_PVP:     return "a PvP quest";
            case QUEST_TYPE_ESCORT:  return "an escort quest";
            default:                 return q->GetSuggestedPlayers() > 1 ? "a group quest" : "a quest";
        }
    }

    std::string AreaOf(int32_t zoneOrSort)
    {
        if (zoneOrSort <= 0)
            return {};
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(uint32(zoneOrSort));
        return (area && area->area_name[0] && *area->area_name[0]) ? std::string(area->area_name[0]) : std::string();
    }
}

std::string Links_DescribeItem(ItemTemplate const* p)
{
    if (!p)
        return {};
    std::string out = p->Name1 + " is an item, not a person or a creature: ";
    const char* quality = QualityWord(p->Quality);
    const std::string kind = KindOf(p);
    const bool gear = p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR;
    out += (gear && *quality) ? std::string(quality) + " " + kind : kind;
    if (gear && p->ItemLevel)
        out += fmt::format(", item level {}", p->ItemLevel);
    if (p->RequiredLevel > 1)
        out += fmt::format(", for level {} and up", p->RequiredLevel);
    if (gear && p->Bonding == BIND_WHEN_EQUIPPED)
        out += ", binds when equipped";
    else if (gear && p->Bonding == BIND_WHEN_PICKED_UP)
        out += ", binds when picked up";
    // A rare find, as the loot tables have it (mod-ollama-chat_drops).
    if (gear && p->Quality >= ITEM_QUALITY_RARE)
    {
        const double pct = Drops_Pct(p->ItemId);
        if (pct > 0.0 && pct < g_TradeRareDropPct)
            out += ", a rare drop: about once in " + Drops_OnceIn(pct) + " kills of anything that can drop it";
    }
    if (p->Class == ITEM_CLASS_QUEST && p->StartQuest)
        if (Quest const* starts = sObjectMgr->GetQuestTemplate(p->StartQuest))
            out += ", which starts the quest '" + starts->GetTitle() + "'";
    return out;
}

namespace
{
    // Who each quest is handed in to: the first NPC (else object) the game
    // has it end at. Built once, on first use.
    std::string EnderName(uint32 questId)
    {
        static std::unordered_map<uint32, std::string> enders;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (auto const& [entry, quest] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
                if (!enders.contains(quest))
                    if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry))
                        enders[quest] = t->Name;
            for (auto const& [entry, quest] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
                if (!enders.contains(quest))
                    if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(entry))
                        enders[quest] = t->name;
        }
        auto it = enders.find(questId);
        return it == enders.end() ? std::string() : it->second;
    }

    // A creature a quest counts that nobody sees -- a trigger, a credit
    // marker: its name is no goal to tell anyone.
    bool Hidden(CreatureTemplate const* t)
    {
        if (!t || (t->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER))
            return true;
        std::string name = t->Name;
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* word : { "credit", "bunny", "trigger", "[dnd]", "[ph]", "dnd ", "(dnd)", "invisible" })
            if (name.find(word) != std::string::npos)
                return true;
        return false;
    }
}

std::string Links_QuestObjective(Quest const* quest, Player* reader, size_t maxLen)
{
    if (!quest)
        return {};
    std::string name, cls, race;
    bool female = false;
    if (reader)
    {
        name   = reader->GetName();
        female = reader->getGender() == GENDER_FEMALE;
        if (ChrClassesEntry const* c = sChrClassesStore.LookupEntry(reader->getClass()))
            cls = c->name[0];
        if (ChrRacesEntry const* r = sChrRacesStore.LookupEntry(reader->getRace()))
            race = r->name[0];
    }
    const std::string who = name.empty() ? std::string("you") : name;

    // Its goals as the game lists them -- what to kill, collect or use, each
    // on its own -- and who it is handed in to: "Kill 15 Blackrock Champions
    // and Guard Howe in Lakeshire will reward you" had a bot tell someone to
    // kill Guard Howe. The quest log's own text only for a quest whose goals
    // the game does not list (talk to someone, go somewhere), or one that
    // counts something nobody sees.
    const std::string& raw = quest->GetObjectives().empty() ? quest->GetDetails() : quest->GetObjectives();
    // A creature counted is one to kill when the quest says so; one to use
    // something on, free or talk to is better told in the quest's own words.
    bool killing = false;
    {
        const std::vector<std::string> words = CrowdText::Words(raw);
        for (const std::string& w : words)
            for (const char* k : { "kill", "kills", "slay", "slain", "destroy", "defeat", "hunt", "cull", "eliminate",
                                   "dispatch", "exterminate", "execute", "assassinate", "vanquish", "murder" })
                killing = killing || w == k;
    }
    std::vector<TextRules::QuestGoal> goals;
    bool unlisted = false;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT && !unlisted; ++i)
    {
        const int32 id = quest->RequiredNpcOrGo[i];
        if (!id || !quest->RequiredNpcOrGoCount[i])
            continue;
        TextRules::QuestGoal g;
        g.count = quest->RequiredNpcOrGoCount[i];
        if (!quest->ObjectiveText[i].empty())
            g.what = TextRules::QuestText(quest->ObjectiveText[i], who, cls, race, female, 0);
        else if (id > 0)
        {
            CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(uint32(id));
            if (Hidden(t) || !killing)
                unlisted = true;
            else
            {
                g.verb = "kill";
                g.what = t->Name;
            }
        }
        else if (GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(uint32(-id)))
        {
            g.verb = "use";
            g.what = go->name;
        }
        else
            unlisted = true;
        goals.push_back(std::move(g));
    }
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT && !unlisted; ++i)
    {
        if (!quest->RequiredItemId[i] || !quest->RequiredItemCount[i])
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]);
        if (!proto)
        {
            unlisted = true;
            break;
        }
        TextRules::QuestGoal g;
        g.verb  = "collect";
        g.what  = proto->Name1;
        g.count = quest->RequiredItemCount[i];
        goals.push_back(std::move(g));
    }
    if (!unlisted && quest->GetPlayersSlain())
        goals.push_back(TextRules::QuestGoal{ "kill", "enemy player", quest->GetPlayersSlain() });
    if (unlisted)
        goals.clear();

    std::string out = TextRules::QuestPlainly(goals, EnderName(quest->GetQuestId()),
                                              goals.empty() ? TextRules::QuestText(raw, who, cls, race, female, maxLen)
                                                            : std::string());
    if (maxLen && out.size() > maxLen)
    {
        size_t cut = out.rfind(' ', maxLen);
        out.resize(cut == std::string::npos || cut < maxLen / 2 ? maxLen : cut);
        out += "...";
    }
    return out;
}

std::string Links_DescribeQuest(Quest const* quest, Player* reader)
{
    if (!quest)
        return {};
    std::string out = "'" + quest->GetTitle() + "' is " + QuestKind(quest);
    const std::string where = AreaOf(quest->GetZoneOrSort());
    if (!where.empty())
        out += " in " + where;
    if (quest->GetQuestLevel() > 0)
        out += fmt::format(" for level {}", quest->GetQuestLevel());
    const std::string what = Links_QuestObjective(quest, reader, g_LinksDescribeQuestChars);
    if (!what.empty())
        out += ", where you have to: " + what;
    return out;
}

namespace
{
    // What a line names, item or quest, as `bot` would take it: links by
    // their id, then names in square brackets -- what the bot itself has,
    // what posted links said, then the tables. At most `max`, each once.
    struct Named
    {
        std::string         key;
        ItemTemplate const* item  = nullptr;
        Quest const*        quest = nullptr;
    };

    // Questie's plain-text reference (TextRules::FindQuestRefs): its quest,
    // when there is one by that id with that title -- "[Hogger (176)]" is not
    // quest 176 unless quest 176 is called Hogger.
    Quest const* QuestOfRef(const TextRules::QuestRef& ref)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(ref.id);
        return quest && TextRules::NameKey(quest->GetTitle()) == TextRules::NameKey(ref.name) ? quest : nullptr;
    }

    std::vector<Named> NamedIn(Player* bot, const std::string& line, size_t max)
    {
        std::vector<Named> out;
        auto add = [&](const std::string& key, ItemTemplate const* item, Quest const* quest)
        {
            if ((!item && !quest) || out.size() >= max)
                return;
            for (const Named& n : out)
                if (n.key == key)
                    return;
            out.push_back(Named{ key, item, quest });
        };

        for (const TextRules::ChatLink& link : TextRules::ExtractChatLinks(line))
        {
            const std::string key = TextRules::NameKey(link.name);
            if (link.type == "item" && link.id)
                add(key, sObjectMgr->GetItemTemplate(link.id), nullptr);
            else if (link.type == "quest" && link.id)
                add(key, nullptr, sObjectMgr->GetQuestTemplate(link.id));
        }

        // Questie's "[[30+] Mor'Ladim (228)]": a quest by its id, though it
        // reached the server as text.
        for (const TextRules::QuestRef& ref : TextRules::FindQuestRefs(TextRules::StripChatMarkup(line)))
            if (Quest const* quest = QuestOfRef(ref))
                add(TextRules::NameKey(quest->GetTitle()), nullptr, quest);

        // Names in brackets, as the line reads: Questie's references by their
        // titles, so the one just added is not looked for again.
        const std::string plain = Links_AsRead(line);
        const time_t now = time(nullptr);
        for (const TextRules::BracketName& n : TextRules::FindBracketNames(plain))
        {
            if (out.size() >= max)
                break;
            const std::string key = TextRules::NameKey(n.name);
            if (key.empty() || std::any_of(out.begin(), out.end(), [&](const Named& d) { return d.key == key; }))
                continue;
            if (Quest const* quest = QuestInLog(bot, key))
            {
                add(key, nullptr, quest);
                continue;
            }
            if (ItemTemplate const* proto = ItemCarried(bot, key))
            {
                add(key, proto, nullptr);
                continue;
            }
            auto seen = g_seen.find(key);
            if (seen != g_seen.end() &&
                (g_LinksRememberMinutes == 0 || now - seen->second.at <= time_t(g_LinksRememberMinutes) * 60))
            {
                const std::vector<TextRules::ChatLink> links = TextRules::ExtractChatLinks(seen->second.markup);
                if (!links.empty() && links[0].id)
                {
                    if (links[0].type == "item")
                        add(key, sObjectMgr->GetItemTemplate(links[0].id), nullptr);
                    else if (links[0].type == "quest")
                        add(key, nullptr, sObjectMgr->GetQuestTemplate(links[0].id));
                    continue;
                }
            }
            if (!g_LinksUseGameData)
                continue;
            BuildIndex();
            if (ItemTemplate const* proto = BestItem(key, bot))
                add(key, proto, nullptr);
            else if (Quest const* quest = BestQuest(key, bot))
                add(key, nullptr, quest);
        }
        return out;
    }

    // " (you are level 12, far too low for it: ...)", or nothing.
    std::string FitNote(const std::string& templ, Player* bot)
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ, fmt::arg("bot_level", bot->GetLevel()));
        return (out.empty() || out == "[Format Error]") ? std::string() : " " + out;
    }

    // Who a quest is handed in to: " to Sven", or nothing.
    std::string EnderOf(uint32 questId)
    {
        const std::string name = EnderName(questId);
        return name.empty() ? std::string() : " to " + name;
    }

    // ": Skeletal Raider 7/15, Skeletal Healer 0/3", or nothing.
    std::string ProgressOf(Player* bot, Quest const* quest)
    {
        auto const& log = bot->getQuestStatusMap();
        auto it = log.find(quest->GetQuestId());
        if (it == log.end())
            return {};
        const QuestStatusData& status = it->second;
        std::vector<std::string> parts;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            const int32 id = quest->RequiredNpcOrGo[i];
            if (!id || !quest->RequiredNpcOrGoCount[i])
                continue;
            std::string what;
            if (id > 0)
            {
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(uint32(id)))
                    what = t->Name;
            }
            else if (GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(uint32(-id)))
                what = go->name;
            if (!what.empty())
                parts.push_back(fmt::format("{} {}/{}", what,
                                            std::min<uint32>(status.CreatureOrGOCount[i], quest->RequiredNpcOrGoCount[i]),
                                            quest->RequiredNpcOrGoCount[i]));
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (!quest->RequiredItemId[i] || !quest->RequiredItemCount[i])
                continue;
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]))
                parts.push_back(fmt::format("{} {}/{}", proto->Name1,
                                            std::min<uint32>(status.ItemCount[i], quest->RequiredItemCount[i]),
                                            quest->RequiredItemCount[i]));
        }
        std::string out;
        for (size_t i = 0; i < parts.size() && i < 4; ++i)
            out += (i ? ", " : ": ") + parts[i];
        return out;
    }

    // Whether the bot has the quest, and how far along: "do you have [Proving
    // Your Worth]?" asks about its quest log, not for something to hand over.
    std::string HaveNote(Player* bot, Quest const* quest)
    {
        std::string out;
        switch (bot->GetQuestStatus(quest->GetQuestId()))
        {
            case QUEST_STATUS_COMPLETE:
                out = SafeFormat(g_LinksQuestDoneText, fmt::arg("ender", EnderOf(quest->GetQuestId())));
                break;
            case QUEST_STATUS_INCOMPLETE:
            case QUEST_STATUS_FAILED:
                out = SafeFormat(g_LinksQuestOnText, fmt::arg("progress", ProgressOf(bot, quest)));
                break;
            case QUEST_STATUS_REWARDED:
                out = g_LinksQuestRewardedText;
                break;
            default:
                out = g_LinksQuestNoneText;
                break;
        }
        return (out.empty() || out == "[Format Error]") ? std::string() : " " + out;
    }

    std::string QuestNote(Player* bot, Quest const* quest)
    {
        switch (Links_QuestFitFor(bot, quest))
        {
            case QuestFit::TooHigh: return FitNote(g_LinksQuestTooHighText, bot);
            case QuestFit::Easy:    return FitNote(g_LinksQuestEasyText, bot);
            default:                return {};
        }
    }

    // The quests that need an item (its RequiredItemId), by item: built once,
    // on first use.
    std::vector<uint32_t> const& QuestsNeeding(uint32_t entry)
    {
        static std::unordered_map<uint32_t, std::vector<uint32_t>> index;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
                if (quest)
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                        if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
                            index[quest->RequiredItemId[i]].push_back(id);
        }
        static const std::vector<uint32_t> none;
        auto it = index.find(entry);
        return it == index.end() ? none : it->second;
    }

    // The quest in `bot`'s log that needs `entry`, nullptr when none does.
    Quest const* OwnQuestNeeding(Player* bot, uint32_t entry)
    {
        for (uint32_t id : QuestsNeeding(entry))
        {
            const QuestStatus st = bot->GetQuestStatus(id);
            if (st == QUEST_STATUS_INCOMPLETE || st == QUEST_STATUS_COMPLETE || st == QUEST_STATUS_FAILED)
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(id))
                    return quest;
        }
        return nullptr;
    }

    // "not done yet: Mor'Ladim's Skull 0/1", or "done, it only needs handing
    // in to Sven Yorgen".
    std::string StateOf(Player* bot, Quest const* quest)
    {
        if (bot->GetQuestStatus(quest->GetQuestId()) == QUEST_STATUS_COMPLETE)
            return "done, it only needs handing in" + EnderOf(quest->GetQuestId());
        return "not done yet" + ProgressOf(bot, quest);
    }

    // Where `speaker` -- in the bot's group -- is with a quest of theirs they
    // named (Links.QuestTheirsText): "Mor'Ladim's Skull: 1/1 for
    // [Mor'Ladim]!" is a quest done, not one still ahead. Empty when it is
    // not in their log.
    std::string TheirsNote(Player* speaker, Quest const* quest)
    {
        if (!speaker || !quest || g_LinksQuestTheirsText.empty())
            return {};
        const QuestStatus st = speaker->GetQuestStatus(quest->GetQuestId());
        if (st != QUEST_STATUS_INCOMPLETE && st != QUEST_STATUS_COMPLETE && st != QUEST_STATUS_FAILED)
            return {};
        const std::string note = SafeFormat(g_LinksQuestTheirsText, fmt::arg("player_name", speaker->GetName()),
                                            fmt::arg("progress", StateOf(speaker, quest)));
        return (note.empty() || note == "[Format Error]") ? std::string() : " " + note;
    }

    // Is the line about money: a price, or talk of buying, selling or what
    // things are worth (Links.WorthWords)?
    bool MoneyTalk(const std::string& line)
    {
        const std::string plain = TextRules::WithoutBracketNames(TextRules::StripChatMarkup(line));
        return TextRules::ParsePrice(plain) != 0 || CrowdText::ContainsPhrase(CrowdText::Words(plain), g_LinksWorthWords);
    }

    // What `bot` should know of an item named to it: too high a level for it
    // yet; whose quest it is for -- "need any more to help?" with a quest's
    // item linked is about the quest, not what it has in its bags; and, with
    // `money` talk, what one goes for (Trade chat's own sums), so "it sells
    // for way more than that" is not said of something nobody checked.
    std::string ItemNote(Player* bot, ItemTemplate const* proto, bool money)
    {
        if (!proto)
            return {};
        std::string out = proto->RequiredLevel > bot->GetLevel() ? FitNote(g_LinksItemTooHighText, bot) : std::string();
        if (Quest const* own = OwnQuestNeeding(bot, proto->ItemId))
        {
            const std::string note = SafeFormat(g_LinksQuestItemText, fmt::arg("quest", own->GetTitle()),
                                                fmt::arg("progress", StateOf(bot, own)));
            if (!note.empty() && note != "[Format Error]")
                out += " " + note;
        }
        else if (std::vector<uint32_t> const& quests = QuestsNeeding(proto->ItemId); !quests.empty())
        {
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(quests.front()))
            {
                const std::string note = SafeFormat(g_LinksQuestItemOtherText, fmt::arg("quest", quest->GetTitle()));
                if (!note.empty() && note != "[Format Error]")
                    out += " " + note;
            }
        }
        std::string worth, vendor;
        if (money && !g_LinksWorthText.empty() && TradeChat_WorthOfOne(proto, uint32_t(bot->GetTeamId()), worth, vendor))
        {
            const std::string note = SafeFormat(g_LinksWorthText, fmt::arg("worth", worth), fmt::arg("vendor", vendor));
            if (!note.empty() && note != "[Format Error]")
                out += " " + note;
        }
        return out;
    }
}

QuestFit Links_QuestFitFor(Player* bot, Quest const* quest)
{
    if (!bot || !quest)
        return QuestFit::Fits;
    const uint32_t level = bot->GetLevel();
    // A quest level of -1 goes with whoever does it.
    const uint32_t questLevel = quest->GetQuestLevel() > 0 ? uint32_t(quest->GetQuestLevel()) : level;
    if (level < quest->GetMinLevel() || questLevel > level + g_LinksQuestLevelsAbove)
        return QuestFit::TooHigh;
    if (questLevel <= Acore::XP::GetGrayLevel(uint8_t(std::min<uint32_t>(level, 255))))
        return QuestFit::Easy;
    return QuestFit::Fits;
}

std::vector<Quest const*> Links_QuestsNamed(Player* speaker, const std::string& line)
{
    std::vector<Quest const*> out;
    if (!speaker || line.find('[') == std::string::npos)
        return out;
    for (const Named& n : NamedIn(speaker, line, 4))
        if (n.quest)
            out.push_back(n.quest);
    return out;
}

bool Links_AllTooHighFor(Player* bot, const std::vector<Quest const*>& quests)
{
    if (!bot || quests.empty())
        return false;
    for (Quest const* quest : quests)
        if (Links_QuestFitFor(bot, quest) != QuestFit::TooHigh)
            return false;
    return true;
}

std::string Links_Describe(Player* bot, const std::string& line, Player* speaker)
{
    if (!g_LinksDescribeEnable || !bot || line.empty() || g_LinksDescribeText.empty() || g_LinksDescribeMax == 0)
        return {};

    // In a group with whoever named it, the bot is with them for it: "too
    // low to help" is not for it -- Kihdon, 24, said "too high level for me to
    // help" to the line saying Mor'Ladim was dead, after helping kill him --
    // and where they are with their own quest is in front of it.
    const bool together = speaker && speaker != bot && bot->GetGroup() && bot->GetGroup() == speaker->GetGroup();
    std::vector<std::string> parts;
    const bool money = MoneyTalk(line);
    for (const Named& n : NamedIn(bot, line, g_LinksDescribeMax))
    {
        std::string text;
        if (n.quest)
        {
            text = Links_DescribeQuest(n.quest, bot);
            if (!together || Links_QuestFitFor(bot, n.quest) != QuestFit::TooHigh)
                text += QuestNote(bot, n.quest);
            if (together)
                text += TheirsNote(speaker, n.quest);
            text += HaveNote(bot, n.quest);
        }
        else
            text = Links_DescribeItem(n.item) + ItemNote(bot, n.item, money);
        if (!text.empty())
            parts.push_back(std::move(text));
    }
    if (parts.empty())
        return {};

    std::string things;
    for (size_t i = 0; i < parts.size(); ++i)
        things += (i ? "; " : "") + parts[i];
    const std::string out = SafeFormat(g_LinksDescribeText, fmt::arg("things", things));
    return out == "[Format Error]" ? std::string() : out;
}

std::string Links_AsRead(const std::string& line)
{
    // A quest link by its own title: quest addons write their level tags into
    // the name ("[[30+] Mor'Ladim]"), and the tag is no part of the quest.
    std::string out = line;
    if (line.find("|H") != std::string::npos)
        for (const TextRules::ChatLink& link : TextRules::ExtractChatLinks(line))
        {
            if (link.type != "quest" || !link.id || link.markup.empty())
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(link.id);
            const size_t at = quest ? out.find(link.markup) : std::string::npos;
            if (at != std::string::npos)
                out.replace(at, link.markup.size(), "[" + quest->GetTitle() + "]");
        }
    out = TextRules::StripChatMarkup(out);
    // ...and Questie's own references ("[[32] Morbent Fel (55)]", what its
    // tracker puts in chat) the same way: the id is no part of what is said.
    if (out.find(")]") == std::string::npos)
        return out;
    const std::vector<TextRules::QuestRef> refs = TextRules::FindQuestRefs(out);
    for (auto ref = refs.rbegin(); ref != refs.rend(); ++ref)
        if (Quest const* quest = QuestOfRef(*ref))
            out.replace(ref->begin, ref->end - ref->begin, "[" + quest->GetTitle() + "]");
    return out;
}

namespace
{
    // A real player's call for a group, and what they say after it in the
    // same place: "LFG [Morbent Fel]", then "I need level 32+ please", then
    // "the quest is level 32+". The follow-ups name no quest, and were
    // answered as if they were about nothing in particular: "is that a group
    // for me?", "you're just being picky because you're lazy".
    struct LfgCall
    {
        std::vector<uint32_t> quests;
        std::string           place;
        uint32_t              minLevel = 0;   // the level they asked for: "32+"
        time_t                until    = 0;
    };
    std::unordered_map<uint64_t, LfgCall> g_lfgCalls;   // by the asker

    LfgCall const* CallOf(Player* asker)
    {
        if (!asker)
            return nullptr;
        auto it = g_lfgCalls.find(asker->GetGUID().GetRawValue());
        return it == g_lfgCalls.end() || it->second.until < time(nullptr) ? nullptr : &it->second;
    }
}

std::string Links_LfgPlace(uint32_t source, const std::string& channelName)
{
    return std::to_string(source) + ":" + channelName;
}

std::vector<Quest const*> Links_LfgQuestsFor(Player* speaker, const std::string& line, const std::string& place)
{
    std::vector<Quest const*> quests = Links_LfgQuests(speaker, line);
    if (!speaker || !g_LinksLfgEnable || g_LinksLfgMinutes == 0)
        return quests;
    const time_t now = time(nullptr);
    const uint32_t level = TextRules::LevelAsked(line);
    const uint64_t key = speaker->GetGUID().GetRawValue();
    if (!quests.empty())
    {
        std::vector<uint32_t> ids;
        for (Quest const* quest : quests)
            ids.push_back(quest->GetQuestId());
        LfgCall& call = g_lfgCalls[key];
        if (call.quests != ids || call.place != place || call.until < now)
            call.minLevel = 0;   // a new call: no level asked for yet
        call.quests = std::move(ids);
        call.place  = place;
        if (level)
            call.minLevel = level;
        call.until = now + time_t(g_LinksLfgMinutes) * 60;
        return quests;
    }
    // A follow-up: in the same place, before the call has gone cold, and
    // naming nothing else -- a line about another quest or an item is about
    // that.
    auto it = g_lfgCalls.find(key);
    if (it == g_lfgCalls.end() || it->second.until < now || it->second.place != place ||
        line.find('[') != std::string::npos)
        return quests;
    LfgCall& call = it->second;
    if (level)
        call.minLevel = level;
    call.until = now + time_t(g_LinksLfgMinutes) * 60;   // still at it
    for (uint32_t id : call.quests)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(id))
            quests.push_back(quest);
    return quests;
}

std::vector<Quest const*> Links_LfgQuests(Player* speaker, const std::string& line)
{
    std::vector<Quest const*> out;
    if (!g_LinksLfgEnable || !speaker || g_LinksLfgWords.empty() || line.find('[') == std::string::npos)
        return out;
    // The asking is in the words around what is linked: "anyone on [...]?".
    const std::string around = TextRules::WithoutBracketNames(TextRules::StripChatMarkup(line));
    if (!CrowdText::ContainsPhrase(CrowdText::Words(around), g_LinksLfgWords))
        return out;
    auto add = [&](Quest const* quest)
    {
        if (quest && std::find(out.begin(), out.end(), quest) == out.end())
            out.push_back(quest);
    };
    for (const Named& n : NamedIn(speaker, line, 4))
    {
        if (n.quest)
            add(n.quest);
        else if (n.item)
        {
            // A quest's item: that quest -- theirs when they have it.
            Quest const* theirs = OwnQuestNeeding(speaker, n.item->ItemId);
            std::vector<uint32_t> const& needing = QuestsNeeding(n.item->ItemId);
            add(theirs ? theirs : needing.empty() ? nullptr : sObjectMgr->GetQuestTemplate(needing.front()));
        }
    }
    return out;
}

bool Links_CanJoinFor(Player* bot, Player* asker, const std::vector<Quest const*>& quests, Quest const** which)
{
    if (which)
        *which = nullptr;
    if (!bot || !asker || quests.empty() || bot->GetTeamId() != asker->GetTeamId())
        return false;
    // In a group already (unless it is theirs): not one to join them.
    if (Group* group = bot->GetGroup(); group && group != asker->GetGroup())
        return false;
    // Too far above them, and not of their guild: playerbots turns their
    // invite down (Playerbots.GroupInvitationPermission 1).
    if (g_LinksLfgLevels && bot->GetLevel() > asker->GetLevel() + g_LinksLfgLevels &&
        (!bot->GetGuildId() || bot->GetGuildId() != asker->GetGuildId()))
        return false;
    // Below the level they asked for ("I need level 32+ please").
    if (LfgCall const* call = CallOf(asker); call && call->minLevel && bot->GetLevel() < call->minLevel)
        return false;
    for (Quest const* quest : quests)
    {
        const QuestStatus st = bot->GetQuestStatus(quest->GetQuestId());
        if (st == QUEST_STATUS_INCOMPLETE || st == QUEST_STATUS_FAILED)
        {
            if (which)
                *which = quest;
            return true;
        }
    }
    return false;
}

std::string Links_LfgNoteFor(Player* bot, Player* asker, const std::string& line, const std::string& place)
{
    if (!g_LinksLfgEnable || g_LinksLfgText.empty())
        return {};
    Quest const* quest = nullptr;
    if (!Links_CanJoinFor(bot, asker, Links_LfgQuestsFor(asker, line, place), &quest) || !quest)
        return {};
    const std::string out = SafeFormat(g_LinksLfgText, fmt::arg("player_name", asker->GetName()),
                                       fmt::arg("quest", quest->GetTitle()),
                                       fmt::arg("progress", StateOf(bot, quest)));
    return out == "[Format Error]" ? std::string() : out;
}

void Links_Update()
{
    const time_t now = time(nullptr);
    if (g_LinksRememberMinutes != 0)
    {
        const time_t cutoff = now - static_cast<time_t>(g_LinksRememberMinutes) * 60;
        for (auto it = g_seen.begin(); it != g_seen.end(); )
            it = (it->second.at < cutoff) ? g_seen.erase(it) : std::next(it);
    }
    const time_t recent = now - static_cast<time_t>(g_LinksRepeatSeconds);
    for (auto it = g_recentLinks.begin(); it != g_recentLinks.end(); )
        it = (it->second < recent) ? g_recentLinks.erase(it) : std::next(it);
    for (auto it = g_lfgCalls.begin(); it != g_lfgCalls.end(); )
        it = (it->second.until < now) ? g_lfgCalls.erase(it) : std::next(it);
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
