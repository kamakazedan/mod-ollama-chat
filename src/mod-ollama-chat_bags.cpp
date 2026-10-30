#include "mod-ollama-chat_bags.h"
#include "mod-ollama-chat_answer.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_conjure.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
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
#include "Player.h"
#include "QuestDef.h"

#include "AiObjectContext.h"
#include "ItemUsageValue.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr size_t kLootKept = 12;   // per character: what it looted last

    struct Looted
    {
        uint32_t          entry = 0;
        Clock::time_point when{};
    };
    // Written from map threads (bots loot there), read on the world thread.
    std::mutex g_lootMutex;
    std::unordered_map<uint64_t, std::vector<Looted>> g_loot;

    struct Offer
    {
        uint64_t          bot   = 0;
        uint32_t          entry = 0;
        Clock::time_point when{};
    };
    std::unordered_map<std::string, std::vector<Offer>> g_offers;   // by scope key, newest last

    // Told to invite it first: once the two are grouped, the trade starts by
    // itself rather than waiting to be asked again.
    struct Promise
    {
        uint64_t          bot    = 0;
        uint64_t          player = 0;
        uint32_t          entry  = 0;
        uint32_t          count  = 0;
        Clock::time_point until{};
    };
    std::vector<Promise> g_promises;

    BagsStats g_stats;

    Player* Find(uint64_t guid)
    {
        return guid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)) : nullptr;
    }

    bool InGroupWith(Player* a, Player* b)
    {
        Group* group = a ? a->GetGroup() : nullptr;
        return group && b && group->IsMember(b->GetGUID());
    }

    bool RecentlyLooted(uint64_t guid, uint32_t entry, Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(g_lootMutex);
        auto it = g_loot.find(guid);
        if (it == g_loot.end())
            return false;
        for (const Looted& l : it->second)
            if (l.entry == entry && now - l.when <= std::chrono::minutes(g_BagsRecentMinutes))
                return true;
        return false;
    }

    std::string QuestReason(Player* bot, uint32_t entry)
    {
        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == entry)
                    return fmt::format("you need it for the quest [{}]", quest->GetTitle());
        }
        return "you need it for a quest";
    }

    // Why the bot keeps this, or empty when it could spare it. Playerbots'
    // item usage decides, as it does for what it loots and sells.
    std::string KeepReason(Player* bot, PlayerbotAI* ai, Item* item, ItemTemplate const* proto)
    {
        if (item->IsSoulBound())
            return "it is soulbound, so it cannot be traded";
        if (proto->Class == ITEM_CLASS_QUEST)
            return QuestReason(bot, proto->ItemId);

        const int32 suffix = item->GetItemRandomPropertyId();
        const std::string qualifier = suffix ? fmt::format("{},{}", proto->ItemId, suffix)
                                             : std::to_string(proto->ItemId);
        Value<ItemUsage>* value = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier);
        switch (value ? value->Get() : ITEM_USAGE_KEEP)
        {
            case ITEM_USAGE_EQUIP:
            case ITEM_USAGE_REPLACE:      return "it is an upgrade you mean to put on";
            case ITEM_USAGE_BROKEN_EQUIP: return "it is to replace gear of yours that is broken";
            case ITEM_USAGE_QUEST:        return QuestReason(bot, proto->ItemId);
            case ITEM_USAGE_SKILL:        return "you use it for your profession";
            case ITEM_USAGE_USE:          return "you use those yourself";
            case ITEM_USAGE_AMMO:         return "it is your ammo";
            case ITEM_USAGE_GUILD_TASK:   return "it is for a guild task";
            case ITEM_USAGE_KEEP:         return "you are keeping it";
            default:                      return {};   // nothing, a worse piece, to sell or disenchant
        }
    }

    // A kept thing worth naming: what someone would actually ask about.
    int KeptRank(const BagEntry& e)
    {
        if (e.why.find("quest") != std::string::npos)
            return 0;
        if (e.why.find("upgrade") != std::string::npos)
            return 1;
        if (e.why.find("profession") != std::string::npos)
            return 2;
        if (e.why.find("yourself") != std::string::npos)
            return 3;
        return 4;
    }

    std::string MoneyText(uint32_t copper)
    {
        const uint32_t gold = copper / 10000, silver = (copper / 100) % 100, rest = copper % 100;
        if (gold)
            return silver ? fmt::format("{}g {}s", gold, silver) : fmt::format("{}g", gold);
        if (silver)
            return fmt::format("{}s", silver);
        return fmt::format("{}c", rest);
    }

    std::string ItemWords(const BagEntry& e, uint32_t count)
    {
        return count > 1 ? fmt::format("[{}] x{}", e.name, count) : fmt::format("[{}]", e.name);
    }

    // Item entries linked in a line: |Hitem:2589:...
    std::vector<uint32_t> LinkedEntries(const std::string& text)
    {
        std::vector<uint32_t> out;
        for (size_t at = text.find("|Hitem:"); at != std::string::npos; at = text.find("|Hitem:", at + 7))
        {
            const uint32_t entry = uint32_t(std::strtoul(text.c_str() + at + 7, nullptr, 10));
            if (entry && std::find(out.begin(), out.end(), entry) == out.end())
                out.push_back(entry);
        }
        return out;
    }

    // Words that say nothing about which thing is meant.
    bool IsCommonWord(const std::string& w)
    {
        static const char* const kCommon[] = { "your", "that", "this", "them", "those", "these", "some", "please",
                                               "thanks", "with", "have", "take", "want", "give", "trade", "need",
                                               "spare", "mate", "just", "from", "there", "here", "what", "where",
                                               "would", "could", "cheers", "really" };
        return std::any_of(std::begin(kCommon), std::end(kCommon), [&](const char* c) { return w == c; }) ||
               CrowdText::ContainsPhrase({ w }, g_BagsYesPhrases);
    }

    // "can I have it", "ill take that one": an ask that points back at what
    // was offered rather than naming something -- a word like "it", and no
    // word that could be the name of something else ("can i have that sword").
    bool PointsAtOffer(const std::vector<std::string>& words)
    {
        static const char* const kPointers[] = { "it", "that", "those", "them", "this", "one", "these" };
        bool pointer = false;
        for (const std::string& w : words)
        {
            if (std::any_of(std::begin(kPointers), std::end(kPointers), [&](const char* p) { return w == p; }))
                pointer = true;
            else if (w.size() >= 4 && !IsCommonWord(w))
            {
                bool askWord = false;
                for (const std::vector<std::string>& phrase : g_BagsAskPhrases)
                    askWord = askWord || std::find(phrase.begin(), phrase.end(), w) != phrase.end();
                if (!askWord)
                    return false;
            }
        }
        return pointer;
    }

    // Which of `items` the line asks for: a link, then a name it contains
    // (longest first), then a word of four letters or more that only one of
    // the names has ("the leggings").
    const BagEntry* MatchAsked(const std::vector<BagEntry>& items, const std::vector<uint32_t>& linked,
                               const std::string& clean, const std::vector<std::string>& words)
    {
        for (uint32_t entry : linked)
            for (const BagEntry& e : items)
                if (e.entry == entry)
                    return &e;

        const std::string text = TextRules::NameKey(clean);
        const BagEntry* best = nullptr;
        for (const BagEntry& e : items)
        {
            const std::string name = TextRules::NameKey(e.name);
            if (!name.empty() && text.find(name) != std::string::npos && (!best || name.size() > best->name.size()))
                best = &e;
        }
        if (best)
            return best;

        const BagEntry* only = nullptr;
        for (const std::string& w : words)
        {
            if (w.size() < 4 || IsCommonWord(w))
                continue;
            const BagEntry* hit = nullptr;
            bool twice = false;
            for (const BagEntry& e : items)
            {
                const std::vector<std::string> nameWords = CrowdText::Words(e.name);
                if (std::find(nameWords.begin(), nameWords.end(), w) == nameWords.end())
                    continue;
                twice = twice || hit;
                hit = &e;
            }
            if (hit && !twice)
            {
                if (only && only != hit)
                    return nullptr;   // two words, two different things: unclear
                only = hit;
            }
        }
        return only;
    }

    // The newest offer made in this place lately.
    const Offer* LatestOffer(const std::string& scopeKey, Clock::time_point now)
    {
        auto it = g_offers.find(scopeKey);
        if (it == g_offers.end() || it->second.empty())
            return nullptr;
        const Offer& last = it->second.back();
        return now - last.when <= std::chrono::seconds(g_BagsOfferSeconds) ? &last : nullptr;
    }

    // Whom the line names first: one of the group, or in /say a bot standing
    // near; for a whisper, whoever it was whispered to. `person`: a real
    // player was named first, so the line is theirs.
    Player* NamedBot(Player* player, const std::string& msg, ChatChannelSourceLocal source, Player* receiver,
                     bool& person)
    {
        person = false;
        if (source == SRC_WHISPER_LOCAL)
            return (receiver && receiver != player && OllamaIsBotPlayer(receiver)) ? receiver : nullptr;

        Player* found = nullptr;
        size_t bestPos = std::string::npos;
        auto consider = [&](Player* p)
        {
            if (!p || p == player || !p->IsInWorld())
                return;
            const size_t pos = OllamaFindBotNameMention(msg, p->GetName());
            if (pos == std::string::npos || pos >= bestPos)
                return;
            bestPos = pos;
            person  = !OllamaIsBotPlayer(p);
            found   = person ? nullptr : p;
        };
        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                consider(ref->GetSource());
        if (!found && !person && source == SRC_SAY_LOCAL)
        {
            std::list<Player*> near;
            Acore::AnyPlayerInObjectRangeCheck check(player, g_SayDistance, false, true);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(player, near, check);
            Cell::VisitObjects(player, searcher, g_SayDistance);
            for (Player* p : near)
                consider(p);
        }
        return found;
    }

    // Nobody named, in party: the bot in the group carrying what was linked
    // or named, a spare one before a kept one.
    Player* GroupBotWith(Player* player, const std::vector<uint32_t>& linked, const std::string& clean,
                         const std::vector<std::string>& words)
    {
        Group* group = player->GetGroup();
        if (!group || (linked.empty() && TextRules::FindBracketNames(clean).empty()))
            return nullptr;   // only a link or a [Name] picks one out of a whole group
        Player* keeper = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player || !member->IsInWorld() || !OllamaIsBotPlayer(member))
                continue;
            const std::vector<BagEntry> items = Bags_Scan(member);
            if (const BagEntry* e = MatchAsked(items, linked, clean, words))
            {
                if (e->spare)
                    return member;
                keeper = keeper ? keeper : member;
            }
        }
        return keeper;
    }

    // "2 cloth": how many; 0 when no number is given.
    uint32_t AskedCount(const std::vector<std::string>& words)
    {
        for (const std::string& w : words)
            if (!w.empty() && w.size() <= 3 && std::all_of(w.begin(), w.end(), ::isdigit))
                return uint32_t(std::strtoul(w.c_str(), nullptr, 10));
        return 0;
    }

    std::string Hint(const std::string& templ, Player* player, const std::string& item,
                     const std::string& when = {}, const std::string& reason = {})
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ,
                                           fmt::arg("player_name", player->GetName()),
                                           fmt::arg("item", item),
                                           fmt::arg("when", when),
                                           fmt::arg("reason", reason),
                                           fmt::arg("pass_token", g_ContextPassToken));
        return out == "[Format Error]" ? std::string() : out;
    }
}

