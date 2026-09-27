//! Tidewater's ShoreField.js — the offline wave-propagation field for the shoreline waves —
//! ported from dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software Solutions LLC).
//!
//! Solves the Eikonal equation |grad T| = 1 / c(x) with the Fast Marching Method, where
//! c = sqrt(g * depth) is the shallow-water wave speed (capped offshore). Sources are the domain
//! borders, initialised with a plane wave travelling along the swell direction, so wave fronts
//! refract around headlands and align with the depth contours near the beach.
//!
//! Output per cell: T (s), the propagation direction scaled by the exposure, and the arrival time
//! at the nearest shoreline (extended onto land for the swash timing). The algorithm, the
//! constants and the channel meanings are Tidewater's. What OP changes:
//!
//! * **Domain.** Tidewater solves its one 2 km island at 512² (4 m cells). OP solves the whole
//!   world heightmap in one field, so there are no region seams to keep in phase (plan §8's
//!   phase-continuity test holds by construction). The cell is 4 m, coarsened only as far as
//!   needed to stay within `MAX_RES` cells a side (6.25 m on a 12.8 km world).
//! * **Terrain.** Heights come from OP's heightmap with the same triangulation the terrain draws
//!   (`height_at`), sampled at cell centres as Tidewater's `terrain.heightAt` is.
//! * **Precision.** The plane-wave initial value is taken relative to the domain centre (Tidewater's
//!   domain is centred on the origin; OP's world is not), and offset by the smallest constant that
//!   keeps it positive (half the diagonal / c0 + 1 s) instead of Tidewater's `+ size` (2048 s
//!   there, 12 800 s on a 12.8 km world): T stays small enough for f32. A constant offset only
//!   shifts the phase of every wave alike.
//! * **When.** Computed on a worker thread when the world loads, and again when the terrain, the
//!   sea level (by 0.25 m) or the swell heading (by 5°) move — never on the render thread. The
//!   thresholds are measured from the field in use (or being computed), not rounded, so a value
//!   hovering at a boundary cannot start one solve after another.
//!   The previous field stays in use until the new one is ready. Tidewater computes it once at
//!   start-up; it has one sea level and one swell direction.

use std::sync::{mpsc, Arc};

/// Tidewater's `GRAVITY` (Frame.js).
pub const GRAVITY: f32 = 9.81;
/// Tidewater's default cell size: 2048 m / 512.
pub const BASE_CELL: f32 = 4.0;
/// Largest field side in cells (memory: rg32float + rgba8snorm = 12 bytes per cell on the GPU).
pub const MAX_RES: u32 = 2048;
/// Smoothed sea depth (W3l, OP): the depth the shore waves decide their breaking from, blurred
/// over ~20 m. OP's heightmaps are 50 m triangle grids, whose depth contours are straight segments
/// with kinks at the grid lines; Tidewater's breaking switches sharply across a contour, so the
/// surf zone's foam ended along those polygons. Stored in the direction texture's z, / this range.
pub const SMOOTH_DEPTH_RANGE: f32 = 32.0;
/// Blur radius (m) of one of the three box passes (~0.6 x it is the resulting gaussian sigma x 2).
pub const SMOOTH_DEPTH_RADIUS: f32 = 18.0;

/// Three separable box passes (≈ gaussian) over a row-major rx x rz grid, radius r cells.
pub fn blur3(v: &mut [f32], rx: usize, rz: usize, r: usize) {
    if r == 0 || rx == 0 || rz == 0 {
        return;
    }
    let mut tmp = vec![0.0f32; v.len()];
    for _ in 0..3 {
        for j in 0..rz {
            let row = &v[j * rx..(j + 1) * rx];
            let mut acc = 0.0f32;
            for i in 0..=r.min(rx - 1) {
                acc += row[i];
            }
            let mut cnt = (r.min(rx - 1) + 1) as f32;
            for i in 0..rx {
                tmp[j * rx + i] = acc / cnt;
                if i + r + 1 < rx {
                    acc += row[i + r + 1];
                    cnt += 1.0;
                }
                if i >= r {
                    acc -= row[i - r];
                    cnt -= 1.0;
                }
            }
        }
        for i in 0..rx {
            let mut acc = 0.0f32;
            for j in 0..=r.min(rz - 1) {
                acc += tmp[j * rx + i];
            }
            let mut cnt = (r.min(rz - 1) + 1) as f32;
            for j in 0..rz {
                v[j * rx + i] = acc / cnt;
                if j + r + 1 < rz {
                    acc += tmp[(j + r + 1) * rx + i];
                    cnt += 1.0;
                }
                if j >= r {
                    acc -= tmp[(j - r) * rx + i];
                    cnt -= 1.0;
                }
            }
        }
    }
}

