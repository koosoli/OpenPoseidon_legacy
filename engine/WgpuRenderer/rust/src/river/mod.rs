//! RFG-162: native Reforger rivers drawn as water (see river.wgsl).
//!
//! The loader hands over one mesh for all of a world's rivers (`wgr_river_set_mesh`): their
//! ribbons, level across their width at the authored water level, with flow coordinates per
//! vertex. They are drawn in the water pass, after the sea, sampling the same frozen scene
//! snapshot and resolved depth the sea refracts through, and the sky's reflection env map.
//! Nothing is allocated until a world sends rivers; an empty mesh releases them.

use crate::gfx3d::depth_format;

/// One river vertex, 32 bytes. Mirrors `VsIn` in river.wgsl and the 8 floats per vertex the
/// C++ loader writes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct RiverVertex {
    pub pos: [f32; 3],
    pub along: f32,
    pub flow: [f32; 2],
    pub across: f32,
    pub half_width: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
struct RiverParams {
    colour: [f32; 4],
    flow: [f32; 4],
    optics: [f32; 4],
    misc: [f32; 4],
    bed: [f32; 4],
}

/// The look the loader read from the river's MatRiver (see `RiverLook::from_floats`).
#[derive(Clone, Copy, Debug)]
pub struct RiverLook {
    pub colour: [f32; 3],
    pub half_light: f32,
    pub flow_speed: f32,
    pub fresnel: f32,
    pub sun_power: f32,
    pub sun_influence: f32,
    pub wet_floor: f32,
}

impl Default for RiverLook {
    fn default() -> Self {
        // RiverSurface_01.emat (Everon's rivers).
        Self {
            colour: [0.035, 0.04, 0.019],
            half_light: 30.0,
            flow_speed: 0.2,
            fresnel: 0.02,
            sun_power: 1024.0,
            sun_influence: 0.025,
            wet_floor: 0.3,
        }
    }
}

impl RiverLook {
    /// `[r, g, b, HalfLightDistance, FlowSpeed, Fresnel, SunPower, SunInfluence, WetFloor]`;
    /// missing or non-finite entries keep the RiverSurface_01 values.
    pub fn from_floats(v: &[f32]) -> Self {
        let mut look = Self::default();
        let get = |i: usize| v.get(i).copied().filter(|x| x.is_finite());
        for c in 0..3 {
            if let Some(x) = get(c) {
                look.colour[c] = x.clamp(0.0, 1.0);
            }
        }
        if let Some(x) = get(3) { look.half_light = x.clamp(0.5, 500.0); }
        if let Some(x) = get(4) { look.flow_speed = x.clamp(0.0, 5.0); }
        if let Some(x) = get(5) { look.fresnel = x.clamp(0.0, 1.0); }
        if let Some(x) = get(6) { look.sun_power = x.clamp(8.0, 8192.0); }
        if let Some(x) = get(7) { look.sun_influence = x.clamp(0.0, 4.0); }
        if let Some(x) = get(8) { look.wet_floor = x.clamp(0.0, 0.9); }
        look
    }
}

fn env_f32(key: &str, default: f32, lo: f32, hi: f32) -> f32 {
    std::env::var(key)
        .ok()
        .and_then(|v| v.trim().parse::<f32>().ok())
        .filter(|v| v.is_finite())
        .map_or(default, |v| v.clamp(lo, hi))
}

struct RiverMesh {
    vertices: wgpu::Buffer,
    indices: wgpu::Buffer,
    index_count: u32,
}

pub struct River {
    params: RiverParams,
    params_buffer: wgpu::Buffer,
    layout: wgpu::BindGroupLayout,
    pipeline: wgpu::RenderPipeline,
    scene_sampler: wgpu::Sampler,
    env_sampler: wgpu::Sampler,
    mesh: Option<RiverMesh>,
    bind: Option<(wgpu::BindGroup, (u64, u64))>,
    start: std::time::Instant,
}

