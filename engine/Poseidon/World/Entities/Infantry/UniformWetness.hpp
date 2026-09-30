#pragma once

#include <algorithm>
#include <cmath>

namespace Poseidon
{
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
