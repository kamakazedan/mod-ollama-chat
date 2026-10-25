#ifndef MOD_OLLAMA_CHAT_CONJURE_H
#define MOD_OLLAMA_CHAT_CONJURE_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal, ScriptMgr.h

#include <cstdint>
#include <string>

class Player;
class WorldPacket;
class WorldSession;

// --------------------------------------------------------------------------
// Conjured food and water, on request.
//
// A real player in a group with a mage bot asks for water the way people do --
// "water pls", "mage can i get food", "anyone got water?", "Frostiq please can
// you give me mana pots" -- and the mage conjures it and opens a trade with
// them, for free. When it can't, it says why: it is not a mage, it has not
// learned to conjure that yet, somebody's bags are full. When it only can't
// yet -- in a fight, out of mana, too far away to trade -- it says it will,
// and does as soon as it can.
//
// The mage's side of the trade is accepted before the person's, so the
// person's accept completes it at once and playerbots' own pricing ("I want
// 20c for this") never runs. The answer in chat comes from the model, in the
// bot's own voice, steered by OllamaChat.Conjure.Hint*.
//
// Requests only count from someone in the same group as the mage: playerbots
// cancels trades with anyone else. A mage that is named or whispered by
// someone outside its group says to invite it.
//
// World thread only, except the packet hook, which only records whose trade
// window has opened.
// --------------------------------------------------------------------------

// A real player's line. True when it asked a mage for conjured food or water
// and the mage is answering: the caller then sends no other replies to it.
// `seq` is the line's transcript number if it has been recorded already, 0 if
// not (it is recorded here then).
bool Conjure_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                             Player* receiver, const std::string& scopeKey, uint64_t seq);

// World tick: start the gifts that were waiting, fill trade windows that have
// opened, keep the mage's side accepted, and let stale offers go. A gift whose
// giver or receiver has logged out is dropped here too -- there is no logout
// hook, since bots log out on map threads.
void Conjure_Update();

struct ConjureStats
{
    uint64_t asked     = 0;   // asks a bot answered
    uint64_t offered   = 0;   // trades opened, or filled when they opened one
    uint64_t completed = 0;   // the food or water changed hands
    uint64_t waited    = 0;   // had to wait for a fight, mana or range first
    uint64_t refused   = 0;   // could not, and said why
    uint64_t lapsed    = 0;   // declined, cancelled, or not taken in time
    uint32_t pending   = 0;
};
ConjureStats Conjure_GetStats();

// Notices when a trade window opens for someone a mage is giving food or
// water to -- the one the mage offered, or one they opened with the mage
// themselves -- so it can go in at once. The server keeps no "window open"
// state; the only sign is the OPEN_WINDOW status sent to their client.
// Registered from mod-ollama-chat_main.
class OllamaChatConjureScript : public ServerScript
{
public:
    OllamaChatConjureScript() : ServerScript("OllamaChatConjureScript", { SERVERHOOK_CAN_PACKET_SEND }) {}
    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override;
};

#endif // MOD_OLLAMA_CHAT_CONJURE_H
