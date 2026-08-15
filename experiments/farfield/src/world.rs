// Synthetic benchmark world: a deterministic, procedurally generated heightfield
// plus coarse vegetation and building layers. No game assets, so the benchmark is
// reproducible on any checkout and carries no data licence.
//
// The world is generated at (n x n) texels of `grid` metres, so world size and
// sample density are independent knobs — which is the whole point: "100 km of
// terrain" says nothing until you also say at what resolution it is stored.
//
// Three GPU resources come out of this:
//   height   R32Float n x n, mip 0 only   -- the authoritative surface
//   top      R32Float n x n, full mip chain, each level the MAX of its children
//            -- the acceleration pyramid the hierarchical raymarch descends
//   cover    RGBA8 (n/4) x (n/4)          -- vegetation density / canopy height /
//                                            building occupancy / building height
//
// `top` is built over terrain alone or over terrain + cover, selected at generation
// time, because EXP-FAR-002 (bare heightfield) and EXP-FAR-003/4 (with a canopy and
// building shell) need different acceleration structures over the same terrain.

pub const COVER_SHIFT: u32 = 2; // cover grid is 1/4 the heightfield resolution
pub const CANOPY_SCALE: f32 = 40.0; // u8 -> metres for canopy height
pub const BUILDING_SCALE: f32 = 60.0; // u8 -> metres for building height

#[derive(Clone, Copy)]
pub struct WorldSpec {
    pub n: u32,       // heightfield texels per side
    pub grid: f32,    // metres between texels
    pub seed: u32,
    pub with_cover: bool, // fold vegetation/buildings into the `top` pyramid
    /// When > 0, vegetation is not taken from the smooth cover field but BAKED from
    /// explicit tree instances into vertical occupancy columns of this cell size, in
    /// metres. This is the 2.5D-column hypothesis under test (EXP-COL-001).
    pub veg_cell: f32,
}

impl WorldSpec {
    pub fn extent_m(&self) -> f32 {
        self.n as f32 * self.grid
    }
}

pub struct World {
    pub spec: WorldSpec,
    pub height: Vec<f32>,      // n*n
    pub cover: Vec<u8>,        // (n>>SHIFT)^2 * 4, RGBA
    pub pyramid: Vec<Vec<f32>>, // level 0 = n*n max surface, then halved
    pub h_min: f32,
    pub h_max: f32,
    pub gen_ms: f32,
    /// Explicit tree instances generated for the column bake, and the column grid
    /// they were baked into (cells per side, cell size in metres).
    pub baked_trees: u64,
    pub column_cells: u32,
    /// Bytes the three layers occupy, before any GPU-side compression.
    pub cpu_bytes: u64,
}

// ---------------------------------------------------------------- noise

#[inline]
fn hash2(x: i32, y: i32, seed: u32) -> u32 {
    let mut h = (x as u32).wrapping_mul(0x8da6_b343) ^ (y as u32).wrapping_mul(0xd8163841) ^ seed;
    h ^= h >> 15;
    h = h.wrapping_mul(0x2c1b_3c6d);
    h ^= h >> 12;
    h = h.wrapping_mul(0x2971_1917);
    h ^= h >> 16;
    h
}

#[inline]
fn grad(x: i32, y: i32, seed: u32) -> f32 {
    (hash2(x, y, seed) >> 8) as f32 * (1.0 / 16_777_216.0) * 2.0 - 1.0
}

#[inline]
fn smooth(t: f32) -> f32 {
    t * t * (3.0 - 2.0 * t)
}

/// Value noise; cheap, and the spectral difference from gradient noise does not
/// matter for a traversal benchmark.
fn vnoise(x: f32, y: f32, seed: u32) -> f32 {
    let xi = x.floor();
    let yi = y.floor();
    let tx = smooth(x - xi);
    let ty = smooth(y - yi);
    let (xi, yi) = (xi as i32, yi as i32);
    let a = grad(xi, yi, seed);
    let b = grad(xi + 1, yi, seed);
    let c = grad(xi, yi + 1, seed);
    let d = grad(xi + 1, yi + 1, seed);
    let ab = a + (b - a) * tx;
    let cd = c + (d - c) * tx;
    ab + (cd - ab) * ty
}

fn fbm(x: f32, y: f32, oct: u32, seed: u32) -> f32 {
    let mut f = 1.0;
    let mut a = 0.5;
    let mut s = 0.0;
    let mut norm = 0.0;
    for i in 0..oct {
        s += vnoise(x * f, y * f, seed.wrapping_add(i * 7919)) * a;
        norm += a;
        f *= 2.0;
        a *= 0.5;
    }
    s / norm
}

