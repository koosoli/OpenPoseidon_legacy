#pragma once
#include <array>

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
// LGT-017: the shape of a spot light's beam, as an exponent on its axis-to-rim ramp.
// 1 is a plain smoothstep -- correct, and too dim, because a headlight used to be FLAT out
// to its authored inner angle and everything was tuned against that. Below 1 restores a hot
// centre while the rim still arrives at zero with a zero derivative, which is what stops the
// beam reading as a disc with an edge on a wall. POSEIDON_SPOT_BEAM_SHAPE overrides.
struct SpotBeamSettings
{
    float shape = 0.35f;
};
SpotBeamSettings& GSpotBeam();

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
    // ON as of LGT-012. It was off because LightReflector's beam measured as a 12 degree
    // searchlight that lit nothing -- but that was the SHADER's hardcoded cone, not the
    // light's, and LGT-011 fixed it: a lamp now opens to the 36 degrees it actually declares.
    // A cone is also the only kind of local light that can cast a shadow (a point needs six
    // maps), which is what the owner asked for: "strassenlaternen machen noch keine schatten".
    // BACK OFF (2026-09-07, second smoke test). Turning lamps into cones to make them
    // shadow-capable changed what they LIGHT: a cone only lights inside its cone, so from
    // any distance outside the beam the lamp went dark. The owner: "wenn ich weiter weg bin
    // von einem strassen licht ... ist das licht nicht mehr zu sehen, das ist eine
    // regression". He is right, and the lesson is the same one as the indoor light gate:
    // do not change what a light DOES in order to make a feature possible. Point lights need
    // shadows of their own (six faces); that is the work, not this.
    bool cone = false;
    // Sodium-vapour orange. The 2001 config asks for (0.90, 0.80, 0.60), a warm white --
    // which is a 1985 idea of a lamp, not what a period street lamp looked like. The owner
    // asked for orange; POSEIDON_LAMP_COLOR=r,g,b overrides, colorOverride=false keeps the
    // authored colour.
    bool colorOverride = true;
    bool useColorTemperature = false;
    float colorTemperature = 2700.0f;
    float color[3] = {1.00f, 0.58f, 0.20f};
    // The cone CUTS: lights_contrib `continue`s outside it, so a pure spot leaves the
    // surroundings absolutely black, which is as wrong as the floodlight was. A second,
    // weak, omnidirectional light per lamp is the spill -- the surroundings stay lit, much
    // less than the pool under the lamp. 0 = pure spot. POSEIDON_LAMP_SPILL.
    float spill = 0.80f;
    // Radius of that spill light relative to the pool's. Small: it is the glow around a
    // lamp, not a second floodlight. POSEIDON_LAMP_SPILL_RADIUS.
    float spillRadius = 5.56f;
    // LGT-022: the spill's own AMBIENT, separately from the pool's. The ambient term is added
    // with no regard to which way a surface faces, so it lifts every wall, roof and leaf
    // uniformly -- that is precisely what reads as floodlight. Zero here lets the spill be
    // strong enough to carry the far field (which is what you see flying over a lit town)
    // while staying directional, so near the lamp it still looks like a pool and a wall facing
    // away stays dark. POSEIDON_LAMP_SPILL_AMBIENT.
    float spillAmbient = 0.0f;
    unsigned generation = 0;
};

LampLightSettings& GLampLightSettings();

