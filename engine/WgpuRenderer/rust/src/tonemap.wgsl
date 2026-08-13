// Fullscreen tonemap + colour-grade resolve: sample the linear HDR scene target,
// apply exposure and a fixed Hable filmic curve, then a small colour-grade block
// (white balance, contrast, saturation, lift, gain), optionally sRGB-encode, and
// write the swapchain. See docs/hdr-pipeline-plan.md.
//
// The Hable curve is FIXED (a chosen "film stock"); the per-time-of-day look comes
// from exposure + the grading block, which is what real engines vary. Fed no vertex
// buffer — a single oversized triangle is generated from the vertex index.
//
// `encode` gates the linear->sRGB step: 0 while the pipeline is still gamma-naive,
// 1 once shading is linear. The swapchain is a non-sRGB (Unorm) surface either way,
// so no hardware encode runs and the gamma-space 2D UI drawn after stays correct.

// Live-tunable via the ImGui Tonemap tab (plumbed through wgr_set_tonemap). Layout
// matches WgrTonemap on the C side (12 f32).
struct Params {
    exposure: f32,    // linear pre-curve multiplier (the main per-ToD lever)
    mode: f32,        // 0 = passthrough (clamp), 1 = Hable filmic
    encode: f32,      // 0 = write as-is, 1 = linear->sRGB encode
    temperature: f32, // white balance warm(+)/cool(-)
    tint: f32,        // white balance magenta(+)/green(-)
    contrast: f32,    // post-curve contrast about mid grey (1 = neutral)
    saturation: f32,  // post-curve saturation (1 = neutral)
    lift: f32,        // shadow lift / black-point raise (0 = neutral)
    gain: f32,        // post-curve overall multiply (1 = neutral)
    bloom_intensity: f32, // linear weight of the bloom pyramid added to the scene
    bloom_threshold: f32, // (used by the bloom passes; unused here)
    bloom_knee: f32,      // (used by the bloom passes; unused here)
};

@group(0) @binding(0) var hdr_tex: texture_2d<f32>;
@group(0) @binding(1) var hdr_samp: sampler;
@group(0) @binding(2) var<uniform> params: Params;
// Bloom pyramid mip0 (linear), bilinearly upsampled to full res and added to the
// scene before exposure. A 1x1 black texture is bound when bloom is disabled.
@group(0) @binding(3) var bloom_tex: texture_2d<f32>;
@group(0) @binding(4) var bloom_samp: sampler;
// 1x1 auto-exposure scale from the eye-adaptation module (1.0 when disabled).
@group(0) @binding(5) var exposure_tex: texture_2d<f32>;

// Fixed Hable / Uncharted 2 filmic curve (original constants).
const HABLE_A: f32 = 0.15;
const HABLE_B: f32 = 0.50;
const HABLE_C: f32 = 0.10;
const HABLE_D: f32 = 0.20;
const HABLE_E: f32 = 0.02;
const HABLE_F: f32 = 0.30;
const HABLE_W: f32 = 11.2;
const LUMA: vec3<f32> = vec3<f32>(0.2126, 0.7152, 0.0722);

// Highlight desaturation ("roll to white"). The Hable curve below is applied PER CHANNEL, so a
// bright saturated colour has its strong channels compressed onto the shoulder while its weak
// channel is still on the near-linear part -- which INCREASES saturation at exactly the luminance
// where a real film stock loses it. Measured on Stratis: authored yellow lichen on breakwater rock
// enters at scene-linear (2.84, 1.57, 0.35), leaves the curve at (0.98, 0.83, 0.41), and reads as
// a glowing orange ball instead of a blown highlight.
//
// This blends the post-curve colour toward its own PEAK CHANNEL as that peak approaches white, so
// bright saturated things go white rather than neon. Toward the peak, not toward luma, so
// brightness is preserved and only chroma is removed.
//
// Strictly bounded: nothing whose post-curve peak is below `desat_start` is touched AT ALL, so the
// whole shadow and midtone range stays bit-identical and only the top stop moves. strength 0 =
// disabled, and that is the default -- this is a look change and must be opted in.
// Driven from tonemap.rs via pipeline constants (WGR_TONEMAP_DESAT).
override desat_start: f32 = 0.75;
override desat_strength: f32 = 0.0;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VsOut {
    let uv = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    var out: VsOut;
    out.uv = uv;
    out.clip = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
    return out;
}

