// Tidewater Native — spray particle integration (W4a): Spray.js `_buildUpdate` (dgreenheck/tidewater
// @ 4811ba48, MIT, © DRG Software Solutions LLC). A second entry point of the breaker kernel's
// module (same bind group: the surface block, terrain, shore field, FFT and the spray ring).
//
// OP adaptations (see spray.rs):
// * the water height under a particle is the sea level plus the FFT's vertical displacement at
//   that point and the shore waves' (Tidewater: its CPU/GPU water query, which inverts the
//   horizontal displacement too; the difference is a few cm on the surf-zone scale the spray lives
//   in, and the drops only use it to know when they fall back in);
// * W6i: the CPU emit requests (the boats' bow spray, boat_spray.rs) spawn the tail slots after the
//   GPU ring, and the particles collide with up to two bodies (the boats' analytic hulls; no
//   wheelhouse box: OP's boats have no collider tags);
// * the emission budget is steered here (sprayBudgetStep) rather than from a CPU readback;
// * drops falling back in do not deposit foam in the shore simulation yet (its state is a
//   texture written by its own kernel; Tidewater accumulates the deposits in a storage buffer).

// W6i: CPU emit requests, 4 vec4 each (Spray.js): (a.xyz, prefix end) (b.xyz, size)
// (vel.xyz, kind) (velSpread, posSpread, life, sizeJitter)
@group(0) @binding(13) var<storage, read> sprayReq: array<vec4<f32>>;

// Push a particle out of body j (its hull) through the nearest face; the velocity into it
// (relative to the body) is removed and the rest damped (the water runs off as a film); drops
// that hit it die soon after (Spray.js sprayCollideBody, without the wheelhouse box).
fn sprayCollideBody(j: u32, p: ptr<function, vec3<f32>>, v: ptr<function, vec3<f32>>, age: f32, life: ptr<function, f32>, isMist: bool) {
    let o = sprayP.bodies[j * 8u].xyz;
    let X = sprayP.bodies[j * 8u + 1u].xyz;
    let Y = sprayP.bodies[j * 8u + 2u].xyz;
    let Z = sprayP.bodies[j * 8u + 3u].xyz;
    let bodyVel = sprayP.bodies[j * 8u + 4u].xyz;
    let H = sprayP.bodies[j * 8u + 5u];
    let W = sprayP.bodies[j * 8u + 6u];
    let S = sprayP.bodies[j * 8u + 7u];
    let d = *p - o;
    var q = vec3<f32>(dot(d, X), dot(d, Y), dot(d, Z));
    let sheer = mix(S.x, S.y, sat((q.z - H.x) / max(H.z - H.x, 0.01)));
    let fh = sat(q.y / max(sheer, 0.1));
    let zSh = mix(W.x, H.y, fh); let zSt = mix(W.y, H.z, fh); let HB = mix(W.z, H.w, fh);
    let bowL = max(zSt - zSh, 0.01);
    let e = sat((q.z - zSh) / bowL);
    let c = sqrt(max(1.0 - e * e, 0.02));
    let hb = HB * c;
    let inHull = q.z > H.x && q.z < zSt && abs(q.x) < hb && q.y < sheer && q.y > S.z;
    if (inHull) {
        var n = vec3<f32>(0.0, 1.0, 0.0);
        let sx = select(-1.0, 1.0, q.x > 0.0);
        if (hb - abs(q.x) < sheer - q.y) {
            // through the side; on the bow the side faces forward too (- d hb / dz)
            n = normalize(vec3<f32>(sx, 0.0, HB * e / (c * bowL)));
            q.x = sx * (hb + 0.02);
        } else {
            q.y = sheer + 0.02;
        }
        let nw = normalize(X * n.x + Y * n.y + Z * n.z);
        var vr = *v - bodyVel;
        let vn = dot(vr, nw);
        if (vn < 0.0) { vr -= nw * vn; }
        *v = bodyVel + vr * 0.5;
        *p = o + X * q.x + Y * q.y + Z * q.z;
        // water that hits it wets it and runs off (gone); mist flows around it and settles
        *life = min(*life, select(age, age + 0.25, isMist));
    }
}

