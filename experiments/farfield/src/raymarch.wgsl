// Far-field raymarch experiments (EXP-FAR-001..004).
//
// One compute pass, one ray per pixel, four modes:
//   0  fixed-step march over the bilinear heightfield          (EXP-FAR-001)
//   1  hierarchical max-mipmap march over the heightfield      (EXP-FAR-002)
//   2  hierarchical march over a pyramid that already includes
//      the canopy/roof shell, resolved as a hard surface        (EXP-FAR-004)
//   3  as 2, but the canopy band is marched volumetrically      (EXP-FAR-003)
//
// The hierarchical traversal is the "maximum mipmap" scheme. The safety condition
// is the one that matters and is easy to get wrong: a cell may be skipped only if
// the ray segment INSIDE that cell stays above the cell's max height for its whole
// length. For a straight ray the segment minimum is at one of its two endpoints,
// so testing min(y_entry, y_exit) > cell_max is exact. Testing only y_entry (the
// obvious thing) tunnels through ridges on any descending ray.

struct Params {
    cam: vec4<f32>,       // xyz camera world position, w tan(fovy/2)
    fwd: vec4<f32>,
    right: vec4<f32>,
    up: vec4<f32>,
    world: vec4<f32>,     // grid, n, levels, extent_m
    cfg: vec4<f32>,       // max_steps, far_dist, mode, fixed_step
    sun: vec4<f32>,       // xyz direction toward the sun
    cover: vec4<f32>,     // cover_n, cover_step, canopy_scale, building_scale
    bounds: vec4<f32>,    // h_min, h_max, sea_level, unused
    screen: vec4<f32>,    // width, height, unused, unused
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var height_tex: texture_2d<f32>;
@group(0) @binding(2) var top_tex: texture_2d<f32>;
@group(0) @binding(3) var cover_tex: texture_2d<f32>;
@group(0) @binding(4) var out_color: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(5) var out_steps: texture_storage_2d<r32uint, write>;

const SKY_TOP = vec3<f32>(0.28, 0.45, 0.78);
const SKY_HORIZON = vec3<f32>(0.72, 0.80, 0.90);

fn h_at_texel(ix: i32, iz: i32) -> f32 {
    let n = i32(P.world.y);
    let cx = clamp(ix, 0, n - 1);
    let cz = clamp(iz, 0, n - 1);
    return textureLoad(height_tex, vec2<i32>(cx, cz), 0).x;
}

// Terrain height at a world xz. `bounds.w` selects the RECONSTRUCTION FILTER, which
// is the whole point of EXP-COL-001: bilinear gives a smooth surface, nearest gives
// one flat top per texel with vertical walls between them - i.e. every heightfield
// texel becomes a vertical occupancy column. The stored data is identical; only the
// filter changes.
fn height_at(wxz: vec2<f32>) -> f32 {
    if (P.bounds.w > 0.5) {
        let t = vec2<i32>(floor(wxz / P.world.x));
        return h_at_texel(t.x, t.y);
    }
    let g = wxz / P.world.x;
    let i = floor(g);
    let f = g - i;
    let ix = i32(i.x);
    let iz = i32(i.y);
    let a = h_at_texel(ix, iz);
    let b = h_at_texel(ix + 1, iz);
    let c = h_at_texel(ix, iz + 1);
    let d = h_at_texel(ix + 1, iz + 1);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

fn cover_at(wxz: vec2<f32>) -> vec4<f32> {
    let cn = i32(P.cover.x);
    let c = vec2<i32>(clamp(vec2<i32>(floor(wxz / P.cover.y)), vec2<i32>(0), vec2<i32>(cn - 1)));
    return textureLoad(cover_tex, c, 0);
}

// Surface normal. In column mode the surface is piecewise constant, so a central
// difference returns zero on a cell top and garbage at a wall; the normal has to come
// from which face was actually hit.
fn normal_at_col(wxz: vec2<f32>, py: f32, dir: vec3<f32>) -> vec3<f32> {
    let top = surface_at(wxz);
    if (py < top - 0.35) {
        // A wall: face the ray back along whichever horizontal axis it crossed.
        let f = fract(wxz / P.world.x);
        let ex = min(f.x, 1.0 - f.x);
        let ez = min(f.y, 1.0 - f.y);
        if (ex < ez) {
            return vec3<f32>(select(1.0, -1.0, dir.x > 0.0), 0.0, 0.0);
        }
        return vec3<f32>(0.0, 0.0, select(1.0, -1.0, dir.z > 0.0));
    }
    return vec3<f32>(0.0, 1.0, 0.0);
}

fn normal_at(wxz: vec2<f32>) -> vec3<f32> {
    let s = P.world.x;
    let hx0 = height_at(wxz - vec2<f32>(s, 0.0));
    let hx1 = height_at(wxz + vec2<f32>(s, 0.0));
    let hz0 = height_at(wxz - vec2<f32>(0.0, s));
    let hz1 = height_at(wxz + vec2<f32>(0.0, s));
    return normalize(vec3<f32>(hx0 - hx1, 2.0 * s, hz0 - hz1));
}

struct Hit {
    t: f32,
    hit: bool,
    steps: u32,
    canopy: f32,   // 0 = bare ground, >0 = fraction of canopy covering the hit
};

// Entry/exit parameters of the ray against the world's xz box.
fn clip_box(o: vec3<f32>, d: vec3<f32>, t_far: f32) -> vec2<f32> {
    let lo = vec2<f32>(0.0);
    let hi = vec2<f32>(P.world.w);
    var t0 = 0.0;
    var t1 = t_far;
    for (var a = 0; a < 2; a = a + 1) {
        let od = select(o.x, o.z, a == 1);
        let dd = select(d.x, d.z, a == 1);
        if (abs(dd) < 1e-8) {
            if (od < lo[a] || od > hi[a]) { return vec2<f32>(1.0, 0.0); }
        } else {
            let ta = (lo[a] - od) / dd;
            let tb = (hi[a] - od) / dd;
            t0 = max(t0, min(ta, tb));
            t1 = min(t1, max(ta, tb));
        }
    }
    // Ceiling: nothing above the global max, so a ray heading up out of the box is done.
    let ceil_y = P.bounds.y + 1.0;
    if (d.y > 1e-8) {
        t1 = min(t1, (ceil_y - o.y) / d.y);
    } else if (d.y < -1e-8) {
        t0 = max(t0, (ceil_y - o.y) / d.y);
    } else if (o.y > ceil_y) {
        return vec2<f32>(1.0, 0.0);
    }
    return vec2<f32>(t0, t1);
}

// ---- mode 0: fixed step -------------------------------------------------

fn march_fixed(o: vec3<f32>, d: vec3<f32>) -> Hit {
    var h: Hit;
    h.hit = false; h.steps = 0u; h.t = 0.0; h.canopy = 0.0;
    let span = clip_box(o, d, P.cfg.y);
    if (span.y <= span.x) { return h; }
    let dt = P.cfg.w;
    let max_steps = u32(P.cfg.x);
    var t = span.x;
    var prev_diff = o.y + d.y * t - height_at((o + d * t).xz);
    while (t < span.y && h.steps < max_steps) {
        h.steps = h.steps + 1u;
        t = t + dt;
        let p = o + d * t;
        let diff = p.y - height_at(p.xz);
        if (diff <= 0.0) {
            // Linear interpolation back to the crossing.
            let f = prev_diff / max(prev_diff - diff, 1e-6);
            h.t = t - dt * (1.0 - f);
            h.hit = true;
            return h;
        }
        prev_diff = diff;
    }
    return h;
}

// ---- modes 1..3: hierarchical max-mipmap --------------------------------

// The surface the pyramid was built over: terrain, plus the cover shell in modes 2/3.
fn surface_at(wxz: vec2<f32>) -> f32 {
    let terr = height_at(wxz);
    if (P.cfg.z < 1.5) { return terr; }
    let c = cover_at(wxz);
    var extra = 0.0;
    if (c.x > 0.02) { extra = max(extra, c.y * P.cover.z); }
    if (c.z > 0.047) { extra = max(extra, c.w * P.cover.w); }
    return terr + extra;
}

fn march_hier(o: vec3<f32>, d: vec3<f32>) -> Hit {
    var h: Hit;
    h.hit = false; h.steps = 0u; h.t = 0.0; h.canopy = 0.0;
    let span = clip_box(o, d, P.cfg.y);
    if (span.y <= span.x) { return h; }

    let grid = P.world.x;
    let n = i32(P.world.y);
    let top_level = i32(P.world.z) - 1;
    let max_steps = u32(P.cfg.x);

    var t = span.x + grid * 1e-3;
    var lvl = top_level;
    let inv_d = vec2<f32>(
        select(1.0 / d.x, 1e30, abs(d.x) < 1e-9),
        select(1.0 / d.z, 1e30, abs(d.z) < 1e-9));
    let step_sign = vec2<f32>(select(0.0, 1.0, d.x > 0.0), select(0.0, 1.0, d.z > 0.0));

    while (t < span.y && h.steps < max_steps) {
        h.steps = h.steps + 1u;
        let p = o + d * t;
        let cs = grid * f32(1 << u32(lvl));
        let cell = floor(p.xz / cs);
        let dim = n >> u32(lvl);
        let ci = vec2<i32>(clamp(cell, vec2<f32>(0.0), vec2<f32>(f32(dim - 1))));
        let cmax = textureLoad(top_tex, ci, lvl).x;

        // Exit parameter of this cell.
        let bound = (cell + step_sign) * cs;
        let tx = (bound - p.xz) * inv_d;
        let t_local = min(max(tx.x, 0.0), max(tx.y, 0.0));
        let t_exit = min(t + t_local, span.y);
        let y_exit = o.y + d.y * t_exit;

        if (min(p.y, y_exit) > cmax) {
            // Whole segment clears the cell: skip it and coarsen.
            t = t_exit + max(grid * 1e-3, t_exit * 1e-6);
            lvl = min(lvl + 1, top_level);
            continue;
        }
        if (lvl > 0) {
            lvl = lvl - 1;
            continue;
        }
        // Leaf cell that the ray dips into: bisect against the actual surface.
        var a = t;
        var b = t_exit;
        for (var i = 0; i < 12; i = i + 1) {
            let m = 0.5 * (a + b);
            let pm = o + d * m;
            let surf = surface_at(pm.xz);
            if (pm.y > surf) { a = m; } else { b = m; }
        }
        h.t = b;
        h.hit = true;
        return h;
    }
    return h;
}

// ---- shading ------------------------------------------------------------

fn sky_color(d: vec3<f32>) -> vec3<f32> {
    let k = clamp(d.y * 2.2 + 0.10, 0.0, 1.0);
    return mix(SKY_HORIZON, SKY_TOP, k);
}

fn shade(o: vec3<f32>, d: vec3<f32>, hit: Hit) -> vec3<f32> {
    if (!hit.hit) {
        return sky_color(d);
    }
    let p = o + d * hit.t;
    let terr = height_at(p.xz);
    let c = cover_at(p.xz);
    var nrm = normal_at(p.xz);
    if (P.bounds.w > 0.5) {
        nrm = normal_at_col(p.xz, p.y, d);
    }
    let lambert = clamp(dot(nrm, normalize(P.sun.xyz)), 0.0, 1.0);

    var albedo = vec3<f32>(0.36, 0.34, 0.26);
    if (terr < P.bounds.z + 0.5) {
        albedo = vec3<f32>(0.10, 0.17, 0.24);
    } else if (terr > 1050.0) {
        albedo = vec3<f32>(0.82, 0.83, 0.86);
    } else if (nrm.y < 0.72) {
        albedo = vec3<f32>(0.30, 0.28, 0.26);
    }
    // Above the bare terrain means the ray stopped on the cover shell.
    if (P.cfg.z >= 1.5 && p.y > terr + 0.6) {
        if (c.z > 0.047 && c.w * P.cover.w >= c.y * P.cover.z) {
            albedo = vec3<f32>(0.52, 0.48, 0.44);       // roofs
        } else {
            albedo = vec3<f32>(0.11, 0.24, 0.10);       // canopy
        }
    } else if (c.x > 0.15) {
        albedo = mix(albedo, vec3<f32>(0.13, 0.26, 0.11), clamp(c.x * 1.4, 0.0, 1.0));
    }

    var col = albedo * (0.30 + 0.85 * lambert);

    // Distance haze: the far field is nothing without it, and it is also what makes
    // a coarse representation acceptable at range.
    let fog = 1.0 - exp(-hit.t / 26000.0);
    col = mix(col, sky_color(d) * 1.02, clamp(fog, 0.0, 1.0));
    return col;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let w = u32(P.screen.x);
    let ht = u32(P.screen.y);
    if (gid.x >= w || gid.y >= ht) { return; }

    let uv = (vec2<f32>(f32(gid.x) + 0.5, f32(gid.y) + 0.5) / vec2<f32>(f32(w), f32(ht))) * 2.0 - 1.0;
    let aspect = f32(w) / f32(ht);
    let dir = normalize(P.fwd.xyz
        + P.right.xyz * (uv.x * aspect * P.cam.w)
        - P.up.xyz * (uv.y * P.cam.w));

    var hit: Hit;
    if (P.cfg.z < 0.5) {
        hit = march_fixed(P.cam.xyz, dir);
    } else {
        hit = march_hier(P.cam.xyz, dir);
    }

    var col = shade(P.cam.xyz, dir, hit);

    // Mode 3: soften the canopy edge by integrating density through the shell.
    if (P.cfg.z > 2.5 && hit.hit) {
        let p = P.cam.xyz + dir * hit.t;
        let terr = height_at(p.xz);
        if (p.y > terr + 0.6) {
            let c = cover_at(p.xz);
            let band = max(c.y * P.cover.z, 1.0);
            var trans = 1.0;
            var tt = hit.t;
            let dt = band / 8.0 / max(abs(dir.y), 0.12);
            for (var i = 0; i < 8; i = i + 1) {
                tt = tt + dt;
                let q = P.cam.xyz + dir * tt;
                let cq = cover_at(q.xz);
                let tq = height_at(q.xz);
                if (q.y < tq) { break; }
                let dens = cq.x * clamp((tq + cq.y * P.cover.z - q.y) / band, 0.0, 1.0);
                trans = trans * exp(-dens * dt * 0.09);
                hit.steps = hit.steps + 1u;
            }
            // What the ray sees past a thin canopy is the ground under it.
            var under = hit;
            under.t = tt;
            col = mix(shade(P.cam.xyz, dir, under), col, 1.0 - trans);
        }
    }

    textureStore(out_color, vec2<i32>(gid.xy), vec4<f32>(pow(col, vec3<f32>(1.0 / 2.2)), 1.0));
    textureStore(out_steps, vec2<i32>(gid.xy), vec4<u32>(hit.steps, 0u, 0u, 0u));
}
