#include "mod-ollama-chat_journal.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <utility>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Phrases = std::vector<std::vector<std::string>>;

    constexpr size_t kSubjectMax = 120;   // the table's columns
    constexpr size_t kDetailMax  = 400;
    constexpr char   kListSep    = '|';   // between the bosses of a run, in `detail`

    const char* const kCreateTable =
        "CREATE TABLE IF NOT EXISTS mod_ollama_chat_journal ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        "bot_guid BIGINT UNSIGNED NOT NULL, "
        "player_guid BIGINT UNSIGNED NOT NULL, "
        "kind TINYINT UNSIGNED NOT NULL, "
        "subject VARCHAR(120) NOT NULL DEFAULT '', "
        "detail VARCHAR(400) NOT NULL DEFAULT '', "
        "times INT UNSIGNED NOT NULL DEFAULT 1, "
        "wipes INT UNSIGNED NOT NULL DEFAULT 0, "
        "first_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "last_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "INDEX idx_pair (bot_guid, player_guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

    // One thing a bot and a person did together, or one outing's worth of it.
    struct Entry
    {
        JournalKind kind = JournalKind::Run;
        std::string subject;       // the dungeon, the zone, the item, where
        std::string detail;        // Run: its bosses; Duel: "bot" when the bot won; a sale: the price;
                                   // Battle: how many of the matches were won
        uint32_t    times   = 0;   // Run: bosses down; Questing: quests; Duel, Raised, Battle: how often
        uint32_t    wipes   = 0;   // Run: wipes; Battle: matches lost
        uint64_t    firstAt = 0;   // unix seconds
        uint64_t    lastAt  = 0;
    };

    struct Pair
    {
        std::vector<Entry> entries;
        bool               dirty = false;
        bool               talked = false;     // the bot has said something to them since startup...
        Clock::time_point  lastTalk{};         // ...and when it last did
    };

    using Key = std::pair<uint64_t, uint64_t>;   // (bot, player), raw guids
    std::map<Key, Pair> g_pairs;                 // the world thread's
    JournalStats        g_stats;
    Clock::time_point   g_nextSave{};

    // What a hook saw, to be written down on the world tick.
    struct Raw
    {
        bool     duel   = false;   // else: a quest `player` finished
        uint64_t bot    = 0;       // duel only
        uint64_t player = 0;
        bool     botWon = false;   // duel only
    };
    std::mutex       g_rawMutex;
    std::vector<Raw> g_raw;   // under g_rawMutex
    constexpr size_t kMaxRaw = 512;

    // Settings: the world thread's (hooks read only g_enable).
    bool        g_enable = true;
    uint32_t    g_keepDays = 180;
    uint32_t    g_keepPerPair = 12;
    uint32_t    g_sessionHours = 6;
    uint32_t    g_recallMax = 3;
    uint32_t    g_reunionHours = 6;
    float       g_nearYards = 100.0f;
    Phrases     g_talkWords;
    std::string g_historyText;
    std::string g_runText, g_runBossesText, g_runWipesText;
    std::string g_questingText, g_duelText, g_duelsText;
    std::string g_soldText, g_boughtText, g_gaveText, g_givenText, g_raisedText;
    std::string g_battleWonText, g_battleLostText, g_battleDrawText, g_battlesText;
    std::vector<std::string> g_agoNames;     // 7 of them
    std::vector<std::string> g_timesNames;   // once | twice | {n} times
    std::vector<std::string> g_questNames;   // 1 quest | {n} quests

    uint64_t NowSeconds()
    {
        return uint64_t(std::time(nullptr));
    }

    std::string Escape(std::string v)
    {
        CharacterDatabase.EscapeString(v);
        return v;
    }

    std::string Filled(const std::string& out)
    {
        return out == "[Format Error]" ? std::string() : out;
    }

    std::string Numbered(const std::string& templ, uint64_t n)
    {
        return Filled(SafeFormat(templ, fmt::arg("n", n)));
    }

    // "6 days ago": how long ago `then` was, in the words people use for it.
    std::string Ago(uint64_t then, uint64_t now)
    {
        if (g_agoNames.size() != 7)
            return {};
        const uint64_t secs  = now > then ? now - then : 0;
        const uint64_t hours = secs / 3600;
        const uint64_t days  = secs / 86400;
        if (secs < 90 * 60)
            return g_agoNames[0];
        if (hours < 20)
            return g_agoNames[1];
        if (hours < 44)
            return g_agoNames[2];
        if (days < 7)
            return Numbered(g_agoNames[3], std::max<uint64_t>(2, (hours + 12) / 24));
        if (days < 14)
            return g_agoNames[4];
        if (days < 60)
            return Numbered(g_agoNames[5], days / 7);
        return Numbered(g_agoNames[6], std::max<uint64_t>(2, days / 30));
    }

    // "once", "twice", "4 times"; "1 quest", "9 quests".
    std::string Counted(const std::vector<std::string>& names, uint32_t n)
    {
        if (names.empty() || !n)
            return {};
        return n <= names.size() - 1 ? names[n - 1] : Numbered(names.back(), n);
    }

    // A count kept as text (a battleground's matches won).
    uint32_t Number(const std::string& text)
    {
        return text.empty() ? 0 : uint32_t(std::strtoul(text.c_str(), nullptr, 10));
    }

    std::vector<std::string> ListOf(const std::string& detail)
    {
        std::vector<std::string> out;
        for (const std::string& name : SplitString(detail, kListSep))
            if (!name.empty())
                out.push_back(name);
        return out;
    }

    // "A, B and C".
    std::string Joined(const std::vector<std::string>& names)
    {
        std::string out;
        for (size_t i = 0; i < names.size(); ++i)
            out += (i ? (i + 1 == names.size() ? " and " : ", ") : "") + names[i];
        return out;
    }

    // One entry in words, as the bot would put it to itself.
    std::string Render(const Entry& e, const std::string& name, uint64_t now)
    {
        const std::string ago = Ago(e.lastAt, now);
        std::string bosses, wipes, winner;
        uint32_t won = 0;
        const std::string* templ = nullptr;
        switch (e.kind)
        {
            case JournalKind::Run:
                templ = &g_runText;
                if (const std::vector<std::string> list = ListOf(e.detail); !list.empty())
                    bosses = Filled(SafeFormat(g_runBossesText, fmt::arg("list", Joined(list))));
                if (e.wipes)
                    wipes = Filled(SafeFormat(g_runWipesText, fmt::arg("times", Counted(g_timesNames, e.wipes))));
                break;
            case JournalKind::Questing: templ = &g_questingText; break;
            case JournalKind::Duel:
                templ  = e.times > 1 ? &g_duelsText : &g_duelText;
                winner = e.detail == "bot" ? std::string("you") : name;
                break;
            case JournalKind::Sold:     templ = &g_soldText;   break;
            case JournalKind::Bought:   templ = &g_boughtText; break;
            case JournalKind::Gave:     templ = &g_gaveText;   break;
            case JournalKind::Given:    templ = &g_givenText;  break;
            case JournalKind::Raised:   templ = &g_raisedText; break;
            case JournalKind::Battle:
                won   = Number(e.detail);
                templ = e.times > 1 ? &g_battlesText
                        : won       ? &g_battleWonText
                        : e.wipes   ? &g_battleLostText
                                    : &g_battleDrawText;
                break;
        }
        if (!templ || templ->empty() || ago.empty())
            return {};
        return Filled(SafeFormat(*templ, fmt::arg("subject", e.subject), fmt::arg("detail", e.detail),
                                 fmt::arg("ago", ago), fmt::arg("bosses", bosses), fmt::arg("wipes", wipes),
                                 fmt::arg("quests", Counted(g_questNames, e.times)), fmt::arg("count", e.times),
                                 fmt::arg("winner", winner), fmt::arg("name", name), fmt::arg("won", won)));
    }

    // The pair's entry for this outing: the same thing with the same subject
    // within Journal.SessionHours, else a new one. The reference is good until
    // the pair's entries next change.
    Entry& Outing(Pair& pair, JournalKind kind, const std::string& subject, uint64_t now)
    {
        const uint64_t span = uint64_t(g_sessionHours) * 3600;
        for (Entry& e : pair.entries)
            if (e.kind == kind && e.subject == subject && now - std::min(now, e.lastAt) <= span)
                return e;
        Entry e;
        e.kind    = kind;
        e.subject = subject;
        e.firstAt = now;
        e.lastAt  = now;
        pair.entries.push_back(std::move(e));
        return pair.entries.back();
    }

    // Written: the pair is to be saved, and keeps no more than its share.
    void Wrote(Pair& pair)
    {
        while (pair.entries.size() > g_keepPerPair)
            pair.entries.erase(std::min_element(pair.entries.begin(), pair.entries.end(),
                                                [](const Entry& a, const Entry& b) { return a.lastAt < b.lastAt; }));
        pair.dirty = true;
        ++g_stats.noted;
    }

    std::string Clipped(const std::string& text, size_t max)
    {
        return text.size() <= max ? text : std::string();   // never cut through a name
    }

    // Each bot of `there` with each of those playing among them.
    template <typename Fn>
    void ForEachPair(const std::vector<Player*>& there, Fn&& fn)
    {
        for (Player* bot : there)
        {
            if (!bot || !OllamaIsBotPlayer(bot))
                continue;
            for (Player* player : there)
                if (player && OllamaIsRealPlayer(player))
                    fn(g_pairs[Key(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue())]);
        }
    }

    // Those of `group` on the map `of` is on, and no further from it than
    // `within` (0: anywhere on it).
    std::vector<Player*> Around(Player* of, float within)
    {
        std::vector<Player*> out;
        Group* group = of ? of->GetGroup() : nullptr;
        if (!group || !of->IsInWorld())
            return out;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->FindMap() == of->FindMap() &&
                (within <= 0.0f || member == of || member->IsWithinDistInMap(of, within)))
                out.push_back(member);
        }
        return out;
    }

    // Where `who` is, as people say it: the dungeon, or else the zone.
    std::string WhereOf(Player* who)
    {
        Map* map = who ? who->FindMap() : nullptr;
        if (!map)
            return {};
        if (map->IsDungeon() && map->GetMapName())
            return map->GetMapName();
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(who->GetZoneId());
        return zone && zone->area_name[0] ? zone->area_name[0] : std::string();
    }

    void QuestDone(uint64_t who, uint64_t now)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(who));
        if (!player || !player->IsInWorld() || !OllamaIsRealPlayer(player))
            return;
        const std::string where = Clipped(WhereOf(player), kSubjectMax);
        if (where.empty())
            return;
        ForEachPair(Around(player, g_nearYards), [&](Pair& pair)
        {
            Entry& e = Outing(pair, JournalKind::Questing, where, now);
            ++e.times;
            e.lastAt = now;
            Wrote(pair);
        });
    }

    void Save(const Key& key, Pair& pair)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(fmt::format("DELETE FROM mod_ollama_chat_journal WHERE bot_guid = {} AND player_guid = {}",
                                  key.first, key.second));
        std::string values;
        for (const Entry& e : pair.entries)
            values += fmt::format("{}({}, {}, {}, '{}', '{}', {}, {}, FROM_UNIXTIME({}), FROM_UNIXTIME({}))",
                                  values.empty() ? "" : ",", key.first, key.second, uint32_t(e.kind),
                                  Escape(e.subject), Escape(e.detail), e.times, e.wipes, e.firstAt, e.lastAt);
        if (!values.empty())
            trans->Append("INSERT INTO mod_ollama_chat_journal (bot_guid, player_guid, kind, subject, detail, times, "
                          "wipes, first_at, last_at) VALUES " + values);
        CharacterDatabase.CommitTransaction(trans);
        pair.dirty = false;
        ++g_stats.saved;
    }

    bool TalksOf(const std::string& text)
    {
        return !text.empty() && !TextRules::FindPhraseIndexes(
                                    TextRules::WithoutBracketNames(TextRules::StripChatMarkup(text)), g_talkWords)
                                    .empty();
    }

    // What `pair` has, newest first, for `name`'s ears.
    std::string History(const Pair& pair, const std::string& name)
    {
        if (pair.entries.empty() || g_historyText.empty())
            return {};
        std::vector<const Entry*> newest;
        for (const Entry& e : pair.entries)
            newest.push_back(&e);
        std::sort(newest.begin(), newest.end(), [](const Entry* a, const Entry* b) { return a->lastAt > b->lastAt; });
        const uint64_t now = NowSeconds();
        std::string history;
        uint32_t told = 0;
        for (const Entry* e : newest)
        {
            if (told >= g_recallMax)
                break;
            const std::string line = Render(*e, name, now);
            if (line.empty())
                continue;
            history += (history.empty() ? "" : "; ") + line;
            ++told;
        }
        if (history.empty())
            return {};
        return Filled(SafeFormat(g_historyText, fmt::arg("name", name), fmt::arg("history", history)));
    }

    Pair* Find(Player* bot, Player* player)
    {
        if (!g_enable || !bot || !player || !OllamaIsBotPlayer(bot) || !OllamaIsRealPlayer(player))
            return nullptr;
        const auto it = g_pairs.find(Key(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue()));
        return it == g_pairs.end() ? nullptr : &it->second;
    }
}

