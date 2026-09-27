//! Tidewater's post/Underwater.js — the view from under the surface — ported from
//! dgreenheck/tidewater @ 4811ba48 (MIT, © DRG Software Solutions LLC). W5a.
//!
//! One full-screen pass in OP's backend underwater slot (`WaterBackend::render_underwater`: it
//! runs while the renderer's underwater compositor is engaged, on the resolved HDR scene and
//! the single-sample depth, before depth of field, bloom and the tonemap): the medium at the lens
//! (the waterline split where the surface crosses the near plane), Beer-Lambert absorption and
//! the single-scattered sun and sky light of the water between the lens and the scene, and the
//! meniscus band along the waterline (`tw_underwater.wgsl`). The surface seen from below (Snell's
//! window, total internal reflection) is the water material's own (tw_water.wgsl).
//!
//! What OP changes: see the header of `tw_underwater.wgsl` (per-pixel medium instead of a
//! medium pass, FFT-only water height, submerged scene lighting attenuated in post, no caustic
//! shafts / torch yet: W5b).

use crate::ffi::{WgrCamera, WgrWaterParams};

/// The composite and the caustic lookup it uses (W5b).
const UW_WGSL: &str = concat!(include_str!("tw_underwater.wgsl"), "\n", include_str!("tw_caustics.wgsl"));

/// Meniscus half-width in pixels (Tidewater's `band`).
pub const BAND_PX: f32 = 9.0;
/// Lowest in-scatter scale under water (see W3j.1: the water glow, at least this).
pub const MIN_GLOW: f32 = 0.12;

/// `UwParams` in tw_underwater.wgsl.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
pub struct UwParams {
    pub inv_view_proj: [f32; 16],
    pub cam: [f32; 4],
    pub sun_dir: [f32; 4],
    pub sun_e: [f32; 4],
    pub sky_e: [f32; 4],
    pub absorption: [f32; 4],
    pub scattering: [f32; 4],
    pub ocean: [f32; 4],
    pub sizes: [f32; 4],
}

fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 { c / 12.92 } else { ((c + 0.055) / 1.055).powf(2.4) }
}

impl UwParams {
    /// This frame's block from the water camera and the water parameters. `linear`: the water
    /// pipelines' linear-lighting override; `glow`: the Water tab's water glow; `sizes`: the FFT
    /// cascade sizes; `absorption` / `scattering`: the water's coefficients (1/m).
    #[allow(clippy::too_many_arguments)]
    pub fn new(cam: &WgrCamera, p: &WgrWaterParams, linear: bool, glow: f32, sizes: [f32; 4], caustic_tiles: [f32; 2], absorption: [f32; 3], scattering: [f32; 3], enabled: bool) -> Self {
        let view = glam::DMat4::from_cols_array(&cam.view.map(f64::from));
        let proj = glam::DMat4::from_cols_array(&cam.proj.map(f64::from));
        let inv_vp = (view.inverse() * proj.inverse()).as_mat4().to_cols_array();
        let near = cam.proj[14].abs().clamp(0.01, 2.0);
        let d = cam.sun_dir_world;
        let dl = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt().max(1e-6);
        let conv = linear && cam.sun_diffuse[3] <= 0.5;
        let lin = |c: f32| if conv { srgb_to_linear(c) } else { c };
        let pi = std::f32::consts::PI;
        Self {
            inv_view_proj: inv_vp,
            cam: [cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2], near],
            sun_dir: [-d[0] / dl, -d[1] / dl, -d[2] / dl, glow.max(MIN_GLOW)],
            sun_e: [lin(cam.sun_diffuse[0]) * pi, lin(cam.sun_diffuse[1]) * pi, lin(cam.sun_diffuse[2]) * pi, BAND_PX],
            sky_e: [lin(cam.sun_ambient[0]), lin(cam.sun_ambient[1]), lin(cam.sun_ambient[2]), if enabled { 1.0 } else { 0.0 }],
            absorption: [absorption[0], absorption[1], absorption[2], 0.0],
            scattering: [scattering[0], scattering[1], scattering[2], p.sea_level],
            ocean: [p.tidewater[0][0], 4.0, caustic_tiles[0], caustic_tiles[1]],
            sizes,
        }
    }
}

pub struct TwUnderwater {
    layout: wgpu::BindGroupLayout,
    pipeline: wgpu::RenderPipeline,
    params: wgpu::Buffer,
    repeat: wgpu::Sampler,
    clamp: wgpu::Sampler,
}

impl TwUnderwater {
    pub fn new(device: &wgpu::Device, target_format: wgpu::TextureFormat) -> Self {
        let f = wgpu::ShaderStages::FRAGMENT;
        let tex = |binding, sample_type, dim| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: f,
            ty: wgpu::BindingType::Texture { sample_type, view_dimension: dim, multisampled: false },
            count: None,
        };
        use wgpu::TextureSampleType as S;
        use wgpu::TextureViewDimension as D;
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("tw_underwater"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: f,
                    ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
                    count: None,
                },
                tex(1, S::Float { filterable: true }, D::D2),
                tex(2, S::Depth, D::D2),
                tex(3, S::Float { filterable: true }, D::D2Array),
                wgpu::BindGroupLayoutEntry { binding: 4, visibility: f, ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
                wgpu::BindGroupLayoutEntry { binding: 5, visibility: f, ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
                tex(6, S::Float { filterable: true }, D::D2),
                tex(7, S::Float { filterable: true }, D::D2),
            ],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("tw_underwater"),
            source: wgpu::ShaderSource::Wgsl(UW_WGSL.into()),
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tw_underwater"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("tw_underwater"),
            layout: Some(&pl),
            vertex: wgpu::VertexState { module: &module, entry_point: Some("vs_uw"), compilation_options: Default::default(), buffers: &[] },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_uw"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format: target_format, blend: None, write_mask: wgpu::ColorWrites::ALL })],
            }),
            multiview_mask: None,
            cache: None,
        });
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tw_underwater_params"),
            size: std::mem::size_of::<UwParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let repeat = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_underwater_repeat"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let clamp = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tw_underwater_clamp"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        Self { layout, pipeline, params, repeat, clamp }
    }

    pub fn set_frame(&self, queue: &wgpu::Queue, params: &UwParams) {
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(params));
    }

    #[allow(clippy::too_many_arguments)]
    pub fn render(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        destination: &wgpu::TextureView,
        displacement: &wgpu::TextureView,
        caustics: (&wgpu::TextureView, &wgpu::TextureView),
    ) {
        use wgpu::BindingResource as R;
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("tw_underwater"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.params.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: R::TextureView(source) },
                wgpu::BindGroupEntry { binding: 2, resource: R::TextureView(depth) },
                wgpu::BindGroupEntry { binding: 3, resource: R::TextureView(displacement) },
                wgpu::BindGroupEntry { binding: 4, resource: R::Sampler(&self.repeat) },
                wgpu::BindGroupEntry { binding: 5, resource: R::Sampler(&self.clamp) },
                wgpu::BindGroupEntry { binding: 6, resource: R::TextureView(caustics.0) },
                wgpu::BindGroupEntry { binding: 7, resource: R::TextureView(caustics.1) },
            ],
        });
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("tw_underwater"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: destination,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.draw(0..3, 0..1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn params_block_matches_the_wgsl() {
        assert_eq!(std::mem::size_of::<UwParams>(), 64 + 8 * 16);
    }

    #[test]
    fn underwater_wgsl_validates() {
        let module = naga::front::wgsl::parse_str(UW_WGSL).expect("parse");
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("valid");
    }
}
