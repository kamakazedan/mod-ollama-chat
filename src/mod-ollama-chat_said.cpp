#include "mod-ollama-chat_said.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"

#include "ObjectGuid.h"
#include "Player.h"

#include <fmt/core.h>

#include <chrono>
#include <deque>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Said
    {
        std::string       line;
        std::string       place;
        std::string       basis;
        Clock::time_point when{};
    };

    std::unordered_map<uint64_t, std::deque<Said>> g_said;   // by bot raw guid
    SaidStats                                      g_stats;

    // What it went on, tidied for a prompt: one line, no quotes to trip over,
    // and not so long that it crowds out the rest.
    std::string Tidy(std::string text)
    {
        for (char& c : text)
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
        while (!text.empty() && (text.back() == ' ' || text.back() == '.'))
            text.pop_back();
        while (!text.empty() && text.front() == ' ')
            text.erase(text.begin());
        if (g_SaidMaxBasisChars && text.size() > g_SaidMaxBasisChars)
        {
            size_t cut = text.rfind(' ', g_SaidMaxBasisChars);
            text.resize(cut == std::string::npos || cut < g_SaidMaxBasisChars / 2 ? g_SaidMaxBasisChars : cut);
            text += "...";
        }
        return text;
    }

    std::string Ago(Clock::duration d)
    {
        const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(d).count();
        if (minutes < 1)
            return "just now";
        if (minutes == 1)
            return "a minute ago";
        return fmt::format("{} minutes ago", minutes);
    }
}

void Said_Note(Player* bot, const std::string& line, const std::string& place, const std::string& basis)
{
    if (!g_SaidEnable || !bot || line.empty() || basis.empty() || !g_SaidMax)
        return;
    Said s;
    s.line  = line;
    s.place = place;
    s.basis = Tidy(basis);
    s.when  = Clock::now();
    if (s.basis.empty())
        return;
    std::deque<Said>& list = g_said[bot->GetGUID().GetRawValue()];
    list.push_back(std::move(s));
    while (list.size() > g_SaidMax)
        list.pop_front();
    ++g_stats.noted;
}

std::string Said_PromptFor(Player* bot)
{
    if (!g_SaidEnable || !bot || g_SaidText.empty() || g_SaidLineText.empty())
        return {};
    auto it = g_said.find(bot->GetGUID().GetRawValue());
    if (it == g_said.end())
        return {};

    const auto now = Clock::now();
    const auto window = std::chrono::minutes(g_SaidMinutes);
    std::string lines;
    for (const Said& s : it->second)
    {
        if (now - s.when > window)
            continue;
        const std::string one = SafeFormat(g_SaidLineText,
                                           fmt::arg("ago", Ago(now - s.when)),
                                           fmt::arg("place", s.place),
                                           fmt::arg("line", s.line),
                                           fmt::arg("basis", s.basis));
        if (one.empty() || one == "[Format Error]")
            continue;
        lines += (lines.empty() ? "" : "; ") + one;
    }
    if (lines.empty())
        return {};
    const std::string out = SafeFormat(g_SaidText, fmt::arg("lines", lines));
    if (out.empty() || out == "[Format Error]")
        return {};
    ++g_stats.told;
    return out;
}

void Said_Forget(ObjectGuid botGuid)
{
    g_said.erase(botGuid.GetRawValue());
}

void Said_Update()
{
    const auto cutoff = Clock::now() - std::chrono::minutes(g_SaidMinutes);
    for (auto it = g_said.begin(); it != g_said.end();)
    {
        std::deque<Said>& list = it->second;
        while (!list.empty() && list.front().when < cutoff)
            list.pop_front();
        it = list.empty() ? g_said.erase(it) : std::next(it);
    }
}

SaidStats Said_GetStats()
{
    SaidStats s = g_stats;
    s.bots = uint32_t(g_said.size());
    return s;
}
