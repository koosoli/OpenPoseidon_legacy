#import frame::{frame, reverse_z, apply_fog, sky_irradiance, terrain_sun_shadow, sky_vis_ao, cloud_sun_shadow}
#import gbuffer::{oct_encode, a2c_coverage}
#import shadow::shadow_strength

// Diagnostic A/B only; coverage, placement and shadow geometry are unchanged.
override GRASS_FILTER_SUBPIXEL: bool = true;
override GRASS_MEDIUM_LIGHTING: bool = true;

struct TerrainParams {
    world_origin: vec2<f32>,
    land_grid: f32,
    terrain_grid: f32,
    hm_width: u32,
    hm_height: u32,
    land_range: u32,
    data_scale: f32,
    sea_level: f32,
    time: f32,
    swash_speed: f32,
    swash_amp: f32,
    wet_height: f32,
    wet_darken: f32,
    pad_a: f32,
    pad_b: f32,
};

struct GrassTrack {
    x: f32,
    z: f32,
    radius: f32,
    age: f32,
};
struct GrassDownwash { x: f32, z: f32, radius: f32, strength: f32, };

struct GrassParams {
    density: f32,
    spacing: f32,
    near_radius: f32,
    enabled: f32,
    blade_height: f32,
    wind_strength: f32,
    wind_direction: f32,
    far_radius: f32,
    interactor_x: f32,
    interactor_z: f32,
    interactor_radius: f32,
    interactor_strength: f32,
    tracks: array<GrassTrack, 256>,
    downwash: array<GrassDownwash, 4>,
    debug_flags: vec4<f32>,
    // MUST match the tail of GrassParams in grass/mod.rs, four scalars per vec4:
    //   .x = cast_shadows, .y = apply_fog,
    //   .z = density noise scale, .w = density noise strength
    render_flags: vec4<f32>,
    //   .x = weed fraction, .y = flower fraction (grass takes the remainder),
    //   .z = blade width scale, .w = use_photo_tuft
    species_mix: vec4<f32>,
    //   .x = albedo saturation (1.0 = untouched)
    //   .y = dry-patch amount, .z = dry-patch noise scale, .w = mid-clump radius
    look: vec4<f32>,
    //   .x = shape variety (0 = legacy three profiles, 1 = eight distinct)
    //   .y = per-blade taper jitter, .z = per-blade bend jitter
    //   .w = blade texture strength (0 = ignore the photo array entirely)
    shape_mix: vec4<f32>,
    //   .x = alpha cut-out cards on/off, .y = alpha cutoff
    //   .z = card widening applied when cards are on, .w spare
    cards: vec4<f32>,
    // .x = GRS-030 multi-blade clump renderer; 0 retains legacy ribbons.
    // .y = photographed A3 clump brightness; .z/.w = mixed-photo coverage / patch size.
    renderer: vec4<f32>,
    // PHOTOGRAPHED CLUMP CARDS ONLY. Nothing here touches procedural ribbons,
    // the procedural mid ring or the far coverage proxy.
    //   .x = contrast about the plate's own mid-luma (1 = the raw photograph)
    //   .y = contour normal strength rebuilt from the plate's luma gradient
    //   .z = grass-on-grass self-shadow strength, .w = root ambient occlusion
    photo: vec4<f32>,
    //   .x = near placement grid edge in cells, .y = mid grid edge (both sized
    //        from their radius by the renderer, so density is radius-independent)
    //   .z = LOD dissolve band in metres, .w = photo-card alpha cutoff
    place: vec4<f32>,
    //   .x = forced photo atlas layer (-1 = normal selection, 0..8 = that layer)
    //   .y = photographed-card saturation about the plate's own luma
    //   .z = walked-imprint lifetime in seconds, .w = imprint depth
    photo_mix: vec4<f32>,
    // Relative selection weights for atlas layers 1..8, four per vec4.
    layer_weights: array<vec4<f32>, 2>,
    //   .x/.y/.z = near / mid / far coverage multipliers on top of `density`
    //   .w = photo-card placement spacing in metres (see card_placement_spacing)
    lod_density: vec4<f32>,
    // Albedo tints applied before lighting. `tint_procedural` covers procedural
    // blades, ribbons and the far proxy; `tint_photo` covers photographed cards.
    tint_procedural: vec4<f32>,
    tint_photo: vec4<f32>,
    // PHOTO CARD COVERAGE (see card_coverage_for).
    //   .x = fraction of clutter-grid cells that grow a card, 0..1
    //   .y = auto: 1 = authored cells keep the map's own density and .x is only
    //        the fallback where the bake did not answer; 0 = .x applies everywhere
    //   .z = card size multiplier (1 = the stock card; see photo_card_scale)
    //   .w = shadow blade stride (renderer-owned; read by vs_grass_shadow only)
    card_look: vec4<f32>,
    // PROCEDURAL BLADES ONLY (near blades + mid ribbons; cards and the far proxy
    // ignore this). Defaults reproduce the pre-existing look exactly.
    //   .x = blade-on-blade self shadow (0 = off), .y = contrast (1 = untouched)
    //   .z = per-patch hue variation (0 = none), .w = root darkening (0.70 = stock)
    blade_look: vec4<f32>,
    // PHOTO CARDS ONLY; renderer-owned (CardTone in grass/mod.rs).
    //   .x = per-layer tone target luma (0 = off), .y = tone gain cap
    //        (see card_tone_gain), .z = wind flutter the cards take, 0..1
    //        (see vs_grass_mid_tuft), .w spare
    card_tone: vec4<f32>,
    // RFG-090: .x = near ring keeps blades while cards are on (native Reforger worlds:
    // the world's own blade atlas on the near ring, its clutter plants as mid cards
    // from the camera outward). .yzw spare. Mirrors `near_blades_only` in mod.rs.
    native: vec4<f32>,
    // RFG-091: .x = how far the native blade takes the ground colour in native.yzw
    // (the PlantMat's SatMapLerp); .y gust variation (-1 = previous gusts),
    // .z front size in metres, .w spare.
    native2: vec4<f32>,
    // Opaque bounding box per photo layer, (u0, u1, v0, v1) in texture space.
    // The card quad is trimmed to it: the margin outside can never survive the
    // alpha test, so rasterising it was pure overdraw on a fill-bound path.
    layer_bounds: array<vec4<f32>, 32>,
    // Mean linear colour of each layer's covered texels. The contrast pivot: a
    // fixed pivot made contrast a brightness change, and a DISTANCE-dependent
    // one, because mips average each texel toward exactly this value.
    layer_means: array<vec4<f32>, 32>,
    // Renderer-integrated phases, shared with shadow geometry. A weather
    // change alters velocity without multiplying it by elapsed mission time.
    wind_advection: vec4<f32>,
    wind_flutter: vec4<f32>,
    // Separate blade_tex means, linear and source-opacity weighted; w=0 invalid.
    blade_layer_means: array<vec4<f32>, 8>,
};

// 32 bytes. `pos_seed` stays f32 for world precision; everything the compute
// placement pass can resolve once per blade rides in `packed`, because the
// vertex shaders re-derived it for all 60 (near) / 24 (mid) vertices of an
// instance from inputs that only ever depended on the instance position.
//   packed.x = pack2x16snorm(flatten direction)
//   packed.y = pack2x16unorm(flatten strength, rotor-wash turbulence)
//   packed.z, packed.w = reserved (cached wind, archetype/palette)
struct GrassInstance {
    pos_seed: vec4<f32>,
    packed: vec4<u32>,
};

// A low-frequency travelling direction field plus a tighter gust field. This
// is the reference project's two-noise wind idea, implemented from the
// renderer's deterministic world-space value noise so it needs no texture
// upload and remains stable while the camera moves.
struct WindField {
    direction: vec2<f32>,
    gust: f32,
    turbulence: f32,
};

@group(1) @binding(0) var<uniform> terrain: TerrainParams;
@group(1) @binding(1) var heightmap: texture_2d<f32>;
@group(1) @binding(2) var geography: texture_2d<u32>;

// Sinkhole W1 (port of Malprave terrainHole2 to wgpu): terrain holes -- world X/Z convex areas a hole-cutting
// object (memory-LOD `terrain_hole*` selection + roadway LOD) has cut out of the ground. Written by
// Engine::SetTerrainHoles via wgr_terrain_set_holes. edges[i] = {nx, nz, d, last}: a point is inside an area when
// nx*x + nz*z + d >= 0 for every edge of that area; last = 1 closes an area. info.x = record count (0 = no holes). Optional {0,0,ceilingY,2} precedes a bounded polygon.
struct TerrainHoles {
    info: vec4<f32>,
    edges: array<vec4<f32>, 64>,
};
@group(1) @binding(3) var<storage, read> holes: TerrainHoles;

fn in_terrain_hole(world: vec3<f32>) -> bool {
    let xz = world.xz;
    let n = min(u32(max(holes.info.x, 0.0)), 64u);
    if (n == 0u) { return false; }
    var inside = true;
    var bounded = false;
    var ceiling = 0.0;
    for (var i = 0u; i < n; i = i + 1u) {
        let e = holes.edges[i];
        // Optional record BEFORE a polygon: {0, 0, world ceiling Y, 2}.
        // It is metadata, never a zero-length edge or a polygon terminator.
        if (e.w == 2.0) {
            bounded = true;
            ceiling = e.z;
            continue;
        }
        // A collapsed edge must remain outside, never an unbounded whole-world cut.
        if (dot(e.xy, e.xy) < 0.25 || e.x * xz.x + e.y * xz.y + e.z < 0.0) { inside = false; }
        if (e.w > 0.5) {
            if (inside && (!bounded || world.y <= ceiling)) { return true; }
            inside = true;
            bounded = false;
            ceiling = 0.0;
        }
    }
    return false;
}
@group(2) @binding(0) var<uniform> grass: GrassParams;
@group(2) @binding(1) var<storage, read_write> instances: array<GrassInstance>;
@group(2) @binding(2) var<storage, read_write> placement_count: array<atomic<u32>>;
// GRS-D blade albedo: one layer per archetype, mipped. Fragment stage only.
@group(2) @binding(3) var blade_tex: texture_2d_array<f32>;
@group(2) @binding(4) var blade_samp: sampler;
// GRS-E: the game's own photographed grass tuft, used by the mid LOD's crossed
// cards. Cutout alpha -- fs_grass_mid alpha-tests it.
@group(2) @binding(5) var tuft_tex: texture_2d_array<f32>;
// FRAGMENT CENSUS (WGR_GRASS_COUNT_FRAGMENTS=1; grass/mod.rs builds the draw pipelines
// with the *_count entries below instead of the production ones). A millisecond
// without its count cannot say whether a pass is paying per blade or per pixel, and
// the instance/vertex counts the placement counters already give are only half of
// that ratio. FRAG_LANES lanes of FRAG_STRIPES counters each: lane 0 near prepass,
// 1 mid prepass, 2 near colour, 3 mid colour, 4 near shadow (grass_shadow.wgsl),
// summed on readback. Striped by pixel x so a warp's atomics spread over 32
// addresses rather than serialising on one. Measurement only: the atomics cost
// time and may cost early-Z, so timings from a counting run are not quotable.
@group(2) @binding(6) var<storage, read_write> frag_counts: array<atomic<u32>>;
const FRAG_STRIPES: u32 = 32u;
fn count_fragment(lane: u32, pos: vec4<f32>) {
    atomicAdd(&frag_counts[lane * FRAG_STRIPES + (u32(pos.x) & (FRAG_STRIPES - 1u))], 1u);
}

// The near grid edge is dynamic (grass.place.x); only its output cap is fixed.
const MAX_INSTANCES: u32 = 1048576u;
const FAR_GRID_DIM: u32 = 384u;
const MAX_FAR_INSTANCES: u32 = 147456u;
// The mid grid edge is dynamic (grass.place.y); this is its output cap, equal to
// MID_GRID_MAX^2 in grass/mod.rs. It was 147456 -- a quarter of the buffer --
// which capped mid coverage at high density for no reason.
const MID_PLACEMENT_SPACING: f32 = 0.82;
const MAX_MID_INSTANCES: u32 = 1048576u;
const LAYERS: u32 = 8u;

// Which rings draw PHOTO CARDS this frame. species_mix.w is only set once the
// atlas is actually resident (grass/mod.rs gates it on have_tuft), so a world
// with no plates keeps the blade spacing below rather than a sparse ribbon field.
// The near ring swaps to cards only under the clump renderer (renderer.x), which
// is the same condition draw() uses to pick near_tuft_* over the blade pipeline.
fn cards_on_mid() -> bool { return grass.species_mix.w > 0.5; }
// native.x carries the world's own blade HEIGHT in metres when it is on (0 = off), so one
// lane says both "blades on the near ring" and how tall Reforger authored them.
fn cards_on_near() -> bool { return cards_on_mid() && grass.renderer.x > 0.5 && grass.native.x <= 0.0; }
// RFG-090: the stock near blade is ~0.45 m; a PlantMat's `Height 0.1` is the cover Reforger
// draws, and a knee-high field of it would be neither world's grass.
fn native_blade_height_scale() -> f32 {
    return select(1.0, clamp(grass.native.x / 0.45, 0.15, 1.0), grass.native.x > 0.0);
}
// RFG-090: cards on the mid ring only, so they must reach the camera themselves.
fn mid_cards_from_camera() -> bool { return cards_on_mid() && !cards_on_near(); }

// Placement spacing for photo cards, in metres, shared by BOTH rings when they
// draw cards. This is the density control for the card path and it is
// deliberately not the blade spacing: a card is ~1.5 m across against a blade's
// centimetres, so placing cards on the 0.16 m blade grid stacked ~90 plates
// over every point of ground -- a solid mat, ~20 layers of alpha-tested
// overdraw, and every one of those edges shimmering in the wind. That is the
// "horrible, flickering, far too dense" report on the map-clutter path.
//
// One card = one clutter object, so the natural unit is the map's own clutter
// grid: Arma's world configs place clutter on `clutterGrid` (1.11 m on the A2/A3
// worlds this path exists for) and thin it by the surface character's
// probability -- which the geography bake already applies (authored_thinned).
// The default in grass/mod.rs is that 1.11 m; WGR_GRASS_CARD_SPACING overrides.
//
// Using ONE spacing for near and mid means the two rings are the same field: the
// near ring exists only to draw its own pipeline (shadow, prepass split), so its
// join with the mid ring must be a change of nothing, not of density.
fn card_placement_spacing() -> f32 {
    return clamp(grass.lod_density.w, 0.06, 4.0);
}

// Must mirror `near_placement_spacing` in grass/mod.rs: the shader decides where
// candidates land, the renderer decides how many cells exist.
fn near_placement_spacing() -> f32 {
    let blade = grass.spacing * select(1.72, 3.20, grass.renderer.x > 0.5);
    return select(blade, card_placement_spacing(), cards_on_near()) / sqrt(max(grass.density, 1.0));
}

// Must mirror `mid_placement_spacing` in grass/mod.rs.
fn mid_placement_spacing() -> f32 {
    // Above-one coverage is extra plants per square metre; thinning alone
    // cannot represent it. CPU dispatch sizing uses this same refined spacing.
    let requested = mid_requested_spacing();
    // Two cells of margin keep the entire requested ring inside the bounded
    // dispatch even when its centres are jittered. Mirrors bounded_mid_spacing.
    return max(requested, mid_requested_radius() / 1218.0);
}

fn mid_requested_spacing() -> f32 {
    return select(MID_PLACEMENT_SPACING, card_placement_spacing(), cards_on_mid()) /
        sqrt(max(grass.density, 1.0) * max(grass.lod_density.y, 1.0));
}

fn mid_coverage_width_scale(packed: u32) -> f32 {
    // Bit 11 marks only mid instances: photographed near cards use the same
    // vertex entry point, and must retain their original size.
    return select(1.0, mid_placement_spacing() / mid_requested_spacing(),
        (packed & (1u << 11u)) != 0u);
}

