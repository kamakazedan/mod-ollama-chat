#include "mod-ollama-chat_orders.h"
#include "mod-ollama-chat_answer.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "GameGraveyard.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "SharedDefines.h"
#include "TradeData.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "AiObjectContext.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace
{
    using TextRules::BotIntent;
    using Clock = std::chrono::steady_clock;

    constexpr uint32 ITEM_HEARTHSTONE  = 6948;
    constexpr uint32 SPELL_HEARTHSTONE = 8690;

    // A little inside TRADE_DISTANCE, as for the conjured food and water.
    constexpr float kTradeRange = TRADE_DISTANCE - 1.5f;

    OrdersStats g_stats;
    uint64_t    g_tick = 0;

    PlayerbotAI* AiOf(Player* p)
    {
        return p ? PlayerbotsMgr::instance().GetPlayerbotAI(p) : nullptr;
    }

    Player* Find(uint64_t guid)
    {
        return guid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)) : nullptr;
    }

    size_t Index(BotIntent i) { return static_cast<size_t>(i); }

    // The playerbots chat command an order is carried out with; empty for the
    // ones done here (trade, hearth, unstuck).
    const char* CommandFor(BotIntent i)
    {
        switch (i)
        {
            case BotIntent::Stay:   return "stay";
            case BotIntent::Follow: return "follow";
            case BotIntent::Attack: return "attack";
            case BotIntent::Flee:   return "flee";
            case BotIntent::Revive: return "release";
            case BotIntent::Buff:   return "rebuff";   // "buff" is playerbots' list of buff potions
            case BotIntent::Pull:   return "pull";
            case BotIntent::Leave:  return "leave";
            default:                return "";
        }
    }

    // "stay put": what was asked, for a bot that has to say it cannot.
    const char* OrderWords(BotIntent i)
    {
        switch (i)
        {
            case BotIntent::Stay:    return "stay put";
            case BotIntent::Follow:  return "follow them";
            case BotIntent::Attack:  return "attack their target";
            case BotIntent::Flee:    return "get out of the fight";
            case BotIntent::Revive:  return "release and run back";
            case BotIntent::Trade:   return "trade with them";
            case BotIntent::Hearth:  return "use your hearthstone";
            case BotIntent::Unstuck: return "get yourself unstuck";
            case BotIntent::Buff:    return "buff them";
            case BotIntent::Pull:    return "pull";
            case BotIntent::Leave:   return "leave the group";
            default:                 return "do something";
        }
    }

    // Orders for one bot only: said to it by name, whispered, or plainly the
    // bot the player was talking to.
    bool OneBotOrder(BotIntent i)
    {
        return i == BotIntent::Trade || i == BotIntent::Unstuck || i == BotIntent::Leave;
    }

    bool HasPartyBuffs(Player* bot)
    {
        switch (bot->getClass())
        {
            case CLASS_PRIEST:
            case CLASS_MAGE:
            case CLASS_DRUID:
            case CLASS_PALADIN:
                return true;
            default:
                return false;
        }
    }

    // ---- who an order is for ---------------------------------------------------

    // Does this bot do what `player` tells it? Its master, or the leader of the
    // group it is in -- the people playerbots itself listens to.
    bool TakesOrders(Player* bot, Player* player)
    {
        PlayerbotAI* ai = AiOf(bot);
        if (!ai || !bot->IsInWorld() || bot == player)
            return false;
        if (ai->GetMaster() == player)
            return true;
        Group* group = player->GetGroup();
        return group && group == bot->GetGroup() && group->IsLeader(player->GetGUID());
    }

    struct Audience
    {
        std::vector<Player*> bots;    // who the order is for
        bool named  = false;          // named, or whispered: these exactly
        bool person = false;          // a real player was named first: the line is theirs
    };

    Audience FindAudience(Player* player, const std::string& msg, ChatChannelSourceLocal source, Player* receiver)
    {
        Audience a;
        if (source == SRC_WHISPER_LOCAL)
        {
            if (receiver && OllamaIsBotPlayer(receiver) && TakesOrders(receiver, player))
            {
                a.bots.push_back(receiver);
                a.named = true;
            }
            return a;
        }

        Group* group = player->GetGroup();
        if (!group)
            return a;

        size_t firstBot = std::string::npos, firstPerson = std::string::npos;
        std::vector<Player*> named, all;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player || !member->IsInWorld())
                continue;
            const bool bot = OllamaIsBotPlayer(member);
            const size_t pos = OllamaFindBotNameMention(msg, member->GetName());
            if (pos != std::string::npos)
            {
                if (!bot)
                    firstPerson = std::min(firstPerson, pos);
                else
                {
                    firstBot = std::min(firstBot, pos);
                    if (TakesOrders(member, player))
                        named.push_back(member);
                }
            }
            if (bot && TakesOrders(member, player))
                all.push_back(member);
        }

        // "Bob, stay here" with Bob a person: his to answer.
        if (firstPerson < firstBot)
        {
            a.person = true;
            return a;
        }
        if (!named.empty())
        {
            a.bots  = std::move(named);
            a.named = true;
            return a;
        }
        a.bots = std::move(all);
        return a;
    }

    // ---- what playerbots does with the line itself ---------------------------

    // The chat command playerbots reads `text` as, found the way
    // ExternalEventHelper::ParseChatCommand finds it: the whole line, then
    // shorter and shorter word prefixes ("stay here" is its "stay"). Empty
    // when it is none.
    std::string ParsedCommand(PlayerbotAI* ai, const std::string& text)
    {
        if (!ai)
            return {};
        AiObjectContext* ctx = ai->GetAiObjectContext();
        if (!ctx)
            return {};

        size_t b = text.find_first_not_of(" \t");
        if (b == std::string::npos)
            return {};
        const size_t e = text.find_last_not_of(" \t");
        const std::string line = text.substr(b, e - b + 1);

        if (ctx->GetTrigger(line))
            return line;
        size_t i = std::string::npos;
        for (;;)
        {
            const size_t found = line.rfind(' ', i);
            if (found == std::string::npos || found == 0)
                break;
            const std::string name = line.substr(0, found);
            if (ctx->GetTrigger(name))
                return name;
            i = found - 1;
        }
        return {};
    }

    // The command playerbots runs by itself for a plain chat line: none with
    // Playerbots.CommandPrefix set, since then a line has to start with the
    // prefix to be a command at all.
    std::string OwnCommand(PlayerbotAI* ai, const std::string& line)
    {
        return sPlayerbotAIConfig.CommandPrefix.empty() ? ParsedCommand(ai, line) : std::string();
    }

    // ---- carrying it out -------------------------------------------------------

    struct Pending
    {
        uint64_t    bot    = 0;
        uint64_t    player = 0;
        BotIntent   intent = BotIntent::None;
        std::string command;
        uint64_t    tick   = 0;    // queued in this tick; carried out in a later one
    };
    std::vector<Pending> g_pending;

    struct TradeWalk
    {
        uint64_t          player = 0;
        Clock::time_point until;
        bool              following = false;
    };
    std::unordered_map<uint64_t, TradeWalk> g_tradeWalks;

    struct FleeWatch
    {
        uint64_t          player = 0;
        Clock::time_point quietSince{};   // both out of combat since; zero while fighting
    };
    std::unordered_map<uint64_t, FleeWatch> g_flees;

    void SendCommand(PlayerbotAI* ai, Player* player, const std::string& command)
    {
        // Through playerbots' own command queue, so its own checks apply (who
        // may command the bot) and it runs in the bot's next AI tick.
        ai->HandleCommand(CHAT_MSG_WHISPER, sPlayerbotAIConfig.CommandPrefix + command, player);
        ++g_stats.sent;
    }

    // The bot asks to trade. When they take it, their client opens the window.
    bool OfferTrade(Player* bot, Player* player)
    {
        if (bot->GetTradeData() || player->GetTradeData())
            return false;
        bot->SetFacingToObject(player);
        WorldPacket packet(CMSG_INITIATE_TRADE, 8);
        packet << player->GetGUID();
        bot->GetSession()->HandleInitiateTradeOpcode(packet);
        return bot->GetTrader() == player;
    }

    // What the stuck tool does for a player: in a dungeon or raid, back to its
    // entrance; anywhere else, to the nearest graveyard.
    bool Unstick(Player* bot)
    {
        Map* map = bot->GetMap();
        if (!map)
            return false;
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        if (map->IsDungeon())
        {
            AreaTriggerTeleport const* at = sObjectMgr->GetMapEntranceTrigger(bot->GetMapId());
            return at && bot->TeleportTo(at->target_mapId, at->target_X, at->target_Y, at->target_Z,
                                         at->target_Orientation);
        }
        GraveyardStruct const* grave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
        return grave && bot->TeleportTo(grave->Map, grave->x, grave->y, grave->z, bot->GetOrientation());
    }

    void Carry(const Pending& p)
    {
        Player* bot = Find(p.bot);
        Player* player = Find(p.player);
        PlayerbotAI* ai = AiOf(bot);
        if (!bot || !player || !ai || !bot->IsInWorld())
            return;

        switch (p.intent)
        {
            case BotIntent::Trade:
            {
                TradeWalk& walk = g_tradeWalks[p.bot];
                walk.player    = p.player;
                walk.until     = Clock::now() + std::chrono::seconds(std::max<uint32_t>(5, g_OrdersTradeWalkSeconds));
                walk.following = false;
                break;
            }
            case BotIntent::Hearth:
                if (ai->DoSpecificAction("hearthstone", Event("ollama orders", "", player), true))
                    ++g_stats.sent;
                break;
            case BotIntent::Unstuck:
                if (Unstick(bot))
                    ++g_stats.unstuck;
                break;
            default:
                if (!p.command.empty())
                    SendCommand(ai, player, p.command);
                if (p.intent == BotIntent::Flee)
                    g_flees[p.bot] = FleeWatch{ p.player, {} };
                else if (p.intent == BotIntent::Follow || p.intent == BotIntent::Attack)
                    g_flees.erase(p.bot);
                break;
        }
    }

    // ---- what the bot says -----------------------------------------------------

    struct Facts
    {
        std::string who;       // "you", "you and the others"
        std::string target;    // what they have targeted
        std::string doing;     // trade: coming over, or opening the window
        std::string where;     // unstuck: where the bot will be
        std::string reason;    // could not
    };

    std::string HintFor(const std::string& templ, Player* player, BotIntent intent, const Facts& f)
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ,
                                           fmt::arg("player_name", player->GetName()),
                                           fmt::arg("who", f.who),
                                           fmt::arg("target", f.target),
                                           fmt::arg("doing", f.doing),
                                           fmt::arg("where", f.where),
                                           fmt::arg("reason", f.reason),
                                           fmt::arg("order", OrderWords(intent)),
                                           fmt::arg("pass_token", g_ContextPassToken));
        return out == "[Format Error]" ? std::string() : out;
    }

    // Why `bot` cannot do it now, in words for the bot; empty when it can.
    std::string CantReason(BotIntent intent, Player* bot, Player* player, Unit* target)
    {
        if (intent == BotIntent::Revive)
            return {};   // only ever asked of the dead
        if (!bot->IsAlive())
            return "you are dead";

        switch (intent)
        {
            case BotIntent::Attack:
                if (!target)
                    return "they do not have anything to fight targeted";
                break;
            case BotIntent::Pull:
                if (!PlayerbotAI::IsTank(bot))
                    return "you are not the tank";
                if (!target)
                {
                    Group* group = bot->GetGroup();
                    if (!group || group->GetTargetIcon(7).IsEmpty())
                        return "they have not targeted or marked what to pull";
                }
                break;
            case BotIntent::Hearth:
                if (bot->InBattleground() || bot->InArena())
                    return "you cannot hearth out of a battleground";
                if (bot->IsInCombat())
                    return "you are in the middle of a fight";
                if (!bot->HasItemCount(ITEM_HEARTHSTONE, 1))
                    return "you do not have a hearthstone on you";
                if (bot->HasSpellCooldown(SPELL_HEARTHSTONE))
                    return "your hearthstone is still on cooldown";
                break;
            case BotIntent::Unstuck:
                if (bot->InBattleground() || bot->InArena())
                    return "there is no unstuck in a battleground";
                if (bot->IsInCombat())
                    return "you are in the middle of a fight";
                break;
            case BotIntent::Trade:
                if (!player->IsAlive())
                    return "they are dead, and nobody can trade with a ghost";
                if (bot->GetMapId() != player->GetMapId())
                    return "you are nowhere near them";
                if (sPlayerbotAIConfig.EnableRandomBotTrading == 0 &&
                    (sRandomPlayerbotMgr.IsRandomBot(bot) || sRandomPlayerbotMgr.IsAddclassBot(bot)))
                    return "bots like you are not allowed to trade on this realm";
                if ((bot->GetTradeData() || player->GetTradeData()) && bot->GetTrader() != player)
                    return "one of you is already in a trade";
                break;
            case BotIntent::Buff:
                if (!HasPartyBuffs(bot))
                    return "you have no buffs to give";
                break;
            case BotIntent::Leave:
                if (!bot->GetGroup())
                    return "you are not in a group";
                break;
            default:
                break;
        }
        return {};
    }

    // Who answers for the group: the tank if it is one of them, so "ok, tell
    // us when to follow" comes from whoever leads; otherwise any of them.
    Player* Spokesperson(const std::vector<Player*>& bots)
    {
        if (bots.empty())
            return nullptr;
        for (Player* b : bots)
            if (PlayerbotAI::IsTank(b))
                return b;
        return bots[urand(0, uint32(bots.size() - 1))];
    }

    void Answer(Player* player, Player* bot, const std::string& msg, ChatChannelSourceLocal source,
                const std::string& scopeKey, uint64_t seq, const std::string& hint)
    {
        if (!bot || hint.empty())
            return;
        OllamaBotAnswer(player, bot, msg, source, scopeKey, seq, hint, CrowdLineKind::Order);
    }
}

