#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat_market.h"
#include "mod-ollama-chat_clock.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_links.h"
#include "mod-ollama-chat_premises.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Bag.h"
#include "CellImpl.h"
#include "Containers.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include "AiFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <iterator>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Acore::AnyUnitInObjectRangeCheck filters out anything not alive, which
    // would make the corpse topic below unreachable. A fresh corpse is one of
    // the more interesting things a bot can remark on, so keep the dead.
    struct NearbyCreatureCheck
    {
        NearbyCreatureCheck(WorldObject const* obj, float range)
            : _obj(obj), _range(range) { }

        bool operator()(Creature* c) const
        {
            return c && _obj->IsWithinDistInMap(c, _range);
        }

        WorldObject const* _obj;
        float              _range;
    };

    struct Candidate
    {
        TopicCategory category;
        std::string   key;
        std::string   text;
        bool          isGuildTopic = false;
        bool          here = false;   // about what is right here: /say or the party only (Here)
    };

    // About what is right here, seen or heard by the bot: someone near it,
    // its group, something that just happened by it, a creature, a body, an
    // NPC or a thing in sight. Said where those here hear it -- /say, or
    // the party -- never in a channel the whole zone reads: "nice mount
    // avenico" went out in General, about a bot riding by the one who said
    // it. A danger near it may be warned of anywhere ("careful, there's an
    // elite by the tower"), and the place itself talked of.
    bool Here(const char* key)
    {
        static const std::unordered_set<std::string> keys = {
            "nearby_player", "group_member", "group_need", "recent_event", "creature", "corpse",
            "named_npc",     "vendor",       "questgiver", "gameobject" };
        return keys.contains(key);
    }

    struct WitnessedEvent
    {
        std::string       text;
        Clock::time_point when;
    };

    struct BotTopicState
    {
        std::deque<std::string>   recentKeys;
        std::deque<WitnessedEvent> memory;
    };

    std::mutex g_mutex;
    std::unordered_map<uint64_t, BotTopicState> g_state;

    // --- helpers ----------------------------------------------------------

    // urand takes uint32; container sizes are size_t. One narrowing cast here
    // instead of one at every call site.
    size_t PickIndex(size_t count)
    {
        if (count <= 1)
            return 0;
        return static_cast<size_t>(urand(0, static_cast<uint32>(count - 1)));
    }

    const std::string& PickOne(const std::vector<std::string>& list)
    {
        static const std::string empty;
        if (list.empty())
            return empty;
        return list[PickIndex(list.size())];
    }

    void Add(std::vector<Candidate>& out, TopicCategory cat, const char* key,
             const std::vector<std::string>& templates, std::string formatted)
    {
        if (templates.empty() || formatted.empty() || formatted == "[Format Error]")
            return;
        out.push_back({ cat, key, std::move(formatted), false, Here(key) });
    }

    std::string DescribePlayerState(Player* p)
    {
        if (!p)
            return "";
        if (p->isDead())          return "lying dead";
        if (p->IsInCombat())      return "fighting";
        if (p->IsMounted())       return "mounted up";
        if (p->IsInFlight())      return "flying overhead";
        if (p->isAFK())           return "standing idle";
        if (p->IsSitState())      return "sitting down";
        if (p->GetHealthPct() < 35.0f) return "badly hurt";
        return "passing by";
    }

    std::string ClassName(Player* p)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        if (ai && ai->GetChatHelper())
            return ai->GetChatHelper()->FormatClass(p->getClass());
        return "adventurer";
    }

    std::string RaceName(Player* p)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        if (ai && ai->GetChatHelper())
            return ai->GetChatHelper()->FormatRace(p->getRace());
        return "traveller";
    }

    // "Saturday afternoon": the weekday too, or the model makes one up.
    std::string TimeOfDayText()
    {
        return Clock_NowText();
    }

    // --- gatherers --------------------------------------------------------

    void GatherPeople(Player* bot, const OllamaWorldSnapshot& world, std::vector<Candidate>& out)
    {
        // Nearby players -- the category that was missing entirely, and the
        // single biggest reason bots never seemed aware of anyone.
        // Grid search: only players actually near the bot, not every character
        // online on the realm.
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck playerCheck(bot, g_TopicPlayerRadius, false, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> playerSearcher(bot, found, playerCheck);
        Cell::VisitObjects(bot, playerSearcher, g_TopicPlayerRadius);

        std::vector<Player*> nearby;
        for (Player* other : found)
        {
            if (!other || other == bot || !other->IsInWorld())
                continue;
            if (!bot->IsWithinLOSInMap(other))
                continue;
            nearby.push_back(other);
        }

        // In a city, nobody's level is a sight (OllamaInCity): only the lines
        // that name none, and no such topic there when every line does.
        std::vector<std::string> nearbyLines;
        const bool inCity = OllamaInCity(bot);
        for (const std::string& line : g_EnvCommentNearbyPlayer)
            if (!inCity || (line.find("{player_level}") == std::string::npos &&
                            line.find("{level_gap}") == std::string::npos))
                nearbyLines.push_back(line);

        if (!nearby.empty() && !nearbyLines.empty())
        {
            Player* subject = nearby[PickIndex(nearby.size())];
            Add(out, TopicCategory::People, "nearby_player", nearbyLines,
                SafeFormat(PickOne(nearbyLines),
                           fmt::arg("player_name", subject->GetName()),
                           fmt::arg("player_class", ClassName(subject)),
                           fmt::arg("player_race", RaceName(subject)),
                           fmt::arg("player_level", subject->GetLevel()),
                           fmt::arg("player_state", DescribePlayerState(subject)),
                           fmt::arg("level_gap",
                                    int32(subject->GetLevel()) - int32(bot->GetLevel()))));
        }

        // Group members by name and what they are doing.
        if (Group* group = bot->GetGroup())
        {
            std::vector<Player*> members;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && member->IsInWorld())
                    members.push_back(member);
            }

            if (!members.empty() && !g_EnvCommentGroupMember.empty())
            {
                Player* m = members[PickIndex(members.size())];
                Add(out, TopicCategory::People, "group_member", g_EnvCommentGroupMember,
                    SafeFormat(PickOne(g_EnvCommentGroupMember),
                               fmt::arg("member_name", m->GetName()),
                               fmt::arg("member_class", ClassName(m)),
                               fmt::arg("member_state", DescribePlayerState(m)),
                               fmt::arg("member_level", m->GetLevel())));
            }

            // Somebody in the group needs something.
            for (Player* m : members)
            {
                std::string need;
                if (m->GetHealthPct() < 35.0f)
                    need = "badly hurt and needs healing";
                else if (m->getPowerType() == POWER_MANA && m->GetMaxPower(POWER_MANA) > 0 &&
                         (float(m->GetPower(POWER_MANA)) / float(m->GetMaxPower(POWER_MANA))) < 0.2f)
                    need = "nearly out of mana";
                else
                    continue;

                if (!g_EnvCommentGroupNeed.empty())
                {
                    Add(out, TopicCategory::Activity, "group_need", g_EnvCommentGroupNeed,
                        SafeFormat(PickOne(g_EnvCommentGroupNeed),
                                   fmt::arg("member_name", m->GetName()),
                                   fmt::arg("need", need)));
                }
                break;
            }
        }

        // A guildmate who is online right now.
        if (Guild* guild = bot->GetGuild())
        {
            // Real players only, from the tick's snapshot -- a guildmate who is
            // an actual person is the more interesting thing to mention anyway.
            std::vector<Player*> guildies;
            for (Player* other : world.realPlayers)
                if (other && other != bot && other->GetGuildId() == guild->GetId())
                    guildies.push_back(other);

            if (!guildies.empty() && !g_EnvCommentGuildMemberOnline.empty())
            {
                Player* g = guildies[PickIndex(guildies.size())];
                Add(out, TopicCategory::People, "guild_online", g_EnvCommentGuildMemberOnline,
                    SafeFormat(PickOne(g_EnvCommentGuildMemberOnline),
                               fmt::arg("member_name", g->GetName()),
                               fmt::arg("member_class", ClassName(g)),
                               fmt::arg("member_level", g->GetLevel())));
            }
        }

        // Something this bot actually witnessed.
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_state.find(bot->GetGUID().GetRawValue());
            if (it != g_state.end() && !it->second.memory.empty() &&
                !g_EnvCommentRecentEvent.empty())
            {
                const WitnessedEvent& ev =
                    it->second.memory[PickIndex(it->second.memory.size())];

                Add(out, TopicCategory::People, "recent_event", g_EnvCommentRecentEvent,
                    SafeFormat(PickOne(g_EnvCommentRecentEvent),
                               fmt::arg("event_text", ev.text)));
            }
        }
    }

    void GatherWorld(Player* bot, std::vector<Candidate>& out)
    {
        const float radius = g_SayDistance > 0.0f ? g_SayDistance : 30.0f;

        std::list<Creature*> creatures;
        NearbyCreatureCheck check(bot, radius);
        Acore::CreatureListSearcher<NearbyCreatureCheck> searcher(bot, creatures, check);
        Cell::VisitObjects(bot, searcher, radius);

        Creature* interesting = nullptr;
        Creature* namedNpc    = nullptr;
        int bestScore = -1;

        for (Creature* c : creatures)
        {
            if (!c || c->IsPet() || c->IsTotem() || c->GetName().empty())
                continue;

            // NPCs with a role are worth naming specifically.
            if (!namedNpc && (c->IsVendor() || c->IsQuestGiver() ||
                              c->IsInnkeeper() || c->IsTrainer()))
                namedNpc = c;

            // Rank the rest so we mention the elite rather than the squirrel.
            // The old code took whatever the searcher landed on first.
            if (c->GetCreatureType() == CREATURE_TYPE_CRITTER)
                continue;
            // Not the guards and townsfolk, whatever the bot's standing with
            // them, nor a training dummy (OllamaIsScenery): "an Orgrimmar
            // Grunt is close by" reads to a model as an enemy to watch out
            // for, and "about level 75" had one ask why the grunt was.
            if (c->IsFriendlyTo(bot) || OllamaIsScenery(c, bot))
                continue;

            int score = 0;
            if (c->isWorldBoss())                        score += 100;
            else if (c->isElite())                       score += 40;
            if (c->IsHostileTo(bot))                     score += 20;
            if (int32(c->GetLevel()) >= int32(bot->GetLevel()))
                score += 10 + std::min<int32>(20, int32(c->GetLevel()) - int32(bot->GetLevel()));
            if (c->isDead())                             score += 5;

            if (score > bestScore)
            {
                bestScore   = score;
                interesting = c;
            }
        }

        if (interesting && !g_EnvCommentCreature.empty())
        {
            const char* key = interesting->isDead() ? "corpse" : "creature";
            const std::vector<std::string>& list =
                (interesting->isDead() && !g_EnvCommentCorpse.empty())
                    ? g_EnvCommentCorpse : g_EnvCommentCreature;

            Add(out, TopicCategory::World, key, list,
                SafeFormat(PickOne(list),
                           fmt::arg("creature_name", interesting->GetName()),
                           fmt::arg("creature_level", interesting->GetLevel()),
                           fmt::arg("level_delta",
                                    int32(interesting->GetLevel()) - int32(bot->GetLevel()))));

            // A meaningfully stronger enemy nearby is worth its own remark.
            if (!interesting->isDead() && interesting->IsHostileTo(bot) &&
                int32(interesting->GetLevel()) >= int32(bot->GetLevel()) + 3 &&
                !g_EnvCommentDanger.empty())
            {
                Add(out, TopicCategory::Activity, "danger", g_EnvCommentDanger,
                    SafeFormat(PickOne(g_EnvCommentDanger),
                               fmt::arg("creature_name", interesting->GetName()),
                               fmt::arg("level_delta",
                                        int32(interesting->GetLevel()) - int32(bot->GetLevel()))));
            }
        }

        if (namedNpc)
        {
            std::string role = "someone";
            if (namedNpc->IsInnkeeper())       role = "innkeeper";
            else if (namedNpc->IsVendor())     role = "merchant";
            else if (namedNpc->IsQuestGiver()) role = "quest giver";
            else if (namedNpc->IsTrainer())    role = "trainer";

            if (!g_EnvCommentNamedNpc.empty())
                Add(out, TopicCategory::World, "named_npc", g_EnvCommentNamedNpc,
                    SafeFormat(PickOne(g_EnvCommentNamedNpc),
                               fmt::arg("npc_name", namedNpc->GetName()),
                               fmt::arg("npc_role", role)));

            // The pre-existing vendor and questgiver pools stay wired up.
            if (namedNpc->IsVendor() && !g_EnvCommentVendor.empty())
                Add(out, TopicCategory::World, "vendor", g_EnvCommentVendor,
                    SafeFormat(PickOne(g_EnvCommentVendor),
                               fmt::arg("vendor_name", namedNpc->GetName())));

            if (namedNpc->IsQuestGiver() && !g_EnvCommentQuestgiver.empty())
            {
                const auto bounds = sObjectMgr->GetCreatureQuestRelationBounds(namedNpc->GetEntry());
                const uint32 questCount =
                    static_cast<uint32>(std::distance(bounds.first, bounds.second));

                Add(out, TopicCategory::World, "questgiver", g_EnvCommentQuestgiver,
                    SafeFormat(PickOne(g_EnvCommentQuestgiver),
                               fmt::arg("questgiver_name", namedNpc->GetName()),
                               fmt::arg("quest_count", questCount)));
            }
        }

        // Gameobjects.
        {
            std::list<GameObject*> objects;
            Acore::GameObjectInRangeCheck goCheck(bot->GetPositionX(), bot->GetPositionY(),
                                                  bot->GetPositionZ(), radius);
            Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> goSearcher(bot, objects, goCheck);
            Cell::VisitObjects(bot, goSearcher, radius);

            std::vector<GameObject*> named;
            for (GameObject* go : objects)
                if (go && !go->GetName().empty())
                    named.push_back(go);

            if (!named.empty() && !g_EnvCommentGameObject.empty())
            {
                GameObject* go = named[PickIndex(named.size())];
                Add(out, TopicCategory::World, "gameobject", g_EnvCommentGameObject,
                    SafeFormat(PickOne(g_EnvCommentGameObject),
                               fmt::arg("object_name", go->GetName())));
            }
        }

        // Where they are.
        if (!g_EnvCommentZoneLandmark.empty())
        {
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            std::string areaName = "these parts";
            std::string zoneName = "this land";

            if (ai)
            {
                if (AreaTableEntry const* area = ai->GetCurrentArea())
                    areaName = PlayerbotAI::GetLocalizedAreaName(area);
                if (AreaTableEntry const* zone = ai->GetCurrentZone())
                    zoneName = PlayerbotAI::GetLocalizedAreaName(zone);
            }

            Add(out, TopicCategory::World, "landmark", g_EnvCommentZoneLandmark,
                SafeFormat(PickOne(g_EnvCommentZoneLandmark),
                           fmt::arg("area_name", areaName),
                           fmt::arg("zone_name", zoneName)));
        }

        if (!g_EnvCommentTimeOfDay.empty())
        {
            Add(out, TopicCategory::World, "time_of_day", g_EnvCommentTimeOfDay,
                SafeFormat(PickOne(g_EnvCommentTimeOfDay),
                           fmt::arg("time_of_day", TimeOfDayText()),
                           fmt::arg("weekday", Clock_WeekdayName()),
                           fmt::arg("part_of_day", Clock_PartOfDay())));
        }

        if (bot->GetMap() && bot->GetMap()->IsDungeon() && !g_EnvCommentDungeon.empty())
        {
            Add(out, TopicCategory::World, "dungeon", g_EnvCommentDungeon,
                SafeFormat(PickOne(g_EnvCommentDungeon),
                           fmt::arg("dungeon_name", bot->GetMap()->GetMapName())));
        }
    }

    // What it just did on the auction house: sold, bought, put up, or got
    // back unsold (mod-playerbots-plus).
    void GatherMarket(Player* bot, std::vector<Candidate>& out)
    {
        OllamaMarketNote deal;
        if (!OllamaMarket_Peek(bot, deal))
            return;

        static const char* const keys[] = { "market_listed", "market_sold", "market_unsold", "market_bought" };
        const std::vector<std::string>* pools[] = { &g_EnvCommentMarketListed, &g_EnvCommentMarketSold,
                                                    &g_EnvCommentMarketUnsold, &g_EnvCommentMarketBought };
        if (deal.type > 3 || pools[deal.type]->empty())
            return;

        const std::vector<std::string>& pool = *pools[deal.type];
        const std::string to   = (deal.type == 1 && !deal.other.empty()) ? " to " + deal.other : std::string();
        const std::string from = (deal.type == 3 && !deal.other.empty()) ? " from " + deal.other : std::string();
        Add(out, TopicCategory::Market, keys[deal.type], pool,
            SafeFormat(PickOne(pool),
                       fmt::arg("item", deal.item),
                       fmt::arg("price", deal.price),
                       fmt::arg("to", to),
                       fmt::arg("from", from)));
    }

    void GatherActivity(Player* bot, std::vector<Candidate>& out)
    {
        // What they are actually trying to do right now. Inside a dungeon,
        // only what is for this dungeon: nobody in the Stockade asks who
        // wants to group for a quest out in the world.
        std::vector<std::pair<std::string, std::string>> active;   // title, objective
        const bool instance = bot->GetMap() && bot->GetMap()->Instanceable();

        for (auto const& qs : bot->getQuestStatusMap())
        {
            if (qs.second.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(qs.first);
            if (!quest)
                continue;
            if (instance && (quest->GetZoneOrSort() <= 0 || uint32(quest->GetZoneOrSort()) != bot->GetZoneId()))
                continue;

            // What the quest log says it wants, not a guess from its title.
            std::string objective;
            for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (!quest->ObjectiveText[i].empty())
                {
                    objective = quest->ObjectiveText[i];
                    break;
                }
            }
            if (objective.empty())
                objective = Links_QuestObjective(quest, bot, 140);
            if (objective.empty())
                objective = "see it through";

            active.emplace_back("[" + quest->GetTitle() + "]", objective);
        }

        if (!active.empty())
        {
            const auto& q = active[PickIndex(active.size())];

            if (!g_EnvCommentQuestObjective.empty())
                Add(out, TopicCategory::Activity, "quest_objective", g_EnvCommentQuestObjective,
                    SafeFormat(PickOne(g_EnvCommentQuestObjective),
                               fmt::arg("quest_name", q.first),
                               fmt::arg("objective", q.second)));

            // Pre-existing pool, kept wired up.
            if (!g_EnvCommentUnfinishedQuest.empty())
                Add(out, TopicCategory::Activity, "unfinished_quest", g_EnvCommentUnfinishedQuest,
                    SafeFormat(PickOne(g_EnvCommentUnfinishedQuest),
                               fmt::arg("quest_name", q.first)));
        }

        // Where the bot could go questing next -- not from inside a dungeon.
        if (!g_EnvCommentQuestArea.empty() && !instance)
        {
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (ai)
            {
                if (AreaTableEntry const* zone = ai->GetCurrentZone())
                    Add(out, TopicCategory::Activity, "quest_area", g_EnvCommentQuestArea,
                        SafeFormat(PickOne(g_EnvCommentQuestArea),
                                   fmt::arg("quest_area",
                                            PlayerbotAI::GetLocalizedAreaName(zone))));
            }
        }
    }

    void GatherSelf(Player* bot, std::vector<Candidate>& out)
    {
        // Equipped item.
        {
            std::vector<Item*> equipped;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    equipped.push_back(item);

            if (!equipped.empty() && !g_EnvCommentEquippedItem.empty())
            {
                Item* item = equipped[PickIndex(equipped.size())];
                if (item->GetTemplate())
                    Add(out, TopicCategory::Self, "equipped_item", g_EnvCommentEquippedItem,
                        SafeFormat(PickOne(g_EnvCommentEquippedItem),
                                   fmt::arg("item_name", item->GetTemplate()->Name1)));
            }
        }

        // A spell they know. Kept, but now one of many rather than the loudest
        // thing in the prompt.
        if (!g_EnvCommentSpell.empty())
        {
            std::vector<std::pair<std::string, std::string>> spells;
            for (const auto& pair : bot->GetSpellMap())
            {
                const SpellInfo* info = sSpellMgr->GetSpellInfo(pair.first);
                if (!info || (info->Attributes & SPELL_ATTR0_PASSIVE))
                    continue;
                if (info->SpellFamilyName == SPELLFAMILY_GENERIC)
                    continue;
                const char* name = info->SpellName[0];
                if (!name || !*name)
                    continue;

                spells.emplace_back(name, info->ManaCost ? std::to_string(info->ManaCost)
                                                         : std::string("nothing"));
                if (spells.size() >= 64)
                    break;
            }

            if (!spells.empty())
            {
                const auto& s = spells[PickIndex(spells.size())];
                Add(out, TopicCategory::Self, "spell", g_EnvCommentSpell,
                    SafeFormat(PickOne(g_EnvCommentSpell),
                               fmt::arg("spell_name", s.first),
                               fmt::arg("spell_cost", s.second)));
            }
        }

        // Something in the bags, and the sales pitch for it. These pools exist
        // in every shipped config, so they stay wired up rather than becoming
        // dead settings.
        {
            std::vector<Item*> bagItems;
            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    if (item->GetTemplate())
                        bagItems.push_back(item);

            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            {
                Bag* container = bot->GetBagByPos(bag);
                if (!container)
                    continue;
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    if (Item* item = container->GetItemByPos(static_cast<uint8>(slot)))
                        if (item->GetTemplate())
                            bagItems.push_back(item);
            }

            if (!bagItems.empty())
            {
                Item* item = bagItems[PickIndex(bagItems.size())];
                const std::string name = item->GetTemplate()->Name1;

                if (!g_EnvCommentBagItem.empty())
                    Add(out, TopicCategory::Self, "bag_item", g_EnvCommentBagItem,
                        SafeFormat(PickOne(g_EnvCommentBagItem), fmt::arg("item_name", name)));

                if (!g_EnvCommentBagItemSell.empty() && item->GetCount() > 1)
                    Add(out, TopicCategory::Self, "bag_item_sell", g_EnvCommentBagItemSell,
                        SafeFormat(PickOne(g_EnvCommentBagItemSell),
                                   fmt::arg("item_name", name),
                                   fmt::arg("item_count", item->GetCount())));
            }
        }

        // Free bag space.
        if (!g_EnvCommentBagSlots.empty())
        {
            uint32 freeSlots = 0;
            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    ++freeSlots;

            Add(out, TopicCategory::Self, "bag_slots", g_EnvCommentBagSlots,
                SafeFormat(PickOne(g_EnvCommentBagSlots), fmt::arg("bag_slots", freeSlots)));
        }
    }

    // One of the things a conversation in town opens with (Premises_Pick:
    // level-tagged, filled from the pools, not again across the realm for
    // a while), as a passing remark.
    void GatherTalk(Player* bot, std::vector<Candidate>& out)
    {
        const std::string premise = Premises_Pick(bot);
        if (premise.empty())
            return;
        Add(out, TopicCategory::Talk, "game_talk", g_EnvCommentGameTalk,
            SafeFormat(PickOne(g_EnvCommentGameTalk), fmt::arg("premise", premise)));
    }

    void GatherGuild(Player* bot, std::vector<Candidate>& out)
    {
        Guild* guild = bot->GetGuild();
        if (!guild || !g_EnableGuildRandomAmbientChatter)
            return;

        auto addGuild = [&](const char* key, const std::vector<std::string>& list,
                            std::string text)
        {
            if (list.empty() || text.empty() || text == "[Format Error]")
                return;
            out.push_back({ TopicCategory::Guild, key, std::move(text), true });
        };

        // "No message set." is the game's own text for a guild whose message
        // of the day nobody ever wrote -- every bot guild's. Read out as if
        // someone had written it, it became "no message set is my spirit
        // animal"; it is talked about as what it is instead.
        std::string motd = guild->GetMOTD();
        while (!motd.empty() && (std::isspace(static_cast<unsigned char>(motd.back())) || motd.back() == '.'))
            motd.pop_back();
        std::transform(motd.begin(), motd.end(), motd.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (motd == "no message set")
            addGuild("guild_motd_unset", g_GuildEnvCommentGuildMOTDUnset,
                     SafeFormat(PickOne(g_GuildEnvCommentGuildMOTDUnset),
                                fmt::arg("guild_motd", guild->GetMOTD())));
        else if (!guild->GetMOTD().empty())
            addGuild("guild_motd", g_GuildEnvCommentGuildMOTD,
                     SafeFormat(PickOne(g_GuildEnvCommentGuildMOTD),
                                fmt::arg("guild_motd", guild->GetMOTD())));

        addGuild("guild_bank", g_GuildEnvCommentGuildBank,
                 SafeFormat(PickOne(g_GuildEnvCommentGuildBank),
                            fmt::arg("bank_gold", guild->GetTotalBankMoney() / 10000)));

        addGuild("guild_raid",      g_GuildEnvCommentGuildRaid,      PickOne(g_GuildEnvCommentGuildRaid));
        addGuild("guild_endgame",   g_GuildEnvCommentGuildEndgame,   PickOne(g_GuildEnvCommentGuildEndgame));
        addGuild("guild_strategy",  g_GuildEnvCommentGuildStrategy,  PickOne(g_GuildEnvCommentGuildStrategy));
        addGuild("guild_group",     g_GuildEnvCommentGuildGroup,     PickOne(g_GuildEnvCommentGuildGroup));
        addGuild("guild_pvp",       g_GuildEnvCommentGuildPvP,       PickOne(g_GuildEnvCommentGuildPvP));
        addGuild("guild_community", g_GuildEnvCommentGuildCommunity, PickOne(g_GuildEnvCommentGuildCommunity));
    }
}