// Geography bits, mirroring GeographyInfo in engine/Poseidon/AI/Path/AITypes.hpp:
//   0-1 waterDepth, 2 full, 3 forestInner, 4 forestOuter, 5 road, 6 track,
//   7 slow, 8-9 howManyObjects, 10-11 howManyHardObjects, 12-14 gradient.
//
// HARD exclusions are never bypassed, not even by the legacy compatibility
// path: grass on a road or inside a building is always wrong, and no amount of
// bad 2001 map data makes it right. Water is here for the same reason.
const GEO_EXCLUDE_HARD: u32 = 0x00000c63u; // waterDepth | road | track | howManyHardObjects
// SOFT exclusions the diagnostic override may relax. Some legacy Everon WRP
// revisions mark broad ordinary ground as forest, which would otherwise leave
// the whole island bare.
const GEO_EXCLUDE_SOFT: u32 = 0x00000018u; // forestInner | forestOuter
// Bit 2 (`full`) is deliberately in NEITHER: legacy Everon marks normal ground
// with it, so excluding it removes every blade from valid grass terrain.
//
// The upper half of the word is written by TerrainWgpu::BakeAuthoredGrassMask
// from the map's OWN per-texel LCA surface mask (see the layout comment in
// TerrainWgpu.cpp -- change both together):
//   bit  15     the cell's answer is authored, so the fields below are real
//   bits 16-22  clutter coverage 0..127, relative to the world's densest
//               clutter character. This is what makes Takistan's grass
//               (character total 0.94) and its desert (0.12) different fields.
//   bits 23-27  a dense surface id for the surface dominating the cell,
//               31 = none. Carried for per-surface clutter VARIETY; unused here.
const GEO_AUTHORED: u32 = 0x00008000u;
const GEO_COVERAGE_MASK: u32 = 0x007f0000u;
const GEO_COVERAGE_SHIFT: u32 = 16u;
const GEO_SURFACE_MASK: u32 = 0x0f800000u;
const GEO_SURFACE_SHIFT: u32 = 23u;
const BLADE_SEGMENTS: u32 = 5u;
const VERTS_PER_CARD: u32 = BLADE_SEGMENTS * 6u;
const NEAR_CLUMP_BLADES: u32 = 6u;
const VERTS_PER_NEAR_BLADE: u32 = BLADE_SEGMENTS * 6u;
const VERTS_PER_NEAR_CLUMP: u32 = NEAR_CLUMP_BLADES * VERTS_PER_NEAR_BLADE;
// UNIQUE vertices per blade, which is what the vertex shader is now invoked for.
//
// A blade is a strip of BLADE_SEGMENTS quads, and drawing it non-indexed issues six
// vertices per quad -- 30 for five segments. But a quad's upper two corners are the
// next quad's lower two, and this shader derives EVERY output from just `t` (the row)
// and `left` (the side): see the `corner` decode this replaces, where `upper` and
// `left` were the only things `corner` fed. So the duplicated corners were producing
// bit-identical vertices, 30 invocations for 12 distinct results.
//
// Indexed, the post-transform cache reuses each computed vertex and the shader runs
// (BLADE_SEGMENTS + 1) * 2 times instead. That is 12 instead of 30 for the near blade
// and 8 instead of 18 for the mid one -- a 2.5x and 2.25x cut in vertex work for
// geometry that is identical by construction, not merely similar.
const VERTS_PER_NEAR_BLADE_UNIQUE: u32 = (BLADE_SEGMENTS + 1u) * 2u;
// Mid LOD's unit is a clump: six independently varied three-segment ribbons.
// It deliberately uses opaque geometry rather than a photographed alpha card,
// preserving early-Z while keeping a volumetric silhouette at 25-160 m.
const MID_CLUMP_BLADES: u32 = 6u;
const MID_BLADE_SEGMENTS: u32 = 3u;
const VERTS_PER_MID_BLADE: u32 = MID_BLADE_SEGMENTS * 6u;
const VERTS_PER_MID_CLUMP: u32 = MID_CLUMP_BLADES * VERTS_PER_MID_BLADE;
const VERTS_PER_MID_BLADE_UNIQUE: u32 = (MID_BLADE_SEGMENTS + 1u) * 2u;

fn hash11(p: vec2<f32>) -> f32 {
    let h = dot(p, vec2<f32>(127.1, 311.7));
    return fract(sin(h) * 43758.5453123);
}

// Stable integer-cell random numbers. The old sine hash plus a half-cell
// jitter left the placement grid faintly visible on broad terrain. This
// avalanche hash and deliberately overlapping jitter remove the rows while
// retaining deterministic world-space placement as the camera moves.
fn hash_u32(x_in: u32) -> u32 {
    var x = x_in;
    x = (x ^ (x >> 16u)) * 0x7feb352du;
    x = (x ^ (x >> 15u)) * 0x846ca68bu;
    return x ^ (x >> 16u);
}

fn hash_cell(cell: vec2<i32>, salt: u32) -> u32 {
    let x = bitcast<u32>(cell.x);
    let z = bitcast<u32>(cell.y);
    return hash_u32(x * 0x9e3779b9u ^ z * 0x85ebca6bu ^ salt);
}

fn hash_cell01(cell: vec2<i32>, salt: u32) -> f32 {
    return f32(hash_cell(cell, salt) >> 8u) * (1.0 / 16777216.0);
}

fn hash_cell2(cell: vec2<i32>, salt: u32) -> vec2<f32> {
    return vec2<f32>(hash_cell01(cell, salt), hash_cell01(cell, salt ^ 0x68bc21ebu));
}

// Smooth world-space value noise is the deterministic equivalent of the
// reference project's clump texture. It controls a broad field, while the
// per-cell hash keeps neighbouring blades from becoming visibly uniform.
fn clump_noise(world_xz: vec2<f32>, frequency: f32, salt: u32) -> f32 {
    let p = world_xz * frequency;
    let base = vec2<i32>(floor(p));
    let f = fract(p);
    let s = f * f * (vec2<f32>(3.0) - 2.0 * f);
    let a = hash_cell01(base, salt);
    let b = hash_cell01(base + vec2<i32>(1, 0), salt);
    let c = hash_cell01(base + vec2<i32>(0, 1), salt);
    let d = hash_cell01(base + vec2<i32>(1, 1), salt);
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

// Begin pure straw palette.
// A dry root remains darker than its tip. The previous 0.55 luma-independent
// floor could lift low-albedo blades into pale grey masses under blue sky fill.
// Keep a warm straw reflectance, with less blue and a modestly lower floor.
// Shared by point shading and its exact affine footprint integral below.
fn grass_straw_palette() -> vec4<f32> {
    return vec4<f32>(0.70, 0.58, 0.20, 0.42);
}
fn grass_straw_luma_gain() -> f32 { return 1.25; }
fn blade_dry_point(colour: vec3<f32>, height_t: f32, patch_mask: f32) -> vec3<f32> {
    let palette = grass_straw_palette();
    let luma = dot(colour, vec3<f32>(0.2126, 0.7152, 0.0722));
    let straw = palette.rgb * (palette.w + grass_straw_luma_gain() * luma);
    return mix(colour, straw, patch_mask * (0.45 + 0.55 * height_t));
}
// End pure straw palette.

// Sun-bleached patches: broad areas of the field shift toward dry straw and
// brighten, the way real meadow burns off unevenly. Driven by its own coarse
// noise field rather than the density or tint fields, so dry ground does not
// correlate with thin ground -- correlated variation reads as one pattern
// rather than several.
//
// `height_t` biases it up the blade: tips dry out first, roots stay green.
fn dry_patch(colour: vec3<f32>, world_xz: vec2<f32>, height_t: f32) -> vec3<f32> {
    let amount = clamp(grass.look.y, 0.0, 1.0);
    if (amount <= 0.001) { return colour; }
    let scale = max(grass.look.z, 0.002);
    let field = clump_noise(world_xz, scale, 0x93b5e1a7u);
    // Only the top of the noise range dries, so this makes PATCHES rather than
    // washing the whole field. A broad smooth fade avoids sharply mottled colour islands.
    // Not `patch`: that is a WGSL reserved keyword and fails composition.
    let patch_mask = smoothstep(1.0 - amount, 1.0 - amount * 0.05, field);
    // Straw keeps the source's luminance structure so blade detail survives.
    return blade_dry_point(colour, height_t, patch_mask);
}

// Grass albedo saturation, pushed about the luma axis so brightness is
// unchanged. Applied to every grass LOD from one control, before lighting, so
// what the sun does to the field is unaffected.
fn grass_saturation(colour: vec3<f32>) -> vec3<f32> {
    let amount = clamp(grass.look.x, 0.0, 2.0);
    let luma = dot(colour, vec3<f32>(0.2126, 0.7152, 0.0722));
    return max(mix(vec3<f32>(luma), colour, amount), vec3<f32>(0.0));
}

// Blade texture detail is a near-LOD feature: a 64x256 blade texture on a
// ribbon a few pixels wide aliases badly, and the mid ring starts past 25 m.
// Fading by distance covers both LODs with one fragment shader.
fn blade_texture_strength(world_rel: vec3<f32>) -> f32 {
    return 1.0 - smoothstep(14.0, 35.0, length(world_rel));
}

// Outer edge of the mid blade ring, shared by cs_place_mid (which grows up to it)
// and cs_place_far (which starts past it).
//
// `far_radius` may only CLAMP this when the far ring is actually on. Clamping it
// unconditionally meant far_radius = 0 ("no outer ring") collapsed mid_end to 0,
// and cs_place_mid's `mid_end <= near_radius` guard then rejected every mid
// candidate -- turning the far ring off silently deleted the mid ring with it and
// ended all grass at the near radius.
// MID_RING_MAX matches GrassSettings::midRadius' upper bound. It used to be 160
// here against a 210 m slider, so the top 50 m of that slider did nothing.
const MID_RING_MAX: f32 = 1000.0;
fn mid_requested_radius() -> f32 {
    let natural = max(grass.near_radius + 10.0, min(MID_RING_MAX, grass.near_radius * 2.5));
    return clamp(select(natural, grass.look.w, grass.look.w > grass.near_radius), grass.near_radius + 10.0, MID_RING_MAX);
}
fn mid_ring_end() -> f32 {
    let want = mid_requested_radius();
    // Defensive reach guard. Bounded spacing and the two-cell dispatch margin
    // now preserve the requested disc even at high coverage and card density.
    let reach = max(grass.place.y * 0.5 - 1.0, 1.0) * mid_placement_spacing();
    return clamp(min(want, reach), grass.near_radius + 10.0, MID_RING_MAX);
}

// LOD dissolve band, in metres. Each ring thins out across it while the next
// thickens, so a ring no longer ends on a hard circle and swap to a different
// representation in one step.
fn lod_band() -> f32 {
    return clamp(grass.place.z, 0.0, 40.0);
}

// Stochastic dissolve. `presence` is the ring's coverage at this distance; a
// per-cell hash decides whether this particular candidate survives. Because the
// hash is world-space deterministic, an individual clump does not blink in and
// out as the camera moves -- it disappears once, at its own threshold distance.
//
// A DIFFERENT salt from the coverage hash on purpose: reusing the density seed
// would remove exactly the same candidates the density test already thinned, so
// the two would compound into a visible hole rather than a fade.
fn lod_dissolve(cell_id: vec2<i32>, presence: f32) -> bool {
    if (presence >= 0.999) { return true; }
    if (presence <= 0.001) { return false; }
    return hash_cell01(cell_id, 0x5a1c93b7u) <= presence;
}

// Cache the surviving fraction so the vertex shaders can also taper the blade,
// which turns the last few metres of a ring into a shrink rather than a pop.
fn pack_lod_fade(fade: f32) -> u32 {
    return pack2x16unorm(vec2<f32>(clamp(fade, 0.0, 1.0), 0.0));
}

// Height multiplier from the cached fade. It deliberately stops at 0.45 rather
// than 0: the dissolve removes the clump, so shrinking it to nothing as well
// would thin the join twice over.
fn lod_height_scale(packed_z: u32) -> f32 {
    return mix(0.45, 1.0, unpack2x16unorm(packed_z).x);
}

// Which photographed atlas layer a clump uses. Layer 0 is the primary clump and
// 1..8 are the local families, each with its own weight so a single suspect
// plate can be removed -- or isolated with the force override -- without
// touching the rest of the field.
// Opaque box of a photo layer, (u0, u1, v0, v1). Defaults to the full quad, so
// a layer with no transparent margin -- or no atlas at all -- is untrimmed.
fn photo_layer_bounds(layer: u32) -> vec4<f32> {
    let b = grass.layer_bounds[min(layer, 31u)];
    // A degenerate or unset box would collapse the card; fall back to the quad.
    if (b.y - b.x < 0.02 || b.w - b.z < 0.02) { return vec4<f32>(0.0, 1.0, 0.0, 1.0); }
    return b;
}

fn photo_layer_weight(index: u32) -> f32 {
    return max(grass.layer_weights[index / 4u][index % 4u], 0.0);
}

// The atlas run this blade's SURFACE declares, unpacked from the instance.
//
//   bits 0-2  species (unchanged)
//   bits 3-7  first atlas layer, 0-31
//   bits 8-10 how many layers follow it, 0 = this surface has none
//
// The whole point of the contiguous run is that selection is `first + hash*count`
// -- an unpack and a multiply, no loop over a mask and no second texture fetch.
// The field arrives free: cs_place* already has the geography texel in a register
// for the exclusion tests, so writing it into the instance costs nothing there,
// and the vertex shader already reads `packed.w` for the species.
fn instance_layer_run(packed_w: u32) -> vec2<u32> {
    return vec2<u32>((packed_w >> 3u) & 31u, (packed_w >> 8u) & 7u);
}

// The "Photo grass mix" slider (renderer.z): the fraction of world-space patches
// that draw from the FULL variety. 0 = ONE variety everywhere -- the surface's
// first clutter class on a map-clutter world, the primary clump on a loose-card
// world -- which is the owner's "a single variety stopped the flicker" setting,
// now reachable on both paths. Before this it was only consulted on the legacy
// global-mix path, so on every map that declares its own clutter runs (Takistan,
// Stratis) the slider did nothing at all.
fn photo_variety_mix() -> f32 {
    return clamp(grass.renderer.z, 0.0, 1.0);
}

fn pick_photo_layer_run(patch_cell: vec2<f32>, run: vec2<u32>) -> u32 {
    let forced = grass.photo_mix.x;
    if (forced > -0.5) { return min(u32(forced + 0.5), 31u); }
    // A surface whose clutter the map actually declares picks from its own run.
    if (run.y > 0u) {
        // Patches outside the mix keep the run's first class: single variety.
        if (hash11(patch_cell + vec2<f32>(19.0, 71.0)) >= photo_variety_mix()) {
            return min(run.x, 31u);
        }
        let pick = u32(hash11(patch_cell + vec2<f32>(43.0, 11.0)) * f32(run.y));
        return min(run.x + min(pick, run.y - 1u), 31u);
    }
    // Otherwise the legacy global mix: every OFP world, and any Arma surface
    // whose clutter classes all failed to resolve a model.
    return pick_photo_layer(patch_cell);
}

fn pick_photo_layer(patch_cell: vec2<f32>) -> u32 {
    let forced = grass.photo_mix.x;
    if (forced > -0.5) { return min(u32(forced + 0.5), 31u); }
    if (hash11(patch_cell + vec2<f32>(19.0, 71.0)) >= photo_variety_mix()) {
        return 0u;
    }
    var total = 0.0;
    for (var i = 0u; i < 8u; i = i + 1u) { total = total + photo_layer_weight(i); }
    // Every family switched off is a legitimate request for the primary clump
    // only, not a reason to divide by zero.
    if (total <= 0.0001) { return 0u; }
    var pick = hash11(patch_cell + vec2<f32>(43.0, 11.0)) * total;
    var chosen = 8u;
    for (var i = 0u; i < 8u; i = i + 1u) {
        let w = photo_layer_weight(i);
        if (pick < w && chosen == 8u) { chosen = i; }
        pick = pick - w;
    }
    return chosen + 1u;
}

// The map's own clutter density for this cell, 0..1.
//
// A cell the bake did not answer -- every OFP cell, and any Arma cell whose
// material carries no authored mask -- returns 1.0, so those worlds keep exactly
// the density they had. Where it IS authored, the number is the fraction of the
// cell's mask texels that selected a clutter-bearing surface, multiplied by that
// surface's own authored probability total, which is deliberately not normalised
// per surface: the remainder is bare ground, and that is what separates a field
// from scree.
fn geo_clutter_coverage(geo: u32) -> f32 {
    if ((geo & GEO_AUTHORED) == 0u) { return 1.0; }
    return f32((geo & GEO_COVERAGE_MASK) >> GEO_COVERAGE_SHIFT) * (1.0 / 127.0);
}

// Stable thinning by the authored coverage. Keyed on a salt of its own rather
// than reusing the candidate's survival seed, so the coverage field thins the
// set independently instead of correlating with the density hash and carving
// the same blades out of every ring.
fn authored_thinned(cell_id: vec2<i32>, geo: u32) -> bool {
    let cover = geo_clutter_coverage(geo);
    if (cover >= 0.999) { return false; }
    return hash_cell01(cell_id, 0x51a3f7c9u) >= cover;
}

// PHOTO CARD coverage for this cell, 0..1: the fraction of clutter-grid cells
// (card_placement_spacing, 1.11 m) that grow a card. Blades ignore this.
//
// One card = one clutter object, so on a cell the map's own bake answered
// (GEO_AUTHORED) the map's density is `authored_thinned` alone: a card per
// clutter cell, thinned by the surface's authored probability -- which is what
// Arma does with clutterGrid x probability. Where the bake did NOT answer (every
// OFP world with cards forced on, any unmasked Arma cell) there is no authored
// number, and card_look.x (0.24 by default) stands in. With auto off, card_look.x
// multiplies everywhere, on top of the bake's own thinning, so 1.0 is the map's
// density and 0.24 is a quarter of it.
//
// Coverage <-> spacing: mean card spacing = card grid / sqrt(coverage), so 0.24
// on the 1.11 m grid is one card every ~2.3 m, ~0.19 cards per m^2.
// Photo card size. The stock card is 0.5-2.3 m tall and wider than tall
// (vs_grass_mid_tuft: 0.34-0.78 x blade_height 1.4 x 1.7 x family 0.62-1.24),
// against Arma clutter plants of ~0.3-0.9 m -- two to three times the plant it
// stands for, which at the map's own density is most of what reads as a solid
// mat. 1.0 keeps the stock size so nothing changes until the slider moves; the
// shadow shader applies the same factor.
fn photo_card_scale() -> f32 {
    let s = grass.card_look.z;
    return select(clamp(s, 0.25, 2.0), 1.0, s <= 0.0);
}

fn card_coverage_for(geo: u32) -> f32 {
    let manual = clamp(grass.card_look.x, 0.0, 1.0);
    let authored = (geo & GEO_AUTHORED) != 0u;
    return select(manual, 1.0, authored && grass.card_look.y > 0.5);
}

// Stable card thinning, salted apart from both the density seed and the
// authored-coverage seed so the three thin the field independently.
fn card_thinned(cell_id: vec2<i32>, geo: u32) -> bool {
    let cover = card_coverage_for(geo);
    if (cover >= 0.999) { return false; }
    return hash_cell01(cell_id, 0x2e7d9b41u) >= cover;
}

// Coverage multiplier from a world-space noise map, so density is patchy rather
// than uniform. Strength 0 = flat; 0.55 reproduces the previous hardcoded
// 0.45..1.35 range. `clumping` stays the master blend so it can still be
// dialled out entirely from the Grass tab.
fn density_field(world_xz: vec2<f32>) -> f32 {
    let strength = clamp(grass.render_flags.w, 0.0, 1.0);
    let scale = max(grass.render_flags.z, 0.002);
    let field = clump_noise(world_xz, scale, 0xc7136d5bu);
    let patchy = mix(1.0 - strength, 1.0 + strength * 0.64, field);
    // A second, metre-scale field creates deliberate bare pockets rather than
    // merely tinting a uniform lawn. It is gated by the existing noise strength:
    // zero remains a useful flat-density diagnostic setting.
    let bare_gap = smoothstep(0.12, 0.30, clump_noise(world_xz, 0.24, 0x8f54a231u));
    return mix(1.0, patchy * bare_gap, grass.debug_flags.y);
}

// The renderer integrates the scroll slider and weather into shared phases.
// Reading the phase here avoids t * v(t), whose jumps grow with mission age.
fn sample_wind_field(world_xz: vec2<f32>, height_t: f32, seed: f32) -> WindField {
    let strength = clamp(grass.wind_strength, 0.0, 3.0);
    let base_angle = grass.wind_direction * 0.01745329252;
    let base_direction = vec2<f32>(cos(base_angle), sin(base_angle));
    // Advect two fields at distinct scales.  The broad field turns coherent
    // gusts gradually; the smaller field supplies strength and tip flutter.
    //
    // Convention: `wind_direction` is the direction the wind travels TOWARD,
    // which is also the direction blades bend. Sampling noise at `p + v*t`
    // makes the pattern travel along `-v`, so the scroll is negated -- gust
    // fronts previously swept across the field opposite to the blades' lean.
    let broad_scroll = grass.wind_advection.xy;
    let gust_scroll = grass.wind_advection.zw;
    let direction_noise = clump_noise(world_xz + broad_scroll, 0.006, 0x0d7e31a5u);
    // Stretch the existing gust sample into coherent fronts perpendicular to
    // the wind. Integrated phase keeps their location stable when weather or
    // the camera changes. This replaces a sample; it does not add a noise field.
    let gust_position = world_xz + gust_scroll;
    let front_size = max(grass.native2.z, 10.0);
    // Frozen heading lives in the existing flutter spare lanes. Changing the
    // wind turns transport velocity, never absolute world-space coordinates.
    let front_direction = grass.wind_flutter.zw;
    let cross_direction = vec2<f32>(-front_direction.y, front_direction.x);
    let front_position = vec2<f32>(dot(gust_position, front_direction),
                                  dot(gust_position, cross_direction) / 3.0) / front_size;
    let gust_coordinates = select(gust_position * 0.022, front_position, grass.native2.y >= 0.0);
    let gust_noise = clump_noise(gust_coordinates, 1.0, 0xa12f7c59u);
    // There is always a small travelling sway. Stronger, soft-edged gusts
    // ride on top of it instead of leaving most of the field motionless,
    // which is the important visual distinction in the reference shader.
    let gust_pulse = pow(smoothstep(0.40, 0.84, gust_noise), 2.0);
    let local_gust = mix(0.18, 1.0, gust_pulse);
    let gust = select(local_gust, mix(0.55, local_gust, clamp(grass.native2.y, 0.0, 1.0)), grass.native2.y >= 0.0);
    let direction_angle = base_angle + (direction_noise - 0.5) * min(strength, 1.5) * 1.10;
    let flutter_scroll = grass.wind_flutter.xy;
    let flutter_noise = clump_noise(world_xz + flutter_scroll + base_direction * (height_t * height_t * 4.0) +
                                    vec2<f32>(seed * 19.0, seed * 31.0),
                                    0.105, 0x3f5a91c7u);
    var result: WindField;
    result.direction = vec2<f32>(cos(direction_angle), sin(direction_angle));
    result.gust = gust;
    // Phase-shift the fine field up the blade: roots stay locked while tips
    // gain small independent turbulence inside a travelling gust.
    result.turbulence = (flutter_noise - 0.5) * (0.035 + 0.085 * gust) * height_t;
    return result;
}

// Two seed-offset, advected noise fields replace the former short shared sine
// cycles, so rotor flutter never visibly restarts in unison across the field.
fn rotor_flutter_direction(world_xz: vec2<f32>, seed: f32) -> vec2<f32> {
    let time_a = vec2<f32>(terrain.time * 13.7, -terrain.time * 9.1);
    let time_b = vec2<f32>(-terrain.time * 7.3, terrain.time * 15.9);
    let a = clump_noise(world_xz * 0.65 + time_a + vec2<f32>(seed * 37.0, seed * 61.0), 0.38, 0x49c28e17u);
    let b = clump_noise(world_xz * 0.65 + time_b + vec2<f32>(seed * 71.0, seed * 23.0), 0.38, 0xb71d4a63u);
    let direction = vec2<f32>(a - 0.5, b - 0.5);
    return select(vec2<f32>(1.0, 0.0), normalize(direction), dot(direction, direction) > 0.0001);
}

fn hm_load(ix: i32, iz: i32) -> f32 {
    let x = clamp(ix, 0, i32(terrain.hm_width) - 1);
    let z = clamp(iz, 0, i32(terrain.hm_height) - 1);
    return textureLoad(heightmap, vec2<i32>(x, z), 0).x;
}

fn sample_height(world_xz: vec2<f32>) -> f32 {
    let coord = (world_xz - terrain.world_origin) / terrain.terrain_grid;
    let cell = floor(coord);
    let f = coord - cell;
    let ix = i32(cell.x);
    let iz = i32(cell.y);
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + 1, iz);
    let y10 = hm_load(ix, iz + 1);
    let y11 = hm_load(ix + 1, iz + 1);
    if (f.x <= 1.0 - f.y) {
        return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x;
    }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y;
}

