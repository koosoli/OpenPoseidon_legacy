// Per-model sky-visibility bake (docs/interior-sky-visibility-plan.md §3c/§3d, Stage 2).
//
// Rasterise ONE model's geometry into a depth map per sky direction, in MODEL space, then reduce
// those maps into a small model-space volume holding the cosine-weighted fraction of sky that
// reaches each voxel. Every instance of that model then samples the same volume, at any position
// or rotation, for one trilinear tap.
//
// This exists because the per-frame camera-space map it replaces could not be fixed by tuning.
// That map's texel grid has no relationship to the surfaces receiving the result, so the boundary
// between lit and occluded lands on the grid rather than on the wall — which reads as hard,
// geometry-unrelated shadow patches indoors. Widening the kernel or improving the bias moves that
// boundary; it cannot make it follow the wall, because the information needed is not in the map.
// A model-space grid does not have that problem by construction.
//
// It is affordable at high quality precisely because it is paid ONCE per model rather than once
// per frame: ~64 directions and a fine raster cost nothing on a per-frame budget when the answer
// is reused for the lifetime of the process.
//
// WHAT IS BAKED IS VISIBILITY, NOT LIGHT. The volume stores geometry occlusion only; the sky
// radiance it modulates is still the per-frame SH projection, so the result tracks time of day
// exactly as the current path does. Baking does not freeze the lighting — it freezes the shape of
// the building, which is the part that does not change.

/// Volume resolution. Deliberately coarse: this is a smooth, low-frequency quantity (the fraction
/// of sky reaching a point), and the whole reason for a model-space grid is that its boundaries
/// follow geometry — not that it resolves fine detail.
#[derive(Clone, Copy, Debug)]
pub struct BakeSettings {
    pub dims: [u32; 3],
    /// Depth-map edge in texels, per direction. This is what sets whether a window reveal is
    /// resolved: a 20 m building at 1024 is ~2 cm/texel.
    pub map_res: u32,
    /// Depth bias in NDC units. Stops a voxel sitting on a surface from being occluded by it.
    pub bias: f32,
    /// Padding added around the model's AABB, as a fraction of its size, so voxels just outside
    /// the geometry still land inside the volume and read as open.
    pub pad: f32,
}

impl Default for BakeSettings {
    fn default() -> Self {
        BakeSettings {
            dims: [32, 16, 32],
            map_res: 1024,
            bias: 0.002,
            pad: 0.05,
        }
    }
}

impl BakeSettings {
    pub fn voxel_count(&self) -> u32 {
        self.dims[0] * self.dims[1] * self.dims[2]
    }
}

/// The sampled sky directions: zenith plus two rings. Affordable at this count precisely because
/// the bake is paid once per MODEL — the per-frame path could only afford five, and the shallow
/// ring is the one that reaches through a vertical opening.
///
/// xyz points from the surface TOWARD the sky; w is the cosine weight (dot with up), so the
/// zenith dominates and the response stays diffuse.
pub fn hemisphere_directions() -> Vec<glam::Vec4> {
    let mut dirs = vec![glam::Vec4::new(0.0, 1.0, 0.0, 1.0)];
    for (tilt_deg, count) in [(30.0f32, 8u32), (55.0f32, 16u32), (75.0f32, 16u32)] {
        let t = tilt_deg.to_radians();
        let (st, ct) = t.sin_cos();
        for i in 0..count {
            let az = std::f32::consts::TAU * i as f32 / count as f32;
            let (sa, ca) = az.sin_cos();
            dirs.push(glam::Vec4::new(st * ca, ct, st * sa, ct));
        }
    }
    dirs
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct BakeParams {
    bbox_min: [f32; 4],
    bbox_max: [f32; 4],
    dims: [u32; 4],
    bias: [f32; 4],
}

/// One model's geometry, in model space, as the bake needs it.
pub struct BakeMesh<'a> {
    pub positions: &'a [[f32; 3]],
    pub indices: &'a [u32],
}

/// Where the bake reads geometry from. `Mesh` is the standalone path (tests, tools): it uploads
/// its own buffers and derives the AABB itself. `Pool` is the renderer path: it rasterises
/// straight out of the shared geometry pool, so there is no CPU copy of the mesh and no second
/// source of truth for what a model's geometry is.
pub enum BakeSource<'a> {
    Mesh(&'a BakeMesh<'a>),
    Pool {
        vbuf: &'a wgpu::Buffer,
        ibuf: &'a wgpu::Buffer,
        ranges: &'a [PoolRange],
        bbox_min: [f32; 3],
        bbox_max: [f32; 3],
    },
}

