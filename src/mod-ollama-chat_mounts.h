#ifndef MOD_OLLAMA_CHAT_MOUNTS_H
#define MOD_OLLAMA_CHAT_MOUNTS_H

#include "ObjectGuid.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;
struct OllamaChatRequest;

// --------------------------------------------------------------------------
// Mounts as they really are when a line goes out.
//
// "nice chestnut mare that is" went out in /say about a player who had got off
// their Chestnut Mare a moment before, and a paladin on a Charger then said
// "she's a beauty, i suppose". A bot is handed someone's mount when its prompt
// is built; the line comes back seconds later -- a minute, with a queue --
// and people get on and off their mounts all the time. And a remark on
// someone's mount was anyone's to answer.
//
// So it is checked where every bot line passes, as it goes out (the
// dispatcher's delivery), whatever made the line -- a passer-by's remark, two
// bots chatting, ambient chatter, a crowd, a conversation, city life:
//
//   * a line in /say, /yell, a party or a raid that remarks on a mount --
//     names one ("chestnut mare"), or "nice mount", "your horse", "that
//     kodo" -- goes out only while someone within Mounts.SightYards of the
//     bot, in its sight, is riding one that fits: one by that name, of that
//     kind, or any for "nice mount"; when it says "you", the one it is said
//     to; its own, when it says "my". Talk of mounts in general is left
//     alone: "saving for my epic mount", a premise about mounts, a person's
//     own question about them.
//   * once a remark on someone's mount is out, for Mounts.AnswerSeconds only
//     its rider answers it, and nobody else nearby starts something nobody
//     asked for: the next line from anyone else would answer for a mount that
//     is not theirs.
//
// World thread only.
// --------------------------------------------------------------------------

enum class MountLineCheck : uint8_t
{
    Ok,          // nothing about a mount here, or about one being ridden in sight
    NotThere,    // a remark on a mount nobody in sight is riding now
    NotTheirs,   // an answer to a remark on someone else's mount
    Held,        // something nobody asked for, right after a remark on someone's mount
};

struct MountLineVerdict
{
    MountLineCheck          check = MountLineCheck::Ok;
    std::string             what;     // what it talked of: "a chestnut mare", "a horse", "a mount"
    std::vector<ObjectGuid> riders;   // a remark that fits: whose mounts it fits, the speaker's own aside
};

// A line about to go out: `text` as it will be said, `origin` the line it
// answers (empty: none). Ok for anything but /say, /yell, a party or a raid.
MountLineVerdict Mounts_CheckLine(Player* bot, const OllamaChatRequest& request, const std::string& text,
                                  const std::string& origin);

// It went out (`plain`: as people read it): a remark on someone's mount is
// theirs to answer for a while, and their mount is not handed to another bot
// to remark on (Pets.MountRemarkMinutes).
void Mounts_NoteDelivered(Player* bot, const OllamaChatRequest& request, const std::string& plain,
                          const MountLineVerdict& verdict);

// Whose mount `line`, which `bot` has just said, remarked on (empty: it was
// no such remark): a crowd answering it is only them.
std::vector<ObjectGuid> Mounts_RemarkedOn(Player* bot, const std::string& line);

// Does the text talk of mounts at all: "mount", "a mare", "chestnut mare".
bool Mounts_TalksOf(const std::string& text);

// A mount's kind, as talk names it: "horse" for a Chestnut Mare or a Charger
// (Mounts.Kinds). Empty: none known.
std::string Mounts_KindOf(const std::string& mountName);

// ...and a word's: "mare" -> "horse". Empty: not a kind of mount.
std::string Mounts_KindOfWord(const std::string& word);

struct MountStats
{
    uint64_t notThere  = 0;   // remarks on a mount nobody in sight was riding, not said
    uint64_t notTheirs = 0;   // answers to a remark on someone else's mount, not said
    uint64_t held      = 0;   // lines nobody asked for, held back after a remark on someone's mount
    uint64_t remarks   = 0;   // remarks on someone's mount that went out
    uint64_t retried   = 0;   // lines owed to someone, asked again without the mount
    uint32_t names     = 0;   // the game's mounts known by name
};
MountStats Mounts_GetStats();
void       Mounts_NoteRetried();

#endif // MOD_OLLAMA_CHAT_MOUNTS_H
