#include "mod-ollama-chat_rolls.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Group.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // What one roll packet said.
    struct Seen
    {
        bool     won    = false;   // SMSG_LOOT_ROLL_WON: the winner
        uint32_t slot   = 0;       // the loot slot rolled for
        uint32_t item   = 0;
        uint64_t roller = 0;       // who rolled, or who won
        uint8_t  number = 0;       // 1 to 100
        uint8_t  type   = 0;       // RollType: need, greed, disenchant
    };
    std::mutex        g_queueMutex;
    std::vector<Seen> g_queue;

    struct Rolled
    {
        uint64_t roller = 0;
        uint8_t  number = 0;
        uint8_t  type   = 0;
    };

    // Rolls for one item so far. The game sends a group every roll and then
    // the winner, one after the other; each member gets its own copy.
    struct Open
    {
        std::vector<Rolled> rolls;
        Clock::time_point   at;
    };
    std::map<std::pair<uint32_t, uint32_t>, Open> g_open;              // (item, slot) -> rolls
    std::map<std::tuple<uint32_t, uint32_t, uint64_t>, Clock::time_point> g_decided;   // (item, slot, winner)
    std::unordered_map<uint64_t, Clock::time_point> g_lastReaction;   // group -> when

    RollsStats g_stats;

    const char* TypeWord(uint8_t type)
    {
        switch (type)
        {
            case ROLL_NEED:       return "need";
            case ROLL_GREED:      return "greed";
            case ROLL_DISENCHANT: return "disenchant";
            default:              return "roll";
        }
    }

    bool Chance(uint32_t percent)
    {
        return percent >= 100 || (percent > 0 && urand(1, 100) <= percent);
    }

    std::string Fill(const std::string& templ, Player* bot, const std::string& who, uint32_t roll,
                     const std::string& item, const std::string& outcome)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (templ.empty() || !ai || !ai->GetChatHelper())
            return {};
        const std::string out = SafeFormat(templ,
                                           fmt::arg("bot_name", bot->GetName()),
                                           fmt::arg("bot_level", bot->GetLevel()),
                                           fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                           fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                           fmt::arg("bot_personality",
                                                    GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                           fmt::arg("player_name", who),
                                           fmt::arg("roll", roll),
                                           fmt::arg("item", item),
                                           fmt::arg("outcome", outcome));
        if (out.empty() || out == "[Format Error]")
            return {};
        return out + Roleplay_BuildVoicePrompt(bot);
    }

    // A real player's roll about as low or as high as it gets: one bot in the
    // group says something about it, now and then.
    void React(Group* group, Player* roller, const Rolled& r, const std::string& item, Player* winner,
               const Rolled* winning)
    {
        const bool low = r.number <= g_RollsLowRoll;
        const bool high = r.number >= g_RollsHighRoll;
        if ((!low && !high) || g_DisableForParty || !Chance(g_RollsReactChance))
            return;
        const uint64_t groupKey = group->GetGUID().GetRawValue();
        const auto now = Clock::now();
        auto last = g_lastReaction.find(groupKey);
        if (last != g_lastReaction.end() &&
            now - last->second < std::chrono::seconds(g_RollsReactCooldownSeconds))
            return;

        std::vector<Player*> bots;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* m = ref->GetSource())
                if (m != roller && m->IsInWorld() && OllamaIsBotPlayer(m) && !m->isAFK())
                    bots.push_back(m);
        if (bots.empty())
            return;
        Player* bot = bots[urand(0, uint32(bots.size() - 1))];

        std::string outcome;
        if (winner == roller)
            outcome = ", and won it";
        else if (winner)
            outcome = ", and " + (winner == bot ? std::string("you") : winner->GetName()) + " won it" +
                      (winning && winning->type == ROLL_NEED && r.type != ROLL_NEED ? " on a need roll" : "");
        const std::string prompt = Fill(low ? g_RollsLowPrompt : g_RollsHighPrompt, bot, roller->GetName(), r.number,
                                        item, outcome);
        if (prompt.empty())
            return;

        const ChatChannelSourceLocal source = group->isRaidGroup() ? SRC_RAID_LOCAL : SRC_PARTY_LOCAL;
        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = roller->GetGUID().GetRawValue();
        request.source            = source;
        request.scopeKey          = Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                                          group->GetGUID().GetCounter());
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.directAddress     = true;   // the moment calls for it
        request.triggerBotReplies = false;
        request.extraDelayMs      = urand(1200, 3500);   // the moment it takes to read the rolls
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        request.prompt            = prompt;
        if (OllamaDispatch_Submit(std::move(request)))
        {
            g_lastReaction[groupKey] = now;
            ++g_stats.reactions;
        }
    }

    // The winner is known: the rolls go into the group's transcript.
    void Decide(const Seen& won)
    {
        const auto now = Clock::now();
        const auto key = std::make_tuple(won.item, won.slot, won.roller);
        auto done = g_decided.find(key);
        if (done != g_decided.end() && now - done->second < std::chrono::seconds(10))
            return;   // another member's copy of the same news
        g_decided[key] = now;

        Open open;
        auto it = g_open.find({ won.item, won.slot });
        if (it != g_open.end())
        {
            open = std::move(it->second);
            g_open.erase(it);
        }
        Player* winner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(won.roller));
        if (!winner || !winner->IsInWorld())
            return;
        Group* group = winner->GetGroup();
        if (!group || !OllamaGroupHasRealPlayer(winner))
            return;   // a group of bots: nobody to follow along
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(won.item);
        const std::string item = proto ? proto->Name1 : std::string("an item");

        // Highest first, the winner's own roll among them.
        if (std::none_of(open.rolls.begin(), open.rolls.end(), [&](const Rolled& r) { return r.roller == won.roller; }))
            open.rolls.push_back({ won.roller, won.number, won.type });
        std::stable_sort(open.rolls.begin(), open.rolls.end(),
                         [](const Rolled& a, const Rolled& b) { return a.number > b.number; });

        std::string rolls;
        std::vector<std::pair<Player*, Rolled>> people;
        for (const Rolled& r : open.rolls)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(r.roller));
            if (!p)
                continue;
            rolls += (rolls.empty() ? "" : ", ") + p->GetName() + " " + std::to_string(r.number) + " (" +
                     TypeWord(r.type) + ")";
            people.emplace_back(p, r);
        }
        if (rolls.empty())
            return;

        const std::string note = SafeFormat(g_RollsNoteText, fmt::arg("item", item), fmt::arg("rolls", rolls),
                                            fmt::arg("winner", winner->GetName()));
        if (!note.empty() && note != "[Format Error]")
        {
            Context_RecordEvent(winner, note, false);
            ++g_stats.noted;
        }

        // A real player's 1 or 99.
        Rolled const* winning = nullptr;
        for (const auto& [p, r] : people)
            if (p == winner)
                winning = &r;
        for (const auto& [p, r] : people)
            if (OllamaIsRealPlayer(p) && (r.number <= g_RollsLowRoll || r.number >= g_RollsHighRoll))
            {
                React(group, p, r, item, winner, winning);
                break;
            }
    }
}

