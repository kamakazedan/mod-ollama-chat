#include "mod-ollama-chat_favor.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_hangout.h"
#include "mod-ollama-chat_personality.h"

#include "Group.h"
#include "Player.h"
#include "Random.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <cctype>
#include <string>

namespace
{
    // Outlasts a world tick or two. Refreshed every tick while the trade
    // lasts, so the AI is back within this of it ending -- and never cut
    // short: a longer hold (a city hangout's) stands.
    constexpr uint32_t kHoldMs = 1500;

    const BuffStyle& StyleOf(Player* bot)
    {
        std::string key = GetBotPersonality(bot);
        for (char& ch : key)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = g_BuffStyles.find(key);
        return it == g_BuffStyles.end() ? g_BuffStyleDefault : it->second;
    }

    bool Chance(uint32_t percent)
    {
        return percent > 0 && (percent >= 100 || urand(1, 100) <= percent);
    }
}

FavorOutcome Favor_Roll(Player* bot, bool spokenTo)
{
    const BuffStyle& s = StyleOf(bot);
    const uint32_t ignore = spokenTo ? s.ignore / 2 : s.ignore;
    const uint32_t total  = s.give + s.quiet + s.no + ignore;
    if (total == 0)
        return FavorOutcome::Ignore;
    uint32_t r = urand(1, total);
    if (r <= s.give)
        return FavorOutcome::Give;
    r -= s.give;
    if (r <= s.quiet)
        return FavorOutcome::Quiet;
    r -= s.quiet;
    if (r <= s.no)
        return FavorOutcome::No;
    return FavorOutcome::Ignore;
}

bool Favor_Explains(Player* bot)
{
    return Chance(StyleOf(bot).explain);
}

bool Favor_Welcomes(Player* bot)
{
    return Chance(StyleOf(bot).welcome);
}

bool Favor_IsStranger(Player* bot, Player* player)
{
    if (!bot || !player)
        return false;
    Group* group = player->GetGroup();
    if (group && group == bot->GetGroup())
        return false;
    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    return !(ai && ai->GetMaster() == player);
}

bool Favor_CanAnswer(Player* bot, bool spokenTo)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetVehicle() ||
        bot->IsBeingTeleported())
        return false;
    if (bot->isAFK() || bot->isDND())
        return spokenTo && Hangout_IsAway(bot);
    return true;
}

void Favor_ComeBack(Player* bot)
{
    if (bot)
        Hangout_ComeBack(bot);
}

bool Favor_TradesWithStrangers()
{
    return g_FavorStrangers;
}

void Favor_HoldAi(Player* bot)
{
    if (PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr)
        ai->YieldThread(bot, kHoldMs);
}
