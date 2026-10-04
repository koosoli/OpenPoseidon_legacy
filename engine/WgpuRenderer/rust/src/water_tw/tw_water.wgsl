// Tidewater Native — the sea surface, ported from dgreenheck/tidewater @ 4811ba48 (MIT,
// © DRG Software Solutions LLC):
//   src/core/CDLOD.js        cdlodMorph / cdlodSnapped
//   src/ocean/WaterSurface.js waterSurfaceVertex / waterSurfaceFragment / cascade attenuation
//   src/ocean/SeaDetail.js   seaDetailSample
//   src/ocean/WaterMaterial.js the shade() output snippet, fresnelDielectric, GGX, _waterSSR
//
// W2 scope (plan §6): open sea. W3a adds the shoreline waves (ShoreWaves.js, tw_shore.wgsl), W3b
// the shore simulation (ShoreSim.js, tw_sim.wgsl): the material is now WaterMaterial._shadeWGSL's
// `T && SH && SIM` variant where a simulated region covers the pixel and `T && SH` elsewhere
// (the two blend by the region weight; Tidewater picks one at compile time for its one region).
// W3c adds SurfFoam (tw_surf_foam.wgsl), attached with the shore simulation as in Tidewater.
// Wake / hull mask / planar reflection / RefractionPass are still off (W4, W6). Refraction
// therefore samples the opaque scene copy (Tidewater's own `!found` fallback), which
// RefractionPass replaces in W4.
//
// Adaptations to OP, all mechanical and listed here once:
// * Group 0 is OP's frame (camera-relative view, forward-Z proj + reverse_z, reversed depth
//   buffer). World positions are carried camera-relative; `world = rel + frame.cam_pos`.
// * Tidewater's frame globals map to OP's: sunColor = PI * sun_diffuse (OP stores E/PI),
//   skyIrradiance = sun_ambient, horizonColor = fog colour, sunDir = -sun_dir_world,
//   windSpeed / windDir = the latched weather wind in WgrWaterParams.fft_wind_sea.
// * skyReflectionRadiance / skyRadianceWithClouds = OP's sky/cloud env bake (sky_env), which
//   is the panorama plan §5 names; OP's CSM + terrain + cloud sun shadow replace
//   sunShadowPCF / terrainSunShadowAt / cloudsShadow.
// * viewDepth / reversed-depth reconstruction go through OP's inv_view_proj.
// * The aerial perspective at the end is OP's (`apply_fog`), as for every OP surface.
// * The per-cascade shallow-water attenuation reads the live cascade sizes (Tidewater bakes the
//   defaults in); identical unless the Water tab's cascade scale is moved off 1.

#import frame::{frame, reverse_z, fog_factor, apply_fog, terrain_sun_shadow, cloud_sun_shadow}
#import shadow::shadow_strength
#import color::srgb_to_linear

const FFT_SIZE: f32 = 256.0;
const CASCADES: i32 = 4;

override linear: f32 = 1.0;

struct OceanParams {
    sizes: array<vec4<f32>, 4>,
    cuts: array<vec4<f32>, 4>,
    sys_a: array<vec4<f32>, 2>,
    sys_b: array<vec4<f32>, 2>,
    p0: vec4<f32>, // choppiness, foamBias, foamGain, foamDecay
    p1: vec4<f32>, // foamAdd, time, depth, dt
    seed: vec4<u32>,
};

@group(1) @binding(0) var<uniform> ocean: OceanParams;
@group(1) @binding(1) var<uniform> tw: TwSurface;
@group(1) @binding(2) var oceanDisplacement: texture_2d_array<f32>;
@group(1) @binding(3) var oceanDerivatives: texture_2d_array<f32>;
@group(1) @binding(4) var smpLinearRepeat: sampler;
@group(1) @binding(5) var smpAniso4Repeat: sampler;
@group(1) @binding(6) var waterFoamTex: texture_2d<f32>;
@group(1) @binding(7) var seaDetailNoise: texture_2d<f32>;
@group(1) @binding(8) var waterSceneColor: texture_2d<f32>;
@group(1) @binding(9) var waterSceneDepth: texture_depth_2d;
@group(1) @binding(10) var smpLinearClamp: sampler;
@group(1) @binding(11) var sky_env: texture_2d<f32>;
@group(1) @binding(12) var sky_env_samp: sampler;
@group(1) @binding(13) var seabed_heightmap: texture_2d<f32>;
@group(1) @binding(14) var<uniform> cf: ConformParams;
@group(1) @binding(15) var shoreFieldT: texture_2d<f32>;   // rg32float: arrival T, shoreline T
@group(1) @binding(16) var shoreFieldDir: texture_2d<f32>; // rgba8snorm: direction x exposure
@group(1) @binding(17) var shoreSimStateTex: texture_2d_array<f32>; // rgba16float, one layer per region
@group(1) @binding(18) var shoreSimLace: texture_2d<f32>;        // rgba8unorm lace (mip 0 read with loads)
@group(1) @binding(19) var<storage, read> breakersCrest: array<vec4<f32>>; // breaker crest records (tw_lip.wgsl)
@group(1) @binding(23) var causticsFineTex: texture_2d<f32>;  // W5b caustics (caustics.rs)
@group(1) @binding(24) var causticsBroadTex: texture_2d<f32>;

// ------------------------------------------------------------------ CDLOD.js

struct CdlodVertex { worldXZ: vec2<f32>, spacing: f32, morphK: f32, lod: f32, size: f32 };

fn cdlodMorph(node: vec4<f32>, grid: vec2<f32>, viewPos: vec3<f32>, y0: f32) -> CdlodVertex {
    let lod = i32(node.w);
    let m = tw.morph[lod];
    let h = m.z;
    let p = node.xy + grid * node.z;
    let idx = floor(p / h + 1e-3);
    let snapped = idx * h;
    let dist = length(viewPos - vec3<f32>(snapped.x, y0, snapped.y));
    let morphK = clamp((dist - m.x) * m.y, 0.0, 1.0);
    let odd = fract(idx * 0.5) * 2.0;
    var o: CdlodVertex;
    o.worldXZ = snapped - odd * h * morphK;
    o.spacing = h * (morphK + 1.0);
    o.morphK = morphK;
    o.lod = node.w;
    o.size = node.z;
    return o;
}

// ------------------------------------------------------------------ WaterSurface.js

// long cascades vanish in shallow water, short ones persist until very shallow:
// d0 = min(40, size * 0.08), floorAmt = [0, 0.05, 0.25, 0.5]
const WATER_SHORE_DEEP: f32 = 26.0; // m: ShoreWaves' envelope smoothstep(26, 13, depth) is 0 beyond
fn waterSurfaceCascadeAttenuation(c: i32, depth: f32) -> f32 {
    var floorAmt = array<f32, 4>(0.0, 0.05, 0.25, 0.5);
    let d0 = min(40.0, ocean.sizes[c].x * 0.08);
    let a = smoothstep(0.0, d0, depth);
    return mix(floorAmt[c] * smoothstep(0.0, 0.6, depth), 1.0, a);
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) rel: vec3<f32>,     // camera-relative world position of the vertex
    @location(1) lagXZ: vec2<f32>,
    @location(2) waveH: f32,
    @location(3) seaDepth: f32,
    @location(4) foam: f32,
    @location(5) shoreN: vec3<f32>,
    @location(6) shoreFoam: f32,
    @location(7) surfMask: vec2<f32>, // clear plunging face, whitewater roller relief (m)
};

