#ifndef MOD_OLLAMA_CHAT_CONFIG_H
#define MOD_OLLAMA_CHAT_CONFIG_H

#include <string>
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <utility>
#include <mutex>
#include <ctime>
#include <bitset>
#include "ScriptMgr.h"  // Ensure WorldScript is defined
#include "mod-ollama-chat_textrules.h"

// --------------------------------------------
// Distance/Range Configuration
// --------------------------------------------
extern float      g_SayDistance;
extern float      g_YellDistance;
extern float      g_RandomChatterRealPlayerDistance;
extern float      g_EventChatterRealPlayerDistance;

// --------------------------------------------
// Bot/Player Chatter Probability & Limits
// --------------------------------------------
// Per-channel-type reply chances
extern uint32_t   g_PlayerReplyChance_Say;
extern uint32_t   g_BotReplyChance_Say;
extern uint32_t   g_PlayerReplyChance_Channel;
extern uint32_t   g_BotReplyChance_Channel;
extern uint32_t   g_PlayerReplyChance_Party;
extern uint32_t   g_BotReplyChance_Party;
extern uint32_t   g_PlayerReplyChance_Guild;
extern uint32_t   g_BotReplyChance_Guild;

extern uint32_t   g_MaxBotsToPick;
extern uint32_t   g_RandomChatterBotCommentChance;
extern uint32_t   g_RandomChatterMaxBotsPerPlayer;
extern uint32_t   g_EventChatterBotCommentChance;
extern uint32_t   g_EventChatterBotSelfCommentChance;
extern uint32_t   g_EventChatterMaxBotsPerPlayer;

// --------------------------------------------
// Ollama LLM API Configuration
// --------------------------------------------

// Which wire format OllamaChat.Url speaks. Everything in the module still
// calls this "Ollama"; the provider only changes how one request is encoded
// and decoded in mod-ollama-chat_api.cpp.
enum class OllamaProvider : uint8_t
{
    Ollama = 0,     // Ollama's native /api/generate
    OpenAI,         // OpenAI Chat Completions format: OpenAI, OpenRouter, Groq,
                    // Mistral, DeepSeek, xAI, Gemini (compat endpoint), LM Studio,
                    // vLLM, llama.cpp server, and Ollama's own /v1/chat/completions
    Anthropic,      // Anthropic Messages API (Claude)
};

OllamaProvider OllamaProvider_Parse(const std::string& text);
const char*    OllamaProvider_Name(OllamaProvider provider);

extern std::string g_OllamaUrl;
extern std::string g_OllamaModel;
extern OllamaProvider g_OllamaProvider;
extern std::string g_OllamaApiKey;            // never log this
extern std::string g_OllamaApiKeyHeader;      // empty = provider default header
extern int32_t     g_OllamaVerifyCertificates; // -1 auto (on when ApiKey set), 0 off, 1 on
extern uint32_t    g_OllamaNumPredict;
extern float       g_OllamaTemperature;
extern float       g_OllamaTopP;
extern float       g_OllamaRepeatPenalty;
extern uint32_t    g_OllamaNumCtx;
extern uint32_t    g_OllamaNumThreads;
extern std::string g_OllamaStop;
extern std::string g_OllamaSystemPrompt;
extern std::string g_OllamaSeed;

// Optional sampling controls for response diversity. All default to "unset",
// in which case the field is not sent at all and the model's default applies.
extern int32_t     g_OllamaTopK;              // -1 = unset
extern float       g_OllamaMinP;              // -1 = unset
extern float       g_OllamaPresencePenalty;   // <= -999 = unset
extern float       g_OllamaFrequencyPenalty;  // <= -999 = unset

// --------------------------------------------
// Concurrency/Queueing
// --------------------------------------------
extern uint32_t    g_MaxConcurrentQueries;

// --------------------------------------------
// Feature Toggles & Core Settings
// --------------------------------------------
extern bool        g_Enable;
extern bool        g_DisableRepliesInCombat;
extern bool        g_EnableRandomChatter;
extern bool        g_EnableEventChatter;
extern bool        g_EnableRPPersonalities;
extern bool        g_EnableWhisperReplies;
extern bool        g_DebugEnabled;
extern bool        g_DebugShowFullPrompt;

// --------------------------------------------
// Random Chatter Timing
// --------------------------------------------
extern uint32_t    g_MinRandomInterval;
extern uint32_t    g_MaxRandomInterval;

// --------------------------------------------
// Conversation History Settings
// --------------------------------------------
extern uint32_t    g_MaxConversationHistory;
extern uint32_t    g_ConversationHistorySaveInterval;

// --------------------------------------------
// Prompt Templates
// --------------------------------------------
extern std::string g_RandomChatterPromptTemplate;
extern std::vector<std::string> g_RandomChatterPromptVariations;
extern std::vector<std::string> g_RandomChatterQuestionVariations;
extern std::string g_EventChatterPromptTemplate;
extern std::string g_ChatPromptTemplate;
extern std::string g_ChatExtraInfoTemplate;

// --------------------------------------------
// Personality and Prompt Data
// --------------------------------------------
extern std::unordered_map<uint64_t, std::string> g_BotPersonalityList;
extern std::unordered_map<std::string, std::string> g_PersonalityPrompts;
extern std::vector<std::string> g_PersonalityKeys;
extern std::vector<std::string> g_PersonalityKeysRandomOnly; // Personalities that can be randomly assigned
extern std::string g_DefaultPersonalityPrompt;

// --------------------------------------------
// Chat History Templates and Toggles
// --------------------------------------------
extern bool        g_EnableChatHistory;
extern std::string g_ChatHistoryHeaderTemplate;
extern std::string g_ChatHistoryLineTemplate;
extern std::string g_ChatHistoryFooterTemplate;

// --------------------------------------------
// Chatbot Snapshot Template
// --------------------------------------------
extern bool        g_EnableChatBotSnapshotTemplate;
extern std::string g_ChatBotSnapshotTemplate;

// --------------------------------------------
// Conversation History Store and Mutex
// --------------------------------------------
// One turn of a bot/player conversation.
//
// `persisted` is what keeps SaveBotConversationHistoryToDB() from rewriting
// the whole window every save interval. It is set once the row has been handed
// to the database, and is false for anything appended since the last save.
struct BotConversationEntry
{
    std::string playerMessage;
    std::string botReply;
    bool        persisted = false;
};

extern std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::deque<BotConversationEntry>>> g_BotConversationHistory;
extern std::mutex   g_ConversationHistoryMutex;
extern time_t       g_LastHistorySaveTime;

// --------------------------------------------
// Blacklist: Prefixes for Commands (not chat)
// --------------------------------------------
extern std::vector<std::string> g_BlacklistCommands;

// --------------------------------------------
// Think Mode (see mod-ollama-chat_capability.h)
// --------------------------------------------
extern bool     g_ThinkModeEnableForModule;   // deprecated; maps onto ThinkMode
extern uint8_t  g_ThinkModePolicy;            // OllamaThinkPolicy
extern uint32_t g_ThinkMaxLatencyMs;
// Extra num_predict headroom granted when reasoning tokens are expected --
// either because think mode is on, or because the model reasons whether or not
// it is asked to. Without it a small NumPredict is spent on reasoning and the
// reply comes back empty. 0 disables the headroom entirely.
extern uint32_t g_ReasoningTokenReserve;
extern uint32_t g_CapabilityProbeTimeoutSeconds;

// --------------------------------------------
// HTTP / dispatcher
// --------------------------------------------
extern uint32_t g_HttpTimeoutSeconds;
extern uint32_t g_RateLimitRetries;          // retries on HTTP 429/503/529
extern uint32_t g_RateLimitMaxWaitSeconds;   // cap on one back-off sleep
extern uint32_t g_DispatchWorkerThreads;
extern uint32_t g_MaxQueueDepth;

// --------------------------------------------
// Response post-processing
// --------------------------------------------
extern uint32_t g_MaxReplyLength;
extern bool     g_ResponseStripMarkdown;
extern bool     g_ResponseStripDecorativeUnicode;

