#ifndef MOD_OLLAMA_CHAT_STUCK_H
#define MOD_OLLAMA_CHAT_STUCK_H

#include "ScriptMgr.h"

#include <cstdint>
#include <string>

class Aura;
class Player;
class Unit;

// --------------------------------------------------------------------------
// Bots stuck getting on and off their mount.
//
// A bot after something it cannot open -- a mining vein another bot has hold
// of, a herb its loot code keeps failing on -- is dismounted to open it, gets
// nowhere, is put back on its mount by the mount check, and is dismounted
// again, a few seconds apart, for as long as nothing gives. Two or three bots
// on one vein do it together. Playerbots never writes the thing off: an
// attempt that only dismounted the bot does not count as a failed one.
//
// Every time a bot mounts, it is noted (a UnitScript hook, on map threads:
// record only). A bot that mounts Stuck.MountCount times in
// Stuck.WindowSeconds without leaving a few yards of where it started, out of
// any fight, takes the hint on the world tick: what it was trying to loot is
// put out of its reach for Stuck.IgnoreMinutes -- no longer its loot target,
// held off in its loot list, written off as a gathering node -- so it goes
// and does something else. Asked, it knows it was stuck (Stuck.StateText);
// grouped with a real player, or with one in earshot, it may laugh it off
// (Stuck.RemarkChance). Each one is logged at INFO, with where it happened.
//
// World thread only, except the hook.
// --------------------------------------------------------------------------

// World tick: what was noted, looked at; the bots that took a hint kept off
// what they were stuck on.
void Stuck_Update(uint32_t diff);

// For a reply prompt: that it was stuck a little while ago, and on what
// (Stuck.StateText). Empty otherwise.
std::string Stuck_StateTextFor(Player* bot);

struct StuckStats
{
    uint64_t mounts  = 0;   // mount auras noted
    uint64_t stuck   = 0;   // bots found getting on and off on the spot
    uint64_t hinted  = 0;   // ...that had something to be kept off
    uint64_t nothing = 0;   // ...that were trying to loot nothing at all: left alone
    uint64_t remarks = 0;   // lines about it
    uint32_t holding = 0;   // things kept out of reach right now
};
StuckStats Stuck_GetStats();

class OllamaChatStuckScript : public UnitScript
{
public:
    OllamaChatStuckScript() : UnitScript("OllamaChatStuckScript", true, { UNITHOOK_ON_AURA_APPLY }) {}
    void OnAuraApply(Unit* unit, Aura* aura) override;
};

#endif // MOD_OLLAMA_CHAT_STUCK_H