/// Side of the blocks the coast is indexed in for placing shore-simulation regions (half a region).
pub const COAST_BLOCK: f32 = 190.0;
/// A block is coast when at least this many field cells in it are surf-zone water.
const COAST_MIN_CELLS: u32 = 12;

/// What the field is computed from.
#[derive(Clone)]
pub struct ShoreFieldInput {
    pub heights: Arc<[f32]>,
    pub hm_width: u32,
    pub hm_height: u32,
    pub origin: [f32; 2],
    pub grid: f32,
    pub sea_level: f32,
    /// unit vector (x, z) the swell travels toward
    pub swell_dir: [f32; 2],
    /// identity of the heights upload (a new upload is a new generation)
    pub terrain_gen: u64,
}

/// What a field was computed from, for deciding when to recompute.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FieldStamp {
    pub terrain_gen: u64,
    pub sea_level: f32,
    pub heading_deg: f32,
}

impl FieldStamp {
    /// Far enough from `other` to be worth a new solve (see the module doc).
    pub fn differs(&self, other: &FieldStamp) -> bool {
        let d = (self.heading_deg - other.heading_deg).rem_euclid(360.0);
        let dh = d.min(360.0 - d);
        self.terrain_gen != other.terrain_gen || (self.sea_level - other.sea_level).abs() >= 0.25 || dh >= 5.0
    }
}

impl ShoreFieldInput {
    pub fn stamp(&self) -> FieldStamp {
        FieldStamp {
            terrain_gen: self.terrain_gen,
            sea_level: self.sea_level,
            heading_deg: self.swell_dir[1].atan2(self.swell_dir[0]).to_degrees(),
        }
    }
}

/// The computed field, in the GPU layout.
pub struct ShoreField {
    pub res_x: u32,
    pub res_z: u32,
    pub cell: f32,
    pub origin: [f32; 2],
    /// (T, shoreline T) per cell, row-major (z rows)
    pub times: Vec<[f32; 2]>,
    /// (dirX * exposure, dirZ * exposure, smoothed depth / SMOOTH_DEPTH_RANGE, unused) as snorm8
    pub dirs: Vec<[i8; 4]>,
    pub stamp: FieldStamp,
    /// Centres (world xz) of the COAST_BLOCK blocks that hold a surf zone: where shore-simulation
    /// regions may be placed (OP: Tidewater has one fixed region over its beach).
    pub coast: Vec<[f32; 2]>,
}

impl ShoreField {
    pub fn centre(&self) -> [f32; 2] {
        [
            self.origin[0] + self.res_x as f32 * self.cell * 0.5,
            self.origin[1] + self.res_z as f32 * self.cell * 0.5,
        ]
    }
}

/// OP's terrain triangulation (terrain.wgsl sample_height / tw_water.wgsl terrainHeightAt).
pub fn height_at(inp: &ShoreFieldInput, x: f32, z: f32) -> f32 {
    let (w, h) = (inp.hm_width as i32, inp.hm_height as i32);
    let tx = (x - inp.origin[0]) / inp.grid.max(1e-4);
    let tz = (z - inp.origin[1]) / inp.grid.max(1e-4);
    let (bx, bz) = (tx.floor(), tz.floor());
    let (fx, fz) = (tx - bx, tz - bz);
    let load = |ix: i32, iz: i32| {
        let cx = ix.clamp(0, w - 1) as usize;
        let cz = iz.clamp(0, h - 1) as usize;
        inp.heights[cz * w as usize + cx]
    };
    let (ix, iz) = (bx as i32, bz as i32);
    let y00 = load(ix, iz);
    let y01 = load(ix + 1, iz);
    let y10 = load(ix, iz + 1);
    let y11 = load(ix + 1, iz + 1);
    if fx <= 1.0 - fz {
        y00 + (y10 - y00) * fz + (y01 - y00) * fx
    } else {
        y10 + (y01 - y11) - (y10 - y11) * fx - (y01 - y11) * fz
    }
}

