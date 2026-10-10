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
};

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
    let r = vel[p].w * 1.5 * fade;
    if (q.w < 0.0 || r <= 0.001) {
        o.clip = vec4<f32>(2.0, 2.0, 2.0, 1.0); // outside the clip volume: nothing drawn
        return o;
    }
    let centre = q.xyz - D.cam.xyz;
    let corner = centre + (D.right.xyz * c.x + D.up.xyz * c.y) * r;
    o.clip = D.view_proj * vec4<f32>(corner, 1.0);
    o.uv = c;
    o.centre = centre;
    o.radius = r;
    return o;
}

struct FOut {
    @location(0) colour: vec4<f32>,
    @builtin(frag_depth) depth: f32,
};

@fragment
fn fs_mud(in: VOut) -> FOut {
    let d2 = dot(in.uv, in.uv);
    if (d2 > 1.0) { discard; }
    let toCam = normalize(-in.centre);
    let n = normalize(D.right.xyz * in.uv.x + D.up.xyz * in.uv.y + toCam * sqrt(1.0 - d2));
    let surface = in.centre + n * in.radius;
    let clip = D.view_proj * vec4<f32>(surface, 1.0);
    let depth = clip.z / clip.w;
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
    let colour = D.albedo.rgb * (D.sun_radiance.rgb * ndl + D.ambient.rgb * sky) + D.sun_radiance.rgb * spec +
                 D.ambient.rgb * fres;
    var o: FOut;
    o.colour = vec4<f32>(colour, 1.0);
    o.depth = depth;
    return o;
}