// --------------------------------------------
// Conversation governor
// --------------------------------------------
extern uint8_t  g_MaxChainDepth;
extern uint32_t g_ChainChanceDecayPct;
extern bool     g_RequireRecentHuman;
extern uint32_t g_HumanWindowSeconds;
extern uint32_t g_BotCooldownSeconds;
extern uint32_t g_ScopeCooldownSeconds;
// Party/raid groups at or below this many members count as direct address,
// so a bot answering a person in them is not held back by the ambient
// pacing cooldowns. 0 disables the group case entirely.
extern uint32_t g_DirectAddressGroupSize;
// How long after a bot answers someone that person keeps its attention in
// that scope. 0 disables the open-conversation rule.
extern uint32_t g_ConversationWindowSeconds;
// Reply chance used instead of the ambient per-channel chance when the line
// is aimed at this bot.
extern uint32_t g_DirectAddressReplyChance;
extern uint32_t g_ScopeMessagesPerMinute;
extern uint32_t g_GlobalMessagesPerMinute;
extern uint32_t g_BotHistorySize;
extern uint32_t g_ScopeHistorySize;
extern float    g_RepetitionSimilarityThreshold;
extern uint32_t g_RepetitionWindowSeconds;
extern uint32_t g_OpenerHistorySize;
// Lines of three words or fewer ("lol", "no", "hey") are only compared with
// what was said this recently: across a whole busy evening the same short
// reaction is bound to come up again, and that is not repetition.
extern uint32_t g_RepetitionShortLineWindowSeconds;
extern std::string g_RepetitionOwnRepeatNote;   // one more go for a line it said itself lately: {line}
extern TextRules::QuoteSources g_ResponseQuoteSources;            // what a quoted line is from: cut off the end
extern std::unordered_set<std::string> g_ResponseQuoteSourceStyles; // ...of these styles' lines

// --------------------------------------------
// Shared chat context (mod-ollama-chat_context)
// --------------------------------------------
extern bool        g_ContextEnable;
extern uint32_t    g_ContextMaxLines;
extern uint32_t    g_ContextMaxAgeSeconds;
// A line typed within this long after someone else's counts as an answer to them.
extern uint32_t    g_ContextReplyWindowSeconds;
// Other bots' reply chance, in percent of normal, when a person's line was
// clearly aimed at someone else.
extern uint32_t    g_ContextBystanderChancePct;
// Chance a bot answers a bot that just replied to it (before chain decay).
extern uint32_t    g_ContextBotAddressedChance;
// BotReplyChance.* is the chance that ANY bot answers a bot's line, instead
// of an independent roll per listening bot.
extern bool        g_ContextBotReplyPerMessage;
// With BotReplyPerMessage: the chance per bot line, by channel (replaces
// BotReplyChance.* for bot lines, which keep their per-bot meaning otherwise).
extern uint32_t    g_ContextBotLineReplyChance_Say;
extern uint32_t    g_ContextBotLineReplyChance_Party;
extern uint32_t    g_ContextBotLineReplyChance_Guild;
extern uint32_t    g_ContextBotLineReplyChance_Channel;
// With BotReplyPerMessage: bots may answer each other whenever a real player
// can see the channel, not only shortly after one typed there.
extern bool        g_ContextBotChatWithoutRecentHuman;
// Leave {chat_history} empty when {channel_history} already has recent lines.
extern bool        g_ContextSuppressPairHistory;
extern std::string g_ContextPassToken;
extern std::string g_ContextPassTokenNorm;      // lowercase, letters and digits only
extern std::string g_ContextHistoryHeader;
extern std::string g_ContextHintAddressed;
extern std::string g_ContextHintBystander;
extern std::string g_ContextHintOpen;
extern std::string g_ContextHintPublicWithGroup;   // someone in its group, talking in General or guild chat
extern std::string g_ContextHintEvent;
extern std::string g_ContextHintAmbient;
extern std::vector<std::string> g_ContextFillerWords;   // lowercase; "haha*" = prefix
extern uint32_t    g_ContextMaxFillersPerLine;
extern uint32_t    g_ContextFillerScopeLines;
extern uint32_t    g_ContextFillerBotCooldownSeconds;

// --------------------------------------------
// Crowd replies (mod-ollama-chat_crowd)
// --------------------------------------------
extern bool        g_CrowdEnable;
// Most bots that answer one line said to everyone.
extern uint32_t    g_CrowdMaxResponders;
// One possible responder per this many bots that can hear the line.
extern uint32_t    g_CrowdBotsPerResponder;
// A line aimed at one bot: chance that another joins in, rolled again for
// each extra voice, up to ChimeInMax.
extern uint32_t    g_CrowdChimeInChance;
extern uint32_t    g_CrowdChimeInMax;
// "ty", "ok", "lol": chance the bot it was aimed at answers at all.
extern uint32_t    g_CrowdAckReplyChance;
// Pause after one bot's line appears before the next starts on theirs.
extern uint32_t    g_CrowdGapMinMs;
extern uint32_t    g_CrowdGapMaxMs;
extern uint32_t    g_CrowdSlotTimeoutSeconds;
extern uint32_t    g_CrowdMaxPlanSeconds;
// Bots answering each other inside one crowd.
extern uint32_t    g_CrowdMaxReplyBacks;
// A bot's line that another bot answers: chance a second bot joins in.
extern uint32_t    g_CrowdBotLineSecondChance;
extern std::vector<std::vector<std::string>> g_CrowdOpenCallPhrases;
extern std::vector<std::vector<std::string>> g_CrowdAckWords;
extern std::unordered_set<std::string>       g_CrowdPlayfulPersonalities;   // uppercase keys
extern std::string g_CrowdHintOpenCall;
extern std::string g_CrowdHintFollowUp;
extern std::string g_CrowdHintReplyBack;
extern std::string g_CrowdHintShout;
// A line that is only a greeting ("hello", "hey there") is said to the room.
extern std::vector<std::vector<std::string>> g_CrowdGreetingWords;
// Busy social hubs: a line to nobody in particular may get up to all of the
// responders, not half.
extern bool        g_CrowdFullCrowdInCities;

// --------------------------------------------
// Natural chat v4
// --------------------------------------------
// Level-ups and other events (mod-ollama-chat_events)
extern std::string g_EventChatterSelfPromptTemplate;   // when it happened to the bot itself
extern std::vector<std::string> g_EventCongratsLines;  // "gz|grats {name}|..."
extern uint32_t    g_EventCongratsCannedChance;        // % of congratulations typed without the LLM

// Joining a group (mod-ollama-chat_joined)
extern bool        g_JoinedEnable;
extern uint32_t    g_JoinedGreetChance;            // % of out-of-the-blue invites that get a hello
extern uint32_t    g_JoinedGroupCooldownSeconds;   // one hello per group in this long
extern uint32_t    g_JoinedContinueMinutes;        // talked this lately: carry on instead of a hello
extern uint32_t    g_JoinedCannedChance;           // % of hellos typed from GreetLines without the model
extern std::vector<std::string> g_JoinedGreetLines;
extern std::string g_JoinedGreetPrompt;
extern std::string g_JoinedContinuePrompt;
extern uint32_t    g_JoinedDelayMinMs;
extern uint32_t    g_JoinedDelayMaxMs;

// "gz", and the one "ty" (mod-ollama-chat_congrats)
extern bool        g_CongratsEnable;
extern uint32_t    g_CongratsWindowSeconds;            // a gz this long after a ding is for it
extern std::vector<std::vector<std::string>> g_CongratsWords;        // "gz", "grats", "congrats"
extern std::vector<std::vector<std::string>> g_CongratsFillerWords;  // "on", "the", "ding", "level"
extern std::vector<std::string> g_CongratsThanksLines;               // "ty", "ty {name}", "cheers"
extern std::vector<std::string> g_CongratsThanksAllLines;            // "ty all", "thanks guys"
extern uint32_t    g_CongratsThanksChance;             // % of dings that get a thank-you at all
extern uint32_t    g_CongratsThanksDelayMinMs;         // after the first gz: the rest come in first
extern uint32_t    g_CongratsThanksDelayMaxMs;
extern bool        g_CongratsAchievementsRareOnly;    // gz an achievement only when it is worth one
extern uint32_t    g_CongratsAchievementRarePoints;   // ...this many points and up is
extern std::unordered_set<uint32_t> g_CongratsAchievementAlways;   // ids: always worth a gz
extern std::unordered_set<uint32_t> g_CongratsAchievementNever;    // ids: never
extern std::bitset<256> g_CongratsGuildLevels;    // dings guild chat says gz to (it only hears of those)

