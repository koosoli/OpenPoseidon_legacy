#pragma once

namespace Poseidon
{

// Tier-A CPU predictor for gameplay. It deliberately does not sample GPU FFT
// textures, so it is deterministic and is only an approximation of rendered water.
struct WaterSurfaceSample
{
    float height;
    float normalX;
    float normalY;
    float normalZ;
    float velocityX;
    float velocityZ;
    float roughness;
};

// Evaluates a compact, stable open-ocean spectrum at world X/Z. seaLevel is supplied by the
// landscape.
//
// The band amplitudes, animation rate and wavelengths are scaled by the live renderer water look
// (amplitude / speed / scale) so a unit floating on the sea rides the same sea state that is being
// drawn. Previously every constant here was hardcoded — a fixed 0.08 sea state giving 16-30 cm
// components, on the superseded 48/144/432/1296 m cascade bands — so buoyancy bobbed by a couple of
// centimetres no matter how rough the rendered ocean was, and never agreed with it.
//
// It remains an approximation: this is a six-component Gerstner sum, not the GPU FFT field, and it
// deliberately samples no GPU texture so it stays deterministic and available on the server. It
// matches the rendered sea in scale, period and roughness rather than crest for crest.
WaterSurfaceSample QueryWaterSurface(float worldX, float worldZ, float time, float seaLevel);

// WRL-003 / WRL-006b — the same predictor for a bounded body: `surfaceLevel` is the body's local
// mean surface at (worldX, worldZ) and `waveScale` scales every component amplitude (a sheltered
// lake carries ripples, not swell). waveScale 1.0 reproduces QueryWaterSurface exactly.
WaterSurfaceSample QueryWaterSurfaceScaled(float worldX, float worldZ, float time, float surfaceLevel,
                                           float waveScale);

// A wave trough can expose a point that is still below mean sea level. Keep the
// contact record available so stable amphibious solvers can avoid losing a
// whole buoyancy cycle; legacy consumers still reject its negative `under`.
inline bool ShouldReportWaterContact(float under, float underFlat)
{
    return under > 0.0f || underFlat > 0.0f;
}

} // namespace Poseidon
