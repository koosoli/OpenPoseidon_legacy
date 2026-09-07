// SMK-037: the scene-depth snapshot that makes soft particles possible.
//
// See soft_particles.wgsl for why a copy is needed rather than sampling the live depth
// target. This module owns the R32Float snapshot texture, resizes it with the render
// target, and records the one full-screen pass that fills it. It is created lazily and the
// pass is recorded only on frames where the soft path is actually enabled, so a build with
// the lever off pays nothing beyond a null check.

pub const SNAPSHOT_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::R32Float;

pub struct SoftParticles {
    pipeline: wgpu::RenderPipeline,
    src_layout: wgpu::BindGroupLayout,
    // The snapshot, its size, and the bind group reading the depth it was filled from.
    target: Option<(wgpu::Texture, wgpu::TextureView)>,
    size: (u32, u32),
    src_bind: Option<wgpu::BindGroup>,
    // Generation of the depth view `src_bind` was built against (Gfx3d::depth_gen), so a
    // resize rebuilds it and nothing else does.
    src_gen: u64,
    src_valid: bool,
}

impl SoftParticles {
    pub fn new(device: &wgpu::Device) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_soft_particles_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("soft_particles.wgsl").into()),
        });
        let src_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_soft_depth_src_layout"),
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
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_soft_depth_layout"),
            bind_group_layouts: &[Some(&src_layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_soft_depth_pipeline"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: SNAPSHOT_FORMAT,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        SoftParticles {
            pipeline,
            src_layout,
            target: None,
            size: (0, 0),
            src_bind: None,
            src_gen: u64::MAX,
            src_valid: false,
        }
    }

    /// The snapshot view, or None until a frame has actually filled one. The 2D pipeline
    /// binds a snapshot only when this is Some AND `valid()` — a stale snapshot from a
    /// previous camera would fade sprites against the wrong geometry.
    pub fn view(&self) -> Option<&wgpu::TextureView> {
        self.target.as_ref().map(|(_, v)| v)
    }

    pub fn valid(&self) -> bool {
        self.src_valid && self.target.is_some()
    }

    /// Mark the snapshot stale (end of frame, or a pass that rewrote depth after it).
    pub fn invalidate(&mut self) {
        self.src_valid = false;
    }

    /// Record the copy. `src` is a SINGLE-SAMPLE depth view not attached to any pass at
    /// this point in the frame (`Gfx3d::water_depth_view` after `ensure_far_depth_resolve`);
    /// `depth_gen` is `Gfx3d::depth_gen()`, which changes on resize.
    pub fn record(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        src: &wgpu::TextureView,
        depth_gen: u64,
        width: u32,
        height: u32,
    ) {
        if width == 0 || height == 0 {
            return;
        }
        if self.target.is_none() || self.size != (width, height) {
            let tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_soft_depth_snapshot"),
                size: wgpu::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: SNAPSHOT_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
            self.target = Some((tex, view));
            self.size = (width, height);
            // A new target invalidates nothing about the source, but the bind group below
            // is keyed on depth_gen and a resize moves both together anyway.
            self.src_bind = None;
        }
        if self.src_bind.is_none() || self.src_gen != depth_gen {
            self.src_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_soft_depth_src"),
                layout: &self.src_layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(src),
                }],
            }));
            self.src_gen = depth_gen;
        }
        let (Some((_, view)), Some(bind)) = (self.target.as_ref(), self.src_bind.as_ref()) else {
            return;
        };
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_soft_depth_snapshot_pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    // Every texel is written by the fullscreen triangle, so nothing is loaded.
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
        pass.set_bind_group(0, bind, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        self.src_valid = true;
    }
}
