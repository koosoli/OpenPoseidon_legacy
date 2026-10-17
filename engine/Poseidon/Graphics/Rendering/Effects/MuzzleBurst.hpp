#pragma once

// Muzzle burst: the short-lived particles a rifle shot throws out, on top of the
// zasleh flash sheet (MuzzleFlashAppearance.hpp) and the flash light (Lights.cpp).
//
//   * port jets   -- gas blasting sideways out of the flash hider's slots
//   * plume       -- the forward cone of burning propellant gas
//   * sparks      -- unburnt powder grains, thrown forward, falling, glowing
//
// Every particle is a pure function of (shot, age): nothing is simulated or
// stored per particle, so a shot costs one ring-buffer slot however many
// particles it shows, and a paused game freezes the burst instead of drifting.
// Particles are drawn as additive, depth-tested, motion-stretched world sprites
// through Engine::DrawDecalRotated (the path rain streaks use).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Poseidon::render
{
struct MuzzleBurstVec
{
    float x = 0, y = 0, z = 0;
};
inline MuzzleBurstVec operator+(MuzzleBurstVec a, MuzzleBurstVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline MuzzleBurstVec operator*(MuzzleBurstVec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(MuzzleBurstVec a, MuzzleBurstVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline MuzzleBurstVec Cross(MuzzleBurstVec a, MuzzleBurstVec b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline MuzzleBurstVec Normalized(MuzzleBurstVec a)
{
    const float len = std::sqrt(Dot(a, a));
    return len > 1e-6f ? a * (1.0f / len) : MuzzleBurstVec{0, 0, 1};
}

struct MuzzleBurstShot
{
    MuzzleBurstVec muzzle;  // world position of the muzzle at the shot
    MuzzleBurstVec forward; // world barrel direction (normalised on record)
    MuzzleBurstVec carrier; // shooter velocity, m/s: the gas leaves with it
    std::int64_t birthMs = 0;
    std::uint32_t seed = 0;
};

enum class MuzzleParticleKind { Jet, Plume, Spark };

// One particle at one instant. `tail` is where it was a motion-blur interval ago,
// so the sprite is stretched along its true velocity (a fast spark is a streak).
struct MuzzleParticle
{
    MuzzleParticleKind kind;
    MuzzleBurstVec head, tail;
    float radius;   // metres, across the motion
    float radiance; // scene-linear HDR multiplier
    float r, g, b;  // emission colour, normalised so max == 1
};

namespace burst_detail
{
inline float Unit(std::uint32_t seed, std::uint32_t k)
{
    std::uint32_t h = seed * 747796405u + k * 2891336453u + 0x9E3779B9u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return float(h & 0xFFFFFFu) / float(0xFFFFFF);
}
// Distance travelled under linear drag k from speed v0 after t seconds.
inline float Dragged(float v0, float k, float t) { return v0 / k * (1.0f - std::exp(-k * t)); }
// Blackbody-ish cooling: white-yellow when fresh, through orange to dull red.
inline void Cooling(float heat, float& r, float& g, float& b)
{
    heat = std::clamp(heat, 0.0f, 1.0f);
    r = 1.0f;
    g = 0.30f + 0.62f * heat;
    b = 0.05f + 0.60f * heat * heat;
}
} // namespace burst_detail

// Longest-lived particle (a spark) in milliseconds: a shot older than this draws nothing.
constexpr int MuzzleBurstLifetimeMs = 260;
// Upper bound on particles one shot can emit at any instant.
constexpr int MuzzleBurstMaxParticles = 5 * 2 + 7 + 6;

// Emits the particles of `shot` alive at `nowMs` through `emit(const MuzzleParticle&)`.
// Returns how many were emitted.
template <class Emit>
int MuzzleBurstParticles(const MuzzleBurstShot& shot, double nowMs, float gravity, Emit&& emit)
{
    using namespace burst_detail;
    if (nowMs < double(shot.birthMs)) return 0;
    const float age = float((nowMs - double(shot.birthMs)) * 0.001);
    if (age * 1000.0f >= float(MuzzleBurstLifetimeMs)) return 0;
    const MuzzleBurstVec fwd = Normalized(shot.forward);
    // Any vector not parallel to the barrel builds the port ring's frame.
    const MuzzleBurstVec helper = std::fabs(fwd.y) < 0.9f ? MuzzleBurstVec{0, 1, 0} : MuzzleBurstVec{1, 0, 0};
    const MuzzleBurstVec side = Normalized(Cross(helper, fwd));
    const MuzzleBurstVec up = Cross(fwd, side);
    const std::uint32_t s = shot.seed;
    constexpr float blur = 1.0f / 120.0f; // motion-blur interval, seconds
    int emitted = 0;
    const auto at = [&](MuzzleBurstVec dir, float v0, float k, float t) {
        return shot.muzzle + shot.carrier * t + dir * Dragged(v0, k, t);
    };

    // Port jets: 4 or 5 slots at a random roll, two puffs each. Gas leaves the
    // slots fast and stops within a hand's width; the puff swells as it cools.
    const int ports = 4 + int(Unit(s, 1) * 2.0f);
    const float roll = Unit(s, 2) * 6.2831853f;
    for (int p = 0; p < ports; ++p)
    {
        const float angle = roll + float(p) * 6.2831853f / float(ports) + (Unit(s, 10 + p) - 0.5f) * 0.4f;
        const MuzzleBurstVec radial = side * std::cos(angle) + up * std::sin(angle);
        for (int puff = 0; puff < 2; ++puff)
        {
            const std::uint32_t k = 100 + std::uint32_t(p) * 8 + std::uint32_t(puff) * 4;
            const float life = 0.012f + 0.010f * Unit(s, k); // gone within a frame or two, but always on the first frame at 60 fps
            if (age >= life) continue;
            const float u = age / life;
            const MuzzleBurstVec dir = Normalized(radial + fwd * (0.25f + 0.35f * Unit(s, k + 1)));
            const float v0 = 7.0f + 7.0f * Unit(s, k + 2) * (puff ? 0.6f : 1.0f);
            constexpr float drag = 55.0f;
            MuzzleParticle q{MuzzleParticleKind::Jet, at(dir, v0, drag, age),
                             at(dir, v0, drag, std::max(age - blur, 0.0f)),
                             0.022f + 0.032f * u * (0.7f + 0.6f * Unit(s, k + 3)),
                             12.0f * (1.0f - u) * (1.0f - u) * (1.0f - u), 0, 0, 0};
            Cooling(1.0f - 1.6f * u, q.r, q.g, q.b); // white only at birth, orange-red within half its life
            emit(q);
            ++emitted;
        }
    }

    // Forward plume: a few puffs riding the barrel axis, spreading slightly.
    const int puffs = 4 + int(Unit(s, 3) * 4.0f);
    for (int i = 0; i < puffs; ++i)
    {
        const std::uint32_t k = 200 + std::uint32_t(i) * 8;
        const float life = 0.014f + 0.014f * Unit(s, k);
        if (age >= life) continue;
        const float u = age / life;
        const MuzzleBurstVec dir = Normalized(fwd + side * ((Unit(s, k + 1) - 0.5f) * 0.25f) +
                                              up * ((Unit(s, k + 2) - 0.5f) * 0.25f));
        const float v0 = 10.0f + 22.0f * Unit(s, k + 3);
        constexpr float drag = 45.0f;
        MuzzleParticle q{MuzzleParticleKind::Plume, at(dir, v0, drag, age),
                         at(dir, v0, drag, std::max(age - blur, 0.0f)),
                         0.030f + 0.040f * u * (0.6f + 0.8f * Unit(s, k + 4)),
                         10.0f * (1.0f - u) * (1.0f - u) * (1.0f - u) * (0.6f + 0.4f * Unit(s, k + 5)), 0, 0, 0};
        Cooling(1.0f - 1.5f * u, q.r, q.g, q.b);
        emit(q);
        ++emitted;
    }

    // Sparks: not every shot throws visible grains. Fast, nearly drag-free,
    // falling, cooling from yellow to dim orange over a few tenths of a second.
    const int sparks = Unit(s, 4) < 0.35f ? 0 : 1 + int(Unit(s, 5) * 5.0f);
    for (int i = 0; i < sparks; ++i)
    {
        const std::uint32_t k = 300 + std::uint32_t(i) * 8;
        const float life = 0.08f + 0.17f * Unit(s, k);
        if (age >= life) continue;
        const float u = age / life;
        const float cone = 0.35f * Unit(s, k + 1);
        const float around = Unit(s, k + 2) * 6.2831853f;
        const MuzzleBurstVec dir = Normalized(fwd + (side * std::cos(around) + up * std::sin(around)) * cone);
        const float v0 = 25.0f + 55.0f * Unit(s, k + 3);
        constexpr float drag = 3.0f;
        const auto spark = [&](float t) {
            MuzzleBurstVec p = at(dir, v0, drag, t);
            p.y -= 0.5f * gravity * t * t;
            return p;
        };
        MuzzleParticle q{MuzzleParticleKind::Spark, spark(age), spark(std::max(age - blur, 0.0f)),
                         0.004f + 0.003f * Unit(s, k + 4), 6.0f * (1.0f - u * u), 0, 0, 0};
        Cooling(0.75f - 0.75f * u, q.r, q.g, q.b);
        emit(q);
        ++emitted;
    }
    return emitted;
}

// The ring of recent shots. Recording is O(1); a full ring drops the oldest shot,
// which at 64 slots and a 260 ms life only happens in a very dense firefight.
class MuzzleBurstRing
{
public:
    static constexpr int Capacity = 64;
    void Record(const MuzzleBurstShot& shot)
    {
        _shots[_next] = shot;
        _shots[_next].forward = Normalized(shot.forward);
        _next = (_next + 1) % Capacity;
        _count = std::min(_count + 1, Capacity);
    }
    template <class Visit>
    void ForEachLive(double nowMs, Visit&& visit) const
    {
        for (int i = 0; i < _count; ++i)
        {
            const MuzzleBurstShot& shot = _shots[i];
            const double age = nowMs - double(shot.birthMs);
            if (age >= 0 && age < MuzzleBurstLifetimeMs) visit(shot);
        }
    }
    void Clear() { _count = 0; _next = 0; }

private:
    MuzzleBurstShot _shots[Capacity]{};
    int _next = 0;
    int _count = 0;
};

bool MuzzleBurstEnabled();
// Record a rifle shot (called from the shooter's fire effects).
void RecordMuzzleBurst(const MuzzleBurstShot& shot);
// Draw every live burst into the current scene (called once per frame from Scene).
void DrawMuzzleBursts();
} // namespace Poseidon::render
