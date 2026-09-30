#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace Poseidon
{
// Actor-owned, unsaved cosmetic event. Sound consumes the actual emitting sole,
// never a later animation phase or an inferred boot side. No simulation/RNG use.
struct SoundStepSole
{
    static constexpr int MaxAgeMs = 250;
    std::array<float, 3> position{};
    std::int64_t timeMs = 0;
    bool valid = false;

    void Clear() { valid = false; }
    void Capture(float x, float y, float z, std::int64_t time)
    {
        position = {x, y, z};
        timeMs = time;
        valid = std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    bool Fresh(std::int64_t time) const
    {
        return valid && time >= timeMs &&
            std::uint64_t(time) - std::uint64_t(timeMs) <= MaxAgeMs;
    }
};

// Standing RAIN water only. Actual sea contact remains the existing GroundWater
// sound route. A valid coarse water cell alone cannot wet a raised roadway/floor
// or a boot in the air. Shelter is an additional actual geometry query at caller.
inline constexpr float StandingWaterStepMinDepth = 0.003f; // a damp film is not a water step
inline bool StandingWaterFootstep(bool valid, float depth, float surfaceY,
                                 float soleY, float supportY, float seaLevel,
                                 bool landContact, bool attached)
{
    if (!valid || !landContact || attached || !std::isfinite(depth) ||
        !std::isfinite(surfaceY) || !std::isfinite(soleY) ||
        !std::isfinite(supportY) || !std::isfinite(seaLevel) || depth < StandingWaterStepMinDepth)
        return false;
    // Physical runoff bed is above sea; submerged shore cells are its outlet.
    // Exact support, not a texture label or the actor's torso, decides contact.
    return surfaceY - depth > seaLevel + 0.02f &&
        surfaceY - supportY >= StandingWaterStepMinDepth &&
        std::abs(soleY - supportY) <= 0.25f && soleY <= surfaceY + 0.12f;
}
} // namespace Poseidon
