// GPU cull + LOD + indirect-arg compaction (docs/gpu-culling-and-depth-plan.md Stage 3).
// See cull.wgsl for the compute; this owns the GPU-side data layouts (filled from C++ via
// the retained-scene FFI in Stage 3b), the compute pipeline, and the CPU-side frustum-plane
// extraction that feeds the cull params each frame.
//
// Stage 3a scope: the data model + shader + pipeline + the (testable) frustum math, with no
// live data source yet. Stage 3b allocates/uploads the buffers from the C++ world walk,
// dispatches the compute, and submits its args via multi_draw_indexed_indirect.

use bytemuck::Zeroable;
use glam::{Mat4, Vec3, Vec4};
use std::sync::mpsc;

// --- GPU buffer layouts (the CPU side of the structs in cull.wgsl) ---

// Per-frame cull parameters (one uniform). 224 bytes, 16-aligned. The occlusion tail
// (view_proj/viewport/hiz_mips/occlusion) is read only by main_occlude; the plain `main`
// entry ignores it, so main/shadow views leave it zeroed.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct CullParamsGpu {
    // World-space planes (nx, ny, nz, d), oriented so dot(n, p) + d >= 0 == inside.
    pub frustum: [[f32; 4]; 6],
    pub cam_pos: [f32; 4],
    pub objects_z2: f32,
    pub lod_scale: f32,
    pub lod_inv_width: f32,
    pub pixel_limit: f32,
    pub instance_count: u32,
    pub variant_capacity: u32,
    pub variant_count: u32,
    // Debug flags (bit 0 = skip the frustum test, WGR_CULL_NO_FRUSTUM); read by cull.wgsl.
    pub debug_flags: u32,
    // Occlusion (main_occlude only). Camera-relative proj*view projecting a camera-relative
    // point to clip, plus the Hi-Z size/mip count and the enable flag.
    pub view_proj: [[f32; 4]; 4],
    pub viewport: [f32; 2],
    pub hiz_mips: u32,
    pub occlusion: u32,
}

// One retained instance. `world` is the ABSOLUTE model->world transform (the GPU-driven VS
// subtracts cam_pos in-shader); `center.xyz` is the transformed bounding-sphere center and
// `center.w` the uniform scale — both used by the cull compute (which never reads `world`).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct InstanceGpu {
    pub world: [f32; 16],
    pub center: [f32; 4],
    pub model: u32,
    pub flags: u32, // reserved
    // Inflated frustum-cull radius (f32 bits) for terrain-conform instances, whose displaced
    // geometry escapes the flat model sphere; 0 = rigid (use model.bounding_sphere * scale).
    pub cull_radius: u32,
    pub _pad: u32,
    // Terrain-conform plane (mirrors WgrDraw3D::conform*), evaluated per vertex by the
    // GPU-driven VS so one shared undeformed mesh conforms to the ground. conform2.z = mode:
    // 0 = rigid (all-zero), 1 = ForestPlain bilinear land-grid plane (conform0/1/2 fields),
    // 2 = individual ClipLand vegetation (per-vertex SurfaceY; conform0.x = bcSurfaceY).
    pub conform0: [f32; 4],
    pub conform1: [f32; 4],
    pub conform2: [f32; 4],
}

// One model = a range of drawable LOD levels + its bounding radius at scale 1.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct ModelGpu {
    pub lod_base: u32,
    pub lod_count: u32,
    pub bounding_sphere: f32,
    pub _pad: u32,
}

// One LOD level = a resolution threshold + a range of sections.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct LodGpu {
    pub resolution: f32,
    pub section_base: u32,
    pub section_count: u32,
    pub is_decal: u32,
}

// One drawable section = a slice of the shared geometry pool + its pipeline variant.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, bytemuck::Pod, bytemuck::Zeroable)]
pub struct SectionGpu {
    pub first_index: u32,
    pub index_count: u32,
    pub base_vertex: u32,
    pub variant: u32,
}

// One per-draw record, written by the compute parallel to out_args: the instance + the
// global section this sub-draw renders. A sub-draw's first_instance indexes this, so the
// VS/FS recovers both the instance transform and the per-section material.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct RecordGpu {
    pub instance: u32,
    pub section: u32,
}

// Per-section shading (static, register-once): raw material the GPU-driven FS folds with
// the frame sun, plus the bindless texture slot / sampler / cutout threshold. Indexed by
// the global section id in RecordGpu. Parallel to the section table.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct SectionMaterialGpu {
    pub emissive: [f32; 4],
    pub ambient: [f32; 4],
    pub diffuse: [f32; 4],
    pub specular: [f32; 4], // w = specular power
    pub texture_slot: u32,
    pub sampler: u32,
    pub alpha_ref: f32,
    // Bindless slot of the section's specular map (SMDI or a family equivalent), 0 = none.
    // Occupies what was the struct's tail padding, so the layout is unchanged in size.
    pub specular_slot: u32,
    // Bindless slot of the section's tangent-space normal map (RVMAT Stage1), 0 = none.
    // This one does grow the struct: WGSL rounds the stride up to the vec4 alignment, so the
    // padding is explicit on both sides rather than left to each language's own rules.
    pub normal_slot: u32,
    // Multi's blend: the mask (0 = not layered) and the three further layer colours.
    pub mask_slot: u32,
    pub layer_slot: [u32; 3],
    // Per-section flags, carried across the ABI in WgrModelSection::flags. Bit 0 =
    // reflective (the material named an EnvironmentMap). Occupies the first of the three
    // explicit pad words below, so the layout is unchanged in size.
    pub flags: u32,
    // WGSL aligns an array<vec4<f32>> to 16 bytes; Rust aligns [[f32; 4]; 4] to 4. Without
    // this the two layouts disagree by 12 bytes and every field from layer_uv on reads the
    // wrong memory -- which renders as white buildings, not as an error.
    pub _pad_layer: [u32; 2],
    // Per layer, layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
    pub layer_uv: [[f32; 4]; 4],
}

// A permissive plane that never culls anything: dot(0, p) + BIG >= -radius always holds.
// Used for the far plane (radial distance is culled by objects_z2 instead of a flat far
// plane, which would clip peripheral content early — see the frustum-cull hazard note).
const NO_OP_PLANE: [f32; 4] = [0.0, 0.0, 0.0, 1.0e30];

/// Extract 6 **camera-relative** cull planes for the compute, oriented so `dot(n, p) + d >= 0`
/// means inside — where `p` is a CAMERA-RELATIVE position (`world - cam_pos`). `view_proj` must
/// be `proj * view` with the view's translation ZEROED (the engine's convention: geometry is
/// camera-relative), so Gribb–Hartmann yields planes in camera-relative space and the compute
/// tests instance centers as `center - cam_pos`.
///
/// Every plane is derived from `proj * view` itself (Gribb–Hartmann), so ALL are exactly
/// consistent with the actual projection for ANY orientation:
///   left/right/top/bottom = row3 ± row0/row1 — the classic side planes;
///   near = row3 = the `clip.w >= 0` half-space. `clip.w` is precisely "in front of the camera"
///     for the true projection (only points with `clip.w > 0` render), so this is the correct,
///     handedness-agnostic near plane — NOT a hand-picked forward axis (the engine's row-major
///     D3D LH layout makes a naive `view.z_axis`/`-Z` guess point the wrong way, which culls a
///     direction-dependent half-space that pops geometry in/out as the camera rotates). The
///     camera-relative view puts `clip.w = z_view = 0` at the origin, so near passes through the
///     camera (a through-apex plane: in/out by direction, not distance — exactly a view cone).
/// The far slot is a no-op: radial distance culling (`objects_z2`) replaces a flat far plane.
pub fn frustum_planes(view_proj: Mat4) -> [[f32; 4]; 6] {
    let r0 = view_proj.row(0);
    let r1 = view_proj.row(1);
    let r3 = view_proj.row(3);

    // Normalize by the length of the plane's xyz so the `-radius` slack in the shader is in
    // world units.
    let norm = |p: Vec4| -> [f32; 4] {
        let n = Vec3::new(p.x, p.y, p.z);
        let len = n.length();
        let inv = if len > 0.0 { 1.0 / len } else { 0.0 };
        [p.x * inv, p.y * inv, p.z * inv, p.w * inv]
    };

    let left = norm(r3 + r0);
    let right = norm(r3 - r0);
    let bottom = norm(r3 + r1);
    let top = norm(r3 - r1);
    let near = norm(r3);

    [left, right, bottom, top, near, NO_OP_PLANE]
}

// --- Compute pipeline ---

// Pipeline-variant buckets: which opaque variants the cull's compaction groups sections
// into (one indirect batch + one multi_draw per variant). The opaque set is small; solid
// vs alpha-cutout is the split that matters. Grown as needed.
pub const CULL_VARIANT_COUNT: u32 = 2;

// Marks a removed static instance slot (a free-list hole); the compute skips it.
pub const INVALID_MODEL: u32 = u32::MAX;

// Default per-variant arg capacity. out_args holds CULL_VARIANT_COUNT * this DrawArgs;
// overflow past it is dropped (compute skips the append), never wrapped. Sized generously:
// past the cap the DROPPED SET depends on the atomic-append order, which varies frame to
// frame, so an overflowing frame flickers random objects (observed with the frustum test
// disabled: the whole retained set appends and 64K overflowed). 256K * 20 B * 2 variants
// = 10 MB — cheap insurance against that failure mode ever appearing in normal play.
const DEFAULT_VARIANT_CAPACITY: u32 = 1 << 18; // 256K sections/variant

// u32 words per DrawIndexedIndirectArgs (20 B / 4).
const ARG_WORDS: u64 = super::INDIRECT_ARG_SIZE / 4;

// Counter buffer = one append cursor per pipeline variant (0..CULL_VARIANT_COUNT), read as the
// count buffer by multi_draw_indexed_indirect_count, PLUS one trailing word (index
// CULL_VARIANT_COUNT): the global out_records bump allocator (the "records cursor") the
// instancing-collapse EMIT pass carves per-section runs from (docs §3.6). The count reads only
// touch words 0..CULL_VARIANT_COUNT, so the extra word is invisible to them.
// --- PERF-005 object accounting -------------------------------------------------------------
// The accounting lives in the TAIL of each view's existing `counters` buffer, not in a buffer of
// its own. The cull bind layout already holds 8 storage buffers and the baseline
// `max_storage_buffers_per_shader_stage` is 8 — a 9th binding is rejected outright by the
// headless test device, so a dedicated stats buffer would have traded compatibility with every
// 8-storage-buffer GPU for one saved indexing offset. Word 3 is spare padding so STAT_BASE_WORD
// lands on a round number; keep in lockstep with STAT_BASE in cull.wgsl.
const STAT_BASE_WORD: u64 = 4;
pub const STATS_WORDS: usize = 16;
const COUNTER_WORDS: u64 = STAT_BASE_WORD + STATS_WORDS as u64;
// Which readback slot each cull view's counters are gathered into. Sky-visibility views
// deliberately SHARE the last slot: there are 5 of them (sky_vis::DIRECTION_COUNT) and their
// numbers are not what this instrumentation is about, so they land in a scratch slot whose sum
// across directions is meaningless and is never reported.
pub const STATS_VIEW_MAIN: usize = 0;
pub const STATS_VIEW_COLOR: usize = 1;
pub const STATS_VIEW_REFLECTION: usize = 2;
pub const STATS_VIEW_SHADOW0: usize = 3;
pub const STATS_MAX_CASCADES: usize = 4;
pub const STATS_VIEW_SKY_SCRATCH: usize = STATS_VIEW_SHADOW0 + STATS_MAX_CASCADES;
pub const STATS_VIEW_COUNT: usize = STATS_VIEW_SKY_SCRATCH + 1;
// Word map — must stay in lockstep with the atomicAdd indices in cull.wgsl.
pub const STAT_INSTANCES: usize = 0;
pub const STAT_RECORDS: usize = 1;
pub const STAT_TRIS: usize = 2;
pub const STAT_DRAWS: usize = 3;
pub const STAT_DRAWS_V0: usize = 4;
pub const STAT_TRIS_V0: usize = 6;
pub const STAT_LOD_HIST: usize = 8;
pub const STAT_LOD_BUCKETS: usize = 8;
/// Bytes gathered per view into a readback slot (the accounting tail of its counters buffer).
const STATS_SLOT_BYTES: u64 = STATS_WORDS as u64 * 4;

/// One view's decoded PERF-005 counters.
#[derive(Clone, Copy, Default, Debug)]
pub struct ViewStats {
    pub instances: u32,
    pub records: u32,
    pub tris: u32,
    pub draws: u32,
    pub draws_solid: u32,
    pub draws_alpha: u32,
    pub tris_solid: u32,
    pub tris_alpha: u32,
    pub lod_hist: [u32; STAT_LOD_BUCKETS],
}

/// The whole frame's object accounting, one entry per cull view.
#[derive(Clone, Copy, Default)]
pub struct CullStats {
    pub views: [ViewStats; STATS_VIEW_COUNT],
    /// False until a readback has landed; the C++ side reports "pending" rather than zeros,
    /// because a zeroed count and an un-harvested count look identical and mean opposite things.
    pub valid: bool,
}

// One extra cull VIEW for a shadow cascade (docs/gpu-culling-and-depth-plan.md §6, multi-view).
// The retained tables + instance buffer are SHARED with the main view (owned by CullState);
// only the per-view cull params (this cascade's light frustum) and the compute outputs (args,
// records, counters) + the bind group over them are per-cascade. Runs the SAME cull.wgsl
// dispatch, so a cascade is just "the same cull with a different frustum + no distance cull".
struct ShadowCullView {
    params_buf: wgpu::Buffer,
    counter_buf: wgpu::Buffer,
    out_args: Option<wgpu::Buffer>,
    out_records: Option<wgpu::Buffer>,
    out_args_cap: u64,
    // Per-section instancing-collapse scratch (§3.6), sized sections.len(); reallocated when the
    // section table grows (like out_args). Bound at binding 9 of this view's cull bind group.
    sec_count: Option<wgpu::Buffer>,
    sec_count_cap: u64,
    bind: Option<wgpu::BindGroup>,
    params: CullParamsGpu,
    // PERF-005: which 256 B slice of the shared stats buffer this view accounts into
    // (STATS_VIEW_*). Assigned by the owner right after construction; defaults to the sky
    // scratch slot so an unassigned view can never silently pollute the main-view numbers.
    stats_view: usize,
}

impl ShadowCullView {
    fn new(device: &wgpu::Device) -> Self {
        let params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_shadow_params"),
            size: std::mem::size_of::<CullParamsGpu>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let counter_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_shadow_counters"),
            size: COUNTER_WORDS * 4,
            // COPY_SRC: PERF-005 gathers this buffer's accounting tail into the readback ring.
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::INDIRECT
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        Self {
            params_buf,
            counter_buf,
            out_args: None,
            out_records: None,
            out_args_cap: 0,
            sec_count: None,
            sec_count_cap: 0,
            bind: None,
            params: CullParamsGpu::zeroed(),
            stats_view: STATS_VIEW_SKY_SCRATCH,
        }
    }
}

