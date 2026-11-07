#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_crowd.h"   // CrowdText: Words, ParsePhrases, ContainsPhrase

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <sstream>

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

    std::string StripInsults(const std::string& text, const std::vector<std::vector<std::string>>& insults)
    {
        if (text.empty() || insults.empty())
            return text;

        struct Tok
        {
            size_t      b = 0;
            size_t      e = 0;
            std::string norm;
        };
        std::vector<Tok> toks;
        for (size_t i = 0; i < text.size();)
        {
            if (!std::isalnum(static_cast<unsigned char>(text[i])) && text[i] != '\'')
            {
                ++i;
                continue;
            }
            Tok t;
            t.b = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '\''))
            {
                if (text[i] != '\'')
                    t.norm += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                ++i;
            }
            t.e = i;
            if (!t.norm.empty())
                toks.push_back(std::move(t));
        }

        auto in = [](const std::string& w, std::initializer_list<const char*> list)
        {
            for (const char* x : list)
                if (w == x)
                    return true;
            return false;
        };
        auto isInsult = [&](const std::string& w) { return CrowdText::ContainsPhrase({ w }, insults); };
        auto isYou = [&](const std::string& w) { return in(w, { "you", "ya", "u", "yu", "yeh", "ye", "yah" }); };
        auto isIntensifier = [&](const std::string& w)
        {
            return in(w, { "absolute", "massive", "utter", "complete", "total", "bloody", "fucking", "fkn", "big",
                           "little", "lil", "fat", "stupid", "daft", "silly", "proper", "right", "mega", "huge",
                           "dirty", "useless" });
        };
        // "a knob", "what a knob", "the knob", "that knob": talked about.
        auto isDeterminer = [&](const std::string& w)
        {
            return in(w, { "a", "an", "the", "my", "your", "ur", "yer", "his", "her", "their", "our", "this", "that",
                           "these", "those", "such", "what", "some", "any", "no", "one", "of", "like", "is", "are",
                           "was", "be", "being", "been", "im", "hes", "shes", "theyre", "youre", "thats", "to", "for",
                           "at", "in", "on", "with", "and", "or", "by", "from", "as" });
        };
        auto hasAny = [](const std::string& s, const char* set) { return s.find_first_of(set) != std::string::npos; };

        std::vector<std::pair<size_t, size_t>> cuts;
        for (size_t k = 0; k < toks.size(); ++k)
        {
            if (!isInsult(toks[k].norm))
                continue;

            // Back over "absolute", then the "you" it was thrown with -- or
            // "youre a", "ur such a", "you are an": said to them all the same.
            size_t first = k;
            while (first > 0 && isIntensifier(toks[first - 1].norm))
                --first;
            bool you = false;
            // "thank you idiot", "see you loser": that "you" goes with the
            // verb, and stays.
            auto takesYou = [&](const std::string& w)
            {
                return in(w, { "thank", "thanks", "see", "told", "tell", "bet", "love", "hate", "miss", "got", "get",
                               "meet", "hear", "help", "like", "owe", "kill", "carry", "fuck", "screw", "ask", "asked",
                               "beat", "catch", "need", "want", "saw", "heard", "warned", "warn", "bless", "trust" });
            };
            const bool verbYou = first > 1 && isYou(toks[first - 1].norm) && takesYou(toks[first - 2].norm);
            if (first > 0 && isYou(toks[first - 1].norm) && !verbYou)
            {
                --first;
                you = true;
            }
            else
            {
                size_t j = first;
                if (j > 0 && in(toks[j - 1].norm, { "a", "an" }))
                    --j;
                if (j > 0 && toks[j - 1].norm == "such")
                    --j;
                if (j < first && j > 0)
                {
                    if (in(toks[j - 1].norm, { "youre", "ur", "ure", "yer", "your", "yur" }))
                    {
                        first = j - 1;
                        you = true;
                    }
                    else if (j > 1 && in(toks[j - 1].norm, { "are", "r", "re" }) && isYou(toks[j - 2].norm))
                    {
                        first = j - 2;
                        you = true;
                    }
                }
            }
            if (!you && first > 0 && isDeterminer(toks[first - 1].norm))
                continue;   // "dont be a knob": about someone, not at them

            const size_t prevEnd   = first > 0 ? toks[first - 1].e : 0;
            const size_t nextBegin = k + 1 < toks.size() ? toks[k + 1].b : text.size();
            const std::string before = text.substr(prevEnd, toks[first].b - prevEnd);
            const std::string after  = text.substr(toks[k].e, nextBegin - toks[k].e);
            const bool sentenceStart = first == 0 || hasAny(before, ".!?\n");
            const bool sentenceEnd   = k + 1 == toks.size() || hasAny(after, ".!?\n");
            const bool commaBefore   = hasAny(before, ",;:");
            const bool commaAfter    = hasAny(after, ",;:");

            // Thrown at someone: "you knob" (or "see you loser") anywhere; else
            // the last word, or set off by commas. "tit for tat" opens a line
            // and is not.
            if (!you && !verbYou && !sentenceEnd && !commaAfter && !(commaBefore && sentenceEnd))
                continue;

            size_t from = toks[first].b;
            size_t to   = toks[k].e;
            if (sentenceEnd || !sentenceStart)
                from = first == 0 ? 0 : prevEnd;   // "... here you knob" -> "... here"
            if (sentenceStart && !sentenceEnd)
            {
                // "you knob, i said no" -> "i said no"
                while (to < text.size() && (text[to] == ',' || text[to] == ';' || text[to] == ':' ||
                                            std::isspace(static_cast<unsigned char>(text[to]))))
                    ++to;
            }
            cuts.emplace_back(from, to);
        }
        if (cuts.empty())
            return text;

        std::string out;
        size_t at = 0;
        for (auto [from, to] : cuts)
        {
            if (to <= at)
                continue;          // inside the last cut already
            from = std::max(from, at);   // "you idiot you moron": the second runs on from the first
            out += text.substr(at, from - at);
            at = to;
        }
        out += text.substr(at);

        std::string tidy;
        for (char c : out)
        {
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
        if (std::none_of(tidy.begin(), tidy.end(), [](unsigned char c) { return std::isalnum(c) != 0; }))
            return std::string();
        return tidy;
    }

    std::string FixWeekday(const std::string& text, int today)
    {
        static const char* const kDays[] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
                                             "saturday" };
        if (today < 0 || today > 6)
            return text;

        // Words with where they are: letters, digits and apostrophes.
        struct W
        {
            size_t      begin = 0, end = 0;
            std::string key;   // lowercase, apostrophes out: "it's" is "its"
        };
        std::vector<W> words;
        for (size_t i = 0; i < text.size();)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (!std::isalnum(c) && c != '\'')
            {
                ++i;
                continue;
            }
            W w;
            w.begin = i;
            while (i < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '\''))
            {
                if (text[i] != '\'')
                    w.key += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                ++i;
            }
            w.end = i;
            if (!w.key.empty())
                words.push_back(std::move(w));
        }

        std::string out = text;
        // Back to front, so the places of the earlier ones still hold.
        for (size_t k = words.size(); k-- > 0;)
        {
            int day = -1;
            for (int d = 0; d < 7; ++d)
                if (words[k].key == kDays[d])
                    day = d;
            if (day < 0 || day == today || k == 0)
                continue;
            const std::string& p1 = words[k - 1].key;
            const std::string p2 = k >= 2 ? words[k - 2].key : std::string();
            const bool now = p1 == "its" || p1 == "happy" || p1 == "todays" ||
                             (p1 == "a" && (p2 == "for" || p2 == "on")) ||
                             (p1 == "is" && (p2 == "it" || p2 == "today"));
            if (!now)
                continue;

            // In the case it was written in: "Tuesday", "tuesday", "TUESDAY".
            const std::string said = text.substr(words[k].begin, words[k].end - words[k].begin);
            std::string name = kDays[today];
            const bool allCaps = std::none_of(said.begin(), said.end(),
                                              [](unsigned char c) { return std::islower(c); });
            if (allCaps)
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return char(std::toupper(c)); });
            else if (std::isupper(static_cast<unsigned char>(said[0])))
                name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            out.replace(words[k].begin, words[k].end - words[k].begin, name);
        }
        return out;
    }

    std::string NormalizeSourceName(const std::string& name)
    {
        std::string out;
        out.reserve(name.size());
        auto space = [&out]()
        {
            if (!out.empty() && out.back() != ' ')
                out.push_back(' ');
        };
        for (size_t i = 0; i < name.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (c == '\'')
                continue;   // "wayne's" -> "waynes"
            if (c == 0xE2 && i + 2 < name.size() && static_cast<unsigned char>(name[i + 1]) == 0x80 &&
                (static_cast<unsigned char>(name[i + 2]) == 0x98 || static_cast<unsigned char>(name[i + 2]) == 0x99))
            {
                i += 2;     // a curly apostrophe, the same
                continue;
            }
            if (c == '&')
            {
                space();
                out += "and ";
                continue;
            }
            if (std::isalnum(c))
                out.push_back(static_cast<char>(std::tolower(c)));
            else if (c >= 0x80)
                out.push_back(static_cast<char>(c));   // part of a word ("pokémon")
            else
                space();
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    QuoteSources ParseQuoteSources(const std::string& names, const std::string& filmWords)
    {
        QuoteSources out;
        size_t start = 0;
        while (start <= names.size())
        {
            size_t bar = names.find('|', start);
            if (bar == std::string::npos)
                bar = names.size();
            const std::string name = NormalizeSourceName(names.substr(start, bar - start));
            if (!name.empty())
            {
                out.names.insert(name);
                std::string joined;
                for (char c : name)
                    if (c != ' ')
                        joined.push_back(c);
                out.joined.insert(joined);
            }
            start = bar + 1;
        }
        out.filmWords = CrowdText::ParsePhrases(filmWords);
        return out;
    }

    namespace
    {
        // Is `tail` -- what follows the line -- the name of what it is from?
        // "Toy Story", "from shrek", "the movie Elf", "#lotr", "Jaws (1975)".
        bool IsQuoteSource(const std::string& tail, const QuoteSources& qs)
        {
            std::string n = NormalizeSourceName(tail);
            if (n.empty() || std::count(n.begin(), n.end(), ' ') > 7)
                return false;

            static const char* const kLeads[] = { "quote from ", "thats from ", "its from ", "from ", "as in ",
                                                  "like in ", "the movie ", "the film ", "the show ", "movie ",
                                                  "film ", "tv ", "via " };
            for (bool again = true; again;)
            {
                again = false;
                for (const char* lead : kLeads)
                {
                    const size_t len = std::strlen(lead);
                    if (n.size() > len && n.compare(0, len, lead) == 0)
                    {
                        n.erase(0, len);
                        again = true;
                    }
                }
            }

            // "toy story lol": a laugh after the name is part of the tag.
            static const char* const kLaughs[] = { " lol", " lmao", " haha", " hehe", " xd", " rofl", " jk" };
            for (bool again = true; again;)
            {
                again = false;
                for (const char* laugh : kLaughs)
                {
                    const size_t len = std::strlen(laugh);
                    if (n.size() > len && n.compare(n.size() - len, len, laugh) == 0)
                    {
                        n.erase(n.size() - len);
                        again = true;
                    }
                }
            }

            // "Jaws 1975", "toy story (1995)": the year it came out.
            const size_t lastSpace = n.rfind(' ');
            if (lastSpace != std::string::npos && n.size() - lastSpace - 1 == 4 &&
                (n.compare(lastSpace + 1, 2, "19") == 0 || n.compare(lastSpace + 1, 2, "20") == 0) &&
                std::all_of(n.begin() + lastSpace + 1, n.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
                n.erase(lastSpace);

            if (n.empty())
                return false;
            if (qs.names.count(n) || qs.names.count("the " + n))
                return true;
            if (n.size() > 4 && n.compare(0, 4, "the ") == 0 && qs.names.count(n.substr(4)))
                return true;

            std::string joined;
            for (char c : n)
                if (c != ' ')
                    joined.push_back(c);
            return qs.joined.count(joined) != 0;
        }
    }

    std::string StripQuoteSource(const std::string& text, const QuoteSources& qs, const std::string& answering)
    {
        if (qs.names.empty() || text.empty())
            return text;

        // Talking about films, naming one is the point.
        if (!qs.filmWords.empty() && !answering.empty() &&
            CrowdText::ContainsPhrase(CrowdText::Words(answering), qs.filmWords))
            return text;

        size_t b = 0, e = text.size();
        while (b < e && std::isspace(static_cast<unsigned char>(text[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])))
            --e;
        const std::string t = text.substr(b, e - b);

        // Where a source can follow the line: after its last full stop (kept),
        // or after a comma, a dash, an opening bracket, a hashtag or "from".
        struct Sep { const char* text; bool keepMark; };
        static const Sep kSeps[] = {
            { ". ", true }, { "! ", true }, { "? ", true }, { ", ", false }, { ": ", false }, { "; ", false },
            { " - ", false }, { " -- ", false }, { " \xE2\x80\x94 ", false }, { " \xE2\x80\x93 ", false },
            { "\xE2\x80\x94", false }, { " ~ ", false }, { " ~", false }, { " (", false }, { " #", false },
            { " | ", false }, { " from ", false } };

        struct Cut { size_t restEnd; size_t tailStart; };
        std::vector<Cut> cuts;
        for (const Sep& sep : kSeps)
        {
            const size_t len = std::strlen(sep.text);
            for (size_t pos = t.find(sep.text); pos != std::string::npos; pos = t.find(sep.text, pos + 1))
            {
                const size_t tailStart = pos + len;
                if (tailStart >= t.size())
                    continue;
                cuts.push_back({ sep.keepMark ? pos + 1 : pos, tailStart });
            }
        }
        // The shortest tail first: the name is the last thing on the line.
        std::sort(cuts.begin(), cuts.end(), [](const Cut& x, const Cut& y) { return x.tailStart > y.tailStart; });

        for (const Cut& cut : cuts)
        {
            if (!IsQuoteSource(t.substr(cut.tailStart), qs))
                continue;

            std::string rest = t.substr(0, cut.restEnd);
            while (!rest.empty() && (std::isspace(static_cast<unsigned char>(rest.back())) ||
                                     std::strchr(",:;-~|(#", rest.back()) != nullptr))
                rest.pop_back();
            while (rest.size() >= 3 && static_cast<unsigned char>(rest[rest.size() - 3]) == 0xE2 &&
                   static_cast<unsigned char>(rest[rest.size() - 2]) == 0x80 &&
                   (static_cast<unsigned char>(rest.back()) == 0x94 || static_cast<unsigned char>(rest.back()) == 0x93))
            {
                rest.resize(rest.size() - 3);   // a dangling dash
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
                    rest.pop_back();
            }

            const std::vector<std::string> left = CrowdText::Words(rest);
            if (left.size() < 2)
                return text;
            if (!qs.filmWords.empty() && CrowdText::ContainsPhrase(left, qs.filmWords))
                return text;
            return rest;
        }
        return text;
    }

    namespace
    {
        std::string TrimSpaces(std::string s)
        {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
                ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
                --e;
            return s.substr(b, e - b);
        }

        std::string LowerAscii(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // One emote at the start or the end of a line's words: "/wave hey",
        // "lol /laugh", "*laughs* yeah", "nice (grins)", "[emote:nod]". Taken
        // out of `text` when `isEmote` knows it, and its first word returned,
        // lowercase. An action in asterisks goes either way -- "*looks
        // around*" is narration, not chat -- but brackets and a slash are
        // left alone when the word in them is no emote: "(the one by the
        // bank)", "and/or".
        std::string TakeEmote(std::string& text, const std::function<bool(const std::string&)>& isEmote)
        {
            text = TrimSpaces(text);
            auto known = [&isEmote](const std::string& word) { return !word.empty() && isEmote(word); };
            auto wordAt = [](const std::string& s, size_t from) -> std::string
            {
                size_t end = from;
                while (end < s.size() && std::isalpha(static_cast<unsigned char>(s[end])))
                    ++end;
                return LowerAscii(s.substr(from, end - from));
            };

            // [emote:nod] anywhere.
            const size_t tag = LowerAscii(text).find("[emote:");
            if (tag != std::string::npos)
            {
                const size_t close = text.find(']', tag);
                if (close != std::string::npos)
                {
                    const std::string word = LowerAscii(TrimSpaces(text.substr(tag + 7, close - tag - 7)));
                    text = TrimSpaces(text.substr(0, tag) + " " + text.substr(close + 1));
                    return known(word) ? word : std::string();
                }
            }

            // "/wave", "/wave hey there" at the start. A slash command it is
            // not given ("/dance") is not typed into chat either: it goes.
            if (text.size() > 1 && text[0] == '/')
            {
                const std::string word = wordAt(text, 1);
                if (!word.empty())
                {
                    text = TrimSpaces(text.substr(1 + word.size()));
                    return known(word) ? word : std::string();
                }
            }

            // "*laughs*" at the start or the end; "*laughs loudly*" is its first word.
            if (!text.empty() && text[0] == '*')
            {
                const size_t end = text.find('*', 1);
                if (end != std::string::npos)
                {
                    const std::string word = wordAt(TrimSpaces(text.substr(1, end - 1)), 0);
                    text = TrimSpaces(text.substr(end + 1));
                    return known(word) ? word : std::string();
                }
            }
            if (text.size() > 2 && text.back() == '*')
            {
                const size_t start = text.rfind('*', text.size() - 2);
                if (start != std::string::npos)
                {
                    const std::string word = wordAt(TrimSpaces(text.substr(start + 1, text.size() - start - 2)), 0);
                    text = TrimSpaces(text.substr(0, start));
                    return known(word) ? word : std::string();
                }
            }

            // "(grins)" at the start or the end: only an emote.
            if (!text.empty() && text[0] == '(')
            {
                const size_t end = text.find(')', 1);
                if (end != std::string::npos)
                {
                    const std::string inside = TrimSpaces(text.substr(1, end - 1));
                    const std::string word = wordAt(inside, 0);
                    if (known(word) && inside.size() <= word.size() + 8)
                    {
                        text = TrimSpaces(text.substr(end + 1));
                        return word;
                    }
                }
            }
            if (text.size() > 2 && text.back() == ')')
            {
                const size_t start = text.rfind('(');
                if (start != std::string::npos)
                {
                    const std::string inside = TrimSpaces(text.substr(start + 1, text.size() - start - 2));
                    const std::string word = wordAt(inside, 0);
                    if (known(word) && inside.size() <= word.size() + 8)
                    {
                        text = TrimSpaces(text.substr(0, start));
                        return word;
                    }
                }
            }

            // "lol /laugh" at the end.
            const size_t slash = text.rfind(" /");
            if (slash != std::string::npos)
            {
                const std::string word = wordAt(text, slash + 2);
                if (known(word) && slash + 2 + word.size() == text.size())
                {
                    text = TrimSpaces(text.substr(0, slash));
                    return word;
                }
            }
            return {};
        }

        // "Grigohm:", "**Grigohm**:", "- Grigohm:", "[Grigohm]:", "Grigohm
        // says:" at the start of `line`: which of `names`, and where the words
        // start. False when the line is not one of theirs.
        bool SpeakerOf(const std::string& line, const std::vector<std::string>& names, size_t& who, size_t& rest)
        {
            size_t i = 0;
            auto skip = [&](const char* chars)
            {
                while (i < line.size() && (std::strchr(chars, line[i]) != nullptr ||
                                           std::isspace(static_cast<unsigned char>(line[i]))))
                    ++i;
            };
            skip("-*#>[");
            // "1." or "1)" numbering
            size_t j = i;
            while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])))
                ++j;
            if (j > i && j < line.size() && (line[j] == '.' || line[j] == ')'))
            {
                i = j + 1;
                skip("-*#>[");
            }

            const std::string lower = LowerAscii(line);
            for (size_t n = 0; n < names.size(); ++n)
            {
                const std::string name = LowerAscii(names[n]);
                if (name.empty() || lower.compare(i, name.size(), name) != 0)
                    continue;
                size_t k = i + name.size();
                if (k < line.size() && std::isalnum(static_cast<unsigned char>(line[k])))
                    continue;   // "Grigohmx": someone else
                while (k < line.size() && (line[k] == '*' || line[k] == ']' || line[k] == ' '))
                    ++k;
                if (k < line.size() && line[k] == '(')   // "Grigohm (dwarf hunter):"
                {
                    const size_t close = line.find(')', k);
                    if (close == std::string::npos)
                        continue;
                    k = close + 1;
                    while (k < line.size() && (line[k] == '*' || line[k] == ' '))
                        ++k;
                }
                if (lower.compare(k, 4, "says") == 0)
                {
                    k += 4;
                    while (k < line.size() && line[k] == ' ')
                        ++k;
                }
                while (k < line.size() && (line[k] == '*' || line[k] == ' '))
                    ++k;
                if (k < line.size() && line[k] == ':')
                {
                    ++k;
                    while (k < line.size() && line[k] == '*')   // the end of "**Grigohm:**"
                        ++k;
                    while (k < line.size() && line[k] == ' ')
                        ++k;
                    who  = n;
                    rest = k;
                    return true;
                }
            }
            return false;
        }

        // Quotation marks round the whole of what they said.
        std::string Unquote(std::string s)
        {
            s = TrimSpaces(s);
            for (bool again = true; again && s.size() >= 2;)
            {
                again = false;
                const unsigned char f = static_cast<unsigned char>(s.front());
                const unsigned char b = static_cast<unsigned char>(s.back());
                if ((f == '"' && b == '"') || (f == '\'' && b == '\'' && s.size() > 2))
                {
                    s = TrimSpaces(s.substr(1, s.size() - 2));
                    again = true;
                }
                else if (s.size() >= 6 && s.compare(0, 3, "\xE2\x80\x9C") == 0 &&
                         s.compare(s.size() - 3, 3, "\xE2\x80\x9D") == 0)
                {
                    s = TrimSpaces(s.substr(3, s.size() - 6));
                    again = true;
                }
            }
            return s;
        }
    }

    std::vector<ScriptLine> ParseScript(const std::string& text, const std::vector<std::string>& names,
                                        size_t maxLines, const std::function<bool(const std::string&)>& isEmote)
    {
        std::vector<ScriptLine> out;
        size_t start = 0;
        while (start < text.size() && out.size() < maxLines)
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            size_t who = 0, rest = 0;
            if (!SpeakerOf(line, names, who, rest))
                continue;

            std::string words = line.substr(rest);
            // The example's "<what Grigohm types>", copied rather than filled in.
            {
                const std::string w = TrimSpaces(words);
                if (w.size() >= 2 && w.front() == '<' && w.back() == '>')
                    continue;
            }
            std::string emote = TakeEmote(words, isEmote);
            words = Unquote(words);
            if (emote.empty())   // the emote after a quoted line: "\"hey\" *waves*"
                emote = TakeEmote(words, isEmote);
            words = Unquote(words);
            if (words.empty() && emote.empty())
                continue;

            ScriptLine l;
            l.speaker = who;
            l.text    = words;
            l.emote   = emote;
            out.push_back(std::move(l));
        }
        return out;
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
            case BotIntent::Grind:   return "grind";
            case BotIntent::Resume:  return "resume";
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
            if (const size_t colon = tag.find(':'); colon != npos)
            {
                uint64_t id = 0;
                size_t d = colon + 1;
                while (d < tag.size() && std::isdigit(static_cast<unsigned char>(tag[d])) && id <= 0xFFFFFFFFull)
                    id = id * 10 + uint64_t(tag[d++] - '0');
                link.id = (d > colon + 1 && id <= 0xFFFFFFFFull) ? uint32_t(id) : 0;
            }

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

    namespace
    {
        // One edit apart (substitution, insertion, deletion, or two letters
        // next to each other swapped), or the same.
        bool WithinOneSlip(const std::string& a, const std::string& b)
        {
            if (a == b)
                return true;
            const size_t n = a.size(), m = b.size();
            if (n + 1 < m || m + 1 < n)
                return false;
            if (n == m)
            {
                size_t first = 0;
                while (first < n && a[first] == b[first])
                    ++first;
                size_t last = n - 1;
                while (last > first && a[last] == b[last])
                    --last;
                if (first == last)
                    return true;   // one letter wrong
                return last == first + 1 && a[first] == b[last] && a[last] == b[first];   // swapped
            }
            const std::string& longer  = n > m ? a : b;
            const std::string& shorter = n > m ? b : a;
            size_t i = 0;
            while (i < shorter.size() && shorter[i] == longer[i])
                ++i;
            return shorter.compare(i, std::string::npos, longer, i + 1, std::string::npos) == 0;
        }
    }

    bool ClaimsOffer(const std::vector<std::string>& words, bool ask,
                     const std::vector<std::vector<std::string>>& askPhrases,
                     const std::vector<std::vector<std::string>>& yesPhrases)
    {
        static const char* const kPointers[] = { "it", "that", "those", "them", "this", "one", "these", "both", "all",
                                                 "em", "ones", "some", "the", "stuff", "everything", "lot" };
        static const char* const kQuiet[] = { "please", "pls", "plz", "now", "then", "mate", "ty", "thanks", "thx",
                                              "cheers", "ok", "okay", "pretty", "of", "too", "yes", "yeah", "sure" };
        auto listed = [](const std::string& w, const char* const* b, const char* const* e)
        {
            return std::any_of(b, e, [&](const char* x) { return w == x; });
        };
        auto inPhrases = [](const std::string& w, const std::vector<std::vector<std::string>>& phrases)
        {
            for (const std::vector<std::string>& phrase : phrases)
                if (std::find(phrase.begin(), phrase.end(), w) != phrase.end())
                    return true;
            return false;
        };
        bool pointer = ask;
        for (const std::string& w : words)
        {
            if (listed(w, std::begin(kPointers), std::end(kPointers)))
                pointer = true;
            else if (!listed(w, std::begin(kQuiet), std::end(kQuiet)) && !inPhrases(w, askPhrases) &&
                     !inPhrases(w, yesPhrases))
                return false;
        }
        return pointer;
    }

    bool IsOnlyCongrats(const std::vector<std::string>& words,
                        const std::vector<std::vector<std::string>>& congrats,
                        const std::vector<std::vector<std::string>>& filler,
                        const std::vector<std::vector<std::string>>& ack,
                        const std::function<bool(const std::string&)>& isName)
    {
        if (words.empty() || congrats.empty() || !CrowdText::ContainsPhrase(words, congrats))
            return false;
        auto is = [](const std::string& w, const std::vector<std::vector<std::string>>& list)
        {
            return !list.empty() && CrowdText::ContainsPhrase({ w }, list);
        };
        for (const std::string& w : words)
        {
            if (isName && isName(w))
                continue;
            const bool number = w.size() <= 3 &&
                                std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            if (!number && !is(w, congrats) && !is(w, filler) && !is(w, ack))
                return false;   // "gz, now lets go" is more than that
        }
        return true;
    }

    bool IsBareYes(const std::vector<std::string>& words, const std::vector<std::vector<std::string>>& yes,
                   const std::vector<std::vector<std::string>>& fillers)
    {
        if (words.empty())
            return false;
        std::vector<bool> covered(words.size(), false);
        bool any = false;
        for (const auto& [b, e] : Occurrences(words, yes))
        {
            any = true;
            for (size_t i = b; i < e; ++i)
                covered[i] = true;
        }
        if (!any)
            return false;
        for (const auto& [b, e] : Occurrences(words, fillers))
            for (size_t i = b; i < e; ++i)
                covered[i] = true;
        return std::all_of(covered.begin(), covered.end(), [](bool c) { return c; });
    }

    bool HasPhraseUnless(const std::vector<std::string>& words, const std::vector<std::vector<std::string>>& phrases,
                         const std::vector<std::vector<std::string>>& unless)
    {
        const std::vector<std::pair<size_t, size_t>> guards = Occurrences(words, unless);
        for (const auto& [b, e] : Occurrences(words, phrases))
        {
            const bool spoiled = std::any_of(guards.begin(), guards.end(), [b = b, e = e](const std::pair<size_t, size_t>& g)
            {
                const bool overlaps = g.first < e && b < g.second;
                const bool inside   = g.first >= b && g.second <= e;
                return overlaps && !inside;
            });
            if (!spoiled)
                return true;
        }
        return false;
    }

    std::string FixKeyTypos(const std::string& text, const std::vector<std::string>& keys)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            if (!std::isalpha(static_cast<unsigned char>(text[i])))
            {
                out += text[i++];
                continue;
            }
            size_t j = i;
            while (j < text.size() && std::isalpha(static_cast<unsigned char>(text[j])))
                ++j;
            const std::string word = text.substr(i, j - i);
            std::string lower = word;
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string fixed = word;
            for (const std::string& key : keys)
                if (key.size() >= 6 && lower != key && WithinOneSlip(lower, key))
                {
                    fixed = key;
                    break;
                }
            out += fixed;
            i = j;
        }
        return out;
    }

    namespace
    {
        // Where the brace opened at `open` closes, counting the braces inside;
        // npos when it does not.
        size_t MatchingBrace(const std::string& text, size_t open)
        {
            int depth = 0;
            for (size_t i = open; i < text.size(); ++i)
            {
                if (text[i] == '{')
                    ++depth;
                else if (text[i] == '}' && --depth == 0)
                    return i;
            }
            return std::string::npos;
        }
    }

    std::string ExpandChoices(const std::string& text, std::mt19937& rng)
    {
        std::string out;
        out.reserve(text.size());
        size_t from = 0;
        for (;;)
        {
            const size_t open = text.find("{~", from);
            if (open == std::string::npos)
                break;
            const size_t close = MatchingBrace(text, open);
            if (close == std::string::npos)
                break;   // unclosed: left as it is
            out.append(text, from, open - from);

            // The alternatives: split at the bars of this group, not at those
            // of a group or slot inside one.
            std::vector<std::string> options;
            int depth = 0;
            size_t at = open + 2;
            for (size_t i = at; i < close; ++i)
            {
                if (text[i] == '{')
                    ++depth;
                else if (text[i] == '}' && depth > 0)
                    --depth;
                else if (text[i] == '|' && depth == 0)
                {
                    options.push_back(text.substr(at, i - at));
                    at = i + 1;
                }
            }
            options.push_back(text.substr(at, close - at));
            const std::string& chosen = options[std::uniform_int_distribution<size_t>(0, options.size() - 1)(rng)];
            out += chosen.find("{~") == std::string::npos ? chosen : ExpandChoices(chosen, rng);
            from = close + 1;
        }
        out.append(text, from, std::string::npos);
        return out;
    }

    bool ParseSpotLine(const std::string& text, SpotLine& out)
    {
        std::istringstream in(text);
        SpotLine line;
        long long map = -1, capacity = -1;
        if (!(in >> map >> line.x >> line.y >> line.z >> line.radius >> capacity) || map < 0 || capacity < 0 ||
            !std::isfinite(line.x) || !std::isfinite(line.y) || !std::isfinite(line.z) || !std::isfinite(line.radius))
            return false;
        line.map      = static_cast<uint32_t>(map);
        line.capacity = static_cast<uint32_t>(std::min<long long>(capacity, 200));
        line.radius   = std::clamp(line.radius, 3.0f, 60.0f);
        std::string rest;
        std::getline(in, rest);
        const size_t b = rest.find_first_not_of(" \t\r\n");
        if (b != std::string::npos)
            line.name = rest.substr(b, rest.find_last_not_of(" \t\r\n") - b + 1);
        out = std::move(line);
        return true;
    }

    std::vector<std::string> SplitBars(const std::string& text)
    {
        std::vector<std::string> out;
        auto push = [&out](const std::string& part)
        {
            const size_t b = part.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)
                return;
            const size_t e = part.find_last_not_of(" \t\r\n");
            out.push_back(part.substr(b, e - b + 1));
        };
        int depth = 0;
        size_t at = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '{')
                ++depth;
            else if (text[i] == '}' && depth > 0)
                --depth;
            else if (text[i] == '|' && depth == 0)
            {
                push(text.substr(at, i - at));
                at = i + 1;
            }
        }
        push(text.substr(at));
        return out;
    }

    std::string FillSlots(const std::string& templ,
                          const std::function<std::string(const std::string&, const std::vector<std::string>&)>& pick)
    {
        std::string out;
        out.reserve(templ.size() + 32);
        std::vector<std::string> taken;
        for (size_t i = 0; i < templ.size();)
        {
            if (templ[i] != '{')
            {
                out += templ[i++];
                continue;
            }
            const size_t close = templ.find('}', i + 1);
            if (close == std::string::npos)
                return {};
            std::string slot = templ.substr(i + 1, close - i - 1);
            for (char& c : slot)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            while (!slot.empty() && std::isdigit(static_cast<unsigned char>(slot.back())))
                slot.pop_back();
            if (slot.empty() || slot.find_first_of("{ |~") != std::string::npos)
                return {};
            const std::string value = pick(slot, taken);
            if (value.empty())
                return {};
            taken.push_back(value);
            out += value;
            i = close + 1;
        }
        return out;
    }

    std::string QuestText(const std::string& text, const std::string& name, const std::string& cls,
                          const std::string& race, bool female, size_t maxLen)
    {
        auto lower = [](std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };

        std::string raw;
        raw.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c != '$' || i + 1 >= text.size())
            {
                raw += c;
                continue;
            }
            const char code = text[i + 1];
            switch (code)
            {
                case 'B': case 'b': raw += ' ';         ++i; break;
                case 'N':           raw += name;        ++i; break;
                case 'n':           raw += name;        ++i; break;
                case 'C':           raw += cls;         ++i; break;
                case 'c':           raw += lower(cls);  ++i; break;
                case 'R':           raw += race;        ++i; break;
                case 'r':           raw += lower(race); ++i; break;
                case 'G': case 'g':
                {
                    // $Glad:lass; -- the colon splits, the semicolon ends it.
                    const size_t colon = text.find(':', i + 2);
                    const size_t semi  = text.find(';', i + 2);
                    if (colon == std::string::npos || semi == std::string::npos || colon > semi)
                    {
                        ++i;   // malformed: the code goes, the rest stays
                        break;
                    }
                    std::string part = female ? text.substr(colon + 1, semi - colon - 1)
                                              : text.substr(i + 2, colon - i - 2);
                    // "$glad : lass;" -- the spaces round the colon belong to neither.
                    while (!part.empty() && part.front() == ' ')
                        part.erase(0, 1);
                    while (!part.empty() && part.back() == ' ')
                        part.pop_back();
                    raw += part;
                    i = semi;
                    break;
                }
                default:
                    raw += c;
                    break;
            }
        }

        // One space between words, none at the ends.
        std::string out;
        out.reserve(raw.size());
        for (char c : raw)
        {
            const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
            if (space)
            {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
            }
            else
                out += c;
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();

        if (maxLen == 0 || out.size() <= maxLen)
            return out;

        // A whole sentence where one ends in the second half; else up to a
        // word, with "..." to say there is more.
        std::string head = out.substr(0, maxLen);
        size_t sentence = std::string::npos;
        for (size_t i = head.size(); i > head.size() / 2; --i)
        {
            const char c = head[i - 1];
            if ((c == '.' || c == '!' || c == '?') && (i == head.size() || head[i] == ' '))
            {
                sentence = i;
                break;
            }
        }
        if (sentence != std::string::npos)
            return head.substr(0, sentence);
        const size_t space = head.find_last_of(' ');
        if (space != std::string::npos && space > head.size() / 2)
            head.erase(space);
        while (!head.empty() && (head.back() == ',' || head.back() == ';' || head.back() == ':' || head.back() == ' '))
            head.pop_back();
        return head + "...";
    }

    std::vector<Emoticon> FindEmoticons(const std::string& text, const std::vector<std::string>& known)
    {
        std::vector<Emoticon> out;
        if (known.empty())
            return out;
        auto same = [](const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        };
        for (size_t i = 0; i < text.size();)
        {
            if (text[i] == ' ')
            {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < text.size() && text[j] != ' ')
                ++j;
            std::string token = text.substr(i, j - i);
            size_t end = j;
            // "nice :)." and "lol :D," -- the punctuation after it is not part of it.
            auto isKnown = [&](const std::string& t)
            {
                return std::any_of(known.begin(), known.end(), [&](const std::string& k) { return same(k, t); });
            };
            bool hit = isKnown(token);
            if (!hit && token.size() > 2 && (token.back() == '.' || token.back() == ',' || token.back() == '!'))
            {
                const std::string trimmed = token.substr(0, token.size() - 1);
                if (isKnown(trimmed))
                {
                    hit = true;
                    token = trimmed;
                    end = j - 1;
                }
            }
            if (hit)
                out.push_back({ i, end, token });
            i = j;
        }
        return out;
    }

    std::string RemoveEmoticons(const std::string& text, const std::vector<Emoticon>& found,
                                const std::vector<size_t>& drop)
    {
        std::vector<bool> cut(text.size(), false);
        for (size_t d : drop)
            if (d < found.size())
                for (size_t i = found[d].begin; i < found[d].end && i < text.size(); ++i)
                    cut[i] = true;

        std::string kept;
        kept.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
            if (!cut[i])
                kept += text[i];

        // Fold the spaces it left, and the comma or dash it left dangling.
        std::string out;
        out.reserve(kept.size());
        for (char c : kept)
        {
            if (c == ' ' && (out.empty() || out.back() == ' '))
                continue;
            out += c;
        }
        auto edgeJunk = [](char c) { return c == ' ' || c == ',' || c == ';' || c == '-'; };
        while (!out.empty() && edgeJunk(out.back()))
            out.pop_back();
        while (!out.empty() && edgeJunk(out.front()))
            out.erase(out.begin());
        // What was either side of it: "ok, :P, see you" is "ok, see you",
        // "haha xD." is "haha.".
        for (const char* pair : { " ,", " .", " !", " ?" })
            for (size_t at = out.find(pair); at != std::string::npos; at = out.find(pair))
                out.erase(at, 1);
        for (size_t at = out.find(",,"); at != std::string::npos; at = out.find(",,"))
            out.erase(at, 1);
        for (size_t at = out.find(",."); at != std::string::npos; at = out.find(",."))
            out.erase(at, 1);

        const bool word = std::any_of(out.begin(), out.end(),
                                      [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
        return word ? out : std::string();
    }

    std::vector<std::string> ContentGrams(const std::string& text, size_t n, const std::vector<std::string>& common)
    {
        std::vector<std::string> out;
        if (n == 0)
            return out;
        // Runs of words between names: "[Look to the Stars]" ends one run and
        // starts the next, and none of its words are in either.
        std::vector<std::vector<std::string>> runs(1);
        std::string cur;
        bool inName = false;
        auto flush = [&]()
        {
            if (!cur.empty())
                runs.back().push_back(std::move(cur));
            cur.clear();
        };
        for (char c : text)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (c == '[' && !inName)
            {
                flush();
                inName = true;
                if (!runs.back().empty())
                    runs.emplace_back();
            }
            else if (c == ']' && inName)
                inName = false;
            else if (inName)
                continue;
            else if (std::isalnum(u))
                cur += static_cast<char>(std::tolower(u));
            else if (c == '\'')
                continue;   // "don't" is "dont"
            else
                flush();
        }
        flush();

        auto isCommon = [&](const std::string& w)
        {
            return std::find(common.begin(), common.end(), w) != common.end();
        };
        for (const std::vector<std::string>& words : runs)
        {
            for (size_t i = 0; i + n <= words.size(); ++i)
            {
                bool content = false;
                std::string gram;
                for (size_t j = i; j < i + n; ++j)
                {
                    content = content || !isCommon(words[j]);
                    if (j > i)
                        gram += ' ';
                    gram += words[j];
                }
                if (content && std::find(out.begin(), out.end(), gram) == out.end())
                    out.push_back(std::move(gram));
            }
        }
        return out;
    }

    // ---- prices --------------------------------------------------------------

    std::string WithoutBracketNames(const std::string& text)
    {
        const std::vector<BracketName> names = FindBracketNames(text);
        if (names.empty())
            return text;
        std::string out;
        size_t at = 0;
        for (const BracketName& n : names)
        {
            out.append(text, at, n.begin - at);
            out += ' ';
            at = n.end;
        }
        out.append(text, at, std::string::npos);
        return out;
    }

    uint32_t ParsePrice(const std::string& text)
    {
        std::string s;
        s.reserve(text.size());
        for (char c : text)
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
        auto isAlpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
        auto coin = [](const std::string& unit) -> uint64_t
        {
            static const char* const gold[]   = { "g", "gold", "golds", "gp" };
            static const char* const silver[] = { "s", "silver", "silvers", "sil", "sp" };
            static const char* const copper[] = { "c", "copper", "coppers", "cp" };
            for (const char* u : gold)
                if (unit == u)
                    return 10000;
            for (const char* u : silver)
                if (unit == u)
                    return 100;
            for (const char* u : copper)
                if (unit == u)
                    return 1;
            return 0;
        };

        const size_t n = s.size();
        double total = 0.0;
        bool   inPrice = false;
        size_t i = 0;
        while (i < n)
        {
            // A number that starts a word ("x5" and "lvl20" are no prices),
            // or one that runs on from the coin before it ("1g50s").
            const bool starts = isDigit(s[i]) &&
                                (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])) ||
                                 (inPrice && isAlpha(s[i - 1])));
            if (!starts)
            {
                if (inPrice && s[i] != ' ' && s[i] != '+' && s[i] != ',')
                    break;   // the price is over: "5g each, 50s for two" is 5g
                if (inPrice && s[i] == ',' && i + 1 < n && s[i + 1] != ' ')
                    break;
                ++i;
                continue;
            }

            // "1,500g": a comma before exactly three digits only groups them.
            auto groups = [&](size_t at)
            {
                return s[at] == ',' && at + 3 < n && isDigit(s[at + 1]) && isDigit(s[at + 2]) &&
                       isDigit(s[at + 3]) && (at + 4 >= n || !isDigit(s[at + 4]));
            };
            size_t j = i;
            double value = 0.0;
            while (j < n && (isDigit(s[j]) || groups(j)))
            {
                if (s[j] != ',')
                    value = value * 10.0 + (s[j] - '0');
                ++j;
            }
            if (j + 1 < n && s[j] == '.' && isDigit(s[j + 1]))
            {
                double scale = 0.1;
                ++j;
                while (j < n && isDigit(s[j]))
                {
                    value += (s[j] - '0') * scale;
                    scale /= 10.0;
                    ++j;
                }
            }

            size_t k = j;
            if (k < n && s[k] == ' ')
                ++k;
            size_t e = k;
            while (e < n && isAlpha(s[e]))
                ++e;
            const uint64_t mult = coin(s.substr(k, e - k));
            if (mult)
            {
                total += value * double(mult);
                inPrice = true;
                i = e;
                continue;
            }
            if (inPrice)
                break;   // "5g and 20 more": the price was 5g
            i = j;
        }

        if (total <= 0.0)
            return 0;
        constexpr double kCap = 2147483647.0;   // the gold cap, in copper
        return total >= kCap ? uint32_t(kCap) : uint32_t(total + 0.5);
    }

    std::string MoneyWords(uint32_t copper)
    {
        const uint32_t gold = copper / 10000, silver = (copper / 100) % 100, rest = copper % 100;
        std::ostringstream out;
        if (gold)
        {
            out << gold << 'g';
            if (silver)
                out << ' ' << silver << 's';
        }
        else if (silver)
        {
            out << silver << 's';
            if (rest)
                out << ' ' << rest << 'c';
        }
        else
            out << rest << 'c';
        return out.str();
    }
}
