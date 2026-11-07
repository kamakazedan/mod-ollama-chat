#ifndef MOD_OLLAMA_CHAT_STUCK_H
#define MOD_OLLAMA_CHAT_STUCK_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Bots stuck getting on and off their mount, as this module sees it.
//
// mod-playerbots-plus notices a bot getting on and off its mount on the spot
// -- a herb two bots went for at once, a vein it cannot open -- keeps it off
// that thing for a while and says so (a StuckMount event). Here, the bot
// knows it was stuck when someone asks (Stuck.StateText, for
// Stuck.RememberMinutes), and grouped with a real player, or with one in
// earshot, it may laugh it off (Stuck.RemarkChance).
//
// Without mod-playerbots-plus nothing is noticed, and nothing is said.
// World thread only.
// --------------------------------------------------------------------------

// Startup: listen for mod-playerbots-plus's StuckMount events.
void Stuck_Init();

// For a reply prompt: that it was stuck a little while ago, and on what
// (Stuck.StateText). Empty otherwise.
std::string Stuck_StateTextFor(Player* bot);

struct StuckStats
{
    uint64_t heard   = 0;   // bots mod-playerbots-plus found stuck on something
    uint64_t remarks = 0;   // lines about it
};
StuckStats Stuck_GetStats();

#endif // MOD_OLLAMA_CHAT_STUCK_H
