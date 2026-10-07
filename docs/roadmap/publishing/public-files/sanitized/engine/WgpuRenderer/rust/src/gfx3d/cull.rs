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
use std::cell::{Cell, RefCell};

const INSTANCE_MAIN_CAMERA_ONLY: u32 = 32; // WGR_INSTANCE_MAIN_CAMERA_ONLY
const INSTANCE_OTHER_VIEWS_ONLY: u32 = 64; // WGR_INSTANCE_OTHER_VIEWS_ONLY
const CULL_VIEW_MAIN_CAMERA: u32 = 2; // positive authority, never inferred from an unknown view

// --- GPU buffer layouts (the CPU side of the structs in cull.wgsl) ---

// Per-frame cull parameters (one uniform). 240 bytes, 16-aligned. The occlusion tail
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
    // Bit 0 skips the frustum test; bit 1 authorizes main-camera-only instances.
    pub debug_flags: u32,
    // Occlusion (main_occlude only). Camera-relative proj*view projecting a camera-relative
    // point to clip, plus the Hi-Z size/mip count and the enable flag.
    pub view_proj: [[f32; 4]; 4],
    pub viewport: [f32; 2],
    pub hiz_mips: u32,
    pub occlusion: u32,
    // Private diagnostic COUNT target; all-zero tail disables the extra atomic.
    pub target_model_id: u32,
    pub target_count_enabled: u32,
    pub _target_pad: [u32; 2],
}

/// Collision-free identity of the inputs that can change a plain COUNT's
/// target-model zero/nonzero result. LOD section choice, Hi-Z and variant
/// capacity do not affect that result. This private key is formed only for
/// diagnostic cached-view publication/lookups, never in the normal cull path.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CountCullInputKey([u32; 32]);

