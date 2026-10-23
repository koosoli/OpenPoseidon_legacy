//! REFLECT-001 phase D: GGX-prefiltered mip chain of the sky reflection env map.
//!
//! The env texture (Sky::env_tex) carries `ENV_MIPS` levels. Level 0 is the per-frame sky bake,
//! unchanged; levels 1.. are filled here, each from the level above it with the incremental GGX
//! lobe (env_prefilter.wgsl), so level k is the sky seen in a surface of perceptual roughness
//! k / (ENV_MIPS - 1). The per-draw object shader picks `roughness * (ENV_MIPS - 1)`
//! (shader3d.wgsl, ENV_PREFILTER_MAX_LOD -- pinned to ENV_MIPS by a test below).
//!
//! Only recorded while WGR_REFLECT / WGR_REFLECT_DEBUG is set: nothing else samples a level
//! above 0, so with the switch off the frame does the same work as before.

use wgpu::util::DeviceExt;

pub const ENV_MIPS: u32 = 6;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Params {
    alpha: f32,
    width: f32,
    height: f32,
    _pad: f32,
}

struct Level {
    target: wgpu::TextureView,
    bind: wgpu::BindGroup,
    #[allow(dead_code)]
    ubo: wgpu::Buffer,
}

pub struct EnvPrefilter {
    pipeline: wgpu::RenderPipeline,
    levels: Vec<Level>,
}

/// Incremental GGX alpha that takes a lobe of perceptual roughness `from` to one of `to`
/// (alpha = roughness^2; lobe widths combine roughly in quadrature).
pub fn incremental_alpha(from: f32, to: f32) -> f32 {
    let a_from = from * from;
    let a_to = to * to;
    (a_to * a_to - a_from * a_from).max(0.0).sqrt().sqrt()
}

pub fn level_roughness(level: u32) -> f32 {
    level as f32 / (ENV_MIPS - 1) as f32
}

pub fn enabled() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| {
        let set = |k: &str| std::env::var(k).map(|v| !v.trim().is_empty() && v.trim() != "0").unwrap_or(false);
        set("WGR_REFLECT") || set("WGR_REFLECT_DEBUG")
    })
}

impl EnvPrefilter {
    pub fn new(
        device: &wgpu::Device,
        env_tex: &wgpu::Texture,
        format: wgpu::TextureFormat,
        width: u32,
        height: u32,
    ) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_sky_env_prefilter"),
            source: wgpu::ShaderSource::Wgsl(include_str!("env_prefilter.wgsl").into()),
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_env_prefilter_layout"),
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
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_sky_env_prefilter"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_sky_env_prefilter"),
            layout: Some(&pl),
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
                entry_point: Some("fs_prefilter"),
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
        // U wraps across the azimuth seam, V clamps at the poles: the same addressing as the
        // object pass's env sampler.
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_env_prefilter_sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let mip_view = |level: u32| {
            env_tex.create_view(&wgpu::TextureViewDescriptor {
                label: Some("wgr_sky_env_mip"),
                base_mip_level: level,
                mip_level_count: Some(1),
                ..Default::default()
            })
        };
        let mut levels = Vec::new();
        for level in 1..ENV_MIPS {
            let params = Params {
                alpha: incremental_alpha(level_roughness(level - 1), level_roughness(level)),
                width: (width >> level).max(1) as f32,
                height: (height >> level).max(1) as f32,
                _pad: 0.0,
            };
            let ubo = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("wgr_sky_env_prefilter_params"),
                contents: bytemuck::bytes_of(&params),
                usage: wgpu::BufferUsages::UNIFORM,
            });
            let src = mip_view(level - 1);
            let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_sky_env_prefilter_bind"),
                layout: &layout,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&src) },
                    wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&sampler) },
                    wgpu::BindGroupEntry { binding: 2, resource: ubo.as_entire_binding() },
                ],
            });
            levels.push(Level { target: mip_view(level), bind, ubo });
        }
        Self { pipeline, levels }
    }

    /// Fill levels 1.. from level 0. Record right after the env bake, on the same encoder.
    pub fn record(&self, encoder: &mut wgpu::CommandEncoder) {
        if !enabled() {
            return;
        }
        for level in &self.levels {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_sky_env_prefilter"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &level.target,
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
            pass.set_bind_group(0, &level.bind, &[]);
            pass.draw(0..3, 0..1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn incremental_lobes_compose_to_the_level_roughness() {
        // Level k's lobe = level k-1's (+) the incremental one, in alpha^2.
        for level in 1..ENV_MIPS {
            let from = level_roughness(level - 1);
            let to = level_roughness(level);
            let inc = incremental_alpha(from, to);
            let a_from = from * from;
            let a_to = to * to;
            assert!((a_from * a_from + inc.powi(4) - a_to * a_to).abs() < 1e-5);
        }
        assert_eq!(level_roughness(0), 0.0);
        assert_eq!(level_roughness(ENV_MIPS - 1), 1.0);
    }

    #[test]
    fn object_shader_lod_range_matches_the_mip_chain() {
        let src = include_str!("../gfx3d/shader3d.wgsl");
        let want = format!("const ENV_PREFILTER_MAX_LOD: f32 = {}.0;", ENV_MIPS - 1);
        assert!(src.contains(&want), "shader3d.wgsl must declare {want}");
    }

    #[test]
    fn prefilter_shader_parses_and_validates() {
        let module = naga::front::wgsl::parse_str(include_str!("env_prefilter.wgsl")).expect("parse");
        naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .expect("validate");
    }
}
