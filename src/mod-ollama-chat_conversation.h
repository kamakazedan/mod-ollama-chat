#ifndef MOD_OLLAMA_CHAT_CONVERSATION_H
#define MOD_OLLAMA_CHAT_CONVERSATION_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>
#include <vector>

class Channel;
class Player;

// --------------------------------------------------------------------------
// Conversations.
//
// A crowd answers one line and stops. A conversation keeps going: a handful
// of bots talking in General, Trade or /say for a few minutes (MinMinutes to
// MaxMinutes, rolled when it starts), one line at a time, each typed after
// reading the chat so far. Others pipe up now and then, people drift off, and
// when its time is up it winds down the way people do -- a last opinion, a
// joke, "anyway gtg" -- rather than stopping mid-sentence.
//
// They start three ways:
//   * a real player says something to a channel and gets a crowd of answers
//     (Conversation.FromPlayerChance) -- the chat then carries on after the
//     crowd, with the player free to join in at any point;
//   * a bot's line gets answered by another bot (FromBotLineChance);
//   * in a city a bot opens one on purpose, about one of the Premises
//     (City.ConversationStartChance).
// Cities multiply the first two chances by City.ConversationBoost.
//
// Each turn is a one-voice crowd plan under the same key as the place's
// crowds, so a conversation and a crowd never talk over each other: when a
// player speaks, their crowd goes first and the conversation waits for it.
//
// /say has one key per zone, but a /say conversation is only the people in
// earshot of it: a line said elsewhere in the zone is not part of it.
//
// World thread only, except Conversation_ForgetPlayer.
// --------------------------------------------------------------------------

// Can a conversation happen here at all? General (and Trade with
// AllowTrade) channels, and /say with AllowSay.
bool Conversation_PlaceAllowed(ChatChannelSourceLocal source, uint32_t channelId);

// Is one running under this crowd key (Crowd_KeyFor)?
bool Conversation_IsActive(const std::string& key);

// ...and is `speaker` part of it? Everyone in a channel is; in /say, those in
// earshot of it.
bool Conversation_IsActiveNear(const std::string& key, Player* speaker);

// A person's line in a channel or /say just got a crowd of answers (`voices`).
// They join a conversation running there; with `mayStart` (a line to the
// room, not to one bot) the line may also start one.
void Conversation_OnPlayerCrowd(Player* player, const std::string& msg, uint64_t seq,
                                ChatChannelSourceLocal source, Channel* channel,
                                const std::string& scopeKey, const std::string& key,
                                const std::vector<Player*>& voices, bool inHub, bool mayStart);

// A bot's own line just got answered by other bots (`voices`): perhaps it
// turns into a conversation.
void Conversation_OnBotLineCrowd(Player* bot, const std::string& msg, uint64_t seq,
                                 ChatChannelSourceLocal source, Channel* channel,
                                 const std::string& scopeKey, const std::string& key,
                                 const std::vector<Player*>& voices, bool inHub);

// A bot opens one on purpose. The conversation waits for `starter`'s next line
// in this place (the opener, which the caller asks for) and goes on from it.
// False when one cannot start here now (running, cooling down, at MaxActive).
bool Conversation_BeginWithOpener(Player* starter, ChatChannelSourceLocal source,
                                  const std::string& channelName, uint32_t channelId,
                                  const std::string& scopeKey, const std::string& key,
                                  const std::string& premise, const std::vector<Player*>& partners);

// Would Conversation_BeginWithOpener accept one here right now?
bool Conversation_CanOpen(const std::string& key);

// The opening line Conversation_BeginWithOpener is waiting for will not come
// (the queue was full, the model said nothing usable, it was held back): the
// place is free again at once, with no cooldown.
void Conversation_OpenerLost(const std::string& key, uint64_t botGuid);

// Every line said where a conversation is running, people's and bots'.
// `addresseeGuid` is who a person's line answers (Context_LikelyAddressee),
// `crowdPlanId` the plan a bot's line was a voice of.
void Conversation_NoteLine(const std::string& key, Player* speaker, bool isBot,
                           const std::string& text, uint64_t seq,
                           uint64_t crowdPlanId, uint64_t addresseeGuid);

void Conversation_Update();                    // world tick, after Crowd_Update

// Logout. Safe from any thread: bots are logged out from map threads, so the
// guid is queued and dropped from the conversations on the next world tick.
void Conversation_ForgetPlayer(uint64_t guid);
void Conversation_Reset();

struct ConversationStats
{
    uint32_t active      = 0;
    uint64_t started     = 0;
    uint64_t byPlayers   = 0;
    uint64_t byBots      = 0;
    uint64_t turns       = 0;   // voices asked
    uint64_t lines       = 0;   // lines said in conversations, by anyone
    uint64_t joins       = 0;
    uint64_t woundDown   = 0;   // ended the natural way
    uint64_t fizzled     = 0;   // ran out of people or audience first
};
ConversationStats Conversation_GetStats();

#endif // MOD_OLLAMA_CHAT_CONVERSATION_H
