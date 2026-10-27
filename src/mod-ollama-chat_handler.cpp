#include "Log.h"
#include "Language.h"
#include "Player.h"
#include "Chat.h"
#include "Channel.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Config.h"
#include "Common.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "World.h"
#include "AiFactory.h"
#include "ChannelMgr.h"
#include <sstream>
#include <vector>
#include <list>
#include "Containers.h"
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <algorithm>
#include <random>
#include <cctype>
#include <chrono>
#include <ctime>
#include "DatabaseEnv.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_rag.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_conversation.h"
#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_orders.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_presence.h"
#include <iomanip>
#include <unordered_set>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "Group.h"
#include "Creature.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

// For AzerothCore range checks
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Map.h"
#include "GridNotifiers.h"

// Forward declarations for internal helper functions.
static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player,
                                             ChatChannelSourceLocal source, Channel* channel = nullptr, Player* receiver = nullptr);

// Crowd replies (defined further down, next to Crowd_FireSlot).
static void PlanCrowdForHumanLine(Player* player, const std::string& msg,
                                  ChatChannelSourceLocal source, Channel* channel,
                                  const std::string& scopeKey, uint64_t seq,
                                  const ContextAddressee& addressee,
                                  const std::vector<Player*>& candidates);
static void HandleCrowdBotLine(Player* bot, const std::string& msg,
                               ChatChannelSourceLocal source, Channel* channel,
                               const std::string& scopeKey, uint64_t seq,
                               const ContextAddressee& addressee,
                               const std::vector<Player*>& candidates,
                               uint8_t chainDepth, uint64_t crowdPlanId);

// Does the line thank someone ("ty", "cheers", "thank you")?
static bool OllamaHasThanksWord(const std::string& msg)
{
    return !g_CrowdThanksWords.empty() &&
           CrowdText::ContainsPhrase(CrowdText::Words(Context_CleanText(msg)), g_CrowdThanksWords);
}

// Is it nothing but thanks ("ty", "cheers mate", "thanks Zonin")? `names`
// are left out, so naming who is thanked does not change that.
static bool OllamaIsThanksOnly(const std::string& msg, const std::vector<std::string>& names)
{
    return OllamaHasThanksWord(msg) &&
           CrowdText::Classify(Context_CleanText(msg), g_CrowdAckWords, g_CrowdOpenCallPhrases, names).ack;
}

// Position of botName inside msg as a whole word, or npos. Case-insensitive.
// Hoisted out of ProcessChat because being named is also what makes a message
// direct address, which the governor needs to know.
size_t OllamaFindBotNameMention(const std::string& msg, const std::string& botName)
{
    if (botName.empty() || msg.size() < botName.size())
        return std::string::npos;

    auto lower = [](const std::string& in)
    {
        std::string out = in;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };

    const std::string lowerMsg     = lower(msg);
    const std::string lowerBotName = lower(botName);

    size_t pos = 0;
    while ((pos = lowerMsg.find(lowerBotName, pos)) != std::string::npos)
    {
        const bool validStart = (pos == 0 || !std::isalnum(static_cast<unsigned char>(lowerMsg[pos - 1])));
        const size_t endPos   = pos + lowerBotName.length();
        const bool validEnd   = (endPos >= lowerMsg.length() || !std::isalnum(static_cast<unsigned char>(lowerMsg[endPos])));

        if (validStart && validEnd)
            return pos;
        ++pos;
    }
    return std::string::npos;
}

// Is this line aimed at this bot in particular, rather than said to the room?
//
// Direct address skips the pacing cooldowns in the governor. Those exist to
// stop ambient chatter running hot; applied to a conversation they make a bot
// answer once and then ignore the next several things said to it, which reads
// as broken rather than as rate limiting.
//
// Four things count:
//   * a whisper -- the bot is the only recipient
//   * being named -- "Uynya, what do you think?"
//   * an open conversation -- this bot already answered this person here, so
//     the next thing they say is a turn in that exchange. Without this a bot
//     answers the opening line and then goes quiet unless it is named again
//     every single message, which is the same bug one step later.
//   * party/raid chat from a person, in a small group the bot belongs to --
//     a five-person party is a conversation, not a crowd. Above
//     DirectAddressGroupSize members it is a crowd again and pacing returns.
static bool OllamaIsDirectAddress(Player* bot, Player* speaker, ChatChannelSourceLocal source,
                                  const std::string& msg, bool senderIsBot,
                                  const std::string& scopeKey)
{
    if (source == SRC_WHISPER_LOCAL)
        return true;

    if (!bot)
        return false;

    if (OllamaFindBotNameMention(msg, bot->GetName()) != std::string::npos)
        return true;

    if (senderIsBot)
        return false;   // only a person's turn obliges an answer

    // Already talking to this person here.
    if (speaker && Governor_InConversation(bot->GetGUID(), speaker->GetGUID(), scopeKey))
        return true;

    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL)
        return false;

    if (g_DirectAddressGroupSize == 0)
        return false;

    Group* group = bot->GetGroup();
    if (!group || !speaker || speaker->GetGroup() != group)
        return false;

    return group->GetMembersCount() <= g_DirectAddressGroupSize;
}

namespace
{
    // Unlike Acore::AnyUnitInObjectRangeCheck this keeps dead creatures --
    // a fresh corpse is worth commenting on.
    struct OllamaNearbyCreatureCheck
    {
        OllamaNearbyCreatureCheck(WorldObject const* obj, float range)
            : _obj(obj), _range(range) { }

        bool operator()(Creature* c) const
        {
            return c && _obj->IsWithinDistInMap(c, _range);
        }

        WorldObject const* _obj;
        float              _range;
    };
}

// Helper function to format class name for any player
static std::string FormatPlayerClass(uint8_t classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:      return "Warrior";
        case CLASS_PALADIN:      return "Paladin";
        case CLASS_HUNTER:       return "Hunter";
        case CLASS_ROGUE:        return "Rogue";
        case CLASS_PRIEST:       return "Priest";
        case CLASS_DEATH_KNIGHT: return "Death Knight";
        case CLASS_SHAMAN:       return "Shaman";
        case CLASS_MAGE:         return "Mage";
        case CLASS_WARLOCK:      return "Warlock";
        case CLASS_DRUID:        return "Druid";
        default:                 return "Unknown";
    }
}

// Helper function to format race name for any player
static std::string FormatPlayerRace(uint8_t raceId)
{
    switch (raceId)
    {
        case RACE_HUMAN:         return "Human";
        case RACE_ORC:           return "Orc";
        case RACE_DWARF:         return "Dwarf";
        case RACE_NIGHTELF:      return "Night Elf";
        case RACE_UNDEAD_PLAYER: return "Undead";
        case RACE_TAUREN:        return "Tauren";
        case RACE_GNOME:         return "Gnome";
        case RACE_TROLL:         return "Troll";
        case RACE_BLOODELF:      return "Blood Elf";
        case RACE_DRAENEI:       return "Draenei";
        default:                 return "Unknown";
    }
}

const char* ChatChannelSourceLocalStr[] =
{
    "Undefined",  // 0
    "Say",        // 1
    "Party",      // 2
    "Raid",       // 3
    "Guild",      // 4
    "Officer",    // 5
    "Yell",       // 6
    "Whisper",    // 7
    "Unknown8",   // 8
    "Unknown9",   // 9
    "Unknown10",  // 10
    "Unknown11",  // 11
    "Unknown12",  // 12
    "Unknown13",  // 13
    "Unknown14",  // 14
    "Unknown15",  // 15
    "Unknown16",  // 16
    "General"     // 17
};

std::string GetConversationEntryKey(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    // Use a combination that guarantees uniqueness
    return SafeFormat("{}:{}:{}:{}", botGuid, playerGuid, playerMessage, botReply);
}

std::string rtrim(const std::string& s)
{
    const std::string whitespace = " \t\n\r,.!?;:";
    size_t end = s.find_last_not_of(whitespace);
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

ChatChannelSourceLocal GetChannelSourceLocal(uint32_t type)
{
    switch (type)
    {
        case CHAT_MSG_SAY:
            return SRC_SAY_LOCAL;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            return SRC_PARTY_LOCAL;
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_RAID_WARNING:
            return SRC_RAID_LOCAL;
        case CHAT_MSG_GUILD:
            return SRC_GUILD_LOCAL;
        case CHAT_MSG_OFFICER:
            return SRC_OFFICER_LOCAL;
        case CHAT_MSG_YELL:
            return SRC_YELL_LOCAL;
        case CHAT_MSG_WHISPER:
        case CHAT_MSG_WHISPER_FOREIGN:
        case CHAT_MSG_WHISPER_INFORM:
            return SRC_WHISPER_LOCAL;
        case CHAT_MSG_CHANNEL:
            return SRC_GENERAL_LOCAL;
        default:
            return SRC_UNDEFINED_LOCAL;
    }
}

Channel* GetValidChannel(uint32_t teamId, const std::string& channelName, Player* player)
{
    ChannelMgr* cMgr = ChannelMgr::forTeam(static_cast<TeamId>(teamId));
    Channel* channel = cMgr->GetChannel(channelName, player);
    if (!channel)
    {
        if(g_DebugEnabled)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] Channel '{}' not found for team {}", channelName, teamId);
        }
    }
    return channel;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    if (!g_Enable)
        return true;

    // Player::Say and Player::Yell run this hook for bots too, so every /say
    // line a bot delivered was processed twice: once here at chain depth 0,
    // as if a person had just spoken, and once by the dispatcher at the real
    // depth. The depth-0 pass escaped the chain limit and ignored
    // triggerBotReplies = false. The dispatcher already hands bot lines to
    // ProcessBotChatMessage, so bots are skipped here.
    if (OllamaIsBotPlayer(player))
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* /*group*/)
{
    if (!g_Enable)
        return true;

    // Same reason as above: bot lines reach ProcessChat through the
    // dispatcher, at their real chain depth and with their crowd attached.
    if (OllamaIsBotPlayer(player))
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* /*guild*/)
{
    if (!g_Enable)
        return true;

    if (OllamaIsBotPlayer(player))
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    if (!g_Enable)
        return true;

    if (OllamaIsBotPlayer(player))
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, channel, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver)
{
    // Only process if our module is enabled
    if (!g_Enable)
        return true;

    if (type == CHAT_MSG_WHISPER)
    {
        // Check if this is a valid whisper to a bot
        if (!receiver || !player || player == receiver)
            return true;

        // Check if sender is a bot - if so, don't trigger Ollama responses for bot-to-bot whispers
        PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        if (senderAI && senderAI->IsBotAI())
        {
            return true;
        }

        PlayerbotAI* receiverAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (!receiverAI || !receiverAI->IsBotAI())
            return true;
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] OnPlayerCanUseChat called: player={}, type={}, receiver={}",
            player->GetName(), type, receiver ? receiver->GetName() : "null");
    }

    // Process the chat immediately in OnPlayerCanUseChat to prevent double processing
    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, receiver);

    // Return false to prevent the message from being processed again in OnPlayerChat
    return true;
}

void AppendBotConversation(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
    auto& playerHistory = g_BotConversationHistory[botGuid][playerGuid];
    playerHistory.push_back({ playerMessage, botReply, /*persisted*/ false });
    while (playerHistory.size() > g_MaxConversationHistory)
    {
        playerHistory.pop_front();
    }

}

namespace
{
    // A multi-row INSERT is one statement for the database worker instead of
    // one per turn, but it still has to fit inside max_allowed_packet. Flush
    // well short of the 4MB older servers default to.
    constexpr size_t kHistoryInsertMaxBytes = 512 * 1024;
}

void SaveBotConversationHistoryToDB()
{
    // Gathered under the lock, written outside it. Escaping and statement
    // building have no business holding a mutex that reply delivery takes on
    // the world thread.
    struct PendingPair
    {
        uint64_t                                         botGuid;
        uint64_t                                         playerGuid;
        std::vector<std::pair<std::string, std::string>> turns;
    };
    std::vector<PendingPair> pending;

    {
        std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

        for (auto& [botGuid, playerMap] : g_BotConversationHistory)
        {
            for (auto& [playerGuid, history] : playerMap)
            {
                PendingPair entry{ botGuid, playerGuid, {} };

                for (BotConversationEntry& turn : history)
                {
                    // The whole point of the flag: this used to re-INSERT
                    // IGNORE every cached turn of every pair on every save,
                    // and let the unique key throw the duplicates away after
                    // MySQL had already done the index probe for each one.
                    if (turn.persisted)
                        continue;

                    entry.turns.emplace_back(turn.playerMessage, turn.botReply);

                    // Marked before the write lands. A dropped row costs one
                    // line of remembered chatter; retrying every turn forever
                    // is the behaviour being removed here.
                    turn.persisted = true;
                }

                if (!entry.turns.empty())
                    pending.push_back(std::move(entry));
            }
        }
    }

    if (pending.empty())
        return;

    // A configured 0 would run the trim below with OFFSET -1.
    const uint32_t keep   = std::max<uint32_t>(g_MaxConversationHistory, 1);
    const uint32_t offset = keep - 1;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32_t savedTurns = 0;

    for (const PendingPair& entry : pending)
    {
        std::string values;

        auto flushValues = [&]()
        {
            if (values.empty())
                return;

            trans->Append("INSERT IGNORE INTO mod_ollama_chat_history "
                          "(bot_guid, player_guid, timestamp, player_message, bot_reply) VALUES " + values);
            values.clear();
        };

        for (const auto& [playerMessage, botReply] : entry.turns)
        {
            std::string escPlayerMsg = playerMessage;
            CharacterDatabase.EscapeString(escPlayerMsg);

            std::string escBotReply = botReply;
            CharacterDatabase.EscapeString(escBotReply);

            if (!values.empty())
                values += ',';

            values += SafeFormat("({}, {}, NOW(), '{}', '{}')",
                                 entry.botGuid, entry.playerGuid, escPlayerMsg, escBotReply);
            ++savedTurns;

            if (values.size() >= kHistoryInsertMaxBytes)
                flushValues();
        }

        flushValues();

        // Trim this pair to its newest `keep` rows.
        //
        // This replaces a ROW_NUMBER() window function evaluated over the
        // whole table on every save, whose DELETE then matched rows by
        // (bot_guid, player_guid, timestamp). That tuple has no index, and
        // every row written in one save batch shares the same
        // second-granularity NOW() -- so the delete matched the entire batch,
        // not just the surplus, and quietly wiped whole conversations.
        //
        // Ordering by the auto-increment id is exact and is served end to end
        // by idx_pair_recent (bot_guid, player_guid, id). Only pairs that
        // gained a row are trimmed; the rest of the table is never touched.
        trans->Append(SafeFormat(
            "DELETE FROM mod_ollama_chat_history "
            "WHERE bot_guid = {0} AND player_guid = {1} AND id < ("
                "SELECT keep_id FROM ("
                    "SELECT id AS keep_id FROM mod_ollama_chat_history "
                    "WHERE bot_guid = {0} AND player_guid = {1} "
                    "ORDER BY id DESC LIMIT 1 OFFSET {2}"
                ") AS oldest_kept"
            ")",
            entry.botGuid, entry.playerGuid, offset));
    }

    CharacterDatabase.CommitTransaction(trans);

    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Saved {} new conversation turn(s) across {} bot/player pair(s).",
                 savedTurns, static_cast<uint32_t>(pending.size()));
    }
}

