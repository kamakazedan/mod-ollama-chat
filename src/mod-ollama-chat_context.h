#ifndef MOD_OLLAMA_CHAT_CONTEXT_H
#define MOD_OLLAMA_CHAT_CONTEXT_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>
#include <vector>

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
// `target`: who it was aimed at, for an emote -- a line the actor types next
// is then taken to be aimed at them (Context_LikelyAddressee). Reads nothing
// of anyone but the actor and the target, under its own lock: safe on the
// map thread both are on. `guildId`, when set, is the guild transcript a
// guild note goes to instead of the actor's own: the guild an event happened
// in, handled a tick later, when a guild leave has cleared the actor's.
void Context_RecordEvent(Player* actor, const std::string& note, bool toGuild, Player* target = nullptr,
                         uint32_t guildId = 0);

// Sequence number of the newest line `speaker` has in that space, or 0.
uint64_t Context_LatestLineFrom(Player* speaker, ChatChannelSourceLocal source,
                                const std::string& channelName);

// The transcript of that space as `viewer` would have seen it, oldest first,
// leaving out line `excludeSeq` (the one being answered; 0 = none). Ends with a
// blank line so it can be put in front of a prompt. Empty when there is nothing
// recent.
std::string Context_BuildTranscript(Player* viewer, ChatChannelSourceLocal source,
                                    const std::string& channelName, uint64_t excludeSeq);

// The newest lines of that space that `viewer` could have seen, newest first:
// at most `max`, none older than `maxAgeSeconds`, events left out.
struct ContextRecentLine
{
    uint64_t    speakerGuid  = 0;
    std::string speakerName;
    bool        speakerIsBot = false;
    std::string text;
};
std::vector<ContextRecentLine> Context_RecentLines(Player* viewer, ChatChannelSourceLocal source,
                                                   const std::string& channelName, size_t max,
                                                   uint32_t maxAgeSeconds);

// The channel as a prompt names it: "party chat", "guild chat", "General"
// (for "General - Elwynn Forest"), "/say nearby".
std::string Context_ChannelLabel(ChatChannelSourceLocal source, const std::string& channelName);

// The sequence number the next line, anywhere, will get. An unprompted line
// notes it when it is asked for (OllamaChatRequest::writtenAtSeq).
uint64_t Context_NextSeq();

// How busy a channel has really been: the lines said in `channelName` (its
// full name, "Trade - City", "General - Elwynn Forest") on side `team` in the
// last `withinSeconds` (an hour at most), by people and bots alike. What people
// type there is noted by the chat hook (Context_NoteChannelLine), what this
// module's bots say there by Context_RecordLine; a line neither sees -- one of
// playerbots' own canned ones -- is not counted. Kept with Context.Enable off
// too. For Outlook_ChatState: "trade chat is a warzone" had been said on a
// realm where nobody had typed in Trade all evening.
uint32_t Context_ChannelLines(uint32_t team, const std::string& channelName, uint32_t withinSeconds);

// Someone said something in that channel, for Context_ChannelLines.
void Context_NoteChannelLine(uint32_t team, const std::string& channelName);

// Did a real player say something in that space that `viewer` could hear, as
// line `fromSeq` or later (0: any line) and no more than `withinSeconds` ago
// (0: however long ago)?
//
// "*luck intensifies*" after a roll of 1 was followed in party by "this place
// is a proper maze": a bot's unprompted line, written before the joke and
// sent after it. An unprompted line now waits Context.AmbientHoldSeconds
// after a real player speaks where it would go, and one written before they
// spoke is not sent at all.
bool Context_HumanSpokeSince(Player* viewer, ChatChannelSourceLocal source, const std::string& channelName,
                             uint64_t fromSeq, uint32_t withinSeconds);

// For a reply prompt of `viewer`: what line `seq` by `speaker` came right
// after (Context.LeadUpText) -- the line before their turn, someone else's
// or an event, and the event that line was about when it came just after one:
//
//   Kami typed that right after this happened: loot roll for Gloves of the
//   Fang (...) -- Lanik won it, and then you said "1 out of 100 is a proper
//   joke.", so it is most likely about that.
//
// The transcript has all of it, but further up, as background: a short line
// means little on its own, and the model answered "*luck intensifies*" as if
// it came out of nowhere. So only for lines of up to Context.LeadUpMaxWords
// words, and only what came within Context.ReplyWindowSeconds before it.
// Empty otherwise.
std::string Context_LeadUpText(Player* viewer, Player* speaker, ChatChannelSourceLocal source,
                               const std::string& channelName, uint64_t seq);

struct ContextThreadStats
{
    uint64_t leadUps = 0;   // reply prompts told what a short line came right after
};
ContextThreadStats Context_GetThreadStats();

struct ContextAddressee
{
    uint64_t    guid  = 0;      // 0 = said to the room, no clear addressee
    bool        isBot = false;
    std::string name;
};

// Who line `seq` by `speaker` was most likely aimed at: the latest other
// speaker (or event subject) in that space within ReplyWindowSeconds of the
// speaker's turn. The speaker's own consecutive lines count as one turn --
// unless one of them is an emote at someone, who is then the one spoken to.
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

// Text smileys -- ":)", ":D", "xD", ">.<", "<3" -- now and then. People use
// them, but not in every line, and not all at once: a reply keeps at most
// one (Emoticons.Tokens), and none when this bot used one in the last
// Emoticons.BotCooldownMinutes or one is already among the last
// Emoticons.ScopeLines lines where it is going. Returns the line as it may
// go out; empty when nothing worth saying is left.
std::string Context_RationEmoticons(Player* bot, ChatChannelSourceLocal source,
                                    const std::string& channelName, const std::string& text);

// A line went out: a smiley in it starts this bot's Emoticons.BotCooldownMinutes.
// (Noted once delivered, so a line dropped after rationing costs it nothing.)
void Context_NoteEmoticonSent(Player* bot, const std::string& text);

// For a prompt, rarely: that a smiley would do in this one (Emoticons.NudgeText,
// with a couple of Emoticons.Offer picked at random), by the bot's style's
// chance (Emoticons.StyleChance, else Emoticons.NudgeChance). Usually empty.
std::string Context_EmoticonNudgeFor(Player* bot);

// The visible text of a chat line: item/quest/spell link markup reduced to
// "[Name]", colour codes and line breaks removed, length capped.
std::string Context_CleanText(const std::string& text);

// Housekeeping.
void Context_Update();                  // prune old lines (maintenance tick)
void Context_ForgetPlayer(uint64_t guid);   // logout
void Context_Reset();

#endif // MOD_OLLAMA_CHAT_CONTEXT_H
