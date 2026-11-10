#ifndef MOD_OLLAMA_CHAT_RACE_H
#define MOD_OLLAMA_CHAT_RACE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// Battle cries, and what each race says.
//
// "FOR THE HORDE!" in General got nothing back, and an orc saying "zug zug"
// got nobody saying it back: the bots knew their race as a word in the
// prompt, and nothing of what an orc says, or that players say it for fun.
//
//   * Sides.Cries.Horde / .Alliance: a real player's short line that is a
//     side's battle cry is said to everyone, and each reply prompt is told
//     what it is -- the bot's own side's (Sides.OwnCryText: shout it back,
//     cheer), or the other side's in its side's chat (Sides.EnemyCryText:
//     give them stick for it, all in fun).
//   * Race.Phrases.<Race>: what each race says in the game -- "zug zug",
//     "hello mon", "ishnu-alah" -- with what it means, from the game's own
//     greetings and languages. A real player's short line with one is said
//     to everyone, one of that race answers first if one is there, and each
//     reply prompt is told what it is and whose (Race.HeardText), and a bot
//     of that race that saying it back is part of the joke
//     (Race.HeardSameText).
//   * A bot's own line, now and then -- Race.UseChance percent of them, more
//     for some styles (Race.StyleChance: the roleplayer) -- may slip in
//     something its own race says (Race.UseText). A /say exchange between
//     bots likewise (Mingle.RaceText).
//
// A line is only a cry or a race's phrase when it is short: "FOR THE HORDE!!",
// "zug zug lads", not a sentence that mentions one.
//
// World thread only.
// --------------------------------------------------------------------------

struct RaceLineFacts
{
    bool        cry         = false;   // a side's battle cry
    bool        cryAlliance = false;   // ...the Alliance's (else the Horde's)
    std::string cryText;               // the cry, as the line says it
    bool        phrase      = false;   // something a race says
    std::string phraseText;            // ...as the list writes it
    std::string meaning;               // ...and what it is
    std::vector<uint8_t> races;        // ...whose
};

// What a line is, if it is either: a battle cry comes first.
RaceLineFacts Race_ReadLine(const std::string& msg);

// Is `bot` one of `races`?
bool Race_Is(Player* bot, const std::vector<uint8_t>& races);

// For a reply prompt of `bot` answering real player `speaker`'s `msg`: what
// the cry or the phrase is, and what `bot` might do with it. Empty when the
// line is neither, or the speaker is not a real player.
std::string Race_HeardFor(Player* bot, Player* speaker, const std::string& msg);

// For a line of `bot`'s own, rolled each time (Race.UseChance or its style's
// Race.StyleChance): something its race says, to slip in if it fits. Usually
// empty.
std::string Race_UseFor(Player* bot);

// The same for one of the people in a /say exchange between bots
// (Mingle.RaceText), with a leading space. Usually empty.
std::string Race_MingleFor(Player* bot);

// After the config is read: the phrase lists, parsed.
void Race_Load();

struct RaceStats
{
    uint64_t cries       = 0;   // reply prompts told of their own side's battle cry
    uint64_t enemyCries  = 0;   // ...of the other side's, in their side's chat
    uint64_t heard       = 0;   // ...of something a race says
    uint64_t used        = 0;   // lines of a bot's own told it could say something its race says
    size_t   phrases     = 0;   // phrases known, all races
};
RaceStats Race_GetStats();

#endif // MOD_OLLAMA_CHAT_RACE_H
