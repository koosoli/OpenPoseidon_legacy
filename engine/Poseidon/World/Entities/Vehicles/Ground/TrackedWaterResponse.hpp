#pragma once

#include <algorithm>
#include <cstdlib>

namespace Poseidon
{
// Tracked amphibians use a stiff legacy displacement spring. Let them follow
// waves, but do not feed the full modern swell height into that spring.
// (W6d) WGR_AMPHIB_WAVE=<m> overrides the excursion for tuning captures (default 0.35 m).
inline float AmphibiousWaveExcursion()
{
    static const float v = []
    {
        const char* e = std::getenv("WGR_AMPHIB_WAVE");
        const float x = e != nullptr ? static_cast<float>(std::atof(e)) : 0.0f;
        return x > 0.0f ? x : 0.35f;
    }();
    return v;
}

inline float AmphibiousTrackedImmersion(float under, float underFlat)
{
    const float MaxWaveExcursion = AmphibiousWaveExcursion();
    const float waveExcursion = std::clamp(under - underFlat, -MaxWaveExcursion, MaxWaveExcursion);
    return std::max(0.0f, underFlat + waveExcursion);
}

inline float AmphibiousTrackedPropulsionScale(bool canFloat, bool landContact, bool objectContact)
{
    if (landContact || objectContact) return 1.0f;
    // Keep amphibious tracks in their slow boat-like regime, but give them enough
    // authority to make headway through quadratic hull drag and modern waves.
    return canFloat ? 0.16f : 0.1f;
}

// TW-WATER W6-pre: a destroyed vehicle in the water floods and goes to the bottom (Tank, Car).
// Before this an amphibious wreck floated, burning, for ever, and any wreck's buoyancy grew with
// its depth (the displacement springs integrate `under`, which has no ceiling), so a sinking hull
// found an equilibrium in mid-water instead of reaching the sea bed.
//  * flooding: from the moment of death in the water the wreck keeps a falling share of its live
//    buoyancy -- all of it at first, WreckResidualBuoyancy after WreckFloodSeconds -- so it burns
//    afloat while it settles, then goes under and sinks rather than dropping like a stone;
//  * immersion: a wreck's buoyancy and water drag integrate at most WreckMaxImmersion per contact
//    point, so once under it sinks at a steady ~3 m/s to the bottom, where the ground holds it.
// (W6c) 60 s: a holed, burning APC stays afloat, settling, for some 20-30 s before it goes under
// (20 s sent a BMP to the bottom about 8 s after it was hit).
constexpr float WreckFloodSeconds = 60.0f;
constexpr float WreckResidualBuoyancy = 0.1f;
constexpr float WreckMaxImmersion = 1.5f;

inline float WreckFloodStep(float flood, float deltaT)
{
    return std::min(1.0f, flood + std::max(deltaT, 0.0f) / WreckFloodSeconds);
}

inline float WreckBuoyancyScale(float flood)
{
    return 1.0f - (1.0f - WreckResidualBuoyancy) * std::clamp(flood, 0.0f, 1.0f);
}

inline float WreckImmersion(float under)
{
    return std::min(under, WreckMaxImmersion);
}
}
