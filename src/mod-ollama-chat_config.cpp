#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_rag.h"
#include "Config.h"
#include "PlayerbotAIConfig.h"
#include "Log.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_market.h"
#include "mod-ollama-chat_marks.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_race.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_stuck.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat-utilities.h"
#include "SharedDefines.h"
#include <fmt/core.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <fstream>


// --------------------------------------------
// Distance/Range Configuration
// --------------------------------------------
float      g_SayDistance       = 30.0f;
float      g_YellDistance      = 100.0f;
float      g_RandomChatterRealPlayerDistance = 40.0f;
float      g_EventChatterRealPlayerDistance = 40.0f;

// --------------------------------------------
// Bot/Player Chatter Probability & Limits
// --------------------------------------------
// Per-channel-type reply chances
uint32_t   g_PlayerReplyChance_Say     = 90;
uint32_t   g_BotReplyChance_Say        = 10;
uint32_t   g_PlayerReplyChance_Channel = 50;
uint32_t   g_BotReplyChance_Channel    = 5;
uint32_t   g_PlayerReplyChance_Party   = 90;
uint32_t   g_BotReplyChance_Party      = 10;
uint32_t   g_PlayerReplyChance_Guild   = 70;
uint32_t   g_BotReplyChance_Guild      = 5;

uint32_t   g_MaxBotsToPick     = 2;
uint32_t   g_RandomChatterBotCommentChance   = 5;
uint32_t   g_RandomChatterMaxBotsPerPlayer   = 2;
uint32_t   g_EventChatterBotCommentChance    = 15;
uint32_t   g_EventChatterBotSelfCommentChance = 5;
uint32_t   g_EventChatterMaxBotsPerPlayer    = 2;

// --------------------------------------------
// Ollama LLM API Configuration
// --------------------------------------------
std::string g_OllamaUrl        = "http://localhost:11434/api/generate";
std::string g_OllamaModel      = "llama3.2:1b";
OllamaProvider g_OllamaProvider = OllamaProvider::Ollama;
std::string g_OllamaApiKey;
std::string g_OllamaApiKeyHeader;
int32_t     g_OllamaVerifyCertificates = -1;

namespace
{
    // Keys get pasted with stray quotes and whitespace more often than not.
    std::string TrimApiKey(const std::string& raw)
    {
        const size_t start = raw.find_first_not_of(" \t\"'");
        const size_t end   = raw.find_last_not_of(" \t\"'");
        return (start == std::string::npos) ? std::string() : raw.substr(start, end - start + 1);
    }
}

OllamaProvider OllamaProvider_Parse(const std::string& text)
{
    std::string lower = text;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const size_t start = lower.find_first_not_of(" \t\"'");
    const size_t end   = lower.find_last_not_of(" \t\"'");
    lower = (start == std::string::npos) ? std::string() : lower.substr(start, end - start + 1);

    if (lower.empty() || lower == "ollama")
        return OllamaProvider::Ollama;
    if (lower == "openai" || lower == "openrouter" || lower == "openai-compatible" || lower == "openai_compatible")
        return OllamaProvider::OpenAI;
    if (lower == "anthropic" || lower == "claude")
        return OllamaProvider::Anthropic;

    LOG_WARN("module.ollamachat",
             "[Ollama Chat] Unknown OllamaChat.Provider '{}'; expected ollama, openai or anthropic. "
             "Falling back to ollama.", text);
    return OllamaProvider::Ollama;
}

const char* OllamaProvider_Name(OllamaProvider provider)
{
    switch (provider)
    {
        case OllamaProvider::OpenAI:    return "openai";
        case OllamaProvider::Anthropic: return "anthropic";
        default:                        return "ollama";
    }
}
uint32_t    g_OllamaNumPredict = 40;
float       g_OllamaTemperature = 0.8f;
float       g_OllamaTopP = 0.95f;
float       g_OllamaRepeatPenalty = 1.1f;
uint32_t    g_OllamaNumCtx = 0;
uint32_t    g_OllamaNumThreads = 0;
std::string g_OllamaStop = "";
std::string g_OllamaSystemPrompt = "";
std::string g_OllamaSeed = "";
int32_t     g_OllamaTopK             = -1;
float       g_OllamaMinP             = -1.0f;
float       g_OllamaPresencePenalty  = -1000.0f;
float       g_OllamaFrequencyPenalty = -1000.0f;

// --------------------------------------------
// Concurrency/Queueing
// --------------------------------------------
uint32_t    g_MaxConcurrentQueries = 0;

// --------------------------------------------
// Feature Toggles & Core Settings
// --------------------------------------------
bool        g_Enable                          = true;
bool        g_DisableRepliesInCombat          = true;
bool        g_EnableRandomChatter             = true;
bool        g_EnableEventChatter              = true;
bool        g_EnableRPPersonalities           = false;
bool        g_EnableWhisperReplies            = false;
bool        g_DebugEnabled                    = false;
bool        g_DebugShowFullPrompt             = false;

// --------------------------------------------
// Think Mode
// --------------------------------------------
bool     g_ThinkModeEnableForModule      = false;
uint8_t  g_ThinkModePolicy               = 0;      // OllamaThinkPolicy::Auto
uint32_t g_ThinkMaxLatencyMs             = 12000;
uint32_t g_ReasoningTokenReserve         = 512;
uint32_t g_CapabilityProbeTimeoutSeconds = 10;

// --------------------------------------------
// HTTP / dispatcher
// --------------------------------------------
uint32_t g_HttpTimeoutSeconds     = 120;
uint32_t g_RateLimitRetries       = 2;
uint32_t g_RateLimitMaxWaitSeconds = 15;
uint32_t g_DispatchWorkerThreads  = 4;
uint32_t g_MaxQueueDepth          = 64;

// --------------------------------------------
// Response post-processing
// --------------------------------------------
uint32_t g_MaxReplyLength                = 240;
bool     g_ResponseStripMarkdown         = true;
bool     g_ResponseStripDecorativeUnicode = true;

// --------------------------------------------
// Conversation governor
// --------------------------------------------
uint8_t  g_MaxChainDepth                 = 3;
uint32_t g_ChainChanceDecayPct           = 50;
bool     g_RequireRecentHuman            = true;
uint32_t g_HumanWindowSeconds            = 120;
uint32_t g_BotCooldownSeconds            = 45;
uint32_t g_ScopeCooldownSeconds          = 15;
uint32_t g_DirectAddressGroupSize        = 5;
uint32_t g_ConversationWindowSeconds     = 180;
uint32_t g_DirectAddressReplyChance      = 95;
uint32_t g_ScopeMessagesPerMinute        = 8;
uint32_t g_GlobalMessagesPerMinute       = 40;
uint32_t g_BotHistorySize                = 12;
uint32_t g_ScopeHistorySize              = 30;
float    g_RepetitionSimilarityThreshold = 0.72f;
uint32_t g_RepetitionWindowSeconds       = 1800;
uint32_t g_OpenerHistorySize             = 8;
uint32_t g_RepetitionShortLineWindowSeconds = 300;
std::string g_RepetitionOwnRepeatNote;
bool        g_EquipEnable               = true;
uint32_t    g_EquipMinQuality           = 3;
uint32_t    g_EquipChance               = 20;
std::unordered_map<std::string, uint32_t> g_EquipPersonalities;
uint32_t    g_EquipBotCooldownMinutes   = 20;
uint32_t    g_EquipGroupCooldownSeconds = 90;
uint32_t    g_EquipLootedMinutes        = 10;
std::string g_EquipPrompt;

std::string g_EmoteEventText;
std::string g_PartyCmdHintBreak;
bool        g_PartyBreakEndEnable        = true;
uint32_t    g_PartyBreakEndHealthPct     = 90;
uint32_t    g_PartyBreakEndManaPct       = 95;
uint32_t    g_PartyBreakEndOthersManaPct = 80;
uint32_t    g_PartyBreakEndMinSeconds    = 10;
uint32_t    g_PartyBreakEndMaxMinutes    = 5;
std::string g_PartyBreakEndSituation;
std::vector<std::vector<std::string>> g_PartyBreakEndWords;
std::vector<std::string> g_PartyBreakEndLines;

// Chat v22
std::vector<std::vector<std::string>> g_ResponseFourthWallPhrases;
std::vector<std::vector<std::string>> g_ResponseFourthWallAllow;
uint32_t    g_ResponseFourthWallMinutes = 10;
std::string g_PetsStateText;
std::vector<std::vector<std::string>> g_PetsWords;
std::vector<std::vector<std::string>> g_PetsMountWords;

// Chat v23
bool        g_LatelyEnable  = true;
uint32_t    g_LatelyMinutes = 45;
std::string g_LatelyStateText;
std::vector<std::vector<std::string>> g_LatelyWords;

bool        g_MingleEnable              = true;
bool        g_MingleCitiesOnly          = true;
uint32_t    g_MingleGapMinSeconds       = 80;
uint32_t    g_MingleGapMaxSeconds       = 200;
uint32_t    g_MingleStartChance         = 70;
uint32_t    g_MingleMaxActive           = 4;
uint32_t    g_MingleMaxNearPlayer       = 2;
float       g_MingleHearRange           = 25.0f;
float       g_MinglePairRange           = 9.0f;
uint32_t    g_MingleThreeChance         = 25;
uint32_t    g_MingleMinLines            = 2;
uint32_t    g_MingleMaxLines            = 10;
uint32_t    g_MingleMaxEmotes           = 3;
uint32_t    g_MingleBotCooldownSeconds  = 600;
uint32_t    g_MingleQueueLimit          = 4;
uint32_t    g_MingleNumPredict          = 320;
uint32_t    g_MingleReadMinMs           = 1200;
uint32_t    g_MingleReadMaxMs           = 3500;
std::string g_MinglePromptTemplate;
std::vector<std::string> g_MinglePremises;
std::vector<std::string> g_MingleEmotes;
std::string g_MingleSameGuildText;
std::string g_MingleStrangersText;
std::string g_MingleLatelyText;
std::string g_MingleThingsText;
std::string g_MingleLinksText;
std::string g_OutlookFactsText;
std::unordered_map<std::string, std::string> g_PersonalityCannedStyles;
bool        g_WantsEnable          = true;
std::vector<std::vector<std::string>> g_WantsWords;
uint32_t    g_WantsFollowSeconds   = 120;
std::string g_WantsText;
std::string g_WantsFollowText;
uint32_t    g_MingleDuelChance     = 25;
std::string g_OutlookSideText;
std::string g_OutlookHomeCityText;
std::vector<std::string> g_OutlookCitiesHorde;
std::vector<std::string> g_OutlookCitiesAlliance;
std::string g_MingleSideText;
std::string g_MingleHomeCityText;
uint32_t    g_ContextAmbientHoldSeconds = 60;
std::string g_ContextLeadUpText;
uint32_t    g_ContextLeadUpMaxWords     = 8;
std::vector<std::vector<std::string>> g_SidesCriesHorde;
std::vector<std::vector<std::string>> g_SidesCriesAlliance;
std::string g_SidesOwnCryText;
std::string g_SidesEnemyCryText;
std::vector<std::pair<uint8_t, std::string>> g_RacePhrasesRaw;
std::string g_RaceHeardText;
std::string g_RaceHeardSameText;
std::string g_RaceUseText;
uint32_t    g_RaceUseChance             = 3;
std::unordered_map<std::string, uint32_t> g_RaceStyleChance;
std::string g_MingleRaceText;
std::vector<std::vector<std::string>> g_BanterWords;
std::string g_BanterText;
bool        g_MingleEmoteEnable         = true;
uint32_t    g_MingleEmoteMinSeconds     = 60;
uint32_t    g_MingleEmoteMaxSeconds     = 160;
uint32_t    g_MingleEmoteChance         = 60;
uint32_t    g_MingleEmoteReturnChance   = 60;
float       g_MingleEmoteRange          = 8.0f;
uint32_t    g_MingleEmoteBotCooldownSeconds = 300;
std::vector<uint32_t> g_MingleGreetings;
TextRules::QuoteSources g_ResponseQuoteSources;
std::unordered_set<std::string> g_ResponseQuoteSourceStyles;

// --------------------------------------------
// Shared chat context
// --------------------------------------------
bool        g_ContextEnable                   = true;
uint32_t    g_ContextMaxLines                 = 10;
uint32_t    g_ContextMaxAgeSeconds            = 300;
uint32_t    g_ContextReplyWindowSeconds       = 60;
uint32_t    g_ContextBystanderChancePct       = 25;
uint32_t    g_ContextBotAddressedChance       = 40;
bool        g_ContextBotReplyPerMessage       = true;
uint32_t    g_ContextBotLineReplyChance_Say     = 50;
uint32_t    g_ContextBotLineReplyChance_Party   = 60;
uint32_t    g_ContextBotLineReplyChance_Guild   = 50;
uint32_t    g_ContextBotLineReplyChance_Channel = 40;
bool        g_ContextBotChatWithoutRecentHuman  = true;
bool        g_ContextSuppressPairHistory      = true;
std::string g_ContextPassToken                = "NOREPLY";
std::string g_ContextPassTokenNorm            = "noreply";
std::string g_ContextHistoryHeader;
std::string g_ContextHintAddressed;
std::string g_ContextHintBystander;
std::string g_ContextHintOpen;
std::string g_ContextHintPublicWithGroup;
std::string g_ContextHintEvent;
std::string g_ContextHintAmbient;
std::vector<std::string> g_ContextFillerWords;
uint32_t    g_ContextMaxFillersPerLine        = 1;
uint32_t    g_ContextFillerScopeLines         = 6;
uint32_t    g_ContextFillerBotCooldownSeconds = 900;

// --------------------------------------------
// Crowd replies
// --------------------------------------------
bool        g_CrowdEnable                 = true;
uint32_t    g_CrowdMaxResponders          = 5;
uint32_t    g_CrowdBotsPerResponder       = 3;
uint32_t    g_CrowdChimeInChance          = 40;
uint32_t    g_CrowdChimeInMax             = 2;
uint32_t    g_CrowdAckReplyChance         = 50;
uint32_t    g_CrowdGapMinMs               = 1000;
uint32_t    g_CrowdGapMaxMs               = 4000;
uint32_t    g_CrowdSlotTimeoutSeconds     = 30;
uint32_t    g_CrowdMaxPlanSeconds         = 120;
uint32_t    g_CrowdMaxReplyBacks          = 2;
uint32_t    g_CrowdBotLineSecondChance    = 25;
std::vector<std::vector<std::string>> g_CrowdOpenCallPhrases;
std::vector<std::vector<std::string>> g_CrowdAckWords;
std::unordered_set<std::string>       g_CrowdPlayfulPersonalities;
std::string g_CrowdHintOpenCall;
std::string g_CrowdHintFollowUp;
std::string g_CrowdHintReplyBack;
std::string g_CrowdHintShout;
std::vector<std::vector<std::string>> g_CrowdGreetingWords;
bool        g_CrowdFullCrowdInCities      = true;

// --------------------------------------------
// Natural chat v4
// --------------------------------------------
std::string g_EventChatterSelfPromptTemplate;
std::vector<std::string> g_EventCongratsLines;
uint32_t    g_EventCongratsCannedChance    = 50;
bool        g_JoinedEnable                 = true;
uint32_t    g_JoinedGreetChance            = 70;
uint32_t    g_JoinedGroupCooldownSeconds   = 60;
uint32_t    g_JoinedContinueMinutes        = 10;
uint32_t    g_JoinedCannedChance           = 50;
std::vector<std::string> g_JoinedGreetLines;
std::string g_JoinedGreetPrompt;
std::string g_JoinedContinuePrompt;
uint32_t    g_JoinedDelayMinMs             = 2000;
uint32_t    g_JoinedDelayMaxMs             = 5000;
bool        g_CongratsEnable               = true;
uint32_t    g_CongratsWindowSeconds        = 300;
std::vector<std::vector<std::string>> g_CongratsWords;
std::vector<std::vector<std::string>> g_CongratsFillerWords;
std::vector<std::string> g_CongratsThanksLines;
std::vector<std::string> g_CongratsThanksAllLines;
uint32_t    g_CongratsThanksChance         = 90;
uint32_t    g_CongratsThanksDelayMinMs     = 2500;
uint32_t    g_CongratsThanksDelayMaxMs     = 6000;
bool        g_CongratsAchievementsRareOnly  = true;
uint32_t    g_CongratsAchievementRarePoints = 25;
std::unordered_set<uint32_t> g_CongratsAchievementAlways;
std::unordered_set<uint32_t> g_CongratsAchievementNever;
std::bitset<256> g_CongratsGuildLevels;

std::vector<std::vector<std::string>> g_ResponseAssistantPhrases;

std::unordered_map<std::string, std::vector<uint32_t>> g_EmoteStyles;
std::vector<uint32_t>        g_EmoteStyleDefault;
std::unordered_set<uint32_t> g_EmotePetEmotes;

bool        g_PresenceEnable               = true;
float       g_PresenceRange                = 10.0f;
uint32_t    g_PresencePlayerIntervalMin    = 45;
uint32_t    g_PresencePlayerIntervalMax    = 150;
uint32_t    g_PresenceBotCooldownSeconds   = 900;
uint32_t    g_PresencePairCooldownSeconds  = 3600;
uint32_t    g_PresenceChance               = 60;
uint32_t    g_PresenceSayChance            = 35;
bool        g_PresenceCityOnly             = false;
std::string g_PresencePromptTemplate;

bool        g_CityEnable                   = true;
std::unordered_set<uint32_t> g_CityExtraIds;
bool        g_CityIncludeInns              = true;
uint32_t    g_CityChatterMultiplier        = 3;
uint32_t    g_CityConversationStartChance  = 35;
uint32_t    g_CityBotLineReplyBonus        = 20;
uint32_t    g_CityConversationBoost        = 2;

bool        g_ConversationEnable           = true;
uint32_t    g_ConversationMinMinutes       = 2;
uint32_t    g_ConversationMaxMinutes       = 15;
uint32_t    g_ConversationGapMinSeconds    = 6;
uint32_t    g_ConversationGapMaxSeconds    = 25;
uint32_t    g_ConversationLullChance       = 12;
uint32_t    g_ConversationLullMinSeconds   = 30;
uint32_t    g_ConversationLullMaxSeconds   = 75;
uint32_t    g_ConversationMaxActive        = 3;
uint32_t    g_ConversationMaxParticipants  = 5;
uint32_t    g_ConversationJoinChance       = 15;
uint32_t    g_ConversationFromPlayerChance = 40;
uint32_t    g_ConversationFromBotLineChance = 15;
uint32_t    g_ConversationCooldownSeconds  = 120;
uint32_t    g_ConversationMaxMisses        = 2;
uint32_t    g_ConversationObserveChance    = 30;
bool        g_ConversationAllowTrade       = true;
bool        g_ConversationAllowSay         = true;
std::vector<std::string> g_ConversationPremises;
std::string g_ConversationHintTurn;
std::string g_ConversationHintJoin;
std::string g_ConversationHintWindDown;
std::string g_ConversationHintStartChannel;
std::string g_ConversationHintStartSay;
std::string g_ConversationHintObserve;

bool        g_PartyCmdEnable               = true;
TextRules::PartyRules g_PartyCmdRules;
float       g_PartyCmdMaxDistance          = 100.0f;
std::vector<std::vector<std::string>> g_PartyRunQuestionPhrases;
uint32_t    g_PartyRunQuestionMaxWords     = 14;
std::string g_PartyCmdHintStart;
std::string g_PartyCmdHintPause;
std::string g_PartyCmdHintResume;
std::string g_PartyCmdHintWaiting;
std::string g_PartyDungeonGlossary;
uint32_t    g_PartyDungeonAmbientPercent   = 35;
std::string g_PartyDungeonFocusText;
uint32_t    g_PartyCmdRecentAcks           = 4;
std::string g_PartyRunStateText;
std::string g_PartyCmdRecentAcksText;

bool        g_OutlookEnable                = true;
std::string g_OutlookLevelBands;
bool        g_OutlookClassRoles            = true;
std::string g_OutlookKnowledgeText;
std::vector<TextRules::LevelPhrase> g_OutlookLevelPhrases;

bool        g_LinksEnable                  = true;
uint32_t    g_LinksMaxPerLine              = 2;
uint32_t    g_LinksRememberMinutes         = 60;
bool        g_LinksUseGameData             = true;
std::string g_LinksPromptHint;

std::vector<std::vector<std::string>> g_ResponseOverusedPhrases;
uint32_t    g_ResponseOverusedCooldownSeconds = 1200;
std::vector<std::vector<std::string>> g_ResponseOffEraPhrases;

bool        g_ConjureEnable                = true;
TextRules::ConjureRules g_ConjureRules;
uint32_t    g_ConjureStacks                = 1;
uint32_t    g_ConjureMaxStacks             = 3;
bool        g_ConjureUseMana               = true;
uint32_t    g_ConjureWaitSeconds           = 90;
uint32_t    g_ConjureTradeSeconds          = 60;
std::string g_ConjureHintGive;
std::string g_ConjureHintWait;
std::string g_ConjureHintCant;
std::string g_ConjureHintNotMage;
std::string g_ConjureHintAlready;
std::string g_ConjureHintNotGrouped;

std::unordered_map<std::string, PersonalityNudge> g_PersonalityNudges;
std::unordered_map<std::string, uint32_t>         g_PersonalityMaxReplyLength;
std::string g_PersonalityLongReplyNote;
std::string g_PersonalitySaidLatelyNote;
uint32_t    g_PersonalitySaidLatelyLines = 5;

std::vector<std::vector<std::string>> g_ResponseDropVocatives;
std::vector<std::vector<std::string>> g_ResponseInsultWords;
std::vector<std::vector<std::string>> g_ResponseProvokeWords;
std::vector<std::string> g_ResponseInsultPersonalities;
uint32_t    g_ResponseInsultCooldownSeconds = 600;
std::vector<std::vector<std::string>> g_ResponseRarePhrases;
uint32_t    g_ResponseRareCooldownSeconds  = 3600;
std::vector<std::vector<std::string>> g_CrowdThanksWords;
uint32_t    g_CrowdThanksReplyChance       = 0;
std::string g_StateDeadText;
std::string g_StateGhostText;
bool        g_OrdersEnable                 = true;
TextRules::BotCommandRules g_OrdersRules;
std::string g_OrdersHint[static_cast<size_t>(TextRules::BotIntent::Count)];
std::string g_OrdersHintCant;
uint32_t    g_OrdersTradeWalkSeconds       = 20;
uint32_t    g_OrdersFleeRecoverSeconds     = 8;
std::string g_StateStayingText;
bool        g_MarksSayEnable               = true;
uint32_t    g_MarksSayCooldownSeconds      = 120;
std::string g_MarksSayPrompt;
std::string g_MarksSapNotLearned;
std::string g_MarksSapWrongType;
std::string g_MarksSapNoStealth;
std::string g_MarksSapFailed;

bool        g_BagsEnable                   = true;
uint32_t    g_BagsRecentMinutes            = 30;
uint32_t    g_BagsOfferSeconds             = 180;
uint32_t    g_BagsInviteWaitSeconds        = 120;
uint32_t    g_BagsMaxSpare                 = 6;
uint32_t    g_BagsMaxKept                  = 3;
std::string g_BagsPromptText;
std::vector<std::vector<std::string>> g_BagsTalkPhrases;
std::vector<std::vector<std::string>> g_BagsAskPhrases;
std::vector<std::vector<std::string>> g_BagsYesPhrases;
std::vector<std::vector<std::string>> g_BagsClaimPhrases;
uint32_t    g_BagsMaxGiftQuality           = 2;
uint32_t    g_BagsMaxGiftCopper            = 20000;
bool        g_BagsGiveAway                 = false;
std::string g_BagsNoGiftReason;
std::string g_BagsSupplyReason;
std::vector<std::vector<std::string>> g_BagsNotPhrases;
std::string g_BagsHintGive;
std::string g_BagsHintInvite;
std::string g_BagsHintKeep;
std::string g_BagsHintNotHave;
std::string g_BagsHintCant;
std::string g_BagsHintAlready;
std::string g_BagsHandoverText;

bool        g_NeedsEnable                  = true;
uint32_t    g_NeedsChance                  = 20;
uint32_t    g_NeedsBotCooldownMinutes      = 20;
uint32_t    g_NeedsRepeatMinutes           = 60;
uint32_t    g_NeedsZoneCooldownSeconds     = 300;
uint32_t    g_NeedsRememberMinutes         = 15;
uint32_t    g_NeedsWeight[4]               = { 40, 25, 25, 10 };
uint32_t    g_NeedsShortMaxLevel           = 15;
uint32_t    g_NeedsShortCopperPerLevel     = 1200;
std::string g_NeedsPromptTemplate;
std::string g_NeedsReasonText;
std::string g_NeedsPlanText;
std::vector<std::vector<std::string>> g_NeedsPlanPhrases;
uint32_t    g_NeedsPlanMaxQuests = 4;
std::string g_NeedsAskQuest;
std::string g_NeedsAskDungeon;
std::string g_NeedsReasonQuest;
std::string g_NeedsAskErrand;
std::string g_NeedsReasonErrand;
std::string g_NeedsAskSpare;
std::string g_NeedsReasonSpare;
std::string g_NeedsAskShort;
std::string g_NeedsReasonShort;
std::string g_PartyAnnouncePrompt;
bool        g_PartyGreetEnable             = true;
uint32_t    g_PartyGreetDelaySeconds       = 8;
std::string g_PartyGreetSituation;
bool        g_PartyDoneEnable              = true;
std::string g_PartyDoneSituation;
std::vector<std::vector<std::string>> g_PartyDoneWords;
std::vector<std::string> g_PartyDoneLines;
bool        g_PartyCheerEnable             = true;
uint32_t    g_PartyCheerChance             = 85;
uint32_t    g_PartyCheerCannedChance       = 60;
std::vector<std::string> g_PartyCheerLines;
std::string g_PartyCheerPrompt;
bool        g_PartyDoneLeave               = true;
uint32_t    g_PartyDoneLeaveSeconds        = 30;
std::vector<std::vector<std::string>> g_PartyDoneStayWords;
std::vector<std::vector<std::string>> g_PartyDoneGoWords;
std::string g_PartyDoneStateText;
std::string g_PartyDoneLeavingText;
std::string g_PartyDoneStayingText;

// --------------------------------------------
// Natural chat v10
// --------------------------------------------
uint32_t    g_StuckRememberMinutes         = 10;
uint32_t    g_StuckRemarkChance            = 50;
std::string g_StuckRemarkPrompt;
std::string g_StuckStateText;

bool        g_ProgressEnable               = true;
uint32_t    g_ProgressCheckSeconds         = 2;
uint32_t    g_ProgressDelaySeconds         = 3;
uint32_t    g_ProgressGapSeconds           = 20;
std::string g_ProgressPrompt;
std::string g_ProgressKillText;
std::string g_ProgressUseText;
std::string g_ProgressItemText;
std::string g_ProgressQuestText;

bool        g_LinksDescribeEnable          = true;
uint32_t    g_LinksDescribeMax             = 3;
uint32_t    g_LinksDescribeQuestChars      = 180;
std::string g_LinksDescribeText;

std::vector<std::string> g_PremiseTemplates;
uint32_t    g_PremiseTemplateChance        = 70;
uint32_t    g_PremiseCooldownMinutes       = 120;
uint32_t    g_PremiseValueMemory           = 8;
std::unordered_map<std::string, std::vector<std::string>> g_PremisePools;

bool        g_EchoEnable                   = true;
uint32_t    g_EchoWords                    = 4;
uint32_t    g_EchoBots                     = 3;
uint32_t    g_EchoWindowMinutes            = 60;
std::vector<std::string> g_EchoCommonWords;
bool        g_ResponseRetryOwed            = true;
std::string g_ResponseRetryNote;

bool        g_EmoticonEnable               = true;
std::vector<std::string> g_EmoticonTokens;
std::vector<std::string> g_EmoticonOffer;
uint32_t    g_EmoticonNudgeChance          = 4;
std::unordered_map<std::string, uint32_t> g_EmoticonStyleChance;
std::string g_EmoticonNudgeText;
uint32_t    g_EmoticonBotCooldownMinutes   = 20;
uint32_t    g_EmoticonScopeLines           = 8;

std::vector<std::vector<std::string>> g_PartyReadyYesPhrases;
uint32_t    g_PartyReadyWindowSeconds      = 180;
std::string g_PartyTogetherText;
std::string g_JoinedLfgPrompt;

std::vector<std::vector<std::string>> g_BagsCancelPhrases;
std::vector<std::vector<std::string>> g_BagsCancelNotPhrases;
std::string g_BagsHintCancel;

std::vector<std::vector<std::string>> g_OrdersNotDoneVerbs;
std::string g_OrdersHintNotDone;

uint32_t    g_PresenceExGroupMinutes       = 15;
std::unordered_set<uint32_t> g_PresenceSkipEmotes;

uint32_t    g_LinksQuestLevelsAbove        = 3;
bool        g_LinksQuietWhenTooLow         = true;
std::string g_LinksQuestTooHighText;
std::string g_LinksQuestEasyText;
std::string g_LinksItemTooHighText;

std::string g_HangoutStateText;
std::string g_HangoutAwayText;

// --------------------------------------------
// A player giving away or selling something
// --------------------------------------------
bool        g_GiftsEnable                  = true;
std::vector<std::vector<std::string>> g_GiftsOfferWords;
std::vector<std::vector<std::string>> g_GiftsSellWords;
std::vector<std::string> g_GiftsGreedyPersonalities;
uint32_t    g_GiftsSecondVoiceChance       = 35;
uint32_t    g_GiftsNobodyChance            = 60;
uint32_t    g_GiftsFollowUpSeconds         = 90;
uint32_t    g_GiftsRememberSeconds         = 300;
uint32_t    g_GiftsMaxSpendPercent         = 50;
std::string g_GiftsHintWant;
std::string g_GiftsHintWantToo;
std::string g_GiftsHintNoNeed;
std::string g_GiftsHintNotTradable;
std::string g_GiftsHintAskPrice;
std::string g_GiftsHintBuy;
std::string g_GiftsHintCounter;
std::string g_GiftsHintTooDear;
std::string g_GiftsHintNoBuy;
std::string g_GiftsInviteText;
std::string g_GiftsSaidGiftText;
std::string g_GiftsSaidBuyText;
std::string g_GiftsSaidCounterText;
std::string g_GiftsSaidAskText;

// --------------------------------------------
// The tank down, a wipe, nobody to rez
// --------------------------------------------
bool        g_WipeEnable                   = true;
uint32_t    g_WipeTankDownChance           = 60;
uint32_t    g_WipeApologyChance            = 75;
uint32_t    g_WipeNoRezChance              = 90;
uint32_t    g_WipeRunBackChance            = 60;
uint32_t    g_WipeTypedChance              = 35;
uint32_t    g_WipeRunBackQuietSeconds      = 4;
uint32_t    g_WipeStateMinutes             = 5;
std::string g_WipeTankDownPrompt;
std::string g_WipeApologyPrompt;
std::string g_WipeNoRezPrompt;
std::string g_WipeRunBackPrompt;
std::vector<std::string> g_WipeTankDownLines;
std::vector<std::string> g_WipeApologyLines;
std::vector<std::string> g_WipeNoRezLines;
std::vector<std::string> g_WipeRunBackLines;
std::string g_WipeStateWipeText;
std::string g_WipeStateDeadText;
std::string g_WipeStateTankFirstText;
std::string g_WipeStateTankDeadText;
std::string g_WipeStateSelfResText;
std::string g_WipeStateRezzerText;
std::string g_WipeStateRunBackText;
std::string g_WipeStateNoRezzerText;
std::string g_WipeStateBackText;

// --------------------------------------------
// Gear talk and teasing
// --------------------------------------------
bool        g_GearEnable                   = true;
std::vector<std::vector<std::string>> g_GearTalkWords;
std::vector<std::vector<std::string>> g_GearBackOffWords;
uint32_t    g_GearMaxPeople                = 4;
uint32_t    g_GearBackOffMinutes           = 5;
std::string g_GearPromptText;
std::string g_GearBackOffText;

// --------------------------------------------
// Whispered asides in a group
// --------------------------------------------
bool        g_AsideEnable                  = true;
std::unordered_map<std::string, uint32_t> g_AsidePersonalities;
uint32_t    g_AsideLineChance              = 10;
uint32_t    g_AsideIdleChance              = 4;
uint32_t    g_AsideCheckSeconds            = 60;
uint32_t    g_AsidePlayerMinutes           = 20;
uint32_t    g_AsideBotMinutes              = 45;
uint32_t    g_AsideMinWords                = 4;
std::string g_AsidePrompt;
std::string g_AsideHistoryNote;
std::string g_AsideFactSaid;
std::string g_AsideFactClass;
std::string g_AsideFactLevel;
std::string g_AsideFactGear;
std::string g_AsideFactPet;
std::string g_AsideFactDeaths;

// --------------------------------------------
// Chat v15
// --------------------------------------------
bool        g_KnowledgeEnable          = true;
std::vector<std::vector<std::string>> g_KnowledgeQuestionWords;
std::vector<std::vector<std::string>> g_KnowledgeNotAskingWords;
std::vector<std::vector<std::string>> g_KnowledgeTrainerWords;
std::vector<KnowledgeSynonym> g_KnowledgeSynonyms;
float       g_KnowledgeGuardRange      = 500.0f;
uint32_t    g_KnowledgeMaxGuardChars   = 240;
uint32_t    g_KnowledgeManyZones       = 8;
uint32_t    g_KnowledgeFollowSeconds   = 120;
std::vector<std::string> g_KnowledgeWrongOnPurpose;
std::string g_KnowledgeItemText;
std::string g_KnowledgeSpreadManyText;
std::string g_KnowledgeSpreadSomeText;
std::string g_KnowledgeSpreadOneText;
std::string g_KnowledgeKindText;
std::string g_KnowledgeTokensText;
std::string g_KnowledgeNearestText;
std::string g_KnowledgeCraftText;
std::string g_KnowledgeCraftAlsoText;
std::string g_KnowledgeNoVendorText;
std::string g_KnowledgeEnemyOnlyText;
std::string g_KnowledgeTradeText;
std::string g_KnowledgeBoundText;
std::string g_KnowledgePlaceText;
std::string g_KnowledgeMarkText;
std::string g_KnowledgeHeadingText;
std::string g_KnowledgeEarlierText;
std::string g_KnowledgeFollowText;
std::string g_KnowledgeAnswerText;
std::string g_KnowledgeWrongText;