std::vector<BagEntry> Bags_Scan(Player* bot)
{
    std::vector<BagEntry> out;
    PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!ai || !ai->GetAiObjectContext())
        return out;

    const auto now = Clock::now();
    const uint64_t guid = bot->GetGUID().GetRawValue();
    std::unordered_map<uint32_t, size_t> at;   // entry -> index in out

    auto add = [&](Item* item, bool worn)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return;
        auto found = at.find(proto->ItemId);
        if (found != at.end())
        {
            if (!worn)
                out[found->second].count += item->GetCount();
            return;
        }
        BagEntry e;
        e.entry   = proto->ItemId;
        e.count   = item->GetCount();
        e.quality = proto->Quality;
        e.name    = proto->Name1;
        e.worn    = worn;
        e.why     = worn ? std::string("you are wearing it") : KeepReason(bot, ai, item, proto);
        e.spare   = !worn && e.why.empty();
        e.recent  = RecentlyLooted(guid, e.entry, now);
        at[e.entry] = out.size();
        out.push_back(std::move(e));
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), false);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                add(bag->GetItemByPos(uint8(i)), false);
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), true);

    std::stable_sort(out.begin(), out.end(), [](const BagEntry& a, const BagEntry& b)
    {
        if (a.spare != b.spare)
            return a.spare;
        if (a.recent != b.recent)
            return a.recent;
        if (a.quality != b.quality)
            return a.quality > b.quality;
        return a.count > b.count;
    });
    return out;
}

