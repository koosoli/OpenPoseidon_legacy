#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Graphics/Rendering/Lighting/Ephemeris.hpp>

#include <cmath>

// Reference values are from Jean Meeus, "Astronomical Algorithms" (2nd ed.) worked
// examples, plus two real syzygy timings. They are the assertion: the point of pinning
// them is that a regression in the series has to disagree with a PUBLISHED position,
// not merely with whatever this code happened to produce last week.
//
// Tolerances are the accuracy claimed in Ephemeris.hpp, not the accuracy measured.
// The measured errors at the time of writing are noted per case so a future tightening
// has a number to aim at.

using namespace Poseidon::astro;

namespace
{
constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;

double Deg(double rad)
{
    return rad * kRad2Deg;
}
} // namespace

TEST_CASE("Julian Day matches the standard Gregorian conversion", "[graphics][ephemeris]")
{
    // Meeus 7.a / 7.b.
    CHECK(JulianDay(1957, 10, 4, 19.0 + 26.0 / 60.0) == Catch::Approx(2436116.31).margin(0.01));
    CHECK(JulianDay(2000, 1, 1, 12.0) == Catch::Approx(2451545.0).margin(1e-6));
    CHECK(JulianDay(1992, 4, 12, 0.0) == Catch::Approx(2448724.5).margin(1e-6));
    // 0h UT must land on a .5 fraction — the sidereal-time recovery inside Compute()
    // depends on that, so it is worth asserting directly.
    const double jd = JulianDay(2026, 8, 12, 0.0);
    CHECK(std::fabs(jd + 0.5 - std::floor(jd + 0.5)) < 1e-9);
}

TEST_CASE("Solar position matches Meeus example 25.a", "[graphics][ephemeris]")
{
    // 1992 October 13.0 TD. Meeus gives geometric ecliptic longitude 199.90604 deg and
    // radius vector 0.99760775 AU. (His 199.90988 is the APPARENT longitude, i.e. after
    // nutation and aberration, which this low-precision model does not apply.)
    // Measured error: 0.003 deg in longitude, 7e-5 AU in distance.
    const SkyState s = Compute(JulianDay(1992, 10, 13, 0.0), GeoObserver{0.0, 0.0});
    CHECK(Deg(s.sun.eclipticLonRad) == Catch::Approx(199.90604).margin(0.02));
    CHECK(s.sun.distanceKm / 149597870.7 == Catch::Approx(0.99760775).margin(0.0005));
}

TEST_CASE("Lunar position matches Meeus example 47.a", "[graphics][ephemeris]")
{
    // 1992 April 12.0 TD. Meeus (full ELP truncation): lambda = 133.162655 deg,
    // beta = -3.229126 deg, Delta = 368409.7 km.
    // Measured error of the truncated series used here: 0.015 deg (54") in longitude,
    // 0.010 deg in latitude, 328 km (0.09%) in distance. 0.015 deg is a thirty-fifth of
    // the moon's apparent DIAMETER, so it is invisible at any rendering scale.
    const SkyState s = Compute(JulianDay(1992, 4, 12, 0.0), GeoObserver{0.0, 0.0});
    CHECK(Deg(s.moon.eclipticLonRad) == Catch::Approx(133.162655).margin(0.05));
    CHECK(Deg(s.moon.eclipticLatRad) == Catch::Approx(-3.229126).margin(0.03));
    CHECK(s.moon.distanceKm == Catch::Approx(368409.7).margin(600.0));
}

TEST_CASE("Lunar phase matches Meeus example 48.a", "[graphics][ephemeris]")
{
    // Same instant: phase angle 69.0756 deg, illuminated fraction 0.6786.
    const SkyState s = Compute(JulianDay(1992, 4, 12, 0.0), GeoObserver{0.0, 0.0});
    CHECK(Deg(s.moonPhaseAngleRad) == Catch::Approx(69.0756).margin(0.4));
    CHECK(s.moonIlluminatedFraction == Catch::Approx(0.6786).margin(0.005));
    // Waxing gibbous: past first quarter (0.25), short of full (0.5).
    CHECK(s.moonAgeFraction > 0.25);
    CHECK(s.moonAgeFraction < 0.5);
    // Apparent radius near perigee: ~0.27 deg. Mean is 0.259, so the disc must not be
    // hard-coded to the mean.
    CHECK(Deg(s.moonAngularRadiusRad) == Catch::Approx(0.2704).margin(0.01));
}

TEST_CASE("Real syzygies land where the almanac says", "[graphics][ephemeris]")
{
    SECTION("full moon, 2024-09-18 02:34 UT")
    {
        const SkyState s = Compute(JulianDay(2024, 9, 18, 2.57), GeoObserver{0.0, 0.0});
        CHECK(s.moonIlluminatedFraction > 0.999);
        CHECK(s.moonAgeFraction == Catch::Approx(0.5).margin(0.01));
        CHECK(s.moonRelativeBrightness > 0.97);
    }
    SECTION("new moon, 2024-01-11 11:57 UT")
    {
        const SkyState s = Compute(JulianDay(2024, 1, 11, 11.95), GeoObserver{0.0, 0.0});
        CHECK(s.moonIlluminatedFraction < 0.005);
        // Age wraps at new moon, so accept either end.
        const double age = s.moonAgeFraction;
        CHECK((age < 0.01 || age > 0.99));
        CHECK(s.moonRelativeBrightness < 0.01);
    }
}

