// Tidewater Native W6 — the interactive boat wake (WakeSim.js), ported from dgreenheck/tidewater
// @ 4811ba48 (MIT, © DRG Software Solutions LLC). Compute kernels; the readers are in
// tw_wake.wgsl. See wake.rs for what OP changes.
//
// A linear free-surface wave simulation with exact dispersion: height h and vertical velocity w on
// a 512^2 grid (0.4 m cells, 205 m) in a window that follows one boat, addressed toroidally (world
// cell I lives in texel I mod N). Three dispatches per step:
//   1. wake_rows:    per-cell physics (hull pressure, dissipation, breaking, foam), display
//                    texture, forward FFT along x
//   2. wake_columns: forward FFT along z, the four depth-blended spectral operators, inverse FFT
//                    along z (spare workgroups update the boat's near-field template)
//   3. wake_inverse: inverse FFT along x, symplectic Euler step, absorbing sponge
// Waves: dw/dt = -g L[h + P], dh/dt = w, L = |k| tanh(|k| d); P is the hull's pressure head.
//
// Prepended at build time (wake.rs): tw_common.wgsl (sat, PI, ConformParams, terrainHeightAt).

const WAKE_N: u32 = 512u;
const WAKE_HALF: u32 = 256u;
const WAKE_MASK: u32 = 511u;
const WAKE_LOG2N: u32 = 9u;
const WAKE_CELL: f32 = 0.4;
const WAKE_SIZE: f32 = 204.8;
const WAKE_TW: u32 = 32u;
const WAKE_TH: u32 = 88u;
const WAKE_SPONGE: f32 = 22.0;
const WAKE_GRAVITY: f32 = 9.81;
// spectral step of the window, 2 pi / SIZE
const WAKE_DK: f32 = 0.030679615757712823;
// 1 / N^2 (the unnormalized forward + inverse transforms)
const WAKE_NORM: f32 = 3.814697265625e-6;
// template workgroups after the columns: ceil(TW * TH / HALF)
const WAKE_NEAR_GROUPS: u32 = 11u;

// WakeKernel (wake.rs `WakeKernel`): all vec4 lanes
struct WakeKernel {
    win: vec4<f32>,   // origin (min corner, cells) xy, previous origin zw
    boat: vec4<f32>,  // boat position xz (m), cos / sin yaw
    trim: vec4<f32>,  // immersion change c + a x + b z (xyz), reset (w)
    m0: vec4<f32>,    // dt, amount, source gain, wash
    m1: vec4<f32>,    // bow, speed, wash half width, boil
    m2: vec4<f32>,    // visc, dynamic head, hollow length, hollow gain
    m3: vec4<f32>,    // foam gain, aeration gain, near-template rate, sea level
    hull: vec4<f32>,  // hull map: X1 (|x| extent), Z0, Z1 - Z0, zAft (transom)
    near: vec4<f32>,  // template box: TX0, TZ0, TX1 - TX0, TZ1 - TZ0
    m4: vec4<f32>,    // time, unused x3
};

@group(0) @binding(0) var<uniform> wk: WakeKernel;
@group(0) @binding(1) var<storage, read_write> wakeState: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write> wakeScratch: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> wakeSpec: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read_write> wakeAerA: array<f32>;
@group(0) @binding(5) var<storage, read_write> wakeAerB: array<f32>;
@group(0) @binding(6) var wakeHullTex: texture_2d<f32>;
@group(0) @binding(7) var smpLinearClamp: sampler;
@group(0) @binding(8) var wakeDisplayOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(9) var wakeAerOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(10) var wakeDisplayTex: texture_2d<f32>;
@group(0) @binding(11) var smpLinearRepeat: sampler;
@group(0) @binding(12) var<storage, read_write> wakeNearBuf: array<f32>;
@group(0) @binding(13) var wakeNearOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(14) var seabed_heightmap: texture_2d<f32>;
@group(0) @binding(15) var<uniform> cf: ConformParams;

