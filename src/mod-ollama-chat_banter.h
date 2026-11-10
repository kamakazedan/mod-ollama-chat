#ifndef MOD_OLLAMA_CHAT_BANTER_H
#define MOD_OLLAMA_CHAT_BANTER_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Banter that bites back.
//
// A bot called a noob in chat answered "Fair point, I'll keep farming". The
// system prompt says banter is how people here get on, but a model told
// nothing at the moment it matters answers a dig politely, as a complaint.
// Now a real player's line with a tease in it -- Banter.Words ("noob", "ur
// mum", "l2p"), or any of Response.InsultWords -- gets each reply prompt told
// so, last, with the reply hint: it is banter, not a real complaint, and a
// bot it was aimed at fires one back in its own style, one of a few kinds of
// comeback picked each time (Banter.Text). Someone who has said they feel
// picked on (Gear.BackOffWords) gets none of this for Gear.BackOffMinutes.
//
// World thread only.
// --------------------------------------------------------------------------

// For a reply prompt of `bot` answering real player `speaker`'s `msg`: that it
// is a tease, and to give as good as it gets. Empty otherwise.
std::string Banter_PromptFor(Player* bot, Player* speaker, const std::string& msg);

struct BanterStats
{
    uint64_t teases = 0;   // reply prompts told a line was a tease
};
BanterStats Banter_GetStats();

#endif // MOD_OLLAMA_CHAT_BANTER_H
