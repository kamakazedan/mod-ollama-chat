#ifndef MOD_OLLAMA_CHAT_MUTE_H
#define MOD_OLLAMA_CHAT_MUTE_H

#include "ScriptMgr.h"

#include <cstdint>
#include <string>
#include <vector>

class WorldPacket;
class WorldSession;

// --------------------------------------------------------------------------
// Stock lines playerbots whispers that nobody needs to see: "Using
// [Hearthstone]", "AI was reset to defaults", "I don't know this dungeon, lead
// the way!", and the "Staying" and "Following" a bot now says in its own
// words instead (mod-ollama-chat_orders). OllamaChat.Mute.Patterns lists
// them. Only whispers and party or raid lines from a bot are looked at, and
// anything the module itself sends for a bot goes through (MuteOwnLine).
//
// The packet hook runs on whatever thread sends the packet, so it reads
// nothing of the world: only the published pattern list and the set of bot
// guids, both refreshed on the world thread.
// --------------------------------------------------------------------------

// Publish OllamaChat.Mute.* for the packet hook. World thread, at config load.
void Mute_Publish(bool enable, std::vector<std::string> patterns);

// World tick: refresh which characters online are bots.
void Mute_Update(uint32_t diff);

// Lines dropped so far.
uint64_t Mute_GetDropped();

// While one of these is alive on a thread, the lines that thread sends are
// the module's own -- what the model (or a canned "gz") says for a bot -- and
// are never muted, even when one happens to read "Hi" or "Following".
// Delivery holds one around sending.
struct MuteOwnLine
{
    MuteOwnLine();
    ~MuteOwnLine();
    MuteOwnLine(const MuteOwnLine&) = delete;
    MuteOwnLine& operator=(const MuteOwnLine&) = delete;
};

class OllamaChatMuteScript : public ServerScript
{
public:
    OllamaChatMuteScript() : ServerScript("OllamaChatMuteScript", { SERVERHOOK_CAN_PACKET_SEND }) {}
    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override;
};

#endif // MOD_OLLAMA_CHAT_MUTE_H
