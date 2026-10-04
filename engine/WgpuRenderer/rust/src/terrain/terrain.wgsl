// GPU terrain: a shared grid mesh instanced per node, heightmap-displaced in the
// vertex shader. Shares group 0 (the camera UBO + cascade shadow map) with the
// lit 3D pipeline, so terrain receives the same CSM shadows. The fragment shader
// blends the four surrounding land cells' detail-array layers (indexed by a
// per-cell index map) and modulates by a tiled high-frequency noise texture.

// Shares group(0) (the camera UBO + cascade shadow map) with the lit 3D
// pipeline via the frame module, so terrain receives the same CSM shadows and
// sun lighting.
#import frame::{frame, reverse_z, fog_factor, apply_fog_terrain, fog_sun_reach, sky_irradiance, sky_vis_ao, cloud_sun_shadow, sky_vis_debug_on, sky_vis_debug_value, gtao_ao, gtao_debug_on, gtao_bent_normal_world, gtao_debug_colour, interior_sky_ao, interior_sky_reach, interior_rain_reach, interior_rain_coverage, interior_sky_debug_on, interior_sky_ambient_normal, normal_map_cavity, grad_scale}
#import ground_puddles::{ground_puddle_mask, ground_puddle_land_factor, ground_puddle_rain_factor, ground_puddle_ripple_normal}
#import frame::ground_sky_reflection
#import frame::wet_soil_debug_mode
#import ground_wet::{cultivated_soil_fraction, soil_wet_amount, soil_wet_surface, soil_wet_fresnel, soil_wet_sun_specular, soil_wet_ground_reflection, soil_wet_environment_samples}
#import shadow::shadow_strength
#import lighting::lights_contrib
#import color::srgb_to_linear
#import gbuffer::oct_encode
#import snow_material::{snow_powder_albedo, snow_powder_normal, snow_surface_coverage}

struct TerrainParams {
    world_origin: vec2<f32>,
    land_grid: f32,
    terrain_grid: f32,
    hm_width: u32,
    hm_height: u32,
    land_range: u32,
    data_scale: f32,
    // Coast wet band (Stage 2c). sea_level + time (+ swash) move the damp intertidal line with
    // the water's edge; wet_height = m above the swash-moved sea level the band reaches;
    // wet_darken = albedo multiplier in the band (1 = off).
    sea_level: f32,
    time: f32,
    swash_speed: f32,
    swash_amp: f32,
    wet_height: f32,
    wet_darken: f32,
    // RFG-065: master weight for the NATIVE Enfusion ground material. 0 = the legacy
    // one-image-per-land-cell route, byte for byte. Never non-zero on a world whose
    // materials do not set `enfusion`, so this is inert everywhere but Reforger.
    enfusion_ground: f32,
    // Alpine snowline (dev weather tab): permanent cover above this height,
    // ramping over snowline_range. Negative height = off. Must stay in this
    // order after enfusion_ground: the C++/Rust ABI reads by offset.
    snowline_height: f32,
    snowline_range: f32,
    snowline_depth: f32,
    rain_wetness: f32, // cosmetic rain history [0,1], former padding at offset 72
    rain_strength: f32, // effective liquid rain, former padding at offset 76
    _pad3: f32,
    _pad4: f32,
};

// Must match GRID_N in terrain/mod.rs.
const GRID_N: f32 = 32.0;

@group(1) @binding(0) var<uniform> tp: TerrainParams;
@group(1) @binding(1) var heightmap: texture_2d<f32>;
// Long-distance terrain sun-shadow mask (terrain_shadow.wgsl sweep): world-aligned
// on the heightmap grid, .r = lit factor (1 = lit, 0 = fully shadowed). One
// bilinear tap gives terrain-on-terrain self-shadowing at any range; it composes
// with CSM by max() (most-occluded wins), and terrain is never a CSM caster so the
// two never double-shadow the same ground.
@group(1) @binding(2) var shadow_mask: texture_2d<f32>;
@group(1) @binding(3) var shadow_mask_samp: sampler;
struct SnowData {
    info: vec4<f32>, // world origin xz, texel metres, undisturbed depth
    deficits: array<f32>,
};
@group(1) @binding(4) var<storage, read> snow: SnowData;

// Sinkhole W1 (port of Malprave terrainHole2 to wgpu): terrain holes -- world X/Z convex areas a hole-cutting
// object (memory-LOD `terrain_hole*` selection + roadway LOD) has cut out of the ground. Written by
// Engine::SetTerrainHoles via wgr_terrain_set_holes. edges[i] = {nx, nz, d, last}: a point is inside an area when
// nx*x + nz*z + d >= 0 for every edge of that area; last = 1 closes an area. info.x = record count (0 = no holes). Optional {0,0,ceilingY,2} precedes a bounded polygon.
struct TerrainHoles {
    info: vec4<f32>,
    edges: array<vec4<f32>, 64>,
};
@group(1) @binding(5) var<storage, read> holes: TerrainHoles;

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

fn snow_depth(world_xz: vec2<f32>) -> f32 {
    if (snow.info.w <= 0.0) { return 0.0; }
    return max(0.0, snow.info.w * snow_drift(world_xz) - snow_deficit(world_xz));
}
// Fixed world-space drifts; never camera/time noise. Keep the maximum at
// the CPU depth so culling bounds remain conservative.
fn snow_drift(world_xz: vec2<f32>) -> f32 {
    return 0.90 + 0.05 * sin(dot(world_xz, vec2<f32>(0.37, 0.81)))
                + 0.05 * sin(dot(world_xz, vec2<f32>(1.73, -0.79)));
}
// Snow pressed away by tracks/rotors at this point (deficit grid around the
// camera, edge-feathered). Kept separate from the global depth so the altitude
// snowline below can share the same carving.
fn snow_deficit(world_xz: vec2<f32>) -> f32 {
    let t = (world_xz - snow.info.xy) / max(snow.info.z, 0.125) - vec2<f32>(0.5);
    if (any(t < vec2<f32>(0.0)) || any(t >= vec2<f32>(511.0))) { return 0.0; }
    let b = vec2<u32>(floor(t));
    let f = fract(t);
    let i = b.y * 512u + b.x;
    let deficit = mix(mix(snow.deficits[i], snow.deficits[i + 1u], f.x),
                      mix(snow.deficits[i + 512u], snow.deficits[i + 513u], f.x), f.y);
    // Detail LOD only: leaving this window never deletes the persistent CPU track.
    let edge = min(min(t.x, t.y), min(511.0 - t.x, 511.0 - t.y));
    return deficit * smoothstep(0.0, 24.0, edge);
}
// Alpine snowline (dev weather tab): permanent cover above snowline_height,
// ramping from first flakes to full cover over snowline_range. Negative
// height (or non-positive range) = off. This is WHERE snow lies, independent
// of the snowfall deposit above; the two combine by maximum. Shares the
// deposit drift so both snow families read as the same material.
fn snowline_depth(world_xz: vec2<f32>, world_y: f32) -> f32 {
    if (tp.snowline_height < 0.0 || tp.snowline_range <= 0.0) { return 0.0; }
    return tp.snowline_depth * snow_drift(world_xz)
        * smoothstep(tp.snowline_height, tp.snowline_height + tp.snowline_range, world_y);
}
// Total volumetric snow at a terrain point: snowfall deposit or altitude
// cover, whichever is deeper, minus track/rotor carving. Deposit-only scenes
// reduce to the old snow_depth formula exactly.
fn snow_cover(world_xz: vec2<f32>, world_y: f32) -> f32 {
    let line_depth = snowline_depth(world_xz, world_y);
    if (snow.info.w <= 0.0 && line_depth <= 0.0) { return 0.0; }
    return max(max(snow.info.w * snow_drift(world_xz), line_depth) - snow_deficit(world_xz), 0.0);
}
fn snow_gradient(xz: vec2<f32>, world_y: f32, footprint: f32) -> vec3<f32> {
    // Resolve the trail slope over at least one projected pixel, rather than
    // letting subpixel 12.5cm track walls alias at aerial distances.
    let h = max(0.125, footprint);
    let dx = (snow_cover(xz + vec2<f32>(h, 0.0), world_y) - snow_cover(xz - vec2<f32>(h, 0.0), world_y)) / (2.0 * h);
    let dz = (snow_cover(xz + vec2<f32>(0.0, h), world_y) - snow_cover(xz - vec2<f32>(0.0, h), world_y)) / (2.0 * h);
    return vec3<f32>(-dx, 0.0, -dz);
}

fn terrain_snow_normal(xz: vec2<f32>, world_y: f32, geometric: vec3<f32>,
    footprint: f32, dry: f32) -> vec3<f32> {
    let shape = normalize(geometric + snow_gradient(xz, world_y, footprint) * geometric.y * dry);
    let powder = snow_powder_normal(vec3<f32>(xz.x, world_y, xz.y), shape, footprint);
    return powder;
}
// Bindless ground textures: one texture_2d per Landscape texture index, native
// size/format/mips. Indexed non-uniformly per fragment (needs the device's
// SAMPLED_TEXTURE_..._NON_UNIFORM_INDEXING feature).
@group(2) @binding(0) var ground: binding_array<texture_2d<f32>>;
@group(2) @binding(1) var ground_samp: sampler;
@group(2) @binding(2) var index_map: texture_2d<u32>;
@group(2) @binding(3) var detail: texture_2d<f32>;
@group(2) @binding(4) var ground_clamp_samp: sampler;
@group(2) @binding(5) var jitter_map: texture_2d<f32>;
#import terrain_material::{TerrainUv, TerrainMaterial, TERRAIN_SURFACE_SLOTS}
@group(2) @binding(6) var<storage, read> terrain_materials: array<TerrainMaterial>;

// TW-WATER W3b — Tidewater Native's wet sand (bind group 3, see shore_wet.rs). In Tidewater mode
// the coast's wetness comes from the shore simulation's wet channel, and the foam the draining
// water strands on the sand from its residue channel (Tidewater App.js terrainWetness; the
// readers are ShoreSim.js's, ported in water_tw/tw_sim.wgsl, repeated here for this group).
// With Current OP `tww.p.x` is 0 and OP's own coast wet band below is unchanged.
struct TwWet {
    regions: array<vec4<f32>, 2>, // per shore-simulation region: min xz, size (m), weight
    p: vec4<f32>,                 // x = on, y = sea level
};
@group(3) @binding(0) var<uniform> tww: TwWet;
@group(3) @binding(1) var tw_sim_state: texture_2d_array<f32>;
@group(3) @binding(2) var tw_sim_lace: texture_2d<f32>;

const TW_SIM_RES: f32 = 768.0;
const TW_LACE_TILE: f32 = 3.5;
const TW_LACE_N: i32 = 512;

fn tw_sim_inside(uv: vec2<f32>) -> f32 {
    // (W3k: 15% of the region, ~57 m, instead of Tidewater's 2%. Tidewater's one region covers its
    // whole bay, so its border never crosses the surf; OP's regions follow the camera along the
    // coast, and a 7.6 m fade drew the region's square outline across the whitewater.)
    return smoothstep(0.0, 0.15, uv.x) * smoothstep(1.0, 0.85, uv.x) * smoothstep(0.0, 0.15, uv.y) * smoothstep(1.0, 0.85, uv.y);
}

// (layer, inside x weight) of the region the point is deepest inside
fn tw_sim_pick(xz: vec2<f32>) -> vec2<f32> {
    var best = vec2<f32>(0.0, 0.0);
    for (var r = 0; r < 2; r++) {
        let uv = (xz - tww.regions[r].xy) / max(tww.regions[r].z, 1e-3);
        var k = 0.0;
        if (tww.regions[r].w > 0.0 && uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0) {
            k = tw_sim_inside(uv) * tww.regions[r].w;
        }
        if (k > best.y + 1e-4) { best = vec2<f32>(f32(r), k); }
    }
    return best;
}

