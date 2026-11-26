#include "mod-ollama-chat_goodturn.h"
#include "mod-ollama-chat_buffs.h"
#include "mod-ollama-chat_citysay.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"

#include "Corpse.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    enum class Kind : uint8_t { Buff, Heal, Rez };

    // What the hook saw.
    struct Noted
    {
        uint64_t giver = 0;
        uint64_t bot   = 0;
        uint32_t spell = 0;
        Kind     kind  = Kind::Buff;
    };
    std::mutex         g_mutex;
    std::vector<Noted> g_noted;   // under g_mutex
    constexpr size_t   kMaxNoted = 512;

    // A thank-you on its way.
    struct Pending
    {
        uint64_t          giver = 0;
        uint64_t          bot   = 0;
        uint32_t          spell = 0;
        Kind              kind  = Kind::Buff;
        Clock::time_point due{};
        Clock::time_point until{};   // not ready by then (dead still, fighting): let go
    };
    std::vector<Pending> g_pending;
    std::map<std::pair<uint64_t, uint64_t>, Clock::time_point> g_last;   // (bot, giver): when it last weighed one
    GoodTurnStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    bool Chance(uint32_t percent)
    {
        return percent > 0 && urand(1, 100) <= percent;
    }

    std::string Pick(const std::vector<std::string>& lines, const std::string& name)
    {
        std::vector<std::string> usable;
        for (const std::string& l : lines)
            if (!l.empty())
                usable.push_back(l);
        if (usable.empty())
            return {};
        const std::string& raw = usable[std::uniform_int_distribution<size_t>(0, usable.size() - 1)(Rng())];
        return TextRules::FormatCannedLine(raw, name, Rng());
    }

    void Act(Player* bot, Player* giver, const Pending& p)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(p.spell);
        const std::string what = info && info->SpellName[0] ? info->SpellName[0] : "a spell";
        Group* group = bot->GetGroup();
        const bool together = group && group == giver->GetGroup();

        if (g_EnableBotFacing && IsSafeForFacing(bot) && !bot->isMoving())
            bot->SetFacingToObject(giver);
        if (g_EnableBotEmotes)
        {
            BotPlayTextEmote(bot, TEXT_EMOTE_THANK, giver->GetGUID());
            Expression_NoteEmote(bot, giver, TEXT_EMOTE_THANK);
            ++g_stats.thanked;
        }

        // A word as well, now and then: typed, in its own chat style. In a
        // city's streets only when someone there may be spoken to out loud
        // just now (mod-ollama-chat_citysay); the emote is said either way.
        const ChatChannelSourceLocal source = !together             ? SRC_SAY_LOCAL
                                              : group->isRaidGroup() ? SRC_RAID_LOCAL
                                                                     : SRC_PARTY_LOCAL;
        const bool muted = source == SRC_SAY_LOCAL ? g_DisableForSayYell : g_DisableForParty;
        if (!g_RoleplayEnable && !muted && Chance(g_GoodTurnSayChance) &&
            (source != SRC_SAY_LOCAL || !OllamaInCity(bot) || CitySay_Claim(bot)))
        {
            const std::string line = Personality_StyleCanned(
                bot, Pick(p.kind == Kind::Rez ? g_GoodTurnRezLines
                          : p.kind == Kind::Heal ? g_GoodTurnHealLines : g_GoodTurnBuffLines,
                          giver->GetName()));
            if (!line.empty())
            {
                OllamaChatRequest request;
                request.botGuid           = bot->GetGUID().GetRawValue();
                request.targetGuid        = giver->GetGUID().GetRawValue();
                request.source            = source;
                request.scopeKey          = Governor_MakeScopeKey(
                    ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                    together ? group->GetGUID().GetCounter() : bot->GetZoneId());
                request.botName           = bot->GetName();
                request.kind              = OllamaRequestKind::EventChatter;
                request.fixedText         = line;
                request.directAddress     = true;    // owed: the pacing cooldowns do not hold it back
                request.noEcho            = true;    // "ty" is what everyone says
                request.triggerBotReplies = false;   // "ty" -> "np" -> "..." is how a chain starts
                request.extraDelayMs      = urand(500, 1500);
                // Asked "what for?", it knows (mod-ollama-chat_said).
                request.basis = fmt::format("you thanked {} for the {} they cast on you", giver->GetName(), what);
                if (OllamaDispatch_Submit(std::move(request)))
                    ++g_stats.said;
            }
        }

        // One good turn deserves another: its own buff, when it has one they
        // are without. Not in its own group, which playerbots keeps buffed.
        bool buffed = false;
        if (!together && Chance(g_GoodTurnBuffBackChance) && Buff_GiveBack(bot, giver))
        {
            buffed = true;
            ++g_stats.buffed;
        }
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} thanks {} for {}{}.", bot->GetName(), giver->GetName(),
                     what, buffed ? ", and buffs them back" : "");
    }
}

