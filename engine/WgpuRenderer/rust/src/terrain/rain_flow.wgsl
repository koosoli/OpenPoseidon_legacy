// RAIN-FLOW (OP, 2026-10-13): rain that runs off and gathers, instead of puddles painted on by a
// noise mask (owner: "can we make our rain and rain puddle system better inspired with the tarp
// runoff demo" of dgreenheck/threejs-particle-fluids). The water near the camera is a shallow-
// water height field on the ground ("virtual pipes": O'Brien & Hodgins 1995, Mei et al. 2007):
// every cell holds a water depth and four outflows to its neighbours, driven by the difference of
// ground + water height. Rain adds water, the soil takes most of it, the rest runs downhill and
// gathers in the ground's dips -- the same dips the noise puddles used to sit in, so the puddles
// are where they were, but they now fill, overflow, run off down slopes and dry out again.
//
// The grid is N x N cells of `cell` metres around the camera, addressed toroidally: an absolute
// cell (x, z) lives in slot (x mod N, z mod N), so the window scrolls without copying. A slot's
// absolute cell is kept in `keys`; a slot whose key is not the cell it now stands for is reset
// (cs_validate), which is how cells entering the window start dry.

struct RainParams {
    origin: vec2<i32>,      // the window's first absolute cell
    n: u32,                 // cells per side
    substep: u32,
    cell: f32,              // cell size (m)
    dt: f32,                // seconds per substep
    rain: f32,              // water arriving (m/s)
    soak: f32,              // water the soil takes (m/s, at most what is there)
    hm_origin: vec2<f32>,   // the heightmap's world origin
    hm_grid: f32,           // its spacing (m)
    sea_level: f32,
    hm_dims: vec2<u32>,
    _pad: vec2<u32>,
};

@group(0) @binding(0) var<uniform> rp: RainParams;
@group(0) @binding(1) var heightmap: texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> depth: array<f32>;
@group(0) @binding(3) var<storage, read_write> flux: array<vec4<f32>>; // out to -x, +x, -z, +z (m3/s)
@group(0) @binding(4) var<storage, read_write> keys: array<u32>;
@group(0) @binding(5) var out_tex: texture_storage_2d<rgba16float, write>;
// ground + micro relief per slot, computed once when the slot takes its cell (cs_validate)
@group(0) @binding(6) var<storage, read_write> ground: array<f32>;

const RF_G: f32 = 9.81;
const RF_DAMP: f32 = 0.985;

fn rf_hm(ix: i32, iz: i32) -> f32 {
    let c = clamp(vec2<i32>(ix, iz), vec2<i32>(0), vec2<i32>(rp.hm_dims) - 1);
    return textureLoad(heightmap, c, 0).x;
}

// terrain.wgsl sample_height's triangle interpolation (without mud / sand offsets)
fn rf_ground(xz: vec2<f32>) -> f32 {
    let t = (xz - rp.hm_origin) / rp.hm_grid;
    let b = floor(t);
    let ix = i32(b.x);
    let iz = i32(b.y);
    let f = t - b;
    let y00 = rf_hm(ix, iz);
    let y01 = rf_hm(ix + 1, iz);
    let y10 = rf_hm(ix, iz + 1);
    let y11 = rf_hm(ix + 1, iz + 1);
    if (f.x <= 1.0 - f.y) {
        return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x;
    }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y;
}

// ground_puddles.wgsl's field (puddle_hash / puddle_noise), so the dips are where the noise
// puddles were
fn rf_hash(p: vec2<f32>) -> f32 {
    let q = vec2<u32>(vec2<i32>(p));
    var h = q.x * 1597334677u ^ q.y * 3812015801u;
    h = (h ^ (h >> 16u)) * 2246822519u;
    return f32(h ^ (h >> 13u)) / 4294967295.0;
}

fn rf_noise(p: vec2<f32>) -> f32 {
    let b = floor(p);
    let f = fract(p);
    let w = f * f * (3.0 - 2.0 * f);
    return mix(mix(rf_hash(b), rf_hash(b + vec2<f32>(1.0, 0.0)), w.x),
               mix(rf_hash(b + vec2<f32>(0.0, 1.0)), rf_hash(b + vec2<f32>(1.0)), w.x), w.y);
}

// the ground's micro relief (m): dips under the puddle field's peaks, and an uneven surface
// elsewhere -- without it water filmed whole 12.5 m terrain triangles, straight-edged (2026-10-13)
fn rf_micro(xz: vec2<f32>) -> f32 {
    let field = rf_noise(xz / 6.0) * 0.8 + rf_noise(xz / 2.2) * 0.2;
    return -smoothstep(0.5, 0.85, field) * 0.07 + (rf_noise(xz / 3.1) - 0.5) * 0.06 + (rf_noise(xz / 0.9) - 0.5) * 0.012;
}

