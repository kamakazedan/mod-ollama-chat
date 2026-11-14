#ifndef MOD_OLLAMA_CHAT_CITYLIFE_H
#define MOD_OLLAMA_CHAT_CITYLIFE_H

// --------------------------------------------------------------------------
// City life, said in /say (OllamaChat.CityLife.*).
//
// What the crowd hanging out in a city does is mod-playerbots-plus's
// (PlayerbotsPlus.CityLife.* there): shifting about, looking round at
// whoever speaks, hopping, dancing, errands, fishing, a few wandering over to
// look at a rare mount. This says the two of those that are said:
//
//  - someone gets on a mount few people about have, and a few of those
//    hanging out nearby come over to look: the first of them says something
//    to the rider about it in /say, naming the mount it really is, and told
//    how rare it really is (plus counts who has it, of everyone online)
//  - a bot starts dancing, or joins someone dancing: the text emote with it
//    ("X bursts into dance.", "X dances with Y."). The dance itself is plus's,
//    and so is its end: the emote leaves the dance on (BotPlayTextEmote's
//    stateKept), or it would stop after StateEmoteDurationMs.
//
// The remark is a line nobody asked for: it is not started within
// Context.AmbientHoldSeconds of a real player speaking nearby, and is dropped
// if one spoke while it was written. Across a language barrier (the other
// side, in a neutral city) nothing is said: it would arrive as gibberish.
//
// The other way round, this module tells plus when a bot is talking or
// emoting, when someone says something nearby, and when a /say exchange
// starts (mod-ollama-chat_hangout: Hangout_Busy, Hangout_Spoke,
// Hangout_Gather), so nobody walks off in the middle of a sentence and those
// talking stand together.
//
// Without mod-playerbots-plus (its API 5), none of this is there.
// World thread only.
// --------------------------------------------------------------------------

#include <cstdint>

void CityLife_Init();

struct CityLifeStats
{
    uint64_t mounts = 0;   // rare mounts looked at, from mod-playerbots-plus
    uint64_t said   = 0;   // ...remarked on, sent to be written
    uint64_t quiet  = 0;   // ...not: off, a player had just spoken, a language barrier, pacing or no room
    uint64_t dances = 0;   // dances started or joined, from mod-playerbots-plus
    uint64_t emoted = 0;   // ...shown with the dance text emote
};
CityLifeStats CityLife_GetStats();

#endif // MOD_OLLAMA_CHAT_CITYLIFE_H
