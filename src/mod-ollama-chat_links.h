#ifndef MOD_OLLAMA_CHAT_LINKS_H
#define MOD_OLLAMA_CHAT_LINKS_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Item and quest links in bot lines.
//
// Players link what they talk about: "anyone got [Linen Cloth]?". The model
// cannot write a link -- it is client markup with an id in it -- so it is
// asked to put exact item and quest names in square brackets, and each
// "[Name]" is turned into a real link when the line is delivered:
//
//   1. a link someone posted in chat lately (the exact one they posted,
//      random suffix and all), so a bot answering about it links the same;
//   2. a quest in the bot's own quest log, then an item it carries;
//   3. the item and quest tables, preferring what suits the bot's level and
//      faction (Links.UseGameData).
//
// A name nothing matches loses its brackets and stays as plain text, so a
// made-up "[Sword of Doom]" just reads as Sword of Doom.
//
// World thread only.
// --------------------------------------------------------------------------

// Remember the links in a line a real player sent. Only links the core's own
// chat-link check accepts are kept, so nobody can plant a link that reads
// [Linen Cloth] and opens something else.
void Links_NoteMessage(const std::string& message);

// " When you name a particular item or quest, ..." -- empty when disabled.
std::string Links_PromptHint();

struct LinkedLine
{
    std::string text;            // what goes out, with the links in
    std::string plain;           // as a reader sees it, links as "[Name]": transcript, history, other bots
    uint32_t    linked     = 0;
    uint32_t    unresolved = 0;
};

// `text` (plain, no markup) with its "[Name]"s turned into links where one is
// known, at most Links.MaxPerLine of them. With links off, both texts are the
// line with its brackets taken off.
LinkedLine Links_Resolve(Player* bot, const std::string& text);

// The line went out: count it in the stats.
void Links_NoteSent(const LinkedLine& line);

void Links_Update();   // forget links not seen for Links.RememberMinutes

struct LinksStats
{
    uint64_t linked     = 0;   // names turned into links
    uint64_t unresolved = 0;   // bracketed names nothing matched
    uint32_t remembered = 0;   // links seen in chat, currently held
    bool     indexed    = false;
};
LinksStats Links_GetStats();

#endif // MOD_OLLAMA_CHAT_LINKS_H
