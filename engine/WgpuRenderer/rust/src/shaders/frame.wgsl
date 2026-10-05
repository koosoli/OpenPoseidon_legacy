#define_import_path frame
#import layered_fog_optics::{FogConsumer, fog_camera_matches, fog_legacy_closure, fog_surface_open, fog_ray_far}

// Shared group(0): the per-frame camera/environment UBO plus the cascade shadow
// map + comparison sampler. Every 3D pipeline (lit meshes, terrain) binds this
// exact layout, so the structs and bindings live here once. The UBO global is
// deliberately named `frame` so importers can write `frame.proj` etc. unchanged.

struct FrameParams {
    fog_start: f32,
    fog_inv_range: f32,
    fog_enabled: f32, // 0 = off, 1 = on
    shadow_strength: f32,
};

struct ShadowBlock {
    cascade_vp: array<mat4x4<f32>, 4>,
    splits: vec4<f32>, // per-tier select distance (omni radius / frustum eye-depth)
    omni_radius: vec4<f32>,
    ctl: vec4<f32>,  // {count, omni_count, fade_range, bias_const}
    // `ctlb`, not `ctl2`: naga_oil forbids composable-module identifiers ending
    // in a digit (naga's namer reserves numeric suffixes for disambiguation).
    ctlb: vec4<f32>, // {texel_size, darkness, normal_offset_scale, pcf}
    cam_fwd: vec4<f32>, // w: contact filter (0 fixed, 1 full, 2 budget)
    sun_dir: vec4<f32>, // w: tan(sun angular radius)
    // LGT-010: one perspective view-projection per shadowing spot light, and
    // {count, darkness, texel_size, first_layer}. Separate from cascade_vp because
    // these are selected by light index rather than by distance.
    local_vp: array<mat4x4<f32>, 24>,
    local_ctl: vec4<f32>,
};

struct Frame {
    proj: mat4x4<f32>,
    view: mat4x4<f32>,
    fog_color: vec4<f32>,
    params: FrameParams,
    shadow: ShadowBlock,
    cam_pos: vec4<f32>, // world-space camera position (used by the terrain pipeline)
    sun_diffuse: vec4<f32>, // sun light, accommodation folded in (terrain)
    sun_ambient: vec4<f32>,
    sun_dir_world: vec4<f32>, // main light's surface-to-light direction (terrain)
    // inverse(view) * inverse(proj), computed Rust-side in f64 (the reversed-Z / infinite-far
    // proj is ill-conditioned to invert in f32; invert the two SEPARATELY, as the sky does).
    // Unprojects a forward-NDC point vec4(ndc.xy, 1 - stored_depth, 1) to a CAMERA-RELATIVE
    // world position (÷ w). Appended after the WgrCamera bytes in the camera upload, so it is
    // NOT part of the WgrCamera C ABI. Used by water seabed-depth reconstruction (Stage 2);
    // reusable by SSAO / refraction / contact shadows.
    inv_view_proj: mat4x4<f32>,
    // Foliage lighting knobs (docs/foliage-translucency-plan.md), appended after inv_view_proj
    // in the camera upload (mirrors WgrFoliage). Read by shade() for cutout/vegetation draws.
    //   foliage  = (trans_scale, distortion, trans_power, wrap)
    //   foliageb = (ambient_boost, normal_bend[bush], crown_y_offset[bush], fill_fade_end)
    //   foliagec = (gi_strength, tree_bend, tree_crown_y, dusk_curve)
    // `foliageb`/`foliagec` not `foliage2`/`foliage3`: naga_oil forbids composable identifiers
    // ending in a digit.
    //   foliaged = (sway_strength, sway_speed, wind_dir_x, wind_dir_z)
    //   foliagee = (sway_time, wind_gust, sway_leaf, sway_stiffness)
    // VEG-SWAY: geometric wind on vegetation MODELS, read by BOTH object vertex shaders
    // (shader3d::vs_main and gpu_driven::vs_gpu). Until this existed the wgpu path animated
    // grass and nothing else, so a forest stood dead still in a gale. The wind vector is the
    // World/Weather/WindModel sample -- the same one the grass field and the cloud deck read,
    // pushed from EngineWgpu -- so all three lean together instead of each inventing air.
    // `sway_time` rather than reusing `wateranim.x`: that lane is 0.0 on any world whose
    // water params were never set (lib.rs `underwater_params().unwrap_or(0.0)`), which would
    // silently freeze the canopy on exactly the imported worlds this was built for.
    foliage: vec4<f32>,
    foliageb: vec4<f32>,
    foliagec: vec4<f32>,
    foliaged: vec4<f32>,
    foliagee: vec4<f32>,
    // xyz = plane normal, w = offset; zero normal disables clipping. This is appended
    // by Rust, keeping WgrCamera's C++ ABI unchanged.
    clip_plane: vec4<f32>,
    // Screen-space AO gate (docs/screen-space-ao-plan.md), appended after clip_plane.
    //   x = 1 when the GTAO pass ran this frame and its buffer may be read (0 = off)
    //   y = 1 for the raw AO debug view
    //   z = 1 to steer sky irradiance by the bent normal (Stage 2 directional ambient)
    //   w = how much of the NORMAL MAP's angular deviation the directional sky ambient keeps,
    //       [0,1] (WGR_AMBIENT_NORMAL_MAPPED, default 1; 0 = the pre-fix look exactly).
    //       Shares this lane with the bent-normal gate because it modifies the same lookup --
    //       x/y/z say how the ambient DIRECTION is steered by occlusion, w says whether the
    //       per-pixel surface detail survives that steer. Unlike x/y/z it is NOT gated on the
    //       GTAO camera: it is a property of the surface, not of a screen-space buffer.
    //       Read by shading::shade; see the block there for why it is a rotation, not a blend.
    gtao: vec4<f32>,
    // Interior sky visibility (docs/interior-sky-visibility-plan.md), appended after gtao.
    //   skyvis_vp[i]  = ABSOLUTE world -> sky-map layer i's ortho clip space (w = 1, ortho)
    //   skyvis_dir[i] = that layer's direction toward the sky (xyz, world), w = its cosine weight
    //   skyvis        = (gate, debug, strength, floor)
    //   skyvisb       = (kernel_uv, bias_ndc, directional, unused)
    // The DIRECTIONS ride the UBO rather than living as constants here: they and the matrices are
    // one consistent set produced together on the CPU, and a WGSL copy of them is exactly the
    // "two twins of one buffer with nothing checking they agree" trap.
    // `skyvisb` not `skyvis2`: naga_oil forbids composable identifiers ending in a digit.
    skyvis_vp: array<mat4x4<f32>, 5>,
    skyvis_dir: array<vec4<f32>, 5>,
    skyvis: vec4<f32>,
    skyvisb: vec4<f32>,
    // Stage 2 (per-model BAKED volumes): (gate, strength, floor, debug). Its own lane rather
    // than sharing `skyvis` so the two implementations can be toggled independently and A/B'd
    // live — which is the only way to judge which one looks right.
    skyvisc: vec4<f32>,
    // Material Debug (dev panel, Materials tab) for the RETAINED path:
    //   x = view      0 full material, 1 base colour only, 2 decoded normal
    //   y = flags     bit0 disable normal map, bit1 invert normal Y,
    //                 bit2 compose Multi layers (SET = compose, the default)
    // These are presentation switches, so they live here and are read per fragment
    // rather than being baked into the material records at registration -- baking
    // them is why the toggles did nothing on a world that was already loaded.
    // REN-GI-001: the probe volume. gi = (enabled, weight, interior AO mix, debug);
    // giorigin = (world origin xyz of probe (0,0,0), spacing). Two lanes before matdbg, which
    // Rust addresses from the END of the block, so the tail offsets there moved by 32 bytes.
    gi: vec4<f32>,
    giorigin: vec4<f32>,
    matdbg: vec4<f32>,
    // DZ-005 — DayZ water surface animation, read by BOTH object paths for sections that
    // declared themselves reflective (the CalmWater family). One frame-global lane rather
    // than a per-material field, because Enoch ships exactly ONE river flow map for all 804
    // river segments and the ripple constants are identical across every water `.emat`.
    //   x = engine time in seconds (WgrWaterParams.time, i.e. Glob.time — pauses when it does)
    //   y = ripple strength; 0 disables the world-space normal sample entirely (A/B switch)
    //   z = flow strength (StreamSpeedInfl); 0 = no scrolling along the flow map
    //   w = the flow map's BINDLESS texture slot, 0 when the world ships none (Chernarus)
    // `wateranim` and not `water_anim0`/`water2`: naga_oil forbids composable identifiers
    // ending in a digit (see `ctlb` above).
    wateranim: vec4<f32>,
    // Normal-map RELIEF UNDER AMBIENT LIGHT — the two terms that make a normal map read when
    // the sun is not carrying it. Appended after wateranim; it is now the LAST lane.
    //   x = cavity exponent   (WGR_NORMAL_CAVITY,  default 2, 0 = off/bit-identical)
    //   y = sky-specular gain (WGR_SKY_SPECULAR,   default 1, 0 = off/bit-identical)
    //   z, w = unused
    // Its own lane rather than the spare halves of `matdbg`: those are DEBUG switches the dev
    // panel owns and resets, and a lighting term parked in one would be turned off by a UI
    // action that has nothing to do with it. Read by frame::normal_map_cavity /
    // frame::sky_specular_split, called from shading::shade and terrain's fs_terrain.
    relief: vec4<f32>,
    // FAR-FOG CLOSURE, the LAST lane (appended by Rust; WgrCamera's C ABI is unchanged).
    //   x = the fraction of the draw distance at which fog reaches FULL. 1.0 is the behaviour
    //       from before this lane existed, where fog saturates only exactly at the far plane.
    //       Lower closes the horizon earlier, which is what hides the edge of the drawn world.
    //   y = OnSurface (road/decal) per-pixel depth lift, flat metres
    //   z = the same, per metre of distance, BEFORE the grazing division
    //   w = ceiling on the total lift, as a fraction of the distance
    // Every lane reads 0 as "use the shipped default", so a zero-filled or never-written camera
    // behaves exactly as it did before the lane existed. The three road knobs live here rather
    // than as pipeline override constants because they were CONSTANTS: tuning them cost a
    // rebuild and a redeploy per value, which is how a ten-minute sweep became an afternoon.
    // See road_lift_flat / road_lift_per_m / road_lift_max_frac below.
    // (No longer the last lane — `renscale` was appended after it, REN-TEMP-001D.)
    fogfar: vec4<f32>,
    // REN-TEMP-001D — render-scale-aware texture LOD, the LAST lane (appended by Rust; the
    // WgrCamera C ABI is unchanged). When the 3D scene renders below the output resolution
    // (WGR_RENDER_SCALE), implicit-derivative texture sampling picks mips for the REDUCED
    // pixel density and the upscaled image goes soft. The fix is the standard one: bias the
    // sampled LOD by log2(render/output).
    //   x = mip bias, log2(render_width / output_width), <= 0 (0 at native scale)
    //   y = gradient scale, 2^x — multiplies EXPLICIT gradients (textureSampleGrad ignores
    //       bias, so the terrain path scales its ddx/ddy instead; same optics, other API)
    //   z = AST-012A mip-feedback ROTATION: this frame's group index
    //   w = AST-012A mip-feedback GROUP COUNT; 0 disables the feedback write entirely
    // The rotation pair rides here rather than in the feedback buffer's own header because
    // every fragment tests it: a uniform-buffer read folds into a constant for the draw, where
    // a storage load would be a memory access per fragment for a value that never varies.
    // A zero-filled camera reads bias 0 / grad_scale() 1.0 and no feedback — bit-identical to
    // pre-lane.
    renscale: vec4<f32>,
    // Renderer-private object snow, appended without changing WgrCamera's public ABI:
    // actual deposit metres, snowline height (-1 disabled), ramp range, snowline depth.
    // Raised roofs use their own fragment height and never the terrain shelter/track deficit.
    snow_surface: vec4<f32>,
    // Shared ground film: retained rain wetness, effective liquid rain, sea level, simulation time.
    // Renderer-private tail; the public WgrCamera layout is unchanged.
    ground_weather: vec4<f32>,
    // Physical vertical weather depth, independent of artistic sky/AO settings.
    // Absolute-world matrix of the map actually recorded before colour this frame.
    weather_vp: mat4x4<f32>,
    // ready, vertical bias in NDC, edge safety margin in UV, physical feature active.
    // Pending/aborted/disabled maps are ready=0 and provide no exposure proof.
    weather_cover: vec4<f32>,
    // This physical cull's actual GPU counters: opaque args, cutout args,
    // total record reservations, per-variant argument capacity. No fragment storage.
    weather_gpu: vec4<u32>,
    // Independent opt-in far physical map: private Rust tail.
    weather_far_vp: mat4x4<f32>,
    weather_far_cover: vec4<f32>,
    weather_far_gpu: vec4<u32>,
};

