// (OP) Whole World terrain sun shadow (gfx3d/earth_shadow.rs). For each texel of two cascades around the camera,
// march from the ground toward the sun across the elevation pyramid's heights and store the height a point there
// must be above to see the sun: max over the march of (terrain height - distance * tan(sun elevation)). shader3d's
// Earth ground compares its own height with it (earth_terrain_lit).

struct Params {
    origin: vec4<f32>,  // sin(origin latitude), cos(origin latitude), origin longitude (radians), 0
    sun: vec4<f32>,     // unit direction to the sun (world), tan(sun elevation)
    c0: vec4<f32>,      // cascade 0: world x, z of texel (0, 0)'s corner, texel metres, samples a side
    c1: vec4<f32>,      // cascade 1
    rows: vec4<u32>,    // first row, rows, 0, 0
};
@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var heights: texture_2d<f32>;     // EarthHeightCache atlas (Terrarium RGBA8)
@group(0) @binding(2) var height_lut: texture_2d<f32>;  // its lookup
@group(0) @binding(3) var out_map: texture_storage_2d<r32float, write>;

const PER_ROW: i32 = 16;   // EarthHeightCache::SlotsPerRow
const LUT_ROWS: i32 = 17;  // EarthHeightCache::LookupRowsPerLevel
const MAX_LEVEL: i32 = 15;
const R: f32 = 6378137.0;
const ANCHOR: f32 = 6400.0;
const PI: f32 = 3.14159265358979;

fn u24(c: vec4<f32>) -> i32 {
    return i32(round(c.r * 255.0)) + i32(round(c.g * 255.0)) * 256 + i32(round(c.b * 255.0)) * 65536;
}
fn slot_of(level: i32, tx: i32, ty: i32) -> i32 {
    let n = 1 << u32(level);
    if (ty < 0 || ty >= n) { return -1; }
    let x = ((tx % n) + n) % n;
    let row = level * LUT_ROWS;
    let ox = u24(textureLoad(height_lut, vec2<i32>(0, row), 0));
    let oy = u24(textureLoad(height_lut, vec2<i32>(1, row), 0));
    let dx = ((x - ox) % n + n) % n;
    let dy = ty - oy;
    if (dx >= 16 || dy < 0 || dy >= 16) { return -1; }
    let e = textureLoad(height_lut, vec2<i32>(dx, row + 1 + dy), 0);
    if (e.a < 0.5) { return -1; }
    return u24(e);
}
fn decode(c: vec4<f32>) -> f32 {
    return round(c.r * 255.0) * 256.0 + round(c.g * 255.0) + round(c.b * 255.0) / 256.0 - 32768.0;
}
// Nearest texel's height at global pixel (px, py) of zoom `level`: (height, 1), or (0, 0) when not resident.
fn texel(level: i32, px: i32, py: i32) -> vec2<f32> {
    let slot = slot_of(level, px >> 8u, py >> 8u);
    if (slot < 0) { return vec2<f32>(0.0); }
    let c = textureLoad(heights, vec2<i32>((slot % PER_ROW) * 256 + (px & 255), (slot / PER_ROW) * 256 + (py & 255)), 0);
    return vec2<f32>(decode(c), 1.0);
}
// The ground's world height (its height less the curvature drop) at world xz, from zoom `level` or the nearest
// resident coarser one; -1e9 where nothing is resident.
fn ground_y(xz: vec2<f32>, level_wanted: i32) -> f32 {
    let d = xz - vec2<f32>(ANCHOR);
    let r2 = dot(d, d);
    let up = sqrt(max(R * R - r2, 0.0));
    let drop = r2 / (R + up);
    // Earth::FromDirection: the point's latitude and longitude
    let n = vec3<f32>(d.x, up, d.y) / R;
    let north = P.origin.x * n.y + P.origin.y * n.z;
    let equator = P.origin.y * n.y - P.origin.x * n.z;
    let lat = atan2(north, length(vec2<f32>(n.x, equator)));
    var lon = P.origin.z + atan2(n.x, equator);
    lon = lon - 2.0 * PI * floor((lon + PI) / (2.0 * PI));
    let mx = (lon + PI) / (2.0 * PI);
    let my = (PI - asinh(tan(clamp(lat, -1.4844, 1.4844)))) / (2.0 * PI);
    for (var level = level_wanted; level >= 0; level = level - 1) {
        // bilinear, as the ground is drawn: nearest texels stepped the threshold and striped lit slopes
        let scale = f32(256 << u32(level));
        let q = vec2<f32>(mx, my) * scale - 0.5;
        let i = vec2<i32>(floor(q));
        let f = q - floor(q);
        let a = texel(level, i.x, i.y);
        let b = texel(level, i.x + 1, i.y);
        let c = texel(level, i.x, i.y + 1);
        let e = texel(level, i.x + 1, i.y + 1);
        if (a.y * b.y * c.y * e.y > 0.5) {
            return mix(mix(a.x, b.x, f.x), mix(c.x, e.x, f.x), f.y) - drop;
        }
    }
    return -1.0e9;
}

@compute @workgroup_size(8, 8)
fn cs_earth_shadow(@builtin(global_invocation_id) id: vec3<u32>) {
    let n = u32(P.c0.w);
    let row = P.rows.x + id.y;
    if (id.x >= 2u * n || id.y >= P.rows.y || row >= n) { return; }
    let cascade = id.x / n;
    let c = select(P.c0, P.c1, cascade == 1u);
    let col = id.x % n;
    let xz = c.xy + (vec2<f32>(f32(col), f32(row)) + 0.5) * c.z;
    var threshold = -1.0e9;
    if (P.sun.y <= 0.0) {
        threshold = 1.0e9; // the sun is down: no direct light to cast shadows from
    } else {
        let toward = normalize(P.sun.xz + vec2<f32>(1e-6, 0.0));
        let tan_e = P.sun.w;
        // From the texel's own size outwards, each step a little longer than the last, to 60 km or until no
        // terrain (at most 9 km high) could still rise above the ray.
        let cos_lat = P.origin.y;
        var t = 2.0 * c.z; // the texel's own slope is the shading's, not a shadow
        for (var i = 0; i < 112; i = i + 1) {
            let step = max(c.z, t * 0.06);
            let level = clamp(i32(log2(40075016.0 * max(cos_lat, 0.05) / (256.0 * step))), 0, MAX_LEVEL);
            let y = ground_y(xz + toward * t, level);
            threshold = max(threshold, y - t * tan_e);
            t = t + step;
            if (t > 60000.0 || 9000.0 - t * tan_e < threshold) { break; }
        }
    }
    textureStore(out_map, vec2<i32>(i32(id.x), i32(row)), vec4<f32>(threshold, 0.0, 0.0, 1.0));
}
