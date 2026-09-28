//! Tidewater Native — multi-cascade FFT ocean, ported from dgreenheck/tidewater
//! `src/ocean/OceanFFT.js` @ 4811ba48 (MIT, © DRG Software Solutions LLC).
//!
//! Source-faithful: the same Horvath/JONSWAP + TMA spectrum, the same two fused dispatches per
//! frame (row pass: evolve + packed row IFFT; column pass: column IFFT + sign + Jacobian foam +
//! texture write), the same 256² rgba16float layout and channel meanings:
//!
//!   displacement layer c = (Dx, Dy, Dz, foam)
//!   derivatives  layer c = (dDy/dx, dDy/dz, dDx/dx, dDz/dz)
//!
//! Adaptations forced by OP, all of them mechanical:
//! * The WGSL is generated here instead of by the JS module system (the unrolled butterfly
//!   stages and the mip reductions are produced by the same loops as in OceanFFT.js).
//! * The mip chain's level 5 is written by the second kernel instead of the first, so neither
//!   kernel binds more than four storage textures (the WebGPU default limit OP runs with;
//!   Tidewater's first kernel binds five).
//! * `frame.dt` / `ocean.time` come from OP's water clock (`WgrWaterParams::time`), so a paused
//!   simulation freezes the sea exactly as it freezes Current OP.

use crate::ffi::WgrWaterParams;

pub const FFT_SIZE: u32 = 256;
pub const CASCADES: u32 = 4;
const N: u32 = FFT_SIZE;
const HALF: u32 = N / 2;
const LOG2N: u32 = 8;
const MIP_LEVELS: u32 = 9; // 256 .. 1
pub const GRAVITY: f32 = 9.81;
/// Tidewater's DEFAULT_CASCADE_SIZES. Non-integer ratios avoid visible repetition.
pub const DEFAULT_CASCADE_SIZES: [f32; 4] = [733.0, 157.0, 33.3, 7.1];

/// One wave system (Tidewater `WaveSystem`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WaveSystem {
    pub scale: f32,
    pub wind_speed: f32,
    pub wind_direction_deg: f32,
    pub fetch_km: f32,
    pub spread_blend: f32,
    pub swell: f32,
    pub peak_enhancement: f32,
    pub short_waves_fade: f32,
}

impl WaveSystem {
    /// Tidewater's `local` system (App.js via OceanFFT defaults).
    pub const LOCAL: WaveSystem = WaveSystem {
        scale: 1.0,
        wind_speed: 7.0,
        wind_direction_deg: 25.0,
        fetch_km: 120.0,
        spread_blend: 0.85,
        swell: 0.05,
        peak_enhancement: 3.3,
        short_waves_fade: 0.01,
    };
    /// Tidewater's `swell` system.
    pub const SWELL: WaveSystem = WaveSystem {
        scale: 0.48,
        wind_speed: 6.0,
        wind_direction_deg: 5.0,
        fetch_km: 1200.0,
        spread_blend: 1.0,
        swell: 0.9,
        peak_enhancement: 3.3,
        short_waves_fade: 0.1,
    };

    /// (sysA, sysB) exactly as `updateSpectrumUniforms` packs them.
    pub fn packed(&self) -> ([f32; 4], [f32; 4]) {
        let fetch_m = self.fetch_km.max(1.0) * 1000.0;
        let u = self.wind_speed.max(0.1);
        let alpha = 0.076 * (GRAVITY * fetch_m / (u * u)).powf(-0.22);
        let peak_omega = 22.0 * (u * fetch_m / (GRAVITY * GRAVITY)).powf(-0.33);
        (
            [self.scale, self.wind_direction_deg.to_radians(), self.spread_blend, self.swell],
            [alpha, peak_omega, self.peak_enhancement, self.short_waves_fade],
        )
    }
}

/// `OceanParams` (Tidewater's uniform block), 240 bytes. Shared by the kernels and the surface
/// shader, which reads `sizes` and `p0.y` (foamBias).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
pub struct OceanParams {
    pub sizes: [[f32; 4]; 4],
    pub cuts: [[f32; 4]; 4],
    pub sys_a: [[f32; 4]; 2],
    pub sys_b: [[f32; 4]; 2],
    /// choppiness, foamBias, foamGain, foamDecay
    pub p0: [f32; 4],
    /// foamAdd, time, depth, dt
    pub p1: [f32; 4],
    /// x = seed
    pub seed: [u32; 4],
}

const _: () = assert!(std::mem::size_of::<OceanParams>() == 240);

/// The inputs that require a spectrum rebuild when they change (h0 depends on all of them).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpectrumInputs {
    pub sizes: [f32; 4],
    pub local: WaveSystem,
    pub swell: WaveSystem,
    pub depth: f32,
    pub seed: u32,
    /// Jacobian foam threshold (OceanFFT `foamBias`), from the whitecaps control (W3g).
    pub foam_bias: f32,
}

impl SpectrumInputs {
    /// Derive the spectrum from Tidewater's authored sea lanes. Weather changes
    /// surface amplitude only, which is intentionally absent from these inputs.
    pub fn from_params(p: &WgrWaterParams) -> Self {
        let tw = p.tidewater;
        let cascade_scale = if tw[2][0] > 0.0 { tw[2][0] } else { 1.0 };
        let mut sizes = DEFAULT_CASCADE_SIZES;
        for s in &mut sizes {
            *s *= cascade_scale;
        }
        // Direction convention (W3a correction). With Tidewater's evolution h0·e^{iωt} and the
        // e^{+ik·x} inverse transform, the waves of a spectrum peaked at angle a travel toward
        // -(cos a, sin a): the angle is where they come FROM. OP's wind lane is the wind's
        // velocity direction and the Water tab's swell heading is where the swell travels TO, so
        // both are turned by 180° here, and the sea runs downwind. (W2 passed them unturned: its
        // sea ran upwind.)
        let mut local = WaveSystem::LOCAL;
        let wind = [p.fft_wind_sea[0], p.fft_wind_sea[1]];
        if wind[0] != 0.0 || wind[1] != 0.0 {
            local.wind_direction_deg = wind[1].atan2(wind[0]).to_degrees() + 180.0;
        }
        if p.fft_wind_sea[2] > 0.0 {
            local.wind_speed = p.fft_wind_sea[2];
        }
        // W3g — sea conditions (resolved from Tidewater's preset table on the C++ side): a preset
        // brings its own wind; every mode brings the fetch. Zero lanes keep Tidewater's defaults.
        let sea = p.tidewater_sea;
        if sea[0] > 0.0 {
            local.wind_speed = sea[0];
        }
        if sea[1] > 0.0 {
            local.fetch_km = sea[1];
        } else {
            local.fetch_km = wind_fetch_km(local.wind_speed);
        }
        let mut swell = WaveSystem::SWELL;
        swell.scale = tw[0][2].max(0.0);
        swell.wind_direction_deg = tw[0][3] + 180.0;
        // Quantize authored edits to avoid spectrum rebuilds from float jitter.
        let q = |v: f32, step: f32| (v / step).round() * step;
        local.wind_speed = q(local.wind_speed, 0.25);
        local.wind_direction_deg = q(local.wind_direction_deg, 1.0);
        swell.scale = q(swell.scale, 0.01);
        swell.wind_direction_deg = q(swell.wind_direction_deg, 1.0);
        local.fetch_km = q(local.fetch_km, 1.0);
        // Zeroed params (no C++ side yet) read as Tidewater's 0.5.
        let whitecaps = if sea == [0.0; 4] { 0.5 } else { sea[2].clamp(0.0, 1.0) };
        let choppiness = tw[0][1];
        let foam_bias = q(whitecap_bias(sizes, local, swell, 500.0, choppiness, whitecaps), 0.005);
        Self { sizes, local, swell, depth: 500.0, seed: 1337, foam_bias }
    }

