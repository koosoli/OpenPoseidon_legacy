// Tidewater Native — the underwater composite (W5a): post/Underwater.js `underwaterMedium` and
// `underwaterComposite` (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC),
// as OP's backend underwater pass (water_backend.rs `render_underwater`: resolved HDR scene +
// single-sample reversed-Z depth in, composited scene out, before DoF / bloom / tonemap).
//
// OP adaptations (see underwater.rs in this directory):
// * the medium at the lens is decided per pixel here, not by a medium pass fed by a mask the water
//   material writes (OP's water pass has one colour target): near the surface (|camera height
//   over the water| < UW_STRADDLE) a pixel is water when the point it sees is below the water
//   surface there, or, seeing the sky, when it looks down; farther, the camera's medium;
// * (W9a) the water height is the drawn surface's own near the camera -- FFT, shore waves and swash
//   sheet, wakes, bursts -- from the probe grid (probe.rs, tw_probe.wgsl), so the split at the lens
//   follows the surface drawn wherever the camera is (in the surf, under a breaking crest, bobbing
//   at the waterline); beyond the grid (scene points further than 4 m) the sea level plus the
//   FFT's vertical displacement;
// * the caustic light shafts are marched here at full resolution with fewer steps (W5b; Tidewater:
//   a half-resolution pass, 20 steps, resolved by its TAA);
// * (W5e) the diver's torch is OP's own spot light at the eye (the dev head torch, or any spot
//   within 2 m of the camera), with OP's cone and fall-off (lighting.wgsl: flat core, inverse
//   square after, the cone ramped from the axis), marched at full resolution, 8 jittered steps;
// * OP lights submerged surfaces as if in air: points seen below the surface get Tidewater's
//   UnderwaterLighting attenuation here, in post (as the water material does from above, W3f);
// * light in Tidewater's units from OP's sun and ambient (see the water material), the in-scatter
//   scaled as the water's own glow seen from below (W3j.1: TW water glow, at least 0.12).

// (W9a) within this of the surface the medium is decided per pixel on the lens (was 0.35 m while
// the camera's height came from the FFT alone; the probe's is the drawn surface's own)
const UW_STRADDLE: f32 = 1.0;
const UW_BAND: i32 = 16;
const UW_IOR: f32 = 1.333;
const UW_PI: f32 = 3.141592653589793;

struct UwParams {
    inv_view_proj: mat4x4<f32>, // inv(view) * inv(proj), camera-relative (Frame.inv_view_proj)
    cam: vec4<f32>,             // camera world position, lens distance (near plane, m)
    sun_dir: vec4<f32>,         // toward the sun (world), w: in-scatter scale (water glow, >= 0.12)
    sun_e: vec4<f32>,           // sun irradiance (Tidewater units), w: meniscus half-width px
    sky_e: vec4<f32>,           // sky irradiance / PI, w: enabled
    absorption: vec4<f32>,      // 1/m
    scattering: vec4<f32>,      // 1/m, w: sea level
    ocean: vec4<f32>,           // FFT amplitude, cascade count, caustic tiles (fine, broad) m
    sizes: vec4<f32>,           // FFT cascade sizes (m)
    torch_pos: vec4<f32>,       // (W5e) the torch: world position, w: on
    torch_dir: vec4<f32>,       // beam direction, w: cos^2 of the outer cone
    torch_col: vec4<f32>,       // irradiance in the core (Tidewater units), w: core radius (m)
    torch_shape: vec4<f32>,     // x: cone shaping exponent, y: beam in-scatter strength
    probe: vec4<f32>,           // (W9a) the probe grid: camera x, z, step (m), size (0 = none)
};

@group(0) @binding(0) var<uniform> uw: UwParams;
@group(0) @binding(1) var uwScene: texture_2d<f32>;
@group(0) @binding(2) var uwDepth: texture_depth_2d;
@group(0) @binding(3) var uwDisplacement: texture_2d_array<f32>;
@group(0) @binding(4) var uwRepeat: sampler;
@group(0) @binding(5) var uwClamp: sampler;
@group(0) @binding(6) var causticsFineTex: texture_2d<f32>;  // W5b (caustics.rs)
@group(0) @binding(7) var causticsBroadTex: texture_2d<f32>;
@group(0) @binding(8) var uwProbe: texture_2d<f32>;           // (W9a) r32float surface heights

