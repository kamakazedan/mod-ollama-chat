#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "DBCStores.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"

#include <fmt/core.h>

#include <random>

namespace
{
    std::vector<TextRules::LevelBand> g_bands;
    uint64_t g_homeCityPrompts = 0;   // prompts that said a bot was in its own side's city

    const char* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "warrior";
            case CLASS_PALADIN:      return "paladin";
            case CLASS_HUNTER:       return "hunter";
            case CLASS_ROGUE:        return "rogue";
            case CLASS_PRIEST:       return "priest";
            case CLASS_DEATH_KNIGHT: return "death knight";
            case CLASS_SHAMAN:       return "shaman";
            case CLASS_MAGE:         return "mage";
            case CLASS_WARLOCK:      return "warlock";
            case CLASS_DRUID:        return "druid";
            default:                 return "";
        }
    }

    // The game's rules, not a setting: whatever a mage says, it cannot heal.
    std::string RoleText(Player* bot)
    {
        const uint8 cls = bot->getClass();
        const char* name = ClassName(cls);
        if (!*name)
            return "";

        bool tank = false, heal = false;
        switch (cls)
        {
            case CLASS_WARRIOR:
            case CLASS_DEATH_KNIGHT: tank = true;        break;
            case CLASS_PRIEST:
            case CLASS_SHAMAN:       heal = true;        break;
            case CLASS_PALADIN:
            case CLASS_DRUID:        tank = heal = true; break;
            default:                                     break;
        }

        if (!tank && !heal)
            return fmt::format("As a {} you can only do damage, so never offer to tank or heal.", name);

        // What its strategies make it do now, the same test the playerbots
        // group logic uses to pick a tank or a healer.
        const char* now = PlayerbotAI::IsTank(bot) ? "tank"
                        : PlayerbotAI::IsHeal(bot) ? "heal"
                                                   : "do damage";
        if (tank && heal)
            return fmt::format("As a {} you can tank, heal or do damage; right now you are set up to {}.",
                               name, now);
        if (tank)
            return fmt::format("As a {} you can tank or do damage but never heal; right now you are set up "
                               "to {}.", name, now);
        return fmt::format("As a {} you can heal or do damage but never tank; right now you are set up to {}.",
                           name, now);
    }
}

void Outlook_Load()
{
    g_bands = TextRules::ParseLevelBands(g_OutlookLevelBands);
}

size_t Outlook_BandCount()
{
    return g_bands.size();
}

std::string Outlook_LevelText(uint32_t level)
{
    const TextRules::LevelBand* band = TextRules::FindLevelBand(g_bands, level);
    if (!band)
        return "";
    const std::string text = SafeFormat(band->text, fmt::arg("level", level));
    return text == "[Format Error]" ? std::string() : text;
}

std::string Outlook_SideCities(bool alliance)
{
    return CrowdText::JoinNames(alliance ? g_OutlookCitiesAlliance : g_OutlookCitiesHorde);
}

std::string Outlook_HomeCity(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return "";
    Map* map = bot->GetMap();
    if (!map || map->Instanceable())
        return "";
    // The game's own say: a capital (AREA_FLAG_CAPITAL, where it lets a
    // character rest) of this character's side -- the same test that makes
    // the other side's capital hostile ground for it.
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    if (!zone || !(zone->flags & AREA_FLAG_CAPITAL))
        return "";
    const uint32 mine = bot->GetTeamId() == TEAM_ALLIANCE ? uint32(AREATEAM_ALLY) : uint32(AREATEAM_HORDE);
    if (zone->team != mine)
        return "";
    return PlayerbotAI::GetLocalizedAreaName(zone);
}

std::string Outlook_SideText(Player* bot)
{
    if (!bot)
        return "";
    const bool alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    const char* side = alliance ? "Alliance" : "Horde";

    std::string out;
    const std::string cities = Outlook_SideCities(alliance);
    if (!g_OutlookSideText.empty() && !cities.empty())
    {
        const std::string text = SafeFormat(g_OutlookSideText, fmt::arg("side", side), fmt::arg("cities", cities));
        if (text != "[Format Error]")
            out = text;
    }
    const std::string city = g_OutlookHomeCityText.empty() ? std::string() : Outlook_HomeCity(bot);
    if (!city.empty())
    {
        const std::string text = SafeFormat(g_OutlookHomeCityText, fmt::arg("city", city), fmt::arg("side", side));
        if (text != "[Format Error]")
        {
            out += (out.empty() ? "" : " ") + text;
            ++g_homeCityPrompts;
        }
    }
    return out;
}

uint64_t Outlook_HomeCityPrompts()
{
    return g_homeCityPrompts;
}

std::string Outlook_ForBot(Player* bot)
{
    if (!g_OutlookEnable || !bot)
        return "";

    std::string out;
    auto add = [&out](const std::string& s)
    {
        if (!s.empty())
            out += " " + s;
    };

    add(Outlook_LevelText(bot->GetLevel()));
    // Its side, and in one of its own cities that it is home: no "loads of
    // grunts around here, too high level" in Orgrimmar, no "afk in stormwind?"
    // from a Horde player.
    add(Outlook_SideText(bot));
    if (g_OutlookClassRoles && PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        add(RoleText(bot));
    add(g_OutlookKnowledgeText);
    // Nothing made up from the game: no Trinket of the Hidden Sun.
    add(g_OutlookFactsText);
    return out;
}

bool Outlook_PremiseFits(std::string& premise, uint32_t level)
{
    return TextRules::PremiseFits(premise, level);
}

std::string Outlook_PickPremise(const std::vector<std::string>& premises, uint32_t level)
{
    std::vector<std::string> fits;
    fits.reserve(premises.size());
    for (std::string p : premises)
    {
        // With the outlook off every premise is fair game, but the tag still
        // has to come off before the model sees it.
        if (TextRules::PremiseFits(p, level) || !g_OutlookEnable)
            if (!p.empty())
                fits.push_back(std::move(p));
    }
    if (fits.empty())
        return "";

    static thread_local std::mt19937 rng{ std::random_device{}() };
    return fits[std::uniform_int_distribution<size_t>(0, fits.size() - 1)(rng)];
}