bool Orders_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                            Player* receiver, const std::string& scopeKey, uint64_t& seq)
{
    if (!g_OrdersEnable || !player || msg.empty() || OllamaIsBotPlayer(player) || !player->IsInWorld())
        return false;
    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL && source != SRC_WHISPER_LOCAL)
        return false;
    if (source == SRC_WHISPER_LOCAL && (!receiver || !OllamaIsBotPlayer(receiver)))
        return false;

    // "!stay": a playerbots command, which playerbots carries out and answers
    // itself (a stock "Staying" muted by OllamaChat.Mute.Patterns is answered
    // here instead).
    const std::string& prefix = sPlayerbotAIConfig.CommandPrefix;
    const bool raw = !prefix.empty() && msg.compare(0, prefix.size(), prefix) == 0;
    const std::string said = raw ? msg.substr(prefix.size()) : msg;

    // Chat filters ("@tank stay") are playerbots' own.
    if (!said.empty() && said[0] == '@')
        return false;

    Audience audience = FindAudience(player, said, source, receiver);
    if (audience.person || audience.bots.empty())
    {
        if (raw)
            ++g_stats.raw;
        return raw;
    }

    std::vector<std::string> names;
    if (Group* group = player->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != player)
                    names.push_back(member->GetName());
    if (receiver && std::find(names.begin(), names.end(), receiver->GetName()) == names.end())
        names.push_back(receiver->GetName());

    const TextRules::BotCommand order = TextRules::MatchBotCommand(Context_CleanText(said), g_OrdersRules, names);
    if (order.intent == BotIntent::None)
    {
        if (raw)
            ++g_stats.raw;
        return raw;
    }
    const BotIntent intent = order.intent;

    // Who exactly.
    std::vector<Player*> bots;
    if (intent == BotIntent::Revive)
    {
        // Only the dead can release: "release the hounds" to a living group is
        // just chat.
        for (Player* b : audience.bots)
            if (b->isDead())
                bots.push_back(b);
        if (bots.empty())
            return raw;
    }
    else if (OneBotOrder(intent) && !audience.named && audience.bots.size() > 1 &&
             !(intent == BotIntent::Leave && order.everyone))
    {
        // "come trade me" with four bots about: the one they were talking to.
        if (!seq)
        {
            Governor_NoteHumanMessage(scopeKey);
            seq = Context_RecordLine(player, source, std::string(), msg, ContextLineKind::Human);
        }
        const ContextAddressee to = Context_LikelyAddressee(player, source, std::string(), seq);
        for (Player* b : audience.bots)
            if (to.isBot && to.guid == b->GetGUID().GetRawValue())
                bots.push_back(b);
        if (bots.empty())
            return raw;   // nobody in particular: ordinary chat
    }
    else
    {
        bots = audience.bots;
        if (intent == BotIntent::Trade && bots.size() > 1)
            bots.resize(1);   // one trade window at a time
    }

    // What they have targeted, for attack and pull.
    Unit* target = nullptr;
    if (intent == BotIntent::Attack || intent == BotIntent::Pull)
    {
        Unit* selected = player->GetSelectedUnit();
        if (selected && selected != player && selected->IsAlive() && player->IsValidAttackTarget(selected))
            target = selected;
    }

    // Who can, and why not for the rest.
    std::vector<Player*> able;
    std::string firstReason;
    Player* firstUnable = nullptr;
    for (Player* b : bots)
    {
        const std::string why = CantReason(intent, b, player, target);
        if (why.empty())
            able.push_back(b);
        else if (!firstUnable)
        {
            firstUnable = b;
            firstReason = why;
        }
    }

    // A whole group of bots that cannot do it ("buff us" with nobody who
    // buffs) answers only when it was asked of them by name.
    if (able.empty() && !audience.named && bots.size() > 1 && intent == BotIntent::Buff)
        return raw;

    if (!seq)
    {
        Governor_NoteHumanMessage(scopeKey);
        seq = Context_RecordLine(player, source, std::string(), msg, ContextLineKind::Human);
    }
    ++g_stats.orders;

    Facts facts;
    facts.who = (bots.size() > 1) ? "you and the others" : "you";
    if (target)
        facts.target = target->GetName();
    else if (intent == BotIntent::Pull)
        facts.target = "the mob marked with the skull";

    if (raw && able.empty())
    {
        ++g_stats.raw;   // playerbots says why not itself
        return true;
    }
    if (able.empty())
    {
        ++g_stats.refused;
        facts.reason = firstReason;
        Answer(player, firstUnable, msg, source, scopeKey, seq, HintFor(g_OrdersHintCant, player, intent, facts));
        return true;
    }

    std::string command = CommandFor(intent);
    if (intent == BotIntent::Pull && !target)
        command = "pull rti";   // what the skull is on

    // "!stay": playerbots' own command, typed as one. It carries it out and
    // answers itself; when that answer is a stock line muted for this one
    // ("Staying"), the bot says it in its own words instead. Anything else
    // typed with the prefix is left to playerbots entirely.
    if (raw)
    {
        if (command.empty() || ParsedCommand(AiOf(able[0]), said) != command)
        {
            ++g_stats.raw;
            return true;
        }
        g_stats.already += able.size();
    }

    // Carry it out, a tick from now: after playerbots has run its own reading
    // of the same line, so ours is the one that stands.
    for (Player* b : able)
    {
        if (raw)
            break;
        PlayerbotAI* ai = AiOf(b);
        if (!ai)
            continue;

        const std::string& cmd = command;
        if (intent == BotIntent::Revive && b->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            continue;           // released already, on the way back

        if (!cmd.empty() && OwnCommand(ai, msg) == cmd)
        {
            ++g_stats.already;   // "stay here": playerbots' "stay" is this order
            continue;
        }

        Pending p;
        p.bot     = b->GetGUID().GetRawValue();
        p.player  = player->GetGUID().GetRawValue();
        p.intent  = intent;
        p.command = cmd;
        p.tick    = g_tick;
        g_pending.push_back(std::move(p));
    }

    // One bot says it is on it.
    Player* voice = audience.named && able.size() == 1 ? able[0] : Spokesperson(able);
    if (intent == BotIntent::Trade)
        facts.doing = voice->IsWithinDistInMap(player, kTradeRange) ? "opening a trade window with them"
                                                                    : "coming over to trade with them";
    if (intent == BotIntent::Unstuck)
    {
        Map* map = voice->GetMap();
        facts.where = (map && map->IsDungeon()) ? "back at the entrance of the dungeon"
                                                : "at the nearest graveyard";
    }
    if (intent == BotIntent::Revive)
        facts.doing = voice->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "already running back to your body as a ghost"
                                                                : "releasing your spirit to run back to your body";

    Answer(player, voice, msg, source, scopeKey, seq, HintFor(g_OrdersHint[Index(intent)], player, intent, facts));

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} gave an order ({}) to {} bot(s){}: '{}'",
                 player->GetName(), TextRules::BotIntentName(intent), able.size(), raw ? " (playerbots command)" : "",
                 Context_CleanText(msg));
    return true;
}

