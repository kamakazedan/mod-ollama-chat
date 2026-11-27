#include "mod-ollama-chat_professions.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "DBCStores.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    bool                                         g_enable = true;
    std::unordered_map<std::string, std::string> g_leanings;       // personality -> leaning, as plus names it
    std::unordered_map<std::string, std::string> g_reasons;        // "profit.own" -> why
    std::vector<std::vector<std::string>>        g_words;          // talk of professions
    std::string                                  g_replyText;
    std::string                                  g_noneText;
    std::string                                  g_whyText;
    std::unordered_set<std::string>              g_goldLeanings;   // lower case
    uint32_t                                     g_goldChance = 20;
    std::string                                  g_goldText;
    ProfessionsStats                             g_stats;

    // The reasons the module ships, by leaning and how its pair came of it
    // (mod-playerbots-plus's LeaningMatch): "own" a pair the leaning names,
    // "armour" the one that makes the armour it wears, "race" its race's
    // knack, "rare" what the fewest of its class take, "class" one of its
    // class's pairs. Each follows the pair's names: "herbalism and mining
    // for the gold: ...".
    constexpr std::array<std::pair<const char*, const char*>, 15> kReasons = { {
        { "Profit.Own", "for the gold: herbs and ore sell well as they are, with nothing to make first" },
        { "Power.Own", "for the gear: they cut gems that go in anything" },
        { "Power.Armour", "for the gear: they make armour of the very kind worn" },
        { "PvP.Own", "for engineering's gadgets, which win fights against other players" },
        { "PvP.Armour", "for the gear: they make armour of the very kind worn" },
        { "Maker.Own", "for the love of making things" },
        { "Style.Own", "for looking good: they make things to wear" },
        { "Tinker.Own", "for the gadgets, and the things that go bang" },
        { "Pets.Own", "for engineering's mechanical pets; skinning a beast is out of the question" },
        { "Outdoors.Own", "for being out in the world gathering, not stood at a forge or a bench" },
        { "Scholar.Own", "for the learning in them: the scholar's trades" },
        { "OldWays.Class", "for being the old trades, nothing that came in later" },
        { "Heritage.Race", "for the knack for {knack} that comes with the race" },
        { "Contrary.Rare", "for being what hardly anyone of the class takes" },
        { "Newbie.Class", "because the guides say they suit the class" },
    } };

    std::string Lower(std::string text)
    {
        for (char& c : text)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    std::string Trim(const std::string& text)
    {
        const size_t from = text.find_first_not_of(" \t");
        if (from == std::string::npos)
            return {};
        return text.substr(from, text.find_last_not_of(" \t") - from + 1);
    }

    std::vector<std::string> Split(const std::string& text, char by)
    {
        std::vector<std::string> out;
        size_t from = 0;
        while (from <= text.size())
        {
            const size_t to = std::min(text.find(by, from), text.size());
            if (std::string part = Trim(text.substr(from, to - from)); !part.empty())
                out.push_back(std::move(part));
            from = to + 1;
        }
        return out;
    }

    // A trade's name as the game has it, in lower case: "first aid".
    std::string NameOf(uint32 skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        if (!line || !line->name[0] || !*line->name[0])
            return {};
        return Lower(line->name[0]);
    }

    // Its two professions by name, the gathering one first: "herbalism and
    // mining". Empty without two.
    std::string PairOf(Player* bot)
    {
        static const uint32 primary[] = { SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING, SKILL_ALCHEMY,
                                          SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                                          SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
                                          SKILL_TAILORING };
        std::vector<std::string> names;
        for (uint32 skill : primary)
            if (bot->HasSkill(skill))
                if (std::string name = NameOf(skill); !name.empty())
                    names.push_back(std::move(name));
        return names.size() == 2 ? names[0] + " and " + names[1] : std::string();
    }

    // Why `bot` has its two professions, when its leaning chose them: its
    // pair ("herbalism and mining") and the reason that follows it ("for the
    // gold: ..."). Nothing when its leaning did not choose them, or without
    // mod-playerbots-plus.
    bool ReasonOf(Player* bot, std::string& pair, std::string& reason)
    {
        const std::string leaning = Professions_LeaningOf(bot);
        if (leaning.empty())
            return false;
        const std::string how = OllamaLeaningMatch(bot);
        if (how.empty())
            return false;
        auto it = g_reasons.find(Lower(leaning) + "." + how);
        if (it == g_reasons.end() || it->second.empty())
            return false;
        pair = PairOf(bot);
        if (pair.empty())
            return false;
        reason = it->second;
        if (reason.find("{knack}") != std::string::npos)
        {
            const std::string knack = Lower(OllamaRaceKnack(bot->getRace()));
            if (knack.empty())
                return false;
            reason = SafeFormat(reason, fmt::arg("knack", knack));
        }
        return !reason.empty() && reason != "[Format Error]";
    }

    bool TalksOf(const std::string& text)
    {
        return !text.empty() &&
               !TextRules::FindPhraseIndexes(TextRules::WithoutBracketNames(TextRules::StripChatMarkup(text)), g_words)
                    .empty();
    }
}