std::string Bags_Describe(Player* bot)
{
    if (!g_BagsEnable || g_BagsPromptText.empty() || !bot)
        return {};
    const std::vector<BagEntry> items = Bags_Scan(bot);

    // Spare: what it just looted, then the better things; grey junk only
    // when there is nothing else.
    std::vector<const BagEntry*> spare, kept;
    for (const BagEntry& e : items)
        if (e.spare && e.quality > ITEM_QUALITY_POOR && spare.size() < g_BagsMaxSpare)
            spare.push_back(&e);
    if (spare.empty())
        for (const BagEntry& e : items)
            if (e.spare && spare.size() < 2)
                spare.push_back(&e);
    for (const BagEntry& e : items)
        if (!e.spare && !e.worn && e.quality > ITEM_QUALITY_POOR && e.why.find("soulbound") == std::string::npos)
            kept.push_back(&e);
    std::stable_sort(kept.begin(), kept.end(), [](const BagEntry* a, const BagEntry* b) { return KeptRank(*a) < KeptRank(*b); });
    if (kept.size() > g_BagsMaxKept)
        kept.resize(g_BagsMaxKept);

    std::string spareText, keptText;
    for (const BagEntry* e : spare)
    {
        std::string tag;
        if (e->recent)
            tag = " (just looted)";
        else if (e->quality == ITEM_QUALITY_RARE)
            tag = " (a blue)";
        else if (e->quality >= ITEM_QUALITY_EPIC)
            tag = " (an epic)";
        else if (e->quality == ITEM_QUALITY_POOR)
            tag = " (junk)";
        spareText += (spareText.empty() ? "" : ", ") + ItemWords(*e, e->count) + tag;
    }
    for (const BagEntry* e : kept)
        keptText += (keptText.empty() ? "" : ", ") + ItemWords(*e, e->count) + " (" + e->why + ")";

    const std::string out = SafeFormat(g_BagsPromptText,
                                       fmt::arg("spare", spareText.empty() ? std::string("nothing much") : spareText),
                                       fmt::arg("kept", keptText.empty() ? std::string("nothing in particular") : keptText),
                                       fmt::arg("money", MoneyText(bot->GetMoney())));
    if (out.empty() || out == "[Format Error]")
        return {};
    ++g_stats.described;
    return out;
}

