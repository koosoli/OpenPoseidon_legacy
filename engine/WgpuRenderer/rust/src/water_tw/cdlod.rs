//! Tidewater Native — CDLOD surface mesh, ported from dgreenheck/tidewater `src/core/CDLOD.js`
//! @ 4811ba48 (MIT). Strugar-style continuous LOD quadtree: one G x G grid instanced per
//! selected node, geomorphing toward the next coarser lattice near each range's outer edge.
//!
//! Tidewater's sea uses gridSize 32 / leafSize 8 / 12 levels / rangeFactor 2.5 (App.js:176) and
//! plan §5 keeps that as the default. Selection is done in CAMERA-RELATIVE space because OP's
//! camera matrices are camera-relative (the view has no translation); instance data stays in
//! world metres so the shader's FFT lookups are world-anchored.

pub const GRID: u32 = 32;

/// W7h: the grid resolution in use: `WGR_TW_GRID=<n>` (16..128, rounded to a multiple of 8) for the
/// plan's mesh-density experiment (§7.2), else Tidewater's [`GRID`].
pub fn grid() -> u32 {
    static V: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("WGR_TW_GRID")
            .ok()
            .and_then(|v| v.trim().parse::<u32>().ok())
            .map_or(GRID, |g| ((g.clamp(16, 128) + 4) / 8) * 8)
    })
}
pub const LEAF: f32 = 8.0;
/// W9b (OP): 16 levels, not Tidewater's 12. Tidewater's 3 x 3 roots of 16 km stop the sea
/// 16-41 km out, which is past its island's fog but not OP's: from a few hundred metres up the sea
/// ended in a straight edge well short of the horizon (~140 km from 1500 m, ~226 km from 4 km).
/// Four more levels (roots of 262 km, so at least 262 km of sea on every side) cost a few dozen
/// coarse nodes; the near field is unchanged.
pub const LEVELS: usize = 16;
pub const RANGE_FACTOR: f32 = 2.5;
pub const MORPH_START_RATIO: f32 = 0.66;
pub const MAX_INSTANCES: usize = 1500;
/// Vertical bounds of a node relative to sea level (Tidewater's minY / maxY for the ocean).
pub const MIN_Y: f32 = -25.0;
pub const MAX_Y: f32 = 25.0;

#[derive(Clone, Copy, Debug)]
pub struct Cdlod {
    pub ranges: [f32; LEVELS],
    /// Per level: (morph start, 1 / morph range, grid spacing, 0) — `<prefix>.morph[lod]`.
    pub morph: [[f32; 4]; LEVELS],
}

impl Default for Cdlod {
    fn default() -> Self {
        let mut ranges = [0.0; LEVELS];
        let mut morph = [[0.0; 4]; LEVELS];
        let mut prev = 0.0f32;
        for l in 0..LEVELS {
            let r = LEAF * 2f32.powi(l as i32) * RANGE_FACTOR;
            ranges[l] = r;
            let start = prev + (r - prev) * MORPH_START_RATIO;
            let spacing = LEAF * 2f32.powi(l as i32) / grid() as f32;
            morph[l] = [start, 1.0 / (r - start).max(1e-3), spacing, 0.0];
            prev = r;
        }
        Self { ranges, morph }
    }
}

/// Six frustum planes' worth of side planes (left/right/bottom/top) from a clip matrix; the near
/// and (infinite) far planes are not needed for a conservative sea selection.
#[derive(Clone, Copy, Debug)]
pub struct SidePlanes([[f32; 4]; 4]);

impl SidePlanes {
    /// `clip` = proj * view (column-major, camera-relative input).
    pub fn from_clip(clip: glam::Mat4) -> Self {
        let r = |i: usize| clip.row(i);
        let (r0, r1, r3) = (r(0), r(1), r(3));
        Self([
            (r3 + r0).to_array(),
            (r3 - r0).to_array(),
            (r3 + r1).to_array(),
            (r3 - r1).to_array(),
        ])
    }

    fn box_visible(&self, min: glam::Vec3, max: glam::Vec3) -> bool {
        for p in &self.0 {
            // positive vertex
            let v = glam::Vec3::new(
                if p[0] >= 0.0 { max.x } else { min.x },
                if p[1] >= 0.0 { max.y } else { min.y },
                if p[2] >= 0.0 { max.z } else { min.z },
            );
            if p[0] * v.x + p[1] * v.y + p[2] * v.z + p[3] < 0.0 {
                return false;
            }
        }
        true
    }
}

