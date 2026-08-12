#include "WaterWgpu.hpp"

#include "CdlodDriver.hpp"
#include "EngineWgpu.hpp"

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/LandscapeShared.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <cstring> // memcpy — packs the WTR freeze mask into WgrWaterParams.fft_control.z

namespace Poseidon
{
// CDLOD leaf span in terrain texels.  The GPU water mesh is intentionally denser
// (see water/mod.rs) so it can represent smooth FFT displacement inside this leaf.
constexpr int WaterGridN = 32;

static float EnvFloat(const char* name, float fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return fallback;
    }
    return std::strtof(v, nullptr);
}

struct ReferenceWaveMode
{
    float kx;
    float kz;
    float omega;
    float h0Real;
    float h0Imag;
    float displacementScale;
};

static std::array<float, 2> ReferenceHash(uint32_t x, uint32_t y)
{
    uint32_t h = y + 374761393u + x * 3266489917u;
    h = 2246822519u * (h ^ (h >> 15u));
    h = 3266489917u * (h ^ (h >> 13u));
    const uint32_t n = h ^ (h >> 16u);
    constexpr float invMax = 1.0f / 2147483647.0f;
    return {static_cast<float>((n >> 1u) & 0x7fffffffu) * invMax,
            static_cast<float>(((n * 48271u) >> 1u) & 0x7fffffffu) * invMax};
}

static float ReferenceSpreadNormalization(float s)
{
    constexpr float pi = 3.14159265358979323846f;
    if (s < 0.4f)
    {
        return 0.5f / pi + s * (0.220636f + s * (-0.109f + s * 0.090f));
    }
    const float a = std::sqrt(s);
    return (a * 0.5f + 0.0625f / a) / std::sqrt(pi);
}