// per-kind constants: droplet, mist, ligament, spray, sheet
fn sprayTau(k: f32) -> f32 { return sprayByKind(k, 3.0, 0.35, 5.0, 1.8, 2.5); }
fn sprayGrav(k: f32) -> f32 { return sprayByKind(k, 9.81, 0.3, 9.81, 8.5, 9.81); }
fn sprayTurb(k: f32) -> f32 { return sprayByKind(k, 0.0, 0.5, 0.0, 0.3, 0.1); }
fn sprayGrow(k: f32) -> f32 { return sprayByKind(k, 0.0, 0.22, 0.0, 0.3, 0.7); }
fn sprayDies(k: f32) -> f32 { return sprayByKind(k, 1.0, 0.0, 1.0, 1.0, 1.0); }

// all four cascades' vertical displacement, attenuated in shallow water as the surface does
fn sprayFftHeight(p: vec2<f32>, depth: f32) -> f32 {
    var y = 0.0;
    var floorAmt = array<f32, 4>(0.0, 0.05, 0.25, 0.5);
    for (var c = 0; c < 4; c++) {
        let L = ocean.sizes[c].x;
        let d0 = min(40.0, L * 0.08);
        y += textureSampleLevel(oceanDisplacement, smpLinearRepeat, p / L, c, 0.0).y
            * mix(floorAmt[c] * smoothstep(0.0, 0.6, depth), 1.0, smoothstep(0.0, d0, depth));
    }
    return y * tw.surface.x;
}

fn sprayWaterHeightAt(xz: vec2<f32>, ground: f32) -> f32 {
    let depth = tw.sea.x - ground;
    var h = tw.sea.x + sprayFftHeight(xz, max(depth, 0.0));
    if (tw.shore1.w > 0.5 && depth < 26.0) {
        let s = shoreEvaluateNoNormal(xz, depth, ground);
        h += s.disp.y;
        if (s.swashCovered > 0.5) { h = max(h, s.swashLevel); }
    }
    return h;
}

// Breakers._budget, on the GPU: emitting faster than the ring size per particle lifetime
// overwrites particles a few frames after they are born, so the emission scale is steered to
// hold ~2 s of spray in the ring (rate -> ring / 2 per second). Tidewater reads the head back
// a few frames late and steers on the CPU; here thread 0 does it after this frame's emission.
fn sprayBudgetStep(dt: f32) {
    let head = atomicLoad(&sprayHead[0]);
    let prev = atomicLoad(&sprayHead[1]);
    atomicStore(&sprayHead[1], head);
    if (dt <= 0.0) { return; }
    let emitted = f32(head - prev); // (uint wrap safe)
    // rate over ~1 s
    let k = 1.0 - exp(-dt);
    let rate = mix(bitcast<f32>(atomicLoad(&sprayHead[2])), emitted / dt, k);
    atomicStore(&sprayHead[2], bitcast<u32>(rate));
    let budget = bitcast<f32>(atomicLoad(&sprayHead[3]));
    let goal = f32(sprayP.seed.z) / 2.0;
    let want = select(1.0, budget * sqrt(goal / rate), rate > 1.0);
    // Tidewater eases by 0.04 per (60 Hz) frame
    let eased = clamp(budget + (want - budget) * min(0.04 * dt * 60.0, 1.0), 0.05, 1.0);
    atomicStore(&sprayHead[3], bitcast<u32>(eased));
}

