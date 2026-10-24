#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_crowd.h"   // CrowdText: Words, ParsePhrases, ContainsPhrase

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>

namespace
{
    bool StartsWith(const std::string& word, const char* prefix)
    {
        const size_t n = std::strlen(prefix);
        return word.size() >= n && word.compare(0, n, prefix) == 0;
    }

    bool IsDingWord(const std::string& w)
    {
        // ding, dinged, dinging, dingding, dinggg -- but not dingy or dinghy.
        return w == "ding" || w == "dinged" || w == "dinging" ||
               StartsWith(w, "dingd") || StartsWith(w, "dingg");
    }

    // Words that make a ding line a congratulation rather than an announcement.
    bool IsCongratsWord(const std::string& w)
    {
        return w == "gz" || StartsWith(w, "gzz") || StartsWith(w, "grat") || w == "grz" ||
               StartsWith(w, "congrat") || StartsWith(w, "conga") || w == "gj" || w == "gg" ||
               w == "wp" || StartsWith(w, "nice") || w == "sweet" || w == "well" || w == "woot" ||
               w == "noice" || w == "grats";
    }

    // "ur ding", "that ding", "the ding": somebody else's.
    bool IsOtherPersonWord(const std::string& w)
    {
        return w == "ur" || w == "your" || w == "youre" || w == "the" || w == "that" ||
               w == "his" || w == "her" || w == "their" || w == "this" || w == "on";
    }

    std::string Trim(const std::string& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return s.substr(b, e - b);
    }

    bool MatchesAny(const std::vector<std::string>& words,
                    const std::vector<TextRules::CommandPhrase>& list)
    {
        for (const TextRules::CommandPhrase& p : list)
        {
            if (p.words.empty())
                continue;
            if (p.wholeLine && words.size() != p.words.size())
                continue;
            if (CrowdText::ContainsPhrase(words, { p.words }))
                return true;
        }
        return false;
    }

    // Does `phrase` match `words` starting exactly at `at`?
    bool MatchesAt(const std::vector<std::string>& words, size_t at, const std::vector<std::string>& phrase)
    {
        if (phrase.empty() || at + phrase.size() > words.size())
            return false;
        const std::vector<std::string> slice(words.begin() + static_cast<std::ptrdiff_t>(at),
                                             words.begin() + static_cast<std::ptrdiff_t>(at + phrase.size()));
        return CrowdText::ContainsPhrase(slice, { phrase });
    }

    // Is the whole of `words` exactly one entry of `list`?
    bool IsExactly(const std::vector<std::string>& words, const std::vector<TextRules::CommandPhrase>& list)
    {
        for (const TextRules::CommandPhrase& p : list)
            if (!p.words.empty() && p.words.size() == words.size() && MatchesAt(words, 0, p.words))
                return true;
        return false;
    }

    // Which words belong to a matching entry of `list`. `any` says whether one
    // matched at all.
    std::vector<bool> Coverage(const std::vector<std::string>& words,
                               const std::vector<TextRules::CommandPhrase>& list, bool& any)
    {
        std::vector<bool> covered(words.size(), false);
        any = false;
        for (const TextRules::CommandPhrase& p : list)
        {
            if (p.words.empty())
                continue;
            if (p.wholeLine)
            {
                if (words.size() == p.words.size() && MatchesAt(words, 0, p.words))
                {
                    any = true;
                    std::fill(covered.begin(), covered.end(), true);
                }
                continue;
            }
            for (size_t i = 0; i + p.words.size() <= words.size(); ++i)
            {
                if (!MatchesAt(words, i, p.words))
                    continue;
                any = true;
                for (size_t j = 0; j < p.words.size(); ++j)
                    covered[i + j] = true;
            }
        }
        return covered;
    }

    // "no", "nah" in front of an instruction: "no wait", "nah hold on".
    bool IsInterjection(const std::string& w)
    {
        return w == "no" || w == "nope" || w == "nah" || w == "naw" || w == "nono" || StartsWith(w, "noo");
    }

    // A negation that is not part of the instruction itself ("dont pull") and
    // not a "no" up front for emphasis ("no wait", "no no stop").
    bool NegatedOutside(const std::vector<std::string>& words, const std::vector<bool>& covered,
                        const std::vector<std::vector<std::string>>& negations)
    {
        size_t lead = 0;
        while (lead + 1 < words.size() && IsInterjection(words[lead]))
            ++lead;

        for (const std::vector<std::string>& neg : negations)
        {
            if (neg.empty())
                continue;
            for (size_t i = 0; i + neg.size() <= words.size(); ++i)
            {
                if (!MatchesAt(words, i, neg))
                    continue;
                for (size_t j = i; j < i + neg.size(); ++j)
                    if (j >= lead && !covered[j])
                        return true;
            }
        }
        return false;
    }

