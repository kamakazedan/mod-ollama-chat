#ifndef MOD_OLLAMA_CHAT_MARKS_H
#define MOD_OLLAMA_CHAT_MARKS_H

#include <cstdint>

// --------------------------------------------------------------------------
// Raid marks, the way groups use them, so a plan needs no barking:
//
//   skull     kill first: putting it on a mob starts the fight
//   cross     kill next: it becomes the skull when the skull dies
//   moon      polymorph (mage)          star      sap (rogue)
//   diamond   banish, fear (warlock)    circle    shackle undead (priest)
//   triangle  hibernate, roots, cyclone (druid)
//   square    freezing trap (hunter)
//
// Every bot whose class can crowd-control is told which mark is its to
// control (playerbots' "rti cc", OllamaChat.Marks.ClassIcons) and gets
// playerbots' "cc" strategy if it lacks it. Marks on mobs nobody is fighting
// start nothing; the skull starts the fight, as it always has (it is every
// bot's target), and each one controls its own mark as the pack comes in.
// The star is the exception, since a sap has to land before the pull: put it
// on a mob that is not fighting and a rogue in the group sneaks over and saps
// it (OllamaChat.Marks.SapStar). It goes into stealth first -- waiting for
// Stealth to come off cooldown rather than walking up in plain sight -- comes
// round to the mob's back, since a mob only spots stealth in front of it,
// stops, faces it and saps. A rogue that cannot (no Sap yet, a mob Sap does
// not work on, no stealth in time) leaves it alone and says why in party.
//
// While someone in the group is marking (a mark changed in the last few
// minutes that this did not make), the skull moves on by itself when its mob
// dies: to the cross, else to the nearest mob still fighting the group that
// nobody marked to control (OllamaChat.Marks.AutoSkull), so the bots finish
// the loose mobs before breaking anything that is controlled. Only ever onto
// a mob in the fight already: a cross on the next pack waits for its skull.
//
// World thread only.
// --------------------------------------------------------------------------

void Marks_Update(uint32_t diff);

struct MarksStats
{
    uint64_t assigned = 0;   // bots told which mark is theirs
    uint64_t moved    = 0;   // skull moved to the cross
    uint64_t picked   = 0;   // skull put on the next loose mob
    uint64_t saps      = 0;   // stars sapped
    uint64_t sapFails  = 0;   // the cast would not go out
    uint64_t sapMissed = 0;   // cast, but it did not take
    uint64_t sapGaveUp = 0;   // no stealth in time, lost it, or could not get round
    uint64_t sapCant   = 0;   // no Sap yet, or a mob it does not work on
    uint64_t explained = 0;   // lines the rogue said about it
    uint32_t lastFail  = 0;   // the last failed cast's SpellCastResult
};
MarksStats Marks_GetStats();

#endif // MOD_OLLAMA_CHAT_MARKS_H
