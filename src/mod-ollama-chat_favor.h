#ifndef MOD_OLLAMA_CHAT_FAVOR_H
#define MOD_OLLAMA_CHAT_FAVOR_H

#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Favours for people outside a bot's group.
//
// A buff, conjured food or water, something spare from its bags, a deal for
// something they are selling: asked by someone it does not serve -- not its
// master, not in its group -- a bot takes it its own way, by its chat style
// (OllamaChat.Buff.Style.<KEY>): does it and says so, does it without a word,
// says no, or lets it go by. Asked in particular and unable, it may say why.
//
// Playerbots turns down any trade with a person outside its group ("I'm
// kind of busy now") when its AI next ticks, from the trade packets it was
// sent. While a bot trades with someone as a favour, its AI is held from the
// world tick, so it never gets to: the trade is this module's to finish, its
// side accepted for it, and the queued packets find no trade when the AI
// comes back. Group members trade the way they always have, playerbots
// pricing and accepting (OllamaChat.Favor.Strangers turns all of this off,
// and strangers are told to invite the bot first again).
//
// World thread only.
// --------------------------------------------------------------------------

enum class FavorOutcome : uint8_t
{
    Give,     // do it, and say so
    Quiet,    // do it without a word
    No,       // say no
    Ignore    // let it go by
};

// How `bot` takes an ask. `spokenTo` (named, whispered, targeted): letting it
// go by is half as likely.
FavorOutcome Favor_Roll(Player* bot, bool spokenTo);

// Asked in particular and unable: does it say why?
bool Favor_Explains(Player* bot);

// Thanked by the one it did it for: does it answer?
bool Favor_Welcomes(Player* bot);

// Neither its master nor in its group: someone playerbots will not trade with.
bool Favor_IsStranger(Player* bot, Player* player);

// Up to doing anyone a favour right now: about, alive, not fighting or
// travelling, and not AFK -- unless it is AFK hanging out in a city and is
// asked in particular (`spokenTo`): it comes back to the keyboard for that.
bool Favor_CanAnswer(Player* bot, bool spokenTo = false);

// Doing it: back at the keyboard, if it was AFK hanging out in a city.
void Favor_ComeBack(Player* bot);

// Do bots trade with strangers at all (OllamaChat.Favor.Strangers)?
bool Favor_TradesWithStrangers();

// Hold `bot`'s AI for a moment. Called every world tick while it trades with
// a stranger, so playerbots never sees the trade. World thread.
void Favor_HoldAi(Player* bot);

#endif // MOD_OLLAMA_CHAT_FAVOR_H
