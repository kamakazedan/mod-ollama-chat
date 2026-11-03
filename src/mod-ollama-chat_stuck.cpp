#include "mod-ollama-chat_stuck.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"

#include "AiObjectContext.h"
#include "GatherNodeMgr.h"
#include "LootObjectStack.h"
#include "NewRpgInfo.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <list>
#include <mutex>
#include <unordered_map>
#include <variant>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Mounted
    {
        uint64_t          guid = 0;
        Clock::time_point when{};
        uint32_t          map = 0;
        float             x = 0.0f, y = 0.0f, z = 0.0f;
        bool              combat = false;
    };

    // Written by the hook on map threads, taken on the world tick.
    std::mutex           g_noteMutex;
    std::vector<Mounted> g_noted;
    constexpr size_t     kMaxNoted = 4096;

    // World thread from here on.
    struct Track
    {
        std::deque<Mounted> recent;
        Clock::time_point   lastLook{};   // stuck already dealt with (or looked into) at this time
    };
    std::unordered_map<uint64_t, Track> g_tracks;

    // Something a bot is kept off.
    struct Hold
    {
        ObjectGuid        object;
        uint32_t          spawnId = 0;   // a gathering node's spawn, for the gathering it may be doing
        uint32_t          map = 0;
        float             x = 0.0f, y = 0.0f, z = 0.0f;
        Clock::time_point until{};
    };
    std::unordered_map<uint64_t, std::vector<Hold>> g_holds;

    // What it was stuck on, for its replies a while after.
    struct Memory
    {
        std::string       what;    // "a Copper Vein"
        std::string       verb;    // "mine"
        std::string       zone;
        Clock::time_point when{};
    };
    std::unordered_map<uint64_t, Memory> g_memory;

    // Bots kept on foot: playerbots' "mount" strategy taken off them, given
    // back at this time (or as soon as they join a group or get a master).
    std::unordered_map<uint64_t, Clock::time_point> g_walkers;

    StuckStats g_stats;

    PlayerbotAI* AiOf(Player* p)
    {
        return p ? PlayerbotsMgr::instance().GetPlayerbotAI(p) : nullptr;
    }

    float Distance(const Mounted& a, const Mounted& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    std::string ZoneName(Player* bot)
    {
        PlayerbotAI* ai = AiOf(bot);
        if (AreaTableEntry const* zone = ai ? ai->GetCurrentZone() : nullptr)
            return PlayerbotAI::GetLocalizedAreaName(zone);
        return {};
    }

    // "a Copper Vein", "an Iron Deposit".
    std::string WithArticle(const std::string& name)
    {
        if (name.empty())
            return name;
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
        const bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
        return (vowel ? "an " : "a ") + name;
    }

    // "mine", "pick" or "open": what the lock on it asks for. (A LootObject
    // only works its skill out when asked whether this bot can open it, so
    // it cannot say.)
    std::string VerbFor(GameObject* go)
    {
        GameObjectTemplate const* info = go ? go->GetGOInfo() : nullptr;
        if (LockEntry const* lock = info ? sLockStore.LookupEntry(info->GetLockId()) : nullptr)
            for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
                if (lock->Type[i] == LOCK_KEY_SKILL)
                    switch (SkillByLockType(LockType(lock->Index[i])))
                    {
                        case SKILL_MINING:    return "mine";
                        case SKILL_HERBALISM: return "pick";
                        default:              break;
                    }
        return "open";
    }

    // The thing a bot getting on and off on the spot is after: its loot
    // target, else the gathering node it is out for, else a chest-like
    // object right where it stands.
    struct Target
    {
        ObjectGuid  guid;
        uint32_t    spawnId = 0;
        std::string what;
        std::string verb;
        Position    pos;
    };

    bool FindTarget(Player* bot, PlayerbotAI* ai, Target& out)
    {
        AiObjectContext* ctx = ai->GetAiObjectContext();
        if (!ctx)
            return false;

        ObjectGuid guid;
        if (Value<LootObject>* target = ctx->GetValue<LootObject>("loot target"))
            guid = target->Get().guid;

        GameObject* go = nullptr;
        if (guid.IsGameObject())
            go = ai->GetGameObject(guid);
        if (!guid && bot->GetMap())
            if (auto* gather = std::get_if<NewRpgInfo::DoGather>(&ai->rpgInfo.data))
                if (gather->nodeSpawnId)
                    if ((go = GatherNodeMgr::FindLiveNode(bot->GetMap(), gather->nodeSpawnId)))
                        guid = go->GetGUID();
        if (!guid)
        {
            // Right under its feet: a chest or a node, the kind of thing it
            // would be opening.
            std::list<GameObject*> objects;
            Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 6.0f);
            Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
            Cell::VisitObjects(bot, searcher, 6.0f);
            float best = 7.0f;
            for (GameObject* o : objects)
            {
                if (!o || !o->isSpawned() || o->GetGoType() != GAMEOBJECT_TYPE_CHEST)
                    continue;
                const float d = bot->GetDistance(o);
                if (d < best)
                {
                    best = d;
                    go = o;
                }
            }
            if (go)
                guid = go->GetGUID();
        }
        if (!guid)
            return false;

        out.guid = guid;
        if (go)
        {
            out.spawnId = go->GetSpawnId();
            out.what    = WithArticle(go->GetName());
            out.pos     = go->GetPosition();
        }
        else if (Creature* corpse = ai->GetCreature(guid))
        {
            out.what = "the body of " + WithArticle(corpse->GetName());
            out.pos  = corpse->GetPosition();
        }
        else
        {
            out.what = "something";
            out.pos  = bot->GetPosition();
        }

        if (go)
            out.verb = VerbFor(go);
        else
        {
            // A body: a LootObject does work out the skill for one.
            LootObject loot(bot, guid);
            switch (loot.skillId)
            {
                case SKILL_MINING:    out.verb = "mine"; break;
                case SKILL_HERBALISM: out.verb = "pick"; break;
                case SKILL_SKINNING:  out.verb = "skin"; break;
                default:              out.verb = "loot"; break;
            }
        }
        return true;
    }

    // Who it is with, for the log.
    const char* CompanyOf(Player* bot, PlayerbotAI* ai)
    {
        if (bot->GetGroup())
            return OllamaGroupHasRealPlayer(bot) ? "grouped with a player" : "in a group of bots";
        if (ai->GetMaster() && ai->GetMaster() != bot)
            return "with a master";
        return "on its own";
    }

    // Someone else right at the thing it was after -- two bots that went for
    // one herb at once -- for the log. Empty when nobody is.
    std::string OtherAt(Player* bot, const Position& pos)
    {
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, 15.0f, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, 15.0f);
        Player* nearest = nullptr;
        float best = 6.0f;
        for (Player* p : found)
        {
            if (!p || p == bot || !p->IsInWorld())
                continue;
            const float d = p->GetExactDist(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
            if (d < best)
            {
                best = d;
                nearest = p;
            }
        }
        return nearest ? fmt::format(", {} at it too", nearest->GetName()) : std::string();
    }

    // Playerbots puts a bot on its mount only while it has the "mount"
    // strategy (the mount check is not even tried without it, unless the bot
    // is mounted already -- then it can still get down). Taking it off keeps
    // a bot on foot, and with it the loot code's dismount-and-wait loop never
    // gets going: it opens the next thing on foot, the way a player would.
    //
    // Only a random bot on its own: one in a group, or with a master, has to
    // keep up with them, so it keeps its mount.
    bool CanGoOnFoot(Player* bot, PlayerbotAI* ai)
    {
        return g_StuckWalkMinutes > 0 && ai->IsBotAI() && !ai->GetMaster() && !bot->GetGroup() &&
               !bot->InBattleground() && !bot->InArena() && sRandomPlayerbotMgr.IsRandomBot(bot) &&
               ai->HasStrategy("mount", BOT_STATE_NON_COMBAT);
    }

    bool GoOnFoot(Player* bot, PlayerbotAI* ai, Clock::time_point now)
    {
        if (!CanGoOnFoot(bot, ai))
            return false;
        ai->ChangeStrategy("-mount", BOT_STATE_NON_COMBAT);
        g_walkers[bot->GetGUID().GetRawValue()] = now + std::chrono::minutes(g_StuckWalkMinutes);
        ++g_stats.walked;
        return true;
    }

    // Its mount back -- unless something has already sorted its strategies
    // out afresh (accepting an invite, joining a battleground, logging in:
    // they all reset them), in which case there is nothing to give back.
    void GiveMountBack(Player* bot, PlayerbotAI* ai)
    {
        if (!bot->InBattleground() && !bot->InArena() && !ai->HasStrategy("mount", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+mount", BOT_STATE_NON_COMBAT);
    }

    // The bots on foot whose time is up, or who have joined a group or got a
    // master since, get their mount back; with letAllGo, all of them.
    void UpdateWalkers(Clock::time_point now, bool letAllGo)
    {
        for (auto it = g_walkers.begin(); it != g_walkers.end();)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->first));
            PlayerbotAI* ai = AiOf(bot);
            if (!ai)
            {
                // Logged out: it gets its strategies afresh when it logs in.
                it = g_walkers.erase(it);
                continue;
            }
            if (letAllGo || now >= it->second || bot->GetGroup() || ai->GetMaster())
            {
                GiveMountBack(bot, ai);
                it = g_walkers.erase(it);
                continue;
            }
            ++it;
        }
    }

    // Keeps `bot` off `hold` for now: not its loot target, not to be tried
    // again, not the node it gathers.
    void KeepOff(PlayerbotAI* ai, const Hold& hold)
    {
        AiObjectContext* ctx = ai->GetAiObjectContext();
        if (!ctx)
            return;
        if (Value<LootObject>* target = ctx->GetValue<LootObject>("loot target"))
            if (target->Get().guid == hold.object)
                target->Set(LootObject());
        if (Value<LootObjectStack*>* stack = ctx->GetValue<LootObjectStack*>("available loot"))
            if (LootObjectStack* loot = stack->Get())
                if (loot->CanAttemptLoot(hold.object))
                    loot->DeferLoot(hold.object);   // held off: it never comes up as the nearest loot
        if (hold.spawnId)
            if (auto* gather = std::get_if<NewRpgInfo::DoGather>(&ai->rpgInfo.data))
            {
                gather->visited.insert(hold.spawnId);
                if (gather->nodeSpawnId == hold.spawnId)
                {
                    gather->nodeSpawnId = 0;
                    gather->lastReach   = 0;
                }
            }
    }

    Player* RealPlayerNear(Player* bot, float range)
    {
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, range, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, range);
        for (Player* p : found)
            if (p && p != bot && p->IsInWorld() && OllamaIsRealPlayer(p))
                return p;
        return nullptr;
    }

    void Remark(Player* bot, PlayerbotAI* ai, const Target& t)
    {
        if (g_StuckRemarkChance == 0 || g_StuckRemarkPrompt.empty() || !ai->GetChatHelper())
            return;

        ChatChannelSourceLocal source;
        uint32_t chance = g_StuckRemarkChance;
        Group* group = bot->GetGroup();
        if (group && OllamaGroupHasRealPlayer(bot) && !g_DisableForParty)
            source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        else if (!g_DisableForSayYell && g_SayDistance > 0.0f && RealPlayerNear(bot, g_SayDistance))
        {
            source = SRC_SAY_LOCAL;
            chance /= 2;   // talking to nobody in particular
        }
        else
            return;
        if (urand(1, 100) > chance)
            return;

        const std::string scopeKey =
            (source == SRC_SAY_LOCAL)
                ? Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, "", 0, bot->GetZoneId())
                : Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                        group->GetGUID().GetCounter());
        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
            return;

        const std::string personality = GetBotPersonality(bot);
        std::string prompt = SafeFormat(g_StuckRemarkPrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(personality)),
                                        fmt::arg("what", t.what),
                                        fmt::arg("verb", t.verb),
                                        fmt::arg("zone", ZoneName(bot)),
                                        fmt::arg("channel", source == SRC_SAY_LOCAL ? "/say, to nobody in particular"
                                                                                    : "party chat"));
        if (prompt.empty() || prompt == "[Format Error]")
            return;
        prompt += Roleplay_BuildVoicePrompt(bot);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.source            = source;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = true;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        if (OllamaDispatch_Submit(std::move(request)))
            ++g_stats.remarks;
    }

    // `bot` has been getting on and off its mount on the spot. False when it
    // was after nothing this can keep it off.
    bool TakeTheHint(Player* bot, const Mounted& at, size_t times)
    {
        ++g_stats.stuck;
        PlayerbotAI* ai = AiOf(bot);
        if (!ai)
            return false;
        const auto now = Clock::now();
        const char* company = CompanyOf(bot, ai);
        const bool onFoot = GoOnFoot(bot, ai, now);
        Target t;
        if (!FindTarget(bot, ai, t))
        {
            ++g_stats.nothing;
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] {} mounted {} times in {}s at ({:.1f}, {:.1f}, {:.1f}) on map {} ({}), {}, with "
                     "nothing it is trying to loot: {}.",
                     bot->GetName(), times, g_StuckWindowSeconds, at.x, at.y, at.z, at.map, ZoneName(bot), company,
                     onFoot ? fmt::format("on foot for {} min", g_StuckWalkMinutes) : std::string("left to playerbots"));
            return false;
        }

        ++g_stats.hinted;
        Hold hold;
        hold.object  = t.guid;
        hold.spawnId = t.spawnId;
        hold.map     = bot->GetMapId();
        hold.x       = t.pos.GetPositionX();
        hold.y       = t.pos.GetPositionY();
        hold.z       = t.pos.GetPositionZ();
        hold.until   = now + std::chrono::minutes(std::max<uint32_t>(1, g_StuckIgnoreMinutes));
        KeepOff(ai, hold);

        std::vector<Hold>& holds = g_holds[bot->GetGUID().GetRawValue()];
        holds.erase(std::remove_if(holds.begin(), holds.end(), [&](const Hold& h) { return h.object == hold.object; }),
                    holds.end());
        holds.push_back(hold);
        if (holds.size() > 8)
            holds.erase(holds.begin());

        Memory& m = g_memory[bot->GetGUID().GetRawValue()];
        m.what = t.what;
        m.verb = t.verb;
        m.zone = ZoneName(bot);
        m.when = now;

        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] {} was stuck getting on and off its mount at {} ({}, {} times in {}s) at "
                 "({:.1f}, {:.1f}, {:.1f}) on map {} ({}), {}{}: keeping it off that for {} min{}.",
                 bot->GetName(), t.what, t.verb, times, g_StuckWindowSeconds, at.x, at.y, at.z, at.map, m.zone,
                 company, OtherAt(bot, t.pos), g_StuckIgnoreMinutes,
                 onFoot ? fmt::format(", and on foot for {} min", g_StuckWalkMinutes) : std::string());

        Remark(bot, ai, t);
        return true;
    }

    void Look(uint64_t guid, Track& track, const Clock::time_point now)
    {
        const auto window = std::chrono::seconds(std::max<uint32_t>(10, g_StuckWindowSeconds));
        while (!track.recent.empty() && now - track.recent.front().when > window)
            track.recent.pop_front();
        if (track.recent.size() < std::max<uint32_t>(2, g_StuckMountCount))
            return;
        if (track.lastLook != Clock::time_point{} && now - track.lastLook < window)
            return;   // dealt with this one already

        // All on the spot, none in a fight. A copy: the deque is cleared below.
        const Mounted last = track.recent.back();
        for (const Mounted& m : track.recent)
            if (m.combat || m.map != last.map || Distance(m, last) > g_StuckRadius)
                return;

        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
        if (!bot || !bot->IsInWorld() || !OllamaIsBotPlayer(bot) || !bot->IsAlive() || bot->IsInCombat() ||
            bot->InBattleground() || bot->InArena() || bot->GetVehicle() || bot->IsInFlight())
            return;

        track.lastLook = now;
        const size_t times = track.recent.size();
        track.recent.clear();
        // Nothing to keep it off: looked at again in ten minutes, not every
        // minute, so a loop of some other kind is one log line, not a stream.
        if (!TakeTheHint(bot, last, times))
            track.lastLook = now + std::chrono::minutes(9);
    }
}