bool Bags_IsItemTalk(const std::string& msg)
{
    if (!g_BagsEnable || msg.empty())
        return false;
    if (msg.find("|Hitem:") != std::string::npos || !TextRules::FindBracketNames(msg).empty())
        return true;   // a link, or one as it reads once the markup is gone: "[Linen Cloth]"
    const std::vector<std::string> words = CrowdText::Words(msg);
    return CrowdText::ContainsPhrase(words, g_BagsTalkPhrases) || CrowdText::ContainsPhrase(words, g_BagsAskPhrases);
}

void Bags_NoteLoot(Player* bot, uint32_t entry)
{
    if (!bot || !entry)
        return;
    const uint64_t guid = bot->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_lootMutex);
    std::vector<Looted>& list = g_loot[guid];
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Looted& l) { return l.entry == entry; }), list.end());
    list.push_back({ entry, Clock::now() });
    if (list.size() > kLootKept)
        list.erase(list.begin(), list.begin() + (list.size() - kLootKept));
}

void Bags_NoteOffersIn(Player* bot, const std::string& linkedText, const std::string& scopeKey)
{
    if (!g_BagsEnable || !bot || scopeKey.empty() || linkedText.find("|Hitem:") == std::string::npos)
        return;
    const std::vector<uint32_t> linked = LinkedEntries(linkedText);
    if (linked.empty())
        return;
    const std::vector<BagEntry> items = Bags_Scan(bot);
    for (uint32_t entry : linked)
        for (const BagEntry& e : items)
            if (e.entry == entry && e.spare)
                Bags_NoteOffer(bot, entry, scopeKey);
}

