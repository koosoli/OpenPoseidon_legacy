#pragma once

// Named EAX reverb presets (the EFX preset table, see thirdparty/efx-presets)
// plus the environment size each was authored for.  Drives the software reverb
// (Reverb.hpp), the environment mapping and the `sound reverb-list` tool.

#include <efx-presets.h>

#include <cstddef>

namespace EFX
{

// EAX reverb parameter ranges; the presets stay inside them and the size
// scaling clamps to them.
inline constexpr float kMinDecayTime = 0.1f;
inline constexpr float kMaxDecayTime = 20.0f;
inline constexpr float kMinDensity = 0.0f;
inline constexpr float kMaxDensity = 1.0f;

struct PresetEntry
{
    const char* name; // CLI-friendly lowercase name
    EFXEAXREVERBPROPERTIES props;
    float defaultSize; // EAX 2.0 environment size for scaling
};

#define EFX_ENTRY(cli_name, macro, sz) {cli_name, macro, sz}

// clang-format off
inline constexpr PresetEntry kPresets[] = {
    // ── Standard EAX 2.0 environments ──────────────────────────────
    EFX_ENTRY("generic",            EFX_REVERB_PRESET_GENERIC,            7.5f),
    EFX_ENTRY("paddedcell",         EFX_REVERB_PRESET_PADDEDCELL,         1.4f),
    EFX_ENTRY("room",               EFX_REVERB_PRESET_ROOM,               1.9f),
    EFX_ENTRY("bathroom",           EFX_REVERB_PRESET_BATHROOM,           1.4f),
    EFX_ENTRY("livingroom",         EFX_REVERB_PRESET_LIVINGROOM,         2.5f),
    EFX_ENTRY("stoneroom",          EFX_REVERB_PRESET_STONEROOM,         11.6f),
    EFX_ENTRY("auditorium",         EFX_REVERB_PRESET_AUDITORIUM,        21.6f),
    EFX_ENTRY("concerthall",        EFX_REVERB_PRESET_CONCERTHALL,       19.6f),
    EFX_ENTRY("cave",               EFX_REVERB_PRESET_CAVE,              14.6f),
    EFX_ENTRY("arena",              EFX_REVERB_PRESET_ARENA,             36.2f),
    EFX_ENTRY("hangar",             EFX_REVERB_PRESET_HANGAR,            50.3f),
    EFX_ENTRY("carpetedhallway",    EFX_REVERB_PRESET_CARPETEDHALLWAY,    1.9f),
    EFX_ENTRY("hallway",            EFX_REVERB_PRESET_HALLWAY,            1.8f),
    EFX_ENTRY("stonecorridor",      EFX_REVERB_PRESET_STONECORRIDOR,     13.5f),
    EFX_ENTRY("alley",              EFX_REVERB_PRESET_ALLEY,              7.5f),
    EFX_ENTRY("forest",             EFX_REVERB_PRESET_FOREST,            38.0f),
    EFX_ENTRY("city",               EFX_REVERB_PRESET_CITY,               7.5f),
    EFX_ENTRY("mountains",          EFX_REVERB_PRESET_MOUNTAINS,        100.0f),
    EFX_ENTRY("quarry",             EFX_REVERB_PRESET_QUARRY,            17.5f),
    EFX_ENTRY("plain",              EFX_REVERB_PRESET_PLAIN,             42.5f),
    EFX_ENTRY("parkinglot",         EFX_REVERB_PRESET_PARKINGLOT,         8.3f),
    EFX_ENTRY("sewerpipe",          EFX_REVERB_PRESET_SEWERPIPE,          1.7f),
    EFX_ENTRY("underwater",         EFX_REVERB_PRESET_UNDERWATER,         1.8f),
    EFX_ENTRY("drugged",            EFX_REVERB_PRESET_DRUGGED,            1.9f),
    EFX_ENTRY("dizzy",              EFX_REVERB_PRESET_DIZZY,              1.8f),
    EFX_ENTRY("psychotic",          EFX_REVERB_PRESET_PSYCHOTIC,          1.0f),

    // ── Castle ─────────────────────────────────────────────────────
    EFX_ENTRY("castle-smallroom",    EFX_REVERB_PRESET_CASTLE_SMALLROOM,    8.3f),
    EFX_ENTRY("castle-shortpassage", EFX_REVERB_PRESET_CASTLE_SHORTPASSAGE, 8.3f),
    EFX_ENTRY("castle-mediumroom",   EFX_REVERB_PRESET_CASTLE_MEDIUMROOM,   8.3f),
    EFX_ENTRY("castle-largeroom",    EFX_REVERB_PRESET_CASTLE_LARGEROOM,    8.3f),
    EFX_ENTRY("castle-longpassage",  EFX_REVERB_PRESET_CASTLE_LONGPASSAGE,  8.3f),
    EFX_ENTRY("castle-hall",         EFX_REVERB_PRESET_CASTLE_HALL,          8.3f),
    EFX_ENTRY("castle-cupboard",     EFX_REVERB_PRESET_CASTLE_CUPBOARD,     8.3f),
    EFX_ENTRY("castle-courtyard",    EFX_REVERB_PRESET_CASTLE_COURTYARD,    8.3f),
    EFX_ENTRY("castle-alcove",       EFX_REVERB_PRESET_CASTLE_ALCOVE,       8.3f),

    // ── Factory ────────────────────────────────────────────────────
    EFX_ENTRY("factory-smallroom",    EFX_REVERB_PRESET_FACTORY_SMALLROOM,    1.8f),
    EFX_ENTRY("factory-shortpassage", EFX_REVERB_PRESET_FACTORY_SHORTPASSAGE, 1.8f),
    EFX_ENTRY("factory-mediumroom",   EFX_REVERB_PRESET_FACTORY_MEDIUMROOM,   1.8f),
    EFX_ENTRY("factory-largeroom",    EFX_REVERB_PRESET_FACTORY_LARGEROOM,    1.8f),
    EFX_ENTRY("factory-longpassage",  EFX_REVERB_PRESET_FACTORY_LONGPASSAGE,  1.8f),
    EFX_ENTRY("factory-hall",         EFX_REVERB_PRESET_FACTORY_HALL,          1.8f),
    EFX_ENTRY("factory-cupboard",     EFX_REVERB_PRESET_FACTORY_CUPBOARD,     1.8f),
    EFX_ENTRY("factory-courtyard",    EFX_REVERB_PRESET_FACTORY_COURTYARD,    1.8f),
    EFX_ENTRY("factory-alcove",       EFX_REVERB_PRESET_FACTORY_ALCOVE,       1.8f),

    // ── Ice Palace ─────────────────────────────────────────────────
    EFX_ENTRY("icepalace-smallroom",    EFX_REVERB_PRESET_ICEPALACE_SMALLROOM,    2.7f),
    EFX_ENTRY("icepalace-shortpassage", EFX_REVERB_PRESET_ICEPALACE_SHORTPASSAGE, 2.7f),
    EFX_ENTRY("icepalace-mediumroom",   EFX_REVERB_PRESET_ICEPALACE_MEDIUMROOM,   2.7f),
    EFX_ENTRY("icepalace-largeroom",    EFX_REVERB_PRESET_ICEPALACE_LARGEROOM,    2.7f),
    EFX_ENTRY("icepalace-longpassage",  EFX_REVERB_PRESET_ICEPALACE_LONGPASSAGE,  2.7f),
    EFX_ENTRY("icepalace-hall",         EFX_REVERB_PRESET_ICEPALACE_HALL,          2.7f),
    EFX_ENTRY("icepalace-cupboard",     EFX_REVERB_PRESET_ICEPALACE_CUPBOARD,     2.7f),
    EFX_ENTRY("icepalace-courtyard",    EFX_REVERB_PRESET_ICEPALACE_COURTYARD,    2.7f),
    EFX_ENTRY("icepalace-alcove",       EFX_REVERB_PRESET_ICEPALACE_ALCOVE,       2.7f),

    // ── Space Station ──────────────────────────────────────────────
    EFX_ENTRY("spacestation-smallroom",    EFX_REVERB_PRESET_SPACESTATION_SMALLROOM,    1.5f),
    EFX_ENTRY("spacestation-shortpassage", EFX_REVERB_PRESET_SPACESTATION_SHORTPASSAGE, 1.5f),
    EFX_ENTRY("spacestation-mediumroom",   EFX_REVERB_PRESET_SPACESTATION_MEDIUMROOM,   1.5f),
    EFX_ENTRY("spacestation-largeroom",    EFX_REVERB_PRESET_SPACESTATION_LARGEROOM,    1.5f),
    EFX_ENTRY("spacestation-longpassage",  EFX_REVERB_PRESET_SPACESTATION_LONGPASSAGE,  1.5f),
    EFX_ENTRY("spacestation-hall",         EFX_REVERB_PRESET_SPACESTATION_HALL,          1.5f),
    EFX_ENTRY("spacestation-cupboard",     EFX_REVERB_PRESET_SPACESTATION_CUPBOARD,     1.5f),
    EFX_ENTRY("spacestation-alcove",       EFX_REVERB_PRESET_SPACESTATION_ALCOVE,       1.5f),

    // ── Wooden Galleon ─────────────────────────────────────────────
    EFX_ENTRY("wooden-smallroom",    EFX_REVERB_PRESET_WOODEN_SMALLROOM,     7.5f),
    EFX_ENTRY("wooden-shortpassage", EFX_REVERB_PRESET_WOODEN_SHORTPASSAGE,  7.5f),
    EFX_ENTRY("wooden-mediumroom",   EFX_REVERB_PRESET_WOODEN_MEDIUMROOM,    7.5f),
    EFX_ENTRY("wooden-largeroom",    EFX_REVERB_PRESET_WOODEN_LARGEROOM,     7.5f),
    EFX_ENTRY("wooden-longpassage",  EFX_REVERB_PRESET_WOODEN_LONGPASSAGE,   7.5f),
    EFX_ENTRY("wooden-hall",         EFX_REVERB_PRESET_WOODEN_HALL,           7.5f),
    EFX_ENTRY("wooden-cupboard",     EFX_REVERB_PRESET_WOODEN_CUPBOARD,      7.5f),
    EFX_ENTRY("wooden-courtyard",    EFX_REVERB_PRESET_WOODEN_COURTYARD,     7.5f),
    EFX_ENTRY("wooden-alcove",       EFX_REVERB_PRESET_WOODEN_ALCOVE,        7.5f),

    // ── Sport ──────────────────────────────────────────────────────
    EFX_ENTRY("sport-emptystadium",      EFX_REVERB_PRESET_SPORT_EMPTYSTADIUM,      7.2f),
    EFX_ENTRY("sport-squashcourt",       EFX_REVERB_PRESET_SPORT_SQUASHCOURT,       7.5f),
    EFX_ENTRY("sport-smallswimmingpool", EFX_REVERB_PRESET_SPORT_SMALLSWIMMINGPOOL, 36.2f),
    EFX_ENTRY("sport-largeswimmingpool", EFX_REVERB_PRESET_SPORT_LARGESWIMMINGPOOL, 36.2f),
    EFX_ENTRY("sport-gymnasium",         EFX_REVERB_PRESET_SPORT_GYMNASIUM,          7.5f),
    EFX_ENTRY("sport-fullstadium",       EFX_REVERB_PRESET_SPORT_FULLSTADIUM,        7.2f),
    EFX_ENTRY("sport-stadiumtannoy",     EFX_REVERB_PRESET_SPORT_STADIUMTANNOY,      3.0f),

    // ── Prefab ─────────────────────────────────────────────────────
    EFX_ENTRY("prefab-workshop",     EFX_REVERB_PRESET_PREFAB_WORKSHOP,      1.9f),
    EFX_ENTRY("prefab-schoolroom",   EFX_REVERB_PRESET_PREFAB_SCHOOLROOM,    1.9f),
    EFX_ENTRY("prefab-practiseroom", EFX_REVERB_PRESET_PREFAB_PRACTISEROOM,  1.9f),
    EFX_ENTRY("prefab-outhouse",     EFX_REVERB_PRESET_PREFAB_OUTHOUSE,      8.0f),
    EFX_ENTRY("prefab-caravan",      EFX_REVERB_PRESET_PREFAB_CARAVAN,       8.0f),

    // ── Dome & Pipe ────────────────────────────────────────────────
    EFX_ENTRY("dome-tomb",       EFX_REVERB_PRESET_DOME_TOMB,        51.8f),
    EFX_ENTRY("pipe-small",      EFX_REVERB_PRESET_PIPE_SMALL,        1.0f),
    EFX_ENTRY("dome-saintpauls", EFX_REVERB_PRESET_DOME_SAINTPAULS,  51.8f),
    EFX_ENTRY("pipe-longthin",   EFX_REVERB_PRESET_PIPE_LONGTHIN,     1.6f),
    EFX_ENTRY("pipe-large",      EFX_REVERB_PRESET_PIPE_LARGE,       50.3f),
    EFX_ENTRY("pipe-resonant",   EFX_REVERB_PRESET_PIPE_RESONANT,     1.3f),

    // ── Outdoors ───────────────────────────────────────────────────
    EFX_ENTRY("outdoors-backyard",      EFX_REVERB_PRESET_OUTDOORS_BACKYARD,       80.3f),
    EFX_ENTRY("outdoors-rollingplains", EFX_REVERB_PRESET_OUTDOORS_ROLLINGPLAINS,  80.3f),
    EFX_ENTRY("outdoors-deepcanyon",    EFX_REVERB_PRESET_OUTDOORS_DEEPCANYON,     80.3f),
    EFX_ENTRY("outdoors-creek",         EFX_REVERB_PRESET_OUTDOORS_CREEK,          80.3f),
    EFX_ENTRY("outdoors-valley",        EFX_REVERB_PRESET_OUTDOORS_VALLEY,         80.3f),

    // ── Mood ───────────────────────────────────────────────────────
    EFX_ENTRY("mood-heaven", EFX_REVERB_PRESET_MOOD_HEAVEN, 19.6f),
    EFX_ENTRY("mood-hell",   EFX_REVERB_PRESET_MOOD_HELL,  100.0f),
    EFX_ENTRY("mood-memory", EFX_REVERB_PRESET_MOOD_MEMORY,  8.0f),

    // ── Driving ────────────────────────────────────────────────────
    EFX_ENTRY("driving-commentator",    EFX_REVERB_PRESET_DRIVING_COMMENTATOR,     3.0f),
    EFX_ENTRY("driving-pitgarage",      EFX_REVERB_PRESET_DRIVING_PITGARAGE,       1.9f),
    EFX_ENTRY("driving-incar-racer",    EFX_REVERB_PRESET_DRIVING_INCAR_RACER,     1.1f),
    EFX_ENTRY("driving-incar-sports",   EFX_REVERB_PRESET_DRIVING_INCAR_SPORTS,    1.1f),
    EFX_ENTRY("driving-incar-luxury",   EFX_REVERB_PRESET_DRIVING_INCAR_LUXURY,    1.6f),
    EFX_ENTRY("driving-fullgrandstand", EFX_REVERB_PRESET_DRIVING_FULLGRANDSTAND,  8.3f),
    EFX_ENTRY("driving-emptygrandstand",EFX_REVERB_PRESET_DRIVING_EMPTYGRANDSTAND, 8.3f),
    EFX_ENTRY("driving-tunnel",         EFX_REVERB_PRESET_DRIVING_TUNNEL,         14.6f),

    // ── City ───────────────────────────────────────────────────────
    EFX_ENTRY("city-streets",   EFX_REVERB_PRESET_CITY_STREETS,    3.0f),
    EFX_ENTRY("city-subway",    EFX_REVERB_PRESET_CITY_SUBWAY,     3.0f),
    EFX_ENTRY("city-museum",    EFX_REVERB_PRESET_CITY_MUSEUM,     3.0f),
    EFX_ENTRY("city-library",   EFX_REVERB_PRESET_CITY_LIBRARY,    3.0f),
    EFX_ENTRY("city-underpass", EFX_REVERB_PRESET_CITY_UNDERPASS,   3.0f),
    EFX_ENTRY("city-abandoned", EFX_REVERB_PRESET_CITY_ABANDONED,   3.0f),

    // ── Misc ───────────────────────────────────────────────────────
    EFX_ENTRY("dustyroom",      EFX_REVERB_PRESET_DUSTYROOM,       1.8f),
    EFX_ENTRY("chapel",         EFX_REVERB_PRESET_CHAPEL,          19.6f),
    EFX_ENTRY("smallwaterroom", EFX_REVERB_PRESET_SMALLWATERROOM,  36.2f),
};
// clang-format on

#undef EFX_ENTRY

inline constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// Find preset by CLI name (case-insensitive). Returns nullptr if not found.
inline const PresetEntry* FindPreset(const char* name)
{
    if (!name)
        return nullptr;
    for (size_t i = 0; i < kPresetCount; ++i)
    {
        const char* a = kPresets[i].name;
        const char* b = name;
        bool match = true;
        while (*a && *b)
        {
            char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
            if (ca != cb)
            {
                match = false;
                break;
            }
            ++a;
            ++b;
        }
        if (match && *a == '\0' && *b == '\0')
            return &kPresets[i];
    }
    return nullptr;
}

// Scale a preset to an environment size the way EAX 2.0 did (decay time
// proportional to size, density = size / 100), clamped to the EFX ranges.
inline EFXEAXREVERBPROPERTIES ScaledPreset(const PresetEntry& entry, float size)
{
    if (size < 2.f)
        size = 2.f;
    if (size > 100.f)
        size = 100.f;
    EFXEAXREVERBPROPERTIES p = entry.props;
    p.flDecayTime *= size / entry.defaultSize;
    if (p.flDecayTime < kMinDecayTime)
        p.flDecayTime = kMinDecayTime;
    if (p.flDecayTime > kMaxDecayTime)
        p.flDecayTime = kMaxDecayTime;
    p.flDensity = size / 100.0f;
    if (p.flDensity < kMinDensity)
        p.flDensity = kMinDensity;
    if (p.flDensity > kMaxDensity)
        p.flDensity = kMaxDensity;
    return p;
}

} // namespace EFX
