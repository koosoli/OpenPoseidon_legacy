// Tidewater Native W6 — the wake readers (WakeSim.js `_buildReaders`), ported from
// dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software Solutions LLC). Appended to the water
// material; the simulation is tw_wake_kernel.wgsl.
//
// OP: up to WAKE_SLOTS wakes (one per simulated boat, the nearest ones); each reader sums the
// slots. The per-slot WakeParams live in the surface block (tw.wake, 4 lanes per slot):
//   [0] window centre xz (m), boat position xz (m)
//   [1] boat cos / sin yaw, residual dead zone (m) x2
//   [2] amount (0 = asleep / unused), amplitude, aeration out, unused
//   [3] near-field template box: TX0, TZ0, TX1 - TX0, TZ1 - TZ0 (boat frame, m)

const WAKE_SLOTS: i32 = 2;
const WAKE_R_SIZE: f32 = 204.8;
const WAKE_R_CELL: f32 = 0.4;
const WAKE_R_MASK: i32 = 511;
const WAKE_R_TW: f32 = 32.0;
const WAKE_R_TH: f32 = 88.0;

@group(1) @binding(25) var wakeDisplay: texture_2d_array<f32>; // rgba16float: h, dh/dx, dh/dz, foam
@group(1) @binding(26) var wakeNear: texture_2d_array<f32>;    // rgba16float: template mean, hull mask
@group(1) @binding(27) var wakeAerTex: texture_2d_array<f32>;  // rgba16float: aeration (x)

fn wakeRHash(ix: i32, iy: i32) -> f32 {
    var v = (u32(ix) * 0x8da6b343u) ^ (u32(iy) * 0xd8163841u);
    v = (v ^ (v >> 13u)) * 0x5bd1e995u;
    v = v ^ (v >> 15u);
    return f32(v >> 8u) * (1.0 / 16777216.0);
}

