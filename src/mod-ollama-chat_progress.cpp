#include "mod-ollama-chat_progress.h"
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
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct QuestSeen
    {
        uint16      kills[QUEST_OBJECTIVES_COUNT]     = {};
        uint16      items[QUEST_ITEM_OBJECTIVES_COUNT] = {};
        QuestStatus status = QUEST_STATUS_NONE;
    };
    struct BotSeen
    {
        std::unordered_map<uint32_t, QuestSeen> quests;
        Clock::time_point lastScan{};
    };
    std::unordered_map<uint64_t, BotSeen> g_seen;

    // Something finished, waiting the moment it takes to say so.
    struct Done
    {
        uint64_t          bot     = 0;
        uint32_t          group   = 0;   // guid counter
        uint32_t          questId = 0;
        int32_t           part    = -1;  // -1: the whole quest; else which objective
        std::string       text;          // what was finished, for the prompt
        Clock::time_point due{};
        Clock::time_point first{};
    };
    std::vector<Done> g_due;

    std::unordered_map<uint32_t, Clock::time_point> g_groupLast;   // group -> its last line about this

    // Group and what -> asked for, by anyone, and whether it went out yet.
    struct SaidMark
    {
        Clock::time_point at{};
        bool              out = false;
    };
    std::unordered_map<uint64_t, SaidMark> g_said;

    // A line asked for and not out after this long is not coming (the model
    // failed, or delivery dropped it): someone else may say it. (A busy model
    // can take a while: the queue, the answer, then the typing delay.)
    constexpr auto kPendingFor = std::chrono::seconds(60);

    ProgressStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint64_t SaidKey(uint32_t group, uint32_t questId, int32_t part)
    {
        return (uint64_t(group) << 32) ^ (uint64_t(questId) * 16 + uint64_t(part + 1));
    }

    // Who a quest is handed in to, for "ready to hand in to Marshal Dughan".
    // The core keeps that the other way round (an NPC's quests), so it is
    // turned round once, on first use.
    std::string EnderName(uint32_t questId)
    {
        static std::unordered_map<uint32_t, uint32_t> enders;
        static bool built = false;
        if (!built)
        {
            built = true;
            if (QuestRelations* map = sObjectMgr->GetCreatureQuestInvolvedRelationMap())
                for (auto const& [creature, quest] : *map)
                    enders.emplace(quest, creature);
        }
        auto it = enders.find(questId);
        if (it == enders.end())
            return {};
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(it->second);
        return ct ? ct->Name : std::string();
    }

    std::string QuestName(Quest const* q)
    {
        return "[" + q->GetTitle() + "]";
    }

    void Note(Player* bot, uint32_t group, Quest const* quest, int32_t part, std::string text)
    {
        const auto now = Clock::now();
        const uint64_t guid = bot->GetGUID().GetRawValue();
        for (Done& d : g_due)
        {
            if (d.bot != guid || d.questId != quest->GetQuestId())
                continue;
            // The same kill finished the quest as well: one line about the quest.
            if (part < 0)
            {
                d.part = -1;
                d.text = std::move(text);
            }
            else if (d.part >= 0 && d.part != part)
                d.text += ", and " + text;
            return;
        }
        Done d;
        d.bot     = guid;
        d.group   = group;
        d.questId = quest->GetQuestId();
        d.part    = part;
        d.text    = std::move(text);
        d.first   = now;
        d.due     = now + std::chrono::seconds(std::max<uint32_t>(1, g_ProgressDelaySeconds));
        g_due.push_back(std::move(d));
    }

    void Scan(Player* bot, uint32_t group, const Clock::time_point now)
    {
        BotSeen& seen = g_seen[bot->GetGUID().GetRawValue()];
        const bool firstLook = seen.lastScan == Clock::time_point{};
        seen.lastScan = now;

        std::unordered_set<uint32_t> inLog;
        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE && status.Status != QUEST_STATUS_COMPLETE)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            inLog.insert(questId);

            QuestSeen cur;
            cur.status = status.Status;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                cur.kills[i] = status.CreatureOrGOCount[i];
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                cur.items[i] = status.ItemCount[i];

            auto it = seen.quests.find(questId);
            if (it == seen.quests.end() || firstLook)
            {
                seen.quests[questId] = cur;   // new to us: what it has is where it starts
                continue;
            }
            const QuestSeen prev = it->second;
            it->second = cur;

            if (prev.status == QUEST_STATUS_INCOMPLETE && cur.status == QUEST_STATUS_COMPLETE)
            {
                ++g_stats.quests;
                const std::string ender = EnderName(questId);
                Note(bot, group, quest, -1,
                     SafeFormat(g_ProgressQuestText, fmt::arg("quest", QuestName(quest)),
                                fmt::arg("ender", ender.empty() ? std::string("whoever gave it out") : ender)));
                continue;
            }
            if (cur.status != QUEST_STATUS_INCOMPLETE)
                continue;

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                const uint32 need = quest->RequiredNpcOrGoCount[i];
                const int32 id = quest->RequiredNpcOrGo[i];
                if (!need || !id || prev.kills[i] >= need || cur.kills[i] < need)
                    continue;
                std::string what;
                if (id > 0)
                {
                    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(uint32(id)))
                        what = ct->Name;
                }
                else if (GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(uint32(-id)))
                    what = go->name;
                if (what.empty())
                    continue;
                ++g_stats.objectives;
                Note(bot, group, quest, i,
                     SafeFormat(id > 0 ? g_ProgressKillText : g_ProgressUseText, fmt::arg("count", need),
                                fmt::arg("what", what), fmt::arg("quest", QuestName(quest))));
            }
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                const uint32 need = quest->RequiredItemCount[i];
                if (!need || !quest->RequiredItemId[i] || prev.items[i] >= need || cur.items[i] < need)
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]);
                if (!proto)
                    continue;
                ++g_stats.objectives;
                Note(bot, group, quest, QUEST_OBJECTIVES_COUNT + i,
                     SafeFormat(g_ProgressItemText, fmt::arg("count", need),
                                fmt::arg("what", "[" + proto->Name1 + "]"), fmt::arg("quest", QuestName(quest))));
            }
        }
        for (auto it = seen.quests.begin(); it != seen.quests.end();)
            it = inLog.count(it->first) ? std::next(it) : seen.quests.erase(it);
    }

    bool Say(Player* bot, const Done& d, uint64_t key)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        Group* group = bot->GetGroup();
        if (!ai || !ai->GetChatHelper() || !group || g_DisableForParty || !OllamaGroupHasRealPlayer(bot) ||
            g_ProgressPrompt.empty() || d.text.empty() || d.text == "[Format Error]")
            return false;

        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        const std::string scopeKey = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                           group->GetGUID().GetCounter());
        if (!Governor_CanSend(bot->GetGUID(), scopeKey, true))
            return false;

        const std::string personality = GetBotPersonality(bot);
        std::string prompt = SafeFormat(TextRules::ExpandChoices(g_ProgressPrompt, Rng()),
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality", GetPersonalityPromptAddition(personality)),
                                        fmt::arg("done", d.text));
        if (prompt.empty() || prompt == "[Format Error]")
            return false;
        prompt += Roleplay_BuildVoicePrompt(bot);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.source            = source;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.directAddress     = true;    // paced here, by the group's gap
        request.triggerBotReplies = false;   // the rest of the group need not answer it
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        request.progressKey       = key;     // out: nobody else need say it
        return OllamaDispatch_Submit(std::move(request));
    }
}

