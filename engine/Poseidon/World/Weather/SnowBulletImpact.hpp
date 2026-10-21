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
// The 3 m limit bounds work and the effect of extrapolating that local plane (it was 0.8 m:
// a grazing round's furrow in real snow runs a metre or more; owner, 2026-10-19: "bullets
// leave trails on the snow in the direction the bullet flew").
inline SnowBulletGroove SnowBulletGroovePolicy(float remainingSnow, float normalUp,
    float incidence, float incomingX, float incomingZ)
{
    for (float value : {remainingSnow, normalUp, incidence, incomingX, incomingZ})
        if (!std::isfinite(value)) return {};
    const float horizontal = std::hypot(incomingX, incomingZ);
    if (remainingSnow <= 0 || normalUp < 0.5f || normalUp > 1.001f ||
        incidence > -0.015f || incidence < -1.001f || horizontal < 0.0001f || horizontal > 1.001f)
        return {};
    const float length = std::clamp(remainingSnow * horizontal * normalUp / -incidence, 0.0f, 3.0f);
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
        incidence > -0.015f || incidence < -1.001f || std::abs(supportError) > 0.08f) return {};
    const float energy = std::sqrt(std::clamp(damage, 0.01f, 400.0f));
    // A bullet's furrow is narrow: one or two snow cells (0.125 m), not a 0.2-0.35 m bowl.
    return {std::clamp(0.12f + energy * 0.003f, 0.12f, 0.18f),
            // Only the disturbed snow around the hole: the hole itself is drawn sharp from the
            // bullet mark (SnowField::AddBulletMark). A deep 0.125 m-cell trench read as a
            // soft gully, not a bullet (owner, 2026-10-19: "would it not be a sharp hole").
            std::min(remainingSnow, std::clamp(0.03f + energy * 0.004f, 0.03f, 0.06f))};
}

// The track's radius in metres: a rifle round's track in snow is 4-6 cm across -- wider than the
// bullet, the snow round it collapses (owner, 2026-10-19: "the hole can also be bigger if it helps").
inline float SnowBulletHoleRadius(float damage)
{
    if (!std::isfinite(damage)) return 0.0f;
    const float energy = std::sqrt(std::clamp(damage, 0.01f, 400.0f));
    return std::clamp(0.014f + energy * 0.0018f, 0.02f, 0.03f);
}

// How much of a round's path under the snow shows at the surface (owner, 2026-10-19: "if i shoot
// at a wall of snow, i do not see a hole but a line"). A round tunnels: the crust above its path
// mostly stays, so what shows is the entry -- an oval, longer the flatter the shot (drawn from
// the angle) -- and a short broken furrow behind it, not the whole path to the ground.
inline float SnowBulletVisibleLength(float pathLength)
{
    if (!std::isfinite(pathLength) || pathLength <= 0.0f) return 0.0f;
    // (a 25 cm slit still read as a line at standing distance: the oval is the mark)
    return std::min(pathLength * 0.05f, 0.06f);
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
