#pragma once
#include <algorithm>
#include <cmath>

namespace Poseidon
{
// LightSun::SunDirection is the astronomical LIGHT-TRAVEL direction, saved
// before the night moonlight swap. A daylight sun above the horizon has Y<0.
// Keep physical retained-water and cosmetic substrate drying on that same
// actual daylight/cloud contract, independent of render-camera or moonlight.
inline float SunlightDryingExposure(float sunTravelY, float overcast, float nightEffect)
{
    if (!std::isfinite(sunTravelY) || !std::isfinite(overcast) || !std::isfinite(nightEffect))
        return 0.0f;
    const float elevation = std::clamp(-sunTravelY, 0.0f, 1.0f);
    const float clear = 1.0f - std::clamp(overcast, 0.0f, 1.0f);
    const float daylight = 1.0f - std::clamp(nightEffect, 0.0f, 1.0f);
    return elevation * clear * clear * daylight;
}
}
