#include "mod-ollama-chat_gifts.h"
#include "mod-ollama-chat_answer.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Bag.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "TradeData.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "AiObjectContext.h"
#include "ItemUsageValue.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // What a player gave away or put up for sale, where they said it: the
    // next line from them there with no link of its own ("5g then", "its
    // free") is about the same thing.
    struct Pending
    {
        uint64_t          player = 0;
        std::string       scopeKey;
        uint32_t          entry  = 0;
        int32_t           suffix = 0;
        bool              sale   = false;
        uint32_t          price  = 0;   // copper; 0 = not named
        uint64_t          taker  = 0;   // the bot that said yes, or asked the price
        Clock::time_point when{};
    };
    std::vector<Pending> g_pending;

    // A bot agreed to pay `price` for `entry`: when the player puts it in a
    // trade window with it, the money goes in on its side.
    struct Deal
    {
        uint64_t          bot    = 0;
        uint64_t          player = 0;
        uint32_t          entry  = 0;
        uint32_t          price  = 0;
        bool              filled = false;   // the money is in a window between them right now
        uint32_t          moneyBefore = 0;  // the bot's money when it went in
        Clock::time_point until{};
    };
    std::vector<Deal> g_deals;

    enum class SaidKind : uint8_t { Gift, Buy, Counter, AskPrice };

    // What a bot said, for its replies afterwards.
    struct Said
    {
        uint64_t          bot    = 0;
        uint64_t          player = 0;
        uint32_t          entry  = 0;
        uint32_t          price  = 0;
        SaidKind          kind   = SaidKind::Gift;
        Clock::time_point since{};   // decided then; said a little after
        Clock::time_point until{};
    };
    std::vector<Said> g_said;

    // Its answer is still being written: its own prompt for it is not told it
    // has already said so.
    constexpr auto kSaying = std::chrono::seconds(15);

    // The lines that gave something away, so their replies list nothing else.
    struct OfferLine
    {
        uint64_t          player = 0;
        std::string       text;
        Clock::time_point when{};
    };
    std::vector<OfferLine> g_lines;

    GiftsStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    bool Chance(uint32_t percent)
    {
        if (percent == 0)
            return false;
        if (percent >= 100)
            return true;
        return std::uniform_int_distribution<uint32_t>(1, 100)(Rng()) <= percent;
    }

    Player* Find(uint64_t guid)
    {
        return guid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)) : nullptr;
    }

    bool SameGroup(Player* a, Player* b)
    {
        return a && b && a->GetGroup() && a->GetGroup() == b->GetGroup();
    }

    // Playerbots trades with a group member only, and not at all when
    // random-bot trading is off or only lets them sell.
    bool BotsTake()
    {
        return sPlayerbotAIConfig.EnableRandomBotTrading != 0 && sPlayerbotAIConfig.EnableRandomBotTrading != 3;
    }

    std::string ItemName(ItemTemplate const* proto)
    {
        return proto ? "[" + proto->Name1 + "]" : std::string("it");
    }

    // An incomplete quest of the bot's own that needs this item, or empty.
    std::string QuestNeeding(Player* bot, uint32_t entry)
    {
        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == entry && bot->GetItemCount(entry, true) < quest->RequiredItemCount[i])
                    return "you still need it for the quest [" + quest->GetTitle() + "]";
        }
        return {};
    }

    struct Room
    {
        uint32_t emptyBagSlots = 0;   // bag slots with no bag in
        uint32_t smallest      = 0;   // slots in its smallest bag (0: no bag at all)
        uint32_t free          = 0;   // empty slots, backpack included
    };

    Room RoomOf(Player* bot)
    {
        Room r;
        uint32_t smallest = 0;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                ++r.free;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            Bag* bag = bot->GetBagByPos(slot);
            if (!bag)
            {
                ++r.emptyBagSlots;
                continue;
            }
            smallest = smallest ? std::min(smallest, bag->GetBagSize()) : bag->GetBagSize();
            r.free += bag->GetFreeSlots();
        }
        r.smallest = smallest;
        return r;
    }

    // How much use the thing is to a bot, and why -- in its own words.
    struct Judgement
    {
        int         score = 0;    // 0: none; 40+: it wants it as a gift; 50+: it would pay for it
        std::string why;
    };

    Judgement Judge(Player* bot, PlayerbotAI* ai, ItemTemplate const* proto, int32_t suffix)
    {
        Judgement j;
        const std::string quest = QuestNeeding(bot, proto->ItemId);
        if (!quest.empty())
        {
            j.score = 95;
            j.why   = quest;
            return j;
        }

        if (proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER)
        {
            const Room room = RoomOf(bot);
            if (room.emptyBagSlots)
            {
                j.score = 80;
                j.why   = "you have an empty bag slot to put it in";
            }
            else if (room.smallest < proto->ContainerSlots)
            {
                j.score = 75;
                j.why   = fmt::format("it has {} slots and your smallest bag only {}", proto->ContainerSlots,
                                      room.smallest);
            }
            else
            {
                j.why = fmt::format("your bags are all {} slots or bigger already", room.smallest);
                return j;
            }
            if (room.free <= 4)
            {
                j.score += 15;
                j.why += room.free ? ", and your bags are nearly full" : ", and your bags are full";
            }
            return j;
        }

        const std::string qualifier = suffix ? fmt::format("{},{}", proto->ItemId, suffix)
                                             : std::to_string(proto->ItemId);
        Value<ItemUsage>* value = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier);
        switch (value ? value->Get() : ITEM_USAGE_NONE)
        {
            case ITEM_USAGE_QUEST:        j.score = 95; j.why = "you need it for a quest"; break;
            case ITEM_USAGE_EQUIP:
            case ITEM_USAGE_REPLACE:      j.score = 85; j.why = "it would be an upgrade on what you wear"; break;
            case ITEM_USAGE_BROKEN_EQUIP: j.score = 85; j.why = "it would replace a piece of yours that is broken"; break;
            case ITEM_USAGE_SKILL:        j.score = 60; j.why = "you could use it for your profession"; break;
            case ITEM_USAGE_AMMO:         j.score = 55; j.why = "it is ammo you can use"; break;
            case ITEM_USAGE_USE:          j.score = 50; j.why = "you would use it"; break;
            case ITEM_USAGE_KEEP:         j.score = 40; j.why = "you would hang on to it"; break;
            case ITEM_USAGE_AH:
            case ITEM_USAGE_VENDOR:
            case ITEM_USAGE_DISENCHANT:
            {
                const std::string style = GetBotPersonality(bot);
                const bool greedy = std::find(g_GiftsGreedyPersonalities.begin(), g_GiftsGreedyPersonalities.end(),
                                              style) != g_GiftsGreedyPersonalities.end();
                j.score = greedy ? 45 : 10;
                j.why   = "you have no real use for it, though it would sell";
                break;
            }
            case ITEM_USAGE_BAD_EQUIP:    j.why = "it is no better than what you already wear"; break;
            default:
                j.why = bot->CanUseItem(proto) != EQUIP_ERR_OK ? "you cannot use it"
                                                                 : "you have no use for it";
                break;
        }
        return j;
    }

    // What a bot pays for something at most: playerbots' own value of it --
    // what a vendor would charge, times this bot's buy rate -- so the trade
    // check it makes when the window is accepted always passes.
    uint32_t MostItPays(Player* bot, ItemTemplate const* proto)
    {
        if (!proto || proto->Quality < ITEM_QUALITY_NORMAL || !proto->BuyPrice)
            return 0;
        // The same test its trade check prices by: a random-bot account.
        const bool priced = bot->GetSession() &&
                            sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId());
        const double rate = priced ? sRandomPlayerbotMgr.GetBuyMultiplier(bot) : 1.0;
        return uint32_t(std::floor(double(proto->BuyPrice) * rate));
    }

    // A round counter-offer at or below `cap`: "35s", not "34s 77c".
    uint32_t RoundDown(uint32_t cap)
    {
        if (cap >= 100000)
            return cap / 10000 * 10000;   // whole gold from 10g
        if (cap >= 10000)
            return cap / 5000 * 5000;     // 50s steps from 1g
        if (cap >= 1000)
            return cap / 500 * 500;       // 5s steps from 10s
        if (cap >= 100)
            return cap / 100 * 100;       // whole silver
        return cap;
    }

    // Tradeable at all: soulbound things are not, unless still in their loot
    // trade window.
    bool CanBeTraded(Player* player, uint32_t entry)
    {
        bool found = false;
        auto check = [&](Item* item)
        {
            if (!item || item->GetEntry() != entry)
                return false;
            found = true;
            return !item->IsSoulBound() || item->IsBOPTradable();
        };
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (check(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
                return true;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag* bag = player->GetBagByPos(slot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    if (check(bag->GetItemByPos(uint8(i))))
                        return true;
        return !found;   // not on them: nothing to say it is bound
    }

    std::string Format(const std::string& templ, Player* player, ItemTemplate const* proto, const std::string& why,
                       uint32_t price = 0, uint32_t counter = 0, uint32_t money = 0,
                       const std::string& first = std::string())
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ,
                                           fmt::arg("player_name", player->GetName()),
                                           fmt::arg("item", ItemName(proto)),
                                           fmt::arg("why", why),
                                           fmt::arg("price", TextRules::MoneyWords(price)),
                                           fmt::arg("counter", TextRules::MoneyWords(counter)),
                                           fmt::arg("money", TextRules::MoneyWords(money)),
                                           fmt::arg("first", first));
        return out == "[Format Error]" ? std::string() : out;
    }

    void RememberSaid(Player* bot, Player* player, uint32_t entry, uint32_t price, SaidKind kind)
    {
        const uint64_t b = bot->GetGUID().GetRawValue();
        g_said.erase(std::remove_if(g_said.begin(), g_said.end(),
                                    [&](const Said& s) { return s.bot == b && s.player == player->GetGUID().GetRawValue(); }),
                     g_said.end());
        const auto now = Clock::now();
        g_said.push_back({ b, player->GetGUID().GetRawValue(), entry, price, kind, now,
                           now + std::chrono::seconds(g_GiftsRememberSeconds) });
    }

    void RememberDeal(Player* bot, Player* player, uint32_t entry, uint32_t price)
    {
        const uint64_t b = bot->GetGUID().GetRawValue(), p = player->GetGUID().GetRawValue();
        g_deals.erase(std::remove_if(g_deals.begin(), g_deals.end(),
                                     [&](const Deal& d) { return d.bot == b && d.player == p && !d.filled; }),
                      g_deals.end());
        Deal d;
        d.bot    = b;
        d.player = p;
        d.entry  = entry;
        d.price  = price;
        d.until  = Clock::now() + std::chrono::seconds(g_GiftsRememberSeconds);
        g_deals.push_back(d);
    }

    struct Voice
    {
        Player*     bot = nullptr;
        std::string hint;
    };

    // The voices, in order: a whisper is answered by whisper, anything else
    // as one crowd plan that nobody else joins.
    void Speak(Player* player, const std::string& msg, ChatChannelSourceLocal source, const std::string& channelName,
               const std::string& scopeKey, uint64_t seq, std::vector<Voice> voices)
    {
        voices.erase(std::remove_if(voices.begin(), voices.end(),
                                    [](const Voice& v) { return !v.bot || v.hint.empty(); }),
                     voices.end());
        if (voices.empty())
            return;

        if (source == SRC_WHISPER_LOCAL)
        {
            OllamaBotAnswer(player, voices.front().bot, msg, source, scopeKey, seq, voices.front().hint,
                            CrowdLineKind::Favor);
            return;
        }

        CrowdPlan plan;
        plan.key          = Crowd_KeyFor(scopeKey, source, uint32(player->GetTeamId()));
        plan.scopeKey     = scopeKey;
        plan.source       = source;
        plan.channelName  = channelName;
        plan.speakerGuid  = player->GetGUID().GetRawValue();
        plan.speakerIsBot = false;
        plan.speakerName  = player->GetName();
        plan.message      = msg;
        plan.messageSeq   = seq;
        plan.kind         = CrowdLineKind::Favor;
        plan.listeners    = uint32_t(voices.size());
        plan.closed       = true;
        for (Voice& v : voices)
        {
            CrowdSlot slot;
            slot.botGuid   = v.bot->GetGUID().GetRawValue();
            slot.addressed = true;
            slot.hint      = std::move(v.hint);
            plan.slots.push_back(std::move(slot));
        }
        Crowd_Start(std::move(plan), 0);
    }

    // The bots that could answer: the one whispered; else the group's bots on
    // the player's map -- or only the one named in the line.
    std::vector<Player*> Listeners(Player* player, ChatChannelSourceLocal source, Player* receiver,
                                   const std::vector<std::string>& words)
    {
        std::vector<Player*> out;
        if (source == SRC_WHISPER_LOCAL)
        {
            if (receiver && receiver->IsInWorld() && OllamaIsBotPlayer(receiver))
                out.push_back(receiver);
            return out;
        }
        Group* group = player->GetGroup();
        if (!group)
            return out;
        std::vector<Player*> named;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player || !member->IsInWorld() || member->GetMap() != player->GetMap() ||
                !OllamaIsBotPlayer(member))
                continue;
            out.push_back(member);
            const std::vector<std::string> name = CrowdText::Words(member->GetName());
            if (!name.empty() && std::find(words.begin(), words.end(), name.front()) != words.end())
                named.push_back(member);
        }
        if (named.size() == 1)
            return named;
        // A raid: a dozen of them is plenty to find who could use it.
        constexpr size_t kMostAsked = 12;
        if (out.size() > kMostAsked)
        {
            std::shuffle(out.begin(), out.end(), Rng());
            out.resize(kMostAsked);
        }
        return out;
    }

    Pending* LatestPending(uint64_t player, const std::string& scopeKey)
    {
        const auto now = Clock::now();
        for (auto it = g_pending.rbegin(); it != g_pending.rend(); ++it)
            if (it->player == player && it->scopeKey == scopeKey &&
                now - it->when <= std::chrono::seconds(g_GiftsFollowUpSeconds))
                return &*it;
        return nullptr;
    }

    // One bot answering a sale at `price` (0: none named). Records what it
    // agreed, and returns its instruction.
    std::string SaleAnswer(Player* bot, Player* player, ItemTemplate const* proto, const Judgement& j,
                           uint32_t price, const std::string& note)
    {
        if (j.score < 50)
            return Format(g_GiftsHintNoBuy, player, proto, j.why, price);

        const uint32_t money  = bot->GetMoney();
        const uint32_t afford = uint32_t(uint64_t(money) * g_GiftsMaxSpendPercent / 100);
        const uint32_t cap    = std::min(MostItPays(bot, proto), afford);
        if (!price)
        {
            ++g_stats.asked;
            RememberSaid(bot, player, proto->ItemId, 0, SaidKind::AskPrice);
            return Format(g_GiftsHintAskPrice, player, proto, j.why) + note;
        }
        if (price <= cap)
        {
            ++g_stats.agreed;
            RememberDeal(bot, player, proto->ItemId, price);
            RememberSaid(bot, player, proto->ItemId, price, SaidKind::Buy);
            return Format(g_GiftsHintBuy, player, proto, j.why, price) + note;
        }
        const uint32_t counter = RoundDown(cap);
        if (counter >= 100 && counter * 4 >= price)   // within reason: not 1s for a 10g ask
        {
            ++g_stats.countered;
            RememberDeal(bot, player, proto->ItemId, counter);
            RememberSaid(bot, player, proto->ItemId, counter, SaidKind::Counter);
            return Format(g_GiftsHintCounter, player, proto, j.why, price, counter) + note;
        }
        return Format(g_GiftsHintTooDear, player, proto, j.why, price, 0, money);
    }
}

