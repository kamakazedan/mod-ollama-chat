#include "mod-ollama-chat_pets.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat-utilities.h"

#include "Creature.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace
{
    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    // "Cat", "Wind Serpent", "Imp": its kind, as the game names it; empty if none.
    std::string FamilyOf(Pet* pet)
    {
        CreatureTemplate const* t = pet->GetCreatureTemplate();
        if (!t || !t->family)
            return {};
        CreatureFamilyEntry const* f = sCreatureFamilyStore.LookupEntry(t->family);
        return (f && f->Name[0] && *f->Name[0]) ? std::string(f->Name[0]) : std::string();
    }

    // What a pet is to the one it belongs to: a hunter's is its pet, a
    // warlock's a demon, a death knight's a ghoul, a mage's a water elemental.
    std::string KindOf(Pet* pet, Player* owner)
    {
        if (pet->getPetType() == HUNTER_PET)
            return "hunter pet";
        switch (owner->getClass())
        {
            case CLASS_WARLOCK:      return "demon";
            case CLASS_DEATH_KNIGHT: return "ghoul";
            case CLASS_MAGE:         return "water elemental";
            default:                 return "pet";
        }
    }

    // What they are riding, as people call it ("Swift Brown Horse"); "Mount"
    // when the game does not say.
    std::string MountOf(Player* p)
    {
        const Unit::AuraEffectList& auras = p->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        SpellInfo const* info = auras.empty() ? nullptr : auras.front()->GetSpellInfo();
        if (!info || !info->SpellName[0] || !*info->SpellName[0])
            return "Mount";
        const std::string name = TextRules::MountFromSpell(info->SpellName[0]);
        return name.empty() ? std::string("Mount") : name;
    }

    // Words in companions' names that say nothing on their own: "great run"
    // is not about a Great Horned Owl.
    bool Plain(const std::string& w)
    {
        static const char* const kPlain[] = { "the", "and", "with", "tiny", "little", "lil", "mini", "baby", "great",
                                              "small", "big", "young", "old", "black", "white", "brown", "green",
                                              "blue", "red", "pink", "golden", "gold", "silver", "grey", "gray",
                                              "crimson", "dark", "light", "wood", "wing", "horned", "spotted",
                                              "swift", "fast", "mount", "summon", "war" };
        for (const char* p : kPlain)
            if (w == p)
                return true;
        return false;
    }

    // The words a name or a kind brings to the talk: "Bombay Cat" -> bombay, cat.
    void AddWords(std::vector<std::vector<std::string>>& phrases, const std::string& text)
    {
        for (std::string& w : CrowdText::Words(text))
            if (w.size() > 2 && !Plain(w))
                phrases.push_back({ std::move(w) });
    }
}

PetsSeen Pets_Seen(Player* who, float range)
{
    PetsSeen seen;
    if (!who)
        return seen;
    TextRules::Animals& a = seen.animals;

    if (who->IsMounted())
        a.mount = MountOf(who);

    if (Pet* pet = who->GetPet();
        pet && pet->IsInWorld() && pet->IsAlive() && pet->IsWithinDistInMap(who, range))
    {
        a.petKind = KindOf(pet, who);
        std::string family = Lower(FamilyOf(pet));
        std::string name   = pet->GetName();
        if (family == a.petKind)
            family.clear();   // a ghoul's kind is Ghoul
        if (Lower(name) == a.petKind || (!family.empty() && Lower(name) == family))
            name.clear();     // "Water Elemental", a cat still called Cat
        a.petFamily  = family;
        a.petName    = name;
        seen.petGuid = pet->GetGUID();
    }

    const ObjectGuid critterGuid = who->GetCritterGUID();
    if (!critterGuid.IsEmpty())
        if (Creature* critter = ObjectAccessor::GetCreature(*who, critterGuid);
            critter && critter->IsInWorld() && critter->IsWithinDistInMap(who, range))
        {
            a.companion        = critter->GetName();
            seen.companionGuid = critter->GetGUID();
        }
    return seen;
}

std::string Pets_StateTextFor(Player* bot, const std::string& message)
{
    if (!g_Enable || !bot || g_PetsStateText.empty() || message.empty())
        return {};

    const PetsSeen seen = Pets_Seen(bot);
    const TextRules::Animals& a = seen.animals;
    const std::vector<std::string> words = CrowdText::Words(message);

    // Talk of mounts: "nice mount" to one on foot gets that it is on foot.
    std::vector<std::vector<std::string>> mountTalk = g_PetsMountWords;
    if (!a.mount.empty())
        AddWords(mountTalk, a.mount);
    const bool aboutMount = CrowdText::ContainsPhrase(words, mountTalk);

    bool aboutPets = false;
    if (!a.petKind.empty() || !a.companion.empty())
    {
        std::vector<std::vector<std::string>> about = g_PetsWords;
        if (!a.petKind.empty())
            about.push_back(CrowdText::Words(a.petKind));   // "hunter pet", "water elemental"
        AddWords(about, a.petName);
        AddWords(about, a.petFamily);
        AddWords(about, a.companion);
        aboutPets = CrowdText::ContainsPhrase(words, about);
    }
    if (!aboutMount && !aboutPets)
        return {};

    const std::vector<std::string> parts = TextRules::AnimalPhrases(a, true, aboutMount);
    if (parts.empty())
        return {};
    std::string pets = parts[0];
    for (size_t i = 1; i < parts.size(); ++i)
        pets += "; " + parts[i];
    const std::string out = SafeFormat(g_PetsStateText, fmt::arg("pets", pets));
    return out == "[Format Error]" ? std::string() : out;
}
