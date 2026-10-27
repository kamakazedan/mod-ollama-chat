#ifndef MOD_OLLAMA_CHAT_TEXTRULES_H
#define MOD_OLLAMA_CHAT_TEXTRULES_H

#include <cstddef>
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

    // ---- calling someone "kid" -------------------------------------------

    // Drop a word from `vocatives` (one-word entries, e.g. "kid") wherever it
    // addresses someone: ending a sentence ("just cave mobs kid", "nice try,
    // kid."), before a comma ("listen kid, ..."), or opening the line ("kid
    // you need ..."). Not where it is what is talked about: "the kid", "a kid",
    // "my kid", "two kids", "that kid is ...". Returns what is left, tidied;
    // empty when the line was only that word.
    std::string StripVocatives(const std::string& text,
                               const std::vector<std::vector<std::string>>& vocatives);

    // ---- muted bot lines ----------------------------------------------------

    // Does `text` match `pattern`? Case-insensitive, the whole text; '*'
    // matches any run of characters, so "Using |c*" is "Using " followed by an
    // item link, and "Staying" is that word alone.
    bool GlobMatch(const std::string& pattern, const std::string& text);

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
        Pause,   // mb / wait / stop / brb: hold here
        Resume   // ok im done / full / done drinking: carry on -- only while paused
    };

    struct PartyRules
    {
        std::vector<CommandPhrase> start;
        std::vector<CommandPhrase> pause;
        std::vector<CommandPhrase> resume;                 // "im done", "full": only means go on after a pause
        std::vector<CommandPhrase> cancel;                 // "dont stop", "no need to wait": not a pause
        std::vector<std::vector<std::string>> negations;   // "not ready", "i dont need mana": neither
        std::vector<std::vector<std::string>> questions;   // anywhere: "wait what", "where do we go"
        std::vector<std::vector<std::string>> openers;     // first word: "you oom", "anyone need mana"
        std::vector<std::vector<std::string>> notYet;      // block a start: "almost ready", "ready in 5"
        std::vector<std::vector<std::string>> resumeFillers;   // "ok", "thanks", "now": go with a resume
        uint32_t maxWords = 10;
        uint32_t resumeMaxWords = 6;                       // "ok im done" yes, a story that says "done" no
        uint32_t resumeMaxStray = 1;                       // words that are neither: "i have done this before" no
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
    //     not yet ("almost ready", "ready in 5");
    //   * a resume -- "ok im done", "full", "done drinking" -- in a short line,
    //     unless negated ("not done") or not yet ("almost done"). Only the
    //     caller knows whether the run is paused, which is the only time it
    //     means anything. A pause word inside it ("drinking") is part of it;
    //     one outside it ("done, wait") makes the line a pause.
    // A pause wins over a start. Lines starting "dc" or "dungeon clear" are left
    // to mod-dungeon-clear.
    PartyIntent MatchPartyIntent(const std::string& cleanText, const PartyRules& rules);

    // A line starting "dc" or "dungeon clear": one of mod-dungeon-clear's own
    // chat commands ("dc on", "dc pause").
    bool IsDungeonClearCommand(const std::string& cleanText);

    // ---- conjured food and water ---------------------------------------------

    enum ConjureKinds : uint8_t
    {
        CONJURE_NONE  = 0,
        CONJURE_DRINK = 1,   // water, mana, "pots"
        CONJURE_FOOD  = 2,   // food, bread
    };

    struct ConjureRules
    {
        std::vector<std::vector<std::string>> drink;      // "water", "mana pot*", "refreshment*"
        std::vector<std::vector<std::string>> food;       // "food", "bread"
        std::vector<std::vector<std::string>> pots;       // "pots": a raid call too ("pot now"), so asked outright
        std::vector<std::vector<std::string>> loose;      // "mana", "drink": only from a bot or mage spoken to
        std::vector<std::vector<std::string>> notMage;    // "health pot*", "food buff": not a mage's to give
        std::vector<std::vector<std::string>> asks;       // "give", "send", "can i have": asking for something
        std::vector<std::vector<std::string>> polite;     // "pls", "can you": asking nicely, for anything
        std::vector<std::vector<std::string>> needs;      // "need", "out of": an ask right before the item
        std::vector<std::vector<std::string>> mage;       // "mage", "mages"
        std::vector<std::vector<std::string>> negations;  // "dont", "no", "not"
        std::vector<std::vector<std::string>> haves;      // "i have", "im good": not asking
        std::vector<std::vector<std::string>> thanks;     // "ty", "thanks"
        std::vector<std::vector<std::string>> questions;  // "where", "how": asking about, not for
        std::vector<std::vector<std::string>> commands;   // first word: "t", "use", "buy": playerbots' or a vendor's
        uint32_t maxWords = 16;
    };

    struct ConjureAsk
    {
        uint8_t  kinds  = CONJURE_NONE;   // CONJURE_DRINK | CONJURE_FOOD
        uint32_t stacks = 0;              // "2 stacks" -> 2; 0 when not said
        uint32_t count  = 0;              // "40 water" -> 40; 0 when not said
        bool     mage   = false;          // "mage" up front or at the end: "mage water pls"
    };

    // Is this a line asking for conjured food or water? People ask the way
    // they do in any group: "water pls", "mage can i get food", "anyone got
    // water?", "need water", "pls send me pots", "Frostiq please can you give
    // me mana pots".
    //
    //   * a real item ("water", "food", "mana pots") needs one of: an ask or a
    //     please ("give", "can i have", "pls"), a need right before it ("need
    //     water", "out of food"), "mage" up front or at the end, a line of
    //     nothing but the item ("water?", "more water"), or -- when a bot is
    //     named or whispered -- a question mark;
    //   * "pots" alone is a raid call as often as an ask ("pot now", "pot
    //     pls"), so it needs a real ask ("send me pots"), or "mage" or a name
    //     with a please or a question;
    //   * a loose word ("mana", "drink") counts only when a bot or "mage" is
    //     addressed and it is asked for: "mage mana pls", "Frostiq mana?" --
    //     never "need mana" or "need a drink", which are mana breaks;
    //   * not when it is negated before the item ("dont need water"), had ("i
    //     have water", "im good"), a thank-you ("ty for the water"), a question
    //     about rather than for it ("where do i buy water"), or not a mage's
    //     ("health pots", "food buff", "water walking").
    //
    // A line whose first words are a command (playerbots' "t water", "use
    // water", "b water", or "buy water") is never matched: playerbots acts on
    // those itself. `names` are words to leave out first (the bots in the
    // group), and `named` says one of them was named, or the line was
    // whispered to one.
    ConjureAsk MatchConjureAsk(const std::string& cleanText, const ConjureRules& rules,
                               bool named, const std::vector<std::string>& names = {});

    // ---- telling bots what to do ---------------------------------------------

    enum class BotIntent : uint8_t
    {
        None,
        Stay,      // "stay here", "wait here", "stay back", "dont move"
        Follow,    // "follow me", "come with me", "come here", "on me"
        Attack,    // "attack my target", "kill that", "get him"
        Flee,      // "run away", "flee", "retreat", "fall back"
        Revive,    // to the dead: "release", "respawn", "run back", "res"
        Trade,     // "trade me", "come trade me", "open a trade"
        Hearth,    // "hearth", "use your hearthstone", "hs out"
        Unstuck,   // "you're stuck", "use unstuck"
        Buff,      // "buff us", "buffs pls", "rebuff"
        Pull,      // "pull", "go ahead and pull"
        Leave,     // "leave the group", "you can leave now"
        Count
    };

    const char* BotIntentName(BotIntent intent);

    struct BotCommandRules
    {
        // Indexed by BotIntent; [None] is unused. "=run" matches only a line
        // (or a clause of one) that is exactly "run".
        std::vector<CommandPhrase> phrases[static_cast<size_t>(BotIntent::Count)];
        std::vector<std::vector<std::string>> fillers;       // "pls", "now", "here", "a sec": go with anything
        std::vector<std::vector<std::string>> polite;        // "can you", "could you": an instruction even asked
        std::vector<std::vector<std::string>> everyone;      // "everyone", "guys", "all": to every bot
        std::vector<std::vector<std::string>> negations;     // "dont", "no need to": not an instruction
        std::vector<std::vector<std::string>> questions;     // "why", "should", "are": asking, not telling
        std::vector<std::vector<std::string>> conditions;    // "if", "unless", "once": not now
        std::vector<std::vector<std::string>> separators;    // "and", "while", "so": split a line into clauses
        uint32_t maxWords = 14;
        uint32_t maxStray = 1;      // words in the clause that are none of the above
    };

    struct BotCommand
    {
        BotIntent intent   = BotIntent::None;
        bool      everyone = false;   // said to every bot: "everyone stay here"
        bool      polite   = false;   // "can you ...", "pls"
    };

    // Is this line telling the bots to do something? People say it the way
    // they would to anyone: "stay back", "wait here", "everyone come with me",
    // "Joanni can you come trade me", "you're stuck, use unstuck", "stay here
    // while i grab the quest".
    //
    // The line is split into clauses (at punctuation and at words like "and",
    // "while", "so"). A clause is an instruction when it is one of the phrases
    // above and little else: names (`names`, left out first), fillers, "can
    // you", "everyone", and at most maxStray other words. So "stay here while
    // i loot" stays, and "we should stay here and fight" does not.
    //
    // Not an instruction: one asked rather than told ("should we wait here?",
    // "why are you staying"), negated outside the phrase ("dont stay here",
    // "no need to wait here"), conditional ("if it runs, kill it"), or a line
    // whose clauses tell the bots two different things.
    BotCommand MatchBotCommand(const std::string& cleanText, const BotCommandRules& rules,
                               const std::vector<std::string>& names = {});

    // ---- level outlook ----------------------------------------------------

    // One entry of OllamaChat.Outlook.LevelBands: "10-19: text", "80+: text"
    // or "80: text".
    struct LevelBand
    {
        uint32_t    lo = 0;
        uint32_t    hi = 0;
        std::string text;
    };
    std::vector<LevelBand> ParseLevelBands(const std::string& raw);

    // The first band that holds `level`; nullptr when none does.
    const LevelBand* FindLevelBand(const std::vector<LevelBand>& bands, uint32_t level);

    // "10-19", "80+", "80" -> [lo, hi]. False when it is not a range.
    bool ParseLevelRange(const std::string& raw, uint32_t& lo, uint32_t& hi);

    // A premise may start with a level tag, "[70+]" or "[10-39]". Strips it
    // and says whether `level` fits; a premise without one fits everyone.
    bool PremiseFits(std::string& premise, uint32_t level);

    // A phrase only players of some levels say: "[40+] repair bill",
    // "[70+] heroic". Entries without a level tag are skipped.
    struct LevelPhrase
    {
        uint32_t lo = 0;
        uint32_t hi = 0;
        std::vector<std::string> words;
    };
    std::vector<LevelPhrase> ParseLevelPhrases(const std::string& raw);

    // The first phrase in `text` that a player of `level` would not say;
    // nullptr when there is none. A phrase listed for several ranges is fine
    // in any of them, and one that `exempt` (the line being answered) also
    // has is fine at any level: answering "heroics?" with "im only 20" is.
    const LevelPhrase* FindOffLevelPhrase(const std::string& text, const std::vector<LevelPhrase>& phrases,
                                          uint32_t level, const std::string& exempt = std::string());

    // ---- stock phrases -----------------------------------------------------

    // Which entries of `phrases` (from CrowdText::ParsePhrases) appear in
    // `text`, by index -- leaving out any that `exempt` (the line being
    // answered) has too, so "no transmog in wrath lol" can answer "can you
    // transmog?".
    std::vector<size_t> FindPhraseIndexes(const std::string& text,
                                          const std::vector<std::vector<std::string>>& phrases,
                                          const std::string& exempt = std::string());

    // "taking forever": the words of a phrase joined by single spaces.
    std::string PhraseKey(const std::vector<std::string>& phrase);

    // ---- chat links ---------------------------------------------------------

    // A link as the client sends it:
    //   |cff1eff00|Hitem:2589:0:0:0:0:0:0:0:0|h[Linen Cloth]|h|r
    // `type` is what follows |H ("item", "quest", "spell"...), `name` the text
    // between the brackets, `markup` the whole link, colour and |r included.
    struct ChatLink
    {
        std::string type;
        std::string name;
        std::string markup;
    };
    std::vector<ChatLink> ExtractChatLinks(const std::string& message);

    // What a reader sees: every link becomes its "[Name]", colour codes and
    // textures go, and no '|' is left that the client could read as markup.
    // Spacing is kept as it was.
    std::string StripChatMarkup(const std::string& text);

    // Every "[Name]" in plain text: `begin`/`end` cover the brackets, `name` is
    // what is inside, trimmed. Empty, nested or over-long ones are skipped.
    struct BracketName
    {
        size_t      begin = 0;
        size_t      end   = 0;   // one past ']'
        std::string name;
    };
    std::vector<BracketName> FindBracketNames(const std::string& text);

    // Lowercase, inner runs of spaces folded: the key names are matched on.
    std::string NameKey(const std::string& name);

    // ---- picking ----------------------------------------------------------

    // Replace {name} in a canned line; lowercase it about half the time, the
    // way players type names.
    std::string FormatCannedLine(const std::string& line, const std::string& name, std::mt19937& rng);
}

#endif // MOD_OLLAMA_CHAT_TEXTRULES_H
