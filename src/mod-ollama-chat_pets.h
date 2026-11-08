#ifndef MOD_OLLAMA_CHAT_PETS_H
#define MOD_OLLAMA_CHAT_PETS_H

#include <string>

class Player;

// --------------------------------------------------------------------------
// A bot's own pet, and the companion following it, when the talk is about
// them.
//
// A hunter's pet has a name (mod-playerbots-plus gives them the kind players
// do), and a bot may have a companion trotting along. "cute cat!", "whats
// your wolf called?", "is that a snake following you?": the bot that is asked
// knows what its pet is and is called, and what is following it
// (OllamaChat.Pets.StateText). Only when the line it answers is about them
// (Pets.Words, the pet's or companion's own name, or its kind): a pet in every
// prompt is what a bot would end up talking about.
//
// World thread.
// --------------------------------------------------------------------------

std::string Pets_StateTextFor(Player* bot, const std::string& message);

#endif // MOD_OLLAMA_CHAT_PETS_H