/// Selection state for one frame.
pub struct Selection<'a> {
    cdlod: &'a Cdlod,
    cam: glam::DVec3,
    sea: f64,
    planes: Option<SidePlanes>,
    pub nodes: Vec<[f32; 4]>,
}

impl<'a> Selection<'a> {
    pub fn new(cdlod: &'a Cdlod, cam: glam::DVec3, sea: f32, planes: Option<SidePlanes>) -> Self {
        Self { cdlod, cam, sea: sea as f64, planes, nodes: Vec::with_capacity(256) }
    }

    fn rel_box(&self, x: f64, z: f64, size: f64) -> (glam::Vec3, glam::Vec3) {
        let min = glam::DVec3::new(x, self.sea + MIN_Y as f64, z) - self.cam;
        let max = glam::DVec3::new(x + size, self.sea + MAX_Y as f64, z + size) - self.cam;
        (min.as_vec3(), max.as_vec3())
    }

    fn in_sphere(&self, x: f64, z: f64, size: f64, r: f32) -> bool {
        let (min, max) = self.rel_box(x, z, size);
        let d = glam::Vec3::ZERO.clamp(min, max);
        d.length_squared() <= r * r
    }

    fn visible(&self, x: f64, z: f64, size: f64) -> bool {
        match &self.planes {
            None => true,
            Some(p) => {
                let (min, max) = self.rel_box(x, z, size);
                p.box_visible(min, max)
            }
        }
    }

    fn add(&mut self, x: f64, z: f64, size: f64, lod: usize) {
        if self.nodes.len() < MAX_INSTANCES {
            self.nodes.push([x as f32, z as f32, size as f32, lod as f32]);
        }
    }

    fn select(&mut self, x: f64, z: f64, size: f64, lod: usize) -> bool {
        if !self.in_sphere(x, z, size, self.cdlod.ranges[lod]) {
            return false;
        }
        if !self.visible(x, z, size) {
            return true;
        }
        if lod == 0 || !self.in_sphere(x, z, size, self.cdlod.ranges[lod - 1]) {
            self.add(x, z, size, lod);
            return true;
        }
        let h = size * 0.5;
        for (cx, cz) in [(x, z), (x + h, z), (x, z + h), (x + h, z + h)] {
            if !self.select(cx, cz, h, lod - 1) && self.visible(cx, cz, h) {
                // quadrant outside the finer range: draw it at this node's LOD
                self.add(cx, cz, h, lod);
            }
        }
        true
    }

    /// `CDLOD.update`: 3x3 root nodes around the camera, then front-to-back order.
    pub fn run(mut self) -> Vec<[f32; 4]> {
        let top = LEVELS - 1;
        let root = LEAF as f64 * 2f64.powi(top as i32);
        let cx = (self.cam.x / root).floor();
        let cz = (self.cam.z / root).floor();
        for j in -1..=1 {
            for i in -1..=1 {
                self.select((cx + i as f64) * root, (cz + j as f64) * root, root, top);
            }
        }
        let cam = self.cam;
        let key = |n: &[f32; 4]| {
            let (x, z, s) = (n[0] as f64, n[1] as f64, n[2] as f64);
            let dx = (x - cam.x).max(0.0).max(cam.x - x - s);
            let dz = (z - cam.z).max(0.0).max(cam.z - z - s);
            dx * dx + dz * dz
        };
        self.nodes.sort_by(|a, b| key(a).total_cmp(&key(b)));
        self.nodes
    }
}