// Reply cleanup
extern std::vector<std::vector<std::string>> g_ResponseAssistantPhrases;

// Emote styles: personality key (uppercase) -> TEXT_EMOTE ids, repeats weight them.
extern std::unordered_map<std::string, std::vector<uint32_t>> g_EmoteStyles;
extern std::vector<uint32_t>      g_EmoteStyleDefault;
extern std::unordered_set<uint32_t> g_EmotePetEmotes;   // aimed at a pet or companion when there is one

// Presence: bots noticing real players walking by (mod-ollama-chat_presence)
extern bool        g_PresenceEnable;
extern float       g_PresenceRange;
extern uint32_t    g_PresencePlayerIntervalMin;    // seconds between moments near one player
extern uint32_t    g_PresencePlayerIntervalMax;
extern uint32_t    g_PresenceBotCooldownSeconds;
extern uint32_t    g_PresencePairCooldownSeconds;
extern uint32_t    g_PresenceChance;
extern uint32_t    g_PresenceSayChance;
extern bool        g_PresenceCityOnly;
extern std::string g_PresencePromptTemplate;

// Cities and other social hubs
extern bool        g_CityEnable;
extern std::unordered_set<uint32_t> g_CityExtraIds;    // zone or area ids besides the capitals
extern bool        g_CityIncludeInns;
extern uint32_t    g_CityChatterMultiplier;
extern uint32_t    g_CityConversationStartChance;
extern uint32_t    g_CityBotLineReplyBonus;
extern uint32_t    g_CityConversationBoost;

// Conversations (mod-ollama-chat_conversation)
extern bool        g_ConversationEnable;
extern uint32_t    g_ConversationMinMinutes;
extern uint32_t    g_ConversationMaxMinutes;
extern uint32_t    g_ConversationGapMinSeconds;
extern uint32_t    g_ConversationGapMaxSeconds;
extern uint32_t    g_ConversationLullChance;
extern uint32_t    g_ConversationLullMinSeconds;
extern uint32_t    g_ConversationLullMaxSeconds;
extern uint32_t    g_ConversationMaxActive;
extern uint32_t    g_ConversationMaxParticipants;
extern uint32_t    g_ConversationJoinChance;
extern uint32_t    g_ConversationFromPlayerChance;
extern uint32_t    g_ConversationFromBotLineChance;
extern uint32_t    g_ConversationCooldownSeconds;
extern uint32_t    g_ConversationMaxMisses;
extern uint32_t    g_ConversationObserveChance;
extern bool        g_ConversationAllowTrade;
extern bool        g_ConversationAllowSay;
extern std::vector<std::string> g_ConversationPremises;
extern std::string g_ConversationHintTurn;
extern std::string g_ConversationHintJoin;
extern std::string g_ConversationHintWindDown;
extern std::string g_ConversationHintStartChannel;
extern std::string g_ConversationHintStartSay;
extern std::string g_ConversationHintObserve;

// Party instructions for mod-dungeon-clear (mod-ollama-chat_partycmd)
extern bool        g_PartyCmdEnable;
extern TextRules::PartyRules g_PartyCmdRules;
extern float       g_PartyCmdMaxDistance;
extern std::vector<std::vector<std::string>> g_PartyRunQuestionPhrases;   // "whats next", "why are we waiting"
extern uint32_t    g_PartyRunQuestionMaxWords;
extern std::string g_PartyCmdHintStart;
extern std::string g_PartyCmdHintPause;
extern std::string g_PartyCmdHintResume;
extern std::string g_PartyCmdHintWaiting;
extern std::string g_PartyDungeonGlossary;
extern uint32_t    g_PartyDungeonAmbientPercent;   // ambient lines inside, as a share of outside
extern std::string g_PartyDungeonFocusText;        // ...and what they keep to
// The tank's last few acknowledgements in a group, so the next one is not a
// copy: how many to keep, and the sentence that shows them ({lines}).
extern uint32_t    g_PartyCmdRecentAcks;
// Inside a dungeon-clear run: the run's state, for bots asked about it.
extern std::string g_PartyRunStateText;
extern std::string g_PartyCmdRecentAcksText;

// --------------------------------------------
// Natural chat v5
// --------------------------------------------
// What a bot would know and talk about at its level, and which roles its
// class can fill (mod-ollama-chat_outlook). World thread only.
extern bool        g_OutlookEnable;
extern std::string g_OutlookLevelBands;     // "1-9: ...|10-19: ...|80: ..."
extern bool        g_OutlookClassRoles;
extern std::string g_OutlookKnowledgeText;  // what a bot can know about other players
extern std::vector<TextRules::LevelPhrase> g_OutlookLevelPhrases;   // "[40+] repair bill"

// Item and quest links in bot lines (mod-ollama-chat_links). World thread only.
extern bool        g_LinksEnable;
extern uint32_t    g_LinksMaxPerLine;
extern uint32_t    g_LinksRememberMinutes;  // links seen in chat stay reusable this long
extern bool        g_LinksUseGameData;      // look names up in the item and quest tables
extern std::string g_LinksPromptHint;

// Stock phrases (mod-ollama-chat_phrases). World thread only.
extern std::vector<std::vector<std::string>> g_ResponseOverusedPhrases;
extern uint32_t    g_ResponseOverusedCooldownSeconds;
extern std::vector<std::vector<std::string>> g_ResponseOffEraPhrases;

// --------------------------------------------
// Natural chat v6
// --------------------------------------------
// Conjured food and water on request (mod-ollama-chat_conjure). World thread
// only.
extern bool        g_ConjureEnable;
extern TextRules::ConjureRules g_ConjureRules;
extern uint32_t    g_ConjureStacks;          // stacks given when they don't say how many
extern uint32_t    g_ConjureMaxStacks;       // the most one ask gets
extern bool        g_ConjureUseMana;         // conjuring costs the mage its mana
extern uint32_t    g_ConjureWaitSeconds;     // how long an ask waits for a fight, mana or range
extern uint32_t    g_ConjureTradeSeconds;    // how long an offered trade stays open
extern std::string g_ConjureHintGive;
extern std::string g_ConjureHintWait;
extern std::string g_ConjureHintCant;
extern std::string g_ConjureHintNotMage;
extern std::string g_ConjureHintAlready;
extern std::string g_ConjureHintNotGrouped;

// Per chat style: OllamaChat.Personality.<setting>.<KEY>, keyed here by the
// upper-case personality key. World thread only.
struct PersonalityNudge
{
    uint32_t    percent = 0;   // share of replies that get `yes`
    std::string yes;           // the instruction, on that share
    std::string no;            // on the rest; may be empty
};
extern std::unordered_map<std::string, PersonalityNudge> g_PersonalityNudges;
extern std::unordered_map<std::string, uint32_t>         g_PersonalityMaxReplyLength;
extern std::string g_PersonalityLongReplyNote;   // what a style with a longer limit is told
extern std::string g_PersonalitySaidLatelyNote;  // with a nudge: what it said lately, {lines}
extern uint32_t    g_PersonalitySaidLatelyLines; // ...at most this many of them

// Read OllamaChat.Emote.Style.<KEY> for every personality loaded from the
// database. Called from the config load, after the personality templates.
void LoadEmoteStyles();

// --------------------------------------------
// Natural chat v7
// --------------------------------------------
// Replies. World thread only.
extern std::vector<std::vector<std::string>> g_ResponseDropVocatives;   // "kid", where it addresses someone
extern std::vector<std::vector<std::string>> g_ResponseInsultWords;     // "knob", "muppet": banter or heat only
extern std::vector<std::vector<std::string>> g_ResponseProvokeWords;    // what makes one fair: "stfu", "ur mum"
extern std::vector<std::string> g_ResponseInsultPersonalities;          // styles that are just rude: NAT_RUDE
extern uint32_t    g_ResponseInsultCooldownSeconds;                     // even then, not every line
extern std::vector<std::vector<std::string>> g_ResponseRarePhrases;     // once per cooldown, even said to someone
extern uint32_t    g_ResponseRareCooldownSeconds;

// A line that is only thanks ("ty", "cheers mate"). World thread only.
extern std::vector<std::vector<std::string>> g_CrowdThanksWords;
extern uint32_t    g_CrowdThanksReplyChance;