fn sample_normal(world_xz: vec2<f32>) -> vec3<f32> {
    let s = terrain.terrain_grid;
    let hx0 = sample_height(world_xz - vec2<f32>(s, 0.0));
    let hx1 = sample_height(world_xz + vec2<f32>(s, 0.0));
    let hz0 = sample_height(world_xz - vec2<f32>(0.0, s));
    let hz1 = sample_height(world_xz + vec2<f32>(0.0, s));
    return normalize(vec3<f32>(-(hx1 - hx0), 2.0 * s, -(hz1 - hz0)));
}

// Player/vehicle contact and the persistent track ring, resolved once per
// accepted blade. Returns xy = flatten direction, z = strength and w = the
// rotor-wash turbulence amount. Persistent tracks intentionally keep w at 0.
//
// This used to run per VERTEX in vs_grass/vs_grass_mid: a 96-iteration loop
// with a length() and two smoothsteps, repeated 60 times per near blade even
// though every input is the instance position. The shadow shader skipped it
// entirely, so flattened grass still cast upright shadows.
fn eval_flatten(world_xz: vec2<f32>) -> vec4<f32> {
    let interactor_delta = world_xz - vec2<f32>(grass.interactor_x, grass.interactor_z);
    let interactor_distance = length(interactor_delta);
    var strength = 0.0;
    var rotor_wash = 0.0;
    if (grass.interactor_radius > 0.01) {
        // A controlled helicopter encodes RPM as (1, 1.5]. Decode it before
        // applying the pressure, while ordinary player/vehicle contact keeps
        // its direct [0, 1] strength.
        let controlled_rotor = grass.interactor_strength > 1.001;
        let interactor_strength = select(grass.interactor_strength,
                                        (grass.interactor_strength - 1.0) * 2.0,
                                        controlled_rotor);
        strength = (1.0 - smoothstep(grass.interactor_radius * 0.25, grass.interactor_radius, interactor_distance)) *
            min(interactor_strength, 1.0);
        if (controlled_rotor) {
            rotor_wash = strength;
        }
    }
    var direction = select(vec2<f32>(0.0), interactor_delta / interactor_distance, interactor_distance > 0.001);
    // Contact stamps hold their full imprint and then recover over the last
    // third of their life, so a trail thins out instead of blinking away. The
    // lifetime is a setting rather than the former fixed 25-60 s, and the ring
    // is now consumed by distance walked rather than by elapsed time, so what
    // survives is a length of trail.
    let lifetime = max(grass.photo_mix.z, 5.0);
    let recover_from = lifetime * 0.66;
    for (var i = 0u; i < 256u; i = i + 1u) {
        let track = grass.tracks[i];
        if (track.radius <= 0.01 || track.age >= lifetime) { continue; }
        let delta = world_xz - vec2<f32>(track.x, track.z);
        let distance = length(delta);
        let imprint = (1.0 - smoothstep(track.radius * 0.20, track.radius, distance)) *
            (1.0 - smoothstep(recover_from, lifetime, track.age));
        if (imprint > strength) {
            strength = imprint;
            rotor_wash = 0.0;
            direction = select(vec2<f32>(0.0), delta / distance, distance > 0.001);
        }
    }
    // Rotor wash spreads out from the rotor disc. It is deliberately steady:
    // downwash presses the field flat, it does not make the blades bounce.
    for (var i = 0u; i < 4u; i = i + 1u) {
        let wash = grass.downwash[i];
        if (wash.radius <= 0.01 || wash.strength <= 0.01) { continue; }
        let delta = world_xz - vec2<f32>(wash.x, wash.z);
        let distance = length(delta);
        let bend = (1.0 - smoothstep(wash.radius * 0.12, wash.radius, distance)) * wash.strength;
        // Prefer active rotor wash on equal pressure so a player helicopter's
        // normal controlled-vehicle footprint still receives turbulence.
        if (bend >= strength) {
            strength = bend;
            rotor_wash = bend;
            direction = select(vec2<f32>(0.0), delta / distance, distance > 0.001);
        }
    }
    return vec4<f32>(direction, strength, rotor_wash);
}

// How far a crushed plant is pressed down, as a fraction of its own height.
// 0.55 is the constant this replaces, so the default is the long-standing look.
fn imprint_depth() -> f32 {
    return clamp(grass.photo_mix.w, 0.0, 0.95);
}

// The same, for a photographed clump card. A card needs a MUCH stronger response
// than a blade to read as walked-on: one plate stands for a whole clump and is
// nearly two metres across, so tipping it the ~30 degrees that flattens a blade
// leaves an obviously upright bush. This lays it down and shortens it hard, which
// is what makes a photo-grass trail look like the procedural one.
fn imprint_depth_card() -> f32 {
    return min(0.95, imprint_depth() * 1.45);
}

// Species layers in blade_atlas.rs: 0..4 grass, 4..6 weed, 6..8 flower.
const SPECIES_GRASS_END: u32 = 4u;
const SPECIES_WEED_END: u32 = 6u;

// Species, chosen per CLUMP rather than per blade. Two independent fields:
// a coarse one decides which GROUP a patch belongs to (so weeds and flowers
// appear in drifts, the way they actually grow), and a finer one varies the
// member within that group. Picking independently per blade would turn eight
// distinct silhouettes into uniform visual noise.
fn pick_species(world_xz: vec2<f32>) -> u32 {
    let weed = clamp(grass.species_mix.x, 0.0, 1.0);
    let flower = clamp(grass.species_mix.y, 0.0, 1.0 - weed);
    // Coarse patch field (~25 m) -> group.
    let group_noise = clump_noise(world_xz, 0.04, 0x6f2ad913u);
    // Finer field (~7 m) -> member of that group.
    let member = clump_noise(world_xz, 0.14, 0x2b91f4d7u);
    if (group_noise < flower) {
        return SPECIES_WEED_END + min(u32(member * f32(LAYERS - SPECIES_WEED_END)),
                                      LAYERS - SPECIES_WEED_END - 1u);
    }
    if (group_noise < flower + weed) {
        return SPECIES_GRASS_END + min(u32(member * f32(SPECIES_WEED_END - SPECIES_GRASS_END)),
                                       SPECIES_WEED_END - SPECIES_GRASS_END - 1u);
    }
    return min(u32(member * f32(SPECIES_GRASS_END)), SPECIES_GRASS_END - 1u);
}

// Per-species blade shape. Weeds are broader and shorter; flowers are narrower
// stems that must NOT taper away at the tip or the petal head would be drawn on
// a point. Returns (width scale, height scale, taper exponent).
// Legacy shape: three profiles for eight species, so the four grass species were
// geometrically IDENTICAL and only differed by texture. That is what "all the
// blades are the same shape" looks like from the outside. Kept as the blend
// target for variety = 0 so the old look is still exactly reachable.
fn species_shape_legacy(species: u32) -> vec3<f32> {
    if (species >= SPECIES_WEED_END) {
        // Flower: slim stem, taller, and a near-constant width so the head reads.
        return vec3<f32>(0.85, 1.15, 0.18);
    }
    if (species >= SPECIES_GRASS_END) {
        // Weed: broad flat leaf, shorter, blunt tip.
        return vec3<f32>(1.9, 0.82, 0.42);
    }
    return vec3<f32>(1.0, 1.0, 0.65);
}

// One profile per species: (width scale, height scale, taper exponent). The
// taper exponent is what actually reads as "a different kind of grass" -- a high
// exponent narrows to a needle, a low one keeps width to a blunt tip.
fn species_shape_varied(species: u32) -> vec3<f32> {
    switch (species) {
        // 0..4 grass: fine upright, stock, broad arching, tall wisp.
        // The taper EXPONENT decides whether this reads as grass or as a spike, and the values
        // here were far too high. width = pow(1 - t, exponent), so 0.65 is already down to 64%
        // width at mid-height -- a spear. A real blade holds most of its width up the stem and
        // gives it all up near the tip, which is a LOW exponent: 0.26 is still 83% at mid-height
        // and then falls away quickly.
        case 0u: { return vec3<f32>(0.74, 1.06, 0.34); }
        case 1u: { return vec3<f32>(1.00, 1.00, 0.26); }
        case 2u: { return vec3<f32>(1.34, 0.92, 0.20); }
        case 3u: { return vec3<f32>(0.62, 1.18, 0.46); }
        // 4..6 weed: broad flat leaf, then a shorter blunter one.
        case 4u: { return vec3<f32>(1.90, 0.82, 0.22); }
        case 5u: { return vec3<f32>(1.52, 0.70, 0.15); }
        // 6..8 flower: stem, then a taller thinner stem. Near-constant so a head can read.
        case 6u: { return vec3<f32>(0.85, 1.15, 0.12); }
        default: { return vec3<f32>(0.68, 1.32, 0.09); }
    }
}