// Drop a bot's persisted history. Called from a worker thread after its
// conversation has been condensed into long-term memories -- without this the
// rows survive, get reloaded on the next startup, and are condensed again.
//
// Worker-safe: Execute() queues, and the statement is a literal plus an
// integer, so no config string is read off the world thread.
void DeleteBotConversationHistoryFromDB(uint64_t botGuid)
{
    CharacterDatabase.Execute(SafeFormat(
        "DELETE FROM mod_ollama_chat_history WHERE bot_guid = {}", botGuid));
}

// Called when a bot sends a message (random chatter or other bot-initiated messages)
// This triggers other bots to potentially reply
void ProcessBotChatMessage(Player* bot, const std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel, uint8_t chainDepth,
                           uint64_t crowdPlanId)
{
    if (!bot || msg.empty())
        return;

    // Bail before the (not cheap) eligibility validation below when the chain
    // is already spent.
    if (!Governor_ChainDepthAllowed(chainDepth))
        return;

        
    // If channel is nullptr but this is a channel-type message, try to find the channel
    if (!channel && sourceLocal == SRC_GENERAL_LOCAL)
    {
        // Look up the General channel for this bot's faction
        std::string channelName = "General";
        ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (cMgr)
        {
            channel = cMgr->GetChannel(channelName, bot);
            if (g_DebugEnabled)
            {
                if (channel)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] ProcessBotChatMessage: Found General channel for bot {}", bot->GetName());
                else
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] ProcessBotChatMessage: Could not find General channel for bot {}", bot->GetName());
            }
        }
    }
    
    // Whether other bots may chain a reply to this line. The line itself has
    // already been delivered by this point -- this only gates propagation.
    bool canSendMessage = false;
    switch (sourceLocal)
    {
        case SRC_SAY_LOCAL:
        case SRC_YELL_LOCAL:
            // Distance checks will be applied during eligibility filtering
            canSendMessage = true;
            break;
            
        case SRC_GENERAL_LOCAL:
            // Must have a channel object
            canSendMessage = (channel != nullptr);
            if (!canSendMessage && g_DebugEnabled)
                LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in General - no channel found", bot->GetName());
            break;
            
        case SRC_GUILD_LOCAL:
        case SRC_OFFICER_LOCAL:
            // Must be in a guild with at least one real player online
            if (bot->GetGuildId() != 0)
            {
                Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
                if (guild)
                {
                    OllamaWorldSnapshot world;
                    world.Build();
                    const bool hasRealPlayer = world.GuildHasRealPlayer(bot->GetGuildId());
                    canSendMessage = hasRealPlayer;
                    if (!canSendMessage && g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] No bot replies to {} in Guild - no real players online in guild", bot->GetName());
                }
                else
                {
                    canSendMessage = false;
                    if (g_DebugEnabled)
                        LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in Guild - guild not found", bot->GetName());
                }
            }
            else
            {
                canSendMessage = false;
                if (g_DebugEnabled)
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in Guild - not in a guild", bot->GetName());
            }
            break;
            
        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:
            // Must be in a group with at least one real player
            if (bot->GetGroup())
            {
                Group* group = bot->GetGroup();
                bool hasRealPlayer = false;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (OllamaIsRealPlayer(member))
                    {
                        hasRealPlayer = true;
                        break;
                    }
                }
                canSendMessage = hasRealPlayer;
                if (!canSendMessage && g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] No bot replies to {} in Party - no real players in group", bot->GetName());
            }
            else
            {
                canSendMessage = false;
                if (g_DebugEnabled)
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in Party - not in a group", bot->GetName());
            }
            break;
            
        case SRC_WHISPER_LOCAL:
            // Whispers are handled separately
            canSendMessage = true;
            break;
            
        default:
            canSendMessage = true;
            break;
    }
    
    if (!canSendMessage)
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] Not propagating {}'s {} line to other bots - no audience.",
                     bot->GetName(), ChatChannelSourceLocalStr[sourceLocal]);
        return;
    }
        
    // Convert ChatChannelSourceLocal back to chat type for ProcessChat
    uint32_t type = 0;
    switch (sourceLocal)
    {
        case SRC_SAY_LOCAL: type = CHAT_MSG_SAY; break;
        case SRC_YELL_LOCAL: type = CHAT_MSG_YELL; break;
        case SRC_PARTY_LOCAL: type = CHAT_MSG_PARTY; break;
        case SRC_RAID_LOCAL: type = CHAT_MSG_RAID; break;
        case SRC_GUILD_LOCAL: type = CHAT_MSG_GUILD; break;
        case SRC_OFFICER_LOCAL: type = CHAT_MSG_OFFICER; break;
        case SRC_WHISPER_LOCAL: type = CHAT_MSG_WHISPER; break;
        case SRC_GENERAL_LOCAL: type = CHAT_MSG_CHANNEL; break;
        default: type = CHAT_MSG_SAY; break;
    }
    
    std::string mutableMsg = msg; // ProcessChat takes non-const reference
    uint32_t lang = bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
    
    // Call the main ProcessChat function with bot as sender
    PlayerBotChatHandler::ProcessChat(bot, type, lang, mutableMsg, sourceLocal, channel, nullptr, chainDepth,
                                      crowdPlanId);
}

std::string GetBotHistoryPrompt(uint64_t botGuid, uint64_t playerGuid, std::string playerMessage)
{
    if(!g_EnableChatHistory)
    {
        return "";
    }
    
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

    std::string result;
    const auto botIt = g_BotConversationHistory.find(botGuid);
    if (botIt == g_BotConversationHistory.end())
        return result;
    const auto playerIt = botIt->second.find(playerGuid);
    if (playerIt == botIt->second.end())
        return result;

    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
    std::string playerName = player ? player->GetName() : "The player";

    result += SafeFormat(g_ChatHistoryHeaderTemplate, fmt::arg("player_name", playerName));

    for (const auto& entry : playerIt->second) {
        result += SafeFormat(g_ChatHistoryLineTemplate,
            fmt::arg("player_name", playerName),
            fmt::arg("player_message", entry.playerMessage),
            fmt::arg("bot_reply", entry.botReply)
        );
    }

    result += SafeFormat(g_ChatHistoryFooterTemplate,
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage)
    );

    return result;
}

// --- Helper: Spells ---
std::string ChatHandler_GetBotSpellInfo(Player* bot)
{
    // Map to store highest rank of each spell: spell name -> (spellId, rank, costText)
    std::map<std::string, std::tuple<uint32, uint32, std::string>> uniqueSpells;
    
    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;
        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;
        if (bot->HasSpellCooldown(spellId))
            continue;
        
        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;
        
        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        
        // Get base spell name (without rank)
        std::string spellName = name;
        uint32 rank = spellInfo->GetRank();
        
        // Check if we already have this spell, and if so, only keep the highest rank
        auto it = uniqueSpells.find(spellName);
        if (it == uniqueSpells.end())
        {
            // First time seeing this spell
            uniqueSpells[spellName] = std::make_tuple(spellId, rank, costText);
        }
        else
        {
            // We've seen this spell before, check if this is a higher rank
            uint32 existingRank = std::get<1>(it->second);
            if (rank > existingRank)
            {
                // Replace with higher rank
                uniqueSpells[spellName] = std::make_tuple(spellId, rank, costText);
            }
        }
    }
    
    // Cap the list. Dumping every off-cooldown spell a level 80 bot knows put
    // dozens of lines of the most concrete, most quotable text into the prompt
    // -- which is precisely why bots ended up reciting their spellbook instead
    // of talking about the world around them.
    if (g_SnapshotMaxSpells == 0)
        return "";

    std::vector<std::string> picked;
    picked.reserve(uniqueSpells.size());

    for (const auto& [spellName, spellData] : uniqueSpells)
    {
        const uint32 rank = std::get<1>(spellData);
        const std::string& costText = std::get<2>(spellData);

        std::string line = spellName;
        if (rank > 0)
            line += " (Rank " + std::to_string(rank) + ")";
        line += " - " + costText;

        picked.push_back(std::move(line));
    }

    // Shuffle so a bot that does mention a spell is not always mentioning the
    // alphabetically-first one it knows.
    if (picked.size() > g_SnapshotMaxSpells)
    {
        Acore::Containers::RandomShuffle(picked);
        picked.resize(g_SnapshotMaxSpells);
    }

    std::ostringstream spellSummary;
    for (const std::string& line : picked)
        spellSummary << line << "\n";

    return spellSummary.str();
}

// --- Helper: Group info ---
std::vector<std::string> ChatHandler_GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;
        if(bot == member) continue;
        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";
        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = " [Under Attack by " + attacker->GetName() +
                            ", Level: " + std::to_string(attacker->GetLevel()) + ", HP: " + std::to_string(attacker->GetHealth()) +
                            "/" + std::to_string(attacker->GetMaxHealth()) + ")]";
        }
        std::string className = FormatPlayerClass(member->getClass());
        std::string raceName = FormatPlayerRace(member->getRace());
        info.push_back(
            member->GetName() +
            " (Level: " + std::to_string(member->GetLevel()) +
            ", Class: " + className +
            ", Race: " + raceName +
            ", HP: " + std::to_string(member->GetHealth()) + "/" + std::to_string(member->GetMaxHealth()) +
            ", Dist: " + std::to_string(dist) + ")" + beingAttacked
        );

    }
    return info;
}

