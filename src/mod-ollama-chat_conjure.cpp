#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_answer.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Bag.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TradeData.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ChatHelper.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using TextRules::CONJURE_DRINK;
    using TextRules::CONJURE_FOOD;

    // A little inside TRADE_DISTANCE, so a mage that takes a step while the
    // window opens is still close enough to finish.
    constexpr float kTradeRange = TRADE_DISTANCE - 1.5f;

    // ---- what mages can conjure ----------------------------------------------

    // One conjure spell, and the food or drink it makes.
    struct ConjureOption
    {
        uint32_t spellId       = 0;
        uint32_t itemId        = 0;
        uint8_t  kinds         = 0;   // CONJURE_DRINK: restores mana; CONJURE_FOOD: health
        uint32_t requiredLevel = 0;   // of the item
        uint32_t itemLevel     = 0;
    };

    std::vector<ConjureOption> g_options;   // every one in the game, best first
    bool g_optionsBuilt = false;

    // What eating or drinking this restores.
    uint8_t RestoreKinds(ItemTemplate const* proto)
    {
        uint8_t kinds = 0;
        for (const _Spell& s : proto->Spells)
        {
            if (s.SpellId <= 0)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(s.SpellId));
            if (!info)
                continue;
            for (const SpellEffectInfo& eff : info->GetEffects())
            {
                if (eff.Effect != SPELL_EFFECT_APPLY_AURA)
                    continue;
                switch (eff.ApplyAuraName)
                {
                    case SPELL_AURA_MOD_POWER_REGEN:
                    case SPELL_AURA_OBS_MOD_POWER:
                    case SPELL_AURA_PERIODIC_ENERGIZE:
                        if (eff.MiscValue == POWER_MANA)
                            kinds |= CONJURE_DRINK;
                        break;
                    case SPELL_AURA_MOD_REGEN:
                    case SPELL_AURA_OBS_MOD_HEALTH:
                    case SPELL_AURA_PERIODIC_HEAL:
                        kinds |= CONJURE_FOOD;
                        break;
                    default:
                        break;
                }
            }
        }
        return kinds;
    }

    // Every mage spell that makes conjured food or drink, from the game data
    // rather than a list of ids: Conjure Water and Food at every rank, and
    // Conjure Refreshment, which does both.
    void BuildOptions()
    {
        g_optionsBuilt = true;
        g_options.clear();
        for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || info->SpellFamilyName != SPELLFAMILY_MAGE)
                continue;
            for (const SpellEffectInfo& eff : info->GetEffects())
            {
                if (eff.Effect != SPELL_EFFECT_CREATE_ITEM || !eff.ItemType)
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(eff.ItemType);
                if (!proto || !proto->IsConjuredConsumable() || proto->SubClass != ITEM_SUBCLASS_FOOD)
                    continue;
                const uint8_t kinds = RestoreKinds(proto);
                if (kinds)
                    g_options.push_back({ id, eff.ItemType, kinds, proto->RequiredLevel, proto->ItemLevel });
            }
        }

        // Best first: what a higher level can use, then what restores both.
        std::stable_sort(g_options.begin(), g_options.end(), [](const ConjureOption& a, const ConjureOption& b)
        {
            if (a.requiredLevel != b.requiredLevel)
                return a.requiredLevel > b.requiredLevel;
            const bool aBoth = a.kinds == (CONJURE_DRINK | CONJURE_FOOD);
            const bool bBoth = b.kinds == (CONJURE_DRINK | CONJURE_FOOD);
            if (aBoth != bBoth)
                return aBoth;
            return a.itemLevel > b.itemLevel;
        });

        LOG_INFO("module.ollamachat", "[Ollama Chat] Found {} mage spells that conjure food or water.",
                 g_options.size());
    }

    const std::vector<ConjureOption>& Options()
    {
        if (!g_optionsBuilt)
            BuildOptions();
        return g_options;
    }

    const ConjureOption* OptionForItem(uint32_t itemId)
    {
        for (const ConjureOption& o : Options())
            if (o.itemId == itemId)
                return &o;
        return nullptr;
    }

    // What this mage can conjure of `kind` that `who` can use, best first.
    std::vector<const ConjureOption*> OptionsFor(Player* mage, Player* who, uint8_t kind)
    {
        std::vector<const ConjureOption*> out;
        for (const ConjureOption& o : Options())
            if ((o.kinds & kind) && o.requiredLevel <= who->GetLevel() && mage->HasSpell(o.spellId))
                out.push_back(&o);
        return out;
    }

    bool KnowsAny(Player* mage, uint8_t kind)
    {
        for (const ConjureOption& o : Options())
            if ((o.kinds & kind) && mage->HasSpell(o.spellId))
                return true;
        return false;
    }

    uint32_t StackSize(uint32_t itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return proto ? std::max<uint32_t>(1, proto->GetMaxStackSize()) : 1;
    }

    // ---- gifts ----------------------------------------------------------------

    struct GiftItem
    {
        uint32_t itemId  = 0;
        uint32_t spellId = 0;
        uint32_t count   = 0;
        uint32_t before  = 0;   // how many the person had when it went into the window
    };

    enum class GiftState : uint8_t
    {
        Waiting,   // for a fight to end, for mana, or for them to come closer
        Offered,   // the mage asked to trade; the window is not open yet
        Open       // the window is open: fill it, keep the mage's side accepted
    };

    struct Gift
    {
        uint64_t              botGuid       = 0;
        uint64_t              requesterGuid = 0;
        std::vector<GiftItem> items;
        GiftState             state      = GiftState::Waiting;
        bool                  windowOpen = false;  // their window for the trade with the mage is open
        bool                  ours       = false;  // the mage offered the trade (not them)
        bool                  placed     = false;
        bool                  refreshed  = false;
        Clock::time_point     created{};
        Clock::time_point     since{};             // entered this state
        Clock::time_point     lastAnswer{};
        Clock::time_point     lastAccept{};
        Clock::time_point     placedAt{};
    };

    std::vector<Gift> g_gifts;
    ConjureStats      g_stats;

    // People whose trade window opened since the last tick. Written by the
    // packet hook, from whichever thread sends the packet; read on the tick.
    std::mutex            g_openedMutex;
    std::vector<uint64_t> g_opened;
    std::atomic<uint32_t> g_watching{ 0 };   // gifts pending: the hook only looks while there are any

    Gift* FindGift(uint64_t botGuid, uint64_t requesterGuid)
    {
        for (Gift& g : g_gifts)
            if (g.botGuid == botGuid && g.requesterGuid == requesterGuid)
                return &g;
        return nullptr;
    }

    void CountWatched()
    {
        g_watching.store(uint32_t(g_gifts.size()), std::memory_order_relaxed);
    }

    // Every stack of `itemId` in the mage's bags, biggest first.
    std::vector<Item*> StacksOf(Player* mage, uint32_t itemId)
    {
        std::vector<Item*> out;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = mage->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetEntry() == itemId)
                    out.push_back(item);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = mage->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    if (Item* item = bag->GetItemByPos(uint8(i)))
                        if (item->GetEntry() == itemId)
                            out.push_back(item);
        std::stable_sort(out.begin(), out.end(), [](Item* a, Item* b) { return a->GetCount() > b->GetCount(); });
        return out;
    }

    // The mana for one cast of each thing the mage does not have enough of
    // yet. That is all it waits for: with less than a full gift's worth it
    // conjures what it can and gives that.
    uint32_t ManaNeeded(Player* mage, const std::vector<GiftItem>& items)
    {
        if (!g_ConjureUseMana)
            return 0;
        uint32_t total = 0;
        for (const GiftItem& gi : items)
        {
            if (mage->GetItemCount(gi.itemId) >= gi.count)
                continue;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(gi.spellId))
                total += uint32(std::max<int32>(0, info->CalcPowerCost(mage, info->GetSchoolMask())));
        }
        return total;
    }

    // Why the gift has to wait, in words for the mage; empty when it can go now.
    std::string WaitReason(Player* mage, Player* req, const std::vector<GiftItem>& items)
    {
        if (mage->IsInCombat() || req->IsInCombat())
            return "as soon as this fight is over";
        if (!mage->IsAlive() || !req->IsAlive())
            return "as soon as you are both up again";
        if ((mage->GetTradeData() || req->GetTradeData()) && mage->GetTrader() != req)
            return "as soon as the trade going on now is done";
        if (mage->GetMapId() != req->GetMapId() || !mage->IsWithinDistInMap(req, kTradeRange))
            return "once they come over to you, since you are too far apart to trade";
        if (mage->GetPower(POWER_MANA) < ManaNeeded(mage, items))
            return "once you have the mana for it, since you are out of mana right now";
        return {};
    }

    // Conjure whatever of the gift the mage does not have yet. A triggered
    // cast: instant, with the spell's own visual, and -- with UseMana -- its
    // mana taken by hand, since a triggered cast ignores the cost. Afterwards
    // each item is cut down to what there really is, and dropped if none.
    bool ConjureAll(Player* mage, std::vector<GiftItem>& items)
    {
        for (GiftItem& gi : items)
        {
            uint32_t have = mage->GetItemCount(gi.itemId);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(gi.spellId);
            for (int casts = 0; info && casts < 6 && have < gi.count; ++casts)
            {
                const int32 cost = g_ConjureUseMana ? info->CalcPowerCost(mage, info->GetSchoolMask()) : 0;
                if (cost > 0 && mage->GetPower(POWER_MANA) < uint32(cost))
                    break;
                mage->CastSpell(mage, gi.spellId, TRIGGERED_FULL_MASK);
                const uint32_t now = mage->GetItemCount(gi.itemId);
                if (now <= have)
                    break;   // nothing made: no room
                if (cost > 0)
                    mage->ModifyPower(POWER_MANA, -cost);
                have = now;
            }
            gi.count = std::min(gi.count, have);
        }
        items.erase(std::remove_if(items.begin(), items.end(), [](const GiftItem& gi) { return gi.count == 0; }),
                    items.end());
        return !items.empty();
    }

    // The mage asks to trade. When they take it, the server tells their client
    // to open the window, which the packet hook passes on.
    bool OfferTrade(Player* mage, Player* req)
    {
        if (mage->GetTradeData() || req->GetTradeData())
            return false;
        mage->SetFacingToObject(req);
        WorldPacket packet(CMSG_INITIATE_TRADE, 8);
        packet << req->GetGUID();
        mage->GetSession()->HandleInitiateTradeOpcode(packet);
        return mage->GetTrader() == req;
    }

    // Offer the trade, then conjure: no mana is spent on a trade that cannot
    // start (another faction, a level requirement, a script that says no).
    // The cast is instant, so it is all in the bags long before the window
    // opens.
    bool StartGift(Gift& g, Player* mage, Player* req)
    {
        if (!OfferTrade(mage, req))
            return false;
        if (!ConjureAll(mage, g.items))
        {
            mage->TradeCancel(true);
            return false;
        }
        g.state = GiftState::Offered;
        g.ours  = true;
        g.since = Clock::now();
        ++g_stats.offered;
        return true;
    }

    // A trade they opened that nothing went into: close it rather than leave
    // an empty window -- unless they put something in it themselves.
    void CloseEmptyTrade(Player* mage, Player* req)
    {
        TradeData* theirs = req->GetTradeData();
        if (mage->GetTrader() != req || !theirs || theirs->GetMoney())
            return;
        for (uint8 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
            if (theirs->GetItem(TradeSlots(slot)))
                return;
        mage->TradeCancel(true);
    }

    // Put the gift in the open window, a stack per slot.
    void PlaceItems(Gift& g, Player* mage, Player* req)
    {
        TradeData* mine = mage->GetTradeData();
        if (!mine)
            return;
        uint8 slot = 0;
        for (GiftItem& gi : g.items)
        {
            gi.before = req->GetItemCount(gi.itemId);
            uint32_t left = gi.count;
            for (Item* item : StacksOf(mage, gi.itemId))
            {
                if (left == 0)
                    break;
                if (mine->HasItem(item->GetGUID()))   // already in: playerbots' "t water"
                {
                    left -= std::min(left, item->GetCount());
                    continue;
                }
                while (slot < TRADE_SLOT_TRADED_COUNT && mine->GetItem(TradeSlots(slot)))
                    ++slot;
                if (slot >= TRADE_SLOT_TRADED_COUNT)
                    return;
                WorldPacket packet(CMSG_SET_TRADE_ITEM, 3);
                packet << uint8(slot) << uint8(item->GetBagSlot()) << uint8(item->GetSlot());
                mage->GetSession()->HandleSetTradeItemOpcode(packet);
                left -= std::min(left, item->GetCount());
            }
        }
    }

    // Keep the mage's side accepted while the trade is a pure gift: the
    // person's accept then completes it, and playerbots never weighs it up.
    // Once they put anything in themselves, it is an ordinary trade again.
    void KeepAccepted(Gift& g, Player* mage, Player* req)
    {
        TradeData* mine   = mage->GetTradeData();
        TradeData* theirs = req->GetTradeData();
        if (!mine || !theirs || mine->IsAccepted() || mine->IsInAcceptProcess() || theirs->GetMoney())
            return;
        bool any = false;
        for (uint8 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
        {
            if (theirs->GetItem(TradeSlots(slot)))
                return;
            any = any || mine->GetItem(TradeSlots(slot));
        }
        if (!any)
            return;

        // Once a second at most: an accept that fails (too far apart) would
        // otherwise be tried, and fail, every tick.
        const auto now = Clock::now();
        if (now - g.lastAccept < std::chrono::seconds(1))
            return;
        g.lastAccept = now;

        WorldPacket packet;
        mage->GetSession()->HandleAcceptTradeOpcode(packet);
    }

    bool ReceivedAll(const Gift& g, Player* req)
    {
        for (const GiftItem& gi : g.items)
            if (req->GetItemCount(gi.itemId) < gi.before + gi.count)
                return false;
        return !g.items.empty();
    }

    // ---- who answers ----------------------------------------------------------

    struct Target
    {
        Player* bot     = nullptr;
        bool    named   = false;   // named, or whispered
        bool    grouped = false;   // in the person's group
        bool    person  = false;   // a real player was named first: the line is theirs to answer
    };

    std::vector<std::string> GroupNames(Player* player)
    {
        std::vector<std::string> names;
        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (member != player)
                        names.push_back(member->GetName());
        return names;
    }

    bool InGroupWith(Player* a, Player* b)
    {
        Group* group = a->GetGroup();
        return group && group->IsMember(b->GetGUID());
    }

    // Whom the line names first: one of the group, or in /say a bot standing
    // near. A real player named first ("Bob can you give me water", with Bob
    // a mage) is left to answer it. For a whisper, whoever it was whispered to.
    Target FindTarget(Player* player, const std::string& msg, ChatChannelSourceLocal source, Player* receiver)
    {
        Target t;
        if (source == SRC_WHISPER_LOCAL)
        {
            if (receiver && receiver != player && OllamaIsBotPlayer(receiver))
            {
                t.bot     = receiver;
                t.named   = true;
                t.grouped = InGroupWith(player, receiver);
            }
            return t;
        }

        size_t bestPos = std::string::npos;
        auto consider = [&](Player* p)
        {
            if (!p || p == player || !p->IsInWorld())
                return;
            const size_t pos = OllamaFindBotNameMention(msg, p->GetName());
            if (pos == std::string::npos || pos >= bestPos)
                return;
            bestPos  = pos;
            t.person = !OllamaIsBotPlayer(p);
            t.bot    = t.person ? nullptr : p;
        };

        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                consider(ref->GetSource());

        if (!t.bot && !t.person && source == SRC_SAY_LOCAL)
        {
            std::list<Player*> near;
            Acore::AnyPlayerInObjectRangeCheck check(player, g_SayDistance, false, true);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(player, near, check);
            Cell::VisitObjects(player, searcher, g_SayDistance);
            for (Player* p : near)
                consider(p);
        }

        if (t.bot)
        {
            t.named   = true;
            t.grouped = InGroupWith(player, t.bot);
        }
        return t;
    }

    // Nobody named: the group's mage that can make what they asked for, and
    // is nearest. In /say, only a mage close enough to hear it.
    Player* PickMage(Player* player, uint8_t kinds, ChatChannelSourceLocal source)
    {
        Group* group = player->GetGroup();
        if (!group)
            return nullptr;

        Player* best      = nullptr;
        bool    bestCan   = false;
        float   bestDist  = 0.0f;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* p = ref->GetSource();
            if (!p || p == player || !p->IsInWorld() || p->getClass() != CLASS_MAGE || !OllamaIsBotPlayer(p))
                continue;
            if (p->GetMapId() != player->GetMapId())
                continue;
            const float dist = p->GetDistance(player);
            if (source == SRC_SAY_LOCAL && dist > g_SayDistance)
                continue;

            bool can = p->IsAlive();
            for (uint8_t kind : { CONJURE_DRINK, CONJURE_FOOD })
                if ((kinds & kind) && OptionsFor(p, player, kind).empty())
                    can = false;

            if (!best || (can && !bestCan) || (can == bestCan && dist < bestDist))
            {
                best     = p;
                bestCan  = can;
                bestDist = dist;
            }
        }
        return best;
    }

    // Another mage in the group to send them to, bot or not.
    std::string OtherMageName(Player* player, Player* except)
    {
        Group* group = player->GetGroup();
        if (!group)
            return {};
        std::string person;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* p = ref->GetSource();
            if (!p || p == player || p == except || p->getClass() != CLASS_MAGE)
                continue;
            if (OllamaIsBotPlayer(p))
                return p->GetName();
            if (person.empty())
                person = p->GetName();
        }
        return person;
    }

    // ---- what the mage says ---------------------------------------------------

    std::string RequestWords(uint8_t kinds)
    {
        if (kinds == (CONJURE_DRINK | CONJURE_FOOD))
            return "conjured food and water";
        return (kinds & CONJURE_FOOD) ? "conjured food" : "conjured water";
    }

    // "a stack of Conjured Mana Strudel", "2 stacks of ...", "12 Conjured ...".
    std::string GiftWords(const std::vector<GiftItem>& items)
    {
        std::vector<std::string> parts;
        for (const GiftItem& gi : items)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(gi.itemId);
            const std::string name = proto ? proto->Name1 : std::string("conjured food");
            const uint32_t stack = StackSize(gi.itemId);
            if (gi.count % stack == 0 && gi.count / stack == 1)
                parts.push_back("a stack of " + name);
            else if (gi.count % stack == 0)
                parts.push_back(fmt::format("{} stacks of {}", gi.count / stack, name));
            else
                parts.push_back(fmt::format("{} {}", gi.count, name));
        }
        return CrowdText::JoinNames(parts);
    }

    // The bot answers the line, in its own voice, steered by `hint`.
    void Answer(Player* player, Player* bot, const std::string& msg, ChatChannelSourceLocal source,
                const std::string& scopeKey, uint64_t seq, std::string hint)
    {
        OllamaBotAnswer(player, bot, msg, source, scopeKey, seq, std::move(hint), CrowdLineKind::Favor);
    }

    std::string Hint(const std::string& templ, Player* player, const std::string& request,
                     const std::string& gift = {}, const std::string& when = {}, const std::string& reason = {},
                     const std::string& cls = {}, const std::string& mage = {})
    {
        if (templ.empty())
            return {};
        return SafeFormat(templ,
                          fmt::arg("player_name", player->GetName()),
                          fmt::arg("request", request),
                          fmt::arg("gift", gift),
                          fmt::arg("when", when),
                          fmt::arg("reason", reason),
                          fmt::arg("class", cls),
                          fmt::arg("mage", mage),
                          fmt::arg("pass_token", g_ContextPassToken));
    }
}