impl CountCullInputKey {
    pub(crate) fn from_params(p: &CullParamsGpu) -> Self {
        let mut bits = [0; 32];
        for (i, plane) in p.frustum.iter().enumerate() {
            for (j, value) in plane.iter().enumerate() {
                bits[i * 4 + j] = value.to_bits();
            }
        }
        for i in 0..3 { bits[24 + i] = p.cam_pos[i].to_bits(); }
        bits[27] = p.objects_z2.to_bits();
        bits[28] = p.lod_scale.to_bits();
        bits[29] = p.lod_inv_width.to_bits();
        bits[30] = p.pixel_limit.to_bits();
        bits[31] = p.debug_flags & 3;
        Self(bits)
    }
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
    pub flags: u32, // WgrInstanceFlags; bit 5 restricts a private page to main camera
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

/// One literal retained handle, resolved without touching stale-operation counters.
/// `Absent` only becomes a removal witness when the caller retained a prior
/// `Present` fact for the same handle; an arbitrary forged handle has no history.
#[derive(Clone, Copy)]
pub(crate) enum InstanceCpuLookup {
    Invalid,
    Absent,
    Present { slot: u32, row: InstanceGpu },
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

// Optional ownership gate reads actual admitted LodGpu values; overlapping LODs
// within a birth are valid, escaping relative spans disable all reuse.
fn section_ownership_valid(lods: &[LodGpu], sections: usize, materials: usize) -> bool {
    super::section_span_pool::valid_ownership(sections, materials, lods.iter().map(|lod|
        (lod.section_base as usize, lod.section_count as usize)))
}

// Same CPU table mutation used by normal retirement and the owner-only census
// tests. Append-only model identity remains valid; historical sections do not.
pub(super) fn retire_model_lods(models: &[ModelGpu], lods: &mut [LodGpu], id: u32) -> bool {
    let mut changed = false;
    if let Some(model) = models.get(id as usize) {
        for lod in &mut lods[model.lod_base as usize..(model.lod_base + model.lod_count) as usize] {
            changed |= lod.section_count != 0;
            lod.section_count = 0;
        }
    }
    changed
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

// One exact, bounded read of the armed renderer model's CURRENT LOD/section
// table. A retired model, out-of-range span or missing source cannot
// certify palette independence. The caller resolves each section's current
// generational mesh handle and skin state; no certificate is retained.
fn palette_independent_target_sections(
    models: &[ModelGpu], lods: &[LodGpu], sections: &[SectionGpu],
    model_id: u32, mut section_plain: impl FnMut(usize, &SectionGpu) -> bool,
) -> bool {
    let Some(model) = models.get(model_id as usize) else { return false; };
    let base = model.lod_base as usize;
    let count = model.lod_count as usize;
    if count == 0 || count > 16 || base.checked_add(count).is_none_or(|end| end > lods.len()) {
        return false;
    }
    let mut visits = 0usize;
    for lod in &lods[base..base + count] {
        let first = lod.section_base as usize;
        let n = lod.section_count as usize;
        if n == 0 || n > 64 - visits || first.checked_add(n).is_none_or(|end| end > sections.len()) {
            return false;
        }
        for section in first..first + n {
            if sections[section].index_count == 0 || !section_plain(section, &sections[section]) { return false; }
        }
        visits += n;
    }
    visits != 0
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
    // MAT-048: the further layers' OWN normal maps (RVMAT Stage12/13/14), 0 = this layer keeps
    // layer 0's normal. Sampled at layer_uv[i + 1], the transform its colour uses.
    pub layer_normal_slot: [u32; 3],
    // WGSL aligns an array<vec4<f32>> to 16 bytes; Rust aligns [[f32; 4]; 4] to 4. Without
    // this the two layouts disagree and every field from layer_uv on reads the wrong memory
    // -- which renders as white buildings, not as an error.
    pub _pad_layer: [u32; 3],
    // Per layer, layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
    pub layer_uv: [[f32; 4]; 4],
    // RFG-072: per layer, layer 0 at index 0: the linear colour multiplier on the layer's
    // tile (Enfusion `Color_N`), identity elsewhere. vec4-aligned already, no pad needed.
    pub layer_colour: [[f32; 4]; 4],
    // Enfusion's per-material normal-map intensity (`NormalPower`): multiplier on the
    // decoded tangent-space XY, renormalised by the shader. 1.0 = no-op default.
    // Trailing scalar: WGSL rounds the array stride to 16, so the pad is explicit and
    // both sides agree on 272.
    pub normal_power: f32,
    pub _pad_np: [u32; 3],
    // Crown self-occlusion volume (xyz = trunk-axis centre in model space,
    // w = intensity; second lane x = height reserved, y = radius). 32 bytes,
    // 16-aligned on both sides, so the stride stays a multiple of 16.
    pub crown_ao_p0: [f32; 4],
    pub crown_ao_p1: [f32; 4],
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
// Spare stats word +20; a future per-view async copy uses this byte offset.
pub const TARGET_MODEL_COUNT_BYTE_OFFSET: u64 = (STAT_BASE_WORD + 20) * 4;
pub const STATS_WORDS: usize = 24; // +16..+19 what-if triangles (see cull.wgsl word map)
const COUNTER_WORDS: u64 = STAT_BASE_WORD + STATS_WORDS as u64;
fn counter_words_for(feedback: bool) -> u64 { COUNTER_WORDS + if feedback { super::lod_feedback::WORDS } else { 0 } }
fn counter_words() -> u64 { counter_words_for(super::lod_feedback::enabled()) }
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
pub const STAT_WHATIF: usize = 16;
pub const STAT_WHATIF_COUNT: usize = 4;
pub const STAT_TARGET_MODEL: usize = 20;
/// The detail-multiplier scales the what-if words were computed for, in word order.
pub const WHATIF_SCALES: [f32; STAT_WHATIF_COUNT] = [0.5, 0.70710678, 1.41421356, 2.0];
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
    /// Triangle total of the same survivors under WHATIF_SCALES[i] x lod_inv_width.
    pub whatif_tris: [u32; STAT_WHATIF_COUNT],
    /// Opt-in target COUNT from this sampled view. Diagnostic only: no pass or
    /// target generation accompanies the existing aggregate-stats readback.
    pub target_model_count: u32,
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
            size: counter_words() * 4,
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

/// The retained static-instance table: slots, the free list that recycles them, and the
/// per-slot generation that makes a recycled slot distinguishable from the one it replaced
/// (ID-2/ID-3, `design notes` §4).
///
/// Split out of `CullState` so the identity rules are testable without a GPU: `CullState::new`
/// builds compute pipelines and therefore needs a device, which made every test of this logic
/// skip silently on a machine with no adapter. Nothing here touches wgpu.
///
/// **The handle is `(slot + 1) | generation << 24`.** Handle 0 is always invalid, which also
/// disambiguates the FFI's 0-on-error return from a real first slot. `instance_remove` bumps
/// the slot's generation, so every handle the caller still holds stops resolving the moment
/// the instance is removed — it addresses *nothing* rather than whatever later reused the
/// slot. Treat a handle as an opaque value; never do arithmetic on it.
///
/// **Bound:** 8 bits of generation, so a stale handle aliases again only after 256
/// remove/add cycles *of the same slot*. That is a real bound, not a proof — it is asserted
/// by `stale_handle_aliases_only_after_256_generations` so the number stays honest.
#[derive(Default)]
pub(crate) struct InstanceTable {
    /// Static slots [0, len). A removed slot keeps its storage but has `model = INVALID_MODEL`,
    /// so the cull compute skips it until the slot is reused.
    slots: Vec<InstanceGpu>,
    /// Removed slots, ready to be recycled by the next `add`.
    free_slots: Vec<u32>,
    /// One generation per slot; `generations.len()` is always `slots.len()`.
    generations: Vec<u8>,
    /// Every handle this table refused. Write-only until ID-3 gave it a reader: a rejection
    /// that nothing can count is a rejection no test can distinguish from a no-op.
    stale_ops: u64,
    /// Inclusive min..=max slot changed since the last upload.
    dirty: Option<(u32, u32)>,
    /// Retained scene-content change counter: add/update/remove, and drawable model
    /// retirement even when its instance remains. Cached images must observe both kinds.
    /// This is not a revision for arbitrary mesh vertices, dynamic tails or pose palettes.
    epoch: u64,
    /// Opt-in instance-row witness for one selected model (static plus dynamic).
    /// This does not cover pose palettes, mesh vertex uploads or section/material
    /// edits. The global epoch also omits dynamic-tail changes. Neither witness
    /// authorizes cached COUNT reuse or Fine retirement.
    target_revision: Option<Box<InstanceTargetRevision>>,
    /// Optional source-local witness for *all* dynamic GPU rows, including
    /// foreign casters omitted by the selected target-model revision. This
    /// cannot establish cached-image currency on its own.
    dynamic_image_mutation: Option<Box<DynamicImageMutationWitness>>,
    /// Optional, source-local provenance for epoch churn. It classifies only
    /// retained-static table mutations; dynamic, material, texture and pose
    /// changes are outside this witness and still forbid image reuse.
    static_noop_epoch: Option<Box<StaticNoopEpochWitness>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum StaticEpochOrigin {
    BitIdenticalUpdate,
    ChangedOrUnknown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum StaticEpochRelation {
    NoMutation,
    OnlyBitIdenticalUpdates,
    ChangedOrUnknown,
    Unknown,
}

#[derive(Clone, Copy, Debug)]
struct StaticNoopEpochWitness {
    start_epoch: u64,
    last_epoch: u64,
    only_noop_updates: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct InstanceTargetRevision {
    model_id: u32,
    // Zero is permanently invalid after overflow; never wrap into an old fact.
    revision: u64,
    // Last completed off-handle uploader write observed under its exclusive
    // CPU lock. None forces the first observation to invalidate old samples.
    uploader_serial: Option<u64>,
    // Last observed invalidation kind, private diagnostic only. Multiple
    // mutations may coalesce before a getter; this is not a complete history.
    last_mutation: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum DynamicRowRelation {
    Unchanged,
    ChangedOrUnknown,
    Unknown,
}

#[derive(Clone, Copy, Debug)]
struct DynamicImageMutationWitness {
    // Zero is permanently invalid after overflow, so an old stamp cannot alias.
    serial: u64,
}

impl InstanceTable {
    fn arm_dynamic_image_mutation(&mut self) -> Option<u64> {
        let witness = self.dynamic_image_mutation.get_or_insert_with(||
            Box::new(DynamicImageMutationWitness { serial: 1 }));
        (witness.serial != 0).then_some(witness.serial)
    }

    fn dynamic_row_relation(&self, stamp: u64) -> DynamicRowRelation {
        match self.dynamic_image_mutation.as_deref() {
            Some(witness) if stamp != 0 && witness.serial != 0 && witness.serial == stamp =>
                DynamicRowRelation::Unchanged,
            Some(witness) if stamp != 0 && witness.serial != 0 =>
                DynamicRowRelation::ChangedOrUnknown,
            _ => DynamicRowRelation::Unknown,
        }
    }

    fn advance_dynamic_image_mutation(&mut self) {
        if let Some(witness) = &mut self.dynamic_image_mutation {
            if witness.serial != 0 {
                witness.serial = witness.serial.checked_add(1).unwrap_or(0);
            }
        }
    }

    /// Private diagnostic arm. This does not change the renderer's epoch,
    /// dirty uploads, target revision, COUNT facts or cache validity.
    fn arm_static_noop_epoch(&mut self) -> u64 {
        let epoch = self.epoch;
        self.static_noop_epoch = Some(Box::new(StaticNoopEpochWitness {
            start_epoch: epoch, last_epoch: epoch, only_noop_updates: true,
        }));
        epoch
    }

    fn static_noop_epoch_relation(&self, start_epoch: u64) -> StaticEpochRelation {
        let Some(witness) = self.static_noop_epoch.as_deref() else {
            return StaticEpochRelation::Unknown;
        };
        if witness.start_epoch != start_epoch || witness.last_epoch != self.epoch {
            return StaticEpochRelation::Unknown;
        }
        if !witness.only_noop_updates { return StaticEpochRelation::ChangedOrUnknown; }
        if self.epoch == start_epoch { StaticEpochRelation::NoMutation }
        else { StaticEpochRelation::OnlyBitIdenticalUpdates }
    }

    /// Start observing a retained model explicitly. Reselecting a model advances
    /// the serial even when no instance changed, so an older binding cannot alias.
    fn arm_target_revision(&mut self, model_id: u32) -> Option<u64> {
        if model_id == INVALID_MODEL { return None; }
        match &mut self.target_revision {
            Some(witness) if witness.model_id == model_id => {},
            Some(witness) => {
                witness.model_id = model_id;
                witness.uploader_serial = None;
                witness.last_mutation = 8;
                if witness.revision != 0 {
                    witness.revision = witness.revision.checked_add(1).unwrap_or(0);
                }
            }
            None => self.target_revision = Some(Box::new(InstanceTargetRevision {
                model_id, revision: 1, uploader_serial: None, last_mutation: 0,
            })),
        }
        self.target_revision().map(|(_, revision)| revision)
    }

    fn target_revision(&self) -> Option<(u32, u64)> {
        self.target_revision.as_deref().and_then(|witness| (witness.revision != 0)
            .then_some((witness.model_id, witness.revision)))
    }

    fn changed_target_model(&mut self, first: u32, second: u32) {
        if let Some(witness) = &mut self.target_revision {
            if witness.revision != 0 &&
                (witness.model_id == first || witness.model_id == second) {
                witness.revision = witness.revision.checked_add(1).unwrap_or(0);
                witness.last_mutation = 1;
            }
        }
    }

    /// The changed section/mesh may belong to the armed model. Advance its
    /// serial conservatively, so old samples fail but fresh samples can follow.
    /// Only overflow permanently invalidates the witness.
    fn advance_target_revision_for_unattributed_geometry(&mut self) {
        self.advance_target_revision_with_kind(0);
    }

    fn advance_target_revision_with_kind(&mut self, kind: u32) {
        if let Some(witness) = &mut self.target_revision {
            if witness.revision != 0 {
                witness.revision = witness.revision.checked_add(1).unwrap_or(0);
                witness.last_mutation = kind;
            }
        }
    }

    fn palette_upload_changed(&mut self, independent_target: bool, image_serial: Option<u64>) {
        // Fresh target COUNT is independent of a foreign pose only when every
        // current target section was proved rigid AND the separate cached-image
        // witness retained a checked stamp. Refusal preserves the old fallback.
        if !independent_target || image_serial.is_none() {
            self.advance_target_revision_with_kind(6);
        }
    }

    fn observe_uploader_mutation_serial(&mut self, serial: u64) {
        let Some(witness) = &mut self.target_revision else { return; };
        if witness.revision == 0 { return; }
        if serial == 0 || witness.uploader_serial.is_some_and(|old| serial < old) {
            witness.revision = 0; // serial wrapped or retreated: no safe identity
            witness.last_mutation = 7;
        } else if witness.uploader_serial != Some(serial) {
            witness.uploader_serial = Some(serial);
            witness.revision = witness.revision.checked_add(1).unwrap_or(0);
            witness.last_mutation = 7;
        }
    }

    fn handle(slot: u32, generation: u8) -> u32 {
        debug_assert!(slot < 0x00FF_FFFE, "instance slot space exhausted");
        (slot + 1) | ((generation as u32) << 24)
    }

    /// Resolve a handle to its slot. `None` for handle 0, for a slot that was never
    /// allocated, and for a generation that no longer matches — the stale case, which is
    /// counted so `stale_ops()` can prove the rejection happened.
    fn slot_of(&mut self, handle: u32) -> Option<u32> {
        let biased = handle & 0x00FF_FFFF;
        if biased == 0 {
            self.stale_ops += 1;
            return None;
        }
        let slot = biased - 1;
        let generation = (handle >> 24) as u8;
        match self.generations.get(slot as usize) {
            Some(&current) if current == generation => Some(slot),
            _ => {
                self.stale_ops += 1;
                if self.stale_ops <= 8 {
                    eprintln!(
                        "[wgr] stale instance handle {:#010x} (slot {} gen {} vs current {:?}) -- ignored",
                        handle,
                        slot,
                        generation,
                        self.generations.get(slot as usize)
                    );
                }
                None
            }
        }
    }

    fn read_handle(&self, handle: u32) -> InstanceCpuLookup {
        let biased = handle & 0x00FF_FFFF;
        if biased == 0 { return InstanceCpuLookup::Invalid; }
        let slot = biased - 1;
        let Some(&generation) = self.generations.get(slot as usize) else {
            return InstanceCpuLookup::Invalid;
        };
        if generation != (handle >> 24) as u8 {
            return InstanceCpuLookup::Absent;
        }
        match self.slots.get(slot as usize).copied() {
            Some(row) if row.model != INVALID_MODEL => InstanceCpuLookup::Present { slot, row },
            _ => InstanceCpuLookup::Absent,
        }
    }

    // Content invalidation does not mutate handles, slot generations or upload ranges.
    fn invalidate_retained_content(&mut self, origin: StaticEpochOrigin) {
        let previous = self.epoch;
        self.epoch = self.epoch.wrapping_add(1);
        if let Some(witness) = &mut self.static_noop_epoch {
            // An epoch wrap could alias old facts. Fail closed permanently
            // until a new explicit diagnostic arm.
            if previous == u64::MAX || witness.last_epoch != previous {
                witness.only_noop_updates = false;
            }
            witness.last_epoch = self.epoch;
            if origin != StaticEpochOrigin::BitIdenticalUpdate {
                witness.only_noop_updates = false;
            }
        }
    }

    fn mark_dirty(&mut self, slot: u32, origin: StaticEpochOrigin) {
        self.invalidate_retained_content(origin);
        self.dirty = Some(match self.dirty {
            Some((lo, hi)) => (lo.min(slot), hi.max(slot)),
            None => (slot, slot),
        });
    }

    /// Add an instance, recycling a free slot when one is available. Returns its handle.
    /// A recycled slot keeps the generation its removal bumped it to, so the handle returned
    /// here can never equal a handle the caller still holds for the dead occupant.
    fn add(&mut self, inst: InstanceGpu) -> u32 {
        let new_model = inst.model;
        let slot = if let Some(s) = self.free_slots.pop() {
            self.slots[s as usize] = inst;
            s
        } else {
            self.slots.push(inst);
            self.generations.push(0);
            (self.slots.len() - 1) as u32
        };
        self.mark_dirty(slot, StaticEpochOrigin::ChangedOrUnknown);
        self.changed_target_model(new_model, INVALID_MODEL);
        Self::handle(slot, self.generations[slot as usize])
    }

    /// Overwrite an instance in place. A stale handle is refused and counted — it must not
    /// write through to whatever now occupies the slot.
    fn update(&mut self, handle: u32, inst: InstanceGpu) {
        let Some(slot) = self.slot_of(handle) else {
            return;
        };
        if let Some(e) = self.slots.get_mut(slot as usize) {
            let old_model = e.model;
            let new_model = inst.model;
            let origin = if self.static_noop_epoch.is_some() &&
                bytemuck::bytes_of(e) == bytemuck::bytes_of(&inst) {
                StaticEpochOrigin::BitIdenticalUpdate
            } else {
                StaticEpochOrigin::ChangedOrUnknown
            };
            *e = inst;
            self.mark_dirty(slot, origin);
            self.changed_target_model(old_model, new_model);
        }
    }

    /// Remove an instance: blank it for the compute, bump its generation so every outstanding
    /// handle dies with it, and recycle the slot. A stale handle is refused and counted, which
    /// is also what stops a double remove from pushing the same slot onto the free list twice.
    fn remove(&mut self, handle: u32) {
        let Some(slot) = self.slot_of(handle) else {
            return;
        };
        if let Some(e) = self.slots.get_mut(slot as usize) {
            let old_model = e.model;
            e.model = INVALID_MODEL;
            self.generations[slot as usize] = self.generations[slot as usize].wrapping_add(1);
            self.free_slots.push(slot);
            self.mark_dirty(slot, StaticEpochOrigin::ChangedOrUnknown);
            self.changed_target_model(old_model, INVALID_MODEL);
        }
    }

    fn len(&self) -> usize {
        self.slots.len()
    }

    fn as_slice(&self) -> &[InstanceGpu] {
        &self.slots
    }

    fn take_dirty(&mut self) -> Option<(u32, u32)> {
        self.dirty.take()
    }

    /// How many handles this table has refused. The only observable evidence that the
    /// generation check did anything.
    pub(crate) fn stale_ops(&self) -> u64 {
        self.stale_ops
    }
}

/// Compare only the selected model's dynamic rows. Other models may change or
/// reorder without changing this witness. Full-row bit comparison is deliberately
/// conservative: transform changes that do not affect the cull still bump it.
/// Called only while the optional target witness is armed; the normal path has
/// no extra dynamic scan, hashing or allocation.
fn same_target_dynamic_rows(previous: &[InstanceGpu], next: &[InstanceGpu], model_id: u32) -> bool {
    let mut old = previous.iter().filter(|row| row.model == model_id);
    let mut new = next.iter().filter(|row| row.model == model_id);
    loop {
        match (old.next(), new.next()) {
            (None, None) => return true,
            (Some(a), Some(b)) if bytemuck::bytes_of(a) == bytemuck::bytes_of(b) => {},
            _ => return false,
        }
    }
}

fn replace_dynamic_instances(dynamic: &mut Vec<InstanceGpu>, next: &[InstanceGpu],
                             static_instances: &mut InstanceTable) {
    // The full-row comparison is opt-in. A foreign caster can change the
    // published image while leaving the selected model's COUNT unchanged.
    if static_instances.dynamic_image_mutation.as_ref().is_some_and(|w| w.serial != 0) &&
        bytemuck::cast_slice::<_, u8>(dynamic) != bytemuck::cast_slice::<_, u8>(next) {
        static_instances.advance_dynamic_image_mutation();
    }
    if let Some((model_id, _)) = static_instances.target_revision() {
        if !same_target_dynamic_rows(dynamic, next, model_id) {
            static_instances.changed_target_model(model_id, INVALID_MODEL);
        }
    }
    dynamic.clear();
    dynamic.extend_from_slice(next);
}

fn resolved_sections_changed(previous: &[SectionGpu], next: &[SectionGpu],
                             instances: &mut InstanceTable) -> bool {
    if previous == next { return false; }
    // This comparison already belonged to set_sections. Unknown reverse
    // ownership advances only an explicitly armed target witness.
    instances.advance_target_revision_with_kind(2);
    true
}

pub struct CullState {
    // No heap/readback allocation until an enabled explicit request.
    lod_feedback: Option<RefCell<Box<super::lod_feedback::LodFeedback>>>,
    // Instancing-collapse three-pass pipelines (§3.6), all over `layout`: COUNT (1/instance) ->
    // EMIT (1/section) -> SCATTER (1/instance). Shared by the main + every shadow view.
    count_pipeline: wgpu::ComputePipeline,
    emit_pipeline: wgpu::ComputePipeline,
    scatter_pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,

    // Retained tables — CPU mirrors + GPU buffers. Registered at load, rarely changed, so
    // uploaded by dirty domain; unchanged historical arrays are not rewritten.
    models: Vec<ModelGpu>,
    lods: Vec<LodGpu>,
    section_spans: Option<Box<super::section_span_pool::SectionSpanPool>>,
    lod_spans: Option<Box<super::section_span_pool::SectionSpanPool>>,
    sections: Vec<SectionGpu>,
    // Per-section shading, parallel to `sections` (same global index). Draw-side only —
    // not a compute input; bound in the GPU-driven draw pass (3b-2b).
    section_materials: Vec<SectionMaterialGpu>,
    // Per-tree crown centres (MODEL space, .xyz; .w unused) for forest spherical normals
    // (foliage-translucency-plan.md §9 Approach A). A global append-only table; a forest vertex's
    // `conform` word indexes it. Draw-side only (group-1 binding 3 in vs_gpu), not a compute input.
    crown_centres: Vec<[f32; 4]>,
    table_uploads: TableUploads,
    // Fixed optional interval only; no per-model record, row scan or heap allocation.
    model_row_uploads: Option<ModelRowUploads>,

    // Unified instance buffer: the static region (ID-3: an InstanceTable, which owns the
    // slots, the free list and the generations, and needs no device) then the dynamic
    // region re-copied every frame.
    instances: InstanceTable,
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
    // CPU diagnostic selection only; no readback or residency policy consumes it.
    target_model_id: Option<u32>,
    target_model_default: Option<u32>,
    // Private one-shot main COUNT readback. None costs no GPU staging allocation.
    main_count_slots: Option<Vec<(wgpu::Buffer, MainCountSlot)>>,
    main_count_armed: Option<MainCountIdentity>,
    main_count_latest: MainCountFact,
    main_count_probe_reason: u32,
    count_probe_logged_reason: [u32; 5],
    count_probe_events_emitted: u32,
    count_probe_superseded: [Option<MainCountIdentity>; 5],
    main_count_last_token: u64,
    main_dispatch_recorded: Cell<bool>,
    // Each solar cascade shares the explicit main request identity, but never
    // a counter or answer. All staging remains absent until the opt-in request.
    // Index 4 is the GI sun-proxy view, backed by sky cull view 5. It uses the
    // same private request and slot state machine, but never a solar counter.
    shadow_count_slots: [Option<Vec<(wgpu::Buffer, MainCountSlot)>>; 5],
    shadow_count_armed: [Option<MainCountIdentity>; 5],
    shadow_count_latest: [MainCountFact; 5],
    shadow_count_probe_reason: [u32; 5],
    gi_dispatch_recorded: Cell<bool>,
    // Tile k=0 has a distinct identity from solar cascade 0: at night both use
    // shadow cull index zero. Allocate this private readback ring only on an
    // explicit diagnostic request; cached tiles never produce a new COUNT.
    local0_count_slots: Option<Vec<(wgpu::Buffer, MainCountSlot)>>,
    local0_count_armed: Option<MainCountIdentity>,
    local0_count_latest: MainCountFact,
    // The planar mirror submits its own encoder before the main frame. Its
    // diagnostic copy therefore has a separate ring and submit transition.
    reflection_count_slots: Option<Vec<(wgpu::Buffer, MainCountSlot)>>,
    reflection_count_armed: Option<MainCountIdentity>,
    reflection_count_latest: MainCountFact,
    reflection_dispatch_recorded: Cell<bool>,
    // Interior sky zenith is index 0; GI sun proxy is index 5 and has its
    // own request state. A cached zenith layer cannot create a new sample.
    sky0_count_slots: Option<Vec<(wgpu::Buffer, MainCountSlot)>>,
    sky0_count_armed: Option<MainCountIdentity>,
    sky0_count_latest: MainCountFact,
    sky0_dispatch_recorded: Cell<bool>,
    // One bounded staging ring for sky directions 1..4 and local tiles 1..23.
    batch_count: Option<BatchCountRing>,
    batch_sky_dispatch_mask: Cell<u8>,
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

// Diagnostic-only main-view COUNT, never an all-view or eviction certificate.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MainCountIdentity {
    pub token: u64,
    pub model_id: u32,
    pub source_generation: u64,
    pub cull_epoch: u64,
    // Private opt-in retained-target witness. Zero is never a valid sample.
    pub target_revision: u64,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum MainCountStatus { #[default] Unknown, Present, Absent }
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MainCountFact {
    pub identity: MainCountIdentity,
    pub status: MainCountStatus,
    pub count: u32,
}
// Private probe-only reason. This never changes COUNT classification.
pub const COUNT_PROBE_NO_COPY: u32 = 0;
pub const COUNT_PROBE_WAITING_MAP: u32 = 1;
pub const COUNT_PROBE_MAP_FAILED: u32 = 2;
pub const COUNT_PROBE_EPOCH_CHANGED: u32 = 3;
pub const COUNT_PROBE_TARGET_CHANGED: u32 = 4;
pub const COUNT_PROBE_READY: u32 = 5;
pub const COUNT_PROBE_SUPERSEDED: u32 = 6;
fn classify_count_probe_reason(identity: MainCountIdentity, reason: u32,
                               epoch: u64, revision: Option<(u32, u64)>) -> u32 {
    if identity.token == 0 { return COUNT_PROBE_NO_COPY; }
    if epoch != identity.cull_epoch { return COUNT_PROBE_EPOCH_CHANGED; }
    if revision != Some((identity.model_id, identity.target_revision)) ||
        identity.target_revision == 0 { return COUNT_PROBE_TARGET_CHANGED; }
    reason
}
#[cfg(test)]
mod count_probe_reason_tests {
    use super::*;

    #[test]
    fn reason_distinguishes_unmapped_failed_stale_and_ready_without_changing_facts() {
        let id = MainCountIdentity { token: 7, model_id: 3,
            source_generation: 4, cull_epoch: 9, target_revision: 12 };
        let current = Some((3, 12));
        assert_eq!(classify_count_probe_reason(MainCountIdentity::default(),
            COUNT_PROBE_WAITING_MAP, 9, current), COUNT_PROBE_NO_COPY);
        assert_eq!(classify_count_probe_reason(id, COUNT_PROBE_WAITING_MAP, 9, current),
            COUNT_PROBE_WAITING_MAP);
        assert_eq!(classify_count_probe_reason(id, COUNT_PROBE_MAP_FAILED, 9, current),
            COUNT_PROBE_MAP_FAILED);
        assert_eq!(classify_count_probe_reason(id, COUNT_PROBE_WAITING_MAP, 10, current),
            COUNT_PROBE_EPOCH_CHANGED);
        assert_eq!(classify_count_probe_reason(id, COUNT_PROBE_READY, 9, Some((3, 13))),
            COUNT_PROBE_TARGET_CHANGED);
        assert_eq!(classify_count_probe_reason(id, COUNT_PROBE_READY, 9, current),
            COUNT_PROBE_READY);
    }
}
enum MainCountSlot {
    Idle,
    Encoded(MainCountIdentity),
    Submitted { identity: MainCountIdentity, committed: bool, aborted: bool },
    Mapping { identity: MainCountIdentity, committed: bool, aborted: bool,
        done: mpsc::Receiver<Result<(), wgpu::BufferAsyncError>> },
}
impl MainCountSlot {
    // Only a committed, non-aborted copy for this exact request can still
    // resolve into a fact. Encoded and older in-flight slots are not pending
    // evidence for the current diagnostic snapshot.
    fn pending_for(&self, request: MainCountIdentity) -> bool {
        matches!(self,
            Self::Submitted { identity, committed: true, aborted: false } |
            Self::Mapping { identity, committed: true, aborted: false, .. }
            if *identity == request)
    }
}

// The remaining 27 required views share three 108-byte frame slots. A slot is
// never reused while its submitted copy can still be in flight.
const BATCH_COUNT_VIEWS: usize = 27; // sky 1..4, then local tiles 1..23
const BATCH_SKY_VIEWS: usize = 4;
enum BatchCountSlot {
    Idle,
    Encoded { identity: MainCountIdentity, copied: u32 },
    Submitted { identity: MainCountIdentity, copied: u32, committed: bool, aborted: bool },
    Mapping { identity: MainCountIdentity, copied: u32, committed: bool, aborted: bool,
        done: mpsc::Receiver<Result<(), wgpu::BufferAsyncError>> },
}
impl BatchCountSlot {
    fn pending_copied_for(&self, request: MainCountIdentity) -> u32 {
        match self {
            Self::Submitted { identity, copied, committed: true, aborted: false } |
            Self::Mapping { identity, copied, committed: true, aborted: false, .. }
                if *identity == request => *copied,
            _ => 0,
        }
    }
}
struct BatchCountRing {
    slots: Vec<(wgpu::Buffer, BatchCountSlot)>,
    armed: Option<MainCountIdentity>,
    latest: [MainCountFact; BATCH_COUNT_VIEWS],
}
impl BatchCountRing {
    fn pending_mask(&self, request: MainCountIdentity) -> u32 {
        self.slots.iter().fold(0, |mask, (_, state)|
            mask | state.pending_copied_for(request))
    }
    fn new(device: &wgpu::Device) -> Self {
        let slots = (0..3).map(|i| (device.create_buffer(&wgpu::BufferDescriptor {
            label: Some(&format!("wgr_view_target_count_batch_{i}")),
            size: (BATCH_COUNT_VIEWS * 4) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        }), BatchCountSlot::Idle)).collect();
        Self { slots, armed: None, latest: [MainCountFact::default(); BATCH_COUNT_VIEWS] }
    }
    fn arm(&mut self, identity: MainCountIdentity) {
        self.armed = Some(identity);
        self.latest = [MainCountFact { identity, ..MainCountFact::default() }; BATCH_COUNT_VIEWS];
    }
    fn before_submit(&mut self, token: u64) -> bool {
        for (_, state) in &mut self.slots {
            if let BatchCountSlot::Encoded { identity, copied } = state {
                if identity.token == token {
                    *state = BatchCountSlot::Submitted { identity: *identity, copied: *copied,
                        committed: false, aborted: true };
                    return true;
                }
            }
        }
        false
    }
    fn submitted(&mut self, token: u64) -> bool {
        for (_, state) in &mut self.slots {
            if let BatchCountSlot::Submitted { identity, aborted, .. } = state {
                if identity.token == token { *aborted = false; return true; }
            }
        }
        false
    }
    fn commit(&mut self, token: u64) -> bool {
        for (_, state) in &mut self.slots {
            if let BatchCountSlot::Submitted { identity, committed, aborted, .. } = state {
                if identity.token == token && !*aborted { *committed = true; return true; }
            }
        }
        false
    }
    fn abort(&mut self, token: u64) {
        if self.armed.is_some_and(|id| id.token == token) { self.armed = None; }
        for fact in &mut self.latest {
            if fact.identity.token == token { fact.status = MainCountStatus::Unknown; fact.count = 0; }
        }
        for (_, state) in &mut self.slots {
            match state {
                BatchCountSlot::Encoded { identity, .. } if identity.token == token =>
                    *state = BatchCountSlot::Idle,
                BatchCountSlot::Submitted { identity, aborted, .. } |
                BatchCountSlot::Mapping { identity, aborted, .. } if identity.token == token =>
                    *aborted = true,
                _ => {}
            }
        }
    }
    fn harvest(&mut self, device: &wgpu::Device, epoch: u64,
               target_revision: Option<(u32, u64)>) {
        for (buf, state) in &mut self.slots {
            if let BatchCountSlot::Submitted { identity, copied, committed, aborted } = state {
                if !*committed && !*aborted { continue; }
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
                *state = BatchCountSlot::Mapping { identity: *identity, copied: *copied,
                    committed: *committed, aborted: *aborted, done: rx };
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in &mut self.slots {
            let current = std::mem::replace(state, BatchCountSlot::Idle);
            match current {
                BatchCountSlot::Mapping { identity, copied, committed, aborted, done } => {
                    match done.try_recv() {
                        Ok(Ok(())) => {
                            let data = buf.slice(..).get_mapped_range();
                            if committed && !aborted && epoch == identity.cull_epoch &&
                                target_revision == Some((identity.model_id, identity.target_revision)) {
                                for (i, fact) in self.latest.iter_mut().enumerate() {
                                    if copied & (1 << i) != 0 && fact.identity == identity {
                                        let count = u32::from_le_bytes(data[i*4..i*4+4].try_into().unwrap());
                                        fact.count = count;
                                        fact.status = if count == 0 { MainCountStatus::Absent }
                                            else { MainCountStatus::Present };
                                    }
                                }
                            }
                            drop(data);
                            buf.unmap();
                        }
                        Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {},
                        Err(mpsc::TryRecvError::Empty) => {
                            *state = BatchCountSlot::Mapping { identity, copied, committed, aborted, done };
                        }
                    }
                }
                other => *state = other,
            }
        }
    }
    fn fact(&self, index: usize, epoch: u64) -> MainCountFact {
        let Some(&mut_fact) = self.latest.get(index) else { return MainCountFact::default(); };
        let mut fact = mut_fact;
        if epoch != fact.identity.cull_epoch { fact.status = MainCountStatus::Unknown; fact.count = 0; }
        fact
    }
}

// Dirty domains for immutable-index retained tables. One byte replaces the old bool;
// the production upload closure below also provides a device-free write-selection test seam.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TableDomain { Model, Lod, Section, Material, Crown }
impl TableDomain {
    const ALL: [Self; 5] = [Self::Model, Self::Lod, Self::Section, Self::Material, Self::Crown];
    fn bit(self) -> u8 { 1 << self as u8 }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct TableUploads(u8);
impl TableUploads {
    const NONE: Self = Self(0);
    const ALL: Self = Self(31);
    fn mark(&mut self, domain: TableDomain) { self.0 |= domain.bit(); }
    // Clear only after every selected upload returns. A panic leaves caller state dirty.
    // Every buffer-growth result participates in the shared-view bindgroup invalidation.
    fn run(self, mut upload: impl FnMut(TableDomain) -> bool) -> (Self, bool) {
        if self == Self::NONE { return (self, false); }
        let mut grew = false;
        for domain in TableDomain::ALL {
            if self.0 & domain.bit() != 0 { grew |= upload(domain); }
        }
        (Self::NONE, grew)
    }
}
const _: () = assert!(std::mem::size_of::<TableUploads>() == std::mem::size_of::<bool>());
// Optional min/max dirty interval for append-only MODEL births. A distant pair of writes
// includes the unchanged gap; this is bounded metadata, not a sparse ownership cache.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ModelRowUploads {
    lo: usize, hi: usize,
    activation_pending: bool, committed_rows: u8,
    // One fixed numeric event, drained by Gfx3d immediately after prepare. No queued heap.
    notice: Option<(usize, usize, usize, u8)>,
}
impl Default for ModelRowUploads {
    fn default() -> Self { Self { lo: usize::MAX, hi: 0, activation_pending: true, committed_rows: 0, notice: None } }
}
impl ModelRowUploads {
    fn mark(&mut self, index: usize) {
        self.lo = self.lo.min(index);
        self.hi = self.hi.max(index.checked_add(1).unwrap_or(usize::MAX));
    }
    // Missing/grown backing storage ALWAYS receives the complete CPU mirror. Unknown
    // MODEL dirtiness (no captured row) also falls back to full; never an empty stale write.
    fn reset_interval(&mut self) { self.lo = usize::MAX; self.hi = 0; }
    fn note_committed(&mut self, first: usize, count: usize, table_rows: usize) {
        if count == 0 || count >= table_rows || self.committed_rows > 16 { return; }
        self.committed_rows += 1;
        self.notice = Some((first, count, table_rows, self.committed_rows));
    }
    fn take_activation(&mut self) -> Option<String> {
        if !std::mem::take(&mut self.activation_pending) { return None; }
        Some(format!("gpu MODEL row upload enabled: interval=1 knownStateBytes={} commitLogLimit=16; source row writes only, not RSS/performance proof",
            std::mem::size_of::<Option<Self>>()))
    }
    fn take_commit(&mut self) -> Option<String> {
        let (first, count, table_rows, event) = self.notice.take()?;
        if event > 16 { return Some("gpu MODEL row upload log truncated: limit=16; partial writes continue unlogged".into()); }
        Some(format!("gpu MODEL row upload committed: first={} rows={} tableRows={} bytes={} event={}; postQueueWrite=true sourceRangeOnly=true",
            first, count, table_rows, count * std::mem::size_of::<ModelGpu>(), event))
    }
    fn range(self, len: usize, grew: bool) -> std::ops::Range<usize> {
        if grew || self.lo >= self.hi || self.hi > len { 0..len }
        else { self.lo..self.hi }
    }
    fn run(self, len: usize, grew: bool, mut write: impl FnMut(std::ops::Range<usize>)) {
        let range = self.range(len, grew);
        if !range.is_empty() { write(range); }
    }
}
const _: () = assert!(std::mem::size_of::<Option<ModelRowUploads>>() <= 80);

// Optional feedback changes only the model tag; use the same mutation in tests.
fn set_model_feedback_tag(models: &mut [ModelGpu], uploads: &mut TableUploads, id: u32, tag: u32) -> bool {
    if let Some(model) = models.get_mut(id as usize) {
        if model._pad != tag {
            model._pad = tag;
            uploads.mark(TableDomain::Model);
            return true;
        }
    }
    false
}

// Same owner-side retirement transition used by CullState and device-free tests.
// Reuse the already-required LOD walk; no instance scan or new epoch storage.
fn retire_model_content(models: &[ModelGpu], lods: &mut [LodGpu], id: u32,
                        instances: &mut InstanceTable, uploads: &mut TableUploads) -> bool {
    if !retire_model_lods(models, lods, id) { return false; }
    instances.invalidate_retained_content(StaticEpochOrigin::ChangedOrUnknown);
    instances.changed_target_model(id, INVALID_MODEL);
    uploads.mark(TableDomain::Lod);
    true
}

// Optional retirement: index0 is an immutable empty sentinel. Clear bounded old rows,
// then redirect the immutable model birth before the allocator can expose any old LOD index.
fn retire_model_to_sentinel(models: &mut [ModelGpu], lods: &mut [LodGpu], id: u32,
    birth_span: Option<super::section_span_pool::Span>, instances: &mut InstanceTable, uploads: &mut TableUploads,
) -> (bool, Option<super::section_span_pool::Span>) {
    let Some(model) = models.get_mut(id as usize) else { return (false, None); };
    if model.lod_base == 0 && model.lod_count == 0 { return (false, None); }
    let span = super::section_span_pool::Span { base: model.lod_base as usize, count: model.lod_count as usize };
    // Do not clear/release a current range unless it is the immutable captured birth range.
    let valid = birth_span == Some(span) && span.base != 0 && span.count <= 4096 && span.base.checked_add(span.count)
        .is_some_and(|end| end <= lods.len());
    if valid { for lod in &mut lods[span.base..span.base + span.count] { lod.section_count = 0; } }
    model.lod_base = 0; model.lod_count = 0; model._pad = 0;
    instances.invalidate_retained_content(StaticEpochOrigin::ChangedOrUnknown);
    instances.changed_target_model(id, INVALID_MODEL);
    uploads.mark(TableDomain::Model); uploads.mark(TableDomain::Lod);
    (true, valid.then_some(span))
}

const STATS_RING_SLOTS: usize = 3;

impl CullState {
    pub(super) fn take_model_row_activation(&mut self) -> Option<String> {
        self.model_row_uploads.as_mut().and_then(ModelRowUploads::take_activation)
    }
    pub(super) fn take_model_row_commit_notice(&mut self) -> Option<String> {
        self.model_row_uploads.as_mut().and_then(ModelRowUploads::take_commit)
    }
    fn set_feedback_tag(&mut self, id: u32, tag: u32) {
        if set_model_feedback_tag(&mut self.models, &mut self.table_uploads, id, tag) {
            if let Some(rows) = &mut self.model_row_uploads { rows.mark(id as usize); }
        }
    }
    pub fn start_lod_demand(&mut self, device: &wgpu::Device, epoch: u64, ids: &[u32], frames: u32) -> u32 {
        if !super::lod_feedback::enabled() { return 0; }
        let previous = self.lod_feedback.as_ref().map(|state| { let state = state.borrow(); (state.report.epoch, state.report.status) });
        let valid = super::lod_feedback::validate_request(epoch, ids, frames, previous);
        if valid != 1 { return valid; }
        let rows: Vec<_> = ids.iter().map(|&id| {
            let mut row = crate::ffi::WgrLodDemandRow { model_id: id, state: 1, ..Default::default() };
            if let Some(lods) = self.allocation_lods(id) {
                row.lod_count = lods.len() as u32;
                row.state = if lods.is_empty() || lods.iter().all(|l| l.section_count == 0) { 2 }
                    else if lods.len() > 32 { 3 } else { 0 };
            }
            row
        }).collect();
        self.clear_lod_demand_targets();
        for (i, row) in rows.iter().enumerate() {
            if row.state == 0 { self.set_feedback_tag(row.model_id, i as u32 + 1); }
        }
        let state = self.lod_feedback.get_or_insert_with(|| RefCell::new(Box::new(super::lod_feedback::LodFeedback::new(device))));
        state.borrow_mut().start(epoch, frames, &rows);
        1
    }
    fn clear_lod_demand_targets(&mut self) {
        if let Some(state) = &self.lod_feedback {
            let state = state.borrow();
            for row in &state.report.rows[..state.report.row_count as usize] {
                if set_model_feedback_tag(&mut self.models, &mut self.table_uploads, row.model_id, 0) {
                    if let Some(rows) = &mut self.model_row_uploads { rows.mark(row.model_id as usize); }
                }
            }
        }
    }
    pub fn cancel_lod_demand(&mut self, epoch: u64) {
        if self.lod_feedback.as_ref().is_some_and(|s| s.borrow().report.epoch == epoch) {
            self.clear_lod_demand_targets(); self.lod_feedback.as_ref().unwrap().borrow_mut().cancel(epoch);
        }
    }
    pub fn begin_lod_demand(&mut self) {
        let Some(state) = &self.lod_feedback else { return; };
        let done = { let s = state.borrow(); s.report.frames_attempted >= s.report.frames_requested || s.report.status != 1 };
        if done { self.clear_lod_demand_targets(); }
        self.lod_feedback.as_ref().unwrap().borrow_mut().begin();
    }
    pub fn resolve_lod_demand(&self) { if let Some(s) = &self.lod_feedback { s.borrow_mut().resolve(); } }
    pub fn harvest_lod_demand(&self, device: &wgpu::Device) { if let Some(s) = &self.lod_feedback { s.borrow_mut().harvest(device); } }
    pub fn lod_demand_report(&self, epoch: u64) -> Option<crate::ffi::WgrLodDemandReport> {
        self.lod_feedback.as_ref().and_then(|s| { let s = s.borrow(); (s.report.epoch == epoch).then_some(s.report) })
    }
    pub(super) fn allocation_lods(&self, model: u32) -> Option<&[LodGpu]> {
        let model = self.models.get(model as usize)?;
        let start = model.lod_base as usize;
        self.lods.get(start..start.checked_add(model.lod_count as usize)?)
    }
    pub(super) fn registered_reference_tables(&self) -> (&[ModelGpu], &[LodGpu]) {
        (&self.models, &self.lods)
    }
    // Byte size of one CullParamsGpu (the uniform is fixed-size).
    const PARAMS_SIZE: u64 = std::mem::size_of::<CullParamsGpu>() as u64;

    pub fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_cull"),
            source: wgpu::ShaderSource::Wgsl(super::lod_feedback::shader_source(
                include_str!("cull.wgsl"), super::lod_feedback::enabled()).expect("checked LOD feedback shader source")),
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
            size: counter_words() * 4,
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
            size: counter_words() * 4,
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

        let lod_reuse = std::env::var("WGR_CULL_LOD_REUSE").ok().as_deref() == Some("1");
        let target_model_default = std::env::var("WGR_GPU_VIEW_TARGET_MODEL_ID")
            .ok().and_then(|s| s.parse::<u32>().ok())
            .filter(|&id| id != INVALID_MODEL);
        Self {
            count_pipeline,
            emit_pipeline,
            scatter_pipeline,
            layout,
            models: Vec::new(),
            // Index0 is permanent and never owned/free-listed; count0 still dereferences it.
            lods: if lod_reuse { vec![LodGpu::zeroed()] } else { Vec::new() },
            lod_spans: lod_reuse.then(|| Box::new(super::section_span_pool::SectionSpanPool::new_table("LOD"))),
            section_spans: (std::env::var("WGR_CULL_SECTION_REUSE").ok().as_deref() == Some("1"))
                .then(|| Box::new(super::section_span_pool::SectionSpanPool::new())),
            sections: Vec::new(),
            section_materials: Vec::new(),
            crown_centres: Vec::new(),
            table_uploads: TableUploads::NONE,
            model_row_uploads: (std::env::var("WGR_CULL_MODEL_ROW_UPLOAD").ok().as_deref() == Some("1"))
                .then(ModelRowUploads::default),
            instances: InstanceTable::default(),
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
            target_model_id: target_model_default,
            target_model_default,
            main_count_slots: None,
            main_count_armed: None,
            main_count_latest: MainCountFact::default(),
            main_count_probe_reason: COUNT_PROBE_NO_COPY,
            count_probe_logged_reason: [u32::MAX; 5],
            count_probe_events_emitted: 0,
            count_probe_superseded: [None; 5],
            main_count_last_token: 0,
            main_dispatch_recorded: Cell::new(false),
            shadow_count_slots: std::array::from_fn(|_| None),
            shadow_count_armed: [None; 5],
            shadow_count_latest: [MainCountFact::default(); 5],
            shadow_count_probe_reason: [COUNT_PROBE_NO_COPY; 5],
            gi_dispatch_recorded: Cell::new(false),
            local0_count_slots: None,
            local0_count_armed: None,
            local0_count_latest: MainCountFact::default(),
            reflection_count_slots: None,
            reflection_count_armed: None,
            reflection_count_latest: MainCountFact::default(),
            reflection_dispatch_recorded: Cell::new(false),
            sky0_count_slots: None,
            sky0_count_armed: None,
            sky0_count_latest: MainCountFact::default(),
            sky0_dispatch_recorded: Cell::new(false),
            batch_count: None,
            batch_sky_dispatch_mask: Cell::new(0),
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
            lod_feedback: None,
        }
    }

    /// Arm one private main-view diagnostic. The caller must supply the exact source
    /// generation; this module cannot derive it from a model ID. No production owner or
    /// residency path calls this API. Invalid input clears any previous answer.
    pub fn arm_main_target_count(&mut self, device: &wgpu::Device, token: u64,
                                 model_id: u32, source_generation: u64) -> bool {
        // An earlier failed render may have submitted an aborted cascade copy
        // without leaving a C++ pending getter. Reclaim it on the next explicit
        // request; the ordinary frame never polls this private ring.
        for cascade in 0..5 {
            if self.shadow_count_slots[cascade].is_some() {
                self.harvest_shadow_target_count(device, cascade);
            }
        }
        if self.local0_count_slots.is_some() {
            self.harvest_local0_target_count(device);
        }
        if self.reflection_count_slots.is_some() {
            self.harvest_reflection_target_count(device);
        }
        if self.sky0_count_slots.is_some() {
            self.harvest_sky0_target_count(device);
        }
        if self.batch_count.is_some() { self.harvest_batch_target_count(device); }
        if self.main_count_latest.identity.token != 0 &&
            self.main_count_probe_reason() != COUNT_PROBE_READY {
            self.count_probe_superseded[0] = Some(self.main_count_latest.identity);
        }
        for cascade in 0..4 {
            if self.shadow_count_latest[cascade].identity.token != 0 &&
                self.shadow_count_probe_reason(cascade) != COUNT_PROBE_READY {
                self.count_probe_superseded[cascade + 1] =
                    Some(self.shadow_count_latest[cascade].identity);
            }
        }
        self.target_model_id = self.target_model_default;
        self.main_count_armed = None;
        self.main_count_latest = MainCountFact::default();
        self.main_count_probe_reason = COUNT_PROBE_NO_COPY;
        self.count_probe_logged_reason = [u32::MAX; 5];
        self.main_dispatch_recorded.set(false);
        self.shadow_count_armed = [None; 5];
        self.shadow_count_latest = [MainCountFact::default(); 5];
        self.shadow_count_probe_reason = [COUNT_PROBE_NO_COPY; 5];
        self.local0_count_armed = None;
        self.local0_count_latest = MainCountFact::default();
        self.reflection_count_armed = None;
        self.reflection_count_latest = MainCountFact::default();
        self.reflection_dispatch_recorded.set(false);
        self.sky0_count_armed = None;
        self.sky0_count_latest = MainCountFact::default();
        self.sky0_dispatch_recorded.set(false);
        self.batch_sky_dispatch_mask.set(0);
        if let Some(batch) = &mut self.batch_count { batch.armed = None; batch.latest = [MainCountFact::default(); BATCH_COUNT_VIEWS]; }
        self.gi_dispatch_recorded.set(false);
        let epoch = self.instance_epoch();
        if token == 0 || token == u64::MAX || token <= self.main_count_last_token ||
            model_id == INVALID_MODEL || model_id as usize >= self.models.len() ||
            source_generation == 0 || epoch == 0 || epoch == u64::MAX {
            return false;
        }
        let Some(target_revision) = self.arm_retained_target_revision(model_id) else { return false; };
        let identity = MainCountIdentity { token, model_id, source_generation,
            cull_epoch: epoch, target_revision };
        if self.main_count_slots.is_none() {
            self.main_count_slots = Some((0..3).map(|i| {
                (device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(&format!("wgr_main_target_count_{i}")),
                    size: 4,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }), MainCountSlot::Idle)
            }).collect());
        }
        self.target_model_id = Some(model_id);
        self.main_count_last_token = token;
        self.main_count_armed = Some(identity);
        self.main_count_latest.identity = identity;
        for cascade in 0..5 {
            if self.shadow_count_slots[cascade].is_none() {
                self.shadow_count_slots[cascade] = Some((0..3).map(|i| {
                    (device.create_buffer(&wgpu::BufferDescriptor {
                        label: Some(&if cascade == 4 {
                            format!("wgr_gi_target_count_{i}")
                        } else {
                            format!("wgr_shadow{cascade}_target_count_{i}")
                        }),
                        size: 4,
                        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                        mapped_at_creation: false,
                    }), MainCountSlot::Idle)
                }).collect());
            }
            self.shadow_count_armed[cascade] = Some(identity);
            self.shadow_count_latest[cascade].identity = identity;
        }
        if self.local0_count_slots.is_none() {
            self.local0_count_slots = Some((0..3).map(|i| {
                (device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(&format!("wgr_local_shadow0_target_count_{i}")),
                    size: 4,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }), MainCountSlot::Idle)
            }).collect());
        }
        self.local0_count_armed = Some(identity);
        self.local0_count_latest.identity = identity;
        if self.reflection_count_slots.is_none() {
            self.reflection_count_slots = Some((0..3).map(|i| {
                (device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(&format!("wgr_reflection_target_count_{i}")),
                    size: 4,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }), MainCountSlot::Idle)
            }).collect());
        }
        self.reflection_count_armed = Some(identity);
        self.reflection_count_latest.identity = identity;
        if self.sky0_count_slots.is_none() {
            self.sky0_count_slots = Some((0..3).map(|i| {
                (device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(&format!("wgr_sky0_target_count_{i}")),
                    size: 4,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }), MainCountSlot::Idle)
            }).collect());
        }
        self.sky0_count_armed = Some(identity);
        self.sky0_count_latest.identity = identity;
        self.batch_count.get_or_insert_with(|| BatchCountRing::new(device)).arm(identity);
        true
    }

    pub fn shadow_count_armed(&self, cascade: usize) -> bool {
        self.shadow_count_armed.get(cascade).is_some_and(Option::is_some)
    }

    pub fn local0_count_armed(&self) -> bool { self.local0_count_armed.is_some() }
    pub fn batch_target_count_armed(&self) -> bool {
        self.batch_count.as_ref().is_some_and(|b| b.armed.is_some())
    }

    /// Copy the mirror's selected-model COUNT into its own encoder after a
    /// real cull and a closed indirect object pass.
    pub fn copy_reflection_target_count(&mut self, encoder: &mut wgpu::CommandEncoder) -> bool {
        let Some(identity) = self.reflection_count_armed else { return false; };
        if !self.reflection_dispatch_recorded.get() ||
            self.instance_epoch() != identity.cull_epoch ||
            !self.target_revision_matches(identity) { return false; }
        let Some(view) = self.reflection_view.as_ref() else { return false; };
        if view.params.target_count_enabled == 0 ||
            view.params.target_model_id != identity.model_id { return false; }
        let Some(slots) = self.reflection_count_slots.as_mut() else { return false; };
        let Some((dst, state)) = slots.iter_mut()
            .find(|(_, state)| matches!(state, MainCountSlot::Idle)) else { return false; };
        encoder.copy_buffer_to_buffer(&view.counter_buf,
            TARGET_MODEL_COUNT_BYTE_OFFSET, dst, 0, 4);
        *state = MainCountSlot::Encoded(identity);
        self.reflection_count_armed = None;
        self.reflection_dispatch_recorded.set(false);
        true
    }

    /// The caller must additionally prove a fresh zenith layer and a closed
    /// indirect depth pass. This only checks the exact cull/counter owner.
    pub fn copy_sky0_target_count(&mut self, encoder: &mut wgpu::CommandEncoder) -> bool {
        let Some(identity) = self.sky0_count_armed else { return false; };
        if !self.sky0_dispatch_recorded.get() ||
            self.instance_epoch() != identity.cull_epoch ||
            !self.target_revision_matches(identity) { return false; }
        let Some(view) = self.sky_views.first() else { return false; };
        if view.params.target_count_enabled == 0 ||
            view.params.target_model_id != identity.model_id { return false; }
        let Some(slots) = self.sky0_count_slots.as_mut() else { return false; };
        let Some((dst, state)) = slots.iter_mut()
            .find(|(_, state)| matches!(state, MainCountSlot::Idle)) else { return false; };
        encoder.copy_buffer_to_buffer(&view.counter_buf,
            TARGET_MODEL_COUNT_BYTE_OFFSET, dst, 0, 4);
        *state = MainCountSlot::Encoded(identity);
        self.sky0_count_armed = None;
        self.sky0_dispatch_recorded.set(false);
        true
    }

    /// Copy only freshly culled, closed indirect views into one 27-word slot.
    /// `sky_closed` uses bits 0..3 for directions 1..4; `local_closed`
    /// uses tile-index bits. The owner supplies the actual cull-index dispatch mask.
    pub fn copy_batch_target_count(&mut self, encoder: &mut wgpu::CommandEncoder,
                                   sky_closed: u8, local_closed: u32,
                                   solar_count: usize, local_culled: u32) -> bool {
        let epoch = self.instance_epoch();
        let target_revision = self.instances.target_revision();
        let Some(batch) = self.batch_count.as_mut() else { return false; };
        let Some(identity) = batch.armed else { return false; };
        if epoch != identity.cull_epoch ||
            target_revision != Some((identity.model_id, identity.target_revision)) { return false; }
        let Some((dst, state)) = batch.slots.iter_mut()
            .find(|(_, state)| matches!(state, BatchCountSlot::Idle)) else { return false; };
        let mut copied = 0u32;
        let sky_ready = sky_closed & self.batch_sky_dispatch_mask.get();
        for i in 0..BATCH_SKY_VIEWS {
            if sky_ready & (1 << i) == 0 { continue; }
            let Some(view) = self.sky_views.get(i + 1) else { continue; };
            if view.params.target_count_enabled == 0 || view.params.target_model_id != identity.model_id { continue; }
            encoder.copy_buffer_to_buffer(&view.counter_buf, TARGET_MODEL_COUNT_BYTE_OFFSET,
                dst, (i * 4) as u64, 4);
            copied |= 1 << i;
        }
        for k in 1..=23usize {
            let c = solar_count + k;
            if c >= u32::BITS as usize || local_closed & (1 << k) == 0 ||
                local_culled & (1 << c) == 0 { continue; }
            let Some(view) = self.shadow_views.get(c) else { continue; };
            if view.params.target_count_enabled == 0 || view.params.target_model_id != identity.model_id { continue; }
            let i = BATCH_SKY_VIEWS + k - 1;
            encoder.copy_buffer_to_buffer(&view.counter_buf, TARGET_MODEL_COUNT_BYTE_OFFSET,
                dst, (i * 4) as u64, 4);
            copied |= 1 << i;
        }
        if copied == 0 { return false; }
        *state = BatchCountSlot::Encoded { identity, copied };
        batch.armed = None;
        self.batch_sky_dispatch_mask.set(0);
        true
    }

    pub fn batch_target_count_before_submit(&mut self, token: u64) -> bool {
        self.batch_count.as_mut().is_some_and(|b| b.before_submit(token))
    }
    pub fn batch_target_count_submitted(&mut self, token: u64) -> bool {
        self.batch_count.as_mut().is_some_and(|b| b.submitted(token))
    }
    pub fn batch_target_count_commit(&mut self, token: u64) -> bool {
        self.batch_count.as_mut().is_some_and(|b| b.commit(token))
    }
    /// Sky words actually copied by the accepted, committed batch. A direction
    /// that only has an armed identity must never bind to a published image.
    pub fn batch_target_count_committed_sky_mask(&self, token: u64) -> u8 {
        self.batch_count.as_ref().map_or(0, |batch| batch.slots.iter()
            .find_map(|(_, state)| match state {
                BatchCountSlot::Submitted { identity, copied, committed: true, aborted: false }
                    if identity.token == token => Some(*copied as u8 & 0x0f),
                _ => None,
            }).unwrap_or(0))
    }
    /// Tile-index bits 1..23 whose private batch words were copied and committed.
    pub fn batch_target_count_committed_local_mask(&self, token: u64) -> u32 {
        self.batch_count.as_ref().map_or(0, |batch| batch.slots.iter()
            .find_map(|(_, state)| match state {
                BatchCountSlot::Submitted { identity, copied, committed: true, aborted: false }
                    if identity.token == token => Some((*copied >> BATCH_SKY_VIEWS) << 1),
                _ => None,
            }).unwrap_or(0))
    }
    pub fn batch_target_count_abort(&mut self, token: u64) {
        if let Some(b) = &mut self.batch_count { b.abort(token); }
        self.batch_sky_dispatch_mask.set(0);
    }
    pub fn harvest_batch_target_count(&mut self, device: &wgpu::Device) {
        let epoch = self.instance_epoch();
        let revision = self.instances.target_revision();
        if let Some(b) = &mut self.batch_count { b.harvest(device, epoch, revision); }
    }
    pub fn batch_target_count_fact(&self, index: usize) -> MainCountFact {
        self.validate_target_fact(self.batch_count.as_ref().map_or(MainCountFact::default(),
            |b| b.fact(index, self.instance_epoch())))
    }

    /// Exact current-request COUNT copies still awaiting their map result.
    /// This observes only existing opt-in rings; it never queues or polls GPU
    /// work. Bits follow the private 36-view diagnostic layout.
    pub fn pending_target_count_mask(&self, request: MainCountIdentity) -> u64 {
        fn ring_pending(ring: &Option<Vec<(wgpu::Buffer, MainCountSlot)>>,
                        request: MainCountIdentity) -> bool {
            ring.as_ref().is_some_and(|slots|
                slots.iter().any(|(_, state)| state.pending_for(request)))
        }
        if request.token == 0 || request.cull_epoch != self.instance_epoch() ||
            !self.target_revision_matches(request) {
            return 0;
        }
        let mut mask = 0u64;
        if ring_pending(&self.main_count_slots, request) { mask |= 1; }
        for i in 0..4 {
            if ring_pending(&self.shadow_count_slots[i], request) { mask |= 1u64 << (1 + i); }
        }
        if ring_pending(&self.local0_count_slots, request) { mask |= 1u64 << 5; }
        if ring_pending(&self.sky0_count_slots, request) { mask |= 1u64 << 29; }
        if ring_pending(&self.shadow_count_slots[4], request) { mask |= 1u64 << 34; }
        if ring_pending(&self.reflection_count_slots, request) { mask |= 1u64 << 35; }
        let batch = self.batch_count.as_ref().map_or(0, |ring| ring.pending_mask(request));
        for direction in 1..5 {
            if batch & (1 << (direction - 1)) != 0 { mask |= 1u64 << (29 + direction); }
        }
        for tile in 1..24 {
            if batch & (1 << (4 + tile - 1)) != 0 { mask |= 1u64 << (5 + tile); }
        }
        mask
    }

    /// `view_index` is the actual shadow cull index, solar_count + local tile 0.
    /// The owner alone can prove a fresh tile draw and a closed pass.
    pub fn copy_local0_target_count(&mut self, encoder: &mut wgpu::CommandEncoder,
                                    view_index: usize) -> bool {
        let Some(identity) = self.local0_count_armed else { return false; };
        let Some(view) = self.shadow_views.get(view_index) else { return false; };
        if self.instance_epoch() != identity.cull_epoch ||
            !self.target_revision_matches(identity) ||
            view.params.target_count_enabled == 0 ||
            view.params.target_model_id != identity.model_id { return false; }
        let Some(slots) = self.local0_count_slots.as_mut() else { return false; };
        let Some((dst, state)) = slots.iter_mut()
            .find(|(_, state)| matches!(state, MainCountSlot::Idle)) else { return false; };
        encoder.copy_buffer_to_buffer(&view.counter_buf,
            TARGET_MODEL_COUNT_BYTE_OFFSET, dst, 0, 4);
        *state = MainCountSlot::Encoded(identity);
        self.local0_count_armed = None;
        true
    }

    /// The owner calls this only after the selected view's actual cull and indirect
    /// draw were recorded and its render pass was closed. 0..3 are solar depth
    /// views; internal slot 4 is the GI RSM sky view. Each counter is private.
    pub fn copy_shadow_target_count(&mut self, encoder: &mut wgpu::CommandEncoder,
                                    cascade: usize) -> bool {
        let Some(armed) = self.shadow_count_armed.get(cascade) else { return false; };
        let Some(identity) = *armed else { return false; };
        let view = if cascade == 4 {
            if !self.gi_dispatch_recorded.get() { return false; }
            self.sky_views.get(super::sky_vis::DIRECTION_COUNT)
        } else {
            self.shadow_views.get(cascade)
        };
        let Some(view) = view else { return false; };
        if self.instance_epoch() != identity.cull_epoch ||
            !self.target_revision_matches(identity) ||
            view.params.target_count_enabled == 0 ||
            view.params.target_model_id != identity.model_id { return false; }
        let Some(slots) = self.shadow_count_slots[cascade].as_mut() else { return false; };
        let Some((dst, state)) = slots.iter_mut()
            .find(|(_, state)| matches!(state, MainCountSlot::Idle)) else { return false; };
        encoder.copy_buffer_to_buffer(&view.counter_buf,
            TARGET_MODEL_COUNT_BYTE_OFFSET, dst, 0, 4);
        *state = MainCountSlot::Encoded(identity);
        self.shadow_count_probe_reason[cascade] = COUNT_PROBE_WAITING_MAP;
        self.shadow_count_armed[cascade] = None;
        true
    }

    /// Encode only after this frame's actual main COUNT dispatch. A full ring, absent
    /// dispatch, or changed target/epoch leaves the answer Unknown.
    pub fn copy_main_target_count(&mut self, encoder: &mut wgpu::CommandEncoder) -> bool {
        let Some(identity) = self.main_count_armed else { return false; };
        if !self.main_dispatch_recorded.get() || self.instance_epoch() != identity.cull_epoch ||
            !self.target_revision_matches(identity) ||
            self.params.target_count_enabled == 0 ||
            self.params.target_model_id != identity.model_id {
            return false;
        }
        let Some(slots) = self.main_count_slots.as_mut() else { return false; };
        let Some((dst, state)) = slots.iter_mut()
            .find(|(_, state)| matches!(state, MainCountSlot::Idle)) else { return false; };
        encoder.copy_buffer_to_buffer(&self.counter_buf,
            TARGET_MODEL_COUNT_BYTE_OFFSET, dst, 0, 4);
        *state = MainCountSlot::Encoded(identity);
        self.main_count_probe_reason = COUNT_PROBE_WAITING_MAP;
        self.target_model_id = self.target_model_default;
        self.main_count_armed = None;
        self.main_dispatch_recorded.set(false);
        true
    }

    /// Conservatively reserve the slot before queue.submit: a panic during submit
    /// cannot prove whether the GPU accepted the copy, so abort must drain it.
    pub fn main_target_count_before_submit(&mut self, token: u64) -> bool {
        let Some(slots) = self.main_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Encoded(identity) = state {
                if identity.token == token {
                    *state = MainCountSlot::Submitted {
                        identity: *identity, committed: false, aborted: true };
                    return true;
                }
            }
        }
        false
    }

    /// Call only after queue.submit accepted the encoder containing this copy.
    pub fn main_target_count_submitted(&mut self, token: u64) -> bool {
        let Some(slots) = self.main_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token {
                    *committed = false;
                    *aborted = false;
                    return true;
                }
            }
        }
        false
    }

