#include "mod-ollama-chat_citylife.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_outlook.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"

#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace
{
    CityLifeStats g_stats;

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 5
    using PlayerbotsPlus::BotEventType;

    // Who came over to look, `n` of them counting the one saying it.
    std::string Who(uint32_t n)
    {
        if (n <= 1)
            return "you";
        if (n == 2)
            return "you and one other person";
        return "you and " + std::to_string(n - 1) + " others";
    }

    // A few hanging out came over to look at someone's rare mount: the first
    // of them says something to the rider about it.
    void MountShown(PlayerbotsPlus::BotEvent const& event)
    {
        ++g_stats.mounts;
        if (!g_Enable || !g_CityLifeEnable || g_DisableForSayYell || g_CityLifeMountPrompt.empty() ||
            event.what.empty())
        {
            ++g_stats.quiet;
            return;
        }

        Player* bot   = ObjectAccessor::FindPlayer(event.bot);
        Player* rider = ObjectAccessor::FindPlayer(event.other);
        PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !rider || bot == rider || !bot->IsInWorld() || !rider->IsInWorld() ||
            bot->GetMap() != rider->GetMap() || !ai || !ai->GetChatHelper())
        {
            ++g_stats.quiet;
            return;
        }

        // The other side's /say arrives as gibberish (a neutral city).
        // A line nobody asked for: not over someone real who has just spoken
        // nearby. And the usual pacing of /say there.
        std::string const scopeKey =
            Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, "", 0, bot->GetZoneId());
        if (Roleplay_IsLanguageBarrier(bot, rider) ||
            (g_ContextAmbientHoldSeconds &&
             Context_HumanSpokeSince(bot, SRC_SAY_LOCAL, std::string(), 0, g_ContextAmbientHoldSeconds)) ||
            !Governor_CanSend(bot->GetGUID(), scopeKey))
        {
            ++g_stats.quiet;
            return;
        }

        std::string prompt = SafeFormat(g_CityLifeMountPrompt,
                                        fmt::arg("bot_name", bot->GetName()),
                                        fmt::arg("bot_level", bot->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
                                        fmt::arg("bot_personality",
                                                 GetPersonalityPromptAddition(GetBotPersonality(bot))),
                                        fmt::arg("rider_name", rider->GetName()),
                                        fmt::arg("mount", event.what),
                                        fmt::arg("rarity", event.detail),
                                        fmt::arg("who", Who(event.have)),
                                        fmt::arg("zone", event.zone));
        if (prompt.empty() || prompt == "[Format Error]")
        {
            ++g_stats.quiet;
            return;
        }
        prompt += Roleplay_BuildVoicePrompt(bot);
        prompt += Outlook_ForBot(bot);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = rider->GetGUID().GetRawValue();
        request.source            = SRC_SAY_LOCAL;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = true;   // the rider, or another who came over, may answer
        request.gentle            = true;   // a stranger, as often as not: nothing rude goes with it
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        // Nobody asked for it: a real player who speaks before it is out was
        // not heard by it.
        request.writtenAtSeq      = Context_NextSeq();
        // Once it has walked over and had a look.
        request.extraDelayMs      = urand(1500, 4000);
        if (!OllamaDispatch_Submit(std::move(request)))
        {
            ++g_stats.quiet;
            return;
        }
        ++g_stats.said;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] City life: {} to say something about {}'s {} ({}).",
                     bot->GetName(), rider->GetName(), event.what, event.detail);
    }

    // A bot started dancing (nobody with it), or joined someone dancing.
    void Danced(PlayerbotsPlus::BotEvent const& event)
    {
        ++g_stats.dances;
        if (!g_Enable || !g_CityLifeEnable || !g_CityLifeDanceEmotes || !g_EnableBotEmotes)
            return;
        Player* bot = ObjectAccessor::FindPlayer(event.bot);
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            return;
        Player* with = nullptr;
        if (event.other)
        {
            with = ObjectAccessor::FindPlayer(event.other);
            if (!with || with == bot || !with->IsInWorld() || with->GetMap() != bot->GetMap())
                return;   // gone already: plus ends the dance
        }
        // The dance is plus's, and so is its end: the emote says it and
        // leaves it on.
        BotPlayTextEmote(bot, TEXT_EMOTE_DANCE, with ? with->GetGUID() : ObjectGuid::Empty, true);
        if (with)
            Expression_NoteEmote(bot, with, TEXT_EMOTE_DANCE);   // "why are you dancing with me?" is its to answer
        ++g_stats.emoted;
    }

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 6
    // Which of mod-playerbots-plus's temperaments a bot is: its chat style's
    // (CityLife.Temperaments), "default" for a style with none. Nothing --
    // plus picks one by its own shares -- with chat styles off.
    std::string TemperamentOf(Player* bot)
    {
        if (!bot || !g_Enable || !g_CityLifeEnable || !g_EnableRPPersonalities || g_CityLifeTemperaments.empty())
            return {};
        auto it = g_CityLifeTemperaments.find(GetBotPersonality(bot));
        return it == g_CityLifeTemperaments.end() ? std::string("default") : it->second;
    }

    // What the show-off has been doing, said to a prompt.
    std::string ShowingOff(std::string const& verb, std::string const& mount, bool own)
    {
        if (verb == "hop")
            return own ? "hopping up and down on the spot" : "hopping up and down on the spot right by you";
        std::string const what = mount.empty() ? std::string(own ? "your mount" : "their mount")
                                               : std::string(own ? "your " : "their ") + mount;
        return "making " + what + " rear up and roar";
    }

    std::string Pick(std::vector<std::string> const& lines)
    {
        return lines.empty() ? std::string() : lines[urand(0, uint32(lines.size() - 1))];
    }

    // Someone near `show` it gets on the nerves of, who could say so: a bot
    // of a chat style that minds it (CityLife.ShowOffAnnoys), in sight, up and
    // about, of a side that can understand it, not held back by pacing.
    Player* Annoyed(Player* show, std::string const& scopeKey)
    {
        std::list<Player*> near;
        Acore::AnyPlayerInObjectRangeCheck check(show, g_CityLifeShowOffRange);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(show, near, check);
        Cell::VisitObjects(show, searcher, g_CityLifeShowOffRange);
        std::vector<Player*> could;
        for (Player* p : near)
        {
            if (p == show || !p->IsInWorld() || !p->IsAlive() || p->IsInCombat() || p->isAFK() ||
                !OllamaIsBotPlayer(p) || Roleplay_IsLanguageBarrier(p, show) || !p->IsWithinLOSInMap(show))
                continue;
            std::string const style = GetBotPersonality(p);
            if (std::find(g_CityLifeShowOffAnnoys.begin(), g_CityLifeShowOffAnnoys.end(), style) ==
                    g_CityLifeShowOffAnnoys.end() ||
                !Governor_CanSend(p->GetGUID(), scopeKey))
                continue;
            could.push_back(p);
        }
        return could.empty() ? nullptr : could[urand(0, uint32(could.size() - 1))];
    }

    // One near a real player has been showing off a while: someone near it
    // whose style it gets on the nerves of may tell it to pack it in.
    void ShowOff(PlayerbotsPlus::BotEvent const& event)
    {
        ++g_stats.showOffs;
        if (!g_Enable || !g_CityLifeEnable || g_DisableForSayYell || g_CityLifeShowOffPrompt.empty() ||
            g_CityLifeShowOffAnnoys.empty() || urand(1, 100) > g_CityLifeShowOffChance)
            return;
        Player* show = ObjectAccessor::FindPlayer(event.bot);
        if (!show || !show->IsInWorld())
            return;
        // A line nobody asked for: not over someone real who has just spoken
        // nearby.
        std::string const scopeKey =
            Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, "", 0, show->GetZoneId());
        if (g_ContextAmbientHoldSeconds &&
            Context_HumanSpokeSince(show, SRC_SAY_LOCAL, std::string(), 0, g_ContextAmbientHoldSeconds))
            return;
        Player* teller = Annoyed(show, scopeKey);
        PlayerbotAI* ai = teller ? PlayerbotsMgr::instance().GetPlayerbotAI(teller) : nullptr;
        if (!teller || !ai || !ai->GetChatHelper())
            return;

        std::string prompt = SafeFormat(g_CityLifeShowOffPrompt,
                                        fmt::arg("bot_name", teller->GetName()),
                                        fmt::arg("bot_level", teller->GetLevel()),
                                        fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(teller->getRace())),
                                        fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(teller->getClass())),
                                        fmt::arg("bot_personality",
                                                 GetPersonalityPromptAddition(GetBotPersonality(teller))),
                                        fmt::arg("show_name", show->GetName()),
                                        fmt::arg("doing", ShowingOff(event.verb, event.what, false)),
                                        fmt::arg("count", event.have),
                                        fmt::arg("zone", event.zone));
        if (prompt.empty() || prompt == "[Format Error]")
            return;
        prompt += Roleplay_BuildVoicePrompt(teller);
        prompt += Outlook_ForBot(teller);

        OllamaChatRequest request;
        request.botGuid           = teller->GetGUID().GetRawValue();
        request.targetGuid        = show->GetGUID().GetRawValue();
        request.source            = SRC_SAY_LOCAL;
        request.chainDepth        = 0;
        request.scopeKey          = scopeKey;
        request.prompt            = std::move(prompt);
        request.botName           = teller->GetName();
        request.kind              = OllamaRequestKind::EventChatter;
        request.triggerBotReplies = false;   // the show-off's answer is its own (CityLife_NoteToldOff)
        request.maxReplyLength    = OllamaMaxReplyLengthFor(teller);
        request.writtenAtSeq      = Context_NextSeq();
        request.extraDelayMs      = urand(500, 2000);
        request.fallbackText      = Personality_StyleCanned(teller, Pick(g_CityLifeShowOffLines));
        request.showOffTold       = show->GetGUID().GetRawValue();
        if (event.verb == "hop")   // on foot: "nice mount" is about nothing there
            request.mustNotSay = g_PetsMountWords;
        if (!OllamaDispatch_Submit(std::move(request)))
            return;
        ++g_stats.toldOff;
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] City life: {} to tell {} to stop showing off ({} times).",
                     teller->GetName(), show->GetName(), event.have);
    }