bool OllamaChatRollsScript::CanPacketSend(WorldSession* session, WorldPacket const& packet)
{
    // Every packet to every client comes through here, from any thread: say
    // yes to all of them, and look no further than the opcode.
    const uint16 op = packet.GetOpcode();
    if ((op != SMSG_LOOT_ROLL && op != SMSG_LOOT_ROLL_WON) || !g_RollsEnable || !session)
        return true;

    Seen s;
    if (op == SMSG_LOOT_ROLL)
    {
        // loot u64, slot u32, roller u64, item u32, suffix u32, property u32,
        // number u8, type u8, auto-pass u8 (Group::SendLootRoll).
        if (packet.size() < 34)
            return true;
        s.slot   = packet.read<uint32>(8);
        s.roller = packet.read<uint64>(12);
        s.item   = packet.read<uint32>(20);
        s.number = packet.read<uint8>(32);
        s.type   = packet.read<uint8>(33);
        if (s.number < 1 || s.number > 100)
            return true;   // a choice made ("has selected greed"), or a pass: not a roll yet
    }
    else
    {
        // loot u64, slot u32, item u32, suffix u32, property u32, winner u64,
        // number u8, type u8 (Group::SendLootRollWon).
        if (packet.size() < 34)
            return true;
        s.won    = true;
        s.slot   = packet.read<uint32>(8);
        s.item   = packet.read<uint32>(12);
        s.roller = packet.read<uint64>(24);
        s.number = packet.read<uint8>(32);
        s.type   = packet.read<uint8>(33);
    }
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.size() < 1024)
        g_queue.push_back(s);
    return true;
}

void Rolls_Update()
{
    std::vector<Seen> seen;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        seen.swap(g_queue);
    }
    const auto now = Clock::now();
    for (const Seen& s : seen)
    {
        if (!s.won)
        {
            Open& open = g_open[{ s.item, s.slot }];
            if (open.rolls.empty() || now - open.at > std::chrono::seconds(10))
                open.rolls.clear();   // left over from an earlier roll for the same thing
            open.at = now;
            if (std::none_of(open.rolls.begin(), open.rolls.end(),
                             [&](const Rolled& r) { return r.roller == s.roller; }))
                open.rolls.push_back({ s.roller, s.number, s.type });
            continue;
        }
        Decide(s);
    }

    // Forget what is old: rolls nobody won (all passed), news already given.
    for (auto it = g_open.begin(); it != g_open.end();)
        it = now - it->second.at > std::chrono::seconds(30) ? g_open.erase(it) : std::next(it);
    for (auto it = g_decided.begin(); it != g_decided.end();)
        it = now - it->second > std::chrono::seconds(30) ? g_decided.erase(it) : std::next(it);
    if (g_lastReaction.size() > 256)
        for (auto it = g_lastReaction.begin(); it != g_lastReaction.end();)
            it = now - it->second > std::chrono::seconds(g_RollsReactCooldownSeconds) ? g_lastReaction.erase(it)
                                                                                      : std::next(it);
}

RollsStats Rolls_GetStats()
{
    return g_stats;
}
