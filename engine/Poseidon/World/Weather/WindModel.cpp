#include <Poseidon/World/Weather/WindModel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Poseidon
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// One bounded oscillator. `periodSeconds` is a real period, `phaseTurns` a
/// fractional offset in turns. Kept in double so the argument to sin() stays
/// accurate for a mission that has been running for hours — the same reason the
/// cloud wind offset is wrapped in double in EngineWgpu.cpp.
inline float Osc(double timeSeconds, double periodSeconds, double phaseTurns)
{
    return static_cast<float>(std::sin(kTwoPi * (timeSeconds / periodSeconds + phaseTurns)));
}

/// Seed -> a fractional turn in [0, 1). A cheap integer hash (splitmix-style
/// finaliser); no RNG object and no shared stream, so seeding the wind cannot
/// perturb any other random draw in the engine. That matters: the legacy wind
/// walk pulled five values out of GRandGen every 5 seconds, and removing those
/// pulls would have shifted every downstream random result in a classic
/// mission. This model does not touch GRandGen at all, and the legacy walk is
/// deliberately left running so that stream is unchanged.
inline double SeedTurns(uint32_t seed, uint32_t salt)
{
    uint32_t x = seed * 0x9E3779B9u + salt * 0x85EBCA6Bu;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<double>(x) * (1.0 / 4294967296.0);
}

/// Weighted average of three incommensurate oscillators. The weights sum to
/// exactly 1, so the result is provably in [-1, 1] and every consumer can scale
/// it without clamping. Periods are mutually irrational-ish so the composite
/// does not visibly repeat.
inline float Composite(double t, uint32_t seed, uint32_t salt, double p0, double p1, double p2)
{
    return 0.55f * Osc(t, p0, SeedTurns(seed, salt)) + 0.30f * Osc(t, p1, SeedTurns(seed, salt + 1u)) +
           0.15f * Osc(t, p2, SeedTurns(seed, salt + 2u));
}

bool ReadBallisticsEnv()
{
    const char* value = std::getenv("POSEIDON_WIND_BALLISTICS");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool& BallisticsFlag()
{
    static bool flag = ReadBallisticsEnv();
    return flag;
}
} // namespace

WindSample EvaluateWind(const WindConditions& conditions, float overcast, int64_t timeMilliseconds)
{
    const float o = std::clamp(overcast, 0.0f, 1.0f);
    const double t = static_cast<double>(timeMilliseconds) * 0.001;
    const uint32_t seed = conditions.seed;

    // --- mean field -------------------------------------------------------
    // The mean answers to overcast, which is the engine's existing authoritative
    // weather scalar (World::_actualOvercast, already serialized and already
    // driven by the mission's startWeather/forecastWeather). Coupling here is
    // what makes an overcast front actually feel like one: the sea builds, the
    // grass leans harder and the clouds run faster off the same number.
    //
    // The slow terms are minutes-long, so the mean is effectively constant on
    // the timescale any single consumer cares about; that is what lets the ocean
    // spectrum hold still (see the hysteresis rule in WaterWgpu.cpp).
    const float meanSpeedBase = conditions.calmSpeed + conditions.overcastSpeed * o;
    const float speedTrend = Composite(t, seed, 11u, 1290.0, 487.0, 233.0);
    const float meanSpeed = std::max(meanSpeedBase * (1.0f + conditions.meanVariation * speedTrend), 0.0f);

    const float veer = Composite(t, seed, 21u, 903.0, 331.0, 149.0);
    const float meanDirection = conditions.baseDirectionRad + conditions.veerAmplitudeRad * veer;

    // --- gust -------------------------------------------------------------
    // Gust envelope grows with overcast: a clear calm day breathes, a squall
    // slams. Bounded by construction, so speed can never go negative.
    const float gustiness = std::clamp(conditions.gustiness, 0.0f, 1.0f);
    const float gustEnvelope = gustiness * (0.30f + 0.70f * o);
    const float gustSignal = Composite(t, seed, 31u, 41.1, 17.3, 6.7);
    const float gustFraction = gustEnvelope * gustSignal;

    // A gust also shifts the heading — real gusts back and veer, they do not
    // only change strength. Quarter of the speed effect, on the same clock.
    const float directionWobble = 0.25f * gustEnvelope * Composite(t, seed, 41u, 37.7, 13.1, 5.3);

    const float speed = std::max(meanSpeed * (1.0f + gustFraction), 0.0f);
    const float direction = meanDirection + directionWobble;

    WindSample sample;
    sample.velocityX = speed * std::cos(direction);
    sample.velocityZ = speed * std::sin(direction);
    sample.speed = speed;
    sample.directionRad = direction;
    sample.meanSpeed = meanSpeed;
    sample.meanDirectionRad = meanDirection;
    sample.gustFraction = gustFraction;
    return sample;
}

void WindModel::Init(const WindConditions& conditions)
{
    _conditions = conditions;
    _overcast = 0.0f;
    _sample = EvaluateWind(_conditions, 0.0f, 0);
    _active = true;

    // POSEIDON_WIND_OVERRIDE="speed bearingDeg [gustiness]" arms the dev
    // override from the environment, so a scripted capture can hold the air
    // still (or at a fixed speed) without the panel. Same object the Weather
    // tab drives; same caveats: local, unreplicated.
    if (const char* env = std::getenv("POSEIDON_WIND_OVERRIDE"))
    {
        float speed = 0.0f, bearing = 0.0f, gust = 0.0f;
        const int n = std::sscanf(env, "%f %f %f", &speed, &bearing, &gust);
        if (n >= 1)
        {
            WindOverride o;
            o.enabled = true;
            o.speed = speed;
            o.directionRad = (90.0f - bearing) * 3.14159265f / 180.0f;
            o.gustiness = n >= 3 ? gust : 0.0f;
            _override = o;
        }
    }
}

void WindModel::Update(int64_t netTimeMilliseconds, float overcast)
{
    _overcast = std::clamp(overcast, 0.0f, 1.0f);

    if (_override.enabled)
    {
        // Expressed as conditions rather than as a hand-built WindSample, so the
        // override goes through the same evaluator every consumer is calibrated
        // against — a dialled-in gust has the same shape as a real one.
        //
        // Evaluated at overcast 1 because the gust envelope is scaled by
        // overcast (0.30 + 0.70*o); at 1 the envelope is exactly `gustiness`, so
        // the slider means what it says regardless of the mission's weather.
        WindConditions steady;
        steady.baseDirectionRad = _override.directionRad;
        steady.calmSpeed = std::max(_override.speed, 0.0f);
        steady.overcastSpeed = 0.0f;
        steady.gustiness = std::clamp(_override.gustiness, 0.0f, 1.0f);
        steady.veerAmplitudeRad = 0.0f; // the heading you set is the heading you get
        steady.meanVariation = 0.0f;    // ...and likewise the speed
        steady.seed = _conditions.seed;

        _sample = EvaluateWind(steady, 1.0f, netTimeMilliseconds);
    }
    else
    {
        _sample = EvaluateWind(_conditions, _overcast, netTimeMilliseconds);
    }

    _active = true;
}

bool WindModel::BallisticsEnabled()
{
    return BallisticsFlag();
}

void WindModel::SetBallisticsEnabled(bool enabled)
{
    BallisticsFlag() = enabled;
}

WindModel& GWindModel()
{
    static WindModel instance;
    return instance;
}

} // namespace Poseidon
