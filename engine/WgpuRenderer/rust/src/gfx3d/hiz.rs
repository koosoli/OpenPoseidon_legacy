// Hi-Z depth pyramid (docs/gpu-culling-and-depth-plan.md §5).
//
// Reduces the depth prepass (terrain + CPU objects + the GPU-driven set — everything that
// wrote prepass depth, so terrain is a first-class occluder for free) into a min-reduced
// reversed-Z mip chain that the color-pass cull samples for occlusion. See hiz.wgsl for the
// min-reduction rationale (reversed-Z => min, the headline hazard).
//
// The pyramid is a separate R32Float texture rather than mips on the depth target: depth
// formats are neither storage-writable nor (as Depth24PlusStencil8) copyable, so mip0 is a
// compute copy of the depth's depth-aspect and each subsequent mip a 2x2 min of the previous.

use wgpu::TextureFormat;

// R32Float is a core storage-writable format (unlike the depth target); one channel is all
// a depth pyramid needs.
const HIZ_FORMAT: TextureFormat = TextureFormat::R32Float;

// Full mip count for a texture of max(width, height): floor(log2(max)) + 1.
fn mip_count(width: u32, height: u32) -> u32 {
    32 - width.max(height).max(1).leading_zeros()
}

pub struct HiZ {
    // depth -> mip0 copy, and per-level 2x2 min reduction.
    copy_pipeline: wgpu::ComputePipeline,
    reduce_pipeline: wgpu::ComputePipeline,
    copy_layout: wgpu::BindGroupLayout,
    reduce_layout: wgpu::BindGroupLayout,

    size: (u32, u32),
    mips: u32,
    tex: Option<wgpu::Texture>,
    // Full-chain view (all mips) — what the cull's occlusion test binds + textureLoads by mip.
    view_all: Option<wgpu::TextureView>,
    // One single-level view per mip: the storage-write target for its own reduce/copy pass and
    // the sampled source for the next level's reduce.
    mip_views: Vec<wgpu::TextureView>,
    // PERF: these were rebuilt every frame (1 copy + up to ~11 reduce binds at 1080p) even
    // though nothing they reference changes between `ensure` calls — the mip views are ours,
    // and the scene depth view is recreated only by Gfx3d::ensure_depth, which also resizes
    // this pyramid. Cached and dropped in `ensure` for that reason.
    copy_bind: Option<wgpu::BindGroup>,
    reduce_binds: Vec<wgpu::BindGroup>,
}