    pub fn cuts(&self) -> [[f32; 4]; 4] {
        let two_pi = std::f32::consts::TAU;
        let mut cuts = [[0.0; 4]; 4];
        for i in 0..CASCADES as usize {
            let low = if i == 0 { 0.0001 } else { two_pi / self.sizes[i] * 6.0 };
            let high = if i == CASCADES as usize - 1 { 9999.0 } else { two_pi / self.sizes[i + 1] * 6.0 };
            cuts[i] = [low, high, 0.0, 0.0];
        }
        cuts
    }
}

// ------------------------------------------------------------------ W9c: whitecap coverage

/// The spectrum's height variance (m^2) and mean square slope, integrated on the CPU over the same
/// cascades, cuts and formulas the init kernel uses (`SPECTRUM` below), on every 4th wavenumber of
/// each 256^2 grid (x16 the cell). Cached for the last inputs.
pub fn sea_stats(sizes: [f32; 4], local: WaveSystem, swell: WaveSystem, depth: f32) -> (f32, f32) {
    type Key = ([f32; 4], [f32; 8], [f32; 8], f32);
    static CACHE: std::sync::Mutex<Option<(Key, (f32, f32))>> = std::sync::Mutex::new(None);
    let lanes = |w: &WaveSystem| [w.scale, w.wind_speed, w.wind_direction_deg, w.fetch_km, w.spread_blend, w.swell, w.peak_enhancement, w.short_waves_fade];
    let key: Key = (sizes, lanes(&local), lanes(&swell), depth);
    if let Ok(c) = CACHE.lock() {
        if let Some((k, v)) = *c {
            if k == key {
                return v;
            }
        }
    }
    let g = GRAVITY as f64;
    let d = depth as f64;
    let disp = |k: f64| (k * g * (k * d).min(20.0).tanh()).sqrt();
    let disp_d = |k: f64| {
        let kd = (k * d).min(20.0);
        let ch = kd.cosh();
        g * (d * k / (ch * ch) + kd.tanh()) / disp(k) * 0.5
    };
    let tma = |w: f64| {
        let wh = w * (d / g).sqrt();
        if wh <= 1.0 { wh * wh * 0.5 } else if wh < 2.0 { 1.0 - (2.0 - wh).powi(2) * 0.5 } else { 1.0 }
    };
    let jonswap = |w: f64, a: [f32; 4], b: [f32; 4]| {
        let (alpha, pw, gamma) = (b[0] as f64, b[1] as f64, b[2] as f64);
        let sigma = if w <= pw { 0.07 } else { 0.09 };
        let dd = w - pw;
        let r = (-dd * dd / (sigma * sigma * pw * pw * 2.0)).exp();
        a[0] as f64 * tma(w) * alpha * g * g * w.powi(-5) * (-(pw / w).powi(4) * 1.25).exp() * gamma.abs().powf(r)
    };
    let norm = |s: f64| {
        let (s2, s3, s4) = (s * s, s * s * s, s * s * s * s);
        if s < 5.0 {
            s4 * -0.000564 + s3 * 0.00776 - s2 * 0.044 + s * 0.192 + 0.163
        } else {
            s4 * -4.80e-08 + s3 * 1.07e-05 - s2 * 9.53e-04 + s * 5.90e-02 + 3.93e-01
        }
    };
    let dir = |th: f64, w: f64, a: [f32; 4], b: [f32; 4]| {
        let ratio = w / b[1] as f64;
        let sp = if w > b[1] as f64 { ratio.abs().powf(-2.5) * 9.77 } else { ratio.abs().powf(5.0) * 6.97 };
        let s = sp + ratio.min(20.0).tanh() * 16.0 * (a[3] as f64).powi(2);
        let dt = th - a[1] as f64;
        let cos2s = norm(s) * (dt * 0.5).cos().abs().powf(s * 2.0);
        let ct = dt.cos();
        let base = if ct > 0.0 { ct * ct * (2.0 / std::f64::consts::PI) } else { 0.0 };
        base + (cos2s - base) * a[2] as f64
    };
    let fade = |k: f64, b: [f32; 4]| (-(b[3] as f64).powi(2) * k * k).exp();
    let (a0, b0) = local.packed();
    let (a1, b1) = swell.packed();
    let spec = SpectrumInputs { sizes, local, swell, depth, seed: 0, foam_bias: 0.0 };
    let cuts = spec.cuts();
    let (mut var_h, mut mss) = (0.0f64, 0.0f64);
    const STEP: i32 = 4;
    for c in 0..CASCADES as usize {
        let dk = std::f64::consts::TAU / sizes[c] as f64;
        for y in (0..N as i32).step_by(STEP as usize) {
            for x in (0..N as i32).step_by(STEP as usize) {
                let kx = (x - HALF as i32) as f64 * dk;
                let kz = (y - HALF as i32) as f64 * dk;
                let k = (kx * kx + kz * kz).sqrt();
                if k < cuts[c][0] as f64 || k > cuts[c][1] as f64 {
                    continue;
                }
                let w = disp(k);
                let th = kz.atan2(kx);
                let s = (jonswap(w, a0, b0) * dir(th, w, a0, b0) * fade(k, b0) + jonswap(w, a1, b1) * dir(th, w, a1, b1) * fade(k, b1)).max(0.0);
                // var(height) = sum S |dw/dk| / k dk^2 (the init kernel's amplitude, squared x 2)
                let e = s * disp_d(k).abs() / k * dk * dk * (STEP * STEP) as f64;
                var_h += e;
                mss += e * k * k;
            }
        }
    }
    let v = (var_h as f32, mss as f32);
    if let Ok(mut c) = CACHE.lock() {
        *c = Some((key, v));
    }
    v
}

