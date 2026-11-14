#include "mod-ollama-chat_tradechat.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_favor.h"
#include "mod-ollama-chat_gifts.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_haggle.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "DBCStores.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "TradeData.h"

#include "AiObjectContext.h"
#include "ItemUsageValue.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    enum class Side : uint8_t
    {
        BotBuys,    // they sell, the bot buys
        BotSells    // they buy, the bot sells
    };

    // A haggle between a real player and one bot over one thing.
    struct Talk
    {
        uint64_t          player = 0;
        uint64_t          bot    = 0;
        Side              side   = Side::BotBuys;
        uint32_t          entry  = 0;
        int32_t           suffix = 0;
        uint32_t          count  = 1;
        ObjectGuid        stack;              // selling: the stack it sells, whole
        Haggle::Style     style;
        Haggle::Buyer     buyer;
        Haggle::Seller    seller;
        uint64_t          worth  = 0;         // what the lot is worth
        uint64_t          ah     = 0;         // what the auction house has the lot for (0: nobody has one up)
        uint64_t          vendor = 0;         // what a vendor pays for the lot
        uint64_t          theirs = 0;         // their last price
        std::string       why;                // buying: what use it is to it
        bool              asked  = false;     // buying: it asked how much, and has had no price yet
        uint64_t          spare  = 0;         // buying: all it can spare, when that is what holds it back
        std::vector<uint32_t> said;           // the prices it has named: it may name them again
        bool              agreed = false;
        uint64_t          price  = 0;         // agreed
        uint32_t          before = 0;         // agreed: how many of it the bot had then
        bool              over   = false;     // it walked away
        bool              remark = false;     // only a remark (a call-out, a price check): no haggle of its own
        bool              whisper = false;    // the haggle has gone to whispers
        std::string       tradeChannel;       // Trade's name, for its lines there
        std::string       tradeScope;         // Trade's governor scope
        std::string       whisperScope;
        Clock::time_point last{};
        Clock::time_point until{};
        // Walking over to them, a deal struck.
        bool              walking = false;
        Clock::time_point walkUntil{};
        Clock::time_point nextMove{};
    };
    std::vector<Talk> g_talks;

    // Something a bot put up for sale in Trade.
    struct Listing
    {
        uint64_t          bot   = 0;
        uint32_t          entry = 0;
        uint32_t          count = 1;
        ObjectGuid        stack;
        uint64_t          ask   = 0;
        uint32_t          team  = 0;
        std::string       tradeChannel;
        std::string       tradeScope;
        Clock::time_point when{};
        Clock::time_point until{};
    };
    std::vector<Listing> g_listings;

    std::array<Clock::time_point, 2> g_nextAd{};   // by side: when the next thing goes up

    // The cheapest of a thing on each side's auction house, per one.
    struct AhSeen
    {
        uint64_t          unit = 0;
        Clock::time_point at{};
    };
    std::unordered_map<uint64_t, AhSeen> g_ah;

    TradeChatStats g_stats;

    constexpr float kTradeRange = TRADE_DISTANCE - 1.5f;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint32_t Roll100()
    {
        return std::uniform_int_distribution<uint32_t>(0, 99)(Rng());
    }

    bool Chance(uint32_t percent)
    {
        return percent > 0 && (percent >= 100 || Roll100() < percent);
    }

    template <typename T>
    T& Pick(std::vector<T>& v)
    {
        return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(Rng())];
    }

    Player* Find(uint64_t guid)
    {
        return guid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)) : nullptr;
    }

    std::string Money(uint64_t copper)
    {
        return TextRules::MoneyWords(uint32_t(std::min<uint64_t>(copper, 2147483647u)));
    }

    // A figure the way people say it, to the nearest round amount: "about
    // 1g 50s", not "1g 45s 60c" -- which the model would round itself, and
    // then be taken for naming a price nobody gave it.
    uint64_t Shown(uint64_t copper)
    {
        if (copper >= 100000)
            return (copper + 5000) / 10000 * 10000;   // whole gold from 10g
        if (copper >= 10000)
            return (copper + 500) / 1000 * 1000;      // 10s steps from 1g
        if (copper >= 1000)
            return (copper + 50) / 100 * 100;         // whole silver from 10s
        return copper;
    }

    uint32_t Clamp32(uint64_t copper)
    {
        return uint32_t(std::min<uint64_t>(copper, 2147483647u));
    }

    // This bot's way with money (OllamaChat.Trade.Style.<KEY>).
    Haggle::Style StyleOf(Player* bot)
    {
        std::string key = GetBotPersonality(bot);
        for (char& c : key)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto it = g_TradeStyles.find(key);
        return it == g_TradeStyles.end() ? g_TradeStyleDefault : it->second;
    }

    // The Trade channel `player` is on, if any.
    Channel* TradeOf(Player* player)
    {
        ChannelMgr* mgr = player ? ChannelMgr::forTeam(player->GetTeamId()) : nullptr;
        if (!mgr)
            return nullptr;
        for (auto const& [key, channel] : mgr->GetChannels())
            if (channel && channel->GetChannelId() == uint32(ChatChannelId::TRADE) &&
                OllamaIsOnChannel(channel, player->GetGUID()))
                return channel;
        return nullptr;
    }

    std::string TradeScope(Player* player, Channel* channel)
    {
        return Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_GENERAL_LOCAL], channel->GetChannelId(),
                                     channel->GetName(), 0, player->GetZoneId());
    }

    // ---- what a thing is ------------------------------------------------------

    const char* ClassWord(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "warriors";
            case CLASS_PALADIN:      return "paladins";
            case CLASS_HUNTER:       return "hunters";
            case CLASS_ROGUE:        return "rogues";
            case CLASS_PRIEST:       return "priests";
            case CLASS_DEATH_KNIGHT: return "death knights";
            case CLASS_SHAMAN:       return "shamans";
            case CLASS_MAGE:         return "mages";
            case CLASS_WARLOCK:      return "warlocks";
            case CLASS_DRUID:        return "druids";
            default:                 return "";
        }
    }

    std::string JoinAnd(const std::vector<std::string>& parts)
    {
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i)
            out += (i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ")) + parts[i];
        return out;
    }

    // The classes that can use a kind of weapon at all (Wrath's weapon skills).
    uint32_t WeaponClasses(uint32_t sub)
    {
        const uint32_t W = 1u << CLASS_WARRIOR, P = 1u << CLASS_PALADIN, H = 1u << CLASS_HUNTER,
                       R = 1u << CLASS_ROGUE, Pr = 1u << CLASS_PRIEST, D = 1u << CLASS_DEATH_KNIGHT,
                       S = 1u << CLASS_SHAMAN, M = 1u << CLASS_MAGE, L = 1u << CLASS_WARLOCK, Dr = 1u << CLASS_DRUID;
        switch (sub)
        {
            case ITEM_SUBCLASS_WEAPON_AXE:      return W | P | H | R | D | S;
            case ITEM_SUBCLASS_WEAPON_AXE2:     return W | P | H | D | S;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_GUN:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW: return W | H | R;
            case ITEM_SUBCLASS_WEAPON_MACE:     return W | P | R | Pr | D | S | Dr;
            case ITEM_SUBCLASS_WEAPON_MACE2:    return W | P | D | S | Dr;
            case ITEM_SUBCLASS_WEAPON_POLEARM:  return W | P | H | D | Dr;
            case ITEM_SUBCLASS_WEAPON_SWORD:    return W | P | H | R | D | M | L;
            case ITEM_SUBCLASS_WEAPON_SWORD2:   return W | P | H | D;
            case ITEM_SUBCLASS_WEAPON_STAFF:    return W | H | Pr | S | M | L | Dr;
            case ITEM_SUBCLASS_WEAPON_FIST:     return W | H | R | S | Dr;
            case ITEM_SUBCLASS_WEAPON_DAGGER:   return W | H | R | Pr | S | M | L | Dr;
            case ITEM_SUBCLASS_WEAPON_THROWN:   return W | H | R;
            case ITEM_SUBCLASS_WEAPON_WAND:     return Pr | M | L;
            default:                            return 0;
        }
    }

    // Who it is for, in a few words: "hunters, warriors and rogues use guns".
    std::string WhoUses(ItemTemplate const* p)
    {
        std::vector<std::string> classes;
        auto list = [&](uint32_t mask)
        {
            for (uint8 cls = CLASS_WARRIOR; cls <= CLASS_DRUID; ++cls)
                if ((mask & (1u << cls)) && *ClassWord(cls))
                    classes.push_back(ClassWord(cls));
        };
        const uint32_t allowed = p->AllowableClass > 0 ? uint32_t(p->AllowableClass) : 0;
        if (allowed && (allowed & 0x5FF) != 0x5FF)   // only some classes: a set piece, a relic
        {
            list(allowed << 1);
            return classes.empty() ? std::string() : "only " + JoinAnd(classes) + " can use it";
        }
        if (p->Class == ITEM_CLASS_WEAPON)
        {
            list(WeaponClasses(p->SubClass));
            return classes.empty() ? std::string() : JoinAnd(classes) + " can use it";
        }
        if (p->Class == ITEM_CLASS_ARMOR)
            switch (p->SubClass)
            {
                case ITEM_SUBCLASS_ARMOR_CLOTH:
                    return p->InventoryType == INVTYPE_CLOAK ? std::string("anyone can wear it")
                                                             : std::string("cloth, for priests, mages and warlocks");
                case ITEM_SUBCLASS_ARMOR_LEATHER:
                    return "leather, for rogues and druids (and hunters and shamans under 40)";
                case ITEM_SUBCLASS_ARMOR_MAIL:
                    return "mail, for hunters and shamans from 40 (and warriors and paladins under 40)";
                case ITEM_SUBCLASS_ARMOR_PLATE:
                    return "plate, for warriors, paladins and death knights, from level 40";
                case ITEM_SUBCLASS_ARMOR_SHIELD:
                    return "a shield, for warriors, paladins and shamans";
                default:
                    break;
            }
        return {};
    }

    const char* StatWord(uint32_t type)
    {
        switch (type)
        {
            case ITEM_MOD_AGILITY:              return "Agility";
            case ITEM_MOD_STRENGTH:             return "Strength";
            case ITEM_MOD_INTELLECT:            return "Intellect";
            case ITEM_MOD_SPIRIT:               return "Spirit";
            case ITEM_MOD_STAMINA:              return "Stamina";
            case ITEM_MOD_DEFENSE_SKILL_RATING: return "defense rating";
            case ITEM_MOD_DODGE_RATING:         return "dodge rating";
            case ITEM_MOD_PARRY_RATING:         return "parry rating";
            case ITEM_MOD_BLOCK_RATING:         return "block rating";
            case ITEM_MOD_HIT_RATING:           return "hit rating";
            case ITEM_MOD_CRIT_RATING:          return "crit rating";
            case ITEM_MOD_HASTE_RATING:         return "haste rating";
            case ITEM_MOD_EXPERTISE_RATING:     return "expertise rating";
            case ITEM_MOD_ATTACK_POWER:         return "attack power";
            case ITEM_MOD_RANGED_ATTACK_POWER:  return "ranged attack power";
            case ITEM_MOD_SPELL_POWER:          return "spell power";
            case ITEM_MOD_MANA_REGENERATION:    return "mana every 5 seconds";
            case ITEM_MOD_ARMOR_PENETRATION_RATING: return "armor penetration";
            case ITEM_MOD_RESILIENCE_RATING:    return "resilience";
            default:                            return "";
        }
    }

    // What it is, plainly: kind, quality, level, who uses it, its stats.
    std::string Facts(ItemTemplate const* p)
    {
        std::string out = Links_DescribeItem(p);
        if (p->Class == ITEM_CLASS_WEAPON && p->getDPS() > 0.0f)
            out += fmt::format(", {:.1f} damage per second", p->getDPS());
        std::vector<std::string> stats;
        for (uint32_t i = 0; i < p->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
            if (p->ItemStat[i].ItemStatValue > 0 && *StatWord(p->ItemStat[i].ItemStatType))
                stats.push_back(fmt::format("+{} {}", p->ItemStat[i].ItemStatValue, StatWord(p->ItemStat[i].ItemStatType)));
        if (p->Armor && p->Class == ITEM_CLASS_ARMOR)
            stats.insert(stats.begin(), fmt::format("{} armor", p->Armor));
        if (!stats.empty())
            out += ", " + JoinAnd(stats);
        const std::string who = WhoUses(p);
        if (!who.empty())
            out += "; " + who;
        return out;
    }

    // ---- what a thing is worth --------------------------------------------------

    // The cheapest one up on `team`'s auction house right now, per one, 0 when
    // there is none. Looked through at most once a minute a thing.
    uint64_t AuctionUnit(uint32_t entry, TeamId team)
    {
        const uint64_t key = (uint64_t(team) << 32) | entry;
        const auto now = Clock::now();
        auto it = g_ah.find(key);
        if (it != g_ah.end() && now - it->second.at < std::chrono::seconds(60))
            return it->second.unit;
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(
            team == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde);
        uint64_t best = 0;
        if (house)
            for (auto const& [id, auction] : house->GetAuctions())
                if (auction && auction->item_template == entry && auction->buyout && auction->itemCount)
                {
                    const uint64_t unit = auction->buyout / auction->itemCount;
                    if (!best || unit < best)
                        best = unit;
                }
        g_ah[key] = { best, now };
        return best;
    }

    struct Worth
    {
        uint64_t lot    = 0;   // what it is worth: the auction house, else what the market reckons (ValueOf)
        uint64_t ah     = 0;   // what the auction house has the lot for, 0 when nobody has one up
        uint64_t vendor = 0;   // what a vendor pays for the lot
    };

    // One of it, with nobody selling one: what the market reckons (with
    // mod-playerbots-plus: by quality, far more the rarer it drops), else a
    // multiple of what a vendor pays, by quality.
    uint64_t ValueOf(ItemTemplate const* p)
    {
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 7
        if (const uint64_t worth = PlayerbotsPlus::ItemWorth(p->ItemId))
            return worth;
#endif
        const bool gear = p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR;
        return Haggle::Value(g_TradeHaggle, p->SellPrice, p->Quality, gear);
    }

    // How rarely it drops, in percent (per kill of what can drop it): 0 when
    // nothing does, or nobody knows.
    double DropPct(ItemTemplate const* p)
    {
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 7
        return PlayerbotsPlus::DropChance(p->ItemId);
#else
        (void)p;
        return 0.0;
#endif
    }

    // "20,800": once in how many, the way people say it -- two figures.
    std::string OnceIn(double pct)
    {
        double n = 100.0 / pct;
        if (n >= 100.0)
        {
            const double step = std::pow(10.0, std::floor(std::log10(n)) - 1.0);
            n = std::round(n / step) * step;
        }
        std::string digits = std::to_string(uint64_t(std::llround(n)));
        for (int i = int(digits.size()) - 3; i > 0; i -= 3)
            digits.insert(size_t(i), ",");
        return digits;
    }

    Worth WorthOf(ItemTemplate const* p, uint32_t count, TeamId team)
    {
        Worth w;
        w.vendor = uint64_t(p->SellPrice) * count;
        w.ah     = AuctionUnit(p->ItemId, team) * count;
        w.lot    = w.ah ? w.ah : ValueOf(p) * count;
        return w;
    }

    // "It is worth about 5g 40s (nobody has one on the auction house; a vendor
    // pays 14s 56c for it). It is a rare drop: about once in 20,800 kills of
    // anything that can drop it."
    std::string WorthText(Worth const& w, ItemTemplate const* p)
    {
        if (!w.lot)
            return "Nobody can say what it is worth: it is not on the auction house and a vendor pays nothing for it.";
        std::string out = "It is worth about " + Money(Shown(w.lot)) + " (";
        out += w.ah ? "the auction house has it for about " + Money(Shown(w.ah))
                    : std::string("nobody has one on the auction house");
        if (w.vendor)
            out += "; a vendor pays about " + Money(Shown(w.vendor)) + " for it";
        out += ").";
        const double pct = p ? DropPct(p) : 0.0;
        if (pct > 0.0 && pct < g_TradeRareDropPct)
            out += " It is a rare drop: about once in " + OnceIn(pct) + " kills of anything that can drop it.";
        return out;
    }

    // ---- what can change hands ------------------------------------------------------

    // A kind of thing anyone could hand to someone else: not bound when
    // picked up, not a quest's, not bound to an account.
    bool CanChangeHands(ItemTemplate const* p)
    {
        return p && p->Bonding != BIND_WHEN_PICKED_UP && p->Bonding != BIND_QUEST_ITEM &&
               p->Bonding != BIND_QUEST_ITEM1 && !p->HasFlag(ITEM_FLAG_IS_BOUND_TO_ACCOUNT);
    }

    // This one could go to a stranger in a trade: of such a kind, and not
    // bound to whoever has it. (A thing bound when picked up can go, for two
    // hours, to those who were there when it dropped: never a stranger.)
    bool CanHandOver(Item* item)
    {
        return item && CanChangeHands(item->GetTemplate()) && !item->IsSoulBound() && !item->IsInTrade();
    }


    // ---- who is about ------------------------------------------------------------

    // Bots in the same city as `player` that could answer them in Trade: on
    // the channel, about and free, not in a group with a real player, and
    // someone playerbots would not trade with anyway (its group, its master).
    std::vector<Player*> CityBots(Player* player, Channel* channel)
    {
        std::vector<Player*> out;
        const uint64_t self = player->GetGUID().GetRawValue();
        for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
        {
            if (!p || p == player || !p->IsInWorld() || p->GetZoneId() != player->GetZoneId() ||
                p->GetMapId() != player->GetMapId() || p->GetTeamId() != player->GetTeamId() || !OllamaIsBotPlayer(p))
                continue;
            if (!OllamaIsOnChannel(channel, p->GetGUID()) || !Favor_CanAnswer(p, false) ||
                !Favor_IsStranger(p, player) || OllamaGroupHasRealPlayer(p) || p->GetTradeData())
                continue;
            // Not in the middle of another deal.
            const uint64_t raw = p->GetGUID().GetRawValue();
            if (std::any_of(g_talks.begin(), g_talks.end(),
                            [&](Talk const& t) { return t.bot == raw && t.player != self && !t.over; }))
                continue;
            out.push_back(p);
        }
        return out;
    }

    Talk* TalkOf(uint64_t player, uint64_t bot)
    {
        for (Talk& t : g_talks)
            if (t.player == player && t.bot == bot)
                return &t;
        return nullptr;
    }

    // The haggle `player` is in the middle of, the latest first.
    Talk* LatestTalk(uint64_t player)
    {
        Talk* best = nullptr;
        for (Talk& t : g_talks)
            if (t.player == player && (!best || t.last > best->last))
                best = &t;
        return best;
    }

    // The bot's stacks of a thing in its bags, the ones it could hand over.
    std::vector<Item*> StacksOf(Player* bot, uint32_t entry)
    {
        std::vector<Item*> out;
        auto take = [&](Item* item)
        {
            if (item && item->GetEntry() == entry && CanHandOver(item))
                out.push_back(item);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            take(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    take(bag->GetItemByPos(uint8(i)));
        return out;
    }

    // What playerbots hands its bots for nothing, again and again: selling
    // that would be making gold out of nothing.
    bool HandedOutFree(ItemTemplate const* p)
    {
        switch (p->Class)
        {
            case ITEM_CLASS_PROJECTILE:
            case ITEM_CLASS_REAGENT:
            case ITEM_CLASS_QUEST:
                return true;
            case ITEM_CLASS_CONSUMABLE:
                return p->SubClass == ITEM_SUBCLASS_FOOD || p->SubClass == ITEM_SUBCLASS_POTION ||
                       p->SubClass == ITEM_SUBCLASS_ITEM_ENHANCEMENT;
            default:
                return false;
        }
    }

    // Something it carries and has no use for, the way playerbots sees it.
    bool Spare(Player* bot, ItemTemplate const* p)
    {
        if (!p || HandedOutFree(p) || p->Quality < ITEM_QUALITY_NORMAL || !p->SellPrice)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->GetAiObjectContext())
            return false;
        ItemUsageValue value(ai);
        value.Qualify(std::to_string(p->ItemId));
        switch (value.Calculate())
        {
            case ITEM_USAGE_NONE:
            case ITEM_USAGE_AH:
            case ITEM_USAGE_VENDOR:
            case ITEM_USAGE_DISENCHANT:
            case ITEM_USAGE_BAD_EQUIP:
                return true;
            default:
                return false;
        }
    }

    // How many it is: "x20", "20x", "a stack" -- 1 when it does not say.
    uint32_t CountIn(const std::vector<std::string>& words, ItemTemplate const* p)
    {
        const uint32_t stack = std::max<uint32_t>(1, p->GetMaxStackSize());
        for (size_t i = 0; i < words.size(); ++i)
        {
            const std::string& w = words[i];
            if (w.size() > 1 && (w.front() == 'x' || w.back() == 'x'))
            {
                const std::string digits = w.front() == 'x' ? w.substr(1) : w.substr(0, w.size() - 1);
                if (!digits.empty() && digits.size() <= 4 &&
                    std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return std::clamp<uint32_t>(uint32_t(std::stoul(digits)), 1, stack);
            }
            if (w == "x" && i + 1 < words.size() && !words[i + 1].empty() && words[i + 1].size() <= 4 &&
                std::all_of(words[i + 1].begin(), words[i + 1].end(), [](char c) { return c >= '0' && c <= '9'; }))
                return std::clamp<uint32_t>(uint32_t(std::stoul(words[i + 1])), 1, stack);
            if ((w == "stack" || w == "stacks") && stack > 1)
                return stack;
        }
        return 1;
    }

    std::string CountWords(uint32_t count)
    {
        return count > 1 ? fmt::format(" x{}", count) : std::string();
    }

    // ---- saying it -----------------------------------------------------------------

    std::string Hint(const std::string& templ, Player* player, ItemTemplate const* p, Talk const& t,
                     uint64_t price, uint64_t offer, const std::string& where = std::string(),
                     const std::string& then = std::string(), const std::string& state = std::string())
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ,
            fmt::arg("player_name", player->GetName()),
            fmt::arg("item", p->Name1 + (t.count > 1 ? fmt::format(" (x{})", t.count) : std::string())),
            fmt::arg("price", Money(price)),
            fmt::arg("offer", Money(offer)),
            fmt::arg("worth", Money(Shown(t.worth))),
            fmt::arg("why", t.why.empty() ? std::string("you could use it") : t.why),
            fmt::arg("count", t.count > 1 ? fmt::format("{} of them", t.count) : std::string("one")),
            fmt::arg("where", where),
            fmt::arg("then", then),
            fmt::arg("state", state));
        return out == "[Format Error]" ? std::string() : out;
    }

    std::string StockLine(Player* bot, const std::string& kind, uint64_t offer, uint64_t price)
    {
        auto it = g_TradeStockLines.find(kind);
        if (it == g_TradeStockLines.end() || it->second.empty())
            return {};
        const std::string line = SafeFormat(it->second, fmt::arg("offer", Money(offer)), fmt::arg("price", Money(price)));
        return line == "[Format Error]" ? std::string() : Personality_StyleCanned(bot, line);
    }

    // Say `hint` about `t` to `player`: in Trade, or by whisper once the
    // haggle is in whispers. `mustPrice` it has to name (0: none), and no
    // price but that, `okPrices` and their units; `stock` goes out when the
    // model's line does not.
    void Say(Talk& t, Player* bot, Player* player, ItemTemplate const* p, const std::string& hint,
             uint64_t mustPrice, std::vector<uint32_t> okPrices, const std::string& stock)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || !ai->GetChatHelper() || hint.empty() || g_TradePromptTemplate.empty())
            return;

        const std::string tradeText = t.whisper
            ? SafeFormat(g_TradeWhisperText, fmt::arg("player_name", player->GetName()))
            : g_TradeChannelText;
        const std::string itemText = Facts(p) + ". " + WorthText({ t.worth, t.ah, t.vendor }, p);
        std::string prompt = SafeFormat(g_TradePromptTemplate,
            fmt::arg("bot_name", bot->GetName()),
            fmt::arg("bot_level", bot->GetLevel()),
            fmt::arg("bot_race", ai->GetChatHelper()->FormatRace(bot->getRace())),
            fmt::arg("bot_class", ai->GetChatHelper()->FormatClass(bot->getClass())),
            fmt::arg("bot_personality", GetPersonalityPromptAddition(GetBotPersonality(bot))),
            fmt::arg("channel", t.whisper ? std::string("a whisper to ") + player->GetName() : std::string("Trade chat")),
            fmt::arg("trade", tradeText == "[Format Error]" ? std::string() : tradeText),
            fmt::arg("item", itemText),
            fmt::arg("hint", hint));
        if (prompt.empty() || prompt == "[Format Error]")
            return;
        prompt += Roleplay_BuildVoicePrompt(bot);

        // What else it may name: their price, its own earlier ones, and the
        // figures it was told -- as given and as people round them -- and,
        // for a stack, each of those a piece ("1s each").
        if (t.theirs)
            okPrices.push_back(Clamp32(t.theirs));
        okPrices.insert(okPrices.end(), t.said.begin(), t.said.end());
        for (uint64_t figure : { t.worth, t.ah, t.vendor, t.spare })
            if (figure)
            {
                okPrices.push_back(Clamp32(figure));
                okPrices.push_back(Clamp32(Shown(figure)));
            }
        if (t.count > 1)
        {
            const size_t n = okPrices.size();
            for (size_t i = 0; i < n; ++i)
                if (okPrices[i] / t.count)
                    okPrices.push_back(uint32_t(okPrices[i] / t.count));
            if (mustPrice / t.count)
                okPrices.push_back(Clamp32(mustPrice / t.count));
        }
        if (mustPrice)
            t.said.push_back(Clamp32(mustPrice));

        Favor_ComeBack(bot);

        OllamaChatRequest request;
        request.botGuid           = bot->GetGUID().GetRawValue();
        request.targetGuid        = player->GetGUID().GetRawValue();
        request.source            = t.whisper ? SRC_WHISPER_LOCAL : SRC_GENERAL_LOCAL;
        request.channelName       = t.whisper ? std::string() : t.tradeChannel;
        request.channelId         = t.whisper ? 0 : uint32_t(ChatChannelId::TRADE);
        request.scopeKey          = t.whisper ? t.whisperScope : t.tradeScope;
        request.chainDepth        = 0;
        request.prompt            = std::move(prompt);
        request.botName           = bot->GetName();
        request.kind              = OllamaRequestKind::ChatReply;
        request.directAddress     = true;    // paced here: one line for each of theirs
        request.triggerBotReplies = false;   // nobody else's business
        request.noEcho            = true;    // a price is a price, however often it comes up
        request.maxReplyLength    = OllamaMaxReplyLengthFor(bot);
        request.mustPrice         = uint32_t(std::min<uint64_t>(mustPrice, 2147483647u));
        request.okPrices          = std::move(okPrices);
        request.fallbackText      = stock;
        request.extraDelayMs      = urand(g_TradeDelayMinMs, std::max(g_TradeDelayMinMs, g_TradeDelayMaxMs));
        if (OllamaDispatch_Submit(std::move(request)))
            ++g_stats.answered;
    }

    // Where it stands for the trade, for the hint once a deal is struck.
    std::string WhereFor(Talk& t, Player* bot, Player* player)
    {
        const float yards = bot->GetDistance(player);
        if (yards <= kTradeRange)
            return g_TradeWhereNear;
        if (yards <= g_TradeWalkYards)
            return g_TradeWhereWalk;
        std::string place, doing;
        if (!Hangout_WhereAndDoing(bot, place, doing))
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
            place = area ? area->area_name[0] : std::string("the city");
        }
        const std::string out = SafeFormat(g_TradeWhereFar, fmt::arg("place", place),
                                           fmt::arg("yards", uint32_t(std::lround(yards))));
        (void)t;
        return out == "[Format Error]" ? std::string() : out;
    }

    // ---- striking a deal ---------------------------------------------------------

    // Back to what playerbots has it doing: the walk over is done.
    void StopWalk(Talk& t, Player* bot)
    {
        if (!t.walking)
            return;
        t.walking = false;
        if (bot && bot->IsInWorld() && bot->IsAlive())
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }
    }

    // The deal is struck at `price`: carried out, and said.
    void Agree(Talk& t, Player* bot, Player* player, ItemTemplate const* p, uint64_t price, bool theirsTaken,
               bool bargain = false)
    {
        t.agreed = true;
        t.price  = price;
        t.asked  = false;
        t.before = bot->GetItemCount(t.entry);
        t.until  = Clock::now() + std::chrono::seconds(g_TradeMeetSeconds);
        ++g_stats.agreed;

        std::string why;
        if (t.side == Side::BotBuys)
            Gifts_AgreeDeal(bot, player, t.entry, t.suffix, t.count, uint32_t(price), g_TradeMeetSeconds);
        else
        {
            std::string when;
            Item* stack = bot->GetItemByGuid(t.stack);
            why = stack ? Conjure_SellItem(bot, player, stack, uint32_t(price), g_TradeMeetSeconds, when)
                        : std::string("you do not have it any more");
        }
        if (!why.empty())
        {
            // Cannot after all: says why, and that is the end of it.
            t.agreed = false;
            t.over   = true;
            Say(t, bot, player, p, Hint(g_TradeHintCannot, player, p, t, price, price, std::string(), why), 0, {},
                StockLine(bot, "pass", price, price));
            return;
        }

        // Close, it walks over; further off, it says where it is.
        const std::string where = WhereFor(t, bot, player);
        if (bot->GetDistance(player) > kTradeRange && bot->GetDistance(player) <= g_TradeWalkYards &&
            bot->GetMapId() == player->GetMapId())
        {
            t.walking   = true;
            t.walkUntil = Clock::now() + std::chrono::seconds(60);
            t.nextMove  = {};
            ++g_stats.walked;
        }
        const std::string& templ = !theirsTaken          ? g_TradeHintAgreed
                                   : t.side == Side::BotSells ? g_TradeHintSellTake
                                   : bargain                  ? g_TradeHintBargain
                                                              : g_TradeHintTake;
        Say(t, bot, player, p, Hint(templ, player, p, t, price, price, where), price, {},
            StockLine(bot, theirsTaken && t.side == Side::BotSells ? "selltake" : "agreed", price, price));
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Trade: {} {} {}'s {}{} for {}.", bot->GetName(),
                     t.side == Side::BotBuys ? "buys" : "sells", t.side == Side::BotBuys ? player->GetName() : "",
                     p->Name1, CountWords(t.count), Money(price));
    }

    // ---- what it says to a price ----------------------------------------------------

    // Buying: what it says to the decision.
    void BuyerSays(Talk& t, Player* bot, Player* player, ItemTemplate const* p, Haggle::Decision const& d,
                   uint64_t ask)
    {
        if (d.move == Haggle::Move::Take)
        {
            Agree(t, bot, player, p, d.price, true, d.bargain);
            return;
        }
        const std::vector<uint32_t> theirs = ask ? std::vector<uint32_t>{ uint32_t(ask) } : std::vector<uint32_t>{};
        // Taking the mickey: called out, the way its style would.
        if (d.ripOff && Chance(t.style.callout))
        {
            ++g_stats.callouts;
            std::string then;
            if (d.move == Haggle::Move::Counter || d.move == Haggle::Move::Final)
                then = SafeFormat(g_TradeThenOffer, fmt::arg("offer", Money(d.price)));
            else
                then = g_TradeThenPass;
            if (d.move == Haggle::Move::Pass)
            {
                t.over = true;
                ++g_stats.passed;
                if (d.price)
                    t.buyer.last = d.price;   // the most it would: "ok then" takes that
            }
            else
                ++g_stats.counters;
            Say(t, bot, player, p, Hint(g_TradeHintRipOff, player, p, t, ask, d.price, std::string(), then),
                d.move == Haggle::Move::Pass ? 0 : d.price, theirs,
                StockLine(bot, d.move == Haggle::Move::Pass ? "callout" : "counter", d.price, ask));
            return;
        }
        switch (d.move)
        {
            case Haggle::Move::Ask:
                t.asked = true;
                Say(t, bot, player, p, Hint(g_TradeHintAsk, player, p, t, 0, 0), 0, {}, StockLine(bot, "ask", 0, 0));
                break;
            case Haggle::Move::Counter:
                ++g_stats.counters;
                Say(t, bot, player, p,
                    Hint(ask ? g_TradeHintCounter : g_TradeHintOpen, player, p, t, ask, d.price), d.price, theirs,
                    StockLine(bot, "counter", d.price, ask));
                break;
            case Haggle::Move::Final:
                ++g_stats.counters;
                Say(t, bot, player, p, Hint(ask ? g_TradeHintFinal : g_TradeHintOpen, player, p, t, ask, d.price),
                    d.price, theirs, StockLine(bot, "final", d.price, ask));
                break;
            default:
                t.over = true;
                ++g_stats.passed;
                if (d.price)
                    t.buyer.last = d.price;   // the most it would: "ok then" takes that
                Say(t, bot, player, p, Hint(g_TradeHintPass, player, p, t, ask, d.price), 0,
                    { uint32_t(ask), uint32_t(d.price) }, StockLine(bot, "pass", d.price, ask));
                break;
        }
    }

    // Selling: what it says to the decision.
    void SellerSays(Talk& t, Player* bot, Player* player, ItemTemplate const* p, Haggle::Decision const& d,
                    uint64_t bid)
    {
        if (d.move == Haggle::Move::Take)
        {
            Agree(t, bot, player, p, d.price, true);
            return;
        }
        const std::vector<uint32_t> theirs = bid ? std::vector<uint32_t>{ uint32_t(bid) } : std::vector<uint32_t>{};
        if (d.lowball && bid && Chance(t.style.callout))
        {
            ++g_stats.callouts;
            std::string then = d.move == Haggle::Move::Pass ? g_TradeThenPass
                                                            : SafeFormat(g_TradeThenAsk, fmt::arg("offer", Money(d.price)));
            if (d.move == Haggle::Move::Pass)
            {
                t.over = true;
                ++g_stats.passed;
            }
            else
                ++g_stats.counters;
            Say(t, bot, player, p, Hint(g_TradeHintLowball, player, p, t, bid, d.price, std::string(), then),
                d.move == Haggle::Move::Pass ? 0 : d.price, theirs,
                StockLine(bot, d.move == Haggle::Move::Pass ? "sellpass" : "sellask", d.price, bid));
            return;
        }
        switch (d.move)
        {
            case Haggle::Move::Counter:
                if (!bid)
                {
                    Say(t, bot, player, p, Hint(g_TradeHintSellAsk, player, p, t, 0, d.price), d.price, {},
                        StockLine(bot, "sellask", d.price, 0));
                    break;
                }
                ++g_stats.counters;
                Say(t, bot, player, p, Hint(g_TradeHintSellCounter, player, p, t, bid, d.price), d.price, theirs,
                    StockLine(bot, "sellask", d.price, bid));
                break;
            case Haggle::Move::Final:
                ++g_stats.counters;
                Say(t, bot, player, p, Hint(g_TradeHintSellFinal, player, p, t, bid, d.price), d.price, theirs,
                    StockLine(bot, "sellfinal", d.price, bid));
                break;
            default:
                t.over = true;
                ++g_stats.passed;
                Say(t, bot, player, p, Hint(g_TradeHintSellPass, player, p, t, bid, d.price), 0,
                    { uint32_t(bid), uint32_t(d.price) }, StockLine(bot, "sellpass", d.price, bid));
                break;
        }
    }

    // How the haggle stands, in a few words, for a line that is about it but
    // names no price.
    std::string StateOf(Talk const& t)
    {
        if (t.agreed)
            return "you agreed " + Money(t.price) + " for it";
        if (t.remark)
            return t.theirs ? "you only said what you thought of their price, and you are not buying it"
                            : "you only told them what it goes for, and you are not buying or selling it";
        if (t.over)
            return "you have passed on it";
        if (t.side == Side::BotBuys)
        {
            if (t.asked)
                return "you asked how much they want for it";
            if (t.buyer.last && t.theirs)
                return "you offered " + Money(t.buyer.last) + " and they want " + Money(t.theirs);
            if (t.buyer.last)
                return "you offered " + Money(t.buyer.last);
            return "they want " + Money(t.theirs);
        }
        if (t.seller.last && t.theirs)
            return "you want " + Money(t.seller.last) + " for it and they offered " + Money(t.theirs);
        return "you want " + Money(t.seller.last) + " for it";
    }

    struct Line
    {
        std::vector<std::string> words;
        uint64_t                 price  = 0;
        bool                     accept = false;
        bool                     reject = false;
        bool                     sells  = false;   // selling something: "wts", "anyone want"
        bool                     buys   = false;   // buying: "wtb", "do you sell"
        bool                     check  = false;   // asking what it goes for: "pc"
    };

    // A line about a haggle going on: a price, a yes, a no, or anything else.
    void FollowUp(Talk& t, Player* bot, Player* player, ItemTemplate const* p, Line const& l)
    {
        t.last  = Clock::now();
        t.until = t.last + std::chrono::seconds(t.agreed ? g_TradeMeetSeconds : g_TradeTalkSeconds);
        const uint64_t ourLast = t.side == Side::BotBuys ? t.buyer.last : t.seller.last;

        if (!t.agreed && !t.over && l.price)
        {
            t.theirs = l.price;
            t.asked  = false;
            if (t.side == Side::BotBuys)
                BuyerSays(t, bot, player, p, Haggle::BuyerAnswer(g_TradeHaggle, t.style, t.buyer, l.price, Roll100()),
                          l.price);
            else
                SellerSays(t, bot, player, p,
                           Haggle::SellerAnswer(g_TradeHaggle, t.style, t.seller, l.price, Roll100()), l.price);
            return;
        }
        if (!t.agreed && !t.over && l.accept && ourLast)
        {
            Agree(t, bot, player, p, ourLast, false);
            return;
        }
        if (!t.agreed && !t.over && l.reject && ourLast)
        {
            if (t.side == Side::BotBuys)
            {
                const Haggle::Decision d = Haggle::BuyerRaise(t.style, t.buyer, t.theirs);
                BuyerSays(t, bot, player, p, d, t.theirs);
            }
            else
            {
                const Haggle::Decision d = Haggle::SellerLower(t.style, t.seller, t.theirs);
                SellerSays(t, bot, player, p, d, t.theirs);
            }
            return;
        }

        // Anything else about it ("its a gun", "where are you?"): answered as
        // things stand, no new price.
        const uint64_t keep = t.agreed ? t.price : ourLast;
        std::string where;
        if (t.agreed)
        {
            where = WhereFor(t, bot, player);
            if (!t.walking && bot->GetDistance(player) > kTradeRange && bot->GetDistance(player) <= g_TradeWalkYards)
            {
                t.walking   = true;
                t.walkUntil = Clock::now() + std::chrono::seconds(60);
                ++g_stats.walked;
            }
        }
        std::vector<uint32_t> ok;
        if (t.theirs)
            ok.push_back(uint32_t(t.theirs));
        if (keep)
            ok.push_back(uint32_t(keep));
        Say(t, bot, player, p, Hint(g_TradeHintTalk, player, p, t, t.theirs, keep, where, std::string(), StateOf(t)),
            0, ok, keep ? StockLine(bot, "talk", keep, t.theirs) : std::string());
    }

    Line Read(const std::string& msg)
    {
        Line l;
        const std::string around = TextRules::WithoutBracketNames(Context_CleanText(msg));
        l.words = CrowdText::Words(around);
        l.price = TextRules::ParsePrice(around);
        // "no deal" has "deal" in it: a no of more than one word ("no deal",
        // "too low", "forget it") outweighs a yes; a lone "no" does not ("no
        // problem").
        std::vector<std::vector<std::string>> strongNo;
        for (const std::vector<std::string>& phrase : g_TradeRejectWords)
            if (phrase.size() > 1)
                strongNo.push_back(phrase);
        const bool no = CrowdText::ContainsPhrase(l.words, strongNo);
        l.accept = !no && CrowdText::ContainsPhrase(l.words, g_TradeAcceptWords);
        l.reject = !l.accept && (no || CrowdText::ContainsPhrase(l.words, g_TradeRejectWords));
        // "do you sell", "u selling?": asked of a seller, which is buying,
        // though it says "sell".
        const bool askSeller = CrowdText::ContainsPhrase(l.words, g_TradeAskSellerWords);
        l.sells = !askSeller && CrowdText::ContainsPhrase(l.words, g_TradeSellWords);
        l.buys  = askSeller || CrowdText::ContainsPhrase(l.words, g_TradeBuyWords);
        l.check = CrowdText::ContainsPhrase(l.words, g_TradeCheckWords);
        return l;
    }

    // The first thing linked in `msg`, and its random property.
    uint32_t LinkedItem(const std::string& msg, int32_t& suffix)
    {
        suffix = 0;
        for (const TextRules::ChatLink& link : TextRules::ExtractChatLinks(msg))
        {
            if (link.type != "item" || !link.id)
                continue;
            // |Hitem:entry:enchant:gem1:gem2:gem3:gem4:suffix:...
            size_t at = link.markup.find("Hitem:");
            for (int field = 0; at != std::string::npos && field < 7; ++field)
                at = link.markup.find(':', at + 1);
            if (at != std::string::npos)
                suffix = std::atoi(link.markup.c_str() + at + 1);
            return link.id;
        }
        return 0;
    }

    // Each of `price` for a unit ("1s each"): the lot is that times `count`.
    uint64_t LotPrice(const std::vector<std::string>& words, uint64_t price, uint32_t count)
    {
        if (price && count > 1 && CrowdText::ContainsPhrase(words, g_TradeEachWords))
            return price * count;
        return price;
    }

    Talk NewTalk(Player* player, Player* bot, Channel* channel, Side side, ItemTemplate const* p, uint32_t count,
                 int32_t suffix, Worth const& w)
    {
        Talk t;
        t.player       = player->GetGUID().GetRawValue();
        t.bot          = bot->GetGUID().GetRawValue();
        t.side         = side;
        t.entry        = p->ItemId;
        t.suffix       = suffix;
        t.count        = count;
        t.style        = StyleOf(bot);
        t.worth        = w.lot;
        t.ah           = w.ah;
        t.vendor       = w.vendor;
        t.tradeChannel = channel->GetName();
        t.tradeScope   = TradeScope(player, channel);
        t.last         = Clock::now();
        t.until        = t.last + std::chrono::seconds(g_TradeTalkSeconds);
        return t;
    }

    // ---- they sell, a bot buys ------------------------------------------------------

    bool CanCarry(Player* player, uint32_t entry, uint32_t count)
    {
        uint32_t have = 0;
        auto add = [&](Item* item)
        {
            if (item && item->GetEntry() == entry && CanHandOver(item))
                have += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    add(bag->GetItemByPos(uint8(i)));
        return have >= count;
    }

    // Nobody wants it, and the price is a joke: now and then someone says so.
    void MaybeCallOut(Player* player, Channel* channel, ItemTemplate const* p, uint32_t count, int32_t suffix,
                      Worth const& w, uint64_t ask, std::vector<Player*>& bots)
    {
        if (!ask || !w.lot || ask * 100 < w.lot * g_TradeHaggle.ripOffPct || bots.empty() ||
            !Chance(g_TradeCalloutChance))
            return;
        std::shuffle(bots.begin(), bots.end(), Rng());
        for (size_t i = 0; i < bots.size() && i < 3; ++i)
        {
            Player* bot = bots[i];
            if (!Chance(StyleOf(bot).callout))
                continue;
            Talk t = NewTalk(player, bot, channel, Side::BotBuys, p, count, suffix, w);
            t.over   = true;
            t.remark = true;
            t.theirs = ask;
            ++g_stats.callouts;
            Say(t, bot, player, p, Hint(g_TradeHintCallout, player, p, t, ask, 0), 0, { uint32_t(ask) },
                StockLine(bot, "callout", 0, ask));
            g_talks.push_back(std::move(t));
            return;
        }
    }

    void TheySell(Player* player, Channel* channel, ItemTemplate const* p, int32_t suffix, uint32_t count,
                  uint64_t ask)
    {
        ++g_stats.sales;
        if (!CanCarry(player, p->ItemId, count))
        {
            ++g_stats.bound;   // not on them, or bound to them: nothing to trade, and nobody answers
            return;
        }
        const Worth w = WorthOf(p, count, player->GetTeamId());
        std::vector<Player*> bots = CityBots(player, channel);

        // Who could use it, cheaply first: the right level and class, and the
        // gold for it; then playerbots' own judgement, for a few of them.
        const bool wearable = p->InventoryType != INVTYPE_NON_EQUIP &&
                              (p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR);
        auto spareOf = [](Player* bot) { return uint64_t(bot->GetMoney()) * g_TradeMaxSpendPercent / 100; };
        // The gold for it: what it would pay for it, or their price if that
        // is less (a quest item it needs, worth more to it, it pays what it
        // can spare for).
        auto affords = [&](Player* bot, uint64_t worth)
        {
            const uint64_t wouldPay = worth * StyleOf(bot).pay / 100;
            return spareOf(bot) >= (ask ? std::min(ask, wouldPay) : wouldPay);
        };
        std::vector<Player*> able;
        for (Player* bot : bots)
        {
            if (p->RequiredLevel > bot->GetLevel())
                continue;
            if (wearable && bot->CanUseItem(p) != EQUIP_ERR_OK)
                continue;
            if (w.lot && !affords(bot, w.lot))
                continue;
            able.push_back(bot);
        }
        std::shuffle(able.begin(), able.end(), Rng());
        if (able.size() > g_TradeMaxJudged)
            able.resize(g_TradeMaxJudged);

        Player* best = nullptr;
        int bestScore = 0;
        std::string bestWhy;
        for (Player* bot : able)
        {
            std::string why;
            const int score = Gifts_Judge(bot, p, suffix, why);
            if (score < 50 || score <= bestScore)
                continue;
            best      = bot;
            bestScore = score;
            bestWhy   = why;
        }
        if (!best || !w.lot)
        {
            ++g_stats.nobody;
            MaybeCallOut(player, channel, p, count, suffix, w, ask, bots);
            return;
        }

        Talk t = NewTalk(player, best, channel, Side::BotBuys, p, count, suffix, w);
        t.why    = bestWhy;
        t.theirs = ask;
        // A thing it needs for a quest is worth a bit more to it.
        const uint64_t worthToIt = bestScore >= 90 ? w.lot * 5 / 4 : w.lot;
        const uint64_t spare = spareOf(best);
        t.buyer.worth = w.lot;
        t.buyer.limit = Haggle::BuyLimit(t.style, worthToIt, spare);
        if (spare < worthToIt * t.style.pay / 100)
        {
            t.spare = spare;
            t.why += ", though you can only spare about " + Money(Shown(spare));
        }
        const Haggle::Decision d = Haggle::BuyerAnswer(g_TradeHaggle, t.style, t.buyer, ask, Roll100());
        g_talks.push_back(std::move(t));
        Talk& added = g_talks.back();
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Trade: {} sells {}{}{}; {} (worth {}, pays up to {}) answers.",
                     player->GetName(), p->Name1, CountWords(count), ask ? " for " + Money(ask) : std::string(),
                     best->GetName(), Money(w.lot), Money(added.buyer.limit));
        BuyerSays(added, best, player, p, d, ask);
    }

    // ---- they buy, a bot sells -------------------------------------------------------

    // The bot's stack nearest `count` without going over (else its smallest).
    Item* StackFor(Player* bot, uint32_t entry, uint32_t count)
    {
        Item* best = nullptr;
        for (Item* item : StacksOf(bot, entry))
        {
            if (!best)
                best = item;
            else if (item->GetCount() <= count && (best->GetCount() > count || item->GetCount() > best->GetCount()))
                best = item;
            else if (best->GetCount() > count && item->GetCount() < best->GetCount())
                best = item;
        }
        return best;
    }

    void StartSale(Player* player, Player* bot, Channel* channel, ItemTemplate const* p, Item* stack, uint64_t bid,
                   uint64_t ask, bool speak = true)
    {
        const uint32_t count = stack->GetCount();
        const Worth w = WorthOf(p, count, player->GetTeamId());
        Talk t = NewTalk(player, bot, channel, Side::BotSells, p, count, stack->GetItemRandomPropertyId(), w);
        t.stack         = stack->GetGUID();
        t.theirs        = bid;
        t.seller.worth  = w.lot;
        t.seller.floor  = Haggle::SellFloor(t.style, w.lot, w.vendor);
        if (ask)
            t.seller.last = std::max<uint64_t>(ask, Haggle::Nice(t.seller.floor, true));   // the price it put up
        g_talks.push_back(std::move(t));
        Talk& added = g_talks.back();
        if (!speak)
        {
            if (!added.seller.last)
                Haggle::SellerOpen(added.style, added.seller);
            return;
        }
        const Haggle::Decision d = bid ? Haggle::SellerAnswer(g_TradeHaggle, added.style, added.seller, bid, Roll100())
                                       : (added.seller.last ? Haggle::Decision{ Haggle::Move::Counter, added.seller.last }
                                                            : Haggle::SellerOpen(added.style, added.seller));
        SellerSays(added, bot, player, p, d, bid);
    }

    void TheyBuy(Player* player, Channel* channel, ItemTemplate const* p, uint32_t count, uint64_t bid)
    {
        ++g_stats.wants;
        std::vector<Player*> sellers;
        for (Player* bot : CityBots(player, channel))
            if (bot->GetItemCount(p->ItemId, false) && Spare(bot, p) && !StacksOf(bot, p->ItemId).empty())
                sellers.push_back(bot);
        if (sellers.empty())
        {
            ++g_stats.nobody;
            return;
        }
        Player* bot = Pick(sellers);
        Item* stack = StackFor(bot, p->ItemId, count);
        if (!stack)
        {
            ++g_stats.nobody;
            return;
        }
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Trade: {} wants {}{}; {} has {} to spare.", player->GetName(),
                     p->Name1, CountWords(count), bot->GetName(), stack->GetCount());
        StartSale(player, bot, channel, p, stack, bid, 0);
    }

    // ---- what it goes for -------------------------------------------------------------

    void PriceCheck(Player* player, Channel* channel, ItemTemplate const* p, uint32_t count)
    {
        ++g_stats.checks;
        std::vector<Player*> bots = CityBots(player, channel);
        if (bots.empty() || !Chance(g_TradePriceCheckChance))
            return;
        Player* bot = Pick(bots);
        const Worth w = WorthOf(p, count, player->GetTeamId());
        Talk t = NewTalk(player, bot, channel, Side::BotBuys, p, count, 0, w);
        t.over   = true;
        t.remark = true;
        Say(t, bot, player, p, Hint(g_TradeHintPriceCheck, player, p, t, 0, 0), 0, {}, std::string());
        g_talks.push_back(std::move(t));
    }

    // ---- things bots put up for sale --------------------------------------------------

    // A bot in a city where a real player is on Trade puts up something it can
    // spare: "WTS [Pagan Vest] 1g 50s".
    void PostAd(TeamId team)
    {
        // Real players on Trade, on this side, in a city.
        std::vector<Player*> people;
        for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
            if (p && p->IsInWorld() && p->GetTeamId() == team && !OllamaIsBotPlayer(p) && TradeOf(p))
                people.push_back(p);
        if (people.empty())
            return;
        Player* person = Pick(people);
        Channel* channel = TradeOf(person);
        if (!channel || Context_HumanSpokeSince(person, SRC_GENERAL_LOCAL, channel->GetName(), 0,
                                                std::max<uint32_t>(g_ContextAmbientHoldSeconds, 30)))
            return;   // not over someone who just said something there

        std::vector<Player*> bots = CityBots(person, channel);
        std::shuffle(bots.begin(), bots.end(), Rng());
        for (size_t i = 0; i < bots.size() && i < 8; ++i)
        {
            Player* bot = bots[i];
            if (std::any_of(g_listings.begin(), g_listings.end(),
                            [&](Listing const& l) { return l.bot == bot->GetGUID().GetRawValue(); }))
                continue;
            // Something worth putting up: a green or better, or anything
            // worth Trade.AdMinCopper, that it can spare.
            struct Option
            {
                Item*               item = nullptr;
                ItemTemplate const* proto = nullptr;
                Worth               worth;
            };
            std::vector<Option> options;
            auto consider = [&](Item* item)
            {
                if (!CanHandOver(item))
                    return;
                ItemTemplate const* p = item->GetTemplate();
                if (!p || HandedOutFree(p) || !p->SellPrice)
                    return;
                const Worth w = WorthOf(p, item->GetCount(), team);
                if (p->Quality < ITEM_QUALITY_UNCOMMON && w.lot < g_TradeAdMinCopper)
                    return;
                options.push_back({ item, p, w });
            };
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 b = 0; b < bag->GetBagSize(); ++b)
                        consider(bag->GetItemByPos(uint8(b)));
            std::shuffle(options.begin(), options.end(), Rng());
            for (Option const& o : options)
            {
                if (!Spare(bot, o.proto))
                    continue;
                const Haggle::Style style = StyleOf(bot);
                Haggle::Seller seller;
                seller.worth = o.worth.lot;
                seller.floor = Haggle::SellFloor(style, o.worth.lot, o.worth.vendor);
                const Haggle::Decision d = Haggle::SellerOpen(style, seller);
                if (!d.price || g_TradeAdLines.empty())
                    return;
                const std::string templ = g_TradeAdLines[std::uniform_int_distribution<size_t>(0, g_TradeAdLines.size() - 1)(Rng())];
                const std::string line = SafeFormat(templ, fmt::arg("item", "[" + o.proto->Name1 + "]"),
                                                    fmt::arg("count", CountWords(o.item->GetCount())),
                                                    fmt::arg("price", Money(d.price)));
                if (line.empty() || line == "[Format Error]")
                    return;

                Listing l;
                l.bot          = bot->GetGUID().GetRawValue();
                l.entry        = o.proto->ItemId;
                l.count        = o.item->GetCount();
                l.stack        = o.item->GetGUID();
                l.ask          = d.price;
                l.team         = uint32_t(team);
                l.tradeChannel = channel->GetName();
                l.tradeScope   = TradeScope(bot, channel);
                l.when         = Clock::now();
                l.until        = l.when + std::chrono::minutes(std::max<uint32_t>(2, g_TradeAdMaxMinutes));

                OllamaChatRequest request;
                request.botGuid           = l.bot;
                request.source            = SRC_GENERAL_LOCAL;
                request.channelName       = l.tradeChannel;
                request.channelId         = uint32_t(ChatChannelId::TRADE);
                request.scopeKey          = l.tradeScope;
                request.botName           = bot->GetName();
                request.kind              = OllamaRequestKind::RandomChatter;
                request.fixedText         = Personality_StyleCanned(bot, line);
                request.directAddress     = true;
                request.triggerBotReplies = false;
                request.noEcho            = true;
                request.writtenAtSeq      = Context_NextSeq();
                if (!OllamaDispatch_Submit(std::move(request)))
                    return;
                g_listings.push_back(std::move(l));
                ++g_stats.ads;
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Trade: {} puts up {}{} for {}.", bot->GetName(),
                             o.proto->Name1, CountWords(o.item->GetCount()), Money(d.price));
                return;
            }
        }
    }

    // A line or whisper about something `bot` put up for sale.
    Listing* ListingOf(uint64_t bot, uint32_t entry)
    {
        const auto now = Clock::now();
        for (Listing& l : g_listings)
            if (l.bot == bot && now < l.until && (!entry || l.entry == entry))
                return &l;
        return nullptr;
    }

    // A reply in Trade to the latest thing put up there: "ill take it", "how
    // much for the vest", "1g?". Only just after it went up.
    Listing* LatestListing(uint32_t team, uint32_t entry)
    {
        const auto now = Clock::now();
        Listing* best = nullptr;
        for (Listing& l : g_listings)
            if (l.team == team && now < l.until && (!entry || l.entry == entry) &&
                (entry || now - l.when < std::chrono::seconds(90)) && (!best || l.when > best->when))
                best = &l;
        return best;
    }

    // Someone answered a listing: a haggle with that bot, at its price.
    bool AnswerListing(Player* player, Listing& l, Channel* channel, Line const& line, bool whispered,
                       const std::string& whisperScope)
    {
        Player* bot = Find(l.bot);
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(l.entry);
        Item* stack = bot ? bot->GetItemByGuid(l.stack) : nullptr;
        if (!bot || !p || !stack || !bot->IsInWorld() || stack->GetCount() != l.count || !Favor_CanAnswer(bot, true) ||
            !Favor_IsStranger(bot, player) || bot->GetMapId() != player->GetMapId())
            return false;
        if (!channel)
            channel = TradeOf(bot);
        if (!channel)
            return false;
        const uint64_t ask = l.ask;
        l.until = Clock::now();   // taken up: no longer for sale to everyone
        // "ill take it" or a price: answered as that, not with its price
        // first and that a moment later.
        StartSale(player, bot, channel, p, stack, 0, ask, !(line.price || line.accept));
        Talk* t = TalkOf(player->GetGUID().GetRawValue(), bot->GetGUID().GetRawValue());
        if (!t)
            return true;
        if (whispered)
        {
            t->whisper      = true;
            t->whisperScope = whisperScope;
        }
        // "ill take it": at its price. A price: haggled. Else: its price, said.
        if (line.price || line.accept)
            FollowUp(*t, bot, player, p, line);
        return true;
    }
}

