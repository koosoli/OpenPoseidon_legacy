// CDLOD raster arm (EXP-TERRAIN-001/002/003).
//
// One shared 32x32 patch mesh, instanced once per selected quadtree node, with the
// vertex stage pulling height straight out of the heightmap and morphing toward the
// parent grid across the node's morph band. This is the scheme the engine's
// TerrainWgpu already runs; it is reproduced here only so the two arms of the
// comparison hit the same GPU, the same target and the same shading.
//
// Reverse-Z with an infinite far plane. At a 100 km view distance a conventional
// 0..1 depth buffer with a 1 m near plane has no usable precision past a few km,
// so this is not a refinement — it is a precondition for the experiment.

struct Params {
    view_proj: mat4x4<f32>,
    cam: vec4<f32>,
    world: vec4<f32>,   // grid, n, levels, extent
    sun: vec4<f32>,
    cover: vec4<f32>,   // cover_n, cover_step, canopy_scale, building_scale
    bounds: vec4<f32>,  // h_min, h_max, sea_level, fog_scale
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var height_tex: texture_2d<f32>;
@group(0) @binding(2) var cover_tex: texture_2d<f32>;

const GRID_N = 32.0;

fn h_at_texel(ix: i32, iz: i32) -> f32 {
    let n = i32(P.world.y);
    return textureLoad(height_tex, vec2<i32>(clamp(ix, 0, n - 1), clamp(iz, 0, n - 1)), 0).x;
}

fn height_at(wxz: vec2<f32>) -> f32 {
    let g = wxz / P.world.x;
    let i = floor(g);
    let f = g - i;
    let ix = i32(i.x);
    let iz = i32(i.y);
    let a = h_at_texel(ix, iz);
    let b = h_at_texel(ix + 1, iz);
    let c = h_at_texel(ix, iz + 1);
    let d = h_at_texel(ix + 1, iz + 1);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world_xz: vec2<f32>,
    @location(1) dist: f32,
};

@vertex
fn vs(
    @location(0) grid_pos: vec2<f32>,
    @location(1) origin: vec2<f32>,
    @location(2) size_level: vec2<f32>,
    @location(3) morph: vec2<f32>,
) -> VsOut {
    let size = size_level.x;

    // Unmorphed position, used only to pick the morph factor — taking it from the
    // morphed position would make the factor depend on itself.
    let coarse_xz = origin + grid_pos * size;
    let h0 = height_at(coarse_xz);
    let d = length(vec3<f32>(coarse_xz.x, h0, coarse_xz.y) - P.cam.xyz);
    let k = clamp((d - morph.x) / max(morph.y - morph.x, 1e-3), 0.0, 1.0);

    // Morph toward the parent grid by snapping odd vertices onto even ones.
    let g = grid_pos * GRID_N;
    let g_even = floor(g * 0.5) * 2.0;
    let gm = mix(g, g_even, k);
    let world_xz = origin + (gm / GRID_N) * size;

    let h = height_at(world_xz);
    let rel = vec3<f32>(world_xz.x, h, world_xz.y) - P.cam.xyz;

    var o: VsOut;
    o.clip = P.view_proj * vec4<f32>(rel, 1.0);
    o.world_xz = world_xz;
    o.dist = length(rel);
    return o;
}

fn cover_at(wxz: vec2<f32>) -> vec4<f32> {
    let cn = i32(P.cover.x);
    let c = clamp(vec2<i32>(floor(wxz / P.cover.y)), vec2<i32>(0), vec2<i32>(cn - 1));
    return textureLoad(cover_tex, c, 0);
}

fn sky_color(dir_y: f32) -> vec3<f32> {
    let k = clamp(dir_y * 2.2 + 0.10, 0.0, 1.0);
    return mix(vec3<f32>(0.72, 0.80, 0.90), vec3<f32>(0.28, 0.45, 0.78), k);
}

@fragment
fn fs(i: VsOut) -> @location(0) vec4<f32> {
    let s = P.world.x;
    let hx0 = height_at(i.world_xz - vec2<f32>(s, 0.0));
    let hx1 = height_at(i.world_xz + vec2<f32>(s, 0.0));
    let hz0 = height_at(i.world_xz - vec2<f32>(0.0, s));
    let hz1 = height_at(i.world_xz + vec2<f32>(0.0, s));
    let nrm = normalize(vec3<f32>(hx0 - hx1, 2.0 * s, hz0 - hz1));
    let terr = height_at(i.world_xz);
    let c = cover_at(i.world_xz);

    var albedo = vec3<f32>(0.36, 0.34, 0.26);
    if (terr < P.bounds.z + 0.5) {
        albedo = vec3<f32>(0.10, 0.17, 0.24);
    } else if (terr > 1050.0) {
        albedo = vec3<f32>(0.82, 0.83, 0.86);
    } else if (nrm.y < 0.72) {
        albedo = vec3<f32>(0.30, 0.28, 0.26);
    }
    if (c.x > 0.15) {
        albedo = mix(albedo, vec3<f32>(0.13, 0.26, 0.11), clamp(c.x * 1.4, 0.0, 1.0));
    }

    let lambert = clamp(dot(nrm, normalize(P.sun.xyz)), 0.0, 1.0);
    var col = albedo * (0.30 + 0.85 * lambert);
    let fog = 1.0 - exp(-i.dist / 26000.0);
    // Sky colour needs the view direction's y; recover it from the distance vector.
    let dir_y = (terr - P.cam.y) / max(i.dist, 1.0);
    col = mix(col, sky_color(dir_y) * 1.02, clamp(fog, 0.0, 1.0));
    return vec4<f32>(pow(col, vec3<f32>(1.0 / 2.2)), 1.0);
}

// Sky background, drawn as a full-screen triangle before the terrain.
struct SkyOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) ndc: vec2<f32>,
};

@vertex
fn vs_sky(@builtin(vertex_index) vi: u32) -> SkyOut {
    var p = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -3.0), vec2<f32>(-1.0, 1.0), vec2<f32>(3.0, 1.0));
    var o: SkyOut;
    o.clip = vec4<f32>(p[vi], 0.0, 1.0);
    o.ndc = p[vi];
    return o;
}

@fragment
fn fs_sky(i: SkyOut) -> @location(0) vec4<f32> {
    // Cheap stand-in: the raymarch arm's sky uses the true ray direction, this uses
    // screen y. Both are backgrounds, and neither is what is being measured.
    let col = sky_color(-i.ndc.y * 0.5 + 0.05);
    return vec4<f32>(pow(col, vec3<f32>(1.0 / 2.2)), 1.0);
}
