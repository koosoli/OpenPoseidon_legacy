#pragma once

#include <algorithm>
#include <cmath>

// Mud thrown off tyres: small solid clods and wet spatter that fly ballistically,
// land, sit on the ground for a few seconds and sink back into it. Pure maths, no
// engine state, so every rule is unit-tested (test_mud_clods.cpp); the entity
// that draws a clod lives in Entities/Vehicles/MudClods.
//
// Why not cloudlets: cloudlets are soft camera-facing smoke sprites that float
// and fade, so mud drawn with them reads as brown smoke. A clod is a small lit
// lump of soil that falls under full gravity and stays where it lands.
namespace Poseidon
{
constexpr float MudClodGravity = 9.81f;

struct MudClodState
{
    float px = 0, py = 0, pz = 0;     // centre, world metres
    float vx = 0, vy = 0, vz = 0;     // m/s
    float radius = 0.03f;             // metres
    float age = 0;                    // seconds since launch
    float life = 6;                   // total seconds before it is gone
    float landedAt = -1;              // age when it hit the ground, -1 in flight
    float spin = 0, spinRate = 0;     // tumble angle (rad) and rate (rad/s)
    bool Landed() const { return landedAt >= 0.0f; }
};

// One simulation step. groundY: terrain height under the clod (mud ruts
// included). Small clods feel a little air drag, big ones barely any. On
// contact the clod stops dead -- wet mud sticks, it does not bounce -- and sits
// pressed half into the ground.
inline void StepMudClod(MudClodState& s, float seconds, float groundY)
{
    if (!std::isfinite(seconds) || seconds <= 0.0f) return;
    s.age += seconds;
    if (s.Landed()) return;
    const float drag = 0.012f / std::max(s.radius, 0.005f);          // 1/s: 0.4 at 3 cm, 1.2 at 1 cm
    const float keep = std::exp(-drag * seconds);
    s.vx *= keep; s.vz *= keep; s.vy *= keep;
    s.vy -= MudClodGravity * seconds;
    s.px += s.vx * seconds; s.py += s.vy * seconds; s.pz += s.vz * seconds;
    s.spin += s.spinRate * seconds;
    if (std::isfinite(groundY) && s.py - s.radius * 0.5f <= groundY && s.vy <= 0.0f)
    {
        s.py = groundY + s.radius * 0.3f;
        s.vx = s.vy = s.vz = 0.0f;
        s.landedAt = s.age;
    }
}

inline bool MudClodExpired(const MudClodState& s)
{
    // A clod that fell off the world (no ground found) also goes after its life.
    return !std::isfinite(s.py) || s.age >= s.life;
}

// Drawn size 0..1 of the clod's radius: full in flight and on landing, then it
// sinks into the mud over the last 40% of its life.
inline float MudClodVisibleScale(const MudClodState& s)
{
    if (!s.Landed()) return 1.0f;
    const float sinkStart = s.life * 0.6f;
    if (s.age <= sinkStart) return 1.0f;
    return std::clamp(1.0f - (s.age - sinkStart) / std::max(s.life - sinkStart, 0.01f), 0.0f, 1.0f);
}

// Launch of one clod off a tyre.
//   forward: unit horizontal direction the vehicle faces (x,z)
//   driveSign: +1 driving forwards, -1 reversing (which way the tread turns)
//   vehicle velocity (vx,vy,vz); groundSpeed = |forward speed|
//   spin: wheelspin 0..1 (MudTraction); r[0..4]: uniform randoms in [0,1)
// The tread's surface moves at ground speed plus the slip; mud leaves the top
// rear of the tyre along that surface, so relative to the truck it flies
// backwards and up. Rolling without slip that cancels the forward speed: clods
// pop up and drop close to where they left. Spinning wheels throw them far back.
struct MudClodLaunch
{
    float vx = 0, vy = 0, vz = 0;
    float radius = 0.03f;
    float life = 6;
    float spinRate = 0;
};

inline MudClodLaunch PlanMudClodLaunch(float forwardX, float forwardZ, float driveSign,
                                      float vehicleVX, float vehicleVY, float vehicleVZ,
                                      float groundSpeed, float spin, const float r[5])
{
    MudClodLaunch out;
    float len = std::hypot(forwardX, forwardZ);
    if (!std::isfinite(len) || len < 1e-4f) { forwardX = 0; forwardZ = 1; len = 1; }
    forwardX /= len; forwardZ /= len;
    const float sideX = forwardZ, sideZ = -forwardX;
    driveSign = driveSign < 0.0f ? -1.0f : 1.0f;
    spin = std::isfinite(spin) ? std::clamp(spin, 0.0f, 1.0f) : 0.0f;
    groundSpeed = std::isfinite(groundSpeed) ? std::clamp(std::abs(groundSpeed), 0.0f, 25.0f) : 0.0f;
    if (!std::isfinite(vehicleVX) || !std::isfinite(vehicleVY) || !std::isfinite(vehicleVZ))
        vehicleVX = vehicleVY = vehicleVZ = 0.0f;

    // Tread surface speed relative to the truck, and how much of it a clod keeps.
    const float tread = groundSpeed + 9.0f * spin;
    const float throwSpeed = tread * (0.55f + 0.45f * r[0]) + 0.6f;
    // Release angle above horizontal: 20..70 degrees.
    const float elevation = 0.35f + 0.87f * r[1];
    const float back = -driveSign * throwSpeed * std::cos(elevation);
    const float up = throwSpeed * std::sin(elevation);
    const float side = (r[2] - 0.5f) * 0.5f * throwSpeed;
    out.vx = vehicleVX + forwardX * back + sideX * side;
    out.vz = vehicleVZ + forwardZ * back + sideZ * side;
    out.vy = std::max(vehicleVY, 0.0f) + up;
    // Mostly small lumps, now and then a fist-sized one; spatter is tiny.
    out.radius = 0.016f + 0.05f * r[3] * r[3] * r[3] + 0.02f * r[3];
    out.life = 2.5f + 3.0f * r[4];
    out.spinRate = (r[2] - 0.5f) * 30.0f;
    return out;
}

// Clods thrown per second by one wheel: wheelspin throws most, plain rolling
// through soft mud flicks a few up. `soft` is MudSoftness of the ground.
inline float MudClodRate(float spin, float groundSpeed, float soft)
{
    if (!std::isfinite(spin) || !std::isfinite(groundSpeed) || !std::isfinite(soft)) return 0.0f;
    soft = std::clamp(soft, 0.0f, 1.0f);
    if (soft < 0.05f) return 0.0f;
    // Tuned in game: the first pass (6 and 30 per second, split over the axles)
    // was barely visible. A truck tyre in wet mud throws a constant shower.
    const float rolling = std::sqrt(std::clamp(std::abs(groundSpeed) / 8.0f, 0.0f, 1.0f)) * 20.0f;
    const float spinning = std::clamp(spin, 0.0f, 1.0f) * 90.0f;
    return (rolling + spinning) * std::sqrt(soft);
}

// Hard cap on live clods in the world (each is a small drawn entity).
constexpr int MudClodMaxAlive = 600;
} // namespace Poseidon