// --- Helper: Visible players ---
std::vector<std::string> ChatHandler_GetVisiblePlayers(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap())
        return players;

    // Grid search rather than a walk of every online character on the realm.
    std::list<Player*> found;
    Acore::AnyPlayerInObjectRangeCheck check(bot, radius, false, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, radius);

    std::vector<std::pair<float, std::string>> scored;
    for (Player* player : found)
    {
        if (!player || player == bot || !player->IsInWorld())
            continue;
        if (!bot->IsWithinLOSInMap(player))
            continue;

        const float dist = bot->GetDistance(player);
        scored.emplace_back(dist, SafeFormat(
            "Player: {} (Level {}, {} {}, {}, {:.0f} yards)",
            player->GetName(), player->GetLevel(),
            FormatPlayerRace(player->getRace()), FormatPlayerClass(player->getClass()),
            player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde", dist));
    }

    std::sort(scored.begin(), scored.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    uint32_t taken = 0;
    for (auto const& entry : scored)
    {
        if (g_SnapshotMaxPlayers > 0 && taken >= g_SnapshotMaxPlayers)
            break;
        players.push_back(entry.second);
        ++taken;
    }

    return players;
}

// --- Helper: Visible locations/objects (creatures and gameobjects) ---
std::vector<std::string> ChatHandler_GetVisibleLocations(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap())
        return visible;

    // Grid search, not a walk of every spawn on the map. The old version
    // iterated Map::GetCreatureBySpawnIdStore() -- tens of thousands of
    // entries in Northrend -- with a LOS raycast per candidate, on the map
    // thread, for every prompt built.
    std::list<Creature*> creatures;
    OllamaNearbyCreatureCheck creatureCheck(bot, radius);
    Acore::CreatureListSearcher<OllamaNearbyCreatureCheck> creatureSearcher(bot, creatures, creatureCheck);
    Cell::VisitObjects(bot, creatureSearcher, radius);

    std::vector<std::pair<float, std::string>> scored;
    scored.reserve(creatures.size());

    for (Creature* c : creatures)
    {
        if (!c || c->IsPet() || c->IsTotem())
            continue;
        if (!bot->IsWithinLOSInMap(c))
            continue;

        std::string type;
        if (c->isDead())              type = "DEAD";
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot))type = "FRIENDLY";
        else                          type = "NEUTRAL";

        const float dist = bot->GetDistance(c);
        scored.emplace_back(dist, SafeFormat("{}: {} (Level {}, HP {}/{}, {:.0f} yards)",
                                             type, c->GetName(), c->GetLevel(),
                                             c->GetHealth(), c->GetMaxHealth(), dist));
    }

    std::sort(scored.begin(), scored.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    uint32_t taken = 0;
    for (auto const& entry : scored)
    {
        if (g_SnapshotMaxCreatures > 0 && taken >= g_SnapshotMaxCreatures)
            break;
        visible.push_back(entry.second);
        ++taken;
    }

    std::list<GameObject*> objects;
    Acore::GameObjectInRangeCheck goCheck(bot->GetPositionX(), bot->GetPositionY(),
                                          bot->GetPositionZ(), radius);
    Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> goSearcher(bot, objects, goCheck);
    Cell::VisitObjects(bot, goSearcher, radius);

    std::vector<std::pair<float, std::string>> goScored;
    for (GameObject* go : objects)
    {
        if (!go || go->GetName().empty())
            continue;
        if (!bot->IsWithinLOSInMap(go))
            continue;

        const float dist = bot->GetDistance(go);
        goScored.emplace_back(dist, SafeFormat("{} ({:.0f} yards)", go->GetName(), dist));
    }

    std::sort(goScored.begin(), goScored.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    taken = 0;
    for (auto const& entry : goScored)
    {
        if (g_SnapshotMaxObjects > 0 && taken >= g_SnapshotMaxObjects)
            break;
        visible.push_back(entry.second);
        ++taken;
    }

    return visible;
}

// --- Helper: Combat summary ---
std::string ChatHandler_GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();

    // Class-specific resource reporting
    auto classId = bot->getClass();

    auto printResource = [&](std::ostringstream& oss) {
        switch (classId)
        {
            case CLASS_WARRIOR:
                oss << ", Rage: " << bot->GetPower(POWER_RAGE) << "/" << bot->GetMaxPower(POWER_RAGE);
                break;
            case CLASS_ROGUE:
                oss << ", Energy: " << bot->GetPower(POWER_ENERGY) << "/" << bot->GetMaxPower(POWER_ENERGY);
                break;
            case CLASS_DEATH_KNIGHT:
                oss << ", Runic Power: " << bot->GetPower(POWER_RUNIC_POWER) << "/" << bot->GetMaxPower(POWER_RUNIC_POWER);
                break;
            case CLASS_HUNTER:
                oss << ", Focus: " << bot->GetPower(POWER_FOCUS) << "/" << bot->GetMaxPower(POWER_FOCUS);
                break;
            default: // Mana classes
                if (bot->GetMaxPower(POWER_MANA) > 0)
                    oss << ", Mana: " << bot->GetPower(POWER_MANA) << "/" << bot->GetMaxPower(POWER_MANA);
                break;
        }
    };

    if (inCombat)
    {
        oss << "IN COMBAT: ";
        if (victim)
        {
            oss << "Target: " << victim->GetName()
                << ", Level: " << victim->GetLevel()
                << ", HP: " << victim->GetHealth() << "/" << victim->GetMaxHealth();
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";
        printResource(oss);
    }
    else
    {
        oss << "NOT IN COMBAT. ";
        printResource(oss);
    }
    return oss.str();
}


std::string GenerateBotGameStateSnapshot(Player* bot)
{
    // Prepare each section
    std::string combat = ChatHandler_GetCombatSummary(bot);

    std::string group;
    std::vector<std::string> groupInfo = ChatHandler_GetGroupStatus(bot);
    if (!groupInfo.empty()) {
        group += "Group members:\n";
        for (const auto& entry : groupInfo) group += " - " + entry + "\n";
    }

    std::string spells = g_SnapshotIncludeSpells ? ChatHandler_GetBotSpellInfo(bot) : std::string();

    std::string quests;
    for (auto const& [questId, qsd] : bot->getQuestStatusMap())
    {
        // look up the template
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // get the English title as a fallback
        std::string title = quest->GetTitle();

        // then, if we have a locale record, overwrite it
        if (auto const* locale = sObjectMgr->GetQuestLocale(questId))
        {
            int locIdx = bot->GetSession()->GetSessionDbLocaleIndex();
            if (locIdx >= 0)
                ObjectMgr::GetLocaleString(locale->Title, locIdx, title);
        }

        // Convert quest status to readable string
        std::string statusText;
        switch (qsd.Status)
        {
            case QUEST_STATUS_NONE:       statusText = "not started"; break;
            case QUEST_STATUS_COMPLETE:   statusText = "complete (ready to turn in)"; break;
            case QUEST_STATUS_INCOMPLETE: statusText = "in progress"; break;
            case QUEST_STATUS_FAILED:     statusText = "failed"; break;
            case QUEST_STATUS_REWARDED:   statusText = "completed and rewarded"; break;
            default:                      statusText = "unknown"; break;
        }

        quests += "Quest \"" + title + "\" is " + statusText + "\n";
    }

    std::string los;
    std::vector<std::string> losLocs = ChatHandler_GetVisibleLocations(bot);
    if (!losLocs.empty()) {
        for (const auto& entry : losLocs) los += " - " + entry + "\n";
    }

    std::string players;
    std::vector<std::string> nearbyPlayers = ChatHandler_GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        for (const auto& entry : nearbyPlayers) players += " - " + entry + "\n";
    }

    // Use template
    return SafeFormat(
        g_ChatBotSnapshotTemplate,
        fmt::arg("combat", combat),
        fmt::arg("group", group),
        fmt::arg("spells", spells),
        fmt::arg("quests", quests),
        fmt::arg("los", los),
        fmt::arg("players", players)
    );
}


