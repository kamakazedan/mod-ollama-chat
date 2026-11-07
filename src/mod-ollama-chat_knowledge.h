#ifndef MOD_OLLAMA_CHAT_KNOWLEDGE_H
#define MOD_OLLAMA_CHAT_KNOWLEDGE_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------------------------------------
// Where things are, and where to get them: from the game's own data.
//
// "anyone know where i get Rune of Teleportation" had only the model's memory
// of WoW to go on: it sent the asker to the auction house, then to kill a
// dragon, and every answer after the first agreed with it. "where is the
// engineering trainer in ironforge" got "near the entrance by the forge".
// Now a line asking where something is or how to get it
// (Knowledge.QuestionWords) has the answer put in the reply prompt:
//
//   * An item -- linked, in brackets or just named: whether vendors sell it,
//     and if so how widely, what kind of vendor most of them are
//     ("Reagents"), the nearest one to the asker and its district, and
//     whether it costs tokens rather than gold; which professions make it;
//     that nothing sells or makes it, so it is looted, gathered or earned;
//     that it binds when picked up, so nobody can trade it to you.
//   * A place in a town or city -- a class or profession trainer, the bank,
//     the auction house, the inn, the flight master and the rest: what the
//     guards there say when asked for directions (the gossip menus and texts
//     behind their "What do you need directions to?"), and what they mark on
//     the minimap. A town or city named in the line ("in ironforge", "in
//     goldshire") decides whose guards; else the guards nearest the asker
//     (Knowledge.GuardRange). A line naming somewhere with no such guards
//     gets nothing rather than another town's answer. "where are the class
//     trainers" gets that the guards direct you to each. Knowledge.Synonyms
//     covers "ah", "fp", "lw", "sw" and the like.
//   * For Knowledge.FollowSeconds after such a question, a reply to a line
//     from whoever asked it, or from a bot that answered it, gets the same
//     answer: a wrong one can be put right.
//
// Each bot is told it is right and to answer from it, in as many words as
// its style takes (Knowledge.AnswerText). The styles in
// Knowledge.WrongOnPurpose -- the keyboard warrior -- are told the right
// answer too, and to say something else with a straight face
// (Knowledge.WrongText).
//
// World thread only. Built once, on the first question: every guard's
// directions, every vendor's stock and every spell that makes an item. Zones
// come from the world map's own rectangles; a district ("Trade District")
// only where the map around the spot is loaded already -- nothing is loaded
// for this.
// --------------------------------------------------------------------------

// For a reply prompt of `bot` answering `speaker`'s `msg`: the answer, when
// the line asks where something is or how to get it, or comes a moment
// later from whoever asked such a question or answered it. Empty otherwise.
std::string Knowledge_PromptFor(Player* bot, Player* speaker, const std::string& msg);

// Does the line ask where something is or how to get it?
bool Knowledge_IsQuestion(const std::string& msg);

struct KnowledgeStats
{
    uint64_t items     = 0;   // reply prompts told how to get an item
    uint64_t places    = 0;   // ...where a place is, as the guards tell it
    uint64_t headings  = 0;   // ...that the guards direct you to each trainer
    uint64_t followUps = 0;   // ...the answer to a question a line or two back
    uint64_t wrong     = 0;   // ...and to get it wrong on purpose
    uint32_t menus     = 0;   // guard direction menus known
    uint32_t sold      = 0;   // items some vendor in the world sells
};
KnowledgeStats Knowledge_GetStats();

#endif // MOD_OLLAMA_CHAT_KNOWLEDGE_H
