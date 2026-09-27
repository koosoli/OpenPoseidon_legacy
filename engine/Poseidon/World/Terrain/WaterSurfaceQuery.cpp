#include <Poseidon/World/Terrain/WaterSurfaceQuery.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace
{
constexpr float Pi2 = 6.28318530718f;
constexpr float Gravity = 9.81f;
constexpr float WindX = 0.82f;
constexpr float WindZ = 0.57f;
constexpr float WindSpeed = 6.0f;
// Raised from 0.08. The component amplitudes are multiples of this, and at 0.08 they were 13-30 cm
// — a total heave far smaller than the FFT ocean actually being drawn, so a floating unit barely
// moved while visible waves passed through it. 0.22 puts the CPU sea in the same scale bracket as
// the rendered one; it remains a six-component approximation of a spectral field, not a match.
constexpr float SeaState = 0.22f;
constexpr float SpectrumSeed = 1337.0f;
constexpr float WindLength = 0.998498873f;

struct WaveComponent
{
    float directionX;
    float directionZ;
    float length;
    float amplitude;
    float seedPhase;
};

// Long components carry the four renderer cascade scales; the two neighbouring bands
// avoid an overly regular single-direction swell while staying below FFT detail scale.
constexpr WaveComponent Waves[] = {
    {WindX / WindLength, WindZ / WindLength, 48.0f, SeaState * 2.00f, 0.017f},
    {0.570f, 0.822f, 96.0f, SeaState * 1.60f, 0.029f},
    {WindX / WindLength, WindZ / WindLength, 144.0f, SeaState * 2.70f, 0.043f},
    {0.944f, 0.330f, 288.0f, SeaState * 2.10f, 0.071f},
    {WindX / WindLength, WindZ / WindLength, 432.0f, SeaState * 3.30f, 0.113f},
    {0.710f, 0.704f, 1296.0f, SeaState * 3.80f, 0.191f},
};
// TW-WATER — the sea the predictor follows while Tidewater Native draws the water.
//
// The Current OP branch below scales every band by `waveAmp / 0.40`. The Water tab's amplitude
// has defaulted to 1.00 since the renderer's reference re-baseline, so that ratio is 2.5: six
// components of 1.1-2.1 m, a significant wave height near 10 m. Tidewater Native never draws
// from `waveAmp` -- its sea comes from its own spectrum (TidewaterLook::amplitude, the swell
// system and the cascade scale) -- so in that mode a boat was heaving and rolling on a storm that
// is not on screen (Dec, 2026-09-27: PBRs "bouncing off the water and tipping to the side").
//
// In Tidewater mode the bands therefore follow the Tidewater look instead:
//  * amplitude: TidewaterLook::amplitude, referenced to 1.0. At the defaults that is the
//    predictor's tuned reference sea (Hs ~4.1 m), which is also close to the significant height
//    of Tidewater's default spectrum (local 7 m/s + 0.48 swell: Hs ~3.9 m by integrating the
//    JONSWAP systems in water_tw/fft.rs). The swell term scales with the swell energy lane:
//    kTidewaterSwellShare is the swell system's share of that variance at the defaults (m0 ratio
//    4.64, taken from the same integration), so swell 0 leaves only the local wind sea.
//  * wavelength: TidewaterLook::cascadeScale (Tidewater's cascades stretch with it); no
//    sea-state stretch, which belongs to the Current OP spectrum.
//  * shallow water: every band fades with depth exactly as Tidewater's surface does it
//    (WaterSurface.cascadeAttenuation, the same curve tw_breakers_kernel.wgsl uses): a band of
//    length L keeps full height below min(40 m, 0.08 L) and fades to a small floor on the beach.
//    Without this the 400 m and 1300 m bands kept their full height in a metre of water, which
//    is exactly where the boats in a beach landing are.
//
// Still the CPU predictor: deterministic, no GPU texture, the same on a server (where GEngine is
// absent and the reference sea is used, as before). WGR_WATER_BACKEND overrides the Water-tab
// selection here exactly as it does in the renderer.
constexpr float kTidewaterDefaultSwell = 0.48f;
constexpr float kTidewaterSwellShare = 4.64f;

int EffectiveWaterBackend(const Engine::WaterSettings& look)
{
    static const int env = []
    {
        const char* v = std::getenv("WGR_WATER_BACKEND");
        return (v != nullptr && (v[0] == '0' || v[0] == '1') && v[1] == '\0') ? v[0] - '0' : -1;
    }();
    return env >= 0 ? env : look.waterBackend;
}

// TW-WATER W6b — the sea conditions follow the weather, as the drawn sea does.
//
// W3e referenced the predictor to Tidewater's DEFAULT sea (7 m/s over a 120 km fetch, swell 0.48)
// whatever the weather. The renderer does not stop there: with the Water tab's sea conditions on
// "Weather" (the default) it interpolates Tidewater's preset table (AppUI.js SEA, WaterWgpu.cpp
// kTwSea) at the world's mean wind, so a calm morning draws a Calm sea and a storm a Storm sea,
// and a preset (Calm .. Storm) brings its own wind. Boats, amphibians and swimmers kept riding the
// Breezy sea through all of it. The same table and the same wind source are used here:
//  * wind: the world's mean wind (WindModel -- simulation state, closed form in mission time and
//    overcast, identical on every machine), in the renderer's 1 m/s steps; 7 m/s when the sea is
//    not taking its wind from the weather (Water tab, or no wind model yet);
//  * the local wind sea's variance from Tidewater's own JONSWAP parameters (fft.rs `packed`:
//    alpha = 0.076 (gF/U^2)^-0.22, peak omega = 22 (UF/g^2)^-0.33; m0 ~ alpha g^2 / omega_p^4),
//    relative to the 7 m/s / 120 km reference; the swell's variance with its scale, as in W3e.
// Amplitude only: the six bands keep their wavelengths (the cascade scale still stretches them).
struct TidewaterSeaRow
{
    float wind, fetchKm, swell;
};
constexpr TidewaterSeaRow kTidewaterSeaRows[4] = {
    {3.5f, 40.0f, 0.28f},   // Calm
    {7.0f, 120.0f, 0.48f},  // Breezy (Tidewater's defaults, the W3e reference)
    {12.0f, 300.0f, 0.68f}, // Choppy
    {20.0f, 900.0f, 1.0f},  // Storm
};

float TidewaterLocalSeaVariance(float windMs, float fetchKm)
{
    auto m0 = [](float u, float fetch)
    {
        const float f = std::max(fetch, 1.0f) * 1000.0f;
        u = std::max(u, 0.5f);
        const float alpha = 0.076f * std::pow(Gravity * f / (u * u), -0.22f);
        const float wp = 22.0f * std::pow(u * f / (Gravity * Gravity), -0.33f);
        return alpha / (wp * wp * wp * wp);
    };
    return m0(windMs, fetchKm) / m0(7.0f, 120.0f);
}

// Variance multiplier of the whole Tidewater sea against the W3e reference (Breezy).
float TidewaterSeaVarianceScale(const Engine::WaterSettings& look)
{
    const bool weatherWind = look.windFromWeather && GWind.IsActive();
    const float w = weatherWind ? std::round(GWind.Sample().meanSpeed) : 7.0f;
    const int mode = std::clamp(look.tidewater.seaConditions, 0, 5);
    float wind = weatherWind ? w : 12.0f; // the spectrum's wind when no preset brings one
    float fetch = 120.0f;
    float swell = std::max(look.tidewater.swellScale, 0.0f);
    if (mode == 0)
    {
        int i = 0;
        while (i < 2 && w > kTidewaterSeaRows[i + 1].wind)
        {
            ++i;
        }
        const TidewaterSeaRow& a = kTidewaterSeaRows[i];
        const TidewaterSeaRow& b = kTidewaterSeaRows[i + 1];
        const float f = std::clamp((w - a.wind) / (b.wind - a.wind), 0.0f, 1.0f);
        fetch = a.fetchKm + (b.fetchKm - a.fetchKm) * f;
        swell = a.swell + (b.swell - a.swell) * f;
    }
    else if (mode <= 4)
    {
        const TidewaterSeaRow& r = kTidewaterSeaRows[mode - 1];
        wind = r.wind;
        fetch = r.fetchKm;
        swell = r.swell;
    }
    // the reference seas' swell factors (WaterWgpu.cpp, cascadePreset 3 / 4 / 5)
    if (look.cascadePreset == 3)
    {
        swell *= 0.10f;
    }
    else if (look.cascadePreset == 4)
    {
        swell *= 1.90f;
    }
    else if (look.cascadePreset == 5)
    {
        swell *= 1.25f;
    }
    const float local = TidewaterLocalSeaVariance(wind, fetch);
    return (local + kTidewaterSwellShare * swell / kTidewaterDefaultSwell) / (1.0f + kTidewaterSwellShare);
}

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Tidewater's per-cascade shallow-water attenuation for a band of wavelength `length` at `depth`
// metres of water. Tidewater's floors are per cascade (733 / 157 / 33 / 7 m: 0, 0.05, 0.25, 0.5);
// a band takes the floor of the cascade its wavelength falls in.
float ShoalAttenuation(float length, float depth)
{
    const float floorAmount = length >= 400.0f ? 0.0f : length >= 90.0f ? 0.05f : length >= 20.0f ? 0.25f : 0.5f;
    const float fullDepth = std::min(40.0f, length * 0.08f);
    const float full = SmoothStep(0.0f, fullDepth, depth);
    const float floor = floorAmount * SmoothStep(0.0f, 0.6f, depth);
    return floor + (1.0f - floor) * full;
}
} // namespace

