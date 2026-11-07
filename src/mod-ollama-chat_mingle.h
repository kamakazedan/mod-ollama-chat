#ifndef MOD_OLLAMA_CHAT_MINGLE_H
#define MOD_OLLAMA_CHAT_MINGLE_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Mingling: bots chatting among themselves in /say.
//
// In a city a real player had bots talking to them -- a wave as they passed,
// an answer to their /say -- and bots arguing in General, but two bots
// standing next to each other never had a word for each other. Now, while a
// real player is in a city (Mingle.CitiesOnly) and not fighting, every
// Mingle.GapMinSeconds..GapMaxSeconds (Mingle.StartChance) two or three idle
// random bots near them -- within Mingle.HearRange of them and
// Mingle.PairRange of each other, on their side, in no group, not AFK, not
// busy -- may have a short exchange in /say: Mingle.MinLines to MaxLines
// lines about something people chat about standing around (Mingle.Premises),
// ending the way people do: a laugh, "anyway gtg", a wave. Bots hanging out
// at a spot (mod-ollama-chat_hangout) are picked first; one walking past
// stops to talk.
//
// One generation writes the whole exchange (Mingle.PromptTemplate), so it has
// a start and an end and costs one request rather than ten; it is only asked
// for when the request queue is quiet (Mingle.QueueLimit). Then each line
// goes out in turn as that bot's own /say, after a pause to read the one
// before and the time to type it, through the same checks as any other line.
// A line can be an emote at the other one -- "Grigohm laughs at Thimack." --
// and whoever is spoken to turns to the speaker. The others in earshot leave
// them to it.
//
// It stops when one of them is wanted (a fight, an invite, a trade, a
// queue), walks off or logs out, when a line is held back, when no real
// player is in earshot any more, or when a real player speaks to one of them
// in /say -- who is then answered as usual.
//
// And every Mingle.Emote.MinSeconds..MaxSeconds near a real player, two bots
// standing close may just greet each other without a word (Mingle.Emote.*):
// a wave, a nod, a bow, and often one back.
//
// World thread only, except Mingle_ForgetPlayer.
// --------------------------------------------------------------------------

void Mingle_Update(uint32_t diff);   // world tick, after OllamaDispatch_Update

// A real player's /say or yell. An exchange it is aimed at -- naming one of
// its bots, or answering one (`addresseeGuid`) -- stops, so that bot can
// answer them instead.
void Mingle_NoteHumanSay(Player* player, const std::string& msg, uint64_t addresseeGuid);

// In an exchange right now: left out of other /say chatter until it is over.
bool Mingle_IsBusy(Player* bot);

void Mingle_ForgetPlayer(uint64_t guid);   // logout; any thread
void Mingle_Reset();

struct MingleStats
{
    uint64_t asked      = 0;   // exchanges asked for
    uint64_t written    = 0;   // ...that came back usable
    uint64_t unusable   = 0;   // ...that came back without two people saying something
    uint64_t finished   = 0;   // ran to their last line
    uint64_t cut        = 0;   // stopped early: someone left, was wanted or spoken to
    uint64_t lines      = 0;   // lines said
    uint64_t emotes     = 0;   // emotes at each other in them
    uint64_t greetings  = 0;   // greetings between bots without a word
    uint32_t running    = 0;
};
MingleStats Mingle_GetStats();

#endif // MOD_OLLAMA_CHAT_MINGLE_H
