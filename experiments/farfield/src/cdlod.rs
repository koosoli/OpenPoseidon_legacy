// CDLOD quadtree selection, ported from the engine's own
// engine/Poseidon/World/Terrain/TerrainCdlod.hpp so the raster arm of the
// comparison measures the SAME algorithm the renderer already ships, not a
// idealised reimplementation.
//
// Also hosts EXP-VEG-001, which is arithmetic rather than a GPU experiment: how
// far does clustering actually take a multi-million-tree source dataset?

use crate::world::{World, COVER_SHIFT};

#[derive(Clone, Copy)]
pub struct Node {
    pub origin: [f32; 2],
    pub size: f32,
    pub min_y: f32,
    pub max_y: f32,
    pub child: [i32; 4],
}

#[derive(Clone, Copy)]
pub struct Selection {
    pub origin: [f32; 2],
    pub size: f32,
    pub level: u32,
    pub morph_start: f32,
    pub morph_end: f32,
}

pub struct Tree {
    pub nodes: Vec<Node>,
    pub root: i32,
    pub levels: u32,
    pub leaf_size: f32,
    pub ranges: Vec<f32>,
}

pub const LEAF_TEXELS: u32 = 32; // TerrainGridN in TerrainWgpu.cpp

fn build(
    nodes: &mut Vec<Node>,
    w: &World,
    ox: u32,
    oz: u32,
    span: u32,
    grid: f32,
) -> i32 {
    let mut n = Node {
        origin: [ox as f32 * grid, oz as f32 * grid],
        size: span as f32 * grid,
        min_y: f32::MAX,
        max_y: f32::MIN,
        child: [-1; 4],
    };
    if span <= LEAF_TEXELS {
        let dim = w.spec.n;
        for z in oz..(oz + span + 1).min(dim) {
            for x in ox..(ox + span + 1).min(dim) {
                let h = w.height[(z * dim + x) as usize];
                n.min_y = n.min_y.min(h);
                n.max_y = n.max_y.max(h);
            }
        }
        if n.min_y > n.max_y {
            n.min_y = 0.0;
            n.max_y = 0.0;
        }
    } else {
        let half = span / 2;
        let c = [
            build(nodes, w, ox, oz, half, grid),
            build(nodes, w, ox + half, oz, half, grid),
            build(nodes, w, ox, oz + half, half, grid),
            build(nodes, w, ox + half, oz + half, half, grid),
        ];
        for (i, &ci) in c.iter().enumerate() {
            n.min_y = n.min_y.min(nodes[ci as usize].min_y);
            n.max_y = n.max_y.max(nodes[ci as usize].max_y);
            n.child[i] = ci;
        }
    }
    nodes.push(n);
    (nodes.len() - 1) as i32
}

impl Tree {
    /// `base_mult` / `ratio` match WGR_TERRAIN_LOD_BASE=4.0, WGR_TERRAIN_LOD_RATIO=2.0.
    pub fn new(w: &World, base_mult: f32, ratio: f32) -> Tree {
        let mut root_texels = LEAF_TEXELS;
        while root_texels < w.spec.n {
            root_texels *= 2;
        }
        let mut nodes = Vec::new();
        let root = build(&mut nodes, w, 0, 0, root_texels, w.spec.grid);
        let mut levels = 1u32;
        let mut t = LEAF_TEXELS;
        while t < root_texels {
            t *= 2;
            levels += 1;
        }
        let leaf_size = LEAF_TEXELS as f32 * w.spec.grid;
        let mut ranges = Vec::with_capacity(levels as usize);
        let mut r = leaf_size * base_mult;
        for _ in 0..levels {
            ranges.push(r);
            r *= ratio;
        }
        Tree { nodes, root, levels, leaf_size, ranges }
    }

    pub fn select(
        &self,
        cam: [f32; 3],
        visible: &dyn Fn(&Node) -> bool,
        out: &mut Vec<Selection>,
    ) {
        self.descend(self.root, self.levels as i32 - 1, cam, visible, out);
    }

    fn emit(&self, n: &Node, level: i32, out: &mut Vec<Selection>) {
        let end = self.ranges[level as usize];
        let prev = if level > 0 { self.ranges[level as usize - 1] } else { 0.0 };
        let morph_region = 0.50; // WGR_TERRAIN_MORPH default
        out.push(Selection {
            origin: n.origin,
            size: n.size,
            level: level as u32,
            morph_start: end - (end - prev) * morph_region,
            morph_end: end,
        });
    }

