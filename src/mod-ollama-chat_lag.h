#ifndef MOD_OLLAMA_CHAT_LAG_H
#define MOD_OLLAMA_CHAT_LAG_H

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Lag (OllamaChat.Lag.*): talked of only when the realm really lags.
//
// "anyone else noticed the lag here?" and "yeah it is proper chunky here"
// went out in guild chat while the world ticked at a 16 ms median. Asked
// for something low-key to type, and whether anyone else had noticed it, the
// model reached for the oldest complaint there is. A bot saying the realm
// lags makes it look bad to everyone reading, and it was not so.
//
// The world's own ticks say how it is (Lag_Tick, every world update, with the
// diff .server info reports). Over the last Lag.Minutes, the realm lags when
// a moment of it has been spent in a tick of Lag.MeanMs or more on average
// (weighted by time, so a run of slow ticks among thousands of quick ones
// still counts for the time it took), or when Lag.Spikes ticks took
// Lag.SpikeMs or longer -- hitches everyone on it felt.
//
// - A line that talks of lag, a crash or a disconnect (Lag.Words) goes out
//   only while the realm lags, or answering a line that talked of it itself
//   ("lag?" -- "none here"). Otherwise it is not said: one owed to someone
//   gets another go without it (Lag.Note).
// - A reply where the talk is of lag is told how the realm really is
//   (Lag.SmoothText, Lag.LaggingText), so "anyone lagging?" gets a true
//   answer, and so does "yeah me too".
// - While the realm lags, an ambient line is now and then told so
//   (Lag.AmbientText, Lag.AmbientChance, no more than one in
//   Lag.AmbientMinutes): a bot that grumbles about lag has lag to grumble at.
//
// World thread only.
// --------------------------------------------------------------------------

// Every world update: how long the tick took (the diff WorldScript::OnUpdate gets).
void Lag_Tick(uint32_t diffMs);

// The realm is lagging, by the last Lag.Minutes of ticks. False with Lag.Enable off.
bool Lag_Now();

// `text` talks of lag (Lag.Words), names in [brackets] aside. `which` gets the
// words, for the log.
bool Lag_TalksOf(const std::string& text, std::string& which);

// Whether a line may go out: always with Lag.Enable off or while the realm
// lags, and when `answering` (the line it answers; empty for one nobody asked
// for) talks of lag too; otherwise only if it does not talk of lag. `which`
// gets the words that kept it in. A line kept in is counted.
bool Lag_MayGoOut(const std::string& text, const std::string& answering, std::string& which);

// For a reply: how the realm really is (Lag.SmoothText or Lag.LaggingText),
// when any of `heard` -- the line answered, and those before it -- talks of
// lag. Empty otherwise.
std::string Lag_PromptFor(const std::vector<std::string>& heard);

// For an ambient line: that the realm is lagging, now and then while it is
// (Lag.AmbientText, Lag.AmbientChance, one in Lag.AmbientMinutes). Empty
// otherwise.
std::string Lag_AmbientText();

struct LagStats
{
    bool     lagging     = false;
    uint32_t meanMs      = 0;   // ticks, weighted by time, over the window
    uint32_t spikes      = 0;   // ticks of Lag.SpikeMs or more in the window
    uint32_t windowSecs  = 0;   // how much of the window there is so far
    uint64_t spells      = 0;   // times the realm started lagging
    uint64_t keptIn      = 0;   // lines talking of lag not said: it did not lag
    uint64_t toldSmooth  = 0;   // replies told it runs smoothly
    uint64_t toldLagging = 0;   // prompts told it lags
};
LagStats Lag_GetStats();

#endif // MOD_OLLAMA_CHAT_LAG_H
