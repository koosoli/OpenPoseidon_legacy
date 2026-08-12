#include <Poseidon/Graphics/Rendering/Lighting/Ephemeris.hpp>

#include <cmath>

// Implementation notes are in the header. Everything here is double precision on
// purpose: the day count runs to ~1e4 and the mean motions carry ~10 significant
// digits, so float would lose the moon's position within a season.

namespace Poseidon::astro
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kRad2Deg = 180.0 / kPi;

// Earth equatorial radius (km) — the lunar orbit's semi-major axis is expressed in
// Earth radii by this model, so this is the unit conversion, not a physical constant
// anyone here depends on to high accuracy.
constexpr double kEarthRadiusKm = 6378.137;
constexpr double kMoonRadiusKm = 1737.4;
constexpr double kAstronomicalUnitKm = 149597870.7;

// Reference epoch of this element set: 1999 Dec 31.0 TDT = JD 2451543.5, the epoch
// the Astronomical-Almanac-derived elements below are stated for.
constexpr double kEpochJD = 2451543.5;

double Wrap360(double deg)
{
    double r = std::fmod(deg, 360.0);
    if (r < 0.0)
    {
        r += 360.0;
    }
    return r;
}

double WrapTwoPi(double rad)
{
    double r = std::fmod(rad, kTwoPi);
    if (r < 0.0)
    {
        r += kTwoPi;
    }
    return r;
}

double SinDeg(double deg)
{
    return std::sin(deg * kDeg2Rad);
}

double CosDeg(double deg)
{
    return std::cos(deg * kDeg2Rad);
}

// Solve Kepler's equation for a small eccentricity. Two Newton steps take the
// starting guess below well past double precision for e < 0.06 (the moon's is 0.0549,
// the Earth's 0.0167), so there is no iteration limit to tune.
double EccentricAnomalyDeg(double meanAnomalyDeg, double e)
{
    double E = meanAnomalyDeg + kRad2Deg * e * SinDeg(meanAnomalyDeg) * (1.0 + e * CosDeg(meanAnomalyDeg));
    for (int i = 0; i < 3; ++i)
    {
        const double dE =
            (E - kRad2Deg * e * SinDeg(E) - meanAnomalyDeg) / (1.0 - e * CosDeg(E));
        E -= dE;
        if (std::fabs(dE) < 1e-10)
        {
            break;
        }
    }
    return E;
}

struct Equatorial
{
    double ra = 0.0;  // radians
    double dec = 0.0; // radians
};

// Ecliptic (lon, lat) -> equatorial (RA, Dec), both radians, for obliquity `obl` (rad).
Equatorial EclipticToEquatorial(double lonRad, double latRad, double oblRad)
{
    const double cl = std::cos(latRad);
    const double x = cl * std::cos(lonRad);
    const double y = cl * std::sin(lonRad);
    const double z = std::sin(latRad);

    const double xe = x;
    const double ye = y * std::cos(oblRad) - z * std::sin(oblRad);
    const double ze = y * std::sin(oblRad) + z * std::cos(oblRad);

    Equatorial out;
    out.ra = WrapTwoPi(std::atan2(ye, xe));
    out.dec = std::atan2(ze, std::sqrt(xe * xe + ye * ye));
    return out;
}

// Equatorial -> horizon. `hourAngle` = LST - RA, radians.
void EquatorialToHorizon(double hourAngle, double dec, double latRad, double& azOut, double& altOut)
{
    const double sinLat = std::sin(latRad);
    const double cosLat = std::cos(latRad);
    const double sinDec = std::sin(dec);
    const double cosDec = std::cos(dec);
    const double sinH = std::sin(hourAngle);
    const double cosH = std::cos(hourAngle);

    altOut = std::asin(std::fmax(-1.0, std::fmin(1.0, sinDec * sinLat + cosDec * cosLat * cosH)));
    // atan2 form measured from SOUTH toward WEST; +pi rotates it to the
    // from-north-toward-east convention this module exports.
    const double az = std::atan2(cosDec * sinH, cosDec * cosH * sinLat - sinDec * cosLat);
    azOut = WrapTwoPi(az + kPi);
}

} // namespace