void PlayerBotChatHandler::ProcessChat(Player* player, uint32_t /*type*/, uint32_t lang, std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel, Player* receiver, uint8_t chainDepth,
                                       uint64_t crowdPlanId)
{
    if (player == nullptr) {
        LOG_ERROR("module.ollamachat", "[Ollama Chat] ProcessChat: player is null");
        return;
    }
    if (msg.empty()) {
        return;
    }
    if (lang == LANG_ADDON) return;
    std::string chanName = (channel != nullptr) ? channel->GetName() : "Unknown";
    uint32_t channelId = (channel != nullptr) ? channel->GetChannelId() : 0;
    std::string receiverName = (receiver != nullptr) ? receiver->GetName() : "None";
    if(g_DebugEnabled)
    {
        LOG_INFO("server.loading",
                "[Ollama Chat] Player {} sent msg: '{}' | Source: {} | Channel Name: {} | Channel ID: {} | Receiver: {}",
                player->GetName(), msg, (int)sourceLocal, chanName, channelId, receiverName);
    }


    PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    bool senderIsBot = (senderAI && senderAI->IsBotAI());

    // An item or quest someone links can be linked back the same way.
    if (!senderIsBot)
        Links_NoteMessage(msg);

    // --- conversation governor -------------------------------------------
    // One key per conversation space so cooldowns, rate limits and repetition
    // history are tracked per channel rather than globally.
    //
    // Party and raid chat keys on the group, not the zone. Keying it on the
    // zone made two unrelated parties standing in Elwynn share one cooldown,
    // one rate limit and one repetition history -- and made a party reset all
    // three every time it walked over a zone border.
    uint32_t scopeGroupOrZone = player->GetZoneId();
    if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
    {
        if (Group* senderGroup = player->GetGroup())
            scopeGroupOrZone = senderGroup->GetGUID().GetCounter();
    }

    const std::string scopeKey = Governor_MakeScopeKey(
        ChatChannelSourceLocalStr[sourceLocal],
        channel ? channel->GetChannelId() : 0,
        channel ? channel->GetName() : std::string(),
        (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
            ? player->GetGuildId() : 0,
        scopeGroupOrZone);

    const std::string contextChannel = channel ? channel->GetName() : std::string();

    // --- party instructions -----------------------------------------------
    // "ready", "mb", "wait": in a dungeon with a dungeon-clear tank these
    // steer the run, and the tank answers -- nobody else. Looked at before the
    // blacklist ("ready", "wait" and "drink" are playerbots commands too) and
    // before DisableForParty, which is about chatter, not about the run.
    uint64_t humanSeq = 0;   // the person's line, once it is in the transcript
    if (!senderIsBot)
    {
        const PartyCmdLine partyLine = PartyCmd_Classify(player, msg, sourceLocal);

        // "dc on", "dc pause": mod-dungeon-clear answers those itself.
        if (partyLine == PartyCmdLine::DcCommand)
            return;

        bool handled = false;
        if (partyLine == PartyCmdLine::Instruction)
        {
            Governor_NoteHumanMessage(scopeKey);
            humanSeq = Context_RecordLine(player, sourceLocal, contextChannel, msg, ContextLineKind::Human);
            handled = PartyCmd_HandleHumanLine(player, msg, sourceLocal, scopeKey, humanSeq);
            // Not handled: nothing to steer after all ("ready" with the run
            // already going), so an ordinary line from here on.
        }

        // "mage water pls": a mage in the group conjures it and trades it over,
        // for free. After the instructions, so "mb, water pls" both pauses the
        // run and gets the water -- the mage answers after the tank. Before the
        // blacklist too, since "trade" starts a line people use to ask.
        if (Conjure_HandleHumanLine(player, msg, sourceLocal, receiver, scopeKey, humanSeq))
            handled = true;

        // "stay back", "Joanni come trade me", "res": orders, carried out and
        // answered by the bot it was for. Before the blacklist, since "stay",
        // "follow" and "trade" start lines that people use to say them.
        if (!handled && Orders_HandleHumanLine(player, msg, sourceLocal, receiver, scopeKey, humanSeq))
            handled = true;

        if (handled)
            return;
    }

    auto startsWithWord = [](const std::string& text, const std::string& word) {
        if (text.size() < word.size()) return false;
        if (text.compare(0, word.size(), word) != 0) return false;
        // If exact length match or next char is whitespace/punctuation, it's a word
        return text.size() == word.size() || !std::isalnum((unsigned char)text[word.size()]);
    };

    std::string trimmedMsg = rtrim(msg);
    for (const std::string& blacklist : g_BlacklistCommands)
    {
        if (startsWithWord(trimmedMsg, blacklist))
        {
            if (g_DebugEnabled)
                LOG_INFO("server.loading",
                         "[Ollama Chat] Message starts with '{}' (blacklisted). Skipping bot responses.",
                         blacklist);
            return;
        }
    }
    
    // Check if this channel type is disabled
    if (sourceLocal == SRC_GENERAL_LOCAL && g_DisableForCustomChannels)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Custom channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL) && g_DisableForSayYell)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Say/Yell channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL) && g_DisableForGuild)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Guild channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL) && g_DisableForParty)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Party/Raid channels are disabled, skipping");
        }
        return;
    }

    if (!senderIsBot)
    {
        // A real player spoke here. This timestamp is what lets bots keep
        // talking to each other for a while afterwards.
        Governor_NoteHumanMessage(scopeKey);
    }
    else
    {
        // Bot-to-bot. Two brakes: an absolute depth ceiling, and the audience
        // rule -- bots do not hold conversations with nobody listening.
        if (!Governor_ChainDepthAllowed(chainDepth))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Chain depth {} reached the limit; not continuing.",
                         chainDepth);
            return;
        }

        // With one roll per bot line (Context.BotReplyPerMessage) the volume
        // stays in hand without it, so bots may also chat while a person is
        // only reading. The audience checks still apply.
        const bool recentHumanRequired =
            !(g_ContextBotReplyPerMessage && g_ContextBotChatWithoutRecentHuman);

        if (recentHumanRequired && !Governor_HasRecentHuman(scopeKey))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] No real player has spoken in {} recently; "
                         "bots will not talk among themselves here.", scopeKey);
            return;
        }
    }

    // --- shared chat context ----------------------------------------------
    // A person's line goes into the transcript now; a bot's line was recorded
    // when it was delivered, just before it was handed here. Either way, find
    // it, then work out who it was most likely aimed at.
    const uint64_t contextSeq = senderIsBot ? Context_LatestLineFrom(player, sourceLocal, contextChannel)
                              : humanSeq    ? humanSeq
                                            : Context_RecordLine(player, sourceLocal, contextChannel, msg,
                                                                 ContextLineKind::Human);

    const ContextAddressee addressee = (sourceLocal == SRC_WHISPER_LOCAL)
        ? ContextAddressee{}
        : Context_LikelyAddressee(player, sourceLocal, contextChannel, contextSeq);

    if (g_DebugEnabled && addressee.guid)
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] {}'s line looks like a reply to {}.",
                 player->GetName(), addressee.name);

    if (!senderIsBot && sourceLocal != SRC_WHISPER_LOCAL)
    {
        // A conversation running here hears the person too. (Bot lines are
        // noted when they are delivered.)
        Conversation_NoteLine(Crowd_KeyFor(scopeKey, sourceLocal, uint32(player->GetTeamId())),
                              player, false, msg, contextSeq, 0, addressee.guid);
    }

    // One pass over the online players, reused by every eligibility test
    // below. These questions used to be answered by a fresh full walk per
    // candidate bot, inside a loop that was itself over every online player.
    OllamaWorldSnapshot world;
    world.Build();

    std::vector<Player*> eligibleBots;
    
    // Handle different chat sources differently
    if (sourceLocal == SRC_WHISPER_LOCAL && receiver != nullptr)
    {
        // Check if whisper replies are disabled
        if (!g_EnableWhisperReplies)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper replies are disabled, skipping");
            }
            return;
        }
        
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Processing whisper from {} to {}", 
                    player->GetName(), receiver->GetName());
        }
        
        // Skip bot-to-bot whispers to prevent Ollama responses
        if (senderIsBot)
        {
            return;
        }
        
        // For whispers, only the receiver bot can respond (if it's a bot)
        PlayerbotAI* receiverAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (receiverAI && receiverAI->IsBotAI())
        {
            eligibleBots.push_back(receiver);
            if(g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat", "[Ollama Chat] Found eligible bot {} for whisper", receiver->GetName());
            }
        }
        else if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper target {} is not a bot or has no AI", receiver->GetName());
        }
    }
    else if (channel != nullptr)
    {
        // For channel chat, find all bots that are in the same channel instance
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Processing channel message in '{}' (ID: {})", 
                    channel->GetName(), channel->GetChannelId());
        }
        
        // Verify the original channel is valid before proceeding
        if (!channel)
        {
            if(g_DebugEnabled)
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] Channel is null, cannot process channel message");
            }
            return;
        }
        
        auto const& allPlayers = ObjectAccessor::GetPlayers();

        // Hoisted out of the per-candidate loop below. This used to be a full
        // GetPlayers() walk nested inside a GetPlayers() walk -- quadratic in
        // online characters, on every single channel message.
        bool hasRealPlayerInChannel = false;
        for (auto const& playerItr : allPlayers)
        {
            Player* candidateReal = playerItr.second;
            if (!candidateReal || !OllamaIsOnChannel(channel, candidateReal->GetGUID()))
                continue;

            if (OllamaIsRealPlayer(candidateReal))
            {
                hasRealPlayerInChannel = true;
                break;
            }
        }

        if (!hasRealPlayerInChannel)
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] No real players in channel '{}'; skipping.",
                         channel->GetName());
            return;
        }

        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (!candidate || candidate == player)
                continue;
                
            // Skip non-bots early
            PlayerbotAI* candidateAI = PlayerbotsMgr::instance().GetPlayerbotAI(candidate);
            if (!candidateAI || !candidateAI->IsBotAI())
                continue;
            
            // Classify by channel id, not by localized name substrings. The
            // old test looked for "General -" / "Trade -" / "LocalDefense -"
            // in the channel name, which only works on an English realm, and
            // GuildRecruitment was not handled at all -- which is why replies
            // went missing in the city channels.
            const uint32 chanId = channel->GetChannelId();

            // Zone-scoped: one instance per zone/city.
            const bool isZoneChannel = (chanId == uint32(ChatChannelId::GENERAL) ||
                                        chanId == uint32(ChatChannelId::TRADE) ||
                                        chanId == uint32(ChatChannelId::LOCAL_DEFENSE) ||
                                        chanId == uint32(ChatChannelId::GUILD_RECRUITMENT));

            // Realm-wide.
            const bool isGlobalChannel = (chanId == uint32(ChatChannelId::WORLD_DEFENSE) ||
                                          chanId == uint32(ChatChannelId::LOOKING_FOR_GROUP));

            if (isZoneChannel && candidate->GetZoneId() != player->GetZoneId())
                continue;   // wrong zone for a zone-scoped channel

            // A custom (unnumbered) channel has id 0 and no zone semantics;
            // membership alone decides, which is checked below.
            
            // CHANNEL MEMBERSHIP CHECK: Bot must actually be in the channel
            if (!OllamaIsOnChannel(channel, candidate->GetGUID()))
            {
                if(g_DebugEnabled)
                {
                    //LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} not in channel '{}', skipping", candidate->GetName(), channel->GetName());
                }
                continue;
            }
            
            // FACTION CHECK: For non-global channels, ensure same faction
            if (candidate->GetTeamId() != player->GetTeamId())
            {
                if (!isGlobalChannel)
                {
                    if(g_DebugEnabled)
                    {
                        //LOG_ERROR("module.ollamachat", "[Ollama Chat] Bot {} FAILED faction check - Bot: {}, Player: {}, Channel: '{}'", candidate->GetName(), (int)candidate->GetTeamId(), (int)player->GetTeamId(), channel->GetName());
                    }
                    continue; // SKIP this bot - wrong faction
                }
            }
            
            if (!hasRealPlayerInChannel)
            {
                if(g_DebugEnabled)
                {
                    //LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} skipped - no real players in channel '{}'", candidate->GetName(), channel->GetName());
                }
                continue;
            }
            
            // ONLY add bots that passed ALL verifications
            eligibleBots.push_back(candidate);
            if(g_DebugEnabled)
            {
                // LOG_INFO("module.ollamachat", "[Ollama Chat] VERIFIED eligible bot {} in channel '{}' - Distance: {:.2f}, Zone match: {}", candidate->GetName(), channel->GetName(), candidate->GetDistance(player), (candidate->GetZoneId() == player->GetZoneId()));
            }
        }
        
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Found {} bots in channel instance '{}'", 
                    eligibleBots.size(), channel->GetName());
        }
    }
    else
    {
        // For other chat types (say, yell, guild, party, etc.), use all players and filter by eligibility
        auto const& allPlayers = ObjectAccessor::GetPlayers();
        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (candidate->IsInWorld() && candidate != player)
            {
                PlayerbotAI* candidateAI = PlayerbotsMgr::instance().GetPlayerbotAI(candidate);
                if (candidateAI && candidateAI->IsBotAI())
                {
                    // For Guild/Party, verify there's a real player in that guild/party
                    if (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
                    {
                        if (candidate->GetGuildId() != 0 &&
                            !world.GuildHasRealPlayer(candidate->GetGuildId()))
                        {
                            continue;   // no real players in that guild
                        }
                    }
                    else if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
                    {
                        Group* group = candidate->GetGroup();
                        if (group)
                        {
                            // Check if any real player is in this group
                            bool hasRealPlayerInGroup = false;
                            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            {
                                if (OllamaIsRealPlayer(ref->GetSource()))
                                {
                                    hasRealPlayerInGroup = true;
                                    break;
                                }
                            }
                            if (!hasRealPlayerInGroup)
                                continue; // Skip bot - no real players in group
                        }
                    }
                    else if (sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL)
                    {
                        // Require a real player within hearing distance.
                        const float threshold =
                            (sourceLocal == SRC_SAY_LOCAL) ? g_SayDistance : g_YellDistance;

                        if (!world.RealPlayerWithin(candidate, threshold))
                            continue;   // nobody can hear it
                    }
                    
                    eligibleBots.push_back(candidate);
                }
            }
        }
    }
    
    std::vector<Player*> candidateBots;
    int notEligibleCount = 0;
    for (Player* bot : eligibleBots)
    {
        if (!bot)
        {
            continue;
        }
        
        // For channel messages, bots in eligibleBots have already passed STRICT channel checks
        // Only run additional eligibility checks for non-channel sources
        // EXCEPTION: If channel is nullptr but sourceLocal is a channel type (like GENERAL), 
        // treat it as a channel message (happens with bot-initiated messages)
        bool isChannelSource = (sourceLocal == SRC_GENERAL_LOCAL);
        
        if (channel != nullptr || isChannelSource)
        {
            // Channel bots have already been verified to be in EXACT same channel instance
            // OR this is a channel-type source (General) even without channel object
            candidateBots.push_back(bot);
        }
        else
        {
            // For non-channel sources (Say/Yell/Guild/Party/Whisper), run the full eligibility check
            if (IsBotEligibleForChatChannelLocal(bot, player, sourceLocal, channel, receiver))
                candidateBots.push_back(bot);
            else
                notEligibleCount++;
        }
    }
    
    if (g_DebugEnabled && notEligibleCount > 0)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} bots not eligible for {} (distance/guild/party checks failed)",
                notEligibleCount, ChatChannelSourceLocalStr[sourceLocal]);
    }

    // A whispered thank-you on its own needs no answer: it is said and done.
    if (!senderIsBot && sourceLocal == SRC_WHISPER_LOCAL && receiver &&
        OllamaIsThanksOnly(msg, { receiver->GetName() }) && urand(0, 99) >= g_CrowdThanksReplyChance)
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} only said thanks to {}; no reply.",
                     player->GetName(), receiver->GetName());
        return;
    }

    // Crowd replies: the line gets a planned set of answers, typed one after
    // another by different bots, instead of an independent roll per listening
    // bot capped at MaxBotsToPick. Whispers keep the path below: a whisper
    // only ever has one listener.
    if (g_CrowdEnable && g_ContextEnable && sourceLocal != SRC_WHISPER_LOCAL)
    {
        if (senderIsBot)
            HandleCrowdBotLine(player, msg, sourceLocal, channel, scopeKey, contextSeq,
                               addressee, candidateBots, chainDepth, crowdPlanId);
        else
            PlanCrowdForHumanLine(player, msg, sourceLocal, channel, scopeKey, contextSeq,
                                  addressee, candidateBots);
        return;
    }

    // Determine reply chance based on channel type
    uint32_t chance;
    if (sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL)
    {
        // Say/Yell channel type
        chance = senderIsBot ? g_BotReplyChance_Say : g_PlayerReplyChance_Say;
    }
    else if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
    {
        // Party/Raid channel type
        chance = senderIsBot ? g_BotReplyChance_Party : g_PlayerReplyChance_Party;
    }
    else if (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
    {
        // Guild/Officer channel type
        chance = senderIsBot ? g_BotReplyChance_Guild : g_PlayerReplyChance_Guild;
    }
    else if (sourceLocal == SRC_GENERAL_LOCAL)
    {
        // General/Trade/Custom channel type
        chance = senderIsBot ? g_BotReplyChance_Channel : g_PlayerReplyChance_Channel;
    }
    else if (sourceLocal == SRC_WHISPER_LOCAL)
    {
        // A whisper is direct address -- as explicit as being named, and the
        // bot is the only recipient. It is not subject to the ambient reply
        // chance; EnableWhisperReplies is the whole gate.
        //
        // This used to fall through to the Say chance below, so lowering
        // PlayerReplyChance.Say randomly swallowed whispers, and setting it to
        // 0 meant a whispered bot never answered at all even with
        // EnableWhisperReplies = 1.
        chance = 100;
    }
    else
    {
        // Default fallback - use Say chances
        chance = senderIsBot ? g_BotReplyChance_Say : g_PlayerReplyChance_Say;
    }
    
    // One roll per bot line rather than per listening bot: the per-line
    // chances live in their own keys, so BotReplyChance.* keeps its meaning.
    if (senderIsBot && g_ContextBotReplyPerMessage && sourceLocal != SRC_WHISPER_LOCAL)
    {
        if (sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL)
            chance = g_ContextBotLineReplyChance_Say;
        else if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
            chance = g_ContextBotLineReplyChance_Party;
        else if (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
            chance = g_ContextBotLineReplyChance_Guild;
        else if (sourceLocal == SRC_GENERAL_LOCAL)
            chance = g_ContextBotLineReplyChance_Channel;
    }

    // Each bot->bot hop makes the next reply less likely, so a chain runs out
    // of energy on its own well before it hits the hard depth ceiling. Never
    // applied to a whisper: bot-to-bot whispers are refused long before this,
    // so a whisper here is always a person talking to a bot directly.
    if (senderIsBot && sourceLocal != SRC_WHISPER_LOCAL)
        chance = Governor_ApplyChainDecay(chance, chainDepth);

    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Sender: {} ({}), Channel: {}, Depth: {}, Reply Chance: {}%, Candidate Bots: {}",
                player->GetName(), senderIsBot ? "BOT" : "PLAYER", ChatChannelSourceLocalStr[sourceLocal], chainDepth, chance, candidateBots.size());
    }

    // Not just an optimisation: this return also used to swallow the name-
    // mention path, so a channel set to 0% ignored a bot being addressed by
    // name. Direct address has its own chance, so only bail when both are off.
    if (chance == 0 && g_DirectAddressReplyChance == 0)
        return;
    
    std::vector<Player*> finalCandidates;

    // Bots this line is aimed at: whispered, named, or the one being answered.
    // They go first when MaxBotsToPick trims the list, and they get the "they
    // are talking to you" hint rather than the bystander one.
    std::unordered_set<Player*> addressedBots;

    // For whispers, handle directly - there should only be one receiver bot
    if (sourceLocal == SRC_WHISPER_LOCAL)
    {
        if (!candidateBots.empty())
        {
            Player* whisperBot = candidateBots[0]; // Should only be one bot for whispers
            if (!(g_DisableRepliesInCombat && whisperBot->IsInCombat()))
            {
                finalCandidates.push_back(whisperBot);
                addressedBots.insert(whisperBot);
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper: Bot {} selected to respond", whisperBot->GetName());
                }
            }
        }
    }
    else
    {
        // Handle non-whisper chats with normal multi-bot logic
        std::vector<std::pair<size_t, Player*>> mentionedBots;

        // Whole-word, case-insensitive name match; see OllamaFindBotNameMention.
        auto isBotNameMentioned = [&trimmedMsg](const std::string& botName) -> size_t {
            return OllamaFindBotNameMention(trimmedMsg, botName);
        };

        for (Player* bot : candidateBots)
        {
            if (!bot)
            {
                continue;
            }
            if (g_DisableRepliesInCombat && bot->IsInCombat())
            {
                continue;
            }

            // Cross-faction say/yell renders as gibberish on the client, so a
            // fluent reply is the most immersion-breaking thing the module can
            // do. Skipping it also saves the round trip.
            if ((sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL) &&
                Roleplay_IsLanguageBarrier(player, bot))
            {
                continue;
            }

            size_t pos = isBotNameMentioned(bot->GetName());
            if (pos != std::string::npos)
            {
                mentionedBots.emplace_back(pos, bot);
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} mentioned at position {} in message", bot->GetName(), pos);
                }
            }
        }

        if (!mentionedBots.empty())
        {
            // Sort by position to get the first mentioned bot
            std::sort(mentionedBots.begin(), mentionedBots.end(),
                      [](const std::pair<size_t, Player*> &a, const std::pair<size_t, Player*> &b) { return a.first < b.first; });
            Player* chosen = mentionedBots.front().second;
            if (!(g_DisableRepliesInCombat && chosen->IsInCombat()))
            {
                finalCandidates.push_back(chosen);
                addressedBots.insert(chosen);
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} selected (mentioned first at position {})", 
                            chosen->GetName(), mentionedBots.front().first);
                }
            }
        }
        else
        {
            // Bots that could answer at all.
            std::vector<Player*> pool;
            for (Player* bot : candidateBots)
            {
                if (g_DisableRepliesInCombat && bot->IsInCombat())
                {
                    if(g_DebugEnabled)
                    {
                        LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} skipped - in combat", bot->GetName());
                    }
                    continue;
                }

                if ((sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL) &&
                    Roleplay_IsLanguageBarrier(player, bot))
                {
                    continue;
                }

                pool.push_back(bot);
            }

            // When the transcript shows who this line answers, that bot is
            // the one being spoken to and everyone else is a bystander -- even
            // a bot that chatted with this person a minute ago. Without that,
            // every bot "in conversation" treated each new line as aimed at it
            // and replied to someone else's exchange.
            auto isAddressee = [&addressee](Player* bot)
            {
                return addressee.guid != 0 && bot->GetGUID().GetRawValue() == addressee.guid;
            };

            if (senderIsBot && g_ContextBotReplyPerMessage)
            {
                // One roll for the whole line: does anybody answer this bot?
                // A roll per listening bot made a reply near-certain in a busy
                // channel, however low BotReplyChance was set.
                Player* aimedAt = nullptr;
                std::vector<Player*> able;
                for (Player* bot : pool)
                {
                    const bool aimed = isAddressee(bot);
                    if (!Governor_CanSend(bot->GetGUID(), scopeKey, aimed))
                        continue;
                    if (aimed)
                        aimedAt = bot;
                    able.push_back(bot);
                }

                uint32_t lineChance = chance;   // already decayed for this hop
                if (aimedAt)
                    lineChance = std::max(lineChance,
                                          Governor_ApplyChainDecay(g_ContextBotAddressedChance, chainDepth));

                const uint32_t roll = urand(0, 99);
                if (!able.empty() && roll < lineChance)
                {
                    Player* pick = aimedAt ? aimedAt : able[urand(0, static_cast<uint32>(able.size() - 1))];
                    finalCandidates.push_back(pick);
                    if (pick == aimedAt)
                        addressedBots.insert(pick);

                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat",
                                 "[Ollama Chat] Bot line from {} answered by {} ({} < {}%{})",
                                 player->GetName(), pick->GetName(), roll, lineChance,
                                 pick == aimedAt ? ", addressed" : "");
                }
                else if (g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat",
                             "[Ollama Chat] Bot line from {} not answered ({} >= {}%, {} able)",
                             player->GetName(), roll, lineChance, able.size());
                }
            }
            else
            {
                for (Player* bot : pool)
                {
                    // Aimed at this bot: the transcript says it is the one being
                    // answered. With no clear addressee the older rules apply --
                    // an open conversation with this person, or a small party.
                    const bool aimed = addressee.guid
                        ? isAddressee(bot)
                        : OllamaIsDirectAddress(bot, player, sourceLocal, trimmedMsg, senderIsBot, scopeKey);

                    uint32_t botChance = chance;
                    if (aimed)
                    {
                        botChance = senderIsBot
                            ? std::max(chance, Governor_ApplyChainDecay(g_ContextBotAddressedChance, chainDepth))
                            : std::max(chance, g_DirectAddressReplyChance);
                    }
                    else if (addressee.guid && !senderIsBot)
                    {
                        botChance = chance * g_ContextBystanderChancePct / 100;
                    }

                    // A bot on cooldown would only be dropped after being
                    // picked, which could leave nobody answering at all.
                    if (!Governor_CanSend(bot->GetGUID(), scopeKey, aimed))
                    {
                        if (g_DebugEnabled)
                            LOG_INFO("module.ollamachat",
                                     "[Ollama Chat] Bot {} not considered: cooldown or rate limit.",
                                     bot->GetName());
                        continue;
                    }

                    uint32_t roll = urand(0, 99);
                    if (roll < botChance)
                    {
                        finalCandidates.push_back(bot);
                        if (aimed)
                            addressedBots.insert(bot);
                        if(g_DebugEnabled)
                        {
                            LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} PASSED chance roll ({} < {}%{})", bot->GetName(), roll, botChance, aimed ? ", addressed" : "");
                        }
                    }
                    else if(g_DebugEnabled)
                    {
                        LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} FAILED chance roll ({} >= {}%{})", bot->GetName(), roll, botChance, aimed ? ", addressed" : "");
                    }
                }
            }
        }
    }

    
    if (finalCandidates.empty())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] *** NO BOTS RESPONDING *** to {} from {} in {} channel. "
                    "Eligible: {}, Candidates: {}, Final: 0, Chance: {}%",
                    senderIsBot ? "BOT" : "PLAYER", player->GetName(), ChatChannelSourceLocalStr[sourceLocal],
                    eligibleBots.size(), candidateBots.size(), chance);
            LOG_INFO("module.ollamachat", "[Ollama Chat] No eligible bots found to respond to message '{}'. "
                    "Source: {}, Eligible bots: {}, Candidate bots: {}, Combat disabled: {}",
                    msg, ChatChannelSourceLocalStr[sourceLocal], eligibleBots.size(), 
                    candidateBots.size(), g_DisableRepliesInCombat);
        }
        return;
    }
    
    if (finalCandidates.size() > g_MaxBotsToPick)
    {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(finalCandidates.begin(), finalCandidates.end(), g);
        // Whoever the line was aimed at answers first; a random cut used to
        // drop them and let a bystander answer in their place.
        std::stable_partition(finalCandidates.begin(), finalCandidates.end(),
                              [&addressedBots](Player* b) { return addressedBots.count(b) > 0; });
        uint32_t countToPick = urand(1, std::max<uint32_t>(1, g_MaxBotsToPick));
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Limiting {} bots to {} (MaxBotsToPick)", finalCandidates.size(), countToPick);
        }
        finalCandidates.resize(countToPick);
    }
    
    if(g_DebugEnabled && !finalCandidates.empty())
    {
        std::string botNames;
        for (Player* bot : finalCandidates)
        {
            if (!botNames.empty()) botNames += ", ";
            botNames += bot->GetName();
        }
        LOG_INFO("module.ollamachat", "[Ollama Chat] *** {} BOTS RESPONDING *** to {} from {} in {}: [{}]",
                finalCandidates.size(), senderIsBot ? "BOT" : "PLAYER", player->GetName(),
                ChatChannelSourceLocalStr[sourceLocal], botNames);
    }
    
    const uint64_t senderGuid = player->GetGUID().GetRawValue();

    for (Player* bot : finalCandidates)
    {
        if (!bot)
            continue;

        // Everything below runs on the world thread: prompt building reads
        // live world state, and the governor decides before we spend an LLM
        // call rather than after.
        // A line aimed at this bot is owed an answer, so it skips the pacing
        // cooldowns. The global messages-per-minute ceiling still applies.
        const bool directAddress = addressedBots.count(bot) > 0 ||
            (!addressee.guid &&
             OllamaIsDirectAddress(bot, player, sourceLocal, trimmedMsg, senderIsBot, scopeKey));

        if (!Governor_CanSend(bot->GetGUID(), scopeKey, directAddress))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} skipped: cooldown or rate limit.", bot->GetName());
            continue;
        }

        // Tell the bot whether it is being spoken to, overhearing someone
        // else's exchange, or hearing something said to the room.
        std::string replyHint;
        if (g_ContextEnable)
        {
            if (directAddress)
                replyHint = g_ContextHintAddressed;
            else if (addressee.guid)
                replyHint = SafeFormat(g_ContextHintBystander,
                                       fmt::arg("addressee", addressee.name),
                                       fmt::arg("pass_token", g_ContextPassToken));
            else
                replyHint = SafeFormat(g_ContextHintOpen,
                                       fmt::arg("pass_token", g_ContextPassToken));

            if (replyHint == "[Format Error]")
                replyHint.clear();
        }

        std::string prompt = GenerateBotPrompt(bot, msg, player, sourceLocal, contextChannel,
                                               contextSeq, replyHint);
        if (prompt.empty())
            continue;

        OllamaChatRequest request;
        request.botGuid     = bot->GetGUID().GetRawValue();
        request.targetGuid  = senderGuid;
        request.source      = sourceLocal;
        request.channelName = channel ? channel->GetName() : std::string();
        request.channelId   = channel ? channel->GetChannelId() : 0;
        request.chainDepth  = chainDepth;
        request.directAddress = directAddress;
        request.scopeKey    = scopeKey;
        request.prompt      = std::move(prompt);
        request.botName     = bot->GetName();
        request.originMessage = msg;
        request.kind = (g_RoleplayEnable && g_RoleplayStrictness >= 1)
                           ? OllamaRequestKind::RoleplayReply
                           : OllamaRequestKind::ChatReply;
        request.triggerBotReplies = (sourceLocal != SRC_WHISPER_LOCAL);
        request.recordHistory     = !senderIsBot;
        request.updateSentiment   = !senderIsBot && g_EnableSentimentTracking;
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);

        if (!OllamaDispatch_Submit(std::move(request)) && g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] Bot {} reply dropped: dispatcher queue full.",
                     bot->GetName());
        }
    }
}

