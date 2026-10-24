#ifndef MOD_OLLAMA_CHAT_PARTYCMD_H
#define MOD_OLLAMA_CHAT_PARTYCMD_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// Party instructions for mod-dungeon-clear.
//
// Inside a dungeon or raid, with a dungeon-clear tank bot in the group, a real
// player's short party line can steer the run the way it would steer a group
// of people:
//
//   "ready", "r?", "lets go", "ok carry on"   start the clear, or resume it
//   "mb", "wait", "stop", "brb", "need mana"  pause it where it stands
//
// The tank answers in party ("np, drink up, say when"), and the line gets no
// other replies -- so "mb" is never read as "my bad" again. A start waits for
// every real player in the group to be inside and near the tank.
//
// Only lines that are short and plainly an instruction count; questions
// ("wait what?", "you oom?") and negations ("not ready", "i dont need mana")
// do not -- see TextRules::MatchPartyIntent. Lines starting "dc" are
// mod-dungeon-clear's own commands: it answers those itself, and bots do not
// chat about them.
//
// These are looked at before the BlacklistCommands and DisableForParty checks:
// "ready", "wait" and "drink" are on the blacklist because they are playerbots
// commands too, and DisableForParty is about chatter, not about steering a run.
//
// Compiled in only when mod-dungeon-clear is in the same build; otherwise
// every function here is a no-op.
//
// World thread only.
// --------------------------------------------------------------------------

// Was mod-dungeon-clear found at build time?
bool PartyCmd_Available();

enum class PartyCmdLine : uint8_t
{
    None,          // ordinary chat
    Instruction,   // "ready", "mb", "wait", in a dungeon with a dungeon-clear tank
    DcCommand      // "dc on", "dc pause": mod-dungeon-clear's own, it answers
};

// What a real player's party or raid line is, before anything else looks at
// it. No side effects.
PartyCmdLine PartyCmd_Classify(Player* player, const std::string& msg, ChatChannelSourceLocal source);

// A real player's party or raid line. True when it was a dungeon-clear
// instruction and has been dealt with: the caller sends no other replies.
bool PartyCmd_HandleHumanLine(Player* player, const std::string& msg,
                              ChatChannelSourceLocal source, const std::string& scopeKey,
                              uint64_t seq);

// The group-chat glossary for a bot answering in party or raid chat inside a
// dungeon or raid (OllamaChat.Party.DungeonGlossary); empty elsewhere.
std::string PartyCmd_GlossaryFor(Player* bot, ChatChannelSourceLocal source);

struct PartyCmdStats
{
    uint64_t started = 0;
    uint64_t paused  = 0;
    uint64_t resumed = 0;
    uint64_t waiting = 0;   // "ready", but someone was not there yet
    uint64_t refused = 0;   // dungeon clear said no (whispered why)
};
PartyCmdStats PartyCmd_GetStats();

#endif // MOD_OLLAMA_CHAT_PARTYCMD_H
