#include "mod-ollama-chat_mingle.h"
#include "mod-ollama-chat_clock.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_lately.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_pets.h"
#include "mod-ollama-chat_presence.h"
#include "mod-ollama-chat_race.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t kHoldMs        = 1500;   // a held bot's AI waits this long; renewed every tick
    constexpr uint32_t kScriptWaitSec = 60;     // an exchange still unwritten after this is given up
    constexpr uint32_t kLookMs        = 2000;   // how often to look for new ones

    struct Line
    {
        size_t      speaker = 0;
        size_t      target  = 0;
        std::string text;          // empty: an emote on its own
        uint32_t    emoteId = 0;
    };

    struct Exchange
    {
        uint64_t                 id = 0;
        std::vector<uint64_t>    members;   // in the order the prompt names them
        std::vector<std::string> names;
        std::vector<bool>        held;      // its AI is held here (it was not hanging out)
        uint32_t                 mapId = 0;
        std::string              scopeKey;  // the /say scope, as for any other /say line
        bool                     waiting = true;
        Clock::time_point        waitUntil{};
        std::vector<Line>        lines;
        size_t                   next = 0;
        Clock::time_point        nextAt{};
        std::string              lastText;  // the line before: what the next one answers
        bool                     duel = false;   // they talk themselves into a duel: at the end, off they go
    };

    std::unordered_map<uint64_t, Exchange>          g_exchanges;      // by id
    std::unordered_map<uint64_t, uint64_t>          g_busy;           // bot -> its exchange
    std::unordered_map<uint64_t, Clock::time_point> g_botRest;        // bot -> may mingle again
    std::unordered_map<uint64_t, Clock::time_point> g_playerNext;     // real player -> next exchange near them
    std::unordered_map<uint64_t, Clock::time_point> g_playerNextNod;  // real player -> next greeting near them
    std::unordered_map<uint64_t, Clock::time_point> g_nodRest;        // bot -> may greet again
    std::deque<std::string>                         g_recentPremises;
    uint64_t          g_nextId = 1;
    Clock::time_point g_nextLook{};
    MingleStats       g_stats;

    // Logouts, queued from whichever thread logged the character out.
    std::mutex            g_forgetMutex;
    std::vector<uint64_t> g_forget;
    std::atomic<bool>     g_forgetPending{ false };

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint32_t Between(uint32_t lo, uint32_t hi)
    {
        if (hi < lo)
            std::swap(lo, hi);
        return std::uniform_int_distribution<uint32_t>(lo, hi)(Rng());
    }

    Player* Find(uint64_t guid)
    {
        return ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
    }

    // First time a player is seen: a while before anything happens near them,
    // so walking into the Trade District does not set everyone talking at once.
    bool Due(std::unordered_map<uint64_t, Clock::time_point>& next, uint64_t key, Clock::time_point now,
             uint32_t firstMinSec, uint32_t firstMaxSec)
    {
        auto it = next.find(key);
        if (it == next.end())
        {
            next[key] = now + std::chrono::seconds(Between(firstMinSec, firstMaxSec));
            return false;
        }
        return now >= it->second;
    }

    bool Queued(Player* bot)
    {
        return bot->InBattlegroundQueue() || sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE;
    }

    // Something wants it: a fight, an invite, a trade, a duel, a queue -- or
    // it is gone, dead or flying off.
    bool Wanted(Player* bot)
    {
        return !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() ||
               bot->IsInFlight() || bot->GetGroup() || bot->GetGroupInvite() || bot->GetTradeData() || bot->duel ||
               bot->GetGuildIdInvited() || bot->GetArenaTeamIdInvited() || Queued(bot);
    }

    bool HangingOut(Player* bot)
    {
        std::string spot, doing;
        return Hangout_WhereAndDoing(bot, spot, doing);
    }

    // One of the random bots standing about, on `listener`'s side: not
    // someone's own bot, not grouped, not AFK, not wanted, not in an exchange.
    bool Idle(Player* bot, Player* listener)
    {
        if (!bot || bot == listener || !bot->IsInWorld() || !OllamaIsBotPlayer(bot))
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->IsBotAI() || ai->GetMaster() || !sRandomPlayerbotMgr.IsRandomBot(bot))
            return false;
        if (Wanted(bot) || bot->isAFK() || Hangout_IsAway(bot) || bot->IsFlying() || bot->GetTransport())
            return false;
        // One riding up to a city hangout gets off its mount when it gets
        // there: "nice mount" would be about one it no longer has.
        if (bot->IsMounted() && HangingOut(bot))
            return false;
        return bot->GetTeamId() == listener->GetTeamId() && !g_busy.count(bot->GetGUID().GetRawValue());
    }

    bool Rested(const std::unordered_map<uint64_t, Clock::time_point>& rest, uint64_t key, Clock::time_point now)
    {
        auto it = rest.find(key);
        return it == rest.end() || now >= it->second;
    }

    std::vector<Player*> BotsNear(Player* listener, float range)
    {
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(listener, range, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(listener, found, check);
        Cell::VisitObjects(listener, searcher, range);
        return std::vector<Player*>(found.begin(), found.end());
    }

    // Two (sometimes three) of `pool` standing within `range` of each other:
    // those hanging out first, then the nearest to them.
    std::vector<Player*> PickGroup(std::vector<Player*> pool, float range, bool allowThree)
    {
        if (pool.size() < 2)
            return {};
        std::shuffle(pool.begin(), pool.end(), Rng());
        std::stable_partition(pool.begin(), pool.end(), [](Player* p) { return HangingOut(p); });

        for (size_t i = 0; i < pool.size() && i < 6; ++i)
        {
            Player* a = pool[i];
            std::vector<std::pair<float, Player*>> close;
            for (Player* b : pool)
                if (b != a && b->GetMapId() == a->GetMapId() && a->GetExactDist(b) <= range)
                    close.emplace_back(a->GetExactDist(b), b);
            if (close.empty())
                continue;
            std::sort(close.begin(), close.end(),
                      [](const std::pair<float, Player*>& x, const std::pair<float, Player*>& y)
                      { return x.first < y.first; });

            const size_t pick = Between(0, uint32_t(std::min<size_t>(close.size(), 3) - 1));
            std::vector<Player*> group = { a, close[pick].second };
            if (allowThree && close.size() >= 2 && Between(1, 100) <= g_MingleThreeChance)
                group.push_back(close[pick == 0 ? 1 : 0].second);
            return group;
        }
        return {};
    }

    // ---- the prompt -------------------------------------------------------

    std::string Where(Player* bot)
    {
        std::string spot, doing;
        if (Hangout_WhereAndDoing(bot, spot, doing) && !spot.empty())
            return spot;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        AreaTableEntry const* area = ai ? ai->GetCurrentArea() : nullptr;
        AreaTableEntry const* zone = ai ? ai->GetCurrentZone() : nullptr;
        const std::string areaName = area ? PlayerbotAI::GetLocalizedAreaName(area) : std::string();
        const std::string zoneName = zone ? PlayerbotAI::GetLocalizedAreaName(zone) : std::string();
        if (!areaName.empty() && !zoneName.empty() && areaName != zoneName)
            return areaName + ", " + zoneName;
        return !zoneName.empty() ? zoneName : areaName;
    }

    // What the quests and items in their "lately" are, so a line about one
    // -- linked or not -- is about what it really is: "[The Legend of
    // Stalvan]" is a quest in Duskwood, not a person.
    std::string ThingsText(const std::vector<Player*>& group)
    {
        if (g_MingleThingsText.empty())
            return {};
        std::string things;
        for (Player* p : group)
        {
            const LatelyNamed named = Lately_Named(p);
            std::string quest, item;
            if (named.quest)
                quest = Links_DescribeQuest(sObjectMgr->GetQuestTemplate(named.quest), p);
            if (named.item)
                item = Links_DescribeItem(sObjectMgr->GetItemTemplate(named.item));
            for (const std::string* t : { &quest, &item })
                if (!t->empty())
                    things += (things.empty() ? "" : "; ") + *t;
        }
        if (things.empty())
            return {};
        const std::string text = SafeFormat(g_MingleThingsText, fmt::arg("things", things));
        return text == "[Format Error]" ? std::string() : text;
    }

    // One line each: what the others can see of them, and their chat style.
    std::string PeopleText(const std::vector<Player*>& group)
    {
        std::string out;
        for (size_t i = 0; i < group.size(); ++i)
        {
            Player* p = group[i];
            std::string line = Presence_Describe(p, group[(i + 1) % group.size()]);
            std::string spot, doing;
            if (!p->IsMounted() && Hangout_WhereAndDoing(p, spot, doing) && !doing.empty())
                line += ", " + doing;
            out += "\n- " + line + ".";
            const std::string style = GetPersonalityPromptAddition(GetBotPersonality(p));
            if (!style.empty())
                out += " " + p->GetName() + "'s chat style, as told to them: " + style;
            // Now and then, something their race says ("zug zug").
            out += Race_MingleFor(p);
            // What they would answer "what you up to?" with, rather than something made up.
            if (const std::string lately = Lately_Text(p); !lately.empty() && !g_MingleLatelyText.empty())
            {
                const std::string text = SafeFormat(g_MingleLatelyText, fmt::arg("name", p->GetName()),
                                                    fmt::arg("lately", lately));
                if (text != "[Format Error]")
                    out += text;
            }
        }
        if (const std::string things = ThingsText(group); !things.empty())
            out += "\n" + things;
        return out + "\n";
    }

    std::string RelationText(const std::vector<Player*>& group)
    {
        const uint32_t guildId = group.front()->GetGuildId();
        bool same = guildId != 0;
        for (Player* p : group)
            same = same && p->GetGuildId() == guildId;
        if (same && !g_MingleSameGuildText.empty())
        {
            std::vector<std::string> names;
            for (Player* p : group)
                names.push_back(p->GetName());
            Guild* guild = sGuildMgr->GetGuildById(guildId);
            const std::string text = SafeFormat(g_MingleSameGuildText, fmt::arg("names", CrowdText::JoinNames(names)),
                                                fmt::arg("guild", guild ? guild->GetName() : std::string("a guild")));
            if (text != "[Format Error]")
                return text;
        }
        return g_MingleStrangersText;
    }

    // What there is among them for a premise to need: "[mount]" one of them
    // riding one, "[pet]" a pet or companion out, "[loot]" something got
    // lately, "[quest]" a quest handed in lately, "[duel]" two who could go
    // out to the gates and duel there now (mod-playerbots-plus), and where
    // that is -- and their side, "[horde]" or "[alliance]" (only one side
    // mingles together).
    std::set<std::string> FactsOf(const std::vector<Player*>& group, std::string& duelWhere)
    {
        std::set<std::string> facts;
        duelWhere.clear();
        if (!group.empty())
            facts.insert(TextRules::SideTag(group.front()->GetTeamId() == TEAM_ALLIANCE));
        if (group.size() == 2 && Hangout_CanGoDuel(group[0], group[1], duelWhere) && !duelWhere.empty())
            facts.insert("duel");
        for (Player* p : group)
        {
            const TextRules::Animals a = Pets_Seen(p).animals;
            if (!a.mount.empty())
                facts.insert("mount");
            if (!a.petKind.empty() || !a.companion.empty())
                facts.insert("pet");
            const LatelyNamed named = Lately_Named(p);
            if (named.item)
                facts.insert("loot");
            if (named.quest)
                facts.insert("quest");
        }
        return facts;
    }

    // One that fits the lowest level among them and what they have, not one
    // heard lately. Two who could go and duel talk about that
    // Mingle.DuelChance of the time. `duel`: the one picked is the duel's.
    std::string PickPremise(uint32_t level, const std::set<std::string>& facts, bool& duel)
    {
        duel = false;
        const bool duelFirst = facts.count("duel") && Between(1, 100) <= g_MingleDuelChance;
        std::vector<std::string> fit, any, duels;
        std::set<std::string> isDuel;
        for (std::string p : g_MinglePremises)
        {
            bool there = true;
            const std::vector<std::string> tags = TextRules::TakePremiseTags(p);
            for (const std::string& tag : tags)
                there = there && facts.count(tag) != 0;
            if (!there || !TextRules::PremiseFits(p, level))
                continue;
            if (std::find(tags.begin(), tags.end(), "duel") != tags.end())
            {
                isDuel.insert(p);
                duels.push_back(p);
            }
            any.push_back(p);
            if (std::find(g_recentPremises.begin(), g_recentPremises.end(), p) == g_recentPremises.end())
                fit.push_back(p);
        }
        const std::vector<std::string>& from = (duelFirst && !duels.empty()) ? duels : (fit.empty() ? any : fit);
        if (from.empty())
            return {};
        const std::string pick = from[Between(0, uint32_t(from.size() - 1))];
        duel = isDuel.count(pick) != 0;
        g_recentPremises.push_back(pick);
        while (g_recentPremises.size() > std::min<size_t>(8, g_MinglePremises.size() / 2 + 1))
            g_recentPremises.pop_front();
        return pick;
    }

    // Their side and its cities, and when they stand in one of those, that
    // they are home there: no "too many guards about" from a Horde pair in
    // Orgrimmar. Only one side mingles together, so the first speaks for all.
    std::string SideText(Player* bot)
    {
        const bool alliance = bot->GetTeamId() == TEAM_ALLIANCE;
        const char* side = alliance ? "Alliance" : "Horde";
        std::string out;
        const std::string cities = Outlook_SideCities(alliance);
        if (!g_MingleSideText.empty() && !cities.empty())
        {
            const std::string text = SafeFormat(g_MingleSideText, fmt::arg("side", side), fmt::arg("cities", cities));
            if (text != "[Format Error]")
                out += " " + text;
        }
        const std::string city = g_MingleHomeCityText.empty() ? std::string() : Outlook_HomeCity(bot);
        if (!city.empty())
        {
            const std::string text = SafeFormat(g_MingleHomeCityText, fmt::arg("city", city), fmt::arg("side", side));
            if (text != "[Format Error]")
                out += " " + text;
        }
        return out;
    }

    std::string BuildPrompt(const std::vector<Player*>& group, bool& duel)
    {
        uint32_t level = group.front()->GetLevel();
        std::vector<std::string> names;
        for (Player* p : group)
        {
            level = std::min<uint32_t>(level, p->GetLevel());
            names.push_back(p->GetName());
        }

        std::string duelWhere;
        std::string premise = PickPremise(level, FactsOf(group, duelWhere), duel);
        if (premise.empty())
            return {};
        // Where they will go and fight it out: the real place, "outside the
        // gates of Stormwind".
        for (size_t at = premise.find("{duel_where}"); at != std::string::npos; at = premise.find("{duel_where}"))
            premise.replace(at, 12, duelWhere);

        // The shape of it, with their own names. An emote is in it only now
        // and then, picked each time: shown one every time, every exchange
        // opened with one.
        std::string example = "\n" + names[0] + ": <what " + names[0] + " types>\n";
        if (g_MingleMaxEmotes > 0 && !g_MingleEmotes.empty() && Between(1, 100) <= 30)
            example += names[1] + ": /" + g_MingleEmotes[Between(0, uint32_t(g_MingleEmotes.size() - 1))] + "\n";
        example += names[1] + ": <what " + names[1] + " types>\n";

        std::string emotes;
        for (const std::string& e : g_MingleEmotes)
            emotes += (emotes.empty() ? "/" : ", /") + e;

        // Mostly short: two dice, the lower one.
        const uint32_t lines = std::min(Between(g_MingleMinLines, g_MingleMaxLines),
                                        Between(g_MingleMinLines, g_MingleMaxLines));

        const std::string prompt = SafeFormat(g_MinglePromptTemplate,
                                              fmt::arg("count", group.size() >= 3 ? "Three" : "Two"),
                                              fmt::arg("where", Where(group.front())),
                                              fmt::arg("when", Clock_NowText()),
                                              fmt::arg("people", PeopleText(group)),
                                              fmt::arg("relation", RelationText(group)),
                                              fmt::arg("premise", premise),
                                              fmt::arg("example", example),
                                              fmt::arg("lines", lines),
                                              fmt::arg("emotes", emotes),
                                              fmt::arg("max_emotes", g_MingleMaxEmotes));
        if (prompt == "[Format Error]")
            return {};
        // "[Are We There, Yeti?]" is linked when the line goes out, as any
        // other bot line is (mod-ollama-chat_links).
        std::string out = prompt + SideText(group.front());
        if (g_LinksEnable && !g_MingleLinksText.empty())
            out += " " + g_MingleLinksText;
        return out;
    }

    // ---- running one ------------------------------------------------------

    uint32_t TypingMs(const std::string& text)
    {
        if (!g_EnableTypingSimulation)
            return 800;
        uint32_t ms = g_TypingSimulationBaseDelay + static_cast<uint32_t>(text.size()) * g_TypingSimulationDelayPerChar;
        if (g_TypingSimulationMaxDelay > 0)
            ms = std::min(ms, g_TypingSimulationMaxDelay);
        return ms;
    }

    // From the line before landing to this one: read it, then type.
    uint32_t GapBefore(const Exchange& e, size_t i)
    {
        const Line& l = e.lines[i];
        if (l.text.empty())
            return Between(700, 1800);   // an emote is quick
        uint32_t read = Between(g_MingleReadMinMs, g_MingleReadMaxMs);
        if (i > 0 && e.lines[i - 1].speaker == l.speaker)
            read /= 3;                     // the same one carrying on
        return read + TypingMs(l.text);
    }

    // Who each line is aimed at: whoever spoke last before it, or for the
    // first, whoever speaks next.
    void AimLines(Exchange& e)
    {
        for (size_t i = 0; i < e.lines.size(); ++i)
        {
            Line& l = e.lines[i];
            l.target = (l.speaker + 1) % e.members.size();
            bool found = false;
            for (size_t j = i; j-- > 0;)
                if (e.lines[j].speaker != l.speaker)
                {
                    l.target = e.lines[j].speaker;
                    found = true;
                    break;
                }
            if (!found)
                for (size_t j = i + 1; j < e.lines.size(); ++j)
                    if (e.lines[j].speaker != l.speaker)
                    {
                        l.target = e.lines[j].speaker;
                        break;
                    }
        }
    }

    std::string Clip(std::string text)
    {
        const size_t max = g_MaxReplyLength > 0 ? g_MaxReplyLength : 160;
        if (text.size() <= max)
            return text;
        size_t cut = text.rfind(' ', max);
        if (cut == std::string::npos || cut < max / 2)
            cut = max;
        text.resize(cut);
        while (!text.empty() && (text.back() == ' ' || text.back() == ','))
            text.pop_back();
        return text;
    }

    void Hold(const Exchange& e)
    {
        for (size_t i = 0; i < e.members.size(); ++i)
        {
            if (!e.held[i])
                continue;
            if (Player* p = Find(e.members[i]))
                if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p))
                    ai->SetNextCheckDelay(kHoldMs);
        }
    }

    // Done: its bots go back to what they were doing, and rest a while.
    void End(std::unordered_map<uint64_t, Exchange>::iterator it, bool natural, const char* why)
    {
        Exchange& e = it->second;
        const auto rest = Clock::now() + std::chrono::seconds(g_MingleBotCooldownSeconds);
        for (size_t i = 0; i < e.members.size(); ++i)
        {
            g_busy.erase(e.members[i]);
            g_botRest[e.members[i]] = rest;
            if (!e.held[i])
                continue;
            if (Player* p = Find(e.members[i]))
                if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p))
                    ai->SetNextCheckDelay(0);
        }
        // Talked themselves into a duel, and said all of it: out to the
        // gates they go, to fight it out there (mod-playerbots-plus).
        if (natural && e.duel && e.members.size() == 2)
        {
            Player* a = Find(e.members[0]);
            Player* b = Find(e.members[1]);
            const bool off = a && b && Hangout_GoDuel(a, b);
            if (off)
                ++g_stats.duels;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Mingle {}: {} and {} {}.", e.id, e.names[0], e.names[1],
                         off ? "head out to duel" : "talked of a duel, but cannot go just now");
        }
        if (!e.waiting)
            ++(natural ? g_stats.finished : g_stats.cut);
        if (g_DebugEnabled)
        {
            std::string who;
            for (const std::string& n : e.names)
                who += (who.empty() ? "" : ", ") + n;
            LOG_INFO("module.ollamachat", "[Ollama Chat] Mingle {} ({}) over after {} of {} lines: {}.", e.id, who,
                     e.next, e.lines.size(), why);
        }
        g_exchanges.erase(it);
    }

    // Anything that ends it early. Null: carry on. With `world`, also whether
    // a real player can still hear it.
    const char* Trouble(const Exchange& e, const OllamaWorldSnapshot* world)
    {
        std::vector<Player*> ps;
        for (uint64_t guid : e.members)
        {
            Player* p = Find(guid);
            if (!p || p->GetMapId() != e.mapId || Wanted(p))
                return "one of them is wanted or gone";
            ps.push_back(p);
        }
        for (size_t i = 0; i < ps.size(); ++i)
            for (size_t j = i + 1; j < ps.size(); ++j)
                if (ps[i]->GetExactDist(ps[j]) > g_MinglePairRange + 12.0f)
                    return "they walked apart";
        if (world)
        {
            bool heard = false;
            for (Player* p : ps)
                heard = heard || world->RealPlayerWithin(p, g_MingleHearRange + 10.0f);
            if (!heard)
                return "nobody in earshot";
        }
        return nullptr;
    }

    // The next line goes out. False: it was held back, or someone is gone.
    bool SayNext(Exchange& e, const OllamaWorldSnapshot& world)
    {
        const Line& l = e.lines[e.next];
        Player* speaker = Find(e.members[l.speaker]);
        Player* target  = Find(e.members[l.target]);
        if (!speaker || !target)
            return false;

        if (l.text.empty())
        {
            // "Grigohm laughs at Thimack." -- and the transcript knows it.
            ScheduleBotExpression(speaker, target->GetGUID(), l.emoteId, 0);
            const std::string name = LookupTextEmoteName(l.emoteId);
            if (!name.empty())
                Context_RecordEvent(speaker, speaker->GetName() + " did /" + name + " at " + target->GetName(), false);
            ++g_stats.emotes;
            return true;
        }

        OllamaChatRequest r;
        r.botGuid           = e.members[l.speaker];
        r.targetGuid        = e.members[l.target];
        r.source            = SRC_SAY_LOCAL;
        r.scopeKey          = e.scopeKey;
        r.fixedText         = l.text;
        r.botName           = speaker->GetName();
        r.originMessage     = e.lastText;
        r.kind              = OllamaRequestKind::RandomChatter;
        r.chainDepth        = 1;       // a bot talking to a bot
        r.directAddress     = true;    // its place in the exchange paces it, not the cooldowns
        r.noEcho            = true;
        r.gentle            = true;    // nothing rude with it
        r.triggerBotReplies = false;   // the others in earshot leave them to it
        r.mingle            = true;
        if (!OllamaDispatch_DeliverNow(std::move(r), l.emoteId, world))
            return false;

        if (l.emoteId)
            ++g_stats.emotes;
        ++g_stats.lines;
        e.lastText = l.text;
        ScheduleBotExpression(target, speaker->GetGUID(), 0, 400);   // the one spoken to looks round
        return true;
    }

    // The written exchange came back.
    void OnScript(const OllamaScriptResult& r, Clock::time_point now)
    {
        auto it = g_exchanges.find(r.scriptId);
        if (it == g_exchanges.end() || !it->second.waiting)
            return;   // over while it was being written
        Exchange& e = it->second;

        // A line or two past the length it was asked for, to finish properly,
        // is let through: an exchange cut off before its goodbye is worse.
        std::vector<TextRules::ScriptLine> parsed;
        if (r.ok)
            parsed = TextRules::ParseScript(r.text, e.names, g_MingleMaxLines + 4,
                                            [](const std::string& w) { return LookupTextEmoteId(w) != 0; });

        // Out of tokens mid-line: that last line is not said half-typed.
        if (!parsed.empty() && r.text.size() >= size_t(g_MingleNumPredict) * 3)
        {
            const std::string& tail = parsed.back().text;
            if (!tail.empty() && std::string(".!?)~:").find(tail.back()) == std::string::npos)
                parsed.pop_back();
        }

        uint32_t emotes = 0;
        for (const TextRules::ScriptLine& s : parsed)
        {
            uint32_t emote = s.emote.empty() ? 0 : LookupTextEmoteId(s.emote);
            if (emote && (g_PresenceSkipEmotes.count(emote) || emotes >= g_MingleMaxEmotes))
                emote = 0;
            // In roleplay mode a line with out-of-world talk in it is not said.
            const std::string text = g_RoleplayEnable ? Roleplay_FilterMetaTerms(Clip(s.text)) : Clip(s.text);
            if (text.empty() && !emote)
                continue;
            if (emote)
                ++emotes;
            Line l;
            l.speaker = s.speaker;
            l.text    = text;
            l.emoteId = emote;
            e.lines.push_back(std::move(l));
        }

        // Two people saying something to each other, or nothing.
        std::vector<bool> spoke(e.members.size(), false);
        size_t said = 0, speakers = 0;
        for (const Line& l : e.lines)
            if (!l.text.empty())
            {
                ++said;
                if (!spoke[l.speaker])
                {
                    spoke[l.speaker] = true;
                    ++speakers;
                }
            }
        if (said < 2 || speakers < 2)
        {
            ++g_stats.unusable;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Mingle {}: nothing usable came back{}", e.id,
                         r.ok ? ": '" + r.text.substr(0, 300) + "'" : std::string());
            e.lines.clear();
            End(it, false, "nothing usable");
            return;
        }

        ++g_stats.written;
        AimLines(e);
        e.waiting = false;
        e.next    = 0;
        e.nextAt  = now + std::chrono::milliseconds(e.lines.front().text.empty() ? Between(300, 900)
                                                                                 : TypingMs(e.lines.front().text));
    }

    // Two or three bots by `real` get talking, if there are two to talk.
    // False when there are not.
    bool Start(Player* real, Clock::time_point now)
    {
        std::vector<Player*> pool;
        for (Player* p : BotsNear(real, g_MingleHearRange))
            if (Idle(p, real) && Rested(g_botRest, p->GetGUID().GetRawValue(), now) &&
                Governor_BotIsFresh(p->GetGUID()))
                pool.push_back(p);

        const std::vector<Player*> group = PickGroup(pool, g_MinglePairRange, true);
        if (group.size() < 2)
            return false;

        bool duel = false;
        const std::string prompt = BuildPrompt(group, duel);
        if (prompt.empty())
            return false;

        Exchange e;
        e.id       = g_nextId++;
        e.duel     = duel;
        e.mapId    = group.front()->GetMapId();
        e.scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, std::string(), 0,
                                           group.front()->GetZoneId());
        for (Player* p : group)
        {
            e.members.push_back(p->GetGUID().GetRawValue());
            e.names.push_back(p->GetName());
            e.held.push_back(!HangingOut(p));
        }

        if (!OllamaDispatch_SubmitScript(e.id, prompt, g_MingleNumPredict))
            return false;

        e.waiting   = true;
        e.waitUntil = now + std::chrono::seconds(kScriptWaitSec);
        ++g_stats.asked;

        // They stop where they are and turn to each other while it is
        // written; one walking past stops to talk.
        for (size_t i = 0; i < group.size(); ++i)
        {
            Player* p = group[i];
            g_busy[e.members[i]] = e.id;
            if (e.held[i] && p->isMoving())
            {
                p->GetMotionMaster()->Clear();
                p->StopMoving();
            }
            ScheduleBotExpression(p, group[(i + 1) % group.size()]->GetGUID(), 0, Between(200, 900));
        }
        Hold(e);

        if (g_DebugEnabled)
        {
            std::string who;
            for (const std::string& n : e.names)
                who += (who.empty() ? "" : ", ") + n;
            LOG_INFO("module.ollamachat", "[Ollama Chat] Mingle {}: {} get talking near {}.", e.id, who,
                     real->GetName());
        }
        g_exchanges.emplace(e.id, std::move(e));
        return true;
    }

    // Two bots near `real` greet each other without a word, and often one back.
    void Greet(Player* real, Clock::time_point now)
    {
        if (g_MingleGreetings.empty())
            return;
        std::vector<Player*> pool;
        for (Player* p : BotsNear(real, g_MingleHearRange))
            if (Idle(p, real) && Rested(g_nodRest, p->GetGUID().GetRawValue(), now))
                pool.push_back(p);

        const std::vector<Player*> pair = PickGroup(pool, g_MingleEmoteRange, false);
        if (pair.size() < 2)
            return;

        Player* a = pair[0];
        Player* b = pair[1];
        const uint32_t first = g_MingleGreetings[Between(0, uint32_t(g_MingleGreetings.size() - 1))];
        ScheduleBotExpression(a, b->GetGUID(), first, 0);
        if (Between(1, 100) <= g_MingleEmoteReturnChance)
        {
            uint32_t back = GetMirrorEmote(first);
            if (!back || g_PresenceSkipEmotes.count(back))
                back = g_MingleGreetings[Between(0, uint32_t(g_MingleGreetings.size() - 1))];
            ScheduleBotExpression(b, a->GetGUID(), back, Between(1200, 2600));
        }
        const auto rest = now + std::chrono::seconds(g_MingleEmoteBotCooldownSeconds);
        g_nodRest[a->GetGUID().GetRawValue()] = rest;
        g_nodRest[b->GetGUID().GetRawValue()] = rest;
        ++g_stats.greetings;
    }

    uint32_t RunningNear(Player* real)
    {
        uint32_t n = 0;
        for (const auto& [id, e] : g_exchanges)
        {
            Player* m = Find(e.members.front());
            if (m && m->GetMapId() == real->GetMapId() && m->GetExactDist(real) <= g_MingleHearRange * 2.0f)
                ++n;
        }
        return n;
    }

    // Rest times run out; past ones are forgotten.
    void Prune(std::unordered_map<uint64_t, Clock::time_point>& times, Clock::time_point now)
    {
        if (times.size() < 2048)
            return;
        for (auto it = times.begin(); it != times.end();)
            it = it->second < now ? times.erase(it) : std::next(it);
    }

    void Look(Clock::time_point now)
    {
        Prune(g_botRest, now);
        Prune(g_nodRest, now);

        OllamaWorldSnapshot world;
        world.Build();

        // Over when nobody can hear it any more, or the place is not fit for it.
        for (auto it = g_exchanges.begin(); it != g_exchanges.end();)
        {
            auto here = it++;
            if (const char* why = Trouble(here->second, &world))
                End(here, false, why);
        }

        for (Player* real : world.realPlayers)
        {
            if (!real->IsInWorld() || !real->IsAlive() || real->IsInCombat() || real->IsBeingTeleported() ||
                real->IsInFlight())
                continue;
            if (Map* map = real->GetMap(); !map || map->Instanceable())
                continue;
            if (g_MingleCitiesOnly && !OllamaIsSocialHub(real))
                continue;
            const uint64_t key = real->GetGUID().GetRawValue();

            // The first of each a while after someone turns up, then at
            // random intervals of MinSeconds..MaxSeconds (GapMin..GapMax).
            if (g_MingleEmoteEnable &&
                Due(g_playerNextNod, key, now, std::min<uint32_t>(60, g_MingleEmoteMinSeconds), g_MingleEmoteMinSeconds))
            {
                g_playerNextNod[key] = now + std::chrono::seconds(Between(g_MingleEmoteMinSeconds, g_MingleEmoteMaxSeconds));
                if (Between(1, 100) <= g_MingleEmoteChance)
                    Greet(real, now);
            }

            if (!Due(g_playerNext, key, now, std::min<uint32_t>(60, g_MingleGapMinSeconds), g_MingleGapMinSeconds))
                continue;
            const auto gap   = std::chrono::seconds(Between(g_MingleGapMinSeconds, g_MingleGapMaxSeconds));
            const auto retry = std::chrono::seconds(Between(45, 90));
            // Not now -- one going on near them already, the model busy, nobody
            // free to talk: a minute or so on, not a whole gap.
            if (g_DisableForSayYell || g_exchanges.size() >= g_MingleMaxActive ||
                OllamaDispatch_QueueDepth() > g_MingleQueueLimit || RunningNear(real) >= g_MingleMaxNearPlayer)
            {
                g_playerNext[key] = now + retry;
                continue;
            }
            if (Between(1, 100) > g_MingleStartChance)
            {
                g_playerNext[key] = now + gap;
                continue;
            }
            g_playerNext[key] = now + (Start(real, now) ? gap : retry);
        }
    }

    void ApplyForgets()
    {
        if (!g_forgetPending.exchange(false))
            return;
        std::vector<uint64_t> gone;
        {
            std::lock_guard<std::mutex> lock(g_forgetMutex);
            gone.swap(g_forget);
        }
        for (uint64_t guid : gone)
        {
            auto busy = g_busy.find(guid);
            if (busy != g_busy.end())
            {
                auto it = g_exchanges.find(busy->second);
                if (it != g_exchanges.end())
                    End(it, false, "one of them logged out");
            }
            g_busy.erase(guid);
            g_botRest.erase(guid);
            g_nodRest.erase(guid);
            g_playerNext.erase(guid);
            g_playerNextNod.erase(guid);
        }
    }

    void EndAll(const char* why)
    {
        while (!g_exchanges.empty())
            End(g_exchanges.begin(), false, why);
    }
}

