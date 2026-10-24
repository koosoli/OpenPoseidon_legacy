// HDR bloom: a dual-filter mip pyramid over the linear HDR scene (see bloom.wgsl).
// Built only on the HDR path, alongside the tonemap; the resolve adds mip0 to the
// scene before exposure. The pyramid is (re)built on resize; `render` records the
// downsample + upsample passes, and `view` hands mip0 to the tonemap.

use wgpu::util::DeviceExt;

const MAX_MIPS: u32 = 7;

#[test]
fn bloom_wgsl_validates() {
    let module =
        naga::front::wgsl::parse_str(include_str!("bloom.wgsl")).expect("bloom.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("bloom.wgsl validate");
}

#[test]
fn tonemap_wgsl_validates() {
    let module =
        naga::front::wgsl::parse_str(include_str!("tonemap.wgsl")).expect("tonemap.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("tonemap.wgsl validate");
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct BloomParams {
    threshold: f32,
    knee: f32,
    intensity: f32,
    radius: f32,
}

pub struct Bloom {
    prefilter: wgpu::RenderPipeline,
    downsample: wgpu::RenderPipeline,
    upsample: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    params_buf: wgpu::Buffer,
    // Per-resize state.
    mip_views: Vec<wgpu::TextureView>,
    // One bind group per downsample dst (index 0 = prefilter from the scene, then
    // mip[i-1] -> mip[i]); and one per upsample dst (mip[i+1] -> mip[i]).
    down_binds: Vec<wgpu::BindGroup>,
    up_binds: Vec<wgpu::BindGroup>,
    mip_count: usize,
    size: (u32, u32),
}

impl Bloom {
    pub fn new(device: &wgpu::Device, format: wgpu::TextureFormat) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_bloom_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("bloom.wgsl").into()),
        });

        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_bloom_layout"),
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

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_bloom_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });

        let make = |label: &str, fs: &str, blend: Option<wgpu::BlendState>| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
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
                    entry_point: Some(fs),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: crate::pipeline_cache::get(&device),
            })
        };

        let add_blend = wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::One,
                dst_factor: wgpu::BlendFactor::One,
                operation: wgpu::BlendOperation::Add,
            },
            alpha: wgpu::BlendComponent::REPLACE,
        };

        let prefilter = make("wgr_bloom_prefilter", "fs_prefilter", None);
        let downsample = make("wgr_bloom_downsample", "fs_downsample", None);
        let upsample = make("wgr_bloom_upsample", "fs_upsample", Some(add_blend));

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_bloom_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let params_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_bloom_params"),
            contents: bytemuck::bytes_of(&BloomParams {
                threshold: 1.0,
                knee: 0.5,
                intensity: 0.05,
                radius: 1.0,
            }),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });

        Self {
            prefilter,
            downsample,
            upsample,
            layout,
            sampler,
            params_buf,
            mip_views: Vec::new(),
            down_binds: Vec::new(),
            up_binds: Vec::new(),
            mip_count: 0,
            size: (0, 0),
        }
    }

    // (Re)build the pyramid for a scene of (width, height). `scene_view` is the linear
    // HDR target the prefilter reads (recreated on resize, so this is called from
    // ensure_hdr right after the HDR target is made).
    pub fn resize(
        &mut self,
        device: &wgpu::Device,
        width: u32,
        height: u32,
        format: wgpu::TextureFormat,
        scene_view: &wgpu::TextureView,
    ) {
        // Base is half-res; add mips until the next halving would drop below 2px.
        let (mut w, mut h) = ((width / 2).max(1), (height / 2).max(1));
        let mut mip_count = 1u32;
        while mip_count < MAX_MIPS && (w / 2) >= 2 && (h / 2) >= 2 {
            w /= 2;
            h /= 2;
            mip_count += 1;
        }
        let mip_count = mip_count as usize;

        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_bloom_pyramid"),
            size: wgpu::Extent3d {
                width: (width / 2).max(1),
                height: (height / 2).max(1),
                depth_or_array_layers: 1,
            },
            mip_level_count: mip_count as u32,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });

        let mip_views: Vec<wgpu::TextureView> = (0..mip_count)
            .map(|i| {
                texture.create_view(&wgpu::TextureViewDescriptor {
                    label: Some("wgr_bloom_mip"),
                    base_mip_level: i as u32,
                    mip_level_count: Some(1),
                    dimension: Some(wgpu::TextureViewDimension::D2),
                    ..Default::default()
                })
            })
            .collect();

        let make_bind = |src: &wgpu::TextureView| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_bloom_bind"),
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
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: self.params_buf.as_entire_binding(),
                    },
                ],
            })
        };

        // Downsample sources: index 0 reads the full-res scene (prefilter), i reads mip[i-1].
        let down_binds: Vec<wgpu::BindGroup> = (0..mip_count)
            .map(|i| {
                if i == 0 {
                    make_bind(scene_view)
                } else {
                    make_bind(&mip_views[i - 1])
                }
            })
            .collect();
        // Upsample sources: index i reads mip[i+1] (rendered additively into mip[i]).
        let up_binds: Vec<wgpu::BindGroup> = (0..mip_count.saturating_sub(1))
            .map(|i| make_bind(&mip_views[i + 1]))
            .collect();

        self.mip_views = mip_views;
        self.down_binds = down_binds;
        self.up_binds = up_binds;
        self.mip_count = mip_count;
        self.size = (width, height);
    }

    pub fn upload_params(&self, queue: &wgpu::Queue, threshold: f32, knee: f32, radius: f32) {
        queue.write_buffer(
            &self.params_buf,
            0,
            bytemuck::bytes_of(&BloomParams {
                threshold,
                knee,
                intensity: 0.0,
                radius,
            }),
        );
    }

    // The underwater compositor substitutes its scratch target for the finished scene.
    pub fn set_source(&mut self, device: &wgpu::Device, scene_view: &wgpu::TextureView) {
        if self.down_binds.is_empty() {
            return;
        }
        self.down_binds[0] = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_bloom_scene_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(scene_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: self.params_buf.as_entire_binding(),
                },
            ],
        });
    }

    // mip0 of the finished pyramid — the bloom the tonemap adds to the scene.
    pub fn view(&self) -> Option<&wgpu::TextureView> {
        self.mip_views.first()
    }

    // Record the downsample chain then the additive upsample chain. `upload_params`
    // must have run this frame. No-op until `resize` has built the pyramid.
    pub fn render(&self, encoder: &mut wgpu::CommandEncoder) {
        if self.mip_count == 0 {
            return;
        }
        encoder.push_debug_group("wgr_bloom");
        // Downsample: prefilter scene -> mip0, then box mip[i-1] -> mip[i].
        for i in 0..self.mip_count {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_bloom_down"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &self.mip_views[i],
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
            pass.set_pipeline(if i == 0 {
                &self.prefilter
            } else {
                &self.downsample
            });
            pass.set_bind_group(0, &self.down_binds[i], &[]);
            pass.draw(0..3, 0..1);
        }
        // Upsample: additively sum mip[i+1] into mip[i], from the top down to mip0.
        for i in (0..self.mip_count - 1).rev() {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_bloom_up"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &self.mip_views[i],
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Load,
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(&self.upsample);
            pass.set_bind_group(0, &self.up_binds[i], &[]);
            pass.draw(0..3, 0..1);
        }
        encoder.pop_debug_group();
    }
}