void Orders_Update(uint32_t /*diff*/)
{
    ++g_tick;

    if (!g_pending.empty())
    {
        std::vector<Pending> due, later;
        for (Pending& p : g_pending)
            (p.tick < g_tick ? due : later).push_back(std::move(p));
        g_pending.swap(later);
        for (const Pending& p : due)
            Carry(p);
    }

    const Clock::time_point now = Clock::now();

    // Bots walking over to trade.
    for (auto it = g_tradeWalks.begin(); it != g_tradeWalks.end();)
    {
        Player* bot = Find(it->first);
        Player* player = Find(it->second.player);
        PlayerbotAI* ai = AiOf(bot);
        bool done = !bot || !player || !ai || now >= it->second.until || !bot->IsAlive() || !player->IsAlive() ||
                    !bot->IsInWorld() || !player->IsInWorld() || bot->GetMapId() != player->GetMapId();
        if (!done && !bot->IsInCombat() && !player->IsInCombat())
        {
            if (bot->GetTrader() == player)
                done = true;
            else if (bot->IsWithinDistInMap(player, kTradeRange))
            {
                if (OfferTrade(bot, player))
                    ++g_stats.trades;
                done = true;
            }
            else if (!it->second.following)
            {
                SendCommand(ai, player, "follow");
                it->second.following = true;
            }
        }
        it = done ? g_tradeWalks.erase(it) : std::next(it);
    }

    // Bots told to run keep out of fights ("flee" is follow and passive)
    // until told otherwise: once it is over, they fight again.
    static Clock::time_point nextFleeCheck{};
    if (!g_flees.empty() && now >= nextFleeCheck)
    {
        nextFleeCheck = now + std::chrono::seconds(1);
        for (auto it = g_flees.begin(); it != g_flees.end();)
        {
            Player* bot = Find(it->first);
            Player* player = Find(it->second.player);
            PlayerbotAI* ai = AiOf(bot);
            if (!bot || !player || !ai || !ai->HasStrategy("passive", BOT_STATE_COMBAT))
            {
                it = g_flees.erase(it);
                continue;
            }
            if (bot->IsInCombat() || player->IsInCombat() || !bot->IsAlive())
            {
                it->second.quietSince = {};
                ++it;
                continue;
            }
            if (it->second.quietSince == Clock::time_point{})
                it->second.quietSince = now;
            if (now - it->second.quietSince >= std::chrono::seconds(g_OrdersFleeRecoverSeconds))
            {
                SendCommand(ai, player, "follow");
                it = g_flees.erase(it);
                continue;
            }
            ++it;
        }
    }
}

std::string Orders_StateTextFor(Player* bot)
{
    if (!bot || g_StateStayingText.empty() || !bot->IsAlive())
        return {};
    PlayerbotAI* ai = AiOf(bot);
    if (!ai || !ai->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return {};
    Player* master = ai->GetMaster();
    const std::string out = SafeFormat(g_StateStayingText,
                                       fmt::arg("player_name", master && master != bot ? master->GetName()
                                                                                         : std::string("your group")));
    return out == "[Format Error]" ? std::string() : out;
}

OrdersStats Orders_GetStats()
{
    return g_stats;
}
