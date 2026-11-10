#include "mod-ollama-chat_race.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace
{
    struct RaceInfo
    {
        uint8_t     id;
        const char* name;     // "orc"
        const char* plural;   // "orcs"
        const char* one;      // "an orc"
    };

    const RaceInfo kRaces[] = {
        { RACE_HUMAN,         "human",     "humans",       "a human" },
        { RACE_ORC,           "orc",       "orcs",         "an orc" },
        { RACE_DWARF,         "dwarf",     "dwarves",      "a dwarf" },
        { RACE_NIGHTELF,      "night elf", "night elves",  "a night elf" },
        { RACE_UNDEAD_PLAYER, "Forsaken",  "the Forsaken", "one of the Forsaken" },
        { RACE_TAUREN,        "tauren",    "tauren",       "a tauren" },
        { RACE_GNOME,         "gnome",     "gnomes",       "a gnome" },
        { RACE_TROLL,         "troll",     "trolls",       "a troll" },
        { RACE_BLOODELF,      "blood elf", "blood elves",  "a blood elf" },
        { RACE_DRAENEI,       "draenei",   "draenei",      "a draenei" },
    };

    const RaceInfo* InfoFor(uint8_t race)
    {
        for (const RaceInfo& r : kRaces)
            if (r.id == race)
                return &r;
        return nullptr;
    }

    // A cry or a phrase is the line, or nearly: "FOR THE HORDE!!", "zug zug
    // lads", "lol zug zug" -- not a sentence that happens to mention one.
    constexpr size_t kCryExtraWords    = 3;
    constexpr size_t kPhraseExtraWords = 4;

    struct Phrase
    {
        std::vector<std::vector<std::string>> variants;   // as words: "zug zug", "zugzug"
        std::string          text;                        // as listed, first spelling: "zug zug"
        std::string          meaning;
        std::vector<uint8_t> races;
    };

    std::vector<Phrase> g_phrases;
    RaceStats           g_stats;

    std::string Trim(const std::string& s)
    {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return {};
        const size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= s.size())
        {
            size_t at = s.find(sep, start);
            if (at == std::string::npos)
                at = s.size();
            const std::string part = Trim(s.substr(start, at - start));
            if (!part.empty())
                out.push_back(part);
            start = at + 1;
        }
        return out;
    }

    // Where `phrase` starts in `words`, as a run; npos when it is not there.
    size_t Find(const std::vector<std::string>& words, const std::vector<std::string>& phrase)
    {
        if (phrase.empty() || phrase.size() > words.size())
            return std::string::npos;
        for (size_t i = 0; i + phrase.size() <= words.size(); ++i)
        {
            bool all = true;
            for (size_t j = 0; j < phrase.size() && all; ++j)
                all = words[i + j] == phrase[j];
            if (all)
                return i;
        }
        return std::string::npos;
    }

    // The longest of `phrases` in `words` that leaves at most `extra` other
    // words: its index, or -1.
    int Longest(const std::vector<std::string>& words, const std::vector<std::vector<std::string>>& phrases,
                size_t extra)
    {
        int best = -1;
        for (size_t i = 0; i < phrases.size(); ++i)
        {
            const std::vector<std::string>& p = phrases[i];
            if (p.empty() || words.size() > p.size() + extra || Find(words, p) == std::string::npos)
                continue;
            if (best < 0 || p.size() > phrases[size_t(best)].size())
                best = int(i);
        }
        return best;
    }

    std::string Joined(const std::vector<std::string>& words)
    {
        std::string out;
        for (const std::string& w : words)
            out += (out.empty() ? "" : " ") + w;
        return out;
    }

    // Up to `count` of `race`'s phrases, picked at random: "zug zug (an orc's
    // okay, ...), dabu (orcish for I obey, ...)".
    std::string SomePhrases(uint8_t race, size_t count)
    {
        std::vector<const Phrase*> mine;
        for (const Phrase& p : g_phrases)
            if (std::find(p.races.begin(), p.races.end(), race) != p.races.end())
                mine.push_back(&p);
        if (mine.empty())
            return {};
        for (size_t i = mine.size(); i > 1; --i)
            std::swap(mine[i - 1], mine[urand(0, uint32(i - 1))]);
        mine.resize(std::min(mine.size(), count));
        std::string out;
        for (const Phrase* p : mine)
            out += (out.empty() ? "" : ", ") + p->text + " (" + p->meaning + ")";
        return out;
    }

    uint32_t ChanceFor(Player* bot)
    {
        std::string style = GetBotPersonality(bot);
        for (char& c : style)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto it = g_RaceStyleChance.find(style);
        return it != g_RaceStyleChance.end() ? it->second : g_RaceUseChance;
    }
}