uint32_t    g_LinksRepeatSeconds       = 180;
std::string g_LinksQuestDoneText;
std::string g_LinksQuestOnText;
std::string g_LinksQuestRewardedText;
std::string g_LinksQuestNoneText;
std::string g_StateOwnWayText;
bool        g_ClockFixWeekdays         = true;
std::vector<std::vector<std::string>> g_ClockWords;
std::string g_ClockPromptText;
bool        g_RollsEnable              = true;
uint32_t    g_RollsLowRoll             = 2;
uint32_t    g_RollsHighRoll            = 99;
uint32_t    g_RollsReactChance         = 50;
uint32_t    g_RollsReactCooldownSeconds = 60;
std::string g_RollsNoteText;
std::string g_RollsLowPrompt;
std::string g_RollsHighPrompt;

std::vector<std::string> g_GuildEnvCommentGuildMOTDUnset;
std::string g_GuildEventTypeGuildMOTD;
int         g_GuildEventTypeGuildMOTD_Chance = 100;

// --------------------------------------------
// Topic engine
// --------------------------------------------
uint32_t g_TopicWeightPeople        = 30;
uint32_t g_TopicWeightWorld         = 30;
uint32_t g_TopicWeightActivity      = 25;
uint32_t g_TopicWeightSelf          = 15;
uint32_t g_TopicWeightGuild         = 20;
uint32_t g_TopicWeightTalk          = 25;
uint32_t g_TopicWeightMarket        = 30;
uint32_t g_MarketTalkMinutes        = 90;
uint32_t g_TopicMemoryCount         = 4;
uint32_t g_TopicEventMemorySize     = 6;
uint32_t g_TopicEventMemorySeconds  = 300;
float    g_TopicPlayerRadius        = 40.0f;
uint32_t g_RandomChatterQuestionChance = 35;

std::vector<std::string> g_EnvCommentNearbyPlayer;
std::vector<std::string> g_EnvCommentGroupMember;
std::vector<std::string> g_EnvCommentGuildMemberOnline;
std::vector<std::string> g_EnvCommentRecentEvent;
std::vector<std::string> g_EnvCommentNamedNpc;
std::vector<std::string> g_EnvCommentZoneLandmark;
std::vector<std::string> g_EnvCommentTimeOfDay;
std::vector<std::string> g_EnvCommentGameTalk;
std::vector<std::string> g_EnvCommentMarketListed;
std::vector<std::string> g_EnvCommentMarketSold;
std::vector<std::string> g_EnvCommentMarketUnsold;
std::vector<std::string> g_EnvCommentMarketBought;
std::vector<std::string> g_EnvCommentCorpse;
std::vector<std::string> g_EnvCommentDanger;
std::vector<std::string> g_EnvCommentQuestObjective;
std::vector<std::string> g_EnvCommentGroupNeed;

// --------------------------------------------
// Game-state snapshot limits
// --------------------------------------------
bool     g_SnapshotIncludeSpells = false;
uint32_t g_SnapshotMaxSpells     = 6;
uint32_t g_SnapshotMaxCreatures  = 8;
uint32_t g_SnapshotMaxObjects    = 8;
uint32_t g_SnapshotMaxPlayers    = 8;

// --------------------------------------------
// Roleplay mode
// --------------------------------------------
bool        g_RoleplayEnable               = false;
uint8_t     g_RoleplayStrictness           = 1;
bool        g_RoleplayUseRaceVoice         = true;
bool        g_RoleplayUseClassVoice        = true;
bool        g_RoleplayFactionAttitude      = true;
bool        g_RoleplayBlockMetaTerms       = true;
std::string g_RoleplayMetaTermList;
bool        g_RoleplayCrossFactionGibberish = true;
std::vector<std::string> g_RoleplayPromptVariations;
std::vector<std::string> g_RoleplayQuestionVariations;

// --------------------------------------------
// Embodiment
// --------------------------------------------
bool        g_EnableBotFacing              = true;
bool        g_EnableBotEmotes              = true;
uint32_t    g_StateEmoteDurationMs         = 15000;
int         g_GestureChance                = 40;
uint32_t    g_EmoteFallbackId              = 0;
uint32_t    g_BotExpressionDelayMs         = 400;
float       g_BotFacingMaxDistance         = 40.0f;
bool        g_EnableEmoteReactions         = true;
uint32_t    g_EmoteReplyChance             = 60;
uint32_t    g_EmoteReplyMirrorWeight       = 55;
uint32_t    g_EmoteReplyCounterWeight      = 30;
uint32_t    g_EmoteReplySpeakWeight        = 15;
uint32_t    g_EmoteReactionCooldownSeconds = 20;
std::string g_EmoteReactionPromptTemplate;

// --------------------------------------------
// Long-term memory and relationships
// --------------------------------------------
bool        g_MemoryEnable             = true;
uint32_t    g_MemoryHistoryTokenLimit  = 1500;
uint32_t    g_MemoryPromptTokenBudget  = 400;
uint32_t    g_MemoryMaxPerBot          = 40;
uint32_t    g_MemorySaveInterval       = 10;
std::string g_MemoryCondensePrompt;
std::string g_MemoryPromptTemplate;

bool        g_RelationshipEnable            = true;
uint32_t    g_RelationshipMentionThreshold  = 8;
uint32_t    g_RelationshipMaxPerPrompt      = 3;
uint32_t    g_RelationshipMaxLength         = 220;
std::string g_RelationshipUpdatePrompt;
std::string g_RelationshipPromptTemplate;

time_t      g_LastMemorySaveTime = time(nullptr);

// --------------------------------------------
// Random Chatter Timing
// --------------------------------------------
uint32_t    g_MinRandomInterval               = 45;
uint32_t    g_MaxRandomInterval               = 180;

// --------------------------------------------
// Conversation History Settings
// --------------------------------------------
uint32_t    g_MaxConversationHistory          = 5;
uint32_t    g_ConversationHistorySaveInterval = 10;

// --------------------------------------------
// Prompt Templates
// --------------------------------------------
std::string g_RandomChatterPromptTemplate;
std::vector<std::string> g_RandomChatterPromptVariations;
std::vector<std::string> g_RandomChatterQuestionVariations;
std::string g_EventChatterPromptTemplate;
std::string g_ChatPromptTemplate;
std::string g_ChatExtraInfoTemplate;

// --------------------------------------------
// Personality and Prompt Data
// --------------------------------------------
std::unordered_map<uint64_t, std::string> g_BotPersonalityList;
std::unordered_map<std::string, std::string> g_PersonalityPrompts;
std::vector<std::string> g_PersonalityKeys;
std::vector<std::string> g_PersonalityKeysRandomOnly;
std::string g_DefaultPersonalityPrompt;

// --------------------------------------------
// Chat History Templates and Toggles
// --------------------------------------------
bool        g_EnableChatHistory = true;
std::string g_ChatHistoryHeaderTemplate;
std::string g_ChatHistoryLineTemplate;
std::string g_ChatHistoryFooterTemplate;

// --------------------------------------------
// Chatbot Snapshot Template
// --------------------------------------------
bool        g_EnableChatBotSnapshotTemplate  = false;
std::string g_ChatBotSnapshotTemplate;

// --------------------------------------------
// Conversation History Store and Mutex
// --------------------------------------------
std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::deque<BotConversationEntry>>> g_BotConversationHistory;
std::mutex g_ConversationHistoryMutex;
// Seeded at startup, not left at 0. difftime(now, 0) is ~56 years, so the old
// value made the very first world tick fire a full save of a store that had
// only just been loaded from the same table.
time_t g_LastHistorySaveTime = time(nullptr);

// --------------------------------------------
// Bot-Player Sentiment Tracking System
// --------------------------------------------
bool        g_EnableSentimentTracking = true;
float       g_SentimentDefaultValue = 0.5f;              // Default sentiment value (0.5 = neutral)
float       g_SentimentAdjustmentStrength = 0.1f;        // How much to adjust sentiment per message
uint32_t    g_SentimentSaveInterval = 10;                // How often to save sentiment to DB (minutes)
std::string g_SentimentAnalysisPrompt = "Analyze the sentiment of this message: \"{message}\". Respond only with: POSITIVE, NEGATIVE, or NEUTRAL.";
std::string g_SentimentPromptTemplate = "Your relationship sentiment with {player_name} is {sentiment_value} (0.0=hostile, 0.5=neutral, 1.0=friendly). Use this to guide your tone and response.";

// In-memory sentiment storage and mutex
std::unordered_map<uint64_t, std::unordered_map<uint64_t, float>> g_BotPlayerSentiments;
std::set<std::pair<uint64_t, uint64_t>> g_DirtySentiments;
std::mutex g_SentimentMutex;
time_t g_LastSentimentSaveTime = time(nullptr);

// --------------------------------------------
// RAG (Retrieval-Augmented Generation) System
// --------------------------------------------
bool        g_EnableRAG = false;
std::string g_RAGDataPath = "rag/";
uint32_t    g_RAGMaxRetrievedItems = 3;
float       g_RAGSimilarityThreshold = 0.3f;
std::string g_RAGPromptTemplate;

class OllamaRAGSystem;
OllamaRAGSystem* g_RAGSystem = nullptr;

// --------------------------------------------
// Blacklist: Prefixes for Commands (not chat)
// --------------------------------------------
std::vector<std::string> g_BlacklistCommands = {
    ".playerbots",
    "playerbot",
};

// --------------------------------------------
// Environment/Contextual Random Chatter Templates
// --------------------------------------------
std::vector<std::string> g_EnvCommentCreature;
std::vector<std::string> g_EnvCommentGameObject;
std::vector<std::string> g_EnvCommentEquippedItem;
std::vector<std::string> g_EnvCommentBagItem;
std::vector<std::string> g_EnvCommentBagItemSell;
std::vector<std::string> g_EnvCommentSpell;
std::vector<std::string> g_EnvCommentQuestArea;
std::vector<std::string> g_EnvCommentVendor;
std::vector<std::string> g_EnvCommentQuestgiver;
std::vector<std::string> g_EnvCommentBagSlots;
std::vector<std::string> g_EnvCommentDungeon;
std::vector<std::string> g_EnvCommentUnfinishedQuest;

// --------------------------------------------
// Guild-Specific Random Chatter Templates
// --------------------------------------------
std::vector<std::string> g_GuildEnvCommentGuildMember;
std::vector<std::string> g_GuildEnvCommentGuildRank;
std::vector<std::string> g_GuildEnvCommentGuildBank;
std::vector<std::string> g_GuildEnvCommentGuildMOTD;
std::vector<std::string> g_GuildEnvCommentGuildInfo;
std::vector<std::string> g_GuildEnvCommentGuildOnlineMembers;
std::vector<std::string> g_GuildEnvCommentGuildRaid;
std::vector<std::string> g_GuildEnvCommentGuildEndgame;
std::vector<std::string> g_GuildEnvCommentGuildStrategy;
std::vector<std::string> g_GuildEnvCommentGuildGroup;
std::vector<std::string> g_GuildEnvCommentGuildPvP;
std::vector<std::string> g_GuildEnvCommentGuildCommunity;

// --------------------------------------------
// Guild-Specific Random Chatter Configuration
// --------------------------------------------
bool        g_EnableGuildEventChatter             = true;
bool        g_EnableGuildRandomAmbientChatter      = true;
uint32_t    g_GuildRandomChatterChance             = 10;
uint32_t    g_GuildChatterBotCommentChance          = 25;
uint32_t    g_GuildChatterMaxBotsPerEvent           = 2;

// --------------------------------------------
// Guild-Specific Event Chatter Templates
// --------------------------------------------
std::string g_GuildEventTypeLevelUp = "";
std::string g_GuildEventTypeDungeonComplete = "";
std::string g_GuildEventTypeEpicGear = "";
std::string g_GuildEventTypeRareGear = "";
std::string g_GuildEventTypeGuildJoin = "";
std::string g_GuildEventTypeGuildLeave = "";
std::string g_GuildEventTypeGuildPromotion = "";
std::string g_GuildEventTypeGuildDemotion = "";
std::string g_GuildEventTypeGuildLogin = "";
std::string g_GuildEventTypeGuildAchievement = "";

// --------------------------------------------
// Event Chatter Templates
// --------------------------------------------
std::string g_EventTypeDefeated;           // "defeated"
std::string g_EventTypeDefeatedPlayer;     // "defeated player"
std::string g_EventTypePetDefeated;        // "pet defeated"
std::string g_EventTypeGotItem;            // "got item"
std::string g_EventTypeDied;               // "died"
std::string g_EventTypeCompletedQuest;     // "completed quest"
std::string g_EventTypeLearnedSpell;       // "learned spell"
std::string g_EventTypeRequestedDuel;      // "requested to duel"
std::string g_EventTypeStartedDueling;     // "started dueling"
std::string g_EventTypeWonDuel;            // "won duel against"
std::string g_EventTypeLeveledUp;          // "leveled up"
std::string g_EventTypeAchievement;        // "earned achievement"
std::string g_EventTypeUsedObject;         // "used object"

// Chance variables for normal events
int g_EventTypeDefeated_Chance = 0;
int g_EventTypeDefeatedPlayer_Chance = 0;
int g_EventTypePetDefeated_Chance = 0;
int g_EventTypeGotItem_Chance = 0;
int g_EventTypeDied_Chance = 0;
int g_EventTypeCompletedQuest_Chance = 0;
int g_EventTypeLearnedSpell_Chance = 0;
int g_EventTypeRequestedDuel_Chance = 0;
int g_EventTypeStartedDueling_Chance = 0;
int g_EventTypeWonDuel_Chance = 0;
int g_EventTypeLeveledUp_Chance = 0;
int g_EventTypeAchievement_Chance = 0;
int g_EventTypeUsedObject_Chance = 0;

// Chance variables for guild events
int g_GuildEventTypeEpicGear_Chance = 0;
int g_GuildEventTypeRareGear_Chance = 0;
int g_GuildEventTypeGuildJoin_Chance = 0;
int g_GuildEventTypeGuildLogin_Chance = 0;
int g_GuildEventTypeGuildLeave_Chance = 0;
int g_GuildEventTypeGuildPromotion_Chance = 0;
int g_GuildEventTypeGuildDemotion_Chance = 0;
int g_GuildEventTypeGuildAchievement_Chance = 0;
int g_GuildEventTypeLevelUp_Chance = 0;
int g_GuildEventTypeDungeonComplete_Chance = 0;

// Event Cooldown
uint32_t g_EventCooldownTime = 10;

// --------------------------------------------
// Channel Disable Settings
// --------------------------------------------
bool g_DisableForCustomChannels = false;
bool g_DisableForSayYell = false;
bool g_DisableForGuild = false;
bool g_DisableForParty = false;

bool g_ChatterUseGeneralChannel           = true;
bool g_ChatterUseTradeChannel             = true;
bool g_ChatterUseLfgChannel               = false;
bool g_ChatterUseGuildRecruitmentChannel  = false;

// --------------------------------------------
// Typing Simulation Settings
// --------------------------------------------
bool g_EnableTypingSimulation = false;
uint32_t g_TypingSimulationBaseDelay = 1000;     // 1000ms base delay
uint32_t g_TypingSimulationDelayPerChar = 250;
uint32_t g_TypingSimulationMaxDelay     = 8000;   // 250ms per character (4 chars/sec)

// Load Bot Personalities from Database
void LoadBotPersonalityList()
{    
    // Let's make sure our user has sourced the required sql file to add the new table
    QueryResult tableExists = CharacterDatabase.Query("SELECT * FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_ollama_chat_personality' LIMIT 1");
    if (!tableExists)
    {
        LOG_ERROR("module.ollamachat", "[Ollama Chat] Please source the required database table first");
        return;
    }

    QueryResult result = CharacterDatabase.Query("SELECT guid,personality FROM mod_ollama_chat_personality");

    if (!result)
    {
        return;
    }
    if (result->GetRowCount() == 0)
    {
        return;
    }    

    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Fetching Bot Personality List into array");
    }

    do
    {
        uint64_t personalityBotGUID = result->Fetch()[0].Get<uint64_t>();
        std::string personalityKey = result->Fetch()[1].Get<std::string>();
        g_BotPersonalityList[personalityBotGUID] = personalityKey;
    } while (result->NextRow());
}

std::string GetMultiLineConfigValue(const std::string& configFilePath, const std::string& key)
{
    std::ifstream infile(configFilePath);
    if (!infile) return "";

    std::string line;
    std::string value;
    bool foundKey = false;
    while (std::getline(infile, line))
    {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        size_t pos = trimmed.find('=');
        if (!foundKey && pos != std::string::npos) {
            std::string possibleKey = trimmed.substr(0, pos);
            possibleKey.erase(possibleKey.find_last_not_of(" \t\r\n") + 1);
            if (possibleKey == key) {
                foundKey = true;
                std::string afterEq = trimmed.substr(pos + 1);
                afterEq.erase(0, afterEq.find_first_not_of(" \t\r\n"));
                value += afterEq;
                continue;
            }
        }
        else if (foundKey) {
            // New config key or section
            if (trimmed.find('=') != std::string::npos && trimmed.find('[') == std::string::npos)
                break;
            if (!value.empty()) value += "\n";
            value += trimmed;
        }
    }

    return value;
}

// --------------------------------------------------------------------------
// Natural chat v4: greetings, level-up congratulations, cities, presence,
// conversations and party instructions. Defaults live here as well as in the
// .conf, so a conf written before these keys existed still gets them.
// --------------------------------------------------------------------------

namespace
{
    std::vector<std::string> ReadPipeList(const char* key, const char* def)
    {
        std::vector<std::string> out;
        for (std::string item : SplitString(sConfigMgr->GetOption<std::string>(key, def), '|'))
        {
            const size_t b = item.find_first_not_of(" \t\r\n");
            const size_t e = item.find_last_not_of(" \t\r\n");
            if (b != std::string::npos)
                out.push_back(item.substr(b, e - b + 1));
        }
        return out;
    }

    // The same, for lines that may hold "{~a|b}": a bar inside braces is part
    // of its line.
    std::vector<std::string> ReadChoiceList(const char* key, const char* def)
    {
        return TextRules::SplitBars(sConfigMgr->GetOption<std::string>(key, def));
    }

    std::vector<uint32_t> ReadEmoteList(const std::string& raw, std::string* unknown = nullptr)
    {
        std::vector<uint32_t> out;
        for (const std::string& name : SplitString(raw, '|'))
        {
            const size_t b = name.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)
                continue;
            const size_t e = name.find_last_not_of(" \t\r\n");
            const std::string clean = name.substr(b, e - b + 1);
            if (const uint32_t id = LookupTextEmoteId(clean))
                out.push_back(id);
            else if (unknown)
                *unknown += (unknown->empty() ? "" : ", ") + clean;
        }
        return out;
    }
}

static void LoadNaturalChatV4Settings()
{
    // --- greetings --------------------------------------------------------
    g_CrowdGreetingWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Crowd.GreetingWords",
        "hi|hello|hey|heya|hiya|hai|helo|hallo|hullo|ello|yo|sup|wassup|wazzup|whatsup|howdy|"
        "greetings|o|morning|mornin|evening|afternoon|gm|hii*|heyy*|yoo*|helloo*|there|again"));
    g_CrowdFullCrowdInCities = sConfigMgr->GetOption<bool>("OllamaChat.Crowd.FullCrowdInCities", true);

    // --- level-ups --------------------------------------------------------
    g_EventChatterSelfPromptTemplate = sConfigMgr->GetOption<std::string>(
        "OllamaChat.EventChatterSelfPromptTemplate",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}. Your chat style: {bot_personality} "
        "This just happened to you: you {event_type} {event_detail}. Type the quick line a real player would, "
        "usually 1 to 4 words: a level-up gets ding, often with the new level, or a happy word; an achievement "
        "gets a happy word; a death gets rip, ugh or a groan; a duel win gets gg. No exclamation-mark hype, and "
        "do not describe what happened.");
    g_EventCongratsLines = ReadPipeList("OllamaChat.Events.CongratsLines",
        "gz|gz|grats|gratz|congrats|congratz|gz {name}|grats {name}|congrats {name}|gratz {name}|gz!|grats!|gzz");
    g_EventCongratsCannedChance = std::min<uint32_t>(100,
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Events.CongratsCannedChance", 50));

    // --- joining a group ---------------------------------------------------
    g_JoinedEnable               = sConfigMgr->GetOption<bool>("OllamaChat.Joined.Enable", true);
    g_JoinedGreetChance          = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Joined.GreetChance", 70));
    g_JoinedGroupCooldownSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Joined.GroupCooldownSeconds", 60);
    g_JoinedContinueMinutes      = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Joined.ContinueMinutes", 10));
    g_JoinedCannedChance         = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Joined.CannedChance", 50));
    g_JoinedGreetLines = ReadPipeList("OllamaChat.Joined.GreetLines",
        "hey|hey, whats up?|hey whats up|hey hey|hi|hiya|heya|yo|sup|hey all|hi all|o/|whats up|hey, whats the plan?|"
        "hey whats the plan");
    g_JoinedGreetPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Joined.GreetPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} playing World of Warcraft. Your chat style: "
        "{bot_personality} {player_name}, a level {player_level} {player_class}, has just invited you to their group "
        "out of the blue, and you joined. Type the one short line you would say in party chat on joining, the way a "
        "real player does: 'hey, whats up?' or something like it, a few words, in your own style. Do not thank them "
        "for the invite, and do not guess what they want.");
    g_JoinedContinuePrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Joined.ContinuePrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} playing World of Warcraft. Your chat style: "
        "{bot_personality} A moment ago, {where}, {player_name} said to you: '{player_line}' and you answered: "
        "'{bot_line}'. Now they have invited you to their group, and you joined. Type the one short line you would "
        "say in party chat now, carrying on from that, the way a real player would: a few words, in your own style. "
        "You have been talking already, so no hello, and do not thank them for the invite.");
    g_JoinedDelayMinMs = sConfigMgr->GetOption<uint32_t>("OllamaChat.Joined.DelayMinMs", 2000);
    g_JoinedDelayMaxMs = sConfigMgr->GetOption<uint32_t>("OllamaChat.Joined.DelayMaxMs", 5000);

    // --- "gz", and the one "ty" -------------------------------------------
    g_CongratsEnable        = sConfigMgr->GetOption<bool>("OllamaChat.Congrats.Enable", true);
    g_CongratsWindowSeconds = std::max<uint32_t>(30, sConfigMgr->GetOption<uint32_t>("OllamaChat.Congrats.WindowSeconds", 300));
    g_CongratsWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Congrats.Words",
        "gz*|grats*|gratz*|grz|grtz|congrats*|congratz*|congratulations|congrat|gratulations"));
    g_CongratsFillerWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Congrats.FillerWords",
        "on|the|a|an|ur|your|ya|that|it|this|again|finally|ding|dinged|lvl|lvls|level|levels|levelled|leveled|"
        "achievement|achievements|achiev*|cheevo*|nice|one|well|done|big|huge|very|so|man|mate|m8|bro|dude|buddy|"
        "pal|lad|lads|guys|all|everyone|yall|haha*|lol*|lmao*|xd*|wp|gg|gj|yay|woo*|ayy*|for|to|you|u|and|both|"
        "new|omg|wow|nicely|sweet|epic|at|last|another|drop|loot|item|mount|pet|title|rep|exalted|kill|first|"
        "upgrade|gear|purple|max|cap|80"));
    g_CongratsThanksLines = ReadPipeList("OllamaChat.Congrats.ThanksLines",
        "ty|ty|ty|thanks|thx|ty {name}|thanks {name}|cheers|cheers {name}|ty ty|tyty");
    g_CongratsThanksAllLines = ReadPipeList("OllamaChat.Congrats.ThanksAllLines",
        "ty all|ty guys|thanks all|thanks guys|cheers all|ty ty|tyty|thx all|cheers guys");
    g_CongratsThanksChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Congrats.ThanksChance", 90));
    g_CongratsThanksDelayMinMs = sConfigMgr->GetOption<uint32_t>("OllamaChat.Congrats.ThanksDelayMinMs", 2500);
    g_CongratsThanksDelayMaxMs = sConfigMgr->GetOption<uint32_t>("OllamaChat.Congrats.ThanksDelayMaxMs", 6000);
    g_CongratsAchievementsRareOnly =
        sConfigMgr->GetOption<bool>("OllamaChat.Congrats.AchievementsRareOnly", true);
    g_CongratsAchievementRarePoints =
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Congrats.AchievementRarePoints", 25);
    {
        // Achievement ids, separated by | or commas.
        auto ids = [](const char* key)
        {
            std::unordered_set<uint32_t> out;
            std::string token;
            for (char ch : sConfigMgr->GetOption<std::string>(key, "") + "|")
            {
                if (ch >= '0' && ch <= '9')
                    token += ch;
                else if (!token.empty())
                {
                    out.insert(uint32_t(std::stoul(token)));
                    token.clear();
                }
            }
            return out;
        };
        g_CongratsAchievementAlways = ids("OllamaChat.Congrats.AchievementAlways");
        g_CongratsAchievementNever  = ids("OllamaChat.Congrats.AchievementNever");
    }

    // --- assistant speak --------------------------------------------------
    g_ResponseAssistantPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Response.AssistantPhrases",
        "how can i help|how may i help|how can i assist|how may i assist|how can i be of|what can i do for you|"
        "what can i help you with|anything i can help you with|anything i can do for you|"
        "need help with something|need help with anything|let me know if you need anything|happy to help|"
        "glad to help|here to help|at your service|is there anything else|what brings you"));

    // --- presence ---------------------------------------------------------
    g_PresenceEnable              = sConfigMgr->GetOption<bool>("OllamaChat.Presence.Enable", true);
    g_PresenceRange               = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Presence.Range", 10.0f), 3.0f, 40.0f);
    g_PresencePlayerIntervalMin   = sConfigMgr->GetOption<uint32_t>("OllamaChat.Presence.PlayerIntervalMinSeconds", 240);
    g_PresencePlayerIntervalMax   = sConfigMgr->GetOption<uint32_t>("OllamaChat.Presence.PlayerIntervalMaxSeconds", 720);
    g_PresenceBotCooldownSeconds  = sConfigMgr->GetOption<uint32_t>("OllamaChat.Presence.BotCooldownSeconds", 900);
    g_PresencePairCooldownSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Presence.PairCooldownSeconds", 3600);
    g_PresenceChance              = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Presence.Chance", 60));
    g_PresenceSayChance           = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Presence.SayChance", 35));
    g_PresenceCityOnly            = sConfigMgr->GetOption<bool>("OllamaChat.Presence.CityOnly", false);
    g_PresencePromptTemplate      = sConfigMgr->GetOption<std::string>("OllamaChat.Presence.PromptTemplate",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}. Your chat style: {bot_personality} "
        "You are walking past {player_name}: {observation}. Say one short thing to them about something in that, "
        "the way a passing player might in /say: a compliment, a joke, a tease or a quick question, 2 to 10 words, "
        "and never about anything it does not mention. Do not start with their name, and do not describe what you "
        "are doing.");

    // --- cities -----------------------------------------------------------
    g_CityEnable                  = sConfigMgr->GetOption<bool>("OllamaChat.City.Enable", true);
    g_CityIncludeInns             = sConfigMgr->GetOption<bool>("OllamaChat.City.IncludeInns", true);
    g_CityChatterMultiplier       = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.City.ChatterMultiplier", 3), 1, 20);
    g_CityConversationStartChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.City.ConversationStartChance", 35));
    g_CityBotLineReplyBonus       = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.City.BotLineReplyBonus", 20));
    g_CityConversationBoost       = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.City.ConversationBoost", 2), 1, 10);
    g_CityExtraIds.clear();
    for (const std::string& id : ReadPipeList("OllamaChat.City.ExtraZoneOrAreaIds", "3703|4395"))
    {
        char* end = nullptr;
        const unsigned long v = std::strtoul(id.c_str(), &end, 10);
        if (end && *end == '\0' && v > 0)
            g_CityExtraIds.insert(static_cast<uint32_t>(v));
    }

    // --- conversations ----------------------------------------------------
    g_ConversationEnable            = sConfigMgr->GetOption<bool>("OllamaChat.Conversation.Enable", true);
    g_ConversationMinMinutes        = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.MinMinutes", 2));
    g_ConversationMaxMinutes        = std::max(g_ConversationMinMinutes, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.MaxMinutes", 15));
    g_ConversationGapMinSeconds     = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.GapMinSeconds", 6));
    g_ConversationGapMaxSeconds     = std::max(g_ConversationGapMinSeconds, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.GapMaxSeconds", 25));
    g_ConversationLullChance        = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.LullChance", 12));
    g_ConversationLullMinSeconds    = sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.LullMinSeconds", 30);
    g_ConversationLullMaxSeconds    = std::max(g_ConversationLullMinSeconds, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.LullMaxSeconds", 75));
    g_ConversationMaxActive         = std::min<uint32_t>(20, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.MaxActive", 3));
    g_ConversationMaxParticipants   = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.MaxParticipants", 5), 2, 10);
    g_ConversationJoinChance        = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.JoinChance", 15));
    g_ConversationFromPlayerChance  = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.FromPlayerChance", 40));
    g_ConversationFromBotLineChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.FromBotLineChance", 15));
    g_ConversationCooldownSeconds   = sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.CooldownSeconds", 120);
    g_ConversationMaxMisses         = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.MaxMisses", 2), 1, 10);
    g_ConversationObserveChance     = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.ObserveChance", 30));
    g_ConversationAllowTrade        = sConfigMgr->GetOption<bool>("OllamaChat.Conversation.AllowTrade", true);
    g_ConversationAllowSay          = sConfigMgr->GetOption<bool>("OllamaChat.Conversation.AllowSay", true);
    // A "[70+]" or "[10-39]" in front: only bots of those levels bring it up;
    // a "[horde]" or "[alliance]", only that side's.
    g_ConversationPremises = ReadChoiceList("OllamaChat.Conversation.Premises",
        "which class is the most overpowered right now|whether death knights ruined everything now that "
        "every other player is one|[70+] the best tank class for heroics|whether ret paladins need a "
        "nerf|[15+] the worst wipe you ever had in a dungeon|[15+] a ninja looter who rolled need on "
        "everything in a pug|[75+] Wintergrasp and who keeps winning it|the best profession for making "
        "gold|[60+] whether epic flying is worth the gold|the funniest hunter pet name you have seen|whether "
        "Arthas was always evil|whether Illidan was right all along|[80] the hardest boss in Naxxramas|[80] "
        "getting into Ulduar runs without much gear|whether achievements are worth chasing|[horde] where "
        "Mankrik's wife could be|[68+] the best place to level from 70 to 80|how expensive the auction house has "
        "got|guild drama|[75+] whether the Lich King fight will be any good|[15+] people who stand in "
        "fire|what you do on long flight paths|a name for a new alt|fishing: relaxing or the most boring "
        "thing in the game|[77+] the Argent Tournament dailies|whether gnomes or goblins make better "
        "engineers|[10+] whether {other_faction} is winning more battlegrounds lately|[80] a heroic you want "
        "a group for|[1-19] which starting zone is the best|[15-39] the first dungeon you ever ran|[20-59] "
        "the most annoying levelling zone|[58-69] how crowded Hellfire Peninsula gets");
    g_ConversationHintTurn = sConfigMgr->GetOption<std::string>("OllamaChat.Conversation.HintTurn",
        "You are chatting in {where} with {others} (see the chat above){about}. Carry it on as yourself with "
        "one short line: answer the last line, disagree, joke, ask something back, or take it somewhere "
        "related. Never make a point someone has already made, not even in other words: if you agree, add "
        "something new to it (a reason, a story, a joke) instead of saying it again. If you really have "
        "nothing to add, reply with just {pass_token}.");
    g_ConversationHintJoin = sConfigMgr->GetOption<std::string>("OllamaChat.Conversation.HintJoin",
        "You have been reading the chat above in {where}{about}, and now you jump in with your own take, a "
        "joke or a question. One short line.");
    g_ConversationHintWindDown = sConfigMgr->GetOption<std::string>("OllamaChat.Conversation.HintWindDown",
        "This chat has been going on for a while and is winding down. Give your last word on it the way players "
        "do: a final opinion, a joke, or something like anyway, gtg, back to farming or cya. Do not start "
        "anything new.");
    g_ConversationHintStartChannel = sConfigMgr->GetOption<std::string>("OllamaChat.Conversation.HintStartChannel",
        "You feel like starting a chat in {where} about {premise}. Bring it up the way a player would: an "
        "opinion, a question to everyone or a complaint.");
    g_ConversationHintStartSay = sConfigMgr->GetOption<std::string>("OllamaChat.Conversation.HintStartSay",
        "{partner} is standing right next to you. Start a chat with them about {premise}, the way a player "
        "would: an opinion or a question. You can use their name.");
    g_ConversationHintObserve = sConfigMgr->GetOption<std::string>("OllamaChat.Conversation.HintObserve",
        "Right now {observation}. You can mention that if it fits.");

    // --- party instructions -----------------------------------------------
    g_PartyCmdEnable      = sConfigMgr->GetOption<bool>("OllamaChat.PartyCommands.Enable", true);
    g_PartyCmdMaxDistance = sConfigMgr->GetOption<float>("OllamaChat.PartyCommands.MaxDistance", 100.0f);
    g_PartyCmdRules.start = TextRules::ParseCommandPhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.StartPhrases",
        "=r|=begin|=start|=continue|=resume|=back|ready|rdy|gogo*|lets go|"
        "lets start|lets begin|lets do this|lets do it|lets roll|lets move|lets continue|"
        "lets carry on|carry on|im back|i am back|back now|good to go|start the run|start the dungeon|"
        "start dungeon|start the instance|start the clear|just start|=start it|lets get started|=get started|"
        "=start now|start already|=start pls|=start please|begin the run|letsgo|lesgo|lezgo|letzgo|lego|leggo|"
        "lessgo|lets get it|letsgetit|lets get this|lets go again|go again"));
    // A bare "go" is playerbots' own command while it reads plain chat; with
    // Playerbots.CommandPrefix set it no longer does, and "go" is just what
    // people type to start.
    if (!sPlayerbotAIConfig.CommandPrefix.empty())
    {
        const std::vector<TextRules::CommandPhrase> go = TextRules::ParseCommandPhrases(sConfigMgr->GetOption<std::string>(
            "OllamaChat.PartyCommands.GoPhrases",
            "=go|=go go|=go go go|=ok go|=k go|=kk go|=go now|=go pls|=go please|=go ahead|=just go|=go for it|"
            "=you can go|=go then|=ok go then"));
        g_PartyCmdRules.start.insert(g_PartyCmdRules.start.end(), go.begin(), go.end());
    }
    g_PartyRunQuestionPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.RunQuestionPhrases",
        "whats next|what next|what now|whats now|whos next|who is next|which boss|what boss|next boss|"
        "wheres the boss|where is the boss|whats the plan|what is the plan|whats our plan|what are we doing|"
        "what we doing|what r we doing|what are we waiting|what we waiting|why are we waiting|why we waiting|"
        "why are we stopped|why did we stop|why we stopped|why stop|why stopping|why are we not moving|"
        "why arent we|whats going on|what is going on|whats happening|what happened|where are we going|"
        "where we going|where to|where now|where next|which way|how many bosses|how many left|bosses left|"
        "whats left|what is left|anything left|are we done|we done|done yet|is that it|is that all|last boss|"
        "how far|status|progress|are you stuck|you stuck|why are you stuck|why stuck|whats wrong|what is wrong|"
        "whos leading|who is leading|whos tanking|who is tanking|are we pulling|next pull|next pack"));
    g_PartyRunQuestionMaxWords = sConfigMgr->GetOption<uint32_t>("OllamaChat.PartyCommands.RunQuestionMaxWords", 14);
    g_PartyCmdRules.pause = TextRules::ParseCommandPhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.PausePhrases",
        "mb|mana break|manabreak|need mana|need some mana|need more mana|need some more mana|need a bit of mana|"
        "need a bit more mana|need a little mana|need a little more mana|need to get mana|get some mana|get my mana|"
        "get mana back|let me get mana|lemme get mana|mana low|mana is low|manas low|mana getting low|"
        "mana is getting low|manas getting low|need to regen|need regen|let me regen|lemme regen|gotta regen|"
        "regen break|need to drink|need a drink|need drink|need some drink|need a quick drink|quick drink|"
        "drink break|drink first|let me drink|lemme drink|gotta drink|got to drink|have to drink|gonna drink|"
        "going to drink|drinking|=drink|oom|no mana|low mana|low on mana|out of mana|outta mana|need to eat|"
        "let me eat|lemme eat|wait|w8|"
        "=w|hold on|hold up|holdup|hang on|stop|pause|=sec|one sec|1 sec|a sec|one min|1 min|a min|brb|=afk|"
        "need a break|need a sec|gimme a sec|give me a sec|bio break|=bio|lets rest|need to rest|=rest|"
        "dont pull|stop pulling|stay here|stay there|stay back|stay put|hold position|hold here|dont move"));
    g_PartyCmdRules.resume = TextRules::ParseCommandPhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.ResumePhrases",
        "done|im done|all done|done now|finished|im finished|done drinking|finished drinking|done eating|"
        "finished eating|full|im full|full mana|mana full|full now|topped up|topped off|good to go|ready now|"
        "lets get going|ok lets go"));
    g_PartyCmdRules.resumeFillers = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.ResumeFillers",
        "ok|okay|k|kk|alright|aight|right|so|im|i am|all|now|then|lol|ty|thanks|thx|cheers|thank you|guys|lads|"
        "mate|yep|yeah|yes|ye|sorry|sry|and|too|finally|phew|np|good|great|cool|nice"));
    g_PartyCmdRules.resumeMaxWords = sConfigMgr->GetOption<uint32_t>("OllamaChat.PartyCommands.ResumeMaxWords", 6);
    g_PartyCmdRules.resumeMaxStray = sConfigMgr->GetOption<uint32_t>("OllamaChat.PartyCommands.ResumeMaxStray", 1);
    g_PartyCmdRules.cancel = TextRules::ParseCommandPhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.CancelPhrases",
        "dont stop|do not stop|dont wait|do not wait|no need to wait|no need to stop|dont need to wait|"
        "keep going|no break|dont need a break|no need"));
    g_PartyCmdRules.negations = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.NegationWords", "not|dont|cant|wont|no|nope|nah|never|aint|isnt|arent|havent"));
    g_PartyCmdRules.questions = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.QuestionWords", "what|where|why|how|who|which|whats|wheres|whos|hows"));
    g_PartyCmdRules.openers = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.QuestionOpeners",
        "are|is|am|do|does|did|you|u|ya|anyone|anybody|someone|somebody|any1|ne1|has|have|when|r u"));
    g_PartyCmdRules.notYet = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.PartyCommands.NotYetWords",
        "almost|nearly|soon|later|after|until|till|til|in a|in one|in two|in few|in 1*|in 2*|in 3*|in 4*|"
        "in 5*|in 6*|in 7*|in 8*|in 9*"));
    g_PartyCmdRules.maxWords = sConfigMgr->GetOption<uint32_t>("OllamaChat.PartyCommands.MaxWords", 10);
    g_PartyCmdHintStart = sConfigMgr->GetOption<std::string>("OllamaChat.PartyCommands.HintStart",
        "{player_name} is ready, so you lead the group on through the dungeon. Tell them in two to six words, "
        "in your own chat style: someone serious just says they are moving, a joker shouts something silly, "
        "and anything in between is fine. Only talk about a fight if one is actually in front of you.{recent_acks}");
    g_PartyCmdHintPause = sConfigMgr->GetOption<std::string>("OllamaChat.PartyCommands.HintPause",
        "{player_name} asked the group to stop for {reason}, and you have stopped the run. Tell them in a few "
        "words, in your own chat style, that you will wait and they should say when they are ready.{recent_acks}");
    g_PartyCmdHintResume = sConfigMgr->GetOption<std::string>("OllamaChat.PartyCommands.HintResume",
        "{player_name} is ready again, so the run carries on and you lead the way. Say so in a few words, in "
        "your own chat style.{recent_acks}");
    g_PartyCmdHintWaiting = sConfigMgr->GetOption<std::string>("OllamaChat.PartyCommands.HintWaiting",
        "{player_name} wants to start, but not everyone is here yet: still waiting for {missing}. Tell them in "
        "a few words that you will start as soon as everyone is here.{recent_acks}");
    g_PartyCmdRecentAcks = std::min<uint32_t>(10, sConfigMgr->GetOption<uint32_t>("OllamaChat.PartyCommands.RecentAcks", 4));
    g_PartyCmdRecentAcksText = sConfigMgr->GetOption<std::string>("OllamaChat.PartyCommands.RecentAcksText",
        " You already said {lines} in this group, so use different words this time.");
    g_PartyRunStateText = sConfigMgr->GetOption<std::string>("OllamaChat.Party.RunStateText",
        "About this dungeon run: {tank} leads it, {progress}, the next boss is {next}, and right now the group "
        "is {state}. Only bring this up when someone asks what is going on, what the plan is or which boss is "
        "next.");
    g_PartyDungeonGlossary = sConfigMgr->GetOption<std::string>("OllamaChat.Party.DungeonGlossary",
        "In group chat here: mb means mana break (they need to drink, so wait for them), oom means out of mana, "
        "r or rdy means ready, brb means be right back, inc means enemies incoming, cc means crowd control, and "
        "pull means start the fight.");
    g_PartyDungeonAmbientPercent = std::min<uint32_t>(100,
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.DungeonAmbientPercent", 35));
    g_PartyDungeonFocusText = sConfigMgr->GetOption<std::string>("OllamaChat.Party.DungeonFocusText",
        "You are in the middle of a dungeon run with your group. If you say anything, keep it to the run: the "
        "pulls, the bosses, the loot, someone low on health or mana, the place itself. No gossip, rumours or "
        "anything that has nothing to do with it.");

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Conversations: {} ({}-{} min, up to {} at once, {} premises). Cities: {} (chatter x{}). "
             "Presence: {}. Party instructions: {}.",
             (g_ConversationEnable && g_CrowdEnable && g_ContextEnable) ? "on" : "off",
             g_ConversationMinMinutes, g_ConversationMaxMinutes, g_ConversationMaxActive,
             g_ConversationPremises.size(), g_CityEnable ? "on" : "off", g_CityChatterMultiplier,
             g_PresenceEnable ? "on" : "off", g_PartyCmdEnable ? "on" : "off");
}

