#include "mod-ollama-chat_holidays.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "GameEventMgr.h"
#include "Log.h"
#include "Random.h"
#include "SharedDefines.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstdlib>

namespace
{
    struct Holiday
    {
        uint32_t    id = 0;                              // HolidayIds, as the game's calendar has it
        std::string name;
        std::vector<std::vector<std::string>> words;     // what it goes by in chat
        std::string about;                               // what is true of it
        bool        on = false;
    };
    std::vector<Holiday> g_holidays;
    HolidaysStats        g_stats;
    uint32_t             g_timer = 60000;   // looked at on the first tick

    std::string Trim(const std::string& s)
    {
        const size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos)
            return {};
        return s.substr(a, s.find_last_not_of(" \t") - a + 1);
    }

    // Which of them `text` talks of, the names of things in [brackets] aside:
    // a linked [Great Brewfest Kodo] is a mount, not the festival.
    std::vector<size_t> Named(const std::string& text)
    {
        std::vector<size_t> out;
        if (text.empty())
            return out;
        const std::string clean = TextRules::WithoutBracketNames(TextRules::StripChatMarkup(text));
        for (size_t i = 0; i < g_holidays.size(); ++i)
            if (!g_holidays[i].words.empty() && !TextRules::FindPhraseIndexes(clean, g_holidays[i].words).empty())
                out.push_back(i);
        return out;
    }

    std::string Fill(const std::string& templ, const Holiday& h)
    {
        const std::string out = SafeFormat(templ, fmt::arg("name", h.name), fmt::arg("about", h.about));
        return out == "[Format Error]" ? std::string() : out;
    }
}

void Holidays_Load(const std::string& list)
{
    g_holidays.clear();
    for (const std::string& raw : SplitString(list, '|'))
    {
        if (Trim(raw).empty())
            continue;
        const std::vector<std::string> f = SplitString(raw, ';');
        Holiday h;
        if (f.size() >= 4)
        {
            h.id   = uint32_t(std::strtoul(Trim(f[0]).c_str(), nullptr, 10));
            h.name = Trim(f[1]);
            std::string words = f[2];
            std::replace(words.begin(), words.end(), ',', '|');
            h.words = CrowdText::ParsePhrases(words);
            h.about = Trim(f[3]);
            for (size_t i = 4; i < f.size(); ++i)   // a ';' in what is true of it
                h.about += ";" + f[i];
        }
        if (!h.id || h.name.empty() || h.about.empty())
        {
            LOG_WARN("module.ollamachat", "[Ollama Chat] Holidays.List: '{}' is not 'id;Name;words;what is true of "
                     "it': left out.", raw);
            continue;
        }
        g_holidays.push_back(std::move(h));
    }
    g_stats.known = uint32_t(g_holidays.size());
    g_timer = 60000;
}

void Holidays_Update(uint32_t diff)
{
    g_timer += diff;
    if (g_timer < 60000)
        return;
    g_timer = 0;
    g_stats.on = 0;
    g_stats.names.clear();
    for (Holiday& h : g_holidays)
    {
        h.on = g_HolidaysEnable && IsHolidayActive(HolidayIds(h.id));
        if (!h.on)
            continue;
        ++g_stats.on;
        g_stats.names += (g_stats.names.empty() ? "" : ", ") + h.name;
    }
}

std::string Holidays_Fact()
{
    if (!g_HolidaysEnable || g_HolidaysFactText.empty())
        return {};
    std::vector<const Holiday*> on;
    for (const Holiday& h : g_holidays)
        if (h.on)
            on.push_back(&h);
    if (on.empty())
        return {};
    const std::string out = Fill(g_HolidaysFactText, *on[urand(0, uint32_t(on.size() - 1))]);
    if (!out.empty())
        ++g_stats.topics;
    return out;
}

std::string Holidays_PromptFor(const std::vector<std::string>& heard)
{
    if (!g_HolidaysEnable || g_holidays.empty())
        return {};
    std::vector<size_t> named;
    for (const std::string& line : heard)
        for (size_t i : Named(line))
            if (std::find(named.begin(), named.end(), i) == named.end())
                named.push_back(i);
    if (named.empty())
        return {};
    // Several that go by the same words (the Darkmoon Faire's three grounds):
    // the one that is on is the one meant.
    const bool anyOn = std::any_of(named.begin(), named.end(), [](size_t i) { return g_holidays[i].on; });
    std::string out;
    for (size_t i : named)
    {
        const Holiday& h = g_holidays[i];
        if (anyOn && !h.on)
            continue;
        const std::string part = Fill(h.on ? g_HolidaysOnText : g_HolidaysOffText, h);
        if (!part.empty() && out.find(part) == std::string::npos)
            out += (out.empty() ? "" : " ") + part;
        if (!anyOn)
            break;   // one "is not on" says it
    }
    if (!out.empty())
        ++g_stats.told;
    return out;
}

bool Holidays_MayGoOut(const std::string& text, const std::string& answering, bool byPerson, std::string& which,
                       std::string& note)
{
    which.clear();
    note.clear();
    if (!g_HolidaysEnable || !g_HolidaysGuard || g_holidays.empty())
        return true;
    const std::vector<size_t> named = Named(text);
    if (named.empty() ||
        std::any_of(named.begin(), named.end(), [](size_t i) { return g_holidays[i].on; }))
        return true;
    // Someone asked about it: it was told it is not on, and may say so.
    if (byPerson)
        for (size_t i : Named(answering))
            if (std::find(named.begin(), named.end(), i) != named.end())
                return true;
    const Holiday& h = g_holidays[named.front()];
    which = h.name;
    note  = Fill(g_HolidaysOffNote, h);
    ++g_stats.keptIn;
    return false;
}

HolidaysStats Holidays_GetStats()
{
    return g_stats;
}