// REN-TEMP-001D helpers. Material/albedo/normal/cutout sampling uses `mip_bias()` via
// textureSampleBias; explicit-gradient sampling multiplies both gradients by `grad_scale()`.
// LUT / env / shadow / SampleLevel lookups take NEITHER — their LOD is not a function of
// screen pixel density.
fn mip_bias() -> f32 {
    return frame.renscale.x;
}
fn grad_scale() -> f32 {
    return select(frame.renscale.y, 1.0, frame.renscale.y <= 0.0);
}

// ---------------------------------------------------------------------------
// VEG-SWAY -- geometric wind on vegetation MODELS.
//
// Shared by both object vertex shaders so a tree cannot sway on one path and stand still on
// the other; that split is exactly the class of bug this codebase keeps paying for (the
// per-draw and GPU-driven paths already have to agree about conform and canopy normals).
//
// The model is deliberately DATA-FREE: it needs no vertex colour, no extra UV set and no bone
// weights, only the vertex's own MODEL-space Y. OFP, Arma and Enfusion vegetation is all
// authored with its origin on the ground, so model Y already IS "height above the trunk
// base". That matters because the .xob reader deliberately skips bones -- anything that
// needed a skinned sway channel would not have the data to drive it.
//
// `height_ref` is the canopy height (m) that `sway_strength` is quoted at, so the same
// amplitude reads sensibly on a 1 m bush and a 25 m spruce.
const VEG_SWAY_HEIGHT_REF: f32 = 12.0;

// Native Reforger vegetation can arrive as separate authored objects for the solid stem and
// the crown. Their origins are not guaranteed to be byte-identical, even though they belong to
// one tree. Use a small world-space phase anchor for the coherent mode so those parts share the
// same wind sample without making neighbouring trees move as one patch. Legacy OFP foliage keeps
// its per-object phase for compatibility.
const VEG_COHERENT_PHASE_CELL: f32 = 2.0;

fn veg_sway_phase(phase_xz: vec2<f32>, leaf: f32) -> vec2<f32> {
    if (leaf < 0.0) {
        let cell = vec2<f32>(VEG_COHERENT_PHASE_CELL, VEG_COHERENT_PHASE_CELL);
        return floor(phase_xz / cell) * cell + cell * 0.5;
    }
    return phase_xz;
}

// `model_y`  : the vertex's UNDEFORMED model-space height (metres above the model origin).
// `phase_xz` : an ABSOLUTE world XZ used only to decorrelate neighbouring plants. Absolute,
//              not camera-relative: both shaders work camera-relative, and seeding the phase
//              from a camera-relative position makes every plant swim as the camera walks.
// `leaf`     : 1 on cutout (leaf) sections, 0 on the solid trunk -- flutter is leaves-only,
//              otherwise a trunk visibly buzzes.
//              -1 selects the native tree's coherent, material-independent shear.
// Returns a WORLD-space horizontal offset (Y is always 0: this bends, it does not lift).
fn veg_sway_offset(model_y: f32, phase_xz: vec2<f32>, leaf: f32) -> vec3<f32> {
    let amp = frame.foliaged.x;
    if (amp <= 0.0) {
        return vec3<f32>(0.0, 0.0, 0.0);
    }
    let t = frame.foliagee.x * frame.foliaged.y;
    let dir = vec2<f32>(frame.foliaged.z, frame.foliaged.w);
    // Height response. Clamped so a merged forest patch or a mis-scaled import cannot throw a
    // vertex hundreds of metres -- an unbounded pow() here is a whole-world corruption, not a
    // cosmetic error.
    let hn = clamp(max(model_y, 0.0) / VEG_SWAY_HEIGHT_REF, 0.0, 3.0);
    // Exponent > 1 keeps the lower trunk planted, so the tree bends instead of sliding.
    // Native trees lack imported branch attachment weights. An affine shear
    // commutes with mesh interpolation, so a coarse trunk and a fine crown stay
    // attached. Negative leaf is this mode on EVERY section, not an alpha flag.
    let coherent_hs = model_y / VEG_SWAY_HEIGHT_REF * (1.6 / max(frame.foliagee.w, 0.25));
    let hs = select(pow(hn, frame.foliagee.w), coherent_hs, leaf < 0.0);
    let ph = dot(veg_sway_phase(phase_xz, leaf), vec2<f32>(0.37, 0.29));
    // Two incommensurate frequencies, so the canopy builds and decays instead of ticking like
    // a metronome. The gust term rides in from the wind AUTHORITY (WindSample::gustFraction),
    // the same number the grass field and the cloud deck answer to.
    let gust = 1.0 + 0.65 * frame.foliagee.y;
    let s = sin(t * 0.9 + ph) * 0.65 + sin(t * 1.63 + ph * 1.7) * 0.35;
    var bend = dir * (amp * hs * s * gust);
    if (leaf > 0.0) {
        // Leaf flutter: small, fast, and ACROSS the wind as well as along it. Along-wind-only
        // flutter reads as the whole tree shaking; the cross term is what reads as leaves.
        let f = frame.foliagee.z * leaf;
        let perp = vec2<f32>(-dir.y, dir.x);
        // Cutout sections also contain branch attachments, not just leaf tips.
        // Keep their extra motion centimetre-sized rather than scaling an entire
        // crown away from its solid trunk in tall trees / strong wind. Shared
        // trunk bending above remains unchanged; velocity paths use the same cap.
        bend = bend + (dir * sin(t * 5.7 + ph * 3.1) + perp * sin(t * 4.3 + ph * 2.3)) *
                          (min(amp * hs * 0.22 * gust, 0.025) * f);
    }
    return vec3<f32>(bend.x, 0.0, bend.y);
}

// Material Debug accessors. `flags` is a bitfield carried in a float lane, so it is
// rounded before masking rather than compared.
fn matdbg_view() -> u32 { return u32(frame.matdbg.x + 0.5); }
fn matdbg_flag(bit: u32) -> bool { return (u32(frame.matdbg.y + 0.5) & (1u << bit)) != 0u; }
// Stable view IDs shared by shader3d.wgsl, gpu_driven.wgsl and Engine.hpp. Keep these
// append-only: captures and command-line automation upload the integer directly.
const MATDBG_VIEW_FULL: u32 = 0u;
const MATDBG_VIEW_BASE_COLOR: u32 = 1u;
const MATDBG_VIEW_NORMAL: u32 = 2u;
const MATDBG_VIEW_AMBIENT_SHADOW: u32 = 3u;
const MATDBG_VIEW_SPECULAR_GLOSS: u32 = 4u;
const MATDBG_VIEW_UV: u32 = 5u;
const MATDBG_VIEW_LIGHTING: u32 = 6u;
const MATDBG_DISABLE_NORMAL: u32 = 0u;
const MATDBG_INVERT_NORMAL_Y: u32 = 1u;
const MATDBG_COMPOSE_MULTI: u32 = 2u;
// DZ-007 -- the dev panel's "Disable Fresnel / Environment" checkbox
// (Engine::MaterialDebugSettings::disableFresnelEnvironment). SET suppresses both the
// Fresnel sky reflection and the world-space water ripple, returning a CalmWater surface to
// its pre-DZ-003 look at runtime with no restart. Read by both object paths.
const MATDBG_DISABLE_FRESNEL_ENV: u32 = 3u;
// The dev panel's "Disable SMDI / Specular-Gloss" (MaterialDebugSettings::disableSpecularGloss,
// WGR_MATERIAL_DISABLE_SPECULAR). SET leaves `specular_slot` unsampled so the section shades
// with its flat specular constant, the state it was in before the map was bound. Read by the
// GPU-driven path only: the per-draw path carries no specular map to begin with.
const MATDBG_DISABLE_SPECULAR: u32 = 4u;

// One frame-global point or spot light. Positions are ABSOLUTE world space so a
// single upload serves every camera; the shader reconstructs the camera-relative
// offset via frame.cam_pos. Colours are pre-scaled by NightEffect on the CPU
// (fade out by day). Matches the C ABI WgrLight.
struct Light {
    pos: vec4<f32>,     // xyz = world-absolute position, w = start-attenuation distance
    diffuse: vec4<f32>, // rgb = diffuse * nightEffect
    ambient: vec4<f32>, // rgb = ambient * nightEffect
    dir: vec4<f32>,     // xyz = beam direction (spot), w = isSpot (1) else 0
};

@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var shadow_map: texture_depth_2d_array;
@group(0) @binding(2) var shadow_samp: sampler_comparison;
// Frame-global light store, shared by the lit-mesh + terrain pipelines. The
// active light count for this camera rides in frame.cam_pos.w (the buffer itself
// is a fixed capacity, so its length is not the count).
@group(0) @binding(3) var<storage, read> lights: array<Light>;

