//! (OP) The water pass's own depth buffer.
//!
//! The water samples the opaque scene depth (seabed reconstruction, coast test) and at 1x that is
//! the live depth target, so the pass attached it read-only and the sea wrote no depth at all. Water
//! against water was then resolved by DRAW ORDER, and the CDLOD nodes are drawn front to back: the
//! far sea painted over the near sea wherever both lay on one pixel. Above the water that drew the
//! hidden backs of distant waves over the crests in front of them (thin dark slivers along the
//! waves, owner 2026-10-13; captured at the owner's pose, gone with this buffer).
//!
//! So the pass gets a depth buffer of its own, of the scene depth's format, size and sample count:
//! a fullscreen pass copies the (single-sample) scene depth the water already samples into it, and
//! the water then tests AND writes it; the scene's depth is untouched. `WGR_TW_WATER_ZBUF=0`
//! restores the read-only attachment.
//!
//! After the pass the water's depth is merged into the scene's (nearest wins), so the passes after
//! the water see its surface: the depth-aware cloud composite marched past every wave crest standing
//! against the sky and painted the cloud deck over it (owner, 2026-10-13, at amplitude 3), and smoke
//! and transparents behind a wave composited over it. `WGR_TW_WATER_DEPTH_MERGE=0` turns it off.
//! (From just below the surface the cloud composite is off altogether: lib.rs `submerged_view`.)

const ZINIT_WGSL: &str = r#"
@group(0) @binding(0) var srcDepth: texture_depth_2d;

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) pos: vec4<f32>) -> @builtin(frag_depth) f32 {
    let size = vec2<i32>(textureDimensions(srcDepth));
    let p = clamp(vec2<i32>(pos.xy), vec2<i32>(0), size - vec2<i32>(1));
    return textureLoad(srcDepth, p, 0);
}
"#;

const MERGE_WGSL: &str = r#"
@group(0) @binding(0) var waterDepth: texture_depth_2d;

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) pos: vec4<f32>) -> @builtin(frag_depth) f32 {
    return textureLoad(waterDepth, vec2<i32>(pos.xy), 0);
}
"#;

const MERGE_MS_WGSL: &str = r#"
@group(0) @binding(0) var waterDepth: texture_depth_multisampled_2d;

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) pos: vec4<f32>, @builtin(sample_index) si: u32) -> @builtin(frag_depth) f32 {
    return textureLoad(waterDepth, vec2<i32>(pos.xy), i32(si));
}
"#;

pub fn merge_enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    // on (WGR_TW_WATER_DEPTH_MERGE=0: off): without it the depth-aware cloud composite painted the cloud
    // deck over every wave standing against the sky (owner, 2026-10-13, amplitude 3)
    *V.get_or_init(|| enabled() && !std::env::var("WGR_TW_WATER_DEPTH_MERGE").is_ok_and(|v| v.trim() == "0"))
}

pub fn enabled() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| !std::env::var("WGR_TW_WATER_ZBUF").is_ok_and(|v| v.trim() == "0"))
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct Key {
    width: u32,
    height: u32,
    samples: u32,
    format: wgpu::TextureFormat,
}

pub struct WaterZBuf {
    key: Option<Key>,
    view: Option<wgpu::TextureView>,
    depth_only: Option<wgpu::TextureView>,
    _tex: Option<wgpu::Texture>,
    merge: Option<(Key, wgpu::RenderPipeline, wgpu::BindGroupLayout)>,
    pipeline: Option<(Key, wgpu::RenderPipeline)>,
    layout: wgpu::BindGroupLayout,
    module: wgpu::ShaderModule,
}

