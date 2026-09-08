// FAR-001: see AerialRange.hpp for what this is for and why the far plane and the
// fog range had to stop being the same number.

#include <Poseidon/World/Scene/Camera/AerialRange.hpp>

#include <cstdlib>

namespace Poseidon::Aerial
{

namespace
{

bool EnvBool(const char* name, bool fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return fallback;
    }
    return !(v[0] == '0' && v[1] == '\0');
}

void EnvFloat(const char* name, float& target)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return;
    }
    char* end = nullptr;
    const float parsed = std::strtof(v, &end);
    if (end != v)
    {
        target = parsed;
    }
}

void EnvInt(const char* name, int& target)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return;
    }
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end != v)
    {
        target = static_cast<int>(parsed);
    }
}

/// Environment overrides are read ONCE, on first use, matching the FlightCeiling
/// convention next door: they exist so a capture campaign can A/B the policy
/// across runs without a rebuild, and the dev panel is the in-session lever that
/// writes the same struct.
AerialRangeSettings MakeSettings()
{
    AerialRangeSettings s;
    s.enabled = EnvBool("POSEIDON_AERIAL_RANGE", s.enabled);
    s.coarseTerrain = EnvBool("POSEIDON_AERIAL_COARSE_TERRAIN", s.coarseTerrain);
    EnvFloat("POSEIDON_AERIAL_START_ALT", s.startAlt);
    EnvFloat("POSEIDON_AERIAL_FULL_ALT", s.fullAlt);
    EnvFloat("POSEIDON_AERIAL_REACH", s.reachFactor);
    EnvFloat("POSEIDON_AERIAL_MAX_REACH", s.maxReach);
    EnvFloat("POSEIDON_AERIAL_FINITE_REACH", s.finiteFarReach);
    EnvFloat("POSEIDON_AERIAL_CLIP_MARGIN", s.clipMargin);
    EnvInt("POSEIDON_AERIAL_COARSE_LOD", s.coarseLod);
    EnvFloat("POSEIDON_AERIAL_WATER_REACH", s.waterReach);
    return s;
}

/// Hermite smoothstep on [0,1]. Used for the altitude ramp rather than a linear
/// one because the derivative matters here: the far plane and the fog range both
/// move with `t`, and a linear ramp has a corner at each end that reads as the
/// haze visibly "starting" as an aircraft climbs through `startAlt`.
float SmoothStep01(float t)
{
    if (t <= 0.0f)
    {
        return 0.0f;
    }
    if (t >= 1.0f)
    {
        return 1.0f;
    }
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

AerialRangeSettings& Settings()
{
    static AerialRangeSettings s = MakeSettings();
    return s;
}

AerialRange Resolve(float fogBaseMaxRange, float altAboveGround, bool infiniteFarZ, const AerialRangeSettings& s)
{
    // The inert result. Returned by value and by identity: `farPlane` and
    // `fogMaxRange` are the caller's own input, not a recomputation of it, so a
    // ground-level frame cannot drift by a float ulp relative to the frame it
    // would have rendered before FAR-001 existed. Callers branch on `active`.
    const AerialRange inert{fogBaseMaxRange, fogBaseMaxRange, false};

    if (!s.enabled || fogBaseMaxRange <= 0.0f)
    {
        return inert;
    }

    // Altitude ABOVE GROUND. A negative value is not an error worth logging --
    // the camera can legitimately be below the terrain surface for a frame during
    // a teleport, inside a building's floor, or under the sea -- it simply means
    // the policy is inert.
    const float span = s.fullAlt - s.startAlt;
    if (altAboveGround <= s.startAlt || span <= 0.0f)
    {
        return inert;
    }

    const float t = SmoothStep01((altAboveGround - s.startAlt) / span);

    // The reach an eye at this altitude wants: a slant distance, so it scales
    // with altitude rather than with anything the player set. Ramped by `t` so it
    // grows in from zero across the transition band instead of stepping.
    float reach = altAboveGround * s.reachFactor;
    const float ceiling = infiniteFarZ ? s.maxReach : s.finiteFarReach;
    if (reach > ceiling)
    {
        reach = ceiling;
    }
    reach *= t;

    // Never shorten anything. Below the crossover the view-distance-derived range
    // is still the larger of the two, and a player who has deliberately set a
    // 10 km view distance must not lose it by climbing.
    if (reach <= fogBaseMaxRange)
    {
        return inert;
    }

    AerialRange r;
    r.fogMaxRange = reach;
    // Strictly greater than the fog range: geometry finishes dissolving into haze
    // before it reaches the clip plane, so there is no circular edge where terrain
    // stops existing. This is the whole reason the two numbers are separate rather
    // than one number used twice.
    r.farPlane = reach * (s.clipMargin > 1.0f ? s.clipMargin : 1.0f);
    r.active = true;
    return r;
}

} // namespace Poseidon::Aerial
