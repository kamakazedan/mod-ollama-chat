#ifndef MOD_OLLAMA_CHAT_ASIDE_H
#define MOD_OLLAMA_CHAT_ASIDE_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Whispered asides: the party gossip.
//
// Now and then -- rarely -- a bot in a group with a real player whispers them
// a quick catty, cheeky or gossipy remark about someone else in the group,
// the kind of thing people only say in a whisper. It is about something real
// and nothing made up: a line that someone just said in party chat, their
// class and spec, a level well off the rest of the group, a piece of their
// gear, their pet, how often they have died.
//
// Only some chat styles do it, NAT_GOSSIP most of all (Aside.Personalities:
// a weight per style, the share of its chances it takes). A chance on a
// party line long enough to remark on (Aside.LineChance, Aside.MinWords), or
// a chance once in a while out of a fight (Aside.IdleChance every
// Aside.CheckSeconds). Never more than one to the same player in
// Aside.PlayerMinutes, from whichever bot, and never one bot again within
// Aside.BotMinutes, so several gossips in one group do not take turns; never
// the same person talked about twice running; not while they or the bot are
// in a fight, nor to someone away.
//
// The whisper goes into that bot's history with the player, so a whisper
// back carries on from it -- when bots answer whispers at all
// (OllamaChat.EnableWhisperReplies). The asides themselves do not need that:
// they are a remark, not a conversation.
//
// World thread only.
// --------------------------------------------------------------------------

// A line said in a group's chat (by a person or a bot), as it went out.
void Aside_NotePartyLine(Player* speaker, const std::string& text, ChatChannelSourceLocal source);

void Aside_Update(uint32_t diff);   // world tick: the odd idle remark, and housekeeping

struct AsideStats
{
    uint64_t whispered = 0;   // asides sent
    uint64_t onLines   = 0;   // ...about something someone said
    uint64_t idle      = 0;   // ...about something else about them
};
AsideStats Aside_GetStats();

#endif // MOD_OLLAMA_CHAT_ASIDE_H
