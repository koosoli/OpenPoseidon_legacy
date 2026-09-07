// SMK-038: a camera-anchored volumetric smoke field, as an alternative to the 2001
// billboards rather than a replacement for them. See smoke_volume.wgsl for the five passes
// and why injection scatters instead of gathering.
//
// WHAT THIS BUYS THAT SMK-037 CANNOT. Soft particles fix the EDGE of a billboard. They do
// not give it an interior, a silhouette that survives the camera moving, or light marched
// through an actual density field -- a sprite has no field to march. Every sprite in a
// plume is also the same round texture at the same screen-space roll, so a column reads as
// repeated identical puffs however well its edges are feathered. A froxel grid has none of
// those problems by construction, and it pays for them with a fixed per-frame cost that
// does not care how many particles there are.
//
// EVERYTHING IS OFF UNLESS ASKED. `record` returns immediately when the mode is Legacy, and
// nothing here allocates until the first frame that actually runs.

/// Grid dimensions in cells. 128 x 64 x 128 at the default 1 m cell is a 128 m box, 64 m
/// tall, centred on the camera -- which covers the plume the player is standing in and not
/// the one across the valley. The cell size is a lever, so trading resolution for reach is
/// one slider; the DIMENSIONS are fixed because they size the accumulator and the texture.
pub const GRID_W: u32 = 128;
pub const GRID_H: u32 = 64;
pub const GRID_D: u32 = 128;
const CELL_COUNT: u32 = GRID_W * GRID_H * GRID_D;

/// Rgba16Float: the widest format that is both storage-writable and FILTERABLE in core
/// WebGPU. The march samples the field trilinearly, so filterable is not optional, which
/// rules out R32Float; r16float is not a core storage format at all.
const FIELD_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;
const MARCH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

/// Particle cap. The ground-shadow path caps at 512 because its cost is per shadow texel
/// TIMES per blob; injection is per blob only, so it can afford four times as many, which
/// is what a burning village actually produces.
pub const MAX_BLOBS: usize = 2048;

/// One smoke particle, byte-identical to `WgrSmokeShadowBlob` / `sky::SmokeShadowBlob` so
/// the C++ side can fill one array and hand it to either consumer.
#[repr(C)]
#[derive(Clone, Copy, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct SmokeBlob {
    /// xyz world position, w radius in metres.
    pub pos_radius: [f32; 4],
    /// x optical density, y height above ground, zw unused.
    pub density: [f32; 4],
}

/// The live tuning the dev panel edits, mirroring `WgrSmokeVolumeParams`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Tuning {
    /// 0 = legacy billboards only (this module does nothing), 1 = volumetric, 2 = both
    /// (a debug view: the billboards are left drawing so the two can be compared in one
    /// frame; it double-counts the smoke and is not a shipping setting).
    pub mode: u32,
    pub cell_size: f32,
    pub march_steps: u32,
    pub sun_steps: u32,
    pub sun_step_len: f32,
    pub density: f32,
    pub extinction: f32,
    pub albedo: f32,
    pub anisotropy: f32,
    pub self_shadow: f32,
    pub jitter: f32,
    /// 1 = march at render resolution, 2 = half (the standard cost control).
    pub scale: u32,
}

impl Default for Tuning {
    fn default() -> Self {
        Tuning {
            mode: 0,
            cell_size: 1.0,
            march_steps: 48,
            sun_steps: 4,
            sun_step_len: 1.5,
            density: 0.3,
            extinction: 1.0,
            albedo: 0.9,
            anisotropy: 0.35,
            self_shadow: 1.0,
            jitter: 1.0,
            scale: 2,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default, bytemuck::Pod, bytemuck::Zeroable)]
struct GpuParams {
    origin_cell: [f32; 4],
    dims_steps: [f32; 4],
    cam_albedo: [f32; 4],
    sun_dir_steps: [f32; 4],
    sun_radiance: [f32; 4],
    ambient_ext: [f32; 4],
    tune: [f32; 4],
    sizes: [f32; 4],
    inv_view_proj: [f32; 16],
    counts: [u32; 4],
}

pub struct SmokeVolume {
    params_buf: wgpu::Buffer,
    accum_buf: wgpu::Buffer,
    blob_buf: wgpu::Buffer,
    // Owns the 3D texture; only the view is ever bound.
    _field: wgpu::Texture,
    field_view: wgpu::TextureView,
    field_sampler: wgpu::Sampler,

