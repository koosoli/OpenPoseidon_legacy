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
// This closes it by integrating the shot rather than solving a parabola. The integrator is not an
// approximation of our flight model -- it is the SAME forward-Euler scheme, step for step:
// acceleration from the old speed, position advanced with the old speed, speed updated afterwards,
// launch delay gating both. Reproducing our own simulation is exactly what a predictor has to do.
//
// TWO POINTS WHERE THE OBVIOUS TRANSCRIPTION WOULD BE WRONG for this engine:
//
//  * Spending a whole step decrementing `initTime` without moving is the natural reading of a
//    launch delay. Ours decrements `_initDelay` and, in the same frame, moves if it has gone
//    non-positive (Shots.cpp:686-703). Doing it the other way would put the prediction one step
//    of flight behind the projectile.
//  * Our drag reads `ShotAirspeed(_speed)`, which folds in wind when the ballistics-wind opt-in is
//    on. The predictor deliberately does NOT model wind: an aiming solution cannot know the wind
//    the round will meet along a path it has not flown, and pretending otherwise would make the AI
//    hit better than a human can. With the opt-in off -- the shipping default -- `ShotAirspeed` is
//    the identity and prediction and flight agree exactly.
//
// The unguided-rocket half is ported too and lives further down (`SimulateUnguidedMissile*`,
// `CompensateUnguidedMissile`). It is a genuinely different model, not a variation -- see the
// comment above MissileParams for why a rocket droops less than a shell of the same speed.
class AmmoType;

namespace Poseidon
{
namespace Ballistics
{

/// Rigid carrier velocity at a world-space offset from its centre of mass.
inline Vector3 CarrierPointVelocity(Vector3Par linear, Vector3Par angular, Vector3Par offset)
{
    return linear + angular.CrossProduct(offset);
}

/// Which drag law a projectile obeys.
///
/// BAL-010 asks for a "verified drag / ballistic coefficient model". The honest position is that
/// there is no single right answer, so this is per-ammo data rather than an engine-wide decision:
///
///  * `Constant` -- `Cd` does not vary with speed, i.e. `accel = k * v^2` with a fixed k. This is
///    what the engine has always done and remains the DEFAULT, so content that says nothing keeps
///    exactly today's flight. It is also the only sane model for things the standard projectiles
///    do not resemble at all: APFSDS long rods, HEAT shells, 40 mm grenades.
///  * `G7` -- boat-tail spitzer standard. Matches 1985 military rifle ball (M855, M80, 7N1,
///    .50 M33), and a single G7 ballistic coefficient stays near-constant across the velocity
///    band for those bullets.
///  * `G1` -- flat-based, blunt-nosed standard. A worse shape match for the same bullets, so one
///    G1 coefficient drifts with velocity. It is here because far MORE G1 data is published than
///    G7, and a cartridge with a trustworthy G1 number beats one with a guessed G7 number.
///
/// Carrying both is deliberate. Which function fits better is a property of the projectile; which
/// function has data is a property of the literature. Neither should be settled in code.
enum class DragModel : uint8_t
{
    Constant,
    G1,
    G7,
};

/// Sea-level ICAO standard atmosphere. Both are needed once drag varies with Mach.
///
/// These are CONSTANTS because the engine has no air temperature: `World/Weather/` models wind and
/// rain and nothing thermal. When a temperature model arrives, speed of sound becomes
/// `20.05 * sqrt(T_kelvin)` and density follows the ideal gas law, and this is where they plug in.
/// Until then a mission in a Takistan summer and one in a Nogova winter fly identically, which is
/// worth knowing before anyone reports it as a bug.
inline constexpr float SpeedOfSound = 340.3f; ///< m/s at 15 C
inline constexpr float AirDensity = 1.225f;   ///< kg/m^3 at 15 C, 1013.25 hPa

/// Ballistic coefficients are conventionally quoted in lb/in^2. Multiply by this for kg/m^2.
inline constexpr float BallisticCoefficientToSI = 703.06958f;

/// Standard-projectile drag coefficient at a given Mach number, linearly interpolated between the
/// tabulated rows and clamped at both ends.
///
/// Returns 0 for DragModel::Constant, which has no table -- callers must not reach here for it.
float StandardDragCoefficient(DragModel model, float mach);

/// The retardation coefficient `k` in `accel = k * v^2`, NEGATIVE.
///
/// For `Constant` this is simply `airFriction`. For a drag function it is
///
///     k = -(rho * pi * Cd(M)) / (8 * BC)
///
/// which follows from `a = rho*v^2*Cd_bullet*A/(2m)` with `Cd_bullet = i*Cd_std` and
/// `BC = m/(i*d^2)`: the projectile's diameter and mass cancel, leaving only the standard function
/// and the coefficient. Cross-validated 2026-08-27 -- M855 integrated with G1 at BC 0.304 and with
/// G7 at BC 0.151 agrees to 0.3 % at 300 m, which only happens if this normalisation is right.
/// See docs/ballistics/drag-functions/SOURCE.md.
float DragRetardation(DragModel model, float airFriction, float ballisticCoefficient, float speed);

/// The projectile properties the flight model actually reads.
struct ShellParams
{
    /// Drag coefficient in `accel = speed * |speed| * airFriction`. NEGATIVE -- it decelerates.
    ///
    /// Read from the ammo's `airFriction` config property, falling back to DefaultAirFriction
    /// when the property is absent, which is the case for all classic CWA content.
    float airFriction = 0.0f;
    /// `coefGravity` from the ammo's config. 1 for an ordinary round.
    float coefGravity = 1.0f;
    /// AmmoType::initTime -- launch delay before the round starts moving. Usually 0.
    float initTime = 0.0f;
    /// `timeToLive` from the ammo's config, else the engine default.
    float timeToLive = 10.0f;
    /// `dragModel` from the ammo's config: "G1", "G7", or absent for the constant law.
    DragModel dragModel = DragModel::Constant;
    /// `ballisticCoefficient` from the ammo's config, converted to kg/m^2. Only consulted when
    /// `dragModel` is not Constant; zero or negative disables the drag function and falls back.
    float ballisticCoefficient = 0.0f;

