#pragma once

// Underwater bubbles: what a bullet and an explosion leave in the water.
//
//   * trail -- a round entering water opens a cavity behind it: a white streak for a
//     few hundredths of a second that collapses into a line of small bubbles, which
//     drift up and fade over a second or two.
//   * blast -- an underwater charge makes a flash, a gas bubble that swells and
//     collapses in a tenth of a second or so, and then a cloud of bubbles of every
//     size climbing to the surface.
//
// Like the muzzle burst, every bubble is a pure function of (event, age): events sit
// in small rings, nothing per bubble is stored or simulated. Drawn only while the
// camera is under water, through the same depth-tested world sprites as rain.

#include <Poseidon/Graphics/Rendering/Effects/MuzzleBurst.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Poseidon::render
{
struct BubbleTrail
{
    MuzzleBurstVec from, to; // the underwater path segment
    float speed = 0;         // round speed over this segment, m/s (bigger cavity when fast)
    float surfaceY = 1e9f;   // water surface: bubbles stop and burst there
    std::int64_t birthMs = 0;
    std::uint32_t seed = 0;
};

struct BubbleBlast
{
    MuzzleBurstVec centre;
    float radius = 1;    // maximum radius of the gas bubble, m
    float surfaceY = 0;  // water surface above it: bubbles stop there
    std::int64_t birthMs = 0;
    std::uint32_t seed = 0;
};

// Foam: a soft white mass of microbubbles (a trail's fizz, a blast's churn).
enum class BubbleKind { Cavity, Bubble, GasBubble, Flash, Foam };

struct BubbleSprite
{
    BubbleKind kind;
    MuzzleBurstVec head, tail; // tail == head for a round sprite
    float radius;              // metres
    float alpha;               // 0..1
};

constexpr int BubbleTrailLifetimeMs = 2400;
constexpr int BubbleBlastLifetimeMs = 4500;

namespace bubble_detail
{
using burst_detail::Unit;
inline float Smooth(float lo, float hi, float v)
{
    const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// Small bubbles rise slowly (~0.2 m/s at 2 mm), bigger ones faster, capped near 0.3 m/s
// for centimetre bubbles that wobble instead of rising faster.
inline float RiseSpeed(float radius) { return std::min(0.12f + 45.0f * radius, 0.32f); }
} // namespace bubble_detail

template <class Emit>
int BubbleTrailSprites(const BubbleTrail& trail, double nowMs, Emit&& emit)
{
    using namespace bubble_detail;
    if (nowMs < double(trail.birthMs)) return 0;
    const float age = float((nowMs - double(trail.birthMs)) * 0.001);
    if (age * 1000.0f >= float(BubbleTrailLifetimeMs)) return 0;
    const MuzzleBurstVec delta{trail.to.x - trail.from.x, trail.to.y - trail.from.y, trail.to.z - trail.from.z};
    const float length = std::sqrt(Dot(delta, delta));
    if (!(length > 1e-4f)) return 0;
    const MuzzleBurstVec dir = delta * (1.0f / length);
    const MuzzleBurstVec helper = std::fabs(dir.y) < 0.9f ? MuzzleBurstVec{0, 1, 0} : MuzzleBurstVec{1, 0, 0};
    const MuzzleBurstVec side = Normalized(Cross(helper, dir));
    const MuzzleBurstVec up = Cross(dir, side);
    const float fast = std::clamp(trail.speed / 800.0f, 0.15f, 1.0f);
    int emitted = 0;

    // The cavity: a white streak along the whole segment while it is open.
    constexpr float cavityLife = 0.14f;
    if (age < cavityLife)
    {
        const float u = age / cavityLife;
        emit(BubbleSprite{BubbleKind::Cavity, trail.to, trail.from, 0.012f + 0.02f * fast,
                          0.95f * (1.0f - u * u)});
        ++emitted;
    }
    // The collapsed cavity: a ragged, fizzing line of microbubbles that thins out.
    const int fizz = std::clamp(int(length * 30.0f) + 3, 3, 40);
    for (int i = 0; i < fizz; ++i)
    {
        const std::uint32_t k = 500u + std::uint32_t(i) * 8u;
        const float born = cavityLife * (0.3f + 0.5f * Unit(trail.seed, k + 5));
        if (age < born) continue;
        const float t = age - born;
        const float life = (0.25f + 0.4f * Unit(trail.seed, k)) * (0.5f + 0.5f * fast);
        if (t >= life) continue;
        const float along = Unit(trail.seed, k + 1);
        const float around = Unit(trail.seed, k + 2) * 6.2831853f;
        const float jitter = 0.015f * fast * Unit(trail.seed, k + 3);
        MuzzleBurstVec p = trail.from + delta * along + (side * std::cos(around) + up * std::sin(around)) * jitter;
        p.y = std::min(p.y + 0.05f * t, trail.surfaceY - 0.02f);
        const float u = t / life;
        emit(BubbleSprite{BubbleKind::Foam, p, p,
                          (0.008f + 0.012f * fast * Unit(trail.seed, k + 4)) * (1.0f + 1.0f * u),
                          0.8f * (1.0f - u)});
        ++emitted;
    }

    // The bubbles it collapses into: denser and bigger where the round was fastest.
    const int count = std::clamp(int(length * 45.0f * fast) + 3, 3, 70);
    const std::uint32_t s = trail.seed;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t k = 1000u + std::uint32_t(i) * 8u;
        const float born = cavityLife * (0.4f + 0.6f * Unit(s, k));
        if (age < born) continue;
        const float life = 0.9f + 1.4f * Unit(s, k + 1);
        const float t = age - born;
        if (t >= life) continue;
        const float along = (float(i) + Unit(s, k + 2)) / float(count);
        const float spread = (0.004f + 0.02f * fast) * Unit(s, k + 3);
        const float around = Unit(s, k + 4) * 6.2831853f;
        const float r0 = (0.0025f + 0.006f * Unit(s, k + 5)) * (0.6f + 0.6f * fast);
        const float radius = r0 * (1.0f + 0.25f * t); // expands slightly as it rises
        const float wobble = 0.004f * std::sin(t * (14.0f + 6.0f * Unit(s, k + 6)) + Unit(s, k + 7) * 6.28f);
        MuzzleBurstVec p = trail.from + delta * along + (side * std::cos(around) + up * std::sin(around)) * spread
            + side * wobble;
        p.y += RiseSpeed(r0) * t;
        if (p.y > trail.surfaceY - radius) continue; // reached the surface: gone
        const float fade = 1.0f - Smooth(0.55f, 1.0f, t / life);
        emit(BubbleSprite{BubbleKind::Bubble, p, p, radius, 0.85f * fade * Smooth(0.0f, 0.04f, t)});
        ++emitted;
    }
    return emitted;
}

template <class Emit>
int BubbleBlastSprites(const BubbleBlast& blast, double nowMs, Emit&& emit)
{
    using namespace bubble_detail;
    if (nowMs < double(blast.birthMs)) return 0;
    const float age = float((nowMs - double(blast.birthMs)) * 0.001);
    if (age * 1000.0f >= float(BubbleBlastLifetimeMs)) return 0;
    const std::uint32_t s = blast.seed;
    const float R = std::clamp(blast.radius, 0.3f, 3.0f);
    int emitted = 0;

    // Flash: the detonation itself, a few hundredths of a second.
    if (age < 0.07f)
    {
        emit(BubbleSprite{BubbleKind::Flash, blast.centre, blast.centre, R * (0.6f + 5.0f * age),
                          1.0f - age / 0.07f});
        ++emitted;
    }
    // The gas bubble swells and collapses (its first oscillation), then a weaker rebound.
    const float period = 0.06f + 0.05f * R;
    if (age < period * 1.7f)
    {
        const float phase = age / period;
        const float size = phase < 1.0f ? std::sin(phase * 3.14159265f) : 0.45f * std::sin((phase - 1.0f) / 0.7f * 3.14159265f);
        if (size > 0.0f)
        {
            MuzzleBurstVec c = blast.centre;
            c.y += 0.6f * R * age; // buoyancy already lifts it
            emit(BubbleSprite{BubbleKind::GasBubble, c, c, R * size, 0.55f});
            ++emitted;
        }
    }
    // The bubble cloud: thrown out with the collapse, braked by the water, then climbing.
    // The churn: a white mass of shattered gas and microbubbles that boils up
    // after the collapse, spreading and thinning as it climbs.
    constexpr int foam = 70;
    for (int i = 0; i < foam; ++i)
    {
        const std::uint32_t k = 4000u + std::uint32_t(i) * 8u;
        const float born = period * (0.8f + 0.6f * Unit(s, k));
        if (age < born) continue;
        const float t = age - born;
        const float life = 1.2f + 1.8f * Unit(s, k + 1);
        if (t >= life) continue;
        const float u = t / life;
        const float z = Unit(s, k + 2) * 2.0f - 1.0f;
        const float around = Unit(s, k + 3) * 6.2831853f;
        const float flat = std::sqrt(std::max(1.0f - z * z, 0.0f));
        const MuzzleBurstVec out{flat * std::cos(around), z * 0.6f, flat * std::sin(around)};
        MuzzleBurstVec p = blast.centre + out * (R * (0.3f + 0.7f * Unit(s, k + 4)) * (1.0f - std::exp(-t * 4.0f)));
        const float climbed = p.y + (0.5f + 0.6f * Unit(s, k + 5)) * t + 0.6f * R * born;
        // At the surface the churn spreads sideways and breaks up instead of stacking.
        const float over = std::max(climbed - (blast.surfaceY - 0.3f), 0.0f);
        p = p + MuzzleBurstVec{out.x, 0.0f, out.z} * (over * 1.5f);
        const float puff = R * (0.07f + 0.13f * Unit(s, k + 6)) * (1.0f + 0.8f * u);
        // A whole puff stays under the surface: a camera-facing quad poking through it
        // is cut along the surface plane into one straight edge across the churn.
        p.y = std::min(climbed, blast.surfaceY - puff - 0.1f * Unit(s, k + 8));
        const float surfaceFade = 1.0f - Smooth(0.0f, 0.8f, over);
        if (surfaceFade <= 0.0f) continue;
        emit(BubbleSprite{BubbleKind::Foam, p, p, puff,
                          (0.25f + 0.2f * Unit(s, k + 7)) * (1.0f - u) * (1.0f - u) * surfaceFade});
        ++emitted;
    }
    constexpr int count = 220;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t k = 2000u + std::uint32_t(i) * 8u;
        const float born = period * (0.7f + 0.8f * Unit(s, k));
        if (age < born) continue;
        const float t = age - born;
        const float life = 1.8f + 2.4f * Unit(s, k + 1);
        if (t >= life) continue;
        const float z = Unit(s, k + 2) * 2.0f - 1.0f;
        const float around = Unit(s, k + 3) * 6.2831853f;
        const float flat = std::sqrt(std::max(1.0f - z * z, 0.0f));
        const MuzzleBurstVec out{flat * std::cos(around), z, flat * std::sin(around)};
        const float reach = R * (0.4f + 0.9f * Unit(s, k + 4));
        const float r0 = 0.006f + 0.045f * Unit(s, k + 5) * Unit(s, k + 5); // mostly small, a few big
        // Out to `reach` with water drag (exponential approach), then rise.
        MuzzleBurstVec p = blast.centre + out * (reach * (1.0f - std::exp(-t * 6.0f)));
        const float rise = (0.35f + 9.0f * r0) * (0.8f + 0.4f * Unit(s, k + 6));
        p.y = std::min(p.y + rise * t + 0.6f * R * born, blast.surfaceY - r0);
        const float atSurface = p.y >= blast.surfaceY - r0 - 1e-3f ? 1.0f : 0.0f;
        const float fade = (1.0f - Smooth(0.6f, 1.0f, t / life)) * (1.0f - 0.8f * atSurface);
        emit(BubbleSprite{BubbleKind::Bubble, p, p, r0 * (1.0f + 0.2f * t), 0.65f * fade});
        ++emitted;
    }
    return emitted;
}

template <class T, int N>
class EventRing
{
public:
    static constexpr int Capacity = N;
    void Record(const T& event)
    {
        _items[_next] = event;
        _next = (_next + 1) % N;
        _count = std::min(_count + 1, N);
    }
    template <class Visit>
    void ForEach(Visit&& visit) const
    {
        for (int i = 0; i < _count; ++i) visit(_items[i]);
    }

private:
    T _items[N]{};
    int _next = 0;
    int _count = 0;
};

bool UnderwaterBubblesEnabled();
void RecordBubbleTrail(const BubbleTrail& trail);
void RecordBubbleBlast(const BubbleBlast& blast);
/// Draw every live trail and blast; does nothing while the camera is above water.
void DrawUnderwaterBubbles();
} // namespace Poseidon::render