    /// Call only after the enclosing render attempt returned successfully.
    pub fn main_target_count_commit(&mut self, token: u64) -> bool {
        let Some(slots) = self.main_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token && !*aborted {
                    *committed = true;
                    return true;
                }
            }
        }
        false
    }

    /// Failed frame: submitted copies still need their map drained before slot reuse.
    pub fn main_target_count_abort(&mut self, token: u64) {
        if self.main_count_latest.identity.token == token {
            self.main_count_latest.status = MainCountStatus::Unknown;
            self.main_count_latest.count = 0;
            self.main_count_probe_reason = COUNT_PROBE_NO_COPY;
            self.target_model_id = self.target_model_default;
        }
        if self.main_count_armed.is_some_and(|id| id.token == token) {
            self.main_count_armed = None;
        }
        if let Some(slots) = self.main_count_slots.as_mut() {
            for (_, state) in slots {
                match state {
                    MainCountSlot::Encoded(id) if id.token == token => *state = MainCountSlot::Idle,
                    MainCountSlot::Submitted { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    MainCountSlot::Mapping { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    _ => {}
                }
            }
        }
    }

    /// Poll only; never block the renderer to make a diagnostic current. A failed map
    /// or stale identity/epoch cannot turn zero bytes into an Absent result.
    pub fn harvest_main_target_count(&mut self, device: &wgpu::Device) {
        let current_epoch = self.instance_epoch();
        let current_revision = self.instances.target_revision();
        let Some(slots) = self.main_count_slots.as_mut() else { return; };
        for (buf, state) in slots.iter_mut() {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if !*committed && !*aborted { continue; }
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
                *state = MainCountSlot::Mapping {
                    identity: *identity, committed: *committed, aborted: *aborted, done: rx };
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in slots.iter_mut() {
            let current = std::mem::replace(state, MainCountSlot::Idle);
            match current {
                MainCountSlot::Mapping { identity, committed, aborted, done } => {
                    match done.try_recv() {
                        Ok(Ok(())) => {
                            let data = buf.slice(..).get_mapped_range();
                            let count = u32::from_le_bytes(data[..4].try_into().unwrap());
                            drop(data);
                            buf.unmap();
                            if committed && !aborted &&
                                identity == self.main_count_latest.identity &&
                                current_epoch == identity.cull_epoch &&
                                current_revision == Some((identity.model_id, identity.target_revision)) {
                                self.main_count_latest.count = count;
                                self.main_count_latest.status = if count == 0 {
                                    MainCountStatus::Absent
                                } else { MainCountStatus::Present };
                                self.main_count_probe_reason = COUNT_PROBE_READY;
                            } else if committed && !aborted && identity == self.main_count_latest.identity {
                                self.main_count_probe_reason = if current_epoch != identity.cull_epoch {
                                    COUNT_PROBE_EPOCH_CHANGED
                                } else { COUNT_PROBE_TARGET_CHANGED };
                            }
                        }
                        Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                            if identity == self.main_count_latest.identity {
                                self.main_count_probe_reason = COUNT_PROBE_MAP_FAILED;
                            }
                        }
                        Err(mpsc::TryRecvError::Empty) => {
                            *state = MainCountSlot::Mapping {
                                identity, committed, aborted, done };
                        }
                    }
                }
                other => *state = other,
            }
        }
    }

    pub fn main_target_count_fact(&self) -> MainCountFact {
        self.validate_target_fact(self.main_count_latest)
    }

    pub fn main_count_probe_reason(&self) -> u32 {
        self.probe_reason(self.main_count_latest, self.main_count_probe_reason)
    }

    /// Emit only a changed reason for the exact private request, capped for the
    /// renderer lifetime. Called solely by the private FFI COUNT getters.
    pub fn take_count_probe_event(&mut self, lane: usize) -> Option<(u64, u32, u64, u64, u64, u64)> {
        static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if !*ENABLED.get_or_init(|| std::env::var("WGR_GEOMETRY_MAIN_COUNT_PROBE")
            .is_ok_and(|value| value == "1")) { return None; }
        if lane >= 5 || self.count_probe_events_emitted >= 64 { return None; }
        if let Some(old) = self.count_probe_superseded[lane].take() {
            self.count_probe_events_emitted += 1;
            return Some((old.token, COUNT_PROBE_SUPERSEDED, old.target_revision,
                self.instances.target_revision().map_or(0, |(_, revision)| revision),
                old.cull_epoch, self.instance_epoch()));
        }
        let (identity, reason) = if lane == 0 {
            (self.main_count_latest.identity, self.main_count_probe_reason())
        } else {
            (self.shadow_count_latest[lane - 1].identity,
                self.shadow_count_probe_reason(lane - 1))
        };
        let token = identity.token;
        if token == 0 || self.count_probe_logged_reason[lane] == reason { return None; }
        self.count_probe_logged_reason[lane] = reason;
        self.count_probe_events_emitted += 1;
        Some((token, reason, identity.target_revision,
            self.instances.target_revision().map_or(0, |(_, revision)| revision),
            identity.cull_epoch, self.instance_epoch()))
    }

    pub fn shadow_count_probe_reason(&self, cascade: usize) -> u32 {
        self.shadow_count_latest.get(cascade).zip(self.shadow_count_probe_reason.get(cascade))
            .map_or(COUNT_PROBE_NO_COPY, |(&fact, &reason)| self.probe_reason(fact, reason))
    }

    fn probe_reason(&self, fact: MainCountFact, reason: u32) -> u32 {
        classify_count_probe_reason(fact.identity, reason,
            self.instance_epoch(), self.instances.target_revision())
    }

    pub fn shadow_target_count_before_submit(&mut self, token: u64, cascade: usize) -> bool {
        let Some(slots) = self.shadow_count_slots.get_mut(cascade).and_then(Option::as_mut) else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Encoded(identity) = state {
                if identity.token == token {
                    *state = MainCountSlot::Submitted {
                        identity: *identity, committed: false, aborted: true };
                    return true;
                }
            }
        }
        false
    }

    pub fn shadow_target_count_submitted(&mut self, token: u64, cascade: usize) -> bool {
        let Some(slots) = self.shadow_count_slots.get_mut(cascade).and_then(Option::as_mut) else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token {
                    *committed = false;
                    *aborted = false;
                    return true;
                }
            }
        }
        false
    }

    pub fn shadow_target_count_commit(&mut self, token: u64, cascade: usize) -> bool {
        let Some(slots) = self.shadow_count_slots.get_mut(cascade).and_then(Option::as_mut) else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token && !*aborted {
                    *committed = true;
                    return true;
                }
            }
        }
        false
    }

    pub fn shadow_target_count_abort(&mut self, token: u64, cascade: usize) {
        let Some(latest) = self.shadow_count_latest.get_mut(cascade) else { return; };
        if latest.identity.token == token {
            latest.status = MainCountStatus::Unknown;
            latest.count = 0;
            self.shadow_count_probe_reason[cascade] = COUNT_PROBE_NO_COPY;
        }
        if self.shadow_count_armed[cascade].is_some_and(|id| id.token == token) {
            self.shadow_count_armed[cascade] = None;
        }
        if let Some(slots) = self.shadow_count_slots[cascade].as_mut() {
            for (_, state) in slots {
                match state {
                    MainCountSlot::Encoded(id) if id.token == token => *state = MainCountSlot::Idle,
                    MainCountSlot::Submitted { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    MainCountSlot::Mapping { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    _ => {}
                }
            }
        }
    }

    pub fn harvest_shadow_target_count(&mut self, device: &wgpu::Device, cascade: usize) {
        let current_epoch = self.instance_epoch();
        let current_revision = self.instances.target_revision();
        let Some(slots) = self.shadow_count_slots.get_mut(cascade).and_then(Option::as_mut) else { return; };
        for (buf, state) in slots.iter_mut() {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if !*committed && !*aborted { continue; }
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
                *state = MainCountSlot::Mapping {
                    identity: *identity, committed: *committed, aborted: *aborted, done: rx };
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in slots.iter_mut() {
            let current = std::mem::replace(state, MainCountSlot::Idle);
            match current {
                MainCountSlot::Mapping { identity, committed, aborted, done } => {
                    match done.try_recv() {
                        Ok(Ok(())) => {
                            let data = buf.slice(..).get_mapped_range();
                            let count = u32::from_le_bytes(data[..4].try_into().unwrap());
                            drop(data);
                            buf.unmap();
                            if committed && !aborted &&
                                identity == self.shadow_count_latest[cascade].identity &&
                                current_epoch == identity.cull_epoch &&
                                current_revision == Some((identity.model_id, identity.target_revision)) {
                                self.shadow_count_latest[cascade].count = count;
                                self.shadow_count_latest[cascade].status = if count == 0 {
                                    MainCountStatus::Absent
                                } else { MainCountStatus::Present };
                                self.shadow_count_probe_reason[cascade] = COUNT_PROBE_READY;
                            } else if committed && !aborted &&
                                identity == self.shadow_count_latest[cascade].identity {
                                self.shadow_count_probe_reason[cascade] = if current_epoch != identity.cull_epoch {
                                    COUNT_PROBE_EPOCH_CHANGED
                                } else { COUNT_PROBE_TARGET_CHANGED };
                            }
                        }
                        Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                            if identity == self.shadow_count_latest[cascade].identity {
                                self.shadow_count_probe_reason[cascade] = COUNT_PROBE_MAP_FAILED;
                            }
                        }
                        Err(mpsc::TryRecvError::Empty) => {
                            *state = MainCountSlot::Mapping {
                                identity, committed, aborted, done };
                        }
                    }
                }
                other => *state = other,
            }
        }
    }

    pub fn shadow_target_count_fact(&self, cascade: usize) -> MainCountFact {
        let Some(&latest) = self.shadow_count_latest.get(cascade) else { return MainCountFact::default(); };
        self.validate_target_fact(latest)
    }

    /// The planar command buffer is submitted before the main encoder. Reserve
    /// its copy at that boundary; a panic must leave it drainable but invalid.
    pub fn reflection_target_count_before_submit(&mut self, token: u64) -> bool {
        let Some(slots) = self.reflection_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Encoded(identity) = state {
                if identity.token == token {
                    *state = MainCountSlot::Submitted {
                        identity: *identity, committed: false, aborted: true };
                    return true;
                }
            }
        }
        false
    }

    pub fn reflection_target_count_submitted(&mut self, token: u64) -> bool {
        let Some(slots) = self.reflection_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token {
                    *committed = false;
                    *aborted = false;
                    return true;
                }
            }
        }
        false
    }

    pub fn reflection_target_count_commit(&mut self, token: u64) -> bool {
        let Some(slots) = self.reflection_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token && !*aborted {
                    *committed = true;
                    return true;
                }
            }
        }
        false
    }

    pub fn reflection_target_count_abort(&mut self, token: u64) {
        if self.reflection_count_latest.identity.token == token {
            self.reflection_count_latest.status = MainCountStatus::Unknown;
            self.reflection_count_latest.count = 0;
        }
        if self.reflection_count_armed.is_some_and(|id| id.token == token) {
            self.reflection_count_armed = None;
        }
        self.reflection_dispatch_recorded.set(false);
        if let Some(slots) = self.reflection_count_slots.as_mut() {
            for (_, state) in slots {
                match state {
                    MainCountSlot::Encoded(id) if id.token == token => *state = MainCountSlot::Idle,
                    MainCountSlot::Submitted { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    MainCountSlot::Mapping { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    _ => {}
                }
            }
        }
    }

    pub fn harvest_reflection_target_count(&mut self, device: &wgpu::Device) {
        let current_epoch = self.instance_epoch();
        let current_revision = self.instances.target_revision();
        let Some(slots) = self.reflection_count_slots.as_mut() else { return; };
        for (buf, state) in slots.iter_mut() {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if !*committed && !*aborted { continue; }
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
                *state = MainCountSlot::Mapping {
                    identity: *identity, committed: *committed, aborted: *aborted, done: rx };
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in slots.iter_mut() {
            let current = std::mem::replace(state, MainCountSlot::Idle);
            match current {
                MainCountSlot::Mapping { identity, committed, aborted, done } => {
                    match done.try_recv() {
                        Ok(Ok(())) => {
                            let data = buf.slice(..).get_mapped_range();
                            let count = u32::from_le_bytes(data[..4].try_into().unwrap());
                            drop(data);
                            buf.unmap();
                            if committed && !aborted &&
                                identity == self.reflection_count_latest.identity &&
                                current_epoch == identity.cull_epoch &&
                                current_revision == Some((identity.model_id, identity.target_revision)) {
                                self.reflection_count_latest.count = count;
                                self.reflection_count_latest.status = if count == 0 {
                                    MainCountStatus::Absent
                                } else { MainCountStatus::Present };
                            }
                        }
                        Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {}
                        Err(mpsc::TryRecvError::Empty) => {
                            *state = MainCountSlot::Mapping {
                                identity, committed, aborted, done };
                        }
                    }
                }
                other => *state = other,
            }
        }
    }

    pub fn reflection_target_count_fact(&self) -> MainCountFact {
        self.validate_target_fact(self.reflection_count_latest)
    }

    /// Private zenith-view diagnostic: only the encoder containing its copied
    /// counter may submit; a failed final frame aborts even an accepted copy.
    pub fn sky0_target_count_before_submit(&mut self, token: u64) -> bool {
        let Some(slots) = self.sky0_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Encoded(identity) = state {
                if identity.token == token {
                    *state = MainCountSlot::Submitted {
                        identity: *identity, committed: false, aborted: true };
                    return true;
                }
            }
        }
        false
    }

    pub fn sky0_target_count_submitted(&mut self, token: u64) -> bool {
        let Some(slots) = self.sky0_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token {
                    *committed = false;
                    *aborted = false;
                    return true;
                }
            }
        }
        false
    }

    pub fn sky0_target_count_commit(&mut self, token: u64) -> bool {
        let Some(slots) = self.sky0_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token && !*aborted {
                    *committed = true;
                    return true;
                }
            }
        }
        false
    }

    pub fn sky0_target_count_abort(&mut self, token: u64) {
        if self.sky0_count_latest.identity.token == token {
            self.sky0_count_latest.status = MainCountStatus::Unknown;
            self.sky0_count_latest.count = 0;
        }
        if self.sky0_count_armed.is_some_and(|id| id.token == token) {
            self.sky0_count_armed = None;
        }
        self.sky0_dispatch_recorded.set(false);
        if let Some(slots) = self.sky0_count_slots.as_mut() {
            for (_, state) in slots {
                match state {
                    MainCountSlot::Encoded(id) if id.token == token => *state = MainCountSlot::Idle,
                    MainCountSlot::Submitted { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    MainCountSlot::Mapping { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    _ => {}
                }
            }
        }
    }

    pub fn harvest_sky0_target_count(&mut self, device: &wgpu::Device) {
        let current_epoch = self.instance_epoch();
        let current_revision = self.instances.target_revision();
        let Some(slots) = self.sky0_count_slots.as_mut() else { return; };
        for (buf, state) in slots.iter_mut() {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if !*committed && !*aborted { continue; }
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
                *state = MainCountSlot::Mapping {
                    identity: *identity, committed: *committed, aborted: *aborted, done: rx };
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in slots.iter_mut() {
            let current = std::mem::replace(state, MainCountSlot::Idle);
            match current {
                MainCountSlot::Mapping { identity, committed, aborted, done } => {
                    match done.try_recv() {
                        Ok(Ok(())) => {
                            let data = buf.slice(..).get_mapped_range();
                            let count = u32::from_le_bytes(data[..4].try_into().unwrap());
                            drop(data);
                            buf.unmap();
                            if committed && !aborted &&
                                identity == self.sky0_count_latest.identity &&
                                current_epoch == identity.cull_epoch &&
                                current_revision == Some((identity.model_id, identity.target_revision)) {
                                self.sky0_count_latest.count = count;
                                self.sky0_count_latest.status = if count == 0 {
                                    MainCountStatus::Absent
                                } else { MainCountStatus::Present };
                            }
                        }
                        Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {}
                        Err(mpsc::TryRecvError::Empty) => {
                            *state = MainCountSlot::Mapping {
                                identity, committed, aborted, done };
                        }
                    }
                }
                other => *state = other,
            }
        }
    }

    pub fn sky0_target_count_fact(&self) -> MainCountFact {
        self.validate_target_fact(self.sky0_count_latest)
    }

    pub fn local0_target_count_before_submit(&mut self, token: u64) -> bool {
        let Some(slots) = self.local0_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Encoded(identity) = state {
                if identity.token == token {
                    *state = MainCountSlot::Submitted {
                        identity: *identity, committed: false, aborted: true };
                    return true;
                }
            }
        }
        false
    }

    pub fn local0_target_count_submitted(&mut self, token: u64) -> bool {
        let Some(slots) = self.local0_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token {
                    *committed = false;
                    *aborted = false;
                    return true;
                }
            }
        }
        false
    }

    pub fn local0_target_count_commit(&mut self, token: u64) -> bool {
        let Some(slots) = self.local0_count_slots.as_mut() else { return false; };
        for (_, state) in slots {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if identity.token == token && !*aborted {
                    *committed = true;
                    return true;
                }
            }
        }
        false
    }

    pub fn local0_target_count_abort(&mut self, token: u64) {
        if self.local0_count_latest.identity.token == token {
            self.local0_count_latest.status = MainCountStatus::Unknown;
            self.local0_count_latest.count = 0;
        }
        if self.local0_count_armed.is_some_and(|id| id.token == token) {
            self.local0_count_armed = None;
        }
        if let Some(slots) = self.local0_count_slots.as_mut() {
            for (_, state) in slots {
                match state {
                    MainCountSlot::Encoded(id) if id.token == token => *state = MainCountSlot::Idle,
                    MainCountSlot::Submitted { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    MainCountSlot::Mapping { identity, aborted, .. } if identity.token == token =>
                        *aborted = true,
                    _ => {}
                }
            }
        }
    }

    pub fn harvest_local0_target_count(&mut self, device: &wgpu::Device) {
        let current_epoch = self.instance_epoch();
        let current_revision = self.instances.target_revision();
        let Some(slots) = self.local0_count_slots.as_mut() else { return; };
        for (buf, state) in slots.iter_mut() {
            if let MainCountSlot::Submitted { identity, committed, aborted } = state {
                if !*committed && !*aborted { continue; }
                let (tx, rx) = mpsc::channel();
                buf.slice(..).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
                *state = MainCountSlot::Mapping {
                    identity: *identity, committed: *committed, aborted: *aborted, done: rx };
            }
        }
        let _ = device.poll(wgpu::PollType::Poll);
        for (buf, state) in slots.iter_mut() {
            let current = std::mem::replace(state, MainCountSlot::Idle);
            match current {
                MainCountSlot::Mapping { identity, committed, aborted, done } => {
                    match done.try_recv() {
                        Ok(Ok(())) => {
                            let data = buf.slice(..).get_mapped_range();
                            let count = u32::from_le_bytes(data[..4].try_into().unwrap());
                            drop(data);
                            buf.unmap();
                            if committed && !aborted &&
                                identity == self.local0_count_latest.identity &&
                                current_epoch == identity.cull_epoch &&
                                current_revision == Some((identity.model_id, identity.target_revision)) {
                                self.local0_count_latest.count = count;
                                self.local0_count_latest.status = if count == 0 {
                                    MainCountStatus::Absent
                                } else { MainCountStatus::Present };
                            }
                        }
                        Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {}
                        Err(mpsc::TryRecvError::Empty) => {
                            *state = MainCountSlot::Mapping {
                                identity, committed, aborted, done };
                        }
                    }
                }
                other => *state = other,
            }
        }
    }

    pub fn local0_target_count_fact(&self) -> MainCountFact {
        self.validate_target_fact(self.local0_count_latest)
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
                                whatif_tris: [0; STAT_WHATIF_COUNT],
                                target_model_count: word(b + STAT_TARGET_MODEL),
                            };
                            for (k, h) in st.lod_hist.iter_mut().enumerate() {
                                *h = word(b + STAT_LOD_HIST + k);
                            }
                            for (k, w) in st.whatif_tris.iter_mut().enumerate() {
                                *w = word(b + STAT_WHATIF + k);
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
    // its drawable LOD levels (whose `section_base` is RELATIVE to `sections`). Model/LOD
    // births append; optional bounded reuse applies only to exclusively owned section rows.
    pub fn register_model(
        &mut self,
        bounding_sphere: f32,
        lods: &[LodGpu],
        sections: &[SectionGpu],
        materials: &[SectionMaterialGpu],
    ) -> u32 {
        self.register_model_span(bounding_sphere, lods, sections, materials).0
    }

    pub(super) fn register_model_span(
        &mut self, bounding_sphere: f32, lods: &[LodGpu],
        sections: &[SectionGpu], materials: &[SectionMaterialGpu],
    ) -> (u32, super::section_span_pool::Span) {
        debug_assert_eq!(sections.len(), materials.len(), "one material per section");
        // Reserve before consuming a reusable range. No allocation occurs during the commit.
        self.sections.reserve(sections.len()); self.section_materials.reserve(materials.len());
        self.lods.reserve(lods.len()); self.models.reserve(1);
        let append = self.sections.len();
        // OFF performs no ownership scan or freelist work.
        let valid = (self.section_spans.is_none() && self.lod_spans.is_none())
            || section_ownership_valid(lods, sections.len(), materials.len());
        let model_birth = self.models.len();
        // Validate both optional domains before taking either range; failure is sticky/global.
        if !valid || self.section_spans.as_ref().is_some_and(|p| !p.accepts_birth(model_birth, append, sections.len()))
            || self.lod_spans.as_ref().is_some_and(|p| !p.accepts_birth(model_birth, self.lods.len(), lods.len())) {
            self.disable_section_reuse();
        }
        let lod_append = self.lods.len();
        let lod_span = match &mut self.lod_spans {
            Some(pool) => pool.register(model_birth, lod_append, lods.len(), valid),
            None => super::section_span_pool::Span { base: lod_append, count: lods.len() },
        };
        let span = match &mut self.section_spans {
            Some(pool) => pool.register(self.models.len(), append, sections.len(), valid),
            None => super::section_span_pool::Span { base: append, count: sections.len() },
        };
        let section_offset = span.base as u32;
        let lod_offset = lod_span.base as u32;
        super::section_span_pool::write_span(&mut self.sections, span, sections);
        // Preserve ordinary malformed-material append behavior when reuse was disabled.
        if span.base == append { self.section_materials.extend_from_slice(materials); }
        else { super::section_span_pool::write_span(&mut self.section_materials, span, materials); }
        for (i, lod) in lods.iter().enumerate() {
            let mut l = *lod;
            l.section_base += section_offset;
            if lod_span.base == lod_append { self.lods.push(l); }
            else { self.lods[lod_span.base + i] = l; }
        }
        let model_id = self.models.len() as u32;
        self.models.push(ModelGpu {
            // ON empty inputs are safe births, not aliases of a future append at vector end.
            lod_base: if self.lod_spans.is_some() && lods.is_empty() { 0 } else { lod_offset },
            lod_count: lods.len() as u32,
            bounding_sphere,
            _pad: 0,
        });
        if let Some(rows) = &mut self.model_row_uploads { rows.mark(model_id as usize); }
        // Model registration changes these four tables, never the crown centres.
        // Preserve any earlier Crown dirtiness in this batch; prepare still bootstraps
        // ALL when any backing buffer is absent, including the empty crown buffer.
        self.table_uploads.mark(TableDomain::Model);
        self.table_uploads.mark(TableDomain::Lod);
        self.table_uploads.mark(TableDomain::Section);
        self.table_uploads.mark(TableDomain::Material);
        (model_id, span)
    }

    // Keep append-only model IDs valid for stale instances. OFF keeps zero-count
    // historical LOD rows; ON redirects to the permanent sentinel before payload reuse.
    // The mesh ranges have their own retirement fence.
    pub fn retire_model(&mut self, id: u32) {
        if let Some(feedback) = &self.lod_feedback { feedback.borrow_mut().retire(id); }
        if let Some(pool) = &mut self.section_spans { pool.retire(id as usize); }
        if self.lod_spans.is_some() {
            let birth_span = self.lod_spans.as_ref().unwrap().birth_span(id as usize);
            let (changed, old_span) = retire_model_to_sentinel(&mut self.models, &mut self.lods,
                id, birth_span, &mut self.instances, &mut self.table_uploads);
            if changed {
                if let Some(rows) = &mut self.model_row_uploads { rows.mark(id as usize); }
                if old_span.is_none() { self.disable_section_reuse(); }
                let pool = self.lod_spans.as_mut().unwrap();
                // Old model is already a permanent tombstone, before its rows become available.
                pool.retire(id as usize);
                pool.drain(4096, 1, |_| {});
                if !pool.enabled() { self.disable_section_reuse(); }
            }
        } else {
            retire_model_content(&self.models, &mut self.lods, id,
                                 &mut self.instances, &mut self.table_uploads);
        }
    }

    pub(super) fn take_lod_reuse_notice(&mut self) -> Option<String> {
        self.lod_spans.as_mut().and_then(|pool| pool.take_notice())
    }
    pub(super) fn lod_reuse_length(&self) -> Option<usize> {
        self.lod_spans.as_ref().map(|_| self.lods.len())
    }
    pub(super) fn note_committed_lod_reuse(&mut self, model: u32, old_len: Option<usize>) -> Option<String> {
        let old_len = old_len?; let birth = self.models.get(model as usize)?;
        let span = super::section_span_pool::Span { base: birth.lod_base as usize, count: birth.lod_count as usize };
        let new_len = self.lods.len();
        self.lod_spans.as_mut().and_then(|pool| pool.note_committed_reuse(model, span, old_len, new_len))
    }
    pub(super) fn take_section_reuse_notice(&mut self) -> Option<String> {
        self.section_spans.as_mut().and_then(|pool| pool.take_notice())
    }
    pub(super) fn note_committed_section_reuse(&mut self, model: u32,
        span: super::section_span_pool::Span, old_len: usize) -> Option<String> {
        let new_len = self.sections.len();
        self.section_spans.as_mut().and_then(|pool|
            pool.note_committed_reuse(model, span, old_len, new_len))
    }
    pub(super) fn disable_section_reuse(&mut self) {
        if let Some(pool) = &mut self.section_spans { pool.disable(); }
        if let Some(pool) = &mut self.lod_spans { pool.disable(); }
    }
    pub(super) fn validate_source_table_length(&mut self, source_len: usize) {
        if (self.section_spans.is_some() || self.lod_spans.is_some()) && self.sections.len() != source_len {
            self.disable_section_reuse();
        }
    }

    /// ON-only bounded metadata cleanup. Caller clears matching source rows in the same operation.
    pub(super) fn clear_retired_sections(&mut self, mut clear_source: impl FnMut(super::section_span_pool::Span)) -> bool {
        let Some(pool) = &mut self.section_spans else { return false; };
        let sections = &mut self.sections; let materials = &mut self.section_materials;
        let changed = pool.drain(4096, 32, |span| {
            clear_source(span);
            sections[span.base..span.base + span.count].fill(SectionGpu::zeroed());
            materials[span.base..span.base + span.count].fill(SectionMaterialGpu::zeroed());
        });
        if changed { self.table_uploads.mark(TableDomain::Section); self.table_uploads.mark(TableDomain::Material); }
        if self.section_spans.as_ref().is_some_and(|pool| !pool.enabled()) { self.disable_section_reuse(); }
        changed
    }

    // Append a batch of per-tree crown centres (model space) to the global table; return the base
    // index of this batch (foliage-translucency-plan.md §9 Approach A). Forest vertices carry
    // `base + local_component_index` in their `conform` word, which vs_gpu reads to get a per-tree
    // radial-normal centre. Register-once; only the crown table becomes dirty.
    pub fn register_crown_centres(&mut self, centres: &[[f32; 4]]) -> u32 {
        let base = self.crown_centres.len() as u32;
        self.crown_centres.extend_from_slice(centres);
        self.table_uploads.mark(TableDomain::Crown);
        base
    }

    // Replace the whole sections table with a freshly-resolved one (same length + order as
    // registration, only base_vertex/first_index may have moved). Marks the tables dirty for
    // re-upload only when something actually changed, so a quiet frame costs nothing. The LOD
    // section_base offsets index this table and are unaffected (order is preserved).
    pub fn set_sections(&mut self, sections: &[SectionGpu]) {
        if resolved_sections_changed(&self.sections, sections, &mut self.instances) {
            // Resolved mesh ranges can change without an instance-table mutation.
            // Without a reverse section->model owner, the optional target witness
            // cannot safely identify which model changed.
            if self.sections.len() != sections.len() {
                if let Some(pool) = &mut self.section_spans { pool.disable(); }
            }
            self.sections.clear();
            self.sections.extend_from_slice(sections);
            self.table_uploads.mark(TableDomain::Section);
        }
    }

    // Add a static instance; returns its generation-carrying HANDLE (see `InstanceTable`).
    pub fn instance_add(&mut self, inst: InstanceGpu) -> u32 {
        self.instances.add(inst)
    }

    pub fn instance_update(&mut self, handle: u32, inst: InstanceGpu) {
        self.instances.update(handle, inst);
    }

    pub fn instance_remove(&mut self, handle: u32) {
        self.instances.remove(handle);
    }

    /// Private opt-in witness for one model's static and dynamic instance rows.
    /// Geometry edits also advance it conservatively. COUNT facts bind to this
    /// revision in addition to the global epoch; neither permits Fine release.
    pub fn arm_retained_target_revision(&mut self, model_id: u32) -> Option<u64> {
        self.models.get(model_id as usize)?;
        self.instances.arm_target_revision(model_id)
    }

    pub fn retained_target_revision(&self) -> Option<(u32, u64)> {
        self.instances.target_revision()
    }

    pub(super) fn armed_target_palette_independent(
        &self, source_len: usize, section_plain: impl FnMut(usize, &SectionGpu) -> bool,
    ) -> bool {
        if source_len != self.sections.len() || self.section_materials.len() != self.sections.len() {
            return false;
        }
        let Some((model_id, _)) = self.instances.target_revision() else { return false; };
        palette_independent_target_sections(
            &self.models, &self.lods, &self.sections, model_id, section_plain)
    }

    /// Opt-in source-local provenance for global-epoch churn caused by the
    /// retained static table. It is never evidence that a cached image is
    /// current: dynamic instances, mesh/material/texture/pose edits and other
    /// casters require separate provenance.
    pub(crate) fn arm_static_noop_epoch(&mut self) -> u64 {
        self.instances.arm_static_noop_epoch()
    }

    pub(crate) fn static_noop_epoch_relation(&self, start_epoch: u64) -> StaticEpochRelation {
        self.instances.static_noop_epoch_relation(start_epoch)
    }

    /// Private source-local evidence only. It watches the ordered bytes of
    /// every dynamic GPU row; unchanged rows do not prove a cached image current.
    pub(crate) fn arm_dynamic_image_mutation(&mut self) -> Option<u64> {
        self.instances.arm_dynamic_image_mutation()
    }

    pub(crate) fn dynamic_image_mutation_serial(&self) -> Option<u64> {
        self.instances.dynamic_image_mutation.as_deref()
            .and_then(|witness| (witness.serial != 0).then_some(witness.serial))
    }

    pub(crate) fn dynamic_row_relation(&self, stamp: u64) -> DynamicRowRelation {
        self.instances.dynamic_row_relation(stamp)
    }

    fn target_revision_matches(&self, identity: MainCountIdentity) -> bool {
        identity.target_revision != 0 &&
            self.instances.target_revision() == Some((identity.model_id, identity.target_revision))
    }

    /// Cached and asynchronously mapped facts must retain the revision sampled
    /// before their cull dispatch. A changed dynamic tail leaves the old count Unknown.
    pub fn validate_target_fact(&self, mut fact: MainCountFact) -> MainCountFact {
        if self.instance_epoch() != fact.identity.cull_epoch ||
            !self.target_revision_matches(fact.identity) {
            fact.status = MainCountStatus::Unknown;
            fact.count = 0;
        }
        fact
    }

    /// An audited geometry mutation without reverse target-model ownership may
    /// affect the armed model. Invalidate old samples by advancing its serial;
    /// the next exact sample can use the new value. Visual policy is unchanged.
    pub fn advance_retained_target_revision_for_geometry(&mut self) {
        self.instances.advance_target_revision_for_unattributed_geometry();
    }

    pub fn advance_retained_target_revision_with_kind(&mut self, kind: u32) {
        self.instances.advance_target_revision_with_kind(kind);
    }

    pub(super) fn palette_upload_changed(&mut self, independent_target: bool, image_serial: Option<u64>) {
        self.instances.palette_upload_changed(independent_target, image_serial);
    }

    pub fn retained_target_last_mutation(&self) -> u32 {
        self.instances.target_revision.as_ref().map_or(0, |w| w.last_mutation)
    }

    /// Completed off-handle writes may belong to any registered model. This
    /// private witness advances conservatively, invalidating stamped COUNT facts.
    pub fn observe_uploader_mutation_serial(&mut self, serial: u64) {
        self.instances.observe_uploader_mutation_serial(serial);
    }

    /// Handles refused by the retained instance table since startup (ID-3). Nonzero means
    /// something held a handle past its instance's removal — a C++-side identity bug that
    /// used to corrupt a live instance silently and now shows up here instead.
    pub fn stale_instance_ops(&self) -> u64 {
        self.instances.stale_ops()
    }

    // Replace the whole dynamic set (re-copied every frame — the churny set the CPU already
    // walks for simulation). The optional target witness compares selected rows only;
    // it does not advance the global epoch or relax existing cache-image gates.
    pub fn set_dynamic(&mut self, instances: &[InstanceGpu]) {
        replace_dynamic_instances(&mut self.dynamic, instances, &mut self.instances);
    }

    // Set this frame's cull params (frustum/cam/objectsZ/lod knobs). instance_count and the
    // variant fields are filled by prepare().
    pub fn set_params(&mut self, mut params: CullParamsGpu) {
        params.debug_flags = self.debug_flags | CULL_VIEW_MAIN_CAMERA;
        self.params = params;
    }

    // Set the color-pass occlusion view's params (frustum/LOD + the occlusion tail). Only used
    // when occlusion is active; instance_count/variant fields are filled by prepare().
    pub fn set_color_params(&mut self, mut params: CullParamsGpu) {
        params.debug_flags = self.debug_flags | CULL_VIEW_MAIN_CAMERA;
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

    pub fn shadow_count_cull_key(&self, i: usize) -> Option<CountCullInputKey> {
        self.shadow_views.get(i).map(|v| CountCullInputKey::from_params(&v.params))
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

    pub fn sky_count_cull_key(&self, i: usize) -> Option<CountCullInputKey> {
        self.sky_views.get(i).map(|v| CountCullInputKey::from_params(&v.params))
    }

    // Upload dirty tables + instances + params and (re)build the bind group. Call once per
    // frame before dispatch. Returns whether any GPU buffer was (re)allocated, so the
    // GPU-driven draw's group-1 bind group (which borrows instances/records/materials) is
    // rebuilt only when one actually moved.
    pub fn prepare(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) -> bool {
        let mut grew = false;

        // Crown/section APIs can be the first writer before any model registration.
        // Original wholesale upload initialized ALL backing buffers on that first dirty
        // prepare. Preserve it without probing buffer state or allocating on clean frames.
        if self.table_uploads != TableUploads::NONE &&
           (self.model_buf.buf.is_none() || self.lod_buf.buf.is_none() ||
            self.section_buf.buf.is_none() || self.section_mat_buf.buf.is_none() ||
            self.crown_centre_buf.buf.is_none()) {
            self.table_uploads = TableUploads::ALL;
        }
        let (next_uploads, tables_grew) = self.table_uploads.run(|domain| match domain {
            TableDomain::Model => match &mut self.model_row_uploads {
                Some(rows) => upload_model_rows(device, queue, &mut self.model_buf, &self.models, rows),
                None => upload_slice(device, queue, &mut self.model_buf, &self.models),
            },
            TableDomain::Lod => upload_slice(device, queue, &mut self.lod_buf, &self.lods),
            TableDomain::Section => upload_slice(device, queue, &mut self.section_buf, &self.sections),
            TableDomain::Material => upload_slice(device, queue, &mut self.section_mat_buf, &self.section_materials),
            TableDomain::Crown => upload_slice(device, queue, &mut self.crown_centre_buf, &self.crown_centres),
        });
        // Publish clean rows only after ALL selected domain bodies returned successfully.
        // A panic in any later domain retains both MODEL dirtiness and its interval.
        self.table_uploads = next_uploads;
        if let Some(rows) = &mut self.model_row_uploads { rows.reset_interval(); }
        grew |= tables_grew;

        // Instance buffer = static region then the dynamic tail. Ensure capacity for both.
        let static_len = self.instances.len() as u32;
        let total = static_len as u64 + self.dynamic.len() as u64;
        let inst_bytes = total.max(1) * std::mem::size_of::<InstanceGpu>() as u64;
        grew |= self.instance_buf.ensure(device, inst_bytes);
        let stride = std::mem::size_of::<InstanceGpu>() as u64;
        // Cleared here, not inside the branch: the pre-ID-3 code cleared `static_dirty`
        // unconditionally at the end of prepare, including on the no-buffer path.
        let dirty = self.instances.take_dirty();
        if let Some(buf) = self.instance_buf.buf.as_ref() {
            // A grown static region shifts the dynamic tail, so re-upload the whole static
            // region (not just the dirty range) whenever static_len changed.
            let full_static = self.uploaded_static_len != static_len;
            if full_static && static_len > 0 {
                queue.write_buffer(buf, 0, bytemuck::cast_slice(self.instances.as_slice()));
            } else if let Some((lo, hi)) = dirty {
                let range = &self.instances.as_slice()[lo as usize..=hi as usize];
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
        let target_model_id = self.target_model_id;
        let finalize = |p: &mut CullParamsGpu| {
            p.instance_count = total as u32;
            p.variant_capacity = variant_capacity;
            p.variant_count = CULL_VARIANT_COUNT;
            p.target_model_id = target_model_id.unwrap_or(0);
            p.target_count_enabled = u32::from(target_model_id.is_some());
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
        // Standalone reflection/sky/GI views must use the same opt-in target as
        // main and cascades. Otherwise their COUNT word stays zero even when the
        // selected model survives, making a later absence readback vacuous.
        view.params.target_model_id = self.target_model_id.unwrap_or(0);
        view.params.target_count_enabled = u32::from(self.target_model_id.is_some());
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
        drop(pass);
        if let Some(feedback) = &self.lod_feedback {
            let category = if label.contains("shadow") { 4 } else if label.contains("reflection") { 8 }
                else if label.contains("sky") { 16 } else if label.contains("color") { 2 } else { 1 };
            feedback.borrow_mut().capture(encoder, counters, COUNTER_WORDS * 4, category);
        }
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
        if self.main_count_armed.is_some() {
            self.main_dispatch_recorded.set(true);
        }
    }

    // Record cascade `i`'s cull into the same encoder. Shares the retained instance + table
    // buffers with the main dispatch; writes this cascade's own args/records/counters/scratch.
    // wgpu barriers the compute writes -> the depth pass's indirect reads. No-op until prepare()
    // has run with instances present.
    pub fn dispatch_shadow(&self, encoder: &mut wgpu::CommandEncoder, i: usize) -> bool {
        if self.params.instance_count == 0 {
            return false;
        }
        let Some(view) = self.shadow_views.get(i) else {
            return false;
        };
        let (Some(bind), Some(args), Some(sec)) = (
            view.bind.as_ref(),
            view.out_args.as_ref(),
            view.sec_count.as_ref(),
        ) else {
            return false;
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
        true
    }

    pub fn dispatch_reflection(&self, encoder: &mut wgpu::CommandEncoder) -> bool {
        if self.params.instance_count == 0 {
            return false;
        }
        let Some(view) = self.reflection_view.as_ref() else {
            return false;
        };
        let (Some(bind), Some(args), Some(sec)) = (
            view.bind.as_ref(),
            view.out_args.as_ref(),
            view.sec_count.as_ref(),
        ) else {
            return false;
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
        if self.reflection_count_armed.is_some() {
            self.reflection_dispatch_recorded.set(true);
        }
        true
    }
    // Record the interior sky-visibility cull (top-down ortho box). Recorded before the sky
    // depth pass so wgpu barriers the compute writes -> that pass's indirect reads. No-op until
    // set_sky_params + prepare() have run with instances present.
    pub fn dispatch_sky(&self, encoder: &mut wgpu::CommandEncoder, i: usize) -> bool {
        if self.params.instance_count == 0 {
            return false;
        }
        let Some(view) = self.sky_views.get(i) else {
            return false;
        };
        let (Some(bind), Some(args), Some(sec)) = (
            view.bind.as_ref(),
            view.out_args.as_ref(),
            view.sec_count.as_ref(),
        ) else {
            return false;
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
        if i == 0 && self.sky0_count_armed.is_some() {
            self.sky0_dispatch_recorded.set(true);
        }
        if (1..=BATCH_SKY_VIEWS).contains(&i) && self.batch_count.as_ref().is_some_and(|b| b.armed.is_some()) {
            self.batch_sky_dispatch_mask.set(self.batch_sky_dispatch_mask.get() | (1 << (i - 1)));
        }
        if i == super::sky_vis::DIRECTION_COUNT && self.shadow_count_armed[4].is_some() {
            self.gi_dispatch_recorded.set(true);
        }
        true
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
    /// Retained scene-content epoch: instance mutations and drawable model retirement.
    /// Name retained for API compatibility; not a full geometry-content revision.
    pub fn instance_epoch(&self) -> u64 {
        self.instances.epoch
    }

    pub(crate) fn instance_cpu_lookup(&self, handle: u32) -> (u64, InstanceCpuLookup) {
        (self.instances.epoch, self.instances.read_handle(handle))
    }

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
            // 8: AST-012A GPU mip feedback — the one WRITABLE binding in this group. Declared
            // only by fs_gpu; the prepass, shadow, velocity and sky entry points share the
            // layout and simply do not use it, which is legal. Fragment-visible writable
            // storage needs DownlevelFlags::FRAGMENT_WRITABLE_STORAGE, present on every
            // desktop backend this renderer targets.
            wgpu::BindGroupLayoutEntry {
                binding: 8,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Storage { read_only: false },
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
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
        if on { 1.0 } else { 0.0 }
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
// REN-TEMP-001T: the retained-path VEGETATION VELOCITY twin (vs_gpu_velocity /
// fs_gpu_velocity). Draws the same indirect stream as the prepass into the Rg16Float
// velocity target; non-canopy instances collapse in the VS. group(2) is the velocity
// uniform + depth (overlaying the bindless slot the colour variants use there).
#[allow(clippy::too_many_arguments)]
pub fn build_gpu_velocity_pipeline(
    device: &wgpu::Device,
    composer: &mut naga_oil::compose::Composer,
    camera_layout: &wgpu::BindGroupLayout,
    group1_layout: &wgpu::BindGroupLayout,
    bindless_layout: &wgpu::BindGroupLayout,
    vel_layout: &wgpu::BindGroupLayout,
    conform_layout: &wgpu::BindGroupLayout,
) -> wgpu::RenderPipeline {
    let module = crate::shaders::make_module(
        device,
        composer,
        "wgr_gpu_driven_velocity",
        include_str!("gpu_driven.wgsl"),
        "gfx3d/gpu_driven.wgsl",
    );
    let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("wgr_gpu_velocity_pipeline_layout"),
        bind_group_layouts: &[
            Some(camera_layout),
            Some(group1_layout),
            Some(bindless_layout),
            Some(vel_layout),
            Some(conform_layout),
        ],
        immediate_size: 0,
    });
    let vbuf_attrs = [
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x3,
            offset: 0,
            shader_location: 0,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x2,
            offset: 24,
            shader_location: 2,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Uint32,
            offset: 32,
            shader_location: 5,
        },
    ];
    let vbuf_layout = wgpu::VertexBufferLayout {
        array_stride: std::mem::size_of::<crate::ffi::WgrMeshVertex>() as u64,
        step_mode: wgpu::VertexStepMode::Vertex,
        attributes: &vbuf_attrs,
    };
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("wgr_gpu_velocity_pipeline"),
        layout: Some(&pipeline_layout),
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("vs_gpu_velocity"),
            buffers: &[vbuf_layout],
            compilation_options: Default::default(),
        },
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            front_face: wgpu::FrontFace::Cw,
            // Vegetation cards are double-sided.
            cull_mode: None,
            ..Default::default()
        },
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("fs_gpu_velocity"),
            targets: &[Some(wgpu::ColorTargetState {
                format: wgpu::TextureFormat::Rg16Float,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
            compilation_options: Default::default(),
        }),
        multiview_mask: None,
        cache: None,
    })
}

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
    // REN-OBJ-004: build the COLOUR pipeline with depth writes off and the forced-early-depth
    // fragment entry. Only for the prepassed main-view draw; the reflection and prepass
    // builders pass false. Callers must have checked shaders::early_depth_supported().
    early_z: bool,
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
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x3,
            offset: 0,
            shader_location: 0,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x3,
            offset: 12,
            shader_location: 1,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x2,
            offset: 24,
            shader_location: 2,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Uint32,
            offset: 32,
            shader_location: 5,
        },
        wgpu::VertexAttribute {
            format: wgpu::VertexFormat::Float32x2,
            offset: 60,
            shader_location: 7,
        },
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
        ("cutout_mip_alpha", super::cutout_mip_alpha() as f64),
        // REN-OBJ-001/003. These two were missing here for one evening: the per-draw pipelines
        // in mod.rs carried them, the GPU-driven ones fell back to the shader's defaults --
        // which is how an A/B of WGR_FOLIAGE_SCREEN_AO read "no difference" (the flag never
        // reached the path that draws Everon's trees) and why the prepass census words stayed 0.
        ("foliage_screen_ao", super::foliage_screen_ao() as f64),
        ("count_fragments", super::count_fragments() as f64),
    ];
    let depth_stencil = wgpu::DepthStencilState {
        format: super::depth_format(device),
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
        depth_stencil: Some(wgpu::DepthStencilState {
            depth_write_enabled: Some(!early_z), // REN-OBJ-004: the prepass already wrote it
            ..depth_stencil.clone()
        }),
        multisample: wgpu::MultisampleState {
            count: sample_count,
            alpha_to_coverage_enabled: foliage_a2c,
            ..Default::default()
        },
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some(if early_z { "fs_gpu_early" } else { "fs_gpu" }),
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
// REN-GI-002: the sun-proxy pipeline -- the shadow depth pass with two colour targets
// (albedo, normal) from the same module. Depth test LessEqual, no bias: the proxy's depth is
// read back by the probe integrator for positions, not compared against.
pub fn build_gpu_rsm_pipeline(
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
        "wgr_gpu_driven_rsm",
        include_str!("gpu_driven_shadow.wgsl"),
        "gfx3d/gpu_driven_shadow.wgsl",
    );
    let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("wgr_gpu_driven_rsm_pipeline_layout"),
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
    let target = |format: wgpu::TextureFormat| {
        Some(wgpu::ColorTargetState {
            format,
            blend: None,
            write_mask: wgpu::ColorWrites::ALL,
        })
    };
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("wgr_gpu_driven_rsm_pipeline"),
        layout: Some(&pipeline_layout),
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("vs_gpu_rsm"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            buffers: &vbuffers,
        },
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            front_face: wgpu::FrontFace::Cw,
            cull_mode: None,
            ..Default::default()
        },
        depth_stencil: Some(wgpu::DepthStencilState {
            format: super::SHADOW_FORMAT,
            depth_write_enabled: Some(true),
            depth_compare: Some(wgpu::CompareFunction::LessEqual),
            stencil: wgpu::StencilState::default(),
            bias: wgpu::DepthBiasState::default(),
        }),
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("fs_gpu_rsm"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            targets: &[
                target(wgpu::TextureFormat::Rgba8Unorm),
                target(wgpu::TextureFormat::Rgba8Unorm),
            ],
        }),
        multiview_mask: None,
        cache: None,
    })
}

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
        format: super::depth_format(device),
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

