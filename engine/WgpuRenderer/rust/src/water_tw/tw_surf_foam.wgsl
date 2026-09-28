// Tidewater Native — the surf-zone foam look (W3c). Port of SurfFoam.js (dgreenheck/tidewater
// @ 4811ba48, MIT, © DRG Software Solutions LLC): SurfFoamArgs, SurfFoamInfo, surfFoamPhaseHG,
// surfFoamHash11, surfFoamFlowLace, surfFoamBigAt, surfFoamShading, surfFoamLight.
// Constants are Tidewater's: bump 0.7, flow-map period 1.2 s, max flow 2.5 m/s, lace tile 3.5 m.
//
// Adaptations to OP, all mechanical:
// * Derivatives. Tidewater samples the lace with textureSample inside the surf branches and takes
//   dpdx/dpdy in surfFoamLight, which the material calls in a branch; naga enforces derivative
//   uniformity, so here the lace is sampled with textureSampleGrad (the gradient of the pattern
//   coordinate, from the fragment's world-position derivatives taken in uniform control flow and
//   carried in SurfFoamArgs), and the relief gradients surfFoamLight needs are taken by the
//   material at its top level (SurfFoamGrad). The gradients are those implicit derivatives would
//   give, without the jumps at the flow-map resets (where implicit derivatives spike).
// * shoreDirAt (Tidewater's regional direction texture) is the shore field's direction
//   (terrainShoreSample), Tidewater's own fallback; shoreSimInside(shoreSimUvOf(xz)) is the
//   weighted coverage of OP's simulated regions (shoreSimInsideAt).
// * frame.time / frame.seaLevel / frame.sunDir / frame.skyIrradiance are tw.sea.y / tw.sea.x /
//   the material's L / its sky irradiance (passed in).

const SURF_FOAM_PERIOD: f32 = 1.2;
const SURF_FOAM_MAX_FLOW: f32 = 2.5;
const SURF_FOAM_KB: f32 = 0.028; // bump 0.7 x 0.04

struct SurfFoamArgs {
    coverage: f32,
    foam: f32,
    footprint: f32,
    depth: f32,
    bubbles: f32,
    lagXZ: vec2<f32>,
    normal: vec3<f32>,
    baseNormal: vec3<f32>,
    fresh: f32,
    sim: f32,
    simState: vec4<f32>,
    roller: f32,
    P: vec3<f32>,
    // (OP) derivatives of the world position, taken in uniform control flow
    dPdx: vec3<f32>,
    dPdy: vec3<f32>,
    // (OP) the material's direction to the sun
    L: vec3<f32>,
};

struct SurfFoamInfo {
    foam: f32,
    density: f32,
    height: f32,
    surf: f32,
    ww: f32,
    relief: f32,
    selfShadow: f32,
    cavity: f32,
    reliefPat: f32,
    reliefK: f32,
    thinPat: f32,
    thinK: f32,
    bubPat: f32,
    bubK: f32,
};

// (OP) the screen-space gradients surfFoamLight differentiates, taken by the material at its top level
struct SurfFoamGrad {
    dpx: vec3<f32>,
    dpy: vec3<f32>,
    dhdx: f32,
    dhdy: f32,
};

fn surfFoamGrad(info: SurfFoamInfo, dpx: vec3<f32>, dpy: vec3<f32>) -> SurfFoamGrad {
    // (called in uniform control flow: see the header)
    let dhdx = dpdx(info.reliefPat) * info.reliefK + (dpdx(info.thinPat) * info.thinK + dpdx(info.bubPat) * info.bubK) * SURF_FOAM_KB;
    let dhdy = dpdy(info.reliefPat) * info.reliefK + (dpdy(info.thinPat) * info.thinK + dpdy(info.bubPat) * info.bubK) * SURF_FOAM_KB;
    return SurfFoamGrad(dpx, dpy, dhdx, dhdy);
}

// Henyey-Greenstein phase (1/sr)
fn surfFoamPhaseHG(cosT: f32, g: f32) -> f32 {
    let g2 = g * g;
    return ((1.0 - g2) / (4.0 * PI)) / pow(max(1.0 + g2 - cosT * 2.0 * g, 1e-4), 1.5);
}

fn surfFoamHash11(x: f32) -> f32 { return fract(sin(x * 91.7 + 17.3) * 43758.5453); }