/// Ridged multifractal — the mountain generator.
fn ridged(x: f32, y: f32, oct: u32, seed: u32) -> f32 {
    let mut f = 1.0;
    let mut a = 0.5;
    let mut s = 0.0;
    let mut norm = 0.0;
    for i in 0..oct {
        let v = 1.0 - vnoise(x * f, y * f, seed.wrapping_add(i * 5417)).abs() * 2.0;
        s += v * v * a;
        norm += a;
        f *= 2.0;
        a *= 0.55;
    }
    s / norm
}

// ---------------------------------------------------------------- terrain

/// One sample of the world model, in world metres. Returns (height, forest, urban).
fn sample(wx: f32, wz: f32, seed: u32) -> (f32, f32, f32) {
    // Continent shape at ~40 km wavelength: gives coastline and open sea.
    let cx = wx / 40_000.0;
    let cz = wz / 40_000.0;
    let continent = fbm(cx * 2.0, cz * 2.0, 4, seed) * 1.6 + 0.25;

    // Mountain belts at ~8 km, masked to the land interior.
    let mask = (continent * 2.2).clamp(0.0, 1.0);
    let mount = ridged(wx / 8_000.0, wz / 8_000.0, 7, seed ^ 0x51ed) * mask * mask;

    // Rolling mid-scale relief and fine detail.
    let rolling = fbm(wx / 2_000.0, wz / 2_000.0, 5, seed ^ 0x9e37) * 0.5 + 0.5;
    let detail = fbm(wx / 220.0, wz / 220.0, 3, seed ^ 0x1234);

    let land = continent.max(0.0);
    let h = -60.0 * (-continent).max(0.0) * 3.0     // sea floor
        + land * 120.0                                // coastal plain
        + mount * 1450.0                              // mountains
        + rolling * 45.0 * mask                       // rolling relief
        + detail * 9.0 * mask;                        // fine detail

    // Forest: patchy, absent above the tree line and below the shore.
    let f0 = fbm(wx / 1_500.0, wz / 1_500.0, 4, seed ^ 0xbeef) * 0.5 + 0.5;
    let f1 = fbm(wx / 300.0, wz / 300.0, 3, seed ^ 0xf00d) * 0.5 + 0.5;
    let tree_line = (1.0 - ((h - 900.0) / 400.0)).clamp(0.0, 1.0);
    let shore = ((h - 1.0) / 8.0).clamp(0.0, 1.0);
    let forest = (((f0 - 0.42) * 3.2).clamp(0.0, 1.0) * (0.35 + 0.65 * f1) * tree_line * shore)
        .clamp(0.0, 1.0);

    // Villages: deterministic cluster lattice at 6 km, jittered, radius ~350 m.
    let lat = 6_000.0f32;
    let gx = (wx / lat).floor() as i32;
    let gz = (wz / lat).floor() as i32;
    let mut urban = 0.0f32;
    for dz in -1..=1 {
        for dx in -1..=1 {
            let (cxi, czi) = (gx + dx, gz + dz);
            let hh = hash2(cxi, czi, seed ^ 0xc0ffee);
            if (hh & 3) == 0 {
                continue; // not every lattice cell gets a village
            }
            let jx = (cxi as f32 + ((hh >> 8) & 255) as f32 / 255.0) * lat;
            let jz = (czi as f32 + ((hh >> 16) & 255) as f32 / 255.0) * lat;
            let d = ((wx - jx).powi(2) + (wz - jz).powi(2)).sqrt();
            let r = 220.0 + ((hh >> 24) & 255) as f32 * 1.4;
            urban = urban.max((1.0 - d / r).clamp(0.0, 1.0));
        }
    }
    // Towns need buildable ground.
    urban *= shore * (1.0 - ((h - 500.0) / 300.0)).clamp(0.0, 1.0);

    (h, forest, urban)
}

