#define_import_path conform

// The terrain heightmap + its sampling params, bound at group(4) by both the lit
// mesh pipeline (shader3d) and the shadow depth pass (shadow_depth), so each conforms
// ClipLand vegetation to the ground (SurfaceY) per vertex without the CPU rewriting the
// shared mesh. The R32Float heightmap is read with textureLoad in the vertex stage
// (non-filterable); HmParams mirrors the Rust TerrainConformParams (48 bytes). Both the
// color and shadow passes import surface_y from HERE — the CPU gameplay conform
// (Object::Animate → occlusion/LOS) evaluates the same SurfaceY, so a single shared
// definition keeps the rendered geometry aligned with the gameplay occluders.
struct HmParams {
    origin: vec2<f32>,   // world xz of heightmap texel (0,0)
    terrain_grid: f32,   // world metres per heightmap texel
    enabled: f32,        // 1 when a heightmap is loaded
    hm_width: u32,
    hm_height: u32,
    // WLD-023(c): the CDLOD lattice the terrain RASTERISES. See surface_y_raster.
    cdlod_base: f32,     // ranges[0] in metres (leafSize * WGR_TERRAIN_LOD_BASE)
    cdlod_ratio: f32,    // per-level range multiplier (WGR_TERRAIN_LOD_RATIO)
    cdlod_morph: f32,    // fraction of each band spent morphing (WGR_TERRAIN_MORPH)
    cdlod_mode: f32,     // 0 = evaluate the fine surface (default), 1 = the rasterised one
};
@group(4) @binding(0) var hm: texture_2d<f32>;
@group(4) @binding(1) var<uniform> hm_params: HmParams;

struct SnowData {
    info: vec4<f32>,
    deficits: array<f32>,
};
@group(4) @binding(2) var<storage, read> snow: SnowData;

// Same buffer and evaluation as terrain.wgsl. The source parity test keeps this
// sampling identical despite the different bind-group locations of both pipelines.
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

// The second view in this binding holds signed mud, independent of snow cover.
// Old/snow-only probe buffers are safely undeformed until a full mud view exists.
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
    let h = max(0.125, footprint);
    return vec2<f32>(sand_height_offset(world_xz + vec2<f32>(h, 0.0)) - sand_height_offset(world_xz - vec2<f32>(h, 0.0)),
        sand_height_offset(world_xz + vec2<f32>(0.0, h)) - sand_height_offset(world_xz - vec2<f32>(0.0, h))) / (2.0 * h);
}

fn mud_height_gradient(world_xz: vec2<f32>, footprint: f32) -> vec2<f32> {
    let h = max(0.125, footprint);
    return vec2<f32>(mud_height_offset(world_xz + vec2<f32>(h, 0.0)) - mud_height_offset(world_xz - vec2<f32>(h, 0.0)),
        mud_height_offset(world_xz + vec2<f32>(0.0, h)) - mud_height_offset(world_xz - vec2<f32>(0.0, h))) / (2.0 * h);
}

fn hm_load(ix: i32, iz: i32) -> f32 {
    let cx = clamp(ix, 0, i32(hm_params.hm_width) - 1);
    let cz = clamp(iz, 0, i32(hm_params.hm_height) - 1);
    return textureLoad(hm, vec2<i32>(cx, cz), 0).x;
}

// Absolute terrain height at world xz, matching Landscape::SurfaceY and the terrain
// shader's sample_height exactly (per-cell two-triangle interpolation, NOT bilinear),
// so conformed vegetation sits on the same ground the terrain pass renders.
fn surface_y(world_xz: vec2<f32>) -> f32 {
    if (hm_params.enabled < 0.5) {
        return 0.0;
    }
    let t = (world_xz - hm_params.origin) / hm_params.terrain_grid;
    let base = floor(t);
    let ix = i32(base.x);
    let iz = i32(base.y);
    let f = t - base;
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + 1, iz);
    let y10 = hm_load(ix, iz + 1);
    let y11 = hm_load(ix + 1, iz + 1);
    if (f.x <= 1.0 - f.y) {
        return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x + mud_height_offset(world_xz) + sand_height_offset(world_xz);
    }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y + mud_height_offset(world_xz) + sand_height_offset(world_xz);
}