// --------------------------------------------------------------------------

void Mingle_Update(uint32_t /*diff*/)
{
    ApplyForgets();

    // Written exchanges that came back -- taken even when switched off, so
    // none pile up.
    std::vector<OllamaScriptResult> scripts = OllamaDispatch_TakeScripts();

    if (!g_Enable || !g_MingleEnable)
    {
        if (!g_exchanges.empty())
            EndAll("switched off");
        return;
    }

    const auto now = Clock::now();
    for (const OllamaScriptResult& r : scripts)
        OnScript(r, now);

    if (!g_exchanges.empty())
    {
        OllamaWorldSnapshot world;
        bool built = false;
        for (auto it = g_exchanges.begin(); it != g_exchanges.end();)
        {
            auto here = it++;
            Exchange& e = here->second;

            if (e.waiting)
            {
                if (now >= e.waitUntil)
                    End(here, false, "it was never written");
                else
                    Hold(e);
                continue;
            }
            Hold(e);
            if (now < e.nextAt)
                continue;

            if (const char* why = Trouble(e, nullptr))
            {
                End(here, false, why);
                continue;
            }
            if (!built)
            {
                world.Build();
                built = true;
            }
            if (!SayNext(e, world))
            {
                End(here, false, "a line was held back");
                continue;
            }
            if (++e.next >= e.lines.size())
            {
                End(here, true, "done");
                continue;
            }
            e.nextAt = now + std::chrono::milliseconds(GapBefore(e, e.next));
        }
    }

    if (now >= g_nextLook)
    {
        g_nextLook = now + std::chrono::milliseconds(kLookMs);
        Look(now);
    }
}

