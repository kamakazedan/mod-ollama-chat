#include "mod-ollama-chat_revive.h"

#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

// While a mod-dungeon-clear run is on, it decides how its party is brought
// back (a rez, a wait, or calling the run off).
#if __has_include("DcLeaderSignal.h") && __has_include("DcModuleEnable.h")
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#define OLLAMA_REVIVE_DUNGEON_CLEAR 1
#else
#define OLLAMA_REVIVE_DUNGEON_CLEAR 0
#endif

#include <chrono>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Who knows a resurrection, looked up once a minute per character.
    struct RezCache
    {
        bool              can = false;
        Clock::time_point until{};
    };
    std::unordered_map<uint64_t, RezCache> g_rezCache;

    bool IsResurrect(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_RESURRECT || effect.Effect == SPELL_EFFECT_RESURRECT_NEW)
                return true;
        return false;
    }
}

bool Revive_RunDecides(Player* bot)
{
#if OLLAMA_REVIVE_DUNGEON_CLEAR
    return bot && DcModule::IsEnabled() && DcLeaderSignal::FindRunOwner(bot) != nullptr;
#else
    (void)bot;
    return false;
#endif
}

// Priests, paladins, shamans and druids once they have learned the spell: the
// spellbook is asked, not the class, so a level 8 priest is no rezzer.
bool Revive_KnowsResurrect(Player* p)
{
    if (!p)
        return false;
    switch (p->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_DEATH_KNIGHT:
            return false;
        default:
            break;
    }

    if (g_rezCache.size() > 2000)
        g_rezCache.clear();
    RezCache& cache = g_rezCache[p->GetGUID().GetRawValue()];
    const Clock::time_point now = Clock::now();
    if (now < cache.until)
        return cache.can;
    cache.until = now + std::chrono::seconds(60);
    cache.can = false;
    for (auto const& [spellId, spell] : p->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (info && !info->IsPassive() && IsResurrect(info))
        {
            cache.can = true;
            break;
        }
    }
    return cache.can;
}
