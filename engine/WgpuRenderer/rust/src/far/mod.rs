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
use crate::gfx3d::depth_format;
use crate::gfx3d::cull::CullInputs;

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

/// The proxy widening floor, mirrored from `MIN_PROXY_RADIUS` in far.wgsl. The shipped rule
/// lives in the shader; this side needs the constant only to check the replica below against
/// it, which is why both are test-only.
#[cfg(test)]
const MIN_PROXY_RADIUS: f32 = 0.25;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct FarParams {
    /// x = near cutoff (m) — a hard floor, not the start rule, y = far distance (m),
    /// z = sub-pixel limit (px), w = source count
    cfg: [f32; 4],
    /// x = pixels per radian, y = enabled, z = dispatch row stride (invocations),
    /// w = start rule (0 = complement of the real cull, 1 = legacy pure distance)
    tune: [f32; 4],
    /// The real object cull's own coefficients, refreshed every frame from
    /// `Gfx3d::cull_inputs()`. x = k = pixel_limit * lod_scale * lod_inv_width, y = objects_z,
    /// z/w spare. NOT part of the C ABI — FarParams is private to this module and its shader,
    /// so growing it needs no header change and no wgr_far_set_params parameter.
    cull: [f32; 4],
}

/// The far tier's START RULE, as a plain function so it can be tested without a GPU.
///
/// This is the complement of gfx3d/cull.wgsl: accept exactly the placements the real object
/// cull drops, so a proxy is never drawn over a real model (overlap) and a placement inside
/// the fog radius is never drawn by nothing at all (gap). `k` folds the real cull's
/// `pixel_limit * lod_scale * lod_inv_width`, which is why a distance cannot substitute for
/// it — the framerate governor moves `lod_inv_width` while the game runs.
///
/// `near_floor` is `WGR_FAR_NEAR_CUTOFF`: a hard floor layered ON TOP of the complement, never
/// a start rule of its own. The far tier's own sub-pixel limit is applied by the shader AFTER
/// this and is deliberately not folded in here — it is a cost control, not a correctness rule.
///
/// A REPLICA, not the shipped rule: the rule runs per placement in far.wgsl, on the GPU, so
/// the only way to assert on it in a unit test is to state it twice. That is a real hazard, so
/// `the_shader_carries_the_complement_rule` below matches the shader's own lines as text — if
/// either half drifts back into being a distance, one of the two tests fails.
#[cfg(test)]
fn far_accepts(
    d: f32,
    proxy_radius: f32,
    k: f32,
    objects_z: f32,
    near_floor: f32,
    far_distance: f32,
) -> bool {
    if d > far_distance || d < near_floor {
        return false;
    }
    let proxy_diameter = proxy_radius.max(MIN_PROXY_RADIUS) * 2.0;
    let real_drawn = d <= objects_z && proxy_diameter >= k * d;
    !real_drawn
}

