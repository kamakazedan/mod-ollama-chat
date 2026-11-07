#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_plus.h"
#include "mod-ollama-chat-utilities.h"

#include "Player.h"

#include <fmt/core.h>

// The hangouts themselves are mod-playerbots-plus's (Hangout.* there).

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 2

bool Hangout_IsAway(Player* bot)
{
    return PlayerbotsPlus::IsAway(bot);
}

void Hangout_ComeBack(Player* bot)
{
    PlayerbotsPlus::ComeBack(bot);
}

bool Hangout_UsingBench()
{
    return PlayerbotsPlus::UsingBench();
}

std::string Hangout_StateTextFor(Player* bot)
{
    PlayerbotsPlus::HangoutState state;
    if (g_HangoutStateText.empty() || !PlayerbotsPlus::GetHangout(bot, state))
        return {};
    std::string out = SafeFormat(g_HangoutStateText, fmt::arg("spot", state.spot), fmt::arg("doing", state.doing));
    if (out == "[Format Error]")
        out.clear();
    if ((state.away || state.justBack) && !g_HangoutAwayText.empty())
        out += (out.empty() ? "" : " ") + g_HangoutAwayText;
    return out;
}

bool Hangout_WhereAndDoing(Player* bot, std::string& spot, std::string& doing)
{
    PlayerbotsPlus::HangoutState state;
    if (!PlayerbotsPlus::GetHangout(bot, state))
        return false;
    spot  = std::move(state.spot);
    doing = std::move(state.doing);
    return true;
}

#else

bool Hangout_IsAway(Player* /*bot*/)
{
    return false;
}

void Hangout_ComeBack(Player* /*bot*/)
{
}

bool Hangout_UsingBench()
{
    return false;
}

std::string Hangout_StateTextFor(Player* /*bot*/)
{
    return {};
}

bool Hangout_WhereAndDoing(Player* /*bot*/, std::string& /*spot*/, std::string& /*doing*/)
{
    return false;
}

#endif
