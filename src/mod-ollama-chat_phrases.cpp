#include "mod-ollama-chat_phrases.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_textrules.h"

#include <ctime>
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

void Phrases_Update()
{
    const time_t now = time(nullptr);
    const time_t cutoff = now - static_cast<time_t>(g_ResponseOverusedCooldownSeconds);
    for (auto it = g_lastSaid.begin(); it != g_lastSaid.end(); )
        it = (it->second < cutoff) ? g_lastSaid.erase(it) : std::next(it);
    const time_t rareCutoff = now - static_cast<time_t>(g_ResponseRareCooldownSeconds);
    for (auto it = g_lastRare.begin(); it != g_lastRare.end(); )
        it = (it->second < rareCutoff) ? g_lastRare.erase(it) : std::next(it);
}

PhrasesStats Phrases_GetStats()
{
    PhrasesStats s;
    s.droppedOffEra   = g_offEra;
    s.droppedOffLevel = g_offLevel;
    s.droppedOverused = g_overused;
    s.droppedRare     = g_rare;
    s.coolingDown     = static_cast<uint32_t>(g_lastSaid.size() + g_lastRare.size());
    return s;
}
