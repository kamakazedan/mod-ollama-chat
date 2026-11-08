#include "mod-ollama-chat_wipe.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_revive.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

// mod-dungeon-clear's tank is the group's tank while it is around.
#if __has_include("DcLeaderSignal.h") && __has_include("DcModuleEnable.h")
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#define OLLAMA_WIPE_DUNGEON_CLEAR 1
#else
#define OLLAMA_WIPE_DUNGEON_CLEAR 0
#endif

#include <algorithm>
#include <chrono>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Watch
    {
        std::unordered_map<uint64_t, bool>     alive;    // as last seen
        std::unordered_map<uint64_t, uint32_t> deaths;   // in this group
        uint64_t          tank      = 0;     // the last one seen tanking, alive
        uint32_t          map       = 0;     // where the group last fought
        uint32_t          instance  = 0;
        bool              fought    = false;
        Clock::time_point lastFight{};
        Clock::time_point lastSeen{};
        bool              tankDownSaid = false;   // this fight
        bool              wiped        = false;
        Clock::time_point wipedAt{};
        uint64_t          firstDown    = 0;       // the first to die in this fight
        bool              runBackSaid  = false;   // these deaths
    };
    std::unordered_map<uint32_t, Watch> g_watch;   // by the group's guid counter
    WipeStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    bool Chance(uint32_t percent)
    {
        if (percent == 0)
            return false;
        return percent >= 100 || std::uniform_int_distribution<uint32_t>(1, 100)(Rng()) <= percent;
    }

    uint32_t Between(uint32_t lo, uint32_t hi)
    {
        return std::uniform_int_distribution<uint32_t>(lo, std::max(lo, hi))(Rng());
    }

    template <typename T>
    T* Pick(std::vector<T*>& from)
    {
        return from.empty() ? nullptr : from[Between(0, uint32_t(from.size() - 1))];
    }

    // "Lahu", "Lahu and Kael", "Lahu, Kael and Mira".
    std::string Names(const std::vector<Player*>& who)
    {
        std::vector<std::string> names;
        for (Player* p : who)
            names.push_back(p->GetName());
        return CrowdText::JoinNames(names);
    }

    bool Here(const Watch& w, Player* p)
    {
        return p->IsInWorld() && p->GetMapId() == w.map && p->GetInstanceId() == w.instance;
    }

    // Where the group fights, or a ghost whose body is there: one of the dead
    // of that fight until it is back up.
    bool InFight(const Watch& w, Player* p)
    {
        return Here(w, p) || (!p->IsAlive() && p->GetCorpseLocation().GetMapId() == w.map);
    }

    Player* TankOf(Group* group, Player* reference)
    {
#if OLLAMA_WIPE_DUNGEON_CLEAR
        if (DcModule::IsEnabled())
            if (Player* tank = DcLeaderSignal::FindLeaderTank(reference))
                return tank;
#else
        (void)reference;
#endif
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && member->IsAlive() && PlayerbotAI::IsTank(member))
                    return member;
        return nullptr;
    }

    std::string BotPrompt(Player* bot, const std::string& templ, const Watch& w)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (templ.empty() || !ai || !ai->GetChatHelper())
            return {};
        Player* tank = w.tank ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(w.tank)) : nullptr;
        const std::string out = SafeFormat(templ,
                                           fmt::arg("bot_name", bot->GetName()),
                                           fmt::arg("bot_level", bot->GetLevel()),
                                           fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                           fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                           fmt::arg("bot_personality",
                                                    GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                           fmt::arg("tank", tank ? tank->GetName() : std::string("your tank")),
                                           fmt::arg("place", bot->GetMap() ? std::string(bot->GetMap()->GetMapName())
                                                                           : std::string("the dungeon")));
        if (out.empty() || out == "[Format Error]")
            return {};
        return out + Roleplay_BuildVoicePrompt(bot);
    }

    // One line in the group's chat: the model's, or now and then a typed one
    // -- which also goes out when the model cannot be reached.
    bool Say(Player* bot, Group* group, const std::string& prompt, const std::vector<std::string>& typed,
             uint32_t typedChance, uint32_t delayMs, bool answerable)
    {
        if (!bot || !group || !OllamaIsBotPlayer(bot))
            return false;
        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        if (g_DisableForParty)
            return false;

        std::string stock;
        if (!typed.empty() && !g_RoleplayEnable)   // a bare "uh oh" would break character
            stock = Personality_StyleCanned(bot, typed[Between(0, uint32_t(typed.size() - 1))]);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.source            = source;
        request.scopeKey          = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                          group->GetGUID().GetCounter());
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.directAddress     = true;   // the moment calls for it: no pacing cooldown
        request.noEcho            = true;   // "uh oh" is what everyone says
        request.triggerBotReplies = answerable;
        request.extraDelayMs      = delayMs;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        if (!stock.empty() && (prompt.empty() || Chance(typedChance)))
            request.fixedText = stock;
        else if (!prompt.empty())
        {
            request.prompt       = prompt;
            request.fallbackText = stock;
        }
        else
            return false;
        return OllamaDispatch_Submit(std::move(request));
    }

    // Can anyone in `who` bring a dead one back on their own: a soulstone, or
    // a reincarnation ready?
    Player* SelfRes(const std::vector<Player*>& who)
    {
        for (Player* p : who)
            if (!p->IsAlive() && !p->HasPlayerFlag(PLAYER_FLAGS_GHOST) && p->GetResurrectionSpellId())
                return p;
        return nullptr;
    }

    void TankDown(Watch& w, Group* group, Player* tank, const std::vector<Player*>& here)
    {
        w.tankDownSaid = true;
        if (!Chance(g_WipeTankDownChance))
            return;
        std::vector<Player*> standing, fighting;
        for (Player* p : here)
            if (p != tank && p->IsAlive() && OllamaIsBotPlayer(p))
                (p->IsInCombat() ? fighting : standing).push_back(p);
        Player* bot = fighting.empty() ? Pick(standing) : Pick(fighting);
        if (!bot)
            return;
        if (Say(bot, group, BotPrompt(bot, g_WipeTankDownPrompt, w), g_WipeTankDownLines, g_WipeTypedChance,
                Between(300, 1500), false))
        {
            ++g_stats.tankDown;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {}'s tank {} went down mid-fight; {} says so.",
                         group->GetLeaderName(), tank->GetName(), bot->GetName());
        }
    }

    void Wiped(Watch& w, Group* group, const std::vector<Player*>& here)
    {
        ++g_stats.wipes;
        uint32_t delay = Between(2500, 4500);

        Player* tank = w.tank ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(w.tank)) : nullptr;
        if (tank && std::find(here.begin(), here.end(), tank) == here.end())
            tank = nullptr;
        if (tank && OllamaIsBotPlayer(tank) && Chance(g_WipeApologyChance) &&
            Say(tank, group, BotPrompt(tank, g_WipeApologyPrompt, w), g_WipeApologyLines, g_WipeTypedChance, delay,
                true))
        {
            ++g_stats.apologies;
            delay += Between(3500, 6000);
        }

        if (SelfRes(here) || !Chance(g_WipeNoRezChance))
            return;
        std::vector<Player*> others;
        for (Player* p : here)
            if (p != tank && OllamaIsBotPlayer(p))
                others.push_back(p);
        Player* bot = others.empty() ? (tank && OllamaIsBotPlayer(tank) ? tank : nullptr) : Pick(others);
        if (bot && Say(bot, group, BotPrompt(bot, g_WipeNoRezPrompt, w), g_WipeNoRezLines, g_WipeTypedChance, delay,
                       true))
            ++g_stats.noRez;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {}'s group wiped{}.", group->GetLeaderName(),
                     tank ? " (tank: " + tank->GetName() + ")" : std::string());
    }

    void Watch1(Group* group, Watch& w, Clock::time_point now)
    {
        std::vector<Player*> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && !member->IsBeingTeleported())
                    members.push_back(member);
        if (members.empty())
            return;

        // Where it is fought; who tanks it.
        bool fighting = false;
        for (Player* m : members)
        {
            if (!m->IsAlive() || !m->IsInCombat())
                continue;
            fighting = true;
            if (!w.fought || m->GetMapId() != w.map || m->GetInstanceId() != w.instance)
            {
                w.map      = m->GetMapId();
                w.instance = m->GetInstanceId();
                w.fought   = true;
            }
        }
        if (fighting)
            w.lastFight = now;
        if (!w.fought)
        {
            // Never seen fighting: where the first of them is.
            w.map      = members.front()->GetMapId();
            w.instance = members.front()->GetInstanceId();
        }
        // Who tanks this fight: decided before it, and kept through it, so the
        // off-tank taking over when the tank dies does not hide that it did.
        const uint64_t tankBefore = w.tank;
        if (!fighting || !w.tank)
            if (Player* tank = TankOf(group, members.front()))
                if (tank->IsAlive())
                    w.tank = tank->GetGUID().GetRawValue();

        const bool recent = w.fought && now - w.lastFight <= std::chrono::seconds(3);
        std::vector<Player*> here, died;
        uint32_t dead = 0;
        for (Player* m : members)
        {
            const uint64_t guid = m->GetGUID().GetRawValue();
            const bool alive = m->IsAlive();
            auto it = w.alive.find(guid);
            const bool was = it == w.alive.end() ? alive : it->second;
            w.alive[guid] = alive;
            if (was && !alive)
            {
                ++w.deaths[guid];
                died.push_back(m);
                if (!w.firstDown && recent)
                    w.firstDown = guid;
            }
            if (InFight(w, m))
            {
                here.push_back(m);
                dead += alive ? 0 : 1;
            }
        }

        // The fight is over and everyone is up: a clean slate.
        if (!fighting && dead == 0)
        {
            w.wiped        = false;
            w.firstDown    = 0;
            w.runBackSaid  = false;
            if (now - w.lastFight > std::chrono::seconds(5))
                w.tankDownSaid = false;
        }

        // The tank, down in the middle of it, others still standing and still
        // fighting.
        if (!w.tankDownSaid && fighting && tankBefore && dead < here.size())
            for (Player* m : died)
                if (m->GetGUID().GetRawValue() == tankBefore && InFight(w, m))
                {
                    TankDown(w, group, m, here);
                    break;
                }

        // Everyone where they were fighting is dead.
        if (!w.wiped && here.size() >= 2 && dead == here.size() && w.fought &&
            now - w.lastFight <= std::chrono::seconds(20))
        {
            w.wiped   = true;
            w.wipedAt = now;
            Wiped(w, group, here);
            return;
        }

        // A fight over with some dead and nobody up who can bring them back:
        // one of the dead says it is running back.
        if (!w.wiped && !w.runBackSaid && !fighting && dead && dead < here.size() &&
            now - w.lastFight >= std::chrono::seconds(g_WipeRunBackQuietSeconds))
        {
            std::vector<Player*> deadBots;
            bool rezzer = false;
            for (Player* m : here)
            {
                if (m->IsAlive())
                    rezzer = rezzer || Revive_KnowsResurrect(m);
                else if (OllamaIsBotPlayer(m))
                    deadBots.push_back(m);
            }
            w.runBackSaid = true;
            if (rezzer || deadBots.empty() || SelfRes(here) || !Chance(g_WipeRunBackChance))
                return;
            Player* bot = Pick(deadBots);
            if (Say(bot, group, BotPrompt(bot, g_WipeRunBackPrompt, w), g_WipeRunBackLines, g_WipeTypedChance,
                    Between(500, 2500), true))
                ++g_stats.runBack;
        }
    }
}

