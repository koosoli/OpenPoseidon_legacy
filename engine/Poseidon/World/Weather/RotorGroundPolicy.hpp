#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Poseidon
{
struct RotorGroundEmission
{
    float density = 0;
    bool snow = false;
};

struct RotorLandAppearance
{
    float radius = 0; // billboard half-width/half-height in world metres
    float opacity = 0;
};
inline constexpr float RotorLandMaxRadius = 3.1f;
inline constexpr float RotorLandMaxTravel = 3.5f;
inline std::array<float,3> RotorLandTint(bool snow)
{
    // The stock AI88 sheet has white RGB: the authored tint supplies all earth
    // colour. Keep warm mineral dust, slightly darkened against sunlit sand.
    // This is albedo only; Draw retains ordinary weather/eye light and alpha.
    return snow ? std::array<float,3>{.90f,.94f,.98f} : std::array<float,3>{.46f,.40f,.30f};
}
// The square billboard fits inside sqrt(2)*radius. Reserve its entire future
// extent and the bounded outward travel before publishing a puff.
inline constexpr float RotorLandProofRadius = 1.414214f * RotorLandMaxRadius + RotorLandMaxTravel;
inline std::array<float, 2> RotorLandFootprint(unsigned probe)
{
    constexpr std::array<std::array<float, 2>, 5> offsets{{
        {0,0}, {RotorLandProofRadius,0}, {-RotorLandProofRadius,0},
        {0,RotorLandProofRadius}, {0,-RotorLandProofRadius}}};
    return probe < offsets.size() ? offsets[probe] : std::array<float, 2>{};
}
inline float RotorLandCullScale(float radius, float shapeSphere)
{
    if (!std::isfinite(radius) || !std::isfinite(shapeSphere) || radius <= 0 || shapeSphere <= 0)
        return 0;
    const float scale = 1.414214f * radius / shapeSphere;
    return std::isfinite(scale) && scale <= 10000.0f ? scale : 0;
}
// Conservative refusal, not a collision hit: the engine's thin convex ray
// narrowphase does not actually expand by its radius parameter. Refuse an
// object's enclosing sphere anywhere in the complete lifetime corridor.
inline bool RotorLandBoundsBlock(std::array<float,3> from, std::array<float,3> to,
                                std::array<float,3> centre, float radius)
{
    if (!std::isfinite(radius) || radius < 0) return true;
    float length2=0, dot=0;
    for (unsigned i=0;i<3;++i) {
        if (!std::isfinite(from[i]) || !std::isfinite(to[i]) || !std::isfinite(centre[i])) return true;
        const float d=to[i]-from[i]; length2+=d*d; dot+=(centre[i]-from[i])*d;
    }
    if (!std::isfinite(length2) || !std::isfinite(dot)) return true;
    const float t=length2>0?std::clamp(dot/length2,0.0f,1.0f):0;
    float distance2=0;
    for (unsigned i=0;i<3;++i) {
        const float d=centre[i]-(from[i]+(to[i]-from[i])*t); distance2+=d*d;
    }
    const float sum=radius+RotorLandProofRadius;
    return !std::isfinite(distance2) || !std::isfinite(sum*sum) || distance2<=sum*sum;
}
inline RotorLandAppearance RotorLandProfile(float age, float density, bool snow)
{
    if (!std::isfinite(age) || !std::isfinite(density) || age < 0 || density <= 0)
        return {};
    const float life = snow ? 1.7f : 1.9f;
    if (age >= life) return {};
    const auto smooth = [](float t) { t=std::clamp(t,0.0f,1.0f); return t*t*(3-2*t); };
    const float start = snow ? 1.6f : 1.8f;
    const float end = snow ? 2.7f : RotorLandMaxRadius;
    const float radius = start + (end-start)*smooth(age/.65f);
    // The stock animated sheet already averages about .32 alpha. Dividing
    // opacity again by expanded area made a normal-height ring barely visible.
    // This is a diffuse optical profile, not a particle-mass simulation: keep
    // actual thrust/source/wetness density. The source policy already squares
    // height attenuation for entrainment; optical alpha uses its square root
    // so ordinary 19-20m hover does not vanish into the soft sheet. The same
    // low-altitude caps and no radius-dependent opacity loss remain. Fades remove
    // material smoothly, and the complete expanded footprint stays proofed.
    const float opacity = std::min(snow ? .28f : .35f,
        (snow ? .60f : .70f)*std::sqrt(std::clamp(density,0.0f,1.0f))) *
        smooth(age/.12f) * smooth((life-age)/.65f);
    return {radius, opacity};
}

// Across all aircraft: at most 32 support/roof probes per quarter simulation
// second. Large sheltered fleets cannot spend an unbounded ray budget merely
// because they emit no particles and therefore never fill the particle cap.
class RotorGroundBudget
{
public:
    bool Take(float simulationTime)
    {
        if (!std::isfinite(simulationTime) || simulationTime < 0) return false;
        const double bucket = std::floor(double(simulationTime) * 4.0);
        if (bucket != _bucket) { _bucket = bucket; _remaining = 32; }
        if (_remaining == 0) return false;
        --_remaining; return true;
    }
private:
    double _bucket = -1;
    unsigned _remaining = 0;
};

// Visible entrainment only: this never edits terrain or water. Each candidate
// point must independently pass actual land, roadway and roof geometry proof.
inline RotorGroundEmission RotorGroundPolicy(float rpm, float height, float up,
    float dustness, float snowDepth, float wetness, float rain,
    float waterDepth, bool waterValid, bool land, bool sheltered, bool roadway)
{
    for (float value : {rpm, height, up, dustness, snowDepth, wetness, rain, waterDepth})
        if (!std::isfinite(value)) return {};
    if (!land || sheltered || roadway || rpm <= 0.2f || height < 0.0f ||
        height >= 30.0f || up < 0.5f || (waterValid && waterDepth > 0.003f)) return {};
    const float thrust = std::clamp(rpm, 0.0f, 1.0f);
    const float attenuation = 1.0f - height / 30.0f;
    const float force = thrust * thrust * attenuation * attenuation;
    if (snowDepth > 0.002f) return {force, true};
    // Authored dustness, rather than brown colour or a guessed map filename.
    const float dry = (1.0f - std::clamp(wetness, 0.0f, 1.0f)) *
                      (1.0f - std::clamp(rain, 0.0f, 1.0f));
    return {force * std::clamp(dustness * 2.5f, 0.0f, 1.0f) * dry * dry, false};
}

inline std::array<float, 2> RotorGroundRing(unsigned phase, unsigned spoke, float radius)
{
    if (!std::isfinite(radius) || radius <= 0) return {};
    // A private deterministic sequence leaves gameplay's random stream alone.
    const uint32_t hash = (phase * 747796405u + spoke * 2891336453u + 277803737u);
    const float jitter = float((hash >> 8) & 0xffffu) / 65535.0f;
    constexpr float tau = 6.283185307179586f;
    const float angle = tau * (float(spoke % 8) + jitter * 0.65f) / 8.0f;
    const float distance = std::clamp(radius, 3.0f, 12.0f) * (0.7f + 0.25f * jitter);
    return {std::cos(angle) * distance, std::sin(angle) * distance};
}
} // namespace Poseidon
