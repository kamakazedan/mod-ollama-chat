#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_congrats.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"

#include "AchievementMgr.h"
#include "Channel.h"
#include "DBCStructure.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // A ding (or achievement) and the one thank-you it gets.
    struct Celebration
    {
        std::string       name;               // "Edos"
        std::string       what;               // "20", "Explore Elwynn Forest"
        Clock::time_point when{};

        bool              thanked = false;    // it has said its "ty"
        bool              pending = false;    // ...or is about to
        Clock::time_point due{};
        ChatChannelSourceLocal source = SRC_PARTY_LOCAL;   // where the first gz was
        std::string       channelName;
        uint32_t          channelId = 0;
        std::string       scopeKey;
        uint64_t          firstFrom = 0;
        std::string       firstName;
        uint32_t          from = 0;           // gz heard so far
    };

    // Dings are noted on map threads; everything else is the world thread.
    std::mutex g_mutex;
    std::unordered_map<uint64_t, Celebration> g_celebrations;
    CongratsStats g_stats;

    // A ding and its "Level 20" achievement land together: one celebration.
    constexpr auto kSameMoment = std::chrono::seconds(10);

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    bool Lower(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // Can `player`'s line in `source` be read by `other`?
    bool Hears(Player* player, Player* other, ChatChannelSourceLocal source, Player* receiver)
    {
        if (!other || !other->IsInWorld())
            return false;
        switch (source)
        {
            case SRC_PARTY_LOCAL:
            case SRC_RAID_LOCAL:
                return player->GetGroup() && player->GetGroup()->IsMember(other->GetGUID());
            case SRC_GUILD_LOCAL:
            case SRC_OFFICER_LOCAL:
                return player->GetGuildId() && other->GetGuildId() == player->GetGuildId();
            case SRC_SAY_LOCAL:
            case SRC_YELL_LOCAL:
                return other->GetMap() == player->GetMap() &&
                       other->IsWithinDist(player, std::max(g_SayDistance, 25.0f) * (source == SRC_YELL_LOCAL ? 3.0f : 1.0f), false);
            case SRC_WHISPER_LOCAL:
                return other == receiver;
            default:
                return false;   // General and the like: only the one it names
        }
    }

    std::string Pick(const std::vector<std::string>& lines, const std::string& name)
    {
        std::vector<std::string> usable;
        for (const std::string& l : lines)
            if (!l.empty() && (!name.empty() || l.find("{name}") == std::string::npos))
                usable.push_back(l);
        if (usable.empty())
            return {};
        const std::string& raw = usable[std::uniform_int_distribution<size_t>(0, usable.size() - 1)(Rng())];
        return TextRules::FormatCannedLine(raw, name, Rng());
    }

    // The first gz starts the clock on the one thank-you; later ones only
    // count, so it can be "ty all". Under g_mutex.
    void Heard(Celebration& c, ChatChannelSourceLocal source, const std::string& channelName, uint32_t channelId,
               const std::string& scopeKey, uint64_t fromGuid, const std::string& fromName, Clock::time_point now)
    {
        ++c.from;
        if (c.thanked)
        {
            ++g_stats.repeat;
            return;
        }
        if (c.pending)
        {
            // A whisper first, then the group too: "ty all" belongs where the
            // group can read it, not in the whisper.
            if (c.source == SRC_WHISPER_LOCAL && source != SRC_WHISPER_LOCAL)
            {
                c.source      = source;
                c.channelName = channelName;
                c.channelId   = channelId;
                c.scopeKey    = scopeKey;
            }
            return;
        }
        const uint32_t lo = std::min(g_CongratsThanksDelayMinMs, g_CongratsThanksDelayMaxMs);
        const uint32_t hi = std::max(g_CongratsThanksDelayMinMs, g_CongratsThanksDelayMaxMs);
        c.pending     = true;
        c.due         = now + std::chrono::milliseconds(std::uniform_int_distribution<uint32_t>(lo, hi)(Rng()));
        c.source      = source;
        c.channelName = channelName;
        c.channelId   = channelId;
        c.scopeKey    = scopeKey;
        c.firstFrom   = fromGuid;
        c.firstName   = fromName;
    }
}

void Congrats_NoteCelebrated(Player* who, const std::string& what)
{
    if (!g_CongratsEnable || !who)
        return;
    const uint64_t guid = who->GetGUID().GetRawValue();
    const std::string name = who->GetName();
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    Celebration& c = g_celebrations[guid];
    if (!c.name.empty() && now - c.when < kSameMoment)
        return;   // the achievement that came with the ding
    c = Celebration{};
    c.name = name;
    c.what = what;
    c.when = now;
    ++g_stats.celebrated;
}