// What a dead bot is told about itself. World thread only.
extern std::string g_StateDeadText;    // dead, not released yet
extern std::string g_StateGhostText;   // released, running back

// Orders in plain words: "stay back", "come trade me" (mod-ollama-chat_orders).
// World thread only.
extern bool        g_OrdersEnable;
extern TextRules::BotCommandRules g_OrdersRules;
// What a bot says it is doing, per order ({player_name}, {who}, {target},
// {doing}, {where}), and that it cannot ({order}, {reason}).
extern std::string g_OrdersHint[static_cast<size_t>(TextRules::BotIntent::Count)];
extern std::string g_OrdersHintCant;
extern uint32_t    g_OrdersTradeWalkSeconds;    // how long a bot walks over to trade before giving up
extern uint32_t    g_OrdersFleeRecoverSeconds;  // after "run!": out of combat this long, and it fights again
extern std::string g_StateStayingText;          // a bot waiting where it was told to stay

// The rogue saying why it is not sapping the star (mod-ollama-chat_marks; the
// marks themselves are mod-playerbots-plus's). World thread only.
extern bool        g_MarksSayEnable;
extern uint32_t    g_MarksSayCooldownSeconds;
extern std::string g_MarksSayPrompt;
extern std::string g_MarksSapNotLearned;
extern std::string g_MarksSapWrongType;
extern std::string g_MarksSapNoStealth;
extern std::string g_MarksSapFailed;

// What a bot carries, and handing over what it can spare (mod-ollama-chat_bags)
extern bool        g_BagsEnable;
extern uint32_t    g_BagsRecentMinutes;      // looted this recently: "just looted"
extern uint32_t    g_BagsOfferSeconds;       // "me!" claims an offer made this recently
extern uint32_t    g_BagsInviteWaitSeconds;  // told to invite it: the trade starts if they do within this
extern uint32_t    g_BagsMaxSpare;
extern uint32_t    g_BagsMaxKept;
extern std::string g_BagsPromptText;
extern std::vector<std::vector<std::string>> g_BagsTalkPhrases;   // "anything to trade", "what did you loot"
extern std::vector<std::vector<std::string>> g_BagsAskPhrases;    // "can i have", "ill take"
extern std::vector<std::vector<std::string>> g_BagsYesPhrases;    // words a bare claim is made of: "me", "yes", "please"
extern std::vector<std::vector<std::string>> g_BagsClaimPhrases;  // ...one of which it needs: "me", "yes", "dibs"
extern uint32_t    g_BagsMaxGiftQuality;     // given away free: no better than this (2 = green)
extern uint32_t    g_BagsMaxGiftCopper;      // ...and a stack selling for no more than this
extern bool        g_BagsGiveAway;           // hands over what it has no use for, for nothing (off by default)
extern std::string g_BagsNoGiftReason;       // why not, when it is off
extern std::string g_BagsSupplyReason;       // why it keeps what playerbots gave it
extern std::vector<std::vector<std::string>> g_BagsNotPhrases;    // "dont", "no thanks"
extern std::string g_BagsHintGive;
extern std::string g_BagsHintInvite;
extern std::string g_BagsHintKeep;
extern std::string g_BagsHintNotHave;
extern std::string g_BagsHintCant;
extern std::string g_BagsHintAlready;      // asked again while it is going over
extern std::string g_BagsHandoverText;     // saying so hands nothing over

// Asks with a real reason behind them (mod-ollama-chat_needs)
extern bool        g_NeedsEnable;
extern uint32_t    g_NeedsChance;                 // of an ambient line, the share that tries one
extern uint32_t    g_NeedsBotCooldownMinutes;
extern uint32_t    g_NeedsRepeatMinutes;          // the same thing again
extern uint32_t    g_NeedsZoneCooldownSeconds;    // the same kind of ask in one zone
extern uint32_t    g_NeedsRememberMinutes;
extern uint32_t    g_NeedsWeight[4];              // quest, errand, spare, short
extern uint32_t    g_NeedsShortMaxLevel;
extern uint32_t    g_NeedsShortCopperPerLevel;
extern std::string g_NeedsPromptTemplate;
extern std::string g_NeedsReasonText;
extern std::string g_NeedsPlanText;                                     // what it is really doing, {doing}
extern std::vector<std::vector<std::string>> g_NeedsPlanPhrases;        // "what you doing", "going for"
extern uint32_t    g_NeedsPlanMaxQuests;
extern std::string g_NeedsAskQuest;
extern std::string g_NeedsAskDungeon;
extern std::string g_NeedsReasonQuest;
extern std::string g_NeedsAskErrand;
extern std::string g_NeedsReasonErrand;
extern std::string g_NeedsAskSpare;
extern std::string g_NeedsReasonSpare;
extern std::string g_NeedsAskShort;
extern std::string g_NeedsReasonShort;

// A bot tank greeting its dungeon group and calling the finish
// (mod-ollama-chat_partycmd). World thread only.
extern std::string g_PartyAnnouncePrompt;
extern bool        g_PartyGreetEnable;
extern uint32_t    g_PartyGreetDelaySeconds;
extern std::string g_PartyGreetSituation;
extern bool        g_PartyDoneEnable;
extern std::string g_PartyDoneSituation;

// The end of a dungeon-clear run (mod-ollama-chat_partycmd): the tank's call
// has to say the dungeon is cleared, the rest say gg, and the bots that came
// in through the dungeon finder leave unless asked to stay. World thread only.
extern std::vector<std::vector<std::string>> g_PartyDoneWords;   // the tank's call has one of these, or...
extern std::vector<std::string> g_PartyDoneLines;                 // ...one of these goes out instead
extern bool        g_PartyCheerEnable;
extern uint32_t    g_PartyCheerChance;         // each other bot says gg this often
extern uint32_t    g_PartyCheerCannedChance;   // ...typed (CheerLines) rather than written by the model
extern std::vector<std::string> g_PartyCheerLines;
extern std::string g_PartyCheerPrompt;
extern bool        g_PartyDoneLeave;
extern uint32_t    g_PartyDoneLeaveSeconds;    // the first one leaves this long after the call
extern std::vector<std::vector<std::string>> g_PartyDoneStayWords;   // "wait", "can you help": they stay
extern std::vector<std::vector<std::string>> g_PartyDoneGoWords;     // "ok bye": then they go after all
extern std::string g_PartyDoneStateText;
extern std::string g_PartyDoneLeavingText;
extern std::string g_PartyDoneStayingText;

// Read OllamaChat.Personality.Nudge.<KEY> and .MaxReplyLength.<KEY> for every
// personality loaded from the database. Called right after LoadEmoteStyles.
void LoadPersonalityExtras();

// --------------------------------------------
// Natural chat v10
// --------------------------------------------
// A bot that was stuck getting on and off its mount, knowing it and saying so
// (mod-ollama-chat_stuck; finding it stuck is mod-playerbots-plus's)
extern uint32_t    g_StuckRememberMinutes;   // knows it was stuck, if asked, for this long
extern uint32_t    g_StuckRemarkChance;
extern std::string g_StuckRemarkPrompt;
extern std::string g_StuckStateText;

// Quest progress said in party, not whispered (mod-ollama-chat_progress)
extern bool        g_ProgressEnable;
extern uint32_t    g_ProgressCheckSeconds;
extern uint32_t    g_ProgressDelaySeconds;   // after it is done, before it is said
extern uint32_t    g_ProgressGapSeconds;     // one such line per group in this long
extern std::string g_ProgressPrompt;
extern std::string g_ProgressKillText;
extern std::string g_ProgressUseText;
extern std::string g_ProgressItemText;
extern std::string g_ProgressQuestText;

// What a named item or quest is (mod-ollama-chat_links)
extern bool        g_LinksDescribeEnable;
extern uint32_t    g_LinksDescribeMax;
extern uint32_t    g_LinksDescribeQuestChars;
extern std::string g_LinksDescribeText;

// Conversation premises with slots, and how often any one comes round (mod-ollama-chat_premises)
extern std::vector<std::string> g_PremiseTemplates;
extern uint32_t    g_PremiseTemplateChance;    // % of openers from a template rather than the fixed list
extern uint32_t    g_PremiseCooldownMinutes;   // one premise, realm-wide, at most once in this long
extern uint32_t    g_PremiseValueMemory;       // a pool's last this-many picks are not picked again
// Pools by name: OllamaChat.Conversation.Pool.<name>, each entry optionally level-tagged.
extern std::unordered_map<std::string, std::vector<std::string>> g_PremisePools;