bool Gifts_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                           const std::string& channelName, Player* receiver, const std::string& scopeKey,
                           uint64_t seq)
{
    if (!g_GiftsEnable || !player || msg.empty() || !player->IsInWorld() || OllamaIsBotPlayer(player) || !BotsTake())
        return false;
    switch (source)
    {
        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:   if (g_DisableForParty || !player->GetGroup()) return false; break;
        case SRC_WHISPER_LOCAL: if (!g_EnableWhisperReplies) return false;                break;
        default:               return false;
    }

    // What they wrote around the links: "[Gold Ring]" is not a price.
    const std::string clean  = Context_CleanText(msg);
    const std::string around = TextRules::WithoutBracketNames(clean);
    const std::vector<std::string> words = CrowdText::Words(around);
    const uint32_t price = TextRules::ParsePrice(around);
    const bool sellWords  = CrowdText::ContainsPhrase(words, g_GiftsSellWords);
    const bool offerWords = CrowdText::ContainsPhrase(words, g_GiftsOfferWords);

    // The thing: the first item linked that they have on them. No link: the
    // one they just gave away or put up for sale here, when the line is still
    // about that ("its free", "5g then").
    uint32_t entry  = 0;
    int32_t  suffix = 0;
    for (const TextRules::ChatLink& link : TextRules::ExtractChatLinks(msg))
    {
        if (link.type != "item" || !link.id || !player->HasItemCount(link.id, 1, false))
            continue;
        entry = link.id;
        // |Hitem:entry:enchant:gem1:gem2:gem3:gem4:suffix:...
        size_t at = link.markup.find("Hitem:");
        for (int field = 0; at != std::string::npos && field < 7; ++field)
            at = link.markup.find(':', at + 1);
        if (at != std::string::npos)
            suffix = std::atoi(link.markup.c_str() + at + 1);
        break;
    }

    Pending* before = LatestPending(player->GetGUID().GetRawValue(), scopeKey);
    bool followUp = false;
    if (!entry)
    {
        if (!before || (!offerWords && !sellWords && !price))
            return false;
        // An answer already went out: a price now goes to the bot that asked
        // for one, a "free" to a gift nobody took yet. Anything else is talk.
        if (before->taker && !(before->sale && price))
            return false;
        // "dont need it" while a bot is handing them something is calling
        // that off (mod-ollama-chat_bags), not giving theirs away again.
        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    if (m != player && OllamaIsBotPlayer(m) && Conjure_IsGiving(m, player, 0))
                        return false;
        entry    = before->entry;
        suffix   = before->suffix;
        followUp = true;
    }

    const bool sale  = sellWords || price || (followUp && before->sale && !offerWords);
    const bool offer = !sale && (offerWords || (followUp && !before->sale));
    if (!sale && !offer)
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto)
        return false;

    std::vector<Player*> bots = Listeners(player, source, receiver, words);
    if (bots.empty())
        return false;

    // A price for the bot that asked how much: it answers, nobody else.
    Player* asker = (followUp && before->sale && before->taker) ? Find(before->taker) : nullptr;
    if (followUp && before->sale && before->taker)
    {
        if (!asker || std::find(bots.begin(), bots.end(), asker) == bots.end())
            return false;
        bots = { asker };
    }

    // In the transcript before anyone answers it.
    auto record = [&]()
    {
        if (!seq)
        {
            Governor_NoteHumanMessage(scopeKey);
            seq = Context_RecordLine(player, source, channelName, msg, ContextLineKind::Human);
        }
        g_lines.push_back({ player->GetGUID().GetRawValue(), msg, Clock::now() });
    };

    Pending fresh;
    Pending& p = followUp ? *before : fresh;
    if (!followUp)
    {
        p.player   = player->GetGUID().GetRawValue();
        p.scopeKey = scopeKey;
        p.entry    = entry;
        p.suffix   = suffix;
    }
    p.sale  = sale;
    p.price = price ? price : p.price;
    p.when  = Clock::now();

    std::vector<Voice> voices;

    if (!CanBeTraded(player, entry))
    {
        if (followUp)
            return false;   // said once already
        Player* bot = bots[std::uniform_int_distribution<size_t>(0, bots.size() - 1)(Rng())];
        voices.push_back({ bot, Format(g_GiftsHintNotTradable, player, proto, std::string()) });
        record();
        g_pending.push_back(p);
        Speak(player, msg, source, channelName, scopeKey, seq, std::move(voices));
        return true;
    }

    // How much use it is to each of them, the most first.
    struct Candidate
    {
        Player*   bot = nullptr;
        Judgement j;
        uint32_t  tie = 0;
    };
    std::vector<Candidate> all;
    for (Player* bot : bots)
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            all.push_back({ bot, Judge(bot, ai, proto, suffix), uint32_t(Rng()()) });
    if (all.empty())
        return false;
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b)
              { return a.j.score != b.j.score ? a.j.score > b.j.score : a.tie < b.tie; });

    const int need = sale ? 50 : 40;
    std::vector<Candidate*> wanters, others;
    for (Candidate& c : all)
        (c.j.score >= need ? wanters : others).push_back(&c);

    // Not in their group: playerbots only trades with its group.
    auto inviteNote = [&](Player* bot) -> std::string
    {
        return SameGroup(player, bot) || g_GiftsInviteText.empty() ? std::string() : " " + g_GiftsInviteText;
    };

    if (sale)
    {
        if (!followUp)
            ++g_stats.sales;
        if (!wanters.empty())
        {
            Candidate& c = *wanters.front();
            voices.push_back({ c.bot, SaleAnswer(c.bot, player, proto, c.j, p.price, inviteNote(c.bot)) });
            p.taker = c.bot->GetGUID().GetRawValue();
        }
        else if (Chance(g_GiftsNobodyChance) || bots.size() == 1)
        {
            ++g_stats.nobody;
            Candidate& c = all[std::uniform_int_distribution<size_t>(0, all.size() - 1)(Rng())];
            voices.push_back({ c.bot, Format(g_GiftsHintNoBuy, player, proto, c.j.why, p.price) });
        }
    }
    else
    {
        if (!followUp)
            ++g_stats.offers;
        if (!wanters.empty())
        {
            ++g_stats.wanted;
            Candidate& first = *wanters.front();
            voices.push_back({ first.bot, Format(g_GiftsHintWant, player, proto, first.j.why) + inviteNote(first.bot) });
            p.taker = first.bot->GetGUID().GetRawValue();
            RememberSaid(first.bot, player, entry, 0, SaidKind::Gift);

            // Now and then a second voice: another who could have used it, or
            // a no thanks.
            if (bots.size() > 1 && Chance(g_GiftsSecondVoiceChance))
            {
                if (wanters.size() > 1)
                    voices.push_back({ wanters[1]->bot, Format(g_GiftsHintWantToo, player, proto, wanters[1]->j.why, 0,
                                                               0, 0, first.bot->GetName()) });
                else if (!others.empty())
                {
                    Candidate& c = *others[std::uniform_int_distribution<size_t>(0, others.size() - 1)(Rng())];
                    voices.push_back({ c.bot, Format(g_GiftsHintNoNeed, player, proto, c.j.why) });
                }
            }
        }
        else if (Chance(g_GiftsNobodyChance) || bots.size() == 1)
        {
            ++g_stats.nobody;
            Candidate& c = all[std::uniform_int_distribution<size_t>(0, all.size() - 1)(Rng())];
            voices.push_back({ c.bot, Format(g_GiftsHintNoNeed, player, proto, c.j.why) });
        }
    }

    // Said again with nobody to answer it: ordinary chat from here.
    if (followUp && voices.empty())
        return false;

    record();
    if (!followUp)
        g_pending.push_back(p);
    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} {} {}{}: {} of {} could use it{}.", player->GetName(),
                 sale ? "is selling" : "is giving away", proto->Name1,
                 p.price ? " for " + TextRules::MoneyWords(p.price) : std::string(), wanters.size(), all.size(),
                 voices.empty() ? ", nobody answers" : ", " + voices.front().bot->GetName() + " answers");
    Speak(player, msg, source, channelName, scopeKey, seq, std::move(voices));
    return true;
}

