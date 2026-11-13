#ifndef MOD_OLLAMA_CHAT_GROUPQUESTS_H
#define MOD_OLLAMA_CHAT_GROUPQUESTS_H

// --------------------------------------------------------------------------
// Group quests, said in General (OllamaChat.GroupQuests.*).
//
// Getting random bots together for group quests is mod-playerbots-plus's
// (PlayerbotsPlus.GroupQuests.* there): who asks, who comes along, who leads.
// This has them say it, each in its own style, in the zone's General
// channel: the ask for help ("LF2M [The Ogre Threat]"), the answers of
// those who have it too, the one getting a party together telling one who
// asks to come along, and the one nobody came for saying it will try alone.
//
// An ask, or going alone, is a line nobody asked for: it is not started
// within Context.AmbientHoldSeconds of a real player speaking there. An
// answer follows the ask it answers by a few seconds, and only an ask that
// went out to be written: none is said to an ask held back. Any of them is
// dropped if a real player spoke while it was being written (an answer with
// the ask it answers), and going alone is only said after asks someone could
// have read. Who answers is mod-playerbots-plus's, so nobody else hears these
// as something to reply to. Nothing is said where no real player would read
// it. The quest is named in brackets, so it goes out as a link
// (mod-ollama-chat_links: the bot holds it).
//
// Without mod-playerbots-plus (its API 4), none of this is there.
// World thread only.
// --------------------------------------------------------------------------

#include <cstdint>

void GroupQuests_Init();

struct GroupQuestsStats
{
    uint64_t heard = 0;   // asks, answers, invites and goes-alone from mod-playerbots-plus
    uint64_t said  = 0;   // ...sent to be written
    uint64_t quiet = 0;   // ...not: nobody real in General there, a player had just spoken, the ask it
                          //    answers (or the asks before going alone) not said, or no room
};
GroupQuestsStats GroupQuests_GetStats();

#endif // MOD_OLLAMA_CHAT_GROUPQUESTS_H
