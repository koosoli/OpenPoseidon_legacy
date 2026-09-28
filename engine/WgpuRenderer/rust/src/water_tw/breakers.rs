//! Tidewater's Breakers.js — the thrown lip of plunging breakers — ported from
//! dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software Solutions LLC). W3d.
//!
//! Stations are laid out every 0.6 m along the shoreline. Each frame a compute pass marches every
//! station's transect (64 steps of 2 m seaward) and finds the crests of the breaking waves as the
//! crossings of the shore-wave phase with integers — the same analytic field that drives the
//! water surface — and stores for each the lip root, the breaking progress and the wave height
//! (`tw_breakers_kernel.wgsl`). The lip is a ribbon mesh extruded along the crest from those
//! records: a ballistic curtain leaving the crest horizontally and falling in front of the
//! concave face, shaded like the water and blended over it with premultiplied alpha
//! (`tw_lip.wgsl`).
//!
//! What OP changes:
//!
//! * **Stations.** Tidewater traces its one beach once (`buildStations`: marching north from
//!   the water over a fixed x range, the longest run). OP traces the sea-level contour of the
//!   heightmap inside each shore-simulation region when the region is placed (marching squares
//!   on a 1 m grid), keeps every run of at least `MIN_RUN` metres (longest first, up to
//!   `MAX_STATIONS_PER_REGION`), and smooths and resamples each run as Tidewater does. The side
//!   toward the sea is where the terrain is lower. Runs are kept apart: a crest's id carries its
//!   run, so no lip segment joins two runs.
//! * **Spray** (W4a, `spray.rs`): the emitters run in this kernel for every crest found; the
//!   particle update is a second entry point of the same module and bind group.
//! * **Occlusion by the wave.** Tidewater's water writes depth, so the wave's back hides the
//!   curtain from a camera on the sea side. OP's water pass keeps depth read-only (for every
//!   backend), so the lip shader hides the part of the curtain the sight line reaches under the
//!   crest line instead (analytic, from the crest record).

/// Station spacing along the shoreline (m), Tidewater's.
pub const SPACING: f32 = 0.6;
/// Profile vertices across the lip: 2 on the back of the crest + 18 along the curtain.
pub const NV: u32 = 20;
// (the transect, 64 steps of 2 m seaward, is in tw_breakers_kernel.wgsl)
/// Shoreline runs shorter than this are not given breakers (rocks, piers, small islets).
pub const MIN_RUN: f32 = 30.0;
/// Station budget per shore-simulation region (~1.2 km of shoreline).
pub const MAX_STATIONS_PER_REGION: usize = 2048;
pub const MAX_STATIONS: usize = MAX_STATIONS_PER_REGION * 2;
/// Marching-squares grid step for the shoreline (m).
const GRID: f32 = 1.0;

/// A station: position, unit direction toward the sea, and its run (breakers never span runs).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Station {
    pub x: f32,
    pub z: f32,
    pub nx: f32,
    pub nz: f32,
    pub run: u32,
}

