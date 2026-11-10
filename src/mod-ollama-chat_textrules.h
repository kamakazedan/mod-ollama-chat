#ifndef MOD_OLLAMA_CHAT_TEXTRULES_H
#define MOD_OLLAMA_CHAT_TEXTRULES_H

#include <cstddef>
#include <cstdint>
#include <random>
#include <functional>
#include <string>
#include <unordered_set>
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
    // An insult (one-word entries of `insults`, a trailing * matching any
    // ending) where it is thrown at someone -- "you knob", "u absolute
    // muppet", "nah just chilling here knob", "knob, i said no" -- taken out
    // with the "you" and the "absolute" that went with it. Not where it is
    // talked about: "dont be a knob", "what a knob", "that pillock in trade".
    // Returns what is left, tidied; empty when that was all the line said.
    std::string StripInsults(const std::string& text, const std::vector<std::vector<std::string>>& insults);

    std::string StripVocatives(const std::string& text,
                               const std::vector<std::vector<std::string>>& vocatives);

    // "far too quiet for a Tuesday" -- on a Saturday. A weekday said as being
    // today ("for a tuesday", "on a tuesday", "its tuesday", "it is tuesday",
    // "happy tuesday", "today is tuesday", "todays tuesday") becomes `today`
    // (0 Sunday .. 6 Saturday), in the same case. "on tuesday", "next
    // tuesday", "last tuesday" and "this tuesday" are other days: left alone.
    std::string FixWeekday(const std::string& text, int today);

    // ---- what a quote is from -------------------------------------------

    // Films, shows and characters a line might be put down to, and the words
    // that say a line is about watching them at all.
    struct QuoteSources
    {
        std::unordered_set<std::string> names;    // NormalizeSourceName: "toy story", "the godfather"
        std::unordered_set<std::string> joined;   // the same without spaces, for "#toystory"
        std::vector<std::vector<std::string>> filmWords;   // CrowdText phrases
    };
    QuoteSources ParseQuoteSources(const std::string& names, const std::string& filmWords);

    // "Wayne's World", "Bill & Ted", "E.T." -> "waynes world", "bill and ted",
    // "e t": lowercase letters and digits in single-spaced words.
    std::string NormalizeSourceName(const std::string& name);

    // "to infinity and beyond. toy story." -> "to infinity and beyond." What a
    // line is from, named at its end after the line itself -- ". lotr",
    // " - Toy Story", " (The Godfather, 1972)", ", from Shrek", " #avengers".
    // Left as it is when that would leave less than two words, or when the
    // line, or `answering` (what it replies to), is about films and shows at
    // all: asked "what's your favourite film?", "easy. jaws" is the answer.
    std::string StripQuoteSource(const std::string& text, const QuoteSources& sources,
                                 const std::string& answering);

    // ---- a /say exchange between bots (mod-ollama-chat_mingle) -----------

    struct ScriptLine
    {
        size_t      speaker = 0;   // index into the names it was read with
        std::string text;          // what they type; empty for an emote on its own
        std::string emote;         // "laugh", "waves" as written; empty for none
    };

    // An exchange written one message per line as "Name: what they type", for
    // the `names` in it (case does not matter). "**Name:**", "- Name:",
    // "[Name]:" and "Name says:" read as "Name:"; anything else -- a heading,
    // narration, someone not in it -- is passed over. An emote at a line's
    // start or end ("/wave", "*laughs*", "(grins)", "[emote:nod]") comes out
    // on its own when `isEmote` knows it, and quotation marks round the
    // words go; an action in asterisks never stays in the words. At most
    // `maxLines`.
    std::vector<ScriptLine> ParseScript(const std::string& text, const std::vector<std::string>& names,
                                        size_t maxLines, const std::function<bool(const std::string&)>& isEmote);

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
    //     one outside it ("done, wait") makes the line a pause. One that is a
    //     start too ("im ready now", "good to go") is a start.
    // A pause wins over a start. Lines starting "dc" or "dungeon clear" are left
    // to mod-dungeon-clear. A word typed long for effect counts as itself
    // ("letsgoooo", "leeggoooo", "readyyy"): a line that matches nothing is
    // tried again with every run of one letter cut to one, the phrases too.
    PartyIntent MatchPartyIntent(const std::string& cleanText, const PartyRules& rules);

    // `word` with every run of one letter cut to one: "leeggoooo" -> "lego".
    std::string SqueezeLetters(const std::string& word);

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
        Grind,     // "go grind", "kill stuff", "farm some mobs"
        Resume,    // "carry on", "do your thing", "ill follow you": back to what it was doing
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
        std::vector<std::vector<std::string>> subjects;      // "ill", "gonna", "they": someone else does it
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
    // "no need to wait here"), conditional ("if it runs, kill it"), the
    // speaker's own plan or someone else's doing ("ill wait here", "gonna
    // hearth", "they attack"; asking for buffs or a trade is still asking: "i
    // need buffs"), or a line whose clauses tell the bots two different things.
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

    // A premise, or a pool's value, may start with a level tag, "[70+]" or
    // "[10-39]", and a side, "[horde]" or "[alliance]", in either order: a
    // Horde player does not go AFK in Stormwind, nor wonder what is down the
    // Deeprun Tram. Strips both and says whether each fits: `level`, and
    // `side` ("horde" or "alliance"; empty fits either). Without a tag it
    // fits every level and both sides.
    struct PremiseTags
    {
        bool levelFits = true;
        bool sideFits  = true;
    };
    PremiseTags TakeLevelAndSide(std::string& premise, uint32_t level, const std::string& side);

    // Both at once.
    bool PremiseFits(std::string& premise, uint32_t level, const std::string& side = std::string());

    // "alliance" or "horde": a side as the tags above write it.
    std::string SideTag(bool alliance);

    // A premise may also need something to be there: "[mount] one of them
    // admires the mount the other is riding" is for when one of them is
    // riding one. Takes those tags (lowercase words) off the front of
    // `premise` and returns them -- a side among them, "[horde]", is one
    // more thing that has to be so; a level tag, in any order with them,
    // stays for PremiseFits.
    std::vector<std::string> TakePremiseTags(std::string& premise);

    // --------------------------------------------------------------------------
    // Mounts, pets and companions, each as what it is.
    //
    // "with their Wolf Howler" next to talk of someone's mount got "wolf
    // howler is a solid choice": a hunter's pet taken for a mount. Each is
    // named as what it is, every time.
    // --------------------------------------------------------------------------

    struct Animals
    {
        std::string mount;       // what they are riding ("Swift Brown Horse"); empty: on foot
        std::string petKind;     // "hunter pet", "demon", "ghoul", "water elemental", "pet"; empty: none out
        std::string petFamily;   // "wolf", "imp"; empty when it would only say the kind again
        std::string petName;     // "Howler"; empty when it is only the kind ("Water Elemental")
        std::string companion;   // the companion pet following them ("Mechanical Squirrel")
    };

    // About someone else: "riding their mount, a Swift Brown Horse", "with
    // their hunter pet, a wolf called Howler", "with their companion pet, a
    // Mechanical Squirrel, following them". With `own`, to the one who has
    // them: "your mount, a Swift Brown Horse, which you are riding", "your
    // hunter pet, a wolf called Howler", ... With `onFoot` and no mount, that
    // is said too ("no mount out: you are on foot"). Mount first.
    std::vector<std::string> AnimalPhrases(const Animals& animals, bool own, bool onFoot);

    // "a wolf", "an imp", "an Acherus Deathcharger".
    std::string WithArticle(const std::string& noun);

    // A mount as people call it, from the spell that summons it: "Summon
    // Felsteed" is a Felsteed.
    std::string MountFromSpell(const std::string& spellName);

    // A canned line ("gz Gotlun", "ty all", "my bad") typed the way a
    // personality types: "proper" ("Congratulations Gotlun."), "caps" ("GZ
    // GOTLUN"), "lower" ("gz gotlun", no full stop), "flat" (no exclamation
    // marks), "short" ("congrats" is "gz", "thanks" is "ty"), "excited"
    // ("gz gotlun!"). Any other style leaves it as it is.
    std::string StyleCanned(const std::string& line, const std::string& style);

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
        uint32_t    id = 0;   // the first number after the type ("item:2589:..." -> 2589); 0 when none
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

    // Right after an offer, does this ask (or yes) mean the thing offered?
    // Every word has to be part of an ask (`askPhrases`: "can you trade", "i
    // want"), a yes (`yesPhrases`), a word pointing at it ("it", "those",
    // "the stuff") or one that says nothing ("please", "now"), and there has
    // to be an ask or a pointer: "trade it", "can you trade me?", "i want
    // those". Not "give me a sec", "i want to go", "can i get in".
    bool ClaimsOffer(const std::vector<std::string>& words, bool ask,
                     const std::vector<std::vector<std::string>>& askPhrases,
                     const std::vector<std::vector<std::string>>& yesPhrases);

    // Nothing but a congratulation: one of `congrats` ("gz", "grats"), and
    // every other word one of those, `filler` ("on", "the", "ding"), `ack`
    // ("lol", "ty"), a number ("20") or a name (`isName`, which may note
    // whose): "gz", "grats on 20 Edos!", "congrats on the ding mate".
    bool IsOnlyCongrats(const std::vector<std::string>& words,
                        const std::vector<std::vector<std::string>>& congrats,
                        const std::vector<std::vector<std::string>>& filler,
                        const std::vector<std::vector<std::string>>& ack,
                        const std::function<bool(const std::string&)>& isName);

    // The whole line is a yes -- one of `yes` ("yep", "go for it", "sure"),
    // with nothing but `fillers` ("ok", "mate", "lol") around it: an answer
    // to "everyone ready?". "yes but wait", "yeah 2 min" and "not yet" are not.
    bool IsBareYes(const std::vector<std::string>& words, const std::vector<std::vector<std::string>>& yes,
                   const std::vector<std::vector<std::string>>& fillers);

    // One of `phrases` is in `words` where none of `unless` runs into it:
    // "thank you, keep it up" holds "you keep it", but it is part of "thank
    // you" and "keep it up"; "dont take it out" holds "take it out". One of
    // `unless` wholly inside the phrase does not count against it ("no
    // thanks" with "thanks" among `unless`).
    bool HasPhraseUnless(const std::vector<std::string>& words, const std::vector<std::vector<std::string>>& phrases,
                         const std::vector<std::vector<std::string>>& unless);

    // `text` with any word one slip off one of `keys` -- a letter wrong,
    // missing, extra or swapped: "dingeon", "dungen", "dugneon" -- written as
    // the key ("dungeon"). Only keys of six letters or more, where one slip
    // off is not some other word. Everything else is left as it is.
    std::string FixKeyTypos(const std::string& text, const std::vector<std::string>& keys);

    // ---- picking ----------------------------------------------------------

    // Replace {name} in a canned line; lowercase it about half the time, the
    // way players type names.
    std::string FormatCannedLine(const std::string& line, const std::string& name, std::mt19937& rng);

    // `text` with every "{~a|b|c}" replaced by one of its alternatives, picked
    // at random: a prompt that says the same thing a different way each time,
    // so the line the model writes from it is not the same each time either.
    // An alternative may be empty ("{~|really }"), and may hold a "{slot}" or
    // another "{~...}" ("{~a joke about {creature}|{~a|one} rumour}"). An
    // unclosed "{~" is left as it is. Run it before any fmt formatting.
    std::string ExpandChoices(const std::string& text, std::mt19937& rng);

    // `text` split at every "|" that is not inside braces, each part trimmed,
    // empty parts dropped: a list of lines where a line may hold "{~a|b}".
    std::vector<std::string> SplitBars(const std::string& text);

    // A template's "{slot}"s filled in: `pick(pool, taken)` gives a value from
    // the pool named by the slot without a number on the end ("{class2}" is
    // pool "class"), not one of `taken` (what this template has used so far),
    // or "" when it has none. Empty when any slot cannot be filled, or a "{"
    // is not closed. Run ExpandChoices first.
    std::string FillSlots(const std::string& templ,
                          const std::function<std::string(const std::string&, const std::vector<std::string>&)>& pick);

    // ---- quest text ---------------------------------------------------------

    // Quest text the way the game shows it to someone: $N their name, $C their
    // class, $R their race (lowercase $n/$c/$r lowercased), $B a line break
    // (a space here), $Gmale:female; the one for their gender. Whitespace
    // folded, and at most `maxLen` bytes (0: no limit), cut after a sentence
    // where one ends late enough, else at a word with "..." after it.
    std::string QuestText(const std::string& text, const std::string& name, const std::string& cls,
                          const std::string& race, bool female, size_t maxLen);

    // What a quest asks, plainly. "Kill 15 Blackrock Champions and Guard
    // Howe in Lakeshire will reward you." -- the quest log's own sentence --
    // had a bot tell someone to kill the champions and Guard Howe. The goals
    // as the game lists them, each on its own, and who it is handed in to,
    // cannot be read that way.
    struct QuestGoal
    {
        std::string verb;        // "kill", "collect", "use"; empty when `what` is the game's own words for it
        std::string what;        // "Blackrock Champion", "Gnoll Paw", "Lakeshire Bridge inspected"
        uint32_t    count = 0;
    };

    // "Blackrock Champions", "Wolves", "Harpies", "Footmen" for more than
    // one; `name` as it is for one. A name that already ends in s stays.
    std::string PluralName(const std::string& name, uint32_t count);

    // "kill 15 Blackrock Champions, then hand it in to Guard Howe". With no
    // goals, `fallback` (the quest log's own text) -- and who it is handed
    // in to after it, unless the text names them already.
    std::string QuestPlainly(const std::vector<QuestGoal>& goals, const std::string& ender,
                             const std::string& fallback);

    // ---- wanting a kind of item ----------------------------------------------

    // A kind of item people say they need: `key` names it ("wand",
    // "shoulders"), `phrase` says it in a sentence ("a wand", "new
    // shoulders"), `words` are what people call it (lowercase phrases).
    struct ItemKindWords
    {
        std::string key;
        std::string phrase;
        std::vector<std::vector<std::string>> words;
    };

    // Every kind there is, the narrower first ("fist weapon" before
    // "weapon").
    const std::vector<ItemKindWords>& ItemKinds();

    // The kind of item a line says its speaker is after -- "i still need a
    // fukin wand", "anyone know where to get a good shield", "lf cloak" -- or
    // null. One of `wanting` ("need", "looking for", "where") comes before
    // what they call it, at most five words before, with nothing like "got"
    // or "dropped" in between; and nobody else is asked ("do you need a
    // wand?", "anyone need a wand?") or told no ("dont need a wand").
    const ItemKindWords* FindItemWant(const std::vector<std::string>& words,
                                      const std::vector<std::vector<std::string>>& wanting);

    // ---- emoticons -----------------------------------------------------------

    // A text emoticon standing as a word of its own -- ":)", ":D", "xD",
    // ">.<", "<3" -- one of `known` (case does not matter, nor a full stop or
    // comma right after it). `begin`/`end` cover it in the text.
    struct Emoticon
    {
        size_t      begin = 0;
        size_t      end   = 0;
        std::string token;
    };
    std::vector<Emoticon> FindEmoticons(const std::string& text, const std::vector<std::string>& known);

    // `text` without the emoticons `drop` lists (indexes into `found`), spaces
    // and stray commas tidied. Empty when nothing with a letter or digit is
    // left.
    std::string RemoveEmoticons(const std::string& text, const std::vector<Emoticon>& found,
                                const std::vector<size_t>& drop);

    // ---- echoes --------------------------------------------------------------

    // Every run of `n` words in `text` -- lowercase, punctuation dropped, joined
    // by single spaces -- that has at least one word not in `common`, each
    // once: the phrases a line is made of, as opposed to its glue ("do you want
    // to"). Fewer than `n` words: none. A name in brackets ("[Look to the
    // Stars]") is game data, not wording: no run includes any of it.
    std::vector<std::string> ContentGrams(const std::string& text, size_t n, const std::vector<std::string>& common);

    // ---- prices --------------------------------------------------------------

    // `text` with every "[Name]" taken out (one space left where it was): what
    // a player wrote around the things they linked, so "[Gold Ring]" is not a
    // price and "[Bag of Spare Parts]" not an offer.
    std::string WithoutBracketNames(const std::string& text);

    // The first price in `text`, in copper: "5g", "5 gold", "1g 50s", "1g50s",
    // "50 silver", "75c", "2.5g". A number with no coin after it ("lvl 20",
    // "x5") is no price, nor a coin letter that starts a word ("5 gnolls").
    // 0 when there is none. Capped at 1,000,000 gold.
    uint32_t ParsePrice(const std::string& text);

    // A price as people write it: "1g 50s", "35s", "80c"; "0c" for nothing.
    std::string MoneyWords(uint32_t copper);
}

#endif // MOD_OLLAMA_CHAT_TEXTRULES_H