/// Tidewater's MinHeap (Float64 keys, Int32 values), growable.
struct MinHeap {
    keys: Vec<f64>,
    vals: Vec<i32>,
}

impl MinHeap {
    fn with_capacity(cap: usize) -> Self {
        Self { keys: Vec::with_capacity(cap), vals: Vec::with_capacity(cap) }
    }

    fn len(&self) -> usize {
        self.keys.len()
    }

    fn push(&mut self, k: f64, v: i32) {
        self.keys.push(k);
        self.vals.push(v);
        let mut i = self.keys.len() - 1;
        while i > 0 {
            let p = (i - 1) >> 1;
            if self.keys[p] <= k {
                break;
            }
            self.keys[i] = self.keys[p];
            self.vals[i] = self.vals[p];
            i = p;
        }
        self.keys[i] = k;
        self.vals[i] = v;
    }

    fn pop(&mut self) -> i32 {
        let top = self.vals[0];
        let k = self.keys.pop().unwrap();
        let v = self.vals.pop().unwrap();
        let n = self.keys.len();
        if n == 0 {
            return top;
        }
        let mut i = 0;
        loop {
            let mut c = 2 * i + 1;
            if c >= n {
                break;
            }
            if c + 1 < n && self.keys[c + 1] < self.keys[c] {
                c += 1;
            }
            if self.keys[c] >= k {
                break;
            }
            self.keys[i] = self.keys[c];
            self.vals[i] = self.vals[c];
            i = c;
        }
        self.keys[i] = k;
        self.vals[i] = v;
        top
    }
}

/// Field resolution for a world: Tidewater's 4 m cells, coarsened to fit `MAX_RES`.
pub fn layout(inp: &ShoreFieldInput) -> (u32, u32, f32) {
    let size_x = (inp.hm_width.saturating_sub(1)) as f32 * inp.grid;
    let size_z = (inp.hm_height.saturating_sub(1)) as f32 * inp.grid;
    let cell = BASE_CELL.max(size_x.max(size_z) / MAX_RES as f32);
    let rx = ((size_x / cell).ceil() as u32).clamp(1, MAX_RES);
    let rz = ((size_z / cell).ceil() as u32).clamp(1, MAX_RES);
    (rx, rz, cell)
}

