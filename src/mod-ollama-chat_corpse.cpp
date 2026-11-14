#include "mod-ollama-chat_corpse.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Creature.h"
#include "Formulas.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    enum class Worth : uint8_t
    {
        None,
        Elite,   // an elite out in the world
        Named,   // a rare, or one of a kind
        Boss,
    };

    // A kill as the hook saw it, on the map thread.
    struct Noted
    {
        ObjectGuid        killer;
        ObjectGuid        victim;
        uint32_t          entry      = 0;
        Worth             worth      = Worth::None;
        bool              oneOfAKind = false;   // named if it is the only spawn of its kind: the tick counts
        Clock::time_point when{};
    };
    std::mutex         g_noteMutex;
    std::vector<Noted> g_notes;
    constexpr size_t   kMaxNotes  = 512;
    // Older than this when the tick comes (switched off and on again in
    // between): the moment has passed.
    constexpr auto     kNoteStale = std::chrono::seconds(10);

    // An emote decided on, waiting for the bot to be out of the fight and near
    // the body.
    struct Due
    {
        ObjectGuid        bot;
        ObjectGuid        body;
        uint32_t          emote = 0;
        Worth             worth = Worth::None;
        Clock::time_point at{};
        Clock::time_point until{};   // still fighting, or too far, after this: let go
    };
    std::vector<Due> g_due;
    constexpr auto   kWait  = std::chrono::seconds(10);
    constexpr auto   kRetry = std::chrono::milliseconds(500);
    // A real player this near the bot when it kills something: someone who
    // could be there to see it (the emote itself only reaches those in the
    // text emote range). Nobody near, no roll at all.
    constexpr float  kNear  = 60.0f;

    // The online players, looked at once a tick and only when needed.
    struct Lookers
    {
        OllamaWorldSnapshot world;
        bool                built = false;

        bool Near(Player* bot, float distance)
        {
            if (!built)
            {
                world.Build();
                built = true;
            }
            return world.RealPlayerWithin(bot, distance);
        }
    };

    std::unordered_map<uint64_t, Clock::time_point> g_lastBy;   // bot -> when it last emoted at a body

    CorpseStats g_stats;

    const char* WorthName(Worth worth)
    {
        switch (worth)
        {
            case Worth::Boss:  return "a boss";
            case Worth::Named: return "a named one";
            case Worth::Elite: return "an elite";
            default:           return "nothing";
        }
    }

    // How many places in the world each kind of creature is spawned at: a
    // named quest target ("Gath'Ilzogg") has one, a camp's bandits dozens.
    // Counted once, on first use, from the spawns the server loaded (a spawn
    // that may be any of up to three kinds counts for each).
    uint32_t SpawnsOf(uint32_t entry)
    {
        static std::unordered_map<uint32_t, uint32_t> counts;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                ++counts[data.id];
                if (data.id2)
                    ++counts[data.id2];
                if (data.id3)
                    ++counts[data.id3];
            }
        }
        auto it = counts.find(entry);
        return it == counts.end() ? 0 : it->second;
    }

    // This bot's chat style at a body (OllamaChat.Corpse.Style.<KEY>), or the
    // default style.
    CorpseStyle const& StyleOf(Player* bot)
    {
        if (!g_CorpseStyles.empty())
        {
            std::string key = GetBotPersonality(bot);
            for (char& c : key)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            auto it = g_CorpseStyles.find(key);
            if (it != g_CorpseStyles.end())
                return it->second;
        }
        return g_CorpseStyleDefault;
    }

    uint32_t ChanceFor(Worth worth)
    {
        switch (worth)
        {
            case Worth::Boss:  return g_CorpseBossChance;
            case Worth::Named: return g_CorpseNamedChance;
            case Worth::Elite: return g_CorpseEliteChance;
            default:           return 0;
        }
    }

    // Who does it: the one that killed it, if that is a bot; when a real
    // player landed the blow, one of their group's bots near the body (on its
    // way back from the fight, it may yet come over).
    Player* PickBot(Player* killer, Creature* body)
    {
        if (OllamaIsBotPlayer(killer))
            return killer;
        Group* group = killer->GetGroup();
        if (!group)
            return nullptr;
        std::vector<Player*> near;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == killer || !member->IsInWorld() || !member->IsAlive() ||
                member->GetMap() != body->GetMap() || !OllamaIsBotPlayer(member) ||
                member->GetDistance(body) > g_CorpseRange + 20.0f)
                continue;
            near.push_back(member);
        }
        if (near.empty())
            return nullptr;
        return near[urand(0, static_cast<uint32>(near.size() - 1))];
    }

    // Decide, for a kill the hook noted: is it worth it, who, and whether
    // their style does anything this time.
    void Consider(Noted const& n, const Clock::time_point now, Lookers& lookers)
    {
        Worth worth = n.worth;
        if (n.oneOfAKind && SpawnsOf(n.entry) == 1)
            worth = Worth::Named;   // an elite with one spawn too: a named one
        if (worth == Worth::None)
            return;

        Player* killer = ObjectAccessor::FindConnectedPlayer(n.killer);
        if (!killer || !killer->IsInWorld())
            return;
        Creature* body = ObjectAccessor::GetCreature(*killer, n.victim);
        if (!body || body->IsAlive())
            return;
        Player* bot = PickBot(killer, body);
        if (!bot || !lookers.Near(bot, kNear))
            return;
        ++g_stats.kills;
        ++(worth == Worth::Boss ? g_stats.bosses : worth == Worth::Named ? g_stats.named : g_stats.elites);

        const uint64_t key = bot->GetGUID().GetRawValue();
        auto last = g_lastBy.find(key);
        if (last != g_lastBy.end() && now - last->second < std::chrono::seconds(g_CorpseCooldownSeconds))
            return;

        CorpseStyle const& style = StyleOf(bot);
        if (style.emotes.empty())
            return;
        const uint32_t chance = std::min<uint32_t>(100, ChanceFor(worth) * style.percent / 100);
        ++g_stats.rolled;
        // urand is inclusive at both ends: 1..100 against the chance, and a
        // chance of 0 is never.
        if (chance == 0 || urand(1, 100) > chance)
            return;

        Due d;
        d.bot   = bot->GetGUID();
        d.body  = body->GetGUID();
        d.emote = style.emotes[urand(0, static_cast<uint32>(style.emotes.size() - 1))];
        d.worth = worth;
        d.at    = now + std::chrono::milliseconds(urand(g_CorpseDelayMinMs, std::max(g_CorpseDelayMinMs, g_CorpseDelayMaxMs)));
        d.until = d.at + kWait;
        g_lastBy[key] = now;   // its turn is taken, whether it gets to do it or not
        g_due.push_back(d);
    }

    // Play it, if the moment is still there: true when it is done with
    // (played, or let go), false to look again in a moment.
    bool Play(Due const& d, const Clock::time_point now, Lookers& lookers)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(d.bot);
        Creature* body = bot && bot->IsInWorld() && bot->IsAlive() ? ObjectAccessor::GetCreature(*bot, d.body)
                                                                     : nullptr;
        if (!body || body->IsAlive())
        {
            ++g_stats.lapsed;   // gone (despawned, skinned away), up again, or the bot is
            return true;
        }

        // Out of the next fight and near the body, and stood still so it can
        // turn to it. Following someone, it may not stop in time: near the
        // body at the end of the wait, it does it anyway.
        const bool ready = !bot->IsInCombat() && bot->GetDistance(body) <= g_CorpseRange;
        if (!ready || !IsSafeForFacing(bot))
        {
            if (now < d.until)
                return false;
            if (!ready)
            {
                ++g_stats.lapsed;
                return true;
            }
        }

        if (!lookers.Near(bot, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE)))
        {
            ++g_stats.unseen;
            return true;
        }

        if (g_EnableBotFacing && IsSafeForFacing(bot))
            bot->SetFacingToObject(body);
        BotPlayTextEmote(bot, d.emote, body->GetGUID());
        ++g_stats.emoted;

        // What those about saw, so a bot asked "why'd you spit on him" knows.
        const std::string emote = LookupTextEmoteName(d.emote);
        if (!emote.empty() && !g_EmoteEventText.empty())
        {
            const std::string note = SafeFormat(g_EmoteEventText, fmt::arg("actor", bot->GetName()),
                                                fmt::arg("target", body->GetName() + "'s body"),
                                                fmt::arg("emote", emote));
            if (!note.empty() && note != "[Format Error]")
                Context_RecordEvent(bot, note, false);
        }
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} gives the body of {} ({}) a /{}.", bot->GetName(),
                     body->GetName(), WorthName(d.worth), emote);
        return true;
    }
}