void Bags_NoteOffer(Player* bot, uint32_t entry, const std::string& scopeKey)
{
    if (!g_BagsEnable || !bot || !entry || scopeKey.empty())
        return;
    const uint64_t guid = bot->GetGUID().GetRawValue();
    std::vector<Offer>& list = g_offers[scopeKey];
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Offer& o) { return o.bot == guid && o.entry == entry; }),
               list.end());
    list.push_back({ guid, entry, Clock::now() });
    if (list.size() > 8)
        list.erase(list.begin());
    ++g_stats.offers;
}

bool Bags_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                          const std::string& channelName, Player* receiver, const std::string& scopeKey,
                          uint64_t seq)
{
    if (!g_BagsEnable || !player || msg.empty() || !player->IsInWorld() || OllamaIsBotPlayer(player))
        return false;
    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL && source != SRC_SAY_LOCAL &&
        source != SRC_WHISPER_LOCAL && source != SRC_GENERAL_LOCAL)
        return false;

    const std::string clean = Context_CleanText(msg);   // a link reads as its [Name]
    const std::vector<std::string> words = CrowdText::Words(clean);
    if (words.empty())
        return false;

    // "can I have", "ill take", or a line of nothing but "me!", "yes please"
    // and the like, which only counts as a claim on an offer.
    const bool ask = CrowdText::ContainsPhrase(words, g_BagsAskPhrases);
    const bool yes = words.size() <= 5 && std::all_of(words.begin(), words.end(), [](const std::string& w)
                     { return CrowdText::ContainsPhrase({ w }, g_BagsYesPhrases); });
    if ((!ask && !yes) || CrowdText::ContainsPhrase(words, g_BagsNotPhrases))
        return false;

    const auto now = Clock::now();
    const Offer* offer = LatestOffer(scopeKey, now);
    const std::vector<uint32_t> linked = LinkedEntries(msg);

    bool person = false;
    Player* bot = (source == SRC_GENERAL_LOCAL) ? nullptr : NamedBot(player, msg, source, receiver, person);
    if (person)
        return false;   // said to a real player: theirs to answer
    uint32_t offered = 0;
    if (offer && (!bot || bot->GetGUID().GetRawValue() == offer->bot))
    {
        bot     = bot ? bot : Find(offer->bot);
        offered = offer->entry;
    }
    if (!bot && (source == SRC_PARTY_LOCAL || source == SRC_RAID_LOCAL))
        bot = GroupBotWith(player, linked, clean, words);
    if (!bot || !bot->IsInWorld() || !OllamaIsBotPlayer(bot) || bot->GetMap() != player->GetMap())
        return false;

    const std::vector<BagEntry> items = Bags_Scan(bot);
    const BagEntry* want = MatchAsked(items, linked, clean, words);
    if (!want && offered && (yes || PointsAtOffer(words)))
        for (const BagEntry& e : items)
            if (e.entry == offered)
                want = &e;
    if (!want && (!ask || linked.empty()))
        return false;   // nothing it carries is meant: ordinary chat

    // The line is in the transcript before anyone answers it.
    if (!seq)
    {
        Governor_NoteHumanMessage(scopeKey);
        seq = Context_RecordLine(player, source, channelName, msg, ContextLineKind::Human);
    }
    ++g_stats.asked;

    // A General line is answered by whisper, the way people do.
    ChatChannelSourceLocal answerSource = source;
    std::string answerScope = scopeKey;
    if (source == SRC_GENERAL_LOCAL)
    {
        answerSource = SRC_WHISPER_LOCAL;
        answerScope  = Governor_MakeScopeKey(ChatChannelSourceLocalStr[SRC_WHISPER_LOCAL], 0, std::string(), 0,
                                             player->GetZoneId());
    }
    auto answer = [&](std::string hint)
    {
        OllamaBotAnswer(player, bot, msg, answerSource, answerScope, seq, std::move(hint), CrowdLineKind::Favor);
    };

    if (!want)
    {
        // A link to something it does not have.
        const std::vector<TextRules::ChatLink> links = TextRules::ExtractChatLinks(msg);
        const std::string name = links.empty() ? std::string("that") : "[" + links.front().name + "]";
        answer(Hint(g_BagsHintNotHave, player, name));
        return true;
    }

    if (!want->spare)
    {
        ++g_stats.kept;
        answer(Hint(g_BagsHintKeep, player, ItemWords(*want, 1), {}, want->why));
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked {} for {}: kept ({}).", player->GetName(),
                     bot->GetName(), want->name, want->why);
        return true;
    }

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(want->entry);
    const uint32_t stack = proto ? std::max<uint32_t>(1, proto->GetMaxStackSize()) : 1;
    const uint32_t asked = AskedCount(words);
    const uint32_t count = std::min({ want->count, stack, asked ? asked : want->count });
    const std::string itemText = ItemWords(*want, count);

    if (!InGroupWith(player, bot))
    {
        ++g_stats.invite;
        g_promises.erase(std::remove_if(g_promises.begin(), g_promises.end(),
                                        [&](const Promise& p) { return p.bot == bot->GetGUID().GetRawValue(); }),
                         g_promises.end());
        g_promises.push_back({ bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(), want->entry, count,
                               now + std::chrono::seconds(g_BagsInviteWaitSeconds) });
        answer(Hint(g_BagsHintInvite, player, itemText));
        return true;
    }

    std::string when;
    const std::string cant = Conjure_GiveItems(bot, player, { { want->entry, count } }, when);
    if (!cant.empty())
    {
        answer(Hint(g_BagsHintCant, player, itemText, {}, cant));
        return true;
    }
    ++g_stats.given;
    answer(Hint(g_BagsHintGive, player, itemText, when.empty() ? std::string("now") : when));
    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked {} for {}: trading it over{}.", player->GetName(),
                 bot->GetName(), itemText, when.empty() ? "" : " (" + when + ")");
    return true;
}

