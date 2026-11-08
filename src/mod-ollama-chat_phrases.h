#ifndef MOD_OLLAMA_CHAT_PHRASES_H
#define MOD_OLLAMA_CHAT_PHRASES_H

#include <cstdint>
#include <string>

// --------------------------------------------------------------------------
// Stock phrases.
//
// Every model has its favourite lines. Said once, "this is taking forever" is
// a player; said by a different bot every ten minutes it is a script. A phrase
// on Response.OverusedPhrases may be said once, by anyone, per
// Response.OverusedCooldownSeconds across the whole realm.
//
// Response.OffEraPhrases is for things that do not exist in Wrath of the Lich
// King (keystones, transmog, world quests): a line with one never goes out.
//
// Response.RarePhrases are a style's hobby horse ("40 man raids", "back in
// vanilla"): once per Response.RareCooldownSeconds across the realm, even in a
// line owed to someone -- otherwise a nostalgic bot answers everything with
// the same memory.
//
// Response.FourthWallPhrases is the fourth wall -- bots, AI, "real players"
// -- on a realm where everyone is a player: a line with one never goes out.
// The one opening is a real player bringing it up: the line answering them
// may wink at it, and then no other bot does for Response.FourthWallMinutes.
// Response.FourthWallAllow are names that only look like it ("Clockwork Rocket
// Bot", "Field Repair Bot").
//
// Outlook.LevelPhrases holds things only some levels say ("[40+] repair
// bill", "[70+] heroic"): a bot outside that range does not say them, so a
// level 6 never complains about repair bills even when the model slips.
//
// Lines owed to someone who spoke to the bot directly are only held to the
// off-era and rare lists: the other two would leave them with no answer. And a phrase
// the line being answered used itself never counts: "naxx? im only 12 lol"
// is a fine answer to "anyone for naxx", and "no transmog in wrath" to "can
// you transmog?".
//
// World thread only.
// --------------------------------------------------------------------------

enum class PhraseCheck : uint8_t
{
    Ok,
    FourthWall, // bots, AI, "real players": only when someone else brought it up
    OffEra,     // never in Wrath
    OffLevel,   // not something a player of the bot's level says
    Overused,   // said somewhere on the realm too recently
    Rare        // a hobby horse, said too recently
};

// `answering` is the line the bot is replying to, plain text (empty for
// chatter). `which` gets the phrase that matched, for the debug log.
//
// `answeringPlayer`: `answering` is a real player's line, not another bot's.
PhraseCheck Phrases_Check(const std::string& text, const std::string& answering, bool answeringPlayer,
                          bool directAddress, uint32_t level, std::string& which);

// The line went out: its stock phrases start their cooldown now.
void Phrases_NoteSaid(const std::string& text);

// Echoes. Nobody can list every phrase a model falls back on, so they are
// counted as they go out: a run of Echo.Words words that says something
// (not only Echo.CommonWords) that Echo.Bots other bots have said in the last
// Echo.WindowMinutes is an echo -- "fight one huge murloc" the fourth time
// that hour, whoever says it. One the line being answered has is fair game.
// `which` gets the phrase. World thread.
bool Phrases_IsEcho(uint64_t botGuid, const std::string& text, const std::string& answering, std::string& which);

// A bot's line went out: its phrases are counted.
void Phrases_NoteEcho(uint64_t botGuid, const std::string& text);

void Phrases_Update();   // forget cooldowns that have run out

struct PhrasesStats
{
    uint64_t droppedFourthWall = 0;
    uint64_t fourthWallWinks   = 0;   // went out, answering a player who brought it up
    uint64_t droppedOffEra   = 0;
    uint64_t droppedOffLevel = 0;
    uint64_t droppedOverused = 0;
    uint64_t droppedRare     = 0;
    uint64_t echoes          = 0;   // lines found to echo other bots
    uint32_t coolingDown     = 0;
    uint32_t phrasesTracked  = 0;
};
PhrasesStats Phrases_GetStats();

#endif // MOD_OLLAMA_CHAT_PHRASES_H
