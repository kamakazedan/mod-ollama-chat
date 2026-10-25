#ifndef MOD_OLLAMA_CHAT_CROWD_H
#define MOD_OLLAMA_CHAT_CROWD_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

// --------------------------------------------------------------------------
// Crowd replies.
//
// A line said to a channel used to get exactly one answer. Once a bot had
// answered, the transcript made every following line "a reply to that bot",
// so the same bot answered everything -- even "anyone else about?". Real
// channels do not work like that: an open call gets a few answers from
// different people, each typed after reading the ones before it, and people
// react to each other.
//
// A crowd plan is the ordered list of bots that will answer one line. The
// first fires at once; each later one fires only after the previous one has
// landed or been dropped, after a short, human-sized pause, and builds its
// prompt then -- so it sees the replies before it and can laugh at them,
// disagree with them, or let it go.
//
// Bot lines go through the same machinery: a bot answering another bot is a
// one- or two-slot plan, and a crowd reply that names another bot in the same
// crowd can get an answer back. That is what replaces the old immediate
// bot-to-bot roll, which the per-scope cooldown silently blocked every time
// (the triggering bot's own send had just reserved the scope).
//
// World thread only, except Crowd_ReportWorkerFailure.
// --------------------------------------------------------------------------

// Pure text helpers, kept free of game types so they can be unit tested.
namespace CrowdText
{
    // Lowercase words, letters and digits only: "Who's here?" -> {whos, here}.
    std::vector<std::string> Words(const std::string& text);

    // Parse a '|'-separated list of words and phrases from config:
    // "anyone|who else|haha*" -> {{anyone}, {who, else}, {haha*}}.
    // A word ending in '*' matches any word that starts with the rest.
    std::vector<std::vector<std::string>> ParsePhrases(const std::string& raw);

    // Does `words` contain any of `phrases` as a contiguous run?
    bool ContainsPhrase(const std::vector<std::string>& words,
                        const std::vector<std::vector<std::string>>& phrases);

    // Every word is one of `ackWords` (single-word entries) -- "ty", "lol",
    // "ok np" -- or the line has no words at all (":)", "?").
    bool IsAcknowledgement(const std::vector<std::string>& words,
                           const std::vector<std::vector<std::string>>& ackWords);

    // Mostly capitals: "HELLO IS ANYONE THERE!?". Short shouts like "LOL" do
    // not count.
    bool IsShouted(const std::string& text);

    // What kind of line it is, from its visible text. Words of `names` (the
    // bots that could be meant) are ignored, so "ty Lahu" and "THANKS LAHU"
    // are still thank-yous.
    struct LineTraits
    {
        bool shouted   = false;   // mostly capitals
        bool ack       = false;   // "ty", "lol", "ok np" -- shouted or not
        bool openWords = false;   // "anyone", "who else", "hi all"...
        bool open      = false;   // said to everyone: open words, or any shout that is not an ack
        bool greeting  = false;   // nothing but a greeting: "hello", "hey there", "o/"
    };
    LineTraits Classify(const std::string& cleanText,
                        const std::vector<std::vector<std::string>>& ackWords,
                        const std::vector<std::vector<std::string>>& openPhrases,
                        const std::vector<std::string>& names = {},
                        const std::vector<std::vector<std::string>>& greetWords = {});

    // Every word is a greeting word (single-word entries of `greetWords`), and
    // at least one of them is more than "there" or "again": "hello", "hey
    // there", "yo". Names should already be taken out.
    bool IsGreeting(const std::vector<std::string>& words,
                    const std::vector<std::vector<std::string>>& greetWords);

    // How many bots may answer one open line, given how many are listening:
    // one per `botsPerResponder` listeners, at least 1, at most `maxResponders`.
    uint32_t Capacity(uint32_t listeners, uint32_t botsPerResponder, uint32_t maxResponders);

    // "Lahu", "Lahu and Kael", "Lahu, Kael and Mira".
    std::string JoinNames(const std::vector<std::string>& names);

    // A rough sense of the audience for the prompt, never a number a bot could
    // contradict: "a few other players", "lots of players".
    std::string ListenerPhrase(uint32_t listeners);
}

enum class CrowdLineKind : uint8_t
{
    Aimed,         // named, or the transcript says who it answers
    Open,          // said to everyone: "anyone around?", or shouted
    Ambient,       // said to nobody in particular
    BotLine,       // a bot's line that another bot answers
    Greeting,      // "hello" to the room: a few hellos back
    Conversation,  // one turn of a running conversation (mod-ollama-chat_conversation)
    Command,       // the acknowledgement of a party instruction (mod-ollama-chat_partycmd)
    Favor          // a bot answering a request it is acting on: food and water (mod-ollama-chat_conjure)
};

struct CrowdSlot
{
    uint64_t    botGuid    = 0;
    bool        addressed  = false;   // named / answered / answered back: owed a reply
    bool        replyBack  = false;   // answering another bot inside this crowd
    uint8_t     chainDepth = 0;       // depth stamped on this slot's reply
    uint64_t    answerGuid = 0;       // replyBack: the bot being answered
    std::string answerText;           // replyBack: what it said
    uint64_t    answerSeq  = 0;       // replyBack: its transcript line
    std::string hint;                 // when set, the prompt hint for this voice, as written
};

