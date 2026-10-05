struct ShadowPass { light_vp: mat4x4<f32>, cam_pos: vec4<f32> };
struct TerrainParams {
    world_origin: vec2<f32>, land_grid: f32, terrain_grid: f32,
    hm_width: u32, hm_height: u32, land_range: u32, data_scale: f32,
    sea_level: f32, time: f32, swash_speed: f32, swash_amp: f32,
    wet_height: f32, wet_darken: f32, pad_a: f32, pad_b: f32,
};
struct GrassTrack { x: f32, z: f32, radius: f32, age: f32 };
struct GrassDownwash { x: f32, z: f32, radius: f32, strength: f32 };
struct GrassParams {
    density: f32, spacing: f32, near_radius: f32, enabled: f32,
    blade_height: f32, wind_strength: f32, wind_direction: f32, far_radius: f32,
    interactor_x: f32, interactor_z: f32, interactor_radius: f32, interactor_strength: f32,
    tracks: array<GrassTrack, 256>, downwash: array<GrassDownwash, 4>, debug_flags: vec4<f32>, render_flags: vec4<f32>,
    species_mix: vec4<f32>,
    // Layout must mirror grass.wgsl exactly -- same uniform buffer.
    look: vec4<f32>,
    shape_mix: vec4<f32>,
    cards: vec4<f32>,
    // .x = clump renderer, .y = brightness, .z/.w = mixed-photo coverage / patch size.
    renderer: vec4<f32>,
    // .x contrast, .y contour, .z self-shadow, .w root AO -- colour-pass only.
    photo: vec4<f32>,
    // .x/.y near+mid grid edges, .z LOD dissolve band, .w photo alpha cutoff.
    place: vec4<f32>,
    // .x = forced layer, .y = card saturation, .z = imprint lifetime, .w = depth.
    photo_mix: vec4<f32>,
    layer_weights: array<vec4<f32>, 2>,
    lod_density: vec4<f32>,
    tint_procedural: vec4<f32>,
    tint_photo: vec4<f32>,
    // .x card coverage, .y auto -- placement only. Then the procedural-blade
    // look block (colour pass only). Both must stay: same uniform buffer.
    card_look: vec4<f32>,
    blade_look: vec4<f32>,
    // .z = card wind flutter, read by vs_grass_tuft_shadow so the shadow sways
    // exactly as the colour card does. Mirrors grass.wgsl.
    card_tone: vec4<f32>,
    // These colour-only lanes still occupy the shared uniform buffer.
    native: vec4<f32>,
    native2: vec4<f32>,
    layer_bounds: array<vec4<f32>, 32>,
    layer_means: array<vec4<f32>, 32>,
    wind_advection: vec4<f32>,
    wind_flutter: vec4<f32>,
    blade_layer_means: array<vec4<f32>, 8>,
};
// Must mirror grass.wgsl's 32-byte layout exactly: both shaders bind the same
// instance buffer through the shared `data_layout`.
struct GrassInstance {
    pos_seed: vec4<f32>,
    packed: vec4<u32>,
};

@group(0) @binding(0) var<uniform> shadow: ShadowPass;
@group(1) @binding(0) var<uniform> terrain: TerrainParams;
@group(1) @binding(1) var heightmap: texture_2d<f32>;
@group(1) @binding(2) var geography: texture_2d<u32>;
@group(2) @binding(0) var<uniform> grass: GrassParams;
@group(2) @binding(1) var<storage, read_write> instances: array<GrassInstance>;
@group(2) @binding(2) var<storage, read_write> placement_count: array<atomic<u32>>;
// The photographed near clumps need the same cut-out silhouette in the depth
// map as in the colour pass. These bindings share grass/mod.rs's data layout.
@group(2) @binding(4) var blade_samp: sampler;
@group(2) @binding(5) var tuft_tex: texture_2d_array<f32>;
// Fragment census (WGR_GRASS_COUNT_FRAGMENTS); see frag_counts in grass.wgsl.
@group(2) @binding(6) var<storage, read_write> frag_counts: array<atomic<u32>>;