@compute @workgroup_size(64, 1, 1)
fn spray_update(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    let par = sprayP.seed.x & 1u;
    if (i == 0u) {
        sprayBudgetStep(sprayP.a.x);
        // (diagnostics) this frame counts into [4 + parity]; the other slot restarts for the next
        atomicStore(&sprayHead[5u - par], 0u);
        atomicAdd(&sprayHead[6], 1u);
    }
    if (i >= sprayP.seed.y) { return; }
    let n = sprayP.seed.y;
    // ---- W6i: the CPU tail, spawned from this frame's requests (Spray.js)
    let NG = sprayP.seed.z;
    let NC = n - NG;
    if (i >= NG && NC > 0u) {
        let k = (i - NG + NC - sprayP.req.x) % NC;
        if (k < sprayP.req.y) {
            var r = 0u;
            for (var j = 0u; j < sprayP.req.z; j++) {
                r = j;
                if (f32(k) < sprayReq[j * 4u].w) { break; }
            }
            let r0 = sprayReq[r * 4u];
            let r1 = sprayReq[r * 4u + 1u];
            let r2 = sprayReq[r * 4u + 2u];
            let r3 = sprayReq[r * 4u + 3u];
            let h0 = sprayRand(i, 11u); let h1 = sprayRand(i, 12u); let h2 = sprayRand(i, 13u);
            let h3 = sprayRand(i, 14u); let h4 = sprayRand(i, 15u); let h5 = sprayRand(i, 16u);
            let h6 = sprayRand(i, 17u);
            let along = mix(r0.xyz, r1.xyz, h0);
            let jit = vec3<f32>(h1 - 0.5, h2 - 0.5, h3 - 0.5) * (r3.y * 2.0);
            let vj = vec3<f32>(h4 - 0.5, h5 - 0.5, h6 - 0.5) * (r3.x * 2.0);
            let size = r1.w * (1.0 + (h2 - 0.5) * r3.w);
            let life0 = r3.z * (h3 * 0.5 + 0.75);
            sprayWrite(i, along + jit, r2.xyz + vj, size, r2.w, life0, h5);
        }
    }
    let info = sprayBuf[2u * n + i];
    if (info.y <= 0.0) { return; }
    let P = sprayBuf[i];
    let Vv = sprayBuf[n + i];
    let dt = sprayP.a.x;
    var p = P.xyz;
    var v = Vv.xyz;
    let age = P.w + dt;
    let kind = info.x;

    // wind near the surface (~70% of the 10 m wind), gusty
    let gust = sin(sprayP.a.y * 0.7 + p.x * 0.05) * 0.25 + 0.85;
    let wind = vec3<f32>(tw.wind.x, 0.0, tw.wind.y) * (tw.sea.z * 0.7 * gust);
    // drag toward the wind: small drops follow the air; mist first keeps moving with the air the
    // wave pushes ahead of it, then joins the wind
    let isMist = kind > 0.5 && kind < 1.5;
    let tau = sprayTau(kind) * select(1.0, exp(age * -1.2) * 4.0 + 1.0, isMist);
    let fs = fract(info.w);
    let turb = vec3<f32>(
        sin(age * 2.1 + fs * 40.0),
        sin(age * 1.7 + fs * 17.0) * 0.5,
        cos(age * 1.9 + fs * 29.0)) * sprayTurb(kind);
    v += ((wind - v) / tau + turb) * dt;
    v.y -= sprayGrav(kind) * dt;
    p += v * dt;

    var life = info.y;
    for (var j = 0u; j < min(sprayP.req.w, 2u); j++) {
        sprayCollideBody(j, &p, &v, age, &life, isMist);
    }
    // water surface and ground below
    let ground = terrainHeightAt(p.xz);
    let hw = sprayWaterHeightAt(p.xz, ground);
    let top = max(hw, ground);

    // drops die in the water / on the sand; mist skims over it
    if (p.y < top && v.y < 0.0 && age > 0.04) {
        if (sprayDies(kind) > 0.5) {
            life = 0.0;
        } else {
            p.y = top + 0.02;
            v.y = max(v.y, 0.0);
            v = vec3<f32>(v.x * 0.95, v.y, v.z * 0.95);
        }
    }
    if (age > life) { life = 0.0; }

    // mist and spray clouds grow as they dilute
    let size = Vv.w * (1.0 + dt * sprayGrow(kind));
    sprayBuf[i] = vec4<f32>(p, age);
    sprayBuf[n + i] = vec4<f32>(v, size);
    sprayBuf[2u * n + i] = vec4<f32>(info.x, life, hw, info.w);
    if (life > 0.0) { atomicAdd(&sprayHead[4u + par], 1u); }
}
