//! FAR INSTANCE TIER — a cheap proxy for EVERY authored placement, fed straight from the
//! world's placement rows and never instantiated as an Object.
//!
//! Why it exists: the object residency window stops at ~900 m on Everon and ~550-711 m on
//! Chernarus. Beyond it placements are not drawn coarsely, they are not drawn at all — the
//! world is bare ground and assets pop into being as the camera closes. That is most
//! visible from the air, which is exactly where the residency window is least able to help
//! (widening it is a memory and simulation cost, not a rendering one).
//!
//! Ownership follows the grass module: C++ uploads one bulk description ONCE (per world),
//! the renderer owns the per-frame cull and both draws, and the C++ side never walks a
//! proxy again. That is the whole affordability argument — the per-frame cost is one
//! compute sweep over a storage buffer, measured at 0.05-0.14 ms for 1.2M placements and
//! 0.15-0.23 ms for 3.65M.
//!
//! Sub-pixel rejection is the load-bearing part of the cull, not a refinement: at a 2 px
//! limit an aircraft view drops 421,323 candidates to 22,135 and its draw from 0.314 ms to
//! 0.025 ms. Distance and frustum tests alone leave the tier costing several times what it
//! is worth.
//!
//! Deliberately NOT in the depth/normal prepass. These are crude proxy silhouettes; letting
//! them feed the Hi-Z pyramid would let a box occlude the real geometry it stands in for.

use crate::ffi::WgrFarInstance;
use crate::gfx3d::DEPTH_FORMAT;

/// Compacted-output partition sizes, in instances. One buffer, two fixed partitions: cards
/// take [0, CARD_CAP) and prisms [CARD_CAP, CARD_CAP + PRISM_CAP). A partition rather than a
/// shared arena because the prism draw then needs no `first_instance` (an optional wgpu
/// capability here — see WGR_RUNTIME_CAP_INDIRECT_FIRST_INSTANCE); its vertex shader just
/// offsets its instance index. Sized against the measured worst case: the whole island at
/// 12 km with sub-pixel rejection on is 693,525 visible.
const CARD_CAP: u32 = 1_048_576;
const PRISM_CAP: u32 = 262_144;
const VISIBLE_CAP: u32 = CARD_CAP + PRISM_CAP;
/// Bytes per compacted record — mirrors `FarVisible` in far.wgsl.
const VISIBLE_STRIDE: u64 = 32;

/// Vertices per proxy. Cards are 2 triangles, prisms 12 — measured at ~4x the card's draw
/// cost, which is the entire reason the source rows carry a class bit.
const CARD_VERTS: u32 = 6;
const PRISM_VERTS: u32 = 36;

/// The compute's workgroup size, and the per-dimension workgroup limit. A world with more
/// than `WORKGROUP * MAX_GROUPS_X` placements is dispatched as a 2D grid.
const WORKGROUP: u32 = 64;
const MAX_GROUPS_X: u32 = 65535;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct FarParams {
    /// x = near cutoff (m), y = far distance (m), z = sub-pixel limit (px), w = source count
    cfg: [f32; 4],
    /// x = pixels per radian, y = enabled, z = dispatch row stride (invocations), w spare
    tune: [f32; 4],
}

/// Everything that only exists once a world has actually pushed placements. A world with no
/// far tier (or one launched with the tier off) allocates none of it — the compacted buffer
/// alone is 42 MB.
struct FarData {
    // Held as explicit ownership anchors. The bind group keeps the underlying resources
    // alive on its own, so nothing reads these again — dropping the fields would still
    // work and would hide which buffers this module actually allocated.
    #[allow(dead_code)]
    src: wgpu::Buffer,
    #[allow(dead_code)]
    visible: wgpu::Buffer,
    counts: wgpu::Buffer,
    indirect: wgpu::Buffer,
    bind: wgpu::BindGroup,
    /// Workgroups along X; Y covers the remainder for worlds past the per-dimension limit.
    groups_x: u32,
    groups_y: u32,
}

pub struct Far {
    params: FarParams,
    params_buffer: wgpu::Buffer,
    layout: wgpu::BindGroupLayout,
    cull_pipeline: wgpu::ComputePipeline,
    card_pipeline: wgpu::RenderPipeline,
    prism_pipeline: wgpu::RenderPipeline,
    data: Option<FarData>,
}