// bilinear state (foam, wetness, residue, flow) of region r at world xz, from 4 loads
fn tw_sim_state_at(xz: vec2<f32>, r: i32) -> vec4<f32> {
    let uv = (xz - tww.regions[r].xy) / max(tww.regions[r].z, 1e-3);
    let fp = clamp(uv * TW_SIM_RES - 0.5, vec2<f32>(0.0), vec2<f32>(TW_SIM_RES - 1.001));
    let i = vec2<i32>(floor(fp));
    let t = fract(fp);
    let a = textureLoad(tw_sim_state, i, r, 0);
    let b = textureLoad(tw_sim_state, i + vec2<i32>(1, 0), r, 0);
    let c = textureLoad(tw_sim_state, i + vec2<i32>(0, 1), r, 0);
    let d = textureLoad(tw_sim_state, i + vec2<i32>(1, 1), r, 0);
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

fn tw_lace_load(q: vec2<f32>) -> vec4<f32> {
    let fp = q / TW_LACE_TILE * f32(TW_LACE_N) - 0.5;
    let i = vec2<i32>(floor(fp));
    let t = fract(fp);
    let m = vec2<i32>(TW_LACE_N - 1);
    let a = textureLoad(tw_sim_lace, i & m, 0);
    let b = textureLoad(tw_sim_lace, (i + vec2<i32>(1, 0)) & m, 0);
    let c = textureLoad(tw_sim_lace, (i + vec2<i32>(0, 1)) & m, 0);
    let d = textureLoad(tw_sim_lace, (i + vec2<i32>(1, 1)) & m, 0);
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

// ShoreSim.js shoreSimSandFoam: foam left on the sand, 0..1 (fp: footprint in lace texels)
fn tw_sand_foam(xz: vec2<f32>, s: vec4<f32>, fp: f32) -> f32 {
    let r = s.z;
    var out = 0.0;
    if (r > 0.01) {
        let lace = tw_lace_load(xz + 11.3);
        let near = smoothstep(3.0, 1.2, fp);
        let keep = smoothstep(lace.w * 0.55, lace.w * 0.55 + 0.08, r);
        let lw = r * 0.1 + 0.06;
        let strand = (1.0 - smoothstep(lw, lw + 0.07, lace.x)) * (lace.z * 0.5 + 0.6);
        let lines = max(strand * smoothstep(0.02, 0.25, r) * keep, lace.y * keep * 0.8);
        out = mix(smoothstep(0.08, 0.6, r) * 0.12, lines, near) * 0.85;
    }
    return out;
}

// The inverse of color::srgb_to_linear, for the one place the ground path has to
// go back: the Enfusion middle tint below is applied in linear (it is a multiplier
// on radiance, RFG-071) while the ground blend stays in gamma until the fold at
// the end of fs_main. Piecewise, so it round-trips the toe exactly.
fn ground_linear_to_srgb(c: vec3<f32>) -> vec3<f32> {
    let lo = c * 12.92;
    let hi = 1.055 * pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3<f32>(0.0031308));
}

fn hm_load(ix: i32, iz: i32) -> f32 {
    let cx = clamp(ix, 0, i32(tp.hm_width) - 1);
    let cz = clamp(iz, 0, i32(tp.hm_height) - 1);
    return textureLoad(heightmap, vec2<i32>(cx, cz), 0).x;
}

// World height at a world-xz, matching Landscape::SurfaceY's per-cell triangle
// interpolation (not bilinear) so surface decals stay coplanar with the mesh.
fn mud_height_offset(world_xz: vec2<f32>) -> f32 {
    if (arrayLength(&snow.deficits) < 524292u) { return 0.0; }
    let origin = vec2<f32>(snow.deficits[262144u], snow.deficits[262145u]);
    let cell = snow.deficits[262146u];
    let limit = snow.deficits[262147u];
    if (limit <= 0.0 || cell != 0.125) { return 0.0; }
    let t = (world_xz - origin) / cell - vec2<f32>(0.5);
    if (any(t < vec2<f32>(0.0)) || any(t >= vec2<f32>(511.0))) { return 0.0; }
    let b = vec2<u32>(floor(t));
    let f = fract(t);
    let i = 262148u + b.y * 512u + b.x;
    let offset = mix(mix(snow.deficits[i], snow.deficits[i + 1u], f.x),
                     mix(snow.deficits[i + 512u], snow.deficits[i + 513u], f.x), f.y);
    let edge = min(min(t.x, t.y), min(511.0 - t.x, 511.0 - t.y));
    return clamp(offset, -limit, 0.0) * smoothstep(0.0, 24.0, edge);
}

// Third signed view: sand compaction and positive loose-grain rims.
fn sand_height_offset(world_xz: vec2<f32>) -> f32 {
    if (arrayLength(&snow.deficits) < 786440u) { return 0.0; }
    let origin = vec2<f32>(snow.deficits[524292u], snow.deficits[524293u]);
    let cell = snow.deficits[524294u];
    let limit = snow.deficits[524295u];
    if (limit <= 0.0 || cell != 0.125) { return 0.0; }
    let t = (world_xz - origin) / cell - vec2<f32>(0.5);
    if (any(t < vec2<f32>(0.0)) || any(t >= vec2<f32>(511.0))) { return 0.0; }
    let b = vec2<u32>(floor(t));
    let f = fract(t);
    let i = 524296u + b.y * 512u + b.x;
    let offset = mix(mix(snow.deficits[i], snow.deficits[i + 1u], f.x),
                     mix(snow.deficits[i + 512u], snow.deficits[i + 513u], f.x), f.y);
    let edge = min(min(t.x, t.y), min(511.0 - t.x, 511.0 - t.y));
    return clamp(offset, -limit, 0.02) * smoothstep(0.0, 24.0, edge);
}
fn sand_height_gradient(world_xz: vec2<f32>, footprint: f32) -> vec2<f32> {
    // A resolved pixel differentiates the same bilinear signed field as CPU
    // support. A compulsory two-cell stencil erased narrow boot impressions
    // even when a pixel covered only millimetres. Keep that filtered stencil
    // once the footprint reaches one cell, so distant relief cannot glitter.
    if (footprint >= 0.125) {
        let h = max(0.125, footprint);
        return vec2<f32>(sand_height_offset(world_xz + vec2<f32>(h, 0.0)) - sand_height_offset(world_xz - vec2<f32>(h, 0.0)),
            sand_height_offset(world_xz + vec2<f32>(0.0, h)) - sand_height_offset(world_xz - vec2<f32>(0.0, h))) / (2.0 * h);
    }
    if (arrayLength(&snow.deficits) < 786440u) { return vec2<f32>(0.0); }
    let origin = vec2<f32>(snow.deficits[524292u], snow.deficits[524293u]);
    let cell = snow.deficits[524294u];
    let limit = snow.deficits[524295u];
    if (limit <= 0.0 || cell != 0.125) { return vec2<f32>(0.0); }
    let t = (world_xz - origin) / cell - vec2<f32>(0.5);
    if (any(t < vec2<f32>(0.0)) || any(t >= vec2<f32>(511.0))) { return vec2<f32>(0.0); }
    let b = vec2<u32>(floor(t));
    let f = fract(t);
    let i = 524296u + b.y * 512u + b.x;
    let a = snow.deficits[i];
    let c = snow.deficits[i + 512u];
    let d = snow.deficits[i + 513u];
    let e = snow.deficits[i + 1u];
    let offset = mix(mix(a, e, f.x), mix(c, d, f.x), f.y);
    var gradient = vec2<f32>(mix(e - a, d - c, f.y), mix(c - a, d - e, f.x)) / cell;
    if (offset <= -limit || offset >= 0.02) { gradient = vec2<f32>(0.0); }
    let edge = min(min(t.x, t.y), min(511.0 - t.x, 511.0 - t.y));
    var edge_gradient = vec2<f32>(0.0);
    if (edge > 0.0 && edge < 24.0) {
        let q = edge / 24.0;
        let slope = 6.0 * q * (1.0 - q) / (24.0 * cell);
        if (edge == t.x) { edge_gradient.x = slope; }
        else if (edge == t.y) { edge_gradient.y = slope; }
        else if (edge == 511.0 - t.x) { edge_gradient.x = -slope; }
        else { edge_gradient.y = -slope; }
    }
    let resolved = gradient * smoothstep(0.0, 24.0, edge)
        + clamp(offset, -limit, 0.02) * edge_gradient;
    if (footprint <= 0.0625) { return resolved; }
    let h = 0.125;
    let filtered = vec2<f32>(sand_height_offset(world_xz + vec2<f32>(h, 0.0)) - sand_height_offset(world_xz - vec2<f32>(h, 0.0)),
        sand_height_offset(world_xz + vec2<f32>(0.0, h)) - sand_height_offset(world_xz - vec2<f32>(0.0, h))) / (2.0 * h);
    return mix(resolved, filtered, smoothstep(0.0625, 0.125, footprint));
}

fn mud_height_gradient(world_xz: vec2<f32>, footprint: f32) -> vec2<f32> {
    let h = max(0.125, footprint);
    return vec2<f32>(mud_height_offset(world_xz + vec2<f32>(h, 0.0)) - mud_height_offset(world_xz - vec2<f32>(h, 0.0)),
        mud_height_offset(world_xz + vec2<f32>(0.0, h)) - mud_height_offset(world_xz - vec2<f32>(0.0, h))) / (2.0 * h);
}

fn sample_height(world_xz: vec2<f32>) -> f32 {
    let t = (world_xz - tp.world_origin) / tp.terrain_grid;
    let base = floor(t);
    let ix = i32(base.x);
    let iz = i32(base.y);
    let f = t - base; // f.x = x within cell, f.y = z within cell
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + 1, iz);
    let y10 = hm_load(ix, iz + 1);
    let y11 = hm_load(ix + 1, iz + 1);
    if (f.x <= 1.0 - f.y)
    {
        return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x + mud_height_offset(world_xz) + sand_height_offset(world_xz);
    }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y + mud_height_offset(world_xz) + sand_height_offset(world_xz);
}

// Central-difference normal at a fixed heightmap step. Taken per-fragment (not
// per-vertex) so it depends only on world position, never on the patch's LOD or
// morph state — that independence is what keeps even terrain evenly lit. A
// mesh/morph-derived normal ramps with camera distance within each patch and
// banded the lighting into radial stripes under grazing sun.
fn sample_normal(world_xz: vec2<f32>, step: f32) -> vec3<f32> {
    let hx0 = sample_height(world_xz - vec2<f32>(step, 0.0));
    let hx1 = sample_height(world_xz + vec2<f32>(step, 0.0));
    let hz0 = sample_height(world_xz - vec2<f32>(0.0, step));
    let hz1 = sample_height(world_xz + vec2<f32>(0.0, step));
    return normalize(vec3<f32>(-(hx1 - hx0), 2.0 * step, -(hz1 - hz0)));
}

// Resolve small mud relief against the actual pixel footprint, preserving the
// broad terrain normal and fading subpixel impressions instead of shimmering.
struct GroundNormalPair {
    broad: vec3<f32>,
    fine: vec3<f32>,
};
fn sample_mud_normals(world_xz: vec2<f32>, step: f32, footprint: f32) -> GroundNormalPair {
    let base = sample_normal(world_xz, step);
    let delta = mud_height_gradient(world_xz, footprint) - mud_height_gradient(world_xz, step)
        + sand_height_gradient(world_xz, footprint) - sand_height_gradient(world_xz, step);
    let fine = normalize(vec3<f32>(base.x - delta.x * base.y, base.y, base.z - delta.y * base.y));
    return GroundNormalPair(base, fine);
}
fn sample_mud_normal(world_xz: vec2<f32>, step: f32, footprint: f32) -> vec3<f32> {
    return sample_mud_normals(world_xz, step, footprint).fine;
}

// Index-map entry for a land cell, clamped to the map (row = z, column = x;
// same orientation as the heightmap and Landscape::GetTexture(z, x)). Bits
// 0-14 = ground-array layer; bit 15 = clamped transition tile (GL33's
// ClampU|ClampV: the texture maps exactly once onto its cell, edges extended,
// instead of tiling).
const CELL_LAYER_MASK: u32 = 0x7fffu;
const CELL_CLAMPED: u32 = 0x8000u;

fn cell_entry(cell: vec2<i32>) -> u32 {
    let cx = clamp(cell.x, 0, i32(tp.land_range) - 1);
    let cz = clamp(cell.y, 0, i32(tp.land_range) - 1);
    return textureLoad(index_map, vec2<i32>(cx, cz), 0).x;
}

// One land cell's contribution to the ground blend. Simple (tileable) layers
// sample at the global one-period-per-cell UV, so where neighbouring cells
// share a layer the blend stays an exact no-op. Clamped transition tiles
// sample in their own cell's [0,1] frame through the edge-extending sampler:
// a neighbour's contribution near the shared border is then that tile's
// matching edge row, not its wrapped-around opposite edge (which both smeared
// the wrong ground type across the border and put the tile's own wrap seam on
// the cell edge). Explicit gradients keep mip selection continuous across the
// frame switch and keep the call legal in non-uniform control flow.
// WGR_TERRAIN_JITTER=0 removes the per-cell UV offset entirely. An ABLATION: the offset is a
// piecewise-BILINEAR warp of the tiling UV, and a bilinear field has a crease along every cell
// boundary -- which is a candidate for the square lattice reported on Desert Island's sand.
override jitter_scale: f32 = 1.0;

