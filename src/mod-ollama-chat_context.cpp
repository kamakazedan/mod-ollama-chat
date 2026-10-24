#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Player.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Line
    {
        uint64_t          seq          = 0;
        Clock::time_point when;
        uint64_t          speakerGuid  = 0;
        std::string       speakerName;
        bool              speakerIsBot = false;
        ContextLineKind   kind         = ContextLineKind::Human;
        std::string       text;

        // /say, /yell and events seen in the world are only heard nearby.
        bool              positional   = false;
        uint8_t           team         = 0;
        uint32_t          mapId        = 0;
        uint32_t          instanceId   = 0;
        float             x = 0.0f, y = 0.0f, z = 0.0f;
        float             range        = 0.0f;
    };

    std::mutex g_mutex;
    std::unordered_map<std::string, std::deque<Line>> g_spaces;
    uint64_t g_nextSeq = 1;

    // bot guid -> filler pattern -> when that bot last used it
    std::unordered_map<uint64_t, std::unordered_map<std::string, Clock::time_point>> g_fillerUse;

    constexpr size_t kMaxStoredPerSpace = 80;
    constexpr size_t kMaxLineChars      = 160;

    double SecondsBetween(Clock::time_point older, Clock::time_point newer)
    {
        return std::chrono::duration<double>(newer - older).count();
    }

    bool IsPositionalSource(ChatChannelSourceLocal source)
    {
        return source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL;
    }

    std::string SayKey(Player* p)
    {
        return "S:" + std::to_string(p->GetMapId()) + ":" + std::to_string(p->GetInstanceId());
    }

    // One key per conversation space. Deliberately independent of the
    // governor's scope keys: those key /say on the zone and do not separate
    // the two factions' copies of a channel, both of which would leak lines
    // into a transcript that the bot could never have seen.
    std::string SpaceKey(Player* p, ChatChannelSourceLocal source, const std::string& channelName)
    {
        if (!p)
            return {};

        switch (source)
        {
            case SRC_PARTY_LOCAL:
            case SRC_RAID_LOCAL:
                if (Group* group = p->GetGroup())
                    return "P:" + std::to_string(group->GetGUID().GetCounter());
                return {};

            case SRC_GUILD_LOCAL:
                return p->GetGuildId() ? "G:" + std::to_string(p->GetGuildId()) : std::string();

            case SRC_OFFICER_LOCAL:
                return p->GetGuildId() ? "O:" + std::to_string(p->GetGuildId()) : std::string();

            case SRC_GENERAL_LOCAL:
                if (channelName.empty())
                    return {};
                return "C:" + std::to_string(uint32(p->GetTeamId())) + ":" + channelName;

            case SRC_SAY_LOCAL:
            case SRC_YELL_LOCAL:
                return SayKey(p);

            default:
                return {};   // whispers keep their own per-pair history
        }
    }

    std::string SpaceLabel(ChatChannelSourceLocal source, const std::string& channelName)
    {
        switch (source)
        {
            case SRC_PARTY_LOCAL:   return "party chat";
            case SRC_RAID_LOCAL:    return "raid chat";
            case SRC_GUILD_LOCAL:   return "guild chat";
            case SRC_OFFICER_LOCAL: return "officer chat";
            case SRC_SAY_LOCAL:
            case SRC_YELL_LOCAL:    return "/say nearby";
            case SRC_GENERAL_LOCAL:
            {
                // "General - Elwynn Forest" -> "General"
                const size_t dash = channelName.find(" - ");
                return dash == std::string::npos ? channelName : channelName.substr(0, dash);
            }
            default:                return "chat";
        }
    }

    // Item, quest and spell links arrive as client markup:
    //   |cff1eff00|Hitem:2589:0:...|h[Linen Cloth]|h|r
    // Keep the visible "[Linen Cloth]" and drop the rest; also fold line breaks
    // and runs of spaces, and cap the length so one long line cannot crowd the
    // rest of the transcript out of the prompt.
    std::string CleanText(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());

        for (size_t i = 0; i < in.size(); )
        {
            const char ch = in[i];

            if (ch == '|')
            {
                if (i + 1 >= in.size())
                {
                    ++i;   // a lone '|' at the end
                    continue;
                }
                const char code = in[i + 1];

                // "||" is a pipe someone typed. Dropped, not unescaped: a bare
                // "|H" in the transcript would read as the start of a link.
                if (code == '|') { i += 2; continue; }

                if (code == 'c')
                {
                    i += 2;
                    for (size_t n = 0; n < 8 && i < in.size() &&
                                       std::isxdigit(static_cast<unsigned char>(in[i])); ++n)
                        ++i;
                    continue;
                }

                if (code == 'H' || code == 'T')
                {
                    const char* close = (code == 'H') ? "|h" : "|t";
                    const size_t end = in.find(close, i + 2);
                    i = (end == std::string::npos) ? in.size() : end + 2;
                    continue;
                }

                if (code == 'r' || code == 'h' || code == 't' || code == 'n')
                {
                    i += 2;
                    continue;
                }

                ++i;   // any other '|' on its own; the character after it stays
                continue;
            }

            const bool space = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
            if (space)
            {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
            }
            else
            {
                out += ch;
            }
            ++i;
        }

        while (!out.empty() && out.back() == ' ')
            out.pop_back();

        if (out.size() > kMaxLineChars)
        {
            size_t cut = out.rfind(' ', kMaxLineChars);
            if (cut == std::string::npos || cut < kMaxLineChars / 2)
                cut = kMaxLineChars;
            // Never split a UTF-8 sequence.
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                --cut;
            out = out.substr(0, cut) + "...";
        }

        return out;
    }

    std::string NormalizeWord(const std::string& token)
    {
        std::string out;
        for (char c : token)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u))
                out += static_cast<char>(std::tolower(u));
        }
        return out;
    }

    std::vector<std::string> SplitSpaces(const std::string& text)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : text)
        {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
            }
            else
            {
                cur += c;
            }
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    // Which configured filler pattern this word matches, or "".
    // "haha*" matches any word starting with "haha".
    std::string MatchFiller(const std::string& normalized)
    {
        if (normalized.empty())
            return {};

        for (const std::string& pattern : g_ContextFillerWords)
        {
            if (pattern.empty())
                continue;

            if (pattern.back() == '*')
            {
                const std::string prefix = pattern.substr(0, pattern.size() - 1);
                if (!prefix.empty() && normalized.size() >= prefix.size() &&
                    normalized.compare(0, prefix.size(), prefix) == 0)
                    return pattern;
            }
            else if (normalized == pattern)
            {
                return pattern;
            }
        }
        return {};
    }

    // Could `viewer` have seen this line? Positional lines need the viewer
    // on the same map, same side, and within earshot of where it was said.
    bool VisibleTo(const Line& line, Player* viewer)
    {
        if (!line.positional)
            return true;

        if (line.team != uint8(viewer->GetTeamId()))
            return false;

        if (viewer->GetMapId() != line.mapId || viewer->GetInstanceId() != line.instanceId)
            return false;

        const float dx = viewer->GetPositionX() - line.x;
        const float dy = viewer->GetPositionY() - line.y;
        const float dz = viewer->GetPositionZ() - line.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz) <= line.range;
    }

    Line MakeLine(Player* speaker, ContextLineKind kind, std::string text)
    {
        Line line;
        line.when         = Clock::now();
        line.speakerGuid  = speaker->GetGUID().GetRawValue();
        line.speakerName  = speaker->GetName();
        line.speakerIsBot = OllamaIsBotPlayer(speaker);
        line.kind         = kind;
        line.text         = std::move(text);
        line.team         = uint8(speaker->GetTeamId());
        line.mapId        = speaker->GetMapId();
        line.instanceId   = speaker->GetInstanceId();
        line.x            = speaker->GetPositionX();
        line.y            = speaker->GetPositionY();
        line.z            = speaker->GetPositionZ();
        return line;
    }

    // Caller holds g_mutex.
    uint64_t AppendLocked(const std::string& key, Line line)
    {
        line.seq = g_nextSeq++;
        std::deque<Line>& lines = g_spaces[key];
        lines.push_back(std::move(line));
        while (lines.size() > kMaxStoredPerSpace)
            lines.pop_front();
        return lines.back().seq;
    }

    // Caller holds g_mutex.
    void NoteFillersUsedLocked(uint64_t botGuid, const std::string& text, Clock::time_point now)
    {
        if (g_ContextFillerWords.empty())
            return;

        for (const std::string& token : SplitSpaces(text))
        {
            const std::string pattern = MatchFiller(NormalizeWord(token));
            if (!pattern.empty())
                g_fillerUse[botGuid][pattern] = now;
        }
    }
}