    /// Projectile mass in kg and calibre in metres, for the penetration model.
    ///
    /// CWA's CfgAmmo carries NEITHER -- it is a 2001 gameplay config, not a
    /// ballistics one -- so these come from the same overlay as the drag data
    /// (`bulletMass`, `caliber`). Absent an overlay they fall back to a
    /// 5.56-sized round, which is right for rifles and wrong for a tank shell;
    /// `massAssumed` says which happened, so nothing presents a guess as a
    /// measurement.
    float mass = 0.004f;
    float calibre = 0.00556f;
    bool  massAssumed = true;
};

/// The coefficient every round used before `airFriction` became config-driven, and the fallback
/// for any ammo that still does not define one.
///
/// It is not arbitrary and it is worth knowing why it survived so long: for a v^2 drag law the
/// speed decays as `v(x) = v0 * exp(k*x)`, so `k = ln(v/v0)/x`. Feeding real 7.62x51 retained
/// velocity -- 838 m/s at the muzzle, about 700 m/s at 300 m -- gives k = ln(700/838)/300 =
/// -6.0e-4. The engine's long-standing -5e-4 is a passable 7.62. It is simply wrong for
/// everything else: the same number makes a 5.56 far too flat and a tank sabot far too draggy.
inline constexpr float DefaultAirFriction = -0.0005f;

/// The ONE place that says what a given ammo type flies like.
///
/// This exists because the bug this whole file addresses was a second, divergent copy of the
/// flight model. Re-deriving the parameters at the aiming site would reintroduce exactly that:
/// the day someone makes airFriction config-driven, an aim solver reading its own defaults would
/// silently keep aiming for the old ones. So `ShotShell`'s constructor takes its values from HERE
/// -- prediction and flight cannot disagree without the compiler noticing.
///
/// Cached per AmmoType, because the underlying values come from a ParamEntry lookup and this is
/// called per aim update per unit.
ShellParams ShellParamsFor(const AmmoType& type);

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

/// Longest flight the predictor will integrate, whatever the ammo's own timeToLive says: a solver
/// must terminate, and no aiming decision depends on where a round is 30 seconds after launch.
inline constexpr float MaxFlightTime = 10.0f;

/// Rockets fly slower and burn for seconds, so the shell cap is too tight for them. The engine's
/// own missile timeToLive default is 20 s, which is the bound used here.
inline constexpr float MaxMissileFlightTime = 20.0f;

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
/// round actually goes, correct by the miss, repeat. Converges in a handful of passes; four.
Vector3 CompensateAim(const ShellParams& shell, float initSpeed, Vector3Par shooterSpeed, Vector3Par firingPos,
                      Vector3Par targetPos, Vector3Par targetSpeed, int iterations = 4, float step = DefaultStep);

/// What the two aiming call sites in the engine actually need: for a level shot at `distance`,
/// how much higher than the target must the barrel point, and how long is the round in the air.
///
/// This is the shape the existing code already works in. `Man::AdjustWeapon` (the PLAYER's sight
/// zeroing) wants the drop as a SLOPE -- drop/distance -- to add to a unit direction's Y.
/// `Man::CalculateAimWeapon` (infantry AI) wants the absolute drop in metres to add to the target
/// position, and the flight time to scale its target lead by. Returning both from one solve keeps
/// those two consistent with each other, which the vacuum formula managed only by accident.
///
/// Deliberately a LEVEL-shot solve rather than the full CompensateAim: the call sites apply their
/// own lead, their own accuracy degradation and their own ability limits afterwards, and folding
/// a lead-aware solve in here would silently override AI skill. Elevation changes the path only
/// in the second order at small angles, so over the ranges infantry weapons are used the two
/// agree to centimetres -- and where they would not (mortars), that call site is not wired.
struct LevelShot
{
    /// How far the round falls below the line of departure at `distance`, in metres. Aim this
    /// much HIGH. Zero or negative means nothing useful was solved.
    float drop = 0.0f;
    /// Flight time to `distance`, in seconds.
    float time = 0.0f;
    /// False if the round cannot reach that far at all (expired first).
    bool valid = false;
};
LevelShot SolveLevelShot(const ShellParams& shell, float initSpeed, float distance, float step = DefaultStep);

/// The form every wired call site uses: "give me the flight time and the drop for this ammo over
/// this range, or tell me you cannot and I will keep my own arithmetic."
///
/// Returns false -- leaving `time` and `fall` untouched -- when the lever is off, the ammo is not
/// an unpowered ballistic projectile (bullets and shells only; missiles have their own model),
/// or the geometry is outside what a level-shot solve can describe. Every call site MUST keep its
/// original formula for the false case; a solver that silently answers zero is worse than one
/// that admits it does not know.
bool SolveAimForAmmo(const AmmoType* ammo, float initSpeed, float distance, float& time, float& fall);

/// Master switch for the drag-correct aiming path, so the whole change can be A/B'd in one
/// binary. Initialised from `POSEIDON_BALLISTIC_AIM` (0 disables) and settable at runtime from
/// the dev panel's Ballistics tab -- comparing old against new is worth a great deal more when it
/// does not need a restart between the two looks.
///
/// OFF means every wired site falls back to the drag-free vacuum parabola the engine shipped with
/// for twenty-five years. That is the legacy behaviour, not a degraded one.
bool DragCorrectAimEnabled();
void SetDragCorrectAim(bool enabled);

/// Diagnostic override: force EVERY round onto a drag function, ignoring what its config says.
///
/// This exists because the Mach-dependent path is otherwise unreachable: no shipped ammo declares
/// `dragModel`, so without a way to force one there is no way to see or feel the new model at all
/// until per-cartridge ballistic coefficients arrive. A single coefficient applied to every round
/// is of course wrong -- that is the point, it is a comparison tool, not a setting.
///
/// `DragModel::Constant` (or a non-positive coefficient) turns it off and returns every round to
/// its authored data. Rounds already in flight keep the parameters they were fired with.
DragModel ForcedDragModel();
float ForcedBallisticCoefficient();
void SetForcedDragModel(DragModel model, float ballisticCoefficientLbIn2);

// ---------------------------------------------------------------------------------------------
// UNGUIDED ROCKETS
//
// A rocket is not a shell with a bigger number. `Missile::Simulate` gives it a launch delay, a
// thrust phase along its OWN forward axis, and an anisotropic body drag with cubic terms that
// differ by axis -- sideways motion is punished an order of magnitude harder than forward motion,
// which is what keeps a fin-stabilised rocket pointing where it was fired. Gravity therefore does
// not simply bend the path: it is resisted by that side drag, so a rocket droops far less than a
// shell of the same speed, and a shell solver applied to one is wrong in the direction that
// matters (it would over-elevate).
//
// WHAT THIS IS A SIMPLIFICATION OF, stated so nobody mistakes it for the full model. The engine
// flies a missile as a RIGID BODY: `Missile::Simulate` also builds torque, lets aerodynamic
// forces rotate the body toward its velocity, and for a guided missile steers it at a target.
// This predictor holds the orientation FIXED at the launch attitude and integrates the point
// mass. That is exactly right for the question an aiming solver asks -- "along which line do I
// launch an unguided rocket so it arrives" -- and it is exactly wrong for a guided missile, whose
// flight is decided by its steering, not by its launch direction. Guided weapons must not use it.
struct MissileParams
{
    float initTime = 0.0f;        ///< AmmoType::initTime -- delay before the motor lights.
    float thrustTime = 0.0f;      ///< AmmoType::thrustTime -- how long the motor burns.
    float thrust = 0.0f;          ///< AmmoType::thrust -- acceleration along the body's +Z.
    float sideAirFriction = 1.0f; ///< AmmoType::sideAirFriction -- scales the whole drag tensor.
    float timeToLive = 20.0f;
};

/// Companion to ShellParamsFor, and the same rule applies: this is the ONE place that says how a
/// rocket flies, so the predictor cannot drift away from Missile::Simulate.
MissileParams MissileParamsFor(const AmmoType& type);

/// Fly an unguided rocket launched from `startPos` with launch attitude `orientation` (its +Z is
/// the way it points) and initial world velocity `worldSpeed`, until it is `distance` from the
/// start.
Impact SimulateUnguidedMissileAtDistance(const MissileParams& missile, Vector3Par startPos,
                                         const Matrix3& orientation, Vector3Par worldSpeed, float distance,
                                         float step = DefaultStep);

/// Closest approach of an unguided rocket to a target moving at constant velocity.
Approach SimulateUnguidedMissileAgainstTarget(const MissileParams& missile, Vector3Par startPos,
                                              const Matrix3& orientation, Vector3Par worldSpeed,
                                              Vector3Par targetPos, Vector3Par targetSpeed,
                                              float step = DefaultStep);

/// The launch direction for an unguided rocket that is to meet `targetPos`.
///
/// `worldUp` is needed and is not decoration: the rocket's drag is anisotropic in ITS OWN frame,
/// so the answer depends on how the launcher is rolled about its aim axis. Pass the firing
/// platform's up vector; VUp is the right default for infantry and ground vehicles.
Vector3 CompensateUnguidedMissile(const MissileParams& missile, float initSpeed, Vector3Par worldUp,
                                  Vector3Par shooterSpeed, Vector3Par firingPos, Vector3Par targetPos,
                                  Vector3Par targetSpeed, int iterations = 4, float step = DefaultStep);

/// The drag-free closed form the engine uses today, exposed so tests can measure the two against
/// each other rather than asserting a hand-copied number. Returns a unit direction.
///
/// This is `Man::GetAimTargetPosition`'s arithmetic, isolated: fall = t^2 * g / 2 with
/// t = distance / initSpeed, applied as a vertical offset to the straight-line direction.
Vector3 CompensateAimVacuum(float initSpeed, Vector3Par firingPos, Vector3Par targetPos);

} // namespace Ballistics
} // namespace Poseidon
