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
