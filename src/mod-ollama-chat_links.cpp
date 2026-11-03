#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_textrules.h"
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
                const std::string kind = ArmorWords(p->SubClass);
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
    std::string out = "[" + p->Name1 + "] is an item, not a person or a creature: ";
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
    if (p->Class == ITEM_CLASS_QUEST && p->StartQuest)
        if (Quest const* starts = sObjectMgr->GetQuestTemplate(p->StartQuest))
            out += ", which starts the quest [" + starts->GetTitle() + "]";
    return out;
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
    const std::string& raw = quest->GetObjectives().empty() ? quest->GetDetails() : quest->GetObjectives();
    return TextRules::QuestText(raw, name.empty() ? std::string("you") : name, cls, race, female, maxLen);
}

std::string Links_DescribeQuest(Quest const* quest, Player* reader)
{
    if (!quest)
        return {};
    std::string out = "[" + quest->GetTitle() + "] is " + QuestKind(quest);
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

        const std::string plain = TextRules::StripChatMarkup(line);
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

    std::string QuestNote(Player* bot, Quest const* quest)
    {
        switch (Links_QuestFitFor(bot, quest))
        {
            case QuestFit::TooHigh: return FitNote(g_LinksQuestTooHighText, bot);
            case QuestFit::Easy:    return FitNote(g_LinksQuestEasyText, bot);
            default:                return {};
        }
    }

    std::string ItemNote(Player* bot, ItemTemplate const* proto)
    {
        return (proto && proto->RequiredLevel > bot->GetLevel()) ? FitNote(g_LinksItemTooHighText, bot) : std::string();
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

std::string Links_Describe(Player* bot, const std::string& line)
{
    if (!g_LinksDescribeEnable || !bot || line.empty() || g_LinksDescribeText.empty() || g_LinksDescribeMax == 0)
        return {};

    std::vector<std::string> parts;
    for (const Named& n : NamedIn(bot, line, g_LinksDescribeMax))
    {
        std::string text = n.quest ? Links_DescribeQuest(n.quest, bot) + QuestNote(bot, n.quest)
                                   : Links_DescribeItem(n.item) + ItemNote(bot, n.item);
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