// --------------------------------------------------------------------------

uint64_t Context_RecordLine(Player* speaker, ChatChannelSourceLocal source,
                            const std::string& channelName, const std::string& text,
                            ContextLineKind kind)
{
    if (!g_ContextEnable || !speaker || !speaker->IsInWorld())
        return 0;

    const std::string key = SpaceKey(speaker, source, channelName);
    if (key.empty())
        return 0;

    std::string clean = CleanText(text);
    if (clean.empty())
        return 0;

    Line line = MakeLine(speaker, kind, std::move(clean));
    if (IsPositionalSource(source))
    {
        line.positional = true;
        line.range      = (source == SRC_YELL_LOCAL) ? g_YellDistance : g_SayDistance;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (kind == ContextLineKind::Bot)
        NoteFillersUsedLocked(line.speakerGuid, line.text, line.when);
    return AppendLocked(key, std::move(line));
}

void Context_RecordEvent(Player* actor, const std::string& note, bool toGuild)
{
    if (!g_ContextEnable || !actor || !actor->IsInWorld() || note.empty())
        return;

    const std::string clean = CleanText(note);
    if (clean.empty())
        return;

    std::lock_guard<std::mutex> lock(g_mutex);

    if (toGuild)
    {
        if (actor->GetGuildId())
            AppendLocked("G:" + std::to_string(actor->GetGuildId()),
                         MakeLine(actor, ContextLineKind::Event, clean));
        return;
    }

    if (Group* group = actor->GetGroup())
        AppendLocked("P:" + std::to_string(group->GetGUID().GetCounter()),
                     MakeLine(actor, ContextLineKind::Event, clean));

    Line nearby = MakeLine(actor, ContextLineKind::Event, clean);
    nearby.positional = true;
    nearby.range      = std::max(g_SayDistance, g_EventChatterRealPlayerDistance);
    AppendLocked(SayKey(actor), std::move(nearby));
}

uint64_t Context_LatestLineFrom(Player* speaker, ChatChannelSourceLocal source,
                                const std::string& channelName)
{
    if (!g_ContextEnable || !speaker)
        return 0;

    const std::string key = SpaceKey(speaker, source, channelName);
    if (key.empty())
        return 0;

    const uint64_t guid = speaker->GetGUID().GetRawValue();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_spaces.find(key);
    if (it == g_spaces.end())
        return 0;

    for (auto line = it->second.rbegin(); line != it->second.rend(); ++line)
        if (line->speakerGuid == guid && line->kind != ContextLineKind::Event)
            return line->seq;

    return 0;
}

std::string Context_BuildTranscript(Player* viewer, ChatChannelSourceLocal source,
                                    const std::string& channelName, uint64_t excludeSeq)
{
    if (!g_ContextEnable || !viewer || g_ContextMaxLines == 0)
        return {};

    const std::string key = SpaceKey(viewer, source, channelName);
    if (key.empty())
        return {};

    const uint64_t viewerGuid = viewer->GetGUID().GetRawValue();
    const auto now = Clock::now();

    std::vector<Line> picked;   // newest first while collecting
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_spaces.find(key);
        if (it == g_spaces.end())
            return {};

        for (auto line = it->second.rbegin();
             line != it->second.rend() && picked.size() < g_ContextMaxLines; ++line)
        {
            if (SecondsBetween(line->when, now) > double(g_ContextMaxAgeSeconds))
                break;   // older lines are older still
            if (excludeSeq && line->seq == excludeSeq)
                continue;
            if (!VisibleTo(*line, viewer))
                continue;
            picked.push_back(*line);
        }
    }

    if (picked.empty())
        return {};

    std::string out = SafeFormat(g_ContextHistoryHeader,
                                 fmt::arg("channel", SpaceLabel(source, channelName)));
    if (out == "[Format Error]")
        out = "Recent chat:";

    for (auto line = picked.rbegin(); line != picked.rend(); ++line)
    {
        out += '\n';
        if (line->kind == ContextLineKind::Event)
        {
            out += "* " + line->text;
            continue;
        }

        out += line->speakerName;
        if (line->speakerGuid == viewerGuid)
            out += " (you)";
        out += ": " + line->text;
    }

    out += "\n\n";
    return out;
}