fn sample_cell(entry: u32, cell: vec2<f32>, tile_uv: vec2<f32>,
               ddx: vec2<f32>, ddy: vec2<f32>) -> vec3<f32> {
    let layer = entry & CELL_LAYER_MASK;
    if ((entry & CELL_CLAMPED) != 0u)
    {
        return textureSampleGrad(ground[layer], ground_clamp_samp, tile_uv - cell, ddx, ddy).rgb;
    }
    return textureSampleGrad(ground[layer], ground_samp, tile_uv, ddx, ddy).rgb;
}

fn terrain_uv(transform: TerrainUv, source_bit: u32, source_mask: u32,
              world_pos: vec3<f32>, tile_uv: vec2<f32>) -> vec2<f32> {
    let source = select(vec3<f32>(tile_uv, 0.0), world_pos, (source_mask & (1u << source_bit)) != 0u);
    return vec2<f32>(dot(source, transform.u.xyz) + transform.u.w,
                     dot(source, transform.v.xyz) + transform.v.w);
}

// OFP enhancement NOHQ uses X in alpha and Y in green, unlike the authored
// RGB whole-tile normal below. Reuse the slot, not its different decode/texgen.
fn sample_legacy_normal(material: TerrainMaterial, cell: vec2<f32>, tile_uv: vec2<f32>,
                        ddx: vec2<f32>, ddy: vec2<f32>, world_pos: vec3<f32>,
                        world_ddx: vec3<f32>, world_ddy: vec3<f32>, view_pos: vec3<f32>) -> vec3<f32> {
    if (material.surface_count != 0u || material.tile_normal == 0u || tile_normal_strength <= 0.0) {
        return vec3<f32>(0.0, 0.0, 1.0);
    }
    var texel: vec4<f32>;
    if ((material.legacy & CELL_CLAMPED) != 0u) {
        texel = textureSampleGrad(ground[material.tile_normal], ground_clamp_samp,
                                  tile_uv - cell, ddx, ddy);
    } else {
        texel = textureSampleGrad(ground[material.tile_normal], ground_samp, tile_uv, ddx, ddy);
    }
    let xy = vec2<f32>(texel.a, texel.g) * 2.0 - vec2<f32>(1.0);
    let z = sqrt(max(1.0 - dot(xy, xy), 1e-6));
    let original = normalize(vec3<f32>(xy * tile_normal_strength, z));
    if (material.enfusion != 0u || material.legacy_detail_normal == 0u || material.legacy_detail_scale <= 0.0) {
        return original;
    }
    let detail_weight = 1.0 - smoothstep(15.0, 35.0, length(view_pos));
    if (detail_weight > 0.0) {
        // Absolute world UVs avoid cell-edge or moving-camera phase discontinuities.
        // Mip gradients remove subpixel grains; the distant path pays no extra fetch.
        let scale = material.legacy_detail_scale;
        let detail = textureSampleGrad(ground[material.legacy_detail_normal], ground_samp,
            world_pos.xz * scale, world_ddx.xz * scale, world_ddy.xz * scale);
        let detail_xy = vec2<f32>(detail.a, detail.g) * 2.0 - vec2<f32>(1.0);
        let detail_z = sqrt(max(1.0 - dot(detail_xy, detail_xy), 1e-6));
        let slopes = xy / max(z, 0.01) + detail_weight * detail_xy / max(detail_z, 0.01);
        return normalize(vec3<f32>(slopes * tile_normal_strength, 1.0));
    }
    return original;
}

fn terrain_uv_gradient(transform: TerrainUv, source_bit: u32, source_mask: u32,
                       world_gradient: vec3<f32>, tile_gradient: vec2<f32>) -> vec2<f32> {
    let source = select(vec3<f32>(tile_gradient, 0.0), world_gradient, (source_mask & (1u << source_bit)) != 0u);
    return vec2<f32>(dot(source, transform.u.xyz), dot(source, transform.v.xyz));
}

// Both later families are authored as a satellite colour plus an LCA mask that
// SELECTS one of a stack of surfaces (Arma 2 / OA `TerrainX`: six slots; Arma 3
// `TerrainSNX`: five, plus a whole-tile normal map). The satellite retains its
// low-frequency tile variation; the mask controls the high-frequency surfaces.
// Legacy terrain descriptors have surface_count == 0 and take the original
// single-texture route exactly.
// Colour plus the tangent-space normal the authored surfaces carry. `normal_ts` is
// (0,0,1) -- flat -- for a legacy descriptor or when the material names no normal, so a
// caller can blend it unconditionally.
struct AuthoredCell {
    rgb: vec3<f32>,
    normal_ts: vec3<f32>,
    ambient_occlusion: f32,
    mud: f32,
};

// Tangent-space view direction for the terrain's world-aligned frame (T = +X, B = +Z,
// N = up). Terrain surface UVs are world-aligned by construction, so this needs no
// per-vertex tangent stream. `world_pos` is camera-relative, so the camera is at the
// origin and the view vector is simply -world_pos.
fn terrain_view_ts(world_pos: vec3<f32>) -> vec3<f32> {
    let v = normalize(-world_pos);
    // Floor the tangent-space Z HARD. The single-step offset below scales with xy/z,
    // and at a grazing view -- standing and looking out along the ground, which is
    // most of the time -- z goes to zero. A 1e-3 floor then multiplies the offset by
    // up to a thousand and samples the surface at essentially random coordinates, so
    // the near ground turns to mush and stops resolving detail as you walk toward it.
    // 0.3 caps the stretch at about 3x, which is the whole useful range of a one-step
    // offset anyway.
    //
    // This was latent until the detail fade started measuring camera distance
    // correctly (WLD-019): before that `parallax` was zero everywhere, so the
    // blowup never ran.
    return vec3<f32>(v.x, v.z, max(abs(v.y), 0.3));
}

// One authored surface: its colour, its tangent-space normal, and the parallax offset its
// own height asks for. `_nopx` packs all three -- Y in green, X in alpha, and the PX height
// in red, which `_nohq` leaves at zero (MAT-040). So the height costs no extra fetch beyond
// the normal, and the colour is then sampled at the displaced coordinate.
fn authored_surface(colour_tex: u32, normal_tex: u32, uv: vec2<f32>,
                    dwx: vec2<f32>, dwy: vec2<f32>, view_ts: vec3<f32>,
                    parallax: f32, out_normal: ptr<function, vec3<f32>>) -> vec3<f32> {
    var uv_shifted = uv;
    if (normal_tex != 0u) {
        let n_sample = textureSampleGrad(ground[normal_tex], ground_samp, uv, dwx, dwy);
        // Single-step parallax offset. A full occlusion march would multiply this
        // function's fetches by its step count on a surface that is already four-way
        // blended; one step recovers most of the depth cue at one fetch, and the fetch
        // was needed for the normal regardless.
        if (parallax > 0.0) {
            // Bounded as well as floored: a one-step offset is a depth CUE, so it may
            // never displace further than the depth it represents. Without this the
            // grazing case still smears even with the Z floor in place.
            let offset = (n_sample.r - 0.5) * parallax * view_ts.xy / view_ts.z;
            uv_shifted = uv + clamp(offset, vec2<f32>(-parallax), vec2<f32>(parallax));
        }
        let n_xy = vec2<f32>(n_sample.a, n_sample.g) * 2.0 - vec2<f32>(1.0);
        *out_normal = vec3<f32>(n_xy, sqrt(max(0.0, 1.0 - dot(n_xy, n_xy))));
    } else {
        *out_normal = vec3<f32>(0.0, 0.0, 1.0);
    }
    return textureSampleGrad(ground[colour_tex], ground_samp, uv_shifted, dwx, dwy).rgb;
}

// Reciprocal of an authored surface's own average colour, read from its own coarsest mip.
//
// A mip chain IS a box-average pyramid, so its smallest level is the texture's average --
// no CPU-side table, no per-material ABI field, and correct for ANY source albedo. That is
// the property the fixed x2 below was reaching for and only achieves on a source that
// happens to average 0.5.
//
// MEASURED CAVEAT: PAA chains bottom out at 4x4, not 1x1 (a 2048^2 `_co` carries 10 levels,
// 2048 -> 4), so one bilinear tap at the centre of the coarsest level is the mean of the
// four CENTRE texels of that 4x4 -- i.e. of the four quadrant averages -- not of all
// sixteen. On a tiling detail image those agree closely, and the residual is a constant per
// texture rather than a spatial artefact, because the tap coordinate is a constant.
//
// FLOORED AND CAPPED: a surface whose average is ~0 (a black or unbound source) must not
// become an unbounded multiplier. The cap binds below a mean of 1/16; the floor only stops
// the divide itself.
const SURFACE_GAIN_MAX: f32 = 16.0;
fn surface_gain(tex: u32) -> vec3<f32> {
    let level = f32(textureNumLevels(ground[tex]) - 1u);
    let mean = textureSampleLevel(ground[tex], ground_samp, vec2<f32>(0.5, 0.5), level).rgb;
    return min(vec3<f32>(1.0) / max(mean, vec3<f32>(1.0 / 255.0)), vec3<f32>(SURFACE_GAIN_MAX));
}

// --- Whole-tile normal (TerrainSNX Stage14, `n_<col>_<row>_no.paa`) ---------------
//
// CHANNEL CONVENTION, MEASURED on Stratis's own images, not assumed. `n_003_000_no.paa`
// is DXT1 with alpha min = max = 255 over the whole 1024x1024, so it CANNOT be the
// DXT5nm/`_nohq` packing the per-surface normals use (X in alpha, Y in green) -- that
// packing needs a live alpha channel and this one has none. Decoding `2*rgb-1` instead
// gives a unit vector: mean length 1.0002, sigma 0.0115 over 1M texels. So it is a plain
// tangent-space RGB = XYZ map with +Z out of the surface, and `authored_surface`'s
// alpha/green swizzle must NOT be copied here.
//
// WHAT IS IN IT is also measured, and it is not what the name suggests. It is a HIGH-PASS
// of the terrain: box-averaged to 16 m its tangent XY collapses to a mean slope of 0.009
// (0.5 deg) while the shipped 4 m heightfield's own slope over the same tiles averages
// 0.34, and its signed correlation with the heightfield gradient is ~0.02. At 1-4 m it
// carries real structure (|n_xy| mean 0.057-0.115, i.e. 3-7 deg, peaking near 0.6 on
// cliffs) whose AMPLITUDE tracks terrain steepness (magnitude correlation 0.40 against
// the heightfield gradient at the tile the filename names, and below 0.23 at every other
// tile and orientation -- which is also how the tile-to-world mapping was confirmed).
//
// So this is the band BETWEEN the heightfield and the per-surface `_nopx` maps: relief
// finer than the 4 m mesh can express and coarser than a detail texture. The macro shape
// is already gone from it, so it cannot double-count what `sample_normal` computes, and
// its 1-4 m features stay above one screen pixel out to kilometres -- long past where the
// per-surface normal band (150 -> 300 m) has faded to nothing.
//
// The TANGENT AXES come from the material's own satellite TexGen rather than a hard-coded
// guess: Stage14 declares `texGen 3`, the SAME TexGen as Stage0, on every Stratis material
// -- so the map is authored in the satellite's frame and needs no new transform. Reading
// that basis (aside/up/dir, transposed at load as LandSave does) gives u = 0.000946*X - 2.89
// and v = -0.000946*Z + 7.77, i.e. +U is world +X and +V is world -Z. Deriving the frame
// keeps this correct on a world whose satellite TexGen is signed differently instead of
// silently shearing its relief.
fn tile_normal_axis(row: vec4<f32>, world_source: bool, fallback: vec2<f32>) -> vec2<f32> {
    // A worldPos-sourced row multiplies (X, Y, Z), so its ground direction is (x, z).
    // A tex-sourced row multiplies (tile_u, tile_v, 0), and the tile UV is itself world
    // (X, Z) over the land grid, so its ground direction is (x, y).
    let axis = select(vec2<f32>(row.x, row.y), vec2<f32>(row.x, row.z), world_source);
    let len = length(axis);
    return select(fallback, axis / len, len > 1e-12);
}