    // Words that only lead into a ready check ("ok", "are we", "everyone") and
    // that may trail one ("ready to go?", "rdy now?", "ready guys?").
    bool IsReadyLeadIn(const std::string& w)
    {
        static const char* const words[] = {
            "ok", "okay", "okk", "k", "kk", "alright", "aight", "right", "so",
            "are", "is", "we", "everyone", "everybody", "every1", "evry1", "all", "yall",
            "guys", "ppl", "people", "group", "team"
        };
        for (const char* x : words)
            if (w == x)
                return true;
        return false;
    }

    bool IsReadyTail(const std::string& w)
    {
        static const char* const words[] = {
            "now", "then", "to", "go", "guys", "all", "everyone", "everybody", "yall", "ppl",
            "people", "team", "folks"
        };
        for (const char* x : words)
            if (w == x)
                return true;
        return false;
    }

    // "r?", "ready?", "everyone ready?", "are we ready", "ok lets go": the
    // line, less those lead-ins and tails, is exactly one start phrase.
    bool IsReadyCheck(const std::vector<std::string>& words, const std::vector<TextRules::CommandPhrase>& start)
    {
        const size_t n = words.size();
        size_t maxLead = 0;
        while (maxLead < n && IsReadyLeadIn(words[maxLead]))
            ++maxLead;
        size_t maxTail = 0;
        while (maxTail < n && IsReadyTail(words[n - 1 - maxTail]))
            ++maxTail;

        for (size_t b = 0; b <= maxLead; ++b)
        {
            for (size_t t = 0; t <= maxTail; ++t)
            {
                if (b + t >= n)
                    continue;
                const std::vector<std::string> core(words.begin() + static_cast<std::ptrdiff_t>(b),
                                                    words.end() - static_cast<std::ptrdiff_t>(t));
                if (IsExactly(core, start))
                    return true;
            }
        }
        return false;
    }

    // "you oom", "anyone need mana", "are you ready". "you guys wait" and "u
    // all ready" are said to the group, not asked.
    bool StartsWithOpener(const std::vector<std::string>& words,
                          const std::vector<std::vector<std::string>>& openers)
    {
        for (const std::vector<std::string>& o : openers)
        {
            if (!MatchesAt(words, 0, o))
                continue;
            const bool you = o.size() == 1 && (o[0] == "you" || o[0] == "u" || o[0] == "ya");
            if (you && words.size() > 1 && (words[1] == "guys" || words[1] == "all"))
                continue;
            return true;
        }
        return false;
    }
}

namespace TextRules
{
    bool IsSelfDing(const std::string& text)
    {
        const std::vector<std::string> words = CrowdText::Words(text);

        bool ding = false;
        for (size_t i = 0; i < words.size(); ++i)
        {
            if (!IsDingWord(words[i]))
                continue;
            if (i > 0 && IsOtherPersonWord(words[i - 1]))
                return false;   // "gz on the ding", "nice, ur ding"
            ding = true;
        }
        if (!ding)
            return false;

        for (const std::string& w : words)
            if (IsCongratsWord(w))
                return false;   // "nice ding", "ding? gz"

        return true;
    }

    std::string StripAssistantSpeak(const std::string& text,
                                    const std::vector<std::vector<std::string>>& phrases)
    {
        if (phrases.empty() || text.empty())
            return text;

        // Split into sentences, each keeping the punctuation that ends it.
        std::vector<std::string> sentences;
        std::string cur;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            cur += c;
            if (c == '.' || c == '!' || c == '?' || c == '\n')
            {
                while (i + 1 < text.size() &&
                       (text[i + 1] == '.' || text[i + 1] == '!' || text[i + 1] == '?'))
                    cur += text[++i];
                sentences.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty())
            sentences.push_back(cur);

        std::string out;
        bool removed = false;
        for (const std::string& s : sentences)
        {
            if (CrowdText::ContainsPhrase(CrowdText::Words(s), phrases))
            {
                removed = true;
                continue;
            }
            const std::string t = Trim(s);
            if (t.empty())
                continue;
            if (!out.empty())
                out += ' ';
            out += t;
        }

        return removed ? Trim(out) : text;
    }

