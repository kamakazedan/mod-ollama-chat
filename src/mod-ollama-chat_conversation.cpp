#include "mod-ollama-chat_conversation.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_convlogic.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_presence.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"   // ChatChannelId

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <random>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Seat
    {
        uint64_t          guid   = 0;
        std::string       name;
        uint32_t          lines  = 0;
        uint32_t          misses = 0;
        Clock::time_point lastSpoke{};
        bool              spoke  = false;
    };

    struct Conversation
    {
        uint64_t               id = 0;
        std::string            key;
        std::string            scopeKey;
        ChatChannelSourceLocal source = SRC_UNDEFINED_LOCAL;
        std::string            channelName;
        uint32_t               channelId = 0;
        uint32_t               teamId = 0;
        std::string            premise;     // what it is about, when a bot opened it on purpose
        bool                   byPlayer = false;

        std::vector<Seat> seats;

        // The newest line, and who spoke before it.
        uint64_t    lastGuid  = 0;
        uint64_t    prevGuid  = 0;
        std::string lastName;
        std::string lastText;
        bool        lastIsBot = true;
        uint64_t    lastSeq   = 0;
        uint64_t    humanAddressee = 0;   // the bot a person's newest line answers

        Clock::time_point created{};
        Clock::time_point endAt{};
        Clock::time_point nextTurnAt{};

        bool     winding  = false;
        uint32_t windLeft = 0;

        uint64_t turnPlanId = 0;
        uint64_t turnGuid   = 0;
        bool     turnLanded = false;
        Clock::time_point turnFiredAt{};
        uint32_t missesInRow = 0;

        bool              awaitingOpener = false;
        uint64_t          openerGuid     = 0;
        Clock::time_point openerDeadline{};

        // /say: where the newest line in it was said. One /say key covers a
        // whole zone, so this is what tells its lines from everyone else's.
        bool     anchored   = false;
        uint32_t mapId      = 0;
        uint32_t instanceId = 0;
        float    ax = 0.0f, ay = 0.0f, az = 0.0f;
    };

    std::unordered_map<std::string, Conversation>      g_convs;
    std::unordered_map<std::string, Clock::time_point> g_cooldownUntil;
    uint64_t          g_nextId = 1;
    ConversationStats g_stats;

    // Logouts, queued from whichever thread logged the character out (bots are
    // logged out from map threads), and how many conversations are running,
    // so a logout with none running costs nothing.
    std::mutex            g_forgetMutex;
    std::vector<uint64_t> g_forget;
    std::atomic<bool>     g_forgetPending{ false };
    std::atomic<uint32_t> g_convCount{ 0 };

    void Recount()
    {
        g_convCount.store(static_cast<uint32_t>(g_convs.size()), std::memory_order_relaxed);
    }

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint32_t RandomBetween(uint32_t lo, uint32_t hi)
    {
        if (hi < lo)
            std::swap(lo, hi);
        return std::uniform_int_distribution<uint32_t>(lo, hi)(Rng());
    }

    Clock::duration PlannedLength()
    {
        return std::chrono::seconds(RandomBetween(g_ConversationMinMinutes * 60, g_ConversationMaxMinutes * 60));
    }

    Seat* FindSeat(Conversation& c, uint64_t guid)
    {
        for (Seat& s : c.seats)
            if (s.guid == guid)
                return &s;
        return nullptr;
    }

    bool AddSeat(Conversation& c, Player* bot)
    {
        if (!bot || FindSeat(c, bot->GetGUID().GetRawValue()))
            return false;
        if (c.seats.size() >= g_ConversationMaxParticipants)
            return false;
        Seat s;
        s.guid = bot->GetGUID().GetRawValue();
        s.name = bot->GetName();
        c.seats.push_back(std::move(s));
        return true;
    }

    // Remember where a /say conversation's newest line was said.
    void Anchor(Conversation& c, Player* p)
    {
        if (c.source != SRC_SAY_LOCAL || !p || !p->IsInWorld())
            return;
        c.anchored   = true;
        c.mapId      = p->GetMapId();
        c.instanceId = p->GetInstanceId();
        c.ax = p->GetPositionX();
        c.ay = p->GetPositionY();
        c.az = p->GetPositionZ();
    }

    // Is `p` part of it? Everyone in a channel hears a channel conversation. A
    // /say one is heard only near where it is going on: by its newest line, or
    // by someone sitting in it.
    bool Hears(const Conversation& c, Player* p)
    {
        if (c.source != SRC_SAY_LOCAL)
            return true;
        if (!p || !p->IsInWorld())
            return false;

        const float range = g_SayDistance + 5.0f;
        if (c.anchored && p->GetMapId() == c.mapId && p->GetInstanceId() == c.instanceId &&
            p->GetExactDist(c.ax, c.ay, c.az) <= range)
            return true;

        const uint64_t self = p->GetGUID().GetRawValue();
        for (const Seat& s : c.seats)
        {
            if (s.guid == self)
                continue;
            Player* other = ObjectAccessor::FindConnectedPlayer(ObjectGuid(s.guid));
            if (other && other->IsInWorld() && other->GetMap() == p->GetMap() && other->IsWithinDist(p, range))
                return true;
        }
        return false;
    }

    // Drop the characters that logged out since the last tick.
    void ApplyForgets()
    {
        if (!g_forgetPending.exchange(false))
            return;

        std::vector<uint64_t> gone;
        {
            std::lock_guard<std::mutex> lock(g_forgetMutex);
            gone.swap(g_forget);
        }

        for (auto& entry : g_convs)
        {
            auto& seats = entry.second.seats;
            seats.erase(std::remove_if(seats.begin(), seats.end(),
                                       [&gone](const Seat& s)
                                       { return std::find(gone.begin(), gone.end(), s.guid) != gone.end(); }),
                        seats.end());
        }
    }

    double Progress(const Conversation& c, Clock::time_point now)
    {
        const auto total = c.endAt - c.created;
        if (total.count() <= 0)
            return 1.0;
        return std::clamp(double((now - c.created).count()) / double(total.count()), 0.0, 1.0);
    }

    void ScheduleNext(Conversation& c, Clock::time_point now)
    {
        const uint32_t gap = ConvLogic::NextGapMs(g_ConversationGapMinSeconds * 1000,
                                                  g_ConversationGapMaxSeconds * 1000,
                                                  Progress(c, now),
                                                  g_ConversationLullChance,
                                                  g_ConversationLullMinSeconds * 1000,
                                                  g_ConversationLullMaxSeconds * 1000, Rng());
        c.nextTurnAt = now + std::chrono::milliseconds(gap);
    }

    Conversation& Create(const std::string& key, const std::string& scopeKey, ChatChannelSourceLocal source,
                         const std::string& channelName, uint32_t channelId, uint32_t teamId,
                         bool byPlayer, Clock::time_point now)
    {
        Conversation& c = g_convs[key];
        c = Conversation();
        c.id          = g_nextId++;
        c.key         = key;
        c.scopeKey    = scopeKey;
        c.source      = source;
        c.channelName = channelName;
        c.channelId   = channelId;
        c.teamId      = teamId;
        c.byPlayer    = byPlayer;
        c.created     = now;
        c.endAt       = now + PlannedLength();
        c.nextTurnAt  = now + std::chrono::seconds(2);
        Recount();

        ++g_stats.started;
        if (byPlayer)
            ++g_stats.byPlayers;
        else
            ++g_stats.byBots;
        return c;
    }

    void End(std::unordered_map<std::string, Conversation>::iterator it, bool natural, const char* why)
    {
        Conversation& c = it->second;
        if (natural)
            ++g_stats.woundDown;
        else
            ++g_stats.fizzled;

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Conversation {} in {} ended ({}) after {} s.",
                     c.id, c.key, why,
                     std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - c.created).count());

        // Bots do not open another one here straight away.
        if (g_ConversationCooldownSeconds > 0)
            g_cooldownUntil[c.key] = Clock::now() + std::chrono::seconds(g_ConversationCooldownSeconds);

        g_convs.erase(it);
        Recount();
    }

    bool CoolingDown(const std::string& key, Clock::time_point now)
    {
        auto it = g_cooldownUntil.find(key);
        if (it == g_cooldownUntil.end())
            return false;
        if (now >= it->second)
        {
            g_cooldownUntil.erase(it);
            return false;
        }
        return true;
    }

    std::string WhereText(const Conversation& c)
    {
        switch (c.source)
        {
            case SRC_SAY_LOCAL:
                return "/say, standing near each other";
            case SRC_GENERAL_LOCAL:
                if (c.channelId == uint32(ChatChannelId::TRADE))
                    return "Trade chat";
                return "General chat";
            default:
                return "chat";
        }
    }

    std::string PlaceName(ChatChannelSourceLocal source, uint32_t channelId)
    {
        if (source == SRC_SAY_LOCAL)
            return "/say";
        return channelId == uint32(ChatChannelId::TRADE) ? "Trade chat" : "General chat";
    }

    // Bots that could pipe up: in the channel, or within /say range of the
    // conversation, and not already in it.
    std::vector<Player*> JoinPool(Conversation& c, std::vector<Player*>& seatsOnline)
    {
        std::vector<Player*> pool;
        seatsOnline.clear();

        Player* anchor = nullptr;
        for (const Seat& s : c.seats)
        {
            if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(s.guid)))
            {
                if (!p->IsInWorld())
                    continue;
                seatsOnline.push_back(p);
                if (!anchor || s.guid == c.lastGuid)
                    anchor = p;
            }
        }
        if (!anchor)
            return pool;

        auto usable = [&](Player* p)
        {
            return p && p->IsInWorld() && !p->IsBeingTeleported() && p->IsAlive() &&
                   !(g_DisableRepliesInCombat && p->IsInCombat()) &&
                   OllamaIsBotPlayer(p) && uint32(p->GetTeamId()) == c.teamId &&
                   !FindSeat(c, p->GetGUID().GetRawValue());
        };

        if (c.source == SRC_SAY_LOCAL)
        {
            std::list<Player*> found;
            Acore::AnyPlayerInObjectRangeCheck check(anchor, g_SayDistance, true, true);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(anchor, found, check);
            Cell::VisitObjects(anchor, searcher, g_SayDistance);
            for (Player* p : found)
                if (usable(p))
                    pool.push_back(p);
            return pool;
        }

        ChannelMgr* mgr = ChannelMgr::forTeam(anchor->GetTeamId());
        Channel* channel = mgr ? mgr->GetChannel(c.channelName, anchor) : nullptr;
        if (!channel)
            return pool;

        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            Player* p = entry.second;
            if (!usable(p) || p->GetZoneId() != anchor->GetZoneId())
                continue;
            if (!OllamaIsOnChannel(channel, p->GetGUID()))
                continue;
            pool.push_back(p);
            if (pool.size() >= 40)
                break;   // plenty to pick a fresh voice from
        }
        return pool;
    }

    // A real player near `bot`, described, for a /say turn to remark on.
    std::string Observation(Player* bot)
    {
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, g_SayDistance, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, g_SayDistance);

        std::vector<Player*> people;
        for (Player* p : found)
            if (p && p != bot && OllamaIsRealPlayer(p) && !p->IsGameMaster())
                people.push_back(p);
        if (people.empty())
            return "";

        Player* subject = people[RandomBetween(0, uint32(people.size() - 1))];
        return Presence_Describe(subject, bot, nullptr);
    }

    std::string BuildHint(Conversation& c, Player* next, bool joiner, bool named)
    {
        std::vector<std::string> others;
        for (const Seat& s : c.seats)
            if (s.guid != next->GetGUID().GetRawValue() && (s.spoke || s.guid == c.lastGuid))
                others.push_back(s.name);
        if (!c.lastIsBot && !c.lastName.empty() &&
            std::find(others.begin(), others.end(), c.lastName) == others.end())
            others.push_back(c.lastName);
        const std::string othersText = others.empty() ? std::string("a few others")
                                                      : CrowdText::JoinNames(others);

        const std::string about = c.premise.empty() ? std::string() : " about " + c.premise;

        const std::string& templ = c.winding ? g_ConversationHintWindDown
                                 : joiner    ? g_ConversationHintJoin
                                             : g_ConversationHintTurn;

        std::string hint = SafeFormat(templ,
                                      fmt::arg("others", othersText),
                                      fmt::arg("where", WhereText(c)),
                                      fmt::arg("about", about),
                                      fmt::arg("premise", c.premise),
                                      fmt::arg("last_speaker", c.lastName),
                                      fmt::arg("pass_token", g_ContextPassToken));
        if (hint == "[Format Error]")
            hint.clear();

        if (named && !g_ContextHintAddressed.empty())
            hint += (hint.empty() ? "" : " ") + g_ContextHintAddressed;

        if (c.source == SRC_SAY_LOCAL && !c.winding && g_ConversationObserveChance > 0 &&
            !g_ConversationHintObserve.empty() && RandomBetween(0, 99) < g_ConversationObserveChance)
        {
            const std::string seen = Observation(next);
            if (!seen.empty())
            {
                const std::string extra = SafeFormat(g_ConversationHintObserve, fmt::arg("observation", seen));
                if (extra != "[Format Error]")
                    hint += (hint.empty() ? "" : " ") + extra;
            }
        }
        return hint;
    }

    // Pick who speaks and send their turn. False when nobody could be asked.
    bool FireTurn(Conversation& c, Clock::time_point now)
    {
        ConvLogic::PickInput in;
        in.lastSpeaker  = c.lastGuid;
        in.beforeLast   = c.prevGuid;
        in.lastWasHuman = !c.lastIsBot;
        in.humanAddressee = c.lastIsBot ? 0 : c.humanAddressee;
        in.maxMisses    = 2;
        in.nowMs        = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        size_t bestPos = std::string::npos;
        for (const Seat& s : c.seats)
        {
            ConvLogic::Seat seat;
            seat.guid   = s.guid;
            seat.lines  = s.lines;
            seat.misses = s.misses;
            seat.lastMs = s.spoke ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                        s.lastSpoke.time_since_epoch()).count()
                                  : -1;
            in.seats.push_back(seat);

            const size_t pos = OllamaFindBotNameMention(c.lastText, s.name);
            if (pos != std::string::npos && s.guid != c.lastGuid && (bestPos == std::string::npos || pos < bestPos))
            {
                bestPos  = pos;
                in.named = s.guid;
            }
        }

        // Fewer than two people in it is not a conversation yet: bring
        // someone in. Early on, newcomers are more likely.
        const bool roomToJoin = c.seats.size() < g_ConversationMaxParticipants && !c.winding;
        in.canJoin = roomToJoin;
        in.joinChancePct = c.seats.size() < 2 ? 100 : g_ConversationJoinChance;

        ConvLogic::PickResult pick = ConvLogic::PickNextSpeaker(in, Rng());

        Player* next = nullptr;
        bool joiner = false;

        if (pick.join)
        {
            std::vector<Player*> seatsOnline;
            std::vector<Player*> pool = JoinPool(c, seatsOnline);
            if (!pool.empty())
            {
                std::vector<Player*> voice = Crowd_PickVoices(pool, seatsOnline, 1);
                if (!voice.empty() && AddSeat(c, voice.front()))
                {
                    next = voice.front();
                    joiner = true;
                    ++g_stats.joins;
                }
            }
            if (!next)
            {
                // Nobody new to bring in: one of those already here, then.
                in.canJoin = false;
                pick = ConvLogic::PickNextSpeaker(in, Rng());
            }
        }

        if (!next && pick.guid)
            next = ObjectAccessor::FindConnectedPlayer(ObjectGuid(pick.guid));
        if (!next || !next->IsInWorld())
        {
            if (pick.guid)
                if (Seat* s = FindSeat(c, pick.guid))
                    ++s->misses;
            return false;
        }

        // Talking to the person who said the last line needs them still here.
        Player* lastSpeaker = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.lastGuid));
        if (!lastSpeaker || !lastSpeaker->IsInWorld())
            return false;

        // The place is at its per-minute ceiling (a crowd just answered a
        // player, say): wait a little rather than lose the turn.
        if (!Governor_CanSendPaced(next->GetGUID(), c.scopeKey, false))
        {
            c.nextTurnAt = now + std::chrono::milliseconds(RandomBetween(5000, 10000));
            return true;
        }

        const bool named = in.named == next->GetGUID().GetRawValue();

        CrowdPlan plan;
        plan.id             = Crowd_NextPlanId();
        plan.key            = c.key;
        plan.scopeKey       = c.scopeKey;
        plan.source         = c.source;
        plan.channelName    = c.channelName;
        plan.channelId      = c.channelId;
        plan.speakerGuid    = c.lastGuid;
        plan.speakerIsBot   = c.lastIsBot;
        plan.speakerName    = c.lastName;
        plan.message        = c.lastText;
        plan.messageSeq     = c.lastSeq;
        plan.kind           = CrowdLineKind::Conversation;
        plan.listeners      = static_cast<uint32_t>(c.seats.size());
        plan.conversationId = c.id;
        plan.closed         = true;

        CrowdSlot slot;
        slot.botGuid    = next->GetGUID().GetRawValue();
        slot.chainDepth = 1;
        slot.hint       = BuildHint(c, next, joiner, named);
        plan.slots.push_back(std::move(slot));

        c.turnPlanId  = plan.id;
        c.turnGuid    = next->GetGUID().GetRawValue();
        c.turnLanded  = false;
        c.turnFiredAt = now;
        if (c.winding && c.windLeft > 0)
            --c.windLeft;
        ++g_stats.turns;

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Conversation {} in {}: {}{} answers {}{}.",
                     c.id, c.key, next->GetName(), joiner ? " (joining)" : "", c.lastName,
                     c.winding ? " (winding down)" : "");

        // If the voice cannot be asked after all, the plan is gone straight
        // away and the next update counts the miss.
        Crowd_Start(std::move(plan), 0);
        return true;
    }
}