// Lace distance (0 on a bubble strand .. 1 in a hole) at pattern coordinates q (m), carried by
// flow (m/s in the same coordinates) with the dual-phase flow map. salt decorrelates layers.
// dqdx / dqdy: screen-space gradient of q (OP, see the header).
fn surfFoamFlowLace(q: vec2<f32>, dqdx: vec2<f32>, dqdy: vec2<f32>, flow: vec2<f32>, salt: f32) -> vec4<f32> {
    let t = tw.sea.y / SURF_FOAM_PERIOD;
    let v = clamp(flow, vec2<f32>(-SURF_FOAM_MAX_FLOW), vec2<f32>(SURF_FOAM_MAX_FLOW)) * SURF_FOAM_PERIOD;
    var out = vec4<f32>(0.0);
    for (var k = 0; k < 2; k++) {
        let tk = t + (f32(k) * 0.5 + salt * 0.37);
        let ph = fract(tk);
        let cycle = floor(tk) + (f32(k) * 13.1 + salt * 5.7);
        let jitter = vec2<f32>(surfFoamHash11(cycle), surfFoamHash11(cycle + 0.5)) * 7.0;
        let p = (q - v * (ph - 0.5)) / SHORE_SIM_LACE_TILE + jitter;
        let w = 1.0 - abs(ph * 2.0 - 1.0);
        out += textureSampleGrad(shoreSimLace, smpAniso4Repeat, p, dqdx / SHORE_SIM_LACE_TILE, dqdy / SHORE_SIM_LACE_TILE) * w;
    }
    return out; // the two weights always add up to 1
}

fn surfFoamBigAt(qq: vec2<f32>, dqdx: vec2<f32>, dqdy: vec2<f32>, pflow: vec2<f32>) -> f32 {
    return sqrt(surfFoamFlowLace(qq / 6.0, dqdx / 6.0, dqdy / 6.0, pflow / 6.0, 2.0).x);
}

// the along-crest pattern coordinate of a steep face (Tidewater's qv) and its gradient
struct SurfFoamQv { q: vec2<f32>, dx: vec2<f32>, dy: vec2<f32> };
fn surfFoamQv(xz: vec2<f32>, y: f32, tangent: vec2<f32>, dPdx: vec3<f32>, dPdy: vec3<f32>) -> SurfFoamQv {
    let al = dot(xz, tangent);
    let q = vec2<f32>(al + sin(al * 0.19 + 0.8) * 2.1 + sin(al * 0.47 + 2.9) * 0.6 + sin(y * 1.7 + al * 0.11) * 0.5, y * 1.1);
    // d q.x / d al and d q.x / d y
    let c3 = cos(y * 1.7 + al * 0.11) * 0.5;
    let qa = 1.0 + cos(al * 0.19 + 0.8) * 0.399 + cos(al * 0.47 + 2.9) * 0.282 + c3 * 0.11;
    let qy = c3 * 1.7;
    let dalx = dot(dPdx.xz, tangent);
    let daly = dot(dPdy.xz, tangent);
    return SurfFoamQv(q, vec2<f32>(qa * dalx + qy * dPdx.y, dPdx.y * 1.1), vec2<f32>(qa * daly + qy * dPdy.y, dPdy.y * 1.1));
}