// The same phrase from bot after bot (mod-ollama-chat_phrases)
extern bool        g_EchoEnable;
extern uint32_t    g_EchoWords;          // phrases this many words long
extern uint32_t    g_EchoBots;           // said by this many different bots...
extern uint32_t    g_EchoWindowMinutes;  // ...in this long: an echo
extern std::vector<std::string> g_EchoCommonWords;
// A line owed to someone and dropped for a stock phrase gets one more try.
extern bool        g_ResponseRetryOwed;
extern std::string g_ResponseRetryNote;

// Text emoticons, now and then (mod-ollama-chat_context)
extern bool        g_EmoticonEnable;
extern std::vector<std::string> g_EmoticonTokens;   // what counts as one, at delivery
extern std::vector<std::string> g_EmoticonOffer;    // what a nudge suggests
extern uint32_t    g_EmoticonNudgeChance;           // % of prompts, for styles without their own
extern std::unordered_map<std::string, uint32_t> g_EmoticonStyleChance;   // personality key (upper) -> %
extern std::string g_EmoticonNudgeText;
extern uint32_t    g_EmoticonBotCooldownMinutes;
extern uint32_t    g_EmoticonScopeLines;

// Dungeon start: the tank asks if everyone is ready; a plain yes starts it
extern std::vector<std::vector<std::string>> g_PartyReadyYesPhrases;
extern uint32_t    g_PartyReadyWindowSeconds;
extern std::string g_PartyTogetherText;
extern std::string g_JoinedLfgPrompt;

// Taking a gift back out of the trade window
extern std::vector<std::vector<std::string>> g_BagsCancelPhrases;
extern std::vector<std::vector<std::string>> g_BagsCancelNotPhrases;
extern std::string g_BagsHintCancel;

// Orders that did nothing: not "on it" (Playerbots.CommandPrefix set)
extern std::vector<std::vector<std::string>> g_OrdersNotDoneVerbs;
extern std::string g_OrdersHintNotDone;

// Presence: not the group they just left, and nothing rude at strangers
extern uint32_t    g_PresenceExGroupMinutes;
extern std::unordered_set<uint32_t> g_PresenceSkipEmotes;

// How a named quest or item sits against the bot's level (mod-ollama-chat_links)
extern uint32_t    g_LinksQuestLevelsAbove;   // a quest more than this many levels above the bot is too high for it
extern bool        g_LinksQuietWhenTooLow;    // public lines: bots far too low for every quest named stay out of it
extern std::string g_LinksQuestTooHighText;
extern std::string g_LinksQuestEasyText;
extern std::string g_LinksItemTooHighText;

// A bot hanging out in a city knowing where it is and what it is doing
// (mod-ollama-chat_hangout; the hangouts are mod-playerbots-plus's)
extern std::string g_HangoutStateText;
extern std::string g_HangoutAwayText;

// --------------------------------------------
// A player giving away or selling something (mod-ollama-chat_gifts)
// --------------------------------------------
extern bool        g_GiftsEnable;
extern std::vector<std::vector<std::string>> g_GiftsOfferWords;   // "anyone want", "free"
extern std::vector<std::vector<std::string>> g_GiftsSellWords;    // "wts", "selling"
extern std::vector<std::string> g_GiftsGreedyPersonalities;       // take what they could only sell
extern uint32_t    g_GiftsSecondVoiceChance;
extern uint32_t    g_GiftsNobodyChance;
extern uint32_t    g_GiftsFollowUpSeconds;   // "5g then": still about the last thing
extern uint32_t    g_GiftsRememberSeconds;   // what a bot said yes to, or a price it agreed
extern uint32_t    g_GiftsMaxSpendPercent;   // of a bot's money, on one thing
extern std::string g_GiftsHintWant;
extern std::string g_GiftsHintWantToo;
extern std::string g_GiftsHintNoNeed;
extern std::string g_GiftsHintNotTradable;
extern std::string g_GiftsHintAskPrice;
extern std::string g_GiftsHintBuy;
extern std::string g_GiftsHintCounter;
extern std::string g_GiftsHintTooDear;
extern std::string g_GiftsHintNoBuy;
extern std::string g_GiftsInviteText;
extern std::string g_GiftsSaidGiftText;
extern std::string g_GiftsSaidBuyText;
extern std::string g_GiftsSaidCounterText;
extern std::string g_GiftsSaidAskText;

// --------------------------------------------
// The tank down, a wipe, nobody to rez (mod-ollama-chat_wipe)
// --------------------------------------------
extern bool        g_WipeEnable;
extern uint32_t    g_WipeTankDownChance;
extern uint32_t    g_WipeApologyChance;
extern uint32_t    g_WipeNoRezChance;
extern uint32_t    g_WipeRunBackChance;
extern uint32_t    g_WipeTypedChance;          // a typed line instead of the model's
extern uint32_t    g_WipeRunBackQuietSeconds;  // after the fight, before "running back"
extern uint32_t    g_WipeStateMinutes;         // a wipe stays in reply prompts this long
extern std::string g_WipeTankDownPrompt;
extern std::string g_WipeApologyPrompt;
extern std::string g_WipeNoRezPrompt;
extern std::string g_WipeRunBackPrompt;
extern std::vector<std::string> g_WipeTankDownLines;
extern std::vector<std::string> g_WipeApologyLines;
extern std::vector<std::string> g_WipeNoRezLines;
extern std::vector<std::string> g_WipeRunBackLines;
extern std::string g_WipeStateWipeText;
extern std::string g_WipeStateDeadText;
extern std::string g_WipeStateTankFirstText;
extern std::string g_WipeStateTankDeadText;
extern std::string g_WipeStateSelfResText;
extern std::string g_WipeStateRezzerText;
extern std::string g_WipeStateRunBackText;
extern std::string g_WipeStateNoRezzerText;
extern std::string g_WipeStateBackText;

// --------------------------------------------
// Gear talk and teasing (mod-ollama-chat_gear)
// --------------------------------------------
extern bool        g_GearEnable;
extern std::vector<std::vector<std::string>> g_GearTalkWords;
extern std::vector<std::vector<std::string>> g_GearBackOffWords;
extern uint32_t    g_GearMaxPeople;
extern uint32_t    g_GearBackOffMinutes;
extern std::string g_GearPromptText;
extern std::string g_GearBackOffText;

// --------------------------------------------
// Whispered asides in a group (mod-ollama-chat_aside)
// --------------------------------------------
extern bool        g_AsideEnable;
extern std::unordered_map<std::string, uint32_t> g_AsidePersonalities;   // style -> weight, percent
extern uint32_t    g_AsideLineChance;
extern uint32_t    g_AsideIdleChance;
extern uint32_t    g_AsideCheckSeconds;
extern uint32_t    g_AsidePlayerMinutes;
extern uint32_t    g_AsideBotMinutes;
extern uint32_t    g_AsideMinWords;
extern std::string g_AsidePrompt;
extern std::string g_AsideHistoryNote;
extern std::string g_AsideFactSaid;
extern std::string g_AsideFactClass;
extern std::string g_AsideFactLevel;
extern std::string g_AsideFactGear;
extern std::string g_AsideFactPet;
extern std::string g_AsideFactDeaths;

