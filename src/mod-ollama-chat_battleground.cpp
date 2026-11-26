#include "mod-ollama-chat_battleground.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_journal.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundEY.h"
#include "BattlegroundMgr.h"
#include "BattlegroundWS.h"
#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "World.h"
#include "WorldPacket.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr size_t   kMaxNodes       = 5;
    constexpr auto     kLookEvery      = std::chrono::seconds(1);
    constexpr auto     kTypedGap       = std::chrono::seconds(10);   // between two typed callouts of a team
    constexpr auto     kClaimFor       = std::chrono::seconds(45);   // how long a line is about what just happened
    constexpr auto     kForgetAfter    = std::chrono::minutes(5);    // a battleground nobody playing is in any more
    constexpr uint32_t kCarrierSaysPct = 40;                         // how often the one with the flag says so itself

    // The battlegrounds whose state the core lets a module read.
    enum class Kind : uint8_t { Other, Warsong, Arathi, Eye };

    // Who holds a base, or is taking it: Arathi Basin's own numbers
    // (BG_AB_NodeStatus), which Eye of the Storm's towers are read into too.
    enum Hold : uint8_t { kNobody = 0, kAlliance = 1, kHorde = 2, kAllianceTaking = 3, kHordeTaking = 4 };
    static_assert(int(BG_AB_NODE_STATE_NEUTRAL) == int(kNobody) &&
                  int(BG_AB_NODE_STATE_ALLY_OCCUPIED) == int(kAlliance) &&
                  int(BG_AB_NODE_STATE_HORDE_OCCUPIED) == int(kHorde) &&
                  int(BG_AB_NODE_STATE_ALLY_CONTESTED) == int(kAllianceTaking) &&
                  int(BG_AB_NODE_STATE_HORDE_CONTESTED) == int(kHordeTaking));
    static_assert(size_t(BG_AB_DYNAMIC_NODES_COUNT) <= kMaxNodes && size_t(EY_POINTS_MAX) <= kMaxNodes);

    // Where a flag is: the same numbers in both battlegrounds that have one.
    enum FlagAt : uint8_t { kAtHome = 1, kCarried = 2, kDropped = 3 };
    static_assert(int(BG_WS_FLAG_STATE_ON_BASE) == int(kAtHome) && int(BG_WS_FLAG_STATE_ON_PLAYER) == int(kCarried) &&
                  int(BG_WS_FLAG_STATE_ON_GROUND) == int(kDropped));
    static_assert(int(BG_EY_FLAG_STATE_ON_BASE) == int(kAtHome) && int(BG_EY_FLAG_STATE_ON_PLAYER) == int(kCarried) &&
                  int(BG_EY_FLAG_STATE_ON_GROUND) == int(kDropped));

    // Where each team's flag stands in Warsong Gulch, [TEAM_ALLIANCE] and
    // [TEAM_HORDE]: the core's own spawns (BattlegroundWS::SetupBattleground),
    // which it does not name.
    constexpr float kWarsongStand[2][2] = { { 1540.423f, 1481.325f }, { 916.0226f, 1434.405f } };

    struct Flag
    {
        uint8_t  at      = kAtHome;
        uint64_t carrier = 0;
    };

    // What one look at a battleground read.
    struct Seen
    {
        BattlegroundStatus status = STATUS_NONE;
        uint32_t score[2] = { 0, 0 };
        Flag     flag[2];                               // Warsong: by whose flag it is. Eye: [0], the one in the middle
        uint8_t  node[kMaxNodes] = { 0, 0, 0, 0, 0 };   // a Hold each
        size_t   nodes = 0;
    };

    // A team as a look finds it.
    struct Side
    {
        std::vector<Player*> bots;
        std::vector<Player*> people;
    };

    // What a line was written on, to be looked at again when it goes out.
    enum class So : uint8_t { Waiting, Going, FlagWith, FlagHome, Score, Node };
    struct Claim
    {
        uint32_t instance = 0;
        So       what  = So::Going;
        uint8_t  index = 0;                 // which flag, or which base
        uint8_t  value = 0;                 // Node: the Hold it has to be
        uint64_t who   = 0;                 // FlagWith: its carrier
        uint32_t score[2] = { 0, 0 };       // Score
        Clock::time_point until{};
    };

    struct Match
    {
        uint32_t    instance = 0;
        const void* self = nullptr;                       // which battleground this is; never followed
        Kind        kind = Kind::Other;
        std::string name;
        Seen        last;
        bool        looked = false;                       // the first look only remembers
        bool        over   = false;                       // its end has been seen to
        uint8_t     lastHeld[kMaxNodes] = { 0, 0, 0, 0, 0 };   // who last held each base outright
        uint8_t     eyeCarrierTeam = TEAM_NEUTRAL;        // whose the one carrying the Eye's flag is
        int32_t     prepAt = 0;                           // the line before the start, with this many ms to go
        bool        prepSaid[2] = { false, false };
        bool        nearEnd[2]  = { false, false };       // this team's score getting there has been said
        uint64_t    lastSpeaker[2] = { 0, 0 };
        Clock::time_point lastLine[2]{};
        Clock::time_point lastTyped[2]{};
        Clock::time_point incoming[2][kMaxNodes]{};
        Clock::time_point seen{};                         // someone playing was last in it
        Clock::time_point startedAt{};                    // the gates opened, when a look saw it
        bool        startSeen = false;
    };

    // A typed line to go out a moment later, whether or not the one who typed
    // it is still there: "gg", and gone.
    struct Later
    {
        Clock::time_point due{};
        uint32_t    instance = 0;
        const void* self = nullptr;   // which battleground, as Match::self
        TeamId      team = TEAM_NEUTRAL;
        uint64_t    from = 0;
        std::string name;
        std::string text;
    };

    // What may be said of something that happened, the weightiest first.
    struct Event
    {
        int         rank = 0;
        uint32_t    chance = 0;
        std::string ask;
        std::string basis;
        Claim       claim;
        uint64_t    must = 0;   // the only one who can say it (who picked the flag up), or 0
    };

    struct Place
    {
        std::string name;   // "the Lumber Mill"
        std::string call;   // "LM"
    };

    // All the world thread's.
    std::unordered_map<uint32_t, Match> g_matches;   // by instance
    std::unordered_map<uint64_t, Claim> g_claims;    // by OllamaChatRequest::battleground
    std::vector<Later>                  g_later;
    uint64_t                            g_nextClaim = 0;
    Clock::time_point                   g_nextLook{};
    BattlegroundStats                   g_stats;

    // Settings.
    bool     g_enable = true;
    uint32_t g_gapSeconds = 20;
    uint32_t g_holdSeconds = 10;
    uint32_t g_prepMin = 10, g_prepMax = 45;
    uint32_t g_startChance = 70, g_flagTakenChance = 70, g_flagOursChance = 40, g_flagReturnedChance = 35;
    uint32_t g_captureChance = 70, g_nodeAttackChance = 65, g_nodeTakenChance = 25, g_nodeLostChance = 45;
    uint32_t g_nearEndChance = 60, g_incomingChance = 75;
    uint32_t g_incomingMin = 2, g_incomingSeconds = 45;
    float    g_incomingYards = 45.0f;
    uint32_t g_ggChance = 60, g_ggMax = 3, g_ggDelayMin = 1, g_ggDelayMax = 7;
    bool     g_journal = true;

    std::string g_linePrompt, g_replyText;
    std::string g_rulesWarsong, g_rulesArathi, g_rulesEye, g_rulesAlterac, g_rulesIsle, g_rulesStrand;
    std::string g_waitingText, g_goingText, g_wonText, g_lostText, g_drawText;
    std::string g_warsongState, g_warsongClock, g_arathiState, g_eyeState;
    std::string g_flagHomeText, g_flagEnemyText, g_flagFriendText, g_flagSelfText, g_flagDroppedText;
    std::string g_flagCarriedText, g_eyeFlagHomeText;
    std::vector<std::string> g_whereTexts;   // five of them, from the team's own base to the enemy's
    std::string g_holdText, g_holdNoneText, g_enemyHoldText, g_takingText, g_enemyTakingText;
    std::string g_startText, g_flagTakenText, g_flagOursText, g_flagMineText, g_flagReturnedText;
    std::string g_captureUsText, g_captureThemText;
    std::string g_nodeAttackText, g_nodeTakenText, g_nodeSavedText, g_nodeLostText, g_towerLostText;
    std::string g_eyeFlagEnemyText, g_eyeFlagOursText, g_eyeFlagMineText, g_eyeCaptureUsText, g_eyeCaptureThemText;
    std::string g_nearEndWinText, g_nearEndLoseText;
    std::vector<std::string> g_incomingLines, g_ggWinLines, g_ggLossLines, g_ggDrawLines;
    std::vector<Place> g_arathiBases, g_eyeTowers;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    TeamId Other(TeamId team)
    {
        return team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    }

    uint8_t HeldBy(TeamId team)
    {
        return team == TEAM_ALLIANCE ? kAlliance : kHorde;
    }

    uint8_t TakenBy(TeamId team)
    {
        return team == TEAM_ALLIANCE ? kAllianceTaking : kHordeTaking;
    }

    const char* SideName(TeamId team)
    {
        return team == TEAM_ALLIANCE ? "Alliance" : "Horde";
    }

    template <typename... Args>
    std::string Fill(const std::string& templ, Args&&... args)
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ, std::forward<Args>(args)...);
        return out == "[Format Error]" ? std::string() : out;
    }

    // What a line is asked to say: "{~Cheer|Say well done}" picked first, one
    // each time, then the slots filled (the other way round, the braces of a
    // choice are no slot fmt knows).
    template <typename... Args>
    std::string Ask(const std::string& templ, Args&&... args)
    {
        return Fill(TextRules::ExpandChoices(templ, Rng()), std::forward<Args>(args)...);
    }

    // "A, B and C".
    std::string Joined(const std::vector<std::string>& names)
    {
        std::string out;
        for (size_t i = 0; i < names.size(); ++i)
            out += (i ? (i + 1 == names.size() ? " and " : ", ") : "") + names[i];
        return out;
    }

    Kind KindOf(Battleground* bg)
    {
        switch (bg->GetBgTypeID(true))
        {
            case BATTLEGROUND_WS: return Kind::Warsong;
            case BATTLEGROUND_AB: return Kind::Arathi;
            case BATTLEGROUND_EY: return Kind::Eye;
            default:              return Kind::Other;
        }
    }

    // What wins it, as the core works it out for each of them.
    uint32_t Target(Kind kind)
    {
        uint32_t set = 0;
        switch (kind)
        {
            case Kind::Warsong:
                set = sWorld->getIntConfig(CONFIG_BATTLEGROUND_WARSONG_FLAGS);
                return set ? set : uint32_t(BG_WS_MAX_TEAM_SCORE);
            case Kind::Arathi:
                set = sWorld->getIntConfig(CONFIG_BATTLEGROUND_ARATHI_CAPTUREPOINTS);
                return set ? set : uint32_t(BG_AB_MAX_TEAM_SCORE);
            case Kind::Eye:
                set = sWorld->getIntConfig(CONFIG_BATTLEGROUND_EYEOFTHESTORM_CAPTUREPOINTS);
                return set ? set : uint32_t(BG_EY_MAX_TEAM_SCORE);
            default:
                return 0;
        }
    }

    const std::vector<Place>& PlacesOf(Kind kind)
    {
        static const std::vector<Place> none;
        return kind == Kind::Arathi ? g_arathiBases : kind == Kind::Eye ? g_eyeTowers : none;
    }

    // Where base `n` is, when the core says. False for one it does not.
    bool WhereIs(Kind kind, size_t n, float& x, float& y, float& z)
    {
        const float* at = nullptr;
        if (kind == Kind::Arathi && n < BG_AB_DYNAMIC_NODES_COUNT)
            at = BG_AB_NodePositions[n];
        else if (kind == Kind::Eye && n < EY_POINTS_MAX)
            at = BG_EY_TriggerPositions[n];
        if (!at)
            return false;
        x = at[0];
        y = at[1];
        z = at[2];
        return true;
    }

    Seen Read(Battleground* bg, Kind kind)
    {
        Seen s;
        s.status = bg->GetStatus();
        if (kind == Kind::Other)
            return s;
        s.score[TEAM_ALLIANCE] = bg->GetTeamScore(TEAM_ALLIANCE);
        s.score[TEAM_HORDE]    = bg->GetTeamScore(TEAM_HORDE);
        if (BattlegroundWS* ws = bg->ToBattlegroundWS())
        {
            for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            {
                s.flag[team].at      = ws->GetFlagState(team);
                s.flag[team].carrier = ws->GetFlagPickerGUID(team).GetRawValue();
            }
        }
        else if (BattlegroundAB* ab = bg->ToBattlegroundAB())
        {
            s.nodes = BG_AB_DYNAMIC_NODES_COUNT;
            for (size_t n = 0; n < s.nodes; ++n)
                s.node[n] = ab->GetCapturePointInfo(uint32(n))._state;
        }
        else if (BattlegroundEY* ey = bg->ToBattlegroundEY())
        {
            s.nodes = EY_POINTS_MAX;
            for (size_t n = 0; n < s.nodes; ++n)
            {
                const TeamId owner = ey->GetCapturePointInfo(uint32(n))._ownerTeamId;
                s.node[n] = owner == TEAM_ALLIANCE ? kAlliance : owner == TEAM_HORDE ? kHorde : kNobody;
            }
            s.flag[0].at      = ey->GetFlagState();
            s.flag[0].carrier = ey->GetFlagPickerGUID().GetRawValue();
        }
        return s;
    }

    // The battleground `who` is in, when it is one this module speaks in.
    Battleground* Of(Player* who)
    {
        if (!who || !who->IsInWorld() || !who->InBattleground() || who->InArena())
            return nullptr;
        Battleground* bg = who->GetBattleground();
        return bg && bg->isBattleground() ? bg : nullptr;
    }

    bool OnATeam(Player* who)
    {
        const TeamId team = who->GetBgTeamId();
        return team == TEAM_ALLIANCE || team == TEAM_HORDE;
    }

    void Sides(Battleground* bg, Side sides[2])
    {
        for (auto const& [guid, player] : bg->GetPlayers())
        {
            if (!player || !player->IsInWorld() || player->GetBattlegroundId() != bg->GetInstanceID() ||
                !OnATeam(player))
                continue;
            Side& side = sides[player->GetBgTeamId()];
            (OllamaIsBotPlayer(player) ? side.bots : side.people).push_back(player);
        }
    }

    std::string ScopeOf(Group* group)
    {
        return Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_RAID_LOCAL], 0, std::string(), 0,
                                     group->GetGUID().GetCounter());
    }

    // Minutes since the gates opened: from the look that saw it start. A match
    // joined already going has only the core's clock, which runs from when the
    // battleground was made -- the wait before the start, and the moments
    // before anyone came in, included -- so it can say a minute too many.
    uint32_t MinutesGoing(Battleground* bg)
    {
        if (const auto it = g_matches.find(bg->GetInstanceID());
            it != g_matches.end() && it->second.self == bg && it->second.startSeen)
            return uint32_t(std::chrono::duration_cast<std::chrono::minutes>(Clock::now() - it->second.startedAt)
                                .count());
        const uint32_t wait = bg->GetBgTypeID(true) == BATTLEGROUND_SA
                                  ? 120 * IN_MILLISECONDS
                                  : sWorld->getIntConfig(CONFIG_BATTLEGROUND_PREP_TIME) * IN_MILLISECONDS;
        const uint32_t run = bg->GetStartTime();
        return run > wait ? (run - wait) / (MINUTE * IN_MILLISECONDS) : 0;
    }

    std::string RulesFor(Battleground* bg, Kind kind)
    {
        const std::string* templ = nullptr;
        switch (bg->GetBgTypeID(true))
        {
            case BATTLEGROUND_WS: templ = &g_rulesWarsong; break;
            case BATTLEGROUND_AB: templ = &g_rulesArathi;  break;
            case BATTLEGROUND_EY: templ = &g_rulesEye;     break;
            case BATTLEGROUND_AV: templ = &g_rulesAlterac; break;
            case BATTLEGROUND_IC: templ = &g_rulesIsle;    break;
            case BATTLEGROUND_SA: templ = &g_rulesStrand;  break;
            default:              return {};
        }
        return Fill(*templ, fmt::arg("size", bg->GetMaxPlayersPerTeam()), fmt::arg("target", Target(kind)));
    }

    // ", who is in your base": where on Warsong Gulch's field `who` is, for
    // someone of `us`, by how far it is from each team's flag stand.
    std::string WhereOnField(Player* who, TeamId us)
    {
        if (g_whereTexts.size() != 5)
            return {};
        const float own   = who->GetExactDist2d(kWarsongStand[us][0], kWarsongStand[us][1]);
        const float enemy = who->GetExactDist2d(kWarsongStand[Other(us)][0], kWarsongStand[Other(us)][1]);
        if (own + enemy <= 0.0f)
            return {};
        const float along = own / (own + enemy);
        const size_t part = along < 0.18f ? 0 : along < 0.42f ? 1 : along <= 0.58f ? 2 : along <= 0.82f ? 3 : 4;
        return ", " + g_whereTexts[part];
    }

    // Where a flag is, in words, for `bot` of team `us`.
    std::string FlagWords(const Flag& flag, Player* bot, TeamId us, bool warsong, const std::string& homeText)
    {
        if (flag.at == kDropped)
            return g_flagDroppedText;
        if (flag.at != kCarried)
            return homeText;
        Player* carrier = ObjectAccessor::FindPlayer(ObjectGuid(flag.carrier));
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!carrier || !ai || !ai->GetChatHelper())
            return g_flagCarriedText;
        if (carrier == bot)
            return g_flagSelfText;
        const std::string where = warsong ? WhereOnField(carrier, us) : std::string();
        if (carrier->GetBgTeamId() == us)
            return Fill(g_flagFriendText, fmt::arg("name", carrier->GetName()), fmt::arg("where", where));
        return Fill(g_flagEnemyText, fmt::arg("name", carrier->GetName()),
                    fmt::arg("race", ai->GetChatHelper()->FormatRace(carrier->getRace())),
                    fmt::arg("class", ai->GetChatHelper()->FormatClass(carrier->getClass())),
                    fmt::arg("where", where));
    }

    // Who holds which base, for someone of `us`.
    std::string BasesWords(Kind kind, const Seen& s, TeamId us)
    {
        const std::vector<Place>& places = PlacesOf(kind);
        std::vector<std::string> ours, theirs, taking, losing;
        for (size_t n = 0; n < s.nodes && n < places.size(); ++n)
        {
            if (s.node[n] == HeldBy(us))
                ours.push_back(places[n].name);
            else if (s.node[n] == HeldBy(Other(us)))
                theirs.push_back(places[n].name);
            else if (s.node[n] == TakenBy(us))
                taking.push_back(places[n].name);
            else if (s.node[n] == TakenBy(Other(us)))
                losing.push_back(places[n].name);
        }
        std::string out;
        auto add = [&out](const std::string& part)
        {
            if (!part.empty())
                out += (out.empty() ? "" : " ") + part;
        };
        add(ours.empty() ? g_holdNoneText : Fill(g_holdText, fmt::arg("list", Joined(ours))));
        if (!theirs.empty())
            add(Fill(g_enemyHoldText, fmt::arg("list", Joined(theirs))));
        if (!taking.empty())
            add(Fill(g_takingText, fmt::arg("list", Joined(taking))));
        if (!losing.empty())
            add(Fill(g_enemyTakingText, fmt::arg("list", Joined(losing))));
        return out;
    }

    // How the battleground stands, for `bot`: nothing that was not read from it.
    std::string StateFor(Battleground* bg, Kind kind, const Seen& s, Player* bot)
    {
        const TeamId us = bot->GetBgTeamId();
        const TeamId them = Other(us);
        std::string out;
        auto add = [&out](const std::string& part)
        {
            if (!part.empty())
                out += (out.empty() ? "" : " ") + part;
        };

        if (s.status == STATUS_WAIT_JOIN)
        {
            add(g_waitingText);
            return out;
        }
        if (s.status == STATUS_WAIT_LEAVE)
        {
            const PvPTeamId winner = bg->GetWinner();
            add(winner == PVP_TEAM_NEUTRAL ? g_drawText : GetTeamId(winner) == us ? g_wonText : g_lostText);
        }
        else if (const uint32_t minutes = MinutesGoing(bg))
            add(Fill(g_goingText, fmt::arg("minutes", minutes)));

        switch (kind)
        {
            case Kind::Warsong:
                add(Fill(g_warsongState, fmt::arg("us", s.score[us]), fmt::arg("them", s.score[them]),
                         fmt::arg("target", Target(kind)),
                         fmt::arg("ours", FlagWords(s.flag[us], bot, us, true, g_flagHomeText)),
                         fmt::arg("theirs", FlagWords(s.flag[them], bot, us, true, g_flagHomeText))));
                // The core's own clock, as the client shows it; it counts from
                // the two-minute wait it expects, so with another wait set it
                // can run past its end: not said then.
                if (s.status == STATUS_IN_PROGRESS)
                    if (BattlegroundWS* ws = bg->ToBattlegroundWS())
                        if (const uint32_t left = ws->GetMatchTime(); left >= 1 && left <= 25)
                            add(Fill(g_warsongClock, fmt::arg("minutes", left)));
                break;
            case Kind::Arathi:
                add(Fill(g_arathiState, fmt::arg("us", s.score[us]), fmt::arg("them", s.score[them]),
                         fmt::arg("target", Target(kind)), fmt::arg("bases", BasesWords(kind, s, us))));
                break;
            case Kind::Eye:
                add(Fill(g_eyeState, fmt::arg("us", s.score[us]), fmt::arg("them", s.score[them]),
                         fmt::arg("target", Target(kind)), fmt::arg("bases", BasesWords(kind, s, us)),
                         fmt::arg("flag", FlagWords(s.flag[0], bot, us, false, g_eyeFlagHomeText))));
                break;
            default:
                break;
        }
        return out;
    }

    bool Holds(const Claim& c, Battleground* bg)
    {
        const BattlegroundStatus status = bg->GetStatus();
        if (c.what == So::Waiting)
            return status == STATUS_WAIT_JOIN || status == STATUS_IN_PROGRESS;
        if (status != STATUS_IN_PROGRESS)
            return false;
        const Seen now = Read(bg, KindOf(bg));
        switch (c.what)
        {
            case So::FlagWith:
                return c.index < 2 && now.flag[c.index].at == kCarried && now.flag[c.index].carrier == c.who;
            case So::FlagHome:
                return c.index < 2 && now.flag[c.index].at == kAtHome;
            case So::Score:
                return now.score[0] == c.score[0] && now.score[1] == c.score[1];
            case So::Node:
                return c.index < now.nodes && now.node[c.index] == c.value;
            default:
                return true;
        }
    }

    bool CanType(Player* bot)
    {
        if (!bot || !Battleground_TeamChat(bot) || bot->IsBeingTeleported())
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        return ai && ai->GetChatHelper();
    }

    // One of the team's bots to say something: `must` when only that one can,
    // else someone who did not speak last, and not in the middle of a fight
    // when anyone else is free.
    Player* Speaker(const Match& m, const Side& side, TeamId team, uint64_t must)
    {
        std::vector<Player*> free, busy;
        for (Player* bot : side.bots)
        {
            if (!CanType(bot))
                continue;
            const uint64_t guid = bot->GetGUID().GetRawValue();
            if (must ? guid != must : (guid == m.lastSpeaker[team] && side.bots.size() > 1))
                continue;
            const bool fighting = bot->IsAlive() && bot->IsInCombat();
            if (fighting && g_DisableRepliesInCombat)
                continue;
            (fighting ? busy : free).push_back(bot);
        }
        const std::vector<Player*>& from = free.empty() ? busy : free;
        return from.empty() ? nullptr : from[urand(0, uint32_t(from.size() - 1))];
    }

    Claim ClaimOf(So what, Clock::time_point until, uint8_t index = 0, uint8_t value = 0, uint64_t who = 0)
    {
        Claim c;
        c.what  = what;
        c.index = index;
        c.value = value;
        c.who   = who;
        c.until = until;
        return c;
    }

    // Sends a line of `bot`'s to be written. True when it is on its way.
    bool Speak(Match& m, Battleground* bg, const Seen& s, TeamId team, Player* bot, const Event& e,
               Clock::time_point now)
    {
        Group* group = bot->GetGroup();
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!group || !ai || !ai->GetChatHelper() || g_linePrompt.empty() || e.ask.empty())
            return false;
        // Someone has only just said something there: not over them.
        if (Context_HumanSpokeSince(bot, SRC_RAID_LOCAL, std::string(), 0, g_holdSeconds))
            return false;
        const std::string scopeKey = ScopeOf(group);
        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
            return false;

        std::string prompt = Ask(g_linePrompt, fmt::arg("bot_name", bot->GetName()),
                                 fmt::arg("bot_level", bot->GetLevel()),
                                 fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                 fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                 fmt::arg("bot_personality", GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                 fmt::arg("battleground", m.name), fmt::arg("side", SideName(team)),
                                 fmt::arg("rules", RulesFor(bg, m.kind)),
                                 fmt::arg("state", StateFor(bg, m.kind, s, bot)), fmt::arg("ask", e.ask));
        if (prompt.empty())
            return false;
        prompt += Roleplay_BuildVoicePrompt(bot);

        const uint64_t id = ++g_nextClaim;
        Claim claim = e.claim;
        claim.instance = m.instance;
        g_claims[id] = claim;

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.source            = SRC_RAID_LOCAL;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        // Nobody answers it for the sake of answering: "omw", from one who is not.
        request.triggerBotReplies = false;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        // Nobody asked for it: one written before someone spoke is not sent.
        request.writtenAtSeq      = Context_NextSeq();
        // ...nor one that is no longer so by the time it is written.
        request.battleground      = id;
        // Asked "what?", it knows what it was on about (mod-ollama-chat_said).
        request.basis             = "in " + m.name + ": " + e.basis;
        if (!OllamaDispatch_Submit(std::move(request)))
        {
            g_claims.erase(id);
            return false;
        }
        m.lastLine[team]    = now;
        m.lastSpeaker[team] = bot->GetGUID().GetRawValue();
        ++g_stats.lines;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} has a line for its team in {}: {}", bot->GetName(), m.name,
                     e.basis);
        return true;
    }

    // The weightiest of what happened gets a line, if the chance and the gap
    // allow; the rest goes unsaid.
    void SayOneOf(Match& m, Battleground* bg, const Seen& s, TeamId team, const Side& side,
                  std::vector<Event>& events, Clock::time_point now)
    {
        if (events.empty())
            return;
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.rank > b.rank; });
        const Event& e = events.front();
        if (now - m.lastLine[team] < std::chrono::seconds(g_gapSeconds) || !e.chance || urand(1, 100) > e.chance)
        {
            ++g_stats.passed;
            return;
        }
        Player* bot = Speaker(m, side, team, e.must);
        if (!bot || !Speak(m, bg, s, team, bot, e, now))
            ++g_stats.passed;
    }

    Event EventOf(int rank, uint32_t chance, std::string ask, std::string basis, const Claim& claim,
                  uint64_t must = 0)
    {
        Event e;
        e.rank   = rank;
        e.chance = chance;
        e.ask    = std::move(ask);
        e.basis  = std::move(basis);
        e.claim  = claim;
        e.must   = must;
        return e;
    }

    // "the enemy orc warrior Grokk": for the model's own words about them.
    struct Who
    {
        std::string name, race, cls;
        TeamId      team = TEAM_NEUTRAL;
        bool        known = false;
    };

    Who WhoIs(uint64_t guid, Player* teller)
    {
        Who w;
        Player* who = ObjectAccessor::FindPlayer(ObjectGuid(guid));
        PlayerbotAI* ai = teller ? PlayerbotsMgr::instance().GetPlayerbotAI(teller) : nullptr;
        if (!who || !ai || !ai->GetChatHelper())
            return w;
        w.name  = who->GetName();
        w.race  = ai->GetChatHelper()->FormatRace(who->getRace());
        w.cls   = ai->GetChatHelper()->FormatClass(who->getClass());
        w.team  = who->GetBgTeamId();
        w.known = true;
        return w;
    }

    // Warsong Gulch: the flags and the score.
    void WarsongEvents(const Match& m, const Seen& s, TeamId us, const Side& side, std::vector<Event>& out,
                       Clock::time_point now)
    {
        const TeamId them = Other(us);
        Player* teller = side.bots.empty() ? nullptr : side.bots.front();
        const auto soon = now + kClaimFor;

        // A capture: the score, and nothing else of the flags this look (both
        // are put back).
        for (TeamId scorer : { us, them })
        {
            if (s.score[scorer] <= m.last.score[scorer])
                continue;
            Claim claim = ClaimOf(So::Score, soon);
            claim.score[0] = s.score[0];
            claim.score[1] = s.score[1];
            const std::string& templ = scorer == us ? g_captureUsText : g_captureThemText;
            out.push_back(EventOf(9, g_captureChance,
                                  Ask(templ, fmt::arg("us", s.score[us]), fmt::arg("them", s.score[them])),
                                  fmt::format("{} had just captured the flag, making it {} to {}",
                                              scorer == us ? "your team" : "the enemy", s.score[us], s.score[them]),
                                  claim));
            return;
        }

        for (TeamId whose : { us, them })
        {
            const Flag& was = m.last.flag[whose];
            const Flag& is  = s.flag[whose];
            if (is.at == kCarried && (was.at != kCarried || was.carrier != is.carrier))
            {
                const Who who = WhoIs(is.carrier, teller);
                if (!who.known)
                    continue;
                const Claim claim = ClaimOf(So::FlagWith, soon, uint8_t(whose), 0, is.carrier);
                if (whose == us)
                    out.push_back(EventOf(8, g_flagTakenChance,
                                          Ask(g_flagTakenText, fmt::arg("name", who.name),
                                              fmt::arg("race", who.race), fmt::arg("class", who.cls)),
                                          who.name + " of the enemy had just picked up your team's flag", claim));
                else
                {
                    // The one who has it says so now and then, when it is a bot.
                    Player* carrier = ObjectAccessor::FindPlayer(ObjectGuid(is.carrier));
                    const bool own = carrier && OllamaIsBotPlayer(carrier) && !g_flagMineText.empty() &&
                                     urand(1, 100) <= kCarrierSaysPct;
                    out.push_back(own ? EventOf(6, g_flagOursChance, Ask(g_flagMineText),
                                                "you had just picked up the enemy flag", claim, is.carrier)
                                      : EventOf(6, g_flagOursChance,
                                                Ask(g_flagOursText, fmt::arg("name", who.name)),
                                                who.name + " of your team had just picked up the enemy flag", claim));
                }
            }
            else if (whose == us && is.at == kAtHome && was.at != kAtHome)
                out.push_back(EventOf(5, g_flagReturnedChance, Ask(g_flagReturnedText),
                                      "your team's flag had just been got back to your base",
                                      ClaimOf(So::FlagHome, soon, uint8_t(whose))));
        }
    }

    // Arathi Basin's bases and Eye of the Storm's towers.
    void NodeEvents(const Match& m, const Seen& s, TeamId us, std::vector<Event>& out, Clock::time_point now)
    {
        const std::vector<Place>& places = PlacesOf(m.kind);
        const uint8_t mine = HeldBy(us), theirs = HeldBy(Other(us));
        const uint8_t theirTaking = TakenBy(Other(us));
        const auto soon = now + kClaimFor;
        for (size_t n = 0; n < s.nodes && n < places.size(); ++n)
        {
            const uint8_t was = m.last.node[n], is = s.node[n];
            if (was == is)
                continue;
            const auto base = fmt::arg("base", places[n].name);
            const Claim claim = ClaimOf(So::Node, soon, uint8_t(n), is);
            // Only of a base the team held: one it was itself still taking
            // (a race for a base nobody had yet) was never the team's.
            if (is == theirTaking && was == mine)
                out.push_back(EventOf(7, g_nodeAttackChance, Ask(g_nodeAttackText, base),
                                      "the enemy was taking " + places[n].name + " from your team", claim));
            // Arathi Basin: theirs a minute after they began taking it. (A
            // tower of the Eye's that went to nobody first has been said.)
            else if (is == theirs && (was == mine || (was == theirTaking && m.lastHeld[n] == mine)))
                out.push_back(EventOf(5, g_nodeLostChance, Ask(g_nodeLostText, base),
                                      "your team had just lost " + places[n].name + " to the enemy", claim));
            else if (is == kNobody && was == mine)
                out.push_back(EventOf(5, g_nodeLostChance, Ask(g_towerLostText, base),
                                      "your team had just lost its hold on " + places[n].name, claim));
            else if (is == mine && was == theirTaking && m.lastHeld[n] == mine)
                out.push_back(EventOf(4, g_nodeTakenChance, Ask(g_nodeSavedText, base),
                                      "your team had just stopped the enemy taking " + places[n].name, claim));
            else if (is == mine)
                out.push_back(EventOf(4, g_nodeTakenChance, Ask(g_nodeTakenText, base),
                                      "your team had just taken " + places[n].name, claim));
        }
    }

    // Eye of the Storm's flag.
    void EyeFlagEvents(const Match& m, const Seen& s, TeamId us, const Side& side, std::vector<Event>& out,
                       Clock::time_point now)
    {
        const Flag& was = m.last.flag[0];
        const Flag& is  = s.flag[0];
        Player* teller = side.bots.empty() ? nullptr : side.bots.front();
        if (is.at == kCarried && (was.at != kCarried || was.carrier != is.carrier))
        {
            const Who who = WhoIs(is.carrier, teller);
            if (!who.known)
                return;
            const Claim claim = ClaimOf(So::FlagWith, now + kClaimFor, 0, 0, is.carrier);
            if (who.team != us)
                out.push_back(EventOf(7, g_flagTakenChance,
                                      Ask(g_eyeFlagEnemyText, fmt::arg("name", who.name), fmt::arg("race", who.race),
                                          fmt::arg("class", who.cls)),
                                      who.name + " of the enemy had just picked up the flag in the middle", claim));
            else
            {
                Player* carrier = ObjectAccessor::FindPlayer(ObjectGuid(is.carrier));
                const bool own = carrier && OllamaIsBotPlayer(carrier) && !g_eyeFlagMineText.empty() &&
                                 urand(1, 100) <= kCarrierSaysPct;
                out.push_back(own ? EventOf(6, g_flagOursChance, Ask(g_eyeFlagMineText),
                                            "you had just picked up the flag in the middle", claim, is.carrier)
                                  : EventOf(6, g_flagOursChance, Ask(g_eyeFlagOursText, fmt::arg("name", who.name)),
                                            who.name + " of your team had just picked up the flag in the middle",
                                            claim));
            }
        }
        // Carried, and now back at its place without having been dropped: it
        // was taken to a tower.
        else if (was.at == kCarried && is.at == kAtHome && m.eyeCarrierTeam != TEAM_NEUTRAL)
        {
            const bool ours = m.eyeCarrierTeam == us;
            out.push_back(EventOf(8, g_captureChance, Ask(ours ? g_eyeCaptureUsText : g_eyeCaptureThemText),
                                  std::string(ours ? "your team" : "the enemy") + " had just captured the flag",
                                  ClaimOf(So::Going, now + std::chrono::seconds(30))));
        }
    }

    // The match nearly won, by either team: said once for each team that
    // gets there.
    void NearEndEvents(const Match& m, const Seen& s, TeamId us, std::vector<Event>& out, Clock::time_point now)
    {
        const uint32_t target = Target(m.kind);
        if (!target)
            return;
        const uint32_t near = target - target / 8;
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            if (m.nearEnd[team] || s.score[team] < near)
                continue;
            const bool ahead = s.score[us] >= s.score[Other(us)];
            out.push_back(EventOf(3, g_nearEndChance, Ask(ahead ? g_nearEndWinText : g_nearEndLoseText),
                                  fmt::format("the match was nearly over, your team {} with {} to the enemy's {}",
                                              ahead ? "ahead" : "behind", s.score[us], s.score[Other(us)]),
                                  ClaimOf(So::Going, now + kClaimFor)));
            return;
        }
    }

    // "inc farm 3": someone standing at a base of its team's, with as many of
    // the enemy there as there are to hold it, or more. Typed, in its own
    // style: by the time a line was written they would have it.
    void Incoming(Match& m, const Seen& s, TeamId us, const Side sides[2], Clock::time_point now)
    {
        const std::vector<Place>& places = PlacesOf(m.kind);
        if (g_incomingLines.empty() || !g_incomingChance || g_RoleplayEnable ||
            now - m.lastTyped[us] < kTypedGap)
            return;
        const Side& ours = sides[us];
        const Side& enemy = sides[Other(us)];
        for (size_t n = 0; n < s.nodes && n < places.size(); ++n)
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if ((s.node[n] != HeldBy(us) && s.node[n] != TakenBy(us)) || !WhereIs(m.kind, n, x, y, z) ||
                now - m.incoming[us][n] < std::chrono::seconds(g_incomingSeconds))
                continue;
            auto there = [&](Player* p) { return p->IsAlive() && p->GetExactDist(x, y, z) <= g_incomingYards; };
            // Counted as anyone there would see them: a rogue in stealth is no
            // one coming.
            auto seen = [&](Player* p) { return there(p) && !p->HasStealthAura() && !p->HasInvisibilityAura(); };
            uint32_t coming = 0, holding = 0;
            for (const std::vector<Player*>* list : { &enemy.bots, &enemy.people })
                coming += uint32_t(std::count_if(list->begin(), list->end(), seen));
            if (coming < std::max<uint32_t>(1, g_incomingMin))
                continue;
            std::vector<Player*> callers;
            for (Player* bot : ours.bots)
                if (there(bot))
                {
                    ++holding;
                    if (CanType(bot))
                        callers.push_back(bot);
                }
            holding += uint32_t(std::count_if(ours.people.begin(), ours.people.end(), there));
            if (callers.empty() || coming < holding)
                continue;
            m.incoming[us][n] = now;   // weighed, said or not
            if (urand(1, 100) > g_incomingChance)
            {
                ++g_stats.passed;
                continue;
            }
            Player* bot = callers[urand(0, uint32_t(callers.size() - 1))];
            const std::string line = Personality_StyleCanned(
                bot, Fill(g_incomingLines[urand(0, uint32_t(g_incomingLines.size() - 1))],
                          fmt::arg("base", places[n].call), fmt::arg("count", coming)));
            Group* group = bot->GetGroup();
            if (line.empty() || !group)
                continue;

            const uint64_t id = ++g_nextClaim;
            Claim claim = ClaimOf(So::Going, now + std::chrono::seconds(20));
            claim.instance = m.instance;
            g_claims[id] = claim;

            OllamaChatRequest request;
            request.botGuid           = bot->GetGUID().GetRawValue();
            request.source            = SRC_RAID_LOCAL;
            request.scopeKey          = ScopeOf(group);
            request.botName           = bot->GetName();
            request.kind              = OllamaRequestKind::EventChatter;
            request.fixedText         = line;
            request.directAddress     = true;    // the moment calls for it: no pacing cooldown
            request.noEcho            = true;    // "inc" is what everyone types
            request.triggerBotReplies = false;
            request.battleground      = id;
            request.basis = fmt::format("in {}: {} of the enemy were coming at {}, which your team held", m.name,
                                        coming, places[n].name);
            if (!OllamaDispatch_Submit(std::move(request)))
            {
                g_claims.erase(id);
                continue;
            }
            m.lastTyped[us] = now;
            ++g_stats.typed;
            return;   // one callout a look
        }
    }

    // The match is over: a few of the team say gg, typed and sent from here a
    // moment on -- a bot leaves the battleground at once -- and what the team
    // did together is written down.
    void Ended(Match& m, Battleground* bg, const Side sides[2], Clock::time_point now)
    {
        m.over = true;
        const PvPTeamId winner = bg->GetWinner();
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            const Side& side = sides[team];
            if (side.people.empty() || side.bots.empty())
                continue;
            const int outcome = winner == PVP_TEAM_NEUTRAL ? 0 : GetTeamId(winner) == team ? 1 : -1;

            if (g_journal)
            {
                std::vector<Player*> team_ = side.bots;
                team_.insert(team_.end(), side.people.begin(), side.people.end());
                Journal_Battle(team_, m.name, outcome);
            }

            std::vector<std::string> lines = outcome > 0 ? g_ggWinLines : outcome < 0 ? g_ggLossLines : g_ggDrawLines;
            if (lines.empty() || !g_ggChance || !g_ggMax || g_RoleplayEnable || g_DisableForParty)
                continue;
            std::shuffle(lines.begin(), lines.end(), Rng());
            std::vector<Player*> bots = side.bots;
            std::shuffle(bots.begin(), bots.end(), Rng());
            uint32_t said = 0;
            for (Player* bot : bots)
            {
                if (said >= g_ggMax)
                    break;
                if (!CanType(bot) || urand(1, 100) > g_ggChance)
                    continue;
                const std::string text = Personality_StyleCanned(bot, lines[said % lines.size()]);
                if (text.empty())
                    continue;
                Later later;
                later.due      = now + std::chrono::milliseconds(urand(g_ggDelayMin * 1000, g_ggDelayMax * 1000));
                later.instance = m.instance;
                later.self     = m.self;
                later.team     = team;
                later.from     = bot->GetGUID().GetRawValue();
                later.name     = bot->GetName();
                later.text     = text;
                g_later.push_back(std::move(later));
                ++said;
            }
        }
    }

    // A typed line whose moment has come goes to those of the team still there.
    void SendLater(Clock::time_point now)
    {
        for (auto it = g_later.begin(); it != g_later.end();)
        {
            if (now < it->due)
            {
                ++it;
                continue;
            }
            if (Battleground* bg = sBattlegroundMgr->GetBattleground(it->instance, BATTLEGROUND_TYPE_NONE);
                bg && bg == it->self)
            {
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, LANG_UNIVERSAL, ObjectGuid(it->from),
                                             ObjectGuid::Empty, it->text, CHAT_TAG_NONE, it->name);
                bool heard = false;
                for (auto const& [guid, player] : bg->GetPlayers())
                    if (player && player->IsInWorld() && player->GetBgTeamId() == it->team &&
                        OllamaIsRealPlayer(player))
                    {
                        player->SendDirectMessage(&data);
                        heard = true;
                    }
                if (heard)
                    ++g_stats.gg;
            }
            it = g_later.erase(it);
        }
    }

    void Look(Match& m, Battleground* bg, Clock::time_point now)
    {
        const Seen s = Read(bg, m.kind);
        Side sides[2];
        Sides(bg, sides);

        auto remember = [&]()
        {
            for (size_t n = 0; n < s.nodes; ++n)
                if (s.node[n] == kAlliance || s.node[n] == kHorde)
                    m.lastHeld[n] = s.node[n];
            if (m.kind == Kind::Eye)
            {
                if (s.flag[0].at == kCarried)
                {
                    if (Player* carrier = ObjectAccessor::FindPlayer(ObjectGuid(s.flag[0].carrier)))
                        m.eyeCarrierTeam = uint8_t(carrier->GetBgTeamId());
                }
                else
                    m.eyeCarrierTeam = TEAM_NEUTRAL;
            }
            m.last = s;
        };

        // The first look, or a battleground joined when it was already over:
        // remembered, and nothing said of what led up to it.
        if (!m.looked)
        {
            m.looked = true;
            m.over   = s.status == STATUS_WAIT_LEAVE;
            remember();
            return;
        }
        if (s.status == STATUS_WAIT_LEAVE)
        {
            if (!m.over)
                Ended(m, bg, sides, now);
            remember();
            return;
        }
        if (s.status == STATUS_IN_PROGRESS && m.last.status == STATUS_WAIT_JOIN)
        {
            m.startedAt = now;
            m.startSeen = true;
        }
        if (g_DisableForParty)
        {
            remember();
            return;
        }

        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            const Side& side = sides[team];
            if (side.people.empty() || side.bots.empty())
                continue;   // nobody to read it, or nobody to say it
            std::vector<Event> events;

            if (s.status == STATUS_WAIT_JOIN)
            {
                // Before the start, once: good luck, and how to win it.
                const int32_t toGo = bg->GetStartDelayTime();
                if (!m.prepSaid[team] && toGo > 0 && toGo <= m.prepAt)
                {
                    m.prepSaid[team] = true;
                    events.push_back(EventOf(1, g_startChance, Ask(g_startText),
                                             "the match was about to start",
                                             ClaimOf(So::Waiting,
                                                     now + std::chrono::milliseconds(toGo) + kClaimFor)));
                }
            }
            else if (s.status == STATUS_IN_PROGRESS)
            {
                if (m.kind == Kind::Warsong)
                    WarsongEvents(m, s, team, side, events, now);
                else if (m.kind == Kind::Arathi || m.kind == Kind::Eye)
                {
                    NodeEvents(m, s, team, events, now);
                    if (m.kind == Kind::Eye)
                        EyeFlagEvents(m, s, team, side, events, now);
                    NearEndEvents(m, s, team, events, now);
                    Incoming(m, s, team, sides, now);
                }
            }
            SayOneOf(m, bg, s, team, side, events, now);
        }

        // A score that has got near the end has been weighed for every team
        // that could say so.
        if (const uint32_t target = Target(m.kind); target && s.status == STATUS_IN_PROGRESS)
            for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
                if (s.score[team] >= target - target / 8)
                    m.nearEnd[team] = true;
        remember();
    }

    std::vector<std::string> Lines(const char* key, const char* def)
    {
        return TextRules::SplitBars(sConfigMgr->GetOption<std::string>(key, def));
    }

    // "the Stables;stables|the Blacksmith;BS|...": `want` of them, or none.
    std::vector<Place> Places(const char* key, const char* def, size_t want)
    {
        std::vector<Place> out;
        for (const std::string& part : SplitString(sConfigMgr->GetOption<std::string>(key, def), '|'))
        {
            const std::vector<std::string> bits = SplitString(part, ';');
            if (bits.empty() || bits[0].empty())
                continue;
            out.push_back({ bits[0], bits.size() > 1 && !bits[1].empty() ? bits[1] : bits[0] });
        }
        if (out.size() != want)
        {
            LOG_WARN("module.ollamachat", "[Ollama Chat] {} names {} places, not the {} the battleground has: its "
                     "bases are left out of what is said.", key, out.size(), want);
            out.clear();
        }
        return out;
    }
}