// What a player of each level is up to, in broad strokes. Kept short and
// general on purpose: whatever is most concrete in a prompt is what the model
// talks about, and this is here to keep a bot to its own stage of the game,
// not to hand every level 15 the same three dungeon names.
static const char* const kDefaultLevelBands =
    "1-9: Your character is only level {level}, brand new and still in its starting area, so you talk like "
    "someone just starting out.|"
    "10-19: Your character is level {level}: early days, questing in the first big zones and trying the "
    "first dungeons.|"
    "20-39: Your character is level {level}, partway through levelling in the old world.|"
    "40-57: Your character is level {level}, well into the old world, with Outland still ahead.|"
    "58-69: Your character is level {level}, levelling in Outland, with Northrend still ahead.|"
    "70-79: Your character is level {level}, levelling in Northrend.|"
    "80: Your character is level 80, the level cap: heroics, raids, gear and gold fill your days.";

static void LoadNaturalChatV5Settings()
{
    // --- level and class ----------------------------------------------------
    g_OutlookEnable        = sConfigMgr->GetOption<bool>("OllamaChat.Outlook.Enable", true);
    g_OutlookLevelBands    = sConfigMgr->GetOption<std::string>("OllamaChat.Outlook.LevelBands", kDefaultLevelBands);
    g_OutlookClassRoles    = sConfigMgr->GetOption<bool>("OllamaChat.Outlook.ClassRoles", true);
    g_OutlookKnowledgeText = sConfigMgr->GetOption<std::string>("OllamaChat.Outlook.KnowledgeText",
        "You know other players only by what anyone could see of them: their name, level, race, class, spec "
        "and guild, and what they say in chat.");
    g_OutlookLevelPhrases = TextRules::ParseLevelPhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Outlook.LevelPhrases",
        "[40+] repair bill|[40+] repair bills|[40+] repair costs|[70+] heroic|[70+] heroics|[70+] naxx|"
        "[70+] naxxramas|[75+] ulduar|[75+] icc|[75+] icecrown citadel|[75+] wintergrasp|[60+] gear score|"
        "[60+] gearscore|[80] emblems|[80] emblem of|[60+] epic flying|[55+] outland|[55+] hellfire|"
        "[55+] hellfire peninsula|[55+] zangarmarsh|[55+] terokkar|[55+] nagrand|[55+] blades edge|"
        "[55+] netherstorm|[55+] shadowmoon valley|[55+] shattrath|[55+] shatt|[65+] northrend|"
        "[65+] borean tundra|[65+] howling fjord|[65+] dragonblight|[65+] grizzly hills|[65+] zuldrak|"
        "[65+] zul drak|[65+] sholazar|[65+] sholazar basin|[65+] storm peaks|[65+] icecrown|[65+] crystalsong"));
    Outlook_Load();

    // --- links --------------------------------------------------------------
    g_LinksEnable          = sConfigMgr->GetOption<bool>("OllamaChat.Links.Enable", true);
    g_LinksMaxPerLine      = std::min<uint32_t>(5, sConfigMgr->GetOption<uint32_t>("OllamaChat.Links.MaxPerLine", 2));
    g_LinksRememberMinutes = sConfigMgr->GetOption<uint32_t>("OllamaChat.Links.RememberMinutes", 60);
    g_LinksUseGameData     = sConfigMgr->GetOption<bool>("OllamaChat.Links.UseGameData", true);
    g_LinksPromptHint      = sConfigMgr->GetOption<std::string>("OllamaChat.Links.PromptHint",
        "Put an item's or quest's exact name in square brackets -- which makes it a link -- only when you are "
        "asking about it, offering it, asking for it or showing it off. Just mentioning it, answering a "
        "question about it or joking about it, write the name plainly, without brackets.");

    // --- stock phrases ------------------------------------------------------
    g_ResponseOverusedPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Response.OverusedPhrases",
        "taking forever|takes forever|take forever|took forever|so slow|too slow|such a grind|what a grind|"
        "so boring|so bored|im bored|getting bored|repair bill|repair bills|nothing to do"));
    g_ResponseOverusedCooldownSeconds =
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Response.OverusedCooldownSeconds", 1200);
    g_ResponseOffEraPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Response.OffEraPhrases",
        "mythic|keystone|transmog*|xmog|world quest|world quests|dragonriding|dragonflying|skyriding|"
        "timewalking|torghast|azerite|shadowlands|dragonflight|pandaria|warlords of draenor|battle for azeroth|"
        "war within|my garrison|class hall|artifact weapon|covenant ability"));

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Level outlook: {} ({} bands, {} level phrases, class roles {}). Links: {} (up to {} "
             "a line). Stock phrases: {} on a {} s cooldown, {} off-era.",
             g_OutlookEnable ? "on" : "off", Outlook_BandCount(), g_OutlookLevelPhrases.size(),
             g_OutlookClassRoles ? "on" : "off",
             g_LinksEnable ? "on" : "off", g_LinksMaxPerLine, g_ResponseOverusedPhrases.size(),
             g_ResponseOverusedCooldownSeconds, g_ResponseOffEraPhrases.size());
}

static void LoadNaturalChatV6Settings()
{
    // --- conjured food and water ---------------------------------------------
    auto phrases = [](const char* key, const char* def)
    {
        return CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(key, def));
    };

    g_ConjureEnable = sConfigMgr->GetOption<bool>("OllamaChat.Conjure.Enable", true);
    g_ConjureRules.drink = phrases("OllamaChat.Conjure.DrinkWords",
        "water|waters|watter|wata|watah|watr|wotah|h2o|drinks|refreshment*|strudel*|biscuit*|mana pie*|"
        "mana bun*|mana cake*|mana pot*|mana potion*|mana gem*");
    g_ConjureRules.food = phrases("OllamaChat.Conjure.FoodWords",
        "food|foods|fud|bread|breads|muffin*|croissant*|cinnamon roll*|sweet roll*|grub|snacks");
    g_ConjureRules.pots = phrases("OllamaChat.Conjure.PotWords", "pot|pots|potion*");
    g_ConjureRules.loose = phrases("OllamaChat.Conjure.LooseWords", "mana|drink");
    g_ConjureRules.notMage = phrases("OllamaChat.Conjure.NotMageWords",
        "health pot*|healing pot*|hp pot*|health potion*|healing potion*|healthstone*|health stone*|"
        "potion of*|speed pot*|haste pot*|rage pot*|indestructible pot*|wild magic pot*|food buff*|buff food|"
        "feast*|water walk*|waterwalk*|water breath*|water elemental*|water shield*");
    g_ConjureRules.asks = phrases("OllamaChat.Conjure.AskWords",
        "give|giv|gib|gief|gimme|gimmie|gimmi|send|trade|hand|spare|share|conjure|conj|toss|throw|drop me|"
        "pass me|hook me up|hook us up|make me|make us|make some|make more|can i have|can i get|can i grab|"
        "could i have|could i get|could i grab|may i have|may i get|do you have|do u have|you have|u have|"
        "ya have|you got|u got|ya got|got any|got some|have any|any spare|anyone have|anyone got|anybody have|"
        "anybody got|any1 have|any1 got");
    g_ConjureRules.polite = phrases("OllamaChat.Conjure.PoliteWords",
        "pls|plz|please|pleas|plox|plis|pl0x|ples|plx|can you|can u|could you|could u|would you|would u|"
        "will you|will u|how about");
    g_ConjureRules.needs = phrases("OllamaChat.Conjure.NeedWords",
        "need|needs|neeed|nid|want|wanna|could use|out of|outta|low on|ran out of|run out of|running low on|"
        "all out of");
    g_ConjureRules.mage = phrases("OllamaChat.Conjure.MageWords", "mage|mages|magey|wizard");
    g_ConjureRules.negations = phrases("OllamaChat.Conjure.NegationWords",
        "not|dont|didnt|doesnt|no|nope|nah|never|without|aint|isnt|arent|havent|wont");
    g_ConjureRules.haves = phrases("OllamaChat.Conjure.HaveWords",
        "i have|i got|ive got|i still have|still have|already have|already got|got plenty|have plenty|"
        "plenty of|im good|im fine|im set|all good|enough");
    g_ConjureRules.thanks = phrases("OllamaChat.Conjure.ThanksWords",
        "ty|thx|thnx|thanks|thank you|thank u|tyvm|tysm|tyty|cheers|appreciate*");
    g_ConjureRules.questions = phrases("OllamaChat.Conjure.QuestionWords",
        "what|where|why|when|which|how|whats|wheres|whys|hows");
    g_ConjureRules.commands = phrases("OllamaChat.Conjure.CommandWords",
        "t|nt|u|use|s|b|e|c|ue|de|q|r|ll|ss|co|nc|destroy|drop|equip|share|sell|buy|vendor|items|inv|inventory|"
        "loot|cast|castnc|drink|roll|mail|sendmail|craft|bank|gb|gbank");
    g_ConjureRules.maxWords = sConfigMgr->GetOption<uint32_t>("OllamaChat.Conjure.MaxWords", 16);

    g_ConjureStacks      = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Conjure.Stacks", 1), 1, 6);
    g_ConjureMaxStacks   = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Conjure.MaxStacks", 3),
                                                g_ConjureStacks, 6);
    g_ConjureUseMana     = sConfigMgr->GetOption<bool>("OllamaChat.Conjure.UseMana", true);
    g_ConjureWaitSeconds = std::max<uint32_t>(10, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conjure.WaitSeconds", 90));
    g_ConjureTradeSeconds = std::max<uint32_t>(10, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conjure.TradeSeconds", 60));

    g_ConjureHintGive = sConfigMgr->GetOption<std::string>("OllamaChat.Conjure.HintGive",
        "{player_name} asked you for {request}. You are a mage, so you have conjured {gift} and are opening a "
        "trade to give it to them, free. Tell them in a few words, in your own chat style. Never ask for gold "
        "or anything in return.");
    g_ConjureHintWait = sConfigMgr->GetOption<std::string>("OllamaChat.Conjure.HintWait",
        "{player_name} asked you for {request}. You will conjure {gift} and trade it to them, free, {when}. "
        "Tell them so in a few words, in your own chat style. Never ask for gold or anything in return.");
    g_ConjureHintCant = sConfigMgr->GetOption<std::string>("OllamaChat.Conjure.HintCant",
        "{player_name} asked you for {request}, but you can't give them any: {reason}. Tell them why in a few "
        "words, in your own chat style.");
    g_ConjureHintNotMage = sConfigMgr->GetOption<std::string>("OllamaChat.Conjure.HintNotMage",
        "{player_name} asked you for {request}, but you are a {class}, not a mage, so you can't conjure "
        "it.{mage} Tell them in a few words, in your own chat style.");
    g_ConjureHintAlready = sConfigMgr->GetOption<std::string>("OllamaChat.Conjure.HintAlready",
        "{player_name} asked you for {request} again, but you have already offered them a trade with it. Tell "
        "them in a few words to accept the trade.");
    g_ConjureHintNotGrouped = sConfigMgr->GetOption<std::string>("OllamaChat.Conjure.HintNotGrouped",
        "{player_name} asked you for {request}, but you only hand out conjured food and water to your own "
        "group, and they are not in it. Tell them in a few words, in your own chat style, to invite you if "
        "they want some.");

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Conjured food and water on request: {} ({} stack(s), up to {}, mana {}, waits {} s, "
             "trades open {} s).",
             g_ConjureEnable ? "on" : "off", g_ConjureStacks, g_ConjureMaxStacks,
             g_ConjureUseMana ? "used" : "free", g_ConjureWaitSeconds, g_ConjureTradeSeconds);
}

static void LoadNaturalChatV7Settings()
{
    auto phrases = [](const char* key, const char* def)
    {
        return CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(key, def));
    };

    // --- replies --------------------------------------------------------------
    g_ResponseDropVocatives = phrases("OllamaChat.Response.DropVocatives", "kid|kiddo");
    g_ResponseInsultWords = phrases("OllamaChat.Response.InsultWords",
        "knob|knobhead|knobend|nob|nobhead|twat|dickhead|bellend|pillock|wanker|tosser|prat|muppet|numpty|"
        "plonker|wally|idiot|moron|berk|noob|nub|loser|weirdo");
    g_ResponseProvokeWords = phrases("OllamaChat.Response.ProvokeWords",
        "fuck*|shit*|bastard*|bollocks|stfu|gtfo|shut up|ur mum|your mum|ur nan|your nan|yo mama|ur mom|"
        "your mom|l2p|learn to play|rekt|cope|useless|clueless|stupid|dumb|bad at|suck|sucks|banter|bants");
    g_ResponseInsultPersonalities = ReadPipeList("OllamaChat.Response.InsultPersonalities", "NAT_RUDE|NAT_KEYBOARD");
    g_ResponseInsultCooldownSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Response.InsultCooldownSeconds", 600);
    g_ResponseRarePhrases = phrases("OllamaChat.Response.RarePhrases",
        "40 man|40man|forty man|40 mans|old school|back in vanilla|back in the day|the old days|old days|"
        "good old days|vanilla days|miss vanilla|miss the old|used to be better|way more epic back|ur nan|your nan|"
        "ur nans|your nans|ur mum|your mum|ur mums|your mums|ur mom|your mom|ur ma|yo mama|yo momma|ur sister|"
        "your sister|ur gran|your gran|ur granny|your granny|ur dad|your dad");
    g_ResponseRareCooldownSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Response.RareCooldownSeconds", 3600);

    // --- thanks -----------------------------------------------------------------
    g_CrowdThanksWords = phrases("OllamaChat.Crowd.ThanksWords",
        "ty|thx|thnx|thanks|thank|tyvm|tysm|tyty|ta|cheers|appreciate*|appreciated");
    g_CrowdThanksReplyChance = std::min<uint32_t>(100,
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.ThanksReplyChance", 0));

    // --- a dead bot knows it is dead -------------------------------------------------
    g_StateDeadText = sConfigMgr->GetOption<std::string>("OllamaChat.State.Dead",
        "Right now you are dead: you died and are lying where you fell.");
    g_StateGhostText = sConfigMgr->GetOption<std::string>("OllamaChat.State.Ghost",
        "Right now you are dead: you released your spirit and are running back to your body as a ghost.");

    // --- orders in plain words ---------------------------------------------------
    auto orders = [](const char* key, const char* def)
    {
        return TextRules::ParseCommandPhrases(sConfigMgr->GetOption<std::string>(key, def));
    };
    using TextRules::BotIntent;
    auto slot = [](BotIntent i) -> std::vector<TextRules::CommandPhrase>&
    {
        return g_OrdersRules.phrases[static_cast<size_t>(i)];
    };

    g_OrdersEnable = sConfigMgr->GetOption<bool>("OllamaChat.Orders.Enable", true);
    slot(BotIntent::Stay) = orders("OllamaChat.Orders.Stay",
        "=stay|=hold|stay here|stay there|stay back|stay put|stay behind|stay where you are|stay where u are|"
        "stay right there|stay right here|wait here|wait there|wait right here|wait right there|wait back|"
        "wait for me|wait for me here|wait for me there|hold here|hold there|hold position|hold your position|hold up here|"
        "stand here|stand there|stand still|stand back|dont move|do not move|stop following|quit following|"
        "hang back|camp here|park here|park up");
    slot(BotIntent::Follow) = orders("OllamaChat.Orders.Follow",
        "=follow|follow me|follow us|come with me|come with us|come w me|=with me|=on me|come here|"
        "come over here|=over here|get over here|come back|come back here|come to me|regroup|regroup on me|"
        "=this way|move out|lets move|keep up|catch up|follow again|start following|you can follow|"
        "come along|tag along|stick with me|stay with me|stay close|stay close to me|get back here|=get back");
    slot(BotIntent::Attack) = orders("OllamaChat.Orders.Attack",
        "attack|attack my target|attack that|attack it|attack him|attack her|attack them|attack this|kill it|"
        "kill that|kill him|kill her|kill them|kill this|kill my target|get him|get her|"
        "focus my target|focus that|focus it|focus him|focus her|assist me|dps that|dps it|nuke it|nuke that|"
        "burn it|burn that|take it down|take that down");
    slot(BotIntent::Flee) = orders("OllamaChat.Orders.Flee",
        "=run|=run run|run away|flee|retreat|fall back|get out|get out of there|get out of here|back off|abort|leg it|"
        "scatter");
    slot(BotIntent::Revive) = orders("OllamaChat.Orders.Revive",
        "release|=res|=rez|res up|rez up|respawn|=spawn|run back|run back here|corpse run|get up|get back up|"
        "come back to life|=revive|revive yourself|resurrect|back to your body|go get your body|"
        "get your corpse");
    slot(BotIntent::Trade) = orders("OllamaChat.Orders.Trade",
        "=trade|trade me|come trade me|come trade|open trade|open a trade|lets trade|trade with me|"
        "come and trade|come and trade me|come over and trade");
    slot(BotIntent::Hearth) = orders("OllamaChat.Orders.Hearth",
        "hearth|hearth out|hearth home|hearth back|use your hearth|use your hearthstone|use your hs|use hearth|"
        "use hearthstone|=hs|hs out|hs home|hearthstone out");
    slot(BotIntent::Unstuck) = orders("OllamaChat.Orders.Unstuck",
        "unstuck|use unstuck|use the unstuck|get unstuck|unstuck yourself|youre stuck|ur stuck|you are stuck|"
        "youre stuck in|ur stuck in|you are stuck in|youre stuck on|ur stuck on");
    slot(BotIntent::Buff) = orders("OllamaChat.Orders.Buff",
        "=buff|=buffs|buff me|buff us|rebuff|need buffs|buff up|buff everyone|buff the group|"
        "can i get a buff|can i get buffs");
    slot(BotIntent::Pull) = orders("OllamaChat.Orders.Pull",
        "=pull|pull it|pull that|pull them|you can pull|pull when ready|start pulling|tank pull|pull the next|"
        "pull next");
    slot(BotIntent::Leave) = orders("OllamaChat.Orders.Leave",
        "leave the group|leave group|leave the party|leave party|you can leave|you can leave now|drop group|"
        "drop party");
    slot(BotIntent::Grind) = orders("OllamaChat.Orders.Grind",
        "=grind|grind mobs|grind some mobs|grind here|grind for a bit|grind a bit|start grinding|keep grinding|"
        "you can grind|kill stuff|kill some stuff|kill some mobs|kill things|kill everything|kill everything here|"
        "farm mobs|farm some mobs|=farm|go farm|go level|level up a bit|fight stuff|clear the area|clear this area|"
        "clear the camp|spread out and grind|spread out and kill stuff");
    slot(BotIntent::Resume) = orders("OllamaChat.Orders.Resume",
        "carry on|=resume|resume what you were doing|go back to what you were doing|back to what you were doing|"
        "get back to what you were doing|do what you were doing|go do what you were doing|do your thing|"
        "do your own thing|go do your thing|go do your own thing|you do you|do your quest|do your quests|"
        "go do your quest|go do your quests|go do that|go do it|=on|on you go|off you go|be on your way|"
        "go about your business|crack on|get on with it|get on with your quest|get on with your quests|"
        "as you were|lead the way|lead on|you lead|you lead the way|ill follow you|i will follow you|ill follow|"
        "ill tag along|ill come with you|ill go with you|dont mind me|dont let me stop you|dont let me keep you");
    g_OrdersRules.fillers = phrases("OllamaChat.Orders.Fillers",
        "pls|plz|please|pleas|plox|now|right now|here|there|then|again|quick|quickly|asap|for now|for a sec|"
        "a sec|for a second|a second|a moment|for a moment|a minute|for a minute|a bit|for a bit|ok|okay|k|kk|"
        "alright|right|yeah|yes|you|u|ya|ye|mate|m8|buddy|man|dude|bro|hun|hon|thanks|ty|cheers|too|"
        "as well|also|just|real quick|rq|lol|go|ahead|go ahead|the|a");
    g_OrdersRules.polite = phrases("OllamaChat.Orders.Polite",
        "can you|can u|could you|could u|would you|would u|will you|will u|can ya|could ya|pls|plz|please|"
        "do you mind|would you mind|mind|wanna");
    g_OrdersRules.everyone = phrases("OllamaChat.Orders.Everyone",
        "everyone|everybody|every1|evry1|all|yall|you all|u all|guys|you guys|u guys|team|group|lads|people|"
        "ppl|folks|party|squad");
    g_OrdersRules.negations = phrases("OllamaChat.Orders.Negations",
        "dont|do not|didnt|doesnt|never|no need to|no need|not|stop|quit|nah|nope");
    g_OrdersRules.questions = phrases("OllamaChat.Orders.Questions",
        "why|where|what|when|how|who|whos|whats|wheres|hows|whys|should|shall|are|is|do|does|did|was|were|"
        "have|has|wonder");
    g_OrdersRules.conditions = phrases("OllamaChat.Orders.Conditions",
        "if|unless|whenever|once|incase|in case");
    g_OrdersRules.separators = phrases("OllamaChat.Orders.Separators",
        "and|while|whilst|till|until|then|but|so|cause|cuz|coz|because|bc|or");
    g_OrdersRules.subjects = phrases("OllamaChat.Orders.Subjects",
        "i|im|ill|ive|id|imma|ima|gonna|gotta|he|hes|she|shes|they|theyre|theyll|theyve");
    g_OrdersRules.maxWords = sConfigMgr->GetOption<uint32_t>("OllamaChat.Orders.MaxWords", 14);
    g_OrdersRules.maxStray = sConfigMgr->GetOption<uint32_t>("OllamaChat.Orders.MaxStray", 1);

    // What a bot says it is doing for each order.
    auto hint = [](BotIntent i, const char* key, const char* def)
    {
        g_OrdersHint[static_cast<size_t>(i)] = sConfigMgr->GetOption<std::string>(key, def);
    };
    hint(BotIntent::Stay, "OllamaChat.Orders.HintStay",
        "{player_name} told {who} to stay put, and you are doing it: you will wait right here until they say to "
        "follow again. Say so in a few words, in your own chat style, the way you would tell them to give you a "
        "shout when they want you following again.");
    hint(BotIntent::Follow, "OllamaChat.Orders.HintFollow",
        "{player_name} told {who} to follow them, and you are on your way. Say so in two to six words, in your "
        "own chat style.");
    hint(BotIntent::Attack, "OllamaChat.Orders.HintAttack",
        "{player_name} told {who} to attack {target}, and you are going in. Say so in two to five words, in your "
        "own chat style.");
    hint(BotIntent::Flee, "OllamaChat.Orders.HintFlee",
        "{player_name} told {who} to get out of the fight, so you are breaking off and running back to them. Say "
        "so in two to five words, in your own chat style.");
    hint(BotIntent::Revive, "OllamaChat.Orders.HintRevive",
        "You died, and {player_name} told you to get yourself back up: you are {doing}. Say so in a few words, "
        "in your own chat style.");
    hint(BotIntent::Trade, "OllamaChat.Orders.HintTrade",
        "{player_name} asked you to trade with them, and you are {doing}. Say so in a few words, in your own "
        "chat style.");
    hint(BotIntent::Hearth, "OllamaChat.Orders.HintHearth",
        "{player_name} told {who} to use your hearthstone, so you are hearthing home now. Say so in a few words, "
        "in your own chat style.");
    hint(BotIntent::Unstuck, "OllamaChat.Orders.HintUnstuck",
        "You were stuck, and {player_name} told you to use the unstuck option. You have: in a moment you will be "
        "{where}, and you will make your way back from there. Say so in a few words, in your own chat style.");
    hint(BotIntent::Buff, "OllamaChat.Orders.HintBuff",
        "{player_name} asked for buffs, and you are buffing the group now. Say so in two to five words, in your "
        "own chat style.");
    hint(BotIntent::Pull, "OllamaChat.Orders.HintPull",
        "{player_name} told you to pull, and you are pulling {target} now. Say so in two to five words, in your "
        "own chat style.");
    hint(BotIntent::Grind, "OllamaChat.Orders.HintGrind",
        "{player_name} told {who} to go off and grind, so you are going to fight whatever is around here for a "
        "while. Say so in a few words, in your own chat style.");
    hint(BotIntent::Resume, "OllamaChat.Orders.HintResume",
        "{player_name} told {who} to get on with what you were doing before, so you are heading off to {doing} -- "
        "still in their group, and they can tag along if they like. Say so in a few words, in your own chat "
        "style.");
    hint(BotIntent::Leave, "OllamaChat.Orders.HintLeave",
        "{player_name} said you can leave the group, so you are leaving now. Say a short goodbye, in your own "
        "chat style.");
    g_OrdersHintCant = sConfigMgr->GetOption<std::string>("OllamaChat.Orders.HintCant",
        "{player_name} asked you to {order}, but you cannot right now: {reason}. Say so in a few words, in your "
        "own chat style.");
    g_OrdersTradeWalkSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Orders.TradeWalkSeconds", 20);
    g_OrdersFleeRecoverSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Orders.FleeRecoverSeconds", 8);
    g_StateStayingText = sConfigMgr->GetOption<std::string>("OllamaChat.State.Staying",
        "Right now you are waiting where {player_name} told you to stay, until they tell you to follow again.");

    // --- the rogue saying why it is not sapping the star (mod-playerbots-plus's marks) ---
    g_MarksSayEnable             = sConfigMgr->GetOption<bool>("OllamaChat.Marks.SayEnable", true);
    g_MarksSayCooldownSeconds    = sConfigMgr->GetOption<uint32_t>("OllamaChat.Marks.SayCooldownSeconds", 120);
    g_MarksSayPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Marks.SayPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in a group. Your chat style: {bot_personality} "
        "{situation} Write it as one party chat line, 3 to 12 words, the way a player types it: no quotes, no name in front.");
    g_MarksSapNotLearned = sConfigMgr->GetOption<std::string>("OllamaChat.Marks.SapNotLearned",
        "{player_name} just put the star on {target} for you to sap, but you can't sap yet: {detail}. Tell them, so "
        "they don't count on it.");
    g_MarksSapWrongType = sConfigMgr->GetOption<std::string>("OllamaChat.Marks.SapWrongType",
        "{player_name} just put the star on {target} for you to sap, but you can't: {detail}. Tell them you can't sap "
        "that one.");
    g_MarksSapNoStealth = sConfigMgr->GetOption<std::string>("OllamaChat.Marks.SapNoStealth",
        "{player_name} put the star on {target} for you to sap, but you could not go into stealth in time ({detail}), "
        "and walking up to it without stealth would pull it. Tell them you are leaving it.");
    g_MarksSapFailed = sConfigMgr->GetOption<std::string>("OllamaChat.Marks.SapFailed",
        "You tried to sap {target} for {player_name} and it did not work: {detail}. Tell them in a few words.");

    // --- what bots carry, and handing over what they can spare ------------------------
    g_BagsEnable            = sConfigMgr->GetOption<bool>("OllamaChat.Bags.Enable", true);
    g_BagsRecentMinutes     = sConfigMgr->GetOption<uint32_t>("OllamaChat.Bags.RecentMinutes", 30);
    g_BagsOfferSeconds      = sConfigMgr->GetOption<uint32_t>("OllamaChat.Bags.OfferSeconds", 180);
    g_BagsInviteWaitSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Bags.InviteWaitSeconds", 120);
    g_BagsMaxSpare          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Bags.MaxSpare", 6);
    g_BagsMaxKept           = sConfigMgr->GetOption<uint32_t>("OllamaChat.Bags.MaxKept", 3);
    g_BagsMaxGiftQuality    = sConfigMgr->GetOption<uint32_t>("OllamaChat.Bags.MaxGiftQuality", 2);
    g_BagsMaxGiftCopper     = sConfigMgr->GetOption<uint32_t>("OllamaChat.Bags.MaxGiftCopper", 20000);
    g_BagsGiveAway          = sConfigMgr->GetOption<bool>("OllamaChat.Bags.GiveAway", false);
    g_BagsNoGiftReason = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.NoGiftReason",
        "you have no use for it, but you don't give your things away");
    g_BagsSupplyReason = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.SupplyReason",
        "it is one of your own supplies");
    g_BagsPromptText = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.PromptText",
        "What you have on you (bring up only what fits what they said): you could spare {spare}; you are keeping "
        "{kept}; and you have {money}. Asked for something you can spare, you are happy to let them have it; asked "
        "for something you are keeping, say no and why. Never offer anything that is not listed here, and never "
        "offer to give anything away that is not on the spare list; name what you mention exactly, in square "
        "brackets.");
    g_BagsTalkPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Bags.TalkWords",
        "trade|trading|tradeable|loot|looted|loots|drop|dropped|drops|bags|inventory|spare|sell|selling|wts|wtb|"
        "for sale|what do you have|what have you got|what you got|what u got|whatcha got|watcha got|got anything|"
        "have anything|anything good|anything nice|anything for me|do you have|have you got|got any|you got any|"
        "u got any|any spare|need any|items|gear|can i have|could i have|can i get|could i get|give me|gimme|"
        "trade me|ill take"));
    g_BagsAskPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Bags.AskWords",
        "can i have|could i have|can i get|could i get|can i grab|can i take|may i have|give me|gimme|giv me|"
        "trade me|trade it|send me|send it|pass me|hand me|ill take|i will take|ill have|i will have|i want|"
        "id like|i would like|id love|can you trade|could you trade|can u trade|dibs"));
    g_BagsYesPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Bags.YesWords",
        "me|mee|meee|me2|mine|dibs|yes|yess|yeah|yea|ye|yep|yup|sure|please|pls|plz|plez|i|would|want|it|that|"
        "one|oh|ooh|too|do|go|on|then|trade|ok|okay"));
    g_BagsClaimPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Bags.ClaimWords",
        "me|mee|meee|me2|mine|dibs|yes|yess|yeah|yea|ye|yep|yup|sure|want|go on then|go on|trade"));
    g_BagsNotPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Bags.NotWords",
        "dont|do not|doesnt|not|no|nah|nope|never|cant|wont|didnt"));
    g_BagsHintGive = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HintGive",
        "{player_name} asked for {item}, which you have no use for, so you are trading it over to them {when}. Say "
        "so in a few words, in your own way.");
    g_BagsHintInvite = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HintInvite",
        "{player_name} wants {item}, which you could spare, but you only trade with people in your group: tell them "
        "to invite you and you will trade it over.");
    g_BagsHintKeep = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HintKeep",
        "{player_name} asked for {item}, but you are keeping it: {reason}. Say no, and why, in your own way.");
    g_BagsHintNotHave = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HintNotHave",
        "{player_name} asked for {item}, but you do not have one. Say so.");
    g_BagsHintCant = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HintCant",
        "{player_name} asked for {item}, and you would give it to them, but you can't right now: {reason}. Say so.");
    g_BagsHintAlready = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HintAlready",
        "{player_name} asked again for {item}, which is going over to them already: it is in the trade window, or "
        "will be as soon as the window is open. Say so in a few words.");
    g_BagsHandoverText = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HandoverText",
        "Saying so hands nothing over: things only change hands in a trade window, and you are told when one is "
        "opening. So never say you have given, sent, traded or put anything in their bags.");

    // --- asks with a real reason behind them --------------------------------------------
    g_NeedsEnable              = sConfigMgr->GetOption<bool>("OllamaChat.Needs.Enable", true);
    g_NeedsChance              = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.Chance", 20);
    g_NeedsBotCooldownMinutes  = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.BotCooldownMinutes", 20);
    g_NeedsRepeatMinutes       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.RepeatMinutes", 60);
    g_NeedsZoneCooldownSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.ZoneCooldownSeconds", 300);
    g_NeedsRememberMinutes     = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.RememberMinutes", 15);
    g_NeedsWeight[0]           = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.WeightQuest", 40);
    g_NeedsWeight[1]           = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.WeightErrand", 25);
    g_NeedsWeight[2]           = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.WeightSpare", 25);
    g_NeedsWeight[3]           = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.WeightShort", 10);
    g_NeedsShortMaxLevel       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.ShortMaxLevel", 15);
    g_NeedsShortCopperPerLevel = sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.ShortCopperPerLevel", 1200);
    g_NeedsPromptTemplate = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.PromptTemplate",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in {bot_zone}. Your chat style: "
        "{bot_personality} {need} Write the line you type into {channel} for it, the way players do: short and "
        "plain, 2 to 12 words, no greeting, no name in front, nothing made up beyond what is given here.");
    g_NeedsReasonText = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.ReasonText",
        "A little while ago you asked something in chat because {reason}. If anyone asks about it or answers it, "
        "that is what it was about.");
    g_NeedsPlanText = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.PlanText",
        "What you are actually doing right now: {doing}. Answer from that, never from anything made up: if they "
        "ask about one of these, you are on it; if they offer to help with one, that is useful to you.");
    g_NeedsPlanPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Needs.PlanWords",
        "what you doing|what u doing|what are you doing|what r u doing|whatcha doing|watcha doing|wyd|"
        "what you up to|what u up to|what are you up to|you up to|you going|u going|going to do|gonna do|"
        "going for|you after|u after|plans|plan|heading|where you going|where u going|where are you going|"
        "wanna do|want to do|need help|need a hand|help with|group for|questing|quest|quests|working on|farming|"
        "you killing|u killing"));
    g_NeedsPlanMaxQuests = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Needs.PlanMaxQuests", 4));
    g_NeedsAskQuest = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.AskQuest",
        "You are on the quest {quest} in {where}{target}, {kind}, too much to do alone at your level. What it asks: "
        "{objective} Ask if anyone wants to group up for it, the way players looking for a group do, saying what it "
        "is you need a hand with.");
    g_NeedsAskDungeon = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.AskDungeon",
        "You have {quest}, a quest for {where}, in your log. Ask if anyone wants to run {where}, the way players "
        "looking for a dungeon group do.");
    g_NeedsReasonQuest = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.ReasonQuest",
        "you are on {quest} in {where}{target}, {kind}, and need people for it (it asks: {objective}); anyone who "
        "wants to come can invite you");
    g_NeedsAskErrand = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.AskErrand",
        "You are in {city} and {why}, so you want to know {what}. Ask, the way a player who is not sure where things "
        "are would.");
    g_NeedsReasonErrand = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.ReasonErrand",
        "you are in {city} and {why}, so you asked {what}");
    g_NeedsAskSpare = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.AskSpare",
        "You have {item}{looted} in your bags and no use for it. Offer it, free, to anyone who wants it, naming it "
        "in square brackets.");
    g_NeedsReasonSpare = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.ReasonSpare",
        "you offered {item}, which you have no use for: whoever wants it can have it, in a trade if they are in "
        "your group, and anyone else has to invite you first");
    g_NeedsAskShort = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.AskShort",
        "You are new and have only {money} to your name, saving for {want}. Ask if anyone could spare a few silver, "
        "the way new players do: friendly, not pushy.");
    g_NeedsReasonShort = sConfigMgr->GetOption<std::string>("OllamaChat.Needs.ReasonShort",
        "you only have {money} and are saving for {want}, so you asked for a few silver");

    // --- a bot tank greeting the group, and calling the finish ----------------------
    g_PartyAnnouncePrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Party.AnnouncePrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}, the tank of your group in {dungeon}. "
        "Your chat style: {bot_personality} {situation} Write it as one party chat line, 3 to 12 words, the way a "
        "player types it: no quotes, no name in front.{recent_acks}");
    g_PartyGreetEnable = sConfigMgr->GetOption<bool>("OllamaChat.Party.GreetEnable", true);
    g_PartyGreetDelaySeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.GreetDelaySeconds", 8);
    g_PartyGreetSituation = sConfigMgr->GetOption<std::string>("OllamaChat.Party.GreetSituation",
        "The group has just come in and nobody has pulled anything yet. {~Ask everyone if they are ready to start|"
        "Check everyone is ready before you start|Ask if they are ready for the first pull|Ask whether they are "
        "ready for you to start pulling}, in your own way and your own words: a plain are-you-ready question, "
        "not whether anyone needs anything.");
    g_PartyDoneEnable = sConfigMgr->GetOption<bool>("OllamaChat.Party.DoneEnable", true);
    g_PartyDoneSituation = sConfigMgr->GetOption<std::string>("OllamaChat.Party.DoneSituation",
        "Every boss in {dungeon} is dead: the group has cleared the whole dungeon and the run is over. Say that "
        "plainly first - {dungeon} is cleared, {bosses} - so nobody can miss that it is done, then celebrate it "
        "your own way: gg, thanks, nice work, whatever suits you.");

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Replies: {} word(s) dropped as forms of address, {} rare phrase(s) once per {} s; "
             "thank-yous answered {}% of the time.",
             g_ResponseDropVocatives.size(), g_ResponseRarePhrases.size(), g_ResponseRareCooldownSeconds,
             g_CrowdThanksReplyChance);
}

