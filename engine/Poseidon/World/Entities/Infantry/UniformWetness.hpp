#pragma once

#include <algorithm>
#include <cmath>

namespace Poseidon
{
// Weather rain remains authoritative even when its visual layer is disabled.
// A visible liquid-particle override also wets cloth, as it already wets the
// terrain and sea. Snowflake overrides alone do not stand in for liquid rain.
// effectiveParticleRain is the read-only RainSystem::EffectiveDensity value:
// that producer already returns zero for particle-off/legacy-only modes.
inline float UniformWettingDensity(float weatherRain, float effectiveParticleRain, bool particleSnowflakes)
{
    const auto bounded = [](float density) {
        return std::isfinite(density) ? std::clamp(density, 0.0f, 1.0f) : 0.0f;
    };
    return std::max(bounded(weatherRain), particleSnowflakes ? 0.0f : bounded(effectiveParticleRain));
}

// Cosmetic water held by cloth, integrated in actual simulation seconds. Solve
// dw/dt = rain/45*(1-w) - (1-rain)/600*w exactly for constant exposure, so
// accelerated time and different simulation steps do not change the result.
inline float AdvanceUniformWetness(float wetness, float rain, bool sheltered, float deltaT)
{
    wetness = std::isfinite(wetness) ? std::clamp(wetness, 0.0f, 1.0f) : 0.0f;
    if (!std::isfinite(deltaT) || deltaT <= 0.0f)
        return wetness;
    rain = sheltered || !std::isfinite(rain) ? 0.0f : std::clamp(rain, 0.0f, 1.0f);
    const double fill = double(rain) / 45.0;
    const double dry = (1.0 - double(rain)) / 600.0;
    const double rate = fill + dry;
    const double equilibrium = fill / rate;
    return float(std::clamp(equilibrium + (double(wetness) - equilibrium) * std::exp(-rate * deltaT),
                            0.0, 1.0));
}
} // namespace Poseidon
