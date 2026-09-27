//! Tidewater Native — the second water backend (Tidewater Water Plan v5), a port of
//! dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software Solutions LLC; see CREDITS.md).
//!
//! W2 (this file): the open sea. The multi-cascade FFT (`fft.rs`), its compute mip chain, the
//! sea-detail and foam-pattern textures (`textures.rs`), the CDLOD surface (`cdlod.rs`) and
//! the water material with SSR + sky/cloud panorama + absorption (`tw_water.wgsl`).
//!
//! Owns every resource it uses; `WaterBackend` drops the whole struct when switching away, so
//! nothing here runs or stays resident while Current OP is active (plan §4.2).
//!
//! Not in W2, by design (later packages): shore waves / breakers / ShoreSim / surf foam (W3),
//! spray and RefractionPass (W4), underwater and caustics (W5), boat wakes (W6). Inland water
//! bodies are not drawn in this mode yet (see TW-WATER-01, open question for the owner).

mod cdlod;
mod fft;
mod textures;

use crate::ffi::{WgrCamera, WgrWaterParams};

/// Whether this build carries a working Tidewater backend.
pub const AVAILABLE: bool = true;

/// `WgrWaterParams::tidewater` defaults: Tidewater's own values (App.js / OceanFFT.js /
/// WaterMaterial.js / SeaDetail.js at 4811ba48). Lane meanings are in wgpu_renderer.hpp.
pub const DEFAULT_PARAMS: [[f32; 4]; 4] = [
    [1.0, 0.9, 0.48, 5.0],
    [1.0, 1.0, 1.0, 1.0],
    [1.0, 0.035, 1.0, 0.035],
    [1.0, 1.0, 0.3, 0.0],
];

/// Tidewater's `frame.waterAbsorption` / `frame.waterScattering` defaults (Frame.js), 1/m.
const WATER_ABSORPTION: [f32; 3] = [0.42, 0.075, 0.035];
const WATER_SCATTERING: [f32; 3] = [0.012, 0.018, 0.024];

/// `TwSurface` in tw_water.wgsl.
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
struct TwSurface {
    sea: [f32; 4],
    wind: [f32; 4],
    surface: [f32; 4],
    material0: [f32; 4],
    material1: [f32; 4],
    absorption: [f32; 4],
    scattering: [f32; 4],
    detail: [f32; 4],
    morph: [[f32; 4]; cdlod::LEVELS],
}

const _: () = assert!(std::mem::size_of::<TwSurface>() == 16 * (8 + cdlod::LEVELS));

fn surface_block(p: &WgrWaterParams, detail_offset: [f32; 2], morph: &[[f32; 4]; cdlod::LEVELS]) -> TwSurface {
    let tw = p.tidewater;
    let wind_len = (p.fft_wind_sea[0].powi(2) + p.fft_wind_sea[1].powi(2)).sqrt();
    let wind_dir = if wind_len > 1e-4 {
        [p.fft_wind_sea[0] / wind_len, p.fft_wind_sea[1] / wind_len]
    } else {
        // Tidewater's default heading
        [0.3494, 0.9370]
    };
    let wind_speed = if p.fft_wind_sea[2] > 0.0 { p.fft_wind_sea[2] } else { 7.0 };
    // WRL-003: the level under the camera when the producer supplied one, else the sea.
    let cam_water = if p.optics_params[3] > 0.0 { p.optics_params[2] } else { p.sea_level };
    TwSurface {
        sea: [p.sea_level, p.time, wind_speed, cam_water],
        wind: [wind_dir[0], wind_dir[1], detail_offset[0], detail_offset[1]],
        // amplitude, slope scale, foam coverage, foam pattern scale (WaterSurface.js)
        surface: [tw[0][0], 1.0, tw[1][0], 0.09],
        // backscatter, sss, foam intensity, reflection strength
        material0: [tw[2][3], tw[2][2], tw[1][1], tw[1][3]],
        // roughness, ssr on, debug view, unused
        material1: [tw[2][1], tw[1][2], tw[3][3], 0.0],
        absorption: [WATER_ABSORPTION[0], WATER_ABSORPTION[1], WATER_ABSORPTION[2], 0.0],
        scattering: [WATER_SCATTERING[0], WATER_SCATTERING[1], WATER_SCATTERING[2], 0.0],
        detail: [tw[3][0], tw[3][1], tw[3][2], 0.0],
        morph: *morph,
    }
}

fn dummy_view(device: &wgpu::Device, label: &str, format: wgpu::TextureFormat) -> wgpu::TextureView {
    device
        .create_texture(&wgpu::TextureDescriptor {
            label: Some(label),
            size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        })
        .create_view(&Default::default())
}

