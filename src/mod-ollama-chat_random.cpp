#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_conversation.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_presence.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_phrases.h"
#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_marks.h"
#include "mod-ollama-chat_mute.h"
#include "mod-ollama-chat_orders.h"
#include "mod-ollama-chat_partycmd.h"
#include "mod-ollama-chat_revive.h"

#include "CellImpl.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "AiFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <ctime>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    std::mutex g_scheduleMutex;
    std::unordered_map<uint64_t, time_t> g_nextRandomChatTime;

    // urand takes uint32; container sizes are size_t.
    size_t PickIndex(size_t count)
    {
        if (count <= 1)
            return 0;
        return static_cast<size_t>(urand(0, static_cast<uint32>(count - 1)));
    }

    // Decide where an ambient line would go, before spending an LLM call on it.
    bool ChooseDestination(Player* bot, const OllamaWorldSnapshot& world, bool guildTopic,
                           ChatChannelSourceLocal& outSource,
                           std::string& outChannelName,
                           uint32_t& outChannelId)
    {
        outChannelName.clear();
        outChannelId = 0;

        if (guildTopic && bot->GetGuild() && !g_DisableForGuild &&
            world.GuildHasRealPlayer(bot->GetGuildId()))
        {
            outSource = SRC_GUILD_LOCAL;
            return true;
        }

        // Was: any group at all. A party of nothing but bots is not an
        // audience, and this path had no check whatsoever -- a bot that
        // qualified for the tick because a guildmate was online could then
        // spend a generation talking to five other bots.
        if (bot->GetGroup() && !g_DisableForParty && OllamaGroupHasRealPlayer(bot))
        {
            outSource = SRC_PARTY_LOCAL;
            return true;
        }

        struct Option
        {
            ChatChannelSourceLocal source;
            std::string            channelName;
            uint32_t               channelId;
        };
        std::vector<Option> options;

        if (!g_DisableForSayYell && world.RealPlayerWithin(bot, g_SayDistance))
            options.push_back({ SRC_SAY_LOCAL, std::string(), 0 });

        // RealPlayerInZoneAndFaction is only a cheap pre-filter here. Being in
        // the bot's zone is not the same as being in the channel -- players
        // leave General -- so each resolved channel is checked for a real
        // listener below before it becomes a candidate.
        if (!g_DisableForCustomChannels && world.RealPlayerInZoneAndFaction(bot))
        {
            // Resolve real zone/city channels and carry their ACTUAL names
            // forward. Looking up "General" by name never matches (channels are
            // "General - Elwynn Forest"), and using the literal would also put
            // ambient lines in a different governor scope than replies in the
            // same channel.
            //
            // Trade and GuildRecruitment only exist in cities, so they simply
            // resolve to nullptr elsewhere and drop out of the options.
            auto addChannel = [&](uint32_t chanId, bool enabled)
            {
                if (!enabled)
                    return;

                Channel* ch = OllamaResolveZoneChannel(bot, chanId);
                if (!ch)
                    return;

                // The audience test that matters, and it happens here --
                // before any prompt is built and before the LLM is touched.
                if (!world.RealPlayerInChannel(ch))
                    return;

                // A conversation is going on in there: an unrelated line
                // dropped into the middle of it only muddles it.
                const std::string scope = Governor_MakeScopeKey(
                    ChatChannelSourceLocalStr[SRC_GENERAL_LOCAL], ch->GetChannelId(), ch->GetName(), 0,
                    bot->GetZoneId());
                if (Conversation_IsActive(Crowd_KeyFor(scope, SRC_GENERAL_LOCAL, uint32(bot->GetTeamId()))))
                    return;

                options.push_back({ SRC_GENERAL_LOCAL, ch->GetName(), ch->GetChannelId() });
            };

            addChannel(ChatChannelId::GENERAL,           g_ChatterUseGeneralChannel);
            addChannel(ChatChannelId::TRADE,             g_ChatterUseTradeChannel);
            addChannel(ChatChannelId::LOOKING_FOR_GROUP, g_ChatterUseLfgChannel);
            addChannel(ChatChannelId::GUILD_RECRUITMENT, g_ChatterUseGuildRecruitmentChannel);
        }

        if (options.empty())
            return false;

        const Option& chosen = options[PickIndex(options.size())];
        outSource      = chosen.source;
        outChannelName = chosen.channelName;
        outChannelId   = chosen.channelId;
        return true;
    }

    // `opener`: the line opens a conversation about something specific, so it
    // gets no random statement-or-question nudge on top.
    std::string BuildRandomChatterPrompt(Player* bot, const std::string& environmentInfo,
                                         ChatChannelSourceLocal source,
                                         const std::string& channelName,
                                         bool opener = false)
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI || !botAI->GetChatHelper())
            return "";

        const std::string personality       = GetBotPersonality(bot);
        const std::string personalityPrompt = GetPersonalityPromptAddition(personality);

        AreaTableEntry const* area = botAI->GetCurrentArea();
        AreaTableEntry const* zone = botAI->GetCurrentZone();

        // Only when there is a conversation to join is the bot told it may.
        std::string channelHistory = Context_BuildTranscript(bot, source, channelName, 0);
        if (!channelHistory.empty() && !g_ContextHintAmbient.empty())
            channelHistory += g_ContextHintAmbient + "\n\n";

        std::string prompt = SafeFormat(
            g_RandomChatterPromptTemplate,
            fmt::arg("bot_name", bot->GetName()),
            fmt::arg("bot_level", bot->GetLevel()),
            fmt::arg("bot_class", botAI->GetChatHelper()->FormatClass(bot->getClass())),
            fmt::arg("bot_race", botAI->GetChatHelper()->FormatRace(bot->getRace())),
            fmt::arg("bot_gender", bot->getGender() == 0 ? "Male" : "Female"),
            fmt::arg("bot_role", CleanRoleForPrompt(ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot)))),
            fmt::arg("bot_faction", bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde"),
            fmt::arg("bot_area", area ? PlayerbotAI::GetLocalizedAreaName(area) : "UnknownArea"),
            fmt::arg("bot_zone", zone ? PlayerbotAI::GetLocalizedAreaName(zone) : "UnknownZone"),
            fmt::arg("bot_map", OllamaContinentName(bot)),
            fmt::arg("bot_personality", personalityPrompt),
            fmt::arg("bot_personality_name", personality),
            fmt::arg("environment_info", environmentInfo),
            fmt::arg("channel_history", channelHistory),
            fmt::arg("pass_token", g_ContextPassToken));

        // What people were just saying where this line will go, so the bot can
        // join a conversation that is already going instead of talking over it.
        // Older templates without the placeholder get it in front.
        if (!channelHistory.empty() &&
            g_RandomChatterPromptTemplate.find("{channel_history}") == std::string::npos)
        {
            prompt = channelHistory + prompt;
        }

        // Statement or question. At roleplay strictness 2 the in-character
        // lists replace the shipped ones, which otherwise push bots toward
        // out-of-world player-forum talk.
        const std::vector<std::string>& statements =
            Roleplay_UseRoleplayVariations() ? g_RoleplayPromptVariations
                                             : g_RandomChatterPromptVariations;
        const std::vector<std::string>& questions =
            Roleplay_UseRoleplayVariations() ? g_RoleplayQuestionVariations
                                             : g_RandomChatterQuestionVariations;

        const bool haveStatements = !opener && !statements.empty();
        const bool haveQuestions  = !opener && !questions.empty();

        if (haveStatements && haveQuestions)
        {
            const std::vector<std::string>& list =
                (urand(0, 99) < g_RandomChatterQuestionChance) ? questions : statements;
            prompt += " " + list[PickIndex(list.size())];
        }
        else if (haveStatements)
        {
            prompt += " " + statements[PickIndex(statements.size())];
        }
        else if (haveQuestions)
        {
            prompt += " " + questions[PickIndex(questions.size())];
        }

        prompt += Memory_BuildPromptSection(bot, nullptr);
        prompt += Roleplay_BuildVoicePrompt(bot);
        prompt += Outlook_ForBot(bot);
        prompt += Links_PromptHint();
        prompt += Expression_BuildGesturePrompt(bot);

        return prompt;
    }

    // A bot standing within /say range of `bot`, near enough a real player to
    // be heard, for a conversation to start with. nullptr when there is none.
    Player* PickSayPartner(Player* bot, const OllamaWorldSnapshot& world)
    {
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, g_SayDistance, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, g_SayDistance);

        std::vector<Player*> partners;
        for (Player* p : found)
        {
            if (!p || p == bot || !p->IsInWorld() || !OllamaIsBotPlayer(p))
                continue;
            if (p->GetTeamId() != bot->GetTeamId() || (g_DisableRepliesInCombat && p->IsInCombat()))
                continue;
            if (!world.RealPlayerWithin(p, g_SayDistance))
                continue;
            partners.push_back(p);
        }
        if (partners.empty())
            return nullptr;
        return partners[PickIndex(partners.size())];
    }

    // A premise with its placeholders filled in for this bot.
    std::string FormatPremise(const std::string& premise, Player* bot)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        AreaTableEntry const* zone = ai ? ai->GetCurrentZone() : nullptr;
        const bool alliance = bot->GetTeamId() == TEAM_ALLIANCE;

        std::string out = SafeFormat(premise,
                                     fmt::arg("zone", zone ? PlayerbotAI::GetLocalizedAreaName(zone) : "town"),
                                     fmt::arg("faction", alliance ? "the Alliance" : "the Horde"),
                                     fmt::arg("other_faction", alliance ? "the Horde" : "the Alliance"));
        return out == "[Format Error]" ? premise : out;
    }
}

