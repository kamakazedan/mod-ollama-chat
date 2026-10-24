#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_config.h"

#include "Random.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <utility>

// --------------------------------------------------------------------------
// Text helpers
// --------------------------------------------------------------------------

namespace
{
    bool WordMatches(const std::string& pattern, const std::string& word)
    {
        if (pattern.empty())
            return false;

        if (pattern.back() == '*')
        {
            const size_t len = pattern.size() - 1;
            return len > 0 && word.size() >= len && word.compare(0, len, pattern, 0, len) == 0;
        }

        return word == pattern;
    }
}

namespace CrowdText
{
    std::vector<std::string> Words(const std::string& text)
    {
        std::vector<std::string> out;
        std::string cur;

        for (char ch : text)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c >= 0x80)
                cur += ch;                      // UTF-8: accented names, Cyrillic, CJK stay words
            else if (std::isalnum(c))
                cur += static_cast<char>(std::tolower(c));
            else if (c == '\'')
                continue;                       // who's -> whos, y'all -> yall
            else if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }

        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    std::vector<std::vector<std::string>> ParsePhrases(const std::string& raw)
    {
        std::vector<std::vector<std::string>> out;

        size_t start = 0;
        while (start <= raw.size())
        {
            size_t bar = raw.find('|', start);
            if (bar == std::string::npos)
                bar = raw.size();

            std::vector<std::string> phrase;
            std::string cur;
            auto flush = [&]()
            {
                if (!cur.empty() && cur != "*")
                    phrase.push_back(cur);
                cur.clear();
            };

            for (size_t i = start; i < bar; ++i)
            {
                const unsigned char c = static_cast<unsigned char>(raw[i]);
                if (std::isalnum(c))
                    cur += static_cast<char>(std::tolower(c));
                else if (c == '*')
                {
                    if (!cur.empty())
                    {
                        cur += '*';
                        flush();
                    }
                }
                else if (c == '\'')
                    continue;
                else
                    flush();
            }
            flush();

            if (!phrase.empty())
                out.push_back(std::move(phrase));

            start = bar + 1;
        }

        return out;
    }

    bool ContainsPhrase(const std::vector<std::string>& words,
                        const std::vector<std::vector<std::string>>& phrases)
    {
        for (const auto& phrase : phrases)
        {
            if (phrase.empty() || phrase.size() > words.size())
                continue;

            for (size_t i = 0; i + phrase.size() <= words.size(); ++i)
            {
                bool all = true;
                for (size_t j = 0; j < phrase.size() && all; ++j)
                    all = WordMatches(phrase[j], words[i + j]);
                if (all)
                    return true;
            }
        }
        return false;
    }

    bool IsAcknowledgement(const std::vector<std::string>& words,
                           const std::vector<std::vector<std::string>>& ackWords)
    {
        for (const std::string& word : words)
        {
            bool known = false;
            for (const auto& entry : ackWords)
            {
                if (entry.size() == 1 && WordMatches(entry.front(), word))
                {
                    known = true;
                    break;
                }
            }
            if (!known)
                return false;
        }
        return true;   // includes a line with no words at all: ":)", "?"
    }

    bool IsShouted(const std::string& text)
    {
        uint32_t letters = 0;
        uint32_t upper   = 0;
        for (char ch : text)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!std::isalpha(c))
                continue;
            ++letters;
            if (std::isupper(c))
                ++upper;
        }
        return letters >= 5 && upper * 10 >= letters * 8;
    }

    bool IsGreeting(const std::vector<std::string>& words,
                    const std::vector<std::vector<std::string>>& greetWords)
    {
        if (words.empty() || greetWords.empty())
            return false;

        bool real = false;   // more than "there" / "again"
        for (const std::string& word : words)
        {
            bool known = false;
            for (const auto& entry : greetWords)
            {
                if (entry.size() == 1 && WordMatches(entry.front(), word))
                {
                    known = true;
                    break;
                }
            }
            if (!known)
                return false;
            if (word != "there" && word != "again")
                real = true;
        }
        return real;
    }

    LineTraits Classify(const std::string& cleanText,
                        const std::vector<std::vector<std::string>>& ackWords,
                        const std::vector<std::vector<std::string>>& openPhrases,
                        const std::vector<std::string>& names,
                        const std::vector<std::vector<std::string>>& greetWords)
    {
        // Names say who a line is for, not what kind of line it is:
        // "ty Lahu" is a thank-you, "THANKS LAHU" too.
        std::vector<std::string> nameWords;
        for (const std::string& name : names)
            for (std::string& w : Words(name))
                nameWords.push_back(std::move(w));

        std::vector<std::string> words;
        for (std::string& w : Words(cleanText))
            if (std::find(nameWords.begin(), nameWords.end(), w) == nameWords.end())
                words.push_back(std::move(w));

        // "guys?" or "anyone??" on its own is a call to the room, even when
        // the same word also ends a thank-you ("thanks guys").
        bool onlyCallWords = !words.empty();
        for (const std::string& w : words)
        {
            if (!ContainsPhrase({ w }, openPhrases))
            {
                onlyCallWords = false;
                break;
            }
        }

        LineTraits t;
        t.shouted   = IsShouted(cleanText);
        t.ack       = !onlyCallWords && IsAcknowledgement(words, ackWords);
        t.openWords = !t.ack && ContainsPhrase(words, openPhrases);
        t.open      = !t.ack && (t.openWords || t.shouted);
        t.greeting  = !t.ack && IsGreeting(words, greetWords);
        return t;
    }

    uint32_t Capacity(uint32_t listeners, uint32_t botsPerResponder, uint32_t maxResponders)
    {
        if (listeners == 0)
            return 0;

        const uint32_t per = std::max<uint32_t>(1, botsPerResponder);
        const uint32_t cap = (listeners + per - 1) / per;
        return std::clamp<uint32_t>(cap, 1, std::max<uint32_t>(1, maxResponders));
    }

    std::string JoinNames(const std::vector<std::string>& names)
    {
        std::string out;
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i > 0)
                out += (i + 1 == names.size()) ? " and " : ", ";
            out += names[i];
        }
        return out;
    }

    std::string ListenerPhrase(uint32_t listeners)
    {
        if (listeners <= 1)
            return "one other player";
        if (listeners <= 4)
            return "a few other players";
        if (listeners <= 12)
            return "a fair few players";
        return "lots of players";
    }
}

