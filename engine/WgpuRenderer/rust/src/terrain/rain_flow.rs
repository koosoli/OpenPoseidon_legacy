//! RAIN-FLOW (OP, 2026-10-13): rain that runs off and gathers into puddles, as a shallow-water
//! height field around the camera (see rain_flow.wgsl for the model). The terrain samples the
//! result (`view`, r = water depth m, g = flow speed m/s) through its shore-wetness group
//! (shore_wet.rs, binding 3) inside the window `window()`, and keeps the noise puddles outside it.
//!
//! Switch: `WGR_RAIN_FLOW=0` turns the simulation off (the window weight goes to 0 and the terrain is
//! the noise puddles everywhere, as before).

use crate::ffi::WgrTerrainParams;

/// Cells per side and their size: a 128 m window at 1/3 m. (512 at 25 cm measured 0.85 ms of GPU
/// frame in rain, 22.07 vs 21.21 ms median, Everon field, 2026-10-13: too much for puddles.)
pub const N: u32 = 384;
pub const CELL: f32 = 128.0 / 384.0;
/// After the rain stops the water keeps flowing and drying this long (s), then the simulation
/// stops (no cost in dry weather) and the terrain is back to its own wetness.
pub const DRY_AFTER: f32 = 600.0;
/// Rain at full strength (m/s): ~50x a real downpour, so puddles gather in minutes, not hours.
pub const RAIN_RATE: f32 = 3.0e-4;
/// The share of the rain the soil takes while it falls, and what it keeps taking afterwards (m/s).
pub const SOAK_SHARE: f32 = 0.85;
pub const SOAK_BASE: f32 = 2.0e-5;
/// Seconds per substep, and the most substeps a frame.
pub const DT: f32 = 0.04;
pub const MAX_SUBSTEPS: u32 = 3;

pub fn enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| !std::env::var("WGR_RAIN_FLOW").is_ok_and(|v| v.trim() == "0"))
}

#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
struct RainParams {
    origin: [i32; 2],
    n: u32,
    substep: u32,
    cell: f32,
    dt: f32,
    rain: f32,
    soak: f32,
    hm_origin: [f32; 2],
    hm_grid: f32,
    sea_level: f32,
    hm_dims: [u32; 2],
    _pad: [u32; 2],
}

pub struct RainFlow {
    layout: wgpu::BindGroupLayout,
    validate: wgpu::ComputePipeline,
    flux_pipe: wgpu::ComputePipeline,
    depth_pipe: wgpu::ComputePipeline,
    /// one uniform buffer per substep (each pass reads its own substep index)
    ubos: Vec<wgpu::Buffer>,
    depth: wgpu::Buffer,
    flux: wgpu::Buffer,
    keys: wgpu::Buffer,
    ground: wgpu::Buffer,
    view: wgpu::TextureView,
    binds: Vec<wgpu::BindGroup>,
    heightmap_gen: u64,
    origin: [i32; 2],
    /// the origin the slots were last validated for (validate only when the window moved)
    validated: Option<[i32; 2]>,
    last_time: Option<f32>,
    /// game seconds since the last rain (DRY_AFTER stops the simulation)
    dry_for: f32,
    active: bool,
    frames: u64,
}

