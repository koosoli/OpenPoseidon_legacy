// AIR-010: the flight ceiling. See FlightCeiling.hpp for what the stock model did
// and why it needed replacing.
//
// HOW THE SCALE HEIGHTS WERE CHOSEN
//
// Not by picking a round number. The two models are tied together at the point
// that actually matters, which is the CRITICAL COEFFICIENT c* -- the value of
// altCoef at which the aircraft can no longer hold altitude. c* is a property of
// the airframe and of the rest of the flight model, not of the falloff curve, so
// it is the SAME number under either curve. That makes the old ceiling directly
// usable as a calibration point:
//
//   1. Find where the aircraft settles under the stock linear ramp. That is
//      h_old.
//   2. c* = 1 - (h_old - full) / (noForce - full), evaluated on the stock ramp.
//   3. Under exp(-(h - full) / scale) the same aircraft settles where the
//      coefficient again equals c*, so the scale height that puts the ceiling at
//      a chosen h_new is
//              scale = (h_new - full) / ln(1 / c*).
//
// HOW h_old WAS MEASURED. Not by flying up from the ground: the AI climbs at a
// flat ~10 m/s no matter how much margin it has left, so a climb from Malden to
// 10 km is twenty minutes of wall clock and the last kilometre looks exactly
// like the first. Instead the aircraft is SPAWNED at altitude (`setPos` from the
// unit's init -- the FLY special ignores the altitude in the mission entry) and
// the question becomes "can it stay here", which settles in about a minute.
// .tmp-ceiling/probe.sh does this; POSEIDON_FLIGHT_LOG=1 gives the trace.
//
// Measured on b83e5cb9, A-10 and UH-60 on Malden, AI pilot with a MOVE waypoint:
//
//   A-10   climbs from 8000 (settles 8545); sinks from 10000 (settles 9988) and
//          from 11000 (settles 10622)      -> h_old ~= 10200 m -> c* ~= 0.35
//   UH-60  climbs from  500 (still rising at 2327); sinks from 2600 and from
//          2900, BOTH settling at 2270-2280 -> h_old ~=  2280 m -> c* ~= 0.36
//
// The helicopter is the trustworthy one: two runs approaching from different
// altitudes settle within 15 m of each other, and the coefficient the log prints
// at the settling point (0.359, 0.361) is c* read directly off the aircraft.
// The plane porpoises instead of settling, so its c* is good to about +-0.04.
//
// Targets, taken from the real aircraft rather than invented:
//   A-10A service ceiling 13,700 m; a period fast jet is ~15 km. Target 15000 m.
//   UH-60A service ceiling ~5,800 m. Target 5500 m.
//
//   plane scale = (15000 - 5000) / ln(1/0.34) = 9270 m -> 9300 in the header
//   heli  scale = ( 5500 - 1000) / ln(1/0.36) = 4405 m -> 4400 in the header
//
// A WARNING ABOUT GUESSING THESE. The first draft of this file put 4600 and 1750
// in the header, reasoned about from the shape of the curve without measuring.
// Those give ceilings of 9,963 m and 2,788 m -- the plane would have gone DOWN
// by 240 m while the change was described as raising it to 15 km. Nothing about
// the code would have looked wrong. Re-derive from a measurement if the flight
// model changes; the closed form is only as good as the c* fed into it.

#include <Poseidon/World/Entities/Vehicles/Air/FlightCeiling.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <cmath>
#include <cstdlib>

namespace Poseidon::Air
{

namespace
{

bool EnvBool(const char* name, bool fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return fallback;
    }
    return !(v[0] == '0' && v[1] == '\0');
}

void EnvFloat(const char* name, float& target)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return;
    }
    char* end = nullptr;
    const float parsed = std::strtof(v, &end);
    if (end != v)
    {
        target = parsed;
    }
}