impl World {
    pub fn generate(spec: WorldSpec) -> World {
        let t0 = std::time::Instant::now();
        let n = spec.n as usize;
        let cn = n >> COVER_SHIFT;

        let mut height = vec![0.0f32; n * n];
        let mut cover = vec![0u8; cn * cn * 4];

        let threads = std::thread::available_parallelism()
            .map(|v| v.get())
            .unwrap_or(4)
            .min(32);

        // A plain mutex, not bit-pattern atomics: IEEE bit patterns do not order
        // negative floats the way an integer max does, and this world has sea floor
        // below zero. That mistake silently reports h_max = -3.4e38.
        let extremes = std::sync::Mutex::new((f32::MAX, f32::MIN));

        std::thread::scope(|s| {
            let rows_per = n.div_ceil(threads);
            for (ti, chunk) in height.chunks_mut(rows_per * n).enumerate() {
                let z0 = ti * rows_per;
                let extremes = &extremes;
                s.spawn(move || {
                    let mut lo = f32::MAX;
                    let mut hi = f32::MIN;
                    for (r, row) in chunk.chunks_mut(n).enumerate() {
                        let wz = (z0 + r) as f32 * spec.grid;
                        for (x, h) in row.iter_mut().enumerate() {
                            let v = sample(x as f32 * spec.grid, wz, spec.seed).0;
                            *h = v;
                            lo = lo.min(v);
                            hi = hi.max(v);
                        }
                    }
                    let mut e = extremes.lock().unwrap();
                    e.0 = e.0.min(lo);
                    e.1 = e.1.max(hi);
                });
            }
        });

        // Cover layers at 1/4 resolution: one sample per cover texel is enough,
        // these are density fields, not surfaces.
        let cstep = spec.grid * (1 << COVER_SHIFT) as f32;
        std::thread::scope(|s| {
            let rows_per = cn.div_ceil(threads);
            for (ti, chunk) in cover.chunks_mut(rows_per * cn * 4).enumerate() {
                let z0 = ti * rows_per;
                s.spawn(move || {
                    for (r, row) in chunk.chunks_mut(cn * 4).enumerate() {
                        let wz = (z0 + r) as f32 * cstep;
                        for (x, px) in row.chunks_mut(4).enumerate() {
                            let wx = x as f32 * cstep;
                            let (_, forest, urban) = sample(wx, wz, spec.seed);
                            // Canopy height varies with a slow field: 8..28 m.
                            let ch = if forest > 0.02 {
                                8.0 + 20.0 * (fbm(wx / 900.0, wz / 900.0, 2, spec.seed ^ 0xaa) * 0.5 + 0.5)
                            } else {
                                0.0
                            };
                            // Building height: mostly low, taller near cluster core.
                            let bh = if urban > 0.05 { 5.0 + 22.0 * urban * urban } else { 0.0 };
                            px[0] = (forest * 255.0) as u8;
                            px[1] = (ch / CANOPY_SCALE * 255.0).min(255.0) as u8;
                            px[2] = (urban * 255.0) as u8;
                            px[3] = (bh / BUILDING_SCALE * 255.0).min(255.0) as u8;
                        }
                    }
                });
            }
        });

        let (h_min, h_max) = *extremes.lock().unwrap();


        // ---- EXP-COL-001: bake explicit tree instances into vertical columns ----
        //
        // The hypothesis under test is that a far field which looks "voxelized" at
        // close range need not be a 3D voxel volume at all: one vertical occupancy
        // interval per XY cell reproduces the same failure mode. So here real tree
        // instances are generated, then collapsed into (base, top, density) per cell
        // and the instances are thrown away. Nothing below this point knows that
        // individual trees ever existed.
        let mut baked_trees = 0u64;
        let mut column_cells = 0u32;
        let mut veg_top = vec![0.0f32; 0];
        if spec.veg_cell > 0.0 {
            let cells = (spec.extent_m() / spec.veg_cell).ceil() as usize;
            column_cells = cells as u32;
            veg_top = vec![0.0f32; cells * cells];
            // Tree instances on a jittered lattice, accepted against the forest
            // density field - the same rule the GPU placement uses, so the two
            // experiments describe the same forest.
            let tstep = 7.0f32;
            let tn = (spec.extent_m() / tstep) as i64;
            let stamp = |veg_top: &mut Vec<f32>, wx: f32, wz: f32, h: f32, r: f32| {
                let c0 = (((wx - r) / spec.veg_cell).floor() as i64).max(0);
                let c1 = (((wx + r) / spec.veg_cell).ceil() as i64).min(cells as i64 - 1);
                let d0 = (((wz - r) / spec.veg_cell).floor() as i64).max(0);
                let d1 = (((wz + r) / spec.veg_cell).ceil() as i64).min(cells as i64 - 1);
                for cz in d0..=d1 {
                    for cx in c0..=c1 {
                        let e = &mut veg_top[cz as usize * cells + cx as usize];
                        if h > *e { *e = h; }
                    }
                }
            };
            for tz in 0..tn {
                for tx in 0..tn {
                    let hh = hash2(tx as i32, tz as i32, spec.seed ^ 0x7ee5);
                    let wx = (tx as f32 + 0.5 + ((hh >> 8) & 255) as f32 / 255.0 - 0.5) * tstep;
                    let wz = (tz as f32 + 0.5 + ((hh >> 16) & 255) as f32 / 255.0 - 0.5) * tstep;
                    let (th, forest, _) = sample(wx, wz, spec.seed);
                    if forest < 0.02 { continue; }
                    if ((hh >> 24) & 255) as f32 / 255.0 > forest { continue; }
                    let canopy = 9.0 + 16.0 * (((hh >> 4) & 63) as f32 / 63.0);
                    baked_trees += 1;
                    stamp(&mut veg_top, wx, wz, th + canopy, canopy * 0.30);
                }
            }
        }

        // Level 0 of the acceleration pyramid: the surface the raymarch must not
        // pass through. With cover folded in, that is the canopy/roof top.
        let mut top = height.clone();
        if spec.veg_cell > 0.0 {
            let cells = column_cells as usize;
            for z in 0..n {
                for x in 0..n {
                    let cx = ((x as f32 * spec.grid) / spec.veg_cell) as usize;
                    let cz = ((z as f32 * spec.grid) / spec.veg_cell) as usize;
                    let v = veg_top[cz.min(cells - 1) * cells + cx.min(cells - 1)];
                    if v > top[z * n + x] {
                        top[z * n + x] = v;
                    }
                }
            }
        } else if spec.with_cover {
            for z in 0..n {
                for x in 0..n {
                    let ci = ((z >> COVER_SHIFT) * cn + (x >> COVER_SHIFT)) * 4;
                    let canopy = if cover[ci] > 5 {
                        cover[ci + 1] as f32 / 255.0 * CANOPY_SCALE
                    } else {
                        0.0
                    };
                    let bld = if cover[ci + 2] > 12 {
                        cover[ci + 3] as f32 / 255.0 * BUILDING_SCALE
                    } else {
                        0.0
                    };
                    top[z * n + x] += canopy.max(bld);
                }
            }
        }

        let mut pyramid = Vec::new();
        pyramid.push(top);
        let mut dim = n;
        while dim > 1 {
            let src = pyramid.last().unwrap();
            let nd = dim / 2;
            let mut dst = vec![0.0f32; nd * nd];
            for z in 0..nd {
                for x in 0..nd {
                    let a = src[(z * 2) * dim + x * 2];
                    let b = src[(z * 2) * dim + x * 2 + 1];
                    let c = src[(z * 2 + 1) * dim + x * 2];
                    let d = src[(z * 2 + 1) * dim + x * 2 + 1];
                    dst[z * nd + x] = a.max(b).max(c).max(d);
                }
            }
            pyramid.push(dst);
            dim = nd;
        }

        let cpu_bytes = (height.len() * 4 + cover.len() + pyramid.iter().map(|p| p.len() * 4).sum::<usize>())
            as u64;

        World {
            spec,
            baked_trees,
            column_cells,
            height,
            cover,
            pyramid,
            h_min,
            h_max,
            gen_ms: t0.elapsed().as_secs_f32() * 1000.0,
            cpu_bytes,
        }
    }

