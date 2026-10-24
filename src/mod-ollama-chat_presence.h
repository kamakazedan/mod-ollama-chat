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
// "Kamidan, a level 23 Human Warrior, riding a Brown Horse, with their Cat
// Whiskers". `observer` formats race and class names. *outPetGuid gets the
// subject's pet or companion when one is close to them.
std::string Presence_Describe(Player* subject, Player* observer, ObjectGuid* outPetGuid = nullptr);

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