void OllamaChatStuckScript::OnAuraApply(Unit* unit, Aura* aura)
{
    // Map threads: note it, nothing more.
    if (!g_Enable || !g_StuckEnable || !unit || !aura || !unit->IsPlayer())
        return;
    SpellInfo const* info = aura->GetSpellInfo();
    if (!info || !info->HasAura(SPELL_AURA_MOUNTED))
        return;

    Mounted m;
    m.guid   = unit->GetGUID().GetRawValue();
    m.when   = Clock::now();
    m.map    = unit->GetMapId();
    m.x      = unit->GetPositionX();
    m.y      = unit->GetPositionY();
    m.z      = unit->GetPositionZ();
    m.combat = unit->IsInCombat();

    std::lock_guard<std::mutex> lock(g_noteMutex);
    if (g_noted.size() < kMaxNoted)
        g_noted.push_back(m);
}

void Stuck_Update(uint32_t /*diff*/)
{
    // Also called with the whole module switched off, so the bots on foot get
    // their mounts back.
    if (!g_Enable || !g_StuckEnable)
    {
        if (!g_walkers.empty())
            UpdateWalkers(Clock::now(), true);
        if (!g_holds.empty() || !g_tracks.empty())
        {
            g_holds.clear();
            g_tracks.clear();
        }
        std::lock_guard<std::mutex> lock(g_noteMutex);
        g_noted.clear();
        return;
    }

    std::vector<Mounted> fresh;
    {
        std::lock_guard<std::mutex> lock(g_noteMutex);
        fresh.swap(g_noted);
    }
    const auto now = Clock::now();

    // Once a second: the bots on foot whose time is up (all of them, with
    // WalkMinutes set to 0 by a reload) get their mounts back.
    static Clock::time_point nextWalkCheck{};
    if (!g_walkers.empty() && now >= nextWalkCheck)
    {
        nextWalkCheck = now + std::chrono::seconds(1);
        UpdateWalkers(now, g_StuckWalkMinutes == 0);
    }

    for (const Mounted& m : fresh)
    {
        ++g_stats.mounts;
        Track& track = g_tracks[m.guid];
        track.recent.push_back(m);
        while (track.recent.size() > 32)
            track.recent.pop_front();
        Look(m.guid, track, now);
    }

    // What the bots that took a hint are kept off. Every tick, while they are
    // near it: the AI that would reach for it again runs between these.
    for (auto it = g_holds.begin(); it != g_holds.end();)
    {
        std::vector<Hold>& holds = it->second;
        holds.erase(std::remove_if(holds.begin(), holds.end(), [&](const Hold& h) { return now >= h.until; }),
                    holds.end());
        Player* bot = holds.empty() ? nullptr : ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->first));
        PlayerbotAI* ai = bot && bot->IsInWorld() ? AiOf(bot) : nullptr;
        if (!ai)
        {
            it = g_holds.erase(it);
            continue;
        }
        for (const Hold& h : holds)
            if (bot->GetMapId() == h.map && bot->GetDistance(h.x, h.y, h.z) < 60.0f)
                KeepOff(ai, h);
        ++it;
    }

    // Now and then: tracks and memories nobody needs any more.
    static Clock::time_point nextSweep{};
    if (now >= nextSweep)
    {
        nextSweep = now + std::chrono::seconds(30);
        const auto window = std::chrono::seconds(std::max<uint32_t>(10, g_StuckWindowSeconds));
        for (auto it = g_tracks.begin(); it != g_tracks.end();)
        {
            const bool idle = it->second.recent.empty() || now - it->second.recent.back().when > window;
            const bool looked = it->second.lastLook != Clock::time_point{} && now - it->second.lastLook < window;
            it = (idle && !looked) ? g_tracks.erase(it) : std::next(it);
        }
        const auto remember = std::chrono::minutes(std::max<uint32_t>(1, g_StuckRememberMinutes));
        for (auto it = g_memory.begin(); it != g_memory.end();)
            it = (now - it->second.when > remember) ? g_memory.erase(it) : std::next(it);
    }
}

std::string Stuck_StateTextFor(Player* bot)
{
    if (!bot || !g_StuckEnable || g_StuckStateText.empty())
        return {};
    auto it = g_memory.find(bot->GetGUID().GetRawValue());
    if (it == g_memory.end() ||
        Clock::now() - it->second.when > std::chrono::minutes(std::max<uint32_t>(1, g_StuckRememberMinutes)))
        return {};
    const Memory& m = it->second;
    const std::string out = SafeFormat(g_StuckStateText,
                                       fmt::arg("what", m.what),
                                       fmt::arg("verb", m.verb),
                                       fmt::arg("zone", m.zone.empty() ? std::string("here") : m.zone));
    return out == "[Format Error]" ? std::string() : out;
}

StuckStats Stuck_GetStats()
{
    StuckStats s = g_stats;
    for (const auto& [guid, holds] : g_holds)
        s.holding += uint32_t(holds.size());
    s.walking = uint32_t(g_walkers.size());
    return s;
}
