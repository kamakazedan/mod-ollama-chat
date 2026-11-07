#include "mod-ollama-chat_market.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_plus.h"

#include "CharacterCache.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"

#include <fmt/core.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr size_t kKept = 3;   // dealings remembered per bot

    struct Dealing
    {
        OllamaMarketNote  note;
        Clock::time_point when{};
        bool              talked = false;
    };

    std::mutex                                    g_mutex;
    std::unordered_map<uint64_t, std::deque<Dealing>> g_dealings;   // by bot raw guid
    uint64_t                                      g_heard = 0;

    std::string Money(uint64_t copper)
    {
        const uint64_t gold = copper / 10000, silver = (copper / 100) % 100, rest = copper % 100;
        if (gold)
            return silver ? fmt::format("{}g {}s", gold, silver) : fmt::format("{}g", gold);
        if (silver)
            return rest ? fmt::format("{}s {}c", silver, rest) : fmt::format("{}s", silver);
        return fmt::format("{}c", rest);
    }

#if OLLAMA_CHAT_PLAYERBOTS_PLUS
    void Heard(PlayerbotsPlus::MarketEvent const& event)
    {
        if (!g_Enable || !g_MarketTalkMinutes)
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(event.itemId);
        if (!proto)
            return;

        Dealing d;
        d.when = Clock::now();
        switch (event.type)
        {
            case PlayerbotsPlus::MarketEventType::Listed:  d.note.type = 0; break;
            case PlayerbotsPlus::MarketEventType::Sold:    d.note.type = 1; break;
            case PlayerbotsPlus::MarketEventType::Expired: d.note.type = 2; break;
            case PlayerbotsPlus::MarketEventType::Bought:  d.note.type = 3; break;
        }
        d.note.item  = event.count > 1 ? fmt::format("{} [{}]", event.count, proto->Name1)
                                       : "[" + proto->Name1 + "]";
        d.note.price = Money(event.money);
        if (event.other)
            sCharacterCache->GetCharacterNameByGuid(event.other, d.note.other);

        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_heard;
        std::deque<Dealing>& list = g_dealings[event.bot.GetRawValue()];
        list.push_back(std::move(d));
        while (list.size() > kKept)
            list.pop_front();
    }
#endif
}

void OllamaMarket_Init()
{
#if OLLAMA_CHAT_PLAYERBOTS_PLUS
    PlayerbotsPlus::AddMarketListener(&Heard);
    LOG_INFO("server.loading", "[Ollama Chat] mod-playerbots-plus found: bots can talk about their auction house trading.");
#endif
}

bool OllamaMarket_Available()
{
    return OLLAMA_CHAT_PLAYERBOTS_PLUS != 0;
}

bool OllamaMarket_Peek(Player* bot, OllamaMarketNote& out)
{
    if (!bot || !g_MarketTalkMinutes)
        return false;

    const auto window = std::chrono::minutes(g_MarketTalkMinutes);
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_dealings.find(bot->GetGUID().GetRawValue());
    if (it == g_dealings.end())
        return false;
    for (auto d = it->second.rbegin(); d != it->second.rend(); ++d)
    {
        if (d->talked || now - d->when > window)
            continue;
        out = d->note;
        return true;
    }
    return false;
}

void OllamaMarket_NoteTalked(ObjectGuid botGuid)
{
    const auto window = std::chrono::minutes(g_MarketTalkMinutes);
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_dealings.find(botGuid.GetRawValue());
    if (it == g_dealings.end())
        return;
    for (auto d = it->second.rbegin(); d != it->second.rend(); ++d)
    {
        if (d->talked || now - d->when > window)
            continue;
        d->talked = true;   // the one OllamaMarket_Peek gave
        return;
    }
}

void OllamaMarket_Update()
{
    const auto window = std::chrono::minutes(std::max<uint32_t>(g_MarketTalkMinutes, 1));
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_dealings.begin(); it != g_dealings.end();)
    {
        std::deque<Dealing>& list = it->second;
        while (!list.empty() && now - list.front().when > window)
            list.pop_front();
        it = list.empty() ? g_dealings.erase(it) : std::next(it);
    }
}

uint64_t OllamaMarket_Heard()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_heard;
}