void Battleground_LoadConfig()
{
    auto text = [](const char* key, const char* def)
    {
        return sConfigMgr->GetOption<std::string>(key, def);
    };
    auto number = [](const char* key, uint32_t def, uint32_t lo, uint32_t hi)
    {
        return std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>(key, def), lo, hi);
    };

    g_enable      = sConfigMgr->GetOption<bool>("OllamaChat.Battleground.Enable", true);
    g_journal     = sConfigMgr->GetOption<bool>("OllamaChat.Battleground.Journal", true);
    g_gapSeconds  = number("OllamaChat.Battleground.GapSeconds", 20, 0, 600);
    g_holdSeconds = number("OllamaChat.Battleground.HoldSeconds", 10, 0, 600);
    g_prepMin     = number("OllamaChat.Battleground.StartMinSeconds", 10, 1, 110);
    g_prepMax     = std::max(g_prepMin, number("OllamaChat.Battleground.StartMaxSeconds", 45, 1, 110));

    g_startChance        = number("OllamaChat.Battleground.StartChance", 70, 0, 100);
    g_flagTakenChance    = number("OllamaChat.Battleground.FlagTakenChance", 70, 0, 100);
    g_flagOursChance     = number("OllamaChat.Battleground.FlagOursChance", 40, 0, 100);
    g_flagReturnedChance = number("OllamaChat.Battleground.FlagReturnedChance", 35, 0, 100);
    g_captureChance      = number("OllamaChat.Battleground.CaptureChance", 70, 0, 100);
    g_nodeAttackChance   = number("OllamaChat.Battleground.BaseAttackedChance", 65, 0, 100);
    g_nodeTakenChance    = number("OllamaChat.Battleground.BaseTakenChance", 25, 0, 100);
    g_nodeLostChance     = number("OllamaChat.Battleground.BaseLostChance", 45, 0, 100);
    g_nearEndChance      = number("OllamaChat.Battleground.NearEndChance", 60, 0, 100);

    g_incomingChance  = number("OllamaChat.Battleground.IncomingChance", 75, 0, 100);
    g_incomingMin     = number("OllamaChat.Battleground.IncomingMin", 2, 1, 40);
    g_incomingSeconds = number("OllamaChat.Battleground.IncomingSeconds", 45, 5, 600);
    g_incomingYards   = float(number("OllamaChat.Battleground.IncomingYards", 45, 10, 100));

    g_ggChance   = number("OllamaChat.Battleground.GgChance", 60, 0, 100);
    g_ggMax      = number("OllamaChat.Battleground.GgMax", 3, 0, 10);
    g_ggDelayMin = number("OllamaChat.Battleground.GgDelayMinSeconds", 1, 0, 60);
    g_ggDelayMax = std::max(g_ggDelayMin, number("OllamaChat.Battleground.GgDelayMaxSeconds", 7, 0, 60));

    g_linePrompt = text("OllamaChat.Battleground.LinePrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} playing in {battleground}, a battleground, on "
        "the {side} team. Your chat style: {bot_personality} {rules} {state} {ask} Say it in battleground chat, which "
        "only your own team reads, the way players do in a battleground: a few words, in your own style. Say only "
        "what is so: nothing of where you will go or what you will do next, and no score or news but what is above. "
        "One short line, 2 to 9 words, no quotes, no name in front.");
    g_replyText = text("OllamaChat.Battleground.ReplyText",
        "You are in {battleground}, a battleground, on the {side} team, and this is battleground chat: only your own "
        "team reads it. {rules} {state} That is for you to know, not to read out: answer what was said, in a few "
        "words, the way players do in a battleground. Say only what is so: nothing of where you will go or what you "
        "will do next, and no score or news but what is above.");

    g_rulesWarsong = text("OllamaChat.Battleground.Rules.Warsong",
        "Warsong Gulch is {size} against {size}, with no creatures to fight, only the other team. Each team has a "
        "flag in its base, and scores by carrying the enemy's flag to where its own flag stands, which counts only "
        "while its own flag is there. The first to {target} captures wins.");
    g_rulesArathi = text("OllamaChat.Battleground.Rules.Arathi",
        "Arathi Basin is {size} against {size}, with no creatures to fight, only the other team. There are five "
        "bases -- the Stables, the Blacksmith, the Farm, the Lumber Mill and the Gold Mine -- each taken by using "
        "the flag at it and holding it for a minute. Bases held earn resources, the more bases the faster, and the "
        "first team to {target} resources wins.");
    g_rulesEye = text("OllamaChat.Battleground.Rules.Eye",
        "Eye of the Storm is {size} against {size}, with no creatures to fight, only the other team. There are four "
        "towers -- the Mage Tower, the Draenei Ruins, the Blood Elf Tower and the Fel Reaver Ruins -- each taken by "
        "having more of your team standing at it than the enemy has. Towers held earn points, and so does carrying "
        "the flag from the middle to a tower your team holds. The first team to {target} points wins.");
    g_rulesAlterac = text("OllamaChat.Battleground.Rules.Alterac",
        "Alterac Valley is {size} against {size}. Each team fights its way along the valley to the other's base, "
        "taking graveyards and burning towers on the way, and wins by killing the other team's general -- the "
        "Alliance's is Vanndar Stormpike, the Horde's is Drek'Thar -- or when the other team runs out of "
        "reinforcements.");
    g_rulesIsle = text("OllamaChat.Battleground.Rules.Isle",
        "Isle of Conquest is {size} against {size}. The teams fight over the Docks, the Hangar and the Workshop, "
        "which give the siege weapons to break into the other team's keep with, and a team wins by killing the "
        "general in that keep -- the Alliance's is High Commander Halford Wyrmbane, the Horde's is Overlord Agmar "
        "-- or when the other team runs out of reinforcements.");
    g_rulesStrand = text("OllamaChat.Battleground.Rules.Strand",
        "Strand of the Ancients is {size} against {size}, in two rounds. One team lands on the beach and breaks "
        "through the gates with demolishers and bombs to reach the relic at the top, while the other defends; then "
        "they change over, and the team that got to the relic faster wins.");

    g_waitingText = text("OllamaChat.Battleground.WaitingText",
        "The match has not started yet: both teams are waiting for it to.");
    g_goingText = text("OllamaChat.Battleground.GoingText", "The match has been going {minutes} minutes.");
    g_wonText   = text("OllamaChat.Battleground.WonText", "The match is over, and your team won.");
    g_lostText  = text("OllamaChat.Battleground.LostText", "The match is over, and your team lost.");
    g_drawText  = text("OllamaChat.Battleground.DrawText", "The match is over, and neither team won.");

    g_warsongState = text("OllamaChat.Battleground.Warsong.StateText",
        "The score is {us} to {them} in captures. Your team's flag is {ours}. The enemy's flag is {theirs}.");
    g_warsongClock = text("OllamaChat.Battleground.Warsong.ClockText",
        "About {minutes} minutes are left on the clock; when it runs out, the team with more captures wins.");
    g_arathiState = text("OllamaChat.Battleground.Arathi.StateText",
        "Your team has {us} resources and the enemy {them}. {bases}");
    g_eyeState = text("OllamaChat.Battleground.Eye.StateText",
        "Your team has {us} points and the enemy {them}. {bases} The flag in the middle is {flag}.");

    g_flagHomeText    = text("OllamaChat.Battleground.FlagHomeText", "safe in its base");
    g_eyeFlagHomeText = text("OllamaChat.Battleground.Eye.FlagHomeText", "at its place, with nobody carrying it");
    g_flagDroppedText = text("OllamaChat.Battleground.FlagDroppedText",
        "lying on the ground where whoever had it dropped it");
    g_flagCarriedText = text("OllamaChat.Battleground.FlagCarriedText", "being carried");
    g_flagSelfText    = text("OllamaChat.Battleground.FlagSelfText", "being carried by you");
    g_flagFriendText  = text("OllamaChat.Battleground.FlagFriendText", "being carried by {name} of your team{where}");
    g_flagEnemyText   = text("OllamaChat.Battleground.FlagEnemyText",
        "being carried by {name}, an enemy {race} {class}{where}");
    g_whereTexts = SplitString(text("OllamaChat.Battleground.Warsong.WhereTexts",
        "who is in your base|who is on your side of the field|who is in the middle of the field|who is on the "
        "enemy's side of the field|who is in the enemy's base"), '|');
    if (g_whereTexts.size() != 5)
    {
        LOG_WARN("module.ollamachat", "[Ollama Chat] Battleground.Warsong.WhereTexts has {} parts, not 5: where a "
                 "flag carrier is goes unsaid.", g_whereTexts.size());
        g_whereTexts.clear();
    }

    g_holdText        = text("OllamaChat.Battleground.HoldText", "Your team holds {list}.");
    g_holdNoneText    = text("OllamaChat.Battleground.HoldNoneText", "Your team holds none of them.");
    g_enemyHoldText   = text("OllamaChat.Battleground.EnemyHoldText", "The enemy holds {list}.");
    g_takingText      = text("OllamaChat.Battleground.TakingText", "Your team is taking {list}.");
    g_enemyTakingText = text("OllamaChat.Battleground.EnemyTakingText",
        "The enemy is taking {list}, which is theirs within a minute unless they are stopped.");

    g_startText = text("OllamaChat.Battleground.StartText",
        "The match is about to start. {~Wish your team luck|Say something short about how this one is won|Say what "
        "the team should not forget to do|Greet your team}.");
    g_flagTakenText = text("OllamaChat.Battleground.FlagTakenText",
        "{name}, an enemy {race} {class}, has just picked up your team's flag. Call it out to your team.");
    g_flagOursText = text("OllamaChat.Battleground.FlagOursText",
        "{name} of your team has just picked up the enemy's flag. {~Tell the team to help get it home|Cheer them "
        "on|Tell the team to keep your own flag safe meanwhile}.");
    g_flagMineText = text("OllamaChat.Battleground.FlagMineText",
        "You have just picked up the enemy's flag yourself. Tell your team, and ask for help getting it home.");
    g_flagReturnedText = text("OllamaChat.Battleground.FlagReturnedText",
        "Your team's flag has just been got back, and is in your base again. {~Say well done|Say to go and get "
        "theirs now|Say that was close}.");
    g_captureUsText = text("OllamaChat.Battleground.CaptureUsText",
        "Your team has just captured the enemy's flag: it is {us} to {them} now. {~Cheer|Say well done|Say one more "
        "like that}.");
    g_captureThemText = text("OllamaChat.Battleground.CaptureThemText",
        "The enemy has just captured your team's flag: it is {us} to {them} now. {~Rally the team|Grumble about "
        "it|Say what the team has to do now}.");
    g_nodeAttackText = text("OllamaChat.Battleground.BaseAttackedText",
        "The enemy is taking {base} from your team: it is theirs within a minute unless someone stops them. Call it "
        "out to your team.");
    g_nodeTakenText = text("OllamaChat.Battleground.BaseTakenText",
        "Your team has just taken {base}. {~Say well done|Say to hold it now|Say what to go for next}.");
    g_nodeSavedText = text("OllamaChat.Battleground.BaseSavedText",
        "Your team has just stopped the enemy taking {base}: it is still yours. {~Say well done|Say that was "
        "close|Say to keep someone there}.");
    g_nodeLostText = text("OllamaChat.Battleground.BaseLostText",
        "Your team has just lost {base} to the enemy. {~Say so|Tell the team to take it back|Grumble about it}.");
    g_towerLostText = text("OllamaChat.Battleground.TowerLostText",
        "Your team has just lost its hold on {base}: nobody holds it now. {~Say so|Tell the team to get back "
        "there}.");
    g_eyeFlagEnemyText = text("OllamaChat.Battleground.Eye.FlagEnemyText",
        "{name}, an enemy {race} {class}, has just picked up the flag in the middle. Call it out to your team.");
    g_eyeFlagOursText = text("OllamaChat.Battleground.Eye.FlagOursText",
        "{name} of your team has just picked up the flag in the middle. {~Tell the team to cover them|Cheer them "
        "on}.");
    g_eyeFlagMineText = text("OllamaChat.Battleground.Eye.FlagMineText",
        "You have just picked up the flag in the middle yourself. Tell your team.");
    g_eyeCaptureUsText = text("OllamaChat.Battleground.Eye.CaptureUsText",
        "Your team has just captured the flag. {~Cheer|Say well done}.");
    g_eyeCaptureThemText = text("OllamaChat.Battleground.Eye.CaptureThemText",
        "The enemy has just captured the flag. {~Tell the team to watch the middle|Grumble about it}.");
    g_nearEndWinText = text("OllamaChat.Battleground.NearEndWinText",
        "The match is nearly over, and your team is ahead. {~Tell the team to hold what it has|Say it is nearly "
        "done|Cheer the team on}.");
    g_nearEndLoseText = text("OllamaChat.Battleground.NearEndLoseText",
        "The match is nearly over, and your team is behind. {~Say what it would take to turn it round|Say well "
        "tried|Grumble a little}.");

    g_incomingLines = Lines("OllamaChat.Battleground.IncomingLines",
        "inc {base} {count}|{count} inc {base}|{base} inc {count}|{count} at {base}|inc {base}, {count} of them|"
        "{base} needs help, {count} inc");
    g_ggWinLines  = Lines("OllamaChat.Battleground.GgWinLines", "gg|gg all|gg wp|nice one all|good game|well played");
    g_ggLossLines = Lines("OllamaChat.Battleground.GgLossLines",
        "gg|gg all|gg wp|unlucky|good game|well tried all|gg, next one");
    g_ggDrawLines = Lines("OllamaChat.Battleground.GgDrawLines", "gg|gg all|good game");

    g_arathiBases = Places("OllamaChat.Battleground.Arathi.Bases",
        "the Stables;stables|the Blacksmith;BS|the Farm;farm|the Lumber Mill;LM|the Gold Mine;mine",
        BG_AB_DYNAMIC_NODES_COUNT);
    g_eyeTowers = Places("OllamaChat.Battleground.Eye.Towers",
        "the Fel Reaver Ruins;FR|the Blood Elf Tower;BE|the Draenei Ruins;DR|the Mage Tower;MT", EY_POINTS_MAX);
}