// --------------------------------------------------------------------------
// Plans
// --------------------------------------------------------------------------

namespace
{
    using Clock = std::chrono::steady_clock;

    // Scope key -> the plan running there. One per scope: a new line in a
    // scope replaces the plan for the line before it. Touched on the world
    // thread only.
    std::unordered_map<std::string, CrowdPlan> g_plans;
    uint64_t   g_nextPlanId = 1;
    CrowdStats g_stats;

    // Workers report failed generations here; Crowd_Update drains it.
    std::mutex g_failMutex;
    std::vector<std::pair<uint64_t, uint32_t>> g_failures;

    // Slots given up on after SlotTimeoutSeconds, by plan and slot. Counted as
    // "said nothing" at the time; if the line turns up after all, the count is
    // moved to "spoke" -- it did appear in chat.
    std::unordered_map<uint64_t, Clock::time_point> g_timedOut;

    uint64_t SlotKey(uint64_t planId, uint32_t slot)
    {
        return (planId << 12) ^ uint64_t(slot & 0xFFF);
    }

    uint32_t RandomMs(uint32_t lo, uint32_t hi)
    {
        if (hi < lo)
            std::swap(lo, hi);
        return lo == hi ? lo : urand(lo, hi);
    }

    // Fire the next slot that can fire. A slot whose bot can no longer answer
    // is skipped at once rather than costing the crowd another pause.
    void FireNext(CrowdPlan& plan, Clock::time_point now)
    {
        while (plan.next < plan.slots.size())
        {
            const size_t index = plan.next++;
            if (Crowd_FireSlot(plan, index))
            {
                plan.inFlight      = true;
                plan.inFlightSlot  = index;
                plan.inFlightSince = now;
                ++g_stats.slotsFired;
                return;
            }
            ++g_stats.slotsSkipped;
        }
    }

