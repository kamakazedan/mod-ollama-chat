#include "mod-ollama-chat_banter.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_gear.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "Player.h"

#include <fmt/core.h>

#include <random>

namespace
{
    BanterStats g_stats;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    // The first of `phrases` the line has, as words; empty when none.
    std::string FirstOf(const std::vector<std::string>& words, const std::vector<std::vector<std::string>>& phrases)
    {
        for (const std::vector<std::string>& p : phrases)
        {
            if (!CrowdText::ContainsPhrase(words, { p }))
                continue;
            std::string out;
            for (const std::string& w : p)
                out += (out.empty() ? "" : " ") + w;
            // "haha*": show the stem, not the wildcard.
            while (!out.empty() && out.back() == '*')
                out.pop_back();
            return out;
        }
        return {};
    }
}

std::string Banter_PromptFor(Player* bot, Player* speaker, const std::string& msg)
{
    if (!bot || !speaker || g_BanterText.empty() || !OllamaIsRealPlayer(speaker))
        return {};
    // They said they feel picked on: leave them be.
    if (Gear_FeelsPickedOn(speaker))
        return {};

    const std::vector<std::string> words = CrowdText::Words(Context_CleanText(msg));
    if (words.empty())
        return {};
    std::string tease = FirstOf(words, g_BanterWords);
    if (tease.empty())
        tease = FirstOf(words, g_ResponseInsultWords);
    if (tease.empty())
        return {};

    // "{~a cheeky comeback|mock outrage|...}": one kind each time, or every
    // bot would reach for the same joke.
    const std::string templ = TextRules::ExpandChoices(g_BanterText, Rng());
    std::string text = SafeFormat(templ, fmt::arg("player_name", speaker->GetName()), fmt::arg("tease", tease));
    if (text.empty() || text == "[Format Error]")
        return {};
    ++g_stats.teases;
    return text;
}

BanterStats Banter_GetStats()
{
    return g_stats;
}