// Long-range terrain sun-shadow mask (terrain_shadow.wgsl compute sweep), promoted
// into the shared frame group so BOTH terrain and lit meshes sample it — that is
// how objects (infantry, buildings, aircraft) receive a mountain's cast shadow, not
// just the terrain. Per column it stores the world height below which that column is
// terrain-shadowed: .r = ceiling, .g = penumbra half-width (m), .b = strength; a
// point at (xz, y) is occluded by how far y sits below the ceiling. `map` takes a
// world-xz to the mask's [0,1] UV; enabled = 0 until a heightmap is loaded.
struct TerrainShadowMap {
    origin: vec2<f32>,     // world_origin
    inv_span: vec2<f32>,   // 1 / (hm_dims * terrain_grid): world-xz -> [0,1] over the map
    half_texel: vec2<f32>, // 0.5 / mask_dims
    enabled: f32,
    // Sky-visibility (AO) controls (docs/sky-visibility-ambient-plan.md): strength scales the effect
    // (0 = off), floor keeps a minimum ambient in fully-occluded columns.
    sky_vis_strength: f32,
    sky_vis_floor: f32,
    sky_vis_debug: f32,    // 1 = terrain outputs the raw sky-view factor as greyscale
    sky_vis_contrast: f32, // occ = 1 - pow(V, contrast); >1 deepens the AO for near-1 V
    // Existing offset44 padding: opt-in wet-soil colour enum0..4, default0.
    pad_c: f32,
    // CLD-020: xy = the cloud transmittance map's snapped world-xz min corner, z = 1/span (m),
    // w = strength (0 = disabled -> fully lit).
    cloud_shadow: vec4<f32>,
};
@group(0) @binding(4) var terrain_shadow_mask: texture_2d<f32>;
@group(0) @binding(5) var terrain_shadow_samp: sampler;
@group(0) @binding(6) var<uniform> terrain_shadow_map: TerrainShadowMap;
fn wet_soil_debug_mode() -> u32 {
    let mode = terrain_shadow_map.pad_c;
    if (!(mode >= 1.0 && mode <= 4.0 && floor(mode) == mode)) { return 0u; }
    return u32(mode);
}

// Aerial-perspective froxel volume (filled by cs_froxel in sky.wgsl): XY = screen,
// Z = distance with a squared distribution. rgb = in-scattered light toward the camera,
// a = transmittance from the camera to that froxel. The forward shaders apply fog with
// ONE trilinear tap here — every fragment fogs by its OWN distance, so transparents and
// foliage are correct and 2D (which never samples this) is simply never fogged. The
// clamping sampler is shared with the terrain mask (binding 5).
@group(0) @binding(21) var layer_fog_tex: texture_3d<f32>;
@group(0) @binding(22) var<uniform> layer_fog_packet: FogConsumer;
@group(0) @binding(23) var layer_fog_solar_tex: texture_3d<f32>;
@group(0) @binding(24) var layer_fog_native_tex: texture_2d<f32>;
@group(0) @binding(7) var froxel_tex: texture_3d<f32>;
@group(0) @binding(8) var froxel_samp: sampler;

// Diffuse sky irradiance as 9 spherical-harmonic RGB coefficients, projected from the sky
// reflection env map each frame (sky_sh.wgsl / Sky::render_sh). The lit-mesh + terrain fragment
// shaders evaluate directional ambient from these on the sky-lit path (sky_irradiance), replacing
// the old flat ambient fill. rgb per coeff; w padding.
struct SkySh {
    c: array<vec4<f32>, 9>,
};
@group(0) @binding(9) var<uniform> sky_sh: SkySh;

// Coarse sky-visibility (sky-view factor) mask: R8Unorm, one bilinear tap gives the cosine-weighted
// fraction of sky a terrain column can see (1 = open, 0 = fully occluded). Terrain-owned, produced by
// the CPU horizon scan (terrain/skyvis.rs); sampled with the terrain-shadow sampler (binding 5) and
// mapping (binding 6). See terrain_sky_visibility / sky_vis_ao below.
@group(0) @binding(10) var terrain_skyvis_mask: texture_2d<f32>;

// Screen-space ambient occlusion (GTAO + bilateral blur, gfx3d/gtao*.wgsl) at render resolution:
//   rgb = bent normal, VIEW space, unit length — the average direction light still reaches this
//         pixel from (Stage 2)
//   a   = ambient visibility in [0,1], 1 = unoccluded
// Gfx3d-owned, produced from the depth+normal prepass each frame before the colour pass. Read
// with textureLoad at the fragment's OWN pixel — it is already a per-pixel screen-space quantity,
// and under MSAA every covered sample of a pixel legitimately shares one value (plan §5).
// See gtao_ao / gtao_bent_normal_world below.
@group(0) @binding(11) var gtao_tex: texture_2d<f32>;

// Interior sky-visibility map (gfx3d/sky_vis.rs): a top-down orthographic DEPTH map of the
// retained object set over a box around the camera, Depth32Float, forward-Z (0 = top of the box).
// Sampled with the CASCADE comparison sampler at binding 2 — its LessEqual compare is exactly
// "is my depth at or in front of the stored occluder", i.e. "can I see the sky", and the hardware
// 2x2 PCF gives each tap a sub-texel gradient for free. See interior_sky_reach below.
@group(0) @binding(12) var interior_sky_map: texture_depth_2d_array;
// CLD-020: sun transmittance through the cloud deck, one texel per world square, written each
// frame by cs_cloud_shadow. Sampled with the clamping terrain sampler (binding 5).
@group(0) @binding(13) var cloud_shadow_tex: texture_2d<f32>;
// REN-GI-001: the probe volume, six y-slabs of 8 (ambient-cube faces +X -X +Y -Y +Z -Z),
// each texel irradiance/pi. Sampled with the froxel's linear sampler.
@group(0) @binding(14) var gi_probes: texture_3d<f32>;
// REN-GI-003: the matching per-face distance moments (mean r, mean r^2), point-sampled.
@group(0) @binding(15) var gi_probe_dist: texture_3d<f32>;

struct SurfaceRays {
    sun: vec4<f32>,
    color: vec4<f32>,
    tune: vec4<f32>,
    shape: vec4<f32>,
};
@group(0) @binding(16) var<uniform> surface_rays: SurfaceRays;
@group(0) @binding(17) var monitor_scene: texture_2d<f32>;
@group(0) @binding(18) var weather_depth_map: texture_depth_2d;
@group(0) @binding(20) var weather_far_depth_map: texture_depth_2d;
@group(0) @binding(19) var ground_sky_env: texture_2d<f32>;

// Existing atmosphere/cloud radiance, shared with the sea. Its coarse 256x128
// bake is already filtered; no sun disc or unresolved microtexture enters it.
fn ground_sky_reflection(dir: vec3<f32>) -> vec3<f32> {
    let u = fract(atan2(dir.z, dir.x) / 6.28318530718 + 0.5);
    let v = acos(clamp(dir.y, -1.0, 1.0)) / 3.14159265359;
    return textureSampleLevel(ground_sky_env, terrain_shadow_samp, vec2<f32>(u, v), 0.0).rgb;
}

// Diffuse sky irradiance for a world-space surface normal, from the SH-9 sky projection
// (Ramamoorthi, "An Efficient Representation for Irradiance Environment Maps"), divided by PI so
// it is the Lambertian ambient reflectance factor (final ambient = albedo * sky_irradiance * scale).
// A future sky-visibility (AO) term multiplies this per point by the fraction of sky it can see.
fn sky_irradiance(n: vec3<f32>) -> vec3<f32> {
    let l00 = sky_sh.c[0].rgb;
    let l1m1 = sky_sh.c[1].rgb;
    let l10 = sky_sh.c[2].rgb;
    let l11 = sky_sh.c[3].rgb;
    let l2m2 = sky_sh.c[4].rgb;
    let l2m1 = sky_sh.c[5].rgb;
    let l20 = sky_sh.c[6].rgb;
    let l21 = sky_sh.c[7].rgb;
    let l22 = sky_sh.c[8].rgb;
    let x = n.x;
    let y = n.y;
    let z = n.z;
    let c1 = 0.429043;
    let c2 = 0.511664;
    let c3 = 0.743125;
    let c4 = 0.886227;
    let c5 = 0.247708;
    let e = c1 * l22 * (x * x - y * y) + c3 * l20 * (z * z) + c4 * l00 - c5 * l20
        + 2.0 * c1 * (l2m2 * x * y + l21 * x * z + l2m1 * y * z)
        + 2.0 * c2 * (l11 * x + l1m1 * y + l10 * z);
    return max(e, vec3<f32>(0.0)) * (1.0 / 3.14159265359);
}

// ---------------------------------------------------------------------------
// Normal-map relief under AMBIENT light.
//
// THE PROBLEM, measured. Arma 3 Stratis stone wall, freefly 3094.20 2138.07 170.45 169.9 -6.3,
// fraction of wall pixels that WGR_MATERIAL_DISABLE_NORMAL moves by more than 8/255:
//   hour 10 (direct sun)      42.6%   mean |d| 11.18   at wall mean level 65
//   hour  6 (ambient)          6.5%   mean |d|  2.85   at wall mean level 39
// Steering the SH-9 sky-irradiance lookup along the mapped normal (the `gtao.w` term, see
// shading::shade) lifted the dawn figure to 10.3% and no further, which is the expected ceiling:
// an order-2 spherical-harmonic field is low-frequency BY CONSTRUCTION, so rotating its argument
// can change the average tone of a bumpy surface but cannot resolve a bump. Relief needs a term
// whose output is high-frequency in the normal, and there are exactly two honest candidates that
// do not invent light: micro-OCCLUSION (a crevice sees less sky) and Fresnel-weighted sky
// SPECULAR (the reflectance itself varies per pixel). Both live here rather than in shading.wgsl
// because the terrain shader is not allowed to import shading (its pipeline layout has no lights
// binding) and the ground has the same dawn flatness.
// ---------------------------------------------------------------------------

