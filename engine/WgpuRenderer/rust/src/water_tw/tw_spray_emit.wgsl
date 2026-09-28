// Tidewater Native — spray emitters of the breaking waves (W4a): Spray.js's emitter API
// (sprayReserve / spraySlot / sprayWrite / sprayRand) and Breakers.js `_emitCode`
// (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC). Composed into the breaker
// kernel after tw_spray_common; breakersProcessCrest calls breakersEmit for every crest it finds.
//
// OP adaptations (see spray.rs): the GPU emitters write the first `seed.z` slots (W6i: the CPU
// tail after them carries the boats' bow spray, spawned by spray_update); Breakers' `spray`, `emitRange` and cameraPos, and the frame's
// dt, come from OP's spray block; the wind is the water's (tw.wind.xy, tw.sea.z); the budget is
// steered on the GPU (spray_update) instead of from a CPU readback of the head.

@group(0) @binding(10) var<uniform> sprayP: SprayParams;
// the ring, three arrays of RING vec4 back to back (OP: one binding): pos: xyz, age (s) |
// vel: xyz, radius (m) | info: kind, life (s, 0 = dead), water height, tag
@group(0) @binding(11) var<storage, read_write> sprayBuf: array<vec4<f32>>;
// [0] ring head, [1] head at the last budget step, [2] emission rate (f32 bits, particles/s),
// [3] emission budget (f32 bits; see spray_update), [4] / [5] live particles counted by the update
// on even / odd frames (diagnostics, read back by spray.rs), [6] frames updated, [7] unused
@group(0) @binding(12) var<storage, read_write> sprayHead: array<atomic<u32>, 8>;

// Breakers' `budget`: the emission scale that keeps the ring from wrapping
fn sprayBudget() -> f32 { return bitcast<f32>(atomicLoad(&sprayHead[3])); }

// Reserve n ring slots; returns the base (pass it to spraySlot)
fn sprayReserve(n: u32) -> u32 { return atomicAdd(&sprayHead[0], n); }
fn spraySlot(base: u32, i: u32) -> u32 { return (base + i) & (sprayP.seed.z - 1u); }

fn sprayWrite(slot: u32, p: vec3<f32>, v: vec3<f32>, size: f32, kind: f32, life: f32, seed: f32) {
    let n = sprayP.seed.y;
    sprayBuf[slot] = vec4<f32>(p, 0.0);
    sprayBuf[n + slot] = vec4<f32>(v, size);
    sprayBuf[2u * n + slot] = vec4<f32>(kind, life, p.y, seed);
}

fn sprayRand(a: u32, b: u32) -> f32 {
    return sprayHash(a + b * 1664525u + sprayP.seed.x * 2654435761u);
}

struct BrkCtx {
    root: vec3<f32>,
    d3: vec3<f32>,
    tg: vec3<f32>,
    Yi: f32,
    Wt: f32,
    wB: f32,
    trough: f32,
    xr: f32,
    Xi: f32,
    q: f32,
};

struct BrkPN { p: vec3<f32>, n: vec3<f32> };

// A point on the front of the wave, on the water surface (the same curve as ShoreWaves' profile:
// concave tube face while plunging, convex roller front of the bore), and its outward normal.
// s: 0 = crest top .. 1 = foot. Everything the breaker throws starts just outside it.
fn breakersFacePoint(C: BrkCtx, sp: f32) -> BrkPN {
    let up = vec3<f32>(0.0, 1.0, 0.0);
    let th = sp * 1.5707963;
    let ct = cos(th);
    let st = sin(th);
    let fx = mix(1.0 - ct, st, C.wB);
    let fy = mix(1.0 - st, ct, C.wB);
    let n = normalize(C.d3 * (C.Yi * mix(ct, st, C.wB)) + up * (C.Wt * mix(st, ct, C.wB) + 0.02));
    let p = C.root + C.d3 * (C.Wt * fx);
    return BrkPN(vec3<f32>(p.x, C.trough + C.Yi * fy, p.z), n);
}

// the plunge point: in the trough ahead of the face while the tube is open; once the bore front
// has formed over it, on the lower part of that front
fn breakersPlunge(C: BrkCtx, r: f32, r2: f32) -> BrkPN {
    let f = breakersFacePoint(C, r2 * 0.45 + 0.55);
    let t = C.root + C.d3 * (C.xr + (r - 0.5) * 0.5);
    let inTrough = vec3<f32>(t.x, C.trough + 0.03, t.z);
    let open = C.xr > C.Wt + 0.1;
    return BrkPN(select(f.p + f.n * 0.04, inTrough, open), select(f.n, vec3<f32>(0.0, 1.0, 0.0), open));
}

