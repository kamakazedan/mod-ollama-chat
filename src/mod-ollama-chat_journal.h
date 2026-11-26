#ifndef MOD_OLLAMA_CHAT_JOURNAL_H
#define MOD_OLLAMA_CHAT_JOURNAL_H

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// The journal (OllamaChat.Journal.*): what a bot and someone playing have
// done together, kept, and known the next time they meet.
//
// Someone who ran the Deadmines with you last week says so when you group
// again: "oh hey, deadmines crew". A bot met the person it had spent an
// evening with as a stranger, every time. What it remembers of people
// (mod-ollama-chat_memory) is distilled from what was said to it; of what it
// did with them it kept nothing.
//
// So what a bot and a person do together is written down as it happens, from
// the game itself, never from a model: the dungeon they were in and the
// bosses that went down there, a wipe, the zone they quested in and how many
// quests, a duel and who won it, something sold, bought or given, a
// resurrection, a battleground fought on the same team and how it went. One
// line a pair and outing (the same thing again within Journal.SessionHours
// adds to it), a few of them a pair
// (Journal.KeepPerPair), for Journal.KeepDays, in the characters database
// (mod_ollama_chat_journal).
//
// It is told, newest first and with how long ago ("6 days ago"), where it
// matters and nowhere else -- a list in every prompt is what every line
// would be about:
//
// - to a bot greeting the group it has just joined (mod-ollama-chat_joined);
// - to a reply to that person when the talk is of it (Journal.TalkWords:
//   "remember", "last time"), or it is the first thing the bot has said to
//   them in Journal.ReunionHours.
//
// Nothing is kept of two bots: only of a bot and someone playing.
//
// World thread, but for the two Note calls a hook makes, which only note.
// --------------------------------------------------------------------------

enum class JournalKind : uint8_t
{
    Run      = 1,   // in a dungeon together: its bosses, its wipes
    Questing = 2,   // quests done together in a zone
    Duel     = 3,
    Sold     = 4,   // the bot sold them something
    Bought   = 5,   // ...bought something from them
    Gave     = 6,   // ...gave them something
    Given    = 7,   // they gave the bot something
    Raised   = 8,   // they brought the bot back from the dead
    Battle   = 9,   // a battleground, on the same team: how many matches, how many won
};

void Journal_LoadConfig();   // startup and .ollama reload
void Journal_Load();         // startup: the table, into memory
void Journal_SaveAll();      // what has changed, to the table
void Journal_Update();       // world tick

// Whether anything is being kept (Journal.Enable). Any thread.
bool Journal_Wants();

// `bot` and `player`, someone playing, did this together. `subject` is what
// it was of (the item; for Raised, left empty, where they are); `detail` what
// goes with it (the price). Nothing is kept of two bots or of two people, nor
// of a thing with no name.
void Journal_Note(JournalKind kind, Player* bot, Player* player, const std::string& subject = std::string(),
                  const std::string& detail = std::string());

// "[Bolt of Silk Cloth] x5": an item as a journal line names it. Empty for
// one the game does not know.
std::string Journal_ItemWords(uint32_t entry, uint32_t count);

// A boss went down to `killer`'s group, in a dungeon: for each of its bots
// there and each of those playing.
void Journal_BossDown(Player* killer, const std::string& boss);

// Those of a group `here` have just wiped, in a dungeon.
void Journal_Wiped(const std::vector<Player*>& here);

// A battleground match has ended for `team`, those of one side still in it:
// for each of its bots and each of those playing. `outcome` is 1 when the
// side won, -1 when it lost, 0 when neither did.
void Journal_Battle(const std::vector<Player*>& team, const std::string& battleground, int outcome);

// From a hook, on whichever thread raised it: noted only. The duel's two are
// on the map whose thread this is.
void Journal_NoteDuel(Player* winner, Player* loser);
void Journal_NoteQuestDone(Player* player);

// For a prompt of `bot`'s to or about `player`: what the two have done
// together, newest first (Journal.HistoryText). Empty with nothing kept.
std::string Journal_PromptFor(Player* bot, Player* player);

// For a reply of `bot`'s to `player`: the same, when any of `heard` talks of
// it (Journal.TalkWords) or the bot has said nothing to them for
// Journal.ReunionHours. Empty otherwise.
std::string Journal_ReplyPromptFor(Player* bot, Player* player, const std::vector<std::string>& heard);

struct JournalStats
{
    uint32_t pairs   = 0;   // a bot and a person with something kept
    uint32_t entries = 0;
    uint64_t noted   = 0;   // things written down since startup
    uint64_t told    = 0;   // prompts told of them
    uint64_t saved   = 0;   // pairs written to the table
};
JournalStats Journal_GetStats();

#endif // MOD_OLLAMA_CHAT_JOURNAL_H