// variety 0 reproduces the legacy three profiles exactly; 1 gives eight distinct
// ones. Blending rather than switching means the dev-tools slider is continuous
// and an A/B against the old look needs no rebuild.
fn species_shape_mixed(species: u32, variety: f32) -> vec3<f32> {
    return mix(species_shape_legacy(species), species_shape_varied(species), clamp(variety, 0.0, 1.0));
}

// RGB is the thin-blade transmission tint and W is authored thickness. Grass
// stays green-gold, broad weeds transmit more olive light, and flower stems are
// cooler and weaker instead of sharing one orange plastic backlight.
fn species_transmission(species: u32) -> vec4<f32> {
    switch (species) {
        case 0u: { return vec4<f32>(0.78, 0.90, 0.24, 0.72); }
        case 1u: { return vec4<f32>(0.66, 0.82, 0.18, 0.88); }
        case 2u: { return vec4<f32>(0.72, 0.86, 0.20, 1.00); }
        case 3u: { return vec4<f32>(0.84, 0.92, 0.30, 0.58); }
        case 4u: { return vec4<f32>(0.58, 0.72, 0.16, 1.18); }
        case 5u: { return vec4<f32>(0.62, 0.76, 0.18, 1.05); }
        case 6u: { return vec4<f32>(0.74, 0.84, 0.42, 0.50); }
        default: { return vec4<f32>(0.78, 0.88, 0.48, 0.44); }
    }
}

// In daylight backlight, blue sky fill used to dominate the shaded grass side.
// Move that fill towards neutral warm light at unchanged luminance; keep the
// nighttime sky and the direct/transmitted sunlight untouched.
fn grass_sky_fill(n: vec3<f32>, view_dir: vec3<f32>, light_dir: vec3<f32>) -> vec3<f32> {
    let sky = sky_irradiance(n);
    let luma = dot(sky, vec3<f32>(0.2126, 0.7152, 0.0722));
    let warm = vec3<f32>(1.02, 1.0, 0.90);
    let neutral = warm * luma / dot(warm, vec3<f32>(0.2126, 0.7152, 0.0722));
    let daylight = smoothstep(-0.02, 0.15, light_dir.y);
    let backlight = smoothstep(0.05, 0.70, dot(view_dir, -light_dir));
    return mix(sky, neutral, daylight * backlight * 0.65);
}

// `geo` carries the surface's atlas run in bits 23-30 (see the geography word
// layout in TerrainWgpu.cpp); it is moved into packed.w bits 3-10 so the vertex
// shader can read it from the instance it already loads.
fn pack_flatten(flatten: vec4<f32>, species: u32, geo: u32) -> vec4<u32> {
    let first = (geo >> 23u) & 31u;
    let count = (geo >> 28u) & 7u;
    return vec4<u32>(pack2x16snorm(clamp(flatten.xy, vec2<f32>(-1.0), vec2<f32>(1.0))),
                     pack2x16unorm(vec2<f32>(clamp(flatten.z, 0.0, 1.0), clamp(flatten.w, 0.0, 1.0))),
                     0u, (species & 7u) | (first << 3u) | (count << 8u));
}

// The player helicopter uses an elevated interactor strength as a rotor marker.
// Re-evaluate that marker at draw time as well as caching it in the instance:
// this guarantees active downwash keeps moving even when a persistent track
// later becomes the strongest flattening source for the same blade.
fn active_rotor_wash(world_xz: vec2<f32>, crush: f32, cached_wash: f32) -> f32 {
    if (grass.interactor_strength <= 1.001 || grass.interactor_radius <= 0.01) {
        return cached_wash;
    }
    let delta = world_xz - vec2<f32>(grass.interactor_x, grass.interactor_z);
    let distance = length(delta);
    let live_wash = (1.0 - smoothstep(grass.interactor_radius * 0.18, grass.interactor_radius, distance)) * crush;
    return max(cached_wash, live_wash);
}

@compute @workgroup_size(8, 8, 1)
fn cs_place(@builtin(global_invocation_id) gid: vec3<u32>) {
    let grid_dim = u32(max(grass.place.x, 8.0));
    if (gid.x >= grid_dim || gid.y >= grid_dim || grass.enabled < 0.5) { return; }
    // Six ribbons replace the old two crossed ribbons, so one clump covers
    // roughly three former cells and stays within the established vertex budget.
    //
    // This spacing is deliberately INDEPENDENT of near_radius: the grid is sized
    // to the radius instead (see NEAR_GRID_MAX in grass/mod.rs). Deriving spacing
    // from the radius kept the accepted count constant, so the slider only ever
    // thinned the field -- and made the disc around the player sparser than the
    // fixed-spacing mid ring beyond it.
    //
    // Photo cards do NOT use the blade spacing at all -- see card_placement_spacing.
    let near_spacing = near_placement_spacing();
    let half = f32(grid_dim) * 0.5;
    let snap = floor(frame.cam_pos.xz / near_spacing) * near_spacing;
    let cell = vec2<f32>(f32(gid.x) - half, f32(gid.y) - half);
    let cell_world = snap + cell * near_spacing;
    let cell_id = vec2<i32>(floor(cell_world / near_spacing));
    let seed = hash_cell01(cell_id, 0x4d2f91c3u);
    // ±0.925 cells: candidates may cross their nominal cell boundaries, so
    // the dense field has no obvious square lattice or marching rows.
    let jitter = (hash_cell2(cell_id, 0x19a8b437u) - vec2<f32>(0.5)) * (near_spacing * 1.85);
    let world_xz = cell_world + jitter;
    let delta = world_xz - frame.cam_pos.xz;
    let coverage = min(grass.density, 1.0) * clamp(grass.lod_density.x, 0.0, 1.0) * density_field(world_xz);
    if (seed > coverage) { return; }
    // Dissolve out over the last `band` metres instead of stopping dead on the
    // near radius; cs_place_mid fades in across exactly the same band.
    //
    // Capped at HALF the radius, which matters the moment the radius is derived
    // rather than dialled. The dissolve band is a fixed 30 m by default, so a
    // near radius below 30 m made `max(near_radius - band, 0)` collapse to 0 and
    // the ring dissolved from the CAMERA outward -- full density only exactly at
    // the eye, and mean presence over the ring falling from 55% to 24%. The old
    // fixed 83 m radius always exceeded the band, so this could never fire; the
    // projected-size clamp is what exposed it, and it is a latent bug in its own
    // right that any short near radius would have hit.
    let band = min(lod_band(), grass.near_radius * 0.5);
    let distance = length(delta);
    let near_fade = 1.0 - smoothstep(max(grass.near_radius - band, 0.0), grass.near_radius, distance);
    if (!lod_dissolve(cell_id, near_fade)) { return; }
    let map_max = terrain.world_origin + vec2<f32>(f32(terrain.hm_width - 1u), f32(terrain.hm_height - 1u)) * terrain.terrain_grid;
    if (any(world_xz < terrain.world_origin) || any(world_xz >= map_max)) { return; }
    let geocell = clamp(vec2<i32>(floor((world_xz - terrain.world_origin) / terrain.land_grid)), vec2<i32>(0), vec2<i32>(i32(terrain.land_range) - 1));
    let geo = textureLoad(geography, geocell, 0).x;
    // C++ marks only cells whose base terrain texture is a named grass material.
    // This prevents the procedural pass from treating clear desert or dirt as grass.
    // Exclude water, roads/tracks, forest areas, and hard building/obstacle
    // cells. Bit 2 (`full`) is intentionally NOT excluded: legacy Everon
    // marks broad normal ground with it, so rejecting it removes every blade
    // despite the surface being valid grass terrain.
    if ((geo & 0x80000000u) == 0u) { return; }
    if ((geo & GEO_EXCLUDE_HARD) != 0u) { return; }
    if (grass.debug_flags.x < 0.5 && (geo & GEO_EXCLUDE_SOFT) != 0u) { return; }
    if (authored_thinned(cell_id, geo)) { return; }
    // Cards only: the card coverage (map's own where authored, else card_look.x).
    if (cards_on_near() && card_thinned(cell_id, geo)) { return; }
    let y = sample_height(world_xz);
    if (in_terrain_hole(vec3<f32>(world_xz.x, y, world_xz.y))) { return; } // preserve ground above a bounded cave mouth
    if (y <= terrain.sea_level + 0.35) { return; }
    let normal = sample_normal(world_xz);
    if (normal.y < 0.70) { return; }
    let rel = vec3<f32>(world_xz.x, y, world_xz.y) - frame.cam_pos.xyz;
    let clip = frame.proj * frame.view * vec4<f32>(rel, 1.0);
    if (clip.w <= 0.0 || abs(clip.x) > clip.w * 1.15 || abs(clip.y) > clip.w * 1.15) { return; }
    let out_index = atomicAdd(&placement_count[0], 1u);
    if (out_index >= MAX_INSTANCES) {
        // The counter is copied straight into the indirect instance count, so an
        // overflowing count would ask the draw for instances the buffer does not
        // hold. Pin it at capacity; only overflowing threads pay for this.
        atomicMin(&placement_count[0], MAX_INSTANCES);
        return;
    }
    instances[out_index].pos_seed = vec4<f32>(world_xz.x, y, world_xz.y, seed);
    var packed = pack_flatten(eval_flatten(world_xz), pick_species(world_xz), geo);
    packed.z = pack_lod_fade(near_fade);
    instances[out_index].packed = packed;
}

// The middle ring uses a stable half-density grid and opaque multi-blade clumps.
// It bridges the detailed near clumps and distant coverage without any
// camera-facing tile swap or frame-to-frame randomisation.
@compute @workgroup_size(8, 8, 1)
fn cs_place_mid(@builtin(global_invocation_id) gid: vec3<u32>) {
    let mid_grid_dim = u32(max(grass.place.y, 8.0));
    if (gid.x >= mid_grid_dim || gid.y >= mid_grid_dim || grass.enabled < 0.5) { return; }
    let mid_end = mid_ring_end();
    if (mid_end <= grass.near_radius) { return; }
    // Radius controls coverage only: this spacing is fixed and the grid is sized
    // from the radius instead, so mid density is unchanged all the way to 400 m.
    // Photo cards place on the card grid instead (card_placement_spacing).
    let mid_spacing = mid_placement_spacing();
    let half = f32(mid_grid_dim) * 0.5;
    let snap = floor(frame.cam_pos.xz / mid_spacing) * mid_spacing;
    let cell = vec2<f32>(f32(gid.x) - half, f32(gid.y) - half);
    let cell_world = snap + cell * mid_spacing;
    let cell_id = vec2<i32>(floor(cell_world / mid_spacing));
    let seed = hash_cell01(cell_id, 0x37c4a51du);
    let jitter = (hash_cell2(cell_id, 0x9426d87bu) - vec2<f32>(0.5)) * (mid_spacing * 1.85);
    let world_xz = cell_world + jitter;
    let delta = world_xz - frame.cam_pos.xz;
    let distance2 = dot(delta, delta);
    let coverage = min(grass.density, 1.0) * clamp(grass.lod_density.y, 0.0, 1.0) * density_field(world_xz);
    // The SAME cap cs_place applies. The two rings are complementary -- near
    // thins out across exactly the band mid thickens in across -- so if only one
    // of them clamped, a short near radius would leave the pair either
    // double-covering or under-covering the join.
    let band = min(lod_band(), grass.near_radius * 0.5);
    // A small overlap avoids a bare annulus at the detailed/mid and mid/far
    // joins; the dissolve band widens that overlap into a real crossfade.
    let mid_start = select(max(0.0, grass.near_radius - mid_spacing * 1.5 - band), 0.0, mid_cards_from_camera());
    if (distance2 < mid_start * mid_start || distance2 > mid_end * mid_end || seed > coverage) { return; }
    // Fade IN as the near ring fades out, and OUT as the far ring fades in.
    let distance = sqrt(distance2);
    let fade_in = select(smoothstep(mid_start, max(mid_start, grass.near_radius), distance), 1.0, mid_cards_from_camera());
    let fade_out = 1.0 - smoothstep(max(mid_end - band, mid_start), mid_end, distance);
    let mid_fade = min(fade_in, fade_out);
    if (!lod_dissolve(cell_id, mid_fade)) { return; }
    let map_max = terrain.world_origin + vec2<f32>(f32(terrain.hm_width - 1u), f32(terrain.hm_height - 1u)) * terrain.terrain_grid;
    if (any(world_xz < terrain.world_origin) || any(world_xz >= map_max)) { return; }
    let geocell = clamp(vec2<i32>(floor((world_xz - terrain.world_origin) / terrain.land_grid)), vec2<i32>(0), vec2<i32>(i32(terrain.land_range) - 1));
    let geo = textureLoad(geography, geocell, 0).x;
    if ((geo & 0x80000000u) == 0u || (geo & GEO_EXCLUDE_HARD) != 0u) { return; }
    if (grass.debug_flags.x < 0.5 && (geo & GEO_EXCLUDE_SOFT) != 0u) { return; }
    if (authored_thinned(cell_id, geo)) { return; }
    // Cards only: the card coverage (map's own where authored, else card_look.x).
    if (cards_on_mid() && card_thinned(cell_id, geo)) { return; }
    let y = sample_height(world_xz);
    if (in_terrain_hole(vec3<f32>(world_xz.x, y, world_xz.y))) { return; } // preserve ground above a bounded cave mouth
    if (y <= terrain.sea_level + 0.35 || sample_normal(world_xz).y < 0.70) { return; }
    let rel = vec3<f32>(world_xz.x, y, world_xz.y) - frame.cam_pos.xyz;
    let clip = frame.proj * frame.view * vec4<f32>(rel, 1.0);
    if (clip.w <= 0.0 || abs(clip.x) > clip.w * 1.15 || abs(clip.y) > clip.w * 1.15) { return; }
    let out_index = atomicAdd(&placement_count[0], 1u);
    if (out_index >= MAX_MID_INSTANCES) {
        atomicMin(&placement_count[0], MAX_MID_INSTANCES);
        return;
    }
    instances[out_index].pos_seed = vec4<f32>(world_xz.x, y, world_xz.y, seed);
    var packed = pack_flatten(eval_flatten(world_xz), pick_species(world_xz), geo);
    packed.z = pack_lod_fade(mid_fade);
    packed.w |= 1u << 11u;
    instances[out_index].packed = packed;
}

