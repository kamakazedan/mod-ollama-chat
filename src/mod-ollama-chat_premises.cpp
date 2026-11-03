#include "mod-ollama-chat_premises.h"
#include "mod-ollama-chat_config.h"
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

    // `list`'s entries that fit `level`, as (raw, untagged): off cooldown if
    // any are, else the one used longest ago.
    std::vector<std::pair<std::string, std::string>> Candidates(const std::vector<std::string>& list, uint32_t level,
                                                                time_t now)
    {
        std::vector<std::pair<std::string, std::string>> fits, fresh;
        for (const std::string& raw : list)
        {
            std::string p = raw;
            if (!TextRules::PremiseFits(p, level) && g_OutlookEnable)
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

    // A value for the pool `name`, fitting `level`, not one of the pool's
    // latest picks nor already in this premise.
    std::string FromPool(const std::string& name, const std::vector<std::string>& taken, uint32_t level,
                         std::vector<std::pair<std::string, std::string>>& picked)
    {
        auto pool = g_PremisePools.find(name);
        if (pool == g_PremisePools.end())
            return {};
        const std::deque<std::string>& recent = g_recent[name];
        std::vector<std::string> fresh, fits;
        for (const std::string& raw : pool->second)
        {
            std::string v = raw;
            if (!TextRules::PremiseFits(v, level) && g_OutlookEnable)
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

    const bool templatesFirst = !g_PremiseTemplates.empty() && !g_PremisePools.empty() &&
                                g_PremiseTemplateChance > 0 &&
                                std::uniform_int_distribution<uint32_t>(1, 100)(Rng()) <= g_PremiseTemplateChance;

    auto tryList = [&](const std::vector<std::string>& list, bool templated) -> std::string
    {
        std::vector<std::pair<std::string, std::string>> cands = Candidates(list, level, now);
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
                    return FromPool(slot, taken, level, picked);
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
