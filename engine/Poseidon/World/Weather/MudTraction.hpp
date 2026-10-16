#pragma once

#include <algorithm>
#include <cmath>

// Simple wet-mud traction for wheeled vehicles. Pure functions, no engine state,
// so every rule here is unit-tested (test_mud_traction.cpp).
//
// The model is deliberately small. It is not a tyre solver; it scales the
// existing OFP-style car forces:
//   - grip       : 1 on firm ground, falls with wetness, the share of wheels in
//                  mud and how deep they already sit. Scales drive force and
//                  braking; side grip falls less so the truck slides, not skates.
//   - wheel spin : heavy throttle with little grip and little ground speed spins
//                  the wheels. Spinning digs the rut deeper, which lowers grip
//                  further -- easing off (or rocking) is how you get out.
//   - spray      : particle density for mud thrown off spinning wheels.
namespace Poseidon
{
// Same softening curve as MudField::ContactDepth: nothing below 15% wetness,
// smooth ramp to fully soft.
inline float MudSoftness(float wetness)
{
    if (!std::isfinite(wetness)) return 0.0f;
    const float t = std::clamp((wetness - 0.15f) / 0.85f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Rut depth relative to the soft layer (0 = untouched, 1 = a full-layer rut).
inline float MudSinkRatio(float sinkDepth, float layerDepth)
{
    if (!std::isfinite(sinkDepth) || !std::isfinite(layerDepth) || sinkDepth <= 0.0f) return 0.0f;
    return std::clamp(sinkDepth / std::max(layerDepth, 0.05f), 0.0f, 1.0f);
}

constexpr float MudMinGrip = 0.2f;

// Overall traction factor in [MudMinGrip, 1]. gripLoss is the vehicle's largest
// possible loss (profile value) reached in soaked ground, all wheels in a deep rut.
inline float MudGrip(float wetness, float sinkDepth, float layerDepth,
                     int mudWheels, int totalWheels, float gripLoss)
{
    if (mudWheels <= 0 || totalWheels <= 0 || !std::isfinite(gripLoss) || gripLoss <= 0.0f) return 1.0f;
    const float share = std::clamp(float(mudWheels) / float(totalWheels), 0.0f, 1.0f);
    const float sink = MudSinkRatio(sinkDepth, layerDepth);
    // Wetness does most of it; sitting in a rut adds a little more. (Depth used to
    // weigh almost half, and since first passes now press deep ruts, that alone
    // stalled the truck on soaked ground.)
    const float loss = std::clamp(gripLoss, 0.0f, 0.8f) * MudSoftness(wetness) * share * (0.75f + 0.25f * sink);
    return std::clamp(1.0f - loss, MudMinGrip, 1.0f);
}

// Side grip falls half as much as drive grip: the truck drifts wide in corners
// and slips a little on cambers, but never slides off a gentle slope by itself.
inline float MudSideGrip(float grip)
{
    if (!std::isfinite(grip)) return 1.0f;
    return 0.5f + 0.5f * std::clamp(grip, 0.0f, 1.0f);
}

// How hard the wheels want to spin, 0..1. Needs throttle, missing grip, and the
// vehicle not already rolling well (a truck cruising through a puddle of mud
// keeps rolling instead of spinning up).
inline float MudWheelSpinTarget(float grip, float thrust, float forwardSpeed, float maxSpeed)
{
    if (!std::isfinite(grip) || !std::isfinite(thrust) || !std::isfinite(forwardSpeed) ||
        !std::isfinite(maxSpeed) || grip >= 0.999f) return 0.0f;
    const float demand = std::clamp(std::abs(thrust), 0.0f, 1.0f);
    if (demand < 0.2f) return 0.0f;
    const float rolling = std::abs(forwardSpeed) / (0.35f * std::max(maxSpeed, 1.0f) + 0.1f);
    const float stall = std::clamp(1.0f - rolling, 0.0f, 1.0f);
    return std::clamp((demand - 0.2f) * 1.25f * (1.0f - grip) * 1.2f * (0.3f + 0.7f * stall), 0.0f, 1.0f);
}

// Spin builds over about a third of a second and dies away over about a second.
inline float MudSpinApproach(float current, float target, float seconds)
{
    if (!std::isfinite(current)) current = 0.0f;
    if (!std::isfinite(target) || !std::isfinite(seconds) || seconds <= 0.0f) return std::clamp(current, 0.0f, 1.0f);
    target = std::clamp(target, 0.0f, 1.0f);
    const float rate = target > current ? 3.0f : 1.2f;
    const float step = rate * seconds;
    const float next = target > current ? std::min(target, current + step) : std::max(target, current - step);
    return std::clamp(next, 0.0f, 1.0f);
}

// Rut depth a spinning wheel digs towards (metres). Never deeper than digMax.
inline float MudDigDepth(float spin, float contactDepth, float digMax)
{
    if (!std::isfinite(spin) || !std::isfinite(contactDepth) || !std::isfinite(digMax) ||
        spin <= 0.0f || contactDepth <= 0.0f || digMax <= 0.0f) return 0.0f;
    return std::min(digMax, contactDepth * (0.6f + 0.8f * std::clamp(spin, 0.0f, 1.0f)));
}

// Fraction of the remaining dig applied this step: a fully spinning wheel needs
// around ten seconds to bury itself, a brief wheelspin barely marks the ground.
inline float MudDigPressure(float spin, float seconds)
{
    if (!std::isfinite(spin) || !std::isfinite(seconds) || spin <= 0.0f || seconds <= 0.0f) return 0.0f;
    return std::clamp(std::clamp(spin, 0.0f, 1.0f) * seconds * 0.15f, 0.0f, 1.0f);
}

// Deepest rut that repeated passes can press (metres): the vehicle's dig limit,
// but no deeper than twice the current soft-layer contact depth, so damp ground
// only takes shallow ruts and soaked ground takes deep ones. Never below `base`.
inline float MudRutCap(float digMax, float contactDepth, float base)
{
    if (!std::isfinite(base) || base <= 0.0f) return 0.0f;
    if (!std::isfinite(digMax) || !std::isfinite(contactDepth)) return base;
    return std::max(base, std::min(digMax, 2.0f * contactDepth));
}

// Depth the very first wheel presses into untouched ground: most of the way from
// the plain contact depth (`base`) to the rut cap, so a single drive already
// leaves the deep, churned rut that used to need five to eight passes.
// Later passes still deepen it a little up to `cap` (MudRutPassDepth).
inline float MudFirstPassDepth(float base, float cap, float share = 0.7f)
{
    if (!std::isfinite(base) || base <= 0.0f) return 0.0f;
    if (!std::isfinite(cap) || cap <= base) return base;
    share = std::isfinite(share) ? std::clamp(share, 0.0f, 1.0f) : 0.0f;
    return base + (cap - base) * share;
}

// Target depth for one wheel stamp. Fresh ground is pressed to `base`; ground
// that already holds a rut is pressed a little deeper each time, up to `cap`.
// Wheel stamps are spaced by distance (MudWheelTrail, 0.25 m) and each cell is
// covered by several of them per pass; each stamp's soft edge absorbs most of
// the extra, so `gain` (share of `base` per stamp) of 0.2 adds ~12% of `base`
// per wheel pass. Soaked ground, Truck5t (3 axles on one track): ~13 cm after
// one wheel, ~17 cm after one drive, ~24 cm after two, the 28 cm cap after ~3.
inline float MudRutPassDepth(float stored, float base, float cap, float gain = 0.2f)
{
    if (!std::isfinite(base) || base <= 0.0f) return 0.0f;
    if (!std::isfinite(cap) || cap < base) cap = base;
    // The field's smoothing and slope limit leave a fresh rut a little short of
    // `base`, so anything past 80% of it already counts as an existing rut.
    if (!std::isfinite(stored) || stored < 0.8f * base) return base;
    if (!std::isfinite(gain) || gain < 0.0f) gain = 0.0f;
    return std::min(cap, std::max(base, stored + base * gain));
}

// Extra rolling resistance from pushing through an existing rut wall.
inline float MudSinkResistanceScale(float sinkDepth, float layerDepth)
{
    return 1.0f + 0.6f * MudSinkRatio(sinkDepth, layerDepth);
}

// Ceiling on mud rolling resistance (m/s^2). Soaked ground with deep ruts used
// to reach ~2.7 m/s^2, more than a half-gripping truck can push; this keeps it
// hard work, never a wall.
constexpr float MudMaxRollingDeceleration = 1.4f;
inline float MudLimitRollingDeceleration(float deceleration)
{
    if (!std::isfinite(deceleration)) return 0.0f;
    return std::clamp(deceleration, -MudMaxRollingDeceleration, MudMaxRollingDeceleration);
}

// Visible mud spray density 0..1: mostly from wheelspin, a little from speed.
inline float MudSprayDensity(float spin, float speed, float softness)
{
    if (!std::isfinite(spin) || !std::isfinite(speed) || !std::isfinite(softness)) return 0.0f;
    const float amount = std::clamp(spin, 0.0f, 1.0f) * 0.9f + std::clamp(std::abs(speed) * 0.03f, 0.0f, 0.3f);
    return std::clamp(amount * std::clamp(softness, 0.0f, 1.0f), 0.0f, 1.0f);
}

// Radius of a wheel rut stamp: about half a truck tyre's width, widening a little
// as the wheel sinks (soil slumps in from the sides). Boot footprints keep
// MudField::ContactRadius; that one grows far too wide for a tyre.
inline float MudWheelRutRadius(float depth)
{
    if (!std::isfinite(depth) || depth <= 0.0f) return 0.25f;
    return std::clamp(0.22f + 1.1f * depth, 0.25f, 0.6f);
}

// ---- Uneven ruts -------------------------------------------------------------
// Deterministic noise in [0,1) tied to a ground position, so the same spot of a
// rut always gets the same lump (no RNG state, identical on replays).
inline float MudNoise(float x, float z, unsigned salt)
{
    if (!std::isfinite(x) || !std::isfinite(z)) return 0.5f;
    const int ix = int(std::floor(x * 4.0f)), iz = int(std::floor(z * 4.0f));   // 25 cm cells
    unsigned h = unsigned(ix) * 73856093u ^ unsigned(iz) * 19349663u ^ (salt + 1u) * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return float(h & 0xffffffu) / float(0x1000000u);
}

// One wheel stamp, made uneven: depth and width vary, the centre wanders sideways,
// and displaced soil heaps up into a lumpy ridge on each side of the rut.
struct MudRutStamp
{
    float x = 0, z = 0, radius = 0, depth = 0;      // the rut press
    float rimX[2]{}, rimZ[2]{}, rimRadius[2]{}, rimHeight[2]{};
};

// (x,z): wheel contact; (dirX,dirZ): direction of travel (any length);
// radius/depth: the even stamp. spoil 0..1 extra soil thrown out (wheelspin).
inline MudRutStamp PlanMudRutStamp(float x, float z, float dirX, float dirZ,
                                  float radius, float depth, float spoil = 0.0f)
{
    MudRutStamp s;
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) || !std::isfinite(depth) ||
        radius <= 0.0f || depth <= 0.0f) return s;
    float len = std::hypot(dirX, dirZ);
    if (!std::isfinite(len) || len < 1e-4f) { dirX = 1.0f; dirZ = 0.0f; len = 1.0f; }
    dirX /= len; dirZ /= len;
    const float sideX = -dirZ, sideZ = dirX;
    spoil = std::isfinite(spoil) ? std::clamp(spoil, 0.0f, 1.0f) : 0.0f;
    // Rut: depth +-20%, width -10..+15%, centre wanders up to +-4 cm.
    const float wander = (MudNoise(x, z, 3) - 0.5f) * 0.08f;
    s.x = x + sideX * wander; s.z = z + sideZ * wander;
    s.depth = depth * (0.8f + 0.4f * MudNoise(x, z, 1));
    s.radius = radius * (0.9f + 0.25f * MudNoise(x, z, 2));
    // Ridges: just outside the rut edge, lumpy height, irregular width and spacing.
    for (int k = 0; k < 2; ++k)
    {
        const float sign = k == 0 ? -1.0f : 1.0f;
        const unsigned salt = 10u + unsigned(k) * 10u;
        const float out = s.radius * (1.0f + 0.15f * MudNoise(x, z, salt)) + 0.04f;
        const float jitter = (MudNoise(x, z, salt + 1) - 0.5f) * 0.2f;
        s.rimX[k] = s.x + sideX * sign * out + dirX * jitter;
        s.rimZ[k] = s.z + sideZ * sign * out + dirZ * jitter;
        s.rimRadius[k] = s.radius * (0.45f + 0.3f * MudNoise(x, z, salt + 2)) * (1.0f + 0.3f * spoil);
        const float lump = 0.25f + 0.35f * MudNoise(x, z, salt + 3);
        s.rimHeight[k] = std::min(0.10f, s.depth * lump * (1.0f + 0.8f * spoil));
    }
    return s;
}

// ---- Track ruts -------------------------------------------------------------
// One track pressed into the mud: a wide, flat-bottomed band rather than the
// round groove of a tyre. Three overlapping presses across the belt (the field's
// press is a smooth bowl; side by side they flatten the floor) and the spoil
// pushed out to both sides as ridges just outside the belt edges.
struct MudTrackStamp
{
    float x[3]{}, z[3]{}, radius = 0, depth[3]{};
    float rimX[2]{}, rimZ[2]{}, rimRadius[2]{}, rimHeight[2]{};
    bool valid = false;
};

// (x,z): track centre on the ground; (dirX,dirZ): travel direction;
// halfWidth: half the belt width (m); depth: target rut depth; spoil 0..1 (slip).
inline MudTrackStamp PlanMudTrackStamp(float x, float z, float dirX, float dirZ,
                                      float halfWidth, float depth, float spoil = 0.0f)
{
    MudTrackStamp s;
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(halfWidth) || !std::isfinite(depth) ||
        depth <= 0.0f) return s;
    halfWidth = std::clamp(halfWidth, 0.15f, 0.6f);
    float len = std::hypot(dirX, dirZ);
    if (!std::isfinite(len) || len < 1e-4f) { dirX = 1.0f; dirZ = 0.0f; len = 1.0f; }
    dirX /= len; dirZ /= len;
    const float sideX = -dirZ, sideZ = dirX;
    spoil = std::isfinite(spoil) ? std::clamp(spoil, 0.0f, 1.0f) : 0.0f;
    // Soil is uneven: depth +-15% per press, the band wanders a couple of cm.
    const float wander = (MudNoise(x, z, 33) - 0.5f) * 0.05f;
    s.radius = halfWidth * 0.8f;
    for (int k = 0; k < 3; ++k)
    {
        const float across = (float(k) - 1.0f) * halfWidth * 0.55f + wander;
        s.x[k] = x + sideX * across;
        s.z[k] = z + sideZ * across;
        s.depth[k] = depth * (0.85f + 0.3f * MudNoise(x, z, 40u + unsigned(k)));
    }
    for (int k = 0; k < 2; ++k)
    {
        const float sign = k == 0 ? -1.0f : 1.0f;
        const unsigned salt = 50u + unsigned(k) * 10u;
        const float out = halfWidth * (1.15f + 0.2f * MudNoise(x, z, salt)) + 0.06f + wander * sign;
        const float jitter = (MudNoise(x, z, salt + 1) - 0.5f) * 0.25f;
        s.rimX[k] = x + sideX * sign * out + dirX * jitter;
        s.rimZ[k] = z + sideZ * sign * out + dirZ * jitter;
        s.rimRadius[k] = halfWidth * (0.45f + 0.3f * MudNoise(x, z, salt + 2)) * (1.0f + 0.4f * spoil);
        const float lump = 0.3f + 0.4f * MudNoise(x, z, salt + 3);
        s.rimHeight[k] = std::min(0.10f, depth * lump * (1.0f + spoil));
    }
    s.valid = true;
    return s;
}

} // namespace Poseidon