// --------------------------------------------------------------------------

bool Conversation_PlaceAllowed(ChatChannelSourceLocal source, uint32_t channelId)
{
    if (!g_ConversationEnable || !g_CrowdEnable || !g_ContextEnable)
        return false;

    switch (source)
    {
        case SRC_SAY_LOCAL:
            return g_ConversationAllowSay && !g_DisableForSayYell;
        case SRC_GENERAL_LOCAL:
            if (g_DisableForCustomChannels)
                return false;
            return channelId == uint32(ChatChannelId::GENERAL) ||
                   (g_ConversationAllowTrade && channelId == uint32(ChatChannelId::TRADE));
        default:
            return false;
    }
}

bool Conversation_IsActive(const std::string& key)
{
    return g_convs.find(key) != g_convs.end();
}

bool Conversation_IsActiveNear(const std::string& key, Player* speaker)
{
    auto it = g_convs.find(key);
    return it != g_convs.end() && Hears(it->second, speaker);
}

bool Conversation_CanOpen(const std::string& key)
{
    if (!g_ConversationEnable || g_convs.size() >= g_ConversationMaxActive)
        return false;
    if (g_convs.count(key))
        return false;
    return !CoolingDown(key, Clock::now());
}

void Conversation_OnPlayerCrowd(Player* player, const std::string& msg, uint64_t seq,
                                ChatChannelSourceLocal source, Channel* channel,
                                const std::string& scopeKey, const std::string& key,
                                const std::vector<Player*>& voices, bool inHub, bool mayStart)
{
    if (!player)
        return;
    const uint32_t channelId = channel ? channel->GetChannelId() : 0;
    if (!Conversation_PlaceAllowed(source, channelId))
        return;

    auto it = g_convs.find(key);
    if (it != g_convs.end())
    {
        // Already talking here: whoever answers the person is in it now --
        // unless the person is out of earshot of it, across the zone in /say.
        if (Hears(it->second, player))
            for (Player* v : voices)
                AddSeat(it->second, v);
        return;
    }

    if (!mayStart || voices.empty() || g_convs.size() >= g_ConversationMaxActive)
        return;

    // Somewhere quiet, one bot answering is a chat with that bot, not a
    // conversation for others to carry on.
    if (voices.size() < 2 && !inHub)
        return;

    uint32_t chance = g_ConversationFromPlayerChance;
    if (inHub)
        chance = std::min<uint32_t>(100, chance * g_CityConversationBoost);
    if (RandomBetween(0, 99) >= chance)
        return;

    const auto now = Clock::now();
    Conversation& c = Create(key, scopeKey, source, channel ? channel->GetName() : std::string(),
                             channelId, uint32(player->GetTeamId()), true, now);
    for (Player* v : voices)
        AddSeat(c, v);

    c.lastGuid  = player->GetGUID().GetRawValue();
    c.lastName  = player->GetName();
    c.lastText  = msg;
    c.lastIsBot = false;
    c.lastSeq   = seq;
    Anchor(c, player);
    // The crowd answering the line goes first; turns start once it is done.
    ScheduleNext(c, now);

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Conversation {} starts in {} from {}'s line ({} min planned).",
                 c.id, key, player->GetName(),
                 std::chrono::duration_cast<std::chrono::minutes>(c.endAt - now).count());
}

