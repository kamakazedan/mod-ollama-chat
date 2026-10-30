#ifndef MOD_OLLAMA_CHAT_NEEDS_H
#define MOD_OLLAMA_CHAT_NEEDS_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Asks with a real reason behind them.
//
// Real people in General ask for a Hogger group, where the potions are sold
// in Stormwind, a few silver for a new player, or give away loot they have no
// use for. Bots used to ask too, about nothing in particular -- and pressed
// on why, had no answer. Now and then an ambient line is one of these, read
// off the bot itself:
//
//   quest    an elite, group or dungeon quest in its log that it is the right
//            level for: out in the world one for that zone, in a city any;
//   errand   in a city, a "where's the ...?" it really needs: a repair with
//            gear nearly broken, a vendor or the auction house with bags
//            full, potions when it has none, the riding trainer at 20 with
//            no riding, its profession's trainer at a skill cap;
//   spare    something it carries and has no use for, linked, free to
//            whoever wants it (mod-ollama-chat_bags: "me!" claims it);
//   short    a low-level bot with hardly any money, asking for a few silver.
//
// What it asked and why is remembered for a while (Needs.RememberMinutes),
// and every reply prompt of that bot in the meantime carries the reason, so
// "why do you need Hogger?" gets the real answer.
//
// World thread only.
// --------------------------------------------------------------------------

enum class NeedKind : uint8_t
{
    Quest,
    Errand,
    Spare,
    Short,
    Count
};

struct BotNeed
{
    NeedKind    kind = NeedKind::Quest;
    std::string key;              // what it is about: "quest:176", "errand:repair", "spare:2589"
    std::string ask;              // for the line's prompt: what to ask or offer, and why
    std::string reason;           // for its replies afterwards: why it asked
    uint32_t    itemEntry = 0;    // spare: what it offers
    bool        party = false;    // fits party chat too (a quest, a spare thing)
};

// One thing this bot could ask for or offer right now. False when there is
// nothing, or it asked lately, or someone in its zone just asked the same
// kind of thing. `hub`: it is in a city.
bool Needs_Pick(Player* bot, bool hub, BotNeed& out);

// The ask is going out: remember why, and hold off the next one.
void Needs_NoteAsked(Player* bot, const BotNeed& need);

// For a reply prompt: why it asked what it asked lately
// (OllamaChat.Needs.ReasonText), or empty.
std::string Needs_PromptFor(Player* bot);

void Needs_Update();   // maintenance tick

struct NeedsStats
{
    uint64_t asked[static_cast<size_t>(NeedKind::Count)] = {};
    uint64_t picked = 0;   // tries that found something to ask
    uint64_t none   = 0;   // tries that found nothing
};
NeedsStats Needs_GetStats();

#endif // MOD_OLLAMA_CHAT_NEEDS_H
