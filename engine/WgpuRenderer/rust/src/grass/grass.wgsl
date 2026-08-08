#import frame::{frame, reverse_z, apply_fog, sky_irradiance, terrain_sun_shadow, sky_vis_ao, cloud_sun_shadow}
#import gbuffer::oct_encode

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
    tracks: array<GrassTrack, 96>,
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
@group(2) @binding(0) var<uniform> grass: GrassParams;
@group(2) @binding(1) var<storage, read_write> instances: array<GrassInstance>;
@group(2) @binding(2) var<storage, read_write> placement_count: array<atomic<u32>>;
// GRS-D blade albedo: one layer per archetype, mipped. Fragment stage only.
@group(2) @binding(3) var blade_tex: texture_2d_array<f32>;
@group(2) @binding(4) var blade_samp: sampler;
// GRS-E: the game's own photographed grass tuft, used by the mid LOD's crossed
// cards. Cutout alpha -- fs_grass_mid alpha-tests it.
@group(2) @binding(5) var tuft_tex: texture_2d<f32>;

const GRID_DIM: u32 = 512u;
const MAX_INSTANCES: u32 = 262144u;
const FAR_GRID_DIM: u32 = 384u;
const MAX_FAR_INSTANCES: u32 = 147456u;
const MID_GRID_DIM: u32 = 384u;
const MAX_MID_INSTANCES: u32 = 147456u;
const LAYERS: u32 = 8u;

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
const BLADE_SEGMENTS: u32 = 5u;
const VERTS_PER_CARD: u32 = BLADE_SEGMENTS * 6u;
const NEAR_CLUMP_BLADES: u32 = 6u;
const VERTS_PER_NEAR_BLADE: u32 = BLADE_SEGMENTS * 6u;
const VERTS_PER_NEAR_CLUMP: u32 = NEAR_CLUMP_BLADES * VERTS_PER_NEAR_BLADE;
// Mid LOD's unit is a clump: six independently varied three-segment ribbons.
// It deliberately uses opaque geometry rather than a photographed alpha card,
// preserving early-Z while keeping a volumetric silhouette at 25-160 m.
const MID_CLUMP_BLADES: u32 = 6u;
const MID_BLADE_SEGMENTS: u32 = 3u;
const VERTS_PER_MID_BLADE: u32 = MID_BLADE_SEGMENTS * 6u;
const VERTS_PER_MID_CLUMP: u32 = MID_CLUMP_BLADES * VERTS_PER_MID_BLADE;

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
    // washing the whole field. Raising `amount` widens the band that qualifies.
    // Not `patch`: that is a WGSL reserved keyword and fails composition.
    let patch_mask = smoothstep(1.0 - amount, 1.0 - amount * 0.30, field);
    let dry = patch_mask * (0.45 + 0.55 * height_t);
    // Straw keeps the source's luminance structure so blade detail survives.
    let luma = dot(colour, vec3<f32>(0.2126, 0.7152, 0.0722));
    let straw = vec3<f32>(0.74, 0.66, 0.32) * (0.55 + 1.35 * luma);
    return mix(colour, straw, dry);
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
fn mid_ring_end() -> f32 {
    let natural = max(grass.near_radius + 10.0, min(160.0, grass.near_radius * 2.5));
    return clamp(select(natural, grass.look.w, grass.look.w > grass.near_radius), grass.near_radius + 10.0, 160.0);
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
    let broad_scroll = -base_direction * terrain.time * (16.0 + strength * 9.0);
    let gust_scroll = -base_direction * terrain.time * (30.0 + strength * 15.0);
    let direction_noise = clump_noise(world_xz + broad_scroll, 0.006, 0x0d7e31a5u);
    let gust_noise = clump_noise(world_xz + gust_scroll, 0.022, 0xa12f7c59u);
    // There is always a small travelling sway. Stronger, soft-edged gusts
    // ride on top of it instead of leaving most of the field motionless,
    // which is the important visual distinction in the reference shader.
    let gust_pulse = pow(smoothstep(0.40, 0.84, gust_noise), 2.0);
    let gust = mix(0.18, 1.0, gust_pulse);
    let direction_angle = base_angle + (direction_noise - 0.5) * min(strength, 1.5) * 1.10;
    let flutter_scroll = gust_scroll * 2.1 + vec2<f32>(terrain.time * 3.7, -terrain.time * 2.9);
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
    // Recent contact stamps fade from strongly flattened to fully recovered
    // between 25 and 60 seconds, leaving a readable trail.
    for (var i = 0u; i < 96u; i = i + 1u) {
        let track = grass.tracks[i];
        if (track.radius <= 0.01 || track.age >= 60.0) { continue; }
        let delta = world_xz - vec2<f32>(track.x, track.z);
        let distance = length(delta);
        let imprint = (1.0 - smoothstep(track.radius * 0.20, track.radius, distance)) *
            (1.0 - smoothstep(25.0, 60.0, track.age));
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

fn pack_flatten(flatten: vec4<f32>, species: u32) -> vec4<u32> {
    return vec4<u32>(pack2x16snorm(clamp(flatten.xy, vec2<f32>(-1.0), vec2<f32>(1.0))),
                     pack2x16unorm(vec2<f32>(clamp(flatten.z, 0.0, 1.0), clamp(flatten.w, 0.0, 1.0))),
                     0u, species & 7u);
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
    if (gid.x >= GRID_DIM || gid.y >= GRID_DIM || grass.enabled < 0.5) { return; }
    // Six ribbons replace the old two crossed ribbons, so one clump covers
    // roughly three former cells and stays within the established vertex budget.
    let near_spacing = grass.spacing * 1.72;
    let half = f32(GRID_DIM) * 0.5;
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
    let coverage = grass.density * density_field(world_xz);
    if (dot(delta, delta) > grass.near_radius * grass.near_radius || seed > coverage) { return; }
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
    let y = sample_height(world_xz);
    if (y <= terrain.sea_level + 0.35) { return; }
    let normal = sample_normal(world_xz);
    if (normal.y < 0.70) { return; }
    let rel = vec3<f32>(world_xz.x, y, world_xz.y) - frame.cam_pos.xyz;
    let clip = frame.proj * frame.view * vec4<f32>(rel, 1.0);
    if (clip.w <= 0.0 || abs(clip.x) > clip.w * 1.15 || abs(clip.y) > clip.w * 1.15) { return; }
    let out_index = atomicAdd(&placement_count[0], 1u);
    if (out_index >= MAX_INSTANCES) { return; }
    instances[out_index].pos_seed = vec4<f32>(world_xz.x, y, world_xz.y, seed);
    instances[out_index].packed = pack_flatten(eval_flatten(world_xz), pick_species(world_xz));
}

// The middle ring uses a stable half-density grid and opaque multi-blade clumps.
// It bridges the detailed near clumps and distant coverage without any
// camera-facing tile swap or frame-to-frame randomisation.
@compute @workgroup_size(8, 8, 1)
fn cs_place_mid(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= MID_GRID_DIM || gid.y >= MID_GRID_DIM || grass.enabled < 0.5) { return; }
    let mid_end = mid_ring_end();
    if (mid_end <= grass.near_radius) { return; }
    let mid_spacing = max(grass.spacing * 2.0, mid_end / (f32(MID_GRID_DIM) * 0.5 - 1.0));
    let half = f32(MID_GRID_DIM) * 0.5;
    let snap = floor(frame.cam_pos.xz / mid_spacing) * mid_spacing;
    let cell = vec2<f32>(f32(gid.x) - half, f32(gid.y) - half);
    let cell_world = snap + cell * mid_spacing;
    let cell_id = vec2<i32>(floor(cell_world / mid_spacing));
    let seed = hash_cell01(cell_id, 0x37c4a51du);
    let jitter = (hash_cell2(cell_id, 0x9426d87bu) - vec2<f32>(0.5)) * (mid_spacing * 1.85);
    let world_xz = cell_world + jitter;
    let delta = world_xz - frame.cam_pos.xz;
    let distance2 = dot(delta, delta);
    let coverage = grass.density * 0.95 * density_field(world_xz);
    // A small overlap avoids a bare annulus at the detailed/mid and mid/far joins.
    let mid_start = max(0.0, grass.near_radius - mid_spacing * 1.5);
    if (distance2 < mid_start * mid_start || distance2 > mid_end * mid_end || seed > coverage) { return; }
    let map_max = terrain.world_origin + vec2<f32>(f32(terrain.hm_width - 1u), f32(terrain.hm_height - 1u)) * terrain.terrain_grid;
    if (any(world_xz < terrain.world_origin) || any(world_xz >= map_max)) { return; }
    let geocell = clamp(vec2<i32>(floor((world_xz - terrain.world_origin) / terrain.land_grid)), vec2<i32>(0), vec2<i32>(i32(terrain.land_range) - 1));
    let geo = textureLoad(geography, geocell, 0).x;
    if ((geo & 0x80000000u) == 0u || (geo & GEO_EXCLUDE_HARD) != 0u) { return; }
    if (grass.debug_flags.x < 0.5 && (geo & GEO_EXCLUDE_SOFT) != 0u) { return; }
    let y = sample_height(world_xz);
    if (y <= terrain.sea_level + 0.35 || sample_normal(world_xz).y < 0.70) { return; }
    let rel = vec3<f32>(world_xz.x, y, world_xz.y) - frame.cam_pos.xyz;
    let clip = frame.proj * frame.view * vec4<f32>(rel, 1.0);
    if (clip.w <= 0.0 || abs(clip.x) > clip.w * 1.15 || abs(clip.y) > clip.w * 1.15) { return; }
    let out_index = atomicAdd(&placement_count[0], 1u);
    if (out_index >= MAX_MID_INSTANCES) { return; }
    instances[out_index].pos_seed = vec4<f32>(world_xz.x, y, world_xz.y, seed);
    instances[out_index].packed = pack_flatten(eval_flatten(world_xz), pick_species(world_xz));
}

// Coarse outer ring: a terrain-conforming coverage field. This follows the
// reference project's distance-LOD principle without visible CPU tile
// relocation or hard mesh swaps at tile borders.
@compute @workgroup_size(8, 8, 1)
fn cs_place_far(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= FAR_GRID_DIM || gid.y >= FAR_GRID_DIM || grass.enabled < 0.5) { return; }
    // Keep the fixed 384x384 far candidate grid bounded even at the 5 km
    // developer radius: farther fields automatically use wider, cheaper cells.
    let far_spacing = max(max(grass.spacing * 4.0, 1.0), grass.far_radius / (f32(FAR_GRID_DIM) * 0.5 - 1.0));
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
    let far_coverage = grass.density * density_field(world_xz);
    if (distance2 <= near_start * near_start || distance2 > grass.far_radius * grass.far_radius || seed > far_coverage) { return; }
    let map_max = terrain.world_origin + vec2<f32>(f32(terrain.hm_width - 1u), f32(terrain.hm_height - 1u)) * terrain.terrain_grid;
    if (any(world_xz < terrain.world_origin) || any(world_xz >= map_max)) { return; }
    let geocell = clamp(vec2<i32>(floor((world_xz - terrain.world_origin) / terrain.land_grid)), vec2<i32>(0), vec2<i32>(i32(terrain.land_range) - 1));
    let geo = textureLoad(geography, geocell, 0).x;
    if ((geo & 0x80000000u) == 0u || (geo & GEO_EXCLUDE_HARD) != 0u) { return; }
    if (grass.debug_flags.x < 0.5 && (geo & GEO_EXCLUDE_SOFT) != 0u) { return; }
    let y = sample_height(world_xz);
    if (y <= terrain.sea_level + 0.35) { return; }
    let normal = sample_normal(world_xz);
    if (normal.y < 0.70) { return; }
    let rel = vec3<f32>(world_xz.x, y, world_xz.y) - frame.cam_pos.xyz;
    let clip = frame.proj * frame.view * vec4<f32>(rel, 1.0);
    if (clip.w <= 0.0 || abs(clip.x) > clip.w * 1.15 || abs(clip.y) > clip.w * 1.15) { return; }
    let out_index = atomicAdd(&placement_count[0], 1u);
    if (out_index >= MAX_FAR_INSTANCES) { return; }
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
    let blade = vertex_index / VERTS_PER_NEAR_BLADE;
    let packed = vertex_index % VERTS_PER_NEAR_BLADE;
    let segment = packed / 6u;
    let corner = packed % 6u;
    let upper = corner == 2u || corner == 4u || corner == 5u;
    let left = corner == 0u || corner == 3u || corner == 5u;
    let t = f32(segment + select(0u, 1u, upper)) / f32(BLADE_SEGMENTS);
    // Four clump archetypes: compact upright, open meadow, low splayed, tall wisp.
    let archetype = u32(seed * 4.0);
    // Most meadow clumps stay short; the occasional wisp preserves a layered
    // field without returning the whole near ring to the former tall profile.
    let clump_height = select(select(select(0.60, 0.75, archetype == 1u), 0.52, archetype == 2u), 0.90, archetype == 3u);
    let clump_spread = select(select(select(0.10, 0.20, archetype == 1u), 0.28, archetype == 2u), 0.14, archetype == 3u);
    let clump_bend = select(select(select(0.70, 1.05, archetype == 1u), 1.45, archetype == 2u), 0.88, archetype == 3u);
    let blade_seed = hash11(inst.xz + vec2<f32>(f32(blade) * 17.0 + 3.0, f32(blade) * 29.0 + 11.0));
    let base_angle = seed * 6.2831853 + f32(blade) * 1.0471976 + (blade_seed - 0.5) * 0.55;
    let radial = vec3<f32>(cos(base_angle), 0.0, sin(base_angle));
    let blade_axis = vec3<f32>(-sin(base_angle), 0.0, cos(base_angle));
    let base_offset = radial * (clump_spread * mix(0.18, 1.0, blade_seed));
    let height = mix(0.32, 0.81, hash11(inst.xz + vec2<f32>(f32(blade) * 7.0, f32(blade) * 13.0))) * grass.blade_height * shape.y * clump_height;
    let card_widen = mix(1.0, max(grass.cards.z, 1.0), grass.cards.x);
    let width = mix(0.016, 0.043, blade_seed) * shape.x * max(grass.species_mix.z, 0.05) * card_widen;
    let bend_jitter = 1.0 + (hash11(inst.xz + vec2<f32>(f32(blade) * 23.0, 57.0)) * 2.0 - 1.0) * clamp(grass.shape_mix.z, 0.0, 1.0);
    let static_bend = radial * mix(0.10, 0.34, hash11(inst.xz + vec2<f32>(f32(blade) * 19.0, 31.0))) * bend_jitter * max(grass.cards.w, 0.0) * height * clump_bend;
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (0.55 * crush);
    let crushed_height = height * (1.0 - 0.55 * crush);
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
    let standing_bend = (static_bend + wind_bend) * (1.0 - 0.55 * crush) + crush_flutter;
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
    let blade = vertex_index / VERTS_PER_MID_BLADE;
    let packed = vertex_index % VERTS_PER_MID_BLADE;
    let segment = packed / 6u;
    let corner = packed % 6u;
    let upper = corner == 2u || corner == 4u || corner == 5u;
    let left = corner == 0u || corner == 3u || corner == 5u;
    let t = f32(segment + select(0u, 1u, upper)) / f32(MID_BLADE_SEGMENTS);
    // Each member gets a stable radial base, orientation, height and bend. The
    // spacing remains at the clump centre, so interaction and placement stay
    // exactly as before while the rendered unit becomes a tuft of real blades.
    let blade_seed = hash11(inst.xz + vec2<f32>(f32(blade) * 17.0 + 3.0, f32(blade) * 29.0 + 11.0));
    let base_angle = seed * 6.2831853 + f32(blade) * 1.0471976 + (blade_seed - 0.5) * 0.55;
    let radial = vec3<f32>(cos(base_angle), 0.0, sin(base_angle));
    let blade_axis = vec3<f32>(-sin(base_angle), 0.0, cos(base_angle));
    let base_offset = radial * mix(0.025, 0.16, blade_seed);
    let height = mix(0.29, 0.71, hash11(inst.xz + vec2<f32>(f32(blade) * 7.0, f32(blade) * 13.0))) *
        grass.blade_height * shape.y;
    let width = mix(0.016, 0.038, blade_seed) * shape.x * max(grass.species_mix.z, 0.05);
    let static_bend = radial * mix(0.10, 0.32, hash11(inst.xz + vec2<f32>(f32(blade) * 19.0, 31.0))) * height;
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (0.55 * crush);
    let crushed_height = height * (1.0 - 0.55 * crush);
    let wind = sample_wind_field(inst.xz + base_offset.xz, t, seed + blade_seed);
    let wind_bend = vec3<f32>(wind.direction.x, 0.0, wind.direction.y) * grass.wind_strength *
        (0.030 + 0.18 * wind.gust + wind.turbulence);
    let flutter_dir = rotor_flutter_direction(inst.xz + base_offset.xz, seed + blade_seed);
    let crush_flutter = vec3<f32>(flutter_dir.x, 0.0, flutter_dir.y) * height * (0.95 * rotor_wash);
    let bend = (static_bend + wind_bend) * (1.0 - 0.55 * crush) + crush_flutter;
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
    let u = select(0.0, 1.0, right);
    let vh = select(0.0, 1.0, top);

    let height_seed = mix(hash11(inst.xz + 13.0), clump_noise(inst.xz, 0.21, 0xa47f3cd1u), grass.debug_flags.y * 0.72);
    // A tuft is a clump, so it is wider and taller than the single blade this
    // instance used to represent.
    let height = mix(0.34, 0.78, height_seed) * grass.blade_height * 1.7;
    let half_width = height * mix(0.55, 0.85, hash11(inst.xz + 23.0));

    let crush_dir = unpack2x16snorm(inst_packed.x);
    let crush_data = unpack2x16unorm(inst_packed.y);
    let crush = crush_data.x;
    let rotor_wash = active_rotor_wash(inst.xz, crush, crush_data.y);
    let crushed_height = height * (1.0 - 0.55 * crush);
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (0.55 * crush);

    let wind = sample_wind_field(inst.xz, vh, seed);
    let wind_bend = vec3<f32>(wind.direction.x, 0.0, wind.direction.y) * grass.wind_strength *
        (0.030 + 0.18 * wind.gust + wind.turbulence);
    let flutter_dir = rotor_flutter_direction(inst.xz, seed);
    let crush_flutter = vec3<f32>(flutter_dir.x, 0.0, flutter_dir.y) * height * (0.95 * rotor_wash);
    // The whole card leans; roots stay pinned. Quadratic in height, as the
    // procedural blades bend, so a clump does not shear against its neighbours.
    let lean = (wind_bend * (1.0 - 0.55 * crush) + crush_bend + crush_flutter) * (vh * vh);
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
    let mirror = hash11(inst.xz + 5.0) < 0.5;
    let slice = mix(0.0, 0.18, hash11(inst.xz + 41.0));
    var uu = mix(slice, 1.0 - slice, u);
    if (mirror) { uu = 1.0 - uu; }
    // Texture v runs 0 at the top of the tuft image, so flip the vertical param.
    // The source has its clump base on the bottom edge, so v maps straight.
    out.blade_uv = vec4<f32>(uu, 1.0 - vh, 0.0, 1.0);
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
    let cutout = textureSample(blade_tex, blade_samp, in.blade_uv.xy, i32(in.blade_uv.z));
    if (cutout.a < grass.cards.y) {
        discard;
    }
    return grass_shade(in);
}

