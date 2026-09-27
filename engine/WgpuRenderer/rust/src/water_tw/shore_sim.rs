//! Tidewater's ShoreSim.js — the Eulerian state of the surf and swash zone — ported from
//! dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software Solutions LLC). W3b.
//!
//! Per texel, updated every frame on the GPU (Tidewater's channel meanings):
//!   r = foam carried by the water (made by the bore roller, the plunge point and the swash front;
//!       advected with the flow, thinned where the flow spreads it out)
//!   g = sand wetness (1 while covered, dries over dryTime ~ half a minute)
//!   b = foam stranded on the sand when the water drains away (pops over a few seconds)
//!   a = depth-averaged flow speed along the local wave direction (m/s)
//!
//! The kernel is Tidewater's, line for line (`kernel_wgsl`). What OP changes (plan §6 W3):
//!
//! * **Regions.** Tidewater has one fixed region (380 m, 768²) over its beach. OP keeps up to
//!   `K` = 2 such regions, the layers of one texture array, on the coast nearest the camera. The
//!   coast is indexed by the shore field (`ShoreField::coast`, 190 m blocks); `select` picks
//!   the nearest blocks, at least a region apart, and keeps a region until a candidate is
//!   `HYSTERESIS` metres nearer, so regions do not flip back and forth.
//! * **Warm-up.** A region placed anew starts empty and fades in over `WARMUP_S` seconds of
//!   simulation (its weight in `tw.sim[r].w`), so no half-built foam state pops in. A region
//!   the camera has left behind is dropped (its layer is reused).
//! * **Beyond the regions** the shore waves still render from the shore-field phase with their
//!   analytic foam (Tidewater's material without SIM); the terrain falls back to Tidewater's
//!   static damp band outside the simulated area, as Tidewater's `terrainWetness` does.
//! * **Spray deposits** (`shoreSimDepositAt`) come with the spray in W4; until then no drops fall
//!   back into the water (Tidewater's deposit buffer, all zeros).
//! * **Heights** are relative to OP's sea level (Tidewater's sea level is 0: its `ground > 3.2`
//!   is `ground - seaLevel > 3.2` here).

/// Tidewater's region: 380 m square, 768² texels.
pub const RES: u32 = 768;
pub const SIZE: f32 = 380.0;
/// Regions kept at once (plan §6 W3: start with K = 2).
pub const K: usize = 2;
/// Fade-in of a new region (s of simulation).
pub const WARMUP_S: f32 = 3.0;
/// Coast farther than this from the camera is not simulated (m).
pub const MAX_DIST: f32 = 1200.0;
/// A region is replaced only by a candidate this much nearer (m).
pub const HYSTERESIS: f32 = 150.0;
/// Two regions' centres are at least this far apart (m).
pub const MIN_SEPARATION: f32 = 300.0;

/// ShoreSimParams defaults (ShoreSim.js).
pub const DRY_TIME: f32 = 28.0;
pub const FOAM_LIFE: f32 = 4.5;
pub const SURF_FOAM_LIFE: f32 = 2.6;
pub const RESIDUE_LIFE: f32 = 5.0;
pub const FOAM_GEN: f32 = 1.0;
pub const DEPOSIT_GAIN: f32 = 0.02;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Region {
    pub centre: [f32; 2],
    pub age: f32,
}

impl Region {
    pub fn min(&self) -> [f32; 2] {
        [self.centre[0] - SIZE * 0.5, self.centre[1] - SIZE * 0.5]
    }

    /// Weight of the region in the materials (fades in over the warm-up).
    pub fn weight(&self) -> f32 {
        let t = (self.age / WARMUP_S).clamp(0.0, 1.0);
        t * t * (3.0 - 2.0 * t)
    }
}

fn dist(a: [f32; 2], b: [f32; 2]) -> f32 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)).sqrt()
}

