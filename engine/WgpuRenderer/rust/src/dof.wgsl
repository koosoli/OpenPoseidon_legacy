// Depth of field — a photographic aid, not a simulation.
//
// Runs on the LINEAR HDR scene, after the MSAA resolve and the god-ray composite and before
// bloom, so an out-of-focus highlight blooms as a disc rather than a point. That ordering is the
// whole reason bokeh reads as bokeh.
//
// SCATTER-AS-GATHER, and the honest limits of it. Real defocus SCATTERS: each source pixel throws
// light over a disc. A fragment shader can only GATHER, so this walks a disc around the current
// pixel and accepts a neighbour's colour when that NEIGHBOUR's circle of confusion is wide enough
// to have reached here. That is the standard approximation and it fails in one visible way: a
// sharply focused object in front of a blurred background gets a thin halo of background bleeding
// onto its edge, because the gather cannot know the foreground occludes the scatter. Weighting by
// the neighbour's CoC keeps it to a fringe rather than a smear. A separable or tile-based
// implementation would do better and costs a great deal more plumbing; for screenshots this is
// the right trade.
//
// The gather walks a golden-angle spiral whose start angle is hashed PER PIXEL. The
// hash is not decoration: with a fixed start angle neighbouring pixels sample the same offsets
// and a wide blur reads as coherent streaks.
//
// Reversed-Z with an infinite far plane, so depth is proportional to 1/distance and view distance
// is simply `near / depth`. Cleared depth (the sky) is zero, which would divide by zero, so it is
// treated as maximally distant — the sky belongs at full background blur anyway.

struct Params {
    // x = focus distance (m), y = half-width of the fully sharp band (m),
    // z = maximum circle of confusion in PIXELS, w = camera near plane (m)
    focus: vec4<f32>,
    // x = background blur scale, y = foreground blur scale,
    // z = how quickly CoC opens up beyond the sharp band (1/m), w = debug view
    strength: vec4<f32>,
    // x = samples per pixel, y = bokeh highlight boost, z = highlight threshold,
    // w = aperture blades (0 = perfectly round, 5..9 = polygonal)
    quality: vec4<f32>,
}

@group(0) @binding(0) var scene_colour: texture_2d<f32>;
@group(0) @binding(1) var scene_sampler: sampler;
@group(0) @binding(2) var scene_depth: texture_depth_2d;
@group(0) @binding(3) var<uniform> params: Params;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VsOut {
    // Full-screen triangle, same convention as every other post pass here.
    var out: VsOut;
    let x = f32((vi << 1u) & 2u);
    let y = f32(vi & 2u);
    out.uv = vec2<f32>(x, y);
    out.clip = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    return out;
}

// View distance in metres from a reversed-Z sample. Cleared depth -> very far.
fn view_distance(d: f32) -> f32 {
    if (d <= 1e-7) {
        return 1.0e6;
    }
    return params.focus.w / d;
}

// Signed circle of confusion in pixels. Negative in front of the focal plane, positive behind it,
// because the two sides are scaled independently -- foreground blur is far more intrusive than
// background blur and wants its own control.
fn coc_pixels(distance_m: f32) -> f32 {
    let focus = params.focus.x;
    let band = max(params.focus.y, 0.0);
    let maxCoc = max(params.focus.z, 0.0);
    let opening = max(params.strength.z, 1e-4);

    let delta = distance_m - focus;
    let outside = max(abs(delta) - band, 0.0);
    // Saturating, not linear: without a ceiling a distant mountain would ask for a thousand-pixel
    // disc and the gather would sample a handful of points across it, which reads as noise.
    let magnitude = maxCoc * (1.0 - exp(-outside * opening));
    let scale = select(params.strength.y, params.strength.x, delta > 0.0);
    return select(-magnitude * scale, magnitude * scale, delta > 0.0);
}

const GOLDEN_ANGLE: f32 = 2.39996323;