impl HiZ {
    pub fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_hiz"),
            source: wgpu::ShaderSource::Wgsl(include_str!("hiz.wgsl").into()),
        });

        // copy: 0 = scene depth (depth aspect), 1 = hiz mip0 storage write.
        let copy_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_hiz_copy_layout"),
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
                storage_tex(1),
            ],
        });
        // reduce: 2 = previous mip (sampled), 3 = this mip storage write.
        let reduce_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_hiz_reduce_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                storage_tex(3),
            ],
        });

        let copy_pl_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_hiz_copy_pipeline_layout"),
            bind_group_layouts: &[Some(&copy_layout)],
            immediate_size: 0,
        });
        let reduce_pl_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_hiz_reduce_pipeline_layout"),
            bind_group_layouts: &[Some(&reduce_layout)],
            immediate_size: 0,
        });
        let copy_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_hiz_copy_pipeline"),
            layout: Some(&copy_pl_layout),
            module: &module,
            entry_point: Some("copy_mip0"),
            compilation_options: Default::default(),
            cache: None,
        });
        let reduce_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_hiz_reduce_pipeline"),
            layout: Some(&reduce_pl_layout),
            module: &module,
            entry_point: Some("reduce"),
            compilation_options: Default::default(),
            cache: None,
        });

        Self {
            copy_pipeline,
            reduce_pipeline,
            copy_layout,
            reduce_layout,
            size: (0, 0),
            mips: 0,
            tex: None,
            view_all: None,
            mip_views: Vec::new(),
            copy_bind: None,
            reduce_binds: Vec::new(),
        }
    }

    // (Re)allocate the pyramid to `width x height`. No-op when the size is unchanged. Called
    // alongside ensure_depth so the full-chain view is stable for the frame (the cull's color
    // bind can reference it at frame top; the build fills it mid-frame).
    pub fn ensure(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let size = (width.max(1), height.max(1));
        if self.size == size && self.tex.is_some() {
            return;
        }
        let mips = mip_count(size.0, size.1);
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_hiz"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: mips,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: HIZ_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
            view_formats: &[],
        });
        self.view_all = Some(tex.create_view(&wgpu::TextureViewDescriptor::default()));
        self.mip_views = (0..mips)
            .map(|m| {
                tex.create_view(&wgpu::TextureViewDescriptor {
                    label: Some("wgr_hiz_mip"),
                    base_mip_level: m,
                    mip_level_count: Some(1),
                    ..Default::default()
                })
            })
            .collect();
        self.tex = Some(tex);
        self.size = size;
        self.mips = mips;
        // The cached binds reference the mip views just replaced (and the depth view whose
        // recreation is what brought us here), so they die with them.
        self.copy_bind = None;
        self.reduce_binds.clear();
    }

    pub fn mips(&self) -> u32 {
        self.mips
    }

    // The full-chain view the color-pass cull samples (textureLoad by computed mip).
    pub fn view(&self) -> Option<&wgpu::TextureView> {
        self.view_all.as_ref()
    }

    // Record the pyramid build: copy the scene depth into mip0, then min-reduce down the chain.
    // `depth_view` MUST be a depth-aspect view of the post-prepass depth target. Recorded after
    // the prepass render pass closes and before the color-pass cull dispatch (wgpu barriers the
    // storage writes -> the cull's textureLoads). No-op until ensure() allocated the pyramid.
    pub fn build(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        depth_view: &wgpu::TextureView,
    ) {
        let Some(_tex) = self.tex.as_ref() else {
            return;
        };
        // Build the bind-group cache once per allocation (see the field comment).
        if self.copy_bind.is_none() {
            self.copy_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_hiz_copy_bind"),
                layout: &self.copy_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(depth_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: wgpu::BindingResource::TextureView(&self.mip_views[0]),
                    },
                ],
            }));
            self.reduce_binds = (1..self.mips as usize)
                .map(|m| {
                    device.create_bind_group(&wgpu::BindGroupDescriptor {
                        label: Some("wgr_hiz_reduce_bind"),
                        layout: &self.reduce_layout,
                        entries: &[
                            wgpu::BindGroupEntry {
                                binding: 2,
                                resource: wgpu::BindingResource::TextureView(
                                    &self.mip_views[m - 1],
                                ),
                            },
                            wgpu::BindGroupEntry {
                                binding: 3,
                                resource: wgpu::BindingResource::TextureView(&self.mip_views[m]),
                            },
                        ],
                    })
                })
                .collect();
        }
        // One RenderDoc/capture group for the whole pyramid build (the copy + every reduce level).
        encoder.push_debug_group("wgr_hiz");
        // mip0: 1:1 copy of the scene depth.
        {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_hiz_copy"),
                timestamp_writes: None,
            });
            cp.set_pipeline(&self.copy_pipeline);
            cp.set_bind_group(0, self.copy_bind.as_ref(), &[]);
            let (w, h) = self.size;
            cp.dispatch_workgroups(w.div_ceil(8), h.div_ceil(8), 1);
        }
        // Reduce down the chain: mip m is a 2x2 min of mip m-1. Nested group so the (potentially
        // ~11) reduce dispatches collapse under one node in a capture.
        encoder.push_debug_group("wgr_hiz_reduce");
        // ONE compute pass for the whole chain, not one per level.
        //
        // This used to open a pass per mip -- "(potentially ~11) reduce dispatches", as the
        // comment above still says of the dispatch count. PERF-010 measured what a pass
        // boundary costs on this hardware: the GPU frame carries ~2.8 ms that belongs to no
        // timer region, near-constant across frames from 10 to 20 ms, which works out at
        // roughly 65 us per boundary across ~45 of them. Eleven boundaries here is ~0.7 ms
        // of a 10 ms frame for a pyramid whose actual work is a few hundred microseconds.
        //
        // Merging is legal, and the reason is worth stating because it looks like it should
        // race. WebGPU's usage scope for a compute pass is ONE DISPATCH, not the pass, so
        // reading mip m-1 while writing mip m across successive dispatches is valid even
        // though both views belong to the same texture. Dispatches within a pass execute in
        // submission order, and the implementation inserts the read-after-write barriers
        // between them; what a separate pass would add is the pass setup, which is exactly
        // the cost being removed. Nothing about the reduction changes -- same pipeline, same
        // bind groups, same dispatch sizes, same order.
        {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_hiz_reduce"),
                timestamp_writes: None,
            });
            cp.set_pipeline(&self.reduce_pipeline);
            for m in 1..self.mips as usize {
                cp.set_bind_group(0, &self.reduce_binds[m - 1], &[]);
                let mw = (self.size.0 >> m).max(1);
                let mh = (self.size.1 >> m).max(1);
                cp.dispatch_workgroups(mw.div_ceil(8), mh.div_ceil(8), 1);
            }
        }
        encoder.pop_debug_group(); // wgr_hiz_reduce
        encoder.pop_debug_group(); // wgr_hiz
    }
}

fn storage_tex(binding: u32) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::StorageTexture {
            access: wgpu::StorageTextureAccess::WriteOnly,
            format: HIZ_FORMAT,
            view_dimension: wgpu::TextureViewDimension::D2,
        },
        count: None,
    }
}
