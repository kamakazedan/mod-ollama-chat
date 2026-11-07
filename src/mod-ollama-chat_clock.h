#ifndef MOD_OLLAMA_CHAT_CLOCK_H
#define MOD_OLLAMA_CHAT_CLOCK_H

#include <string>

// --------------------------------------------------------------------------
// What day and what time it is, for the people in chat.
//
// "It is far too quiet for a Tuesday" -- on a Saturday. The model has no
// calendar, and the time-of-day topic only ever told it "afternoon". Now:
//
//   * that topic says "Saturday afternoon" ({time_of_day}; {weekday} and
//     {part_of_day} are there too);
//   * a reply to a line about days or times (Clock.Words: "today", "the
//     weekend", "what time"...) is told the day and the time
//     (Clock.PromptText);
//   * a weekday a bot uses for today in any line it says is put right
//     (Clock.FixWeekdays, TextRules::FixWeekday): "for a Tuesday" on a
//     Saturday goes out as "for a Saturday".
//
// Server time: the time the realm's players live by. World thread.
// --------------------------------------------------------------------------

int         Clock_Weekday();       // 0 Sunday .. 6 Saturday
std::string Clock_WeekdayName();   // "Saturday"
std::string Clock_PartOfDay();     // "afternoon"
std::string Clock_NowText();       // "Saturday afternoon"

// For a reply prompt: the day and the time, when the line is about days or
// times. Empty otherwise.
std::string Clock_PromptFor(const std::string& msg);

#endif // MOD_OLLAMA_CHAT_CLOCK_H