    void Finish(CrowdPlan& plan, bool landed, uint64_t botGuid, const std::string& botName,
                Clock::time_point now)
    {
        plan.inFlight = false;

        if (landed)
        {
            ++g_stats.slotsLanded;
            if (botGuid &&
                std::find(plan.answeredGuids.begin(), plan.answeredGuids.end(), botGuid) == plan.answeredGuids.end())
            {
                plan.answeredGuids.push_back(botGuid);
                plan.answeredBy.push_back(botName);
            }
            // Time to read it before the next person starts typing. The next
            // reply's own generation and typing delay come on top of this.
            plan.nextFireAt = now + std::chrono::milliseconds(RandomMs(g_CrowdGapMinMs, g_CrowdGapMaxMs));
        }
        else
        {
            ++g_stats.slotsDropped;
            // Nothing appeared in chat, so there is nothing to read first.
            plan.nextFireAt = now + std::chrono::milliseconds(RandomMs(250, 1000));
        }
    }

    CrowdPlan* FindById(uint64_t id)
    {
        if (id == 0)
            return nullptr;
        for (auto& entry : g_plans)
            if (entry.second.id == id)
                return &entry.second;
        return nullptr;
    }

    bool Done(const CrowdPlan& plan)
    {
        return !plan.inFlight && plan.next >= plan.slots.size();
    }
}

uint64_t Crowd_NextPlanId()
{
    return g_nextPlanId++;
}

std::string Crowd_KeyFor(const std::string& scopeKey, ChatChannelSourceLocal source, uint32_t teamId)
{
    switch (source)
    {
        case SRC_GENERAL_LOCAL:
        case SRC_SAY_LOCAL:
        case SRC_YELL_LOCAL:
            return scopeKey + "|t" + std::to_string(teamId);
        default:
            return scopeKey;
    }
}

bool Crowd_IsPlayful(const std::string& personalityKey)
{
    if (personalityKey.empty() || g_CrowdPlayfulPersonalities.empty())
        return false;

    std::string key;
    key.reserve(personalityKey.size());
    for (char c : personalityKey)
        key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    return g_CrowdPlayfulPersonalities.count(key) != 0;
}

void Crowd_Start(CrowdPlan plan, uint32_t firstDelayMs)
{
    if (plan.slots.empty() || plan.scopeKey.empty())
        return;

    const auto now = Clock::now();
    if (plan.id == 0)
        plan.id = Crowd_NextPlanId();

    plan.created    = now;
    plan.next       = 0;
    plan.inFlight   = false;
    plan.nextFireAt = now + std::chrono::milliseconds(firstDelayMs);

    ++g_stats.plansStarted;

    if (plan.key.empty())
        plan.key = plan.scopeKey;
    const std::string key = plan.key;
    CrowdPlan& stored = g_plans[key];   // replaces the plan for an earlier line
    stored = std::move(plan);

    if (firstDelayMs == 0)
    {
        FireNext(stored, now);
        if (Done(stored))
            g_plans.erase(key);
    }
}

CrowdPlan* Crowd_Find(const std::string& key, uint64_t planId)
{
    auto it = g_plans.find(key);
    if (it == g_plans.end())
        return nullptr;
    if (planId != 0 && it->second.id != planId)
        return nullptr;
    return &it->second;
}

CrowdPlan* Crowd_FindById(uint64_t planId)
{
    return FindById(planId);
}

