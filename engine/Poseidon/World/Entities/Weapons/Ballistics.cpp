// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp> // G_CONST
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>  // AmmoType
#include <Poseidon/IO/ParamFile/ParamFile.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

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

ShellParams ShellParamsFor(const AmmoType& type)
{
    // Keyed on the type's address: AmmoTypes live for the mission and this is asked per aim
    // update per unit, so the ParamEntry lookups underneath must not be paid every time.
    static std::unordered_map<const AmmoType*, ShellParams> cache;
    static std::mutex cacheMutex;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto found = cache.find(&type);
        if (found != cache.end())
        {
            return found->second;
        }
    }

    ShellParams out;
    // These four lines ARE ShotShell's constructor (Shots.cpp:632-655). If that ever stops
    // agreeing with this, the predictor is aiming for a projectile the engine does not fire --
    // which is the exact defect this file exists to remove. ShotShell now reads from here.
    out.initTime = type.initTime;
    out.airFriction = DefaultAirFriction;
    out.coefGravity = 1.0f;
    out.timeToLive = 20.0f;

    const ParamEntry& pars = type.GetParamEntry();
    if (const ParamEntry* entry = pars.FindEntry("coefGravity"))
    {
        out.coefGravity = *entry;
    }
    if (const ParamEntry* entry = pars.FindEntry("timeToLive"))
    {
        out.timeToLive = *entry;
    }
    // BAL-010's first core item: "verified drag / ballistic coefficient model". Until now every
    // bullet and shell in the game shared one hardcoded coefficient, so a 5.56 and a .50
    // decelerated identically and no amount of solver accuracy could make the result realistic.
    //
    // A property that is ABSENT keeps the old constant, which is deliberate and is what makes this
    // safe: classic CWA data does not define `airFriction` anywhere (measured -- the full 755 KB
    // debinarised BIN\CONFIG.BIN has zero occurrences), so shipped missions are byte-identical
    // until someone supplies values. `docs/ballistics/` carries a derived set and the reasoning.
    if (const ParamEntry* entry = pars.FindEntry("airFriction"))
    {
        const float authored = *entry;
        // Sign discipline. The model is `accel = speed * |speed| * airFriction`, so the
        // coefficient must be NEGATIVE to decelerate. BI-era data writes it negative, but a
        // hand-authored positive value would accelerate the round without bound and read as a
        // physics explosion rather than a typo. Take the magnitude and apply the correct sign.
        out.airFriction = authored > 0.0f ? -authored : authored;
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[&type] = out;
    }
    return out;
}

bool DragCorrectAimEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("POSEIDON_BALLISTIC_AIM");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return enabled;
}

LevelShot SolveLevelShot(const ShellParams& shell, float initSpeed, float distance, float step)
{
    LevelShot out;
    if (initSpeed <= 0.0f || distance <= 0.0f)
    {
        return out;
    }

    // Fire dead level and see where the round is once it has covered `distance`. The drop is how
    // far below the line of departure it ended up, which is precisely the amount the barrel has
    // to be raised -- expressed as a distance here, as a slope by the caller that needs one.
    const Vector3 start(0, 0, 0);
    const Vector3 velocity(0, 0, initSpeed);

    const Impact hit = SimulateAtDistance(shell, start, velocity, distance, step);
    if (!hit.reached)
    {
        // Cannot reach: leave `valid` false so the caller keeps whatever it would have done.
        return out;
    }

    out.drop = -hit.position[1];
    out.time = hit.time;

    // A LEVEL-shot solve only describes a FLAT trajectory. Raise the barrel far enough and the
    // path is no longer approximately the level path shifted up -- the round spends longer in the
    // air, drags for longer, and the answer diverges. That is a lobbed weapon: a 30 m/s grenade
    // launcher reaching 200 m is not a rifle with more elevation, it is a different problem, and
    // the call sites that handle those already carry their own hand-tuned approximations.
    //
    // The cut-off is stated as geometry, not tuned: a drop of a quarter of the range is an
    // elevation of about 14 degrees. Below that the linearisation holds to centimetres; above it
    // the caller keeps whatever it was doing. This is what stops the solver from confidently
    // returning nonsense for mortars and grenade launchers.
    out.valid = out.drop >= 0.0f && out.drop <= 0.25f * distance;
    return out;
}

