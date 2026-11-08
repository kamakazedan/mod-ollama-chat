#ifndef MOD_OLLAMA_CHAT_CONGRATS_H
#define MOD_OLLAMA_CHAT_CONGRATS_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Channel;
class Player;
struct AchievementEntry;

// --------------------------------------------------------------------------
// "gz", and the one "ty".
//
// Someone dings and the group types gz. What happens next on a real server:
// the one who dinged says "ty" -- or "ty all" once the gz have stopped coming
// -- and that is the end of it. Nobody else answers a "gz", and the one who
// dinged does not thank every single one.
//
// So a line from a real player that is nothing but a congratulation ("gz",
// "grats on 20 Edos!", "congrats on the ding mate"; Congrats.Words, with
// names, numbers and Congrats.FillerWords around them) gets no crowd at all.
// It is for whoever it names, else for whoever dinged or earned an
// achievement lately among the people who can read it (the group, the guild,
// those near enough to hear). When that is a bot, it thanks -- once for that
// ding, however many gz it gets, from real players or other bots -- a few
// seconds after the first, so "ty all" can come after the lot
// (Congrats.ThanksLines, Congrats.ThanksAllLines).
//
// World thread only, except Congrats_NoteCelebrated.
// --------------------------------------------------------------------------

// `who` levelled or earned something. Any thread: level-ups and achievements
// fire on map threads.
void Congrats_NoteCelebrated(Player* who, const std::string& what);

// Whether an achievement is worth a gz from the people around. Nobody types
// gz for Can I Keep Him? or Duel-icious; they do for a Feat of Strength, a
// realm first, one that gives a title, a mount, a pet or a tabard, one made
// of other achievements (Glory of the Hero, Loremaster, World Explorer), or
// one worth Congrats.AchievementRarePoints points. Never a level one: the
// ding gets its own gz. Congrats.AchievementAlways / .AchievementNever
// overrule it by id; Congrats.AchievementsRareOnly off makes them all worth
// it. Reads only game data loaded at startup: any thread.
bool Congrats_AchievementWorthIt(AchievementEntry const* achievement);

// Whether guild chat congratulates a guild member's ding to `level`. The
// game never tells a guild about a ding, only about the Level 10, 20 ... 80
// achievements that come with some, and those are as common as any: only the
// levels in Congrats.GuildLevels (the top one, by default) get a gz there.
// Any thread.
bool Congrats_GuildHearsDing(uint32_t level);

// A real player's line. True when it is nothing but a congratulation: the
// one it is for thanks (once), and nobody else answers it. `scopeKey` is
// where it was said.
bool Congrats_HandleHumanLine(Player* player, const std::string& msg, ChatChannelSourceLocal source,
                              Channel* channel, Player* receiver, const std::string& scopeKey);

// A bot's congratulation for `name` went out: the one it was for hears it
// too -- the same single thank-you covers it.
void Congrats_NoteBotCongrats(Player* bot, const std::string& name, ChatChannelSourceLocal source,
                              const std::string& channelName, uint32_t channelId, const std::string& scopeKey);

void Congrats_Tick();     // world tick: the thank-yous that are due go out
void Congrats_Update();   // maintenance: old dings are forgotten

struct CongratsStats
{
    uint64_t celebrated = 0;   // dings and achievements noted
    uint64_t common     = 0;   // ...achievements of them too common for anyone to gz
    uint64_t guildQuiet = 0;   // dings a guild was not told of: no gz there
    uint64_t heard      = 0;   // congratulations that only the one it was for answers
    uint64_t thanked    = 0;   // thank-yous sent
    uint64_t repeat     = 0;   // more gz after it had thanked already: nothing
    uint64_t nobody     = 0;   // a gz for nobody in particular: nothing
};
CongratsStats Congrats_GetStats();

#endif // MOD_OLLAMA_CHAT_CONGRATS_H
