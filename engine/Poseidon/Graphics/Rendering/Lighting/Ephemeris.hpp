#pragma once

// Low-precision solar / lunar ephemeris. Pure math: no engine types, no globals, no
// allocation — so it can be unit-tested against published almanac positions without
// standing a World up (see tests/unit/engine/Poseidon/Graphics/test_ephemeris.cpp).
//
// WHY THIS EXISTS
// ---------------
// The 2001 engine positioned the sun and the moon with a toy orbital model
// (Lights.cpp: a 23-degree "earth axis" matrix, a 5-degree "moon orbit" matrix, and a
// 28-day lunar month advanced straight off the year fraction). It has no eccentricity,
// no lunar perturbations, no node regression, no equation of time, and its lunar month
// is 0.53 days short — so the moon drifts a full phase every ~50 in-game days and its
// position is wrong by tens of degrees. It also ignores longitude entirely (the world
// config's `longitude` field is loaded and then never read).
//
// MODEL
// -----
// Sun: the classic two-body Keplerian solar position (Astronomical Almanac "low
// precision" / Schlyter). Accuracy ~0.01 deg in longitude over 1900-2100.
//
// Moon: the same Keplerian orbit plus the standard truncated perturbation series
// (evection, variation, yearly equation, parallactic equation, and the largest
// latitude/distance terms). Accuracy ~2 arcmin (0.03 deg) in ecliptic longitude,
// ~10 arcsec in latitude, ~0.01 Earth radii in distance. That is roughly a
// fifteenth of the moon's own apparent diameter — far past what any renderer needs.
//
// Both are then converted to equatorial-of-date and to the observer's horizon frame.
// The moon additionally gets the topocentric parallax correction, which is NOT a
// refinement: lunar parallax reaches ~1 degree, i.e. about two lunar diameters, so
// skipping it would put the moon visibly off its true altitude near the horizon.
//
// FRAME / SIGN CONVENTIONS
// ------------------------
//  - latitudeRad  : GEOGRAPHIC latitude, north positive. NOTE the world config's
//                   `latitude` field uses the opposite sign (BI convention: negative
//                   is north; see World.hpp / WorldInit.cpp). Use
//                   GeographicLatitudeFromWorld() rather than passing World::GetLatitude()
//                   straight in.
//  - longitudeRad : east positive (this matches the world config directly).
//  - azimuthRad   : measured from NORTH, increasing toward EAST, in [0, 2pi).
//  - altitudeRad  : above the true horizon (no atmospheric refraction applied).
//  - UT           : the caller supplies Universal Time. The engine's clock is LOCAL
//                   mean solar time, so JulianDayFromLocal() subtracts longitude/15 h.

namespace Poseidon::astro
{

// Observer position on the globe.
struct GeoObserver
{
    double latitudeRad = 0.0;  // geographic, north positive
    double longitudeRad = 0.0; // east positive
};

// One body, resolved into every frame a renderer or a lighting model might want.
struct BodyState
{
    double eclipticLonRad = 0.0; // geocentric apparent ecliptic longitude
    double eclipticLatRad = 0.0; // geocentric apparent ecliptic latitude
    double rightAscensionRad = 0.0;
    double declinationRad = 0.0;
    double azimuthRad = 0.0;  // from north, toward east
    double altitudeRad = 0.0; // above horizon
    double distanceKm = 0.0;
};

struct SkyState
{
    BodyState sun;
    BodyState moon;

    // Local apparent sidereal time, radians (only the mean value is modelled).
    double localSiderealTimeRad = 0.0;

    // Sun-moon elongation as seen from Earth, radians. 0 = new moon direction.
    double moonElongationRad = 0.0;
    // Phase angle: the Sun-Moon-Earth angle. 0 = full, pi = new.
    double moonPhaseAngleRad = 0.0;
    // Illuminated fraction of the visible disc, 0 (new) .. 1 (full).
    double moonIlluminatedFraction = 0.0;
    // Synodic age as a fraction of the lunation: 0 = new, 0.25 = first quarter,
    // 0.5 = full, 0.75 = last quarter. This is the convention the legacy engine's
    // `LightSun::MoonPhase()` used to drive the moon.p3d texture animation, so it is
    // reported in the same units to keep that path working.
    double moonAgeFraction = 0.5;
    // Position angle of the midpoint of the bright limb, measured from the celestial
    // north pole toward the east (Meeus 48.5). The terminator is perpendicular to it.
    // Only needed by a renderer that orients a phase TEXTURE; a renderer that shades
    // a sphere with the sun direction gets the same answer for free.
    double moonBrightLimbAngleRad = 0.0;
    // Apparent angular RADIUS of the lunar disc, radians (~0.0045, i.e. ~0.26 deg).
    // Varies ~+/-6% over an anomalistic month — this is the real "supermoon" effect.
    double moonAngularRadiusRad = 0.0;
    // Total brightness of the moon relative to a full moon at mean distance (1.0).
    // See MoonPhaseBrightness().
    double moonRelativeBrightness = 0.0;
};

// Julian Day number (including fraction) from a UT calendar date. Gregorian.
// `hoursUT` may be any real number of hours; it is not required to be in [0,24).
double JulianDay(int year, int month, int day, double hoursUT);

// Same, but the caller has LOCAL MEAN SOLAR time (what the engine's clock holds:
// 12:00 means "sun near the meridian"). Converts to UT using the observer longitude
// only — no civil time zone, which is exactly the behaviour the legacy model had.
double JulianDayFromLocal(int year, int month, int day, double hoursLocal, double longitudeRad);

// The whole computation. ~40 sin/cos; costs nothing at the once-per-simulation-tick
// rate LightSun::Recalculate runs at.
SkyState Compute(double julianDay, const GeoObserver& observer);

// Total lunar brightness relative to full moon, as a function of phase angle in
// DEGREES (0 = full, 180 = new).
//
// This is the standard empirical lunar phase function
//     dm = 0.026*|a| + 4.0e-9*a^4      (magnitudes)
//     ratio = 10^(-0.4*dm)
// (Allen, Astrophysical Quantities; the same form Krisciunas & Schaefer 1991 use for
// their sky-brightness model). It is deliberately NOT linear in illuminated area: the
// lunar regolith backscatters hard, so a first-quarter moon lights the ground about
// ELEVEN times less than a full moon even though half the disc is lit. Values:
//     a=0   -> 1.000     (full)
//     a=45  -> 0.336
//     a=90  -> 0.091     (quarter)
//     a=135 -> 0.0146
//     a=180 -> 0.00028   (new; not exactly zero, but 3.5 magnitudes below a crescent)
double MoonPhaseBrightness(double phaseAngleDeg);

// Convert the world config's `latitude` (BI convention: NEGATIVE IS NORTH) into a
// geographic latitude in radians. World::GetLatitude() already returns radians in the
// config's sign convention, so this is just a negation — named so the sign flip is
// documented at every call site instead of being folded silently into a matrix.
inline double GeographicLatitudeFromWorld(double worldLatitudeRad)
{
    return -worldLatitudeRad;
}

} // namespace Poseidon::astro
