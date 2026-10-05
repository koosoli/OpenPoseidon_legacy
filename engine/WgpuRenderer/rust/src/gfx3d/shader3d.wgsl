// Unlit textured 3D, plain (vs_main) and GPU-skinned (vs_skinned) variants
// sharing fs_main (and, for OnSurface road/decal pipelines on the plain path, its
// frag_depth-writing twin fs_surface). The two group(1) declarations coexist because each entry
// point statically uses only one (binding collisions are validated per entry
// point): the plain pipeline binds `object`, the skinned pipeline binds the
// bone `palette` (from the skin module). Shadowing is the shared cascade kernel.

#import frame::{frame, reverse_z, fog_factor, atmo_applied, terrain_sun_shadow, apply_fog, apply_fog_terrain, veg_sway_offset, gtao_ao, matdbg_view, matdbg_flag, MATDBG_VIEW_FULL, MATDBG_VIEW_BASE_COLOR, MATDBG_VIEW_NORMAL, MATDBG_VIEW_AMBIENT_SHADOW, MATDBG_VIEW_SPECULAR_GLOSS, MATDBG_VIEW_UV, MATDBG_VIEW_LIGHTING, MATDBG_DISABLE_FRESNEL_ENV}
#import shadow::shadow_strength
#import frame::{surface_rays, cloud_shadow_tex, terrain_shadow_samp, terrain_shadow_map}
#import frame::monitor_scene
#import skin::{skin_pos, skin_normal}
#import lighting::lights_contrib
#import color::srgb_to_linear
#import boot_relief::{boot_relief_height, boot_relief_normal, boot_relief_parallax, boot_relief_multiplier, boot_relief_composite}
#import gbuffer::{oct_encode, a2c_coverage, mip_alpha_boost}
// Shared fragment shading, also used by the GPU-driven indirect path (gpu_driven.wgsl).
#import shading::{shade, ShadeMaterial, surface_normal, object_snow_coverage, object_snow_normal}
#import tree_snow::{tree_snow_height, tree_snow_coverage}
// Group(4) terrain heightmap + surface_y, shared with the shadow depth pass.
// surface_grad tilts conformed veg normals to follow the ground slope (color pass only).
#import conform::{surface_y, surface_grad, surface_y_raster, hm_params, snow_depth, rigid_ground_fragment, cave_daylight}

// BEGIN_MUZZLE_FLASH_HELPERS (actual helper exercised by the GPU probe)
fn stock_muzzle_flash_radiance(srgb: vec3<f32>, linear: f32) -> vec3<f32> {
    let bounded = clamp(srgb, vec3<f32>(0.0), vec3<f32>(1.0));
    return select(bounded, srgb_to_linear(bounded) * 1.25, linear > 0.5);
}
// END_MUZZLE_FLASH_HELPERS

// BEGIN_UNIFORM_CLOTH_HELPERS (also exercised directly by the shader probe)
fn uniform_cloth_rect(uv: vec2<f32>, lo: vec2<f32>, hi: vec2<f32>, pad: vec2<f32>) -> f32 {
    let edge = min(uv - lo, hi - uv) / max(pad, vec2<f32>(0.000001));
    return clamp(min(edge.x, edge.y), 0.0, 1.0);
}

fn uniform_cloth_mask(uv: vec2<f32>, encoded: f32, footprint: vec2<f32>) -> f32 {
    if (encoded <= 1.0) { return 1.0; }
    let pad = max(footprint * 2.0, vec2<f32>(2.0 / 1024.0));
    // Verified 1024px stock merged atlases mix cloth with skin/boots/helmets
    // and baked whole-body far LODs. Only the main cloth islands receive wetness.
    // West hand-photo islands: x=256..304,456..560,720..772,y=220..344;
    // East: x=508..564,712..820,972..1024,y=246..350.
    // Expand exclusions as minification increases, never select skin
    // by colour. Articulated hands/faces bind separate xicht_a, never admitted.
    var mask = uniform_cloth_rect(uv, vec2<f32>(0.0), vec2<f32>(1.0, 0.50), pad);
    if (encoded < 4.0) {
        let hand_a = uniform_cloth_rect(uv, vec2<f32>(0.245, 0.210) - pad * 2.0,
                                       vec2<f32>(0.300, 0.340) + pad * 2.0, pad);
        let hand_b = uniform_cloth_rect(uv, vec2<f32>(0.440, 0.210) - pad * 2.0,
                                       vec2<f32>(0.550, 0.340) + pad * 2.0, pad);
        let hand_c = uniform_cloth_rect(uv, vec2<f32>(0.700, 0.210) - pad * 2.0,
                                       vec2<f32>(0.760, 0.340) + pad * 2.0, pad);
        let hands = max(hand_a, max(hand_b, hand_c));
        let helmet = uniform_cloth_rect(uv, vec2<f32>(0.750, 0.375) - pad * 2.0,
                                       vec2<f32>(1.0, 0.50) + pad * 2.0, pad);
        mask *= (1.0 - hands) * (1.0 - helmet);
    } else {
        let hand_a = uniform_cloth_rect(uv, vec2<f32>(0.490, 0.235) - pad * 2.0,
                                       vec2<f32>(0.555, 0.345) + pad * 2.0, pad);
        let hand_b = uniform_cloth_rect(uv, vec2<f32>(0.685, 0.235) - pad * 2.0,
                                       vec2<f32>(0.810, 0.345) + pad * 2.0, pad);
        let hand_c = uniform_cloth_rect(uv, vec2<f32>(0.945, 0.235) - pad * 2.0,
                                       vec2<f32>(1.0, 0.345) + pad * 2.0, pad);
        let hands = max(hand_a, max(hand_b, hand_c));
        let boots = uniform_cloth_rect(uv, vec2<f32>(0.25, 0.375) - pad * 2.0,
                                      vec2<f32>(0.50, 0.50) + pad * 2.0, pad);
        let boot_tops = uniform_cloth_rect(uv, vec2<f32>(0.50, 0.460) - pad * 2.0,
                                          vec2<f32>(1.0, 0.50) + pad * 2.0, pad);
        mask *= (1.0 - hands) * (1.0 - max(boots, boot_tops));
    }
    return mask;
}

fn uniform_cloth_wetness(encoded: f32, uv: vec2<f32>, footprint: vec2<f32>) -> f32 {
    if (!(encoded > 0.0)) { return 0.0; }
    var wetness = encoded;
    if (encoded >= 4.0) { wetness -= 4.0; }
    else if (encoded >= 2.0) { wetness -= 2.0; }
    return clamp(wetness, 0.0, 1.0) * uniform_cloth_mask(uv, encoded, footprint);
}

fn uniform_cloth_color(rgb: vec3<f32>, encoded: f32, uv: vec2<f32>, footprint: vec2<f32>) -> vec3<f32> {
    let wet = uniform_cloth_wetness(encoded, uv, footprint);
    if (wet <= 0.0) { return rgb; }
    // A visibly soaked textile retains 45% of its dry linear reflectance.
    // The smooth early absorption response makes accumulated dampness visible
    // before saturation: w=.2 retains .802, w=.38 retains .6614. The original
    // linear22% response at w=.38 changed sRGB by only about4%, easily hidden
    // by ordinary weather/exposure. Hue and authored detail remain unchanged;
    // The shared lighting adds a separate broad, bounded wet-textile sheen.
    // This absorption helper preserves authored hue and detail.
    let absorption = wet * (2.0 - wet);
    let c = srgb_to_linear(rgb) * (1.0 - 0.55 * absorption);
    return select(1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055, c * 12.92,
                  c <= vec3<f32>(0.0031308));
}
// END_UNIFORM_CLOTH_HELPERS

struct Object {
    world: mat4x4<f32>,
    // Terrain-conform plane, published per instance by the CPU (ForestPlain).
    // When conform2.z (mode) > 0 the vertex shader conforms this instance to the ground
    // exactly like ForestPlain::Animate's two-triangle bilinear fit, so the shared
    // forest mesh can be uploaded ONCE undeformed instead of rewritten per instance.
    conform0: vec4<f32>,   // inv_land_grid, -xf, -zf, bias(=BoundingCenter().y)
    conform1: vec4<f32>,   // y00, y10, d1000, d0100
    conform2: vec4<f32>,   // d1011, d0111, mode(0=none,1=forest), _pad
};

// Per-draw material lighting, folded on the CPU exactly like GL33's
// UploadVSMaterialConstants (raw sun colour x material, sun-enable already in the
// sun terms). Bound at group(1)/binding(1) for BOTH the plain and skinned
// pipelines — binding(0) is `object` (plain) or the skin module's `palette`
// (skinned), so the material coexists with either. Only rgb is read.
struct Material {
    emissive: vec4<f32>,
    sun_ambient: vec4<f32>,
    sun_diffuse: vec4<f32>,
    // Modulation for the frame-global point/spot lights (GL33's matDif/matAmb).
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    // Sun-only Blinn-Phong highlight (GL33's c18): rgb = sun diffuse x material
    // specular (sun-enable folded in), w = power. Added per-fragment when w > 0.
    specular: vec4<f32>,
    // x = bindless Stage1 NOHQ texture/sampler, y = MaterialDebug View,
    // z = Stage1 enabled. NOHQ is decoded strictly as X=A, Y=G, Z reconstructed.
    // w retains the small BITFIELD (see MATFLAG_* below), not a bool.
    normal_map: vec4<f32>,
    local_specular: vec4<f32>, // w = admitted uniform wetness/mask category (0..1, 2..3, 4..5)
};

// Material.normal_map.w bits.
const MATFLAG_INVERT_NORMAL_Y: u32 = 1u;
// DZ-003: this draw's material declared itself reflective (WGR_DRAW3D_REFLECTIVE) --
// DayZ's CalmWater family, and nothing else.
const MATFLAG_REFLECTIVE: u32 = 2u;
// WGR_DRAW3D_NIGHT_EMITTER: a recognised light fixture, exempt from the emissive_night fade.
const MATFLAG_NIGHT_EMITTER: u32 = 4u;
// MAT-051 (WGR_DRAW3D_GLASS): classic glass pane. Output alpha is floored so the pane still
// reads at normal incidence, where Fresnel gives ~F0 and an OFP-era 11%-alpha texture would
// otherwise vanish from the pilot seat. 0.18 is the retail tint's own average (46/255).
const MATFLAG_GLASS: u32 = 8u;
const GLASS_MIN_ALPHA: f32 = 0.18;
// MAT-052 second half (WGR_DRAW3D_COCKPIT): a first-person cockpit draw. shade() takes the
// FLAT scene ambient for these instead of the sky-dome irradiance -- an interior plate lit
// by the full outdoor sky washes to near-sky brightness and reads as a TRANSPARENT cockpit
// against the sky (fine against dark terrain), which is exactly the owner's report on the
// stock T72 and UH-60 alike.
const MATFLAG_COCKPIT: u32 = 16u;
const MATFLAG_NORMAL_RG: u32 = 128u;
const MATFLAG_SNOW_RECEIVER: u32 = 2048u;
const MATFLAG_GROUND_RECEIVER: u32 = 4096u;
const MATFLAG_BOOT_RELIEF: u32 = 16384u;

fn material_flags(m: Material) -> u32 {
    return u32(m.normal_map.w + 0.5);
}

