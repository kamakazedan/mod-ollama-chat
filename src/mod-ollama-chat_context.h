#ifndef MOD_OLLAMA_CHAT_CONTEXT_H
#define MOD_OLLAMA_CHAT_CONTEXT_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Shared chat context.
//
// A bot used to see only its own earlier exchanges with whoever was talking to
// it (the per-pair {chat_history}). It never saw what anyone else had said in
// the channel, so a bot that had not been part of a conversation answered the
// latest line as though it were the first thing anybody had said -- "idk what
// you mean lol" to the second half of somebody else's exchange.
//
// This keeps a short rolling transcript per conversation space -- one party,
// one guild, one channel instance, one map for /say -- of what people and bots
// said there, plus a few game events (level-ups, deaths, duels, guildmates
// coming online) so that a "gz" can be tied to whoever dinged.
//
// It also answers "who was that line most likely aimed at?": in chat, a line
// typed right after someone else's is usually an answer to them. The reply
// logic uses that to let the right bot answer and keep the others out of it.
//
// World thread only, like everything else that touches a Player.
// --------------------------------------------------------------------------

enum class ContextLineKind : uint8_t
{
    Human,
    Bot,
    Event,
};

// Record what `speaker` said. channelName is the full channel name for
// SRC_GENERAL_LOCAL ("General - Elwynn Forest") and ignored otherwise.
// Returns the line's sequence number, or 0 when nothing was recorded (feature
// off, whisper, a source with no shared space, empty text).
uint64_t Context_RecordLine(Player* speaker, ChatChannelSourceLocal source,
                            const std::string& channelName, const std::string& text,
                            ContextLineKind kind);

// Record a game event about `actor`, e.g. "Gyku reached level 15", where the
// people who could have noticed it will see it: the actor's party and the
// /say area around them, or the guild transcript when toGuild is set.
void Context_RecordEvent(Player* actor, const std::string& note, bool toGuild);

// Sequence number of the newest line `speaker` has in that space, or 0.
uint64_t Context_LatestLineFrom(Player* speaker, ChatChannelSourceLocal source,
                                const std::string& channelName);

// The transcript of that space as `viewer` would have seen it, oldest first,
// leaving out line `excludeSeq` (the one being answered; 0 = none). Ends with a
// blank line so it can be put in front of a prompt. Empty when there is nothing
// recent.
std::string Context_BuildTranscript(Player* viewer, ChatChannelSourceLocal source,
                                    const std::string& channelName, uint64_t excludeSeq);

struct ContextAddressee
{
    uint64_t    guid  = 0;      // 0 = said to the room, no clear addressee
    bool        isBot = false;
    std::string name;
};

// Who line `seq` by `speaker` was most likely aimed at: the latest other
// speaker (or event subject) in that space within ReplyWindowSeconds of the
// speaker's turn. The speaker's own consecutive lines count as one turn.
ContextAddressee Context_LikelyAddressee(Player* speaker, ChatChannelSourceLocal source,
                                         const std::string& channelName, uint64_t seq);

// True when the model answered with the pass token instead of a chat line.
bool Context_IsPassReply(const std::string& text);

// Ration lol/omg/xD-style fillers in a bot's reply: at most MaxFillersPerLine
// per message, none that already appeared in the last FillerScopeLines lines
// of the space, and none this bot used within FillerBotCooldownSeconds.
//
// A reply that is nothing but a laugh ("lol", "haha") is a fine reaction once,
// so it survives when it passes those checks -- unless a bot's laugh-only
// line (its own or another's) is already among the last FillerScopeLines
// lines, which is how a crowd would otherwise turn one joke into lol, haha,
// lmao. What counts as a laugh is FillerWords, so with that empty all of this
// is off.
//
// Returns the adjusted line; empty when nothing worth saying is left.
std::string Context_RationFillers(Player* bot, ChatChannelSourceLocal source,
                                  const std::string& channelName, const std::string& text);

// The visible text of a chat line: item/quest/spell link markup reduced to
// "[Name]", colour codes and line breaks removed, length capped.
std::string Context_CleanText(const std::string& text);

// Housekeeping.
void Context_Update();                  // prune old lines (maintenance tick)
void Context_ForgetPlayer(uint64_t guid);   // logout
void Context_Reset();

#endif // MOD_OLLAMA_CHAT_CONTEXT_H