fn sample_tile_normal(material: TerrainMaterial, uv: vec2<f32>,
                      dwx: vec2<f32>, dwy: vec2<f32>) -> vec3<f32> {
    // Arma 2 spends Stage14's slot on its sixth surface and Arma 1 has no such stage, so
    // `tile_normal` is 0 on most of the corpus. Returning a flat tangent normal here (not
    // a decoded black texel) is what makes the whole path a clean no-op on those worlds:
    // the whiteout blend below reproduces its other input exactly.
    if (tile_normal_strength <= 0.0 || material.tile_normal == 0u) {
        return vec3<f32>(0.0, 0.0, 1.0);
    }
    let texel = textureSampleGrad(ground[material.tile_normal], ground_samp, uv, dwx, dwy).rgb;
    let n = texel * 2.0 - vec3<f32>(1.0);
    let world_source = (material.uv_source_mask & 1u) != 0u;
    let u_axis = tile_normal_axis(material.satellite_uv.u, world_source, vec2<f32>(1.0, 0.0));
    let v_axis = tile_normal_axis(material.satellite_uv.v, world_source, vec2<f32>(0.0, 1.0));
    // Into the terrain's world-aligned tangent frame (T = +X, B = +Z), the same frame the
    // per-surface normals already use, so both layers compose without a tangent stream.
    let ground_xy = (n.x * u_axis + n.y * v_axis) * tile_normal_strength;
    return normalize(vec3<f32>(ground_xy, max(n.z, 1e-3)));
}

// Whiteout blend: add the tangent XY, multiply the Z. Chosen over replace-or-lerp for two
// reasons. It is EXACT at both identities -- a flat (0,0,1) on either side reproduces the
// other bit for bit, which is what lets the unauthored path and the far side of the
// per-surface normal fade cost nothing and change nothing -- and it keeps both layers'
// slopes instead of averaging them away, which a mix() toward one of them does precisely
// where both have something to say (a 4 deg tile ridge under a 30 deg surface grain).
fn blend_normal_ts(base: vec3<f32>, detail: vec3<f32>) -> vec3<f32> {
    return normalize(vec3<f32>(base.x + detail.x, base.y + detail.y, base.z * detail.z));
}

// Decode the LCA selector into one weight per surface slot.
//
// The mask is NOT a weight field. Measured over every shipped mask of both
// generations (67M texels on Stratis, 57M on Takistan): R, G and B hold only 0
// or 255 and are strictly one-hot -- 0.0000% of texels have two channels high --
// while alpha holds only 255, 128 or 0. It is an indexed surface SELECTOR, and
// the fractional coverage the ground needs comes from filtering it, not from the
// stored values.
//
// The index is BI's own "encoding tricks employed ... increased from four to
// six" (bohemia.net/en/blog/show-me-the-light):
//
//   alpha 255 -> the RGB one-hot picks slots 0..3 (none = slot 0, the base)
//   alpha 128 -> slot 4
//   alpha   0 -> slot 5
//
// Four surfaces in RGB plus two more addressed through alpha is exactly six.
// The correlation is exact across 284 tiles: alpha level 128 occurs only on
// tiles that bind a fifth surface (137/137 Takistan, 30/30 Stratis) and level 0
// only on tiles that bind a sixth (47/47) -- no counterexamples. Arma 3's
// TerrainSNX has five slots and correspondingly never authors level 0.
//
// Reading alpha as a fifth WEIGHT, which is what this did before, gave slot 4
// full coverage over the 90% of texels sitting at alpha 255 and left the base
// nothing. That is the washed-out ground with banding along the mask's hard
// edges, and on Arma 2 it was worse still: OA's masks are DXT1 with no alpha at
// all, so the decoder's constant 255 covered every tile completely.
//
// The three alpha levels are interpolated as a partition of unity, so a filtered
// tap between two authored levels crossfades instead of decoding as a level that
// was never painted. Every returned weight is non-negative and they sum to 1.
fn decode_mask(mask: vec4<f32>) -> array<f32, 6> {
    let a = mask.a;
    let group = clamp((a - 0.5) * 2.0, 0.0, 1.0);       // alpha 1.0 -> RGB slots
    let level_four = 1.0 - abs(a - 0.5) * 2.0;          // alpha 0.5 -> slot 4
    let level_five = clamp(1.0 - a * 2.0, 0.0, 1.0);    // alpha 0.0 -> slot 5
    // One-hot by construction, so the sum is the painted coverage and the
    // remainder is the base's. Clamped because a filtered tap across a seam can
    // overshoot by a rounding step.
    let painted = min(mask.r + mask.g + mask.b, 1.0);
    return array<f32, 6>(group * (1.0 - painted), group * mask.r, group * mask.g,
                         group * mask.b, level_four, level_five);
}

// Admission is a property of the SOURCE mask, not the subset of color textures
// currently bound. An unknown/unbound selected slot still occupies its source
// share, so dropping 80% rock cannot turn the remaining 20% mud into 100% mud.
fn source_mud_fraction(weights: array<f32, 6>, flags: u32) -> f32 {
    var selected = 0.0;
    var mud = 0.0;
    for (var i = 0u; i < TERRAIN_SURFACE_SLOTS; i = i + 1u) {
        selected += weights[i];
        if ((flags & (1u << (8u + i))) != 0u) {
            mud += weights[i];
        }
    }
    return clamp(mud / max(selected, 1e-6), 0.0, 1.0);
}