void Wipe_Update(uint32_t diff)
{
    if (!g_WipeEnable)
        return;
    static uint32_t timer = 0;
    timer += diff;
    if (timer < 1000)
        return;
    timer = 0;

    const Clock::time_point now = Clock::now();
    std::unordered_set<uint32_t> looked;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* human = entry.second;
        if (!human || !human->IsInWorld() || OllamaIsBotPlayer(human))
            continue;
        Group* group = human->GetGroup();
        if (!group || group->isBGGroup() || group->isBFGroup() || human->InBattleground() || human->InArena())
            continue;
        const uint32_t key = group->GetGUID().GetCounter();
        if (!looked.insert(key).second)
            continue;
        Watch& w = g_watch[key];
        w.lastSeen = now;
        Watch1(group, w, now);
    }

    for (auto it = g_watch.begin(); it != g_watch.end();)
        it = (now - it->second.lastSeen > std::chrono::minutes(10)) ? g_watch.erase(it) : std::next(it);
}

std::string Wipe_StateTextFor(Player* bot)
{
    if (!g_WipeEnable || !bot)
        return {};
    Group* group = bot->GetGroup();
    if (!group)
        return {};
    auto it = g_watch.find(group->GetGUID().GetCounter());
    if (it == g_watch.end())
        return {};
    const Watch& w = it->second;
    const auto now = Clock::now();

    std::vector<Player*> dead, up, rezzers;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (!m || !m->IsInWorld())
            continue;
        if (!InFight(w, m))
            continue;
        if (!m->IsAlive())
            dead.push_back(m);
        else
        {
            up.push_back(m);
            if (Revive_KnowsResurrect(m))
                rezzers.push_back(m);
        }
    }

    Player* tank = w.tank ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(w.tank)) : nullptr;
    auto fill = [&](const std::string& templ, const std::string& rez, const std::string& extra) -> std::string
    {
        std::string first;
        if (w.firstDown && tank && w.firstDown == w.tank && !g_WipeStateTankFirstText.empty())
            first = SafeFormat(g_WipeStateTankFirstText, fmt::arg("tank", tank->GetName()));
        std::vector<Player*> others;
        for (Player* p : dead)
            if (p != bot)
                others.push_back(p);
        const std::string out = SafeFormat(templ,
                                           fmt::arg("dead", Names(others)),
                                           fmt::arg("is_are", others.size() == 1 ? "is" : "are"),
                                           fmt::arg("you", bot->IsAlive() ? "" : " (you too)"),
                                           fmt::arg("first", first == "[Format Error]" ? std::string() : first),
                                           fmt::arg("place", bot->GetMap() ? std::string(bot->GetMap()->GetMapName())
                                                                           : std::string("the dungeon")),
                                           fmt::arg("rez", rez));
        if (out.empty() || out == "[Format Error]")
            return {};
        return extra.empty() ? out : out + " " + extra;
    };

    if (w.wiped && now - w.wipedAt <= std::chrono::minutes(g_WipeStateMinutes))
    {
        std::string rez;
        if (Player* self = SelfRes(dead))
            rez = SafeFormat(g_WipeStateSelfResText, fmt::arg("name", self->GetName()));
        else if (!rezzers.empty())
            rez = SafeFormat(g_WipeStateRezzerText, fmt::arg("rezzers", Names(rezzers)),
                             fmt::arg("them", dead.size() == 1 ? "them" : "the rest"));
        else
            rez = g_WipeStateRunBackText;
        std::string back;
        if (!up.empty() && !dead.empty() && !g_WipeStateBackText.empty())
            back = SafeFormat(g_WipeStateBackText, fmt::arg("back", Names(up)),
                              fmt::arg("is_are", up.size() == 1 ? "is" : "are"));
        if (dead.empty())
            return {};   // all back up
        return fill(g_WipeStateWipeText, rez == "[Format Error]" ? std::string() : rez,
                    back == "[Format Error]" ? std::string() : back);
    }

    std::vector<Player*> others;
    for (Player* p : dead)
        if (p != bot)
            others.push_back(p);
    if (others.empty())
        return {};   // only itself: Prompt.State*Text says so
    std::string rez;
    if (!rezzers.empty())
        rez = SafeFormat(g_WipeStateRezzerText, fmt::arg("rezzers", Names(rezzers)),
                         fmt::arg("them", others.size() == 1 ? "them" : "them all"));
    else if (Player* self = SelfRes(dead))
        rez = SafeFormat(g_WipeStateSelfResText, fmt::arg("name", self->GetName()));
    else
        rez = g_WipeStateNoRezzerText;
    std::string tankNote;
    if (tank && !tank->IsAlive() && tank != bot && std::find(others.begin(), others.end(), tank) != others.end() &&
        !g_WipeStateTankDeadText.empty())
        tankNote = SafeFormat(g_WipeStateTankDeadText, fmt::arg("tank", tank->GetName()));
    std::string out = fill(g_WipeStateDeadText, rez == "[Format Error]" ? std::string() : rez, std::string());
    if (!tankNote.empty() && tankNote != "[Format Error]")
        out += " " + tankNote;
    return out;
}

uint32_t Wipe_DeathsOf(Player* member)
{
    if (!member || !member->GetGroup())
        return 0;
    auto it = g_watch.find(member->GetGroup()->GetGUID().GetCounter());
    if (it == g_watch.end())
        return 0;
    auto d = it->second.deaths.find(member->GetGUID().GetRawValue());
    return d == it->second.deaths.end() ? 0 : d->second;
}

WipeStats Wipe_GetStats()
{
    return g_stats;
}