void Progress_NoteDelivered(uint64_t key)
{
    if (!key)
        return;
    SaidMark& mark = g_said[key];
    mark.at  = Clock::now();
    mark.out = true;
}

void Progress_Update(uint32_t diff)
{
    if (!g_ProgressEnable)
        return;

    static uint32_t timer = 0;
    timer += diff;
    const auto now = Clock::now();

    if (timer >= std::max<uint32_t>(1, g_ProgressCheckSeconds) * 1000)
    {
        timer = 0;
        std::unordered_set<uint32_t> looked;
        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            Player* player = entry.second;
            if (!player || !player->IsInWorld() || OllamaIsBotPlayer(player))
                continue;
            Group* group = player->GetGroup();
            if (!group || group->isBGGroup() || group->isBFGroup())
                continue;
            const uint32_t key = group->GetGUID().GetCounter();
            if (!looked.insert(key).second)
                continue;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsInWorld() && OllamaIsBotPlayer(member))
                    Scan(member, key, now);
            }
        }

        // Bots nobody has looked at for a while start afresh next time.
        for (auto it = g_seen.begin(); it != g_seen.end();)
            it = (now - it->second.lastScan > std::chrono::minutes(10)) ? g_seen.erase(it) : std::next(it);
    }

    if (g_due.empty())
        return;

    const auto gap = std::chrono::seconds(g_ProgressGapSeconds);
    std::vector<Done> later;
    for (Done& d : g_due)
    {
        if (now < d.due)
        {
            later.push_back(std::move(d));
            continue;
        }
        const uint64_t key = SaidKey(d.group, d.questId, d.part);
        auto said = g_said.find(key);
        if (said != g_said.end() && said->second.out && now - said->second.at < std::chrono::minutes(2))
        {
            ++g_stats.shared;   // someone in the group said it already
            continue;
        }
        if (said != g_said.end() && !said->second.out && now - said->second.at < kPendingFor)
        {
            // Someone is saying it: wait and see that it goes out.
            if (now - d.first > std::chrono::seconds(120))
                ++g_stats.lapsed;
            else
            {
                d.due = now + std::chrono::seconds(5);
                later.push_back(std::move(d));
            }
            continue;
        }
        auto last = g_groupLast.find(d.group);
        if (last != g_groupLast.end() && now - last->second < gap)
        {
            if (now - d.first > std::chrono::seconds(60))
                ++g_stats.lapsed;   // its moment has passed
            else
            {
                d.due = last->second + gap;
                later.push_back(std::move(d));
            }
            continue;
        }

        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(d.bot));
        Group* group = bot && bot->IsInWorld() ? bot->GetGroup() : nullptr;
        if (!group || group->GetGUID().GetCounter() != d.group || !bot->IsAlive() || !Say(bot, d, key))
        {
            ++g_stats.lapsed;
            continue;
        }
        ++g_stats.said;
        g_groupLast[d.group] = now;
        g_said[key] = SaidMark{ now, false };
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} tells the group it finished something: {}", bot->GetName(),
                     d.text);
    }
    g_due.swap(later);

    // The "said" marks and gaps do not need to outlive a few minutes.
    if (g_said.size() > 256 || g_groupLast.size() > 256)
    {
        for (auto it = g_said.begin(); it != g_said.end();)
            it = (now - it->second.at > std::chrono::minutes(2)) ? g_said.erase(it) : std::next(it);
        for (auto it = g_groupLast.begin(); it != g_groupLast.end();)
            it = (now - it->second > gap) ? g_groupLast.erase(it) : std::next(it);
    }
}

ProgressStats Progress_GetStats()
{
    return g_stats;
}