// "a|b|c" lists, and "KEY:N" pairs separated by spaces.
static std::vector<std::string> ReadSpaceList(const char* key, const char* def)
{
    std::vector<std::string> out;
    const std::string raw = sConfigMgr->GetOption<std::string>(key, def);
    std::string cur;
    for (char c : raw + " ")
    {
        if (c == ' ' || c == '\t')
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    return out;
}

static void LoadNaturalChatV10Settings()
{
    // --- a bot that was stuck getting on and off its mount (mod-playerbots-plus finds it) ---
    g_StuckRememberMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Stuck.RememberMinutes", 10));
    g_StuckRemarkChance    = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Stuck.RemarkChance", 50));
    g_StuckRemarkPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Stuck.RemarkPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}. Your chat style: {bot_personality} For the "
        "last minute you were stuck at {what}, getting on and off your mount over and over without managing to "
        "{verb} it, and you have only just noticed and given up on it. Say it in {channel} in a few words, your own "
        "way: fed up, embarrassed or laughing at yourself, whatever suits you.");
    g_StuckStateText = sConfigMgr->GetOption<std::string>("OllamaChat.Stuck.StateText",
        "A little while ago you were stuck for a minute at {what} in {zone}, getting on and off your mount on the "
        "spot without managing to {verb} it, until you gave up on it and moved on. If someone asks whether you are "
        "stuck or what you were doing, that is what happened.");

    // --- quest progress in party, not whispered ---------------------------------------
    g_ProgressEnable       = sConfigMgr->GetOption<bool>("OllamaChat.Progress.Enable", true);
    g_ProgressCheckSeconds = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Progress.CheckSeconds", 2));
    g_ProgressDelaySeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Progress.DelaySeconds", 3);
    g_ProgressGapSeconds   = sConfigMgr->GetOption<uint32_t>("OllamaChat.Progress.GapSeconds", 20);
    g_ProgressPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Progress.Prompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}, questing with your group. Your chat style: "
        "{bot_personality} You have just got {done}. {~Tell the group|Let the group know|Say so} in party chat in a "
        "few words, the way players do when a bit of a quest is done: no exclamation-mark hype, and nobody needs "
        "every detail.");
    g_ProgressKillText = sConfigMgr->GetOption<std::string>("OllamaChat.Progress.KillText",
        "the last of the {count} {what} you had to kill for {quest}");
    g_ProgressUseText = sConfigMgr->GetOption<std::string>("OllamaChat.Progress.UseText",
        "the last of the {count} {what} you had to do for {quest}");
    g_ProgressItemText = sConfigMgr->GetOption<std::string>("OllamaChat.Progress.ItemText",
        "the last of the {count} {what} that {quest} wants");
    g_ProgressQuestText = sConfigMgr->GetOption<std::string>("OllamaChat.Progress.QuestText",
        "everything {quest} needs: it is ready to hand in to {ender}");

    // --- what a named item or quest is -----------------------------------------------
    g_LinksDescribeEnable     = sConfigMgr->GetOption<bool>("OllamaChat.Links.DescribeEnable", true);
    g_LinksDescribeMax        = sConfigMgr->GetOption<uint32_t>("OllamaChat.Links.DescribeMax", 3);
    g_LinksDescribeQuestChars = std::max<uint32_t>(40, sConfigMgr->GetOption<uint32_t>("OllamaChat.Links.DescribeQuestChars", 180));
    g_LinksDescribeText = sConfigMgr->GetOption<std::string>("OllamaChat.Links.DescribeText",
        "So you know what that is about: {things}. Talk about each as what it is.");

    // --- premises with slots ------------------------------------------------------------
    g_PremiseTemplates = ReadChoiceList("OllamaChat.Conversation.PremiseTemplates", "");
    g_PremiseTemplateChance  = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.PremiseTemplateChance", 70));
    g_PremiseCooldownMinutes = sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.PremiseCooldownMinutes", 120);
    g_PremiseValueMemory     = sConfigMgr->GetOption<uint32_t>("OllamaChat.Conversation.PremiseValueMemory", 8);
    g_PremisePools.clear();
    {
        const std::string prefix = "OllamaChat.Conversation.Pool.";
        for (const std::string& key : sConfigMgr->GetKeysByString(prefix))
        {
            std::string name = key.substr(prefix.size());
            for (char& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::vector<std::string> values = ReadChoiceList(key.c_str(), "");
            if (!name.empty() && !values.empty())
                g_PremisePools[name] = std::move(values);
        }
    }

    // --- the same phrase from bot after bot ---------------------------------------------
    g_EchoEnable        = sConfigMgr->GetOption<bool>("OllamaChat.Echo.Enable", true);
    g_EchoWords         = std::max<uint32_t>(3, sConfigMgr->GetOption<uint32_t>("OllamaChat.Echo.Words", 4));
    g_EchoBots          = std::max<uint32_t>(2, sConfigMgr->GetOption<uint32_t>("OllamaChat.Echo.Bots", 3));
    g_EchoWindowMinutes = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Echo.WindowMinutes", 60));
    g_EchoCommonWords = ReadPipeList("OllamaChat.Echo.CommonWords",
        "i|im|ive|ill|id|you|your|youre|ur|u|ya|the|a|an|to|of|in|on|at|for|with|and|or|but|so|it|its|is|are|was|"
        "were|be|been|am|do|does|did|dont|doesnt|didnt|not|no|yes|yeah|yep|just|get|got|go|going|gonna|can|cant|"
        "will|wont|would|could|should|what|how|why|where|when|who|that|this|there|here|they|them|we|us|me|my|he|"
        "she|his|her|have|has|had|if|then|than|too|up|out|all|any|some|one|lol|haha|ok|okay|oh|ah|like|really|well|"
        "now|anyone|someone|need|want|know|think|see|mean|sure|right|bit|lot|much|more|good|nice|thanks|ty|thx|"
        "about|from|by|as|off|over|still|again|back|yet|never|always|maybe|might|let|lets|make|take|give|come|say|"
        "said|thing|things|stuff|way|time|mate|guys|lads|man|though|tho|even|only|also");
    g_ResponseRetryOwed = sConfigMgr->GetOption<bool>("OllamaChat.Response.RetryOwed", true);
    g_ResponseRetryNote = sConfigMgr->GetOption<std::string>("OllamaChat.Response.RetryNote",
        "Say it a different way this time, without '{phrase}'.");

    // --- text emoticons ------------------------------------------------------------------
    g_EmoticonEnable = sConfigMgr->GetOption<bool>("OllamaChat.Emoticons.Enable", true);
    g_EmoticonTokens = ReadSpaceList("OllamaChat.Emoticons.Tokens",
        ":) :-) :( :-( :D :-D xD :P :-P ;) ;-) :O o.O O.o o.o >.< >_< <3 ^^ ^_^ :/ D: :3 =) =D -_- T_T :S ;P :]");
    g_EmoticonOffer = ReadSpaceList("OllamaChat.Emoticons.Offer", ":) :D xD :P ;) :O >.< <3 ^^ :( o.O -_- :/");
    g_EmoticonNudgeChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Emoticons.NudgeChance", 4));
    g_EmoticonStyleChance.clear();
    for (const std::string& pair : ReadSpaceList("OllamaChat.Emoticons.StyleChance",
             "NAT_EXCITABLE:15 NAT_BUTTERFLY:15 NAT_HYPE:12 NAT_REACTOR:12 NAT_SOCIAL:10 NAT_FRIENDLY:8 NAT_JOKER:8 "
             "NAT_NEWBIE:8 NAT_PETLOVER:8 NAT_LOWERCASE:6 NAT_PRANKSTER:6 NAT_CHILL:5 NAT_BANTER:5 NAT_TERSE_VET:0 "
             "NAT_PROPER:1 NAT_DEADPAN:1 NAT_GRUMBLER:1 NAT_RUDE:1 NAT_LONER:1 NAT_EXPERT:1 NAT_MINMAXER:1 NAT_QUIET:2"))
    {
        const size_t colon = pair.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= pair.size())
            continue;
        std::string key = pair.substr(0, colon);
        for (char& c : key)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const std::string num = pair.substr(colon + 1);
        if (std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) && num.size() <= 3)
            g_EmoticonStyleChance[key] = std::min<uint32_t>(100, uint32_t(std::stoul(num)));
    }
    g_EmoticonNudgeText = sConfigMgr->GetOption<std::string>("OllamaChat.Emoticons.NudgeText",
        "Some people put a smiley in a line now and then. If it really fits this one, you can use {emoticons}; "
        "most of the time you would not.");
    g_EmoticonBotCooldownMinutes = sConfigMgr->GetOption<uint32_t>("OllamaChat.Emoticons.BotCooldownMinutes", 20);
    g_EmoticonScopeLines         = sConfigMgr->GetOption<uint32_t>("OllamaChat.Emoticons.ScopeLines", 8);

    // --- dungeon start ---------------------------------------------------------------------
    g_PartyReadyYesPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Party.ReadyYesPhrases",
        "yes|yep|yup|yeah|ye|ya|yea|y|sure|aye|go|gogo|go go|of course|ofc|sure thing|why not|"
        "mhm|yes please|do it|lets do it|go for it|go ahead|go on|go on then|whenever|when you are|"
        "ready when you are|yes im ready|yeah im ready|yep im ready|born ready|yes lets go|"
        "yeah lets go|yep lets go|lets"));
    g_PartyReadyWindowSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.ReadyWindowSeconds", 180);
    g_PartyTogetherText = sConfigMgr->GetOption<std::string>("OllamaChat.Party.TogetherText",
        "You and everyone in this group are inside {dungeon} together, doing it as one group{tank}. You are part of "
        "this run: never talk about it as if they were going without you, and anything you ask about or bring up "
        "is about this place, not quests or groups somewhere else.");
    g_JoinedLfgPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Joined.LfgPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} playing World of Warcraft. Your chat style: "
        "{bot_personality} The dungeon finder has just put you in a group with {player_name} and a few others for a "
        "dungeon. Type the one short line you would say in party chat as the group forms, the way people do in a "
        "dungeon finder group: a quick hello at most, a few words, in your own style. You are all off to the same "
        "dungeon and the tank will start it, so do not ask where you are going or what the plan is.");

    // --- taking a gift back out of the window ----------------------------------------------
    g_BagsCancelPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Bags.CancelPhrases",
        "take it out|take them out|take that out|take it back|take them back|take it away|actually no|actually nah|"
        "no wait|nvm|nevermind|never mind|cancel|cancel it|cancel that|cancel the trade|you keep it|you keep them|"
        "you can keep it|you can keep them|dont want it|dont want them|dont need it|dont need them|no thanks|no ty|"
        "nah its fine|changed my mind|on second thought|forget it|no need for it|no need for them"));
    g_BagsCancelNotPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Bags.CancelNotPhrases",
        "thank you|keep it up|keep it coming|keep them coming|keep em coming|dont take*|do not take*|dont cancel|"
        "do not cancel|never cancel*"));
    g_BagsHintCancel = sConfigMgr->GetOption<std::string>("OllamaChat.Bags.HintCancel",
        "{player_name} changed their mind about {item}, so you have taken it back out and closed the trade window. "
        "Say so in a few words, in your own chat style, no fuss.");

    // --- orders that did nothing -------------------------------------------------------------
    g_OrdersNotDoneVerbs = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Orders.NotDoneVerbs",
        "go|come|spread|kill|farm|grind|loot|sell|repair|mount|dismount|follow|stay|wait|attack|pull|heal|tank|run|"
        "move|use|drink|eat|craft|mine|gather|pick|open|clear|fetch|bring|jump|dance|sit|stand|hurry|fight|stop|"
        "leave|join|invite|buff|summon|accept|equip|wear|drop|destroy|learn|train|fly|ride|swim|hide|stealth|sap|"
        "mark|focus|assist|reset|hearth|unstuck|release"));
    g_OrdersHintNotDone = sConfigMgr->GetOption<std::string>("OllamaChat.Orders.HintNotDone",
        "If {player_name} just told you to do something, you are not doing it: nothing in the game has made you. "
        "So do not say you are on it or that you are doing it; say plainly you cannot from here, or answer it some "
        "other way, in your own chat style.");

    // --- presence --------------------------------------------------------------------------------
    g_PresenceExGroupMinutes = sConfigMgr->GetOption<uint32_t>("OllamaChat.Presence.ExGroupMinutes", 15);
    g_PresenceSkipEmotes.clear();
    {
        std::string unknown;
        for (const std::string& name : ReadPipeList("OllamaChat.Presence.SkipEmotes",
                 "rude|spit|threaten|insult|mock|taunt|chicken|rasp|fart|burp|belch|scoff|shakefist|crack|gloat|"
                 "bored|yawn|glare|scowl|snarl|growl|pinch|slap"))
        {
            if (const uint32_t id = LookupTextEmoteId(name))
                g_PresenceSkipEmotes.insert(id);
            else
                unknown += (unknown.empty() ? "" : ", ") + name;
        }
        if (!unknown.empty())
            LOG_WARN("module.ollamachat", "[Ollama Chat] OllamaChat.Presence.SkipEmotes: not emotes: {}", unknown);
    }

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Chat v10: quest progress in party {}, item and quest descriptions {}, {} premise "
             "template(s) with {} pool(s), echoes {} ({} words, {} bots, {} min), emoticons {}.",
             g_ProgressEnable ? "on" : "off",
             g_LinksDescribeEnable ? "on" : "off", g_PremiseTemplates.size(), g_PremisePools.size(),
             g_EchoEnable ? "on" : "off", g_EchoWords, g_EchoBots, g_EchoWindowMinutes,
             g_EmoticonEnable ? "on" : "off");
}

static void LoadNaturalChatV11Settings()
{
    // --- how a named quest or item sits against the bot's level ------------------------
    g_LinksQuestLevelsAbove = sConfigMgr->GetOption<uint32_t>("OllamaChat.Links.QuestLevelsAbove", 3);
    g_LinksQuietWhenTooLow  = sConfigMgr->GetOption<bool>("OllamaChat.Links.QuietWhenTooLow", true);
    g_LinksQuestTooHighText = sConfigMgr->GetOption<std::string>("OllamaChat.Links.QuestTooHighText",
        "(you are level {bot_level}, far too low for it: you could not do it or help anyone with it, so you would "
        "not offer to)");
    g_LinksQuestEasyText = sConfigMgr->GetOption<std::string>("OllamaChat.Links.QuestEasyText",
        "(you are level {bot_level}, so it would be easy for you)");
    g_LinksItemTooHighText = sConfigMgr->GetOption<std::string>("OllamaChat.Links.ItemTooHighText",
        "(you are level {bot_level}, too low to use it yet)");

    // --- a bot hanging out in a city (mod-playerbots-plus's hangouts) --------------------
    g_HangoutStateText = sConfigMgr->GetOption<std::string>("OllamaChat.Hangout.StateText",
        "Right now you are hanging around {spot}, {doing}, with nowhere in particular to be.");
    g_HangoutAwayText = sConfigMgr->GetOption<std::string>("OllamaChat.Hangout.AwayText",
        "You had stepped away from your keyboard there (AFK) and have only just come back to it.");

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Chat v11: quest level fit {} levels, quiet when too low {}.",
             g_LinksQuestLevelsAbove, g_LinksQuietWhenTooLow ? "on" : "off");
}

static void LoadNaturalChatV13Settings()
{
    auto phrases = [](const char* key, const char* def)
    {
        return CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(key, def));
    };

    // --- the end of a dungeon-clear run -------------------------------------------------
    g_PartyDoneWords = phrases("OllamaChat.Party.DoneWords",
        "clear|cleared|full clear|complete|completed|finished|all bosses|all the bosses|every boss|all done|"
        "run done|dungeon done|were done|we are done|thats it|that is it");
    g_PartyDoneLines = ReadPipeList("OllamaChat.Party.DoneLines",
        "{dungeon} cleared, gg all|That's {dungeon} done, full clear. gg everyone|Full clear of {dungeon}! "
        "nice work all|And that's {dungeon} cleared, every boss down. gg|{dungeon} complete, gg and ty all|"
        "Every boss in {dungeon} is down, that's the clear done. gg");
    g_PartyCheerEnable       = sConfigMgr->GetOption<bool>("OllamaChat.Party.CheerEnable", true);
    g_PartyCheerChance       = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.CheerChance", 85));
    g_PartyCheerCannedChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.CheerCannedChance", 60));
    g_PartyCheerLines = ReadPipeList("OllamaChat.Party.CheerLines",
        "gg|gg all|gg ty|gg everyone|gj all|ty all|thanks all|thx all|good run|nice run|gg wp|ty for the group|"
        "great run|gg thanks all|ty gg");
    g_PartyCheerPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Party.CheerPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in a dungeon group. Your chat style: "
        "{bot_personality} Your group has just cleared {dungeon} - every boss is dead and the run is over - and "
        "{tank} has just said so. Say a quick end-of-run line in party chat, 1 to 6 words, the way players do "
        "before they leave: gg, gj, ty all, good run, thanks for the group, or something like it in your own "
        "words. No quotes, no name in front.");
    g_PartyDoneLeave        = sConfigMgr->GetOption<bool>("OllamaChat.Party.DoneLeave", true);
    g_PartyDoneLeaveSeconds = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.DoneLeaveSeconds", 30), 10, 600);
    g_PartyDoneStayWords = phrases("OllamaChat.Party.DoneStayWords",
        "wait|hang on|hold on|one sec|1 sec|sec|brb|dont leave|dont go|not yet|pls stay|please stay|stay a bit|"
        "stay for|can you stay|stay pls|stay please|help me|need help|help with|help us|can you|could you|"
        "can someone|can anyone|anyone up|anyone want|anyone fancy|quest|one more|another run|another dungeon|"
        "go again|run it again|do it again|run it back|next one|next run|next dungeon");
    g_PartyDoneGoWords = phrases("OllamaChat.Party.DoneGoWords",
        "bye|cya|see ya|see you|you can go|u can go|you can leave|u can leave|ok go|thats all|thats it|im done|"
        "we are done|were done|done now|all good|thanks for the help|ty for the help|thx for the help|stay safe|"
        "take care");
    g_PartyDoneStateText = sConfigMgr->GetOption<std::string>("OllamaChat.Party.DoneStateText",
        "The run is over: every boss in {dungeon} is dead and the group has cleared the dungeon.{plan}");
    g_PartyDoneLeavingText = sConfigMgr->GetOption<std::string>("OllamaChat.Party.DoneLeavingText",
        " Everyone who came in through the dungeon finder is about to leave the group, the way people do after a "
        "run, unless someone asks them to stay.");
    g_PartyDoneStayingText = sConfigMgr->GetOption<std::string>("OllamaChat.Party.DoneStayingText",
        " Someone asked everyone to stick around, so you are all staying in the group for now.");

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Chat v13: end of a run - gg from {}% of the group, leaving after {} s {}.",
             g_PartyCheerEnable ? g_PartyCheerChance : 0, g_PartyDoneLeaveSeconds, g_PartyDoneLeave ? "on" : "off");
}