bool Gifts_IsOfferLine(Player* player, const std::string& msg)
{
    if (!player || g_lines.empty())
        return false;
    const uint64_t guid = player->GetGUID().GetRawValue();
    const auto now = Clock::now();
    for (const OfferLine& l : g_lines)
        if (l.player == guid && l.text == msg && now - l.when < std::chrono::minutes(2))
            return true;
    return false;
}

std::string Gifts_PromptFor(Player* bot)
{
    if (!g_GiftsEnable || !bot || g_said.empty())
        return {};
    const uint64_t guid = bot->GetGUID().GetRawValue();
    const auto now = Clock::now();
    std::string out;
    for (const Said& s : g_said)
    {
        if (s.bot != guid || now >= s.until || now - s.since < kSaying)
            continue;
        Player* player = Find(s.player);
        if (!player)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(s.entry);
        const std::string& templ = s.kind == SaidKind::Gift    ? g_GiftsSaidGiftText
                                 : s.kind == SaidKind::Buy     ? g_GiftsSaidBuyText
                                 : s.kind == SaidKind::Counter ? g_GiftsSaidCounterText
                                                               : g_GiftsSaidAskText;
        const std::string line = Format(templ, player, proto, std::string(), s.price);
        if (!line.empty())
            out += (out.empty() ? "" : " ") + line;
    }
    return out;
}