// --------------------------------------------
// Chat v15: where things are and how to get them; links not repeated; a guild
// message of the day nobody set
// --------------------------------------------
struct KnowledgeSynonym
{
    std::vector<std::vector<std::string>> triggers;   // "ah", "flight path"
    std::vector<std::string> adds;                    // the words they stand for
};
extern bool        g_KnowledgeEnable;
extern std::vector<std::vector<std::string>> g_KnowledgeQuestionWords;    // "where", "who sells"
extern std::vector<std::vector<std::string>> g_KnowledgeNotAskingWords;   // "where can i sell my..."
extern std::vector<std::vector<std::string>> g_KnowledgeTrainerWords;     // "trainer", "learn"
extern std::vector<KnowledgeSynonym> g_KnowledgeSynonyms;
extern float       g_KnowledgeGuardRange;
extern uint32_t    g_KnowledgeMaxGuardChars;
extern uint32_t    g_KnowledgeManyZones;
extern uint32_t    g_KnowledgeFollowSeconds;
extern std::vector<std::string> g_KnowledgeWrongOnPurpose;   // styles that answer wrong on purpose
extern std::string g_KnowledgeItemText;
extern std::string g_KnowledgeSpreadManyText;
extern std::string g_KnowledgeSpreadSomeText;
extern std::string g_KnowledgeSpreadOneText;
extern std::string g_KnowledgeKindText;
extern std::string g_KnowledgeTokensText;
extern std::string g_KnowledgeNearestText;
extern std::string g_KnowledgeCraftText;
extern std::string g_KnowledgeCraftAlsoText;
extern std::string g_KnowledgeNoVendorText;
extern std::string g_KnowledgeEnemyOnlyText;
extern std::string g_KnowledgeTradeText;
extern std::string g_KnowledgeBoundText;
extern std::string g_KnowledgePlaceText;
extern std::string g_KnowledgeMarkText;
extern std::string g_KnowledgeHeadingText;
extern std::string g_KnowledgeEarlierText;
extern std::string g_KnowledgeFollowText;
extern std::string g_KnowledgeAnswerText;
extern std::string g_KnowledgeWrongText;

extern uint32_t    g_LinksRepeatSeconds;   // the same thing is not linked again in one chat this soon
extern std::string g_LinksQuestDoneText;     // a quest named: the bot has it, done
extern std::string g_LinksQuestOnText;       // ...has it, not done yet
extern std::string g_LinksQuestRewardedText; // ...did it already
extern std::string g_LinksQuestNoneText;     // ...does not have it
extern std::string g_StateOwnWayText;        // a bot told to get on with its own thing
extern bool        g_ClockFixWeekdays;       // "for a Tuesday" on a Saturday is put right
extern std::vector<std::vector<std::string>> g_ClockWords;   // a line about days or times
extern std::string g_ClockPromptText;

extern bool        g_RollsEnable;                 // loot rolls into the group's transcript
extern uint32_t    g_RollsLowRoll;                // a roll this low or lower is as unlucky as it gets
extern uint32_t    g_RollsHighRoll;               // ...this high or higher as lucky
extern uint32_t    g_RollsReactChance;
extern uint32_t    g_RollsReactCooldownSeconds;
extern std::string g_RollsNoteText;
extern std::string g_RollsLowPrompt;
extern std::string g_RollsHighPrompt;

extern std::vector<std::string> g_GuildEnvCommentGuildMOTDUnset;   // the guild's MOTD is still the default
extern std::string g_GuildEventTypeGuildMOTD;                       // a new message of the day
extern int         g_GuildEventTypeGuildMOTD_Chance;

// --------------------------------------------
// Chat v16: bots mingling in /say (mod-ollama-chat_mingle)
// --------------------------------------------
// A bot putting on a rare or epic it just got, and saying so in party
// (mod-ollama-chat_equip)
extern bool        g_EquipEnable;
extern uint32_t    g_EquipMinQuality;           // rare (3) and up
extern uint32_t    g_EquipChance;               // for a chat style not in Equip.Personalities
extern std::unordered_map<std::string, uint32_t> g_EquipPersonalities;   // chat style -> chance
extern uint32_t    g_EquipBotCooldownMinutes;
extern uint32_t    g_EquipGroupCooldownSeconds;
extern uint32_t    g_EquipLootedMinutes;        // got it this recently: loot, a roll, a quest reward
extern std::string g_EquipPrompt;

// --------------------------------------------
// Chat v21: a rest that ends by itself (mod-ollama-chat_partycmd), and emotes
// in the transcript (mod-ollama-chat_expression)
// --------------------------------------------
extern std::string g_EmoteEventText;              // "{actor} gave {target} a /{emote}"
extern std::string g_PartyCmdHintBreak;           // the tank's ok to "mb" when the rest ends by itself
extern bool        g_PartyBreakEndEnable;
extern uint32_t    g_PartyBreakEndHealthPct;      // everyone's health back to at least this
extern uint32_t    g_PartyBreakEndManaPct;        // a real player's and a healer's mana
extern uint32_t    g_PartyBreakEndOthersManaPct;  // every other bot's mana
extern uint32_t    g_PartyBreakEndMinSeconds;     // never sooner than this after it began
extern uint32_t    g_PartyBreakEndMaxMinutes;     // longer than this: over when someone says so
extern std::string g_PartyBreakEndSituation;      // the tank's "lets go", for Party.AnnouncePrompt
extern std::vector<std::vector<std::string>> g_PartyBreakEndWords;   // the line has one of these, or...
extern std::vector<std::string> g_PartyBreakEndLines;                 // ...one of these goes out instead

// --------------------------------------------
// Chat v22: the fourth wall (mod-ollama-chat_phrases), and a bot knowing its
// own pets (mod-ollama-chat_pets)
// --------------------------------------------
extern std::vector<std::vector<std::string>> g_ResponseFourthWallPhrases;   // bots, AI, "real players"
extern std::vector<std::vector<std::string>> g_ResponseFourthWallAllow;     // "rocket bot", "repair bot"
extern uint32_t    g_ResponseFourthWallMinutes;   // after one wink at it, nobody else for this long
extern std::string g_PetsStateText;               // "...: {pets}. ..."
extern std::vector<std::vector<std::string>> g_PetsWords;   // the talk is about pets
extern std::vector<std::vector<std::string>> g_PetsMountWords;   // ...or about mounts

// --------------------------------------------
// Chat v23: what a bot has been up to lately (mod-ollama-chat_lately)
// --------------------------------------------
extern bool        g_LatelyEnable;
extern uint32_t    g_LatelyMinutes;             // how far back counts as lately
extern std::string g_LatelyStateText;           // "...: {lately}. ..."
extern std::vector<std::vector<std::string>> g_LatelyWords;   // someone asks what it has been up to