/// The pipelines and layouts for the bake, built once and reused for every model.
/// One draw range into the shared geometry pool: (first_index, index_count, base_vertex).
/// Exactly what the cull's section table already holds, so the bake consumes the renderer's
/// existing geometry with no CPU copy of it.
pub type PoolRange = (u32, u32, i32);

pub struct SkyBake {
    depth_pipeline: wgpu::RenderPipeline,
    depth_layout: wgpu::BindGroupLayout,
    reduce_pipeline: wgpu::ComputePipeline,
    reduce_layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
}

const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;

impl SkyBake {
    /// `vertex_stride` is the byte stride of the vertex buffer the bake will read. The renderer
    /// passes the geometry pool's stride so the bake can rasterise straight out of the pool;
    /// tests pass 12 for a bare position array. Position must be at offset 0 in both.
    pub fn new(device: &wgpu::Device, vertex_stride: u64) -> Self {
        let depth_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_sky_bake_depth"),
            source: wgpu::ShaderSource::Wgsl(include_str!("sky_bake_depth.wgsl").into()),
        });
        let reduce_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_sky_bake"),
            source: wgpu::ShaderSource::Wgsl(include_str!("sky_bake.wgsl").into()),
        });

        let depth_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_bake_depth_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let depth_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_sky_bake_depth_pl"),
            bind_group_layouts: &[Some(&depth_layout)],
            immediate_size: 0,
        });
        let depth_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_sky_bake_depth_pipeline"),
            layout: Some(&depth_pl),
            vertex: wgpu::VertexState {
                module: &depth_shader,
                entry_point: Some("vs_bake_depth"),
                compilation_options: Default::default(),
                buffers: &[wgpu::VertexBufferLayout {
                    array_stride: vertex_stride,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &[wgpu::VertexAttribute {
                        format: wgpu::VertexFormat::Float32x3,
                        offset: 0,
                        shader_location: 0,
                    }],
                }],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                // No culling: a building's walls and roofs are frequently single-sided, and a
                // back-face-culled bake would simply not see them.
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::LessEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            fragment: None,
            multiview_mask: None,
            cache: None,
        });

        let storage = |binding: u32, read_only: bool| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let reduce_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_bake_layout"),
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
                storage(1, true),
                storage(2, true),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                    count: None,
                },
                storage(5, false),
            ],
        });
        let reduce_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_sky_bake_pl"),
            bind_group_layouts: &[Some(&reduce_layout)],
            immediate_size: 0,
        });
        let reduce_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_sky_bake_pipeline"),
            layout: Some(&reduce_pl),
            module: &reduce_shader,
            entry_point: Some("cs_bake"),
            compilation_options: Default::default(),
            cache: None,
        });

        SkyBake {
            depth_pipeline,
            depth_layout,
            reduce_pipeline,
            reduce_layout,
            sampler: device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("wgr_sky_bake_cmp"),
                address_mode_u: wgpu::AddressMode::ClampToEdge,
                address_mode_v: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Linear,
                min_filter: wgpu::FilterMode::Linear,
                compare: Some(wgpu::CompareFunction::LessEqual),
                ..Default::default()
            }),
        }
    }

    /// Bake one model. Returns the visibility volume as `dims.x*dims.y*dims.z` floats in
    /// x-major order, 1 = open sky, 0 = fully enclosed, plus the padded model-space AABB the
    /// volume covers (which the sampler needs to map a model-space position into it).
    ///
    /// Synchronous: it submits and waits. That is fine for a load-time bake and is exactly what
    /// §3d says must NOT happen on the main thread for a whole model library — the caching and
    /// background scheduling live above this, so that this stays a pure, testable function.
    /// Bake one model. Returns the visibility volume as `dims.x*dims.y*dims.z` floats in
    /// x-major order, 1 = open sky, 0 = fully enclosed, plus the padded model-space AABB the
    /// volume covers (which the sampler needs to map a model-space position into it).
    ///
    /// Synchronous: it submits and waits. That is fine for a load-time bake and is exactly what
    /// §3d says must NOT happen on the main thread for a whole model library — the caching and
    /// background scheduling live above this, so that this stays a pure, testable function.
    // Blocking form: submit + wait + read. Used by the unit tests, where a stall is fine.
    pub fn bake(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        src: BakeSource<'_>,
        dirs: &[glam::Vec4],
        s: &BakeSettings,
    ) -> Option<(Vec<[f32; 4]>, [f32; 3], [f32; 3])> {
        let pending = self.bake_begin(device, queue, src, dirs, s)?;
        if device.poll(wgpu::PollType::wait_indefinitely()).is_err() {
            return None;
        }
        pending.try_take()?
    }

    // Non-blocking form: encode + submit the whole bake and start the readback map, but do
    // NOT wait. The caller keeps the PendingBake and calls try_take() once the map has
    // completed (after a later poll/submit) — this is what keeps a per-model load-time bake
    // from stalling the main thread on a GPU round-trip (measured 17 ms avg, 173 ms max per
    // registration on the Everon traverse).
    pub fn bake_begin(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        src: BakeSource<'_>,
        dirs: &[glam::Vec4],
        s: &BakeSettings,
    ) -> Option<PendingBake> {
        if dirs.is_empty() {
            return None;
        }
        // Own the buffers only on the standalone path; the pool path borrows the renderer's.
        let mut owned: Option<(wgpu::Buffer, wgpu::Buffer)> = None;
        let (vbuf, ibuf, ranges, lo, hi) = match src {
            BakeSource::Mesh(mesh) => {
                if mesh.positions.is_empty() || mesh.indices.is_empty() {
                    return None;
                }
                let (mut lo, mut hi) = ([f32::MAX; 3], [f32::MIN; 3]);
                for p in mesh.positions {
                    for a in 0..3 {
                        lo[a] = lo[a].min(p[a]);
                        hi[a] = hi[a].max(p[a]);
                    }
                }
                owned = Some((
                    device.create_buffer_init_slice(
                        queue,
                        "wgr_sky_bake_vbuf",
                        bytemuck::cast_slice(mesh.positions),
                        wgpu::BufferUsages::VERTEX,
                    ),
                    device.create_buffer_init_slice(
                        queue,
                        "wgr_sky_bake_ibuf",
                        bytemuck::cast_slice(mesh.indices),
                        wgpu::BufferUsages::INDEX,
                    ),
                ));
                let (v, i) = owned.as_ref().unwrap();
                (v, i, vec![(0u32, mesh.indices.len() as u32, 0i32)], lo, hi)
            }
            // Straight out of the renderer's geometry pool: no CPU copy of the mesh, and no
            // second source of truth for what a model's geometry is. The AABB comes from the
            // meshes, which computed it once when their vertices were still on the CPU.
            BakeSource::Pool {
                vbuf,
                ibuf,
                ranges,
                bbox_min,
                bbox_max,
            } => {
                if ranges.is_empty() {
                    return None;
                }
                (vbuf, ibuf, ranges.to_vec(), bbox_min, bbox_max)
            }
        };
        let mut bbox_min = [0.0f32; 3];
        let mut bbox_max = [0.0f32; 3];
        for a in 0..3 {
            // A degenerate axis (a flat model) would give a zero-width box and a division by zero
            // in the mapping, so floor the extent.
            let ext = (hi[a] - lo[a]).max(0.01);
            let pad = ext * s.pad;
            bbox_min[a] = lo[a] - pad;
            bbox_max[a] = hi[a] + pad;
        }
        let centre = glam::Vec3::new(
            (bbox_min[0] + bbox_max[0]) * 0.5,
            (bbox_min[1] + bbox_max[1]) * 0.5,
            (bbox_min[2] + bbox_max[2]) * 0.5,
        );
        // One radius covers the box from every direction, so every direction's ortho box is the
        // same size and no direction can clip the model.
        let radius = 0.5
            * glam::Vec3::new(
                bbox_max[0] - bbox_min[0],
                bbox_max[1] - bbox_min[1],
                bbox_max[2] - bbox_min[2],
            )
            .length();

        let n = dirs.len() as u32;
        let maps = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_sky_bake_maps"),
            size: wgpu::Extent3d {
                width: s.map_res,
                height: s.map_res,
                depth_or_array_layers: n,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: DEPTH_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });

        // One ortho VP per direction, model space -> that direction's clip space.
        let mut vps: Vec<[f32; 16]> = Vec::with_capacity(dirs.len());
        for d in dirs {
            vps.push(direction_vp(centre, d.truncate(), radius).to_cols_array());
        }

        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("wgr_sky_bake"),
        });
        for (i, vp) in vps.iter().enumerate() {
            let ubo = device.create_buffer_init_slice(
                queue,
                "wgr_sky_bake_vp",
                bytemuck::cast_slice(vp),
                wgpu::BufferUsages::UNIFORM,
            );
            let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_sky_bake_depth_bind"),
                layout: &self.depth_layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: ubo.as_entire_binding(),
                }],
            });
            let view = maps.create_view(&wgpu::TextureViewDescriptor {
                label: Some("wgr_sky_bake_layer"),
                dimension: Some(wgpu::TextureViewDimension::D2),
                base_array_layer: i as u32,
                array_layer_count: Some(1),
                ..Default::default()
            });
            let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_sky_bake_depth_pass"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &view,
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(1.0),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            rp.set_pipeline(&self.depth_pipeline);
            rp.set_bind_group(0, &bind, &[]);
            rp.set_vertex_buffer(0, vbuf.slice(..));
            rp.set_index_buffer(ibuf.slice(..), wgpu::IndexFormat::Uint32);
            for &(first, count, base) in ranges.iter() {
                rp.draw_indexed(first..(first + count), base, 0..1);
            }
        }

        let params = BakeParams {
            bbox_min: [bbox_min[0], bbox_min[1], bbox_min[2], 0.0],
            bbox_max: [bbox_max[0], bbox_max[1], bbox_max[2], 0.0],
            dims: [s.dims[0], s.dims[1], s.dims[2], n],
            bias: [s.bias, 0.0, 0.0, 0.0],
        };
        let params_buf = device.create_buffer_init_slice(
            queue,
            "wgr_sky_bake_params",
            bytemuck::bytes_of(&params),
            wgpu::BufferUsages::UNIFORM,
        );
        let vp_buf = device.create_buffer_init_slice(
            queue,
            "wgr_sky_bake_vps",
            bytemuck::cast_slice(&vps),
            wgpu::BufferUsages::STORAGE,
        );
        let dir_buf = device.create_buffer_init_slice(
            queue,
            "wgr_sky_bake_dirs",
            bytemuck::cast_slice(dirs),
            wgpu::BufferUsages::STORAGE,
        );
        // vec4 per voxel: xyz = incoming sky direction, w = visibility.
        let out_bytes = s.voxel_count() as u64 * 16;
        let out_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_sky_bake_out"),
            size: out_bytes,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let maps_view = maps.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_sky_bake_maps_view"),
            dimension: Some(wgpu::TextureViewDimension::D2Array),
            ..Default::default()
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_bake_bind"),
            layout: &self.reduce_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: vp_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: dir_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&maps_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: out_buf.as_entire_binding(),
                },
            ],
        });
        {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_sky_bake_reduce"),
                timestamp_writes: None,
            });
            cp.set_pipeline(&self.reduce_pipeline);
            cp.set_bind_group(0, &bind, &[]);
            cp.dispatch_workgroups(
                s.dims[0].div_ceil(4),
                s.dims[1].div_ceil(4),
                s.dims[2].div_ceil(4),
            );
        }

        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_sky_bake_readback"),
            size: out_bytes,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        encoder.copy_buffer_to_buffer(&out_buf, 0, &staging, 0, out_bytes);
        queue.submit(std::iter::once(encoder.finish()));

        let slice = staging.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        Some(PendingBake {
            staging,
            rx,
            bbox_min,
            bbox_max,
        })
    }
}

