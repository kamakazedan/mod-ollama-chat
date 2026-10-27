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
extern std::string g_PartyCmdHintStart;
extern std::string g_PartyCmdHintPause;
extern std::string g_PartyCmdHintResume;
extern std::string g_PartyCmdHintWaiting;
extern std::string g_PartyDungeonGlossary;
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

// Read OllamaChat.Emote.Style.<KEY> for every personality loaded from the
// database. Called from the config load, after the personality templates.
void LoadEmoteStyles();

// --------------------------------------------
// Natural chat v7
// --------------------------------------------
// Replies. World thread only.
extern std::vector<std::vector<std::string>> g_ResponseDropVocatives;   // "kid", where it addresses someone
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

// Dead bots getting themselves back up (mod-ollama-chat_revive). World thread only.
extern bool        g_ReviveEnable;
extern uint32_t    g_ReviveReleaseDelaySeconds;
extern uint32_t    g_ReviveWaitForRezSeconds;
extern uint32_t    g_ReviveRunBackSeconds;

// Raid marks (mod-ollama-chat_marks). World thread only.
extern bool        g_MarksEnable;
extern std::string g_MarksClassIcon[16];        // by class id: "moon", "star", ... or empty
extern bool        g_MarksAddCcStrategy;
extern bool        g_MarksSapStar;
extern bool        g_MarksAutoSkull;
extern uint32_t    g_MarksInUseSeconds;

// A bot tank greeting its dungeon group and calling the finish
// (mod-ollama-chat_partycmd). World thread only.
extern std::string g_PartyAnnouncePrompt;
extern bool        g_PartyGreetEnable;
extern uint32_t    g_PartyGreetDelaySeconds;
extern std::string g_PartyGreetSituation;
extern bool        g_PartyDoneEnable;
extern std::string g_PartyDoneSituation;

// Read OllamaChat.Personality.Nudge.<KEY> and .MaxReplyLength.<KEY> for every
// personality loaded from the database. Called right after LoadEmoteStyles.
void LoadPersonalityExtras();

// --------------------------------------------
// Topic engine
// --------------------------------------------
extern uint32_t g_TopicWeightPeople;
extern uint32_t g_TopicWeightWorld;
extern uint32_t g_TopicWeightActivity;
extern uint32_t g_TopicWeightSelf;
extern uint32_t g_TopicWeightGuild;
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
