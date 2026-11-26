#include "mod-ollama-chat_weather.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat_textrules.h"
#include "mod-ollama-chat-utilities.h"

#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Phrases = std::vector<std::vector<std::string>>;

    // What a sky is doing, as far as what may be said of it goes.
    enum class Sky : uint8_t { Fine, Fog, Rain, Snow, Sandstorm, Thunder, Count };
    constexpr size_t kSkies = size_t(Sky::Count);
    constexpr size_t kNoName = size_t(-1);
    constexpr size_t kNames = 13;   // the states the game has, the clear one aside

    struct Seen
    {
        Sky    sky   = Sky::Fine;
        size_t name  = kNoName;   // into Weather.Names
        bool   known = true;      // false: which sky is over it cannot be told
    };

    Seen Read(uint32_t state)
    {
        switch (state)
        {
            case WEATHER_STATE_FOG:              return { Sky::Fog, 0 };
            case WEATHER_STATE_LIGHT_RAIN:       return { Sky::Rain, 1 };
            case WEATHER_STATE_MEDIUM_RAIN:      return { Sky::Rain, 2 };
            case WEATHER_STATE_HEAVY_RAIN:       return { Sky::Rain, 3 };
            case WEATHER_STATE_LIGHT_SNOW:       return { Sky::Snow, 4 };
            case WEATHER_STATE_MEDIUM_SNOW:      return { Sky::Snow, 5 };
            case WEATHER_STATE_HEAVY_SNOW:       return { Sky::Snow, 6 };
            case WEATHER_STATE_LIGHT_SANDSTORM:  return { Sky::Sandstorm, 7 };
            case WEATHER_STATE_MEDIUM_SANDSTORM: return { Sky::Sandstorm, 8 };
            case WEATHER_STATE_HEAVY_SANDSTORM:  return { Sky::Sandstorm, 9 };
            case WEATHER_STATE_THUNDERS:         return { Sky::Thunder, 10 };
            case WEATHER_STATE_BLACKRAIN:        return { Sky::Rain, 11 };
            case WEATHER_STATE_BLACKSNOW:        return { Sky::Snow, 12 };
            default:                             return {};
        }
    }

    // Each zone's weather as the core last said it was. Written by the hook,
    // on whichever map thread the zone's map runs on. The core keeps weather
    // by map and zone and says only the zone: see Over.
    std::mutex                             g_skyMutex;
    std::unordered_map<uint32_t, uint32_t> g_states;   // zone -> WeatherState; under g_skyMutex

    // Settings: read and written on the world thread only (the hook reads none).
    bool        g_enable = true;
    bool        g_guard = true;
    Phrases     g_talkWords;
    Phrases     g_notWords;
    std::array<Phrases, kSkies> g_claimWords;   // by Sky
    std::vector<std::string>    g_names;        // kNames of them
    std::string g_fineName;
    std::string g_hereText;
    std::string g_indoorsText;
    std::string g_note;
    std::string g_ambientText;
    uint32_t    g_ambientChance = 8;
    uint32_t    g_ambientMinutes = 20;

    std::unordered_map<uint32_t, Clock::time_point> g_lastAmbient;   // by zone
    OllamaWeatherStats g_stats;

    // The sky over `bot`, where that can be told. A zone the core has said
    // nothing about has no weather of its own: a clear sky. Out in the world
    // a zone is on one map and has one sky. An instance whose zone has
    // weather (game_weather has Alterac Valley, Arathi Basin, Stratholme and
    // Ahn'Qiraj among them) has one for every copy of it running, and the
    // core's hook does not say which copy's changed: there the sky is not
    // known, so nothing is said of it and nothing held back.
    Seen Over(Player* bot)
    {
        Seen unknown;
        unknown.known = false;
        if (!bot || !bot->IsInWorld() || !bot->FindMap())
            return unknown;
        const bool copies = bot->FindMap()->Instanceable();
        std::lock_guard<std::mutex> lock(g_skyMutex);
        const auto it = g_states.find(bot->GetZoneId());
        if (it == g_states.end())
            return {};
        return copies ? unknown : Read(it->second);
    }

    const std::string& NameOf(const Seen& seen)
    {
        return seen.name < g_names.size() ? g_names[seen.name] : g_fineName;
    }

    std::string Fill(const std::string& templ, const Seen& seen)
    {
        if (templ.empty())
            return {};
        const std::string out = SafeFormat(templ, fmt::arg("weather", NameOf(seen)));
        return out == "[Format Error]" ? std::string() : out;
    }

    std::string Clean(const std::string& text)
    {
        return TextRules::WithoutBracketNames(TextRules::StripChatMarkup(text));
    }

    bool TalksOf(const std::string& text)
    {
        return !text.empty() && !TextRules::FindPhraseIndexes(Clean(text), g_talkWords).empty();
    }

    // Whether saying it is `claim` is right under `sky`. A thunderstorm is
    // rain too.
    bool Holds(Sky claim, Sky sky)
    {
        if (claim == Sky::Rain)
            return sky == Sky::Rain || sky == Sky::Thunder;
        return claim == sky;
    }
}