void OllamaChatGoodTurnScript::OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/)
{
    // A map thread: read the cast and who it is on, note it, nothing more.
    if (!g_Enable || !g_GoodTurnEnable || !player || !spell || !player->IsInWorld() || OllamaIsBotPlayer(player))
        return;
    SpellInfo const* info = spell->GetSpellInfo();
    if (!info || spell->IsTriggered() || info->IsPassive() || !info->IsPositive())
        return;

    Kind kind = Kind::Buff;
    if (info->HasEffect(SPELL_EFFECT_RESURRECT) || info->HasEffect(SPELL_EFFECT_RESURRECT_NEW))
        kind = Kind::Rez;
    else if (info->HasEffect(SPELL_EFFECT_HEAL) || info->HasEffect(SPELL_EFFECT_HEAL_MAX_HEALTH) ||
             info->HasAura(SPELL_AURA_PERIODIC_HEAL))
        kind = Kind::Heal;
    else if (info->HasAreaAuraEffect() || info->GetDuration() < int32(g_GoodTurnBuffMinMinutes) * 60 * 1000)
        return;   // not a buff that lasts (an aura with no end is the caster's own, not a gift)

    // Cast on a bot by name: the unit they targeted, or for a resurrection
    // the body of one that has released.
    uint64_t on = 0;
    if (Unit* target = spell->m_targets.GetUnitTarget())
    {
        Player* them = target->ToPlayer();
        if (!them || them == player || !OllamaIsBotPlayer(them))
            return;
        if (kind == Kind::Heal && them->GetHealthPct() > float(g_GoodTurnHealPct))
            return;   // topped off: nothing it would notice
        on = them->GetGUID().GetRawValue();
    }
    else if (kind == Kind::Rez)
    {
        if (Corpse* body = spell->m_targets.GetCorpseTarget())
            on = body->GetOwnerGUID().GetRawValue();
    }
    if (!on || on == player->GetGUID().GetRawValue())
        return;

    Noted n;
    n.giver = player->GetGUID().GetRawValue();
    n.bot   = on;
    n.spell = info->Id;
    n.kind  = kind;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_noted.size() < kMaxNoted)
        g_noted.push_back(n);
}

void GoodTurn_Update()
{
    std::vector<Noted> fresh;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fresh.swap(g_noted);
    }
    if (fresh.empty() && g_pending.empty() && g_last.empty())
        return;
    const auto now = Clock::now();

    for (const Noted& n : fresh)
    {
        Player* giver = ObjectAccessor::FindConnectedPlayer(ObjectGuid(n.giver));
        Player* bot   = ObjectAccessor::FindConnectedPlayer(ObjectGuid(n.bot));
        if (!g_GoodTurnEnable || !giver || !bot || giver == bot || !OllamaIsBotPlayer(bot) ||
            OllamaIsBotPlayer(giver) || bot->GetTeamId() != giver->GetTeamId())
            continue;
        ++g_stats.noted;

        // Once for the same player in a while, whatever it made of the last.
        const auto key = std::make_pair(n.bot, n.giver);
        auto last = g_last.find(key);
        if (last != g_last.end() && now - last->second < std::chrono::minutes(g_GoodTurnPairMinutes))
        {
            ++g_stats.again;
            continue;
        }
        g_last[key] = now;

        const bool together = bot->GetGroup() && bot->GetGroup() == giver->GetGroup();
        if (!Chance(together ? g_GoodTurnInGroupChance : g_GoodTurnChance))
            continue;

        Pending p;
        p.giver = n.giver;
        p.bot   = n.bot;
        p.spell = n.spell;
        p.kind  = n.kind;
        p.due   = now + std::chrono::milliseconds(
                            urand(g_GoodTurnDelayMinMs, std::max(g_GoodTurnDelayMinMs, g_GoodTurnDelayMaxMs)));
        // Raised, it has to take the resurrection and get up first.
        p.until = p.due + std::chrono::seconds(n.kind == Kind::Rez ? 90 : 20);
        g_pending.push_back(p);
    }

    for (auto it = g_pending.begin(); it != g_pending.end();)
    {
        if (now < it->due)
        {
            ++it;
            continue;
        }
        Player* giver = ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->giver));
        Player* bot   = ObjectAccessor::FindConnectedPlayer(ObjectGuid(it->bot));
        const bool there = g_GoodTurnEnable && giver && bot && giver->IsInWorld() && bot->IsInWorld() &&
                           bot->GetMap() == giver->GetMap() && bot->IsWithinDistInMap(giver, 40.0f);
        if (there && bot->IsAlive() && !bot->IsInCombat())
        {
            Act(bot, giver, *it);
            it = g_pending.erase(it);
        }
        else if (!there || now >= it->until)
        {
            ++g_stats.missed;
            it = g_pending.erase(it);
        }
        else
            ++it;
    }

    const auto keep = std::chrono::minutes(g_GoodTurnPairMinutes);
    for (auto it = g_last.begin(); it != g_last.end();)
        it = now - it->second > keep ? g_last.erase(it) : std::next(it);
}

GoodTurnStats GoodTurn_GetStats()
{
    GoodTurnStats s = g_stats;
    s.pending = uint32_t(g_pending.size());
    return s;
}