bool Journal_Wants()
{
    return g_enable;
}

void Journal_LoadConfig()
{
    auto text = [](const char* key, const char* def)
    {
        return sConfigMgr->GetOption<std::string>(key, def);
    };
    auto number = [](const char* key, uint32_t def, uint32_t lo, uint32_t hi)
    {
        return std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>(key, def), lo, hi);
    };
    auto names = [&text](const char* key, const char* def, size_t want)
    {
        std::vector<std::string> out = SplitString(text(key, def), '|');
        if (out.size() != want)
        {
            LOG_WARN("module.ollamachat", "[Ollama Chat] {} has {} names, not {}: the default is used.", key,
                     out.size(), want);
            out = SplitString(def, '|');
        }
        return out;
    };

    g_enable       = sConfigMgr->GetOption<bool>("OllamaChat.Journal.Enable", true);
    g_keepDays     = number("OllamaChat.Journal.KeepDays", 180, 1, 3650);
    g_keepPerPair  = number("OllamaChat.Journal.KeepPerPair", 12, 1, 100);
    g_sessionHours = number("OllamaChat.Journal.SessionHours", 6, 1, 72);
    g_recallMax    = number("OllamaChat.Journal.RecallMax", 3, 1, 10);
    g_reunionHours = number("OllamaChat.Journal.ReunionHours", 6, 1, 720);
    g_nearYards    = float(number("OllamaChat.Journal.NearYards", 100, 10, 500));
    g_talkWords = CrowdText::ParsePhrases(text("OllamaChat.Journal.TalkWords",
        "remember|last time|the other day|been a while|long time no see|a while back|we did|we ran|we killed|"
        "we wiped|know me|recognise me|recognize me|miss me|have we met|met before|do i know you"));
    g_historyText = text("OllamaChat.Journal.HistoryText",
        "What you and {name} have done together before: {history}. That is how you know them, and all you have "
        "done together: do not add to it.");
    g_runText = text("OllamaChat.Journal.RunText", "you were in {subject} together {ago}{bosses}{wipes}");
    g_runBossesText = text("OllamaChat.Journal.RunBossesText", ", and killed {list} there");
    g_runWipesText = text("OllamaChat.Journal.RunWipesText", ", wiping {times}");
    g_questingText = text("OllamaChat.Journal.QuestingText",
        "you quested together in {subject} {ago} ({quests} done)");
    g_duelText = text("OllamaChat.Journal.DuelText", "the two of you dueled {ago} and {winner} won");
    g_duelsText = text("OllamaChat.Journal.DuelsText",
        "the two of you have dueled {count} times, the last {ago}, when {winner} won");
    g_soldText = text("OllamaChat.Journal.SoldText", "you sold them {subject} for {detail} {ago}");
    g_boughtText = text("OllamaChat.Journal.BoughtText", "you bought {subject} from them for {detail} {ago}");
    g_gaveText = text("OllamaChat.Journal.GaveText", "you gave them {subject} {ago}");
    g_givenText = text("OllamaChat.Journal.GivenText", "they gave you {subject} {ago}");
    g_raisedText = text("OllamaChat.Journal.RaisedText",
        "they brought you back from the dead in {subject} {ago}");
    g_battleWonText = text("OllamaChat.Journal.BattleWonText",
        "you fought in {subject} on the same team {ago}, and your team won");
    g_battleLostText = text("OllamaChat.Journal.BattleLostText",
        "you fought in {subject} on the same team {ago}, and your team lost");
    g_battleDrawText = text("OllamaChat.Journal.BattleDrawText",
        "you fought in {subject} on the same team {ago}, and neither team won");
    g_battlesText = text("OllamaChat.Journal.BattlesText",
        "you fought {count} matches in {subject} on the same team, the last {ago}, and your team won {won} of them");
    g_agoNames = names("OllamaChat.Journal.AgoNames",
        "a little while ago|a few hours ago|yesterday|{n} days ago|last week|{n} weeks ago|{n} months ago", 7);
    g_timesNames = names("OllamaChat.Journal.TimesNames", "once|twice|{n} times", 3);
    g_questNames = names("OllamaChat.Journal.QuestNames", "1 quest|{n} quests", 2);
}

