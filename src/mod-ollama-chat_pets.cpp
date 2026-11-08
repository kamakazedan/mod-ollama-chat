#include "mod-ollama-chat_pets.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_crowd.h"
#include "mod-ollama-chat-utilities.h"

#include "Creature.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"

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

    // Words in companions' names that say nothing on their own: "great run"
    // is not about a Great Horned Owl.
    bool Plain(const std::string& w)
    {
        static const char* const kPlain[] = { "the", "and", "with", "tiny", "little", "lil", "mini", "baby", "great",
                                              "small", "big", "young", "old", "black", "white", "brown", "green",
                                              "blue", "red", "pink", "golden", "gold", "silver", "grey", "gray",
                                              "crimson", "dark", "light", "wood", "wing", "horned", "spotted" };
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

std::string Pets_StateTextFor(Player* bot, const std::string& message)
{
    if (!g_Enable || !bot || g_PetsStateText.empty() || message.empty())
        return {};

    std::vector<std::string> parts;
    std::vector<std::vector<std::string>> about = g_PetsWords;

    if (Pet* pet = bot->GetPet(); pet && pet->IsInWorld() && pet->IsAlive())
    {
        const std::string family = FamilyOf(pet);
        const std::string name   = pet->GetName();
        std::string part = (!family.empty() && family != name) ? "your " + Lower(family) + " " + name : "your " + name;
        if (pet->getPetType() == HUNTER_PET)
            part += " (your hunter pet)";
        else if (bot->getClass() == CLASS_WARLOCK)
            part += " (your demon)";
        parts.push_back(std::move(part));
        AddWords(about, name);
        AddWords(about, family);
    }

    const ObjectGuid critterGuid = bot->GetCritterGUID();
    if (!critterGuid.IsEmpty())
        if (Creature* critter = ObjectAccessor::GetCreature(*bot, critterGuid); critter && critter->IsInWorld())
        {
            parts.push_back("a little " + critter->GetName() + " following you (a companion pet)");
            AddWords(about, critter->GetName());
        }

    if (parts.empty() || !CrowdText::ContainsPhrase(CrowdText::Words(message), about))
        return {};

    std::string pets = parts[0];
    for (size_t i = 1; i < parts.size(); ++i)
        pets += " and " + parts[i];
    const std::string out = SafeFormat(g_PetsStateText, fmt::arg("pets", pets));
    return out == "[Format Error]" ? std::string() : out;
}
