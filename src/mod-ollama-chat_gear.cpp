#include "mod-ollama-chat_gear.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "Random.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    std::unordered_map<uint64_t, Clock::time_point> g_backOff;   // real player -> when they felt picked on
    GearStats g_stats;

    const char* Quality(uint32_t q)
    {
        switch (q)
        {
            case ITEM_QUALITY_POOR:      return "grey";
            case ITEM_QUALITY_NORMAL:    return "white";
            case ITEM_QUALITY_UNCOMMON:  return "green";
            case ITEM_QUALITY_RARE:      return "blue";
            case ITEM_QUALITY_EPIC:      return "purple";
            case ITEM_QUALITY_LEGENDARY: return "orange";
            case ITEM_QUALITY_HEIRLOOM:  return "heirloom";
            default:                     return "";
        }
    }

    const char* Place(uint8_t slot)
    {
        switch (slot)
        {
            case EQUIPMENT_SLOT_HEAD:      return "helm";
            case EQUIPMENT_SLOT_SHOULDERS: return "shoulders";
            case EQUIPMENT_SLOT_CHEST:     return "chest piece";
            case EQUIPMENT_SLOT_LEGS:      return "trousers";
            case EQUIPMENT_SLOT_HANDS:     return "gloves";
            case EQUIPMENT_SLOT_FEET:      return "boots";
            default:                       return "";
        }
    }

    std::string WeaponKind(ItemTemplate const* p)
    {
        const bool twoHanded = p->InventoryType == INVTYPE_2HWEAPON;
        switch (p->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_AXE:
            case ITEM_SUBCLASS_WEAPON_AXE2:     return twoHanded ? "two-handed axe" : "axe";
            case ITEM_SUBCLASS_WEAPON_MACE:
            case ITEM_SUBCLASS_WEAPON_MACE2:    return twoHanded ? "two-handed mace" : "mace";
            case ITEM_SUBCLASS_WEAPON_SWORD:
            case ITEM_SUBCLASS_WEAPON_SWORD2:   return twoHanded ? "two-handed sword" : "sword";
            case ITEM_SUBCLASS_WEAPON_BOW:      return "bow";
            case ITEM_SUBCLASS_WEAPON_GUN:      return "gun";
            case ITEM_SUBCLASS_WEAPON_CROSSBOW: return "crossbow";
            case ITEM_SUBCLASS_WEAPON_POLEARM:  return "polearm";
            case ITEM_SUBCLASS_WEAPON_STAFF:    return "staff";
            case ITEM_SUBCLASS_WEAPON_FIST:     return "fist weapon";
            case ITEM_SUBCLASS_WEAPON_DAGGER:   return "dagger";
            case ITEM_SUBCLASS_WEAPON_THROWN:   return "throwing weapon";
            case ITEM_SUBCLASS_WEAPON_WAND:     return "wand";
            case ITEM_SUBCLASS_WEAPON_FISHING_POLE: return "fishing pole";
            default:                            return "weapon";
        }
    }

    std::string ArmorKind(ItemTemplate const* p, uint8_t slot)
    {
        switch (p->SubClass)
        {
            case ITEM_SUBCLASS_ARMOR_SHIELD: return "shield";
            case ITEM_SUBCLASS_ARMOR_LIBRAM: return "libram";
            case ITEM_SUBCLASS_ARMOR_IDOL:   return "idol";
            case ITEM_SUBCLASS_ARMOR_TOTEM:  return "totem";
            case ITEM_SUBCLASS_ARMOR_SIGIL:  return "sigil";
            default:                         break;
        }
        if (p->InventoryType == INVTYPE_HOLDABLE)
            return "off-hand item";
        const char* place = Place(slot);
        const char* material = p->SubClass == ITEM_SUBCLASS_ARMOR_CLOTH   ? "cloth"
                             : p->SubClass == ITEM_SUBCLASS_ARMOR_LEATHER ? "leather"
                             : p->SubClass == ITEM_SUBCLASS_ARMOR_MAIL    ? "mail"
                             : p->SubClass == ITEM_SUBCLASS_ARMOR_PLATE   ? "plate"
                                                                          : "";
        if (p->InventoryType == INVTYPE_ROBE)
            return *material ? std::string(material) + " robe" : std::string("robe");
        if (!*place)
            return "piece of gear";
        return *material ? std::string(material) + " " + place : std::string(place);
    }

    std::string Join(const std::vector<std::string>& parts)
    {
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i)
            out += (i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ")) + parts[i];
        return out;
    }

    bool Talks(const std::vector<std::string>& words)
    {
        return !words.empty() && CrowdText::ContainsPhrase(words, g_GearTalkWords);
    }
}

