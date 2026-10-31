#include "mod-ollama-chat_joined.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // A bot's last words with a real player outside party chat.
    struct Exchange
    {
        std::string       where;        // "in a whisper", "in General"
        std::string       playerLine;
        std::string       botLine;
        Clock::time_point when{};
    };
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, Exchange>> g_exchanges;   // bot -> player -> last

    // Group::AddMember runs on map threads: joins are only noted there.
    struct Join
    {
        uint64_t          group  = 0;
        uint64_t          member = 0;
        Clock::time_point when{};
    };
    std::mutex        g_joinMutex;
    std::vector<Join> g_joins;

    // Joins waiting out the moment it takes to type, on the world thread.
    struct Due
    {
        Join              join;
        Clock::time_point due{};
    };
    std::vector<Due> g_due;

    std::unordered_map<uint64_t, Clock::time_point> g_lastGreet;   // by group: someone said hi just now

    JoinedStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    std::string Where(ChatChannelSourceLocal source, const std::string& channelName)
    {
        switch (source)
        {
            case SRC_WHISPER_LOCAL: return "in a whisper";
            case SRC_SAY_LOCAL:
            case SRC_YELL_LOCAL:    return "out loud, standing near each other";
            case SRC_GUILD_LOCAL:   return "in guild chat";
            case SRC_OFFICER_LOCAL: return "in officer chat";
            case SRC_GENERAL_LOCAL: return "in " + Context_ChannelLabel(source, channelName) + " chat";
            default:                return {};
        }
    }

    std::string Fill(const std::string& templ, Player* bot, PlayerbotAI* ai, Player* player, const Exchange* ex)
    {
        const std::string personality = GetBotPersonality(bot);
        std::string out = SafeFormat(templ,
                                     fmt::arg("bot_name", bot->GetName()),
                                     fmt::arg("bot_level", bot->GetLevel()),
                                     fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                     fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                     fmt::arg("bot_personality", GetPersonalityPromptAddition(personality)),
                                     fmt::arg("player_name", player->GetName()),
                                     fmt::arg("player_level", player->GetLevel()),
                                     fmt::arg("player_class", ai->GetChatHelper()->FormatClass(player->getClass())),
                                     fmt::arg("where", ex ? ex->where : std::string()),
                                     fmt::arg("player_line", ex ? ex->playerLine : std::string()),
                                     fmt::arg("bot_line", ex ? ex->botLine : std::string()));
        if (out.empty() || out == "[Format Error]")
            return {};
        out += Roleplay_BuildVoicePrompt(bot);
        return out;
    }

    // The newest exchange this bot had with anyone real in `group` lately.
    const Exchange* LastWords(Player* bot, Group* group, Player*& with)
    {
        with = nullptr;
        auto mine = g_exchanges.find(bot->GetGUID().GetRawValue());
        if (mine == g_exchanges.end())
            return nullptr;
        const auto now = Clock::now();
        const Exchange* best = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!OllamaIsRealPlayer(member))
                continue;
            auto it = mine->second.find(member->GetGUID().GetRawValue());
            if (it == mine->second.end() || now - it->second.when > std::chrono::minutes(g_JoinedContinueMinutes))
                continue;
            if (!best || it->second.when > best->when)
            {
                best = &it->second;
                with = member;
            }
        }
        return best;
    }

    void Greet(const Join& j)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(j.member));
        if (!bot || !bot->IsInWorld() || !OllamaIsBotPlayer(bot) || !bot->IsAlive())
            return;
        Group* group = bot->GetGroup();
        if (!group || group->GetGUID().GetRawValue() != j.group || group->isBGGroup() || group->isBFGroup() ||
            bot->InBattleground() || bot->InArena())
            return;   // gone again, or a battleground raid
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->GetChatHelper())
            return;

        // Who it is talking to: the one it was just talking with, else the
        // leader who invited it, else anyone real in the group.
        Player* with = nullptr;
        const Exchange* ex = LastWords(bot, group, with);
        Player* player = with;
        if (!player)
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
            if (OllamaIsRealPlayer(leader))
                player = leader;
            else
                for (GroupReference* ref = group->GetFirstMember(); ref && !player; ref = ref->next())
                    if (OllamaIsRealPlayer(ref->GetSource()))
                        player = ref->GetSource();
        }
        if (!player)
            return;   // a group of bots: nobody to say hi to
        ++g_stats.joins;

        const auto now = Clock::now();
        if (!ex)
        {
            // Out of the blue: one hello for a group invited together, and
            // not every time.
            auto last = g_lastGreet.find(j.group);
            const bool justNow = last != g_lastGreet.end() &&
                                 now - last->second < std::chrono::seconds(g_JoinedGroupCooldownSeconds);
            if (justNow || g_JoinedGreetChance == 0 || urand(1, 100) > g_JoinedGreetChance)
            {
                ++g_stats.quiet;
                return;
            }
        }

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = player->GetGUID().GetRawValue();
        request.source            = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        request.scopeKey          = Governor_MakeScopeKey(ChatChannelSourceLocalStr[request.source], 0, std::string(), 0,
                                                          group->GetGUID().GetCounter());
        request.botName           = bot->GetName();
        request.directAddress     = true;    // owed, like an answer: no pacing cooldown
        request.triggerBotReplies = false;   // the rest of the group need not all say hi back
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);

        if (ex)
        {
            request.kind          = OllamaRequestKind::ChatReply;
            request.originMessage = ex->playerLine;
            request.prompt        = Fill(g_JoinedContinuePrompt, bot, ai, player, ex);
        }
        else
        {
            request.kind = OllamaRequestKind::EventChatter;
            if (!g_RoleplayEnable && !g_JoinedGreetLines.empty() && g_JoinedCannedChance > 0 &&
                urand(1, 100) <= g_JoinedCannedChance)
                request.fixedText = TextRules::FormatCannedLine(
                    g_JoinedGreetLines[std::uniform_int_distribution<size_t>(0, g_JoinedGreetLines.size() - 1)(Rng())],
                    player->GetName(), Rng());
            else
                request.prompt = Fill(g_JoinedGreetPrompt, bot, ai, player, nullptr);
        }
        if (request.prompt.empty() && request.fixedText.empty())
            return;
        if (!OllamaDispatch_Submit(std::move(request)))
            return;

        g_lastGreet[j.group] = now;
        if (ex)
        {
            ++g_stats.carriedOn;
            // Said: the next invite is not a carry-on from the same words.
            g_exchanges[j.member].erase(player->GetGUID().GetRawValue());
        }
        else
            ++g_stats.greeted;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} joined {}'s group: {}.", bot->GetName(), player->GetName(),
                     ex ? "carrying on from " + ex->where : std::string("saying hi"));
    }
}

