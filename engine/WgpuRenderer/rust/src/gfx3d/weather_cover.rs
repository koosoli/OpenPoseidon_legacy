//! Dedicated physical vertical depth. No artistic AO extent, kernel or publication reuse.
use super::{sky_vis, weather_cover_policy as policy};

pub const CULL_VIEW: usize = sky_vis::DIRECTION_COUNT + 1; // GI owns index5
pub const RESOLUTION: u32 = 2048;
pub const EXTENT: f32 = 640.0;
pub const HEIGHT: f32 = 1024.0;
pub const BIAS_METRES: f32 = 0.08;

fn physical_view(camera: glam::Vec3) -> sky_vis::SkyVisView {
    let settings = sky_vis::SkyVisSettings {
        extent: EXTENT,
        height: HEIGHT,
        resolution: RESOLUTION,
        kernel: 0.0,
        bias: BIAS_METRES,
        ..Default::default()
    };
    sky_vis::build_view_for(camera, glam::Vec3::Y, &settings)
}

pub struct Map {
    pub target: Option<(wgpu::Texture, wgpu::TextureView)>,
    pub dummy: wgpu::TextureView,
    pub generation: u64,
    pub frame: u64,
    pub view: Option<sky_vis::SkyVisView>,
    pub identity: policy::Identity,
    pub publication: std::cell::Cell<policy::Publication>,
    pub queue: Option<wgpu::Queue>,
    pub ready_offsets: Vec<u64>,
    pub camera_offset: u32,
    pub direct: Vec<(crate::ffi::WgrDraw3D, u32)>,
    pub direct_complete: bool,
    pub trace: bool,
    pub trace_rows: std::cell::Cell<u32>,
}