void OllamaChatWeatherScript::OnWeatherChange(Weather* weather, WeatherState state, float /*grade*/)
{
    // A map thread: note the zone and what its weather now is, nothing more.
    if (!weather)
        return;
    std::lock_guard<std::mutex> lock(g_skyMutex);
    g_states[weather->GetZone()] = uint32_t(state);
}

void Weather_LoadConfig()
{
    auto text = [](const char* key, const char* def)
    {
        return sConfigMgr->GetOption<std::string>(key, def);
    };
    auto phrases = [&text](const char* key, const char* def)
    {
        return CrowdText::ParsePhrases(text(key, def));
    };

    g_enable = sConfigMgr->GetOption<bool>("OllamaChat.Weather.Enable", true);
    g_guard = sConfigMgr->GetOption<bool>("OllamaChat.Weather.Guard", true);
    g_ambientChance = std::min<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Weather.AmbientChance", 8), 100);
    g_ambientMinutes =
        std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Weather.AmbientMinutes", 20), 1, 1440);

    g_talkWords = phrases("OllamaChat.Weather.TalkWords",
        "weather|raining|rain|rainy|snowing|snow|snowy|sandstorm|thunderstorm|thunder|foggy|fog|sunny|sunshine|"
        "drizzle|downpour");
    g_notWords = phrases("OllamaChat.Weather.NotWords",
        "not raining|isnt raining|isn't raining|stopped raining|no rain|rain stopped|rain has stopped|"
        "not snowing|isnt snowing|isn't snowing|stopped snowing|no snow|not sunny|no sun|if it rains|"
        "when it rains|in case it rains|hope it doesnt rain|hope it doesn't rain");
    g_claimWords[size_t(Sky::Fine)] = phrases("OllamaChat.Weather.FineWords",
        "sunny|sunshine|clear skies|clear sky|nice weather|lovely weather|beautiful weather|weather is nice|"
        "weather's nice|not a cloud");
    g_claimWords[size_t(Sky::Fog)] = phrases("OllamaChat.Weather.FogWords", "foggy|this fog|the fog");
    g_claimWords[size_t(Sky::Rain)] = phrases("OllamaChat.Weather.RainWords",
        "raining|the rain|this rain|rain again|more rain|pouring rain|pouring down|downpour|drizzle|drizzling|"
        "chucking it down|bucketing down");
    g_claimWords[size_t(Sky::Snow)] = phrases("OllamaChat.Weather.SnowWords",
        "snowing|snowstorm|snowfall|snow coming down");
    g_claimWords[size_t(Sky::Sandstorm)] = phrases("OllamaChat.Weather.SandstormWords",
        "sandstorm|sand storm|dust storm");
    g_claimWords[size_t(Sky::Thunder)] = phrases("OllamaChat.Weather.ThunderWords",
        "thunderstorm|thunder storm|thunder and lightning");

    g_names.clear();
    for (const std::string& name : SplitString(text("OllamaChat.Weather.Names",
             "fog|light rain|rain|heavy rain|light snow|snow|heavy snow|a light sandstorm|a sandstorm|"
             "a heavy sandstorm|a thunderstorm|dark rain|dark snow"), '|'))
        g_names.push_back(name);
    if (g_names.size() != kNames)
    {
        LOG_WARN("module.ollamachat", "[Ollama Chat] Weather.Names has {} names, not the {} the game has states "
                 "for: weather is left out of prompts.", g_names.size(), kNames);
        g_names.clear();
    }
    g_fineName = text("OllamaChat.Weather.FineName", "clear skies");
    g_hereText = text("OllamaChat.Weather.HereText", "The weather where you are right now: {weather}.");
    g_indoorsText = text("OllamaChat.Weather.IndoorsText", "You are indoors just now, out of it.");
    g_note = text("OllamaChat.Weather.Note",
        "Right now, where you are: {weather}. Do not say the weather is anything else.");
    g_ambientText = text("OllamaChat.Weather.AmbientText",
        "Right now there is {weather} where you are. You may grumble about it or mention it in passing, or say "
        "nothing of it.");
}

