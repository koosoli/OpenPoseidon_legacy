#pragma once
#include <algorithm>
#include <cmath>

namespace Poseidon
{
struct SnowBulletCut { float radius = 0, depth = 0; };
struct SnowBulletGroove { float directionX = 0, directionZ = 0, length = 0; };

// The terminal point is the end of the groove. Intersect the actual incoming
// direction with the remaining snow layer above the local terrain plane; a
// shallow shot therefore opens an entry channel upstream, not a camera trail.
// The 0.8m limit bounds work and the effect of extrapolating that local plane.
inline SnowBulletGroove SnowBulletGroovePolicy(float remainingSnow, float normalUp,
    float incidence, float incomingX, float incomingZ)
{
    for (float value : {remainingSnow, normalUp, incidence, incomingX, incomingZ})
        if (!std::isfinite(value)) return {};
    const float horizontal = std::hypot(incomingX, incomingZ);
    if (remainingSnow <= 0 || normalUp < 0.5f || normalUp > 1.001f ||
        incidence > -0.04f || incidence < -1.001f || horizontal < 0.0001f || horizontal > 1.001f)
        return {};
    const float length = std::clamp(remainingSnow * horizontal * normalUp / -incidence, 0.0f, 0.8f);
    return {incomingX / horizontal, incomingZ / horizontal, length};
}

// Actual terminal terrain hits only. Object, fuse, sea and explosion behavior
// remains with the projectile; no visual or camera event can license this cut.
inline SnowBulletCut SnowBulletImpactPolicy(float speed, float damage, float remainingSnow,
    float normalUp, float incidence, float supportError, bool local, bool explosive,
    bool water, bool roadway, bool sheltered)
{
    for (float value : {speed, damage, remainingSnow, normalUp, incidence, supportError})
        if (!std::isfinite(value)) return {};
    if (!local || explosive || water || roadway || sheltered || speed < 30 || speed > 5000 ||
        damage <= 0 || remainingSnow <= 0.008f || normalUp < 0.5f || normalUp > 1.001f ||
        incidence > -0.04f || incidence < -1.001f || std::abs(supportError) > 0.08f) return {};
    const float energy = std::sqrt(std::clamp(damage, 0.01f, 400.0f));
    return {std::clamp(0.20f + energy * 0.009f, 0.20f, 0.35f),
            std::min(remainingSnow, std::clamp(0.05f + energy * 0.012f, 0.05f, 0.16f))};
}

// Across all shooters, <=8 admitted impact footprints per quarter simulation
// second. Each footprint asks five bounded support/roof queries, no fleet-sized
// unbounded ray loop. A clock reset starts a new bucket.
class SnowBulletBudget
{
public:
    bool Take(float simulationTime)
    {
        if (!std::isfinite(simulationTime) || simulationTime < 0) return false;
        const double bucket = std::floor(double(simulationTime) * 4.0);
        if (bucket != _bucket) { _bucket = bucket; _remaining = 8; }
        if (_remaining == 0) return false;
        --_remaining; return true;
    }
private:
    double _bucket = -1;
    unsigned _remaining = 0;
};
} // namespace Poseidon
