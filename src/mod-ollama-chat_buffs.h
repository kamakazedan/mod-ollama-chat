#ifndef MOD_OLLAMA_CHAT_BUFFS_H
#define MOD_OLLAMA_CHAT_BUFFS_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Buffs, on request.
//
// Someone asks for a buff the way people do -- "can i get a buff", "buff pls",
// "anyone got fort?", "Verona int pls" -- in /say, or whispers a bot for one,
// and a bot standing near that can put one on anybody might: a priest Power
// Word: Fortitude, a mage Arcane Intellect, a druid Mark of the Wild, a
// paladin a blessing. It really casts it, a moment later, the way people do:
// off its mount or out of its form first, with its own mana.
//
// Whether it does, and whether it says anything, is its chat style's
// (OllamaChat.Buff.Style.<KEY>): buff them and say so, buff them without a
// word, say no, or let it go by. Asked by name, or by the one they have
// targeted or were talking to, a bot that can't may say why: it is a warrior,
// it is out of mana, they have it already. A "ty" soon after gets a word back
// from the one who gave it, or nothing, and that is the end of it: nobody else
// answers it either.
//
// Bots in their group, and bots they are master of, are left out: playerbots
// keeps its own group buffed, and "buff us" in party chat is an order
// (mod-ollama-chat_orders).
//
// World thread only.
// --------------------------------------------------------------------------

// A real player's line in /say, or whispered to a bot. True when it asked for
// a buff, or thanked a bot for one, and was answered here (with words, a
// buff, or a bot letting it go by): the caller then sends no other replies to
// it. `seq` is the line's transcript number, 0 if it is not recorded yet; it
// is recorded here when this answers it, and `seq` set.
bool Buff_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                          Player* receiver, const std::string& scopeKey, uint64_t& seq);

// World tick: cast the buffs that are due, and forget old asks and thanks.
void Buff_Update();

struct BuffStats
{
    uint64_t asked   = 0;   // asks bots heard
    uint64_t given   = 0;   // buffs cast
    uint64_t quiet   = 0;   // bots that buffed without a word
    uint64_t refused = 0;   // said no
    uint64_t cant    = 0;   // said why they could not
    uint64_t ignored = 0;   // let it go by
    uint64_t failed  = 0;   // a cast that did not go off after all
    uint64_t thanked = 0;   // thank-yous answered
    uint32_t pending = 0;
};
BuffStats Buff_GetStats();

#endif // MOD_OLLAMA_CHAT_BUFFS_H