/// Zero crossings of `h - level` along the four cell edges -> polylines (marching squares, the
/// saddle cases decided by the cell centre).
fn contour(h: &dyn Fn(f32, f32) -> f32, min: [f32; 2], size: f32, level: f32) -> Vec<Vec<[f32; 2]>> {
    let n = (size / GRID).round().max(1.0) as usize; // cells per side
    let np = n + 1;
    let mut v = vec![0.0f32; np * np];
    for j in 0..np {
        for i in 0..np {
            v[j * np + i] = h(min[0] + i as f32 * GRID, min[1] + j as f32 * GRID) - level;
        }
    }
    let at = |i: usize, j: usize| v[j * np + i];
    // edge ids: horizontal edge (i,j)-(i+1,j) = 2 * (j * np + i), vertical (i,j)-(i,j+1) = +1
    let eh = |i: usize, j: usize| 2 * (j * np + i);
    let ev = |i: usize, j: usize| 2 * (j * np + i) + 1;
    let point = |e: usize| -> [f32; 2] {
        let k = e / 2;
        let (i, j) = (k % np, k / np);
        let (a, b, di, dj) = if e % 2 == 0 { (at(i, j), at(i + 1, j), 1.0, 0.0) } else { (at(i, j), at(i, j + 1), 0.0, 1.0) };
        let t = if (a - b).abs() > 1e-9 { (a / (a - b)).clamp(0.0, 1.0) } else { 0.5 };
        [min[0] + (i as f32 + di * t) * GRID, min[1] + (j as f32 + dj * t) * GRID]
    };
    // adjacency: every crossing edge is shared by at most two cells, each linking it to one other
    let mut link: std::collections::HashMap<usize, Vec<usize>> = std::collections::HashMap::new();
    let add = |a: usize, b: usize, link: &mut std::collections::HashMap<usize, Vec<usize>>| {
        link.entry(a).or_default().push(b);
        link.entry(b).or_default().push(a);
    };
    for j in 0..n {
        for i in 0..n {
            let c = [at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)];
            // edges in order: bottom, right, top, left
            let e = [eh(i, j), ev(i + 1, j), eh(i, j + 1), ev(i, j)];
            let mut crossing = Vec::with_capacity(4);
            for k in 0..4 {
                let (a, b) = (c[k], c[(k + 1) % 4]);
                if (a > 0.0) != (b > 0.0) {
                    crossing.push(k);
                }
            }
            match crossing.len() {
                2 => add(e[crossing[0]], e[crossing[1]], &mut link),
                4 => {
                    // saddle: connect so the centre's side stays connected
                    let centre = (c[0] + c[1] + c[2] + c[3]) * 0.25;
                    if (centre > 0.0) == (c[0] > 0.0) {
                        add(e[0], e[1], &mut link);
                        add(e[2], e[3], &mut link);
                    } else {
                        add(e[0], e[3], &mut link);
                        add(e[1], e[2], &mut link);
                    }
                }
                _ => {}
            }
        }
    }
    // walk the chains (open ones from an end first, then the closed loops)
    let mut seen = std::collections::HashSet::new();
    let mut out = Vec::new();
    let mut starts: Vec<usize> = link.iter().filter(|(_, l)| l.len() == 1).map(|(e, _)| *e).collect();
    starts.sort_unstable();
    let mut rest: Vec<usize> = link.keys().copied().collect();
    rest.sort_unstable();
    for s in starts.into_iter().chain(rest) {
        if seen.contains(&s) {
            continue;
        }
        let mut chain = vec![point(s)];
        seen.insert(s);
        let mut cur = s;
        loop {
            let next = link[&cur].iter().copied().find(|e| !seen.contains(e));
            match next {
                Some(nx) => {
                    seen.insert(nx);
                    chain.push(point(nx));
                    cur = nx;
                }
                None => break,
            }
        }
        out.push(chain);
    }
    out
}

fn length(p: &[[f32; 2]]) -> f32 {
    p.windows(2).map(|w| ((w[1][0] - w[0][0]).powi(2) + (w[1][1] - w[0][1]).powi(2)).sqrt()).sum()
}

