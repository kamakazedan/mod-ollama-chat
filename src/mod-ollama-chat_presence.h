#ifndef MOD_OLLAMA_CHAT_PRESENCE_H
#define MOD_OLLAMA_CHAT_PRESENCE_H

#include "ObjectGuid.h"

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Presence: bots noticing the real players around them.
//
// A bot walking past a real player now and then does what that bot's
// personality would: a friendly one waves, a pet lover pets their cat, a
// prankster rasps at them and keeps walking. Sometimes it says something
// about them in /say as well -- their mount, their pet, what they are up to.
//
// Paced so it stays an occasional moment rather than a parade: one moment per
// real player every PlayerIntervalMin..Max seconds at most, a long cooldown
// per bot, and a longer one per bot and player pair. Never in instances, never
// in combat, never from the player's own party.
//
// World thread only.
// --------------------------------------------------------------------------

// What a passer-by can see about `subject`, as a phrase for a prompt:
// "Kamidan, a level 23 Human Hunter, riding their mount, a Brown Horse, with
// their hunter pet, a cat called Whiskers" -- each animal as what it is
// (mod-ollama-chat_pets). `observer` formats race and class names, and in a
// city (OllamaInCity) the level is left out: "Kamidan, a Human Hunter, ...".
// *outPetGuid gets the subject's pet or companion when one is close to them.
// Without `withMount`, a rider is only "passing by": two bots chatting are
// not handed a passer-by's mount, or one talks it up and the other answers
// for it (mod-ollama-chat_mounts).
std::string Presence_Describe(Player* subject, Player* observer, ObjectGuid* outPetGuid = nullptr,
                              bool withMount = true);

void Presence_Update(uint32_t diff);           // world tick; forgets logged-out characters by itself
void Presence_Reset();

struct PresenceStats
{
    uint64_t emotes    = 0;   // gestures at a real player or their pet
    uint64_t petEmotes = 0;   // ...of which at the pet
    uint64_t remarks   = 0;   // /say lines asked for
};
PresenceStats Presence_GetStats();

#endif // MOD_OLLAMA_CHAT_PRESENCE_H
