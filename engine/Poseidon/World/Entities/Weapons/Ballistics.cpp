// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp> // G_CONST

#include <algorithm>
#include <cmath>

namespace Poseidon
{
namespace Ballistics
{
namespace
{

/// One integration step of the SAME scheme as ShotShell::Simulate (Shots.cpp:684-703, :913):
/// acceleration from the CURRENT speed, position advanced with the CURRENT speed, speed updated
/// afterwards. Writing it once here is the point of the whole file -- a second, subtly different
/// copy of the flight model is how the vacuum-parabola bug happened in the first place.
struct Integrator
{
    const ShellParams& shell;
    float step;
    Vector3 position;
    Vector3 speed;
    float initDelay;
    float timeToLive;
    float time = 0.0f;

    Integrator(const ShellParams& s, Vector3Par start, Vector3Par velocity, float dt)
        : shell(s), step(dt), position(start), speed(velocity), initDelay(s.initTime),
          timeToLive(std::min(s.timeToLive, MaxFlightTime))
    {
    }

    /// Returns false once the round has expired.
    bool Advance()
    {
        Vector3 accel = speed * (speed.Size() * shell.airFriction);
        accel[1] -= shell.coefGravity * G_CONST;

        // Ours, not the fork's: the delay is decremented and the SAME step still moves if it has
        // gone non-positive. See the header for why that one-step difference matters.
        initDelay -= step;
        if (initDelay <= 0.0f)
        {
            timeToLive -= step;
            if (timeToLive < 0.0f)
            {
                return false;
            }
            position += speed * step;
            speed += accel * step;
        }
        time += step;
        return true;
    }
};

} // namespace

Impact SimulateAtDistance(const ShellParams& shell, Vector3Par start, Vector3Par velocity, float distance, float step)
{
    Impact out;
    const float distance2 = distance * distance;

    Integrator it(shell, start, velocity, step);
    Vector3 previous = it.position;
    float previousTime = 0.0f;

    while (true)
    {
        previous = it.position;
        previousTime = it.time;
        if (!it.Advance())
        {
            out.position = it.position;
            out.time = it.time;
            out.reached = false;
            return out;
        }
        if ((it.position - start).SquareSize() >= distance2)
        {
            break;
        }
    }

    // Land exactly on the distance sphere rather than one step past it. Without this the answer
    // is quantised to the step, and the aim correction below would chase that quantisation
    // instead of converging. Linear interpolation across the step the crossing happened in is
    // ample: the step is 5 ms and the path over it is straight to well under a millimetre.
    const float before = (previous - start).Size();
    const float after = (it.position - start).Size();
    const float span = after - before;
    const float t = span > 1e-6f ? std::clamp((distance - before) / span, 0.0f, 1.0f) : 0.0f;

    out.position = previous + (it.position - previous) * t;
    out.time = previousTime + (it.time - previousTime) * t;
    out.reached = true;
    return out;
}

Approach SimulateAgainstTarget(const ShellParams& shell, Vector3Par start, Vector3Par velocity, Vector3Par targetPos,
                               Vector3Par targetSpeed, float step)
{
    Approach out;
    Integrator it(shell, start, velocity, step);

    float bestError2 = (it.position - targetPos).SquareSize();
    out.position = it.position;
    out.time = 0.0f;

    while (it.Advance())
    {
        // The target moves while the round flies -- that is the whole of "lead".
        const Vector3 targetNow = targetPos + targetSpeed * it.time;
        const float error2 = (it.position - targetNow).SquareSize();
        if (error2 < bestError2)
        {
            bestError2 = error2;
            out.position = it.position;
            out.time = it.time;
        }
        else if (error2 > bestError2 * 4.0f)
        {
            // Past the point of closest approach and receding fast; nothing later can win.
            break;
        }
    }

    out.error = std::sqrt(bestError2);
    return out;
}

Vector3 CompensateAim(const ShellParams& shell, float initSpeed, Vector3Par shooterSpeed, Vector3Par firingPos,
                      Vector3Par targetPos, Vector3Par targetSpeed, int iterations, float step)
{
    Vector3 toTarget = targetPos - firingPos;
    const float distance = toTarget.Size();
    if (distance < 1e-3f || initSpeed <= 0.0f)
    {
        return Vector3(0, 0, 1);
    }

    // Start by aiming straight at the target, then correct by where the round actually went.
    // `aimPoint` is the point we PRETEND to aim at; each pass moves it by the miss vector, which
    // is the standard fixed-point iteration for this problem and is what the fork does.
    Vector3 aimPoint = targetPos;

    for (int i = 0; i < iterations; i++)
    {
        Vector3 dir = aimPoint - firingPos;
        const float len = dir.Size();
        if (len < 1e-3f)
        {
            break;
        }
        dir *= 1.0f / len;

        // The shooter's own motion is added to the muzzle velocity, exactly as a moving vehicle's
        // rounds inherit its speed.
        const Vector3 velocity = dir * initSpeed + shooterSpeed;

        const Approach hit = SimulateAgainstTarget(shell, firingPos, velocity, targetPos, targetSpeed, step);
        const Vector3 targetAtImpact = targetPos + targetSpeed * hit.time;

        // Miss vector, applied to the aim point. If the round landed short and low, we aim long
        // and high by the same amount.
        aimPoint += targetAtImpact - hit.position;
    }

    Vector3 dir = aimPoint - firingPos;
    const float len = dir.Size();
    if (len < 1e-6f)
    {
        return Vector3(0, 0, 1);
    }
    return dir * (1.0f / len);
}

Vector3 CompensateAimVacuum(float initSpeed, Vector3Par firingPos, Vector3Par targetPos)
{
    Vector3 dir = targetPos - firingPos;
    const float distance = dir.Size();
    if (distance < 1e-3f || initSpeed <= 0.0f)
    {
        return Vector3(0, 0, 1);
    }
    dir *= 1.0f / distance;

    // SoldierOldAI.cpp:446-448, verbatim in effect: t = d / v0, fall = t * (1/v0) * 0.5 * g,
    // added to the vertical component of the unit direction.
    const float invInitSpeed = 1.0f / initSpeed;
    const float time = distance * invInitSpeed;
    const float fallPerM = time * invInitSpeed * 0.5f * G_CONST;
    dir[1] += fallPerM;

    const float len = dir.Size();
    return len > 1e-6f ? dir * (1.0f / len) : Vector3(0, 0, 1);
}

} // namespace Ballistics
} // namespace Poseidon
