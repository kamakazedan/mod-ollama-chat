#ifndef MOD_OLLAMA_CHAT_STAY_H
#define MOD_OLLAMA_CHAT_STAY_H

#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Spoken to, a bot stays to answer.
//
// A real player says something in /say to a bot going about its business.
// The answer takes a few seconds to write, and by then the bot had walked
// on: out of earshot the line was dropped as it went out and they got
// nothing, or it came from someone running the other way.
//
// So a bot picked to answer a real player face to face stops and turns to
// them, as anyone spoken to does, waits for its own line (Stay.MaxSeconds at
// most), and stays a moment after it (Stay.LingerSeconds) in case they say
// more -- their next line starts the wait again -- before it goes on with
// what it was doing. The same for one waved at that answers in words.
//
// Only a bot on its own: one in a group goes where its group goes, and one
// that is someone's own does as it is told. A fight ends the wait at once.
// Its AI is held the way a favour's trade holds it (PlayerbotAI::YieldThread,
// every world tick), and city life is told it is busy (Hangout_Busy), so
// nothing walks it off.
//
// Stay_Begin only notes who, under a mutex: a line may be sent to be written
// from a map thread (someone's emote, answered in words). The stopping, and
// everything else here, is the world tick's.
// --------------------------------------------------------------------------

// `bot` is about to answer `to`, a real player near it, in /say: from the
// dispatcher, as the line is sent to be written. Any thread.
void Stay_Begin(uint64_t bot, uint64_t to);

// Its line in /say is out: it stays a moment more. World thread.
void Stay_Spoke(Player* bot);

void Stay_Update();   // world tick

struct StayStats
{
    uint64_t stayed = 0;   // bots that stopped to answer
    uint64_t spoke  = 0;   // ...whose line then went out while they stood there
    uint64_t fights = 0;   // ...let go for a fight
    uint64_t gaveUp = 0;   // ...whose line never came
    uint32_t now    = 0;   // standing there now
};
StayStats Stay_GetStats();

#endif // MOD_OLLAMA_CHAT_STAY_H