struct CrowdPlan
{
    using TimePoint = std::chrono::steady_clock::time_point;

    uint64_t               id = 0;
    std::string            key;        // one running plan per key (see Crowd_Start)
    std::string            scopeKey;   // the governor's scope: cooldowns, rate limits, repetition
    ChatChannelSourceLocal source = SRC_UNDEFINED_LOCAL;
    std::string            channelName;
    uint32_t               channelId = 0;

    // The line the crowd answers.
    uint64_t      speakerGuid  = 0;
    bool          speakerIsBot = false;
    std::string   speakerName;
    std::string   message;
    uint64_t      messageSeq   = 0;    // its transcript line
    CrowdLineKind kind         = CrowdLineKind::Open;
    bool          shouted      = false;
    uint32_t      listeners    = 0;    // bots that could hear it
    std::string   aimedAt;             // Aimed: who it was meant for, if anyone

    std::vector<CrowdSlot> slots;
    size_t      next         = 0;      // next slot to fire
    bool        inFlight     = false;
    size_t      inFlightSlot = 0;
    TimePoint   inFlightSince{};
    TimePoint   nextFireAt{};
    TimePoint   created{};

    // Bots whose line has landed, in order. Later slots are told who already
    // answered, and a reply that names one of them can get an answer back.
    std::vector<std::string> answeredBy;
    std::vector<uint64_t>    answeredGuids;
    uint32_t                 replyBacks = 0;

    // Set on a conversation turn or a command acknowledgement: its line starts
    // no answer-backs and no crowd of its own -- for a conversation, the
    // conversation decides who speaks next.
    uint64_t conversationId = 0;
    bool     closed         = false;
};

// The key one running plan is stored under: party, raid and guild chat are one
// conversation each; a channel and a /say area are one per faction, since the
// two sides never see each other's lines there.
std::string Crowd_KeyFor(const std::string& scopeKey, ChatChannelSourceLocal source, uint32_t teamId);

// Run `plan`, replacing whatever plan was running under its key (scopeKey
// when key is empty). The first slot fires at once, or after `firstDelayMs`.
// World thread.
void Crowd_Start(CrowdPlan plan, uint32_t firstDelayMs = 0);

// The plan running under this key, if any. With planId != 0, only that plan.
// The pointer is valid until the next Crowd_* call. World thread.
CrowdPlan* Crowd_Find(const std::string& key, uint64_t planId = 0);

// The running plan with this id, wherever it is; nullptr once it has ended or
// been replaced. World thread.
CrowdPlan* Crowd_FindById(uint64_t planId);

// A slot's reply was delivered (landed = true) or will never be. Delivery
// calls this, on the world thread, BEFORE handing the line to other bots --
// so a reply-back slot inserted for it lands in the right place. A reply that
// lands after its slot timed out is still remembered as said, so later voices
// are told about it.
void Crowd_OnSlotFinished(uint64_t planId, uint32_t slot, bool landed,
                          uint64_t botGuid, const std::string& botName);

// A worker's generation failed or came back empty. Thread-safe.
void Crowd_ReportWorkerFailure(uint64_t planId, uint32_t slot);

// Fire due slots, expire stuck ones. World tick.
void Crowd_Update();
void Crowd_Reset();

uint64_t Crowd_NextPlanId();
bool     Crowd_IsPlayful(const std::string& personalityKey);

// Implemented in mod-ollama-chat_handler.cpp, where the eligibility and prompt
// helpers live: re-check that the slot's bot can still answer, build its
// prompt from the transcript as it is NOW, and submit it. False when the bot
// can no longer answer (gone, out of range, rate-limited, queue full); the
// plan then moves straight on to the next slot. World thread.
bool Crowd_FireSlot(CrowdPlan& plan, size_t index);

class Player;

// Implemented in mod-ollama-chat_handler.cpp: `count` more voices from `pool`
// that sound like different people -- a different personality each where
// possible, playful and straight styles taking turns, and nobody who has only
// just spoken. `fixed` are already taken. World thread.
std::vector<Player*> Crowd_PickVoices(const std::vector<Player*>& pool,
                                      const std::vector<Player*>& fixed, uint32_t count);

struct CrowdStats
{
    uint64_t plansStarted = 0;
    uint64_t slotsFired   = 0;
    uint64_t slotsLanded  = 0;
    uint64_t slotsSkipped = 0;   // bot gone, out of range or rate-limited at fire time
    uint64_t slotsDropped = 0;   // fired, but nothing was said (pass, filter, error)
    uint64_t slotsLate    = 0;   // landed after its slot had timed out (counted as landed)
    uint64_t replyBacks   = 0;
    uint32_t active       = 0;
};
CrowdStats Crowd_GetStats();
void       Crowd_NoteReplyBack();

#endif // MOD_OLLAMA_CHAT_CROWD_H
