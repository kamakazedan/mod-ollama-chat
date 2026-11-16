#ifndef MOD_OLLAMA_CHAT_CITYSAY_H
#define MOD_OLLAMA_CHAT_CITYSAY_H

#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// In a city, General is where people talk; /say is for what is said to
// someone.
//
// Bots chatting away in /say had chat bubbles popping up all round a player
// standing about in Stormwind. So in a city (OllamaInCity), a real player
// hears a bot start something nobody asked for in /say -- an ambient line, an
// ask, a remark to someone walking past, a line of city life, a /say exchange
// between bots -- at most once in City.SayGapSeconds, the clock starting
// again with every such line they hear; and of the ambient lines said near
// them, only City.SayShare in a hundred go to /say rather than General. A
// bot's /say line there gets no city bonus to its answers, and no
// conversation grows out of one. An answer owed to a real player who spoke
// is never held back, nor is a gesture: emotes are as they were.
//
// World thread only.
// --------------------------------------------------------------------------

// Whether `bot` may start a line nobody asked for in /say now: always
// outside a city; in one, when every real player in earshot of it
// (SayDistance) has heard none for City.SayGapSeconds.
bool CitySay_Free(Player* bot);

// ...and it starts one: true when it may (those in earshot have now heard
// one), false when it may not. Called just before the line is submitted.
// Always true outside a city.
bool CitySay_Claim(Player* bot);

// A bot's line went out in /say that was not an answer owed to a real
// player: in a city, those in earshot heard one now (the dispatcher, at
// delivery). Nothing outside a city.
void CitySay_Heard(Player* bot);

struct CitySayStats
{
    uint64_t started = 0;   // lines nobody asked for started in a city's /say
    uint64_t held    = 0;   // ...held back: someone in earshot had heard one lately
    uint64_t heard   = 0;   // lines not owed to anyone that went out there
};
CitySayStats CitySay_GetStats();

#endif // MOD_OLLAMA_CHAT_CITYSAY_H