static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player, ChatChannelSourceLocal source, Channel* channel, Player* receiver)
{
    if (!bot || !player || bot == player)
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligible: FAILED basic check - bot={}, player={}, same={}", 
                    (void*)bot, (void*)player, (bot == player));
        return false;
    }
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligible: Bot {} FAILED - no PlayerbotAI", bot->GetName());
        return false;
    }
        
    // For whispers, only the specific receiver should respond
    if (source == SRC_WHISPER_LOCAL)
    {
        // Don't allow bot-to-bot whisper responses
        PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        if (senderAI && senderAI->IsBotAI())
        {
            return false;
        }
        
        return (receiver && bot == receiver);
    }
    
    // Check team compatibility for non-proximity chats (except channels which can be cross-faction)
    // Say and Yell are proximity-based and don't require same faction
    bool isProximityChatSource = (source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL);
    if (!channel && !isProximityChatSource && bot->GetTeamId() != player->GetTeamId())
        return false;
    
    // For channels, check if bot is in the specific channel instance
    if (channel)
    {
        // Verify the channel is valid before proceeding
        if (!channel)
        {
            if(g_DebugEnabled)
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Channel is null");
            }
            return false;
        }
            
        // ONLY use exact channel instance check - NO Player::IsInChannel() anymore
        ChannelMgr* candidateCMgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (!candidateCMgr)
            return false;
            
        Channel* candidateChannel = candidateCMgr->GetChannel(channel->GetName(), bot);
        // Verify both channels are valid and are the exact same instance
        if (!candidateChannel || candidateChannel != channel)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Bot {} not in same channel instance '{}' - Bot team: {}, Channel ptr: {} vs {}", 
                        bot->GetName(), channel->GetName(), (int)bot->GetTeamId(),
                        (void*)candidateChannel, (void*)channel);
            }
            return false;
        }
        
        // Additional team check for cross-faction channels - only allow same faction unless it's a global channel
        if (bot->GetTeamId() != player->GetTeamId())
        {
            // Allow cross-faction only for specific global channels
            const uint32 chanId = channel->GetChannelId();
            const bool isGlobalChannel = (chanId == uint32(ChatChannelId::WORLD_DEFENSE) ||
                                          chanId == uint32(ChatChannelId::LOOKING_FOR_GROUP));
            if (!isGlobalChannel)
            {
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Bot {} different faction from player - Bot: {}, Player: {}, Channel: '{}'", bot->GetName(), (int)bot->GetTeamId(), (int)player->GetTeamId(), channel->GetName());
                }
                return false;
            }
        }
    }
    
    bool isInParty = (player->GetGroup() && bot->GetGroup() && (player->GetGroup() == bot->GetGroup()));
    float threshold = 0.0f;
    
    switch (source)
    {
        case SRC_SAY_LOCAL:    
            threshold = g_SayDistance;
            if (threshold > 0.0f)
            {
                if (!bot->IsInWorld() || !player->IsInWorld())
                    return false;
                    
                float distance = bot->GetDistance(player);
                return distance <= threshold;
            }
            return false;
            
        case SRC_YELL_LOCAL:   
            threshold = g_YellDistance;
            return (threshold > 0.0f && player->GetDistance(bot) <= threshold);
            
        case SRC_GUILD_LOCAL:
        case SRC_OFFICER_LOCAL:
            return (player->GetGuild() && bot->GetGuildId() == player->GetGuildId());
            
        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:
            return isInParty;
            
        case SRC_WHISPER_LOCAL:
            // For whispers, the bot should only respond if it's the specific receiver
            return (receiver && bot == receiver);
            
        case SRC_GENERAL_LOCAL:
            // For channels like General, Trade, etc., no distance check - only channel membership matters
            // Channel membership was already checked above
            return true;
            
        default:
            return false;
    }
}

// --------------------------------------------------------------------------
// Crowd replies
//
// ProcessChat hands every channel line here (see mod-ollama-chat_crowd.h for
// the why). A person's line gets a plan: who answers, in what order. A bot's
// line either adds an answer-back to the crowd it belongs to, or -- with
// nothing running -- may get a bot or two answering it.
// --------------------------------------------------------------------------

namespace
{
    std::mt19937& CrowdRng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint32_t PlayerReplyChanceFor(ChatChannelSourceLocal source)
    {
        switch (source)
        {
            case SRC_PARTY_LOCAL:
            case SRC_RAID_LOCAL:    return g_PlayerReplyChance_Party;
            case SRC_GUILD_LOCAL:
            case SRC_OFFICER_LOCAL: return g_PlayerReplyChance_Guild;
            case SRC_GENERAL_LOCAL: return g_PlayerReplyChance_Channel;
            default:                return g_PlayerReplyChance_Say;
        }
    }

    uint32_t BotLineReplyChanceFor(ChatChannelSourceLocal source)
    {
        switch (source)
        {
            case SRC_PARTY_LOCAL:
            case SRC_RAID_LOCAL:    return g_ContextBotLineReplyChance_Party;
            case SRC_GUILD_LOCAL:
            case SRC_OFFICER_LOCAL: return g_ContextBotLineReplyChance_Guild;
            case SRC_GENERAL_LOCAL: return g_ContextBotLineReplyChance_Channel;
            default:                return g_ContextBotLineReplyChance_Say;
        }
    }