pub struct CullState {
    // Instancing-collapse three-pass pipelines (§3.6), all over `layout`: COUNT (1/instance) ->
    // EMIT (1/section) -> SCATTER (1/instance). Shared by the main + every shadow view.
    count_pipeline: wgpu::ComputePipeline,
    emit_pipeline: wgpu::ComputePipeline,
    scatter_pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,

    // Retained tables — CPU mirrors + GPU buffers. Registered at load, rarely changed, so
    // re-uploaded wholesale when `tables_dirty`.
    models: Vec<ModelGpu>,
    lods: Vec<LodGpu>,
    sections: Vec<SectionGpu>,
    // Per-section shading, parallel to `sections` (same global index). Draw-side only —
    // not a compute input; bound in the GPU-driven draw pass (3b-2b).
    section_materials: Vec<SectionMaterialGpu>,
    // Per-tree crown centres (MODEL space, .xyz; .w unused) for forest spherical normals
    // (foliage-translucency-plan.md §9 Approach A). A global append-only table; a forest vertex's
    // `conform` word indexes it. Draw-side only (group-1 binding 3 in vs_gpu), not a compute input.
    crown_centres: Vec<[f32; 4]>,
    tables_dirty: bool,

    // Unified instance buffer: static slots [0, static.len()) with a free-list, then the
    // dynamic region re-copied every frame. A removed static slot has model = INVALID_MODEL.
    static_instances: Vec<InstanceGpu>,
    free_slots: Vec<u32>,
    static_dirty: Option<(u32, u32)>, // inclusive min..=max changed static slot this frame
    dynamic: Vec<InstanceGpu>,
    // Static region length uploaded last frame, so a grown static region re-copies the
    // dynamic tail to its new offset.
    uploaded_static_len: u32,

    model_buf: super::StorageArray,
    lod_buf: super::StorageArray,
    section_buf: super::StorageArray,
    section_mat_buf: super::StorageArray,
    crown_centre_buf: super::StorageArray,
    instance_buf: super::StorageArray,
    // Per-variant append cursors written by the compute. Fixed size (CULL_VARIANT_COUNT
    // words) and carries INDIRECT usage so it can double as the count buffer for
    // multi_draw_indexed_indirect_count (the 3b-4 tail trim) on adapters that support it.
    counter_buf: wgpu::Buffer,
    out_args: Option<wgpu::Buffer>,
    // Per-draw records — a flat global array carved into contiguous per-section runs by the
    // instancing-collapse EMIT/SCATTER passes (§3.6), sized to hold every surviving pair.
    out_records: Option<wgpu::Buffer>,
    out_args_cap: u64,
    // Main-view per-section instancing-collapse scratch (§3.6), sized sections.len().
    sec_count: Option<wgpu::Buffer>,
    sec_count_cap: u64,
    params_buf: wgpu::Buffer,

    variant_capacity: u32,
    params: CullParamsGpu,
    // Debug flags written into CullParamsGpu.debug_flags each frame (cull.wgsl reads them). bit 0
    // = WGR_CULL_NO_FRUSTUM (skip the frustum test — discriminates "culled" from "not drawn").
    debug_flags: u32,
    bind: Option<wgpu::BindGroup>,

    // Per-cascade shadow views (§6 multi-view). Length = active cascade count this frame
    // (set by set_shadow_view_count); each shares the tables/instances above.
    shadow_views: Vec<ShadowCullView>,
    // The planar mirror has its own frustum/outputs. It shares retained scene data only;
    // never the main camera's cull records or indirect arguments.
    reflection_view: Option<ShadowCullView>,
    // Interior sky-visibility views (docs/interior-sky-visibility-plan.md §4): ONE ortho frustum
    // per sampled sky direction (zenith + tilted), each culled independently of everything above.
    // Same multi-view shape as `shadow_views`; empty when the feature is off.
    sky_views: Vec<ShadowCullView>,

    // Color-pass occlusion view (§5 Hi-Z). Same retained tables/instances, its own params
    // (occlusion tail) + args/records/counters, run by the `main_occlude` pipeline against the
    // Hi-Z. Its args feed the color draw; the main view's args stay the prepass/occluder set.
    occlude_count_pipeline: wgpu::ComputePipeline,
    occlude_emit_pipeline: wgpu::ComputePipeline,
    occlude_scatter_pipeline: wgpu::ComputePipeline,
    occlude_layout: wgpu::BindGroupLayout,
    color_params_buf: wgpu::Buffer,
    color_counter_buf: wgpu::Buffer,
    color_out_args: Option<wgpu::Buffer>,
    color_out_records: Option<wgpu::Buffer>,
    color_out_args_cap: u64,
    // Color/occlusion-view per-section instancing-collapse scratch (§3.6).
    color_sec_count: Option<wgpu::Buffer>,
    color_sec_count_cap: u64,
    color_params: CullParamsGpu,
    color_bind: Option<wgpu::BindGroup>,
    // Full-chain Hi-Z view the color bind samples (cloned from Gfx3d's HiZ when it (re)allocs).
    // The color view is only prepared when this is Some.
    hiz_view: Option<wgpu::TextureView>,

    // PERF-005 accounting: a non-blocking readback ring (modelled on gpu_timers.rs) that gathers
    // every view's counters tail into one staging buffer. A blocking map here would serialise the
    // frame and change the very timings this instrumentation exists to measure.
    stats_readback: Vec<(wgpu::Buffer, StatsSlot)>,
    stats_latest: CullStats,
}

// Readback slot state, mirroring gpu_timers.rs's ring so the two behave identically under
// device loss and ring saturation (a saturated ring drops the frame's sample, never blocks).
enum StatsSlot {
    Idle,
    Pending,
    InFlight(mpsc::Receiver<Result<(), wgpu::BufferAsyncError>>),
}

const STATS_RING_SLOTS: usize = 3;

impl CullState {
    // Byte size of one CullParamsGpu (the uniform is fixed-size).
    const PARAMS_SIZE: u64 = std::mem::size_of::<CullParamsGpu>() as u64;

