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

#import frame::{frame, reverse_z, fog_factor, apply_fog, terrain_sun_shadow, cloud_sun_shadow, sky_irradiance}
#import shadow::shadow_strength
#import color::srgb_to_linear
#ifdef SEA_RAIN
#import rain_water_optics::rain_water_impact_layer
#endif

const FFT_SIZE: f32 = 256.0;
const CASCADES: i32 = 4;
// SEA-GLINTS: sparkles ADDED to the whole smooth highlight (owner, 2026-10-26: "i can no really see
// much glitter"). The energy-conserving version took GLINT_SHARE of the lobe from every pixel and gave
// it back to the few a facet lit, which at any share read as the path dimmed and etched with dark
// rims, not as glitter. Now: the finest facet cell (m), a facet's glint tolerance in units of the
// capillary slope spread, its radius in cells, a glint's peak over the smooth path's own peak (above 1
// the tone curve clips it white, as the eye sees the sun in a facet), and the capillary slope spread
// floor: facets are tilted by ripples finer than the resolved waves, so the sparkles spread past the
// smooth path into a halo round it.
const GLINT_CELL: f32 = 0.05;
const GLINT_TOL: f32 = 0.4;
const GLINT_RADIUS: f32 = 0.35;
const GLINT_PEAK: f32 = 8.0;
const GLINT_CAPILLARY: f32 = 0.09;
// integer hashes (Jarzynski & Olano 2020, pcg3d / pcg4d): a handful of integer ops where the
// sin-based hash cost a sin each, and a facet needs eleven random numbers
fn glintPcg3(v0: vec3<u32>) -> vec3<f32> {
    var v = v0 * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> vec3<u32>(16u);
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return vec3<f32>(v >> vec3<u32>(8u)) * (1.0 / 16777216.0);
}
fn glintPcg4(v0: vec4<u32>) -> vec4<f32> {
    var v = v0 * 1664525u + 1013904223u;
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    v ^= v >> vec4<u32>(16u);
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    return vec4<f32>(v >> vec4<u32>(8u)) * (1.0 / 16777216.0);
}
const GLINT_RATE: f32 = 3.0;
// the glint of the facet in this pixel's cell of size `cell` (octave `lvl`), 0..1: `need` is the
// tilt that mirrors the sun, `sig` the facets' slope spread, `fp` the footprint (m)
fn glintFacet(xz: vec2<f32>, cell: f32, lvl: u32, need: vec2<f32>, sig: f32, fp: f32) -> f32 {
    let cf = floor(xz / cell);
    let cu = bitcast<vec2<u32>>(vec2<i32>(cf));
    let h = glintPcg3(vec3<u32>(cu, lvl * 0x9E37u + 17u));
    let centre = (cf + 0.4 + 0.2 * h.xy) * cell;
    let r = GLINT_RADIUS * cell;
    // the share of the pixel the facet disc covers: a soft disc whose edge widens with the footprint
    let d = length(xz - centre);
    let cover = clamp((r + 0.5 * fp - d) / max(fp, 1e-4), 0.0, 1.0) * min(r * r / max(0.25 * fp * fp, 1e-8), 1.0);
    // the facet's tilt, re-drawn GLINT_RATE times a second and blended between draws so it drifts:
    // each draw two sums of two uniforms (triangular, scaled to unit variance)
    let tt = frame.ground_weather.w * GLINT_RATE + h.z;
    let ti = u32(max(floor(tt), 0.0));
    let tf = smoothstep(0.0, 1.0, fract(tt));
    let q0 = glintPcg4(vec4<u32>(cu, ti, lvl));
    let q1 = glintPcg4(vec4<u32>(cu, ti + 1u, lvl));
    let d0 = vec2<f32>(q0.x + q0.y - 1.0, q0.z + q0.w - 1.0) * 2.449;
    let d1 = vec2<f32>(q1.x + q1.y - 1.0, q1.z + q1.w - 1.0) * 2.449;
    // two draws blended lose variance mid-way; restore it
    let tilt = mix(d0, d1, tf) / sqrt((1.0 - tf) * (1.0 - tf) + tf * tf);
    let tau = GLINT_TOL * sig;
    let off = tilt * sig - need;
    // how well the facet mirrors the sun into the eye, times the share of the pixel it covers
    return exp(-dot(off, off) / (2.0 * tau * tau)) * cover;
}

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
// (OP) Whole World: the sea floor beyond the heightmap's window (TwWater::set_far_floor): world heights on a coarse
// grid, placed by (origin x, origin z, cell, samples a side; 0 = none).
@group(1) @binding(28) var farFloorTex: texture_2d<f32>;
@group(1) @binding(29) var<uniform> farFloor: vec4<f32>;
#ifdef TW_FOAM2
// TW-FOAM2: the FFT foam buffer, N x N x cascades persistent foam then the same for active breaking
@group(1) @binding(30) var<storage, read> oceanFoamBuf: array<f32>;
// active breaking of cascade c at a world point, bilinear and periodic like the FFT textures
fn activeFoamAt(xz: vec2<f32>, c: i32) -> f32 {
    let n = i32(FFT_SIZE);
    let st = xz / ocean.sizes[c].x * FFT_SIZE - 0.5;
    let i0 = vec2<i32>(floor(st));
    let f = st - floor(st);
    let base = (n * n) * (CASCADES + c);
    var v = array<f32, 4>();
    for (var k = 0; k < 4; k++) {
        let q = ((i0 + vec2<i32>(k & 1, k >> 1)) % n + n) % n;
        v[k] = oceanFoamBuf[base + q.y * n + q.x];
    }
    return mix(mix(v[0], v[1], f.x), mix(v[2], v[3], f.x), f.y);
}
#endif