static void LoadNaturalChatV14Settings()
{
    auto phrases = [](const char* key, const char* def)
    {
        return CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(key, def));
    };
    auto percent = [](const char* key, uint32_t def)
    {
        return std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>(key, def));
    };

    // --- a player giving away or selling something ---------------------------------------
    g_GiftsEnable = sConfigMgr->GetOption<bool>("OllamaChat.Gifts.Enable", true);
    g_GiftsOfferWords = phrases("OllamaChat.Gifts.OfferWords",
        "anyone want|anybody want|any1 want|anyone wants|anybody wants|who wants|who want|whoever wants|anyone need|"
        "anybody need|any1 need|who needs|whoever needs|you want|u want|ya want|you need|u need|want my|want this|"
        "want it|want one|need this|need it|need one|need my|free|for free|freebie|giving away|give away|"
        "giving it away|giving this away|giving them away|up for grabs|anyone use|anybody use|can anyone use|"
        "can anybody use|any use|use this|use it|you can have|u can have|can have it|have it if|take it|take this|"
        "have this|for you|for u|here you go|here u go|dont need|do not need|no longer need|getting rid|get rid");
    g_GiftsSellWords = phrases("OllamaChat.Gifts.SellWords",
        "wts|selling|for sale|buy|buying|buyer|buyers|make an offer|make me an offer|any offers|best offer|or offer|"
        "obo|cheap|bargain|pay|paying");
    g_GiftsGreedyPersonalities = ReadPipeList("OllamaChat.Gifts.GreedyPersonalities", "NAT_GOLDMAKER|NAT_RUDE");
    for (std::string& key : g_GiftsGreedyPersonalities)
        for (char& ch : key)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    g_GiftsSecondVoiceChance = percent("OllamaChat.Gifts.SecondVoiceChance", 35);
    g_GiftsNobodyChance      = percent("OllamaChat.Gifts.NobodyChance", 60);
    g_GiftsFollowUpSeconds   = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Gifts.FollowUpSeconds", 90), 10, 600);
    g_GiftsRememberSeconds   = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Gifts.RememberSeconds", 300), 30, 3600);
    g_GiftsMaxSpendPercent   = percent("OllamaChat.Gifts.MaxSpendPercent", 50);
    g_GiftsHintWant = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintWant",
        "{player_name} is giving away {item} for free to anyone in the group who wants it -- they are offering it, "
        "not asking for one. It would be useful to you: {why}. Say you would like it, in a few words and in your "
        "own way, so they can trade it to you.");
    g_GiftsHintWantToo = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintWantToo",
        "{player_name} is giving away {item} for free, and {first} has already said they would like it. It would "
        "have been useful to you too ({why}); if you say anything, keep it light -- that you would have taken it "
        "too, or that {first} can have it.");
    g_GiftsHintNoNeed = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintNoNeed",
        "{player_name} is giving away {item} for free to anyone who wants it -- they are offering it, not asking "
        "for one. It is no use to you: {why}. If you say anything, just a quick no thanks in your own words.");
    g_GiftsHintNotTradable = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintNotTradable",
        "{player_name} is offering {item}, but it is soulbound, so it cannot be traded to anyone. Say so lightly, "
        "in a few words.");
    g_GiftsHintAskPrice = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintAskPrice",
        "{player_name} is selling {item} but has not said what they want for it. It would be useful to you: {why}. "
        "Ask how much they want for it, in a few words.");
    g_GiftsHintBuy = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintBuy",
        "{player_name} is selling {item} for {price}. It would be useful to you ({why}) and that is a fair price, "
        "so you will buy it: say so in a few words, and that they can trade it to you.");
    g_GiftsHintCounter = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintCounter",
        "{player_name} is selling {item} for {price}. It would be useful to you ({why}), but {price} is more than "
        "you would pay: offer {counter} for it instead, in a few words.");
    g_GiftsHintTooDear = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintTooDear",
        "{player_name} is selling {item} for {price}. It would be useful to you, but you cannot afford that: you "
        "only have {money}. Say so, in a few words.");
    g_GiftsHintNoBuy = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.HintNoBuy",
        "{player_name} is selling {item}. It is no use to you: {why}. If you say anything, a quick no thanks.");
    g_GiftsInviteText = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.InviteText",
        "You are not in their group, and you only trade with your group: ask them to invite you first.");
    g_GiftsSaidGiftText = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.SaidGiftText",
        "You told {player_name} you would like their {item}: it comes over when they put it in a trade window "
        "with you.");
    g_GiftsSaidBuyText = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.SaidBuyText",
        "You agreed to buy {player_name}'s {item} for {price}: when they put it in a trade window with you, you put "
        "the money in.");
    g_GiftsSaidCounterText = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.SaidCounterText",
        "You offered {player_name} {price} for their {item}; if they agree, they can put it in a trade window with "
        "you and you pay that.");
    g_GiftsSaidAskText = sConfigMgr->GetOption<std::string>("OllamaChat.Gifts.SaidAskText",
        "You asked {player_name} how much they want for their {item}.");

    // --- the tank down, a wipe, nobody to rez --------------------------------------------
    g_WipeEnable              = sConfigMgr->GetOption<bool>("OllamaChat.Wipe.Enable", true);
    g_WipeTankDownChance      = percent("OllamaChat.Wipe.TankDownChance", 60);
    g_WipeApologyChance       = percent("OllamaChat.Wipe.ApologyChance", 75);
    g_WipeNoRezChance         = percent("OllamaChat.Wipe.NoRezChance", 90);
    g_WipeRunBackChance       = percent("OllamaChat.Wipe.RunBackChance", 60);
    g_WipeTypedChance         = percent("OllamaChat.Wipe.TypedChance", 35);
    g_WipeRunBackQuietSeconds = std::min<uint32_t>(120, sConfigMgr->GetOption<uint32_t>("OllamaChat.Wipe.RunBackQuietSeconds", 4));
    g_WipeStateMinutes        = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Wipe.StateMinutes", 5), 1, 60);
    g_WipeTankDownPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.TankDownPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in a group in {place}. Your chat style: "
        "{bot_personality} You are in the middle of a fight and {tank}, your tank, has just died. React in party "
        "chat the way players do the moment it starts going wrong: a couple of words like uh oh or oh no, in your "
        "own way. Under 6 words, no name in front, no quotes.");
    g_WipeApologyPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.ApologyPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}, the tank of your group in {place}. Your "
        "chat style: {bot_personality} Your whole group has just died: a wipe, and as the tank it is on you. Say "
        "sorry in party chat in a few words, your own way (sorry all, my bad, that one's on me). No long excuses, "
        "under 10 words, no name in front, no quotes.");
    g_WipeNoRezPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.NoRezPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in a group in {place}. Your chat style: "
        "{bot_personality} Your whole group has just died -- a wipe -- and nobody is left alive who could resurrect "
        "anyone. Say in party chat, in a few words, that it is release and run back for everyone (no rez, gotta "
        "run back, release and run in). Under 12 words, no name in front, no quotes.");
    g_WipeRunBackPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.RunBackPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in a group in {place}. Your chat style: "
        "{bot_personality} You died in that fight, and nobody still alive in your group can resurrect you. Say in "
        "party chat, in a few words, that you will release and run back (no rez, running back). Under 10 words, no "
        "name in front, no quotes.");
    g_WipeTankDownLines = ReadPipeList("OllamaChat.Wipe.TankDownLines",
        "uh oh|oh no|uh oh...|oh no tank|uh oh, tank down|oh dear|oh no no no|this is bad|uh oh guys|welp|tank down!");
    g_WipeApologyLines = ReadPipeList("OllamaChat.Wipe.ApologyLines",
        "sorry guys|my bad|sorry all, my fault|my bad guys|sorry all|oops, my fault|thats on me, sorry|sorry, my bad");
    g_WipeNoRezLines = ReadPipeList("OllamaChat.Wipe.NoRezLines",
        "no one left to res, gotta run back|release and run back i guess|no rez, run back time|guess we're running "
        "back|nobody can res, release and run in|corpse run time|run back time all");
    g_WipeRunBackLines = ReadPipeList("OllamaChat.Wipe.RunBackLines",
        "no rez, running back|running back|no one can res me, running back|ill run back|corpse run, brb|"
        "releasing, brb");
    g_WipeStateWipeText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateWipeText",
        "Your whole group died a moment ago in {place}: a wipe{first}. {rez}");
    g_WipeStateDeadText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateDeadText",
        "In your group, {dead} {is_are} dead{you}. {rez}");
    g_WipeStateTankFirstText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateTankFirstText",
        " -- {tank}, the tank, went down first");
    g_WipeStateTankDeadText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateTankDeadText",
        "{tank}, your tank, is one of them.");
    g_WipeStateSelfResText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateSelfResText",
        "{name} can get back up on their own and bring the rest back.");
    g_WipeStateRezzerText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateRezzerText",
        "{rezzers} can resurrect {them} once the fight is over.");
    g_WipeStateRunBackText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateRunBackText",
        "Nobody is left alive who could resurrect anyone, so everyone has to release their spirit and run back in "
        "from the graveyard.");
    g_WipeStateNoRezzerText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateNoRezzerText",
        "Nobody still alive can resurrect, so the dead have to release and run back.");
    g_WipeStateBackText = sConfigMgr->GetOption<std::string>("OllamaChat.Wipe.StateBackText",
        "{back} {is_are} back up already.");

    // --- gear talk and teasing -----------------------------------------------------------
    g_GearEnable    = sConfigMgr->GetOption<bool>("OllamaChat.Gear.Enable", true);
    g_GearTalkWords = phrases("OllamaChat.Gear.TalkWords",
        "gear|geared|wearing|wear|wears|worn|outfit|outfits|robe|robes|armor|armour|cloth|leather|chainmail|"
        "chain mail|plate|helm|helmet|hat|shoulders|pauldrons|pants|trousers|leggings|boots|gloves|gauntlets|staff|"
        "wand|sword|axe|mace|dagger|shield|weapon|weapons|transmog|ilvl|item level|gearscore|gear score|naked|"
        "dressed|fashion|drip|rags|tailor|look like|looks like");
    g_GearBackOffWords = phrases("OllamaChat.Gear.BackOffWords",
        "rude|rood|harsh|so mean|thats mean|youre mean|you are mean|being mean|how mean|leave me alone|stop it|"
        "stop that|knock it off|bully|bullying|bullies|not nice|uncalled for|no need for that|wow ok|ok wow|"
        "savage|toxic|hurtful");
    g_GearMaxPeople      = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Gear.MaxPeople", 4), 1, 8);
    g_GearBackOffMinutes = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Gear.BackOffMinutes", 5), 1, 60);
    g_GearPromptText = sConfigMgr->GetOption<std::string>("OllamaChat.Gear.PromptText",
        "What the people in this chat are wearing, as anyone can see by looking at them: {gear}. If gear comes up, "
        "go only by this. Never make up what someone's gear looks like or is made of -- no wood, trees, rags or "
        "sacks -- and never invent gear they are not wearing. A light tease about something real is fine once, "
        "but do not pile on: if someone has already teased them about it, let it go.");
    g_GearBackOffText = sConfigMgr->GetOption<std::string>("OllamaChat.Gear.BackOffText",
        "{player_name} has said they feel picked on: do not tease them any more. A quick sorry, something kind, or "
        "just talk about something else.");

    // --- whispered asides in a group -----------------------------------------------------
    g_AsideEnable = sConfigMgr->GetOption<bool>("OllamaChat.Aside.Enable", true);
    g_AsidePersonalities.clear();
    for (const std::string& item : ReadPipeList("OllamaChat.Aside.Personalities",
             "NAT_GOSSIP:100|NAT_BUTTERFLY:50|NAT_BANTER:40|NAT_DEADPAN:30|NAT_JOKER:30|NAT_RUDE:30|NAT_GRUMBLER:25|"
             "NAT_RAIDER:25|NAT_SOCIAL:25|NAT_TROLL:20|NAT_CONTRARIAN:20|NAT_MINMAXER:20|NAT_PETLOVER:20|"
             "NAT_PRANKSTER:20|NAT_KEYBOARD:20|NAT_HYPE:15"))
    {
        const size_t colon = item.find(':');
        std::string key = item.substr(0, colon);
        for (char& ch : key)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const uint32_t weight = colon == std::string::npos ? 100
                              : uint32_t(std::clamp(std::atoi(item.c_str() + colon + 1), 0, 100));
        if (!key.empty() && weight)
            g_AsidePersonalities[key] = weight;
    }
    g_AsideLineChance    = percent("OllamaChat.Aside.LineChance", 10);
    g_AsideIdleChance    = percent("OllamaChat.Aside.IdleChance", 4);
    g_AsideCheckSeconds  = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Aside.CheckSeconds", 60), 10, 3600);
    g_AsidePlayerMinutes = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Aside.PlayerMinutes", 20), 1, 1440);
    g_AsideBotMinutes    = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Aside.BotMinutes", 45), 1, 1440);
    g_AsideMinWords      = std::min<uint32_t>(20, sConfigMgr->GetOption<uint32_t>("OllamaChat.Aside.MinWords", 4));
    g_AsidePrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.Prompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}. Your chat style: {bot_personality} You are "
        "in a group with {player_name} and with {subject}. Whisper {player_name} privately: a quick aside about "
        "{subject}, the cheeky, gossipy or slightly catty kind of remark you would only make in a whisper, to get "
        "a laugh out of them. Base it on this, and nothing else: {fact}. Keep it playful, never "
        "cruel, nothing about anyone's real life, and make up nothing about {subject} beyond that. Do not say you "
        "are whispering. One short line, under 20 words, no name in front, no quotes.");
    g_AsideHistoryNote = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.HistoryNote",
        "(nothing yet: you whispered them first)");
    g_AsideFactSaid = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.FactSaid",
        "{subject} just said in party chat: '{line}'");
    g_AsideFactClass = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.FactClass",
        "{subject} is a level {level} {race} {role}{duty}");
    g_AsideFactLevel = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.FactLevel",
        "{subject} is level {level}, {gap} levels {dir} the rest of the group");
    g_AsideFactGear = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.FactGear",
        "{subject} is wearing {item}");
    g_AsideFactPet = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.FactPet",
        "{subject}'s pet is a {family} called {pet}");
    g_AsideFactDeaths = sConfigMgr->GetOption<std::string>("OllamaChat.Aside.FactDeaths",
        "{subject} has died {deaths} times since joining the group");

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Chat v14: gifts and sales {}; wipes {} (uh oh {}%, sorry {}%, run back {}%); gear talk {}; "
             "asides {} ({} styles, one per player per {} min).",
             g_GiftsEnable ? "on" : "off", g_WipeEnable ? "on" : "off", g_WipeTankDownChance, g_WipeApologyChance,
             g_WipeNoRezChance, g_GearEnable ? "on" : "off", g_AsideEnable ? "on" : "off",
             g_AsidePersonalities.size(), g_AsidePlayerMinutes);
}

static void LoadNaturalChatV15Settings()
{
    auto phrases = [](const char* key, const char* def)
    {
        return CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(key, def));
    };
    auto text = [](const char* key, const char* def)
    {
        return sConfigMgr->GetOption<std::string>(key, def);
    };

    // --- where things are, and how to get them --------------------------------------------
    g_KnowledgeEnable = sConfigMgr->GetOption<bool>("OllamaChat.Knowledge.Enable", true);
    g_KnowledgeQuestionWords = phrases("OllamaChat.Knowledge.QuestionWords",
        "where|wheres|whereabouts|who sells|what sells|anyone sell|anybody sell|anyone selling|anybody selling|"
        "who has|how do i get|how do you get|how to get|how can i get|how would i get|how do i find|how to find|"
        "any idea where|know where|cant find|cannot find|can not find");
    g_KnowledgeNotAskingWords = phrases("OllamaChat.Knowledge.NotAskingWords",
        "wts|sell it|sell this|sell my|sell these|sell them|sell those|to sell|can i sell|do i sell|im selling|"
        "i am selling|i sell");
    g_KnowledgeTrainerWords = phrases("OllamaChat.Knowledge.TrainerWords",
        "trainer|trainers|train|training|learn|learning|teach|teacher|teaches|teaching|spells|skill|skills");
    g_KnowledgeSynonyms.clear();
    for (const std::string& entry : ReadPipeList("OllamaChat.Knowledge.Synonyms",
             "ah,auctioneer,auctioneers=auction house|banker,bankers,vault=bank|"
             "fp,flight path,flight paths,flightmaster,flight master,flight masters=flight master gryphon wind rider "
             "bat handler hippogryph dragonhawk|innkeeper,innkeepers,hearth,tavern=inn|mail,mailbox,mailboxes=mailbox|"
             "tram,subway=deeprun tram|boat,boats,ship,ships,docks,harbour=harbor|"
             "zep,zepp,zeppelin,zeppelins=zeppelin master|"
             "guild charter,charter,tabard,tabards,make a guild,create a guild,start a guild=guild master|"
             "lockpicking,lockpick=locksmith|stable,stables,pet stable,stablemaster=stable master|"
             "weapon master,weapon trainer,weapon skill,weapon skills,weapons master=weapons trainer|"
             "battle master,battlemasters,bg,bgs,battleground,battlegrounds=battlemaster|"
             "haircut,hair,barbershop,barber shop=barber|pally,pallies=paladin|lock,locks=warlock|sham,shammy=shaman|"
             "dk,dks=death knight|lw,leather working=leatherworking|bs,black smithing,blacksmith=blacksmithing|"
             "jc,jewel crafting,jewelcrafter=jewelcrafting|ench,enchanter,enchant=enchanting|"
             "engi,eng,engy,engineer=engineering|alch,alchemist=alchemy|insc,scribe=inscription|"
             "herb,herbing,herbalist=herbalism|skin,skinner=skinning|tailor=tailoring|cook,chef=cooking|"
             "fish,fisher,fisherman=fishing|miner=mining|firstaid=first aid|sw=stormwind|org,orgri=orgrimmar|"
             "uc=undercity|tb=thunder bluff|darna=darnassus|exo=exodar|smc=silvermoon|shatt=shattrath|dal,dala=dalaran"))
    {
        const size_t eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        std::string triggers = entry.substr(0, eq);
        std::replace(triggers.begin(), triggers.end(), ',', '|');
        KnowledgeSynonym s;
        s.triggers = CrowdText::ParsePhrases(triggers);
        s.adds     = CrowdText::Words(entry.substr(eq + 1));
        if (!s.triggers.empty() && !s.adds.empty())
            g_KnowledgeSynonyms.push_back(std::move(s));
    }
    g_KnowledgeGuardRange    = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Knowledge.GuardRange", 500.0f), 0.0f, 5000.0f);
    g_KnowledgeMaxGuardChars = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Knowledge.MaxGuardChars", 240), 60, 1000);
    g_KnowledgeManyZones     = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Knowledge.ManyZones", 8), 2, 100);
    g_KnowledgeFollowSeconds = std::min<uint32_t>(600, sConfigMgr->GetOption<uint32_t>("OllamaChat.Knowledge.FollowSeconds", 120));
    g_KnowledgeWrongOnPurpose = ReadPipeList("OllamaChat.Knowledge.WrongOnPurpose", "NAT_KEYBOARD");
    for (std::string& key : g_KnowledgeWrongOnPurpose)
        for (char& ch : key)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    g_KnowledgeItemText = text("OllamaChat.Knowledge.ItemText", "How to get {item}, as it is in the game:");
    g_KnowledgeSpreadManyText = text("OllamaChat.Knowledge.SpreadManyText",
        "vendors all over sell it, in nearly every city and in many towns");
    g_KnowledgeSpreadSomeText = text("OllamaChat.Knowledge.SpreadSomeText", "vendors in {zones} sell it");
    g_KnowledgeSpreadOneText  = text("OllamaChat.Knowledge.SpreadOneText", "only {vendor}, in {place}, sells it");
    g_KnowledgeKindText       = text("OllamaChat.Knowledge.KindText", "mostly {kinds} vendors");
    g_KnowledgeTokensText     = text("OllamaChat.Knowledge.TokensText", "It costs tokens or honor rather than gold.");
    g_KnowledgeNearestText    = text("OllamaChat.Knowledge.NearestText",
        "The nearest one to {asker} is {vendor}, in {place}.");
    g_KnowledgeCraftText      = text("OllamaChat.Knowledge.CraftText",
        "No vendor sells it, but players with {professions} can make it.");
    g_KnowledgeCraftAlsoText  = text("OllamaChat.Knowledge.CraftAlsoText", "Players with {professions} can make it too.");
    g_KnowledgeNoVendorText   = text("OllamaChat.Knowledge.NoVendorText",
        "No vendor sells it and no profession makes it: it is looted, gathered or earned from a quest.");
    g_KnowledgeEnemyOnlyText  = text("OllamaChat.Knowledge.EnemyOnlyText",
        "Only the other faction's vendors sell it.");
    g_KnowledgeTradeText      = text("OllamaChat.Knowledge.TradeText",
        "Players can trade it, so the auction house might have some.");
    g_KnowledgeBoundText      = text("OllamaChat.Knowledge.BoundText",
        "It binds when picked up, so nobody can trade it to anyone.");
    g_KnowledgePlaceText = text("OllamaChat.Knowledge.PlaceText",
        "Where {place} is in {town}, as the guards there tell anyone who asks them for directions: '{guard}'{marked} "
        "Just telling them to ask a guard is a fine answer too.");
    g_KnowledgeMarkText = text("OllamaChat.Knowledge.MarkText", "A guard also marks it on the minimap ({mark}).");
    g_KnowledgeHeadingText = text("OllamaChat.Knowledge.HeadingText",
        "In {town}, a guard asked for directions to the {heading} offers {choices}, and marks the one picked on the "
        "minimap -- so: ask a guard.");
    g_KnowledgeEarlierText = text("OllamaChat.Knowledge.EarlierText", "{asker} asked a moment ago: '{question}'.");
    g_KnowledgeFollowText  = text("OllamaChat.Knowledge.FollowText",
        "If anyone in the chat said otherwise, they got it wrong.");
    g_KnowledgeAnswerText = text("OllamaChat.Knowledge.AnswerText",
        "That is how it is in the game: if you answer, answer from it and add nothing it does not say -- in a few "
        "words or a bit more, as your style goes.");
    g_KnowledgeWrongText = text("OllamaChat.Knowledge.WrongText",
        "That is the right answer and you know it -- but, as you do, give a wrong one with a straight face, "
        "to wind them up: another real place, vendor or way from the game, never one you made up. If someone "
        "corrects you, double down.");

    // --- links: not the same one over and over --------------------------------------------
    g_LinksRepeatSeconds = std::min<uint32_t>(3600, sConfigMgr->GetOption<uint32_t>("OllamaChat.Links.RepeatSeconds", 180));
    g_LinksQuestDoneText = text("OllamaChat.Links.QuestDoneText",
        "(you have it in your quest log and it is done: it only needs handing in{ender})");
    g_LinksQuestOnText = text("OllamaChat.Links.QuestOnText",
        "(you have it in your quest log, not done yet{progress})");
    g_LinksQuestRewardedText = text("OllamaChat.Links.QuestRewardedText", "(you did it already)");
    g_LinksQuestNoneText = text("OllamaChat.Links.QuestNoneText", "(you do not have it in your quest log)");
    // --- what day and time it is --------------------------------------------------------
    g_ClockFixWeekdays = sConfigMgr->GetOption<bool>("OllamaChat.Clock.FixWeekdays", true);
    g_ClockWords = phrases("OllamaChat.Clock.Words",
        "today|tonight|tomorrow|yesterday|weekend|weekday|this week|monday*|tuesday*|wednesday*|thursday*|friday*|"
        "saturday*|sunday*|what day|which day|what time|time is it|this morning|this afternoon|this evening|"
        "morning|afternoon|evening|midnight|bedtime|so late|so early|oclock|o clock|work tomorrow|school night");
    g_ClockPromptText = text("OllamaChat.Clock.PromptText",
        "For everyone here it is {now} right now ({time} server time).");

    // --- loot rolls ----------------------------------------------------------------------
    g_RollsEnable  = sConfigMgr->GetOption<bool>("OllamaChat.Rolls.Enable", true);
    g_RollsLowRoll = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Rolls.LowRoll", 2));
    g_RollsHighRoll = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Rolls.HighRoll", 99), 1, 101);
    g_RollsReactChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Rolls.ReactChance", 50));
    g_RollsReactCooldownSeconds = std::min<uint32_t>(3600, sConfigMgr->GetOption<uint32_t>("OllamaChat.Rolls.ReactCooldownSeconds", 60));
    g_RollsNoteText = text("OllamaChat.Rolls.NoteText",
        "loot roll for {item} (1 to 100, highest wins; need beats greed): {rolls} -- {winner} won it.");
    g_RollsLowPrompt = text("OllamaChat.Rolls.LowPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}. Your chat style: {bot_personality} In your "
        "group's loot roll for {item}, {player_name} just rolled {roll} out of 100 -- about as unlucky as a roll "
        "gets{outcome}. Say something about it in party chat the way players do: a quick tease, or feel their "
        "pain. Under 10 words, no name in front, no quotes.");
    g_RollsHighPrompt = text("OllamaChat.Rolls.HighPrompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}. Your chat style: {bot_personality} In your "
        "group's loot roll for {item}, {player_name} just rolled {roll} out of 100 -- about as lucky as a roll "
        "gets{outcome}. React in party chat the way players do: gz, lucky, nice roll, or a joke about it. Under "
        "10 words, no name in front, no quotes.");

    g_StateOwnWayText = text("OllamaChat.State.OwnWay",
        "Right now you are off doing your own thing -- questing and grinding -- because {player_name} told you to, "
        "instead of following them. You are still in their group, and they may be tagging along.");

    // --- a guild message of the day nobody set ---------------------------------------------
    g_GuildEnvCommentGuildMOTDUnset = ReadPipeList("OllamaChat.GuildEnvCommentGuildMOTDUnset",
        "the guild message of the day (the note at the top of the guild window): nobody has ever set one, so it "
        "still shows the game's default text, No message set. Call it the gmotd or the guild message of the day, "
        "so everyone knows what you mean, and say nobody has set one rather than repeating the default text.");
    g_GuildEventTypeGuildMOTD = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeGuildMOTD",
        "set the guild message of the day to:");
    g_GuildEventTypeGuildMOTD_Chance = std::clamp(sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeGuildMOTD_Chance", 100), 0, 100);

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Chat v15: knowledge {} ({} synonyms, wrong on purpose: {}); links not repeated for {} s.",
             g_KnowledgeEnable ? "on" : "off", g_KnowledgeSynonyms.size(), g_KnowledgeWrongOnPurpose.size(),
             g_LinksRepeatSeconds);
}

// Chat v16: bots mingling in /say (mod-ollama-chat_mingle).
// A bot putting on a rare or epic it just got, and saying so in party.
static void LoadNaturalChatV20Settings()
{
    g_EquipEnable     = sConfigMgr->GetOption<bool>("OllamaChat.Equip.Enable", true);
    g_EquipMinQuality = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Equip.MinQuality", 3), 2, 5);
    g_EquipChance     = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Equip.Chance", 20));
    g_EquipPersonalities.clear();
    for (const std::string& item : ReadPipeList("OllamaChat.Equip.Personalities",
             "NAT_HYPE:90|NAT_EXCITABLE:90|NAT_BUTTERFLY:70|NAT_NEWBIE:70|NAT_SOCIAL:60|NAT_FRIENDLY:50|"
             "NAT_MINMAXER:50|NAT_RAIDER:50|NAT_JOKER:45|NAT_BANTER:45|NAT_GOLDMAKER:40|NAT_PRANKSTER:40|"
             "NAT_REACTOR:40|NAT_PVPER:40|NAT_TROLL:30|NAT_KEYBOARD:30|NAT_GRUMBLER:20|NAT_CONTRARIAN:20|"
             "NAT_LOWERCASE:20|NAT_ABBREVIATOR:20|NAT_ROLEPLAYER:20|NAT_NOSTALGIC:20|NAT_CHILL:15|NAT_DEADPAN:10|"
             "NAT_PROPER:10|NAT_HELPFUL_VET:10|NAT_TERSE_VET:5|NAT_QUIET:0|NAT_LONER:0"))
    {
        const size_t colon = item.find(':');
        std::string key = item.substr(0, colon);
        for (char& ch : key)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const uint32_t chance = colon == std::string::npos ? 100
                              : uint32_t(std::clamp(std::atoi(item.c_str() + colon + 1), 0, 100));
        if (!key.empty())
            g_EquipPersonalities[key] = chance;
    }
    g_EquipBotCooldownMinutes   = sConfigMgr->GetOption<uint32_t>("OllamaChat.Equip.BotCooldownMinutes", 20);
    g_EquipGroupCooldownSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Equip.GroupCooldownSeconds", 90);
    g_EquipLootedMinutes = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Equip.LootedMinutes", 10),
                                                1, 120);
    g_EquipPrompt = sConfigMgr->GetOption<std::string>("OllamaChat.Equip.Prompt",
        "You are {bot_name}, a level {bot_level} {bot_race} {bot_class} in a group. Your chat style: "
        "{bot_personality} You just got {item}, {what}, and you are putting it on right now. Say so in party chat "
        "in your own way, the way players do with a nice bit of new gear: chuffed, excited, a quick finally, or "
        "showing it off, whatever suits you. Name it as {item}, once. One short line, 3 to 12 words, no quotes, no "
        "name in front.");

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v20: new gear said in party {} ({} and up, {}% for a style "
             "not listed, {} styles listed).", g_EquipEnable ? "on" : "off",
             g_EquipMinQuality >= 4 ? "epics" : "rares", g_EquipChance, g_EquipPersonalities.size());
}

static void LoadNaturalChatV21Settings()
{
    // --- emotes in the transcript ------------------------------------------------------
    g_EmoteEventText = sConfigMgr->GetOption<std::string>("OllamaChat.Emote.EventText",
        "{actor} gave {target} a /{emote}");

    // --- a rest that ends by itself ----------------------------------------------------
    g_PartyCmdHintBreak = sConfigMgr->GetOption<std::string>("OllamaChat.PartyCommands.HintBreak",
        "{player_name} asked for {reason}, so you have stopped the run for it. Just say ok, in one to four "
        "words and your own chat style, the easy way a tank does when someone needs to drink. You will see "
        "for yourself when everyone is ready, so do not ask them to tell you.{recent_acks}");
    g_PartyBreakEndEnable = sConfigMgr->GetOption<bool>("OllamaChat.Party.BreakEndEnable", true);
    g_PartyBreakEndHealthPct =
        std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.BreakEndHealthPct", 90));
    g_PartyBreakEndManaPct =
        std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.BreakEndManaPct", 95));
    g_PartyBreakEndOthersManaPct =
        std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.BreakEndOthersManaPct", 80));
    g_PartyBreakEndMinSeconds =
        std::min<uint32_t>(600, sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.BreakEndMinSeconds", 10));
    g_PartyBreakEndMaxMinutes =
        std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Party.BreakEndMaxMinutes", 5), 1, 60);
    g_PartyBreakEndSituation = sConfigMgr->GetOption<std::string>("OllamaChat.Party.BreakEndSituation",
        "Everyone has had their break and has their health and mana back, so you are getting the run going "
        "again, right now. {~Tell them you are off|Get them moving|Tell them you are carrying on|Call them back "
        "to it}, in a few words of your own: hurry them along, tease them for being slow, keep it short and "
        "bossy, or just say you are going, whatever suits your chat style.");
    g_PartyBreakEndWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Party.BreakEndWords",
        "go|gogo|move|moving|carry on|carrying on|keep up|keep going|onward|onwards|continue|continuing|pull|"
        "pulling|next|back to it|lets roll|rolling|push on|pushing on|follow|resume|resuming|off we|on we"));
    g_PartyBreakEndLines = ReadPipeList("OllamaChat.Party.BreakEndLines",
        "ok lets go|right, moving on|alright, lets go|ok, pulling|right, back to it|ok everyone good, lets carry on|"
        "lets keep going");

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v21: a rest ends by itself {} (health {}%, mana {}% for "
             "players and healers, {}% for other bots), emotes in the transcript {}.",
             g_PartyBreakEndEnable ? "on" : "off", g_PartyBreakEndHealthPct, g_PartyBreakEndManaPct,
             g_PartyBreakEndOthersManaPct, g_EmoteEventText.empty() ? "off" : "on");
}

static void LoadNaturalChatV25Settings()
{
    // --- nothing made up from the game ---------------------------------------------------
    g_OutlookFactsText = sConfigMgr->GetOption<std::string>("OllamaChat.Outlook.FactsText",
        "Anything from the game you name -- an item, quest, NPC, creature, boss, dungeon, zone, mount, pet, "
        "spell or drop -- must really be in Wrath of the Lich King, called what it is really called. Never "
        "make one up, and never state a fact about one (where it drops, what it does, who can use it, or that "
        "it is not in the game) unless you are sure of it: when you do not know or cannot remember, say so, "
        "or ask. Made-up fun is fine when it is plainly made up, like imagine if murlocs were ten feet tall.");

    // --- canned lines in each personality's own way ------------------------------------
    g_PersonalityCannedStyles.clear();
    {
        std::stringstream ss(sConfigMgr->GetOption<std::string>("OllamaChat.Personality.CannedStyles",
            "NAT_PROPER:proper|NAT_ROLEPLAYER:proper|NAT_HELPFUL_VET:proper|NAT_LOWERCASE:lower|NAT_CHILL:lower|"
            "NAT_TERSE_VET:flat|NAT_QUIET:flat|NAT_DEADPAN:flat|NAT_LONER:flat|NAT_GRUMBLER:flat|NAT_KEYBOARD:flat|"
            "NAT_ABBREVIATOR:short|NAT_MINMAXER:short|NAT_EXCITABLE:excited|NAT_HYPE:excited|NAT_BUTTERFLY:excited|"
            "NAT_NEWBIE:excited"));
        std::string entry;
        while (std::getline(ss, entry, '|'))
        {
            const size_t colon = entry.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = entry.substr(0, colon), style = entry.substr(colon + 1);
            auto trim = [](std::string& t)
            {
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
                    t.erase(t.begin());
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
                    t.pop_back();
            };
            trim(key);
            trim(style);
            if (!key.empty() && !style.empty())
                g_PersonalityCannedStyles[key] = style;
        }
    }

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v25: nothing made up from the game ({}); canned lines "
             "in {} personalities' own way.", g_OutlookFactsText.empty() ? "off" : "on",
             g_PersonalityCannedStyles.size());
}

static void LoadNaturalChatV26Settings()
{
    // --- "need a wand": where one really comes from ------------------------------------
    g_WantsEnable = sConfigMgr->GetOption<bool>("OllamaChat.Wants.Enable", true);
    g_WantsWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Wants.Words",
        "need|needs|needed|needing|neeed|want|wants|wanna|looking for|lookin for|looking 4|lf|"
        "hunting for|after|could use|could do with|where|wish i had"));
    g_WantsFollowSeconds = std::min<uint32_t>(600, sConfigMgr->GetOption<uint32_t>("OllamaChat.Wants.FollowSeconds", 120));
    g_WantsText = sConfigMgr->GetOption<std::string>("OllamaChat.Wants.Text",
        "{speaker} is after {kind}. What is really so about getting one, for a level {level} {class}: {facts} {self} "
        "Answer in your own way and style, from that: the boss here that can drop one, one of those dungeons, a "
        "crafter, the auction house, hoping one drops for them, needing one too, or just a word of sympathy. Never "
        "send them anywhere, or tell them to kill or buy anything, that is not in this.");
    g_WantsFollowText = sConfigMgr->GetOption<std::string>("OllamaChat.Wants.FollowText",
        "A moment ago {speaker} said they are after {kind}. What is really so about getting one, for a level {level} "
        "{class}: {facts} {self} If what you are answering is about that, answer from this, in your own way, and "
        "never send them anywhere, or tell them to kill or buy anything, that is not in it; if it is about something "
        "else, leave this be.");

    // --- duel talk that ends in a duel ---------------------------------------------------
    g_MingleDuelChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Mingle.DuelChance", 25));

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v26: where wanted gear comes from {} ({} want words, "
             "{}s follow-ups); duel talk {}% of the time when two could go and duel.",
             g_WantsEnable ? "on" : "off", g_WantsWords.size(), g_WantsFollowSeconds, g_MingleDuelChance);
}

