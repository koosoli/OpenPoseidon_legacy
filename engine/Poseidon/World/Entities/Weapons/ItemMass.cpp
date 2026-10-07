// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#include "ItemMass.hpp"

#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/AI/EntityAIType.hpp>

#include <unordered_map>
#include <cstring>

namespace Poseidon::ItemMass
{

namespace
{

// An empty magazine still weighs this fraction of its full mass.
constexpr float EmptyMagFraction = 0.35f;

// -----------------------------------------------------------------------
// Sourced weight table. See WEIGHT_SOURCES.md for citations - every entry
// here has two independent sources unless the comment says otherwise.
// -----------------------------------------------------------------------

struct WeaponEntry { const char *needle; float kg; };
struct MagEntry     { const char *needle; float emptyKg; float loadedKg; };

// Substring match against the classname, case-insensitive. Ordered
// longest/most-specific first isn't required since these don't overlap,
// but keep it that way if you add near-duplicate names (e.g. "AK74" vs
// "AK74U") so the more specific one can be listed first and still match
// correctly regardless of order - substr matching doesn't care about order
// unless two needles both match the same string, in which case first wins.
// Empty weapon weight (no magazine), kg. Needles are substring-matched against the
// class name; NEEDLES ARE ORDERED SPECIFIC-FIRST because the loop takes the first
// match (e.g. "ak74su" before "ak74", "hkg3" before "hk", "glocks" before "glock",
// grenade-launcher variants before their base rifle). See WEIGHT_SOURCES.md.
const WeaponEntry kWeapons[] =
{
    { "m16grenadelauncher",  4.76f },  // M16A2 + M203
    { "ak74grenadelauncher", 4.70f },
    { "ak47grenadelauncher", 4.87f },
    { "ak74su",   2.70f },  // AKS-74U. militaryfactory + Wikipedia
    { "ak74",     3.30f },  // AK-74 fixed stock. weaponsystems.net + SAS PDF
    { "ak47",     3.47f },  // AK-47/AKM. Wikipedia + weaponatlas
    { "m16",      3.40f },  // M16A2. militaryfactory + weaponsystems.net + Wikipedia
    { "xms",      2.43f },  // XM177S
    { "m4",       2.43f },  // XM177E2 Commando. militaryfactory + Wikipedia
    { "fal",      4.45f },  // FN FAL. militaryfactory + Small Arms Survey
    { "hkg3",     4.70f },  // H&K G3A4. Wikipedia + SAS PDF  (before "hk")
    { "hk",       2.50f },  // H&K MP5. Wikipedia + weaponsystems.net
    { "g36",      3.63f },  // H&K G36. Wikipedia + militaryfactory
    { "steyr",    3.60f },  // Steyr AUG A1. Wikipedia + Steyr-Arms
    { "riffle",   4.19f },  // Lee-Enfield SMLE (WW1 icon). Wikipedia + imfdb
    { "uzi",      3.50f },  // Uzi. Small Arms Survey + militaryfactory
    { "bizon",    2.10f },  // PP-19 Bizon. Wikipedia + militaryfactory
    { "ingram",   2.84f },  // MAC-10. militaryfactory + Wikipedia
    { "skorpion", 1.28f },  // Skorpion vz.61. Wikipedia + modernfirearms
    { "beretta",  0.95f },  // Beretta 92F. militaryfactory + NRA
    { "glocks",   0.78f },  // Glock 17 + suppressor  (before "glock")
    { "glock",    0.625f }, // Glock 17. Wikipedia + weaponsystems.net
    { "cz75",     1.12f },  // CZ 75. Wikipedia + imfdb
    { "tokarev",  0.87f },  // TT-33. Wikipedia + militaryfactory
    { "revolver", 1.25f },  // S&W 686 (icon). Wikipedia + smith-wesson
    { "m21",      5.27f },  // M21 DMR. Wikipedia + military-history
    { "svd",      4.30f },  // Dragunov SVD. Wikipedia + SAS PDF
    { "dragunov", 4.30f },
    { "kozlice",  3.20f },  // over/under 12ga. Wikipedia + globalmilitary
    { "huntingrifle", 2.80f }, // CZ/ZKM 452 .22LR bolt-action. Wikipedia CZ 452 + budsgunshop
    { "m60",      10.5f },  // M60 GPMG. militaryfactory + vietnamwar.fandom
    { "pk",       9.00f },  // PK machine gun. militaryfactory + Wikipedia
    { "6g30",     6.20f },  // RG-6. Wikipedia + modernfirearms
    { "mm1",      5.70f },  // Hawk MM-1. Wikipedia + militaryfactory
    { "binocular",1.02f },  // 7x50 military. Steiner (B&H) + optics4birding
    { "nvgoggles",0.52f },  // AN/PVS-7. Wikipedia + FAS
    { "laserdesignator", 7.26f }, // AN/PAQ-1 LTD (16 lb). globalsecurity + FAS
    // Launcher TUBES only (empty); the projectile weight is in kMagazines. The
    // disposable ones (LAW/RPG-75/AT4) are split since the game reloads them.
    { "lawlauncher", 1.50f },        // M72 LAW tube
    { "law",         1.50f },
    { "rpglauncher", 2.40f },        // RPG-75 "Nh-75" tube. Wikipedia + militaryfactory
    { "at4launcher", 3.60f },        // AT4 tube. globalsecurity + forecastinternational
    { "carlgustavlauncher", 14.2f }, // Carl Gustaf M2. Wikipedia + weaponsystems.net
    { "9k32launcher", 5.00f },       // Strela-2 gripstock. Wikipedia + armyrecognition
    { "aalauncher",  5.50f },        // Stinger gripstock (assumed). Wikipedia + globalsecurity
};

// Loaded magazine / projectile weight, kg (loadedKg is what's used; emptyKg is
// informational). Rounds/rockets/grenades don't "empty" so both are equal. Ordered
// specific-first (e.g. "minee" before "mine", "hkg3mag" before "hk").
const MagEntry kMagazines[] =
{
    { "berettamag",  0.10f, 0.27f },   // 9x19 15rd
    { "glocksmag",   0.10f, 0.28f },   // 9x19 17rd
    { "glockmag",    0.10f, 0.28f },
    { "cz75mag",     0.10f, 0.28f },
    { "uzimag",      0.19f, 0.56f },   // 9x19 32rd
    { "ingrammag",   0.19f, 0.56f },
    { "bizonmag",    0.40f, 1.10f },   // 9x19 64rd helical
    { "tokarevmag",  0.06f, 0.16f },   // 7.62x25 8rd
    { "skorpionmag", 0.09f, 0.24f },   // .32 ACP 20rd
    { "revolvermag", 0.10f, 0.10f },   // 6 loose rounds
    { "falmag",      0.27f, 0.76f },   // 7.62x51 20rd
    { "hkg3mag",     0.27f, 0.76f },
    { "g36amag",     0.17f, 0.49f },   // 5.56 30rd
    { "steyrmag",    0.17f, 0.49f },
    { "huntingriflemag", 0.03f, 0.05f },   // .22 LR 5rd (ZKM 452)
    { "m60",         0.95f, 2.80f },   // 7.62x51 100rd belt
    { "kozlice",     0.02f, 0.05f },   // 12-ga shell, ~50 g each (ONE shell per mag; a box of 10 ~0.5 kg)
    { "handgrenade", 0.40f, 0.40f },   // M67 frag. Wikipedia + FAS
    { "smokeshell",  0.54f, 0.54f },   // M18 smoke. Wikipedia + FAS
    { "flare",       0.23f, 0.23f },   // 40mm M583 flare. FAS + bulletpicker
    { "grenadelauncher", 0.23f, 0.23f }, // 40mm M406 HE
    { "mortar",      0.23f, 0.23f },   // 40mm HE (M203)
    { "6g30",        0.23f, 0.23f },   // 40mm
    { "mm1",         0.23f, 0.23f },   // 40mm
    { "minee",       0.55f, 0.55f },   // AP mine, PMN (disc icon).  before "mine"
    { "mine",        9.50f, 9.50f },   // AT mine, TM-62. Wikipedia + military-history
    { "pipebomb",    9.10f, 9.10f },   // M183 satchel. military-history + globalsecurity
    // Launcher projectiles (the tube weight is in kWeapons).
    { "lawlauncher", 1.00f, 1.00f },   // M72 rocket
    { "rpglauncher", 0.80f, 0.80f },   // RPG-75 rocket. Wikipedia + militaryfactory
    { "at4launcher", 3.10f, 3.10f },   // AT4 projectile. globalsecurity + forecast
    { "carlgustavlauncher", 3.20f, 3.20f }, // FFV551 HEAT. Wikipedia + IWM
    { "aalauncher",  10.1f, 10.1f },   // Stinger missile. Wikipedia + globalsecurity
    { "9k32launcher",9.80f, 9.80f },   // Strela missile. Wikipedia + armyrecognition
    // Base rifle calibers (AFTER the specific needles above).
    { "svddragunov", 0.15f, 0.43f },   // 7.62x54R 10rd
    { "m21",         0.27f, 0.76f },   // 7.62x51 20rd
    { "m16",         0.17f, 0.49f },   // 5.56 30rd
    { "556",         0.17f, 0.49f },
    { "m4",          0.17f, 0.49f },
    { "hk",          0.17f, 0.50f },   // MP5 9x19 30rd (after hkg3mag)
    { "ak74",        0.20f, 0.52f },   // 5.45x39 30rd
    { "ak47",        0.30f, 0.82f },   // 7.62x39 30rd
};

// PK/7.62x54mmR belt ammo has no two-source figure - flagged as computed
// in WEIGHT_SOURCES.md rather than presented as sourced fact.
constexpr float k762x54RRoundMass = 0.023f;   // kg/round, estimated

// --- Volume (litres) tables. Values from ITEM_WEIGHTS.md (bbox x fill factor).
// Same needle-ordering rules as the weight tables (specific-first). -----------
struct VolEntry { const char *needle; float litres; };

const VolEntry kWeaponVol[] =
{
    { "m16grenadelauncher",  4.6f }, { "ak74grenadelauncher", 4.1f }, { "ak47grenadelauncher", 3.9f },
    { "ak74su",   2.7f }, { "ak74", 4.1f }, { "ak47", 3.9f }, { "m16", 4.6f },
    { "xms", 3.2f }, { "m4", 3.2f }, { "fal", 5.0f }, { "hkg3", 4.7f }, { "hk", 2.7f },
    { "g36", 4.6f }, { "steyr", 3.6f }, { "uzi", 2.8f }, { "bizon", 2.9f },
    { "ingram", 1.0f }, { "skorpion", 0.7f }, { "beretta", 0.6f }, { "glocks", 0.6f },
    { "glock", 0.4f }, { "cz75", 0.6f }, { "tokarev", 0.4f }, { "revolver", 0.9f },
    { "m21", 5.1f }, { "svd", 5.6f }, { "dragunov", 5.6f }, { "kozlice", 2.8f },
    { "huntingrifle", 3.1f }, { "m60", 11.9f }, { "pk", 12.5f }, { "6g30", 5.7f },
    { "mm1", 9.4f }, { "binocular", 0.7f }, { "nvgoggles", 0.5f }, { "laserdesignator", 4.5f },
    { "lawlauncher", 2.9f }, { "law", 2.9f }, { "rpglauncher", 3.6f }, { "at4launcher", 5.1f },
    { "carlgustavlauncher", 13.6f }, { "9k32launcher", 6.9f }, { "aalauncher", 7.3f },
};

const VolEntry kMagVol[] =
{
    { "berettamag", 0.06f }, { "glocksmag", 0.07f }, { "glockmag", 0.07f }, { "cz75mag", 0.07f },
    { "uzimag", 0.16f }, { "ingrammag", 0.16f }, { "bizonmag", 0.9f }, { "tokarevmag", 0.04f },
    { "skorpionmag", 0.07f }, { "revolvermag", 0.05f }, { "falmag", 0.25f }, { "hkg3mag", 0.25f },
    { "g36amag", 0.2f }, { "steyrmag", 0.2f }, { "huntingriflemag", 0.04f }, { "m60", 0.7f },
    { "kozlice", 0.03f }, { "handgrenade", 0.2f }, { "smokeshell", 0.3f }, { "flare", 0.13f },
    { "grenadelauncher", 0.13f }, { "mortar", 0.13f }, { "6g30", 0.13f }, { "mm1", 0.13f },
    { "minee", 0.4f }, { "mine", 7.3f }, { "pipebomb", 3.8f },
    { "lawlauncher", 1.5f }, { "rpglauncher", 1.8f }, { "at4launcher", 2.5f },
    { "carlgustavlauncher", 2.0f }, { "aalauncher", 5.0f }, { "9k32launcher", 5.0f },
    { "svddragunov", 0.2f }, { "m21", 0.25f }, { "m16", 0.2f }, { "556", 0.2f },
    { "m4", 0.2f }, { "hk", 0.2f }, { "ak74", 0.2f }, { "ak47", 0.3f },
};

float DefaultWeaponVol(int slotMask)
{
    if (slotMask & MaskSlotPrimary)   return 4.5f;
    if (slotMask & MaskSlotSecondary) return 6.0f;
    if (slotMask & MaskSlotHandGun)   return 0.5f;
    if (slotMask & MaskSlotBinocular) return 0.6f;
    if (slotMask & MaskSlotItem)      return 0.3f;
    return 2.0f;
}

bool ContainsCI(const RStringB &haystack, const char *needle)
{
    // RString has a built-in in-place lowercase (RString.hpp:101) - use it
    // rather than iterating, since RString exposes no mutable char iterator
    // (operator[] returns char by value, Data() is const).
    RString h(haystack);
    h.Lower();
    RString n(needle);
    n.Lower();
    return strstr(h.Data(), n.Data()) != nullptr;
}

float DefaultWeaponMass(int slotMask)
{
    if (slotMask & MaskSlotPrimary)   return 3.5f;
    if (slotMask & MaskSlotSecondary) return 6.5f;
    if (slotMask & MaskSlotHandGun)   return 0.9f;
    if (slotMask & MaskSlotBinocular) return 0.6f;
    if (slotMask & MaskSlotItem)      return 0.2f;
    return 1.0f;
}

std::unordered_map<const void *, float> g_cache;

float ConfigMass(const ParamEntry *cls)
{
    if (!cls)
    {
        return -1.0f;
    }
    const ParamEntry *entry = cls->FindEntry("mass");
    // VERIFY: FindEntry / the read pattern. See the identical VERIFY note in
    // the previous pass - VehicleAI.cpp:441 uses `par >> "transportMaxMagazines"`
    // as the established idiom in this codebase; swap to that if FindEntry
    // isn't the right accessor.
    if (!entry)
    {
        return -1.0f;
    }
    const float mass = *entry;
    return (mass >= 0.0f) ? mass : -1.0f;
}

} // namespace

float Of(const WeaponType *weapon)
{
    if (!weapon)
    {
        return 0.0f;
    }

    auto it = g_cache.find(weapon);
    if (it != g_cache.end())
    {
        return it->second;
    }

    float mass = ConfigMass(weapon->_parClass);

    if (mass < 0.0f)
    {
        const RStringB &name = weapon->GetName();
        for (const WeaponEntry &e : kWeapons)
        {
            if (ContainsCI(name, e.needle))
            {
                mass = e.kg;
                break;
            }
        }
    }

    if (mass < 0.0f)
    {
        mass = DefaultWeaponMass(weapon->_weaponType);
    }

    g_cache[weapon] = mass;
    return mass;
}

float Of(const MagazineType *type)
{
    if (!type)
    {
        return 0.0f;
    }

    auto it = g_cache.find(type);
    if (it != g_cache.end())
    {
        return it->second;
    }

    float mass = ConfigMass(type->_parClass);

    if (mass < 0.0f)
    {
        const RStringB &name = type->GetName();
        for (const MagEntry &e : kMagazines)
        {
            if (ContainsCI(name, e.needle))
            {
                mass = e.loadedKg;   // "full" mass - Of(Magazine*) scales it down
                break;
            }
        }
    }

    if (mass < 0.0f)
    {
        // 7.62x54R belts don't match a fixed-capacity entry above; estimate
        // from round count if this looks like one, otherwise fall back to
        // the generic capacity-scaled default.
        const RStringB &name = type->GetName();
        if (ContainsCI(name, "762x54") || ContainsCI(name, "pk"))
        {
            mass = 0.5f + k762x54RRoundMass * static_cast<float>(type->_maxAmmo);
        }
        else
        {
            mass = 0.09f + 0.012f * static_cast<float>(type->_maxAmmo);
        }
    }

    g_cache[type] = mass;
    return mass;
}

float Of(const Magazine *magazine)
{
    if (!magazine || !magazine->_type)
    {
        return 0.0f;
    }

    const MagazineType *type = magazine->_type;

    // Jerry can: 4 kg empty steel can + 0.85 kg per litre of fuel, so a full 20 L can
    // weighs 21 kg. _ammo holds the current litres (0..20), so weight tracks fuel level.
    if (ContainsCI(type->GetName(), "jerrycan"))
    {
        return 4.0f + 0.85f * static_cast<float>(magazine->_ammo);
    }

    // Shotgun shells (Kozlice 12-ga): weight scales with the number of SHELLS present,
    // not the raw pellet/round count. A magazine now holds two shells (a break-action
    // pair), and buckshot stores `division` engine-rounds per shell (7 pellets), so
    // shells = ammo / division. ~50 g per 12-ga shell -> a full 2-shell mag ~0.10 kg.
    if (ContainsCI(type->GetName(), "kozlice"))
    {
        const int div = 1;
        return 0.050f * (static_cast<float>(magazine->_ammo) / static_cast<float>(div));
    }

    const float full = Of(type);

    const int maxAmmo = type->_maxAmmo;
    if (maxAmmo <= 0)
    {
        return full;
    }

    const int ammo = magazine->_ammo;   // Encrypted<int> -> int
    const float loaded = static_cast<float>(ammo) / static_cast<float>(maxAmmo);

    return full * (EmptyMagFraction + (1.0f - EmptyMagFraction) * loaded);
}

float TotalCarried(const EntityAI *unit)
{
    if (!unit)
    {
        return 0.0f;
    }

    float total = 0.0f;

    for (int i = 0; i < unit->NWeaponSystems(); i++)
    {
        total += Of(unit->GetWeaponSystem(i));
    }
    for (int i = 0; i < unit->NMagazines(); i++)
    {
        total += Of(unit->GetMagazine(i));
    }

    return total;
}

float VolumeOf(const WeaponType *weapon)
{
    if (!weapon)
        return 0.0f;
    const RStringB &name = weapon->GetName();
    for (const VolEntry &e : kWeaponVol)
        if (ContainsCI(name, e.needle))
            return e.litres;
    return DefaultWeaponVol(weapon->_weaponType);
}

float VolumeOf(const MagazineType *type)
{
    if (!type)
        return 0.0f;
    const RStringB &name = type->GetName();
    for (const VolEntry &e : kMagVol)
        if (ContainsCI(name, e.needle))
            return e.litres;
    return 0.08f + 0.006f * static_cast<float>(type->_maxAmmo);   // generic fallback
}

float VolumeOf(const Magazine *magazine)
{
    return (magazine && magazine->_type) ? VolumeOf(magazine->_type) : 0.0f;
}

float TotalVolumeCarried(const EntityAI *unit)
{
    if (!unit)
        return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < unit->NWeaponSystems(); i++)
        total += VolumeOf(unit->GetWeaponSystem(i));
    for (int i = 0; i < unit->NMagazines(); i++)
        total += VolumeOf(unit->GetMagazine(i));
    return total;
}

void InvalidateCache()
{
    g_cache.clear();
}

} // namespace Poseidon::ItemMass