namespace
{
    // "[Robe of Evocation] (green cloth robe)"
    std::string PieceWords(ItemTemplate const* proto, uint8_t slot)
    {
        const std::string kind = proto->Class == ITEM_CLASS_WEAPON ? WeaponKind(proto)
                               : proto->Class == ITEM_CLASS_ARMOR  ? ArmorKind(proto, slot)
                                                                   : std::string("piece of gear");
        const char* quality = Quality(proto->Quality);
        return "[" + proto->Name1 + "] (" + (*quality ? std::string(quality) + " " : std::string()) + kind + ")";
    }

    ItemTemplate const* Worn(Player* p, uint8_t slot)
    {
        Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item ? item->GetTemplate() : nullptr;
    }
}

std::string Gear_Describe(Player* p, Player* viewer)
{
    if (!p || !p->IsInWorld())
        return {};
    static const uint8_t kSlots[] = { EQUIPMENT_SLOT_HEAD,  EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_CHEST,
                                      EQUIPMENT_SLOT_LEGS,  EQUIPMENT_SLOT_HANDS,     EQUIPMENT_SLOT_FEET,
                                      EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED };
    std::vector<std::string> parts;
    for (uint8_t slot : kSlots)
    {
        ItemTemplate const* proto = Worn(p, slot);
        if (!proto)
        {
            if (slot == EQUIPMENT_SLOT_CHEST)
                parts.push_back("nothing on the chest");
            else if (slot == EQUIPMENT_SLOT_LEGS)
                parts.push_back("nothing on the legs");
            continue;
        }
        parts.push_back(PieceWords(proto, slot));
    }
    if (parts.empty())
        return {};
    return (p == viewer ? std::string("You are") : p->GetName() + " is") + " wearing " + Join(parts);
}

std::string Gear_OnePiece(Player* p)
{
    if (!p || !p->IsInWorld())
        return {};
    for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (ItemTemplate const* proto = Worn(p, slot))
            if (proto->Quality == ITEM_QUALITY_POOR && (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON))
                return PieceWords(proto, slot);
    std::vector<uint8_t> slots;
    for (uint8_t slot : { EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_MAINHAND })
        if (Worn(p, slot))
            slots.push_back(slot);
    if (slots.empty())
        return {};
    const uint8_t slot = slots[urand(0, uint32(slots.size() - 1))];
    return PieceWords(Worn(p, slot), slot);
}

std::string Gear_ShowPiece(Player* p)
{
    if (!p || !p->IsInWorld())
        return {};
    std::vector<uint8_t> slots = { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED,
                                   EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_CHEST,  EQUIPMENT_SLOT_TABARD };
    if (!p->HasPlayerFlag(PLAYER_FLAGS_HIDE_HELM))
        slots.push_back(EQUIPMENT_SLOT_HEAD);
    if (!p->HasPlayerFlag(PLAYER_FLAGS_HIDE_CLOAK))
        slots.push_back(EQUIPMENT_SLOT_BACK);
    std::vector<uint8_t> best;
    uint32_t bestQuality = 0;
    for (uint8_t slot : slots)
    {
        ItemTemplate const* proto = Worn(p, slot);
        // A wand or a thrown dagger is out of sight on a belt; a ranged one on the back is not.
        if (!proto || (slot == EQUIPMENT_SLOT_RANGED && (proto->SubClass == ITEM_SUBCLASS_WEAPON_WAND ||
                                                         proto->SubClass == ITEM_SUBCLASS_WEAPON_THROWN)))
            continue;
        if (best.empty() || proto->Quality > bestQuality)
        {
            best.assign(1, slot);
            bestQuality = proto->Quality;
        }
        else if (proto->Quality == bestQuality)
            best.push_back(slot);
    }
    if (best.empty())
        return {};
    const uint8_t slot = best[urand(0, uint32(best.size() - 1))];
    ItemTemplate const* proto = Worn(p, slot);
    if (slot == EQUIPMENT_SLOT_TABARD)
        return "[" + proto->Name1 + "] (a tabard)";
    if (slot == EQUIPMENT_SLOT_BACK)
    {
        const char* quality = Quality(proto->Quality);
        return "[" + proto->Name1 + "] (" + (*quality ? std::string(quality) + " " : std::string()) + "cloak)";
    }
    return PieceWords(proto, slot);
}