// Per-draw world matrix + material as read-only storage arrays, indexed by
// @builtin(instance_index) (fed as the draw's base_instance). One upload per
// frame, group(1) bound once — no per-draw dynamic offsets. The skinned pipeline
// binds the bone `palette` (from the skin module) at binding(0) instead of
// `objects`; each entry point statically uses only one, so both coexist.
@group(1) @binding(0) var<storage, read> objects: array<Object>;   // plain pipeline
@group(1) @binding(1) var<storage, read> materials: array<Material>;
// DZ-003 -- Sky's reflection environment map (equirect, LINEAR radiance) and its sampler
// (U wraps across the azimuth seam, V clamps at the poles). The same view water reflects.
// Sampled only by draws flagged MATFLAG_REFLECTIVE; a 1x1 dummy is bound until Sky lends
// the real view, so the binding is always valid.
@group(1) @binding(2) var sky_env: texture_2d<f32>;
@group(1) @binding(3) var sky_env_samp: sampler;

// AST-012A -- GPU MIP FEEDBACK, the per-draw path's half. One word per bindless texture slot
// holding the FINEST mip any fragment asked for, atomicMax-ed so a plain clear-to-zero is the
// per-frame reset and 0 reads as "not sampled". The identical buffer the GPU-driven path
// writes at its own group(1) binding 8: both paths index the same bindless array, so both must
// report into the same table or the answer is only true of whichever path happened to draw.
//
// See gpu_driven.wgsl's copy of this for the full argument; the constants must agree with it
// and with mip_feedback.rs.
@group(1) @binding(4) var<storage, read_write> mip_feedback: array<atomic<u32>>;

const MIPFB_HEADER_WORDS: u32 = 8u;
const MIPFB_CODE_BASE: u32 = 16u;

// Record the mip this fragment wanted of `slot`. `duvx`/`duvy` are the caller's UV derivatives,
// taken in uniform control flow -- derivative builtins are illegal inside the group test, and
// the test is what keeps this to a fraction of the fragments: only slots congruent to
// frame.renscale.z modulo frame.renscale.w write, so one frame covers one group's worth of the
// texture table. renscale.w == 0 disables it; slot 0 is the white bindless fallback.
// REN-OBJ-003: the per-draw path's share of the object fragment census (words 0/1; it has no
// prepass twin of its own). Same buffer, same words as gpu_driven.wgsl.
// REN-ATM-001 adds words 4..7 (atmosphere by family) here too -- see the word map in
// gpu_driven.wgsl. Both paths write the same words on purpose: the question they answer is
// per-family, not per-path, so the two must sum rather than be told apart.
fn census(word: u32) {
    if (count_fragments > 0.5) {
        atomicAdd(&mip_feedback[word], 1u);
    }
}

fn mip_feedback_note(slot: u32, duvx: vec2<f32>, duvy: vec2<f32>) {
    let groups = u32(max(frame.renscale.w, 0.0));
    if (groups == 0u || slot == 0u || (slot % groups) != u32(max(frame.renscale.z, 0.0))) {
        return;
    }
    let dims = vec2<f32>(textureDimensions(textures[slot], 0));
    let dx = duvx * dims;
    let dy = duvy * dims;
    let rho = max(dot(dx, dx), dot(dy, dy));
    var lod = 0.0;
    if (rho > 0.0) {
        lod = 0.5 * log2(rho) + frame.renscale.x;
    }
    let level = u32(clamp(floor(lod), 0.0, f32(MIPFB_CODE_BASE) - 1.0));
    atomicMax(&mip_feedback[MIPFB_HEADER_WORDS + slot], MIPFB_CODE_BASE - level);
}

// Equirect lookup into the sky reflection env map. Copied from water.wgsl's sky_env_sample
// so both consumers agree with fs_sky_env's convention in sky.wgsl: u = azimuth
// (atan2(z, x)/2pi + 0.5, U-wrapped), v = 0 at zenith .. 1 at nadir (acos(y)/pi). `dir` is
// a world-space direction; the result is LINEAR radiance.
const ENV_TWO_PI: f32 = 6.28318530718;
fn sky_env_sample(dir: vec3<f32>) -> vec3<f32> {
    let u = 0.5 + atan2(dir.z, dir.x) / ENV_TWO_PI;
    let v = acos(clamp(dir.y, -1.0, 1.0)) / (ENV_TWO_PI * 0.5);
    return textureSampleLevel(sky_env, sky_env_samp, vec2<f32>(u, v), 0.0).rgb;
}

// Schlick with the air/water normal-incidence reflectance; see gpu_driven.wgsl's twin.
const ENV_F0: f32 = 0.02;
fn env_fresnel(cos_view_normal: f32) -> f32 {
    let c = clamp(1.0 - cos_view_normal, 0.0, 1.0);
    let c2 = c * c;
    return ENV_F0 + (1.0 - ENV_F0) * (c2 * c2 * c);
}

// DZ-005 -- the world-space water ripple. Verbatim twin of gpu_driven.wgsl's copy, which
// carries the full derivation; the short version is that DayZ water cannot be normal-mapped
// through its mesh uv (all 185 Enoch pond meshes carry UV (0,0) at every vertex; the 804 river
// meshes are ~360x anisotropic), and its `.emat` addresses the map in WORLD space instead --
// `NormalMapping 0.1` is one tile per 10 m.
const WATER_RIPPLE_PERIOD_A: f32 = 10.0;
const WATER_RIPPLE_PERIOD_B: f32 = 3.3;
const WATER_RIPPLE_POWER_A: f32 = 1.55;
const WATER_RIPPLE_POWER_B: f32 = 0.80;

// The still-water drift; twin of gpu_driven.wgsl's, which carries the reasoning. Two octaves
// crawling in different directions at different speeds, slowly -- a pond ripples, it does not
// flow. Negative because sampling at `uv - d` shifts the pattern by `+d`.
const WATER_DRIFT_DIR_A: vec2<f32> = vec2<f32>(0.8, 0.6);
const WATER_DRIFT_DIR_B: vec2<f32> = vec2<f32>(-0.5547, 0.8321);
const WATER_DRIFT_SPEED_A: f32 = 0.12;
const WATER_DRIFT_SPEED_B: f32 = 0.075;
fn water_drift(dir: vec2<f32>, speed: f32, period: f32, t: f32) -> vec2<f32> {
    return -dir * (speed * t / period);
}

// One octave, returned as a world-xz slope. `samplers[0]` is linear + REPEAT on both axes; the
// material's own sampler may clamp, which on a world-space uv smears one edge texel across a
// whole lake. textureSampleGrad takes the world-position derivatives from the caller so the
// call stays legal inside the reflective branch and still picks a correct mip.
//
// The decode is s.rg, NOT the OFP NOHQ (X in alpha, Y in green) convention used above: these
// maps are DXT1 with a constant 255 alpha and a unit normal in RGB (measured on
// enoch_pond_nohq.edds / enoch_river_nohq.edds, which are byte-identical).
fn water_octave(slot: u32, xz: vec2<f32>, cam_xz: vec2<f32>, dxz_dx: vec2<f32>, dxz_dy: vec2<f32>,
                period: f32, drift: vec2<f32>) -> vec2<f32> {
    let inv = 1.0 / period;
    // fract() on the camera's share keeps the uv inside f32's useful mantissa at 12.8 km out.
    let uv = xz * inv + fract(cam_xz * inv) + drift;
    let s = textureSampleGrad(textures[slot], samplers[0], uv, dxz_dx * inv, dxz_dy * inv);
    return s.rg * 2.0 - vec2<f32>(1.0);
}

// The rippled shading normal for a reflective (CalmWater) fragment. `drift` is the per-octave
// uv scroll -- zero for a still surface, the flow term for a river.
fn water_ripple_normal(slot: u32, geometric: vec3<f32>, world_pos: vec3<f32>,
                       dwx: vec3<f32>, dwy: vec3<f32>, drift_a: vec2<f32>, drift_b: vec2<f32>,
                       strength: f32) -> vec3<f32> {
    let xz = vec2<f32>(world_pos.x, world_pos.z);
    let cam_xz = vec2<f32>(frame.cam_pos.x, frame.cam_pos.z);
    let dxz_dx = vec2<f32>(dwx.x, dwx.z);
    let dxz_dy = vec2<f32>(dwy.x, dwy.z);
    var slope = water_octave(slot, xz, cam_xz, dxz_dx, dxz_dy, WATER_RIPPLE_PERIOD_A, drift_a)
                * WATER_RIPPLE_POWER_A;
    slope = slope + water_octave(slot, xz, cam_xz, dxz_dx, dxz_dy, WATER_RIPPLE_PERIOD_B, drift_b)
                    * WATER_RIPPLE_POWER_B;
    slope = slope * strength;
    // Horizontal quads, so the map's tangent X/Y are world X/Z; water.wgsl's convention turns
    // an xz slope into normalize(vec3(-sx, 1, -sy)). The up axis keeps the geometric sign.
    let up = select(-1.0, 1.0, geometric.y >= 0.0);
    return normalize(vec3<f32>(-slope.x, up, -slope.y));
}

// Fresnel-weighted sky reflection. `world_pos` is CAMERA-RELATIVE (camera at the origin),
// `fog` is frame::fog_factor (1 = clear). Verbatim twin of gpu_driven.wgsl's env_reflection
// -- the two paths must not disagree about what a reflective surface looks like.
fn env_reflection(shaded: vec3<f32>, normal: vec3<f32>, world_pos: vec3<f32>, fog: f32) -> vec3<f32> {
    let view_dir = normalize(-world_pos);
    // Face the normal at the viewer first: a quad whose winding puts its normal downward
    // would otherwise reflect into the nadir and read as a dark blotch.
    let n = select(-normal, normal, dot(normal, view_dir) >= 0.0);
    let cos_view_normal = clamp(dot(n, view_dir), 0.0, 1.0);
    let env = sky_env_sample(reflect(-view_dir, n));
    // Sinkhole W1b: a cave mirrors no sky -- the reflected environment is daylight, faded like the rest.
    let daylight = cave_daylight(world_pos + frame.cam_pos.xyz);
    return mix(shaded, env, env_fresnel(cos_view_normal) * clamp(fog, 0.0, 1.0) * daylight);
}
// Bindless object textures + the 8-variant sampler array, bound once for the whole
// lit-mesh + prepass. Each draw's texture/sampler index is packed per-instance into
// material.emissive.w (docs/bindless-textures-plan.md): (tex_slot << 3) | sampler.
@group(2) @binding(0) var textures: binding_array<texture_2d<f32>>;
@group(3) @binding(0) var samplers: binding_array<sampler, 8>;

// Baked per-pipeline (pipeline-overridable constants): the alpha-test cutout
// threshold and whether this is a shadow-darken pipeline. Keeping them out of a
// per-draw binding avoids a 5th bind group (wgpu's default maxBindGroups is 4).
override alpha_ref: f32 = 0.0;   // discard fragments with alpha below this (0 = off)
override weather_depth: f32 = 0.0; // dedicated physical map only; ordinary paths unchanged
override is_shadow: f32 = 0.0;   // 1 = output black + shadow-strength alpha
// 1 = this cutout pipeline uses alpha-to-coverage (MSAA foliage): the fragment emits a
// sharpened coverage alpha instead of a hard discard, and the pipeline has
// alpha_to_coverage_enabled. Set only on cutout (alpha_ref > 0), non-shadow pipelines under
// MSAA; the prepass twin (fs_prepass_a2c) emits the same coverage so the masks match.
override a2c: f32 = 0.0;
override foliage_screen_ao: f32 = 0.0; // REN-OBJ-001, as in gpu_driven.wgsl
override count_fragments: f32 = 0.0;   // REN-OBJ-003, as in gpu_driven.wgsl (colour pass only here)
// 1 = an alpha-blended (glass) pipeline: the shared shading damps its diffuse sky-irradiance
// ambient so transparent cockpit canopies don't wash out to the sky (and crash auto-exposure).
override translucent: f32 = 0.0;