    // Bots that could answer this line at all.
    std::vector<Player*> CrowdPool(Player* speaker, ChatChannelSourceLocal source,
                                   const std::vector<Player*>& candidates)
    {
        std::vector<Player*> pool;
        pool.reserve(candidates.size());
        for (Player* bot : candidates)
        {
            if (!bot || bot == speaker || !bot->IsInWorld())
                continue;
            if (g_DisableRepliesInCombat && bot->IsInCombat())
                continue;
            // Cross-faction /say arrives as gibberish; a fluent answer would
            // be the most immersion-breaking thing the module could do.
            if ((source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL) &&
                Roleplay_IsLanguageBarrier(speaker, bot))
                continue;
            pool.push_back(bot);
        }
        return pool;
    }

    // Bots named in the line, in the order they are named.
    std::vector<Player*> NamedBots(const std::string& msg, const std::vector<Player*>& pool, size_t max)
    {
        std::vector<std::pair<size_t, Player*>> found;
        for (Player* bot : pool)
        {
            const size_t pos = OllamaFindBotNameMention(msg, bot->GetName());
            if (pos != std::string::npos)
                found.emplace_back(pos, bot);
        }

        std::sort(found.begin(), found.end(),
                  [](const std::pair<size_t, Player*>& a, const std::pair<size_t, Player*>& b)
                  { return a.first < b.first; });

        std::vector<Player*> out;
        for (const auto& entry : found)
        {
            if (out.size() >= max)
                break;
            out.push_back(entry.second);
        }
        return out;
    }

    Player* FindInPool(const std::vector<Player*>& pool, uint64_t guid)
    {
        if (!guid)
            return nullptr;
        for (Player* bot : pool)
            if (bot->GetGUID().GetRawValue() == guid)
                return bot;
        return nullptr;
    }

    // `count` more voices for a crowd that should sound like several different
    // people: a different personality each where possible, playful and
    // straight styles taking turns, and nobody who has only just spoken.
    std::vector<Player*> PickCrowdVoices(const std::vector<Player*>& pool,
                                         const std::vector<Player*>& fixed, uint32_t count)
    {
        std::vector<Player*> out;
        if (count == 0)
            return out;

        // Bots that have not spoken lately first; the ones that just did only
        // when there are not enough others, so a small group does not fall
        // silent just because everyone in it has already said something.
        std::vector<Player*> rest, stale;
        for (Player* bot : pool)
        {
            if (std::find(fixed.begin(), fixed.end(), bot) != fixed.end())
                continue;
            (Governor_BotIsFresh(bot->GetGUID()) ? rest : stale).push_back(bot);
        }
        std::shuffle(rest.begin(), rest.end(), CrowdRng());
        if (rest.size() < count)
        {
            std::shuffle(stale.begin(), stale.end(), CrowdRng());
            rest.insert(rest.end(), stale.begin(), stale.end());
        }
        if (rest.empty())
            return out;

        // Looking a personality up can assign one, and assigning writes to the
        // database -- so only a handful of shuffled candidates are considered.
        const size_t window = std::min<size_t>(rest.size(), std::max<size_t>(12, size_t(count) * 3));
        rest.resize(window);

        std::vector<std::string> keys;
        keys.reserve(rest.size());
        for (Player* bot : rest)
            keys.push_back(GetBotPersonality(bot));

        std::unordered_set<std::string> usedKeys;
        uint32_t playful  = 0;
        uint32_t straight = 0;
        for (Player* bot : fixed)
        {
            const std::string key = GetBotPersonality(bot);
            usedKeys.insert(key);
            if (Crowd_IsPlayful(key))
                ++playful;
            else
                ++straight;
        }

        std::vector<bool> taken(rest.size(), false);
        while (out.size() < count)
        {
            const bool wantPlayful = (playful == straight) ? (urand(0, 1) == 0) : (playful < straight);

            int best = -1, unused = -1, any = -1;
            for (size_t i = 0; i < rest.size(); ++i)
            {
                if (taken[i])
                    continue;
                if (any < 0)
                    any = int(i);
                if (usedKeys.count(keys[i]))
                    continue;
                if (unused < 0)
                    unused = int(i);
                if (Crowd_IsPlayful(keys[i]) == wantPlayful)
                {
                    best = int(i);
                    break;
                }
            }

            const int pick = best >= 0 ? best : (unused >= 0 ? unused : any);
            if (pick < 0)
                break;

            taken[size_t(pick)] = true;
            out.push_back(rest[size_t(pick)]);
            usedKeys.insert(keys[size_t(pick)]);
            if (Crowd_IsPlayful(keys[size_t(pick)]))
                ++playful;
            else
                ++straight;
        }
        return out;
    }

    std::string CrowdNames(const CrowdPlan& plan)
    {
        std::string names;
        for (const CrowdSlot& slot : plan.slots)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(slot.botGuid));
            if (!names.empty())
                names += ", ";
            names += bot ? bot->GetName() : std::string("?");
            if (slot.addressed)
                names += "*";
        }
        return names;
    }

    const char* CrowdKindName(CrowdLineKind kind)
    {
        switch (kind)
        {
            case CrowdLineKind::Aimed:        return "aimed";
            case CrowdLineKind::Open:         return "open call";
            case CrowdLineKind::Ambient:      return "to the room";
            case CrowdLineKind::BotLine:      return "bot line";
            case CrowdLineKind::Greeting:     return "greeting";
            case CrowdLineKind::Conversation: return "conversation";
            case CrowdLineKind::Command:      return "instruction";
            case CrowdLineKind::Favor:        return "favour";
            case CrowdLineKind::Order:        return "order";
        }
        return "?";
    }

    // One running plan per key. Party, raid and guild chat are one
    // conversation each; a channel and /say area are one per faction, since
    // the two sides never see each other's lines there.
    std::string CrowdKeyFor(const std::string& scopeKey, ChatChannelSourceLocal source, Player* speaker)
    {
        return Crowd_KeyFor(scopeKey, source, uint32(speaker->GetTeamId()));
    }

    // Names a line might be addressed to, so "ty Lahu" still reads as a thank-you.
    std::vector<std::string> NamesOf(const std::vector<Player*>& bots, const std::string& extra = std::string())
    {
        std::vector<std::string> names;
        names.reserve(bots.size() + 1);
        for (Player* bot : bots)
            names.push_back(bot->GetName());
        if (!extra.empty())
            names.push_back(extra);
        return names;
    }

    CrowdPlan NewPlan(Player* speaker, const std::string& msg, ChatChannelSourceLocal source,
                      Channel* channel, const std::string& scopeKey, uint64_t seq)
    {
        CrowdPlan plan;
        plan.key          = CrowdKeyFor(scopeKey, source, speaker);
        plan.scopeKey     = scopeKey;
        plan.source       = source;
        plan.channelName  = channel ? channel->GetName() : std::string();
        plan.channelId    = channel ? channel->GetChannelId() : 0;
        plan.speakerGuid  = speaker->GetGUID().GetRawValue();
        plan.speakerIsBot = OllamaIsBotPlayer(speaker);
        plan.speakerName  = speaker->GetName();
        plan.message      = msg;
        plan.messageSeq   = seq;
        return plan;
    }
}

static void PlanCrowdForHumanLine(Player* player, const std::string& msg,
                                  ChatChannelSourceLocal source, Channel* channel,
                                  const std::string& scopeKey, uint64_t seq,
                                  const ContextAddressee& addressee,
                                  const std::vector<Player*>& candidates)
{
    const std::vector<Player*> pool = CrowdPool(player, source, candidates);
    if (pool.empty())
        return;

    // "ty", "lol", "ok" -- even shouted, even with a name -- are said to
    // whoever spoke last or was named, if anyone. "HELLO IS ANYONE THERE!?"
    // is said to everyone, and so is any other shout ("IM ON FIRE"): a shout
    // is for the whole room to hear.
    const CrowdText::LineTraits traits =
        CrowdText::Classify(Context_CleanText(msg), g_CrowdAckWords, g_CrowdOpenCallPhrases,
                            NamesOf(candidates, addressee.name), g_CrowdGreetingWords);
    const bool shouted   = traits.shouted;
    const bool ack       = traits.ack;
    const bool openWords = traits.openWords;
    const bool open      = traits.open;
    const bool greeting  = traits.greeting;

    const uint32_t listeners = static_cast<uint32_t>(pool.size());
    const uint32_t cap = CrowdText::Capacity(listeners, g_CrowdBotsPerResponder, g_CrowdMaxResponders);

    // A city or an inn: people are there to talk, so more of them answer.
    const bool hub = (source == SRC_GENERAL_LOCAL || source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL) &&
                     OllamaIsSocialHub(player);
    const uint32_t half = std::max<uint32_t>(1, (cap + 1) / 2);

    // Who it is aimed at. Names first -- and a named bot that cannot answer
    // right now (in combat, or the roll) does not hand its line to whoever
    // spoke last.
    std::vector<Player*> fixed;
    const std::vector<Player*> firstNamed = NamedBots(msg, candidates, 1);
    const bool someoneNamed = !firstNamed.empty();
    for (Player* bot : NamedBots(msg, pool, std::min<uint32_t>(cap, 3)))
        if (urand(0, 99) < g_DirectAddressReplyChance)
            fixed.push_back(bot);

    // ...then the bot they were just talking to -- unless they are plainly
    // asking everyone else ("anyone else about?"), which is exactly when the
    // last bot must NOT be the one to answer.
    bool aimedAtSomeoneElse = someoneNamed && fixed.empty();
    if (!someoneNamed && !openWords)
    {
        Player* aimed = nullptr;
        if (addressee.guid)
        {
            aimed = addressee.isBot ? FindInPool(pool, addressee.guid) : nullptr;
            aimedAtSomeoneElse = !aimed;   // a person, or a bot that cannot answer now
        }
        else
        {
            for (Player* bot : pool)
            {
                if (Governor_InConversation(bot->GetGUID(), player->GetGUID(), scopeKey))
                {
                    aimed = bot;
                    break;
                }
            }
        }

        if (aimed && urand(0, 99) < g_DirectAddressReplyChance)
            fixed.push_back(aimed);
    }

    // A small party is a conversation, not a crowd.
    bool smallParty = false;
    if (source == SRC_PARTY_LOCAL || source == SRC_RAID_LOCAL)
    {
        if (Group* group = player->GetGroup())
            smallParty = g_DirectAddressGroupSize > 0 &&
                         group->GetMembersCount() <= g_DirectAddressGroupSize;
    }

    CrowdLineKind kind = CrowdLineKind::Ambient;
    uint32_t total = 0;

    if (ack)
    {
        // At most the bot it was aimed at, and often nobody: "ty" is not a
        // conversation starter. A thank-you gets nobody at all
        // (Crowd.ThanksReplyChance): "grats" -> "ty" is finished.
        const uint32_t chance = OllamaHasThanksWord(msg) ? g_CrowdThanksReplyChance : g_CrowdAckReplyChance;
        if (fixed.empty() || urand(0, 99) >= chance)
            return;
        fixed.resize(1);
        kind  = CrowdLineKind::Aimed;
        total = 1;
    }
    else if (open)
    {
        // Busy channel: three to five answers. Quiet one: one or two.
        kind = CrowdLineKind::Open;
        const uint32_t low = std::max<uint32_t>(1, (cap + 1) / 2);
        total = std::max<uint32_t>(urand(low, cap), static_cast<uint32_t>(fixed.size()));
    }
    else if (!fixed.empty() || aimedAtSomeoneElse)
    {
        // Aimed at one bot (or at someone else): they answer, and now and
        // then somebody else chimes in.
        kind  = CrowdLineKind::Aimed;
        total = static_cast<uint32_t>(fixed.size());
        const uint32_t room     = cap > total ? cap - total : 0;
        const uint32_t maxExtra = std::min<uint32_t>(g_CrowdChimeInMax, room);
        for (uint32_t i = 0; i < maxExtra && urand(0, 99) < g_CrowdChimeInChance; ++i)
            ++total;
    }
    else if (smallParty)
    {
        total = urand(1, cap);
    }
    else if (greeting && PlayerReplyChanceFor(source) > 0)
    {
        // "hello" to the room: a few hellos back -- more in a busy city. (A
        // channel whose reply chance is set to 0 stays quiet for hellos too.)
        kind  = CrowdLineKind::Greeting;
        total = urand(1, hub ? cap : half);
    }
    else
    {
        if (urand(0, 99) >= PlayerReplyChanceFor(source))
            return;
        total = urand(1, (hub && g_CrowdFullCrowdInCities) ? cap : half);
    }

    std::vector<Player*> extras;
    if (total > fixed.size())
        extras = PickCrowdVoices(pool, fixed, total - static_cast<uint32_t>(fixed.size()));

    if (fixed.empty() && extras.empty())
        return;

    std::vector<Player*> voices = fixed;
    voices.insert(voices.end(), extras.begin(), extras.end());

    CrowdPlan plan = NewPlan(player, msg, source, channel, scopeKey, seq);
    plan.kind       = kind;
    plan.shouted    = shouted;
    plan.listeners  = listeners;
    if (kind == CrowdLineKind::Aimed)
        plan.aimedAt = !fixed.empty() ? fixed.front()->GetName()
                     : someoneNamed   ? firstNamed.front()->GetName()
                                      : addressee.name;

    for (Player* bot : fixed)
    {
        CrowdSlot slot;
        slot.botGuid   = bot->GetGUID().GetRawValue();
        slot.addressed = true;
        plan.slots.push_back(slot);
    }
    for (Player* bot : extras)
    {
        CrowdSlot slot;
        slot.botGuid = bot->GetGUID().GetRawValue();
        plan.slots.push_back(slot);
    }

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Crowd for {}'s line in {} ({}{}, {} listening, room for {}): [{}]",
                 player->GetName(), ChatChannelSourceLocalStr[source], CrowdKindName(kind),
                 shouted ? ", shouted" : "", listeners, cap, CrowdNames(plan));

    const std::string key = plan.key;
    Crowd_Start(std::move(plan));

    // The chat may carry on after the crowd: those who answered join any
    // conversation running here, and a line to the room may start one.
    Conversation_OnPlayerCrowd(player, msg, seq, source, channel, scopeKey, key, voices,
                               hub, kind != CrowdLineKind::Aimed);
}