bool SolveAimForAmmo(const AmmoType* ammo, float initSpeed, float distance, float& time, float& fall)
{
    // One gate for every wired call site, so "which shots does this govern" has a single answer
    // rather than six copies that drift.
    //
    // Bullets and shells only: those are unpowered ballistic projectiles, which is precisely what
    // the integrator models. Missiles carry thrust, a launcher-relative up vector and, in this
    // engine, a full rigid-body flight model (Missile::Simulate) -- they get their own solver,
    // not this one.
    if (!DragCorrectAimEnabled() || !ammo || initSpeed <= 0.0f || distance <= 0.0f)
    {
        return false;
    }
    if (ammo->_simulation != AmmoShotBullet && ammo->_simulation != AmmoShotShell)
    {
        return false;
    }

    const LevelShot shot = SolveLevelShot(ShellParamsFor(*ammo), initSpeed, distance);
    if (!shot.valid)
    {
        return false;
    }
    time = shot.time;
    fall = shot.drop;
    return true;
}

MissileParams MissileParamsFor(const AmmoType& type)
{
    MissileParams out;
    out.initTime = type.initTime;
    out.thrustTime = type.thrustTime;
    out.thrust = type.thrust;
    out.sideAirFriction = type.sideAirFriction;
    out.timeToLive = 20.0f;
    if (const ParamEntry* entry = type.GetParamEntry().FindEntry("timeToLive"))
    {
        out.timeToLive = *entry;
    }
    return out;
}

namespace
{

/// One step of the rocket flight, held to `Missile::Simulate` (Shots.cpp:1024-1060) term for term.
///
/// The body-drag polynomial is the engine's, copied deliberately rather than "cleaned up": the
/// per-axis constants (5e-4 / x10 / x10 sideways, 1e-5 / x0.01 / x2 forward) are the whole reason
/// a rocket flies straight, and rounding them would change flight, not just prediction. The
/// engine divides by ten after scaling by sideAirFriction and multiplies by mass because it feeds
/// a force into a rigid body; here the same quantity is already an acceleration, so mass cancels
/// and does not appear.
struct MissileIntegrator
{
    const MissileParams& missile;
    const Matrix3 orient;
    float step;
    Vector3 position;
    Vector3 modelSpeed;
    float initTime;
    float thrustTime;
    float timeToLive;
    float time = 0.0f;

    MissileIntegrator(const MissileParams& m, Vector3Par startPos, const Matrix3& o, Vector3Par worldSpeed, float dt)
        : missile(m), orient(o), step(dt), position(startPos), modelSpeed(ToModel(o, worldSpeed)),
          initTime(m.initTime), thrustTime(m.thrustTime), timeToLive(std::min(m.timeToLive, MaxMissileFlightTime))
    {
    }

    // Matrix3 carries its basis as three named vectors and exposes no Rotate (only Matrix4 does),
    // so the two transforms are written out. Model space is the engine's: +X aside, +Y up,
    // +Z forward -- which is why thrust is applied to [2] and gravity has to be resolved onto all
    // three.
    static Vector3 ToModel(const Matrix3& o, Vector3Par world)
    {
        return Vector3(world.DotProduct(o.DirectionAside()), world.DotProduct(o.DirectionUp()),
                       world.DotProduct(o.Direction()));
    }
    static Vector3 ToWorld(const Matrix3& o, Vector3Par model)
    {
        return o.DirectionAside() * model[0] + o.DirectionUp() * model[1] + o.Direction() * model[2];
    }

    Vector3 WorldSpeed() const { return ToWorld(orient, modelSpeed); }