void Journal_Load()
{
    g_pairs.clear();
    // The file in data/sql makes the table too; this is for a server whose
    // updater does not run for modules.
    CharacterDatabase.DirectExecute(kCreateTable);
    CharacterDatabase.DirectExecute(fmt::format("DELETE FROM mod_ollama_chat_journal WHERE last_at < FROM_UNIXTIME({})",
                                                NowSeconds() - uint64_t(g_keepDays) * 86400));
    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, player_guid, kind, subject, detail, times, wipes, UNIX_TIMESTAMP(first_at), "
        "UNIX_TIMESTAMP(last_at) FROM mod_ollama_chat_journal");
    uint32_t loaded = 0;
    if (result)
        do
        {
            Field* f = result->Fetch();
            const uint32_t kind = f[2].Get<uint8>();
            if (kind < uint32_t(JournalKind::Run) || kind > uint32_t(JournalKind::Battle))
                continue;
            Entry e;
            e.kind    = JournalKind(kind);
            e.subject = f[3].Get<std::string>();
            e.detail  = f[4].Get<std::string>();
            e.times   = f[5].Get<uint32>();
            e.wipes   = f[6].Get<uint32>();
            e.firstAt = f[7].Get<uint64>();
            e.lastAt  = f[8].Get<uint64>();
            g_pairs[Key(f[0].Get<uint64>(), f[1].Get<uint64>())].entries.push_back(std::move(e));
            ++loaded;
        } while (result->NextRow());
    LOG_INFO("module.ollamachat", "[Ollama Chat] Loaded {} journal entries for {} pairs.", loaded, g_pairs.size());
}