// `world_pos` is ABSOLUTE (the TexGens are authored in world space); `view_pos` is
// the same point camera-relative, and is the only thing distance and the view
// vector may be taken from. Passing the absolute position for both -- which is
// what this did -- makes `length()` the distance from the world origin, so on
// every real world the detail fade sat at zero from the first metre and the
// authored stack was never drawn at all. The only visible difference the flag
// made was satellite-instead-of-legacy, which is why turning it up measured as
// almost nothing.
fn sample_authored_cell(entry: u32, cell: vec2<f32>, tile_uv: vec2<f32>,
                        ddx: vec2<f32>, ddy: vec2<f32>, world_pos: vec3<f32>,
                        view_pos: vec3<f32>,
                        world_dwx: vec3<f32>, world_dwy: vec3<f32>) -> AuthoredCell {
    var material = terrain_materials[entry & CELL_LAYER_MASK];
    let legacy_mud = max(f32((material.puddle_flags & 2u) >> 1u),
        cultivated_soil_fraction(material.puddle_flags,
            fract((world_pos.xz - tp.world_origin) / tp.land_grid)));
    // The layer is a material-table lookup, but clamping belongs to the cell.
    material.legacy |= entry & CELL_CLAMPED;
    let authored_weight = clamp(a3_surface_blend, 0.0, 1.0);
    // --- RFG-065: the NATIVE Enfusion surface ----------------------------------------
    //
    // A Reforger `.emat` is not the Arma stack and must not be routed through it: it has
    // no LCA mask and no satellite tile, so `surface_count` is 0 and everything above
    // would early-out. What it DOES carry is the two things the legacy branch gets
    // wrong by a wide margin.
    //
    // 1. TILING. The legacy route maps one image onto each land cell, and a native
    //    Everon land cell is 12.5 m (12,800 m / a 1024-cell land grid). The corpus
    //    authors `ScaleUV` 2..8 m -- median 4, on 36 of the 51 shipped surfaces -- so
    //    the ground is drawn 12.5/ScaleUV times too coarse: 6.25x at ScaleUV 2, 3.1x at
    //    the median. It also parks the texture's period exactly on the land grid, so a
    //    cell IS a texture and the tiling reads as a lattice. The UV here is
    //    world-space, so it is continuous across cell borders and needs no jitter; the
    //    four-cell blend above still crossfades between SURFACES.
    //
    // 2. MIDDLE DISTANCE. Enfusion crossfades the detail map out over
    //    [DetailMaxDistance - DetailBlendDistance, DetailMaxDistance] and replaces it
    //    with `BCRMiddleMap` at `MiddleScaleUV` (20..150 m). At the authored 4 m tiling
    //    that fade is not optional decoration: it is what stops a 4 m period from
    //    becoming a lattice in the middle distance.
    //
    // NHO uses raw RG normal XY and A ambient occlusion. Height remains unused.
    // `SatMapBlend` is zero on 19 of the 37 that declare it
    // and needs Everon's 2,500 `_supertexture.edds` paged in, which is a build, not a
    // binding.
    if (material.enfusion != 0u && tp.enfusion_ground > 0.0) {
        let wxz = vec2<f32>(world_pos.x, world_pos.z);
        let duv = vec2<f32>(world_dwx.x, world_dwx.z);
        let dvv = vec2<f32>(world_dwy.x, world_dwy.z);
        let d_uv = wxz * material.detail_scale;
        var rgb = textureSampleGrad(ground[material.legacy], ground_samp, d_uv,
                                    duv * material.detail_scale, dvv * material.detail_scale).rgb;
        var normal_ts = vec3<f32>(0.0, 0.0, 1.0);
        var authored_ao = 1.0;
        let detail_end = max(material.detail_max, 1.0);
        let detail_far = smoothstep(max(detail_end - max(material.detail_fade, 1.0), 0.0),
                                    detail_end, length(view_pos));
        if (material.tile_normal != 0u && detail_far < 1.0) {
            let packed = textureSampleGrad(ground[material.tile_normal], ground_samp, d_uv,
                                          duv * material.detail_scale, dvv * material.detail_scale);
            let xy = packed.rg * 2.0 - vec2<f32>(1.0);
            // fs_main's terrain frame points T toward -X, B toward +Z.
            // Native UVs grow toward +X/+Z, unlike the legacy tile convention.
            let decoded = normalize(vec3<f32>(-xy.x, xy.y, sqrt(max(1.0 - dot(xy, xy), 1e-4))));
            normal_ts = normalize(mix(normal_ts, decoded, (1.0 - detail_far) * clamp(tp.enfusion_ground, 0.0, 1.0)));
#ifndef DISABLE_NATIVE_GROUND_AO
            // NHO alpha is linear ambient visibility, not coverage or normal X.
            // Reuse the normal tap and its detail band; no extra texture read.
            authored_ao = mix(1.0, clamp(packed.a, 0.0, 1.0),
                              (1.0 - detail_far) * clamp(tp.enfusion_ground, 0.0, 1.0));
#endif
        }
        if (material.middle != 0u && material.middle_blend > 0.0) {
            let m_uv = wxz * material.middle_scale;
            var mid = textureSampleGrad(ground[material.middle], ground_samp, m_uv,
                                        duv * material.middle_scale, dvv * material.middle_scale).rgb;
            // `MiddleColor` is a multiplier in LINEAR on the middle texel -- the rule
            // RFG-071 measured for `Color_N` against Enfusion's own bakes, and the one
            // the pairs in this corpus confirm: Dirt_01_Middle_BCR is a 216/216/216
            // brightness tile; times Dirt_01's MiddleColor (0.175, 0.137, 0.095) in
            // linear it lands on 97/86/73, beside Dirt_01_BCR's own measured mean of
            // 88/80/69, and Dirt_03's (190/185/181 x 0.262/0.23/0.192) on 103/94/85
            // against its detail's 102/96/89. Multiplied in gamma the same tile would
            // be 38/30/21, and left untinted it is the light grey the forest floor was
            // fading to past DetailMaxDistance. Applied here and not baked into the
            // tile because the tile is SHARED: one image, nine tints on Everon.
            // Skipped at 1,1,1 so the untinted materials pay no pow().
            if (any(material.middle_color < vec3<f32>(0.999))) {
                mid = ground_linear_to_srgb(srgb_to_linear(mid) * max(material.middle_color, vec3<f32>(0.0)));
            }
            // `detail_fade` is the WIDTH of the band, not its start: every surface in the
            // corpus has DetailBlendDistance <= DetailMaxDistance, and reading it as a
            // start would put Asphalt_01 (500/450) and Grass_03 (50/50) on opposite
            // sides of the same rule.
            let far = smoothstep(max(material.detail_max - material.detail_fade, 0.0),
                                 max(material.detail_max, 1.0), length(view_pos));
            rgb = mix(rgb, mid, far * clamp(material.middle_blend, 0.0, 1.0));
        }
        // Blended, not switched, so the dev panel's A/B is a crossfade and 0 is
        // BIT-EXACT the legacy sample below. Full on costs ONE fetch (two with a
        // middle map): the legacy tap is only taken while the slider is between.
        if (tp.enfusion_ground >= 0.999) {
            return AuthoredCell(rgb, normal_ts, authored_ao, legacy_mud);
        }
        let legacy_rgb = sample_cell(material.legacy, cell, tile_uv, ddx, ddy);
        return AuthoredCell(mix(legacy_rgb, rgb, clamp(tp.enfusion_ground, 0.0, 1.0)),
                            normal_ts, authored_ao, legacy_mud);
    }
    // The normal renderer must remain byte-for-byte on the established cell
    // path while the authored composition is evaluated and compared.
    // This also avoids applying an unavailable texgen to legacy descriptors.
    if (material.surface_count == 0u || authored_weight == 0.0) {
        if (material.legacy_satellite_generated == 1u && material.satellite != 0u) {
            let far = smoothstep(800.0, 2400.0, length(view_pos));
            if (far <= 0.0) {
                return AuthoredCell(sample_cell(material.legacy,cell,tile_uv,ddx,ddy),
                                    sample_legacy_normal(material,cell,tile_uv,ddx,ddy,world_pos,world_dwx,world_dwy,view_pos), 1.0, legacy_mud);
            }
            let inv_span = 1.0 / (tp.land_grid * f32(tp.land_range));
            let uv = (world_pos.xz - tp.world_origin) * inv_span;
            let satellite = textureSampleGrad(ground[material.satellite], ground_clamp_samp,
                uv, world_dwx.xz * inv_span, world_dwy.xz * inv_span).rgb;
            if (far >= 0.999) {
                return AuthoredCell(satellite, vec3<f32>(0.0,0.0,1.0), 1.0, legacy_mud);
            }
            let near = sample_cell(material.legacy,cell,tile_uv,ddx,ddy);
            let normal = sample_legacy_normal(material,cell,tile_uv,ddx,ddy,world_pos,world_dwx,world_dwy,view_pos);
            return AuthoredCell(mix(near,satellite,far),
                                normalize(mix(normal,vec3<f32>(0.0,0.0,1.0),far)), 1.0, legacy_mud);
        }
        return AuthoredCell(sample_cell(material.legacy, cell, tile_uv, ddx, ddy),
                            sample_legacy_normal(material,cell,tile_uv,ddx,ddy,world_pos,world_dwx,world_dwy,view_pos), 1.0, legacy_mud);
    }
    // RVMAT `worldPos` is the engine's own world vector, Y up -- the same axes
    // the renderer already carries. The swizzle that used to sit here was
    // compensating for the transposed TexGen read (see LandSave's uvForStage);
    // with the basis transposed at load, the source needs no permutation, and
    // permuting it now would put the height back into the planar UV.
    let material_pos = world_pos;
    let material_dwx = world_dwx;
    let material_dwy = world_dwy;
    let satellite_uv = terrain_uv(material.satellite_uv, 0u, material.uv_source_mask, material_pos, tile_uv);
    let mask_uv = terrain_uv(material.mask_uv, 1u, material.uv_source_mask, material_pos, tile_uv);
    let satellite_dwx = terrain_uv_gradient(material.satellite_uv, 0u, material.uv_source_mask, material_dwx, ddx);
    let satellite_dwy = terrain_uv_gradient(material.satellite_uv, 0u, material.uv_source_mask, material_dwy, ddy);
    let mask_dwx = terrain_uv_gradient(material.mask_uv, 1u, material.uv_source_mask, material_dwx, ddx);
    let mask_dwy = terrain_uv_gradient(material.mask_uv, 1u, material.uv_source_mask, material_dwy, ddy);
    let satellite = textureSampleGrad(ground[material.satellite], ground_samp,
                                      satellite_uv, satellite_dwx, satellite_dwy).rgb;
    // The whole-tile normal rides the SATELLITE's UV and gradients -- Stage14 names the
    // same TexGen as Stage0 -- so it is fetched here, beside the satellite and BEFORE the
    // detail-band early-out below. That placement is the point of the feature: past
    // `detail_fade_end` the ground is a bare satellite photo lit by the 4 m mesh normal
    // alone, and this is the only surface relief left out there. It costs one fetch on a
    // fragment that was already paying for one, and its own mip chain fades it out at the
    // range where 1 m features stop resolving, so it needs no distance band of its own.
    let tile_normal = sample_tile_normal(material, satellite_uv, satellite_dwx, satellite_dwy);
    // Work out the detail's strength BEFORE fetching any of it. Past the fade the gain is
    // 1 and every surface sample is multiplied away, so computing the stack there is pure
    // waste -- and in any landscape view most of the screen is past it. Bailing here is
    // exact, not an approximation: the returned colour is the satellite either way.
    //
    // WHERE THAT LINE BELONGS IS A MEASURED QUESTION, and 120 m was far too close.
    // The satellite is one texel per ~1.03 m on Stratis (its TexGen puts 992 usable
    // texels of a 1024 image across a 1024 m tile) and per ~2 m on the OA worlds, so
    // past the fade the ground is a photo being MAGNIFIED, not minified: at a ~6.4e-4
    // rad/pixel view one satellite texel covers ~13 screen pixels at 120 m and does not
    // reach one pixel until ~1.6 km. Everything between those two distances was ground
    // drawn below screen resolution with the authored surfaces -- 2.4 mm/texel images
    // whose own trilinear mip tracks the pixel footprint exactly -- switched off.
    let view_dist = length(view_pos);
    let detail_fade = 1.0 - smoothstep(detail_fade_start, detail_fade_end, view_dist);
    let blend = authored_weight * detail_fade;
    if (blend <= 0.001) {
        return AuthoredCell(satellite, tile_normal, 1.0, 0.0);
    }
    let mask = textureSampleGrad(ground[material.mask], ground_samp, mask_uv, mask_dwx, mask_dwy).rgba;
    // `var`, not `let`: the loop below indexes it with a non-constant slot.
    var weights = decode_mask(mask);
    let view_ts = terrain_view_ts(view_pos);
    // The per-surface NORMAL is on its own, SHORTER band than the colour, and that is the
    // whole reason extending the colour band is affordable. A surface's normal costs one
    // texture fetch per selected slot and buys micro-shading whose features go sub-pixel
    // long before the albedo does; the colour costs the same fetch and buys structure that
    // stays above screen resolution to kilometres. So the far band keeps the colour and
    // drops the normal, which holds the FAR fetch count at satellite + mask + colour
    // instead of growing it with the distance the band now covers.
    //
    // `normal_tex = 0` is the existing "this surface names no normal" path in
    // authored_surface, so switching it off here needs no second code path: it skips the
    // fetch, skips the parallax offset, and returns a flat tangent-space normal.
    let normal_fade = 1.0 - smoothstep(detail_normal_start, detail_normal_end, view_dist);
    let normal_blend = authored_weight * normal_fade;
    let want_normal = normal_blend > 0.001;
    // Parallax fades out with the NORMAL it is read from (`_nopx` red), and is off entirely
    // beyond it: displacing a surface whose normal is no longer fetched has nothing to read.
    let parallax = select(0.0, terrain_parallax * (1.0 - smoothstep(10.0, 45.0, view_dist)),
                          want_normal);
    // A slot the source left empty carries no surface, so its share is not spent:
    // the total is renormalised over the slots that resolved. Handing it to the
    // base instead would paint the base wherever a hole was selected, which is
    // the unbound-layer trap in its other direction.
    var surface = vec3<f32>(0.0);
    var normal_ts = vec3<f32>(0.0);
    var bound_weight = 0.0;
    for (var i = 0u; i < min(material.surface_count, TERRAIN_SURFACE_SLOTS); i = i + 1u) {
        let colour_tex = material.surfaces[i];
        if (colour_tex == 0u || weights[i] <= 0.0) {
            continue;
        }
        let slot_uv = terrain_uv(material.surface_uvs[i], 2u + i, material.uv_source_mask, material_pos, tile_uv);
        let slot_dwx = terrain_uv_gradient(material.surface_uvs[i], 2u + i, material.uv_source_mask, material_dwx, ddx);
        let slot_dwy = terrain_uv_gradient(material.surface_uvs[i], 2u + i, material.uv_source_mask, material_dwy, ddy);
        var slot_normal_ts = vec3<f32>(0.0, 0.0, 1.0);
        let slot_normal_tex = select(0u, material.surface_normals[i], want_normal);
        // The gain a slot enters with, applied PER SLOT and inside the weighted sum rather
        // than once to the blended result: each surface has its own average, so one shared
        // divisor would leave a brightness step wherever the mask crosses from a dark
        // surface to a light one -- exactly the boundary the LCA exists to draw.
        //
        // Blended, not switched, and 2.0 is the identity: `surface_normalise = 0` is
        // BIT-EXACT legacy, since scaling every addend of a sum by a power of two scales
        // the sum by it exactly. The fetch is inside the branch so the off path also costs
        // exactly what it costs today.
        var slot_gain = vec3<f32>(2.0);
        if (surface_normalise > 0.0) {
            slot_gain = mix(vec3<f32>(2.0), surface_gain(colour_tex), surface_normalise);
        }
        surface += authored_surface(colour_tex, slot_normal_tex, slot_uv,
                                    slot_dwx, slot_dwy, view_ts, parallax,
                                    &slot_normal_ts) * slot_gain * weights[i];
        normal_ts += slot_normal_ts * weights[i];
        bound_weight += weights[i];
    }
    if (bound_weight <= 0.0) {
        // Every slot the mask selected here is a hole. The satellite is still the
        // right answer for the colour, so fall through with no detail at all
        // rather than dividing by zero. The tile normal is a property of the TILE, not of
        // the slots the mask selected, so it still applies here.
        return AuthoredCell(sample_cell(material.legacy, cell, tile_uv, ddx, ddy),
                            tile_normal, 1.0, 0.0);
    }
    // `surface` already carries each slot's gain (the x2, or 1/its own average), applied
    // per slot inside the sum above. From here it IS the multiplier the satellite takes.
    surface = surface / bound_weight;
    normal_ts = normal_ts / bound_weight;
    // The satellite is the colour authority at EVERY range, and the surface stack
    // MODULATES it -- it does not replace it.
    //
    // Replacing it is what made close range worse than the legacy path even with
    // the mask decoded correctly (owner-reported, and visible as flat pale ground
    // that has lost the satellite's local colour): a surface's `_co` is a
    // high-frequency detail image with an essentially uniform average, so
    // substituting it for the satellite throws away every metre-scale colour cue
    // the terrain has and keeps only the grain.
    //
    // Real Virtuality composites the other way round, and three independent things
    // in the corpus say so. Arma 1 carries an explicit per-surface `_mco` MACRO
    // colour beside every `_nohq`/`_co` (measured: 9,598 of 9,598 stage groups are
    // exactly that triple) -- a low-frequency colour for the detail to multiply.
    // Arma 2 and 3 drop `_mco` and gain the satellite, which is the same role at
    // world scale. And this renderer already carries the convention for OFP's own
    // detail layer a few lines below: `rgb *= 2.0 * detail.a`.
    //
    // So the surface enters as a gain around mid-grey, and the distance fade now fades that
    // gain to 1.0 rather than fading the colour back to the satellite. Zero remains the
    // exact legacy fallback. The override is no longer capped at 0.35: that cap was a guard
    // rail around a mask read as weights.
    //
    // WHAT "AROUND MID-GREY" MUST MEAN, and what the x2 assumed it meant. x2 is neutral
    // only where the source averages 0.5. That is TRUE of OFP's own detail layer --
    // `data\detail_dx.paa` alpha means 0.5085, so `rgb *= 2.0 * detail.a` a few lines below
    // is neutral by construction and must not be touched -- and FALSE of the authored
    // surface stack, which measures 0.105 (`en_grass1_ca`) to 0.144 (`en_soil_ca`) on DayZ.
    // At those averages x2 is a 3.5x DARKENING, not a neutral gain, and it is measurable
    // from outside the terrain entirely: grass drawn over the ground it replaces reads
    // 3.46x brighter than that ground on Enoch and 1.38x on Stratis against 1.09x on stock
    // OFP, where the stack is inert. Grass colour is world-independent, so a mismatch that
    // moves with the world cannot be the grass.
    //
    // `surface_normalise` replaces the assumed 0.5 with each surface's measured own average
    // (see `surface_gain`). It defaults to 0 -- the shipped x2 -- because turning it on
    // changes the look of every authored world.
    //
    // Stage 0 is not a repeating close-detail texture: both families provide a
    // separate TexGen for their continuous, low-frequency satellite colour.
    // Sampling it through the old cell frame is what made a genuine layered
    // A3 material look like a tiled single texture.
    let gain = mix(vec3<f32>(1.0), surface, blend);
    // The normal follows its OWN (shorter) fade, so a surface whose normal is no longer
    // fetched contributes exactly a flat tangent-space normal and the caller's guard falls
    // back to the geometric one. At normal_blend = 0 this is (0,0,1) exactly, which is what
    // makes dropping the fetch above a no-op rather than a discontinuity.
    // The tile normal joins AFTER that fade, not inside it: the two layers live on
    // different bands and must not share a distance ramp.
    return AuthoredCell(satellite * gain,
                        blend_normal_ts(normalize(mix(vec3<f32>(0.0, 0.0, 1.0), normal_ts, normal_blend)),
                                        tile_normal), 1.0, source_mud_fraction(weights, material.puddle_flags));
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world_xz: vec2<f32>,  // absolute world-xz
    @location(1) fog: f32,             // 1 = keep colour, 0 = full fog
    @location(2) world_pos: vec3<f32>, // camera-relative
};

