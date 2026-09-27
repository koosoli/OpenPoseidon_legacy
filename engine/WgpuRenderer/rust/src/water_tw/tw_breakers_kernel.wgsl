// Tidewater Native — the breaker crest finder (W3d) and spray emission (W4a): Breakers.js
// `_buildKernel` (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC). Composed as
// tw_common + tw_shore + tw_spray_common + tw_spray_emit + this file. OP adaptations (see breakers.rs): stations carry their
// shoreline run, which goes into the crest id so no lip spans two runs; the cascade sizes are
// the live ones (Tidewater bakes its defaults into the code); frame.seaLevel is tw.sea.x.

struct OceanParams {
    sizes: array<vec4<f32>, 4>,
    cuts: array<vec4<f32>, 4>,
    sys_a: array<vec4<f32>, 2>,
    sys_b: array<vec4<f32>, 2>,
    p0: vec4<f32>,
    p1: vec4<f32>,
    seed: vec4<u32>,
};

@group(0) @binding(0) var<uniform> tw: TwSurface;
@group(0) @binding(1) var<uniform> cf: ConformParams;
@group(0) @binding(2) var seabed_heightmap: texture_2d<f32>;
@group(0) @binding(3) var shoreFieldT: texture_2d<f32>;
@group(0) @binding(4) var shoreFieldDir: texture_2d<f32>;
@group(0) @binding(5) var<uniform> ocean: OceanParams;
@group(0) @binding(6) var oceanDisplacement: texture_2d_array<f32>;
@group(0) @binding(7) var smpLinearRepeat: sampler;
// two vec4 per station: (x, z, nx, nz) toward the sea, (run, 0, 0, 0)
@group(0) @binding(8) var<storage, read> breakersStations: array<vec4<f32>>;
// per station and slot (wave parity): 3 x vec4
//   (root.xyz, b) (back.xyz, H) (dir.xz, trough y, wave id (+ run x 1024) or 0 = none)
@group(0) @binding(9) var<storage, read_write> breakersCrestW: array<vec4<f32>>;

const BRK_GRAVITY: f32 = 9.81;
const BRK_STEP: f32 = 2.0;
const BRK_K: i32 = 64;

// FFT displacement at a Lagrangian point (the short cascades that survive in the surf zone),
// with WaterSurface.cascadeAttenuation (the long cascades vanish in shallow water)
fn breakersFftDisp(p: vec2<f32>, depth: f32) -> vec3<f32> {
    var d = vec3<f32>(0.0);
    var floorAmt = array<f32, 4>(0.0, 0.05, 0.25, 0.5);
    for (var c = 1; c < 4; c++) {
        let L = ocean.sizes[c].x;
        let texel = L / 256.0;
        let level = max(log2(0.35 / texel) + 0.7, 0.0);
        let d0 = min(40.0, L * 0.08);
        d += textureSampleLevel(oceanDisplacement, smpLinearRepeat, p / L, c, level).xyz
            * mix(floorAmt[c] * smoothstep(0.0, 0.6, depth), 1.0, smoothstep(0.0, d0, depth));
    }
    return d * tw.surface.x;
}

// One crest of wave m at Lagrangian point pc (station i): store the lip frame.
fn breakersProcessCrest(i: u32, pc: vec2<f32>, m: f32, run: f32) {
    let ph = shorePhaseAt(pc);
    let dir = ph.dir;
    let along = ph.along;
    let ground = terrainHeightAt(pc);
    let depth = tw.sea.x - ground;
    let A = shoreWaveAmp(m, along);
    // (W3l) the breaking state from the smoothed depth, as the water surface's (shoreBreakDepth)
    let dS = shoreSmoothDepth(pc, depth);
    let cr = shoreCrest(A, dS); // (b, H, trough, lipThrow)
    let b = cr.x;
    let env = smoothstep(26.0, 13.0, depth) * sat(ph.exposure * 1.4) * tw.shore1.w;

    // followed from before it breaks until the bore reaches the shore (the lip sheet only uses b < 1.25)
    if (b > -0.6 && env > 0.3 && depth > 0.12) {
        let c = sqrt(clamp(depth, 0.3, 25.0) * BRK_GRAVITY);
        let lam = c * tw.shore0.x;
        let s0 = shoreShape(0.0, A, dS, lam);
        let ub = 0.4 / lam;
        let s1 = shoreShape(ub, A, dS, lam);
        let fd = breakersFftDisp(pc, depth);
        let d3 = vec3<f32>(dir.x, 0.0, dir.y);
        let root = vec3<f32>(pc.x, tw.sea.x, pc.y) + d3 * (s0.x * env) + vec3<f32>(0.0, s0.y * env, 0.0) + fd;
        let back = vec3<f32>(pc.x, tw.sea.x, pc.y) + d3 * (s1.x * env - 0.4) + vec3<f32>(0.0, s1.y * env, 0.0) + fd;
        let H = cr.y * env;
        let trough = tw.sea.x + cr.z * env + fd.y;
        // slot by wave parity (neighbouring stations agree on it), id = wave index mod 1024, + 1
        // (0 = none), + the shoreline run x 1024 (OP: no lip joins two runs)
        let slot = u32(m - floor(m * 0.5) * 2.0);
        let id = m - floor(m / 1024.0) * 1024.0 + 1.0 + run * 1024.0;
        let k = i * 6u + slot * 3u;
        breakersCrestW[k] = vec4<f32>(root, b);
        breakersCrestW[k + 1u] = vec4<f32>(back, H);
        breakersCrestW[k + 2u] = vec4<f32>(dir, trough, id);
        breakersEmit(i, slot, root, dir, b, H, trough, c, m, depth, shoreBore(A, dS));
    }
}

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    let ns = u32(tw.brk.x);
    if (i >= ns) { return; }
    let st = breakersStations[i * 2u];
    let run = breakersStations[i * 2u + 1u].x;
    let o = st.xy;
    let n = st.zw; // toward the sea
    let base = i * 6u;
    // clear both slots
    breakersCrestW[base + 2u] = vec4<f32>(0.0);
    breakersCrestW[base + 5u] = vec4<f32>(0.0);

    var sPrev = 0.0;
    for (var k = 0; k < BRK_K; k++) {
        let dist = f32(k) * BRK_STEP;
        let s = shorePhaseAt(o + n * dist).s;
        // s grows seaward: a crest (integer phase) lies between this sample and the previous one
        if (k > 0 && floor(s) > floor(sPrev)) {
            let m = floor(s);
            // secant refinement on the exact phase
            var lo = dist - BRK_STEP;
            var hi = dist;
            var sLo = sPrev;
            var sHi = s;
            var x = lo + (m - sLo) / max(sHi - sLo, 1e-5) * BRK_STEP;
            for (var it = 0; it < 2; it++) {
                let sx = shorePhaseAt(o + n * x).s;
                if (sx < m) {
                    lo = x;
                    sLo = sx;
                } else {
                    hi = x;
                    sHi = sx;
                }
                x = lo + (m - sLo) / max(sHi - sLo, 1e-5) * (hi - lo);
            }
            let pc = o + n * x;
            breakersProcessCrest(i, pc, m, run);
        }
        sPrev = s;
    }
}
