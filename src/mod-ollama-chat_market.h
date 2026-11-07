#ifndef MOD_OLLAMA_CHAT_MARKET_H
#define MOD_OLLAMA_CHAT_MARKET_H

#include <cstdint>
#include <string>

class ObjectGuid;
class Player;

// --------------------------------------------------------------------------
// The bots' auction house trading as something to talk about.
//
// mod-playerbots-plus, when it is in the same build, has random bots selling
// and buying on the auction house; it says so as it happens. A bot that just
// sold something, bought something, put something up or got it back unsold
// may bring it up in General for a while after (Market.TalkMinutes), the way
// players do -- through the Activity topics (EnvCommentMarket*).
//
// Without mod-playerbots-plus this is all inert. World thread only.
// --------------------------------------------------------------------------

void OllamaMarket_Init();          // startup
bool OllamaMarket_Available();     // mod-playerbots-plus is in this build

struct OllamaMarketNote
{
    uint8_t     type  = 0;   // 0 listed, 1 sold, 2 back unsold, 3 bought
    std::string item;        // "[Linen Cloth]", "20 [Linen Cloth]"
    std::string price;       // "1g 20s"
    std::string other;       // who bought it / who sold it, if known
};

// The newest dealing of this bot not talked about yet, within TalkMinutes.
bool OllamaMarket_Peek(Player* bot, OllamaMarketNote& out);

// A "market_*" topic went out for this bot: that dealing has been talked about.
void OllamaMarket_NoteTalked(ObjectGuid botGuid);

void OllamaMarket_Update();        // maintenance: forget old dealings
uint64_t OllamaMarket_Heard();     // dealings heard of since startup

#endif // MOD_OLLAMA_CHAT_MARKET_H