// Brightness a fully terrain-shadowed alpha-tested surface (foliage cutout) keeps.
// Dense canopy self-occludes its sky ambient — which the world-space terrain mask
// can't model and foliage materials inflate for the sunlit look — so shadowed
// leaves stay too bright under the ambient-preserving model that suits solid
// ground/decals. This extra multiply darkens only alpha-tested foliage in terrain
// shadow toward the close-up CSM look. 1 = off (no extra darkening).
override foliage_shadow_ao: f32 = 0.35;
// Decal/overlay depth bias in reversed-NDC depth units, pulling the draw toward the
// camera. Applied in the vertex shader (not DepthBiasState, which is a no-op on the
// float depth format this backend gets) so roads/decals/overlays win the depth test
// against coplanar geometry.
override depth_bias: f32 = 0.0;
// HDR path (docs/hdr-pipeline-plan.md): 1 = decode the sampled albedo + folded
// material/light colours from sRGB to linear and drop the [0,1] radiance clamp, so
// shading writes linear radiance (that can exceed 1.0) into the HDR target. 0 = the
// exact gamma-naive GL33 behaviour for the LDR-direct path.
override linear: f32 = 0.0;
// Fraction of a draw's MATERIAL EMISSIVE that survives at full night. 1 = the shipped
// behaviour (emissive is absolute radiance, unaffected by time of day) and the default.
//
// WHY IT EXISTS (owner report 2026-08-16, Arma 2 Takistan, hour 0 vs hour 12, capture
// .tmp-shadow/owner/road-night/): the road reads (40.6, 41.0, 44.5) at midnight while the
// terrain one metre beside it reads (0.26, 0.22, 0.09) -- ~195x. By DAY the same road is
// correctly DARKER than the sand (186.9 vs 233.7), so this is not albedo and not the
// texture. Terrain and objects share one ambient expression (terrain.wgsl:852 and
// shading.wgsl:176 are the same `sky_irradiance(n) * sun_ambient.w`), so the only term an
// object can carry that terrain cannot is `m_emissive` -- which shade() adds as absolute
// radiance and nothing scales by the day/night level. Note the road's night brightness still
// falls with distance (40.6 -> 3.8), i.e. fog IS applied: it is specifically the sun/night
// factor that is missing, which is exactly the shape of an unattenuated emissive.
//
// A lit window or a lamp glass SHOULD stay lit at night, and this constant cannot tell one
// from the other -- so the CPU does: EngineWgpu's MaterialEmitsAtNight marks recognised
// fixture families with WGR_DRAW3D_NIGHT_EMITTER (bit 2 of material.normal_map.w) and those
// are exempt. Everything else fades with the sun. Default 0 since 2026-08-16: with 1.0 the
// forests self-illuminated at midnight on Arma 2, Arma 3 and DayZ (Takistan hour 0 with
// WGR_AUTO_EXPOSURE=0: dead trees ~150/255, canopy ~60, terrain 0.2 -- exposure exonerated).
// WGR_EMISSIVE_NIGHT=1 is the A/B back to the old behaviour.
override emissive_night: f32 = 0.0;
// Per-mip alpha gain for cutout foliage (WGR_CUTOUT_MIP_ALPHA); see gbuffer.wgsl
// mip_alpha_boost and gpu_driven.wgsl's twin.
override cutout_mip_alpha: f32 = 0.0;
// WGR_ROAD_PIXEL_CONFORM / WGR_ROAD_PIXEL_LIFT -- metres an OnSurface (road/decal) fragment is
// pulled TOWARD THE CAMERA along its view ray after fs_surface has re-seated its depth on the
// terrain (see surface_pixel_depth). Only fs_surface reads it; fs_main never writes depth.
override pixel_lift: f32 = 0.02;
// WGR_ROAD_KILL_DOOR=1 -- see fs_surface. A probe, never a look.
override road_kill_door: f32 = 0.0;
// WGR_ALPHA_KILL_DOOR=1 -- forces blended fragments opaque. A probe, never a look.
override alpha_kill_door: f32 = 0.0;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) fog: f32,             // 1 = keep colour, 0 = full fog
    @location(2) world_pos: vec3<f32>, // camera-relative
    @location(3) normal: vec3<f32>,    // world space, outward
    // Draw slot, carried flat so fs_main can index its material. Equals the draw's
    // base_instance (see draw_one); both vertex paths pass it through.
    @location(4) @interpolate(flat) instance: u32,
    // Authored ODOL tangent/binormal frame when supplied, otherwise zero. Kept
    // separate from the normal so fs_main can retain its safe derivative fallback.
    @location(5) tangent: vec3<f32>,
    @location(6) binormal: vec3<f32>,
    // 1 where the vertex was PINNED to the terrain (mode 2, conform_sel 2 -- roads and other
    // ClipLandOn geometry), 0 elsewhere. Interpolated, so fs_surface can weight its per-pixel
    // depth re-seat across a triangle that mixes pinned and rigid vertices. Skinned draws
    // and every rigid vertex carry 0, which makes fs_surface a no-op for them.
    @location(7) conform_w: f32,
    @location(8) tree_snow: vec2<f32>, // authored crown height, actual cached external exposure
};

// world_pos is camera-relative (the world matrix / palette is offset by the
// camera position on the C++ side), so its length is the camera distance.
fn finish_vertex(
    world_pos: vec4<f32>, normal_ws: vec3<f32>, tangent_ws: vec3<f32>, binormal_ws: vec3<f32>,
    uv: vec2<f32>, instance: u32, conform_w: f32, tree_snow: vec2<f32>,
) -> VsOut {
    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * world_pos);
    if (weather_depth > 0.5) {
        out.clip = frame.weather_vp * vec4<f32>(world_pos.xyz + frame.cam_pos.xyz, 1.0);
        if (weather_depth > 1.5) { out.clip = frame.weather_far_vp * vec4<f32>(world_pos.xyz + frame.cam_pos.xyz, 1.0); }
    }
    // Bias toward the camera (larger reversed depth) so decals/overlays win the
    // depth test against coplanar geometry.
    //
    // SCALED BY DEPTH SQUARED, not constant. This replaced glPolygonOffset(-1,-1),
    // which is denominated in RESOLVABLE DEPTH UNITS AT THE FRAGMENT and therefore
    // self-scales with distance; a constant offset in NDC does not, and reversed-Z NDC
    // is wildly non-linear in distance (stored depth is near/z for this
    // forward-infinite-far projection). A constant 1e-5 pulls a surface toward the
    // camera by z^2 * bias / near: about 1 m at 100 m, but ~95 m at 1 km and ~716 m at
    // 3 km. That is why a road could be seen THROUGH a mountain ridge - it was being
    // depth-tested as though it stood hundreds of metres in front of itself, and
    // roads carry NoZWrite so the ridge could not paint back over them.
    //
    // ndc^2 undoes exactly that: with depth = near/z, holding (bias * ndc^2) constant in
    // NDC is holding a constant WORLD-SPACE pull, which is what the GL call meant. Near
    // geometry is unaffected (ndc ~ 1 there, so this matches the old value), and the
    // far-field error disappears.
    //
    // Not env-gated, unlike most changes on this branch: the constant-NDC form is not a
    // behaviour worth preserving, it is a porting error with a measured failure mode.
    // WGR_DECAL_SCALE still tunes the magnitude, and near geometry is unchanged.
    let ndc_z = select(1.0, out.clip.z / out.clip.w, out.clip.w != 0.0);
    out.clip.z += depth_bias * ndc_z * ndc_z * out.clip.w;
    out.uv = uv;
    out.world_pos = world_pos.xyz;
    out.normal = normal_ws;
    out.tangent = tangent_ws;
    out.binormal = binormal_ws;
    out.fog = fog_factor(length(world_pos.xyz));
    out.instance = instance;
    out.conform_w = conform_w;
    out.tree_snow = tree_snow;
    return out;
}