void Race_Load()
{
    g_phrases.clear();
    for (const auto& [race, raw] : g_RacePhrasesRaw)
    {
        for (const std::string& entry : Split(raw, '|'))
        {
            const size_t eq = entry.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string meaning = Trim(entry.substr(eq + 1));
            std::vector<std::vector<std::string>> variants;
            std::string text;
            for (const std::string& spelling : Split(entry.substr(0, eq), ','))
            {
                std::vector<std::string> words = CrowdText::Words(spelling);
                if (words.empty())
                    continue;
                if (text.empty())
                    text = spelling;
                if (std::find(variants.begin(), variants.end(), words) == variants.end())
                    variants.push_back(std::move(words));
            }
            if (variants.empty() || meaning.empty())
                continue;

            // The same words for two races ("well met"): one phrase, both races.
            auto same = std::find_if(g_phrases.begin(), g_phrases.end(), [&](const Phrase& p)
                                     { return p.variants.front() == variants.front(); });
            if (same != g_phrases.end())
            {
                if (std::find(same->races.begin(), same->races.end(), race) == same->races.end())
                    same->races.push_back(race);
                continue;
            }
            Phrase p;
            p.variants = std::move(variants);
            p.text     = text;
            p.meaning  = meaning;
            p.races.push_back(race);
            g_phrases.push_back(std::move(p));
        }
    }
    g_stats.phrases = g_phrases.size();
}

RaceLineFacts Race_ReadLine(const std::string& msg)
{
    RaceLineFacts facts;
    const std::vector<std::string> words = CrowdText::Words(Context_CleanText(msg));
    if (words.empty())
        return facts;

    // A battle cry first: "lok'tar ogar" is one, and an orc's word too.
    const int horde    = Longest(words, g_SidesCriesHorde, kCryExtraWords);
    const int alliance = Longest(words, g_SidesCriesAlliance, kCryExtraWords);
    if (horde >= 0 || alliance >= 0)
    {
        const bool isAlliance =
            horde < 0 || (alliance >= 0 && g_SidesCriesAlliance[size_t(alliance)].size() >
                                               g_SidesCriesHorde[size_t(horde)].size());
        facts.cry         = true;
        facts.cryAlliance = isAlliance;
        facts.cryText     = Joined(isAlliance ? g_SidesCriesAlliance[size_t(alliance)]
                                              : g_SidesCriesHorde[size_t(horde)]);
        return facts;
    }

    const Phrase* best = nullptr;
    size_t bestSize = 0;
    for (const Phrase& p : g_phrases)
    {
        const int at = Longest(words, p.variants, kPhraseExtraWords);
        if (at >= 0 && p.variants[size_t(at)].size() > bestSize)
        {
            best     = &p;
            bestSize = p.variants[size_t(at)].size();
        }
    }
    if (best)
    {
        facts.phrase     = true;
        facts.phraseText = best->text;
        facts.meaning    = best->meaning;
        facts.races      = best->races;
    }
    return facts;
}

bool Race_Is(Player* bot, const std::vector<uint8_t>& races)
{
    return bot && std::find(races.begin(), races.end(), uint8_t(bot->getRace())) != races.end();
}