double JulianDay(int year, int month, int day, double hoursUT)
{
    // Standard Gregorian-calendar JD (Meeus 7.1), valid for any date after 1582.
    int y = year;
    int m = month;
    if (m <= 2)
    {
        y -= 1;
        m += 12;
    }
    const int a = static_cast<int>(std::floor(y / 100.0));
    const int b = 2 - a + static_cast<int>(std::floor(a / 4.0));
    const double jd0 = std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + day + b - 1524.5;
    return jd0 + hoursUT / 24.0;
}

double JulianDayFromLocal(int year, int month, int day, double hoursLocal, double longitudeRad)
{
    // Local MEAN SOLAR time -> UT. No civil time zone: the engine's clock has always
    // meant "12:00 is local noon", and a fake time zone would only move the sun off the
    // meridian at midday for no gain.
    const double longitudeHours = longitudeRad * kRad2Deg / 15.0;
    return JulianDay(year, month, day, hoursLocal - longitudeHours);
}

double MoonPhaseBrightness(double phaseAngleDeg)
{
    double a = std::fabs(phaseAngleDeg);
    if (a > 180.0)
    {
        a = 180.0;
    }
    const double dm = 0.026 * a + 4.0e-9 * a * a * a * a;
    return std::pow(10.0, -0.4 * dm);
}