@vertex
fn vs_main(
    @builtin(instance_index) instance: u32,
    @location(0) pos: vec3<f32>,
    @location(1) norm: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(5) conform_sel: u32,   // per-vertex conform selector (mode 2): 0/1/2
    @location(6) tangent: vec3<f32>,
    @location(7) binormal: vec3<f32>,
) -> VsOut {
    let obj = objects[instance];
    let world = obj.world;
    var world_pos = world * vec4<f32>(pos, 1.0);
    // Vertex normals arrive already negated (MeshBuild::BuildVertices stores
    // -Norm, the D3D convention). GL33 rotates that stored normal to world space
    // and lights with it as-is (EngineGL33_Shaders VSNormal: `mat3(world)*normal`,
    // no extra negation), so do the same here — negating again would flip N and
    // invert the diffuse/specular/local-light terms relative to the sun.
    let rot = mat3x3<f32>(world[0].xyz, world[1].xyz, world[2].xyz);
    var normal_ws = rot * norm;
    // Terrain conform: the shared base mesh is uploaded undeformed and conformed here
    // per instance. world_pos is camera-relative, so heights are evaluated in ABSOLUTE
    // world xz (+ frame.cam_pos) and written back camera-relative. conform2.z = mode:
    // 1 = ForestPlain bilinear plane, 2 = per-vertex ClipLand vegetation (heightmap).
    let mode = obj.conform2.z;
    // 1 for a vertex pinned onto the terrain (see VsOut.conform_w). Vertex-only knowledge;
    // fs_surface uses it to decide where its per-pixel depth re-seat applies.
    var conform_w = 0.0;
    if (mode > 1.5) {
        // Mode 2: individual ClipLand vegetation, conformed per vertex to SurfaceY,
        // matching Object::Animate (Object.cpp:395-423). conform_sel: 1 = ClipLandKeep
        // (keep height above the surface), 2 = ClipLandOn (pin onto it), 0 = rigid.
        let abs_x = world_pos.x + frame.cam_pos.x;
        let abs_z = world_pos.z + frame.cam_pos.z;
        let abs_xz = vec2<f32>(abs_x, abs_z);
        let sy = surface_y(abs_xz);
        if (conform_sel == 1u) {
            // world.y_abs = SurfaceY + undeformedWorldY - bcSurfaceY (conform0.x); the
            // cam.y offset cancels between the two camera-relative terms.
            //
            // ClipLandKeep deliberately keeps the FINE surface even under WGR_CONFORM_RASTER:
            // this vertex sits a fixed height ABOVE the ground, where a chord error of a few
            // decimetres is invisible, and the raster surface is camera-distance dependent --
            // a plant tracking it would visibly rise and sink as the camera walked. The
            // coplanar depth-fight the raster surface exists to fix is a ClipLandOn problem.
            world_pos.y = sy + world_pos.y - obj.conform0.x;
        } else if (conform_sel == 2u) {
            // WLD-023(c): pin to the surface the terrain RASTERISES, not the one it
            // evaluates, so a road cannot sit above the coarse chord of the ridge in front
            // of it. Returns surface_y exactly when WGR_CONFORM_RASTER is unset (default).
            world_pos.y = surface_y_raster(abs_xz, frame.cam_pos.xyz) - frame.cam_pos.y;
            conform_w = 1.0;
        }
        // Tilt the conformed vertex's normal by the terrain slope (inverse-transpose of
        // the vertical height-field shear, same as mode 1) so lighting follows the ground
        // the CPU's InvalidateNormals would have produced. Rigid verts (sel 0) keep theirs.
        //
        // This stays on the FINE gradient under WGR_CONFORM_RASTER, deliberately: terrain
        // shades from the fine field too, per-fragment and explicitly LOD/morph-independent
        // (terrain.wgsl:112-123 — a mesh-derived normal banded the lighting into radial
        // stripes). Matching the drawn POSITION is a depth question; matching the drawn
        // NORMAL would reintroduce the banding that comment records.
        if (conform_sel != 0u) {
            let g = surface_grad(vec2<f32>(abs_x, abs_z));
            normal_ws = vec3<f32>(normal_ws.x - g.x * normal_ws.y, normal_ws.y,
                                  normal_ws.z - g.y * normal_ws.y);
        }
    } else if (mode > 0.5) {
        // Mode 1: ForestPlain bilinear plane fit (ObjectClasses.cpp:571-605).
        let s = obj.conform0.x;                          // inv_land_grid
        let xIn = (world_pos.x + frame.cam_pos.x) * s + obj.conform0.y;  // *invLand - xf
        let zIn = (world_pos.z + frame.cam_pos.z) * s + obj.conform0.z;  // *invLand - zf
        let y00 = obj.conform1.x; let y10 = obj.conform1.y;
        let d1000 = obj.conform1.z; let d0100 = obj.conform1.w;
        let d1011 = obj.conform2.x; let d0111 = obj.conform2.y;
        let triA = xIn <= 1.0 - zIn;
        let py = select(y10 + d0111 - d1011 * xIn - zIn * d0111,
                        y00 + d1000 * zIn + d0100 * xIn,
                        triA);
        // Camera-relative conformed height: absolute plane height + the vertex's own
        // model height above surface (conform0.w = BoundingCenter().y), minus cam.y.
        world_pos.y = py - frame.cam_pos.y + pos.y + obj.conform0.w;
        // Tilt the undeformed normal by the plane gradient (inverse-transpose of the
        // affine y-shear) so lighting matches the CPU's post-deform InvalidateNormals.
        let gx = select(-d1011, d0100, triA) * s;
        let gz = select(-d0111, d1000, triA) * s;
        normal_ws = vec3<f32>(normal_ws.x - gx * normal_ws.y, normal_ws.y, normal_ws.z - gz * normal_ws.y);
    }
    // VEG-SWAY, the direct-path twin of gpu_driven::vs_gpu. Applied AFTER conform for the same
    // reason: conform samples terrain height at the vertex's XZ, so swaying first would make the
    // plant ride the ground height of wherever the wind had just pushed it.
    //
    // The vegetation gate is `sun_ambient.w` (1 = bush, 2 = tree/forest) — the direct path's
    // equivalent of the retained path's INST_CANOPY_* instance flags. Unlike the canopy-normal
    // block below, sway is NOT restricted to alpha-cut sections: a trunk has to lean with its
    // own crown or the tree tears apart at the bark line.
    let foliage_kind = materials[instance].sun_ambient.w;
    if (foliage_kind > 0.5 && frame.foliaged.x > 0.0) {
        // world[3] is the object origin, already camera-relative on this path (the C++ side
        // offsets the world matrix by the camera position), so the camera position is added
        // back to seed the phase from an ABSOLUTE world XZ. Seeding from the camera-relative
        // value would make every plant swim as the camera walks.
        let phase_xz = world[3].xz + frame.cam_pos.xz;
        let leaf = select(select(0.0, 1.0, alpha_ref > 0.0), -1.0, foliage_kind > 2.5);
        let sway = veg_sway_offset(pos.y, phase_xz, leaf);
        world_pos = vec4<f32>(world_pos.xyz + sway, world_pos.w);
    }
    // REGRESSION GUARD (CWA stock-tree canopies): legacy leaf cards have arbitrary authored
    // normals, so a visible card can point sideways or downward and receive almost no daylight
    // or directional sky. Without this direct-path radial-crown correction, standard CWA trees
    // such as `data3d\str buk.p3d` regress to large black canopy slabs in daylight, even though
    // their PAC texture and alpha cutout are correct. The retained/GPU-driven path already bends
    // cutout canopy normals this way; keep this direct-path equivalent. sun_ambient.w carries
    // 1=bush or 2=tree/forest, so only alpha-cut vegetation changes.
    var bend_canopy = alpha_ref > 0.0 && foliage_kind > 0.5;
#ifdef NATIVE_AUTHORED_NORMALS
    // Kind 3 identifies native trees independently of the wind amplitude.
    bend_canopy = bend_canopy && foliage_kind < 2.5;
#endif
    if (bend_canopy) {
        let is_tree = foliage_kind > 1.5;
        let crown_y = select(frame.foliageb.z, frame.foliagec.z, is_tree);
        let bend = select(frame.foliageb.y, frame.foliagec.y, is_tree);
        let crown_center = world[3].xyz + vec3<f32>(0.0, crown_y, 0.0);
        let radial = world_pos.xyz - crown_center;
        if (dot(radial, radial) > 1e-6) {
            normal_ws = normalize(mix(normal_ws, normalize(radial), clamp(bend, 0.0, 1.0)));
        }
    }
    return finish_vertex(world_pos, normal_ws, rot * tangent, rot * binormal, uv, instance, conform_w,
        vec2<f32>(tree_snow_height(pos.y, obj.conform0.y, obj.conform0.z, obj.conform2.w), obj.conform1.x));
}

// Linear-blend skinning (see the skin module). Vertices with no skin weight
// carry a single weight of 1.0 on a reserved bone whose palette entry is just
// `world`, so no zero-weight fallback is needed.
@vertex
fn vs_skinned(
    @builtin(instance_index) instance: u32,
    @location(0) pos: vec3<f32>,
    @location(1) norm: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) bones: vec4<u32>,   // Uint8x4: palette indices
    @location(4) weights: vec4<f32>, // Unorm8x4: normalised weights
) -> VsOut {
    let world_pos = skin_pos(pos, bones, weights);
    // As in vs_main: `norm` is the already-negated stored normal (SetSkinData
    // uploads -OrigNorm), skin_normal rotates it into world space, and we light
    // with it as-is to match GL33 — no extra negation.
    let normal_ws = skin_normal(norm, bones, weights);
    return finish_vertex(world_pos, normal_ws, vec3<f32>(0.0), vec3<f32>(0.0), uv, instance, 0.0, vec2<f32>(0.0));
}

// The colour-pass fragment. Depth is fixed-function (no frag_depth), so early-Z stays on for
// every ordinary draw; the OnSurface variant below (fs_surface) shares this body and adds
// only a depth output.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    census(0u);
    if (alpha_ref > 0.0) {
        census(1u);
    }
    return shade_fragment(in, direct_ground_receiver(in));
}

#ifdef EARLY_DEPTH
// REN-OBJ-004: fs_main with the depth test forced before the shader; used only by prepassed
// pipelines (depth writes off), see gpu_driven.wgsl's fs_gpu_early for the reasoning.
@fragment
@early_depth_test(force)
fn fs_main_early(in: VsOut) -> @location(0) vec4<f32> {
    census(0u);
    if (alpha_ref > 0.0) {
        census(1u);
    }
    return shade_fragment(in, direct_ground_receiver(in));
}
#endif

// OnSurface (roads, footprint decals) colour output plus an explicit depth. Only pipelines
// built for Offset::Decal on the plain (non-skinned) path use this entry point: writing
// frag_depth turns early-Z off, which is acceptable for the handful of road/decal draws a
// frame and not for everything else.
struct SurfaceOut {
    @location(0) color: vec4<f32>,
    @builtin(frag_depth) depth: f32,
};

@fragment
fn fs_surface(in: VsOut) -> SurfaceOut {
    var out: SurfaceOut;
    // KILL DOOR (WGR_ROAD_KILL_DOOR=1). Paint every OnSurface fragment solid magenta and hand
    // it the nearest possible depth, so nothing downstream can hide it. It answers one question
    // and only one: is this road tile RASTERISED AT ALL?
    //
    // That is the question left after the Malden hole, and the two answers need opposite fixes.
    // Magenta appears -> the tile is submitted and drawn, and something after rasterisation
    // loses it (depth, blending, a later pass). Hole stays -> the tile never reaches the
    // rasteriser, and the fault is upstream in culling, LOD or the draw list, where no amount of
    // depth-conform tuning will ever reach it.
    //
    // Four things have already been eliminated by ablation and none of them was it: the
    // GPU-driven path, the CPU road split, the conform height source, and the lift itself
    // (raising it closes the fringes and never closes this hole). Guessing a fifth is worth
    // less than this one measurement.
    if (road_kill_door > 0.5) {
        out.color = vec4<f32>(1.0, 0.0, 1.0, 1.0);
        out.depth = 1.0; // reversed-Z near plane: wins every comparison
        return out;
    }
    out.color = shade_fragment(in, in.conform_w > 0.99);
    // The road's distance-scaled depth lift is not physical elevation. Do not let
    // it pull a ground-pinned road/decal through an opaque snow cover. Tracks and
    // roof shelter use the same deficit buffer as the terrain; rigid bridges stay.
    // Altitude snowline cover intentionally does NOT discard here: a mountain road
    // reads as cleared asphalt, which is the correct look, not a defect.
    if (in.conform_w > 0.99 && snow_depth(in.world_pos.xz + frame.cam_pos.xz) >= 0.04) {
        discard;
    }
    out.depth = surface_pixel_depth(in);
    return out;
}