// A bake whose GPU work has been submitted and whose readback map is in flight. The map
// callback fires during a later device poll or queue submission; try_take() is cheap to
// call every frame until then.
pub struct PendingBake {
    staging: wgpu::Buffer,
    rx: std::sync::mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>,
    bbox_min: [f32; 3],
    bbox_max: [f32; 3],
}

impl PendingBake {
    // None = still in flight. Some(None) = the map failed (bake lost). Some(Some(..)) = done.
    #[allow(clippy::option_option)]
    pub fn try_take(&self) -> Option<Option<(Vec<[f32; 4]>, [f32; 3], [f32; 3])>> {
        match self.rx.try_recv() {
            Err(std::sync::mpsc::TryRecvError::Empty) => None,
            Err(std::sync::mpsc::TryRecvError::Disconnected) | Ok(Err(_)) => Some(None),
            Ok(Ok(())) => {
                let slice = self.staging.slice(..);
                let data = slice.get_mapped_range();
                let out = bytemuck::cast_slice::<u8, [f32; 4]>(&data).to_vec();
                drop(data);
                self.staging.unmap();
                Some(Some((out, self.bbox_min, self.bbox_max)))
            }
        }
    }
}

// Ortho view-projection looking along -dir at a model centred on `centre` with `radius`.
// The box is a cube of side 2*radius in every direction, so no direction clips the model and
// every direction's depth range is the same — which keeps one bias correct for all of them.
fn direction_vp(centre: glam::Vec3, dir: glam::Vec3, radius: f32) -> glam::Mat4 {
    let back = dir.normalize();
    let seed = if back.y.abs() > 0.99 {
        glam::Vec3::Z
    } else {
        glam::Vec3::Y
    };
    let right = back.cross(seed).normalize();
    let up = back.cross(right);
    let eye = centre + back * radius;
    let view = glam::Mat4::from_cols(
        glam::Vec4::new(right.x, up.x, back.x, 0.0),
        glam::Vec4::new(right.y, up.y, back.y, 0.0),
        glam::Vec4::new(right.z, up.z, back.z, 0.0),
        glam::Vec4::new(-eye.dot(right), -eye.dot(up), -eye.dot(back), 1.0),
    );
    let proj = glam::camera::rh::proj::directx::orthographic(
        -radius,
        radius,
        -radius,
        radius,
        0.0,
        2.0 * radius,
    );
    proj * view
}