// W9a: the surface at Lagrangian point worldXZ (mesh spacing `spacing`, m): what the vertex
// shader draws, shared with the camera height probe (tw_probe.wgsl) so that the underwater
// composite's waterline is the drawn surface's own.
struct TwDisp {
    world: vec3<f32>,   // displaced point (absolute y)
    total: vec3<f32>,   // displacement (x, z horizontal; y the wave height)
    ground: f32,
    depth: f32,
    foam: f32,
    shoreN: vec3<f32>,
    shoreFoam: f32,
    surfMask: vec2<f32>,
};

fn twDisplace(worldXZ: vec2<f32>, spacing: f32) -> TwDisp {
    let seaLevel = tw.sea.x;
    let ground = terrainHeightAt(worldXZ);
    let depth = seaLevel - ground;

    var disp = vec3<f32>(0.0);
    var foam = 0.0;
    var foamWeights = array<f32, 4>(0.35, 0.45, 0.5, 0.25);
    for (var c = 0; c < CASCADES; c++) {
        let L = ocean.sizes[c].x;
        let texel = L / FFT_SIZE;
        // band-limit to the mesh spacing to avoid aliasing / swimming
        let level = max(log2(spacing / texel) + 0.7, 0.0);
        let att = waterSurfaceCascadeAttenuation(c, depth);
        let uv = worldXZ / L;
        let s = textureSampleLevel(oceanDisplacement, smpLinearRepeat, uv, c, level);
        disp += s.xyz * att;
        var fv = s.w;
        if (level < 1.5) { fv = textureSampleLevel(oceanDisplacement, smpLinearRepeat, uv, c, 1.5).w; }
        foam += fv * foamWeights[c] * att;
    }
    disp *= tw.surface.x;

    var extra = vec3<f32>(0.0);
    var shoreN = vec3<f32>(0.0, 1.0, 0.0);
    var shoreFoam = 0.0;
    var surfMask = vec2<f32>(0.0); // clear plunging face, whitewater roller relief (m)
    // Offshore of WATER_SHORE_DEEP the shore waves have faded out completely (their envelope is 0
    // from 26 m of depth, see ShoreWaves) and there is no swash: most of the sea skips them.
    // (shore waves off, or no shore field yet: Tidewater's material without ShoreWaves)
    let nearShore = depth < WATER_SHORE_DEEP && tw.shore1.w > 0.0;
    var swashLevel = -1e4;
    if (nearShore) {
        let sw = shoreEvaluate(worldXZ, depth, ground);
        extra += sw.disp;
        shoreN = clamp(sw.nShore, vec3<f32>(-1.0), vec3<f32>(1.0));
        // (the foam line on the swash front is added per pixel in the water shader: on this coarse
        // mesh it would end short of the front and follow the triangles)
        shoreFoam = sw.foam;
        surfMask = vec2<f32>(sw.face, sw.roller);
        swashLevel = sw.swashLevel;
    }

    // W6: the boats' wakes (WaterSurface.js: `extra += wakeDisplacement( worldXZ )`)
    extra += wakeDisplacement(worldXZ);
    // W8b: explosions on the water (ring wave, cavity)
    extra += impactDisplacement(worldXZ);

    var total = disp + extra;
    var y = seaLevel + total.y;
    if (nearShore) {
        // thin run-up sheet on the sand: take whichever surface is higher (smooth max)
        let k = 0.04;
        // no run-up sheet on steep rock (cliffs, sea stacks): waves break against it instead
        let nr = terrainNormalXZ(worldXZ);
        let gentle = smoothstep(0.45, 0.25, length(nr));
        let hmx = sat((swashLevel - y) / k * 0.5 + 0.5) * gentle;
        let smax = mix(y, swashLevel, hmx) + hmx * (1.0 - hmx) * k;
        y = smax;
        // Where the sheet is the surface it is the sheet that is seen, not the wave below it: the
        // sheet lies on the sand (the sand's slope, no horizontal wave motion, no plunging face or
        // roller).
        shoreN = normalize(mix(shoreN, vec3<f32>(nr.x, 1.0, nr.y), hmx));
        let still = 1.0 - hmx;
        total = vec3<f32>(total.x * still, total.y, total.z * still);
        surfMask *= still;
    }
    // hide the water sheet below dry land (beyond the swash zone)
    let below = select(ground - 0.06, min(ground - 2.0, seaLevel - 1.0), depth < -3.0);
    y = select(y, min(y, below), y < ground);

    var o: TwDisp;
    o.world = vec3<f32>(worldXZ.x + total.x, y, worldXZ.y + total.z);
    o.total = total;
    o.ground = ground;
    o.depth = depth;
    o.foam = foam;
    o.shoreN = shoreN;
    o.shoreFoam = shoreFoam;
    o.surfMask = surfMask;
    return o;
}

@vertex
fn vs_tidewater(@location(0) grid: vec2<f32>, @location(1) node: vec4<f32>) -> VsOut {
    let seaLevel = tw.sea.x;
    let lod = cdlodMorph(node, grid, frame.cam_pos.xyz, seaLevel);
    let worldXZ = lod.worldXZ;
    let d = twDisplace(worldXZ, lod.spacing);
    let world = d.world;
    let total = d.total;
    let depth = d.depth;
    let foam = d.foam;
    let shoreN = d.shoreN;
    let shoreFoam = d.shoreFoam;
    let surfMask = d.surfMask;
    let rel = world - frame.cam_pos.xyz;
    var out: VsOut;
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(rel, 1.0));
    out.rel = rel;
    out.lagXZ = worldXZ;
    out.waveH = total.y;
    out.seaDepth = depth;
    out.foam = foam;
    out.shoreN = shoreN;
    out.shoreFoam = shoreFoam;
    out.surfMask = surfMask;
    return out;
}

// ------------------------------------------------------------------ SeaDetail.js

struct SeaDetailSample { rough: f32, gust: f32, slick: f32, streak: f32 };

fn seaDetailLoad(uv: vec2<f32>) -> vec4<f32> {
    return textureSampleLevel(seaDetailNoise, smpLinearRepeat, uv, 0.0);
}

// (W9b, OP) footprint: the pixel's size on the water (m)
fn seaDetailSample(xz: vec2<f32>, footprint: f32) -> SeaDetailSample {
    let p = xz - tw.wind.zw;
    let g1 = seaDetailLoad(p / 620.0).x;
    let g2 = seaDetailLoad(p / 230.0 + vec2<f32>(tw.sea.y * 0.0009, 0.37)).y;
    let gustRaw = g1 * 0.62 + g2 * 0.38;
    let gust = sat((gustRaw - 0.5) * 2.4 * tw.detail.x + 0.5);

    let w = tw.wind.xy;
    let along = dot(xz, w);
    let across = dot(xz, vec2<f32>(-w.y, w.x)) + (g2 - 0.5) * 26.0;

    let sl = seaDetailLoad(vec2<f32>(along / 1100.0, across / 70.0)).z;
    let calmWind = smoothstep(13.0, 4.0, tw.sea.z);
    let slick = smoothstep(0.64, 0.76, sl) * (1.0 - gust * 0.8) * calmWind * tw.detail.y;

    let st = seaDetailLoad(vec2<f32>(along / 380.0, across / 11.0) + vec2<f32>(0.13, 0.71)).w;
    let breakUp = seaDetailLoad(vec2<f32>(along / 140.0, across / 40.0) + vec2<f32>(0.51, 0.29)).x;
    let freshWind = smoothstep(6.0, 12.0, tw.sea.z);
    // (W9b, OP) the windrows are ~11 m across, read from the noise's full-resolution level: from
    // high up (pixels of metres) they alias into long parallel lines. They fade out as the pixel
    // grows past a few metres (Tidewater never looks from that high).
    let streakFar = 1.0 - smoothstep(2.0, 8.0, footprint);
    let streak = smoothstep(0.68, 0.82, st) * smoothstep(0.4, 0.62, breakUp) * freshWind * tw.detail.z * streakFar;

    let rough = mix(0.5, 1.5, gust) * (1.0 - slick * 0.8) * (1.0 - streak / max(tw.detail.z, 1e-3) * 0.45);
    return SeaDetailSample(rough, gust, slick, streak);
}

