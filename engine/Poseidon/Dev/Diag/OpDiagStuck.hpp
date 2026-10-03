#pragma once

// DIAG-001: the stuck / spin detector of the diagnostics layer, as pure logic so the unit tests can
// drive it with made-up positions. OpDiagPollers.cpp feeds it one sample per AI unit per second.
//
// Ported from Malprave (Dec's GPL fork of the same Poseidon source), MwDiag2.cpp PollUnits:
//   stuck  the unit has somewhere to go (wantsToMove) but has not moved 0.3 m for stuckSec seconds
//   spin   it turned more than 540 degrees within 10 s while moving less than 1 m
// Moving more than 1 m starts a new stretch; creeping (0.3 .. 1 m) for more than 2 x stuckSec also
// starts a new one, so a slow walker is not flagged. Each event is reported once per stretch.

#include <cmath>

namespace Poseidon::Dev::OpDiag
{

struct StuckSample
{
    float x = 0, y = 0, z = 0; //!< position (engine X, Y up, Z)
    float dirX = 0, dirZ = 1;  //!< facing on the ground plane
    bool wantsToMove = false;  //!< a real destination and a path or a planning state
};

struct StuckTrack
{
    bool started = false;
    float ax = 0, ay = 0, az = 0; //!< where the current "not moving" stretch began
    double anchorT = 0;           //!< when it began (seconds)
    float turned = 0;             //!< heading change accumulated since (radians)
    float lastDirX = 0, lastDirZ = 1;
    bool reportedStuck = false;
    bool reportedSpin = false;
};

enum StuckEvents : unsigned
{
    StuckNone = 0,
    StuckEventSpin = 1,
    StuckEventStuck = 2,
};

//! one sample; returns the events to report now (StuckEvents bits); secOut = length of the stretch
inline unsigned StuckUpdate(StuckTrack& t, const StuckSample& s, double now, float stuckSec, double* secOut = nullptr)
{
    constexpr float kPi = 3.14159265f;
    if (!t.started)
    {
        t = StuckTrack{};
        t.started = true;
        t.ax = s.x, t.ay = s.y, t.az = s.z;
        t.anchorT = now;
        t.lastDirX = s.dirX, t.lastDirZ = s.dirZ;
        if (secOut)
            *secOut = 0;
        return StuckNone;
    }
    // heading change since the last sample, on the ground plane, wrapped to [-pi, pi]
    float da = std::atan2(s.dirX, s.dirZ) - std::atan2(t.lastDirX, t.lastDirZ);
    while (da > kPi)
        da -= 2 * kPi;
    while (da < -kPi)
        da += 2 * kPi;
    t.turned += std::fabs(da);
    t.lastDirX = s.dirX, t.lastDirZ = s.dirZ;

    const float dx = s.x - t.ax, dy = s.y - t.ay, dz = s.z - t.az;
    const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (moved > 1.0f)
    {
        t.ax = s.x, t.ay = s.y, t.az = s.z;
        t.anchorT = now;
        t.turned = 0;
        t.reportedStuck = t.reportedSpin = false;
        if (secOut)
            *secOut = 0;
        return StuckNone;
    }
    const double sec = now - t.anchorT;
    if (secOut)
        *secOut = sec;
    unsigned ev = StuckNone;
    if (!t.reportedSpin && t.turned > 3 * kPi && sec < 10)
    {
        t.reportedSpin = true;
        ev |= StuckEventSpin;
    }
    if (!t.reportedStuck && s.wantsToMove && moved < 0.3f && sec >= stuckSec)
    {
        t.reportedStuck = true;
        ev |= StuckEventStuck;
    }
    if (moved >= 0.3f && sec > stuckSec * 2)
    {
        t.ax = s.x, t.ay = s.y, t.az = s.z;
        t.anchorT = now;
        t.turned = 0;
    }
    return ev;
}

} // namespace Poseidon::Dev::OpDiag
