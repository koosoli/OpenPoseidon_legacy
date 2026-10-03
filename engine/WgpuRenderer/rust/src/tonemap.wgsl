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
    // NV-001. Night vision belongs HERE and not in the lighting: an image intensifier
    // amplifies the image that reached it. The legacy filter multiplied the SUN colour, which
    // at night contributes nothing at all -- and on the sky-lit path it was overwritten a few
    // lines further down in EngineWgpu, so it was computed and thrown away. That is the whole
    // of "the NV are see through / no green just darkness".
    nv_strength: f32,
    nv_gain: f32,
    nv_noise: f32,
    nv_vignette: f32,
    // Sinkhole W1b, renderer-side only (not part of WgrTonemap): x = 1 to despeckle (camera
    // underground), yzw unused.
    cave: vec4<f32>,
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
// Linear-white point of the Hable curve: the post-exposure radiance that maps to display 1.0.
// Above it everything CLIPS. Uncharted's 11.2 was written for scenes whose midtones sit near
// 1-2; here a sunlit Takistan slope arrives at ~6-7 (sky-lit sun 22 x transmittance x albedo,
// x preset exposure 3.625, x the auto-exposure floor 0.25), i.e. already at 0.9 of white -- so
// a house wall at 2-3x the terrain's albedo has nowhere to go but flat white, which is the
// owner's "the sun hits a wall and totally overexposes it". Raising this lengthens the
// shoulder: midtones darken a little, highlights get a stop of room. Driven by
// WGR_TONEMAP_WHITE (tonemap.rs) so the value is settled by a capture, not a rebuild.
override linear_white: f32 = 11.2;

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
    let white_scale = vec3<f32>(1.0) / hable_partial(vec3<f32>(max(linear_white, 1.0)));
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

    // Sinkhole W1b: underground despeckle. A model's pinhole cracks (T-junctions at corners and
    // edges) show the sky or a sunlit surface through a dark cave wall as single bright pixels, and
    // the eye adaptation underground (WGR_CAVE_EXPOSURE_MAX) multiplies them into the owner's
    // "green flashing dots in the corners". Underground only, a pixel more than 3x as bright as the
    // brightest of its eight neighbours takes that neighbour's colour; a real highlight or an
    // opening wider than a pixel has a bright neighbour and is untouched.
    if (params.cave.x > 0.5) {
        let dims = vec2<i32>(textureDimensions(hdr_tex));
        let p = clamp(vec2<i32>(in.uv * vec2<f32>(dims)), vec2<i32>(0), dims - vec2<i32>(1));
        let w = vec3<f32>(0.2126, 0.7152, 0.0722);
        let c = textureLoad(hdr_tex, p, 0).rgb;
        var best = vec3<f32>(0.0);
        var best_l = -1.0;
        for (var dy = -1; dy <= 1; dy = dy + 1) {
            for (var dx = -1; dx <= 1; dx = dx + 1) {
                if (dx == 0 && dy == 0) { continue; }
                let q = clamp(p + vec2<i32>(dx, dy), vec2<i32>(0), dims - vec2<i32>(1));
                let n = textureLoad(hdr_tex, q, 0).rgb;
                let l = dot(n, w);
                if (l > best_l) { best_l = l; best = n; }
            }
        }
        if (dot(c, w) > 3.0 * best_l + 1e-4) {
            color = best;
        }
    }

    // Add bloom to the linear scene before exposure (bloom is scene-referred light
    // spread by the pyramid; exposure + curve then act on the sum).
    let bloom = textureSampleLevel(bloom_tex, bloom_samp, in.uv, 0.0).rgb;
    color += bloom * params.bloom_intensity;

    // NV-001, first half: the gain, applied to LINEAR scene light before the curve, because
    // that is what an intensifier does -- it multiplies the photons that arrived, and the
    // filmic curve then rolls the result off exactly as it does for daylight. Amplifying after
    // the curve instead would just wash a black frame to grey.
    //
    // The bloom pyramid is already summed in above, so a bright source blooms through the same
    // gain and halates the way a real tube does around a lamp. That is free here and would have
    // needed its own pass anywhere else.
    if (params.nv_strength > 0.0) {
        color *= mix(1.0, params.nv_gain, params.nv_strength);
    }

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

    // NV-001, second half: the tube. Display-referred, after the grade, because everything
    // below is what the EYE sees through the eyepiece rather than what the sensor measured.
    if (params.nv_strength > 0.0) {
        // A phosphor screen is monochrome -- it has no idea what colour arrived. Collapse to
        // luminance FIRST; tinting the original RGB instead leaves colour information that the
        // real device cannot have, and it reads as a green filter over a colour picture.
        let l = dot(color, LUMA);
        // P43 phosphor, roughly. Not pure green: a real tube's white point sits slightly warm
        // of the primary, and pure (0,1,0) looks like a colour key rather than a screen.
        let phosphor = vec3<f32>(0.18, 1.0, 0.34);
        var nv = phosphor * l;
        // Sensor grain, scaled by DARKNESS: an intensifier is noisiest where it is working
        // hardest, so a lit doorway is clean and the shadow beside it crawls. Uniform grain
        // over the whole frame is the giveaway of a fake NV filter.
        let grain = (ign(in.clip.xy + vec2<f32>(params.exposure * 977.0, 0.0)) - 0.5);
        nv += phosphor * grain * params.nv_noise * (1.0 - clamp(l, 0.0, 1.0));
        // The eyepiece. Radius from the frame centre in uv, so it follows the aspect.
        let d = distance(in.uv, vec2<f32>(0.5, 0.5)) * 1.4142;
        nv *= 1.0 - params.nv_vignette * smoothstep(0.45, 1.0, d);
        color = mix(color, max(nv, vec3<f32>(0.0)), params.nv_strength);
    }

    color = clamp(color, vec3<f32>(0.0), vec3<f32>(1.0));
    if (params.encode > 0.5) {
        color = linear_to_srgb(color);
    }
    // Dither in display space (~1 LSB) so smooth gradients don't band on the 8-bit
    // swapchain. in.clip.xy is the framebuffer pixel coordinate.
    color += (ign(in.clip.xy) - 0.5) / 255.0;
    return vec4<f32>(color, 1.0);
}