// ------------------------------------------------------------------ waterSurfaceFragment

struct WaterSurfaceFrag {
    normal: vec3<f32>,
    foam: f32,
    coverage: f32,
    slopes: vec2<f32>,
    jacobian: f32,
    rough: f32,
    aeration: f32,
    gust: f32,
    slick: f32,
    // the surf-zone foam look (SurfFoam, with the shore simulation on)
    foamInfo: SurfFoamInfo,
};

// what SurfFoam needs from the material (OP: bundled; Tidewater passes it through SurfFoamArgs)
struct SurfFoamCtx {
    simState: vec4<f32>,
    P: vec3<f32>,
    dPdx: vec3<f32>,
    dPdy: vec3<f32>,
    L: vec3<f32>,
    on: bool,
};

fn rot2(v: vec2<f32>, a: f32) -> vec2<f32> {
    let c = cos(a); let s = sin(a);
    return vec2<f32>(v.x * c - v.y * s, v.x * s + v.y * c);
}

// extraFoam: foam carried by the water (ShoreSim, 0 until W3b); surfMask: clear face of a
// plunging wave, whitewater roller relief (from the vertex stage)
// simIn: how much a simulated region covers the pixel (Tidewater's SIM, which it decides at
// compile time for its one region; blended here)
// sf: the SurfFoam inputs; sf.on = the shore simulation runs (Tidewater attaches SurfFoam with it)
fn rotorWashSlopes(worldXZ: vec2<f32>, footprint: f32) -> vec2<f32> {
    let count = min(u32(tw.rotorControl.y), 8u);
    if (count == 0u || tw.rotorDomain.z <= 0.0) { return vec2<f32>(0.0); }
    let local = worldXZ - tw.rotorDomain.xy;
    let edge = min(min(local.x, local.y), min(tw.rotorDomain.z-local.x, tw.rotorDomain.z-local.y));
    if (edge <= 0.0) { return vec2<f32>(0.0); }
    let domainFade = smoothstep(0.0, 8.0, edge);
    var slopes = vec2<f32>(0.0);
    for (var i = 0u; i < count; i += 1u) {
        let rotor = tw.rotorSources[i];
        let delta = worldXZ - rotor.xy;
        let reach = rotor.z * 2.0;
        if (dot(delta,delta) >= reach*reach) { continue; }
        // Filtering/trigonometry run only inside the rotor's bounded reach.
        let fine = exp(-0.5 * pow(max(footprint, 0.0) * 9.0, 2.0));
        let broad = exp(-0.5 * pow(max(footprint, 0.0) * 5.7, 2.0));
        let distance = length(delta);
        let q = distance / rotor.z;
        let envelope = smoothstep(0.15, 0.35, q) * (1.0-smoothstep(1.25,2.0,q));
        let phase = dot(rotor.xy,vec2<f32>(0.013,0.017));
        let wave = sin(distance*9.0-tw.rotorControl.x*23.0+phase)*fine*0.65 +
                   sin(distance*5.7-tw.rotorControl.x*16.0+phase*1.73)*broad*0.35;
        slopes += delta/max(distance,0.001) * (wave*envelope*rotor.w*0.14*domainFade);
    }
    return slopes * min(1.0,0.32/max(length(slopes),0.001));
}

