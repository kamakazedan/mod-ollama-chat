#ifndef MOD_OLLAMA_CHAT_ORDERS_H
#define MOD_OLLAMA_CHAT_ORDERS_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Orders in plain words.
//
// A real player's party, raid or whispered line that tells the bots to do
// something -- "stay back", "wait here", "everyone come with me", "Joanni come
// trade me", "you're stuck, use unstuck", "res" -- is carried out with the
// matching playerbots command, and one bot says it is on it in its own voice
// ("ok, shout when you want us following again") instead of a stock
// "Staying". TextRules::MatchBotCommand decides what is an order.
//
// Who it is for: the bots it names, else the bot it was whispered to, else
// every bot in the group that takes orders from that player. An order that
// only makes sense for one bot (trade, unstuck, leave) needs that bot named,
// whispered, or plainly the one the player was just talking to.
//
// With Playerbots.CommandPrefix empty, playerbots reads every chat line as a
// command as well ("stay here" is its "stay"). An order it is carrying out
// already is only answered here, never sent twice; one it read differently
// ("stay close" is its "stay") is put right a tick later, after its own. A
// line starting with the prefix is a playerbots command: bots do not chat
// about it.
//
// Nothing here summons a bot. "Unstuck" does what the stuck tool does for a
// player: inside a dungeon back to its entrance, outside to the nearest
// graveyard.
//
// World thread only.
// --------------------------------------------------------------------------

// A real player's party, raid or whisper line. True when it was an order (or
// a prefixed playerbots command) and has been dealt with: no other replies.
// `seq` is the line's transcript number, 0 when it has not been recorded yet;
// an order records it.
bool Orders_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                            Player* receiver, const std::string& scopeKey, uint64_t& seq);

// The world tick: carries out orders a tick after they were given, walks bots
// over to trade, and lets bots that ran from a fight fight again once it is
// over.
void Orders_Update(uint32_t diff);

// A dungeon-clear run in this player's group just started or carried on
// (mod-ollama-chat_partycmd): bots here told to stay follow again. Playerbots'
// stay walks a bot back to where it was told to wait, which would hold it
// against the tank leading the run. `held`, when given, names the bots the run
// holds with a stay of its own, which are left to it.
void Orders_ReleaseStays(Player* player, bool (*held)(Player* bot) = nullptr);

// For a bot's prompt: what it was told to do and is still doing ("you are
// waiting where you were told to"), or empty. OllamaChat.State.Staying.
std::string Orders_StateTextFor(Player* bot);

struct OrdersStats
{
    uint64_t orders  = 0;   // lines that were orders
    uint64_t sent    = 0;   // commands sent to bots
    uint64_t already = 0;   // playerbots was carrying it out already
    uint64_t redone  = 0;   // sent again: playerbots' own reading of the line came after ours
    uint64_t refused = 0;   // could not: dead, nothing targeted, no hearthstone, ...
    uint64_t trades  = 0;   // trade windows offered
    uint64_t unstuck = 0;
    uint64_t raw     = 0;   // prefixed playerbots commands left alone
};
OrdersStats Orders_GetStats();

#endif // MOD_OLLAMA_CHAT_ORDERS_H