// Skirt drop, as a multiple of the patch's vertex spacing. Tunable via
// WGR_TERRAIN_SKIRT_K (0 = skirts flush with the surface, i.e. effectively off).
override skirt_k: f32 = 4.0;

@vertex
fn vs_terrain(
    @location(0) grid_in: vec3<f32>, // xy = unit grid position in [0,1]^2, z = skirt flag
    @location(1) origin: vec2<f32>,  // node world-xz origin
    @location(2) size: f32,          // node world size
    @location(3) lod: u32,
    @location(4) morph: vec2<f32>,   // (morph_start, morph_end) camera-distance band
) -> VsOut {
    let grid = grid_in.xy;
    let world_xz_fine = origin + grid * size;
    let height_fine = sample_height(world_xz_fine);
    let dist = length(vec3<f32>(world_xz_fine.x, height_fine, world_xz_fine.y) - frame.cam_pos.xyz);

    // Snap toward the coarser even lattice as the vertex nears morph_end, so the
    // edge meets the parent grid at the LOD switch without a crack.
    var morph_k = 0.0;
    if (morph.y > morph.x)
    {
        morph_k = clamp((dist - morph.x) / (morph.y - morph.x), 0.0, 1.0);
    }
    let gidx = grid * GRID_N;
    let grid_coarse = (round(gidx * 0.5) * 2.0) / GRID_N;
    let world_xz = origin + mix(grid, grid_coarse, morph_k) * size;

    let bare_height = sample_height(world_xz);
    let snow_height = snow_cover(world_xz, bare_height) * smoothstep(tp.sea_level, tp.sea_level + 0.25, bare_height);
    let height = bare_height + snow_height - grid_in.z * max((size / GRID_N) * skirt_k, snow.info.w);
    let world_rel = vec3<f32>(world_xz.x, height, world_xz.y) - frame.cam_pos.xyz;

    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(world_rel, 1.0));
    // Texture + normal use the same morphed world position the geometry is drawn
    // at, so the UV stays locked to the mesh and morphs smoothly with it. (Using
    // the un-morphed position instead decouples the UV from the screen-space
    // interpolation and compresses the tiling wherever the morph collapses
    // vertices -> broken tiling at LOD > 0.)
    out.world_xz = world_xz;
    out.world_pos = world_rel;

    out.fog = fog_factor(length(world_rel));
    return out;
}

// Half-width (in land-cell fractions) of the texture cross-fade band centred on
// each cell boundary. Land cells are large (~50 m), so a full-cell linear blend
// smears a wide muddy seam; narrowing it to a band near the boundary keeps cell
// interiors crisp. 0 -> hard edges (GL33-like); 0.5 -> full-cell blend.
override blend_width: f32 = 0.15;
// TerrainSNX's authored LCA base/overlay detail contribution. Kept opt-in while
// it is verified against the legacy cell fallback; it is capped in
// sample_authored_cell so the satellite base remains continuous.
// Parallax depth for the authored terrain surfaces, in UV units of the surface's own
// TexGen. Height comes from `_nopx` red (MAT-040); `_nohq` holds that channel at zero, so
// a surface without one simply does not displace. Tunable via WGR_TERRAIN_PARALLAX.
override terrain_parallax: f32 = 0.02;
override a3_surface_blend: f32 = 0.0;
// Distance band (metres from the CAMERA -- WLD-019 amendment) over which the authored
// surface stack's colour gain eases back to neutral and the ground becomes the bare
// satellite. Past `detail_fade_end` the fragment costs ONE ground fetch.
//
// These were 25 -> 120 and that is where the "low resolution at mid distance" report
// comes from: the satellite is ~1.03 m/texel (Stratis) to ~2 m/texel (OA), which at a
// typical ~6.4e-4 rad/pixel view is one texel per ~13 screen pixels at 120 m and does
// not reach one texel per pixel until ~1.6 km. So the old band switched the detail off
// an order of magnitude closer than the point where it stops adding anything, and the
// detail was ALREADY half gone by ~55 m.
//
// Tunable via WGR_TERRAIN_DETAIL_START / WGR_TERRAIN_DETAIL_END. The exact pre-change
// behaviour is WGR_TERRAIN_DETAIL_START=25 WGR_TERRAIN_DETAIL_END=120
// WGR_TERRAIN_DETAIL_NORMAL_START=25 WGR_TERRAIN_DETAIL_NORMAL_END=120.
override detail_fade_start: f32 = 600.0;
override detail_fade_end: f32 = 900.0;
// The per-surface NORMAL's own, shorter band (WGR_TERRAIN_DETAIL_NORMAL_START / _END).
// It is deliberately separate from the colour: the normal is the fetch that does NOT
// have to grow with the extended colour band, so the far band stays at satellite + mask
// + colour. Parallax rides this band too -- its height is `_nopx` red, i.e. the same
// fetch. Set _END equal to detail_fade_end to keep normals for the whole band (one more
// fetch per selected slot); set it low to buy frame time back.
override detail_normal_start: f32 = 150.0;
override detail_normal_end: f32 = 300.0;
// Gain on the whole-tile normal's tangent XY (WGR_TERRAIN_TILE_NORMAL). 1 = as authored,
// 0 = off and the fetch is compiled out, >1 exaggerates the relief. This is the A/B switch
// for the feature: it toggles on ONE binary, so a before/after pair cannot drift on
// anything but the tile normal.
override tile_normal_strength: f32 = 1.0;
// How much of the authored surface's gain comes from its OWN measured average instead of
// the assumed 0.5 behind the x2 (WGR_TERRAIN_SURFACE_NORMALISE). 0 = the shipped x2, exactly;
// 1 = `surface / mean(surface)`, neutral by construction for any source albedo; between =
// a lerp of the two gains, which is the tuning knob if the full correction reads too flat.
//
// DEFAULT 0 ON PURPOSE. This changes the ground of every authored world (Arma 2, Arma 3,
// DayZ) and nothing else, so the flip is a look decision, not a correctness one, and it is
// the owner's. To make it the default, change this line and the fallback in
// `terrain/mod.rs` to 1.0.
override surface_normalise: f32 = 0.0;
// HDR path (docs/hdr-pipeline-plan.md): 1 = decode ground albedo + sun/light/fog
// colours from sRGB to linear and drop the [0,1] radiance clamp. 0 = gamma-naive.
override linear: f32 = 0.0;

