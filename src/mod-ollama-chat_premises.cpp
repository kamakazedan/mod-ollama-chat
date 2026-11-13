#include "mod-ollama-chat_premises.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_facts.h"
#include "mod-ollama-chat_textrules.h"

#include "Player.h"
#include "SharedDefines.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <ctime>
#include <deque>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{
    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    // Raw premise or template -> when it was last used, realm-wide.
    std::unordered_map<std::string, time_t> g_used;
    // Pool -> its latest picks, newest last.
    std::unordered_map<std::string, std::deque<std::string>> g_recent;

    PremiseStats g_stats;

    // Whether a tagged premise or value is for this bot: its side always (a
    // Horde player does not go AFK in Stormwind), its level while the outlook
    // is on.
    bool Fits(std::string& premise, uint32_t level, const std::string& side)
    {
        const TextRules::PremiseTags fit = TextRules::TakeLevelAndSide(premise, level, side);
        return fit.sideFits && (fit.levelFits || !g_OutlookEnable);
    }

    // `list`'s entries that fit `level` and `side`, as (raw, untagged): off
    // cooldown if any are, else the one used longest ago.
    std::vector<std::pair<std::string, std::string>> Candidates(const std::vector<std::string>& list, uint32_t level,
                                                                const std::string& side, time_t now)
    {
        std::vector<std::pair<std::string, std::string>> fits, fresh;
        for (const std::string& raw : list)
        {
            std::string p = raw;
            if (!Fits(p, level, side))
                continue;
            if (p.empty())
                continue;
            fits.emplace_back(raw, p);
            auto used = g_used.find(raw);
            if (used == g_used.end() || g_PremiseCooldownMinutes == 0 ||
                now - used->second >= time_t(g_PremiseCooldownMinutes) * 60)
                fresh.emplace_back(raw, p);
        }
        if (!fresh.empty() || fits.empty())
            return fresh;
        auto oldest = std::min_element(fits.begin(), fits.end(), [](const auto& a, const auto& b)
                                       { return g_used[a.first] < g_used[b.first]; });
        return { *oldest };
    }

    // A value for the pool `name`, fitting `level` and `side`, not one of the
    // pool's latest picks nor already in this premise.
    std::string FromPool(const std::string& name, const std::vector<std::string>& taken, uint32_t level,
                         const std::string& side, std::vector<std::pair<std::string, std::string>>& picked)
    {
        auto pool = g_PremisePools.find(name);
        if (pool == g_PremisePools.end())
            return {};
        const std::deque<std::string>& recent = g_recent[name];
        std::vector<std::string> fresh, fits;
        for (const std::string& raw : pool->second)
        {
            std::string v = raw;
            if (!Fits(v, level, side))
                continue;
            if (v.empty() || std::find(taken.begin(), taken.end(), v) != taken.end())
                continue;
            fits.push_back(v);
            if (std::find(recent.begin(), recent.end(), v) == recent.end())
                fresh.push_back(v);
        }
        const std::vector<std::string>& from = fresh.empty() ? fits : fresh;
        if (from.empty())
            return {};
        std::string v = from[std::uniform_int_distribution<size_t>(0, from.size() - 1)(Rng())];
        picked.emplace_back(name, v);
        return v;
    }
}

std::string Premises_Pick(Player* bot)
{
    if (!bot)
        return {};
    const time_t now = time(nullptr);
    const uint32_t level = bot->GetLevel();

    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    AreaTableEntry const* zone = ai ? ai->GetCurrentZone() : nullptr;
    const std::string zoneName = zone ? PlayerbotAI::GetLocalizedAreaName(zone) : std::string("town");
    const bool alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    const std::string side = TextRules::SideTag(alliance);

    const bool templatesFirst = !g_PremiseTemplates.empty() && !g_PremisePools.empty() &&
                                g_PremiseTemplateChance > 0 &&
                                std::uniform_int_distribution<uint32_t>(1, 100)(Rng()) <= g_PremiseTemplateChance;

    auto tryList = [&](const std::vector<std::string>& list, bool templated) -> std::string
    {
        std::vector<std::pair<std::string, std::string>> cands = Candidates(list, level, side, now);
        std::shuffle(cands.begin(), cands.end(), Rng());
        for (size_t i = 0; i < cands.size() && i < 6; ++i)
        {
            std::vector<std::pair<std::string, std::string>> picked;
            const std::string expanded = TextRules::ExpandChoices(cands[i].second, Rng());
            std::string filled = TextRules::FillSlots(expanded,
                [&](const std::string& slot, const std::vector<std::string>& taken) -> std::string
                {
                    if (slot == "zone")
                        return zoneName;
                    if (slot == "faction")
                        return alliance ? "the Alliance" : "the Horde";
                    if (slot == "other_faction")
                        return alliance ? "the Horde" : "the Alliance";
                    // Real names from the game's data, not ones a model would
                    // make up (mod-ollama-chat_facts).
                    if (slot == "trinket")
                        return Facts_Trinket(level, taken);
                    if (slot == "itemset")
                        return Facts_ItemSet(level, taken);
                    if (slot == "rare")
                        return Facts_Rare(level, taken);
                    if (slot == "bosses" || slot == "class_races")
                    {
                        // About the dungeon, raid or class picked just before it.
                        for (auto it = picked.rbegin(); it != picked.rend(); ++it)
                        {
                            if (slot == "bosses" && (it->first == "dungeon" || it->first == "raid"))
                                return Facts_Bosses(it->second);
                            if (slot == "class_races" && it->first == "class")
                                return Facts_ClassRaces(it->second, alliance);
                        }
                        return slot == "bosses" ? std::string(" ") : std::string();
                    }
                    return FromPool(slot, taken, level, side, picked);
                });
            if (filled.empty())
                continue;
            if (filled.find("{~") != std::string::npos)   // a pool value with wording of its own
                filled = TextRules::ExpandChoices(filled, Rng());
            g_used[cands[i].first] = now;
            for (const auto& [pool, value] : picked)
            {
                std::deque<std::string>& recent = g_recent[pool];
                recent.push_back(value);
                while (recent.size() > std::max<uint32_t>(1, g_PremiseValueMemory))
                    recent.pop_front();
            }
            ++(templated ? g_stats.templated : g_stats.fixed);
            return filled;
        }
        return {};
    };

    std::string out = templatesFirst ? tryList(g_PremiseTemplates, true) : tryList(g_ConversationPremises, false);
    if (out.empty())
        out = templatesFirst ? tryList(g_ConversationPremises, false) : tryList(g_PremiseTemplates, true);
    if (out.empty())
        ++g_stats.none;
    return out;
}

PremiseStats Premises_GetStats()
{
    return g_stats;
}