@fragment
fn fs_grass(in: VsOut) -> @location(0) vec4<f32> {
    return grass_shade(in);
}

fn grass_shade(in: VsOut) -> vec4<f32> {
    let world = in.world_rel + frame.cam_pos.xyz;
    let base = vec3<f32>(0.055, 0.16, 0.025);
    let tip = vec3<f32>(0.32, 0.43, 0.10);
    let root = mix(0.30, 1.0, in.height_t * in.height_t);
    let field_tint = clump_noise(world.xz, 0.16, 0x5e3a91c7u);
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
    let blade = textureSample(blade_tex, blade_samp, in.blade_uv.xy, i32(in.blade_uv.z));
    // Alpha cut-out cards: the silhouette comes from the texture, not the quad,
    // which is what buys shape variety without more geometry. It costs the early-Z
    // the solid path enjoys, so it is a toggle and not the default -- measure
    // before switching it on for good.
    let textured = blade.rgb * variation * gust_highlight;
    // shape_mix.w scales the photo texture globally on top of the distance fade
    // already in blade_uv.w, so the detail can be dialled back without deleting
    // the atlas and falling all the way to procedural.
    let texture_amount = in.blade_uv.w * clamp(grass.shape_mix.w, 0.0, 1.0);
    let albedo = grass_saturation(dry_patch(mix(procedural, textured, texture_amount), world.xz, in.height_t));
    // Bend card normals toward an upright rounded-blade normal. This avoids
    // the flat dark-side look of a raw ribbon while preserving its silhouette.
    let n = normalize(mix(normalize(in.normal), vec3<f32>(0.0, 1.0, 0.0), 0.24));
    let light_dir = normalize(frame.sun_dir_world.xyz);
    let ndl = max(dot(n, light_dir), 0.0);
    let wrap = max((dot(n, light_dir) + 0.35) / 1.35, 0.0);
    let terrain_shadow = terrain_sun_shadow(world.xz, world.y);
    let cloud_lit = cloud_sun_shadow(world.xz);
    let direct = frame.sun_diffuse.rgb * mix(ndl, wrap, 0.35) * (1.0 - terrain_shadow) * cloud_lit;
    let ambient = sky_irradiance(normalize(mix(n, vec3<f32>(0.0, 1.0, 0.0), 0.45))) * sky_vis_ao(world.xz);
    let view_dir = normalize(-in.world_rel);
    let transmission = species_transmission(u32(in.blade_uv.z));
    let transmission_amount = pow(max(dot(view_dir, -light_dir), 0.0), 1.5) * (1.0 - ndl) *
        grass.debug_flags.w * transmission.w;
    let subsurface = transmission.rgb * transmission_amount * frame.sun_diffuse.rgb * (1.0 - terrain_shadow) * cloud_lit;
    let lit = albedo * (ambient + direct + subsurface);
    let fogged = apply_fog(lit, in.world_rel);
    return vec4<f32>(select(lit, fogged, grass.render_flags.y >= 0.5), 1.0);
}

