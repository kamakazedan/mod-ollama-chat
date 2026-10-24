#include "mod-ollama-chat_presence.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <chrono>
#include <list>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Next moment allowed near each real player, per bot, and per bot and
    // player pair. World thread only. Entries that have come due are pruned
    // every minute, so a character that logs out is gone within a minute of
    // its moment passing -- nothing has to be erased from the (map-thread)
    // logout hook.
    std::unordered_map<uint64_t, Clock::time_point> g_playerNext;
    std::unordered_map<uint64_t, Clock::time_point> g_botNext;
    std::unordered_map<uint64_t, Clock::time_point> g_pairNext;

    PresenceStats g_stats;
    uint32_t      g_timer = 0;
    uint32_t      g_pruneTimer = 0;

    uint64_t PairKey(uint64_t bot, uint64_t player)
    {
        // Collisions only ever cost a skipped moment.
        return (bot * 0x9E3779B97F4A7C15ULL) ^ player;
    }

    bool Due(const std::unordered_map<uint64_t, Clock::time_point>& map, uint64_t key, Clock::time_point now)
    {
        auto it = map.find(key);
        return it == map.end() || now >= it->second;
    }

    std::string MountName(Player* p)
    {
        if (!p->IsMounted())
            return "";
        const Unit::AuraEffectList& auras = p->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        if (auras.empty())
            return "a mount";
        SpellInfo const* info = auras.front()->GetSpellInfo();
        if (!info || !info->SpellName[0] || !*info->SpellName[0])
            return "a mount";
        return info->SpellName[0];
    }

    std::string ClassAndRace(Player* subject, Player* observer)
    {
        PlayerbotAI* ai = observer ? PlayerbotsMgr::instance().GetPlayerbotAI(observer) : nullptr;
        if (!ai || !ai->GetChatHelper())
            return "adventurer";
        return ai->GetChatHelper()->FormatRace(subject->getRace()) + " " +
               ai->GetChatHelper()->FormatClass(subject->getClass());
    }

    void Prune(std::unordered_map<uint64_t, Clock::time_point>& map, Clock::time_point now)
    {
        for (auto it = map.begin(); it != map.end(); )
        {
            if (now >= it->second)
                it = map.erase(it);
            else
                ++it;
        }
    }

    void SubmitRemark(Player* bot, Player* subject, const std::string& observation)
    {
        if (g_DisableForSayYell || g_PresencePromptTemplate.empty())
            return;

        // Cross-faction /say arrives as gibberish.
        if (Roleplay_IsLanguageBarrier(bot, subject))
            return;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->GetChatHelper())
            return;

        const std::string scopeKey = Governor_MakeScopeKey(
            ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, "", 0, bot->GetZoneId());
        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
            return;

        const std::string personality = GetBotPersonality(bot);

        std::string prompt = SafeFormat(
            g_PresencePromptTemplate,
            fmt::arg("bot_name", bot->GetName()),
            fmt::arg("bot_level", bot->GetLevel()),
            fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
            fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
            fmt::arg("bot_personality", GetPersonalityPromptAddition(personality)),
            fmt::arg("bot_personality_name", personality),
            fmt::arg("player_name", subject->GetName()),
            fmt::arg("observation", observation),
            fmt::arg("pass_token", g_ContextPassToken));
        if (prompt.empty() || prompt == "[Format Error]")
            return;

        // The gesture has just been made; the line comes without another.
        prompt += Roleplay_BuildVoicePrompt(bot);
        prompt += Outlook_ForBot(bot);   // includes what a passer-by can actually see

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = subject->GetGUID().GetRawValue();
        request.source            = SRC_SAY_LOCAL;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::RandomChatter;
        request.triggerBotReplies = true;

        if (OllamaDispatch_Submit(std::move(request)))
            ++g_stats.remarks;
    }
}

std::string Presence_Describe(Player* subject, Player* observer, ObjectGuid* outPetGuid)
{
    if (outPetGuid)
        *outPetGuid = ObjectGuid::Empty;
    if (!subject)
        return "";

    std::string text = SafeFormat("{}, a level {} {}", subject->GetName(), subject->GetLevel(),
                                  ClassAndRace(subject, observer));

    if (subject->IsMounted())
        text += ", riding " + MountName(subject);
    else if (subject->GetUInt32Value(UNIT_NPC_EMOTESTATE) == EMOTE_STATE_DANCE)
        text += ", dancing";
    else if (subject->IsSitState())
        text += ", sitting down";
    else if (subject->isAFK())
        text += ", standing there AFK";
    else if (subject->isMoving())
        text += ", walking past";
    else
        text += ", standing around";

    // Their own pet, or else a companion: the thing people coo over.
    if (Pet* pet = subject->GetPet())
    {
        if (pet->IsInWorld() && pet->IsAlive() && pet->IsWithinDistInMap(subject, 20.0f))
        {
            std::string family;
            if (CreatureTemplate const* ct = pet->GetCreatureTemplate())
                if (CreatureFamilyEntry const* fe = sCreatureFamilyStore.LookupEntry(ct->family))
                    if (fe->Name[0] && *fe->Name[0])
                        family = fe->Name[0];

            if (!family.empty() && family != pet->GetName())
                text += SafeFormat(", with their {} {}", family, pet->GetName());
            else
                text += SafeFormat(", with their pet {}", pet->GetName());

            if (outPetGuid)
                *outPetGuid = pet->GetGUID();
            return text;
        }
    }

    const ObjectGuid critterGuid = subject->GetCritterGUID();
    if (!critterGuid.IsEmpty())
    {
        if (Creature* critter = ObjectAccessor::GetCreature(*subject, critterGuid))
        {
            if (critter->IsInWorld() && critter->IsWithinDistInMap(subject, 20.0f))
            {
                text += SafeFormat(", with a little {} following them", critter->GetName());
                if (outPetGuid)
                    *outPetGuid = critter->GetGUID();
            }
        }
    }

    return text;
}

