#pragma once

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace Poseidon::EarthStreaming
{
// Shared presentation/camera gate; this opt-in does not replace simulation land.
inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("POSEIDON_WHOLE_WORLD");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
constexpr float CoordinateLimit = 500000.0f;
constexpr float OrbitAltitudeLimit = 30000000.0f;
inline float ZoomAltitude(float altitude, float wheelSteps)
{
    if (!std::isfinite(altitude) || !std::isfinite(wheelSteps)) return 4500.0f;
    return std::clamp((altitude+1000.0f)*std::pow(0.72f,std::clamp(wheelSteps,-20.0f,20.0f))-1000.0f,
        500.0f,OrbitAltitudeLimit);
}
}
