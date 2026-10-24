#ifndef MOD_OLLAMA_CHAT_CONVLOGIC_H
#define MOD_OLLAMA_CHAT_CONVLOGIC_H

#include <cstdint>
#include <random>
#include <vector>

// --------------------------------------------------------------------------
// The decisions a running conversation makes, kept free of game types so they
// can be unit tested: who speaks next, and how long everyone waits first.
// mod-ollama-chat_conversation.cpp does the game side.
// --------------------------------------------------------------------------

namespace ConvLogic
{
    struct Seat
    {
        uint64_t guid   = 0;
        uint32_t lines  = 0;     // lines said in this conversation
        uint32_t misses = 0;     // turns in a row that produced nothing
        int64_t  lastMs = -1;    // when it last spoke (ms, any clock), -1 = not yet
    };

    struct PickInput
    {
        std::vector<Seat> seats;
        uint64_t lastSpeaker    = 0;     // whoever said the newest line (bot or person)
        uint64_t beforeLast     = 0;     // the speaker before that
        uint64_t named          = 0;     // a seat the newest line names, 0 = none
        bool     lastWasHuman   = false;
        uint64_t humanAddressee = 0;     // the seat a person's newest line answers, 0 = none
        bool     canJoin        = false; // someone new could join and there is room
        uint32_t joinChancePct  = 0;
        uint32_t maxMisses      = 2;     // a seat that missed this many turns in a row sits out
        int64_t  nowMs          = 0;
    };

    struct PickResult
    {
        bool     join = false;   // bring someone new in (the caller picks who)
        uint64_t guid = 0;       // the seat to speak; 0 with join == false: nobody
    };

    // Who talks next:
    //  * someone the newest line names, or the bot a person just answered,
    //    almost always gets to answer;
    //  * otherwise, now and then, somebody new pipes up;
    //  * otherwise one of the others, never the one who just spoke, leaning
    //    toward the one who spoke before that -- conversations go back and
    //    forth -- and toward whoever has been quiet longest.
    PickResult PickNextSpeaker(const PickInput& in, std::mt19937& rng);

    // The pause between a line appearing and the next person starting to type.
    // It stretches as the conversation gets older (progress 0..1), and now and
    // then there is a lull.
    uint32_t NextGapMs(uint32_t minMs, uint32_t maxMs, double progress,
                       uint32_t lullChancePct, uint32_t lullMinMs, uint32_t lullMaxMs,
                       std::mt19937& rng);
}

#endif // MOD_OLLAMA_CHAT_CONVLOGIC_H