bool Gear_IsGearTalk(const std::string& msg)
{
    return g_GearEnable && Talks(CrowdText::Words(Context_CleanText(msg)));
}

std::string Gear_PromptFor(Player* bot, Player* speaker, const std::string& msg, ChatChannelSourceLocal source,
                           const std::string& channelName)
{
    if (!g_GearEnable || !bot)
        return {};
    std::string out;
    Group* group = bot->GetGroup();
    const auto now = Clock::now();

    // When gear comes up -- in this line or the last few -- what the people
    // in the conversation wear.
    const std::vector<std::string> words = CrowdText::Words(Context_CleanText(msg));
    bool topic = Talks(words);
    if (!topic && source != SRC_WHISPER_LOCAL)
        for (const ContextRecentLine& line : Context_RecentLines(bot, source, channelName, 3, 90))
            if ((topic = Talks(CrowdText::Words(line.text))))
                break;

    if (topic && !g_GearPromptText.empty())
    {
        std::vector<Player*> who;
        auto add = [&](Player* p)
        {
            if (p && p->IsInWorld() && who.size() < g_GearMaxPeople && std::find(who.begin(), who.end(), p) == who.end())
                who.push_back(p);
        };
        add(speaker);
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                {
                    const std::vector<std::string> name = CrowdText::Words(m->GetName());
                    if (!name.empty() && std::find(words.begin(), words.end(), name.front()) != words.end())
                        add(m);
                }
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    if (OllamaIsRealPlayer(m))
                        add(m);
        }
        add(bot);

        std::vector<std::string> said;
        for (Player* p : who)
        {
            const std::string worn = Gear_Describe(p, bot);
            if (!worn.empty())
                said.push_back(worn);
        }
        if (!said.empty())
        {
            std::string all;
            for (const std::string& s : said)
                all += (all.empty() ? "" : "; ") + s;
            const std::string text = SafeFormat(g_GearPromptText, fmt::arg("gear", all));
            if (!text.empty() && text != "[Format Error]")
            {
                out = text;
                ++g_stats.described;
            }
        }
    }

    // A real player here who felt picked on lately: leave them be.
    if (!g_GearBackOffText.empty() && !g_backOff.empty())
    {
        std::vector<Player*> people;
        if (OllamaIsRealPlayer(speaker))
            people.push_back(speaker);
        if (group)
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    if (OllamaIsRealPlayer(m) && std::find(people.begin(), people.end(), m) == people.end())
                        people.push_back(m);
        for (Player* p : people)
        {
            auto it = g_backOff.find(p->GetGUID().GetRawValue());
            if (it == g_backOff.end() || now - it->second > std::chrono::minutes(g_GearBackOffMinutes))
                continue;
            const std::string text = SafeFormat(g_GearBackOffText, fmt::arg("player_name", p->GetName()));
            if (!text.empty() && text != "[Format Error]")
                out += (out.empty() ? "" : " ") + text;
        }
    }
    return out;
}

void Gear_NoteHumanLine(Player* player, const std::string& msg)
{
    if (!g_GearEnable || !player || OllamaIsBotPlayer(player) || g_GearBackOffWords.empty())
        return;
    const std::vector<std::string> words = CrowdText::Words(Context_CleanText(msg));
    if (words.empty() || !CrowdText::ContainsPhrase(words, g_GearBackOffWords))
        return;
    g_backOff[player->GetGUID().GetRawValue()] = Clock::now();
    ++g_stats.backOffs;
}

bool Gear_FeelsPickedOn(Player* player)
{
    if (!player || g_backOff.empty())
        return false;
    auto it = g_backOff.find(player->GetGUID().GetRawValue());
    return it != g_backOff.end() && Clock::now() - it->second <= std::chrono::minutes(g_GearBackOffMinutes);
}

void Gear_Update()
{
    const auto now = Clock::now();
    for (auto it = g_backOff.begin(); it != g_backOff.end();)
        it = (now - it->second > std::chrono::minutes(g_GearBackOffMinutes)) ? g_backOff.erase(it) : std::next(it);
}

GearStats Gear_GetStats()
{
    return g_stats;
}