// LGT-010: shadows for LOCAL lights -- headlights, street lamps, a glowing physics probe.
// Until now the renderer had shadow maps for the SUN only (four cascade layers), and
// lights_contrib had no occlusion term at all, so every local light shone through walls.
//
// A shadow view for a spot is one more layer in the depth array the cascades already live
// in, and one more perspective matrix. Points would need six faces each and are not in this
// slice. See design notes
struct LocalShadowSettings
{
    // ON as of LGT-014. It shipped off under the LGT-010 rule "a lever defaulted off is
    // acceptable, a changed default is not" -- but the owner has now asked for local-light
    // shadows in three consecutive messages, so off IS the regression. The lever stays:
    // POSEIDON_LOCAL_SHADOWS=0, or the Lighting tab.
    bool enabled = true;   //!< POSEIDON_LOCAL_SHADOWS=0 turns local light shadows off.
    int maxLights = 4;      //!< POSEIDON_LOCAL_SHADOW_LIGHTS: how many get shadow views at once.
                            //!< LGT-015: a spot costs one view of sixteen, a point costs six
                            //!< (its cube), so three lights is the point where the view budget
                            //!< and this one run out together for a street full of lamps.
    // LGT-016: a caster whose ORIGIN is closer to the bulb than this is not submitted to
    // that light's shadow views. The light is INSIDE it, and a closed mesh around a light
    // occludes every direction at once -- which is not a subtle artefact: the first cube
    // capture had six faces each filled edge to edge by the emitting sphere's own shell, and
    // the whole village went black. A self-illuminating object is the obvious case; a lamp
    // is safe because the post model's origin is metres from the bulb.
    float selfRadius = 1.2f;
    // The near plane of the light's own frustum, metres. Perspective depth precision is
    // spent between the near plane and a few times its distance, so this is NOT "as small as
    // possible": 1 cm on a 40 m beam is shadow acne.
    float nearD = 0.35f;    //!< POSEIDON_LOCAL_SHADOW_NEAR
    // Shadow distance multiplier, not necessarily the lighting shader's actual cutoff.
    // The matched-reach path takes at least the light's own multiplier, still capped.
    // WGR_MATCH_LOCAL_SHADOW_REACH=0 restores the legacy multiplier-only control.
    float rangeScale = 6.0f;//!< POSEIDON_LOCAL_SHADOW_RANGE
    // ...but never past this, whatever the light claims. `startAtten` means different things
    // to different light kinds -- LightPoint makes it 50 x brightness while LightReflector
    // makes it 200 / sqrt(brightness), which measured 368 m for a hand-held torch. A shadow
    // map that admits geometry over kilometres also increases caster workload. The cap
    // bounds that work and the depth interval, but can truncate still-visible shadows.
    // The owner's report: "die schatten von fahrzeugen gehen nicht weit genug". 90 m was
    // chosen to keep the depth precision usable, not from anything a headlight needs; a beam
    // reaches further than that down a road. Raised, and still a lever, because the trade is
    // real. Perspective angular texel density does not simply halve with the far plane.
    float rangeMax = 180.0f; //!< POSEIDON_LOCAL_SHADOW_RANGE_MAX, metres
    float darkness = 1.0f;  //!< POSEIDON_LOCAL_SHADOW_DARKNESS: 0 = no occlusion, 1 = full
    // LGT-026 -- a street lamp does not move, and neither does the house beside it, so its six
    // cube faces were being recomputed sixty times a second to produce an identical depth map.
    // With the budget at four lights that is 24 depth passes a frame of almost entirely wasted
    // work, and it is the difference between "four lamps can have shadows" and "every lamp near
    // you can". A view's tile is rendered once and re-used until its light moves or changes
    // shape, a caster inside its volume moves, the retained set changes, or the depth target's
    // shape does. The CAMERA is deliberately not one of those: the depth map's contents are
    // camera-independent (the camera-relative light matrix and the camera-relative caster worlds
    // cancel), and only the per-camera LOOKUP matrix is camera-relative -- that is rebuilt from
    // scratch every frame anyway, being a matrix multiply rather than a depth pass.
    bool cacheStatic = true;   //!< POSEIDON_LOCAL_SHADOW_CACHE=0 renders every view every frame.
    // The switch that makes a suspected stale tile a one-keypress question rather than an
    // investigation. Getting invalidation wrong is worse than not caching -- a stale shadow is a
    // shadow in the WRONG PLACE, which reads as a rendering bug and not as a performance choice
    // -- so there has to be a way to rule it in or out without a rebuild or a restart.
    bool forceRefresh = false; //!< POSEIDON_LOCAL_SHADOW_FORCE_REFRESH=1: re-render every view.
    // Counters that separate "no light qualified" from "the pass never ran".
    long long framesWithShadow = 0;
    long long lightsShadowed = 0;
    long long candidatesSeen = 0;
    // LGT-026 -- last frame's cache verdict, for the Lighting tab and the stats log line.
    int viewsRendered = 0;
    int viewsCached = 0;
};
LocalShadowSettings& GLocalShadowSettings();

// LGT-012: the VOLUME cone a reflector draws around itself -- the translucent shaft you see
// coming out of a headlight, not the pool it casts. The owner: "ich denke den lichtkegel von
// headlights sollte man ueberhaupt nicht sehen". It is a 2001 stand-in for volumetric
// scattering and it reads as a solid grey wedge next to lighting that now has real
// occlusion, so it is off. POSEIDON_LIGHT_VOLUME_CONE=1 brings it back.
struct LightVolumeConeSettings
{
    bool enabled = false;
};
LightVolumeConeSettings& GLightVolumeCone();


// Call after changing any field above.
void LampLightSettingsChanged();
void ResetLightingSettings();
std::array<float, 3> LampTemperatureColor(float kelvin);

} // namespace Poseidon::Dev
