#include "mod-ollama-chat_phrases.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_textrules.h"

#include <algorithm>
#include <ctime>
#include <deque>
#include <iterator>
#include <unordered_map>

namespace
{
    // By TextRules::PhraseKey, so a reload that reorders the list keeps them.
    std::unordered_map<std::string, time_t> g_lastSaid;
    std::unordered_map<std::string, time_t> g_lastRare;

    uint64_t g_offEra   = 0;
    uint64_t g_offLevel = 0;
    uint64_t g_overused = 0;
    uint64_t g_rare     = 0;
    uint64_t g_echoes   = 0;

    // phrase -> who said it when, oldest first
    struct Use
    {
        time_t   at  = 0;
        uint64_t bot = 0;
    };
    std::unordered_map<std::string, std::deque<Use>> g_grams;
    constexpr size_t kMaxGrams = 60000;
}

PhraseCheck Phrases_Check(const std::string& text, const std::string& answering, bool directAddress,
                          uint32_t level, std::string& which)
{
    which.clear();

    // Words the line being answered used are fair game: "can you transmog
    // in wrath?" gets "no transmog in wrath lol".
    const std::vector<size_t> offEra = TextRules::FindPhraseIndexes(text, g_ResponseOffEraPhrases, answering);
    if (!offEra.empty())
    {
        which = TextRules::PhraseKey(g_ResponseOffEraPhrases[offEra.front()]);
        ++g_offEra;
        return PhraseCheck::OffEra;
    }

    if (g_ResponseRareCooldownSeconds > 0)
    {
        const time_t now = time(nullptr);
        for (size_t i : TextRules::FindPhraseIndexes(text, g_ResponseRarePhrases, answering))
        {
            const std::string key = TextRules::PhraseKey(g_ResponseRarePhrases[i]);
            auto it = g_lastRare.find(key);
            if (it != g_lastRare.end() && now - it->second < static_cast<time_t>(g_ResponseRareCooldownSeconds))
            {
                which = key;
                ++g_rare;
                return PhraseCheck::Rare;
            }
        }
    }

    if (directAddress)
        return PhraseCheck::Ok;

    if (g_OutlookEnable)
        if (const TextRules::LevelPhrase* off =
                TextRules::FindOffLevelPhrase(text, g_OutlookLevelPhrases, level, answering))
        {
            which = TextRules::PhraseKey(off->words);
            ++g_offLevel;
            return PhraseCheck::OffLevel;
        }

    if (g_ResponseOverusedCooldownSeconds == 0)
        return PhraseCheck::Ok;

    const time_t now = time(nullptr);
    for (size_t i : TextRules::FindPhraseIndexes(text, g_ResponseOverusedPhrases, answering))
    {
        const std::string key = TextRules::PhraseKey(g_ResponseOverusedPhrases[i]);
        auto it = g_lastSaid.find(key);
        if (it != g_lastSaid.end() && now - it->second < static_cast<time_t>(g_ResponseOverusedCooldownSeconds))
        {
            which = key;
            ++g_overused;
            return PhraseCheck::Overused;
        }
    }
    return PhraseCheck::Ok;
}

void Phrases_NoteSaid(const std::string& text)
{
    const time_t now = time(nullptr);
    if (g_ResponseOverusedCooldownSeconds > 0)
        for (size_t i : TextRules::FindPhraseIndexes(text, g_ResponseOverusedPhrases))
            g_lastSaid[TextRules::PhraseKey(g_ResponseOverusedPhrases[i])] = now;
    if (g_ResponseRareCooldownSeconds > 0)
        for (size_t i : TextRules::FindPhraseIndexes(text, g_ResponseRarePhrases))
            g_lastRare[TextRules::PhraseKey(g_ResponseRarePhrases[i])] = now;
}

bool Phrases_IsEcho(uint64_t botGuid, const std::string& text, const std::string& answering, std::string& which)
{
    which.clear();
    if (!g_EchoEnable || text.empty() || g_grams.empty())
        return false;
    const std::vector<std::string> grams = TextRules::ContentGrams(text, g_EchoWords, g_EchoCommonWords);
    if (grams.empty())
        return false;
    const std::vector<std::string> theirs = answering.empty()
                                                ? std::vector<std::string>()
                                                : TextRules::ContentGrams(answering, g_EchoWords, g_EchoCommonWords);
    const time_t cutoff = time(nullptr) - time_t(g_EchoWindowMinutes) * 60;
    for (const std::string& gram : grams)
    {
        if (std::find(theirs.begin(), theirs.end(), gram) != theirs.end())
            continue;
        auto it = g_grams.find(gram);
        if (it == g_grams.end())
            continue;
        std::vector<uint64_t> others;
        for (const Use& u : it->second)
            if (u.at >= cutoff && u.bot != botGuid && std::find(others.begin(), others.end(), u.bot) == others.end())
                others.push_back(u.bot);
        if (others.size() >= g_EchoBots)
        {
            which = gram;
            ++g_echoes;
            return true;
        }
    }
    return false;
}

void Phrases_NoteEcho(uint64_t botGuid, const std::string& text)
{
    if (!g_EchoEnable || text.empty())
        return;
    if (g_grams.size() > kMaxGrams)
        g_grams.clear();   // a flood: start counting again rather than grow without end
    const time_t now = time(nullptr);
    for (const std::string& gram : TextRules::ContentGrams(text, g_EchoWords, g_EchoCommonWords))
    {
        std::deque<Use>& uses = g_grams[gram];
        uses.push_back({ now, botGuid });
        while (uses.size() > 32)
            uses.pop_front();
    }
}

void Phrases_Update()
{
    const time_t now = time(nullptr);
    const time_t cutoff = now - static_cast<time_t>(g_ResponseOverusedCooldownSeconds);
    for (auto it = g_lastSaid.begin(); it != g_lastSaid.end(); )
        it = (it->second < cutoff) ? g_lastSaid.erase(it) : std::next(it);
    const time_t rareCutoff = now - static_cast<time_t>(g_ResponseRareCooldownSeconds);
    for (auto it = g_lastRare.begin(); it != g_lastRare.end(); )
        it = (it->second < rareCutoff) ? g_lastRare.erase(it) : std::next(it);
    const time_t echoCutoff = now - static_cast<time_t>(g_EchoWindowMinutes) * 60;
    for (auto it = g_grams.begin(); it != g_grams.end(); )
    {
        std::deque<Use>& uses = it->second;
        while (!uses.empty() && uses.front().at < echoCutoff)
            uses.pop_front();
        it = uses.empty() ? g_grams.erase(it) : std::next(it);
    }
}

PhrasesStats Phrases_GetStats()
{
    PhrasesStats s;
    s.droppedOffEra   = g_offEra;
    s.droppedOffLevel = g_offLevel;
    s.droppedOverused = g_overused;
    s.droppedRare     = g_rare;
    s.echoes          = g_echoes;
    s.phrasesTracked  = static_cast<uint32_t>(g_grams.size());
    s.coolingDown     = static_cast<uint32_t>(g_lastSaid.size() + g_lastRare.size());
    return s;
}
