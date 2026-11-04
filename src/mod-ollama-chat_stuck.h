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
// Playerbots' loot code gets a mounted bot down before it opens something.
// When the opening then fails -- a mining vein another bot has hold of, a
// herb two bots went for at once, a body it cannot get close to -- the thing
// is put off for a second, then two, four, up to sixteen, and never written
// off while the bot is near it. In that pause the mount check -- once a
// second, for a random bot with nothing to fight -- puts it back on its
// mount, so the next try starts with another dismount: the bot gets on and
// off on the spot, every few seconds, for as long as nothing gives. (A try
// that only got the bot down does not count as a failed one.)
//
// Every time a bot mounts, it is noted (a UnitScript hook, on map threads:
// record only). A bot that mounts Stuck.MountCount times in
// Stuck.WindowSeconds without leaving a few yards of where it started, and
// without a fight anywhere in between (a bot grinding mobs that stand close
// together gets on and off on the spot too, between kills -- polled once a
// second), takes the hint on the world tick: what it was trying to loot is
// put out of its reach for Stuck.IgnoreMinutes -- no longer its loot target,
// held off in its loot list, written off as a gathering node -- so it goes
// and does something else. A random bot on its own (no group, no master) is
// also kept on foot for Stuck.WalkMinutes, so the loop does not start again
// at the next herb: playerbots' "mount" strategy, without which the mount
// check never puts a bot on its mount, is taken off it and given back when
// the time is up -- at once if it joins a group or gets a master, or this is
// switched off. Asked, it knows it was stuck (Stuck.StateText); grouped with
// a real player, or with one in earshot, it may laugh it off
// (Stuck.RemarkChance). Each one is logged at INFO, with where it happened,
// who it was with and whether anyone else was at the same thing.
//
// World thread only, except the hook.
// --------------------------------------------------------------------------

// World tick: what was noted, looked at; the bots that took a hint kept off
// what they were stuck on, and the ones on foot given their mount back when
// it is time. Call it with the module switched off too: then everyone on
// foot gets their mount back at once.
void Stuck_Update(uint32_t diff);

// For a reply prompt: that it was stuck a little while ago, and on what
// (Stuck.StateText). Empty otherwise.
std::string Stuck_StateTextFor(Player* bot);

struct StuckStats
{
    uint64_t mounts  = 0;   // mount auras noted
    uint64_t stuck   = 0;   // bots found getting on and off on the spot
    uint64_t hinted  = 0;   // ...that had something to be kept off
    uint64_t nothing = 0;   // ...that were trying to loot nothing at all
    uint64_t walked  = 0;   // ...that were kept on foot for a while
    uint64_t remarks = 0;   // lines about it
    uint32_t holding = 0;   // things kept out of reach right now
    uint32_t walking = 0;   // bots on foot right now
};
StuckStats Stuck_GetStats();

class OllamaChatStuckScript : public UnitScript
{
public:
    OllamaChatStuckScript() : UnitScript("OllamaChatStuckScript", true, { UNITHOOK_ON_AURA_APPLY }) {}
    void OnAuraApply(Unit* unit, Aura* aura) override;
};

#endif // MOD_OLLAMA_CHAT_STUCK_H
