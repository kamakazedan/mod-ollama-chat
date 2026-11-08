#ifndef MOD_OLLAMA_CHAT_PETS_H
#define MOD_OLLAMA_CHAT_PETS_H

#include "mod-ollama-chat_textrules.h"

#include "ObjectGuid.h"

#include <string>

class Player;

// --------------------------------------------------------------------------
// A bot's own mount, pet and companion, when the talk is about them -- and
// what anyone has with them, each as what it is.
//
// A hunter's pet has a name (mod-playerbots-plus gives them the kind players
// do), and a bot may have a companion trotting along or be riding its mount.
// "cute cat!", "whats your wolf called?", "nice mount": the bot that is asked
// knows what its pet is and is called, what is following it, and whether it
// is riding anything at all (OllamaChat.Pets.StateText). Only when the line
// it answers is about them (Pets.Words, Pets.MountWords, or the name or kind
// of one of them): a pet in every prompt is what a bot would end up talking
// about.
//
// A wolf at a hunter's side is its pet; a wolf someone sits on is a mount.
// Every prompt names each as what it is (Pets_Seen, TextRules::AnimalPhrases),
// or "nice mount" gets "wolf howler is a solid choice".
//
// World thread.
// --------------------------------------------------------------------------

struct PetsSeen
{
    TextRules::Animals animals;
    ObjectGuid         petGuid;         // their pet, when it is out and with them
    ObjectGuid         companionGuid;   // the companion following them
};

// What `who` is riding and has with them, within `range` of them.
PetsSeen Pets_Seen(Player* who, float range = 20.0f);

std::string Pets_StateTextFor(Player* bot, const std::string& message);

#endif // MOD_OLLAMA_CHAT_PETS_H