bool Battleground_TeamChat(Player* who)
{
    if (!g_enable || !who || !who->InBattleground() || who->InArena())
        return false;
    Group* group = who->GetGroup();
    return group && group->isBGGroup() && OnATeam(who);
}

bool Battleground_Say(Player* bot, const std::string& text)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group || !group->isBGGroup() || text.empty())
        return false;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, LANG_UNIVERSAL, bot->GetGUID(), ObjectGuid::Empty, text,
                                 CHAT_TAG_NONE, bot->GetName());
    bool heard = false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* member = ref->GetSource(); member && member->IsInWorld() && OllamaIsRealPlayer(member))
        {
            member->SendDirectMessage(&data);
            heard = true;
        }
    return heard;
}

std::string Battleground_PromptFor(Player* bot, ChatChannelSourceLocal source)
{
    if ((source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL) || !Battleground_TeamChat(bot) ||
        g_replyText.empty())
        return {};
    Battleground* bg = Of(bot);
    if (!bg)
        return {};
    const Kind kind = KindOf(bg);
    const std::string out = Fill(g_replyText, fmt::arg("battleground", bg->GetName()),
                                 fmt::arg("side", SideName(bot->GetBgTeamId())),
                                 fmt::arg("rules", RulesFor(bg, kind)),
                                 fmt::arg("state", StateFor(bg, kind, Read(bg, kind), bot)));
    if (!out.empty())
        ++g_stats.answered;
    return out;
}