// --------------------------------------------------------------------------

TopicPick Topics_Pick(Player* bot, const OllamaWorldSnapshot& world, bool local)
{
    TopicPick pick;
    if (!bot || !bot->IsInWorld())
        return pick;

    // Pick the category FIRST, by weight, then gather only that one. Gathering
    // all five and discarding four was the single most wasteful thing the
    // ambient path did.
    struct CatEntry { TopicCategory cat; uint32_t weight; };
    std::vector<CatEntry> cats = {
        { TopicCategory::People,   g_TopicWeightPeople   },
        { TopicCategory::World,    g_TopicWeightWorld    },
        { TopicCategory::Activity, g_TopicWeightActivity },
        { TopicCategory::Self,     g_TopicWeightSelf     },
    };

    // The game itself -- a raid, an old meme, a bit of lore -- the way people
    // in General talk about things nowhere near them. Not in a dungeon, where
    // the run is what there is to talk about.
    if (g_TopicWeightTalk > 0 && !g_EnvCommentGameTalk.empty() && !bot->GetMap()->Instanceable())
        cats.push_back({ TopicCategory::Talk, g_TopicWeightTalk });

    // Something it just did on the auction house, while it is fresh.
    OllamaMarketNote deal;
    if (g_TopicWeightMarket > 0 && OllamaMarket_Peek(bot, deal))
        cats.push_back({ TopicCategory::Market, g_TopicWeightMarket });

    // Guild topics only apply when there is a guild with someone in it.
    if (bot->GetGuild() && g_EnableGuildRandomAmbientChatter &&
        world.GuildHasRealPlayer(bot->GetGuildId()))
    {
        cats.push_back({ TopicCategory::Guild, g_TopicWeightGuild });
    }

    // Recently used topic keys, so a bot does not repeat itself.
    std::deque<std::string> recent;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_state.find(bot->GetGUID().GetRawValue());
        if (it != g_state.end())
            recent = it->second.recentKeys;
    }

    // Weighted order: draw categories without replacement, so if the first
    // choice yields nothing we fall through to the next rather than going
    // silent.
    std::vector<Candidate> candidates;

    while (!cats.empty())
    {
        uint64_t total = 0;
        for (const CatEntry& c : cats)
            total += c.weight;

        size_t chosenIdx = 0;
        if (total == 0)
        {
            chosenIdx = PickIndex(cats.size());
        }
        else
        {
            uint64_t roll = urand(0, static_cast<uint32>(std::min<uint64_t>(total - 1, 0xFFFFFFFFull)));
            for (size_t i = 0; i < cats.size(); ++i)
            {
                if (roll < cats[i].weight)
                {
                    chosenIdx = i;
                    break;
                }
                roll -= cats[i].weight;
                chosenIdx = i;
            }
        }

        const TopicCategory cat = cats[chosenIdx].cat;
        cats.erase(cats.begin() + chosenIdx);

        candidates.clear();
        switch (cat)
        {
            case TopicCategory::People:   GatherPeople(bot, world, candidates); break;
            case TopicCategory::World:    GatherWorld(bot, candidates);         break;
            case TopicCategory::Activity: GatherActivity(bot, candidates);      break;
            case TopicCategory::Self:     GatherSelf(bot, candidates);          break;
            case TopicCategory::Guild:    GatherGuild(bot, candidates);         break;
            case TopicCategory::Talk:     GatherTalk(bot, candidates);          break;
            case TopicCategory::Market:   GatherMarket(bot, candidates);        break;
            default: break;
        }

        // For a channel, nothing that is only for those here.
        if (!local)
            std::erase_if(candidates, [](Candidate const& c) { return c.here; });
        if (candidates.empty())
            continue;

        // Prefer something this bot has not just used.
        std::vector<const Candidate*> fresh;
        for (const Candidate& c : candidates)
            if (std::find(recent.begin(), recent.end(), c.key) == recent.end())
                fresh.push_back(&c);

        const Candidate* chosen = fresh.empty()
            ? &candidates[PickIndex(candidates.size())]
            : fresh[PickIndex(fresh.size())];

        pick.valid        = true;
        pick.category     = chosen->category;
        pick.key          = chosen->key;
        pick.text         = chosen->text;
        pick.isGuildTopic = chosen->isGuildTopic;
        pick.here         = chosen->here;
        return pick;
    }

    return pick;
}