/// computeShoreField(terrain, { swellDir, seaLevel, maxDepth = 25, minDepth = 0.25 }).
pub fn compute(inp: &ShoreFieldInput) -> ShoreField {
    let max_depth = 25.0f32;
    let min_depth = 0.25f32;
    let (rx, rz, h) = layout(inp);
    let (rxu, rzu) = (rx as usize, rz as usize);
    let n = rxu * rzu;
    let ox = inp.origin[0];
    let oz = inp.origin[1];
    let size = rx.max(rz) as f32 * h;
    // (Tidewater: + size; see the module doc)
    let t_offset = 0.5 * size * std::f32::consts::SQRT_2 / (GRAVITY * 25.0f32).sqrt() + 1.0;
    // plane-wave phase relative to the domain centre (see the module doc)
    let cx = ox + rx as f32 * h * 0.5;
    let cz = oz + rz as f32 * h * 0.5;

    let mut speed = vec![0.0f32; n];
    for j in 0..rzu {
        let z = oz + (j as f32 + 0.5) * h;
        for i in 0..rxu {
            let x = ox + (i as f32 + 0.5) * h;
            let d = inp.sea_level - height_at(inp, x, z);
            speed[j * rxu + i] = if d > 0.0 { (GRAVITY * d.clamp(min_depth, max_depth)).sqrt() } else { 0.0 };
        }
    }

    let inf = f32::INFINITY;
    let mut t = vec![inf; n];
    let mut state = vec![0u8; n]; // 0 far, 1 trial, 2 known
    let mut heap = MinHeap::with_capacity(n / 4 + 16);
    let (sdx, sdz) = (inp.swell_dir[0], inp.swell_dir[1]);
    let c0 = (GRAVITY * max_depth).sqrt();

    // plane-wave initial condition on the border water cells
    for j in 0..rzu {
        for i in 0..rxu {
            if i != 0 && j != 0 && i != rxu - 1 && j != rzu - 1 {
                continue;
            }
            let k = j * rxu + i;
            if speed[k] <= 0.0 {
                continue;
            }
            let x = ox + (i as f32 + 0.5) * h - cx;
            let z = oz + (j as f32 + 0.5) * h - cz;
            t[k] = (x * sdx + z * sdz) / c0 + t_offset; // offset keeps T positive
            state[k] = 1;
            heap.push(t[k] as f64, k as i32);
        }
    }

    let solve = |t: &[f32], state: &[u8], i: usize, j: usize| -> f32 {
        let k = j * rxu + i;
        let c = speed[k];
        if c <= 0.0 {
            return inf;
        }
        let f = h / c;
        let known = |kk: usize| if state[kk] == 2 { t[kk] } else { inf };
        let tx = (if i > 0 { known(k - 1) } else { inf }).min(if i < rxu - 1 { known(k + 1) } else { inf });
        let tz = (if j > 0 { known(k - rxu) } else { inf }).min(if j < rzu - 1 { known(k + rxu) } else { inf });
        let a = tx.min(tz);
        let b = tx.max(tz);
        if !b.is_finite() || b - a >= f {
            return a + f;
        }
        0.5 * (a + b + (2.0 * f * f - (a - b) * (a - b)).sqrt())
    };

    while heap.len() > 0 {
        let k = heap.pop() as usize;
        if state[k] == 2 {
            continue;
        }
        state[k] = 2;
        let i = k % rxu;
        let j = k / rxu;
        let nb = [
            (i as i64 - 1, j as i64),
            (i as i64 + 1, j as i64),
            (i as i64, j as i64 - 1),
            (i as i64, j as i64 + 1),
        ];
        for (ni, nj) in nb {
            if ni < 0 || nj < 0 || ni >= rxu as i64 || nj >= rzu as i64 {
                continue;
            }
            let (ni, nj) = (ni as usize, nj as usize);
            let nk = nj * rxu + ni;
            if state[nk] == 2 || speed[nk] <= 0.0 {
                continue;
            }
            let tn = solve(&t, &state, ni, nj);
            if tn < t[nk] {
                t[nk] = tn;
                state[nk] = 1;
                heap.push(tn as f64, nk as i32);
            }
        }
    }
    drop(state);

    // Coast index for the shore-simulation regions: surf-zone water (up to 2 m deep, the band the
    // simulation acts on most; `speed` clamps depths under 0.25 m), counted per COAST_BLOCK block.
    let coast = {
        let bx = ((rx as f32 * h) / COAST_BLOCK).ceil().max(1.0) as usize;
        let bz = ((rz as f32 * h) / COAST_BLOCK).ceil().max(1.0) as usize;
        let mut count = vec![0u32; bx * bz];
        let (c_lo, c_hi) = ((GRAVITY * 0.1).sqrt(), (GRAVITY * 2.0).sqrt());
        for j in 0..rzu {
            for i in 0..rxu {
                let c = speed[j * rxu + i];
                if c >= c_lo && c <= c_hi {
                    let bi = (((i as f32 + 0.5) * h) / COAST_BLOCK) as usize;
                    let bj = (((j as f32 + 0.5) * h) / COAST_BLOCK) as usize;
                    count[bj.min(bz - 1) * bx + bi.min(bx - 1)] += 1;
                }
            }
        }
        let mut out = Vec::new();
        for bj in 0..bz {
            for bi in 0..bx {
                if count[bj * bx + bi] >= COAST_MIN_CELLS {
                    out.push([ox + (bi as f32 + 0.5) * COAST_BLOCK, oz + (bj as f32 + 0.5) * COAST_BLOCK]);
                }
            }
        }
        out
    };
    drop(speed);

    // Extend a field onto land one ring of cells per pass (average of the known neighbours +
    // inc). Each pass reads the previous pass only.
    let extend = |f: &mut Vec<f32>, passes: usize, inc: f32| {
        let mut prev = vec![0.0f32; n];
        for _ in 0..passes {
            prev.copy_from_slice(f);
            let mut changed = false;
            for j in 0..rzu {
                for i in 0..rxu {
                    let k = j * rxu + i;
                    if prev[k].is_finite() {
                        continue;
                    }
                    let (mut s, mut cnt) = (0.0f32, 0u32);
                    if i > 0 && prev[k - 1].is_finite() { s += prev[k - 1]; cnt += 1; }
                    if i < rxu - 1 && prev[k + 1].is_finite() { s += prev[k + 1]; cnt += 1; }
                    if j > 0 && prev[k - rxu].is_finite() { s += prev[k - rxu]; cnt += 1; }
                    if j < rzu - 1 && prev[k + rxu].is_finite() { s += prev[k + rxu]; cnt += 1; }
                    if cnt > 0 {
                        f[k] = s / cnt as f32 + inc;
                        changed = true;
                    }
                }
            }
            if !changed {
                break;
            }
        }
    };

    // arrival time at the nearest shoreline, extended unchanged onto land (swash timing)
    let mut t_shore = t.clone();
    extend(&mut t_shore, 40, 0.0);

    // extend T onto land (so the swash zone has a continuous phase), continuing slowly up the beach
    let mut t_filled = t;
    extend(&mut t_filled, 24, h / 1.5);

    // smooth to remove first-order FMM kinks (keeps phase monotonic)
    let mut ts = t_filled;
    for _ in 0..3 {
        let mut out = vec![0.0f32; n];
        for j in 0..rzu {
            for i in 0..rxu {
                let k = j * rxu + i;
                if !ts[k].is_finite() {
                    out[k] = ts[k];
                    continue;
                }
                let mut s = ts[k] * 4.0;
                let mut w = 4.0f32;
                for (di, dj) in [(-1i64, 0i64), (1, 0), (0, -1), (0, 1)] {
                    let ni = i as i64 + di;
                    let nj = j as i64 + dj;
                    if ni < 0 || nj < 0 || ni >= rxu as i64 || nj >= rzu as i64 {
                        continue;
                    }
                    let v = ts[nj as usize * rxu + ni as usize];
                    if v.is_finite() {
                        s += v;
                        w += 1.0;
                    }
                }
                out[k] = s / w;
            }
        }
        ts = out;
    }

    // (W3l) smoothed depth, land counted as 4 m above the sea so the blur does not pull the
    // shoreline far out
    let mut depth_s = vec![0.0f32; n];
    for j in 0..rzu {
        let z = oz + (j as f32 + 0.5) * h;
        for i in 0..rxu {
            let x = ox + (i as f32 + 0.5) * h;
            depth_s[j * rxu + i] = (inp.sea_level - height_at(inp, x, z)).clamp(-4.0, SMOOTH_DEPTH_RANGE);
        }
    }
    blur3(&mut depth_s, rxu, rzu, (SMOOTH_DEPTH_RADIUS / h).round() as usize);

    // directions + exposure
    let mut times = vec![[0.0f32; 2]; n];
    let mut dirs = vec![[0i8; 4]; n];
    let sl = (sdx * sdx + sdz * sdz).sqrt().max(1e-6);
    let snorm = |v: f32| (v.clamp(-1.0, 1.0) * 127.0).round() as i8;
    for j in 0..rzu {
        for i in 0..rxu {
            let k = j * rxu + i;
            let tt = ts[k];
            let g = |a: f32, b: f32| if a.is_finite() && b.is_finite() { a - b } else { 0.0 };
            let tl = if i > 0 { ts[k - 1] } else { tt };
            let tr = if i < rxu - 1 { ts[k + 1] } else { tt };
            let td = if j > 0 { ts[k - rxu] } else { tt };
            let tu = if j < rzu - 1 { ts[k + rxu] } else { tt };
            let mut gx = g(tr, tl);
            let mut gz = g(tu, td);
            if gx == 0.0 && tr.is_finite() && tt.is_finite() {
                gx = tr - tt;
            }
            if gz == 0.0 && tu.is_finite() && tt.is_finite() {
                gz = tu - tt;
            }
            let len = {
                let l = (gx * gx + gz * gz).sqrt();
                if l > 0.0 { l } else { 1.0 }
            };
            let (dx, dz) = (gx / len, gz / len);
            // exposure: how directly the local wave direction faces the incoming swell
            let align = (dx * sdx + dz * sdz) / sl;
            let exposure = (align * 1.4 + 0.1).clamp(0.02, 1.0);
            times[k] = [
                if tt.is_finite() { tt } else { 1e5 },
                if t_shore[k].is_finite() { t_shore[k] } else if tt.is_finite() { tt } else { 1e5 },
            ];
            dirs[k] = [snorm(dx * exposure), snorm(dz * exposure), snorm(depth_s[k] / SMOOTH_DEPTH_RANGE), 0];
        }
    }

    ShoreField { res_x: rx, res_z: rz, cell: h, origin: [ox, oz], times, dirs, stamp: inp.stamp(), coast }
}