/// Place the regions for a camera at `cam` (world xz) over the coast `candidates`.
/// Returns the slots whose region is new (their state must be cleared).
pub fn select(slots: &mut [Option<Region>; K], candidates: &[[f32; 2]], cam: [f32; 2]) -> [bool; K] {
    let mut fresh = [false; K];
    // the wanted set: nearest candidates within reach, at least MIN_SEPARATION apart
    let mut near: Vec<(f32, [f32; 2])> = candidates
        .iter()
        .map(|c| (dist(*c, cam), *c))
        .filter(|(d, _)| *d <= MAX_DIST)
        .collect();
    near.sort_by(|a, b| a.0.total_cmp(&b.0));
    let mut wanted: Vec<(f32, [f32; 2])> = Vec::with_capacity(K);
    for (d, c) in near {
        if wanted.iter().all(|(_, w)| dist(*w, c) >= MIN_SEPARATION) {
            wanted.push((d, c));
            if wanted.len() == K {
                break;
            }
        }
    }
    // drop regions the camera has left behind
    for s in slots.iter_mut() {
        if let Some(r) = s {
            if dist(r.centre, cam) > MAX_DIST + HYSTERESIS {
                *s = None;
            }
        }
    }
    for (d, c) in wanted {
        // already simulated (or covered by a region close enough to it)
        if slots.iter().flatten().any(|r| dist(r.centre, c) < MIN_SEPARATION) {
            continue;
        }
        // an empty slot, else the farthest region if this candidate is clearly nearer
        let target = if let Some(i) = slots.iter().position(|s| s.is_none()) {
            Some(i)
        } else {
            let (i, far) = slots
                .iter()
                .enumerate()
                .map(|(i, s)| (i, s.map_or(f32::INFINITY, |r| dist(r.centre, cam))))
                .max_by(|a, b| a.1.total_cmp(&b.1))
                .unwrap();
            if d + HYSTERESIS < far { Some(i) } else { None }
        };
        if let Some(i) = target {
            slots[i] = Some(Region { centre: c, age: 0.0 });
            fresh[i] = true;
        }
    }
    fresh
}

pub struct ShoreSim {
    state_a: wgpu::Texture,
    state_a_view: wgpu::TextureView,
    state_b: wgpu::Texture,
    state_b_view: wgpu::TextureView,
    pub lace_view: wgpu::TextureView,
    layout: wgpu::BindGroupLayout,
    pipeline: wgpu::ComputePipeline,
    bind: Option<wgpu::BindGroup>,
    pub regions: [Option<Region>; K],
    zeros: Vec<u8>,
    /// bumped whenever the views the materials read change (bind-group rebuilds)
    pub generation: u64,
}