bool TradeChat_IsTrade(Channel const* channel)
{
    return channel && channel->GetChannelId() == uint32(ChatChannelId::TRADE);
}

bool TradeChat_OnlyCommerce()
{
    return g_TradeEnable && g_TradeOnlyCommerce;
}

bool TradeChat_HandleHumanLine(Player* player, const std::string& msg, Channel* channel, const std::string& scopeKey)
{
    if (!g_TradeEnable || !player || !channel || msg.empty())
        return false;
    ++g_stats.heard;
    const bool only = TradeChat_OnlyCommerce();

    // In the transcript, like any line, before anyone answers it -- here when
    // nothing else will see it.
    bool recorded = false;
    auto record = [&]()
    {
        if (recorded)
            return;
        recorded = true;
        Governor_NoteHumanMessage(scopeKey);
        Context_RecordLine(player, SRC_GENERAL_LOCAL, channel->GetName(), msg, ContextLineKind::Human);
    };
    // Not a deal: in Trade for buying and selling only, nobody answers it;
    // otherwise it is ordinary chat, for the rest of the handler.
    auto notMine = [&]()
    {
        if (!only)
            return false;
        record();
        ++g_stats.ignored;
        return true;
    };

    if (!Favor_TradesWithStrangers())
        return notMine();   // bots trade with nobody outside their group: no market

    const Line line = Read(msg);
    int32_t suffix = 0;
    const uint32_t entry = LinkedItem(msg, suffix);
    const uint64_t self = player->GetGUID().GetRawValue();

    // A haggle going on, and this is about it: no other thing linked.
    if (Talk* t = LatestTalk(self))
        if (!t->whisper && (!entry || entry == t->entry) && Clock::now() < t->until && !(t->over && entry))
        {
            Player* bot = Find(t->bot);
            ItemTemplate const* p = sObjectMgr->GetItemTemplate(t->entry);
            if (bot && p && bot->IsInWorld())
            {
                if (t->remark)
                {
                    // Only a remark was made (on their price, or what it goes
                    // for): a new price is the thing offered again, for
                    // anyone -- wanted, when they say so, else for sale.
                    // Anything else is not to anyone.
                    if (!line.price)
                        return notMine();
                    record();
                    const uint32_t said = CountIn(line.words, p);
                    const uint32_t count = said > 1 ? said : t->count;
                    const int32_t sfx = t->suffix;
                    const uint64_t price = LotPrice(line.words, line.price, count);
                    t->until = Clock::now();
                    if (line.buys && !line.sells)
                        TheyBuy(player, channel, p, count, price);
                    else
                        TheySell(player, channel, p, sfx, count, price);
                    return true;
                }
                if (t->over && !line.price && !line.accept)
                    return notMine();   // it has said its last word; this is not to it
                record();
                if (t->over)
                {
                    // Back with a price after it passed: a fresh haggle.
                    t->over = false;
                    t->buyer.counters = t->seller.counters = 0;
                }
                FollowUp(*t, bot, player, p, line);
                return true;
            }
        }

    ItemTemplate const* p = entry ? sObjectMgr->GetItemTemplate(entry) : nullptr;
    const bool check = line.check, buys = line.buys, sells = line.sells;

    // A deal in a thing nobody can trade -- bound when picked up, a quest's,
    // bound to an account: there is none to be had, and nobody answers it.
    if (p && !CanChangeHands(p) && (buys || check || sells || line.accept || line.price))
    {
        record();
        ++g_stats.bound;
        return true;
    }

    // Answering something a bot put up -- linking it, or right after it went
    // up: wanting it, taking it, asking about it, or a price for it. Not
    // selling one of their own: "still selling [it]?" from someone with none
    // to sell is asking.
    if (buys || check || sells || line.accept || line.price)
        if (Listing* l = LatestListing(uint32_t(player->GetTeamId()), entry))
            if (!sells || (entry && !CanCarry(player, entry, 1)))
            {
                record();
                if (AnswerListing(player, *l, channel, line, false, std::string()))
                    return true;
            }

    // Something linked, bought, sold or priced: the market's. Anything else
    // (services, chat) is not.
    if (!p || (!check && !buys && !sells && !line.price))
        return recorded || notMine();
    record();
    const uint32_t count = CountIn(line.words, p);
    const uint64_t price = LotPrice(line.words, line.price, count);
    // "worth" and "value" turn up in a sale too ("WTS [it] 5g, worth 8"):
    // only a line that neither buys nor sells is asking what it goes for.
    if (check && !buys && !sells)
        PriceCheck(player, channel, p, count);
    else if (buys && !sells)
        TheyBuy(player, channel, p, count, price);
    else
        TheySell(player, channel, p, suffix, count, price);
    return true;
}