/// Standard normal quantile (Acklam's rational approximation, |error| < 1.2e-9).
fn normal_quantile(p: f64) -> f64 {
    let p = p.clamp(1e-9, 1.0 - 1e-9);
    const A: [f64; 6] = [-3.969683028665376e1, 2.209460984245205e2, -2.759285104469687e2, 1.383577518672690e2, -3.066479806614716e1, 2.506628277459239];
    const B: [f64; 5] = [-5.447609879822406e1, 1.615858368580409e2, -1.556989798598866e2, 6.680131188771972e1, -1.328068155288572e1];
    const C: [f64; 6] = [-7.784894002430293e-3, -3.223964580411365e-1, -2.400758277161838, -2.549732539343734, 4.374664141464968, 2.938163982698783];
    const D: [f64; 4] = [7.784695709041462e-3, 3.224671290700398e-1, 2.445134137142996, 3.754408661907416];
    let pl = 0.02425;
    if p < pl {
        let q = (-2.0 * p.ln()).sqrt();
        (((((C[0] * q + C[1]) * q + C[2]) * q + C[3]) * q + C[4]) * q + C[5]) / ((((D[0] * q + D[1]) * q + D[2]) * q + D[3]) * q + 1.0)
    } else if p > 1.0 - pl {
        let q = (-2.0 * (1.0 - p).ln()).sqrt();
        -(((((C[0] * q + C[1]) * q + C[2]) * q + C[3]) * q + C[4]) * q + C[5]) / ((((D[0] * q + D[1]) * q + D[2]) * q + D[3]) * q + 1.0)
    } else {
        let q = p - 0.5;
        let r = q * q;
        (((((A[0] * r + A[1]) * r + A[2]) * r + A[3]) * r + A[4]) * r + A[5]) * q / (((((B[0] * r + B[1]) * r + B[2]) * r + B[3]) * r + B[4]) * r + 1.0)
    }
}

/// Standard normal CDF (Abramowitz-Stegun 26.2.17 via erfc, |error| < 7.5e-8).
fn normal_cdf(z: f64) -> f64 {
    let t = 1.0 / (1.0 + 0.2316419 * z.abs());
    let d = 0.3989422804014327 * (-z * z * 0.5).exp();
    let p = d * t * (0.319381530 + t * (-0.356563782 + t * (1.781477937 + t * (-1.821255978 + t * 1.330274429))));
    if z >= 0.0 { 1.0 - p } else { p }
}

/// W9c (OP): the fetch (km) for an authored wind the sea-conditions lane brings no fetch with,
/// as Tidewater's own presets pair them (Breezy 7 m/s / 120 km, Choppy
/// 12 / 300, Storm 20 / 900: ~2.3 U^2), from its 120 km default up to 600 km. A strong wind then
/// raises the sea, not only its foam: at 30 m/s the local sea's Hs goes from ~6 m (120 km) to
/// ~7.6 m, at 15 m/s from ~3.6 m to ~6.5 m. (Above ~20 m/s the 733 m cascade holds the peak and the
/// height levels off, as a fully risen sea's does.)
pub fn wind_fetch_km(wind_speed: f32) -> f32 {
    (2.3 * wind_speed * wind_speed).clamp(120.0, 600.0)
}

/// W9c (OP): the whitecap growth with the wind, relative to Tidewater's look at 7 m/s: x (U/7)^3.4
/// below it, x (U/7)^2 above, at most x4 (reached at 14 m/s). Measured whitecap cover grows ~U^3.4 (Monahan and
/// O'Muircheartaigh 1980) and a hurricane sea is white, but in a game the owner asked for high
/// wind to show as wave height, not as a sea turned to foam.
pub const WHITECAP_GROWTH_MAX: f32 = 4.0;

/// W9c (OP): the Jacobian foam threshold (OceanFFT `foamBias`) that gives the whitecap cover a set
/// share of the surface. The foam kernel foams where the horizontal Jacobian J falls below the
/// bias; J ~ 1 + choppiness x (divergence of the horizontal displacement), whose spread is
/// choppiness x sqrt(mean square slope). Tidewater's fixed bias (0.58 at its defaults, plus 0.01
/// per m/s of wind) foamed a share of the sea that grew with the slope spread itself: at 20-30 m/s
/// most of J fell below it and the sea turned white while the waves barely looked bigger. Here
/// the bias keeps the share Tidewater's 0.58 gives at its 7 m/s defaults, times the growth above,
/// times 2^(2 (whitecaps - 0.5)) for the Water tab's whitecaps control (0.5 = Tidewater's).
pub fn whitecap_bias(sizes: [f32; 4], local: WaveSystem, swell: WaveSystem, depth: f32, choppiness: f32, whitecaps: f32) -> f32 {
    let chop = choppiness.max(0.05) as f64;
    // Tidewater's reference: its defaults, bias 0.58
    static REF: std::sync::OnceLock<f64> = std::sync::OnceLock::new();
    let p_ref = *REF.get_or_init(|| {
        let (_, mss) = sea_stats(DEFAULT_CASCADE_SIZES, WaveSystem::LOCAL, WaveSystem::SWELL, 500.0);
        let sigma = 0.9 * (mss as f64).sqrt();
        normal_cdf((0.58 - 1.0) / sigma.max(1e-4))
    });
    let (_, mss) = sea_stats(sizes, local, swell, depth);
    let sigma = chop * (mss as f64).sqrt();
    let u = local.wind_speed.max(0.1) as f64;
    // below 7 m/s the measured cover's own U^3.4 (whitecaps fade out in a light breeze), above it
    // (U/7)^2 up to the cap
    let growth = if u < 7.0 { (u / 7.0).powf(3.41) } else { ((u / 7.0) * (u / 7.0)).min(WHITECAP_GROWTH_MAX as f64) };
    let control = 2f64.powf(2.0 * (whitecaps.clamp(0.0, 1.0) as f64 - 0.5));
    let share = (p_ref * growth * control).clamp(1e-6, 0.5);
    (1.0 + sigma * normal_quantile(share)) as f32
}

/// Build the uniform for this frame.
pub fn ocean_params(spec: &SpectrumInputs, choppiness: f32, time: f32, dt: f32) -> OceanParams {
    let (a0, b0) = spec.local.packed();
    let (a1, b1) = spec.swell.packed();
    let mut sizes = [[0.0; 4]; 4];
    for i in 0..4 {
        sizes[i] = [spec.sizes[i], 0.0, 0.0, 0.0];
    }
    OceanParams {
        sizes,
        cuts: spec.cuts(),
        sys_a: [a0, a1],
        sys_b: [b0, b1],
        // Tidewater's foam constants (OceanFFT.js): bias 0.58 (W3g: from the whitecaps control,
        // 0.58 at its default), gain 3, decay 0.35, add 2.5.
        p0: [choppiness, spec.foam_bias, 3.0, 0.35],
        p1: [2.5, time, spec.depth, dt],
        seed: [spec.seed, 0, 0, 0],
    }
}

// ------------------------------------------------------------------ WGSL generation

const OCEAN_STRUCT: &str = r#"
struct OceanParams {
    sizes: array<vec4<f32>, 4>,
    cuts: array<vec4<f32>, 4>,
    sys_a: array<vec4<f32>, 2>,
    sys_b: array<vec4<f32>, 2>,
    p0: vec4<f32>, // choppiness, foamBias, foamGain, foamDecay
    p1: vec4<f32>, // foamAdd, time, depth, dt
    seed: vec4<u32>,
};
const PI: f32 = 3.141592653589793;
const TWO_PI: f32 = 6.283185307179586;
fn sat(x: f32) -> f32 { return clamp(x, 0.0, 1.0); }
"#;

