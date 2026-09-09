#pragma once

namespace Poseidon
{
inline bool CanObserveWithBinoculars(bool selected, float observationTime, float requestedSpeed)
{
    // Use movement intent, not actual speed: a raised optic can keep speed zero.
    return selected && observationTime > 0.0f && requestedSpeed >= -0.1f && requestedSpeed <= 0.1f;
}
}
