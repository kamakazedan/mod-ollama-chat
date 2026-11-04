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
//   "ok im done", "full", "done drinking"     resume it -- only while paused
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

// "whats next?", "why are we waiting", "how many bosses left": a question
// about the run (OllamaChat.PartyCommands.RunQuestionPhrases), in a dungeon
// with a dungeon-clear bot tank. The tank, which answers it and nobody else;
// nullptr for anything else, or with no bot tank to ask (the player tanks).
Player* PartyCmd_RunVoiceFor(Player* player, const std::string& msg, ChatChannelSourceLocal source);

// A real player's party or raid line. True when it was a dungeon-clear
// instruction and has been dealt with: the caller sends no other replies.
bool PartyCmd_HandleHumanLine(Player* player, const std::string& msg,
                              ChatChannelSourceLocal source, const std::string& scopeKey,
                              uint64_t seq);

// The tank's answer to an instruction went out: remember it for its group, so
// the next answer can be told not to copy it (PartyCommands.RecentAcks).
void PartyCmd_NoteAck(Player* bot, const std::string& line);

// The tank's "everyone ready?" went out: for Party.ReadyWindowSeconds a plain
// "yep" or "sure" from the group answers it and starts the run.
void PartyCmd_NoteReadyAsked(Player* bot);

// The tank's "that's the dungeon cleared" went out: the rest of the group says
// gg, each once and to nobody in particular (Party.Cheer*).
void PartyCmd_NoteDoneCalled(Player* bot);

// A real player's party or raid line once a run is over. "wait", "can you help
// me with..." keeps the bots that were about to leave (Party.DoneStayWords);
// after that, "ok bye" lets them go after all (Party.DoneGoWords). Only looks:
// the line is answered as usual.
void PartyCmd_NoteHumanPartyLine(Player* player, const std::string& msg, ChatChannelSourceLocal source);

// For a party or raid prompt once a run is over, in the copy of the dungeon it
// was in: that it is over, and whether the group is about to split up
// (Party.DoneStateText). Empty otherwise.
std::string PartyCmd_DoneStateFor(Player* bot, ChatChannelSourceLocal source);

// The group-chat glossary for a bot answering in party or raid chat inside a
// dungeon or raid (OllamaChat.Party.DungeonGlossary); empty elsewhere.
std::string PartyCmd_GlossaryFor(Player* bot, ChatChannelSourceLocal source);

// Inside a dungeon-clear run, for a bot answering in party or raid chat: who
// leads, how many bosses are down, which is next and what the group is doing
// right now (OllamaChat.Party.RunStateText), so it can answer "which boss is
// next?" or "what are we waiting for?". Empty outside a run.
std::string PartyCmd_RunStateFor(Player* bot, ChatChannelSourceLocal source);

// For a party or raid prompt inside a dungeon: that the bot and everyone in
// its group are doing this dungeon together, and who tanks it
// (Party.TogetherText) -- so nobody says "good luck with the run" to the run
// they are in, or asks for a group for a quest out in the world. Empty
// anywhere else.
std::string PartyCmd_TogetherFor(Player* bot, ChatChannelSourceLocal source);

// The world tick, every couple of seconds: a bot tank leading a dungeon-clear
// group greets it when it comes into a dungeon ("let me know when you're ready
// to start", OllamaChat.Party.Greet*) and says so in party when every boss is
// down (OllamaChat.Party.Done*), in its own chat style -- in words that make
// it plain the dungeon is cleared, or in stock ones (Party.DoneLines). The rest
// of the group says gg, and in a dungeon-finder group the random bots leave
// one by one, Party.DoneLeaveSeconds later, unless someone asks them to stay.
void PartyCmd_Update(uint32_t diff);

// Notes who joined a group while it was a dungeon-finder group -- the
// players the finder matched (and the first of a group it made) -- and who
// was invited by whom, so that after a run only the finder's bots leave: not
// ones a player invited, even when they queued together and the finder
// rebuilt the group around them. Group::AddMember and AddInvite can run on
// map threads: this only records, under a lock.
class OllamaChatFinderJoinScript : public GroupScript
{
public:
    OllamaChatFinderJoinScript()
        : GroupScript("OllamaChatFinderJoinScript", { GROUPHOOK_ON_ADD_MEMBER, GROUPHOOK_ON_INVITE_MEMBER }) {}
    void OnAddMember(Group* group, ObjectGuid guid) override;
    void OnInviteMember(Group* group, ObjectGuid guid) override;
};

struct PartyCmdStats
{
    uint64_t started = 0;
    uint64_t paused  = 0;
    uint64_t resumed = 0;
    uint64_t waiting = 0;   // "ready", but someone was not there yet
    uint64_t refused = 0;   // dungeon clear said no (whispered why)
    uint64_t done    = 0;   // runs the tank called finished
    uint64_t cheers  = 0;   // gg lines from the rest
    uint64_t left    = 0;   // bots that left the group after a run
    uint64_t stayed  = 0;   // groups asked to stay
};
PartyCmdStats PartyCmd_GetStats();

#endif // MOD_OLLAMA_CHAT_PARTYCMD_H