/// Shoreline stations inside one region (see the module doc). `h`: terrain height at world xz.
pub fn trace_stations(h: &dyn Fn(f32, f32) -> f32, min: [f32; 2], size: f32, sea_level: f32, first_run: u32) -> Vec<Station> {
    let mut runs: Vec<Vec<[f32; 2]>> = contour(h, min, size, sea_level).into_iter().filter(|r| length(r) >= MIN_RUN).collect();
    runs.sort_by(|a, b| length(b).total_cmp(&length(a)));
    let mut out = Vec::new();
    for (ri, run) in runs.into_iter().enumerate() {
        // smooth the polyline (the transects should not follow every wiggle of the waterline):
        // Tidewater's 30 passes of (1, 2, 1) / 4, here on both coordinates (its beach runs along x)
        let mut sm = run;
        for _ in 0..30 {
            let prev = sm.clone();
            for k in 1..sm.len().saturating_sub(1) {
                sm[k] = [
                    (prev[k - 1][0] + 2.0 * prev[k][0] + prev[k + 1][0]) * 0.25,
                    (prev[k - 1][1] + 2.0 * prev[k][1] + prev[k + 1][1]) * 0.25,
                ];
            }
        }
        // resample by arc length
        let mut stations: Vec<Station> = Vec::new();
        let mut acc = 0.0f32;
        let mut next = 0.0f32;
        for k in 1..sm.len() {
            let (a, b) = (sm[k - 1], sm[k]);
            let seg = ((b[0] - a[0]).powi(2) + (b[1] - a[1]).powi(2)).sqrt();
            if seg < 1e-6 {
                continue;
            }
            let (tx, tz) = ((b[0] - a[0]) / seg, (b[1] - a[1]) / seg);
            while next <= acc + seg {
                let t = (next - acc) / seg;
                let (x, z) = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t);
                // normal toward the sea: decided once for the whole run below
                stations.push(Station { x, z, nx: -tz, nz: tx, run: first_run + ri as u32 });
                next += SPACING;
            }
            acc += seg;
        }
        // the side toward the sea (the lower terrain), by a vote over the run: one station on
        // uneven ground must not flip its transect inland
        let vote: i32 = stations
            .iter()
            .map(|s| if h(s.x + s.nx * 2.0, s.z + s.nz * 2.0) > h(s.x - s.nx * 2.0, s.z - s.nz * 2.0) { -1 } else { 1 })
            .sum();
        if vote < 0 {
            for s in &mut stations {
                s.nx = -s.nx;
                s.nz = -s.nz;
            }
        }
        if out.len() + stations.len() > MAX_STATIONS_PER_REGION {
            continue;
        }
        out.extend(stations);
    }
    out
}

/// Stations for the GPU: two vec4 per station, (x, z, nx, nz) and (run, 0, 0, 0).
pub fn pack(stations: &[Station]) -> Vec<[f32; 4]> {
    let mut out = Vec::with_capacity(stations.len() * 2);
    for s in stations {
        out.push([s.x, s.z, s.nx, s.nz]);
        out.push([s.run as f32, 0.0, 0.0, 0.0]);
    }
    out
}

/// The lip ribbon: per vertex (segment, slot, side, k) and the triangle list over
/// `max_stations - 1` segments x 2 slots (Breakers._buildMesh).
pub fn lip_mesh(max_stations: usize) -> (Vec<[f32; 4]>, Vec<u32>) {
    let nseg = max_stations.saturating_sub(1);
    let nv = NV as usize;
    let per_strip = nv * 2;
    let mut ids = Vec::with_capacity(nseg * 2 * per_strip);
    let mut index = Vec::with_capacity(nseg * 2 * (nv - 1) * 6);
    for seg in 0..nseg {
        for slot in 0..2 {
            let v0 = ((seg * 2 + slot) * per_strip) as u32;
            for side in 0..2 {
                for k in 0..nv {
                    ids.push([seg as f32, slot as f32, side as f32, k as f32]);
                }
            }
            for k in 0..(nv as u32 - 1) {
                let (a, b, c, d) = (v0 + k, v0 + k + 1, v0 + NV + k, v0 + NV + k + 1);
                index.extend_from_slice(&[a, c, b, b, c, d]);
            }
        }
    }
    (ids, index)
}

/// Indices to draw for `ns` stations (the segments between them, both slots).
pub fn lip_index_count(ns: usize) -> u32 {
    (ns.saturating_sub(1) * 2 * (NV as usize - 1) * 6) as u32
}


/// The GPU side: stations, crest records, the crest-finder kernel and the lip mesh + pipeline.
pub struct BreakersGpu {
    stations: wgpu::Buffer,
    crest: wgpu::Buffer,
    kernel_layout: wgpu::BindGroupLayout,
    kernel: wgpu::ComputePipeline,
    /// the spray update (tw_spray_update.wgsl), same module and bind group
    spray_update: wgpu::ComputePipeline,
    kernel_bind: Option<wgpu::BindGroup>,
    lip_vbuf: wgpu::Buffer,
    lip_ibuf: wgpu::Buffer,
    lip_pipeline: wgpu::RenderPipeline,
    /// the lip's coverage into the upscalers' reactive mask (W4b)
    lip_mask_pipeline: wgpu::RenderPipeline,
    /// stations uploaded (0: no lips)
    pub ns: u32,
    /// the lips are drawn (lip opacity > 0)
    pub lips_on: bool,
}

