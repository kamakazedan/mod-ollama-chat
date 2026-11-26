#ifndef MOD_OLLAMA_CHAT_WEATHER_H
#define MOD_OLLAMA_CHAT_WEATHER_H

#include "ALEScript.h"
#include "Weather.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// The weather (OllamaChat.Weather.*): talked of as it really is.
//
// The game has weather -- rain over Elwynn, snow in Dun Morogh, sandstorms in
// Tanaris -- and everyone standing in a zone sees the same sky. A bot saw
// none of it: the model talks about the weather as freely as anyone, and
// knows nothing of it. "lovely day for it" in a downpour, or a grumble about
// rain under a clear sky, is the same made-up fact as lag on a smooth realm.
//
// What a zone's weather is doing is the core's own (its weather-change hook,
// raised when a zone's weather is made and each time it changes; a zone with
// no weather of its own has a clear sky, as the client shows it). The hook
// says which zone, not which map: out in the world that is one sky, but an
// instance whose zone has weather -- Alterac Valley, Arathi Basin, Stratholme
// and Ahn'Qiraj among them -- has a sky for each copy of it running. In those
// the module does not know the weather, so it says nothing of it and holds no
// line back.
//
// - A reply where the talk is of the weather (Weather.TalkWords) is told what
//   it is doing where the bot stands (Weather.HereText), and whether the bot
//   is indoors.
// - A bot's line that says it is raining, snowing, storming, foggy or fine
//   (Weather.RainWords and the rest) goes out only when that is so where it
//   stands (Weather.Guard), or answering a person who brought the weather
//   up. One owed to someone gets another go, told how it is (Weather.Note).
// - While it is doing something, an ambient line of a bot out in it is now
//   and then told so (Weather.AmbientText, Weather.AmbientChance, no more
//   than one for a zone in Weather.AmbientMinutes): a grumble about the rain
//   is then about rain that is falling.
//
// The hook runs on a map thread and only notes the zone and the state. The
// rest is the world thread's.
// --------------------------------------------------------------------------

class OllamaChatWeatherScript : public ALEScript
{
public:
    OllamaChatWeatherScript() : ALEScript("OllamaChatWeatherScript") {}
    void OnWeatherChange(Weather* weather, WeatherState state, float grade) override;
};

void Weather_LoadConfig();   // startup and .ollama reload

// For a reply: what the weather is doing where `bot` stands, when any of
// `heard` -- the line answered, and those before it -- talks of the weather.
// Empty otherwise.
std::string Weather_PromptFor(Player* bot, const std::vector<std::string>& heard);

// Whether a line of `bot`'s may go out: not when it says the weather is
// something it is not where the bot stands, unless `byPerson` and
// `answering` (the line it answers) talks of the weather. `which` gets the
// words that kept it in, `note` what a second go is told.
bool Weather_MayGoOut(Player* bot, const std::string& text, const std::string& answering, bool byPerson,
                      std::string& which, std::string& note);

// What it is doing where `bot` stands, in words ("heavy rain", "a
// sandstorm"), when it is doing something and the bot is out in it. Empty
// under a clear sky, indoors, where it is not known, or with Weather.Enable
// off.
std::string Weather_NameFor(Player* bot);

// For an ambient line: that it is raining (snowing, ...) where `bot` stands,
// now and then while it is and the bot is out in it. Empty otherwise.
std::string Weather_AmbientText(Player* bot);

struct OllamaWeatherStats
{
    uint32_t zones   = 0;   // zones whose weather has been heard of
    uint32_t unclear = 0;   // ...where it is doing something now
    uint64_t told    = 0;   // replies told what it is doing
    uint64_t ambient = 0;   // ambient lines told
    uint64_t keptIn  = 0;   // lines that had the weather wrong, not said
};
OllamaWeatherStats Weather_GetStats();

#endif // MOD_OLLAMA_CHAT_WEATHER_H