ContextAddressee Context_LikelyAddressee(Player* speaker, ChatChannelSourceLocal source,
                                         const std::string& channelName, uint64_t seq)
{
    ContextAddressee result;

    if (!g_ContextEnable || !speaker || seq == 0 || g_ContextReplyWindowSeconds == 0)
        return result;

    const std::string key = SpaceKey(speaker, source, channelName);
    if (key.empty())
        return result;

    const uint64_t speakerGuid = speaker->GetGUID().GetRawValue();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_spaces.find(key);
    if (it == g_spaces.end())
        return result;

    // The speaker's turn starts at their first consecutive line; the line
    // before that turn is what they are most likely answering.
    Clock::time_point turnStart = Clock::now();
    uint32_t looked = 0;

    for (auto line = it->second.rbegin(); line != it->second.rend() && looked < 16; ++line)
    {
        if (line->seq >= seq)
        {
            if (line->seq == seq)
                turnStart = line->when;
            continue;
        }

        if (!VisibleTo(*line, speaker))
            continue;

        ++looked;

        // Too long a pause before this point: the line starts something new.
        if (SecondsBetween(line->when, turnStart) > double(g_ContextReplyWindowSeconds))
            break;

        if (line->speakerGuid == speakerGuid)
        {
            turnStart = line->when;   // still the speaker's own turn
            continue;
        }

        result.guid  = line->speakerGuid;
        result.isBot = line->speakerIsBot;
        result.name  = line->speakerName;
        break;
    }

    return result;
}

