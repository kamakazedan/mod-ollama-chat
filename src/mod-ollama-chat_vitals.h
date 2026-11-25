#ifndef MOD_OLLAMA_CHAT_VITALS_H
#define MOD_OLLAMA_CHAT_VITALS_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// How the fights have really gone (OllamaChat.Vitals.*).
//
// A healer drank a mana potion in a dungeon and said "THANK GOD FOR THAT MANA
// POTION!"; the tank, who had never been in any trouble, answered "nearly
// dead there for a sec", and the healer "CLOSE CALL! YOU WERE LOOKING PRETTY
// SHAKY!". Nobody had been told how anyone's health had been, so the model
// made the drama up.
//
// Everyone in a group with someone playing in it is looked at once a second:
// health, mana, fighting or not, alive or not. Kept for Vitals.Minutes, ten
// seconds to a slot.
//
// - A reply in the group's own chat where the talk is of it
//   (Vitals.TalkWords: health, mana, a potion, a close call, dying) is told
//   each member's lowest and average health and mana over that time, and who
//   died (Vitals.Text): the truth to talk from.
// - A line in the group's chat that says someone nearly died
//   (Vitals.ScareWords) goes out only if someone's health really was at
//   Vitals.ScarePct or less, or someone died; one that says someone ran out
//   of mana (Vitals.DryWords) only if someone's mana was at Vitals.DryPct or
//   less. Otherwise it is not said: one owed to someone gets another go,
//   told how it was (Vitals.ScareNote, Vitals.DryNote). A line answering a
//   person who said so themselves is let through: it was told how it was.
//
// World thread only.
// --------------------------------------------------------------------------

void Vitals_Update(uint32_t diff);   // world tick

// A dip a look a second may have missed: health or mana at `healthPct` /
// `manaPct` (-1: not this one) just now -- a potion drunk at it.
void Vitals_NoteLow(Player* who, int32_t healthPct, int32_t manaPct);

// For a reply of `bot` in its group's chat: how the last Vitals.Minutes have
// gone for the group, when any of `heard` -- the line answered, and those
// before it -- talks of it. Empty otherwise, or with nothing known.
std::string Vitals_PromptFor(Player* bot, ChatChannelSourceLocal source, const std::vector<std::string>& heard);

// Whether a line of `bot`'s may go out in `source`: always but in its
// group's chat, and there unless it says someone nearly died or ran dry when
// nobody did. `answering`: the line it answers, `byPerson` when a person
// said it. False: `which` gets the words, `note` what to tell it for another
// go.
bool Vitals_MayGoOut(Player* bot, ChatChannelSourceLocal source, const std::string& text,
                     const std::string& answering, bool byPerson, std::string& which, std::string& note);

struct VitalsStats
{
    uint32_t members = 0;   // people looked at now
    uint64_t told    = 0;   // replies told how it had gone
    uint64_t keptIn  = 0;   // lines of a scare or of no mana not said: there was none
    uint64_t dips    = 0;   // dips noted between looks (a potion)
};
VitalsStats Vitals_GetStats();

#endif // MOD_OLLAMA_CHAT_VITALS_H