void Corpse_NoteKill(Player* killer, Creature* victim)
{
    if (!g_Enable || !g_CorpseEnable || !g_EnableBotEmotes || !killer || !victim)
        return;
    CreatureTemplate const* t = victim->GetCreatureTemplate();
    if (!t || victim->IsPet() || victim->IsTotem() || victim->GetOwnerGUID().IsPlayer())
        return;
    if (t->type == CREATURE_TYPE_CRITTER || t->type == CREATURE_TYPE_TOTEM || t->type == CREATURE_TYPE_NON_COMBAT_PET ||
        t->type == CREATURE_TYPE_GAS_CLOUD || t->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER))
        return;
    Map* map = victim->GetMap();
    if (!map || map->IsBattlegroundOrArena())
        return;
    // A fight worth it: nothing grey to whoever killed it.
    if (victim->GetLevel() <= Acore::XP::GetGrayLevel(killer->GetLevel()))
        return;

    Noted n;
    if (victim->IsDungeonBoss() || victim->isWorldBoss() || t->rank == CREATURE_ELITE_WORLDBOSS)
        n.worth = Worth::Boss;
    else if (t->rank == CREATURE_ELITE_RARE || t->rank == CREATURE_ELITE_RAREELITE)
        n.worth = Worth::Named;
    else if (map->IsDungeon())
        return;   // in a dungeon or raid, elites are every pull, and one-of-a-kind trash is everywhere
    else
    {
        if (t->rank == CREATURE_ELITE_ELITE)
            n.worth = Worth::Elite;
        n.oneOfAKind = victim->GetSpawnId() != 0;   // a spawn of its own: the tick counts how many there are
        if (n.worth == Worth::None && !n.oneOfAKind)
            return;
    }
    n.killer = killer->GetGUID();
    n.victim = victim->GetGUID();
    n.entry  = victim->GetEntry();
    n.when   = Clock::now();

    std::lock_guard<std::mutex> lock(g_noteMutex);
    if (g_notes.size() < kMaxNotes)
        g_notes.push_back(n);
}

