#include "mod-ollama-chat_presence.h"
#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_mingle.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_gear.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_pets.h"
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

    // Bot and real player who were in a group together, until when that
    // still counts: the group they have just left is not passing them by.
    std::unordered_map<uint64_t, Clock::time_point> g_wereGrouped;

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
        request.gentle            = true;   // a stranger: nothing rude goes with it

        // Only what it was told of: someone on foot is not told "nice mount"
        // (a compliment with nothing real to go on was one), nor someone
        // with no pet out "cute pet".
        const PetsSeen seen = Pets_Seen(subject);
        if (seen.animals.mount.empty())
            request.mustNotSay = g_PetsMountWords;
        if (seen.animals.petKind.empty() && seen.animals.companion.empty())
            request.mustNotSay.insert(request.mustNotSay.end(), g_PetsWords.begin(), g_PetsWords.end());

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

    // Their level, except in a city, where people of every level are and
    // nobody's is a sight (OllamaInCity).
    std::string text = OllamaInCity(observer)
                           ? subject->GetName() + ", " + TextRules::WithArticle(ClassAndRace(subject, observer))
                           : SafeFormat("{}, a level {} {}", subject->GetName(), subject->GetLevel(),
                                        ClassAndRace(subject, observer));

    // Each as what it is: "riding their mount, a Swift Brown Horse", "with
    // their hunter pet, a wolf called Howler", "with their companion pet, a
    // Mechanical Squirrel, following them" (mod-ollama-chat_pets). The mount
    // comes first, as what they are doing.
    const PetsSeen seen = Pets_Seen(subject);
    if (seen.animals.mount.empty())
    {
        if (subject->GetUInt32Value(UNIT_NPC_EMOTESTATE) == EMOTE_STATE_DANCE)
            text += ", dancing";
        else if (subject->IsSitState())
            text += ", sitting down";
        else if (subject->isAFK())
            text += ", standing there AFK";
        else if (subject->isMoving())
            text += ", walking past";
        else
            text += ", standing around";
    }
    for (const std::string& part : TextRules::AnimalPhrases(seen.animals, false, false))
        text += ", " + part;

    // Their own pet, or else a companion: the thing people coo over.
    if (outPetGuid)
        *outPetGuid = !seen.petGuid.IsEmpty() ? seen.petGuid : seen.companionGuid;
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
        Prune(g_wereGrouped, now);
    }

    OllamaWorldSnapshot world;
    world.Build();

    // Who is grouped with whom, every look: once they part, the bots of that
    // group leave them be for a while (no rude gesture as they go).
    if (g_PresenceExGroupMinutes > 0)
        for (Player* real : world.realPlayers)
            if (Group* group = real ? real->GetGroup() : nullptr)
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource())
                        if (member != real && OllamaIsBotPlayer(member))
                            g_wereGrouped[PairKey(member->GetGUID().GetRawValue(), real->GetGUID().GetRawValue())] =
                                now + std::chrono::minutes(g_PresenceExGroupMinutes);

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
            // AFK, or sat down (a bench, the ground): not waving at anyone.
            // Talking to another bot (mod-ollama-chat_mingle): busy.
            if (Hangout_IsAway(p) || p->IsSitState() || Mingle_IsBusy(p))
                continue;
            if (realGroup && p->GetGroup() == realGroup)
                continue;   // their own party is not passing by
            const uint64_t botKey = p->GetGUID().GetRawValue();
            if (!Due(g_botNext, botKey, now) || !Due(g_pairNext, PairKey(botKey, realKey), now))
                continue;
            if (!Due(g_wereGrouped, PairKey(botKey, realKey), now))
                continue;   // their group a moment ago: not a stranger walking by
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
        std::string observation = Presence_Describe(real, bot, &petGuid);
        // Something real to remark on, what catches the eye of what they
        // wear: told to pay a compliment with nothing to go on, a bot paid
        // one to a mount nobody was riding.
        if (const std::string piece = Gear_ShowPiece(real); !piece.empty())
            observation += ", their " + piece + " catching the eye";

        // Nothing rude or gross at a stranger (Presence.SkipEmotes): another
        // of its style's emotes, or none.
        bool petEmote = false;
        uint32_t emote = Expression_PickStyleEmote(bot, !petGuid.IsEmpty(), &petEmote);
        for (int tries = 0; emote && g_PresenceSkipEmotes.contains(emote) && tries < 6; ++tries)
            emote = Expression_PickStyleEmote(bot, !petGuid.IsEmpty(), &petEmote);
        if (!emote || g_PresenceSkipEmotes.contains(emote))
            continue;

        const ObjectGuid target = (petEmote && !petGuid.IsEmpty()) ? petGuid : real->GetGUID();
        ScheduleBotExpression(bot, target, emote, urand(200, 900));

        // Written down where they both are: "why you sighing at me?" is then
        // this bot's to answer, and it knows what it did.
        if (g_EnableBotEmotes)
            Expression_NoteEmote(bot, real, emote, target == petGuid);

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
    g_wereGrouped.clear();
}

PresenceStats Presence_GetStats()
{
    return g_stats;
}