    std::vector<CommandPhrase> ParseCommandPhrases(const std::string& raw)
    {
        std::vector<CommandPhrase> out;

        size_t start = 0;
        while (start <= raw.size())
        {
            size_t bar = raw.find('|', start);
            if (bar == std::string::npos)
                bar = raw.size();

            std::string entry = Trim(raw.substr(start, bar - start));
            bool whole = false;
            if (!entry.empty() && entry.front() == '=')
            {
                whole = true;
                entry = Trim(entry.substr(1));
            }

            for (std::vector<std::string>& words : CrowdText::ParsePhrases(entry))
                if (!words.empty())
                    out.push_back({ std::move(words), whole });

            start = bar + 1;
        }
        return out;
    }

    bool IsDungeonClearCommand(const std::string& cleanText)
    {
        const std::vector<std::string> words = CrowdText::Words(cleanText);
        return !words.empty() &&
               (words[0] == "dc" || (words.size() >= 2 && words[0] == "dungeon" && words[1] == "clear"));
    }

    PartyIntent MatchPartyIntent(const std::string& cleanText, const PartyRules& rules)
    {
        const std::vector<std::string> words = CrowdText::Words(cleanText);
        if (words.empty() || (rules.maxWords > 0 && words.size() > rules.maxWords))
            return PartyIntent::None;

        // mod-dungeon-clear's own chat keywords.
        if (IsDungeonClearCommand(cleanText))
            return PartyIntent::None;

        // Whoever leads asking the group: "r?", "everyone ready?".
        if (IsReadyCheck(words, rules.start))
            return PartyIntent::Start;

        // Anything else asked is talk, not an instruction: "you oom?", "need
        // mana?", "wait what", "anyone need a drink", "are you ready".
        if (cleanText.find('?') != std::string::npos ||
            CrowdText::ContainsPhrase(words, rules.questions) ||
            StartsWithOpener(words, rules.openers))
            return PartyIntent::None;

        bool pause = false;
        const std::vector<bool> covered = Coverage(words, rules.pause, pause);
        if (pause && !MatchesAny(words, rules.cancel) && !NegatedOutside(words, covered, rules.negations))
            return PartyIntent::Pause;

        if (MatchesAny(words, rules.start) &&
            !CrowdText::ContainsPhrase(words, rules.negations) &&
            !CrowdText::ContainsPhrase(words, rules.notYet))
            return PartyIntent::Start;

        return PartyIntent::None;
    }

    // ---- level outlook ----------------------------------------------------

    bool ParseLevelRange(const std::string& raw, uint32_t& lo, uint32_t& hi)
    {
        const std::string r = Trim(raw);
        size_t i = 0;
        auto number = [&](uint32_t& out) -> bool
        {
            const size_t from = i;
            uint64_t v = 0;
            while (i < r.size() && std::isdigit(static_cast<unsigned char>(r[i])))
            {
                v = v * 10 + static_cast<uint64_t>(r[i] - '0');
                if (v > 1000)
                    return false;
                ++i;
            }
            out = static_cast<uint32_t>(v);
            return i > from;
        };
        auto skipSpace = [&]()
        {
            while (i < r.size() && std::isspace(static_cast<unsigned char>(r[i])))
                ++i;
        };

        uint32_t a = 0;
        if (!number(a))
            return false;
        skipSpace();
        if (i == r.size())
        {
            lo = hi = a;
            return true;
        }
        if (r[i] == '+')
        {
            ++i;
            skipSpace();
            if (i != r.size())
                return false;
            lo = a;
            hi = 1000;
            return true;
        }
        if (r[i] != '-')
            return false;
        ++i;
        skipSpace();
        uint32_t b = 0;
        if (!number(b))
            return false;
        skipSpace();
        if (i != r.size() || b < a)
            return false;
        lo = a;
        hi = b;
        return true;
    }

    std::vector<LevelBand> ParseLevelBands(const std::string& raw)
    {
        std::vector<LevelBand> out;
        size_t start = 0;
        while (start <= raw.size())
        {
            size_t bar = raw.find('|', start);
            if (bar == std::string::npos)
                bar = raw.size();
            const std::string entry = raw.substr(start, bar - start);
            start = bar + 1;

            const size_t colon = entry.find(':');
            if (colon == std::string::npos)
                continue;
            LevelBand band;
            if (!ParseLevelRange(entry.substr(0, colon), band.lo, band.hi))
                continue;
            band.text = Trim(entry.substr(colon + 1));
            if (!band.text.empty())
                out.push_back(std::move(band));
        }
        return out;
    }