fn texture(
    device: &wgpu::Device,
    resolution: u32,
    label: &'static str,
) -> (wgpu::Texture, wgpu::TextureView) {
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d {
            width: resolution,
            height: resolution,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let view = texture.create_view(&Default::default());
    (texture, view)
}

impl Map {
    pub fn new(device: &wgpu::Device) -> Self {
        let (_, dummy) = texture(device, 1, "wgr_weather_cover_dummy");
        Self {
            target: None,
            dummy,
            generation: 0,
            frame: 0,
            view: None,
            identity: policy::Identity {
                frame: 0,
                instance_epoch: 0,
                image: 0,
            },
            publication: Default::default(),
            queue: None,
            ready_offsets: Vec::new(),
            camera_offset: 0,
            direct: Vec::new(),
            direct_complete: false,
            trace: std::env::var("WGR_WEATHER_COVER_TRACE").as_deref() == Ok("1"),
            trace_rows: std::cell::Cell::new(0),
        }
    }
    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        camera: glam::Vec3,
        epoch: u64,
        snow: [f32; 4],
        ground: [f32; 4],
        retained: bool,
    ) {
        self.frame = self.frame.wrapping_add(1);
        self.ready_offsets.clear();
        self.direct.clear();
        self.direct_complete = true;
        let enabled = std::env::var("WGR_WEATHER_COVER").as_deref() != Ok("0");
        if !policy::active(enabled, snow, ground) || !camera.is_finite() {
            if self.target.take().is_some() {
                self.generation = self.generation.wrapping_add(1);
            }
            self.view = None;
            self.publication.get_mut().abort();
            return;
        }
        if self.target.is_none() {
            self.target = Some(texture(device, RESOLUTION, "wgr_physical_weather_cover"));
            self.generation = self.generation.wrapping_add(1);
        }
        self.view = Some(physical_view(camera));
        self.identity = policy::Identity {
            frame: self.frame,
            instance_epoch: epoch,
            image: self.generation,
        };
        self.publication.get_mut().plan(self.identity, retained);
        self.queue = Some(queue.clone());
    }
    pub fn params(&self, ready: bool) -> [f32; 4] {
        [
            if ready { 1.0 } else { 0.0 },
            BIAS_METRES / (2.0 * HEIGHT),
            1.5 / RESOLUTION as f32,
            if self.view.is_some() { 1.0 } else { 0.0 },
        ]
    }
    pub fn invalidate(&self, camera: Option<&wgpu::Buffer>) {
        let mut p = self.publication.get();
        p.abort();
        self.publication.set(p);
        if let (Some(queue), Some(buffer)) = (&self.queue, camera) {
            for &offset in &self.ready_offsets {
                queue.write_buffer(buffer, offset, bytemuck::cast_slice(&self.params(false)));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn weather_cover_actual_view_covers_distant_world_roofs_and_matches_relative_cull() {
        let camera = glam::Vec3::new(12000.3, 712.5, 9400.7);
        let view = physical_view(camera);
        for distance in [200.0, 500.0] {
            let point = camera + glam::Vec3::new(distance, 12.0, -distance);
            let absolute = view.view_proj * point.extend(1.0);
            let relative = view.view_proj
                * glam::Mat4::from_translation(camera)
                * (point - camera).extend(1.0);
            assert!((absolute - relative).abs().max_element() < 2e-5);
            assert!(
                absolute.x.abs() < 1.0
                    && absolute.y.abs() < 1.0
                    && absolute.z > 0.0
                    && absolute.z < 1.0
            );
            let covered = view.view_proj * (point - glam::Vec3::Y * 0.2).extend(1.0);
            assert!(
                covered.z - absolute.z > BIAS_METRES / (2.0 * HEIGHT),
                "20cm below roof must exceed actual 8cm comparison bias"
            );
        }
    }
}

// Original vertex programs preserve current wind, conform and pose. Only clip-space
// projection is specialised; no private fragment storage or public C ABI is added.
pub fn depth_pipeline(
    device: &wgpu::Device,
    shader: &wgpu::ShaderModule,
    layout: &wgpu::PipelineLayout,
    buffers: &[wgpu::VertexBufferLayout<'_>],
    vs: &'static str,
    fs: &'static str,
    alpha: f32,
) -> wgpu::RenderPipeline {
    let constants = [
        ("weather_depth", 1.0),
        ("alpha_ref", alpha as f64),
        ("depth_bias", 0.0),
        ("cutout_mip_alpha", super::cutout_mip_alpha() as f64),
    ];
    // GPU entry has no alpha_ref/depth_bias overrides; direct and retained use
    // distinct option lists, otherwise wgpu rejects unknown constants.
    let retained_constants = [
        ("weather_depth", 1.0),
        ("cutout_mip_alpha", super::cutout_mip_alpha() as f64),
    ];
    let values = if vs == "vs_gpu" {
        &retained_constants[..]
    } else {
        &constants[..]
    };
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("wgr_physical_weather_depth"),
        layout: Some(layout),
        vertex: wgpu::VertexState {
            module: shader,
            entry_point: Some(vs),
            buffers,
            compilation_options: wgpu::PipelineCompilationOptions {
                constants: values,
                ..Default::default()
            },
        },
        primitive: wgpu::PrimitiveState {
            front_face: wgpu::FrontFace::Cw,
            cull_mode: None,
            ..Default::default()
        }, // an external roof can be single-sided
        depth_stencil: Some(wgpu::DepthStencilState {
            format: wgpu::TextureFormat::Depth32Float,
            depth_write_enabled: Some(true),
            depth_compare: Some(wgpu::CompareFunction::LessEqual),
            stencil: Default::default(),
            bias: Default::default(),
        }),
        multisample: Default::default(),
        fragment: Some(wgpu::FragmentState {
            module: shader,
            entry_point: Some(fs),
            targets: &[],
            compilation_options: wgpu::PipelineCompilationOptions {
                constants: values,
                ..Default::default()
            },
        }),
        multiview_mask: None,
        cache: None,
    })
}

pub fn retained_pipeline(
    device: &wgpu::Device,
    composer: &mut naga_oil::compose::Composer,
    camera: &wgpu::BindGroupLayout,
    instances: &wgpu::BindGroupLayout,
    textures: &wgpu::BindGroupLayout,
    samplers: &wgpu::BindGroupLayout,
    conform: &wgpu::BindGroupLayout,
) -> wgpu::RenderPipeline {
    let module = crate::shaders::make_module(
        device,
        composer,
        "weather_retained_depth",
        include_str!("gpu_driven.wgsl"),
        "gfx3d/gpu_driven.wgsl",
    );
    let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("wgr_weather_retained_layout"),
        bind_group_layouts: &[
            Some(camera),
            Some(instances),
            Some(textures),
            Some(samplers),
            Some(conform),
        ],
        immediate_size: 0,
    });
    let attributes = [
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x3,
            offset: 0,
            shader_location: 0,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x3,
            offset: 12,
            shader_location: 1,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x2,
            offset: 24,
            shader_location: 2,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Uint32,
            offset: 32,
            shader_location: 5,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x2,
            offset: 60,
            shader_location: 7,
        },
    ];
    depth_pipeline(
        device,
        &module,
        &layout,
        &[wgpu::VertexBufferLayout {
            array_stride: super::BAKED_VERT_SIZE,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &attributes,
        }],
        "vs_gpu",
        "fs_gpu_weather_depth",
        0.0,
    )
}