@fragment
fn fs_grass_far(in: VsOut) -> @location(0) vec4<f32> {
    let world = in.world_rel + frame.cam_pos.xyz;
    let field_noise = clump_noise(world.xz, 0.11, 0x4f93d71bu);
    // Keep the proxy close to terrain colour: it supplies the distant grassy
    // field, not bright individual blades. The same aerial fog removes it
    // naturally at the horizon.
    let gust_highlight = 1.0 + in.wind_gust * clamp(grass.wind_strength, 0.0, 1.5) * 0.05;
    let albedo = grass_saturation(dry_patch(mix(vec3<f32>(0.075, 0.135, 0.028), vec3<f32>(0.19, 0.27, 0.055), field_noise) * gust_highlight, world.xz, 0.65));
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
const TUFT_ALPHA_CUTOFF: f32 = 0.5;

@fragment
fn fs_grass_mid_tuft(in: VsOut) -> @location(0) vec4<f32> {
    let tex = textureSample(tuft_tex, blade_samp, in.blade_uv.xy);
    if (tex.a < TUFT_ALPHA_CUTOFF) { discard; }
    let world = in.world_rel + frame.cam_pos.xyz;
    // The photo already carries base-to-tip shading, so only the field-scale
    // tint and gust highlight are applied on top -- re-adding the procedural
    // root darkening would double up what the photograph already shows.
    let field_tint = clump_noise(world.xz, 0.16, 0x5e3a91c7u);
    let variation = mix(1.0, mix(0.82, 1.16, mix(in.seed, field_tint, 0.55)), grass.debug_flags.z);
    let gust_highlight = 1.0 + in.wind_gust * clamp(grass.wind_strength, 0.0, 1.5) * 0.05;
    // The authored clump is already the right colour -- measured opaque mean
    // (0.525, 0.622, 0.127), green on every texel -- so its own albedo is used
    // directly. No palette substitution: that was only needed for the legacy
    // 2001 PAA fallback, whose hue is grey-teal. If that fallback is ever the
    // active texture the mid ring will look desaturated, which is why the
    // authored PNG is preferred at load time.
    let albedo = grass_saturation(dry_patch(tex.rgb * variation * gust_highlight, world.xz, in.height_t));
    let n = normalize(mix(normalize(in.normal), vec3<f32>(0.0, 1.0, 0.0), 0.35));
    let light_dir = normalize(frame.sun_dir_world.xyz);
    let ndl = max(dot(n, light_dir), 0.0);
    let wrap = max((dot(n, light_dir) + 0.35) / 1.35, 0.0);
    let terrain_shadow = terrain_sun_shadow(world.xz, world.y);
    let cloud_lit = cloud_sun_shadow(world.xz);
    let direct = frame.sun_diffuse.rgb * mix(ndl, wrap, 0.5) * (1.0 - terrain_shadow) * cloud_lit;
    let ambient = sky_irradiance(normalize(mix(n, vec3<f32>(0.0, 1.0, 0.0), 0.45))) * sky_vis_ao(world.xz);
    let lit = albedo * (ambient + direct);
    let fogged = apply_fog(lit, in.world_rel);
    return vec4<f32>(select(lit, fogged, grass.render_flags.y >= 0.5), 1.0);
}

@fragment
fn fs_grass_mid_tuft_prepass(in: VsOut) -> @location(0) vec2<f32> {
    if (textureSample(tuft_tex, blade_samp, in.blade_uv.xy).a < TUFT_ALPHA_CUTOFF) { discard; }
    let normal_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(normal_view));
}

@fragment
fn fs_grass_prepass(in: VsOut) -> @location(0) vec2<f32> {
    let normal_view = (frame.view * vec4<f32>(normalize(in.normal), 0.0)).xyz;
    return oct_encode(normalize(normal_view));
}