    const LevelBand* FindLevelBand(const std::vector<LevelBand>& bands, uint32_t level)
    {
        for (const LevelBand& band : bands)
            if (level >= band.lo && level <= band.hi)
                return &band;
        return nullptr;
    }

    bool PremiseFits(std::string& premise, uint32_t level)
    {
        const std::string p = Trim(premise);
        if (p.empty() || p[0] != '[')
            return true;

        const size_t close = p.find(']');
        if (close == std::string::npos)
            return true;

        uint32_t lo = 0, hi = 0;
        if (!ParseLevelRange(p.substr(1, close - 1), lo, hi))
            return true;   // "[something]" that is not a level tag: part of the premise

        premise = Trim(p.substr(close + 1));
        return level >= lo && level <= hi;
    }

    std::vector<LevelPhrase> ParseLevelPhrases(const std::string& raw)
    {
        std::vector<LevelPhrase> out;
        size_t start = 0;
        while (start <= raw.size())
        {
            size_t bar = raw.find('|', start);
            if (bar == std::string::npos)
                bar = raw.size();
            const std::string entry = Trim(raw.substr(start, bar - start));
            start = bar + 1;

            const size_t close = entry.find(']');
            if (entry.empty() || entry[0] != '[' || close == std::string::npos)
                continue;
            LevelPhrase phrase;
            if (!ParseLevelRange(entry.substr(1, close - 1), phrase.lo, phrase.hi))
                continue;
            // As every other phrase list: lowercase words, '*' at the end of
            // one matching any word that starts with the rest.
            std::vector<std::vector<std::string>> parsed = CrowdText::ParsePhrases(entry.substr(close + 1));
            if (parsed.empty() || parsed.front().empty())
                continue;
            phrase.words = std::move(parsed.front());
            out.push_back(std::move(phrase));
        }
        return out;
    }

    const LevelPhrase* FindOffLevelPhrase(const std::string& text, const std::vector<LevelPhrase>& phrases,
                                          uint32_t level, const std::string& exempt)
    {
        if (phrases.empty() || text.empty())
            return nullptr;
        const std::vector<std::string> words = CrowdText::Words(text);
        const std::vector<std::string> echo  = CrowdText::Words(exempt);
        for (const LevelPhrase& p : phrases)
        {
            if (level >= p.lo && level <= p.hi)
                continue;
            if (!CrowdText::ContainsPhrase(words, { p.words }) || CrowdText::ContainsPhrase(echo, { p.words }))
                continue;
            // The same phrase may be listed for more than one range: any of
            // them allowing this level is enough.
            bool allowed = false;
            for (const LevelPhrase& q : phrases)
                if (q.words == p.words && level >= q.lo && level <= q.hi)
                    allowed = true;
            if (!allowed)
                return &p;
        }
        return nullptr;
    }

    // ---- stock phrases -----------------------------------------------------

    std::vector<size_t> FindPhraseIndexes(const std::string& text,
                                          const std::vector<std::vector<std::string>>& phrases,
                                          const std::string& exempt)
    {
        std::vector<size_t> found;
        if (phrases.empty() || text.empty())
            return found;
        const std::vector<std::string> words = CrowdText::Words(text);
        const std::vector<std::string> echo  = CrowdText::Words(exempt);
        for (size_t i = 0; i < phrases.size(); ++i)
            if (!phrases[i].empty() && CrowdText::ContainsPhrase(words, { phrases[i] }) &&
                !CrowdText::ContainsPhrase(echo, { phrases[i] }))
                found.push_back(i);
        return found;
    }

    std::string PhraseKey(const std::vector<std::string>& phrase)
    {
        std::string key;
        for (const std::string& w : phrase)
        {
            if (!key.empty())
                key += ' ';
            key += w;
        }
        return key;
    }

    // ---- chat links ---------------------------------------------------------