    pub fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_cull"),
            source: wgpu::ShaderSource::Wgsl(include_str!("cull.wgsl").into()),
        });

        // Bindings 0..=6, matching cull.wgsl. 0 uniform, 1..=4 read-only storage,
        // 5..=6 read-write storage (the args + per-variant counters the compute writes).
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
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_cull_layout"),
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
                storage(3, true),
                storage(4, true),
                storage(5, false),
                storage(6, false),
                storage(7, false),
                // Binding 9 = the per-section instancing-collapse scratch (binding 8 is the
                // Hi-Z, present only in the occlude layout; layouts may be sparse).
                storage(9, false),
            ],
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_cull_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        // Instancing collapse (§3.6): the cull is three dispatches (COUNT -> EMIT -> SCATTER)
        // sharing one bind group. `emit_args` is layout-agnostic (no Hi-Z) so both the main and
        // occlude pipeline layouts reuse the same entry.
        let make_pl = |label: &str, pl_layout: &wgpu::PipelineLayout, entry: &str| {
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some(label),
                layout: Some(pl_layout),
                module: &module,
                entry_point: Some(entry),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        let count_pipeline = make_pl("wgr_cull_count", &pipeline_layout, "count");
        let emit_pipeline = make_pl("wgr_cull_emit", &pipeline_layout, "emit_args");
        let scatter_pipeline = make_pl("wgr_cull_scatter", &pipeline_layout, "scatter");

        // Color-occlusion layout = the main 0..=7 bindings + binding 8 = the Hi-Z pyramid
        // (non-filterable float, sampled by textureLoad). Only main_occlude references it.
        let occlude_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_cull_occlude_layout"),
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
                storage(3, true),
                storage(4, true),
                storage(5, false),
                storage(6, false),
                storage(7, false),
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                storage(9, false), // instancing-collapse scratch (see main layout)
            ],
        });
        let occlude_pl_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_cull_occlude_pipeline_layout"),
            bind_group_layouts: &[Some(&occlude_layout)],
            immediate_size: 0,
        });
        let occlude_count_pipeline = make_pl(
            "wgr_cull_occlude_count",
            &occlude_pl_layout,
            "count_occlude",
        );
        let occlude_emit_pipeline =
            make_pl("wgr_cull_occlude_emit", &occlude_pl_layout, "emit_args");
        let occlude_scatter_pipeline = make_pl(
            "wgr_cull_occlude_scatter",
            &occlude_pl_layout,
            "scatter_occlude",
        );

        let params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_params"),
            size: Self::PARAMS_SIZE,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let counter_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_counters"),
            size: COUNTER_WORDS * 4,
            // COPY_SRC: PERF-005 gathers this buffer's accounting tail into the readback ring.
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::INDIRECT
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let color_params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_color_params"),
            size: Self::PARAMS_SIZE,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let color_counter_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_color_counters"),
            size: COUNTER_WORDS * 4,
            // COPY_SRC: PERF-005 gathers this buffer's accounting tail into the readback ring.
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::INDIRECT
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        // PERF-005 readback ring: 3 slots (same depth as the GPU timers' — enough to ride out
        // triple-buffered presentation without ever blocking), each holding every view's
        // gathered counters tail.
        let stats_bytes = STATS_SLOT_BYTES * STATS_VIEW_COUNT as u64;
        let stats_readback = (0..STATS_RING_SLOTS)
            .map(|i| {
                (
                    device.create_buffer(&wgpu::BufferDescriptor {
                        label: Some(&format!("wgr_cull_stats_readback_{i}")),
                        size: stats_bytes,
                        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                        mapped_at_creation: false,
                    }),
                    StatsSlot::Idle,
                )
            })
            .collect();

        Self {
            count_pipeline,
            emit_pipeline,
            scatter_pipeline,
            layout,
            models: Vec::new(),
            lods: Vec::new(),
            sections: Vec::new(),
            section_materials: Vec::new(),
            crown_centres: Vec::new(),
            tables_dirty: false,
            static_instances: Vec::new(),
            free_slots: Vec::new(),
            static_dirty: None,
            dynamic: Vec::new(),
            uploaded_static_len: 0,
            model_buf: super::StorageArray::new("wgr_cull_models"),
            lod_buf: super::StorageArray::new("wgr_cull_lods"),
            section_buf: super::StorageArray::new("wgr_cull_sections"),
            section_mat_buf: super::StorageArray::new("wgr_cull_section_mats"),
            crown_centre_buf: super::StorageArray::new("wgr_cull_crown_centres"),
            instance_buf: super::StorageArray::new("wgr_cull_instances"),
            counter_buf,
            out_args: None,
            out_records: None,
            out_args_cap: 0,
            sec_count: None,
            sec_count_cap: 0,
            params_buf,
            variant_capacity: DEFAULT_VARIANT_CAPACITY,
            params: CullParamsGpu::zeroed(),
            debug_flags: if std::env::var("WGR_CULL_NO_FRUSTUM").is_ok() {
                1
            } else {
                0
            },
            bind: None,
            shadow_views: Vec::new(),
            reflection_view: None,
            sky_views: Vec::new(),
            occlude_count_pipeline,
            occlude_emit_pipeline,
            occlude_scatter_pipeline,
            occlude_layout,
            color_params_buf,
            color_counter_buf,
            color_out_args: None,
            color_out_records: None,
            color_out_args_cap: 0,
            color_sec_count: None,
            color_sec_count_cap: 0,
            color_params: CullParamsGpu::zeroed(),
            color_bind: None,
            hiz_view: None,
            stats_readback,
            stats_latest: CullStats::default(),
        }
    }

    /// PERF-005: the latest harvested per-view object accounting (see CullStats).
    pub fn stats(&self) -> CullStats {
        self.stats_latest
    }

    /// Zero every view's counters (including the PERF-005 accounting tail). MUST be recorded
    /// into the frame encoder before the frame's first cull dispatch. Whole-buffer and
    /// every-view, so a view that does NOT dispatch this frame reads back honest zeros instead
    /// of repeating its last live frame — the same stale-value trap the GPU timers had to solve,
    /// and the reason `record_collapse`'s own clear is not enough on its own.
    pub fn begin_frame_stats(&self, encoder: &mut wgpu::CommandEncoder) {
        for (_, buf) in self.stats_sources() {
            encoder.clear_buffer(buf, 0, None);
        }
    }

    /// (readback slot, counters buffer) for every cull view that owns one. The single place the
    /// view -> slot mapping is enumerated, so clear / resolve / harvest cannot disagree.
    fn stats_sources(&self) -> Vec<(usize, &wgpu::Buffer)> {
        let mut v: Vec<(usize, &wgpu::Buffer)> = vec![
            (STATS_VIEW_MAIN, &self.counter_buf),
            (STATS_VIEW_COLOR, &self.color_counter_buf),
        ];
        if let Some(r) = self.reflection_view.as_ref() {
            v.push((r.stats_view, &r.counter_buf));
        }
        for sv in &self.shadow_views {
            v.push((sv.stats_view, &sv.counter_buf));
        }
        // Sky views all carry STATS_VIEW_SKY_SCRATCH and would overwrite each other in the
        // gather; they are deliberately excluded rather than reported as a meaningless sum.
        v
    }

    /// Gather every view's accounting tail into a free readback slot. Called from the frame
    /// encoder AFTER every cull dispatch and before submit. Skips silently when the ring is
    /// saturated — dropping a sample is correct; stalling to collect one would distort the
    /// frame it is measuring.
    pub fn resolve_stats(&mut self, encoder: &mut wgpu::CommandEncoder) {
        let sources: Vec<(usize, wgpu::Buffer)> = self
            .stats_sources()
            .into_iter()
            .map(|(i, b)| (i, b.clone()))
            .collect();
        let Some((dst, state)) = self
            .stats_readback
            .iter_mut()
            .find(|(_, s)| matches!(s, StatsSlot::Idle))
        else {
            return;
        };
        for (slot, src) in sources {
            if slot >= STATS_VIEW_COUNT {
                continue;
            }
            encoder.copy_buffer_to_buffer(
                &src,
                STAT_BASE_WORD * 4,
                dst,
                slot as u64 * STATS_SLOT_BYTES,
                STATS_SLOT_BYTES,
            );
        }
        *state = StatsSlot::Pending;
    }

    /// Kick map_async on freshly copied slots and drain completed ones (non-blocking).
    /// Called once per frame after queue.submit, alongside GpuTimers::harvest.
    pub fn harvest_stats(&mut self, device: &wgpu::Device) {
        for (buf, state) in &mut self.stats_readback {
            if matches!(state, StatsSlot::Pending) {
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| {
                    let _ = tx.send(r);
                });
                *state = StatsSlot::InFlight(rx);
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in &mut self.stats_readback {
            let StatsSlot::InFlight(rx) = state else {
                continue;
            };
            match rx.try_recv() {
                Ok(Ok(())) => {
                    {
                        let data = buf.slice(..).get_mapped_range();
                        let word = |i: usize| {
                            u32::from_le_bytes(data[i * 4..i * 4 + 4].try_into().unwrap())
                        };
                        for v in 0..STATS_VIEW_COUNT {
                            let b = v * STATS_WORDS;
                            let mut st = ViewStats {
                                instances: word(b + STAT_INSTANCES),
                                records: word(b + STAT_RECORDS),
                                tris: word(b + STAT_TRIS),
                                draws: word(b + STAT_DRAWS),
                                draws_solid: word(b + STAT_DRAWS_V0),
                                draws_alpha: word(b + STAT_DRAWS_V0 + 1),
                                tris_solid: word(b + STAT_TRIS_V0),
                                tris_alpha: word(b + STAT_TRIS_V0 + 1),
                                lod_hist: [0; STAT_LOD_BUCKETS],
                            };
                            for (k, h) in st.lod_hist.iter_mut().enumerate() {
                                *h = word(b + STAT_LOD_HIST + k);
                            }
                            self.stats_latest.views[v] = st;
                        }
                        self.stats_latest.valid = true;
                    }
                    buf.unmap();
                    *state = StatsSlot::Idle;
                }
                Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                    // Mapping failed (device loss etc.) — recycle the slot, keep last values.
                    *state = StatsSlot::Idle;
                }
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
    }

    // Register a model: its sections (already resolved to pool offsets by the caller) and
    // its drawable LOD levels (whose `section_base` is RELATIVE to `sections`). Appends to
    // the global tables and returns the model id (index into models[]). Called at load.
    pub fn register_model(
        &mut self,
        bounding_sphere: f32,
        lods: &[LodGpu],
        sections: &[SectionGpu],
        materials: &[SectionMaterialGpu],
    ) -> u32 {
        debug_assert_eq!(sections.len(), materials.len(), "one material per section");
        let section_offset = self.sections.len() as u32;
        let lod_offset = self.lods.len() as u32;
        self.sections.extend_from_slice(sections);
        self.section_materials.extend_from_slice(materials);
        for lod in lods {
            let mut l = *lod;
            l.section_base += section_offset;
            self.lods.push(l);
        }
        let model_id = self.models.len() as u32;
        self.models.push(ModelGpu {
            lod_base: lod_offset,
            lod_count: lods.len() as u32,
            bounding_sphere,
            _pad: 0,
        });
        self.tables_dirty = true;
        model_id
    }

    // Append a batch of per-tree crown centres (model space) to the global table; return the base
    // index of this batch (foliage-translucency-plan.md §9 Approach A). Forest vertices carry
    // `base + local_component_index` in their `conform` word, which vs_gpu reads to get a per-tree
    // radial-normal centre. Register-once, so re-uploaded wholesale under `tables_dirty`.
    pub fn register_crown_centres(&mut self, centres: &[[f32; 4]]) -> u32 {
        let base = self.crown_centres.len() as u32;
        self.crown_centres.extend_from_slice(centres);
        self.tables_dirty = true;
        base
    }

    // Replace the whole sections table with a freshly-resolved one (same length + order as
    // registration, only base_vertex/first_index may have moved). Marks the tables dirty for
    // re-upload only when something actually changed, so a quiet frame costs nothing. The LOD
    // section_base offsets index this table and are unaffected (order is preserved).
    pub fn set_sections(&mut self, sections: &[SectionGpu]) {
        if self.sections.as_slice() != sections {
            self.sections.clear();
            self.sections.extend_from_slice(sections);
            self.tables_dirty = true;
        }
    }

    // Add a static instance; returns its stable slot (reused from the free-list if any).
    pub fn instance_add(&mut self, inst: InstanceGpu) -> u32 {
        let slot = if let Some(s) = self.free_slots.pop() {
            self.static_instances[s as usize] = inst;
            s
        } else {
            self.static_instances.push(inst);
            (self.static_instances.len() - 1) as u32
        };
        self.mark_static_dirty(slot);
        slot
    }

    pub fn instance_update(&mut self, slot: u32, inst: InstanceGpu) {
        if let Some(e) = self.static_instances.get_mut(slot as usize) {
            *e = inst;
            self.mark_static_dirty(slot);
        }
    }

    // Remove a static instance: mark its slot a free-list hole (model = INVALID_MODEL, so
    // the compute skips it) and recycle the slot.
    pub fn instance_remove(&mut self, slot: u32) {
        if let Some(e) = self.static_instances.get_mut(slot as usize) {
            e.model = INVALID_MODEL;
            self.free_slots.push(slot);
            self.mark_static_dirty(slot);
        }
    }

    // Replace the whole dynamic set (re-copied every frame — the churny set the CPU already
    // walks for simulation).
    pub fn set_dynamic(&mut self, instances: &[InstanceGpu]) {
        self.dynamic.clear();
        self.dynamic.extend_from_slice(instances);
    }

    // Set this frame's cull params (frustum/cam/objectsZ/lod knobs). instance_count and the
    // variant fields are filled by prepare().
    pub fn set_params(&mut self, mut params: CullParamsGpu) {
        params.debug_flags = self.debug_flags;
        self.params = params;
    }

    // Set the color-pass occlusion view's params (frustum/LOD + the occlusion tail). Only used
    // when occlusion is active; instance_count/variant fields are filled by prepare().
    pub fn set_color_params(&mut self, mut params: CullParamsGpu) {
        params.debug_flags = self.debug_flags;
        self.color_params = params;
    }

    // Point the color view's occlusion bind at the current Hi-Z pyramid (cloned full-chain
    // view), or clear it (None) when occlusion is off / the pyramid is gone. Forces a color-bind
    // rebuild on the next prepare().
    pub fn set_hiz(&mut self, view: Option<wgpu::TextureView>) {
        self.hiz_view = view;
        self.color_bind = None;
    }

    // Grow/shrink the shadow-cascade view set to `n` (0 = no GPU shadow culling this frame).
    // Cheap: each view lazily allocates its output buffers in prepare(); shrinking drops the
    // tail views (their buffers free with them).
    pub fn set_shadow_view_count(&mut self, device: &wgpu::Device, n: usize) {
        while self.shadow_views.len() < n {
            let mut v = ShadowCullView::new(device);
            // PERF-005: cascade c accounts into slot SHADOW0 + c. Cascades past STATS_MAX_CASCADES
            // (4, matching gfx3d MAX_CASCADES) would alias, so they fall into the sky scratch slot
            // rather than being added to cascade 0's totals.
            let c = self.shadow_views.len();
            v.stats_view = if c < STATS_MAX_CASCADES {
                STATS_VIEW_SHADOW0 + c
            } else {
                STATS_VIEW_SKY_SCRATCH
            };
            self.shadow_views.push(v);
        }
        self.shadow_views.truncate(n);
    }

    pub fn shadow_view_count(&self) -> usize {
        self.shadow_views.len()
    }

    // Set cascade `i`'s cull params (frustum from its light-VP; typically objects_z2 disabled).
    // No-op if `i` is out of range (view count not yet set).
    pub fn set_shadow_params(&mut self, i: usize, mut params: CullParamsGpu) {
        if let Some(v) = self.shadow_views.get_mut(i) {
            params.debug_flags = self.debug_flags;
            v.params = params;
        }
    }

    pub fn set_reflection_params(&mut self, device: &wgpu::Device, mut params: CullParamsGpu) {
        params.debug_flags = self.debug_flags;
        let view = self.reflection_view.get_or_insert_with(|| {
            let mut v = ShadowCullView::new(device);
            v.stats_view = STATS_VIEW_REFLECTION;
            v
        });
        view.params = params;
    }

    pub fn clear_reflection_view(&mut self) {
        self.reflection_view = None;
    }

    // Grow/shrink the interior sky-visibility view set to `n` (0 = feature off, no dispatches).
    // Cheap: each view lazily allocates its outputs in prepare(), and dropping the tail frees
    // them with it.
    pub fn set_sky_view_count(&mut self, device: &wgpu::Device, n: usize) {
        while self.sky_views.len() < n {
            self.sky_views.push(ShadowCullView::new(device));
        }
        self.sky_views.truncate(n);
    }

    pub fn sky_view_count(&self) -> usize {
        self.sky_views.len()
    }

    // Set sky direction `i`'s cull params (frustum from its ortho VP). No-op out of range.
    pub fn set_sky_params(&mut self, i: usize, mut params: CullParamsGpu) {
        if let Some(v) = self.sky_views.get_mut(i) {
            params.debug_flags = self.debug_flags;
            v.params = params;
        }
    }

    fn mark_static_dirty(&mut self, slot: u32) {
        self.static_dirty = Some(match self.static_dirty {
            Some((lo, hi)) => (lo.min(slot), hi.max(slot)),
            None => (slot, slot),
        });
    }

    // Upload dirty tables + instances + params and (re)build the bind group. Call once per
    // frame before dispatch. Returns whether any GPU buffer was (re)allocated, so the
    // GPU-driven draw's group-1 bind group (which borrows instances/records/materials) is
    // rebuilt only when one actually moved.
    pub fn prepare(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) -> bool {
        let mut grew = false;

        if self.tables_dirty {
            grew |= upload_slice(device, queue, &mut self.model_buf, &self.models);
            grew |= upload_slice(device, queue, &mut self.lod_buf, &self.lods);
            grew |= upload_slice(device, queue, &mut self.section_buf, &self.sections);
            grew |= upload_slice(
                device,
                queue,
                &mut self.section_mat_buf,
                &self.section_materials,
            );
            grew |= upload_slice(
                device,
                queue,
                &mut self.crown_centre_buf,
                &self.crown_centres,
            );
            self.tables_dirty = false;
        }

        // Instance buffer = static region then the dynamic tail. Ensure capacity for both.
        let static_len = self.static_instances.len() as u32;
        let total = static_len as u64 + self.dynamic.len() as u64;
        let inst_bytes = total.max(1) * std::mem::size_of::<InstanceGpu>() as u64;
        grew |= self.instance_buf.ensure(device, inst_bytes);
        let stride = std::mem::size_of::<InstanceGpu>() as u64;
        if let Some(buf) = self.instance_buf.buf.as_ref() {
            // A grown static region shifts the dynamic tail, so re-upload the whole static
            // region (not just the dirty range) whenever static_len changed.
            let full_static = self.uploaded_static_len != static_len;
            if full_static && static_len > 0 {
                queue.write_buffer(buf, 0, bytemuck::cast_slice(&self.static_instances));
            } else if let Some((lo, hi)) = self.static_dirty {
                let range = &self.static_instances[lo as usize..=hi as usize];
                queue.write_buffer(buf, lo as u64 * stride, bytemuck::cast_slice(range));
            }
            // Dynamic tail every frame at the (possibly new) static offset.
            if !self.dynamic.is_empty() {
                queue.write_buffer(
                    buf,
                    static_len as u64 * stride,
                    bytemuck::cast_slice(&self.dynamic),
                );
            }
        }
        self.static_dirty = None;
        self.uploaded_static_len = static_len;

        // Shared-buffer growth (tables + instances) forces a rebuild of EVERY view's bind
        // group (all views reference these read-only buffers); a per-view output realloc only
        // rebuilds that view's bind.
        let shared_grew = grew;

        // Main-view outputs (out_args = variant_count * capacity; flat records; per-section
        // scratch sized to the section table; counters fixed).
        let sections_len = self.sections.len() as u64;
        let args_grew = ensure_view_outputs(
            device,
            self.variant_capacity,
            sections_len,
            &mut self.out_args,
            &mut self.out_records,
            &mut self.out_args_cap,
            &mut self.sec_count,
            &mut self.sec_count_cap,
        );
        grew |= args_grew;

        // Finalize the per-frame variant + count fields shared by every view, then upload each
        // view's params (frustum/cam differ; instance_count/variant_* are identical). Captured
        // as locals so the closure doesn't borrow self (rebuild_bind below needs &mut self).
        let variant_capacity = self.variant_capacity;
        let finalize = |p: &mut CullParamsGpu| {
            p.instance_count = total as u32;
            p.variant_capacity = variant_capacity;
            p.variant_count = CULL_VARIANT_COUNT;
        };
        finalize(&mut self.params);
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(&self.params));

        if shared_grew || args_grew || self.bind.is_none() {
            self.rebuild_bind(device);
        }

        // Shadow-cascade views: same outputs + params machinery, this cascade's frustum.
        for i in 0..self.shadow_views.len() {
            let view_grew = {
                let v = &mut self.shadow_views[i];
                let g = ensure_view_outputs(
                    device,
                    self.variant_capacity,
                    sections_len,
                    &mut v.out_args,
                    &mut v.out_records,
                    &mut v.out_args_cap,
                    &mut v.sec_count,
                    &mut v.sec_count_cap,
                );
                finalize(&mut v.params);
                queue.write_buffer(&v.params_buf, 0, bytemuck::bytes_of(&v.params));
                g
            };
            grew |= view_grew;
            if shared_grew || view_grew || self.shadow_views[i].bind.is_none() {
                let bind = {
                    let v = &self.shadow_views[i];
                    match (
                        v.out_args.as_ref(),
                        v.out_records.as_ref(),
                        v.sec_count.as_ref(),
                    ) {
                        (Some(a), Some(r), Some(sc)) => self.build_view_bind(
                            device,
                            &v.params_buf,
                            a,
                            &v.counter_buf,
                            r,
                            sc,
                            v.stats_view,
                        ),
                        _ => None,
                    }
                };
                self.shadow_views[i].bind = bind;
            }
        }

        // Standalone extra views (planar reflection, interior sky visibility): each owns its
        // frustum + outputs and shares only the retained tables. Taken out and put back so the
        // shared-table bind build (&self) can run while the view is owned locally.
        if let Some(v) = self.reflection_view.take() {
            let (v, g) =
                self.prepare_standalone_view(device, queue, sections_len, shared_grew, total, v);
            grew |= g;
            self.reflection_view = Some(v);
        }
        for i in 0..self.sky_views.len() {
            let v = std::mem::replace(&mut self.sky_views[i], ShadowCullView::new(device));
            let (v, g) =
                self.prepare_standalone_view(device, queue, sections_len, shared_grew, total, v);
            grew |= g;
            self.sky_views[i] = v;
        }

        // Color-occlusion view (§5): only prepared when a Hi-Z view is set (occlusion active).
        // Its args feed the color draw; the main-view args stay the prepass/occluder set.
        if self.hiz_view.is_some() {
            let color_grew = ensure_view_outputs(
                device,
                self.variant_capacity,
                sections_len,
                &mut self.color_out_args,
                &mut self.color_out_records,
                &mut self.color_out_args_cap,
                &mut self.color_sec_count,
                &mut self.color_sec_count_cap,
            );
            grew |= color_grew;
            finalize(&mut self.color_params);
            queue.write_buffer(
                &self.color_params_buf,
                0,
                bytemuck::bytes_of(&self.color_params),
            );
            if shared_grew || color_grew || self.color_bind.is_none() {
                self.rebuild_color_bind(device);
            }
        } else {
            self.color_bind = None;
        }
        grew
    }

    // Prepare ONE standalone extra view (reflection / sky): allocate its outputs, upload its
    // params, and rebuild its bind when a buffer moved. Taken BY VALUE and handed back because
    // the bind is built from `&self` (the shared retained tables) while the view is mutated —
    // owning it locally is what keeps the two borrows apart. Returns (view, grew).
    fn prepare_standalone_view(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        sections_len: u64,
        shared_grew: bool,
        instance_count: u64,
        mut view: ShadowCullView,
    ) -> (ShadowCullView, bool) {
        let view_grew = ensure_view_outputs(
            device,
            self.variant_capacity,
            sections_len,
            &mut view.out_args,
            &mut view.out_records,
            &mut view.out_args_cap,
            &mut view.sec_count,
            &mut view.sec_count_cap,
        );
        view.params.instance_count = instance_count as u32;
        view.params.variant_capacity = self.variant_capacity;
        view.params.variant_count = CULL_VARIANT_COUNT;
        queue.write_buffer(&view.params_buf, 0, bytemuck::bytes_of(&view.params));
        if shared_grew || view_grew || view.bind.is_none() {
            view.bind = match (
                view.out_args.as_ref(),
                view.out_records.as_ref(),
                view.sec_count.as_ref(),
            ) {
                (Some(a), Some(r), Some(sc)) => self.build_view_bind(
                    device,
                    &view.params_buf,
                    a,
                    &view.counter_buf,
                    r,
                    sc,
                    view.stats_view,
                ),
                _ => None,
            };
        }
        (view, view_grew)
    }

    fn rebuild_bind(&mut self, device: &wgpu::Device) {
        let (Some(args), Some(records), Some(sec)) = (
            self.out_args.as_ref(),
            self.out_records.as_ref(),
            self.sec_count.as_ref(),
        ) else {
            self.bind = None;
            return;
        };
        self.bind = self.build_view_bind(
            device,
            &self.params_buf,
            args,
            &self.counter_buf,
            records,
            sec,
            STATS_VIEW_MAIN,
        );
    }

    // Build the color-occlusion bind (occlude_layout): the color view's own params/args/
    // counters/records + the SHARED retained tables + the Hi-Z pyramid at binding 8.
    fn rebuild_color_bind(&mut self, device: &wgpu::Device) {
        let (
            Some(args),
            Some(records),
            Some(sec),
            Some(hiz),
            Some(inst),
            Some(models),
            Some(lods),
            Some(sections),
        ) = (
            self.color_out_args.as_ref(),
            self.color_out_records.as_ref(),
            self.color_sec_count.as_ref(),
            self.hiz_view.as_ref(),
            self.instance_buf.buf.as_ref(),
            self.model_buf.buf.as_ref(),
            self.lod_buf.buf.as_ref(),
            self.section_buf.buf.as_ref(),
        )
        else {
            self.color_bind = None;
            return;
        };
        self.color_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_cull_color_bind"),
            layout: &self.occlude_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: self.color_params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: inst.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: models.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: lods.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: sections.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: args.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: self.color_counter_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: records.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 8,
                    resource: wgpu::BindingResource::TextureView(hiz),
                },
                wgpu::BindGroupEntry {
                    binding: 9,
                    resource: sec.as_entire_binding(),
                },
            ],
        }));
    }

    // Record one view's instancing-collapse cull (§3.6): clear the scratch, then COUNT (1 thread/
    // instance) -> EMIT (1 thread/section) -> SCATTER (1 thread/instance), each its own compute
    // pass so wgpu barriers the storage writes between them. `count_pl`/`scatter_pl` differ per
    // view flavour (plain vs Hi-Z occlusion); `emit_pl` is layout-agnostic. All three share the
    // one `bind`. Assumes instance_count > 0.
    #[allow(clippy::too_many_arguments)]
    fn record_collapse(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        label: &str,
        bind: &wgpu::BindGroup,
        args: &wgpu::Buffer,
        counters: &wgpu::Buffer,
        sec_count: &wgpu::Buffer,
        count_pl: &wgpu::ComputePipeline,
        emit_pl: &wgpu::ComputePipeline,
        scatter_pl: &wgpu::ComputePipeline,
        stats_view: usize,
    ) {
        // Counters (incl. the trailing records cursor) and the per-section scratch reset to 0;
        // out_args zeroed so unfilled arg slots stay instance_count = 0 no-op draws. Records need
        // no clear — only slots a live arg points at (filled by SCATTER) are ever read.
        encoder.clear_buffer(counters, 0, None);
        encoder.clear_buffer(args, 0, None);
        encoder.clear_buffer(sec_count, 0, None);
        // PERF-005 note: this view's stats slice is NOT cleared here. begin_frame_stats() zeroes
        // the WHOLE buffer once per frame instead, so a view that does not dispatch at all this
        // frame reports honest zeros rather than repeating its last dispatch's numbers — the
        // same stale-region trap the GPU timers already had to solve. `stats_view` is kept in the
        // signature so each dispatch site still states which slice it writes.
        let _ = stats_view;
        let inst_groups = self.params.instance_count.div_ceil(64);
        // EMIT is one thread per GLOBAL section; a scene with no registered sections skips it.
        let sec_groups = (self.sections.len() as u32).div_ceil(64);
        // Each pass is its OWN begin_compute_pass: wgpu auto-inserts the storage barrier BETWEEN
        // compute passes (COUNT's sec_count writes -> EMIT reads; EMIT's writes -> SCATTER reads),
        // but NOT between dispatches within one pass (they'd race). Same as the terrain/sky computes.
        let mut pass = |pl: &wgpu::ComputePipeline, groups: u32, name: &str| {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some(name),
                timestamp_writes: None,
            });
            cp.set_pipeline(pl);
            cp.set_bind_group(0, bind, &[]);
            cp.dispatch_workgroups(groups, 1, 1);
        };
        pass(count_pl, inst_groups, label);
        if sec_groups > 0 {
            pass(emit_pl, sec_groups, label);
        }
        pass(scatter_pl, inst_groups, label);
    }

    // Record the main-view cull. No-op until prepare() has run with instances present.
    pub fn dispatch(&self, encoder: &mut wgpu::CommandEncoder) {
        let (Some(bind), Some(args), Some(sec)) = (
            self.bind.as_ref(),
            self.out_args.as_ref(),
            self.sec_count.as_ref(),
        ) else {
            return;
        };
        if self.params.instance_count == 0 {
            return;
        }
        self.record_collapse(
            encoder,
            "wgr_cull",
            bind,
            args,
            &self.counter_buf,
            sec,
            &self.count_pipeline,
            &self.emit_pipeline,
            &self.scatter_pipeline,
            STATS_VIEW_MAIN,
        );
    }

    // Record cascade `i`'s cull into the same encoder. Shares the retained instance + table
    // buffers with the main dispatch; writes this cascade's own args/records/counters/scratch.
    // wgpu barriers the compute writes -> the depth pass's indirect reads. No-op until prepare()
    // has run with instances present.
    pub fn dispatch_shadow(&self, encoder: &mut wgpu::CommandEncoder, i: usize) {
        if self.params.instance_count == 0 {
            return;
        }
        let Some(view) = self.shadow_views.get(i) else {
            return;
        };
        let (Some(bind), Some(args), Some(sec)) = (
            view.bind.as_ref(),
            view.out_args.as_ref(),
            view.sec_count.as_ref(),
        ) else {
            return;
        };
        self.record_collapse(
            encoder,
            "wgr_cull_shadow",
            bind,
            args,
            &view.counter_buf,
            sec,
            &self.count_pipeline,
            &self.emit_pipeline,
            &self.scatter_pipeline,
            view.stats_view,
        );
    }

    pub fn dispatch_reflection(&self, encoder: &mut wgpu::CommandEncoder) {
        if self.params.instance_count == 0 {
            return;
        }
        let Some(view) = self.reflection_view.as_ref() else {
            return;
        };
        let (Some(bind), Some(args), Some(sec)) = (
            view.bind.as_ref(),
            view.out_args.as_ref(),
            view.sec_count.as_ref(),
        ) else {
            return;
        };
        self.record_collapse(
            encoder,
            "wgr_cull_reflection",
            bind,
            args,
            &view.counter_buf,
            sec,
            &self.count_pipeline,
            &self.emit_pipeline,
            &self.scatter_pipeline,
            view.stats_view,
        );
    }

    // Record the interior sky-visibility cull (top-down ortho box). Recorded before the sky
    // depth pass so wgpu barriers the compute writes -> that pass's indirect reads. No-op until
    // set_sky_params + prepare() have run with instances present.
    pub fn dispatch_sky(&self, encoder: &mut wgpu::CommandEncoder, i: usize) {
        if self.params.instance_count == 0 {
            return;
        }
        let Some(view) = self.sky_views.get(i) else {
            return;
        };
        let (Some(bind), Some(args), Some(sec)) = (
            view.bind.as_ref(),
            view.out_args.as_ref(),
            view.sec_count.as_ref(),
        ) else {
            return;
        };
        self.record_collapse(
            encoder,
            "wgr_cull_sky",
            bind,
            args,
            &view.counter_buf,
            sec,
            &self.count_pipeline,
            &self.emit_pipeline,
            &self.scatter_pipeline,
            view.stats_view,
        );
    }

    // Record the color-occlusion cull (Hi-Z): frustum + distance + LOD + occlusion, collapsed.
    // MUST be recorded AFTER the Hi-Z build (which reads this frame's prepass depth) and before
    // the color pass reads color_out_args. No-op unless prepare() set up the color bind (Hi-Z
    // present / occlusion active) and there are instances.
    pub fn dispatch_color(&self, encoder: &mut wgpu::CommandEncoder) {
        if self.params.instance_count == 0 {
            return;
        }
        let (Some(bind), Some(args), Some(sec)) = (
            self.color_bind.as_ref(),
            self.color_out_args.as_ref(),
            self.color_sec_count.as_ref(),
        ) else {
            return;
        };
        self.record_collapse(
            encoder,
            "wgr_cull_color",
            bind,
            args,
            &self.color_counter_buf,
            sec,
            &self.occlude_count_pipeline,
            &self.occlude_emit_pipeline,
            &self.occlude_scatter_pipeline,
            STATS_VIEW_COLOR,
        );
    }

    // Whether the color-occlusion view is live this frame (Hi-Z bound + params uploaded). The
    // color draw reads color_out_args/records only when this holds; else it reuses the main view.
    pub fn color_active(&self) -> bool {
        self.color_bind.is_some()
    }

    pub fn color_out_args(&self) -> Option<&wgpu::Buffer> {
        self.color_out_args.as_ref()
    }

    pub fn color_out_records(&self) -> Option<&wgpu::Buffer> {
        self.color_out_records.as_ref()
    }

    pub fn color_counter_buf(&self) -> &wgpu::Buffer {
        &self.color_counter_buf
    }

    // Cascade `i`'s compute outputs, consumed by the GPU-driven shadow depth draw
    // (draw_gpu_driven_shadow). None until prepare() allocated them.
    pub fn shadow_out_args(&self, i: usize) -> Option<&wgpu::Buffer> {
        self.shadow_views.get(i).and_then(|v| v.out_args.as_ref())
    }

    pub fn shadow_out_records(&self, i: usize) -> Option<&wgpu::Buffer> {
        self.shadow_views
            .get(i)
            .and_then(|v| v.out_records.as_ref())
    }

    pub fn shadow_counter_buf(&self, i: usize) -> Option<&wgpu::Buffer> {
        self.shadow_views.get(i).map(|v| &v.counter_buf)
    }

    pub fn reflection_out_args(&self) -> Option<&wgpu::Buffer> {
        self.reflection_view
            .as_ref()
            .and_then(|v| v.out_args.as_ref())
    }

    pub fn reflection_out_records(&self) -> Option<&wgpu::Buffer> {
        self.reflection_view
            .as_ref()
            .and_then(|v| v.out_records.as_ref())
    }

    pub fn reflection_counter_buf(&self) -> Option<&wgpu::Buffer> {
        self.reflection_view.as_ref().map(|v| &v.counter_buf)
    }

    pub fn sky_out_args(&self, i: usize) -> Option<&wgpu::Buffer> {
        self.sky_views.get(i).and_then(|v| v.out_args.as_ref())
    }

    pub fn sky_out_records(&self, i: usize) -> Option<&wgpu::Buffer> {
        self.sky_views.get(i).and_then(|v| v.out_records.as_ref())
    }

    pub fn sky_counter_buf(&self, i: usize) -> Option<&wgpu::Buffer> {
        self.sky_views.get(i).map(|v| &v.counter_buf)
    }

    // Build a cull bind group for one VIEW: its own params/args/counters/records, but the
    // SHARED retained tables (instances/models/lods/sections). Factored out of rebuild_bind so
    // the main view and every shadow-cascade view bind identically over the same read-only data.
    #[allow(clippy::too_many_arguments)]
    #[allow(clippy::too_many_arguments)]
    fn build_view_bind(
        &self,
        device: &wgpu::Device,
        params_buf: &wgpu::Buffer,
        out_args: &wgpu::Buffer,
        counter_buf: &wgpu::Buffer,
        out_records: &wgpu::Buffer,
        sec_count: &wgpu::Buffer,
        // PERF-005 readback slot this view gathers into. Not a binding any more (the accounting
        // rides `counter_buf`'s tail) — kept in the signature so every call site still states
        // which slot it owns, which is what stops two views silently sharing one.
        _stats_view: usize,
    ) -> Option<wgpu::BindGroup> {
        let (Some(inst), Some(models), Some(lods), Some(sections)) = (
            self.instance_buf.buf.as_ref(),
            self.model_buf.buf.as_ref(),
            self.lod_buf.buf.as_ref(),
            self.section_buf.buf.as_ref(),
        ) else {
            return None;
        };
        Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_cull_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: params_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: inst.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: models.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: lods.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: sections.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: out_args.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: counter_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: out_records.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 9,
                    resource: sec_count.as_entire_binding(),
                },
            ],
        }))
    }

    // The GPU-produced indirect args + per-variant counters (consumed by the Stage-3b
    // multi_draw submission).
    pub fn out_args(&self) -> Option<&wgpu::Buffer> {
        self.out_args.as_ref()
    }

    // Buffers the GPU-driven draw pass (3b-2b) reads: the retained instances (VS transform),
    // the per-draw records + per-section materials (VS/FS), and the indirect args.
    pub fn instance_buf(&self) -> Option<&wgpu::Buffer> {
        self.instance_buf.buf.as_ref()
    }

    pub fn out_records(&self) -> Option<&wgpu::Buffer> {
        self.out_records.as_ref()
    }

    // The per-variant counters, doubling as the count buffer for the
    // multi_draw_indexed_indirect_count tail trim (3b-4). Word `v` holds the number of args
    // the compute appended to variant `v`'s partition (clamped to variant_capacity at draw).
    pub fn counter_buf(&self) -> &wgpu::Buffer {
        &self.counter_buf
    }

    pub fn section_material_buf(&self) -> Option<&wgpu::Buffer> {
        self.section_mat_buf.buf.as_ref()
    }

    // Per-tree crown-centre table (model space), read by vs_gpu at group-1 binding 3 for forest
    // spherical normals. None until prepare() first uploaded the tables.
    pub fn crown_centre_buf(&self) -> Option<&wgpu::Buffer> {
        self.crown_centre_buf.buf.as_ref()
    }

    // The per-model table (lod range + bounding_sphere). Read by the cull-sphere debug pass to
    // recover each instance's radius (models[inst.model].bounding_sphere * scale).
    pub fn model_buf(&self) -> Option<&wgpu::Buffer> {
        self.model_buf.buf.as_ref()
    }

    // Total retained instances (static slots incl. free-list holes + dynamic), = the value the
    // compute dispatches over. The cull-sphere debug pass draws this many instances (holes are
    // skipped in-shader by the INVALID_MODEL guard).
    pub fn instance_count(&self) -> u32 {
        self.params.instance_count
    }

    // Runtime toggle for the ImGui Culling tab: OR/clear bit 0 of the debug flags (skip the
    // frustum test). Takes effect on the next set_params (called every frame in prepare_cull).
    pub fn set_no_frustum(&mut self, no_frustum: bool) {
        if no_frustum {
            self.debug_flags |= 1;
        } else {
            self.debug_flags &= !1;
        }
    }

    pub fn variant_capacity(&self) -> u32 {
        self.variant_capacity
    }
}