extern bool        g_MingleEnable;
extern bool        g_MingleCitiesOnly;          // only where a real player is in a city (or inn)
extern uint32_t    g_MingleGapMinSeconds;       // how often one may start near a real player
extern uint32_t    g_MingleGapMaxSeconds;
extern uint32_t    g_MingleStartChance;
extern uint32_t    g_MingleMaxActive;           // realm-wide
extern uint32_t    g_MingleMaxNearPlayer;
extern float       g_MingleHearRange;           // bots within this of the real player
extern float       g_MinglePairRange;           // ...and this of each other
extern uint32_t    g_MingleThreeChance;
extern uint32_t    g_MingleMinLines;
extern uint32_t    g_MingleMaxLines;
extern uint32_t    g_MingleMaxEmotes;
extern uint32_t    g_MingleBotCooldownSeconds;
extern uint32_t    g_MingleQueueLimit;          // not started with more requests than this waiting
extern uint32_t    g_MingleNumPredict;          // tokens for one whole exchange
extern uint32_t    g_MingleReadMinMs;           // the pause to read a line before typing the next
extern uint32_t    g_MingleReadMaxMs;
extern std::string g_MinglePromptTemplate;
extern std::vector<std::string> g_MinglePremises;
extern std::vector<std::string> g_MingleEmotes;   // offered in the prompt, by name
extern std::string g_MingleSameGuildText;
extern std::string g_MingleStrangersText;
extern std::string g_MingleLatelyText;          // a person line's "what they have been doing": {name} {lately}
extern std::string g_MingleThingsText;          // what the quests and items they have done or got are: {things}
extern std::string g_MingleLinksText;           // names in square brackets, as links
extern std::string g_OutlookFactsText;          // nothing made up from the game
extern std::unordered_map<std::string, std::string> g_PersonalityCannedStyles;   // personality -> canned style
extern bool        g_WantsEnable;               // "need a wand": where one really comes from
extern std::vector<std::vector<std::string>> g_WantsWords;   // ...said before the kind of item
extern uint32_t    g_WantsFollowSeconds;        // ...and for this long after, for follow-ups
extern std::string g_WantsText;                 // {speaker} {kind} {level} {class} {facts} {self}
extern std::string g_WantsFollowText;           // ...for a line a moment after
extern uint32_t    g_MingleDuelChance;          // two who could go and duel: how often that is the talk
extern std::string g_OutlookSideText;           // which side a bot plays, and its own cities: {side} {cities}
extern std::string g_OutlookHomeCityText;       // ...standing in one of them, that it is home: {city} {side}
extern std::vector<std::string> g_OutlookCitiesHorde;      // the Horde's own cities, by name
extern std::vector<std::string> g_OutlookCitiesAlliance;   // the Alliance's
extern std::string g_MingleSideText;            // the same for a /say exchange between bots: {side} {cities}
extern std::string g_MingleHomeCityText;        // ...{city}
extern uint32_t    g_ContextAmbientHoldSeconds; // no unprompted line where a real player spoke this lately
extern std::string g_ContextLeadUpText;         // what a short line came right after: {player_name} {lead_up}
extern uint32_t    g_ContextLeadUpMaxWords;     // ...for lines of up to this many words (0: any)
extern std::vector<std::vector<std::string>> g_SidesCriesHorde;      // "for the horde", "lok'tar ogar"
extern std::vector<std::vector<std::string>> g_SidesCriesAlliance;   // "for the alliance"
extern std::string g_SidesOwnCryText;           // their own side's cry: {player_name} {cry} {side} {the_side}
extern std::string g_SidesEnemyCryText;         // ...the other side's, in their side's chat
extern std::vector<std::pair<uint8_t, std::string>> g_RacePhrasesRaw;   // race id -> "spellings=meaning|..."
extern std::string g_RaceHeardText;             // a real player said one: {player_name} {phrase} {meaning} {race_plural} {same}
extern std::string g_RaceHeardSameText;         // ...and the bot is of that race: {a_race} {race_plural}
extern std::string g_RaceUseText;               // now and then, one of its own: {a_race} {race_plural} {phrases}
extern uint32_t    g_RaceUseChance;             // ...on this % of a bot's lines
extern std::unordered_map<std::string, uint32_t> g_RaceStyleChance;   // personality key (upper) -> %
extern std::string g_MingleRaceText;            // the same in a /say exchange: {name} {a_race} {race_plural} {phrases}
extern std::vector<std::vector<std::string>> g_BanterWords;   // "noob", "ur mum": a tease
extern std::string g_BanterText;                // ...and to give as good as it gets: {player_name} {tease}
extern bool        g_MingleEmoteEnable;         // bots greeting each other without a word
extern uint32_t    g_MingleEmoteMinSeconds;
extern uint32_t    g_MingleEmoteMaxSeconds;
extern uint32_t    g_MingleEmoteChance;
extern uint32_t    g_MingleEmoteReturnChance;
extern float       g_MingleEmoteRange;
extern uint32_t    g_MingleEmoteBotCooldownSeconds;
extern std::vector<uint32_t> g_MingleGreetings;   // TEXT_EMOTE_* two bots greet each other with

// --------------------------------------------
// Topic engine
// --------------------------------------------
extern uint32_t g_TopicWeightPeople;
extern uint32_t g_TopicWeightWorld;
extern uint32_t g_TopicWeightActivity;
extern uint32_t g_TopicWeightSelf;
extern uint32_t g_TopicWeightGuild;
extern uint32_t g_TopicWeightTalk;     // the game itself: a conversation premise as a passing remark
extern uint32_t g_TopicWeightMarket;   // what it just did on the auction house (mod-playerbots-plus)
extern uint32_t g_MarketTalkMinutes;   // ...for this long after
extern uint32_t g_TopicMemoryCount;
extern uint32_t g_TopicEventMemorySize;
extern uint32_t g_TopicEventMemorySeconds;
extern float    g_TopicPlayerRadius;
extern uint32_t g_RandomChatterQuestionChance;

extern std::vector<std::string> g_EnvCommentNearbyPlayer;
extern std::vector<std::string> g_EnvCommentGroupMember;
extern std::vector<std::string> g_EnvCommentGuildMemberOnline;
extern std::vector<std::string> g_EnvCommentRecentEvent;
extern std::vector<std::string> g_EnvCommentNamedNpc;
extern std::vector<std::string> g_EnvCommentZoneLandmark;
extern std::vector<std::string> g_EnvCommentTimeOfDay;
extern std::vector<std::string> g_EnvCommentGameTalk;   // {premise}
extern std::vector<std::string> g_EnvCommentMarketListed;   // {item} {price}
extern std::vector<std::string> g_EnvCommentMarketSold;     // {item} {price} {to}
extern std::vector<std::string> g_EnvCommentMarketUnsold;   // {item} {price}
extern std::vector<std::string> g_EnvCommentMarketBought;   // {item} {price} {from}
extern std::vector<std::string> g_EnvCommentCorpse;
extern std::vector<std::string> g_EnvCommentDanger;
extern std::vector<std::string> g_EnvCommentQuestObjective;
extern std::vector<std::string> g_EnvCommentGroupNeed;

// --------------------------------------------
// Game-state snapshot limits
// --------------------------------------------
extern bool     g_SnapshotIncludeSpells;
extern uint32_t g_SnapshotMaxSpells;
extern uint32_t g_SnapshotMaxCreatures;
extern uint32_t g_SnapshotMaxObjects;
extern uint32_t g_SnapshotMaxPlayers;

// --------------------------------------------
// Roleplay mode
// --------------------------------------------
extern bool        g_RoleplayEnable;
extern uint8_t     g_RoleplayStrictness;
extern bool        g_RoleplayUseRaceVoice;
extern bool        g_RoleplayUseClassVoice;
extern bool        g_RoleplayFactionAttitude;
extern bool        g_RoleplayBlockMetaTerms;
extern std::string g_RoleplayMetaTermList;
extern bool        g_RoleplayCrossFactionGibberish;
extern std::vector<std::string> g_RoleplayPromptVariations;
extern std::vector<std::string> g_RoleplayQuestionVariations;

// --------------------------------------------
// How long a looping emote state (dance) is allowed to run before the module
// clears it. Nothing else clears it for a bot: the core only does so on a
// movement packet from a real client. 0 leaves it set forever.
extern uint32_t g_StateEmoteDurationMs;

// Percent chance (0-100) that a reply's prompt invites a gesture at all.
// Offering it on every line makes bots gesture almost constantly. 0 = never.
extern int      g_GestureChance;

// TEXT_EMOTE_* to play when the model supplies a gesture name the table does
// not know. Resolved from its config name on the world thread at load, because
// ExtractEmoteTag runs on a worker and must not read a config string. 0 = none.
extern uint32_t g_EmoteFallbackId;

// Embodiment: facing, gestures, emote reactions
// --------------------------------------------
extern bool     g_EnableBotFacing;
extern bool     g_EnableBotEmotes;
extern uint32_t g_BotExpressionDelayMs;
extern float    g_BotFacingMaxDistance;
extern bool     g_EnableEmoteReactions;
extern uint32_t g_EmoteReplyChance;
extern uint32_t g_EmoteReplyMirrorWeight;
extern uint32_t g_EmoteReplyCounterWeight;
extern uint32_t g_EmoteReplySpeakWeight;
extern uint32_t g_EmoteReactionCooldownSeconds;
extern std::string g_EmoteReactionPromptTemplate;

// --------------------------------------------
// Long-term memory and relationships
// --------------------------------------------
extern bool        g_MemoryEnable;
extern uint32_t    g_MemoryHistoryTokenLimit;   // condense once history exceeds this
extern uint32_t    g_MemoryPromptTokenBudget;   // how much of the prompt memories may use
extern uint32_t    g_MemoryMaxPerBot;
extern uint32_t    g_MemorySaveInterval;        // minutes
extern std::string g_MemoryCondensePrompt;
extern std::string g_MemoryPromptTemplate;

extern bool        g_RelationshipEnable;
extern uint32_t    g_RelationshipMentionThreshold;
extern uint32_t    g_RelationshipMaxPerPrompt;
extern uint32_t    g_RelationshipMaxLength;
extern std::string g_RelationshipUpdatePrompt;
extern std::string g_RelationshipPromptTemplate;

extern time_t      g_LastMemorySaveTime;

