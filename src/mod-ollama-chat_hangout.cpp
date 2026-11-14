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

// Duels talked into: mod-playerbots-plus walks them out to the gates and has
// them fight it out there (Duels.* there).

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 3

bool Hangout_CanGoDuel(Player* a, Player* b, std::string& where)
{
    return PlayerbotsPlus::CanGoDuel(a, b, where);
}

bool Hangout_GoDuel(Player* a, Player* b)
{
    return PlayerbotsPlus::GoDuel(a, b);
}

#else

bool Hangout_CanGoDuel(Player* /*a*/, Player* /*b*/, std::string& /*where*/)
{
    return false;
}

bool Hangout_GoDuel(Player* /*a*/, Player* /*b*/)
{
    return false;
}

#endif

// City life: the crowd's own goings-on are mod-playerbots-plus's (CityLife.*
// there); this only tells it who is talking.
#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 5

void Hangout_Busy(Player* bot, uint32_t seconds)
{
    if (bot)
        PlayerbotsPlus::CityBusy(bot, seconds);
}

void Hangout_Spoke(Player* speaker)
{
    if (speaker)
        PlayerbotsPlus::CitySpoke(speaker);
}

void Hangout_Gather(const std::vector<Player*>& bots, uint32_t seconds)
{
    PlayerbotsPlus::CityGather(bots, seconds);
}

#else

void Hangout_Busy(Player* /*bot*/, uint32_t /*seconds*/)
{
}

void Hangout_Spoke(Player* /*speaker*/)
{
}

void Hangout_Gather(const std::vector<Player*>& /*bots*/, uint32_t /*seconds*/)
{
}

#endif
