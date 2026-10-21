#pragma once

#include <algorithm>
#include <cmath>

// Mud caked on a vehicle: how dirty its wheels and its body are, 0 (clean) to 1
// (plastered). Pure maths, unit-tested in test_mud_coat.cpp. The renderer turns
// the two numbers into brown mud on the lower part of the model (shader3d.wgsl,
// vehicle_mud_cover): wheels and the bottom first, creeping up the body the
// longer you drive through mud.
namespace Poseidon
{
struct MudCoat
{
    float wheels = 0.0f;
    float body = 0.0f;
};

// seconds: step; soft: MudSoftness of the ground under the wheels (0 off mud);
// speed: ground speed m/s; spin: wheelspin 0..1; waterDepth: how deep the
// vehicle's lowest points are in water, metres (0 = dry); rain: 0..1.
//
// Rates (damp field, soft ~0.6, no wheelspin): tyres visibly muddy within a few
// seconds and caked after ~15 s, well before the body; the body about half covered after
// ~1.5 minutes and plastered after ~4 minutes. Wheelspin is much faster.
//
// Washing follows how deep you are and how fast you push through (the bow wave
// and splash do the scrubbing). Tested in game: a flat "in water = wash" made a
// truck spotless within a second of a tyre touching a stream. Now:
//   - tyre just dipping in (10 cm): wheels barely change, body not at all;
//   - wheel-deep ford (60 cm) at walking pace: tyres mostly clean in ~30 s,
//     the body only a little;
//   - deep ford (1.2 m+): the body clears over ~1.5-2 minutes;
//   - standing still in water soaks slowly, driving through cleans faster.
// Heavy rain rinses everything over ~10 minutes.
// pickup: vehicle's coat rate (MudWheelProfile::coatRate; light cars < 1).
inline void AdvanceMudCoat(MudCoat& coat, float seconds, float soft, float speed, float spin,
                           float waterDepth, float rain, float pickup = 1.0f)
{
    pickup = std::isfinite(pickup) ? std::clamp(pickup, 0.0f, 2.0f) : 1.0f;
    if (!std::isfinite(seconds) || seconds <= 0.0f) return;
    soft = std::isfinite(soft) ? std::clamp(soft, 0.0f, 1.0f) : 0.0f;
    speed = std::isfinite(speed) ? std::min(std::abs(speed), 20.0f) : 0.0f;
    spin = std::isfinite(spin) ? std::clamp(spin, 0.0f, 1.0f) : 0.0f;
    rain = std::isfinite(rain) ? std::clamp(rain, 0.0f, 1.0f) : 0.0f;
    waterDepth = std::isfinite(waterDepth) ? std::clamp(waterDepth, 0.0f, 5.0f) : 0.0f;
    if (!std::isfinite(coat.wheels)) coat.wheels = 0.0f;
    if (!std::isfinite(coat.body)) coat.body = 0.0f;

    if (waterDepth > 0.02f)
    {
        // Moving water scrubs; still water only soaks the mud off slowly.
        const float splash = 0.35f + 0.65f * std::min(speed / 4.0f, 1.0f);
        const float wheelSub = std::clamp(waterDepth / 0.6f, 0.0f, 1.0f);       // tyre under water
        const float t = std::clamp((waterDepth - 0.4f) / 0.9f, 0.0f, 1.0f);     // body going under
        const float bodySub = t * t * (3.0f - 2.0f * t);
        // Tested in game twice: still too quick. Now slower than the mud built
        // up, so the patches shrink and break up visibly (the same coverage curve
        // as gathering, run backwards) instead of the coat just vanishing.
        const float wheelRate = 0.08f * wheelSub * wheelSub * splash;          // ~30 s wheel-deep, moving
        const float bodyRate = (0.025f * bodySub + 0.002f * wheelSub) * splash; // ~1.5 min fully under
        coat.wheels -= coat.wheels * std::min(1.0f, wheelRate * seconds);
        coat.body -= coat.body * std::min(1.0f, bodyRate * seconds);
    }
    else if (soft > 0.0f)
    {
        // Rolling picks mud up with distance, spinning throws it everywhere.
        // Tuned in game: the first rates needed a long drive before anything showed
        // on a damp field. Tyres now pick mud up from the first metre; the body
        // follows over half a minute to a minute. sqrt: damp ground still sticks.
        const float moving = std::min(speed / 6.0f, 1.0f);
        const float stick = std::sqrt(soft) * pickup;
        // Second pass: that was too quick (tyres and sills fully caked within
        // metres). Spots still show from the first seconds, the tyres cake over
        // about half a minute, the body over a few minutes; spinning is faster.
        // Third pass: the tyres must clearly lead (caked in ~15 s) while the body
        // stays slow, so the two read as separate stages.
        const float wheelGain = stick * (0.12f + 0.12f * moving + 0.6f * spin);
        const float bodyGain = stick * (0.008f * moving + 0.08f * spin);
        coat.wheels += (1.0f - coat.wheels) * std::min(1.0f, wheelGain * seconds);
        coat.body += (1.0f - coat.body) * std::min(1.0f, bodyGain * seconds);
        // The body never looks dirtier than the wheels that threw the mud.
        coat.body = std::min(coat.body, coat.wheels);
    }
    if (rain > 0.0f && waterDepth <= 0.02f)
    {
        const float rinse = std::min(1.0f, rain * seconds / 600.0f);
        coat.wheels -= coat.wheels * rinse;
        coat.body -= coat.body * rinse;
    }
    coat.wheels = std::clamp(coat.wheels, 0.0f, 1.0f);
    coat.body = std::clamp(coat.body, 0.0f, 1.0f);
}

// One float for the renderer's per-draw material lane (Material.light_ambient.w,
// whose ordinary value is the material ambient alpha, never negative):
//   -(1 + round(wheels * 127) + min(body, 0.999))
// Anything >= 0 means "no mud". Decoded by vehicle_mud_cover in shader3d.wgsl.
inline float EncodeMudCoat(const MudCoat& coat)
{
    const float wheels = std::isfinite(coat.wheels) ? std::clamp(coat.wheels, 0.0f, 1.0f) : 0.0f;
    const float body = std::isfinite(coat.body) ? std::clamp(coat.body, 0.0f, 0.999f) : 0.0f;
    return -(1.0f + std::round(wheels * 127.0f) + body);
}

// The vehicle's up axis in one float (Material.sun_diffuse.w, otherwise unread),
// so the renderer measures mud height along the vehicle, not the world: on a
// slope or a bump the mud line stays on the panels instead of sliding over the
// bonnet. x and z quantised to 12 bits each; y is rebuilt (always positive for
// an upright vehicle; a vehicle on its side gets y = 0).
inline float PackMudUp(float x, float y, float z)
{
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(len) || len < 1e-4f) { x = 0; z = 0; }
    else { x /= len; z /= len; }
    const float qx = std::round((std::clamp(x, -1.0f, 1.0f) * 0.5f + 0.5f) * 4095.0f);
    const float qz = std::round((std::clamp(z, -1.0f, 1.0f) * 0.5f + 0.5f) * 4095.0f);
    return qx * 4096.0f + qz;
}

struct MudUp { float x = 0, y = 1, z = 0; };
inline MudUp UnpackMudUp(float packed)
{
    MudUp up;
    if (!std::isfinite(packed) || packed < 0.0f || packed > 4096.0f * 4096.0f) return up;
    const float qx = std::floor(packed / 4096.0f);
    const float qz = packed - qx * 4096.0f;
    up.x = qx / 4095.0f * 2.0f - 1.0f;
    up.z = qz / 4095.0f * 2.0f - 1.0f;
    up.y = std::sqrt(std::max(0.0f, 1.0f - up.x * up.x - up.z * up.z));
    return up;
}

inline MudCoat DecodeMudCoat(float encoded)
{
    MudCoat coat;
    if (!std::isfinite(encoded) || encoded > -1.0f) return coat;
    const float t = -encoded - 1.0f;
    const float whole = std::floor(t + 1e-4f);
    coat.wheels = std::clamp(whole / 127.0f, 0.0f, 1.0f);
    coat.body = std::clamp(t - whole, 0.0f, 1.0f);
    return coat;
}
} // namespace Poseidon
