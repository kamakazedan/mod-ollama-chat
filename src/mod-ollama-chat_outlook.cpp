#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"

#include <fmt/core.h>

#include <random>

namespace
{
    std::vector<TextRules::LevelBand> g_bands;

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
    if (g_OutlookClassRoles && PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        add(RoleText(bot));
    add(g_OutlookKnowledgeText);
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