static void LoadNaturalChatV27Settings()
{
    // --- which side a bot plays, and its own cities ------------------------------------
    g_OutlookSideText = sConfigMgr->GetOption<std::string>("OllamaChat.Outlook.SideText",
        "You play {side}: your own side's cities are {cities}.");
    g_OutlookHomeCityText = sConfigMgr->GetOption<std::string>("OllamaChat.Outlook.HomeCityText",
        "You are in {city}, one of your side's own cities: you are safe here, and its guards are on your side.");
    g_OutlookCitiesHorde = ReadPipeList("OllamaChat.Outlook.Cities.Horde",
        "Orgrimmar|Thunder Bluff|the Undercity|Silvermoon City");
    g_OutlookCitiesAlliance = ReadPipeList("OllamaChat.Outlook.Cities.Alliance",
        "Stormwind|Ironforge|Darnassus|the Exodar");
    g_MingleSideText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.SideText",
        "They all play {side}: their own side's cities are {cities}.");
    g_MingleHomeCityText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.HomeCityText",
        "{city} is one of those, so they are safe here, and its guards are on their side.");

    // --- party chat that keeps the thread ------------------------------------------------
    g_ContextAmbientHoldSeconds = std::min<uint32_t>(600,
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.AmbientHoldSeconds", 60));
    g_ContextLeadUpText = sConfigMgr->GetOption<std::string>("OllamaChat.Context.LeadUpText",
        "{player_name} typed that right after {lead_up}, so it is most likely about that.");
    g_ContextLeadUpMaxWords = sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.LeadUpMaxWords", 8);

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v27: bots told their side {} ({} Horde and {} Alliance "
             "cities), home in their own city {}; unprompted lines held {}s after a real player speaks; short lines "
             "tied to what they follow {} (up to {} words).",
             g_OutlookSideText.empty() ? "off" : "on", g_OutlookCitiesHorde.size(), g_OutlookCitiesAlliance.size(),
             g_OutlookHomeCityText.empty() ? "off" : "on", g_ContextAmbientHoldSeconds,
             g_ContextLeadUpText.empty() ? "off" : "on", g_ContextLeadUpMaxWords);
}

// What each race says in the game: its NPCs' greetings and farewells, and its
// own language's best-known phrases, from the game and its lore.
static const char* const kRacePhrasesHuman =
    "well met=a greeting human and dwarf NPCs give you|light bless you=a human NPC's farewell";
static const char* const kRacePhrasesOrc =
    "zug zug, zug-zug, zugzug=an orc's okay, the greeting orc NPCs give you, and what peons said in the old "
    "Warcraft games|lok'tar, lok-tar, loktar=orcish for victory, a war cry and an orc NPC's greeting|"
    "aka'magosh, akamagosh=orcish for a blessing on you and yours|dabu=orcish for I obey, what orc grunts said in "
    "the old Warcraft games|swobu=orcish for as you command, what orc peons and grunts said in the old Warcraft "
    "games|work work=what orc peons said in Warcraft III|go with honor, go with honour=an orc NPC's farewell";
static const char* const kRacePhrasesDwarf =
    "well met=a greeting human and dwarf NPCs give you|off with ye=a dwarf NPC's farewell";
static const char* const kRacePhrasesNightElf =
    "ishnu-alah, ishnu alah, ishnualah=darnassian for good fortune to you, a greeting|elune-adore, elune adore="
    "darnassian for Elune be with you, a greeting|ande'thoras-ethil, andethoras ethil=darnassian for may your "
    "troubles be diminished, a farewell|shan'do=darnassian for honoured teacher|shaha lor'ma=darnassian for thank "
    "you|elune be with you=a night elf NPC's greeting";
static const char* const kRacePhrasesUndead =
    "dark lady watch over you=a Forsaken NPC's farewell, the Dark Lady being Sylvanas";
static const char* const kRacePhrasesTauren =
    "walk with the earth mother=a tauren NPC's farewell|ish-ne-alo por-ah, ishne alo porah, ishnealo porah="
    "taur-ahe for may the days ahead be guided by the elders of long ago, Cairne Bloodhoof's greeting|moo=the cow "
    "joke every tauren hears";
static const char* const kRacePhrasesGnome =
    "salutations=a gnome NPC's greeting|for gnomeregan=the gnomes' battle cry";
static const char* const kRacePhrasesTroll =
    "hello mon, hey mon, hi mon, greetings mon, ya mon, yah mon, yeh mon, yeah mon=how trolls talk in the game, "
    "in their Caribbean-sounding accent: troll NPCs greet you with hello mon|stay away from the voodoo, stay away "
    "from da voodoo, stay away from de voodoo=a troll NPC's farewell|see ya layta=a troll NPC's farewell";
static const char* const kRacePhrasesBloodElf =
    "bal'a dash malanore, bal'a dash, bala dash=thalassian for greetings, traveller|anaria shola=thalassian for "
    "speak your business, a blood elf NPC's greeting|shorel'aran, shorelaran=thalassian for farewell|selama "
    "ashal'anore=thalassian for justice for our people|sinu a'manore=thalassian for well met|anu belore dela'na="
    "thalassian for the sun guides us|al diel shala=thalassian for safe travels";
static const char* const kRacePhrasesDraenei =
    "archenon poros, arkanon poros=draenei for good fortune|blessings upon you=a draenei NPC's greeting|dioniss aca="
    "draenei for safe journey";

static void LoadNaturalChatV28Settings()
{
    // --- battle cries --------------------------------------------------------------------
    g_SidesCriesHorde = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Sides.Cries.Horde",
        "for the horde|lok'tar ogar|loktar ogar|lok-tar ogar"));
    g_SidesCriesAlliance = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Sides.Cries.Alliance", "for the alliance"));
    g_SidesOwnCryText = sConfigMgr->GetOption<std::string>("OllamaChat.Sides.OwnCryText",
        "{player_name} just shouted {the_side}'s battle cry ('{cry}'), and you are {side} too: shout it back, cheer "
        "or join in, in your own style.");
    g_SidesEnemyCryText = sConfigMgr->GetOption<std::string>("OllamaChat.Sides.EnemyCryText",
        "{player_name} just shouted the other side's battle cry ('{cry}') among {the_side}: give them some stick for "
        "it, all in fun -- boo, call them a traitor or a spy, or ask whose side they are on -- in your own style.");

    // --- what each race says ---------------------------------------------------------------
    g_RacePhrasesRaw.clear();
    const std::pair<uint8_t, std::pair<const char*, const char*>> races[] = {
        { RACE_HUMAN,         { "OllamaChat.Race.Phrases.Human",    kRacePhrasesHuman } },
        { RACE_ORC,           { "OllamaChat.Race.Phrases.Orc",      kRacePhrasesOrc } },
        { RACE_DWARF,         { "OllamaChat.Race.Phrases.Dwarf",    kRacePhrasesDwarf } },
        { RACE_NIGHTELF,      { "OllamaChat.Race.Phrases.NightElf", kRacePhrasesNightElf } },
        { RACE_UNDEAD_PLAYER, { "OllamaChat.Race.Phrases.Undead",   kRacePhrasesUndead } },
        { RACE_TAUREN,        { "OllamaChat.Race.Phrases.Tauren",   kRacePhrasesTauren } },
        { RACE_GNOME,         { "OllamaChat.Race.Phrases.Gnome",    kRacePhrasesGnome } },
        { RACE_TROLL,         { "OllamaChat.Race.Phrases.Troll",    kRacePhrasesTroll } },
        { RACE_BLOODELF,      { "OllamaChat.Race.Phrases.BloodElf", kRacePhrasesBloodElf } },
        { RACE_DRAENEI,       { "OllamaChat.Race.Phrases.Draenei",  kRacePhrasesDraenei } },
    };
    for (const auto& [race, keyAndDefault] : races)
        g_RacePhrasesRaw.emplace_back(race, sConfigMgr->GetOption<std::string>(keyAndDefault.first,
                                                                                keyAndDefault.second));
    g_RaceHeardText = sConfigMgr->GetOption<std::string>("OllamaChat.Race.HeardText",
        "{player_name} said '{phrase}': {meaning}. Players say these for fun, so play along if it suits your "
        "style.{same}");
    g_RaceHeardSameText = sConfigMgr->GetOption<std::string>("OllamaChat.Race.HeardSameText",
        "You are {a_race} too: saying it back, or something else {race_plural} say, is part of the joke.");
    g_RaceUseText = sConfigMgr->GetOption<std::string>("OllamaChat.Race.UseText",
        "You are {a_race}. If it fits this line and your style, you could slip in something {race_plural} say in "
        "the game, just for fun: {phrases}.");
    g_RaceUseChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Race.UseChance", 3));
    g_RaceStyleChance.clear();
    for (const std::string& pair : ReadSpaceList("OllamaChat.Race.StyleChance",
             "NAT_ROLEPLAYER:25 NAT_LOREBUFF:10 NAT_JOKER:6 NAT_BANTER:6 NAT_PRANKSTER:6 NAT_HYPE:5"))
    {
        const size_t colon = pair.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= pair.size())
            continue;
        std::string key = pair.substr(0, colon);
        for (char& c : key)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const std::string num = pair.substr(colon + 1);
        if (std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) && num.size() <= 3)
            g_RaceStyleChance[key] = std::min<uint32_t>(100, uint32_t(std::stoul(num)));
    }
    g_MingleRaceText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.RaceText",
        "{name} is {a_race}: if it fits, they could slip in something {race_plural} say in the game, just for fun: "
        "{phrases}.");
    Race_Load();

    // --- banter that bites back -------------------------------------------------------------
    g_BanterWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Banter.Words",
        "noob|noobs|nub|newb|n00b|scrub|clown|git gud|get good|l2p|learn to play|ur mum|your mum|ur mom|your mom|"
        "yo mama|ur nan|your nan|you suck|u suck|ur bad|youre bad|you are bad|tryhard|try hard|cringe"));
    g_BanterText = sConfigMgr->GetOption<std::string>("OllamaChat.Banter.Text",
        "{player_name} is having a go ('{tease}'): that is banter, not a real complaint, and players here give as "
        "good as they get. If it was at you, fire one back in your own style -- {~a cheeky comeback about something "
        "real|mock outrage|turning it right back on them|a your mum joke|a daft threat to duel them over it|calling "
        "them a noob right back} -- and if it was at someone else, you can pile in on either side. Do not take it "
        "seriously, agree with it or apologise.");

    const RaceStats race = Race_GetStats();
    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v28: battle cries {} ({} Horde, {} Alliance); {} race phrases "
             "known, said back by their own race, slipped in on {}% of lines ({} styles more); banter given back {} "
             "({} tease words).",
             (g_SidesOwnCryText.empty() && g_SidesEnemyCryText.empty()) ? "off" : "on", g_SidesCriesHorde.size(),
             g_SidesCriesAlliance.size(), race.phrases, g_RaceUseText.empty() ? 0 : g_RaceUseChance,
             g_RaceStyleChance.size(), g_BanterText.empty() ? "off" : "on", g_BanterWords.size());
}

static void LoadNaturalChatV24Settings()
{
    // --- a mount is not a pet -----------------------------------------------------------
    g_PetsMountWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Pets.MountWords",
        "mount|mounts|mounted|ride|rides|riding|horse|horses|steed|steeds|kodo|kodos|raptor|raptors|"
        "mechanostrider|ram|rams|elekk|hawkstrider|nightsaber|frostsaber|charger|warhorse|felsteed|dreadsteed|"
        "deathcharger|mammoth|drake|proto drake|gryphon|wyvern|flying mount|epic mount"));

    // --- the dings a guild is told of ---------------------------------------------------
    g_CongratsGuildLevels.reset();
    std::string levels;
    {
        std::string token;
        for (char ch : sConfigMgr->GetOption<std::string>("OllamaChat.Congrats.GuildLevels", "80") + "|")
        {
            if (ch >= '0' && ch <= '9')
                token += ch;
            else if (!token.empty())
            {
                const unsigned long level = token.size() <= 3 ? std::stoul(token) : 0;
                if (level > 0 && level < g_CongratsGuildLevels.size())
                {
                    g_CongratsGuildLevels.set(level);
                    levels += (levels.empty() ? "" : ", ") + token;
                }
                token.clear();
            }
        }
        if (levels.empty())
            levels = "none";
    }

    // --- /say mingles: what the things they name are, and links -------------------------
    g_MingleThingsText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.ThingsText",
        "So they know what those are: {things}.");
    g_MingleLinksText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.LinksText",
        "When a line asks about, offers, asks for or shows off a particular item or quest, its exact name goes "
        "in square brackets, the way players link things in chat; named in passing or in a joke, it is written "
        "plainly.");

    // Mingle.Premises tags: what each one needs to be there.
    static const char* const kKnown[] = { "mount", "pet", "loot", "quest", "duel", "horde", "alliance" };
    std::string unknown;
    for (std::string premise : g_MinglePremises)
        for (const std::string& tag : TextRules::TakePremiseTags(premise))
            if (std::find_if(std::begin(kKnown), std::end(kKnown), [&tag](const char* k) { return tag == k; }) ==
                std::end(kKnown))
                unknown += (unknown.empty() ? "[" : ", [") + tag + "]";
    if (!unknown.empty())
        LOG_WARN("module.ollamachat", "[Ollama Chat] OllamaChat.Mingle.Premises: tags it does not know (a premise "
                 "with one is never picked): {}", unknown);

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v24: mounts told from pets and companions ({} mount words); "
             "guild chat gz for dings at level {}; mingles say what they name {} and link it {}.",
             g_PetsMountWords.size(), levels, g_MingleThingsText.empty() ? "off" : "on",
             (g_LinksEnable && !g_MingleLinksText.empty()) ? "on" : "off");
}

static void LoadNaturalChatV23Settings()
{
    // --- what a bot has been up to lately --------------------------------------------
    g_LatelyEnable    = sConfigMgr->GetOption<bool>("OllamaChat.Lately.Enable", true);
    g_LatelyMinutes   = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Lately.Minutes", 45), 5, 240);
    g_LatelyStateText = sConfigMgr->GetOption<std::string>("OllamaChat.Lately.StateText",
        "What you have been doing lately: {lately}. If that is what they ask about, say it the way a player would, "
        "in a few words of your own: what and where.");
    g_LatelyWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Lately.Words",
        "up to|what you doing|what u doing|what are you doing|what r u doing|wyd|whats up|sup|wassup|hows it going|"
        "how is it going|how you doing|how u doing|how are you|how r u|where you been|where have you been|"
        "been doing|you busy|u busy|any luck|hows the grind|how was it|how did it go|howd it go"));
    g_MingleLatelyText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.LatelyText",
        " What {name} has been doing lately: {lately}.");

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v23: what bots have been up to lately {} (the last {} min); "
             "mingling every {}-{} min near a player, {} at a time, up to {} emote(s) each; bots greeting each other "
             "every {}-{} min.",
             g_LatelyEnable ? "on" : "off", g_LatelyMinutes, g_MingleGapMinSeconds / 60, g_MingleGapMaxSeconds / 60,
             g_MingleMaxNearPlayer, g_MingleMaxEmotes, g_MingleEmoteMinSeconds / 60, g_MingleEmoteMaxSeconds / 60);
}

static void LoadNaturalChatV22Settings()
{
    // --- the fourth wall ---------------------------------------------------------------
    g_ResponseFourthWallPhrases = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Response.FourthWallPhrases",
        "bot|bots|botting|botter|botters|botted|playerbot|playerbots|ai|chatbot|chatbots|chatgpt|llm|"
        "language model|artificial intelligence|programmed|my programming|real player|real players|real person|"
        "real people|actual player|actual players|actual person|actual people|fake player|fake players"));
    g_ResponseFourthWallAllow = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Response.FourthWallAllow",
        "repair bot|repair bots|rocket bot|rocket bots|bomb bot|bomb bots|assault bot|assault bots|junk bot|"
        "junk bots|fire bot|fire bots|scrap bot|scrap bots|boom bot|boom bots"));
    g_ResponseFourthWallMinutes =
        std::min<uint32_t>(1440, sConfigMgr->GetOption<uint32_t>("OllamaChat.Response.FourthWallMinutes", 10));

    // --- a bot's own pets --------------------------------------------------------------
    g_PetsStateText = sConfigMgr->GetOption<std::string>("OllamaChat.Pets.StateText",
        "What you have with you right now: {pets}. If the talk is about them, you know what each one is and what "
        "it is called, and talk about them the way players do about their own: a mount is what you ride, and a "
        "hunter's pet, a demon or a companion pet is no mount.");
    g_PetsWords = CrowdText::ParsePhrases(sConfigMgr->GetOption<std::string>("OllamaChat.Pets.Words",
        "pet|pets|companion|companions|critter|critters|minion|minions|demon|demons|tame|tamed|taming|"
        "following you|following u|follows you|follow you around"));

    LOG_INFO("module.ollamachat", "[Ollama Chat] Chat v22: the fourth wall {} ({} phrases, {} names that only "
             "look like it, one wink in {} min), bots knowing their own pets {}.",
             g_ResponseFourthWallPhrases.empty() ? "off" : "on", g_ResponseFourthWallPhrases.size(),
             g_ResponseFourthWallAllow.size(), g_ResponseFourthWallMinutes, g_PetsStateText.empty() ? "off" : "on");
}

static void LoadNaturalChatV16Settings()
{
    auto number = [](const char* key, uint32_t def, uint32_t lo, uint32_t hi)
    {
        return std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>(key, def), lo, hi);
    };

    g_MingleEnable         = sConfigMgr->GetOption<bool>("OllamaChat.Mingle.Enable", true);
    g_MingleCitiesOnly     = sConfigMgr->GetOption<bool>("OllamaChat.Mingle.CitiesOnly", true);
    g_MingleGapMinSeconds  = number("OllamaChat.Mingle.GapMinSeconds", 300, 5, 7200);
    g_MingleGapMaxSeconds  = std::max(g_MingleGapMinSeconds, number("OllamaChat.Mingle.GapMaxSeconds", 1800, 5, 7200));
    g_MingleStartChance    = number("OllamaChat.Mingle.StartChance", 100, 0, 100);
    g_MingleMaxActive      = number("OllamaChat.Mingle.MaxActive", 4, 0, 50);
    g_MingleMaxNearPlayer  = number("OllamaChat.Mingle.MaxNearPlayer", 1, 1, 10);
    g_MingleHearRange      = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Mingle.HearRange", 25.0f), 5.0f, 60.0f);
    g_MinglePairRange      = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Mingle.PairRange", 9.0f), 2.0f, 30.0f);
    g_MingleThreeChance    = number("OllamaChat.Mingle.ThreeChance", 25, 0, 100);
    g_MingleMinLines       = number("OllamaChat.Mingle.MinLines", 2, 2, 20);
    g_MingleMaxLines       = std::max(g_MingleMinLines, number("OllamaChat.Mingle.MaxLines", 10, 2, 20));
    g_MingleMaxEmotes      = number("OllamaChat.Mingle.MaxEmotes", 1, 0, 10);
    g_MingleBotCooldownSeconds = number("OllamaChat.Mingle.BotCooldownSeconds", 600, 0, 86400);
    g_MingleQueueLimit     = number("OllamaChat.Mingle.QueueLimit", 4, 0, 1000);
    g_MingleNumPredict     = number("OllamaChat.Mingle.NumPredict", 320, 64, 4096);
    g_MingleReadMinMs      = number("OllamaChat.Mingle.ReadMinMs", 1200, 0, 60000);
    g_MingleReadMaxMs      = std::max(g_MingleReadMinMs, number("OllamaChat.Mingle.ReadMaxMs", 3500, 0, 60000));
    g_MinglePromptTemplate = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.PromptTemplate",
        "{count} World of Warcraft players are standing near each other in {where} and get chatting in /say. "
        "It is {when}. They are:{people}{relation} What gets them talking: {premise}. Write what they type, "
        "one message per line, each line starting with the speaker's name and a colon, like "
        "this:{example}About {lines} lines in all, mostly taking turns. It reads like a real chat: every "
        "line follows on from the one before it, answering what was just asked, reacting to what was just "
        "said, or asking something back, and nobody answers a question nobody asked. A greeting gets a "
        "greeting back, or a question. When one of them says what they have been doing, it is what they have "
        "been doing lately as told above, put the way a player would in a few words, what and where; with "
        "nothing told, they have not been up to much. Each line is what that player types: short (2 to 12 "
        "words), casual, in their own chat style, mostly lowercase, but a question ends with a question "
        "mark, and a line can end with an exclamation mark where it fits. Reactions and goodbyes are as "
        "short as players make them, in each one's own style. Instead of words, a line can be just an emote "
        "at the other one, written as a slash command ({emotes}), at most {max_emotes} in all. It ends the "
        "way a chat between players does: a laugh, a goodbye, someone heading off, or simply done. No "
        "narration, no actions in asterisks, no quotation marks, and nothing made up from the game -- no "
        "item, quest, NPC, creature, place, mount or pet that is not really in it, and nobody riding a mount "
        "or with a pet out but as told above (a hunter's pet, a warlock's demon or a companion pet is no "
        "mount); plainly made-up fun is fine as an imagine-if. A famous line from a film or show is fine now "
        "and then, but never say what it is from.");
    g_MinglePremises = ReadPipeList("OllamaChat.Mingle.Premises",
        "saying hello and asking what the other has been up to|[quest] one of them just got back from "
        "questing and moans about it|[quest] one of them asks the other about the quest they just handed "
        "in|[mount] one of them admires the mount the other is riding|[pet] one of them admires the other's "
        "pet or companion|one of them admires the other's gear|one of them teases the other about their race "
        "or class, all in good fun|swapping tips about levelling their classes|one of them is waiting for a "
        "group and getting bored|[duel] one of them challenges the other to a duel, and they agree to head "
        "{duel_where} and fight it out|one of them asks the other if they "
        "have a few silver to spare|[loot] one of them shows off something they got lately|people-watching: "
        "the players walking past|levelling their professions|which zone is the worst to quest in|being lost "
        "in this city|what they are saving their gold for|a funny death one of them had today|one of them is "
        "about to log off|their guilds, or not having one|an imagine-if one of them dreams up, plainly made "
        "up for a laugh|gnomes|murlocs|[15+] whether to run a dungeon together later|[15+] a dungeon one of "
        "them ran earlier|[20+] getting their first mount|[40+] the epic mount they are saving for|[68+] "
        "flying mounts");
    g_MingleEmotes.clear();
    {
        std::string unknown;
        for (const std::string& name : ReadPipeList("OllamaChat.Mingle.Emotes",
                 "wave|laugh|bow|nod|cheer|grin|shrug|flex|chuckle|wink|thank|salute|applaud|facepalm|sigh|smile|"
                 "poke|hug|point|giggle|agree|clap"))
        {
            if (LookupTextEmoteId(name))
                g_MingleEmotes.push_back(name);
            else
                unknown += (unknown.empty() ? "" : ", ") + name;
        }
        if (!unknown.empty())
            LOG_WARN("module.ollamachat", "[Ollama Chat] OllamaChat.Mingle.Emotes: not emotes: {}", unknown);
    }
    g_MingleSameGuildText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.SameGuildText",
        "{names} are in the same guild, {guild}.");
    g_MingleStrangersText = sConfigMgr->GetOption<std::string>("OllamaChat.Mingle.StrangersText",
        "They do not know each other, beyond seeing each other around.");

    g_MingleEmoteEnable          = sConfigMgr->GetOption<bool>("OllamaChat.Mingle.Emote.Enable", true);
    g_MingleEmoteMinSeconds      = number("OllamaChat.Mingle.Emote.MinSeconds", 300, 5, 7200);
    g_MingleEmoteMaxSeconds      = std::max(g_MingleEmoteMinSeconds, number("OllamaChat.Mingle.Emote.MaxSeconds", 1200, 5, 7200));
    g_MingleEmoteChance          = number("OllamaChat.Mingle.Emote.Chance", 50, 0, 100);
    g_MingleEmoteReturnChance    = number("OllamaChat.Mingle.Emote.ReturnChance", 60, 0, 100);
    g_MingleEmoteRange           = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Mingle.Emote.Range", 8.0f), 2.0f, 30.0f);
    g_MingleEmoteBotCooldownSeconds = number("OllamaChat.Mingle.Emote.BotCooldownSeconds", 300, 0, 86400);
    g_MingleGreetings.clear();
    {
        std::string unknown;
        for (const std::string& name : ReadPipeList("OllamaChat.Mingle.Emote.Greetings",
                 "wave|nod|bow|smile|hello|salute|wink|cheer|grin"))
        {
            if (const uint32_t id = LookupTextEmoteId(name))
                g_MingleGreetings.push_back(id);
            else
                unknown += (unknown.empty() ? "" : ", ") + name;
        }
        if (!unknown.empty())
            LOG_WARN("module.ollamachat", "[Ollama Chat] OllamaChat.Mingle.Emote.Greetings: not emotes: {}", unknown);
    }

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Chat v16: mingling {} ({} premises, {} emotes, up to {} at once); bots greeting each other {}.",
             g_MingleEnable ? "on" : "off", g_MinglePremises.size(), g_MingleEmotes.size(), g_MingleMaxActive,
             g_MingleEmoteEnable ? "on" : "off");
}