bool Congrats_AchievementWorthIt(AchievementEntry const* achievement)
{
    if (!achievement)
        return false;
    bool worth = !g_CongratsAchievementsRareOnly;
    if (!worth)
    {
        constexpr uint32_t kFeatsOfStrength = 81;   // the achievement category
        if (g_CongratsAchievementAlways.contains(achievement->ID))
            worth = true;
        else if (g_CongratsAchievementNever.contains(achievement->ID))
            worth = false;
        else if ((achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL)) ||
                 achievement->categoryId == kFeatsOfStrength)
            worth = true;
        else
        {
            // What it is made of: a level ("Level 40" -- the ding has its own gz),
            // or nothing but other achievements (a meta: Glory of the Hero).
            const uint32_t criteriaOf = achievement->refAchievement ? achievement->refAchievement : achievement->ID;
            AchievementCriteriaEntryList const* criteria = sAchievementMgr->GetAchievementCriteriaByAchievement(criteriaOf);
            bool level = false, meta = criteria && !criteria->empty();
            if (criteria)
                for (AchievementCriteriaEntry const* c : *criteria)
                {
                    level = level || c->requiredType == ACHIEVEMENT_CRITERIA_TYPE_REACH_LEVEL;
                    meta  = meta && c->requiredType == ACHIEVEMENT_CRITERIA_TYPE_COMPLETE_ACHIEVEMENT;
                }
            if (level)
                worth = false;
            else
                worth = meta || (g_CongratsAchievementRarePoints && achievement->points >= g_CongratsAchievementRarePoints) ||
                        sAchievementMgr->GetAchievementReward(achievement) != nullptr;   // a title, mount, pet, tabard
        }
    }
    if (!worth)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_stats.common;
    }
    return worth;
}

bool Congrats_GuildHearsDing(uint32_t level)
{
    if (level < g_CongratsGuildLevels.size() && g_CongratsGuildLevels.test(level))
        return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats.guildQuiet;
    return false;
}

bool Congrats_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                              Channel* channel, Player* receiver, const std::string& scopeKey)
{
    if (!g_CongratsEnable || !player || msg.empty() || OllamaIsBotPlayer(player))
        return false;
    const std::vector<std::string> words = CrowdText::Words(Context_CleanText(msg));
    if (words.empty() || !CrowdText::ContainsPhrase(words, g_CongratsWords))
        return false;

    const auto now = Clock::now();
    const auto window = std::chrono::seconds(g_CongratsWindowSeconds);

    // Who it could be for, by name: the group, whoever it was whispered to,
    // and whoever dinged lately.
    struct Who
    {
        uint64_t    guid = 0;
        std::string name;
    };
    std::vector<Who> people;
    auto add = [&](Player* p)
    {
        if (p && p != player)
            people.push_back({ p->GetGUID().GetRawValue(), p->GetName() });
    };
    if (Group* group = player->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            add(ref->GetSource());
    add(receiver);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [guid, c] : g_celebrations)
            if (now - c.when <= window && guid != player->GetGUID().GetRawValue())
                people.push_back({ guid, c.name });
    }

    // Nothing but the congratulation, names, numbers ("grats on 20") and the
    // words around them -- "gz, now lets go" is more than that, and an
    // ordinary line.
    uint64_t named = 0;
    auto isName = [&](const std::string& w)
    {
        for (const Who& p : people)
            for (const std::string& part : CrowdText::Words(p.name))
                if (part == w)
                {
                    named = named ? named : p.guid;
                    return true;
                }
        return false;
    };
    if (!TextRules::IsOnlyCongrats(words, g_CongratsWords, g_CongratsFillerWords, g_CrowdAckWords, isName))
        return false;

    // For whom it names -- someone who can read it: in General, on that
    // channel -- else for whoever dinged last among those reading it.
    Player* whom = named ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(named)) : nullptr;
    if (whom && !(source == SRC_GENERAL_LOCAL ? (channel && OllamaIsOnChannel(channel, whom->GetGUID()))
                                              : Hears(player, whom, source, receiver)))
        whom = nullptr;
    if (!whom && !named)
    {
        std::vector<std::pair<Clock::time_point, uint64_t>> recent;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto& [guid, c] : g_celebrations)
                if (now - c.when <= window && guid != player->GetGUID().GetRawValue())
                    recent.emplace_back(c.when, guid);
        }
        std::sort(recent.begin(), recent.end(), std::greater<>());
        for (const auto& [when, guid] : recent)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            if (p && Hears(player, p, source, receiver))
            {
                whom = p;
                break;
            }
        }
    }

    if (!whom || !whom->IsInWorld() || !OllamaIsBotPlayer(whom))
    {
        // A gz for nobody in particular, or for a person: no bot answers it.
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_stats.nobody;
        }
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {}'s congratulation '{}' is for {}: no bot answers it.",
                     player->GetName(), Context_CleanText(msg), whom ? whom->GetName() : "nobody in particular");
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_stats.heard;
        Celebration& c = g_celebrations[whom->GetGUID().GetRawValue()];
        if (c.name.empty() || (!c.pending && now - c.when > window))
        {
            // Named, with nothing noted lately ("gz on the drop Edos"): one
            // thank-you for it all the same.
            c = Celebration{};
            c.name = whom->GetName();
            c.when = now;
        }
        Heard(c, source, channel ? channel->GetName() : std::string(), channel ? channel->GetChannelId() : 0,
              scopeKey, player->GetGUID().GetRawValue(), player->GetName(), now);
    }
    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {}'s congratulation is for {}: only it answers, once.",
                 player->GetName(), whom->GetName());
    return true;
}