// WGR_ROAD_PIXEL_CONFORM -- per-PIXEL terrain conform of an OnSurface draw's DEPTH.
//
// The vertex conform (vs_main, conform_sel 2) pins each road VERTEX to the terrain, but
// between two road vertices the road is a flat chord while the terrain is not: wherever the
// ground bulges up between them the terrain wins `GreaterEqual` and pokes through the ribbon
// as terrain-triangle-shaped patches (owner, Arma 2 Takistan, .tmp-shadow/a2clip). A
// vertex-side fix cannot reach a between-vertex error (WGR_CONFORM_RASTER moved 0.76% of the
// pixels), and the CPU road split along terrain triangles the old engine used
// (WGR_ROAD_CPU_SPLIT) was rejected as too expensive. So the depth is re-seated per fragment:
//
//   1. Take the fragment's view ray (camera-relative world_pos; the camera is at the origin).
//   2. Walk that ray onto the SAME height source the vertex conform reads (surface_y_raster,
//      i.e. surface_y unless WGR_CONFORM_RASTER=1) -- a short fixed-point iteration, because
//      the terrain height at the road-plane hit is not yet the terrain hit along the ray. At a
//      grazing 10 degrees a 0.2 m height gap is a 1.1 m slide along the ray, so lifting the
//      point vertically would leave the terrain fragment nearer and STILL winning. Three
//      steps converge to sub-cm on any slope the iteration is stable on (|slope| < 1/tan(el)),
//      and the lift below covers the residual.
//   3. Pull the seated point `pixel_lift` (+ 0.5 mm/m of distance) toward the camera ALONG
//      THE RAY -- always nearer under reversed-Z regardless of whether the camera looks down
//      onto or up at the road, unlike a vertical lift -- and re-apply the vertex path's
//      ndc-squared decal bias so WGR_DECAL_SCALE still means what it meant.
//
// The COLOUR position, shading and uv are untouched; only the depth this fragment is tested
// with moves. The rigid parts of a partly-pinned draw keep their fixed-function depth
// (conform_w = 0), and so does everything drawn before a heightmap exists.
const PIXEL_CONFORM_STEPS: i32 = 3;
// Below this |dir.y| the ray is too flat to solve for its terrain hit (a 1 m height gap is
// a 20 m slide); the point is seated vertically instead, which the lift then covers.
const PIXEL_CONFORM_MIN_DIR_Y: f32 = 0.05;
// How far the seated point may still be from the terrain before the solve is declared failed
// and the fragment keeps its vertex-conformed depth. Generous: the conform is worth having
// whenever it is roughly right, and this only has to catch the runaway case, where the error is
// metres rather than centimetres.
const PIXEL_CONFORM_MAX_RESIDUAL: f32 = 0.75;
// Per metre of seated distance, BEFORE the grazing division below. Sweepable, because the
// value that matters is the product of this and 1/|dir.y| and neither half is guessable.
override pixel_lift_per_m: f32 = 0.0067;
// Upper bound on the along-ray pull, as a fraction of the seated distance. The grazing
// compensation below is a division and therefore unbounded as the ray flattens; without a cap a
// near-horizontal ray would pull the road far enough toward the camera to show through a rise in
// front of it. Default 3.5% (the owner's value) = 2.8 m at 80 m: generous, because the residual it
// covers is itself metres along the ray once the grazing division is in play.
override pixel_lift_max_frac: f32 = 0.04;

// ROAD / DECAL per-pixel depth conform knobs, from the reserved fogfar lanes. Each reads 0 as
// "use the shipped default", so an unwritten camera is bit-identical to the pre-lane behaviour.
// The defaults here are the values swept on Malden -- see surface_pixel_depth in shader3d.wgsl
// for what they do and why the per-metre term is divided by the grazing angle.
fn road_lift_flat() -> f32 {
    return select(clamp(frame.fogfar.y, 0.0, 1.0), 0.02, frame.fogfar.y <= 0.0);
}
fn road_lift_per_m() -> f32 {
    return select(clamp(frame.fogfar.z, 0.0, 0.2), 0.0067, frame.fogfar.z <= 0.0);
}
fn road_lift_max_frac() -> f32 {
    return select(clamp(frame.fogfar.w, 0.0005, 2.0), 0.04, frame.fogfar.w <= 0.0);
}

fn surface_pixel_depth(in: VsOut) -> f32 {
    let w = clamp(in.conform_w, 0.0, 1.0);
    let dist = length(in.world_pos);
    if (w <= 0.0 || hm_params.enabled < 0.5 || dist <= 1e-3) {
        return in.clip.z;
    }
    let cam = frame.cam_pos.xyz;
    let dir = in.world_pos / dist;
    var p = in.world_pos;
    var i = 0;
    loop {
        if (i >= PIXEL_CONFORM_STEPS) { break; }
        // Camera-relative terrain height under the current point.
        let hy = surface_y_raster(p.xz + cam.xz, cam) - cam.y;
        if (abs(dir.y) > PIXEL_CONFORM_MIN_DIR_Y) {
            // Slide along the ray to where it reaches that height. Clamped so a step onto a
            // steep facing slope cannot throw the point far past the road it belongs to.
            let t = clamp(hy / dir.y, 0.5 * dist, 1.5 * dist);
            p = dir * t;
        } else {
            p = vec3<f32>(p.x, hy, p.z);
        }
        i = i + 1;
    }
    // DID THE ITERATION ACTUALLY CONVERGE? Three steps of a fixed-point solve are enough on any
    // slope the solve is stable on -- and it is NOT stable everywhere. The step slides along the
    // ray by hy / dir.y, so at a grazing angle a small height error is a large slide, and the
    // clamp above then pins the point at 1.5x its true distance. A fragment placed half again as
    // far away as the road really is loses the depth test against the terrain by TENS OF METRES,
    // which is why raising the lift narrowed the fringes and never closed the owner's hole on
    // Malden: the lift is centimetres and the error is not.
    //
    // Established by kill door (WGR_ROAD_KILL_DOOR=1): with depth forced to the near plane the
    // road is a continuous ribbon with no hole at all, so every fragment IS rasterised and the
    // loss is downstream in depth. Four upstream suspects had already been eliminated by
    // ablation -- GPU-driven, the CPU road split, the conform height source, and the lift.
    //
    // So: measure the residual and, when the solve did not land on the terrain, return the
    // VERTEX-conformed depth this function was handed. That is the depth the road had before
    // per-pixel conform existed -- imperfect, but never catastrophically wrong. A refinement
    // that cannot converge must not be allowed to return something worse than its input.
    let residual_y = surface_y_raster(p.xz + cam.xz, cam) - cam.y;
    if (abs(p.y - residual_y) > PIXEL_CONFORM_MAX_RESIDUAL) {
        return in.clip.z;
    }
    let seated_dist = length(p);
    // GRAZING COMPENSATION. The iteration above leaves a small height residual, and the comment
    // at the head of this function already states the conversion: a height gap becomes an
    // ALONG-RAY distance of gap / |dir.y|, which is 6x at the ~9 degrees you get looking down a
    // road from standing height, and 20x at the floor below. The lift has to cover the residual
    // in the same units the depth test sees it in, so it takes the same division -- otherwise a
    // constant that is generous when looking straight down is a coin toss at a grazing angle,
    // and the road breaks into a staggered chequer of tiles that win and lose the depth test
    // alternately. That is exactly what was reported on Malden: continuous from above,
    // continuous close up, and in pieces across the middle distance.
    //
    // Only the DISTANCE term is divided. `pixel_lift` stays flat so the near field and the
    // top-down view are bit-identical to before; there was nothing wrong with either.
    let graze = max(abs(dir.y), PIXEL_CONFORM_MIN_DIR_Y);
    // Live from the dev panel's Roads section via the reserved fogfar lanes; the override
    // constants remain as the env-var seed for a headless A/B and as the value an unwritten
    // camera falls back to.
    let flat = select(road_lift_flat(), pixel_lift, frame.fogfar.y <= 0.0 && pixel_lift > 0.0);
    let per_m = select(road_lift_per_m(), pixel_lift_per_m, frame.fogfar.z <= 0.0);
    let max_frac = select(road_lift_max_frac(), pixel_lift_max_frac, frame.fogfar.w <= 0.0);
    let lift = min(flat + per_m * seated_dist / graze, max_frac * seated_dist);
    let pulled = p * max(seated_dist - lift, 1e-3) / max(seated_dist, 1e-3);
    let clip = reverse_z(frame.proj * frame.view * vec4<f32>(pulled, 1.0));
    if (clip.w <= 1e-6) {
        return in.clip.z;
    }
    var ndc = clip.z / clip.w;
    ndc = ndc + depth_bias * ndc * ndc;
    return mix(in.clip.z, clamp(ndc, 0.0, 1.0), w);
}

fn direct_snow_receiver(in: VsOut) -> bool {
    // Keep vertices leave conform_w zero; only pinned On vertices set it to one.
    // CPU source admission separately proves mode-2 meshes contain no On hints.
    let material = materials[in.instance];
    let flags = material_flags(material);
    return (flags & MATFLAG_SNOW_RECEIVER) != 0u &&
        (flags & (2u | 4u | 8u | 16u | 32u)) == 0u &&
        // Solid cutout fabric/mesh keeps the unchanged base-alpha discard/A2C
        // coverage. Snow changes RGB, never its holes or sample mask.
        translucent <= 0.5 && material.sun_ambient.w <= 0.0 &&
        dot(material.emissive.rgb, material.emissive.rgb) < 1e-8 &&
        dot(in.normal, in.normal) > 1e-8 && in.conform_w <= 0.0;
}

fn direct_ground_receiver(in: VsOut) -> bool {
    if (frame.ground_weather.x <= 0.18 || linear <= 0.5) { return false; }
    let material = materials[in.instance];
    let flags = material_flags(material);
    if ((flags & MATFLAG_GROUND_RECEIVER) == 0u ||
        (flags & (2u | 4u | 8u | 16u | 32u)) != 0u || translucent > 0.5 ||
        material.sun_ambient.w > 0.0 || dot(material.emissive.rgb, material.emissive.rgb) >= 1e-8 ||
        in.conform_w > 0.0) { return false; }
    return rigid_ground_fragment(in.world_pos + frame.cam_pos.xyz, in.normal, in.world_pos);
}

fn direct_tree_snow_cover(in: VsOut) -> f32 {
    let material = materials[in.instance];
    if (alpha_ref <= 0.0 || translucent > 0.5 || linear <= 0.5 ||
        material.sun_ambient.w < 1.5 || (material_flags(material) & (2u | 4u | 8u | 16u | 32u)) != 0u) { return 0.0; }
    return tree_snow_coverage(in.world_pos, in.tree_snow.x, in.tree_snow.y);
}

