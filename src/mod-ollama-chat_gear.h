#ifndef MOD_OLLAMA_CHAT_GEAR_H
#define MOD_OLLAMA_CHAT_GEAR_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Gear talk that goes by what people really wear, and teasing that stops.
//
// "im wearing robes what you mean" had nothing to go on -- the bots knew
// nobody's gear -- so they made it up, and egged each other on: "those look
// like actual pieces of wood", "you definitely need a new tailor", "hard to
// be rude when you're wearing trees". Now:
//
//   * When gear comes up -- Gear.TalkWords in the line, or in the last few
//     lines of the chat ("gear", "robes", "wearing", "plate"...) -- the reply
//     prompt says what the people in the conversation visibly wear: whoever
//     spoke, anyone named, the real players in the group and the bot itself,
//     a few slots each, by name, quality and kind ("[Robe of Evocation], a
//     green cloth chest"). Gear.PromptText tells it to go only by that -- no
//     made-up looks, a light tease about something real at most once, no
//     piling on.
//   * A real player saying they feel picked on (Gear.BackOffWords: "rude",
//     "harsh", "leave me alone") gets the group's bots to drop it for
//     Gear.BackOffMinutes (Gear.BackOffText): a quick sorry, a kind word, or
//     something else to talk about.
//
// World thread only.
// --------------------------------------------------------------------------

// What `p` visibly wears, for a prompt: "Kami is wearing [Robe of Evocation]
// (green cloth chest), [Gnarled Staff] (white staff) and ..." -- "You are
// wearing" when `p` is `viewer`. Empty when nothing is worn.
std::string Gear_Describe(Player* p, Player* viewer);

// For a reply prompt of `bot` answering `speaker`: what the people in the
// conversation wear when gear is the topic (Gear.PromptText), and that a real
// player in its group felt picked on lately (Gear.BackOffText). Empty
// otherwise.
std::string Gear_PromptFor(Player* bot, Player* speaker, const std::string& msg, ChatChannelSourceLocal source,
                           const std::string& channelName);

// One piece of what `p` wears worth a remark: something grey if they wear
// any, else one of their helm, chest piece, trousers or weapon -- "[Ragged
// Cloak] (grey cloak)". Empty when they wear none of those.
std::string Gear_OnePiece(Player* p);

// The piece of what `p` wears that a passer-by would notice first: the best
// of their weapons, shield, helm and cloak (unless they hide them),
// shoulders, chest piece and tabard -- "[Arcanite Reaper] (purple two-handed
// axe)". Of two as good, either. Empty when they wear none of those.
std::string Gear_ShowPiece(Player* p);

// A real player's line: noted when they feel picked on.
void Gear_NoteHumanLine(Player* player, const std::string& msg);

// Has `player` said they feel picked on in the last Gear.BackOffMinutes?
// Nobody teases them back (mod-ollama-chat_banter) while they have.
bool Gear_FeelsPickedOn(Player* player);

// Is this line about gear (Gear.TalkWords)?
bool Gear_IsGearTalk(const std::string& msg);

void Gear_Update();   // maintenance: old back-offs are forgotten

struct GearStats
{
    uint64_t described = 0;   // reply prompts told what people wear
    uint64_t backOffs  = 0;   // real players who felt picked on
};
GearStats Gear_GetStats();

#endif // MOD_OLLAMA_CHAT_GEAR_H