// the leading edge of the falling lip
fn breakersTipAt(C: BrkCtx, r: f32) -> vec3<f32> {
    return C.root + C.d3 * (C.Xi * C.q * (r * 0.12 + 0.88)) - vec3<f32>(0.0, 1.0, 0.0) * (C.Yi * C.q * C.q);
}

// spread along the crest over one station (0.6 m x 1.15)
fn breakersAlong(C: BrkCtx, r: f32) -> vec3<f32> {
    return C.tg * ((r - 0.5) * 0.69);
}

fn breakersCount(rate: f32, gain: f32, seed: u32, salt: u32) -> u32 {
    return u32(floor(rate * gain + sprayRand(seed, salt)));
}

fn breakersEmit(i: u32, slot: u32, root: vec3<f32>, dir: vec2<f32>, b: f32, H: f32, trough: f32, c: f32, m: f32, depth: f32, bore: vec4<f32>) {
    if (sprayP.b.w < 0.5) { return; }
    let emitRange = sprayP.b.x;
    let camFade = smoothstep(emitRange, emitRange * 0.35, length(root - sprayP.cam.xyz));
    let gain = sprayP.a.w * camFade * sprayP.a.x;
    if (gain <= 0.0) { return; }
    // the many sub-pixel drops and ligaments take the ring budget; the few, visible spray and mist
    // sprites always get their full rate
    let gainD = gain * sprayBudget();
    let d3 = vec3<f32>(dir.x, 0.0, dir.y);
    let tg = vec3<f32>(-dir.y, 0.0, dir.x);
    let up = vec3<f32>(0.0, 1.0, 0.0);
    let Hc = clamp(H, 0.15, 3.0);
    // (the energy released goes as H^2.5, but what reads on screen saturates: bigger breakers make
    // bigger, longer-lived structures, not ever more particles)
    let E = pow(Hc, 1.7);
    let Hb = max(bore.x, 0.0);
    let Eb = pow(min(Hb, 2.0) / 0.6, 1.7);
    let Wt = bore.y;
    let q = clamp(b / 0.9, 0.0, 1.0);
    let Xi = H * 0.8;
    let Yi = max(root.y - trough, 0.05);
    let seed = i * 7919u + u32(m - floor(m / 64.0) * 64.0) * 31u;
    // integer part: which crest made the particle (its wave shadow, see sprayWaveShadow); fraction: random
    let tag = f32(i * 2u + slot + 1u);
    let offshore = max(-dot(tw.wind.xy, dir), 0.0) * tw.sea.z;
    let wB = bore.w;
    let xr = Xi - bore.z; // horizontal distance from the crest top
    let C = BrkCtx(root, d3, tg, Yi, Wt, wB, trough, xr, Xi, q);

    // ---- stages of the breaker (weights 0..1)
    let lipShed = smoothstep(0.5, 0.75, b) * (1.0 - smoothstep(0.86, 0.93, b));
    // the splash-up is a fast burst (~0.15-0.2 s) as the lip hits the trough; its mist lingers
    let impact = smoothstep(0.87, 0.92, b) * (1.0 - smoothstep(1.0, 1.14, b));
    let haze = smoothstep(0.9, 1.0, b) * (1.0 - smoothstep(1.2, 1.5, b));
    let spit = smoothstep(0.98, 1.08, b) * (1.0 - smoothstep(1.15, 1.3, b));
    let roller = smoothstep(1.05, 1.3, b) * smoothstep(0.06, 0.25, Hb);
    let clash = roller * smoothstep(0.1, 0.2, depth) * smoothstep(0.7, 0.35, depth);
    let drift = smoothstep(9.0, 15.0, offshore) * smoothstep(-0.4, 0.1, b) * (1.0 - smoothstep(0.75, 0.9, b));

    // ---- drops and ligaments (ballistic)
    let n0 = breakersCount(lipShed * 9.0 * E, gainD, seed, 1u); // drops off the lip
    let n1 = n0 + breakersCount(lipShed * 5.0 * E, gainD, seed, 2u); // ligaments off the lip
    let n2 = n1 + breakersCount(impact * 240.0 * E, gainD, seed, 3u); // splash-up drops
    let n3 = n2 + breakersCount(impact * 90.0 * E, gainD, seed, 4u); // splash-up ligaments (torn strands)
    let n4 = n3 + breakersCount(roller * 45.0 * Eb, gainD, seed, 5u); // roller front (breaks up its silhouette)
    let n5 = n4 + breakersCount(clash * 40.0 * Eb, gainD, seed, 6u); // bore / backwash collision
    let n6 = n5 + breakersCount(drift * 24.0 * Hc, gainD, seed, 7u); // spindrift
    if (n6 > 0u) {
        let base = sprayReserve(n6);
        for (var j = 0u; j < n6; j++) {
            let ring = spraySlot(base, j);
            let h = seed + j * 13u;
            let r0 = sprayRand(h, 11u);
            let r1 = sprayRand(h, 12u);
            let r2 = sprayRand(h, 13u);
            let r3 = sprayRand(h, 14u);
            let r4 = sprayRand(h, 15u);
            let isLip = j < n1;
            let isImp = j < n3;
            let isRol = j < n4;
            let isCl = j < n5;
            let lig = (j >= n0 && j < n1) || (j >= n2 && j < n3);
            // lip: moving with the jet (thrown forward a little faster than the wave, falling)
            let vLip = d3 * (c * (r2 * 0.2 + 1.05)) - up * (q * sqrt(Yi * 19.62) * (r3 * 0.3 + 0.7)) + tg * ((r4 - 0.5) * 0.6);
            // splash-up: most drops stay low, some reach ~1.3 H; thrown up and forward, out of the surface
            let pl = breakersPlunge(C, r1, r2);
            let vUp = sqrt(Hc * (r3 * r3 * 1.0 + 0.3) * 19.62);
            let vImp = up * vUp + pl.n * (r4 * 1.5) + d3 * (c * (r2 * 0.6 + 0.15) - 0.3) + tg * ((r4 - 0.5) * 2.0);
            // roller: tossed forward and up by the tumbling front (from its upper part)
            let fr = breakersFacePoint(C, r1 * r1 * 0.7); // mostly from the tumbling top
            let vRol = d3 * (c * (r2 * 0.3 + 0.95)) + up * ((r3 * 1.6 + 0.6) * sqrt(Hb / 0.5)) + fr.n * 0.5 + tg * ((r4 - 0.5) * 1.2);
            // bore running into the backwash: thrown straight up from its front
            let fc = breakersFacePoint(C, r1 * 0.5 + 0.3);
            let vCl = up * ((r3 * 1.6 + 0.8) * sqrt(Hb / 0.4)) + d3 * (r2 * 2.0 - 0.6) + tg * ((r4 - 0.5) * 1.5);
            // spindrift: fine drops blown back over the crest
            let vDr = d3 * (-(offshore * (r2 * 0.3 + 0.35))) + up * (r3 * 1.5 + 0.8) + tg * ((r4 - 0.5) * 0.5);
            let p = select(select(select(select(root + up * 0.04, fc.p + fc.n * 0.03, isCl), fr.p + fr.n * 0.03, isRol), pl.p, isImp), breakersTipAt(C, r1), isLip) + breakersAlong(C, r0);
            let v = select(select(select(select(vDr, vCl, isCl), vRol, isRol), vImp, isImp), vLip, isLip);
            // radius (m): drops of a few mm (smaller off the roller, finest in the spindrift), ligaments ~1-2 cm
            // heavy-tailed drop sizes (many fine drops, a few big ones): r = r0 (1 - u)^-0.7
            let tail = pow(1.0 - r4 * 0.98, -0.7);
            let rDrop = min(select(select(select(0.0005, 0.001, isLip), 0.0008, isRol), 0.001, isImp || isCl) * tail, 0.012);
            let rLig = 0.005 + r4 * r4 * select(0.008, 0.016, isImp);
            let kind = select(SPRAY_DROPLET, SPRAY_LIGAMENT, lig);
            sprayWrite(ring, p, v, select(rDrop, rLig, lig), kind, 2.5, tag + r0 * 0.999);
        }
    }

    // ---- dense spray (clouds of drops: the white of the splash-up), a puff out of the barrel
    let c0 = breakersCount(impact * 110.0 * E, gain, seed, 21u);
    let c1 = c0 + breakersCount(spit * 10.0 * E, gain, seed, 22u);
    let c2 = c1 + breakersCount(roller * 0.6 * Eb, gain, seed, 23u); // (rare: a row of them reads as cotton puffs)
    let c3 = c2 + breakersCount(clash * 2.0 * Eb, gain, seed, 24u);
    if (c3 > 0u) {
        let base = sprayReserve(c3);
        for (var j = 0u; j < c3; j++) {
            let ring = spraySlot(base, j);
            let h = seed + j * 29u;
            let r0 = sprayRand(h, 41u);
            let r1 = sprayRand(h, 42u);
            let r2 = sprayRand(h, 43u);
            let r3 = sprayRand(h, 44u);
            let isImp = j < c0;
            let isSpit = j < c1;
            let isRol = j < c2;
            // torn sheets of the splash-up (half-width), heavy-tailed: many small, a few big
            let sz = select(select(select(0.05 + r2 * r2 * 0.08, sqrt(Hb / 0.5) * (r2 * r2 * 0.06 + 0.03), isRol), sqrt(Hc) * (r2 * r2 * 0.14 + 0.05), isSpit), sqrt(Hc) * (r2 * r2 * r2 * 0.18 + 0.05), isImp);
            // splash-up: sheets thrown up and forward out of the plunge line, rising up to ~1.3 H and
            // falling back as curtains
            let pl = breakersPlunge(C, r1, r3);
            let pImp = pl.p + pl.n * (sz * 0.4) + up * (r2 * Hc * 0.1);
            let vImp = up * sqrt(Hc * (r3 * r3 * 0.95 + 0.35) * 19.62) + pl.n * 0.8 + d3 * (c * (r2 * 0.5 + 0.2)) + tg * ((r1 - 0.5) * 1.2);
            // the puff blown out of the collapsing barrel: out of the middle of the front, along the crest
            let fs = breakersFacePoint(C, r1 * 0.3 + 0.35);
            let pSpit = fs.p + fs.n * (sz * 0.5);
            let vSpit = d3 * (c * 0.7) + fs.n * 1.2 + tg * ((r1 - 0.5) * 4.0);
            // the tumbling top of the roller
            let fr = breakersFacePoint(C, r1 * 0.4);
            let pRol = fr.p + fr.n * (sz * 0.4);
            let vRol = d3 * (c * 0.85) + up * (r3 * 0.5 + 0.3);
            let fc = breakersFacePoint(C, r1 * 0.5 + 0.3);
            let pCl = fc.p + fc.n * (sz * 0.5);
            let vCl = up * ((r3 * 1.2 + 0.8) * sqrt(Hb / 0.4)) + d3 * (r2 - 0.3);
            let p = select(select(select(pCl, pRol, isRol), pSpit, isSpit), pImp, isImp) + breakersAlong(C, r0);
            let v = select(select(select(vCl, vRol, isRol), vSpit, isSpit), vImp, isImp);
            let life = select(select(r3 * 0.3 + 0.5, 0.9, isSpit), r3 * 0.5 + 1.0, isImp);
            sprayWrite(ring, p, v, sz, SPRAY_SPRAY, life, tag + r0 * 0.999);
        }
    }

    // ---- mist: the fine spray that drifts off with the wind (and the air pushed by the wave)
    // (few, large, faint sprites: mist is the biggest overdraw of the spray)
    let m0 = breakersCount(haze * 5.0 * E + spit * 3.0 * E, gain, seed, 31u);
    // the lip feathers as it throws: a thin wisp torn off the crest and carried by the air
    let mL = breakersCount(lipShed * 1.6 * E, gain, seed, 33u);
    // the churning top of the bore smokes: a thin haze drifting off it softens its silhouette
    let m1 = m0 + breakersCount(roller * 4.5 * Eb, gain, seed, 32u);
    if (mL > 0u) {
        let base = sprayReserve(mL);
        for (var j = 0u; j < mL; j++) {
            let ring = spraySlot(base, j);
            let h = seed + j * 23u;
            let r0 = sprayRand(h, 61u);
            let r1 = sprayRand(h, 62u);
            let r2 = sprayRand(h, 63u);
            let p = breakersTipAt(C, r1 * 0.4) + up * (r2 * 0.1) + breakersAlong(C, r0);
            let v = d3 * (c * (r1 * 0.2 + 0.85)) + up * (r2 * 0.8 + 0.3);
            sprayWrite(ring, p, v, (r2 * r2 * 0.2 + 0.1) * sqrt(Hc), SPRAY_MIST, r1 * 0.8 + 0.7, tag + r0 * 0.999);
        }
    }
    if (m1 > 0u) {
        let base = sprayReserve(m1);
        for (var j = 0u; j < m1; j++) {
            let ring = spraySlot(base, j);
            let h = seed + j * 17u;
            let r0 = sprayRand(h, 51u);
            let r1 = sprayRand(h, 52u);
            let r2 = sprayRand(h, 53u);
            let r3 = sprayRand(h, 54u);
            let isImp = j < m0;
            let pl = breakersPlunge(C, r1, r3);
            let ft = breakersFacePoint(C, r1 * 0.3);
            let p = select(ft.p + ft.n * 0.15 + up * (r2 * r2 * Hb * 0.35), pl.p + pl.n * 0.2 + up * (r2 * Hc * 0.4), isImp) + breakersAlong(C, r0);
            let v = select(d3 * (c * (r1 * 0.3 + 0.65)) + up * (r2 * 0.4 + 0.15), d3 * (c * (r1 * 0.3 + 0.4)) + up * (r3 * 0.9 + 0.4), isImp);
            let size = select((r3 * r3 * 0.35 + 0.18) * sqrt(max(Hb, 0.2) / 0.6), sqrt(Hc) * (r3 * 0.3 + 0.45), isImp);
            let life = select(r3 * r3 * 3.0 + 1.2, r3 * 1.5 + 2.5, isImp);
            sprayWrite(ring, p, v, size, SPRAY_MIST, life, tag + r0 * 0.999);
        }
    }
}