fn fft_common() -> String {
    format!(
        r#"
const FFT_N: u32 = {N}u;
const FFT_HALF: u32 = {HALF}u;
const FFT_G: f32 = {GRAVITY:.2};

fn fftBitReverse8(v: u32) -> u32 {{
    var r = v;
    r = ((r & 0x55u) << 1u) | ((r >> 1u) & 0x55u);
    r = ((r & 0x33u) << 2u) | ((r >> 2u) & 0x33u);
    r = ((r & 0x0Fu) << 4u) | ((r >> 4u) & 0x0Fu);
    return r;
}}
fn fftCmul2(v: vec4<f32>, w: vec2<f32>) -> vec4<f32> {{
    return vec4<f32>(v.x * w.x - v.y * w.y, v.x * w.y + v.y * w.x, v.z * w.x - v.w * w.y, v.z * w.y + v.w * w.x);
}}
fn fftPcg(v: u32) -> u32 {{
    let state = v * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}}
fn fftToUnit(h: u32) -> f32 {{ return f32(h >> 8u) * (1.0 / 16777216.0) + (0.5 / 16777216.0); }}
"#
    )
}

const SPECTRUM: &str = r#"
fn dispersion(k: f32) -> f32 { return sqrt(k * FFT_G * tanh(min(k * ocean.p1.z, 20.0))); }

fn dispersionDerivative(k: f32) -> f32 {
    let kd = min(k * ocean.p1.z, 20.0);
    let th = tanh(kd);
    let ch = cosh(kd);
    return FFT_G * (ocean.p1.z * k / (ch * ch) + th) / dispersion(k) * 0.5;
}

fn tmaCorrection(omega: f32) -> f32 {
    let omegaH = omega * sqrt(ocean.p1.z / FFT_G);
    let a = omegaH * omegaH * 0.5;
    let b = 1.0 - pow(2.0 - omegaH, 2.0) * 0.5;
    return select(select(1.0, b, omegaH < 2.0), a, omegaH <= 1.0);
}

fn jonswap(omega: f32, sysA: vec4<f32>, sysB: vec4<f32>) -> f32 {
    let alpha = sysB.x; let peakOmega = sysB.y; let gamma = sysB.z;
    let sigma = select(0.09, 0.07, omega <= peakOmega);
    let d = omega - peakOmega;
    let r = exp(-d * d / (sigma * sigma * peakOmega * peakOmega * 2.0));
    let inv = 1.0 / omega;
    let po = peakOmega * inv;
    return sysA.x * tmaCorrection(omega) * alpha * (FFT_G * FFT_G)
        * pow(inv, 5.0)
        * exp(pow(po, 4.0) * -1.25)
        * pow(abs(gamma), r);
}

fn normalisationFactor(s: f32) -> f32 {
    let s2 = s * s; let s3 = s2 * s; let s4 = s3 * s;
    let lo = s4 * -0.000564 + s3 * 0.00776 - s2 * 0.044 + s * 0.192 + 0.163;
    let hi = s4 * -4.80e-08 + s3 * 1.07e-05 - s2 * 9.53e-04 + s * 5.90e-02 + 3.93e-01;
    return select(hi, lo, s < 5.0);
}

fn directionSpectrum(theta: f32, omega: f32, sysA: vec4<f32>, sysB: vec4<f32>) -> f32 {
    let peakOmega = sysB.y;
    let ratio = omega / peakOmega;
    let spreadPower = select(pow(abs(ratio), 5.0) * 6.97, pow(abs(ratio), -2.5) * 9.77, omega > peakOmega);
    let s = spreadPower + tanh(min(ratio, 20.0)) * 16.0 * sysA.w * sysA.w;
    let dTheta = theta - sysA.y;
    let cos2s = normalisationFactor(s) * pow(abs(cos(dTheta * 0.5)), s * 2.0);
    let cosT = cos(dTheta);
    let base = cosT * cosT * (2.0 / PI) * select(0.0, 1.0, cosT > 0.0);
    return mix(base, cos2s, sysA.z);
}

fn shortWavesFade(k: f32, sysB: vec4<f32>) -> f32 { return exp(-sysB.w * sysB.w * k * k); }
"#;

fn header_uniform_and(storage: &[(&str, &str)]) -> String {
    let mut s = String::from(OCEAN_STRUCT);
    s.push_str("@group(0) @binding(0) var<uniform> ocean: OceanParams;\n");
    for (i, (name, ty)) in storage.iter().enumerate() {
        s.push_str(&format!(
            "@group(0) @binding({}) var<storage, read_write> {}: {};\n",
            i + 1,
            name,
            ty
        ));
    }
    s
}

pub fn wgsl_init_spectrum() -> String {
    let nn = N * N;
    format!(
        "{}{}{}{}",
        header_uniform_and(&[("h0", "array<vec4<f32>>"), ("waveData", "array<vec4<f32>>")]),
        fft_common(),
        SPECTRUM,
        format!(
            r#"
@compute @workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {{
    let x = i32(gid.x); let y = i32(gid.y); let c = i32(gid.z);
    let idx = c * {nn} + y * {N} + x;

    let L = ocean.sizes[c].x;
    let dk = TWO_PI / L;
    let kx = f32(x - {HALF}) * dk;
    let kz = f32(y - {HALF}) * dk;
    let kLen = length(vec2<f32>(kx, kz));

    var outH = vec4<f32>(0.0);
    var outW = vec4<f32>(kx, kz, 0.0, 0.0);

    if (kLen >= ocean.cuts[c].x && kLen <= ocean.cuts[c].y) {{
        let omega = dispersion(kLen);
        let dOmega = dispersionDerivative(kLen);
        let theta = atan2(kz, kx);

        let sa0 = ocean.sys_a[0]; let sb0 = ocean.sys_b[0];
        let sa1 = ocean.sys_a[1]; let sb1 = ocean.sys_b[1];

        let S0 = jonswap(omega, sa0, sb0) * directionSpectrum(theta, omega, sa0, sb0) * shortWavesFade(kLen, sb0);
        let S1 = jonswap(omega, sa1, sb1) * directionSpectrum(theta, omega, sa1, sb1) * shortWavesFade(kLen, sb1);
        let S = max(S0 + S1, 0.0);

        // E|h0|^2 = S(k) dk^2 / 2 so that var(height) = sum S(k) dk^2
        let amp = sqrt(S * abs(dOmega) / kLen * dk * dk) * 0.5;

        let seed = u32(idx) * 4u + ocean.seed.x * 7919u;
        let u1 = fftToUnit(fftPcg(seed));
        let u2 = fftToUnit(fftPcg(seed + 1u));
        let r = sqrt(log(u1) * -2.0);
        let g0 = r * cos(u2 * TWO_PI);
        let g1 = r * sin(u2 * TWO_PI);

        outH = vec4<f32>(g0 * amp, g1 * amp, 0.0, 0.0);
        outW = vec4<f32>(kx, kz, 1.0 / kLen, omega);
    }}

    h0[idx] = outH;
    waveData[idx] = outW;
}}
"#
        )
    )
}

pub fn wgsl_conjugate() -> String {
    let nn = N * N;
    format!(
        "{}{}",
        header_uniform_and(&[("h0", "array<vec4<f32>>"), ("tmp", "array<vec4<f32>>")]),
        format!(
            r#"
@compute @workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {{
    let x = i32(gid.x); let y = i32(gid.y); let c = i32(gid.z);
    let base = c * {nn};
    let idx = base + y * {N} + x;
    let xm = ({N} - x) % {N};
    let ym = ({N} - y) % {N};
    let idxm = base + ym * {N} + xm;
    let hm = h0[idxm].xy;
    let cur = h0[idx].xy;
    tmp[idx] = vec4<f32>(cur.x, cur.y, hm.x, -hm.y);
    // keep the uniform statically used so all three init kernels share one layout
    if (ocean.seed.w == 0xFFFFFFFFu) {{ tmp[idx] = vec4<f32>(0.0); }}
}}
"#
        )
    )
}

