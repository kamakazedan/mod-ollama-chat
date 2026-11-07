#include "mod-ollama-chat_equip.h"
#include "mod-ollama-chat_bags.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr size_t kMaxNoted = 512;
    constexpr auto   kCombatWait = std::chrono::minutes(2);   // still fighting after this: let it go

    struct Noted
    {
        uint64_t          bot   = 0;
        uint64_t          item  = 0;
        uint32_t          entry = 0;
        Clock::time_point first{};   // when it went on
        Clock::time_point at{};      // looked at from then
    };

    // From the hook, any thread.
    std::mutex          g_mutex;
    std::vector<Noted>  g_noted;
    uint64_t            g_notedCount = 0;

    // World thread.
    std::vector<Noted>                              g_pending;
    std::unordered_map<uint64_t, Clock::time_point> g_botSaid;     // bot -> its last line about gear
    std::unordered_map<uint32_t, Clock::time_point> g_groupSaid;   // group -> the last one there
    EquipStats                                      g_stats;

    // "a rare ring", "an epic two-handed weapon".
    std::string WhatItIs(ItemTemplate const* proto)
    {
        std::string slot;
        switch (proto->InventoryType)
        {
            case INVTYPE_HEAD:           slot = "helm";               break;
            case INVTYPE_NECK:           slot = "necklace";           break;
            case INVTYPE_SHOULDERS:      slot = "pair of shoulders";  break;
            case INVTYPE_CLOAK:          slot = "cloak";              break;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:           slot = "chest piece";        break;
            case INVTYPE_WRISTS:         slot = "pair of bracers";    break;
            case INVTYPE_HANDS:          slot = "pair of gloves";     break;
            case INVTYPE_WAIST:          slot = "belt";               break;
            case INVTYPE_LEGS:           slot = "pair of legs";       break;
            case INVTYPE_FEET:           slot = "pair of boots";      break;
            case INVTYPE_FINGER:         slot = "ring";               break;
            case INVTYPE_TRINKET:        slot = "trinket";            break;
            case INVTYPE_SHIELD:         slot = "shield";             break;
            case INVTYPE_HOLDABLE:       slot = "off-hand";           break;
            case INVTYPE_2HWEAPON:       slot = "two-handed weapon";  break;
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_THROWN:         slot = "ranged weapon";      break;
            case INVTYPE_RELIC:          slot = "relic";              break;
            default:                     slot = proto->Class == ITEM_CLASS_WEAPON ? "weapon" : "piece of gear"; break;
        }
        const char* quality = proto->Quality >= ITEM_QUALITY_LEGENDARY ? "legendary"
                            : proto->Quality == ITEM_QUALITY_EPIC     ? "epic"
                            : proto->Quality == ITEM_QUALITY_RARE     ? "rare"
                                                                      : "new";
        const bool vowel = quality[0] == 'e';
        return fmt::format("{} {} {}", vowel ? "an" : "a", quality, slot);
    }

    uint32_t ChanceFor(Player* bot)
    {
        auto it = g_EquipPersonalities.find(GetBotPersonality(bot));
        return it == g_EquipPersonalities.end() ? g_EquipChance : it->second;
    }

    bool Cooling(Clock::time_point last, Clock::time_point now, Clock::duration span)
    {
        return last != Clock::time_point{} && now - last < span;
    }

    // Says so, maybe. False to look again in a little while (in a fight).
    bool Consider(const Noted& n, Clock::time_point now)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(n.bot));
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !OllamaIsBotPlayer(bot))
            return true;
        Group* group = bot->GetGroup();
        if (!group || g_DisableForParty || !OllamaGroupHasRealPlayer(bot))
            return true;
        Item* item = bot->GetItemByGuid(ObjectGuid(n.item));
        if (!item || !item->IsEquipped())
            return true;   // straight back off again
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !Bags_GotLately(bot, n.entry, g_EquipLootedMinutes))
            return true;   // nothing new: playerbots' gear, or an old piece swapped back

        if (bot->IsInCombat())
            return now - n.first > kCombatWait;   // after the fight, if it ends soon
        ++g_stats.fresh;

        const uint64_t self = bot->GetGUID().GetRawValue();
        const uint32_t groupId = group->GetGUID().GetCounter();
        if (Cooling(g_botSaid[self], now, std::chrono::minutes(g_EquipBotCooldownMinutes)) ||
            Cooling(g_groupSaid[groupId], now, std::chrono::seconds(g_EquipGroupCooldownSeconds)))
            return true;
        const uint32_t chance = ChanceFor(bot);
        if (!chance || urand(1, 100) > chance)
            return true;   // the kind that just puts it on

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->GetChatHelper() || g_EquipPrompt.empty())
            return true;
        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           groupId);
        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
            return true;

        std::string prompt = SafeFormat(g_EquipPrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality",
                                                 GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("item", "[" + proto->Name1 + "]"),
                                        fmt::arg("what", WhatItIs(proto)));
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
        request.triggerBotReplies = true;   // a "nice" from someone else is fair
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        if (OllamaDispatch_Submit(std::move(request)))
        {
            g_botSaid[self]     = now;
            g_groupSaid[groupId] = now;
            ++g_stats.said;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} put on [{}] and says so in party.", bot->GetName(),
                         proto->Name1);
        }
        return true;
    }
}

void OllamaChatEquipScript::OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/)
{
    // Any thread: note it, nothing more. Not while a character is loading:
    // putting its gear on at login comes through here too.
    if (!g_Enable || !g_EquipEnable || !player || !item || !player->IsInWorld())
        return;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->Quality < g_EquipMinQuality ||
        (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
        return;

    Noted n;
    n.bot   = player->GetGUID().GetRawValue();
    n.item  = item->GetGUID().GetRawValue();
    n.entry = proto->ItemId;
    n.first = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_notedCount;
    if (g_noted.size() < kMaxNoted)
        g_noted.push_back(n);
}

void Equip_Update()
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
        // A moment after, the way a person looks at what they just put on.
        n.at = now + std::chrono::seconds(urand(2, 6));
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
            n.at = now + std::chrono::seconds(5);
            again.push_back(n);
        }
    }
    g_pending.insert(g_pending.end(), again.begin(), again.end());

    // Now and then: cooldowns long run out.
    static Clock::time_point nextSweep{};
    if (now < nextSweep)
        return;
    nextSweep = now + std::chrono::minutes(5);
    const auto botSpan = std::chrono::minutes(g_EquipBotCooldownMinutes);
    for (auto it = g_botSaid.begin(); it != g_botSaid.end();)
        it = (now - it->second > botSpan) ? g_botSaid.erase(it) : std::next(it);
    const auto groupSpan = std::chrono::seconds(g_EquipGroupCooldownSeconds);
    for (auto it = g_groupSaid.begin(); it != g_groupSaid.end();)
        it = (now - it->second > groupSpan) ? g_groupSaid.erase(it) : std::next(it);
}

EquipStats Equip_GetStats()
{
    return g_stats;
}