// coverage: total foam amount (0..1, all sources); foam: the default (offshore whitecap) foam;
// footprint: pixel size on the surface (m); depth: sea depth; bubbles: fine bubble detail;
// normal: surface normal; fresh: whitewater made by the breaking wave right here (roller,
// plunge point, swash front); sim: foam carried by the water (ShoreSim, already kept off the
// clear face of plunging waves); simState: shoreSimSample() at this pixel; roller: relief of
// the whitewater roller (m)
fn surfFoamShading(a: SurfFoamArgs) -> SurfFoamInfo {
    // world position of the fragment (the pattern is in world space); a caller that left P unset
    // (zero) gets the rest position instead
    let P = select(a.P, vec3<f32>(a.lagXZ.x, tw.sea.x, a.lagXZ.y), all(a.P == vec3<f32>(0.0)));
    let xz = P.xz;
    let dxz = a.dPdx.xz;
    let dyz = a.dPdy.xz;
    let coverage = a.coverage;
    var opacity = a.foam;
    var density = sat(coverage);
    var height = 0.0;
    var wwOut = 0.0;
    var wwRelief = 0.0;
    var wwShadow = 1.0;
    var wwCav = 1.0;
    var reliefPat = 0.0; var reliefK = 0.0;
    var thinPat = 0.0; var thinK = 0.0;
    var bubPat = 0.0; var bubK = 0.0;
    // surf look near the beach, the default whitecap look offshore
    let surf = smoothstep(7.0, 3.0, a.depth) * shoreSimInsideAt(xz);
    if (surf > 0.0 && coverage > 0.04) {

        // flow of the water here, from the shore simulation (along the local wave direction)
        let de = terrainShoreSample(xz).yz;
        let dir = de / max(length(de), 1e-4);
        let speed = a.simState.w;
        let flow = dir * speed;

        let steep = smoothstep(0.82, 0.5, a.baseNormal.y);
        let ww = sat(a.fresh * 1.4);
        // (W10f, OP) Tidewater's 0.12 m / 0.03-0.12 m: at OP's view distances and FOV nearly all of
        // a beach's whitewater band seen from the shore is past that, and the band was drawn as its
        // flat average (the flat white stripe); the lace and lumps are mip-filtered
        // (textureSampleGrad), so they are kept out to 0.3 m
        let farOnly = a.footprint >= 0.3;
        var flat = vec4<f32>(0.5);
        if (!farOnly) { flat = surfFoamFlowLace(xz, dxz, dyz, flow, 0.0); }
        var lace = flat;
        var lumps = 0.5;
        let tangent = vec2<f32>(-dir.y, dir.x);
        let qv = surfFoamQv(xz, P.y, tangent, a.dPdx, a.dPdy);
        if (steep > 0.01 && !farOnly) {
            let vert = surfFoamFlowLace(qv.q, qv.dx, qv.dy, vec2<f32>(0.0, -0.9), 1.0);
            lace = mix(flat, vert, steep);
        }
        var bigFar = 0.5;
        if (ww > 0.02) {
            let L = a.L;
            let pq = mix(xz, qv.q, steep);
            let pqx = mix(dxz, qv.dx, steep);
            let pqy = mix(dyz, qv.dy, steep);
            let pflow = mix(flow, vec2<f32>(0.0, -0.9), steep);
            let big = surfFoamBigAt(pq, pqx, pqy, pflow);
            bigFar = big;
          if (!farOnly) {
            let mid = sqrt(surfFoamFlowLace(pq / 2.2, pqx / 2.2, pqy / 2.2, pflow / 2.2, 3.0).x);
            lumps = big * 0.6 + mid * 0.4;
            let A = 0.22; // relief of the lumps (m)
            reliefPat = big * A + mid * (A * 0.45) + (1.0 - lace.x) * (A * 0.12);
            wwRelief = reliefPat * ww;
            let Lp = mix(L.xz, vec2<f32>(dot(L.xz, tangent), L.y * 1.1), steep);
            let Ld = Lp / max(length(Lp), 1e-3);
            let NdL = dot(a.normal, L);
            let tanE = NdL / max(length(L - a.normal * NdL), 0.05);
            var occ = 0.0;
            var steps = array<f32, 3>(0.14, 0.34, 0.7);
            for (var i = 0; i < 3; i++) {
                let d = steps[i];
                let hk = surfFoamBigAt(pq + Ld * d, pqx, pqy, pflow);
                occ = max(occ, smoothstep(0.0, 0.05, (hk - big) * A - tanE * d));
            }
            wwShadow = 1.0 - occ * mix(0.85, 0.7, steep);
            // (W10f, OP) deeper dips between the lumps (Tidewater 0.5)
            wwCav = mix(0.38, 1.0, smoothstep(0.05, 0.75, lumps));
          }
        }

        let boil = lace.x * 0.7 + lace.z * 0.3;
        let wwEdge = smoothstep(0.25, 0.75, ww * 1.35 - boil * 0.3 - (1.0 - lumps) * 0.5);
        let whitewater = (ww * 0.25 + wwEdge * 0.75) * ((1.0 - boil) * 0.12 + 0.88);

        let c = sat((a.sim + max(coverage - a.sim - a.fresh, 0.0) * 0.5 - 0.05) / 0.95);
        let w = max(pow(c, 1.4) * 0.9 * (lace.z * 0.5 + 0.75) + (lace.y - 0.5) * 0.06, 0.0);
        let soft = 0.05 + a.footprint * 4.5;
        let mat = (1.0 - smoothstep(w - soft, w + soft, lace.x)) * smoothstep(0.0, 0.05, c);
        let bub = lace.y * smoothstep(w + 0.25, w, lace.x) * smoothstep(0.02, 0.2, c) * 0.5;
        let inner = sat((w - lace.x) / 0.25);
        let laceFoam = max(mat * sat(inner * 0.35 + 0.45 + lace.z * 0.3), bub);

        let far = smoothstep(0.08, 0.3, a.footprint);
        // (W10f, OP) far off the whitewater keeps its big lumps: patchy cover, shaded dips
        let bigK = smoothstep(0.1, 0.8, bigFar);
        let average = max(sat(pow(w, 1.45) * 1.9) * 0.8, ww * mix(0.72, 0.95, bigK));
        let near = max(laceFoam, whitewater);
        opacity = mix(a.foam, mix(near, average, far), surf);

        density = max(sat(c * 1.3) * (inner * 0.5 + 0.5), ww);
        let reliefThin = inner * sat(c * 1.5);
        let relief = reliefThin * (1.0 - ww);
        wwOut = ww * (1.0 - far * 0.6);
        wwShadow = mix(wwShadow, mix(0.7, 0.9, bigK), far);
        wwCav = mix(wwCav, mix(0.5, 0.95, bigK), far);
        wwRelief *= 1.0 - far;
        height = (relief + a.bubbles * 0.15) * surf * (1.0 - far);
        reliefK = ww * (1.0 - far);
        thinPat = reliefThin;
        thinK = (1.0 - ww) * surf * (1.0 - far);
        bubPat = a.bubbles * 0.15;
        bubK = surf * (1.0 - far);
    }

    return SurfFoamInfo(opacity, density, height, surf, wwOut, wwRelief, wwShadow, wwCav, reliefPat, reliefK, thinPat, thinK, bubPat, bubK);
}