/// One background computation at a time; the newest request wins.
#[derive(Default)]
pub struct ShoreFieldWorker {
    rx: Option<mpsc::Receiver<ShoreField>>,
    running: Option<FieldStamp>,
    queued: Option<ShoreFieldInput>,
    failed: Option<FieldStamp>,
}

impl ShoreFieldWorker {
    /// Ask for the field of `inp` unless the field in use (`installed`) or the one being
    /// computed is already close enough to it, or a solve for these inputs has failed.
    pub fn request(&mut self, inp: ShoreFieldInput, installed: Option<FieldStamp>) {
        let st = inp.stamp();
        if let Some(r) = self.running.or(installed) {
            if !st.differs(&r) {
                if self.running.is_some() {
                    self.queued = None;
                }
                return;
            }
        }
        if let Some(f) = self.failed {
            if !st.differs(&f) {
                return;
            }
        }
        if self.rx.is_some() {
            self.queued = Some(inp);
            return;
        }
        self.start(inp);
    }

    fn start(&mut self, inp: ShoreFieldInput) {
        let (tx, rx) = mpsc::channel();
        let st = inp.stamp();
        let spawned = std::thread::Builder::new()
            .name("tw-shore-field".into())
            .spawn(move || {
                // a closed receiver (backend switched away) just drops the result
                let _ = tx.send(compute(&inp));
            });
        match spawned {
            Ok(_) => {
                self.running = Some(st);
                self.rx = Some(rx);
            }
            Err(_) => self.failed = Some(st),
        }
    }