bool TradeChat_HandleWhisper(Player* player, Player* bot, const std::string& msg, const std::string& scopeKey)
{
    if (!g_TradeEnable || !player || !bot || msg.empty() || !OllamaIsBotPlayer(bot) || !Favor_TradesWithStrangers())
        return false;
    const uint64_t self = player->GetGUID().GetRawValue(), them = bot->GetGUID().GetRawValue();
    const Line line = Read(msg);
    int32_t suffix = 0;
    const uint32_t entry = LinkedItem(msg, suffix);

    Talk* t = TalkOf(self, them);
    if (t && Clock::now() < t->until && (!entry || entry == t->entry))
    {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(t->entry);
        if (!p)
            return false;
        Governor_NoteHumanMessage(scopeKey);
        Context_RecordLine(player, SRC_WHISPER_LOCAL, std::string(), msg, ContextLineKind::Human);
        t->whisper      = true;
        t->whisperScope = scopeKey;
        // Back with a price after it passed: a fresh haggle. Not after a
        // remark: it never wanted the thing.
        if (t->over && line.price && !t->remark)
        {
            t->over = false;
            t->buyer.counters = t->seller.counters = 0;
        }
        FollowUp(*t, bot, player, p, line);
        return true;
    }

    // Something it put up for sale: linked, or a whisper about buying it.
    Listing* l = ListingOf(them, entry);
    if (l && (entry || line.buys || line.check || line.accept || line.price))
    {
        Governor_NoteHumanMessage(scopeKey);
        Context_RecordLine(player, SRC_WHISPER_LOCAL, std::string(), msg, ContextLineKind::Human);
        return AnswerListing(player, *l, nullptr, line, true, scopeKey);
    }
    return false;
}