void Professions_LoadConfig()
{
    auto text = [](const char* key, const char* def) { return sConfigMgr->GetOption<std::string>(key, def); };

    g_enable = sConfigMgr->GetOption<bool>("OllamaChat.Professions.Enable", true);

    g_leanings.clear();
    for (const std::string& entry : Split(text("OllamaChat.Professions.Leanings",
             "NAT_GOLDMAKER:Profit|NAT_RUDE:Profit|NAT_MINMAXER:Power|NAT_RAIDER:Power|NAT_EXPERT:Power|"
             "NAT_PVPER:PvP|NAT_FRIENDLY:Maker|NAT_HELPFUL_VET:Maker|NAT_SOCIAL:Maker|NAT_BUTTERFLY:Style|"
             "NAT_PRANKSTER:Tinker|NAT_JOKER:Tinker|NAT_TROLL:Tinker|NAT_HYPE:Tinker|NAT_PETLOVER:Pets|"
             "NAT_LONER:Outdoors|NAT_CHILL:Outdoors|NAT_LOREBUFF:Scholar|NAT_NOSTALGIC:OldWays|"
             "NAT_ROLEPLAYER:Heritage|NAT_CONTRARIAN:Contrary|NAT_NEWBIE:Newbie"), '|'))
    {
        const size_t colon = entry.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string personality = Trim(entry.substr(0, colon)), leaning = Trim(entry.substr(colon + 1));
        if (!personality.empty() && !leaning.empty())
            g_leanings[personality] = leaning;
    }

    // Why, by leaning and how: OllamaChat.Professions.Reason.<Leaning>.<How>.
    // With none in the conf, the module's own; with any, only those.
    g_reasons.clear();
    const std::string prefix = "OllamaChat.Professions.Reason.";
    const std::vector<std::string> keys = sConfigMgr->GetKeysByString(prefix);
    if (keys.empty())
        for (const auto& [key, why] : kReasons)
            g_reasons[Lower(key)] = why;
    for (const std::string& key : keys)
        g_reasons[Lower(key.substr(prefix.size()))] = sConfigMgr->GetOption<std::string>(key, "");

    g_words = CrowdText::ParsePhrases(text("OllamaChat.Professions.Words",
        "profession|professions|prof|profs|trade skill|trade skills|crafting|crafter|gathering|herbalism|herbalist|"
        "herbing|mining|miner|skinning|skinner|tailoring|tailor|enchanting|enchanter|alchemy|alchemist|"
        "blacksmithing|blacksmith|engineering|engineer|jewelcrafting|jewelcrafter|jc|leatherworking|"
        "leatherworker|inscription|scribe"));
    g_replyText = text("OllamaChat.Professions.ReplyText", "Your professions: {professions}.");
    g_noneText  = text("OllamaChat.Professions.NoneText", "You have no professions.");
    g_whyText   = text("OllamaChat.Professions.WhyText", "You have {pair} {reason}.");

    g_goldLeanings.clear();
    for (const std::string& leaning : Split(text("OllamaChat.Professions.GoldLeanings", "Profit"), '|'))
        g_goldLeanings.insert(Lower(leaning));
    g_goldChance = std::min<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Professions.GoldChance", 20), 100);
    g_goldText   = text("OllamaChat.Professions.GoldText",
        "You have {gold} to your name right now: any talk of your gold or your bank goes by that.");
}

void Professions_Init()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 13
    PlayerbotsPlus::SetLeaningProvider(Professions_LeaningOf);
#endif
}

std::string Professions_LeaningOf(Player* bot)
{
    if (!g_enable || !bot)
        return {};
    auto it = g_leanings.find(GetBotPersonality(bot));
    return it == g_leanings.end() ? std::string() : it->second;
}

std::string Professions_Of(Player* p)
{
    static const uint32 skills[] = { SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                                     SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING,
                                     SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING,
                                     SKILL_COOKING, SKILL_FIRST_AID, SKILL_FISHING };
    std::string out;
    if (!p)
        return out;
    for (uint32 skill : skills)
    {
        if (!p->HasSkill(skill))
            continue;
        const std::string name = NameOf(skill);
        if (!name.empty())
            out += (out.empty() ? "" : ", ") + name + " " + std::to_string(p->GetPureSkillValue(skill));
    }
    return out;
}

std::string Professions_WhyOf(Player* bot)
{
    std::string pair, reason;
    return ReasonOf(bot, pair, reason) ? pair + " " + reason : std::string();
}

std::string Professions_PromptFor(Player* bot, const std::vector<std::string>& heard)
{
    if (!g_enable || !bot || g_words.empty() || g_replyText.empty())
        return {};
    if (!std::any_of(heard.begin(), heard.end(), TalksOf))
        return {};
    const std::string professions = Professions_Of(bot);
    std::string out = professions.empty() ? g_noneText
                                          : SafeFormat(g_replyText, fmt::arg("professions", professions));
    if (out.empty() || out == "[Format Error]")
        return {};
    ++g_stats.told;
    // ...and why, when its leaning chose them: asked why those, it has its
    // answer.
    std::string pair, reason;
    if (!professions.empty() && !g_whyText.empty() && ReasonOf(bot, pair, reason))
    {
        const std::string line = SafeFormat(g_whyText, fmt::arg("pair", pair), fmt::arg("reason", reason));
        if (!line.empty() && line != "[Format Error]")
        {
            out += " " + line;
            ++g_stats.why;
        }
    }
    return out;
}

std::string Professions_GoldFor(Player* bot)
{
    if (!g_enable || !bot || g_goldChance == 0 || g_goldText.empty() || g_goldLeanings.empty())
        return {};
    const std::string leaning = Lower(Professions_LeaningOf(bot));
    if (leaning.empty() || !g_goldLeanings.contains(leaning))
        return {};
    // urand is inclusive at both ends: 1..100 against the chance.
    if (urand(1, 100) > g_goldChance)
        return {};
    const uint32_t money = bot->GetMoney();
    const std::string out =
        SafeFormat(g_goldText, fmt::arg("gold", money ? TextRules::MoneyWords(money) : std::string("no money")));
    if (out.empty() || out == "[Format Error]")
        return {};
    ++g_stats.gold;
    return out;
}

ProfessionsStats Professions_GetStats()
{
    ProfessionsStats s = g_stats;
    s.leanings = g_leanings.size();
    return s;
}
