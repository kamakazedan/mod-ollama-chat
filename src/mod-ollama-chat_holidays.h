#ifndef MOD_OLLAMA_CHAT_HOLIDAYS_H
#define MOD_OLLAMA_CHAT_HOLIDAYS_H

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// What is on in the world (OllamaChat.Holidays.*).
//
// Brewfest, Hallow's End, Winter Veil, the Darkmoon Faire, a battleground's
// bonus weekend, the fishing tournament: when one is on, people talk about
// it, and when it is not, nobody does. A bot knew neither -- the model has
// read about all of them, and would as soon wish everyone a merry Winter
// Veil in July, or have nothing to say of the feast tables outside the gates
// in the week they stand there.
//
// Which holidays the realm is running is the game's own calendar
// (IsHolidayActive, looked at once a minute). For each one in
// Holidays.List -- its name, the words it goes by, and a line of what is
// true of it:
//
// - While it is on, it is something to talk about: a conversation premise
//   with {holiday} in it is filled with its name and that line, and is not
//   picked when nothing is on (mod-ollama-chat_premises).
// - A reply where the talk is of one is told whether it is on, and what is
//   true of it (Holidays.OnText, Holidays.OffText).
// - A bot's line that talks of one that is not on is not said
//   (Holidays.Guard), unless it answers a person who brought it up: one owed
//   to someone gets another go, told it is not on (Holidays.OffNote).
//
// World thread only.
// --------------------------------------------------------------------------

// Holidays.List, at startup and on reload: "id;Name;word,word;what is true
// of it|...". An entry that does not read is left out, and logged.
void Holidays_Load(const std::string& list);

void Holidays_Update(uint32_t diff);   // world tick: once a minute, which are on

// For a premise: one that is on now, "Brewfest (which is on now: ...)".
// Empty when none is.
std::string Holidays_Fact();

// For a reply: of each holiday any of `heard` talks of, whether it is on and
// what is true of it. Empty when none does.
std::string Holidays_PromptFor(const std::vector<std::string>& heard);

// Whether a line may go out: not when it talks of a holiday that is not on,
// unless `byPerson` and `answering` (the line it answers) talks of the same
// one. `which` gets its name, `note` what a second go is told.
bool Holidays_MayGoOut(const std::string& text, const std::string& answering, bool byPerson, std::string& which,
                       std::string& note);

struct HolidaysStats
{
    uint32_t known  = 0;   // holidays in the list
    uint32_t on     = 0;   // ...on now
    uint64_t told   = 0;   // replies told of one
    uint64_t topics = 0;   // premises about one
    uint64_t keptIn = 0;   // lines about one that is not on, not said
    std::string names;     // those on now
};
HolidaysStats Holidays_GetStats();

#endif // MOD_OLLAMA_CHAT_HOLIDAYS_H
