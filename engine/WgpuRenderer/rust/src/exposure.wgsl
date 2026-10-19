// Eye adaptation / auto-exposure. A luminance pyramid reduces the linear HDR scene
// to a 1x1 average log-luminance (each halving is a single linear tap = a 2x2 box
// average); the adapt pass then eases a persistent EXPOSURE SCALE toward
// key/avgLuminance. The tonemap multiplies its exposure by that scale, so no exposure
// params live in the tonemap. Disabled -> the scale eases back to 1.0 (a no-op).
//
// Spatial metering: the first reduction weights each pixel so the bright sky (top of
// frame) doesn't drag the average up and over-darken the ground. Each texel carries
// (weighted log-luminance, weight); the pyramid averages both channels and the adapt
// pass divides R/G to recover the weighted mean. B/A carry the highlight cap (fs_lum_first),
// so the pyramid is Rgba16Float.

const LUMA: vec3<f32> = vec3<f32>(0.2126, 0.7152, 0.0722);

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

// Field order MUST match ffi::WgrExposure (enabled first) — a mismatch silently
// transposes the bytes: rate gets read as enabled and the whole thing sticks at 1.0.
// Shared by the first reduction (spatial weight) and the adapt pass, both at binding 2.
struct ExpParams {
    enabled: f32,    // 0 = ease back to 1.0 (auto-exposure off)
    key: f32,        // target middle-grey luminance (higher = brighter auto-exposure)
    min_scale: f32,  // clamp on the exposure multiplier
    max_scale: f32,
    rate: f32,       // per-frame ease toward the target (0..1)
    sky_weight: f32, // metering weight of the top of frame (sky) vs bottom (ground)
    // RFG-150: the share of a metered brightness change the iris compensates (1 = all of it,
    // key/avg; <= 0 also means 1). Native Enfusion worlds use 0.55 -- see EngineWgpu.
    compensation: f32,
    meter_mode: f32, // 1 = Reforger's centre-weighted mask; else sky_weight by screen height, and
                     // < 0 carries -(tonemap exposure) for the highlight cap (stock worlds)
};

// ---- Reduction (src + sampler + params at group 0) ---------------------------
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var src_samp: sampler;
@group(0) @binding(2) var<uniform> ep: ExpParams;
@group(0) @binding(3) var reduce_prev: texture_2d<f32>; // 1x1 last frame's exposure scale

// Metering weight by vertical screen position: uv.y = 0 is the top of the frame (sky,
// down-weighted toward ep.sky_weight), uv.y = 1 is the bottom (ground, full weight).
// A screen-space heuristic — cheap, and the horizon usually sits in the upper half.
fn meter_weight(uv: vec2<f32>) -> f32 {
    if (ep.meter_mode > 0.5) {
        // RFG-150: Reforger's HDR.emat meters through HDR_MeasureArea_01_Mask.edds, a radially
        // symmetric fall-off stretched over the frame: 1.0 out to r = 0.4, 0.74 at the edge
        // midpoints, 0.30 in the corners (r = 1 at an edge midpoint). This curve fits the
        // decoded mask to 0.012 everywhere. It does NOT down-weight the sky.
        let r = length((uv - vec2<f32>(0.5)) * 2.0);
        let t = max(r - 0.35, 0.0) / 1.05;
        return max(1.0 - 0.7 * t * t, 0.05);
    }
    return mix(ep.sky_weight, 1.0, smoothstep(0.2, 0.8, uv.y));
}

// First step: scene colour -> (weighted log2 luminance, weight). The linear tap has
// already averaged the underlying 2x2 scene texels for the luminance sample.
@fragment
fn fs_lum_first(in: VsOut) -> @location(0) vec4<f32> {
    let c = textureSampleLevel(src, src_samp, in.uv, 0.0).rgb;
    let lum = max(dot(max(c, vec3<f32>(0.0)), LUMA), 1e-5);
    let w = meter_weight(in.uv);
    return vec4<f32>(log2(lum) * w, w, 0.0, 0.0);
}

// Subsequent steps: average both channels (the linear tap = 2x2 mean of R and G).
@fragment
fn fs_lum_down(in: VsOut) -> @location(0) vec4<f32> {
    return textureSampleLevel(src, src_samp, in.uv, 0.0);
}

// ---- Adapt (1x1: current weighted avg + previous scale + params) -------------
@group(0) @binding(0) var lum_tex: texture_2d<f32>;  // 1x1 (sum wlog, sum w)
@group(0) @binding(1) var prev_tex: texture_2d<f32>; // 1x1 previous exposure scale
// ep is declared above at binding 2 and reused here.

// BOOM-006: the share of the picture allowed to clip (sampled every 4th pixel by the tonemap),
// and the counters' layout: CLIP_SLOTS clipped counts, then CLIP_SLOTS sampled counts.
const CLIP_ALLOWED: f32 = 0.015;
const CLIP_SLOTS: u32 = 64u;
@group(0) @binding(4) var<storage, read> clip_counts: array<u32, 128>;

@fragment
fn fs_adapt(in: VsOut) -> @location(0) vec4<f32> {
    let full = textureLoad(lum_tex, vec2<i32>(0, 0), 0);
    let acc = full.rg;
    let avg_lum = exp2(acc.r / max(acc.g, 1e-5));
    var prev = textureLoad(prev_tex, vec2<i32>(0, 0), 0).r;
    // Guard uninitialised / NaN history so the loop can't get stuck off-screen.
    if (!(prev > 0.0)) {
        prev = 1.0;
    }
    var tgt = 1.0;
    if (ep.enabled > 0.5) {
        var raw = ep.key / max(avg_lum, 1e-4);
        if (ep.compensation > 0.0 && ep.compensation != 1.0) {
            raw = ep.key * pow(max(avg_lum, 1e-4), -ep.compensation);
        }
        // BOOM-006 (owner, 2026-10-19: "ground and buildings must never get overexposed -- that
        // is what auto exposure is for"; "if i look into the sun it even gets brighter"). The
        // log average cannot see clipping: a backlit frame meters dark and opened the iris
        // into the sun. The tonemap counts, every 4th pixel, how many leave it burnt out (any
        // channel >= 0.97 on screen, after bloom and the grade). While more than CLIP_ALLOWED
        // of the picture clips, the iris closes -- a sunlit wall, snow, the glare round the sun
        // alike; between half that and CLIP_ALLOWED it may not open further (no hunting).
        var clipped = 0u;
        var sampled = 0u;
        for (var i = 0u; i < CLIP_SLOTS; i++) {
            clipped += clip_counts[i];
            sampled += clip_counts[CLIP_SLOTS + i];
        }
        if (sampled > 64u) {
            let frac = f32(clipped) / f32(sampled);
            if (frac > CLIP_ALLOWED) {
                raw = min(raw, prev * exp2(-min((frac - CLIP_ALLOWED) * 12.0, 1.5)));
            } else if (frac > 0.5 * CLIP_ALLOWED) {
                raw = min(raw, prev);
            }
        }
        tgt = clamp(raw, ep.min_scale, ep.max_scale);
    }
    // closing the iris on a bright view runs twice as fast as opening it again
    let rate = select(ep.rate, min(ep.rate * 2.0, 1.0), tgt < prev);
    let scale = mix(prev, tgt, clamp(rate, 0.0, 1.0));
    return vec4<f32>(scale, 0.0, 0.0, 1.0);
}