impl River {
    pub fn new(
        device: &wgpu::Device,
        camera_layout: &wgpu::BindGroupLayout,
        color_format: wgpu::TextureFormat,
        sample_count: u32,
        composer: &mut naga_oil::compose::Composer,
    ) -> Self {
        let params_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_river_params"),
            size: std::mem::size_of::<RiverParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let texture = |binding: u32, sample_type: wgpu::TextureSampleType| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let sampler = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_river_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                texture(1, wgpu::TextureSampleType::Depth),
                texture(2, wgpu::TextureSampleType::Float { filterable: true }),
                sampler(3),
                texture(4, wgpu::TextureSampleType::Float { filterable: true }),
                sampler(5),
            ],
        });
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_river_shader",
            include_str!("river.wgsl"),
            "river/river.wgsl",
        );
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_river_pipeline_layout"),
            bind_group_layouts: &[Some(camera_layout), Some(&layout)],
            immediate_size: 0,
        });
        let attributes = wgpu::vertex_attr_array![
            0 => Float32x3, 1 => Float32, 2 => Float32x2, 3 => Float32, 4 => Float32
        ];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_river"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_river"),
                compilation_options: Default::default(),
                buffers: &[wgpu::VertexBufferLayout {
                    array_stride: std::mem::size_of::<RiverVertex>() as u64,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &attributes,
                }],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                // Seen from above only in practice, but a ribbon's winding follows the spline's
                // direction, which nothing guarantees: draw both sides.
                cull_mode: None,
                ..Default::default()
            },
            // The water pass's depth: reversed-Z, tested, never written -- the river is a film
            // over its bed, and the banks that cover its edges must keep winning.
            depth_stencil: Some(wgpu::DepthStencilState {
                format: depth_format(device),
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState {
                count: sample_count,
                ..Default::default()
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_river"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: color_format,
                    // Opaque: the shader composites the refracted scene itself.
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let linear = |label: &str, mode: wgpu::AddressMode| {
            device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some(label),
                address_mode_u: mode,
                address_mode_v: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Linear,
                min_filter: wgpu::FilterMode::Linear,
                ..Default::default()
            })
        };
        let mut river = Self {
            params: bytemuck::Zeroable::zeroed(),
            params_buffer,
            layout,
            pipeline,
            scene_sampler: linear("wgr_river_scene_samp", wgpu::AddressMode::ClampToEdge),
            env_sampler: linear("wgr_river_env_samp", wgpu::AddressMode::Repeat),
            mesh: None,
            bind: None,
            start: std::time::Instant::now(),
        };
        river.set_look(RiverLook::default());
        river
    }

    /// The look, from the river's MatRiver plus the WGR_RIVER_* overrides.
    pub fn set_look(&mut self, look: RiverLook) {
        self.params.colour = [look.colour[0], look.colour[1], look.colour[2], look.half_light];
        self.params.flow = [
            env_f32("WGR_RIVER_FLOW_SPEED", look.flow_speed.max(0.05) * 4.0, 0.0, 10.0),
            env_f32("WGR_RIVER_RIPPLE_TILING", 1.6, 0.05, 20.0),
            env_f32("WGR_RIVER_RIPPLE", 1.0, 0.0, 8.0),
            0.0,
        ];
        self.params.optics = [
            look.fresnel,
            look.sun_power,
            env_f32("WGR_RIVER_GLINT", look.sun_influence * 8.0, 0.0, 10.0),
            env_f32("WGR_RIVER_REFRACT", 0.06, 0.0, 1.0),
        ];
        self.params.misc = [
            env_f32("WGR_RIVER_SHORE", 0.04, 0.001, 1.0),
            env_f32("WGR_RIVER_SSR", 1.0, 0.0, 1.0),
            env_f32("WGR_RIVER_SCATTER", 0.6, 0.0, 4.0),
            env_f32("WGR_RIVER_DEBUG", 0.0, 0.0, 1.0),
        ];
        self.params.bed = [env_f32("WGR_RIVER_WET_FLOOR", look.wet_floor, 0.0, 0.9), 0.0, 0.0, 0.0];
    }

    /// Replace every river of the world. Empty releases them. Returns the triangle count.
    pub fn set_mesh(&mut self, device: &wgpu::Device, vertices: &[RiverVertex], indices: &[u32]) -> u32 {
        use wgpu::util::DeviceExt;
        self.mesh = None;
        let count = vertices.len() as u32;
        // Every index must name a vertex: a bad index is a GPU fault, not a bad picture.
        if vertices.is_empty() || indices.len() < 3 || indices.iter().any(|&i| i >= count) {
            return 0;
        }
        let usable = indices.len() / 3 * 3;
        let vb = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_river_vertices"),
            contents: bytemuck::cast_slice(vertices),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let ib = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_river_indices"),
            contents: bytemuck::cast_slice(&indices[..usable]),
            usage: wgpu::BufferUsages::INDEX,
        });
        self.mesh = Some(RiverMesh {
            vertices: vb,
            indices: ib,
            index_count: usable as u32,
        });
        (usable / 3) as u32
    }

    pub fn drawable(&self) -> bool {
        self.mesh.is_some() && std::env::var("WGR_RIVERS").map_or(true, |v| v != "0")
    }

    /// Bind this frame's scene snapshot, resolved depth and sky env map (rebuilt only when the
    /// snapshot or the depth target changed), and push the clock.
    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        scene: &wgpu::TextureView,
        scene_gen: u64,
        depth: &wgpu::TextureView,
        depth_gen: u64,
        env: &wgpu::TextureView,
    ) {
        self.params.flow[3] = (self.start.elapsed().as_secs_f64() % 3600.0) as f32;
        queue.write_buffer(&self.params_buffer, 0, bytemuck::bytes_of(&self.params));
        let key = (scene_gen, depth_gen);
        if self.bind.as_ref().is_some_and(|(_, k)| *k == key) {
            return;
        }
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_river_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.params_buffer.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(depth) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::TextureView(scene) },
                wgpu::BindGroupEntry { binding: 3, resource: wgpu::BindingResource::Sampler(&self.scene_sampler) },
                wgpu::BindGroupEntry { binding: 4, resource: wgpu::BindingResource::TextureView(env) },
                wgpu::BindGroupEntry { binding: 5, resource: wgpu::BindingResource::Sampler(&self.env_sampler) },
            ],
        });
        self.bind = Some((bind, key));
    }

    pub fn draw(&self, pass: &mut wgpu::RenderPass<'_>, camera_bind: &wgpu::BindGroup, camera_offset: u32) {
        let (Some(mesh), Some((bind, _))) = (self.mesh.as_ref(), self.bind.as_ref()) else {
            return;
        };
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, bind, &[]);
        pass.set_vertex_buffer(0, mesh.vertices.slice(..));
        pass.set_index_buffer(mesh.indices.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed(0..mesh.index_count, 0, 0..1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vertex_layout_is_the_eight_floats_the_loader_writes() {
        assert_eq!(std::mem::size_of::<RiverVertex>(), 32);
        assert_eq!(std::mem::offset_of!(RiverVertex, along), 12);
        assert_eq!(std::mem::offset_of!(RiverVertex, flow), 16);
        assert_eq!(std::mem::offset_of!(RiverVertex, across), 24);
        assert_eq!(std::mem::offset_of!(RiverVertex, half_width), 28);
    }

    #[test]
    fn look_defaults_are_river_surface_01_and_bad_values_keep_them() {
        let look = RiverLook::from_floats(&[f32::NAN, 0.5, 0.2, -3.0]);
        assert_eq!(look.colour[0], 0.035);
        assert_eq!(look.colour[1], 0.5);
        assert_eq!(look.half_light, 0.5);
        assert_eq!(look.fresnel, 0.02);
    }

    #[test]
    fn the_shader_composes_and_validates() {
        let mut composer = crate::shaders::build_composer();
        let module = composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source: include_str!("river.wgsl"),
                file_path: "river/river.wgsl",
                shader_defs: crate::shaders::shader_defs(),
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("river.wgsl: {}", e.emit_to_string(&composer)));
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("river.wgsl validates");
    }
}