// --------------------------------------------
// Environment/Contextual Random Chatter Templates
// --------------------------------------------
extern std::vector<std::string> g_EnvCommentCreature;
extern std::vector<std::string> g_EnvCommentGameObject;
extern std::vector<std::string> g_EnvCommentEquippedItem;
extern std::vector<std::string> g_EnvCommentBagItem;
extern std::vector<std::string> g_EnvCommentBagItemSell;
extern std::vector<std::string> g_EnvCommentSpell;
extern std::vector<std::string> g_EnvCommentQuestArea;
extern std::vector<std::string> g_EnvCommentVendor;
extern std::vector<std::string> g_EnvCommentQuestgiver;
extern std::vector<std::string> g_EnvCommentBagSlots;
extern std::vector<std::string> g_EnvCommentDungeon;
extern std::vector<std::string> g_EnvCommentUnfinishedQuest;

// --------------------------------------------
// Guild-Specific Random Chatter Templates
// --------------------------------------------
extern std::vector<std::string> g_GuildEnvCommentGuildMember;
extern std::vector<std::string> g_GuildEnvCommentGuildRank;
extern std::vector<std::string> g_GuildEnvCommentGuildBank;
extern std::vector<std::string> g_GuildEnvCommentGuildMOTD;
extern std::vector<std::string> g_GuildEnvCommentGuildInfo;
extern std::vector<std::string> g_GuildEnvCommentGuildOnlineMembers;
extern std::vector<std::string> g_GuildEnvCommentGuildRaid;
extern std::vector<std::string> g_GuildEnvCommentGuildEndgame;
extern std::vector<std::string> g_GuildEnvCommentGuildStrategy;
extern std::vector<std::string> g_GuildEnvCommentGuildGroup;
extern std::vector<std::string> g_GuildEnvCommentGuildPvP;
extern std::vector<std::string> g_GuildEnvCommentGuildCommunity;

// --------------------------------------------
// Guild-Specific Random Chatter Configuration
// --------------------------------------------
extern bool        g_EnableGuildEventChatter;
extern bool        g_EnableGuildRandomAmbientChatter;
extern uint32_t    g_GuildRandomChatterChance;
extern uint32_t    g_GuildChatterBotCommentChance;
extern uint32_t    g_GuildChatterMaxBotsPerEvent;

// --------------------------------------------
// Guild-Specific Event Chatter Templates
// --------------------------------------------
extern std::string g_GuildEventTypeLevelUp;
extern std::string g_GuildEventTypeDungeonComplete;
extern std::string g_GuildEventTypeEpicGear;
extern std::string g_GuildEventTypeRareGear;
extern std::string g_GuildEventTypeGuildJoin;
extern std::string g_GuildEventTypeGuildLeave;
extern std::string g_GuildEventTypeGuildPromotion;
extern std::string g_GuildEventTypeGuildDemotion;
extern std::string g_GuildEventTypeGuildLogin;
extern std::string g_GuildEventTypeGuildAchievement;

// Chance variables for normal events
extern int g_EventTypeDefeated_Chance;
extern int g_EventTypeDefeatedPlayer_Chance;
extern int g_EventTypePetDefeated_Chance;
extern int g_EventTypeGotItem_Chance;
extern int g_EventTypeDied_Chance;
extern int g_EventTypeCompletedQuest_Chance;
extern int g_EventTypeLearnedSpell_Chance;
extern int g_EventTypeRequestedDuel_Chance;
extern int g_EventTypeStartedDueling_Chance;
extern int g_EventTypeWonDuel_Chance;
extern int g_EventTypeLeveledUp_Chance;
extern int g_EventTypeAchievement_Chance;
extern int g_EventTypeUsedObject_Chance;

// Chance variables for guild events
extern int g_GuildEventTypeEpicGear_Chance;
extern int g_GuildEventTypeRareGear_Chance;
extern int g_GuildEventTypeGuildJoin_Chance;
extern int g_GuildEventTypeGuildLogin_Chance;
extern int g_GuildEventTypeGuildLeave_Chance;
extern int g_GuildEventTypeGuildPromotion_Chance;
extern int g_GuildEventTypeGuildDemotion_Chance;
extern int g_GuildEventTypeGuildAchievement_Chance;
extern int g_GuildEventTypeLevelUp_Chance;
extern int g_GuildEventTypeDungeonComplete_Chance;

// --------------------------------------------
// Bot-Player Sentiment Tracking System
// --------------------------------------------
extern bool        g_EnableSentimentTracking;
extern float       g_SentimentDefaultValue;              // Default sentiment value (0.5 = neutral)
extern float       g_SentimentAdjustmentStrength;        // How much to adjust sentiment per message (0.1)
extern uint32_t    g_SentimentSaveInterval;              // How often to save sentiment to DB (minutes)
extern std::string g_SentimentAnalysisPrompt;            // Prompt template for sentiment analysis
extern std::string g_SentimentPromptTemplate;            // Template for including sentiment in bot prompts

// In-memory sentiment storage and mutex
extern std::unordered_map<uint64_t, std::unordered_map<uint64_t, float>> g_BotPlayerSentiments;
// (bot_guid, player_guid) pairs changed since the last save. Only these are
// written; the map as a whole is not re-REPLACE INTO'd every interval.
extern std::set<std::pair<uint64_t, uint64_t>> g_DirtySentiments;
extern std::mutex g_SentimentMutex;
extern time_t g_LastSentimentSaveTime;

// --------------------------------------------
// RAG (Retrieval-Augmented Generation) System
// --------------------------------------------
extern bool        g_EnableRAG;                          // Enable/disable RAG feature
extern std::string g_RAGDataPath;                        // Path to RAG data files
extern uint32_t    g_RAGMaxRetrievedItems;               // Max items to retrieve
extern float       g_RAGSimilarityThreshold;             // Similarity threshold for retrieval
extern std::string g_RAGPromptTemplate;                  // Template for RAG info in prompts

class OllamaRAGSystem;
extern OllamaRAGSystem* g_RAGSystem;                     // Global RAG system instance

// --------------------------------------------
// Event Chatter: Event Type Strings
// These control the event type string sent to eventChatter for world event prompts.
// Values are loaded from conf (see mod_ollama_chat.conf.dist)
// --------------------------------------------
extern std::string g_EventTypeDefeated;           // "defeated"
extern std::string g_EventTypeDefeatedPlayer;     // "defeated player"
extern std::string g_EventTypePetDefeated;        // "pet defeated"
extern std::string g_EventTypeGotItem;            // "got item"
extern std::string g_EventTypeDied;               // "died"
extern std::string g_EventTypeCompletedQuest;     // "completed quest"
extern std::string g_EventTypeLearnedSpell;       // "learned spell"
extern std::string g_EventTypeRequestedDuel;      // "requested to duel"
extern std::string g_EventTypeStartedDueling;     // "started dueling"
extern std::string g_EventTypeWonDuel;            // "won duel against"
extern std::string g_EventTypeLeveledUp;          // "leveled up"
extern std::string g_EventTypeAchievement;        // "earned achievement"
extern std::string g_EventTypeUsedObject;         // "used object"

// Event Cooldown
extern uint32_t g_EventCooldownTime;

// --------------------------------------------
// Channel Disable Settings
// --------------------------------------------
extern bool g_DisableForCustomChannels;
extern bool g_DisableForSayYell;
extern bool g_DisableForGuild;
extern bool g_DisableForParty;

// Which numbered channels ambient chatter may use.
extern bool g_ChatterUseGeneralChannel;
extern bool g_ChatterUseTradeChannel;
extern bool g_ChatterUseLfgChannel;
extern bool g_ChatterUseGuildRecruitmentChannel;

// --------------------------------------------
// Typing Simulation Settings
// --------------------------------------------
extern bool g_EnableTypingSimulation;
extern uint32_t g_TypingSimulationBaseDelay;      // Base delay in milliseconds
extern uint32_t g_TypingSimulationDelayPerChar;   // Delay per character in milliseconds
extern uint32_t g_TypingSimulationMaxDelay;       // Ceiling, so a long reply is not lost

// --------------------------------------------
// Loader Functions
// --------------------------------------------
void LoadOllamaChatConfig();
void LoadBotPersonalityList();
void LoadBotConversationHistoryFromDB();
void LoadPersonalityTemplatesFromDB();

// --------------------------------------------
// Declaration of the configuration WorldScript.
// --------------------------------------------
class OllamaChatConfigWorldScript : public WorldScript
{
public:
    OllamaChatConfigWorldScript();
    void OnStartup() override;
    void OnShutdown() override;
};

#endif // MOD_OLLAMA_CHAT_CONFIG_H