// Coarse outer ring: a terrain-conforming coverage field. This follows the
// reference project's distance-LOD principle without visible CPU tile
// relocation or hard mesh swaps at tile borders.
@compute @workgroup_size(8, 8, 1)
fn cs_place_far(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= FAR_GRID_DIM || gid.y >= FAR_GRID_DIM || grass.enabled < 0.5) { return; }
    // The outer edge dissolves over `band` metres BEYOND the requested radius,
    // never inside it: fading inward would quietly shorten the draw distance the
    // slider asks for. So the candidate grid has to reach `far_radius + band`,
    // and the band is capped at a fraction of the radius so the short presets do
    // not dilate their spacing much (see the density note below).
    // Floored so the dissolve's two smoothstep edges can never coincide: the
    // Grass tab can set the band to 0, and `smoothstep(e, e, x)` is a division by
    // zero. At 1 mm the edge is hard again, which is what a 0 band asks for.
    let far_band = max(min(lod_band(), grass.far_radius * 0.10), 0.001);
    let far_end = grass.far_radius + far_band;
    // Keep the fixed 384x384 far candidate grid bounded even at the 5 km
    // developer radius: farther fields automatically use wider, cheaper cells.
    // The grid is a FIXED 384^2, so widening the reach costs no candidates, no
    // instances and no memory -- only a slightly wider cell, which thins the far
    // field by (1 + far_band/far_radius)^-2. At the default 30 m dissolve that is
    // -4.8% at the 1200 m flying preset, and the 0.10 cap bounds the worst case
    // at -17.4% for radii under 300 m, where the far ring is a minor contributor
    // anyway.
    let far_spacing = max(max(grass.spacing * 4.0, 1.0), far_end / (f32(FAR_GRID_DIM) * 0.5 - 1.0));
    let half = f32(FAR_GRID_DIM) * 0.5;
    let snap = floor(frame.cam_pos.xz / far_spacing) * far_spacing;
    let cell = vec2<f32>(f32(gid.x) - half, f32(gid.y) - half);
    let cell_world = snap + cell * far_spacing;
    let cell_id = vec2<i32>(floor(cell_world / far_spacing));
    let seed = hash_cell01(cell_id, 0xb1e4c025u);
    // Small stable jitter plus overlapping coverage tiles avoids regular rows
    // without leaving kilometre-scale holes in the distant field.
    let jitter = (hash_cell2(cell_id, 0x7b32d119u) - vec2<f32>(0.5)) * (far_spacing * 0.30);
    let world_xz = cell_world + jitter;
    let delta = world_xz - frame.cam_pos.xz;
    let distance2 = dot(delta, delta);
    // Start one coarse cell after the near ring; this avoids double-drawing
    // while keeping the LOD join visually continuous.
    let mid_end = mid_ring_end();
    let near_start = mid_end + far_spacing * 0.5;
    // Keep the outer ring visually continuous. It is still economical (one
    // triangle per tuft), but no longer becomes invisible just past 60 m.
    let far_coverage = grass.density * clamp(grass.lod_density.z, 0.0, 1.0) * density_field(world_xz);
    if (distance2 <= near_start * near_start || distance2 > far_end * far_end || seed > far_coverage) { return; }
    // Thicken in across the same band the mid ring thins out over, so the outer
    // proxy does not appear as a complete disc edge at one distance.
    //
    // And thin out again over the last band, which the mid ring has always done
    // (`fade_out` above) and this ring never did: it rejected everything past
    // `far_radius` outright, so the whole field ended on one hard circle. From
    // the air that circle is the horizon of the grass and it tracks the camera,
    // which is the same complaint as vegetation popping in.
    let band = lod_band();
    let distance = sqrt(distance2);
    let fade_in = smoothstep(near_start, near_start + band, distance);
    let fade_out = 1.0 - smoothstep(max(grass.far_radius, near_start), far_end, distance);
    let far_fade = min(fade_in, fade_out);
    if (!lod_dissolve(cell_id, far_fade)) { return; }
    let map_max = terrain.world_origin + vec2<f32>(f32(terrain.hm_width - 1u), f32(terrain.hm_height - 1u)) * terrain.terrain_grid;
    if (any(world_xz < terrain.world_origin) || any(world_xz >= map_max)) { return; }
    let geocell = clamp(vec2<i32>(floor((world_xz - terrain.world_origin) / terrain.land_grid)), vec2<i32>(0), vec2<i32>(i32(terrain.land_range) - 1));
    let geo = textureLoad(geography, geocell, 0).x;
    if ((geo & 0x80000000u) == 0u || (geo & GEO_EXCLUDE_HARD) != 0u) { return; }
    if (grass.debug_flags.x < 0.5 && (geo & GEO_EXCLUDE_SOFT) != 0u) { return; }
    if (authored_thinned(cell_id, geo)) { return; }
    let y = sample_height(world_xz);
    if (in_terrain_hole(vec3<f32>(world_xz.x, y, world_xz.y))) { return; } // preserve ground above a bounded cave mouth
    if (y <= terrain.sea_level + 0.35) { return; }
    let normal = sample_normal(world_xz);
    if (normal.y < 0.70) { return; }
    let rel = vec3<f32>(world_xz.x, y, world_xz.y) - frame.cam_pos.xyz;
    let clip = frame.proj * frame.view * vec4<f32>(rel, 1.0);
    if (clip.w <= 0.0 || abs(clip.x) > clip.w * 1.15 || abs(clip.y) > clip.w * 1.15) { return; }
    let out_index = atomicAdd(&placement_count[0], 1u);
    if (out_index >= MAX_FAR_INSTANCES) {
        atomicMin(&placement_count[0], MAX_FAR_INSTANCES);
        return;
    }
    instances[out_index].pos_seed = vec4<f32>(world_xz.x, y, world_xz.y, seed);
    // The outer coverage proxy is a ground-conforming quad: nothing to flatten.
    instances[out_index].packed = vec4<u32>(0u);
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world_rel: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) height_t: f32,
    @location(3) seed: f32,
    @location(4) wind_gust: f32,
    // .xy = blade UV (u across the ribbon, v = 1 - height_t), .z = archetype
    // layer, .w = texture strength (faded out by distance so sub-pixel blades
    // sample flat colour instead of sparkling).
    @location(5) blade_uv: vec4<f32>,
    // Constant on each ribbon, including its near-ring taper jitter.
    @location(6) @interpolate(flat) blade_taper: f32,
};

