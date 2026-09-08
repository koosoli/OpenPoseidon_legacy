// MEASURED CEILINGS, and the method, because the obvious one does not work: flying up cannot
// measure a ceiling, since the AI climbs at a flat 10.4 m/s whatever margin is left. The
// aircraft is spawned AT altitude and asked whether it holds.
//
//   A-10    ~10,000-10,600 m stock  ->  ~14,000-14,400 m, bracketed from both directions
//   UH-60        ~2,280 m stock     ->  ~5,500-5,700 m
//
// The A/B at 3,111 m: the stock helicopter sinks to 2,251 m; the modern one CLIMBS AWAY, to
// 4,903 m and still rising. An earlier write-up of this commit said it "holds 3,111 m dead
// steady" -- that number was read out of a log while the run was still writing it, and it is
// wrong. Recorded here rather than quietly dropped, because a measurement that is wrong in the
// harmless direction is the kind nobody re-checks.
#pragma once

// AIR-010: the flight ceiling.
//
// Stock OFP does not clamp an aircraft's altitude anywhere. There is no
// `if (y > limit) y = limit`, no config `maxAltitude`, and no AI rule that
// refuses to climb. The ceiling is entirely aerodynamic, and it is produced by
// ONE coefficient in each of the two air simulations:
//
//   Airplane.cpp   altCoef = 1 - (alt - 5000) / 8000     -> 0 at 13000 m
//   Helicopter.cpp altCoef = 1 - (alt - 1000) / 2000     ->  0 at  3000 m
//
// That coefficient scales thrust, lift, control authority and drag together, so
// it stands in for air density. Two things are wrong with it.
//
// FIRST, it is a straight line to zero, which is a WALL rather than a falloff.
// Real density decays exponentially and never reaches zero, so a real aircraft
// runs out of climb rate asymptotically -- it mushes, it does not stop. The
// linear ramp makes the last few hundred metres feel like hitting glass.
//
// SECOND -- and this is a live defect, not a matter of taste -- NOTHING CLAMPS
// THE COEFFICIENT AT ZERO. Above 13000 m (plane) or 3000 m (helicopter) it goes
// NEGATIVE, and every term it multiplies inverts with it:
//   * thrust pushes the aircraft backwards,
//   * lift pulls it down,
//   * and `friction *= GetMass() * altCoef` (Airplane.cpp) turns DRAG into an
//     accelerant, which injects energy instead of removing it.
// The aircraft cannot normally climb that high under its own power, so the bug
// is latent -- but `setPos`, `setVelocity`, a parachute drop or a scripted
// insertion can all put an entity there, and the simulation then misbehaves in a
// way that reads as a physics explosion rather than as thin air.
//
// The replacement here is an exponential density falloff with a positive floor.
// It keeps the thing the original was reaching for -- power and lift fall away
// with altitude, so the climb rate decays and the aircraft still has a natural
// ceiling it cannot simply power through -- while removing the wall and the sign
// flip. The scale heights are chosen so the ceiling lands where an aircraft of
// the period actually tops out rather than at an arbitrary round number.
//
// The classic curve is kept, exactly as it was, behind `modernModel = false`, so
// the two can be A/B'd at runtime without a rebuild.
//
// WHAT ACTUALLY LIMITS HIGH-ALTITUDE FLIGHT NOW: THE FAR PLANE, NOT THE AIR.
//
// Raising the ceiling makes the aircraft go higher. It does NOT make the world
// visible up there, and the two limits are nowhere near each other.
//
// The camera's far plane is the view distance. `Scene::GetFogMaxRange()` is what
// World.cpp:1598 hands to `SetPerspectiveForView`, and it is derived from
// `ENGINE_CONFIG.horizontZ` -- the same slider the player sets in Options, which
// DEFAULTS TO 900 m and is clamped to 10,000 m at the very most
// (UI/Settings/ViewDistance.hpp). Ground 5 km below the aircraft is 5 km away,
// so at the default setting the terrain is clipped from about 900 m upwards.
//
// Measured by freefly capture on Malden, looking down 35 degrees
// (.tmp-ceiling/look.sh):
//   1,500 m, default view distance -> terrain, coastline, roads, all correct.
//   5,000 m, default view distance -> NO GROUND AT ALL. Cloud and sky only.
//   5,000 m, --vd 10000            -> sea and terrain return, heavily fogged.
//  14,000 m, --vd 10000            -> no ground; 14 km is past the 10 km cap.
//
// The sky itself holds up well: at 14 km the cloud deck renders correctly from
// above, blue above it, and it reads as genuine high-altitude flight. Nothing
// divides by an assumed altitude range and nothing was seen to break. The world
// simply is not drawn that far.
//
// So this change gives an aircraft that CAN climb to 14 km, above a world that
// stops being drawn somewhere between 900 m and 10 km depending on the player's
// view distance. Making high-altitude flight look right is a separate piece of
// work in the terrain and fog path -- a far plane decoupled from the fog range,
// and a coarse terrain LOD that survives being 10 km away -- and it is NOT done
// here.