    pub fn levels(&self) -> u32 {
        self.pyramid.len() as u32
    }
}

// ---------------------------------------------------------------- upload

pub struct WorldGpu {
    pub height: wgpu::Texture,
    pub height_view: wgpu::TextureView,
    pub top: wgpu::Texture,
    pub top_view: wgpu::TextureView,
    pub cover: wgpu::Texture,
    pub cover_view: wgpu::TextureView,
    pub vram_bytes: u64,
}

fn upload_r32(
    gpu: &crate::gpu::Gpu,
    label: &str,
    n: u32,
    mips: &[Vec<f32>],
) -> (wgpu::Texture, wgpu::TextureView, u64) {
    let tex = gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: n, height: n, depth_or_array_layers: 1 },
        mip_level_count: mips.len() as u32,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R32Float,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    let mut bytes = 0u64;
    let mut dim = n;
    for (level, data) in mips.iter().enumerate() {
        gpu.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &tex,
                mip_level: level as u32,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(data),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(dim * 4),
                rows_per_image: Some(dim),
            },
            wgpu::Extent3d { width: dim, height: dim, depth_or_array_layers: 1 },
        );
        bytes += (dim as u64) * (dim as u64) * 4;
        dim = (dim / 2).max(1);
    }
    let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    (tex, view, bytes)
}

impl WorldGpu {
    pub fn upload(gpu: &crate::gpu::Gpu, w: &World) -> WorldGpu {
        let n = w.spec.n;
        let (height, height_view, hb) = upload_r32(gpu, "height", n, std::slice::from_ref(&w.height));
        let (top, top_view, tb) = upload_r32(gpu, "top-maxmip", n, &w.pyramid);

        let cn = n >> COVER_SHIFT;
        let cover = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("cover"),
            size: wgpu::Extent3d { width: cn, height: cn, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        gpu.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &cover,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &w.cover,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(cn * 4),
                rows_per_image: Some(cn),
            },
            wgpu::Extent3d { width: cn, height: cn, depth_or_array_layers: 1 },
        );
        let cover_view = cover.create_view(&wgpu::TextureViewDescriptor::default());
        let cb = (cn as u64) * (cn as u64) * 4;

        WorldGpu {
            height,
            height_view,
            top,
            top_view,
            cover,
            cover_view,
            vram_bytes: hb + tb + cb,
        }
    }
}