// --------------------------------------------------------------------------

void OllamaRandomChatter_ForgetBot(ObjectGuid botGuid)
{
    std::lock_guard<std::mutex> lock(g_scheduleMutex);
    g_nextRandomChatTime.erase(botGuid.GetRawValue());
}

OllamaBotRandomChatter::OllamaBotRandomChatter() : WorldScript("OllamaBotRandomChatter") { }

void OllamaBotRandomChatter::OnUpdate(uint32 diff)
{
    if (!g_Enable)
        return;

    // The module's world tick. These run before any feature toggle can return
    // early, because pending replies still have to be delivered even when
    // random chatter itself is switched off.
    OllamaDispatch_Update(diff);

    // After delivery, so a crowd sees the line that just landed and can send
    // its next voice in the same tick the pause runs out.
    Crowd_Update();

    // Mages handing over the food and water they were asked for.
    Conjure_Update();

    // Orders carried out a tick after they were given; bots walking over to trade.
    Orders_Update(diff);

    // Which characters are bots, for the stock lines that are muted.
    Mute_Update(diff);

    // Dead bots with nobody to rez them releasing and running back.
    Revive_Update(diff);

    // Raid marks: whose mark is whose, saps on the star, the skull moving on.
    Marks_Update(diff);

    // A bot tank greeting its dungeon group, and calling the finish.
    PartyCmd_Update(diff);

    // After the crowds: a conversation waits for any crowd in its place.
    Conversation_Update();

    // Bots noticing the real players walking past them.
    Presence_Update(diff);

    static uint32 maintenanceTimer = 0;
    if (maintenanceTimer <= diff)
    {
        maintenanceTimer = 30000;
        Governor_Update();
        Topics_Update();
        Context_Update();
        Links_Update();
        Phrases_Update();
    }
    else
    {
        maintenanceTimer -= diff;
    }

    if (g_ConversationHistorySaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastHistorySaveTime) >= g_ConversationHistorySaveInterval * 60)
        {
            SaveBotConversationHistoryToDB();
            g_LastHistorySaveTime = now;
        }
    }

    if (g_MemoryEnable && g_MemorySaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastMemorySaveTime) >= g_MemorySaveInterval * 60)
        {
            Memory_SaveAll();
            g_LastMemorySaveTime = now;
        }
    }

    if (g_EnableSentimentTracking && g_SentimentSaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastSentimentSaveTime) >= g_SentimentSaveInterval * 60)
        {
            SaveBotPlayerSentimentsToDB();
            g_LastSentimentSaveTime = now;
        }
    }

    if (!g_EnableRandomChatter)
        return;

    static uint32 chatterTimer = 0;
    if (chatterTimer <= diff)
    {
        chatterTimer = 30000;
        HandleRandomChatter();
    }
    else
    {
        chatterTimer -= diff;
    }
}

