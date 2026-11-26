#ifndef MOD_OLLAMA_CHAT_GOODTURN_H
#define MOD_OLLAMA_CHAT_GOODTURN_H

#include "ScriptMgr.h"

#include <cstdint>

class Player;
class Spell;

// --------------------------------------------------------------------------
// One good turn: a bot thanks whoever buffed, healed or raised it.
//
// A real player puts Fortitude on someone running past, and they get a
// /thank, a "ty", often a buff back. A bot said nothing and ran on.
//
// So when a real player casts a buff that lasts (GoodTurn.BuffMinMinutes) on
// a bot, heals one that was hurt (GoodTurn.HealPct) or resurrects one, the
// bot may acknowledge it (GoodTurn.Chance; GoodTurn.InGroupChance for one in
// their own group, where buffs are routine): it turns to them and thanks them
// with the emote, sometimes with a word as well (GoodTurn.SayChance: a typed
// line in its own chat style, in /say, or the group's chat for someone in its
// group), and -- when it has one of its own they are without -- with a buff
// back (GoodTurn.BuffBackChance: its own cast, mod-ollama-chat_buffs). Once
// for the same player in GoodTurn.PairMinutes.
//
// What counts is what the game says: a helpful spell cast on the bot by
// name, with an aura that lasts, an effect that heals or one that
// resurrects. A buff that lands on a whole group at once is nobody's in
// particular, and is not thanked.
//
// The cast hook runs on a map thread: it only notes who, on whom and what.
// The rest is on the world tick.
// --------------------------------------------------------------------------

class OllamaChatGoodTurnScript : public PlayerScript
{
public:
    OllamaChatGoodTurnScript() : PlayerScript("OllamaChatGoodTurnScript", { PLAYERHOOK_ON_SPELL_CAST }) {}
    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override;
};

void GoodTurn_Update();   // world tick

struct GoodTurnStats
{
    uint64_t noted   = 0;   // buffs, heals and resurrections a real player cast on a bot
    uint64_t thanked = 0;   // ...thanked with the emote
    uint64_t said    = 0;   // ...with a word as well
    uint64_t buffed  = 0;   // ...with a buff back
    uint64_t again   = 0;   // ...not acknowledged: the same player's, a moment ago
    uint64_t missed  = 0;   // ...not acknowledged: gone, fighting or dead by then
    uint32_t pending = 0;
};
GoodTurnStats GoodTurn_GetStats();

#endif // MOD_OLLAMA_CHAT_GOODTURN_H