// Foam radiance: a dense scatterer, wrapped diffuse sun (light diffuses through the bubbles),
// sky ambient, darker in the dips of the bubbly relief, glowing at thin edges when backlit.
// g: the gradients from surfFoamGrad; sky: the sky irradiance (Tidewater frame.skyIrradiance).
fn surfFoamLight(info: SurfFoamInfo, g: SurfFoamGrad, N: vec3<f32>, L: vec3<f32>, V: vec3<f32>, sun: vec3<f32>, sky: vec3<f32>) -> vec3<f32> {
    let ww = info.ww;
    let cavity = info.cavity;
    // relief normal from the screen-space gradient of the height (Mikkelsen surface gradient)
    let dpx = g.dpx;
    let dpy = g.dpy;
    let r1 = cross(dpy, N);
    let r2 = cross(N, dpx);
    let det = dot(dpx, r1);
    let grad = (r1 * g.dhdx + r2 * g.dhdy) * sign(det);
    let Nf = normalize(N * abs(det) - grad + N * 1e-6);
    let NdL = dot(Nf, L);
    let wrap = mix(0.45, 0.12, ww);
    let diff = sat(NdL * (1.0 - wrap) + wrap) * mix(1.0, info.selfShadow, ww);
    let ao = mix(mix(1.0, info.height * 0.3 + 0.76, info.surf), cavity, ww);
    let thin = (1.0 - info.density) * 0.8 + ww * (1.0 - cavity) * 0.3;
    // (W10d, OP) x0.45 (Tidewater 1.3): looking toward a low sun the transmitted lobe alone was
    // ~3x the lit foam's own radiance and the whole surf band clipped to flat white under OP's
    // exposure; backlit whitewater reads grey with bright thin edges
    let trans = surfFoamPhaseHG(dot(-V, L), 0.55) * thin * 0.45;
    let transCol = mix(vec3<f32>(1.0), vec3<f32>(0.6, 0.92, 0.82), ww * 0.7 + 0.3);
    let skyGrey = vec3<f32>(dot(sky, vec3<f32>(0.2126, 0.7152, 0.0722)));
    let amb = mix(sky, skyGrey, ww * 0.35) * ao + sun * (ww * 0.05) * ao;
    return (sun * (diff * mix(1.0, sqrt(cavity), ww) / PI + transCol * trans) + amb) * 0.86;
}