// Opt-in screenshot diagnostic entry only. Production fs_main does not use group1.
struct PostOpticsInput {
    dims: vec4<u32>, origins: array<vec4<u32>,4>,
    request: vec4<u32>, frame: vec4<u32>, source: vec4<u32>,
};
@group(1) @binding(0) var post_meter: texture_2d<f32>;
@group(1) @binding(1) var<uniform> post_probe: PostOpticsInput;
struct PostExposureParams {enabled:f32,key:f32,min_scale:f32,max_scale:f32,rate:f32,sky_weight:f32,pad1:f32,pad2:f32};
@group(1) @binding(2) var<uniform> post_ep: PostExposureParams;
@fragment
fn fs_post_optics_probe(in: VsOut) -> @location(0) vec4<f32> {
    let pixel=u32(in.clip.x); let row=u32(in.clip.y);
    if(row<16u){
        let tile=row/4u; let channel=row%4u;
        let xy=post_probe.origins[tile].xy+vec2<u32>(pixel%5u,pixel/5u);
        let uv=(vec2<f32>(xy)+vec2<f32>(0.5))/vec2<f32>(post_probe.dims.xy);
        let hdr=textureSampleLevel(hdr_tex,hdr_samp,uv,0.0).rgb;
        let bloom=textureSampleLevel(bloom_tex,bloom_samp,uv,0.0).rgb;
        let scale=textureLoad(exposure_tex,vec2<i32>(0),0).r;
        let precurve=white_balance((hdr+bloom*params.bloom_intensity)*params.exposure*scale,params.temperature,params.tint);
        if(channel==0u){return vec4<f32>(hdr,1.0);}
        if(channel==1u){return vec4<f32>(bloom,1.0);}
        if(channel==2u){return vec4<f32>(precurve,1.0);}
        if(params.mode>0.5){return vec4<f32>(hable(precurve),1.0);}
        return vec4<f32>(clamp(precurve,vec3<f32>(0),vec3<f32>(1)),1.0);
    }
    if(row==16u){return vec4<f32>(textureLoad(post_meter,vec2<i32>(0),0).rg,textureLoad(exposure_tex,vec2<i32>(0),0).r,linear_white);}
    if(row==17u){return vec4<f32>(params.exposure,params.mode,params.encode,params.temperature);}
    if(row==18u){return vec4<f32>(params.tint,params.contrast,params.saturation,params.lift);}
    if(row==19u){return vec4<f32>(params.gain,params.bloom_intensity,params.bloom_threshold,params.bloom_knee);}
    if(row==20u){return vec4<f32>(params.nv_strength,params.nv_gain,params.nv_noise,params.nv_vignette);}
    if(row==21u){return vec4<f32>(desat_start,desat_strength,vec2<f32>(textureDimensions(hdr_tex)));}
    if(row==22u){return vec4<f32>(post_probe.request);}
    if(row==23u){return vec4<f32>(post_probe.frame);}
    if(row==24u){return vec4<f32>(post_probe.source);}
    if(row==25u){return vec4<f32>(post_ep.enabled,post_ep.key,post_ep.min_scale,post_ep.max_scale);}
    if(row==26u){return vec4<f32>(post_ep.rate,post_ep.sky_weight,post_ep.pad1,post_ep.pad2);}
    return vec4<f32>(vec2<f32>(textureDimensions(bloom_tex)),vec2<f32>(post_probe.dims.xy));
}