void Conversation_OnBotLineCrowd(Player* bot, const std::string& msg, uint64_t seq,
                                 ChatChannelSourceLocal source, Channel* channel,
                                 const std::string& scopeKey, const std::string& key,
                                 const std::vector<Player*>& voices, bool inHub)
{
    if (!bot || voices.empty())
        return;
    const uint32_t channelId = channel ? channel->GetChannelId() : 0;
    if (!Conversation_PlaceAllowed(source, channelId) || !Conversation_CanOpen(key))
        return;

    uint32_t chance = g_ConversationFromBotLineChance;
    if (inHub)
        chance = std::min<uint32_t>(100, chance * g_CityConversationBoost);
    if (RandomBetween(0, 99) >= chance)
        return;

    const auto now = Clock::now();
    Conversation& c = Create(key, scopeKey, source, channel ? channel->GetName() : std::string(),
                             channelId, uint32(bot->GetTeamId()), false, now);
    AddSeat(c, bot);
    if (Seat* s = FindSeat(c, bot->GetGUID().GetRawValue()))
    {
        s->spoke = true;
        s->lines = 1;
        s->lastSpoke = now;
    }
    for (Player* v : voices)
        AddSeat(c, v);

    c.lastGuid  = bot->GetGUID().GetRawValue();
    c.lastName  = bot->GetName();
    c.lastText  = msg;
    c.lastIsBot = true;
    c.lastSeq   = seq;
    Anchor(c, bot);
    ScheduleNext(c, now);

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Conversation {} starts in {} from {}'s line.",
                 c.id, key, bot->GetName());
}