impl Far {
    pub fn new(
        device: &wgpu::Device,
        camera_layout: &wgpu::BindGroupLayout,
        color_format: wgpu::TextureFormat,
        sample_count: u32,
        composer: &mut naga_oil::compose::Composer,
    ) -> Self {
        let params = FarParams {
            // Inert until the game pushes real values: a far distance of 0 rejects
            // everything, which is the right state for a frame that has no world yet.
            cfg: [0.0, 0.0, 2.0, 0.0],
            tune: [0.0, 0.0, WORKGROUP as f32, 0.0],
        };
        let params_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_far_params"),
            size: std::mem::size_of::<FarParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_far_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE
                        | wgpu::ShaderStages::VERTEX
                        | wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                // The authored rows are read by the cull alone.
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                // The compacted survivors: written by the cull, read by both vertex stages.
                // Bound read_write in the vertex stage, which needs VERTEX_WRITABLE_STORAGE
                // — already a hard device requirement here (the grass field needs it too).
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE | wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_far_shader",
            include_str!("far.wgsl"),
            "far/far.wgsl",
        );
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_far_pipeline_layout"),
            bind_group_layouts: &[Some(camera_layout), Some(&layout)],
            immediate_size: 0,
        });
        let cull_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_far_cull"),
            layout: Some(&pipeline_layout),
            module: &shader,
            entry_point: Some("cs_cull"),
            compilation_options: Default::default(),
            cache: None,
        });
        // Copied verbatim from the GPU-driven object pipeline (gfx3d/cull.rs): the depth
        // buffer is REVERSED-Z, so the comparison is GreaterEqual and nothing else. Proxies
        // write depth — they are opaque stand-ins for opaque things, and without the write
        // a nearer proxy would not hide a farther one.
        let depth_stencil = wgpu::DepthStencilState {
            format: DEPTH_FORMAT,
            depth_write_enabled: Some(true),
            depth_compare: Some(wgpu::CompareFunction::GreaterEqual), // reversed-Z
            stencil: wgpu::StencilState::default(),
            bias: wgpu::DepthBiasState::default(),
        };
        let make_pipeline = |label: &str, vs: &str, fs: &str| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some(vs),
                    compilation_options: Default::default(),
                    buffers: &[],
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    // No back-face culling for EITHER proxy. A card is a billboard and is
                    // seen from both sides by construction; the prism's winding is inherited
                    // from the experiment's corner table and has never been verified against
                    // this pipeline's front_face, and a silently-inverted box that vanishes
                    // is a far worse outcome than the handful of back faces early-Z rejects.
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(depth_stencil.clone()),
                multisample: wgpu::MultisampleState {
                    count: sample_count,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(fs),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: color_format,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let card_pipeline = make_pipeline("wgr_far_cards", "vs_card", "fs_card");
        let prism_pipeline = make_pipeline("wgr_far_prisms", "vs_prism", "fs_prism");
        Self {
            params,
            params_buffer,
            layout,
            cull_pipeline,
            card_pipeline,
            prism_pipeline,
            data: None,
        }
    }

    /// Replace the whole proxy set. Called once per world load; an empty slice releases the
    /// ~42 MB of compacted storage as well as the source rows. Returns (cards, prisms) so
    /// the caller can log a denominator rather than a reassuring "ok".
    pub fn set_instances(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        instances: &[WgrFarInstance],
    ) -> (u32, u32) {
        self.data = None;
        if instances.is_empty() {
            self.params.cfg[3] = 0.0;
            self.upload_params(queue);
            return (0, 0);
        }
        // One mapped-at-creation write rather than queue.write_buffer: this is a single
        // bulk upload of up to ~117 MB (3.65M rows), and pushing that through the staging
        // belt copies it twice for no benefit.
        let bytes: &[u8] = bytemuck::cast_slice(instances);
        let src = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_far_src"),
            size: bytes.len() as u64,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: true,
        });
        src.slice(..).get_mapped_range_mut().copy_from_slice(bytes);
        src.unmap();

        let visible = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_far_visible"),
            size: VISIBLE_CAP as u64 * VISIBLE_STRIDE,
            usage: wgpu::BufferUsages::STORAGE,
            mapped_at_creation: false,
        });
        let counts = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_far_counts"),
            size: 8,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_SRC
                | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Two 16-byte draw-indirect arg blocks: cards at 0, prisms at 16.
        let indirect = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_far_indirect"),
            size: 32,
            usage: wgpu::BufferUsages::INDIRECT | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_far_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: self.params_buffer.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: src.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: visible.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: counts.as_entire_binding(),
                },
            ],
        });

        let count = instances.len() as u32;
        let groups = count.div_ceil(WORKGROUP);
        let groups_x = groups.min(MAX_GROUPS_X);
        let groups_y = groups.div_ceil(groups_x.max(1));
        self.params.cfg[3] = count as f32;
        self.params.tune[2] = (groups_x * WORKGROUP) as f32;
        self.upload_params(queue);
        self.data = Some(FarData {
            src,
            visible,
            counts,
            indirect,
            bind,
            groups_x,
            groups_y,
        });

        let prisms = instances
            .iter()
            .filter(|i| (i.flags & crate::ffi::WGR_FAR_FLAG_PRISM) != 0)
            .count() as u32;
        (count - prisms, prisms)
    }

    /// Per-frame knobs from the game. Cheap and idempotent — the uniform is 32 bytes.
    pub fn set_params(
        &mut self,
        queue: &wgpu::Queue,
        near_cutoff_m: f32,
        far_distance_m: f32,
        pixel_limit: f32,
        enabled: bool,
    ) {
        self.params.cfg[0] = near_cutoff_m.max(0.0);
        self.params.cfg[1] = far_distance_m.max(0.0);
        self.params.cfg[2] = pixel_limit;
        self.params.tune[1] = if enabled { 1.0 } else { 0.0 };
        self.upload_params(queue);
    }

    /// Pixels per radian for the main scene camera, `(viewport_height / 2) * proj[1][1]`.
    /// The sub-pixel test is meaningless without it — a fixed metric threshold is correct at
    /// exactly one resolution — so a zero here disables that test rather than guessing.
    pub fn set_view_metrics(&mut self, queue: &wgpu::Queue, px_per_radian: f32) {
        if (self.params.tune[0] - px_per_radian).abs() > 1e-3 {
            self.params.tune[0] = px_per_radian;
            self.upload_params(queue);
        }
    }

    fn upload_params(&self, queue: &wgpu::Queue) {
        queue.write_buffer(&self.params_buffer, 0, bytemuck::bytes_of(&self.params));
    }

    /// True when there is a set to sweep and the game has not switched the tier off.
    pub fn active(&self) -> bool {
        self.data.is_some() && self.params.tune[1] >= 0.5 && self.params.cfg[1] > 0.0
    }

    /// One compute sweep over EVERY placement, compacted into the two indirect draws.
    /// Recorded next to the main object cull so wgpu barriers its storage writes against
    /// the indirect reads later in the frame.
    pub fn dispatch(
        &self,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        camera_bind: &wgpu::BindGroup,
        camera_offset: u32,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.active() {
            return;
        }
        let Some(data) = &self.data else {
            return;
        };
        // Reset before the sweep, not after: the counters are the compaction cursors and the
        // indirect blocks carry the fixed vertex counts alongside them.
        queue.write_buffer(&data.counts, 0, bytemuck::cast_slice(&[0u32, 0u32]));
        queue.write_buffer(
            &data.indirect,
            0,
            bytemuck::cast_slice(&[CARD_VERTS, 0u32, 0u32, 0u32, PRISM_VERTS, 0u32, 0u32, 0u32]),
        );
        use crate::gpu_timers::Region;
        timers.begin(encoder, Region::FarCull);
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_far_cull"),
                timestamp_writes: None,
            });
            pass.set_pipeline(&self.cull_pipeline);
            pass.set_bind_group(0, camera_bind, &[camera_offset]);
            pass.set_bind_group(1, &data.bind, &[]);
            pass.dispatch_workgroups(data.groups_x, data.groups_y, 1);
        }
        timers.end(encoder, Region::FarCull);
        // The draws consume a dedicated indirect buffer: a buffer bound STORAGE read_write
        // cannot also be the indirect source, which is the same split the grass placement
        // compute makes for the same reason.
        encoder.copy_buffer_to_buffer(&data.counts, 0, &data.indirect, 4, 4);
        encoder.copy_buffer_to_buffer(&data.counts, 4, &data.indirect, 20, 4);
    }

    /// Both proxy draws, as ops inside the shared 3D colour sub-pass. Cards first: they are
    /// the overwhelming majority and the cheaper pipeline, so the prisms' larger fragments
    /// meet a depth buffer that already rejects most of what they cover.
    pub fn draw(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        camera_bind: &wgpu::BindGroup,
        camera_offset: u32,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.active() {
            return;
        }
        let Some(data) = &self.data else {
            return;
        };
        use crate::gpu_timers::Region;
        // An op inside a shared pass, so this bracket needs TIMESTAMP_QUERY_INSIDE_PASSES
        // and reads "n/a" without it — the cull row still reports either way.
        timers.begin_pass(pass, Region::FarDraw);
        pass.set_pipeline(&self.card_pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, &data.bind, &[]);
        pass.draw_indirect(&data.indirect, 0);
        pass.set_pipeline(&self.prism_pipeline);
        pass.set_bind_group(0, camera_bind, &[camera_offset]);
        pass.set_bind_group(1, &data.bind, &[]);
        pass.draw_indirect(&data.indirect, 16);
        timers.end_pass(pass, Region::FarDraw);
    }
}

