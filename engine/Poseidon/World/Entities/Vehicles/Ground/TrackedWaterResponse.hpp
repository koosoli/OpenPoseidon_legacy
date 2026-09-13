#pragma once

#include <algorithm>

namespace Poseidon
{
// Tracked amphibians use a stiff legacy displacement spring. Let them follow
// waves, but do not feed the full modern swell height into that spring.
inline float AmphibiousTrackedImmersion(float under, float underFlat)
{
    constexpr float MaxWaveExcursion = 0.35f;
    const float waveExcursion = std::clamp(under - underFlat, -MaxWaveExcursion, MaxWaveExcursion);
    return std::max(0.0f, underFlat + waveExcursion);
}
}