fn hash11(p: vec2<f32>) -> f32 { return fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453123); }
fn hash_u32(x_in: u32) -> u32 {
    var x = x_in;
    x = (x ^ (x >> 16u)) * 0x7feb352du;
    x = (x ^ (x >> 15u)) * 0x846ca68bu;
    return x ^ (x >> 16u);
}
fn hash_cell01(cell: vec2<i32>, salt: u32) -> f32 {
    let x = bitcast<u32>(cell.x);
    let z = bitcast<u32>(cell.y);
    let h = hash_u32(x * 0x9e3779b9u ^ z * 0x85ebca6bu ^ salt);
    return f32(h >> 8u) * (1.0 / 16777216.0);
}
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

// Mirrors grass.wgsl. A blade tapered by the LOD dissolve must cast the shadow
// of the size actually drawn, or the join reappears in the shadow map.
fn lod_height_scale(packed_z: u32) -> f32 {
    return mix(0.45, 1.0, unpack2x16unorm(packed_z).x);
}

fn native_blade_height_scale() -> f32 {
    return select(1.0, clamp(grass.native.x / 0.45, 0.15, 1.0), grass.native.x > 0.0);
}

// Mirrors pick_photo_layer in grass.wgsl: the cut-out shadow must use the same
// plate as the colour pass, or a forced or de-weighted family would still cast
// the silhouette of a different one.
// Mirrors photo_layer_bounds in grass.wgsl. The shadow card must be trimmed the
// same way as the colour card, or the cascade would carry a silhouette larger
// than the one drawn.
fn photo_layer_bounds(layer: u32) -> vec4<f32> {
    let b = grass.layer_bounds[min(layer, 31u)];
    if (b.y - b.x < 0.02 || b.w - b.z < 0.02) { return vec4<f32>(0.0, 1.0, 0.0, 1.0); }
    return b;
}

fn photo_layer_weight(index: u32) -> f32 {
    return max(grass.layer_weights[index / 4u][index % 4u], 0.0);
}

// Mirrors instance_layer_run / pick_photo_layer_run in grass.wgsl. The shadow
// must resolve to the SAME atlas layer as the colour pass, so the per-surface
// run is read from the instance here too rather than falling back to the global
// mix -- otherwise a clutter-driven surface would cast the silhouette of the
// legacy photo card.
fn instance_layer_run(packed_w: u32) -> vec2<u32> {
    return vec2<u32>((packed_w >> 3u) & 31u, (packed_w >> 8u) & 7u);
}

fn pick_photo_layer_run(patch_cell: vec2<f32>, run: vec2<u32>) -> u32 {
    let forced = grass.photo_mix.x;
    if (forced > -0.5) { return min(u32(forced + 0.5), 31u); }
    if (run.y > 0u) {
        // Mirrors grass.wgsl: patches outside the variety mix keep the run's
        // first class (single variety), so the shadow matches the colour card.
        if (hash11(patch_cell + vec2<f32>(19.0, 71.0)) >= clamp(grass.renderer.z, 0.0, 1.0)) {
            return min(run.x, 31u);
        }
        let pick = u32(hash11(patch_cell + vec2<f32>(43.0, 11.0)) * f32(run.y));
        return min(run.x + min(pick, run.y - 1u), 31u);
    }
    return pick_photo_layer(patch_cell);
}