// integer lattice hash -> [0, 1), smooth value noise
fn wakeHash(ix: i32, iy: i32) -> f32 {
    var v = (u32(ix) * 0x8da6b343u) ^ (u32(iy) * 0xd8163841u);
    v = (v ^ (v >> 13u)) * 0x5bd1e995u;
    v = v ^ (v >> 15u);
    return f32(v >> 8u) * (1.0 / 16777216.0);
}

fn wakeNoise(q: vec2<f32>) -> f32 {
    let i = floor(q);
    let fr = fract(q);
    let u = fr * fr * (3.0 - 2.0 * fr);
    let ix = i32(i.x); let iy = i32(i.y);
    let a = wakeHash(ix, iy); let b = wakeHash(ix + 1, iy);
    let c = wakeHash(ix, iy + 1); let d = wakeHash(ix + 1, iy + 1);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

fn wakeBrev(v: u32) -> u32 { return reverseBits(v) >> (32u - WAKE_LOG2N); }

// toroidal texel -> world cell index in the window starting at o
fn wakeWorldIndex(i: i32, o: i32) -> i32 { return o + ((i - o) & i32(WAKE_MASK)); }
fn wakeInWindow(I: i32, J: i32, o: vec2<f32>) -> bool {
    return I >= i32(o.x) && I < i32(o.x) + i32(WAKE_N) && J >= i32(o.y) && J < i32(o.y) + i32(WAKE_N);
}

// state of texel (c, r) if it held the same world cell last step, else zero
fn wakeLoadState(c: u32, r: u32) -> vec4<f32> {
    let I = wakeWorldIndex(i32(c), i32(wk.win.x));
    let J = wakeWorldIndex(i32(r), i32(wk.win.y));
    let keep = wakeInWindow(I, J, wk.win.zw) && wk.trim.w < 0.5;
    return select(vec4<f32>(0.0), wakeState[r * WAKE_N + c], keep);
}

fn wakeLoadAer(c: u32, r: u32) -> f32 {
    let I = wakeWorldIndex(i32(c), i32(wk.win.x));
    let J = wakeWorldIndex(i32(r), i32(wk.win.y));
    let keep = wakeInWindow(I, J, wk.win.zw) && wk.trim.w < 0.5;
    return select(0.0, wakeAerA[r * WAKE_N + c], keep);
}

fn wakeCellPos(c: u32, r: u32) -> vec2<f32> {
    let I = wakeWorldIndex(i32(c), i32(wk.win.x));
    let J = wakeWorldIndex(i32(r), i32(wk.win.y));
    return vec2<f32>(f32(I) + 0.5, f32(J) + 0.5) * WAKE_CELL;
}

// sqrt of the blend weights of the four operators (deep, 6, 1.8, 0.6 m) for local depth d
fn wakeDepthWeights(d: f32) -> vec4<f32> {
    let t0 = 1.0 - exp((d - 6.0) / -9.0);
    let t1 = pow(sat((d - 1.8) / 4.2), 0.8);
    let t2 = pow(sat((d - 0.6) / 1.2), 0.85);
    let w0 = select(0.0, t0, d > 6.0);
    let w1 = select(select(0.0, t1, d > 1.8), 1.0 - t0, d > 6.0);
    let w2 = select(select(select(0.0, t2, d > 0.6), 1.0 - t1, d > 1.8), 0.0, d > 6.0);
    let w3 = select(select(sat(d / 0.6), 1.0 - t2, d > 0.6), 0.0, d > 1.8);
    return sqrt(max(vec4<f32>(w0, w1, w2, w3), vec4<f32>(0.0)));
}

fn wakeHullUv(x: f32, z: f32) -> vec2<f32> {
    return vec2<f32>(abs(x) / wk.hull.x, (z - wk.hull.y) / wk.hull.z);
}

// hull immersion (m) at boat-frame (x, z): design immersion + scheduled heave / trim
fn wakeImmersionAt(x: f32, z: f32) -> f32 {
    let D = textureSampleLevel(wakeHullTex, smpLinearClamp, wakeHullUv(x, z), 0.0).x;
    return max(D + wk.trim.x + wk.trim.y * x + wk.trim.z * z, 0.0) * smoothstep(0.0, 0.04, D);
}

// radix-2 DIT butterflies (bit-reversed in, natural order out); dir = -1 forward, +1 inverse
fn wakeTwiddle(t: u32, s: u32, dir: f32) -> vec2<f32> {
    let half = 1u << s;
    let pos = t & (half - 1u);
    let ang = f32(pos) * (dir * PI / f32(half));
    return vec2<f32>(cos(ang), sin(ang));
}
fn wakeStageIndex(t: u32, s: u32) -> u32 {
    let half = 1u << s;
    return ((t >> s) << (s + 1u)) | (t & (half - 1u));
}
fn wakeCmul(b: vec4<f32>, w: vec2<f32>) -> vec4<f32> {
    return vec4<f32>(b.x * w.x - b.y * w.y, b.x * w.y + b.y * w.x, b.z * w.x - b.w * w.y, b.z * w.y + b.w * w.x);
}

// ------------------------------------------------------------------ pass 1: cell physics

fn wakeCell(col: u32, row: u32) -> vec4<f32> {
    let dt = wk.m0.x;
    let idx = row * WAKE_N + col;
    let p = wakeCellPos(col, row);
    let s = wakeLoadState(col, row);
    let cL = (col + WAKE_MASK) & WAKE_MASK; let cR = (col + 1u) & WAKE_MASK;
    let rD = (row + WAKE_MASK) & WAKE_MASK; let rU = (row + 1u) & WAKE_MASK;
    let sL = wakeLoadState(cL, row); let sR = wakeLoadState(cR, row);
    let sD = wakeLoadState(col, rD); let sU = wakeLoadState(col, rU);

    let d = wk.m3.w - terrainHeightAt(p);

    // boat frame of the cell
    let rel = p - wk.boat.xy;
    let cs = wk.boat.z; let sn = wk.boat.w;
    let bx = rel.x * cs - rel.y * sn;
    let bz = rel.x * sn + rel.y * cs;
    // hull pressure head (m): hydrostatic (immersion, with the speed-scheduled trim) and a
    // dynamic part where the hull enters the water: ~ K U^2/2g times the rise of the bottom
    let e = 0.7;
    let Ph = wakeImmersionAt(bx, bz);
    let entry = max((wakeImmersionAt(bx, bz - e) - wakeImmersionAt(bx, bz + e)) / (2.0 * e), 0.0);
    // transom stern at speed: the flow separates at the transom and leaves a hollow behind it
    let zAft = wk.hull.w;
    let aft = zAft - bz;
    let hollow = wakeImmersionAt(bx, zAft + 0.25) * exp(-max(aft, 0.0) / wk.m2.z) * smoothstep(-0.3, 0.3, aft);
    let P = (max(Ph, hollow * wk.m2.w) + entry * wk.m2.y) * wk.m0.z;

    // on a (re)start the water under the hull is already displaced: no transient
    let h = select(s.x, -P, wk.trim.w > 0.5);
    let w = s.y;

    let gx = (sR.x - sL.x) / (2.0 * WAKE_CELL); let gz = (sU.x - sD.x) / (2.0 * WAKE_CELL);
    let slope = length(vec2<f32>(gx, gz));

    // obstacles: dry land (OP has no pier-pile colliders to bake; see wake.rs)
    let wet = smoothstep(0.0, 0.05, d);
    let keep = wet;

    // breaking: steep crests (deep water) and depth-limited (surf zone); free waves only
    let brk = sat((slope - 0.2) / 0.2) * (1.0 - smoothstep(0.0, 0.03, P));
    let surf = sat((h - d * 0.42) / max(d * 0.3, 0.03)) * wet;
    let turb = s.w;

    // propeller wash / transom wake (boat frame)
    let time = wk.m4.x;
    let speed = wk.m1.y;
    let wash = wk.m0.w;
    let behind = zAft - bz;
    let washW = max(behind, 0.0) * 0.1 + wk.m1.z;
    let inWash = smoothstep(-0.6, 0.3, behind) * smoothstep(7.0, 1.5, behind) * smoothstep(washW, washW * 0.3, abs(bx));
    let streak = wakeNoise(vec2<f32>(bx * 1.7, time * 0.9));
    let patchN = wakeNoise(p * 0.45 + time * 0.13);
    let race = smoothstep(7.0, 0.0, behind) * 0.9;
    let along = p.x * sn + p.y * cs; let across = p.x * cs - p.y * sn;
    let streaks = wakeNoise(vec2<f32>(along * 0.12, across * 0.9)) * 0.7 + wakeNoise(vec2<f32>(along * 0.3, across * 1.9) + 5.3) * 0.3;
    let mottle = clamp((streaks - 0.5) * 3.2, -0.85, 0.85) + 1.0;
    let washGen = inWash * wash * (speed + 1.5) * (streak * patchN * 0.6 + race + 0.08) * mix(mottle, 1.0, smoothstep(3.0, 0.0, behind) * 0.7);

    // bow: the spray thrown off the forward hull falls back in a band just outside the waterline
    // (OP: the band's fore/aft limits follow the hull length instead of Tidewater's 3.2-4.2 m)
    let bowZ = wk.hull.y + wk.hull.z - 0.8;
    let outside = 1.0 - smoothstep(0.0, 0.03, Ph);
    let nearHull = smoothstep(0.0, 0.03, wakeImmersionAt(max(abs(bx) - 0.9, 0.0), bz));
    let bowBand = outside * nearHull * smoothstep(bowZ - 4.7, bowZ - 3.0, bz) * smoothstep(bowZ, bowZ - 1.0, bz);
    let bowGen = bowBand * wk.m1.x * (speed + 1.0) * (wakeNoise(vec2<f32>(bz * 1.5, time * 3.0)) * 1.4 + 0.1) * 0.35;

    // turbulent eddy viscosity on w in flux form (conserves mass)
    let visc = wk.m2.x;
    let nuC = min(turb * 0.6, 1.0) + visc;
    let flux = ((nuC + min(sL.w * 0.6, 1.0) + visc) * (sL.y - w)
        + (nuC + min(sR.w * 0.6, 1.0) + visc) * (sR.y - w)
        + (nuC + min(sD.w * 0.6, 1.0) + visc) * (sD.y - w)
        + (nuC + min(sU.w * 0.6, 1.0) + visc) * (sU.y - w)) * (0.5 / (WAKE_CELL * WAKE_CELL));
    // boils: Laplacian of (turbulence x noise), so the stirring sums to zero (conserves mass)
    var boil = 0.0;
    if (turb + sL.w + sR.w + sD.w + sU.w > 0.02) {
        let drift = vec2<f32>(time * 0.5, time * -0.35);
        let c0 = (wakeNoise(p * 0.45 + drift) - 0.5) * min(turb, 1.5);
        boil = (wakeNoise((p - vec2<f32>(WAKE_CELL, 0.0)) * 0.45 + drift) - 0.5) * min(sL.w, 1.5)
            + (wakeNoise((p + vec2<f32>(WAKE_CELL, 0.0)) * 0.45 + drift) - 0.5) * min(sR.w, 1.5)
            + (wakeNoise((p - vec2<f32>(0.0, WAKE_CELL)) * 0.45 + drift) - 0.5) * min(sD.w, 1.5)
            + (wakeNoise((p + vec2<f32>(0.0, WAKE_CELL)) * 0.45 + drift) - 0.5) * min(sU.w, 1.5)
            - c0 * 4.0;
    }
    let w1 = (w + flux * dt + boil * (dt * wk.m1.w)) * exp(dt * -0.006) * keep;

    // foam: thick foam thins quickly (bubbles rise and burst), the lace lingers
    let lapF = (sL.z + sR.z + sD.z + sU.z - s.z * 4.0) / (WAKE_CELL * WAKE_CELL);
    let f0 = max(s.z + lapF * min(dt * (turb * 0.3 + 0.04), 0.03), 0.0);
    let gen = (washGen + bowGen + brk * 3.0 + surf * 4.0) * wk.m3.x;
    let breakup = wakeNoise(p * 0.17 + vec2<f32>(13.1, 7.3)) * 0.65 + wakeNoise(p * 0.55 + vec2<f32>(3.7, 1.9)) * 0.35;
    let clr = smoothstep(0.3, 0.72, breakup) * 2.6 + 0.2;
    let foam = min(f0 * exp(-dt * (f0 * f0 * 0.6 + clr / 35.0)) + gen * dt, 3.0);
    let turb1 = min(turb * exp(dt * (-1.0 / 6.0)) + (inWash * wash * (speed + 1.5) * 0.5 + brk * 4.0 + surf * 6.0) * dt, 3.0);

    // aeration (0..~3): the bubbles in the water column, rising out over ~25 s
    let aC = wakeLoadAer(col, row);
    let lapA = wakeLoadAer(cL, row) + wakeLoadAer(cR, row) + wakeLoadAer(col, rD) + wakeLoadAer(col, rU) - aC * 4.0;
    let a0 = max(aC + lapA * min((turb * 0.35 + 0.07) * dt / (WAKE_CELL * WAKE_CELL), 0.2), 0.0);
    let wA = washW * (wakeNoise(p * 0.22 + time * 0.05) * 0.8 + 0.8);
    let aerBand = smoothstep(-0.6, 0.3, behind) * smoothstep(9.0, 2.0, behind) * smoothstep(wA, wA * 0.2, abs(bx));
    let aerGen = (aerBand * wash * (speed + 1.5) * ((race * 1.2 + 0.15) * (patchN * 1.3 + streak * 0.5 + 0.1))
        * (mottle * mottle * 0.55) + bowGen * 0.6 + brk * 1.5 + surf * 2.0) * wk.m3.y;
    let rise = smoothstep(0.25, 0.75, breakup * 0.6 + patchN * 0.4) * 1.3 + 0.45;
    let aer = min(a0 * exp(-dt * (a0 * 0.06 + rise / 25.0)) + aerGen * dt, 3.0) * wet;
    wakeAerB[idx] = aer;
    textureStore(wakeAerOut, vec2<u32>(col, row), vec4<f32>(aer, 0.0, 0.0, 0.0));

    wakeScratch[idx] = vec4<f32>(h, w1, foam, turb1);
    // the free surface is h (pushed down under the hull, the hollow behind the transom)
    textureStore(wakeDisplayOut, vec2<u32>(col, row), vec4<f32>(h, gx, gz, foam));

    return wakeDepthWeights(d) * (h + P);
}

var<workgroup> shRow: array<vec4<f32>, 512>;

@compute @workgroup_size(256, 1, 1)
fn wake_rows(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {
    let t = lid.x;
    let row = wid.x;
    for (var e = 0u; e < 2u; e++) {
        let col = t + e * WAKE_HALF;
        shRow[wakeBrev(col)] = wakeCell(col, row);
    }
    workgroupBarrier();
    for (var s = 0u; s < WAKE_LOG2N; s++) {
        let half = 1u << s;
        let i = wakeStageIndex(t, s);
        let j = i + half;
        let w = wakeTwiddle(t, s, -1.0);
        let a = shRow[i];
        let bw = wakeCmul(shRow[j], w);
        shRow[i] = a + bw;
        shRow[j] = a - bw;
        workgroupBarrier();
    }
    for (var e = 0u; e < 2u; e++) {
        let col = t + e * WAKE_HALF;
        wakeSpec[row * WAKE_N + col] = shRow[col];
    }
}

// ------------------------------------------------------------------ pass 2: columns

var<workgroup> shCol: array<vec4<f32>, 1024>;

// Z = FFT(a) + i FFT(b) for each packed pair of real fields (a, b) = (s_i u, s_j u);
// Y = L_i FFT(a) + i L_j FFT(b) = Z (L_i + L_j) / 2 + conj( Z(-k) ) (L_i - L_j) / 2
fn wakeApply(Z: vec4<f32>, Zm: vec4<f32>, p: vec2<f32>, m: vec2<f32>) -> vec4<f32> {
    return vec4<f32>(Z.x * p.x + Zm.x * m.x, Z.y * p.x - Zm.y * m.x, Z.z * p.y + Zm.z * m.y, Z.w * p.y - Zm.w * m.y);
}

fn wakeColStages(dir: f32, t: u32) {
    for (var s = 0u; s < WAKE_LOG2N; s++) {
        let half = 1u << s;
        let i0 = wakeStageIndex(t, s);
        let w = wakeTwiddle(t, s, dir);
        {
            let i = i0;
            let j = i + half;
            let a = shCol[i];
            let bw = wakeCmul(shCol[j], w);
            shCol[i] = a + bw;
            shCol[j] = a - bw;
        }
        {
            let i = i0 + WAKE_N;
            let j = i + half;
            let a = shCol[i];
            let bw = wakeCmul(shCol[j], w);
            shCol[i] = a + bw;
            shCol[j] = a - bw;
        }
        workgroupBarrier();
    }
}

@compute @workgroup_size(256, 1, 1)
fn wake_columns(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {
    let t = lid.x;
    let wg = wid.x;
    // (uniform per workgroup: the barriers inside the branch are in uniform control flow)
    if (wg <= WAKE_HALF) {
        let cA = wg;
        let cB = (WAKE_N - wg) & WAKE_MASK;
        for (var e = 0u; e < 2u; e++) {
            let r = t + e * WAKE_HALF;
            let rb = wakeBrev(r);
            shCol[rb] = wakeSpec[r * WAKE_N + cA];
            shCol[rb + WAKE_N] = wakeSpec[r * WAKE_N + cB];
        }
        workgroupBarrier();
        wakeColStages(-1.0, t);
        let fx = f32(select(i32(cA) - i32(WAKE_N), i32(cA), cA < WAKE_HALF)) * WAKE_DK;
        var outs: array<vec4<f32>, 4>;
        for (var e = 0u; e < 2u; e++) {
            let r = t + e * WAKE_HALF;
            let rm = (WAKE_N - r) & WAKE_MASK;
            let fz = f32(select(i32(r) - i32(WAKE_N), i32(r), r < WAKE_HALF)) * WAKE_DK;
            let k = length(vec2<f32>(fx, fz));
            // exp-based tanh overflows: clamp the argument
            let L0 = k * WAKE_NORM;
            let L1 = k * tanh(min(k * 6.0, 12.0)) * WAKE_NORM;
            let L2 = k * tanh(min(k * 1.8, 12.0)) * WAKE_NORM;
            let L3 = k * tanh(min(k * 0.6, 12.0)) * WAKE_NORM;
            let pp = vec2<f32>(L0 + L1, L2 + L3) * 0.5;
            let mm = vec2<f32>(L0 - L1, L2 - L3) * 0.5;
            outs[e * 2u] = wakeApply(shCol[r], shCol[rm + WAKE_N], pp, mm);
            outs[e * 2u + 1u] = wakeApply(shCol[r + WAKE_N], shCol[rm], pp, mm);
        }
        workgroupBarrier();
        for (var e = 0u; e < 2u; e++) {
            let rb = wakeBrev(t + e * WAKE_HALF);
            shCol[rb] = outs[e * 2u];
            shCol[rb + WAKE_N] = outs[e * 2u + 1u];
        }
        workgroupBarrier();
        wakeColStages(1.0, t);
        for (var e = 0u; e < 2u; e++) {
            let r = t + e * WAKE_HALF;
            wakeSpec[r * WAKE_N + cA] = shCol[r];
            if (cB != cA) {
                wakeSpec[r * WAKE_N + cB] = shCol[r + WAKE_N];
            }
        }
    } else if (wg <= WAKE_HALF + WAKE_NEAR_GROUPS) {
        // near-field template (spare workgroups): running mean of the wake height at boat-frame
        // points (the rows pass has just written this step's h into the display texture)
        let k = (wg - (WAKE_HALF + 1u)) * WAKE_HALF + t;
        if (k < WAKE_TW * WAKE_TH) {
            let tx = k % WAKE_TW; let tz = k / WAKE_TW;
            let bx = (f32(tx) + 0.5) * (wk.near.z / f32(WAKE_TW)) + wk.near.x;
            let bz = (f32(tz) + 0.5) * (wk.near.w / f32(WAKE_TH)) + wk.near.y;
            let cs = wk.boat.z; let sn = wk.boat.w;
            let pw = wk.boat.xy + vec2<f32>(bx * cs + bz * sn, bz * cs - bx * sn);
            let eta = textureSampleLevel(wakeDisplayTex, smpLinearRepeat, pw / WAKE_SIZE, 0.0).x;
            let prev = wakeNearBuf[k];
            let mean = prev + (eta - prev) * wk.m3.z;
            wakeNearBuf[k] = mean;
            let border = tx == 0u || tx == WAKE_TW - 1u || tz == 0u || tz == WAKE_TH - 1u;
            let D = textureSampleLevel(wakeHullTex, smpLinearClamp, wakeHullUv(bx, bz), 0.0).x;
            let m = select(smoothstep(0.0, 0.03, D), 0.0, border);
            textureStore(wakeNearOut, vec2<u32>(tx, tz), vec4<f32>(mean, m, 0.0, 0.0));
        }
    }
}

// ------------------------------------------------------------------ pass 3: inverse rows, step

@compute @workgroup_size(256, 1, 1)
fn wake_inverse(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {
    let t = lid.x;
    let row = wid.x;
    let dt = wk.m0.x;
    for (var e = 0u; e < 2u; e++) {
        let col = t + e * WAKE_HALF;
        shRow[wakeBrev(col)] = wakeSpec[row * WAKE_N + col];
    }
    workgroupBarrier();
    for (var s = 0u; s < WAKE_LOG2N; s++) {
        let half = 1u << s;
        let i = wakeStageIndex(t, s);
        let j = i + half;
        let w = wakeTwiddle(t, s, 1.0);
        let a = shRow[i];
        let bw = wakeCmul(shRow[j], w);
        shRow[i] = a + bw;
        shRow[j] = a - bw;
        workgroupBarrier();
    }
    let rl = f32((i32(row) - i32(wk.win.y)) & i32(WAKE_MASK)) + 0.5;
    let ez = min(rl, f32(WAKE_N) - rl);
    for (var e = 0u; e < 2u; e++) {
        let col = t + e * WAKE_HALF;
        let idx = row * WAKE_N + col;
        let Y = shRow[col];
        let sc = wakeScratch[idx];
        let p = wakeCellPos(col, row);
        let d = wk.m3.w - terrainHeightAt(p);
        let Lu = dot(wakeDepthWeights(d), Y);
        let w1 = sc.y - Lu * (dt * WAKE_GRAVITY);
        let h1 = sc.x + w1 * dt;
        let cl = f32((i32(col) - i32(wk.win.x)) & i32(WAKE_MASK)) + 0.5;
        let edge = min(min(cl, f32(WAKE_N) - cl), ez);
        let sig = smoothstep(WAKE_SPONGE, 0.0, edge);
        let k = exp(dt * (sig * sig * -6.0));
        wakeState[idx] = vec4<f32>(h1 * k, w1 * k, sc.z, sc.w);
        wakeAerA[idx] = wakeAerB[idx];
    }
}