void Bags_Update()
{
    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lock(g_lootMutex);
        for (auto it = g_loot.begin(); it != g_loot.end();)
        {
            std::vector<Looted>& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const Looted& l) { return now - l.when > std::chrono::minutes(g_BagsRecentMinutes); }),
                       list.end());
            it = list.empty() ? g_loot.erase(it) : std::next(it);
        }
    }
    for (auto it = g_offers.begin(); it != g_offers.end();)
    {
        std::vector<Offer>& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const Offer& o) { return now - o.when > std::chrono::seconds(g_BagsOfferSeconds); }),
                   list.end());
        it = list.empty() ? g_offers.erase(it) : std::next(it);
    }

}

void Bags_Tick()
{
    if (g_promises.empty())
        return;
    const auto now = Clock::now();

    // Promised once they invite it: grouped now, so the trade starts.
    for (auto it = g_promises.begin(); it != g_promises.end();)
    {
        Player* bot = Find(it->bot);
        Player* player = Find(it->player);
        bool done = !bot || !player || now >= it->until;
        if (!done && InGroupWith(player, bot))
        {
            std::string when;
            if (Conjure_GiveItems(bot, player, { { it->entry, it->count } }, when).empty())
                ++g_stats.given;
            done = true;
        }
        it = done ? g_promises.erase(it) : std::next(it);
    }
}

BagsStats Bags_GetStats()
{
    return g_stats;
}
