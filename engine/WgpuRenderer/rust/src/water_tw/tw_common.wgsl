// Tidewater Native — declarations shared by the water material (tw_water.wgsl) and the shore
// simulation kernel (shore_sim.rs): the per-frame surface block, the heightmap conform block and
// OP's terrain height (the same triangulation the terrain draws). The consumers declare the
// bindings `tw`, `cf` and `seabed_heightmap` themselves (different groups / numbers).

const PI: f32 = 3.141592653589793;
const INV_PI: f32 = 0.3183098861837907;
const IOR: f32 = 1.333;

fn sat(x: f32) -> f32 { return clamp(x, 0.0, 1.0); }

struct TwSurface {
    sea: vec4<f32>,        // sea level, water time, wind speed (m/s), camera water height
    wind: vec4<f32>,       // wind dir xz (toward), sea-detail drift offset xy (m)
    surface: vec4<f32>,    // amplitude, slope scale, foam coverage, foam pattern scale (1/m)
    material0: vec4<f32>,  // backscatter, sss, foam intensity, reflection strength
    material1: vec4<f32>,  // roughness, ssr on, debug view, water glow (W3j: in-scatter scale)
    absorption: vec4<f32>, // rgb 1/m
    scattering: vec4<f32>, // rgb 1/m
    detail: vec4<f32>,     // gust, slick, streak amounts
    shore0: vec4<f32>,     // ShoreParams: period (s), amplitude (H/2, m), variation, gamma
    shore1: vec4<f32>,     // ShoreParams: breakSpan, curl, runup, enabled (0 until the field exists)
    shore2: vec4<f32>,     // ShoreParams: turbidity (1/m), swell heading (rad, travel), shore field centre xz
    shoreGrid: vec4<f32>,  // shore field: origin xz (world), cell size (m), unused
    sim: array<vec4<f32>, 2>, // ShoreSim regions: min xz (world), size (m), weight (0 = inactive, fades in)
    simP0: vec4<f32>,      // ShoreSimParams: dryTime, foamLife, surfFoamLife, residueLife (s)
    simP1: vec4<f32>,      // ShoreSimParams: foamGen, depositGain, dt (s), shore sim on
    brk: vec4<f32>,        // Breakers: station count, lip opacity (Tidewater `sheet`); W5b caustic tiles (fine, broad) m
    morph: array<vec4<f32>, 16>, // CDLOD per level: morph start, 1/range, spacing (W9b: 16 levels)
    wake: array<vec4<f32>, 8>,   // W6: the wake readers' lanes, 4 per slot (tw_wake.wgsl)
    impacts: array<vec4<f32>, 8>, // W8b: explosions in the surface, 2 lanes per slot (tw_impact.wgsl)
    probe: vec4<f32>,             // W9a/W12d: grid origin lattice x, z, step (m), size (tw_probe.wgsl)
    rotorDomain: vec4<f32>,       // camera interaction domain: min XZ, size, reciprocal
    rotorControl: vec4<f32>,      // frozen water clock, source count, reserved
    rotorSources: array<vec4<f32>, 8>, // actual rotor-only world XZ, radius, strength
};

struct ConformParams {
    origin: vec2<f32>,
    terrain_grid: f32,
    enabled: f32,
    hm_width: u32,
    hm_height: u32,
};

// ------------------------------------------------------------------ terrain (OP heightmap)

fn hm_load(ix: i32, iz: i32) -> f32 {
    let cx = clamp(ix, 0, i32(cf.hm_width) - 1);
    let cz = clamp(iz, 0, i32(cf.hm_height) - 1);
    return textureLoad(seabed_heightmap, vec2<i32>(cx, cz), 0).x;
}

// Same triangulation as terrain.wgsl's sample_height (and water.wgsl's seabed_height), so the
// water's idea of the ground is the ground the terrain draws. -500 without a heightmap
// (Tidewater's "no terrain" value).
fn terrainHeightAt(xz: vec2<f32>) -> f32 {
    if (cf.enabled <= 0.5 || cf.hm_width < 2u || cf.hm_height < 2u) {
        return -500.0;
    }
    let t = (xz - cf.origin) / max(cf.terrain_grid, 1e-4);
    let base = floor(t);
    let ix = i32(base.x);
    let iz = i32(base.y);
    let f = t - base;
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + 1, iz);
    let y10 = hm_load(ix, iz + 1);
    let y11 = hm_load(ix + 1, iz + 1);
    // select, not an early return: keeps the fragment's derivatives in uniform control flow
    let lower = y00 + (y10 - y00) * f.y + (y01 - y00) * f.x;
    let upper = y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y;
    let h = select(upper, lower, f.x <= 1.0 - f.y);
    // (W9b, OP) beyond the map the heightmap repeats its edge texels outward, so every edge texel
    // ran on to the horizon as a straight strip of its own depth: seen from high up, parallel
    // lines of shallow (cyan) or dry (holed) sea. The water's idea of the ground goes deep there
    // over 400 m instead (OP's own sea shows open water beyond the map too).
    let size = vec2<f32>(f32(cf.hm_width - 1u), f32(cf.hm_height - 1u)) * cf.terrain_grid;
    let r = xz - cf.origin;
    let outside = length(max(max(-r, r - size), vec2<f32>(0.0)));
    return mix(h, min(h, OFF_MAP_GROUND), smoothstep(0.0, 400.0, outside));
}

// (W9b) the ground beyond the map as the water sees it (absolute m; deep open sea)
const OFF_MAP_GROUND: f32 = -200.0;