    bool Advance()
    {
        Vector3 friction;
        friction[0] = modelSpeed[0] * modelSpeed[0] * modelSpeed[0] * 5e-4f +
                      modelSpeed[0] * std::fabs(modelSpeed[0]) * 10.0f + modelSpeed[0] * 10.0f;
        friction[1] = modelSpeed[1] * modelSpeed[1] * modelSpeed[1] * 5e-4f +
                      modelSpeed[1] * std::fabs(modelSpeed[1]) * 10.0f + modelSpeed[1] * 10.0f;
        friction[2] = modelSpeed[2] * modelSpeed[2] * modelSpeed[2] * 1e-5f +
                      modelSpeed[2] * std::fabs(modelSpeed[2]) * 0.01f + modelSpeed[2] * 2.0f;
        friction *= missile.sideAirFriction * 0.1f;

        Vector3 accel(0, 0, 0);
        if (initTime > 0.0f)
        {
            initTime -= step;
        }
        else if (thrustTime > 0.0f)
        {
            thrustTime -= step;
            // The engine's own "FIX too strong missile engines": the motor fades over the last
            // quarter of its burn instead of cutting out. Without it the predicted burnout speed
            // is higher than the real one and every long shot lands over.
            float fade = 1.0f;
            if (missile.thrustTime > 0.0f && 4.0f * thrustTime < missile.thrustTime)
            {
                fade = 4.0f * thrustTime / missile.thrustTime;
            }
            accel[2] += missile.thrust * fade;
        }

        // Gravity is a WORLD force felt in the body frame, which is the whole reason the launch
        // roll matters: it lands on whichever body axes the attitude points at, and those axes
        // have very different drag.
        accel += ToModel(orient, Vector3(0.0f, -G_CONST, 0.0f));

        position += WorldSpeed() * step;
        Friction(modelSpeed, friction, accel, step);

        timeToLive -= step;
        time += step;
        return timeToLive >= 0.0f;
    }
};

} // namespace

Impact SimulateUnguidedMissileAtDistance(const MissileParams& missile, Vector3Par startPos, const Matrix3& orientation,
                                         Vector3Par worldSpeed, float distance, float step)
{
    Impact out;
    const float distance2 = distance * distance;

    MissileIntegrator it(missile, startPos, orientation, worldSpeed, step);
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
        if ((it.position - startPos).SquareSize() >= distance2)
        {
            break;
        }
    }

    const float before = (previous - startPos).Size();
    const float after = (it.position - startPos).Size();
    const float span = after - before;
    const float t = span > 1e-6f ? std::clamp((distance - before) / span, 0.0f, 1.0f) : 0.0f;

    out.position = previous + (it.position - previous) * t;
    out.time = previousTime + (it.time - previousTime) * t;
    out.reached = true;
    return out;
}

Approach SimulateUnguidedMissileAgainstTarget(const MissileParams& missile, Vector3Par startPos,
                                              const Matrix3& orientation, Vector3Par worldSpeed, Vector3Par targetPos,
                                              Vector3Par targetSpeed, float step)
{
    Approach out;
    MissileIntegrator it(missile, startPos, orientation, worldSpeed, step);

    float bestError2 = (it.position - targetPos).SquareSize();
    out.position = it.position;

    while (it.Advance())
    {
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
            break;
        }
    }

    out.error = std::sqrt(bestError2);
    return out;
}

Vector3 CompensateUnguidedMissile(const MissileParams& missile, float initSpeed, Vector3Par worldUp,
                                  Vector3Par shooterSpeed, Vector3Par firingPos, Vector3Par targetPos,
                                  Vector3Par targetSpeed, int iterations, float step)
{
    const Vector3 toTarget = targetPos - firingPos;
    if (toTarget.SquareSize() < 1e-6f)
    {
        return Vector3(0, 0, 1);
    }

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

        // The launcher's attitude IS the candidate direction -- an unguided rocket leaves pointing
        // where it was aimed. The roll about that axis comes from worldUp, and it matters because
        // the drag tensor is anisotropic in the body frame.
        const Matrix3 orientation(MDirection, dir, worldUp);
        const Vector3 velocity = dir * initSpeed + shooterSpeed;

        const Approach hit =
            SimulateUnguidedMissileAgainstTarget(missile, firingPos, orientation, velocity, targetPos, targetSpeed, step);
        const Vector3 targetAtImpact = targetPos + targetSpeed * hit.time;
        aimPoint += targetAtImpact - hit.position;
    }

    Vector3 dir = aimPoint - firingPos;
    const float len = dir.Size();
    return len > 1e-6f ? dir * (1.0f / len) : Vector3(0, 0, 1);
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
