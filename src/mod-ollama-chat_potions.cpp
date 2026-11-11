#include "mod-ollama-chat_potions.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <cctype>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr size_t kMaxNoted   = 256;
    constexpr auto   kCombatWait = std::chrono::minutes(3);   // still fighting after this: let it go

    struct Noted
    {
        uint64_t          bot     = 0;
        uint32_t          entry   = 0;       // the potion
        bool              mana    = false;   // a mana potion (else a healing one)
        uint32_t          pct     = 0;       // health or mana, percent, as it was drunk
        Clock::time_point drunk{};
        Clock::time_point at{};              // looked at from then
        bool              calm    = false;   // the fight is over: one last breath, then it speaks
    };

    // From the hook, any map thread.
    std::mutex         g_mutex;
    std::vector<Noted> g_noted;
    uint64_t           g_notedCount = 0;

    // World thread.
    std::vector<Noted>                              g_pending;
    std::unordered_map<uint64_t, Clock::time_point> g_botSaid;     // bot -> its last line about a potion
    std::unordered_map<uint32_t, Clock::time_point> g_groupSaid;   // group -> the last one there
    PotionStats                                     g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint32_t ChanceFor(Player* bot)
    {
        std::string style = GetBotPersonality(bot);
        for (char& c : style)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto it = g_PotionsPersonalities.find(style);
        return it == g_PotionsPersonalities.end() ? g_PotionsChance : it->second;
    }

    bool Cooling(Clock::time_point last, Clock::time_point now, Clock::duration span)
    {
        return last != Clock::time_point{} && now - last < span;
    }

    // True when done with it (said, or not to be); false to look again soon.
    bool Consider(Noted& n, Clock::time_point now)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(n.bot));
        if (!bot || !bot->IsInWorld() || !OllamaIsBotPlayer(bot))
            return true;
        if (!bot->IsAlive())
            return true;   // it did not save them after all
        Map* map = bot->GetMap();
        if (!map || !map->IsDungeon())
            return true;
        Group* group = bot->GetGroup();
        if (!group || g_DisableForParty || !OllamaGroupHasRealPlayer(bot))
            return true;

        // After the fight, once they have caught their breath.
        if (bot->IsInCombat())
        {
            n.calm = false;
            return now - n.drunk > kCombatWait;
        }
        if (!n.calm)
        {
            n.calm = true;
            n.at   = now + std::chrono::seconds(urand(2, 5));
            return false;
        }
        ++g_stats.lived;

        const uint64_t self    = bot->GetGUID().GetRawValue();
        const uint32_t groupId = group->GetGUID().GetCounter();
        if (Cooling(g_botSaid[self], now, std::chrono::minutes(g_PotionsBotCooldownMinutes)) ||
            Cooling(g_groupSaid[groupId], now, std::chrono::seconds(g_PotionsGroupCooldownSeconds)))
            return true;
        const uint32_t chance = ChanceFor(bot);
        if (!chance || urand(1, 100) > chance)
            return true;   // the kind that just gets on with it

        const std::string& templ = n.mana ? g_PotionsManaPrompt : g_PotionsHealPrompt;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(n.entry);
        if (!ai || !ai->GetChatHelper() || !proto || templ.empty())
            return true;
        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           groupId);
        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
            return true;

        const char* role = PlayerbotAI::IsTank(bot) ? "the group's tank"
                         : PlayerbotAI::IsHeal(bot) ? "the group's healer"
                                                    : "one of the group's damage dealers";
        // "{~that the pot saved you|how close that was|...}": one each time.
        std::string prompt = SafeFormat(TextRules::ExpandChoices(templ, Rng()),
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality",
                                                 GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("role", role),
                                        fmt::arg("dungeon", map->GetMapName()),
                                        fmt::arg("item", TextRules::WithArticle(proto->Name1)),
                                        fmt::arg("pct", n.pct));
        if (prompt.empty() || prompt == "[Format Error]")
            return true;
        prompt += Roleplay_BuildVoicePrompt(bot);

        OllamaChatRequest request;
        request.botGuid           = self;
        request.source            = source;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = true;   // "lol close one" from someone else is fair
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        if (OllamaDispatch_Submit(std::move(request)))
        {
            g_botSaid[self]      = now;
            g_groupSaid[groupId] = now;
            ++g_stats.said;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} drank {} at {}% {} and lived: says so in party.",
                         bot->GetName(), proto->Name1, n.pct, n.mana ? "mana" : "health");
        }
        return true;
    }
}