    compute_bind: wgpu::BindGroup,
    clear_pipeline: wgpu::ComputePipeline,
    inject_pipeline: wgpu::ComputePipeline,
    resolve_pipeline: wgpu::ComputePipeline,

    // Two group-1 layouts, because the two entry points read DIFFERENT things and the march
    // must not have its own render target bound: fs_march uses the field, its sampler and the
    // scene depth; fs_composite uses the scene depth and the march result.
    march_g1: wgpu::BindGroupLayout,
    comp_g1: wgpu::BindGroupLayout,
    march_g0_bind: wgpu::BindGroup,
    march_pipeline: wgpu::RenderPipeline,
    composite_pipeline: wgpu::RenderPipeline,

    /// Half-res (or full-res) march target, resized with the render target.
    march_target: Option<(wgpu::Texture, wgpu::TextureView)>,
    march_size: (u32, u32),

    pub tuning: Tuning,
    blob_count: u32,
    /// Set once a frame has actually recorded the passes, so the dev panel can tell
    /// "switched on but nothing reached it" from "switched on and marching".
    pub frames_marched: u64,
    pub last_blob_count: u32,
}

impl SmokeVolume {
    /// `sample_count` is the SCENE target's MSAA count. The march renders to its own 1x
    /// target, but the composite blends straight into the scene, so that pipeline has to
    /// match or wgpu rejects the set_pipeline outright.
    pub fn new(
        device: &wgpu::Device,
        color_format: wgpu::TextureFormat,
        sample_count: u32,
    ) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_smoke_volume_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("smoke_volume.wgsl").into()),
        });

        let params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_smoke_volume_params"),
            size: std::mem::size_of::<GpuParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let accum_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_smoke_volume_accum"),
            size: (CELL_COUNT as u64) * 4,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let blob_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_smoke_volume_blobs"),
            size: (MAX_BLOBS * std::mem::size_of::<SmokeBlob>()) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let field = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_smoke_volume_field"),
            size: wgpu::Extent3d {
                width: GRID_W,
                height: GRID_H,
                depth_or_array_layers: GRID_D,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            format: FIELD_FORMAT,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let field_view = field.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_smoke_volume_field_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        let field_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_smoke_volume_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });

        // --- compute set: params + accumulator + blobs + the field as a storage image.
        let compute_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_smoke_volume_compute_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: FIELD_FORMAT,
                        view_dimension: wgpu::TextureViewDimension::D3,
                    },
                    count: None,
                },
            ],
        });
        let compute_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_smoke_volume_compute_bind"),
            layout: &compute_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: accum_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: blob_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&field_view),
                },
            ],
        });
        let compute_pipeline_layout =
            device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_smoke_volume_compute_pl"),
                bind_group_layouts: &[Some(&compute_layout)],
                immediate_size: 0,
            });
        let make_compute = |entry: &str, label: &str| {
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some(label),
                layout: Some(&compute_pipeline_layout),
                module: &shader,
                entry_point: Some(entry),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        let clear_pipeline = make_compute("cs_clear", "wgr_smoke_volume_clear");
        let inject_pipeline = make_compute("cs_inject", "wgr_smoke_volume_inject");
        let resolve_pipeline = make_compute("cs_resolve", "wgr_smoke_volume_resolve");

        // --- render set: group 0 is the same params buffer (fragment-visible), group 1 the
        // field, the scene depth and the march result.
        let march_g0 = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_smoke_volume_march_g0"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let depth_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Depth,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        // fs_march reads the field, its filtering sampler and the scene depth. It does NOT
        // read the march result -- that is this pass's own colour attachment, and a texture
        // cannot be a writable attachment and a sampled binding at the same time. Hence two
        // group-1 layouts rather than one: WGSL prunes the globals an entry point does not
        // use, so each pipeline layout only has to satisfy what its own entry point reads.
        let march_g1 = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_smoke_volume_march_g1"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D3,
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
                depth_entry(2),
            ],
        });
        // fs_composite reads the scene depth (for the nearest-depth pick) and the march result.
        let comp_g1 = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_smoke_volume_comp_g1"),
            entries: &[
                depth_entry(2),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });
        let march_g0_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_smoke_volume_march_g0_bind"),
            layout: &march_g0,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: params_buf.as_entire_binding(),
            }],
        });
        let march_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_smoke_volume_march_pl"),
            bind_group_layouts: &[Some(&march_g0), Some(&march_g1)],
            immediate_size: 0,
        });
        let comp_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_smoke_volume_comp_pl"),
            bind_group_layouts: &[Some(&march_g0), Some(&comp_g1)],
            immediate_size: 0,
        });
        let make_render = |entry: &str,
                           label: &str,
                           format: wgpu::TextureFormat,
                           blend: Option<wgpu::BlendState>,
                           pl: &wgpu::PipelineLayout,
                           samples: u32| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(pl),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_fullscreen"),
                    compilation_options: Default::default(),
                    buffers: &[],
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: wgpu::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(entry),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let march_pipeline =
            make_render("fs_march", "wgr_smoke_volume_march", MARCH_FORMAT, None, &march_pl, 1);
        // dst = dst * transmittance + inscatter. The march writes premultiplied light in rgb
        // and the ray's surviving transmittance in alpha, so the volume both ADDS its own
        // scattering and DIMS what is behind it in one blend.
        let composite_pipeline = make_render(
            "fs_composite",
            "wgr_smoke_volume_composite",
            color_format,
            Some(wgpu::BlendState {
                color: wgpu::BlendComponent {
                    src_factor: wgpu::BlendFactor::One,
                    dst_factor: wgpu::BlendFactor::SrcAlpha,
                    operation: wgpu::BlendOperation::Add,
                },
                alpha: wgpu::BlendComponent {
                    src_factor: wgpu::BlendFactor::One,
                    dst_factor: wgpu::BlendFactor::SrcAlpha,
                    operation: wgpu::BlendOperation::Add,
                },
            }),
            &comp_pl,
            sample_count,
        );

        SmokeVolume {
            params_buf,
            accum_buf,
            blob_buf,
            _field: field,
            field_view,
            field_sampler,
            compute_bind,
            clear_pipeline,
            inject_pipeline,
            resolve_pipeline,
            march_g1,
            comp_g1,
            march_g0_bind,
            march_pipeline,
            composite_pipeline,
            march_target: None,
            march_size: (0, 0),
            tuning: Tuning::default(),
            blob_count: 0,
            frames_marched: 0,
            last_blob_count: 0,
        }
    }

    pub fn active(&self) -> bool {
        self.tuning.mode != 0
    }

    /// True when the legacy billboards should be suppressed: the volumetric path owns the
    /// smoke. Mode 2 ("both") deliberately keeps them, which is the whole point of it.
    pub fn suppresses_billboards(&self) -> bool {
        self.tuning.mode == 1
    }

    pub fn set_tuning(&mut self, t: &Tuning) {
        self.tuning = *t;
        self.tuning.cell_size = self.tuning.cell_size.clamp(0.1, 8.0);
        self.tuning.march_steps = self.tuning.march_steps.clamp(4, 256);
        self.tuning.sun_steps = self.tuning.sun_steps.min(16);
        self.tuning.sun_step_len = self.tuning.sun_step_len.clamp(0.1, 32.0);
        self.tuning.scale = self.tuning.scale.clamp(1, 4);
    }

    /// This frame's particles. Truncated at MAX_BLOBS; the count the dev panel shows is the
    /// number ACCEPTED, so a clipped plume is visible rather than silent.
    pub fn set_blobs(&mut self, queue: &wgpu::Queue, blobs: &[SmokeBlob]) {
        let n = blobs.len().min(MAX_BLOBS);
        self.blob_count = n as u32;
        self.last_blob_count = n as u32;
        if n > 0 {
            queue.write_buffer(&self.blob_buf, 0, bytemuck::cast_slice(&blobs[..n]));
        }
    }

    fn ensure_march_target(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        if self.march_target.is_some() && self.march_size == (w, h) {
            return;
        }
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_smoke_volume_march_target"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: MARCH_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.march_target = Some((tex, view));
        self.march_size = (w, h);
    }

    /// Record the whole thing: clear, inject, resolve, march, composite.
    ///
    /// `depth` is a SINGLE-SAMPLE scene depth view not attached to any pass at this point in
    /// the frame (`Gfx3d::water_depth_view` after `ensure_far_depth_resolve`) -- the same one
    /// the god rays and the cloud march read, and for the same reason.
    #[allow(clippy::too_many_arguments)]
    pub fn record(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        scene: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        render_size: (u32, u32),
        cam_pos: [f32; 3],
        inv_view_proj: [f32; 16],
        sun_dir: [f32; 3],
        sun_radiance: [f32; 3],
        ambient: [f32; 3],
    ) {
        if !self.active() || render_size.0 == 0 || render_size.1 == 0 {
            return;
        }
        let scale = self.tuning.scale.max(1);
        let mw = (render_size.0 / scale).max(1);
        let mh = (render_size.1 / scale).max(1);
        self.ensure_march_target(device, mw, mh);

        // SNAP the grid origin to the cell size. Without this the field slides continuously
        // under the camera and every trilinear sample crawls, which reads as the whole plume
        // shimmering when you walk -- the classic camera-anchored-volume artefact.
        let cell = self.tuning.cell_size;
        let half = [
            GRID_W as f32 * cell * 0.5,
            GRID_H as f32 * cell * 0.5,
            GRID_D as f32 * cell * 0.5,
        ];
        let origin = [
            ((cam_pos[0] - half[0]) / cell).floor() * cell,
            ((cam_pos[1] - half[1]) / cell).floor() * cell,
            ((cam_pos[2] - half[2]) / cell).floor() * cell,
        ];

        let p = GpuParams {
            origin_cell: [origin[0], origin[1], origin[2], cell],
            dims_steps: [
                GRID_W as f32,
                GRID_H as f32,
                GRID_D as f32,
                self.tuning.march_steps as f32,
            ],
            cam_albedo: [cam_pos[0], cam_pos[1], cam_pos[2], self.tuning.albedo],
            sun_dir_steps: [
                sun_dir[0],
                sun_dir[1],
                sun_dir[2],
                self.tuning.sun_steps as f32,
            ],
            sun_radiance: [
                sun_radiance[0],
                sun_radiance[1],
                sun_radiance[2],
                self.tuning.sun_step_len,
            ],
            ambient_ext: [ambient[0], ambient[1], ambient[2], self.tuning.extinction],
            tune: [
                self.tuning.density,
                self.tuning.anisotropy,
                self.tuning.jitter,
                self.tuning.self_shadow,
            ],
            sizes: [
                mw as f32,
                mh as f32,
                render_size.0 as f32,
                render_size.1 as f32,
            ],
            inv_view_proj,
            counts: [self.blob_count, 0, 0, 0],
        };
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(&p));

        encoder.push_debug_group("wgr_smoke_volume");
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_smoke_volume_build"),
                timestamp_writes: None,
            });
            pass.set_bind_group(0, &self.compute_bind, &[]);
            pass.set_pipeline(&self.clear_pipeline);
            pass.dispatch_workgroups(CELL_COUNT.div_ceil(64), 1, 1);
            if self.blob_count > 0 {
                pass.set_pipeline(&self.inject_pipeline);
                // One workgroup per particle -- see the shader header for why this is not a
                // per-froxel gather.
                pass.dispatch_workgroups(self.blob_count, 1, 1);
            }
            pass.set_pipeline(&self.resolve_pipeline);
            pass.dispatch_workgroups(GRID_W.div_ceil(4), GRID_H.div_ceil(4), GRID_D.div_ceil(4));
        }

        let Some((_, march_view)) = self.march_target.as_ref() else {
            encoder.pop_debug_group();
            return;
        };
        let march_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_smoke_volume_march_g1_bind"),
            layout: &self.march_g1,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&self.field_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.field_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
            ],
        });
        let comp_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_smoke_volume_comp_g1_bind"),
            layout: &self.comp_g1,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(march_view),
                },
            ],
        });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_smoke_volume_march"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: march_view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(&self.march_pipeline);
            pass.set_bind_group(0, &self.march_g0_bind, &[]);
            pass.set_bind_group(1, &march_bind, &[]);
            pass.draw(0..3, 0..1);
        }
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_smoke_volume_composite"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: scene,
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
            pass.set_pipeline(&self.composite_pipeline);
            pass.set_bind_group(0, &self.march_g0_bind, &[]);
            pass.set_bind_group(1, &comp_bind, &[]);
            pass.draw(0..3, 0..1);
        }
        encoder.pop_debug_group();
        self.frames_marched += 1;
    }
}