impl BreakersGpu {
    /// `render_layout`: the water's group 1 layout (the lip reads the crests at binding 19);
    /// `shader`: the water module, which carries vs_lip / fs_lip.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        device: &wgpu::Device,
        kernel_src: &str,
        camera_layout: &wgpu::BindGroupLayout,
        render_layout: &wgpu::BindGroupLayout,
        shader: &wgpu::ShaderModule,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
        linear: f64,
    ) -> Self {
        use wgpu::util::DeviceExt;
        let stations = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_brk_stations"),
            size: (MAX_STATIONS * 2 * 16) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let crest = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_brk_crest"),
            size: (MAX_STATIONS * 6 * 16) as u64,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let c = wgpu::ShaderStages::COMPUTE;
        let buf = |binding, ty| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Buffer { ty, has_dynamic_offset: false, min_binding_size: None },
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
        use wgpu::BufferBindingType as B;
        use wgpu::TextureViewDimension as D;
        let kernel_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("tw_brk_kernel"),
            entries: &[
                buf(0, B::Uniform),
                buf(1, B::Uniform),
                tex(2, D::D2, false),
                tex(3, D::D2, false),
                tex(4, D::D2, true),
                buf(5, B::Uniform),
                tex(6, D::D2Array, true),
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: c,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                buf(8, B::Storage { read_only: true }),
                buf(9, B::Storage { read_only: false }),
                // W4a spray: the block, the ring, the head
                buf(10, B::Uniform),
                buf(11, B::Storage { read_only: false }),
                buf(12, B::Storage { read_only: false }),
                // W6i: the CPU emit requests (the boats' bow spray)
                buf(13, B::Storage { read_only: true }),
            ],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("tw_brk_kernel"),
            source: wgpu::ShaderSource::Wgsl(kernel_src.into()),
        });
        let kpl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_brk_kernel"),
            bind_group_layouts: &[Some(&kernel_layout)],
            immediate_size: 0,
        });
        let kernel = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("tw_brk_kernel"),
            layout: Some(&kpl),
            module: &module,
            entry_point: Some("main"),
            compilation_options: Default::default(),
            cache: None,
        });
        let spray_update = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("tw_spray_update"),
            layout: Some(&kpl),
            module: &module,
            entry_point: Some("spray_update"),
            compilation_options: Default::default(),
            cache: None,
        });

        let (ids, index) = lip_mesh(MAX_STATIONS);
        let lip_vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_brk_lip_ids"),
            contents: bytemuck::cast_slice(&ids),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let lip_ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_brk_lip_index"),
            contents: bytemuck::cast_slice(&index),
            usage: wgpu::BufferUsages::INDEX,
        });
        let lpl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_brk_lip"),
            bind_group_layouts: &[Some(camera_layout), Some(render_layout)],
            immediate_size: 0,
        });
        let attrs = wgpu::vertex_attr_array![0 => Float32x4];
        let lip_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_brk_lip"),
            layout: Some(&lpl),
            vertex: wgpu::VertexState {
                module: shader,
                entry_point: Some("vs_lip"),
                compilation_options: Default::default(),
                buffers: &[wgpu::VertexBufferLayout {
                    array_stride: 16,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &attrs,
                }],
            },
            // side: 'double'
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            // transparent, depthWrite false, depthTest true
            depth_stencil: Some(wgpu::DepthStencilState {
                format: crate::gfx3d::depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: sample_count, ..Default::default() },
            fragment: Some(wgpu::FragmentState {
                module: shader,
                entry_point: Some("fs_lip"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &[("linear", linear)],
                    ..Default::default()
                },
                // blending: 'premultiplied'
                targets: &[Some(wgpu::ColorTargetState {
                    format: surface_format,
                    blend: Some(wgpu::BlendState::PREMULTIPLIED_ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let lip_mask_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_brk_lip_mask"),
            layout: Some(&lpl),
            vertex: wgpu::VertexState {
                module: shader,
                entry_point: Some("vs_lip"),
                compilation_options: Default::default(),
                buffers: &[wgpu::VertexBufferLayout {
                    array_stride: 16,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &attrs,
                }],
            },
            primitive: wgpu::PrimitiveState { topology: wgpu::PrimitiveTopology::TriangleList, cull_mode: None, ..Default::default() },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: shader,
                entry_point: Some("fs_lip_mask"),
                compilation_options: wgpu::PipelineCompilationOptions { constants: &[("linear", linear)], ..Default::default() },
                targets: &[Some(super::spray::mask_target())],
            }),
            multiview_mask: None,
            cache: None,
        });
        Self {
            stations,
            crest,
            kernel_layout,
            kernel,
            spray_update,
            kernel_bind: None,
            lip_vbuf,
            lip_ibuf,
            lip_pipeline,
            lip_mask_pipeline,
            ns: 0,
            lips_on: true,
        }
    }

    pub fn crest_buffer(&self) -> &wgpu::Buffer {
        &self.crest
    }

    #[allow(clippy::too_many_arguments)]
    pub fn rebuild_bind(
        &mut self,
        device: &wgpu::Device,
        surface_ubo: &wgpu::Buffer,
        conform_ubo: &wgpu::Buffer,
        heightmap: &wgpu::TextureView,
        field_t: &wgpu::TextureView,
        field_dir: &wgpu::TextureView,
        ocean_ubo: &wgpu::Buffer,
        displacement: &wgpu::TextureView,
        smp_linear_repeat: &wgpu::Sampler,
        spray: &super::spray::SprayGpu,
    ) {
        use wgpu::BindingResource as R;
        self.kernel_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_brk_kernel"),
            layout: &self.kernel_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: surface_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: conform_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: R::TextureView(heightmap) },
                wgpu::BindGroupEntry { binding: 3, resource: R::TextureView(field_t) },
                wgpu::BindGroupEntry { binding: 4, resource: R::TextureView(field_dir) },
                wgpu::BindGroupEntry { binding: 5, resource: ocean_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 6, resource: R::TextureView(displacement) },
                wgpu::BindGroupEntry { binding: 7, resource: R::Sampler(smp_linear_repeat) },
                wgpu::BindGroupEntry { binding: 8, resource: self.stations.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 9, resource: self.crest.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 10, resource: spray.params.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 11, resource: spray.ring.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 12, resource: spray.head.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 13, resource: spray.requests.as_entire_binding() },
            ],
        }));
    }

    pub fn set_stations(&mut self, queue: &wgpu::Queue, stations: &[Station]) {
        let n = stations.len().min(MAX_STATIONS);
        if n > 0 {
            queue.write_buffer(&self.stations, 0, bytemuck::cast_slice(&pack(&stations[..n])));
        }
        self.ns = n as u32;
    }

    /// The crest finder and the spray emitters (Breakers.update), then the spray update
    /// (Spray.update). `crests`: the shore simulation runs (the crest finder has regions to
    /// search); `breaker_spray`: the breakers emit; `spray_update`: the spray is on this frame
    /// (breakers or, W6i, the boats' bow spray).
    pub fn dispatch(&self, encoder: &mut wgpu::CommandEncoder, crests: bool, breaker_spray: bool, spray_update: bool) {
        let Some(bind) = &self.kernel_bind else { return };
        if crests && self.ns >= 2 && (self.lips_on || breaker_spray) {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("tw_brk_crests"), timestamp_writes: None });
            cp.set_pipeline(&self.kernel);
            cp.set_bind_group(0, bind, &[]);
            cp.dispatch_workgroups(self.ns.div_ceil(64), 1, 1);
        }
        if spray_update {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("tw_spray_update"), timestamp_writes: None });
            cp.set_pipeline(&self.spray_update);
            cp.set_bind_group(0, bind, &[]);
            cp.dispatch_workgroups(super::spray::SLOTS.div_ceil(64), 1, 1);
        }
    }

    /// The lips' coverage into the reactive mask (W4b; a mask pass, no depth attachment).
    pub fn draw_mask(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32, render_bind: &wgpu::BindGroup) {
        if self.ns < 2 || !self.lips_on {
            return;
        }
        pass.set_pipeline(&self.lip_mask_pipeline);
        pass.set_bind_group(0, camera, &[offset]);
        pass.set_bind_group(1, render_bind, &[]);
        pass.set_vertex_buffer(0, self.lip_vbuf.slice(..));
        pass.set_index_buffer(self.lip_ibuf.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed(0..lip_index_count(self.ns as usize), 0, 0..1);
    }

    /// The lips, over the water (same pass, after the surface).
    pub fn draw(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32, render_bind: &wgpu::BindGroup) {
        if self.ns < 2 || !self.lips_on {
            return;
        }
        pass.set_pipeline(&self.lip_pipeline);
        pass.set_bind_group(0, camera, &[offset]);
        pass.set_bind_group(1, render_bind, &[]);
        pass.set_vertex_buffer(0, self.lip_vbuf.slice(..));
        pass.set_index_buffer(self.lip_ibuf.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed(0..lip_index_count(self.ns as usize), 0, 0..1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A beach whose waterline is the line z = 100 + 10 sin(x / 40), sea at smaller z.
    fn beach(x: f32, z: f32) -> f32 {
        (z - 100.0 - 10.0 * (x / 40.0).sin()) * 0.05
    }

    #[test]
    fn traces_the_waterline_with_normals_toward_the_sea() {
        let st = trace_stations(&beach, [0.0, 0.0], 380.0, 0.0, 0);
        assert!(st.len() > 500, "{}", st.len());
        for s in &st {
            let zl = 100.0 + 10.0 * (s.x / 40.0).sin();
            assert!((s.z - zl).abs() < 1.5, "{s:?} vs {zl}");
            assert!(s.nz < -0.5, "normal should face the sea (-z): {s:?}");
            assert!(((s.nx * s.nx + s.nz * s.nz).sqrt() - 1.0).abs() < 1e-3);
        }
        // evenly spaced
        for w in st.windows(2) {
            let d = ((w[1].x - w[0].x).powi(2) + (w[1].z - w[0].z).powi(2)).sqrt();
            assert!(d < SPACING * 1.05, "{d}");
        }
        assert!(st.iter().all(|s| s.run == 0));
    }

    #[test]
    fn small_islets_get_no_breakers_and_runs_are_kept_apart() {
        // open sea with one 60 m round island and one 6 m rock
        let h = |x: f32, z: f32| {
            let a = 30.0 - ((x - 120.0).powi(2) + (z - 120.0).powi(2)).sqrt();
            let b = 3.0 - ((x - 300.0).powi(2) + (z - 300.0).powi(2)).sqrt();
            a.max(b) * 0.1
        };
        let st = trace_stations(&h, [0.0, 0.0], 380.0, 0.0, 7);
        assert!(!st.is_empty());
        assert!(st.iter().all(|s| s.run == 7), "one run: the island");
        for s in &st {
            let r = ((s.x - 120.0).powi(2) + (s.z - 120.0).powi(2)).sqrt();
            assert!((r - 30.0).abs() < 2.0);
            // outward = toward the sea
            assert!((s.x - 120.0) * s.nx + (s.z - 120.0) * s.nz > 0.0);
        }
    }

    #[test]
    fn mesh_matches_tidewaters_layout() {
        let (ids, index) = lip_mesh(3);
        assert_eq!(ids.len(), 2 * 2 * 2 * NV as usize);
        assert_eq!(index.len() as u32, lip_index_count(3));
        assert_eq!(ids[NV as usize], [0.0, 0.0, 1.0, 0.0]);
        assert_eq!(&index[0..6], &[0, NV, 1, 1, NV, NV + 1]);
    }
}