// Micro-occlusion of the AMBIENT term, derived from how far the normal map turned this pixel.
//
// DERIVATION, not a tuned curve. Treat the mapped normal as a microfacet sitting on the macro
// surface. The macro surface is an occluding half-space at that facet, and the cosine-weighted
// fraction of the facet's own hemisphere lying above a plane whose normal makes an angle t with
// it is exactly (1 + cos t)/2 -- the standard half-space ambient-occlusion result. cos t is
// dot(geometric, mapped), which for a tangent-space map is just its z. So the physical term is
// `0.5 + 0.5 * c`, needing no new texture fetch, no derivative and no authored data.
//
// The exponent `k` (WGR_NORMAL_CAVITY) generalises it because the half-space value is a LOWER
// BOUND on the occlusion: it counts one occluder, and a real micro-surface has neighbours on
// every side, so k = 1 is the provable floor and the shipped default of 2 is the honest estimate
// of a surface that occludes itself from more than one direction. k = 0 returns exactly 1.0 and
// the whole term vanishes -- a flat normal map (c == 1) is also exactly 1.0 at every k, so an
// unmapped surface is bit-identical either way. That double guarantee is the point: this may
// never brighten anything and may never touch a surface that has no relief to reveal.
//
// It multiplies the AMBIENT only. On direct sun it would double-count N.L (which already answers
// "how much sun does this facet catch") and produce the classic over-darkened normal map. It is
// also safe against double-darkening an authored occlusion map, because the wgpu material path
// binds none: SectionMaterial carries colour / normal / SMDI / Multi-layer slots and no ambient-
// shadow (`_as`) slot at all -- Engine::MaterialDebugSettings names an AmbientShadow view but
// nothing implements it. The one occlusion this cannot see is AO baked into a `_co` albedo by the
// artist, which is a constant of the texture and would be double-counted by ANY runtime AO term
// (GTAO and sky-vis included); it is not new exposure introduced here.
//
// `geo_n` may be zero (decal sections authored without normals) and `n` is assumed unit.
fn normal_map_cavity(geo_n: vec3<f32>, n: vec3<f32>) -> f32 {
    let k = frame.relief.x;
    if (k <= 0.0) {
        return 1.0;
    }
    let g2 = dot(geo_n, geo_n);
    if (g2 < 1e-8) {
        return 1.0;
    }
    // SATURATED at the far hemisphere, not guarded out of it. The ambient STEER refuses to act
    // when the mapped normal falls behind the geometric one (it would rotate by ~180 degrees
    // about an arbitrary axis), and copying that "no information, change nothing" stance here
    // was the first draft — but this term is a scalar, so bailing to 1.0 puts a hard step from
    // 0.5^k straight back to full brightness at exactly 90 degrees, i.e. a bright rim around the
    // steepest part of every crevice. Clamping instead makes the term continuous and monotone
    // everywhere and lands on the physically right value: a facet at or past the horizon sees
    // exactly half the hemisphere, which is what 0.5^k says.
    //
    // Unreachable in practice on an authored surface either way — decode_nohq reconstructs z as
    // a non-negative sqrt and terrain floors it at 1e-3, so a real normal map cannot produce a
    // negative cosine. It is the world-space water-ripple normal and broken tangent frames this
    // has to stay well-behaved for.
    let c = max(dot(geo_n * inverseSqrt(g2), n), 0.0);
    return pow(0.5 + 0.5 * c, k);
}

// Dielectric normal-incidence reflectance. 0.04 is the standard value for the non-metals this
// corpus is made of (stone, plaster, painted metal, wood); it is NOT the 0.02 of env_fresnel,
// which is specifically air/water for the DayZ CalmWater surfaces.
const SKY_SPEC_F0: f32 = 0.04;

// Grazing-angle SKY SPECULAR, returned as a REALLOCATION of the ambient rather than an addition.
//   .rgb = the specular radiance, already weighted by its Fresnel term
//   .a   = the weight the DIFFUSE sky term must be scaled by, i.e. 1 - that Fresnel term
// so the caller writes `sky_irradiance(diffuse_n) * split.a + split.rgb` and the total ambient
// energy is unchanged to the accuracy of "the two lobes see a similar sky". That construction is
// deliberate and is the answer to the standing complaint that sunlit walls blow out: this term
// CANNOT add energy, only move it between two lookups, so a bright surface stays exactly as
// bright and only its per-pixel VARIATION changes. At gain 0 it returns (0,0,0,1) and the caller
// is bit-identical to the pre-existing line.
//
// The relief comes from two places at once, and both are per-pixel: the Schlick term is a fifth
// power of (1 - N.V), so a bump that tips a facet toward grazing gains reflectance sharply; and
// the lookup direction is the MIRROR direction, which swings by twice the normal's deviation, so
// it walks much further across the sky's gradient than the diffuse normal does. That is why this
// resolves detail the SH steer alone cannot, even though it reads the same SH-9 field.
//
// ROUGHNESS-AWARE, via Lazarov's `max(gloss, F0)` ceiling on the grazing term: a rough surface
// does not become a mirror at grazing incidence, and letting Schlick run to 1.0 paints a bright
// Fresnel rim on every silhouette in the scene. `gloss` is derived from the material's Blinn
// exponent by the caller; a material with no authored specular has gloss 0, so it keeps a flat
// 4% reallocation and this term is (correctly) invisible on it.
//
// `mask` is the per-texel specular level (SMDI green, 1 when the material binds no map) — a real
// per-pixel signal already fetched, so it costs nothing to respect and it keeps the term off the
// parts of a surface the artist marked as non-reflective.
//
// SH-9 is used rather than the `sky_env` equirect map on purpose: an order-2 field is a good
// approximation of a WIDE specular lobe (the rough surfaces this term is for) and it is bound to
// every 3D pipeline, whereas sky_env is a group(1) binding that only the two object pipelines
// have and that is a 1x1 dummy until Sky lends the real view. A sharp lobe for genuinely glossy
// materials would need that map and is not attempted here.
//
// `view_dir` is the surface -> eye direction (camera-relative space puts the camera at 0, so it
// is normalize(-world_pos)); `n` is the MAPPED shading normal and must be unit.
fn sky_specular_split(n: vec3<f32>, view_dir: vec3<f32>, gloss: f32, mask: f32) -> vec4<f32> {
    let gain = frame.relief.y;
    if (gain <= 0.0) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let ndotv = clamp(dot(n, view_dir), 0.0, 1.0);
    let f_grazing = max(clamp(gloss, 0.0, 1.0), SKY_SPEC_F0);
    let c = 1.0 - ndotv;
    let c2 = c * c;
    let f = clamp(
        (SKY_SPEC_F0 + (f_grazing - SKY_SPEC_F0) * (c2 * c2 * c)) * clamp(mask, 0.0, 1.0) * gain,
        0.0,
        1.0);
    let mirror = reflect(-view_dir, n);
    return vec4<f32>(sky_irradiance(mirror) * f, 1.0 - f);
}

// Blinn-Phong exponent -> gloss (1 - roughness), for sky_specular_split's grazing ceiling.
// Roughness from the exponent by the usual correspondence alpha = sqrt(2 / (p + 2)): p = 0 gives
// roughness 1 / gloss 0 (a material with no authored specular gets no sky specular beyond F0),
// p = 8 gives gloss 0.55, p = 64 gives 0.83.
fn spec_power_to_gloss(spec_power: f32) -> f32 {
    return clamp(1.0 - sqrt(2.0 / (max(spec_power, 0.0) + 2.0)), 0.0, 1.0);
}

// Aerial-perspective fog for a camera-relative fragment position. The froxel volume
// supplies only the fog COLOUR — the physically scattered airlight along this view ray,
// so it reddens toward the sun and meets the sky seamlessly at the horizon. The blend
// AMOUNT comes from the scene fog range (fogStart -> fogMax), NOT the froxel's physical
// transmittance: over the game's short (~km) view distance physical extinction is far
// too weak to dissolve distant geometry (objects would pop at the cull edge) and its
// inscatter is far too bright up close (uniform grey wash, since the airlight is on the
// sky's physical radiance scale while surfaces are legacy-lit ~1). Gating by the game
// fog factor fixes both by construction: near -> amount 0 -> untouched surface; at fogMax
// -> amount 1 -> surface fully replaced by the airlight, dissolving into the sky. The
// engine widens/narrows [fogStart, fogMax] per weather, so THAT is the atmosphere-density
// / "not every morning is foggy" control. Per-fragment: foliage, fences and transparents
// all fog by their own pixel distance; 2D never calls this.
//
// REN-ATM-001. `g_atmo_amount` records what this invocation's aerial-perspective blend actually
// was, so a caller can COUNT the term rather than assert it: -1 = apply_fog never ran or bailed
// (no atmosphere on this fragment), >= 0 = the airlight mix weight that was applied. It is a
// per-invocation private, so it costs a register and no memory traffic, and it is set by the fog
// code itself -- a census reading it cannot drift away from the path it is measuring the way a
// re-evaluated copy of the condition would.
var<private> g_atmo_amount: f32 = -1.0;

// REN-ATM-001: did THIS fragment receive the aerial-perspective term? See `g_atmo_amount`.
fn atmo_applied() -> bool {
    return g_atmo_amount >= 0.0;
}

fn apply_fog_receiver(rgb: vec3<f32>, world_pos_rel: vec3<f32>, receiver: bool) -> vec3<f32> {
    if (frame.params.fog_enabled <= 0.5 || frame.params.fog_inv_range <= 0.0) {
        return rgb;
    }
    let dist = length(world_pos_rel);
    // max_dist = fogStart + 1/fogInvRange = the scene fog-max range, which the engine also
    // uses as the camera far plane AND the terrain-grid cull distance — i.e. the real max
    // draw distance. Geometry cannot exist past it, so it's the anchor: at max_dist the fog
    // is full, so a terrain tile appearing at the far clip is already fully dissolved into
    // the sky (see cs_froxel: the far froxels ARE the sky) and fades in smoothly as it nears.
    let max_dist = frame.params.fog_start + 1.0 / frame.params.fog_inv_range;
    // Exponential (power) ramp, replacing the game's broad linear fogStart->fogMax fade:
    // pow(u, k) is ~0 across the near/mid field and rises hard only near the edge, so the
    // scene stays clear yet nothing pops at the cull distance. Density tracks the weather
    // fog range via max_dist (shorter range in fog -> u climbs sooner -> heavier fog), so
    // this stays weather-responsive with no gameplay impact (game fog logic is untouched).
    // Falloff exponent (frame.fog_color.w, from SkySettings::fogFalloff): high = clear near/
    // mid + fog only at the edge; low (~1) = dense fog throughout, revealing the froxel's
    // volumetric terrain sun-shadowing / god rays. Guarded so a zero-fill can't full-fog.
    let falloff = max(frame.fog_color.w, 0.1);
    // WHERE the fog finishes, not just how fast it gets there.
    //
    // `falloff` alone cannot solve "the far distance should be denser, to hide the terrain that
    // has been deleted", because it is a single exponent on a ramp pinned to reach 1.0 exactly AT
    // max_dist and nowhere before it. Lowering it to thicken the far field also thickens the near
    // and mid field -- it murks up the whole world to fix the last few hundred metres -- and even
    // then the horizon itself is only just barely closed.
    //
    // The horizon is where things actually disappear: the terrain grid, the object cull ring
    // (Scene::GetObjectDrawDistance) and the map's own edge all sit at or just inside max_dist,
    // and any of them showing through means watching the world end. Dividing by `close` moves the
    // point of FULL fog inward, so everything from there out is uniformly the sky's own airlight
    // and there is no edge left to see. Near and mid barely move, because pow() keeps them near
    // zero either way.
    //
    // This can only ever ADD cover at the far end, so it cannot reintroduce the object pop-in
    // that anchoring the cull to max_dist was there to prevent: an object arriving at the cull
    // edge is now fully fogged with margin rather than exactly at the boundary.
    let close = select(clamp(frame.fogfar.x, 0.3, 1.0), 1.0, frame.fogfar.x <= 0.0);
    let u = clamp(dist / (max_dist * close), 0.0, 1.0);
    let amount = pow(u, falloff);
    // Screen uv from a reprojection of the world position (matches cs_froxel's ndc->uv),
    // and distance -> slice with the fill's squared map (w = sqrt(dist / max)).
    let clip = frame.proj * frame.view * vec4<f32>(world_pos_rel, 1.0);
    let uv = (clip.xy / clip.w) * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5, 0.5);
    let inscat = textureSampleLevel(froxel_tex, froxel_samp, vec3<f32>(uv, sqrt(u)), 0.0).rgb;
    g_atmo_amount = amount; // REN-ATM-001 census witness; see g_atmo_amount.
    let background = mix(rgb, inscat, amount);
    if (!fog_camera_matches(layer_fog_packet, frame.view, frame.proj, frame.cam_pos.xyz)) { return background; }
    let absolute = world_pos_rel + frame.cam_pos.xyz;
    // A surface can block rain while outdoor mist still lies in front of it.
    let surface_open = fog_surface_open(weather_map_coverage(absolute), receiver);
    let ray_far=fog_ray_far(layer_fog_packet.control.y,frame.cam_pos.y,world_pos_rel.y/max(dist,0.00001),layer_fog_packet.camera.w,layer_fog_packet.control.w);
    let local = textureSampleLevel(layer_fog_tex, froxel_samp,
        vec3<f32>(uv, sqrt(clamp(dist / ray_far, 0.0, 1.0))), 0.0);
    let distant = textureSampleLevel(layer_fog_tex, froxel_samp, vec3<f32>(uv, 1.0), 0.0);
    return fog_legacy_closure(rgb, inscat, amount, local, distant, surface_open);
}
fn apply_fog(rgb: vec3<f32>, world_pos_rel: vec3<f32>) -> vec3<f32> {
    return apply_fog_receiver(rgb, world_pos_rel, true);
}
// Only the native terrain pass calls this: canopy rain shelter at its endpoint
// must not discard outdoor air already certified along the main-camera ray.
// Actual roof/underground samples remain absent from the encoded volume.
fn apply_fog_terrain(rgb: vec3<f32>, world_pos_rel: vec3<f32>) -> vec3<f32> {
    return apply_fog_receiver(rgb, world_pos_rel, true);
}