void Topics_NoteUsed(ObjectGuid botGuid, const std::string& key)
{
    if (key.empty())
        return;

    if (key.rfind("market_", 0) == 0)
        OllamaMarket_NoteTalked(botGuid);   // that dealing has had its say

    std::lock_guard<std::mutex> lock(g_mutex);
    auto& state = g_state[botGuid.GetRawValue()];
    state.recentKeys.push_back(key);
    while (state.recentKeys.size() > g_TopicMemoryCount)
        state.recentKeys.pop_front();
}

void Topics_NoteWitnessedEvent(Player* witness, const std::string& text)
{
    if (!witness || text.empty() || g_TopicEventMemorySize == 0)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto& state = g_state[witness->GetGUID().GetRawValue()];
    state.memory.push_back({ text, Clock::now() });
    while (state.memory.size() > g_TopicEventMemorySize)
        state.memory.pop_front();
}

void Topics_BroadcastEventToNearby(Player* actor, const std::string& text, float radius)
{
    if (!actor || text.empty() || g_TopicEventMemorySize == 0 || !actor->IsInWorld())
        return;

    // The actor's own map: only someone there can have seen it. (Was every
    // player online, for every kill and loot in the world.)
    for (auto const& ref : actor->GetMap()->GetPlayers())
    {
        Player* witness = ref.GetSource();
        if (!witness || witness == actor || !witness->IsInWorld())
            continue;
        if (!actor->IsWithinDistInMap(witness, radius))
            continue;

        // Only bots keep a memory; real players have their own.
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(witness);
        if (!ai || !ai->IsBotAI())
            continue;

        Topics_NoteWitnessedEvent(witness, text);
    }
}

void Topics_ForgetBot(ObjectGuid botGuid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.erase(botGuid.GetRawValue());
}

void Topics_Update()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = Clock::now();

    for (auto it = g_state.begin(); it != g_state.end(); )
    {
        auto& memory = it->second.memory;
        while (!memory.empty())
        {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - memory.front().when).count();
            if (age <= int64_t(g_TopicEventMemorySeconds))
                break;
            memory.pop_front();
        }

        it = (memory.empty() && it->second.recentKeys.empty())
                 ? g_state.erase(it)
                 : std::next(it);
    }
}
