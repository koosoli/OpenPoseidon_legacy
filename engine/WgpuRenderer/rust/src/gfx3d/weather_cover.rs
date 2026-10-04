//! Dedicated physical vertical depth. No artistic AO extent, kernel or publication reuse.
use super::{sky_vis, weather_cover_policy as policy};

pub const CULL_VIEW: usize = sky_vis::DIRECTION_COUNT + 1; // GI owns index5
pub const RESOLUTION: u32 = 2048;
pub const EXTENT: f32 = 640.0;
pub const FAR_EXTENT: f32 = 1600.0;
pub const FAR_CULL_VIEW: usize = CULL_VIEW + 1;
pub const HEIGHT: f32 = 1024.0;
pub const BIAS_METRES: f32 = 0.08;

fn cascade_enabled(master: Option<&str>, far: bool, override_value: Option<&str>) -> bool {
    master != Some("0") && (!far || override_value.is_none() || override_value == Some("1"))
}

fn physical_view(camera: glam::Vec3) -> sky_vis::SkyVisView {
    physical_view_extent(camera, EXTENT)
}
fn physical_view_extent(camera: glam::Vec3, extent: f32) -> sky_vis::SkyVisView {
    let settings = sky_vis::SkyVisSettings {
        extent,
        height: HEIGHT,
        resolution: RESOLUTION,
        kernel: 0.0,
        bias: BIAS_METRES,
        ..Default::default()
    };
    sky_vis::build_view_for(camera, glam::Vec3::Y, &settings)
}

pub struct Map {
    pub far: bool,
    pub extent: f32,
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
    pub trace_view: std::cell::Cell<Option<[f32;16]>>,
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
        Self::new_cascade(device, false)
    }
    pub fn new_cascade(device: &wgpu::Device, far: bool) -> Self {
        if far {
            let on = cascade_enabled(std::env::var("WGR_WEATHER_COVER").ok().as_deref(), true,
                std::env::var("WGR_WEATHER_COVER_FAR").ok().as_deref());
            eprintln!("[wgr] weather cover far cascade: enabled={} default-on WGR_WEATHER_COVER_FAR=0 disables resolution={} extent={} depthBytes={} scope=registered-retained-and-current-direct",
                on, RESOLUTION, FAR_EXTENT, RESOLUTION as u64 * RESOLUTION as u64 * 4);
        }
        let (_, dummy) = texture(device, 1, "wgr_weather_cover_dummy");
        Self {
            far,
            extent: if far { FAR_EXTENT } else { EXTENT },
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
            trace_view: std::cell::Cell::new(None),
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
        layer_fog: bool,
    ) {
        self.frame = self.frame.wrapping_add(1);
        self.ready_offsets.clear();
        self.direct.clear();
        self.direct_complete = true;
        let enabled = cascade_enabled(std::env::var("WGR_WEATHER_COVER").ok().as_deref(), self.far,
            std::env::var("WGR_WEATHER_COVER_FAR").ok().as_deref());
        if !(policy::active(enabled, snow, ground) || (enabled && layer_fog)) || !camera.is_finite() {
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
        self.view = Some(physical_view_extent(camera, self.extent));
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
    pub fn trace_changed(&self) -> bool {
        let matrix = self.view.map(|v|v.view_proj.to_cols_array());
        let changed = self.trace_view.get() != matrix;
        if changed { self.trace_view.set(matrix); }
        self.trace && self.view.is_some() && (changed || self.trace_rows.get() < 32)
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
    fn weather_cover_far_defaults_on_and_obeys_exact_override_and_master_off() {
        assert!(cascade_enabled(None,true,None));
        for value in [Some("0"),Some("true"),Some("2"),Some("")] {
            assert!(!cascade_enabled(None,true,value));
            assert!(cascade_enabled(None,false,value));
        }
        assert!(cascade_enabled(None,true,Some("1")));
        assert!(!cascade_enabled(Some("0"),true,Some("1")));
        assert!(!cascade_enabled(Some("0"),true,None));
        assert!(!cascade_enabled(Some("0"),false,None));
        assert_eq!(FAR_CULL_VIEW,CULL_VIEW+1);
        assert_eq!(RESOLUTION as u64*RESOLUTION as u64*4,16777216);
    }
    #[test]
    fn weather_cover_far_view_covers_audited_eden_rear_rows_without_changing_near_precision() {
        let camera = glam::Vec3::new(5025.0,144.54546,3725.0);
        let near = physical_view(camera);
        let far = physical_view_extent(camera,FAR_EXTENT);
        for point in [glam::Vec3::new(5027.1,105.0,4427.1),
            glam::Vec3::new(5025.0,120.0,4475.0)] {
            let n = near.view_proj * point.extend(1.0);
            let f = far.view_proj * point.extend(1.0);
            assert!(n.y.abs() > 1.0, "actual rear rows must be outside near proof");
            assert!(f.x.abs() < 1.0 && f.y.abs() < 1.0 && f.z > 0.0 && f.z < 1.0);
            let relative = far.view_proj * glam::Mat4::from_translation(camera)
                * (point-camera).extend(1.0);
            assert!((relative-f).abs().max_element() < 2e-5);
            let below = far.view_proj * (point-glam::Vec3::Y*0.2).extend(1.0);
            assert!(below.z-f.z > BIAS_METRES/(2.0*HEIGHT));
        }
        assert_eq!(2.0*EXTENT/RESOLUTION as f32,0.625);
        assert_eq!(2.0*FAR_EXTENT/RESOLUTION as f32,1.5625);
        let outside = far.view_proj * (camera+glam::Vec3::X*1800.0).extend(1.0);
        assert!(outside.x > 1.0, "far cascade remains a bounded source footprint");
    }
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
    far: bool,
) -> wgpu::RenderPipeline {
    let constants = [
        ("weather_depth", if far {2.0} else {1.0}),
        ("alpha_ref", alpha as f64),
        ("depth_bias", 0.0),
        ("cutout_mip_alpha", super::cutout_mip_alpha() as f64),
    ];
    // GPU entry has no alpha_ref/depth_bias overrides; direct and retained use
    // distinct option lists, otherwise wgpu rejects unknown constants.
    let retained_constants = [
        ("weather_depth", if far {2.0} else {1.0}),
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
    far: bool,
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
        far,
    )
}
