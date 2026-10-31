#ifndef MOD_OLLAMA_CHAT_JOINED_H
#define MOD_OLLAMA_CHAT_JOINED_H

#include "mod-ollama-chat_handler.h"   // ChatChannelSourceLocal, ScriptMgr.h

#include <cstdint>
#include <string>

class Group;
class Player;

// --------------------------------------------------------------------------
// Joining a group.
//
// Playerbots has a bot whisper "Hello" (or "Hi!", "Hello there!") to whoever
// invited it, every time. Nobody does that. What people do:
//
//   * invited out of the blue, they say hi in party chat -- "hey, whats up?"
//     and the like -- and not all five of a group invited at once do: one
//     says it (Joined.GroupCooldownSeconds), and not every time
//     (Joined.GreetChance);
//   * invited after talking to them -- in General, a whisper, out loud
//     nearby -- they carry on from that in party chat ("right, deadmines
//     then?"), instead of greeting someone they were just talking to.
//
// The stock whisper is on OllamaChat.Mute.Patterns. World thread only,
// except the group hook, which only notes the join.
// --------------------------------------------------------------------------

// A bot's line went out to a real player outside party chat: what was said
// on each side, for a group invite that follows. World thread, at delivery.
void Joined_NoteExchange(Player* bot, Player* player, ChatChannelSourceLocal source,
                         const std::string& channelName, const std::string& playerLine,
                         const std::string& botLine);

// World tick: greet for the joins that are due.
void Joined_Update();

// Maintenance: old exchanges are forgotten.
void Joined_Maintain();

struct JoinedStats
{
    uint64_t joins     = 0;   // bots that joined a group with a real player in it
    uint64_t greeted   = 0;   // said hi out of the blue
    uint64_t carriedOn = 0;   // carried on a conversation instead
    uint64_t quiet     = 0;   // said nothing: someone else in the group just had, or by chance
};
JoinedStats Joined_GetStats();

// Notes who joined which group. Group::AddMember runs on map threads when a
// bot accepts an invite in its AI tick, so this only records it.
class OllamaChatJoinScript : public GroupScript
{
public:
    OllamaChatJoinScript() : GroupScript("OllamaChatJoinScript", { GROUPHOOK_ON_ADD_MEMBER }) {}
    void OnAddMember(Group* group, ObjectGuid guid) override;
};

#endif // MOD_OLLAMA_CHAT_JOINED_H