/// Environment overrides are read ONCE, on first use. They exist so the curve can
/// be tuned across runs without a rebuild; the dev panel is the in-session lever
/// and writes the same struct.
FlightCeilingSettings MakeSettings()
{
    FlightCeilingSettings s;
    s.modernModel = EnvBool("POSEIDON_FLIGHT_MODERN", s.modernModel);
    s.logSamples = EnvBool("POSEIDON_FLIGHT_LOG", s.logSamples);
    EnvFloat("POSEIDON_FLIGHT_PLANE_FULL", s.planeFullForceAlt);
    EnvFloat("POSEIDON_FLIGHT_PLANE_SCALE", s.planeScaleHeight);
    EnvFloat("POSEIDON_FLIGHT_HELI_FULL", s.heliFullForceAlt);
    EnvFloat("POSEIDON_FLIGHT_HELI_SCALE", s.heliScaleHeight);
    EnvFloat("POSEIDON_FLIGHT_MIN_COEF", s.minCoef);
    EnvFloat("POSEIDON_FLIGHT_HELI_HELPER_EASY", s.heliHelperMaxHeightEasy);
    EnvFloat("POSEIDON_FLIGHT_HELI_HELPER", s.heliHelperMaxHeight);
    return s;
}

/// Logs whenever altitude has MOVED 25 m from the last line, in either direction.
///
/// The first version logged only new records, and that cannot answer the question
/// the probe is asking. Spawn an aircraft at 9000 m and a record-only log emits
/// one line at 9000 and then goes quiet whether the aircraft is holding station
/// or falling out of the sky -- the two cases that have to be told apart. Logging
/// movement in both directions makes a sink visible as a descending trace, and a
/// ceiling as the altitude the trace turns around at.
///
/// Not a periodic sample either: a settled aircraft would print the same number
/// for ever and bury the interesting part.
///
/// Separate state per aircraft class, because a plane and a helicopter in one
/// mission would otherwise interleave into a single meaningless trace.
void LogMove(const char* what, float& last, float alt, float coef)
{
    if (last > -1e8f && std::fabs(alt - last) < 25.0f)
    {
        return;
    }
    last = alt;
    LOG_INFO(World, "AIR-010 {} altitude {:.0f} m, force coef {:.3f}", what, alt, coef);
}

/// The stock OFP curve, reproduced exactly -- including the fact that it is not
/// clamped and goes negative past `noForce`. Kept unclamped ON PURPOSE: this
/// branch exists to be an honest baseline for an A/B, and a baseline with the bug
/// quietly fixed would make the comparison say the wrong thing about what the
/// change bought.
float LegacyCoef(float alt, float full, float noForce)
{
    if (alt <= full)
    {
        return 1.0f;
    }
    return 1.0f - (alt - full) * (1.0f / (noForce - full));
}

/// The replacement: exponential decay with a positive floor.
///
/// exp() rather than the ISA polynomial because ISA is only valid to 11 km and
/// needs a second branch above it, and because the whole point is the SHAPE --
/// asymptotic, never zero, never negative. A tuned exponential gives that in one
/// expression with one parameter the dev panel can move.
float ModernCoef(float alt, float full, float scale, float minCoef)
{
    if (alt <= full)
    {
        return 1.0f;
    }
    if (scale <= 1.0f)
    {
        // A degenerate scale height would divide by ~0 and hand back a NaN that
        // then propagates into thrust, lift and the position integrator. Fail to
        // the floor instead: thin air everywhere is survivable, NaN is not.
        return minCoef;
    }
    const float coef = std::exp(-(alt - full) / scale);
    return coef < minCoef ? minCoef : coef;
}

} // namespace

FlightCeilingSettings& FlightCeiling()
{
    static FlightCeilingSettings s = MakeSettings();
    return s;
}

float PlaneAirCoef(float altMetres)
{
    const FlightCeilingSettings& s = FlightCeiling();
    const float coef = s.modernModel
                           ? ModernCoef(altMetres, s.planeFullForceAlt, s.planeScaleHeight, s.minCoef)
                           : LegacyCoef(altMetres, s.planeFullForceAlt, 13000.0f);
    if (s.logSamples)
    {
        static float last = -1e9f;
        LogMove("plane", last, altMetres, coef);
    }
    return coef;
}

float HeliAirCoef(float altMetres)
{
    const FlightCeilingSettings& s = FlightCeiling();
    const float coef = s.modernModel ? ModernCoef(altMetres, s.heliFullForceAlt, s.heliScaleHeight, s.minCoef)
                                     : LegacyCoef(altMetres, s.heliFullForceAlt, 3000.0f);
    if (s.logSamples)
    {
        static float last = -1e9f;
        LogMove("helicopter", last, altMetres, coef);
    }
    return coef;
}

} // namespace Poseidon::Air