void Journal_SaveAll()
{
    for (auto& [key, pair] : g_pairs)
        if (pair.dirty)
            Save(key, pair);
}

void Journal_Update()
{
    std::vector<Raw> raw;
    {
        std::lock_guard<std::mutex> lock(g_rawMutex);
        raw.swap(g_raw);
    }
    const uint64_t now = NowSeconds();
    if (g_enable)
        for (const Raw& r : raw)
        {
            if (!r.duel)
            {
                QuestDone(r.player, now);
                continue;
            }
            Pair& pair = g_pairs[Key(r.bot, r.player)];
            Entry& e = Outing(pair, JournalKind::Duel, std::string(), now);
            ++e.times;
            e.detail = r.botWon ? "bot" : "player";
            e.lastAt = now;
            Wrote(pair);
        }

    const auto tick = Clock::now();
    if (tick >= g_nextSave)
    {
        g_nextSave = tick + std::chrono::seconds(60);
        Journal_SaveAll();
    }
}

void Journal_Note(JournalKind kind, Player* bot, Player* player, const std::string& subject,
                  const std::string& detail)
{
    if (!g_enable || !bot || !player || !OllamaIsBotPlayer(bot) || !OllamaIsRealPlayer(player))
        return;
    // A resurrection is of where it happened. Anything else is of the thing
    // named, and one the game has no name for is not kept: "you gave them
    // Elwynn Forest".
    const std::string what =
        Clipped(kind == JournalKind::Raised && subject.empty() ? WhereOf(bot) : subject, kSubjectMax);
    if (what.empty())
        return;
    const uint64_t now = NowSeconds();
    Pair& pair = g_pairs[Key(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue())];
    if (kind == JournalKind::Raised)
    {
        Entry& e = Outing(pair, kind, what, now);   // raised twice in one run is one line of it
        ++e.times;
        e.lastAt = now;
    }
    else
    {
        Entry e;
        e.kind    = kind;
        e.subject = what;
        e.detail  = Clipped(detail, kDetailMax);
        e.times   = 1;
        e.firstAt = now;
        e.lastAt  = now;
        pair.entries.push_back(std::move(e));
    }
    Wrote(pair);
}