fn hable_partial(x: vec3<f32>) -> vec3<f32> {
    return ((x * (HABLE_A * x + HABLE_C * HABLE_B) + HABLE_D * HABLE_E)
            / (x * (HABLE_A * x + HABLE_B) + HABLE_D * HABLE_F)) - HABLE_E / HABLE_F;
}

fn hable(color: vec3<f32>) -> vec3<f32> {
    let white_scale = vec3<f32>(1.0) / hable_partial(vec3<f32>(HABLE_W));
    return hable_partial(color) * white_scale;
}

// Cheap channel-gain white balance (linear, pre-curve). Warm boosts R / cuts B;
// tint trades green vs magenta. Gentle scale so the [-1,1] knobs stay usable.
fn white_balance(c: vec3<f32>, temp: f32, tint: f32) -> vec3<f32> {
    return vec3<f32>(c.r * (1.0 + temp * 0.2), c.g * (1.0 - tint * 0.2), c.b * (1.0 - temp * 0.2));
}

fn linear_to_srgb(c: vec3<f32>) -> vec3<f32> {
    let lo = c * 12.92;
    let hi = 1.055 * pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3<f32>(0.0031308));
}

// Interleaved gradient noise (Jimenez) — ~1 LSB dither to break up 8-bit banding
// in smooth gradients (the sky especially) before the swapchain write.
fn ign(p: vec2<f32>) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2<f32>(0.06711056, 0.00583715))));
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    var color = textureSampleLevel(hdr_tex, hdr_samp, in.uv, 0.0).rgb;

    // Add bloom to the linear scene before exposure (bloom is scene-referred light
    // spread by the pyramid; exposure + curve then act on the sum).
    let bloom = textureSampleLevel(bloom_tex, bloom_samp, in.uv, 0.0).rgb;
    color += bloom * params.bloom_intensity;

    // Scene-referred (linear): exposure (x auto-exposure scale) + white balance.
    let auto_exp = textureLoad(exposure_tex, vec2<i32>(0, 0), 0).r;
    color *= params.exposure * auto_exp;
    color = white_balance(color, params.temperature, params.tint);

    // Fixed filmic curve (or debug passthrough).
    if (params.mode > 0.5) {
        color = hable(color);
    } else {
        color = clamp(color, vec3<f32>(0.0), vec3<f32>(1.0));
    }

    // Roll-to-white, BEFORE the display-referred grade block: the grade is tuned against the
    // curve's output and should keep seeing a well-formed highlight. Note the grade's saturation
    // partially undoes this by design -- re-tune the two together rather than fighting them.
    if (desat_strength > 0.0) {
        let peak = max(color.r, max(color.g, color.b));
        let w = clamp((peak - desat_start) / max(1.0 - desat_start, 1e-4), 0.0, 1.0);
        color = mix(color, vec3<f32>(peak), w * desat_strength);
    }

    // Display-referred grading: contrast about mid grey, saturation, shadow lift,
    // overall gain. lift raises darks more than brights (1 - color weight).
    color = (color - vec3<f32>(0.5)) * params.contrast + vec3<f32>(0.5);
    let luma = dot(color, LUMA);
    color = mix(vec3<f32>(luma), color, params.saturation);
    color = color + params.lift * (1.0 - color);
    color *= params.gain;

    color = clamp(color, vec3<f32>(0.0), vec3<f32>(1.0));
    if (params.encode > 0.5) {
        color = linear_to_srgb(color);
    }
    // Dither in display space (~1 LSB) so smooth gradients don't band on the 8-bit
    // swapchain. in.clip.xy is the framebuffer pixel coordinate.
    color += (ign(in.clip.xy) - 0.5) / 255.0;
    return vec4<f32>(color, 1.0);
}