void Joined_NoteExchange(Player* bot, Player* player, ChatChannelSourceLocal source,
                         const std::string& channelName, const std::string& playerLine,
                         const std::string& botLine)
{
    if (!g_JoinedEnable || !bot || !player || playerLine.empty() || botLine.empty())
        return;
    const std::string where = Where(source, channelName);
    if (where.empty())
        return;   // party or raid: they are grouped already
    Exchange& ex = g_exchanges[bot->GetGUID().GetRawValue()][player->GetGUID().GetRawValue()];
    ex.where      = where;
    ex.playerLine = Context_CleanText(playerLine);
    ex.botLine    = botLine;
    ex.when       = Clock::now();
}

void Joined_Update()
{
    if (!g_JoinedEnable)
        return;
    std::vector<Join> fresh;
    {
        std::lock_guard<std::mutex> lock(g_joinMutex);
        fresh.swap(g_joins);
    }
    const uint32_t lo = std::min(g_JoinedDelayMinMs, g_JoinedDelayMaxMs);
    const uint32_t hi = std::max(g_JoinedDelayMinMs, g_JoinedDelayMaxMs);
    for (const Join& j : fresh)
        g_due.push_back({ j, j.when + std::chrono::milliseconds(std::uniform_int_distribution<uint32_t>(lo, hi)(Rng())) });
    if (g_due.empty())
        return;

    const auto now = Clock::now();
    std::vector<Due> ready;
    for (auto it = g_due.begin(); it != g_due.end();)
    {
        if (now >= it->due)
        {
            ready.push_back(*it);
            it = g_due.erase(it);
        }
        else
            ++it;
    }
    for (const Due& d : ready)
        Greet(d.join);
}

void Joined_Maintain()
{
    const auto now = Clock::now();
    for (auto bot = g_exchanges.begin(); bot != g_exchanges.end();)
    {
        for (auto it = bot->second.begin(); it != bot->second.end();)
            it = (now - it->second.when > std::chrono::minutes(g_JoinedContinueMinutes)) ? bot->second.erase(it)
                                                                                           : std::next(it);
        bot = bot->second.empty() ? g_exchanges.erase(bot) : std::next(bot);
    }
    for (auto it = g_lastGreet.begin(); it != g_lastGreet.end();)
        it = (now - it->second > std::chrono::seconds(g_JoinedGroupCooldownSeconds)) ? g_lastGreet.erase(it)
                                                                                      : std::next(it);
}

JoinedStats Joined_GetStats()
{
    return g_stats;
}

void OllamaChatJoinScript::OnAddMember(Group* group, ObjectGuid guid)
{
    // Any thread: note it, nothing more. Only bots greet; whether anyone real
    // is in the group is looked at on the world thread.
    if (!g_Enable || !g_JoinedEnable || !group || !guid.IsPlayer() || group->isBGGroup() || group->isBFGroup())
        return;
    std::lock_guard<std::mutex> lock(g_joinMutex);
    if (g_joins.size() < 256)
        g_joins.push_back({ group->GetGUID().GetRawValue(), guid.GetRawValue(), Clock::now() });
}
