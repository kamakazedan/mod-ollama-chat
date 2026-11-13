#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_crowd.h"   // CrowdText: Words, ParsePhrases, ContainsPhrase

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <sstream>
#include <tuple>

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

    // Words between whoever needs something and the need: "i really need",
    // "still need", "i could really use".
    bool IsNeedAdverb(const std::string& w)
    {
        static const char* const words[] = {
            "really", "rly", "realy", "still", "just", "so", "also", "def", "defo", "actually", "kinda", "sorta",
            "seriously", "srsly", "could", "would", "do"
        };
        for (const char* x : words)
            if (w == x)
                return true;
        return false;
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

    std::vector<PhraseSwap> ParseSwaps(const std::string& raw)
    {
        auto trim = [](std::string s)
        {
            const size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos)
                return std::string();
            return s.substr(b, s.find_last_not_of(" \t") - b + 1);
        };
        std::vector<PhraseSwap> out;
        std::stringstream entries(raw);
        std::string entry;
        while (std::getline(entries, entry, '|'))
        {
            const size_t eq = entry.find('=');
            if (eq == std::string::npos)
                continue;
            PhraseSwap swap;
            swap.from = CrowdText::Words(entry.substr(0, eq));
            std::stringstream news(entry.substr(eq + 1));
            std::string one;
            while (std::getline(news, one, ','))
                if (std::string t = trim(one); !t.empty())
                    swap.to.push_back(std::move(t));
            if (!swap.from.empty() && !swap.to.empty())
                out.push_back(std::move(swap));
        }
        return out;
    }

    std::string SwapPhrases(const std::string& text, const std::vector<PhraseSwap>& swaps,
                            const std::function<size_t(size_t)>& pick)
    {
        if (text.empty() || swaps.empty())
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
            for (; i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '\''); ++i)
                if (text[i] != '\'')
                    t.norm += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
            t.e = i;
            if (!t.norm.empty())
                toks.push_back(std::move(t));
        }

        // What makes the phrase a thing talked about: a word for it before it
        // ("a", "thats a", "very"), a word that carries on from it after it
        // ("about the gear", "there").
        static const std::unordered_set<std::string> kBefore = {
            "a", "an", "the", "that", "thats", "this", "these", "those", "its", "your", "ur", "yer", "my", "his",
            "her", "their", "our", "one", "another", "some", "any", "no", "every", "each", "such", "very",
            "really", "pretty", "quite", "so", "too", "most", "more", "real", "good", "great", "totally", "fairly"
        };
        static const std::unordered_set<std::string> kAfter = {
            "about", "on", "regarding", "re", "there", "here", "made", "well", "with", "from", "in", "of", "for",
            "you", "u", "ya", "youve", "uve", "to", "is", "was", "and"
        };
        // Only blanks between two of its words, so "fair. point" is not it.
        auto blanks = [&](size_t from, size_t to)
        {
            for (size_t i = from; i < to; ++i)
                if (text[i] != ' ' && text[i] != '\t')
                    return false;
            return true;
        };

        std::string out;
        size_t copied = 0;
        for (size_t k = 0; k < toks.size(); ++k)
        {
            for (const PhraseSwap& swap : swaps)
            {
                const size_t n = swap.from.size();
                if (k + n > toks.size())
                    continue;
                bool same = true;
                for (size_t i = 0; i < n && same; ++i)
                    same = toks[k + i].norm == swap.from[i] && (i == 0 || blanks(toks[k + i - 1].e, toks[k + i].b));
                if (!same)
                    continue;
                if (k > 0 && blanks(toks[k - 1].e, toks[k].b) && kBefore.contains(toks[k - 1].norm))
                    continue;
                if (k + n < toks.size() && blanks(toks[k + n - 1].e, toks[k + n].b) &&
                    kAfter.contains(toks[k + n].norm))
                    continue;

                const std::string said = text.substr(toks[k].b, toks[k + n - 1].e - toks[k].b);
                std::string now = swap.to[swap.to.size() > 1 ? std::min(pick(swap.to.size()), swap.to.size() - 1) : 0];
                const bool allCaps = said.size() > 1 && std::none_of(said.begin(), said.end(), [](unsigned char c)
                                                                     { return std::islower(c); });
                if (allCaps)
                    std::transform(now.begin(), now.end(), now.begin(),
                                   [](unsigned char c) { return char(std::toupper(c)); });
                else if (std::isupper(static_cast<unsigned char>(said[0])))
                    now[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(now[0])));

                out += text.substr(copied, toks[k].b - copied) + now;
                copied = toks[k + n - 1].e;
                k += n - 1;
                break;
            }
        }
        return copied ? out + text.substr(copied) : text;
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
            if (qs.names.contains(n) || qs.names.contains("the " + n))
                return true;
            if (n.size() > 4 && n.compare(0, 4, "the ") == 0 && qs.names.contains(n.substr(4)))
                return true;

            std::string joined;
            for (char c : n)
                if (c != ' ')
                    joined.push_back(c);
            return qs.joined.contains(joined);
        }
    }

    std::string StripQuoteSource(const std::string& text, const QuoteSources& sources, const std::string& answering)
    {
        if (sources.names.empty() || text.empty())
            return text;

        // Talking about films, naming one is the point.
        if (!sources.filmWords.empty() && !answering.empty() &&
            CrowdText::ContainsPhrase(CrowdText::Words(answering), sources.filmWords))
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
            if (!IsQuoteSource(t.substr(cut.tailStart), sources))
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
            if (!sources.filmWords.empty() && CrowdText::ContainsPhrase(left, sources.filmWords))
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

    std::string SqueezeLetters(const std::string& word)
    {
        std::string out;
        out.reserve(word.size());
        for (char c : word)
            if (out.empty() || c != out.back() || !std::isalpha(static_cast<unsigned char>(c)))
                out += c;
        return out;
    }

    namespace
    {
        // A phrase word with its letter runs cut, unless it has a '*': "gogo*"
        // cut to "go*" would take "good" for a go.
        std::string SqueezePhraseWord(const std::string& w)
        {
            return w.find('*') == std::string::npos ? SqueezeLetters(w) : w;
        }

        std::vector<std::vector<std::string>> SqueezePhrases(const std::vector<std::vector<std::string>>& list)
        {
            std::vector<std::vector<std::string>> out = list;
            for (std::vector<std::string>& phrase : out)
                for (std::string& w : phrase)
                    w = SqueezePhraseWord(w);
            return out;
        }

        std::vector<CommandPhrase> SqueezePhrases(const std::vector<CommandPhrase>& list)
        {
            std::vector<CommandPhrase> out = list;
            for (CommandPhrase& phrase : out)
                for (std::string& w : phrase.words)
                    w = SqueezePhraseWord(w);
            return out;
        }

        PartyRules SqueezeRules(const PartyRules& rules)
        {
            PartyRules out = rules;
            out.start         = SqueezePhrases(rules.start);
            out.pause         = SqueezePhrases(rules.pause);
            out.resume        = SqueezePhrases(rules.resume);
            out.cancel        = SqueezePhrases(rules.cancel);
            out.negations     = SqueezePhrases(rules.negations);
            out.questions     = SqueezePhrases(rules.questions);
            out.openers       = SqueezePhrases(rules.openers);
            out.notYet        = SqueezePhrases(rules.notYet);
            out.resumeFillers = SqueezePhrases(rules.resumeFillers);
            return out;
        }

        PartyIntent MatchPartyWords(const std::vector<std::string>& words, const std::string& cleanText,
                                    const PartyRules& rules);
    }

    PartyIntent MatchPartyIntent(const std::string& cleanText, const PartyRules& rules)
    {
        const std::vector<std::string> words = CrowdText::Words(cleanText);
        const PartyIntent intent = MatchPartyWords(words, cleanText, rules);
        if (intent != PartyIntent::None || words.empty() || (rules.maxWords > 0 && words.size() > rules.maxWords))
            return intent;

        // "letsgoooo", "leeggoooo": typed long. Only worth a second look when
        // a word has a run in it at all.
        std::vector<std::string> squeezed;
        bool changed = false;
        for (const std::string& w : words)
        {
            squeezed.push_back(SqueezeLetters(w));
            changed = changed || squeezed.back() != w;
        }
        if (!changed)
            return intent;
        return MatchPartyWords(squeezed, cleanText, SqueezeRules(rules));
    }

    namespace
    {
    PartyIntent MatchPartyWords(const std::vector<std::string>& words, const std::string& cleanText,
                                const PartyRules& rules)
    {
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
    }

    // ---- skipping a boss -------------------------------------------------------

    namespace
    {
        // How far apart `a` and `b` are -- a letter wrong, missing, extra or two
        // swapped each counting one -- or `cap + 1` once it is further than that.
        size_t Slips(const std::string& a, const std::string& b, size_t cap)
        {
            const size_t n = a.size(), m = b.size();
            if ((n > m ? n - m : m - n) > cap)
                return cap + 1;
            std::vector<std::vector<size_t>> d(n + 1, std::vector<size_t>(m + 1, 0));
            for (size_t i = 0; i <= n; ++i)
                d[i][0] = i;
            for (size_t j = 0; j <= m; ++j)
                d[0][j] = j;
            for (size_t i = 1; i <= n; ++i)
                for (size_t j = 1; j <= m; ++j)
                {
                    const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                    d[i][j] = std::min({ d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost });
                    if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                        d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
                }
            return std::min(d[n][m], cap + 1);
        }

        // A word of a boss's name, to read what someone typed against.
        struct BossNameKey
        {
            std::string word;
            std::string squeezed;
            size_t      boss = 0;
        };

        // Every word of every name but its titles ("Lord", "the"), and each
        // name of more than one word run together ("mrsmite").
        std::vector<BossNameKey> BossNameKeys(const std::vector<std::string>& bossNames,
                                      const std::vector<std::vector<std::string>>& titles)
        {
            std::vector<BossNameKey> keys;
            for (size_t i = 0; i < bossNames.size(); ++i)
            {
                const std::vector<std::string> words = CrowdText::Words(bossNames[i]);
                std::vector<std::string> picked;
                for (const std::string& w : words)
                    if (w.size() >= 3 && !CrowdText::ContainsPhrase({ w }, titles))
                        picked.push_back(w);
                if (picked.empty())   // nothing but titles: still somebody
                    for (const std::string& w : words)
                        if (w.size() >= 3)
                            picked.push_back(w);
                if (words.size() > 1)
                {
                    std::string whole;
                    for (const std::string& w : words)
                        whole += w;
                    picked.push_back(whole);
                }
                for (const std::string& w : picked)
                    keys.push_back({ w, SqueezeLetters(w), i });
            }
            return keys;
        }

        // How well a typed word reads as `key`: 4 it is the word, 3 typed long
        // or short ("oglefint"), 2 the start of it ("taraga"), 1 a slip off --
        // two in a long name -- and 0 not at all.
        int NameMatch(const std::string& typed, const BossNameKey& key)
        {
            if (typed.size() < 3)
                return 0;
            if (typed == key.word)
                return 4;
            const std::string squeezed = SqueezeLetters(typed);
            if (squeezed == key.squeezed)
                return 3;
            if (typed.size() >= 4 && typed.size() < key.word.size() && typed.size() * 2 >= key.word.size() &&
                key.word.compare(0, typed.size(), typed) == 0)
                return 2;
            const size_t allowed = key.squeezed.size() >= 8 ? 2 : (key.squeezed.size() >= 5 ? 1 : 0);
            if (allowed && Slips(squeezed, key.squeezed, allowed) <= allowed)
                return 1;
            return 0;
        }

        // The bosses a typed word names best; none when it names nobody.
        std::vector<size_t> BossesNamed(const std::string& typed, const std::vector<BossNameKey>& keys)
        {
            int best = 0;
            std::vector<size_t> out;
            for (const BossNameKey& key : keys)
            {
                const int m = NameMatch(typed, key);
                if (m == 0 || m < best)
                    continue;
                if (m > best)
                {
                    best = m;
                    out.clear();
                }
                if (std::find(out.begin(), out.end(), key.boss) == out.end())
                    out.push_back(key.boss);
            }
            return out;
        }

        // The longest of `phrases` that matches at `at`, in words; 0 for none.
        size_t LongestAt(const std::vector<std::string>& words, size_t at,
                         const std::vector<std::vector<std::string>>& phrases)
        {
            size_t best = 0;
            for (const std::vector<std::string>& p : phrases)
                if (p.size() > best && MatchesAt(words, at, p))
                    best = p.size();
            return best;
        }

        // The sentences of a line: up to each ?, ! or ;, and whether it ended
        // with a ?.
        std::vector<std::pair<std::string, bool>> Sentences(const std::string& text)
        {
            std::vector<std::pair<std::string, bool>> out;
            std::string cur;
            for (char c : text)
            {
                if (c == '?' || c == '!' || c == ';')
                {
                    out.emplace_back(cur, c == '?');
                    cur.clear();
                }
                else
                    cur += c;
            }
            out.emplace_back(cur, false);
            return out;
        }

        TextRules::SkipAsk MatchSkipSentence(const std::vector<std::string>& words, bool asked,
                                             const TextRules::SkipRules& rules, const std::vector<BossNameKey>& keys)
        {
            using TextRules::SkipKind;
            TextRules::SkipAsk ask;

            // What is asked, and where: the first of each kind, taken in this
            // order so "dont skip" is no skip and "skip to" no plain one.
            struct Kind
            {
                const std::vector<std::vector<std::string>>* phrases;
                SkipKind kind;
                bool     named;   // only with a boss's name
            };
            const Kind kinds[] = {
                { &rules.unskip, SkipKind::Unskip, false }, { &rules.skipTo, SkipKind::SkipTo, false },
                { &rules.skip, SkipKind::Skip, false },     { &rules.skipNamed, SkipKind::Skip, true },
                { &rules.doIt, SkipKind::Do, true },
            };
            size_t start = 0, length = 0;
            const Kind* found = nullptr;
            for (const Kind& k : kinds)
            {
                for (size_t i = 0; i < words.size() && !found; ++i)
                    if (const size_t len = LongestAt(words, i, *k.phrases))
                    {
                        found  = &k;
                        start  = i;
                        length = len;
                    }
                if (found)
                    break;
            }
            if (!found)
                return ask;

            // "why did we skip him", "is he worth skipping", "are we skipping
            // him?": talk about it, not an ask.
            if (CrowdText::ContainsPhrase(words, rules.questions))
                return ask;
            const std::string& verb = words[start + length - 1];
            if (asked && verb.size() > 3 && verb.compare(verb.size() - 3, 3, "ing") == 0)
                return ask;

            // What it is about: the words after it, up to another ask or a
            // stop ("then", "because").
            size_t end = start + length;
            while (end < words.size() && !LongestAt(words, end, rules.stops))
            {
                bool another = false;
                for (const Kind& k : kinds)
                    another = another || LongestAt(words, end, *k.phrases) > 0;
                if (another)
                    break;
                ++end;
            }

            // "we cant skip him", "skip him or not" -- but "no, skip him", and
            // the "dont" of "dont kill" is the ask itself.
            size_t lead = 0;
            while (lead + 1 < words.size() && IsInterjection(words[lead]))
                ++lead;
            for (size_t i = lead; i < end; ++i)
                if ((i < start || i >= start + length) && LongestAt(words, i, rules.negations))
                    return ask;

            size_t stray = 0;
            bool pointed = false;       // "the next boss", "next one": the boss the run heads for...
            bool pronoun = false;       // ..."him", "it", "this one": or one just named
            bool saysBoss = false;      // ...said to be a boss
            for (size_t i = start + length; i < end;)
            {
                if (LongestAt(words, i, rules.notBoss))
                    return ask;   // "skip this pull", "skip the trash"
                if (NumberWord(words[i]) >= 2)
                    return ask;   // "skip the next 2": which ones is anyone's guess
                // "skip the boss after oggleflint": which one is anyone's guess.
                if ((words[i] == "after" || words[i] == "before") && i + 1 < end &&
                    (!BossesNamed(words[i + 1], keys).empty() || LongestAt(words, i + 1, rules.nextBoss) ||
                     LongestAt(words, i + 1, rules.nextAny) || LongestAt(words, i + 1, rules.last)))
                    return ask;

                // The longest of them says what it is: "this time" is no "this".
                const size_t lenLast = LongestAt(words, i, rules.last);
                const size_t lenBoss = LongestAt(words, i, rules.nextBoss);
                const size_t lenAny  = LongestAt(words, i, rules.nextAny);
                const size_t lenFill = LongestAt(words, i, rules.fillers);
                if (const size_t longest = std::max({ lenLast, lenBoss, lenAny, lenFill }))
                {
                    if (lenLast == longest)
                        ask.last = true;
                    else if (lenBoss == longest || lenAny == longest)
                    {
                        bool next = false;
                        for (size_t j = i; j < i + longest; ++j)
                            next = next || words[j] == "next" || words[j] == "first" || words[j] == "current";
                        (next ? pointed : pronoun) = true;
                        saysBoss = saysBoss || lenBoss == longest;
                    }
                    i += longest;
                    continue;
                }

                // A name: this word, or it and the next run together ("van cleef").
                std::vector<size_t> named = BossesNamed(words[i], keys);
                size_t used = 1;
                if (named.empty() && i + 1 < end)
                {
                    named = BossesNamed(words[i] + words[i + 1], keys);
                    used  = 2;
                }
                if (named.empty())
                {
                    ++stray;
                    ++i;
                    continue;
                }
                i += used;
                // "skip oggleflint, taragaman next": the second is where to go,
                // and "skip oggleflint first" no skip of the next boss.
                if (i < end && (words[i] == "next" || words[i] == "first"))
                {
                    ++stray;
                    break;
                }
                for (size_t b : named)
                    if (std::find(ask.bosses.begin(), ask.bosses.end(), b) == ask.bosses.end())
                        ask.bosses.push_back(b);
            }

            // "skip bazzalan, he is annoying": the "he" is the one named.
            const bool named = !ask.bosses.empty();
            ask.next         = pointed || (pronoun && !named);
            ask.nextSaysBoss = ask.next && saysBoss;
            bool ok = false;
            switch (found->kind)
            {
                case SkipKind::Skip:
                    if (found->named)
                        ok = named || ask.last;
                    else
                    {
                        ok = named || ask.next || ask.last || stray == 0;
                        if (ok && !named && !ask.last)
                            ask.next = true;   // a bare "skip": what the run heads for
                    }
                    break;
                case SkipKind::SkipTo:
                    ok = named || ask.last;
                    if (named)
                        ask.bosses.resize(1);   // where to skip to: the first one named
                    ask.next = ask.nextSaysBoss = false;
                    break;
                case SkipKind::Unskip:
                    ok = named || ask.next || stray == 0;
                    if (ok && !named)
                        ask.next = true;   // "dont skip", "unskip him": the one just skipped
                    ask.last = false;
                    break;
                case SkipKind::Do:
                    ok = named;
                    ask.next = ask.nextSaysBoss = ask.last = false;
                    break;
                default:
                    break;
            }
            if (!ok)
                return TextRules::SkipAsk();
            ask.kind = found->kind;
            return ask;
        }
    }

    SkipAsk MatchSkipAsk(const std::string& cleanText, const SkipRules& rules,
                         const std::vector<std::string>& bossNames)
    {
        const std::vector<std::string> all = CrowdText::Words(cleanText);
        if (all.empty() || (rules.maxWords > 0 && all.size() > rules.maxWords) || IsDungeonClearCommand(cleanText))
            return SkipAsk();

        const std::vector<BossNameKey> keys = BossNameKeys(bossNames, rules.titles);
        for (const auto& [sentence, asked] : Sentences(cleanText))
        {
            const std::vector<std::string> words = CrowdText::Words(sentence);
            if (words.empty())
                continue;
            const SkipAsk ask = MatchSkipSentence(words, asked, rules, keys);
            if (ask.kind != SkipKind::None)
                return ask;
        }
        return SkipAsk();
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

    // ---- buffs, on request ---------------------------------------------------

    BuffAsk MatchBuffAsk(const std::string& cleanText, const BuffRules& rules, bool named,
                         const std::vector<std::string>& names)
    {
        static constexpr size_t npos = std::string::npos;
        BuffAsk none;   // not const: returned, it is moved, not copied

        // "cast fort", "c Power Word: Fortitude": playerbots' own command.
        const std::vector<std::string> all = CrowdText::Words(cleanText);
        if (all.empty())
            return none;
        for (const std::vector<std::string>& c : rules.commands)
            if (MatchesAt(all, 0, c))
                return none;

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

        // No spell anyone casts on you: "food buff", "flask", "world buffs";
        // "mark skull" is a raid mark.
        if (!Occurrences(words, rules.notBuffs).empty())
            return none;

        // What they want.
        std::vector<bool> buffWord(words.size(), false);
        size_t firstBuff = npos;
        auto mark = [&](const std::vector<std::vector<std::string>>& phrases) -> bool
        {
            bool any = false;
            for (const auto& [b, e] : Occurrences(words, phrases))
            {
                any = true;
                for (size_t i = b; i < e; ++i)
                    buffWord[i] = true;
                firstBuff = std::min(firstBuff, b);
            }
            return any;
        };
        uint16_t strongKinds = BUFF_NONE, shortKinds = BUFF_NONE;
        for (const BuffRules::Kind& k : rules.kinds)
        {
            if (mark(k.words))
                strongKinds |= k.kind;
            if (mark(k.shortWords))
                shortKinds |= k.kind;
        }
        const bool generic = mark(rules.buffWords);
        const bool strong  = generic || strongKinds;
        if (!strong && !shortKinds)
            return none;

        // Who they asked: "priest can i get a buff", "any pallys?".
        std::vector<bool> classWord(words.size(), false);
        size_t firstClass = npos;
        const BuffRules::Class* asked = nullptr;
        for (const BuffRules::Class& c : rules.classes)
            for (const auto& [b, e] : Occurrences(words, c.words))
            {
                for (size_t i = b; i < e; ++i)
                    classWord[i] = true;
                if (b < firstClass)
                {
                    firstClass = b;
                    asked = &c;
                }
            }

        // Offering, not asking: "anyone need fort?", "who wants buffs", "im
        // buffing", and "buffs anyone?" with "anyone" at the end.
        if (!Occurrences(words, rules.offers).empty())
            return none;
        if (words.size() > 1 && (words.back() == "anyone" || words.back() == "anybody" || words.back() == "any1"))
            return none;

        // Asking for it, and asking nicely.
        std::vector<bool> askWord(words.size(), false);
        bool asks = false, polite = false;
        for (const auto& [b, e] : Occurrences(words, rules.asks))
        {
            asks = true;
            for (size_t i = b; i < e; ++i)
                askWord[i] = true;
        }
        for (const auto& [b, e] : Occurrences(words, rules.polite))
        {
            polite = true;
            for (size_t i = b; i < e; ++i)
                askWord[i] = true;
        }

        // Had: "i have fort", "im buffed" -- but not the "i have" in "can i
        // have" or the "got" in "anyone got".
        for (const auto& [b, e] : Occurrences(words, rules.haves))
        {
            bool inAsk = false;
            for (size_t i = b; i < e; ++i)
                inAsk = inAsk || askWord[i];
            if (!inAsk)
                return none;
        }

        // Negated before the buff: "dont need buffs", "no buffs". A "no" up
        // front is an interjection when the buff does not follow it at once:
        // "no worries, can i get a buff".
        for (const auto& [b, e] : Occurrences(words, rules.negations))
        {
            if (b >= firstBuff)
                continue;
            if (b == 0 && IsInterjection(words[0]) && e != firstBuff)
                continue;
            return none;
        }

        const bool question = cleanText.find('?') != npos;

        // A thank-you, or a question about it rather than for it.
        if (!asks && !polite && (CrowdText::ContainsPhrase(words, rules.thanks) ||
                                 CrowdText::ContainsPhrase(words, rules.questions)))
            return none;

        // Their own need, right before the buff: "need a buff", "i could use
        // fort", "really need some int". Never "warriors need a buff", which
        // is about the game.
        bool needs = false;
        for (const auto& [b, e] : Occurrences(words, rules.needs))
        {
            // Whose need: back over "really", "still" and the like to the
            // start of the line, "i" or "we", or an ask.
            size_t j = b;
            while (j > 0 && IsNeedAdverb(words[j - 1]))
                --j;
            const bool theirs = j == 0 || words[j - 1] == "i" || words[j - 1] == "we" || askWord[j - 1];
            if (!theirs)
                continue;
            size_t i = e;
            for (int skipped = 0; i < words.size() && skipped < 3 && !buffWord[i] && IsNeedFiller(words[i]); ++skipped)
                ++i;
            if (i < words.size() && buffWord[i])
            {
                needs = true;
                break;
            }
        }

        // "anyone", "someone" up front: "anyone fort?", "someone buff me";
        // "any mages? need int".
        const bool openUp = words[0] == "anyone" || words[0] == "anybody" || words[0] == "any1" ||
                            words[0] == "someone" || words[0] == "somebody" || words[0] == "sum1" ||
                            (words[0] == "any" && words.size() > 1 && classWord[1]);

        // "buffs?", "any buffs", "fort pls", "kings for me": nothing but it.
        bool bare = words.size() <= 4;
        for (size_t i = 0; bare && i < words.size(); ++i)
            bare = buffWord[i] || classWord[i] || askWord[i] || IsBareFiller(words[i]);

        const bool ask = asks || polite || needs;
        bool ok = false;
        if (strong)
            ok = ask || bare || openUp || (question && (named || words.size() <= 6));
        else
            ok = words.size() <= 6 && (ask || bare || openUp || named);
        if (!ok)
            return none;

        BuffAsk out;
        out.ask   = true;
        out.kinds = strongKinds | shortKinds;
        if (asked)
        {
            out.askedClass = asked->name;
            if (out.kinds == BUFF_NONE)
                out.kinds = asked->kinds;   // "priest buff pls": a priest's
        }
        return out;
    }

    bool IsThanksLine(const std::string& cleanText, const std::vector<std::vector<std::string>>& thanks,
                      const std::vector<std::vector<std::string>>& negations,
                      const std::vector<std::string>& names, uint32_t maxWords)
    {
        if (cleanText.find('?') != std::string::npos)
            return false;
        std::vector<std::string> nameWords;
        for (const std::string& n : names)
            for (std::string& w : CrowdText::Words(n))
                nameWords.push_back(std::move(w));
        std::vector<std::string> words;
        for (std::string& w : CrowdText::Words(cleanText))
            if (std::find(nameWords.begin(), nameWords.end(), w) == nameWords.end())
                words.push_back(std::move(w));
        if (words.empty() || (maxWords > 0 && words.size() > maxWords))
            return false;

        const std::vector<std::pair<size_t, size_t>> said = Occurrences(words, thanks);
        if (said.empty())
            return false;
        // "no thanks", "nah ty": turning something down.
        for (const auto& [b, e] : Occurrences(words, negations))
            if (e == said.front().first)
                return false;
        // "thanks for nothing".
        for (size_t i = said.front().second; i < words.size(); ++i)
            if (words[i] == "nothing" || words[i] == "nothin")
                return false;
        return true;
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

    PremiseTags TakeLevelAndSide(std::string& premise, uint32_t level, const std::string& side)
    {
        PremiseTags fit;
        std::string p = Trim(premise);
        bool levelSeen = false, sideSeen = false;
        while (!p.empty() && p[0] == '[')
        {
            const size_t close = p.find(']');
            if (close == std::string::npos)
                break;
            const std::string inner = Trim(p.substr(1, close - 1));
            uint32_t lo = 0, hi = 0;
            if (!levelSeen && ParseLevelRange(inner, lo, hi))
            {
                levelSeen     = true;
                fit.levelFits = level >= lo && level <= hi;
            }
            else if (!sideSeen && (inner == "horde" || inner == "alliance"))
            {
                sideSeen     = true;
                fit.sideFits = side.empty() || side == inner;
            }
            else
                break;   // "[Hogger] ...", or a tag for someone else to read: part of the premise
            p = Trim(p.substr(close + 1));
        }
        premise = p;
        return fit;
    }

    bool PremiseFits(std::string& premise, uint32_t level, const std::string& side)
    {
        const PremiseTags fit = TakeLevelAndSide(premise, level, side);
        return fit.levelFits && fit.sideFits;
    }

    std::string SideTag(bool alliance)
    {
        return alliance ? "alliance" : "horde";
    }

    std::vector<std::string> TakePremiseTags(std::string& premise)
    {
        std::vector<std::string> tags;
        std::string levelTag;
        std::string p = Trim(premise);
        while (!p.empty() && p[0] == '[')
        {
            const size_t close = p.find(']');
            if (close == std::string::npos)
                break;
            const std::string inner = Trim(p.substr(1, close - 1));
            uint32_t lo = 0, hi = 0;
            if (levelTag.empty() && ParseLevelRange(inner, lo, hi))
                levelTag = p.substr(0, close + 1);
            else if (!inner.empty() && std::all_of(inner.begin(), inner.end(),
                                                   [](unsigned char c) { return c >= 'a' && c <= 'z'; }))
                tags.push_back(inner);
            else
                break;   // "[Hogger] ..." -- part of the premise
            p = Trim(p.substr(close + 1));
        }
        premise = levelTag.empty() ? p : levelTag + " " + p;
        return tags;
    }

    std::string WithArticle(const std::string& noun)
    {
        if (noun.empty())
            return noun;
        const char c = char(std::tolower(static_cast<unsigned char>(noun[0])));
        return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' ? "an " : "a ") + noun;
    }

    std::string AtPlace(const std::string& place)
    {
        std::string trimmed = Trim(place);
        if (trimmed.empty())
            return trimmed;
        static const char* const kLeads[] = { "outside", "inside", "just", "by", "near", "at", "on", "in", "under",
                                              "beside", "behind", "around", "atop", "above", "below", "beneath",
                                              "up", "down", "across", "along", "between", "among", "off" };
        const size_t space = trimmed.find(' ');
        std::string first = trimmed.substr(0, space);
        for (char& c : first)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        for (const char* lead : kLeads)
            if (space != std::string::npos && first == lead)
                return trimmed;
        return "in " + trimmed;
    }

    std::string MountFromSpell(const std::string& spellName)
    {
        std::string name = Trim(spellName);
        if (name.rfind("Summon ", 0) == 0)
            name = Trim(name.substr(7));
        return name;
    }

    namespace
    {
        // Words and two-word phrases swapped for others, matched whole and
        // whatever their case; the rest of the line is left as it is. A
        // capitalised word after the first is a name ("gz Ty") and is kept.
        std::string SwapWords(const std::string& line, const std::vector<std::pair<std::string, std::string>>& swaps)
        {
            // Words (letters, digits, ' and /) and what is between them.
            std::vector<std::string> parts;
            std::vector<bool> isWord;
            for (size_t i = 0; i < line.size();)
            {
                auto wordy = [&](char c)
                { return std::isalnum(static_cast<unsigned char>(c)) || c == '\'' || c == '/'; };
                const bool w = wordy(line[i]);
                size_t j = i;
                while (j < line.size() && wordy(line[j]) == w)
                    ++j;
                parts.push_back(line.substr(i, j - i));
                isWord.push_back(w);
                i = j;
            }
            std::string out;
            bool first = true;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                bool done = false;
                const bool name = isWord[i] && !first && std::isupper(static_cast<unsigned char>(parts[i][0]));
                if (isWord[i])
                    first = false;
                if (isWord[i] && !name)
                    for (const auto& [from, to] : swaps)
                    {
                        const size_t space = from.find(' ');
                        if (space == std::string::npos)
                        {
                            if (LowerAscii(parts[i]) == from)
                            {
                                out += to;
                                done = true;
                                break;
                            }
                        }
                        else if (i + 2 < parts.size() && parts[i + 1] == " " && isWord[i + 2] &&
                                 !std::isupper(static_cast<unsigned char>(parts[i + 2][0])) &&
                                 LowerAscii(parts[i]) == from.substr(0, space) &&
                                 LowerAscii(parts[i + 2]) == from.substr(space + 1))
                        {
                            out += to;
                            i += 2;
                            done = true;
                            break;
                        }
                    }
                if (!done)
                    out += parts[i];
            }
            return out;
        }

        std::string TrimEnd(std::string s, const char* chars)
        {
            while (!s.empty() && std::strchr(chars, s.back()))
                s.pop_back();
            return s;
        }

        // The line ends on a word, not on ":)", "o/" or punctuation: a full
        // stop or an exclamation mark can go after it.
        bool EndsOnWord(const std::string& s)
        {
            const size_t start = s.find_last_of(' ') == std::string::npos ? 0 : s.find_last_of(' ') + 1;
            if (start >= s.size())
                return false;
            for (size_t i = start; i < s.size(); ++i)
                if (!std::isalnum(static_cast<unsigned char>(s[i])) && s[i] != '\'')
                    return false;
            return true;
        }
    }

    std::string StyleCanned(const std::string& line, const std::string& style)
    {
        if (line.empty() || style.empty())
            return line;
        std::string out = line;
        if (style == "proper")
        {
            static const std::vector<std::pair<std::string, std::string>> kProper = {
                { "gg wp", "good game, well played" }, { "gg ty", "good game, thank you" },
                { "ty gg", "thank you, good game" },   { "gg thanks", "good game, thanks" },
                { "hey hey", "hello there" },
                { "ty ty", "thank you" },  { "thank u", "thank you" }, { "gzz", "congratulations" },
                { "gz", "congratulations" }, { "grats", "congratulations" }, { "gratz", "congratulations" },
                { "congratz", "congratulations" }, { "congrats", "congratulations" }, { "tyty", "thank you" },
                { "ty", "thank you" },    { "thx", "thanks" },         { "gg", "good game" },
                { "gj", "good job" },     { "wp", "well played" },     { "cya", "see you" },
                { "laters", "see you later" }, { "hiya", "hello" },     { "heya", "hello" },
                { "hey", "hello" },       { "yo", "hello" },           { "sup", "hello" },
                { "o/", "hello" },        { "ok", "okay" },            { "brb", "back in a moment" },
                { "whats", "what's" },    { "im", "I'm" },              { "ill", "I'll" },
                { "ive", "I've" },        { "i", "I" },                 { "lets", "let's" },
                { "thats", "that's" },    { "dont", "don't" },          { "cant", "can't" },
                { "wont", "won't" },      { "youre", "you're" },        { "u", "you" },
            };
            out = SwapWords(out, kProper);
            if (!out.empty())
                out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
            out = TrimEnd(out, " ");
            if (EndsOnWord(out))
                out += '.';
        }
        else if (style == "caps")
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        else if (style == "lower")
            out = TrimEnd(LowerAscii(out), ".! ");
        else if (style == "flat")
        {
            // "Full clear! nice work" -> "full clear, nice work"; "gz!!" -> "gz".
            std::string flat;
            for (size_t i = 0; i < out.size(); ++i)
                if (out[i] != '!')
                    flat += out[i];
                else if (i + 1 < out.size() && out[i + 1] == ' ' &&
                         out.find_first_not_of("! ", i) != std::string::npos && (flat.empty() || flat.back() != ','))
                    flat += ',';
            out = TrimEnd(flat, ". ");
            if (!out.empty())
                out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
        }
        else if (style == "short")
        {
            static const std::vector<std::pair<std::string, std::string>> kShort = {
                { "thank you", "ty" },   { "see you", "cya" },    { "good game", "gg" },  { "good job", "gj" },
                { "well played", "wp" }, { "congratulations", "gz" }, { "congrats", "gz" }, { "congratz", "gz" },
                { "grats", "gz" },       { "gratz", "gz" },       { "thanks", "ty" },     { "please", "pls" },
                { "okay", "k" },         { "ok", "k" },
            };
            out = TrimEnd(LowerAscii(SwapWords(out, kShort)), ".! ");
        }
        else if (style == "excited")
        {
            out = TrimEnd(out, ". ");
            if (EndsOnWord(out))
                out += '!';
        }
        return out;
    }

    std::vector<std::string> AnimalPhrases(const Animals& a, bool own, bool onFoot)
    {
        std::vector<std::string> out;
        const std::string their = own ? "your" : "their";

        if (!a.mount.empty() && LowerAscii(a.mount) == "mount")   // the game did not say which
            out.push_back(own ? "your mount, which you are riding" : "riding their mount");
        else if (!a.mount.empty())
            out.push_back(own ? "your mount, " + WithArticle(a.mount) + ", which you are riding"
                              : "riding their mount, " + WithArticle(a.mount));
        else if (onFoot)
            out.push_back(own ? "no mount out: you are on foot" : "on foot, with no mount out");

        if (!a.petKind.empty())
        {
            // "their hunter pet, a wolf called Howler", "their ghoul called
            // Drakz", "their water elemental".
            std::string pet = their + " " + a.petKind;
            if (!a.petFamily.empty())
                pet += ", " + WithArticle(a.petFamily);
            if (!a.petName.empty())
                pet += " called " + a.petName;
            out.push_back(own ? pet : "with " + pet);
        }

        if (!a.companion.empty())
            out.push_back(own ? "your companion pet, " + WithArticle(a.companion) + ", following you"
                              : "with their companion pet, " + WithArticle(a.companion) + ", following them");
        return out;
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
                case 'N': case 'n': raw += name;        ++i; break;
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

    // ---- what a quest asks, plainly ------------------------------------------

    std::string PluralName(const std::string& name, uint32_t count)
    {
        if (count <= 1 || name.empty())
            return name;
        const size_t space = name.find_last_of(' ');
        const std::string head = space == std::string::npos ? std::string() : name.substr(0, space + 1);
        const std::string word = space == std::string::npos ? name : name.substr(space + 1);
        std::string low = word;
        for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.empty() || !std::isalpha(static_cast<unsigned char>(low.back())))
            return name;

        static const std::vector<std::pair<std::string, std::string>> kOdd = {
            { "wolf", "wolves" }, { "elf", "elves" },     { "dwarf", "dwarves" }, { "thief", "thieves" },
            { "leaf", "leaves" }, { "calf", "calves" },   { "half", "halves" },   { "loaf", "loaves" },
            { "knife", "knives" }, { "wife", "wives" },   { "life", "lives" },    { "foot", "feet" },
            { "tooth", "teeth" }, { "mouse", "mice" },    { "goose", "geese" },   { "ox", "oxen" },
            { "child", "children" }, { "person", "people" }, { "man", "men" },    { "woman", "women" },
        };
        static const char* const kSame[] = { "vermin", "sheep", "deer", "moose", "fish", "kin", "dragonkin",
                                             "undead", "infantry", "cavalry", "remains", "scum", "spawn" };
        std::string plural;
        for (const auto& [one, many] : kOdd)
            if (low == one)
                plural = many;
        if (plural.empty())
        {
            for (const char* same : kSame)
                if (low == same)
                    return name;
            const auto ends = [&low](const char* tail)
            {
                const size_t n = std::strlen(tail);
                return low.size() >= n && low.compare(low.size() - n, n, tail) == 0;
            };
            if (ends("s"))
                return name;   // already plural, or best left alone
            if (ends("man") && low != "shaman" && low != "talisman" && low != "human" && low != "caiman")
                plural = low.substr(0, low.size() - 3) + "men";   // footman, watchman
            else if (ends("x") || ends("z") || ends("ch") || ends("sh"))
                plural = low + "es";
            else if (ends("y") && low.size() > 1 && std::string("aeiou").find(low[low.size() - 2]) == std::string::npos)
                plural = low.substr(0, low.size() - 1) + "ies";
            else
                plural = low + "s";
        }
        // In the word's own case: "Wolf" -> "Wolves", "WOLF" -> "WOLVES".
        const bool allUpper = std::all_of(word.begin(), word.end(), [](unsigned char c) { return !std::islower(c); });
        if (allUpper && word.size() > 1)
            for (char& c : plural)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (std::isupper(static_cast<unsigned char>(word[0])))
            plural[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(plural[0])));
        return head + plural;
    }

    std::string QuestPlainly(const std::vector<QuestGoal>& goals, const std::string& ender,
                             const std::string& fallback)
    {
        std::vector<std::string> parts;
        for (const QuestGoal& g : goals)
        {
            const std::string what = Trim(g.what);
            if (what.empty())
                continue;
            if (g.verb.empty())
                parts.push_back(g.count > 1 ? what + " (" + std::to_string(g.count) + ")" : what);
            else if (g.count > 1)
                parts.push_back(g.verb + " " + std::to_string(g.count) + " " + PluralName(what, g.count));
            else
                parts.push_back(g.verb + " " + what);
        }
        const std::string to = Trim(ender);
        if (parts.empty())
        {
            std::string text = Trim(fallback);
            if (to.empty())
                return text;
            std::string lowText = text, lowTo = to;
            for (char& c : lowText)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : lowTo)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lowText.find(lowTo) != std::string::npos)
                return text;
            if (text.empty())
                return "hand it in to " + to;
            while (!text.empty() && (text.back() == '.' || text.back() == ' '))
                text.pop_back();
            return text + " (it is handed in to " + to + ")";
        }
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i)
            out += (i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ")) + parts[i];
        if (!to.empty())
            out += ", then hand it in to " + to;
        return out;
    }

    // ---- wanting a kind of item ----------------------------------------------

    const std::vector<ItemKindWords>& ItemKinds()
    {
        static const std::vector<ItemKindWords> kinds = []
        {
            const std::vector<std::tuple<const char*, const char*, const char*>> raw = {
                { "wand", "a wand", "wand|wands" },
                { "staff", "a staff", "staff|staves|stave" },
                { "crossbow", "a crossbow", "crossbow|crossbows|xbow|xbows" },
                { "bow", "a bow", "bow|bows|longbow|shortbow" },
                { "gun", "a gun", "gun|guns|rifle|rifles" },
                { "thrown weapon", "a throwing weapon", "throwing weapon|throwing weapons|thrown weapon|thrown" },
                { "ranged weapon", "a ranged weapon", "ranged weapon|ranged weapons|ranged" },
                { "fist weapon", "a fist weapon", "fist weapon|fist weapons|fists|fist" },
                { "two-hander", "a two-hander",
                  "2h|2hs|2 hander|2hander|two hander|two handed|two handed weapon|2h weapon|two hand" },
                { "sword", "a sword", "sword|swords|greatsword|blade" },
                { "axe", "an axe", "axe|axes" },
                { "mace", "a mace", "mace|maces" },
                { "dagger", "a dagger", "dagger|daggers|dirk" },
                { "polearm", "a polearm", "polearm|polearms|pole arm|spear|halberd" },
                { "shield", "a shield", "shield|shields" },
                { "off-hand", "an off-hand", "offhand|off hand|offhand item" },
                { "weapon", "a weapon", "weapon|weapons|wep|weap|wpn|weps" },
                { "cloak", "a cloak", "cloak|cloaks|cape|capes" },
                { "ring", "a ring", "ring|rings" },
                { "necklace", "a necklace", "necklace|neck|amulet|pendant|neck piece|necklaces" },
                { "trinket", "a trinket", "trinket|trinkets" },
                { "helm", "a helm", "helm|helmet|helms|helmets|hat|headpiece|head piece" },
                { "shoulders", "new shoulders", "shoulders|shoulder|pauldrons|shoulderpads|shoulder pads|spaulders" },
                { "chest piece", "a chest piece",
                  "chestpiece|chest piece|chest armor|chest armour|a chest|new chest|better chest|good chest|"
                  "decent chest|robe|breastplate|tunic|vest|chest slot" },
                { "bracers", "bracers", "bracers|bracer|wrist|wrists|bracelets|wristguards" },
                { "gloves", "gloves", "gloves|gauntlets|handwraps|handguards" },
                { "belt", "a belt", "belt|belts|girdle|waist" },
                { "leggings", "leggings", "pants|legs|leggings|trousers|legplates|kilt|britches|leg armor" },
                { "boots", "boots", "boots|shoes|sandals|treads|footwear" },
                { "relic", "a relic", "relic|relics|idol|libram|totem|sigil" },
            };
            std::vector<ItemKindWords> out;
            for (const auto& [key, phrase, words] : raw)
            {
                ItemKindWords k;
                k.key    = key;
                k.phrase = phrase;
                std::string word;
                std::vector<std::string> current;
                for (const char* c = words;; ++c)
                {
                    if (*c == ' ' || *c == '|' || *c == '\0')
                    {
                        if (!word.empty())
                            current.push_back(word);
                        word.clear();
                        if ((*c == '|' || *c == '\0') && !current.empty())
                        {
                            k.words.push_back(current);
                            current.clear();
                        }
                        if (*c == '\0')
                            break;
                    }
                    else
                        word += *c;
                }
                out.push_back(std::move(k));
            }
            return out;
        }();
        return kinds;
    }

    const ItemKindWords* FindItemWant(const std::vector<std::string>& words,
                                      const std::vector<std::vector<std::string>>& wanting)
    {
        if (words.empty() || wanting.empty())
            return nullptr;
        // A word ending in '*' matches any word that starts with the rest.
        const auto at = [&words](size_t i, const std::vector<std::string>& phrase)
        {
            if (phrase.empty() || i + phrase.size() > words.size())
                return false;
            for (size_t k = 0; k < phrase.size(); ++k)
            {
                const std::string& p = phrase[k];
                const std::string& w = words[i + k];
                if (!p.empty() && p.back() == '*')
                {
                    if (w.compare(0, p.size() - 1, p, 0, p.size() - 1) != 0 || w.size() < p.size() - 1)
                        return false;
                }
                else if (w != p)
                    return false;
            }
            return true;
        };
        const auto oneOf = [](const std::string& w, std::initializer_list<const char*> list)
        {
            for (const char* x : list)
                if (w == x)
                    return true;
            return false;
        };
        // Where each want ends -- not one asking someone else, or denied.
        std::vector<size_t> wantEnds;
        for (size_t i = 0; i < words.size(); ++i)
            for (const std::vector<std::string>& w : wanting)
            {
                if (!at(i, w))
                    continue;
                // Someone else asked ("anyone need a wand?", "do you need
                // one?") just before it; told no ("dont really need one") a
                // word or two before.
                bool other = i >= 1 && oneOf(words[i - 1], { "anyone", "any1", "anybody", "who", "you", "u", "ya",
                                                             "yall", "nobody", "noone", "somebody", "someone" });
                for (size_t back = 1; back <= 2 && back <= i; ++back)
                    other = other || oneOf(words[i - back], { "dont", "don", "not", "no", "never", "doesnt", "didnt",
                                                               "wont", "cant" });
                if (!other)
                    wantEnds.push_back(i + w.size());
            }
        if (wantEnds.empty())
            return nullptr;
        // Between the want and the thing: only words like these ("need a
        // fukin wand", "where can i get a good shield") -- so not "need to
        // rest my legs", nor "need it, got a wand".
        const auto filler = [&oneOf](const std::string& w)
        {
            return oneOf(w, { "a", "an", "the", "some", "new", "good", "better", "decent", "nice", "proper", "real",
                              "any", "my", "me", "to", "get", "find", "buy", "for", "one", "more", "another",
                              "f", "fkn", "fkin", "fking", "fukin", "fuking", "fucking", "fuckin", "freaking",
                              "frigging", "damn", "bloody", "flipping", "still", "really", "epic", "blue",
                              "purple", "green", "rare", "cool", "shiny", "i", "can", "could", "do", "would",
                              "should", "go", "pick", "up", "low", "lvl", "level", "lowbie", "starter",
                              "first", "proper", "replacement", "upgraded", "upgrade", "of", "pair" });
        };
        for (const ItemKindWords& kind : ItemKinds())
            for (const std::vector<std::string>& phrase : kind.words)
                for (size_t i = 0; i < words.size(); ++i)
                {
                    if (!at(i, phrase))
                        continue;
                    for (size_t end : wantEnds)
                    {
                        if (end > i || i - end > 6)
                            continue;
                        bool between = true;
                        for (size_t k = end; k < i && between; ++k)
                            between = filler(words[k]);
                        if (between)
                            return &kind;
                    }
                }
        return nullptr;
    }
}