WaterSurfaceSample QueryWaterSurface(float worldX, float worldZ, float time, float seaLevel)
{
    return QueryWaterSurfaceScaled(worldX, worldZ, time, seaLevel, 1.0f);
}

WaterSurfaceSample QueryWaterSurfaceScaled(float worldX, float worldZ, float time, float seaLevel, float waveScale)
{
    WaterSurfaceSample sample = {seaLevel, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float slopeX = 0.0f;
    float slopeZ = 0.0f;

    // Track the live renderer sea state so a floating unit rides the ocean that is actually drawn.
    // The renderer amplitude control is referenced to 0.40 (its shipped default), matching
    // kSeaStateReferenceAmplitude in WaterWgpu.cpp, so an untouched Water tab reproduces the
    // previously tuned buoyancy exactly and only moving the slider changes how much a unit heaves.
    float amplitudeScale = 1.0f;
    float speedScale = 1.0f;
    float lengthScale = 1.0f;
    bool shoal = false;
    const Engine::WaterSettings look = GEngine != nullptr ? GEngine->GetWaterSettings() : Engine::WaterSettings{};
    if (GEngine != nullptr && EffectiveWaterBackend(look) == 1)
    {
        // Tidewater Native: follow the Tidewater look and, W6b, its weather-driven sea conditions
        // (see the block comments above). The reference seas' amplitude factor (Sheltered x0.55,
        // Rough x1.60) is the renderer's too.
        float amplitude = std::clamp(look.tidewater.amplitude, 0.0f, 4.0f);
        if (look.cascadePreset == 3)
        {
            amplitude *= 0.55f;
        }
        else if (look.cascadePreset == 5)
        {
            amplitude *= 1.60f;
        }
        amplitudeScale = amplitude * std::sqrt(TidewaterSeaVarianceScale(look));
        lengthScale = std::max(look.tidewater.cascadeScale, 0.01f);
        shoal = true;
    }
    else if (GEngine != nullptr)
    {
        constexpr float referenceAmplitude = 0.40f;
        amplitudeScale = std::clamp(look.waveAmp, 0.0f, 4.0f) / referenceAmplitude;
        speedScale = std::clamp(look.waveSpeed, 0.0f, 4.0f);
        // Sea-state coupling grows the wavelength as lambda ~ amp^0.75 (and never shrinks it), so
        // the CPU bands have to stretch the same way or the heave period drifts out of step with
        // the rendered swell as the sea gets rougher.
        lengthScale = std::max(look.waveScale, 0.01f);
        if (look.seaStateCoupling)
        {
            lengthScale *= std::max(std::pow(amplitudeScale, 0.75f), 1.0f);
        }
    }

    // Water depth for the shallow-water fade (Tidewater mode only). Sea bed from the landscape,
    // which is simulation data, so this stays deterministic and server-side safe.
    float depth = 1.0e4f;
    if (shoal && GLandscape != nullptr)
    {
        depth = std::max(seaLevel - GLandscape->SurfaceY(worldX, worldZ), 0.0f);
    }

    for (const WaveComponent& wave : Waves)
    {
        const float length = std::max(wave.length * lengthScale, 1.0f);
        float amplitude = wave.amplitude * amplitudeScale * std::max(waveScale, 0.0f);
        if (shoal)
        {
            amplitude *= ShoalAttenuation(length, depth);
        }
        const float waveNumber = Pi2 / length;
        // Deep-water dispersion, nudged by the stable renderer wind-speed default.
        const float angularSpeed = std::sqrt(Gravity * waveNumber) * (0.75f + WindSpeed * 0.12f) * speedScale;
        const float phase = waveNumber * (wave.directionX * worldX + wave.directionZ * worldZ) - angularSpeed * time +
                            SpectrumSeed * wave.seedPhase;
        const float sine = std::sin(phase);
        const float cosine = std::cos(phase);
        const float slope = amplitude * waveNumber * cosine;
        // Guard the division: a zero amplitude (calm water) must not produce a NaN drift.
        const float steepness = 0.08f / std::max(waveNumber * amplitude * 6.0f, 1e-4f);

        sample.height += amplitude * sine;
        slopeX += wave.directionX * slope;
        slopeZ += wave.directionZ * slope;
        // The horizontal velocity is the time derivative of a bounded Gerstner drift.
        sample.velocityX += steepness * amplitude * wave.directionX * angularSpeed * sine;
        sample.velocityZ += steepness * amplitude * wave.directionZ * angularSpeed * sine;
    }

    const float normalLength = std::sqrt(slopeX * slopeX + 1.0f + slopeZ * slopeZ);
    sample.normalX = -slopeX / normalLength;
    sample.normalY = 1.0f / normalLength;
    sample.normalZ = -slopeZ / normalLength;
    sample.roughness = std::sqrt(slopeX * slopeX + slopeZ * slopeZ);
    return sample;
}

} // namespace Poseidon
