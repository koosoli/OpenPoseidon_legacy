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
    // LAMP-004. The flat core is what made a village read as floodlight: lights_contrib is
    // full strength out to startAtten (= 50 * brightness metres, i.e. FIFTY) and only then
    // falls off inverse-square. Shrinking the core is what turns a lamp into a pool -- at
    // 0.18 the ground under a lamp is ~9x the far field, measured.
    float radiusScale = 0.18f;
    float brightnessScale = 1.0f;
    // The other half, and the one that actually reads as flood: the per-light AMBIENT term
    // is added with NO regard to which way a surface faces, so it lifts every wall, roof
    // and leaf in range uniformly. Cutting it keeps the pool on the ground and still leaves
    // the surroundings visible, which is what "a nice falloff" means here.
    // POSEIDON_LAMP_AMBIENT_SCALE=1 restores the authored value.
    float ambientScale = 0.30f;
    // LAMP-004. A street lamp was an omnidirectional point light, and the shader gives a
    // point light a FLAT full-strength core out to startAtten before any falloff -- twenty
    // 50 m white cores in one village, which is floodlight, not lamps. The renderer has
    // carried a spot path all along (WgrLight::dir.w, lights_contrib's `cone` term), it was
    // only ever used for headlights. A lamp points DOWN, so it should use it.
    //
    // POSEIDON_LAMP_CONE=1 turns it on. OFF by default, and the reason is measured:
    // LightReflector is a HEADLIGHT. Its cone is hardcoded at 12 degrees (MIN_INSIDE in
    // Lights.cpp) and its SetBrightness means something else again (200 * InvSqrt(...),
    // so ~258 m instead of 50). Twenty 12-degree beams straight down lit almost nothing --
    // a whole night village measured mean luma 0.3 against 100 with point lights -- and
    // they crowded the point lights out of the fixed light budget on score. A street lamp
    // wants a small core and a real falloff, not a searchlight.
    bool cone = false;
    // Sodium-vapour orange. The 2001 config asks for (0.90, 0.80, 0.60), a warm white --
    // which is a 1985 idea of a lamp, not what a period street lamp looked like. The owner
    // asked for orange; POSEIDON_LAMP_COLOR=r,g,b overrides, colorOverride=false keeps the
    // authored colour.
    bool colorOverride = true;
    float color[3] = {1.00f, 0.58f, 0.20f};
    // The cone CUTS: lights_contrib `continue`s outside it, so a pure spot leaves the
    // surroundings absolutely black, which is as wrong as the floodlight was. A second,
    // weak, omnidirectional light per lamp is the spill -- the surroundings stay lit, much
    // less than the pool under the lamp. 0 = pure spot. POSEIDON_LAMP_SPILL.
    float spill = 0.30f;
    // Radius of that spill light relative to the pool's. Small: it is the glow around a
    // lamp, not a second floodlight. POSEIDON_LAMP_SPILL_RADIUS.
    float spillRadius = 0.45f;
    unsigned generation = 0;
};

LampLightSettings& GLampLightSettings();

// LGT-001: local lights have NO occlusion term. lights_contrib does distance, a cone for
// spots, and N.L -- and nothing at all about what is between the light and the fragment.
// Street lamps therefore shine straight through walls, and that is the ENTIRE night
// illumination inside an Everon house: measured 17.95 mean luma standing in the cafe at
// 23:00 with the lamps on, and 0.00 with them off. The room is not dimly lit, it is lit by
// twenty lamps out in the street that the walls do not stop.
//
// Real occlusion means a shadow map per local light, which is a project. This is the cheap
// gate that answers the owner's actual complaint -- "if no light can hit the inside of a
// building at night it should not be bright": while the CAMERA is inside a room, a local
// light further than `indoorRadius` from it cannot plausibly be in that room, so it is
// scaled by `indoorLeak` (0 = dropped). A lamp you carry inside, or a glowing probe thrown
// through a door, is within the radius and still lights the room.
//
// It is a GATE, not shadows: it cannot let light in through a window, and it changes with
// the camera rather than per fragment. Both of those are the honest limits of doing this
// without a shadow map. Daylight is untouched -- the whole block only runs at night, since
// the light selection itself is gated on nightEffect.
struct IndoorLightGate
{
    bool enabled = true;      //!< POSEIDON_INDOOR_LIGHT_GATE=0 turns it off
    float indoorRadius = 7.0f;//!< POSEIDON_INDOOR_LIGHT_RADIUS: metres; inside this a light counts as "in here"
    float indoorLeak = 0.0f;  //!< POSEIDON_INDOOR_LIGHT_LEAK: 0..1 of an outside light that still reaches in
    // Counters, so "the room went dark" can be told from "the query never ran".
    long long cameraQueries = 0;
    long long framesIndoors = 0;
    long long lightsDropped = 0;
};
IndoorLightGate& GIndoorLightGate();

// Call after changing any field above.
void LampLightSettingsChanged();

} // namespace Poseidon::Dev