pub fn wgsl_copy_h0() -> String {
    let nn = N * N;
    format!(
        "{}{}",
        header_uniform_and(&[
            ("h0", "array<vec4<f32>>"),
            ("tmp", "array<vec4<f32>>"),
            ("foam", "array<f32>"),
        ]),
        format!(
            r#"
@compute @workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {{
    let idx = gid.z * {nn}u + gid.y * {N}u + gid.x;
    h0[idx] = tmp[idx];
    foam[idx] = 0.0;
    if (ocean.seed.w == 0xFFFFFFFFu) {{ foam[idx] = 1.0; }}
}}
"#
        )
    )
}

fn butterfly_stages() -> String {
    let mut stages = String::new();
    for s in 0..LOG2N {
        let half = 1u32 << s;
        let ang = std::f64::consts::PI / half as f64;
        stages.push_str(&format!(
            r#"
    {{
        let pos = t & {hm1}u;
        let i = ((t >> {s}u) << {s1}u) | pos;
        let j = i + {half}u;
        let ang = f32(pos) * {ang:.12};
        let w = vec2<f32>(cos(ang), sin(ang));
        let i2 = i * 2u; let j2 = j * 2u;
        let a0 = fftShared[i2]; let a1 = fftShared[i2 + 1u];
        let b0 = fftCmul2(fftShared[j2], w); let b1 = fftCmul2(fftShared[j2 + 1u], w);
        fftShared[i2] = a0 + b0;
        fftShared[i2 + 1u] = a1 + b1;
        fftShared[j2] = a0 - b0;
        fftShared[j2 + 1u] = a1 - b1;
        workgroupBarrier();
    }}"#,
            hm1 = half - 1,
            s = s,
            s1 = s + 1,
            half = half,
            ang = ang
        ));
    }
    stages
}