    std::vector<ChatLink> ExtractChatLinks(const std::string& message)
    {
        std::vector<ChatLink> out;
        const size_t npos = std::string::npos;
        size_t colourAt = npos;   // where a |cAARRGGBB right in front of the next |H began
        for (size_t i = 0; i < message.size(); )
        {
            if (message[i] != '|' || i + 1 >= message.size())
            {
                ++i;
                colourAt = npos;
                continue;
            }

            const char code = message[i + 1];
            if (code == '|')
            {
                i += 2;                // "||" is a pipe someone typed, not markup
                colourAt = npos;
                continue;
            }
            if (code == 'c' || code == 'C')
            {
                size_t hex = 0;
                while (hex < 8 && i + 2 + hex < message.size() &&
                       std::isxdigit(static_cast<unsigned char>(message[i + 2 + hex])))
                    ++hex;
                colourAt = (hex == 8) ? i : npos;
                i += 2 + hex;
                continue;
            }
            if (code != 'H')
            {
                i += 2;
                colourAt = npos;
                continue;
            }

            // |Htype:data|h[Name]|h, with no '|' in the data or the name.
            const size_t dataEnd = message.find('|', i + 2);
            if (dataEnd == npos || message.compare(dataEnd, 3, "|h[") != 0)
            {
                i += 2;
                colourAt = npos;
                continue;
            }
            const size_t nameEnd = message.find('|', dataEnd + 3);
            if (nameEnd == npos || nameEnd < dataEnd + 5 || message[nameEnd - 1] != ']' ||
                message.compare(nameEnd, 2, "|h") != 0)
            {
                i = dataEnd;           // carry on from the bad part
                colourAt = npos;
                continue;
            }

            ChatLink link;
            const std::string tag = message.substr(i + 2, dataEnd - i - 2);
            link.type = tag.substr(0, tag.find(':'));
            link.name = message.substr(dataEnd + 3, nameEnd - 1 - (dataEnd + 3));

            const size_t begin = (colourAt != npos && colourAt + 10 == i) ? colourAt : i;
            size_t end = nameEnd + 2;
            if (message.compare(end, 2, "|r") == 0)
                end += 2;
            link.markup = message.substr(begin, end - begin);

            if (!link.type.empty() && !link.name.empty())
                out.push_back(std::move(link));
            i = end;
            colourAt = npos;
        }
        return out;
    }

    std::string StripChatMarkup(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); )
        {
            const char ch = text[i];
            if (ch != '|')
            {
                out += ch;
                ++i;
                continue;
            }
            if (i + 1 >= text.size())
            {
                ++i;   // a lone '|' at the end
                continue;
            }

            const char code = text[i + 1];
            if (code == 'c' || code == 'C')
            {
                i += 2;
                for (size_t n = 0; n < 8 && i < text.size() &&
                                   std::isxdigit(static_cast<unsigned char>(text[i])); ++n)
                    ++i;
                continue;
            }
            if (code == 'H')
            {
                // |Htype:data|h -- the tag and its data go; the "[Name]" after
                // it is ordinary text, and its closing |h goes below.
                const size_t h = text.find("|h", i + 2);
                i = (h == std::string::npos) ? text.size() : h + 2;
                continue;
            }
            if (code == 'T')
            {
                const size_t t = text.find("|t", i + 2);
                i = (t == std::string::npos) ? text.size() : t + 2;
                continue;
            }
            // |r, |h, |t, |n and "||" show nothing; any other '|' goes on its
            // own, so the character after it stays. No '|' is ever kept.
            i += (code == 'r' || code == 'h' || code == 't' || code == 'n' || code == '|') ? 2 : 1;
        }
        return out;
    }

    std::vector<BracketName> FindBracketNames(const std::string& text)
    {
        std::vector<BracketName> out;
        for (size_t at = 0; (at = text.find('[', at)) != std::string::npos; )
        {
            const size_t close = text.find(']', at + 1);
            if (close == std::string::npos)
                break;
            const size_t nested = text.find('[', at + 1);
            if (nested != std::string::npos && nested < close)
            {
                at = nested;           // "[[Name]]" or "[a [b]": start again from the inner one
                continue;
            }

            const std::string inner = Trim(text.substr(at + 1, close - at - 1));
            bool letter = false;
            for (char c : inner)
                if (std::isalpha(static_cast<unsigned char>(c)))
                    letter = true;
            if (letter && inner.size() <= 80)
                out.push_back({ at, close + 1, inner });
            at = close + 1;
        }
        return out;
    }

    std::string NameKey(const std::string& name)
    {
        std::string key;
        key.reserve(name.size());
        bool space = false;
        for (char c : name)
        {
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                space = !key.empty();
                continue;
            }
            if (space)
            {
                key += ' ';
                space = false;
            }
            key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return key;
    }

    std::string FormatCannedLine(const std::string& line, const std::string& name, std::mt19937& rng)
    {
        static const std::string token = "{name}";
        const size_t at = line.find(token);
        if (at == std::string::npos)
            return line;

        std::string who = name;
        if (std::uniform_int_distribution<int>(0, 1)(rng) == 0)
            for (char& c : who)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::string out = line;
        out.replace(at, token.size(), who);
        return out;
    }
}