#endif

    void Heard(PlayerbotsPlus::BotEvent const& event)
    {
        switch (event.type)
        {
            case BotEventType::CityMountShown: MountShown(event); break;
            case BotEventType::CityDance:      Danced(event);     break;
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 6
            case BotEventType::CityShowOff:    ShowOff(event);    break;
#endif
            default:                                              break;
        }
    }
#endif
}

void CityLife_Init()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 5
    PlayerbotsPlus::AddBotEventListener(Heard);
#endif
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 6
    PlayerbotsPlus::SetTemperamentProvider(TemperamentOf);
#endif
}

void CityLife_NoteToldOff(Player* teller, uint64_t showOffGuid)
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 6
    Player* show = ObjectAccessor::FindPlayer(ObjectGuid(showOffGuid));
    if (!teller || !show || show == teller || !show->IsInWorld() || show->GetMap() != teller->GetMap())
        return;
    // It laughs it off, looking at them -- and stops (now and then with one
    // last go, for cheek: plus's).
    if (g_EnableBotFacing && IsSafeForFacing(show))
        show->SetFacingToObject(teller);
    if (g_EnableBotEmotes)
    {
        BotPlayTextEmote(show, TEXT_EMOTE_LAUGH, teller->GetGUID());
        Expression_NoteEmote(show, teller, TEXT_EMOTE_LAUGH);
    }
    PlayerbotsPlus::CityCalmDown(show);
    ++g_stats.laughed;

    // Now and then, in words too.
    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(show);
    if (!g_Enable || g_CityLifeShowOffAnswerPrompt.empty() || !ai || !ai->GetChatHelper() ||
        urand(1, 100) > g_CityLifeShowOffAnswerChance)
        return;
    std::string const scopeKey =
        Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_SAY_LOCAL], 0, "", 0, show->GetZoneId());
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(show->GetZoneId());
    std::string prompt = SafeFormat(g_CityLifeShowOffAnswerPrompt,
                                    fmt::arg("bot_name", show->GetName()),
                                    fmt::arg("bot_level", show->GetLevel()),
                                    fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(show->getRace())),
                                    fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(show->getClass())),
                                    fmt::arg("bot_personality", GetPersonalityPromptAddition(GetBotPersonality(show))),
                                    fmt::arg("teller_name", teller->GetName()),
                                    fmt::arg("doing", ShowingOff(show->IsMounted() ? "rear" : "hop", std::string(), true)),
                                    fmt::arg("zone", zone ? ai->GetLocalizedAreaName(zone) : std::string("the city")));
    if (prompt.empty() || prompt == "[Format Error]")
        return;
    prompt += Roleplay_BuildVoicePrompt(show);

    OllamaChatRequest request;
    request.botGuid           = show->GetGUID().GetRawValue();
    request.targetGuid        = teller->GetGUID().GetRawValue();
    request.source            = SRC_SAY_LOCAL;
    request.chainDepth        = 1;
    request.scopeKey          = scopeKey;
    request.prompt            = std::move(prompt);
    request.botName           = show->GetName();
    request.kind              = OllamaRequestKind::ChatReply;
    request.directAddress     = true;    // told off to its face: it answers, cooldowns or not
    request.triggerBotReplies = false;
    request.maxReplyLength    = OllamaMaxReplyLengthFor(show);
    request.extraDelayMs      = urand(1200, 3000);
    request.fallbackText      = Personality_StyleCanned(show, Pick(g_CityLifeShowOffAnswerLines));
    if (!show->IsMounted())
        request.mustNotSay = g_PetsMountWords;
    if (OllamaDispatch_Submit(std::move(request)))
        ++g_stats.answered;
#else
    (void)teller;
    (void)showOffGuid;
#endif
}

CityLifeStats CityLife_GetStats()
{
    return g_stats;
}