impl WaterZBuf {
    pub fn new(device: &wgpu::Device) -> Self {
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_water_zinit"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Depth,
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            }],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_water_zinit"),
            source: wgpu::ShaderSource::Wgsl(ZINIT_WGSL.into()),
        });
        Self { key: None, view: None, depth_only: None, _tex: None, merge: None, pipeline: None, layout, module }
    }

    /// Records the copy of `scene_depth` (single-sample depth aspect) into the water's depth buffer,
    /// sized like `attachment` (the scene depth target the water pass would otherwise attach), and
    /// returns that buffer's view for the water pass.
    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        attachment: &wgpu::Texture,
        scene_depth: &wgpu::TextureView,
    ) -> &wgpu::TextureView {
        let key = Key {
            width: attachment.width(),
            height: attachment.height(),
            samples: attachment.sample_count(),
            format: attachment.format(),
        };
        if self.key != Some(key) {
            let tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_water_zbuf"),
                size: wgpu::Extent3d { width: key.width, height: key.height, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: key.samples,
                dimension: wgpu::TextureDimension::D2,
                format: key.format,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            self.view = Some(tex.create_view(&Default::default()));
            self.depth_only = Some(tex.create_view(&wgpu::TextureViewDescriptor {
                label: Some("wgr_water_zbuf_depth"),
                aspect: wgpu::TextureAspect::DepthOnly,
                ..Default::default()
            }));
            self._tex = Some(tex);
            self.key = Some(key);
        }
        if self.pipeline.as_ref().map(|(k, _)| *k) != Some(key) {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_water_zinit"),
                bind_group_layouts: &[Some(&self.layout)],
                immediate_size: 0,
            });
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_water_zinit"),
                layout: Some(&pl),
                vertex: wgpu::VertexState { module: &self.module, entry_point: Some("vs_main"), compilation_options: Default::default(), buffers: &[] },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: key.format,
                    depth_write_enabled: Some(true),
                    depth_compare: Some(wgpu::CompareFunction::Always),
                    stencil: wgpu::StencilState::default(),
                    bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState { count: key.samples, ..Default::default() },
                fragment: Some(wgpu::FragmentState {
                    module: &self.module,
                    entry_point: Some("fs_main"),
                    compilation_options: Default::default(),
                    targets: &[],
                }),
                multiview_mask: None,
                cache: crate::pipeline_cache::get(&device),
            });
            self.pipeline = Some((key, pipeline));
        }
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_water_zinit"),
            layout: &self.layout,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(scene_depth) }],
        });
        let view = self.view.as_ref().expect("sized above");
        {
            let has_stencil = key.format.has_stencil_aspect();
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_water_zinit"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: has_stencil.then_some(wgpu::Operations { load: wgpu::LoadOp::Clear(0), store: wgpu::StoreOp::Store }),
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(&self.pipeline.as_ref().expect("built above").1);
            pass.set_bind_group(0, &bind, &[]);
            pass.draw(0..3, 0..1);
        }
        view
    }

    /// After the water pass: writes the water's depth into `scene_depth` (the attachment the water
    /// buffer was sized from) wherever it is nearer, so the passes after the water see its surface.
    pub fn merge(&mut self, device: &wgpu::Device, encoder: &mut wgpu::CommandEncoder, scene_depth: &wgpu::TextureView) {
        let (Some(key), Some(src)) = (self.key, self.depth_only.as_ref()) else { return };
        if self.merge.as_ref().map(|(k, _, _)| *k) != Some(key) {
            let ms = key.samples > 1;
            let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_water_zmerge"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: ms,
                    },
                    count: None,
                }],
            });
            let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some("wgr_water_zmerge"),
                source: wgpu::ShaderSource::Wgsl(if ms { MERGE_MS_WGSL } else { MERGE_WGSL }.into()),
            });
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_water_zmerge"),
                bind_group_layouts: &[Some(&layout)],
                immediate_size: 0,
            });
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_water_zmerge"),
                layout: Some(&pl),
                vertex: wgpu::VertexState { module: &module, entry_point: Some("vs_main"), compilation_options: Default::default(), buffers: &[] },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: key.format,
                    depth_write_enabled: Some(true),
                    // reversed Z: nearer is greater
                    depth_compare: Some(wgpu::CompareFunction::Greater),
                    stencil: wgpu::StencilState::default(),
                    bias: wgpu::DepthBiasState::default(),
                }),
                multisample: wgpu::MultisampleState { count: key.samples, ..Default::default() },
                fragment: Some(wgpu::FragmentState {
                    module: &module,
                    entry_point: Some("fs_main"),
                    compilation_options: Default::default(),
                    targets: &[],
                }),
                multiview_mask: None,
                cache: crate::pipeline_cache::get(&device),
            });
            self.merge = Some((key, pipeline, layout));
        }
        let (_, pipeline, layout) = self.merge.as_ref().expect("built above");
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_water_zmerge"),
            layout,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(src) }],
        });
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_water_zmerge"),
            color_attachments: &[],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view: scene_depth,
                depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store }),
                stencil_ops: None,
            }),
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.draw(0..3, 0..1);
    }
}