static void HandleCrowdBotLine(Player* bot, const std::string& msg,
                               ChatChannelSourceLocal source, Channel* channel,
                               const std::string& scopeKey, uint64_t seq,
                               const ContextAddressee& addressee,
                               const std::vector<Player*>& candidates,
                               uint8_t chainDepth, uint64_t crowdPlanId)
{
    // chainDepth counts the hops to this line's replies; the first answer to
    // a line is not decayed, each answer to an answer is.
    const uint8_t hops = chainDepth > 0 ? static_cast<uint8_t>(chainDepth - 1) : 0;
    const uint64_t self = bot->GetGUID().GetRawValue();

    const std::string clean = Context_CleanText(msg);

    // A line that was one voice of a crowd stays inside that crowd. Looked up
    // by id, not by where it was said: a voice standing over a zone border is
    // still part of it. Once the crowd has ended or been replaced its late
    // lines start nothing at all -- that is what keeps a chain from escaping.
    if (crowdPlanId != 0)
    {
        // A conversation turn or a command acknowledgement starts nothing of
        // its own: the conversation decides who speaks next.
        CrowdPlan* plan = Crowd_FindById(crowdPlanId);
        if (!plan || plan->closed || plan->conversationId != 0 ||
            plan->replyBacks >= g_CrowdMaxReplyBacks)
            return;

        // "ty Lahu" from a bot does not need an answer back.
        std::vector<std::string> names;
        for (uint64_t guid : plan->answeredGuids)
            if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)))
                names.push_back(p->GetName());
        if (plan->speakerIsBot)
            names.push_back(plan->speakerName);
        if (CrowdText::Classify(clean, g_CrowdAckWords, g_CrowdOpenCallPhrases, names).ack)
            return;

        // Only an answer-back: a bot this line names, who has already spoken
        // in this crowd, may answer it.
        Player* target = nullptr;
        size_t  bestPos = std::string::npos;
        auto consider = [&](uint64_t guid)
        {
            if (guid == 0 || guid == self)
                return;
            Player* other = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            if (!other || !OllamaIsBotPlayer(other))
                return;
            const size_t pos = OllamaFindBotNameMention(msg, other->GetName());
            if (pos != std::string::npos && (bestPos == std::string::npos || pos < bestPos))
            {
                bestPos = pos;
                target  = other;
            }
        };
        for (uint64_t guid : plan->answeredGuids)
            consider(guid);
        if (plan->speakerIsBot)
            consider(plan->speakerGuid);

        // Two bots already trading lines: the one just answered may answer
        // back without being named.
        if (!target && plan->kind == CrowdLineKind::BotLine &&
            addressee.guid && addressee.isBot && addressee.guid != self)
        {
            const bool participant = addressee.guid == plan->speakerGuid ||
                std::find(plan->answeredGuids.begin(), plan->answeredGuids.end(), addressee.guid) !=
                    plan->answeredGuids.end();
            if (participant)
                target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(addressee.guid));
        }

        if (!target)
            return;

        const uint32_t chance = Governor_ApplyChainDecay(g_ContextBotAddressedChance, hops);
        if (urand(0, 99) >= chance)
            return;

        CrowdSlot slot;
        slot.botGuid    = target->GetGUID().GetRawValue();
        slot.addressed  = true;
        slot.replyBack  = true;
        slot.chainDepth = chainDepth;
        slot.answerGuid = self;
        slot.answerText = msg;
        slot.answerSeq  = seq;

        const size_t at = std::min(plan->next, plan->slots.size());
        plan->slots.insert(plan->slots.begin() + static_cast<std::ptrdiff_t>(at), std::move(slot));
        ++plan->replyBacks;
        Crowd_NoteReplyBack();

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Crowd: {} will answer {} back (chance {}%).",
                     target->GetName(), bot->GetName(), chance);
        return;
    }

    // An ambient line: random chatter, or anything else a bot said on its own.
    // If a crowd or a conversation is already going on here, it does not
    // start another (a running conversation has already taken the line in).
    // A /say conversation is only the people in earshot of it: a line across
    // the zone may still get its answer.
    const std::string key = CrowdKeyFor(scopeKey, source, bot);
    if (Crowd_Find(key) || Conversation_IsActiveNear(key, bot))
        return;

    const std::vector<Player*> pool = CrowdPool(bot, source, candidates);
    if (pool.empty())
        return;

    if (CrowdText::Classify(clean, g_CrowdAckWords, g_CrowdOpenCallPhrases, NamesOf(pool)).ack)
        return;

    Player* aimed = nullptr;
    {
        const std::vector<Player*> named = NamedBots(msg, pool, 1);
        if (!named.empty())
            aimed = named.front();
        else if (addressee.guid && addressee.isBot)
            aimed = FindInPool(pool, addressee.guid);
    }

    // Cities and inns talk more: a line there is likelier to get an answer.
    const bool hub = (source == SRC_GENERAL_LOCAL || source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL) &&
                     OllamaIsSocialHub(bot);
    uint32_t base = BotLineReplyChanceFor(source);
    if (hub)
        base = std::min<uint32_t>(100, base + g_CityBotLineReplyBonus);

    uint32_t chance = Governor_ApplyChainDecay(base, hops);
    if (aimed)
        chance = std::max(chance, Governor_ApplyChainDecay(g_ContextBotAddressedChance, hops));
    if (urand(0, 99) >= chance)
        return;

    std::vector<Player*> fixed;
    if (aimed)
        fixed.push_back(aimed);

    const uint32_t listeners = static_cast<uint32_t>(pool.size());
    const uint32_t cap = CrowdText::Capacity(listeners, g_CrowdBotsPerResponder, g_CrowdMaxResponders);
    uint32_t total = 1;
    if (cap >= 2 && urand(0, 99) < g_CrowdBotLineSecondChance)
        total = 2;

    std::vector<Player*> extras;
    if (total > fixed.size())
        extras = PickCrowdVoices(pool, fixed, total - static_cast<uint32_t>(fixed.size()));

    if (fixed.empty() && extras.empty())
        return;

    CrowdPlan plan = NewPlan(bot, msg, source, channel, scopeKey, seq);
    plan.kind      = CrowdLineKind::BotLine;
    plan.listeners = listeners;

    for (Player* p : fixed)
    {
        CrowdSlot slot;
        slot.botGuid    = p->GetGUID().GetRawValue();
        slot.addressed  = true;
        slot.chainDepth = chainDepth;
        plan.slots.push_back(slot);
    }
    for (Player* p : extras)
    {
        CrowdSlot slot;
        slot.botGuid    = p->GetGUID().GetRawValue();
        slot.chainDepth = chainDepth;
        plan.slots.push_back(slot);
    }

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Crowd for bot {}'s line in {} (depth {}, {} listening): [{}]",
                 bot->GetName(), ChatChannelSourceLocalStr[source], chainDepth, listeners, CrowdNames(plan));

    // Bots read before they answer, too.
    const uint32_t lo = std::min(g_CrowdGapMinMs, g_CrowdGapMaxMs);
    const uint32_t hi = std::max(g_CrowdGapMinMs, g_CrowdGapMaxMs);
    Crowd_Start(std::move(plan), lo == hi ? lo : urand(lo, hi));

    // Two bots talking may become a conversation others can join.
    std::vector<Player*> voices = fixed;
    voices.insert(voices.end(), extras.begin(), extras.end());
    Conversation_OnBotLineCrowd(bot, msg, seq, source, channel, scopeKey, key, voices, hub);
}

std::vector<Player*> Crowd_PickVoices(const std::vector<Player*>& pool,
                                      const std::vector<Player*>& fixed, uint32_t count)
{
    return PickCrowdVoices(pool, fixed, count);
}

bool Crowd_FireSlot(CrowdPlan& plan, size_t index)
{
    if (index >= plan.slots.size())
        return false;

    const CrowdSlot slot = plan.slots[index];   // a copy: answer-backs may insert later

    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(slot.botGuid));
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return false;
    if (g_DisableRepliesInCombat && bot->IsInCombat())
        return false;

    const uint64_t answerGuid = slot.replyBack ? slot.answerGuid : plan.speakerGuid;
    Player* speaker = ObjectAccessor::FindConnectedPlayer(ObjectGuid(answerGuid));
    if (!speaker || !speaker->IsInWorld() || speaker == bot)
        return false;

    // A crowd takes a minute. People move, leave channels and drop group in
    // that time, so who can still hear the line -- and whether a person can
    // still see the answer -- is checked again now.
    OllamaWorldSnapshot world;
    world.Build();

    switch (plan.source)
    {
        case SRC_GENERAL_LOCAL:
        {
            ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
            Channel* channel = mgr ? mgr->GetChannel(plan.channelName, bot) : nullptr;
            if (!channel || !OllamaIsOnChannel(channel, bot->GetGUID()) || !world.RealPlayerInChannel(channel))
                return false;
            break;
        }

        case SRC_SAY_LOCAL:
        case SRC_YELL_LOCAL:
        {
            const float range = (plan.source == SRC_SAY_LOCAL) ? g_SayDistance : g_YellDistance;
            if (!IsBotEligibleForChatChannelLocal(bot, speaker, plan.source, nullptr, nullptr) ||
                !world.RealPlayerWithin(bot, range) ||
                Roleplay_IsLanguageBarrier(speaker, bot))
                return false;
            break;
        }

        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:
            if (!IsBotEligibleForChatChannelLocal(bot, speaker, plan.source, nullptr, nullptr) ||
                !OllamaGroupHasRealPlayer(bot))
                return false;
            break;

        case SRC_GUILD_LOCAL:
        case SRC_OFFICER_LOCAL:
            if (!IsBotEligibleForChatChannelLocal(bot, speaker, plan.source, nullptr, nullptr) ||
                !world.GuildHasRealPlayer(bot->GetGuildId()))
                return false;
            break;

        default:
            return false;
    }

    if (!Governor_CanSendPaced(bot->GetGUID(), plan.scopeKey, slot.addressed))
        return false;

    // Who has already answered, as far as this bot could hear: in /say a
    // voice thirty yards off is not in its transcript, so it must not be told
    // to "see the chat above" for it.
    std::vector<std::string> heard;
    if (plan.source == SRC_SAY_LOCAL || plan.source == SRC_YELL_LOCAL)
    {
        const float range = (plan.source == SRC_SAY_LOCAL) ? g_SayDistance : g_YellDistance;
        for (size_t i = 0; i < plan.answeredGuids.size() && i < plan.answeredBy.size(); ++i)
        {
            Player* voice = ObjectAccessor::FindConnectedPlayer(ObjectGuid(plan.answeredGuids[i]));
            if (voice && voice->IsInWorld() && voice->GetMapId() == bot->GetMapId() &&
                bot->IsWithinDistInMap(voice, range))
                heard.push_back(plan.answeredBy[i]);
        }
    }
    else
    {
        heard = plan.answeredBy;
    }

    // The first voice answers the line itself. Later ones see it in the
    // transcript, in order, with the answers that came after it.
    const bool followUp = !slot.replyBack && !heard.empty();
    const std::string& message = slot.replyBack ? slot.answerText : plan.message;
    const uint64_t excludeSeq  = slot.replyBack ? slot.answerSeq : (followUp ? 0 : plan.messageSeq);

    const std::string speakerName  = speaker->GetName();
    const std::string listenerText = CrowdText::ListenerPhrase(plan.listeners);
    const std::string answered     = CrowdText::JoinNames(heard);
    auto hintFrom = [&](const std::string& templ) -> std::string
    {
        if (templ.empty())
            return {};
        std::string out = SafeFormat(templ,
                                     fmt::arg("player_name", speakerName),
                                     fmt::arg("replied_to", speakerName),
                                     fmt::arg("addressee", plan.aimedAt),
                                     fmt::arg("listeners", listenerText),
                                     fmt::arg("answered", answered),
                                     fmt::arg("pass_token", g_ContextPassToken));
        return out == "[Format Error]" ? std::string() : out;
    };

    std::string hint;
    if (!slot.hint.empty())
        hint = slot.hint;                           // a conversation turn or an instruction, written for this voice
    else if (slot.replyBack)
        hint = hintFrom(g_CrowdHintReplyBack);
    else if (slot.addressed)
        hint = g_ContextHintAddressed;
    else if (followUp)
        hint = hintFrom(g_CrowdHintFollowUp);
    else if (plan.kind == CrowdLineKind::Open || plan.kind == CrowdLineKind::Greeting)
        hint = hintFrom(g_CrowdHintOpenCall);
    else if (plan.kind == CrowdLineKind::Aimed && !plan.aimedAt.empty())
        hint = hintFrom(g_ContextHintBystander);   // meant for someone else, who has not answered
    else
        hint = hintFrom(g_ContextHintOpen);

    if (plan.shouted && !slot.replyBack && slot.hint.empty())
    {
        const std::string shout = hintFrom(g_CrowdHintShout);
        if (!shout.empty())
            hint += (hint.empty() ? "" : " ") + shout;
    }

    std::string prompt = GenerateBotPrompt(bot, message, speaker, plan.source, plan.channelName,
                                           excludeSeq, hint);
    if (prompt.empty())
        return false;

    const bool toPerson = OllamaIsRealPlayer(speaker);

    OllamaChatRequest request;
    request.botGuid       = slot.botGuid;
    request.targetGuid    = answerGuid;
    request.source        = plan.source;
    request.channelName   = plan.channelName;
    request.channelId     = plan.channelId;
    request.chainDepth    = slot.chainDepth;
    request.directAddress = slot.addressed;
    request.scopeKey      = plan.scopeKey;
    request.prompt        = std::move(prompt);
    request.botName       = bot->GetName();
    request.originMessage = message;
    request.kind = (g_RoleplayEnable && g_RoleplayStrictness >= 1)
                       ? OllamaRequestKind::RoleplayReply
                       : OllamaRequestKind::ChatReply;
    request.triggerBotReplies = true;
    request.recordHistory     = toPerson;
    // One sentiment read per line, by the bot it was aimed at -- not one per
    // voice in the crowd, each of which would be its own LLM call.
    request.updateSentiment   = toPerson && slot.addressed && g_EnableSentimentTracking;
    request.crowdPlanId       = plan.id;
    request.crowdSlot         = static_cast<uint32_t>(index);
    // The tank's answer is slot 0; a mage answering "mb, water pls" after it
    // (mod-ollama-chat_conjure) is not an instruction ack.
    request.partyAck          = (plan.kind == CrowdLineKind::Command && index == 0);
    request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);

    if (!OllamaDispatch_Submit(std::move(request)))
        return false;

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Crowd voice {}/{}: {} answers {}{}.",
                 index + 1, plan.slots.size(), bot->GetName(), speakerName,
                 slot.replyBack ? " (answering back)" : (followUp ? " (after others)" : ""));
    return true;
}