pub fn wgsl_rows() -> String {
    let nn = N * N;
    format!(
        "{}{}var<workgroup> fftShared: array<vec4<f32>, {n2}>;\n{}",
        header_uniform_and(&[
            ("h0", "array<vec4<f32>>"),
            ("waveData", "array<vec4<f32>>"),
            ("tmp", "array<vec4<f32>>"),
        ]),
        fft_common(),
        format!(
            r#"
@compute @workgroup_size({HALF}, 1, 1)
fn main(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {{
    let t = lid.x;
    let row = wid.x;
    let c = wid.y;
    let base = c * {nn}u + row * {N}u;
    let time = ocean.p1.y;

    for (var e = 0u; e < 2u; e++) {{
        let x = t + e * FFT_HALF;
        let idx = base + x;
        let w = waveData[idx];
        let hv = h0[idx];
        let ph = w.w * time;
        let cs = cos(ph); let sn = sin(ph);

        // h = h0 * e^(i w t) + conj(h0(-k)) * e^(-i w t)
        let hr = hv.x * cs - hv.y * sn + hv.z * cs + hv.w * sn;
        let hi = hv.x * sn + hv.y * cs - hv.z * sn + hv.w * cs;

        let kx = w.x; let kz = w.y; let ik = w.z;
        let fx = kx * ik; let fz = kz * ik;

        let c0 = vec2<f32>(-(fx * hi + fz * hr), fx * hr - fz * hi);
        let q = -(kx * kz * ik);
        let c1 = vec2<f32>(hr - q * hi, hi + q * hr);
        let c2 = vec2<f32>(-(kx * hi + kz * hr), kx * hr - kz * hi);
        let a = -(kx * kx * ik); let b = -(kz * kz * ik);
        let c3 = vec2<f32>(a * hr - b * hi, a * hi + b * hr);

        let r = fftBitReverse8(x) * 2u;
        fftShared[r] = vec4<f32>(c0, c1);
        fftShared[r + 1u] = vec4<f32>(c2, c3);
    }}

    workgroupBarrier();
{stages}

    for (var e = 0u; e < 2u; e++) {{
        let x = t + e * FFT_HALF;
        let o = (base + x) * 2u;
        tmp[o] = fftShared[x * 2u];
        tmp[o + 1u] = fftShared[x * 2u + 1u];
    }}
}}
"#,
            stages = butterfly_stages()
        ),
        n2 = N * 2
    )
}

pub fn wgsl_columns() -> String {
    let nn = N * N;
    let mut s = header_uniform_and(&[
        ("tmp", "array<vec4<f32>>"),
        ("foam", "array<f32>"),
        ("mipSrc", "array<vec4<f32>>"),
    ]);
    s.push_str("@group(0) @binding(4) var dispOut: texture_storage_2d_array<rgba16float, write>;\n");
    s.push_str("@group(0) @binding(5) var derivOut: texture_storage_2d_array<rgba16float, write>;\n");
    s.push_str(&fft_common());
    s.push_str(&format!("var<workgroup> fftShared: array<vec4<f32>, {}>;\n", N * 2));
    s.push_str(&format!(
        r#"
@compute @workgroup_size({HALF}, 1, 1)
fn main(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {{
    let t = lid.x;
    let col = wid.x;
    let c = wid.y;
    let base = c * {nn}u;

    for (var e = 0u; e < 2u; e++) {{
        let y = t + e * FFT_HALF;
        let idx = base + y * {N}u + col;
        let r = fftBitReverse8(y) * 2u;
        fftShared[r] = tmp[idx * 2u];
        fftShared[r + 1u] = tmp[idx * 2u + 1u];
    }}

    workgroupBarrier();
{stages}

    let lambda = ocean.p0.x;
    for (var e = 0u; e < 2u; e++) {{
        let y = t + e * FFT_HALF;
        let idx = base + y * {N}u + col;
        let sign = select(-1.0, 1.0, ((col + y) & 1u) == 0u);
        let A = fftShared[y * 2u] * sign;
        let B = fftShared[y * 2u + 1u] * sign;

        let Dx = A.x; let Dz = A.y; let Dy = A.z; let Dxz = A.w;
        let Dyx = B.x; let Dyz = B.y; let Dxx = B.z; let Dzz = B.w;

        let jxx = lambda * Dxx + 1.0;
        let jzz = lambda * Dzz + 1.0;
        let jxz = lambda * Dxz;
        let J = jxx * jzz - jxz * jxz;

        // Persistent foam: generated where the surface compresses (J < bias), then decays.
        let prev = foam[idx];
        let gen = sat((ocean.p0.y - J) * ocean.p0.z);
        let f = prev * exp(-ocean.p0.w * ocean.p1.w) + gen * ocean.p1.x * ocean.p1.w;
        let fNew = clamp(max(f, gen * 0.5), 0.0, 1.5);
        foam[idx] = fNew;

        let uv = vec2<u32>(col, y);
        let vDisp = vec4<f32>(lambda * Dx, Dy, lambda * Dz, fNew);
        let vDeriv = vec4<f32>(Dyx, Dyz, lambda * Dxx, lambda * Dzz);
        textureStore(dispOut, uv, c, vDisp);
        textureStore(derivOut, uv, c, vDeriv);
        mipSrc[idx * 2u] = vDisp;
        mipSrc[idx * 2u + 1u] = vDeriv;
    }}
}}
"#,
        stages = butterfly_stages()
    ));
    s
}

/// `reduce( from, to, width, lvl, t )` from OceanFFT.js. `to == None` at the last level of a
/// kernel; `to_mid` writes the 8x8 level-5 result of kernel A into `mipMid`.
fn reduce(from: &str, to: Option<&str>, width: u32, lvl: u32, t: u32, to_mid: bool) -> String {
    let w2 = width * 2;
    let dst = match to {
        Some(to) => format!("{to}[ly * {width}u + lx] = v;"),
        None if to_mid => format!("mipMid[(c * 64u + gy * 8u + gx) * 2u + {t}u] = v;"),
        None => String::new(),
    };
    let store = if to_mid {
        String::new()
    } else {
        format!("textureStore(out{lvl}, vec2<u32>(gx * {width}u + lx, gy * {width}u + ly), c, v);")
    };
    format!(
        r#"
    if (lx < {width}u && ly < {width}u) {{
        let i = ly * {row}u + lx * 2u;
        let v = ({from}[i] + {from}[i + 1u] + {from}[i + {w2}u] + {from}[i + {w2p1}u]) * 0.25;
        {store}
        {dst}
    }}"#,
        row = 2 * w2,
        w2p1 = w2 + 1
    )
}

/// Kernel A: levels 1..4 of a 32x32 tile of level 0, plus the tile's level-5 texel into
/// `mipMid` (Tidewater also stores it to level 5 here; kernel B does that in this port).
pub fn wgsl_mips_a(t: u32) -> String {
    let nn = N * N;
    let mut s = String::from(
        "@group(0) @binding(0) var<storage, read_write> mipSrc: array<vec4<f32>>;\n\
         @group(0) @binding(1) var<storage, read_write> mipMid: array<vec4<f32>>;\n",
    );
    for l in 1..=4u32 {
        s.push_str(&format!(
            "@group(0) @binding({}) var out{l}: texture_storage_2d_array<rgba16float, write>;\n",
            l + 1
        ));
    }
    s.push_str(&format!(
        r#"
var<workgroup> s1: array<vec4<f32>, 256>;
var<workgroup> s2: array<vec4<f32>, 64>;
var<workgroup> s3: array<vec4<f32>, 16>;
var<workgroup> s4: array<vec4<f32>, 4>;
fn src(c: u32, x: u32, y: u32) -> vec4<f32> {{ return mipSrc[(c * {nn}u + y * {N}u + x) * 2u + {t}u]; }}
@compute @workgroup_size(16, 16, 1)
fn main(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {{
    let lx = lid.x; let ly = lid.y;
    let gx = wid.x; let gy = wid.y; let c = wid.z;
    let x1 = gx * 16u + lx; let y1 = gy * 16u + ly;
    let x0 = x1 * 2u; let y0 = y1 * 2u;
    let v1 = (src(c, x0, y0) + src(c, x0 + 1u, y0) + src(c, x0, y0 + 1u) + src(c, x0 + 1u, y0 + 1u)) * 0.25;
    textureStore(out1, vec2<u32>(x1, y1), c, v1);
    s1[ly * 16u + lx] = v1;
    workgroupBarrier();
{r2}
    workgroupBarrier();
{r3}
    workgroupBarrier();
{r4}
    workgroupBarrier();
{r5}
}}
"#,
        r2 = reduce("s1", Some("s2"), 8, 2, t, false),
        r3 = reduce("s2", Some("s3"), 4, 3, t, false),
        r4 = reduce("s3", Some("s4"), 2, 4, t, false),
        r5 = reduce("s4", None, 1, 5, t, true),
    ));
    s
}

/// Kernel B: level 5 (from `mipMid`) and levels 6..8, one 8x8 workgroup per layer.
pub fn wgsl_mips_b(t: u32) -> String {
    let mut s = String::from("@group(0) @binding(0) var<storage, read_write> mipMid: array<vec4<f32>>;\n");
    for l in 5..=8u32 {
        s.push_str(&format!(
            "@group(0) @binding({}) var out{l}: texture_storage_2d_array<rgba16float, write>;\n",
            l - 4
        ));
    }
    s.push_str(&format!(
        r#"
var<workgroup> s5: array<vec4<f32>, 64>;
var<workgroup> s6: array<vec4<f32>, 16>;
var<workgroup> s7: array<vec4<f32>, 4>;
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {{
    let lx = lid.x; let ly = lid.y; let c = wid.z;
    let gx = 0u; let gy = 0u;
    let v5 = mipMid[(c * 64u + ly * 8u + lx) * 2u + {t}u];
    textureStore(out5, vec2<u32>(lx, ly), c, v5);
    s5[ly * 8u + lx] = v5;
    workgroupBarrier();
{r6}
    workgroupBarrier();
{r7}
    workgroupBarrier();
{r8}
}}
"#,
        r6 = reduce("s5", Some("s6"), 4, 6, t, false),
        r7 = reduce("s6", Some("s7"), 2, 7, t, false),
        r8 = reduce("s7", None, 1, 8, t, false),
    ));
    s
}

// ------------------------------------------------------------------ GPU resources

struct Kernel {
    pipeline: wgpu::ComputePipeline,
    bind: wgpu::BindGroup,
}

fn kernel(
    device: &wgpu::Device,
    label: &str,
    source: String,
    entries: &[wgpu::BindGroupEntry<'_>],
) -> Kernel {
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some(label),
        source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some(label),
        layout: None,
        module: &module,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });
    let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(label),
        layout: &pipeline.get_bind_group_layout(0),
        entries,
    });
    Kernel { pipeline, bind }
}

pub struct OceanFft {
    params: wgpu::Buffer,
    // Kept alive for the bind groups; never read on the CPU.
    _buffers: Vec<wgpu::Buffer>,
    displacement: wgpu::Texture,
    derivatives: wgpu::Texture,
    displacement_view: wgpu::TextureView,
    derivatives_view: wgpu::TextureView,
    init: Kernel,
    conjugate: Kernel,
    copy_h0: Kernel,
    rows: Kernel,
    columns: Kernel,
    mips_a: [Kernel; 2],
    mips_b: [Kernel; 2],
    spectrum: Option<SpectrumInputs>,
    needs_spectrum: bool,
    current: OceanParams,
}

fn storage(device: &wgpu::Device, label: &str, bytes: u64) -> wgpu::Buffer {
    device.create_buffer(&wgpu::BufferDescriptor {
        label: Some(label),
        size: bytes,
        usage: wgpu::BufferUsages::STORAGE,
        mapped_at_creation: false,
    })
}

fn level_view(tex: &wgpu::Texture, level: u32) -> wgpu::TextureView {
    tex.create_view(&wgpu::TextureViewDescriptor {
        label: Some("tw_ocean_level"),
        dimension: Some(wgpu::TextureViewDimension::D2Array),
        base_mip_level: level,
        mip_level_count: Some(1),
        ..Default::default()
    })
}

