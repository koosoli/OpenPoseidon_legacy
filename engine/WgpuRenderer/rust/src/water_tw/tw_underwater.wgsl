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
// * the water height is the sea level plus the FFT's vertical displacement (the shore waves are not
//   evaluated here);
// * the caustic light shafts and the diver's torch (Tidewater's half-resolution march) are not
//   ported yet (W5b), nor the seabed caustics;
// * OP lights submerged surfaces as if in air: points seen below the surface get Tidewater's
//   UnderwaterLighting attenuation here, in post (as the water material does from above, W3f);
// * light in Tidewater's units from OP's sun and ambient (see the water material), the in-scatter
//   scaled as the water's own glow seen from below (W3j.1: TW water glow, at least 0.12).

const UW_STRADDLE: f32 = 0.35;
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
    ocean: vec4<f32>,           // FFT amplitude, cascade count, unused x2
    sizes: vec4<f32>,           // FFT cascade sizes (m)
};

@group(0) @binding(0) var<uniform> uw: UwParams;
@group(0) @binding(1) var uwScene: texture_2d<f32>;
@group(0) @binding(2) var uwDepth: texture_depth_2d;
@group(0) @binding(3) var uwDisplacement: texture_2d_array<f32>;
@group(0) @binding(4) var uwRepeat: sampler;
@group(0) @binding(5) var uwClamp: sampler;

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

// water surface height at xz: sea level + the FFT's vertical displacement
fn uwWaterHeight(xz: vec2<f32>) -> f32 {
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

// medium at the near clip plane (1 water, 0 air); camH: camera height over the water
fn uwMedium(uv: vec2<f32>, camH: f32) -> f32 {
    var water = select(0.0, 1.0, camH < 0.0);
    if (abs(camH) < UW_STRADDLE) {
        let d = uwDepthAt(uv);
        if (d < 1e-7) {
            water = select(0.0, 1.0, uwDir(uv).y < 0.0);
        } else {
            let q = uwRelAt(uv, d) + uw.cam.xyz;
            water = select(0.0, 1.0, q.y < uwWaterHeight(q.xz));
        }
    }
    return water;
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
        let dist = uwDist(uv, d);
        let dir = uwDir(uv);

        // (OP) UnderwaterLighting in post: OP lit the submerged scene as if it stood in air;
        // attenuate its light by the water column above it (direct along the refracted sun,
        // ambient over ~1.2 x the depth), blended by the direct share of a flat bed's irradiance
        if (d > 1e-7) {
            let q = uwRelAt(uv, d) + uw.cam.xyz;
            let zq = uwWaterHeight(q.xz) - q.y;
            if (zq > 0.0) {
                let attDirect = exp(-sigT * zq / mu);
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
        let T = exp(-sigT * dist);
        result = base * T + inSun + inAmb;
    }

    // meniscus contact line and bright rim
    let line = smoothstep(2.0, 0.0, lineDist);
    let rim = smoothstep(bandW, bandW * 0.4, lineDist) * smoothstep(0.5, 2.5, lineDist);
    let withLine = result * (1.0 - line * 0.55) + rim * 0.15 * (uw.sky_e.rgb * 2.5 + uw.sun_e.rgb * 0.02);
    return vec4<f32>(min(withLine, vec3<f32>(60000.0)), 1.0);
}