// Engine-derived per-frame cull + LOD inputs — the REAL values behind Scene::LevelFromDistance2
// (SceneDraw.cpp:572), pushed from C++ via wgr_set_cull_params each frame:
//   objects_z     = ENGINE_CONFIG.objectsZ         — draw distance (distance cull; squared here)
//   lod_scale     = Camera::Left()                 — projection tan(halfFovX); the LOD/sub-pixel
//                                                    `scale` (≈ 0.75 at default FOV, NOT 1)
//   lod_inv_width = Scene::GetLodInvWidth()         — ≈ lodCoef*2/screenWidth (~1e-3, NOT 1);
//                                                    the whole LOD-distance scale rides on this
//   pixel_limit   = 0.125                          — legacy sub-pixel invisibility threshold
#[derive(Clone, Copy)]
pub struct CullInputs {
    pub objects_z: f32,
    pub lod_scale: f32,
    pub lod_inv_width: f32,
    pub pixel_limit: f32,
}

impl Default for CullInputs {
    fn default() -> Self {
        // Inert-safe until C++ pushes the real values. lod_inv_width = 0 makes detail2 = 0 ->
        // always the finest LOD and no sub-pixel cull, so a missing push degrades to "draw
        // everything at full detail within objects_z" — never the ~1e6-too-large resol2 that a
        // value of 1.0 produced (which jumped every model to its coarsest LOD within metres).
        Self {
            objects_z: 900.0,
            lod_scale: 1.0,
            lod_inv_width: 0.0,
            pixel_limit: 0.0,
        }
    }
}