void OllamaChatPotionScript::OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/)
{
    // A map thread: read what is the player's own, note it, nothing more.
    if (!g_Enable || !g_PotionsEnable || !player || !spell || !player->IsInWorld())
        return;
    Item* item = spell->m_CastItem;
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE)
        return;
    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())
        return;
    SpellInfo const* info = spell->GetSpellInfo();
    if (!info)
        return;

    // What the game says it does: heal (a healing potion, a healthstone) or
    // give back mana (a mana potion, a mana gem).
    bool heals = false, mana = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (info->Effects[i].Effect == SPELL_EFFECT_HEAL)
            heals = true;
        else if (info->Effects[i].Effect == SPELL_EFFECT_ENERGIZE && info->Effects[i].MiscValue == POWER_MANA)
            mana = true;
    }
    if (!heals && !mana)
        return;

    // In a pinch: the health or mana it was drunk at, before it takes effect.
    Noted n;
    const float health = player->GetHealthPct();
    const uint32 maxMana = player->GetMaxPower(POWER_MANA);
    const float manaPct = maxMana ? 100.0f * float(player->GetPower(POWER_MANA)) / float(maxMana) : 100.0f;
    if (heals && health <= float(g_PotionsHealthPct))
        n.pct = uint32(health + 0.5f);
    else if (mana && manaPct <= float(g_PotionsManaPct))
    {
        n.mana = true;
        n.pct  = uint32(manaPct + 0.5f);
    }
    else
        return;
    n.bot   = player->GetGUID().GetRawValue();
    n.entry = proto->ItemId;
    n.drunk = Clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_notedCount;
    if (g_noted.size() < kMaxNoted)
        g_noted.push_back(n);
}

void Potions_Update()
{
    std::vector<Noted> fresh;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fresh.swap(g_noted);
        g_stats.noted = g_notedCount;
    }
    const Clock::time_point now = Clock::now();
    for (Noted& n : fresh)
    {
        // A second potion in the same fight is the same story.
        bool known = false;
        for (Noted& p : g_pending)
            if (p.bot == n.bot)
            {
                known = true;
                p.drunk = n.drunk;
                p.calm  = false;
                if (n.mana == p.mana && n.pct < p.pct)
                    p.pct = n.pct;
            }
        if (known)
            continue;
        n.at = now + std::chrono::seconds(3);
        g_pending.push_back(n);
    }
    if (g_pending.empty())
        return;

    std::vector<Noted> again;
    for (auto it = g_pending.begin(); it != g_pending.end();)
    {
        if (now < it->at)
        {
            ++it;
            continue;
        }
        Noted n = *it;
        it = g_pending.erase(it);
        if (!Consider(n, now))
        {
            if (n.at <= now)
                n.at = now + std::chrono::seconds(3);
            again.push_back(n);
        }
    }
    g_pending.insert(g_pending.end(), again.begin(), again.end());

    // Now and then: cooldowns long run out.
    static Clock::time_point nextSweep{};
    if (now < nextSweep)
        return;
    nextSweep = now + std::chrono::minutes(5);
    const auto botSpan = std::chrono::minutes(g_PotionsBotCooldownMinutes);
    for (auto it = g_botSaid.begin(); it != g_botSaid.end();)
        it = (now - it->second > botSpan) ? g_botSaid.erase(it) : std::next(it);
    const auto groupSpan = std::chrono::seconds(g_PotionsGroupCooldownSeconds);
    for (auto it = g_groupSaid.begin(); it != g_groupSaid.end();)
        it = (now - it->second > groupSpan) ? g_groupSaid.erase(it) : std::next(it);
}

PotionStats Potions_GetStats()
{
    return g_stats;
}