std::string TradeChat_PromptFor(Player* bot, Player* player)
{
    if (!bot || !player)
        return {};
    Talk* t = TalkOf(player->GetGUID().GetRawValue(), bot->GetGUID().GetRawValue());
    if (!t || Clock::now() >= t->until)
        return {};
    ItemTemplate const* p = sObjectMgr->GetItemTemplate(t->entry);
    if (!p)
        return {};
    return Hint(g_TradeSaidText, player, p, *t, t->theirs, t->agreed ? t->price : 0, std::string(), std::string(),
                StateOf(*t));
}

void TradeChat_Tick()
{
    if (!g_TradeEnable)
        return;
    const auto now = Clock::now();

    for (auto it = g_talks.begin(); it != g_talks.end();)
    {
        Talk& t = *it;
        Player* bot    = Find(t.bot);
        Player* player = Find(t.player);
        if (!bot || !player || !bot->IsInWorld() || !player->IsInWorld())
        {
            if (t.agreed)
                ++g_stats.lapsed;
            it = g_talks.erase(it);
            continue;
        }

        if (t.agreed)
        {
            // Done when the trade has gone through, or the deal has lapsed.
            const bool standing = t.side == Side::BotBuys ? Gifts_HasDeal(bot, player)
                                                          : Conjure_IsSelling(bot, player);
            if (!standing)
            {
                const uint32_t have = bot->GetItemCount(t.entry);
                const bool changed = t.side == Side::BotBuys ? have >= t.before + t.count : have + t.count <= t.before;
                ++(changed ? g_stats.done : g_stats.lapsed);
                if (g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Trade: {} and {} {}.", bot->GetName(),
                             player->GetName(), changed ? "traded" : "never traded");
                StopWalk(t, bot);
                it = g_talks.erase(it);
                continue;
            }

            // Walking over: its AI held, so playerbots does not walk it off.
            if (t.walking)
            {
                if (now >= t.walkUntil || bot->IsInCombat() || player->IsInCombat() || !bot->IsAlive() ||
                    bot->GetMapId() != player->GetMapId() || bot->GetTrader() == player)
                    StopWalk(t, bot);
                else if (bot->IsWithinDistInMap(player, kTradeRange))
                {
                    StopWalk(t, bot);
                    bot->SetFacingToObject(player);
                }
                else
                {
                    Favor_HoldAi(bot);
                    Hangout_Busy(bot, 3);
                    if (now >= t.nextMove && !bot->isMoving())
                    {
                        if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                            bot->SetStandState(UNIT_STAND_STATE_STAND);   // off the bench first
                        bot->GetMotionMaster()->MoveFollow(player, 2.0f, bot->GetFollowAngle());
                        t.nextMove = now + std::chrono::seconds(1);
                    }
                }
            }
        }
        ++it;
    }

    // Now and then, something put up for sale, on each side.
    if (g_TradeAdMaxMinutes && Favor_TradesWithStrangers())
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            Clock::time_point& next = g_nextAd[team];
            if (next == Clock::time_point{})
                next = now + std::chrono::minutes(urand(g_TradeAdMinMinutes, std::max(g_TradeAdMinMinutes, g_TradeAdMaxMinutes)));
            if (now < next)
                continue;
            next = now + std::chrono::minutes(urand(g_TradeAdMinMinutes, std::max(g_TradeAdMinMinutes, g_TradeAdMaxMinutes)));
            PostAd(team);
        }
}

void TradeChat_Update()
{
    const auto now = Clock::now();
    g_talks.erase(std::remove_if(g_talks.begin(), g_talks.end(),
                                 [&](Talk const& t) { return !t.agreed && now >= t.until; }),
                  g_talks.end());
    g_listings.erase(std::remove_if(g_listings.begin(), g_listings.end(),
                                    [&](Listing const& l) { return now >= l.until; }),
                     g_listings.end());
    for (auto it = g_ah.begin(); it != g_ah.end();)
        it = now - it->second.at > std::chrono::minutes(5) ? g_ah.erase(it) : std::next(it);
}

TradeChatStats TradeChat_GetStats()
{
    TradeChatStats s = g_stats;
    s.talking = uint32_t(std::count_if(g_talks.begin(), g_talks.end(), [](Talk const& t) { return !t.over; }));
    return s;
}

void TradeChat_NoteFallback()
{
    ++g_stats.fallbacks;
}