void Mingle_NoteHumanSay(Player* player, const std::string& msg, uint64_t addresseeGuid)
{
    if (!player || g_exchanges.empty())
        return;
    for (auto it = g_exchanges.begin(); it != g_exchanges.end();)
    {
        auto here = it++;
        const Exchange& e = here->second;
        bool aimed = false;
        for (size_t i = 0; i < e.members.size() && !aimed; ++i)
            aimed = e.members[i] == addresseeGuid ||
                    OllamaFindBotNameMention(msg, e.names[i]) != std::string::npos;
        if (!aimed)
            continue;
        Player* m = Find(e.members.front());
        if (m && m->GetMapId() == player->GetMapId() && m->GetExactDist(player) <= g_MingleHearRange + 10.0f)
            End(here, false, "a real player spoke to one of them");
    }
}

bool Mingle_IsBusy(Player* bot)
{
    return bot && !g_busy.empty() && g_busy.count(bot->GetGUID().GetRawValue()) != 0;
}

void Mingle_ForgetPlayer(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(g_forgetMutex);
    if (g_forget.size() < 4096)
        g_forget.push_back(guid);
    g_forgetPending.store(true);
}

void Mingle_Reset()
{
    EndAll("reset");
    g_busy.clear();
    g_botRest.clear();
    g_playerNext.clear();
    g_playerNextNod.clear();
    g_nodRest.clear();
    g_recentPremises.clear();
}

MingleStats Mingle_GetStats()
{
    MingleStats s = g_stats;
    s.running = static_cast<uint32_t>(g_exchanges.size());
    return s;
}
