#include "mod-ollama-chat_buffs.h"
#include "mod-ollama-chat_answer.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_favor.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CellImpl.h"
#include "Containers.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <list>
#include <map>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    BuffStats g_stats;

    // ---- what can go on anybody -------------------------------------------------

    struct BuffSpell
    {
        uint16_t    kind;
        uint8_t     cls;
        uint32_t    firstRank;
        const char* name;   // as the game has it
    };

    // The single-target buffs a priest, mage, druid or paladin can cast on any
    // friendly player. Their group versions (Prayer of Fortitude, Arcane
    // Brilliance, Gift of the Wild, the greater blessings) only reach the
    // caster's own group or raid.
    const BuffSpell kSpells[] = {
        { TextRules::BUFF_FORT,   CLASS_PRIEST,  1243,  "Power Word: Fortitude" },
        { TextRules::BUFF_SPIRIT, CLASS_PRIEST,  14752, "Divine Spirit" },
        { TextRules::BUFF_SHADOW, CLASS_PRIEST,  976,   "Shadow Protection" },
        { TextRules::BUFF_INT,    CLASS_MAGE,    1459,  "Arcane Intellect" },
        { TextRules::BUFF_MARK,   CLASS_DRUID,   1126,  "Mark of the Wild" },
        { TextRules::BUFF_THORNS, CLASS_DRUID,   467,   "Thorns" },
        { TextRules::BUFF_KINGS,  CLASS_PALADIN, 20217, "Blessing of Kings" },
        { TextRules::BUFF_MIGHT,  CLASS_PALADIN, 19740, "Blessing of Might" },
        { TextRules::BUFF_WISDOM, CLASS_PALADIN, 19742, "Blessing of Wisdom" },
        { TextRules::BUFF_SANCT,  CLASS_PALADIN, 20911, "Blessing of Sanctuary" },
    };

    constexpr uint16_t kBlessings = TextRules::BUFF_KINGS | TextRules::BUFF_MIGHT | TextRules::BUFF_WISDOM |
                                    TextRules::BUFF_SANCT;

    const BuffSpell* SpellOfKind(uint16_t kind)
    {
        for (const BuffSpell& s : kSpells)
            if (s.kind == kind)
                return &s;
        return nullptr;
    }

    const BuffSpell* SpellOfInfo(SpellInfo const* info)
    {
        const uint32_t first = info ? info->GetFirstRankSpell()->Id : 0;
        for (const BuffSpell& s : kSpells)
            if (s.firstRank == first)
                return &s;
        return nullptr;
    }

    // Every buff a class can put on anyone.
    uint16_t KindsOf(uint8_t cls)
    {
        uint16_t kinds = TextRules::BUFF_NONE;
        for (const BuffSpell& s : kSpells)
            if (s.cls == cls)
                kinds |= s.kind;
        return kinds;
    }

    std::string ClassName(uint8_t cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "warrior";
            case CLASS_PALADIN:      return "paladin";
            case CLASS_HUNTER:       return "hunter";
            case CLASS_ROGUE:        return "rogue";
            case CLASS_PRIEST:       return "priest";
            case CLASS_DEATH_KNIGHT: return "death knight";
            case CLASS_SHAMAN:       return "shaman";
            case CLASS_MAGE:         return "mage";
            case CLASS_WARLOCK:      return "warlock";
            case CLASS_DRUID:        return "druid";
            default:                 return "player";
        }
    }

    // "a priest" (every class name starts with a consonant).
    std::string AClass(uint8_t cls)
    {
        return "a " + ClassName(cls);
    }

    // What they asked for, for the hint: "a buff", "Power Word: Fortitude",
    // "a blessing" ("pally buff pls").
    std::string RequestWords(uint16_t kinds)
    {
        if (kinds == TextRules::BUFF_NONE)
            return "a buff";
        std::vector<std::string> names;
        for (const BuffSpell& s : kSpells)
            if (kinds & s.kind)
                names.push_back(s.name);
        if (names.size() > 1 && (kinds & ~kBlessings) == 0)
            return "a blessing";
        if (names.size() > 2)
            return "a buff";
        return CrowdText::JoinNames(names);
    }

    // The best rank of `s` the bot has learned, or none.
    SpellInfo const* BestKnown(Player* bot, const BuffSpell& s)
    {
        SpellInfo const* first = sSpellMgr->GetSpellInfo(s.firstRank);
        if (!first)
            return nullptr;
        for (SpellInfo const* rank = first->GetLastRankSpell(); rank; rank = rank->GetPrevRankSpell())
            if (bot->HasSpell(rank->Id))
                return rank;
        return nullptr;
    }

    // Has `target` got this buff, or one it does not stack with -- its group
    // version, a scroll, drums? A lower rank of the very same spell is no
    // reason not to: what lands now is better. (Blessings from one paladin
    // replace each other, so another paladin's are no reason either.)
    bool HasCover(Player* target, SpellInfo const* landing)
    {
        SpellInfo const* first = landing->GetFirstRankSpell();
        for (auto const& [id, app] : target->GetAppliedAuras())
        {
            (void)id;
            Aura const* aura = app ? app->GetBase() : nullptr;
            SpellInfo const* have = aura ? aura->GetSpellInfo() : nullptr;
            if (!have)
                continue;
            if (have->GetFirstRankSpell() == first)
            {
                if (have->GetRank() >= landing->GetRank())
                    return true;
                continue;
            }
            switch (sSpellMgr->CheckSpellGroupStackRules(landing, have))
            {
                case SPELL_GROUP_STACK_RULE_EXCLUSIVE:
                case SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST:
                case SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

    bool UsesMana(Player* p)
    {
        return p->GetMaxPower(POWER_MANA) > 0;
    }

    // Who a paladin gives Might rather than Wisdom: everyone who hits things.
    bool HitsThings(Player* p)
    {
        switch (p->getClass())
        {
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_DEATH_KNIGHT:
            case CLASS_HUNTER:
                return true;
            default:
                return false;
        }
    }

    // What a bot of this class puts on someone who asks for "a buff", in the
    // order it tries them: the priest's Fortitude, the druid's Mark, the mage's
    // Intellect for anyone with mana, the paladin's Kings -- or Might or
    // Wisdom, before it has Kings or when they have it already.
    std::vector<uint16_t> UsualKinds(uint8_t cls, Player* target)
    {
        switch (cls)
        {
            case CLASS_PRIEST:
                return { TextRules::BUFF_FORT };
            case CLASS_DRUID:
                return { TextRules::BUFF_MARK };
            case CLASS_MAGE:
                if (UsesMana(target))
                    return { TextRules::BUFF_INT };
                return {};
            case CLASS_PALADIN:
                return { TextRules::BUFF_KINGS, HitsThings(target) ? TextRules::BUFF_MIGHT : TextRules::BUFF_WISDOM };
            default:
                return {};
        }
    }

    // What a bot would put on someone.
    struct Choice
    {
        std::vector<SpellInfo const*> casts;   // the ranks it knows
        uint16_t    kinds = TextRules::BUFF_NONE;
        std::string names;                     // "Power Word: Fortitude", "A and B"
        std::string reason;                    // why it can't, when it can't
        bool        instead = false;           // its own buff, not the one asked for
        std::string askedClass;                // whose the one asked for is: "a priest"
    };

    // Why `s` can't go on them, or nothing. `known` is the rank it would cast.
    std::string WhyNot(Player* bot, Player* target, const BuffSpell& s, SpellInfo const*& known)
    {
        known = BestKnown(bot, s);
        if (!known)
            return fmt::format("you have not learned {} yet", s.name);
        // The core puts a rank fit for them on anyone within ten levels of
        // the spell's first rank, and refuses anyone lower.
        if (uint32_t(target->GetLevel()) + 10 < known->GetFirstRankSpell()->SpellLevel)
            return fmt::format("they are too low a level for {}", s.name);
        if (HasCover(target, known->GetAuraRankForLevel(target->GetLevel())))
            return fmt::format("they have {} already", s.name);
        return {};
    }

    // What `bot` would put on `target` for an ask of `asked` (none: any buff),
    // leaving out what is on its way to them already (`coming`). `spokenTo`:
    // asked in particular, so it offers its own buff when the one asked for
    // is another class's.
    Choice Choose(Player* bot, Player* target, uint16_t asked, uint16_t coming, bool spokenTo)
    {
        Choice c;
        const uint8_t cls = bot->getClass();

        std::vector<std::vector<uint16_t>> wants;   // each: what it tries, in turn, until one can go on
        if (asked == TextRules::BUFF_NONE)
        {
            std::vector<uint16_t> usual = UsualKinds(cls, target);
            if (!usual.empty())
                wants.push_back(std::move(usual));
            else if (cls == CLASS_MAGE)
                c.reason = fmt::format("{} has no mana, so Arcane Intellect is no use to them",
                                       AClass(target->getClass()));
        }
        else
        {
            for (const BuffSpell& s : kSpells)
                if ((asked & s.kind) && s.cls == cls)
                    wants.push_back({ s.kind });
            if (wants.empty())
            {
                for (const BuffSpell& s : kSpells)
                    if (asked & s.kind)
                    {
                        c.askedClass = AClass(s.cls);
                        break;
                    }
                std::vector<uint16_t> usual = UsualKinds(cls, target);
                if (spokenTo && !usual.empty())
                {
                    wants.push_back(std::move(usual));
                    c.instead = true;
                }
            }
        }

        if (wants.empty())
        {
            if (c.reason.empty())
            {
                auto note = g_BuffClassNotes.find(ClassName(cls));
                if (KindsOf(cls) == TextRules::BUFF_NONE && note != g_BuffClassNotes.end())
                    c.reason = fmt::format("you are {}, and {}", AClass(cls), note->second);
                else if (!c.askedClass.empty())
                    c.reason = fmt::format("that is {}'s to give, and you are {}", c.askedClass, AClass(cls));
                else
                    c.reason = fmt::format("you are {}, with no buffs to give anyone", AClass(cls));
            }
            return c;
        }

        std::vector<std::string> names;
        for (const std::vector<uint16_t>& tries : wants)
        {
            std::string why;
            for (uint16_t kind : tries)
            {
                const BuffSpell* s = SpellOfKind(kind);
                if (!s)
                    continue;
                if (coming & kind)
                {
                    if (why.empty())
                        why = fmt::format("somebody else is putting {} on them already", s->name);
                    continue;
                }
                SpellInfo const* known = nullptr;
                const std::string no = WhyNot(bot, target, *s, known);
                if (no.empty())
                {
                    c.casts.push_back(known);
                    c.kinds |= kind;
                    names.push_back(s->name);
                    why.clear();
                    break;
                }
                if (why.empty())
                    why = no;
            }
            if (!why.empty() && c.reason.empty())
                c.reason = why;
            if (c.casts.size() >= 2)
                break;
        }

        // The mana for all of it.
        if (!c.casts.empty())
        {
            uint32_t cost = 0;
            for (SpellInfo const* s : c.casts)
                cost += uint32_t(std::max<int32>(0, s->CalcPowerCost(bot, s->GetSchoolMask())));
            if (bot->GetPower(POWER_MANA) < cost)
            {
                c.casts.clear();
                c.kinds = TextRules::BUFF_NONE;
                names.clear();
                c.reason = "you are out of mana";
            }
        }
        c.names = CrowdText::JoinNames(names);
        return c;
    }

    // Buffing someone flagged for PvP flags whoever does it.
    bool PvpStops(Player* bot, Player* target)
    {
        return g_BuffSkipPvp && (target->IsPvP() || target->IsFFAPvP()) && !bot->IsPvP();
    }

    // Close enough to cast it, and in sight. A whisper can come from anywhere.
    std::string RangeProblem(Player* bot, Player* target, const Choice& c)
    {
        if (bot->GetMapId() != target->GetMapId() || !bot->InSamePhase(target))
            return "you are nowhere near them";
        for (SpellInfo const* s : c.casts)
            if (!bot->IsWithinDistInMap(target, s->GetMaxRange(true, bot)))
                return "you are too far away from them";
        if (!bot->IsWithinLOSInMap(target))
            return "you can't see them from where you are";
        return {};
    }

    // How a bot takes being asked is its chat style's (mod-ollama-chat_favor).
    using Outcome = FavorOutcome;

    // What a bot made of someone's last ask, so asking again straight away
    // does not get it a new roll.
    struct Decision
    {
        Outcome           outcome = Outcome::Ignore;
        Clock::time_point when{};
    };
    std::map<std::pair<uint64_t, uint64_t>, Decision> g_decided;   // (bot, them)

    // ---- the casts --------------------------------------------------------------

    struct Cast
    {
        uint64_t          botGuid    = 0;
        uint64_t          targetGuid = 0;
        uint32_t          spellId    = 0;
        uint16_t          kind       = TextRules::BUFF_NONE;
        Clock::time_point due{};
        Clock::time_point until{};   // a cast that keeps hitting the global cooldown gives up then
    };
    std::vector<Cast> g_casts;

    // What is on its way to someone already.
    uint16_t ComingTo(uint64_t targetGuid)
    {
        uint16_t kinds = TextRules::BUFF_NONE;
        for (const Cast& c : g_casts)
            if (c.targetGuid == targetGuid)
                kinds |= c.kind;
        return kinds;
    }

    // A moment to target them and press the button, then the next a global
    // cooldown later.
    void Schedule(Player* bot, Player* target, const Choice& choice)
    {
        const auto first = Clock::now() + std::chrono::milliseconds(urand(g_BuffDelayMinMs, g_BuffDelayMaxMs));
        for (size_t i = 0; i < choice.casts.size(); ++i)
        {
            const BuffSpell* s = SpellOfInfo(choice.casts[i]);
            Cast cast;
            cast.botGuid    = bot->GetGUID().GetRawValue();
            cast.targetGuid = target->GetGUID().GetRawValue();
            cast.spellId    = choice.casts[i]->Id;
            cast.kind       = s ? s->kind : TextRules::BUFF_NONE;
            cast.due        = first + std::chrono::milliseconds(1600 * i);
            cast.until      = cast.due + std::chrono::seconds(8);
            g_casts.push_back(cast);
        }
    }

    // Who gave someone what lately, for their "ty".
    struct Given
    {
        uint64_t          botGuid    = 0;
        uint64_t          targetGuid = 0;
        std::string       names;   // "Power Word: Fortitude", "A and B"
        Clock::time_point when{};
    };
    std::vector<Given> g_given;

    void NoteGiven(Player* bot, Player* target, const std::string& name)
    {
        const uint64_t b = bot->GetGUID().GetRawValue();
        const uint64_t t = target->GetGUID().GetRawValue();
        for (Given& g : g_given)
        {
            if (g.botGuid != b || g.targetGuid != t)
                continue;
            if (g.names.find(name) == std::string::npos)
                g.names += " and " + name;
            g.when = Clock::now();
            return;
        }
        g_given.push_back({ b, t, name, Clock::now() });
    }

    // As anyone would: off the mount (never in the air), out of a form the
    // spell can't be cast in, turned to face them. Standing up from a bench
    // is the core's own doing.
    SpellCastResult CastOn(Player* bot, Player* target, SpellInfo const* info)
    {
        if (bot->IsMounted())
        {
            if (bot->IsFlying() || bot->GetPositionZ() - bot->GetFloorZ() > 3.0f)
                return SPELL_FAILED_NOT_MOUNTED;
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }
        if (bot->GetShapeshiftForm() != FORM_NONE && info->CheckShapeshift(bot->GetShapeshiftForm()) != SPELL_CAST_OK)
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        if (!bot->HasInArc(static_cast<float>(M_PI / 2), target))
            bot->SetFacingToObject(target);
        return bot->CastSpell(target, info->Id, false);
    }

    // ---- who hears it -----------------------------------------------------------

    struct Near
    {
        std::vector<Player*> bots;     // who could answer it
        std::vector<Player*> people;   // real players near, whose names might be said
    };

    // The bot whispered, or everyone near enough to hear it in /say. Bots in
    // their group or that they are master of are playerbots' to buff, and
    // the other side's do not understand it.
    Near Gather(Player* player, ChatChannelSourceLocal source, Player* receiver)
    {
        Near n;
        auto mayAnswer = [player](Player* b)
        {
            return b->GetTeamId() == player->GetTeamId() && !b->InBattleground() && !b->InArena() &&
                   Favor_IsStranger(b, player);
        };

        if (source == SRC_WHISPER_LOCAL)
        {
            if (receiver && receiver->IsInWorld() && mayAnswer(receiver))
                n.bots.push_back(receiver);
            return n;
        }

        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(player, g_SayDistance, false, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(player, found, check);
        Cell::VisitObjects(player, searcher, g_SayDistance);
        for (Player* p : found)
        {
            if (!p || p == player || !p->IsInWorld())
                continue;
            if (!OllamaIsBotPlayer(p))
                n.people.push_back(p);
            else if (mayAnswer(p))
                n.bots.push_back(p);
        }
        return n;
    }

    // Whom the line names first, of those near. A real player named first
    // (`person`) is theirs to answer.
    Player* FirstNamed(const std::string& msg, const Near& n, bool& person)
    {
        size_t best = std::string::npos;
        Player* who = nullptr;
        person = false;
        auto consider = [&](Player* p, bool real)
        {
            const size_t pos = OllamaFindBotNameMention(msg, p->GetName());
            if (pos == std::string::npos || pos >= best)
                return;
            best   = pos;
            who    = p;
            person = real;
        };
        for (Player* p : n.bots)
            consider(p, false);
        for (Player* p : n.people)
            consider(p, true);
        return person ? nullptr : who;
    }

    // ---- what it says -------------------------------------------------------------

    // " They took you for a priest, but you are a mage." -- or nothing.
    std::string TookFor(const std::string& askedClass, Player* bot)
    {
        if (askedClass.empty() || askedClass == ClassName(bot->getClass()))
            return {};
        return fmt::format(" They took you for a {}, but you are {}.", askedClass, AClass(bot->getClass()));
    }

    std::string Hint(const std::string& templ, Player* player, Player* bot, const std::string& request,
                     const std::string& buff, const std::string& reason, const std::string& tookFor,
                     const std::string& askedClass)
    {
        if (templ.empty())
            return {};
        return SafeFormat(templ,
                          fmt::arg("player_name", player->GetName()),
                          fmt::arg("request", request),
                          fmt::arg("buff", buff),
                          fmt::arg("reason", reason),
                          fmt::arg("took_for", tookFor),
                          fmt::arg("class", AClass(bot->getClass())),
                          fmt::arg("asked_class", askedClass));
    }

    void Answer(Player* player, Player* bot, const std::string& msg, ChatChannelSourceLocal source,
                const std::string& scopeKey, uint64_t seq, std::string hint)
    {
        OllamaBotAnswer(player, bot, msg, source, scopeKey, seq, std::move(hint), CrowdLineKind::Favor);
    }

    void Record(Player* player, const std::string& msg, ChatChannelSourceLocal source, const std::string& scopeKey,
                uint64_t& seq)
    {
        if (seq)
            return;
        Governor_NoteHumanMessage(scopeKey);
        seq = Context_RecordLine(player, source, std::string(), msg, ContextLineKind::Human);
    }

    // ---- the "ty" ----------------------------------------------------------------

    // A thank-you soon after a buff: the one who gave it may answer, nobody
    // else does, and that is the end of it.
    bool Thanks(Player* player, const std::string& msg, const std::string& clean, ChatChannelSourceLocal source,
                Player* receiver, const Near& n, const std::string& scopeKey, uint64_t& seq)
    {
        const uint64_t who = player->GetGUID().GetRawValue();
        const auto now = Clock::now();
        const auto window = std::chrono::seconds(g_BuffThanksSeconds);

        std::vector<Given> mine;
        for (const Given& g : g_given)
            if (g.targetGuid == who && now - g.when <= window)
                mine.push_back(g);
        if (mine.empty())
            return false;

        std::vector<std::string> names;
        for (Player* p : n.bots)
            names.push_back(p->GetName());
        for (const Given& g : mine)
            if (Player* b = ObjectAccessor::FindConnectedPlayer(ObjectGuid(g.botGuid)))
                names.push_back(b->GetName());
        if (!TextRules::IsThanksLine(clean, g_BuffRules.thanks, g_BuffRules.negations, names, g_BuffThanksMaxWords))
            return false;

        // Thanking a real player near: theirs.
        bool person = false;
        FirstNamed(msg, n, person);
        if (person)
            return false;

        // Which giver: the one whispered or named, else the last one.
        const Given* to = nullptr;
        if (source == SRC_WHISPER_LOCAL)
        {
            for (const Given& g : mine)
                if (receiver && g.botGuid == receiver->GetGUID().GetRawValue())
                    to = &g;
            if (!to)
                return false;   // whispered to someone who gave them nothing
        }
        else
        {
            size_t best = std::string::npos;
            for (const Given& g : mine)
                if (Player* b = ObjectAccessor::FindConnectedPlayer(ObjectGuid(g.botGuid)))
                {
                    const size_t pos = OllamaFindBotNameMention(msg, b->GetName());
                    if (pos != std::string::npos && pos < best)
                    {
                        best = pos;
                        to   = &g;
                    }
                }
            if (!to)
                for (const Given& g : mine)
                    if (!to || g.when > to->when)
                        to = &g;
        }

        Record(player, msg, source, scopeKey, seq);

        Player* bot = to ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(to->botGuid)) : nullptr;
        const bool hears = bot && bot->IsInWorld() &&
                           (source == SRC_WHISPER_LOCAL ||
                            (bot->GetMapId() == player->GetMapId() && bot->IsWithinDistInMap(player, g_SayDistance)));
        if (hears && Favor_CanAnswer(bot, true) && Favor_Welcomes(bot))
        {
            ++g_stats.thanked;
            Answer(player, bot, msg, source, scopeKey, seq,
                   Hint(g_BuffHintThanks, player, bot, "a buff", to->names, {}, {}, {}));
        }

        g_given.erase(std::remove_if(g_given.begin(), g_given.end(),
                                     [who](const Given& g) { return g.targetGuid == who; }),
                      g_given.end());
        return true;
    }

    // ---- answering an ask ----------------------------------------------------------

    struct Ask
    {
        Player*                player = nullptr;
        const std::string*     msg    = nullptr;
        ChatChannelSourceLocal source = SRC_SAY_LOCAL;
        const std::string*     scopeKey = nullptr;
        uint64_t               seq    = 0;
        uint16_t               wanted = TextRules::BUFF_NONE;   // none: any buff
        std::string            request;                       // "a buff", "Blessing of Kings"
        std::string            askedClass;                    // "priest"
    };

    void Remember(Player* bot, Player* player, Outcome o)
    {
        g_decided[{ bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue() }] = { o, Clock::now() };
    }

    // Asked in particular: it answers, its own way.
    void AnswerOne(const Ask& a, Player* bot)
    {
        Player* player = a.player;
        if (!Favor_CanAnswer(bot, true))
        {
            ++g_stats.ignored;
            return;
        }
        const Choice c = Choose(bot, player, a.wanted, ComingTo(player->GetGUID().GetRawValue()), true);
        std::string why = c.casts.empty() ? c.reason : std::string();
        if (why.empty() && PvpStops(bot, player))
            why = "they are flagged for PvP, and buffing them would flag you too";
        if (why.empty())
            why = RangeProblem(bot, player, c);
        const std::string tookFor = TookFor(a.askedClass, bot);

        if (!why.empty())
        {
            if (Favor_Explains(bot))
            {
                Favor_ComeBack(bot);
                ++g_stats.cant;
                Answer(player, bot, *a.msg, a.source, *a.scopeKey, a.seq,
                       Hint(g_BuffHintCant, player, bot, a.request, {}, why, tookFor, {}));
            }
            else
                ++g_stats.ignored;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked {} for {}: can't ({}).", player->GetName(),
                         bot->GetName(), a.request, why);
            return;
        }

        const Outcome o = Favor_Roll(bot, true);
        if (o != Outcome::Ignore)
            Favor_ComeBack(bot);
        switch (o)
        {
            case Outcome::Give:
                Answer(player, bot, *a.msg, a.source, *a.scopeKey, a.seq,
                       c.instead ? Hint(g_BuffHintInstead, player, bot, a.request, c.names, {}, {}, c.askedClass)
                                 : Hint(g_BuffHintGive, player, bot, a.request, c.names, {}, tookFor, {}));
                Schedule(bot, player, c);
                break;
            case Outcome::Quiet:
                ++g_stats.quiet;
                Schedule(bot, player, c);
                break;
            case Outcome::No:
                ++g_stats.refused;
                Answer(player, bot, *a.msg, a.source, *a.scopeKey, a.seq,
                       Hint(g_BuffHintNo, player, bot, a.request, c.names, {}, tookFor, {}));
                break;
            case Outcome::Ignore:
                ++g_stats.ignored;
                break;
        }
        Remember(bot, player, o);
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked {} for {}: {}.", player->GetName(), bot->GetName(),
                     a.request,
                     o == Outcome::Give  ? "buffs them with " + c.names + ", and says so"
                   : o == Outcome::Quiet ? "buffs them with " + c.names + ", without a word"
                   : o == Outcome::No    ? std::string("says no")
                                         : std::string("lets it go by"));
    }

    // Said to everyone near: a bot or two who can buff them might, a rude one
    // might say no, the rest let it go by.
    void AnswerOpen(const Ask& a, const std::vector<Player*>& bots)
    {
        Player* player = a.player;
        const uint64_t them = player->GetGUID().GetRawValue();
        const auto now = Clock::now();
        uint16_t coming = ComingTo(them);
        uint32_t givers = 0, voices = 0, noes = 0;

        std::vector<Player*> order = bots;
        Acore::Containers::RandomShuffle(order);
        for (Player* bot : order)
        {
            if (!Favor_CanAnswer(bot))
                continue;
            // "buff pls" from a priest is not asking the other priests.
            if (a.wanted == TextRules::BUFF_NONE && bot->getClass() == player->getClass() &&
                bot->getClass() != CLASS_PALADIN)
                continue;
            // Let the last ask go by: lets this one go by too.
            auto d = g_decided.find({ bot->GetGUID().GetRawValue(), them });
            if (d != g_decided.end() && now - d->second.when < std::chrono::seconds(g_BuffRememberSeconds) &&
                (d->second.outcome == Outcome::Ignore || d->second.outcome == Outcome::No))
                continue;

            const Choice c = Choose(bot, player, a.wanted, coming, false);
            if (c.casts.empty() || PvpStops(bot, player) || !RangeProblem(bot, player, c).empty())
                continue;

            Outcome o = Favor_Roll(bot, false);
            const Outcome rolled = o;
            if ((o == Outcome::Give || o == Outcome::Quiet) && givers >= g_BuffMaxGivers)
                o = Outcome::Ignore;   // enough people on it already
            if (o == Outcome::Give && voices >= g_BuffMaxVoices)
                o = Outcome::Quiet;
            if (o == Outcome::No && (noes >= 1 || voices >= g_BuffMaxVoices))
                o = Outcome::Ignore;

            switch (o)
            {
                case Outcome::Give:
                    Answer(player, bot, *a.msg, a.source, *a.scopeKey, a.seq,
                           Hint(g_BuffHintGive, player, bot, a.request, c.names, {}, TookFor(a.askedClass, bot), {}));
                    Schedule(bot, player, c);
                    coming |= c.kinds;
                    ++givers;
                    ++voices;
                    break;
                case Outcome::Quiet:
                    ++g_stats.quiet;
                    Schedule(bot, player, c);
                    coming |= c.kinds;
                    ++givers;
                    break;
                case Outcome::No:
                    ++g_stats.refused;
                    Answer(player, bot, *a.msg, a.source, *a.scopeKey, a.seq,
                           Hint(g_BuffHintNo, player, bot, a.request, c.names, {}, TookFor(a.askedClass, bot), {}));
                    ++noes;
                    ++voices;
                    break;
                case Outcome::Ignore:
                    break;
            }
            // Only what it made of it itself: one left out because others had
            // it covered may yet answer the next ask.
            if (o == rolled)
                Remember(bot, player, o);
            if (g_DebugEnabled && o != Outcome::Ignore)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} asked anyone for {}: {} {}.", player->GetName(),
                         a.request, bot->GetName(),
                         o == Outcome::Give  ? "buffs them with " + c.names + ", and says so"
                       : o == Outcome::Quiet ? "buffs them with " + c.names + ", without a word"
                                             : std::string("says no"));
        }
        if (!givers && !voices)
            ++g_stats.ignored;
    }
}