impl CullInputs {
    /// Adjust the main-view LOD metric for a render target with fewer pixels and/or a wider
    /// field of view. `linear_reduction` is the ratio of main-view pixels-per-radian to the
    /// secondary view's pixels-per-radian. A half-resolution view is 2; a half-resolution
    /// view whose projection is widened by 1.35 is 2.7.
    ///
    /// The cull shader's detail metric is `distance * lod_inv_width * lod_scale`. Scaling
    /// `lod_inv_width` therefore selects the same model LOD and sub-pixel cutoff that the
    /// reduced view would have received if the engine had computed its LOD inputs directly.
    pub fn for_reduced_view(self, linear_reduction: f32) -> Self {
        let reduction = if linear_reduction.is_finite() {
            linear_reduction.clamp(1.0, 64.0)
        } else {
            1.0
        };
        Self {
            lod_inv_width: self.lod_inv_width * reduction,
            ..self
        }
    }
}

// Build this frame's cull params from the main camera + the engine's LOD inputs. `view` must be
// the engine's camera-relative view (translation zeroed, as PushSceneCamera hands over); all six
// frustum planes are then extracted directly from `proj * view` (see frustum_planes).
pub fn params_from_camera(
    view: Mat4,
    proj: Mat4,
    cam_pos: Vec3,
    inputs: CullInputs,
) -> CullParamsGpu {
    let mut p = CullParamsGpu::zeroed();
    p.frustum = frustum_planes(proj * view);
    p.cam_pos = [cam_pos.x, cam_pos.y, cam_pos.z, 0.0];
    p.objects_z2 = inputs.objects_z * inputs.objects_z;
    p.lod_scale = inputs.lod_scale;
    p.lod_inv_width = inputs.lod_inv_width;
    p.pixel_limit = inputs.pixel_limit;
    p
}

// Build the COLOR-pass cull params: the same frustum/distance/LOD as the main view (so the
// occluded set is a subset of the prepass set) plus the occlusion tail — the camera-relative
// proj*view (projects a camera-relative bound to clip), the Hi-Z size/mip count, and the enable
// flag. `viewport` is the Hi-Z mip0 size in texels (= render target size).
#[allow(clippy::too_many_arguments)]
pub fn params_from_camera_occlude(
    view: Mat4,
    proj: Mat4,
    cam_pos: Vec3,
    inputs: CullInputs,
    viewport: [f32; 2],
    hiz_mips: u32,
    occlusion: bool,
) -> CullParamsGpu {
    let mut p = params_from_camera(view, proj, cam_pos, inputs);
    p.view_proj = (proj * view).to_cols_array_2d();
    p.viewport = viewport;
    p.hiz_mips = hiz_mips;
    p.occlusion = u32::from(occlusion);
    p
}

// Build one shadow CASCADE's cull params (§6 multi-view). The frustum is extracted from the
// cascade's CAMERA-RELATIVE light view-projection `light_vp` (Gribb–Hartmann, exactly as the
// main view does from proj*view) — an orthographic light matrix yields the 4 working side
// planes + degenerate near/far no-ops, which is correct: casters outside the cascade's depth
// range are clipped by NDC z in the depth pass, so only the lateral side planes need to cull.
// `cam_pos` MUST be the origin `light_vp` is relative to (the shadow pass camera). The LOD
// knobs come from the main view (so a caster's shadow uses the same LOD its colour draw does),
// but the radial DISTANCE cull is disabled (objects_z2 = +inf): the cascade side planes bound
// the set laterally and the shared sub-pixel cull drops tiny far casters, so the main camera's
// draw distance must not clip casters the far cascades still cover.
pub fn params_from_shadow_cascade(
    light_vp: Mat4,
    cam_pos: Vec3,
    inputs: CullInputs,
) -> CullParamsGpu {
    let mut p = CullParamsGpu::zeroed();
    p.frustum = frustum_planes(light_vp);
    p.cam_pos = [cam_pos.x, cam_pos.y, cam_pos.z, 0.0];
    p.objects_z2 = 1.0e30; // distance cull disabled for shadow views (finite, fast-math-safe)
    p.lod_scale = inputs.lod_scale;
    p.lod_inv_width = inputs.lod_inv_width;
    p.pixel_limit = inputs.pixel_limit;
    p
}

// The GPU-driven draw's group-1 layout: instances + records + section materials, all
// read-only storage (docs/gpu-culling-and-depth-plan.md Stage 3b). Groups 0/2/3 (camera,
// bindless textures, sampler array) are shared with the per-draw path.
pub fn gpu_group1_layout(device: &wgpu::Device) -> wgpu::BindGroupLayout {
    let storage = |binding: u32| wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
        ty: wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage { read_only: true },
            has_dynamic_offset: false,
            min_binding_size: None,
        },
        count: None,
    };
    device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("wgr_gpu_driven_group1"),
        // 0 instances, 1 records, 2 section materials, 3 per-tree crown centres (forest spherical
        // normals; unused by the shadow VS but present so the shared layout stays compatible),
        // 4 per-model sky-visibility volume metadata + 5 the volume data itself (LIT-020 Stage 2;
        // both are single-element dummies until a bake runs, so the layout never varies),
        // 6/7 the sky reflection env map + its sampler (DZ-003), read only by sections whose
        // material named an EnvironmentMap. Bound from frame one against a 1x1 dummy, so the
        // layout never varies here either. The shadow VS declares neither and does not have to:
        // a layout entry a shader does not use is legal.
        entries: &[
            storage(0),
            storage(1),
            storage(2),
            storage(3),
            storage(4),
            storage(5),
            wgpu::BindGroupLayoutEntry {
                binding: 6,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Float { filterable: true },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            },
            wgpu::BindGroupLayoutEntry {
                binding: 7,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                count: None,
            },
        ],
    })
}

/// `prepass_match` override for the GPU-driven prepass FS: 1 (default) = the prepass applies
/// fs_gpu's full discard set, so it lays depth only where the colour pass will shade. 0 restores
/// the pre-fix behaviour for an A/B.
///
/// Resolved ONCE (build_gpu_pipeline runs twice — main + mirrored reflection) so both pipelines
/// are specialised identically and the log carries one line, like the other WGR_* gates.
fn prepass_match_from_env() -> f64 {
    static MATCH: std::sync::OnceLock<f64> = std::sync::OnceLock::new();
    *MATCH.get_or_init(|| {
        let on = std::env::var("WGR_PREPASS_MATCH")
            .map(|v| v != "0")
            .unwrap_or(true);
        eprintln!(
            "[wgr] gpu-driven prepass discard parity: {} from WGR_PREPASS_MATCH",
            if on {
                "on (prepass drops what fs_gpu drops)"
            } else {
                "off (legacy: raw-uv cutout test, no clip plane)"
            }
        );
        if on {
            1.0
        } else {
            0.0
        }
    })
}

