// Downsample the planar reflection after it has been rendered. Water samples this
// chain directly by roughness, avoiding a screen-space blur of a sharp mip zero.

const MAX_MIPS: u32 = 7;

pub struct PlanarMips {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    // PERF: one bind group per level was created every frame although the only thing they
    // reference — the planar target's mip views — changes solely when the caller reallocates
    // that target on a resize. Cached alongside the exact views they were built from and
    // compared by identity (wgpu resource equality is identity, not structural), so a
    // reallocation that happens to keep the same mip COUNT still invalidates the cache. A
    // count-only key would silently keep binding views of a dropped texture.
    binds: Vec<wgpu::BindGroup>,
    bind_sources: Vec<wgpu::TextureView>,
}

impl PlanarMips {
    pub fn new(device: &wgpu::Device, format: wgpu::TextureFormat) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_planar_mips_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("planar_mips.wgsl").into()),
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_planar_mips_layout"),
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
            ],
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_planar_mips_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_planar_mips_pipeline"),
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
                entry_point: Some("fs_downsample"),
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
            label: Some("wgr_planar_mips_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        Self {
            pipeline,
            layout,
            sampler,
            binds: Vec::new(),
            bind_sources: Vec::new(),
        }
    }

    pub fn mip_count(width: u32, height: u32) -> u32 {
        let mut count = 1;
        let (mut width, mut height) = (width, height);
        while count < MAX_MIPS && width > 1 && height > 1 {
            width = (width / 2).max(1);
            height = (height / 2).max(1);
            count += 1;
        }
        count
    }

    pub fn render(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        mip_views: &[wgpu::TextureView],
    ) {
        // Rebuild the cache only when the source views actually differ (identity compare).
        let sources = &mip_views[..mip_views.len().saturating_sub(1)];
        if self.bind_sources != sources {
            self.binds = sources
                .iter()
                .map(|src| {
                    device.create_bind_group(&wgpu::BindGroupDescriptor {
                        label: Some("wgr_planar_mips_bind"),
                        layout: &self.layout,
                        entries: &[
                            wgpu::BindGroupEntry {
                                binding: 0,
                                resource: wgpu::BindingResource::TextureView(src),
                            },
                            wgpu::BindGroupEntry {
                                binding: 1,
                                resource: wgpu::BindingResource::Sampler(&self.sampler),
                            },
                        ],
                    })
                })
                .collect();
            self.bind_sources = sources.to_vec();
        }
        for level in 1..mip_views.len() {
            let bind = &self.binds[level - 1];
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_planar_mips_downsample"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &mip_views[level],
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
            pass.set_bind_group(0, bind, &[]);
            pass.draw(0..3, 0..1);
        }
    }
}

#[test]
fn planar_mips_wgsl_validates() {
    let module = naga::front::wgsl::parse_str(include_str!("planar_mips.wgsl"))
        .expect("planar_mips.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("planar_mips.wgsl validate");
}