// Occlusion [0,1] of the sun by terrain at world position (xz, y): 0 = lit, 1 =
// fully in terrain shadow. Zero when the feature is off or the point is off the map
// or above the shadow ceiling. Shared by the terrain and lit-mesh fragment shaders.
// Fraction of direct sunlight reaching a world position through the clouds: 1 = full sun,
// 0 = fully shadowed. CLD-020.
//
// Returns 1 (fully lit) when disabled OR outside the map. That direction is deliberate and worth
// stating: absence of data must never invent shadow, because a dark band at the map's edge would
// track the camera and read as a rendering fault rather than as weather.
// CLD-021. Outside the map the lookup returns fully lit, which is the correct thing to say when
// there is no data: absence of data must never invent shadow, because a dark band at the map's
// edge would track the camera and read as a rendering fault rather than as weather.
//
// It was still a rendering fault, just the other way up. Stepping from "shadowed" to "fully lit"
// in one texel drew a dead-straight line across the landscape at the boundary, with everything
// past it in full sun, and the line moved with the camera. So the shadow now EASES to fully lit
// over the last CLOUD_SHADOW_EDGE_FADE of the map's half-extent. The direction above is preserved
// exactly -- the fade only ever mixes TOWARD 1.0, so it cannot darken a point the map says is lit
// and cannot produce the dark ring the hard return is there to prevent.
//
// Second line of defence, not the fix: Sky::select_cloud_shadow_span sizes the map to the
// camera's draw distance so the boundary normally sits past the far plane where nothing is drawn.
// This keeps the failure graceful when it cannot -- past the 16 km span cap, or on a camera whose
// fog range the engine never filled in.
const CLOUD_SHADOW_EDGE_FADE: f32 = 0.85;

fn cloud_sun_shadow(world_xz: vec2<f32>) -> f32 {
    // Cloud strength is baked into red by cs_cloud_shadow. Green is independent
    // smoke extinction and must still be sampled when cloud shadows are off.
    let uv = (world_xz - terrain_shadow_map.cloud_shadow.xy) * terrain_shadow_map.cloud_shadow.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 1.0;
    }
    // .r cloud x .g smoke: two independent occluders sharing one map.
    let t = textureSampleLevel(cloud_shadow_tex, terrain_shadow_samp, uv, 0.0);
    let shadow = clamp(t.r * t.g, 0.0, 1.0);
    // Chebyshev distance from the map's centre, normalised: 0 in the middle, 1 on the boundary.
    // Chebyshev and not Euclidean because the map is a SQUARE -- a radial fade would still leave a
    // hard step along the four sides beyond the inscribed circle.
    let edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0;
    let keep = 1.0 - smoothstep(CLOUD_SHADOW_EDGE_FADE, 1.0, edge);
    return mix(1.0, shadow, keep);
}

fn terrain_sun_shadow(world_xz: vec2<f32>, world_y: f32) -> f32 {
    if (terrain_shadow_map.enabled < 0.5) {
        return 0.0;
    }
    let uv = (world_xz - terrain_shadow_map.origin) * terrain_shadow_map.inv_span
             + terrain_shadow_map.half_texel;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 0.0;
    }
    let sm = textureSampleLevel(terrain_shadow_mask, terrain_shadow_samp, uv, 0.0);
    let lit = smoothstep(sm.r - sm.g, sm.r + sm.g + 1e-3, world_y);
    return clamp(sm.b * (1.0 - lit), 0.0, 1.0);
}

// Cosine-weighted fraction of sky a terrain column can see [0,1] (1 = open sky). Position-only
// (evaluated at the terrain surface); the AMBIENT-occlusion analogue of terrain_sun_shadow, which
// occludes the DIRECT sun. Returns 1 (full sky) off-map or before a heightmap loads so absence of
// data never darkens. Reuses the terrain-shadow mapping/sampler (no half-texel offset: the coarse,
// smooth mask relies on the clamping sampler at the edges).
fn terrain_sky_visibility(world_xz: vec2<f32>) -> f32 {
    if (terrain_shadow_map.enabled < 0.5) {
        return 1.0;
    }
    let uv = (world_xz - terrain_shadow_map.origin) * terrain_shadow_map.inv_span;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 1.0;
    }
    return textureSampleLevel(terrain_skyvis_mask, terrain_shadow_samp, uv, 0.0).r;
}

// Ambient-occlusion multiplier for the sky ambient term at a terrain column: blends from 1 (no AO)
// toward the sky-view factor by sky_vis_strength, then floors it so occluded ground never goes fully
// black. strength = 0 -> returns 1 (feature off). Multiply the sky ambient term by this.
fn sky_vis_ao(world_xz: vec2<f32>) -> f32 {
    let v = clamp(terrain_sky_visibility(world_xz), 0.0, 1.0);
    // Occlusion, contrast-shaped so the near-1 V of smooth heightfields still darkens visibly:
    // contrast = 1 -> occ = 1 - V (linear); contrast > 1 -> pow(V, contrast) < V -> deeper occ.
    let occ = 1.0 - pow(v, terrain_shadow_map.sky_vis_contrast);
    let ao = 1.0 - terrain_shadow_map.sky_vis_strength * occ;
    return max(ao, terrain_shadow_map.sky_vis_floor);
}

// Screen-space AO multiplier at this fragment [0,1], 1 = unoccluded. Returns 1 when the pass did
// not run: the AO texture RETAINS its last contents when GTAO is disabled or skipped, so an
// ungated read would shade the world with a frozen AO buffer — a failure that looks like a
// lighting bug rather than a missing pass. Multiply into the AMBIENT term only (plan §6): AO on
// direct sun is the classic over-darkening artifact, and direct occlusion is the shadow maps' job.
fn gtao_ao(frag_coord: vec2<f32>) -> f32 {
    if (frame.gtao.x < 0.5) {
        return 1.0;
    }
    let px = vec2<i32>(frag_coord);
    let dims = vec2<i32>(textureDimensions(gtao_tex));
    let q = clamp(px, vec2<i32>(0), dims - vec2<i32>(1));
    return clamp(textureLoad(gtao_tex, q, 0).a, 0.0, 1.0);
}

// The bent normal in WORLD space, or `fallback` (the geometric normal) when GTAO is off or the
// feature is disabled. This is the Stage-2 payload: sampling sky irradiance along the direction
// light actually arrives from, rather than along the surface normal, is what gives a shaded
// surface near an occluder some form instead of a flat wash.
//
// GTAO works in VIEW space, so rotate back. frame.view has its translation zeroed and is
// otherwise a rotation, so its inverse is its transpose — which `v * M` computes in WGSL
// (row-vector convention), avoiding an explicit inverse.
//
// frame.gtao.z gates it separately from gtao.x: the AO term is worth having on its own, and the
// directional ambient is the part most likely to need backing out if it looks wrong.
fn gtao_bent_normal_world(frag_coord: vec2<f32>, fallback: vec3<f32>) -> vec3<f32> {
    if (frame.gtao.x < 0.5 || frame.gtao.z < 0.5) {
        return fallback;
    }
    let px = vec2<i32>(frag_coord);
    let dims = vec2<i32>(textureDimensions(gtao_tex));
    let q = clamp(px, vec2<i32>(0), dims - vec2<i32>(1));
    let bent_view = textureLoad(gtao_tex, q, 0).xyz;
    if (dot(bent_view, bent_view) < 1e-6) {
        return fallback;
    }
    let bent_world = (vec4<f32>(normalize(bent_view), 0.0) * frame.view).xyz;
    return normalize(bent_world);
}

// Raw GTAO debug view mode: 0 = off, 1 = AO as greyscale, 2 = bent normal as RGB. Shipped WITH
// the effect, not after it: judging AO through a full lighting pipeline — sun, SH ambient, fog,
// tonemap — is much harder than looking at the buffer itself.
//
// Mode 2 exists because mode 1 shows only the scalar term, so the bent normal was invisible to
// inspection: toggling directional ambient changed nothing in the debug view and everything in
// the lit one, which is a confusing way to evaluate a feature.
fn gtao_debug_mode() -> f32 {
    return frame.gtao.y;
}

fn gtao_debug_on() -> f32 {
    return select(0.0, 1.0, frame.gtao.y > 0.5);
}

// What the debug view should draw at this pixel: greyscale AO, or the bent normal mapped from
// [-1,1] to [0,1] so directions read as colour.
fn gtao_debug_colour(frag_coord: vec2<f32>, fallback_n: vec3<f32>) -> vec3<f32> {
    if (frame.gtao.y > 1.5) {
        return gtao_bent_normal_world(frag_coord, fallback_n) * 0.5 + vec3<f32>(0.5);
    }
    return vec3<f32>(gtao_ao(frag_coord));
}