// ---------------------------------------------------------------------------
// WLD-023(c) -- the surface the terrain RASTERISES, not the one it EVALUATES.
//
// `surface_y` above is the FULL-RESOLUTION height field. The terrain pass only evaluates
// that field at its CDLOD lattice vertices and linearly interpolates between them
// (terrain.wgsl vs_terrain: one node of world size S is tessellated into GRID_N=32 quads,
// so its vertex spacing is S/32 = one heightmap texel x 2^level). On CONVEX ground the fine
// field sits ABOVE the coarse chord, and a road pinned to the fine field therefore wins
// `depth_compare: GreaterEqual` against the ridge in front of it -- with depth_write = false
// and no depth-prepass entry, the ridge can never paint back over it. That is the road drawn
// through the mountain. A constant world-space depth bias bounds the symptom but cannot
// remove it, because the chord error grows with LOD while the bias does not.
//
// The fix is to evaluate the height field on the SAME lattice the terrain draws:
//
//   * level      L = ceil(log(dist / ranges[0]) / log(ratio)), floored at 0. This mirrors
//                  SelectCdlod (TerrainCdlod.hpp): a node is emitted at the coarsest level
//                  whose range still contains it. APPROXIMATION: the selector measures the
//                  distance to the node's AABB, we measure it to the point. The two differ
//                  by at most one node radius, i.e. only within one patch of a LOD boundary.
//   * spacing    step = terrain_grid * 2^L texels. The quadtree always halves (BuildCdlodNode),
//                  independent of cdlod_ratio, which scales the DISTANCE bands only.
//   * morph      k = clamp((dist - morphStart) / (morphEnd - morphStart)), matching
//                  CdlodMorphBand + terrain.wgsl:586-593.
//
// The lattice is anchored on heightmap texel (0,0). Real node origins are
// rootOriginTexel + n*32*2^L and rootOriginTexel is a whole number of leaves (32 texels),
// so for L <= 5 every lattice point lands on a texel index that is a multiple of 2^L --
// EXACT. Above L = 5 (spacing > 32 texels, i.e. kilometres out on any real world) the
// anchor can be off by up to 32 texels; the surface is still a chord of the right length,
// just not the same chord.
//
// The morph blend mixes the two lattice interpolants rather than reproducing the per-vertex
// snap. It is EXACT at both ends (k = 0 is the fine lattice, k = 1 is the coarse one, which
// is what a fully-morphed patch actually draws) and monotone between them.
const CDLOD_MAX_LEVEL: f32 = 12.0;

// The two-triangle interpolant of the height field on a lattice of `step` heightmap texels,
// anchored at texel (0,0). `step` = 1 reproduces surface_y bit-for-bit. The diagonal is the
// ANTI-diagonal, matching both Landscape::SurfaceY and the terrain grid mesh's index order
// (terrain/mod.rs build_grid emits [i0,i2,i1] = (x,z),(x,z+1),(x+1,z) as its first triangle).
// (`step_texels`, not `step`: `step` is a WGSL predeclared built-in function name and
// shadowing one is the class of mistake that costs a deploy here.)
fn lattice_y(world_xz: vec2<f32>, step_texels: f32) -> f32 {
    let cell = hm_params.terrain_grid * step_texels;
    let t = (world_xz - hm_params.origin) / cell;
    let base = floor(t);
    let f = t - base;
    let n = i32(step_texels);
    let ix = i32(base.x) * n;
    let iz = i32(base.y) * n;
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + n, iz);
    let y10 = hm_load(ix, iz + n);
    let y11 = hm_load(ix + n, iz + n);
    if (f.x <= 1.0 - f.y) {
        return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x + mud_height_offset(world_xz) + sand_height_offset(world_xz);
    }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y + mud_height_offset(world_xz) + sand_height_offset(world_xz);
}