bool Buff_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                          Player* receiver, const std::string& scopeKey, uint64_t& seq)
{
    if (!g_BuffEnable || !player || msg.empty() || OllamaIsBotPlayer(player) || !player->IsInWorld())
        return false;
    if (source != SRC_SAY_LOCAL && source != SRC_WHISPER_LOCAL)
        return false;
    if (source == SRC_WHISPER_LOCAL && (!receiver || receiver == player || !OllamaIsBotPlayer(receiver)))
        return false;
    if (player->InBattleground() || player->InArena())
        return false;

    const std::string clean = Context_CleanText(msg);

    const Near n = Gather(player, source, receiver);
    if (n.bots.empty())
        return false;

    // "ty" after a buff.
    if (Thanks(player, msg, clean, source, receiver, n, scopeKey, seq))
        return true;

    // Named first: a real player's to answer; a bot's, that bot's.
    Player* named = nullptr;
    if (source == SRC_WHISPER_LOCAL)
        named = n.bots.front();
    else
    {
        bool person = false;
        named = FirstNamed(msg, n, person);
        if (person)
            return false;
    }

    // A ghost has nothing anyone could put a buff on.
    if (!player->IsAlive())
        return false;

    std::vector<std::string> names;
    for (Player* b : n.bots)
        names.push_back(b->GetName());
    const TextRules::BuffAsk ask = TextRules::MatchBuffAsk(clean, g_BuffRules, named != nullptr, names);
    if (!ask.ask)
        return false;

    // A priest saying "fort?" is offering it, not asking for it.
    const uint16_t own = KindsOf(player->getClass());
    if (ask.kinds != TextRules::BUFF_NONE && (ask.kinds & ~own) == 0)
        return false;

    // Said to one bot: the one named or whispered, the one they have
    // targeted, or the one they were talking to -- unless it was said to
    // everyone ("anyone got fort?").
    const bool open = CrowdText::ContainsPhrase(CrowdText::Words(clean), g_CrowdOpenCallPhrases);
    Player* to = named;
    if (!to && !open)
        if (Unit* selected = player->GetSelectedUnit())
            if (Player* p = selected->ToPlayer())
                if (std::find(n.bots.begin(), n.bots.end(), p) != n.bots.end())
                    to = p;

    // Answered from here on, if only by everyone letting it go by.
    Record(player, msg, source, scopeKey, seq);

    if (!to && !open)
    {
        const ContextAddressee likely = Context_LikelyAddressee(player, source, std::string(), seq);
        if (likely.guid && !likely.isBot)
            return false;   // they were talking to a real player
        for (Player* b : n.bots)
            if (likely.isBot && b->GetGUID().GetRawValue() == likely.guid)
                to = b;
    }

    ++g_stats.asked;
    Ask a;
    a.player     = player;
    a.msg        = &msg;
    a.source     = source;
    a.scopeKey   = &scopeKey;
    a.seq        = seq;
    a.wanted     = ask.kinds & ~own;
    a.request    = RequestWords(ask.kinds);
    a.askedClass = ask.askedClass;
    if (to)
        AnswerOne(a, to);
    else
        AnswerOpen(a, n.bots);
    return true;
}