fn rf_slot_of(abs_cell: vec2<i32>) -> u32 {
    let n = i32(rp.n);
    let s = ((abs_cell % n) + n) % n;
    return u32(s.y) * rp.n + u32(s.x);
}

// the absolute cell slot (sx, sz) stands for now
fn rf_abs_of(slot: vec2<i32>) -> vec2<i32> {
    let n = i32(rp.n);
    let o = ((rp.origin % n) + n) % n;
    let rel = ((slot - o) % n + n) % n;
    return rp.origin + rel;
}

fn rf_key(c: vec2<i32>) -> u32 {
    return (u32(c.x) & 0xffffu) | ((u32(c.y) & 0xffffu) << 16u);
}

fn rf_world(c: vec2<i32>) -> vec2<f32> {
    return (vec2<f32>(c) + 0.5) * rp.cell;
}

// ground + micro relief + water at an absolute cell
fn rf_level(c: vec2<i32>) -> f32 {
    let i = rf_slot_of(c);
    return ground[i] + depth[i];
}

fn rf_inside(c: vec2<i32>) -> bool {
    let r = c - rp.origin;
    return all(r >= vec2<i32>(0)) && all(r < vec2<i32>(i32(rp.n)));
}

@compute @workgroup_size(8, 8)
fn cs_validate(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= rp.n || gid.y >= rp.n) { return; }
    let slot = vec2<i32>(gid.xy);
    let c = rf_abs_of(slot);
    let i = gid.y * rp.n + gid.x;
    let k = rf_key(c);
    if (keys[i] != k) {
        keys[i] = k;
        depth[i] = 0.0;
        flux[i] = vec4<f32>(0.0);
        let xz = rf_world(c);
        ground[i] = rf_ground(xz) + rf_micro(xz);
    }
}

@compute @workgroup_size(8, 8)
fn cs_flux(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= rp.n || gid.y >= rp.n) { return; }
    let c = rf_abs_of(vec2<i32>(gid.xy));
    let i = gid.y * rp.n + gid.x;
    let d = depth[i];
    let h = rf_level(c);
    let a = rp.cell; // pipe cross-section / length
    var f = flux[i] * RF_DAMP;
    var nb = array<vec2<i32>, 4>(vec2<i32>(-1, 0), vec2<i32>(1, 0), vec2<i32>(0, -1), vec2<i32>(0, 1));
    for (var k = 0; k < 4; k++) {
        let o = c + nb[k];
        // the window's edge is a wall: water neither leaves nor enters through it
        var dh = 0.0;
        if (rf_inside(o)) { dh = h - rf_level(o); }
        f[k] = max(0.0, f[k] + rp.dt * RF_G * a * dh);
    }
    // never more out than there is
    let out = (f.x + f.y + f.z + f.w) * rp.dt;
    let have = d * rp.cell * rp.cell;
    if (out > have) { f = f * (have / max(out, 1e-9)); }
    flux[i] = f;
}

@compute @workgroup_size(8, 8)
fn cs_depth(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= rp.n || gid.y >= rp.n) { return; }
    let c = rf_abs_of(vec2<i32>(gid.xy));
    let i = gid.y * rp.n + gid.x;
    let f = flux[i];
    var inflow = 0.0;
    // what the neighbours send this way: -x neighbour's +x pipe, +x's -x, -z's +z, +z's -z
    if (rf_inside(c + vec2<i32>(-1, 0))) { inflow += flux[rf_slot_of(c + vec2<i32>(-1, 0))].y; }
    if (rf_inside(c + vec2<i32>(1, 0))) { inflow += flux[rf_slot_of(c + vec2<i32>(1, 0))].x; }
    if (rf_inside(c + vec2<i32>(0, -1))) { inflow += flux[rf_slot_of(c + vec2<i32>(0, -1))].w; }
    if (rf_inside(c + vec2<i32>(0, 1))) { inflow += flux[rf_slot_of(c + vec2<i32>(0, 1))].z; }
    let outflow = f.x + f.y + f.z + f.w;
    var d = depth[i] + (inflow - outflow) * rp.dt / (rp.cell * rp.cell);
    d = max(d + (rp.rain - rp.soak) * rp.dt, 0.0);
    // the sea takes what reaches it
    if (ground[i] < rp.sea_level + 0.05) { d = 0.0; }
    depth[i] = d;
    if (rp.substep == 1u) {
        // flow speed (m/s): the net flux through the cell over its wetted cross-section
        let v = vec2<f32>(f.y - f.x, f.w - f.z) / max(d * rp.cell, 1e-4);
        textureStore(out_tex, vec2<i32>(gid.xy), vec4<f32>(d, min(length(v), 4.0), 0.0, 1.0));
    }
}
