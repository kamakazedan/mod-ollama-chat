#ifndef MOD_OLLAMA_CHAT_PLUS_H
#define MOD_OLLAMA_CHAT_PLUS_H

// --------------------------------------------------------------------------
// mod-playerbots-plus, when it is part of the same build.
//
// What bots do without a language model lives there: hanging out in cities,
// getting unstuck, getting back up, raid marks, muting playerbots' stock
// lines, trading on the auction house. This module only has them talk about
// it. Modules are compiled into one worldserver with each other's src/ on the
// include path, so it is found without depending on it; without it, all of
// that is simply not there, and nothing here needs it.
//
// OLLAMA_CHAT_PLAYERBOTS_PLUS is its API version: 0 when it is not in the
// build, 1 for the market only, 2 for hangouts, bot events and muting, 3 for
// duels talked into as well.
// --------------------------------------------------------------------------

#if __has_include("PlayerbotsPlus.h")
#include "PlayerbotsPlus.h"
#endif

#if defined(PLAYERBOTS_PLUS_API_VERSION)
#define OLLAMA_CHAT_PLAYERBOTS_PLUS PLAYERBOTS_PLUS_API_VERSION
#else
#define OLLAMA_CHAT_PLAYERBOTS_PLUS 0
#endif

#endif // MOD_OLLAMA_CHAT_PLUS_H
