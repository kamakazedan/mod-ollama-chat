#ifndef MOD_OLLAMA_CHAT_CORPSE_H
#define MOD_OLLAMA_CHAT_CORPSE_H

#include <cstdint>

class Creature;
class Player;

// --------------------------------------------------------------------------
// A last word over the body, without a word: a bot that has just killed
// something worth it emotes at the body -- spits on it, laughs at it, salutes
// it, mourns it -- the way its chat style would (OllamaChat.Corpse.*).
//
// Worth it: a named enemy (a rare, or one of a kind: a single spawn in the
// world, like a quest's named target), an elite out in the world (in a
// dungeon or raid they are every pull), or a boss. Never anything grey to the
// one who killed it, nobody's pet or totem, and nothing in a battleground.
//
// Who: the bot that landed the killing blow (or whose pet did); when a real
// player did, one of that player's group's bots near the body. One roll per
// kill, at that bot's style's share of the kind's chance
// (Corpse.Style.<KEY>), and no bot does it twice in Corpse.CooldownSeconds.
// A moment later, out of the fight and near the body, it turns to it and
// plays the emote -- only where a real player is near enough to see it. What
// it did goes into the transcript of those about ("Lianon gave
// Gath'Ilzogg's body a /spit"), so a bot asked about it knows.
//
// Corpse_NoteKill runs in the kill hooks, on the map thread: it reads the
// killer and the creature it killed, and notes the kill under a mutex.
// Corpse_Update, on the world tick, decides and plays.
// --------------------------------------------------------------------------

void Corpse_NoteKill(Player* killer, Creature* victim);
void Corpse_Update();

struct CorpseStats
{
    uint64_t kills  = 0;   // kills worth it, with a real player near enough to see
    uint64_t rolled = 0;   // a bot rolled for one
    uint64_t emoted = 0;   // played at the body
    uint64_t unseen = 0;   // nobody real near enough to see it
    uint64_t lapsed = 0;   // still fighting, too far, or the body gone, until it was too late
    uint64_t named  = 0;   // of the kills: named (a rare, or one of a kind)
    uint64_t elites = 0;   //               elites out in the world
    uint64_t bosses = 0;   //               bosses
};
CorpseStats Corpse_GetStats();

#endif // MOD_OLLAMA_CHAT_CORPSE_H
