#include "mod-ollama-chat_mingle.h"
#include "mod-ollama-chat_citysay.h"
#include "mod-ollama-chat_clock.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_gear.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_knowledge.h"
#include "mod-ollama-chat_lately.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_pets.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_presence.h"
#include "mod-ollama-chat_professions.h"
#include "mod-ollama-chat_race.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "Config.h"
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
#include "QuestDef.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

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
    constexpr uint32_t kGatherSeconds = 240;    // those hanging out stay put this long at most (till it ends)

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
        bool                     structured = false;   // asked for as JSON (Mingle.Structured)
        Clock::time_point        waitUntil{};
        std::vector<Line>        lines;
        size_t                   next = 0;
        Clock::time_point        nextAt{};
        std::string              lastText;  // the line before: what the next one answers
        bool                     duel = false;   // they talk themselves into a duel: at the end, off they go
        uint32_t                 groupQuest = 0;   // ...into a group quest: at the end, off they go and do it
        std::string              premise;   // what got them talking, with its facts: each line goes on it
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

    // Mingle.Structured: the exchange asked for as JSON -- {"lines": [{"who":
    // a name, "says": what they type}, ...]} -- where the server can hold the
    // model to that shape, so a line cannot come back in a form the reader
    // does not know. What is read out of it is the same "Name: words" lines
    // the plain form has, and is read the same way from there.
    bool        g_structured = false;
    std::string g_structuredText;

    std::string SchemaFor(const std::vector<std::string>& names)
    {
        const nlohmann::json line = {
            { "type", "object" },
            { "properties", { { "who", { { "type", "string" }, { "enum", names } } },
                              { "says", { { "type", "string" } } } } },
            { "required", { "who", "says" } },
            { "additionalProperties", false },
        };
        const nlohmann::json schema = {
            { "type", "object" },
            { "properties", { { "lines", { { "type", "array" }, { "items", line } } } } },
            { "required", { "lines" } },
            { "additionalProperties", false },
        };
        return schema.dump();
    }

    // One {"who": ..., "says": ...} as a "Name: words" line; empty when it is
    // not one. Words that begin with the name and a colon again lose that.
    std::string LineOf(const nlohmann::json& entry)
    {
        if (!entry.is_object() || !entry.contains("who") || !entry.contains("says") ||
            !entry["who"].is_string() || !entry["says"].is_string())
            return {};
        const std::string who = entry["who"].get<std::string>();
        std::string says = entry["says"].get<std::string>();
        std::replace(says.begin(), says.end(), '\n', ' ');
        if (says.size() > who.size() + 1 && says.compare(0, who.size(), who) == 0 && says[who.size()] == ':')
            says.erase(0, who.size() + 1);
        says.erase(0, says.find_first_not_of(' '));
        return who.empty() ? std::string() : who + ": " + says;
    }

    // The exchange as lines, from what came back for Mingle.Structured. Text
    // that is not that JSON at all is handed back as it is: the server could
    // not hold the model to a shape, and it wrote the plain form. JSON cut
    // off part way (out of tokens) gives the entries that are whole.
    std::string FromStructured(const std::string& text)
    {
        std::string out;
        const nlohmann::json whole = nlohmann::json::parse(text, nullptr, false);
        if (!whole.is_discarded() && whole.is_object() && whole.contains("lines") && whole["lines"].is_array())
        {
            for (const nlohmann::json& entry : whole["lines"])
                if (const std::string line = LineOf(entry); !line.empty())
                    out += line + "\n";
            return out;
        }
        size_t at = text.find("\"lines\"");
        if (at == std::string::npos)
            return text;
        while ((at = text.find('{', at + 1)) != std::string::npos)
        {
            // To the first closing brace that makes it whole (one inside the
            // words is passed over).
            for (size_t end = text.find('}', at); end != std::string::npos; end = text.find('}', end + 1))
            {
                const nlohmann::json entry = nlohmann::json::parse(text.substr(at, end - at + 1), nullptr, false);
                if (entry.is_discarded())
                    continue;
                if (const std::string line = LineOf(entry); !line.empty())
                    out += line + "\n";
                at = end;
                break;
            }
        }
        return out;
    }

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
        return bot->GetTeamId() == listener->GetTeamId() && !g_busy.contains(bot->GetGUID().GetRawValue());
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

    // Where a premise is about a real place: "[duel]" where two go to fight
    // it out, "[lost]" one of the town's places and what its guards say of
    // the way there.
    struct PlaceFacts
    {
        std::string duelWhere;
        std::string lostPlace;
        std::string lostWay;
        std::string gear;          // "Thimack's [Arcanite Reaper] (purple two-handed axe)"
        std::string professions;   // "Thimack: mining 150, skinning 120; Grigohm: tailoring 75"
        std::string passersby;     // who is about: "Lianon, an orc warrior, riding their mount, ..."
        uint32_t    groupQuest = 0;   // "[groupquest]": the one they could go and do together
        std::string gqAsker, gqOther, gqQuest, gqFacts;   // ...who asks whom, "[Mor'Ladim]", what it is
    };

    // People-watching is of the people really about, this near.
    constexpr float kPassersbyYards = 30.0f;

    // What there is among them for a premise to need: "[gear]" something one
    // of them wears worth a look, "[prof]" a profession among them (and how
    // far along each is), "[mount]" one of them riding one, "[pet]" a pet or
    // companion out, "[loot]" something got
    // lately, "[quest]" a quest handed in lately, "[duel]" two who could go
    // out to the gates and duel there now (mod-playerbots-plus), and where
    // that is, "[lost]" a town whose guards give directions, and one of its
    // places other than `here`, "[people]" others about to watch (up to
    // three, within 30 yards: {passersby}), "[groupquest]" two who could go
    // and do a group or elite quest of the zone they stand in together now
    // (mod-playerbots-plus) -- and their side, "[horde]" or "[alliance]"
    // (only one side mingles together).
    std::set<std::string> FactsOf(const std::vector<Player*>& group, const std::string& here, PlaceFacts& places)
    {
        std::set<std::string> facts;
        places = {};
        if (!group.empty())
            facts.insert(TextRules::SideTag(group.front()->GetTeamId() == TEAM_ALLIANCE));
        if (group.size() == 2 && Hangout_CanGoDuel(group[0], group[1], places.duelWhere) &&
            !places.duelWhere.empty())
            facts.insert("duel");
        // The quest as itself -- what it is, where, for what level, what it
        // asks and who takes it back -- so neither makes up what it is about.
        if (group.size() == 2)
            if (const uint32_t quest = OllamaGroupQuestFor(group[0], group[1]))
                if (Quest const* q = sObjectMgr->GetQuestTemplate(quest))
                {
                    places.groupQuest = quest;
                    places.gqAsker    = group[0]->GetName();
                    places.gqOther    = group[1]->GetName();
                    places.gqQuest    = "[" + q->GetTitle() + "]";
                    places.gqFacts    = Links_DescribeQuest(q, group[0]);
                    while (!places.gqFacts.empty() && (places.gqFacts.back() == '.' || places.gqFacts.back() == ' '))
                        places.gqFacts.pop_back();   // the prompt puts the full stop after a premise
                    facts.insert("groupquest");
                }
        if (!group.empty() && Knowledge_TownWay(group.front(), here, places.lostPlace, places.lostWay))
            facts.insert("lost");
        for (Player* p : group)
        {
            // Something real to admire, and what they really do: "nice robe"
            // and "how's your tailoring going" were made up with nothing to go on.
            if (places.gear.empty())
                if (const std::string piece = Gear_ShowPiece(p); !piece.empty())
                {
                    places.gear = p->GetName() + "'s " + piece;
                    facts.insert("gear");
                }
            // ...with why, where its leaning chose its pair: "herbalism and
            // mining for the gold", never a reason made up.
            const std::string profs = Professions_Of(p);
            const std::string why = profs.empty() ? std::string() : Professions_WhyOf(p);
            places.professions += (places.professions.empty() ? "" : "; ") + p->GetName() + ": " +
                                  (profs.empty() ? std::string("none") : profs) +
                                  (why.empty() ? std::string() : " (" + why + ")");
            if (!profs.empty())
                facts.insert("prof");
            const TextRules::Animals a = Pets_Seen(p).animals;
            if (!a.mount.empty() && !Pets_MountRemarkedLately(p->GetGUID()))   // not handed round again
                facts.insert("mount");
            if (!a.petKind.empty() || !a.companion.empty())
                facts.insert("pet");
            const LatelyNamed named = Lately_Named(p);
            if (named.item)
                facts.insert("loot");
            if (named.quest)
                facts.insert("quest");
        }
        // "[people]": who is really about to watch -- "people-watching: the
        // players walking past" had them make up who was walking past.
        if (!group.empty() && group.front()->GetMap())
        {
            Player* first = group.front();
            std::vector<std::string> seen;
            for (auto const& ref : first->GetMap()->GetPlayers())
            {
                Player* p = ref.GetSource();
                if (!p || !p->IsInWorld() || !p->IsAlive() || p->IsGameMaster() ||
                    std::find(group.begin(), group.end(), p) != group.end() ||
                    first->GetDistance(p) > kPassersbyYards)
                    continue;
                // Without their mount: talked up by one of the pair, the other
                // answered for it -- "nice chestnut mare that is", "she's a
                // beauty, i suppose", of a passer-by's (mod-ollama-chat_mounts).
                if (const std::string who = Presence_Describe(p, first, nullptr, false); !who.empty())
                    seen.push_back(who);
                if (seen.size() >= 3)
                    break;
            }
            for (const std::string& who : seen)
                places.passersby += (places.passersby.empty() ? "" : "; ") + who;
            if (!seen.empty())
                facts.insert("people");
        }
        return facts;
    }

    // One that fits the lowest level among them and what they have, not one
    // heard lately. Two who could go and duel talk about that
    // Mingle.DuelChance of the time, two who could go and do a group quest
    // together Mingle.GroupQuestChance of the time -- and out questing
    // (`questOnly`), of nothing else. `duel`, `groupQuest`: the one picked is
    // the duel's, the group quest's.
    std::string PickPremise(uint32_t level, const std::set<std::string>& facts, bool questOnly, bool& duel,
                            bool& groupQuest)
    {
        duel = groupQuest = false;
        const bool duelFirst = facts.contains("duel") && Between(1, 100) <= g_MingleDuelChance;
        const bool questFirst =
            facts.contains("groupquest") && (questOnly || Between(1, 100) <= g_MingleGroupQuestChance);
        std::vector<std::string> fit, any, duels, quests;
        std::set<std::string> isDuel, isQuest;
        for (std::string p : g_MinglePremises)
        {
            bool there = true;
            const std::vector<std::string> tags = TextRules::TakePremiseTags(p);
            for (const std::string& tag : tags)
                there = there && facts.contains(tag);
            if (!there || !TextRules::PremiseFits(p, level))
                continue;
            const bool forQuest = std::find(tags.begin(), tags.end(), "groupquest") != tags.end();
            if (questOnly && !forQuest)
                continue;
            if (forQuest)
            {
                isQuest.insert(p);
                quests.push_back(p);
            }
            if (std::find(tags.begin(), tags.end(), "duel") != tags.end())
            {
                isDuel.insert(p);
                duels.push_back(p);
            }
            any.push_back(p);
            if (std::find(g_recentPremises.begin(), g_recentPremises.end(), p) == g_recentPremises.end())
                fit.push_back(p);
        }
        const std::vector<std::string>& from = (questFirst && !quests.empty()) ? quests
                                             : (duelFirst && !duels.empty())   ? duels
                                             : (fit.empty() ? any : fit);
        if (from.empty())
            return {};
        const std::string pick = from[Between(0, uint32_t(from.size() - 1))];
        duel = isDuel.contains(pick);
        groupQuest = isQuest.contains(pick);
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

    // What chat is like where they stand, if it comes up: in every player's
    // own chat window, and as busy as it has really been. Written all at once,
    // an exchange cannot be told it only when chat comes up, as a reply is.
    std::string ChatText(Player* bot)
    {
        if (g_MingleChatText.empty())
            return {};
        const std::string state = Outlook_ChatState(bot);
        if (state.empty())
            return {};
        const std::string text = SafeFormat(g_MingleChatText, fmt::arg("chat", state));
        if (text == "[Format Error]")
            return {};
        Outlook_NoteChatPrompt();
        return " " + text;
    }

    // `about`: what gets them talking, the facts filled in -- what each line
    // of it goes on (mod-ollama-chat_said). `groupQuest`: the quest they talk
    // each other into doing together, when that is it; 0 otherwise.
    std::string BuildPrompt(const std::vector<Player*>& group, bool questOnly, bool& duel, uint32_t& groupQuest,
                            std::string& about)
    {
        groupQuest = 0;
        uint32_t level = group.front()->GetLevel();
        std::vector<std::string> names;
        for (Player* p : group)
        {
            level = std::min<uint32_t>(level, p->GetLevel());
            names.push_back(p->GetName());
        }

        // {at} reads after "standing near each other": "in the Trade District
        // in Stormwind", "outside the bank in Orgrimmar".
        const std::string where = Where(group.front());

        PlaceFacts places;
        bool forQuest = false;
        std::string premise = PickPremise(level, FactsOf(group, where, places), questOnly, duel, forQuest);
        if (premise.empty())
            return {};
        if (forQuest)
            groupQuest = places.groupQuest;
        // The real places in it: where they will go and fight it out,
        // "outside the gates of Stormwind"; the one somebody cannot find, and
        // what the guards say of the way there.
        auto fill = [&premise](const std::string& slot, const std::string& value)
        {
            for (size_t at = premise.find(slot); at != std::string::npos; at = premise.find(slot, at + value.size()))
                premise.replace(at, slot.size(), value);
        };
        fill("{duel_where}", places.duelWhere);
        fill("{lost_place}", places.lostPlace);
        fill("{lost_way}", places.lostWay);
        fill("{gear}", places.gear);
        fill("{professions}", places.professions);
        fill("{passersby}", places.passersby);
        fill("{gq_asker}", places.gqAsker);
        fill("{gq_other}", places.gqOther);
        fill("{gq_quest}", places.gqQuest);
        fill("{gq_facts}", places.gqFacts);
        about = premise;

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
                                              fmt::arg("where", where),
                                              fmt::arg("at", TextRules::AtPlace(where)),
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
        std::string out = prompt + SideText(group.front()) + ChatText(group.front());
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
            if (Player* p = Find(e.members[i]))
                Hangout_Busy(p, 0);   // done talking: free to shift about again
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
        // Talked each other into a group quest, and said all of it: off they
        // go and do it together, the one who asked leading (mod-playerbots-plus).
        if (natural && e.groupQuest && e.members.size() == 2)
        {
            Player* a = Find(e.members[0]);
            Player* b = Find(e.members[1]);
            const bool off = a && b && OllamaGroupQuestTogether(a, b, e.groupQuest);
            if (off)
                ++g_stats.groupQuests;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] Mingle {}: {} and {} {} quest {}.", e.id, e.names[0],
                         e.names[1], off ? "go and do" : "talked of, but cannot go and do just now,", e.groupQuest);
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
        // Asked about it later, it knows what they were on about.
        if (!e.premise.empty())
            r.basis = "chatting in /say with " + target->GetName() + " about " + e.premise;
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
        const bool structured = e.structured;
        if (r.ok)
            parsed = TextRules::ParseScript(structured ? FromStructured(r.text) : r.text, e.names,
                                            g_MingleMaxLines + 4,
                                            [](const std::string& w) { return LookupTextEmoteId(w) != 0; });

        // Out of tokens mid-line: that last line is not said half-typed. (As
        // JSON, an entry cut short is not read at all.)
        if (!structured && !parsed.empty() && r.text.size() >= size_t(g_MingleNumPredict) * 3)
        {
            const std::string& tail = parsed.back().text;
            if (!tail.empty() && std::string(".!?)~:").find(tail.back()) == std::string::npos)
                parsed.pop_back();
        }

        uint32_t emotes = 0;
        for (const TextRules::ScriptLine& s : parsed)
        {
            uint32_t emote = s.emote.empty() ? 0 : LookupTextEmoteId(s.emote);
            if (emote && (g_PresenceSkipEmotes.contains(emote) || emotes >= g_MingleMaxEmotes))
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

    // Out questing: two of `pool` standing by each other who could go and do
    // a group quest of the zone together now (mod-playerbots-plus) -- the one
    // that would ask first. Empty when no two could.
    std::vector<Player*> PickQuestPair(std::vector<Player*> pool)
    {
        if (pool.size() < 2)
            return {};
        std::shuffle(pool.begin(), pool.end(), Rng());
        // A little further apart than a chat in town: out questing, two at
        // the same camp stand a few yards off.
        const float range = g_MinglePairRange + 6.0f;
        for (size_t i = 0; i < pool.size() && i < 8; ++i)
            for (size_t j = 0; j < pool.size() && j < 8; ++j)
            {
                Player* a = pool[i];
                Player* b = pool[j];
                if (a != b && a->GetMapId() == b->GetMapId() && a->GetExactDist(b) <= range &&
                    OllamaGroupQuestFor(a, b))
                    return { a, b };
            }
        return {};
    }

    // Two or three bots by `real` get talking, if there are two to talk.
    // False when there are not. `questOnly`: out questing, where only two
    // who could go and do a group quest together get talking, of that.
    bool Start(Player* real, Clock::time_point now, bool questOnly)
    {
        std::vector<Player*> pool;
        for (Player* p : BotsNear(real, g_MingleHearRange))
            if (Idle(p, real) && Rested(g_botRest, p->GetGUID().GetRawValue(), now) &&
                Governor_BotIsFresh(p->GetGUID()))
                pool.push_back(p);

        const std::vector<Player*> group = questOnly ? PickQuestPair(pool) : PickGroup(pool, g_MinglePairRange, true);
        if (group.size() < 2)
            return false;

        // In a city, /say is not where people talk (mod-ollama-chat_citysay):
        // only when nobody in earshot has heard a bot start something there
        // lately.
        for (Player* p : group)
            if (!CitySay_Free(p))
            {
                ++g_stats.cityHeld;
                return false;
            }

        bool duel = false;
        uint32_t groupQuest = 0;
        std::string about;
        const std::string prompt = BuildPrompt(group, questOnly, duel, groupQuest, about);
        if (prompt.empty() || (questOnly && !groupQuest))
            return false;

        Exchange e;
        e.id         = g_nextId++;
        e.duel       = duel;
        e.groupQuest = groupQuest;
        e.premise    = about;
        e.mapId    = group.front()->GetMapId();
        e.scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, std::string(), 0,
                                           group.front()->GetZoneId());
        for (Player* p : group)
        {
            e.members.push_back(p->GetGUID().GetRawValue());
            e.names.push_back(p->GetName());
            e.held.push_back(!HangingOut(p));
        }

        // As JSON it takes a few tokens more a line.
        e.structured = g_structured;
        const bool sent = e.structured
            ? OllamaDispatch_SubmitScript(e.id, g_structuredText.empty() ? prompt : prompt + " " + g_structuredText,
                                          g_MingleNumPredict + g_MingleNumPredict / 3, g_MingleTemperature,
                                          SchemaFor(e.names))
            : OllamaDispatch_SubmitScript(e.id, prompt, g_MingleNumPredict, g_MingleTemperature);
        if (!sent)
            return false;
        for (Player* p : group)
            CitySay_Claim(p);   // in a city: those in earshot are hearing one now

        e.waiting   = true;
        e.waitUntil = now + std::chrono::seconds(kScriptWaitSec);
        ++g_stats.asked;

        // Those hanging out stand together to talk, the one further off
        // walking over (mod-playerbots-plus's city life), and nobody there
        // wanders off while they talk.
        Hangout_Gather(group, kGatherSeconds);

        // They stop where they are and turn to each other while it is
        // written; one walking past stops to talk. (One walking over turns
        // once it is there: a turn while walking is not made.)
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
            if (!back || g_PresenceSkipEmotes.contains(back))
                back = g_MingleGreetings[Between(0, uint32_t(g_MingleGreetings.size() - 1))];
            ScheduleBotExpression(b, a->GetGUID(), back, Between(1200, 2600));
        }
        const auto rest = now + std::chrono::seconds(g_MingleEmoteBotCooldownSeconds);
        g_nodRest[a->GetGUID().GetRawValue()] = rest;
        g_nodRest[b->GetGUID().GetRawValue()] = rest;
        Hangout_Busy(a, 6);   // greeting each other: neither steps off mid-wave
        Hangout_Busy(b, 6);
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
            // Out of cities and towns (Mingle.CitiesOnly), the only thing two
            // bots out questing get talking about is doing a group quest
            // together, when they could (Mingle.QuestAsks).
            const bool questOnly = g_MingleCitiesOnly && !OllamaIsSocialHub(real);
            if (questOnly && !g_MingleQuestAsks)
                continue;
            const uint64_t key = real->GetGUID().GetRawValue();

            // The first of each a while after someone turns up, then at
            // random intervals of MinSeconds..MaxSeconds (GapMin..GapMax).
            if (!questOnly && g_MingleEmoteEnable &&
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
            g_playerNext[key] = now + (Start(real, now, questOnly) ? gap : retry);
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
    return bot && !g_busy.empty() && g_busy.contains(bot->GetGUID().GetRawValue());
}

void Mingle_ForgetPlayer(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(g_forgetMutex);
    if (g_forget.size() < 4096)
        g_forget.push_back(guid);
    g_forgetPending.store(true);
}

void Mingle_LoadConfig()
{
    g_structured = sConfigMgr->GetOption<bool>("OllamaChat.Mingle.Structured", false);
    g_structuredText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.StructuredText",
        "Give the whole exchange as JSON: an object with a list called lines, one entry for each message in "
        "order, each with who (the speaker's name, exactly as given above) and says (what they type, or the "
        "slash emote on its own).");
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