// Earth presentation belongs to the material: the wake kernel also consumes
// tw_common.wgsl, but deliberately has no TwSurface binding.
fn earthWater() -> bool { return tw.detail.w > 0.5; }
// The Earth sea is drawn to EARTH_SEA_CEILING (camera metres; WaterWgpu::DrawEarthWater's EarthSeaCeiling), fading
// into the imagery from EARTH_SEA_FADE_START.
const EARTH_SEA_FADE_START: f32 = 30000.0;
const EARTH_SEA_CEILING: f32 = 40000.0;
fn earthWaterDrop(xz: vec2<f32>) -> f32 {
    if (!earthWater()) { return 0.0; }
    let d = xz - vec2<f32>(6400.0);
    let r2 = dot(d, d);
    let radius = 6378137.0;
    return r2 / (radius + sqrt(max(radius * radius - r2, 0.0)));
}
fn waterLevelAt(xz: vec2<f32>) -> f32 { return tw.sea.x - earthWaterDrop(xz); }
fn earthWaterNormal(xz: vec2<f32>) -> vec3<f32> {
    if (!earthWater()) { return vec3<f32>(0.0, 1.0, 0.0); }
    let p = xz - vec2<f32>(6400.0);
    return normalize(vec3<f32>(p.x, 6378137.0 - earthWaterDrop(xz), p.y));
}
fn earthMaterialCorner(ix: i32, iz: i32) -> f32 {
    let corner = clamp(vec2<i32>(ix, iz), vec2<i32>(0), vec2<i32>(i32(cf.hm_width) - 1, i32(cf.hm_height) - 1));
    // The ground as drawn: since 2026-10-24 the streamed heights carry the real sea floor (EarthTerrainStream::
    // WithSeaDepth) and keep low land dry, so the old -5..0.5 m 'open sea, 200 m down' stand-in is gone here too.
    return hm_load(corner.x, corner.y);
}
// The far floor at xz: (height, 1), or (0, 0) outside it or without one.
fn farFloorAt(xz: vec2<f32>) -> vec2<f32> {
    let n = farFloor.w;
    if (n < 2.0) { return vec2<f32>(0.0); }
    let t = (xz - farFloor.xy) / max(farFloor.z, 1e-3);
    if (any(t < vec2<f32>(0.0)) || any(t > vec2<f32>(n - 1.0))) { return vec2<f32>(0.0); }
    let base = min(floor(t), vec2<f32>(n - 2.0));
    let f = t - base;
    let i = vec2<i32>(base);
    let a = textureLoad(farFloorTex, i, 0).x;
    let b = textureLoad(farFloorTex, i + vec2<i32>(1, 0), 0).x;
    let c = textureLoad(farFloorTex, i + vec2<i32>(0, 1), 0).x;
    let d = textureLoad(farFloorTex, i + vec2<i32>(1, 1), 0).x;
    return vec2<f32>(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), 1.0);
}
fn materialTerrainHeightAt(xz: vec2<f32>) -> f32 {
    if (!earthWater() || cf.enabled <= 0.5 || cf.hm_width < 2u || cf.hm_height < 2u) {
        return terrainHeightAt(xz);
    }
    // Lower corners before interpolation, exactly as the opaque near mesh.
    // Classifying an interpolated height instead leaves a depth discontinuity
    // across mixed land/ocean triangles and makes offshore pixels shallow surf.
    let t = (xz - cf.origin) / max(cf.terrain_grid, 1e-4);
    let base = floor(t);
    let f = t - base;
    let ix = i32(base.x); let iz = i32(base.y);
    let a = earthMaterialCorner(ix, iz); let b = earthMaterialCorner(ix + 1, iz);
    let c = earthMaterialCorner(ix, iz + 1); let d = earthMaterialCorner(ix + 1, iz + 1);
    let lower = a + (c - a) * f.y + (b - a) * f.x;
    let upper = c + (b - d) - (c - d) * f.x - (b - d) * f.y;
    let h = select(upper, lower, f.x + f.y <= 1.0);
    // Preserve the existing finite off-map transition, with Earth's curved
    // datum. Clamped dry edge texels must not run out to the water horizon.
    let size = vec2<f32>(f32(cf.hm_width - 1u), f32(cf.hm_height - 1u)) * cf.terrain_grid;
    let r = xz - cf.origin;
    // Beyond the window: the far floor (Whole World's real sea depth), handed over inside the window's last 400 m
    // so the seam has no crease; without one the old 200 m stand-in outside it -- that stand-in drew the window as
    // a bright rectangle on a shallow sea (Helgoland, 2026-10-25).
    let far = farFloorAt(xz);
    if (far.y > 0.5) {
        let inside = min(min(r.x, r.y), min(size.x - r.x, size.y - r.y));
        return mix(far.x, h, smoothstep(0.0, 400.0, inside));
    }
    let outside = length(max(max(-r, r - size), vec2<f32>(0.0)));
    return mix(h, min(h, waterLevelAt(xz) - 200.0), smoothstep(0.0, 400.0, outside));
}

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
    let seaLevel = waterLevelAt(worldXZ);
    let ground = materialTerrainHeightAt(worldXZ);
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
    var shoreN = earthWaterNormal(worldXZ);
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

    // (OP, 2026-10-13) the windrows no longer touch the surface at all: as foam they were white worms,
    // left in the roughness they drew thin dark creases across the whole sea once the wind passed
    // ~10 m/s (owner: "remove these darker lines ... they look like shit"; gone with windrow 0 at his
    // pose). `streak` is still reported for the debug views.
    let rough = mix(0.5, 1.5, gust) * (1.0 - slick * 0.8);
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
    // (OP) blood in the water, 0..1 (tw_impact.wgsl bloodAmount)
    blood: f32,
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