void Corpse_Update()
{
    std::vector<Noted> notes;
    {
        std::lock_guard<std::mutex> lock(g_noteMutex);
        notes.swap(g_notes);
    }
    if (notes.empty() && g_due.empty())
        return;

    const auto now = Clock::now();
    const bool on  = g_Enable && g_CorpseEnable && g_EnableBotEmotes;
    if (!on)
    {
        g_due.clear();
        return;
    }

    Lookers lookers;
    for (Noted const& n : notes)
        if (now - n.when <= kNoteStale)
            Consider(n, now, lookers);

    if (!g_due.empty())
    {
        std::vector<Due> later;
        for (Due& d : g_due)
        {
            if (now < d.at)
            {
                later.push_back(d);
                continue;
            }
            if (!Play(d, now, lookers))
            {
                d.at = now + kRetry;
                later.push_back(d);
            }
        }
        g_due.swap(later);
    }

    // Who did one when does not need to outlive the cooldown.
    if (g_lastBy.size() > 256)
        for (auto it = g_lastBy.begin(); it != g_lastBy.end();)
            it = now - it->second >= std::chrono::seconds(g_CorpseCooldownSeconds) ? g_lastBy.erase(it)
                                                                                  : std::next(it);
}

CorpseStats Corpse_GetStats()
{
    return g_stats;
}