void Presence_Update(uint32_t diff)
{
    if (!g_Enable || !g_PresenceEnable || !g_EnableBotEmotes)
        return;

    g_timer += diff;
    if (g_timer < 2500)
        return;
    g_timer = 0;

    const auto now = Clock::now();

    g_pruneTimer += 2500;
    if (g_pruneTimer >= 60000)
    {
        g_pruneTimer = 0;
        Prune(g_playerNext, now);
        Prune(g_botNext, now);
        Prune(g_pairNext, now);
    }

    OllamaWorldSnapshot world;
    world.Build();

    for (Player* real : world.realPlayers)
    {
        if (!real || !real->IsInWorld() || real->isDead() || real->IsInCombat() ||
            real->IsBeingTeleported() || real->IsInFlight() || real->IsGameMaster())
            continue;

        Map* map = real->GetMap();
        if (!map || map->Instanceable())
            continue;

        if (g_PresenceCityOnly && !OllamaIsSocialHub(real))
            continue;

        const uint64_t realKey = real->GetGUID().GetRawValue();
        if (!Due(g_playerNext, realKey, now))
            continue;

        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(real, g_PresenceRange, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(real, found, check);
        Cell::VisitObjects(real, searcher, g_PresenceRange);

        Group* realGroup = real->GetGroup();
        std::vector<Player*> bots;
        for (Player* p : found)
        {
            if (!p || p == real || !p->IsInWorld() || !OllamaIsBotPlayer(p))
                continue;
            if (p->IsInCombat() || p->IsInFlight() || p->IsBeingTeleported())
                continue;
            if (realGroup && p->GetGroup() == realGroup)
                continue;   // their own party is not passing by
            const uint64_t botKey = p->GetGUID().GetRawValue();
            if (!Due(g_botNext, botKey, now) || !Due(g_pairNext, PairKey(botKey, realKey), now))
                continue;
            if (!p->IsWithinLOSInMap(real))
                continue;
            bots.push_back(p);
        }

        if (bots.empty())
        {
            // Nobody near right now; look again shortly.
            g_playerNext[realKey] = now + std::chrono::seconds(4);
            continue;
        }

        // The moment is spent whether or not the bot does anything with it.
        const uint32_t lo = std::min(g_PresencePlayerIntervalMin, g_PresencePlayerIntervalMax);
        const uint32_t hi = std::max(g_PresencePlayerIntervalMin, g_PresencePlayerIntervalMax);
        g_playerNext[realKey] = now + std::chrono::seconds(lo == hi ? lo : urand(lo, hi));

        if (urand(0, 99) >= g_PresenceChance)
            continue;

        Player* bot = bots[urand(0, static_cast<uint32>(bots.size() - 1))];

        ObjectGuid petGuid;
        const std::string observation = Presence_Describe(real, bot, &petGuid);

        bool petEmote = false;
        const uint32_t emote = Expression_PickStyleEmote(bot, !petGuid.IsEmpty(), &petEmote);
        if (!emote)
            continue;

        const ObjectGuid target = (petEmote && !petGuid.IsEmpty()) ? petGuid : real->GetGUID();
        ScheduleBotExpression(bot, target, emote, urand(200, 900));

        ++g_stats.emotes;
        if (target == petGuid)
            ++g_stats.petEmotes;

        const uint64_t botKey = bot->GetGUID().GetRawValue();
        g_botNext[botKey] = now + std::chrono::seconds(g_PresenceBotCooldownSeconds);
        g_pairNext[PairKey(botKey, realKey)] = now + std::chrono::seconds(g_PresencePairCooldownSeconds);

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Presence: {} -> {} ({}), emote {}.",
                     bot->GetName(), real->GetName(), target == petGuid ? "their pet" : "them",
                     LookupTextEmoteName(emote));

        if (g_PresenceSayChance > 0 && urand(0, 99) < g_PresenceSayChance)
            SubmitRemark(bot, real, observation);
    }
}

void Presence_Reset()
{
    g_playerNext.clear();
    g_botNext.clear();
    g_pairNext.clear();
}

PresenceStats Presence_GetStats()
{
    return g_stats;
}
