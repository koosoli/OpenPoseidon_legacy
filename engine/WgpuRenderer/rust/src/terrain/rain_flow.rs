//! RAIN-FLOW (OP, 2026-10-13): rain that runs off and gathers into puddles, as a shallow-water
//! height field around the camera (see rain_flow.wgsl for the model). The terrain samples the
//! result (`view`, r = water depth m, g = flow speed m/s) through its shore-wetness group
//! (shore_wet.rs, binding 3) inside the window `window()`, and keeps the noise puddles outside it.
//!
//! Switch: `WGR_RAIN_FLOW=0` turns the simulation off (the window weight goes to 0 and the terrain is
//! the noise puddles everywhere, as before).

use crate::ffi::WgrTerrainParams;

/// Cells per side and their size: a 128 m window at 25 cm.
pub const N: u32 = 512;
pub const CELL: f32 = 0.25;
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
    last_time: Option<f32>,
    active: bool,
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
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
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
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
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
            last_time: None,
            active: false,
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
        self.active = enabled() && params.hm_width > 1;
        if !self.active {
            self.last_time = None;
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
            if s == 0 {
                pass.set_pipeline(&self.validate);
                pass.dispatch_workgroups(groups, groups, 1);
            }
            pass.set_pipeline(&self.flux_pipe);
            pass.dispatch_workgroups(groups, groups, 1);
            pass.set_pipeline(&self.depth_pipe);
            pass.dispatch_workgroups(groups, groups, 1);
        }
        encoder.pop_debug_group();
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn params_layout_matches_the_wgsl_struct() {
        assert_eq!(std::mem::size_of::<super::RainParams>(), 64);
    }

    #[test]
    fn rain_flow_wgsl_validates() {
        let module = naga::front::wgsl::parse_str(include_str!("rain_flow.wgsl")).expect("parse");
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("validate");
    }
}