bool Battleground_StillSo(Player* bot, uint64_t claim)
{
    const auto it = g_claims.find(claim);
    Battleground* bg = Of(bot);
    const bool so = it != g_claims.end() && bg && bg->GetInstanceID() == it->second.instance &&
                    Clock::now() <= it->second.until && Holds(it->second, bg);
    if (!so)
        ++g_stats.stale;
    return so;
}

void Battleground_Update(uint32_t /*diff*/)
{
    const auto now = Clock::now();
    if (!g_later.empty())
        SendLater(now);
    if (!g_Enable || !g_enable)
    {
        g_matches.clear();
        g_claims.clear();
        return;
    }

    // The end is looked for every tick in a battleground being watched: its
    // bots are gone by the next.
    for (auto& [instance, m] : g_matches)
    {
        if (m.over || !m.looked)
            continue;
        Battleground* bg = sBattlegroundMgr->GetBattleground(instance, BATTLEGROUND_TYPE_NONE);
        if (bg && bg == m.self && bg->GetStatus() == STATUS_WAIT_LEAVE)
            Look(m, bg, now);
    }

    if (now < g_nextLook)
        return;
    g_nextLook = now + kLookEvery;

    // Every battleground with someone playing in it, once.
    std::vector<uint32_t> looked;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->InBattleground() || OllamaIsBotPlayer(player))
            continue;
        Battleground* bg = Of(player);
        if (!bg || !OnATeam(player))
            continue;
        const uint32_t instance = bg->GetInstanceID();
        if (std::find(looked.begin(), looked.end(), instance) != looked.end())
            continue;
        looked.push_back(instance);

        auto [it, fresh] = g_matches.try_emplace(instance);
        Match& m = it->second;
        // The core hands a finished battleground's number to the next one
        // made: the same number with another battleground behind it, or one
        // that was over and is not, is a new match.
        if (!fresh && (m.self != bg || (m.over && bg->GetStatus() != STATUS_WAIT_LEAVE)))
        {
            m = Match{};
            fresh = true;
        }
        if (fresh)
        {
            m.instance = instance;
            m.self     = bg;
            m.kind     = KindOf(bg);
            m.name     = bg->GetName().empty() ? std::string("the battleground") : bg->GetName();
            m.prepAt   = int32_t(urand(g_prepMin, g_prepMax) * IN_MILLISECONDS);
            ++g_stats.matches;
        }
        m.seen = now;
        Look(m, bg, now);
    }

    for (auto it = g_matches.begin(); it != g_matches.end();)
        it = now - it->second.seen > kForgetAfter ? g_matches.erase(it) : std::next(it);
    for (auto it = g_claims.begin(); it != g_claims.end();)
        it = now > it->second.until + std::chrono::minutes(2) ? g_claims.erase(it) : std::next(it);
}

BattlegroundStats Battleground_GetStats()
{
    BattlegroundStats s = g_stats;
    const auto now = Clock::now();
    for (const auto& [instance, m] : g_matches)
        if (!m.over && now - m.seen < std::chrono::seconds(10))
            ++s.watched;
    return s;
}