fn shade_fragment(in: VsOut, ground_surface: bool) -> vec4<f32> {
    // Derivatives must run in uniform control flow — before the discard.
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);
    let uniform_uv_footprint = fwidth(in.uv);
    let boot_duv_dx = dpdx(in.uv);
    let boot_duv_dy = dpdy(in.uv);
    // The reflected camera carries an absolute-world water clip plane in Frame.
    // Terrain already honours it; apply the same plane to retained/skinned objects
    // so submerged geometry cannot leak into or occlude the planar reflection.
    // Main cameras upload a zero plane, making this branch compile to a cheap no-op.
    let clip_len2 = dot(frame.clip_plane.xyz, frame.clip_plane.xyz);
    if (clip_len2 > 0.0 &&
        dot(frame.clip_plane.xyz, in.world_pos + frame.cam_pos.xyz) + frame.clip_plane.w < 0.0) {
        discard;
    }
    // Per-draw material for this draw slot (base_instance == draw slot). Its emissive.w
    // packs the bindless texture + sampler indices ((tex_slot << 3) | sampler); the index
    // is uniform across a derivative quad (one instance per primitive), so implicit-mip
    // textureSample stays legal.
    let material = materials[in.instance];
    let packed = bitcast<u32>(material.emissive.w);
    let normal_packed = bitcast<u32>(material.normal_map.x);
    // AST-012A: report the albedo slot's wanted mip. The UV derivatives are the RAW ones --
    // `uv` below may have been displaced by parallax, and a POM step's derivative describes
    // the search, not the surface. The albedo alone is reported because it is the slot a
    // residency decision is about; the normal map shares its fate.
    mip_feedback_note(packed >> 3u, dpdx(in.uv), dpdy(in.uv));

    if ((material_flags(material) & MATFLAG_BOOT_RELIEF) != 0u) {
        let fade = 1.0 - smoothstep(4.0, 12.0, length(in.world_pos));
        if (fade <= 0.0) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
        let slot = packed >> 3u;
        let samp = packed & 7u;
        let dims = vec2<f32>(textureDimensions(textures[slot], 0));
        let texel = 1.0 / max(dims, vec2<f32>(1.0));
        let rho = max(length(boot_duv_dx * dims), length(boot_duv_dy * dims));
        let lod = max(0.0, log2(max(rho, 1.0)) + frame.renscale.x);
        let centre = textureSampleLevel(textures[slot], samplers[samp], in.uv, lod);
        let height = boot_relief_height(centre);
        let determinant = boot_duv_dx.x * boot_duv_dy.y - boot_duv_dx.y * boot_duv_dy.x;
        if (abs(determinant) < 0.000000000001) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
        let tangent = (dwx * boot_duv_dy.y - dwy * boot_duv_dx.y) / determinant;
        let bitangent = (dwy * boot_duv_dx.x - dwx * boot_duv_dy.x) / determinant;
        let n = surface_normal(in.normal, in.world_pos, dwx, dwy);
        let view = -in.world_pos / max(length(in.world_pos), 0.0001);
        let shifted = clamp(in.uv + boot_relief_parallax(view, n, tangent, bitangent, height) * fade,
                            texel * 0.5, vec2<f32>(1.0) - texel * 0.5);
        let mask = textureSampleLevel(textures[slot], samplers[samp], shifted, lod);
        let hp = boot_relief_height(textureSampleLevel(textures[slot], samplers[samp], shifted + vec2<f32>(texel.x, 0.0), lod));
        let hm = boot_relief_height(textureSampleLevel(textures[slot], samplers[samp], shifted - vec2<f32>(texel.x, 0.0), lod));
        let vp = boot_relief_height(textureSampleLevel(textures[slot], samplers[samp], shifted + vec2<f32>(0.0, texel.y), lod));
        let vm = boot_relief_height(textureSampleLevel(textures[slot], samplers[samp], shifted - vec2<f32>(0.0, texel.y), lod));
        let gradient = vec2<f32>((hp - hm) / (2.0 * texel.x), (vp - vm) / (2.0 * texel.y));
        let relieved = boot_relief_normal(n, tangent, bitangent, gradient);
        let sun = -frame.sun_dir_world.xyz;
        let light = sun / max(length(sun), 0.0001);
        let direct = max(frame.sun_diffuse.r, max(frame.sun_diffuse.g, frame.sun_diffuse.b));
        let multiplier = boot_relief_multiplier(n, relieved, light, boot_relief_height(mask),
            material.local_specular.w * min(mask.a, centre.a), fade, direct);
        let airlight = apply_fog_terrain(vec3<f32>(0.0), in.world_pos);
        return boot_relief_composite(multiplier, airlight);
    }
    // Parallax occlusion mapping, carried in the normal map's RED channel.
    //
    // R is free and it is free by MEASUREMENT, not by hope: every shipped Arma NOHQ in this
    // programme's corpus has R constant 0 (40 of 40), because the format stores X in alpha and Y in
    // green and reconstructs Z. So a stock texture yields height 0, the loop below finds no
    // displacement, and the offset is exactly zero -- this is self-gating and needs no uniform, no
    // ABI change and no extra texture binding. An enhanced texture carries height there and gets
    // relief. Both sample the SAME already-bound normal map.
    //
    // Height taps use textureSampleLevel: the march is per-pixel control flow, and implicit-mip
    // sampling inside non-uniform flow is undefined. The final base/normal samples below stay
    // implicit-mip and unconditional, so their derivatives remain valid.
    var uv = in.uv;
    // NOT on alpha-tested surfaces. The parallax offset moves the uv the BASE colour is sampled
    // at, and the cutout test below runs on that sample's alpha -- so on a cutout texture the
    // shifted uv can land in a transparent region and discard the pixel, punching see-through
    // holes in vehicles and foliage. Displacing a surface whose silhouette is defined by its alpha
    // is wrong in any case: the hole would move with the view while its edge stayed put.
    // alpha_ref is the existing cutout threshold, so this costs nothing to ask.
    // RG normals spend red on normal X, never on parallax height.
    let normal_rg = (material_flags(material) & MATFLAG_NORMAL_RG) != 0u;
    if (material.normal_map.z > 0.5 && alpha_ref <= 0.0 && !normal_rg) {
        // Tangent frame for the view vector, derived the same way the shading frame below is, so
        // the displacement runs along the surface's real UV directions.
        let duv_dx = dpdx(in.uv);
        let duv_dy = dpdy(in.uv);
        let n_geo = surface_normal(in.normal, in.world_pos, dwx, dwy);
        var t_raw = dwx * duv_dy.y - dwy * duv_dx.y;
        var b_raw = dwy * duv_dx.x - dwx * duv_dy.x;
        if (dot(in.tangent, in.tangent) > 1e-10 && dot(in.binormal, in.binormal) > 1e-10) {
            t_raw = in.tangent;
            b_raw = in.binormal;
        }
        let t_ortho = t_raw - n_geo * dot(n_geo, t_raw);
        if (dot(t_ortho, t_ortho) > 1e-10 && dot(b_raw, b_raw) > 1e-10) {
            let tan = normalize(t_ortho);
            var hand = 1.0;
            if (dot(cross(n_geo, tan), b_raw) < 0.0) {
                hand = -1.0;
            }
            let bit = normalize(cross(n_geo, tan)) * hand;
            // Surface -> eye. world_pos is camera-relative, so the eye is at the origin.
            let v = normalize(-in.world_pos);
            let v_ts = vec3<f32>(dot(v, tan), dot(v, bit), dot(v, n_geo));
            // Distance fade: relief is a close-up cue and the march is the expensive part, so it
            // fades out well before the texture does. Beyond the fade there is no march at all.
            let dist_fade = 1.0 - smoothstep(35.0, 60.0, length(in.world_pos));
            // Self-gating needs an explicit early-out, not just the maths working out. A FLAT R=0
            // field is not "no displacement": the march would walk the ray to the bottom of the
            // height range and apply the FULL offset, shifting the texture on every stock
            // normal-mapped surface in the game. Stock NOHQ has R exactly 0, so one tap at the
            // undisplaced uv separates "no height authored" from "height authored and dark here".
            let h_probe = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], in.uv, 0.0).r;
            if (dist_fade > 0.001 && v_ts.z > 0.15 && h_probe > 0.004) {
                // Step count falls off at grazing angles rather than rising: a grazing ray needs the
                // most steps and is also where a bounded march looks worst, so this trades a little
                // accuracy there for a hard cost ceiling.
                let steps = mix(8.0, 20.0, clamp(v_ts.z, 0.0, 1.0)) ;
                let amplitude = 0.02 * dist_fade;
                let max_offset = (v_ts.xy / max(v_ts.z, 0.15)) * amplitude;
                let dstep = 1.0 / steps;
                var cur_h = 1.0;
                var cur_uv = in.uv;
                var h = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], cur_uv, 0.0).r;
                var i = 0.0;
                loop {
                    if (i >= steps || h >= cur_h) { break; }
                    cur_h = cur_h - dstep;
                    cur_uv = in.uv - max_offset * (1.0 - cur_h);
                    h = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], cur_uv, 0.0).r;
                    i = i + 1.0;
                }
                // One linear refine between the last two steps: without it the silhouette of the
                // relief stairsteps at exactly the step count, which is more obvious than the
                // undersampling it comes from.
                let prev_uv = cur_uv + max_offset * dstep;
                let prev_h = textureSampleLevel(textures[normal_packed >> 3u], samplers[normal_packed & 7u], prev_uv, 0.0).r - (cur_h + dstep);
                let this_h = h - cur_h;
                let w = this_h / max(this_h - prev_h, 1e-5);
                uv = mix(cur_uv, prev_uv, clamp(w, 0.0, 1.0));
            }
        }
    }

    // REN-TEMP-001D: material samples take the render-scale mip bias (0 at native scale).
    var base = textureSampleBias(textures[packed >> 3u], samplers[packed & 7u], uv, frame.renscale.x);
    // Uniform derivatives before cutout discard. RGB only: depth, alpha,
    // shadow coverage and the prepass retain their authored values.
    base = vec4<f32>(uniform_cloth_color(base.rgb, material.local_specular.w, in.uv, uniform_uv_footprint), base.a);
    if ((material_flags(material) & 32u) != 0u && linear > 0.5) {
        let dims = vec2<f32>(textureDimensions(monitor_scene));
        let screen_uv = in.clip.xy / dims;
        let scene = textureSampleLevel(monitor_scene, terrain_shadow_samp, screen_uv, 0.0).rgb;
        let luminance = dot(scene, vec3<f32>(0.2126, 0.7152, 0.0722));
        // Texture alpha defines the authored display aperture, including its black rim.
        let aperture = 1.0 - smoothstep(0.90, 1.0, base.a);
        let footprint = max(fwidth(uv.y) * 256.0, 0.0);
        let lines = 1.0 - 0.10 * (0.5 + 0.5 * cos(uv.y * 256.0 * 6.2831853))
            * (1.0 - smoothstep(0.5, 1.5, footprint));
        let phosphor = vec3<f32>(0.30, 0.85, 0.48);
        let signal = (luminance * 0.85 + 0.008) * lines;
        return vec4<f32>(phosphor * signal * aperture, 1.0);
    }
    let normal_sample = textureSampleBias(textures[normal_packed >> 3u], samplers[normal_packed & 7u], uv, frame.renscale.x);
    var normal_ts_xy = vec2<f32>(select(normal_sample.a, normal_sample.r, normal_rg), normal_sample.g) * 2.0 - vec2<f32>(1.0);
    if ((material_flags(material) & MATFLAG_INVERT_NORMAL_Y) != 0u) {
        normal_ts_xy.y = -normal_ts_xy.y;
    }
    let normal_ts = vec3<f32>(normal_ts_xy, sqrt(max(0.0, 1.0 - dot(normal_ts_xy, normal_ts_xy))));
    // Cutout: A2C emits a sharpened coverage alpha (edge dithered across MSAA samples);
    // otherwise the classic hard discard. a2c is a compile-time override, so exactly one
    // path is compiled and control flow stays uniform for the derivatives above/below.
    // Cutout alpha with the mip-coverage lift (the sampled alpha stays what it was for the
    // shading below). alpha_ref is a pipeline constant, so this folds away on opaque draws.
    var cut_a = base.a;
    if (alpha_ref > 0.0) {
        let dims0 = vec2<f32>(textureDimensions(textures[packed >> 3u]));
        cut_a = mip_alpha_boost(base.a, uv, dims0, cutout_mip_alpha);
    }
    var out_a = base.a;
    if (a2c > 0.5) {
        // OPAQUE sections take full coverage and must not reach the coverage sharpener at all.
        // With alpha_to_coverage_enabled the fragment's alpha IS its sample mask, so any value
        // below 1 punches holes in solid geometry. An opaque section has alpha_ref 0, and
        // a2c_coverage(a, 0) is clamp(a / fwidth(a) + 0.5, 0, 1) -- which is 1 for a texture
        // whose alpha really is 1 everywhere, but is NOT 1 for the many legacy textures that
        // carry a non-coverage alpha channel.
        //
        // gpu_driven.wgsl -- this shader's retained twin, same inputs, same intent -- has the
        // identical guard. The two draw the same sections depending only on whether a model
        // registered for GPU-driven rendering, so any divergence between them is a bug by
        // construction: the same geometry would render differently for a reason that has
        // nothing to do with how it should look.
        if (alpha_ref > 0.0) {
            out_a = a2c_coverage(cut_a, alpha_ref);
            if (out_a <= 0.0) {
                discard;
            }
        } else {
            out_a = 1.0;
        }
    } else if (cut_a < alpha_ref) {
        discard;
    }
    // WGR_ALPHA_KILL_DOOR=1 -- force every blended fragment fully opaque. One question only:
    // is a translucent-looking surface translucent because of its ALPHA, or because of the
    // order it is drawn in? Those need opposite fixes and a screenshot cannot tell them apart.
    // Written for the Malden town sign, whose plate texture measures 89% fully opaque and still
    // shows the houses behind it.
    if (alpha_kill_door > 0.5) {
        // Solid red at alpha 1, not just alpha 1. Forcing the alpha alone cannot tell three
        // cases apart, and on the Malden sign that ambiguity cost a wrong conclusion: nothing
        // changed, and "the alpha is not the cause" and "the probe never reached this draw"
        // look identical. With a colour: red and opaque = the probe reached it and the blend is
        // over, so the fault is upstream of alpha. Red and still see-through = it reached it and
        // the blend is NOT over. Unchanged = it never reached this draw at all.
        return vec4<f32>(1.0, 0.0, 0.0, 1.0);
    }
    if (is_shadow > 0.5) {
        // An alpha shadow changes the receiver behind the intervening air.
        // Compositing black after surface fog also blackened that air, exposing
        // rectangular projected-shadow meshes on distant roads. Transport black
        // through the same medium so its in-scattering survives the overlay.
        var shadow_rgb = vec3<f32>(0.0);
        if (frame.params.fog_enabled >= 1.5) {
            shadow_rgb = apply_fog(shadow_rgb, in.world_pos);
        } else if (frame.params.fog_enabled > 0.5) {
            let air = select(frame.fog_color.rgb, srgb_to_linear(frame.fog_color.rgb), linear > 0.5);
            shadow_rgb = air * (1.0 - in.fog);
        }
        return vec4<f32>(shadow_rgb, frame.params.shadow_strength * base.a);
    }
    // Material debug is global for the Dev Tools path. Keep the old per-draw lane as a fallback
    // for the explicit WGR_MATERIAL_DEBUG_RVMAT selector, so capture fixtures and the live panel
    // share one set of view IDs without changing the WgrDraw3D ABI.
    let global_material_view = matdbg_view();
    let material_view = select(u32(material.normal_map.y + 0.5), global_material_view,
                               global_material_view != MATDBG_VIEW_FULL);
    // The material colours are already sun-FOLDED on the CPU (mat_sun_* = raw × sun); hand
    // them to the shared shading straight. The GPU-driven path folds raw × frame-sun instead,
    // then calls the same shade(). srgb-decode of albedo + terms happens inside shade().
    var m: ShadeMaterial;
    // The same filtered source mask drives absorption and the rough wet layer.
    // Never infer wetness from rainfall, owner type or albedo colour here.
    m.wet_cloth = uniform_cloth_wetness(material.local_specular.w, in.uv, uniform_uv_footprint);
    m.native_cavity = 1.0;
    m.native_ao = 1.0;