void LoadPersonalityExtras()
{
    g_PersonalityNudges.clear();
    g_PersonalityMaxReplyLength.clear();
    g_PersonalityLongReplyNote = sConfigMgr->GetOption<std::string>("OllamaChat.Personality.LongReplyNote",
        "You can write more than most players do: when you explain how to do something, use two or three short "
        "sentences, up to about {words} words, clear and to the point. Still one chat line, no lists or "
        "headings.");
    g_PersonalitySaidLatelyNote = sConfigMgr->GetOption<std::string>("OllamaChat.Personality.SaidLatelyNote",
        "Lately you have said: {lines}. Nothing like any of those again.");
    g_PersonalitySaidLatelyLines = std::min<uint32_t>(10,
        sConfigMgr->GetOption<uint32_t>("OllamaChat.Personality.SaidLatelyLines", 5));

    std::string bad;
    for (const std::string& key : g_PersonalityKeys)
    {
        std::string upper;
        for (char c : key)
            upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        // "<percent>|<instruction>" or "<percent>|<instruction>|<otherwise>".
        // A "{~a|b}" in an instruction is wording that changes each time:
        // its bars do not split.
        const std::string nudge = sConfigMgr->GetOption<std::string>("OllamaChat.Personality.Nudge." + key, "", false);
        if (!nudge.empty())
        {
            auto barAfter = [&nudge](size_t from) -> size_t
            {
                int depth = 0;
                for (size_t i = from; i < nudge.size(); ++i)
                {
                    if (nudge[i] == '{')   // "{~a|b}", and any "{slot}" inside one
                        ++depth;
                    else if (nudge[i] == '}' && depth > 0)
                        --depth;
                    else if (nudge[i] == '|' && depth == 0)
                        return i;
                }
                return std::string::npos;
            };
            std::vector<std::string> parts;
            size_t start = 0;
            for (int i = 0; i < 2; ++i)
            {
                const size_t bar = barAfter(start);
                if (bar == std::string::npos)
                    break;
                parts.push_back(nudge.substr(start, bar - start));
                start = bar + 1;
            }
            parts.push_back(nudge.substr(start));

            auto trim = [](std::string s)
            {
                const size_t b = s.find_first_not_of(" \t");
                const size_t e = s.find_last_not_of(" \t");
                return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
            };
            const std::string pct = trim(parts[0]);
            const bool number = !pct.empty() && pct.size() <= 3 &&
                std::all_of(pct.begin(), pct.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            if (parts.size() >= 2 && number && !trim(parts[1]).empty())
            {
                PersonalityNudge n;
                n.percent = std::min<uint32_t>(100, static_cast<uint32_t>(std::stoul(pct)));
                n.yes     = trim(parts[1]);
                n.no      = parts.size() > 2 ? trim(parts[2]) : std::string();
                g_PersonalityNudges[upper] = std::move(n);
            }
            else
            {
                bad += (bad.empty() ? "" : ", ") + ("Nudge." + key);
            }
        }

        // One chat line holds about 255 characters.
        const uint32_t length = sConfigMgr->GetOption<uint32_t>("OllamaChat.Personality.MaxReplyLength." + key, 0, false);
        if (length > 0)
            g_PersonalityMaxReplyLength[upper] = std::min<uint32_t>(length, 250);
    }

    if (!bad.empty())
        LOG_WARN("module.ollamachat",
                 "[Ollama Chat] Ignored OllamaChat.Personality settings that are not \"<percent>|<instruction>\": {}",
                 bad);
    if (!g_PersonalityNudges.empty() || !g_PersonalityMaxReplyLength.empty())
        LOG_INFO("module.ollamachat", "[Ollama Chat] Chat styles with a nudge: {}, with a longer reply limit: {}.",
                 g_PersonalityNudges.size(), g_PersonalityMaxReplyLength.size());
}

void LoadEmoteStyles()
{
    g_EmoteStyles.clear();

    std::string unknown;
    g_EmoteStyleDefault = ReadEmoteList(sConfigMgr->GetOption<std::string>(
        "OllamaChat.Emote.Style.Default", "wave|wave|nod|smile|cheer|bow|thank|laugh|hello"), &unknown);

    g_EmotePetEmotes.clear();
    for (uint32_t id : ReadEmoteList(sConfigMgr->GetOption<std::string>(
             "OllamaChat.Emote.PetEmotes", "pet|pat|cuddle|love|purr|hug"), &unknown))
        g_EmotePetEmotes.insert(id);

    // One optional key per personality; a personality without one uses Default.
    for (const std::string& key : g_PersonalityKeys)
    {
        const std::string raw = sConfigMgr->GetOption<std::string>("OllamaChat.Emote.Style." + key, "", false);
        if (raw.empty())
            continue;

        std::string upper;
        for (char c : key)
            upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        std::vector<uint32_t> ids = ReadEmoteList(raw, &unknown);
        if (!ids.empty())
            g_EmoteStyles[upper] = std::move(ids);
    }

    if (!unknown.empty())
        LOG_WARN("module.ollamachat", "[Ollama Chat] Unknown emote names in OllamaChat.Emote settings: {}", unknown);

    LOG_INFO("module.ollamachat", "[Ollama Chat] Emote styles for {} personalities ({} default emotes).",
             g_EmoteStyles.size(), g_EmoteStyleDefault.size());
}

void LoadOllamaChatConfig()
{
    g_SayDistance                     = sConfigMgr->GetOption<float>("OllamaChat.SayDistance", 30.0f);
    g_YellDistance                    = sConfigMgr->GetOption<float>("OllamaChat.YellDistance", 100.0f);
    
    // Load per-channel-type reply chances
    g_PlayerReplyChance_Say           = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Say", 90);
    g_BotReplyChance_Say              = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Say", 10);
    g_PlayerReplyChance_Channel       = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Channel", 50);
    g_BotReplyChance_Channel          = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Channel", 5);
    g_PlayerReplyChance_Party         = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Party", 90);
    g_BotReplyChance_Party            = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Party", 10);
    g_PlayerReplyChance_Guild         = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Guild", 70);
    g_BotReplyChance_Guild            = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Guild", 5);
    
    g_MaxBotsToPick                   = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxBotsToPick", 2);
    g_OllamaUrl                       = sConfigMgr->GetOption<std::string>("OllamaChat.Url", "http://localhost:11434/api/generate");
    g_OllamaModel                     = sConfigMgr->GetOption<std::string>("OllamaChat.Model", "llama3.2:1b");
    g_OllamaProvider                  = OllamaProvider_Parse(sConfigMgr->GetOption<std::string>("OllamaChat.Provider", "ollama"));
    g_OllamaApiKey                    = TrimApiKey(sConfigMgr->GetOption<std::string>("OllamaChat.ApiKey", ""));
    g_OllamaApiKeyHeader              = sConfigMgr->GetOption<std::string>("OllamaChat.ApiKeyHeader", "");
    g_OllamaVerifyCertificates        = sConfigMgr->GetOption<int32_t>("OllamaChat.VerifyCertificates", -1);

    // The endpoint path is provider-specific and the most common mistake is
    // switching Provider while leaving Url on Ollama's /api/generate. Warn,
    // do not override: proxies and self-hosted gateways use arbitrary paths.
    {
        const char* expected = nullptr;
        switch (g_OllamaProvider)
        {
            case OllamaProvider::OpenAI:    expected = "/chat/completions"; break;
            case OllamaProvider::Anthropic: expected = "/messages";         break;
            default:                        expected = "/api/generate";     break;
        }
        if (g_OllamaUrl.find(expected) == std::string::npos)
        {
            LOG_WARN("module.ollamachat",
                     "[Ollama Chat] OllamaChat.Provider is '{}' but OllamaChat.Url ('{}') does not "
                     "contain '{}'. Check that the URL is the full endpoint for this provider.",
                     OllamaProvider_Name(g_OllamaProvider), g_OllamaUrl, expected);
        }
        if (g_OllamaProvider != OllamaProvider::Ollama && g_OllamaApiKey.empty())
        {
            LOG_WARN("module.ollamachat",
                     "[Ollama Chat] OllamaChat.Provider is '{}' and OllamaChat.ApiKey is empty. "
                     "Hosted services will reject every request; local servers (LM Studio, vLLM) are fine.",
                     OllamaProvider_Name(g_OllamaProvider));
        }

        // Say this at startup, once, rather than on every failed request. Every
        // hosted provider is HTTPS-only, so without OpenSSL none of them work.
        if (g_OllamaUrl.rfind("https://", 0) == 0 && !OllamaHttp_TlsAvailable())
        {
            LOG_ERROR("module.ollamachat",
                      "[Ollama Chat] OllamaChat.Url is HTTPS but this build has no OpenSSL support, "
                      "so every request will fail. Install the OpenSSL development package "
                      "(Windows: 'vcpkg install openssl' or the Win64 OpenSSL installer; "
                      "Linux: libssl-dev / openssl-devel), re-run CMake and confirm it prints "
                      "'[mod-ollama-chat] OpenSSL found - HTTPS support enabled'.");
        }
    }

    g_OllamaNumPredict                = sConfigMgr->GetOption<uint32_t>("OllamaChat.NumPredict", 40);
    g_OllamaTemperature               = sConfigMgr->GetOption<float>("OllamaChat.Temperature", 0.8f);
    g_OllamaTopP                      = sConfigMgr->GetOption<float>("OllamaChat.TopP", 0.95f);
    g_OllamaRepeatPenalty             = sConfigMgr->GetOption<float>("OllamaChat.RepeatPenalty", 1.1f);
    g_OllamaNumCtx                    = sConfigMgr->GetOption<uint32_t>("OllamaChat.NumCtx", 0);
    g_OllamaNumThreads                = sConfigMgr->GetOption<uint32_t>("OllamaChat.NumThreads", 0);
    g_OllamaStop                      = sConfigMgr->GetOption<std::string>("OllamaChat.Stop", "");
    g_OllamaSystemPrompt              = sConfigMgr->GetOption<std::string>("OllamaChat.SystemPrompt", "");
    g_OllamaSeed                      = sConfigMgr->GetOption<std::string>("OllamaChat.Seed", "");

    g_MaxConcurrentQueries            = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxConcurrentQueries", 0);

    g_Enable                          = sConfigMgr->GetOption<bool>("OllamaChat.Enable", true);
    g_DisableRepliesInCombat          = sConfigMgr->GetOption<bool>("OllamaChat.DisableRepliesInCombat", true);
    g_EnableRandomChatter             = sConfigMgr->GetOption<bool>("OllamaChat.EnableRandomChatter", true);
    g_EnableEventChatter              = sConfigMgr->GetOption<bool>("OllamaChat.EnableEventChatter", true);
    g_EnableWhisperReplies            = sConfigMgr->GetOption<bool>("OllamaChat.EnableWhisperReplies", false);

    g_DebugEnabled                    = sConfigMgr->GetOption<bool>("OllamaChat.DebugEnabled", false);
    g_DebugShowFullPrompt             = sConfigMgr->GetOption<bool>("OllamaChat.DebugShowFullPrompt", false);

    g_MinRandomInterval               = sConfigMgr->GetOption<uint32_t>("OllamaChat.MinRandomInterval", 45);
    g_MaxRandomInterval               = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxRandomInterval", 180);
    g_RandomChatterRealPlayerDistance = sConfigMgr->GetOption<float>("OllamaChat.RandomChatterRealPlayerDistance", 40.0f);
    g_RandomChatterBotCommentChance   = sConfigMgr->GetOption<uint32_t>("OllamaChat.RandomChatterBotCommentChance", 25);
    g_RandomChatterMaxBotsPerPlayer   = sConfigMgr->GetOption<uint32_t>("OllamaChat.RandomChatterMaxBotsPerPlayer", 2);

    g_EnableGuildRandomAmbientChatter = sConfigMgr->GetOption<bool>("OllamaChat.EnableGuildRandomAmbientChatter", true);
    g_GuildRandomChatterChance        = sConfigMgr->GetOption<uint32_t>("OllamaChat.GuildRandomChatterChance", 10);

    g_EventChatterRealPlayerDistance = sConfigMgr->GetOption<float>("OllamaChat.EventChatterRealPlayerDistance", 40.0f);
    g_EventChatterBotCommentChance   = sConfigMgr->GetOption<uint32_t>("OllamaChat.EventChatterBotCommentChance", 15);
    g_EventChatterBotSelfCommentChance = sConfigMgr->GetOption<uint32_t>("OllamaChat.EventChatterBotSelfCommentChance", 5);
    g_EventChatterMaxBotsPerPlayer   = sConfigMgr->GetOption<uint32_t>("OllamaChat.EventChatterMaxBotsPerPlayer", 2);

    g_EnableRPPersonalities           = sConfigMgr->GetOption<bool>("OllamaChat.EnableRPPersonalities", false);

    g_RandomChatterPromptTemplate     = sConfigMgr->GetOption<std::string>("OllamaChat.RandomChatterPromptTemplate", "");

    // Load random chatter prompt variations
    std::string variationsStr = sConfigMgr->GetOption<std::string>("OllamaChat.RandomChatterPromptVariations", "");
    g_RandomChatterPromptVariations.clear();
    if (!variationsStr.empty())
    {
        std::stringstream ss(variationsStr);
        std::string variation;
        while (std::getline(ss, variation, '|'))
        {
            if (!variation.empty())
            {
                g_RandomChatterPromptVariations.push_back(variation);
            }
        }
    }

    // Load random chatter question variations
    std::string questionsStr = sConfigMgr->GetOption<std::string>("OllamaChat.RandomChatterQuestionVariations", "");
    g_RandomChatterQuestionVariations.clear();
    if (!questionsStr.empty())
    {
        std::stringstream ss(questionsStr);
        std::string question;
        while (std::getline(ss, question, '|'))
        {
            if (!question.empty())
            {
                g_RandomChatterQuestionVariations.push_back(question);
            }
        }
    }

    g_EventChatterPromptTemplate     = sConfigMgr->GetOption<std::string>("OllamaChat.EventChatterPromptTemplate", "");

    g_ChatPromptTemplate              = sConfigMgr->GetOption<std::string>("OllamaChat.ChatPromptTemplate", "");
    
    g_ChatExtraInfoTemplate           = sConfigMgr->GetOption<std::string>("OllamaChat.ChatExtraInfoTemplate", "");

    g_DefaultPersonalityPrompt        = sConfigMgr->GetOption<std::string>("OllamaChat.DefaultPersonalityPrompt", "");

    g_MaxConversationHistory          = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxConversationHistory", 5);
    g_ConversationHistorySaveInterval = sConfigMgr->GetOption<uint32_t>("OllamaChat.ConversationHistorySaveInterval", 10);

    g_ChatHistoryHeaderTemplate       = sConfigMgr->GetOption<std::string>("OllamaChat.ChatHistoryHeaderTemplate", "");
    g_ChatHistoryLineTemplate         = sConfigMgr->GetOption<std::string>("OllamaChat.ChatHistoryLineTemplate", "");
    g_ChatHistoryFooterTemplate       = sConfigMgr->GetOption<std::string>("OllamaChat.ChatHistoryFooterTemplate", "");

    g_EnableChatBotSnapshotTemplate   = sConfigMgr->GetOption<bool>("OllamaChat.EnableChatBotSnapshotTemplate", false);
    g_ChatBotSnapshotTemplate         = sConfigMgr->GetOption<std::string>("OllamaChat.ChatBotSnapshotTemplate", "");

    g_EnableChatHistory               = sConfigMgr->GetOption<bool>("OllamaChat.EnableChatHistory", true);

    // Bot-Player Sentiment Tracking
    g_EnableSentimentTracking         = sConfigMgr->GetOption<bool>("OllamaChat.EnableSentimentTracking", true);
    g_SentimentDefaultValue           = sConfigMgr->GetOption<float>("OllamaChat.SentimentDefaultValue", 0.5f);
    g_SentimentAdjustmentStrength     = sConfigMgr->GetOption<float>("OllamaChat.SentimentAdjustmentStrength", 0.1f);
    g_SentimentSaveInterval           = sConfigMgr->GetOption<uint32_t>("OllamaChat.SentimentSaveInterval", 10);
    g_SentimentAnalysisPrompt         = sConfigMgr->GetOption<std::string>("OllamaChat.SentimentAnalysisPrompt", "Analyze the sentiment of this message: \"{message}\". Respond only with: POSITIVE, NEGATIVE, or NEUTRAL.");
    g_SentimentPromptTemplate         = sConfigMgr->GetOption<std::string>("OllamaChat.SentimentPromptTemplate", "Your relationship sentiment with {player_name} is {sentiment_value} (0.0=hostile, 0.5=neutral, 1.0=friendly). Use this to guide your tone and response.");

    // RAG (Retrieval-Augmented Generation) System
    g_EnableRAG                       = sConfigMgr->GetOption<bool>("OllamaChat.EnableRAG", false);
    g_RAGDataPath                     = sConfigMgr->GetOption<std::string>("OllamaChat.RAGDataPath", "rag/");
    g_RAGMaxRetrievedItems            = sConfigMgr->GetOption<uint32_t>("OllamaChat.RAGMaxRetrievedItems", 3);
    g_RAGSimilarityThreshold          = sConfigMgr->GetOption<float>("OllamaChat.RAGSimilarityThreshold", 0.3f);
    g_RAGPromptTemplate               = sConfigMgr->GetOption<std::string>("OllamaChat.RAGPromptTemplate", "RELEVANT INFORMATION:\n{rag_info}\nUse this information to provide accurate and detailed responses when applicable.");

    g_ChatterUseGeneralChannel          = sConfigMgr->GetOption<bool>("OllamaChat.Chatter.UseGeneralChannel", true);
    g_ChatterUseTradeChannel            = sConfigMgr->GetOption<bool>("OllamaChat.Chatter.UseTradeChannel", true);
    g_ChatterUseLfgChannel              = sConfigMgr->GetOption<bool>("OllamaChat.Chatter.UseLookingForGroupChannel", false);
    g_ChatterUseGuildRecruitmentChannel = sConfigMgr->GetOption<bool>("OllamaChat.Chatter.UseGuildRecruitmentChannel", false);

    // --- Long-term memory and relationships ------------------------------
    g_MemoryEnable                 = sConfigMgr->GetOption<bool>("OllamaChat.Memory.Enable", true);
    g_MemoryHistoryTokenLimit      = sConfigMgr->GetOption<uint32_t>("OllamaChat.Memory.HistoryTokenLimit", 1500);
    g_MemoryPromptTokenBudget      = sConfigMgr->GetOption<uint32_t>("OllamaChat.Memory.PromptTokenBudget", 400);
    g_MemoryMaxPerBot              = sConfigMgr->GetOption<uint32_t>("OllamaChat.Memory.MaxPerBot", 40);
    g_MemorySaveInterval           = sConfigMgr->GetOption<uint32_t>("OllamaChat.Memory.SaveInterval", 10);

    g_MemoryCondensePrompt         = sConfigMgr->GetOption<std::string>("OllamaChat.Memory.CondensePrompt", "");
    if (g_MemoryCondensePrompt.empty())
    {
        g_MemoryCondensePrompt =
            "You are {bot_name}. Below is a record of conversations you took part in.\n"
            "{history}\n"
            "Write the handful of things worth remembering long after the words are forgotten: "
            "what happened, what you learned about someone, how something made you feel. "
            "One per line. Prefix each with an importance from 1 to 10 and a pipe, like "
            "\"7 | ...\". Write them in the third person as short factual notes, at most 20 words each. "
            "Skip small talk. If nothing is worth keeping, write nothing.";
    }

    g_MemoryPromptTemplate         = sConfigMgr->GetOption<std::string>("OllamaChat.Memory.PromptTemplate", "");
    if (g_MemoryPromptTemplate.empty())
        g_MemoryPromptTemplate = " Things you remember:\n{memories}";

    g_RelationshipEnable           = sConfigMgr->GetOption<bool>("OllamaChat.Relationship.Enable", true);
    g_RelationshipMentionThreshold = sConfigMgr->GetOption<uint32_t>("OllamaChat.Relationship.MentionThreshold", 8);
    g_RelationshipMaxPerPrompt     = sConfigMgr->GetOption<uint32_t>("OllamaChat.Relationship.MaxPerPrompt", 3);
    g_RelationshipMaxLength        = sConfigMgr->GetOption<uint32_t>("OllamaChat.Relationship.MaxLength", 220);

    g_RelationshipUpdatePrompt     = sConfigMgr->GetOption<std::string>("OllamaChat.Relationship.UpdatePrompt", "");
    if (g_RelationshipUpdatePrompt.empty())
    {
        g_RelationshipUpdatePrompt =
            "You are {bot_name}. What you currently think of {other_name}: {existing}\n"
            "Recent conversations:\n{history}\n"
            "In one or two sentences, write how you now feel about {other_name} and why. "
            "Revise your earlier view only if these conversations actually changed it. "
            "Write it as a plain statement, no quotes, no preamble.";
    }

    g_RelationshipPromptTemplate   = sConfigMgr->GetOption<std::string>("OllamaChat.Relationship.PromptTemplate", "");
    if (g_RelationshipPromptTemplate.empty())
        g_RelationshipPromptTemplate = " How you feel about people you know:\n{relationships}";

    // --- Optional sampling controls --------------------------------------
    // Left unset by default so nothing changes unless an operator opts in.
    g_OllamaTopK                      = sConfigMgr->GetOption<int32_t>("OllamaChat.TopK", -1);
    g_OllamaMinP                      = sConfigMgr->GetOption<float>("OllamaChat.MinP", -1.0f);
    g_OllamaPresencePenalty           = sConfigMgr->GetOption<float>("OllamaChat.PresencePenalty", -1000.0f);
    g_OllamaFrequencyPenalty          = sConfigMgr->GetOption<float>("OllamaChat.FrequencyPenalty", -1000.0f);

    // --- Think mode ------------------------------------------------------
    // ThinkModeEnableForModule is kept as a deprecated alias: when it is set
    // and ThinkMode is left at auto, it forces think on, matching old configs.
    g_ThinkModeEnableForModule        = sConfigMgr->GetOption<bool>("OllamaChat.ThinkModeEnableForModule", false);
    g_ThinkModePolicy                 = static_cast<uint8_t>(OllamaCapability_ParsePolicy(
                                            sConfigMgr->GetOption<std::string>("OllamaChat.ThinkMode", "auto"),
                                            g_ThinkModeEnableForModule));
    g_ThinkMaxLatencyMs               = sConfigMgr->GetOption<uint32_t>("OllamaChat.ThinkMaxLatencyMs", 12000);
    g_ReasoningTokenReserve           = sConfigMgr->GetOption<uint32_t>("OllamaChat.ReasoningTokenReserve", 512);
    g_CapabilityProbeTimeoutSeconds   = sConfigMgr->GetOption<uint32_t>("OllamaChat.CapabilityProbeTimeoutSeconds", 10);

    // --- HTTP / dispatcher ----------------------------------------------
    g_HttpTimeoutSeconds              = sConfigMgr->GetOption<uint32_t>("OllamaChat.HttpTimeoutSeconds", 120);
    g_RateLimitRetries                = sConfigMgr->GetOption<uint32_t>("OllamaChat.RateLimitRetries", 2);
    g_RateLimitMaxWaitSeconds         = sConfigMgr->GetOption<uint32_t>("OllamaChat.RateLimitMaxWaitSeconds", 15);
    g_DispatchWorkerThreads           = sConfigMgr->GetOption<uint32_t>("OllamaChat.WorkerThreads",
                                            g_MaxConcurrentQueries > 0 ? g_MaxConcurrentQueries : 4);
    g_MaxQueueDepth                   = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxQueueDepth", 64);

    // --- Response pipeline ----------------------------------------------
    g_MaxReplyLength                  = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxReplyLength", 240);
    g_ResponseStripMarkdown           = sConfigMgr->GetOption<bool>("OllamaChat.StripMarkdown", true);
    g_ResponseStripDecorativeUnicode  = sConfigMgr->GetOption<bool>("OllamaChat.StripDecorativeUnicode", true);

    // --- Conversation governor -------------------------------------------
    g_MaxChainDepth                   = static_cast<uint8_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.BotConversation.MaxChainDepth", 3));
    g_ChainChanceDecayPct             = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotConversation.ChanceDecayPct", 50);
    g_RequireRecentHuman              = sConfigMgr->GetOption<bool>("OllamaChat.BotConversation.RequireRecentHuman", true);
    g_HumanWindowSeconds              = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotConversation.HumanWindowSeconds", 120);
    g_BotCooldownSeconds              = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.PerBotSeconds", 45);
    g_ScopeCooldownSeconds            = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.PerScopeSeconds", 15);
    g_DirectAddressGroupSize          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.DirectAddressGroupSize", 5);
    g_ConversationWindowSeconds       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.ConversationWindowSeconds", 180);
    g_DirectAddressReplyChance        = sConfigMgr->GetOption<uint32_t>("OllamaChat.DirectAddressReplyChance", 95);
    g_ScopeMessagesPerMinute          = sConfigMgr->GetOption<uint32_t>("OllamaChat.RateLimit.ScopePerMinute", 8);
    g_GlobalMessagesPerMinute         = sConfigMgr->GetOption<uint32_t>("OllamaChat.RateLimit.GlobalPerMinute", 40);
    g_BotHistorySize                  = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.BotHistorySize", 12);
    g_ScopeHistorySize                = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.ScopeHistorySize", 30);
    g_RepetitionSimilarityThreshold   = sConfigMgr->GetOption<float>("OllamaChat.Repetition.SimilarityThreshold", 0.72f);
    g_RepetitionWindowSeconds         = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.WindowSeconds", 1800);
    g_OpenerHistorySize               = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.OpenerHistorySize", 8);
    g_RepetitionShortLineWindowSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.ShortLineWindowSeconds", 300);
    g_RepetitionOwnRepeatNote         = sConfigMgr->GetOption<std::string>("OllamaChat.Repetition.OwnRepeatNote",
        "You said '{line}' a moment ago, to something else: say something different this time.");

    // "i'm the spider king now. avengers.": what a quote is from, cut off the end.
    g_ResponseQuoteSourceStyles.clear();
    {
        const std::string styles = sConfigMgr->GetOption<std::string>("OllamaChat.Response.QuoteSourceStyles", "NAT_BINGER");
        size_t start = 0;
        while (start <= styles.size())
        {
            size_t bar = styles.find('|', start);
            if (bar == std::string::npos)
                bar = styles.size();
            std::string key;
            for (char ch : styles.substr(start, bar - start))
                if (!std::isspace(static_cast<unsigned char>(ch)))
                    key += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (!key.empty())
                g_ResponseQuoteSourceStyles.insert(key);
            start = bar + 1;
        }
    }
    g_ResponseQuoteSources = TextRules::ParseQuoteSources(
        sConfigMgr->GetOption<std::string>("OllamaChat.Response.QuoteSources",
        "the godfather|the godfather part ii|scarface|goodfellas|pulp fiction|reservoir dogs|kill bill|"
        "taxi driver|the departed|casablanca|citizen kane|gone with the wind|the wizard of oz|titanic|"
        "the sixth sense|the shining|apocalypse now|full metal jacket|platoon|saving private ryan|"
        "black hawk down|the great escape|where eagles dare|zulu|the dirty dozen|band of brothers|"
        "a few good men|jerry maguire|the silence of the lambs|the terminator|terminator 2|predator|"
        "the exorcist|poltergeist|jaws|e.t.|home alone|home alone 2|braveheart|top gun|top gun maverick|"
        "die hard|die hard 2|die hard with a vengeance|back to the future|bttf|ghostbusters|forrest gump|"
        "jurassic park|jurassic world|star wars|a new hope|the empire strikes back|return of the jedi|"
        "the phantom menace|revenge of the sith|rogue one|the mandalorian|the lord of the rings|lotr|"
        "the fellowship of the ring|the two towers|the return of the king|the hobbit|harry potter|"
        "indiana jones|raiders of the lost ark|the matrix|the dark knight|the dark knight rises|batman|"
        "batman begins|superman|spider-man|iron man|the avengers|infinity war|captain america|thor|"
        "guardians of the galaxy|deadpool|x-men|wolverine|wonder woman|black panther|doctor strange|ant-man|"
        "aquaman|shazam|justice league|suicide squad|captain marvel|watchmen|hellboy|the incredibles|"
        "toy story|toy story 2|toy story 3|toy story 4|monsters inc|finding nemo|finding dory|the lion king|"
        "aladdin|beauty and the beast|the little mermaid|snow white|cinderella|pinocchio|peter pan|bambi|"
        "dumbo|mulan|moana|encanto|zootopia|lilo and stitch|the jungle book|mary poppins|the sound of music|"
        "shrek|shrek 2|madagascar|kung fu panda|despicable me|how to train your dragon|ice age|ratatouille|"
        "wall-e|the lego movie|chicken run|wallace and gromit|paddington|paddington 2|willy wonka|"
        "charlie and the chocolate factory|the princess bride|monty python|monty python and the holy grail|"
        "holy grail|life of brian|the meaning of life|anchorman|anchorman 2|mean girls|the big lebowski|"
        "fight club|the shawshank redemption|shawshank|the usual suspects|apollo 13|groundhog day|"
        "mrs doubtfire|the truman show|dumb and dumber|ace ventura|the mask|liar liar|bruce almighty|"
        "zoolander|napoleon dynamite|borat|the hangover|step brothers|talladega nights|superbad|caddyshack|"
        "blazing saddles|young frankenstein|spaceballs|the naked gun|police academy|beverly hills cop|"
        "office space|waynes world|bill and ted|bill and teds excellent adventure|ferris bueller|"
        "ferris buellers day off|the breakfast club|dirty dancing|the goonies|stand by me|the karate kid|"
        "gremlins|robocop|total recall|commando|kindergarten cop|rambo|first blood|mad max|mad max fury road|"
        "blade runner|the fifth element|men in black|independence day|armageddon|mission impossible|"
        "james bond|goldfinger|skyfall|casino royale|austin powers|john wick|lethal weapon|point break|"
        "con air|face off|bad boys|rush hour|the fast and the furious|fast and furious|crocodile dundee|"
        "enter the dragon|the mummy|the bourne identity|pretty woman|notting hill|love actually|"
        "four weddings and a funeral|bridget jones|bridget joness diary|hot fuzz|shaun of the dead|"
        "the worlds end|inception|interstellar|the hunger games|pirates of the caribbean|potc|"
        "the wolf of wall street|the social network|the nightmare before christmas|the grinch|"
        "how the grinch stole christmas|a christmas story|christmas vacation|national lampoon|home alone 3|"
        "the santa clause|scrooged|the muppet christmas carol|the lost boys|beetlejuice|edward scissorhands|"
        "jumanji|the sandlot|space jam|lock stock|lock stock and two smoking barrels|trainspotting|"
        "the full monty|withnail and i|a fish called wanda|planes trains and automobiles|coming to america|"
        "trading places|the blues brothers|uncle buck|galaxy quest|tropic thunder|dodgeball|wedding crashers|"
        "meet the parents|american pie|scary movie|hot shots|this is spinal tap|spinal tap|team america|"
        "the pink panther|johnny english|kingsman|school of rock|happy gilmore|billy madison|the waterboy|"
        "the cable guy|legally blonde|bridesmaids|pineapple express|the evil dead|army of darkness|"
        "the rocky horror picture show|rocky horror|donnie darko|big trouble in little china|"
        "escape from new york|they live|highlander|conan the barbarian|the neverending story|"
        "the dark crystal|chitty chitty bang bang|the iron giant|the land before time|"
        "an american werewolf in london|a nightmare on elm street|nightmare on elm street|friday the 13th|"
        "the texas chainsaw massacre|the blair witch project|childs play|the evil dead 2|close encounters|"
        "2001 a space odyssey|starship troopers|district 9|the martian|the hitchhikers guide to the galaxy|"
        "hitchhikers guide|battlestar galactica|stargate|les miserables|les mis|mamma mia|"
        "the greatest showman|west side story|singin in the rain|the phantom of the opera|hairspray|"
        "la la land|fargo|no country for old men|se7en|the godfather 2|the hurt locker|inglourious basterds|"
        "django unchained|the hateful eight|once upon a time in hollywood|the good the bad and the ugly|"
        "a fistful of dollars|dirty harry|the italian job|the long good friday|get carter|mrs brown|"
        "willy wonka and the chocolate factory|honey i shrunk the kids|hocus pocus|the addams family|"
        "free willy|the mighty ducks|cool runnings|the polar express|happy feet|the secret life of pets|"
        "the croods|megamind|big hero 6|wreck-it ralph|the princess and the frog|the emperors new groove|"
        "tarzan|pocahontas|the hunchback of notre dame|101 dalmatians|lady and the tramp|the aristocats|"
        "the fox and the hound|sleeping beauty|alice in wonderland|bedknobs and broomsticks|the simpsons|"
        "futurama|family guy|south park|seinfeld|the office|the office uk|parks and rec|parks and recreation|"
        "brooklyn nine-nine|brooklyn 99|b99|how i met your mother|himym|the big bang theory|game of thrones|"
        "breaking bad|better call saul|the sopranos|the wire|star trek|star trek the next generation|"
        "the next generation|tng|deep space nine|doctor who|dr who|sherlock|blackadder|only fools and horses|"
        "only fools|fawlty towers|red dwarf|father ted|the it crowd|peep show|the inbetweeners|mr bean|"
        "monty pythons flying circus|flying circus|little britain|the young ones|allo allo|dads army|"
        "keeping up appearances|the vicar of dibley|absolutely fabulous|ab fab|gavin and stacey|"
        "the thick of it|im alan partridge|knowing me knowing you|the league of gentlemen|black books|"
        "the mighty boosh|men behaving badly|the royle family|phoenix nights|top gear|the grand tour|"
        "eastenders|coronation street|corrie|emmerdale|home and away|hollyoaks|who wants to be a millionaire|"
        "the weakest link|deal or no deal|family fortunes|blankety blank|the crystal maze|"
        "the generation game|university challenge|the price is right|wheel of fortune|family feud|"
        "the krypton factor|play your cards right|bake off|great british bake off|strictly come dancing|"
        "britains got talent|the x factor|big brother|love island|spongebob|spongebob squarepants|scooby doo|"
        "rick and morty|avatar the last airbender|the last airbender|dragon ball z|dbz|pokemon|looney tunes|"
        "tom and jerry|the flintstones|the jetsons|the a-team|knight rider|baywatch|the x-files|buffy|"
        "buffy the vampire slayer|the walking dead|stranger things|downton abbey|arrested development|"
        "its always sunny|its always sunny in philadelphia|always sunny|iasip|the fresh prince|"
        "the fresh prince of bel-air|frasier|the golden girls|m*a*s*h|the twilight zone|columbo|the muppets|"
        "the muppet show|sesame street|teletubbies|power rangers|transformers|he-man|thundercats|"
        "teenage mutant ninja turtles|ninja turtles|tmnt|peaky blinders|line of duty|black mirror|"
        "the witcher|westworld|the expanse|thunderbirds|danger mouse|wacky races|top cat|yogi bear|"
        "the smurfs|inspector gadget|count duckula|ducktales|rugrats|hey arnold|johnny bravo|"
        "dexters laboratory|the powerpuff girls|samurai jack|adventure time|gravity falls|bluey|peppa pig|"
        "postman pat|fireman sam|bob the builder|thomas the tank engine|the magic roundabout|chucklevision|"
        "art attack|robot wars|knightmare|mr blobby|noels house party|youve been framed|grange hill|"
        "byker grove|narcos|ozark|yoda|gandalf|darth vader|vader|han solo|obi-wan|obi-wan kenobi|"
        "luke skywalker|princess leia|chewbacca|chewie|buzz lightyear|homer simpson|bart simpson|del boy|"
        "del boy trotter|rodney trotter|basil fawlty|jack sparrow|captain jack sparrow|arnold schwarzenegger|"
        "schwarzenegger|arnie|spock|mr spock|captain kirk|captain picard|jean-luc picard|picard|"
        "sherlock holmes|sheldon cooper|michael scott|dwight schrute|ron swanson|jake peralta|barney stinson|"
        "jon snow|tyrion lannister|walter white|heisenberg|tony soprano|ron burgundy|inigo montoya|gollum|"
        "smeagol|frodo|aragorn|boromir|dumbledore|hagrid|voldemort|severus snape|snape|hermione|simba|mufasa|"
        "groot|thanos|tony stark|the hulk|baldrick|father jack|alan partridge|david brent|hyacinth bucket|"
        "victor meldrew|captain mainwaring|corporal jones|arnold rimmer|michael corleone|vito corleone|"
        "don corleone|tony montana|hannibal lecter|jack torrance|freddy krueger|john mcclane|hans gruber|"
        "marty mcfly|doc brown|optimus prime|fred flintstone|bugs bunny|daffy duck|elmer fudd|wile e coyote|"
        "patrick star|squidward|rick sanchez|peter griffin|stewie griffin|eric cartman|cartman|mr burns|"
        "ned flanders|ralph wiggum|chandler bing|joey tribbiani|ross geller|george costanza|frasier crane|"
        "zoidberg|rocky balboa|lord farquaad|the genie|jafar|baby yoda|r2-d2|c-3po|darth sidious|palpatine|"
        "bilbo|bilbo baggins|legolas|gimli|samwise|sauron|saruman|t-800|lieutenant dan|boycie"),
        sConfigMgr->GetOption<std::string>("OllamaChat.Response.QuoteFilmWords",
        "film|films|movie|movies|flick|flicks|cinema|tv|telly|tv show|tv shows|that show|this show|the series|"
        "series|sitcom|sitcoms|cartoon|cartoons|anime|episode|episodes|season|seasons|sequel|sequels|remake|"
        "trailer|netflix|rewatch*|binge*|watching|watched|seen it|favourite|favorite|fave|actor|actress|director|"
        "whats that from|what is that from|wheres that from|where is that from|what film|what movie|which film|"
        "which movie"));

    // --- Shared chat context ---------------------------------------------
    g_ContextEnable                   = sConfigMgr->GetOption<bool>("OllamaChat.Context.Enable", true);
    g_ContextMaxLines                 = sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.MaxLines", 10);
    g_ContextMaxAgeSeconds            = sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.MaxAgeSeconds", 300);
    g_ContextReplyWindowSeconds       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.ReplyWindowSeconds", 60);
    g_ContextBystanderChancePct       = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.BystanderChancePct", 25));
    g_ContextBotAddressedChance       = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.BotAddressedChance", 40));
    g_ContextBotReplyPerMessage       = sConfigMgr->GetOption<bool>("OllamaChat.Context.BotReplyPerMessage", true);
    g_ContextBotLineReplyChance_Say     = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.BotLineReplyChance.Say", 50));
    g_ContextBotLineReplyChance_Party   = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.BotLineReplyChance.Party", 60));
    g_ContextBotLineReplyChance_Guild   = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.BotLineReplyChance.Guild", 50));
    g_ContextBotLineReplyChance_Channel = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.BotLineReplyChance.Channel", 40));
    g_ContextBotChatWithoutRecentHuman  = sConfigMgr->GetOption<bool>("OllamaChat.Context.BotChatWithoutRecentHuman", true);
    g_ContextSuppressPairHistory      = sConfigMgr->GetOption<bool>("OllamaChat.Context.SuppressPairHistory", true);
    g_ContextMaxFillersPerLine        = sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.MaxFillersPerLine", 1);
    g_ContextFillerScopeLines         = sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.FillerScopeLines", 6);
    g_ContextFillerBotCooldownSeconds = sConfigMgr->GetOption<uint32_t>("OllamaChat.Context.FillerBotCooldownSeconds", 900);

    g_ContextPassToken = sConfigMgr->GetOption<std::string>("OllamaChat.Context.PassToken", "NOREPLY");
    g_ContextPassTokenNorm.clear();
    for (char c : g_ContextPassToken)
        if (std::isalnum(static_cast<unsigned char>(c)))
            g_ContextPassTokenNorm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    g_ContextHistoryHeader = sConfigMgr->GetOption<std::string>("OllamaChat.Context.HistoryHeader",
        "Recent {channel}, oldest first (context only):");
    g_ContextHintAddressed = sConfigMgr->GetOption<std::string>("OllamaChat.Context.HintAddressed",
        "They are talking to you, so answer them.");
    g_ContextHintBystander = sConfigMgr->GetOption<std::string>("OllamaChat.Context.HintBystander",
        "That message was most likely meant for {addressee}, not you. Usually stay out of it and reply with "
        "just {pass_token}; only chip in if a real player would, with a joke, a correction or something useful.");
    g_ContextHintOpen = sConfigMgr->GetOption<std::string>("OllamaChat.Context.HintOpen",
        "It was said to everyone, not to you in particular. Answer if you know or have something to add; "
        "if the chat above already covers it, reply with just {pass_token}.");
    g_ContextHintPublicWithGroup = sConfigMgr->GetOption<std::string>("OllamaChat.Context.HintPublicWithGroup",
        "{player_name} is in your group, but said that in {channel}, not in party chat, so answer it as a {channel} "
        "line: keep group business (moving on, pulling, waiting for someone) for party chat.");
    g_ContextHintEvent = sConfigMgr->GetOption<std::string>("OllamaChat.Context.HintEvent",
        "If a real player in your place would not bother commenting, reply with just {pass_token}.");
    g_ContextHintAmbient = sConfigMgr->GetOption<std::string>("OllamaChat.Context.HintAmbient",
        "If that conversation is still going, you can add to it naturally instead of changing the subject.");

    g_ContextFillerWords.clear();
    for (std::string word : SplitString(sConfigMgr->GetOption<std::string>("OllamaChat.Context.FillerWords", ""), '|'))
    {
        std::string clean;
        for (char c : word)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u))
                clean += static_cast<char>(std::tolower(u));
            else if (c == '*')
                clean += '*';
        }
        if (!clean.empty() && clean != "*")
            g_ContextFillerWords.push_back(clean);
    }

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Shared chat context: {} (up to {} lines / {} s, reply window {} s, "
             "pass token '{}', {} filler words rationed).",
             g_ContextEnable ? "on" : "off", g_ContextMaxLines, g_ContextMaxAgeSeconds,
             g_ContextReplyWindowSeconds, g_ContextPassToken, g_ContextFillerWords.size());

    // --- Crowd replies ---------------------------------------------------
    // Defaults live here, not only in the .conf, so a conf written before
    // these keys existed still gets sensible crowds.
    static const char* kDefaultOpenCallPhrases =
        "anyone|anybody|any1|ne1|anyone else|anybody else|everyone|everybody|every1|evry1|"
        "yall|you all|guys|people|ppl|peeps|folks|someone|somebody|sum1|who else|whos here|"
        "who is here|whos around|who is around|whos on|who is on|whos up|who wants|who needs|"
        "who can|hi all|hey all|hello all|hiya all|sup all|yo all|morning all|evening all|"
        "night all|hi chat|hey chat|hello chat|yo chat|dead chat|chat dead|so quiet|too quiet|"
        "quiet in here|talk to me|say something|lfg|lfm|lf1m|lf2m|lf3m|wts|wtb|wtt";
    static const char* kDefaultAckWords =
        "ty|thx|thnx|thanks|thank|tyvm|tysm|you|u|all|ok|okay|okey|k|kk|np|yw|lol*|lool*|lmao*|"
        "lmfao*|rofl*|haha*|hehe*|xd*|gg|wp|brb|afk|bbl|bye|cya|nice|cool|yes|yeah|ya|yep|"
        "yup|no|nah|nope|ah|oh|hmm*|same|true|rip|oof|gz|grats|gratz|grz|man|mate|bro|dude|"
        "guys|everyone|everybody|yall|folks|cheers|ta|tyty|m8|lads|lad|appreciate*|appreciated";
    static const char* kDefaultPlayful =
        "NAT_JOKER|NAT_TROLL|NAT_CONTRARIAN|NAT_REACTOR|NAT_HYPE|NAT_BANTER|NAT_DEADPAN|"
        "NAT_EXCITABLE|NAT_GRUMBLER|NAT_PRANKSTER|NAT_GOSSIP|NAT_PVPER|NAT_BINGER|NAT_BUTTERFLY|NAT_KEYBOARD";

    g_CrowdEnable              = sConfigMgr->GetOption<bool>("OllamaChat.Crowd.Enable", true);
    g_CrowdMaxResponders       = std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.MaxResponders", 5), 1, 10);
    g_CrowdBotsPerResponder    = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.BotsPerResponder", 3));
    g_CrowdChimeInChance       = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.ChimeInChance", 40));
    g_CrowdChimeInMax          = std::min<uint32_t>(4, sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.ChimeInMax", 2));
    g_CrowdAckReplyChance      = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.AckReplyChance", 50));
    g_CrowdGapMinMs            = sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.ReplyGapMinMs", 1000);
    g_CrowdGapMaxMs            = sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.ReplyGapMaxMs", 4000);
    g_CrowdSlotTimeoutSeconds  = sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.SlotTimeoutSeconds", 30);
    g_CrowdMaxPlanSeconds      = sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.MaxPlanSeconds", 120);
    g_CrowdMaxReplyBacks       = std::min<uint32_t>(6, sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.MaxReplyBacks", 2));
    g_CrowdBotLineSecondChance = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Crowd.BotLineSecondChance", 25));

    g_CrowdOpenCallPhrases = CrowdText::ParsePhrases(
        sConfigMgr->GetOption<std::string>("OllamaChat.Crowd.OpenCallPhrases", kDefaultOpenCallPhrases));
    g_CrowdAckWords = CrowdText::ParsePhrases(
        sConfigMgr->GetOption<std::string>("OllamaChat.Crowd.AckWords", kDefaultAckWords));

    g_CrowdPlayfulPersonalities.clear();
    for (const std::string& raw : SplitString(
             sConfigMgr->GetOption<std::string>("OllamaChat.Crowd.PlayfulPersonalities", kDefaultPlayful), '|'))
    {
        std::string key;
        for (char c : raw)
            if (!std::isspace(static_cast<unsigned char>(c)))
                key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (!key.empty())
            g_CrowdPlayfulPersonalities.insert(key);
    }

    g_CrowdHintOpenCall = sConfigMgr->GetOption<std::string>("OllamaChat.Crowd.HintOpenCall",
        "{player_name} said that to everyone here, and {listeners} can see the chat. Answer the way your "
        "own chat style would: a straight answer, a joke, a tease or a quick reaction are all fine.");
    g_CrowdHintFollowUp = sConfigMgr->GetOption<std::string>("OllamaChat.Crowd.HintFollowUp",
        "{answered} already answered {player_name} (see the chat above). Add your own line only if you have "
        "something different to say: your own answer, a joke, disagreeing with or teasing one of them, or just "
        "a laugh if it was funny. If you are answering one of them rather than {player_name}, use their name. "
        "Never repeat what was already said. If you have nothing to add, reply with just {pass_token}.");
    g_CrowdHintReplyBack = sConfigMgr->GetOption<std::string>("OllamaChat.Crowd.HintReplyBack",
        "{replied_to} said that to you. Answer them back if a real player would, short and in your own "
        "style; otherwise reply with just {pass_token}.");
    g_CrowdHintShout = sConfigMgr->GetOption<std::string>("OllamaChat.Crowd.HintShout",
        "{player_name} typed it in capitals, like shouting; you can answer in capitals too if that suits you.");

    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Crowd replies: {} (up to {} bots per line, one per {} listening, chime-in {}%, "
             "{}-{} ms between replies, {} playful styles).",
             (g_CrowdEnable && g_ContextEnable) ? "on" : "off", g_CrowdMaxResponders, g_CrowdBotsPerResponder,
             g_CrowdChimeInChance, g_CrowdGapMinMs, g_CrowdGapMaxMs, g_CrowdPlayfulPersonalities.size());

    LoadNaturalChatV4Settings();
    LoadNaturalChatV5Settings();
    LoadNaturalChatV6Settings();
    LoadNaturalChatV7Settings();
    LoadNaturalChatV10Settings();
    LoadNaturalChatV11Settings();
    LoadNaturalChatV13Settings();
    LoadNaturalChatV14Settings();
    LoadNaturalChatV15Settings();
    LoadNaturalChatV16Settings();
    LoadNaturalChatV20Settings();
    LoadNaturalChatV21Settings();
    LoadNaturalChatV22Settings();
    LoadNaturalChatV23Settings();
    LoadNaturalChatV24Settings();
    LoadNaturalChatV25Settings();
    LoadNaturalChatV26Settings();
    LoadNaturalChatV27Settings();
    LoadNaturalChatV28Settings();

    // --- Topic engine ----------------------------------------------------
    g_TopicWeightPeople               = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.WeightPeople", 30);
    g_TopicWeightWorld                = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.WeightWorld", 30);
    g_TopicWeightActivity             = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.WeightActivity", 25);
    g_TopicWeightSelf                 = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.WeightSelf", 15);
    g_TopicWeightGuild                = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.WeightGuild", 20);
    g_TopicWeightTalk                 = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.WeightTalk", 25);
    g_TopicWeightMarket               = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.WeightMarket", 30);
    g_MarketTalkMinutes               = sConfigMgr->GetOption<uint32_t>("OllamaChat.Market.TalkMinutes", 90);
    g_TopicMemoryCount                = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.MemoryCount", 4);
    g_TopicEventMemorySize            = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.EventMemorySize", 6);
    g_TopicEventMemorySeconds         = sConfigMgr->GetOption<uint32_t>("OllamaChat.Topic.EventMemorySeconds", 300);
    g_TopicPlayerRadius               = sConfigMgr->GetOption<float>("OllamaChat.Topic.PlayerRadius", 40.0f);
    g_RandomChatterQuestionChance     = sConfigMgr->GetOption<uint32_t>("OllamaChat.RandomChatterQuestionChance", 35);

    // --- Snapshot limits -------------------------------------------------
    g_SnapshotIncludeSpells           = sConfigMgr->GetOption<bool>("OllamaChat.Snapshot.IncludeSpells", false);
    g_SnapshotMaxSpells               = sConfigMgr->GetOption<uint32_t>("OllamaChat.Snapshot.MaxSpells", 6);
    g_SnapshotMaxCreatures            = sConfigMgr->GetOption<uint32_t>("OllamaChat.Snapshot.MaxCreatures", 8);
    g_SnapshotMaxObjects              = sConfigMgr->GetOption<uint32_t>("OllamaChat.Snapshot.MaxObjects", 8);
    g_SnapshotMaxPlayers              = sConfigMgr->GetOption<uint32_t>("OllamaChat.Snapshot.MaxPlayers", 8);

    // --- Roleplay --------------------------------------------------------
    g_RoleplayEnable                  = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.Enable", false);
    g_RoleplayStrictness              = static_cast<uint8_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Roleplay.Strictness", 1));
    g_RoleplayUseRaceVoice            = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.UseRaceVoice", true);
    g_RoleplayUseClassVoice           = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.UseClassVoice", true);
    g_RoleplayFactionAttitude         = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.FactionAttitude", true);
    g_RoleplayBlockMetaTerms          = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.BlockMetaTerms", true);
    g_RoleplayMetaTermList            = sConfigMgr->GetOption<std::string>("OllamaChat.Roleplay.MetaTermList",
        "dps|nerf|proc|patch|server|realm|respec|mmo|npc|toon|irl|lfg|afk|noob|meta|aggro|dot|hot|aoe|wipe|hitbox|hotfix|expansion");
    g_RoleplayCrossFactionGibberish   = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.CrossFactionGibberish", true);

    // --- Embodiment ------------------------------------------------------
    g_EnableBotFacing                 = sConfigMgr->GetOption<bool>("OllamaChat.EnableBotFacing", true);
    g_EnableBotEmotes                 = sConfigMgr->GetOption<bool>("OllamaChat.EnableBotEmotes", true);
    g_StateEmoteDurationMs            = sConfigMgr->GetOption<uint32_t>("OllamaChat.StateEmoteDurationMs", 15000);
    g_GestureChance                   = sConfigMgr->GetOption<int>("OllamaChat.GestureChance", 40);

    // Resolved to an id here, on the world thread. The worker that parses a
    // reply must never touch a config string -- reload reassigns them.
    {
        const std::string fallbackName =
            sConfigMgr->GetOption<std::string>("OllamaChat.EmoteFallback", "talk");
        g_EmoteFallbackId = fallbackName.empty() ? 0 : LookupTextEmoteId(fallbackName);

        if (!fallbackName.empty() && g_EmoteFallbackId == 0)
        {
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] OllamaChat.EmoteFallback '{}' is not a known emote name; "
                     "unresolved gesture tags will play nothing.", fallbackName);
        }
    }
    g_BotExpressionDelayMs            = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotExpressionDelayMs", 400);
    g_BotFacingMaxDistance            = sConfigMgr->GetOption<float>("OllamaChat.Facing.MaxDistance", 40.0f);
    g_EnableEmoteReactions            = sConfigMgr->GetOption<bool>("OllamaChat.EnableEmoteReactions", true);
    g_EmoteReplyChance                = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReplyChance", 60);
    g_EmoteReplyMirrorWeight          = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReply.MirrorWeight", 55);
    g_EmoteReplyCounterWeight         = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReply.CounterWeight", 30);
    g_EmoteReplySpeakWeight           = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReply.SpeakWeight", 15);
    g_EmoteReactionCooldownSeconds    = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReply.CooldownSeconds", 20);
    g_EmoteReactionPromptTemplate     = sConfigMgr->GetOption<std::string>("OllamaChat.EmoteReactionPromptTemplate", "");
    if (g_EmoteReactionPromptTemplate.empty())
    {
        g_EmoteReactionPromptTemplate =
            "You are {bot_name}, a level {bot_level} {bot_race} {bot_class}. "
            "Personality: {bot_personality_name}: {bot_personality}. "
            "{player_name}, a {player_race} {player_class}, just did the '{emote_name}' emote at you. "
            "React out loud, in character, in under 12 words. "
            "No narration, no asterisks, no markdown, no emojis.";
    }

    // Typing Simulation
    g_EnableTypingSimulation          = sConfigMgr->GetOption<bool>("OllamaChat.EnableTypingSimulation", false);
    g_TypingSimulationBaseDelay       = sConfigMgr->GetOption<uint32_t>("OllamaChat.TypingSimulationBaseDelay", 1000);
    g_TypingSimulationDelayPerChar    = sConfigMgr->GetOption<uint32_t>("OllamaChat.TypingSimulationDelayPerChar", 250);
    g_TypingSimulationMaxDelay        = sConfigMgr->GetOption<uint32_t>("OllamaChat.TypingSimulationMaxDelay", 8000);

    g_EventTypeDefeated           = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeDefeated", "");
    g_EventTypeDefeatedPlayer     = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeDefeatedPlayer", "");
    g_EventTypePetDefeated        = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypePetDefeated", "");
    g_EventTypeGotItem            = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeGotItem", "");
    g_EventTypeDied               = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeDied", "");
    g_EventTypeCompletedQuest     = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeCompletedQuest", "");
    g_EventTypeLearnedSpell       = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeLearnedSpell", "");
    g_EventTypeRequestedDuel      = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeRequestedDuel", "");
    g_EventTypeStartedDueling     = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeStartedDueling", "");
    g_EventTypeWonDuel            = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeWonDuel", "");
    g_EventTypeLeveledUp          = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeLeveledUp", "");
    g_EventTypeAchievement        = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeAchievement", "");
    g_EventTypeUsedObject         = sConfigMgr->GetOption<std::string>("OllamaChat.EventTypeUsedObject", "");


    // Load extra blacklist commands from config (comma-separated list). With
    // Playerbots.CommandPrefix set, playerbots only reads "!!stay" as a
    // command, so a plain "stay safe" or "go to the inn" is chat again: its
    // commands give way to OllamaChat.BlacklistWithPrefix (the addons), and
    // a line starting with the prefix gets no replies (ProcessChat). Built
    // afresh, so a reload does not add the list again.
    g_BlacklistCommands = { ".playerbots", "playerbot" };
    const bool commandPrefix = !sPlayerbotAIConfig.CommandPrefix.empty();
    std::string extraBlacklist = commandPrefix
        ? sConfigMgr->GetOption<std::string>("OllamaChat.BlacklistWithPrefix",
              "ELVUI_VERSIONCHK,Asked,DPSMate_,LibGroupTalents,BLT,oRA3,Skada,HealBot,hbComms,questie,pfQuest,"
              "DBMv4-Ver,BWVQ3,LHC40,RECOUNT,GTFO_v,Altoholic,DS_,DataStore")
        : sConfigMgr->GetOption<std::string>("OllamaChat.BlacklistCommands", "");
    if (!extraBlacklist.empty())
    {
        std::vector<std::string> extraList = SplitString(extraBlacklist, ',');
        for (const auto& cmd : extraList)
        {
            if (!cmd.empty())
                g_BlacklistCommands.push_back(cmd);
        }
    }

    // Publish the endpoint settings for worker threads. They must never read
    // the g_Ollama* globals directly -- reassigning those here would free the
    // buffers under a worker mid-request.
    OllamaConfig_Publish();

    LoadPersonalityTemplatesFromDB();
    LoadEmoteStyles();
    LoadPersonalityExtras();

    // Loads the environment random chatter message templates for each type.
    // Each config option is a pipe-separated list of string templates,
    // using {} as a placeholder for named substitutions.
    // Helper to load a multi-line config option into a std::vector<std::string>
    auto LoadEnvCommentVector = [](const char* key, const std::vector<std::string>& defaults = {}) -> std::vector<std::string>
    {
        std::string val = sConfigMgr->GetOption<std::string>(key, "");
        std::vector<std::string> result;
        std::istringstream iss(val);
        std::string token;
        while (std::getline(iss, token, '|')) { // Split by '|'
            // Trim whitespace from token
            size_t start = token.find_first_not_of(" \t\r\n");
            size_t end = token.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos)
                result.push_back(token.substr(start, end - start + 1));
        }
        if (result.empty() && !defaults.empty())
            return defaults;
        return result;
    };

    g_EnvCommentCreature        = LoadEnvCommentVector("OllamaChat.EnvCommentCreature", { "" });
    g_EnvCommentGameObject      = LoadEnvCommentVector("OllamaChat.EnvCommentGameObject", { "" });
    g_EnvCommentEquippedItem    = LoadEnvCommentVector("OllamaChat.EnvCommentEquippedItem", { "" });
    g_EnvCommentBagItem         = LoadEnvCommentVector("OllamaChat.EnvCommentBagItem", { "" });
    g_EnvCommentBagItemSell     = LoadEnvCommentVector("OllamaChat.EnvCommentBagItemSell", { "" });
    g_EnvCommentSpell           = LoadEnvCommentVector("OllamaChat.EnvCommentSpell", { "" });
    g_EnvCommentQuestArea       = LoadEnvCommentVector("OllamaChat.EnvCommentQuestArea", { "" });
    g_EnvCommentVendor          = LoadEnvCommentVector("OllamaChat.EnvCommentVendor", { "" });
    g_EnvCommentQuestgiver      = LoadEnvCommentVector("OllamaChat.EnvCommentQuestgiver", { "" });
    g_EnvCommentBagSlots        = LoadEnvCommentVector("OllamaChat.EnvCommentBagSlots", { "" });
    g_EnvCommentDungeon         = LoadEnvCommentVector("OllamaChat.EnvCommentDungeon", { "" });
    g_EnvCommentUnfinishedQuest = LoadEnvCommentVector("OllamaChat.EnvCommentUnfinishedQuest", { "" });

    // Guild-specific random chatter templates
    g_GuildEnvCommentGuildMember = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildMember", { "" });
    g_GuildEnvCommentGuildRank = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildRank", { "" });
    g_GuildEnvCommentGuildBank = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildBank", { "" });
    g_GuildEnvCommentGuildMOTD = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildMOTD", { "" });
    g_GuildEnvCommentGuildInfo = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildInfo", { "" });
    g_GuildEnvCommentGuildOnlineMembers = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildOnlineMembers", { "" });
    g_GuildEnvCommentGuildRaid = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildRaid", { "" });
    g_GuildEnvCommentGuildEndgame = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildEndgame", { "" });
    g_GuildEnvCommentGuildStrategy = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildStrategy", { "" });
    g_GuildEnvCommentGuildGroup = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildGroup", { "" });
    g_GuildEnvCommentGuildPvP = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildPvP", { "" });
    g_GuildEnvCommentGuildCommunity = LoadEnvCommentVector("OllamaChat.GuildEnvCommentGuildCommunity", { "" });

    // --- Topic engine comment pools --------------------------------------
    // These carry built-in defaults so the outward-facing topics work even on
    // a server whose .conf predates them. The old pool had no way at all to
    // talk about the people standing next to the bot.
    g_EnvCommentNearbyPlayer = LoadEnvCommentVector("OllamaChat.EnvCommentNearbyPlayer", {
        "You notice {player_name}, a {player_race} {player_class}, {player_state} nearby. Say something to or about them.",
        "{player_name} the {player_class} is {player_state} close by. React to seeing them.",
        "A level {player_level} {player_class} called {player_name} is nearby. Comment on them.",
        "You size up {player_name}, a {player_race} {player_class}. Say what you make of them.",
        "{player_name} is {player_state} within earshot. Greet or remark on them.",
        "You spot {player_name} and note they are {player_state}. Say something fitting."
    });

    g_EnvCommentGroupMember = LoadEnvCommentVector("OllamaChat.EnvCommentGroupMember", {
        "Your companion {member_name} the {member_class} is {member_state}. Say something to them.",
        "{member_name} is {member_state}. Comment on how your group is doing.",
        "You glance at {member_name}, your {member_class}, and remark on the group's state.",
        "Talk to {member_name} about what the group should do next."
    });

    g_EnvCommentGuildMemberOnline = LoadEnvCommentVector("OllamaChat.EnvCommentGuildMemberOnline", {
        "Your guildmate {member_name} the {member_class} is online. Say something to the guild about them.",
        "Greet {member_name} in guild chat, or ask what they are up to.",
        "Mention {member_name}, level {member_level}, to the guild."
    });

    g_EnvCommentRecentEvent = LoadEnvCommentVector("OllamaChat.EnvCommentRecentEvent", {
        "You just saw this happen: {event_text}. React to it.",
        "Something you witnessed: {event_text}. Comment on it naturally.",
        "Bring up what just happened: {event_text}.",
        "You are still thinking about this: {event_text}. Say something about it."
    });

    g_EnvCommentNamedNpc = LoadEnvCommentVector("OllamaChat.EnvCommentNamedNpc", {
        "{npc_name} the {npc_role} is standing nearby. Say something about them.",
        "You see {npc_name}, a {npc_role}. Remark on what they offer.",
        "Mention {npc_name} the {npc_role} and whether they are worth visiting."
    });

    g_EnvCommentZoneLandmark = LoadEnvCommentVector("OllamaChat.EnvCommentZoneLandmark", {
        "You are in {area_name}, part of {zone_name}. Say something about this place.",
        "Remark on the look or feel of {area_name}.",
        "Comment on what {zone_name} is known for.",
        "Say whether {area_name} is worth lingering in."
    });

    g_EnvCommentGameTalk = LoadEnvCommentVector("OllamaChat.EnvCommentGameTalk", {
        "Something about the game on your mind: {premise}.",
        "Something you could bring up, about the game: {premise}."
    });

    g_EnvCommentMarketListed = LoadEnvCommentVector("OllamaChat.EnvCommentMarketListed", {
        "You just put {item} up on the auction house for {price}. Mention it the way players do in General: that it is up if anyone wants it, or ask what it usually goes for.",
        "{item} is up on the auction house now, yours, for {price}. Say so in passing, or wonder whether you priced it right."
    });

    g_EnvCommentMarketSold = LoadEnvCommentVector("OllamaChat.EnvCommentMarketSold", {
        "Your {item} just sold on the auction house for {price}{to}. Say something about it in passing: pleased it went, or that you should have asked more.",
        "Someone bought your {item} off the auction house for {price}{to}. Mention it the way a player would."
    });

    g_EnvCommentMarketUnsold = LoadEnvCommentVector("OllamaChat.EnvCommentMarketUnsold", {
        "Nobody bought your {item} on the auction house at {price}, and it came back in the mail. Grumble about it, or ask what it is actually worth.",
        "Your {item} came back unsold from the auction house. Say something about the auction house, the way players do."
    });

    g_EnvCommentMarketBought = LoadEnvCommentVector("OllamaChat.EnvCommentMarketBought", {
        "You just bought {item} off the auction house for {price}{from}. Mention it the way a player would: a bargain, a rip-off, or just what you needed.",
        "You got {item} on the auction house for {price}{from}. Say something about it in passing."
    });

    g_EnvCommentTimeOfDay = LoadEnvCommentVector("OllamaChat.EnvCommentTimeOfDay", {
        "It is {time_of_day}. Remark on the hour.",
        "Comment on it being {time_of_day} and what that means for travelling.",
        "Mention that it is {time_of_day} and how you feel about it."
    });

    g_EnvCommentCorpse = LoadEnvCommentVector("OllamaChat.EnvCommentCorpse", {
        "There is a dead {creature_name} on the ground here. React to it.",
        "You see the corpse of a {creature_name}. Say something about the fight that happened.",
        "Remark on the slain {creature_name} nearby."
    });

    g_EnvCommentDanger = LoadEnvCommentVector("OllamaChat.EnvCommentDanger", {
        "A {creature_name} is nearby and it is {level_delta} levels above you. Warn or brag.",
        "The {creature_name} here looks dangerous. Say whether you would take it on.",
        "Comment on the threat the {creature_name} poses."
    });

    g_EnvCommentQuestObjective = LoadEnvCommentVector("OllamaChat.EnvCommentQuestObjective", {
        "You are partway through '{quest_name}' and still need to {objective}. Mention it.",
        "Talk about your task '{quest_name}' and what is left: {objective}.",
        "Bring up '{quest_name}' and ask for help or advice with it.",
        "Complain or enthuse about '{quest_name}'."
    });

    g_EnvCommentGroupNeed = LoadEnvCommentVector("OllamaChat.EnvCommentGroupNeed", {
        "{member_name} is {need}. Say something useful about it.",
        "Point out that {member_name} is {need}.",
        "React to {member_name} being {need}."
    });

    // --- Roleplay-mode variation lists -----------------------------------
    // These replace the shipped out-of-character lists at strictness 2. The
    // defaults ship in-world: no patches, no specs, no forum talk.
    g_RoleplayPromptVariations = LoadEnvCommentVector("OllamaChat.Roleplay.PromptVariations", {
        "Remark on the weather or the road ahead.",
        "Mention a rumour you heard in the last town.",
        "Say something about the war and what it has cost.",
        "Complain about the price of supplies.",
        "Speak of someone you have lost.",
        "Mention where you are headed and why.",
        "Say something about your faith or what you believe in.",
        "Comment on the state of the land here.",
        "Talk about a beast or enemy you have fought recently.",
        "Mention hunger, thirst, or how long since you last slept.",
        "Say something about your homeland.",
        "Remark on how far you are from anywhere safe.",
        "Speak of an old debt or grudge.",
        "Mention a piece of your gear and where it came from.",
        "Say something about the people passing through here."
    });

    g_RoleplayQuestionVariations = LoadEnvCommentVector("OllamaChat.Roleplay.QuestionVariations", {
        "Ask whether the road ahead is safe.",
        "Ask if anyone has news from the front.",
        "Ask where a person might find work here.",
        "Ask what happened in this place.",
        "Ask whether anyone has seen a particular kind of beast nearby.",
        "Ask where the nearest inn or camp is.",
        "Ask someone where they hail from.",
        "Ask whether the local lord or chieftain is worth trusting.",
        "Ask if anyone needs a hand with something.",
        "Ask what the strangest thing seen around here has been.",
        "Ask whether the water or food here can be trusted.",
        "Ask how long the fighting has been going on."
    });

    // Guild-specific configuration
    g_EnableGuildEventChatter = sConfigMgr->GetOption<bool>("OllamaChat.EnableGuildEventChatter", true);
    g_GuildChatterBotCommentChance = sConfigMgr->GetOption<uint32_t>("OllamaChat.GuildChatterBotCommentChance", 25);
    g_GuildChatterMaxBotsPerEvent = sConfigMgr->GetOption<uint32_t>("OllamaChat.GuildChatterMaxBotsPerEvent", 2);

    // Guild-specific event templates
    g_GuildEventTypeLevelUp = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeLevelUp", "");
    g_GuildEventTypeDungeonComplete = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeDungeonComplete", "");
    g_GuildEventTypeEpicGear = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeEpicGear", "");
    g_GuildEventTypeRareGear = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeRareGear", "");
    g_GuildEventTypeGuildJoin = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeGuildJoin", "");
    g_GuildEventTypeGuildLogin = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeGuildLogin", "");
    g_GuildEventTypeGuildLeave = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeGuildLeave", "");
    g_GuildEventTypeGuildPromotion = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeGuildPromotion", "");
    g_GuildEventTypeGuildDemotion = sConfigMgr->GetOption<std::string>("OllamaChat.GuildEventTypeGuildDemotion", "");

    // Load chance variables for normal events
    g_EventTypeDefeated_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeDefeated_Chance", 0);
    g_EventTypeDefeatedPlayer_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeDefeatedPlayer_Chance", 0);
    g_EventTypePetDefeated_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypePetDefeated_Chance", 0);
    g_EventTypeGotItem_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeGotItem_Chance", 0);
    g_EventTypeDied_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeDied_Chance", 0);
    g_EventTypeCompletedQuest_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeCompletedQuest_Chance", 0);
    g_EventTypeLearnedSpell_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeLearnedSpell_Chance", 0);
    g_EventTypeRequestedDuel_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeRequestedDuel_Chance", 0);
    g_EventTypeStartedDueling_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeStartedDueling_Chance", 0);
    g_EventTypeWonDuel_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeWonDuel_Chance", 0);
    g_EventTypeLeveledUp_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeLeveledUp_Chance", 0);
    g_EventTypeAchievement_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeAchievement_Chance", 0);
    g_EventTypeUsedObject_Chance = sConfigMgr->GetOption<int>("OllamaChat.EventTypeUsedObject_Chance", 0);

    // Load chance variables for guild events
    g_GuildEventTypeEpicGear_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeEpicGear_Chance", 0);
    g_GuildEventTypeRareGear_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeRareGear_Chance", 0);
    g_GuildEventTypeGuildJoin_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeGuildJoin_Chance", 0);
    g_GuildEventTypeGuildLogin_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeGuildLogin_Chance", 0);
    g_GuildEventTypeGuildLeave_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeGuildLeave_Chance", 0);
    g_GuildEventTypeGuildPromotion_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeGuildPromotion_Chance", 0);
    g_GuildEventTypeGuildDemotion_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeGuildDemotion_Chance", 0);
    g_GuildEventTypeGuildAchievement_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeGuildAchievement_Chance", 0);
    g_GuildEventTypeLevelUp_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeLevelUp_Chance", 0);
    g_GuildEventTypeDungeonComplete_Chance = sConfigMgr->GetOption<int>("OllamaChat.GuildEventTypeDungeonComplete_Chance", 0);


    // Cooldown time for events
    g_EventCooldownTime = sConfigMgr->GetOption<uint32_t>("OllamaChat.EventCooldownTime", 10);

    // Channel disable settings
    g_DisableForCustomChannels = sConfigMgr->GetOption<bool>("OllamaChat.DisableForCustomChannels", false);
    g_DisableForSayYell = sConfigMgr->GetOption<bool>("OllamaChat.DisableForSayYell", false);
    g_DisableForGuild = sConfigMgr->GetOption<bool>("OllamaChat.DisableForGuild", false);
    g_DisableForParty = sConfigMgr->GetOption<bool>("OllamaChat.DisableForParty", false);

    LOG_INFO("server.loading",
             "[Ollama Chat] Config loaded: Enabled = {}, SayDistance = {}, YellDistance = {}, "
             "Reply Chances - Say: P{}%/B{}%, Channel: P{}%/B{}%, Party: P{}%/B{}%, Guild: P{}%/B{}%, MaxBotsToPick = {}, "
             "Provider = {}, Url = {}, Model = {}, ApiKey = {}, MaxConcurrentQueries = {}, EnableRandomChatter = {}, MinRandInt = {}, MaxRandInt = {}, RandomChatterRealPlayerDistance = {}, "
             "RandomChatterBotCommentChance = {}. MaxConcurrentQueries = {}. Extra blacklist commands: {}",
             g_Enable, g_SayDistance, g_YellDistance,
             g_PlayerReplyChance_Say, g_BotReplyChance_Say,
             g_PlayerReplyChance_Channel, g_BotReplyChance_Channel,
             g_PlayerReplyChance_Party, g_BotReplyChance_Party,
             g_PlayerReplyChance_Guild, g_BotReplyChance_Guild,
             g_MaxBotsToPick,
             OllamaProvider_Name(g_OllamaProvider), g_OllamaUrl, g_OllamaModel,
             g_OllamaApiKey.empty() ? "not set" : "set", g_MaxConcurrentQueries,
             g_EnableRandomChatter, g_MinRandomInterval, g_MaxRandomInterval, g_RandomChatterRealPlayerDistance,
             g_RandomChatterBotCommentChance, g_MaxConcurrentQueries, extraBlacklist);
}