@vertex
fn vs_grass(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VsOut {
    let inst = instances[instance_index].pos_seed;
    let seed = inst.w;
    let inst_packed = instances[instance_index].packed;
    let shape = species_shape_mixed(inst_packed.w & 7u, grass.shape_mix.x);
    let crush_dir = unpack2x16snorm(inst_packed.x);
    let crush_data = unpack2x16unorm(inst_packed.y);
    let crush = crush_data.x;
    let rotor_wash = active_rotor_wash(inst.xz, crush, crush_data.y);
    // `vertex_index` is now an INDEX VALUE, not a draw-order slot: the index buffer
    // (build_blade_indices in mod.rs) maps the six corners of each quad onto the two
    // rows they share with their neighbours. Row and side are all this shader ever
    // needed; see VERTS_PER_NEAR_BLADE_UNIQUE.
    let blade = vertex_index / VERTS_PER_NEAR_BLADE_UNIQUE;
    let blade_count = select(6.0, 12.0, grass.renderer.x > 0.5);
    let packed = vertex_index % VERTS_PER_NEAR_BLADE_UNIQUE;
    let row = packed / 2u;
    let left = (packed % 2u) == 0u;
    let t = f32(row) / f32(BLADE_SEGMENTS);
    // Four clump archetypes: compact upright, open meadow, low splayed, tall wisp.
    let archetype = u32(seed * 4.0);
    // Most meadow clumps stay short; the occasional wisp preserves a layered
    // field without returning the whole near ring to the former tall profile.
    let clump_height = select(select(select(0.60, 0.75, archetype == 1u), 0.52, archetype == 2u), 0.90, archetype == 3u);
    let clump_spread = select(select(select(0.10, 0.20, archetype == 1u), 0.28, archetype == 2u), 0.14, archetype == 3u);
    let clump_bend = select(select(select(0.70, 1.05, archetype == 1u), 1.45, archetype == 2u), 0.88, archetype == 3u);
    let blade_seed = hash11(inst.xz + vec2<f32>(f32(blade) * 17.0 + 3.0, f32(blade) * 29.0 + 11.0));
    let base_angle = seed * 6.2831853 + f32(blade) * (6.2831853 / blade_count) + (blade_seed - 0.5) * 0.55;
    let radial = vec3<f32>(cos(base_angle), 0.0, sin(base_angle));
    let blade_axis = vec3<f32>(-sin(base_angle), 0.0, cos(base_angle));
    let base_offset = radial * (clump_spread * mix(0.18, 1.0, blade_seed));
    let height = mix(0.32, 0.81, hash11(inst.xz + vec2<f32>(f32(blade) * 7.0, f32(blade) * 13.0))) *
        grass.blade_height * shape.y * clump_height * lod_height_scale(inst_packed.z) * native_blade_height_scale();
    let card_widen = mix(1.0, max(grass.cards.z, 1.0), grass.cards.x);
    let width = mix(0.016, 0.043, blade_seed) * shape.x * max(grass.species_mix.z, 0.05) * card_widen;
    let bend_jitter = 1.0 + (hash11(inst.xz + vec2<f32>(f32(blade) * 23.0, 57.0)) * 2.0 - 1.0) * clamp(grass.shape_mix.z, 0.0, 1.0);
    let static_bend = radial * mix(0.10, 0.34, hash11(inst.xz + vec2<f32>(f32(blade) * 19.0, 31.0))) * bend_jitter * max(grass.cards.w, 0.0) * height * clump_bend;
    let depth = imprint_depth();
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (depth * crush);
    let crushed_height = height * (1.0 - depth * crush);
    let wind = sample_wind_field(inst.xz + base_offset.xz, t, seed + blade_seed);
    let wind_bend = vec3<f32>(wind.direction.x, 0.0, wind.direction.y) * grass.wind_strength *
        (0.035 + 0.21 * wind.gust + wind.turbulence);
    // Rotor wash has its own fast, per-blade turbulence. It is driven by the
    // cached crush amount so normal grass remains governed solely by weather.
    let flutter_dir = rotor_flutter_direction(inst.xz + base_offset.xz, seed + blade_seed);
    let crush_flutter = vec3<f32>(flutter_dir.x, 0.0, flutter_dir.y) * height * (0.95 * rotor_wash);
    // Flowers use a near-flat taper exponent so the stem keeps its width up to
    // the tip -- a petal head painted on a point would vanish.
    // Per-blade taper jitter, multiplicative about 1.0 for the same reason as the
    // bend jitter: 0 is the stock look, not a shifted one.
    let taper_jitter = 1.0 + (hash11(inst.xz + vec2<f32>(f32(blade) * 13.0, 71.0)) * 2.0 - 1.0) * clamp(grass.shape_mix.y, 0.0, 1.0);
    let taper = pow(max(1.0 - t, 0.0), max(shape.z * taper_jitter, 0.05));
    let half_width = width * taper;
    let standing_bend = (static_bend + wind_bend) * (1.0 - depth * crush) + crush_flutter;
    let curve = (standing_bend + crush_bend) * (t * t);
    let tangent = vec3<f32>(0.0, crushed_height, 0.0) + (standing_bend + crush_bend) * (2.0 * t);
    // Preserve a minimum apparent silhouette when a card is nearly edge-on
    // to the camera. This mirrors the reference's view-space widening without
    // reconstructing a second model matrix or making distant ribbons explode.
    let blade_normal = normalize(cross(blade_axis, tangent));
    let view_dir = normalize(frame.cam_pos.xyz - inst.xyz);
    let edge_on = pow(1.0 - abs(dot(blade_normal, view_dir)), 4.0);
    let lateral = select(blade_axis * half_width, -blade_axis * half_width, left) * (1.0 + edge_on * 1.6);
    let local = base_offset + lateral + vec3<f32>(0.0, crushed_height * t, 0.0) + curve;
    let world = inst.xyz + local;
    let rel = world - frame.cam_pos.xyz;
    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    out.world_rel = rel;
    out.normal = normalize(cross(blade_axis, tangent));
    out.height_t = t;
    out.seed = seed;
    out.wind_gust = wind.gust;
    out.blade_uv = vec4<f32>(select(1.0, 0.0, left), 1.0 - t, f32(inst_packed.w & 7u),
                             blade_texture_strength(rel));
    out.blade_taper = max(shape.z * taper_jitter, 0.05);
    return out;
}

@vertex
fn vs_grass_mid(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VsOut {
    let inst = instances[instance_index].pos_seed;
    let seed = inst.w;
    let inst_packed = instances[instance_index].packed;
    let shape = species_shape_mixed(inst_packed.w & 7u, grass.shape_mix.x);
    let crush_dir = unpack2x16snorm(inst_packed.x);
    let crush_data = unpack2x16unorm(inst_packed.y);
    let crush = crush_data.x;
    let rotor_wash = active_rotor_wash(inst.xz, crush, crush_data.y);
    let blade = vertex_index / VERTS_PER_MID_BLADE_UNIQUE;
    let packed = vertex_index % VERTS_PER_MID_BLADE_UNIQUE;
    let row = packed / 2u;
    let left = (packed % 2u) == 0u;
    let t = f32(row) / f32(MID_BLADE_SEGMENTS);
    // Each member gets a stable radial base, orientation, height and bend. The
    // spacing remains at the clump centre, so interaction and placement stay
    // exactly as before while the rendered unit becomes a tuft of real blades.
    let blade_seed = hash11(inst.xz + vec2<f32>(f32(blade) * 17.0 + 3.0, f32(blade) * 29.0 + 11.0));
    let base_angle = seed * 6.2831853 + f32(blade) * 1.0471976 + (blade_seed - 0.5) * 0.55;
    let radial = vec3<f32>(cos(base_angle), 0.0, sin(base_angle));
    let blade_axis = vec3<f32>(-sin(base_angle), 0.0, cos(base_angle));
    let coverage_width = mid_coverage_width_scale(inst_packed.w);
    let base_offset = radial * mix(0.025, 0.16, blade_seed) * coverage_width;
    let height = mix(0.29, 0.71, hash11(inst.xz + vec2<f32>(f32(blade) * 7.0, f32(blade) * 13.0))) *
        grass.blade_height * shape.y * lod_height_scale(inst_packed.z) * native_blade_height_scale();
    let width = mix(0.016, 0.038, blade_seed) * shape.x * max(grass.species_mix.z, 0.05) * coverage_width;
    let static_bend = radial * mix(0.10, 0.32, hash11(inst.xz + vec2<f32>(f32(blade) * 19.0, 31.0))) * height;
    let depth = imprint_depth();
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (depth * crush);
    let crushed_height = height * (1.0 - depth * crush);
    let wind = sample_wind_field(inst.xz + base_offset.xz, t, seed + blade_seed);
    let wind_bend = vec3<f32>(wind.direction.x, 0.0, wind.direction.y) * grass.wind_strength *
        (0.030 + 0.18 * wind.gust + wind.turbulence);
    let flutter_dir = rotor_flutter_direction(inst.xz + base_offset.xz, seed + blade_seed);
    let crush_flutter = vec3<f32>(flutter_dir.x, 0.0, flutter_dir.y) * height * (0.95 * rotor_wash);
    let bend = (static_bend + wind_bend) * (1.0 - depth * crush) + crush_flutter;
    let curve = (bend + crush_bend) * (t * t);
    let tangent = vec3<f32>(0.0, crushed_height, 0.0) + (bend + crush_bend) * (2.0 * t);
    let blade_normal = normalize(cross(blade_axis, tangent));
    let view_dir = normalize(frame.cam_pos.xyz - inst.xyz);
    let edge_on = pow(1.0 - abs(dot(blade_normal, view_dir)), 4.0);
    let lateral = select(blade_axis, -blade_axis, left) * width * pow(max(1.0 - t, 0.0), shape.z) * (1.0 + edge_on * 1.25);
    let rel = inst.xyz + base_offset + lateral + vec3<f32>(0.0, crushed_height * t, 0.0) + curve - frame.cam_pos.xyz;
    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    out.world_rel = rel;
    out.normal = normalize(cross(blade_axis, tangent));
    out.height_t = t;
    out.seed = seed;
    out.wind_gust = wind.gust;
    out.blade_uv = vec4<f32>(select(1.0, 0.0, left), 1.0 - t, f32(inst_packed.w & 7u),
                             blade_texture_strength(rel));
    out.blade_taper = shape.z;
    return out;
}

// GRS-E — Arma-style mid LOD: two crossed quads carrying the game's own
// photographed tuft, instead of one procedural blade. One instance now stands
// for a clump rather than a single plant, which is why the mid ring can be far
// sparser and still read as continuous cover.
//
// 12 vertices (2 cards x 6) against the procedural path's 24, and the photo
// supplies detail no closed-form function reproduces. The trade is the alpha
// test: fs_grass_mid_tuft discards, so this path gives up early-Z. That is
// acceptable here and NOT on the dense near ring, which stays opaque ribbons.
@vertex
fn vs_grass_mid_tuft(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VsOut {
    let inst = instances[instance_index].pos_seed;
    let seed = inst.w;
    let inst_packed = instances[instance_index].packed;
    let field = clump_noise(inst.xz, 0.075, 0x48ac2f19u);
    let angle = mix(seed * 6.2831853, field * 6.2831853, grass.debug_flags.y);
    // Two cards at 90 degrees, so the clump keeps volume from any viewing angle.
    let card = vertex_index / 6u;
    let card_angle = angle + select(0.0, 1.5707963, card != 0u);
    let axis = vec3<f32>(cos(card_angle), 0.0, sin(card_angle));

    let corner = vertex_index % 6u;
    // Quad: (0,0) (1,0) (1,1) / (0,0) (1,1) (0,1) in (u, vertical) space.
    let right = corner == 1u || corner == 2u || corner == 4u;
    let top = corner == 2u || corner == 4u || corner == 5u;
    let quad_u = select(0.0, 1.0, right);
    let quad_v = select(0.0, 1.0, top);
    // Trim the quad to the plate's opaque box. The margin outside it is texels
    // the alpha test always discarded -- but only after rasterising them and
    // sampling the texture, on a path that is entirely fill-bound. Shrinking the
    // geometry to the box removes that work and leaves the image identical,
    // because the plant occupies exactly the same world space either way.
    let patch_cell = floor(inst.xz / max(grass.renderer.w, 4.0));
    let layer = pick_photo_layer_run(patch_cell, instance_layer_run(inst_packed.w));
    let bounds = photo_layer_bounds(layer);
    let u = mix(bounds.x, bounds.y, quad_u);
    // Texture v runs downward, so the BOTTOM of the content is bounds.w and the
    // top is bounds.z; vh is the fraction of full card height, as before.
    let img_v = mix(bounds.w, bounds.z, quad_v);
    let vh = 1.0 - img_v;

    let height_seed = mix(hash11(inst.xz + 13.0), clump_noise(inst.xz, 0.21, 0xa47f3cd1u), grass.debug_flags.y * 0.72);
    // A tuft is a clump, so it is wider and taller than the single blade this
    // instance used to represent.
    // Four stable clump families stop one photographed silhouette repeating:
    // compact, open, tall and low/splayed. All are world-space deterministic.
    let family = u32(seed * 4.0);
    let family_height = select(select(select(0.78, 1.00, family == 1u), 1.24, family == 2u), 0.62, family == 3u);
    let family_width = select(select(select(0.72, 1.00, family == 1u), 1.20, family == 2u), 1.38, family == 3u);
    let height = mix(0.34, 0.78, height_seed) * grass.blade_height * 1.7 * family_height *
        lod_height_scale(inst_packed.z) * photo_card_scale();
    let half_width = height * mix(0.55, 0.85, hash11(inst.xz + 23.0)) * family_width *
        mid_coverage_width_scale(inst_packed.w);

    let crush_dir = unpack2x16snorm(inst_packed.x);
    let crush_data = unpack2x16unorm(inst_packed.y);
    let crush = crush_data.x;
    let rotor_wash = active_rotor_wash(inst.xz, crush, crush_data.y);
    let card_depth = imprint_depth_card();
    let crushed_height = height * (1.0 - card_depth * crush);
    // 1.35x the vertical press as horizontal displacement, so a crushed card
    // rotates toward the ground rather than merely shrinking in place.
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (card_depth * 1.35 * crush);

    let wind = sample_wind_field(inst.xz, vh, seed);
    // A card sways on the GUST field only, plus card_tone.z of the turbulence.
    // The turbulence term is blade-TIP flutter -- a 0.105 /m noise scrolled at
    // ~97 m/s (gust_scroll * 2.1) is a ~10 Hz signal of +-3..6 cm at the tip.
    // On a 5 cm blade tip that reads as flutter; on a rigid 1-2 m plate it is
    // the whole plant vibrating at 10 Hz (~1 px at 50 m, ~8 px at 5 m) under a
    // 1 px alpha transition, which is temporal aliasing by construction and
    // reads as flicker at a fixed camera. The gust field (45 m features at
    // 30-45 m/s, ~1 Hz) is the smooth wind that remains. Mirrored in
    // vs_grass_tuft_shadow so the shadow sways with the card. Renderer default 0;
    // WGR_GRASS_CARD_FLUTTER=1 is the exact old motion for an A/B.
    let card_flutter = clamp(grass.card_tone.z, 0.0, 1.0);
    let wind_bend = vec3<f32>(wind.direction.x, 0.0, wind.direction.y) * grass.wind_strength *
        (0.030 + 0.18 * wind.gust + wind.turbulence * card_flutter);
    let flutter_dir = rotor_flutter_direction(inst.xz, seed);
    let crush_flutter = vec3<f32>(flutter_dir.x, 0.0, flutter_dir.y) * height * (0.95 * rotor_wash);
    // The whole card leans; roots stay pinned. Quadratic in height, as the
    // procedural blades bend, so a clump does not shear against its neighbours.
    let lean = (wind_bend * (1.0 - card_depth * crush) + crush_bend + crush_flutter) * (vh * vh);
    let lateral = axis * (u - 0.5) * 2.0 * half_width;
    let rel = inst.xyz + lateral + vec3<f32>(0.0, crushed_height * vh, 0.0) + lean - frame.cam_pos.xyz;

    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    out.world_rel = rel;
    // Card normal faces the viewer's side of the plane; the fragment shader
    // pulls it upright anyway, so a flat billboard normal is enough here.
    out.normal = normalize(vec3<f32>(-axis.z, 1.15, axis.x));
    out.height_t = vh;
    out.seed = seed;
    out.wind_gust = wind.gust;
    // Variation from ONE source image, so scattered clumps do not read as the
    // same stamp repeated: mirror half of them, and take a slightly different
    // horizontal slice per clump. Both are deterministic in world space, so a
    // clump keeps its identity as the camera moves.
    // The slice inset works within the trimmed box, not the raw image, so a
    // trimmed card still samples the same content it did untrimmed.
    let mirror = hash11(inst.xz + 5.0) < 0.5;
    let slice = mix(0.0, 0.18, hash11(inst.xz + 41.0)) * (bounds.y - bounds.x);
    var uu = clamp(mix(bounds.x + slice, bounds.y - slice, quad_u), bounds.x, bounds.y);
    if (mirror) { uu = bounds.x + bounds.y - uu; }
    out.blade_uv = vec4<f32>(uu, img_v, f32(layer), 1.0);
    return out;
}

@vertex
fn vs_grass_far(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VsOut {
    let inst = instances[instance_index].pos_seed;
    // A true distant-grass representation is coverage, not a handful of
    // giant vertical blades. Each retained coarse cell becomes one slightly
    // lifted, terrain-conforming quad. The field therefore remains legible
    // at kilometre ranges while retaining one indirect draw and no texture.
    let far_spacing = max(max(grass.spacing * 4.0, 1.0), grass.far_radius / (f32(FAR_GRID_DIM) * 0.5 - 1.0));
    let corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0),
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, 1.0), vec2<f32>(-1.0, 1.0));
    let jitter = (hash_cell2(vec2<i32>(floor(inst.xz / far_spacing)), 0x6d3b718fu) - vec2<f32>(0.5)) * 0.12;
    // The 0.66 half extent overlaps the at-most 0.30-cell placement jitter,
    // giving a continuous proxy field rather than separated distant patches.
    let world_xz = inst.xz + (corners[vertex_index] * 0.66 + jitter) * far_spacing;
    let y = sample_height(world_xz) + 0.045;
    let rel = vec3<f32>(world_xz.x, y, world_xz.y) - frame.cam_pos.xyz;
    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    out.world_rel = rel;
    out.normal = sample_normal(world_xz);
    out.height_t = 0.65;
    out.seed = inst.w;
    out.wind_gust = sample_wind_field(inst.xz, 0.65, inst.w).gust;
    // The outer coverage quad has no blade to texture; fs_grass_far ignores this.
    out.blade_uv = vec4<f32>(0.0);
    return out;
}

// Alpha cut-out variant. This exists as a SEPARATE entry point, rather than an
// `if` inside fs_grass, because the mere PRESENCE of `discard` in a fragment
// shader makes the driver disable early-Z for that pipeline -- whether or not the
// branch is ever taken. Measured on the reference mission: keeping the cutout
// behind a runtime flag inside fs_grass cost 1.761 ms against 1.053 ms in the
// grass colour pass, a 67% penalty paid with the feature switched OFF. Two
// pipelines, one shading function, no penalty on the default path.
@fragment
fn fs_grass_cards(in: VsOut) -> @location(0) vec4<f32> {
    let cutout = textureSampleBias(blade_tex, blade_samp, in.blade_uv.xy, i32(in.blade_uv.z), frame.renscale.x);
    if (cutout.a < grass.cards.y) {
        discard;
    }
    return grass_shade(in);
}

@fragment
fn fs_grass(in: VsOut) -> @location(0) vec4<f32> {
    return grass_shade(in);
}

// PROCEDURAL BLADE look controls (grass.blade_look), near blades and mid ribbons.
// Every default is the value that reproduces the pre-existing shading exactly,
// so the field only changes when a slider is moved:
//   .x self shadow 0   .y contrast 1.0   .z hue variation 0   .w root shade 0.70
fn blade_root_shade() -> f32 { return clamp(grass.blade_look.w, 0.0, 0.95); }
fn blade_contrast() -> f32 { return clamp(grass.blade_look.y, 0.25, 3.0); }
fn blade_hue_variation() -> f32 { return clamp(grass.blade_look.z, 0.0, 1.0); }
fn blade_self_shadow() -> f32 { return clamp(grass.blade_look.x, 0.0, 1.0); }

// Blade-on-blade shadowing, the cheap way: (a) occlusion toward the root of the
// tuft -- the lower a point on a blade, the more of its neighbours stand between
// it and the sky and the sun -- and (b) the cascade the near grass itself writes
// (draw_shadow submits the near ring), sampled at the blade like tuft_csm_shadow
// does for cards. Contact range only, for the same reason as the cards: past the
// first cascades a shadow texel is larger than a tuft and the sample degenerates
// into a flat darkening of the distance. Returns (sun occlusion 0..1, ambient AO
// multiplier 0..1); both are identity at strength 0.
fn blade_self_shadow_terms(world_rel: vec3<f32>, normal: vec3<f32>, height_t: f32,
                           taper: f32, detail: f32) -> vec2<f32> {
    let strength = blade_self_shadow();
    // Root occlusion: at full strength the base sees ~35% of the light a tip
    // does, and the occlusion reaches most of the way up the blade -- neighbours
    // in a closed field stand shoulder-height, not knee-height.
    var height_ao = smoothstep(0.0, 0.85, height_t);
    if (detail < 1.0 && strength > 0.001) {
        height_ao = mix(blade_height_ao_mean(taper), height_ao, detail);
    }
    let tuft_ao = mix(1.0 - 0.65 * strength, 1.0, height_ao);
    // Derivatives outside the branch: they must come from uniform control flow.
    let dwx = dpdx(world_rel);
    let dwy = dpdy(world_rel);
    var cast_shadow = 0.0;
    if (strength > 0.001) {
        let range_fade = 1.0 - smoothstep(30.0, 60.0, length(world_rel));
        cast_shadow = shadow_strength(world_rel, normal, 1.0, dwx, dwy) * strength * range_fade;
    }
    // The tuft occlusion also dims the sun a little (blades in the middle of a
    // clump are shaded by the ones in front), at half the ambient weight.
    let sun_occlusion = max(cast_shadow, (1.0 - tuft_ao) * 0.5);
    return vec2<f32>(sun_occlusion, tuft_ao);
}

// A blade can be unresolved even inside the near ring, especially from above.
// UV derivatives measure its projected width, so this works at every camera
// pitch and render resolution rather than relying on world distance.
fn blade_shading_detail(uv: vec2<f32>) -> f32 {
    let width_gradient = length(vec2<f32>(dpdx(uv.x), dpdy(uv.x)));
    let width_pixels = 1.0 / max(width_gradient, 0.0001);
    return select(1.0, smoothstep(0.75, 3.0, width_pixels), GRASS_FILTER_SUBPIXEL);
}

// Begin pure medium grass lighting.
// A thin blade can still resolve its vertical root/tip ramp. UV-y derivatives
// measure projected height, not world height: foreshortened aerial blades stay
// filtered. Require 3..12 pixels before restoring this lower-frequency detail.
fn blade_height_resolution(height_gradient: f32) -> f32 {
    return smoothstep(3.0, 12.0, 1.0 / max(height_gradient, 0.0001));
}
fn grass_medium_view_weight(view_vertical: f32) -> f32 {
    return 1.0 - smoothstep(0.70, 0.90, abs(view_vertical));
}
fn grass_ramp_detail(width_detail: f32, height_detail: f32, view_vertical: f32, enabled: bool) -> f32 {
    return select(width_detail, max(width_detail, height_detail * grass_medium_view_weight(view_vertical)), enabled);
}
// Rough leaf-cuticle response: a wide cosine^4 lobe, at most 3.5% of sun
// radiance. Its orientation belongs to metre-scale clumps, never seed/atlas
// fibres or moving per-blade normals. No extra sheen in unresolved/top views.
fn grass_broad_sun_response(alignment: f32, height_detail: f32, view_vertical: f32, enabled: bool) -> f32 {
    let lobe = pow(clamp(alignment, 0.0, 1.0), 4.0);
    return select(0.0, 0.035 * lobe * clamp(height_detail, 0.0, 1.0) * grass_medium_view_weight(view_vertical), enabled);
}
// End pure medium grass lighting.

// Pure footprint moments. An unresolved ribbon represents its entire tapered
// area, not whichever bright tip happened to hit the sample this frame. Width
// is (1-t)^p, so E[t^k] = k! / ((p+2)...(p+k+1)). Integrate the colour ramp,
// quadratic root shade and linear gust highlight together to retain their mean.
// This averages albedo only; it does not change silhouette or sample coverage.
struct BladeRampMean {
    colour: vec3<f32>,
    height_colour: vec3<f32>,
    height: f32,
};
fn blade_ramp_mean(base: vec3<f32>, tip: vec3<f32>, root_shade: f32,
                   gust_amount: f32, taper: f32) -> BladeRampMean {
    let p = max(taper, 0.05);
    let m1 = 1.0 / (p + 2.0);
    let m2 = m1 * 2.0 / (p + 3.0);
    let m3 = m2 * 3.0 / (p + 4.0);
    let m4 = m3 * 4.0 / (p + 5.0);
    let m5 = m4 * 5.0 / (p + 6.0);
    let a = 1.0 - root_shade;
    let delta = tip - base;
    let c0 = base * a;
    let c1 = delta * a;
    let c2 = base * root_shade;
    let c3 = delta * root_shade;
    let unlit = c0 + c1 * m1 + c2 * m2 + c3 * m3;
    let height_unlit = c0 * m1 + c1 * m2 + c2 * m3 + c3 * m4;
    let height2_unlit = c0 * m2 + c1 * m3 + c2 * m4 + c3 * m5;
    let gust_base = 1.0 + gust_amount * 0.035;
    let gust_tip = gust_amount * 0.075;
    return BladeRampMean(unlit * gust_base + height_unlit * gust_tip,
                         height_unlit * gust_base + height2_unlit * gust_tip, m1);
}

// dry_patch is affine in colour and height*colour. Keep that correlation:
// merely grading the mean at the mean height would lose the dry, bright tips.
fn blade_dry_mean(mean: BladeRampMean, patch_mask: f32) -> vec3<f32> {
    let luma = vec3<f32>(0.2126, 0.7152, 0.0722);
    let dry_colour = mean.colour * 0.45 + mean.height_colour * 0.55;
    let palette = grass_straw_palette();
    let straw = palette.rgb *
        (palette.w * (0.45 + 0.55 * mean.height) + grass_straw_luma_gain() * dot(dry_colour, luma));
    return mean.colour + patch_mask * (straw - dry_colour);
}

// Integral of smoothstep(0, .85, t) under the same taper area. The tail moments
// integrate t^2/t^3 over [.85,1], where the AO ramp has already reached one.
fn blade_height_ao_mean(taper: f32) -> f32 {
    let p = max(taper, 0.05);
    let m2 = 2.0 / ((p + 2.0) * (p + 3.0));
    let m3 = m2 * 3.0 / (p + 4.0);
    let q = 0.15;
    let tail = pow(q, p + 1.0);
    let q1 = q * (p + 1.0) / (p + 2.0);
    let q2 = q * q * (p + 1.0) / (p + 3.0);
    let q3 = q * q * q * (p + 1.0) / (p + 4.0);
    let tail2 = tail * (1.0 - 2.0 * q1 + q2);
    let tail3 = tail * (1.0 - 3.0 * q1 + 3.0 * q2 - q3);
    return clamp(3.0 * (m2 - tail2) / (0.85 * 0.85)
        - 2.0 * (m3 - tail3) / (0.85 * 0.85 * 0.85) + tail, 0.0, 1.0);
}
// Preserve the authored species palette on an unresolved native ribbon. Its
// opacity-weighted CPU mean excludes transparent margins; sampled alpha still
// belongs to coverage and is never altered here. Empty layers retain the sample.
fn native_blade_colour(sampled: vec3<f32>, mean: vec4<f32>, detail: f32) -> vec3<f32> {
    return select(sampled, mix(mean.rgb, sampled, detail), mean.w > 0.0);
}
// End pure footprint moments.

fn grass_shade(in: VsOut) -> vec4<f32> {
    let shading_detail = blade_shading_detail(in.blade_uv.xy);
    let height_detail = blade_height_resolution(length(vec2<f32>(dpdx(in.blade_uv.y), dpdy(in.blade_uv.y))));
    let view_dir = -in.world_rel / max(length(in.world_rel), 0.0001);
    let ramp_detail = grass_ramp_detail(shading_detail, height_detail, view_dir.y, GRASS_MEDIUM_LIGHTING);
    let world = in.world_rel + frame.cam_pos.xyz;
    let base = vec3<f32>(0.055, 0.095, 0.034);
    let tip = vec3<f32>(0.18, 0.23, 0.10);
    // 0.70 root shade = the long-standing mix(0.30, 1.0, t*t).
    let root = mix(1.0 - blade_root_shade(), 1.0, in.height_t * in.height_t);
    let field_tint = clump_noise(world.xz, 0.003, 0x5e3a91c7u);
    let blade_tint = mix(in.seed, field_tint, 0.55);
    let variation = mix(1.0, mix(0.78, 1.20, blade_tint), grass.debug_flags.z);
    // A restrained, field-coherent highlight makes gusts readable without
    // turning grass into emissive green waves.
    let gust_highlight = 1.0 + in.wind_gust * clamp(grass.wind_strength, 0.0, 1.5) * (0.035 + 0.075 * in.height_t);
    let procedural = mix(base, tip, in.height_t) * root * variation * gust_highlight;
    // GRS-D: the archetype layer supplies midrib, fibre and dry-tip detail. It
    // is blended toward the procedural colour by distance so the field-scale
    // tint, gust highlight and per-blade variation all still apply -- the
    // texture adds surface detail, it does not replace the palette.
    // The layer already carries the species' own root-to-tip gradient, so it
    // REPLACES the procedural ramp rather than multiplying it. Multiplying was
    // wrong: the atlas averages ~0.17 linear, so `procedural * blade * 2` scaled
    // near grass to about a third of its brightness while barely showing detail.
    // Photo veins can alias as sub-pixel blades move in the wind. A small
    // positive mip bias stabilises that fine detail while keeping it readable
    // at the close ranges where this near-LOD texture is actually visible.
    let blade = textureSampleBias(blade_tex, blade_samp, in.blade_uv.xy, i32(in.blade_uv.z), frame.renscale.x);
    var blade_colour = blade.rgb;
    if (grass.native.x > 0.0) {
        blade_colour = native_blade_colour(blade.rgb,
            grass.blade_layer_means[min(u32(in.blade_uv.z), 7u)], shading_detail);
    } else {
        // The procedural atlas has several green, yellow and brown species.
        // Keep their fibres and luminance relief, but give the OFP meadow one
        // restrained palette rather than small contrasting colour islands.
        let luma_weights = vec3<f32>(0.2126, 0.7152, 0.0722);
        let layer_mean = grass.blade_layer_means[min(u32(in.blade_uv.z), 7u)];
        let mean_luma = select(0.17, dot(layer_mean.rgb, luma_weights), layer_mean.w > 0.0);
        let texture_relief = clamp(dot(blade.rgb, luma_weights) / max(mean_luma, 0.02), 0.75, 1.25);
        blade_colour = mix(base, tip, in.height_t) * texture_relief;
    }
    // Alpha cut-out cards: the silhouette comes from the texture, not the quad,
    // which is what buys shape variety without more geometry. It costs the early-Z
    // the solid path enjoys, so it is a toggle and not the default -- measure
    // before switching it on for good.
    // The atlas carries its own species gradient, but it is authored shallow --
    // multiplied straight in, the near field lost the root ramp entirely and
    // read as one pale wash (the owner's "grass looks flat"). Re-apply 75% of
    // the procedural root shade on top, so the Blade root shade slider governs
    // the textured near field too instead of going inert inside 35 m.
    var textured = blade_colour * variation * gust_highlight * mix(1.0, root, 0.75);
    if (grass.native.x > 0.0 && grass.native2.x > 0.0) {
        // RFG-091: Reforger's blade layer is authored grey-green and takes its colour from
        // the satellite map (`SatMapLerp 0.8`). With no satellite pages on the native path,
        // the ground's mean albedo stands in: the atlas keeps its luminance structure, the
        // ground lends its hue and saturation.
        let luma = vec3<f32>(0.2126, 0.7152, 0.0722);
        let ground = grass.native.yzw;
        let recoloured = ground * (dot(blade_colour, luma) / max(dot(ground, luma), 0.02));
        // No root shade: the atlas tuft carries its own base darkening.
        textured = mix(blade_colour, recoloured, clamp(grass.native2.x, 0.0, 1.0)) * variation * gust_highlight;
    }
    // shape_mix.w scales the photo texture globally on top of the distance fade
    // already in blade_uv.w, so the detail can be dialled back without deleting
    // the atlas and falling all the way to procedural.
    let texture_amount = in.blade_uv.w * clamp(grass.shape_mix.w, 0.0, 1.0);
    var dry_colour = dry_patch(mix(procedural, textured, texture_amount), world.xz, in.height_t);
    if (ramp_detail < 1.0 && grass.native.x <= 0.0) {
        // Seed tint is constant per blade, and field tint fades across roughly 333 metres; keep
        // both rather than erasing the meadow's stable colour variation.
        var mean = blade_ramp_mean(base, tip, blade_root_shade(),
            in.wind_gust * clamp(grass.wind_strength, 0.0, 1.5), in.blade_taper);
        mean.colour *= variation;
        mean.height_colour *= variation;
        let amount = clamp(grass.look.y, 0.0, 1.0);
        var patch_mask = 0.0;
        if (amount > 0.001) {
            let field = clump_noise(world.xz, max(grass.look.z, 0.002), 0x93b5e1a7u);
            patch_mask = smoothstep(1.0 - amount, 1.0 - amount * 0.05, field);
        }
        dry_colour = mix(blade_dry_mean(mean, patch_mask), dry_colour, ramp_detail);
    }
    let graded = grass_saturation(dry_colour);
    // Per-patch hue drift on top of the luminance-only colour variation above:
    // warm straw one way, cool blue-green the other, on a smooth ~333 m field of its own
    // so it lines up with neither the dry patches nor the density. 0 = none.
    let hue_field = clump_noise(world.xz, 0.003, 0x7a1c3e5du);
    let hue_tint = mix(vec3<f32>(1.10, 1.00, 0.78), vec3<f32>(0.88, 1.00, 1.12), hue_field);
    // The hue drift exists for the procedural blades; on a native atlas it read as lilac.
    let hued = graded * mix(vec3<f32>(1.0), hue_tint, blade_hue_variation() * select(1.0, 0.0, grass.native.x > 0.0));
    // Contrast about the palette's own mid-tone (the root/tip pair at half height
    // and mean root shade), so 1.0 is exactly the untouched palette. Applied
    // before the tint, so the tint still does what it did.
    let pivot = mix(base, tip, 0.5) * 0.65;
    var albedo = grass.tint_procedural.rgb *
        max((hued - pivot) * blade_contrast() + pivot, vec3<f32>(0.0));
    if (grass.native.x > 0.0) {
        // RFG-091: the grading chain above (saturation, dry patches, hue drift, contrast
        // about a GREEN pivot, the procedural tint) was built for the procedural blades;
        // on a grey-brown native atlas the contrast step pushed every blade away from
        // green -- towards magenta. The native blade is its atlas, ground-tinted, lit.
        albedo = textured;
    }
    // Bend card normals toward an upright rounded-blade normal. This avoids
    // the flat dark-side look of a raw ribbon while preserving its silhouette.
    let upright = vec3<f32>(0.0, 1.0, 0.0);
    let resolved_normal = normalize(mix(normalize(in.normal), upright, 0.24));
    let n = normalize(mix(upright, resolved_normal, shading_detail));
    let light_dir = normalize(frame.sun_dir_world.xyz);
    let ndl = max(dot(n, light_dir), 0.0);
    let wrap = max((dot(n, light_dir) + 0.35) / 1.35, 0.0);
    let terrain_shadow = terrain_sun_shadow(world.xz, world.y);
    let cloud_lit = cloud_sun_shadow(world.xz);
    // Blade-on-blade: (sun occlusion, ambient AO). Identity at strength 0.
    // RFG-091: the blade-on-blade terms assume a bare procedural blade; the native atlas is
    // already a lit tuft, and the two together were black.
    let self_shadow = select(blade_self_shadow_terms(in.world_rel, n, in.height_t,
                             in.blade_taper, ramp_detail), vec2<f32>(0.0, 1.0),
                             grass.native.x > 0.0);
    let direct = frame.sun_diffuse.rgb * mix(ndl, wrap, 0.35) *
        (1.0 - max(terrain_shadow, self_shadow.x)) * cloud_lit;
    let ambient = grass_sky_fill(normalize(mix(n, vec3<f32>(0.0, 1.0, 0.0), 0.45)), view_dir, light_dir) * sky_vis_ao(world.xz) *
        self_shadow.y;
    let transmission = species_transmission(u32(in.blade_uv.z));
    let transmission_amount = pow(max(dot(view_dir, -light_dir), 0.0), 1.5) * (1.0 - ndl) *
        grass.debug_flags.w * transmission.w;
    let subsurface = transmission.rgb * transmission_amount * frame.sun_diffuse.rgb * (1.0 - terrain_shadow) * cloud_lit;
    // Reuse the existing 6m/11m stable fields for rounded clump orientation.
    // The width-filtered blade normal above remains the diffuse authority.
    let clump_normal = normalize(vec3<f32>((field_tint - 0.5) * 0.24, 1.0, (hue_field - 0.5) * 0.24));
    let half_direction = (light_dir + view_dir) / max(length(light_dir + view_dir), 0.0001);
    let sheen_amount = grass_broad_sun_response(dot(clump_normal, half_direction), height_detail,
        view_dir.y, GRASS_MEDIUM_LIGHTING);
    let sheen = frame.sun_diffuse.rgb * vec3<f32>(0.85, 1.0, 0.75) * sheen_amount *
        (1.0 - max(terrain_shadow, self_shadow.x)) * cloud_lit;
    let lit = albedo * (ambient + direct + subsurface) + sheen;
    let fogged = apply_fog(lit, in.world_rel);
    return vec4<f32>(select(lit, fogged, grass.render_flags.y >= 0.5), 1.0);
}

@fragment
fn fs_grass_far(in: VsOut) -> @location(0) vec4<f32> {
    let world = in.world_rel + frame.cam_pos.xyz;
    let field_noise = clump_noise(world.xz, 0.003, 0x5e3a91c7u);
    // Keep the proxy close to terrain colour: it supplies the distant grassy
    // field, not bright individual blades. The same aerial fog removes it
    // naturally at the horizon.
    let gust_highlight = 1.0 + in.wind_gust * clamp(grass.wind_strength, 0.0, 1.5) * 0.05;
    // The outer proxy's colour is DERIVED from the grass it stands in for, not
    // authored separately. Two hardcoded greens were the reason this ring read as
    // a different material painted on the ground: they were tuned against the
    // 2001 palette and never moved when the field switched to photographed
    // plates, so the far ring stayed dark olive while everything nearer went
    // green. Take the primary plate's measured mean through the same correction
    // chain as the cards when they are in use, and the procedural blade palette
    // when they are not, so the join is a change of detail, not of material.
    let photo_far = grass.species_mix.w > 0.5;
    // Same chain as tuft_shade, tone lift included, or the proxy would sit at
    // the plates' RAW tone under lifted cards -- the material change this ring
    // exists to avoid.
    let mean_a3 = grass.layer_means[0].rgb * PHOTO_TONE_CORRECTION *
        clamp(grass.renderer.y, 0.5, 2.5) * card_tone_gain(0u);
    let mean_luma = dot(mean_a3, vec3<f32>(0.2126, 0.7152, 0.0722));
    let photo_base = mix(vec3<f32>(mean_luma), mean_a3, clamp(grass.photo_mix.y, 0.0, 2.0));
    // The procedural blades run root-to-tip base..tip; a ground proxy stands for
    // the whole blade, so it uses the same pair averaged toward the tip.
    let procedural_base = mix(vec3<f32>(0.055, 0.095, 0.034), vec3<f32>(0.18, 0.23, 0.10), 0.65);
    let field_base = select(procedural_base, photo_base, photo_far);
    // Keep the field-scale light/dark mottle the proxy always had, about the
    // chosen base rather than between two constants.
    let tint_source = select(grass.tint_procedural.rgb, grass.tint_photo.rgb, photo_far);
    let albedo = tint_source *
        grass_saturation(dry_patch(field_base * mix(0.72, 1.28, field_noise) * gust_highlight,
                                   world.xz, 0.65));
    let macro_dx = clump_noise(world.xz + vec2<f32>(1.0, 0.0), 0.075, 0x4f93d71bu) -
        clump_noise(world.xz - vec2<f32>(1.0, 0.0), 0.075, 0x4f93d71bu);
    let macro_dz = clump_noise(world.xz + vec2<f32>(0.0, 1.0), 0.075, 0x4f93d71bu) -
        clump_noise(world.xz - vec2<f32>(0.0, 1.0), 0.075, 0x4f93d71bu);
    let macro_normal = normalize(vec3<f32>(-macro_dx * 0.24, 1.0, -macro_dz * 0.24));
    let n = normalize(mix(mix(in.normal, macro_normal, 0.42), vec3<f32>(0.0, 1.0, 0.0), 0.48));
    let light_dir = normalize(frame.sun_dir_world.xyz);
    let diffuse = max((dot(n, light_dir) + 0.35) / 1.35, 0.0);
    let terrain_shadow = terrain_sun_shadow(world.xz, world.y);
    let cloud_lit = cloud_sun_shadow(world.xz);
    let direct = frame.sun_diffuse.rgb * diffuse * (1.0 - terrain_shadow) * cloud_lit;
    let ambient = sky_irradiance(n) * sky_vis_ao(world.xz);
    let lit = albedo * (ambient + direct);
    let fogged = apply_fog(lit, in.world_rel);
    return vec4<f32>(select(lit, fogged, grass.render_flags.y >= 0.5), 1.0);
}

// GRS-E — photographed tuft card. Alpha-tested cutout, so it must discard in
// BOTH the colour and prepass entries or the prepass would stamp opaque
// rectangles into the depth/normal buffer.
// The atlas mip chain preserves coverage against 0.5 (blade_atlas.rs), so that
// stays the neutral value; the control exists because several local family
// plates carry JPEG opacity maps whose compression noise around thin stems
// flickers, and trimming those partial texels is the direct remedy.
fn tuft_alpha_cutoff() -> f32 {
    return clamp(grass.place.w, 0.05, 0.95);
}

// The A2/A3 tone correction every photographed plate goes through before the
// user's brightness: A3's clutter albedo is authored warmer/duller than CWA's
// meadow palette. ONE definition, so the card, its contrast pivot and the far
// proxy that stands in for it cannot drift apart.
const PHOTO_TONE_CORRECTION: vec3<f32> = vec3<f32>(0.74, 1.22, 0.56);

// PHOTO CARD TONE LIFT (card_tone.x = target luma, .y = gain cap; renderer-owned,
// see CardTone in grass/mod.rs). Per LAYER, from the plate mean the renderer
// measures at upload (layer_means), never below 1 and never above the cap.
//
// This stands in for the clutter MATERIAL, which the card path never reads. An
// Arma clutter plate is authored to be multiplied by its rvmat's lighting gains
// -- Takistan's c_plants / c_grass_desert rvmats are ambient 10, forcedDiffuse
// 2.5, diffuse 0 -- so the texels themselves are dark: the seven Takistan
// plates decode (Bohemia's ImageToPAA) to opaque linear luma means of 0.054 to
// 0.243, against 0.63 for the A3 stock plate this whole chain was tuned on.
// Drawn raw, under a sun that puts the terrain at ~0.44 linear, a TK card is a
// flat dark silhouette in BOTH rings. Lifting each plate's mean to the target
// puts every atlas where the tuning was done; a plate already at or above it is
// untouched, so an A3 atlas renders exactly as before.
//
// Not premultiplied alpha, measured: the plates' transparent texels carry
// colour and their partial-alpha texels are as bright as the opaque ones, so
// dividing by alpha would be wrong. Not the contrast pivot: layer_means are
// linear and per layer. The exact per-class gains want the C++ atlas builder to
// read each class's rvmat and pass a per-layer gain over the ABI; this is the
// data-driven stand-in until then. 0 target = off (the raw look, for A/B).
fn card_tone_gain(layer: u32) -> f32 {
    // Not `target`: a WGSL reserved word, and composition fails on it.
    let tone_target = grass.card_tone.x;
    if (tone_target <= 0.001) { return 1.0; }
    let plate_mean = grass.layer_means[min(layer, 31u)].rgb * PHOTO_TONE_CORRECTION;
    let plate_luma = dot(plate_mean, vec3<f32>(0.2126, 0.7152, 0.0722));
    return clamp(tone_target / max(plate_luma, 0.005), 1.0, max(grass.card_tone.y, 1.0));
}

// Near clumps already render into the cascade shadow map. Sample that same map
// at the cut-out cards so one clump can shade another -- grass-on-grass
// shadowing is most of what stops a field of photographed plates reading as
// stickers. Strength is a Grass-tab control: terrain and cloud shadows remain
// the authoritative long-range occluders, so this only has to supply contact
// contrast, and solid-black cards are a worse error than a flat one.
fn tuft_csm_shadow(world_rel: vec3<f32>, normal: vec3<f32>) -> f32 {
    // Contact shadowing only. Past the first cascades a shadow texel is larger
    // than a whole clump, so every card samples a map that its neighbours have
    // filled and the result degenerates into a flat darkening of the distance --
    // the second reason photographed grass got darker as it receded. Fade it out
    // where it stops describing anything.
    let range_fade = 1.0 - smoothstep(45.0, 95.0, length(world_rel));
    return shadow_strength(world_rel, normal, 1.0, dpdx(world_rel), dpdy(world_rel)) *
        clamp(grass.photo.z, 0.0, 1.0) * range_fade;
}

fn tuft_luma(uv: vec2<f32>, layer: i32) -> f32 {
    let c = textureSampleBias(tuft_tex, blade_samp, uv, layer, 0.75);
    // Weight by alpha so the transparent gaps BETWEEN stems read as background
    // rather than as dark texels, which would invert the contour of every edge.
    return dot(c.rgb, vec3<f32>(0.2126, 0.7152, 0.0722)) * c.a;
}

// Rebuild a per-texel surface normal from the photograph's own luma, so
// individual stems inside one card catch the sun differently. The base stays
// upright: the two crossed cards face different ways, and lighting their card
// normal made the same clump brighten or darken as the camera orbited it.
//
// `card_normal` is what vs_grass_mid_tuft wrote, from which the card's
// horizontal axis is recoverable exactly: n = normalize(-axis.z, 1.15, axis.x).
fn tuft_contour_normal(uv: vec2<f32>, layer: i32, card_normal: vec3<f32>) -> vec3<f32> {
    let strength = clamp(grass.photo.y, 0.0, 2.0);
    let up = vec3<f32>(0.0, 1.0, 0.0);
    if (strength <= 0.001) { return up; }
    let n = normalize(card_normal);
    let tangent = normalize(vec3<f32>(n.z, 0.0, -n.x));
    // Horizontal facing of the card, used for the vertical gradient's tilt.
    let facing_xz = vec3<f32>(n.x, 0.0, n.z);
    let facing = select(tangent, normalize(facing_xz), dot(facing_xz, facing_xz) > 0.0001);
    // Keep close relief, but filter it as the photograph becomes unresolved.
    // Preserving full-strength gradients at every mip made tiny wind-blown
    // stems flash between sunlit and shaded in aerial views.
    let footprint = max(length(dpdx(uv)), length(dpdy(uv)));
    let texel = max(2.0 / 1024.0, footprint * 1.5);
    let dimensions = vec2<f32>(textureDimensions(tuft_tex));
    let texel_footprint = max(length(dpdx(uv) * dimensions), length(dpdy(uv) * dimensions));
    let relief_detail = select(1.0, 1.0 - smoothstep(2.0, 8.0, texel_footprint), GRASS_FILTER_SUBPIXEL);
    let du = tuft_luma(uv + vec2<f32>(texel, 0.0), layer) - tuft_luma(uv - vec2<f32>(texel, 0.0), layer);
    // Texture v runs downward, so a positive dv means brighter BELOW.
    let dv = tuft_luma(uv + vec2<f32>(0.0, texel), layer) - tuft_luma(uv - vec2<f32>(0.0, texel), layer);
    let k = strength * 3.2;
    let tilt = tangent * (-du * k) + facing * (dv * k * 0.6);
    // Bound the tilt. `du` is an alpha-weighted luma difference, so at a stem
    // edge it is the whole stem luma: on a bright plate that is k * 0.5 = 3 at
    // the default strength, a normal 71 degrees off vertical whose sun term
    // swings from full to nothing across ONE texel. Every frame the wind moves
    // the taps a fraction of a texel, so that swing is per-pixel lighting
    // noise -- sparkle. Capping the tangent length at 1.2 (~50 degrees) keeps the
    // relief on every plate and removes only the flip; plates whose gradients
    // never reach it (the dark A2 sets, du ~0.05-0.1) are untouched.
    let tilt_len = length(tilt);
    let bounded = tilt * (min(tilt_len, 1.2) / max(tilt_len, 1e-5));
    return normalize(up + bounded * relief_detail);
}

// The ONE texture fetch every card entry point makes. Thin photographed stems
// otherwise flip alpha coverage between adjacent mip levels as the camera moves;
// a modest positive bias makes the card settle before it reaches sub-pixel size
// without softening close clumps. Colour, prepass and A2C twins all go through
// here so they sample the same mip of the same texel and agree on the alpha.
fn tuft_sample(in: VsOut) -> vec4<f32> {
    return textureSampleBias(tuft_tex, blade_samp, in.blade_uv.xy, i32(in.blade_uv.z), 0.75);
}

// Alpha-to-coverage for the cards under MSAA (grass/mod.rs builds the *_a2c
// pipelines with alpha_to_coverage_enabled when the scene is multisampled).
//
// This is what the "flickering" on the card path actually was. A card's alpha
// test used to be one hard `discard` at the pixel centre; the cards sway in the
// wind every frame (sample_wind_field runs on terrain.time), so a stem edge a
// pixel wide crossing a pixel centre pops that whole pixel on and off from one
// frame to the next -- and with several plates stacked over every point of the
// field, every pixel had several such edges. Nothing else on this path varies at
// a fixed camera: placement is a hash of the integer world cell, the atomic
// compaction only permutes instance ORDER, and depth resolves order.
//
// A2C gives an edge four coverage levels instead of one, so a moving edge
// slides through 25% steps rather than snapping, and the sharpen (the same
// Wyman/McGuire rescale the object foliage uses, gbuffer::a2c_coverage) keeps
// the transition ~1 px wide so JPEG alpha noise near the cutoff dithers instead
// of popping. The colour pass and the prepass twin MUST emit the same coverage
// or the prepass writes depth to samples the colour pass never shades: both call
// this from uniform control flow, before any discard, on the same sample.
fn tuft_coverage(tex_a: f32) -> f32 {
    return a2c_coverage(tex_a, tuft_alpha_cutoff());
}

// Shading shared by the discard and A2C colour entries. `tex` is tuft_sample(in).
fn tuft_shade(in: VsOut, tex: vec4<f32>) -> vec3<f32> {
    let world = in.world_rel + frame.cam_pos.xyz;
    let height_detail = blade_height_resolution(length(vec2<f32>(dpdx(in.blade_uv.y), dpdy(in.blade_uv.y))));
    // The photo already carries base-to-tip shading, so only the field-scale
    // tint and gust highlight are applied on top -- re-adding the procedural
    // root darkening would double up what the photograph already shows.
    let field_tint = clump_noise(world.xz, 0.006, 0x5e3a91c7u);
    let variation = mix(1.0, mix(0.82, 1.16, mix(in.seed, field_tint, 0.55)), grass.debug_flags.z);
    let gust_highlight = 1.0 + in.wind_gust * clamp(grass.wind_strength, 0.0, 1.5) * 0.05;
    // The authored clump is already the right colour -- measured opaque mean
    // (0.525, 0.622, 0.127), green on every texel -- so its own albedo is used
    // directly. No palette substitution: that was only needed for the legacy
    // 2001 PAA fallback, whose hue is grey-teal. If that fallback is ever the
    // active texture the mid ring will look desaturated, which is why the
    // authored PNG is preferred at load time.
    // A3's clutter albedo is authored warmer/duller than CWA's sunlit meadow
    // palette. Calibrate only the photographed-tuft path, never legacy grass.
    // The source plate is deliberately dry/brown and CWA's blue skylight makes
    // its uncorrected shadowed stems read slate grey. Lift green enough for a
    // meadow while retaining red/blue stem variation; the former 1.85 green
    // multiplier made bright patches unnaturally saturated.
    let photo_brightness = clamp(grass.renderer.y, 0.5, 2.5);
    // The per-layer material stand-in (card_tone_gain) multiplies BOTH the
    // sample and its contrast pivot below, so contrast stays brightness-neutral.
    let tone_gain = card_tone_gain(u32(in.blade_uv.z));
    let a3_albedo = tex.rgb * PHOTO_TONE_CORRECTION * photo_brightness * tone_gain;
    // The source plates have strong chlorophyll green. Keep their natural
    // stem variation but pull this card-only correction toward luma before
    // the user's global grass saturation is applied.
    let photo_luma = dot(a3_albedo, vec3<f32>(0.2126, 0.7152, 0.0722));
    // Saturation of the photographed cards about their own luma, exposed
    // separately from the field-wide grass saturation so the plates can be
    // matched to the procedural grass rather than moved with it.
    let photo_sat = clamp(grass.photo_mix.y, 0.0, 2.0);
    let photo_natural = mix(vec3<f32>(photo_luma), a3_albedo, photo_sat);
    // Contour contrast. The source plates are flatly lit by design, so pushing
    // contrast about their own mean is what separates individual stems from the
    // mass behind them. 1.0 leaves the photograph exactly as authored.
    //
    // The pivot is the LAYER'S OWN mean, run through the same correction chain
    // as the sample so the two are directly comparable. A constant pivot made
    // contrast darken every plate whose mean fell below it -- and darken it MORE
    // with distance, because mip averaging pulls each texel toward that mean, so
    // the near-field cancellation between bright stems and dark gaps disappears.
    let contrast = clamp(grass.photo.x, 0.5, 2.5);
    let mean_a3 = grass.layer_means[min(u32(in.blade_uv.z), 31u)].rgb *
        PHOTO_TONE_CORRECTION * photo_brightness * tone_gain;
    let mean_luma = dot(mean_a3, vec3<f32>(0.2126, 0.7152, 0.0722));
    let plate_pivot = mix(vec3<f32>(mean_luma), mean_a3, photo_sat);
    let shaped = max((photo_natural - plate_pivot) * contrast + plate_pivot, vec3<f32>(0.0));
    // Light does not reach the base of a clump. Occluding it there gives the
    // card a bottom, which is most of why an unoccluded plate floats.
    let root_ao = mix(1.0 - clamp(grass.photo.w, 0.0, 1.0), 1.0, smoothstep(0.0, 0.55, in.height_t));
    let albedo = grass.tint_photo.rgb *
        grass_saturation(dry_patch(shaped * variation * gust_highlight * root_ao, world.xz, in.height_t));
    // Photo cards are crossed billboards, so the base normal stays upright:
    // lighting the card's own facing made one clump brighten or darken as the
    // camera orbited it. The photograph's luma gradient then perturbs that
    // upright normal per texel, which is view-stable and gives the silhouette
    // real relief instead of one flat value across the whole plate.
    let n = tuft_contour_normal(in.blade_uv.xy, i32(in.blade_uv.z), in.normal);
    let light_dir = normalize(frame.sun_dir_world.xyz);
    let ndl = max(dot(n, light_dir), 0.0);
    let wrap = max((dot(n, light_dir) + 0.35) / 1.35, 0.0);
    let terrain_shadow = terrain_sun_shadow(world.xz, world.y);
    // Grass shadowing grass. The cascade already contains these same cards
    // (draw_shadow submits the near and mid tuft rings), so this is a real
    // occlusion test, not a fake.
    let clump_shadow = tuft_csm_shadow(in.world_rel, n);
    let cloud_lit = cloud_sun_shadow(world.xz);
    let direct = frame.sun_diffuse.rgb * mix(ndl, wrap, 0.5) *
        (1.0 - max(terrain_shadow, clump_shadow)) * cloud_lit;
    // Ambient keeps the softer half-upright normal: the contour detail belongs
    // in the sun term, and applying it to skylight only flattens it again.
    let view_dir = -in.world_rel / max(length(in.world_rel), 0.0001);
    let ambient = grass_sky_fill(normalize(mix(n, vec3<f32>(0.0, 1.0, 0.0), 0.45)), view_dir, light_dir) * sky_vis_ao(world.xz);
    let broad_field = clump_noise(world.xz, 0.005, 0x7a1c3e5du);
    let clump_normal = normalize(vec3<f32>((field_tint - 0.5) * 0.24, 1.0, (broad_field - 0.5) * 0.24));
    let half_direction = (light_dir + view_dir) / max(length(light_dir + view_dir), 0.0001);
    let sheen_amount = grass_broad_sun_response(dot(clump_normal, half_direction), height_detail,
        view_dir.y, GRASS_MEDIUM_LIGHTING);
    let sheen = frame.sun_diffuse.rgb * vec3<f32>(0.85, 1.0, 0.75) * sheen_amount *
        (1.0 - max(terrain_shadow, clump_shadow)) * cloud_lit;
    let lit = albedo * (ambient + direct) + sheen;
    let fogged = apply_fog(lit, in.world_rel);
    return select(lit, fogged, grass.render_flags.y >= 0.5);
}

// Hard-cutout card colour: the 1x (no MSAA) path, and the WGR_GRASS_CARD_A2C=0 A/B.
@fragment
fn fs_grass_mid_tuft(in: VsOut) -> @location(0) vec4<f32> {
    let tex = tuft_sample(in);
    if (tex.a < tuft_alpha_cutoff()) { discard; }
    return vec4<f32>(tuft_shade(in, tex), 1.0);
}

// A2C card colour: .a is the sharpened coverage the pipeline dithers across the
// MSAA samples. Coverage is computed BEFORE the discard (fwidth needs uniform
// control flow) and the discard only skips shading of fully uncovered fragments.
@fragment
fn fs_grass_mid_tuft_a2c(in: VsOut) -> @location(0) vec4<f32> {
    let tex = tuft_sample(in);
    let cov = tuft_coverage(tex.a);
    if (cov <= 0.0) { discard; }
    return vec4<f32>(tuft_shade(in, tex), cov);
}

@fragment
fn fs_grass_mid_tuft_prepass(in: VsOut) -> @location(0) vec2<f32> {
    if (tuft_sample(in).a < tuft_alpha_cutoff()) { discard; }
    let normal_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(normal_view));
}