void Gifts_Tick()
{
    if (g_deals.empty())
        return;
    const auto now = Clock::now();
    for (auto it = g_deals.begin(); it != g_deals.end();)
    {
        Deal& d = *it;
        Player* bot    = Find(d.bot);
        Player* player = Find(d.player);
        if (!bot || !player || !bot->IsInWorld() || !player->IsInWorld())
        {
            it = g_deals.erase(it);
            continue;
        }

        TradeData* mine   = bot->GetTradeData();
        TradeData* theirs = player->GetTradeData();
        const bool open = mine && theirs && bot->GetTrader() == player;
        if (!open)
        {
            if (d.filled)
            {
                // The window closed with the money in it: paid, if the money
                // has gone; else cancelled, and it can be tried again.
                d.filled = false;
                if (bot->GetMoney() + d.price <= d.moneyBefore + 1 && bot->HasItemCount(d.entry, 1, false))
                {
                    ++g_stats.paid;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] {} paid {} {} for {}.", bot->GetName(),
                                 player->GetName(), TextRules::MoneyWords(d.price), d.entry);
                    it = g_deals.erase(it);
                    continue;
                }
            }
            it = (now >= d.until) ? g_deals.erase(it) : std::next(it);
            continue;
        }

        bool offered = false;
        for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT && !offered; ++slot)
            if (Item* item = theirs->GetItem(TradeSlots(slot)))
                offered = item->GetEntry() == d.entry;

        const uint32_t want = offered ? d.price : 0;
        if (mine->GetMoney() != want && (!want || bot->HasEnoughMoney(want)) &&
            (offered || d.filled))   // only ever take out money it put in
        {
            if (offered && !d.filled)
                d.moneyBefore = bot->GetMoney();
            WorldPacket packet(CMSG_SET_TRADE_GOLD, 4);
            packet << uint32(want);
            bot->GetSession()->HandleSetTradeGoldOpcode(packet);
            d.filled = offered;
        }
        ++it;
    }
}

void Gifts_Update()
{
    const auto now = Clock::now();
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                   [&](const Pending& p) { return now - p.when > std::chrono::seconds(g_GiftsRememberSeconds); }),
                    g_pending.end());
    g_said.erase(std::remove_if(g_said.begin(), g_said.end(), [&](const Said& s) { return now >= s.until; }),
                 g_said.end());
    g_lines.erase(std::remove_if(g_lines.begin(), g_lines.end(),
                                 [&](const OfferLine& l) { return now - l.when > std::chrono::minutes(2); }),
                  g_lines.end());
    if (g_pending.size() > 200)
        g_pending.erase(g_pending.begin(), g_pending.begin() + (g_pending.size() - 200));
    if (g_lines.size() > 200)
        g_lines.erase(g_lines.begin(), g_lines.begin() + (g_lines.size() - 200));
}

GiftsStats Gifts_GetStats()
{
    GiftsStats s = g_stats;
    s.deals = uint32_t(g_deals.size());
    return s;
}
