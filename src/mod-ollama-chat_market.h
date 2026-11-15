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
// sold something, bought something or got it back unsold may bring it up in
// chat for a while after (Market.TalkMinutes), the way players do -- through
// the Market topics (EnvCommentMarket*). Something it just put up is still
// for sale, and selling is for Trade: that is only brought up there.
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
// `listings`: something it put up may be the one (a line in Trade); else only
// what is over -- sold, bought, or back unsold.
bool OllamaMarket_Peek(Player* bot, OllamaMarketNote& out, bool listings);

// A "market_*" topic went out for this bot: the newest dealing of that type
// (OllamaMarketNote::type) not talked about yet has been.
void OllamaMarket_NoteTalked(ObjectGuid botGuid, uint8_t type);

void OllamaMarket_Update();        // maintenance: forget old dealings
uint64_t OllamaMarket_Heard();     // dealings heard of since startup

#endif // MOD_OLLAMA_CHAT_MARKET_H