TEST_CASE("Horizon frame is right-way-up and right-way-round", "[graphics][ephemeris]")
{
    // The failure this catches is a latitude or azimuth SIGN error, which produces a
    // perfectly plausible-looking sky that is mirrored north-south.
    const double lonRad = 15.0 / kRad2Deg;

    SECTION("northern-hemisphere summer noon: sun high and due south")
    {
        const double latRad = 50.0 / kRad2Deg;
        const SkyState s =
            Compute(JulianDayFromLocal(2020, 6, 21, 12.0, lonRad), GeoObserver{latRad, lonRad});
        // 90 - 50 + 23.44 = 63.4 deg.
        CHECK(Deg(s.sun.altitudeRad) == Catch::Approx(63.4).margin(0.5));
        // Within a degree of due south; the residual is the equation of time.
        CHECK(Deg(s.sun.azimuthRad) == Catch::Approx(180.0).margin(1.5));
    }
    SECTION("southern-hemisphere summer noon: sun due north")
    {
        const double latRad = -33.0 / kRad2Deg;
        const SkyState s =
            Compute(JulianDayFromLocal(2020, 12, 21, 12.0, lonRad), GeoObserver{latRad, lonRad});
        CHECK(Deg(s.sun.altitudeRad) == Catch::Approx(80.4).margin(1.0));
        const double az = Deg(s.sun.azimuthRad);
        CHECK((az < 3.0 || az > 357.0));
    }
    SECTION("midnight puts the sun below the horizon")
    {
        const double latRad = 50.0 / kRad2Deg;
        const SkyState s =
            Compute(JulianDayFromLocal(2020, 6, 21, 0.0, lonRad), GeoObserver{latRad, lonRad});
        CHECK(Deg(s.sun.altitudeRad) < -10.0);
    }
    SECTION("polar day: sun never sets at the north pole in June")
    {
        const double latRad = 89.0 / kRad2Deg;
        for (double h = 0.0; h < 24.0; h += 3.0)
        {
            const SkyState s =
                Compute(JulianDayFromLocal(2020, 6, 21, h, 0.0), GeoObserver{latRad, 0.0});
            CHECK(s.sun.altitudeRad > 0.0);
        }
    }
}

TEST_CASE("World latitude sign convention is inverted on the way in", "[graphics][ephemeris]")
{
    // CfgWorlds `latitude` is NEGATIVE for the northern hemisphere (the engine's own
    // comment: "-40 - Croatia, -90 - north pole"). Getting this backwards is the single
    // most likely way for a correct ephemeris to render a wrong sky.
    CHECK(GeographicLatitudeFromWorld(-40.0) == Catch::Approx(40.0));
    CHECK(GeographicLatitudeFromWorld(0.0) == Catch::Approx(0.0));
}

TEST_CASE("Lunar phase brightness is strongly non-linear", "[graphics][ephemeris]")
{
    // The whole point of the empirical phase function: a first-quarter moon lights the
    // ground about an order of magnitude less than a full moon, not half as much.
    CHECK(MoonPhaseBrightness(0.0) == Catch::Approx(1.0));
    CHECK(MoonPhaseBrightness(90.0) == Catch::Approx(0.0910).margin(0.002));
    CHECK(MoonPhaseBrightness(0.0) / MoonPhaseBrightness(90.0) > 8.0);
    CHECK(MoonPhaseBrightness(0.0) / MoonPhaseBrightness(90.0) < 14.0);
    // Monotonically decreasing, and symmetric in the sign of the phase angle.
    double prev = MoonPhaseBrightness(0.0);
    for (double a = 5.0; a <= 180.0; a += 5.0)
    {
        const double v = MoonPhaseBrightness(a);
        CHECK(v < prev);
        CHECK(v == Catch::Approx(MoonPhaseBrightness(-a)));
        prev = v;
    }
    CHECK(MoonPhaseBrightness(200.0) == Catch::Approx(MoonPhaseBrightness(180.0)));
}

TEST_CASE("Moon completes a synodic month in 29.53 days", "[graphics][ephemeris]")
{
    // The legacy model used a 28-day month, which is 1.5 days short — enough to be a
    // whole phase out within two in-game months. Measure the real interval between two
    // consecutive new moons by scanning the illuminated fraction.
    const double start = JulianDay(2024, 1, 11, 11.95); // known new moon
    double bestJd = start + 25.0;
    double bestK = 1.0;
    for (double t = start + 25.0; t < start + 34.0; t += 0.001)
    {
        const double k = Compute(t, GeoObserver{0.0, 0.0}).moonIlluminatedFraction;
        if (k < bestK)
        {
            bestK = k;
            bestJd = t;
        }
    }
    CHECK(bestJd - start == Catch::Approx(29.53).margin(0.35));
}