void Congrats_NoteBotCongrats(Player* bot, const std::string& name, ChatChannelSourceLocal source,
                              const std::string& channelName, uint32_t channelId, const std::string& scopeKey)
{
    if (!g_CongratsEnable || !bot || name.empty())
        return;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [guid, c] : g_celebrations)
    {
        if (!Lower(c.name, name) || now - c.when > std::chrono::seconds(g_CongratsWindowSeconds))
            continue;
        if (guid != bot->GetGUID().GetRawValue())
            Heard(c, source, channelName, channelId, scopeKey, bot->GetGUID().GetRawValue(), bot->GetName(), now);
        return;
    }
}

void Congrats_Tick()
{
    struct Due
    {
        uint64_t    guid = 0;
        Celebration c;
    };
    std::vector<Due> due;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_celebrations.empty())
            return;
        const auto now = Clock::now();
        for (auto& [guid, c] : g_celebrations)
        {
            if (!c.pending || now < c.due)
                continue;
            c.pending = false;
            c.thanked = true;
            due.push_back({ guid, c });
        }
    }

    for (const Due& d : due)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(d.guid));
        if (!bot || !bot->IsInWorld() || !OllamaIsBotPlayer(bot))
            continue;
        // A plain "ty" would break character in roleplay mode.
        if (g_RoleplayEnable || g_CongratsThanksChance == 0 || urand(1, 100) > g_CongratsThanksChance)
            continue;

        // "ty all" after several -- but a whisper is to one person: a plain
        // "ty" back to whoever whispered first.
        const bool whisper = d.c.source == SRC_WHISPER_LOCAL;
        const bool many = d.c.from > 1 && !whisper;
        const std::string line = Personality_StyleCanned(
            bot, many ? Pick(g_CongratsThanksAllLines, {}) : Pick(g_CongratsThanksLines, d.c.firstName));
        if (line.empty())
            continue;

        OllamaChatRequest request;
        request.botGuid           = d.guid;
        request.targetGuid        = many ? 0 : d.c.firstFrom;
        request.source            = d.c.source;
        request.channelName       = d.c.channelName;
        request.channelId         = d.c.channelId;
        request.scopeKey          = d.c.scopeKey;
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.fixedText         = line;
        request.directAddress     = true;    // owed: the pacing cooldowns do not hold it back
        request.triggerBotReplies = false;   // "ty" -> "np" -> "..." is how a chain starts
        if (!OllamaDispatch_Submit(std::move(request)))
            continue;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_stats.thanked;
        }
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} thanks for the {} gz it got: '{}'.", bot->GetName(),
                     d.c.from, line);
    }
}

void Congrats_Update()
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_celebrations.begin(); it != g_celebrations.end();)
        it = (!it->second.pending && now - it->second.when > std::chrono::seconds(g_CongratsWindowSeconds) * 2)
                 ? g_celebrations.erase(it)
                 : std::next(it);
}

CongratsStats Congrats_GetStats()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stats;
}