fn upload_model_rows(device: &wgpu::Device, queue: &wgpu::Queue,
    arr: &mut super::StorageArray, data: &[ModelGpu], rows: &mut ModelRowUploads,
) -> bool {
    let grew = arr.ensure(device, std::mem::size_of_val(data).max(1) as u64);
    let selection = *rows;
    selection.run(data.len(), grew, |range| {
        let first = range.start; let count = range.len();
        queue.write_buffer(arr.buf.as_ref().unwrap(),
            (first * std::mem::size_of::<ModelGpu>()) as u64,
            bytemuck::cast_slice(&data[range]));
        // Only after the real queue.write_buffer returned, never on range choice/growth.
        rows.note_committed(first, count, data.len());
    });
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
    fn palette_independent_target_requires_current_bounded_model_sections() {
        let models = [
            ModelGpu { lod_base: 0, lod_count: 1, bounding_sphere: 1.0, _pad: 0 },
            ModelGpu { lod_base: 1, lod_count: 1, bounding_sphere: 1.0, _pad: 0 },
        ];
        let lods = [
            LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 },
            LodGpu { resolution: 1.0, section_base: 1, section_count: 1, is_decal: 0 },
        ];
        let sections = [SectionGpu { first_index: 0, index_count: 3, base_vertex: 0, variant: 0 }; 2];
        assert!(palette_independent_target_sections(&models, &lods, &sections, 0,
            |section, _| section == 0));
        assert!(!palette_independent_target_sections(&models, &lods, &sections, 0,
            |_, _| false), "a skinned or missing generational mesh refuses");
        assert!(!palette_independent_target_sections(&models, &lods, &sections, 1,
            |section, _| section == 0), "a new model incarnation cannot borrow the old section proof");
        let retired = [ModelGpu { lod_count: 0, ..models[0] }, models[1]];
        assert!(!palette_independent_target_sections(&retired, &lods, &sections, 0,
            |_, _| true));
        let missing = [LodGpu { section_base: 2, ..lods[0] }, lods[1]];
        assert!(!palette_independent_target_sections(&models, &missing, &sections, 0,
            |_, _| true));
        let excessive = [ModelGpu { lod_count: 17, ..models[0] }, models[1]];
        assert!(!palette_independent_target_sections(&excessive, &lods, &sections, 0,
            |_, _| true));
        let wide = [LodGpu { section_count: 65, ..lods[0] }, lods[1]];
        let many_sections = vec![sections[0]; 65];
        assert!(!palette_independent_target_sections(&models, &wide, &many_sections, 0,
            |_, _| true));
    }

    #[test]
    fn foreign_palette_upload_keeps_rigid_target_count_but_refusal_or_skin_invalidates() {
        let mut instances = InstanceTable::default();
        assert_eq!(instances.arm_target_revision(3), Some(1));
        instances.palette_upload_changed(true, Some(2));
        assert_eq!(instances.target_revision(), Some((3, 1)));
        instances.palette_upload_changed(false, Some(3));
        assert_eq!(instances.target_revision(), Some((3, 2)));
        instances.palette_upload_changed(true, None);
        assert_eq!(instances.target_revision(), Some((3, 3)));
        assert_eq!(instances.arm_target_revision(4), Some(4));
        instances.palette_upload_changed(false, Some(4));
        assert_eq!(instances.target_revision(), Some((4, 5)));
    }

    #[test]
    fn plain_count_cull_key_tracks_exact_classify_inputs() {
        let base = CullParamsGpu::zeroed();
        let key = CountCullInputKey::from_params(&base);
        let mut changed = base;
        changed.frustum[5][3] = 1.0;
        assert_ne!(CountCullInputKey::from_params(&changed), key);
        changed = base; changed.cam_pos[2] = 1.0;
        assert_ne!(CountCullInputKey::from_params(&changed), key);
        changed = base; changed.objects_z2 = 1.0;
        assert_ne!(CountCullInputKey::from_params(&changed), key);
        changed = base; changed.lod_scale = 1.0;
        assert_ne!(CountCullInputKey::from_params(&changed), key);
        changed = base; changed.lod_inv_width = 1.0;
        assert_ne!(CountCullInputKey::from_params(&changed), key);
        changed = base; changed.pixel_limit = 1.0;
        assert_ne!(CountCullInputKey::from_params(&changed), key);
        changed = base; changed.debug_flags = 1;
        assert_ne!(CountCullInputKey::from_params(&changed), key);
        changed = base; changed.debug_flags = CULL_VIEW_MAIN_CAMERA;
        assert_ne!(CountCullInputKey::from_params(&changed), key,
            "a cached nonmain COUNT cannot stand in for a main-camera view");

        changed = base;
        changed.cam_pos[3] = 1.0;
        changed.debug_flags = 4; // unrelated/debug-future bit does not change classify
        changed.view_proj[0][0] = 1.0;
        changed.viewport = [1.0, 1.0];
        changed.hiz_mips = 1;
        changed.occlusion = 1;
        changed.variant_capacity = 1;
        changed.variant_count = 1;
        changed.instance_count = 1; // foreign rows may churn while target rows stay fixed
        changed.target_model_id = 1;
        changed.target_count_enabled = 1;
        assert_eq!(CountCullInputKey::from_params(&changed), key,
            "plain COUNT ignores the Hi-Z/variant tail and separately bound target identity");
    }

    #[test]
    fn target_count_stamp_ignores_foreign_rows_and_rejects_changed_target_before_copy() {
        let Some((device, _queue)) = headless() else { return; };
        let mut cull = CullState::new(&device);
        let lod = [LodGpu { resolution: 0.0, section_base: 0,
            section_count: 0, is_decal: 0 }];
        let target = cull.register_model(1.0, &lod, &[], &[]);
        let foreign = cull.register_model(1.0, &lod, &[], &[]);
        let row = |model, z| InstanceGpu { model, center: [0.0, 0.0, z, 1.0],
            ..InstanceGpu::zeroed() };
        cull.instance_add(row(foreign, 1.0)); // establish the existing global-epoch prerequisite
        cull.set_dynamic(&[row(target, 2.0)]);
        assert!(cull.arm_main_target_count(&device, 1, target, 1));
        let first = cull.main_target_count_fact().identity;
        assert_ne!(first.target_revision, 0);
        cull.set_dynamic(&[row(target, 2.0), row(foreign, 3.0)]);
        assert!(cull.target_revision_matches(first), "foreign dynamic churn is irrelevant");
        cull.params.target_count_enabled = 1;
        cull.params.target_model_id = target;
        cull.main_dispatch_recorded.set(true);
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        assert!(cull.copy_main_target_count(&mut encoder), "unchanged target can be copied");

        assert!(cull.arm_main_target_count(&device, 2, target, 1));
        let second = cull.main_target_count_fact().identity;
        cull.params.target_count_enabled = 1;
        cull.params.target_model_id = target;
        cull.main_dispatch_recorded.set(true);
        cull.set_dynamic(&[row(target, 4.0), row(foreign, 3.0)]);
        assert!(!cull.target_revision_matches(second));
        assert!(!cull.copy_main_target_count(&mut encoder),
            "a target change after arm cannot label an earlier COUNT with the new revision");
        assert_eq!(cull.validate_target_fact(MainCountFact { identity: second,
            status: MainCountStatus::Absent, count: 0 }).status, MainCountStatus::Unknown);
        assert_eq!(cull.validate_target_fact(MainCountFact { identity: MainCountIdentity {
            target_revision: 0, ..second }, status: MainCountStatus::Absent, count: 0 }).status,
            MainCountStatus::Unknown, "unstamped old facts are never certified");
    }

    #[test]
    fn pending_target_count_map_cannot_resolve_absent_after_dynamic_mutation() {
        let Some((device, _queue)) = headless() else { return; };
        let mut cull = CullState::new(&device);
        let lod = [LodGpu { resolution: 0.0, section_base: 0,
            section_count: 0, is_decal: 0 }];
        let target = cull.register_model(1.0, &lod, &[], &[]);
        let row = |z| InstanceGpu { model: target, center: [0.0, 0.0, z, 1.0],
            ..InstanceGpu::zeroed() };
        cull.instance_add(row(1.0));
        cull.set_dynamic(&[row(2.0)]);
        assert!(cull.arm_main_target_count(&device, 1, target, 1));
        let identity = cull.main_target_count_fact().identity;
        cull.main_count_slots.as_mut().unwrap()[0].1 = MainCountSlot::Submitted {
            identity, committed: true, aborted: false };
        cull.set_dynamic(&[row(3.0)]);
        for _ in 0..100 {
            cull.harvest_main_target_count(&device);
            if matches!(cull.main_count_slots.as_ref().unwrap()[0].1, MainCountSlot::Idle) { break; }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert!(matches!(cull.main_count_slots.as_ref().unwrap()[0].1, MainCountSlot::Idle),
            "the asynchronous map must have completed");
        assert_eq!(cull.main_count_latest.status, MainCountStatus::Unknown,
            "a mapped zero from the old revision must never become Absent");
        assert_eq!(cull.main_target_count_fact().status, MainCountStatus::Unknown);
    }

    #[test]
    fn pending_count_copy_requires_exact_committed_request() {
        let current = MainCountIdentity { token: 7, model_id: 3,
            source_generation: 11, cull_epoch: 13, target_revision: 1 };
        let old = MainCountIdentity { token: 6, ..current };
        assert!(!MainCountSlot::Encoded(current).pending_for(current));
        assert!(!MainCountSlot::Submitted { identity: current, committed: false,
            aborted: false }.pending_for(current));
        assert!(!MainCountSlot::Submitted { identity: current, committed: true,
            aborted: true }.pending_for(current));
        let submitted = MainCountSlot::Submitted { identity: current,
            committed: true, aborted: false };
        assert!(submitted.pending_for(current));
        assert!(!submitted.pending_for(old));
        let (_tx, rx) = mpsc::channel();
        let mapping = MainCountSlot::Mapping { identity: current,
            committed: true, aborted: false, done: rx };
        assert!(mapping.pending_for(current));
        let copied = (1 << 0) | (1 << 4) | (1 << 26);
        assert_eq!(BatchCountSlot::Submitted { identity: current, copied,
            committed: true, aborted: false }.pending_copied_for(current), copied);
        assert_eq!(BatchCountSlot::Submitted { identity: current, copied,
            committed: true, aborted: true }.pending_copied_for(current), 0);
        assert_eq!(BatchCountSlot::Submitted { identity: old, copied,
            committed: true, aborted: false }.pending_copied_for(current), 0);
    }

    #[test]
    fn cull_wgsl_validates() {
        for feedback in [false, true] {
        let source = super::super::lod_feedback::shader_source(include_str!("cull.wgsl"), feedback).unwrap();
        let module = naga::front::wgsl::parse_str(&source).expect("cull.wgsl parse");
        naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .expect("cull.wgsl validate");
        assert_eq!(module.global_variables.iter().filter(|(_, v)| v.binding.is_some()).count(), 10);
        let ir = format!("{:?}", module.functions);
        assert_eq!(ir.contains("InclusiveOr"), feedback); // optional injection leaves OFF source unchanged
        }
        assert_eq!(counter_words_for(false), COUNTER_WORDS);
        assert_eq!(counter_words_for(true), COUNTER_WORDS + 8);
    }

    // AST-012A: binding 8 is fragment-visible WRITABLE storage, which needs a downlevel
    // capability the composer test above cannot see — naga_oil validates the SHADER, not
    // whether a device will accept the layout it wants. Build the layout on a real device so a
    // backend that refuses it fails here, rather than as an object draw that silently never
    // gets a bind group. Skips silently with no adapter.
    #[test]
    fn gpu_group1_layout_builds_on_a_real_device() {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let Ok(adapter) =
            pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::default(),
                compatible_surface: None,
                force_fallback_adapter: false,
            }))
        else {
            return;
        };
        assert!(
            adapter
                .get_downlevel_capabilities()
                .flags
                .contains(wgpu::DownlevelFlags::FRAGMENT_WRITABLE_STORAGE),
            "the mip-feedback write needs FRAGMENT_WRITABLE_STORAGE"
        );
        let Ok((device, _queue)) =
            pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))
        else {
            return;
        };
        let _layout = gpu_group1_layout(&device);
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
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
        // Actual object shaders include conform resources in group 4, matching
        // the production device's five-group requirement.
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            required_limits: wgpu::Limits {
                max_bind_groups: 5,
                ..wgpu::Limits::default()
            },
            ..Default::default()
        })).ok()
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
    // ID-2: instance_add returns a generation-carrying HANDLE; the GPU buffers speak raw
    // slot indices. Tests that assert against GPU records decode with this.
    fn raw_slot(handle: u32) -> u32 {
        (handle & 0x00FF_FFFF) - 1
    }

    #[test]
    fn instance_cpu_lookup_is_literal_read_only_and_generation_checked() {
        let mut table = InstanceTable::default();
        assert!(matches!(table.read_handle(0), InstanceCpuLookup::Invalid));
        assert!(matches!(table.read_handle(0xff00_0000), InstanceCpuLookup::Invalid));
        assert!(matches!(table.read_handle(1), InstanceCpuLookup::Invalid));

        let mut row = inst(31);
        row.flags = INSTANCE_MAIN_CAMERA_ONLY;
        row.world[12] = 123.25;
        row.center = [8.0, 9.0, 10.0, 2.0];
        row.conform2 = [1.0, 2.0, 3.0, 4.0];
        let first = table.add(row);
        let epoch = table.epoch;
        let stale_ops = table.stale_ops();
        let InstanceCpuLookup::Present { slot, row: actual } = table.read_handle(first) else {
            panic!("new literal handle must resolve");
        };
        assert_eq!(slot, raw_slot(first));
        assert_eq!(bytemuck::bytes_of(&actual), bytemuck::bytes_of(&row));
        assert_eq!((table.epoch, table.stale_ops()), (epoch, stale_ops));

        let mut updated = row;
        updated.model = 32;
        updated.world[12] = -456.5;
        table.update(first, updated);
        let InstanceCpuLookup::Present { row: actual, .. } = table.read_handle(first) else {
            panic!("update must be visible through the same handle");
        };
        assert_eq!(bytemuck::bytes_of(&actual), bytemuck::bytes_of(&updated));
        table.remove(first);
        let removed_epoch = table.epoch;
        assert!(matches!(table.read_handle(first), InstanceCpuLookup::Absent));
        let second = table.add(inst(77));
        assert_eq!(raw_slot(first), raw_slot(second));
        assert_ne!(first, second);
        assert!(matches!(table.read_handle(first), InstanceCpuLookup::Absent));
        assert!(matches!(table.read_handle(second), InstanceCpuLookup::Present { row, .. } if row.model == 77));
        assert!(table.epoch > removed_epoch);
        assert_eq!(table.stale_ops(), stale_ops, "the getter must not count stale operations");
    }

    // ---- ID-3: the retained instance table's identity rules -------------------------
    //
    // These run on `InstanceTable` directly, NOT through `CullState`, and that is the
    // point: `CullState::new` builds compute pipelines and so needs an adapter, which
    // means every test written against it begins `let Some(..) = headless() else {
    // return; }` and PASSES VACUOUSLY on a machine with no GPU. The identity rules are
    // pure bookkeeping and must be provable without one.
    //
    // ID-2 landed the generation check with no test at all: deleting the check from
    // `slot_of` (returning `Some(slot)` unconditionally) broke nothing in the suite.
    // Each test below fails under exactly that ablation.

    fn inst(model: u32) -> InstanceGpu {
        InstanceGpu {
            world: [0.0; 16],
            center: [0.0; 4],
            model,
            flags: 0,
            cull_radius: 0,
            _pad: 0,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        }
    }

    #[test]
    fn a_recycled_slot_refuses_the_dead_occupants_handle() {
        let mut t = InstanceTable::default();
        let dead = t.add(inst(11));
        t.remove(dead);
        // The next add MUST land in the slot just freed -- otherwise this test would be
        // proving nothing about recycling.
        let live = t.add(inst(22));
        assert_eq!(
            raw_slot(dead),
            raw_slot(live),
            "test is vacuous unless the slot was actually recycled"
        );
        assert_ne!(dead, live, "a recycled slot must not reissue the same handle");

        // The whole point: writing through the stale handle must not touch the live
        // instance now occupying that slot.
        let before = t.stale_ops();
        t.update(dead, inst(99));
        assert_eq!(
            t.as_slice()[raw_slot(live) as usize].model,
            22,
            "a stale handle wrote through to the live instance that reused its slot"
        );
        assert_eq!(t.stale_ops(), before + 1, "the rejection was not counted");

        // ...and the live handle still works, so the check is not simply refusing everything.
        t.update(live, inst(33));
        assert_eq!(t.as_slice()[raw_slot(live) as usize].model, 33);
        assert_eq!(t.stale_ops(), before + 1);
    }

    #[test]
    fn a_stale_handle_cannot_remove_the_instance_that_replaced_it() {
        let mut t = InstanceTable::default();
        let dead = t.add(inst(11));
        t.remove(dead);
        let live = t.add(inst(22));

        // A double remove is the realistic C++-side bug (an object freed without its
        // SceneObjectRemoved, then the recycled address removed again). Without the
        // generation check this frees a live instance AND pushes the slot onto the free
        // list twice, after which two different objects are handed the same slot.
        t.remove(dead);
        assert_ne!(
            t.as_slice()[raw_slot(live) as usize].model,
            INVALID_MODEL,
            "a stale handle removed the live instance occupying its slot"
        );

        let a = t.add(inst(1));
        let b = t.add(inst(2));
        assert_ne!(
            raw_slot(a),
            raw_slot(b),
            "the free list handed the same slot out twice (double-free of a slot)"
        );
    }

    #[test]
    fn handle_zero_is_never_a_valid_slot() {
        let mut t = InstanceTable::default();
        let live = t.add(inst(7));
        assert_ne!(live, 0, "handle 0 is the FFI's error value and must never be issued");
        // 0 must be refused rather than resolving to slot 0, which is what the +1 bias buys.
        t.update(0, inst(99));
        assert_eq!(t.as_slice()[raw_slot(live) as usize].model, 7);
        assert_eq!(t.stale_ops(), 1);
    }

    #[test]
    fn a_handle_for_a_slot_that_was_never_allocated_is_refused() {
        let mut t = InstanceTable::default();
        t.add(inst(7));
        t.update(InstanceTable::handle(9999, 0), inst(99));
        assert_eq!(t.as_slice().len(), 1, "an out-of-range handle grew the table");
        assert_eq!(t.stale_ops(), 1);
    }

    #[test]
    fn stale_handle_aliases_only_after_256_generations() {
        // The documented bound, asserted so the number cannot quietly drift. 8 bits of
        // generation: 255 remove/add cycles of one slot are still distinguishable, the
        // 256th wraps and the original handle resolves again.
        let mut t = InstanceTable::default();
        let first = t.add(inst(1));
        for _ in 0..255 {
            let h = InstanceTable::handle(raw_slot(first), t.generations[raw_slot(first) as usize]);
            t.remove(h);
            let again = t.add(inst(1));
            assert_eq!(raw_slot(again), raw_slot(first));
            assert_ne!(again, first, "generations must stay distinct for 255 cycles");
        }
        // One more cycle wraps the 8-bit generation back onto the original handle.
        let h = InstanceTable::handle(raw_slot(first), t.generations[raw_slot(first) as usize]);
        t.remove(h);
        let wrapped = t.add(inst(1));
        assert_eq!(wrapped, first, "the 256-cycle aliasing bound moved");
    }

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
            raw_slot(front),
            "record.instance == visible slot"
        );
        assert_eq!(
            recs[rec_slot as usize * 2 + 1],
            1,
            "record.section == LOD1 section id"
        );
        // Retiring a model must suppress even an accidentally retained instance,
        // without indexing another model's LOD or reusing its stable identifier.
        cull.retire_model(model);
        cull.retire_model(model); // idempotent
        cull.retire_model(u32::MAX); // invalid id is ignored
        cull.prepare(&device, &queue);
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        cull.dispatch(&mut enc);
        queue.submit([enc.finish()]);
        let retired = read_u32s(&device, &queue, cull.out_args().unwrap(), total);
        assert!(retired.chunks_exact(words_per_arg as usize).all(|a| a[1] == 0));

    }

    #[test]
    fn authored_far_lod_uses_coarse_mesh_skips_decals_and_retires() {
        let Some((device, queue)) = headless() else { return; };
        let mut cull = CullState::new(&device);
        let sections: Vec<_> = (0..3).map(|i| SectionGpu {
            first_index: i * 6, index_count: 3, base_vertex: 0, variant: 0,
        }).collect();
        let lods: Vec<_> = (0..3).map(|i| LodGpu {
            resolution: i as f32 * 100.0, section_base: i, section_count: 1,
            is_decal: u32::from(i == 2),
        }).collect();
        let model = cull.register_model(1.0, &lods, &sections, &[SectionMaterialGpu::zeroed(); 3]);
        let mk = |flags| InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(), center: [0.0, 0.0, 10.0, 1.0],
            model, flags, cull_radius: 0, _pad: 0,
            conform0: [0.0; 4], conform1: [0.0; 4], conform2: [0.0; 4],
        };
        cull.instance_add(mk(0));
        let far = cull.instance_add(mk(16));
        let mut params = CullParamsGpu::zeroed();
        params.objects_z2 = 10000.0;
        params.lod_inv_width = 0.001;
        params.lod_scale = 1.0;
        cull.set_params(params);
        for expected in [vec![0, 6], vec![0]] {
            cull.prepare(&device, &queue);
            let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
            cull.dispatch(&mut encoder);
            queue.submit([encoder.finish()]);
            let count = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64 * ARG_WORDS;
            let words = read_u32s(&device, &queue, cull.out_args().unwrap(), count);
            let mut first_indices: Vec<_> = words.chunks_exact(ARG_WORDS as usize)
                .filter(|arg| arg[1] > 0).map(|arg| { assert_eq!(arg[1], 1); arg[2] }).collect();
            first_indices.sort_unstable();
            assert_eq!(first_indices, expected);
            if expected.len() == 2 { cull.instance_remove(far); }
        }
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
        assert!(cull.dispatch_shadow(&mut enc, 0), "cascade cull was encoded");
        assert!(!cull.dispatch_shadow(&mut enc, 1), "missing cascade cannot claim a dispatch");
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
            raw_slot(front),
            "shadow record.instance == front slot"
        );
        assert_eq!(
            recs[rec_slot as usize * 2 + 1],
            1,
            "shadow record.section == LOD1 section id"
        );
    }

    #[test]
    fn main_camera_only_instance_count_and_records_exclude_all_other_views() {
        let Some((device, queue)) = headless() else {
            assert_ne!(std::env::var("WGR_REQUIRE_TEST_GPU").as_deref(), Ok("1"),
                "main-camera-only cull needs an actual GPU adapter");
            return;
        };
        let mut cull = CullState::new(&device);
        cull.variant_capacity = 16; // bounded readback; production capacity is unchanged
        let sections = [SectionGpu { first_index: 0, index_count: 3, base_vertex: 0, variant: 0 }];
        let lods = [LodGpu { resolution: 0.0, section_base: 0, section_count: 1, is_decal: 0 }];
        let materials = [SectionMaterialGpu::zeroed()];
        let page_model = cull.register_model(1.0, &lods, &sections, &materials);
        let source_model = cull.register_model(1.0, &lods, &sections, &materials);
        let control_model = cull.register_model(1.0, &lods, &sections, &materials);
        let make = |model, flags| InstanceGpu {
            world: Mat4::IDENTITY.to_cols_array(), center: [0.0, 0.0, 0.0, 1.0],
            model, flags, cull_radius: 0, _pad: 0,
            conform0: [0.0; 4], conform1: [0.0; 4], conform2: [0.0; 4],
        };
        let page = raw_slot(cull.instance_add(make(page_model, INSTANCE_MAIN_CAMERA_ONLY)));
        let source = raw_slot(cull.instance_add(make(source_model, INSTANCE_OTHER_VIEWS_ONLY)));
        let control = raw_slot(cull.instance_add(make(control_model, 0)));
        let conflicting = raw_slot(cull.instance_add(make(page_model,
            INSTANCE_MAIN_CAMERA_ONLY | INSTANCE_OTHER_VIEWS_ONLY)));
        let mut params = CullParamsGpu::zeroed(); // zero planes include every instance
        params.objects_z2 = 100.0;
        params.lod_scale = 1.0;
        params.lod_inv_width = 1.0;
        params.pixel_limit = 0.0;
        cull.set_params(params);
        // Bind a real Hi-Z view so prepare/dispatch use the color compute path;
        // occlusion stays disabled to isolate the pass-authority predicate.
        cull.set_hiz(Some(const_hiz(&device, &queue, 4, 4, 0.0)));
        cull.set_color_params(params);
        cull.set_shadow_view_count(&device, 2); // solar cascade plus local-light tile
        cull.set_shadow_params(0, params);
        cull.set_shadow_params(1, params);
        cull.set_reflection_params(&device, params);
        let gi = super::super::sky_vis::DIRECTION_COUNT;
        cull.set_sky_view_count(&device, gi + 1);
        for i in 0..=gi { cull.set_sky_params(i, params); }
        cull.target_model_id = Some(page_model);
        cull.prepare(&device, &queue);
        assert_eq!(cull.params.debug_flags & CULL_VIEW_MAIN_CAMERA, CULL_VIEW_MAIN_CAMERA);
        assert_eq!(cull.color_params.debug_flags & CULL_VIEW_MAIN_CAMERA, CULL_VIEW_MAIN_CAMERA);
        assert_eq!(cull.shadow_views[0].params.debug_flags & CULL_VIEW_MAIN_CAMERA, 0);
        assert_eq!(cull.reflection_view.as_ref().unwrap().params.debug_flags & CULL_VIEW_MAIN_CAMERA, 0);
        assert_eq!(cull.sky_views[gi].params.debug_flags & CULL_VIEW_MAIN_CAMERA, 0);
        // A view without positive main-camera authority is also the fail-closed
        // answer for a future/unknown view: source remains, page does not.

        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        cull.begin_frame_stats(&mut enc);
        cull.dispatch(&mut enc);
        assert!(cull.color_active(), "color view must have its own live compute bind");
        cull.dispatch_color(&mut enc);
        for i in 0..2 { assert!(cull.dispatch_shadow(&mut enc, i)); }
        assert!(cull.dispatch_reflection(&mut enc));
        for i in 0..=gi { assert!(cull.dispatch_sky(&mut enc, i)); }
        queue.submit([enc.finish()]);

        let views = [
            ("main", cull.out_args().unwrap(), cull.out_records().unwrap(), cull.counter_buf(), true),
            ("color", cull.color_out_args().unwrap(), cull.color_out_records().unwrap(),
                cull.color_counter_buf(), true),
            ("solar", cull.shadow_out_args(0).unwrap(), cull.shadow_out_records(0).unwrap(),
                cull.shadow_counter_buf(0).unwrap(), false),
            ("local", cull.shadow_out_args(1).unwrap(), cull.shadow_out_records(1).unwrap(),
                cull.shadow_counter_buf(1).unwrap(), false),
            ("reflection", cull.reflection_out_args().unwrap(), cull.reflection_out_records().unwrap(),
                cull.reflection_counter_buf().unwrap(), false),
            ("sky0", cull.sky_out_args(0).unwrap(), cull.sky_out_records(0).unwrap(),
                cull.sky_counter_buf(0).unwrap(), false),
            ("sky1", cull.sky_out_args(1).unwrap(), cull.sky_out_records(1).unwrap(),
                cull.sky_counter_buf(1).unwrap(), false),
            ("gi", cull.sky_out_args(gi).unwrap(), cull.sky_out_records(gi).unwrap(),
                cull.sky_counter_buf(gi).unwrap(), false),
        ];
        let slots = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
        let target_word = (TARGET_MODEL_COUNT_BYTE_OFFSET / 4) as usize;
        for (name, args, records, counters, main) in views {
            let counts = read_u32s(&device, &queue, counters, target_word as u64 + 1);
            assert_eq!(counts[target_word], u32::from(main), "{name} page COUNT");
            assert_eq!(counts[STAT_BASE_WORD as usize], 2,
                "{name} must retain the control and exactly one complementary instance");
            let args = read_u32s(&device, &queue, args, slots * ARG_WORDS);
            let records = read_u32s(&device, &queue, records, slots * 2);
            let mut visible = Vec::new();
            for arg in args.chunks_exact(ARG_WORDS as usize).filter(|arg| arg[1] != 0) {
                let first = arg[4] as usize;
                for index in first..first + arg[1] as usize {
                    visible.push(records[index * 2]);
                }
            }
            visible.sort_unstable();
            let mut expected = if main { vec![control, page] } else { vec![control, source] };
            expected.sort_unstable();
            assert_eq!(visible, expected, "{name} emitted instance records");
            assert!(!visible.contains(&conflicting), "{name} conflicting bits must exclude all views");
        }
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
        cull.target_model_id = Some(model);

        cull.prepare(&device, &queue);
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut enc);
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
            raw_slot(under),
            "the surviving record is the instance under the camera"
        );
        let target_word = TARGET_MODEL_COUNT_BYTE_OFFSET / 4;
        let sky_count = read_u32s(
            &device, &queue, cull.sky_counter_buf(0).unwrap(), target_word + 1,
        );
        assert_eq!(
            sky_count[target_word as usize], 1,
            "standalone sky COUNT must see its selected surviving model"
        );

        // Keep the same live sky instance, select a model that has no instances,
        // and reset the frame stats exactly as the renderer does. A real dispatch
        // must now write zero, rather than reusing the previous positive word.
        let absent_model = cull.register_model(4.0, &lods, &sections, &materials);
        cull.target_model_id = Some(absent_model);
        cull.prepare(&device, &queue);
        let mut absent =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut absent);
        cull.dispatch_sky(&mut absent, 0);
        queue.submit(std::iter::once(absent.finish()));
        let absent_count = read_u32s(
            &device, &queue, cull.sky_counter_buf(0).unwrap(), target_word + 1,
        );
        assert_eq!(
            absent_count[target_word as usize], 0,
            "standalone sky COUNT must clear and reject an absent selected model"
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
        assert!(cull.shadow_count_slots.iter().all(Option::is_none),
            "the ordinary renderer must not allocate diagnostic staging");
        assert!(cull.local0_count_slots.is_none());
        assert!(!cull.arm_main_target_count(&device, 1, model, 0),
            "unknown source generation cannot produce an absence fact");
        assert!(cull.shadow_count_slots.iter().all(Option::is_none),
            "a rejected request must not allocate diagnostic staging");
        assert!(cull.local0_count_slots.is_none());
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
        cull.set_shadow_view_count(&device, 5);
        let mut empty_params = params;
        let far_eye = Vec3::new(10_000.0, 0.0, 10.0);
        empty_params.frustum = frustum_planes(proj *
            Mat4::look_at_rh(far_eye, Vec3::new(10_000.0, 0.0, 0.0), Vec3::Y));
        for cascade in 0..4 {
            cull.set_shadow_params(cascade, if cascade % 2 == 0 { params } else { empty_params });
        }
        cull.set_shadow_params(4, params); // daytime local tile 0 follows four solar views
        cull.set_sky_view_count(&device, super::super::sky_vis::DIRECTION_COUNT + 1);
        cull.set_sky_params(super::super::sky_vis::DIRECTION_COUNT, params);
        assert!(cull.arm_main_target_count(&device, 1, model, 17));
        cull.prepare(&device, &queue);

        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut enc);
        cull.dispatch(&mut enc);
        for cascade in 0..4 { assert!(cull.dispatch_shadow(&mut enc, cascade)); }
        assert!(cull.dispatch_shadow(&mut enc, 4));
        assert!(!cull.copy_shadow_target_count(&mut enc, 4),
            "a prepared GI view without its cull cannot publish a zero");
        cull.dispatch_sky(&mut enc, super::super::sky_vis::DIRECTION_COUNT);
        assert!(cull.copy_main_target_count(&mut enc));
        for cascade in 0..4 { assert!(cull.copy_shadow_target_count(&mut enc, cascade)); }
        assert!(cull.copy_local0_target_count(&mut enc, 4));
        assert!(cull.copy_shadow_target_count(&mut enc, 4));
        cull.resolve_stats(&mut enc);
        let commands = enc.finish();
        assert!(cull.main_target_count_before_submit(1));
        for cascade in 0..4 { assert!(cull.shadow_target_count_before_submit(1, cascade)); }
        assert!(cull.local0_target_count_before_submit(1));
        assert!(cull.shadow_target_count_before_submit(1, 4));
        queue.submit(std::iter::once(commands));
        assert!(cull.main_target_count_submitted(1));
        for cascade in 0..4 { assert!(cull.shadow_target_count_submitted(1, cascade)); }
        assert!(cull.local0_target_count_submitted(1));
        assert!(cull.shadow_target_count_submitted(1, 4));
        assert!(cull.main_target_count_commit(1));
        for cascade in 0..4 { assert!(cull.shadow_target_count_commit(1, cascade)); }
        assert!(cull.local0_target_count_commit(1));
        assert!(cull.shadow_target_count_commit(1, 4));
        // The harvest is deliberately non-blocking, so pump it until the ring drains rather than
        // asserting on the first (necessarily empty) attempt.
        for _ in 0..64 {
            cull.harvest_stats(&device);
            cull.harvest_main_target_count(&device);
            for cascade in 0..4 { cull.harvest_shadow_target_count(&device, cascade); }
            cull.harvest_local0_target_count(&device);
            cull.harvest_shadow_target_count(&device, 4);
            if cull.stats().valid &&
                cull.main_target_count_fact().status == MainCountStatus::Present &&
                (0..5).all(|view| cull.shadow_target_count_fact(view).status != MainCountStatus::Unknown) &&
                cull.local0_target_count_fact().status != MainCountStatus::Unknown {
                break;
            }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let stats = cull.stats();
        assert!(stats.valid, "a stats readback must land within 64 polls");
        let first = cull.main_target_count_fact();
        assert_eq!(first.identity.source_generation, 17);
        assert_eq!(first.identity.model_id, model);
        assert_eq!(first.status, MainCountStatus::Present);
        assert_eq!(first.count, 8);
        for cascade in 0..4 {
            let fact = cull.shadow_target_count_fact(cascade);
            assert_eq!(fact.identity, first.identity);
            let expected = if cascade % 2 == 0 { 8 } else { 0 };
            assert_eq!(fact.count, expected, "cascade {cascade} uses its own dispatched counter");
            assert_eq!(fact.status, if expected == 0 {
                MainCountStatus::Absent
            } else { MainCountStatus::Present });
        }
        let gi = cull.shadow_target_count_fact(4);
        assert_eq!(gi.identity, first.identity);
        assert_eq!(gi.status, MainCountStatus::Present);
        assert_eq!(gi.count, 8, "GI must read sky view 5's dispatched counter");
        let local = cull.local0_target_count_fact();
        assert_eq!(local.identity, first.identity);
        assert_eq!(local.status, MainCountStatus::Present);
        assert_eq!(local.count, 8, "local tile must read its own cull index 4");
        let m = stats.views[STATS_VIEW_MAIN];
        assert_eq!(
            m.instances, 8,
            "every surviving instance counted exactly once"
        );
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
        assert_eq!(m.target_model_count, 8, "target COUNT must come from GPU COUNT survivors");
        // A view that never dispatched must read zeros, not another view's numbers — this is the
        // stale-region trap the GPU timers document, in its counter form.
        let unused = stats.views[STATS_VIEW_REFLECTION];
        assert_eq!(unused.instances, 0);
        assert_eq!(unused.draws, 0);
        assert_eq!(unused.target_model_count, 0, "an undispatched view cannot inherit main COUNT");

        // Add an equally visible but different model. The aggregate survivor
        // count rises while the exact target must remain unchanged.
        let other = cull.register_model(1.0, &lods, &sections, &materials);
        let mut other_instance = mk(5.0);
        other_instance.model = other;
        cull.instance_add(other_instance);
        assert!(cull.arm_main_target_count(&device, 2, model, 29));
        assert_eq!(cull.shadow_target_count_fact(4).status, MainCountStatus::Unknown,
            "a new request cannot inherit a cached RSM count");
        assert_eq!(cull.local0_target_count_fact().status, MainCountStatus::Unknown,
            "a new request cannot inherit a cached local tile count");
        cull.set_shadow_view_count(&device, 1);
        cull.set_shadow_params(0, params); // night: tile 0 is cull index 0, not solar cascade 0
        cull.prepare(&device, &queue);
        let mut second = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut second);
        cull.dispatch(&mut second);
        assert!(cull.dispatch_shadow(&mut second, 0));
        assert!(cull.copy_main_target_count(&mut second));
        assert!(cull.copy_local0_target_count(&mut second, 0));
        cull.resolve_stats(&mut second);
        assert!(cull.main_target_count_before_submit(2));
        assert!(cull.local0_target_count_before_submit(2));
        queue.submit(std::iter::once(second.finish()));
        assert!(cull.main_target_count_submitted(2));
        assert!(cull.local0_target_count_submitted(2));
        assert!(cull.main_target_count_commit(2));
        assert!(cull.local0_target_count_commit(2));
        for _ in 0..64 {
            cull.harvest_stats(&device);
            cull.harvest_local0_target_count(&device);
            if cull.stats().views[STATS_VIEW_MAIN].instances == 9 &&
                cull.local0_target_count_fact().status == MainCountStatus::Present { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let second_main = cull.stats().views[STATS_VIEW_MAIN];
        assert_eq!(second_main.instances, 9, "decoy model must survive this view");
        assert_eq!(second_main.target_model_count, 8, "decoy must not alter exact model COUNT");
        assert_eq!(cull.local0_target_count_fact().count, 8,
            "night local tile 0 must read cull index 0");
        assert_eq!(cull.shadow_target_count_fact(0).status, MainCountStatus::Unknown,
            "night local tile 0 must never publish as solar cascade 0");
        cull.main_target_count_abort(2);
        cull.local0_target_count_abort(2);
        assert_eq!(cull.local0_target_count_fact().status, MainCountStatus::Unknown,
            "an aborted local tile copy cannot remain a present fact");
        // A real dispatched frame with nine other survivors must return a tagged zero.
        let absent_model = cull.register_model(1.0, &lods, &sections, &materials);
        assert!(cull.arm_main_target_count(&device, 3, absent_model, 30));
        cull.set_shadow_view_count(&device, 5);
        for cascade in 0..4 {
            cull.set_shadow_params(cascade, if cascade % 2 == 0 { params } else { empty_params });
        }
        cull.set_shadow_params(4, params);
        cull.prepare(&device, &queue);
        let mut third = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut third);
        cull.dispatch(&mut third);
        for cascade in 0..4 { assert!(cull.dispatch_shadow(&mut third, cascade)); }
        assert!(cull.dispatch_shadow(&mut third, 4));
        cull.dispatch_sky(&mut third, super::super::sky_vis::DIRECTION_COUNT);
        assert!(cull.copy_main_target_count(&mut third));
        for cascade in 0..4 { assert!(cull.copy_shadow_target_count(&mut third, cascade)); }
        assert!(cull.copy_local0_target_count(&mut third, 4));
        assert!(cull.copy_shadow_target_count(&mut third, 4));
        let commands = third.finish();
        assert!(cull.main_target_count_before_submit(3));
        for cascade in 0..4 { assert!(cull.shadow_target_count_before_submit(3, cascade)); }
        assert!(cull.local0_target_count_before_submit(3));
        assert!(cull.shadow_target_count_before_submit(3, 4));
        queue.submit(std::iter::once(commands));
        assert!(cull.main_target_count_submitted(3));
        for cascade in 0..4 { assert!(cull.shadow_target_count_submitted(3, cascade)); }
        assert!(cull.local0_target_count_submitted(3));
        assert!(cull.shadow_target_count_submitted(3, 4));
        assert!(cull.main_target_count_commit(3));
        for cascade in 0..3 { assert!(cull.shadow_target_count_commit(3, cascade)); }
        assert!(cull.shadow_target_count_commit(3, 4));
        assert!(cull.local0_target_count_commit(3));
        cull.shadow_target_count_abort(3, 3);
        assert_eq!(cull.main_target_count_fact().status, MainCountStatus::Unknown);
        for _ in 0..64 {
            cull.harvest_main_target_count(&device);
            for cascade in 0..4 { cull.harvest_shadow_target_count(&device, cascade); }
            cull.harvest_shadow_target_count(&device, 4);
            cull.harvest_local0_target_count(&device);
            if cull.main_target_count_fact().status == MainCountStatus::Absent &&
                (0..3).all(|cascade| cull.shadow_target_count_fact(cascade).status == MainCountStatus::Absent) &&
                cull.shadow_target_count_fact(4).status == MainCountStatus::Absent &&
                cull.local0_target_count_fact().status == MainCountStatus::Absent { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let absent = cull.main_target_count_fact();
        assert_eq!(absent.identity.model_id, absent_model);
        assert_eq!(absent.identity.source_generation, 30);
        assert_eq!(absent.status, MainCountStatus::Absent);
        assert_eq!(absent.count, 0);
        for cascade in 0..3 {
            let cascade_absent = cull.shadow_target_count_fact(cascade);
            assert_eq!(cascade_absent.identity, absent.identity);
            assert_eq!(cascade_absent.status, MainCountStatus::Absent);
            assert_eq!(cascade_absent.count, 0);
        }
        assert_eq!(cull.shadow_target_count_fact(3).status, MainCountStatus::Unknown,
            "aborted cascade must not inherit another cascade's absence");
        let gi_absent = cull.shadow_target_count_fact(4);
        assert_eq!(gi_absent.identity, absent.identity);
        assert_eq!(gi_absent.status, MainCountStatus::Absent);
        assert_eq!(gi_absent.count, 0, "GI zero needs its own current cull and commit");
        let local_absent = cull.local0_target_count_fact();
        assert_eq!(local_absent.identity, absent.identity);
        assert_eq!(local_absent.status, MainCountStatus::Absent);
        assert_eq!(local_absent.count, 0, "local zero needs its own fresh cull and commit");
        eprintln!("PASS exact target-model GPU COUNT: main and four isolated solar cascades");
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
        let far_raw: Vec<u32> = far_slots.iter().map(|h| raw_slot(*h)).collect();
        let near_raw: Vec<u32> = near_slots.iter().map(|h| raw_slot(*h)).collect();
        let base_far = check(a_far, 5, 3, 1, &far_raw);
        let base_near = check(a_near, 3, 0, 0, &near_raw);
        // Runs are disjoint (contiguous carving, no overlap).
        assert!(
            base_far + 5 <= base_near || base_near + 3 <= base_far,
            "per-section record runs must not overlap"
        );
    }
    #[test]
    fn table_upload_domains_select_real_closure_writes_and_growth() {
        let mut writes = Vec::new();
        let (clean, grew) = TableUploads::ALL.run(|domain| {
            writes.push(domain); domain == TableDomain::Material
        });
        assert_eq!(writes, TableDomain::ALL);
        assert!(grew, "material buffer growth must invalidate every shared view binding");
        writes.clear();
        assert!(!clean.run(|d| { writes.push(d); true }).1);
        assert!(writes.is_empty(), "clean frames do not call the upload body");
        for domain in TableDomain::ALL {
            let mut pending = TableUploads::NONE;
            pending.mark(domain); pending.mark(domain);
            writes.clear();
            let (next, grew) = pending.run(|d| { writes.push(d); true });
            assert_eq!(writes, vec![domain]);
            assert!(grew); assert_eq!(next, TableUploads::NONE);
        }
        let pending = TableUploads::ALL;
        let failure = std::panic::catch_unwind(|| pending.run(|_| panic!("upload failure")));
        assert!(failure.is_err());
        assert_eq!(pending, TableUploads::ALL, "uncompleted upload cannot publish clean state");
    }

    #[test]
    fn cull_table_domains_actual_mutations_and_gpu_content_match_legacy() {
        let Some((device, queue)) = headless() else {
            eprintln!("SKIP cull_table_domains_actual_mutations: no adapter/device"); return;
        };
        let mut cull = CullState::new(&device);
        assert_eq!(cull.table_uploads, TableUploads::NONE);
        assert!(!cull.table_uploads.run(|_| panic!("empty unregistered state has no uploads")).1);
        // Only test backing buffers gain COPY_SRC: normal resource usage is unchanged.
        for arr in [&mut cull.model_buf, &mut cull.lod_buf, &mut cull.section_buf,
                    &mut cull.section_mat_buf, &mut cull.crown_centre_buf] {
            arr.cap = 4096;
            arr.buf = Some(device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("table-domain-readback"), size: arr.cap,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            }));
        }
        let sections = [SectionGpu { first_index: 7, index_count: 3, base_vertex: 9, variant: 0 }];
        let lods = [LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 }];
        let mut material = SectionMaterialGpu::zeroed(); material.diffuse = [0.2, 0.4, 0.6, 1.0];
        let a = cull.register_model(2.0, &lods, &sections, &[material]);
        assert_eq!(cull.table_uploads, TableUploads(15),
                   "model append dirties only its four tables; prepare handles bootstrap");
        let b = cull.register_model(3.0, &lods, &sections, &[material]);
        cull.register_crown_centres(&[[1.0, 2.0, 3.0, 4.0]]);
        cull.prepare(&device, &queue);
        assert_eq!(cull.table_uploads, TableUploads::NONE);
        let snapshot = |c: &CullState| {
            vec![read_u32s(&device, &queue, c.model_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.models.as_slice()) / 4) as u64),
                 read_u32s(&device, &queue, c.lod_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.lods.as_slice()) / 4) as u64),
                 read_u32s(&device, &queue, c.section_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.sections.as_slice()) / 4) as u64),
                 read_u32s(&device, &queue, c.section_mat_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.section_materials.as_slice()) / 4) as u64),
                 read_u32s(&device, &queue, c.crown_centre_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.crown_centres.as_slice()) / 4) as u64)]
        };
        let untagged = snapshot(&cull);
        set_model_feedback_tag(&mut cull.models, &mut cull.table_uploads, b, 1);
        assert_eq!(cull.table_uploads, TableUploads(TableDomain::Model.bit()));
        cull.prepare(&device, &queue);
        let tagged = snapshot(&cull);
        assert_ne!(untagged[0], tagged[0]);
        for i in [1, 2, 3, 4] { assert_eq!(untagged[i], tagged[i]); }
        set_model_feedback_tag(&mut cull.models, &mut cull.table_uploads, b, 1);
        assert_eq!(cull.table_uploads, TableUploads::NONE);
        set_model_feedback_tag(&mut cull.models, &mut cull.table_uploads, b, 0);
        cull.prepare(&device, &queue);
        let before = snapshot(&cull);
        cull.retire_model(a);
        assert_eq!(cull.table_uploads, TableUploads(TableDomain::Lod.bit()));
        cull.prepare(&device, &queue);
        let retired = snapshot(&cull);
        assert_ne!(before[1], retired[1]);
        for i in [0, 2, 3, 4] { assert_eq!(before[i], retired[i]); }
        assert_eq!(cull.lods[cull.models[b as usize].lod_base as usize].section_count, 1);
        let mut changed = cull.sections.clone(); changed[1].base_vertex = 71;
        cull.set_sections(&changed);
        assert_eq!(cull.table_uploads, TableUploads(TableDomain::Section.bit()));
        cull.prepare(&device, &queue);
        cull.set_sections(&changed);
        assert_eq!(cull.table_uploads, TableUploads::NONE, "same addresses cause no upload");
        cull.register_crown_centres(&[[5.0, 6.0, 7.0, 8.0]]);
        assert_eq!(cull.table_uploads, TableUploads(TableDomain::Crown.bit()));
        cull.prepare(&device, &queue);
        // Prove an actual unchanged Crown upload is skipped, not just a mask assertion:
        // seed different GPU bytes without changing the CPU crown mirror. Registering a
        // non-crown model must leave these exact GPU bytes and capacity untouched.
        let cpu_crowns = cull.crown_centres.clone();
        let crown_capacity = cull.crown_centre_buf.cap;
        let sentinel = [[91.0f32, 92.0, 93.0, 94.0]];
        queue.write_buffer(cull.crown_centre_buf.buf.as_ref().unwrap(), 0, bytemuck::cast_slice(&sentinel));
        let seeded_crown = snapshot(&cull)[4].clone();
        assert_ne!(seeded_crown, bytemuck::cast_slice::<[f32;4],u32>(&cpu_crowns));
        cull.register_model(5.0, &lods, &sections, &[material]);
        let mut selected_writes = Vec::new();
        cull.table_uploads.run(|domain| { selected_writes.push(domain); false });
        assert_eq!(selected_writes, vec![TableDomain::Model, TableDomain::Lod,
                                       TableDomain::Section, TableDomain::Material]);
        cull.prepare(&device, &queue);
        assert_eq!(snapshot(&cull)[4], seeded_crown,
                   "actual production prepare must not overwrite unchanged Crown GPU data");
        assert_eq!(cull.crown_centres, cpu_crowns);
        assert_eq!(cull.crown_centre_buf.cap, crown_capacity);
        // Append Crown FIRST, then a model in the same pending batch. Registration must
        // preserve that pending domain and upload all authored centres, including the new one.
        cull.register_crown_centres(&[[9.0, 10.0, 11.0, 12.0]]);
        cull.register_model(6.0, &lods, &sections, &[material]);
        assert_eq!(cull.table_uploads, TableUploads::ALL);
        cull.prepare(&device, &queue);
        assert_eq!(snapshot(&cull)[4], bytemuck::cast_slice::<[f32;4],u32>(&cull.crown_centres));
        assert_eq!(cull.crown_centre_buf.cap, crown_capacity);
        let optimized = snapshot(&cull);
        // Original path: all five writes regardless of which mirror changed.
        upload_slice(&device, &queue, &mut cull.model_buf, &cull.models);
        upload_slice(&device, &queue, &mut cull.lod_buf, &cull.lods);
        upload_slice(&device, &queue, &mut cull.section_buf, &cull.sections);
        upload_slice(&device, &queue, &mut cull.section_mat_buf, &cull.section_materials);
        upload_slice(&device, &queue, &mut cull.crown_centre_buf, &cull.crown_centres);
        assert_eq!(optimized, snapshot(&cull));
        eprintln!("PASS cull_table_domains_actual_mutations: actual GPU upload/readback equals legacy all-table writes");
    }

    // Read the actual STORAGE-only production buffer via a tiny GPU copy kernel;
    // no production COPY_SRC usage or allocation contract changes are needed.
    fn read_table_storage_words(device: &wgpu::Device, queue: &wgpu::Queue,
                                source: &wgpu::Buffer, words: u64) -> Vec<u32> {
        if words == 0 { return Vec::new(); }
        let output = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("table-bootstrap-output"), size: words * 4,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("table-bootstrap-copy"),
            source: wgpu::ShaderSource::Wgsl(std::borrow::Cow::Borrowed(
                "@group(0) @binding(0) var<storage, read> source: array<u32>;
                 @group(0) @binding(1) var<storage, read_write> output: array<u32>;
                 @compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
                 if id.x < arrayLength(&output) { output[id.x] = source[id.x]; } }")),
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("table-bootstrap-copy"), layout: None, module: &shader,
            entry_point: Some("main"), compilation_options: Default::default(), cache: None,
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None, layout: &pipeline.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: source.as_entire_binding() },
                       wgpu::BindGroupEntry { binding: 1, resource: output.as_entire_binding() }],
        });
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: None, timestamp_writes: None });
            pass.set_pipeline(&pipeline); pass.set_bind_group(0, &bind, &[]);
            pass.dispatch_workgroups(words.div_ceil(64) as u32, 1, 1);
        }
        queue.submit([encoder.finish()]);
        read_u32s(device, queue, &output, words)
    }

    #[test]
    fn cull_table_domains_crown_and_section_first_initialize_all_tables() {
        let Some((device, queue)) = headless() else {
            eprintln!("SKIP cull_table_domains_bootstrap: no adapter/device"); return;
        };
        // All original first-writer cases, plus first registration with NO backing tables.
        for first_writer in 0..3 {
            let mut cull = CullState::new(&device);
            assert_eq!(cull.table_uploads, TableUploads::NONE);
            assert!(cull.model_buf.buf.is_none() && cull.section_mat_buf.buf.is_none());
            let section = SectionGpu { first_index: 8, index_count: 3, base_vertex: 19, variant: 0 };
            if first_writer == 0 { cull.register_crown_centres(&[[1.0, 2.0, 3.0, 4.0]]); }
            else if first_writer == 1 { cull.set_sections(&[section]); }
            else {
                let lod = LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 };
                let mut material = SectionMaterialGpu::zeroed(); material.diffuse = [0.1, 0.2, 0.3, 1.0];
                cull.register_model(4.0, &[lod], &[section], &[material]);
                assert_eq!(cull.table_uploads, TableUploads(15));
            }
            // Actual production prepare promotes every missing-buffer first writer to ALL.
            cull.prepare(&device, &queue);
            assert!(cull.model_buf.buf.is_some() && cull.lod_buf.buf.is_some() &&
                    cull.section_buf.buf.is_some() && cull.section_mat_buf.buf.is_some() &&
                    cull.crown_centre_buf.buf.is_some());
            assert!(cull.bind.is_some(), "first writer preserves existing cull bind construction");
            let lod = LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 };
            let mut material = SectionMaterialGpu::zeroed(); material.diffuse = [0.3, 0.6, 0.9, 1.0];
            cull.register_model(7.0, &[lod], &[section], &[material]);
            cull.prepare(&device, &queue);
            let capture = |c: &CullState| {
                vec![read_table_storage_words(&device, &queue, c.model_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.models.as_slice()) / 4) as u64),
                     read_table_storage_words(&device, &queue, c.lod_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.lods.as_slice()) / 4) as u64),
                     read_table_storage_words(&device, &queue, c.section_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.sections.as_slice()) / 4) as u64),
                     read_table_storage_words(&device, &queue, c.section_mat_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.section_materials.as_slice()) / 4) as u64),
                     read_table_storage_words(&device, &queue, c.crown_centre_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(c.crown_centres.as_slice()) / 4) as u64)]
            };
            let optimized = capture(&cull);
            assert!(!optimized[0].is_empty() && !optimized[3].is_empty());
            upload_slice(&device, &queue, &mut cull.model_buf, &cull.models);
            upload_slice(&device, &queue, &mut cull.lod_buf, &cull.lods);
            upload_slice(&device, &queue, &mut cull.section_buf, &cull.sections);
            upload_slice(&device, &queue, &mut cull.section_mat_buf, &cull.section_materials);
            upload_slice(&device, &queue, &mut cull.crown_centre_buf, &cull.crown_centres);
            assert_eq!(optimized, capture(&cull));
        }
        eprintln!("PASS cull_table_domains_bootstrap: crown-first/section-first actual GPU readback matches wholesale initialization");
    }

    #[test]
    #[allow(deprecated)]
    fn retired_model_neighbor_and_mesh_reuse_draw_only_live_models_all_three_views() {
        use super::super::{Mesh, MeshKey, GpuSectionSrc, resolve_registered_section};
        use super::super::pool::GeometryPool;
        use slotmap::{Key, SlotMap};
        let Some((device, queue)) = headless() else {
            eprintln!("SKIP retired_model_three_views: no adapter/device"); return;
        };
        let mut pool = GeometryPool::new(&device);
        let mut meshes: SlotMap<MeshKey, Mesh> = SlotMap::with_key();
        let mut verts = [crate::ffi::WgrMeshVertex::zeroed(); 3];
        verts[0].pos = Vec3::new(-0.5, 0.0, 0.0);
        verts[1].pos = Vec3::new(0.5, 0.0, 0.0);
        verts[2].pos = Vec3::new(0.0, 0.5, 0.0);
        let insert = |pool: &mut GeometryPool, meshes: &mut SlotMap<MeshKey, Mesh>| {
            let alloc = pool.alloc(&device, &queue, &verts, &[0, 1, 2]).unwrap();
            meshes.insert(Mesh { alloc, index_count: 3, vert_count: 3, skin: None,
                                 aabb_min: [-0.5, 0.0, 0.0], aabb_max: [0.5, 0.5, 0.0] })
        };
        let a_mesh = insert(&mut pool, &mut meshes);
        let b_mesh = insert(&mut pool, &mut meshes);
        let source = |key: MeshKey| GpuSectionSrc { mesh: key.data().as_ffi(),
                                                  index_begin: 0, index_count: 3, variant: 0 };
        let mut sources = vec![source(a_mesh), source(b_mesh)];
        let mut cull = CullState::new(&device);
        cull.variant_capacity = 16; // bounded test output; production capacity is unchanged
        let lod = LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 };
        let material = SectionMaterialGpu::zeroed();
        let a = cull.register_model(1.0, &[lod], &[resolve_registered_section(&meshes, &sources[0])], &[material]);
        let b = cull.register_model(1.0, &[lod], &[resolve_registered_section(&meshes, &sources[1])], &[material]);
        let instance = |model| InstanceGpu { world: Mat4::IDENTITY.to_cols_array(), center: [0.0, 0.0, 0.0, 1.0],
            model, flags: 0, cull_radius: 0, _pad: 0, conform0: [0.0; 4], conform1: [0.0; 4], conform2: [0.0; 4] };
        let a_instance = cull.instance_add(instance(a));
        cull.instance_add(instance(b));
        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_rh(60f32.to_radians(), 1.0, 0.1, 1000.0);
        let mut main = CullParamsGpu::zeroed();
        main.frustum = frustum_planes(proj * view);
        main.cam_pos = [eye.x, eye.y, eye.z, 0.0]; main.objects_z2 = 1.0e6;
        main.lod_scale = 1.0; main.lod_inv_width = 1.0; main.pixel_limit = 0.0;
        cull.set_params(main);
        let inputs = CullInputs { objects_z: 1000.0, lod_scale: 1.0, lod_inv_width: 1.0, pixel_limit: 0.0 };
        cull.set_shadow_view_count(&device, 1);
        cull.set_shadow_params(0, params_from_shadow_cascade(proj * view, eye, inputs));
        cull.set_hiz(Some(const_hiz(&device, &queue, 64, 64, 0.0)));
        cull.set_color_params(params_from_camera_occlude(view, proj, eye, inputs, [64.0, 64.0], 7, true));
        let run_and_check = |cull: &mut CullState, expected: &[(u32, SectionGpu)]| {
            cull.prepare(&device, &queue);
            let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
            cull.dispatch(&mut encoder); cull.dispatch_shadow(&mut encoder, 0); cull.dispatch_color(&mut encoder);
            queue.submit([encoder.finish()]);
            let views = [("main", cull.out_args().unwrap(), cull.out_records().unwrap()),
                         ("shadow", cull.shadow_out_args(0).unwrap(), cull.shadow_out_records(0).unwrap()),
                         ("color", cull.color_out_args().unwrap(), cull.color_out_records().unwrap())];
            let slots = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
            for (label, args, records) in views {
                let args = read_u32s(&device, &queue, args, slots * super::ARG_WORDS as u64);
                let records = read_u32s(&device, &queue, records, slots * 2);
                let mut seen = Vec::new();
                for draw in args.chunks_exact(super::ARG_WORDS as usize).filter(|row| row[1] != 0) {
                    assert_eq!(draw[1], 1, "{label}: one test instance per model section");
                    let record = draw[4] as usize;
                    let instance_slot = records[record * 2] as usize;
                    let model = cull.instances.as_slice()[instance_slot].model;
                    let (_, section) = expected.iter().find(|(id, _)| *id == model)
                        .unwrap_or_else(|| panic!("{label}: unexpected stale model {model}"));
                    assert_eq!(draw[0], section.index_count, "{label}: index count");
                    assert_eq!(draw[2], section.first_index, "{label}: current pool index address");
                    assert_eq!(draw[3], section.base_vertex, "{label}: current pool vertex address");
                    let global_section = records[record * 2 + 1] as usize;
                    assert!(cull.sections[global_section] == *section, "{label}: exact current section row");
                    seen.push(model);
                }
                seen.sort_unstable();
                let mut expected_models: Vec<_> = expected.iter().map(|(id, _)| *id).collect();
                expected_models.sort_unstable();
                assert_eq!(seen, expected_models, "{label}: each live model draws once and stale A never draws");
            }
        };
        let a_section = resolve_registered_section(&meshes, &sources[0]);
        let b_section = resolve_registered_section(&meshes, &sources[1]);
        run_and_check(&mut cull, &[(a, a_section), (b, b_section)]);
        let epoch = cull.instance_epoch();
        cull.retire_model(a);
        assert_eq!(cull.instance_epoch(), epoch.wrapping_add(1),
                   "retained A retirement must invalidate cached scene content without removing A");
        cull.retire_model(a);
        cull.retire_model(u32::MAX);
        assert_eq!(cull.instance_epoch(), epoch.wrapping_add(1), "repeat/unknown retire is inert");
        let old = meshes.remove(a_mesh).unwrap();
        pool.free(&old.alloc, old.vert_count, old.index_count);
        let resolved: Vec<_> = sources.iter().map(|s| resolve_registered_section(&meshes, s)).collect();
        assert_eq!(resolved[0].index_count, 0);
        cull.set_sections(&resolved);
        run_and_check(&mut cull, &[(b, b_section)]);
        // Keep the stale A instance throughout; normal instance cleanup is not the reason it disappears.
        assert_eq!(cull.instances.as_slice()[raw_slot(a_instance) as usize].model, a);
        let pool_generation = pool.generation();
        let c_mesh = insert(&mut pool, &mut meshes);
        assert_eq!(a_mesh.data().as_ffi() as u32, c_mesh.data().as_ffi() as u32, "actual SlotMap slot reuse");
        assert_ne!(a_mesh.data().as_ffi(), c_mesh.data().as_ffi(), "different full mesh generation");
        assert_eq!(pool.generation(), pool_generation, "recreation must be safe without pool relocation");
        assert_eq!(resolve_registered_section(&meshes, &sources[0]).index_count, 0,
                   "the old source must not bind a new-generation mesh even at the same pool address");
        sources.push(source(c_mesh));
        let c_section = resolve_registered_section(&meshes, sources.last().unwrap());
        let c = cull.register_model(1.0, &[lod], &[c_section], &[material]);
        assert!(a != b && b != c && a != c, "stable model IDs cannot alias retired A");
        cull.instance_add(instance(c));
        let resolved: Vec<_> = sources.iter().map(|s| resolve_registered_section(&meshes, s)).collect();
        cull.set_sections(&resolved);
        run_and_check(&mut cull, &[(b, b_section), (c, c_section)]);
        eprintln!("PASS retired_model_three_views: actual pool/full-generation resolver and main/shadow/color GPU readback preserve live B/C with stale A");
    }

    #[test]
    #[allow(deprecated)]
    fn reused_section_span_with_stale_model_draws_only_live_neighbors_all_three_views() {
        use super::super::{Mesh, MeshKey, GpuSectionSrc, resolve_registered_section};
        use super::super::pool::GeometryPool;
        use slotmap::{Key, SlotMap};
        let Some((device, queue)) = headless() else {
            eprintln!("SKIP reused_section_three_views: no adapter/device"); return;
        };
        let mut pool = GeometryPool::new(&device);
        let mut meshes: SlotMap<MeshKey, Mesh> = SlotMap::with_key();
        let mut verts = [crate::ffi::WgrMeshVertex::zeroed(); 3];
        verts[0].pos = Vec3::new(-0.5, 0.0, 0.0);
        verts[1].pos = Vec3::new(0.5, 0.0, 0.0);
        verts[2].pos = Vec3::new(0.0, 0.5, 0.0);
        let insert = |pool: &mut GeometryPool, meshes: &mut SlotMap<MeshKey, Mesh>| {
            let alloc = pool.alloc(&device, &queue, &verts, &[0, 1, 2]).unwrap();
            meshes.insert(Mesh { alloc, index_count: 3, vert_count: 3, skin: None,
                                 aabb_min: [-0.5, 0.0, 0.0], aabb_max: [0.5, 0.5, 0.0] })
        };
        let a_mesh = insert(&mut pool, &mut meshes);
        let b_mesh = insert(&mut pool, &mut meshes);
        let source = |key: MeshKey| GpuSectionSrc { mesh: key.data().as_ffi(),
                                                  index_begin: 0, index_count: 3, variant: 0 };
        let old_source = source(a_mesh); // immutable literal survives clearing the source table
        let mut sources = vec![old_source, source(b_mesh)];
        let mut cull = CullState::new(&device);
        cull.section_spans = Some(Box::new(super::super::section_span_pool::SectionSpanPool::new()));
        cull.variant_capacity = 16; // bounded test output; production capacity is unchanged
        let lod = LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 };
        let mut mat_a = SectionMaterialGpu::zeroed(); mat_a.emissive = [1.0, 0.1, 0.2, 1.0];
        let mut mat_b = SectionMaterialGpu::zeroed(); mat_b.emissive = [2.0, 0.3, 0.4, 1.0];
        let mut mat_c = SectionMaterialGpu::zeroed(); mat_c.emissive = [3.0, 0.5, 0.6, 1.0];
        let a = cull.register_model(1.0, &[lod], &[resolve_registered_section(&meshes, &sources[0])], &[mat_a]);
        let b = cull.register_model(1.0, &[lod], &[resolve_registered_section(&meshes, &sources[1])], &[mat_b]);
        assert_eq!(bytemuck::bytes_of(&cull.section_materials[0]), bytemuck::bytes_of(&mat_a));
        let instance = |model| InstanceGpu { world: Mat4::IDENTITY.to_cols_array(), center: [0.0, 0.0, 0.0, 1.0],
            model, flags: 0, cull_radius: 0, _pad: 0, conform0: [0.0; 4], conform1: [0.0; 4], conform2: [0.0; 4] };
        let a_instance = cull.instance_add(instance(a));
        cull.instance_add(instance(b));
        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_rh(60f32.to_radians(), 1.0, 0.1, 1000.0);
        let mut main = CullParamsGpu::zeroed();
        main.frustum = frustum_planes(proj * view);
        main.cam_pos = [eye.x, eye.y, eye.z, 0.0]; main.objects_z2 = 1.0e6;
        main.lod_scale = 1.0; main.lod_inv_width = 1.0; main.pixel_limit = 0.0;
        cull.set_params(main);
        let inputs = CullInputs { objects_z: 1000.0, lod_scale: 1.0, lod_inv_width: 1.0, pixel_limit: 0.0 };
        cull.set_shadow_view_count(&device, 1);
        cull.set_shadow_params(0, params_from_shadow_cascade(proj * view, eye, inputs));
        cull.set_hiz(Some(const_hiz(&device, &queue, 64, 64, 0.0)));
        cull.set_color_params(params_from_camera_occlude(view, proj, eye, inputs, [64.0, 64.0], 7, true));
        let run_and_check = |cull: &mut CullState, expected: &[(u32, SectionGpu)]| {
            assert_eq!(bytemuck::bytes_of(&cull.section_materials[1]), bytemuck::bytes_of(&mat_b),
                       "live B material is unchanged through all coherent updates");
            cull.prepare(&device, &queue);
            // Production storage upload is read through a compute copy: the table itself
            // intentionally has no COPY_SRC usage. Compare the full material table exactly.
            let uploaded = read_table_storage_words(&device, &queue,
                cull.section_mat_buf.buf.as_ref().unwrap(),
                (std::mem::size_of_val(cull.section_materials.as_slice()) / 4) as u64);
            let expected_materials: &[u32] = bytemuck::cast_slice(cull.section_materials.as_slice());
            assert_eq!(uploaded.as_slice(), expected_materials, "actual GPU material row parity");
            let stride = std::mem::size_of::<SectionMaterialGpu>() / 4;
            let expected_b: &[u32] = bytemuck::cast_slice(std::slice::from_ref(&mat_b));
            assert_eq!(&uploaded[stride..2 * stride], expected_b, "actual GPU live B sentinel survives");
            let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
            cull.dispatch(&mut encoder); cull.dispatch_shadow(&mut encoder, 0); cull.dispatch_color(&mut encoder);
            queue.submit([encoder.finish()]);
            let views = [("main", cull.out_args().unwrap(), cull.out_records().unwrap()),
                         ("shadow", cull.shadow_out_args(0).unwrap(), cull.shadow_out_records(0).unwrap()),
                         ("color", cull.color_out_args().unwrap(), cull.color_out_records().unwrap())];
            let slots = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
            for (label, args, records) in views {
                let args = read_u32s(&device, &queue, args, slots * super::ARG_WORDS as u64);
                let records = read_u32s(&device, &queue, records, slots * 2);
                let mut seen = Vec::new();
                for draw in args.chunks_exact(super::ARG_WORDS as usize).filter(|row| row[1] != 0) {
                    assert_eq!(draw[1], 1, "{label}: one test instance per model section");
                    let record = draw[4] as usize;
                    let instance_slot = records[record * 2] as usize;
                    let model = cull.instances.as_slice()[instance_slot].model;
                    let (_, section) = expected.iter().find(|(id, _)| *id == model)
                        .unwrap_or_else(|| panic!("{label}: unexpected stale model {model}"));
                    assert_eq!(draw[0], section.index_count, "{label}: index count");
                    assert_eq!(draw[2], section.first_index, "{label}: current pool index address");
                    assert_eq!(draw[3], section.base_vertex, "{label}: current pool vertex address");
                    let global_section = records[record * 2 + 1] as usize;
                    assert!(cull.sections[global_section] == *section, "{label}: exact current section row");
                    seen.push(model);
                }
                seen.sort_unstable();
                let mut expected_models: Vec<_> = expected.iter().map(|(id, _)| *id).collect();
                expected_models.sort_unstable();
                assert_eq!(seen, expected_models, "{label}: each live model draws once and stale A never draws");
            }
        };
        let a_section = resolve_registered_section(&meshes, &sources[0]);
        let b_section = resolve_registered_section(&meshes, &sources[1]);
        run_and_check(&mut cull, &[(a, a_section), (b, b_section)]);
        let epoch = cull.instance_epoch();
        cull.retire_model(a);
        assert_eq!(cull.instance_epoch(), epoch.wrapping_add(1),
                   "retained A retirement must invalidate cached scene content without removing A");
        cull.retire_model(a);
        cull.retire_model(u32::MAX);
        assert_eq!(cull.instance_epoch(), epoch.wrapping_add(1), "repeat/unknown retire is inert");
        assert!(cull.clear_retired_sections(|span| {
            sources[span.base..span.base + span.count].fill(GpuSectionSrc {
                mesh: 0, index_begin: 0, index_count: 0, variant: 0 });
        }));
        assert_eq!(cull.sections[0].index_count, 0);
        assert_eq!(bytemuck::bytes_of(&cull.section_materials[0]), bytemuck::bytes_of(&SectionMaterialGpu::zeroed()));
        let old = meshes.remove(a_mesh).unwrap();
        pool.free(&old.alloc, old.vert_count, old.index_count);
        let resolved: Vec<_> = sources.iter().map(|s| resolve_registered_section(&meshes, s)).collect();
        assert_eq!(resolved[0].index_count, 0);
        cull.set_sections(&resolved);
        run_and_check(&mut cull, &[(b, b_section)]);
        // Keep the stale A instance throughout; normal instance cleanup is not the reason it disappears.
        assert_eq!(cull.instances.as_slice()[raw_slot(a_instance) as usize].model, a);
        let pool_generation = pool.generation();
        let c_mesh = insert(&mut pool, &mut meshes);
        assert_eq!(a_mesh.data().as_ffi() as u32, c_mesh.data().as_ffi() as u32, "actual SlotMap slot reuse");
        assert_ne!(a_mesh.data().as_ffi(), c_mesh.data().as_ffi(), "different full mesh generation");
        assert_eq!(pool.generation(), pool_generation, "recreation must be safe without pool relocation");
        assert_eq!(old_source.mesh, a_mesh.data().as_ffi());
        assert_ne!(old_source.mesh, c_mesh.data().as_ffi());
        assert_eq!(resolve_registered_section(&meshes, &old_source).index_count, 0,
                   "literal old full-generation source must not bind recreated C at the same slot/address");
        let c_source = source(c_mesh);
        let c_section = resolve_registered_section(&meshes, &c_source);
        let (c, span) = cull.register_model_span(1.0, &[lod], &[c_section], &[mat_c]);
        assert_eq!(span.base, 0, "reuse retired A section; immutable A model stays empty");
        super::super::section_span_pool::write_span(&mut sources, span, &[c_source]);
        assert_eq!(sources.len(), 2); assert_eq!(cull.sections.len(), 2);
        assert!(cull.sections[1] == b_section, "live B retained its exact row");
        assert_eq!(bytemuck::bytes_of(&cull.section_materials[0]), bytemuck::bytes_of(&mat_c));
        assert_eq!(bytemuck::bytes_of(&cull.section_materials[1]), bytemuck::bytes_of(&mat_b));
        assert!(a != b && b != c && a != c, "stable model IDs cannot alias retired A");
        cull.instance_add(instance(c));
        let resolved: Vec<_> = sources.iter().map(|s| resolve_registered_section(&meshes, s)).collect();
        cull.set_sections(&resolved);
        run_and_check(&mut cull, &[(b, b_section), (c, c_section)]);
        eprintln!("PASS reused_section_three_views: actual pool/full-generation resolver and main/shadow/color GPU readback preserve live B/C with stale A");
    }

    #[test]
    #[allow(deprecated)]
    fn reused_lod_span_sentinel_with_stale_and_empty_models_all_three_views() {
        use super::super::{Mesh, MeshKey, GpuSectionSrc, resolve_registered_section};
        use super::super::pool::GeometryPool;
        use slotmap::{Key, SlotMap};
        let Some((device, queue)) = headless() else {
            eprintln!("SKIP reused_lod_three_views: no adapter/device"); return;
        };
        let mut pool = GeometryPool::new(&device);
        let mut meshes: SlotMap<MeshKey, Mesh> = SlotMap::with_key();
        let mut verts = [crate::ffi::WgrMeshVertex::zeroed(); 3];
        verts[0].pos = Vec3::new(-0.5, 0.0, 0.0);
        verts[1].pos = Vec3::new(0.5, 0.0, 0.0);
        verts[2].pos = Vec3::new(0.0, 0.5, 0.0);
        let insert = |pool: &mut GeometryPool, meshes: &mut SlotMap<MeshKey, Mesh>| {
            let alloc = pool.alloc(&device, &queue, &verts, &[0, 1, 2]).unwrap();
            meshes.insert(Mesh { alloc, index_count: 3, vert_count: 3, skin: None,
                                 aabb_min: [-0.5, 0.0, 0.0], aabb_max: [0.5, 0.5, 0.0] })
        };
        let a_mesh = insert(&mut pool, &mut meshes);
        let b_mesh = insert(&mut pool, &mut meshes);
        let source = |key: MeshKey| GpuSectionSrc { mesh: key.data().as_ffi(),
                                                  index_begin: 0, index_count: 3, variant: 0 };
        let old_source = source(a_mesh); // immutable literal survives clearing the source table
        let mut sources = vec![old_source, source(b_mesh)];
        let mut cull = CullState::new(&device);
        cull.section_spans = None; // LOD flag is independent of SECTION reuse.
        cull.lod_spans = Some(Box::new(super::super::section_span_pool::SectionSpanPool::new_table("LOD")));
        if cull.lods.is_empty() { cull.lods.push(LodGpu::zeroed()); }
        cull.variant_capacity = 16; // bounded test output; production capacity is unchanged
        let lod = LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 };
        let mut mat_a = SectionMaterialGpu::zeroed(); mat_a.emissive = [1.0, 0.1, 0.2, 1.0];
        let mut mat_b = SectionMaterialGpu::zeroed(); mat_b.emissive = [2.0, 0.3, 0.4, 1.0];
        let mut mat_c = SectionMaterialGpu::zeroed(); mat_c.emissive = [3.0, 0.5, 0.6, 1.0];
        let a = cull.register_model(1.0, &[lod], &[resolve_registered_section(&meshes, &sources[0])], &[mat_a]);
        let b = cull.register_model(1.0, &[lod], &[resolve_registered_section(&meshes, &sources[1])], &[mat_b]);
        assert_eq!(bytemuck::bytes_of(&cull.section_materials[0]), bytemuck::bytes_of(&mat_a));
        let instance = |model| InstanceGpu { world: Mat4::IDENTITY.to_cols_array(), center: [0.0, 0.0, 0.0, 1.0],
            model, flags: 0, cull_radius: 0, _pad: 0, conform0: [0.0; 4], conform1: [0.0; 4], conform2: [0.0; 4] };
        let a_instance = cull.instance_add(instance(a));
        cull.instance_add(instance(b));
        let empty = cull.register_model(1.0, &[], &[], &[]);
        cull.instance_add(instance(empty));
        assert_eq!(cull.models[empty as usize].lod_base, 0);
        assert_eq!(cull.models[empty as usize].lod_count, 0);
        let old_a_lod_base = cull.models[a as usize].lod_base;
        let b_lod_before = bytemuck::bytes_of(&cull.lods[cull.models[b as usize].lod_base as usize]).to_vec();
        // The same retirement API also isolates relative diagnostic identity from reused payload.
        let mut feedback=super::super::lod_feedback::LodFeedback::new(&device);
        feedback.start(101,1,&[crate::ffi::WgrLodDemandRow {
            model_id:a,lod_count:1,lod_mask:1,state:0,..Default::default()}]);
        cull.lod_feedback=Some(RefCell::new(Box::new(feedback)));
        set_model_feedback_tag(&mut cull.models,&mut cull.table_uploads,a,1);
        let eye = Vec3::new(0.0, 0.0, 10.0);
        let mut view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        view.w_axis = Vec4::new(0.0, 0.0, 0.0, 1.0);
        let proj = Mat4::perspective_rh(60f32.to_radians(), 1.0, 0.1, 1000.0);
        let mut main = CullParamsGpu::zeroed();
        main.frustum = frustum_planes(proj * view);
        main.cam_pos = [eye.x, eye.y, eye.z, 0.0]; main.objects_z2 = 1.0e6;
        main.lod_scale = 1.0; main.lod_inv_width = 1.0; main.pixel_limit = 0.0;
        cull.set_params(main);
        let inputs = CullInputs { objects_z: 1000.0, lod_scale: 1.0, lod_inv_width: 1.0, pixel_limit: 0.0 };
        cull.set_shadow_view_count(&device, 1);
        cull.set_shadow_params(0, params_from_shadow_cascade(proj * view, eye, inputs));
        cull.set_hiz(Some(const_hiz(&device, &queue, 64, 64, 0.0)));
        cull.set_color_params(params_from_camera_occlude(view, proj, eye, inputs, [64.0, 64.0], 7, true));
        let run_and_check = |cull: &mut CullState, expected: &[(u32, SectionGpu)]| {
            assert_eq!(bytemuck::bytes_of(&cull.section_materials[1]), bytemuck::bytes_of(&mat_b),
                       "live B material is unchanged through all coherent updates");
            cull.prepare(&device, &queue);
            // Production storage upload is read through a compute copy: the table itself
            // intentionally has no COPY_SRC usage. Compare the full material table exactly.
            let uploaded = read_table_storage_words(&device, &queue,
                cull.section_mat_buf.buf.as_ref().unwrap(),
                (std::mem::size_of_val(cull.section_materials.as_slice()) / 4) as u64);
            let expected_materials: &[u32] = bytemuck::cast_slice(cull.section_materials.as_slice());
            assert_eq!(uploaded.as_slice(), expected_materials, "actual GPU material row parity");
            let stride = std::mem::size_of::<SectionMaterialGpu>() / 4;
            let expected_b: &[u32] = bytemuck::cast_slice(std::slice::from_ref(&mat_b));
            assert_eq!(&uploaded[stride..2 * stride], expected_b, "actual GPU live B sentinel survives");
            assert_eq!(bytemuck::bytes_of(&cull.lods[0]), bytemuck::bytes_of(&LodGpu::zeroed()),
                       "permanent sentinel never belongs to a recycled range");
            for (buffer, words, expected) in [
                (cull.model_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(cull.models.as_slice()) / 4) as u64,
                    bytemuck::cast_slice::<ModelGpu,u32>(cull.models.as_slice())),
                (cull.lod_buf.buf.as_ref().unwrap(), (std::mem::size_of_val(cull.lods.as_slice()) / 4) as u64,
                    bytemuck::cast_slice::<LodGpu,u32>(cull.lods.as_slice()))] {
                assert_eq!(read_table_storage_words(&device, &queue, buffer, words).as_slice(), expected,
                    "actual GPU model+LOD publication matches same owner cut");
            }
            let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
            cull.dispatch(&mut encoder); cull.dispatch_shadow(&mut encoder, 0); cull.dispatch_color(&mut encoder);
            queue.submit([encoder.finish()]);
            let views = [("main", cull.out_args().unwrap(), cull.out_records().unwrap()),
                         ("shadow", cull.shadow_out_args(0).unwrap(), cull.shadow_out_records(0).unwrap()),
                         ("color", cull.color_out_args().unwrap(), cull.color_out_records().unwrap())];
            let slots = CULL_VARIANT_COUNT as u64 * cull.variant_capacity() as u64;
            for (label, args, records) in views {
                let args = read_u32s(&device, &queue, args, slots * super::ARG_WORDS as u64);
                let records = read_u32s(&device, &queue, records, slots * 2);
                let mut seen = Vec::new();
                for draw in args.chunks_exact(super::ARG_WORDS as usize).filter(|row| row[1] != 0) {
                    assert_eq!(draw[1], 1, "{label}: one test instance per model section");
                    let record = draw[4] as usize;
                    let instance_slot = records[record * 2] as usize;
                    let model = cull.instances.as_slice()[instance_slot].model;
                    let (_, section) = expected.iter().find(|(id, _)| *id == model)
                        .unwrap_or_else(|| panic!("{label}: unexpected stale model {model}"));
                    assert_eq!(draw[0], section.index_count, "{label}: index count");
                    assert_eq!(draw[2], section.first_index, "{label}: current pool index address");
                    assert_eq!(draw[3], section.base_vertex, "{label}: current pool vertex address");
                    let global_section = records[record * 2 + 1] as usize;
                    assert!(cull.sections[global_section] == *section, "{label}: exact current section row");
                    seen.push(model);
                }
                seen.sort_unstable();
                let mut expected_models: Vec<_> = expected.iter().map(|(id, _)| *id).collect();
                expected_models.sort_unstable();
                assert_eq!(seen, expected_models, "{label}: each live model draws once and stale A never draws");
            }
        };
        let a_section = resolve_registered_section(&meshes, &sources[0]);
        let b_section = resolve_registered_section(&meshes, &sources[1]);
        run_and_check(&mut cull, &[(a, a_section), (b, b_section)]);
        let epoch = cull.instance_epoch();
        cull.retire_model(a);
        assert_eq!(cull.instance_epoch(), epoch.wrapping_add(1),
                   "retained A retirement must invalidate cached scene content without removing A");
        cull.retire_model(a);
        cull.retire_model(u32::MAX);
        assert_eq!(cull.instance_epoch(), epoch.wrapping_add(1), "repeat/unknown retire is inert");
        assert!(!cull.clear_retired_sections(|_|panic!("SECTION prototype disabled")));
        assert_eq!(cull.models[a as usize].lod_base, 0);
        assert_eq!(cull.models[a as usize].lod_count, 0);
        assert!(cull.allocation_lods(a).unwrap().is_empty());
        let feedback=cull.lod_demand_report(101).unwrap();
        assert_eq!(feedback.rows[0].model_id,a);assert_eq!(feedback.rows[0].state,2);
        assert_eq!(feedback.rows[0].lod_mask,0);assert_eq!(cull.models[a as usize]._pad,0);
        let old = meshes.remove(a_mesh).unwrap();
        pool.free(&old.alloc, old.vert_count, old.index_count);
        let resolved: Vec<_> = sources.iter().map(|s| resolve_registered_section(&meshes, s)).collect();
        assert_eq!(resolved[0].index_count, 0);
        cull.set_sections(&resolved);
        run_and_check(&mut cull, &[(b, b_section)]);
        // Keep the stale A instance throughout; normal instance cleanup is not the reason it disappears.
        assert_eq!(cull.instances.as_slice()[raw_slot(a_instance) as usize].model, a);
        let pool_generation = pool.generation();
        let c_mesh = insert(&mut pool, &mut meshes);
        assert_eq!(a_mesh.data().as_ffi() as u32, c_mesh.data().as_ffi() as u32, "actual SlotMap slot reuse");
        assert_ne!(a_mesh.data().as_ffi(), c_mesh.data().as_ffi(), "different full mesh generation");
        assert_eq!(pool.generation(), pool_generation, "recreation must be safe without pool relocation");
        assert_eq!(old_source.mesh, a_mesh.data().as_ffi());
        assert_ne!(old_source.mesh, c_mesh.data().as_ffi());
        assert_eq!(resolve_registered_section(&meshes, &old_source).index_count, 0,
                   "literal old full-generation source must not bind recreated C at the same slot/address");
        let c_source = source(c_mesh);
        let c_section = resolve_registered_section(&meshes, &c_source);
        let (c, span) = cull.register_model_span(1.0, &[lod], &[c_section], &[mat_c]);
        assert_eq!(span.base, 2, "SECTION remains append-only");
        assert_eq!(cull.models[c as usize].lod_base, old_a_lod_base, "actual reuse of A LOD payload");
        assert_eq!(cull.models[a as usize].lod_base, 0, "old A remains permanent sentinel after C overwrite");
        assert_eq!(cull.models[a as usize].lod_count, 0);
        assert_eq!(cull.models[c as usize]._pad,0,"new model does not inherit A feedback target");
        assert_eq!(cull.lod_demand_report(101).unwrap().rows[0].model_id,a);
        super::super::section_span_pool::write_span(&mut sources, span, &[c_source]);
        assert_eq!(sources.len(), 3); assert_eq!(cull.sections.len(), 3);
        assert_eq!(bytemuck::bytes_of(&cull.lods[cull.models[b as usize].lod_base as usize]), b_lod_before.as_slice());
        assert!(cull.sections[1] == b_section, "live B retained its exact row");
        assert_eq!(bytemuck::bytes_of(&cull.section_materials[2]), bytemuck::bytes_of(&mat_c));
        assert_eq!(bytemuck::bytes_of(&cull.section_materials[1]), bytemuck::bytes_of(&mat_b));
        assert!(a != b && b != c && a != c, "stable model IDs cannot alias retired A");
        cull.instance_add(instance(c));
        let resolved: Vec<_> = sources.iter().map(|s| resolve_registered_section(&meshes, s)).collect();
        cull.set_sections(&resolved);
        let handles=[old_source.mesh,b_mesh.data().as_ffi(),c_mesh.data().as_ffi()];
        let (rows,summary)=super::super::registered_mesh_refs::collect(&handles,&cull.models,&cull.lods,
            &sources, |src|src.mesh, |handle|handle!=old_source.mesh, super::super::registered_mesh_refs::LIMITS);
        assert_eq!(summary.complete,1);assert_eq!(rows[0].section_occurrences,0);
        assert_eq!(rows[1].distinct_model_lods,1);assert_eq!(rows[2].distinct_model_lods,1);
        run_and_check(&mut cull, &[(b, b_section), (c, c_section)]);
        eprintln!("PASS reused_lod_three_views: actual pool/full-generation resolver and main/shadow/color GPU readback preserve live B/C with stale A");
    }

    #[test]
    fn registered_section_ownership_overlap_unused_rows_and_global_alias_disable() {
        let lod = |base, count| LodGpu { resolution: 1.0, section_base: base, section_count: count, is_decal: 0 };
        assert!(section_ownership_valid(&[lod(0,1),lod(0,1)],2,2), "overlap within one birth and unused submitted row are safe");
        assert!(!section_ownership_valid(&[lod(1,1)],1,1), "future-birth alias is invalid");
        assert!(!section_ownership_valid(&[lod(0,1)],1,0), "material table mismatch invalidates ownership");
        let Some((device, _queue)) = headless() else {
            eprintln!("SKIP section_ownership_registration: no adapter/device"); return;
        };
        let mut cull=CullState::new(&device);
        cull.section_spans=Some(Box::new(super::super::section_span_pool::SectionSpanPool::new()));
        let rows=[SectionGpu::zeroed();2];let materials=[SectionMaterialGpu::zeroed();2];
        let (a, span)=cull.register_model_span(1.0,&[lod(0,1),lod(0,1)],&rows,&materials);
        assert_eq!(span.count,2);let mut sources=vec![11,12];
        cull.retire_model(a);
        assert!(cull.clear_retired_sections(|s|sources[s.base..s.base+s.count].fill(0)));
        assert_eq!(sources,vec![0,0]);
        let (b, reused)=cull.register_model_span(1.0,&[lod(0,1)],&rows,&materials);
        assert_eq!(reused.base,0);assert_ne!(a,b);assert_eq!(cull.sections.len(),2);
        super::super::section_span_pool::write_span(&mut sources,reused,&[21,22]);
        // Accept ordinary malformed relative LOD semantics, but permanently forbid reuse.
        let (_, escaped)=cull.register_model_span(1.0,&[lod(1,1)],&rows[..1],&materials[..1]);
        assert_eq!(escaped.base,2);sources.push(31);cull.retire_model(b);
        assert!(!cull.clear_retired_sections(|_|panic!("global invalid ownership must disable cleanup")));
        let (_, appended)=cull.register_model_span(1.0,&[lod(0,1)],&rows[..1],&materials[..1]);
        assert_eq!(appended.base,3);
        let mut cull=CullState::new(&device);
        cull.section_spans=Some(Box::new(super::super::section_span_pool::SectionSpanPool::new()));
        let (id,_)=cull.register_model_span(1.0,&[lod(0,1)],&rows[..1],&materials[..1]);
        cull.retire_model(id);cull.validate_source_table_length(0);
        assert!(!cull.clear_retired_sections(|_|panic!("source table mismatch")));
        let (_,appended)=cull.register_model_span(1.0,&[lod(0,1)],&rows[..1],&materials[..1]);
        assert_eq!(appended.base,1);
        eprintln!("PASS section_ownership_registration: actual LodGpu/register/retire gates and parallel source ownership");
    }

    #[test]
    fn lod_reuse_invalid_section_owner_sticky_disables_and_preserves_sentinel() {
        let Some((device,_queue))=headless() else { eprintln!("SKIP lod_ownership_registration: no adapter/device");return; };
        let mut cull=CullState::new(&device);cull.section_spans=None;
        cull.lod_spans=Some(Box::new(super::super::section_span_pool::SectionSpanPool::new_table("LOD")));
        if cull.lods.is_empty() {cull.lods.push(LodGpu::zeroed());}
        let lod=|base|LodGpu{resolution:1.0,section_base:base,section_count:1,is_decal:0};
        let row=[SectionGpu::zeroed()];let material=[SectionMaterialGpu::zeroed()];
        let a=cull.register_model(1.0,&[lod(0)],&row,&material);
        let old_base=cull.models[a as usize].lod_base;cull.retire_model(a);
        // Even with a free old A range, new admitted escaping SECTION LOD disables all reuse.
        let invalid=cull.register_model(1.0,&[lod(1)],&row,&material);
        assert_ne!(cull.models[invalid as usize].lod_base,old_base);
        let b=cull.register_model(1.0,&[lod(0)],&row,&material);
        assert_ne!(cull.models[b as usize].lod_base,old_base);
        assert_eq!(cull.models[a as usize].lod_base,0);assert_eq!(cull.models[a as usize].lod_count,0);
        assert_eq!(bytemuck::bytes_of(&cull.lods[0]),bytemuck::bytes_of(&LodGpu::zeroed()));
        assert!(!cull.lod_spans.as_ref().unwrap().enabled());
        let mut cull=CullState::new(&device);cull.section_spans=None;
        cull.lod_spans=Some(Box::new(super::super::section_span_pool::SectionSpanPool::new_table("LOD")));
        if cull.lods.is_empty() {cull.lods.push(LodGpu::zeroed());}
        let a=cull.register_model(1.0,&[lod(0)],&row,&material);
        let b=cull.register_model(1.0,&[lod(0)],&row,&material);
        let b_base=cull.models[b as usize].lod_base;
        let b_bytes=bytemuck::bytes_of(&cull.lods[b_base as usize]).to_vec();
        // Private malformed mutation simulates any future mutable escape: no foreign zero/free.
        cull.models[a as usize].lod_base=b_base;
        cull.retire_model(a);
        assert_eq!(bytemuck::bytes_of(&cull.lods[b_base as usize]),b_bytes.as_slice());
        assert_eq!(cull.models[a as usize].lod_base,0);assert_eq!(cull.models[a as usize].lod_count,0);
        assert!(!cull.lod_spans.as_ref().unwrap().enabled());
        let c=cull.register_model(1.0,&[lod(0)],&row,&material);
        assert!(cull.models[c as usize].lod_base>b_base,"foreign alias disables reuse rather than clearing B");
        eprintln!("PASS lod_ownership_registration: invalid admitted SECTION and foreign LOD alias disable reuse with B/tombstone preserved");
    }

    #[test]
    fn lod_sentinel_retirement_transaction_marks_both_domains_and_never_realiases_old_birth() {
        let lod=|count|LodGpu {resolution:1.0,section_base:0,section_count:count,is_decal:0};
        let mut lods=[LodGpu::zeroed(),lod(1),lod(2),lod(3)];
        let mut models=[ModelGpu{lod_base:1,lod_count:2,bounding_sphere:1.0,_pad:7},
                        ModelGpu{lod_base:3,lod_count:1,bounding_sphere:2.0,_pad:0}];
        let before_b=bytemuck::bytes_of(&models[1]).to_vec();let before_lod_b=bytemuck::bytes_of(&lods[3]).to_vec();
        let mut instances=InstanceTable::default();let mut uploads=TableUploads::NONE;let epoch=instances.epoch;
        let birth=Some(super::super::section_span_pool::Span{base:1,count:2});
        let (changed,span)=retire_model_to_sentinel(&mut models,&mut lods,0,birth,&mut instances,&mut uploads);
        assert!(changed);assert_eq!(span.unwrap(),super::super::section_span_pool::Span{base:1,count:2});
        assert_eq!(models[0].lod_base,0);assert_eq!(models[0].lod_count,0);assert_eq!(models[0]._pad,0);
        assert_eq!(instances.epoch,epoch.wrapping_add(1));
        assert_eq!(uploads,TableUploads(TableDomain::Model.bit()|TableDomain::Lod.bit()));
        assert_eq!(bytemuck::bytes_of(&models[1]),before_b.as_slice());assert_eq!(bytemuck::bytes_of(&lods[3]),before_lod_b.as_slice());
        lods[1]=lod(99);lods[2]=lod(100); // C overwrites freed payload; A still sees permanent empty index0.
        assert_eq!(lods[models[0].lod_base as usize].section_count,0);
        uploads=TableUploads::NONE;
        for id in [0,u32::MAX] {assert!(!retire_model_to_sentinel(&mut models,&mut lods,id,birth,&mut instances,&mut uploads).0);}
        assert_eq!(uploads,TableUploads::NONE);assert_eq!(instances.epoch,epoch.wrapping_add(1));
    }

    #[test]
    fn lod_sentinel_foreign_or_missing_birth_witness_never_clears_another_owner() {
        let mut pool=super::super::section_span_pool::SectionSpanPool::new_table("LOD");
        pool.register(0,1,1,true);pool.register(1,2,1,true);
        let lod=LodGpu{resolution:1.0,section_base:0,section_count:1,is_decal:0};
        for witness in [pool.birth_span(0),None] {
            let mut lods=[LodGpu::zeroed(),lod,lod];
            let mut models=[ModelGpu{lod_base:2,lod_count:1,bounding_sphere:1.0,_pad:0},
                            ModelGpu{lod_base:2,lod_count:1,bounding_sphere:2.0,_pad:0}];
            let before=bytemuck::cast_slice::<LodGpu,u8>(&lods).to_vec();
            let mut instances=InstanceTable::default();let mut uploads=TableUploads::NONE;
            let (changed,freed)=retire_model_to_sentinel(&mut models,&mut lods,0,witness,&mut instances,&mut uploads);
            assert!(changed);assert!(freed.is_none());
            assert_eq!(bytemuck::cast_slice::<LodGpu,u8>(&lods),before.as_slice());
            assert_eq!(models[0].lod_base,0);assert_eq!(models[0].lod_count,0);
            assert_eq!(models[1].lod_base,2);assert_eq!(lods[2].section_count,1);
        }
    }

    #[test]
    fn model_retirement_invalidates_retained_content_only_for_actual_change() {
        let models = [ModelGpu { lod_base: 0, lod_count: 2, bounding_sphere: 1.0, _pad: 0 },
                      ModelGpu { lod_base: 2, lod_count: 1, bounding_sphere: 2.0, _pad: 0 }];
        let mut lods = [LodGpu { resolution: 1.0, section_base: 0, section_count: 0, is_decal: 0 },
                        LodGpu { resolution: 2.0, section_base: 0, section_count: 3, is_decal: 0 },
                        LodGpu { resolution: 1.0, section_base: 3, section_count: 1, is_decal: 0 }];
        let mut instances = InstanceTable::default();
        let mut instance = InstanceGpu::zeroed(); instance.model = 0;
        let handle = instances.add(instance);
        instances.take_dirty();
        let epoch = instances.epoch;
        let slots = bytemuck::cast_slice::<InstanceGpu, u8>(&instances.slots).to_vec();
        let generations = instances.generations.clone();
        let free = instances.free_slots.clone();
        let mut uploads = TableUploads::NONE;
        assert!(!retire_model_content(&models, &mut lods, u32::MAX, &mut instances, &mut uploads));
        assert_eq!(instances.epoch, epoch);
        assert_eq!(uploads, TableUploads::NONE);
        assert!(retire_model_content(&models, &mut lods, 0, &mut instances, &mut uploads));
        assert_eq!(instances.epoch, epoch.wrapping_add(1));
        assert_eq!(uploads, TableUploads(TableDomain::Lod.bit()));
        assert_eq!(lods[0].section_count, 0); assert_eq!(lods[1].section_count, 0);
        assert_eq!(lods[2].section_count, 1, "live neighbor remains untouched");
        assert_eq!(instances.slot_of(handle), Some(0), "instance handle still valid");
        assert_eq!(bytemuck::cast_slice::<InstanceGpu, u8>(&instances.slots), slots.as_slice());
        assert_eq!(instances.generations, generations); assert_eq!(instances.free_slots, free);
        assert!(instances.dirty.is_none(), "content epoch must not fabricate instance uploads");
        uploads = TableUploads::NONE;
        for id in [0, 0, u32::MAX] {
            assert!(!retire_model_content(&models, &mut lods, id, &mut instances, &mut uploads));
            assert_eq!(instances.epoch, epoch.wrapping_add(1));
            assert_eq!(uploads, TableUploads::NONE);
        }
        assert!(retire_model_content(&models, &mut lods, 1, &mut instances, &mut uploads));
        assert_eq!(instances.epoch, epoch.wrapping_add(2));
    }

    #[test]
    fn retained_target_revision_tracks_old_and_new_model_without_foreign_churn() {
        let inst = |model| { let mut row = InstanceGpu::zeroed(); row.model = model; row };
        let mut table = InstanceTable::default();
        assert_eq!(table.target_revision(), None, "default path has no witness");
        let a = table.add(inst(7));
        let b = table.add(inst(8));
        assert_eq!(table.arm_target_revision(7), Some(1));
        assert_eq!(table.arm_target_revision(7), Some(1), "same target is inert");
        table.update(b, inst(8));
        let extra_b = table.add(inst(8));
        table.remove(b);
        let reused_b = table.add(inst(8));
        assert_ne!(b, reused_b, "recycled slot has a new handle");
        assert_eq!(table.target_revision(), Some((7, 1)));

        table.update(a, inst(8)); // old A must invalidate A
        assert_eq!(table.target_revision(), Some((7, 2)));
        table.update(a, inst(7)); // new A must invalidate A
        assert_eq!(table.target_revision(), Some((7, 3)));
        table.update(a, inst(7)); // conservative same-model mutation
        assert_eq!(table.target_revision(), Some((7, 4)));
        table.remove(a); // capture old model before writing INVALID_MODEL
        assert_eq!(table.target_revision(), Some((7, 5)));
        let reused_a = table.add(inst(7)); // reused slot must count as a new A
        assert_ne!(a, reused_a);
        assert_eq!(table.target_revision(), Some((7, 6)));
        table.update(a, inst(7)); // stale handle is refused
        table.remove(a);
        table.remove(extra_b);
        assert_eq!(table.target_revision(), Some((7, 6)));

        assert_eq!(table.arm_target_revision(8), Some(7));
        table.update(reused_a, inst(8)); // new B must invalidate B
        assert_eq!(table.target_revision(), Some((8, 8)));
        assert_eq!(table.arm_target_revision(7), Some(9),
            "switching back cannot alias an older A fact");
    }

    #[test]
    fn static_noop_epoch_witness_only_accepts_bit_identical_valid_updates() {
        let mut table = InstanceTable::default();
        let original = inst(7);
        let live = table.add(original);
        let dead = table.add(inst(8));
        table.remove(dead);
        assert_eq!(table.static_noop_epoch_relation(table.epoch), StaticEpochRelation::Unknown,
            "the default path is unarmed");

        let start = table.arm_static_noop_epoch();
        assert_eq!(table.static_noop_epoch_relation(start), StaticEpochRelation::NoMutation);
        table.update(live, original);
        table.update(live, original);
        assert_eq!(table.epoch, start + 2, "the existing global epoch still advances");
        assert_eq!(table.static_noop_epoch_relation(start),
            StaticEpochRelation::OnlyBitIdenticalUpdates);
        assert_eq!(table.static_noop_epoch_relation(start + 1), StaticEpochRelation::Unknown,
            "a fact may only bind to its own explicitly armed baseline");

        let before_stale = table.epoch;
        table.update(dead, original);
        assert_eq!(table.epoch, before_stale, "a stale handle never changes the table");
        assert_eq!(table.static_noop_epoch_relation(start),
            StaticEpochRelation::OnlyBitIdenticalUpdates);

        let mut changed = original;
        changed.world[0] = 1.0;
        table.update(live, changed);
        assert_eq!(table.static_noop_epoch_relation(start), StaticEpochRelation::ChangedOrUnknown);
        table.update(live, changed);
        assert_eq!(table.static_noop_epoch_relation(start), StaticEpochRelation::ChangedOrUnknown,
            "a later no-op cannot erase an earlier change");

        let fresh = table.arm_static_noop_epoch();
        table.update(live, changed);
        assert_eq!(table.static_noop_epoch_relation(fresh),
            StaticEpochRelation::OnlyBitIdenticalUpdates);
        table.remove(live);
        assert_eq!(table.static_noop_epoch_relation(fresh), StaticEpochRelation::ChangedOrUnknown);
    }

    #[test]
    fn static_noop_epoch_witness_fails_closed_on_retirement_and_epoch_wrap() {
        let mut table = InstanceTable::default();
        let live = table.add(inst(7));
        let start = table.arm_static_noop_epoch();
        let models = [ModelGpu { lod_base: 0, lod_count: 1,
            bounding_sphere: 1.0, _pad: 0 }];
        let mut lods = [LodGpu { resolution: 1.0, section_base: 0,
            section_count: 1, is_decal: 0 }];
        let mut uploads = TableUploads::NONE;
        assert!(retire_model_content(&models, &mut lods, 0, &mut table, &mut uploads));
        assert_eq!(table.static_noop_epoch_relation(start), StaticEpochRelation::ChangedOrUnknown,
            "drawable retirement and unattributed invalidation are never no-ops");

        table.epoch = u64::MAX;
        let wrapped_start = table.arm_static_noop_epoch();
        table.update(live, inst(7));
        assert_eq!(table.epoch, 0);
        assert_eq!(table.static_noop_epoch_relation(wrapped_start),
            StaticEpochRelation::ChangedOrUnknown, "wrapping must not alias an old image");
    }

    #[test]
    fn retained_target_revision_covers_both_retirement_paths_and_fails_closed_on_wrap() {
        let lod = LodGpu { resolution: 1.0, section_base: 0, section_count: 1, is_decal: 0 };
        let models = [ModelGpu { lod_base: 0, lod_count: 1, bounding_sphere: 1.0, _pad: 0 },
                      ModelGpu { lod_base: 1, lod_count: 1, bounding_sphere: 1.0, _pad: 0 }];
        let mut lods = [lod, lod];
        let mut table = InstanceTable::default();
        let mut uploads = TableUploads::NONE;
        assert_eq!(table.arm_target_revision(0), Some(1));
        assert!(retire_model_content(&models, &mut lods, 1, &mut table, &mut uploads));
        assert_eq!(table.target_revision(), Some((0, 1)), "foreign retirement is inert");
        assert!(retire_model_content(&models, &mut lods, 0, &mut table, &mut uploads));
        assert_eq!(table.target_revision(), Some((0, 2)));
        assert!(!retire_model_content(&models, &mut lods, 0, &mut table, &mut uploads));
        assert_eq!(table.target_revision(), Some((0, 2)), "repeat retirement is inert");

        let mut sentinel_models = [ModelGpu { lod_base: 1, lod_count: 1, bounding_sphere: 1.0, _pad: 0 }];
        let mut sentinel_lods = [LodGpu::zeroed(), lod];
        let mut sentinel_table = InstanceTable::default();
        assert_eq!(sentinel_table.arm_target_revision(0), Some(1));
        let birth = Some(super::super::section_span_pool::Span { base: 1, count: 1 });
        assert!(retire_model_to_sentinel(&mut sentinel_models, &mut sentinel_lods, 0,
            birth, &mut sentinel_table, &mut uploads).0);
        assert_eq!(sentinel_table.target_revision(), Some((0, 2)));
        assert!(!retire_model_to_sentinel(&mut sentinel_models, &mut sentinel_lods, 0,
            birth, &mut sentinel_table, &mut uploads).0);
        assert_eq!(sentinel_table.target_revision(), Some((0, 2)));

        sentinel_table.target_revision.as_mut().unwrap().revision = u64::MAX;
        assert_eq!(sentinel_table.arm_target_revision(1), None);
        sentinel_table.changed_target_model(1, INVALID_MODEL);
        assert_eq!(sentinel_table.arm_target_revision(0), None,
            "overflow cannot revive an old serial");
    }

    #[test]
    fn dynamic_target_rows_change_only_the_armed_model_revision() {
        let row = |model, x| {
            let mut value = InstanceGpu::zeroed();
            value.model = model;
            value.center[0] = x;
            value
        };
        let a1 = row(7, 1.0);
        let a2 = row(7, 2.0);
        let b1 = row(8, 3.0);
        let mut static_instances = InstanceTable::default();
        let mut dynamic = Vec::new();
        replace_dynamic_instances(&mut dynamic, &[a1, b1, a2], &mut static_instances);
        assert_eq!(static_instances.target_revision(), None, "default path stays unarmed");
        let global_epoch = static_instances.epoch;
        assert_eq!(static_instances.arm_target_revision(7), Some(1));
        replace_dynamic_instances(&mut dynamic, &[a1, b1, a2], &mut static_instances);
        assert_eq!(static_instances.target_revision(), Some((7, 1)));
        replace_dynamic_instances(&mut dynamic, &[row(8, 9.0), a1, a2], &mut static_instances);
        assert_eq!(static_instances.target_revision(), Some((7, 1)),
            "foreign row changes and reorder do not touch A");

        replace_dynamic_instances(&mut dynamic, &[row(8, 9.0), a1, row(7, 4.0)], &mut static_instances);
        assert_eq!(static_instances.target_revision(), Some((7, 2)), "A transform changed");
        replace_dynamic_instances(&mut dynamic, &[row(8, 9.0), a1], &mut static_instances);
        assert_eq!(static_instances.target_revision(), Some((7, 3)), "A was removed");
        replace_dynamic_instances(&mut dynamic, &[row(8, 9.0), a1, a2], &mut static_instances);
        assert_eq!(static_instances.target_revision(), Some((7, 4)), "A was added");
        replace_dynamic_instances(&mut dynamic, &[a2, row(8, 9.0), a1], &mut static_instances);
        assert_eq!(static_instances.target_revision(), Some((7, 5)),
            "target-row reorder conservatively invalidates");
        assert_eq!(static_instances.epoch, global_epoch,
            "diagnostic witness never changes global visual-cache scheduling");
    }

    #[test]
    fn dynamic_image_witness_detects_foreign_rows_without_changing_target_or_epoch() {
        let row = |model, x| {
            let mut value = InstanceGpu::zeroed();
            value.model = model;
            value.center[0] = x;
            value
        };
        let target = row(7, 1.0);
        let foreign = row(8, 2.0);
        let mut table = InstanceTable::default();
        let mut dynamic = Vec::new();
        replace_dynamic_instances(&mut dynamic, &[target, foreign], &mut table);
        assert_eq!(table.dynamic_row_relation(1), DynamicRowRelation::Unknown);
        assert_eq!(table.arm_target_revision(7), Some(1));
        let stamp = table.arm_dynamic_image_mutation().unwrap();
        let epoch = table.epoch;

        replace_dynamic_instances(&mut dynamic, &[target, foreign], &mut table);
        assert_eq!(table.dynamic_row_relation(stamp), DynamicRowRelation::Unchanged);
        replace_dynamic_instances(&mut dynamic, &[target, row(8, 3.0)], &mut table);
        assert_eq!(table.target_revision(), Some((7, 1)), "target COUNT rows stayed equal");
        assert_eq!(table.dynamic_row_relation(stamp), DynamicRowRelation::ChangedOrUnknown,
            "a foreign caster may have changed the cached image");
        assert_eq!(table.epoch, epoch, "this witness never changes visual-cache scheduling");

        let fresh = table.arm_dynamic_image_mutation().unwrap();
        assert_ne!(stamp, fresh);
        assert_eq!(table.dynamic_row_relation(fresh), DynamicRowRelation::Unchanged);
        replace_dynamic_instances(&mut dynamic, &[row(8, 3.0), target], &mut table);
        assert_eq!(table.dynamic_row_relation(fresh), DynamicRowRelation::ChangedOrUnknown,
            "row reorder is conservatively treated as an image mutation");
        let again = table.arm_dynamic_image_mutation().unwrap();
        replace_dynamic_instances(&mut dynamic, &[target], &mut table);
        assert_eq!(table.dynamic_row_relation(again), DynamicRowRelation::ChangedOrUnknown,
            "foreign row removal must invalidate the prior stamp");
    }

    #[test]
    fn dynamic_image_witness_overflow_fails_closed_and_cannot_rearm() {
        let mut table = InstanceTable::default();
        let mut dynamic = Vec::new();
        assert_eq!(table.arm_dynamic_image_mutation(), Some(1));
        table.dynamic_image_mutation.as_mut().unwrap().serial = u64::MAX;
        let mut row = InstanceGpu::zeroed();
        row.model = 7;
        replace_dynamic_instances(&mut dynamic, &[row], &mut table);
        assert_eq!(table.arm_dynamic_image_mutation(), None);
        assert_eq!(table.dynamic_row_relation(u64::MAX), DynamicRowRelation::Unknown);
        replace_dynamic_instances(&mut dynamic, &[], &mut table);
        assert_eq!(table.arm_dynamic_image_mutation(), None);
        assert_eq!(table.dynamic_row_relation(1), DynamicRowRelation::Unknown);
    }

    #[test]
    fn unknown_geometry_dependency_advances_revision_and_old_fact_mismatches() {
        let mut table = InstanceTable::default();
        table.advance_target_revision_for_unattributed_geometry();
        assert_eq!(table.target_revision(), None, "unarmed normal path stays empty");
        assert_eq!(table.arm_target_revision(7), Some(1));
        let old_fact_revision = table.target_revision().unwrap().1;
        table.changed_target_model(8, INVALID_MODEL);
        assert_eq!(table.target_revision(), Some((7, 1)));
        table.advance_target_revision_for_unattributed_geometry();
        assert_eq!(table.target_revision(), Some((7, 2)));
        assert_ne!(table.target_revision().unwrap().1, old_fact_revision,
            "an earlier cached COUNT stamp cannot match after geometry changes");
        let mut row = InstanceGpu::zeroed(); row.model = 7;
        table.add(row);
        assert_eq!(table.arm_target_revision(7), Some(3));
        assert_eq!(table.arm_target_revision(8), Some(4));
        table.target_revision.as_mut().unwrap().revision = u64::MAX;
        table.advance_target_revision_for_unattributed_geometry();
        assert_eq!(table.arm_target_revision(7), None,
            "only serial overflow permanently refuses future samples");
    }

    #[test]
    fn changed_resolved_sections_advance_only_an_armed_witness() {
        let old = [SectionGpu::zeroed()];
        let mut changed = old;
        changed[0].index_count = 3;
        let mut table = InstanceTable::default();
        assert!(!resolved_sections_changed(&old, &old, &mut table));
        assert!(resolved_sections_changed(&old, &changed, &mut table));
        assert_eq!(table.target_revision(), None, "default path remains unarmed");
        assert_eq!(table.arm_target_revision(7), Some(1));
        assert!(!resolved_sections_changed(&old, &old, &mut table));
        assert_eq!(table.target_revision(), Some((7, 1)));
        assert!(resolved_sections_changed(&old, &changed, &mut table));
        assert_eq!(table.target_revision(), Some((7, 2)));
        assert!(resolved_sections_changed(&changed, &old, &mut table));
        assert_eq!(table.target_revision(), Some((7, 3)),
            "a fresh sample after each change has a usable new serial");
    }

    #[test]
    fn completed_uploader_serial_invalidates_old_target_samples_once() {
        let mut table = InstanceTable::default();
        table.observe_uploader_mutation_serial(1);
        assert_eq!(table.target_revision(), None, "unarmed observation is inert");
        assert_eq!(table.arm_target_revision(7), Some(1));
        table.observe_uploader_mutation_serial(1); // first enable covers untracked history
        let first = table.target_revision().unwrap().1;
        assert_eq!(first, 2);
        table.observe_uploader_mutation_serial(1);
        assert_eq!(table.target_revision(), Some((7, first)));
        table.observe_uploader_mutation_serial(3); // two completed writes since last read
        assert_eq!(table.target_revision(), Some((7, first + 1)));
        assert_ne!(table.target_revision().unwrap().1, first,
            "a cached fact stamped before off-handle writes must mismatch");
        assert_eq!(table.arm_target_revision(8), Some(first + 2));
        table.observe_uploader_mutation_serial(3);
        assert_eq!(table.target_revision(), Some((8, first + 3)),
            "target switch cannot inherit a previous model's observed serial");
        table.observe_uploader_mutation_serial(0); // uploader serial overflow
        assert_eq!(table.arm_target_revision(7), None);
        table.observe_uploader_mutation_serial(4);
        assert_eq!(table.target_revision(), None, "overflow remains fail-closed");
    }


    #[test]
    fn model_row_upload_interval_counts_actual_selected_writes() {
        let mut rows = ModelRowUploads::default();
        rows.mark(100); rows.mark(104); rows.mark(101);
        let mut writes = Vec::new(); rows.run(200, false, |r| writes.push(r));
        assert_eq!(writes, vec![100..105]);
        writes.clear(); rows.run(200, true, |r| writes.push(r));
        assert_eq!(writes, vec![0..200], "growth must initialize every old row too");
        writes.clear(); ModelRowUploads::default().run(200, false, |r| writes.push(r));
        assert_eq!(writes, vec![0..200], "unknown MODEL dirtiness remains conservative");
        writes.clear(); ModelRowUploads::default().run(0, true, |r| writes.push(r));
        assert!(writes.is_empty(), "empty bootstrap allocates storage but writes no rows");
        rows.mark(usize::MAX); writes.clear(); rows.run(200, false, |r| writes.push(r));
        assert_eq!(writes, vec![0..200]);
        assert!(rows.take_activation().unwrap().contains("knownStateBytes="));
        assert!(rows.take_activation().is_none());
        for event in 1..=16 { rows.note_committed(100, 1, 200);
            assert!(rows.take_commit().unwrap().contains(&format!("event={}", event))); }
        rows.note_committed(100, 1, 200); assert!(rows.take_commit().unwrap().contains("truncated"));
        rows.note_committed(100, 1, 200); assert!(rows.take_commit().is_none());
        rows.reset_interval(); assert_eq!(rows.committed_rows, 17);
        let mut tables = TableUploads::NONE;
        tables.run(|_| panic!("clean prepare makes no resolver/write call"));
        tables.mark(TableDomain::Model); let pending = rows;
        let failure = std::panic::catch_unwind(|| tables.run(|_| panic!("failed upload")));
        assert!(failure.is_err()); assert_eq!(rows, pending); assert_ne!(tables, TableUploads::NONE);
    }

    #[test]
    fn model_row_upload_actual_gpu_preserves_neighbors_and_full_growth() {
        let Some((device, queue)) = headless() else {
            eprintln!("SKIP model_row_upload_actual_gpu: no adapter/device"); return;
        };
        let mut cull = CullState::new(&device);
        cull.model_row_uploads = Some(ModelRowUploads::default());
        cull.variant_capacity = 16; // bounded test outputs, production capacity unchanged
        // COPY_SRC is test-only. Use real production prepare and table mutation APIs.
        cull.model_buf.cap = 4096;
        cull.model_buf.buf = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("model-row-readback"), size: 4096,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        }));
        let lod = LodGpu { resolution: 1.0, section_base: 0, section_count: 0, is_decal: 0 };
        for i in 0..128 { cull.register_model(i as f32 + 1.0, &[lod], &[], &[]); }
        cull.prepare(&device, &queue);
        let words = (std::mem::size_of_val(cull.models.as_slice()) / 4) as u64;
        let baseline = read_u32s(&device, &queue, cull.model_buf.buf.as_ref().unwrap(), words);
        assert_eq!(baseline, bytemuck::cast_slice::<ModelGpu,u32>(&cull.models));
        // Seed a deliberately different GPU-only neighbor. Partial update must leave it;
        // the legacy full write would erase it. This proves actual write selection.
        let sentinel = ModelGpu { lod_base: 0, lod_count: 0, bounding_sphere: 987.0, _pad: 73 };
        queue.write_buffer(cull.model_buf.buf.as_ref().unwrap(), 0, bytemuck::bytes_of(&sentinel));
        cull.set_feedback_tag(100, 7);
        assert_eq!(cull.model_row_uploads.unwrap().range(128, false), 100..101);
        cull.prepare(&device, &queue);
        let changed = read_u32s(&device, &queue, cull.model_buf.buf.as_ref().unwrap(), words);
        assert_eq!(&changed[..4], bytemuck::cast_slice::<ModelGpu,u32>(&[sentinel]));
        assert_eq!(&changed[4..], &bytemuck::cast_slice::<ModelGpu,u32>(&cull.models)[4..]);
        cull.set_feedback_tag(100, 7); assert_eq!(cull.table_uploads, TableUploads::NONE);
        cull.set_feedback_tag(9999, 7); assert_eq!(cull.table_uploads, TableUploads::NONE);
        // Restore the GPU-only neighbor, then compare exact partial versus legacy bytes.
        queue.write_buffer(cull.model_buf.buf.as_ref().unwrap(), 0, bytemuck::bytes_of(&cull.models[0]));
        cull.set_feedback_tag(101, 8); cull.prepare(&device, &queue);
        let optimized = read_u32s(&device, &queue, cull.model_buf.buf.as_ref().unwrap(), words);
        upload_slice(&device, &queue, &mut cull.model_buf, &cull.models);
        assert_eq!(optimized, read_u32s(&device, &queue, cull.model_buf.buf.as_ref().unwrap(), words));
        // Force actual missing/reallocation path. Existing prefix must be fully initialized.
        cull.model_buf.buf = None; cull.model_buf.cap = 0;
        cull.set_feedback_tag(102, 9); cull.prepare(&device, &queue);
        assert_eq!(read_table_storage_words(&device, &queue, cull.model_buf.buf.as_ref().unwrap(), words),
            bytemuck::cast_slice::<ModelGpu,u32>(&cull.models));
        // Actual capacity growth also initializes the full historical prefix, not only
        // appended rows. New storage cannot inherit old GPU contents without a copy.
        let old_capacity = cull.model_buf.cap;
        for i in 128..300 { cull.register_model(i as f32 + 1.0, &[lod], &[], &[]); }
        cull.prepare(&device, &queue); assert!(cull.model_buf.cap > old_capacity);
        assert_eq!(read_table_storage_words(&device, &queue, cull.model_buf.buf.as_ref().unwrap(),
            (std::mem::size_of_val(cull.models.as_slice()) / 4) as u64), bytemuck::cast_slice::<ModelGpu,u32>(&cull.models));
        // Separate actual epoch starts with its LOD pool at birth; no retroactive fake owner.
        let mut retirement = CullState::new(&device);
        retirement.model_row_uploads = Some(ModelRowUploads::default());
        retirement.variant_capacity = 16;
        retirement.lod_spans = Some(Box::new(super::super::section_span_pool::SectionSpanPool::new_table("LOD")));
        retirement.lods = vec![LodGpu::zeroed()];
        let a = retirement.register_model(3.0, &[lod], &[], &[]);
        let b = retirement.register_model(4.0, &[lod], &[], &[]);
        retirement.prepare(&device, &queue); let b_before = bytemuck::bytes_of(&retirement.models[b as usize]).to_vec();
        retirement.retire_model(a); assert_eq!(retirement.model_row_uploads.unwrap().range(retirement.models.len(), false), a as usize..a as usize+1);
        retirement.prepare(&device, &queue);
        assert_eq!(bytemuck::bytes_of(&retirement.models[b as usize]), b_before.as_slice());
        assert_eq!(read_table_storage_words(&device, &queue, retirement.model_buf.buf.as_ref().unwrap(),
            (std::mem::size_of_val(retirement.models.as_slice()) / 4) as u64), bytemuck::cast_slice::<ModelGpu,u32>(&retirement.models));
        retirement.retire_model(a); assert_eq!(retirement.table_uploads, TableUploads::NONE);
        eprintln!("PASS model_row_upload_actual_gpu: actual production partial/full GPU writes and neighbor readback matched");
    }
