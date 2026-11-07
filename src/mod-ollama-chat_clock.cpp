#include "mod-ollama-chat_clock.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_context.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat-utilities.h"

#include "GameTime.h"

#include <ctime>
#include <fmt/format.h>

namespace
{
    tm Now()
    {
        const time_t now = static_cast<time_t>(GameTime::GetGameTime().count());
        tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        return local;
    }

    const char* const kDays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
}

int Clock_Weekday()
{
    const int day = Now().tm_wday;
    return (day >= 0 && day <= 6) ? day : 0;
}

std::string Clock_WeekdayName()
{
    return kDays[Clock_Weekday()];
}

std::string Clock_PartOfDay()
{
    const int hour = Now().tm_hour;
    if (hour < 5)  return "the dead of night";
    if (hour < 8)  return "early morning";
    if (hour < 12) return "mid-morning";
    if (hour < 14) return "midday";
    if (hour < 18) return "afternoon";
    if (hour < 21) return "evening";
    return "night";
}

std::string Clock_NowText()
{
    const std::string part = Clock_PartOfDay();
    // "Saturday afternoon"; "Saturday, the dead of night".
    return Clock_WeekdayName() + (part.compare(0, 4, "the ") == 0 ? ", " : " ") + part;
}

std::string Clock_PromptFor(const std::string& msg)
{
    if (g_ClockPromptText.empty() || g_ClockWords.empty() || msg.empty())
        return {};
    if (!CrowdText::ContainsPhrase(CrowdText::Words(Context_CleanText(msg)), g_ClockWords))
        return {};
    const tm now = Now();
    const std::string out = SafeFormat(g_ClockPromptText, fmt::arg("now", Clock_NowText()),
                                       fmt::arg("weekday", Clock_WeekdayName()),
                                       fmt::arg("part_of_day", Clock_PartOfDay()),
                                       fmt::arg("time", fmt::format("{:02}:{:02}", now.tm_hour, now.tm_min)));
    return out == "[Format Error]" ? std::string() : out;
}
