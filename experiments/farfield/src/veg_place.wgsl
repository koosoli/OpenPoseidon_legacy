// EXP-VEG-002: far vegetation reconstructed from a density field, not streamed.
//
// This generalises what the engine's grass rings already do (GPU placement on a
// camera-snapped candidate grid, stochastic accept against a density field, atomic
// compaction, one indirect draw) from grass tufts to tree cards. Nothing is stored
// per tree and nothing is streamed: the placement is a pure function of world
// position, so the same tree appears in the same place every frame and across
// machines, and the cost scales with the CANDIDATE GRID, not with how many trees
// the source dataset nominally contains.

struct Params {
    view_proj: mat4x4<f32>,
    cam: vec4<f32>,          // xyz camera, w = card cutoff in metres
    fwd: vec4<f32>,
    right: vec4<f32>,
    up: vec4<f32>,
    world: vec4<f32>,        // grid, n, cover_n, cover_step
    cfg: vec4<f32>,          // grid_dim, spacing, radius, canopy_scale
    sun: vec4<f32>,
};

struct Instance {
    pos: vec3<f32>,
    height: f32,
    seed: f32,
    _pad: vec3<f32>,
};

struct DrawArgs {
    index_count: u32,
    instance_count: atomic<u32>,
    first_index: u32,
    base_vertex: i32,
    first_instance: u32,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var height_tex: texture_2d<f32>;
@group(0) @binding(2) var cover_tex: texture_2d<f32>;
@group(0) @binding(3) var<storage, read_write> instances: array<Instance>;
@group(0) @binding(4) var<storage, read_write> args: DrawArgs;

fn hash_u(v: u32) -> u32 {
    var h = v;
    h ^= h >> 16u; h = h * 0x7feb352du;
    h ^= h >> 15u; h = h * 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}
fn hash01(v: u32) -> f32 { return f32(hash_u(v) & 0xffffffu) / 16777216.0; }

fn height_at(wxz: vec2<f32>) -> f32 {
    let n = i32(P.world.y);
    let g = wxz / P.world.x;
    let i = floor(g);
    let f = g - i;
    let ix = i32(i.x); let iz = i32(i.y);
    let c = func_load(ix, iz, n);
    let b = func_load(ix + 1, iz, n);
    let d = func_load(ix, iz + 1, n);
    let e = func_load(ix + 1, iz + 1, n);
    return mix(mix(c, b, f.x), mix(d, e, f.x), f.y);
}
fn func_load(ix: i32, iz: i32, n: i32) -> f32 {
    return textureLoad(height_tex, vec2<i32>(clamp(ix, 0, n - 1), clamp(iz, 0, n - 1)), 0).x;
}

fn cover_at(wxz: vec2<f32>) -> vec4<f32> {
    let cn = i32(P.world.z);
    let c = clamp(vec2<i32>(floor(wxz / P.world.w)), vec2<i32>(0), vec2<i32>(cn - 1));
    return textureLoad(cover_tex, c, 0);
}

@compute @workgroup_size(8, 8, 1)
fn place(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dim = u32(P.cfg.x);
    if (gid.x >= dim || gid.y >= dim) { return; }

    let spacing = P.cfg.y;
    let radius = P.cfg.z;
    // Camera-snapped origin: the grid slides with the camera in whole cells, so a
    // moving camera never re-randomises the trees it already placed.
    let origin = floor((P.cam.xz - vec2<f32>(radius)) / spacing) * spacing;
    let cell = vec2<f32>(f32(gid.x), f32(gid.y));

    let key = gid.x * 73856093u ^ gid.y * 19349663u;
    // Jitter breaks the lattice; derived from the WORLD cell, not the thread id, so
    // it is stable as the window slides.
    let wcell = vec2<i32>(origin / spacing) + vec2<i32>(i32(gid.x), i32(gid.y));
    let wkey = u32(wcell.x) * 73856093u ^ u32(wcell.y) * 19349663u;
    let jx = hash01(wkey) - 0.5;
    let jz = hash01(wkey ^ 0x9e3779b9u) - 0.5;
    let wxz = origin + (cell + vec2<f32>(0.5) + vec2<f32>(jx, jz) * 0.9) * spacing;

    let to_cam = wxz - P.cam.xz;
    let d = length(to_cam);
    if (d > radius) { return; }
    // Inside the card cutoff the near renderer owns these trees.
    if (d < P.cam.w) { return; }

    let cover = cover_at(wxz);
    if (cover.x < 0.02) { return; }
    if (hash01(wkey ^ 0x51ed270bu) > cover.x) { return; }

    let terr = height_at(wxz);
    let canopy = max(cover.y * P.cfg.w, 6.0);
    let h = canopy * (0.75 + 0.5 * hash01(wkey ^ 0x2545f491u));

    // Frustum reject on the card's bounding sphere, in the compute, exactly as the
    // engine's object cull does.
    let c_rel = vec3<f32>(wxz.x, terr + h * 0.5, wxz.y) - P.cam.xyz;
    let clip = P.view_proj * vec4<f32>(c_rel, 1.0);
    let r = h * 0.75;
    if (clip.w < -r) { return; }
    let m = max(abs(clip.w), 1.0);
    if (abs(clip.x) > m + r * 2.0 || abs(clip.y) > m + r * 2.0) { return; }

    let slot = atomicAdd(&args.instance_count, 1u);
    if (slot >= arrayLength(&instances)) { return; }
    instances[slot].pos = vec3<f32>(wxz.x, terr, wxz.y);
    instances[slot].height = h;
    instances[slot].seed = hash01(wkey);
}
