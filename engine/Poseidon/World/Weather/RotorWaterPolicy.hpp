#pragma once
#include <cmath>
namespace Poseidon
{
// Native sea datum / seabed, independent of island name and camera altitude.
inline bool RotorSeaWashEligible(float rpm, float aircraftY, float groundY, float seaY)
{
    if (!std::isfinite(rpm) || !std::isfinite(aircraftY) || !std::isfinite(groundY) || !std::isfinite(seaY))
        return false;
    const float altitude = aircraftY - seaY;
    return rpm > .02f && rpm <= 1 && altitude >= -2 && altitude <= 30 && groundY <= seaY + .25f;
}
} // namespace Poseidon