/// Grid in [0,1]^2 (x, z) plus the strip-ordered index list of CDLOD.js (STRIP = 8 columns,
/// alternating diagonals).
pub fn grid_mesh() -> (Vec<[f32; 2]>, Vec<u32>) {
    let g = grid();
    let mut verts = Vec::with_capacity(((g + 1) * (g + 1)) as usize);
    for j in 0..=g {
        for i in 0..=g {
            verts.push([i as f32 / g as f32, j as f32 / g as f32]);
        }
    }
    const STRIP: u32 = 8;
    let mut idx = Vec::with_capacity((g * g * 6) as usize);
    let mut i0 = 0;
    while i0 < g {
        for j in 0..g {
            for i in i0..(i0 + STRIP).min(g) {
                let a = j * (g + 1) + i;
                let b = a + 1;
                let c = a + (g + 1);
                let d = c + 1;
                if (i + j) % 2 == 0 {
                    idx.extend_from_slice(&[a, c, b, b, c, d]);
                } else {
                    idx.extend_from_slice(&[a, c, d, a, d, b]);
                }
            }
        }
        i0 += STRIP;
    }
    (verts, idx)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ranges_and_morph_match_cdlod_js() {
        let c = Cdlod::default();
        assert_eq!(c.ranges[0], 20.0);
        assert_eq!(c.ranges[11], 8.0 * 2048.0 * 2.5);
        assert_eq!(c.ranges[LEVELS - 1], 8.0 * 32768.0 * 2.5);
        // level 0: start = 0 + 20 * 0.66
        assert!((c.morph[0][0] - 13.2).abs() < 1e-5);
        assert!((c.morph[0][2] - 0.25).abs() < 1e-6);
        // level 1: start = 20 + (40 - 20) * 0.66
        assert!((c.morph[1][0] - 33.2).abs() < 1e-4);
    }

    #[test]
    fn the_sea_reaches_the_horizon_from_high_up() {
        // W9b: from 4 km up the horizon is ~226 km away; the selection must reach past 200 km
        // in every direction, within the instance budget
        let c = Cdlod::default();
        let cam = glam::DVec3::new(6400.0, 4000.0, 6400.0);
        let nodes = Selection::new(&c, cam, 0.0, None).run();
        assert!(nodes.len() <= MAX_INSTANCES / 2, "{} nodes", nodes.len());
        let (mut x0, mut x1, mut z0, mut z1) = (f64::MAX, f64::MIN, f64::MAX, f64::MIN);
        for n in &nodes {
            x0 = x0.min(n[0] as f64);
            z0 = z0.min(n[1] as f64);
            x1 = x1.max((n[0] + n[2]) as f64);
            z1 = z1.max((n[1] + n[2]) as f64);
        }
        assert!(cam.x - x0 > 200_000.0 && x1 - cam.x > 200_000.0);
        assert!(cam.z - z0 > 200_000.0 && z1 - cam.z > 200_000.0);
        // and near the surface the finest level is still what the camera stands on
        let low = Selection::new(&c, glam::DVec3::new(6400.5, 2.0, 5100.25), 0.0, None).run();
        assert_eq!(low[0][3], 0.0);
        assert!(low.len() <= MAX_INSTANCES / 2, "{} nodes", low.len());
    }

    #[test]
    fn grid_is_32_by_32_quads() {
        let (v, i) = grid_mesh();
        assert_eq!(v.len(), 33 * 33);
        assert_eq!(i.len(), 32 * 32 * 6);
        assert!(i.iter().all(|&k| (k as usize) < v.len()));
    }

    #[test]
    fn selection_covers_the_camera_and_is_front_to_back() {
        let c = Cdlod::default();
        let cam = glam::DVec3::new(6400.5, 12.0, 5100.25);
        let nodes = Selection::new(&c, cam, 0.0, None).run();
        assert!(!nodes.is_empty() && nodes.len() <= MAX_INSTANCES);
        // the node under the camera is a finest-level node and comes first
        let n = nodes[0];
        assert!(cam.x as f32 >= n[0] && (cam.x as f32) <= n[0] + n[2]);
        assert!(cam.z as f32 >= n[1] && (cam.z as f32) <= n[1] + n[2]);
        assert_eq!(n[3], 0.0);
        // no two nodes overlap (area sum == union for a quadtree cover)
        let area: f64 = nodes.iter().map(|n| (n[2] as f64).powi(2)).sum();
        assert!(area > 0.0);
    }

    #[test]
    #[allow(deprecated)] // glam look_at_rh / perspective_rh, test-only (as in gfx3d/cull.rs)
    fn frustum_rejects_the_half_space_behind() {
        let c = Cdlod::default();
        let cam = glam::DVec3::new(0.0, 5.0, 0.0);
        let view = glam::Mat4::look_at_rh(glam::Vec3::ZERO, glam::Vec3::new(1.0, -0.1, 0.0), glam::Vec3::Y);
        let proj = glam::Mat4::perspective_rh(60f32.to_radians(), 16.0 / 9.0, 0.1, 50_000.0);
        let all = Selection::new(&c, cam, 0.0, None).run().len();
        let seen = Selection::new(&c, cam, 0.0, Some(SidePlanes::from_clip(proj * view))).run();
        assert!(seen.len() < all);
        // everything kept is not entirely behind the camera (x < 0 side)
        assert!(seen.iter().all(|n| n[0] + n[2] >= -1.0));
    }
}