// Absolute terrain height at world xz ON THE DRAWN SURFACE. `cam_pos` is the ABSOLUTE camera
// position (both object vertex shaders work camera-relative and already reconstruct it).
// Falls back to surface_y when the feature is off or the CDLOD description is missing, so an
// unpopulated params buffer degrades to the previous behaviour instead of to a flat world.
fn surface_y_raster(world_xz: vec2<f32>, cam_pos: vec3<f32>) -> f32 {
    if (hm_params.enabled < 0.5) {
        return 0.0;
    }
    if (hm_params.cdlod_mode < 0.5 || hm_params.cdlod_base <= 0.0 || hm_params.cdlod_ratio <= 1.0) {
        return surface_y(world_xz);
    }
    // Distance is measured to the point ON the fine surface, the same quantity
    // terrain.wgsl:582 uses to drive morph_k.
    let y_fine = surface_y(world_xz);
    let d = max(length(vec3<f32>(world_xz.x, y_fine, world_xz.y) - cam_pos), 1e-3);
    let level = clamp(ceil(log2(d / hm_params.cdlod_base) / log2(hm_params.cdlod_ratio)),
                      0.0, CDLOD_MAX_LEVEL);
    if (level < 0.5) {
        // Level 0 IS the full-resolution lattice; skip the second set of taps. This is also
        // the arm the WGR_TERRAIN_LOD_BASE pin exercises, so it must be exactly surface_y.
        let range0 = hm_params.cdlod_base;
        let ms0 = range0 * (1.0 - hm_params.cdlod_morph);
        var k0 = 0.0;
        if (range0 > ms0) {
            k0 = clamp((d - ms0) / (range0 - ms0), 0.0, 1.0);
        }
        if (k0 <= 0.0) {
            return y_fine;
        }
        return mix(y_fine, lattice_y(world_xz, 2.0), k0);
    }
    let lod_step = exp2(level);
    let range_end = hm_params.cdlod_base * pow(hm_params.cdlod_ratio, level);
    let range_prev = hm_params.cdlod_base * pow(hm_params.cdlod_ratio, level - 1.0);
    let morph_start = range_end - (range_end - range_prev) * hm_params.cdlod_morph;
    var k = 0.0;
    if (range_end > morph_start) {
        k = clamp((d - morph_start) / (range_end - morph_start), 0.0, 1.0);
    }
    let y_lod = lattice_y(world_xz, lod_step);
    if (k <= 0.0) {
        return y_lod;
    }
    return mix(y_lod, lattice_y(world_xz, lod_step * 2.0), k);
}

// Terrain height gradient (dSy/dx, dSy/dz) at world xz — the analytic derivative of
// surface_y's two-triangle interpolation (piecewise-constant per triangle). Used to tilt
// conformed vegetation normals so they follow the ground slope, matching the CPU's
// InvalidateNormals over the deformed faces. Same triangle split as surface_y.
fn surface_grad(world_xz: vec2<f32>) -> vec2<f32> {
    if (hm_params.enabled < 0.5) {
        return vec2<f32>(0.0, 0.0);
    }
    let t = (world_xz - hm_params.origin) / hm_params.terrain_grid;
    let base = floor(t);
    let ix = i32(base.x);
    let iz = i32(base.y);
    let f = t - base;
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + 1, iz);
    let y10 = hm_load(ix, iz + 1);
    let y11 = hm_load(ix + 1, iz + 1);
    var dhx: f32;
    var dhz: f32;
    if (f.x <= 1.0 - f.y) {
        dhx = y01 - y00;
        dhz = y10 - y00;
    } else {
        dhx = -(y10 - y11);
        dhz = -(y01 - y11);
    }
    return vec2<f32>(dhx, dhz) / hm_params.terrain_grid + mud_height_gradient(world_xz, 0.125) + sand_height_gradient(world_xz, 0.125);
}