bool Conversation_BeginWithOpener(Player* starter, ChatChannelSourceLocal source,
                                  const std::string& channelName, uint32_t channelId,
                                  const std::string& scopeKey, const std::string& key,
                                  const std::string& premise, const std::vector<Player*>& partners)
{
    if (!starter || !Conversation_PlaceAllowed(source, channelId) || !Conversation_CanOpen(key))
        return false;

    const auto now = Clock::now();
    Conversation& c = Create(key, scopeKey, source, channelName, channelId,
                             uint32(starter->GetTeamId()), false, now);
    c.premise = premise;
    AddSeat(c, starter);
    for (Player* p : partners)
        AddSeat(c, p);
    Anchor(c, starter);

    c.awaitingOpener = true;
    c.openerGuid     = starter->GetGUID().GetRawValue();
    c.openerDeadline = now + std::chrono::seconds(std::max<uint32_t>(45, g_CrowdSlotTimeoutSeconds + 15));

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Conversation {} opens in {}: {} brings up {} ({}).",
                 c.id, key, starter->GetName(), premise, PlaceName(source, channelId));
    return true;
}

void Conversation_NoteLine(const std::string& key, Player* speaker, bool isBot,
                           const std::string& text, uint64_t seq,
                           uint64_t crowdPlanId, uint64_t addresseeGuid)
{
    if (!speaker || g_convs.empty())
        return;

    auto it = g_convs.find(key);
    if (it == g_convs.end())
        return;

    Conversation& c = it->second;

    // Said out of earshot of it: someone else's /say across the zone.
    if (!Hears(c, speaker))
        return;

    const auto now = Clock::now();
    const uint64_t guid = speaker->GetGUID().GetRawValue();
    Anchor(c, speaker);

    c.prevGuid  = c.lastGuid;
    c.lastGuid  = guid;
    c.lastName  = speaker->GetName();
    c.lastText  = text;
    c.lastIsBot = isBot;
    c.lastSeq   = seq;
    c.humanAddressee = isBot ? 0 : addresseeGuid;
    c.missesInRow = 0;
    ++g_stats.lines;

    if (isBot)
    {
        Seat* s = FindSeat(c, guid);
        if (!s && AddSeat(c, speaker))
        {
            s = FindSeat(c, guid);
            ++g_stats.joins;
        }
        if (s)
        {
            ++s->lines;
            s->misses    = 0;
            s->lastSpoke = now;
            s->spoke     = true;
        }

        if (crowdPlanId != 0 && crowdPlanId == c.turnPlanId)
            c.turnLanded = true;

        if (c.awaitingOpener && guid == c.openerGuid)
            c.awaitingOpener = false;
    }
    else
    {
        // A person joined in: the chat gets a second wind.
        const auto more = now + std::chrono::seconds(RandomBetween(90, 240));
        if (c.endAt < more)
            c.endAt = more;
        c.winding  = false;
        c.windLeft = 0;
    }

    ScheduleNext(c, now);
}