std::string Weather_PromptFor(Player* bot, const std::vector<std::string>& heard)
{
    if (!g_enable || !bot || g_talkWords.empty() || g_names.empty() || g_hereText.empty())
        return {};
    if (!std::any_of(heard.begin(), heard.end(), TalksOf))
        return {};
    const Seen seen = Over(bot);
    if (!seen.known)
        return {};
    std::string out = Fill(g_hereText, seen);
    if (out.empty())
        return {};
    if (!g_indoorsText.empty() && bot->IsInWorld() && !bot->IsOutdoors())
        out += " " + g_indoorsText;
    ++g_stats.told;
    return out;
}

bool Weather_MayGoOut(Player* bot, const std::string& text, const std::string& answering, bool byPerson,
                      std::string& which, std::string& note)
{
    which.clear();
    note.clear();
    if (!g_enable || !g_guard || !bot || g_names.empty() || text.empty())
        return true;
    const std::string clean = Clean(text);

    // What the line says the weather is.
    std::array<size_t, kSkies> hit{};   // which phrase, plus one
    bool any = false;
    for (size_t s = 0; s < kSkies; ++s)
    {
        if (g_claimWords[s].empty())
            continue;
        const std::vector<size_t> found = TextRules::FindPhraseIndexes(clean, g_claimWords[s]);
        if (found.empty())
            continue;
        hit[s] = found.front() + 1;
        any = true;
    }
    if (!any)
        return true;
    // "at least its not raining": it says what the weather is not.
    if (!TextRules::FindPhraseIndexes(clean, g_notWords).empty())
        return true;

    const Seen seen = Over(bot);
    if (!seen.known)
        return true;
    for (size_t s = 0; s < kSkies; ++s)
    {
        if (!hit[s] || Holds(Sky(s), seen.sky))
            continue;
        // Asked about it, it was told how it is, and may say so its own way.
        if (byPerson && TalksOf(answering))
            return true;
        which = TextRules::PhraseKey(g_claimWords[s][hit[s] - 1]);
        note  = Fill(g_note, seen);
        ++g_stats.keptIn;
        return false;
    }
    return true;
}

std::string Weather_NameFor(Player* bot)
{
    if (!g_enable || !bot || g_names.empty())
        return {};
    const Seen seen = Over(bot);
    if (!seen.known || seen.sky == Sky::Fine || !bot->IsOutdoors())
        return {};
    return NameOf(seen);
}

std::string Weather_AmbientText(Player* bot)
{
    if (!g_enable || !bot || g_ambientChance == 0 || g_ambientText.empty() || g_names.empty())
        return {};
    const Seen seen = Over(bot);
    if (!seen.known || seen.sky == Sky::Fine)
        return {};
    // urand is inclusive at both ends: 1..100 against the chance.
    if (urand(1, 100) > g_ambientChance)
        return {};
    const uint32_t zone = bot->GetZoneId();
    const auto now = Clock::now();
    const auto last = g_lastAmbient.find(zone);
    if (last != g_lastAmbient.end() && now - last->second < std::chrono::minutes(g_ambientMinutes))
        return {};
    if (!bot->IsOutdoors())
        return {};   // it is not out in it
    const std::string out = Fill(g_ambientText, seen);
    if (out.empty())
        return {};
    g_lastAmbient[zone] = now;
    ++g_stats.ambient;
    return out;
}

OllamaWeatherStats Weather_GetStats()
{
    OllamaWeatherStats s = g_stats;
    std::lock_guard<std::mutex> lock(g_skyMutex);
    s.zones = uint32_t(g_states.size());
    for (const auto& [zone, state] : g_states)
        if (Read(state).sky != Sky::Fine)
            ++s.unclear;
    return s;
}
