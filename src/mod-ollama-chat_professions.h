#ifndef MOD_OLLAMA_CHAT_PROFESSIONS_H
#define MOD_OLLAMA_CHAT_PROFESSIONS_H

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// Professions (OllamaChat.Professions.*): a bot's trades, and why it has
// them, said as they are.
//
// mod-playerbots-plus has random bots earn their professions, and leans each
// to some by the kind of person it is (PlayerbotsPlus.Skills.Leaning.<Name>):
// a gold maker to herbs and ore, which sell as they are; a pet lover to
// engineering's mechanical pets, never to skinning; a roleplayer to its
// race's knack. Which kind each bot is, is this module's to say. Here:
//
// - Each personality's leaning (Professions.Leanings), told to plus as it
//   asks. Without plus in the build nobody is leaned anywhere.
// - A bot whose two professions are a pair its leaning chose is told why, as
//   it stands (Professions.Reason.<Leaning>.<How>): in a /say exchange about
//   professions (mingle's [prof]), and in a reply where the talk is of
//   professions (Professions.Words), which is told what its professions are
//   too: asked "what profs u got?", a bot made its answer up. A pair its
//   leaning did not choose is given no reason, which would be made up.
// - A bot whose personality is all about gold (Professions.GoldLeanings) is
//   now and then told its real gold for a line nobody asked for
//   (Professions.GoldChance): "a little smug about your bank", it boasted of
//   a bank it did not have. A reply is told its gold already
//   (ChatExtraInfoTemplate's {bot_gold}).
//
// World thread only.
// --------------------------------------------------------------------------

void Professions_LoadConfig();   // startup and .ollama reload
void Professions_Init();         // startup: mod-playerbots-plus is told who leans which way

// The leaning `bot`'s personality is, as mod-playerbots-plus names them
// ("Profit"). Empty for none.
std::string Professions_LeaningOf(Player* bot);

// The trades `p` has, with how far along: "mining 150, skinning 120, cooking
// 75". Empty for none.
std::string Professions_Of(Player* p);

// Why `bot` has its two professions, when its leaning chose them: "herbalism
// and mining for the gold: ..." -- the same whoever it is said to. Empty
// otherwise.
std::string Professions_WhyOf(Player* bot);

// For a reply of `bot`'s where the talk (`heard`: the line answered and the
// few before it) is of professions: what its are, and why when its leaning
// chose them. Empty otherwise.
std::string Professions_PromptFor(Player* bot, const std::vector<std::string>& heard);

// For a line nobody asked for, now and then: its real gold, for a bot whose
// personality is all about gold. Empty otherwise.
std::string Professions_GoldFor(Player* bot);

struct ProfessionsStats
{
    size_t   leanings = 0;   // personalities with a leaning
    uint64_t told     = 0;   // replies told their professions
    uint64_t why      = 0;   // ...and why they have them
    uint64_t gold     = 0;   // lines nobody asked for told their gold
};
ProfessionsStats Professions_GetStats();

#endif // MOD_OLLAMA_CHAT_PROFESSIONS_H