// The area mean of the lace (`detail` in waterSurfaceFragment) at a coverage, 0..1 in steps of
// 0.05, measured over the full-resolution foam pattern: what a pixel covering metres of lace shows.
// The lace thresholds a pattern centred on 0.49, so it stays nearly clear up to a coverage of 0.3
// and its brightest is 0.82. Guarded against the pattern by fft_cover_tests.rs
// (`lace_expected_table_matches_the_pattern`).
fn laceExpected(c: f32) -> f32 {
    var g = array<f32, 21>(0.0, 0.0, 0.001, 0.005, 0.018, 0.046, 0.09, 0.154, 0.234, 0.325, 0.417, 0.506, 0.586, 0.655, 0.711, 0.755, 0.785, 0.804, 0.815, 0.821, 0.823);
    let x = sat(c) * 20.0;
    let i = min(u32(x), 19u);
    return mix(g[i], g[i + 1u], x - f32(i));
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
#ifdef SEA_RAIN
    // (OP, SEA-RAIN) heavy rain flattens the capillary wind ripples and the drop rings take their place
    // (below): they fade out with the rain, and in full rain this branch and its two taps are skipped,
    // which pays for the rings
    let near = smoothstep(0.04, 0.01, footprint) * rough * (1.0 - sat(frame.ground_weather.y));
#else
    let near = smoothstep(0.04, 0.01, footprint) * rough;
#endif
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
#ifdef SEA_RAIN
    // (OP, SEA-RAIN) rain drop rings: the puddles' world-fixed impact events (rain_water_optics), one layer
    // (the puddles add a second in heavy rain). Drawn only where the capillary ripples they replace were
    // drawn (footprint < 0.04 m) so they cost what those two taps cost; beyond that a ring is already
    // fading below a pixel and fs_tidewater carries it as roughness. Dry weather skips the call.
    let seaRain = frame.ground_weather.y;
    if (seaRain > 0.0 && footprint < 0.04) {
        slopes += rain_water_impact_layer(lagXZ, frame.ground_weather.w, seaRain, footprint, 0u)
            * smoothstep(0.04, 0.025, footprint);
    }
#endif

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
    // OP: the per-pixel term only sharpens a cap the persistent (per-vertex) foam already draws.
    // On its own it is a thin line along every compressed crest ridge -- at a rough sea's
    // choppiness, white worms all over the water (owner, 2026-10-12); the debug view (25) showed
    // the vertex foam as soft patches and every worm in the per-pixel coverage.
    // whitecaps: persistent (per vertex) + fresh where the surface is compressed right now; more of
    // them inside gusts -- island-expansion's WaterSurface.js, synced on the owner's request
    // (2026-10-13). OP keeps one departure the owner asked for: no windrow lines (Tidewater adds
    // det.streak * 0.5 here, drawn as thin white lines across the sea in fresh wind).
    let fresh = sat((ocean.p0.y - 0.15 - jac) * 2.0);
#ifdef TW_FOAM2
    // (OP, TW-FOAM2) the cap is where the crest breaks NOW: the active field (~0.4 s) of the two
    // whitecap cascades (x3: it settles at about a third of the persistent foam's level); the
    // persistent foam, seconds old, goes to the lace below instead of keeping the cap solid.
    // Close to the eye only: beyond 0.8 m pixels (a cap is a few pixels there) the plain per-vertex
    // foam carries the caps -- the 8 buffer reads are paid only where a cap's core and rim resolve.
    let activeNear = 1.0 - smoothstep(0.3, 0.8, footprint);
    var activeFoam = 0.0;
    if (activeNear > 0.0) {
        activeFoam = (activeFoamAt(lagXZ, 1) * 0.45 + activeFoamAt(lagXZ, 2) * 0.5) * 3.0;
    }
    var whitecaps = mix(vertexFoam, activeFoam, activeNear) + fresh;
#else
    var whitecaps = vertexFoam + fresh;
#endif
    whitecaps = whitecaps * mix(0.5, 1.5, det.gust);
    var capCover = whitecaps * tw.surface.z;
    // OP: whitecaps as patches on the crests (see water_tw::WHITECAP_PATCH): the low foam a rough
    // sea leaves on every wave goes to nothing instead of to the lace pattern's thinnest veins,
    // and a crest that is breaking reads as a solid cap that frays at its edge.
    let capPatches = tw.absorption.w > 0.0;
    if (capPatches) {
        // open water only: in the surf and on the swash sheet the shore foam draws the white water,
        // and the (attenuated) FFT foam there, which this threshold sits far below, turned the uprush
        // into solid white blobs running up the sand
        capCover = smoothstep(tw.absorption.w, tw.scattering.w, capCover) * smoothstep(1.0, 4.0, depth);
    }
    let coverage = sat(foamSum * tw.surface.z + capCover);
    // the cover the lace pattern below draws: everything but the caps in patch mode
#ifdef TW_FOAM2
    // (TW-FOAM2) the old foam the caps no longer hold opens up into the lace
    let laceCover = select(coverage, sat((foamSum + vertexFoam * activeNear) * tw.surface.z), capPatches);
#else
    let laceCover = select(coverage, sat(foamSum * tw.surface.z), capPatches);
#endif

    let fuv = lagXZ * tw.surface.w;
    let p1 = textureSample(waterFoamTex, smpAniso4Repeat, fuv);
    let r2 = vec2<f32>(fuv.x * 0.8 - fuv.y * 0.6, fuv.x * 0.6 + fuv.y * 0.8);
    let p2 = textureSample(waterFoamTex, smpAniso4Repeat, r2 * 2.37 + vec2<f32>(0.31, 0.77));
    let pattern = p1.x * 0.62 + p2.x * 0.38;
    let thresh = 1.05 - laceCover * 1.1;
    let soft = 0.06 + footprint * 0.1;
    let detail = smoothstep(thresh - soft, thresh + soft, pattern) * (p1.y * 0.25 + 0.8);
    let far = smoothstep(0.15, 1.2, footprint);
    // (OP, 2026-10-13) far off, the white the lace WOULD show at this coverage, not the coverage:
    // `laceCover * 0.85` drew a light sea up to 9x whiter than the lace it stands for (a 30 m/s
    // gale went white as milk with distance while close up it read as dark water with caps)
    var foam = mix(detail, laceExpected(laceCover), far);
    if (capPatches) {
        // a cap is solid white water with a ragged edge -- never a translucent veil (owner: "looks
        // like a white oil film") and never the lace's veins. The edge is the slow mottle, the
        // bubbles speckle its rim; a far cap stays white because its edge only softens with the
        // pixel footprint, it does not fade.
        // the fringe tears into streaks down the wind, not round floes: the mottle read on a
        // wind-aligned coordinate stretched 4:1
        let wdir = tw.wind.xy;
        let wuv = vec2<f32>(dot(lagXZ, wdir) * 0.25, dot(lagXZ, vec2<f32>(-wdir.y, wdir.x))) * tw.surface.w;
        let tear = textureSample(waterFoamTex, smpAniso4Repeat, wuv * 1.7 + vec2<f32>(0.21, 0.63)).z;
        let rag = capCover * (0.45 + 1.1 * mix(p1.z, tear, 0.6)) + (p1.y - 0.4) * 0.12;
        // a dense core whose rim the lace eats into, so a cap frays like foam instead of ending in a
        // sheet's hard edge (the floes of the first attempt); the core is where the crest breaks now
        let capSoft = min(0.25 + footprint * 0.05, 0.4); // unclamped, a far pixel turned the horizon white
        let body = smoothstep(0.5 - capSoft, 0.5 + capSoft, rag);
        // the core is opaque (owner: "white caps need to be whiter"); only the rim is lace
        let cap = sat(body * 1.45 - (1.0 - pattern) * 0.75) * mix(p1.y * 0.2 + 0.85, 1.0, smoothstep(0.7, 1.0, body));
        foam = max(foam, cap);
    }

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
    o.blood = bloodAmount(lagXZ);
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

// (OP) Tidewater draws no sea inside its boat's hull (SceneRenderer hull mask: a pre-pass of the hull volume,
// and the surface behind its nearest face discarded). Here the hull volume is tested directly: a point of the
// surface inside a masked hull's open cockpit and wheelhouse (Tidewater's lobster boat, below its gunwale) is not
// drawn -- from any camera, the cockpit included. Its cockpit sole stands 0.35 m over the waterline, under
// every swell's crest. Lanes: mod.rs hull_mask_lanes.
fn insideHull(p: vec3<f32>) -> bool {
    for (var i = 0; i < 2; i++) {
        let h3 = tw.hulls[i * 4 + 3];
        if (h3.z < 0.5) { continue; }
        let h0 = tw.hulls[i * 4];
        let h1 = tw.hulls[i * 4 + 1];
        let h2 = tw.hulls[i * 4 + 2];
        let d = p - h0.xyz;
        let lz = dot(d, h1.xyz);
        if (lz <= h1.w || lz >= h2.w) { continue; }
        let ly = dot(d, h2.xyz);
        let t = (lz - h1.w) / (h2.w - h1.w);
        let sheer = mix(h3.x, h3.y, t);
        if (ly < -0.8 || ly > sheer) { continue; }
        // The mask spans the open cockpit and the wheelhouse only (transom to house front, where the hull is
        // still near its full breadth: HullLines.sheerX 0.88 of the beam at the transom, 0.92 at the house
        // front); forward of it the foredeck closes the hull over the water. The flare: the waterline is 0.83
        // of the sheer's breadth (2.42 m against 2.9 m), the bottom narrower still. Kept well inside the shell,
        // so the sea just outside the hull is never cut.
        let plan = mix(0.88, 0.92, t);
        let flare = select(mix(0.83, 0.3, clamp(-ly / 0.5, 0.0, 1.0)), mix(0.83, 1.0, clamp(ly / sheer, 0.0, 1.0)), ly >= 0.0);
        let lx = abs(dot(d, cross(h2.xyz, h1.xyz)));
        if (lx < h0.w * plan * flare - 0.08) { return true; }
    }
    return false;
}

@fragment
fn fs_tidewater(in: VsOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    let rel = in.rel;
    let pos = rel + frame.cam_pos.xyz;
    if (insideHull(pos)) { discard; }
    let screenUV = in.clip.xy / vec2<f32>(textureDimensions(waterSceneDepth));
    let posV = twView(vec4<f32>(rel, 1.0));
    let lagXZ = in.lagXZ;
    let groundH = materialTerrainHeightAt(pos.xz);
    let vDepth = select(in.seaDepth, waterLevelAt(pos.xz) - groundH, earthWater());
    let vHeight = in.waveH;
    // derivatives in uniform control flow
    let footprint = max(length(fwidth(lagXZ)), 1e-4);
    let dwx = dpdx(rel);
    let dwy = dpdy(rel);
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
    if (linear > 0.5 && frame.sun_diffuse.w > 0.5) {
        // Match physical Earth land's directional sky irradiance. The legacy
        // flat ambient RGB is an artistic sun-scaled fill, not sky radiance;
        // feeding it to the water volume/foam produces an ambient wash.
        // (OP, 2026-10-12) on every physical world, not only Earth: on stock worlds the fill is ~30x
        // the sun, and it lit the water's in-scatter and the foam into neon turquoise and glowing caps.
        let skyN = select(vec3<f32>(0.0, 1.0, 0.0), earthWaterNormal(pos.xz), earthWater());
        skyIrr = sky_irradiance(skyN) * frame.sun_ambient.w;
    }
    var csm = 0.0;
    var ter = 0.0;
    var cloudShadow = 1.0;
    if (!earthWater()) {
        csm = shadow_strength(rel, vec3<f32>(0.0, 1.0, 0.0), fogK, dwx, dwy);
        ter = terrain_sun_shadow(pos.xz, pos.y) * fogK;
        cloudShadow = cloud_sun_shadow(pos.xz);
    }
    var sunLight = sunDiffuse * PI * (1.0 - max(csm, ter)) * cloudShadow;

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
    // (OP, 2026-10-12) away from the lens, a point is seen from below only when the eye is under
    // the water AND the point lies above the eye. It was "the eye is under the water" alone, so a crest
    // passing over a swimmer's eye turned the whole distant sea, seen through that crest from above,
    // into the view from below: total internal reflection, the water's own teal in-scatter, a teal band
    // along the horizon at the waterline (owner's "turquoise in high waves").
    let farFromBelow = camH < 0.0 && pos.y > frame.cam_pos.y;
    let viewFromBelow = select(farFromBelow, !seenFromAbove, nearSurface && !folded);
    let Nup = surf.normal;
#ifdef SEA_SPECAA
    // (OP, SEA-SPECAA) the cost-free part of LEAN (open-water's water.js, after Kaplanyan & Hoffman
    // 2016): the spread of normals inside the pixel footprint is slope variance the sun lobe must
    // carry, or sub-pixel ripples alias into crawling hairlines; taken here in uniform control flow
    let aaNx = dpdx(Nup);
    let aaNy = dpdy(Nup);
    let specAA = min(0.5 * (dot(aaNx, aaNx) + dot(aaNy, aaNy)), 0.08);
#endif
    let Nside = select(Nup, -Nup, viewFromBelow);
    let Nview = normalize(Nside + V * max(-dot(Nside, V) + 0.03, 0.0));

    // roughness from unresolved slope variance (Cox-Munk: mss = 0.003 + 0.00512 U)
    let mss = (0.003 + tw.sea.z * 0.00512) * tw.surface.y;
    let kpx = PI / footprint;
    let unresolved = sat(log2(110.0 / kpx) / 9.0);
    let roughVar = surf.rough * surf.rough;
#ifdef SEA_RAIN
    // (OP, SEA-RAIN) rain on the sea, far field (after open-water's ripples.js, MIT): rain damps the
    // capillary wind ripples (unresolved Cox-Munk term x (1 - 0.4 rain)), and the drop rings too small
    // for the pixel become roughness (slope variance 0.006 rain per axis) where waterSurfaceFragment
    // no longer draws them
    let seaRain = sat(frame.ground_weather.y);
    let rainCalm = 1.0 - 0.4 * seaRain;
    let rainVar = 0.006 * seaRain * smoothstep(0.025, 0.04, footprint);
    let alpha2 = tw.material1.x * tw.material1.x + mss * 2.0 * unresolved * roughVar * rainCalm + rainVar * 2.0 + foam * 0.2 + surf.aeration * 0.03;
    let sigmaUnres = sqrt(mss * unresolved * roughVar * rainCalm + rainVar);
#else
    let alpha2 = tw.material1.x * tw.material1.x + mss * 2.0 * unresolved * roughVar + foam * 0.2 + surf.aeration * 0.03;
    let sigmaUnres = sqrt(mss * unresolved * roughVar);
#endif

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
        let gradH = vec2<f32>(materialTerrainHeightAt(pos.xz + vec2<f32>(e, 0.0)) - materialTerrainHeightAt(pos.xz - vec2<f32>(e, 0.0)),
                              materialTerrainHeightAt(pos.xz + vec2<f32>(0.0, e)) - materialTerrainHeightAt(pos.xz - vec2<f32>(0.0, e))) / (2.0 * e);
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
#ifdef SEA_SPECAA
        // ... capped at the sea's whole slope variance (alpha^2 ~ 2 mss), so the glitter path keeps
        // narrowing toward the horizon instead of widening with the footprint
        let alphaSun = min(alpha2 + 2.0 * specAA, max(alpha2, 2.0 * mss));
        let spec = _waterDGGX(NdH, alphaSun) * _waterVSmithGGX(NdL, NdV, alphaSun) * Fs * NdL;
#else
        let spec = _waterDGGX(NdH, alpha2) * _waterVSmithGGX(NdL, NdV, alpha2) * Fs * NdL;
#endif
#ifdef SEA_GLINTS
        // (OP, SEA-GLINTS) the sun's glitter carried by capillary facets fixed on the moving water
        // (open-water's facet statistics, napali's footprint-filtered points; both MIT): one facet a
        // cell, its tilt about the resolved surface drifting smoothly with time, glints where that
        // tilt mirrors the sun into the eye, lighting the pixel by the share of it the facet covers.
        // Normalised by the expected glint so the glitter path keeps its mean. Anchored to the water
        // (lagXZ), not to the screen: sparkles ride the waves and twinkle instead of re-rolling per
        // pixel, which DLSS averaged into nothing.
        var sunSpec = sunLight * min(spec, 400.0);
        // facet slopes: the resolved roughness and the capillaries under it
        let sig = sqrt(max(alpha2, 1e-5) * 0.5 + GLINT_CAPILLARY * GLINT_CAPILLARY);
        // the facet tilt that mirrors the sun, in a world-stable frame about the shading normal
        let t1 = normalize(vec3<f32>(1.0, 0.0, 0.0) - N * N.x);
        let t2 = cross(N, t1);
        let hN = max(dot(H, N), 1e-3);
        let need = vec2<f32>(dot(H, t1), dot(H, t2)) / hN;
        // past 2.5 spreads hardly a facet mirrors the sun (one in ~80)
        if (dot(need, need) < 6.25 * sig * sig && NdL > 0.0) {
            let lod = max(log2(footprint * 1.5 / GLINT_CELL), 0.0);
            let l0 = floor(lod);
            // one facet octave, two only across the middle of the step between them
            let k = smoothstep(0.3, 0.7, lod - l0);
            var g = 0.0;
            if (k < 1.0) {
                g = glintFacet(lagXZ, GLINT_CELL * exp2(l0), u32(l0), need, sig, footprint) * (1.0 - k);
            }
            if (k > 0.0) {
                g += glintFacet(lagXZ, GLINT_CELL * exp2(l0 + 1.0), u32(l0) + 1u, need, sig, footprint) * k;
            }
            // a glint: GLINT_PEAK times the smooth path's peak (the GGX lobe at NdH = 1), Fresnel at
            // the mirror angle; faded in over the first light of day like the lobe
            let peak = _waterDGGX(1.0, alpha2) * _waterVSmithGGX(NdL, NdV, alpha2) * Fs * NdL;
            sunSpec = min(sunSpec + sunLight * (g * GLINT_PEAK * min(peak, 400.0)), vec3<f32>(30000.0));
        }
#else
        let sunSpec = sunLight * min(spec, 400.0);
#endif

        // ---- refraction / water volume (Snell ray to the sea floor)
        let Tr = refract(-V, N, 1.0 / IOR);
        let Tv = normalize(vec3<f32>(Tr.x, min(Tr.y, -0.08), Tr.z));
        let tDown = max(-Tv.y, 0.04);
        let surfViewZ = posV.z;

        let L0 = max(pos.y - groundH, 0.0) / tDown;
        var Lt = L0;
        if (L0 < 100.0) {
            let L1 = max(pos.y - materialTerrainHeightAt(pos.xz + Tv.xz * min(L0, 200.0)), 0.0) / tDown;
            Lt = max(pos.y - materialTerrainHeightAt(pos.xz + Tv.xz * min(L1 * 0.5 + L0 * 0.5, 200.0)), 0.0) / tDown;
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
        // (OP) a whale's blood: it takes the green and blue out of the light and scatters back a dark red
        let sigA = tw.absorption.rgb + surfMed.absorb * sandK + surf.blood * vec3<f32>(0.25, 2.6, 2.3);
        // (bubble plumes are shallow and patchy: a moderate scatterer, milky turquoise rather than a glow)
        let sigS = tw.scattering.rgb + surfMed.scatter * sandK + aer * 1.6 + surf.blood * vec3<f32>(0.1, 0.004, 0.004);
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
        // (OP) temperate green, not Tidewater's tropical cyan (0.12, 0.55, 0.45): see tw_lip.wgsl tint
        let sssCol = vec3<f32>(0.13, 0.36, 0.30) * 0.06;
        let sss = sunLight * sssCol * back * crest * tw.material0.y * smoothstep(0.0, 0.25, L.y);

        // (W3j) the water's own glow (light scattered inside it toward the eye) x the Water tab's
        // "TW water glow" (tw.material1.w). Tidewater's formula; OP's lighting makes it several
        // times brighter relative to the reflection than in Tidewater, which turned deep water
        // pale cyan (debug view 22). The scale brings it back between Tidewater's reflection-led
        // navy and Current OP's steel blue.
        let glowK = tw.material1.w;
        let transmitted = sceneCol * Tview * (1.0 - 0.3 * lipW) + (inSun + inAmb + sss) * glowK;

        // ---- foam: diffuse scatterer, wrapped sun + sky irradiance
        // Tidewater's 0.85 (owner: "white caps must not be gray"). The glow (owner, 2026-10-12) was
        // never the albedo: it was the legacy flat ambient (foamSky below), ~30x the sun here.
        let foamAlbedo = 0.85;
        // OP: the foam's sky light is the sky's own irradiance (SH, E/PI) wherever the frame is
        // physical, not the legacy flat ambient (an artistic sun-scaled fill, ~30x the sun here):
        // with it every cap clipped past white and bloomed (owner, 2026-10-12). Earth water already
        // reads the SH sky for the same reason (above).
        var foamSky = skyIrr;
        if (linear > 0.5 && frame.sun_diffuse.w > 0.5) {
            foamSky = sky_irradiance(N) * frame.sun_ambient.w;
        }
        var foamLit = (sunLight * (max(dot(N, L), 0.0) * 0.75 + 0.25) * INV_PI + foamSky * 0.95) * foamAlbedo;
        if (simOn) { foamLit = surfFoamLight(surf.foamInfo, foamGrad, N, L, V, sunLight, foamSky); }
        // (foam on bloody water is pink)
        let foamCol = foamLit * tw.material0.z * mix(vec3<f32>(1.0), vec3<f32>(0.95, 0.42, 0.4), surf.blood);

        let rim = smoothstep(0.0, 0.025, frontD) * smoothstep(0.1, 0.035, frontD) * uprush;
        var water = mix(transmitted, reflCol, F) + sunSpec + skyRefl * (0.22 * rim);
        // (OP) blood at the surface: a dark red film over the water that dulls its reflection (the medium terms
        // above only colour what is seen through the water, which at a grazing view is almost nothing)
        if (surf.blood > 0.001) {
            let bloodLit = (sunLight * (max(dot(N, L), 0.0) * 0.6 + 0.2) * INV_PI + skyIrr * 0.9) * vec3<f32>(0.055, 0.004, 0.0035);
            water = mix(water, bloodLit + sunSpec * 0.5, surf.blood * 0.85);
        }
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
        // (OP, 2026-10-12) the Snell window shimmers: fine ripples the FFT mesh does not resolve bend
        // the refracted sky near the eye (it read as undistorted sky through a clean hole)
        let ripT = tw.sea.y;
        let rip = vec2<f32>(sin(lagXZ.x * 3.1 + ripT * 1.7 + sin(lagXZ.y * 2.3)), cos(lagXZ.y * 2.9 - ripT * 1.3 + sin(lagXZ.x * 1.9)))
            + 0.5 * vec2<f32>(sin(lagXZ.y * 7.3 - ripT * 2.9), cos(lagXZ.x * 6.7 + ripT * 3.1));
        let Nr = normalize(N + vec3<f32>(rip.x, 0.0, rip.y) * 0.09 * smoothstep(30.0, 2.0, dist));
        let Tt = refract(-V, Nr, IOR);
        let tValid = dot(Tt, Tt) > 0.5;
        // (OP, 2026-10-13) the mirror share from the SAME normal the ray is refracted through, and all
        // mirror where that ray cannot leave the water. It came from the unrippled normal: wherever the
        // ripple tipped a ray past the critical angle the shader still let (1 - F) through and, with no
        // refracted ray, showed the zenith sky -- bright sky and clouds across the total-reflection
        // band just under the surface (owner: clouds visible at 0.7 m, gone at 2.5 m)
        let F = select(1.0, fresnelDielectric(max(dot(Nr, V), 1e-4), 1.0 / IOR), tValid);
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
    if (earthWater()) {
        // Source coverage is not a rectangular ocean boundary. Existing CDLOD
        // geometry follows the sphere; reject its hidden far side analytically.
        // Match the pipeline's GreaterEqual reverse-Z test against the opaque
        // snapshot explicitly: unknown source height must never override land.
        // Seen from below (the camera under the sea: free flight dives), every point of the surface faces
        // away; rejecting those as the planet's far side removed the whole underside, and the view up went
        // straight through to the open sky. The far side is out of sight from under the water anyway.
        let camBelow = frame.cam_pos.y < waterLevelAt(frame.cam_pos.xz);
        if (thickness <= 0.0 || (!camBelow && dot(earthWaterNormal(pos.xz), -rel) <= 0.0) ||
            in.clip.z < _waterSceneDepthAt(screenUV)) { discard; }
        // From high up the sea becomes the imagery (WaterWgpu::DrawEarthWater stops drawing it at the top of
        // this band). It did so from 6-8 km, where the imagery's zooms (and its sources: Esri, EOX where Esri
        // fails) disagree about the sea's colour and drew rings round the camera (Helgoland at 9 km, 2026-10-25).
        let fade = 1.0 - smoothstep(EARTH_SEA_FADE_START, EARTH_SEA_CEILING, frame.cam_pos.y);
        let base = textureSampleLevel(waterSceneColor, smpLinearClamp, screenUV, 0.0).rgb;
        res = mix(base, res, fade);
    }
    return vec4<f32>(res, 1.0);
}