@fragment
fn fs_terrain(in: VsOut) -> @location(0) vec4<f32> {
    // The reflected pass keeps only terrain on/above the global water plane. Fragment
    // clipping is conservative for a displaced heightfield and avoids an oblique matrix.
    if (dot(frame.clip_plane.xyz, in.world_pos + frame.cam_pos.xyz) + frame.clip_plane.w < 0.0) {
        discard;
    }
    // Sinkhole W1: no ground where a hole-cutting object has cut it away. The prepass discards the same
    // pixels, so the colour pass's depth-equal test never sees a prepass depth for them.
    if (in_terrain_hole(in.world_pos + frame.cam_pos.xyz)) {
        discard;
    }
    // Receiver-plane derivatives must run in uniform control flow.
    let dwx = dpdx(in.world_pos);
    let dwy = dpdy(in.world_pos);
    let snow_footprint = max(length(dwx), length(dwy));
    let puddle_footprint = max(length(dwx.xz), length(dwy.xz));

    // Continuous land-cell position; the ground texture repeats once per cell,
    // so this (plus jitter) doubles as the tiling UV. Blend the four cells
    // around the sample by fractional distance to their centres — where
    // neighbours share a layer the blend is a no-op (seamless), elsewhere it
    // cross-fades the hard cell edge.
    let cell_pos = (in.world_xz - tp.world_origin) / tp.land_grid;
    // Random UV offset breaking up the per-cell tiling repetition: the jitter
    // map holds Landscape::_random's per-grid-point offset (texel (x,z) = grid
    // point (x,z)), so a bilinear tap at cell_pos + half a texel reproduces the
    // corner interpolation GL33 bakes into its vertex UVs. Geography zeroes and
    // smooths the field around non-simple cells, so clamped transition tiles
    // are never warped off their designed edges.
    let jdim = vec2<f32>(textureDimensions(jitter_map));
    // SMOOTH the interpolation, do not just interpolate.
    //
    // A hardware bilinear tap is C0 but not C1: its gradient is piecewise-constant and jumps
    // across every texel boundary. Here one texel IS one land cell, so a plain bilinear jitter
    // gives the tiling UV a CREASE along every cell edge -- and a crease in a texture warp reads
    // as a straight line. That is the square lattice reported on Desert Island's sand: visible
    // looking steeply down from close, gone at a flat angle or from height, and (measured) gone
    // entirely with WGR_TERRAIN_JITTER=0.
    //
    // Ruled out first, each with its own capture: the ground texture's own wrap seam (bt.paa's
    // opposite edges differ by 8.3/255 against an adjacent-pixel variation of 6.5, so it tiles),
    // and mip selection from the jittered UV (WGR_TERRAIN_MIP_FROM_TILE, no change).
    //
    // The fix is Perlin's: keep the same values at the grid points -- which is what preserves
    // the GL33 vertex-UV parity this field exists for -- and interpolate with a smoothstep
    // weight, whose derivative is zero at both ends, so neighbouring cells meet with matching
    // gradients and there is no crease to see.
    let jcoord = cell_pos;
    let jbase = floor(jcoord);
    let jf = jcoord - jbase;
    let jw = jf * jf * (3.0 - 2.0 * jf);
    let ji = vec2<i32>(jbase);
    let jmax = vec2<i32>(jdim) - vec2<i32>(1, 1);
    let j00 = textureLoad(jitter_map, clamp(ji, vec2<i32>(0), jmax), 0).xy;
    let j10 = textureLoad(jitter_map, clamp(ji + vec2<i32>(1, 0), vec2<i32>(0), jmax), 0).xy;
    let j01 = textureLoad(jitter_map, clamp(ji + vec2<i32>(0, 1), vec2<i32>(0), jmax), 0).xy;
    let j11 = textureLoad(jitter_map, clamp(ji + vec2<i32>(1, 1), vec2<i32>(0), jmax), 0).xy;
    let jitter = mix(mix(j00, j10, jw.x), mix(j01, j11, jw.x), jw.y);
    let tile_uv = cell_pos + jitter * jitter_scale;
    // Gradients of the shared tiling UV, for every ground tap (see sample_cell).
    // REN-TEMP-001D: ground taps use textureSampleGrad, which ignores any sampler/mip bias,
    // so the render-scale correction multiplies the TEXTURE gradients instead (2^bias).
    // The raw dwx/dwy above stay unscaled — the shadow receiver-plane math they also feed
    // describes geometry, not texel density.
    let gscale = grad_scale();
    let duvdx = dpdx(tile_uv) * gscale;
    let duvdy = dpdy(tile_uv) * gscale;
    let dwx_tex = dwx * gscale;
    let dwy_tex = dwy * gscale;
    let cc = cell_pos - vec2<f32>(0.5);
    let base = floor(cc);
    let f = cc - base;
    let bi = vec2<i32>(i32(base.x), i32(base.y));
    let e00 = cell_entry(bi + vec2<i32>(0, 0));
    let e10 = cell_entry(bi + vec2<i32>(1, 0));
    let e01 = cell_entry(bi + vec2<i32>(0, 1));
    let e11 = cell_entry(bi + vec2<i32>(1, 1));

    // Sharpen the fraction so the cross-fade concentrates in a band around the
    // cell boundary (f = 0.5) rather than ramping across the whole cell. Endpoints
    // stay 0/1, so neighbouring samples remain seamless.
    let bw = clamp(blend_width, 0.001, 0.5);
    let fx = smoothstep(0.5 - bw, 0.5 + bw, f.x);
    let fy = smoothstep(0.5 - bw, 0.5 + bw, f.y);
    let w00 = (1.0 - fx) * (1.0 - fy);
    let w10 = fx * (1.0 - fy);
    let w01 = (1.0 - fx) * fy;
    let w11 = fx * fy;
    let absolute_world_pos = in.world_pos + frame.cam_pos.xyz;
    var rgb = vec3<f32>(0.0);
    var authored_normal_ts = vec3<f32>(0.0, 0.0, 1.0);
    var authored_ao = 1.0;
    var mud = 0.0;
    // The four-cell cross-fade exists to hide the LEGACY per-cell texture's hard cell
    // edge. When all four cells name the same entry it changes nothing -- and it is
    // the same entry over the whole interior of every cell, which is nearly the whole
    // screen -- so evaluating one tap there is EXACTLY equal, not an approximation.
    //
    // That equality is worth a lot now the authored composition is live: a legacy cell
    // is one texture fetch, but an authored one is a mask, a satellite and up to six
    // surfaces with a colour and a normal each, so the four taps went from 4 fetches to
    // as many as 56 per terrain fragment -- and terrain is most of the frame. This is
    // the optimisation with no visual cost, by construction.
    //
    // The exception is a CLAMPED transition tile: there `sample_cell` samples in the
    // cell's own [0,1] frame, so the four taps genuinely differ even for one entry.
    // Those keep the full blend.
    let uniform_cell = e00 == e10 && e00 == e01 && e00 == e11 && (e00 & CELL_CLAMPED) == 0u;
    if (uniform_cell) {
        let c = sample_authored_cell(e00, base, tile_uv, duvdx, duvdy, absolute_world_pos, in.world_pos, dwx_tex, dwy_tex);
        rgb = c.rgb;
        authored_normal_ts = c.normal_ts;
        authored_ao = c.ambient_occlusion;
        mud = c.mud;
    } else {
        let c00 = sample_authored_cell(e00, base, tile_uv, duvdx, duvdy, absolute_world_pos, in.world_pos, dwx_tex, dwy_tex);
        let c10 = sample_authored_cell(e10, base + vec2<f32>(1.0, 0.0), tile_uv, duvdx, duvdy, absolute_world_pos, in.world_pos, dwx_tex, dwy_tex);
        let c01 = sample_authored_cell(e01, base + vec2<f32>(0.0, 1.0), tile_uv, duvdx, duvdy, absolute_world_pos, in.world_pos, dwx_tex, dwy_tex);
        let c11 = sample_authored_cell(e11, base + vec2<f32>(1.0, 1.0), tile_uv, duvdx, duvdy, absolute_world_pos, in.world_pos, dwx_tex, dwy_tex);
        rgb = w00 * c00.rgb + w10 * c10.rgb + w01 * c01.rgb + w11 * c11.rgb;
        // The authored surface normal, blended across the same four cells as the colour so it
        // stays continuous over a cell boundary.
        authored_normal_ts = w00 * c00.normal_ts + w10 * c10.normal_ts
                           + w01 * c01.normal_ts + w11 * c11.normal_ts;
        authored_ao = w00 * c00.ambient_occlusion + w10 * c10.ambient_occlusion
                    + w01 * c01.ambient_occlusion + w11 * c11.ambient_occlusion;
        mud = w00 * c00.mud + w10 * c10.mud + w01 * c01.mud + w11 * c11.mud;
    }

    // High-frequency detail noise: alpha modulates around neutral (matches GL33's
    // r0.rgb *= t1.a * 2.0, detail UV = base UV * 32).
    let detail_a = textureSampleBias(detail, ground_samp, tile_uv * 32.0, frame.renscale.x).a;
    rgb *= detail_a * 2.0;
    // HDR path: decode the gamma-space ground albedo (blend + detail done in gamma,
    // the §5 pragmatic fold) to linear before lighting.
    if (linear > 0.5) {
        rgb = srgb_to_linear(rgb);
    }

    // Per-pixel normal at a fixed heightmap step (independent of patch LOD/morph).
    let ground_normals = sample_mud_normals(in.world_xz, tp.terrain_grid, snow_footprint);
    let geometric_n = ground_normals.fine;
    // Reuse the pre-existing broad normal: no additional height samples.
    // Keep actual horizon/pixel filtering outside material/admission branches.
    let moist_horizon = dot(reflect(-normalize(-in.world_pos), geometric_n), ground_normals.broad);
    let moist_horizon_width = fwidth(moist_horizon);
    let moist_normal_variation = length(fwidth(geometric_n));
    // Bound the cone's horizon variation with derivatives in uniform flow.
    // No derivative operation enters the wet/source-dependent sample loop.
    let moist_cone_horizon_width = max(moist_horizon_width,
        2.0*moist_normal_variation + length(fwidth(normalize(-in.world_pos)))
        + length(fwidth(ground_normals.broad)));
    // Apply the authored surfaces' own normal on top of the heightfield's. The terrain
    // frame is world-aligned by construction -- T = +X, B = +Z -- so this needs no tangent
    // stream, and a flat (0,0,1) from an unauthored cell leaves the geometric normal
    // exactly as it was.
    var n = geometric_n;
    // Only build a frame when there is actually a perturbation to apply. Guarding on the
    // tangent components rather than doing the maths and multiplying by zero is not an
    // optimisation: cross((0,0,1), n) is degenerate for a slope whose normal lies along Z,
    // normalize() of that is NaN, and NaN * 0 is still NaN -- so the unauthored path was
    // not the no-op it looked like.
    if (abs(authored_normal_ts.x) + abs(authored_normal_ts.y) > 1e-5) {
        // Pick the reference axis furthest from the normal so the cross stays well
        // conditioned on any slope.
        var reference = vec3<f32>(0.0, 0.0, 1.0);
        if (abs(geometric_n.z) > 0.9) {
            reference = vec3<f32>(1.0, 0.0, 0.0);
        }
        let t_axis = normalize(cross(reference, geometric_n));
        let b_axis = cross(geometric_n, t_axis);
        n = normalize(t_axis * authored_normal_ts.x
                    + b_axis * authored_normal_ts.y
                    + geometric_n * max(authored_normal_ts.z, 1e-3));
    }

    // Fine-surface world height, sampled once and shared by the snow cover
    // below and the terrain-shadow test further down (LOD/morph-independent).
    let world_y = sample_height(in.world_xz);
    let line_depth = snowline_depth(in.world_xz, world_y);
    let initial_soil_wet = soil_wet_amount(mud, tp.rain_wetness, snow_cover(in.world_xz, world_y));
    var soil_wet = initial_soil_wet;
    var rain_puddle = 0.0;
    if (tp.rain_wetness > 0.0) {
        rain_puddle = ground_puddle_mask(in.world_xz, puddle_footprint, tp.rain_wetness)
            * ground_puddle_land_factor(geometric_n.y, world_y, tp.sea_level,
                                       snow_cover(in.world_xz, world_y));
    }
    if (snow.info.w > 0.0 || line_depth > 0.0) {
        let dry = smoothstep(tp.sea_level, tp.sea_level + 0.25, world_y);
        let snow_h = snow_cover(in.world_xz, world_y) * dry;
        let coverage = snow_surface_coverage(snow_h, geometric_n.y, 1.0);
        // Compact against the deepest base present, so pressed altitude tracks
        // darken exactly like pressed deposit snow. Deposit-only is unchanged.
        let snow_ref = max(snow.info.w, line_depth);
        let compacted = clamp(1.0 - snow_h / max(snow_ref, 0.001), 0.0, 1.0);
        var snow_rgb = snow_powder_albedo(vec3<f32>(in.world_xz.x, world_y, in.world_xz.y), snow_footprint, compacted);
        if (linear <= 0.5) { snow_rgb = ground_linear_to_srgb(snow_rgb); }
        rgb = mix(rgb, snow_rgb, coverage);
        authored_ao = mix(authored_ao, 1.0, coverage);
        n = normalize(mix(n, terrain_snow_normal(in.world_xz, world_y, geometric_n, snow_footprint, dry), coverage));
    }

    // Combined sun shadow: CSM (objects + near contact) and the long-range
    // heightfield mask (terrain-on-terrain) compose by max() — whichever occludes
    // the sun more wins. Both fade out with fog. The mask stores, per column, the
    // world height below which that column is terrain-shadowed (.r = ceiling,
    // .g = penumbra half-width in metres, .b = strength), so a point is shadowed by
    // how far its world height sits below the ceiling. Its grid is `scale`x the
    // heightmap (sharper edges): sample in mask-texel space (world -> heightfield
    // texel -> * scale), landing on texel centres at (coord + 0.5)/dims.
    let csm_s = shadow_strength(in.world_pos, n, in.fog, dwx, dwy);
    let mask_dims = vec2<f32>(textureDimensions(shadow_mask));
    let mask_scale = mask_dims / vec2<f32>(f32(tp.hm_width), f32(tp.hm_height));
    let mask_coord = (in.world_xz - tp.world_origin) / tp.terrain_grid * mask_scale;
    let mask_uv = (mask_coord + vec2<f32>(0.5)) / mask_dims;
    let sm = textureSampleLevel(shadow_mask, shadow_mask_samp, mask_uv, 0.0);
    // Fine-surface world height for the shadow test, sampled from the heightmap at this
    // fragment rather than the interpolated mesh height. The mask's ceiling (sm.r) is
    // baked from the full-resolution heightfield, but in.world_pos.y sags toward the
    // coarse lattice at distant LODs; testing that mismatched height drops whole patches
    // below the ceiling and flashes tile-sized shadow blobs several km out. Sampling the
    // fine surface makes the test LOD/morph-independent, the same independence the
    // per-fragment normal above relies on (shared with the snow cover above, so
    // no extra heightmap tap).
    // (world_y was sampled once above for the snow cover; reused here.)
    let lit = smoothstep(sm.r - sm.g, sm.r + sm.g + 1e-3, world_y);
    let terrain_s = clamp(sm.b * (1.0 - lit), 0.0, 1.0) * in.fog;
    let shadow = max(csm_s, terrain_s);

    // Coast wet band: near-flat terrain around the (swash-moved) sea level reads as damp sand —
    // darker albedo — strongest at the waterline, fading out over wet_height metres. Keyed on
    // the SAME sea level + swash the water uses, so the wet line registers with the water's edge.
    // Slope-gated by n.y so cliffs/steep coast stay dry. Cosmetic; zero gameplay impact.
    let sea_ref = tp.sea_level + sin(6.2831853 * tp.time * tp.swash_speed) * tp.swash_amp;
    let above_sea = world_y - sea_ref;
    let flat = smoothstep(0.55, 0.85, n.y);
    var wet = flat * (1.0 - smoothstep(0.0, tp.wet_height, above_sea));
    // TW-WATER W3b: in Tidewater mode, Tidewater's wetness instead (see TwWet above). The lace
    // footprint is needed only in the Tidewater branch. This UBO selector is uniform,
    // so the derivatives stay in uniform control flow without normal Current OP work.
    if (tww.p.x > 0.5) {
        let tw_fp = length(fwidth(in.world_xz)) / TW_LACE_TILE * f32(TW_LACE_N);
        let h = world_y - tww.p.y;
        var sim = vec4<f32>(0.0);
        var inside = 0.0;
        if (h < 3.5 && h > -8.0) {
            let pick = tw_sim_pick(in.world_xz);
            inside = pick.y;
            if (inside > 0.0) { sim = tw_sim_state_at(in.world_xz, i32(pick.x)) * inside; }
        }
        // outside the simulated regions: Tidewater's static damp band
        let band = smoothstep(0.45, 0.0, h);
        // (Tidewater's landW: the seabed below the waterline is not the wet beach)
        let land = 1.0 - smoothstep(0.12, -0.6, h);
        let tw_wet = max(sim.y, band * (1.0 - inside)) * flat * land;
        // Tidewater Terrain.js: wet sand is darker (wetDarken 0.58), a little more saturated and
        // a touch cooler
        let wet_c = rgb * 0.58;
        let lum = dot(wet_c, vec3<f32>(0.2126, 0.7152, 0.0722));
        let wet_albedo = mix(vec3<f32>(lum), wet_c, 1.15) * vec3<f32>(0.97, 0.98, 1.0);
        rgb = mix(rgb, wet_albedo, tw_wet);
        // foam stranded on the sand, lacy and popping (ShoreSim.sandFoam)
        let residue = tw_sand_foam(in.world_xz, sim, tw_fp) * flat * land;
        var foam_c = vec3<f32>(0.748414, 0.787412, 0.787412); // Tidewater srgb(0.88, 0.9, 0.9), linear
        if (linear <= 0.5) { foam_c = vec3<f32>(0.88, 0.9, 0.9); }
        rgb = mix(rgb, foam_c, residue);
        wet = 0.0;
    }
    rgb *= mix(1.0, tp.wet_darken, wet);

    // A shadow removes the direct sun (the N.L diffuse term); sky ambient and the
    // local point/spot lamps survive, so shadowed terrain settles to the ambient
    // tone rather than going black. This is why the darkening reads as a soft cast
    // shadow and never collapses to pure black when CSM's darkness constant is 0
    // (shadow maps disabled). Terrain has no material, so local lights modulate white.
    let cos_fi = max(dot(n, -frame.sun_dir_world.xyz), 0.0);
    var sun_diffuse = frame.sun_diffuse.rgb * fog_sun_reach(in.world_pos);
    var sun_ambient = frame.sun_ambient.rgb;
    var fog_color = frame.fog_color.rgb;
    // sun_diffuse.w = 1: sky-based lighting, sun/ambient are already physical linear radiance
    // (atmosphere-derived), so don't sRGB-decode them (that path only applies to legacy gamma sun).
    let sky_lit = frame.sun_diffuse.w > 0.5;
    if (linear > 0.5) {
        if (!sky_lit) {
            sun_diffuse = srgb_to_linear(sun_diffuse);
            sun_ambient = srgb_to_linear(sun_ambient);
        }
        fog_color = srgb_to_linear(fog_color);
    }
    // Sky-based lighting: replace the flat ambient with DIRECTIONAL sky irradiance (SH-9 env
    // projection, per surface normal), scaled by the skyAmbient knob in sun_ambient.w.
    if (sky_lit) {
        // Directional ambient (Stage 2): sky sampled along the bent normal — the average open
        // direction — rather than the surface normal, so a slope beside an occluder picks up
        // light from where it can actually see sky. Returns `n` when the path is off.
        let amb_n = interior_sky_ambient_normal(
            in.world_pos + frame.cam_pos.xyz,
            gtao_bent_normal_world(in.clip.xy, n),
        );
        sun_ambient = sky_irradiance(amb_n) * frame.sun_ambient.w;
    }
    // Ambient occlusion: scale the ambient (directional SH or legacy flat) by how much sky this
    // point can see. Orthogonal to `shadow`, which removes the DIRECT sun. The two terms MULTIPLY
    // because they occlude independently and at disjoint scales (plan §6): sky-visibility is the
    // baked km-scale column factor that darkens valleys and cliff-bases, GTAO the screen-space
    // near/mid term that resolves local folds and object contact. Each returns 1 when off.
    //
    // Interior sky visibility joins them as a third independent occluder. Terrain is deliberately
    // absent from that MAP (a hillside is not a roof), but it must still RECEIVE the term: the
    // floor of a shed, a barrack or an archway is terrain, and leaving it at full sky ambient
    // while the walls around it darkened would look worse than not having the feature.
    //
    // The authored surfaces' own normal map joins them as a FOURTH occluder, at a scale none of
    // the three can resolve (their smallest feature is a depth-buffer pixel; a crevice in a
    // normal map has no depth at all). Same term, same derivation and the same never-brightens /
    // exactly-1-on-a-flat-map guarantees as the object path — frame::normal_map_cavity — because
    // the dawn ground is flat for exactly the reason a dawn wall is: with the sun's N.L gone,
    // nothing left in the frame was a function of the per-pixel normal at better than ~2 m.
    // `geometric_n` is the heightfield's central difference and `n` the surface-normal-mapped
    // result, which is precisely the (macro, micro) pair the term is defined on; at an unauthored
    // or fully faded-out cell they are the same vector and this is exactly 1.0.
    //
    // Dry terrain gets NO sky specular, and that is a judgement rather than an omission: the term is
    // scaled by the material's gloss and terrain has no material at all, so ground would take the
    // bare 4% dielectric reallocation — an invisible result for a per-pixel reflect() plus a
    // second SH evaluation on the single most fill-heavy surface in the frame.
    let local_sky_visibility = interior_sky_ao(in.world_pos + frame.cam_pos.xyz, n);
    var diagnostic_rain = vec3<f32>(0.0);
    let procedural_film = rain_puddle;
    if (rain_puddle > 0.0 || soil_wet > 0.0) {
        // Zenith geometry gates vertical rain independently of diffuse sky light
        // entering through windows or around a canopy. Only wet eligible puddle
        // pixels pay this one-layer query. Missing map coverage remains unproven.
        let rain_reach = interior_rain_reach(in.world_pos + frame.cam_pos.xyz);
        // Unknown physical coverage cannot prove an exposed ground surface.
        let rain_coverage = interior_rain_coverage(in.world_pos + frame.cam_pos.xyz);
        let rain_proof = ground_puddle_rain_factor(rain_reach, rain_coverage);
        diagnostic_rain = vec3<f32>(rain_coverage, rain_reach, rain_proof);
        rain_puddle *= rain_proof;
        soil_wet *= rain_proof;
    }
    // Proven wet soil absorbs through the authored substrate and uses the
    // rough moist-soil coat below. Keep the smooth film on the hard fraction.
    let wet_surface = soil_wet_surface(mud, soil_wet, rain_puddle);
    // Render-only classification after actual source/snow/rain admission and
    // film partition. Water overlay stays present and marks its own survivors.
    let wet_debug = wet_soil_debug_mode();
    if (wet_debug == 1u) { return vec4<f32>(mud, initial_soil_wet, tp.rain_wetness, 1.0); }
    if (wet_debug == 2u) { return vec4<f32>(diagnostic_rain, 1.0); }
    if (wet_debug == 3u) { return vec4<f32>(soil_wet, wet_surface.y, procedural_film, 1.0); }
    if (wet_debug == 4u) { return vec4<f32>(0.08, 0.08, 0.08, 1.0); }
    rain_puddle = wet_surface.y;
    rgb *= wet_surface.x;
    sun_ambient *= sky_vis_ao(in.world_xz)
        * gtao_ao(in.clip.xy)
        * local_sky_visibility
        * normal_map_cavity(geometric_n, n) * authored_ao;
    // CLD-020: cloud transmittance dims the DIRECT term only, leaving sky ambient intact, so
    // ground under a cloud settles toward ambient rather than toward black.
    let sun_raw = sun_diffuse * cos_fi * (1.0 - shadow) * cloud_sun_shadow(in.world_xz) + sun_ambient;
    // HDR keeps radiance uncapped into the float target; LDR saturates like GL33.
    let sun = select(min(sun_raw, vec3<f32>(1.0)), sun_raw, linear > 0.5);
    let local = lights_contrib(in.world_pos, n, vec3<f32>(1.0), vec3<f32>(1.0), linear);
    let light_sum = sun + local;
    rgb *= select(clamp(light_sum, vec3<f32>(0.0), vec3<f32>(1.0)), max(light_sum, vec3<f32>(0.0)), linear > 0.5);
    if (soil_wet > 0.0) {
        let view = normalize(-in.world_pos);
        // Filter the disturbed-soil normal at the actual pixel footprint;
        // never reflect the authored albedo grain or its fine normal map.
        let moist_n = geometric_n;
        let fresnel = soil_wet_fresnel(dot(moist_n, view));
        var reflected = sun_ambient;
        if (sky_lit) {
            // A wet soil coat reflects directional sky radiance. Diffuse SH
            // irradiance here washes the soil into a pale, cloudless coating.
            let samples = soil_wet_environment_samples(view, moist_n,
                soil_wet * (1.0-rain_puddle), moist_normal_variation);
            let sky_scale = frame.sun_ambient.w * sky_vis_ao(in.world_xz) * local_sky_visibility;
            var rough_reflected = vec3<f32>(0.0);
            var reflection_weight = 0.0;
            for (var i=0u; i<8u; i+=1u) {
                if (samples[i].w > 0.0) {
                    let sky_reflected = ground_sky_reflection(samples[i].xyz) * sky_scale;
                    // Every direction respects the same existing local plane;
                    // diffuse-ground fallback is approximate, never an SSR hit.
                    rough_reflected += soil_wet_ground_reflection(sky_reflected, rgb,
                        dot(samples[i].xyz, ground_normals.broad), moist_cone_horizon_width) * samples[i].w;
                    reflection_weight += samples[i].w;
                }
            }
            // Back-facing/no-sample geometry has no available reflected sky.
            reflected = select(rgb, rough_reflected, reflection_weight > 0.0);
        }
        let half_vector = normalize(view - frame.sun_dir_world.xyz + vec3<f32>(0.0, 1e-4, 0.0));
        rgb = mix(rgb, rgb * (1.0 - fresnel) + reflected * fresnel,
            soil_wet * (1.0 - rain_puddle));
        // Direct GGX already contains dielectric Fresnel; add after the sky mix.
        // Only the admitted moist fraction smooths its broad slope width;
        // geometric derivatives still widen it at unresolved pixel footprints.
        let solar_visibility = (1.0 - shadow) * cloud_sun_shadow(in.world_xz)
            * local_sky_visibility;
        let solar_coat = soil_wet_sun_specular(
            clamp(dot(moist_n, view), 0.0, 1.0),
            clamp(dot(moist_n, -frame.sun_dir_world.xyz), 0.0, 1.0),
            clamp(dot(moist_n, half_vector), 0.0, 1.0),
            clamp(dot(view, half_vector), 0.0, 1.0),
            moist_normal_variation, soil_wet * (1.0 - rain_puddle), solar_visibility);
        rgb += sun_diffuse * 3.14159265359 * solar_coat;
    }
    if (rain_puddle > 0.0) {
        // Reflect actual directional sky/cloud radiance. Diffuse SH erased
        // cloud contrast and made these wet patches read as a pale paint film.
        let view = normalize(-in.world_pos);
        let water_n = ground_puddle_ripple_normal(in.world_xz, tp.time, tp.rain_strength, puddle_footprint);
        let grazing = 1.0 - clamp(dot(water_n, view), 0.0, 1.0);
        let fresnel = 0.02 + 0.98 * pow(grazing, 5.0);
        var reflected = sun_ambient;
        if (sky_lit) {
            reflected = ground_sky_reflection(reflect(-view, water_n)) * frame.sun_ambient.w
                * sky_vis_ao(in.world_xz) * local_sky_visibility;
        }
        let half_vector = normalize(view - frame.sun_dir_world.xyz + vec3<f32>(0.0, 1e-4, 0.0));
        let sun_lobe = pow(max(dot(water_n, half_vector), 0.0), 96.0);
        reflected += sun_diffuse * sun_lobe * (1.0 - shadow) * cloud_sun_shadow(in.world_xz)
            * local_sky_visibility;
        // Raw roof admission above suppresses both the film and its reflected sky.
        rgb = mix(rgb, rgb * (1.0 - fresnel) + reflected * fresnel, rain_puddle);
    }

    // Debug: output the contrast-shaped sky-view factor as greyscale (unfogged) to inspect/tune the
    // mask — responds to radius/azimuths/downsample/contrast.
    if (sky_vis_debug_on() > 0.5) {
        return vec4<f32>(vec3<f32>(sky_vis_debug_value(in.world_xz)), 1.0);
    }

    // Debug: the raw screen-space AO buffer as greyscale (unfogged), for tuning radius/strength/
    // slices/steps/blur against the buffer itself rather than through the lit result.
    if (gtao_debug_on() > 0.5) {
        return vec4<f32>(gtao_debug_colour(in.clip.xy, n), 1.0);
    }

    // Debug: the interior sky-reach factor as greyscale (unfogged). Terrain and objects switch
    // together so the whole opaque scene shows the same buffer.
    if (interior_sky_debug_on() > 0.5) {
        return vec4<f32>(vec3<f32>(interior_sky_reach(in.world_pos + frame.cam_pos.xyz)), 1.0);
    }

    // fog_enabled: 2 = aerial perspective via the froxel volume (per-fragment); 1 =
    // legacy flat distance fog; 0 = off. in.fog is still used above for distance-faded
    // shadows regardless.
    if (frame.params.fog_enabled >= 1.5) {
        rgb = apply_fog_terrain(rgb, in.world_pos);
    } else {
        rgb = mix(fog_color, rgb, in.fog);
    }
    return vec4<f32>(rgb, 1.0);
}

