#ifndef MOD_OLLAMA_CHAT_ARRIVE_H
#define MOD_OLLAMA_CHAT_ARRIVE_H

#include "ScriptMgr.h"

#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Getting somewhere (OllamaChat.Arrive.*): a word on arriving.
//
// A group rides out of Westfall into Duskwood and somebody says something --
// "ugh, this place", "so what's the plan", "bit high for me this". A bot
// crossed into a new zone beside the person it was grouped with and said
// nothing, every time.
//
// So when someone playing crosses into another zone out in the world with
// bots in their group, one of the bots by them may say a word about getting
// there, in the group's chat (Arrive.Chance), once the group has had a
// moment to get there too (Arrive.DelaySeconds). It is told only what is so
// of the place: its name, the levels most of its quests are for, the
// dungeons whose way in is there, and the weather it has ridden into. Not
// whether it has been there before: it does not know.
//
// Not for a zone they were in within Arrive.ZoneHours (a run along a border
// is not a journey), not more than once in Arrive.MinMinutes for the same
// person, not into a city, a dungeon or a battleground, and not while they
// are flying, on a boat or in a fight.
//
// The zone hook runs on a map thread: it only notes who and where. The rest
// is on the world tick.
// --------------------------------------------------------------------------

class OllamaChatArriveScript : public PlayerScript
{
public:
    OllamaChatArriveScript() : PlayerScript("OllamaChatArriveScript", { PLAYERHOOK_ON_UPDATE_ZONE }) {}
    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override;
};

void Arrive_LoadConfig();   // startup and .ollama reload
void Arrive_Update();       // world tick

struct ArriveStats
{
    uint64_t crossed = 0;   // zone changes of someone playing with bots in their group
    uint64_t said    = 0;   // ...remarked on
    uint64_t again   = 0;   // ...not: a zone they had just been in, or one said too lately
    uint64_t passed  = 0;   // ...not: the chance said no, or nobody of the group was by them
    uint32_t pending = 0;
};
ArriveStats Arrive_GetStats();

#endif // MOD_OLLAMA_CHAT_ARRIVE_H