void Crowd_OnSlotFinished(uint64_t planId, uint32_t slot, bool landed,
                          uint64_t botGuid, const std::string& botName)
{
    // A slot that timed out was counted as "said nothing". If its line made it
    // into chat after all, count it where it belongs -- even when the plan is
    // over by now.
    if (planId != 0 && !g_timedOut.empty())
    {
        auto late = g_timedOut.find(SlotKey(planId, slot));
        if (late != g_timedOut.end())
        {
            g_timedOut.erase(late);
            if (landed)
            {
                if (g_stats.slotsDropped > 0)
                    --g_stats.slotsDropped;
                ++g_stats.slotsLanded;
                ++g_stats.slotsLate;
            }
        }
    }

    CrowdPlan* plan = FindById(planId);
    if (!plan)
        return;   // superseded by a newer line, or over

    if (plan->inFlight && plan->inFlightSlot == slot)
    {
        Finish(*plan, landed, botGuid, botName, Clock::now());
        return;
    }

    // A reply that came in after its slot was given up on: the crowd has
    // moved on, but the line is in chat, so later voices should know.
    if (landed && botGuid &&
        std::find(plan->answeredGuids.begin(), plan->answeredGuids.end(), botGuid) == plan->answeredGuids.end())
    {
        plan->answeredGuids.push_back(botGuid);
        plan->answeredBy.push_back(botName);
    }
}

void Crowd_ReportWorkerFailure(uint64_t planId, uint32_t slot)
{
    if (planId == 0)
        return;

    std::lock_guard<std::mutex> lock(g_failMutex);
    g_failures.emplace_back(planId, slot);
}

void Crowd_Update()
{
    std::vector<std::pair<uint64_t, uint32_t>> failures;
    {
        std::lock_guard<std::mutex> lock(g_failMutex);
        failures.swap(g_failures);
    }

    const auto now = Clock::now();

    // Forget timed-out slots whose line never turned up.
    if (!g_timedOut.empty())
    {
        for (auto it = g_timedOut.begin(); it != g_timedOut.end(); )
        {
            if (now - it->second > std::chrono::minutes(10))
                it = g_timedOut.erase(it);
            else
                ++it;
        }
    }

    if (g_plans.empty())
        return;

    for (const auto& [planId, slot] : failures)
    {
        CrowdPlan* plan = FindById(planId);
        if (plan && plan->inFlight && plan->inFlightSlot == slot)
            Finish(*plan, false, 0, std::string(), now);
    }

    const auto slotTimeout = std::chrono::seconds(std::max<uint32_t>(5, g_CrowdSlotTimeoutSeconds));
    const auto maxAge      = std::chrono::seconds(std::max<uint32_t>(15, g_CrowdMaxPlanSeconds));

    for (auto it = g_plans.begin(); it != g_plans.end(); )
    {
        CrowdPlan& plan = it->second;

        if (now - plan.created > maxAge)
        {
            it = g_plans.erase(it);
            continue;
        }

        // A generation that never reported back (lost to a reload, a crash in
        // the HTTP client, a model still loading...) must not stall the rest
        // of the crowd.
        if (plan.inFlight && now - plan.inFlightSince > slotTimeout)
        {
            if (g_timedOut.size() < 4096)
                g_timedOut[SlotKey(plan.id, static_cast<uint32_t>(plan.inFlightSlot))] = now;
            Finish(plan, false, 0, std::string(), now);
        }

        if (!plan.inFlight && plan.next < plan.slots.size() && now >= plan.nextFireAt)
            FireNext(plan, now);

        if (Done(plan))
        {
            it = g_plans.erase(it);
            continue;
        }
        ++it;
    }
}

void Crowd_Reset()
{
    g_plans.clear();
    g_timedOut.clear();
    std::lock_guard<std::mutex> lock(g_failMutex);
    g_failures.clear();
}

void Crowd_NoteReplyBack()
{
    ++g_stats.replyBacks;
}

CrowdStats Crowd_GetStats()
{
    CrowdStats out = g_stats;
    out.active = static_cast<uint32_t>(g_plans.size());
    return out;
}
