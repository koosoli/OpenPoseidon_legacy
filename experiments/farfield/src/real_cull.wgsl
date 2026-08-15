// EXP-ASSET-002: cull a real placement set on the GPU and compact it into a far-field
// draw. Source is every authored placement of a shipped world, uploaded once.
//
// This is the shape a far ASSET tier has to take, and it differs from the vegetation
// tier in one decisive way: a structure's position is authored and not negotiable, so
// it cannot be regenerated from a density field. It must be stored. The question this
// measures is whether storing and sweeping all of them is affordable.

struct Params {
    view_proj: mat4x4<f32>,
    cam: vec4<f32>,        // xyz camera, w = near cutoff (near renderer owns closer)
    cfg: vec4<f32>,        // count, radius, px_limit, screen_h
    classes: vec4<f32>,    // height per class: vegetation, structure, other, unused
};

struct Src {
    pos: vec3<f32>,
    class_id: u32,
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
@group(0) @binding(1) var<storage, read> src: array<Src>;
@group(0) @binding(2) var<storage, read_write> instances: array<Instance>;
@group(0) @binding(3) var<storage, read_write> args: DrawArgs;

fn hash01(v: u32) -> f32 {
    var h = v;
    h ^= h >> 16u; h = h * 0x7feb352du;
    h ^= h >> 15u; h = h * 0x846ca68bu;
    h ^= h >> 16u;
    return f32(h & 0xffffffu) / 16777216.0;
}

@compute @workgroup_size(64, 1, 1)
fn cull(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= u32(P.cfg.x)) { return; }

    let s = src[i];
    let h = P.classes[min(s.class_id, 2u)];

    let rel = s.pos - P.cam.xyz;
    let d = length(rel);
    if (d > P.cfg.y || d < P.cam.w) { return; }

    // Sub-pixel rejection. This is the test that actually decides whether a far tier
    // is affordable: most of a million placements are smaller than a pixel long
    // before they are outside the frustum, and dropping them is free correctness.
    // px_limit <= 0 disables it, so the A/B is one uniform apart.
    if (P.cfg.z > 0.0) {
        let px = h / max(d, 1.0) * P.cfg.w;
        if (px < P.cfg.z) { return; }
    }

    // Frustum reject on the bounding sphere, camera-relative (never absolute: at
    // kilometre-scale coordinates an absolute test shifts the frustum by cam_pos).
    let clip = P.view_proj * vec4<f32>(rel + vec3<f32>(0.0, h * 0.5, 0.0), 1.0);
    let r = h * 0.9;
    if (clip.w < -r) { return; }
    let m = max(abs(clip.w), 1.0) + r * 2.0;
    if (abs(clip.x) > m || abs(clip.y) > m) { return; }

    let slot = atomicAdd(&args.instance_count, 1u);
    if (slot >= arrayLength(&instances)) { return; }
    instances[slot].pos = s.pos;
    instances[slot].height = h * (0.75 + 0.5 * hash01(i));
    instances[slot].seed = hash01(i ^ 0x9e3779b9u);
}
