#ifndef MOD_OLLAMA_CHAT_MARKS_H
#define MOD_OLLAMA_CHAT_MARKS_H

#include <cstdint>

// --------------------------------------------------------------------------
// Raid marks, as this module sees them.
//
// mod-playerbots-plus gives every crowd-controlling bot its mark, sends a
// rogue to sap the star, and moves the skull on; when the rogue cannot sap
// the star -- no Sap yet, a mob Sap does not work on, no stealth in time, a
// sap that did not take -- it says why (a Sap* event). Here, the rogue tells
// the group so in party, in its own words (Marks.SayPrompt with one of the
// Marks.Sap* situations), at most once per reason every
// Marks.SayCooldownSeconds.
//
// Without mod-playerbots-plus there is no sapping, and nothing is said.
// World thread only.
// --------------------------------------------------------------------------

// Startup: listen for mod-playerbots-plus's Sap* events.
void Marks_Init();

struct MarksStats
{
    uint64_t heard     = 0;   // saps mod-playerbots-plus said could not be done
    uint64_t explained = 0;   // lines the rogue said about it
};
MarksStats Marks_GetStats();

#endif // MOD_OLLAMA_CHAT_MARKS_H