void Buff_Update()
{
    const auto now = Clock::now();

    for (auto it = g_casts.begin(); it != g_casts.end();)
    {
        const Cast cast = *it;
        if (now < cast.due)
        {
            ++it;
            continue;
        }

        Player* bot    = ObjectAccessor::FindConnectedPlayer(ObjectGuid(cast.botGuid));
        Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(cast.targetGuid));
        SpellInfo const* info = sSpellMgr->GetSpellInfo(cast.spellId);

        bool keep = false;
        if (bot && target && info && bot->IsInWorld() && target->IsInWorld() && bot->IsAlive() &&
            target->IsAlive() && !bot->IsInCombat() && bot->GetMapId() == target->GetMapId() &&
            bot->InSamePhase(target))
        {
            if (HasCover(target, info->GetAuraRankForLevel(target->GetLevel())))
            {
                // Somebody got there first: nothing to do.
            }
            else if (!bot->IsWithinDistInMap(target, info->GetMaxRange(true, bot)) || !bot->IsWithinLOSInMap(target))
            {
                ++g_stats.failed;   // they walked off
            }
            else
            {
                const SpellCastResult result = CastOn(bot, target, info);
                if (result == SPELL_CAST_OK)
                {
                    ++g_stats.given;
                    if (const BuffSpell* s = SpellOfInfo(info))
                        NoteGiven(bot, target, s->name);
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] {} buffs {} with {}.", bot->GetName(),
                                 target->GetName(), info->SpellName[0] ? info->SpellName[0] : "a buff");
                }
                else if ((result == SPELL_FAILED_NOT_READY || result == SPELL_FAILED_SPELL_IN_PROGRESS) &&
                         now < cast.until)
                {
                    keep = true;   // its global cooldown: next tick
                }
                else
                {
                    ++g_stats.failed;
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] {} could not buff {} after all (spell {}, "
                                 "result {}).", bot->GetName(), target->GetName(), cast.spellId, uint32(result));
                }
            }
        }
        else
            ++g_stats.failed;

        it = keep ? std::next(it) : g_casts.erase(it);
    }

    const auto thanks = std::chrono::seconds(g_BuffThanksSeconds);
    g_given.erase(std::remove_if(g_given.begin(), g_given.end(),
                                 [&](const Given& g) { return now - g.when > thanks; }),
                  g_given.end());
    const auto remember = std::chrono::seconds(g_BuffRememberSeconds);
    for (auto it = g_decided.begin(); it != g_decided.end();)
        it = (now - it->second.when > remember) ? g_decided.erase(it) : std::next(it);
}

BuffStats Buff_GetStats()
{
    BuffStats out = g_stats;
    out.pending = uint32_t(g_casts.size());
    return out;
}
