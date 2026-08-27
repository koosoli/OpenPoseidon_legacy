// Depth of field. A screenshot aid, off unless the Picture Mode tab turns it on.
//
// Structurally this is the smallest post pass in the renderer: one full-screen triangle reading
// the linear HDR colour and the single-sample depth, writing a new colour target that becomes the
// input to bloom. It deliberately mirrors `underwater.rs`, which does the same job with the same
// inputs, so there is one shape to learn rather than two.
//
// The interesting decisions are all in `dof.wgsl` -- read that for what the blur actually does and
// where the approximation shows.

use bytemuck::{Pod, Zeroable};
use wgpu::util::DeviceExt;

/// Mirrors `Params` in dof.wgsl. Two vec4s, so alignment is trivially correct on every backend.
#[repr(C)]
#[derive(Copy, Clone, Pod, Zeroable)]
struct Params {
    focus: [f32; 4],
    strength: [f32; 4],
    quality: [f32; 4],
}

/// What the Picture Mode tab sets. Metres and pixels, because those are the units a person
/// composing a shot can reason about -- "focus at 12 m, 40 px of blur" is checkable by eye in a
/// way an f-number and a sensor size would not be, given the game has neither.
#[derive(Copy, Clone, Debug)]
pub struct DofSettings {
    pub enabled: bool,
    /// Distance the sharp band is centred on, in metres.
    pub focus_distance: f32,
    /// Half-width of the fully sharp band, in metres. Everything within it is untouched.
    pub focus_range: f32,
    /// Widest circle of confusion, in pixels, at 1080p-equivalent. The blur saturates here.
    pub max_blur_pixels: f32,
    /// Independent scales for behind and in front of the focal plane. Foreground blur is far more
    /// intrusive than background blur, so it gets its own control rather than sharing one slider.
    pub background_scale: f32,
    pub foreground_scale: f32,
    /// How quickly the blur opens up past the sharp band, in 1/metres. Small values give a long
    /// gentle falloff, large values a hard cut.
    pub transition: f32,
    /// Camera near plane, in metres. NOT a look setting -- the shader turns reversed-Z depth into
    /// metres with `near / depth`, so this is part of the projection. It is pushed with the rest
    /// because the renderer has no other access to it; the engine owns the projection.
    pub near_plane: f32,
    /// 0 = normal, 1 = paint the circle of confusion, 2 = paint raw view distance.
    pub debug_view: f32,
    /// Samples per pixel. THE cost of this pass -- each is a colour fetch and a depth fetch.
    /// Low counts show the spiral as noise in smooth gradients; high counts are for stills.
    pub sample_count: f32,
    /// Bokeh: how strongly a sample brighter than the threshold outweighs its neighbours.
    /// 0 gives a flat average, which turns a bright point into a faint smear.
    pub bokeh_boost: f32,
    /// Linear HDR luminance above which a sample counts as a highlight.
    pub bokeh_threshold: f32,
    /// Aperture blades: 0 = a perfect circle, 5..9 = a polygon, as a real iris prints.
    pub aperture_blades: f32,
}

impl Default for DofSettings {
    fn default() -> Self {
        // A portrait-ish default: sharp from roughly 8 to 16 m, soft beyond. Chosen so that the
        // first time someone ticks the box they see the effect rather than wondering if it worked.
        Self {
            enabled: false,
            focus_distance: 12.0,
            focus_range: 4.0,
            max_blur_pixels: 24.0,
            background_scale: 1.0,
            foreground_scale: 0.6,
            transition: 0.05,
            near_plane: 0.05,
            debug_view: 0.0,
            sample_count: 48.0,
            bokeh_boost: 3.0,
            bokeh_threshold: 0.7,
            aperture_blades: 0.0,
        }
    }
}

pub struct DepthOfField {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    params: wgpu::Buffer,
}

impl DepthOfField {
    pub fn new(device: &wgpu::Device, format: wgpu::TextureFormat) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_dof_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("dof.wgsl").into()),
        });

        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_dof_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_dof_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });

        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_dof_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_dof_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_dof_params"),
            contents: bytemuck::bytes_of(&Params::zeroed()),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });

        Self {
            pipeline,
            layout,
            sampler,
            params,
        }
    }

    pub fn upload(&self, queue: &wgpu::Queue, settings: &DofSettings, height: u32) {
        // The blur radius is authored in pixels at a reference height, so a screenshot taken at
        // 4K does not come out sharper than the same shot at 1080p. Without this the effect
        // silently depends on window size, which is exactly the kind of thing that makes a photo
        // mode untrustworthy.
        let scale = (height.max(1) as f32) / 1080.0;
        let params = Params {
            focus: [
                settings.focus_distance.max(0.01),
                settings.focus_range.max(0.0),
                (settings.max_blur_pixels.max(0.0) * scale).min(128.0),
                settings.near_plane.max(1e-3),
            ],
            strength: [
                settings.background_scale.clamp(0.0, 4.0),
                settings.foreground_scale.clamp(0.0, 4.0),
                settings.transition.max(1e-4),
                settings.debug_view,
            ],
            quality: [
                settings.sample_count.clamp(4.0, 128.0),
                settings.bokeh_boost.clamp(0.0, 20.0),
                settings.bokeh_threshold.max(0.0),
                settings.aperture_blades.clamp(0.0, 12.0),
            ],
        };
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(&params));
    }

    pub fn render(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        destination: &wgpu::TextureView,
    ) {
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_dof_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(source),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: self.params.as_entire_binding(),
                },
            ],
        });

        encoder.push_debug_group("wgr_dof");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_dof"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: destination,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }
}