    /// A finished field, if one arrived since the last poll.
    pub fn poll(&mut self) -> Option<ShoreField> {
        let rx = self.rx.as_ref()?;
        let got = match rx.try_recv() {
            Ok(field) => Some(field),
            Err(mpsc::TryRecvError::Empty) => return None,
            // the solve panicked: do not start the same one again
            Err(mpsc::TryRecvError::Disconnected) => {
                self.failed = self.running;
                None
            }
        };
        self.rx = None;
        self.running = None;
        if let Some(next) = self.queued.take() {
            self.start(next);
        }
        got
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A planar beach rising along +z: sea at z < 400, land beyond.
    fn beach(swell: [f32; 2]) -> ShoreFieldInput {
        let (w, h) = (129u32, 129u32);
        let grid = 5.0;
        let mut heights = vec![0.0f32; (w * h) as usize];
        for j in 0..h {
            for i in 0..w {
                let z = j as f32 * grid;
                heights[(j * w + i) as usize] = (z - 400.0) * 0.05;
            }
        }
        ShoreFieldInput {
            heights: heights.into(),
            hm_width: w,
            hm_height: h,
            origin: [1000.0, 2000.0],
            grid,
            sea_level: 0.0,
            swell_dir: swell,
            terrain_gen: 1,
        }
    }

    #[test]
    fn blur_keeps_a_constant_and_rounds_a_step() {
        let (rx, rz) = (40, 30);
        let mut v = vec![5.0f32; rx * rz];
        blur3(&mut v, rx, rz, 3);
        assert!(v.iter().all(|x| (x - 5.0).abs() < 1e-4));
        // a step at i = 20 becomes a ramp, monotonic, symmetric about the step
        let mut s: Vec<f32> = (0..rx * rz).map(|k| if k % rx < 20 { 0.0 } else { 10.0 }).collect();
        blur3(&mut s, rx, rz, 3);
        let row = &s[15 * rx..16 * rx];
        assert!(row.windows(2).all(|w| w[1] >= w[0] - 1e-4));
        assert!(row[17] > 0.2 && row[17] < 5.0 && row[22] > 5.0 && row[22] < 9.8, "{row:?}");
    }

    #[test]
    fn smoothed_depth_is_in_the_direction_texture() {
        let f = compute(&beach([0.0, 1.0]));
        let deep = f.dirs.iter().map(|d| d[2]).max().unwrap();
        let land = f.dirs.iter().map(|d| d[2]).min().unwrap();
        assert!(deep > 0 && land < 0, "{deep} {land}");
    }

    #[test]
    fn heap_pops_in_order() {
        let mut hp = MinHeap::with_capacity(4);
        for (k, v) in [(5.0, 5), (1.0, 1), (3.0, 3), (2.0, 2), (4.0, 4)] {
            hp.push(k, v);
        }
        let got: Vec<i32> = (0..5).map(|_| hp.pop()).collect();
        assert_eq!(got, vec![1, 2, 3, 4, 5]);
        assert_eq!(hp.len(), 0);
    }

    #[test]
    fn layout_is_tidewaters_4m_until_it_must_coarsen() {
        let inp = beach([0.0, 1.0]);
        let (rx, rz, cell) = layout(&inp);
        assert_eq!(cell, 4.0);
        assert_eq!((rx, rz), (160, 160));
        let big = ShoreFieldInput { hm_width: 1025, hm_height: 1025, grid: 12.5, ..inp };
        let (rx, _, cell) = layout(&big);
        assert_eq!(rx, MAX_RES);
        assert!((cell - 6.25).abs() < 1e-4);
    }

    #[test]
    fn waves_run_up_the_beach_and_face_it() {
        // swell toward +z, straight onto the beach
        let f = compute(&beach([0.0, 1.0]));
        let (rx, rz) = (f.res_x as usize, f.res_z as usize);
        let i = rx / 2;
        // T grows shoreward in the water
        let t_at = |j: usize| f.times[j * rx + i][0];
        assert!(t_at(10) < t_at(40) && t_at(40) < t_at(90));
        // direction points up the beach with full exposure
        let d = f.dirs[60 * rx + i];
        assert!(d[1] > 100, "dirZ*exposure {:?}", d);
        assert!(d[0].abs() < 10);
        // the beach is indexed for the shore simulation: blocks along z ~ 400 m (the shoreline)
        assert!(!f.coast.is_empty());
        for c in &f.coast {
            assert!((c[1] - 2000.0 - 380.0).abs() <= COAST_BLOCK, "{:?}", c);
        }
        // land is reached by the extension: finite everywhere a few cells inland
        let land = (400.0 + 20.0) as usize / 4; // 20 m inland
        assert!(f.times[land * rx + i][0] < 1e5);
        assert!(f.times[land * rx + i][1] < 1e5);
        let _ = rz;
    }

    #[test]
    fn same_input_same_field() {
        let a = compute(&beach([0.3, 0.95]));
        let b = compute(&beach([0.3, 0.95]));
        assert_eq!(a.times, b.times);
        assert_eq!(a.dirs, b.dirs);
        assert_eq!(a.stamp, b.stamp);
    }

    #[test]
    fn small_moves_do_not_recompute_and_the_heading_wraps() {
        let st = |g: u64, sea: f32, hd: f32| FieldStamp { terrain_gen: g, sea_level: sea, heading_deg: hd };
        let a = st(1, 0.0, 179.0);
        assert!(!st(1, 0.24, 179.0).differs(&a));
        assert!(st(1, 0.25, 179.0).differs(&a));
        assert!(!st(1, 0.0, -178.0).differs(&a)); // 3° across the wrap
        assert!(st(1, 0.0, 185.0).differs(&a));
        assert!(st(2, 0.0, 179.0).differs(&a));
        // hovering around a rounding boundary never looks like a move
        assert!(!st(1, 0.126, 179.0).differs(&st(1, 0.124, 179.0)));
    }

    #[test]
    fn height_matches_the_terrain_triangulation_at_vertices() {
        let inp = beach([0.0, 1.0]);
        assert!((height_at(&inp, 1000.0, 2000.0 + 400.0) - 0.0).abs() < 1e-5);
        assert!((height_at(&inp, 1000.0, 2000.0 + 500.0) - 5.0).abs() < 1e-5);
    }
}