#ifdef NATIVE_LEAF_CAVITY
    if ((material_flags(material) & 1024u) != 0u && material.normal_map.z > 0.5) {
        m.native_cavity = clamp(normal_sample.a, 0.0, 1.0);
    }
#endif
    m.emissive = material.emissive.rgb;
    if (emissive_night < 1.0 && (material_flags(material) & MATFLAG_NIGHT_EMITTER) == 0u) {
        // At EVERY hour, not only at night. The first cut faded this in with the sun-only
        // daylight factor, and the owner's 05:25 Takistan shots showed why that is not enough:
        // at dawn the ramp is already up while the real light is at its weakest, so the dead
        // trees went white and the bushes lit up exactly when they should be darkest. Legacy
        // compensation emissive is not radiance at noon either -- it is merely invisible under
        // full sun. Fixtures (MATFLAG_NIGHT_EMITTER) are the only exemption.
        m.emissive = m.emissive * clamp(emissive_night, 0.0, 1.0);
    }
    m.sun_ambient = material.sun_ambient.rgb;
    m.sun_diffuse = material.sun_diffuse.rgb;
    m.light_diffuse = material.light_diffuse.rgb;
    m.light_ambient = material.light_ambient.rgb;
    m.specular = material.specular.rgb;
    m.local_specular = material.local_specular.rgb;
    m.spec_power = material.specular.w;
    // The per-draw path carries no specular map: this is the ~7 sections a frame that reach
    // DrawSectionTL, not the retained set. 1.0 leaves the flat constant exactly as before.
    m.spec_mask = 1.0;
    // The direct path has no retained SMDI binding. Show the constant specular lane and its
    // normalized power, with blue = 0 to make the absence of a texel map explicit.
    let specular_debug = vec3<f32>(
        clamp(max(material.specular.r, max(material.specular.g, material.specular.b)), 0.0, 1.0),
        clamp(material.specular.w / 128.0, 0.0, 1.0), 0.0);
    // Stage 2 MapType gate: foliage lighting (leaf SSS + canopy AO) applies only to real
    // VEGETATION cutouts, so roads / characters / fences don't pick up the leaf look. The object's
    // MapType is published per draw in the free .w of the sun-ambient material lane (only .rgb is
    // read for shading) — set from GCurrentIsVegetation in EngineWgpu::DrawSectionTL. The
    // alpha-test discard above stays keyed on alpha_ref (every cutout still discards).
    let veg_cutout = material.sun_ambient.w > 0.5 && alpha_ref > 0.0;
    // The mesh has no authored tangent stream in this production path. Recover a
    // tangent frame from the actual interpolated UV0/world-position derivatives;
    // this is deterministic and lets Stage1 use its declared `tex` source without
    // fabricating tangent handedness in the canonical model representation.
    let geometric_normal = surface_normal(in.normal, in.world_pos, dwx, dwy);
    var shading_normal = geometric_normal;
    if (material.normal_map.z > 0.5) {
        let authored_frame = dot(in.tangent, in.tangent) > 1e-10 && dot(in.binormal, in.binormal) > 1e-10;
        let duv_dx = dpdx(in.uv);
        let duv_dy = dpdy(in.uv);
        // Scale-invariant cotangent frame; see the matching block in gpu_driven.wgsl.
        // The previous form multiplied the world-space and UV footprints together and
        // tested the result against an absolute epsilon, so the frame was rejected --
        // and the normal map silently dropped -- once a surface came close enough that
        // both footprints were small. Crossing with the geometric normal first also
        // makes the frame perpendicular to it by construction, which is what the
        // orthonormalisation step used to be for.
        var tangent_raw = cross(dwy, geometric_normal) * duv_dx.x + cross(geometric_normal, dwx) * duv_dy.x;
        var bitangent_raw = cross(dwy, geometric_normal) * duv_dx.y + cross(geometric_normal, dwx) * duv_dy.y;
        if (authored_frame) {
            // An authored frame is already the right shape; only make it perpendicular.
            let tangent_ortho = in.tangent - geometric_normal * dot(geometric_normal, in.tangent);
            tangent_raw = tangent_ortho;
            bitangent_raw = cross(geometric_normal, tangent_ortho) *
                            select(-1.0, 1.0, dot(cross(geometric_normal, tangent_ortho), in.binormal) >= 0.0);
        }
        let frame_scale2 = max(dot(tangent_raw, tangent_raw), dot(bitangent_raw, bitangent_raw));
        if (frame_scale2 > 0.0) {
            let inv_scale = inverseSqrt(frame_scale2);
            let tangent = tangent_raw * inv_scale;
            let bitangent = bitangent_raw * inv_scale;
            shading_normal = normalize(tangent * normal_ts.x + bitangent * normal_ts.y + geometric_normal * normal_ts.z);
        }
    }
    // DZ-005 -- reflective (CalmWater) draws OVERRIDE the tangent-space result with a
    // world-space sample of the same normal map; see water_ripple_normal. Kept in step with
    // gpu_driven.wgsl's copy on purpose: DayZ water reaches the retained path in practice, but
    // the two paths must not disagree about what a water surface looks like.
    if ((material_flags(material) & MATFLAG_REFLECTIVE) != 0u && frame.wateranim.y > 0.0
        && !matdbg_flag(MATDBG_DISABLE_FRESNEL_ENV)) {
        let t = frame.wateranim.x;
        shading_normal = water_ripple_normal(
            normal_packed >> 3u, geometric_normal, in.world_pos, dwx, dwy,
            water_drift(WATER_DRIFT_DIR_A, WATER_DRIFT_SPEED_A, WATER_RIPPLE_PERIOD_A, t),
            water_drift(WATER_DRIFT_DIR_B, WATER_DRIFT_SPEED_B, WATER_RIPPLE_PERIOD_B, t),
            frame.wateranim.y);
    }
    // Debug views are before shared lighting, fog and environment reflection. That keeps the
    // outputs useful for isolating source/material mistakes instead of mixing them with exposure.
    if (material_view == MATDBG_VIEW_BASE_COLOR) {
        // Same contract as the retained path: what shade() consumes.
        var base_dbg = base.rgb;
        if (linear > 0.5) {
            base_dbg = srgb_to_linear(base_dbg);
        }
        return vec4<f32>(base_dbg, out_a);
    }
    if (material_view == MATDBG_VIEW_NORMAL) {
        return vec4<f32>(normal_ts * 0.5 + vec3<f32>(0.5), out_a);
    }
    if (material_view == MATDBG_VIEW_AMBIENT_SHADOW) {
        return vec4<f32>(vec3<f32>(gtao_ao(in.clip.xy)), out_a);
    }
    if (material_view == MATDBG_VIEW_SPECULAR_GLOSS) {
        return vec4<f32>(specular_debug, 1.0);
    }
    if (material_view == MATDBG_VIEW_UV) {
        let uv0 = fract(in.uv);
        return vec4<f32>(uv0.x, uv0.y, 0.0, out_a);
    }
    if (material_view == MATDBG_VIEW_LIGHTING) {
        base = vec4<f32>(1.0, 1.0, 1.0, base.a);
    }
    var rgb = shade(
        // `geometric_normal` rides along beside the shading normal so the directional sky
        // ambient can recover how far the normal map turned this pixel; see shade()'s
        // `geo_normal` parameter.
        base.rgb, m, shading_normal, geometric_normal, in.world_pos, in.fog, dwx, dwy, linear,
        // The per-draw path has no retained model id, so no baked volume applies.
        1.0, vec3<f32>(0.0), foliage_shadow_ao,
        // shade()'s `is_translucent` damps the sky-dome ambient to 0.2 so a canopy pane
        // reads as GLAZING rather than a lit diffuse dome. `translucent` alone cannot carry
        // that: it is a PIPELINE override (line 242) meaning "this draw went through the
        // alpha-blend pipeline", and shading.wgsl's own note at the damp site says so --
        // "`is_translucent` means 'this draw is alpha-blended', not 'this is glass'".
        //
        // MEASURED (PoseidonTools model inspect --classify, O.pbo road/): every original
        // OFP/CWA road surface is such a draw. asf_new.paa / sil_new.paa / ces_hned.paa all
        // classify BLEND, so asf/sil/ces/kos/kr_new_* -- the whole Everon/Malden/Kolgujev
        // road set -- lost 80% of its sky ambient as if it were a cockpit window. It is not
        // remotely glass by the engine's own measured discriminator: MAT-051 demands
        // pctClear < 5, pctPartial > 80, aMean < 128 (EngineWgpu.cpp:4956), and asf_new.paa
        // reports pctClear 11.5, pctPartial 10.4, aMean 212 -- an opaque asphalt strip with
        // a soft alpha edge, i.e. a road DECAL.
        //
        // So key the damp on the flag that already answers "is this glass" -- the same
        // MAT-051 classification that grants the sheen -- instead of on the blend pipeline.
        // Narrowing only: a draw that gets MATFLAG_GLASS is by construction one that also
        // set `translucent`, so every genuine glass pane keeps exactly the shading it had.
        // The retained path already passes `false` here (gpu_driven.wgsl:933), so this also
        // removes a standing disagreement between the two paths about the same surface.
        // `translucent` is kept in the conjunction rather than dropped: an override the
        // module no longer references can be eliminated before pipeline creation, and the
        // constant mod.rs supplies for it (gfx3d/mod.rs:4517) then has no target.
        veg_cutout, translucent > 0.5 && (material_flags(material) & MATFLAG_GLASS) != 0u, veg_cutout,
        material.sun_ambient.w > 0.5,
        (material_flags(material) & MATFLAG_COCKPIT) != 0u, in.clip.xy,
        !(veg_cutout && foliage_screen_ao < 0.5),
        direct_snow_receiver(in), 0.0,
        direct_tree_snow_cover(in),
        ground_surface,
        select(0.0, snow_depth(in.world_pos.xz + frame.cam_pos.xz), ground_surface),
    );
    if (material.local_specular.w == -1.0) {
        // Only exact authored animated stock muzzle sections receive this value.
        // Evaluate shade uniformly first: its texture derivatives stay valid.
        // The flame itself is radiance, not diffuse geometry receiving sun/SH.
        rgb = stock_muzzle_flash_radiance(base.rgb, linear);
        if (frame.params.fog_enabled >= 1.5) {
            rgb = apply_fog(rgb, in.world_pos);
        } else {
            var fog_rgb = frame.fog_color.rgb;
            if (linear > 0.5) { fog_rgb = srgb_to_linear(fog_rgb); }
            rgb = mix(fog_rgb, rgb, in.fog);
        }
    }
    // REN-ATM-001: the per-draw twin of gpu_driven.wgsl's atmosphere census -- same words, same
    // witness. Both paths draw vegetation (retained trees go GPU-driven, scripted/near ones do
    // not), so counting only one of them would leave exactly the gap the measurement exists to
    // close. `alpha_ref` is the pipeline override here, not a per-section field.
    if (atmo_applied()) {
        census(select(4u, 5u, alpha_ref > 0.0));
        if (veg_cutout) {
            census(6u);
        }
    } else {
        census(7u);
    }
    // DZ-003 -- Fresnel-weighted sky reflection for materials that declared themselves
    // reflective. Every other draw takes the branch not-taken and is bit-identical to
    // before. `linear` gates it to the HDR target: the env map is linear radiance that
    // routinely exceeds 1.0, and on an LDR target it would clip to white.
    //
    // DZ-007: and only while the dev panel's "Disable Fresnel / Environment" checkbox is
    // clear. `frame.matdbg` is documented as the RETAINED path's lane, but it is the dev
    // panel's single copy of this state in the shared frame UBO and both paths can see it --
    // the per-draw path's own per-instance debug lane is a legacy of the material-debug
    // slice and would give the panel two switches for one checkbox.
    var out_rgb = rgb;
    if ((material_flags(material) & MATFLAG_REFLECTIVE) != 0u && linear > 0.5
        && !matdbg_flag(MATDBG_DISABLE_FRESNEL_ENV)) {
        out_rgb = env_reflection(out_rgb, shading_normal, in.world_pos, in.fog);
    }
    // MAT-051: a glass pane keeps a minimum presence head-on. Applied AFTER the
    // reflection so the floor composites the reflected colour, not the bare albedo.
    var out_a2 = out_a;
    if ((material_flags(material) & MATFLAG_GLASS) != 0u) {
        out_a2 = max(out_a2, GLASS_MIN_ALPHA);
    }
    // Bit 6 marks only prepassed world draws whose colour is replayed after
    // atmospheric compositing. It does not turn them into alpha/glass materials.
    if ((translucent > 0.5 || (material_flags(material) & 64u) != 0u) && linear > 0.5) {
        out_rgb = out_rgb + surface_scattering(in.world_pos);
    }
    return vec4<f32>(out_rgb, out_a2);
}