std::string Journal_ItemWords(uint32_t entry, uint32_t count)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto || proto->Name1.empty())
        return {};
    return "[" + proto->Name1 + "]" + (count > 1 ? " x" + std::to_string(count) : std::string());
}

void Journal_BossDown(Player* killer, const std::string& boss)
{
    Map* map = killer ? killer->FindMap() : nullptr;
    if (!g_enable || !map || !map->IsDungeon() || !map->GetMapName() || boss.empty() ||
        boss.find(kListSep) != std::string::npos)
        return;
    const std::string where = Clipped(map->GetMapName(), kSubjectMax);
    if (where.empty())
        return;
    const uint64_t now = NowSeconds();
    ForEachPair(Around(killer, 0.0f), [&](Pair& pair)
    {
        Entry& e = Outing(pair, JournalKind::Run, where, now);
        std::vector<std::string> list = ListOf(e.detail);
        if (std::find(list.begin(), list.end(), boss) == list.end() &&
            e.detail.size() + boss.size() + 1 <= kDetailMax)
        {
            e.detail += (e.detail.empty() ? "" : std::string(1, kListSep)) + boss;
            list.push_back(boss);
        }
        e.times  = uint32_t(list.size());
        e.lastAt = now;
        Wrote(pair);
    });
}

void Journal_Wiped(const std::vector<Player*>& here)
{
    Map* map = !here.empty() && here.front() ? here.front()->FindMap() : nullptr;
    if (!g_enable || !map || !map->IsDungeon() || !map->GetMapName())
        return;
    const std::string where = Clipped(map->GetMapName(), kSubjectMax);
    if (where.empty())
        return;
    const uint64_t now = NowSeconds();
    ForEachPair(here, [&](Pair& pair)
    {
        Entry& e = Outing(pair, JournalKind::Run, where, now);
        ++e.wipes;
        e.lastAt = now;
        Wrote(pair);
    });
}