    fn descend(
        &self,
        idx: i32,
        level: i32,
        cam: [f32; 3],
        visible: &dyn Fn(&Node) -> bool,
        out: &mut Vec<Selection>,
    ) -> bool {
        let n = &self.nodes[idx as usize];
        let d2 = node_dist_sq(n, cam);
        let r = self.ranges[level as usize];
        if d2 > r * r {
            return false;
        }
        if !visible(n) {
            return true;
        }
        if level == 0 || n.child[0] < 0 {
            self.emit(n, level, out);
            return true;
        }
        let rp = self.ranges[level as usize - 1];
        if d2 > rp * rp {
            self.emit(n, level, out);
            return true;
        }
        for c in 0..4 {
            let ci = n.child[c];
            if !self.descend(ci, level - 1, cam, visible, out) && visible(&self.nodes[ci as usize]) {
                self.emit(&self.nodes[ci as usize], level - 1, out);
            }
        }
        true
    }
}

pub fn node_dist_sq(n: &Node, p: [f32; 3]) -> f32 {
    let max_x = n.origin[0] + n.size;
    let max_z = n.origin[1] + n.size;
    let dx = if p[0] < n.origin[0] {
        n.origin[0] - p[0]
    } else if p[0] > max_x {
        p[0] - max_x
    } else {
        0.0
    };
    let dy = if p[1] < n.min_y {
        n.min_y - p[1]
    } else if p[1] > n.max_y {
        p[1] - n.max_y
    } else {
        0.0
    };
    let dz = if p[2] < n.origin[1] {
        n.origin[1] - p[2]
    } else if p[2] > max_z {
        p[2] - max_z
    } else {
        0.0
    };
    dx * dx + dy * dy + dz * dz
}

// ------------------------------------------------------------- EXP-VEG-001

/// How many trees does the synthetic world actually carry, and what do the three
/// candidate representations cost? This is deliberately arithmetic on the real
/// density field rather than a hand-waved "20 million trees" figure.
pub fn cmd_veg(c: &crate::Common) {
    let spec = crate::world::WorldSpec { n: c.n, grid: c.grid, seed: c.seed, with_cover: true, veg_cell: 0.0 };
    let w = World::generate(spec);
    let cn = (w.spec.n >> COVER_SHIFT) as usize;
    let cstep = w.spec.grid * (1 << COVER_SHIFT) as f32;

    // Trees per hectare at full density — a plausible mixed-forest figure.
    const TREES_PER_HA_FULL: f32 = 420.0;
    let cell_ha = (cstep * cstep) / 10_000.0;

    let mut trees = 0.0f64;
    let mut forested_cells = 0u64;
    for i in 0..(cn * cn) {
        let d = w.cover[i * 4] as f32 / 255.0;
        if d > 0.02 {
            forested_cells += 1;
            trees += (d * TREES_PER_HA_FULL * cell_ha) as f64;
        }
    }

    let extent_km = w.spec.extent_m() / 1000.0;
    println!();
    println!("EXP-VEG-001  source dataset");
    println!("  world                 {:.1} x {:.1} km", extent_km, extent_km);
    println!("  cover cells           {} x {} @ {:.0} m", cn, cn, cstep);
    println!("  forested cover cells  {} ({:.1}%)", forested_cells, forested_cells as f32 / (cn * cn) as f32 * 100.0);
    println!("  implied trees         {:.2} M at {} trees/ha peak", trees / 1.0e6, TREES_PER_HA_FULL);

    println!();
    println!("  representation costs");
    // (a) one GPU instance per tree: 4x3 transform + model id
    let per_instance = 4 * 12 + 4;
    println!(
        "  a) per-tree instance    {:>10.0} MB   ({} B x {:.2} M)",
        trees * per_instance as f64 / 1.048576e6,
        per_instance,
        trees / 1.0e6
    );

    // (b) cluster records: one per cell of a chosen size, with a seed + counts.
    for &cluster_m in &[64.0f32, 128.0, 256.0, 512.0] {
        let cells = (w.spec.extent_m() / cluster_m).ceil() as u64;
        let total = cells * cells;
        let occupied = (total as f64 * (forested_cells as f64 / (cn * cn) as f64)) as u64;
        // seed + count + bounds + species mix + height range
        let rec = 32u64;
        println!(
            "  b) clusters @ {:>4.0} m    {:>10.2} MB   ({} occupied of {} cells, {} B each)",
            cluster_m,
            (occupied * rec) as f64 / 1.048576e6,
            occupied,
            total,
            rec
        );
    }

    // (c) density field only — what the far field actually needs.
    let dens_bytes = (cn * cn * 4) as f64; // density, canopy h, species, variation
    println!(
        "  c) density field        {:>10.2} MB   ({} x {} x 4 B, the cover map itself)",
        dens_bytes / 1.048576e6,
        cn,
        cn
    );
    println!();
    println!(
        "  ratio a:c = {:.0}x",
        (trees * per_instance as f64) / dens_bytes
    );
}