bool Context_IsPassReply(const std::string& text)
{
    if (g_ContextPassTokenNorm.empty() || text.empty())
        return false;

    std::vector<std::string> words;
    for (const std::string& token : SplitSpaces(text))
    {
        const std::string w = NormalizeWord(token);
        if (!w.empty())
            words.push_back(w);
    }

    for (size_t i = 0; i < words.size(); ++i)
    {
        if (words[i] == g_ContextPassTokenNorm)
            return true;
        // "NO REPLY", "no-reply"
        if (i + 1 < words.size() && words[i] + words[i + 1] == g_ContextPassTokenNorm)
            return true;
    }
    return false;
}

std::string Context_RationFillers(Player* bot, ChatChannelSourceLocal source,
                                  const std::string& channelName, const std::string& text)
{
    if (!g_ContextEnable || !bot || g_ContextFillerWords.empty() || text.empty())
        return text;

    const auto now = Clock::now();
    const uint64_t botGuid = bot->GetGUID().GetRawValue();

    std::set<std::string> recentlySaid;     // patterns heard in this space lately
    std::set<std::string> recentlyUsed;     // patterns this bot used lately
    bool recentBareLaugh = false;           // a bot line lately (any bot) that was only filler
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        const std::string key = SpaceKey(bot, source, channelName);
        if (!key.empty() && g_ContextFillerScopeLines > 0)
        {
            auto it = g_spaces.find(key);
            if (it != g_spaces.end())
            {
                uint32_t seen = 0;
                for (auto line = it->second.rbegin();
                     line != it->second.rend() && seen < g_ContextFillerScopeLines; ++line)
                {
                    if (line->kind == ContextLineKind::Event || !VisibleTo(*line, bot))
                        continue;
                    ++seen;

                    bool onlyFiller = true;
                    bool anyWord    = false;
                    for (const std::string& token : SplitSpaces(line->text))
                    {
                        const std::string word = NormalizeWord(token);
                        if (word.empty())
                            continue;
                        anyWord = true;
                        const std::string pattern = MatchFiller(word);
                        if (!pattern.empty())
                            recentlySaid.insert(pattern);
                        else
                            onlyFiller = false;
                    }

                    if (line->kind == ContextLineKind::Bot && anyWord && onlyFiller)
                        recentBareLaugh = true;
                }
            }
        }

        if (g_ContextFillerBotCooldownSeconds > 0)
        {
            auto use = g_fillerUse.find(botGuid);
            if (use != g_fillerUse.end())
                for (const auto& [pattern, when] : use->second)
                    if (SecondsBetween(when, now) < double(g_ContextFillerBotCooldownSeconds))
                        recentlyUsed.insert(pattern);
        }
    }

    // Nothing but a laugh: a fine reaction once, never the second in a row.
    if (recentBareLaugh)
    {
        bool anyWord    = false;
        bool onlyFiller = true;
        for (const std::string& token : SplitSpaces(text))
        {
            const std::string word = NormalizeWord(token);
            if (word.empty())
                continue;
            anyWord = true;
            if (MatchFiller(word).empty())
            {
                onlyFiller = false;
                break;
            }
        }
        if (anyWord && onlyFiller)
            return std::string();
    }

    std::vector<std::string> kept;
    uint32_t fillersKept = 0;
    bool changed = false;

    for (const std::string& token : SplitSpaces(text))
    {
        const std::string pattern = MatchFiller(NormalizeWord(token));
        if (pattern.empty())
        {
            kept.push_back(token);
            continue;
        }

        const bool drop = fillersKept >= g_ContextMaxFillersPerLine ||
                          recentlySaid.count(pattern) > 0 ||
                          recentlyUsed.count(pattern) > 0;
        if (drop)
        {
            changed = true;
            continue;
        }

        ++fillersKept;
        kept.push_back(token);
    }

    if (!changed)
        return text;

    std::string out;
    for (const std::string& token : kept)
    {
        if (!out.empty())
            out += ' ';
        out += token;
    }

    // Tidy what the removals left at the edges: ", why" -> "why".
    auto isEdgeJunk = [](char c) { return c == ' ' || c == ',' || c == ';' || c == ':' || c == '-'; };
    while (!out.empty() && isEdgeJunk(out.front()))
        out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == ',' || out.back() == ';' || out.back() == '-'))
        out.pop_back();

    // Nothing left worth saying.
    const bool hasWord = std::any_of(out.begin(), out.end(),
                                     [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
    return hasWord ? out : std::string();
}

std::string Context_CleanText(const std::string& text)
{
    return CleanText(text);
}

void Context_Update()
{
    const auto now = Clock::now();
    const double maxAge = std::max<double>(60.0, double(g_ContextMaxAgeSeconds));
    const double fillerAge = std::max<double>(60.0, double(g_ContextFillerBotCooldownSeconds));

    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto it = g_spaces.begin(); it != g_spaces.end(); )
    {
        std::deque<Line>& lines = it->second;
        while (!lines.empty() && SecondsBetween(lines.front().when, now) > maxAge)
            lines.pop_front();

        it = lines.empty() ? g_spaces.erase(it) : std::next(it);
    }

    for (auto bot = g_fillerUse.begin(); bot != g_fillerUse.end(); )
    {
        auto& uses = bot->second;
        for (auto use = uses.begin(); use != uses.end(); )
            use = (SecondsBetween(use->second, now) > fillerAge) ? uses.erase(use) : std::next(use);

        bot = uses.empty() ? g_fillerUse.erase(bot) : std::next(bot);
    }
}

void Context_ForgetPlayer(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fillerUse.erase(guid);
}

void Context_Reset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_spaces.clear();
    g_fillerUse.clear();
}