SkyState Compute(double julianDay, const GeoObserver& observer)
{
    SkyState out;

    // Days since the element epoch.
    const double d = julianDay - kEpochJD;

    // Mean obliquity of the ecliptic (deg).
    const double oblDeg = 23.4393 - 3.563e-7 * d;
    const double oblRad = oblDeg * kDeg2Rad;

    // ---------------------------------------------------------------- Sun ----
    const double sunW = 282.9404 + 4.70935e-5 * d;  // argument of perihelion
    const double sunE = 0.016709 - 1.151e-9 * d;    // eccentricity
    const double sunM = Wrap360(356.0470 + 0.9856002585 * d); // mean anomaly
    const double sunL = Wrap360(sunW + sunM);       // mean longitude

    const double sunEcc = EccentricAnomalyDeg(sunM, sunE);
    const double sunXv = CosDeg(sunEcc) - sunE;
    const double sunYv = std::sqrt(1.0 - sunE * sunE) * SinDeg(sunEcc);
    const double sunR = std::sqrt(sunXv * sunXv + sunYv * sunYv);           // AU
    const double sunV = std::atan2(sunYv, sunXv) * kRad2Deg;                // true anomaly
    const double sunLonDeg = Wrap360(sunV + sunW);                          // ecliptic longitude

    out.sun.eclipticLonRad = sunLonDeg * kDeg2Rad;
    out.sun.eclipticLatRad = 0.0; // < 1 arcsec; not modelled
    out.sun.distanceKm = sunR * kAstronomicalUnitKm;

    const Equatorial sunEq = EclipticToEquatorial(out.sun.eclipticLonRad, 0.0, oblRad);
    out.sun.rightAscensionRad = sunEq.ra;
    out.sun.declinationRad = sunEq.dec;

    // ------------------------------------------------------- Sidereal time ----
    // GMST0 is the sidereal time at Greenwich at 00:00 UT, and equals the sun's mean
    // longitude + 180 deg. Recovering UT from the Julian Day rather than threading it
    // through keeps Compute() a function of (JD, observer) alone.
    const double ut = (julianDay + 0.5 - std::floor(julianDay + 0.5)) * 24.0; // hours
    const double gmst0Deg = Wrap360(sunL + 180.0);
    const double lstDeg = gmst0Deg + ut * 15.0 + observer.longitudeRad * kRad2Deg;
    const double lstRad = WrapTwoPi(lstDeg * kDeg2Rad);
    out.localSiderealTimeRad = lstRad;

    const double latRad = observer.latitudeRad;

    {
        const double ha = lstRad - out.sun.rightAscensionRad;
        EquatorialToHorizon(ha, out.sun.declinationRad, latRad, out.sun.azimuthRad, out.sun.altitudeRad);
    }

    // --------------------------------------------------------------- Moon ----
    const double moonN = 125.1228 - 0.0529538083 * d; // longitude of ascending node
    const double moonI = 5.1454;                      // inclination
    const double moonW = 318.0634 + 0.1643573223 * d; // argument of perigee
    const double moonA = 60.2666;                     // semi-major axis, Earth radii
    const double moonE = 0.054900;                    // eccentricity
    const double moonM = Wrap360(115.3654 + 13.0649929509 * d); // mean anomaly

    const double moonEcc = EccentricAnomalyDeg(moonM, moonE);
    const double moonXv = moonA * (CosDeg(moonEcc) - moonE);
    const double moonYv = moonA * std::sqrt(1.0 - moonE * moonE) * SinDeg(moonEcc);
    double moonR = std::sqrt(moonXv * moonXv + moonYv * moonYv);   // Earth radii
    const double moonV = std::atan2(moonYv, moonXv) * kRad2Deg;

    // Orbit plane -> ecliptic rectangular.
    const double vw = moonV + moonW;
    const double xh = moonR * (CosDeg(moonN) * CosDeg(vw) - SinDeg(moonN) * SinDeg(vw) * CosDeg(moonI));
    const double yh = moonR * (SinDeg(moonN) * CosDeg(vw) + CosDeg(moonN) * SinDeg(vw) * CosDeg(moonI));
    const double zh = moonR * (SinDeg(vw) * SinDeg(moonI));

    double moonLonDeg = Wrap360(std::atan2(yh, xh) * kRad2Deg);
    double moonLatDeg = std::atan2(zh, std::sqrt(xh * xh + yh * yh)) * kRad2Deg;

    // Perturbations. Without these the moon is wrong by up to ~1.3 degrees (evection
    // alone is 2.5x the moon's own diameter), which is the difference between "the
    // moon is where it should be" and "the moon is somewhere in the sky".
    {
        const double Ms = sunM;                       // sun mean anomaly
        const double Mm = moonM;                      // moon mean anomaly
        const double Lm = Wrap360(moonN + moonW + moonM); // moon mean longitude
        const double Ls = sunL;                       // sun mean longitude
        const double D = Wrap360(Lm - Ls);            // mean elongation
        const double F = Wrap360(Lm - moonN);         // argument of latitude

        moonLonDeg += -1.274 * SinDeg(Mm - 2.0 * D);  // evection
        moonLonDeg += +0.658 * SinDeg(2.0 * D);       // variation
        moonLonDeg += -0.186 * SinDeg(Ms);            // yearly equation
        moonLonDeg += -0.059 * SinDeg(2.0 * Mm - 2.0 * D);
        moonLonDeg += -0.057 * SinDeg(Mm - 2.0 * D + Ms);
        moonLonDeg += +0.053 * SinDeg(Mm + 2.0 * D);
        moonLonDeg += +0.046 * SinDeg(2.0 * D - Ms);
        moonLonDeg += +0.041 * SinDeg(Mm - Ms);
        moonLonDeg += -0.035 * SinDeg(D);             // parallactic equation
        moonLonDeg += -0.031 * SinDeg(Mm + Ms);
        moonLonDeg += -0.015 * SinDeg(2.0 * F - 2.0 * D);
        moonLonDeg += +0.011 * SinDeg(Mm - 4.0 * D);

        moonLatDeg += -0.173 * SinDeg(F - 2.0 * D);
        moonLatDeg += -0.055 * SinDeg(Mm - F - 2.0 * D);
        moonLatDeg += -0.046 * SinDeg(Mm + F - 2.0 * D);
        moonLatDeg += +0.033 * SinDeg(F + 2.0 * D);
        moonLatDeg += +0.017 * SinDeg(2.0 * Mm + F);

        moonR += -0.58 * CosDeg(Mm - 2.0 * D);
        moonR += -0.46 * CosDeg(2.0 * D);
    }

    moonLonDeg = Wrap360(moonLonDeg);
    out.moon.eclipticLonRad = moonLonDeg * kDeg2Rad;
    out.moon.eclipticLatRad = moonLatDeg * kDeg2Rad;
    out.moon.distanceKm = moonR * kEarthRadiusKm;

    Equatorial moonEq = EclipticToEquatorial(out.moon.eclipticLonRad, out.moon.eclipticLatRad, oblRad);

    // Phase quantities are GEOCENTRIC by definition, so take them before the
    // topocentric shift below.
    {
        const double cosElong = std::cos(out.moon.eclipticLonRad - out.sun.eclipticLonRad) *
                                std::cos(out.moon.eclipticLatRad);
        out.moonElongationRad = std::acos(std::fmax(-1.0, std::fmin(1.0, cosElong)));
        out.moonPhaseAngleRad = kPi - out.moonElongationRad;
        out.moonIlluminatedFraction = 0.5 * (1.0 + std::cos(out.moonPhaseAngleRad));
        out.moonRelativeBrightness = MoonPhaseBrightness(out.moonPhaseAngleRad * kRad2Deg);
        // Synodic age from the ecliptic-longitude difference: 0 at new, 0.5 at full.
        out.moonAgeFraction = Wrap360(moonLonDeg - sunLonDeg) / 360.0;

        // Meeus 48.5 — position angle of the bright limb's midpoint, from the north
        // celestial pole toward the east.
        const double dRa = out.sun.rightAscensionRad - moonEq.ra;
        const double y = std::cos(out.sun.declinationRad) * std::sin(dRa);
        const double x = std::sin(out.sun.declinationRad) * std::cos(moonEq.dec) -
                         std::cos(out.sun.declinationRad) * std::sin(moonEq.dec) * std::cos(dRa);
        out.moonBrightLimbAngleRad = WrapTwoPi(std::atan2(y, x));
    }

    // Topocentric correction. Lunar horizontal parallax reaches ~1 deg — two lunar
    // diameters — so an observer-frame moon that skips this sits visibly too high.
    {
        const double parallax = std::asin(1.0 / moonR); // radians
        // Geocentric latitude of the observer + the geocentric radius factor for an
        // oblate Earth (the flattening term is small but free).
        const double gclat = latRad - 0.1924 * kDeg2Rad * std::sin(2.0 * latRad);
        const double rho = 0.99833 + 0.00167 * std::cos(2.0 * latRad);

        const double ha = lstRad - moonEq.ra;
        // atan2 keeps the quadrant that plain atan(tan(gclat)/cos(HA)) loses.
        double g = std::atan2(std::sin(gclat), std::cos(gclat) * std::cos(ha));

        const double cosDec = std::cos(moonEq.dec);
        if (std::fabs(cosDec) > 1e-9)
        {
            moonEq.ra -= parallax * rho * std::cos(gclat) * std::sin(ha) / cosDec;
        }
        const double sinG = std::sin(g);
        if (std::fabs(sinG) > 1e-9)
        {
            moonEq.dec -= parallax * rho * std::sin(gclat) * std::sin(g - moonEq.dec) / sinG;
        }
        moonEq.ra = WrapTwoPi(moonEq.ra);
    }

    out.moon.rightAscensionRad = moonEq.ra;
    out.moon.declinationRad = moonEq.dec;
    {
        const double ha = lstRad - moonEq.ra;
        EquatorialToHorizon(ha, moonEq.dec, latRad, out.moon.azimuthRad, out.moon.altitudeRad);
    }

    // Apparent angular radius from the topocentric-ish geocentric distance. The
    // difference between geocentric and topocentric distance is at most 1.7% (one Earth
    // radius out of 60), i.e. under a tenth of the disc's own size variation.
    out.moonAngularRadiusRad = std::asin(std::fmax(-1.0, std::fmin(1.0, kMoonRadiusKm / out.moon.distanceKm)));

    return out;
}

} // namespace Poseidon::astro