// Build the GPU-driven draw pipelines (gpu_driven.wgsl) — the opaque COLOUR pipeline
// (vs_gpu / fs_gpu) and the depth+normal PREPASS pipeline (vs_gpu / fs_gpu_prepass), sharing
// one shader module + layout. Both: reversed-Z GreaterEqual depth test + WRITE, back-face
// cull. The colour one carries the `linear` HDR override (from the colour format) and one
// pipeline serves solid + alpha-cutout (dynamic per-section alpha_ref discard). The prepass
// one drops shading and writes only the view-space octahedral normal into NORMAL_FORMAT (the
// same G-buffer the per-draw fs_prepass fills), so the GPU-driven set participates in the
// depth+normal prepass (SSAO normals, early-Z) instead of colour-pass only.
#[allow(clippy::too_many_arguments)]
pub fn build_gpu_pipeline(
    device: &wgpu::Device,
    composer: &mut naga_oil::compose::Composer,
    camera_layout: &wgpu::BindGroupLayout,
    group1_layout: &wgpu::BindGroupLayout,
    bindless_layout: &wgpu::BindGroupLayout,
    sampler_layout: &wgpu::BindGroupLayout,
    conform_layout: &wgpu::BindGroupLayout,
    surface_format: wgpu::TextureFormat,
    sample_count: u32,
    foliage_a2c: bool,
    front_face: wgpu::FrontFace,
) -> (wgpu::RenderPipeline, wgpu::RenderPipeline) {
    let module = crate::shaders::make_module(
        device,
        composer,
        "wgr_gpu_driven",
        include_str!("gpu_driven.wgsl"),
        "gfx3d/gpu_driven.wgsl",
    );
    let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("wgr_gpu_driven_pipeline_layout"),
        bind_group_layouts: &[
            Some(camera_layout),
            Some(group1_layout),
            Some(bindless_layout),
            Some(sampler_layout),
            // Group 4: terrain-conform heightmap (surface_y/surface_grad), so vs_gpu can
            // conform ClipLand vegetation/fences per vertex — same binding as shader3d.
            Some(conform_layout),
        ],
        immediate_size: 0,
    });
    // pos / norm / uv / conform_sel / uv1 (locations 0/1/2/5/7). Same WgrMeshVertex stride as
    // the per-draw path; location 5 (the conform selector at byte 32) drives mode-2 conform.
    // Location 7 is the SECOND UV set (byte 60, after the authored tangent frame): Multi's
    // mask / macro / ambient-shadow stages are authored on `tex1`, and sampling them with
    // the tiling uv painted rectangles across every Multi wall on Takistan (walls_l3.rvmat).
    let vbuf_attrs = [
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 12, shader_location: 1 },
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 24, shader_location: 2 },
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Uint32, offset: 32, shader_location: 5 },
        wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 60, shader_location: 7 },
    ];
    let vbuf_layout = wgpu::VertexBufferLayout {
        array_stride: super::BAKED_VERT_SIZE,
        step_mode: wgpu::VertexStepMode::Vertex,
        attributes: &vbuf_attrs,
    };
    let linear = if surface_format == wgpu::TextureFormat::Rgba16Float {
        1.0
    } else {
        0.0
    };
    // The GPU-driven set mixes opaque + cutout sections through one pipeline. Under MSAA foliage
    // A2C, the colour shader decides coverage per-fragment (cutout -> sharpened, opaque -> 1.0);
    // `a2c` just tells it the pipeline has alpha_to_coverage enabled. Module-level override, so
    // it's valid to hand to both stages of this module.
    // ONE constants map, handed to every stage of BOTH pipelines.
    //
    // This is not tidiness. `vs_gpu` is the vertex stage of the colour pipeline AND of the
    // prepass pipeline, and the colour pass depth-tests (reversed-Z GreaterEqual) against the
    // depth the prepass wrote — so the two must be the same program, specialised the same way,
    // or a fragment can fail its own prepass depth and leave the pixel at the clear colour.
    // shader3d states the same invariant for the CPU twin ("same VS + override constants ->
    // bit-for-bit depth as fs_main", shader3d.wgsl ~655); the GPU-driven prepass was built with
    // `PipelineCompilationOptions::default()` and therefore did not hold it.
    //
    // `prepass_match` = 1 makes the prepass FS apply fs_gpu's full discard set (Multi's layer-0
    // uv transform on the cutout test, plus the waterline clip). WGR_PREPASS_MATCH=0 restores
    // the previous behaviour for an A/B; it is read once at pipeline build, so an arm is two
    // runs of the same binary differing in exactly one term.
    let prepass_match = prepass_match_from_env();
    let constants = [
        ("linear", linear),
        ("a2c", if foliage_a2c { 1.0 } else { 0.0 }),
        ("prepass_match", prepass_match),
        ("emissive_night", super::emissive_night() as f64),
    ];
    let depth_stencil = wgpu::DepthStencilState {
        format: super::DEPTH_FORMAT,
        depth_write_enabled: Some(true),
        depth_compare: Some(wgpu::CompareFunction::GreaterEqual), // reversed-Z
        stencil: wgpu::StencilState::default(),
        bias: wgpu::DepthBiasState::default(),
    };
    let primitive = wgpu::PrimitiveState {
        topology: wgpu::PrimitiveTopology::TriangleList,
        front_face,
        cull_mode: Some(wgpu::Face::Back),
        ..Default::default()
    };
    let vbuffers = [vbuf_layout];
    let color = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("wgr_gpu_driven_pipeline"),
        layout: Some(&pipeline_layout),
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("vs_gpu"),
            compilation_options: wgpu::PipelineCompilationOptions {
                constants: &constants,
                ..Default::default()
            },
            buffers: &vbuffers,
        },
        primitive,
        depth_stencil: Some(depth_stencil.clone()),
        multisample: wgpu::MultisampleState {
            count: sample_count,
            alpha_to_coverage_enabled: foliage_a2c,
            ..Default::default()
        },
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("fs_gpu"),
            compilation_options: wgpu::PipelineCompilationOptions {
                constants: &constants,
                ..Default::default()
            },
            targets: &[Some(wgpu::ColorTargetState {
                format: surface_format,
                blend: None, // opaque
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview_mask: None,
        cache: None,
    });
    let prepass = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("wgr_gpu_driven_prepass_pipeline"),
        layout: Some(&pipeline_layout),
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("vs_gpu"),
            // The SAME constants as the colour pipeline's vs_gpu — see the `constants` comment.
            compilation_options: wgpu::PipelineCompilationOptions {
                constants: &constants,
                ..Default::default()
            },
            buffers: &vbuffers,
        },
        primitive,
        depth_stencil: Some(depth_stencil),
        multisample: wgpu::MultisampleState {
            count: sample_count,
            alpha_to_coverage_enabled: foliage_a2c,
            ..Default::default()
        },
        fragment: Some(wgpu::FragmentState {
            module: &module,
            // A2C twin emits a vec4 whose .a carries coverage; the plain prepass writes the vec2
            // normal only. Coverage matches fs_gpu so the depth pass covers the same samples.
            entry_point: Some(if foliage_a2c {
                "fs_gpu_prepass_a2c"
            } else {
                "fs_gpu_prepass"
            }),
            compilation_options: wgpu::PipelineCompilationOptions {
                constants: &constants,
                ..Default::default()
            },
            targets: &[Some(wgpu::ColorTargetState {
                format: super::NORMAL_FORMAT,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview_mask: None,
        cache: None,
    });
    (color, prepass)
}

// Build the GPU-driven SHADOW depth pipeline (gpu_driven_shadow.wgsl): the retained set cast
// into a cascade's depth map, consuming that cascade's cull args. Depth-only, forward-Z (clear
// 1.0 / LessEqual / no reversed-Z — mirrors the CPU shadow_depth pipeline), CW winding + NO
// back-face cull (single-sided walls/roofs must still cast), and the SAME depth bias as the CPU
// caster pipeline so GPU + CPU casters land at the same offset. One pipeline serves both opaque
// variants: the FS discards cutout foliage below the per-section alpha_ref (solid sections carry
// alpha_ref = 0 and never discard). Groups: 0 = the shadow pass UBO (light-VP, dynamic offset
// per cascade), 1 = instances/records/materials (shared with the colour path), 2/3 = bindless
// textures + sampler array, 4 = the terrain-conform heightmap.
#[allow(clippy::too_many_arguments)]
pub fn build_gpu_shadow_pipeline(
    device: &wgpu::Device,
    composer: &mut naga_oil::compose::Composer,
    shadow_pass_layout: &wgpu::BindGroupLayout,
    group1_layout: &wgpu::BindGroupLayout,
    bindless_layout: &wgpu::BindGroupLayout,
    sampler_layout: &wgpu::BindGroupLayout,
    conform_layout: &wgpu::BindGroupLayout,
) -> wgpu::RenderPipeline {
    let module = crate::shaders::make_module(
        device,
        composer,
        "wgr_gpu_driven_shadow",
        include_str!("gpu_driven_shadow.wgsl"),
        "gfx3d/gpu_driven_shadow.wgsl",
    );
    let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("wgr_gpu_driven_shadow_pipeline_layout"),
        bind_group_layouts: &[
            Some(shadow_pass_layout),
            Some(group1_layout),
            Some(bindless_layout),
            Some(sampler_layout),
            Some(conform_layout),
        ],
        immediate_size: 0,
    });
    let vbuf_attrs =
        wgpu::vertex_attr_array![0 => Float32x3, 1 => Float32x3, 2 => Float32x2, 5 => Uint32];
    let vbuf_layout = wgpu::VertexBufferLayout {
        array_stride: super::BAKED_VERT_SIZE,
        step_mode: wgpu::VertexStepMode::Vertex,
        attributes: &vbuf_attrs,
    };
    let vbuffers = [vbuf_layout];
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("wgr_gpu_driven_shadow_pipeline"),
        layout: Some(&pipeline_layout),
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("vs_gpu_shadow"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            buffers: &vbuffers,
        },
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            front_face: wgpu::FrontFace::Cw,
            cull_mode: None, // single-sided walls/roofs must still cast
            ..Default::default()
        },
        depth_stencil: Some(wgpu::DepthStencilState {
            format: super::SHADOW_FORMAT,
            depth_write_enabled: Some(true),
            depth_compare: Some(wgpu::CompareFunction::LessEqual),
            stencil: wgpu::StencilState::default(),
            bias: wgpu::DepthBiasState {
                constant: 4,
                slope_scale: 2.5,
                clamp: 0.0,
            },
        }),
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("fs_gpu_shadow"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            targets: &[],
        }),
        multiview_mask: None,
        cache: None,
    })
}

// Group-1 layout for the cull-sphere DEBUG pass: the retained instance buffer + the model
// table, both read-only storage in the vertex stage (the VS recovers centre + radius).
pub fn cull_debug_layout(device: &wgpu::Device) -> wgpu::BindGroupLayout {
    let storage = |binding: u32| wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::VERTEX,
        ty: wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage { read_only: true },
            has_dynamic_offset: false,
            min_binding_size: None,
        },
        count: None,
    };
    device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("wgr_cull_debug_group1"),
        entries: &[storage(0), storage(1)],
    })
}

// Build the cull-sphere debug pipeline (cull_debug.wgsl): an instanced LINE-LIST wireframe over
// the retained instances. Group 0 = camera (dynamic offset), group 1 = instances + models.
// Depth: test ALWAYS + no write, so the spheres draw on top of the scene (visible even where the
// object itself vanished) without disturbing the depth buffer.
pub fn build_cull_debug_pipeline(
    device: &wgpu::Device,
    composer: &mut naga_oil::compose::Composer,
    camera_layout: &wgpu::BindGroupLayout,
    group1_layout: &wgpu::BindGroupLayout,
    surface_format: wgpu::TextureFormat,
    sample_count: u32,
) -> wgpu::RenderPipeline {
    let module = crate::shaders::make_module(
        device,
        composer,
        "wgr_cull_debug",
        include_str!("cull_debug.wgsl"),
        "gfx3d/cull_debug.wgsl",
    );
    let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("wgr_cull_debug_pipeline_layout"),
        bind_group_layouts: &[Some(camera_layout), Some(group1_layout)],
        immediate_size: 0,
    });
    let depth_stencil = wgpu::DepthStencilState {
        format: super::DEPTH_FORMAT,
        depth_write_enabled: Some(false),
        depth_compare: Some(wgpu::CompareFunction::Always), // debug: always on top
        stencil: wgpu::StencilState::default(),
        bias: wgpu::DepthBiasState::default(),
    };
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("wgr_cull_debug_pipeline"),
        layout: Some(&pipeline_layout),
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("vs_sphere"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            buffers: &[],
        },
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::LineList,
            cull_mode: None,
            ..Default::default()
        },
        depth_stencil: Some(depth_stencil),
        multisample: wgpu::MultisampleState {
            count: sample_count,
            ..Default::default()
        },
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("fs_sphere"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            targets: &[Some(wgpu::ColorTargetState {
                format: surface_format,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview_mask: None,
        cache: None,
    })
}

// Ensure one view's compute-output buffers hold enough for the current scene: the indirect args
// (CULL_VARIANT_COUNT * variant_capacity slots), the flat per-draw records (same total slot count
// — the upper bound on surviving pairs), and the per-section instancing-collapse scratch
// (sections_len words). Reallocates (and reports true) when any is short or unallocated; the main
// view and every shadow cascade use this so their output layout is identical. Counters are a
// fixed buffer allocated per view up front (not here).
#[allow(clippy::too_many_arguments)]
fn ensure_view_outputs(
    device: &wgpu::Device,
    variant_capacity: u32,
    sections_len: u64,
    out_args: &mut Option<wgpu::Buffer>,
    out_records: &mut Option<wgpu::Buffer>,
    out_args_cap: &mut u64,
    sec_count: &mut Option<wgpu::Buffer>,
    sec_count_cap: &mut u64,
) -> bool {
    let mut grew = false;

    let args_bytes = CULL_VARIANT_COUNT as u64 * variant_capacity as u64 * super::INDIRECT_ARG_SIZE;
    if *out_args_cap < args_bytes || out_args.is_none() {
        *out_args = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_out_args"),
            size: args_bytes,
            usage: wgpu::BufferUsages::INDIRECT
                | wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        }));
        // Records: the flat run-carved array, one slot per (arg-slot) upper bound, 8 B each.
        let slots = CULL_VARIANT_COUNT as u64 * variant_capacity as u64;
        *out_records = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_out_records"),
            size: slots * std::mem::size_of::<RecordGpu>() as u64,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        }));
        *out_args_cap = args_bytes;
        grew = true;
    }

    // Per-section scratch, sized to the section table. COPY_DST so it can be cleared each frame.
    let sec_bytes = sections_len.max(1) * 4;
    if *sec_count_cap < sec_bytes || sec_count.is_none() {
        *sec_count = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_cull_sec_count"),
            size: sec_bytes,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        *sec_count_cap = sec_bytes;
        grew = true;
    }

    grew
}

