#pragma once

// Per-frame local-light gauges (roadmap Phase 3: "active-light count and lighting
// shader cost"). Written once per frame by EngineWgpu's light selection, read by the
// dev panel and the --capture-metrics sidecar. The COST half of the roadmap bullet is
// answered by A/B, not by a counter: WGR_MAX_ACTIVE_LIGHTS=0 removes every local light
// from the flat loop, and the GPU frame delta on a night scene is the number the
// Forward+ gate (B.1) reads. Storage follows the StreamingDiag pattern -- Poseidon::Dev
// side of the boundary, so no new link edge and no Engine vtable slot.

#include <cstdint>

namespace Poseidon::Dev
{

struct LightCounters
{
    // Last frame's values; single-writer (render prep, main thread), plain fields.
    uint32_t candidates = 0; // world lights considered this frame
    uint32_t selected = 0;   // survivors handed to the shader (<= WGR_MAX_LIGHTS and the env clamp)
    uint32_t cap = 0;        // the effective cap that clipped them (for the capture's context)
};

LightCounters& GLightCounters();

// LAMP-003: the street-lamp look lever.
//
// Measured on Everon at the cafe, 23:00, mission Intel night (asset-obs-001):
// with WGR_MAX_ACTIVE_LIGHTS=0 the village frame is mean luma 0.11 -- essentially
// black, stars and moon only. With the lamps on it is 105.6. So at night the street
// lamps are not *a* light source in an OFP village, they are the ONLY one, and the
// scene ambient contributes nothing worth measuring.
//
// That makes the lamp's reach the whole look. StreetLamp passes CfgNonAIVehicles
// `brightness` to LightPoint::SetBrightness, which is `_startAtten = 50 * coef`
// (Lights.hpp), and the shader (rust/src/shaders/lighting.wgsl, lights_contrib)
// gives every point light a FLAT FULL-BRIGHTNESS CORE out to startAtten and an
// inverse-square tail cut at 10x that. A lamp with brightness 1 therefore floods a
// 50 m sphere at full strength and still reaches 500 m -- which is why the whole
// village lights up flat instead of each lamp laying down a small pool.
//
// radiusScale multiplies the coef handed to SetBrightness (so it scales startAtten
// and the 10x cutoff with it); brightnessScale multiplies the lamp's diffuse and
// ambient colour. Both default to 1.0: the shipped look is NOT retuned here, the
// lever exists so the owner can pick the pool size and so any capture says which
// value produced it. `enabled` is the A/B (no lamp light at all) without needing
// the renderer-side WGR_MAX_ACTIVE_LIGHTS, which also kills vehicle lights.
//
// `generation` bumps on every change; StreetLamp::CreateLight rebuilds a light whose
// generation is stale, so a dev-panel slider takes effect on the next lamp simulation
// (or immediately, if the panel broadcasts Landscape::OnTimeSkipped).
struct LampLightSettings
{
    bool enabled = true;
    float radiusScale = 1.0f;
    float brightnessScale = 1.0f;
    unsigned generation = 0;
};

LampLightSettings& GLampLightSettings();

// Call after changing any field above.
void LampLightSettingsChanged();

} // namespace Poseidon::Dev