fn wakeRNoise(q: vec2<f32>) -> f32 {
    let i = floor(q);
    let fr = fract(q);
    let u = fr * fr * (3.0 - 2.0 * fr);
    let ix = i32(i.x); let iy = i32(i.y);
    let a = wakeRHash(ix, iy); let b = wakeRHash(ix + 1, iy);
    let c = wakeRHash(ix, iy + 1); let d = wakeRHash(ix + 1, iy + 1);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

fn wakeEdgeDist(s: i32, xz: vec2<f32>) -> f32 {
    let rel = abs(xz - tw.wake[s * 4].xy);
    return WAKE_R_SIZE / 2.0 - max(rel.x, rel.y);
}
fn wakeFade(s: i32, xz: vec2<f32>) -> f32 { return smoothstep(4.0, 16.0, wakeEdgeDist(s, xz)) * tw.wake[s * 4 + 2].x; }
// foam and aeration: a long fade toward the window edge, no visible end of the track
fn wakeFadeLong(s: i32, xz: vec2<f32>) -> f32 { return smoothstep(4.0, 45.0, wakeEdgeDist(s, xz)) * tw.wake[s * 4 + 2].x; }

// template texel coordinates of world xz (boat frame)
fn wakeNearCoord(s: i32, xz: vec2<f32>) -> vec2<f32> {
    let rel = xz - tw.wake[s * 4].zw;
    let cs = tw.wake[s * 4 + 1].x; let sn = tw.wake[s * 4 + 1].y;
    let bx = rel.x * cs - rel.y * sn; let bz = rel.x * sn + rel.y * cs;
    let box = tw.wake[s * 4 + 3];
    return vec2<f32>((bx - box.x) * (WAKE_R_TW / max(box.z, 1e-3)), (bz - box.y) * (WAKE_R_TH / max(box.w, 1e-3))) - 0.5;
}
fn wakeInTemplate(tc: vec2<f32>) -> bool { return tc.x > 0.0 && tc.y > 0.0 && tc.x < WAKE_R_TW - 1.0 && tc.y < WAKE_R_TH - 1.0; }

// (outside the simulated window, or asleep, both fades are 0: nothing to read)
fn wakeOff(s: i32, xz: vec2<f32>) -> bool { return tw.wake[s * 4 + 2].x <= 0.0 || wakeEdgeDist(s, xz) <= 4.0; }

fn wakeSampleSlot(s: i32, xz: vec2<f32>) -> vec4<f32> {
    if (wakeOff(s, xz)) { return vec4<f32>(0.0); }
    var o = textureSampleLevel(wakeDisplay, smpLinearRepeat, xz / WAKE_R_SIZE, s, 0.0) * vec4<f32>(vec3<f32>(wakeFade(s, xz)), wakeFadeLong(s, xz));
    // the forced depression under the hull is covered by it: keep its edge out of the normals
    let tc = wakeNearCoord(s, xz);
    if (wakeInTemplate(tc)) {
        let m = textureLoad(wakeNear, vec2<i32>(tc + 0.5), s, 0).y;
        o = vec4<f32>(o.x, o.yz * (1.0 - m), o.w);
    }
    return o;
}

// aeration (0..aerOut): bilinear by hand, then mottled by a noise fixed in the water
fn wakeAerationSlot(s: i32, xz: vec2<f32>) -> f32 {
    var out = 0.0;
    if (wakeOff(s, xz)) { return out; }
    let tc = xz / WAKE_R_CELL - 0.5;
    let i = vec2<i32>(floor(tc));
    let fr = fract(tc);
    let M = vec2<i32>(WAKE_R_MASK);
    let a = mix(mix(textureLoad(wakeAerTex, i & M, s, 0).x, textureLoad(wakeAerTex, (i + vec2<i32>(1, 0)) & M, s, 0).x, fr.x),
        mix(textureLoad(wakeAerTex, (i + vec2<i32>(0, 1)) & M, s, 0).x, textureLoad(wakeAerTex, (i + vec2<i32>(1, 1)) & M, s, 0).x, fr.x), fr.y);
    if (a > 0.01) {
        let m = wakeRNoise(xz * 0.3 + vec2<f32>(tw.sea.y * 0.02, 0.0)) * 0.55 + wakeRNoise(xz * 0.85 + 17.3) * 0.45;
        let aN = (1.0 - exp(a * -0.5)) * (m * 0.8 + 0.6);
        out = smoothstep(m * 0.2 + 0.03, m * 0.2 + 0.55, aN) * tw.wake[s * 4 + 2].z * wakeFadeLong(s, xz);
    }
    return out;
}

fn wakeHeightSlot(s: i32, xz: vec2<f32>) -> f32 {
    if (wakeOff(s, xz)) { return 0.0; }
    var h = textureSampleLevel(wakeDisplay, smpLinearRepeat, xz / WAKE_R_SIZE, s, 0.0).x;
    let tc = wakeNearCoord(s, xz);
    if (wakeInTemplate(tc)) {
        // under the hull only waves moving relative to it remain (an old wake being crossed);
        // small residuals are sampling jitter of the steep near field and are dropped
        let i = vec2<i32>(floor(tc));
        let fr = fract(tc);
        let a = textureLoad(wakeNear, i, s, 0); let b = textureLoad(wakeNear, i + vec2<i32>(1, 0), s, 0);
        let c = textureLoad(wakeNear, i + vec2<i32>(0, 1), s, 0); let d = textureLoad(wakeNear, i + vec2<i32>(1, 1), s, 0);
        let t = mix(mix(a, b, fr.x), mix(c, d, fr.x), fr.y);
        let r = h - t.x;
        let dead = tw.wake[s * 4 + 1].zw;
        h = mix(h, r * smoothstep(dead.x, dead.y, abs(r)), t.y);
    }
    return h * wakeFade(s, xz) * tw.wake[s * 4 + 2].y;
}

// Displacement (vec3) of the wake surface at world xz (Lagrangian point of the ocean grid).
fn wakeDisplacement(xz: vec2<f32>) -> vec3<f32> {
    var h = 0.0;
    for (var s = 0; s < WAKE_SLOTS; s++) { h += wakeHeightSlot(s, xz); }
    return vec3<f32>(0.0, h, 0.0);
}

// slopes (dh/dx, dh/dz), foam, aeration (0..1)
struct WakeFrag { slopes: vec2<f32>, foam: f32, aeration: f32 };
fn wakeFragment(xz: vec2<f32>) -> WakeFrag {
    var o: WakeFrag;
    o.slopes = vec2<f32>(0.0);
    o.foam = 0.0;
    o.aeration = 0.0;
    for (var s = 0; s < WAKE_SLOTS; s++) {
        let smp = wakeSampleSlot(s, xz);
        o.slopes += smp.yz * tw.wake[s * 4 + 2].y;
        o.foam += smp.w;
        o.aeration += wakeAerationSlot(s, xz);
    }
    return o;
}