fn waterSurfaceFragment(lagXZ: vec2<f32>, footprint: f32, depth: f32, vertexFoam: f32, shoreN: vec3<f32>, shoreFoam: f32, extraFoam: f32, surfMask: vec2<f32>, simIn: f32, sf: SurfFoamCtx) -> WaterSurfaceFrag {
    var d = vec4<f32>(0.0);
    var foamSum = 0.0;
    // the clear concave face of a plunging wave overhangs the trough: the foam carried by the
    // (depth-averaged, world-space) shore simulation below it is not on the face
    let face = sat(surfMask.x);
    // (some of it stays: the lace of the previous wave is drawn up the face)
    let simFoam = extraFoam * (1.0 - face * 0.72);
    foamSum += simFoam;
    // bubbles mixed into the water (milky, turquoise, hides the bottom): surf and wake
    var aeration = 0.0;

    let det = seaDetailSample(lagXZ, footprint);
    let rough = det.rough;

    for (var c = 0; c < CASCADES; c++) {
        var att = waterSurfaceCascadeAttenuation(c, depth);
        if (c >= CASCADES - 2) { att *= rough; }
        else if (c == CASCADES - 3) { att *= mix(1.0, rough, 0.4); }
        d += textureSample(oceanDerivatives, smpAniso4Repeat, lagXZ / ocean.sizes[c].x, c) * att;
    }
    d *= tw.surface.x;
    var slopes = vec2<f32>(d.x / max(d.z + 1.0, 0.2), d.y / max(d.w + 1.0, 0.2));

    // Near-field capillary ripples: re-sample the finest cascade at ~1 m and ~2.3 m tiles,
    // rotated, wherever a pixel covers less than its texel (explicit LOD, runs in a branch).
    let Lf = ocean.sizes[CASCADES - 1].x;
    let k1 = 7.3; let k2 = 3.1;
    let texel1 = Lf / k1 / FFT_SIZE;
    let texel2 = Lf / k2 / FFT_SIZE;
    let near = smoothstep(0.04, 0.01, footprint) * rough;
    if (near > 0.002) {
        let c1 = textureSampleLevel(oceanDerivatives, smpLinearRepeat, rot2(lagXZ, 0.63) * (k1 / Lf), CASCADES - 1, max(log2(footprint / texel1), 0.0));
        let c2 = textureSampleLevel(oceanDerivatives, smpLinearRepeat, rot2(lagXZ, 2.14) * (k2 / Lf), CASCADES - 1, max(log2(footprint / texel2), 0.0));
        let g = rot2(c1.xy, -0.63) * 0.55 + rot2(c2.xy, -2.14) * 0.35;
        slopes += g * near;
    }
    let jac = (d.z + 1.0) * (d.w + 1.0);
    // W6: the boats' wakes (slopes, the foam they carry, the bubbles in the water)
    {
        let w = wakeFragment(lagXZ);
        slopes += w.slopes;
        foamSum += w.foam;
        aeration += w.aeration;
    }
    // W8b: explosions on the water (the ring's slopes, the foam and bubbles they leave)
    {
        let im = impactFragment(lagXZ);
        slopes += im.slopes;
        foamSum += im.foam;
        aeration += im.aeration;
    }

    // Rotor-only normals: do not add legacy boat impulses over TW's own wakes.
    slopes += rotorWashSlopes(lagXZ, footprint);

    // base normal: large shoreline waves (per-vertex, can overhang) perturbed by FFT detail
    var normal: vec3<f32>;
    var baseNormal = vec3<f32>(0.0, 1.0, 0.0);
    {
        // On a coarse mesh the shore normal can flip between the vertices of a folding crest: the
        // interpolated vector then cancels out (or is NaN). Keep it finite and facing up.
        let sn = clamp(shoreN, vec3<f32>(-1.0), vec3<f32>(1.0)) + vec3<f32>(0.0, 1e-3, 0.0);
        let Ns0 = sn / max(length(sn), 1e-4);
        let Ns = normalize(vec3<f32>(Ns0.x, max(Ns0.y, 0.12), Ns0.z));
        baseNormal = Ns;
        // the ripples and chop ride on the wave: the detail normal is rotated onto the tilted face
        // (reoriented normal mapping) instead of being flattened by it
        let nd = normalize(vec3<f32>(-slopes.x, 1.0, -slopes.y));
        let tq = Ns + vec3<f32>(0.0, 1.0, 0.0);
        let uq = vec3<f32>(slopes.x, 1.0, slopes.y) * nd.y;
        normal = normalize(tq * (dot(tq, uq) / tq.y) - uq);
        // (WaterSurface: shoreFoam * 0.55 with the shore simulation attached, 1.0 without)
        foamSum += shoreFoam * mix(1.0, 0.55, simIn);
        // the roller and the water behind the plunge point are full of bubbles, decaying behind the
        // bore with the foam it sheds; the clear face of a plunging wave is not
        aeration += sat(shoreFoam * 1.2 + simFoam * 0.7) * (1.0 - face) * smoothstep(-0.1, 0.3, depth);
    }

    // whitecaps: persistent (per vertex) + fresh where the surface is compressed right now;
    // more of them inside gusts, plus windrow lines in fresh wind
    let fresh = sat((ocean.p0.y - 0.15 - jac) * 2.0);
    var whitecaps = vertexFoam + fresh;
    whitecaps = whitecaps * mix(0.5, 1.5, det.gust) + det.streak * 0.5;
    let coverage = sat((foamSum + whitecaps) * tw.surface.z);

    let fuv = lagXZ * tw.surface.w;
    let p1 = textureSample(waterFoamTex, smpAniso4Repeat, fuv);
    let r2 = vec2<f32>(fuv.x * 0.8 - fuv.y * 0.6, fuv.x * 0.6 + fuv.y * 0.8);
    let p2 = textureSample(waterFoamTex, smpAniso4Repeat, r2 * 2.37 + vec2<f32>(0.31, 0.77));
    let pattern = p1.x * 0.62 + p2.x * 0.38;
    let thresh = 1.05 - coverage * 1.1;
    let soft = 0.06 + footprint * 0.1;
    let detail = smoothstep(thresh - soft, thresh + soft, pattern) * (p1.y * 0.25 + 0.8);
    let far = smoothstep(0.15, 1.2, footprint);
    var foam = mix(detail, coverage * 0.85, far);

    var o: WaterSurfaceFrag;
    o.foamInfo = SurfFoamInfo(foam, sat(coverage), 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    if (sf.on) {
        // foam look (surf zone whitewater / lace, see SurfFoam)
        var fa: SurfFoamArgs;
        fa.coverage = coverage; fa.foam = foam; fa.footprint = footprint; fa.depth = depth; fa.bubbles = p1.y;
        fa.lagXZ = lagXZ; fa.normal = normal; fa.baseNormal = baseNormal;
        fa.fresh = shoreFoam; fa.sim = simFoam; fa.simState = sf.simState; fa.roller = surfMask.y; fa.P = sf.P;
        fa.dPdx = sf.dPdx; fa.dPdy = sf.dPdy; fa.L = sf.L;
        o.foamInfo = surfFoamShading(fa);
        foam = o.foamInfo.foam;
    }
    o.normal = normal;
    o.foam = foam;
    o.coverage = coverage;
    o.slopes = slopes;
    o.jacobian = jac;
    o.rough = rough;
    o.aeration = sat(aeration);
    o.gust = det.gust;
    o.slick = det.slick;
    return o;
}

// ------------------------------------------------------------------ WaterMaterial.js helpers

fn fresnelDielectric(cosI: f32, eta: f32) -> f32 {
    let c = clamp(cosI, 0.0, 1.0);
    let g2 = eta * eta - 1.0 + c * c;
    let tir = g2 < 0.0;
    let g = sqrt(max(g2, 0.0));
    let a = (g - c) / (g + c);
    let b = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
    return select(0.5 * (a * a) * (b * b + 1.0), 1.0, tir);
}

fn waterPhaseHG(cosT: f32, g: f32) -> f32 {
    let g2 = g * g;
    return ((1.0 - g2) / (4.0 * PI)) / pow(max(1.0 + g2 - cosT * 2.0 * g, 1e-4), 1.5);
}

fn _waterDGGX(NdH: f32, a2: f32) -> f32 {
    let d = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / (d * d * PI);
}

fn _waterVSmithGGX(NdL: f32, NdV: f32, a2: f32) -> f32 {
    let gv = NdL * sqrt(NdV * NdV * (1.0 - a2) + a2);
    let gl = NdV * sqrt(NdL * NdL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// a refracted sample is usable when it lies this far behind the water surface (view depth, m)
const WATER_BEHIND: f32 = 0.05;

fn sky_env_sample(dir: vec3<f32>) -> vec3<f32> {
    let u = 0.5 + atan2(dir.z, dir.x) / (2.0 * PI);
    let v = acos(clamp(dir.y, -1.0, 1.0)) / PI;
    return textureSampleLevel(sky_env, sky_env_samp, vec2<f32>(u, v), 0.0).rgb;
}

// Camera-relative world position of the opaque scene at uv (y down), from OP's reversed depth.
fn scene_rel_at(uv: vec2<f32>, d: f32) -> vec3<f32> {
    let ndc = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    // (W9b, OP) Not through the forward depth 1 - d: kilometres away d is ~1e-5 and 1 - d keeps
    // only a few bits of it, so the reconstructed distance jumped in steps from one pixel row to
    // the next -- from high up, bands of wrong water depth and path length across the whole sea
    // (the horizontal cyan and white lines). The reversed depth is inverted exactly as reverse_z
    // (frame.wgsl) wrote it (d = K - p32 / w), and the point is placed along the pixel's ray.
    if (frame.proj[3].w == 0.0 && frame.proj[2].w != 0.0 && frame.proj[0].z == 0.0 && frame.proj[1].z == 0.0) {
        let hm = frame.inv_view_proj * vec4<f32>(ndc, 0.5, 1.0);
        let pm = hm.xyz / hm.w;
        let wm = (frame.proj * (frame.view * vec4<f32>(pm, 1.0))).w;
        let K = 1.0 - frame.proj[2].z / frame.proj[2].w;
        let w = frame.proj[3].z / min(K - d, -1e-12);
        return pm * (w / wm);
    }
    let h = frame.inv_view_proj * vec4<f32>(ndc, 1.0 - d, 1.0);
    return h.xyz / h.w;
}

fn _waterSceneDepthAt(uv: vec2<f32>) -> f32 {
    let size = vec2<f32>(textureDimensions(waterSceneDepth));
    let p = vec2<i32>(clamp(uv, vec2<f32>(0.0), vec2<f32>(0.9999)) * size);
    return textureLoad(waterSceneDepth, p, 0);
}

// W6i.7: Tidewater's material works in a view space with -z forward (three.js). OP's has +z
// forward (its projection's w is +z: proj column 2 = (0, 0, a, 1)), so every view-space depth
// test was inverted: the SSR march stopped at its first step (p.z > -0.1), the refraction never
// found its sample valid, and the soft fade of the spray sprites removed them all. The material's
// view space is OP's with z mirrored when OP's is +z forward (detected from the projection, so a
// -z projection would pass through unchanged); helpers convert both ways.
fn twViewSign() -> f32 {
    return select(1.0, -1.0, frame.proj[2].w > 0.0);
}
fn twView(v: vec4<f32>) -> vec3<f32> {
    let q = (frame.view * v).xyz;
    return vec3<f32>(q.x, q.y, q.z * twViewSign());
}

// View-space z (negative in front of the camera) of the opaque scene at uv. Sky -> very far.
fn _waterSceneZAt(uv: vec2<f32>) -> f32 {
    let d = _waterSceneDepthAt(uv);
    if (d <= 1e-7) { return -1e9; }
    return twView(vec4<f32>(scene_rel_at(uv, d), 1.0)).z;
}

fn viewToRel(v0: vec3<f32>) -> vec3<f32> {
    // (back to OP's view space first) view is a rotation (camera-relative rendering) plus
    // whatever translation it carries
    let v = vec3<f32>(v0.x, v0.y, v0.z * twViewSign());
    let r = mat3x3<f32>(frame.view[0].xyz, frame.view[1].xyz, frame.view[2].xyz);
    return transpose(r) * (v - frame.view[3].xyz);
}

fn _waterProject(p: vec3<f32>) -> vec2<f32> {
    let clip = frame.proj * vec4<f32>(p.x, p.y, p.z * twViewSign(), 1.0);
    let ndc = clip.xy / max(clip.w, 1e-4);
    return vec2<f32>(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5);
}

// Screen-space reflection: march the reflected ray through the opaque depth copy (view space,
// geometric steps, then a short bisection). Returns (color, weight). _waterSSR, verbatim.
fn _waterSSR(posV: vec3<f32>, Rv: vec3<f32>, y0: f32, ry: f32) -> vec4<f32> {
    var hit = false;
    let stepScale = max(-posV.z / 60.0, 1.0);
    var t = 0.15 * stepScale;
    var dt = 0.25 * stepScale;
    var prevT = 0.0;
    for (var i = 0; i < 11; i++) {
        prevT = t;
        if (prevT >= 260.0 || (ry <= 0.0 && y0 + ry * prevT < tw.sea.x - 0.2)) { break; }
        t += dt;
        dt *= 1.7;
        let p = posV + Rv * t;
        let uv = _waterProject(p);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > -0.1) { break; }
        let sz = _waterSceneZAt(uv);
        if (p.z < sz && sz - p.z < max(dt * 1.3, max(t * 0.08, 0.3))) {
            hit = true;
            break;
        }
    }

    var color = vec3<f32>(0.0);
    var weight = 0.0;
    if (hit) {
        var a = prevT; var b = t;
        for (var k = 0; k < 3; k++) {
            let m = (a + b) * 0.5;
            let p = posV + Rv * m;
            let behind = p.z < _waterSceneZAt(_waterProject(p));
            b = select(b, m, behind);
            a = select(m, a, behind);
        }
        let hitT = b;
        let hitV = posV + Rv * b;
        let uv = _waterProject(hitV);
        let gap = abs(_waterSceneZAt(uv) - hitV.z);
        let touch = smoothstep(max(b * 0.04, 0.4), max(b * 0.02, 0.2), gap);
        let hitY = viewToRel(hitV).y + frame.cam_pos.y;
        color = textureSampleLevel(waterSceneColor, smpLinearClamp, uv, 0.0).rgb;
        let edge = smoothstep(0.0, 0.06, uv.x) * smoothstep(1.0, 0.94, uv.x) * smoothstep(0.0, 0.06, uv.y) * smoothstep(1.0, 0.94, uv.y);
        let facing = smoothstep(0.5, 0.1, Rv.z);
        weight = edge * facing * touch * smoothstep(260.0, 120.0, hitT) * smoothstep(tw.sea.x - 0.15, tw.sea.x + 0.35, hitY);
    }
    return vec4<f32>(color, weight);
}

// ------------------------------------------------------------------ the material

@fragment
fn fs_tidewater(in: VsOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    let rel = in.rel;
    let pos = rel + frame.cam_pos.xyz;
    let screenUV = in.clip.xy / vec2<f32>(textureDimensions(waterSceneDepth));
    let posV = twView(vec4<f32>(rel, 1.0));
    let lagXZ = in.lagXZ;
    let vDepth = in.seaDepth;
    let vHeight = in.waveH;
    // derivatives in uniform control flow
    let footprint = max(length(fwidth(lagXZ)), 1e-4);
    let dwx = dpdx(rel);
    let dwy = dpdy(rel);
    let groundH = terrainHeightAt(pos.xz);
    var thickness = pos.y - groundH;

    let dist = length(rel);
    let V = -rel / max(dist, 1e-4);
    let L = normalize(-frame.sun_dir_world.xyz); // surface -> sun (as water.wgsl)
    let fogK = fog_factor(dist);

    // OP light in Tidewater's units: sunColor is irradiance (OP keeps E/PI), sky irradiance is
    // already E/PI in both.
    var sunDiffuse = frame.sun_diffuse.rgb;
    var skyIrr = frame.sun_ambient.rgb;
    var horizon = frame.fog_color.rgb;
    if (linear > 0.5) {
        if (frame.sun_diffuse.w <= 0.5) {
            sunDiffuse = srgb_to_linear(sunDiffuse);
            skyIrr = srgb_to_linear(skyIrr);
        }
        horizon = srgb_to_linear(horizon);
    }
    let csm = shadow_strength(rel, vec3<f32>(0.0, 1.0, 0.0), fogK, dwx, dwy);
    let ter = terrain_sun_shadow(pos.xz, pos.y) * fogK;
    var sunLight = sunDiffuse * PI * (1.0 - max(csm, ter)) * cloud_sun_shadow(pos.xz);

    // water film thickness at this pixel and the distance to the swash front (ShoreWaves.swashEdge):
    // the sheet ends exactly on its analytic leading edge, not on the mesh triangles
    var frontD = 1e3;
    var swTau = 0.0;
    var swRt = 0.0;
    if (vDepth < 1.0 && tw.shore1.w > 0.0) {
        let tRaw = thickness;
        let se = shoreSwashEdge(pos.xz, thickness);
        thickness = se.x; frontD = se.y; swTau = se.z; swRt = se.w;
        // The draining sheet has no rounded front: it thins out over decimetres and breaks up where
        // the sand drains faster (kept as a hard edge it read as a dark line ruled along the beach).
        let backwash = smoothstep(0.32, 0.46, swTau);
        if (backwash > 0.0 && swRt > 0.0 && frontD < 3.0) {
            frontD += (perlin2(pos.xz * 1.1) * 0.35 + perlin2(pos.xz * 3.7 + vec2<f32>(5.3, 1.9)) * 0.15) * backwash;
            thickness = min(tRaw, frontD * mix(0.08, 0.025, backwash));
        }
    }
    // (fwidth here, in uniform control flow, of the clipped film thickness Tidewater differentiates)
    let thicknessAA = max(fwidth(thickness) * 1.5, 0.004);
    // the foam line riding the swash front, per pixel: a dense bubbly bead right at the edge while
    // the sheet runs up, a thinning lace behind it; weaker in the backwash (it sinks into the sand)
    let uprush = smoothstep(0.46, 0.32, swTau);
    let bead = smoothstep(-0.01, 0.05, frontD) * smoothstep(0.6, 0.12, frontD);
    let trail = smoothstep(-0.01, 0.25, frontD) * smoothstep(2.2, 0.3, frontD);
    // patchy along the front (dense bunches and thin stretches), not an even white rope
    var edgePatch = 1.0;
    if (swRt > 0.0 && vDepth < 0.4) {
        edgePatch = smoothstep(-0.45, 0.55, perlin2(pos.xz * 0.42)) * 0.7 + smoothstep(-0.3, 0.6, perlin2(pos.xz * 1.7 + vec2<f32>(3.1, 7.7))) * 0.3;
    }
    let edgeFoam = (bead * mix(0.45, 1.1, uprush) * mix(0.35, 1.0, edgePatch) + trail * mix(0.12, 0.4, uprush) * edgePatch) * smoothstep(0.0, 1.0, swRt) * smoothstep(0.4, -0.2, vDepth);
    // the meniscus: the last decimetre of the advancing sheet bends down to the sand
    let lipW = (1.0 - smoothstep(0.0, 0.14, frontD)) * uprush;

    // the shore simulation here (0 outside the simulated regions), and how much of the pixel it covers
    let simPick = shoreSimPick(pos.xz);
    let simState = shoreSimStateFor(pos.xz, simPick);
    let simIn = simPick.y * select(0.0, 1.0, tw.simP1.w > 0.5);
    let simOn = tw.simP1.w > 0.5;
    let sfCtx = SurfFoamCtx(simState, pos, dwx, dwy, L, simOn);
    // (W10f, OP) with the shore simulation on, most of the swash-front foam is drawn as carried
    // lace (holes, strands) rather than as fresh whitewater: as whitewater it was an even, opaque
    // white rope with detached blobs along the beach
    let edgeLace = 0.75 * simIn;
    let surf = waterSurfaceFragment(lagXZ, footprint, vDepth, in.foam, in.shoreN, in.shoreFoam + edgeFoam * (1.0 - edgeLace), simState.x + edgeFoam * edgeLace, in.surfMask, simIn, sfCtx);
    let foam = surf.foam;
    // the gradients the SurfFoam lighting differentiates, in uniform control flow (see tw_surf_foam.wgsl)
    let foamGrad = surfFoamGrad(surf.foamInfo, dwx, dwy);

    // Which medium is the view ray in? (see WaterMaterial.js) The GPU WaterQuery camera slot is
    // not ported in W2: the camera's own medium against the flat sea level decides away from
    // the surface, the triangle winding near it.
    let camH = frame.cam_pos.y - tw.sea.w;
    let folded = surf.jacobian < 0.1 || normalize(in.shoreN).y < 0.35;
    // (W10g, OP) ...and only near the lens (where the split view is): out on the sea the choppy
    // FFT folds the coarse far mesh, and its back-facing triangles, read as the view from below,
    // drew pale wedges and tiny triangles over the distant waves whenever the eye was near sea level
    let nearSurface = abs(camH) < 1.5 && dist < 25.0;
    // (W9d, OP) OP's world is left-handed (x east, z north, y up) where Tidewater's is
    // right-handed: the grid's triangles, wound as in CDLOD.js, face DOWN here, so front_facing
    // is the view from below. Read as Tidewater's, it drew the surface as seen from below whenever
    // the eye was within 1.5 m of the flat sea above it (the flat blue sheet at the waterline), and
    // as seen from above when it was just under it.
    let seenFromAbove = !front;
    let viewFromBelow = select(camH < 0.0, !seenFromAbove, nearSurface && !folded);
    let Nup = surf.normal;
    let Nside = select(Nup, -Nup, viewFromBelow);
    let Nview = normalize(Nside + V * max(-dot(Nside, V) + 0.03, 0.0));

    // roughness from unresolved slope variance (Cox-Munk: mss = 0.003 + 0.00512 U)
    let mss = (0.003 + tw.sea.z * 0.00512) * tw.surface.y;
    let kpx = PI / footprint;
    let unresolved = sat(log2(110.0 / kpx) / 9.0);
    let roughVar = surf.rough * surf.rough;
    let alpha2 = tw.material1.x * tw.material1.x + mss * 2.0 * unresolved * roughVar + foam * 0.2 + surf.aeration * 0.03;
    let sigmaUnres = sqrt(mss * unresolved * roughVar);

    var outCol = vec3<f32>(0.0);
    var ssrW = 0.0;
    var dbgPath = 0.0;
    var dbgScene = vec3<f32>(0.0);
    // (W3h colour diagnostics: debug views 20-23, see the list at the end)
    var dbgRefl = vec3<f32>(0.0);
    var dbgTrans = vec3<f32>(0.0);
    var dbgIn = vec3<f32>(0.0);
    var dbgLight = vec3<f32>(0.0);
    var dbgUnder = vec3<f32>(0.0);

    if (!viewFromBelow) {
        // ================= ABOVE WATER =================
        // near the leading edge the surface bends down to meet the sand like a rounded bead
        // (meniscus), tilting the normal toward dry land. Tidewater's terrainNormalRock slope is
        // taken here from central differences of OP's heightmap.
        let edgeW = max((1.0 - smoothstep(0.0, 0.006, thickness)) * uprush, lipW);
        let e = 0.5;
        let gradH = vec2<f32>(terrainHeightAt(pos.xz + vec2<f32>(e, 0.0)) - terrainHeightAt(pos.xz - vec2<f32>(e, 0.0)),
                              terrainHeightAt(pos.xz + vec2<f32>(0.0, e)) - terrainHeightAt(pos.xz - vec2<f32>(0.0, e))) / (2.0 * e);
        let uphill = normalize(gradH + vec2<f32>(1e-5, 0.0));
        let N = normalize(Nview + vec3<f32>(uphill.x, 0.0, uphill.y) * (edgeW * edgeW * 0.7));
        let NdV = max(dot(N, V), 1e-4);
        let F = fresnelDielectric(NdV, IOR);

        // ---- reflection
        let Rraw = reflect(-V, N);
        let Rup = max(Rraw.y, 0.004) + sigmaUnres * 1.3 * (1.0 - max(Rraw.y, 0.0));
        let R = normalize(vec3<f32>(Rraw.x, Rup, Rraw.z));
        let horizonOcc = max(smoothstep(-0.12, 0.08, Rraw.y), smoothstep(0.25, 0.06, thickness));
        var skyRefl = vec3<f32>(0.0);
        if (horizonOcc > 0.0 || frontD < 0.1) { skyRefl = sky_env_sample(R); }
        var reflCol = mix(horizon * 0.35, skyRefl, horizonOcc);

        // (W9g, OP) no screen-space reflections from high up (fading out 400-1200 m over the sea):
        // there they only reflect the sea into itself, and far off they drew faint white dashes
        // along the horizon from 8 km up
        let ssrAlt = 1.0 - smoothstep(400.0, 1200.0, frame.cam_pos.y - tw.sea.x);
        if (Rraw.y < 0.45 && F > 0.05 && tw.material1.y > 0.5 && ssrAlt > 0.0) {
            let Rv = normalize(twView(vec4<f32>(Rraw, 0.0)));
            if (Rv.z < 0.5) {
                let r = _waterSSR(posV, Rv, pos.y, Rraw.y);
                reflCol = mix(reflCol, r.rgb, r.a * ssrAlt);
                ssrW = r.a * ssrAlt;
            }
        }
        reflCol *= tw.material0.w;

        // ---- sun specular (GGX)
        let H = normalize(L + V);
        let NdL = max(dot(N, L), 0.0);
        let NdH = max(dot(N, H), 0.0);
        let VdH = max(dot(V, H), 0.0);
        let Fs = fresnelDielectric(VdH, IOR);
        let spec = _waterDGGX(NdH, alpha2) * _waterVSmithGGX(NdL, NdV, alpha2) * Fs * NdL;
        let sunSpec = sunLight * min(spec, 400.0);

        // ---- refraction / water volume (Snell ray to the sea floor)
        let Tr = refract(-V, N, 1.0 / IOR);
        let Tv = normalize(vec3<f32>(Tr.x, min(Tr.y, -0.08), Tr.z));
        let tDown = max(-Tv.y, 0.04);
        let surfViewZ = posV.z;

        let L0 = max(pos.y - groundH, 0.0) / tDown;
        var Lt = L0;
        if (L0 < 100.0) {
            let L1 = max(pos.y - terrainHeightAt(pos.xz + Tv.xz * min(L0, 200.0)), 0.0) / tDown;
            Lt = max(pos.y - terrainHeightAt(pos.xz + Tv.xz * min(L1 * 0.5 + L0 * 0.5, 200.0)), 0.0) / tDown;
        }
        let Lter = clamp(Lt, 0.0, 400.0);
        // thin breaking crests: the refracted ray leaves through the back of the wave into the sky
        let crestT = shoreCrestPath(lagXZ, vDepth, Tv);
        let thruCrest = crestT < Lter;

        let pEndRel = rel + Tv * min(Lter, 80.0);
        let clipEnd = frame.proj * (frame.view * vec4<f32>(pEndRel, 1.0));
        let ndcEnd = clipEnd.xy / max(clipEnd.w, 1e-4);
        let uvR = vec2<f32>(ndcEnd.x * 0.5 + 0.5, ndcEnd.y * -0.5 + 0.5);
        let onScreen = all(uvR > vec2<f32>(0.0)) && all(uvR < vec2<f32>(1.0));

        // (W2: no RefractionPass) the opaque copy where the refracted sample lies behind the
        // water surface, else the unrefracted pixel
        let dO = _waterSceneDepthAt(uvR);
        let zO = _waterSceneZAt(uvR);
        let valid = onScreen && surfViewZ - zO > WATER_BEHIND;
        let uvF = select(screenUV, uvR, valid);
        let dR = select(_waterSceneDepthAt(screenUV), dO, valid);
        var sceneCol = textureSampleLevel(waterSceneColor, smpLinearClamp, uvF, 0.0).rgb;
        // the ray leaves a thin crest through its back: the sky behind the wave, not the seabed
        // (a branch: select() would evaluate the sky for every pixel)
        if (thruCrest) { sceneCol = sky_env_sample(normalize(vec3<f32>(Tv.x, max(abs(Tv.y), 0.03), Tv.z))); }

        // objects in front of the sea floor (rocks, piers, hulls) shorten the path
        var qDist = 1e4;
        if (dR > 1e-7) {
            qDist = length(scene_rel_at(uvF, dR) - rel);
        }
        // (W9f, OP) Beyond ~2 km the scene behind the water is not taken from the depth buffer:
        // there it is tens of metres of sea over kilometres of view, and the scene point it gives
        // came out in steps from one pixel row to the next (debug views 6 and 21 from 15 km up:
        // regular one-pixel rows of short path and bright sea bed -- the owner's cyan and white
        // lines). The heightmap's path (Lter) and sea bed stand there instead.
        let farT = smoothstep(1500.0, 2500.0, length(rel));
        var pathLen = clamp(mix(min(Lter, qDist), Lter, farT), 0.0, 400.0);
        pathLen = min(pathLen, crestT);
        dbgPath = pathLen;
        dbgScene = sceneCol;

        let aer = surf.aeration;
        // sand stirred up where the bores have just passed (the foam they left marks that water):
        // clouds of sediment, not a uniform tint (1.0 without the shore simulation)
        let sandK = mix(1.0, sat(simState.x * 2.5) * 1.8 + 0.45, simIn);
        // surf zone: sand and bubbles stirred up by the breakers (see ShoreWaves.surfMedium)
        let surfMed = shoreSurfMedium(pos.xz, vDepth);
        let sigA = tw.absorption.rgb + surfMed.absorb * sandK;
        // (bubble plumes are shallow and patchy: a moderate scatterer, milky turquoise rather than a glow)
        let sigS = tw.scattering.rgb + surfMed.scatter * sandK + aer * 1.6;
        let sigT = sigA + sigS;

        // (W3f) Light reaching what lies under the water. OP lights its sea bed (and anything else
        // below the surface) as if it stood in air; Tidewater's UnderwaterLighting attenuates that
        // light through the water column above it (hookDirectModulation: the sun along its refracted
        // path, exp(-sigT d / mu); hookAmbientModulation: diffuse light over ~1.2 d, plus a little
        // in-scattered blue). Without it every metre of sea shows sunlit sand through a thin blue
        // filter, and deep water reads pale cyan instead of Tidewater's navy. The opaque copy holds
        // direct + ambient light already summed, so the two attenuations are blended by the direct
        // share of a flat bed's irradiance. Caustics are not ported here (W5).
        if ((dR > 1e-7 || farT > 0.5) && !thruCrest) {
            let bedPos = select(scene_rel_at(uvF, dR) + frame.cam_pos.xyz, pos + Tv * min(Lter, 400.0), farT > 0.5);
            let dBed = max(tw.sea.x - bedPos.y, 0.0);
            if (dBed > 0.0) {
                let sigTc = tw.absorption.rgb + tw.scattering.rgb;
                let LsB = refract(-L, vec3<f32>(0.0, 1.0, 0.0), 1.0 / IOR);
                let muB = max(-LsB.y, 0.15);
                // (W5b) the direct light arrives focused into Tidewater's caustic networks
                let causLvl = twCausticsLevel(footprint * 1.5, tw.brk.z);
                let caus = twCausticsSample(causticsFineTex, causticsBroadTex, smpLinearRepeat, tw.brk.zw, bedPos, dBed, L, causLvl, false);
                let attDirect = exp(-sigTc * dBed / muB) * caus;
                let attAmbient = exp(-sigTc * dBed * 1.2) * 0.85 + vec3<f32>(0.0, 0.02, 0.04) * exp(dBed * -0.1);
                let lumW = vec3<f32>(0.2126, 0.7152, 0.0722);
                let eSun = dot(sunLight, lumW) * max(L.y, 0.0) * INV_PI;
                let eSky = dot(skyIrr, lumW);
                let directShare = eSun / max(eSun + eSky, 1e-5);
                let bedAtt = attDirect * directShare + attAmbient * (1.0 - directShare);
                sceneCol *= mix(vec3<f32>(1.0), bedAtt, smoothstep(0.0, 0.08, dBed));
            }
        }

        let Ls = -refract(-L, vec3<f32>(0.0, 1.0, 0.0), 1.0 / IOR);
        let muS = max(Ls.y, 0.1);
        let muV = max(-Tv.y, 0.15);

        let Tview = exp(-sigT * pathLen);

        let sunIn = sunLight * (1.0 - fresnelDielectric(max(L.y, 0.02), IOR));
        let kSun = sigT * (1.0 + muV / muS);
        let kAmb = sigT * (1.0 + muV / 0.75);
        let cosPh = dot(Tv, Ls);
        let phase = waterPhaseHG(cosPh, 0.86) * 0.7 + 0.3 / (4.0 * PI);
        let bb = sigS * mix(tw.material0.x, 0.06, sat(aer * 2.0));
        let albedoMS = bb * (0.33 * 4.0) / (sigA + bb);
        let inSun = sunIn * (sigS * phase + albedoMS * sigT * INV_PI) * (1.0 - exp(-kSun * pathLen)) / kSun;
        let inAmb = skyIrr * (sigS * 0.25 + albedoMS * sigT) * (1.0 - exp(-kAmb * pathLen)) / kAmb;

        // crest translucency (sun shining through thin wave tips)
        let vH = normalize(vec2<f32>(V.x, V.z) + 1e-5);
        let lH = normalize(vec2<f32>(L.x, L.z) + 1e-5);
        let back = pow(sat(dot(vH, -lH) * 0.6 + 0.4), 2.5);
        let crest = sat(vHeight * 0.9 + 0.1) * (sat((1.0 - N.y) * 4.0) + 0.25);
        let sssCol = vec3<f32>(0.12, 0.55, 0.45) * 0.06;
        let sss = sunLight * sssCol * back * crest * tw.material0.y * smoothstep(0.0, 0.25, L.y);

        // (W3j) the water's own glow (light scattered inside it toward the eye) x the Water tab's
        // "TW water glow" (tw.material1.w). Tidewater's formula; OP's lighting makes it several
        // times brighter relative to the reflection than in Tidewater, which turned deep water
        // pale cyan (debug view 22). The scale brings it back between Tidewater's reflection-led
        // navy and Current OP's steel blue.
        let glowK = tw.material1.w;
        let transmitted = sceneCol * Tview * (1.0 - 0.3 * lipW) + (inSun + inAmb + sss) * glowK;

        // ---- foam: bright diffuse scatterer (albedo ~0.85), wrapped sun + sky irradiance
        var foamLit = (sunLight * (max(dot(N, L), 0.0) * 0.75 + 0.25) * INV_PI + skyIrr * 0.95) * 0.85;
        if (simOn) { foamLit = surfFoamLight(surf.foamInfo, foamGrad, N, L, V, sunLight, skyIrr); }
        let foamCol = foamLit * tw.material0.z;

        let rim = smoothstep(0.0, 0.025, frontD) * smoothstep(0.1, 0.035, frontD) * uprush;
        let water = mix(transmitted, reflCol, F) + sunSpec + skyRefl * (0.22 * rim);
        dbgRefl = reflCol * F + sunSpec;
        dbgTrans = sceneCol * Tview * (1.0 - F);
        dbgIn = (inSun + inAmb + sss) * glowK * (1.0 - F);
        dbgLight = vec3<f32>(dot(sunLight, vec3<f32>(0.2126, 0.7152, 0.0722)), dot(skyIrr, vec3<f32>(0.2126, 0.7152, 0.0722)), F);
        let shaded = mix(water, foamCol + sunSpec * 0.05, sat(foam));
        let edgeAA = smoothstep(0.0, thicknessAA, thickness);
        outCol = shaded;
        if (edgeAA < 1.0) {
            // contact shadow: the sand just ahead of the advancing edge is darkened
            let contact = smoothstep(-0.16, -0.005, frontD) * (1.0 - edgeAA) * uprush;
            let sandC = textureSampleLevel(waterSceneColor, smpLinearClamp, screenUV, 0.0).rgb * (1.0 - 0.3 * contact);
            outCol = mix(sandC, shaded, edgeAA);
        }
        outCol = apply_fog(outCol, rel);
    } else {
        // ================= BELOW WATER (looking up at the surface) =================
        let N = Nview;
        let NdV = max(dot(N, V), 1e-4);
        let F = fresnelDielectric(NdV, 1.0 / IOR);
        let Tt = refract(-V, N, IOR);
        let tValid = dot(Tt, Tt) > 0.5;
        let Td = normalize(select(vec3<f32>(0.0, 1.0, 0.0), Tt, tValid));
        let skyT = min(sky_env_sample(Td), vec3<f32>(60.0));

        let sigA = tw.absorption.rgb; let sigS = tw.scattering.rgb; let sigT = sigA + sigS;
        let bb = sigS * tw.material0.x;
        let albedoMS = bb * (0.33 * 4.0) / (sigA + bb);
        let Rr = reflect(-V, N);
        let LsU = -refract(-L, vec3<f32>(0.0, 1.0, 0.0), 1.0 / IOR);
        let muU = max(LsU.y, 0.15);
        let phR = waterPhaseHG(dot(Rr, LsU), 0.86) * 0.7 + 0.3 / (4.0 * PI);
        let kS = sigT * (1.0 - min(Rr.y, 0.0) / muU);
        let kA = sigT * (1.0 - min(Rr.y, 0.0) / 0.8);
        let eSunU = sunLight * (1.0 - fresnelDielectric(max(L.y, 0.02), IOR));
        let deepCol = (eSunU * (sigS * phR + albedoMS * sigT * INV_PI) / kS
            + skyIrr * PI * (sigS * (1.0 / (4.0 * PI)) + albedoMS * sigT * INV_PI) / kA) * max(tw.material1.w, 0.12);

        let sceneZ = _waterSceneZAt(screenUV);
        let hasObj = posV.z - sceneZ > 0.0 && sceneZ > -1e8;
        let objCol = textureSampleLevel(waterSceneColor, smpLinearClamp, screenUV, 0.0).rgb;
        let transmittedU = select(skyT, objCol, hasObj);
        dbgUnder = vec3<f32>(select(1.0, 0.0, hasObj), select(0.0, 1.0, hasObj), F);

        let foamUnder = (skyIrr + sunLight * 0.5) * 0.25;
        outCol = mix(transmittedU * (1.0 - F) + deepCol * F, foamUnder, sat(foam) * 0.7);
    }

    // debug views (Tidewater numbering): 1 back faces red, 2 normals, 3 foam, 5 lattice,
    // 6 path length, 7 seabed seen through, 8 sea depth, 10 SSR weight; OP additions (W3h): the
    // above-water composite split into 20 reflection (+ sun glint), 21 sea bed transmitted,
    // 22 in-scattered light, and 23 the light inputs (r: sun irradiance luminance / 10,
    // g: sky irradiance luminance, b: Fresnel), all before fog; W3m: 24 foam sources (r shore
    // waves, g simulation, b region coverage), 25 (r depth / 26 m, g whitecaps, b foam coverage)
    let dbg = i32(tw.material1.z + 0.5);
    var res = min(outCol, vec3<f32>(16000.0));
    if (dbg == 1) {
        res = select(vec3<f32>(50.0, 0.0, 0.0), res, front);
    } else if (dbg == 2) {
        res = Nview * 0.5 + 0.5;
    } else if (dbg == 3) {
        res = vec3<f32>(foam);
    } else if (dbg == 5) {
        res = vec3<f32>(fract(lagXZ.x * 0.1), fract(vHeight), fract(lagXZ.y * 0.1));
    } else if (dbg == 6) {
        res = vec3<f32>(dbgPath * 0.02, 0.0, 0.0);
    } else if (dbg == 7) {
        res = dbgScene;
    } else if (dbg == 8) {
        res = vec3<f32>(0.0, vDepth * 0.02, 0.0);
    } else if (dbg == 10) {
        res = vec3<f32>(ssrW);
    } else if (dbg == 20) {
        res = dbgRefl;
    } else if (dbg == 21) {
        res = dbgTrans;
    } else if (dbg == 22) {
        res = dbgIn;
    } else if (dbg == 24) {
        // (W3m) the foam sources: r shore-wave foam (per vertex), g simulated foam, b region coverage
        res = vec3<f32>(sat(in.shoreFoam), sat(simState.x), simIn);
    } else if (dbg == 25) {
        // (W3m) r depth / 26 m (the shore waves' reach), g whitecaps (per vertex), b foam coverage
        res = vec3<f32>(sat(vDepth / 26.0), sat(in.foam), sat(surf.coverage));
    } else if (dbg == 26) {
        // (Sinkhole) the surface seen from below: r nothing found above it (the sky is shown), g an object
        // above it (the scene copy is shown), b the Fresnel (mirror) share
        res = dbgUnder * 2.0;
    } else if (dbg == 23) {
        res = dbgLight * vec3<f32>(0.1, 1.0, 1.0);
    }
    return vec4<f32>(res, 1.0);
}
