// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

// Drag-correct ballistic prediction.
//
// WHY THIS EXISTS. Our projectiles fly with air drag -- `ShotShell::Simulate`
// (World/Entities/Weapons/Shots.cpp:684) integrates
//
//     accel  = speed * (|speed| * _airFriction)
//     accel.y -= _coefGravity * G_CONST
//
// but every AI aiming site predicts where that projectile will go with a DRAG-FREE closed form.
// The canonical instance is Man::GetAimTargetPosition (World/Entities/Infantry/SoldierOldAI.cpp:446):
//
//     float time     = distance * _invInitSpeed;
//     float fallPerM = time * _invInitSpeed * 0.5f * G_CONST;
//
// -- a vacuum parabola. The same two lines are repeated at SoldierOldAI.cpp:357 and :1259,
// AI/VehicleAICombat.cpp:1417, Air/AirplaneAI.cpp:282, Air/HelicopterAI.cpp:303, Ground/Car.cpp:2168,
// Ground/Motorcycle.cpp:2184, Ground/TankWithAI.cpp:260 and Misc/Ship.cpp:1746. So the AI aims with
// a model that is not the model the bullet flies under, and the disagreement grows with range and
// with airFriction. That is a correctness gap, not a difficulty setting.
//
// This is the fix the FP-GL fork made (`Poseidon2/shots.cpp:776-1450`), ported here. The port is
// worth making because the fork's integrator is not an approximation of our flight model -- it is
// the SAME forward-Euler scheme, step for step: acceleration from the old speed, position advanced
// with the old speed, speed updated afterwards, launch delay gating both. Reproducing our own
// simulation is exactly what a predictor has to do.
//
// TWO DELIBERATE DIFFERENCES FROM THE FORK, both to match THIS engine rather than that one:
//
//  * The fork spends a whole step decrementing `initTime` without moving. Ours decrements
//    `_initDelay` and, in the same frame, moves if it has gone non-positive (Shots.cpp:686-703).
//    Matching the fork there would put the prediction one step of flight behind the projectile.
//  * Our drag reads `ShotAirspeed(_speed)`, which folds in wind when the ballistics-wind opt-in is
//    on. The predictor deliberately does NOT model wind: an aiming solution cannot know the wind
//    the round will meet along a path it has not flown, and pretending otherwise would make the AI
//    hit better than a human can. With the opt-in off -- the shipping default -- `ShotAirspeed` is
//    the identity and prediction and flight agree exactly.
//
// The missile/rocket half of the fork's set (`SimulateUnguidedMissile*`, `CompensateUnguidedMissile*`,
// which additionally carry the launcher's up-vector through the thrust phase) is NOT ported yet.
namespace Poseidon
{
namespace Ballistics
{

/// The projectile properties the flight model actually reads, named as `AmmoType` names them.
struct ShellParams
{
    /// AmmoType::_airFriction. Negative in the shipped data (it is a deceleration coefficient).
    float airFriction = 0.0f;
    /// AmmoType::_coefGravity. 1 for an ordinary round.
    float coefGravity = 1.0f;
    /// AmmoType::_initTime -- launch delay before the round starts moving. Usually 0.
    float initTime = 0.0f;
    /// AmmoType::_timeToLive, in seconds.
    float timeToLive = 10.0f;
};

/// Integration step. Exposed so a test can show the answer converges rather than being a property
/// of one chosen step.
///
/// 1/200 s was the first choice and it is measurably too coarse: over an 800 m rifle shot it puts
/// the predicted impact 2.6 cm from an eight-times-finer integration, because forward Euler's
/// error is linear in the step and 1.3 s of flight is ~260 steps of accumulating bias. 1/500 s
/// brings that under a centimetre and still costs only ~650 steps per trajectory, so a
/// 4-iteration aim solve is a few thousand multiply-adds -- nothing next to the target selection
/// that precedes it.
///
/// Note what the "correct" answer even is here: the engine itself integrates with the SAME
/// forward-Euler rule at the frame delta (ShotShell::Simulate), so a real trajectory is mildly
/// framerate-dependent and no fixed step reproduces every frame rate exactly. Making the
/// predictor arbitrarily fine would therefore buy nothing real -- the bar is "far below weapon
/// dispersion", not "converged to machine precision".
inline constexpr float DefaultStep = 1.0f / 500.0f;

/// Longest flight the predictor will integrate, whatever the ammo's own timeToLive says. The fork
/// clamps to 10 s for the same reason: a solver must terminate, and no aiming decision depends on
/// where a round is 30 seconds after launch.
inline constexpr float MaxFlightTime = 10.0f;

struct Impact
{
    /// Where the round crossed the requested distance, or where it expired.
    Vector3 position;
    /// Flight time to that point, in seconds.
    float time = 0.0f;
    /// False when the round expired (timeToLive or MaxFlightTime) before reaching the distance.
    bool reached = false;
};

/// Fly the round from `start` with initial velocity `velocity` until it is `distance` metres from
/// `start`, and report where and when that happened.
Impact SimulateAtDistance(const ShellParams& shell, Vector3Par start, Vector3Par velocity, float distance,
                          float step = DefaultStep);

/// Fly the round and report the point of closest approach to a target moving at constant velocity.
/// `error` is that closest distance -- zero means a hit.
struct Approach
{
    Vector3 position;
    float time = 0.0f;
    float error = 0.0f;
};
Approach SimulateAgainstTarget(const ShellParams& shell, Vector3Par start, Vector3Par velocity, Vector3Par targetPos,
                               Vector3Par targetSpeed, float step = DefaultStep);

/// The aiming answer: a UNIT direction to fire in so that a round leaving `firingPos` at
/// `initSpeed` (plus the shooter's own `shooterSpeed`) meets a target at `targetPos` moving at
/// `targetSpeed`.
///
/// Iterative, because there is no closed form once drag is in: fire at the target, see where the
/// round actually goes, correct by the miss, repeat. Converges in a handful of passes; the fork
/// uses 4 and so do we.
Vector3 CompensateAim(const ShellParams& shell, float initSpeed, Vector3Par shooterSpeed, Vector3Par firingPos,
                      Vector3Par targetPos, Vector3Par targetSpeed, int iterations = 4, float step = DefaultStep);

/// The drag-free closed form the engine uses today, exposed so tests can measure the two against
/// each other rather than asserting a hand-copied number. Returns a unit direction.
///
/// This is `Man::GetAimTargetPosition`'s arithmetic, isolated: fall = t^2 * g / 2 with
/// t = distance / initSpeed, applied as a vertical offset to the straight-line direction.
Vector3 CompensateAimVacuum(float initSpeed, Vector3Par firingPos, Vector3Par targetPos);

} // namespace Ballistics
} // namespace Poseidon
