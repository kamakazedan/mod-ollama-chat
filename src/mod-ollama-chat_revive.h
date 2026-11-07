#ifndef MOD_OLLAMA_CHAT_REVIVE_H
#define MOD_OLLAMA_CHAT_REVIVE_H

class Player;

// --------------------------------------------------------------------------
// Who can bring the dead back, for orders ("rez him") and for what a group
// says after a wipe. Getting dead bots back up by themselves is
// mod-playerbots-plus's (Revive.* there). World thread only.
// --------------------------------------------------------------------------

// A mod-dungeon-clear run owns this bot's group, so the run decides how its
// dead come back: nothing else releases them.
bool Revive_RunDecides(Player* bot);

// Does this character know a resurrection spell (priest, paladin, shaman or
// druid, once learned)? Looked up once a minute per character.
bool Revive_KnowsResurrect(Player* player);

#endif // MOD_OLLAMA_CHAT_REVIVE_H