// Depth + normal prepass fragment (docs/depth-prepass-plan.md). Reuses vs_terrain
// unchanged and writes ONLY the view-space octahedral normal into the Rg16Float
// G-buffer (depth is written by the fixed-function stage). The normal is the same
// per-fragment heightmap central difference fs_terrain derives, transformed to view
// space (view translation is zeroed, so the direction transform is a pure rotation).
@fragment
fn fs_terrain_prepass(in: VsOut) -> @location(0) vec2<f32> {
    if (in_terrain_hole(in.world_pos + frame.cam_pos.xyz)) {
        discard;
    }
    let footprint = max(length(dpdx(in.world_pos)), length(dpdy(in.world_pos)));
    var n = sample_mud_normal(in.world_xz, tp.terrain_grid, footprint);
    let py = sample_height(in.world_xz);
    if (snow.info.w > 0.0 || snowline_depth(in.world_xz, py) > 0.0) {
        let dry = smoothstep(tp.sea_level, tp.sea_level + 0.25, py);
        let coverage = snow_surface_coverage(snow_cover(in.world_xz, py) * dry, n.y, 1.0);
        n = normalize(mix(n, terrain_snow_normal(in.world_xz, py, n, footprint, dry), coverage));
    }
    let n_view = (frame.view * vec4<f32>(n, 0.0)).xyz;
    return oct_encode(normalize(n_view));
}
