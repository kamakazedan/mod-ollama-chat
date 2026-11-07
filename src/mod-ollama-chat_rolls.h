#ifndef MOD_OLLAMA_CHAT_ROLLS_H
#define MOD_OLLAMA_CHAT_ROLLS_H

#include "ScriptMgr.h"

#include <cstdint>

class WorldPacket;
class WorldSession;

// --------------------------------------------------------------------------
// Loot rolls the bots can see.
//
// A player lost a greed roll with a 1, typed "1 lol", and got "don't spend it
// all at once": the rolls only ever reach the players' chat frames, never what
// the bots are told. Now each loot roll in a group with a real player goes
// into the group's chat transcript once it is decided -- "loot roll for
// Twisted Sabre (1 to 100, highest wins; need beats greed): Ynos 93 (greed),
// Kami 1 (greed) -- Ynos won it" (Rolls.NoteText) -- so a reply knows what
// "1 lol" was about. And when a real player rolls about as low or as high as
// it gets (Rolls.LowRoll, Rolls.HighRoll), now and then (Rolls.ReactChance)
// a bot in the group says something about it (Rolls.LowPrompt,
// Rolls.HighPrompt): a 1 is as unlucky as it gets, a 99 as lucky.
//
// The numbers come off the roll packets the game sends the group
// (SMSG_LOOT_ROLL, SMSG_LOOT_ROLL_WON). The packet hook runs on whatever
// thread sends them and only queues what it read; the rest happens in the
// world tick.
// --------------------------------------------------------------------------

class OllamaChatRollsScript : public ServerScript
{
public:
    OllamaChatRollsScript() : ServerScript("OllamaChatRollsScript", { SERVERHOOK_CAN_PACKET_SEND }) {}
    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override;
};

// World tick: rolls decided since the last one go into their group's
// transcript, and the odd one gets a reaction.
void Rolls_Update();

struct RollsStats
{
    uint64_t noted     = 0;   // rolls put in a group's transcript
    uint64_t reactions = 0;   // a bot said something about a very low or high roll
};
RollsStats Rolls_GetStats();

#endif // MOD_OLLAMA_CHAT_ROLLS_H