static std::vector<ReferenceWaveMode> BuildReferenceWaveModes()
{
    struct AuthoredCascade
    {
        float length;
        float displacement;
        float windSpeed;
        float windDirection;
        float fetch;
        float swell;
        float spread;
        float detail;
    };
    constexpr float pi = 3.14159265358979323846f;
    constexpr std::array<AuthoredCascade, 2> cascades{{
        {88.0f, 1.0f, 10.0f, 20.0f * pi / 180.0f, 150000.0f, 0.8f, 0.2f, 1.0f},
        {57.0f, 0.75f, 5.0f, 15.0f * pi / 180.0f, 150000.0f, 0.8f, 0.4f, 1.0f},
    }};
    constexpr int resolution = 256;
    constexpr float gravity = 9.81f;
    constexpr float tau = 2.0f * pi;
    constexpr size_t retainedPerCascade = 4096;
    std::vector<ReferenceWaveMode> retained;
    retained.reserve(retainedPerCascade * cascades.size());

    for (const AuthoredCascade& cascade : cascades)
    {
        std::vector<ReferenceWaveMode> all;
        all.reserve(resolution * resolution);
        const float dk = tau / cascade.length;
        const float alpha = 0.076f * std::pow(cascade.windSpeed * cascade.windSpeed / (cascade.fetch * gravity), 0.22f);
        const float peak = 22.0f * std::pow(gravity * gravity / (cascade.windSpeed * cascade.fetch), 1.0f / 3.0f);

        for (int y = 0; y < resolution; ++y)
        {
            for (int x = 0; x < resolution; ++x)
            {
                const float kx = (static_cast<float>(x) - resolution * 0.5f) * dk;
                const float kz = (static_cast<float>(y) - resolution * 0.5f) * dk;
                const float k = std::sqrt(kx * kx + kz * kz) + 1e-6f;
                const float kd = k * 20.0f;
                const float tanhKd = std::tanh(kd);
                const float omega = std::sqrt(gravity * k * tanhKd);
                const float derivative = 0.5f * gravity * (tanhKd + kd * (1.0f - tanhKd * tanhKd)) / omega;
                const float p = omega / peak;
                const float s =
                    omega <= peak
                        ? 6.97f * std::pow(std::abs(p), 4.06f)
                        : 9.77f * std::pow(std::abs(p), -2.33f - 1.45f * (cascade.windSpeed * peak / gravity - 1.17f));
                const float sx = 16.0f * std::tanh(peak / omega) * cascade.swell * cascade.swell;
                const float alignment =
                    (kx * std::sin(cascade.windDirection) + kz * std::cos(cascade.windDirection)) / k;
                const float directional =
                    ReferenceSpreadNormalization(s + sx) * std::pow(std::max(0.5f * (1.0f + alignment), 0.0f), s + sx);
                const float spread = directional * (1.0f - cascade.spread) + (0.5f / pi) * cascade.spread;
                const float sigma = omega <= peak ? 0.07f : 0.09f;
                const float r = std::exp(-(omega - peak) * (omega - peak) / (2.0f * sigma * sigma * peak * peak));
                const float jonswap = alpha * gravity * gravity / std::pow(omega, 5.0f) *
                                      std::exp(-1.25f * std::pow(peak / omega, 4.0f)) * std::pow(3.3f, r);
                const float wh = std::min(omega * std::sqrt(20.0f / gravity), 2.0f);
                const float attenuation = wh <= 1.0f ? 0.5f * wh * wh : 1.0f - 0.5f * (2.0f - wh) * (2.0f - wh);
                const float damping = std::exp(-(1.0f - cascade.detail) * (1.0f - cascade.detail) * k * k);
                const float variance = jonswap * attenuation * spread * damping * derivative / k * dk * dk;
                const auto uniform = ReferenceHash(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                const float radius = std::sqrt(-2.0f * std::log(std::max(uniform[0], 1e-7f)));
                const float theta = tau * uniform[1];
                const float amplitude = std::sqrt(std::max(2.0f * variance, 0.0f));
                all.push_back({kx, kz, omega, radius * std::cos(theta) * amplitude,
                               radius * std::sin(theta) * amplitude, cascade.displacement});
            }
        }
        const size_t count = std::min(retainedPerCascade, all.size());
        std::partial_sort(all.begin(), all.begin() + count, all.end(),
                          [](const ReferenceWaveMode& a, const ReferenceWaveMode& b)
                          {
                              return std::hypot(a.h0Real, a.h0Imag) * a.displacementScale >
                                     std::hypot(b.h0Real, b.h0Imag) * b.displacementScale;
                          });
        retained.insert(retained.end(), all.begin(), all.begin() + count);
    }
    return retained;
}

static float ReferenceSurfaceHeight(float x, float z, float time, float amplitude, float speed, float wavelengthScale)
{
    static const std::vector<ReferenceWaveMode> modes = BuildReferenceWaveModes();
    const float invScale = 1.0f / std::max(wavelengthScale, 0.01f);
    float height = 0.0f;
    for (const ReferenceWaveMode& mode : modes)
    {
        const float phase = mode.omega * time * speed + (mode.kx * x + mode.kz * z) * invScale;
        height += 2.0f * (mode.h0Real * std::cos(phase) - mode.h0Imag * std::sin(phase)) * mode.displacementScale;
    }
    return height * std::max(amplitude, 0.0f);
}

// The spectrum consumes these independently per layer.  Keeping all preset data here
// makes a live Water-tab switch atomic: lengths in WgrWaterParams and their matching
// wind/fetch/seed/scales reach the GPU in the same frame.
// WTR-001: `seedXor` reconnects the dev-tab FFT seed override to the per-cascade seeds
// (the spectrum shader reads cfg.spectrum_seed, not fft_control.y). 0 leaves the tuned
// preset seeds untouched; any other value xors every cascade's seed so the whole h0
// field re-randomises deterministically.
// WTR-LOOK — physical sea-state coupling.
//
// A wind sea's height and its wavelength are not independent: JONSWAP gives the Phillips
// constant alpha ~ U^0.44 and the peak frequency omega_p ~ U^(-1/3) at a fixed fetch, so the
// total variance m0 ~ alpha / omega_p^4 ~ U^1.773 and the significant height H_s = 4*sqrt(m0)
// ~ U^0.887. Inverting that gives the wind multiplier needed for a height multiplier, so the
// Water-tab amplitude stays linear in wave height:
//
//     U_mult = amp^(1/0.887) = amp^1.127
//
// The peak wavelength then follows for free: lambda_p ~ 1/omega_p^2 ~ U^(2/3) = amp^0.75. That
// is the whole point — a rougher sea becomes a LONGER sea, instead of the same short waves
// stretched vertically into unrealistic chop.
// The coupling is referenced to the authored default amplitude, so leaving the slider where the
// preset put it reproduces the tuned look byte-for-byte and only *moving* it changes the sea
// state. Without this the default 0.4 would be read as a near-calm 3.5 m/s wind and silently
// reshape the shipped ocean.
static constexpr float kSeaStateReferenceAmplitude = 0.40f;

// Only ever GROWS the sea. Below the reference amplitude the residual spectrum scale below
// handles it instead, because driving the wind down would shorten the waves (chop) and, at
// amplitude 0, a clamped wind floor still produced a live sea — calm water became unreachable.
static float SeaStateWindMultiplier(float amplitude)
{
    const float ratio = std::max(amplitude, 0.0f) / kSeaStateReferenceAmplitude;
    return std::max(std::pow(ratio, 1.127f), 1.0f);
}

// Residual variance scale handed to the h0 pass. Below the reference amplitude this is just the
// old linear control, so 0 is dead flat and the calm end of the slider behaves exactly as it used
// to. At and above the reference it holds, and the wind multiplier takes over — which is what
// makes a rougher sea grow LONGER waves instead of merely taller ones.
//
// Getting this wrong is what broke the default look: returning 1.0 here while the wind multiplier
// was also 1.0 dropped the reference amplitude's own 0.4 factor, so every wave came out 2.5x too
// tall at the shipped setting.
static float SeaStateResidualAmplitude(float amplitude)
{
    return std::min(std::max(amplitude, 0.0f), kSeaStateReferenceAmplitude);
}

// The cascade domains have to grow with the peak wavelength, or the longer swell simply does not
// fit in the tile and wraps. Scaling them together keeps the spectral peak at the same relative
// position inside each cascade, so no new aliasing is introduced.
//
// Clamped to >= 1: the domain may grow but must never shrink. A calmer sea has shorter waves, but
// shrinking the tile to match would multiply how often it repeats across a long view and make the
// ocean read as a visible grid — the one artefact a large domain is there to prevent. Keeping the
// tile large merely leaves the low-wavenumber end of the spectrum quiet, which costs nothing.
static float SeaStateLengthMultiplier(float amplitude)
{
    const float ratio = std::max(amplitude, 0.0f) / kSeaStateReferenceAmplitude;
    return std::max(std::pow(ratio, 0.75f), 1.0f);
}

// ---------------------------------------------------------------------------
// Sea-state wind latch.
//
// The rule, stated explicitly because it is the whole reason the ocean can read
// the live wind at all without costing a spectrum rebuild per frame:
//
//   The spectrum re-derives from wind ONLY when BOTH of these hold:
//     (a) at least kSeaWindMinDwellSeconds have passed since the last re-derive, AND
//     (b) the MEAN wind has moved out of the latched bucket by more than the
//         bucket half-width plus a hysteresis band —
//           speed:     1.0 m/s buckets, 0.35 m/s hysteresis
//           direction: 15 deg buckets,  4 deg hysteresis
//
// Consequences worth being explicit about:
//   * Gusts never rebuild the spectrum. Only WindSample::meanSpeed /
//     meanDirectionRad are read here, and those move on a 4-20 minute clock. A
//     real sea is fetch/duration limited: it answers to the mean wind over tens
//     of minutes, so this is the physically correct input as well as the cheap one.
//   * Worst case is one rebuild per 20 s even if the wind is sitting exactly on
//     a bucket edge; the hysteresis band means it will not even do that.
//   * Typical gameplay: overcast moves at ~1/3600 per second, so the mean wind
//     crosses a 1 m/s bucket roughly every eight minutes.
// ---------------------------------------------------------------------------
constexpr float kSeaWindMinDwellSeconds = 20.0f;
constexpr float kSeaWindSpeedBucket = 1.0f;      // m/s
constexpr float kSeaWindSpeedHysteresis = 0.35f; // m/s beyond the half bucket
constexpr float kSeaWindDirBucket = 0.2617994f;  // 15 deg in rad
constexpr float kSeaWindDirHysteresis = 0.06981f; // 4 deg in rad

// Shortest signed angle from `from` to `to`, in (-pi, pi].
static float AngleDelta(float from, float to)
{
    constexpr float kPi = 3.14159265358979323846f;
    float d = std::fmod(to - from + kPi, 2.0f * kPi);
    if (d < 0.0f)
    {
        d += 2.0f * kPi;
    }
    return d - kPi;
}

static float SnapToBucket(float value, float bucket)
{
    return std::round(value / bucket) * bucket;
}

static void ApplyCascadePreset(WgrRenderer* renderer, int preset, int fftResolution = 512, uint32_t seedXor = 0,
                               float windMultiplier = 1.0f, float lengthMultiplier = 1.0f,
                               float windDirDeltaRad = 0.0f)
{
    WgrWaterCascadeConfig c{};
    c.enabled = 1;
    c.resolution = static_cast<uint32_t>(fftResolution == 256 || fftResolution == 1024 ? fftResolution : 512);
    c.displacement_scale = 1.0f;
    c.horiz_displacement_scale = 1.0f;
    c.normal_scale = 1.0f;
    c.foam_scale = 1.0f;
    c.wind_speed = 10.0f;
    c.wind_direction_rad = 0.349f;
    c.fetch_meters = 150000.0f;
    c.water_depth_meters = 20.0f;
    c.swell = 0.80f;
    c.directional_spread = 0.20f;
    c.short_wave_detail = 1.0f;
    c.whitecap_threshold = 0.50f;
    c.spectrum_seed = 0;
    // Godot water.gd dispatches cascade i at 120 + PI*i seconds.
    c.phase_offset_seconds = 120.0f;
    c.update_rate_hz = 60.0f;

    // Push one cascade config, applying the WTR-001 dev seed override (xor) so a non-zero
    // override re-randomises the whole h0 field deterministically while seedXor == 0 leaves
    // the tuned preset seeds untouched.
    auto push = [renderer, seedXor, windMultiplier, lengthMultiplier, windDirDeltaRad](
                    uint32_t index, const WgrWaterCascadeConfig& cfg)
    {
        WgrWaterCascadeConfig out = cfg;
        out.spectrum_seed ^= seedXor;
        // Sea-state coupling: wind speed drives the JONSWAP energy AND the peak frequency, and
        // the tile has to grow with the peak wavelength so the longer swell still fits.
        out.wind_speed *= windMultiplier;
        out.tile_length_x *= lengthMultiplier;
        out.tile_length_y *= lengthMultiplier;
        // World wind heading. Added as a DELTA rather than assigned, so the small
        // authored spread between cascades (0.349 / 0.2618 rad — the directional
        // spread that keeps the sea from being a single-heading corduroy) survives
        // the rotation. The delta comes from the latched sea wind, so it changes in
        // 15-degree steps at most once per 20 s and does not churn the spectrum.
        out.wind_direction_rad += windDirDeltaRad;
        wgr_water_set_cascade_config(renderer, index, &out);
    };

    if (preset == 1)
    {
        // krautdev/GodotOceanWaves' three published cascades.
        c.tile_length_x = c.tile_length_y = 88.0f;
        WgrWaterCascadeConfig b = c;
        b.tile_length_x = b.tile_length_y = 57.0f;
        b.displacement_scale = b.horiz_displacement_scale = 0.75f;
        b.foam_scale = 0.0f;
        b.wind_speed = 5.0f;
        b.wind_direction_rad = 0.2618f;
        b.directional_spread = 0.40f;
        b.spectrum_seed = 0;
        b.phase_offset_seconds = 120.0f + 3.14159265359f;
        WgrWaterCascadeConfig d = c;
        d.tile_length_x = d.tile_length_y = 16.0f;
        d.displacement_scale = d.horiz_displacement_scale = 0.0f;
        d.normal_scale = 0.25f;
        d.foam_scale = 3.0f;
        d.wind_speed = 20.0f;
        d.fetch_meters = 550000.0f;
        d.directional_spread = 0.40f;
        d.whitecap_threshold = 0.25f;
        d.spectrum_seed = 0;
        d.phase_offset_seconds = 120.0f + 2.0f * 3.14159265359f;
        c.foam_scale = 8.0f;
        WgrWaterCascadeConfig disabled{};
        push(0, c);
        push(1, b);
        push(2, d);
        push(3, disabled);
        return;
    }

    if (preset == 2)
    {
        // Retained solely for visual A/B of the old harmonic implementation.
        c.tile_length_x = c.tile_length_y = 48.0f;
        WgrWaterCascadeConfig b = c;
        b.tile_length_x = b.tile_length_y = 144.0f;
        b.wind_speed = 8.5f;
        b.spectrum_seed = 5678;
        b.phase_offset_seconds += 3.14159265359f;
        WgrWaterCascadeConfig d = c;
        d.tile_length_x = d.tile_length_y = 432.0f;
        d.wind_speed = 7.0f;
        d.spectrum_seed = 91011;
        d.phase_offset_seconds += 2.0f * 3.14159265359f;
        WgrWaterCascadeConfig e = c;
        e.tile_length_x = e.tile_length_y = 1296.0f;
        e.wind_speed = 6.0f;
        e.spectrum_seed = 121314;
        e.phase_offset_seconds += 3.0f * 3.14159265359f;
        push(0, c);
        push(1, b);
        push(2, d);
        push(3, e);
        return;
    }

    // Production: larger pairwise-prime domains make an individual cascade's repeat
    // imperceptible as well as pushing the shared period beyond the playable world.
    // Small per-layer direction/seed changes prevent the bands from phase-locking.
    c.tile_length_x = c.tile_length_y = 97.0f;
    c.wind_speed = 12.0f;
    c.wind_direction_rad = 0.31f;
    c.fetch_meters = 210000.0f;
    c.spectrum_seed = 1471;
    WgrWaterCascadeConfig b = c;
    b.tile_length_x = b.tile_length_y = 257.0f;
    b.wind_speed = 9.5f;
    b.wind_direction_rad = 0.43f;
    b.spectrum_seed = 8623;
    b.phase_offset_seconds += 3.14159265359f;
    WgrWaterCascadeConfig d = c;
    d.tile_length_x = d.tile_length_y = 683.0f;
    d.wind_speed = 7.5f;
    d.wind_direction_rad = 0.22f;
    d.fetch_meters = 350000.0f;
    d.spectrum_seed = 24593;
    d.phase_offset_seconds += 2.0f * 3.14159265359f;
    WgrWaterCascadeConfig e = c;
    e.tile_length_x = e.tile_length_y = 1777.0f;
    e.wind_speed = 6.0f;
    e.wind_direction_rad = 0.37f;
    e.fetch_meters = 550000.0f;
    e.spectrum_seed = 73471;
    e.phase_offset_seconds += 3.0f * 3.14159265359f;
    push(0, c);
    push(1, b);
    push(2, d);
    push(3, e);
}

WaterWgpu::WaterWgpu(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer)
{
    // Preserve WGR_WATER_LOD_BASE as an expert scale over the Water-tab geometry
    // presets. 8 was the former fixed default, so an unset environment is neutral.
    _baseMultScale = std::max(0.125f, EnvFloat("WGR_WATER_LOD_BASE", 8.0f) / 8.0f);
    _baseMult = 8.0f * _baseMultScale;
    _lodRatio = EnvFloat("WGR_WATER_LOD_RATIO", 2.0f);
    _morphRegion = std::clamp(EnvFloat("WGR_WATER_MORPH", 0.50f), 0.05f, 1.0f);
    // >= 1 keeps at least the map; 3 gives ~one map width of ocean past each edge,
    // which comfortably clears the far plane/fog on the stock maps.
    _extentFactor = std::max(1.0f, EnvFloat("WGR_WATER_EXTENT", 3.0f));
    _interactionDemo = EnvFloat("WGR_WATER_INTERACTION_DEMO", 0.0f) > 0.5f;
}

void WaterWgpu::BuildQuadtree(const Landscape& land)
{
    const int range = land.GetTerrainRange();
    const float grid = land.GetTerrainGrid();

    // Over-size the root and centre the map inside it so the ocean continues past the
    // map edges to the horizon (the legacy path fills off-map with all-water tiles).
    const int coverage = std::max(range, static_cast<int>(std::lround(_extentFactor * range)));
    const int rootTexels = CdlodRootTexels(coverage, WaterGridN);
    const int originTexel = CdlodCenteredOrigin(rootTexels, range, WaterGridN);

    // Same leaf min/max scan as terrain where the leaf overlaps the map (water
    // existence is defined by the terrain dipping below sea level); off-map texels are
    // open ocean. Their bound is pinned to the sea datum (0), NOT a fake deep seabed:
    // the node minY/maxY drive both the below-sea keep test AND the CDLOD frustum/LOD
    // bounding volume, and the surface is drawn at sea level regardless of any seabed —
    // a deep off-map value would sink the bounding sphere below the horizon view and
    // wrongly cull near off-map tiles. 0 <= the keep threshold, so off-map is kept.
    // WTR-034 — Conservative CDLOD displacement bounds:
    // Derivation of conservative bounding volume expansion:
    // 1. Vertical displacement bound (D_y): sum of FFT cascade max crest heights (wave_amp * 1.8f)
    // 2. Horizontal choppiness bound (D_xz): horizontal displacement shifts vertices by up to choppiness * wave_amp
    // (1.2f * wave_amp)
    // 3. Interaction & particle impulse padding: maximum vessel/interaction splash impulse height (+/- 1.5m)
    // 4. Safety margin (1.25x): guarantees bounding spheres never cull crests near frustum edges.
    constexpr float OffMapSurface = 0.0f;
    // 5. Shore breakers: the shoaling train adds up to ~0.62 * wave_amp * shoreGain * 3.2 (its
    //    Green's-law gain) on top of the offshore FFT crest, so the coastal band needs its own
    //    term or near-beach nodes get culled at exactly the moment their waves are tallest.
    const float vertDisplacement = _params.wave_amp * 1.8f;
    const float horizChoppiness = _params.wave_amp * 1.2f;
    const float shoreBreaker = _params.wave_amp * 2.0f * std::max(_engine.WaterLook().shoreWaveGain, 0.0f);
    const float interactionImpulse = 1.5f;
    const float conservativeBound =
        (vertDisplacement + horizChoppiness * 0.5f + shoreBreaker + interactionImpulse) * 1.25f;
    const float crestPadding = std::max(conservativeBound, 3.5f);
    auto leafBounds = [&](int ox, int oz, int span, float& mn, float& mx)
    {
        for (int z = oz; z <= oz + span; z++)
        {
            for (int x = ox; x <= ox + span; x++)
            {
                const float h = (x < 0 || z < 0 || x >= range || z >= range) ? OffMapSurface : land.GetHeight(z, x);
                mn = std::min(mn, h - crestPadding);
                mx = std::max(mx, h + crestPadding);
            }
        }
    };
    BuildCdlodTree(rootTexels, originTexel, originTexel, grid, WaterGridN, leafBounds, _tree, _rootIndex, _numLevels,
                   _leafSize);
    if (_rootIndex < 0)
    {
        return;
    }
    ComputeCdlodRanges(_leafSize * _baseMult, _lodRatio, _numLevels, _ranges);
    _treeMin = originTexel * grid;
    _treeMax = (originTexel + rootTexels) * grid;

    // Highest possible sea surface (max tide) plus a wave-crest margin: nodes whose
    // terrain never rises to this height are entirely underwater and join the tree;
    // the rest are pruned. Generous + conservative — the shoreline is depth-cut, not
    // meshed, so precision here does not matter (§3 of the plan).
    _seaThreshold = maxTide + maxWave + 1.0f;

    _params = WgrWaterParams{};
    _params.world_origin = {0.0f, 0.0f};
    _params.terrain_grid = grid;
    _params.sea_level = land.GetSeaLevel();
    _params.hm_width = static_cast<uint32_t>(range);
    _params.hm_height = static_cast<uint32_t>(range);
    // Weather does not currently expose a renderer-facing wind vector. Keep this deterministic
    // until the environment weather service is threaded here.
    _params.fft_control = {1.0f, 1337.0f, 12.0f, 0.0f};
    // Match the energetic reference sea state.  The last lane drives spectral energy;
    // 0.08 made waves, foam, and spray effectively invisible in normal gameplay.
    _params.fft_wind_sea = {0.82f, 0.57f, 12.0f, 0.65f};
    _params.fft_cascade_lengths = {48.0f, 144.0f, 432.0f, 1296.0f};
    // The sole draw path is the global ocean plane. Keep directed flow disabled until
    // water-body batches can supply a river-only material signal.
    _params.flow_direction_speed = {0.0f, 0.0f, 0.0f, static_cast<float>(WGR_WATER_KIND_OCEAN)};
    _haveInteractionDomain = false;

    // WTR-001: pass the dev-tab seed override (>= 0) as the xor so the initial spectrum
    // build honours it; 0 when the override is disabled.
    const auto& fz0 = _engine.WaterLook().freeze;
    ApplyCascadePreset(_renderer, _engine.WaterLook().cascadePreset, _engine.WaterLook().fftResolution,
                       fz0.fftSeed >= 0 ? static_cast<uint32_t>(fz0.fftSeed) : 0u);
}

bool WaterWgpu::UpdateSeaWind(float now)
{
    // Opt-out: the setting, or WGR_WATER_WIND=0 for an A/B capture without a UI.
    static const bool envEnabled = EnvFloat("WGR_WATER_WIND", 1.0f) != 0.0f;
    if (!_engine.WaterLook().windFromWeather || !envEnabled || !GWind.IsActive())
    {
        return false;
    }

    // MEAN wind only. Never `speed` / `directionRad` — see the latch commentary
    // above ApplyCascadePreset for why a gust must not touch h0.
    const WindSample& wind = GWind.Sample();
    const float wantSpeed = SnapToBucket(wind.meanSpeed, kSeaWindSpeedBucket);
    const float wantDir = SnapToBucket(wind.meanDirectionRad, kSeaWindDirBucket);

    if (!_seaWindInit)
    {
        _seaWindInit = true;
        _seaWindSpeed = wantSpeed;
        _seaWindDirRad = wantDir;
        _seaWindLatchTime = now;
        return true;
    }

    // (a) dwell
    if (now - _seaWindLatchTime < kSeaWindMinDwellSeconds)
    {
        return false;
    }
    // (b) moved out of the latched bucket by more than half a bucket + hysteresis
    const bool speedMoved =
        std::abs(wind.meanSpeed - _seaWindSpeed) > (kSeaWindSpeedBucket * 0.5f + kSeaWindSpeedHysteresis);
    const bool dirMoved =
        std::abs(AngleDelta(_seaWindDirRad, wind.meanDirectionRad)) > (kSeaWindDirBucket * 0.5f + kSeaWindDirHysteresis);
    if (!speedMoved && !dirMoved)
    {
        return false;
    }

    _seaWindSpeed = wantSpeed;
    _seaWindDirRad = wantDir;
    _seaWindLatchTime = now;
    LOG_INFO(Graphics, "Water sea-state wind latched: {:.1f} m/s heading {:.0f} deg (spectrum re-derives once)",
             _seaWindSpeed, _seaWindDirRad * (180.0f / 3.14159265358979323846f));
    return true;
}

bool WaterWgpu::RebuildIfNeeded(const Landscape& land)
{
    const int range = land.GetTerrainRange();
    const char* name = land.GetName();
    if (_built == &land && _builtRange == range && _builtName == name)
    {
        return false;
    }
    BuildQuadtree(land);
    _built = &land;
    _builtRange = range;
    _builtName = name;
    return true;
}

void WaterWgpu::DrawWater(Scene& scene, int xBeg, int zBeg, int xEnd, int zEnd)
{
    if (_renderer == nullptr || GLandscape == nullptr)
    {
        return;
    }
    const Engine::WaterSettings& look = _engine.WaterLook();
    // Keep gameplay-side rifle droplets in lockstep with the renderer setting even
    // when the developer overlay has never been opened.
    SetRifleWaterImpactSprayEnabled(look.rifleImpactSpray);
    if (!look.enabled)
    {
        // Water suppressed from the tab: draw nothing (the seabed shows, for A/B).
        return;
    }

    const Landscape& land = *GLandscape;
    RebuildIfNeeded(land);
    if (_rootIndex < 0)
    {
        return;
    }

    // GodotOceanWaves swaps between camera-following clipmap meshes. A literal
    // 512 m demo clipmap cannot replace this renderer's terrain-aware shoreline
    // pruning and off-map horizon coverage, so expose the equivalent quantity:
    // how far fine CDLOD patches remain active. Recomputing the small range array
    // is immediate and does not rebuild the quadtree or any GPU resource.
    constexpr std::array<float, 4> GeometryBaseMult = {4.0f, 6.0f, 8.0f, 12.0f};
    const int geometryQuality = std::clamp(look.geometryQuality, 0, 3);
    const float desiredBaseMult = GeometryBaseMult[geometryQuality] * _baseMultScale;
    if (_activeGeometryQuality != geometryQuality || std::abs(_baseMult - desiredBaseMult) > 0.001f)
    {
        _activeGeometryQuality = geometryQuality;
        _baseMult = desiredBaseMult;
        ComputeCdlodRanges(_leafSize * _baseMult, _lodRatio, _numLevels, _ranges);
        LOG_INFO(Graphics, "Water geometry quality applied: preset={} baseMult={:.2f} range0={:.0f}m", geometryQuality,
                 _baseMult, _ranges.empty() ? 0.0f : _ranges[0]);
    }

    Camera* camera = scene.GetCamera();
    if (camera == nullptr)
    {
        return;
    }

    // WTR-001 — deterministic water debug. All freeze switches substitute a fixed value for the
    // variable the shader reads, so the same test frame produces the same UBO (and the same
    // h0/random stream) every launch. Glob.time itself is NOT mutated — only the value handed to
    // the water/cloud/underwater shaders — so gameplay and net time are untouched.
    const Engine::WaterSettings::Freeze& fz = look.freeze;

    // Refresh the animated sea level + wave clock + live look every frame; the whole
    // plane rides at this height (no mesh regeneration — the legacy path re-levelled
    // cached vertices), and the Gerstner waves advance off `time` in the shader.
    _params.sea_level = land.GetSeaLevel();
    _params.time = fz.freezeTime ? fz.fixedTime : Glob.time.toFloat();
    _params.wave_amp = look.waveAmp;
    _params.wave_choppy = look.waveChoppy;
    _params.wave_speed = look.waveSpeed;
    _params.wave_scale = look.waveScale;
    _params.fade_start = look.fadeStart;
    _params.fade_end = look.fadeEnd;
    _params.warp_amp = look.warpAmp;
    _params.spec_power = look.specPower;
    _params.spec_intensity = look.specIntensity;
    _params.alpha = look.alpha;
    _params.shadow_dim = look.shadowDim;
    _params.color_ext = look.colorExt;
    _params.coast_fade = look.coastFade;
    _params.shallow_color = {look.shallowColor[0], look.shallowColor[1], look.shallowColor[2], 0.0f};
    _params.deep_color = {look.deepColor[0], look.deepColor[1], look.deepColor[2], 0.0f};
    _params.foam_width = look.foamWidth;
    _params.foam_intensity = look.foamIntensity;
    _params.swash_amp = look.swashAmp;
    _params.swash_speed = look.swashSpeed;
    const Vector3 cameraPos = camera->Position();
    // Post-processing is a camera effect.  Player-body water depth is useful for
    // splash events, but using it here made wading apply underwater fog above water.
    float localSurface = land.GetSeaLevel();
    if (look.cascadePreset == 1)
    {
        localSurface += ReferenceSurfaceHeight(cameraPos.X(), cameraPos.Z(), _params.time, look.waveAmp, look.waveSpeed,
                                               look.waveScale);
    }
    // Small asymmetric hysteresis keeps the compositor from flickering when the eye
    // rides exactly on a moving FFT crest.
    // Both thresholds are live from the Water tab. Clamping exit to at least enter keeps the
    // band from inverting if they are dragged past each other, which would make the state
    // oscillate every frame instead of latching.
    const float enterDepth = std::max(look.underwaterEnterDepth, 0.0f);
    const float exitDepth = std::max(look.underwaterExitDepth, enterDepth);
    if (_cameraSubmerged)
    {
        _cameraSubmerged = cameraPos.Y() < localSurface + exitDepth;
    }
    else
    {
        _cameraSubmerged = cameraPos.Y() < localSurface - enterDepth;
    }
    // Gates BOTH the fullscreen underwater compositor and the water shader's own
    // underwater tint. Off unless the Water tab enables it.
    //
    // Carries the eye's submersion DEPTH in metres rather than a 0/1 flag. As a
    // flag, one centimetre under the surface produced exactly the same
    // full-strength tint as ten metres down, and the compositor snapped its
    // camera height to a hard -0.08 the instant the flag tripped — so a passing
    // crest popped the whole screen to full underwater colour. Depth lets both
    // consumers ramp instead. Positive means submerged; zero means dry, so the
    // readers test `> 0` where they used to test `> 0.5`.
    //
    // The hysteresis above still decides *whether* the effect is on; this only
    // changes how strongly.
    const float submersion = localSurface - cameraPos.Y();
    _params.fft_control.w = (_cameraSubmerged && look.underwaterEffect) ? std::max(submersion, 0.001f) : 0.0f;
    // "The underwater effect kicks in when I am 3-4 metres above the water." Every
    // explanation for that is a guess until these five numbers are on the table, so log
    // them whenever the answer changes, plus once a second while the effect is engaged.
    //
    // What to read: `submerged=1` with `camY` well above `sea` means localSurface is the
    // culprit, which on cascadePreset 1 is sea level plus ReferenceSurfaceHeight scaled by
    // the Water tab's amplitude — crank that and the CPU thinks the eye is under a wave
    // several metres up. `submerged=0` with the effect still visible means it is not this
    // gate at all and the compositor's own near-surface band is what engaged.
    {
        const float camAbove = cameraPos.Y() - land.GetSeaLevel();
        const bool stateChanged = _cameraSubmerged != _loggedCameraSubmerged;
        const bool dueForRepeat = _cameraSubmerged && (_params.time - _lastSubmersionLogTime) >= 1.0f;
        if (stateChanged || dueForRepeat)
        {
            _loggedCameraSubmerged = _cameraSubmerged;
            _lastSubmersionLogTime = _params.time;
            LOG_INFO(Graphics,
                     "Water submersion: submerged={} camY={:.2f} sea={:.2f} localSurface={:.2f} "
                     "camAbove={:.2f} submersion={:.2f} preset={} waveAmp={:.2f} effect={}",
                     _cameraSubmerged ? 1 : 0, cameraPos.Y(), land.GetSeaLevel(), localSurface, camAbove, submersion,
                     look.cascadePreset, look.waveAmp, look.underwaterEffect ? 1 : 0);
        }
    }
    // WTR-001 — deterministic FFT seed. The authored default (1337.0, set in BuildQuadtree)
    // already keeps the random field stable across frames; allow the dev tab to override it,
    // so a frozen frame is reproducible regardless of seq-of-edits to the spectrum. Setting a
    // non-negative value rewrites fft_control[1]; -1 keeps the authored 1337 (no swap).
    if (fz.fftSeed >= 0)
    {
        _params.fft_control.y = static_cast<float>(fz.fftSeed);
    }
    // WTR-001 — freeze dispatch mask packed into fft_control.z. The Rust side reads the float's
    // bits as the WGR_WATER_FREEZE_* mask and skips the matching compute dispatch. Encoding is
    // bit-cast so the shaders still see a normal IEEE float (0.0 with no bits = no freeze).
    uint32_t freezeMask = 0u;
    if (fz.freezeFft)
    {
        freezeMask |= WGR_WATER_FREEZE_FFT;
    }
    if (fz.freezeInteraction)
    {
        freezeMask |= WGR_WATER_FREEZE_INTERACTION;
    }
    if (fz.freezeFoam)
    {
        freezeMask |= WGR_WATER_FREEZE_FOAM;
    }
    float freezeBits = 0.0f;
    std::memcpy(&freezeBits, &freezeMask, sizeof(freezeBits));
    _params.fft_control.z = freezeBits;
    // WTR-036C / WTR-037 — Apply cascade configuration presets
    if (look.cascadePreset == 1)
    {
        // GodotOceanWaves Reference Style (3 cascades: 88m, 57m, 16m)
        _params.fft_cascade_lengths = {88.0f, 57.0f, 16.0f, 0.0f};
    }
    else if (look.cascadePreset == 2)
    {
        // Legacy 4-Cascade Harmonic (48m, 144m, 432m, 1296m)
        _params.fft_cascade_lengths = {48.0f, 144.0f, 432.0f, 1296.0f};
    }
    else
    {
        // Production non-repeating four-cascade ocean.  The smallest 97m domain is
        // large enough that its own pattern does not read as a visible tiled square.
        _params.fft_cascade_lengths = {97.0f, 257.0f, 683.0f, 1777.0f};
    }
    // WTR-LOOK — sea-state coupling. In coupled mode the amplitude becomes a wind speed and a
    // matching set of cascade lengths, so a rougher sea grows longer waves rather than steeper
    // ones, and the spectrum pass is told to apply no residual amplitude of its own. Legacy mode
    // keeps the old behaviour (uniform variance scaling at unchanged wavelengths) for A/B.
    float windMultiplier = look.seaStateCoupling ? SeaStateWindMultiplier(look.waveAmp) : 1.0f;
    const float lengthMultiplier = look.seaStateCoupling ? SeaStateLengthMultiplier(look.waveAmp) : 1.0f;
    // WIND AUTHORITY — latch the world's mean wind into the spectrum. UpdateSeaWind
    // is the throttle; everything below reads only the latched values, which change
    // in 1 m/s / 15-degree steps and at most once per 20 s, so the h0 rebuild budget
    // is spent on real sea-state changes and nothing else.
    UpdateSeaWind(_params.time);
    float windDirDeltaRad = 0.0f;
    if (_seaWindInit)
    {
        _params.fft_wind_sea.x = std::cos(_seaWindDirRad);
        _params.fft_wind_sea.y = std::sin(_seaWindDirRad);
        _params.fft_wind_sea.z = _seaWindSpeed;
        // Sea state lane. 12 m/s -> 0.667, which is where the authored 0.65 sat, so
        // an average blustery day reproduces the shipped look.
        _params.fft_wind_sea.w = std::clamp(_seaWindSpeed / 18.0f, 0.0f, 1.0f);
        // Rotate the cascades off their authored reference heading (0.349 rad), which
        // preserves the inter-cascade directional spread — see the push lambda.
        windDirDeltaRad = _seaWindDirRad - 0.349f;
        // Gentle energy coupling on top of the amplitude slider. Deliberately NOT a
        // linear speed ratio: at calm the mean wind is ~1.5 m/s and a literal ratio
        // would flatten the sea to a mirror, which reads as a broken ocean rather
        // than a calm one. Anchored so 12 m/s (the shipped constant) is exactly 1.0.
        windMultiplier *= std::clamp(0.55f + 0.45f * (_seaWindSpeed / 12.0f), 0.55f, 1.60f);
    }
    if (look.seaStateCoupling)
    {
        _params.fft_cascade_lengths.x *= lengthMultiplier;
        _params.fft_cascade_lengths.y *= lengthMultiplier;
        _params.fft_cascade_lengths.z *= lengthMultiplier;
        _params.fft_cascade_lengths.w *= lengthMultiplier;
    }
    // WTR-001: per-frame preset push carries the dev-tab seed override (>= 0) as the xor
    // so toggling it live re-randomises h0 deterministically on the next spectrum rebuild.
    ApplyCascadePreset(_renderer, look.cascadePreset, look.fftResolution,
                       fz.fftSeed >= 0 ? static_cast<uint32_t>(fz.fftSeed) : 0u, windMultiplier, lengthMultiplier,
                       windDirDeltaRad);

    // y gates the GPU whitewater/spray billboard pass and z controls its activity.
    // The authored default is enabled at a restrained 0.25 activity. x remains the
    // debug-view selector.
    // w carries the live viewport height in pixels so the shader's per-cascade
    // projected-pixel filtering (compute_cascade_weights) uses the real backbuffer
    // height instead of a hardcoded 1080.
    _params.debug_params = {static_cast<float>(look.debugView), look.rifleImpactSpray ? 1.0f : 0.0f,
                            look.waterSplashParticleActivity, static_cast<float>(std::max(_engine.Height(), 1))};
    // WTR-LOOK — surface energy model + artist gains. x selects the composite (0 legacy,
    // 1 physical); y/z/w gain the sun glitter, subsurface scattering and environment reflection.
    _params.look_params = {look.physicalLook ? 1.0f : 0.0f, look.glitterGain, look.sssGain, look.reflectionGain};
    // WTR-LOOK — sea state / quality / shore lanes. y is the residual spectrum amplitude: 1.0 in
    // coupled mode (the wind speed and cascade lengths above already carry the energy), the raw
    // slider in legacy mode.
    _params.underwater_params = {std::max(look.underwaterEngageBand, 0.0f), std::max(look.underwaterDensity, 0.0f),
                                 std::clamp(look.underwaterColorBias, 0.0f, 1.0f),
                                 std::max(look.underwaterCausticGain, 0.0f)};
    // The compositor's own switch. fft_control.w below carries submersion DEPTH and is also
    // gated by this flag, but depth alone cannot turn the pass off: the renderer engages it on
    // either that depth or the camera being near the surface, and a submerged camera is always
    // near the surface. That is why the checkbox appeared to do nothing.
    // Planar-reflection frustum padding. Its own entry point rather than a params lane, because
    // it is consumed when the reflected CAMERA is built, before any water params are read.
    wgr_set_planar_reflection_pad(_renderer, std::clamp(look.reflectionFovPad, 1.0f, 3.0f));
    // y/z were reserved lanes; they now carry the WAVE-foam controls (whitecaps), which are
    // deliberately separate from foamIntensity's shoreline band. Reusing spare lanes keeps
    // WgrWaterParams at its asserted size on both sides of the FFI.
    _params.underwater_gate = {look.underwaterEffect ? 1.0f : 0.0f,
                               std::clamp(look.waveFoamIntensity, 0.0f, 2.0f),
                               std::clamp(look.waveFoamDeepFalloff, 0.0f, 1.0f),
                               std::clamp(look.reflectionEdgeFade, 0.02f, 0.49f)};
    _params.sea_params = {look.seaStateCoupling ? 1.0f : 0.0f,
                          look.seaStateCoupling ? SeaStateResidualAmplitude(look.waveAmp) : look.waveAmp,
                          look.lowQuality ? 1.0f : 0.0f, look.shoreWaveGain};
    // Runtime proof that the Water tab reaches the actual renderer. This is deliberately
    // edge-triggered: one log row per edited amplitude/resolution, not one row per frame.
    static float lastLoggedWaveAmp = -1.0f;
    static int lastLoggedFftResolution = 0;
    if (std::abs(lastLoggedWaveAmp - look.waveAmp) > 0.0001f || lastLoggedFftResolution != look.fftResolution)
    {
        LOG_INFO(
            Graphics, "Water look applied: amplitude={:.3f}, choppiness={:.3f}, speed={:.3f}, preset={}, FFT={}x{}",
            look.waveAmp, look.waveChoppy, look.waveSpeed, look.cascadePreset, look.fftResolution, look.fftResolution);
        lastLoggedWaveAmp = look.waveAmp;
        lastLoggedFftResolution = look.fftResolution;
    }
    wgr_water_set_params(_renderer, &_params);

    // WTR-001 — repeatable-camera-path foundation. When the Water tab sets a frame tag (>= 0),
    // log it with an FNV-1a digest of the exact UBO bytes just uploaded plus the camera pose,
    // so two launches can be diffed frame-by-frame from the log alone (the acceptance evidence
    // for "the same test frame should reproduce the same result between launches").
    if (fz.cameraPathFrame >= 0)
    {
        uint32_t digest = 2166136261u; // FNV-1a 32-bit offset basis
        const auto* bytes = reinterpret_cast<const unsigned char*>(&_params);
        for (size_t i = 0; i < sizeof(_params); ++i)
        {
            digest = (digest ^ bytes[i]) * 16777619u;
        }
        LOG_INFO(Graphics,
                 "WTR-001 camPath frame={} waterUboDigest={:08x} cam=({:.3f},{:.3f},{:.3f}) dir=({:.3f},{:.3f},{:.3f})",
                 fz.cameraPathFrame, digest, cameraPos.X(), cameraPos.Y(), cameraPos.Z(), camera->Direction().X(),
                 camera->Direction().Y(), camera->Direction().Z());
    }
    constexpr float interactionSize = 256.0f;
    const float originX = std::floor((cameraPos.X() - interactionSize * 0.5f) / 4.0f) * 4.0f;
    const float originZ = std::floor((cameraPos.Z() - interactionSize * 0.5f) / 4.0f) * 4.0f;
    const float now = _params.time;
    // WTR-063 — Fixed simulation timestep accumulator:
    // Accumulates frame dt and executes sub-steps of fixed 1/60s (0.016666s) to ensure wave physics stability.
    static float s_interactionAccumulator = 0.0f;
    constexpr float kFixedStep = 1.0f / 60.0f;
    const float rawDt =
        fz.freezeInteraction ? 0.0f : (fz.fixedDelta > 0.0f ? fz.fixedDelta : (now - _lastInteractionTime));
    s_interactionAccumulator += std::clamp(rawDt, 0.0f, 0.1f);
    // The accumulator existed but `dt` was set to kFixedStep unconditionally and the accumulator
    // was only drained when it happened to hold a full step. That advanced the simulation by 1/60 s
    // EVERY frame regardless of real elapsed time: at 120 fps the water ran at roughly twice speed,
    // and below 60 fps it drained only one step per frame and fell progressively behind.
    //
    // There is one compute dispatch per frame, so catching up means handing the solver a dt worth
    // several steps rather than looping. Capped at three so a hitch or a loading stall cannot
    // deliver one enormous step and blow the solver up.
    constexpr int kMaxCatchUpSteps = 3;
    const int steps = std::min(static_cast<int>(s_interactionAccumulator / kFixedStep), kMaxCatchUpSteps);
    float dt = kFixedStep * static_cast<float>(steps);
    s_interactionAccumulator -= dt;
    // Drop any excess beyond the catch-up cap instead of letting it accumulate forever.
    s_interactionAccumulator = std::min(s_interactionAccumulator, kFixedStep * kMaxCatchUpSteps);

    std::array<WgrWaterInteractionEvent, WGR_MAX_WATER_INTERACTIONS> events{};
    uint32_t eventCount = 0;
    if (_interactionDemo)
    {
        const int pulse = static_cast<int>(std::floor(now / 3.0f));
        if (pulse != _lastInteractionDemoPulse)
        {
            const Vector3 direction = camera->Direction();
            WgrWaterInteractionEvent& event = events[eventCount++];
            event.position_radius = {cameraPos.X() + direction.X() * 14.0f, cameraPos.Z() + direction.Z() * 14.0f, 1.7f,
                                     0.30f};
            event.velocity_kind = {direction.X() * 2.0f, direction.Z() * 2.0f, -4.0f,
                                   static_cast<float>(WGR_WATER_INTERACTION_OBJECT)};
            event.time_life_foam_mass = {0.0f, 1.6f, 0.35f, 0.0f};
            event.direction_depth_flags = {direction.X(), direction.Z(), 0.0f,
                                           static_cast<float>(WGR_WATER_INTERACTION_PENDING_IMPULSE)};
            _lastInteractionDemoPulse = pulse;
        }
    }
    std::array<HydroWaterInteractionEvent, HydroMaxWaterInteractions> submitted{};
    const uint32_t submittedCount = DrainWaterInteractions(submitted.data(), HydroMaxWaterInteractions);
    for (uint32_t i = 0; i < submittedCount && eventCount < WGR_MAX_WATER_INTERACTIONS; ++i)
    {
        const HydroWaterInteractionEvent& source = submitted[i];
        WgrWaterInteractionEvent& event = events[eventCount++];
        event.position_radius = {source.positionRadius[0], source.positionRadius[1], source.positionRadius[2],
                                 source.positionRadius[3]};
        event.velocity_kind = {source.velocityKind[0], source.velocityKind[1], source.velocityKind[2],
                               source.velocityKind[3]};
        event.time_life_foam_mass = {source.timeLifeFoamMass[0], source.timeLifeFoamMass[1], source.timeLifeFoamMass[2],
                                     source.timeLifeFoamMass[3]};
        event.direction_depth_flags = {source.directionDepthFlags[0], source.directionDepthFlags[1],
                                       source.directionDepthFlags[2], source.directionDepthFlags[3]};
    }

    // Rotor wash repeatedly injects the fast capillary ripple profile used by
    // bullet impacts, but over a wide rotor-disc-sized area.  This makes the
    // water visibly chatter beneath a helicopter instead of reading as a slow
    // boat wake, while RPM still controls the strength.
    const Helicopter* lastRotor = _engine.LastGrassRotor();
    auto addRotorWash = [&](const Helicopter* helicopter)
    {
        if (!helicopter || eventCount >= WGR_MAX_WATER_INTERACTIONS)
            return;
        const float rotorSpeed = std::clamp(helicopter->RotorSpeed(), 0.0f, 1.0f);
        if (rotorSpeed <= 0.02f)
            return;
        const Vector3 pos = helicopter->Position();
        const float altitude = pos.Y() - _params.sea_level;
        // A hovering aircraft can still stir the surface from several metres
        // up, but do not inject ripples from a helicopter high in the sky or
        // parked over dry land.
        if (altitude < -2.0f || altitude > 30.0f || GLandscape->SurfaceY(pos.X(), pos.Z()) > _params.sea_level + 0.25f)
            return;
        WgrWaterInteractionEvent& event = events[eventCount++];
        event.position_radius = {pos.X(), pos.Z(), 3.0f + 12.0f * rotorSpeed, 1.10f * rotorSpeed * rotorSpeed};
        event.velocity_kind = {0.0f, 0.0f, -20.0f, static_cast<float>(WGR_WATER_INTERACTION_BULLET)};
        // Bullet events are one-frame impulses. Re-emitting them every frame
        // while the rotor turns creates a fast, dense ripple field that stops
        // immediately when the RPM reaches zero.
        event.time_life_foam_mass = {now, 0.0f, 0.0f, 0.0f};
        event.direction_depth_flags = {0.0f, 0.0f, 0.0f, static_cast<float>(WGR_WATER_INTERACTION_PENDING_IMPULSE)};
    };
    bool lastRotorWasListed = false;
    if (GWorld)
    {
        for (int i = 0; i < GWorld->NVehicles(); ++i)
        {
            const Helicopter* helicopter = dynamic_cast<const Helicopter*>(GWorld->GetVehicle(i));
            if (helicopter == lastRotor)
                lastRotorWasListed = true;
            addRotorWash(helicopter);
        }
    }
    // Mirrors grass: the dismounted player helicopter may be absent from the
    // distributed list for a short handoff, but its weak link still exposes RPM.
    if (!lastRotorWasListed)
        addRotorWash(lastRotor);

    const bool reset = !_haveInteractionDomain || std::abs(originX - _interaction.domain.x) > interactionSize * 0.5f ||
                       std::abs(originZ - _interaction.domain.y) > interactionSize * 0.5f;
    _interaction.previous_domain = _haveInteractionDomain
                                       ? _interaction.domain
                                       : WgrVec4{originX, originZ, interactionSize, 1.0f / interactionSize};
    _interaction.domain = {originX, originZ, interactionSize, 1.0f / interactionSize};
    _interaction.grid = {256.0f, dt, static_cast<float>(eventCount), reset ? 1.0f : 0.0f};
    _interaction.physics = {12.0f, 1.6f, 0.35f, 1.2f};
    _interaction.misc = {0.0f, now, 0.0f, 0.0f};
    _interaction.weather = {0.0f, std::clamp(1.0f - look.waveAmp * 0.45f, 0.15f, 1.0f), 0.0f, 0.0f};
    wgr_water_set_interaction_params(_renderer, &_interaction);

    if (eventCount != 0)
    {
        wgr_water_submit_interactions(_renderer, events.data(), eventCount);
    }
    _lastInteractionTime = now;
    _haveInteractionDomain = true;

    // Water clips to the whole (over-sized) tree, not the engine's land draw-distance
    // rect: the ocean should reach the horizon, past where terrain stops. Frustum
    // culling + the CDLOD distance ranges + fog bound what actually draws. (xBeg..zEnd
    // stay part of the interface for the sibling look plan / future per-rect work.)
    (void)xBeg;
    (void)zBeg;
    (void)xEnd;
    (void)zEnd;
    const float rx0 = _treeMin;
    const float rz0 = _treeMin;
    const float rx1 = _treeMax;
    const float rz1 = _treeMax;

    auto emit = [&](const CdlodSelection& s)
    {
        WgrWaterNode node{};
        node.origin = {s.originX, s.originZ};
        node.size = s.size;
        node.lod = static_cast<uint32_t>(s.level);
        node.morph_start = s.morphStart;
        node.morph_end = s.morphEnd;
        // Find the closest dry/very-shallow terrain sample around this water tile.
        // The result is deliberately tile-local: it bends only a short coastal breaker
        // train, never the globally coherent FFT sea state offshore.
        const float centreX = s.originX + s.size * 0.5f;
        const float centreZ = s.originZ + s.size * 0.5f;
        const int centreIx = static_cast<int>(std::lround(centreX / land.GetTerrainGrid()));
        const int centreIz = static_cast<int>(std::lround(centreZ / land.GetTerrainGrid()));
        const int terrainRange = land.GetTerrainRange();
        float bestDistance = 1.0e9f;
        float dirX = 0.0f;
        float dirZ = 0.0f;
        float localDepth = 1000.0f;
        if (centreIx >= 0 && centreIz >= 0 && centreIx < terrainRange && centreIz < terrainRange)
        {
            localDepth = std::max(land.GetSeaLevel() - land.GetHeight(centreIz, centreIx), 0.0f);
            // The shore direction used to come from an 8-way ray search, which snapped every node
            // to one of 8 directions. Adjacent CDLOD nodes landed on different 45-degree buckets,
            // so the breaker train changed direction abruptly at node boundaries and left a
            // visible crease along the coast. A central-difference gradient of the seabed height
            // is continuous in the node centre, so neighbouring nodes now agree and the crease is
            // gone. Uphill (toward shallower water) is the direction the waves run.
            auto sampleHeight = [&](int x, int z)
            { return land.GetHeight(std::clamp(z, 0, terrainRange - 1), std::clamp(x, 0, terrainRange - 1)); };
            // A multi-cell radius smooths out single-cell terrain noise so the train direction is
            // stable rather than jittering along a rough seabed.
            constexpr int kGradientRadius = 4;
            const float gx =
                sampleHeight(centreIx + kGradientRadius, centreIz) - sampleHeight(centreIx - kGradientRadius, centreIz);
            const float gz =
                sampleHeight(centreIx, centreIz + kGradientRadius) - sampleHeight(centreIx, centreIz - kGradientRadius);
            const float gradientLength = std::sqrt(gx * gx + gz * gz);
            if (gradientLength > 1.0e-5f)
            {
                dirX = gx / gradientLength;
                dirZ = gz / gradientLength;
            }
            // Distance to land is still useful as a band, but it no longer sets the direction, so
            // the coarse 8-way probe is fine here.
            constexpr int directions[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
            for (const auto& d : directions)
            {
                for (int step = 2; step <= 64; step += 2)
                {
                    const int x = centreIx + d[0] * step;
                    const int z = centreIz + d[1] * step;
                    if (x < 0 || z < 0 || x >= terrainRange || z >= terrainRange)
                    {
                        break;
                    }
                    if (land.GetHeight(z, x) >= land.GetSeaLevel() - 0.15f)
                    {
                        const float dx = static_cast<float>(d[0] * step) * land.GetTerrainGrid();
                        const float dz = static_cast<float>(d[1] * step) * land.GetTerrainGrid();
                        bestDistance = std::min(bestDistance, std::sqrt(dx * dx + dz * dz));
                        break;
                    }
                }
            }
        }
        // Waves start feeling the bottom at roughly half their wavelength, which for open-ocean
        // swell is tens of metres — not the 8 m this used to use. Widening the band is what makes
        // a swell visibly run in toward the beach instead of appearing right at the waterline.
        // smoothstep rather than a linear clamp so the band's own edges do not become a second
        // crease at node boundaries.
        auto smoothFade = [](float edge0, float edge1, float x)
        {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const float shoreDistanceFade = smoothFade(220.0f, 20.0f, bestDistance);
        const float shallowFade = smoothFade(30.0f, 2.0f, localDepth);
        node.shore_direction = {dirX, dirZ};
        node.shore_factor = shoreDistanceFade * shallowFade;
        _selected.push_back(node);
    };
    // Reject nodes that sit entirely above the highest sea surface: their terrain
    // never floods, so no water is drawn there (a coarse ancestor that contains any
    // below-sea descendant still passes — its aggregate minY carries the low corner).
    auto belowSea = [&](const CdlodNode& n) { return n.minY <= _seaThreshold; };

    _selected.clear();
    SelectVisibleCdlod(_tree, _rootIndex, _numLevels, _ranges, _morphRegion, *camera, rx0, rz0, rx1, rz1, belowSea,
                       emit);

    // WTR-LOD diagnostic: the per-LOD mesh-density work only pays off if the selection
    // actually spreads across levels. With _baseMult = 8 and a 32-texel leaf, ranges[0]
    // may already exceed the draw distance, in which case every visible node is level 0
    // and coarse index buffers would buy nothing. Edge-triggered on the histogram so this
    // is a handful of log rows, not one per frame.
    {
        std::array<int, 16> histogram{};
        for (const WgrWaterNode& n : _selected)
        {
            histogram[std::min<size_t>(n.lod, histogram.size() - 1)]++;
        }

        // Publish the same numbers to the harness every frame, not on the
        // edge-triggered schedule the log below uses: a test that samples water
        // state must not depend on whether a log row happened to fire.
        {
            WaterFrameStats stats;
            stats.frame = GEngine ? static_cast<unsigned long long>(GEngine->GetFrameCounter()) : 0ull;

            // Same regions the log row below sums, but read every frame: a
            // baseline that only exists when a 2 s log budget happens to fire
            // is not a baseline.
            float regionMs[WGR_GPU_TIMER_WATER_REGION_COUNT] = {};
            const int regions = _engine.GetWaterGpuTimings(regionMs, WGR_GPU_TIMER_WATER_REGION_COUNT);
            if (regions > 0)
            {
                float total = 0.0f;
                for (int i = 0; i < regions; ++i)
                {
                    // Negative means the pass did not run this frame; excluded
                    // rather than summed, matching the logged total.
                    if (regionMs[i] > 0.0f)
                        total += regionMs[i];
                }
                stats.gpuMsTotal = total;
            }

            stats.nodes = static_cast<unsigned int>(_selected.size());
            stats.triangles = static_cast<unsigned int>(_selected.size()) * 96u * 96u * 2u;
            for (int i = 0; i < WaterFrameStats::kLodBuckets; ++i)
            {
                stats.lod[i] = static_cast<unsigned int>(histogram[static_cast<size_t>(i)]);
            }
            PublishWaterFrameStats(stats);
        }

        static std::array<int, 16> lastHistogram{};
        static float lastLoggedBaseMult = -1.0f;
        // The histogram changes on almost every frame the camera moves, so edge-triggering
        // alone produced ~25 rows/second. Rate-limit to one row every 2 s.
        static std::chrono::steady_clock::time_point lastLog{};
        const auto nowClock = std::chrono::steady_clock::now();
        const bool rateOk = (nowClock - lastLog) >= std::chrono::seconds(2);
        if (std::abs(lastLoggedBaseMult - _baseMult) > 0.001f)
        {
            LOG_INFO(Graphics,
                     "Water CDLOD config: leafSize={:.1f}m levels={} baseMult={:.1f} ratio={:.2f} range0={:.0f}m",
                     _leafSize, _numLevels, _baseMult, _lodRatio, _ranges.empty() ? 0.0f : _ranges[0]);
            lastLoggedBaseMult = _baseMult;
        }
        // WTR-002 timings, mirrored into the log. The ImGui Water tab already shows these,
        // but a screenshot cannot be diffed against a previous build and is easy to confuse
        // with a stale capture; a timestamped log row can. Same 2 s budget as the histogram.
        if (rateOk)
        {
            float ms[WGR_GPU_TIMER_WATER_REGION_COUNT] = {};
            const int count = _engine.GetWaterGpuTimings(ms, WGR_GPU_TIMER_WATER_REGION_COUNT);
            if (count > WGR_GPU_TIMER_WATER_DRAW)
            {
                // -1 means the pass never ran this frame ("n/a" in the tab); report it as 0
                // in the total but keep the raw value in the per-region text.
                float total = 0.0f;
                for (int i = 0; i < count; ++i)
                {
                    if (ms[i] > 0.0f)
                    {
                        total += ms[i];
                    }
                }
                LOG_INFO(
                    Graphics,
                    "Water GPU ms: evolve={:.3f} fftH={:.3f} fftV={:.3f} compose={:.3f} "
                    "draw={:.3f} foam={:.3f} interaction={:.3f} planar={:.3f} total={:.3f}",
                    ms[WGR_GPU_TIMER_SPECTRUM_EVOLVE], ms[WGR_GPU_TIMER_FFT_HORIZONTAL], ms[WGR_GPU_TIMER_FFT_VERTICAL],
                    ms[WGR_GPU_TIMER_FFT_COMPOSE], ms[WGR_GPU_TIMER_WATER_DRAW], ms[WGR_GPU_TIMER_FOAM],
                    ms[WGR_GPU_TIMER_INTERACTION],
                    std::max(ms[WGR_GPU_TIMER_PLANAR_SKY], 0.0f) + std::max(ms[WGR_GPU_TIMER_PLANAR_TERRAIN], 0.0f) +
                        std::max(ms[WGR_GPU_TIMER_PLANAR_OBJECTS], 0.0f) +
                        std::max(ms[WGR_GPU_TIMER_PLANAR_CLOUDS], 0.0f) + std::max(ms[WGR_GPU_TIMER_PLANAR_MIPS], 0.0f),
                    total);
                // LIT-010 / LIT-020 live in the same shared region array. Logged from here for
                // the reason the water rows are: a tab screenshot cannot be diffed against a
                // previous build, and a default-ON feature whose cost only exists on screen is
                // an unmeasured feature in practice. Piggybacks the same 2 s budget.
                float all[WGR_GPU_TIMER_REGION_COUNT] = {};
                const int n = _engine.GetWaterGpuTimings(all, WGR_GPU_TIMER_REGION_COUNT);
                if (n >= (int)WGR_GPU_TIMER_REGION_COUNT)
                {
                    const float ao = std::max(all[WGR_GPU_TIMER_GTAO_PREP], 0.0f) +
                                     std::max(all[WGR_GPU_TIMER_GTAO_COMPUTE], 0.0f) +
                                     std::max(all[WGR_GPU_TIMER_GTAO_BLUR], 0.0f);
                    const float sky = std::max(all[WGR_GPU_TIMER_INTERIOR_SKY_CULL], 0.0f) +
                                      std::max(all[WGR_GPU_TIMER_INTERIOR_SKY_DRAW], 0.0f);
                    LOG_INFO(Graphics,
                             "Lighting GPU ms: ao_prep={:.3f} ao_march={:.3f} ao_blur={:.3f} ao_total={:.3f} | "
                             "isky_cull={:.3f} isky_maps={:.3f} isky_total={:.3f} | frame={:.3f}",
                             all[WGR_GPU_TIMER_GTAO_PREP], all[WGR_GPU_TIMER_GTAO_COMPUTE],
                             all[WGR_GPU_TIMER_GTAO_BLUR], ao, all[WGR_GPU_TIMER_INTERIOR_SKY_CULL],
                             all[WGR_GPU_TIMER_INTERIOR_SKY_DRAW], sky, all[WGR_GPU_TIMER_FRAME_TOTAL]);
                }
            }
        }
        if (rateOk)
        {
            if (histogram != lastHistogram)
            {
                LOG_INFO(Graphics,
                         "Water CDLOD nodes: total={} lod0={} lod1={} lod2={} lod3={} lod4={} lod5+={} tris={}",
                         _selected.size(), histogram[0], histogram[1], histogram[2], histogram[3], histogram[4],
                         histogram[5] + histogram[6] + histogram[7] + histogram[8] + histogram[9],
                         _selected.size() * 96u * 96u * 2u);
                lastHistogram = histogram;
            }
            // Stamp the budget here, not inside the histogram branch: a static camera leaves
            // the histogram unchanged, which would hold rateOk true forever and let the
            // timings row above log every single frame.
            lastLog = nowClock;
        }
    }

    _engine.SubmitWater(_selected);
}

} // namespace Poseidon