// A2C prepass twin: the same coverage as fs_grass_mid_tuft_a2c in .a, so the
// prepass writes depth to exactly the samples the colour pass will shade. The
// Rg16Float normal attachment drops .b/.a on write; .a still drives coverage.
@fragment
fn fs_grass_mid_tuft_prepass_a2c(in: VsOut) -> @location(0) vec4<f32> {
    let cov = tuft_coverage(tuft_sample(in).a);
    if (cov <= 0.0) { discard; }
    let normal_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    let oct = oct_encode(normalize(normal_view));
    return vec4<f32>(oct.x, oct.y, 0.0, cov);
}

@fragment
fn fs_grass_prepass(in: VsOut) -> @location(0) vec2<f32> {
    let normal_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(normal_view));
}

// Fragment-census twins (see frag_counts). Same output as the entry they shadow,
// plus one atomic per fragment. `in.clip` is the window position in this stage.
@fragment
fn fs_grass_count_near(in: VsOut) -> @location(0) vec4<f32> {
    count_fragment(2u, in.clip);
    return grass_shade(in);
}

@fragment
fn fs_grass_count_mid(in: VsOut) -> @location(0) vec4<f32> {
    count_fragment(3u, in.clip);
    return grass_shade(in);
}

@fragment
fn fs_grass_prepass_count_near(in: VsOut) -> @location(0) vec2<f32> {
    count_fragment(0u, in.clip);
    let normal_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(normal_view));
}

@fragment
fn fs_grass_prepass_count_mid(in: VsOut) -> @location(0) vec2<f32> {
    count_fragment(1u, in.clip);
    let normal_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(normal_view));
}