// Blended surfaces do not write scene depth. Integrate only the air IN FRONT
// of this fragment; compositing the screen-space rays afterwards sees its background.
fn surface_scattering(pos: vec3<f32>) -> vec3<f32> {
    let end = min(length(pos), surface_rays.tune.z);
    if (surface_rays.tune.x <= 0.0 || end <= 1.0) {
        return vec3<f32>(0.0);
    }
    let ray = normalize(pos);
    let sun = normalize(surface_rays.sun.xyz);
    let g = clamp(surface_rays.shape.x, 0.0, 0.95);
    let phase = (1.0 - g * g) / (12.566370614 * pow(max(1.0 + g * g - 2.0 * g * dot(ray, sun), 1e-4), 1.5));
    let steps = i32(clamp(surface_rays.tune.w, 4.0, 48.0));
    var previous = 0.0;
    var transmit = 1.0;
    var scatter = 0.0;
    for (var i = 0; i < steps; i = i + 1) {
        let u = f32(i + 1) / f32(steps);
        let next = end * u * u;
        let rel = ray * ((previous + next) * 0.5);
        let world = frame.cam_pos.xyz + rel;
        let sigma = surface_rays.tune.y * exp(-max(world.y, 0.0) / max(surface_rays.color.w, 1.0));
        let segment = exp(-sigma * (next - previous));
        let ground = world.xz - sun.xz * (world.y / max(sun.y, 0.05));
        let uv = (ground - terrain_shadow_map.cloud_shadow.xy) * terrain_shadow_map.cloud_shadow.z;
        var cloud = 1.0;
        if (all(uv >= vec2<f32>(0.0)) && all(uv <= vec2<f32>(1.0))) {
            let tap = textureSampleLevel(cloud_shadow_tex, terrain_shadow_samp, uv, 0.0);
            let strength = terrain_shadow_map.cloud_shadow.w;
            var t = tap.r;
            if (strength > 0.25) { t = clamp(1.0 - (1.0 - t) / strength, 0.0, 1.0); }
            if (strength <= 0.001) { t = 1.0; }
            let visibility = mix(1.0, t, surface_rays.shape.y) * tap.g;
            let edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0;
            cloud = mix(visibility, 1.0, smoothstep(0.85, 1.0, edge));
        }
        let terrain = terrain_sun_shadow(world.xz, world.y);
        let object = shadow_strength(rel, sun, 1.0, vec3<f32>(0.0), vec3<f32>(0.0));
        scatter = scatter + transmit * cloud * (1.0 - max(terrain, object)) * (1.0 - segment);
        transmit = transmit * segment;
        previous = next;
    }
    return surface_rays.color.rgb * (scatter * phase * surface_rays.tune.x * surface_rays.sun.w);
}

// Depth + normal prepass fragment (docs/depth-prepass-plan.md). Reuses vs_main/
// vs_skinned unchanged (same VS + override constants -> bit-for-bit depth as fs_main),
// and writes ONLY the view-space octahedral normal into the Rg16Float G-buffer; depth is
// written by the fixed-function stage. Cutout foliage (alpha_ref > 0) applies the SAME
// hard discard as fs_main so prepass coverage matches the colour pass by construction.
// `alpha_ref` is a pipeline-override constant, so the branch is compile-time uniform and
// the textureSample stays legal.
@fragment
fn fs_prepass(in: VsOut) -> @location(0) vec2<f32> {
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);
    let surface_n = surface_normal(in.normal, in.world_pos, dwx, dwy);
    let snow_cover = object_snow_coverage(in.world_pos, surface_n, direct_snow_receiver(in) && linear > 0.5, 0.0);
    let snow_surface_n = object_snow_normal(in.world_pos, surface_n, surface_n, dwx, dwy, snow_cover);
    census(2u);
    if (alpha_ref > 0.0) {
        let packed = bitcast<u32>(materials[in.instance].emissive.w);
        let dims0 = vec2<f32>(textureDimensions(textures[packed >> 3u]));
        let a = mip_alpha_boost(textureSampleBias(textures[packed >> 3u], samplers[packed & 7u], in.uv, frame.renscale.x).a, in.uv, dims0,
                                cutout_mip_alpha);
        if (a < alpha_ref) {
            discard;
        }
    }
    // in.normal is world space (camera-relative world uses the same orientation); the
    // view matrix's translation is zeroed, so multiplying the direction gives view space.
    let n_view = (frame.view * vec4<f32>(snow_surface_n, 0.0)).xyz;
    return oct_encode(normalize(n_view));
}

// Real direct geometry/pose/wind, without shading or reading the target being written.
@fragment
fn fs_weather_depth(in: VsOut) {
    if (alpha_ref > 0.0) {
        let packed = bitcast<u32>(materials[in.instance].emissive.w);
        let dims = vec2<f32>(textureDimensions(textures[packed >> 3u]));
        let alpha = mip_alpha_boost(textureSampleBias(textures[packed >> 3u],
            samplers[packed & 7u], in.uv, frame.renscale.x).a, in.uv, dims, cutout_mip_alpha);
        if (alpha < alpha_ref) { discard; }
    }
}

// A2C prepass twin (MSAA cutout foliage). Same normal output as fs_prepass, but returns a
// vec4 so location(0)'s .a carries the sharpened coverage for alpha-to-coverage — the extra
// .b/.a components are dropped by the Rg16Float attachment write but still drive the coverage
// mask. Emits the SAME coverage as fs_main so the prepass writes depth to exactly the samples
// the colour pass will shade (terrain fills the rest -> no background halo at edges).
@fragment
fn fs_prepass_a2c(in: VsOut) -> @location(0) vec4<f32> {
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);
    let surface_n = surface_normal(in.normal, in.world_pos, dwx, dwy);
    let snow_cover = object_snow_coverage(in.world_pos, surface_n, direct_snow_receiver(in) && linear > 0.5, 0.0);
    let snow_surface_n = object_snow_normal(in.world_pos, surface_n, surface_n, dwx, dwy, snow_cover);
    census(2u);
    census(3u);
    let packed = bitcast<u32>(materials[in.instance].emissive.w);
    let dims0 = vec2<f32>(textureDimensions(textures[packed >> 3u]));
    let alpha = mip_alpha_boost(textureSampleBias(textures[packed >> 3u], samplers[packed & 7u], in.uv, frame.renscale.x).a, in.uv, dims0,
                                cutout_mip_alpha);
    let cov = a2c_coverage(alpha, alpha_ref);
    if (cov <= 0.0) {
        discard;
    }
    let n_view = (frame.view * vec4<f32>(snow_surface_n, 0.0)).xyz;
    let oct = oct_encode(normalize(n_view));
    return vec4<f32>(oct.x, oct.y, 0.0, cov);
}
