#ifndef MOD_OLLAMA_CHAT_PROGRESS_H
#define MOD_OLLAMA_CHAT_PROGRESS_H

#include <cstdint>

// --------------------------------------------------------------------------
// Quest progress, said the way a player says it.
//
// Playerbots whispers its master every kill and every item a quest wants --
// "Kobold Vermin 3/10 [Kobold Camp Cleanup]", "[Linen Cloth] 2/6 [...]" --
// and "Quest completed [...]" on top. Nobody needs 1 of 6 of anything. Those
// whispers are muted (OllamaChat.Mute.Patterns); what a bot says instead, in
// party chat and in its own words, is the moment something is done: the last
// kill or item a quest wants, or the whole quest ready to hand in.
//
// Only for bots in a group with a real player. A few seconds pass first, so
// the kill that finishes the quest as well as the objective gets one line,
// not two; when several bots finish the same thing together (kill credit is
// shared), one of them says it; and a group hears at most one of these in
// Progress.GapSeconds -- the rest wait their turn, or are let go.
//
// World thread only: the quest logs of the bots that qualify are read every
// Progress.CheckSeconds.
// --------------------------------------------------------------------------

void Progress_Update(uint32_t diff);

// A line about something finished went out (OllamaChatRequest::progressKey):
// the rest of the group who finished it too need not say it. Until it is out,
// they wait; if it never goes out, one of them says it instead.
void Progress_NoteDelivered(uint64_t key);

struct ProgressStats
{
    uint64_t objectives = 0;   // a quest's last kill or item, seen
    uint64_t quests     = 0;   // a quest ready to hand in, seen
    uint64_t said       = 0;   // lines about it
    uint64_t shared     = 0;   // someone else in the group said it already
    uint64_t lapsed     = 0;   // waited too long for a turn, or nowhere to say it
};
ProgressStats Progress_GetStats();

#endif // MOD_OLLAMA_CHAT_PROGRESS_H