impl RainFlow {
    pub fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_rain_flow"),
            source: wgpu::ShaderSource::Wgsl(include_str!("rain_flow.wgsl").into()),
        });
        let c = wgpu::ShaderStages::COMPUTE;
        let storage = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: c,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: false },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_rain_flow"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: c,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: c,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                storage(2),
                storage(3),
                storage(4),
                storage(6),
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: c,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba16Float,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                    count: None,
                },
            ],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_rain_flow"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipe = |entry: &str| {
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some(entry),
                layout: Some(&pl),
                module: &module,
                entry_point: Some(entry),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        let cells = (N * N) as u64;
        let buffer = |label, size| {
            device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(label),
                size,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            })
        };
        // A fresh buffer is zero, the key of cell (0, 0): a slot standing for any other cell resets
        // on the first validate, and one that does stand for (0, 0) is correctly dry anyway.
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_rain_flow_out"),
            size: wgpu::Extent3d { width: N, height: N, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba16Float,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let ubos = (0..MAX_SUBSTEPS)
            .map(|_| {
                device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("wgr_rain_flow_params"),
                    size: std::mem::size_of::<RainParams>() as u64,
                    usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                })
            })
            .collect();
        Self {
            validate: pipe("cs_validate"),
            flux_pipe: pipe("cs_flux"),
            depth_pipe: pipe("cs_depth"),
            layout,
            ubos,
            depth: buffer("wgr_rain_flow_depth", cells * 4),
            flux: buffer("wgr_rain_flow_flux", cells * 16),
            keys: buffer("wgr_rain_flow_keys", cells * 4),
            ground: buffer("wgr_rain_flow_ground", cells * 4),
            view: tex.create_view(&Default::default()),
            binds: Vec::new(),
            heightmap_gen: u64::MAX,
            origin: [0, 0],
            validated: None,
            last_time: None,
            dry_for: f32::INFINITY,
            active: false,
            frames: 0,
        }
    }

    /// The water texture (r depth m, g speed m/s), slot (x mod N, z mod N) for absolute cell (x, z).
    pub fn view(&self) -> &wgpu::TextureView {
        &self.view
    }

    /// (window min x, min z, size m, weight): the terrain's rain lane; weight 0 = not running.
    pub fn window(&self) -> [f32; 4] {
        if !self.active {
            return [0.0; 4];
        }
        [self.origin[0] as f32 * CELL, self.origin[1] as f32 * CELL, N as f32 * CELL, 1.0]
    }

    /// One frame: re-centre the window on the camera, then up to MAX_SUBSTEPS steps of the flow.
    #[allow(clippy::too_many_arguments)]
    pub fn step(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        heightmap: &wgpu::TextureView,
        heightmap_gen: u64,
        params: &WgrTerrainParams,
        cam_xz: [f32; 2],
    ) {
        let raining = params._pad2 > 0.0;
        let dt_seen = match self.last_time {
            Some(t) if params.time >= t => (params.time - t).min(5.0),
            _ => 0.0,
        };
        self.dry_for = if raining { 0.0 } else { self.dry_for + dt_seen };
        self.active = enabled() && params.hm_width > 1 && self.dry_for < DRY_AFTER;
        if !self.active {
            self.last_time = Some(params.time);
            // the water is gone: a later shower starts from dry ground
            if self.validated.is_some() {
                encoder.clear_buffer(&self.keys, 0, None);
                self.validated = None;
            }
            return;
        }
        if self.binds.is_empty() || self.heightmap_gen != heightmap_gen {
            self.binds = self
                .ubos
                .iter()
                .map(|ubo| {
                    device.create_bind_group(&wgpu::BindGroupDescriptor {
                        label: Some("wgr_rain_flow"),
                        layout: &self.layout,
                        entries: &[
                            wgpu::BindGroupEntry { binding: 0, resource: ubo.as_entire_binding() },
                            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(heightmap) },
                            wgpu::BindGroupEntry { binding: 2, resource: self.depth.as_entire_binding() },
                            wgpu::BindGroupEntry { binding: 3, resource: self.flux.as_entire_binding() },
                            wgpu::BindGroupEntry { binding: 4, resource: self.keys.as_entire_binding() },
                            wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::TextureView(&self.view) },
                            wgpu::BindGroupEntry { binding: 6, resource: self.ground.as_entire_binding() },
                        ],
                    })
                })
                .collect();
            // a new heightmap (another world, a crater) invalidates every slot's cached ground
            if self.heightmap_gen != u64::MAX {
                encoder.clear_buffer(&self.keys, 0, None);
                self.validated = None;
            }
            self.heightmap_gen = heightmap_gen;
        }
        // the window, centred on the camera, moved in whole cells
        let half = (N / 2) as i32;
        self.origin = [
            (cam_xz[0] / CELL).floor() as i32 - half,
            (cam_xz[1] / CELL).floor() as i32 - half,
        ];
        // game time drives it (a paused game holds the water still); a jump re-syncs
        let dt_frame = match self.last_time {
            Some(t) if params.time >= t && params.time - t < 0.5 => params.time - t,
            _ => DT,
        };
        self.last_time = Some(params.time);
        let substeps = ((dt_frame / DT).ceil() as u32).clamp(1, MAX_SUBSTEPS);
        let dt = (dt_frame / substeps as f32).min(DT * 1.5);
        // tp.rain_strength is the effective liquid rain (0..1) the puddles already use
        let rain = params._pad2.clamp(0.0, 1.0) * RAIN_RATE;
        let soak = rain * SOAK_SHARE + SOAK_BASE;
        let groups = N.div_ceil(8);
        encoder.push_debug_group("wgr_rain_flow");
        for s in 0..substeps {
            let p = RainParams {
                origin: self.origin,
                n: N,
                substep: if s + 1 == substeps { 1 } else { 0 },
                cell: CELL,
                dt,
                rain,
                soak,
                hm_origin: [params.world_origin.x, params.world_origin.y],
                hm_grid: params.terrain_grid,
                sea_level: params.sea_level,
                hm_dims: [params.hm_width, params.hm_height],
                _pad: [0; 2],
            };
            queue.write_buffer(&self.ubos[s as usize], 0, bytemuck::bytes_of(&p));
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_rain_flow"),
                timestamp_writes: None,
            });
            pass.set_bind_group(0, &self.binds[s as usize], &[]);
            if s == 0 && self.validated != Some(self.origin) {
                pass.set_pipeline(&self.validate);
                pass.dispatch_workgroups(groups, groups, 1);
            }
            pass.set_pipeline(&self.flux_pipe);
            pass.dispatch_workgroups(groups, groups, 1);
            pass.set_pipeline(&self.depth_pipe);
            pass.dispatch_workgroups(groups, groups, 1);
        }
        encoder.pop_debug_group();
        self.validated = Some(self.origin);
        self.frames += 1;
        if self.frames % 600 == 1 {
            eprintln!(
                "[wgr] rain flow: window {:.1},{:.1} +{:.0} m, camera {:.1},{:.1}, rain {:.2}, {} substeps of {:.3} s",
                self.origin[0] as f32 * CELL, self.origin[1] as f32 * CELL, N as f32 * CELL, cam_xz[0], cam_xz[1],
                params._pad2, substeps, dt
            );
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn params_layout_matches_the_wgsl_struct() {
        assert_eq!(std::mem::size_of::<super::RainParams>(), 64);
    }

    /// Runs the real passes on a synthetic slope with a shallow bowl, five minutes of rain, and
    /// writes the depth field to RAIN_FLOW_PGM (default: the temp dir). Ignored: a look, not a gate.
    #[test]
    #[ignore]
    fn rain_flow_depth_field_on_a_slope() {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let Ok(adapter) = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default())) else {
            eprintln!("skipped: no adapter");
            return;
        };
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).unwrap();
        // 64 x 64 heights at 12.5 m: a 2 % slope down +x with a 1.5 m deep bowl at (400, 420)
        let (w, h, grid) = (64u32, 64u32, 12.5f32);
        let mut heights = Vec::with_capacity((w * h) as usize);
        for z in 0..h {
            for x in 0..w {
                let (wx, wz) = (x as f32 * grid, z as f32 * grid);
                let bowl = -1.5 * (-(((wx - 400.0).powi(2) + (wz - 420.0).powi(2)) / (2.0 * 30.0f32.powi(2)))).exp();
                heights.push(50.0 - wx * 0.02 + bowl);
            }
        }
        let hm = device.create_texture(&wgpu::TextureDescriptor {
            label: None,
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        queue.write_texture(
            hm.as_image_copy(),
            bytemuck::cast_slice(&heights),
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        let hm_view = hm.create_view(&Default::default());
        let mut params = <crate::ffi::WgrTerrainParams as bytemuck::Zeroable>::zeroed();
        params.terrain_grid = grid;
        params.hm_width = w;
        params.hm_height = h;
        params.sea_level = -100.0;
        params._pad2 = 1.0;
        let mut flow = super::RainFlow::new(&device);
        let cam = [410.0, 400.0];
        for frame in 0..(300 * 30) {
            params.time = frame as f32 / 30.0;
            let mut enc = device.create_command_encoder(&Default::default());
            flow.step(&device, &queue, &mut enc, &hm_view, 1, &params, cam);
            queue.submit([enc.finish()]);
            if frame % 300 == 0 {
                let _ = device.poll(wgpu::PollType::wait_indefinitely());
            }
        }
        // read the output texture back
        let n = super::N;
        let row = n * 8;
        let buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: (row * n) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc = device.create_command_encoder(&Default::default());
        enc.copy_texture_to_buffer(
            flow.view().texture().as_image_copy(),
            wgpu::TexelCopyBufferInfo {
                buffer: &buf,
                layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(row), rows_per_image: Some(n) },
            },
            wgpu::Extent3d { width: n, height: n, depth_or_array_layers: 1 },
        );
        queue.submit([enc.finish()]);
        buf.slice(..).map_async(wgpu::MapMode::Read, |_| {});
        let _ = device.poll(wgpu::PollType::wait_indefinitely());
        let data = buf.slice(..).get_mapped_range().expect("wgpu: mapped range of a mapped buffer");
        let px: &[u16] = bytemuck::cast_slice(&data);
        let f16 = |b: u16| -> f32 {
            let (s, e, m) = ((b >> 15) as i32, ((b >> 10) & 31) as i32, (b & 1023) as f32);
            let v = if e == 0 { m / 1024.0 * 2f32.powi(-14) } else { (1.0 + m / 1024.0) * 2f32.powi(e - 15) };
            if s == 1 { -v } else { v }
        };
        let depth: Vec<f32> = (0..(n * n) as usize).map(|i| f16(px[i * 4])).collect();
        // the cached ground per slot, to tell the sim from its input
        let gbuf = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: (n * n * 4) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc = device.create_command_encoder(&Default::default());
        enc.copy_buffer_to_buffer(&flow.ground, 0, &gbuf, 0, (n * n * 4) as u64);
        queue.submit([enc.finish()]);
        gbuf.slice(..).map_async(wgpu::MapMode::Read, |_| {});
        let _ = device.poll(wgpu::PollType::wait_indefinitely());
        let gdata = gbuf.slice(..).get_mapped_range().expect("wgpu: mapped range of a mapped buffer");
        let ground: &[f32] = bytemuck::cast_slice(&gdata);
        let (gmin, gmax) = ground.iter().fold((f32::MAX, f32::MIN), |(a, b), v| (a.min(*v), b.max(*v)));
        eprintln!("ground: {gmin:.3} .. {gmax:.3}; slot (0,0) {:.3}, (128,0) {:.3}, (256,0) {:.3}, (0,128) {:.3}",
                  ground[0], ground[128], ground[256], ground[128 * n as usize]);
        if let Ok(dir) = std::env::var("RAIN_FLOW_RAW") {
            std::fs::write(format!("{dir}/ground.bin"), bytemuck::cast_slice(ground)).unwrap();
            std::fs::write(format!("{dir}/depth.bin"), bytemuck::cast_slice(&depth)).unwrap();
            std::fs::write(format!("{dir}/origin.txt"), format!("{} {} {}", flow.origin[0], flow.origin[1], n)).unwrap();
        }
        let max = depth.iter().cloned().fold(0.0f32, f32::max);
        let wet = depth.iter().filter(|d| **d > 0.003).count();
        eprintln!("rain flow: max depth {max:.3} m, {wet} of {} cells over 3 mm", depth.len());
        // the image in WINDOW order (slot -> absolute cell), so the seam would show as a seam
        let origin = flow.origin;
        let mut img = vec![0u8; (n * n) as usize];
        for z in 0..n as i32 {
            for x in 0..n as i32 {
                let (ax, az) = (origin[0] + x, origin[1] + z);
                let s = (az.rem_euclid(n as i32) * n as i32 + ax.rem_euclid(n as i32)) as usize;
                img[(z * n as i32 + x) as usize] = ((depth[s] / 0.05).clamp(0.0, 1.0) * 255.0) as u8;
            }
        }
        let path = std::env::var("RAIN_FLOW_PGM")
            .unwrap_or_else(|_| std::env::temp_dir().join("rain_flow.pgm").to_string_lossy().into_owned());
        let mut pgm = format!("P5\n{n} {n}\n255\n").into_bytes();
        pgm.extend_from_slice(&img);
        std::fs::write(&path, pgm).unwrap();
        eprintln!("wrote {path}");
    }

    #[test]
    fn rain_flow_wgsl_validates() {
        let module = naga::front::wgsl::parse_str(include_str!("rain_flow.wgsl")).expect("parse");
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("validate");
    }
}