void Conversation_OpenerLost(const std::string& key, uint64_t botGuid)
{
    auto it = g_convs.find(key);
    if (it == g_convs.end() || !it->second.awaitingOpener || it->second.openerGuid != botGuid)
        return;

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Conversation {} in {} never opened: the opening line did not go out.",
                 it->second.id, key);

    // It never really started: no count, and no cooldown for the place.
    if (g_stats.started > 0)
        --g_stats.started;
    if (g_stats.byBots > 0)
        --g_stats.byBots;
    g_convs.erase(it);
    Recount();
}

void Conversation_Update()
{
    ApplyForgets();

    if (g_convs.empty())
        return;

    if (!g_ConversationEnable || !g_CrowdEnable || !g_ContextEnable)
    {
        g_convs.clear();
        Recount();
        return;
    }

    const auto now = Clock::now();

    for (auto it = g_convs.begin(); it != g_convs.end(); )
    {
        Conversation& c = it->second;
        auto current = it++;   // End() erases `current`; `it` stays valid

        // Running long past its time: something kept it from closing.
        if (now > c.endAt + std::chrono::minutes(4))
        {
            End(current, true, "out of time");
            continue;
        }

        if (c.awaitingOpener)
        {
            // A lost opener is normally reported straight away; this is the
            // backstop. Either way the place is free again with no cooldown.
            if (now > c.openerDeadline)
            {
                const std::string key = c.key;   // `c` goes with it
                Conversation_OpenerLost(key, c.openerGuid);
            }
            continue;
        }

        if (c.turnPlanId != 0)
        {
            if (Crowd_FindById(c.turnPlanId))
                continue;   // still typing

            if (!c.turnLanded)
            {
                if (Crowd_Find(c.key))
                {
                    // A person's line took the place over before this turn was
                    // said: their crowd goes first, and nobody missed a turn.
                }
                else
                {
                    ++c.missesInRow;
                    if (Seat* s = FindSeat(c, c.turnGuid))
                        ++s->misses;

                    // Timed out rather than refused: the line may still turn
                    // up, so leave it room before someone else answers the
                    // same thing.
                    const auto timeout = std::chrono::seconds(std::max<uint32_t>(4, g_CrowdSlotTimeoutSeconds) - 2);
                    if (now - c.turnFiredAt >= timeout)
                        ScheduleNext(c, now);
                    else
                        c.nextTurnAt = now + std::chrono::milliseconds(RandomBetween(1000, 3000));
                }
            }
            c.turnPlanId = 0;
            c.turnGuid   = 0;

            if (c.winding && c.windLeft == 0 && c.turnLanded)
            {
                End(current, true, "wound down");
                continue;
            }
        }

        if (c.missesInRow >= g_ConversationMaxMisses)
        {
            End(current, false, "nobody carried it on");
            continue;
        }

        // Someone else's crowd is answering here (a player spoke): it goes
        // first, and the conversation picks up after it.
        if (Crowd_Find(c.key))
        {
            if (c.nextTurnAt < now + std::chrono::seconds(1))
                c.nextTurnAt = now + std::chrono::milliseconds(RandomBetween(1500, 4000));
            continue;
        }

        if (now < c.nextTurnAt)
            continue;

        if (!c.winding && now >= c.endAt)
        {
            c.winding  = true;
            c.windLeft = RandomBetween(1, 2);
        }
        if (c.winding && c.windLeft == 0)
        {
            End(current, true, "wound down");
            continue;
        }

        if (!FireTurn(c, now))
        {
            ++c.missesInRow;
            c.nextTurnAt = now + std::chrono::milliseconds(RandomBetween(2000, 5000));
        }
    }

    // Old cooldowns.
    if (g_cooldownUntil.size() > 64)
    {
        for (auto it = g_cooldownUntil.begin(); it != g_cooldownUntil.end(); )
        {
            if (now >= it->second)
                it = g_cooldownUntil.erase(it);
            else
                ++it;
        }
    }
}

void Conversation_ForgetPlayer(uint64_t guid)
{
    if (g_convCount.load(std::memory_order_relaxed) == 0)
        return;   // nobody is sitting in anything

    std::lock_guard<std::mutex> lock(g_forgetMutex);
    // The world tick has not run for a long while (the module is off): a
    // seat left behind only ever costs a missed turn.
    if (g_forget.size() >= 4096)
        g_forget.clear();
    g_forget.push_back(guid);
    g_forgetPending = true;
}

void Conversation_Reset()
{
    g_convs.clear();
    g_cooldownUntil.clear();
    Recount();
    std::lock_guard<std::mutex> lock(g_forgetMutex);
    g_forget.clear();
}

ConversationStats Conversation_GetStats()
{
    ConversationStats out = g_stats;
    out.active = static_cast<uint32_t>(g_convs.size());
    return out;
}
