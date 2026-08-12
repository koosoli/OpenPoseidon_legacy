// Linear view-Z mip chain for GTAO's horizon march (screen-space-ao-plan Stage 3).
//
// Deliberately NOT the Hi-Z pyramid in hiz.rs. That one min-reduces REVERSED-Z, which selects
// the FARTHEST surface per block — right for occlusion culling (never cull something that might
// be visible), exactly wrong for AO, which cares about the nearest thing that could occlude.
// Sampling it here would under-occlude, progressively worse at coarser mips.
//
// What this buys: the horizon march can cover a large screen radius with a fixed tap budget by
// stepping up a mip as it goes, so the world-space radius no longer has to be clamped in pixels.
// That clamp was the cause of AO weakening as the camera approached a surface — the shortfall
// grew with proximity, so walls visibly brightened as you walked toward them.

use wgpu::TextureFormat;

// One channel of linear view-space metres. R32Float is core storage-writable (unlike a depth
// format) and f32 keeps distant metres exact enough for the falloff comparisons.
const MIP_FORMAT: TextureFormat = TextureFormat::R32Float;

fn mip_count(width: u32, height: u32) -> u32 {
    32 - width.max(height).max(1).leading_zeros()
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct MipParams {
    // x = camera near plane; z = near / stored_depth is the whole linearisation.
    proj: [f32; 4],
}

pub(crate) struct GtaoDepthMips {
    linearise_pipeline: wgpu::ComputePipeline,
    reduce_pipeline: wgpu::ComputePipeline,
    linearise_layout: wgpu::BindGroupLayout,
    reduce_layout: wgpu::BindGroupLayout,
    params: wgpu::Buffer,
    size: (u32, u32),
    mips: u32,
    view_all: Option<wgpu::TextureView>,
    mip_views: Vec<wgpu::TextureView>,
    tex: Option<wgpu::Texture>,
    // PERF: the bind groups were rebuilt from scratch every frame although nothing they
    // reference can change without `resize` running: the mip views are ours, the params buffer
    // is fixed, and `depth_view` is recreated only by Gfx3d::ensure_depth — which calls
    // `resize` in the same branch. Cached here and dropped in `resize` for exactly that reason.
    linearise_bind: Option<wgpu::BindGroup>,
    reduce_binds: Vec<wgpu::BindGroup>,
}

impl GtaoDepthMips {
    pub(crate) fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_gtao_depth_mips"),
            source: wgpu::ShaderSource::Wgsl(include_str!("gtao_depth_mips.wgsl").into()),
        });
        let storage_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::StorageTexture {
                access: wgpu::StorageTextureAccess::WriteOnly,
                format: MIP_FORMAT,
                view_dimension: wgpu::TextureViewDimension::D2,
            },
            count: None,
        };
        let linearise_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_gtao_mip_linearise_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                storage_entry(1),
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let reduce_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_gtao_mip_reduce_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                storage_entry(4),
            ],
        });
        let mk = |label: &str, layout: &wgpu::BindGroupLayout, entry: &str| {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some(label),
                bind_group_layouts: &[Some(layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some(label),
                layout: Some(&pl),
                module: &module,
                entry_point: Some(entry),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        Self {
            linearise_pipeline: mk("wgr_gtao_mip_linearise", &linearise_layout, "cs_linearise"),
            reduce_pipeline: mk("wgr_gtao_mip_reduce", &reduce_layout, "cs_reduce"),
            linearise_layout,
            reduce_layout,
            params: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_gtao_mip_params"),
                size: std::mem::size_of::<MipParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }),
            size: (0, 0),
            mips: 0,
            view_all: None,
            mip_views: Vec::new(),
            tex: None,
            linearise_bind: None,
            reduce_binds: Vec::new(),
        }
    }

    pub(crate) fn resize(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let size = (width.max(1), height.max(1));
        if self.size == size && self.tex.is_some() {
            return;
        }
        let mips = mip_count(size.0, size.1);
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gtao_depth_mips"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: mips,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: MIP_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
            view_formats: &[],
        });
        self.view_all = Some(tex.create_view(&wgpu::TextureViewDescriptor::default()));
        self.mip_views = (0..mips)
            .map(|m| {
                tex.create_view(&wgpu::TextureViewDescriptor {
                    label: Some("wgr_gtao_depth_mip"),
                    base_mip_level: m,
                    mip_level_count: Some(1),
                    ..Default::default()
                })
            })
            .collect();
        self.tex = Some(tex);
        self.size = size;
        self.mips = mips;
        // The cached binds reference the views just replaced (and the depth view the caller is
        // replacing in the same breath), so they die here.
        self.linearise_bind = None;
        self.reduce_binds.clear();
    }

    pub(crate) fn mips(&self) -> u32 {
        self.mips
    }

    // Whole-chain view; GTAO textureLoads it at a computed mip.
    pub(crate) fn view(&self) -> Option<&wgpu::TextureView> {
        self.view_all.as_ref()
    }

    // Record the chain build: linearise the resolved depth into mip0, then min-reduce down.
    // `depth_view` must be the single-sample nearest-resolved depth (post-prepass).
    //
    // `top_mip` is the highest level the horizon march may read (GtaoParams.proj.w — the shipped
    // default is 0). PERF: levels above it are never sampled, so building them was pure waste —
    // gtao.wgsl clamps `mip` to `max_mip` and sample_z's upper tap is guarded by the same value,
    // so nothing can read past `top_mip`. At the default that removes the ENTIRE reduce chain
    // (~10 dispatches at 1080p) and leaves only the full-res linearise, with no visual change.
    pub(crate) fn build(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        depth_view: &wgpu::TextureView,
        near: f32,
        top_mip: u32,
    ) {
        if self.tex.is_none() {
            return;
        }
        queue.write_buffer(
            &self.params,
            0,
            bytemuck::bytes_of(&MipParams {
                proj: [near, 0.0, 0.0, 0.0],
            }),
        );
        encoder.push_debug_group("wgr_gtao_depth_mips");
        let linearise_bind = self.linearise_bind.get_or_insert_with(|| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_gtao_mip_linearise_bind"),
                layout: &self.linearise_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(depth_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: wgpu::BindingResource::TextureView(&self.mip_views[0]),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: self.params.as_entire_binding(),
                    },
                ],
            })
        });
        {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_gtao_mip_linearise"),
                timestamp_writes: None,
            });
            cp.set_pipeline(&self.linearise_pipeline);
            cp.set_bind_group(0, &*linearise_bind, &[]);
            cp.dispatch_workgroups(self.size.0.div_ceil(8), self.size.1.div_ceil(8), 1);
        }
        let last = top_mip.min(self.mips.saturating_sub(1)) as usize;
        while self.reduce_binds.len() < last {
            let m = self.reduce_binds.len() + 1;
            self.reduce_binds
                .push(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("wgr_gtao_mip_reduce_bind"),
                    layout: &self.reduce_layout,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 3,
                            resource: wgpu::BindingResource::TextureView(&self.mip_views[m - 1]),
                        },
                        wgpu::BindGroupEntry {
                            binding: 4,
                            resource: wgpu::BindingResource::TextureView(&self.mip_views[m]),
                        },
                    ],
                }));
        }
        for m in 1..=last {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_gtao_mip_reduce"),
                timestamp_writes: None,
            });
            cp.set_pipeline(&self.reduce_pipeline);
            cp.set_bind_group(0, &self.reduce_binds[m - 1], &[]);
            let mw = (self.size.0 >> m).max(1);
            let mh = (self.size.1 >> m).max(1);
            cp.dispatch_workgroups(mw.div_ceil(8), mh.div_ceil(8), 1);
        }
        encoder.pop_debug_group();
    }
}
