#include "mod-ollama-chat_said.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
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
        uint64_t          to    = 0;   // said to them alone (a whisper)
        uint32_t          group = 0;   // said in this group's chat
        Clock::time_point when{};
    };

    // Per bot: enough to find the line someone asks back about, and the few
    // with something behind them. A whisper is kept before a channel line.
    constexpr size_t kKept = 8;

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

    // Could `asker` have heard it?
    bool HeardBy(const Said& s, Player* asker)
    {
        if (s.to)
            return asker && asker->GetGUID().GetRawValue() == s.to;
        if (s.group)
        {
            Group* group = asker ? asker->GetGroup() : nullptr;
            return group && group->GetGUID().GetCounter() == s.group;
        }
        return true;
    }

    // `word` is one of the single words of `list` ("haha*": any word starting so).
    bool OneOf(const std::string& word, const std::vector<std::vector<std::string>>& list)
    {
        for (const auto& entry : list)
        {
            if (entry.size() != 1 || entry.front().empty())
                continue;
            const std::string& w = entry.front();
            if (w.back() == '*' ? word.compare(0, w.size() - 1, w, 0, w.size() - 1) == 0 : word == w)
                return true;
        }
        return false;
    }

    // A word of the line asked about: "leggings" in "those leggings she's
    // rocking", "legging" too.
    bool InLine(const std::string& word, const std::vector<std::string>& said)
    {
        for (const std::string& s : said)
        {
            if (s == word)
                return true;
            const size_t shorter = std::min(s.size(), word.size());
            if (shorter >= 4 && s.compare(0, shorter, word, 0, shorter) == 0)
                return true;
        }
        return false;
    }

    // "lol who?", "what leggings?", "what do you mean?", "?": a short question
    // with nothing in it but asking words (one at least, or a question mark),
    // filler, and words of the line it is about. "what level are you?" is a
    // question of its own.
    bool AsksBack(const std::string& message, const std::string& line)
    {
        const std::vector<std::string> words = CrowdText::Words(message);
        const bool mark = message.find('?') != std::string::npos;
        if (words.empty())
            return mark;
        if (words.size() > g_SaidAskMaxWords)
            return false;
        const std::vector<std::string> said = CrowdText::Words(line);
        bool asking = mark;
        for (const std::string& w : words)
        {
            if (OneOf(w, g_SaidAskWords))
                asking = true;
            else if (!OneOf(w, g_SaidAskFiller) && !InLine(w, said))
                return false;
        }
        return asking;
    }

    std::string Fill(const std::string& templ, const Said& s, Clock::time_point now)
    {
        const std::string out = SafeFormat(templ,
                                           fmt::arg("ago", Ago(now - s.when)),
                                           fmt::arg("place", s.place),
                                           fmt::arg("line", s.line),
                                           fmt::arg("basis", s.basis));
        return out == "[Format Error]" ? std::string() : out;
    }
}

std::string Said_Place(ChatChannelSourceLocal source, const std::string& channelName, Player* speaker)
{
    return source == SRC_WHISPER_LOCAL ? std::string("a whisper")
                                       : Context_ChannelLabel(source, channelName, speaker);
}

void Said_Note(Player* bot, const std::string& line, const std::string& place, const std::string& basis,
               uint64_t to, uint32_t group)
{
    if (!g_SaidEnable || !bot || line.empty())
        return;
    Said s;
    s.line  = line;
    s.place = place;
    s.basis = basis.empty() ? std::string() : Tidy(basis);
    s.to    = to;
    s.group = to ? 0 : group;
    s.when  = Clock::now();
    std::deque<Said>& list = g_said[bot->GetGUID().GetRawValue()];
    list.push_back(std::move(s));
    ++g_stats.lines;
    if (!list.back().basis.empty())
        ++g_stats.noted;
    while (list.size() > kKept)
    {
        // The oldest line to everyone goes first; a whisper is someone's to ask about.
        auto drop = std::find_if(list.begin(), list.end(), [](const Said& x) { return !x.to; });
        list.erase(drop == list.end() || drop == list.end() - 1 ? list.begin() : drop);
    }
}

std::string Said_PromptFor(Player* bot, Player* asker, const std::string& message, const std::string& place,
                           std::string* askedBasis)
{
    if (askedBasis)
        askedBasis->clear();
    if (!g_SaidEnable || !bot)
        return {};
    auto it = g_said.find(bot->GetGUID().GetRawValue());
    if (it == g_said.end())
        return {};
    const std::deque<Said>& list = it->second;
    const auto now = Clock::now();

    // The line they ask back about, if they do: the newest they heard where
    // they ask, else the newest they heard at all, still fresh.
    const Said* asked = nullptr;
    if (asker && g_SaidAskMinutes && !g_SaidAskedText.empty())
    {
        const Said* newest = nullptr;
        const Said* here = nullptr;
        for (auto r = list.rbegin(); r != list.rend(); ++r)
        {
            if (now - r->when > std::chrono::minutes(g_SaidAskMinutes))
                break;
            if (!HeardBy(*r, asker))
                continue;
            if (!newest)
                newest = &*r;
            if (r->place == place)
            {
                here = &*r;
                break;
            }
        }
        const Said* candidate = here ? here : newest;
        if (candidate && AsksBack(message, candidate->line))
            asked = candidate;
    }

    // The last few they heard with something behind them.
    std::string lines;
    if (!g_SaidText.empty() && !g_SaidLineText.empty() && g_SaidMax)
    {
        std::vector<const Said*> picked;
        for (auto r = list.rbegin(); r != list.rend() && picked.size() < g_SaidMax; ++r)
            if (&*r != asked && !r->basis.empty() && now - r->when <= std::chrono::minutes(g_SaidMinutes) &&
                HeardBy(*r, asker))
                picked.push_back(&*r);
        for (auto p = picked.rbegin(); p != picked.rend(); ++p)
        {
            const std::string one = Fill(g_SaidLineText, **p, now);
            if (!one.empty())
                lines += (lines.empty() ? "" : "; ") + one;
        }
    }

    std::string out;
    if (!lines.empty())
    {
        const std::string told = SafeFormat(g_SaidText, fmt::arg("lines", lines));
        if (!told.empty() && told != "[Format Error]")
        {
            out = told;
            ++g_stats.told;
        }
    }
    if (asked)
    {
        std::string wentOn;
        if (!asked->basis.empty() && !g_SaidAskedBasisText.empty())
            wentOn = Fill(g_SaidAskedBasisText, *asked, now);
        if (!wentOn.empty() && wentOn.front() != ' ')
            wentOn.insert(wentOn.begin(), ' ');
        const std::string ask = SafeFormat(g_SaidAskedText,
                                           fmt::arg("player_name", asker->GetName()),
                                           fmt::arg("ago", Ago(now - asked->when)),
                                           fmt::arg("place", asked->place),
                                           fmt::arg("line", asked->line),
                                           fmt::arg("went_on", wentOn));
        if (!ask.empty() && ask != "[Format Error]")
        {
            out += (out.empty() ? "" : " ") + ask;
            ++g_stats.askedBack;
            if (askedBasis)
                *askedBasis = asked->basis;
        }
    }
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
