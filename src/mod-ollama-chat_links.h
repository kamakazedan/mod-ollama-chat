#ifndef MOD_OLLAMA_CHAT_LINKS_H
#define MOD_OLLAMA_CHAT_LINKS_H

#include <cstdint>
#include <string>
#include <vector>

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
// line with its brackets taken off. A thing linked in the chat `scopeKey` in
// the last Links.RepeatSeconds -- in `replyingTo`, the line being answered,
// or by a bot -- is named plainly instead of linked again.
LinkedLine Links_Resolve(Player* bot, const std::string& text, const std::string& scopeKey = std::string(),
                         const std::string& replyingTo = std::string());

// The line went out: count it in the stats.
void Links_NoteSent(const LinkedLine& line);

// --------------------------------------------------------------------------
// What a named thing is.
//
// "anyone seen [Dusty Mining Boots]?" was answered with how to kill the
// boots, and "need a healer for [Look To The Stars]" was a delivery quest.
// The model only sees a name in brackets. For a reply prompt, these say what
// the items and quests a line names really are: an item's kind, slot, quality
// and level, a quest's zone, kind and what it asks for (its objectives, as
// the quest log has them).
// --------------------------------------------------------------------------

struct ItemTemplate;
class Quest;

// "Linen Cloth is an item: cloth for tailoring and first aid, ..."; empty
// for nullptr. Names go in plainly: a name in brackets in a prompt is copied,
// and becomes a link nobody needed.
std::string Links_DescribeItem(ItemTemplate const* proto);

// The item a name means ("Rune of Teleportation", any case), as a bot of this
// level would take it when several share the name. Null when no item does.
ItemTemplate const* Links_ItemNamed(const std::string& name, Player* bot);

// "[The Fargodeep Mine] is a quest in Elwynn Forest, for level 7: Explore ...";
// `reader` fills in the quest text's $N, $C, $R.
std::string Links_DescribeQuest(Quest const* quest, Player* reader);

// The quest text a quest log shows for `quest`, as `reader` would read it, at
// most `maxLen` bytes: its objectives (else the start of its details).
std::string Links_QuestObjective(Quest const* quest, Player* reader, size_t maxLen);

// For a reply prompt: what the items and quests `line` names are -- links
// first, then names in square brackets that match something real -- at most
// Links.DescribeMax of them, in Links.DescribeText, each with how it sits
// against `bot`'s level (a quest far above it, an item it cannot use yet, a
// quest that would be easy). Empty when it names none.
std::string Links_Describe(Player* bot, const std::string& line);

// How a quest sits against `bot`'s level: too high when it is more than
// Links.QuestLevelsAbove levels above the bot or the bot cannot take it yet;
// easy when it is grey to the bot.
enum class QuestFit : uint8_t
{
    Fits,
    TooHigh,
    Easy
};
QuestFit Links_QuestFitFor(Player* bot, Quest const* quest);

// The quests `line` names -- links by id, names in square brackets as
// `speaker` would mean them -- at most 4. Worked out once per line.
std::vector<Quest const*> Links_QuestsNamed(Player* speaker, const std::string& line);

// "anyone want to group for [Wanted: Gath'Ilzogg]?" to a level 12: every one
// of `quests` (and there is at least one) is too high for `bot`. A bot like
// that has nothing to offer about it (Links.QuietWhenTooLow).
bool Links_AllTooHighFor(Player* bot, const std::vector<Quest const*>& quests);

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
