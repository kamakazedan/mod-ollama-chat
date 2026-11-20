#ifndef MOD_OLLAMA_CHAT_MOUNTSTEXT_H
#define MOD_OLLAMA_CHAT_MOUNTSTEXT_H

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// --------------------------------------------------------------------------
// What a line says of mounts, and whether the mount someone rides fits it:
// the sums of mod-ollama-chat_mounts, with no game types in them, so they can
// be tested without a server. Words are CrowdText words (lowercase, letters
// and digits).
// --------------------------------------------------------------------------

namespace MountsText
{
    using Phrases = std::vector<std::vector<std::string>>;
    using KindMap = std::unordered_map<std::string, std::string>;   // "mare" -> "horse"

    struct Lists
    {
        const Phrases* names       = nullptr;   // the game's mounts by name: {chestnut, mare}
        const KindMap* kinds       = nullptr;   // Mounts.Kinds
        const Phrases* words       = nullptr;   // Mounts.Words: "mount", "ride"
        const Phrases* remarkWords = nullptr;   // Mounts.RemarkWords: "nice", "that"
        const Phrases* notRemarks  = nullptr;   // Mounts.NotRemarks: "mount up"
        const Phrases* mountWords  = nullptr;   // Pets.MountWords: talk of mounts at all
    };

    struct Talk
    {
        std::vector<const std::vector<std::string>*> names;   // the game's mounts it names (into Lists::names)
        std::set<std::string> kinds;                           // the kinds it names: "horse" for "mare"
        std::string said;                                      // what it calls it: "chestnut mare", "mare", "mount"
        bool generic  = false;   // a word for any mount (Mounts.Words)
        bool remark   = false;   // a remark word (Mounts.RemarkWords)
        bool second   = false;   // "you", "your", "ur": to someone, about theirs
        bool first    = false;   // "my", "mine": about its own
        bool excluded = false;   // "mount up", "ride over" (Mounts.NotRemarks): not about one in sight

        bool Any() const { return !names.empty() || !kinds.empty() || generic; }
    };

    std::string Join(const std::vector<std::string>& words);

    // Is `run` in `words`, word for word, in a row?
    bool HasRun(const std::vector<std::string>& words, const std::vector<std::string>& run);

    // A mount's name as people would say it as a name ("Chestnut Mare",
    // "Swift Brown Horse"): its words, when it has two of its own or more and
    // none is a developer's ("(DND)", "Test", a number). Empty for one word
    // ("Charger": a kind) or a name that is only a kind and what anyone calls
    // one ("Kodo Mount", "Riding Turtle").
    std::vector<std::string> NameWords(const std::string& mountName);

    // "mare" -> "horse"; empty when it is not a kind of mount.
    std::string KindOfWord(const std::string& word, const KindMap& kinds);

    // A mount's kind by its name's words: the last that is one ("Swift Brown
    // Horse", "Chestnut Mare", "Charger": horse). Empty when none is.
    std::string KindOf(const std::vector<std::string>& nameWords, const KindMap& kinds);

    Talk TalkOf(const std::string& text, const Lists& lists);

    // A remark on a mount in sight: one named, or a kind or "mount" with a
    // remark word or "you" ("nice horse", "your mount", "look at that kodo")
    // -- and not "i need a horse", nor "mount up".
    bool IsRemark(const Talk& talk);

    // What a line went on (`basis`: its premise, topic, what it was told)
    // talks of mounts, and of nobody riding one: "the epic mount they are
    // saving for". A bot handed someone's mount is told they are riding it
    // ("riding their mount, ...", "mounted up", "the mount the other is
    // riding").
    bool GeneralTalk(const std::string& basis, const Lists& lists);

    // Does the mount someone rides -- its name's words, its kind -- fit what
    // the line says: one named by that name ("chestnut mare" is a Chestnut
    // Mare, not any horse), one of a kind named, any for "nice mount"?
    bool Fits(const Talk& talk, const std::vector<std::string>& riderWords, const std::string& riderKind);

    // It names only kinds, and every one is one a hunter's pet can be too
    // (Mounts.PetKinds): "cute cat", "nice wolf".
    bool OnlyPetKinds(const Talk& talk, const std::vector<std::string>& petKinds);
}

#endif // MOD_OLLAMA_CHAT_MOUNTSTEXT_H
