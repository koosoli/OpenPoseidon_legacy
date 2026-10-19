// Tidewater Native W8b (OP) — explosions in the water surface: a ring wave running out from the
// burst, the cavity it leaves collapsing, and a patch of foam and bubbly water spreading and fading.
// Appended to the water material; the lanes come from impacts.rs (tw.impacts, 2 per slot):
//   [0] burst xz (m), age (s), plume height (m; 0 = unused slot)
//   [1] column radius (m), indirectHitRange (m), ring amplitude (m), unused

const IMPACT_SLOTS: i32 = 4;

// h, dh/dx, dh/dz of one burst at xz
fn impactWave(s: i32, xz: vec2<f32>) -> vec3<f32> {
    let l0 = tw.impacts[s * 2];
    let l1 = tw.impacts[s * 2 + 1];
    let H = l0.w;
    if (H <= 0.0) { return vec3<f32>(0.0); }
    let age = l0.z;
    let R0 = max(l1.x, 0.4);
    let rel = xz - l0.xy;
    let d = length(rel);
    // the ring: a short packet running out at a few m/s (faster for a bigger burst), widening
    let c = 3.0 + 0.12 * H;
    let Rw = R0 * 0.5 + c * age;
    let w = 1.2 + 0.08 * H + 0.35 * age;
    if (d > Rw + 3.5 * w) { return vec3<f32>(0.0); }
    let A = l1.z * exp(-age / 3.5) * inverseSqrt(1.0 + Rw / R0);
    let u = (d - Rw) / w;
    let env = exp(-u * u);
    let k = PI / w;
    let ph = k * (d - Rw);
    var h = A * cos(ph) * env;
    var dhdd = A * env * (-k * sin(ph) - 2.0 * u / w * cos(ph));
    // the cavity under the burst, filling in within half a second
    let cav = -min(0.05 * H, 2.0) * exp(-age / 0.35);
    let g = exp(-(d * d) / (R0 * R0));
    h += cav * g;
    dhdd += cav * g * (-2.0 * d / (R0 * R0));
    let dir = rel / max(d, 1e-3);
    return vec3<f32>(h, dhdd * dir.x, dhdd * dir.y);
}

// Displacement (vec3) of the bursts at world xz (Lagrangian point of the ocean grid).
fn impactDisplacement(xz: vec2<f32>) -> vec3<f32> {
    var h = 0.0;
    for (var s = 0; s < IMPACT_SLOTS; s++) { h += impactWave(s, xz).x; }
    return vec3<f32>(0.0, h, 0.0);
}

// slopes (dh/dx, dh/dz), foam, aeration (0..1)
struct ImpactFrag { slopes: vec2<f32>, foam: f32, aeration: f32 };
fn impactFragment(xz: vec2<f32>) -> ImpactFrag {
    var o: ImpactFrag;
    o.slopes = vec2<f32>(0.0);
    o.foam = 0.0;
    o.aeration = 0.0;
    for (var s = 0; s < IMPACT_SLOTS; s++) {
        let l0 = tw.impacts[s * 2];
        let l1 = tw.impacts[s * 2 + 1];
        if (l0.w <= 0.0) { continue; }
        let wv = impactWave(s, xz);
        o.slopes += wv.yz;
        let age = l0.z;
        let R0 = max(l1.x, 0.4);
        let d = length(xz - l0.xy);
        // the foam the column falls back into, spreading with the blast range and fading over
        // several seconds, broken up by a noise fixed in the water; the ring crest whitens early
        let fR = R0 * (1.1 + 0.9 * sqrt(age)) + l1.y * 0.25;
        let mottle = wakeRNoise(xz * 0.35 + 3.1) * 0.6 + wakeRNoise(xz * 1.1 + 9.7) * 0.4;
        let disc = (1.0 - smoothstep(0.55 * fR, fR, d)) * exp(-age / 8.0);
        let c = 3.0 + 0.12 * l0.w;
        let Rw = R0 * 0.5 + c * age;
        let w = 1.2 + 0.08 * l0.w + 0.35 * age;
        let u = (d - Rw) / w;
        let ringF = exp(-u * u) * exp(-age / 2.0) * 0.5;
        o.foam += sat(disc * (0.5 + mottle) + ringF);
        o.aeration += (1.0 - smoothstep(0.5 * fR, 1.1 * fR, d)) * exp(-age / 4.0);
    }
    o.foam = sat(o.foam);
    o.aeration = sat(o.aeration);
    return o;
}

// (OP) Blood in the water (a wounded whale, Whale.cpp; impacts.rs blood lanes: x, z, age, amount per
// stain). A cloud that spreads out from the wound and thins over two minutes, its edge broken up by the
// same fixed noise as the foam. 0..1: how much blood the water at xz holds near the surface.
fn bloodAmount(xz: vec2<f32>) -> f32 {
    var b = 0.0;
    for (var s = 0; s < 16; s++) {
        let l = tw.blood[s];
        if (l.w <= 0.0) { continue; }
        let age = l.z;
        let R = (0.8 + 1.1 * sqrt(l.w)) * (1.0 + 0.55 * sqrt(age));
        let d = length(xz - l.xy);
        if (d > R * 1.4) { continue; }
        let mottle = wakeRNoise(xz * 0.45 + 5.3) * 0.65 + wakeRNoise(xz * 1.3 + 1.7) * 0.35;
        let edge = 1.0 - smoothstep(0.35 * R, R * (0.9 + 0.5 * mottle), d);
        // the same blood spread over a wider patch, and slowly lost
        let thin = exp(-age / 90.0) / (1.0 + 0.02 * age);
        b += edge * min(l.w, 3.0) * 0.5 * thin * (0.7 + 0.6 * mottle);
    }
    return sat(b);
}