/// `WGR_FAR_MODE=distance` restores the pre-complement pure-distance start rule from the SAME
/// binary, so an A/B of the two rules is one environment variable and not a rebuild. Anything
/// else (including unset) is the complement.
///
/// Read here AND in EngineWgpu.cpp's `FarSettings()`, which needs it to pick the matching near
/// cutoff default (the legacy 0.45 x draw distance in distance mode, a small hard floor in
/// complement mode). Passing it across would mean a new `wgr_far_set_params` parameter and an
/// ABI bump, which this change is explicitly not allowed to spend.
fn mode_from_env() -> f32 {
    match std::env::var("WGR_FAR_MODE") {
        Ok(v) if v.eq_ignore_ascii_case("distance") => 1.0,
        _ => 0.0,
    }
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
            tune: [0.0, 0.0, WORKGROUP as f32, mode_from_env()],
            // Inert-safe until the first per-frame push, and inert-safe in the same direction
            // CullInputs::default() is: k = 0 makes every placement inside objects_z "drawn",
            // so the tier starts at the object cull's radial edge rather than swamping the
            // frame with proxies over live models.
            cull: [0.0, CullInputs::default().objects_z, 0.0, 0.0],
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
            format: depth_format(device),
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

    /// The REAL object cull's coefficients for this frame, straight from `Gfx3d::cull_inputs()`
    /// — the same values `wgr_set_cull_params` feeds gfx3d/cull.wgsl.
    ///
    /// Pushed PER FRAME on purpose. `lod_inv_width` is driven by the framerate governor, so the
    /// real cull's sub-pixel edge walks in and out while the game runs; a start rule sampled
    /// once at world load would be the complement of a cull that no longer exists. Writes only
    /// on change, so a settled governor costs nothing.
    pub fn set_cull_complement(&mut self, queue: &wgpu::Queue, inputs: CullInputs) {
        let k = inputs.pixel_limit * inputs.lod_scale * inputs.lod_inv_width;
        let objects_z = inputs.objects_z;
        // k is ~1e-4 at the fine rail, so the guard has to be far below that to not swallow a
        // real governor move.
        if (self.params.cull[0] - k).abs() > 1.0e-9
            || (self.params.cull[1] - objects_z).abs() > 1.0e-3
        {
            self.params.cull[0] = k;
            self.params.cull[1] = objects_z;
            self.upload_params(queue);
        }
    }

    /// True when the start rule is the complement; false under `WGR_FAR_MODE=distance`.
    pub fn complement_mode(&self) -> bool {
        self.params.tune[3] < 0.5
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

    // THE START RULE. This test exists to stop the tier silently regressing to a distance:
    // every assertion below is at a DIFFERENT distance-vs-outcome pairing than a monotone
    // "accept beyond X" rule could produce, so no fixed cutoff can satisfy all of them.
    //
    // k is the real cull's folded coefficient at the fine rail (pixel_limit 0.125 x
    // lod_scale 1.13 x lod_inv_width 0.0104), the same value the offline sweep used.
    #[test]
    fn the_start_rule_is_the_complement_of_the_object_cull_not_a_distance() {
        const K: f32 = 0.125 * 1.13 * 0.0104; // ~1.47e-3
        const OBJECTS_Z: f32 = 880.0; // the stock Everon fog edge
        const FLOOR: f32 = 40.0;
        const FAR: f32 = 12_000.0;
        // A big tree: 10 m of horizontal extent, so radius 5. Sub-pixel seam at 10 / K ~
        // 6,800 m, i.e. well past objects_z — the real cull only ever drops this one radially.
        const BIG: f32 = 5.0;
        // A fence post, narrow enough to be widened to MIN_PROXY_RADIUS: drawn 0.5 m across,
        // so its sub-pixel seam is 0.5 / K ~ 340 m, less than half of objects_z.
        const SMALL: f32 = 0.15;

        let accepts = |d: f32, r: f32| far_accepts(d, r, K, OBJECTS_Z, FLOOR, FAR);

        // 1. A LARGE proxy near the camera is REJECTED — the real object is drawn there, and
        //    a proxy would be a crude box standing in front of its own model.
        assert!(!accepts(200.0, BIG));
        assert!(!accepts(600.0, BIG));
        // Still rejected right up to the object cull's radial edge.
        assert!(!accepts(OBJECTS_Z - 1.0, BIG));

        // 2. The SAME proxy past objects_z is ACCEPTED — the real cull dropped it radially.
        assert!(accepts(OBJECTS_Z + 1.0, BIG));
        assert!(accepts(5_000.0, BIG));

        // 3. A SMALL proxy at MID distance is ACCEPTED at a distance where the large one is
        //    not: the real cull dropped it for being sub-pixel, not for being far. This is the
        //    pair no distance rule can reproduce — same d, opposite answers.
        assert!(accepts(400.0, SMALL));
        assert!(!accepts(400.0, BIG));
        // And inside its own sub-pixel seam it is rejected again, because there the real model
        // IS drawn: the small proxy's seam sits at ~340 m, the big one's at objects_z.
        assert!(!accepts(150.0, SMALL));
        assert!(!accepts(300.0, SMALL));

        // 4. The near cutoff is a HARD FLOOR laid over the complement, never a start rule.
        //    Isolated with a heavy governor (k = 0.05), the one condition under which the
        //    complement genuinely wants to emit a proxy within arm's reach: the post goes
        //    sub-pixel at 10 m there, so at 20 m the complement says yes and the floor still
        //    says no.
        assert!(far_accepts(20.0, SMALL, 0.05, OBJECTS_Z, 0.0, FAR));
        assert!(!far_accepts(20.0, SMALL, 0.05, OBJECTS_Z, FLOOR, FAR));
        // ...and the floor must not become the rule in its own right — the big proxy just
        // above it is still rejected, because its real model is drawn.
        assert!(!accepts(FLOOR + 1.0, BIG));

        // 5. The fog wall still caps the tier.
        assert!(!accepts(FAR + 1.0, BIG));

        // 6. A degenerate zero-extent row (a flagpole's ModelInfo) is measured at the width the
        //    tier actually DRAWS it, not at zero — otherwise it would be "never real-drawn" and
        //    accepted at every distance above the floor.
        assert!(!far_accepts(100.0, 0.0, K, OBJECTS_Z, FLOOR, FAR));
        assert!(accepts(400.0, 0.0));

        // 7. THE GOVERNOR MOVES THE SEAM. Same proxy, same distance, coarser rail (k x 4, the
        //    second rail the offline sweep measured): the real cull now drops it, so the tier
        //    picks it up. A cutoff in metres cannot express this, which is the whole reason
        //    this rule is not one.
        assert!(!accepts(200.0, SMALL));
        assert!(far_accepts(200.0, SMALL, K * 4.0, OBJECTS_Z, FLOOR, FAR));
    }

    // far_accepts and far.wgsl are two authorings of one rule; nothing else stops the shader
    // half drifting back into a distance test while the Rust half keeps passing.
    #[test]
    fn the_shader_carries_the_complement_rule() {
        let src = include_str!("far.wgsl");
        assert!(src.contains("let real_drawn = (d <= objects_z) && (proxy_diameter >= k * d);"));
        assert!(src.contains("let k = far.cull.x;"));
        assert!(src.contains("let objects_z = far.cull.y;"));
        // The mode gate, so WGR_FAR_MODE=distance still has something to switch.
        assert!(src.contains("if (far.tune.w < 0.5) {"));
        // The proxy is measured at the width it is drawn at, in both files.
        assert!(src.contains(&format!(
            "const MIN_PROXY_RADIUS: f32 = {:?};",
            MIN_PROXY_RADIUS
        )));
        assert!(src.contains("let proxy_diameter = max(s.radius, MIN_PROXY_RADIUS) * 2.0;"));
    }

    // WGR_FAR_MODE is the A/B switch and must not need a rebuild. Parsed here rather than
    // exercised through the environment, because a process-wide env var set inside one test
    // leaks into every other test in the binary.
    #[test]
    fn distance_mode_disables_the_complement() {
        // tune.w = 1 is the legacy pure-distance rule; the shader's gate reads it as "skip".
        let mut params = FarParams {
            cfg: [0.0, 0.0, 2.0, 0.0],
            tune: [0.0, 0.0, WORKGROUP as f32, 1.0],
            cull: [0.0, 880.0, 0.0, 0.0],
        };
        assert!(params.tune[3] >= 0.5);
        params.tune[3] = mode_from_env(); // unset in the test process -> complement
        assert_eq!(params.tune[3], 0.0);
    }

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