namespace Poseidon::Air
{

/// Runtime-switchable parameters for the altitude falloff.
///
/// Plain struct behind an accessor rather than a set of loose globals: the two
/// call sites are in different translation units, and the dev panel wants to see
/// and edit the same values the simulation reads.
struct FlightCeilingSettings
{
    /// false reproduces the stock OFP linear ramp EXACTLY, sign flip included, so
    /// an A/B against it is a true baseline rather than a partly-fixed one.
    bool modernModel = true;

    /// Altitude below which there is no penalty at all, in metres above sea
    /// level. Left at the stock values: the original picked sensible points for
    /// "thin air starts to matter", and only the decay past them was wrong.
    float planeFullForceAlt = 5000.0f;
    float heliFullForceAlt = 1000.0f;

    /// e-folding height of the falloff above `*FullForceAlt`, in metres. The
    /// coefficient is exp(-(alt - full) / scale), so this is the distance over
    /// which available force drops to ~37% of its sea-level value.
    ///
    /// These are FITTED TO MEASURED CEILINGS, not to the ISA atmosphere -- see the
    /// measurement note in the .cpp for the numbers and the method. The shape is
    /// physical; the scale is calibrated against the aircraft this engine
    /// actually has.
    ///
    /// Do not eyeball these. The first draft guessed 4600 / 1750 from the shape
    /// of the curve alone, and those would have moved the A-10's ceiling from
    /// 10,200 m to 9,963 m -- DOWNWARDS -- while looking like a generous change.
    float planeScaleHeight = 9300.0f;
    float heliScaleHeight = 4400.0f;

    /// The helicopter's OTHER ceiling: the cap on the height the keyboard
    /// collective can command, in metres ABOVE GROUND (HelicopterAI.cpp,
    /// HelicopterAuto::KeyboardPilot). Nothing aerodynamic about it -- it is a
    /// clamp on the pilot helper's wanted height, and in cadet mode stock OFP set
    /// it to 150 m, which stops a player thousands of metres below the air.
    ///
    /// The easy-mode value now defaults to the aerodynamic ceiling instead, so
    /// cadet mode runs out of air rather than out of permission. Set it back to
    /// 150 to restore the stock guard rail.
    ///
    /// The expert value was already 10000 and never bound. It is a setting rather
    /// than a literal so that raising `heliScaleHeight` past a 10 km ceiling
    /// cannot make it start binding unnoticed.
    float heliHelperMaxHeightEasy = 6000.0f;
    float heliHelperMaxHeight = 10000.0f;

    /// Floor on the coefficient. Never zero and never negative: this is what
    /// stops thrust, lift and drag from inverting. Small enough that an aircraft
    /// at the floor is falling, not flying.
    float minCoef = 0.02f;

    /// POSEIDON_FLIGHT_LOG=1. Logs an aircraft's altitude whenever it has moved
    /// 25 m since the last line, in EITHER direction, with the coefficient that
    /// applied there. Both directions matters: the altitude a trace settles at
    /// after being dropped in above the ceiling is the measurement, and a
    /// climb-only log cannot tell a hovering aircraft from a falling one.
    bool logSamples = false;
};

/// The live settings. Mutated by the dev panel and by the environment overrides
/// applied on first use.
FlightCeilingSettings& FlightCeiling();

/// Available-force coefficient for a fixed-wing aircraft at `altMetres` above sea
/// level. Multiplies thrust, lift, control surface authority and drag.
float PlaneAirCoef(float altMetres);

/// Available-force coefficient for a rotor at `altMetres` above sea level.
float HeliAirCoef(float altMetres);

} // namespace Poseidon::Air