void Journal_Battle(const std::vector<Player*>& team, const std::string& battleground, int outcome)
{
    const std::string where = Clipped(battleground, kSubjectMax);
    if (!g_enable || where.empty())
        return;
    const uint64_t now = NowSeconds();
    ForEachPair(team, [&](Pair& pair)
    {
        Entry& e = Outing(pair, JournalKind::Battle, where, now);
        ++e.times;
        if (outcome > 0)
            e.detail = std::to_string(Number(e.detail) + 1);
        else if (outcome < 0)
            ++e.wipes;
        e.lastAt = now;
        Wrote(pair);
    });
}

void Journal_NoteDuel(Player* winner, Player* loser)
{
    // A map thread: who they are and which of them is a bot, nothing more.
    if (!g_enable || !winner || !loser)
        return;
    const bool winnerBot = OllamaIsBotPlayer(winner);
    if (winnerBot == OllamaIsBotPlayer(loser))
        return;   // two bots, or two people
    Raw r;
    r.duel   = true;
    r.bot    = (winnerBot ? winner : loser)->GetGUID().GetRawValue();
    r.player = (winnerBot ? loser : winner)->GetGUID().GetRawValue();
    r.botWon = winnerBot;
    std::lock_guard<std::mutex> lock(g_rawMutex);
    if (g_raw.size() < kMaxRaw)
        g_raw.push_back(r);
}

void Journal_NoteQuestDone(Player* player)
{
    // A map thread: who, nothing more.
    if (!g_enable || !player || OllamaIsBotPlayer(player))
        return;
    Raw r;
    r.player = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_rawMutex);
    if (g_raw.size() < kMaxRaw)
        g_raw.push_back(r);
}

std::string Journal_PromptFor(Player* bot, Player* player)
{
    Pair* pair = Find(bot, player);
    if (!pair)
        return {};
    const std::string out = History(*pair, player->GetName());
    if (out.empty())
        return {};
    pair->talked   = true;
    pair->lastTalk = Clock::now();
    ++g_stats.told;
    return out;
}

std::string Journal_ReplyPromptFor(Player* bot, Player* player, const std::vector<std::string>& heard)
{
    Pair* pair = Find(bot, player);
    if (!pair)
        return {};
    const auto now = Clock::now();
    // The first thing it has said to them in a while: it knows who they are.
    const bool reunion = !pair->talked || now - pair->lastTalk > std::chrono::hours(g_reunionHours);
    pair->talked   = true;
    pair->lastTalk = now;
    if (!reunion && !std::any_of(heard.begin(), heard.end(), TalksOf))
        return {};
    const std::string out = History(*pair, player->GetName());
    if (!out.empty())
        ++g_stats.told;
    return out;
}

JournalStats Journal_GetStats()
{
    JournalStats s = g_stats;
    for (const auto& [key, pair] : g_pairs)
    {
        if (pair.entries.empty())
            continue;
        ++s.pairs;
        s.entries += uint32_t(pair.entries.size());
    }
    return s;
}
