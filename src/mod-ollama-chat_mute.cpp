#include "mod-ollama-chat_mute.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat_world.h"

#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace
{
    // What the packet hook reads. Replaced whole, never changed in place.
    struct Snapshot
    {
        bool                         enable = false;
        std::vector<std::string>     patterns;
        std::unordered_set<uint64_t> bots;
    };

    std::mutex                      g_mutex;
    std::shared_ptr<const Snapshot> g_snapshot = std::make_shared<Snapshot>();
    std::atomic<bool>               g_enabled{ false };   // fast path: most packets are not chat at all
    std::atomic<uint64_t>           g_dropped{ 0 };

    // Kept on the world thread, published with the bots.
    bool                     g_enable = false;
    std::vector<std::string> g_patterns;

    std::shared_ptr<const Snapshot> Current()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_snapshot;
    }

    void Publish(std::unordered_set<uint64_t> bots)
    {
        auto next = std::make_shared<Snapshot>();
        next->enable   = g_enable && !g_patterns.empty();
        next->patterns = g_patterns;
        next->bots     = std::move(bots);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snapshot = std::move(next);
        }
        g_enabled.store(g_enable && !g_patterns.empty(), std::memory_order_relaxed);
    }

    std::unordered_set<uint64_t> OnlineBots()
    {
        std::unordered_set<uint64_t> bots;
        for (auto const& entry : ObjectAccessor::GetPlayers())
            if (Player* p = entry.second)
                if (OllamaIsBotPlayer(p))
                    bots.insert(p->GetGUID().GetRawValue());
        return bots;
    }
}

void Mute_Publish(bool enable, std::vector<std::string> patterns)
{
    g_enable = enable;
    g_patterns.clear();
    for (std::string& p : patterns)
    {
        const size_t b = p.find_first_not_of(" \t");
        if (b == std::string::npos)
            continue;
        const size_t e = p.find_last_not_of(" \t");
        g_patterns.push_back(p.substr(b, e - b + 1));
    }
    Publish(OnlineBots());
    LOG_INFO("module.ollamachat", "[Ollama Chat] Muted bot lines: {} ({} pattern(s)).",
             g_enable ? "on" : "off", g_patterns.size());
}

void Mute_Update(uint32_t /*diff*/)
{
    if (!g_enable)
        return;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next{};
    const Clock::time_point now = Clock::now();
    if (now < next)
        return;
    next = now + std::chrono::seconds(2);
    Publish(OnlineBots());
}

uint64_t Mute_GetDropped()
{
    return g_dropped.load(std::memory_order_relaxed);
}

bool OllamaChatMuteScript::CanPacketSend(WorldSession* session, WorldPacket const& packet)
{
    // Every packet to every client comes through here, from any thread: say
    // yes to all of them, and look no further than the opcode unless muting
    // is on.
    if (packet.GetOpcode() != SMSG_MESSAGECHAT || !g_enabled.load(std::memory_order_relaxed) || !session)
        return true;

    // type u8, language i32, sender u64, flags u32, receiver u64, length u32,
    // text: the layout ChatHandler::BuildChatPacket writes for these types.
    constexpr size_t kTextAt = 1 + 4 + 8 + 4 + 8 + 4;
    const size_t size = packet.size();
    if (size < kTextAt + 1)
        return true;

    const uint8 type = packet.read<uint8>(0);
    if (type != CHAT_MSG_WHISPER && type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER &&
        type != CHAT_MSG_RAID && type != CHAT_MSG_RAID_LEADER)
        return true;
    if (packet.read<uint32>(1) == uint32(LANG_ADDON))
        return true;

    const uint32 length = packet.read<uint32>(kTextAt - 4);   // includes the closing NUL
    if (length < 2 || kTextAt + length > size)
        return true;

    const std::shared_ptr<const Snapshot> snap = Current();
    if (!snap->enable || !snap->bots.count(packet.read<uint64>(5)))
        return true;

    const char* text = reinterpret_cast<const char*>(packet.contents() + kTextAt);
    const std::string line(text, strnlen(text, length - 1));
    for (const std::string& pattern : snap->patterns)
    {
        if (TextRules::GlobMatch(pattern, line))
        {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}