bool Conjure_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                             Player* receiver, const std::string& scopeKey, uint64_t seq)
{
    if (!g_ConjureEnable || !player || msg.empty() || OllamaIsBotPlayer(player) || !player->IsInWorld())
        return false;
    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL && source != SRC_SAY_LOCAL &&
        source != SRC_WHISPER_LOCAL)
        return false;

    // A linked item is playerbots' to trade (Playerbots.EnableAutoTradeOnItemMention).
    if (sPlayerbotAIConfig.EnableAutoTradeOnItemMention && msg.find("|Hitem:") != std::string::npos)
        return false;

    const std::string clean = Context_CleanText(msg);
    std::vector<std::string> names = GroupNames(player);

    // Could this be an ask at all, if a bot had been named? Nearly every line
    // stops here, before anything walks the group or the area.
    if (TextRules::MatchConjureAsk(clean, g_ConjureRules, true, names).kinds == TextRules::CONJURE_NONE)
        return false;

    const Target target = FindTarget(player, msg, source, receiver);
    if (target.person)
        return false;   // said to a real player: theirs to answer
    if (target.bot && std::find(names.begin(), names.end(), target.bot->GetName()) == names.end())
        names.push_back(target.bot->GetName());

    const TextRules::ConjureAsk ask = TextRules::MatchConjureAsk(clean, g_ConjureRules, target.named, names);
    if (ask.kinds == TextRules::CONJURE_NONE)
        return false;

    Player* mage = target.bot;
    if (!mage)
        mage = PickMage(player, ask.kinds, source);
    if (!mage)
        return false;   // no mage to ask: ordinary chat
    if (!target.grouped && target.named && mage->getClass() != CLASS_MAGE)
        return false;   // asked a stranger who could not have helped anyway

    // The line is in the transcript before anyone answers it.
    if (!seq)
    {
        Governor_NoteHumanMessage(scopeKey);
        seq = Context_RecordLine(player, source, std::string(), msg, ContextLineKind::Human);
    }

    ++g_stats.asked;
    const std::string request = RequestWords(ask.kinds);
    const uint64_t botGuid = mage->GetGUID().GetRawValue();
    const uint64_t reqGuid = player->GetGUID().GetRawValue();
    const auto now = Clock::now();

    // Not in their group: playerbots would cancel the trade, so say to invite.
    if (!target.grouped && target.named)
    {
        ++g_stats.refused;
        Answer(player, mage, msg, source, scopeKey, seq, Hint(g_ConjureHintNotGrouped, player, request));
        return true;
    }

    if (mage->getClass() != CLASS_MAGE)
    {
        ++g_stats.refused;
        const std::string other = OtherMageName(player, mage);
        const std::string pointer = other.empty() ? std::string(" There is no mage in your group.")
                                                  : " " + other + " is the mage in your group.";
        Answer(player, mage, msg, source, scopeKey, seq,
               Hint(g_ConjureHintNotMage, player, request, {}, {}, {}, ChatHelper::FormatClass(mage->getClass()),
                    pointer));
        return true;
    }

    // Asked again: say so once in a while, not to every repeat.
    if (Gift* existing = FindGift(botGuid, reqGuid))
    {
        if (now - existing->lastAnswer >= std::chrono::seconds(15))
        {
            existing->lastAnswer = now;
            if (existing->state == GiftState::Waiting)
            {
                const std::string when = WaitReason(mage, player, existing->items);
                Answer(player, mage, msg, source, scopeKey, seq,
                       Hint(g_ConjureHintWait, player, request, GiftWords(existing->items),
                            when.empty() ? std::string("in a moment") : when));
            }
            else
            {
                Answer(player, mage, msg, source, scopeKey, seq, Hint(g_ConjureHintAlready, player, request));
            }
        }
        return true;
    }

    auto cant = [&](const std::string& reason)
    {
        ++g_stats.refused;
        Answer(player, mage, msg, source, scopeKey, seq,
               Hint(g_ConjureHintCant, player, request, {}, {}, reason));
        return true;
    };

    // Playerbots.EnableRandomBotTrading = 0 makes a random bot cancel every
    // trade the moment it hears of one.
    if (sPlayerbotAIConfig.EnableRandomBotTrading == 0 &&
        (sRandomPlayerbotMgr.IsRandomBot(mage) || sRandomPlayerbotMgr.IsAddclassBot(mage)))
        return cant("bots like you are not allowed to trade on this realm");
    if (!mage->IsAlive())
        return cant("you are dead");
    if (!player->IsAlive())
        return cant("they are dead, and nobody can trade with a ghost");
    if (mage->GetMapId() != player->GetMapId())
        return cant("you are nowhere near them");

    // What to give: the best they can use of each thing asked for, preferring
    // something the mage already has enough of. A refreshment is food and
    // water at once, so it answers both.
    Gift gift;
    gift.botGuid       = botGuid;
    gift.requesterGuid = reqGuid;
    gift.created = gift.since = gift.lastAnswer = now;

    uint8_t missing = 0;
    for (uint8_t kind : { CONJURE_DRINK, CONJURE_FOOD })
    {
        if (!(ask.kinds & kind))
            continue;
        bool covered = false;
        for (const GiftItem& gi : gift.items)
            if (const ConjureOption* o = OptionForItem(gi.itemId))
                covered = covered || (o->kinds & kind);
        if (covered)
            continue;

        const std::vector<const ConjureOption*> options = OptionsFor(mage, player, kind);
        if (options.empty())
        {
            missing |= kind;
            continue;
        }

        auto countFor = [&](const ConjureOption* o)
        {
            const uint32_t stack = StackSize(o->itemId);
            uint32_t stacks = ask.stacks ? ask.stacks
                            : ask.count  ? (ask.count + stack - 1) / stack
                                         : g_ConjureStacks;
            stacks = std::clamp<uint32_t>(stacks, 1, g_ConjureMaxStacks);
            return stacks * stack;
        };

        // The best there is -- unless the mage would have to wait for the mana
        // to conjure it and already carries enough of something else.
        const ConjureOption* pick = options.front();
        if (mage->GetItemCount(pick->itemId) < countFor(pick) &&
            mage->GetPower(POWER_MANA) < ManaNeeded(mage, { { pick->itemId, pick->spellId, countFor(pick), 0 } }))
        {
            for (const ConjureOption* o : options)
                if (mage->GetItemCount(o->itemId) >= countFor(o))
                {
                    pick = o;
                    break;
                }
        }
        gift.items.push_back({ pick->itemId, pick->spellId, countFor(pick), 0 });
    }

    if (gift.items.empty())
    {
        const char* what = (missing & CONJURE_DRINK) ? "water" : "food";
        if (KnowsAny(mage, missing))
            return cant(fmt::format("nothing you can conjure is {} they can use at their level", what));
        return cant(fmt::format("you have not learned to conjure {} yet", what));
    }

    // Room for it, on both sides.
    for (const GiftItem& gi : gift.items)
    {
        const uint32_t have = mage->GetItemCount(gi.itemId);
        ItemPosCountVec dest;
        if (have < gi.count &&
            mage->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, gi.itemId, gi.count - have) != EQUIP_ERR_OK)
            return cant("your bags are full");
        dest.clear();
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, gi.itemId, gi.count) != EQUIP_ERR_OK)
            return cant("their bags are full, so they need to make room first");
    }

    // They opened a trade with the mage before asking: that window is open
    // already (playerbots opens it as soon as it hears of the trade, and they
    // took a moment to type), so the tick fills it as soon as there is the
    // mana to conjure.
    if (mage->GetTrader() == player)
    {
        gift.windowOpen = true;
        const bool mana = mage->GetPower(POWER_MANA) >= ManaNeeded(mage, gift.items);
        if (!mana)
            ++g_stats.waited;
        const std::string giftWords = GiftWords(gift.items);
        g_gifts.push_back(std::move(gift));
        CountWatched();
        Answer(player, mage, msg, source, scopeKey, seq,
               mana ? Hint(g_ConjureHintGive, player, request, giftWords)
                    : Hint(g_ConjureHintWait, player, request, giftWords,
                           "once you have the mana for it, since you are out of mana right now"));
        return true;
    }

    const std::string when = WaitReason(mage, player, gift.items);
    if (!when.empty())
    {
        ++g_stats.waited;
        Answer(player, mage, msg, source, scopeKey, seq,
               Hint(g_ConjureHintWait, player, request, GiftWords(gift.items), when));
        g_gifts.push_back(std::move(gift));
        CountWatched();
        return true;
    }

    if (!StartGift(gift, mage, player))
        return cant("something got in the way of the trade");

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked {} for {}: offering {}.", player->GetName(),
                 mage->GetName(), request, GiftWords(gift.items));

    const std::string giftWords = GiftWords(gift.items);
    g_gifts.push_back(std::move(gift));
    CountWatched();
    Answer(player, mage, msg, source, scopeKey, seq, Hint(g_ConjureHintGive, player, request, giftWords));
    return true;
}