// A composed-and-validated shader is NOT a valid pipeline: naga never sees a bind group
// layout, so a binding whose declared type or visibility disagrees with the layout compiles
// clean and then panics at pipeline creation — which presents as the game exiting instantly
// with nothing on screen. These build the real pipelines against the real group(0) camera
// layout on a real device, which is the only place that disagreement is visible.
#[cfg(test)]
mod tests {
    use super::*;

    fn headless() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        // The compacted buffer is bound read_write to a VERTEX stage. Not optional here, and
        // not optional in production either — the renderer refuses to start without it.
        if !adapter
            .features()
            .contains(wgpu::Features::VERTEX_WRITABLE_STORAGE)
        {
            return None;
        }
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            required_features: wgpu::Features::VERTEX_WRITABLE_STORAGE,
            ..Default::default()
        }))
        .ok()
    }

    fn row(height: f32, flags: u32) -> WgrFarInstance {
        WgrFarInstance {
            x: 1000.0,
            y: 12.0,
            z: -2000.0,
            height,
            radius: 3.0,
            colour: 0xFF3F5A2Cu32,
            flags,
            pad: 0,
        }
    }

    #[test]
    fn far_pipelines_build_and_the_set_is_releasable() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let camera_layout = crate::gfx3d::camera_layout_for_tests(&device);
        let mut composer = crate::shaders::build_composer();
        let mut far = Far::new(&device, &camera_layout, crate::HDR_FORMAT, 1, &mut composer);
        // Nothing uploaded and no params pushed: the tier must be inert, not merely quiet.
        // This is the state every frame before a world loads, and the state WGR_FAR_TIER=0
        // leaves it in permanently.
        assert!(!far.active());

        let rows = vec![row(14.0, 0), row(9.0, WGR_FAR_FLAG_PRISM_TEST), row(0.5, 0)];
        let (cards, prisms) = far.set_instances(&device, &queue, &rows);
        assert_eq!((cards, prisms), (2, 1));
        // Params still unset, so a live buffer alone must NOT switch the tier on — otherwise
        // "off" would depend on nothing having been loaded yet.
        assert!(!far.active());
        far.set_params(&queue, 700.0, 12000.0, 2.0, true);
        far.set_view_metrics(&queue, 540.0);
        assert!(far.active());
        // The kill switch has to work with a set resident, which is the only case that matters.
        far.set_params(&queue, 700.0, 12000.0, 2.0, false);
        assert!(!far.active());

        far.set_instances(&device, &queue, &[]);
        far.set_params(&queue, 700.0, 12000.0, 2.0, true);
        assert!(!far.active());
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }

    const WGR_FAR_FLAG_PRISM_TEST: u32 = crate::ffi::WGR_FAR_FLAG_PRISM;

    // The compacted partition is addressed by the prism vertex shader as CARD_CAP + index,
    // so the two constants in far.wgsl and the buffer this module allocates have to agree.
    // They are separately authored, which is exactly why this is checked.
    #[test]
    fn the_shader_partition_matches_the_allocated_buffer() {
        let src = include_str!("far.wgsl");
        assert!(src.contains(&format!("const CARD_CAP: u32 = {}u;", CARD_CAP)));
        assert!(src.contains(&format!("const PRISM_CAP: u32 = {}u;", PRISM_CAP)));
        assert_eq!(VISIBLE_CAP, CARD_CAP + PRISM_CAP);
        // The compacted record's Rust-side stride against the WGSL struct it mirrors.
        assert_eq!(VISIBLE_STRIDE, std::mem::size_of::<WgrFarInstance>() as u64);
    }
}