void OllamaBotRandomChatter::HandleRandomChatter()
{
    // One pass for the whole tick. GuildHasRealPlayerOnline() used to be a
    // full player walk per bot, so this was O(bots x players) every 30s.
    OllamaWorldSnapshot world;
    world.Build();

    if (world.Empty())
        return;   // nobody to talk to; do not burn LLM calls on an empty world

    auto const& allPlayers = ObjectAccessor::GetPlayers();

    const time_t now = time(nullptr);

    for (auto const& itr : allPlayers)
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
            continue;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->IsBotAI())
            continue;

        if (g_DisableRepliesInCombat && bot->IsInCombat())
            continue;

        const uint64_t rawGuid = bot->GetGUID().GetRawValue();

        // Guild bots with a guildmate online may talk without anyone standing
        // next to them; everyone else needs an audience in range.
        const bool guildAudience = world.GuildHasRealPlayer(bot->GetGuildId());
        const bool nearRealPlayer =
            world.RealPlayerWithin(bot, g_RandomChatterRealPlayerDistance);

        if (!guildAudience && !nearRealPlayer)
            continue;

        // Schedule.
        {
            std::lock_guard<std::mutex> lock(g_scheduleMutex);
            auto it = g_nextRandomChatTime.find(rawGuid);
            if (it == g_nextRandomChatTime.end())
            {
                g_nextRandomChatTime[rawGuid] = now + urand(g_MinRandomInterval, g_MaxRandomInterval);
                continue;
            }
            if (now < it->second)
                continue;
        }

        auto reschedule = [&]()
        {
            std::lock_guard<std::mutex> lock(g_scheduleMutex);
            g_nextRandomChatTime[rawGuid] = now + urand(g_MinRandomInterval, g_MaxRandomInterval);
        };

        // People come to a city to talk: bots there speak up more often.
        const bool hub = OllamaIsSocialHub(bot);
        uint32_t commentChance = g_RandomChatterBotCommentChance;
        if (hub)
            commentChance = std::min<uint32_t>(100, commentChance * g_CityChatterMultiplier);

        if (urand(0, 99) >= commentChance)
        {
            reschedule();
            continue;
        }

        // Pick what to talk about. Weighted toward the people and the world
        // around the bot rather than its own inventory.
        TopicPick topic = Topics_Pick(bot, world);
        if (!topic.valid)
        {
            reschedule();
            continue;
        }

        ChatChannelSourceLocal source = SRC_SAY_LOCAL;
        std::string channelName;
        uint32_t channelId = 0;
        if (!ChooseDestination(bot, world, topic.isGuildTopic, source, channelName, channelId))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} has nowhere to speak; skipping ambient line.",
                         bot->GetName());
            reschedule();
            continue;
        }

        // Same inputs the reply path uses, so ambient lines and replies in the
        // same channel share one cooldown, rate limit and repetition history.
        // Party scope keys on the group, as ProcessChat does, so ambient lines
        // and replies in one party share a cooldown and repetition history.
        const uint32_t groupOrZone =
            ((source == SRC_PARTY_LOCAL || source == SRC_RAID_LOCAL) && bot->GetGroup())
                ? bot->GetGroup()->GetGUID().GetCounter()
                : bot->GetZoneId();

        const std::string scopeKey = Governor_MakeScopeKey(
            ChatChannelSourceLocalStr[source],
            channelId, channelName,
            source == SRC_GUILD_LOCAL ? bot->GetGuildId() : 0,
            groupOrZone);

        if (!Governor_CanSend(bot->GetGUID(), scopeKey))
        {
            reschedule();
            continue;
        }

        // In a city, the line sometimes opens a conversation about something
        // players argue about, instead of a remark about the surroundings.
        std::string environment = topic.text;
        bool opener = false;
        uint64_t openerTarget = 0;
        std::string openerKey;
        if (hub && !g_ConversationPremises.empty() && g_CityConversationStartChance > 0 &&
            Conversation_PlaceAllowed(source, channelId) &&
            urand(0, 99) < g_CityConversationStartChance)
        {
            const std::string key = Crowd_KeyFor(scopeKey, source, uint32(bot->GetTeamId()));
            if (Conversation_CanOpen(key))
            {
                std::vector<Player*> partners;
                Player* partner = nullptr;
                if (source == SRC_SAY_LOCAL)
                {
                    partner = PickSayPartner(bot, world);
                    if (partner)
                        partners.push_back(partner);
                }

                // Only a premise this bot's level fits: no Ulduar from a
                // level 12 ("[80] getting into Ulduar runs"). /say needs
                // someone to talk to; a channel finds its people once the
                // opening line is out.
                const std::string picked = Outlook_PickPremise(g_ConversationPremises, bot->GetLevel());
                if (!picked.empty() && (source != SRC_SAY_LOCAL || partner))
                {
                    const std::string premise = FormatPremise(picked, bot);
                    const std::string& templ = (source == SRC_SAY_LOCAL) ? g_ConversationHintStartSay
                                                                         : g_ConversationHintStartChannel;
                    const std::string where = (source == SRC_SAY_LOCAL) ? std::string("/say")
                        : (channelId == uint32(ChatChannelId::TRADE) ? std::string("Trade chat")
                                                                     : std::string("General chat"));
                    const std::string start = SafeFormat(templ,
                                                         fmt::arg("premise", premise),
                                                         fmt::arg("where", where),
                                                         fmt::arg("partner", partner ? partner->GetName() : std::string()));
                    if (start != "[Format Error]" && !start.empty() &&
                        Conversation_BeginWithOpener(bot, source, channelName, channelId, scopeKey, key,
                                                     premise, partners))
                    {
                        environment = start;
                        opener = true;
                        openerKey = key;
                        if (partner)
                            openerTarget = partner->GetGUID().GetRawValue();   // faces them as it speaks
                    }
                }
            }
        }

        std::string prompt = BuildRandomChatterPrompt(bot, environment, source, channelName, opener);
        if (prompt.empty())
        {
            if (opener)
                Conversation_OpenerLost(openerKey, rawGuid);
            reschedule();
            continue;
        }

        OllamaChatRequest request;
        request.botGuid     = rawGuid;
        request.targetGuid  = openerTarget;
        request.source      = source;
        request.channelName = channelName;
        request.channelId   = channelId;
        request.chainDepth  = 0;
        request.scopeKey    = scopeKey;
        request.prompt      = std::move(prompt);
        request.botName     = bot->GetName();
        request.kind        = OllamaRequestKind::RandomChatter;
        request.triggerBotReplies = true;
        request.openerKey   = openerKey;

        if (OllamaDispatch_Submit(std::move(request)))
            Topics_NoteUsed(bot->GetGUID(), topic.key);
        else if (opener)
            Conversation_OpenerLost(openerKey, rawGuid);   // the queue is full: free the place again

        reschedule();
    }
}