void Conjure_Update()
{
    std::vector<uint64_t> opened;
    {
        std::lock_guard<std::mutex> lock(g_openedMutex);
        opened.swap(g_opened);
    }
    if (g_gifts.empty())
        return;

    const auto now        = Clock::now();
    const auto waitLimit  = std::chrono::seconds(g_ConjureWaitSeconds);
    const auto tradeLimit = std::chrono::seconds(g_ConjureTradeSeconds);

    for (auto it = g_gifts.begin(); it != g_gifts.end();)
    {
        Gift& g = *it;
        Player* mage = ObjectAccessor::FindConnectedPlayer(ObjectGuid(g.botGuid));
        Player* req  = ObjectAccessor::FindConnectedPlayer(ObjectGuid(g.requesterGuid));
        bool done = !mage || !req || !mage->IsInWorld() || !req->IsInWorld() || !InGroupWith(req, mage);

        // Their window for a trade with the mage stays open while the two are
        // trading. The server says so once, as it opens.
        if (!done)
        {
            if (mage->GetTrader() != req)
                g.windowOpen = false;
            else if (std::find(opened.begin(), opened.end(), g.requesterGuid) != opened.end())
                g.windowOpen = true;
        }

        if (!done && g.state == GiftState::Waiting)
        {
            if (now - g.created > waitLimit)
            {
                CloseEmptyTrade(mage, req);
                ++g_stats.lapsed;
                done = true;
            }
            else if (mage->GetTrader() == req)
            {
                // They opened a trade with the mage themselves: once the
                // window is open, put the gift in, given the mana to make it.
                if (g.windowOpen && mage->GetPower(POWER_MANA) >= ManaNeeded(mage, g.items))
                {
                    if (!ConjureAll(mage, g.items))
                    {
                        CloseEmptyTrade(mage, req);
                        ++g_stats.lapsed;
                        done = true;
                    }
                    else
                    {
                        g.state = GiftState::Open;
                        g.since = now;
                        ++g_stats.offered;
                    }
                }
            }
            else if (WaitReason(mage, req, g.items).empty())
            {
                if (!StartGift(g, mage, req))
                {
                    ++g_stats.lapsed;
                    done = true;
                }
            }
        }
        else if (!done && g.state == GiftState::Offered)
        {
            if (mage->GetTrader() != req)
            {
                ++g_stats.lapsed;   // declined, or trades blocked
                done = true;
            }
            else if (g.windowOpen)
            {
                g.state = GiftState::Open;
                g.since = now;
            }
            else if (now - g.since > tradeLimit)
            {
                mage->TradeCancel(true);
                ++g_stats.lapsed;
                done = true;
            }
        }

        if (!done && g.state == GiftState::Open)
        {
            if (mage->GetTrader() != req)
            {
                if (g.placed && ReceivedAll(g, req))
                    ++g_stats.completed;
                else
                    ++g_stats.lapsed;
                done = true;
            }
            else if (now - g.since > tradeLimit)
            {
                // Not taken: the offer goes. A trade they have put something
                // into is theirs to finish or cancel.
                CloseEmptyTrade(mage, req);
                ++g_stats.lapsed;
                done = true;
            }
            else
            {
                if (!g.placed)
                {
                    PlaceItems(g, mage, req);
                    g.placed   = true;
                    g.placedAt = now;
                }
                KeepAccepted(g, mage, req);

                // In a trade they opened, the window may have opened a moment
                // after it was filled; show them the mage's side once more.
                if (!g.ours && !g.refreshed && now - g.placedAt >= std::chrono::seconds(2))
                {
                    req->GetSession()->SendUpdateTrade(true);
                    g.refreshed = true;
                }
            }
        }

        it = done ? g_gifts.erase(it) : std::next(it);
    }
    CountWatched();
}

