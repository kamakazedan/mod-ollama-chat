#ifndef MOD_OLLAMA_CHAT_TEXTRULES_H
#define MOD_OLLAMA_CHAT_TEXTRULES_H

#include <cstdint>
#include <random>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Small text rules, kept free of game types so they can be unit tested.
//
// Words and phrases are matched the way CrowdText does it: lowercase words of
// letters and digits, apostrophes dropped ("let's" -> "lets", "r?" -> "r"),
// a trailing '*' in a pattern matching any word that starts with the rest.
// --------------------------------------------------------------------------

namespace TextRules
{
    // ---- level-ups ------------------------------------------------------

    // True when a reaction to SOMEONE ELSE's level-up announces a level-up of
    // its own: "ding", "DING!", "ding 23", "ding ding". Only the player who
    // levelled says ding. A ding that is clearly about them is fine: "nice
    // ding", "gz on the ding", "ur ding".
    bool IsSelfDing(const std::string& text);

    // ---- assistant speak ------------------------------------------------

    // Drop every sentence that offers help like a customer-service bot ("How
    // can I help you?", "need help with something?"). `phrases` comes from
    // CrowdText::ParsePhrases. Returns what is left, trimmed; empty when the
    // whole line was that.
    std::string StripAssistantSpeak(const std::string& text,
                                    const std::vector<std::vector<std::string>>& phrases);

    // ---- party commands --------------------------------------------------

    // A phrase from a command list. "=r" in the config matches only a line
    // that is exactly "r" (or "r?"), so "r u ok" does not start a dungeon.
    struct CommandPhrase
    {
        std::vector<std::string> words;
        bool wholeLine = false;
    };
    std::vector<CommandPhrase> ParseCommandPhrases(const std::string& raw);

    enum class PartyIntent : uint8_t
    {
        None,
        Start,   // ready / lets go / ok carry on: start or resume
        Pause    // mb / wait / stop / brb: hold here
    };

    struct PartyRules
    {
        std::vector<CommandPhrase> start;
        std::vector<CommandPhrase> pause;
        std::vector<CommandPhrase> cancel;                 // "dont stop", "no need to wait": not a pause
        std::vector<std::vector<std::string>> negations;   // "not ready", "i dont need mana": neither
        std::vector<std::vector<std::string>> questions;   // anywhere: "wait what", "where do we go"
        std::vector<std::vector<std::string>> openers;     // first word: "you oom", "anyone need mana"
        std::vector<std::vector<std::string>> notYet;      // block a start: "almost ready", "ready in 5"
        uint32_t maxWords = 10;
    };

    // What a short party line asks for:
    //   * a ready check is a start: "r?", "ready?", "everyone ready?", "are we
    //     ready", "ok rdy", "lets go?";
    //   * anything else asked is talk: a '?' in the line, a question word
    //     ("wait what"), or a question opener up front ("you oom", "anyone
    //     need mana", "are you ready");
    //   * a pause -- "mb", "wait", "stop" -- unless it is called off ("dont
    //     stop") or negated ("i dont need mana", "cant wait for this boss"). A
    //     "no" up front is emphasis ("no wait"), and a negation inside the
    //     phrase itself ("dont pull") is the instruction;
    //   * a start -- "lets go", "im back" -- unless negated ("not ready") or
    //     not yet ("almost ready", "ready in 5").
    // A pause wins over a start. Lines starting "dc" or "dungeon clear" are left
    // to mod-dungeon-clear.
    PartyIntent MatchPartyIntent(const std::string& cleanText, const PartyRules& rules);

    // A line starting "dc" or "dungeon clear": one of mod-dungeon-clear's own
    // chat commands ("dc on", "dc pause").
    bool IsDungeonClearCommand(const std::string& cleanText);

    // ---- picking ----------------------------------------------------------

    // Replace {name} in a canned line; lowercase it about half the time, the
    // way players type names.
    std::string FormatCannedLine(const std::string& line, const std::string& name, std::mt19937& rng);
}

#endif // MOD_OLLAMA_CHAT_TEXTRULES_H