// Small helper so the bake reads as intent rather than as buffer boilerplate.
trait CreateInit {
    fn create_buffer_init_slice(
        &self,
        queue: &wgpu::Queue,
        label: &str,
        bytes: &[u8],
        usage: wgpu::BufferUsages,
    ) -> wgpu::Buffer;
}

impl CreateInit for wgpu::Device {
    // Deliberately NOT `wgpu::util::create_buffer_init`. That helper maps the new buffer at
    // creation and immediately takes `get_mapped_range_mut()`, which PANICS
    // ("Error in Buffer::get_mapped_range: Validation Error / Buffer with '<label>' label is
    // invalid") when the create was refused for want of memory — and an out-of-memory create is
    // precisely what a dense world does to this bake. Measured on DayZ Chernarus at 2.85x the
    // residency budget: 173 unwinds in one ten-minute run, all on 'wgr_sky_bake_vp', each one
    // aborting the model that was being baked.
    //
    // create_buffer + write_buffer has no mapped range and therefore no panic. If the create did
    // fail, the buffer stays invalid, the bind group over it is rejected, and the depth pass that
    // would have written the occlusion maps is dropped — leaving every map at its CLEARED far
    // value, i.e. a fully OPEN volume, which is what an unbaked model already looks like. The
    // failure mode degrades toward "no volume", never toward "spuriously enclosed", so a bake
    // lost to memory pressure cannot darken anything.
    fn create_buffer_init_slice(
        &self,
        queue: &wgpu::Queue,
        label: &str,
        bytes: &[u8],
        usage: wgpu::BufferUsages,
    ) -> wgpu::Buffer {
        let buf = self.create_buffer(&wgpu::BufferDescriptor {
            label: Some(label),
            // Rounded up to the 4-byte copy alignment write_buffer requires; every caller here
            // passes whole f32/matrix/struct arrays, so this is a no-op in practice.
            size: bytes.len().next_multiple_of(4) as u64,
            usage: usage | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(&buf, 0, bytes);
        buf
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gfx3d::cull::tests::headless;

    // A closed rectangular room, 6 x 3 x 6 m, walls/floor/ceiling, with a hole in the +X wall.
    // `window` toggles that hole, so the same geometry can be baked sealed or open and the two
    // compared — which is the only way to show the bake responds to the OPENING rather than to
    // the room's shape.
    fn room(window: bool) -> (Vec<[f32; 3]>, Vec<u32>) {
        let (x0, x1) = (-3.0f32, 3.0f32);
        let (y0, y1) = (0.0f32, 3.0f32);
        let (z0, z1) = (-3.0f32, 3.0f32);
        let mut pos: Vec<[f32; 3]> = Vec::new();
        let mut idx: Vec<u32> = Vec::new();
        let mut quad = |a: [f32; 3], b: [f32; 3], c: [f32; 3], d: [f32; 3]| {
            let base = pos.len() as u32;
            pos.extend_from_slice(&[a, b, c, d]);
            idx.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
        };
        // floor + ceiling
        quad([x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1]);
        quad([x0, y1, z0], [x1, y1, z0], [x1, y1, z1], [x0, y1, z1]);
        // -X, -Z, +Z walls (solid)
        quad([x0, y0, z0], [x0, y1, z0], [x0, y1, z1], [x0, y0, z1]);
        quad([x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0]);
        quad([x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]);
        if window {
            // +X wall in four pieces around a 2 m x 1.5 m hole centred at (y 1.5, z 0).
            let (wy0, wy1, wz0, wz1) = (0.75f32, 2.25f32, -1.0f32, 1.0f32);
            quad([x1, y0, z0], [x1, wy0, z0], [x1, wy0, z1], [x1, y0, z1]); // below
            quad([x1, wy1, z0], [x1, y1, z0], [x1, y1, z1], [x1, wy1, z1]); // above
            quad([x1, wy0, z0], [x1, wy1, z0], [x1, wy1, wz0], [x1, wy0, wz0]); // -Z side
            quad([x1, wy0, wz1], [x1, wy1, wz1], [x1, wy1, z1], [x1, wy0, z1]); // +Z side
        } else {
            quad([x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]);
        }
        (pos, idx)
    }

    fn bake_room(window: bool) -> Option<(Vec<[f32; 4]>, [f32; 3], [f32; 3], BakeSettings)> {
        let (device, queue) = headless()?;
        let bake = SkyBake::new(&device, 12);
        let (pos, idx) = room(window);
        // A dome sample: straight up plus rings at 35 and 65 degrees. The shallow ring is what
        // reaches through a vertical opening — the entire reason a per-model bake beats a
        // zenith-only map.
        let mut dirs = vec![glam::Vec4::new(0.0, 1.0, 0.0, 1.0)];
        for (tilt_deg, count) in [(35.0f32, 8u32), (65.0f32, 16u32)] {
            let t = tilt_deg.to_radians();
            let (st, ct) = t.sin_cos();
            for i in 0..count {
                let az = std::f32::consts::TAU * i as f32 / count as f32;
                let (sa, ca) = az.sin_cos();
                dirs.push(glam::Vec4::new(st * ca, ct, st * sa, ct));
            }
        }
        let s = BakeSettings {
            dims: [24, 12, 24],
            map_res: 512,
            ..Default::default()
        };
        let mesh = BakeMesh {
            positions: &pos,
            indices: &idx,
        };
        let (v, lo, hi) = bake.bake(&device, &queue, BakeSource::Mesh(&mesh), &dirs, &s)?;
        Some((v, lo, hi, s))
    }

    // Sample the volume at a model-space point (nearest voxel — the test asks about regions, not
    // about interpolation).
    fn at(v: &[[f32; 4]], lo: [f32; 3], hi: [f32; 3], s: &BakeSettings, p: [f32; 3]) -> f32 {
        let mut i = [0usize; 3];
        for a in 0..3 {
            let t = ((p[a] - lo[a]) / (hi[a] - lo[a])).clamp(0.0, 0.999);
            i[a] = (t * s.dims[a] as f32) as usize;
        }
        v[i[0] + i[1] * s.dims[0] as usize + i[2] * (s.dims[0] * s.dims[1]) as usize][3]
    }

    // The core claim: inside a sealed room the sky is not visible, outside it is, and the bake
    // says so from the model's own geometry with no camera involved.
    #[test]
    fn sealed_room_is_dark_and_outside_is_open() {
        let Some((v, lo, hi, s)) = bake_room(false) else {
            return;
        };
        let inside = at(&v, lo, hi, &s, [0.0, 1.5, 0.0]);
        let above = at(&v, lo, hi, &s, [0.0, 3.4, 0.0]);
        assert!(
            inside < 0.15,
            "sealed interior should be dark, got {inside}"
        );
        assert!(above > 0.7, "above the roof should be open, got {above}");
    }

    // The criterion the per-frame map could never satisfy: with a window in the wall, the part of
    // the room beside it must receive more sky than the far corner. This is the whole reason the
    // bake exists, so it is asserted directly rather than inferred from a screenshot.
    #[test]
    fn light_reaches_in_through_a_window() {
        let Some((v, lo, hi, s)) = bake_room(true) else {
            return;
        };
        let by_window = at(&v, lo, hi, &s, [2.4, 1.5, 0.0]);
        let far_corner = at(&v, lo, hi, &s, [-2.4, 1.5, 2.4]);
        assert!(
            by_window > far_corner + 0.05,
            "the window side ({by_window}) must see more sky than the far corner ({far_corner})"
        );
    }

    // Direction, not just amount. Beside a window the baked direction must LEAN TOWARD it, which
    // is what makes a room read as lit through the opening rather than merely dimmer. Asserted
    // against the sealed room too, because in a sealed room the only way out is up — so a
    // direction that leans +X there would mean the test is measuring room shape, not the window.
    #[test]
    fn the_baked_direction_leans_toward_the_window() {
        let (Some((open, lo, hi, s)), Some((sealed, ..))) = (bake_room(true), bake_room(false))
        else {
            return;
        };
        // Just inside the +X wall, level with the opening.
        let p = [2.4f32, 1.5, 0.0];
        let dir = |v: &[[f32; 4]]| {
            let mut i = [0usize; 3];
            for a in 0..3 {
                let t = ((p[a] - lo[a]) / (hi[a] - lo[a])).clamp(0.0, 0.999);
                i[a] = (t * s.dims[a] as f32) as usize;
            }
            v[i[0] + i[1] * s.dims[0] as usize + i[2] * (s.dims[0] * s.dims[1]) as usize]
        };
        let d_open = dir(&open);
        let d_sealed = dir(&sealed);
        assert!(
            d_open[0] > 0.2,
            "beside the window the sky must arrive from +X, got {d_open:?}"
        );
        assert!(
            d_open[0] > d_sealed[0] + 0.15,
            "the +X lean must come from the opening: open {} vs sealed {}",
            d_open[0],
            d_sealed[0]
        );
    }

    // ...and the same room with the hole filled must NOT show that gradient. Without this, the
    // test above would pass on any volume that merely darkens toward corners — a shape effect,
    // not a window effect. This is the falsifier for the claim, built in.
    #[test]
    fn the_window_gradient_is_caused_by_the_window() {
        let (Some((open, lo, hi, s)), Some((sealed, ..))) = (bake_room(true), bake_room(false))
        else {
            return;
        };
        let p_win = [2.4f32, 1.5, 0.0];
        let p_far = [-2.4f32, 1.5, 2.4];
        let open_delta = at(&open, lo, hi, &s, p_win) - at(&open, lo, hi, &s, p_far);
        let sealed_delta = at(&sealed, lo, hi, &s, p_win) - at(&sealed, lo, hi, &s, p_far);
        assert!(
            open_delta > sealed_delta + 0.05,
            "the gradient must come from the opening: open {open_delta} vs sealed {sealed_delta}"
        );
    }
}

// The volume's dimensions exist twice: here, and as SKY_VOL_* constants in gpu_driven.wgsl which
// index the flat buffer. Nothing at runtime checks they agree — a mismatch silently reads the
// wrong voxels and shows up as a wrong-looking building rather than an error, which is the same
// class of failure as a Rust struct drifting from its WGSL twin.
#[test]
fn volume_dims_match_the_shader_constants() {
    let d = BakeSettings::default().dims;
    let src = include_str!("gpu_driven.wgsl");
    for (name, want) in [
        ("SKY_VOL_X", d[0]),
        ("SKY_VOL_Y", d[1]),
        ("SKY_VOL_Z", d[2]),
    ] {
        let needle = format!("const {name}: u32 = {want}u;");
        assert!(
            src.contains(&needle),
            "gpu_driven.wgsl must declare `{needle}` to match BakeSettings::default().dims"
        );
    }
}