// How much foreign light landed on this pixel, normalised. A sharp subject in front of a heavily
// blurred background has centre_coc == 0 but a large accumulated weight, and this is what lets
// that background halo appear at all instead of being clipped away.
fn luma(c: vec3<f32>) -> f32 {
    return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

// Radius multiplier for a polygonal aperture at a given angle. A real lens has blades, and out of
// focus they print the APERTURE shape rather than a circle -- which is why photographic bokeh is
// often hexagonal. Fewer than 3 blades keeps the perfect disc a mirror lens would give.
fn aperture_scale(angle: f32, blades: f32) -> f32 {
    if (blades < 3.0) {
        return 1.0;
    }
    let n = floor(blades);
    let seg = 6.28318530718 / n;
    let half_seg = seg * 0.5;
    // Distance from centre to the polygon edge along this angle. Always <= 1, so the polygon
    // inscribes the disc instead of exceeding the radius the circle of confusion asked for.
    let offset = angle - seg * floor(angle / seg) - half_seg;
    return cos(half_seg) / max(cos(offset), 1e-3);
}

fn weight_bleed(weight: f32, count: f32) -> f32 {
    return clamp((weight - 1.0) / max(count, 1.0), 0.0, 1.0);
}


@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let dims = vec2<f32>(textureDimensions(scene_colour));
    let dims_i = vec2<i32>(dims);
    let pixel = clamp(vec2<i32>(in.clip.xy), vec2<i32>(0), dims_i - vec2<i32>(1));

    let centre_colour = textureSampleLevel(scene_colour, scene_sampler, in.uv, 0.0);
    let centre_coc = coc_pixels(view_distance(textureLoad(scene_depth, pixel, 0)));
    let centre_radius = abs(centre_coc);

    // A pixel that is in focus AND has no blurred neighbour reaching it still has to run the
    // gather, because it is the neighbours that decide. Skipping on the centre CoC alone is what
    // produces a hard sharp/blurred seam.
    var accum = centre_colour.rgb;
    var weight = 1.0;

    // Golden-angle spiral: even coverage of the disc at any sample count, no repeating pattern to
    // read as banding, and no lookup table.
    // Samples per pixel, from the Picture Mode quality control. This is the whole cost of the
    // pass: every one is a colour fetch and a depth fetch, so 96 samples at 1080p is ~200 million
    // texture reads a frame. Cheap for a paused screenshot, not for playing.
    let sample_count = clamp(params.quality.x, 4.0, 128.0);
    let samples_i = i32(sample_count);

    var radius = 1.0;
    let radius_step = 1.0 / sample_count;
    // Per-pixel start angle. Without it every pixel walks the SAME spiral, the sample positions
    // line up between neighbours, and a wide blur comes out as coherent streaks rather than
    // smooth defocus -- clearly visible at 27 px on the first working capture. Rotating each
    // pixel's disc turns that structure into fine noise, which the eye forgives completely.
    var angle = fract(sin(dot(in.uv, vec2<f32>(12.9898, 78.233))) * 43758.5453) * 6.28318530718;

    // Search radius is the widest CoC this pixel could plausibly receive. Using the centre's own
    // radius alone would miss a very blurred background bleeding onto a sharp subject, so the
    // search always spans at least the configured maximum when the centre is near-focus.
    let search = max(centre_radius, params.focus.z * 0.5);

    for (var i = 0; i < samples_i; i = i + 1) {
        angle = angle + GOLDEN_ANGLE;
        radius = radius - radius_step;
        let r = sqrt(max(1.0 - radius, 0.0)) * search;
        let offset = vec2<f32>(cos(angle), sin(angle)) * (r * aperture_scale(angle, params.quality.w));
        let sample_uv = clamp(in.uv + offset / dims, vec2<f32>(0.0), vec2<f32>(1.0));
        let sample_pixel = clamp(vec2<i32>(sample_uv * dims), vec2<i32>(0), dims_i - vec2<i32>(1));

        let sample_coc = coc_pixels(view_distance(textureLoad(scene_depth, sample_pixel, 0)));
        // Does THIS neighbour's disc reach the centre pixel? That is the scatter test, evaluated
        // from the gather side. Softened over a pixel so the acceptance edge is not aliased.
        let reach = smoothstep(r - 1.0, r + 1.0, abs(sample_coc));
        if (reach > 0.0) {
            let c = textureSampleLevel(scene_colour, scene_sampler, sample_uv, 0.0).rgb;
            // BOKEH. A flat average turns a bright out-of-focus point into a faint smear: its
            // energy gets divided over the whole disc. A real lens does the opposite -- the point
            // prints as a bright disc of its own. Weighting each sample by how far it exceeds a
            // highlight threshold restores that, and it only works because this pass runs in
            // LINEAR HDR before the tonemap, where a specular or a sun-lit edge genuinely carries
            // a value above 1 instead of having been clipped to white.
            let w = reach * (1.0 + params.quality.y * max(luma(c) - params.quality.z, 0.0));
            accum = accum + c * w;
            weight = weight + w;
        }
    }

    // Debug view (strength.w != 0): paint the circle of confusion instead of the scene. Green is
    // behind the focal plane, red in front, black is in focus. There is no way to tell a broken
    // depth mapping from a weak blur by looking at the composited image, so this exists.
    // Debug view 2 (strength.w > 1.5): raw view distance, 0..200 m as greyscale. Separates "the
    // depth is wrong" from "the CoC formula is wrong", which the CoC view alone cannot.
    if (params.strength.w > 1.5) {
        let z = view_distance(textureLoad(scene_depth, pixel, 0)) / 200.0;
        return vec4<f32>(clamp(z, 0.0, 1.0), clamp(z, 0.0, 1.0), clamp(z, 0.0, 1.0), 1.0);
    }

    if (params.strength.w > 0.5) {
        let n = clamp(centre_radius / max(params.focus.z, 1e-4), 0.0, 1.0);
        let behind = select(0.0, n, centre_coc > 0.0);
        let infront = select(0.0, n, centre_coc < 0.0);
        return vec4<f32>(infront, behind, 0.0, 1.0);
    }
    let blurred = accum / max(weight, 1e-4);
    // Blend the gathered result in by the centre's own defocus, so a genuinely sharp pixel keeps
    // its own colour and only picks up what actually scattered onto it.
    let mix_factor = clamp(centre_radius / max(params.focus.z, 1e-4), 0.0, 1.0);
    let out_rgb = mix(centre_colour.rgb, blurred, max(mix_factor, weight_bleed(weight, sample_count)));
    return vec4<f32>(out_rgb, centre_colour.a);
}