impl OceanFft {
    pub fn new(device: &wgpu::Device) -> Self {
        let total = (N * N * CASCADES) as u64;
        let v4 = 16u64;
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_ocean_params"),
            size: std::mem::size_of::<OceanParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let h0 = storage(device, "tw_fft_h0", total * v4);
        let wave = storage(device, "tw_fft_wave", total * v4);
        let tmp = storage(device, "tw_fft_tmp", total * 2 * v4);
        let foam = storage(device, "tw_fft_foam", total * 4);
        let mip_src = storage(device, "tw_fft_mip_src", total * 2 * v4);
        let mip_mid = storage(device, "tw_fft_mip_mid", 64 * CASCADES as u64 * 2 * v4);

        let make_tex = |label: &str| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d { width: N, height: N, depth_or_array_layers: CASCADES },
                mip_level_count: MIP_LEVELS,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                view_formats: &[],
            })
        };
        let displacement = make_tex("tw_ocean_displacement");
        let derivatives = make_tex("tw_ocean_derivatives");
        let full = |t: &wgpu::Texture| {
            t.create_view(&wgpu::TextureViewDescriptor {
                label: Some("tw_ocean_sampled"),
                dimension: Some(wgpu::TextureViewDimension::D2Array),
                ..Default::default()
            })
        };
        let displacement_view = full(&displacement);
        let derivatives_view = full(&derivatives);

        fn ub(b: u32, buf: &wgpu::Buffer) -> wgpu::BindGroupEntry<'_> {
            wgpu::BindGroupEntry { binding: b, resource: buf.as_entire_binding() }
        }
        let init = kernel(
            device,
            "tw_ocean_init_spectrum",
            wgsl_init_spectrum(),
            &[ub(0, &params), ub(1, &h0), ub(2, &wave)],
        );
        let conjugate = kernel(
            device,
            "tw_ocean_conjugate",
            wgsl_conjugate(),
            &[ub(0, &params), ub(1, &h0), ub(2, &tmp)],
        );
        let copy_h0 = kernel(
            device,
            "tw_ocean_copy_h0",
            wgsl_copy_h0(),
            &[ub(0, &params), ub(1, &h0), ub(2, &tmp), ub(3, &foam)],
        );
        let rows = kernel(
            device,
            "tw_ocean_fft_rows",
            wgsl_rows(),
            &[ub(0, &params), ub(1, &h0), ub(2, &wave), ub(3, &tmp)],
        );
        let d0 = level_view(&displacement, 0);
        let v0 = level_view(&derivatives, 0);
        let columns = kernel(
            device,
            "tw_ocean_fft_columns",
            wgsl_columns(),
            &[
                ub(0, &params),
                ub(1, &tmp),
                ub(2, &foam),
                ub(3, &mip_src),
                wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(&d0) },
                wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(&v0) },
            ],
        );
        let mips = |tex: &wgpu::Texture, t: u32| {
            let a_views: Vec<_> = (1..=4).map(|l| level_view(tex, l)).collect();
            let mut a_entries = vec![ub(0, &mip_src), ub(1, &mip_mid)];
            for (i, v) in a_views.iter().enumerate() {
                a_entries.push(wgpu::BindGroupEntry {
                    binding: i as u32 + 2,
                    resource: wgpu::BindingResource::TextureView(v),
                });
            }
            let a = kernel(device, "tw_ocean_mips_a", wgsl_mips_a(t), &a_entries);
            let b_views: Vec<_> = (5..=8).map(|l| level_view(tex, l)).collect();
            let mut b_entries = vec![ub(0, &mip_mid)];
            for (i, v) in b_views.iter().enumerate() {
                b_entries.push(wgpu::BindGroupEntry {
                    binding: i as u32 + 1,
                    resource: wgpu::BindingResource::TextureView(v),
                });
            }
            let b = kernel(device, "tw_ocean_mips_b", wgsl_mips_b(t), &b_entries);
            (a, b)
        };
        let (da, db) = mips(&displacement, 0);
        let (va, vb) = mips(&derivatives, 1);

        Self {
            params,
            _buffers: vec![h0, wave, tmp, foam, mip_src, mip_mid],
            displacement,
            derivatives,
            displacement_view,
            derivatives_view,
            init,
            conjugate,
            copy_h0,
            rows,
            columns,
            mips_a: [da, va],
            mips_b: [db, vb],
            spectrum: None,
            needs_spectrum: true,
            current: bytemuck::Zeroable::zeroed(),
        }
    }

    pub fn displacement_view(&self) -> &wgpu::TextureView {
        &self.displacement_view
    }

    pub fn derivatives_view(&self) -> &wgpu::TextureView {
        &self.derivatives_view
    }

    pub fn params_buffer(&self) -> &wgpu::Buffer {
        &self.params
    }

    pub fn current(&self) -> &OceanParams {
        &self.current
    }

    /// W9c: the spectrum inputs in use (for the log).
    pub fn spectrum(&self) -> Option<SpectrumInputs> {
        self.spectrum
    }

    /// Upload this frame's uniform; a changed spectrum marks h0 for regeneration.
    pub fn set_frame(&mut self, queue: &wgpu::Queue, spec: SpectrumInputs, choppiness: f32, time: f32, dt: f32) {
        if self.spectrum != Some(spec) {
            self.spectrum = Some(spec);
            self.needs_spectrum = true;
        }
        self.current = ocean_params(&spec, choppiness, time, dt);
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&self.current));
    }

    /// `OceanFFT.update`: the spectrum (when dirty) plus the two fused FFT dispatches and the
    /// compute mip chain.
    pub fn dispatch(&mut self, encoder: &mut wgpu::CommandEncoder, timers: &crate::gpu_timers::GpuTimers) {
        use crate::gpu_timers::Region;
        if self.needs_spectrum {
            self.needs_spectrum = false;
            timers.begin(encoder, Region::TwOceanSpectrum);
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("tw_ocean_spectrum"),
                timestamp_writes: None,
            });
            for k in [&self.init, &self.conjugate, &self.copy_h0] {
                pass.set_pipeline(&k.pipeline);
                pass.set_bind_group(0, &k.bind, &[]);
                pass.dispatch_workgroups(N / 16, N / 16, CASCADES);
            }
            drop(pass);
            timers.end(encoder, Region::TwOceanSpectrum);
        }
        timers.begin(encoder, Region::TwOceanFft);
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("tw_ocean_fft"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.rows.pipeline);
        pass.set_bind_group(0, &self.rows.bind, &[]);
        pass.dispatch_workgroups(N, CASCADES, 1);
        pass.set_pipeline(&self.columns.pipeline);
        pass.set_bind_group(0, &self.columns.bind, &[]);
        pass.dispatch_workgroups(N, CASCADES, 1);
        for k in &self.mips_a {
            pass.set_pipeline(&k.pipeline);
            pass.set_bind_group(0, &k.bind, &[]);
            pass.dispatch_workgroups(N / 32, N / 32, CASCADES);
        }
        for k in &self.mips_b {
            pass.set_pipeline(&k.pipeline);
            pass.set_bind_group(0, &k.bind, &[]);
            pass.dispatch_workgroups(1, 1, CASCADES);
        }
        drop(pass);
        timers.end(encoder, Region::TwOceanFft);
    }

    #[allow(dead_code)]
    pub fn textures(&self) -> (&wgpu::Texture, &wgpu::Texture) {
        (&self.displacement, &self.derivatives)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn validate(label: &str, src: &str) {
        let module = naga::front::wgsl::parse_str(src).unwrap_or_else(|e| panic!("{label}: {}", e.emit_to_string(src)));
        let mut v = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all());
        v.validate(&module).unwrap_or_else(|e| panic!("{label}: {e:?}"));
    }

    #[test]
    fn tidewater_fft_kernels_validate() {
        validate("init", &wgsl_init_spectrum());
        validate("conjugate", &wgsl_conjugate());
        validate("copy_h0", &wgsl_copy_h0());
        validate("rows", &wgsl_rows());
        validate("columns", &wgsl_columns());
        for t in 0..2 {
            validate("mips_a", &wgsl_mips_a(t));
            validate("mips_b", &wgsl_mips_b(t));
        }
    }

    #[test]
    fn no_kernel_binds_more_than_four_storage_textures() {
        for src in [wgsl_columns(), wgsl_mips_a(0), wgsl_mips_b(0)] {
            assert!(src.matches("texture_storage_2d_array").count() <= 4);
        }
    }

    #[test]
    fn spectrum_packing_matches_tidewater() {
        // OceanFFT.js updateSpectrumUniforms for the default local system.
        let (a, b) = WaveSystem::LOCAL.packed();
        assert_eq!(a[0], 1.0);
        assert!((a[1] - 25f32.to_radians()).abs() < 1e-6);
        let fetch_m = 120_000.0f32;
        let alpha = 0.076 * (9.81 * fetch_m / 49.0f32).powf(-0.22);
        assert!((b[0] - alpha).abs() < 1e-7);
        let s = SpectrumInputs {
            sizes: DEFAULT_CASCADE_SIZES,
            local: WaveSystem::LOCAL,
            swell: WaveSystem::SWELL,
            depth: 500.0,
            seed: 1337,
            foam_bias: 0.58,
        };
        let cuts = s.cuts();
        assert_eq!(cuts[0][0], 0.0001);
        assert!((cuts[0][1] - std::f32::consts::TAU / 157.0 * 6.0).abs() < 1e-6);
        assert_eq!(cuts[3][1], 9999.0);
    }

    #[test]
    fn directions_are_turned_so_the_sea_runs_downwind() {
        let mut p = <WgrWaterParams as bytemuck::Zeroable>::zeroed();
        p.tidewater = crate::water_tw::DEFAULT_PARAMS;
        p.fft_wind_sea = [1.0, 0.0, 8.0, 0.5]; // wind blowing toward +x
        let s = SpectrumInputs::from_params(&p);
        assert_eq!(s.local.wind_direction_deg, 180.0);
        assert_eq!(s.swell.wind_direction_deg, 185.0); // swell heading 5° (travels toward)
    }

    #[test]
    fn sea_conditions_lane_overrides_wind_fetch_and_foam() {
        let mut p = <WgrWaterParams as bytemuck::Zeroable>::zeroed();
        p.tidewater = crate::water_tw::DEFAULT_PARAMS;
        p.fft_wind_sea = [1.0, 0.0, 6.0, 0.3];
        // zeroed lane: Tidewater's defaults (120 km, foam bias at local 6 m/s)
        let s = SpectrumInputs::from_params(&p);
        assert_eq!(s.local.wind_speed, 6.0);
        assert_eq!(s.local.fetch_km, 120.0);
        // Storm preset: 20 m/s, 900 km, whitecaps 1
        p.tidewater_sea = [20.0, 900.0, 1.0, 4.0];
        let s = SpectrumInputs::from_params(&p);
        assert_eq!(s.local.wind_speed, 20.0);
        assert_eq!(s.local.fetch_km, 900.0);
    }

    #[test]
    fn surface_amplitude_does_not_rebuild_tidewater_spectrum() {
        let mut p = <WgrWaterParams as bytemuck::Zeroable>::zeroed();
        p.tidewater = crate::water_tw::DEFAULT_PARAMS;
        p.tidewater_sea = [7.0, 120.0, 0.5, 0.0];
        let baseline = SpectrumInputs::from_params(&p);
        p.tidewater[0][0] *= 1.6;
        assert_eq!(SpectrumInputs::from_params(&p), baseline);
    }

    #[test]
    fn whitecap_bias_keeps_tidewaters_look_and_bounds_the_foam_in_a_gale() {
        // W9c: Tidewater's own defaults give its own bias
        let b7 = whitecap_bias(DEFAULT_CASCADE_SIZES, WaveSystem::LOCAL, WaveSystem::SWELL, 500.0, 0.9, 0.5);
        assert!((b7 - 0.58).abs() < 0.01, "{b7}");
        // the foamed share: the bias's normal tail over the slope spread
        let share = |w: f32, whitecaps: f32| {
            let mut l = WaveSystem::LOCAL;
            l.wind_speed = w;
            let b = whitecap_bias(DEFAULT_CASCADE_SIZES, l, WaveSystem::SWELL, 500.0, 0.9, whitecaps) as f64;
            let (_, mss) = sea_stats(DEFAULT_CASCADE_SIZES, l, WaveSystem::SWELL, 500.0);
            normal_cdf((b - 1.0) / (0.9 * (mss as f64).sqrt()))
        };
        let s7 = share(7.0, 0.5);
        // more foam in more wind, at most x4 Tidewater's 7 m/s share, however hard it blows
        assert!(share(12.0, 0.5) > s7 * 2.0);
        for w in [14.0, 20.0, 30.0, 40.0] {
            let s = share(w, 0.5);
            assert!(s <= s7 * (WHITECAP_GROWTH_MAX as f64) * 1.02 && s >= s7 * 3.9, "{w}: {s} vs {s7}");
        }
        // the Water tab's whitecaps control: x2 at 1, /2 at 0
        assert!((share(7.0, 1.0) / s7 - 2.0).abs() < 0.05);
        assert!((share(7.0, 0.0) / s7 - 0.5).abs() < 0.02);
        // and the waves do grow: Hs rises with the wind (with the fetch the wind brings)
        let hs = |w: f32| {
            let mut l = WaveSystem::LOCAL;
            l.wind_speed = w;
            l.fetch_km = wind_fetch_km(w);
            4.0 * sea_stats(DEFAULT_CASCADE_SIZES, l, WaveSystem::SWELL, 500.0).0.sqrt()
        };
        assert_eq!(wind_fetch_km(7.0), 120.0);
        assert!(hs(15.0) > hs(7.0) * 1.5 && hs(30.0) > hs(15.0), "{} {} {}", hs(7.0), hs(15.0), hs(30.0));
    }

    #[test]
    fn normal_quantile_inverts_the_cdf() {
        for p in [1e-5, 0.001, 0.02, 0.2, 0.5, 0.8, 0.99] {
            assert!((normal_cdf(normal_quantile(p)) - p).abs() < 1e-6 + p * 1e-4, "{p}");
        }
    }

    #[test]
    fn params_block_is_240_bytes_of_vec4() {
        assert_eq!(std::mem::size_of::<OceanParams>() % 16, 0);
        assert_eq!(std::mem::offset_of!(OceanParams, p0), 192);
    }
}