void LoadPersonalityTemplatesFromDB()
{
    g_PersonalityPrompts.clear();
    g_PersonalityKeys.clear();
    g_PersonalityKeysRandomOnly.clear();

    QueryResult result = CharacterDatabase.Query("SELECT `key`, `prompt`, `manual_only` FROM `mod_ollama_chat_personality_templates`");
    if (!result)
    {
        LOG_ERROR("module.ollamachat", "[Ollama Chat] No personality templates found in the database!");
        return;
    }

    do
    {
        std::string key = (*result)[0].Get<std::string>();
        std::string prompt = (*result)[1].Get<std::string>();
        bool manualOnly = (*result)[2].Get<bool>();
        
        g_PersonalityPrompts[key] = prompt;
        g_PersonalityKeys.push_back(key);
        
        // Only add to random pool if not manual_only
        if (!manualOnly)
        {
            g_PersonalityKeysRandomOnly.push_back(key);
        }
    } while (result->NextRow());

    LOG_INFO("module.ollamachat", "[Ollama Chat] Cached {} personalities ({} available for random assignment).", 
             g_PersonalityKeys.size(), g_PersonalityKeysRandomOnly.size());
}

void LoadBotConversationHistoryFromDB()
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, player_guid, player_message, bot_reply FROM mod_ollama_chat_history ORDER BY timestamp ASC"
    );
    if (!result)
        return;

    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
    g_BotConversationHistory.clear();

    do {
        uint64_t botGuid = (*result)[0].Get<uint64_t>();
        uint64_t playerGuid = (*result)[1].Get<uint64_t>();
        std::string playerMsg = (*result)[2].Get<std::string>();
        std::string botReply = (*result)[3].Get<std::string>();

        auto& playerHistory = g_BotConversationHistory[botGuid][playerGuid];
        playerHistory.push_back({ playerMsg, botReply, /*persisted*/ true });
        while (playerHistory.size() > g_MaxConversationHistory)
        {
            playerHistory.pop_front();
        }

    } while (result->NextRow());

}


// Definition of the configuration WorldScript.
OllamaChatConfigWorldScript::OllamaChatConfigWorldScript() : WorldScript("OllamaChatConfigWorldScript") { }

void OllamaChatConfigWorldScript::OnStartup()
{
    LoadOllamaChatConfig();
    OllamaMarket_Init();
    Stuck_Init();
    Marks_Init();
    LoadBotPersonalityList();
    LoadBotConversationHistoryFromDB();
    InitializeSentimentTracking();
    Roleplay_Load();
    Memory_Load();

    // Spread the three periodic saves so they do not all come due on the same
    // world tick. Each is incremental now, but they queue onto one database
    // worker and there is no reason to bunch them.
    const time_t nowSeed = time(nullptr);
    g_LastHistorySaveTime   = nowSeed;
    g_LastMemorySaveTime    = nowSeed + static_cast<time_t>(g_MemorySaveInterval) * 60 / 3;
    g_LastSentimentSaveTime = nowSeed + static_cast<time_t>(g_SentimentSaveInterval) * 60 * 2 / 3;

    // Ask Ollama what this model can actually do. Runs on its own thread so a
    // missing or slow Ollama cannot stall worldserver startup; until it
    // answers, think mode stays off, which is the safe default.
    OllamaCapability_Init(true);

    // Bounded worker pool. Replaces one detached std::thread per bot per
    // message, and it is the only place LLM traffic is throttled now.
    OllamaDispatch_Start();

    // Initialize RAG system if enabled
    if (g_EnableRAG) {
        if (g_RAGSystem) {
            delete g_RAGSystem;
        }
        g_RAGSystem = new OllamaRAGSystem();
        if (!g_RAGSystem->Initialize()) {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] Failed to initialize RAG system");
            delete g_RAGSystem;
            g_RAGSystem = nullptr;
        } else {
            LOG_INFO("module.ollamachat", "[Ollama Chat] RAG system initialized successfully");
        }
    }
}

void OllamaChatConfigWorldScript::OnShutdown()
{
    // Stop accepting work and join the workers before anything they touch goes
    // away. Detached threads had no such guarantee.
    OllamaDispatch_Stop();

    SaveBotConversationHistoryToDB();
    Memory_SaveAll();
    if (g_EnableSentimentTracking)
        SaveBotPlayerSentimentsToDB();

    // Clean up RAG system
    if (g_RAGSystem) {
        delete g_RAGSystem;
        g_RAGSystem = nullptr;
        LOG_INFO("module.ollamachat", "[Ollama Chat] RAG system cleaned up");
    }
}
