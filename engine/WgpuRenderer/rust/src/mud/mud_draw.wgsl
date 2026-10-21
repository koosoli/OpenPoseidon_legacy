// MUD-001: the mud particles drawn as lit, wet spheres (impostors: a camera-facing quad per
// particle, the sphere's surface, normal and depth in the fragment). Drawn into the HDR scene
// after the opaque world against its resolved depth (tested here, as the fire march does), with
// a depth target of their own so the blobs occlude each other; slightly larger than the solver's
// radius so neighbouring blobs merge into one gooey mass.

struct DrawParams {
    view_proj: mat4x4<f32>, // camera-relative
    cam: vec4<f32>,         // camera world position, w: render width
    right: vec4<f32>,       // camera right (world), w: render height
    up: vec4<f32>,          // camera up (world), w: fade-out seconds at the end of life
    sun_dir: vec4<f32>,     // toward the sun, w: life (s)
    sun_radiance: vec4<f32>,
    ambient: vec4<f32>,
    albedo: vec4<f32>,      // linear, w: wetness (specular)
};

@group(0) @binding(0) var<uniform> D: DrawParams;
@group(0) @binding(1) var<storage, read> pos: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> vel: array<vec4<f32>>;
@group(0) @binding(3) var sceneDepth: texture_depth_2d;

struct VOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) centre: vec3<f32>,
    @location(2) radius: f32,
    // per-blob random: x size, y/z outline lobe phases, w shade
    @location(3) @interpolate(flat) rnd: vec4<f32>,
};

fn mud_hash(n: u32) -> f32 {
    var x = n * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    x = (x >> 22u) ^ x;
    return f32(x) / 4294967295.0;
}

@vertex
fn vs_mud(@builtin(vertex_index) vi: u32) -> VOut {
    var o: VOut;
    let p = vi / 6u;
    let corners = array<vec2<f32>, 6>(vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0),
                                      vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, 1.0), vec2<f32>(-1.0, 1.0));
    let c = corners[vi % 6u];
    let q = pos[p];
    let fadeFor = max(D.up.w, 0.01);
    let fade = clamp((D.sun_dir.w - q.w) / fadeFor, 0.0, 1.0);
    // drawn half again the solver's spread so neighbouring blobs overlap into one lumpy mass
    // instead of a string of beads
    // (2026-10-14) every blob its own size, outline and shade, and stretched along its flight on
    // screen: identical lit spheres read as "brown balls" (owner), not as flung mud
    let rnd = vec4<f32>(mud_hash(p * 4u), mud_hash(p * 4u + 1u), mud_hash(p * 4u + 2u), mud_hash(p * 4u + 3u));
    let r = vel[p].w * 1.5 * fade * mix(0.4, 1.3, rnd.x * rnd.x);
    if (q.w < 0.0 || r <= 0.001) {
        o.clip = vec4<f32>(2.0, 2.0, 2.0, 1.0); // outside the clip volume: nothing drawn
        return o;
    }
    let centre = q.xyz - D.cam.xyz;
    let v = vel[p].xyz;
    let vs = vec2<f32>(dot(v, D.right.xyz), dot(v, D.up.xyz));
    let speed = length(vs);
    let axis = select(vec2<f32>(1.0, 0.0), vs / max(speed, 1e-4), speed > 1e-3);
    let stretch = 1.0 + clamp(speed * 0.08, 0.0, 1.6);
    let side = vec2<f32>(-axis.y, axis.x);
    let s2 = axis * dot(c, axis) * stretch + side * dot(c, side);
    let corner = centre + (D.right.xyz * s2.x + D.up.xyz * s2.y) * r;
    o.clip = D.view_proj * vec4<f32>(corner, 1.0);
    o.uv = c;
    o.centre = centre;
    o.radius = r;
    o.rnd = rnd;
    return o;
}

struct FOut {
    @location(0) colour: vec4<f32>,
    @builtin(frag_depth) depth: f32,
};

@fragment
fn fs_mud(in: VOut) -> FOut {
    // a lumpy outline, not a disc: two lobe frequencies with this blob's own phases
    let ang = atan2(in.uv.y, in.uv.x);
    let edge = 1.0 - 0.22 * (0.5 + 0.5 * sin(ang * 3.0 + in.rnd.y * 6.2832))
                   - 0.14 * (0.5 + 0.5 * sin(ang * 5.0 + in.rnd.z * 6.2832));
    let uvn = in.uv / edge;
    let d2 = dot(uvn, uvn);
    if (d2 > 1.0) { discard; }
    let toCam = normalize(-in.centre);
    let n = normalize(D.right.xyz * uvn.x + D.up.xyz * uvn.y + toCam * sqrt(1.0 - d2));
    let surface = in.centre + n * in.radius;
    let clip = D.view_proj * vec4<f32>(surface, 1.0);
    // D.view_proj is the CPU's forward-Z projection; the raster (and the scene depth read below) is
    // reversed-Z. Compared raw, no blob was ever behind anything -- mud drew over the truck that threw
    // it (owner, 2026-10-14) -- and blobs sorted far-over-near among themselves.
    let depth = 1.0 - clip.z / clip.w;
    // the scene in front (reversed-Z: nearer is larger)
    let scene = textureLoad(sceneDepth, vec2<i32>(in.clip.xy), 0);
    if (scene > depth) { discard; }
    // wet mud: a dark diffuse body under a thin glossy film
    let l = normalize(D.sun_dir.xyz);
    let ndl = max(dot(n, l), 0.0);
    let hv = normalize(l + toCam);
    let spec = pow(max(dot(n, hv), 0.0), 60.0) * D.albedo.w;
    let fres = pow(1.0 - max(dot(n, toCam), 0.0), 4.0) * 0.06 * D.albedo.w;
    let sky = 0.6 + 0.4 * n.y;
    let colour = D.albedo.rgb * mix(0.7, 1.25, in.rnd.w) * (D.sun_radiance.rgb * ndl + D.ambient.rgb * sky) + D.sun_radiance.rgb * spec +
                 D.ambient.rgb * fres;
    var o: FOut;
    o.colour = vec4<f32>(colour, 1.0);
    o.depth = depth;
    return o;
}
