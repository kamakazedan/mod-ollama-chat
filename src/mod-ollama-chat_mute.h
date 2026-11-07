#ifndef MOD_OLLAMA_CHAT_MUTE_H
#define MOD_OLLAMA_CHAT_MUTE_H

#include "mod-ollama-chat_plus.h"

// --------------------------------------------------------------------------
// mod-playerbots-plus mutes stock lines playerbots whispers that nobody needs
// to see -- "Using [Hearthstone]", "AI was reset to defaults" -- and, on a
// realm with this module, the "Staying", "Following" and "Hello!" a bot now
// says in its own words instead (PlayerbotsPlus.Mute.Patterns).
//
// While one of these is alive on a thread, the lines that thread sends are
// this module's own -- what the model (or a canned "gz") says for a bot --
// and are never muted, even when one happens to read "Hi" or "Following".
// Delivery holds one around sending. Without mod-playerbots-plus nothing is
// muted, and this does nothing.
// --------------------------------------------------------------------------

struct MuteOwnLine
{
    MuteOwnLine() {}
    MuteOwnLine(const MuteOwnLine&) = delete;
    MuteOwnLine& operator=(const MuteOwnLine&) = delete;

#if OLLAMA_CHAT_PLAYERBOTS_PLUS >= 2
    PlayerbotsPlus::OwnLine line;
#endif
};

#endif // MOD_OLLAMA_CHAT_MUTE_H
