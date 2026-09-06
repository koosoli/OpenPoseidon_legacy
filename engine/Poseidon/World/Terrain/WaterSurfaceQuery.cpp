#include <Poseidon/World/Terrain/WaterSurfaceQuery.hpp>

#include <Poseidon/Graphics/Core/Engine.hpp>

#include <algorithm>
#include <cmath>

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
    if (GEngine != nullptr)
    {
        const Engine::WaterSettings look = GEngine->GetWaterSettings();
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

    for (const WaveComponent& wave : Waves)
    {
        const float length = std::max(wave.length * lengthScale, 1.0f);
        const float amplitude = wave.amplitude * amplitudeScale * std::max(waveScale, 0.0f);
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