std::string Race_HeardFor(Player* bot, Player* speaker, const std::string& msg)
{
    if (!bot || !speaker || !OllamaIsRealPlayer(speaker))
        return {};
    const RaceLineFacts facts = Race_ReadLine(msg);

    if (facts.cry)
    {
        const bool botAlliance = bot->GetTeamId() == TEAM_ALLIANCE;
        const bool own = facts.cryAlliance == botAlliance;
        const std::string& templ = own ? g_SidesOwnCryText : g_SidesEnemyCryText;
        if (templ.empty())
            return {};
        const std::string text = SafeFormat(templ, fmt::arg("player_name", speaker->GetName()),
                                            fmt::arg("cry", facts.cryText),
                                            fmt::arg("side", botAlliance ? "Alliance" : "Horde"),
                                            fmt::arg("the_side", botAlliance ? "the Alliance" : "the Horde"));
        if (text.empty() || text == "[Format Error]")
            return {};
        ++(own ? g_stats.cries : g_stats.enemyCries);
        return text;
    }

    if (!facts.phrase || g_RaceHeardText.empty())
        return {};

    // Whose it is, by name: "orcs", "humans and dwarves".
    std::vector<std::string> whose;
    for (uint8_t race : facts.races)
        if (const RaceInfo* info = InfoFor(race))
            whose.push_back(info->plural);

    std::string same;
    if (Race_Is(bot, facts.races) && !g_RaceHeardSameText.empty())
    {
        const RaceInfo* mine = InfoFor(uint8_t(bot->getRace()));
        same = SafeFormat(g_RaceHeardSameText, fmt::arg("a_race", mine ? mine->one : "one"),
                          fmt::arg("race_plural", mine ? mine->plural : "they"));
        if (same == "[Format Error]")
            same.clear();
    }
    const std::string text = SafeFormat(g_RaceHeardText, fmt::arg("player_name", speaker->GetName()),
                                        fmt::arg("phrase", facts.phraseText), fmt::arg("meaning", facts.meaning),
                                        fmt::arg("race_plural", CrowdText::JoinNames(whose)),
                                        fmt::arg("same", same.empty() ? std::string() : " " + same));
    if (text.empty() || text == "[Format Error]")
        return {};
    ++g_stats.heard;
    return text;
}

std::string Race_UseFor(Player* bot)
{
    if (!bot || g_RaceUseText.empty())
        return {};
    const uint32_t chance = ChanceFor(bot);
    if (chance == 0 || urand(1, 100) > chance)
        return {};
    const RaceInfo* info = InfoFor(uint8_t(bot->getRace()));
    const std::string phrases = info ? SomePhrases(info->id, 3) : std::string();
    if (phrases.empty())
        return {};
    const std::string text = SafeFormat(g_RaceUseText, fmt::arg("a_race", info->one),
                                        fmt::arg("race_plural", info->plural), fmt::arg("phrases", phrases));
    if (text.empty() || text == "[Format Error]")
        return {};
    ++g_stats.used;
    return text;
}

std::string Race_MingleFor(Player* bot)
{
    if (!bot || g_MingleRaceText.empty())
        return {};
    const uint32_t chance = ChanceFor(bot);
    if (chance == 0 || urand(1, 100) > chance)
        return {};
    const RaceInfo* info = InfoFor(uint8_t(bot->getRace()));
    const std::string phrases = info ? SomePhrases(info->id, 3) : std::string();
    if (phrases.empty())
        return {};
    const std::string text = SafeFormat(g_MingleRaceText, fmt::arg("name", bot->GetName()),
                                        fmt::arg("a_race", info->one), fmt::arg("race_plural", info->plural),
                                        fmt::arg("phrases", phrases));
    if (text.empty() || text == "[Format Error]")
        return {};
    ++g_stats.used;
    return " " + text;
}

RaceStats Race_GetStats()
{
    return g_stats;
}