#[test]
    fn reflection_target_count_early_submit_present_absent_and_skip() {
        let Some((device, queue)) = headless() else { return; };
        let mut cull = CullState::new(&device);
        let sections = [SectionGpu {
            first_index: 0, index_count: 3, base_vertex: 0, variant: 0,
        }];
        let lods = [LodGpu {
            resolution: 0.0, section_base: 0, section_count: 1, is_decal: 0,
        }];
        let materials = [SectionMaterialGpu::zeroed()];
        let model = cull.register_model(1.0, &lods, &sections, &materials);
        let absent_model = cull.register_model(1.0, &lods, &sections, &materials);
        assert!(cull.reflection_count_slots.is_none());
        assert!(!cull.arm_main_target_count(&device, 1, model, 0));
        assert!(cull.reflection_count_slots.is_none(),
            "invalid opt-in request must not allocate reflection staging");
        for _ in 0..8 {
            cull.instance_add(InstanceGpu {
                world: Mat4::IDENTITY.to_cols_array(),
                center: [0.0, 0.0, 0.0, 1.0],
                model, flags: 0, cull_radius: 0, _pad: 0,
                conform0: [0.0; 4], conform1: [0.0; 4], conform2: [0.0; 4],
            });
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
        cull.set_params(params);
        cull.set_reflection_params(&device, params);

        assert!(cull.arm_main_target_count(&device, 2, model, 17));
        cull.prepare(&device, &queue);
        let mut planar = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut planar);
        assert!(!cull.copy_reflection_target_count(&mut planar),
            "a prepared view with no actual dispatch cannot publish zero");
        assert!(cull.dispatch_reflection(&mut planar));
        assert!(cull.copy_reflection_target_count(&mut planar));
        assert!(cull.reflection_target_count_before_submit(2));
        queue.submit(std::iter::once(planar.finish()));
        assert!(cull.reflection_target_count_submitted(2));
        // The real frame commits the early planar submission only after its
        // later main encoder also succeeded.
        assert!(cull.reflection_target_count_commit(2));
        for _ in 0..64 {
            cull.harvest_reflection_target_count(&device);
            if cull.reflection_target_count_fact().status == MainCountStatus::Present { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let present = cull.reflection_target_count_fact();
        assert_eq!(present.identity.source_generation, 17);
        assert_eq!(present.status, MainCountStatus::Present);
        assert_eq!(present.count, 8);

        // A new request with the mirror disabled must not inherit the old
        // positive or fabricate an absence from begin_frame_stats' zeroing.
        assert!(cull.arm_main_target_count(&device, 3, model, 18));
        cull.clear_reflection_view();
        cull.prepare(&device, &queue);
        let mut skipped = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut skipped);
        assert!(!cull.dispatch_reflection(&mut skipped));
        assert!(!cull.copy_reflection_target_count(&mut skipped));
        assert!(!cull.reflection_target_count_before_submit(3));
        assert_eq!(cull.reflection_target_count_fact().status, MainCountStatus::Unknown);
        cull.reflection_target_count_abort(3);

        // True zero requires a current selected model, a live mirror cull,
        // an encoded copy, accepted early submission, and final commit.
        cull.set_reflection_params(&device, params);
        assert!(cull.arm_main_target_count(&device, 4, absent_model, 19));
        cull.prepare(&device, &queue);
        let mut zero = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut zero);
        assert!(cull.dispatch_reflection(&mut zero));
        assert!(cull.copy_reflection_target_count(&mut zero));
        assert!(cull.reflection_target_count_before_submit(4));
        queue.submit(std::iter::once(zero.finish()));
        assert!(cull.reflection_target_count_submitted(4));
        assert!(cull.reflection_target_count_commit(4));
        for _ in 0..64 {
            cull.harvest_reflection_target_count(&device);
            if cull.reflection_target_count_fact().status == MainCountStatus::Absent { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let absent = cull.reflection_target_count_fact();
        assert_eq!(absent.identity.model_id, absent_model);
        assert_eq!(absent.status, MainCountStatus::Absent);
        assert_eq!(absent.count, 0);

        // A later failure after the early submit still invalidates that sample
        // and drains its staging slot rather than treating acceptance as commit.
        assert!(cull.arm_main_target_count(&device, 5, model, 20));
        cull.prepare(&device, &queue);
        let mut aborted = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut aborted);
        assert!(cull.dispatch_reflection(&mut aborted));
        assert!(cull.copy_reflection_target_count(&mut aborted));
        assert!(cull.reflection_target_count_before_submit(5));
        queue.submit(std::iter::once(aborted.finish()));
        assert!(cull.reflection_target_count_submitted(5));
        cull.reflection_target_count_abort(5);
        for _ in 0..64 {
            cull.harvest_reflection_target_count(&device);
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        assert_eq!(cull.reflection_target_count_fact().status, MainCountStatus::Unknown);
        assert!(cull.reflection_count_slots.as_ref().unwrap().iter()
            .all(|(_, state)| matches!(state, MainCountSlot::Idle)),
            "an aborted early submission must drain its staging slot");
    }
#[test]
    fn sky0_target_count_present_absent_cache_and_abort() {
        let Some((device, queue)) = headless() else {
            assert_ne!(std::env::var("WGR_REQUIRE_TEST_GPU").as_deref(), Ok("1"),
                "GPU COUNT test required an adapter");
            return;
        };
        let mut cull = CullState::new(&device);
        let sections = [SectionGpu {
            first_index: 0, index_count: 3, base_vertex: 0, variant: 0,
        }];
        let lods = [LodGpu {
            resolution: 0.0, section_base: 0, section_count: 1, is_decal: 0,
        }];
        let materials = [SectionMaterialGpu::zeroed()];
        let model = cull.register_model(1.0, &lods, &sections, &materials);
        let absent_model = cull.register_model(1.0, &lods, &sections, &materials);
        assert!(cull.sky0_count_slots.is_none());
        assert!(!cull.arm_main_target_count(&device, 1, model, 0));
        assert!(cull.sky0_count_slots.is_none(),
            "invalid request cannot allocate zenith diagnostic staging");
        for _ in 0..8 {
            cull.instance_add(InstanceGpu {
                world: Mat4::IDENTITY.to_cols_array(),
                center: [0.0, 0.0, 0.0, 1.0],
                model, flags: 0, cull_radius: 0, _pad: 0,
                conform0: [0.0; 4], conform1: [0.0; 4], conform2: [0.0; 4],
            });
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
        cull.set_params(params);
        cull.set_sky_view_count(&device, super::super::sky_vis::DIRECTION_COUNT + 1);
        cull.set_sky_params(0, params);
        cull.set_sky_params(super::super::sky_vis::DIRECTION_COUNT, params);

        assert!(cull.arm_main_target_count(&device, 2, model, 17));
        cull.prepare(&device, &queue);
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut enc);
        assert!(!cull.copy_sky0_target_count(&mut enc),
            "prepared but undispatched zenith cannot publish zero");
        assert!(cull.dispatch_sky(&mut enc, super::super::sky_vis::DIRECTION_COUNT));
        assert!(!cull.copy_sky0_target_count(&mut enc),
            "GI view 5 must not witness zenith view 0");
        assert!(cull.dispatch_sky(&mut enc, 0));
        assert!(cull.copy_sky0_target_count(&mut enc));
        assert!(cull.sky0_target_count_before_submit(2));
        queue.submit(std::iter::once(enc.finish()));
        assert!(cull.sky0_target_count_submitted(2));
        assert!(cull.sky0_target_count_commit(2));
        let sky_view = super::super::sky_vis::build_views(glam::Vec3::ZERO,
            &super::super::sky_vis::SkyVisSettings::default())[0];
        let cache_key = super::super::interior_cached_count::Key::new(
            0, 1, 1, sky_view, cull.instance_epoch(), cull.sky_count_cull_key(0).unwrap());
        let mut cached_count = super::super::interior_cached_count::CachedCount::default();
        cached_count.publish(cache_key, cull.sky0_target_count_fact().identity);
        for _ in 0..64 {
            cull.harvest_sky0_target_count(&device);
            if cull.sky0_target_count_fact().status == MainCountStatus::Present { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let present = cull.sky0_target_count_fact();
        assert_eq!(present.identity.source_generation, 17);
        assert_eq!(present.status, MainCountStatus::Present);
        assert_eq!(present.count, 8);
        cached_count.observe(present);
        assert_eq!(cached_count.fact(Some(cache_key)), present);

        // Production caches this layer until the snapped VP or instance epoch
        // changes. A new token with no refresh has no fresh cull or draw.
        assert!(cull.arm_main_target_count(&device, 3, model, 18));
        cached_count.source_changed(cull.sky0_target_count_fact().identity);
        assert_eq!(cached_count.fact(Some(cache_key)).status, MainCountStatus::Unknown);
        cull.prepare(&device, &queue);
        let mut cached = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut cached);
        assert!(cull.dispatch_sky(&mut cached, super::super::sky_vis::DIRECTION_COUNT));
        assert!(!cull.copy_sky0_target_count(&mut cached));
        assert!(!super::super::sky0_count_pass_ready(true, &[], true));
        assert!(!super::super::sky0_count_pass_ready(true, &[1], true));
        assert!(!super::super::sky0_count_pass_ready(true, &[0], false));
        assert!(!super::super::sky0_count_pass_ready(false, &[0], true));
        assert!(super::super::sky0_count_pass_ready(true, &[0], true));
        assert_eq!(cull.sky0_target_count_fact().status, MainCountStatus::Unknown);
        cull.sky0_target_count_abort(3);

        assert!(cull.arm_main_target_count(&device, 4, absent_model, 19));
        cull.prepare(&device, &queue);
        let mut zero = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut zero);
        assert!(cull.dispatch_sky(&mut zero, 0));
        assert!(cull.copy_sky0_target_count(&mut zero));
        assert!(cull.sky0_target_count_before_submit(4));
        queue.submit(std::iter::once(zero.finish()));
        assert!(cull.sky0_target_count_submitted(4));
        assert!(cull.sky0_target_count_commit(4));
        let absent_key = super::super::interior_cached_count::Key::new(
            0, 2, 1, sky_view, cull.instance_epoch(), cull.sky_count_cull_key(0).unwrap());
        cached_count.publish(absent_key, cull.sky0_target_count_fact().identity);
        for _ in 0..64 {
            cull.harvest_sky0_target_count(&device);
            if cull.sky0_target_count_fact().status == MainCountStatus::Absent { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        let absent = cull.sky0_target_count_fact();
        assert_eq!(absent.identity.model_id, absent_model);
        assert_eq!(absent.status, MainCountStatus::Absent);
        assert_eq!(absent.count, 0);
        cached_count.observe(absent);
        assert_eq!(cached_count.fact(Some(absent_key)), absent);
        assert_eq!(cached_count.fact(Some(cache_key)).status, MainCountStatus::Unknown);

        assert!(cull.arm_main_target_count(&device, 5, model, 20));
        cull.prepare(&device, &queue);
        let mut aborted = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut aborted);
        assert!(cull.dispatch_sky(&mut aborted, 0));
        assert!(cull.copy_sky0_target_count(&mut aborted));
        assert!(cull.sky0_target_count_before_submit(5));
        queue.submit(std::iter::once(aborted.finish()));
        assert!(cull.sky0_target_count_submitted(5));
        cull.sky0_target_count_abort(5);
        cached_count.invalidate();
        for _ in 0..64 {
            cull.harvest_sky0_target_count(&device);
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        assert_eq!(cull.sky0_target_count_fact().status, MainCountStatus::Unknown);
        cached_count.observe(absent);
        assert_eq!(cached_count.fact(Some(absent_key)).status, MainCountStatus::Unknown);
        assert!(cull.sky0_count_slots.as_ref().unwrap().iter()
            .all(|(_, state)| matches!(state, MainCountSlot::Idle)),
            "aborted submitted copy must drain");

        // The four tilted sky views and local tile 1 use independent words in
        // one staging slot. Tile 1's daytime cull index follows four cascades.
        for i in 1..=4 { cull.set_sky_params(i, params); }
        cull.set_shadow_view_count(&device, 6);
        cull.set_shadow_params(5, params);
        assert!(cull.arm_main_target_count(&device, 6, model, 21));
        cull.prepare(&device, &queue);
        let mut batch = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut batch);
        for i in 1..=4 { assert!(cull.dispatch_sky(&mut batch, i)); }
        assert!(cull.dispatch_shadow(&mut batch, 5));
        assert!(cull.copy_batch_target_count(&mut batch, 0b1111, 1 << 1, 4, 1 << 5));
        assert!(cull.batch_target_count_before_submit(6));
        queue.submit(std::iter::once(batch.finish()));
        assert!(cull.batch_target_count_submitted(6));
        assert!(cull.batch_target_count_commit(6));
        assert_eq!(cull.batch_target_count_committed_sky_mask(6), 0b1111);
        assert_eq!(cull.batch_target_count_committed_local_mask(6), 1 << 1);
        let local_tile = super::super::local_publication::LocalTileIdentity {
            tile_index: 1, key: 42, instance_epoch: cull.instance_epoch(),
            shape: (2048, 4, 2, 5), cull_index: 5,
        };
        let mut local_publication = super::super::local_publication::LocalTilePublication::default();
        assert!(local_publication.plan(local_tile));
        local_publication.cull_recorded(5);
        local_publication.draw_closed(5);
        local_publication.final_submit_accepted();
        let (generation, published) = local_publication.commit(local_tile).unwrap();
        let local_key = super::super::local_cached_count::Key::current(generation, published,
            local_tile.shape, local_tile.shape, 5, Some(42), cull.instance_epoch(),
            cull.shadow_count_cull_key(local_tile.cull_index).unwrap()).unwrap();
        let mut local_cache = super::super::local_cached_count::CachedCount::default();
        local_cache.publish(local_key, cull.batch_target_count_fact(4).identity);
        let sky_views = super::super::sky_vis::build_views(glam::Vec3::ZERO,
            &super::super::sky_vis::SkyVisSettings::default());
        let mut interior_caches = [super::super::interior_cached_count::CachedCount::default(); 4];
        let interior_keys = std::array::from_fn::<_, 4, _>(|index|
            super::super::interior_cached_count::Key::new(index + 1, 1, 1,
                sky_views[index + 1], cull.instance_epoch(),
                cull.sky_count_cull_key(index + 1).unwrap()));
        for index in 0..4 {
            interior_caches[index].publish(interior_keys[index], cull.batch_target_count_fact(index).identity);
        }
        for _ in 0..64 {
            cull.harvest_batch_target_count(&device);
            if cull.batch_target_count_fact(4).status == MainCountStatus::Present { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        for i in 0..=4 {
            let fact = cull.batch_target_count_fact(i);
            assert_eq!((fact.status, fact.count), (MainCountStatus::Present, 8));
            assert_eq!(fact.identity.source_generation, 21);
            if i < 4 {
                interior_caches[i].observe(fact);
                assert_eq!(interior_caches[i].fact(Some(interior_keys[i])), fact);
            }
        }
        local_cache.observe(cull.batch_target_count_fact(4));
        assert_eq!(local_cache.fact(Some(local_key)), cull.batch_target_count_fact(4));
        assert_eq!(local_cache.fact(None).status, MainCountStatus::Unknown);
        for i in 0..4 {
            let mut wrong_direction = interior_keys[i];
            wrong_direction.direction = (i + 2) % 5;
            assert_eq!(interior_caches[i].fact(Some(wrong_direction)).status, MainCountStatus::Unknown);
        }
        assert_eq!(cull.batch_target_count_fact(5).status, MainCountStatus::Unknown);
        assert!(cull.batch_count.as_ref().unwrap().slots.iter()
            .all(|(_, state)| matches!(state, BatchCountSlot::Idle)));

        assert!(cull.arm_main_target_count(&device, 7, absent_model, 22));
        local_cache.source_changed(cull.batch_target_count_fact(4).identity);
        assert_eq!(local_cache.fact(Some(local_key)).status, MainCountStatus::Unknown);
        for (index, cache) in interior_caches.iter_mut().enumerate() {
            cache.source_changed(cull.batch_target_count_fact(index).identity);
            assert_eq!(cache.fact(Some(interior_keys[index])).status, MainCountStatus::Unknown);
        }
        cull.prepare(&device, &queue);
        let mut zero_batch = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut zero_batch);
        assert!(cull.dispatch_sky(&mut zero_batch, 2));
        assert!(cull.dispatch_shadow(&mut zero_batch, 5));
        assert!(cull.copy_batch_target_count(&mut zero_batch, 0b1111, 1 << 1, 4, 1 << 5));
        assert!(cull.batch_target_count_before_submit(7));
        queue.submit(std::iter::once(zero_batch.finish()));
        assert!(cull.batch_target_count_submitted(7));
        assert!(cull.batch_target_count_commit(7));
        assert_eq!(cull.batch_target_count_committed_sky_mask(7), 0b0010);
        assert_eq!(cull.batch_target_count_committed_local_mask(7), 1 << 1);
        assert!(local_publication.plan(local_tile));
        local_publication.cull_recorded(5);
        local_publication.draw_closed(5);
        local_publication.final_submit_accepted();
        let (generation, published) = local_publication.commit(local_tile).unwrap();
        let absent_local_key = super::super::local_cached_count::Key::current(generation, published,
            local_tile.shape, local_tile.shape, 5, Some(42), cull.instance_epoch(),
            cull.shadow_count_cull_key(local_tile.cull_index).unwrap()).unwrap();
        local_cache.publish(absent_local_key, cull.batch_target_count_fact(4).identity);
        let absent_interior_key = super::super::interior_cached_count::Key::new(
            2, 2, 1, sky_views[2], cull.instance_epoch(), cull.sky_count_cull_key(2).unwrap());
        interior_caches[1].publish(absent_interior_key, cull.batch_target_count_fact(1).identity);
        for _ in 0..64 {
            cull.harvest_batch_target_count(&device);
            if cull.batch_target_count_fact(4).status == MainCountStatus::Absent { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        assert_eq!(cull.batch_target_count_fact(1).status, MainCountStatus::Absent);
        interior_caches[1].observe(cull.batch_target_count_fact(1));
        assert_eq!(interior_caches[1].fact(Some(absent_interior_key)).status, MainCountStatus::Absent);
        assert_eq!(interior_caches[1].fact(Some(interior_keys[1])).status, MainCountStatus::Unknown);
        assert_eq!(cull.batch_target_count_fact(4).status, MainCountStatus::Absent);
        local_cache.observe(cull.batch_target_count_fact(4));
        assert_eq!(local_cache.fact(Some(absent_local_key)).status, MainCountStatus::Absent);
        assert_eq!(local_cache.fact(Some(local_key)).status, MainCountStatus::Unknown);
        assert_eq!(cull.batch_target_count_fact(0).status, MainCountStatus::Unknown,
            "a cached or draw-less direction must not inherit a zero");

        assert!(cull.arm_main_target_count(&device, 8, model, 23));
        cull.prepare(&device, &queue);
        let mut abort_batch = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut abort_batch);
        assert!(cull.dispatch_sky(&mut abort_batch, 1));
        assert!(cull.dispatch_shadow(&mut abort_batch, 5));
        assert!(cull.copy_batch_target_count(&mut abort_batch, 1, 1 << 1, 4, 1 << 5));
        assert!(cull.batch_target_count_before_submit(8));
        queue.submit(std::iter::once(abort_batch.finish()));
        assert!(cull.batch_target_count_submitted(8));
        cull.batch_target_count_abort(8);
        assert_eq!(cull.batch_target_count_committed_sky_mask(8), 0);
        assert_eq!(cull.batch_target_count_committed_local_mask(8), 0);
        local_publication.abort();
        local_cache.invalidate();
        interior_caches[1].invalidate();
        for _ in 0..64 {
            cull.harvest_batch_target_count(&device);
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        assert_eq!(cull.batch_target_count_fact(0).status, MainCountStatus::Unknown);
        assert_eq!(cull.batch_target_count_fact(4).status, MainCountStatus::Unknown);
        interior_caches[1].observe(cull.batch_target_count_fact(1));
        assert_eq!(interior_caches[1].fact(Some(absent_interior_key)).status, MainCountStatus::Unknown);
        assert!(cull.batch_count.as_ref().unwrap().slots.iter()
            .all(|(_, state)| matches!(state, BatchCountSlot::Idle)));
        // With no solar cascade, the same local tile is cull index 1.
        cull.set_shadow_view_count(&device, 2);
        cull.set_shadow_params(1, params);
        assert!(cull.arm_main_target_count(&device, 9, model, 24));
        cull.prepare(&device, &queue);
        let mut night = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut night);
        assert!(cull.dispatch_shadow(&mut night, 1));
        assert!(cull.copy_batch_target_count(&mut night, 0, 1 << 1, 0, 1 << 1));
        assert!(cull.batch_target_count_before_submit(9));
        queue.submit(std::iter::once(night.finish()));
        assert!(cull.batch_target_count_submitted(9));
        assert!(cull.batch_target_count_commit(9));
        assert_eq!(cull.batch_target_count_committed_local_mask(9), 1 << 1);
        for _ in 0..64 {
            cull.harvest_batch_target_count(&device);
            if cull.batch_target_count_fact(4).status == MainCountStatus::Present { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        assert_eq!(cull.batch_target_count_fact(4).count, 8);
        assert_eq!(cull.batch_target_count_fact(4).identity.source_generation, 24);
        let night_tile = super::super::local_publication::LocalTileIdentity {
            shape: (2048, 0, 2, 6), cull_index: 1, ..local_tile
        };
        assert!(local_publication.plan(night_tile));
        local_publication.cull_recorded(1);
        local_publication.draw_closed(1);
        local_publication.final_submit_accepted();
        let (generation, published) = local_publication.commit(night_tile).unwrap();
        let night_key = super::super::local_cached_count::Key::current(generation, published,
            night_tile.shape, night_tile.shape, 1, Some(42), cull.instance_epoch(),
            cull.shadow_count_cull_key(night_tile.cull_index).unwrap()).unwrap();
        local_cache.publish(night_key, cull.batch_target_count_fact(4).identity);
        local_cache.observe(cull.batch_target_count_fact(4));
        assert_eq!(local_cache.fact(Some(night_key)).count, 8);
        assert_eq!(local_cache.fact(Some(absent_local_key)).status, MainCountStatus::Unknown);

        // The last atlas tile uses the last batch word, even at night.
        cull.set_shadow_view_count(&device, 24);
        cull.set_shadow_params(23, params);
        assert!(cull.arm_main_target_count(&device, 10, model, 25));
        cull.prepare(&device, &queue);
        let mut last = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut last);
        assert!(cull.dispatch_shadow(&mut last, 23));
        assert!(cull.copy_batch_target_count(&mut last, 0, 1 << 23, 0, 1 << 23));
        assert!(cull.batch_target_count_before_submit(10));
        queue.submit(std::iter::once(last.finish()));
        assert!(cull.batch_target_count_submitted(10));
        assert!(cull.batch_target_count_commit(10));
        assert_eq!(cull.batch_target_count_committed_local_mask(10), 1 << 23);
        let last_tile = super::super::local_publication::LocalTileIdentity {
            tile_index: 23, key: 99, instance_epoch: cull.instance_epoch(),
            shape: (2048, 0, 24, 7), cull_index: 23,
        };
        let mut last_publication = super::super::local_publication::LocalTilePublication::default();
        assert!(last_publication.plan(last_tile));
        last_publication.cull_recorded(23);
        last_publication.draw_closed(23);
        last_publication.final_submit_accepted();
        let (generation, published) = last_publication.commit(last_tile).unwrap();
        let last_key = super::super::local_cached_count::Key::current(generation, published,
            last_tile.shape, last_tile.shape, 1, Some(99), cull.instance_epoch(),
            cull.shadow_count_cull_key(last_tile.cull_index).unwrap()).unwrap();
        let mut last_cache = super::super::local_cached_count::CachedCount::default();
        last_cache.publish(last_key, cull.batch_target_count_fact(26).identity);
        for _ in 0..64 {
            cull.harvest_batch_target_count(&device);
            if cull.batch_target_count_fact(26).status == MainCountStatus::Present { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        last_cache.observe(cull.batch_target_count_fact(26));
        assert_eq!(last_cache.fact(Some(last_key)).count, 8);
        assert_eq!(cull.batch_target_count_fact(25).status, MainCountStatus::Unknown);

        assert!(cull.arm_main_target_count(&device, 11, absent_model, 26));
        last_cache.source_changed(cull.batch_target_count_fact(26).identity);
        assert_eq!(last_cache.fact(Some(last_key)).status, MainCountStatus::Unknown);
        cull.prepare(&device, &queue);
        let mut last_zero = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        cull.begin_frame_stats(&mut last_zero);
        assert!(cull.dispatch_shadow(&mut last_zero, 23));
        assert!(cull.copy_batch_target_count(&mut last_zero, 0, 1 << 23, 0, 1 << 23));
        assert!(cull.batch_target_count_before_submit(11));
        queue.submit(std::iter::once(last_zero.finish()));
        assert!(cull.batch_target_count_submitted(11));
        assert!(cull.batch_target_count_commit(11));
        assert_eq!(cull.batch_target_count_committed_local_mask(11), 1 << 23);
        assert!(last_publication.plan(last_tile));
        last_publication.cull_recorded(23);
        last_publication.draw_closed(23);
        last_publication.final_submit_accepted();
        let (generation, published) = last_publication.commit(last_tile).unwrap();
        let last_zero_key = super::super::local_cached_count::Key::current(generation, published,
            last_tile.shape, last_tile.shape, 1, Some(99), cull.instance_epoch(),
            cull.shadow_count_cull_key(last_tile.cull_index).unwrap()).unwrap();
        last_cache.publish(last_zero_key, cull.batch_target_count_fact(26).identity);
        for _ in 0..64 {
            cull.harvest_batch_target_count(&device);
            if cull.batch_target_count_fact(26).status == MainCountStatus::Absent { break; }
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
        }
        last_cache.observe(cull.batch_target_count_fact(26));
        assert_eq!(last_cache.fact(Some(last_zero_key)).status, MainCountStatus::Absent);
        assert_eq!(last_cache.fact(Some(last_zero_key)).count, 0);
    }
}