fn pick_photo_layer(patch_cell: vec2<f32>) -> u32 {
    let forced = grass.photo_mix.x;
    if (forced > -0.5) { return min(u32(forced + 0.5), 31u); }
    if (hash11(patch_cell + vec2<f32>(19.0, 71.0)) >= clamp(grass.renderer.z, 0.0, 1.0)) {
        return 0u;
    }
    var total = 0.0;
    for (var i = 0u; i < 8u; i = i + 1u) { total = total + photo_layer_weight(i); }
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

// Mirrors grass.wgsl's world-space wind field. Shadow cascades now use the
// same moving gusts as the visible blades, instead of the former sine wave.
// Mirror of grass.wgsl's helper of the same name, reading the SAME uniform lane: the blade
// and the shadow it casts are two shaders over one wind field, and a scroll scale applied to
// only one of them makes a blade lean while its shadow leans elsewhere.
fn sample_wind_field(world_xz: vec2<f32>, height_t: f32, seed: f32) -> vec4<f32> {
    let strength = clamp(grass.wind_strength, 0.0, 3.0);
    let base_angle = grass.wind_direction * 0.01745329252;
    let base_direction = vec2<f32>(cos(base_angle), sin(base_angle));
    // Negated scroll: see the convention note in grass.wgsl. Both copies must
    // agree or shadows drift against the blades that cast them.
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
    let gust_pulse = pow(smoothstep(0.40, 0.84, gust_noise), 2.0);
    let local_gust = mix(0.18, 1.0, gust_pulse);
    let gust = select(local_gust, mix(0.55, local_gust, clamp(grass.native2.y, 0.0, 1.0)), grass.native2.y >= 0.0);
    let direction_angle = base_angle + (direction_noise - 0.5) * min(strength, 1.5) * 1.10;
    let flutter_scroll = grass.wind_flutter.xy;
    let flutter_noise = clump_noise(world_xz + flutter_scroll + base_direction * (height_t * height_t * 4.0) +
                                    vec2<f32>(seed * 19.0, seed * 31.0), 0.105, 0x3f5a91c7u);
    let turbulence = (flutter_noise - 0.5) * (0.035 + 0.085 * gust) * height_t;
    return vec4<f32>(cos(direction_angle), sin(direction_angle), gust, turbulence);
}

fn rotor_flutter_direction(world_xz: vec2<f32>, seed: f32) -> vec2<f32> {
    let time_a = vec2<f32>(terrain.time * 13.7, -terrain.time * 9.1);
    let time_b = vec2<f32>(-terrain.time * 7.3, terrain.time * 15.9);
    let a = clump_noise(world_xz * 0.65 + time_a + vec2<f32>(seed * 37.0, seed * 61.0), 0.38, 0x49c28e17u);
    let b = clump_noise(world_xz * 0.65 + time_b + vec2<f32>(seed * 71.0, seed * 23.0), 0.38, 0xb71d4a63u);
    let direction = vec2<f32>(a - 0.5, b - 0.5);
    return select(vec2<f32>(1.0, 0.0), normalize(direction), dot(direction, direction) > 0.0001);
}

@vertex
fn vs_grass_shadow(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> @builtin(position) vec4<f32> {
    let inst = instances[instance_index].pos_seed;
    let seed = inst.w;
    // Species shape must mirror grass.wgsl's species_shape_mixed(): a broad weed
    // leaf casts a broad shadow, and a flower stem must not taper to a point.
    // This file re-implements it rather than importing, so the two drift unless
    // edited together -- which is why the numbers below are the same literals.
    let species = instances[instance_index].packed.w & 7u;
    var legacy = vec3<f32>(1.0, 1.0, 0.65);
    if (species >= 6u) { legacy = vec3<f32>(0.85, 1.15, 0.18); }
    else if (species >= 4u) { legacy = vec3<f32>(1.9, 0.82, 0.42); }
    var varied = vec3<f32>(1.0, 1.0, 0.65);
    switch (species) {
        case 0u: { varied = vec3<f32>(0.74, 1.06, 0.34); }
        case 1u: { varied = vec3<f32>(1.00, 1.00, 0.26); }
        case 2u: { varied = vec3<f32>(1.34, 0.92, 0.20); }
        case 3u: { varied = vec3<f32>(0.62, 1.18, 0.46); }
        case 4u: { varied = vec3<f32>(1.90, 0.82, 0.22); }
        case 5u: { varied = vec3<f32>(1.52, 0.70, 0.15); }
        case 6u: { varied = vec3<f32>(0.85, 1.15, 0.12); }
        default: { varied = vec3<f32>(0.68, 1.32, 0.09); }
    }
    let shape = mix(legacy, varied, clamp(grass.shape_mix.x, 0.0, 1.0));
    // Flattening cached by cs_place. Without this the shadow pass rebuilt an
    // upright blade, so grass a player had walked flat kept casting a full
    // standing shadow.
    let inst_packed = instances[instance_index].packed;
    let crush_dir = unpack2x16snorm(inst_packed.x);
    let crush_data = unpack2x16unorm(inst_packed.y);
    let crush = crush_data.x;
    let cached_wash = crush_data.y;
    var rotor_wash = cached_wash;
    if (grass.interactor_strength > 1.001 && grass.interactor_radius > 0.01) {
        let delta = inst.xz - vec2<f32>(grass.interactor_x, grass.interactor_z);
        let live_wash = (1.0 - smoothstep(grass.interactor_radius * 0.18, grass.interactor_radius, length(delta))) * crush;
        rotor_wash = max(cached_wash, live_wash);
    }
    // Indexed, mirroring grass.wgsl: `vertex_index` is an index value, and a five-segment
    // blade has 12 distinct vertices rather than 30 draw slots. This shader, like the
    // colour one, derives everything from the row and the side -- `left` at the lateral
    // offset below is the only other use of `corner` -- so the duplicated quad corners
    // were recomputing identical positions.
    // SHADOW BLADE STRIDE (grass.card_look.w, renderer-owned: WGR_GRASS_SHADOW_STRIDE in
    // grass/mod.rs). The cascade pass is per-TRIANGLE bound -- it has no fragment shader
    // and still costs the same per triangle as the prepass -- and in the shadow map a
    // clump is a coverage stipple, not twelve resolvable blades. So the shadow draws every
    // `stride`-th blade of the clump the colour pass draws (blade 0, 2, 4, ... at stride
    // 2), with its exact seed, angle, height and bend, and widens each survivor by the
    // stride so the shadow COVERAGE the ground receives is preserved while the triangle
    // count falls by the stride. The index table (build_blade_indices(count / stride, 5)
    // in mod.rs) enumerates blades 0..count/stride; this maps them back onto the colour
    // clump's blade ids. Stride 1 is the old behaviour exactly.
    let stride = max(u32(grass.card_look.w + 0.5), 1u);
    let blade = (vertex_index / 12u) * stride;
    let blade_count = select(6.0, 12.0, grass.renderer.x > 0.5);

    let packed = vertex_index % 12u;
    let row = packed / 2u;
    let left = (packed % 2u) == 0u;
    let t = f32(row) / 5.0;
    let archetype = u32(seed * 4.0);
    let clump_height = select(select(select(0.60, 0.75, archetype == 1u), 0.52, archetype == 2u), 0.90, archetype == 3u);
    let clump_spread = select(select(select(0.10, 0.20, archetype == 1u), 0.28, archetype == 2u), 0.14, archetype == 3u);
    let clump_bend = select(select(select(0.70, 1.05, archetype == 1u), 1.45, archetype == 2u), 0.88, archetype == 3u);
    let blade_seed = hash11(inst.xz + vec2<f32>(f32(blade) * 17.0 + 3.0, f32(blade) * 29.0 + 11.0));
    let base_angle = seed * 6.2831853 + f32(blade) * (6.2831853 / blade_count) + (blade_seed - 0.5) * 0.55;
    let radial = vec3<f32>(cos(base_angle), 0.0, sin(base_angle));
    let axis = vec3<f32>(-sin(base_angle), 0.0, cos(base_angle));
    let base_offset = radial * (clump_spread * mix(0.18, 1.0, blade_seed));
    let height = mix(0.32, 0.81, hash11(inst.xz + vec2<f32>(f32(blade) * 7.0, f32(blade) * 13.0))) * grass.blade_height * shape.y * clump_height * lod_height_scale(inst_packed.z) * native_blade_height_scale();
    let bend_jitter = 1.0 + (hash11(inst.xz + vec2<f32>(f32(blade) * 23.0, 57.0)) * 2.0 - 1.0) * clamp(grass.shape_mix.z, 0.0, 1.0);
    let static_bend = radial * mix(0.10, 0.34, hash11(inst.xz + vec2<f32>(f32(blade) * 19.0, 31.0))) * bend_jitter * max(grass.cards.w, 0.0) * height * clump_bend;
    let depth = clamp(grass.photo_mix.w, 0.0, 0.95);
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (depth * crush);
    let crushed_height = height * (1.0 - depth * crush);
    let wind = sample_wind_field(inst.xz + base_offset.xz, t, seed + blade_seed);
    let wind_bend = vec3<f32>(wind.x, 0.0, wind.y) * grass.wind_strength *
        (0.035 + 0.21 * wind.z + wind.w);
    let flutter_dir = rotor_flutter_direction(inst.xz + base_offset.xz, seed + blade_seed);
    let crush_flutter = vec3<f32>(flutter_dir.x, 0.0, flutter_dir.y) * height * (0.95 * rotor_wash);
    let bend = (static_bend + wind_bend) * (1.0 - depth * crush) + crush_bend + crush_flutter;
    // species_mix.z mirrors the Grass-tab blade width multiplier, or a widened
    // blade would cast the shadow of a thin one.
    // Card widening and taper jitter mirror grass.wgsl: a shadow cast by the
    // un-widened blade would not match the silhouette actually drawn.
    let card_widen = mix(1.0, max(grass.cards.z, 1.0), grass.cards.x);
    let taper_jitter = 1.0 + (hash11(inst.xz + vec2<f32>(f32(blade) * 13.0, 71.0)) * 2.0 - 1.0) * clamp(grass.shape_mix.y, 0.0, 1.0);
    // `* f32(stride)`: the survivors stand in for the blades the stride skipped.
    let width = mix(0.016, 0.043, blade_seed) * shape.x *
        max(grass.species_mix.z, 0.05) * card_widen * f32(stride) *
        pow(max(1.0 - t, 0.0), max(shape.z * taper_jitter, 0.05));
    let lateral = select(axis * width, -axis * width, left);
    let world = inst.xyz + base_offset + lateral + vec3<f32>(0.0, crushed_height * t, 0.0) + bend * (t * t);
    return shadow.light_vp * vec4<f32>(world - shadow.cam_pos.xyz, 1.0);
}

// Fragment census twin of vs_grass_shadow (WGR_GRASS_COUNT_FRAGMENTS only; the
// production shadow pipeline has no fragment stage). Lane 4 of frag_counts, striped
// by x the same way grass.wgsl's count_fragment is.
@fragment
fn fs_grass_shadow_count(@builtin(position) pos: vec4<f32>) {
    atomicAdd(&frag_counts[4u * 32u + (u32(pos.x) & 31u)], 1u);
}

struct TuftShadowOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec3<f32>,
};

// Mirrors vs_grass_mid_tuft in grass.wgsl, but projects through the cascade
// matrix. Keeping the card geometry and UVs identical makes the alpha-tested
// shadow conform to the photographed A3 clump instead of the legacy ribbons.
@vertex
fn vs_grass_tuft_shadow(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> TuftShadowOut {
    let inst = instances[instance_index].pos_seed;
    let seed = inst.w;
    let inst_packed = instances[instance_index].packed;
    let field = clump_noise(inst.xz, 0.075, 0x48ac2f19u);
    let angle = mix(seed * 6.2831853, field * 6.2831853, grass.debug_flags.y);
    let card = vertex_index / 6u;
    let card_angle = angle + select(0.0, 1.5707963, card != 0u);
    let axis = vec3<f32>(cos(card_angle), 0.0, sin(card_angle));
    let corner = vertex_index % 6u;
    let right = corner == 1u || corner == 2u || corner == 4u;
    let top = corner == 2u || corner == 4u || corner == 5u;
    let quad_u = select(0.0, 1.0, right);
    let quad_v = select(0.0, 1.0, top);
    let patch_cell = floor(inst.xz / max(grass.renderer.w, 4.0));
    let layer = pick_photo_layer_run(patch_cell, instance_layer_run(inst_packed.w));
    let bounds = photo_layer_bounds(layer);
    let u = mix(bounds.x, bounds.y, quad_u);
    let img_v = mix(bounds.w, bounds.z, quad_v);
    let vh = 1.0 - img_v;

    let height_seed = mix(hash11(inst.xz + 13.0), clump_noise(inst.xz, 0.21, 0xa47f3cd1u), grass.debug_flags.y * 0.72);
    let family = u32(seed * 4.0);
    let family_height = select(select(select(0.78, 1.00, family == 1u), 1.24, family == 2u), 0.62, family == 3u);
    let family_width = select(select(select(0.72, 1.00, family == 1u), 1.20, family == 2u), 1.38, family == 3u);
    // Mirrors photo_card_scale() in grass.wgsl (card_look.z, 0 = stock).
    let card_scale = select(clamp(grass.card_look.z, 0.25, 2.0), 1.0, grass.card_look.z <= 0.0);
    let height = mix(0.34, 0.78, height_seed) * grass.blade_height * 1.7 * family_height *
        lod_height_scale(inst_packed.z) * card_scale;
    let half_width = height * mix(0.55, 0.85, hash11(inst.xz + 23.0)) * family_width;
    let crush_dir = unpack2x16snorm(inst_packed.x);
    let crush_data = unpack2x16unorm(inst_packed.y);
    let crush = crush_data.x;
    let cached_wash = crush_data.y;
    var rotor_wash = cached_wash;
    if (grass.interactor_strength > 1.001 && grass.interactor_radius > 0.01) {
        let delta = inst.xz - vec2<f32>(grass.interactor_x, grass.interactor_z);
        let live_wash = (1.0 - smoothstep(grass.interactor_radius * 0.18, grass.interactor_radius, length(delta))) * crush;
        rotor_wash = max(cached_wash, live_wash);
    }
    let card_depth = min(0.95, clamp(grass.photo_mix.w, 0.0, 0.95) * 1.45);
    let crushed_height = height * (1.0 - card_depth * crush);
    let crush_bend = vec3<f32>(crush_dir.x, 0.0, crush_dir.y) * height * (card_depth * 1.35 * crush);
    let wind = sample_wind_field(inst.xz, vh, seed);
    // Mirrors vs_grass_mid_tuft: gust sway plus card_tone.z of the turbulence
    // (renderer default 0). The shadow must lean exactly as the card it belongs
    // to, or grass-on-grass shadowing samples a card that is not there.
    let card_flutter = clamp(grass.card_tone.z, 0.0, 1.0);
    let wind_bend = vec3<f32>(wind.x, 0.0, wind.y) * grass.wind_strength *
        (0.030 + 0.18 * wind.z + wind.w * card_flutter);
    let flutter_dir = rotor_flutter_direction(inst.xz, seed);
    let crush_flutter = vec3<f32>(flutter_dir.x, 0.0, flutter_dir.y) * height * (0.95 * rotor_wash);
    let lean = (wind_bend * (1.0 - card_depth * crush) + crush_bend + crush_flutter) * (vh * vh);
    let lateral = axis * (u - 0.5) * 2.0 * half_width;
    let world = inst.xyz + lateral + vec3<f32>(0.0, crushed_height * vh, 0.0) + lean;
    let mirror = hash11(inst.xz + 5.0) < 0.5;
    let slice = mix(0.0, 0.18, hash11(inst.xz + 41.0)) * (bounds.y - bounds.x);
    var uu = clamp(mix(bounds.x + slice, bounds.y - slice, quad_u), bounds.x, bounds.y);
    if (mirror) { uu = bounds.x + bounds.y - uu; }
    var out: TuftShadowOut;
    out.clip = shadow.light_vp * vec4<f32>(world - shadow.cam_pos.xyz, 1.0);
    out.uv = vec3<f32>(uu, img_v, f32(layer));
    return out;
}

@fragment
fn fs_grass_tuft_shadow(in: TuftShadowOut) {
    // Same cutoff as the colour pass, or a trimmed card would still cast the
    // untrimmed silhouette.
    if (textureSampleBias(tuft_tex, blade_samp, in.uv.xy, i32(in.uv.z), 0.75).a <
        clamp(grass.place.w, 0.05, 0.95)) { discard; }
}
