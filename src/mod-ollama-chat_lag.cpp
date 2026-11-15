#include "mod-ollama-chat_lag.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "Random.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <deque>

namespace
{
    using Clock = std::chrono::steady_clock;

    // The world's ticks, ten seconds to a bucket: how long they took in all,
    // the same weighted by how long each took (so the mean is the tick a
    // moment of play sat in), and how many were hitches.
    struct Bucket
    {
        int64_t  second  = 0;   // steady seconds at the bucket's start, a multiple of kBucketSeconds
        uint64_t ms      = 0;
        uint64_t msSq    = 0;
        uint32_t ticks   = 0;
        uint32_t spikes  = 0;
    };
    constexpr int64_t  kBucketSeconds = 10;
    std::deque<Bucket> g_buckets;

    struct Judged
    {
        bool     lagging = false;
        uint32_t meanMs  = 0;
        uint32_t spikes  = 0;
        uint32_t covered = 0;   // seconds of the window there are ticks for
    };
    Judged  g_judged;
    int64_t g_judgedAt = -1;   // steady second it was judged at

    LagStats g_stats;
    Clock::time_point g_lastAmbient{};
    bool g_ambientEver = false;

    int64_t NowSecond()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    }

    int64_t WindowSeconds()
    {
        return int64_t(std::max<uint32_t>(1, g_LagMinutes)) * 60;
    }

    // The last Lag.Minutes, judged at most once a second.
    Judged const& Judge()
    {
        const int64_t now = NowSecond();
        if (g_judgedAt == now)
            return g_judged;
        g_judgedAt = now;

        const int64_t from = now - WindowSeconds();
        uint64_t ms = 0, msSq = 0;
        uint32_t spikes = 0;
        int64_t first = now;
        for (Bucket const& b : g_buckets)
        {
            if (b.second + kBucketSeconds <= from)
                continue;
            ms += b.ms;
            msSq += b.msSq;
            spikes += b.spikes;
            first = std::min(first, b.second);
        }
        Judged j;
        j.meanMs  = ms ? uint32_t(msSq / ms) : 0;
        j.spikes  = spikes;
        j.covered = uint32_t(std::clamp<int64_t>(now - first, 0, WindowSeconds()));
        j.lagging = g_LagEnable && ms &&
                    ((g_LagMeanMs && j.meanMs >= g_LagMeanMs) || (g_LagSpikes && spikes >= g_LagSpikes));
        if (j.lagging && !g_judged.lagging)
            ++g_stats.spells;
        g_judged = j;
        return g_judged;
    }

    std::string Fill(const std::string& text)
    {
        if (text.empty())
            return {};
        const std::string out = SafeFormat(text, fmt::arg("minutes", std::max<uint32_t>(1, g_LagMinutes)));
        return out == "[Format Error]" ? std::string() : out;
    }
}

void Lag_Tick(uint32_t diffMs)
{
    if (!g_LagEnable)
        return;
    const int64_t now = NowSecond();
    const int64_t start = now - now % kBucketSeconds;
    if (g_buckets.empty() || g_buckets.back().second != start)
    {
        g_buckets.push_back(Bucket{ start });
        const int64_t keepFrom = start - WindowSeconds() - kBucketSeconds;
        while (!g_buckets.empty() && g_buckets.front().second < keepFrom)
            g_buckets.pop_front();
    }
    Bucket& b = g_buckets.back();
    b.ms += diffMs;
    b.msSq += uint64_t(diffMs) * diffMs;
    ++b.ticks;
    if (g_LagSpikeMs && diffMs >= g_LagSpikeMs)
        ++b.spikes;
    Judge();   // so a spell of lag is counted as it starts
}

bool Lag_Now()
{
    return g_LagEnable && Judge().lagging;
}

bool Lag_TalksOf(const std::string& text, std::string& which)
{
    which.clear();
    if (!g_LagEnable || g_LagWords.empty() || text.empty())
        return false;
    const std::vector<size_t> found =
        TextRules::FindPhraseIndexes(TextRules::WithoutBracketNames(TextRules::StripChatMarkup(text)), g_LagWords);
    if (found.empty())
        return false;
    which = TextRules::PhraseKey(g_LagWords[found.front()]);
    return true;
}

bool Lag_MayGoOut(const std::string& text, const std::string& answering, std::string& which)
{
    if (!g_LagEnable || !Lag_TalksOf(text, which))
        return true;
    if (Lag_Now())
        return true;
    std::string theirs;
    if (!answering.empty() && Lag_TalksOf(answering, theirs))
        return true;   // "lag?" -- "none here": told how it really is
    ++g_stats.keptIn;
    return false;
}

std::string Lag_PromptFor(const std::vector<std::string>& heard)
{
    if (!g_LagEnable)
        return {};
    std::string which;
    const bool about = std::any_of(heard.begin(), heard.end(),
                                   [&which](const std::string& line) { return Lag_TalksOf(line, which); });
    if (!about)
        return {};
    if (Lag_Now())
    {
        const std::string out = Fill(g_LagLaggingText);
        if (!out.empty())
            ++g_stats.toldLagging;
        return out;
    }
    const std::string out = Fill(g_LagSmoothText);
    if (!out.empty())
        ++g_stats.toldSmooth;
    return out;
}

std::string Lag_AmbientText()
{
    if (!g_LagEnable || g_LagAmbientChance == 0 || g_LagAmbientText.empty() || !Lag_Now())
        return {};
    const auto now = Clock::now();
    if (g_ambientEver && now - g_lastAmbient < std::chrono::minutes(g_LagAmbientMinutes))
        return {};
    // urand is inclusive at both ends: 1..100 against the chance.
    if (urand(1, 100) > g_LagAmbientChance)
        return {};
    const std::string out = Fill(g_LagAmbientText);
    if (out.empty())
        return {};
    g_lastAmbient = now;
    g_ambientEver = true;
    ++g_stats.toldLagging;
    return out;
}

LagStats Lag_GetStats()
{
    Judged const& j = Judge();
    LagStats s = g_stats;
    s.lagging    = j.lagging;
    s.meanMs     = j.meanMs;
    s.spikes     = j.spikes;
    s.windowSecs = j.covered;
    return s;
}
