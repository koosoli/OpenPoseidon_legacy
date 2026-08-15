// EXP-VEG-002, draw half. Deliberately a SEPARATE module from the placement
// compute, because baseline WebGPU/WGPU will not let a vertex stage bind a
// `read_write` storage buffer (that needs the non-portable VERTEX_WRITABLE_STORAGE
// feature). Splitting the module lets the same buffer be bound read_write to the
// compute and read-only to the draw, which is the portable pattern and the one a
// production implementation should use.

struct Params {
    view_proj: mat4x4<f32>,
    cam: vec4<f32>,
    fwd: vec4<f32>,
    right: vec4<f32>,
    up: vec4<f32>,
    world: vec4<f32>,
    cfg: vec4<f32>,
    sun: vec4<f32>,
};

struct Instance {
    pos: vec3<f32>,
    height: f32,
    seed: f32,
    _pad: vec3<f32>,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read> instances: array<Instance>;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) shade: vec3<f32>,
    @location(2) dist: f32,
    @location(3) seed: f32,
};

@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
    var corner = array<vec2<f32>, 6>(
        vec2<f32>(-0.5, 0.0), vec2<f32>(0.5, 0.0), vec2<f32>(-0.5, 1.0),
        vec2<f32>(0.5, 0.0), vec2<f32>(0.5, 1.0), vec2<f32>(-0.5, 1.0));
    let c = corner[vi];
    let inst = instances[ii];

    // Camera-facing about the world up axis: a cylindrical billboard, which is what
    // a distant tree wants (a spherical one would lean as the camera pitches).
    let to_cam = normalize(vec3<f32>(P.cam.x - inst.pos.x, 0.0, P.cam.z - inst.pos.z));
    let side = normalize(cross(vec3<f32>(0.0, 1.0, 0.0), to_cam));
    let w = inst.height * 0.62;
    let world = inst.pos + side * (c.x * w) + vec3<f32>(0.0, c.y * inst.height, 0.0);
    let rel = world - P.cam.xyz;

    var o: VsOut;
    o.clip = P.view_proj * vec4<f32>(rel, 1.0);
    o.uv = vec2<f32>(c.x + 0.5, c.y);
    o.dist = length(rel);
    o.seed = inst.seed;
    let tint = 0.8 + 0.4 * inst.seed;
    o.shade = vec3<f32>(0.10, 0.24, 0.09) * tint;
    return o;
}

@fragment
fn fs(i: VsOut) -> @location(0) vec4<f32> {
    // Analytic conifer silhouette; a real implementation samples an impostor atlas,
    // but the cost being measured here is placement + rasterisation, not texturing.
    let x = (i.uv.x - 0.5) * 2.0;
    let y = i.uv.y;
    let width = mix(1.0, 0.06, pow(y, 0.75));
    let n = fract(sin((i.uv.x * 37.0 + y * 91.0 + i.seed * 53.0)) * 43758.5453) * 0.30;
    if (abs(x) > width * (1.0 - n) || y > 0.985) {
        discard;
    }
    let lit = 0.45 + 0.55 * (1.0 - abs(x)) * (0.4 + 0.6 * y);
    var col = i.shade * lit;
    let fog = 1.0 - exp(-i.dist / 26000.0);
    col = mix(col, vec3<f32>(0.72, 0.80, 0.90) * 1.02, clamp(fog, 0.0, 1.0));
    return vec4<f32>(pow(col, vec3<f32>(1.0 / 2.2)), 1.0);
}