struct UwOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_uw(@builtin(vertex_index) vi: u32) -> UwOut {
    let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    var o: UwOut;
    o.pos = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

fn uwSat(x: f32) -> f32 { return clamp(x, 0.0, 1.0); }

// (W12d) the drawn mesh's vertex at probe texel i (world position)
fn uwProbeV(i: vec2<i32>) -> vec3<f32> { return textureLoad(uwProbe, i, 0).xyz; }

// height of triangle (a, b, c) at xz when xz lies inside it in plan, else y = 0
fn uwTri(xz: vec2<f32>, a: vec3<f32>, b: vec3<f32>, c: vec3<f32>) -> vec2<f32> {
    let v0 = b.xz - a.xz;
    let v1 = c.xz - a.xz;
    let v2 = xz - a.xz;
    let den = v0.x * v1.y - v1.x * v0.y;
    if (abs(den) < 1e-10) { return vec2<f32>(0.0); }
    let u = (v2.x * v1.y - v1.x * v2.y) / den;
    let v = (v0.x * v2.y - v2.x * v0.y) / den;
    if (u < -1e-4 || v < -1e-4 || u + v > 1.0 + 1e-4) { return vec2<f32>(0.0); }
    return vec2<f32>(a.y + (b.y - a.y) * u + (c.y - a.y) * v, 1.0);
}

// (W9a, W12d) the drawn surface's height at xz: the mesh triangle over xz, from the mesh's own
// vertices (tw_probe.wgsl). The triangles are CDLOD.js's grid (cdlod.rs grid_mesh): in a cell
// (i, j) with i + j even, (a c b) (b c d), else (a c d) (a d b), a = (i, j), b = (i+1, j),
// c = (i, j+1), d = (i+1, j+1); nodes start on even lattice indices, so the parity is the
// world lattice's. The cell under xz is found from the lattice point displaced to it (two
// fixed-point steps), then it and its 8 neighbours are tested. y = 0 outside the grid.
fn uwProbeHeight(xz: vec2<f32>) -> vec2<f32> {
    let nf = uw.probe.w;
    if (nf < 2.0) { return vec2<f32>(0.0); }
    let n = i32(nf);
    let h = uw.probe.z;
    let base = uw.probe.xy;
    var p = xz;
    for (var k = 0; k < 2; k++) {
        let f = clamp((p - base) / h, vec2<f32>(0.0), vec2<f32>(nf - 1.001));
        let i = vec2<i32>(floor(f));
        let t = f - vec2<f32>(i);
        let d00 = uwProbeV(i).xz - (base + vec2<f32>(i) * h);
        let d10 = uwProbeV(i + vec2<i32>(1, 0)).xz - (base + vec2<f32>(i + vec2<i32>(1, 0)) * h);
        let d01 = uwProbeV(i + vec2<i32>(0, 1)).xz - (base + vec2<f32>(i + vec2<i32>(0, 1)) * h);
        let d11 = uwProbeV(i + vec2<i32>(1, 1)).xz - (base + vec2<f32>(i + vec2<i32>(1, 1)) * h);
        p = xz - mix(mix(d00, d10, t.x), mix(d01, d11, t.x), t.y);
    }
    let f = (p - base) / h;
    if (any(f < vec2<f32>(1.0)) || any(f > vec2<f32>(nf - 2.0))) { return vec2<f32>(0.0); }
    let c0 = vec2<i32>(floor(f));
    let g0 = vec2<i32>(round(base / h));
    for (var k = 0; k < 9; k++) {
        let c = c0 + vec2<i32>(k % 3 - 1, k / 3 - 1);
        if (any(c < vec2<i32>(0)) || any(c > vec2<i32>(n - 2))) { continue; }
        let a = uwProbeV(c);
        let b = uwProbeV(c + vec2<i32>(1, 0));
        let cc = uwProbeV(c + vec2<i32>(0, 1));
        let d = uwProbeV(c + vec2<i32>(1, 1));
        let g = g0 + c;
        let even = ((g.x + g.y) & 1) == 0;
        var r: vec2<f32>;
        if (even) {
            r = uwTri(xz, a, cc, b);
            if (r.y < 0.5) { r = uwTri(xz, b, cc, d); }
        } else {
            r = uwTri(xz, a, cc, d);
            if (r.y < 0.5) { r = uwTri(xz, a, d, b); }
        }
        if (r.y > 0.5) { return r; }
    }
    // (no triangle found: a folded patch) the four vertices around the lattice point, averaged
    let i = clamp(c0, vec2<i32>(0), vec2<i32>(n - 2));
    return vec2<f32>((uwProbeV(i).y + uwProbeV(i + vec2<i32>(1, 0)).y + uwProbeV(i + vec2<i32>(0, 1)).y + uwProbeV(i + vec2<i32>(1, 1)).y) * 0.25, 1.0);
}

// water surface height at xz: the drawn surface near the camera (W9a), else sea level + the FFT's
// vertical displacement
fn uwWaterHeight(xz: vec2<f32>) -> f32 {
    let near = uwProbeHeight(xz);
    if (near.y > 0.5) { return near.x; }
    var y = 0.0;
    let n = i32(uw.ocean.y);
    for (var c = 0; c < 4; c++) {
        if (c < n) {
            y += textureSampleLevel(uwDisplacement, uwRepeat, xz / uw.sizes[c], c, 0.0).y;
        }
    }
    return uw.scattering.w + y * uw.ocean.x;
}

fn uwDepthAt(uv: vec2<f32>) -> f32 {
    let size = vec2<f32>(textureDimensions(uwDepth));
    return textureLoad(uwDepth, vec2<i32>(clamp(uv, vec2<f32>(0.0), vec2<f32>(0.9999)) * size), 0);
}

// camera-relative position of the scene at uv (stored reversed-Z depth d > 0)
fn uwRelAt(uv: vec2<f32>, d: f32) -> vec3<f32> {
    let ndc = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    let h = uw.inv_view_proj * vec4<f32>(ndc, 1.0 - d, 1.0);
    return h.xyz / h.w;
}

// world direction of the view ray through uv
fn uwDir(uv: vec2<f32>) -> vec3<f32> {
    return normalize(uwRelAt(uv, 0.5));
}

// the view distance through the medium: to the scene (600 m on the sky), less the lens distance
// (the water between the eye and the near plane is clipped away)
fn uwDist(uv: vec2<f32>, d: f32) -> f32 {
    var dist = 600.0;
    if (d > 1e-7) { dist = min(length(uwRelAt(uv, d)), 600.0); }
    return max(dist - uw.cam.w, 0.0);
}

// medium at the near clip plane (1 water, 0 air); camH: camera height over the water.
// (W5a.1, OP) Near the surface: whether this pixel's point ON the lens (the near plane) is below
// the water there. Tidewater reads the side the rendered surface is seen from, which OP's water
// pass does not write; the lens point is the same answer (the rendered surface crosses the near
// plane exactly where it does), and the water material's view side (its triangle's facing)
// agrees with it, so the split and the surface never disagree.
fn uwMedium(uv: vec2<f32>, camH: f32) -> f32 {
    var water = select(0.0, 1.0, camH < 0.0);
    if (abs(camH) < UW_STRADDLE) {
        let dir = uwDir(uv);
        let fwd = uwDir(vec2<f32>(0.5, 0.5));
        let q = uw.cam.xyz + dir * (uw.cam.w / max(dot(dir, fwd), 0.1));
        water = select(0.0, 1.0, q.y < uwWaterHeight(q.xz));
    }
    return water;
}

// (W5e) OP's spot profile (lighting.wgsl, LGT-012/017): smoothstepped from the axis to the outer
// cone in the angle, shaped by an exponent; 0 outside
fn uwTorchCone(cosToAxis: f32) -> f32 {
    let cOut2 = uw.torch_dir.w;
    if (cosToAxis <= 0.0 || cosToAxis * cosToAxis < cOut2) { return 0.0; }
    let aOut = acos(sqrt(clamp(cOut2, 0.0, 1.0)));
    let aHere = acos(clamp(cosToAxis, 0.0, 1.0));
    let t = clamp((aOut - aHere) / max(aOut, 1e-4), 0.0, 1.0);
    return pow(t * t * (3.0 - 2.0 * t), uw.torch_shape.x);
}

fn uwLit(m: f32, sigT: vec3<f32>, zc: f32, dirY: f32, dist: f32) -> vec3<f32> {
    let a = sigT * zc / m;
    let kk = sigT * (1.0 - dirY / m);
    let e0 = exp(-a);
    let e1 = exp(-max(a + kk * dist, vec3<f32>(0.0)));
    return select((e0 - e1) / kk, e0 * dist, abs(kk) < vec3<f32>(1e-4));
}

@fragment
fn fs_uw(in: UwOut) -> @location(0) vec4<f32> {
    let uv = in.uv;
    let dims = vec2<f32>(textureDimensions(uwScene));
    let camWater = uwWaterHeight(uw.cam.xz);
    let camH = uw.cam.y - camWater;
    let here = uwMedium(uv, camH);
    let under = here > 0.5;

    // distance to the waterline on the lens in pixels, searched vertically
    var lineDist = 1e4;
    var lineDir = 0.0;
    if (abs(camH) < UW_STRADDLE) {
        let px = 1.0 / dims.y;
        let flips = abs(uwMedium(uv - vec2<f32>(0.0, 8.0 * px), camH) - here) + abs(uwMedium(uv + vec2<f32>(0.0, 8.0 * px), camH) - here)
            + abs(uwMedium(uv - vec2<f32>(0.0, f32(UW_BAND) * px), camH) - here) + abs(uwMedium(uv + vec2<f32>(0.0, f32(UW_BAND) * px), camH) - here);
        if (flips > 0.5) {
            for (var i = 1; i < UW_BAND + 1; i++) {
                let up = abs(uwMedium(uv - vec2<f32>(0.0, f32(i) * px), camH) - here) > 0.5;
                let down = abs(uwMedium(uv + vec2<f32>(0.0, f32(i) * px), camH) - here) > 0.5;
                if (lineDist > 1e3 && (up || down)) {
                    lineDist = f32(i) - 0.5;
                    lineDir = select(1.0, -1.0, up);
                }
            }
        }
    }

    // meniscus band: the rounded water edge refracts like a cylindrical lens
    let bandW = uw.sun_e.w;
    let tBand = uwSat(lineDist / bandW);
    let bendPx = tBand * sqrt(max(1.0 - tBand * tBand, 0.0)) * bandW * 0.8;
    let uvB = uv - vec2<f32>(0.0, lineDir * bendPx / dims.y);
    var base = textureSampleLevel(uwScene, uwClamp, uvB, 0.0).rgb;
    var result = base;

    let sigA = uw.absorption.rgb;
    let sigS = uw.scattering.rgb;
    let sigT = sigA + sigS;
    let L = uw.sun_dir.xyz;
    let Ls = -refract(-L, vec3<f32>(0.0, 1.0, 0.0), 1.0 / UW_IOR); // toward the sun, underwater
    let mu = max(Ls.y, 0.15);

    if (under && uw.sky_e.w > 0.5) {
        let d = uwDepthAt(uv);
        let dir = uwDir(uv);
        var dist = uwDist(uv, d);
        // (W5a.1, OP) OP's water writes no depth: a ray that meets the surface from below leaves
        // the water there (Tidewater: the surface's own depth ends it)
        if (dir.y > 0.01) {
            dist = min(dist, max(camWater - uw.cam.y, 0.0) / dir.y);
        }

        // (OP) UnderwaterLighting in post: OP lit the submerged scene as if it stood in air;
        // attenuate its light by the water column above it (direct along the refracted sun,
        // ambient over ~1.2 x the depth), blended by the direct share of a flat bed's irradiance
        if (d > 1e-7) {
            let q = uwRelAt(uv, d) + uw.cam.xyz;
            let zq = uwWaterHeight(q.xz) - q.y;
            if (zq > 0.0) {
                // (W5b) Tidewater's caustic networks on the submerged scene, filtered over the pixel
                let fp = length(uwRelAt(uv, d)) * 2.0 / dims.y;
                let caus = twCausticsSample(causticsFineTex, causticsBroadTex, uwRepeat, uw.ocean.zw, q, zq, L, twCausticsLevel(fp, uw.ocean.z), false);
                let attDirect = exp(-sigT * zq / mu) * caus;
                let attAmbient = exp(-sigT * zq * 1.2) * 0.85 + vec3<f32>(0.0, 0.02, 0.04) * exp(zq * -0.1);
                let lumW = vec3<f32>(0.2126, 0.7152, 0.0722);
                let eSun = dot(uw.sun_e.rgb, lumW) * max(L.y, 0.0) / UW_PI;
                let eSky = dot(uw.sky_e.rgb, lumW);
                let share = eSun / max(eSun + eSky, 1e-5);
                base *= mix(vec3<f32>(1.0), attDirect * share + attAmbient * (1.0 - share), smoothstep(0.0, 0.08, zq));
            }
        }

        let zc = max(camWater - uw.cam.y, 0.0); // camera depth below the surface
        let cosPh = dot(dir, Ls);
        let g = 0.85;
        let phase = (1.0 - g * g) / (4.0 * UW_PI) / pow(max(1.0 + g * g - cosPh * 2.0 * g, 1e-4), 1.5) * 0.75 + 0.25 / (4.0 * UW_PI);
        let sunE = uw.sun_e.rgb * 0.96;
        let ambE = uw.sky_e.rgb * UW_PI * 0.9;
        let bb = sigS * 0.035;
        let msAlb = bb * 1.3 / (sigA + bb);
        let k = uw.sun_dir.w;
        let inSun = sunE * (sigS * phase + msAlb * sigT * (1.0 / UW_PI)) * uwLit(mu, sigT, zc, dir.y, dist) * k;
        let inAmb = ambE * (sigS * (1.0 / (4.0 * UW_PI)) + msAlb * sigT * (1.0 / UW_PI)) * uwLit(0.8, sigT, zc, dir.y, dist) * k;
        // (W5b) caustic light shafts: the focused light scattered toward the eye along the view ray
        // (the excess over the mean: the mean is in inSun), 12 jittered steps over the first 22 m
        var shafts = vec3<f32>(0.0);
        if (uw.ocean.z > 0.0) {
            let steps = 12;
            let maxD = min(dist, 22.0);
            let ds = maxD / f32(steps);
            let jitter = fract(fract(dot(in.pos.xy, vec2<f32>(0.06711056, 0.00583715))) * 52.9829189);
            for (var i = 0; i < steps; i++) {
                let s = (f32(i) + jitter) * ds;
                let p = uw.cam.xyz + dir * s;
                let z = max(camWater - p.y, 0.01);
                let c = twCausticsSample(causticsFineTex, causticsBroadTex, uwRepeat, uw.ocean.zw, p, z, L, 1.5, true);
                shafts += (c - 1.0) * exp(-sigT * (s + z / mu)) * ds;
            }
            shafts = max(shafts * sunE * sigS * phase * 2.5 * k, vec3<f32>(0.0));
        }
        // (W5e) the diver's torch (Underwater.js): single scattering of the beam along the view ray
        // (the lamp sits beside the eye, so the beam is a shaft slightly off the view axis),
        // extinction on the light and view legs, the sun's lobe (0.75 forward, Schlick) + 0.25
        // isotropic, OP's flat-core / inverse-square fall-off; up to the scene, at most 25 m
        var torch = vec3<f32>(0.0);
        if (uw.torch_pos.w > 0.5 && uw.torch_shape.y > 0.0) {
            let tSteps = 8;
            let tMax = min(dist, 25.0);
            let tds = tMax / f32(tSteps);
            let tJit = fract(fract(dot(in.pos.xy + vec2<f32>(17.3, 17.3), vec2<f32>(0.06711056, 0.00583715))) * 52.9829189);
            let sk = 1.55 * g - 0.55 * g * g * g;
            let r0 = uw.torch_col.w;
            for (var i = 0; i < tSteps; i++) {
                let s = (f32(i) + tJit) * tds;
                let v = uw.cam.xyz + dir * s - uw.torch_pos.xyz;
                let r2 = max(dot(v, v), 1e-4);
                let r = sqrt(r2);
                let Lr = v / r;
                let spot = uwTorchCone(dot(Lr, uw.torch_dir.xyz));
                let sd = 1.0 - dot(dir, -Lr) * sk;
                let ph = (1.0 - sk * sk) / (4.0 * UW_PI) * 0.75 / (sd * sd) + 0.25 / (4.0 * UW_PI);
                let fall = select(1.0, r0 * r0 / r2, r2 > r0 * r0);
                torch += exp(-sigT * (r + s)) * (spot * ph * fall);
            }
            torch = torch * uw.torch_col.rgb * sigS * tds * uw.torch_shape.y;
        }
        let T = exp(-sigT * dist);
        result = base * T + inSun + inAmb + shafts + torch;
    }

    // meniscus contact line and bright rim
    let line = smoothstep(2.0, 0.0, lineDist);
    let rim = smoothstep(bandW, bandW * 0.4, lineDist) * smoothstep(0.5, 2.5, lineDist);
    // (W9e, OP) the rim brightens what is there by up to 30% instead of adding Tidewater's
    // absolute sky + sun term: in OP's light units that term is several times the scene's own
    // brightness and drew the waterline as a hard double white stripe across the lens
    let withLine = result * (1.0 - line * 0.45 + rim * 0.3);
    return vec4<f32>(min(withLine, vec3<f32>(60000.0)), 1.0);
}