// Per-direction unoccluded fraction at a WORLD-ABSOLUTE point, for sky-map layer `i`.
// 1 = that direction's sky is visible from here, 0 = fully blocked. Returns 1 outside the map,
// because absence of data must never darken.
//
// Why a KERNEL and not a single tap: a hard "is anything above me" test gives black rooms and a
// razor edge at the doorway. Taking the fraction of taps over a small world-space disc makes the
// transition grade — under the middle of a roof every tap is blocked, at its lip some are not —
// and the kernel radius is roughly "how far light appears to reach in past an opening".
//
// The comparison sampler does the depth test: LessEqual returns 1 when the receiver's (biased)
// depth is at or in front of the stored occluder, i.e. nothing lies between it and the sky along
// this direction. The bias stops a surface that is its own highest geometry — open ground, a
// crate in the street — from shadowing itself.
fn interior_sky_reach_dir(world_abs: vec3<f32>, n: vec3<f32>, i: i32) -> f32 {
    // Orthographic: w is 1, so clip IS NDC.
    let clip = frame.skyvis_vp[i] * vec4<f32>(world_abs, 1.0);
    let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
    if (clip.z < 0.0 || clip.z > 1.0) {
        return 1.0;
    }
    // SLOPE-SCALED bias. A fixed bias is only correct when the map looks straight at the surface;
    // for a TILTED direction meeting a wall at a grazing angle, one texel spans far more depth
    // than it does head-on, and a head-on bias leaves that wall shadowing itself in hard stripes.
    // Scaling by 1/cos of the angle between the surface and the sampled direction tracks exactly
    // that stretch. Clamped so a surface nearly edge-on to the direction cannot demand an
    // unbounded bias and start leaking light through itself.
    let ndotd = max(dot(n, frame.skyvis_dir[i].xyz), 0.15);
    let ref_d = clip.z - frame.skyvisb.y / ndotd;
    let k = frame.skyvisb.x;
    // TWO rings, not one. A single ring of 4 taps gives a direction only 6 possible values, and
    // at a kernel wide enough to soften a doorway (16 texels here) those 6 levels read as hard
    // stepped patches across a ceiling rather than a gradient — measured in a real building, and
    // the reason this pattern is not just "centre plus a cross". The inner ring is rotated 45 deg
    // against the outer so the taps do not line up along the same axes.
    var sum = textureSampleCompareLevel(interior_sky_map, shadow_samp, uv, i, ref_d) * 2.0;
    var offs = array<vec2<f32>, 8>(
        // outer ring
        vec2<f32>(1.0, 0.0), vec2<f32>(-1.0, 0.0),
        vec2<f32>(0.0, 1.0), vec2<f32>(0.0, -1.0),
        // inner ring, rotated 45 deg, at ~half the radius
        vec2<f32>(0.38, 0.38), vec2<f32>(-0.38, 0.38),
        vec2<f32>(0.38, -0.38), vec2<f32>(-0.38, -0.38),
    );
    for (var t = 0; t < 8; t++) {
        let p = clamp(uv + offs[t] * k, vec2<f32>(0.0), vec2<f32>(1.0));
        sum += textureSampleCompareLevel(interior_sky_map, shadow_samp, p, i, ref_d);
    }
    let reach = sum / 10.0;
    // Fade out at the box border instead of ending in a hard line: the map moves with the camera,
    // so a discontinuity at its edge would sweep across the world as the player walks.
    let edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    return mix(1.0, reach, smoothstep(0.0, 0.05, edge));
}

// Cosine-weighted fraction of the whole sky DOME reaching this point [0,1]. The zenith dominates
// (it carries weight 1 against the tilted directions' cos 50 deg), which matches the diffuse
// response — but the tilted maps are what let light in through a window, because their rays
// arrive near-horizontally and a zenith map cannot see a vertical opening at any resolution.
fn interior_sky_reach_n(world_abs: vec3<f32>, n: vec3<f32>) -> f32 {
    if (frame.skyvis.x < 0.5) {
        return 1.0;
    }
    var num = 0.0;
    var den = 0.0;
    for (var i = 0; i < 5; i++) {
        let w = frame.skyvis_dir[i].w;
        num += interior_sky_reach_dir(world_abs, n, i) * w;
        den += w;
    }
    return num / max(den, 1e-4);
}

// Normal-free form for the DEBUG view, which draws the factor as a property of the point rather
// than of the surface. Straight up stands in for the normal so the slope-scaled bias has
// something sane to work with.
fn interior_sky_reach(world_abs: vec3<f32>) -> f32 {
    return interior_sky_reach_n(world_abs, vec3<f32>(0.0, 1.0, 0.0));
}

// Vertical rain admission is a different query from diffuse sky lighting. A
// canopy can block the zenith while all four tilted sky directions stay open;
// their cosine-weighted diffuse reach is then ~0.72, not proof of rain access.
// Reuse the existing zenith depth layer and its filtered geometry comparison.
// Artistic ambient strength/floor and tilted directions do not affect rain.
// The near map retains its legacy OPEN edge internally. The separate physical
// map below removes that feather before blending; absent active-map proof closes
// the query outside near coverage. This remains bounded rendered geometry.
// Coverage proof separate from the lighting map's OPEN fallback. Snow on a raised surface
// must not interpret a disabled/out-of-box map as proof that an interior floor is outdoors.
fn near_rain_coverage(world_abs: vec3<f32>) -> f32 {
    if (frame.skyvis.x < 0.5) { return 0.0; }
    let clip = frame.skyvis_vp[0] * vec4<f32>(world_abs, 1.0);
    if (clip.z < 0.0 || clip.z > 1.0) { return 0.0; }
    let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
    let edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    return smoothstep(0.0, 0.05, edge);
}

// The far cascade is only selected outside the planned near footprint.
// A near map which is pending/overflowed must not fall back to coarser proof.
fn weather_far_selected(world_abs: vec3<f32>) -> bool {
    if (frame.weather_cover.w < 0.5 || !all(abs(world_abs) < vec3<f32>(1e20))) { return false; }
    let clip = frame.weather_vp * vec4<f32>(world_abs, 1.0);
    let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
    let edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    return clip.z < 0.0 || clip.z > 1.0 || edge < frame.weather_cover.z;
}
fn weather_cascade_coverage(world_abs: vec3<f32>, outer: bool) -> f32 {
    var ctl = frame.weather_cover;
    var counts = frame.weather_gpu;
    var vp = frame.weather_vp;
    if (outer) { ctl = frame.weather_far_cover; counts = frame.weather_far_gpu; vp = frame.weather_far_vp; }
    if (ctl.x < 0.5 || !all(abs(world_abs) < vec3<f32>(1e20))) { return 0.0; }
    if (counts.w > 0u && (counts.x > counts.w || counts.y > counts.w || counts.z > 2u * counts.w)) {
        return 0.0; // actual retained output overflow cannot certify physical openness
    }
    let clip = vp * vec4<f32>(world_abs, 1.0);
    if (clip.z < 0.0 || clip.z > 1.0) { return 0.0; }
    let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
    let edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    return select(0.0, 1.0, edge >= ctl.z);
}
fn weather_map_coverage(world_abs: vec3<f32>) -> f32 {
    return weather_cascade_coverage(world_abs, weather_far_selected(world_abs));
}
fn weather_map_reach(world_abs: vec3<f32>) -> f32 {
    if (weather_map_coverage(world_abs) < 0.5) { return 0.0; }
    let outer = weather_far_selected(world_abs);
    var vp = frame.weather_vp;
    var bias = frame.weather_cover.y;
    if (outer) { vp = frame.weather_far_vp; bias = frame.weather_far_cover.y; }
    let clip = vp * vec4<f32>(world_abs, 1.0);
    let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
    if (outer) { return textureSampleCompareLevel(weather_far_depth_map, shadow_samp, uv, clip.z - bias); }
    return textureSampleCompareLevel(weather_depth_map, shadow_samp, uv, clip.z - bias);
}

fn interior_rain_coverage(world_abs: vec3<f32>) -> f32 {
    return max(near_rain_coverage(world_abs), weather_map_coverage(world_abs));
}

fn interior_rain_reach(world_abs: vec3<f32>) -> f32 {
    if (weather_map_coverage(world_abs) > 0.5) {
        let far_reach = weather_map_reach(world_abs);
        let near_coverage = near_rain_coverage(world_abs);
        if (near_coverage <= 0.0) { return far_reach; }
        // Remove artistic OPEN feather before blending the physical maps.
        let near_feather = interior_sky_reach_dir(world_abs, vec3<f32>(0.0, 1.0, 0.0), 0);
        let near_raw = clamp((near_feather - (1.0 - near_coverage)) / near_coverage, 0.0, 1.0);
        return mix(far_reach, near_raw, near_coverage);
    }
    if (frame.weather_cover.w > 0.5 && near_rain_coverage(world_abs) <= 0.0) {
        return 0.0; // active physical query with no submitted map proof
    }
    if (frame.skyvis.x < 0.5) {
        return 1.0;
    }
    return interior_sky_reach_dir(world_abs, vec3<f32>(0.0, 1.0, 0.0), 0);
}

// Snow asks whether this actual surface receives precipitation, not how diffuse
// light reaches a room. Never reuse the artistic metre-wide ambient kernel here:
// comparing its uphill taps with the centre depth makes an open inclined roof
// shadow itself. Predict the local receiver plane at each stored texel centre.
// The view is the actual orthographic producer matrix; no camera/FOV assumption.
fn snow_receiver_depth_gradient(vp: mat4x4<f32>, n: vec3<f32>) -> vec2<f32> {
    let rx = vec3<f32>(vp[0].x, vp[1].x, vp[2].x);
    let ry = vec3<f32>(vp[0].y, vp[1].y, vp[2].y);
    let rz = vec3<f32>(vp[0].z, vp[1].z, vp[2].z);
    let bx = rx / max(dot(rx, rx), 1e-20);
    let by = ry / max(dot(ry, ry), 1e-20);
    let bz = rz / max(dot(rz, rz), 1e-20);
    let den = dot(n, bz);
    if (abs(den) < 1e-8) { return vec2<f32>(0.0); }
    // UV = (NDC.x/2+1/2, -NDC.y/2+1/2).
    return vec2<f32>(-2.0 * dot(n, bx), 2.0 * dot(n, by)) / den;
}

