#include "mod-ollama-chat_mountstext.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

// CrowdText's word helpers (mod-ollama-chat_crowd), declared here so this
// file needs nothing of the game.
namespace CrowdText
{
    std::vector<std::string> Words(const std::string& text);
    bool ContainsPhrase(const std::vector<std::string>& words, const std::vector<std::vector<std::string>>& phrases);
}

namespace MountsText
{
    namespace
    {
        bool OneOf(const std::string& word, std::initializer_list<const char*> list)
        {
            return std::any_of(list.begin(), list.end(), [&word](const char* w) { return word == w; });
        }

        bool Contains(const Phrases* phrases, const std::vector<std::string>& words)
        {
            return phrases && CrowdText::ContainsPhrase(words, *phrases);
        }
    }

    std::string Join(const std::vector<std::string>& words)
    {
        std::string out;
        for (const std::string& w : words)
            out += (out.empty() ? "" : " ") + w;
        return out;
    }

    bool HasRun(const std::vector<std::string>& words, const std::vector<std::string>& run)
    {
        return !run.empty() && run.size() <= words.size() &&
               std::search(words.begin(), words.end(), run.begin(), run.end()) != words.end();
    }

    std::vector<std::string> NameWords(const std::string& mountName)
    {
        std::vector<std::string> words = CrowdText::Words(mountName);
        if (words.size() > 5)
            return {};
        size_t own = 0;
        for (const std::string& w : words)
        {
            if (std::any_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) ||
                OneOf(w, { "dnd", "test", "unused", "deprecated", "copy", "qa", "zz", "zzold", "npc", "visual" }))
                return {};
            if (!OneOf(w, { "mount", "riding", "summon", "the", "of", "a", "an" }))
                ++own;
        }
        return own >= 2 ? words : std::vector<std::string>();
    }

    std::string KindOfWord(const std::string& word, const KindMap& kinds)
    {
        auto it = kinds.find(word);
        return it == kinds.end() ? std::string() : it->second;
    }

    std::string KindOf(const std::vector<std::string>& nameWords, const KindMap& kinds)
    {
        for (auto it = nameWords.rbegin(); it != nameWords.rend(); ++it)
            if (std::string kind = KindOfWord(*it, kinds); !kind.empty())
                return kind;
        return {};
    }

    Talk TalkOf(const std::string& text, const Lists& lists)
    {
        Talk t;
        const std::vector<std::string> words = CrowdText::Words(text);
        if (words.empty())
            return t;

        // The game's mounts by name, the longest named being what it called it.
        size_t longest = 0;
        if (lists.names)
            for (const std::vector<std::string>& name : *lists.names)
                if (HasRun(words, name))
                {
                    t.names.push_back(&name);
                    if (name.size() > longest)
                    {
                        longest = name.size();
                        t.said  = Join(name);
                    }
                }

        std::string kindSaid;
        for (const std::string& w : words)
        {
            if (lists.kinds)
                if (const std::string kind = KindOfWord(w, *lists.kinds); !kind.empty())
                {
                    t.kinds.insert(kind);
                    if (kindSaid.empty())
                        kindSaid = w;
                }
            if (OneOf(w, { "you", "your", "ur", "u", "yours", "youre", "ya", "yer", "yourself", "urs", "yall" }))
                t.second = true;
            if (OneOf(w, { "my", "mine", "myself" }))
                t.first = true;
        }
        t.generic  = Contains(lists.words, words);
        t.remark   = Contains(lists.remarkWords, words);
        t.excluded = Contains(lists.notRemarks, words);
        if (t.said.empty())
            t.said = !kindSaid.empty() ? kindSaid : std::string(t.generic ? "mount" : "");
        return t;
    }

    bool IsRemark(const Talk& talk)
    {
        if (!talk.Any() || talk.excluded)
            return false;
        return !talk.names.empty() || talk.remark || talk.second;
    }

    bool GeneralTalk(const std::string& basis, const Lists& lists)
    {
        if (basis.empty())
            return false;
        const std::vector<std::string> words = CrowdText::Words(basis);
        static const Phrases kRiding = { { "riding" }, { "mounted" }, { "rides" }, { "on", "their", "mount" } };
        if (CrowdText::ContainsPhrase(words, kRiding))
            return false;
        return TalkOf(basis, lists).Any() || Contains(lists.mountWords, words);
    }

    bool Fits(const Talk& talk, const std::vector<std::string>& riderWords, const std::string& riderKind)
    {
        if (!talk.names.empty())
        {
            for (const std::vector<std::string>* name : talk.names)
                if (HasRun(riderWords, *name) || (riderWords.size() >= 2 && HasRun(*name, riderWords)))
                    return true;
            return false;
        }
        if (!talk.kinds.empty())
            return !riderKind.empty() && talk.kinds.contains(riderKind);
        return true;
    }

    bool OnlyPetKinds(const Talk& talk, const std::vector<std::string>& petKinds)
    {
        return talk.names.empty() && !talk.generic && !talk.kinds.empty() &&
               std::all_of(talk.kinds.begin(), talk.kinds.end(), [&petKinds](const std::string& k)
                           { return std::find(petKinds.begin(), petKinds.end(), k) != petKinds.end(); });
    }
}