impl ShoreSim {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue, shader_src: &str) -> Self {
        let make = |label: &str, usage: wgpu::TextureUsages| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d { width: RES, height: RES, depth_or_array_layers: K as u32 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage,
                view_formats: &[],
            })
        };
        use wgpu::TextureUsages as U;
        // A: read by the materials and the kernel; B: written by the kernel, copied into A
        let state_a = make("tw_shore_sim_a", U::TEXTURE_BINDING | U::COPY_DST);
        let state_b = make("tw_shore_sim_b", U::STORAGE_BINDING | U::COPY_SRC);
        let array_view = |t: &wgpu::Texture| {
            t.create_view(&wgpu::TextureViewDescriptor {
                dimension: Some(wgpu::TextureViewDimension::D2Array),
                ..Default::default()
            })
        };
        let state_a_view = array_view(&state_a);
        let state_b_view = array_view(&state_b);
        let zeros = vec![0u8; (RES * RES * 8) as usize];
        for layer in 0..K as u32 {
            clear_layer(queue, &state_a, layer, &zeros);
        }

        // the lace (SurfFoam.js makeLaceTexture: rgba8unorm with the CPU mip chain)
        let mips = super::lace::lace_mips(super::lace::LACE_SIZE);
        let lace = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("tw_surf_lace"),
            size: wgpu::Extent3d {
                width: super::lace::LACE_SIZE,
                height: super::lace::LACE_SIZE,
                depth_or_array_layers: 1,
            },
            mip_level_count: mips.len() as u32,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: U::TEXTURE_BINDING | U::COPY_DST,
            view_formats: &[],
        });
        for (m, data) in mips.iter().enumerate() {
            let s = (super::lace::LACE_SIZE >> m).max(1);
            queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: &lace,
                    mip_level: m as u32,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                data,
                wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(s * 4), rows_per_image: Some(s) },
                wgpu::Extent3d { width: s, height: s, depth_or_array_layers: 1 },
            );
        }
        let lace_view = lace.create_view(&Default::default());

        let c = wgpu::ShaderStages::COMPUTE;
        let uniform = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let tex = |binding, dim, filterable| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable },
                view_dimension: dim,
                multisampled: false,
            },
            count: None,
        };
        use wgpu::TextureViewDimension as D;
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("tw_shore_sim"),
            entries: &[
                uniform(0),
                uniform(1),
                tex(2, D::D2, false),
                tex(3, D::D2, false),
                tex(4, D::D2, true),
                tex(5, D::D2Array, true),
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: c,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: D::D2Array,
                    },
                    count: None,
                },
                tex(7, D::D2, true),
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: c,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("tw_shore_sim_kernel"),
            source: wgpu::ShaderSource::Wgsl(shader_src.into()),
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_shore_sim"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("tw_shore_sim"),
            layout: Some(&pl),
            module: &module,
            entry_point: Some("main"),
            compilation_options: Default::default(),
            cache: None,
        });
        Self {
            state_a,
            state_a_view,
            state_b,
            state_b_view,
            lace_view,
            layout,
            pipeline,
            bind: None,
            regions: [None; K],
            zeros,
            // unique per instance: a backend switch makes a new simulation with new views
            generation: {
                static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
                NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            },
        }
    }

    pub fn state_view(&self) -> &wgpu::TextureView {
        &self.state_a_view
    }

    /// (Re)build the kernel's bind group from the backend's current resources.
    #[allow(clippy::too_many_arguments)]
    pub fn rebuild_bind(
        &mut self,
        device: &wgpu::Device,
        surface_ubo: &wgpu::Buffer,
        conform_ubo: &wgpu::Buffer,
        heightmap: &wgpu::TextureView,
        field_t: &wgpu::TextureView,
        field_dir: &wgpu::TextureView,
        smp_linear_repeat: &wgpu::Sampler,
    ) {
        use wgpu::BindingResource as R;
        self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_shore_sim"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: surface_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: conform_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: R::TextureView(heightmap) },
                wgpu::BindGroupEntry { binding: 3, resource: R::TextureView(field_t) },
                wgpu::BindGroupEntry { binding: 4, resource: R::TextureView(field_dir) },
                wgpu::BindGroupEntry { binding: 5, resource: R::TextureView(&self.state_a_view) },
                wgpu::BindGroupEntry { binding: 6, resource: R::TextureView(&self.state_b_view) },
                wgpu::BindGroupEntry { binding: 7, resource: R::TextureView(&self.lace_view) },
                wgpu::BindGroupEntry { binding: 8, resource: R::Sampler(smp_linear_repeat) },
            ],
        }));
    }

    /// Place the regions for this frame and age them. `dt`: water-clock step (0 while paused).
    pub fn update_regions(&mut self, queue: &wgpu::Queue, candidates: &[[f32; 2]], cam: [f32; 2], dt: f32) {
        let fresh = select(&mut self.regions, candidates, cam);
        for (i, f) in fresh.iter().enumerate() {
            if *f {
                clear_layer(queue, &self.state_a, i as u32, &self.zeros);
            }
        }
        for r in self.regions.iter_mut().flatten() {
            r.age += dt;
        }
    }

    /// `tw.sim[r]` lanes: min xz, size, weight (all 0 for an empty slot).
    pub fn lanes(&self) -> [[f32; 4]; K] {
        let mut out = [[0.0; 4]; K];
        for (i, r) in self.regions.iter().enumerate() {
            if let Some(r) = r {
                let m = r.min();
                out[i] = [m[0], m[1], SIZE, r.weight()];
            }
        }
        out
    }

    pub fn any_active(&self) -> bool {
        self.regions.iter().any(|r| r.is_some())
    }

    /// One simulation step for every placed region (ShoreSim.update), then B -> A.
    pub fn dispatch(&self, encoder: &mut wgpu::CommandEncoder) {
        let Some(bind) = &self.bind else { return };
        if !self.any_active() {
            return;
        }
        {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("tw_shore_sim"),
                timestamp_writes: None,
            });
            cp.set_pipeline(&self.pipeline);
            cp.set_bind_group(0, bind, &[]);
            cp.dispatch_workgroups(RES.div_ceil(8), RES.div_ceil(8), K as u32);
        }
        encoder.copy_texture_to_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &self.state_b,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyTextureInfo {
                texture: &self.state_a,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::Extent3d { width: RES, height: RES, depth_or_array_layers: K as u32 },
        );
    }
}

