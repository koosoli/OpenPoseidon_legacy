
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Ephemeris.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

using namespace Poseidon;
namespace Poseidon
{

static const Color BackgroundColor(0.55, 0.6, 0.8);
static const Color FullSunColor(0.85, 0.75, 0.4);

static const Color SunsetColor(0.8, 0.30, 0.23); // color of sun light before sunset
static const Color MoonColor(HBlack);

static const Color SunsetSkyColor(1.0, 0.2, 0.1);        // color of sky around sun before sunset
static const Color SunsetObjectColor(1.0, 0.8, 0.1);     // color of sun before sunset
static const Color SunsetHaloObjectColor(0.9, 0.4, 0.2); // color of sun helo before sunset
static const Color SunObjectColor(1, 1, 0.9);            // color of full moon
static const Color SunHaloObjectColor(0.9, 0.9, 0.7);    // color of full sun halo

static const Color MoonObjectColor(0.9, 0.9, 1.0, 0.7);      // color of full moon
static const Color MoonHaloObjectColor(0.9, 0.9, 1.0, 0.05); // color of full moon halo
static const Color MoonsetObjectColor(0.9, 0.75, 0.4);       // color of setting moon
static const Color MoonsetHaloObjectColor(0.9, 0.5, 0.2);    // color of setting moon halo

LightSun::LightSun()
{
    _direction = VForward;
    _shadowDirection = VForward;
    _sunDirection = VForward;
    _moonDirection = VForward;
    _moonDirectionUp = VUp;
    _starsOrientation = M3Identity;

    _moonPhase = 0;
    _nightEffect = 0;
    _starsVisible = 0;

    _moonSunDirection = VUp;
    _moonIllumination = 0;
    _moonBrightness = 0;
    _moonAngularRadius = 0.00452f; // mean apparent radius, ~0.259 deg
    _moonLightAmount = 0;

    _colorFull = FullSunColor;
    _diffuse = _colorFull; // default - full sun
    _diffuse.SaturateMinMax();
    _ambient = BackgroundColor;
    _sunColor = _colorFull;
    _sunObjectColor = ::SunObjectColor;
    _sunHaloObjectColor = ::SunHaloObjectColor;
    _moonObjectColor = ::MoonObjectColor;
    _moonHaloObjectColor = ::MoonHaloObjectColor;
    _skyColor = BackgroundColor + _colorFull * 0.5f;
    _sunSkyColor = _colorFull;
    _ambientPrecalc = _ambient;
    _diffusePrecalc = _diffuse;
}

Color LightSun::AmbientResult() const
{
    return _ambientPrecalc;
}

Color LightSun::FullResult(float diffuse) const
{
    return _diffusePrecalc * diffuse + _ambientPrecalc;
}

#define MIN_BACK_INTENSITY 0.05
#define MIN_SKY_INTENSITY 0.03

const float sunSunset = 20 * (H_PI / 180); // sunset object limit
const float begSunset = 25 * (H_PI / 180); // sunset light
const float endSunset = 10 * (H_PI / 180); // UHEL_VYCHODU sunrise angle range
const float nightAngle = 5 * (H_PI / 180); // UHEL_NEF night effect angle

const float sinSunSunset = sin(sunSunset), invSinSunSunset = 1 / sinSunSunset;
const float sinBegSunset = sin(begSunset), invSinBegSunset = 1 / sinBegSunset;
const float sinEndSunset = sin(endSunset), invSinEndSunset = 1 / sinEndSunset;
const float sinNightAngle = sin(nightAngle), invSinNightAngle = 1 / sinNightAngle;

const float sunsetRamp = 0.7; // SUNSET_RAMP

inline float ConvertSunAngle(float sunAngle)
{
    return AngleDifference(H_PI, sunAngle);
}

// ---------------------------------------------------------------------------------
// Celestial mechanics: the real ephemeris, its dev overrides, and the env-var
// ablation surface. See Ephemeris.hpp for what the 2001 model got wrong and why.
// ---------------------------------------------------------------------------------

// Defined in Foundation/Platform/Globals.cpp alongside Clock::FormatDate; the month
// walk below has to agree with FormatDate exactly, or the date the ephemeris uses and
// the date the UI prints diverge.
int GetDaysInMonth(int year, int month);

namespace
{

// Colour of moonlight as a DIRECTIONAL term. Real moonlight is very slightly warmer
// than sunlight (the regolith is grey-brown and it is reflected sunlight), and looks
// blue only because scotopic vision is blue-shifted — the Purkinje effect. This
// leans blue on purpose: the eye's own response is not modelled anywhere downstream,
// so the shift has to live in the light.
const Color MoonLightColor(0.62f, 0.68f, 0.85f);

// Peak DIFFUSE contribution of a full moon on the legacy (GL33 / non-sky-lit) path.
// The engine's night ambient floor is BackgroundColor * MIN_BACK_INTENSITY, i.e. about
// 0.03, so a directional term of this size reads as "the moon is lighting this" without
// turning night into a blue day. The physically correct number is ~2.5e-6 of the sun
// and renders as black.
const float MoonFullDiffuse = 0.075f;

// Ramps, in sine-of-elevation (the engine's own currency for sun height).
// Moonlight only exists once the sun is genuinely down...
const float MoonSunDownHigh = sinf(-2.0f * (float)H_PI / 180.0f); // no moonlight above this
const float MoonSunDownLow = sinf(-8.0f * (float)H_PI / 180.0f);  // full moonlight below this
// ...and only while the moon is actually up. A moon within a degree of the horizon is
// heavily extincted and casts nothing useful, so the ramp starts below zero and
// finishes a few degrees up rather than switching at the geometric horizon.
const float MoonUpLow = sinf(-1.0f * (float)H_PI / 180.0f);
const float MoonUpHigh = sinf(4.0f * (float)H_PI / 180.0f);

// Engine world frame: X = east, Y = up, Z = north. Azimuth is measured from north
// toward east. Verified against the legacy orbital model, which agrees with the
// ephemeris on the sun's azimuth to within a few degrees all year.
Vector3 HorizonToDirection(double azimuthRad, double altitudeRad)
{
    const double ca = cos(altitudeRad);
    return Vector3(static_cast<float>(ca * sin(azimuthRad)), static_cast<float>(sin(altitudeRad)),
                   static_cast<float>(ca * cos(azimuthRad)));
}

float Saturate01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float RampUp(float v, float lo, float hi)
{
    if (hi <= lo)
    {
        return v >= hi ? 1.0f : 0.0f;
    }
    const float t = Saturate01((v - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t); // smoothstep — no kink where the light appears
}

// WGR_* ablation surface, parsed once. This exists so a capture command line can turn
// the whole feature off (or pin a phase) without the dev panel:
//
//   WGR_MOON=0            legacy 2001 orbit AND no moonlight — the pre-change behaviour
//   WGR_MOON=1            force the ephemeris moon on
//   WGR_MOON_LIGHT=0|1    moonlight only (position still realistic)
//   WGR_MOON_SUN=1        put the SUN on the ephemeris too (moves daylight; see below)
//   WGR_MOON_DATE=Y-M-D   celestial date override, e.g. 1985-9-28
//   WGR_MOON_LAT=<deg>    observer latitude override, GEOGRAPHIC, north positive
//   WGR_MOON_AZEL=az,el   hand-place the moon (degrees; azimuth from north toward east)
//   WGR_MOON_SIZE=<k>     multiply the true angular radius
//   WGR_MOON_BRIGHT=<k>   multiply disc + moonlight
//   WGR_MOON_PHASE=<k>    force the illuminated fraction, 0 (new) .. 1 (full)
struct MoonEnv
{
    int enable = -1;    // -1 = unset
    int light = -1;
    int sun = -1;
    bool date = false;
    int year = 0, month = 1, day = 1;
    bool latitude = false;
    float latitudeDeg = 0.0f;
    bool azel = false;
    float azimuthDeg = 0.0f, elevationDeg = 0.0f;
    float size = -1.0f;
    float bright = -1.0f;
    float phase = -1.0f;
};

int EnvBool(const char* name)
{
    const char* v = std::getenv(name);
    if (!v || !*v)
    {
        return -1;
    }
    return (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0) ? 0 : 1;
}

const MoonEnv& GetMoonEnv()
{
    static MoonEnv env = []
    {
        MoonEnv e;
        e.enable = EnvBool("WGR_MOON");
        e.light = EnvBool("WGR_MOON_LIGHT");
        e.sun = EnvBool("WGR_MOON_SUN");
        if (const char* d = std::getenv("WGR_MOON_DATE"))
        {
            int y = 0, m = 0, dd = 0;
            if (std::sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3 && m >= 1 && m <= 12 && dd >= 1 && dd <= 31)
            {
                e.date = true;
                e.year = y;
                e.month = m;
                e.day = dd;
            }
        }
        if (const char* l = std::getenv("WGR_MOON_LAT"))
        {
            e.latitude = std::sscanf(l, "%f", &e.latitudeDeg) == 1;
        }
        if (const char* a = std::getenv("WGR_MOON_AZEL"))
        {
            e.azel = std::sscanf(a, "%f,%f", &e.azimuthDeg, &e.elevationDeg) == 2;
        }
        if (const char* s = std::getenv("WGR_MOON_SIZE"))
        {
            if (std::sscanf(s, "%f", &e.size) != 1)
            {
                e.size = -1.0f;
            }
        }
        if (const char* b = std::getenv("WGR_MOON_BRIGHT"))
        {
            if (std::sscanf(b, "%f", &e.bright) != 1)
            {
                e.bright = -1.0f;
            }
        }
        if (const char* p = std::getenv("WGR_MOON_PHASE"))
        {
            if (std::sscanf(p, "%f", &e.phase) != 1)
            {
                e.phase = -1.0f;
            }
        }
        return e;
    }();
    return env;
}

// The dev-panel knobs (Engine::SkySettings) with the env overrides folded in. GEngine
// is null in headless tools and on the dedicated server, where the defaults apply.
Engine::SkySettings ResolveCelestialSettings()
{
    Engine::SkySettings s = GEngine ? GEngine->GetSkySettings() : Engine::SkySettings{};
    const MoonEnv& e = GetMoonEnv();
    if (e.enable == 0)
    {
        s.moonRealistic = false;
        s.moonLighting = false;
        s.sunRealistic = false;
    }
    else if (e.enable == 1)
    {
        s.moonRealistic = true;
    }
    if (e.light >= 0)
    {
        s.moonLighting = e.light != 0;
    }
    if (e.sun >= 0)
    {
        s.sunRealistic = e.sun != 0;
    }
    if (e.date)
    {
        s.moonDateOverride = true;
        s.moonDateYear = e.year;
        s.moonDateMonth = e.month;
        s.moonDateDay = e.day;
    }
    if (e.latitude)
    {
        s.moonLatitudeOverride = true;
        s.moonLatitudeDeg = e.latitudeDeg;
    }
    if (e.azel)
    {
        s.moonManual = true;
        s.moonManualAzimuthDeg = e.azimuthDeg;
        s.moonManualElevationDeg = e.elevationDeg;
    }
    if (e.size > 0.0f)
    {
        s.moonSizeScale = e.size;
    }
    if (e.bright >= 0.0f)
    {
        s.moonBrightnessScale = e.bright;
    }
    if (e.phase >= 0.0f)
    {
        s.moonPhaseOverride = e.phase;
    }
    return s;
}

// The SIMULATION clock's calendar date and local time-of-day. Deliberately not
// wall-clock: in multiplayer every client advances Glob.clock from the replicated
// mission start date and the shared simulation delta, so a sky derived from it is the
// same sky on every machine. Reading the host's local time here would desync it.
void CelestialDateTime(const Engine::SkySettings& s, int& year, int& month, int& day, double& hoursLocal)
{
    const float timeOfDay = Glob.clock.GetTimeOfDay();
    hoursLocal = static_cast<double>(timeOfDay) * 24.0;

    if (s.moonDateOverride)
    {
        year = s.moonDateYear;
        month = s.moonDateMonth;
        day = s.moonDateDay;
        return;
    }

    year = Glob.clock.GetYear();
    // Mirrors Clock::FormatDate: the engine's year is a fixed 365 grid, and the day is
    // the floor of the year fraction (plus the roll-over FormatDate applies).
    int dayOfYear = static_cast<int>(std::floor(Glob.clock.GetTimeInYear() * 365.0f));
    if (timeOfDay > 1.0f)
    {
        dayOfYear++;
    }
    if (dayOfYear < 0)
    {
        dayOfYear = 0;
    }
    if (dayOfYear > 364)
    {
        dayOfYear = 364;
    }
    int m = 0;
    while (m < 11 && dayOfYear >= GetDaysInMonth(year, m))
    {
        dayOfYear -= GetDaysInMonth(year, m);
        m++;
    }
    month = m + 1;
    day = dayOfYear + 1;
}

} // namespace

// create a coordinate system simulating Earth and Moon movement relative to sun
void LightSun::Recalculate(World* world)
{
    if (!world)
    {
        world = GWorld;
    }
    float latitudeCoord = world ? world->GetLatitude() : -40 * H_PI / 180;
    // Longitude was loaded from CfgWorlds and then never read by anything — the legacy
    // model has no concept of it. The ephemeris uses it to convert the clock's LOCAL
    // mean solar time to UT (no civil time zone, so 12:00 is still local noon).
    float longitudeCoord = world ? world->GetLongitude() : +15 * H_PI / 180;
    static const Matrix3 moonOrbitAngle(MRotationZ, 5 * H_PI / 180);
    static const Matrix3 earthAxis = Matrix3(MRotationX, 23 * H_PI / 180);
    Matrix3 latitude(MRotationX, latitudeCoord); // -40 - Croatia, -90 - north pole

    const float initMoonOnOrbitPos = 0.5;
    const float day = 1.0 / 365;
    const float lunarMonth = 28 * day;

    float timeInYear = Glob.clock.GetTimeInYear();
    float timeOfDay = Glob.clock.GetTimeOfDay();
    float moonOnOrbitPos = initMoonOnOrbitPos + timeInYear * (1.0 / lunarMonth);
    Matrix3Val moonOnOrbit = moonOrbitAngle * Matrix3(MRotationY, moonOnOrbitPos * (H_PI * 2));
    Matrix3Val earthOnOrbit = Matrix3(MRotationY, timeInYear * (H_PI * 2));
    // note - midnight is on the point furthest from the sun
    Matrix3Val midnightToCurrent = Matrix3(MRotationY, timeOfDay * (H_PI * 2));
    // calculate sun and moon position relative to current postion
    Matrix3Val cameraToCosmos = earthAxis * midnightToCurrent * earthOnOrbit * latitude;
    Matrix3Val cameraToStars = midnightToCurrent * earthOnOrbit * latitude;
    Matrix3Val cosmosToCamera = cameraToCosmos.InverseRotation();
    Matrix3Val starsToCamera = cameraToStars.InverseRotation();
    // use rotation of PI/2 to achieve this
    static const Matrix3 normalDirection(MRotationX, -H_PI / 2);
    Matrix3Val convert = normalDirection * cosmosToCamera;
    _direction = convert * earthOnOrbit.Direction();
    _moonDirection = convert * moonOnOrbit.Direction();

    _starsOrientation = normalDirection * starsToCamera;

    // reverse N-S, W-W
    _direction[0] = -_direction[0];
    _direction[2] = -_direction[2];
    _moonDirection[0] = -_moonDirection[0];
    _moonDirection[2] = -_moonDirection[2];
    _starsOrientation(0, 0) = -_starsOrientation(0, 0);
    _starsOrientation(2, 0) = -_starsOrientation(2, 0);
    _starsOrientation(0, 1) = -_starsOrientation(0, 1);
    _starsOrientation(2, 1) = -_starsOrientation(2, 1);
    _starsOrientation(0, 2) = -_starsOrientation(0, 2);
    _starsOrientation(2, 2) = -_starsOrientation(2, 2);

    // ---- Real ephemeris ---------------------------------------------------------
    // Everything above this line is the 2001 toy model, kept because it is still the
    // SUN's default (see Engine::SkySettings::sunRealistic) and the fallback when the
    // ephemeris is switched off. Below, the real positions overwrite it.
    const Engine::SkySettings celestial = ResolveCelestialSettings();

    // Legacy phase, computed before any overwrite so the fallback path is unchanged.
    float cosMoonPhase = -(_direction * _moonDirection);
    _moonPhase = 0.5 + acos(floatMin(floatMax(cosMoonPhase, -1.0f), 1.0f)) / (2 * H_PI);
    _moonIllumination = Saturate01((cosMoonPhase + 1.0f) * 0.5f);
    // cosMoonPhase is +1 at full and -1 at new, so acos of it IS the phase angle the
    // brightness curve wants (0 = full, 180 = new).
    _moonBrightness = static_cast<float>(astro::MoonPhaseBrightness(
        acos(floatMin(floatMax(cosMoonPhase, -1.0f), 1.0f)) * (180.0 / H_PI)));
    _moonAngularRadius = 0.00452f;
    // Fallback terminator reference: the legacy sun. `_direction` is a light-TRAVEL
    // vector, so the direction to the sun is its negation.
    Vector3 sunToDirection = -_direction;

    if (celestial.moonRealistic || celestial.sunRealistic)
    {
        double latitudeRad = astro::GeographicLatitudeFromWorld(static_cast<double>(latitudeCoord));
        if (celestial.moonLatitudeOverride)
        {
            latitudeRad = static_cast<double>(celestial.moonLatitudeDeg) * (H_PI / 180.0);
        }
        int year = 1985, month = 1, day = 1;
        double hoursLocal = 12.0;
        CelestialDateTime(celestial, year, month, day, hoursLocal);

        const double jd =
            astro::JulianDayFromLocal(year, month, day, hoursLocal, static_cast<double>(longitudeCoord));
        const astro::SkyState sky =
            astro::Compute(jd, astro::GeoObserver{latitudeRad, static_cast<double>(longitudeCoord)});

        // The terminator's orientation always comes from the REAL sun, even when the
        // drawn/lighting sun is the legacy one: a moon whose phase is right but whose
        // terminator is tilted wrong looks worse than a moon with no phase at all.
        sunToDirection = HorizonToDirection(sky.sun.azimuthRad, sky.sun.altitudeRad);

        if (celestial.sunRealistic)
        {
            _direction = -sunToDirection;
        }
        if (celestial.moonRealistic)
        {
            _moonDirection = -HorizonToDirection(sky.moon.azimuthRad, sky.moon.altitudeRad);
            _moonPhase = static_cast<float>(sky.moonAgeFraction);
            _moonIllumination = static_cast<float>(sky.moonIlluminatedFraction);
            _moonBrightness = static_cast<float>(sky.moonRelativeBrightness);
            _moonAngularRadius = static_cast<float>(sky.moonAngularRadiusRad);
        }
    }

    // Hand-placed moon (screenshots / isolating a phase). Position only — the phase
    // and brightness stay whatever the astronomy or the phase override says, so a
    // manual placement does not silently also change the look.
    if (celestial.moonManual)
    {
        _moonDirection = -HorizonToDirection(static_cast<double>(celestial.moonManualAzimuthDeg) * (H_PI / 180.0),
                                             static_cast<double>(celestial.moonManualElevationDeg) * (H_PI / 180.0));
    }
    if (celestial.moonPhaseOverride >= 0.0f)
    {
        _moonIllumination = Saturate01(celestial.moonPhaseOverride);
        // Invert k = (1 + cos a)/2 to recover the phase angle the brightness curve wants.
        const double alphaDeg = acos(floatMin(floatMax(2.0f * _moonIllumination - 1.0f, -1.0f), 1.0f)) * (180.0 / H_PI);
        _moonBrightness = static_cast<float>(astro::MoonPhaseBrightness(alphaDeg));
        // Keep the legacy p3d texture animation consistent with the forced phase. Age
        // runs 0 = new .. 0.5 = full, so the WAXING half is 0.5 - alpha/360 (an
        // arbitrary but harmless choice: a forced phase says nothing about which way
        // the lunation is going).
        _moonPhase = static_cast<float>(0.5 - alphaDeg / 360.0);
    }
    _moonAngularRadius *= floatMax(celestial.moonSizeScale, 0.01f);
    _moonSunDirection = sunToDirection;
    _moonSunDirection.Normalize();

    // Orient the moon model so its lit side faces the sun. Same construction the 2001
    // code used, but referred to the ephemeris sun.
    _moonDirectionUp = _moonDirection + _moonSunDirection;
    if (_moonDirectionUp.SquareSize() < 1e-6f)
    {
        // Sun exactly behind the moon (new or full): the "up" is degenerate and the
        // terminator is undefined anyway. Any stable vector will do.
        _moonDirectionUp = VUp;
    }

    // Halo/disc alpha on the legacy skydome path. (cosMoonPhase + 1) * 0.5 was always
    // the illuminated fraction in disguise, so both now read it directly and stay
    // correct when the ephemeris is driving.
    float moonHaloIntensity = _moonIllumination;
    float moonIntensity = floatMin(_moonIllumination * 2.0f, 1.0f);

    float sinSun = -_direction.Y();
    float absSinSun = fabs(sinSun);

    if (sinSun < 0)
    {
        // night, early morning or late evening
        if (absSinSun > sinEndSunset)
        {
            _sunSkyColor = Color(HBlack);
            _colorFull = MoonColor;
        }
        else
        {
            // sunset or sunrise
            // interpolate between moon and sunset colors
            float sunset = 1 - absSinSun * invSinEndSunset;
            _colorFull = MoonColor + (SunsetColor - MoonColor) * sunset;
            _sunSkyColor = SunsetSkyColor * sunset;
        }
        if (absSinSun > sinEndSunset)
        {
            _starsVisible = 1;
        }
        else
        {
            _starsVisible = absSinSun * invSinEndSunset;
        }
        _nightEffect = 1;
        _sunColor = SunsetColor;
        _sunObjectColor = SunsetObjectColor;
        _sunHaloObjectColor = SunsetHaloObjectColor;
    }
    else
    {
        // day
        if (absSinSun > sinBegSunset)
        {
            _colorFull = FullSunColor;
            _sunSkyColor = _colorFull;
        }
        else
        {
            // evening or morning
            float sunset = 1 - absSinSun * invSinBegSunset;
            _colorFull = FullSunColor + (SunsetColor - FullSunColor) * sunset;
            _sunSkyColor = _colorFull + (SunsetSkyColor - _colorFull) * sunset;
        }

        if (absSinSun > sinSunSunset)
        {
            _sunObjectColor = ::SunObjectColor;
            _sunHaloObjectColor = ::SunHaloObjectColor;
        }
        else
        {
            float sunset = 1 - absSinSun * invSinSunSunset;
            _sunHaloObjectColor = ::SunHaloObjectColor + (::SunsetHaloObjectColor - ::SunHaloObjectColor) * sunset;
            _sunObjectColor = ::SunObjectColor + (::SunsetObjectColor - ::SunObjectColor) * sunset;
        }

        if (absSinSun > sinNightAngle)
        {
            _nightEffect = 0;
        }
        else
        {
            _nightEffect = 1 - absSinSun * invSinNightAngle;
        }
        _starsVisible = 0;

        _sunColor = _colorFull;
    }

    float sinMoon = -_moonDirection.Y();
    if (sinMoon < 0) // moon below horizont
    {
        _moonObjectColor = ::MoonsetObjectColor;
        _moonHaloObjectColor = ::MoonsetHaloObjectColor;
    }
    else
    {
        if (sinMoon > sinSunSunset)
        {
            _moonObjectColor = ::MoonObjectColor;
            _moonHaloObjectColor = ::MoonHaloObjectColor;
        }
        else
        {
            float sunset = 1 - sinMoon * invSinSunSunset;
            _moonObjectColor = ::MoonHaloObjectColor + (::MoonsetHaloObjectColor - ::MoonHaloObjectColor) * sunset;
            _moonHaloObjectColor = ::MoonObjectColor + (::MoonsetObjectColor - ::MoonObjectColor) * sunset;
        }
    }
    _moonObjectColor.SetA(::MoonObjectColor.A() * moonIntensity);
    _moonHaloObjectColor.SetA(::MoonHaloObjectColor.A() * moonHaloIntensity);

    // ---- Moonlight --------------------------------------------------------------
    // Before this, the moon lit NOTHING: at night _colorFull was set to MoonColor,
    // which is HBlack, so the scene's only night illumination was the flat ambient
    // floor. The moon was scenery.
    //
    // Two geometric gates, then the phase. `moonGeometry` is deliberately independent
    // of brightness: it answers "is the moon the thing lighting this scene right now",
    // and is what steers the shading/shadow direction. Scaling the DIRECTION blend by
    // phase would leave a crescent night lit from halfway between a black sun and the
    // moon, which is a direction nothing is in.
    float moonGeometry = 0.0f;
    if (celestial.moonLighting)
    {
        const float sunDown = RampUp(-sinSun, -MoonSunDownHigh, -MoonSunDownLow);
        const float moonUp = RampUp(sinMoon, MoonUpLow, MoonUpHigh);
        moonGeometry = sunDown * moonUp;
    }
    _moonLightAmount = moonGeometry * _moonBrightness * floatMax(celestial.moonBrightnessScale, 0.0f);
    if (_moonLightAmount > 0.0f)
    {
        // Legacy / non-sky-lit path: the moon becomes the directional light's colour.
        // (The wgpu HDR sky-lit path derives its sun radiance from the atmosphere
        // instead and adds its own moon term — see EngineWgpu::PushFrame.)
        _colorFull = _colorFull + MoonLightColor * (MoonFullDiffuse * _moonLightAmount);
    }

    float ambientI = sinSun * 1.5;
    float backgroundI = sinSun * 2.0;
    saturate(ambientI, MIN_BACK_INTENSITY, 1);
    saturate(backgroundI, MIN_SKY_INTENSITY, 1);
    _ambient = BackgroundColor * ambientI;
    _skyColor = BackgroundColor * backgroundI + _colorFull * 0.5;

    // enable lights when night effects are on

    // consider how much sun is visible
    _sunColor.SaturateMinMax();
    _skyColor.SaturateMinMax();
    _sunSkyColor.SaturateMinMax();
    _ambient.SaturateMinMax();
    _direction.Normalize();

    // _sunDirection is the ASTRONOMICAL sun and is taken before the moonlight swap
    // below: the sky pass positions its sun disc from it, and the atmosphere's
    // transmittance lookup uses it to decide the scene is in night. _direction is the
    // SHADING/shadow direction, which at night is the moon.
    _sunDirection = _direction;

    if (moonGeometry > 0.0f)
    {
        Vector3 moonTravel = _moonDirection;
        moonTravel.Normalize();
        _direction = _direction * (1.0f - moonGeometry) + moonTravel * moonGeometry;
        if (_direction.SquareSize() < 1e-6f)
        {
            // Moon exactly opposite the (already-set) sun travel vector; take the moon.
            _direction = moonTravel;
        }
        _direction.Normalize();
    }

    _shadowDirection = _direction;

    const float maxShadowDer = -0.2;
    if (_shadowDirection[1] > maxShadowDer)
    {
        _shadowDirection[1] = maxShadowDer;
        _shadowDirection.Normalize();
    }
}

// additional lights
Light::Light()
{
    _on = true;
}

Light::~Light()
{
    // unregister light with D3D
}

int Light::Compare(const Light& with, const LightContext& context) const
{
    // compare distance relative to given context
    Vector3Val camPos = context.position;
    Vector3 relThisPos = camPos - Position();
    Vector3 relWithPos = camPos - with.Position();
    float distThis2 = relThisPos.SquareSize();
    float distWith2 = relWithPos.SquareSize();
    float diff = distWith2 * SortBrightness() - distThis2 * with.SortBrightness();
    if (diff > 0)
    {
        return +1;
    }
    if (diff < 0)
    {
        return -1;
    }
    return 0;
}

int Light::Compare(const Light& with) const
{
    // compare distance relative to global camera
    Vector3Val camPos = GLOB_SCENE->GetCamera()->Position();
    Vector3 relThisPos = camPos - Position();
    Vector3 relWithPos = camPos - with.Position();
    float distThis2 = relThisPos.SquareSize();
    float distWith2 = relWithPos.SquareSize();
    float diff = distWith2 * SortBrightness() - distThis2 * with.SortBrightness();
    if (diff > 0)
    {
        return +1;
    }
    if (diff < 0)
    {
        return -1;
    }
    return 0;
}

float Light::SortBrightness() const
{
    return Brightness();
}

bool Light::Visible(const Object* obj) const
{
    // default implementation: check only distance
    Vector3Val position = obj->Position() - Position();
    return Brightness() > 0.1 * SquareDistance(position);
}

LightPositioned::LightPositioned() = default;

void LightPositioned::Prepare(const Matrix4& worldToModel)
{
    _modelPos = worldToModel.FastTransform(Position());
    _modelDir = worldToModel.Rotate(Direction());
    _modelDir.Normalize();
}

LightPositionedColored::LightPositionedColored() : _ambient(HWhite), _diffuse(HWhite) {}

LightPositionedColored::LightPositionedColored(ColorVal diffuse, ColorVal ambient)
    : _diffuse(diffuse), _ambient(ambient)
{
}

void LightSun::SetMaterial(const TLMaterial& mat)
{
    _ambientPrecalc = _ambient * mat.ambient + _diffuse * mat.forcedDiffuse;
    _diffusePrecalc = _diffuse * mat.diffuse;
}

void LightSun::GetDescription(LightDescription& desc) const
{
    // used for HW T&L
    desc.type = LTDirectional;
    desc.dir = Direction();
    desc.pos = VZero;       // ignored for directional
    desc.startAtten = 1e10; // ignored for directional
    desc.ambient = Ambient();
    desc.diffuse = Diffuse();
    desc.phi = 0;
    desc.theta = 0;
}

void LightPositionedColored::SetMaterial(const TLMaterial& mat)
{
    _ambientPrecalc = _ambient * mat.ambient + _diffuse * mat.forcedDiffuse;
    _diffusePrecalc = _diffuse * mat.diffuse;
}

LightPoint::LightPoint(ColorVal color, ColorVal ambient) : base(color, ambient), _startAtten(50) {}

LightPoint::LightPoint() = default;

float LightPoint::FlareIntensity(Vector3Par camPos, Vector3Par camDir) const
{
    Vector3Val relPos = camPos - Position();
    // calculate surface lighting factor
    float startAtten = Square(_startAtten);
    float endAtten = startAtten * 100;
    float size2 = relPos.SquareSize();
    if (size2 >= endAtten)
    {
        return 0;
    }
    float invSize = InvSqrt(size2);
    float atten = 1;
    float cosFi = -camDir * relPos * invSize;
    if (size2 >= startAtten)
    {
        atten = startAtten * invSize * invSize;
    }
    return atten * cosFi;
}

Color LightPoint::Apply(Vector3Par point, Vector3Par normal)
{
    // normal and point is given in model space
    // calculate distance from pointlight
    Vector3Val relPos = point - _modelPos;
    // calculate surface lighting factor
    float startAtten = Square(_startAtten);
    float endAtten = startAtten * 100;
    float size2 = relPos.SquareSize();
    if (size2 >= endAtten)
    {
        return Color(HBlack);
    }
    float invSize = InvSqrt(size2);
    float atten = 1;
    if (size2 >= startAtten)
    {
        atten = startAtten * invSize * invSize;
    }
    // not cosFi is actualy cosFi*size
    float cosFi = relPos * normal;
    if (cosFi > 0)
    {
        cosFi *= invSize;
        return (_diffusePrecalc * cosFi + _ambientPrecalc) * atten;
    }
    else
    {
        return _ambientPrecalc * atten;
    }
}

void LightPoint::GetDescription(LightDescription& desc) const
{
    // used for HW T&L
    desc.type = LTPoint;
    desc.dir = Direction();
    desc.pos = Position();         // ignored for directional
    desc.startAtten = _startAtten; // ignored for directional
    desc.ambient = Ambient();
    desc.diffuse = GetDiffuse();
    desc.phi = 0;
    desc.theta = 0;
}

LightReflector::LightReflector(LODShapeWithShadow* shape, ColorVal color, ColorVal ambient, float angle, float size)
    : _shape(shape), base(color, ambient), _angle(angle), _startAtten(200), _size(size)
{
}

bool LightReflector::Visible(const Object* obj) const
{
    // default implementation: check only distance
    float dist2 = obj->Position().Distance2(Position());
    return Brightness() > 0.1 * dist2;
}

#define MIN_INSIDE 0.97814760073 // 12 degree
#define MAX_INSIDE 0.99026806874 // 8 degree

float LightReflector::FlareIntensity(Vector3Par camPos, Vector3Par camDir) const
{
    // flare only if camera is in light cone
    // check distance
    // check "inside cone" value
    Vector3Val relPos = camPos - Position();
    float inside = relPos * Direction();
    if (inside <= 0)
    {
        return 0;
    }
    float size2 = relPos.SquareSize();

    float startAtten = Square(_startAtten);
    float endAtten = startAtten * 100;
    if (size2 >= endAtten)
    {
        return 0;
    }

    float minInside2 = size2 * (MIN_INSIDE * MIN_INSIDE);
    float inside2 = inside * inside;
    if (inside2 < minInside2)
    {
        return 0;
    }

    float atten = 1;
    float invSize = InvSqrt(size2);
    if (size2 >= startAtten)
    {
        atten = startAtten * invSize * invSize;
    }
    float cosFi = -camDir * relPos * invSize;
    if (cosFi > 0)
    {
        float maxInside2 = size2 * (MAX_INSIDE * MAX_INSIDE);
        // note: distance normalization is VERY slow
        // it takes usually one division and one square root
        // we need some approximation for this
        cosFi *= invSize;
        if (inside2 > maxInside2)
        {
            inside = 1;
        }
        else
        {
            inside = (inside2 - minInside2) * (1 / (maxInside2 - minInside2));
        }
        atten *= inside;
        return atten * cosFi;
    }
    else
    {
        return 0;
    }
}

Color LightReflector::Apply(Vector3Par point, Vector3Par normal)
{
    // calculate distance from pointlight
    Vector3Val relPos = point - _modelPos;
    // calculate surface lighting factor
    float startAtten = Square(_startAtten);
    float endAtten = startAtten * 100;
    float size2 = relPos.SquareSize();
    if (size2 >= endAtten)
    {
        return Color(HBlack);
    }
    // determine if the point is inside the light cone
    // cos(coneangle)=relPosNorm*direction
    // if point is inside, then cos(coneangle)>cos(_angle)
    float inside = relPos * _modelDir;
    if (inside <= 0)
    {
        return Color(HBlack);
    }
    float minInside2 = size2 * (MIN_INSIDE * MIN_INSIDE);
    float inside2 = inside * inside;
    if (inside2 < minInside2)
    {
        return Color(HBlack);
    }
    // not cosFi is actualy cosFi*size
    float cosFi = relPos * normal;
    float atten = 1;
    float invSize = InvSqrt(size2);
    if (size2 >= startAtten)
    {
        atten = startAtten * invSize * invSize;
    }
    if (cosFi > 0)
    {
        float maxInside2 = size2 * (MAX_INSIDE * MAX_INSIDE);
        // note: distance normalization is VERY slow
        // it takes usually one division and one square root
        // we need some approximation for this
        cosFi *= invSize;
        if (inside2 > maxInside2)
        {
            inside = 1;
        }
        else
        {
            inside = (inside2 - minInside2) * (1 / (maxInside2 - minInside2));
        }
        atten *= inside;
        return (_ambientPrecalc + _diffusePrecalc * cosFi) * atten;
    }
    else
    {
        return _ambientPrecalc * atten;
    }
}

void LightReflector::GetDescription(LightDescription& desc) const
{
    // used for HW T&L
    desc.type = LTSpotLight;
    desc.dir = Direction();
    desc.pos = Position();         // ignored for directional
    desc.startAtten = _startAtten; // ignored for directional
    desc.ambient = Ambient();
    desc.diffuse = GetDiffuse();
    desc.phi = H_PI * 0.20;
    desc.theta = H_PI * 0.12;
}

float LightPoint::Brightness() const
{
    return _diffuse.Brightness() * _startAtten * _startAtten;
}

float LightPoint::SortBrightness() const
{
    // point lights have bigger chance of affecting result
    // increase their brightness for sorting purposes
    return Brightness() * 5;
}

Color LightPoint::GetObjectColor() const
{
    return _diffuse;
}

void LightPoint::ToDraw(ClipFlags clipFlags, bool dimmed)
{
    // note: most point lights are invisible
}

void LightPoint::Load(const ParamEntry& cls)
{
    _diffuse = GetColor(cls >> "color");
    _ambient = GetColor(cls >> "ambient");
    float brightness = cls >> "brightness";
    SetBrightness(brightness);
}

float LightReflector::Brightness() const
{
    return _diffuse.Brightness() * _startAtten * _startAtten;
}

void LightReflector::SetBrightness(float coef)
{
    _startAtten = 200 * InvSqrt(_diffuse.Brightness() / coef);
}

Color LightReflector::GetObjectColor() const
{
    return _diffuse;
}

void LightReflector::ToDraw(ClipFlags clipFlags, bool dimmed)
{
    // reflector: draw volumetrical light object
    if (GScene->MainLight()->NightEffect() < 0.01)
    {
        return;
    }
    if (dimmed)
    {
        return;
    }
    Color c = GetObjectColor();
    float invSize = InvSqrt(c.R() * c.R() + c.G() * c.G() + c.B() * c.B());
    c = c * invSize;
    GScene->DrawVolumeLight(_shape, PackedColor(c), *this, _size);
}

LightPointVisible::LightPointVisible() = default;

LightPointVisible::LightPointVisible(LODShapeWithShadow* shape, ColorVal color, ColorVal ambient, float size)
    : LightPoint(color, ambient), _shape(shape), _size(size)
{
}

void LightPointVisible::ToDraw(ClipFlags clipFlags, bool dimmed)
{
    // reflector: draw volumetrical light object
    if (GScene->MainLight()->NightEffect() < 0.01)
    {
        return;
    }
    if (dimmed)
    {
        return;
    }
    Color c = GetObjectColor();
    GScene->DrawVolumeLight(_shape, PackedColor(c), *this, _size);
}

void LightPointVisible::Load(const ParamEntry& cls)
{
    LightPoint::Load(cls);
    RString shapeName = GetShapeName(cls >> "shape");
    _shape = Shapes.New(shapeName, false, false);
    if (!_shape)
        LOG_ERROR(Graphics, "LightPointVisible: shape='{}' failed to load", static_cast<const char*>(shapeName));
    _size = cls >> "size";
}

LightPointOnVehicle::LightPointOnVehicle(LODShapeWithShadow* shape, ColorVal color, ColorVal ambient, Object* vehicle,
                                         Vector3Par position, float size)
    : LightPointVisible(shape, color, ambient, size), AttachedOnVehicle(vehicle, position, Vector3(0, 0, 1))
{
}

LightPointOnVehicle::LightPointOnVehicle(Object* vehicle, Vector3Par position)
    : AttachedOnVehicle(vehicle, position, Vector3(0, 0, 1))
{
}

void LightPointOnVehicle::Load(const ParamEntry& cls)
{
    LightPointVisible::Load(cls);
    Object* obj = AttachedOn();
    LODShape* shape = obj ? obj->GetShape() : nullptr;
    if (shape)
    {
        RString pos = cls >> "position";
        Vector3Val position = shape->MemoryPoint(pos);
        SetAttachedPos(position, Vector3(0, 0, 1));
    }
}
} // namespace Poseidon
