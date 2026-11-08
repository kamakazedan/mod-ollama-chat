#ifndef MOD_OLLAMA_CHAT_LATELY_H
#define MOD_OLLAMA_CHAT_LATELY_H

#include <string>

#include <cstdint>

class Creature;
class Player;
class Quest;
struct ItemTemplate;

// --------------------------------------------------------------------------
// What a bot has been up to lately.
//
// "what you up to?" -- "nothing much just finished some grinds" is a bot
// with nothing to go on making something up. Players say what they did and
// where: grinding murlocs in Westfall, a quest in Duskwood, a ding. So each
// bot's own last half hour or so is kept: the run of kills it was on (in
// which zone, killing what, how many), the last quest it handed in and
// where, the best thing it got (a green or better piece of gear, or anything
// rare), and the last level it reached. Lately_Text puts it in a few words
// for a prompt; a mingle exchange gets it for each of them, and a bot asked
// what it has been up to gets it (Lately.Words).
//
// The Note functions are called from the game's own hooks, which can run on
// a map thread: they only note names and numbers under a mutex. The rest is
// world thread.
// --------------------------------------------------------------------------

void Lately_NoteKill(Player* killer, Creature* victim);
void Lately_NoteQuest(Player* player, Quest const* quest);
void Lately_NoteLevel(Player* player);
void Lately_NoteItem(Player* player, ItemTemplate const* item);

// "out in Westfall for about 25 minutes killing Murloc Coastrunner (12) and
// Murloc Tidehunter (5), until a few minutes ago; handed in the quest The
// Defias Brotherhood in Westfall about 10 minutes ago".
// Empty when there is nothing from the last Lately.Minutes.
std::string Lately_Text(Player* bot);

// The quest it handed in and the item it got, from the last Lately.Minutes,
// as Lately_Text tells them: ids, 0 for none. A chat that names them can say
// what they are (Links_DescribeQuest, Links_DescribeItem).
struct LatelyNamed
{
    uint32_t quest = 0;
    uint32_t item  = 0;
};
LatelyNamed Lately_Named(Player* bot);

// The line for a reply prompt (Lately.StateText), when what `message` asks
// is what the bot has been up to (Lately.Words). Empty otherwise.
std::string Lately_StateTextFor(Player* bot, const std::string& message);

#endif // MOD_OLLAMA_CHAT_LATELY_H