fn snow_receiver_four_taps(uv: vec2<f32>, clip_depth: f32, dims: vec2<f32>,
                          depth: vec4<f32>, gradient: vec2<f32>, bias: f32) -> f32 {
    let px = uv * dims - vec2<f32>(0.5);
    let base = floor(px);
    let f = fract(px);
    let weights = vec4<f32>((1.0-f.x)*(1.0-f.y), f.x*(1.0-f.y),
                            (1.0-f.x)*f.y, f.x*f.y);
    // Four occupied coplanar samples also reconstruct the roof at the receiver
    // centre. This rejects a FLAT receiver just below a tilted roof even when a
    // downslope texel is lower than that receiver. Do not bridge clear cutout
    // holes/overhang edges with this extra plane proof. 1cm planarity tolerance
    // scales with the same physical 8cm bias, never an enlarged slope bias.
    let coplanar = abs(depth.x + depth.w - depth.y - depth.z) <= max(bias * 0.125, 1e-7);
    if (all(depth < vec4<f32>(1.0)) && coplanar) {
        // This actual depth plane is stronger self-surface evidence than a
        // smooth authored vertex normal (stock tent fabric intentionally has
        // smooth normals across bent facets). It also closes the flat-below-
        // tilted-roof case. The remaining branch handles discontinuous/clear
        // edges, where only the bounded local receiver-plane assumption remains.
        return select(0.0, 1.0, clip_depth - bias <= dot(depth, weights));
    }
    let origin = (base + vec2<f32>(0.5)) / dims;
    let step = vec2<f32>(1.0) / dims;
    let receiver = vec4<f32>(
        clip_depth + dot(gradient, origin - uv),
        clip_depth + dot(gradient, origin + vec2<f32>(step.x,0.0) - uv),
        clip_depth + dot(gradient, origin + vec2<f32>(0.0,step.y) - uv),
        clip_depth + dot(gradient, origin + step - uv)) - vec4<f32>(bias);
    return dot(select(vec4<f32>(0.0), vec4<f32>(1.0), receiver <= depth), weights);
}

fn snow_receiver_map_reach(world_abs: vec3<f32>, n: vec3<f32>, far: bool) -> f32 {
    let outer = far && weather_far_selected(world_abs);
    var vp = frame.skyvis_vp[0];
    if (far) { vp = frame.weather_vp; }
    if (outer) { vp = frame.weather_far_vp; }
    let clip = vp * vec4<f32>(world_abs, 1.0);
    let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
    var dims = vec2<f32>(textureDimensions(interior_sky_map));
    if (far) { dims = vec2<f32>(textureDimensions(weather_depth_map)); }
    if (outer) { dims = vec2<f32>(textureDimensions(weather_far_depth_map)); }
    let px = vec2<i32>(floor(uv * dims - vec2<f32>(0.5)));
    // Neither clamping nor an unrendered map border is physical open proof.
    if (any(px < vec2<i32>(0)) || any(px + vec2<i32>(1) >= vec2<i32>(dims))) { return 0.0; }
    var depth: vec4<f32>;
    if (outer) {
        depth = vec4<f32>(textureLoad(weather_far_depth_map, px, 0),
            textureLoad(weather_far_depth_map, px + vec2<i32>(1,0), 0),
            textureLoad(weather_far_depth_map, px + vec2<i32>(0,1), 0),
            textureLoad(weather_far_depth_map, px + vec2<i32>(1,1), 0));
    } else if (far) {
        depth = vec4<f32>(textureLoad(weather_depth_map, px, 0),
            textureLoad(weather_depth_map, px + vec2<i32>(1,0), 0),
            textureLoad(weather_depth_map, px + vec2<i32>(0,1), 0),
            textureLoad(weather_depth_map, px + vec2<i32>(1,1), 0));
    } else {
        depth = vec4<f32>(textureLoad(interior_sky_map, px, 0, 0),
            textureLoad(interior_sky_map, px + vec2<i32>(1,0), 0, 0),
            textureLoad(interior_sky_map, px + vec2<i32>(0,1), 0, 0),
            textureLoad(interior_sky_map, px + vec2<i32>(1,1), 0, 0));
    }
    // Near snow has its OWN tight physical bias, independent of artistic AO's
    // 25cm bias/kernel knobs. Far retains the actual producer's 8cm lane.
    let rz = vec3<f32>(vp[0].z, vp[1].z, vp[2].z);
    var bias = select(0.08 * length(rz), frame.weather_cover.y, far);
    if (outer) { bias = frame.weather_far_cover.y; }
    return snow_receiver_four_taps(uv, clip.z, dims, depth,
        snow_receiver_depth_gradient(vp, n), bias);
}

// Caller supplies its outward authored geometric normal, never powder/normal
// detail. Raw physical reach has NO cosmetic OPEN feather; coverage is separate.
fn snow_receiver_reach(world_abs: vec3<f32>, normal: vec3<f32>) -> f32 {
    if (!all(abs(normal) < vec3<f32>(1e20)) || !all(abs(world_abs) < vec3<f32>(1e20))) { return 0.0; }
    let len = length(normal);
    if (len < 1e-4 || normal.y / len <= 0.25) { return 0.0; }
    let n = normal / len;
    var near = near_rain_coverage(world_abs);
    // The lighting feather is not a valid physical sample when the four-tap
    // footprint straddles the image edge. Let a published far map own that
    // receiver completely, rather than blend an unknown near sample into it.
    let clip = frame.skyvis_vp[0] * vec4<f32>(world_abs, 1.0);
    let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
    let dims = vec2<f32>(textureDimensions(interior_sky_map));
    let px = vec2<i32>(floor(uv * dims - vec2<f32>(0.5)));
    if (any(px < vec2<i32>(0)) || any(px + vec2<i32>(1) >= vec2<i32>(dims))) { near = 0.0; }
    let far = weather_map_coverage(world_abs);
    var near_reach = 0.0;
    if (near > 0.0) { near_reach = snow_receiver_map_reach(world_abs, n, false); }
    if (far > 0.5) {
        let far_reach = snow_receiver_map_reach(world_abs, n, true);
        return mix(far_reach, near_reach, near);
    }
    return near_reach;
}

// Ambient multiplier from interior sky visibility [0,1]. AMBIENT ONLY — direct sun is already
// occluded by the cascade shadow maps plus the terrain sun-shadow mask, and local lights are the
// thing keeping an interior readable at all.
//
// The floor is not a nicety: with the sun shadowed and few local lights, the sky ambient is the
// ONLY light in an OFP room, so an unfloored version of this is a black box.
fn interior_sky_ao(world_abs: vec3<f32>, n: vec3<f32>) -> f32 {
    if (frame.skyvis.x < 0.5) {
        return 1.0;
    }
    let occ = 1.0 - interior_sky_reach_n(world_abs, n);
    return max(1.0 - frame.skyvis.z * occ, frame.skyvis.w);
}

// The direction the sky actually reaches this point FROM, world space — the sampled directions
// weighted by their own visibility and by how much this surface faces them. Falls back to the
// surface normal when nothing is visible or the feature is off.
//
// This is the difference between a room that is merely DARKER and a room that is LIT THROUGH ITS
// WINDOW. A visibility scalar can only scale brightness; it never says where the light came from,
// so with uniform dimming every wall of a room stays equally lit relative to the others and the
// opening reads as a bright patch rather than a light source. Steering the sky-irradiance lookup
// toward the open direction makes the wall facing the window brighter than the wall beside it,
// which is what the eye reads as light entering and bouncing.
fn interior_sky_ambient_normal(world_abs: vec3<f32>, n: vec3<f32>) -> vec3<f32> {
    if (frame.skyvis.x < 0.5 || frame.skyvisb.z <= 0.0) {
        return n;
    }
    var acc = vec3<f32>(0.0);
    for (var i = 0; i < 5; i++) {
        let d = frame.skyvis_dir[i].xyz;
        // Only the visible hemisphere contributes: a direction behind the surface cannot light it.
        let facing = max(dot(n, d), 0.0);
        if (facing > 0.0) {
            acc += d * (interior_sky_reach_dir(world_abs, n, i) * facing * frame.skyvis_dir[i].w);
        }
    }
    if (dot(acc, acc) < 1e-8) {
        return n;
    }
    return normalize(mix(n, normalize(acc), frame.skyvisb.z));
}

// 1 when the interior-sky debug view is on: surfaces draw the reach factor as greyscale instead
// of being lit by it. Shipped WITH the effect for the same reason the GTAO one was — judging this
// through sun + SH ambient + fog + tonemap is much harder than looking at the buffer.
fn interior_sky_debug_on() -> f32 {
    return select(0.0, 1.0, frame.skyvis.y > 0.5);
}

// 1 when the sky-visibility debug view is on (terrain shows the factor as greyscale). A helper
// so importers need not reference the terrain_shadow_map global directly.
fn sky_vis_debug_on() -> f32 {
    return terrain_shadow_map.sky_vis_debug;
}

// Contrast-shaped visibility for the debug view: pow(V, contrast). Unlike raw V (which sits near 1 on
// smooth terrain and reads as flat white), this pulls the factor down by the same contrast the AO
// uses, so the mask shape — and its response to radius/azimuths/downsample/contrast — is legible.
fn sky_vis_debug_value(world_xz: vec2<f32>) -> f32 {
    let v = clamp(terrain_sky_visibility(world_xz), 0.0, 1.0);
    return pow(v, terrain_shadow_map.sky_vis_contrast);
}

// Reversed-Z: the shared projection is forward (near->0, far->1). Remap to
// near->1, far->0 so the float depth buffer spends its exponent bits where
// geometry actually is (far from 0), which massively improves precision at range
// vs forward float depth. Pipelines use GreaterEqual + clear-to-0.
fn reverse_z(clip: vec4<f32>) -> vec4<f32> {
    var c = clip;
    c.z = c.w - c.z;
    // Perspective depth must be reversed BEFORE subtracting kilometre-sized
    // clip coordinates. The late subtraction loses the small near-plane term.
    if (frame.proj[3].w == 0.0 && frame.proj[2].w != 0.0
        && frame.proj[0].z == 0.0 && frame.proj[1].z == 0.0) {
        c.z = fma(1.0 - frame.proj[2].z / frame.proj[2].w, clip.w, -frame.proj[3].z);
    }
    return c;
}

// Scene fog blend factor in [0,1] for a camera distance: 1 = keep colour, 0 =
// full fog. Returns 1 unconditionally when fog is disabled.
fn fog_factor(dist: f32) -> f32 {
    let f = clamp(1.0 - (dist - frame.params.fog_start) * frame.params.fog_inv_range, 0.0, 1.0);
    return select(1.0, f, frame.params.fog_enabled > 0.5);
}

// The probe grid's dimensions, mirrored from gfx3d::gi (NX/NY/NZ). Separate float and integer
// constants rather than one vector: see the note on `iny` below.
const GI_DIM_X: f32 = 32.0;
const GI_DIM_Y: f32 = 8.0;
const GI_DIM_Z: f32 = 32.0;
const GI_NY_I: i32 = 8;