std::string Conjure_GiveItems(Player* bot, Player* player, const std::vector<std::pair<uint32_t, uint32_t>>& items,
                              std::string& when)
{
    when.clear();
    if (!bot || !player || items.empty())
        return "there is nothing to give";
    if (!InGroupWith(player, bot))
        return "you only trade with people in your group";
    if (sPlayerbotAIConfig.EnableRandomBotTrading == 0 &&
        (sRandomPlayerbotMgr.IsRandomBot(bot) || sRandomPlayerbotMgr.IsAddclassBot(bot)))
        return "bots like you are not allowed to trade on this realm";
    if (!bot->IsAlive())
        return "you are dead";
    if (!player->IsAlive())
        return "they are dead, and nobody can trade with a ghost";
    if (bot->GetMapId() != player->GetMapId())
        return "you are nowhere near them";

    const uint64_t botGuid = bot->GetGUID().GetRawValue();
    const uint64_t reqGuid = player->GetGUID().GetRawValue();
    if (FindGift(botGuid, reqGuid))
        return {};   // one is on its way already

    const auto now = Clock::now();
    Gift gift;
    gift.botGuid       = botGuid;
    gift.requesterGuid = reqGuid;
    gift.created = gift.since = gift.lastAnswer = now;
    for (const auto& [entry, count] : items)
    {
        // No spell: nothing to conjure, so ConjureAll only cuts it down to
        // what the bot really has, and no mana is waited for.
        const uint32_t have = bot->GetItemCount(entry);
        if (have == 0 || count == 0)
            continue;
        gift.items.push_back({ entry, 0, std::min(count, have), 0 });
    }
    if (gift.items.empty())
        return "you do not have it any more";

    for (const GiftItem& gi : gift.items)
    {
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, gi.itemId, gi.count) != EQUIP_ERR_OK)
            return "their bags are full, so they need to make room first";
    }

    // A window between them is open already: the tick fills it.
    if (bot->GetTrader() == player)
    {
        gift.windowOpen = true;
        g_gifts.push_back(std::move(gift));
        CountWatched();
        return {};
    }

    when = WaitReason(bot, player, gift.items);
    if (!when.empty())
    {
        ++g_stats.waited;
        g_gifts.push_back(std::move(gift));
        CountWatched();
        return {};
    }

    if (!StartGift(gift, bot, player))
        return "something got in the way of the trade";
    g_gifts.push_back(std::move(gift));
    CountWatched();
    return {};
}

ConjureStats Conjure_GetStats()
{
    ConjureStats s = g_stats;
    s.pending = uint32_t(g_gifts.size());
    return s;
}

bool OllamaChatConjureScript::CanPacketSend(WorldSession* session, WorldPacket const& packet)
{
    // Every packet to every client comes through here, from any thread: say
    // yes to all of them, and look no further than the opcode unless a gift
    // is pending.
    if (packet.GetOpcode() != SMSG_TRADE_STATUS || g_watching.load(std::memory_order_relaxed) == 0 || !session ||
        packet.size() < 4 || packet.read<uint32>(0) != TRADE_STATUS_OPEN_WINDOW)
        return true;
    if (Player* player = session->GetPlayer())
    {
        std::lock_guard<std::mutex> lock(g_openedMutex);
        g_opened.push_back(player->GetGUID().GetRawValue());
    }
    return true;
}