fn clear_layer(queue: &wgpu::Queue, tex: &wgpu::Texture, layer: u32, zeros: &[u8]) {
    queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture: tex,
            mip_level: 0,
            origin: wgpu::Origin3d { x: 0, y: 0, z: layer },
            aspect: wgpu::TextureAspect::All,
        },
        zeros,
        wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(RES * 8), rows_per_image: Some(RES) },
        wgpu::Extent3d { width: RES, height: RES, depth_or_array_layers: 1 },
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(n: usize) -> Vec<[f32; 2]> {
        // a straight coast along x, blocks every 190 m
        (0..n).map(|i| [i as f32 * 190.0 + 95.0, 1000.0]).collect()
    }

    #[test]
    fn places_the_nearest_coast_a_region_apart() {
        let mut slots = [None; K];
        let fresh = select(&mut slots, &line(20), [1000.0, 900.0]);
        assert_eq!(fresh, [true, true]);
        let a = slots[0].unwrap().centre;
        let b = slots[1].unwrap().centre;
        assert!(dist(a, b) >= MIN_SEPARATION);
        assert!(dist(a, [1000.0, 900.0]) < 200.0);
    }

    #[test]
    fn small_camera_moves_keep_the_regions() {
        let mut slots = [None; K];
        select(&mut slots, &line(20), [1000.0, 900.0]);
        let before = slots;
        for step in 0..10 {
            let fresh = select(&mut slots, &line(20), [1000.0 + step as f32 * 10.0, 900.0]);
            assert_eq!(fresh, [false, false]);
        }
        assert_eq!(slots.map(|s| s.map(|r| r.centre)), before.map(|s| s.map(|r| r.centre)));
    }

    #[test]
    fn a_far_move_replaces_one_region_and_leaving_the_coast_drops_them() {
        let mut slots = [None; K];
        select(&mut slots, &line(40), [1000.0, 900.0]);
        let fresh = select(&mut slots, &line(40), [4000.0, 900.0]);
        assert!(fresh.iter().any(|f| *f));
        assert!(slots.iter().flatten().any(|r| dist(r.centre, [4000.0, 900.0]) < 200.0));
        select(&mut slots, &line(40), [4000.0, 900.0 - MAX_DIST - HYSTERESIS - 500.0]);
        assert!(slots.iter().all(|s| s.is_none()));
    }

    #[test]
    fn warm_up_fades_in() {
        let r = Region { centre: [0.0, 0.0], age: 0.0 };
        assert_eq!(r.weight(), 0.0);
        assert_eq!(Region { age: WARMUP_S, ..r }.weight(), 1.0);
        assert!(Region { age: WARMUP_S * 0.5, ..r }.weight() > 0.4);
    }
}