// Personality keys are matched upper-case, the way the per-style settings
// are stored.
static std::string OllamaUpperKey(const std::string& key)
{
    std::string upper;
    upper.reserve(key.size());
    for (char c : key)
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper;
}

uint32_t OllamaMaxReplyLengthFor(Player* bot)
{
    if (!bot || g_PersonalityMaxReplyLength.empty())
        return 0;
    const auto it = g_PersonalityMaxReplyLength.find(OllamaUpperKey(GetBotPersonality(bot)));
    return it == g_PersonalityMaxReplyLength.end() ? 0 : it->second;
}

// What a chat style is told on top of its personality, for one reply. A nudge
// rolls each time, so "90|quote a film" is a film quote nine replies in ten
// and, with an "otherwise" part, something plainly different on the tenth.
static std::string OllamaPersonalityNote(const std::string& personality)
{
    if (g_PersonalityNudges.empty() && g_PersonalityMaxReplyLength.empty())
        return {};
    const std::string key = OllamaUpperKey(personality);

    std::string out;
    const auto nudge = g_PersonalityNudges.find(key);
    if (nudge != g_PersonalityNudges.end())
    {
        const PersonalityNudge& n = nudge->second;
        out = (n.percent > 0 && urand(1, 100) <= n.percent) ? n.yes : n.no;
    }

    // Only when it really is longer than everyone else's limit.
    const auto longer = g_PersonalityMaxReplyLength.find(key);
    if (longer != g_PersonalityMaxReplyLength.end() && longer->second > g_MaxReplyLength &&
        !g_PersonalityLongReplyNote.empty())
    {
        const std::string note = SafeFormat(g_PersonalityLongReplyNote, fmt::arg("words", longer->second / 6));
        if (note != "[Format Error]")
            out += (out.empty() ? "" : " ") + note;
    }
    return out;
}

std::string GenerateBotPrompt(Player* bot, std::string playerMessage, Player* player,
                              ChatChannelSourceLocal source, const std::string& channelName,
                              uint64_t contextExcludeSeq, const std::string& replyHint)
{
    if (!bot || !player) {
        return "";
    }
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (botAI == nullptr) {
        return "";
    }
    ChatHelper* helper = botAI->GetChatHelper();
    if (helper == nullptr) {
        return "";
    }
    if (g_ChatPromptTemplate.empty()) {
        LOG_ERROR("module.ollamachat", "[Ollama Chat] GenerateBotPrompt: template is empty");
        return "";
    }

    // What they typed, as it reads on screen: a link becomes its "[Name]".
    // The model never sees client markup, so it never echoes broken markup.
    playerMessage = TextRules::StripChatMarkup(playerMessage);

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    uint64_t botGuid                = bot->GetGUID().GetRawValue();
    uint64_t playerGuid             = player->GetGUID().GetRawValue();

    std::string personality         = GetBotPersonality(bot);
    std::string personalityPrompt   = GetPersonalityPromptAddition(personality);
    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = OllamaContinentName(bot);
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botRole             = CleanRoleForPrompt(ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot)));
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGuild            = (bot->GetGuild() ? bot->GetGuild()->GetName() : "No Guild");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;

    std::string playerName          = player->GetName();
    uint32_t playerLevel            = player->GetLevel();
    std::string playerClass         = botAI->GetChatHelper()->FormatClass(player->getClass());
    std::string playerRace          = botAI->GetChatHelper()->FormatRace(player->getRace());
    // Their spec as well as their class: anyone can guess it from the gear
    // they wear and the spells they cast.
    std::string playerRole          = CleanRoleForPrompt(ChatHelper::FormatClass(player, AiFactory::GetPlayerSpecTab(player)));
    uint8_t playerGenderByte        = player->getGender();
    std::string playerGender        = (playerGenderByte == 0 ? "Male" : "Female");
    std::string playerFaction       = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string playerGuild         = (player->GetGuild() ? player->GetGuild()->GetName() : "No Guild");
    std::string playerGroupStatus   = (player->GetGroup() ? "In a group" : "Solo");
    uint32_t playerGold             = player->GetMoney() / 10000;
    float playerDistance            = player->IsInWorld() && bot->IsInWorld() ? player->GetDistance(bot) : -1.0f;

    std::string chatHistory         = TextRules::StripChatMarkup(GetBotHistoryPrompt(botGuid, playerGuid, playerMessage));
    std::string sentimentInfo       = GetSentimentPromptAddition(bot, player);

    // What everyone in this channel said lately, not only this bot's own past
    // exchanges with this person. When it has lines, the per-pair history
    // would mostly repeat them, so it is dropped.
    std::string channelHistory = Context_BuildTranscript(bot, source, channelName, contextExcludeSeq);
    if (!channelHistory.empty() && g_ContextSuppressPairHistory)
        chatHistory.clear();

    // Retrieve RAG information if enabled
    std::string ragInfo;
    if (g_EnableRAG && g_RAGSystem) {
        auto ragResults = g_RAGSystem->RetrieveRelevantInfo(playerMessage, g_RAGMaxRetrievedItems, g_RAGSimilarityThreshold);
        std::string ragContent = g_RAGSystem->GetFormattedRAGInfo(ragResults);
        if (!ragContent.empty()) {
            ragInfo = SafeFormat(g_RAGPromptTemplate, fmt::arg("rag_info", ragContent));
        }
        if (g_DebugEnabled) {
            LOG_INFO("module.ollamachat", "[Ollama Chat] RAG Debug - Enabled: {}, System: {}, Message: '{}', Results: {}, Content length: {}",
                g_EnableRAG, (void*)g_RAGSystem, playerMessage, ragResults.size(), ragContent.length());
        }
    } else if (g_DebugEnabled) {
        LOG_INFO("module.ollamachat", "[Ollama Chat] RAG Debug - Not enabled or no system - Enabled: {}, System: {}",
            g_EnableRAG, (void*)g_RAGSystem);
    }

    std::string extraInfo = SafeFormat(
        g_ChatExtraInfoTemplate,
        fmt::arg("bot_race", botRace),
        fmt::arg("bot_gender", botGender),
        fmt::arg("bot_role", botRole),
        fmt::arg("bot_faction", botFaction),
        fmt::arg("bot_guild", botGuild),
        fmt::arg("bot_group_status", botGroupStatus),
        fmt::arg("bot_gold", botGold),
        fmt::arg("player_race", playerRace),
        fmt::arg("player_gender", playerGender),
        fmt::arg("player_role", playerRole),
        fmt::arg("player_faction", playerFaction),
        fmt::arg("player_guild", playerGuild),
        fmt::arg("player_group_status", playerGroupStatus),
        fmt::arg("player_gold", playerGold),
        fmt::arg("player_distance", playerDistance),
        fmt::arg("bot_area", botAreaName),
        fmt::arg("bot_zone", botZoneName),
        fmt::arg("bot_map", botMapName)
    );
    
    std::string prompt = SafeFormat(
        g_ChatPromptTemplate,
        fmt::arg("bot_name", botName),
        fmt::arg("bot_level", botLevel),
        fmt::arg("bot_class", botClass),
        fmt::arg("bot_personality", personalityPrompt),
        fmt::arg("bot_personality_name", personality),
        fmt::arg("player_level", playerLevel),
        fmt::arg("player_class", playerClass),
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage),
        fmt::arg("extra_info", extraInfo),
        fmt::arg("chat_history", chatHistory),
        fmt::arg("sentiment_info", sentimentInfo),
        fmt::arg("channel_history", channelHistory),
        fmt::arg("reply_hint", replyHint),
        fmt::arg("pass_token", g_ContextPassToken)
    );

    // Templates written before {channel_history} existed still get it, in
    // front, where it reads as "here is the chat so far".
    if (!channelHistory.empty() &&
        g_ChatPromptTemplate.find("{channel_history}") == std::string::npos)
    {
        prompt = channelHistory + prompt;
    }

    // Add RAG information to the prompt if available
    if (!ragInfo.empty()) {
        prompt += ragInfo + "\n";
    }

    if(g_EnableChatBotSnapshotTemplate)
    {
        prompt += GenerateBotGameStateSnapshot(bot);
    }

    // What this bot remembers, and how it feels about people. Bounded by
    // their own token budgets, so this cannot grow the prompt without limit.
    prompt += Memory_BuildPromptSection(bot, player);

    // Race and class as a voice rather than as a stat line.
    prompt += Roleplay_BuildVoicePrompt(bot);

    // What a player of this level talks about, which roles this class can
    // fill, and what anyone can know about someone else.
    prompt += Outlook_ForBot(bot);
    prompt += Links_PromptHint();

    // Let the model gesture. The tag is parsed back out and stripped before
    // the line is spoken, so it never reaches chat as text.
    prompt += Expression_BuildGesturePrompt(bot);

    // In a dungeon party, "mb" is a mana break, not "my bad".
    const std::string glossary = PartyCmd_GlossaryFor(bot, source);
    if (!glossary.empty())
        prompt += " " + glossary;

    // ...and in a dungeon-clear run, the bots know what the run is doing, so
    // "which boss is next?" gets a real answer.
    const std::string runState = PartyCmd_RunStateFor(bot, source);
    if (!runState.empty())
        prompt += " " + runState;

    // A dead bot knows it is dead, so "respawn ffs" is about it.
    if (bot->isDead())
    {
        const std::string& state = bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? g_StateGhostText : g_StateDeadText;
        if (!state.empty())
            prompt += " " + state;
    }
    else
    {
        // ...and one told to stay put knows why it is not following.
        const std::string staying = Orders_StateTextFor(bot);
        if (!staying.empty())
            prompt += " " + staying;
    }

    // What this chat style is told on top of its personality, this time: a
    // nudge on some share of its replies (Personality.Nudge.<KEY>), or that it
    // may write more than one short line (Personality.MaxReplyLength.<KEY>).
    const std::string styleNote = OllamaPersonalityNote(personality);
    if (!styleNote.empty())
        prompt += " " + styleNote;

    // Last, so it is the freshest instruction: is this bot being spoken to?
    if (!replyHint.empty() && g_ChatPromptTemplate.find("{reply_hint}") == std::string::npos)
        prompt += " " + replyHint;

    // Debug logging for full prompt including RAG information
    if (g_DebugEnabled && g_DebugShowFullPrompt) {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Full prompt sent to bot {} for player {}: {}", botName, playerName, prompt);
    }

    return prompt;
}

// --------------------------------------------------------------------------
// Emote reactions
// --------------------------------------------------------------------------

std::string BuildEmoteReactionPrompt(Player* bot, Player* player, uint32_t textEmote)
{
    if (!bot || !player)
        return "";

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI || !botAI->GetChatHelper())
        return "";

    std::string emoteName = LookupTextEmoteName(textEmote);
    if (emoteName.empty())
        emoteName = "gestures at";

    const std::string personality       = GetBotPersonality(bot);
    const std::string personalityPrompt = GetPersonalityPromptAddition(personality);

    std::string prompt = SafeFormat(
        g_EmoteReactionPromptTemplate,
        fmt::arg("bot_name", bot->GetName()),
        fmt::arg("bot_level", bot->GetLevel()),
        fmt::arg("bot_class", botAI->GetChatHelper()->FormatClass(bot->getClass())),
        fmt::arg("bot_race", botAI->GetChatHelper()->FormatRace(bot->getRace())),
        fmt::arg("bot_personality", personalityPrompt),
        fmt::arg("bot_personality_name", personality),
        fmt::arg("player_name", player->GetName()),
        fmt::arg("player_class", botAI->GetChatHelper()->FormatClass(player->getClass())),
        fmt::arg("player_race", botAI->GetChatHelper()->FormatRace(player->getRace())),
        fmt::arg("emote_name", emoteName));

    if (g_RoleplayEnable)
        prompt += Roleplay_BuildVoicePrompt(bot);

    return prompt;
}

// --------------------------------------------------------------------------
// Maintenance
// --------------------------------------------------------------------------

void OllamaChatMaintenance::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    // These maps used to grow for the lifetime of the process, and the event
    // cooldown one was keyed on a raw Player* that could be recycled by a
    // different character at the same address.
    const ObjectGuid guid = player->GetGUID();

    Governor_OnPlayerLogout(guid);
    Memory_ForgetBot(guid);
    OllamaRandomChatter_ForgetBot(guid);
    Topics_ForgetBot(guid);
    Context_ForgetPlayer(guid.GetRawValue());
    Conversation_ForgetPlayer(guid.GetRawValue());
}