// Upload a whole CPU slice into a StorageArray, growing it if needed. Returns whether the
// backing buffer moved (so a bind group referencing it must be rebuilt).
fn upload_slice<T: bytemuck::Pod>(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    arr: &mut super::StorageArray,
    data: &[T],
) -> bool {
    let bytes = std::mem::size_of_val(data).max(1) as u64;
    let grew = arr.ensure(device, bytes);
    if !data.is_empty() {
        queue.write_buffer(arr.buf.as_ref().unwrap(), 0, bytemuck::cast_slice(data));
    }
    grew
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    #[test]
    fn cull_wgsl_validates() {
        let module =
            naga::front::wgsl::parse_str(include_str!("cull.wgsl")).expect("cull.wgsl parse");
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .expect("cull.wgsl validate");
    }

    #[test]
    fn reduced_view_scales_only_the_pixel_detail_metric() {
        let input = CullInputs {
            objects_z: 1500.0,
            lod_scale: 0.75,
            lod_inv_width: 0.001,
            pixel_limit: 0.125,
        };
        let reduced = input.for_reduced_view(2.7);
        assert_eq!(reduced.objects_z, input.objects_z);
        assert_eq!(reduced.lod_scale, input.lod_scale);
        assert!((reduced.lod_inv_width - 0.0027).abs() < 1.0e-7);
        assert_eq!(reduced.pixel_limit, input.pixel_limit);
        assert_eq!(input.for_reduced_view(f32::NAN).lod_inv_width, 0.001);
    }

    fn inside(planes: &[[f32; 4]; 6], p: Vec3) -> bool {
        planes
            .iter()
            .all(|pl| pl[0] * p.x + pl[1] * p.y + pl[2] * p.z + pl[3] >= 0.0)
    }

    // The engine's projection as it actually reaches glam: C++ builds a row-major D3D GfxMatrix
    // (w_clip = +z_view, reversed-Z infinite far with _33 = 1, _43 = -near) and from_cols_array
    // reads it transposed. Columns here = the engine matrix's ROWS.
    fn engine_proj(inv_left: f32, inv_top: f32, near: f32) -> Mat4 {
        Mat4::from_cols(
            Vec4::new(inv_left, 0.0, 0.0, 0.0), // engine row0
            Vec4::new(0.0, inv_top, 0.0, 0.0),  // engine row1
            Vec4::new(0.0, 0.0, 1.0, 1.0),      // engine row2: _33 = 1, _34 = 1
            Vec4::new(0.0, 0.0, -near, 0.0),    // engine row3: _43 = -near, _44 = 0
        )
    }

    // Engine-layout smoke test: with the real row-major D3D projection (w_clip = +z_view), the
    // near plane params_from_camera builds (= row3 of proj*view, the clip.w>=0 half-space) must
    // keep geometry in FRONT of an +X-looking camera and reject what's behind.
    #[test]
    fn params_from_camera_near_plane_faces_forward() {
        // Orthonormal engine-style view basis, forward (Direction, view col 2) = +X.
        let view = Mat4::from_cols(
            Vec4::new(0.0, 0.0, 1.0, 0.0), // aside
            Vec4::new(0.0, 1.0, 0.0, 0.0), // up
            Vec4::new(1.0, 0.0, 0.0, 0.0), // dir = forward = +X
            Vec4::W,
        );
        let proj = engine_proj(1.0, 1.0, 0.1);
        let cam_pos = Vec3::new(5.0, 0.0, 0.0);
        let p = params_from_camera(view, proj, cam_pos, CullInputs::default());

        // Near plane (index 4) points along +X (engine forward), through the camera origin.
        let near = p.frustum[4];
        assert!(
            near[0] > 0.9,
            "near normal must be +X (engine forward), got {near:?}"
        );
        let dot = |pl: [f32; 4], v: Vec3| pl[0] * v.x + pl[1] * v.y + pl[2] * v.z + pl[3];
        // Camera-relative (world - cam_pos): in front of +X is inside, behind is out.
        assert!(
            dot(near, Vec3::new(20.0, 0.0, 0.0) - cam_pos) >= 0.0,
            "front inside near"
        );
        assert!(
            dot(near, Vec3::new(-20.0, 0.0, 0.0) - cam_pos) < 0.0,
            "behind outside near"
        );
    }

    // The decisive guard: every plane from frustum_planes must AGREE with the actual projection,
    // for rotated (yaw+pitch) cameras. A camera-relative point that the projection shows
    // (clip.w > 0 and within the NDC x/y box) must be KEPT — never culled. This catches a wrong
    // near normal (the through-origin near plane is distance-independent, so a bad forward pops
    // geometry in/out purely by look direction — the observed bug) and any swapped/rotated side
    // plane. Also verifies behind-camera and far-to-the-side points ARE culled.
    #[test]
    #[allow(deprecated)] // glam look_at_rh / perspective_infinite_reverse_rh, test-only
    fn frustum_matches_projection_rotated() {
        let proj = Mat4::perspective_infinite_reverse_rh(70f32.to_radians(), 16.0 / 9.0, 0.05);
        for dir in [
            Vec3::new(1.0, 0.3, 0.2),
            Vec3::new(-0.4, 0.8, -1.0),
            Vec3::new(0.2, -0.9, 0.5),
            Vec3::new(-1.0, -0.2, -0.3),
        ] {
            let dir = dir.normalize();
            let mut view = Mat4::look_at_rh(Vec3::ZERO, dir, Vec3::Y);
            view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0); // camera-relative (translation zeroed)
            let m = proj * view;
            let planes = frustum_planes(m);

            // Grid of camera-relative points: whatever the projection renders must be kept.
            for gx in -6..=6 {
                for gy in -6..=6 {
                    for gz in 1..=12 {
                        let p = Vec3::new(gx as f32 * 3.0, gy as f32 * 3.0, gz as f32 * 4.0);
                        let clip = m * p.extend(1.0);
                        let ndc_visible =
                            clip.w > 1e-3 && clip.x.abs() <= clip.w && clip.y.abs() <= clip.w;
                        if ndc_visible {
                            assert!(
                                inside(&planes, p),
                                "projection-visible point {p:?} wrongly culled (dir {dir:?})"
                            );
                        }
                    }
                }
            }
            // Behind the camera must be culled (near plane must face the right way).
            assert!(
                !inside(&planes, dir * -10.0),
                "behind-camera point kept (dir {dir:?})"
            );
            // 90 deg off the view axis (straight out the camera's right) must be culled by a side.
            let right = dir.cross(Vec3::Y).normalize();
            assert!(
                !inside(&planes, right * 50.0),
                "side point kept (dir {dir:?})"
            );
        }
    }

    // A reversed-Z, infinite-far perspective (what this backend uses) + a look-at view with
    // the translation ZEROED (the engine's camera-relative convention). The planes are then
    // camera-relative, so points are tested as `world - eye`: the view centre is inside;
    // behind the camera and off to the sides are out.
    #[test]
    #[allow(deprecated)] // glam's look_at_rh / perspective_infinite_reverse_rh, test-only
    fn frustum_planes_classify_points() {
        let eye = Vec3::new(0.0, 0.0, 10.0);
        let target = Vec3::ZERO;
        let up = Vec3::Y;
        let mut view = Mat4::look_at_rh(eye, target, up);
        // Engine convention: geometry is camera-relative, so the view translation is zeroed.
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_infinite_reverse_rh(60f32.to_radians(), 16.0 / 9.0, 0.1);
        let planes = frustum_planes(proj * view);

        // Points are CAMERA-RELATIVE (world - eye).
        // Straight ahead, well inside the view.
        assert!(inside(&planes, target - eye)); // rel (0,0,-10)
        assert!(inside(&planes, Vec3::new(0.0, 0.0, 5.0) - eye)); // rel (0,0,-5)
        // Behind the camera -> outside the near plane. rel (0,0,10).
        assert!(!inside(&planes, Vec3::new(0.0, 0.0, 20.0) - eye));
        // Far to the side, in front -> outside a side plane. rel (100,0,-10) / (0,100,-10).
        assert!(!inside(&planes, Vec3::new(100.0, 0.0, 0.0) - eye));
        assert!(!inside(&planes, Vec3::new(0.0, 100.0, 0.0) - eye));
        // A distant point straight ahead is NOT culled by the planes (radial distance,
        // not a flat far plane, handles far — the no-op far slot must never reject it).
        assert!(inside(&planes, Vec3::new(0.0, 0.0, -5000.0) - eye));
    }

    // Best-effort headless device; returns None (test skips) when no adapter is available
    // (e.g. CI without a GPU).
    pub(crate) fn headless() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).ok()
    }

    fn read_u32s(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        buf: &wgpu::Buffer,
        len: u64,
    ) -> Vec<u32> {
        let bytes = len * 4;
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("readback"),
            size: bytes,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        enc.copy_buffer_to_buffer(buf, 0, &staging, 0, bytes);
        queue.submit(std::iter::once(enc.finish()));
        let slice = staging.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        rx.recv().unwrap().unwrap();
        let data = slice.get_mapped_range();
        bytemuck::cast_slice::<u8, u32>(&data).to_vec()
    }

    // End-to-end: register a model, add three instances (one visible, one behind the
    // camera, one past the draw distance), run the compute, and assert exactly the visible
    // one produced an indirect draw for its chosen LOD's section.
    #[test]
    #[allow(deprecated)] // glam look_at_rh / perspective_infinite_reverse_rh, test-only
    fn cull_compute_end_to_end() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut cull = CullState::new(&device);

        // 1 model, 2 LODs (finest -> section 0, next -> section 1), variant 0.
        let sections = [
            SectionGpu {
                first_index: 0,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
            SectionGpu {
                first_index: 3,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
        ];
        let lods = [
            LodGpu {
                resolution: 0.0,
                section_base: 0,
                section_count: 1,
                is_decal: 0,
            },
            LodGpu {
                resolution: 10.0,
                section_base: 1,
                section_count: 1,
                is_decal: 0,
            },
        ];
        let materials = [SectionMaterialGpu::zeroed(); 2];
        let model = cull.register_model(1.0, &lods, &sections, &materials);

        let mk = |z: f32| InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(), // cull uses `center`, not `world`
            center: [0.0, 0.0, z, 1.0],
            model,
            flags: 0,
            cull_radius: 0,
            _pad: 0,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        };
        let front = cull.instance_add(mk(0.0)); // 10 units ahead: visible
        let _behind = cull.instance_add(mk(20.0)); // behind the camera at z=10
        let _far = cull.instance_add(mk(-9000.0)); // past the draw distance

        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        // Camera-relative convention (matches the engine + the compute's `rel` test).
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_infinite_reverse_rh(60f32.to_radians(), 1.0, 0.1);
        let mut params = CullParamsGpu::zeroed();
        params.frustum = frustum_planes(proj * view);
        params.cam_pos = [eye.x, eye.y, eye.z, 0.0];
        params.objects_z2 = 100.0 * 100.0;
        params.lod_scale = 1.0;
        params.lod_inv_width = 1.0;
        params.pixel_limit = 0.0; // disable sub-pixel cull for a deterministic result
        cull.set_params(params);

        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.dispatch(&mut enc);
        queue.submit(std::iter::once(enc.finish()));

        // resol2 = dist²(=100) * lod_scale²(=1) => level 1 (section 1: first_index 3).
        let words_per_arg = super::ARG_WORDS;
        let total = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64 * words_per_arg;
        let raw = read_u32s(&device, &queue, cull.out_args().unwrap(), total);
        let live: Vec<&[u32]> = raw
            .chunks_exact(words_per_arg as usize)
            .filter(|a| a[1] != 0) // instance_count != 0
            .collect();
        assert_eq!(live.len(), 1, "exactly one instance should draw");
        let a = live[0];
        assert_eq!(a[0], 3, "index_count of the chosen LOD's section");
        assert_eq!(a[1], 1, "instance_count == 1");
        assert_eq!(a[2], 3, "first_index of section 1");
        // first_instance is the record slot; the record resolves to the visible instance
        // and LOD 1's section (global section id 1).
        let rec_slot = a[4] as u64;
        let slots = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
        let recs = read_u32s(&device, &queue, cull.out_records().unwrap(), slots * 2);
        assert_eq!(
            recs[rec_slot as usize * 2],
            front,
            "record.instance == visible slot"
        );
        assert_eq!(
            recs[rec_slot as usize * 2 + 1],
            1,
            "record.section == LOD1 section id"
        );
    }

    // Multi-view (§6): a shadow-cascade view culls the SAME retained scene against its own
    // frustum into its OWN args/records, independent of the main view. Here the cascade frustum
    // = the main camera's (a stand-in for a light-VP that frames the instance), so the visible
    // instance must produce exactly one draw in the shadow view's args — proving the per-view
    // params/outputs/bind + dispatch_shadow wire up correctly.
    #[test]
    #[allow(deprecated)] // glam look_at_rh / perspective_infinite_reverse_rh, test-only
    fn shadow_cull_view_end_to_end() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut cull = CullState::new(&device);

        let sections = [
            SectionGpu {
                first_index: 0,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
            SectionGpu {
                first_index: 3,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
        ];
        let lods = [
            LodGpu {
                resolution: 0.0,
                section_base: 0,
                section_count: 1,
                is_decal: 0,
            },
            LodGpu {
                resolution: 10.0,
                section_base: 1,
                section_count: 1,
                is_decal: 0,
            },
        ];
        let materials = [SectionMaterialGpu::zeroed(); 2];
        let model = cull.register_model(1.0, &lods, &sections, &materials);

        let mk = |z: f32| InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(),
            center: [0.0, 0.0, z, 1.0],
            model,
            flags: 0,
            cull_radius: 0,
            _pad: 0,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        };
        let front = cull.instance_add(mk(0.0)); // in front of the camera
        let _behind = cull.instance_add(mk(20.0)); // behind (culled by the near plane)

        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_infinite_reverse_rh(60f32.to_radians(), 1.0, 0.1);
        let mut params = CullParamsGpu::zeroed();
        params.frustum = frustum_planes(proj * view);
        params.cam_pos = [eye.x, eye.y, eye.z, 0.0];
        params.objects_z2 = 100.0 * 100.0;
        params.lod_scale = 1.0;
        params.lod_inv_width = 1.0;
        params.pixel_limit = 0.0;
        cull.set_params(params);

        // One shadow cascade view: light_vp = the main proj*view stand-in, distance cull off.
        cull.set_shadow_view_count(&device, 1);
        let inputs = CullInputs {
            objects_z: 900.0,
            lod_scale: 1.0,
            lod_inv_width: 1.0,
            pixel_limit: 0.0,
        };
        cull.set_shadow_params(0, params_from_shadow_cascade(proj * view, eye, inputs));

        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.dispatch(&mut enc);
        cull.dispatch_shadow(&mut enc, 0);
        queue.submit(std::iter::once(enc.finish()));

        // The shadow view's own args: exactly the front instance drew (LOD 1's section).
        let words_per_arg = super::ARG_WORDS;
        let total = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64 * words_per_arg;
        let raw = read_u32s(&device, &queue, cull.shadow_out_args(0).unwrap(), total);
        let live: Vec<&[u32]> = raw
            .chunks_exact(words_per_arg as usize)
            .filter(|a| a[1] != 0)
            .collect();
        assert_eq!(live.len(), 1, "exactly one instance casts into the cascade");
        assert_eq!(live[0][0], 3, "index_count of the chosen LOD's section");
        assert_eq!(live[0][1], 1, "instance_count == 1");
        assert_eq!(live[0][2], 3, "first_index of section 1");
        // The shadow record resolves to the front instance + LOD 1's section.
        let rec_slot = live[0][4] as u64;
        let slots = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
        let recs = read_u32s(
            &device,
            &queue,
            cull.shadow_out_records(0).unwrap(),
            slots * 2,
        );
        assert_eq!(
            recs[rec_slot as usize * 2],
            front,
            "shadow record.instance == front slot"
        );
        assert_eq!(
            recs[rec_slot as usize * 2 + 1],
            1,
            "shadow record.section == LOD1 section id"
        );
    }

    // The sky map's ortho frustum, extracted exactly as prepare_cull does it, at the WORLD
    // COORDINATES a real mission uses. Measured in-game before this existed: the sky cull
    // rejected all 98684 instances while the same view with the frustum test disabled kept 192
    // sub-draws, so the planes — not the dispatch, the bind or the args — were the rejector.
    #[test]
    fn sky_ortho_frustum_accepts_what_is_inside_the_box() {
        let s = super::super::sky_vis::SkyVisSettings::default();
        // A plausible mission position on Everon, not the origin: whatever is wrong here is
        // invisible at (0,0,0), where every translation term is zero.
        let cam = Vec3::new(2537.0, 21.5, -4381.0);
        let sky = super::super::sky_vis::build_view(cam, &s);
        let vp_rel = sky.view_proj * Mat4::from_translation(cam);
        let planes = frustum_planes(vp_rel);

        // Camera-relative offsets, as the cull tests them (center - cam_pos).
        assert!(inside(&planes, Vec3::ZERO), "under the camera");
        assert!(inside(&planes, Vec3::new(20.0, -5.0, 30.0)), "nearby");
        assert!(
            inside(&planes, Vec3::new(0.0, 250.0, 0.0)),
            "high above: the box spans +-height, and a roof ABOVE the camera is the whole point"
        );
        assert!(
            !inside(&planes, Vec3::new(4000.0, 0.0, 0.0)),
            "far outside the box"
        );
    }

    // The interior sky-visibility view (docs/interior-sky-visibility-plan.md §4) on a real
    // device: its ortho box must accept what is under the camera and reject what is outside it.
    //
    // This runs on the headless device rather than validating WGSL, because the failure this
    // guards is a RESOURCE one — a standalone extra view whose bind group or output buffers were
    // never prepared produces no draws at all, and Naga has nothing to say about that.
    #[test]
    fn sky_cull_view_covers_the_box_and_rejects_outside_it() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut cull = CullState::new(&device);

        let sections = [SectionGpu {
            first_index: 0,
            index_count: 3,
            base_vertex: 0,
            variant: 0,
        }];
        let lods = [LodGpu {
            resolution: 0.0,
            section_base: 0,
            section_count: 1,
            is_decal: 0,
        }];
        let materials = [SectionMaterialGpu::zeroed(); 1];
        let model = cull.register_model(4.0, &lods, &sections, &materials);

        let mk = |x: f32| InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(),
            center: [x, 0.0, 0.0, 1.0],
            model,
            flags: 0,
            cull_radius: 0,
            _pad: 0,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        };
        let under = cull.instance_add(mk(0.0)); // under the camera: inside the box
        let _far = cull.instance_add(mk(4000.0)); // far outside the 128 m half-extent

        // Main view params: instance_count comes from here, and dispatch_sky no-ops at 0.
        let eye = Vec3::new(0.0, 20.0, 0.0);
        let mut params = CullParamsGpu::zeroed();
        params.cam_pos = [eye.x, eye.y, eye.z, 0.0];
        params.objects_z2 = 1.0e30;
        params.lod_scale = 1.0;
        params.lod_inv_width = 1.0;
        params.pixel_limit = 0.0;
        params.debug_flags = 1; // main view: frustum off, it is not what is under test
        cull.set_params(params);

        let settings = super::super::sky_vis::SkyVisSettings::default();
        let sky = super::super::sky_vis::build_view(eye, &settings);
        // Camera-relative, exactly as prepare_cull hands it over (the cull tests
        // center - cam_pos, and the depth VS makes vertices camera-relative before the VP).
        let vp_rel = sky.view_proj * Mat4::from_translation(eye);
        let inputs = CullInputs {
            objects_z: 900.0,
            lod_scale: 1.0,
            lod_inv_width: 1.0,
            pixel_limit: 0.0,
        };
        cull.set_sky_view_count(&device, super::super::sky_vis::DIRECTION_COUNT);
        cull.set_sky_params(0, params_from_shadow_cascade(vp_rel, eye, inputs));

        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.dispatch(&mut enc);
        cull.dispatch_sky(&mut enc, 0);
        queue.submit(std::iter::once(enc.finish()));

        let words_per_arg = super::ARG_WORDS;
        let total = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64 * words_per_arg;
        let raw = read_u32s(&device, &queue, cull.sky_out_args(0).unwrap(), total);
        let live: Vec<&[u32]> = raw
            .chunks_exact(words_per_arg as usize)
            .filter(|a| a[1] != 0)
            .collect();
        assert_eq!(live.len(), 1, "one section survives the sky ortho frustum");
        assert_eq!(
            live[0][1], 1,
            "instance_count == 1: the far instance is outside the box"
        );
        let rec_slot = live[0][4] as u64;
        let slots = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
        let recs = read_u32s(&device, &queue, cull.sky_out_records(0).unwrap(), slots * 2);
        assert_eq!(
            recs[rec_slot as usize * 2],
            under,
            "the surviving record is the instance under the camera"
        );

        // Disabling the view must actually release it — a stale sky view would keep drawing an
        // out-of-date map into the frame after the feature was switched off.
        cull.set_sky_view_count(&device, 0);
        cull.prepare(&device, &queue);
        assert!(cull.sky_out_args(0).is_none());
        assert!(cull.sky_out_records(0).is_none());
    }

    #[test]
    fn hiz_wgsl_validates() {
        let module =
            naga::front::wgsl::parse_str(include_str!("hiz.wgsl")).expect("hiz.wgsl parse");
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .expect("hiz.wgsl validate");
    }

    // A constant-value Hi-Z pyramid (all mips filled with `value`), so whichever mip the
    // occlusion test picks reads the same depth — lets the test drive the reversed-Z comparison
    // deterministically without a real depth reduction.
    fn const_hiz(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        w: u32,
        h: u32,
        value: f32,
    ) -> wgpu::TextureView {
        let mips = 32 - w.max(h).max(1).leading_zeros();
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("test_hiz"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: mips,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        for m in 0..mips {
            let mw = (w >> m).max(1);
            let mh = (h >> m).max(1);
            let data = vec![value; (mw * mh) as usize];
            queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: &tex,
                    mip_level: m,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                bytemuck::cast_slice(&data),
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(mw * 4),
                    rows_per_image: Some(mh),
                },
                wgpu::Extent3d {
                    width: mw,
                    height: mh,
                    depth_or_array_layers: 1,
                },
            );
        }
        tex.create_view(&wgpu::TextureViewDescriptor::default())
    }

    // End-to-end color-occlusion cull (main_occlude): one on-screen instance, run twice against a
    // constant Hi-Z. A FAR pyramid (reversed-Z 0 = nothing in front) must NOT occlude it; a NEAR
    // pyramid (reversed-Z 1 = a wall right in front, min-reduced) MUST. This pins the reversed-Z
    // min-comparison direction — the headline hazard: swap it and it culls everything or nothing.
    #[test]
    #[allow(deprecated)] // glam look_at_rh / perspective_infinite_reverse_rh, test-only
    fn color_occlusion_end_to_end() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut cull = CullState::new(&device);

        let sections = [SectionGpu {
            first_index: 0,
            index_count: 3,
            base_vertex: 0,
            variant: 0,
        }];
        let lods = [LodGpu {
            resolution: 0.0,
            section_base: 0,
            section_count: 1,
            is_decal: 0,
        }];
        let materials = [SectionMaterialGpu::zeroed(); 1];
        let model = cull.register_model(1.0, &lods, &sections, &materials);
        cull.instance_add(InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(),
            center: [0.0, 0.0, 0.0, 1.0], // at the look-at target: centred on screen
            model,
            flags: 0,
            cull_radius: 0,
            _pad: 0,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        });

        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        // FORWARD projection (near->0, far->1), matching the engine: the pipelines apply
        // frame.wgsl reverse_z (z = w - z) in-shader, so occluded() must too. Using a reversed
        // projection here would hide a missing reverse_z in the occlusion test (the original bug).
        let proj = Mat4::perspective_rh(60f32.to_radians(), 1.0, 0.1, 1000.0);
        let mut main = CullParamsGpu::zeroed();
        main.frustum = frustum_planes(proj * view);
        main.cam_pos = [eye.x, eye.y, eye.z, 0.0];
        main.objects_z2 = 1.0e6;
        main.lod_scale = 1.0;
        main.lod_inv_width = 1.0;
        main.pixel_limit = 0.0;
        cull.set_params(main);
        let inputs = CullInputs {
            objects_z: 1000.0,
            lod_scale: 1.0,
            lod_inv_width: 1.0,
            pixel_limit: 0.0,
        };

        let live_color_count = |cull: &CullState| -> usize {
            let words = super::ARG_WORDS;
            let total = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64 * words;
            let raw = read_u32s(&device, &queue, cull.color_out_args().unwrap(), total);
            raw.chunks_exact(words as usize)
                .filter(|a| a[1] != 0)
                .count()
        };

        // FAR Hi-Z (reversed-Z 0 everywhere): nothing occludes -> the instance draws.
        cull.set_hiz(Some(const_hiz(&device, &queue, 64, 64, 0.0)));
        cull.set_color_params(params_from_camera_occlude(
            view,
            proj,
            eye,
            inputs,
            [64.0, 64.0],
            7,
            true,
        ));
        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.dispatch_color(&mut enc);
        queue.submit(std::iter::once(enc.finish()));
        assert_eq!(
            live_color_count(&cull),
            1,
            "FAR Hi-Z must not occlude the visible instance"
        );

        // NEAR Hi-Z (reversed-Z 1 everywhere): a wall right in front -> the instance is occluded.
        cull.set_hiz(Some(const_hiz(&device, &queue, 64, 64, 1.0)));
        cull.set_color_params(params_from_camera_occlude(
            view,
            proj,
            eye,
            inputs,
            [64.0, 64.0],
            7,
            true,
        ));
        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.dispatch_color(&mut enc);
        queue.submit(std::iter::once(enc.finish()));
        assert_eq!(
            live_color_count(&cull),
            0,
            "NEAR Hi-Z must occlude the instance"
        );

        // MID Hi-Z (reversed-Z 0.5 = a mid-depth wall). The instance sits near the far end
        // (distance 10, near 0.1 -> reversed depth ~0.01), so a mid-depth occluder is IN FRONT of
        // it -> occluded. This is the discriminating case for the reverse_z remap: without it the
        // test would use the FORWARD depth (~0.99), read 0.99 < 0.5 = false, and wrongly draw.
        cull.set_hiz(Some(const_hiz(&device, &queue, 64, 64, 0.5)));
        cull.set_color_params(params_from_camera_occlude(
            view,
            proj,
            eye,
            inputs,
            [64.0, 64.0],
            7,
            true,
        ));
        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.dispatch_color(&mut enc);
        queue.submit(std::iter::once(enc.finish()));
        assert_eq!(
            live_color_count(&cull),
            0,
            "a mid-depth occluder must hide the far instance (reverse_z)"
        );
    }

    // Instancing collapse (§3.6): many instances that select the SAME LOD section must produce
    // ONE instanced DrawArgs (instance_count = N) with N contiguous records, and instances that
    // land on a DIFFERENT LOD must get a separate draw. Proves the three-pass count->emit->scatter
    // carves per-section runs correctly.
    // PERF-005 — the accounting must be REAL, not a readback of zeros that happens to compile.
    // The scene is built so every number has an independently known right answer:
    //   5 instances at LOD1 (global section 1) + 3 instances at LOD0 (global section 0),
    //   each section 3 indices = 1 triangle.
    // So: 8 survivors, LOD histogram {3 at lod0, 5 at lod1}, 2 draw args, 8 records, 8 triangles.
    // A vacuous readback (all zeros) and a double-counted one (SCATTER re-recording what COUNT
    // already did, which would give 16) both fail this.
    #[test]
    fn object_stats_count_survivors_lods_and_triangles() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut cull = CullState::new(&device);
        let sections = [
            SectionGpu {
                first_index: 0,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
            SectionGpu {
                first_index: 3,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
        ];
        let lods = [
            LodGpu {
                resolution: 0.0,
                section_base: 0,
                section_count: 1,
                is_decal: 0,
            },
            LodGpu {
                resolution: 10.0,
                section_base: 1,
                section_count: 1,
                is_decal: 0,
            },
        ];
        let materials = [SectionMaterialGpu::zeroed(); 2];
        let model = cull.register_model(1.0, &lods, &sections, &materials);
        let mk = |z: f32| InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(),
            center: [0.0, 0.0, z, 1.0],
            model,
            flags: 0,
            cull_radius: 0,
            _pad: 0,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        };
        for _ in 0..5 {
            cull.instance_add(mk(-5.0)); // far -> LOD1
        }
        for _ in 0..3 {
            cull.instance_add(mk(5.0)); // near -> LOD0
        }

        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_infinite_reverse_rh(60f32.to_radians(), 1.0, 0.1);
        let mut params = CullParamsGpu::zeroed();
        params.frustum = frustum_planes(proj * view);
        params.cam_pos = [eye.x, eye.y, eye.z, 0.0];
        params.objects_z2 = 1.0e6;
        params.lod_scale = 1.0;
        params.lod_inv_width = 1.0;
        params.pixel_limit = 0.0;
        cull.set_params(params);
        cull.prepare(&device, &queue);

        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut enc);
        cull.dispatch(&mut enc);
        cull.resolve_stats(&mut enc);
        queue.submit(std::iter::once(enc.finish()));
        // The harvest is deliberately non-blocking, so pump it until the ring drains rather than
        // asserting on the first (necessarily empty) attempt.
        for _ in 0..64 {
            cull.harvest_stats(&device);
            if cull.stats().valid {
                break;
            }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let stats = cull.stats();
        assert!(stats.valid, "a stats readback must land within 64 polls");
        let m = stats.views[STATS_VIEW_MAIN];
        assert_eq!(m.instances, 8, "every surviving instance counted exactly once");
        assert_eq!(m.lod_hist[0], 3, "the three near instances chose LOD 0");
        assert_eq!(m.lod_hist[1], 5, "the five far instances chose LOD 1");
        assert_eq!(
            m.lod_hist[2..].iter().sum::<u32>(),
            0,
            "no instance chose a LOD this model does not have"
        );
        assert_eq!(m.draws, 2, "one instanced draw arg per surviving section");
        assert_eq!(m.draws_solid, 2, "both sections are variant 0 (solid)");
        assert_eq!(m.draws_alpha, 0);
        assert_eq!(m.records, 8, "one record per (instance, section) pair");
        assert_eq!(m.tris, 8, "3 indices per section x 8 collapsed instances");
        assert_eq!(m.tris_solid, 8);
        // A view that never dispatched must read zeros, not another view's numbers — this is the
        // stale-region trap the GPU timers document, in its counter form.
        let unused = stats.views[STATS_VIEW_REFLECTION];
        assert_eq!(unused.instances, 0);
        assert_eq!(unused.draws, 0);
    }

    #[test]
    #[allow(deprecated)] // glam look_at_rh / perspective_infinite_reverse_rh, test-only
    fn instancing_collapse_end_to_end() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut cull = CullState::new(&device);

        // 2 LODs: LOD0 -> section 0 (coarse, near), LOD1 -> section 1 (fine, farther). variant 0.
        let sections = [
            SectionGpu {
                first_index: 0,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
            SectionGpu {
                first_index: 3,
                index_count: 3,
                base_vertex: 0,
                variant: 0,
            },
        ];
        let lods = [
            LodGpu {
                resolution: 0.0,
                section_base: 0,
                section_count: 1,
                is_decal: 0,
            },
            LodGpu {
                resolution: 10.0,
                section_base: 1,
                section_count: 1,
                is_decal: 0,
            },
        ];
        let materials = [SectionMaterialGpu::zeroed(); 2];
        let model = cull.register_model(1.0, &lods, &sections, &materials);

        let mk = |z: f32| InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(),
            center: [0.0, 0.0, z, 1.0],
            model,
            flags: 0,
            cull_radius: 0,
            _pad: 0,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        };
        // Eye at z=10 looking down -Z. Batch FAR: 5 instances at dist 15 (z=-5) -> resol2=225 ->
        // LOD1 -> global section 1. Batch NEAR: 3 at dist 5 (z=5) -> resol2=25 -> LOD0 -> section 0.
        let mut far_slots = Vec::new();
        for _ in 0..5 {
            far_slots.push(cull.instance_add(mk(-5.0)));
        }
        let mut near_slots = Vec::new();
        for _ in 0..3 {
            near_slots.push(cull.instance_add(mk(5.0)));
        }

        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_infinite_reverse_rh(60f32.to_radians(), 1.0, 0.1);
        let mut params = CullParamsGpu::zeroed();
        params.frustum = frustum_planes(proj * view);
        params.cam_pos = [eye.x, eye.y, eye.z, 0.0];
        params.objects_z2 = 1.0e6;
        params.lod_scale = 1.0;
        params.lod_inv_width = 1.0;
        params.pixel_limit = 0.0;
        cull.set_params(params);

        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.dispatch(&mut enc);
        queue.submit(std::iter::once(enc.finish()));

        let words = super::ARG_WORDS;
        let total = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64 * words;
        let raw = read_u32s(&device, &queue, cull.out_args().unwrap(), total);
        let live: Vec<&[u32]> = raw
            .chunks_exact(words as usize)
            .filter(|a| a[1] != 0)
            .collect();
        assert_eq!(
            live.len(),
            2,
            "one instanced draw per surviving section (not per pair)"
        );

        let slots_cap = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
        let recs = read_u32s(&device, &queue, cull.out_records().unwrap(), slots_cap * 2);
        // (instance, section) at record slot `r`.
        let rec = |r: usize| (recs[r * 2], recs[r * 2 + 1]);

        // Assert one arg: instance_count `n`, its `n` contiguous records carry `section`, and the
        // record instances match `want` (as a set — run order within a section is arbitrary).
        let check = |arg: &[u32], n: u32, first_index: u32, section: u32, want: &[u32]| {
            assert_eq!(arg[1], n, "instance_count collapsed");
            assert_eq!(arg[2], first_index, "section's first_index");
            let base = arg[4] as usize;
            let mut got: Vec<u32> = (0..n as usize)
                .map(|i| {
                    let (inst, sec) = rec(base + i);
                    assert_eq!(sec, section, "record tagged with its section");
                    inst
                })
                .collect();
            got.sort();
            let mut want = want.to_vec();
            want.sort();
            assert_eq!(got, want, "records = the instances that chose this section");
            base
        };

        let a_far = live
            .iter()
            .find(|a| a[2] == 3)
            .expect("section 1 arg (5 collapsed)");
        let a_near = live
            .iter()
            .find(|a| a[2] == 0)
            .expect("section 0 arg (3 collapsed)");
        let base_far = check(a_far, 5, 3, 1, &far_slots);
        let base_near = check(a_near, 3, 0, 0, &near_slots);
        // Runs are disjoint (contiguous carving, no overlap).
        assert!(
            base_far + 5 <= base_near || base_near + 3 <= base_far,
            "per-section record runs must not overlap"
        );
    }
}