// REN-GI-001/003: irradiance/pi at a world point for normal n from the probe volume, and the
// weight to blend it with (0 outside the volume, or with the feature off).
//
// Stage 3 gathers the eight surrounding probes by hand instead of letting the hardware
// interpolate, because two weights have to be applied PER PROBE and trilinear filtering cannot
// carry them:
//
//   * a facing weight, so a probe behind the shaded surface contributes nothing;
//   * a Chebyshev VISIBILITY weight from that probe's distance moments in the direction of the
//     shaded point -- if the probe's rays that way stopped well short of the point, something
//     stands between them, and its light must not arrive. That is what stops a sunlit outdoor
//     probe bleeding through a wall into a room.
//
// Everything is INLINE on purpose. A `textureLoad` inside a helper function called from a loop
// makes naga's SPIR-V backend panic ("Expression [n] is not cached") at pipeline creation, i.e.
// when the renderer starts, and the game falls back to no renderer at all. A toy fragment
// shader over this module does NOT reproduce it; only the real object shader does, which is
// what the `gi_gather_survives_the_object_shader_backend` test builds.
fn gi_irradiance(world_abs: vec3<f32>, n: vec3<f32>) -> vec4<f32> {
    if (frame.gi.x < 0.5) {
        return vec4<f32>(0.0);
    }
    let spacing = max(frame.giorigin.w, 0.01);
    let dims = vec3<f32>(GI_DIM_X, GI_DIM_Y, GI_DIM_Z);
    // A LITERAL, deliberately. `i32(dims.y)` -- an integer cast of a component of a
    // const-foldable vector -- makes wgpu's pipeline-constant pass declare the whole function
    // invalid at pipeline creation ("Function ... is invalid"), which takes the renderer down
    // at startup with no other diagnosis. naga's own validator accepts it, so only a real
    // pipeline test catches this (gi_gather_survives_the_object_shader_backend).
    let iny = GI_NY_I;
    // probe (i) sits at origin + (i + 0.5) * spacing, so probe-index space is:
    let local = (world_abs - frame.giorigin.xyz) / spacing - vec3<f32>(0.5);
    let hi = dims - vec3<f32>(1.0);
    let edge = min(min(min(local.x, hi.x - local.x), min(local.z, hi.z - local.z)),
                   min(local.y, hi.y - local.y));
    let w = clamp(edge * 0.5, 0.0, 1.0); // fades over two probes at the volume edge
    if (w <= 0.0) {
        return vec4<f32>(0.0);
    }
    let cl = clamp(local, vec3<f32>(0.0), hi);
    let base = floor(cl);
    let frac = cl - base;
    // Which ambient-cube face each axis of the normal reads.
    let n2 = n * n;
    let fx = i32(f32(n.x < 0.0));
    let fy = 2 + i32(f32(n.y < 0.0));
    let fz = 4 + i32(f32(n.z < 0.0));
    var sum = vec3<f32>(0.0);
    var wsum = 0.0;
    for (var c = 0; c < 8; c++) {
        let ox = f32(c & 1);
        let oy = f32((c >> 1) & 1);
        let oz = f32((c >> 2) & 1);
        let pf = min(base + vec3<f32>(ox, oy, oz), hi);
        let pi = vec3<i32>(i32(pf.x), i32(pf.y), i32(pf.z));
        // Trilinear weight of this corner.
        let tx = mix(1.0 - frac.x, frac.x, ox);
        let ty = mix(1.0 - frac.y, frac.y, oy);
        let tz = mix(1.0 - frac.z, frac.z, oz);
        var wt = tx * ty * tz;
        // The probe's world position, and the direction from it to the shaded point.
        let pw = frame.giorigin.xyz + (pf + vec3<f32>(0.5)) * spacing;
        let to_point = world_abs - pw;
        let dist = max(length(to_point), 1e-4);
        let dir = to_point / dist;
        // A probe behind the surface has nothing to say about it.
        wt = wt * clamp(dot(n, -dir) * 0.5 + 0.5, 0.0, 1.0);
        // Chebyshev visibility from this probe's distance moments along `dir`: if its rays that
        // way stopped well short of the point, something stands between them, and its light
        // must not arrive. This is what stops a sunlit outdoor probe bleeding through a wall.
        let ad = abs(dir);
        let y_dom = f32(ad.y > ad.x && ad.y >= ad.z);
        let z_dom = f32(ad.z > ad.x && ad.z > ad.y);
        let x_dom = 1.0 - y_dom - z_dom;
        let comp = dir.x * x_dom + dir.y * y_dom + dir.z * z_dom;
        let vf = i32((y_dom + 2.0 * z_dom) * 2.0 + f32(comp < 0.0));
        // Every load here is UNCONDITIONAL. A textureLoad inside an `if` inside this loop makes
        // wgpu's pipeline-constant pass reject the whole function ("Function is invalid", at
        // renderer startup); weighting by zero costs a fetch and keeps the shader legal.
        let m = textureLoad(gi_probe_dist, vec3<i32>(pi.x, pi.y + vf * iny, pi.z), 0).xy;
        let variance = max(m.y - m.x * m.x, 1e-4);
        let dd = max(dist - m.x, 0.0);
        let cheb = variance / (variance + dd * dd);
        wt = wt * clamp(cheb * cheb * cheb, 0.0, 1.0);
        if (frame.gi.w > 1.5) {
            // REN-GI-009: second-order SH. E(n)/pi = sum over l of (A_l / pi) * L_lm * Y_lm(n),
            // with A0/pi = 1, A1/pi = 2/3 and A2/pi = 1/4. Nine loads a corner against the
            // cube's three -- that is what this basis costs, and REN-GI-008 measured that the
            // four-coefficient version is not a cheaper way to get it.
            let c00 = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y, pi.z), 0).rgb;
            let c1y = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + iny, pi.z), 0).rgb;
            let c1z = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 2 * iny, pi.z), 0).rgb;
            let c1x = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 3 * iny, pi.z), 0).rgb;
            let cxy = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 4 * iny, pi.z), 0).rgb;
            let cyz = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 5 * iny, pi.z), 0).rgb;
            let czz = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 6 * iny, pi.z), 0).rgb;
            let cxz = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 7 * iny, pi.z), 0).rgb;
            let cxx = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 8 * iny, pi.z), 0).rgb;
            let band0 = c00 * 0.2820948;
            let band1 = (c1y * n.y + c1z * n.z + c1x * n.x) * (0.6666667 * 0.4886025);
            let band2 = (cxy * (1.0925484 * n.x * n.y)
                       + cyz * (1.0925484 * n.y * n.z)
                       + czz * (0.3153916 * (3.0 * n.z * n.z - 1.0))
                       + cxz * (1.0925484 * n.x * n.z)
                       + cxx * (0.5462742 * (n.x * n.x - n.y * n.y))) * 0.25;
            sum += max(band0 + band1 + band2, vec3<f32>(0.0)) * wt;
        } else if (frame.gi.w > 0.5) {
            // REN-GI-008: first-order SH. Slabs 0..3 hold L0 and the three L1 (y, z, x).
            // Irradiance/pi for normal n is A0/pi * Y0 * L0 + A1/pi * Y1 * dot(L1, n), with
            // A0 = pi and A1 = 2pi/3 -- the two constants below. Unlike the ambient cube this
            // cannot return zero for a normal merely because six faces could not hold the
            // arrival direction, which is what REN-GI-007 measured.
            let l0 = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y, pi.z), 0).rgb;
            let ly = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + iny, pi.z), 0).rgb;
            let lz = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 2 * iny, pi.z), 0).rgb;
            let lx = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + 3 * iny, pi.z), 0).rgb;
            let e = l0 * 0.2820948 + (ly * n.y + lz * n.z + lx * n.x) * 0.3257350;
            sum += max(e, vec3<f32>(0.0)) * wt;
        } else {
            let ex = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + fx * iny, pi.z), 0).rgb;
            let ey = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + fy * iny, pi.z), 0).rgb;
            let ez = textureLoad(gi_probes, vec3<i32>(pi.x, pi.y + fz * iny, pi.z), 0).rgb;
            sum += (ex * n2.x + ey * n2.y + ez * n2.z) * wt;
        }
        wsum += wt;
    }
    if (wsum <= 1e-5) {
        // Every surrounding probe is occluded from this point: no GI, keep the sky ambient.
        return vec4<f32>(0.0);
    }
    return vec4<f32>(sum / wsum, w);
}

// Incoming main-light fog attenuation, never camera-path alpha. Rain shelter
// cannot erase intervening air. Unknown domains/mismatched cameras stay neutral;
// actual surface CSM owns solid occlusion.
fn fog_sun_reach(world_pos_rel: vec3<f32>) -> f32 {
    if (!fog_camera_matches(layer_fog_packet,frame.view,frame.proj,frame.cam_pos.xyz)) { return 1.0; }
    let absolute=world_pos_rel+frame.cam_pos.xyz;
    if(layer_fog_packet.native.w<0.5 || layer_fog_packet.native.z<=0.0
        || min(layer_fog_packet.native_size.x,layer_fog_packet.native_size.y)<2u) {return 1.0;}
    if(any(textureDimensions(layer_fog_native_tex)!=layer_fog_packet.native_size.xy)){return 1.0;}
    let cell=(absolute.xz-layer_fog_packet.native.xy)/layer_fog_packet.native.z;
    let extent=vec2<f32>(layer_fog_packet.native_size.xy)-1.0;
    if(any(cell<vec2<f32>(0.0)) || any(cell>=extent)){return 1.0;}
    let ij=vec2<i32>(floor(cell));let q=fract(cell);
    let a=textureLoad(layer_fog_native_tex,ij,0).r;let b=textureLoad(layer_fog_native_tex,ij+vec2<i32>(1,0),0).r;
    let c=textureLoad(layer_fog_native_tex,ij+vec2<i32>(0,1),0).r;let d=textureLoad(layer_fog_native_tex,ij+vec2<i32>(1,1),0).r;
    let ground=select(b+c-d+(d-c)*q.x+(d-b)*q.y,a+(b-a)*q.x+(c-a)*q.y,q.x+q.y<=1.0);
    let agl=absolute.y-ground;
    if(agl < -0.05 || agl>=layer_fog_packet.solar_domain.w){return 1.0;}
    let uv=(absolute.xz-layer_fog_packet.solar_domain.xy)*layer_fog_packet.solar_domain.z;
    if(any(uv<vec2<f32>(0.0)) || any(uv>vec2<f32>(1.0))){return 1.0;}
    let dims=textureDimensions(layer_fog_solar_tex);
    let w=sqrt(max(agl,0.0)/layer_fog_packet.solar_domain.w);
    let z=(w*f32(dims.z-1u)+0.5)/f32(dims.z);
    let transmission=textureSampleLevel(layer_fog_solar_tex,froxel_samp,vec3<f32>(uv,z),0.0).r;
    if ((bitcast<u32>(transmission)&0x7f800000u)==0x7f800000u) { return 1.0; }
    return clamp(transmission,0.0,1.0);
}
