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

    // Every place `phrases` matches in `words`, as [start, end).
    std::vector<std::pair<size_t, size_t>> Occurrences(const std::vector<std::string>& words,
                                                       const std::vector<std::vector<std::string>>& phrases)
    {
        std::vector<std::pair<size_t, size_t>> out;
        for (const std::vector<std::string>& p : phrases)
        {
            if (p.empty())
                continue;
            for (size_t i = 0; i + p.size() <= words.size(); ++i)
                if (MatchesAt(words, i, p))
                    out.emplace_back(i, i + p.size());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // "2", "two", "a", "couple" -> 2 and so on; 0 when it is not a number.
    uint32_t NumberWord(const std::string& w)
    {
        if (!w.empty() && w.size() <= 3 &&
            std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return static_cast<uint32_t>(std::stoul(w));
        static const std::pair<const char*, uint32_t> words[] = {
            { "a", 1 }, { "an", 1 }, { "one", 1 }, { "two", 2 }, { "couple", 2 }, { "pair", 2 },
            { "three", 3 }, { "few", 3 }, { "four", 4 }, { "five", 5 }, { "six", 6 }
        };
        for (const auto& [name, n] : words)
            if (w == name)
                return n;
        return 0;
    }

    // Words that can sit between a need and the item: "need some water",
    // "out of the good water", "need 2 stacks of water".
    bool IsNeedFiller(const std::string& w)
    {
        static const char* const words[] = {
            "some", "sum", "more", "moar", "a", "an", "any", "new", "fresh", "few", "couple", "of",
            "the", "ur", "your", "stack", "stacks", "bit", "lil", "little", "proper", "decent", "good"
        };
        for (const char* x : words)
            if (w == x)
                return true;
        return NumberWord(w) != 0;
    }

    // "u got", "u have": "u" meaning "you", not playerbots' "use".
    bool IsYouVerb(const std::string& w)
    {
        static const char* const words[] = {
            "got", "have", "has", "need", "needs", "want", "wanna", "can", "could", "still", "there", "mind",
            "gonna", "able"
        };
        for (const char* x : words)
            if (w == x)
                return true;
        return false;
    }

    // Words a line of nothing but the item can have besides it: "more water",
    // "water for me", "2 stacks of water".
    bool IsBareFiller(const std::string& w)
    {
        static const char* const words[] = {
            "more", "moar", "some", "sum", "any", "again", "too", "also", "the", "a", "me", "us", "for",
            "of", "stack", "stacks", "ur", "your", "fresh", "new", "x2", "x3", "plz", "pls", "please"
        };
        for (const char* x : words)
            if (w == x)
                return true;
        return NumberWord(w) != 0;
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

    std::string StripVocatives(const std::string& text,
                               const std::vector<std::vector<std::string>>& vocatives)
    {
        if (text.empty() || vocatives.empty())
            return text;

        auto isVocative = [&](const std::string& w)
        {
            for (const std::vector<std::string>& v : vocatives)
                if (v.size() == 1 && v[0] == w)
                    return true;
            return false;
        };
        // "the kid", "a kid", "my kid", "two kids", "poor kid": the kid is
        // being talked about, not to.
        auto isDeterminer = [](const std::string& w)
        {
            static const char* const words[] = {
                "the", "a", "an", "my", "your", "ur", "yer", "his", "her", "their", "our", "its", "this",
                "that", "these", "those", "some", "any", "every", "each", "no", "one", "little", "lil",
                "young", "old", "poor", "silly", "new", "other", "another", "same", "first", "last",
                "real", "goblin", "gnome", "orc", "human", "dwarf", "elf", "troll", "tauren"
            };
            for (const char* x : words)
                if (w == x)
                    return true;
            return NumberWord(w) != 0 && w != "a" && w != "an";
        };

        struct Tok
        {
            size_t      b = 0;
            size_t      e = 0;
            std::string norm;
        };
        std::vector<Tok> toks;
        for (size_t i = 0; i < text.size();)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (!std::isalnum(c) && c != '\'')
            {
                ++i;
                continue;
            }
            Tok t;
            t.b = i;
            while (i < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '\''))
            {
                if (text[i] != '\'')
                    t.norm += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                ++i;
            }
            t.e = i;
            if (!t.norm.empty())
                toks.push_back(std::move(t));
        }

        auto hasAny = [](const std::string& s, const char* set)
        {
            return s.find_first_of(set) != std::string::npos;
        };

        // Spans of `text` to cut: the word, and the comma or space that tied it on.
        std::vector<std::pair<size_t, size_t>> cuts;
        for (size_t k = 0; k < toks.size(); ++k)
        {
            if (!isVocative(toks[k].norm))
                continue;

            const size_t prevEnd   = k > 0 ? toks[k - 1].e : 0;
            const size_t nextBegin = k + 1 < toks.size() ? toks[k + 1].b : text.size();
            const std::string before = text.substr(prevEnd, toks[k].b - prevEnd);
            const std::string after  = text.substr(toks[k].e, nextBegin - toks[k].e);

            const bool lineStart     = k == 0;
            const bool sentenceStart = lineStart || hasAny(before, ".!?\n");
            const bool commaBefore   = hasAny(before, ",;:");
            const bool lastWord      = k + 1 == toks.size();
            const bool sentenceEnd   = lastWord || hasAny(after, ".!?\n");
            const bool commaAfter    = hasAny(after, ",;:");

            if (!sentenceStart && !commaBefore && isDeterminer(toks[k - 1].norm))
                continue;   // "the kid", "two kids"
            if (!(sentenceEnd || commaAfter || sentenceStart))
                continue;   // "that kid is ...": talked about
            if (sentenceStart && !sentenceEnd && !commaAfter && k + 1 < toks.size() &&
                isDeterminer(toks[k + 1].norm) && toks[k + 1].norm != "your" && toks[k + 1].norm != "ur")
                continue;   // "kid the ..." reads oddly either way; leave it

            size_t from = toks[k].b;
            size_t to   = toks[k].e;
            if (sentenceEnd || (!sentenceStart && !commaAfter))
            {
                // "... mobs kid" / "..., kid." -> "... mobs" / "...": the
                // separator in front goes with it, the full stop stays.
                from = prevEnd;
                if (lineStart)
                    from = 0;
            }
            else
            {
                // "kid, you ..." / "listen kid, you ..." -> "you ..." / "listen, you ..."
                size_t j = to;
                if (sentenceStart)
                {
                    while (j < text.size() && (text[j] == ',' || text[j] == ';' || text[j] == ':' ||
                                               std::isspace(static_cast<unsigned char>(text[j]))))
                        ++j;
                    to = j;
                }
                else
                {
                    from = prevEnd;   // "listen kid, you" -> "listen, you"
                }
            }
            cuts.emplace_back(from, to);
        }
        if (cuts.empty())
            return text;

        std::string out;
        size_t at = 0;
        for (const auto& [from, to] : cuts)
        {
            if (from < at)
                continue;
            out += text.substr(at, from - at);
            at = to;
        }
        out += text.substr(at);

        // Tidy: no doubled spaces, no space before punctuation, no comma left
        // hanging at either end.
        std::string tidy;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const char c = out[i];
            if (c == ' ' && (tidy.empty() || tidy.back() == ' '))
                continue;
            if ((c == '.' || c == '!' || c == '?' || c == ',') && !tidy.empty() && tidy.back() == ' ')
                tidy.pop_back();
            if (c == ',' && !tidy.empty() && (tidy.back() == ',' || tidy.back() == '.' || tidy.back() == '!' ||
                                              tidy.back() == '?'))
                continue;
            tidy += c;
        }
        tidy = Trim(tidy);
        while (!tidy.empty() && (tidy.front() == ',' || tidy.front() == ';' || tidy.front() == ':'))
            tidy = Trim(tidy.substr(1));
        while (!tidy.empty() && (tidy.back() == ',' || tidy.back() == ';' || tidy.back() == ':'))
            tidy.pop_back();
        tidy = Trim(tidy);
        // "Kid." leaves only the full stop: nothing was said.
        if (std::none_of(tidy.begin(), tidy.end(), [](unsigned char c) { return std::isalnum(c) != 0; }))
            return std::string();
        return tidy;
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

    bool GlobMatch(const std::string& pattern, const std::string& text)
    {
        if (pattern.empty())
            return false;
        // Iterative, with one backtrack point for the last '*'.
        size_t p = 0, t = 0, star = std::string::npos, mark = 0;
        auto same = [](char a, char b)
        {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        };
        while (t < text.size())
        {
            if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                mark = t;
            }
            else if (p < pattern.size() && same(pattern[p], text[t]))
            {
                ++p;
                ++t;
            }
            else if (star != std::string::npos)
            {
                p = star + 1;
                t = ++mark;
            }
            else
                return false;
        }
        while (p < pattern.size() && pattern[p] == '*')
            ++p;
        return p == pattern.size();
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

        // "ok im done", "done drinking": what someone says once they have
        // had their break. The "drinking" in it is no pause -- unless the break
        // is not over ("almost done drinking", "not done drinking").
        bool resume = false;
        const std::vector<bool> resumed = Coverage(words, rules.resume, resume);
        const bool resumeCounts = resume && !NegatedOutside(words, resumed, rules.negations) &&
                                  !CrowdText::ContainsPhrase(words, rules.notYet);
        bool pauseOutside = false;
        for (size_t i = 0; i < words.size(); ++i)
            pauseOutside = pauseOutside || (covered[i] && !(resumeCounts && resumed[i]));

        if (resumeCounts && (rules.resumeMaxWords == 0 || words.size() <= rules.resumeMaxWords))
        {
            // "done drinking, lets go": the start in it is no stray word.
            std::vector<bool> filler(words.size(), false);
            for (const auto& [b, e] : Occurrences(words, rules.resumeFillers))
                for (size_t i = b; i < e; ++i)
                    filler[i] = true;
            bool startAny = false;
            const std::vector<bool> started = Coverage(words, rules.start, startAny);
            uint32_t stray = 0;
            for (size_t i = 0; i < words.size(); ++i)
                if (!resumed[i] && !filler[i] && !started[i])
                    ++stray;
            if (!pauseOutside && stray <= rules.resumeMaxStray)
            {
                // "im ready now", "good to go": a start as well, and a start
                // carries on a paused run too, where a resume alone would be
                // no use to a run that has not begun.
                if (MatchesAny(words, rules.start) && !CrowdText::ContainsPhrase(words, rules.negations))
                    return PartyIntent::Start;
                return PartyIntent::Resume;
            }
        }

        if (pause && pauseOutside && !MatchesAny(words, rules.cancel) &&
            !NegatedOutside(words, covered, rules.negations))
            return PartyIntent::Pause;

        if (MatchesAny(words, rules.start) &&
            !CrowdText::ContainsPhrase(words, rules.negations) &&
            !CrowdText::ContainsPhrase(words, rules.notYet))
            return PartyIntent::Start;

        return PartyIntent::None;
    }

    // ---- conjured food and water ---------------------------------------------

    ConjureAsk MatchConjureAsk(const std::string& cleanText, const ConjureRules& rules,
                               bool named, const std::vector<std::string>& names)
    {
        static constexpr size_t npos = std::string::npos;
        const ConjureAsk none;

        // "t water", "use water", "b water": playerbots' own commands, which
        // it acts on itself; "buy water" is vendor talk.
        const std::vector<std::string> all = CrowdText::Words(cleanText);
        if (all.empty())
            return none;
        for (const std::vector<std::string>& c : rules.commands)
        {
            if (!MatchesAt(all, 0, c))
                continue;
            // ...except "u" for "you": "u got any food", "u have water?".
            if (c.size() == 1 && c[0] == "u" && all.size() > 1 && IsYouVerb(all[1]))
                continue;
            return none;
        }

        // The bots' names are not part of what is asked for.
        std::vector<std::string> nameWords;
        for (const std::string& n : names)
            for (std::string& w : CrowdText::Words(n))
                nameWords.push_back(std::move(w));
        std::vector<std::string> words;
        for (const std::string& w : all)
            if (std::find(nameWords.begin(), nameWords.end(), w) == nameWords.end())
                words.push_back(w);
        if (words.empty() || (rules.maxWords > 0 && words.size() > rules.maxWords))
            return none;

        // What they want, leaving out what a mage cannot make: "health pots"
        // are not "pots", and "food buff" is not food.
        std::vector<bool> blocked(words.size(), false);
        for (const auto& [b, e] : Occurrences(words, rules.notMage))
            for (size_t i = b; i < e; ++i)
                blocked[i] = true;
        std::vector<bool> itemWord(words.size(), false);
        auto firstFree = [&](const std::vector<std::vector<std::string>>& phrases) -> size_t
        {
            size_t first = npos;
            for (const auto& [b, e] : Occurrences(words, phrases))
            {
                bool clear = true;
                for (size_t i = b; i < e; ++i)
                    clear = clear && !blocked[i];
                if (!clear)
                    continue;
                for (size_t i = b; i < e; ++i)
                    itemWord[i] = true;
                first = std::min(first, b);
            }
            return first;
        };
        const size_t drinkAt = firstFree(rules.drink);
        const size_t foodAt  = firstFree(rules.food);
        const size_t potAt   = firstFree(rules.pots);
        const size_t looseAt = firstFree(rules.loose);
        const bool strong = drinkAt != npos || foodAt != npos;
        if (!strong && potAt == npos && looseAt == npos)
            return none;
        const size_t firstItem = std::min({ drinkAt, foodAt, potAt, looseAt });

        // Asking for something, and asking nicely. Their words are not "had"
        // words: the "i have" inside "can i have".
        std::vector<bool> asked(words.size(), false);
        bool asks = false, polite = false;
        for (const auto& [b, e] : Occurrences(words, rules.asks))
        {
            asks = true;
            for (size_t i = b; i < e; ++i)
                asked[i] = true;
        }
        for (const auto& [b, e] : Occurrences(words, rules.polite))
        {
            polite = true;
            for (size_t i = b; i < e; ++i)
                asked[i] = true;
        }

        // Had, not wanted: "i have water", "im good on food".
        for (const auto& [b, e] : Occurrences(words, rules.haves))
        {
            bool insideAsk = true;
            for (size_t i = b; i < e; ++i)
                insideAsk = insideAsk && asked[i];
            if (!insideAsk)
                return none;
        }

        // Negated before the item: "dont need water", "no food pls". A "no" up
        // front is only an interjection when the item does not follow it at
        // once: "no worries, can i get water" asks, "no water" does not.
        for (const auto& [b, e] : Occurrences(words, rules.negations))
        {
            if (b >= firstItem)
                continue;
            if (b == 0 && IsInterjection(words[0]) && e != firstItem)
                continue;
            return none;
        }

        const bool question = cleanText.find('?') != std::string::npos;

        // A thank-you, or a question about it rather than for it.
        if (!asks && !polite && (CrowdText::ContainsPhrase(words, rules.thanks) ||
                                 CrowdText::ContainsPhrase(words, rules.questions)))
            return none;

        // A need right before a real item: "need water", "out of food", "need
        // some water". Never "need mana" or "need a drink": those are mana
        // breaks, and the run's tank answers them.
        bool needs = false;
        if (strong)
        {
            for (const auto& [b, e] : Occurrences(words, rules.needs))
            {
                (void)b;
                size_t i = e;
                for (int skipped = 0; i < words.size() && skipped < 3 && i != drinkAt && i != foodAt &&
                                      IsNeedFiller(words[i]); ++skipped)
                    ++i;
                if (i == drinkAt || i == foodAt)
                {
                    needs = true;
                    break;
                }
            }
        }

        // "mage water pls", "hey mage, food?", "water mage": the mage is spoken
        // to, not just mentioned.
        bool mage = false;
        std::vector<bool> mageWord(words.size(), false);
        for (const auto& [b, e] : Occurrences(words, rules.mage))
        {
            for (size_t i = b; i < e; ++i)
                mageWord[i] = true;
            const bool leadIn = b == 1 && (words[0] == "hey" || words[0] == "yo" || words[0] == "oi" ||
                                           words[0] == "hi" || words[0] == "ok" || words[0] == "okay" ||
                                           words[0] == "pls" || words[0] == "plz" || words[0] == "please");
            if (b == 0 || leadIn || e == words.size())
                mage = true;
        }

        // "water", "water?", "more water", "water for me": nothing but the
        // item. "drinks on me" and "pot now" are something else.
        bool bare = words.size() <= 4;
        for (size_t i = 0; bare && i < words.size(); ++i)
            bare = itemWord[i] || mageWord[i] || asked[i] || IsBareFiller(words[i]);

        const bool ask = asks || polite;
        bool ok = false;
        if (strong)
            ok = ask || needs || mage || bare || (named && question);
        else if (potAt != npos)
            ok = asks || ((named || mage) && (polite || question));
        else
            ok = (named || mage) && (ask || question);
        if (!ok)
            return none;

        ConjureAsk out;
        if (drinkAt != npos || potAt != npos || looseAt != npos)
            out.kinds |= CONJURE_DRINK;
        if (foodAt != npos)
            out.kinds |= CONJURE_FOOD;
        out.mage = mage;

        // "2 stacks of water", "a stack"; or "40 water".
        for (size_t i = 1; i < words.size(); ++i)
        {
            if (words[i] != "stack" && words[i] != "stacks" && words[i] != "stax")
                continue;
            if (const uint32_t n = NumberWord(words[i - 1]))
            {
                out.stacks = n;
                break;
            }
        }
        if (!out.stacks && firstItem != npos && firstItem > 0)
            if (const uint32_t n = NumberWord(words[firstItem - 1]); n > 1)
                out.count = n;
        return out;
    }

    // ---- telling bots what to do ---------------------------------------------

    const char* BotIntentName(BotIntent intent)
    {
        switch (intent)
        {
            case BotIntent::Stay:    return "stay";
            case BotIntent::Follow:  return "follow";
            case BotIntent::Attack:  return "attack";
            case BotIntent::Flee:    return "flee";
            case BotIntent::Revive:  return "revive";
            case BotIntent::Trade:   return "trade";
            case BotIntent::Hearth:  return "hearth";
            case BotIntent::Unstuck: return "unstuck";
            case BotIntent::Buff:    return "buff";
            case BotIntent::Pull:    return "pull";
            case BotIntent::Leave:   return "leave";
            default:                 return "none";
        }
    }

    BotCommand MatchBotCommand(const std::string& cleanText, const BotCommandRules& rules,
                               const std::vector<std::string>& names)
    {
        const BotCommand none;

        // Clauses: split at punctuation, each keeping whether it was asked.
        struct Clause
        {
            std::vector<std::string> words;
            bool asked = false;
        };
        std::vector<Clause> raw;
        {
            std::string cur;
            auto flush = [&](bool asked)
            {
                std::vector<std::string> w = CrowdText::Words(cur);
                if (!w.empty())
                    raw.push_back({ std::move(w), asked });
                else if (asked && !raw.empty())
                    raw.back().asked = true;   // "stay here ?"
                cur.clear();
            };
            for (const char c : cleanText)
            {
                if (c == ',' || c == ';' || c == '.' || c == '!' || c == '?' || c == ':' || c == '\n')
                    flush(c == '?');
                else
                    cur += c;
            }
            flush(false);
        }

        // The bots' names say who it is for, not what.
        std::vector<std::string> nameWords;
        for (const std::string& n : names)
            for (std::string& w : CrowdText::Words(n))
                nameWords.push_back(std::move(w));

        size_t total = 0;
        std::vector<Clause> clauses;
        for (Clause& c : raw)
        {
            std::vector<std::string> kept;
            for (std::string& w : c.words)
                if (std::find(nameWords.begin(), nameWords.end(), w) == nameWords.end())
                    kept.push_back(std::move(w));
            total += kept.size();

            // ...and at "and", "while", "so": "stay here while i loot".
            std::vector<std::string> part;
            for (size_t i = 0; i < kept.size(); ++i)
            {
                bool split = false;
                for (const std::vector<std::string>& s : rules.separators)
                    if (s.size() == 1 && kept[i] == s[0])
                        split = true;
                if (split)
                {
                    if (!part.empty())
                        clauses.push_back({ std::move(part), false });
                    part.clear();
                    continue;
                }
                part.push_back(kept[i]);
            }
            if (!part.empty())
                clauses.push_back({ std::move(part), c.asked });
        }
        if (clauses.empty() || (rules.maxWords > 0 && total > rules.maxWords))
            return none;

        // Anything conditional is not for now: "if it runs, kill it".
        for (const Clause& c : clauses)
            if (CrowdText::ContainsPhrase(c.words, rules.conditions))
                return none;

        // Asked nicely ("pls, wait here"), anywhere in the line; said to
        // everyone ("guys, stay here") -- outside the order's own words, since
        // "leave the group" is not said to the group.
        BotCommand out;
        bool everyoneElsewhere = false;
        for (const Clause& c : clauses)
            out.polite = out.polite || CrowdText::ContainsPhrase(c.words, rules.polite);

        for (const Clause& c : clauses)
        {
            const std::vector<std::string>& words = c.words;

            bool polite = false, everyone = false, filler = false;
            std::vector<bool> extra(words.size(), false);
            auto mark = [&](const std::vector<std::vector<std::string>>& phrases, bool& any)
            {
                for (const auto& [b, e] : Occurrences(words, phrases))
                {
                    any = true;
                    for (size_t i = b; i < e; ++i)
                        extra[i] = true;
                }
            };
            mark(rules.fillers, filler);
            mark(rules.polite, polite);
            mark(rules.everyone, everyone);

            // What is left once fillers, "can you" and "everyone" are set
            // aside: "=stay" matches "stay", "stay pls" and "everyone stay".
            std::vector<size_t> coreAt;
            std::vector<std::string> core;
            for (size_t i = 0; i < words.size(); ++i)
                if (!extra[i])
                {
                    coreAt.push_back(i);
                    core.push_back(words[i]);
                }

            auto coverage = [&](const std::vector<CommandPhrase>& list, bool& any)
            {
                std::vector<bool> cov(words.size(), false);
                any = false;
                for (const CommandPhrase& p : list)
                {
                    if (p.words.empty())
                        continue;
                    if (p.wholeLine)
                    {
                        if (core.size() == p.words.size() && MatchesAt(core, 0, p.words))
                        {
                            any = true;
                            for (size_t at : coreAt)
                                cov[at] = true;
                        }
                        continue;
                    }
                    for (size_t i = 0; i + p.words.size() <= words.size(); ++i)
                    {
                        if (!MatchesAt(words, i, p.words))
                            continue;
                        any = true;
                        for (size_t j = 0; j < p.words.size(); ++j)
                            cov[i + j] = true;
                    }
                }
                return cov;
            };

            // The intent whose phrase covers the most of the clause.
            BotIntent best = BotIntent::None;
            size_t bestN = 0;
            bool tie = false;
            std::vector<bool> bestCov;
            for (size_t i = 1; i < static_cast<size_t>(BotIntent::Count); ++i)
            {
                bool any = false;
                const std::vector<bool> cov = coverage(rules.phrases[i], any);
                if (!any)
                    continue;
                const size_t n = static_cast<size_t>(std::count(cov.begin(), cov.end(), true));
                if (n > bestN)
                {
                    best = static_cast<BotIntent>(i);
                    bestN = n;
                    bestCov = cov;
                    tie = false;
                }
                else if (n == bestN)
                    tie = true;
            }
            if (best == BotIntent::None || tie)
            {
                everyoneElsewhere = everyoneElsewhere || everyone;
                continue;
            }

            // Is any of `phrases` here, outside the order's own words?
            auto outside = [&](const std::vector<std::vector<std::string>>& phrases)
            {
                for (const auto& [b, e] : Occurrences(words, phrases))
                    for (size_t i = b; i < e; ++i)
                        if (!bestCov[i])
                            return true;
                return false;
            };

            // Asked, not told: "should we wait here?", "why are you staying".
            // "can you stay here?" is still telling, and "pull when ready" is
            // an order.
            if (!polite && (c.asked || outside(rules.questions)))
                continue;

            // "dont stay here", "no need to wait here": what not to do. A
            // negation inside the phrase is the instruction ("dont move"); a
            // "no" up front is emphasis ("no, stay here").
            if (NegatedOutside(words, bestCov, rules.negations))
                continue;

            // Who does it, when it is not the bots: the speaker ("ill wait
            // here", "gonna hearth") or someone else ("they attack"). Asking
            // for buffs or a trade is still asking ("i need buffs", "can i get
            // a buff").
            if (best != BotIntent::Buff && best != BotIntent::Trade && outside(rules.subjects))
                continue;

            uint32_t stray = 0;
            for (size_t i = 0; i < words.size(); ++i)
                if (!bestCov[i] && !extra[i] && !(i == 0 && IsInterjection(words[i])))
                    ++stray;
            if (stray > rules.maxStray)
                continue;   // talk that mentions it: "we should stay here"

            // Two clauses telling two different things: not sure, so nothing.
            if (out.intent != BotIntent::None && out.intent != best)
                return none;
            out.intent   = best;
            out.everyone = out.everyone || outside(rules.everyone);
        }
        if (out.intent == BotIntent::None)
            return none;
        out.everyone = out.everyone || everyoneElsewhere;
        return out;
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