pub struct Tidewater {
    fft: fft::OceanFft,
    cdlod: cdlod::Cdlod,
    surface_ubo: wgpu::Buffer,
    conform_ubo: wgpu::Buffer,
    foam_view: wgpu::TextureView,
    detail_view: wgpu::TextureView,
    smp_linear_repeat: wgpu::Sampler,
    smp_aniso_repeat: wgpu::Sampler,
    smp_linear_clamp: wgpu::Sampler,
    env_sampler: wgpu::Sampler,
    scene_view: wgpu::TextureView,
    depth_view: wgpu::TextureView,
    env_view: wgpu::TextureView,
    heightmap_view: wgpu::TextureView,
    layout: wgpu::BindGroupLayout,
    bind: wgpu::BindGroup,
    bind_dirty: bool,
    pipeline: wgpu::RenderPipeline,
    grid_vbuf: wgpu::Buffer,
    grid_ibuf: wgpu::Buffer,
    grid_index_count: u32,
    instance_buf: wgpu::Buffer,
    instance_count: u32,
    last_params: Option<WgrWaterParams>,
    last_time: Option<f32>,
    detail_offset: [f32; 2],
    gens: [u64; 4], // depth, env, scene, heightmap
}

impl Tidewater {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera_layout: &wgpu::BindGroupLayout,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
    ) -> Self {
        use wgpu::util::DeviceExt;
        let fft = fft::OceanFft::new(device);
        let cdlod = cdlod::Cdlod::default();
        let surface_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_surface"),
            size: std::mem::size_of::<TwSurface>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let conform_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_conform"),
            size: std::mem::size_of::<crate::terrain::TerrainConformParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(
            &conform_ubo,
            0,
            bytemuck::bytes_of(&<crate::terrain::TerrainConformParams as bytemuck::Zeroable>::zeroed()),
        );
        let foam_view = textures::create_foam_texture(device, queue);
        let detail_view = textures::create_sea_detail(device, queue);
        let smp_linear_repeat = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_linear_repeat"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });
        let smp_aniso_repeat = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_aniso4_repeat"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            anisotropy_clamp: 4,
            ..Default::default()
        });
        let smp_linear_clamp = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_linear_clamp"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let env_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_env"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });
        let scene_view = dummy_view(device, "tw_dummy_scene", wgpu::TextureFormat::Rgba16Float);
        let depth_view = dummy_view(device, "tw_dummy_depth", wgpu::TextureFormat::Depth32Float);
        let env_view = dummy_view(device, "tw_dummy_env", wgpu::TextureFormat::Rgba16Float);
        let heightmap_view = dummy_view(device, "tw_dummy_heightmap", wgpu::TextureFormat::R32Float);

        let vf = wgpu::ShaderStages::VERTEX_FRAGMENT;
        let uniform = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: vf,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let tex = |binding, dim, sample_type| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: vf,
            ty: wgpu::BindingType::Texture { sample_type, view_dimension: dim, multisampled: false },
            count: None,
        };
        let smp = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: vf,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        use wgpu::TextureSampleType as S;
        use wgpu::TextureViewDimension as D;
        let flt = S::Float { filterable: true };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("tw_group1"),
            entries: &[
                uniform(0),
                uniform(1),
                tex(2, D::D2Array, flt),
                tex(3, D::D2Array, flt),
                smp(4),
                smp(5),
                tex(6, D::D2, flt),
                tex(7, D::D2, flt),
                tex(8, D::D2, flt),
                tex(9, D::D2, S::Depth),
                smp(10),
                tex(11, D::D2, flt),
                smp(12),
                tex(13, D::D2, S::Float { filterable: false }),
                uniform(14),
            ],
        });

        let mut composer = crate::shaders::build_composer();
        let shader = crate::shaders::make_module(
            device,
            &mut composer,
            "tw_water_shader",
            include_str!("tw_water.wgsl"),
            "water_tw/tw_water.wgsl",
        );
        let linear = if surface_format == wgpu::TextureFormat::Rgba16Float { 1.0 } else { 0.0 };
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_water_layout"),
            bind_group_layouts: &[Some(camera_layout), Some(&layout)],
            immediate_size: 0,
        });
        let grid_attrs = wgpu::vertex_attr_array![0 => Float32x2];
        let node_attrs = wgpu::vertex_attr_array![1 => Float32x4];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_water_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_tidewater"),
                compilation_options: Default::default(),
                buffers: &[
                    wgpu::VertexBufferLayout {
                        array_stride: 8,
                        step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &grid_attrs,
                    },
                    wgpu::VertexBufferLayout {
                        array_stride: 16,
                        step_mode: wgpu::VertexStepMode::Instance,
                        attributes: &node_attrs,
                    },
                ],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                // WaterMaterial side: 'double' (the surface is seen from below as well)
                cull_mode: None,
                ..Default::default()
            },
            // The water pass attaches depth READ-ONLY (lib.rs): the sea depth-tests against the
            // coast like Current OP but cannot write depth there. Tidewater writes depth to
            // reject hidden wave faces; front-to-back node order keeps most of that benefit.
            depth_stencil: Some(wgpu::DepthStencilState {
                format: crate::gfx3d::depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: sample_count, ..Default::default() },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_tidewater"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &[("linear", linear)],
                    ..Default::default()
                },
                // WaterMaterial: blending 'none' — the material composites the scene itself.
                targets: &[Some(wgpu::ColorTargetState {
                    format: surface_format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });

        let (verts, indices) = cdlod::grid_mesh();
        let grid_vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_grid_vertices"),
            contents: bytemuck::cast_slice(&verts),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let grid_ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("tw_grid_indices"),
            contents: bytemuck::cast_slice(&indices),
            usage: wgpu::BufferUsages::INDEX,
        });
        let instance_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_nodes"),
            size: (cdlod::MAX_INSTANCES * 16) as u64,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let mut me = Self {
            bind: device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("tw_group1_placeholder"),
                layout: &device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                    label: Some("tw_empty"),
                    entries: &[],
                }),
                entries: &[],
            }),
            fft,
            cdlod,
            surface_ubo,
            conform_ubo,
            foam_view,
            detail_view,
            smp_linear_repeat,
            smp_aniso_repeat,
            smp_linear_clamp,
            env_sampler,
            scene_view,
            depth_view,
            env_view,
            heightmap_view,
            layout,
            bind_dirty: true,
            pipeline,
            grid_vbuf,
            grid_ibuf,
            grid_index_count: indices.len() as u32,
            instance_buf,
            instance_count: 0,
            last_params: None,
            last_time: None,
            detail_offset: [0.0; 2],
            gens: [u64::MAX; 4],
        };
        me.rebuild_bind(device);
        me
    }

    fn rebuild_bind(&mut self, device: &wgpu::Device) {
        use wgpu::BindingResource as R;
        self.bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_group1"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.fft.params_buffer().as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: self.surface_ubo.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: R::TextureView(self.fft.displacement_view()) },
                wgpu::BindGroupEntry { binding: 3, resource: R::TextureView(self.fft.derivatives_view()) },
                wgpu::BindGroupEntry { binding: 4, resource: R::Sampler(&self.smp_linear_repeat) },
                wgpu::BindGroupEntry { binding: 5, resource: R::Sampler(&self.smp_aniso_repeat) },
                wgpu::BindGroupEntry { binding: 6, resource: R::TextureView(&self.foam_view) },
                wgpu::BindGroupEntry { binding: 7, resource: R::TextureView(&self.detail_view) },
                wgpu::BindGroupEntry { binding: 8, resource: R::TextureView(&self.scene_view) },
                wgpu::BindGroupEntry { binding: 9, resource: R::TextureView(&self.depth_view) },
                wgpu::BindGroupEntry { binding: 10, resource: R::Sampler(&self.smp_linear_clamp) },
                wgpu::BindGroupEntry { binding: 11, resource: R::TextureView(&self.env_view) },
                wgpu::BindGroupEntry { binding: 12, resource: R::Sampler(&self.env_sampler) },
                wgpu::BindGroupEntry { binding: 13, resource: R::TextureView(&self.heightmap_view) },
                wgpu::BindGroupEntry { binding: 14, resource: self.conform_ubo.as_entire_binding() },
            ],
        });
        self.bind_dirty = false;
    }

    pub fn set_params(&mut self, _queue: &wgpu::Queue, params: WgrWaterParams) {
        self.last_params = Some(params);
    }

    pub fn ocean_level(&self) -> Option<f32> {
        self.last_params.map(|p| p.sea_level)
    }

    /// `(local surface level, water clock, eye submersion depth)`, same meaning as Current OP.
    pub fn surface_params(&self) -> Option<(f32, f32, f32)> {
        self.last_params.map(|p| {
            let level = if p.optics_params[3] > 0.0 { p.optics_params[2] } else { p.sea_level };
            (level, p.time, p.fft_control[3].max(0.0))
        })
    }

    fn set_view(&mut self, slot: usize, view_gen: u64) -> bool {
        if self.gens[slot] == view_gen {
            return false;
        }
        self.gens[slot] = view_gen;
        self.bind_dirty = true;
        true
    }

    pub fn set_depth_view(&mut self, device: &wgpu::Device, depth: &wgpu::TextureView, view_gen: u64) {
        if self.set_view(0, view_gen) {
            self.depth_view = depth.clone();
            self.rebuild_bind(device);
        }
    }

    pub fn set_env_view(&mut self, device: &wgpu::Device, env: &wgpu::TextureView, view_gen: u64) {
        if self.set_view(1, view_gen) {
            self.env_view = env.clone();
            self.rebuild_bind(device);
        }
    }

    pub fn set_scene_view(&mut self, device: &wgpu::Device, scene: &wgpu::TextureView, view_gen: u64) {
        if self.set_view(2, view_gen) {
            self.scene_view = scene.clone();
            self.rebuild_bind(device);
        }
    }

    pub fn set_heightmap(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        heightmap: &wgpu::TextureView,
        view_gen: u64,
        params: &crate::terrain::TerrainConformParams,
    ) {
        queue.write_buffer(&self.conform_ubo, 0, bytemuck::bytes_of(params));
        if self.set_view(3, view_gen) {
            self.heightmap_view = heightmap.clone();
            self.rebuild_bind(device);
        }
    }

    /// CDLOD selection for this frame's water camera, plus the per-frame uniforms.
    pub fn prepare(&mut self, _device: &wgpu::Device, queue: &wgpu::Queue, camera: Option<&WgrCamera>) {
        let (Some(p), Some(cam)) = (self.last_params, camera) else {
            self.instance_count = 0;
            return;
        };
        // water clock, pause-coherent like Current OP's (Glob.time)
        let dt = self.last_time.map_or(0.0, |t| (p.time - t).clamp(0.0, 0.1));
        self.last_time = Some(p.time);
        // SeaDetail.update: gust patterns drift downwind at ~0.7 x the wind speed
        let block0 = surface_block(&p, self.detail_offset, &self.cdlod.morph);
        self.detail_offset[0] += block0.wind[0] * block0.sea[2] * 0.7 * dt;
        self.detail_offset[1] += block0.wind[1] * block0.sea[2] * 0.7 * dt;
        let block = surface_block(&p, self.detail_offset, &self.cdlod.morph);
        queue.write_buffer(&self.surface_ubo, 0, bytemuck::bytes_of(&block));

        let spec = fft::SpectrumInputs::from_params(&p);
        self.fft.set_frame(queue, spec, p.tidewater[0][1], p.time, dt);

        let cam_pos = glam::DVec3::new(cam.cam_pos[0] as f64, cam.cam_pos[1] as f64, cam.cam_pos[2] as f64);
        let clip = glam::Mat4::from_cols_array(&cam.proj) * glam::Mat4::from_cols_array(&cam.view);
        let planes = cdlod::SidePlanes::from_clip(clip);
        let nodes = cdlod::Selection::new(&self.cdlod, cam_pos, p.sea_level, Some(planes)).run();
        self.instance_count = nodes.len() as u32;
        if !nodes.is_empty() {
            queue.write_buffer(&self.instance_buf, 0, bytemuck::cast_slice(&nodes));
        }
    }

    /// Per-frame compute before the water draw: the FFT and its mip chain.
    pub fn update(&mut self, encoder: &mut wgpu::CommandEncoder, timers: &crate::gpu_timers::GpuTimers) {
        if self.last_params.is_some() {
            self.fft.dispatch(encoder, timers);
        }
    }

    pub fn draw(&self, pass: &mut wgpu::RenderPass<'_>, camera: &wgpu::BindGroup, offset: u32) {
        if self.instance_count == 0 || self.bind_dirty {
            return;
        }
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, camera, &[offset]);
        pass.set_bind_group(1, &self.bind, &[]);
        pass.set_vertex_buffer(0, self.grid_vbuf.slice(..));
        pass.set_vertex_buffer(1, self.instance_buf.slice(..));
        pass.set_index_buffer(self.grid_ibuf.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed(0..self.grid_index_count, 0, 0..self.instance_count);
    }

    /// Triangles submitted by the last draw (for the §7 harness).
    #[allow(dead_code)]
    pub fn submitted_triangles(&self) -> u64 {
        self.instance_count as u64 * (self.grid_index_count as u64 / 3)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn surface_block_layout_matches_the_wgsl() {
        assert_eq!(std::mem::offset_of!(TwSurface, morph), 128);
        assert_eq!(std::mem::size_of::<TwSurface>(), 320);
    }

    #[test]
    fn tidewater_water_shader_composes() {
        let mut composer = crate::shaders::build_composer();
        if let Err(e) = composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
            source: include_str!("tw_water.wgsl"),
            file_path: "water_tw/tw_water.wgsl",
            ..Default::default()
        }) {
            panic!("{}", e.emit_to_string(&composer));
        }
    }

    #[test]
    fn default_params_are_tidewaters() {
        assert_eq!(DEFAULT_PARAMS[0][1], 0.9); // OceanFFT choppiness
        assert_eq!(DEFAULT_PARAMS[0][2], 0.48); // swell system scale
        assert_eq!(DEFAULT_PARAMS[2][1], 0.035); // WaterMaterial waterRoughness
        assert_eq!(DEFAULT_PARAMS[2][3], 0.035); // WaterMaterial backscatter
        assert_eq!(DEFAULT_PARAMS[3][2], 0.3); // SeaDetail streakAmount
    }
}
