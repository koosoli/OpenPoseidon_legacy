use rustc_hash::FxHashMap;
use slotmap::{Key, KeyData, SlotMap};
use wgpu::util::DeviceExt;

pub(crate) mod pool;
use pool::{GeometryPool, MeshAlloc};

// GPU cull + LOD + indirect-arg compaction (docs/gpu-culling-and-depth-plan.md Stage 3).
// Stage 3a builds the data model + compute + frustum math; the live data source (C++
// retained-scene FFI) and dispatch/submission land in Stage 3b, so the items are unused
// for now.
#[allow(dead_code)]
pub mod cull;

// Hi-Z depth pyramid for GPU-driven occlusion culling (docs/gpu-culling-and-depth-plan.md §5).
mod gtao_depth_mips;
mod hiz;
// AST-012A: GPU mip feedback — the finest mip each bindless slot was actually asked for.
pub mod mip_feedback;
use mip_feedback::MipFeedback;
pub mod sky_bake;
pub mod sky_vis;
use sky_vis::{SkyVisSettings, SkyVisView};

use crate::ffi::{
    DRAW3D_ON_SURFACE, DRAW3D_ZBIAS_MASK, DRAW3D_ZBIAS_SHIFT, NO_PALETTE, WgrBlend, WgrCamera,
    WgrCmd, WgrCmdKind, WgrDepthMode, WgrDraw3D, WgrInstance, WgrLight, WgrMat4, WgrMeshVertex,
    WgrModelLod, WgrModelMaterial, WgrModelSection, WgrShadowCaster, WgrShadowPass, WgrVec4,
};
use crate::grass::Grass;
use crate::textures::SharedTextures;

// Depth + stencil: the stencil aspect gives per-poly shadow exclusion (a pixel is
// darkened by at most one shadow polygon, so overlapping shadow casters don't
// compound — mirrors GL33's stencil EQUAL 0 / INCR shadow path).
pub const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth24PlusStencil8;

// Depth+normal prepass G-buffer target (docs/depth-prepass-plan.md, decision 9): a
// view-space octahedral normal, Rg16Float (compact + banding-free for SSAO/GTAO/SSR).
// Written unconditionally by the prepass; sampled by no consumer yet (Stage 1).
pub const NORMAL_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rg16Float;
// AO + bent normal in ONE target: rgb = bent normal (view space), a = ambient visibility.
//
// R8Unorm would be plenty of precision for a bare visibility term, but it is NOT a core WebGPU
// storage-texture format: creating the target with STORAGE_BINDING silently invalidates the
// texture AND every bind-group layout naming the format, which surfaces far downstream as
// "TextureView is invalid" on the shared camera bind group. Rgba16Float is core-guaranteed for
// write-only storage, carries the Stage-2 bent normal in the same fetch, and lets the bilateral
// blur filter direction and visibility with identical weights (see gtao.wgsl).
pub const AO_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Cascade shadow depth maps: one D32 array layer per cascade.
const SHADOW_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
// Single-sample target the MSAA depth is resolved into (depth-only; the resolve keeps depth
// but drops stencil, which no depth consumer samples). Depth32Float samples as Depth like the
// 1x depth aspect, so the Hi-Z copy layout is unchanged.
const RESOLVED_DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
const MAX_CASCADES: u32 = 4;
// The shadow pass UBO's slot reserved for the interior sky-visibility map's ortho VP. It sits
// past every cascade so the two never collide however many cascades are active, which is what
// lets the sky map reuse the cascades' pass-UBO layout (and therefore their whole pipeline)
// instead of duplicating one.
const SKY_UBO_SLOT: usize = MAX_CASCADES as usize;

// Polygon-offset variants (mirror GL33's SetPolygonOffsetForDecals / ..ForShadows):
// decals nudge coplanar overlays toward the camera; ZBias overlay faces (signs) get
// a stronger, level-scaled push; shadows need a much stronger, angle-independent
// constant bias so ground shadows stay above their surface.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
enum Offset {
    None,
    Decal,
    ZBias(u8), // ZBias level 1..3
    Shadow,
}

// Identifies one 3D render-pipeline variant. Variants are built lazily as draws
// demand new (blend, depth, offset, cutout-threshold) combinations, keyed here so
// identical draws share a pipeline. `alpha_ref`/`is_shadow` are baked as
// pipeline-overridable constants, so the cutout threshold is part of the key.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct PipelineKey {
    blend: u8,           // WgrBlend
    depth: u8,           // WgrDepthMode
    offset: Offset,      // polygon-offset variant
    alpha_ref_bits: u32, // f32::to_bits of the cutout threshold
    skinned: bool,
    // Colour-pass depth-write override for the depth prepass (decision 2/4): when the
    // prepass already laid down this segment's opaque depth, the colour pass draws the
    // prepassed set GreaterEqual + write-OFF. Same key otherwise, so post-ClearDepth
    // segments (no prepass) still get the write-ON variant.
    depth_write_off: bool,
}

// Identifies one depth+normal prepass pipeline variant (docs/depth-prepass-plan.md,
// decision 3). The prepass only ever draws the opaque set (blend Opaque, offset None,
// depth TestWrite), so blend/depth/offset are fixed and drop out of the key — only the
// VS path (skinned) and the cutout threshold (foliage vs pure opaque) vary.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct PrepassKey {
    skinned: bool,
    alpha_ref_bits: u32,
}

// Per-level constant depth-bias magnitude for ZBias overlay faces (signs).
// Tunable live via WGR_ZBIAS_SCALE for z-fight debugging; default 4 per level.
fn zbias_scale() -> f32 {
    env_f32("WGR_ZBIAS_SCALE", 4.0)
}

// OnSurface decal offset magnitude (roads, footprint decals, notebook text).
// Tunable live via WGR_DECAL_SCALE; default 1 → the classic glPolygonOffset(-1,-1).
fn decal_scale() -> f32 {
    env_f32("WGR_DECAL_SCALE", 1.0)
}

// WGR_ROAD_PIXEL_CONFORM (default ON; 0 = the vertex-only conform): OnSurface draws on the
// plain per-draw path use `fs_surface`, which re-seats every FRAGMENT's depth on the terrain
// height field the vertex conform reads (shader3d.wgsl surface_pixel_depth). Owner, Arma 2
// Takistan (.tmp-shadow/a2clip): between two road vertices the road is a flat chord and the
// terrain is not, so wherever the ground bulges up between them it wins the depth test and
// pokes through the ribbon as terrain-triangle-shaped patches. No vertex-side fix can reach a
// between-vertex error; the per-pixel one hugs the rasterised ground everywhere.
pub(crate) fn road_pixel_conform() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| {
        std::env::var("WGR_ROAD_PIXEL_CONFORM")
            .map(|v| !v.is_empty() && v != "0")
            .unwrap_or(true)
    })
}

// Metres an fs_surface fragment is pulled toward the camera along its view ray after the
// re-seat, on top of the vertex path's WGR_DECAL_SCALE bias. Small on purpose: the owner has
// also seen roads THROUGH mountains, which is what over-biasing looks like.
pub(crate) fn road_pixel_lift() -> f32 {
    env_f32("WGR_ROAD_PIXEL_LIFT", 0.02).min(1.0)
}

/// Ceiling on the along-ray road lift, as a fraction of the seated distance. The grazing
/// compensation in `surface_pixel_depth` is a division by |dir.y| and so is unbounded as the
/// ray flattens; this is what stops a near-horizontal ray pulling a road far enough forward to
/// show through a rise in front of it. `WGR_ROAD_PIXEL_LIFT_MAX_FRAC` sweeps it without a
/// rebuild, which is how the shipped value was chosen.
pub(crate) fn road_pixel_lift_max_frac() -> f32 {
    // 0.0716: the owner's swept value (2026-08-30), up from the first-guess 0.04.
    env_f32("WGR_ROAD_PIXEL_LIFT_MAX_FRAC", 0.0716).clamp(0.0005, 2.0)
}

/// Along-ray lift per metre of distance, before the grazing division. `WGR_ROAD_PIXEL_LIFT_PER_M`.
pub(crate) fn road_pixel_lift_per_m() -> f32 {
    env_f32("WGR_ROAD_PIXEL_LIFT_PER_M", 0.0067).clamp(0.0, 0.2)
}

/// The GAME-log announcement (the launcher discards stderr): which arm drew the roads.
pub(crate) fn road_pixel_conform_log_line() -> String {
    if road_pixel_conform() {
        format!(
            "wgpu road pixel conform: ON (WGR_ROAD_PIXEL_CONFORM=1) -- OnSurface draws re-seat \
             depth per fragment on the terrain height field, lift {:.3} m (WGR_ROAD_PIXEL_LIFT) \
             + 0.5 mm/m",
            road_pixel_lift()
        )
    } else {
        "wgpu road pixel conform: OFF (WGR_ROAD_PIXEL_CONFORM=0) -- vertex-only conform, \
         terrain can poke through the road ribbon between road vertices"
            .to_string()
    }
}

// Fraction of a NON-FIXTURE material's emissive kept at full night; see the `emissive_night`
// override in shader3d.wgsl for the measurement that motivates it. Default 0: the imported
// corpora use the lane as fixed-function brightness compensation (Arma 2 vegetation 2.6, Arma 3
// tree crowns 1.0), and at 1.0 whole forests self-illuminated at midnight -- surviving
// WGR_AUTO_EXPOSURE=0, so exposure was never the cause. Recognised fixtures are exempt via
// WGR_DRAW3D_NIGHT_EMITTER / WGR_MODEL_SECTION_NIGHT_EMITTER. WGR_EMISSIVE_NIGHT=1 is the A/B
// back to the old behaviour. Shared by the per-draw and the GPU-driven pipelines.
pub(crate) fn emissive_night() -> f32 {
    env_f32("WGR_EMISSIVE_NIGHT", 0.0).clamp(0.0, 1.0)
}

// Per-mip alpha gain for cutout foliage (gbuffer.wgsl mip_alpha_boost). Owner, Arma 2: "the
// leaves of trees only render when one comes extremely close" -- the canopy dissolves to
// speckle past ~15 m while its shadow stays full, because a sparse leaf card's mip alpha sinks
// under the 0.5 cutoff a few mips down. 0.5 per mip is the customary starting point;
// WGR_CUTOUT_MIP_ALPHA=0 restores the old behaviour for an A/B.
// REN-OBJ-001: whether vegetation cutouts sample GTAO + the interior-sky volume per fragment.
// Default OFF (they keep their canopy term); WGR_FOLIAGE_SCREEN_AO=1 is the A/B and the old look.
pub(crate) fn foliage_screen_ao() -> f32 {
    static V: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("WGR_FOLIAGE_SCREEN_AO")
            .map(|v| if v.trim() == "1" { 1.0 } else { 0.0 })
            .unwrap_or(0.0)
    })
}

// REN-OBJ-003: WGR_OBJECT_COUNT_FRAGMENTS=1 builds the object pipelines with the fragment census
// on. Off by default: a counting run's timings are not quotable.
pub(crate) fn count_fragments() -> f32 {
    static V: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("WGR_OBJECT_COUNT_FRAGMENTS")
            .map(|v| if v.trim() == "1" { 1.0 } else { 0.0 })
            .unwrap_or(0.0)
    })
}

// REN-OBJ-004: the prepassed colour pipelines (both families) test depth before the fragment
// shader runs. ON by default since 2026-09-03: Everon perf_combat, three alternating runs per
// arm, Objects colour alpha-test 1.77 -> 0.29 ms, colour solid 0.37 -> 0.08, GPU frame
// 22.4 -> 19.1, crops identical. WGR_OBJECT_EARLY_Z=0 restores the late test; inert on a
// device without SHADER_EARLY_DEPTH_TEST, where the early entries are not compiled at all.
pub(crate) fn object_early_z() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        crate::shaders::early_depth_supported()
            && !std::env::var("WGR_OBJECT_EARLY_Z").map(|v| v.trim() == "0").unwrap_or(false)
    })
}

pub(crate) fn cutout_mip_alpha() -> f32 {
    env_f32("WGR_CUTOUT_MIP_ALPHA", 0.5).clamp(0.0, 4.0)
}

// Restore the historical PANIC when a per-frame staging allocation is refused.
//
// OFF by default: a refused allocation is an out-of-memory condition, and unwinding the
// render thread for it turns a recoverable frame into a lost one. Measured on DayZ
// Chernarus at 2.85x the residency budget (6.13 GB tracked / 2.15 GB budget): 697 identical
// `.expect("world staging view")` unwinds in a single ten-minute run. Set
// WGR_STAGING_STRICT=1 to get the panic (and a backtrace) back for a reproducible failure.
fn staging_strict() -> bool {
    static STRICT: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *STRICT.get_or_init(|| std::env::var("WGR_STAGING_STRICT").is_ok_and(|v| v != "0"))
}

// 1, 10, 100, ... — the throttle for a diagnostic whose failure mode repeats every frame.
fn is_power_of_ten(n: u64) -> bool {
    let mut v = n;
    if v == 0 {
        return false;
    }
    while v % 10 == 0 {
        v /= 10;
    }
    v == 1
}

pub(crate) fn env_f32(name: &'static str, default: f32) -> f32 {
    use std::collections::HashMap;
    use std::sync::{Mutex, OnceLock};
    static CACHE: OnceLock<Mutex<HashMap<&'static str, f32>>> = OnceLock::new();
    let mut map = CACHE
        .get_or_init(|| Mutex::new(HashMap::new()))
        .lock()
        .unwrap();
    *map.entry(name).or_insert_with(|| {
        std::env::var(name)
            .ok()
            .and_then(|v| v.parse::<f32>().ok())
            // `> 0.0` here silently swallowed the single most useful value these knobs have.
            // Every one of them is a SCALE multiplied into an effect, so 0 means "ablate this
            // and show me the frame without it" — and it was being turned back into 1.0, i.e.
            // the effect at FULL strength. So `WGR_DECAL_SCALE=0` did not disable the decal
            // depth bias, it left it exactly as it was, and every prior conclusion of the form
            // "ablated the decal bias, nothing changed, therefore the bias is not the mechanism"
            // is void: that ablation never ran. Accept 0, reject only negatives and NaN
            // (NaN is excluded by the comparison, which is false for NaN in either direction).
            .filter(|v| *v >= 0.0 && v.is_finite())
            .unwrap_or(default)
    })
}

impl PipelineKey {
    fn from_draw(d: &WgrDraw3D, skinned: bool) -> Self {
        let zbias_level = ((d.flags & DRAW3D_ZBIAS_MASK) >> DRAW3D_ZBIAS_SHIFT) as u8;
        let offset = if d.blend == WgrBlend::Shadow {
            Offset::Shadow
        } else if d.flags & DRAW3D_ON_SURFACE != 0 {
            Offset::Decal
        } else if zbias_level > 0 {
            Offset::ZBias(zbias_level)
        } else {
            Offset::None
        };
        PipelineKey {
            blend: d.blend as u8,
            depth: d.depth as u8,
            offset,
            alpha_ref_bits: d.alpha_ref.to_bits(),
            skinned,
            depth_write_off: false,
        }
    }

    // The prepassed opaque set (decision 4): opaque blend, no polygon offset, depth
    // test+write. Foliage (alpha_ref > 0) qualifies; skinned qualifies. Transparents,
    // decals/ZBias and the shadow-darken pass do not. Derives entirely from the key.
    fn prepassed(&self) -> bool {
        self.blend == WgrBlend::Opaque as u8
            && self.offset == Offset::None
            && self.depth == WgrDepthMode::TestWrite as u8
    }

    fn with_write_off(mut self) -> Self {
        self.depth_write_off = true;
        self
    }
}

// The engine's bone-palette cap (MATRIX_4_ARRAY(matrix, 128)); one skinned draw
// occupies this many matrices in the palette pool and in the shader UBO.
const PALETTE_SIZE: usize = 128;

// Per-draw material UBO size: six vec4 (emissive, sun_ambient, sun_diffuse,
// light_diffuse, light_ambient, specular), matching the `Material` struct in
// shader3d.wgsl.
const MATERIAL_SIZE: u64 = 112;

// Fixed capacity of the frame-global light storage buffer (group 0). The
// active count per frame rides in WgrCamera::cam_pos.w; slots beyond it aren't read.
const MAX_LIGHTS: u64 = 256;

// One per-draw entry in the world storage buffer, indexed by @builtin(instance_index).
// The world matrix plus the terrain-conform plane (see WgrDraw3D::conform* and
// the `Object` struct in shader3d.wgsl). Widened from a bare matrix so vegetation can
// upload one shared undeformed mesh and conform per instance in the vertex shader.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct ObjectGpu {
    world: WgrMat4,
    conform0: WgrVec4,
    conform1: WgrVec4,
    conform2: WgrVec4,
}

// Per-draw material lighting, uploaded into the group(1)/binding(1) UBO. Fields
// are already folded on the C++ side (WgrDraw3D::mat_*); this is just the GPU
// layout. Only rgb is read by the shader.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct MaterialUbo {
    emissive: [f32; 4],
    sun_ambient: [f32; 4],
    sun_diffuse: [f32; 4],
    light_diffuse: [f32; 4],
    light_ambient: [f32; 4],
    specular: [f32; 4],
    normal_map: [f32; 4], // x packed texture/sampler, y debug view, z enabled
}

// One indexed indirect draw command, the layout every backend's
// draw_indexed_indirect / multi_draw_indexed_indirect consumes (Vulkan/DX/Metal all
// agree, 20 bytes). Stage 2 (docs/gpu-culling-and-depth-plan.md) builds these on the
// CPU from the instancing plan; Stage 3's cull compute writes the same layout. Under
// indirect the pool buffers are bound WHOLE, so `base_vertex` = the mesh's vbase and
// `first_index` = its ibase + the section start; `first_instance` = the bucket's
// base_instance (needs the INDIRECT_FIRST_INSTANCE feature), selecting each instance's
// world/material slot exactly as the direct path's base_instance range does.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct DrawIndexedIndirectArgs {
    index_count: u32,
    instance_count: u32,
    first_index: u32,
    base_vertex: i32,
    first_instance: u32,
}

const INDIRECT_ARG_SIZE: u64 = std::mem::size_of::<DrawIndexedIndirectArgs>() as u64;

// Column-major identity, packed into the per-instance world SSBO / ShadowCasterGpu for
// a baked skinned draw: the compute bake already folded the camera-relative world into
// the palette (palette[i] = world * bone[i]), so the rigid pipeline must NOT re-apply it.
const IDENTITY_MAT4: WgrMat4 = [
    1.0, 0.0, 0.0, 0.0, //
    0.0, 1.0, 0.0, 0.0, //
    0.0, 0.0, 1.0, 0.0, //
    0.0, 0.0, 0.0, 1.0,
];

// The per-draw entry for the world SSBO (REN-TEMP-002). A draw the skin bake covers
// (bake is on AND prepare_skin_bake planned this (palette_slot, mesh)) MUST pack an
// identity world + no conform: the C++ side folds the camera-relative world into every
// palette matrix (EngineWgpu.cpp BeginMeshTL), so the baked vertices are already
// world-transformed, and the rigid pipeline the baked draw routes through applies
// objects[instance].world on top of them. Packing the real world here applies the
// world TWICE and moves every skinned character far off-screen — the "invisible
// soldiers, floating rigid weapon proxies" DLSS symptom. The predicate must mirror
// baked_draw_base exactly (same map, same key), or draw routing and world packing
// disagree — which is precisely how the bug shipped: 728f4035 disconnected routing and
// (consistently) disabled this identity pack; 60c9d63c reconnected routing but not the
// pack. Free function so the seam is testable without a device (see
// baked_world_packing tests).
fn object_gpu_for_draw(
    d: &WgrDraw3D,
    bake_on: bool,
    base_map: &FxHashMap<(u32, u64), u32>,
) -> ObjectGpu {
    let baked =
        bake_on && d.palette_slot != NO_PALETTE && base_map.contains_key(&(d.palette_slot, d.mesh));
    if baked {
        ObjectGpu {
            world: IDENTITY_MAT4,
            conform0: [0.0; 4],
            conform1: [0.0; 4],
            conform2: [0.0; 4],
        }
    } else {
        ObjectGpu {
            world: d.world,
            conform0: d.conform0,
            conform1: d.conform1,
            conform2: d.conform2,
        }
    }
}

// One compute-skin-bake dispatch (docs/compute-skin-bake-plan.md): all instances of one
// skinned mesh, baked into `skinned_vbuf` starting at `out_base_vertex`. Phase 1 always
// has instance_count == 1 (one dispatch per distinct palette_slot); Phase 2 flattens
// same-mesh instances into a single dispatch. `palette_base` is the absolute palette
// block of instance 0 (== the draw/caster's palette_slot).
struct BakeGroup {
    mesh: MeshKey,
    palette_base: u32,
    out_base_vertex: u32,
    instance_count: u32,
    vert_count: u32,
    // The mesh's first vertex in the shared geometry pool: the bake reads its rest-pose
    // source from `pool.vbuf` starting here (docs/gpu-culling-and-depth-plan.md §2.1).
    in_base_vertex: u32,
}

// Per-dispatch uniform for the skin bake, mirrored by `BakeParams` in skin_bake.wgsl.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct BakeParamsGpu {
    vert_count: u32,
    instance_count: u32,
    palette_base: u32,
    out_base_vertex: u32,
    // First source vertex of this mesh in the shared pool vbuf (the bake now reads its
    // rest pose from the pool, not a per-mesh buffer). Padded to a 16-byte multiple so
    // the dynamic-offset UBO stride stays aligned.
    in_base_vertex: u32,
    _pad: [u32; 3],
}

// Bytes per baked vertex = one WgrMeshVertex (pos+norm+uv+conform+tangent/binormal).
const BAKED_VERT_SIZE: u64 = std::mem::size_of::<WgrMeshVertex>() as u64;

// REN-TEMP-001H: the rigid dynamic-object velocity pass (velocity_obj.wgsl). Owned by
// Gfx3d because it re-draws pool geometry — the pool, the mesh table and the depth
// resolve are all private here, and draw_one's addressing (vbuf sliced to vbase,
// first_index = ibase + index_begin) must be mirrored exactly.
struct VelocityObjPass {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    uniform: wgpu::Buffer,
    ssbo: wgpu::Buffer,
    ssbo_cap: u64,
    bind: Option<wgpu::BindGroup>,
    bound_depth_gen: u64,
    // REN-TEMP-001I skinned variant: previous pose from the ping-ponged bake buffer.
    // Its bind is rebuilt every frame — the prev buffer identity alternates by design.
    skin_pipeline: wgpu::RenderPipeline,
    skin_layout: wgpu::BindGroupLayout,
    skin_ssbo: wgpu::Buffer,
    skin_ssbo_cap: u64,
    // REN-TEMP-001T vegetation-sway variant: cutout cards are double-sided, so its
    // pipeline draws uncculled; its bind is rebuilt per frame (uniform + instances).
    veg_pipeline: wgpu::RenderPipeline,
    veg_layout: wgpu::BindGroupLayout,
    veg_uniform: wgpu::Buffer,
    veg_ssbo: wgpu::Buffer,
    veg_ssbo_cap: u64,
}

impl VelocityObjPass {
    // All pipeline/BGL construction for both variants lives here so the headless device
    // test can build them without a full Gfx3d — a lazily-built pipeline that first runs
    // in-game is exactly the validation gap the test suite exists to close.
    fn new(device: &wgpu::Device) -> Self {
        {
            let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some("wgr_velocity_obj_shader"),
                source: wgpu::ShaderSource::Wgsl(include_str!("velocity_obj.wgsl").into()),
            });
            let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_velocity_obj_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Storage { read_only: true },
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                ],
            });
            let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_velocity_obj_pipeline_layout"),
                bind_group_layouts: &[Some(&layout)],
                immediate_size: 0,
            });
            // Position-only view of the pool vertex layout: same stride, one attribute.
            let vbuf_layout = wgpu::VertexBufferLayout {
                array_stride: BAKED_VERT_SIZE,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &wgpu::vertex_attr_array![0 => Float32x3],
            };
            let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_velocity_obj_pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_velocity"),
                    buffers: &[vbuf_layout],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    front_face: wgpu::FrontFace::Cw,
                    cull_mode: Some(wgpu::Face::Back),
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_velocity"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rg16Float,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            let uniform_buf = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_velocity_obj_uniform"),
                size: std::mem::size_of::<crate::temporal::ObjVelUniform>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let ssbo = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_velocity_obj_worlds"),
                size: 128 * 128,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            // Skinned variant: bindings 0 (mats), 3 (per-instance prev base), 4 (prev
            // baked floats), 2 (depth) — group indices match velocity_obj.wgsl.
            let skin_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_velocity_skin_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 3,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Storage { read_only: true },
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 4,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Storage { read_only: true },
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
            let skin_pipeline_layout =
                device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                    label: Some("wgr_velocity_skin_pipeline_layout"),
                    bind_group_layouts: &[Some(&skin_layout)],
                    immediate_size: 0,
                });
            let skin_vbuf_layout = wgpu::VertexBufferLayout {
                array_stride: BAKED_VERT_SIZE,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &wgpu::vertex_attr_array![0 => Float32x3],
            };
            let skin_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_velocity_skin_pipeline"),
                layout: Some(&skin_pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_skinned_velocity"),
                    buffers: &[skin_vbuf_layout],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    front_face: wgpu::FrontFace::Cw,
                    cull_mode: Some(wgpu::Face::Back),
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_velocity"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rg16Float,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            let skin_ssbo = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_velocity_skin_insts"),
                size: 16 * 256,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            // Vegetation-sway variant: bindings 0 (mats), 2 (depth), 5 (sway params),
            // 6 (instances) — indices match velocity_obj.wgsl.
            let veg_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_velocity_veg_layout"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 5,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 6,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Storage { read_only: true },
                            has_dynamic_offset: false,
                            min_binding_size: None,
                        },
                        count: None,
                    },
                ],
            });
            let veg_pipeline_layout =
                device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                    label: Some("wgr_velocity_veg_pipeline_layout"),
                    bind_group_layouts: &[Some(&veg_layout)],
                    immediate_size: 0,
                });
            let veg_vbuf_layout = wgpu::VertexBufferLayout {
                array_stride: BAKED_VERT_SIZE,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &wgpu::vertex_attr_array![0 => Float32x3],
            };
            let veg_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_velocity_veg_pipeline"),
                layout: Some(&veg_pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_veg_velocity"),
                    buffers: &[veg_vbuf_layout],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    front_face: wgpu::FrontFace::Cw,
                    // Vegetation cards are double-sided; culling would drop half the
                    // canopy's velocity.
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_velocity"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rg16Float,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            });
            let veg_uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_velocity_veg_uniform"),
                size: std::mem::size_of::<crate::temporal::VegVelUniform>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let veg_ssbo = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_velocity_veg_insts"),
                size: 80 * 512,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            VelocityObjPass {
                pipeline,
                layout,
                uniform: uniform_buf,
                ssbo,
                ssbo_cap: 128 * 128,
                bind: None,
                bound_depth_gen: u64::MAX,
                skin_pipeline,
                skin_layout,
                skin_ssbo,
                skin_ssbo_cap: 16 * 256,
                veg_pipeline,
                veg_layout,
                veg_uniform,
                veg_ssbo,
                veg_ssbo_cap: 80 * 512,
            }
        }
    }
}


// Blocking copy of one D32 texture layer into `out` (row 0 = top).
fn read_depth_layer(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    tex: &wgpu::Texture,
    res: u32,
    layer: u32,
    out: &mut [f32],
) -> bool {
    let res = res as usize;
    let unpadded = (res * 4) as u32;
    let padded =
        unpadded.div_ceil(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT) * wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("wgr_depth_readback"),
        size: padded as u64 * res as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("wgr_depth_readback"),
    });
    encoder.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: tex,
            mip_level: 0,
            origin: wgpu::Origin3d {
                x: 0,
                y: 0,
                z: layer,
            },
            aspect: wgpu::TextureAspect::DepthOnly,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &buf,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(padded),
                rows_per_image: Some(res as u32),
            },
        },
        wgpu::Extent3d {
            width: res as u32,
            height: res as u32,
            depth_or_array_layers: 1,
        },
    );
    queue.submit(std::iter::once(encoder.finish()));

    let slice = buf.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    if device.poll(wgpu::PollType::wait_indefinitely()).is_err() || !matches!(rx.recv(), Ok(Ok(())))
    {
        return false;
    }
    let data = slice.get_mapped_range();
    for y in 0..res {
        let row = &data[y * padded as usize..y * padded as usize + unpadded as usize];
        out[y * res..(y + 1) * res].copy_from_slice(bytemuck::cast_slice(row));
    }
    drop(data);
    buf.unmap();
    true
}

// Synchronous read of the first `words` u32s of a GPU buffer. Diagnostic use only (it stalls on
// a device poll); the buffer must carry COPY_SRC.
fn read_u32_buffer(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    src: &wgpu::Buffer,
    words: u64,
) -> Vec<u32> {
    let bytes = words * 4;
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("wgr_u32_readback"),
        size: bytes,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("wgr_u32_readback"),
    });
    encoder.copy_buffer_to_buffer(src, 0, &staging, 0, bytes);
    queue.submit(std::iter::once(encoder.finish()));
    let slice = staging.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    if device.poll(wgpu::PollType::wait_indefinitely()).is_err() || !matches!(rx.recv(), Ok(Ok(())))
    {
        return Vec::new();
    }
    let data = slice.get_mapped_range();
    let out = bytemuck::cast_slice::<u8, u32>(&data).to_vec();
    drop(data);
    staging.unmap();
    out
}

slotmap::new_key_type! {
    struct MeshKey;
}

struct Mesh {
    // Where this mesh's geometry lives in the shared GeometryPool (docs/
    // gpu-culling-and-depth-plan.md §2.1). `vbase`/`ibase` are the mesh's first vertex
    // / first index in the pool; indices are stored 0-based mesh-local (Uint32).
    alloc: MeshAlloc,
    index_count: u32,
    vert_count: u32,
    // Per-vertex skin data (4 bone indices + 4 weights, 8 bytes/vertex); present
    // only for skinned meshes. Standalone (0-based), bound at vertex slot 1 with
    // base_vertex = 0 alongside the pool vbuf sliced to `vbase`.
    skin: Option<wgpu::Buffer>,
    // Model-space AABB, computed once here while the vertices are still on the CPU. The
    // sky-visibility bake (docs/interior-sky-visibility-plan.md §3c) needs a model's extent to
    // place its volume, and this is the only moment the positions are cheaply available —
    // afterwards they live in the GPU pool and recovering them means a readback.
    aabb_min: [f32; 3],
    aabb_max: [f32; 3],
}

// One GPU-driven section's registration source: the mesh handle it lives in, its mesh-local
// index range, and its pipeline variant. Kept so base_vertex / first_index can be re-resolved
// from the current mesh alloc each frame (the pool can move a mesh on VB recreate).
#[derive(Clone, Copy)]
struct GpuSectionSrc {
    mesh: u64,
    index_begin: u32,
    index_count: u32,
    variant: u32,
}

struct ShadowTarget {
    tex: wgpu::Texture,
    layer_views: Vec<wgpu::TextureView>,
    sample_view: wgpu::TextureView,
    res: u32,
    layers: u32,
}

struct ShadowPipelines {
    solid: wgpu::RenderPipeline,
    alpha: wgpu::RenderPipeline,
    skin_solid: wgpu::RenderPipeline,
    skin_alpha: wgpu::RenderPipeline,
}

impl ShadowPipelines {
    fn get(&self, skinned: bool, alpha: bool) -> &wgpu::RenderPipeline {
        match (skinned, alpha) {
            (false, false) => &self.solid,
            (false, true) => &self.alpha,
            (true, false) => &self.skin_solid,
            (true, true) => &self.skin_alpha,
        }
    }
}

// One per-caster entry in the shadow-caster storage buffer, indexed by
// @builtin(instance_index) (the draw's base_instance). The cutout threshold is baked as
// a pipeline override (always 0.5), so no alpha_ref rides here and the fragment stage
// never touches this buffer.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct ShadowCasterGpu {
    world: WgrMat4,
    conform0: [f32; 4], // x = bcSurfaceY (mode 2)
    conform2: [f32; 4], // z = conform mode (0 = none, 2 = per-vertex ClipLand heightmap)
}

// One instanced draw in a cascade's shadow plan (see prepare_shadows). `repr` is a
// caster index supplying the mesh/section/texture/sampler/skin shared by the bucket;
// `base..base+count` is the base_instance range into the reordered caster SSBO.
struct ShadowBucket {
    repr: u32,
    base: u32,
    count: u32,
}

// Coalesce key for instanceable (non-skinned) shadow casters. Solid casters sample no
// texture, so their texture/sampler are normalized to 0 in the key to let same-mesh
// solids merge regardless of their (unused) material.
#[derive(PartialEq, Eq, Hash)]
struct ShadowBucketKey {
    mesh: u64,
    index_begin: u32,
    index_count: u32,
    alpha: bool,
    texture_id: u64,
    sampler: u32,
}

// group(0) of the shadow depth pass: the light view-projection for one cascade plus
// the camera world position (casters are camera-relative, so surface_y needs cam_pos
// to reconstruct absolute world xz). One per cascade (dynamic offset).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct ShadowPassUbo {
    light_vp: WgrMat4,
    cam_pos: [f32; 4],
}

// Group 0 of the lit 3D pipelines: the per-camera UBO (dynamic offset) plus the
// cascade shadow map + comparison sampler. The bind group is recreated when the
// UBO regrows or the shadow target changes (tracked by `shadow_gen`).
struct CameraGroup {
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    stride: u64,
    bind_size: u64,
    buf: Option<wgpu::Buffer>,
    cap: u64,
    bind: Option<wgpu::BindGroup>,
    bound_shadow_gen: u64,
    // Frame-global light storage buffer (binding 3). Fixed capacity, created
    // once, so it stays valid across camera-UBO regrowth / shadow-target swaps.
    lights_buf: wgpu::Buffer,
    // Terrain sun-shadow (bindings 4-6): a clamping filter sampler and the
    // world->UV mapping uniform (both created once); the mask texture is owned by
    // Terrain and lent by view, so the bind rebuilds when its generation changes.
    mask_sampler: wgpu::Sampler,
    mapping_buf: wgpu::Buffer,
    bound_mask_gen: u64,
    // Generation of the GTAO target bound at @binding(11); see Gfx3d::depth_gen.
    bound_ao_gen: u64,
    // Generation of the interior sky-visibility map bound at @binding(12); see
    // Gfx3d::interior_sky_gen.
    bound_interior_sky_gen: u64,
}

impl CameraGroup {
    fn new(device: &wgpu::Device) -> Self {
        // The GPU `Frame` UBO is the WgrCamera bytes plus a Rust-appended `inv_view_proj`
        // (mat4, 64 B), the foliage knob block (3×vec4, 48 B = sizeof(WgrFoliage)), the clip
        // plane (vec4) and the GTAO knobs (vec4), written after each camera in the upload loop —
        // so the bind size is NOT the raw C-ABI size. Keep the three in sync (see prepare's
        // camera upload + frame.wgsl).
        let bind_size = std::mem::size_of::<WgrCamera>() as u64
            + 64
            + std::mem::size_of::<crate::ffi::WgrFoliage>() as u64
            + 16
            + 16
            // Interior sky visibility: one ortho VP (mat4) and one direction vec4 per sampled
            // sky direction, plus two knob vec4s.
            + 64 * sky_vis::DIRECTION_COUNT as u64
            + 16 * sky_vis::DIRECTION_COUNT as u64
            + 16
            + 16
            // Stage 2 baked-volume knobs.
            + 16
            // Material Debug lane for the retained path (frame.wgsl `matdbg`).
            + 16
            // DZ-005 water surface animation (frame.wgsl `wateranim`): time, ripple
            // strength, flow strength, flow-map bindless slot.
            + 16
            // Normal-map relief under ambient light (frame.wgsl `relief`): cavity exponent,
            // sky-specular gain. The LAST lane — the two write_buffer calls at the end of the
            // camera upload address matdbg and wateranim from `bind_size` backwards, so adding
            // here moves them and both offsets are updated with it.
            + 16
            // Far-fog closure (frame.wgsl `fogfar`): the fraction of the draw distance at which
            // distance fog reaches full. The three writes above it are addressed from
            // bind_size backwards and all three moved with it.
            + 16
            // REN-TEMP-001D render-scale texture LOD (frame.wgsl `renscale`): mip bias +
            // gradient scale. NOW the last lane; the four writes above it moved again.
            + 16;
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_3d_camera_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    // The grass placement compute pass reuses Frame.camera at this
                    // dynamic offset; the remaining camera-group resources retain
                    // their graphics-only visibility.
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT | wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true,
                        min_binding_size: wgpu::BufferSize::new(bind_size),
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                    count: None,
                },
                // Frame-global point/spot lights, read-only storage. Shared by the
                // lit-mesh + terrain pipelines (terrain reuses this exact layout).
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                            crate::ffi::WgrLight,
                        >() as u64),
                    },
                    count: None,
                },
                // Long-range terrain sun-shadow mask (Rgba16Float, filterable) +
                // its clamping sampler + the world->UV mapping uniform. Written by
                // the terrain compute sweep; sampled here so lit meshes (not just
                // terrain) receive a mountain's cast shadow.
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                            crate::terrain::TerrainShadowMap,
                        >() as u64),
                    },
                    count: None,
                },
                // Aerial-perspective froxel volume (3D) + its sampler (the mask sampler
                // is reused for it). Sampled per-fragment by the lit-mesh + terrain
                // fragment shaders (frame::froxel_fog). Owned by Sky, lent by view.
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D3,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                // SH-9 sky-irradiance coefficients (Sky-owned, lent by buffer), for directional sky
                // ambient on the lit-mesh + terrain fragment shaders (frame::sky_irradiance).
                wgpu::BindGroupLayoutEntry {
                    binding: 9,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(9 * 16),
                    },
                    count: None,
                },
                // Coarse sky-visibility (sky-view factor) mask (Terrain-owned, R8Unorm, lent by view),
                // sampled with the terrain-shadow sampler (binding 5) + mapping (binding 6) to modulate
                // ambient by terrain sky occlusion (frame::terrain_sky_visibility).
                wgpu::BindGroupLayoutEntry {
                    binding: 10,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Screen-space AO, blurred (Gfx3d-owned, R8Unorm, lent by view). Non-filterable:
                // it is read with textureLoad at the fragment's own pixel, never interpolated —
                // it is already a per-pixel screen-space quantity, so there is nothing to filter.
                wgpu::BindGroupLayoutEntry {
                    binding: 11,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Interior sky-visibility depth map (Gfx3d-owned, Depth32Float, lent by view):
                // the top-down ortho map of the retained object set. Sampled with the COMPARISON
                // sampler at binding 2 — its LessEqual compare IS the "is my depth at or above the
                // stored occluder" test, and the hardware 2x2 PCF gives the softening kernel its
                // sub-texel gradient for free.
                wgpu::BindGroupLayoutEntry {
                    binding: 12,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                // CLD-020 cloud sun-transmittance map. Filterable float: the map is coarse
                // (~8 m per texel) and its whole job is to be sampled smoothly.
                wgpu::BindGroupLayoutEntry {
                    binding: 13,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });
        let lights_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_lights"),
            size: MAX_LIGHTS * std::mem::size_of::<crate::ffi::WgrLight>() as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Bilinear 2x2 hardware PCF per compare tap; LessEqual = lit when the
        // receiver is at or in front of the stored occluder depth.
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_shadow_compare_sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            compare: Some(wgpu::CompareFunction::LessEqual),
            ..Default::default()
        });
        let mask_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_terrain_shadow_mask_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });
        let mapping_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_terrain_shadow_mapping"),
            size: std::mem::size_of::<crate::terrain::TerrainShadowMap>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Dynamic uniform offsets must be multiples of the device alignment. The
        // camera block itself is larger than that alignment, so round its size UP to
        // the next alignment multiple instead of using its raw size as the alignment.
        let align = device.limits().min_uniform_buffer_offset_alignment as u64;
        CameraGroup {
            layout,
            sampler,
            stride: bind_size.div_ceil(align) * align,
            bind_size,
            buf: None,
            cap: 0,
            bind: None,
            bound_shadow_gen: u64::MAX,
            lights_buf,
            mask_sampler,
            mapping_buf,
            bound_mask_gen: u64::MAX,
            bound_ao_gen: u64::MAX,
            bound_interior_sky_gen: u64::MAX,
        }
    }

    // Upload the frame's active lights (clamped to the buffer capacity).
    // The per-camera count travels separately in WgrCamera::cam_pos.w.
    fn upload_lights(&self, queue: &wgpu::Queue, lights: &[crate::ffi::WgrLight]) {
        let n = lights.len().min(MAX_LIGHTS as usize);
        if n > 0 {
            queue.write_buffer(&self.lights_buf, 0, bytemuck::cast_slice(&lights[..n]));
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn ensure(
        &mut self,
        device: &wgpu::Device,
        count: usize,
        shadow_view: &wgpu::TextureView,
        shadow_gen: u64,
        mask_view: &wgpu::TextureView,
        mask_gen: u64,
        froxel_view: &wgpu::TextureView,
        sky_sh_buf: &wgpu::Buffer,
        skyvis_view: &wgpu::TextureView,
        ao_view: &wgpu::TextureView,
        ao_gen: u64,
        interior_sky_view: &wgpu::TextureView,
        interior_sky_gen: u64,
        cloud_shadow_view: &wgpu::TextureView,
    ) {
        let needed = count as u64 * self.stride;
        let grow = self.cap < needed || self.buf.is_none();
        if grow {
            let cap = needed.next_power_of_two().max(self.stride * 8);
            self.buf = Some(device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_3d_camera_ubo"),
                size: cap,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }));
            self.cap = cap;
        }
        if grow
            || self.bound_shadow_gen != shadow_gen
            || self.bound_mask_gen != mask_gen
            // The AO target is reallocated on every resize, so the bind group must follow it or
            // it keeps a view of a destroyed texture.
            || self.bound_ao_gen != ao_gen
            // Same reason as the AO target: the sky map is reallocated when its resolution
            // changes (or dropped when the feature is turned off), and a stale bind group would
            // hold a view of a destroyed texture.
            || self.bound_interior_sky_gen != interior_sky_gen
            || self.bind.is_none()
        {
            self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_3d_camera_bind"),
                layout: &self.layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                            buffer: self.buf.as_ref().unwrap(),
                            offset: 0,
                            size: wgpu::BufferSize::new(self.bind_size),
                        }),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: wgpu::BindingResource::TextureView(shadow_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::Sampler(&self.sampler),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: self.lights_buf.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 4,
                        resource: wgpu::BindingResource::TextureView(mask_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 5,
                        resource: wgpu::BindingResource::Sampler(&self.mask_sampler),
                    },
                    wgpu::BindGroupEntry {
                        binding: 6,
                        resource: self.mapping_buf.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 7,
                        resource: wgpu::BindingResource::TextureView(froxel_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 8,
                        resource: wgpu::BindingResource::Sampler(&self.mask_sampler),
                    },
                    wgpu::BindGroupEntry {
                        binding: 9,
                        resource: sky_sh_buf.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 10,
                        resource: wgpu::BindingResource::TextureView(skyvis_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 11,
                        resource: wgpu::BindingResource::TextureView(ao_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 12,
                        resource: wgpu::BindingResource::TextureView(interior_sky_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 13,
                        resource: wgpu::BindingResource::TextureView(cloud_shadow_view),
                    },
                ],
            }));
            self.bound_shadow_gen = shadow_gen;
            self.bound_mask_gen = mask_gen;
            self.bound_ao_gen = ao_gen;
            self.bound_interior_sky_gen = interior_sky_gen;
        }
    }

    // Upload the world->UV shadow-mask mapping for this frame (cheap; overwrites).
    fn upload_mapping(&self, queue: &wgpu::Queue, mapping: &crate::terrain::TerrainShadowMap) {
        queue.write_buffer(&self.mapping_buf, 0, bytemuck::bytes_of(mapping));
    }
}

// Holds a dynamic uniform buffer + its bind group, regrown as the frame needs.
struct DynUbo {
    layout: wgpu::BindGroupLayout,
    stride: u64,
    bind_size: u64,
    buf: Option<wgpu::Buffer>,
    bind: Option<wgpu::BindGroup>,
    cap: u64,
    // Distinct buffer label (e.g. "wgr_3d_palette") so per-instance writes are
    // identifiable in a capture instead of a shared "wgr_dyn_ubo".
    label: &'static str,
}

impl DynUbo {
    fn new(
        device: &wgpu::Device,
        label: &'static str,
        bind_size: u64,
        visibility: wgpu::ShaderStages,
    ) -> Self {
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some(label),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: true,
                    min_binding_size: wgpu::BufferSize::new(bind_size),
                },
                count: None,
            }],
        });
        let align = device
            .limits()
            .min_uniform_buffer_offset_alignment
            .max(bind_size as u32) as u64;
        let stride = bind_size.div_ceil(align) * align;
        DynUbo {
            layout,
            stride,
            bind_size,
            buf: None,
            bind: None,
            cap: 0,
            label,
        }
    }

    // Ensure capacity for `count` entries; (re)create buffer + bind group on
    // growth. Returns true when the buffer was (re)created, so callers that build
    // their own combined bind groups over `buf` know to rebuild them.
    fn ensure(&mut self, device: &wgpu::Device, count: usize) -> bool {
        let needed = count as u64 * self.stride;
        if self.cap >= needed && self.buf.is_some() {
            return false;
        }
        let cap = needed.next_power_of_two().max(self.stride * 64);
        let buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some(self.label),
            size: cap,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_dyn_ubo_bind"),
            layout: &self.layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                    buffer: &buf,
                    offset: 0,
                    size: wgpu::BufferSize::new(self.bind_size),
                }),
            }],
        }));
        self.buf = Some(buf);
        self.cap = cap;
        true
    }
}

// Growable read-only storage buffer holding one packed element per draw, uploaded
// with a single `write_buffer` and indexed in-shader by `@builtin(instance_index)`
// (fed as the draw's `base_instance`). This replaces the per-draw dynamic-offset
// UBO uploads — one `write_buffer` per draw — that dominated frame-start
// buffer-copy/barrier traffic, and lays out the per-instance data exactly as
// Stage-3 instancing needs it (a run of instances = a contiguous slot range).
struct StorageArray {
    buf: Option<wgpu::Buffer>,
    cap: u64,
    label: &'static str,
}

impl StorageArray {
    fn new(label: &'static str) -> Self {
        StorageArray {
            buf: None,
            cap: 0,
            label,
        }
    }

    // Ensure capacity for `bytes`; (re)create the buffer on growth. Returns true
    // when the buffer moved, so callers rebuild any bind group that borrows it.
    fn ensure(&mut self, device: &wgpu::Device, bytes: u64) -> bool {
        if self.cap >= bytes && self.buf.is_some() {
            return false;
        }
        let cap = bytes.next_power_of_two().max(4096);
        self.buf = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some(self.label),
            size: cap,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        self.cap = cap;
        true
    }

    // Drop the buffer and forget its capacity, so the next `ensure` builds a fresh one.
    //
    // `ensure` records the size it ASKED for, not the size it GOT: wgpu answers an
    // out-of-memory `create_buffer` with an *invalid* buffer handle rather than an error
    // return, and `cap >= bytes` is then true forever after. Without this, one transient
    // OOM poisons the field for the rest of the process and every later upload into it is
    // refused — which is exactly the shape of the 697 consecutive staging failures measured
    // on DayZ Chernarus, not 697 independent ones.
    fn invalidate(&mut self) {
        self.buf = None;
        self.cap = 0;
    }
}

// Build a combined group-1 bind group. For the plain pipeline both bindings are
// whole-buffer read-only storage (world @0, material @1), indexed by
// instance_index and bound once per frame. For the skinned pipeline binding 0 is
// instead the dynamic-offset bone palette (`slot0_dynamic` = Some(one-block size)).
fn build_group1_bind(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    slot0: &wgpu::Buffer,
    // Some(one-block size) → dynamic-offset palette (skinned); None → whole-buffer
    // read-only storage (plain world array, indexed by instance_index).
    slot0_dynamic: Option<u64>,
    material: &wgpu::Buffer,
    // DZ-003: Sky's equirect reflection env map + sampler (a 1x1 dummy until Sky lends
    // the real one), for the Fresnel reflection on materials naming an EnvironmentMap.
    env_view: &wgpu::TextureView,
    env_sampler: &wgpu::Sampler,
    // AST-012A: the shared mip-feedback buffer (binding 4).
    mip_feedback: &wgpu::Buffer,
    label: &str,
) -> wgpu::BindGroup {
    let slot0_binding = wgpu::BufferBinding {
        buffer: slot0,
        offset: 0,
        size: slot0_dynamic.and_then(wgpu::BufferSize::new),
    };
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(label),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::Buffer(slot0_binding),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: material.as_entire_binding(),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(env_view),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::Sampler(env_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 4,
                resource: mip_feedback.as_entire_binding(),
            },
        ],
    })
}

// Group 4 of the lit mesh pipelines: the terrain heightmap + its sampling params, so
// vs_main can conform ClipLand vegetation to the ground (SurfaceY) per vertex without
// the CPU rewriting the shared mesh. The heightmap is owned by Terrain and lent by
// view; the bind rebuilds when the heightmap generation bumps (realloc on set_heightmap).
// The R32Float heightmap is sampled with textureLoad in the vertex stage (non-filterable),
// exactly like the terrain shader's sample_height (== Landscape::SurfaceY).
struct ConformGroup {
    layout: wgpu::BindGroupLayout,
    params_buf: wgpu::Buffer,
    bind: Option<wgpu::BindGroup>,
    bound_gen: u64,
}

impl ConformGroup {
    fn new(device: &wgpu::Device) -> Self {
        // VERTEX | FRAGMENT: the vertex conform reads the heightmap per vertex; fs_surface
        // (WGR_ROAD_PIXEL_CONFORM) reads the SAME heightmap per fragment to re-seat an
        // OnSurface draw's depth on the terrain. Broadening visibility costs nothing for the
        // shaders that never touch it from the fragment stage.
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_3d_conform_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                            crate::terrain::TerrainConformParams,
                        >() as u64),
                    },
                    count: None,
                },
            ],
        });
        let params_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_3d_conform_params"),
            size: std::mem::size_of::<crate::terrain::TerrainConformParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // A 1x1 dummy heightmap so the bind is valid before terrain loads and for the
        // shadow-depth probe. Its params default to enabled=0, so surface_y() is a no-op
        // and rigid (mode 0) draws are unaffected; ensure() swaps in the real heightmap.
        let dummy = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_3d_conform_dummy_hm"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R32Float,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_view = dummy.create_view(&wgpu::TextureViewDescriptor::default());
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_3d_conform_bind_dummy"),
            layout: &layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&dummy_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: params_buf.as_entire_binding(),
                },
            ],
        });
        ConformGroup {
            layout,
            params_buf,
            bind: Some(bind),
            bound_gen: u64::MAX,
        }
    }

    // Upload the current params and (re)build the bind group when the heightmap moved
    // (generation bumped) or on first use.
    fn ensure(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        heightmap_view: &wgpu::TextureView,
        generation: u64,
        params: &crate::terrain::TerrainConformParams,
    ) {
        queue.write_buffer(&self.params_buf, 0, bytemuck::bytes_of(params));
        if self.bind.is_none() || self.bound_gen != generation {
            self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_3d_conform_bind"),
                layout: &self.layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(heightmap_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: self.params_buf.as_entire_binding(),
                    },
                ],
            }));
            self.bound_gen = generation;
        }
    }
}

// MSAA depth resolve. WebGPU has no depth resolve_target, so a tiny fullscreen pass reduces the
// multisampled depth (bound as texture_depth_multisampled_2d) to a single-sample Depth32Float
// target that the Hi-Z build (+ future SSAO / depth-based water opacity) can sample like the 1x
// depth aspect. Present only when sample_count > 1.
pub(crate) struct DepthResolve {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    // Per-size: the resolved depth target's view (both the resolve pass' depth attachment and the
    // sample view handed to depth_sample_view) + the bind group over the MSAA source depth.
    // The texture rides along for native interop (REN-TEMP-001M: NGX wants the VkImage).
    texture: Option<wgpu::Texture>,
    view: Option<wgpu::TextureView>,
    bind: Option<wgpu::BindGroup>,
}

impl DepthResolve {
    // `reduce_far` picks the per-sample reduction: false = nearest (Hi-Z occlusion), true = farthest
    // (the true seabed for water depth — skips A2C foliage/rotor edges that would ring as foam).
    pub(crate) fn new(device: &wgpu::Device, sample_count: u32, reduce_far: bool) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_depth_resolve"),
            source: wgpu::ShaderSource::Wgsl(include_str!("depth_resolve.wgsl").into()),
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_depth_resolve_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Depth,
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: true,
                },
                count: None,
            }],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_depth_resolve_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        // The FS unrolls a per-sample reduction over the source; the sample count + reduction
        // direction are spec constants so the loop bound + branch resolve at pipeline creation.
        let constants = [
            ("sample_count", sample_count as f64),
            ("reduce_far", if reduce_far { 1.0 } else { 0.0 }),
        ];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_depth_resolve_pipeline"),
            layout: Some(&pl),
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: Some(wgpu::DepthStencilState {
                format: RESOLVED_DEPTH_FORMAT,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::Always),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_main"),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &constants,
                    ..Default::default()
                },
                targets: &[],
            }),
            multiview_mask: None,
            cache: None,
        });
        Self {
            pipeline,
            layout,
            texture: None,
            view: None,
            bind: None,
        }
    }

    // (Re)allocate the resolved depth target for `w x h` and bind `src` (the MSAA depth's DepthOnly
    // aspect view) as the resolve source. Returns a clone of the resolved view for depth_sample_view.
    pub(crate) fn resize(
        &mut self,
        device: &wgpu::Device,
        w: u32,
        h: u32,
        src: &wgpu::TextureView,
    ) -> wgpu::TextureView {
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_3d_depth_resolved"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: RESOLVED_DEPTH_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_depth_resolve_bind"),
            layout: &self.layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(src),
            }],
        }));
        self.view = Some(view.clone());
        self.texture = Some(tex);
        view
    }

    // Record the resolve pass (MSAA depth -> single-sample). Recorded after the prepass depth is
    // complete and before the Hi-Z build reads the resolved view.
    pub(crate) fn resolve(&self, encoder: &mut wgpu::CommandEncoder) {
        let (Some(view), Some(bind)) = (self.view.as_ref(), self.bind.as_ref()) else {
            return;
        };
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_depth_resolve"),
            color_attachments: &[],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view,
                depth_ops: Some(wgpu::Operations {
                    load: wgpu::LoadOp::Clear(0.0),
                    store: wgpu::StoreOp::Store,
                }),
                stencil_ops: None,
            }),
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, bind, &[]);
        pass.draw(0..3, 0..1);
    }
}

// Single-sample resolve of the prepass' oct-encoded view-space normal target, the one
// input GTAO needs that the prepass does not already produce (screen-space-ao-plan §2).
// MSAA only — at 1x the prepass normal is already single-sample and this is not built.
//
// Built but NOT yet recorded per frame: nothing samples the resolved normal until the GTAO
// pass lands, and adding a fullscreen pass with no consumer would be per-frame GPU cost for
// nothing. `resolve` is called by GTAO when it arrives. Same "present, deliberately unwired"
// shape the compute skin bake uses.
#[test]
fn gtao_blur_is_edge_aware_on_both_depth_and_normal() {
    let src = include_str!("gtao_blur.wgsl");
    let module = naga::front::wgsl::parse_str(src).expect("gtao_blur.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("gtao_blur.wgsl validate");

    // With no TAA this blur IS the denoise. Both rejection terms are required and it is
    // tempting to drop the normal one as redundant: it is not, because two surfaces meeting
    // at a crease sit at nearly the same depth, so depth alone smears a wall-floor contact
    // shadow flat.
    assert!(
        src.contains("w_depth"),
        "blur must reject across depth discontinuities"
    );
    assert!(
        src.contains("w_normal"),
        "blur must reject across normal discontinuities"
    );

    // Reversed-Z is non-linear, so the depth test has to be relative. An absolute epsilon
    // tuned near the camera rejects nothing at distance, where reversed-Z values crowd.
    assert!(
        src.contains("/ max(max(dq, d_centre), 1e-6)"),
        "depth rejection must be relative, not an absolute epsilon"
    );

    // Sky must not be pulled into a surface's AO, nor filtered itself.
    assert!(
        src.contains("if (d_centre <= 0.0)"),
        "blur must early-out on sky"
    );
}

#[test]
fn gtao_validates_and_keeps_its_no_taa_constraints() {
    let src = include_str!("gtao.wgsl");
    let module = naga::front::wgsl::parse_str(src).expect("gtao.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("gtao.wgsl validate");

    // This project runs MSAA and no TAA (plan §0), so the noise has to be resolvable by a
    // spatial blur alone. A frame-varying rotation is the standard GTAO trick and is exactly
    // wrong here: with no history to accumulate into it becomes crawling per-frame noise.
    // Pin the absence, because adding one looks like an improvement.
    //
    // Scan CODE only. Scanning the raw source made this assertion fire on the word "Real-Time"
    // in a paper citation, which is a false positive that teaches you to weaken the test.
    let code: String = src
        .lines()
        .map(|l| l.split("//").next().unwrap_or(""))
        .collect::<Vec<_>>()
        .join("\n");
    for temporal in ["frame_index", "frame_count", "time", "jitter"] {
        assert!(
            !code.contains(temporal),
            "GTAO must stay spatial-only with no TAA to resolve a temporal term (found {temporal})"
        );
    }
    // Sky must be left unoccluded rather than marched: cleared reversed-Z is 0, and
    // integrating horizons against a surface that was never drawn produces garbage.
    assert!(
        src.contains("if (z >= SKY_Z * 0.5)"),
        "GTAO must early-out where nothing was drawn"
    );
    // World-space radius projected per pixel is what makes AO scale-stable.
    assert!(
        src.contains("radius / dist"),
        "GTAO radius must be world-space, projected per pixel"
    );

    // The slice must be weighted by the PROJECTED normal and its angle carried into the
    // integral. Scaling the finished slice by n.v instead is the tempting shortcut, and it
    // silently darkens flat unoccluded ground by cos(view angle) — see the numeric test below.
    assert!(
        src.contains("proj_len * (gtao_arc(hn, gamma) + gtao_arc(hp, gamma))"),
        "GTAO must weight each slice by the projected normal, not by a global n.v"
    );
    assert!(
        !src.contains("n_dot_v"),
        "GTAO must not scale slice visibility by a global n.v factor"
    );
}

#[test]
fn gtao_round_trips_a_view_point_through_depth_and_back() {
    // Full round trip across BOTH shaders: take a known view-space point, push it through the
    // exact path the geometry takes (forward projection -> frame::reverse_z's `z = w - z` ->
    // perspective divide -> depth buffer), linearise it the way gtao_depth_mips.wgsl does, then
    // reconstruct the position the way gtao.wgsl does, and require the original point back.
    //
    // This is the test that was missing when GTAO fed the raw stored depth into an inverse
    // projection. The projection is FORWARD; the reversal happens afterwards in the vertex
    // shader, so the buffer holds `1 - forward_depth`. For an infinite-far forward projection
    // that works out to exactly `near / z`, which is why the linearisation is a divide.
    //
    // Assert on the RECONSTRUCTED POSITION, not on shader text: the wrong version still
    // validates, still runs, and still produces a plausible picture — it just silently puts
    // every sample outside the search radius so no occlusion is ever found.
    let near = 0.0957_f32;
    let (proj_xx, proj_yy) = (1.4286_f32, 1.9048_f32);
    let proj = glam::Mat4::from_cols(
        glam::Vec4::new(proj_xx, 0.0, 0.0, 0.0),
        glam::Vec4::new(0.0, proj_yy, 0.0, 0.0),
        glam::Vec4::new(0.0, 0.0, 1.0, 1.0),
        glam::Vec4::new(0.0, 0.0, -near, 0.0),
    );

    for &z in &[0.5_f32, 2.0, 10.0, 95.0] {
        for &(x, y) in &[(0.0_f32, 0.0_f32), (0.4, -0.3)] {
            let p = glam::Vec3::new(x * z, y * z, z);
            // Vertex path: project, reverse-z, divide.
            let clip = proj * p.extend(1.0);
            let stored = (clip.w - clip.z) / clip.w;
            let ndc = glam::Vec2::new(clip.x / clip.w, clip.y / clip.w);

            // gtao_depth_mips.wgsl cs_linearise.
            let z_lin = near / stored.max(1e-9);
            assert!(
                (z_lin - z).abs() < 1e-3 * z.max(1.0),
                "linearisation must recover view z: sent {z}, stored {stored:.6}, got {z_lin}"
            );

            // gtao.wgsl view_pos.
            let got = glam::Vec3::new(ndc.x / proj_xx, ndc.y / proj_yy, 1.0) * z_lin;
            assert!(
                (got - p).length() < 0.01 * z.max(1.0),
                "reconstruction must recover the original point: sent {p:?}, got {got:?}"
            );
        }
    }

    // And pin both halves in the shaders, since the arithmetic above only proves the maths.
    assert!(
        include_str!("gtao_depth_mips.wgsl").contains("params.proj.x / max(d, 1e-9)"),
        "the mip chain must linearise stored depth as near / d"
    );
    assert!(
        include_str!("gtao.wgsl").contains("* z;"),
        "gtao.wgsl must scale the reconstructed ray by linear z"
    );
}

#[test]
fn gtao_reconstructs_positions_in_the_same_space_the_prepass_normals_are_in() {
    // The single most damaging way to get GTAO wrong, and it is invisible to every other test
    // here: the normal and the position must live in the SAME space. Every prepass writes
    // `frame.view * normal`, i.e. VIEW space. This engine's Frame.inv_view_proj unprojects to
    // CAMERA-RELATIVE WORLD, which differs by the camera rotation, so reaching for the matrix
    // that is already in the frame UBO — the obvious thing to do — silently rotates the normal
    // relative to everything it is dotted against.
    //
    // It does not read as noise, which is why it needs pinning. The error is constant for a given
    // face orientation, so it renders as whole walls in flat black next to whole walls in flat
    // white: structured enough to look like a feature until someone points out that real AO is
    // smooth and lives in the corners.
    for (name, src) in [
        ("shader3d.wgsl", include_str!("shader3d.wgsl")),
        ("gpu_driven.wgsl", include_str!("gpu_driven.wgsl")),
        (
            "../terrain/terrain.wgsl",
            include_str!("../terrain/terrain.wgsl"),
        ),
    ] {
        assert!(
            src.contains("frame.view * vec4<f32>("),
            "{name}'s prepass must write a VIEW-space normal; GTAO's unprojection assumes it"
        );
    }
    let gtao = include_str!("gtao.wgsl");
    // View-space positions reconstructed from linear z and the projection's scale terms.
    assert!(
        gtao.contains("vec3<f32>(ndc.x / params.proj.x, ndc.y / params.proj.y, 1.0) * z"),
        "GTAO must reconstruct VIEW-space positions from linear z, matching the prepass normals"
    );
    assert!(
        !gtao.contains("inv_view_proj"),
        "GTAO must NOT use Frame.inv_view_proj: it yields camera-relative WORLD, not view space"
    );
}

#[test]
fn gtao_resources_are_valid_on_a_real_device() {
    // The naga-only tests above validate the SHADERS. They cannot see whether the resources
    // wgpu is asked to build are legal, and that gap shipped a real bug: AO_FORMAT was R8Unorm,
    // which is not a core storage-texture format, so the AO texture and both bind-group layouts
    // naming it came back invalid. Nothing failed loudly — the breakage surfaced as
    // "TextureView is invalid" on the shared camera bind group, one frame graph away from the
    // cause, and only when the game was launched. Build the real objects here instead.
    let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
        return;
    };
    let scope = device.push_error_scope(wgpu::ErrorFilter::Validation);

    // AO_FORMAT must actually be usable as a write-only storage texture, which is the property
    // R8Unorm silently lacked.
    assert!(
        AO_FORMAT
            .guaranteed_format_features(wgpu::Features::empty())
            .allowed_usages
            .contains(wgpu::TextureUsages::STORAGE_BINDING),
        "AO_FORMAT ({AO_FORMAT:?}) must be a core storage-texture format"
    );

    let (w, h) = (64u32, 48u32);
    let depth = device
        .create_texture(&wgpu::TextureDescriptor {
            label: Some("gtao_test_depth"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Depth32Float,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        })
        .create_view(&wgpu::TextureViewDescriptor::default());
    let normal = device
        .create_texture(&wgpu::TextureDescriptor {
            label: Some("gtao_test_normal"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: NORMAL_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        })
        .create_view(&wgpu::TextureViewDescriptor::default());

    let mut mips = crate::gfx3d::gtao_depth_mips::GtaoDepthMips::new(&device);
    mips.resize(&device, w, h);
    let mut gtao = Gtao::new(&device);
    let mut blur = GtaoBlur::new(&device);
    let ao = gtao.resize(&device, w, h, mips.view().unwrap(), &normal);
    blur.resize(&device, w, h, &depth, &normal, &ao);

    // And record both dispatches, so a bad workgroup size or an unbound resource fails here too.
    gtao.upload(
        &queue,
        &GtaoParams {
            proj: [1.4286, 1.9048, 0.0957, (mips.mips() - 1) as f32],
            screen: [w as f32, h as f32, 1.0 / w as f32, 1.0 / h as f32],
            tuning: [1.5, 1.0, 3.0, 10.0],
            // Scale 1: this harness pins the per-pixel march, which is the reference path the
            // block-replicated one has to agree with on a flat surface.
            limits: [512.0, 1.0, 1.0, 0.0],
        },
    );
    blur.upload(&queue, w, h, 6.0, 24.0, 8.0);
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
    gtao.dispatch(&mut enc, w, h, 1);
    blur.dispatch(&mut enc, w, h);
    queue.submit(std::iter::once(enc.finish()));
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();

    let err = pollster::block_on(scope.pop());
    assert!(err.is_none(), "GTAO resources failed validation: {err:?}");
}

// Minimal IEEE half -> f32 for reading back an Rgba16Float target. Written out rather than
// pulling in a `half` dependency for one assertion; only finite normals/zero occur here.
#[cfg(test)]
fn f16_to_f32(bits: u16) -> f32 {
    let sign = ((bits >> 15) & 1) as u32;
    let exp = ((bits >> 10) & 0x1f) as u32;
    let frac = (bits & 0x3ff) as u32;
    let out = if exp == 0 {
        // Zero or subnormal; subnormals are far below anything asserted on, so flush to signed 0.
        sign << 31
    } else if exp == 0x1f {
        (sign << 31) | (0xff << 23) | (frac << 13)
    } else {
        (sign << 31) | ((exp + 127 - 15) << 23) | (frac << 13)
    };
    f32::from_bits(out)
}

#[test]
fn gtao_writes_full_visibility_where_nothing_was_drawn() {
    // End-to-end through the real compute pass: dispatch over a depth buffer cleared to the
    // reversed-Z far plane (0 = sky, nothing drawn) and read the AO target back.
    //
    // "It launches without validation errors" is NOT evidence the pass produced anything — a
    // dispatch whose stores never land looks identical from the log. Sky is the one input whose
    // output is exactly known (1.0, fully unoccluded) without authoring a synthetic scene, so it
    // is what pins the write path: uniform, storage binding, workgroup coverage and store.
    let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
        return;
    };
    let (w, h) = (64u32, 48u32);
    let extent = wgpu::Extent3d {
        width: w,
        height: h,
        depth_or_array_layers: 1,
    };
    let depth_tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("gtao_sky_depth"),
        size: extent,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let depth = depth_tex.create_view(&wgpu::TextureViewDescriptor::default());
    let normal_tex = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("gtao_sky_normal"),
        size: extent,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: NORMAL_FORMAT,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let normal = normal_tex.create_view(&wgpu::TextureViewDescriptor::default());

    let mut mips = crate::gfx3d::gtao_depth_mips::GtaoDepthMips::new(&device);
    mips.resize(&device, w, h);
    let mut gtao = Gtao::new(&device);
    gtao.resize(&device, w, h, mips.view().unwrap(), &normal);
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
    // Clear the depth target to the reversed-Z far plane. No draws: the whole frame is sky.
    drop(enc.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: Some("gtao_sky_clear"),
        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
            view: &normal,
            depth_slice: None,
            resolve_target: None,
            ops: wgpu::Operations {
                load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                store: wgpu::StoreOp::Store,
            },
        })],
        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
            view: &depth,
            depth_ops: Some(wgpu::Operations {
                load: wgpu::LoadOp::Clear(0.0),
                store: wgpu::StoreOp::Store,
            }),
            stencil_ops: None,
        }),
        timestamp_writes: None,
        occlusion_query_set: None,
        multiview_mask: None,
    }));
    // The chain is the pass' actual input, so build it from the cleared depth first. Full chain
    // (top_mip = every level) so this harness keeps exercising the reduce path even though the
    // shipped GtaoSettings::max_mip is 0 and the runtime build now stops at mip 0.
    mips.build(&device, &queue, &mut enc, &depth, 0.0957, mips.mips());
    // Scale 2 (the shipped default) on purpose: this test reads back EVERY texel, so it is also
    // the check that the block store covers the whole target. A store loop that dropped the last
    // column or row of a block would leave those texels never written, and they would fail the
    // full-visibility assert below rather than passing quietly.
    //
    // The params MUST be uploaded, not left zeroed: limits.z carries the same scale the dispatch
    // grid was sized from, and a zeroed uniform would make the shader walk 1x1 blocks over a grid
    // sized for 2x2 and leave three quarters of the target untouched.
    gtao.upload(
        &queue,
        &GtaoParams {
            proj: [1.4286, 1.9048, 0.0957, (mips.mips() - 1) as f32],
            screen: [w as f32, h as f32, 1.0 / w as f32, 1.0 / h as f32],
            tuning: [1.5, 1.0, 3.0, 10.0],
            limits: [512.0, 1.0, 2.0, 0.0],
        },
    );
    gtao.dispatch(&mut enc, w, h, 2);

    // Copy the AO target out. Rgba16Float = 8 B/texel; the row stride must be 256-aligned.
    let row = (w * 8).div_ceil(256) * 256;
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("gtao_sky_readback"),
        size: (row * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    // Read back the GTAO target ITSELF, not the blur's output. Going through the blur made an
    // earlier version of this test vacuous: the blur early-outs on sky and stores 1.0 without
    // consulting its input at all, so it passed with the compute pass contributing nothing.
    enc.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: gtao.ao_texture().expect("AO target allocated by resize"),
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &readback,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(row),
                rows_per_image: Some(h),
            },
        },
        extent,
    );
    queue.submit(std::iter::once(enc.finish()));

    let slice = readback.slice(..);
    slice.map_async(wgpu::MapMode::Read, |_| {});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    let data = slice.get_mapped_range();
    let mut seen = 0usize;
    for y in 0..h as usize {
        for x in 0..w as usize {
            // rgb = bent normal, a = AO; visibility is the last of four f16 lanes.
            let o = y * row as usize + x * 8 + 6;
            let v = f16_to_f32(u16::from_le_bytes([data[o], data[o + 1]]));
            assert!(
                (v - 1.0).abs() < 1e-3,
                "sky pixel ({x},{y}) must be fully unoccluded, got {v}"
            );
            seen += 1;
        }
    }
    assert_eq!(seen, (w * h) as usize, "every pixel must have been written");
    drop(data);
    readback.unmap();
}

// The GTAO slice integral, transcribed from gtao.wgsl's gtao_arc. Kept in Rust so the
// normalisation below is checkable without a GPU.
#[cfg(test)]
fn gtao_arc(h: f32, g: f32) -> f32 {
    0.25 * (g.cos() - (2.0 * h - g).cos() + 2.0 * h * g.sin())
}

#[test]
fn gtao_slice_integral_leaves_unoccluded_surfaces_fully_lit() {
    // An unoccluded surface must come out at AO = 1 whatever angle it is viewed from —
    // otherwise every flat field darkens toward the horizon and the effect reads as fog.
    // That property is NOT per-slice (a single slice can exceed 1); it emerges only from
    // weighting each slice by |projected normal| and integrating from that slice's own gamma.
    // This is what makes the shortcut of scaling by n.v wrong, so measure it rather than
    // asserting on the source alone.
    let slices = 256;
    for tilt_deg in [0.0_f32, 15.0, 30.0, 45.0, 60.0, 75.0] {
        let a = tilt_deg.to_radians();
        // View direction is +z; the normal tilts away from it in the xz plane.
        let (n_x, n_z) = (a.sin(), a.cos());
        let mut visibility = 0.0_f32;
        for s in 0..slices {
            let phi = s as f32 * std::f32::consts::PI / slices as f32;
            // In-plane basis (v, w); the normal has no y component so n.w is n_x * cos(phi).
            let n_v = n_z;
            let n_w = n_x * phi.cos();
            let proj_len = (n_v * n_v + n_w * n_w).sqrt();
            let gamma = n_w.atan2(n_v);
            // Nothing occludes: both horizons sit on the tangent plane after the clamp.
            let hp = gamma + std::f32::consts::FRAC_PI_2;
            let hn = gamma - std::f32::consts::FRAC_PI_2;
            visibility += proj_len * (gtao_arc(hn, gamma) + gtao_arc(hp, gamma));
        }
        visibility /= slices as f32;
        assert!(
            (visibility - 1.0).abs() < 0.01,
            "unoccluded AO at {tilt_deg} deg tilt should be 1.0, got {visibility}"
        );
    }
}

#[test]
fn gtao_slice_integral_darkens_as_horizons_close_in() {
    // The counterpart: with the horizons pulled in toward the view direction (a surface in a
    // pit), visibility must fall monotonically.
    //
    // Deliberately tested at a NON-ZERO gamma. At gamma = 0 the arc integral is symmetric
    // (F(-h) == F(h)), so an implementation that mishandles the negative half is
    // indistinguishable there — a seeded sign error passed a version of this test written at
    // gamma = 0. Everything sign-sensitive about this function lives off-axis.
    let gamma = 0.6_f32;
    let half_open = std::f32::consts::FRAC_PI_2;
    let mut last = f32::INFINITY;
    for closed in [0.0_f32, 0.2, 0.4, 0.6, 0.8] {
        let span = half_open * (1.0 - closed);
        let v = gtao_arc(gamma - span, gamma) + gtao_arc(gamma + span, gamma);
        assert!(
            v < last,
            "visibility must decrease as horizons close (closed={closed}, v={v}, last={last})"
        );
        assert!(v >= 0.0, "visibility must never go negative: {v}");
        last = v;
    }

    // Asymmetry check: the normal leans toward +w (gamma > 0), so most of the cosine lobe sits
    // on that side. Closing the +w horizon must therefore cost MORE visibility than closing the
    // -w horizon by the same angle. This is what actually distinguishes the two half-arcs, and
    // it is exactly what a dropped sign or a swapped h_pos/h_neg destroys.
    let bite = 0.5_f32;
    let full = gtao_arc(gamma - half_open, gamma) + gtao_arc(gamma + half_open, gamma);
    let close_pos = gtao_arc(gamma - half_open, gamma) + gtao_arc(gamma + half_open - bite, gamma);
    let close_neg = gtao_arc(gamma - half_open + bite, gamma) + gtao_arc(gamma + half_open, gamma);
    assert!(
        close_pos < close_neg && close_neg < full,
        "closing the horizon the normal faces must cost more \
         (full={full}, close_pos={close_pos}, close_neg={close_neg})"
    );
}

#[test]
fn normal_resolve_takes_a_single_sample_rather_than_averaging() {
    let src = include_str!("normal_resolve.wgsl");
    let module = naga::front::wgsl::parse_str(src).expect("normal_resolve.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("normal_resolve.wgsl validate");

    // The reduction is the whole correctness question here. Octahedral codes wrap, so a
    // texel-space average of two samples either side of the fold points nowhere near either
    // normal. Taking sample 0 is what makes this correct-if-coarse; averaging raw texels
    // would be quietly wrong, and looks more principled, so pin it.
    assert!(
        src.contains("textureLoad(src, p, 0)"),
        "normal resolve must select a sample, not blend"
    );
    for wrong in ["+ textureLoad", "* 0.25", "/ f32(sample_count)"] {
        assert!(
            !src.contains(wrong),
            "normal resolve must not average oct-encoded texels (found {wrong})"
        );
    }
}

/// AO march resolution gate. DEFAULT 2 (one march per 2x2 block, denoised back to full
/// resolution) — the owner's call after the full-res march measured as the frame's most
/// resolution-hungry pass. `WGR_GTAO_SCALE=1` restores the per-pixel march for an A/B capture or
/// for anyone who wants it back; the dev panel's AO quality control drives the same value live
/// through `Gfx3d::set_gtao_scale`.
///
/// Logs what it resolved to, like the other WGR_* gates, so a benchmark log says which path ran.
/// DZ-005 — the world-space water ripple's master strength, and its A/B switch.
///
/// Fixed once at renderer construction rather than read per frame, for the same reason
/// `MaterialIsReflective`'s `WGR_ENV_REFLECTION` is: a before/after capture must be two runs
/// of the SAME binary differing in exactly one term, and a value that can drift mid-run (or
/// be re-read after a world load) is not that. `WGR_WATER_RIPPLE=0` restores the pre-DZ-005
/// glassy surface exactly — the shader's whole world-space branch is gated on this being > 0.
/// Fraction of the draw distance at which distance fog reaches FULL, matching
/// `Engine::SkySettings::fogFarClose`. See the note where it is used for why the two must agree.
const FOG_FAR_CLOSE_DEFAULT: f32 = 0.85;

/// `WGR_FOG_CLOSE=<0.3..1.0>` -- startup override, for the same reason the sky ones exist:
/// `--benchmark` and `--test-type screenshot` run `--no-dev`, so the Sky tab is unreachable and
/// the control could not otherwise be captured or ablated at all.
fn fog_far_close_from_env() -> Option<f32> {
    let v = std::env::var("WGR_FOG_CLOSE")
        .ok()?
        .trim()
        .parse::<f32>()
        .ok()?
        .clamp(0.3, 1.0);
    eprintln!("[wgr] fog: far closure {} from WGR_FOG_CLOSE", v);
    Some(v)
}

fn water_ripple_strength_from_env() -> f32 {
    let strength = std::env::var("WGR_WATER_RIPPLE")
        .ok()
        .and_then(|v| v.parse::<f32>().ok())
        .unwrap_or(1.0)
        .clamp(0.0, 8.0);
    eprintln!("[wgr] water ripple strength: {strength} from WGR_WATER_RIPPLE (0 = glassy)");
    strength
}

/// How much of the normal map's own angular deviation the DIRECTIONAL SKY AMBIENT keeps, in
/// [0,1] — published to the shader in `frame.gtao.w` and consumed by `shading::shade`.
///
/// 1 (the default) rotates the bent/interior sky-ambient direction by the same rotation the
/// normal map applied to this pixel, so a normal-mapped surface has relief in its ambient term.
/// **0 restores the pre-fix look exactly**: the shader skips the branch and samples along the
/// unmodified steer, which is what shipped before this knob existed. That is the A/B arm.
///
/// Read through `env_f32`, which caches per name, so the value is fixed for the run — an A/B
/// must be two runs of the same binary differing in exactly one term.
pub(crate) fn ambient_normal_mapped() -> f32 {
    env_f32("WGR_AMBIENT_NORMAL_MAPPED", 1.0).clamp(0.0, 1.0)
}

/// The one-line announcement of the above, for the GAME log. Not `eprintln!`: the launcher
/// discards stderr, so a lever announced only there cannot be proven to have fired from a
/// captured run — the same lesson the grass card-spacing knob paid for.
pub(crate) fn ambient_normal_mapped_log_line() -> String {
    let w = ambient_normal_mapped();
    format!(
        "wgpu ambient normal-mapping: {w:.2} (WGR_AMBIENT_NORMAL_MAPPED) — {}",
        if w <= 0.0 {
            "OFF, sky ambient samples the bent/interior steer only (pre-fix look)"
        } else {
            "sky irradiance is sampled along the normal-mapped direction, so ambient-dominated \
             surfaces (dawn/dusk/overcast/shadow/interior) keep their relief"
        }
    )
}

/// Exponent on the normal map's half-space micro-occlusion of the AMBIENT term — published in
/// `frame.relief.x` and consumed by `frame::normal_map_cavity` (objects and terrain).
///
/// **0 is off and is bit-identical to the pre-fix look** (the shader returns 1.0 before touching
/// anything), so it is the A/B arm. 1 is the provable half-space value `(1 + cos t)/2`; the
/// default of **2** is the argued one: the half-space result counts a SINGLE occluding plane, and
/// a real micro-surface occludes itself from every side, so 1 is a lower bound rather than an
/// estimate. Squaring it is the cheapest honest way to say "more than one occluder" and it keeps
/// the mean cost small — for a typical `_nohq` (mean deviation ~20 degrees) the mean ambient loses
/// ~6%, i.e. about 2 levels at the measured dawn wall level of 38.8, while a 45-degree facet loses
/// 27%. The complaint being answered is about CONTRAST, so the term is shaped to spend its budget
/// on the steep pixels and almost nothing on the flat ones.
///
/// Values above ~4 read as a painted-on dirt map: the surface stops looking lit and starts looking
/// textured with shadow. Not clamped to that, because judging it is the owner's job, but the log
/// line says what was asked for.
pub(crate) fn normal_cavity() -> f32 {
    env_f32("WGR_NORMAL_CAVITY", 2.0).clamp(0.0, 8.0)
}

/// Gain on the grazing-angle SKY SPECULAR — published in `frame.relief.y`, consumed by
/// `frame::sky_specular_split` on the object paths only (terrain has no gloss to drive it).
///
/// 1 (the default) is the physical term: a Fresnel-weighted mirror-direction sky lookup, with the
/// diffuse sky scaled by exactly the weight the specular took, so the ambient is REALLOCATED and
/// never increased. That is why the default is 1 rather than something timid — the standing
/// complaint that sunlit walls blow out cannot be caused by a term that conserves energy by
/// construction. 0 skips it and is bit-identical to the pre-fix look.
pub(crate) fn sky_specular() -> f32 {
    env_f32("WGR_SKY_SPECULAR", 1.0).clamp(0.0, 1.0)
}

/// The one-line announcement of both, for the GAME log — same reasoning as
/// `ambient_normal_mapped_log_line`: the launcher discards stderr, so a lever announced only
/// there cannot be matched to a captured dawn screenshot.
pub(crate) fn normal_relief_log_line() -> String {
    let cav = normal_cavity();
    let spec = sky_specular();
    format!(
        "wgpu normal relief: cavity {cav:.2} (WGR_NORMAL_CAVITY), sky specular {spec:.2} \
         (WGR_SKY_SPECULAR) — {}",
        match (cav > 0.0, spec > 0.0) {
            (false, false) =>
                "BOTH OFF, ambient carries no per-pixel normal detail beyond the SH steer \
                 (pre-fix look)",
            (true, false) => "cavity only: crevices darken under ambient, no sky specular",
            (false, true) => "sky specular only: grazing/glossy relief, no cavity darkening",
            (true, true) =>
                "the normal map occludes its own ambient and reflects the sky at grazing angles; \
                 both multiply/reallocate, neither adds energy",
        }
    )
}

/// WGR_GTAO=0 hard-disables screen-space AO (see `Gfx3d::gtao_force_off`). Read once at renderer
/// construction so an A/B is two runs of the same binary differing in exactly one term.
fn gtao_force_off_from_env() -> bool {
    let off = std::env::var("WGR_GTAO").map(|v| v == "0").unwrap_or(false);
    if off {
        eprintln!(
            "[wgr] gtao: FORCED OFF by WGR_GTAO=0 (the ambient AO term stays 1.0 everywhere)"
        );
    }
    off
}

fn gtao_scale_from_env() -> u32 {
    let scale = std::env::var("WGR_GTAO_SCALE")
        .ok()
        .and_then(|v| v.parse::<u32>().ok())
        .unwrap_or(2)
        .clamp(1, 2);
    eprintln!(
        "[wgr] gtao march scale: {scale} ({}) from WGR_GTAO_SCALE",
        if scale == 1 {
            "one march per pixel"
        } else {
            "one march per 2x2 block, bilateral-upsampled by the denoise"
        }
    );
    scale
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub(crate) struct GtaoParams {
    // x = proj[0][0], y = proj[1][1], z = near plane, w = highest mip index in the depth chain.
    // No inverse-projection matrix: the chain stores LINEAR view z, so a view position is
    // (ndc/proj_scale, 1) * z. See gtao.wgsl.
    pub proj: [f32; 4],
    // xy = size in px, zw = 1/size.
    pub screen: [f32; 4],
    // x = world radius (m), y = strength, z = slices, w = steps per slice.
    pub tuning: [f32; 4],
    // x = max screen radius (px, a sanity bound), y = thickness falloff, z = AO scale
    // (screen pixels per marched sample, 1 or 2 — see cs_gtao), w unused.
    pub limits: [f32; 4],
}

const _: () = assert!(std::mem::size_of::<GtaoParams>() == 64);

// GTAO compute pass (screen-space-ao-plan section 3). Owns its AO target and its own
// uniform rather than riding the frame group: that group is shared by every 3D pipeline,
// so extending it is the LAST step of this feature, not the first — a layout change with
// nothing bound fails validation in every pass at once.
//
// Built but not dispatched yet; the bilateral blur and the ambient consumers come next.
pub(crate) struct Gtao {
    pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,
    params: wgpu::Buffer,
    // The texture as well as its view: a view alone keeps it alive, but the texture handle is
    // what a copy_texture_to_buffer needs, which is how the AO buffer gets read back and checked.
    tex: Option<wgpu::Texture>,
    view: Option<wgpu::TextureView>,
    bind: Option<wgpu::BindGroup>,
}

impl Gtao {
    pub(crate) fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_gtao"),
            source: wgpu::ShaderSource::Wgsl(include_str!("gtao.wgsl").into()),
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_gtao_layout"),
            entries: &[
                // The linear-view-Z mip chain, not the depth target: GTAO marches it by mip.
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    // Non-filterable on purpose: the shader textureLoads exact texels.
                    // Oct-encoded normals must never be bilinearly sampled — interpolating
                    // across the octahedral fold gives a direction near neither neighbour.
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
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
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: AO_FORMAT,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                    count: None,
                },
            ],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_gtao_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_gtao_pipeline"),
            layout: Some(&pl),
            module: &module,
            entry_point: Some("cs_gtao"),
            compilation_options: Default::default(),
            cache: None,
        });
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_gtao_params"),
            size: std::mem::size_of::<GtaoParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        Self {
            pipeline,
            layout,
            params,
            tex: None,
            view: None,
            bind: None,
        }
    }

    // (Re)allocate the AO target and bind the prepass inputs. `normal` must be the
    // SINGLE-SAMPLE normal — normal_sample_view() under MSAA, normal_view() at 1x.
    pub(crate) fn resize(
        &mut self,
        device: &wgpu::Device,
        w: u32,
        h: u32,
        depth: &wgpu::TextureView,
        normal: &wgpu::TextureView,
    ) -> wgpu::TextureView {
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gtao_ao"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: AO_FORMAT,
            // COPY_SRC so the finished AO buffer can be read back — both by the test that pins
            // the pass actually writes, and by any future frame dump. Costs nothing otherwise.
            usage: wgpu::TextureUsages::STORAGE_BINDING
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.tex = Some(tex);
        self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_gtao_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(normal),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: self.params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
            ],
        }));
        self.view = Some(view.clone());
        view
    }

    pub(crate) fn upload(&self, queue: &wgpu::Queue, params: &GtaoParams) {
        queue.write_buffer(&self.params, 0, bytemuck::bytes_of(params));
    }

    // `scale` = screen pixels per marched sample; MUST equal GtaoParams.limits.z, because the
    // shader indexes blocks and this sizes the grid of blocks. Passing them separately is the
    // one way they can disagree, so the caller derives both from the same value.
    pub(crate) fn dispatch(&self, encoder: &mut wgpu::CommandEncoder, w: u32, h: u32, scale: u32) {
        let Some(bind) = self.bind.as_ref() else {
            return;
        };
        let scale = scale.max(1);
        encoder.push_debug_group("wgr_gtao");
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_gtao"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, bind, &[]);
        // One invocation per scale x scale BLOCK. Workgroup is 8x8; round up so edge blocks are
        // covered (the shader bounds-checks every store).
        pass.dispatch_workgroups(
            w.div_ceil(scale).div_ceil(8),
            h.div_ceil(scale).div_ceil(8),
            1,
        );
        drop(pass);
        encoder.pop_debug_group();
    }

    pub(crate) fn ao_view(&self) -> Option<&wgpu::TextureView> {
        self.view.as_ref()
    }

    #[cfg(test)]
    pub(crate) fn ao_texture(&self) -> Option<&wgpu::Texture> {
        self.tex.as_ref()
    }
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub(crate) struct GtaoBlurParams {
    // xy = size in px, zw = 1/size.
    pub screen: [f32; 4],
    // x = axis (0 = horizontal, 1 = vertical), y = radius in taps,
    // z = depth rejection scale, w = normal rejection power.
    pub tuning: [f32; 4],
}

const _: () = assert!(std::mem::size_of::<GtaoBlurParams>() == 32);

// Separable bilateral denoise over the GTAO output. Two dispatches: AO -> scratch
// (horizontal), scratch -> AO (vertical), so the result lands back in the texture the
// ambient term will sample and no consumer needs to know a scratch buffer exists.
//
// Two uniform buffers rather than one rewritten between dispatches: the axis differs per
// pass, and both dispatches are recorded into the same encoder before anything is
// submitted, so a single buffer would have both passes read whichever value was written
// last. That is a genuinely nasty bug — it would look like the blur simply being weak.
pub(crate) struct GtaoBlur {
    pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,
    params_h: wgpu::Buffer,
    params_v: wgpu::Buffer,
    scratch: Option<wgpu::TextureView>,
    bind_h: Option<wgpu::BindGroup>,
    bind_v: Option<wgpu::BindGroup>,
}

impl GtaoBlur {
    pub(crate) fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_gtao_blur"),
            source: wgpu::ShaderSource::Wgsl(include_str!("gtao_blur.wgsl").into()),
        });
        let tex = |binding: u32, sample_type: wgpu::TextureSampleType| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Texture {
                sample_type,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_gtao_blur_layout"),
            entries: &[
                tex(0, wgpu::TextureSampleType::Depth),
                // Non-filterable for the same reason as the GTAO pass: oct-encoded normals
                // must be loaded, never interpolated across the octahedral fold.
                tex(1, wgpu::TextureSampleType::Float { filterable: false }),
                tex(2, wgpu::TextureSampleType::Float { filterable: false }),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: AO_FORMAT,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                    count: None,
                },
            ],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_gtao_blur_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_gtao_blur_pipeline"),
            layout: Some(&pl),
            module: &module,
            entry_point: Some("cs_gtao_blur"),
            compilation_options: Default::default(),
            cache: None,
        });
        let mk_buf = |label| {
            device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(label),
                size: std::mem::size_of::<GtaoBlurParams>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            })
        };
        Self {
            pipeline,
            layout,
            params_h: mk_buf("wgr_gtao_blur_params_h"),
            params_v: mk_buf("wgr_gtao_blur_params_v"),
            scratch: None,
            bind_h: None,
            bind_v: None,
        }
    }

    // Allocate the scratch target and build both bind groups. `ao` is the GTAO output,
    // which is also the final destination of the vertical pass.
    pub(crate) fn resize(
        &mut self,
        device: &wgpu::Device,
        w: u32,
        h: u32,
        depth: &wgpu::TextureView,
        normal: &wgpu::TextureView,
        ao: &wgpu::TextureView,
    ) {
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gtao_blur_scratch"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: AO_FORMAT,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let scratch = tex.create_view(&wgpu::TextureViewDescriptor::default());
        let mk_bind =
            |label, src: &wgpu::TextureView, dst: &wgpu::TextureView, buf: &wgpu::Buffer| {
                device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some(label),
                    layout: &self.layout,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: wgpu::BindingResource::TextureView(depth),
                        },
                        wgpu::BindGroupEntry {
                            binding: 1,
                            resource: wgpu::BindingResource::TextureView(normal),
                        },
                        wgpu::BindGroupEntry {
                            binding: 2,
                            resource: wgpu::BindingResource::TextureView(src),
                        },
                        wgpu::BindGroupEntry {
                            binding: 3,
                            resource: buf.as_entire_binding(),
                        },
                        wgpu::BindGroupEntry {
                            binding: 4,
                            resource: wgpu::BindingResource::TextureView(dst),
                        },
                    ],
                })
            };
        self.bind_h = Some(mk_bind(
            "wgr_gtao_blur_bind_h",
            ao,
            &scratch,
            &self.params_h,
        ));
        self.bind_v = Some(mk_bind(
            "wgr_gtao_blur_bind_v",
            &scratch,
            ao,
            &self.params_v,
        ));
        self.scratch = Some(scratch);
    }

    pub(crate) fn upload(
        &self,
        queue: &wgpu::Queue,
        w: u32,
        h: u32,
        radius: f32,
        depth_scale: f32,
        normal_power: f32,
    ) {
        let screen = [
            w as f32,
            h as f32,
            1.0 / w.max(1) as f32,
            1.0 / h.max(1) as f32,
        ];
        queue.write_buffer(
            &self.params_h,
            0,
            bytemuck::bytes_of(&GtaoBlurParams {
                screen,
                tuning: [0.0, radius, depth_scale, normal_power],
            }),
        );
        queue.write_buffer(
            &self.params_v,
            0,
            bytemuck::bytes_of(&GtaoBlurParams {
                screen,
                tuning: [1.0, radius, depth_scale, normal_power],
            }),
        );
    }

    pub(crate) fn dispatch(&self, encoder: &mut wgpu::CommandEncoder, w: u32, h: u32) {
        let (Some(bh), Some(bv)) = (self.bind_h.as_ref(), self.bind_v.as_ref()) else {
            return;
        };
        encoder.push_debug_group("wgr_gtao_blur");
        for bind in [bh, bv] {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("wgr_gtao_blur"),
                timestamp_writes: None,
            });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, bind, &[]);
            pass.dispatch_workgroups(w.div_ceil(8), h.div_ceil(8), 1);
        }
        encoder.pop_debug_group();
    }
}

// Live GTAO knobs. Mirrors the C ABI WgrGtao, but kept as its own type so the renderer's
// internal defaults don't depend on the FFI struct being pushed (it is, every frame — but the
// pass has to be correct on frame 0 too, before the first push lands).
#[derive(Clone, Copy, PartialEq)]
pub struct GtaoSettings {
    pub enabled: bool,
    pub radius_m: f32,
    pub strength: f32,
    pub slices: u32,
    pub steps: u32,
    pub max_radius_px: f32,
    pub thickness: f32,
    pub blur_radius: f32,
    pub blur_depth_scale: f32,
    pub blur_normal_power: f32,
    // Raw debug view: 0 = off, 1 = AO as greyscale, 2 = bent normal as RGB. A mode rather than
    // a bool because mode 1 shows only the scalar term, so the bent normal was invisible to
    // inspection — toggling directional ambient changed nothing in the debug view and everything
    // in the lit one.
    pub debug_mode: u32,
    // Stage 2: steer the SH sky-irradiance lookup by the bent normal instead of the surface
    // normal. Separate from `enabled` because the scalar AO is worth having on its own and this
    // is the part most likely to need backing out if it looks wrong.
    pub bent_normal: bool,
    // Highest mip the horizon march may climb. 0 = every tap at full resolution (the stable
    // default). Higher trades temporal stability for reach up close — see gtao.wgsl.
    pub max_mip: u32,
}

impl Default for GtaoSettings {
    fn default() -> Self {
        // Default ON since 2026-08-05 — it HAS now been seen on a real island, and the GPU cost
        // it used to ship without is measured (Region::GtaoPrep/Compute/Blur). Kept in sync with
        // C++ Engine::AoSettings, which pushes every frame and therefore wins; this value only
        // decides frame 0.
        //
        // max_radius_px is the value that matters and it is MEASURED, not guessed. It is a cost
        // clamp, but it silently shortens the world radius whenever it bites, and at 800x600 with
        // proj_yy=1.9 the old 96 px bit for everything nearer than ~10 m:
        //
        //   dist  2 m -> 429 px wanted, capped 96 -> effective radius 0.34 m (asked for 1.5)
        //   dist  3 m -> 286 px wanted, capped 96 -> effective radius 0.50 m
        //   dist  5 m -> 171 px wanted, capped 96 -> effective radius 0.84 m
        //
        // Which is why AO showed up on foliage and fingers but not on a room's walls, floor or
        // ceiling: indoors the horizon search never reached them. Steps go up with the cap so the
        // wider span is not undersampled.
        //
        // The clamp also makes AO WEAKEN as you walk toward a surface, because the shortfall grows
        // as the wanted pixel radius grows — a wall visibly brightens as you approach it, which is
        // the opposite of what a world-space radius is for. Measured at radius 2.0 m:
        //
        //   dist  1 m -> wants 1143 px | cap 256 -> 0.45 m | cap 512 -> 0.90 m
        //   dist  2 m -> wants  571 px | cap 256 -> 0.90 m | cap 512 -> 1.79 m
        //   dist  3 m -> wants  381 px | cap 256 -> 1.34 m | cap 512 -> 2.00 m
        //   dist  5 m -> wants  229 px | cap 256 -> 2.00 m | cap 512 -> 2.00 m
        //
        // 512 pushes the onset from ~5 m in to ~3 m and doubles close-range reach. It costs
        // almost nothing: the tap COUNT is `steps`, not the cap — the cap only sets how far apart
        // the taps are spread, so raising it trades cache coherence, not bandwidth.
        //
        // It MITIGATES rather than removes: any fixed screen clamp shortens the world radius
        // somewhere. The real fix is a hierarchical-depth (Hi-Z mip) march, which makes a large
        // screen radius O(log n) instead of O(n) — plan Stage 3, and the one genuinely useful
        // idea to take from ZenRCAO. The Hi-Z pyramid already exists here for occlusion culling.
        Self {
            enabled: true,
            radius_m: 2.0,
            strength: 1.0,
            slices: 3,
            steps: 12,
            max_radius_px: 512.0,
            thickness: 1.0,
            blur_radius: 6.0,
            blur_depth_scale: 24.0,
            blur_normal_power: 8.0,
            debug_mode: 0,
            bent_normal: true,
            max_mip: 0,
        }
    }
}

pub(crate) struct NormalResolve {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    view: Option<wgpu::TextureView>,
    bind: Option<wgpu::BindGroup>,
}

impl NormalResolve {
    pub(crate) fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_normal_resolve"),
            source: wgpu::ShaderSource::Wgsl(include_str!("normal_resolve.wgsl").into()),
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_normal_resolve_layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Float { filterable: false },
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: true,
                },
                count: None,
            }],
        });
        let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_normal_resolve_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_normal_resolve_pipeline"),
            layout: Some(&pl),
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format: NORMAL_FORMAT,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        Self {
            pipeline,
            layout,
            view: None,
            bind: None,
        }
    }

    // (Re)allocate the resolved normal target and bind `src` (the MSAA prepass normal view).
    // Returns a clone of the resolved view for normal_sample_view.
    pub(crate) fn resize(
        &mut self,
        device: &wgpu::Device,
        w: u32,
        h: u32,
        src: &wgpu::TextureView,
    ) -> wgpu::TextureView {
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_3d_normal_resolved"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: NORMAL_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_normal_resolve_bind"),
            layout: &self.layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(src),
            }],
        }));
        self.view = Some(view.clone());
        view
    }

    // Record the resolve (MSAA normal -> single-sample). Must run after the prepass has
    // written the normal target and before GTAO reads it.
    pub(crate) fn resolve(&self, encoder: &mut wgpu::CommandEncoder) {
        let (Some(view), Some(bind)) = (self.view.as_ref(), self.bind.as_ref()) else {
            return;
        };
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_normal_resolve"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view,
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
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, bind, &[]);
        pass.draw(0..3, 0..1);
    }
}

pub struct Gfx3d {
    cameras: CameraGroup,
    conform: ConformGroup,
    // Per-draw world matrix, one entry per draw slot, read-only storage indexed by
    // instance_index (the draw's base_instance). Uploaded in a single write_buffer.
    world: StorageArray,
    // Skinned draws: one PALETTE_SIZE-matrix block per slot, dynamic-offset UBO.
    palette: DynUbo,
    // Per-draw material lighting, one entry per draw slot, read-only storage
    // indexed by instance_index. Bound at group(1)/binding(1) for both the plain
    // and skinned group-1 bind groups (combined with world / palette below).
    material: StorageArray,
    // Combined group-1 bind groups: {world|palette @0, material @1}. Rebuilt when
    // any of their backing buffers regrows (tracked via DynUbo::ensure's return).
    group1_plain_layout: wgpu::BindGroupLayout,
    group1_skinned_layout: wgpu::BindGroupLayout,
    group1_plain_bind: Option<wgpu::BindGroup>,
    group1_skinned_bind: Option<wgpu::BindGroup>,

    // DZ-003 -- Sky's equirect reflection env map (linear radiance) + its sampler, lent to
    // BOTH object paths' group(1). Only sections/draws whose material named an
    // EnvironmentMap sample it; everything else never touches the binding. Starts as a 1x1
    // dummy so the layouts are satisfiable from frame one, exactly like water's.
    env_view: wgpu::TextureView,
    env_sampler: wgpu::Sampler,
    // Generation of the bound env view. u64::MAX = the dummy is still bound. The env
    // texture is created once and never resized, so this flips exactly once.
    env_gen: u64,

    // Pipeline build inputs, kept so variants can be created lazily as draws
    // demand new (blend, depth, polygon-offset, cutout-threshold) combinations.
    shader: wgpu::ShaderModule,
    plain_layout: wgpu::PipelineLayout,
    skinned_layout: wgpu::PipelineLayout,
    surface_format: wgpu::TextureFormat,
    // MSAA sample count of the scene targets (1 = no MSAA). Every scene-targeting object
    // pipeline (colour + prepass + GPU-driven) is built with it; the shadow-depth pipelines
    // stay single-sample (the shadow map is never multisampled).
    sample_count: u32,
    // Alpha-to-coverage for cutout foliage (needs MSAA). When set, cutout (blend Opaque,
    // alpha_ref > 0) colour + prepass pipelines enable alpha_to_coverage and emit a sharpened
    // coverage instead of a hard discard, antialiasing leaf/grass edges across the MSAA samples.
    foliage_a2c: bool,
    vbuf_attrs: [wgpu::VertexAttribute; 6],
    skin_attrs: [wgpu::VertexAttribute; 2],
    pipelines: FxHashMap<PipelineKey, wgpu::RenderPipeline>,
    // Depth+normal prepass variants (docs/depth-prepass-plan.md), built lazily
    // alongside the colour pipelines for every opaque draw the frame submits.
    prepass_pipelines: FxHashMap<PrepassKey, wgpu::RenderPipeline>,

    depth: Option<(wgpu::Texture, wgpu::TextureView)>,
    // View-space normal G-buffer, allocated with (and to the same size as) the depth
    // target; the prepass' one colour attachment. Sampled by no consumer yet (Stage 1).
    normal: Option<(wgpu::Texture, wgpu::TextureView)>,
    depth_size: (u32, u32),
    // Depth-aspect view fed to the Hi-Z copy pass (+ future SSAO / depth-based water opacity).
    // 1x path: the DepthOnly aspect of the depth target (Depth24PlusStencil8 needs an explicit
    // aspect to sample). MSAA path: the single-sample resolved depth (`depth_resolve`), since
    // WebGPU cannot resolve depth via a render-pass resolve_target and consumers want a plain
    // single-sample texture. Rebuilt with the depth target.
    depth_sample_view: Option<wgpu::TextureView>,
    // Farthest-sample counterpart of depth_sample_view for water's seabed reconstruction: at 1x the
    // same depth aspect; under MSAA a SEPARATE far-resolve (min under reversed-Z) so A2C foliage /
    // rotor edges don't poison the water column depth into a foam ring. Rebuilt with the depth target.
    water_depth_view: Option<wgpu::TextureView>,
    // Bumped whenever the sampleable depth views are (re)created (resize), so external consumers that
    // build their own bind group over them (e.g. Water) rebuild only when they actually changed.
    depth_gen: u64,
    // MSAA depth resolves (Some only when sample_count > 1). Tiny fullscreen passes reducing the
    // multisampled depth to single-sample Depth32Float: `depth_resolve` = nearest (Hi-Z, feeds
    // depth_sample_view); `depth_resolve_far` = farthest (water, feeds water_depth_view).
    depth_resolve: Option<DepthResolve>,
    depth_resolve_far: Option<DepthResolve>,
    // MSAA-only single-sample normal for GTAO (screen-space-ao-plan §2). Present but not
    // recorded per frame until the GTAO pass consumes it.
    normal_resolve: Option<NormalResolve>,
    normal_sample_view: Option<wgpu::TextureView>,
    // GTAO. Allocated at both 1x and MSAA — its inputs are single-sample either way.
    // The depth chain is GTAO's own: the Hi-Z pyramid next door reduces the FARTHEST surface
    // (right for culling, backwards for AO). See gtao_depth_mips.rs.
    gtao_depth_mips: gtao_depth_mips::GtaoDepthMips,
    gtao: Gtao,
    gtao_blur: GtaoBlur,
    // Live GTAO tuning (ImGui / WgrRenderParams). `enabled` gates the whole pass: when off the
    // AO target keeps whatever it last held, which is why the consumers read it through the same
    // `strength` gate rather than sampling unconditionally.
    gtao_settings: GtaoSettings,
    // Screen pixels per marched AO sample (1 or 2). Deliberately NOT a field of GtaoSettings:
    // C++ pushes that struct every frame, so anything living in it is frame-0-only from the
    // renderer's side and an env override would be silently reverted. See `set_gtao_scale`.
    gtao_scale: u32,
    // Material Debug (dev panel) for the retained path, pushed straight into the frame
    // UBO each frame. Presentation state, never baked into the material records.
    material_debug_view: u32,
    material_debug_flags: u32,
    // DZ-005 water surface animation, pushed into frame.wgsl's `wateranim` lane every frame:
    // (time_s, ripple_strength, flow_strength, flow_map_bindless_slot). See `set_water_anim`.
    water_anim: [f32; 4],
    /// frame.wgsl `fogfar`: x = the fraction of the draw distance at which distance fog reaches
    /// full, yzw reserved. See `set_fog_far_close`.
    fog_far: [f32; 4],
    // REN-TEMP-001D: frame.wgsl `renscale` — (mip bias log2(render/output), gradient scale
    // 2^bias, then AST-012A's mip-feedback rotation pair: this frame's group index and the
    // group count, 0 = feedback off). Set once per frame; all-zero = native, no feedback.
    render_scale_lane: [f32; 4],
    // AST-012A: the feedback buffer, its readback ring and the harvested per-slot answers.
    mip_feedback: MipFeedback,
    // Set when `ensure_mip_feedback` allocated (or reallocated) that buffer, so the group-1
    // binds that borrow it are rebuilt on the next prepare_cull. A separate flag rather than
    // an extra return value because the allocation happens outside prepare_cull — the
    // bindless capacity it is sized from lives on SharedTextures, which prepare_cull is not
    // given.
    mip_feedback_grew: bool,
    // Live-tunable absolute mip-bias override (None = auto half log2(scale)). Seeded
    // from WGR_MIP_BIAS at startup.
    mip_bias_override: Option<f32>,
    // (proj_xx, proj_yy, near) per camera, cached from `prepare` so the GTAO dispatch uses the
    // SAME projection the prepass rasterised with. Recomputing it at dispatch time from a
    // separately-chosen camera is how AO ends up subtly offset from the depth it is reading.
    // `near` is the whole linearisation: stored reversed-Z depth d gives view z = near / d.
    cam_gtao_proj: Vec<[f32; 3]>,
    // WGR_GTAO=0: hard-disable screen-space AO for an ablation arm. Deliberately NOT a field of
    // GtaoSettings, for exactly the reason `gtao_scale` above is not — C++ pushes that struct
    // every frame (Engine::AoSettings), so anything living in it is frame-0-only from the
    // renderer's side and an env override would be silently reverted before the first capture.
    //
    // It exists because GTAO is the one term that can drive a lit pixel to EXACTLY (0,0,0):
    // shading.wgsl folds it into `amb_ao`, ambient is `sky_irradiance * sun_ambient.w * amb_ao`,
    // and gtao_ao() returns a clamped [0,1] texture read (frame.wgsl:410). Indoors, with no
    // direct sun on a wall, ambient is the whole radiance — so AO = 0 renders the same exact
    // black as an untouched clear. Any argument of the form "the pixel is 0, therefore nothing
    // drew it" has to eliminate this first, and it is fed BY the depth prepass, so it also
    // vanishes under WGR_PREPASS=0.
    gtao_force_off: bool,
    // Single-sample depth-stencil for the post-tonemap UI phase (Some only when sample_count > 1).
    // That phase composites display-referred 2D to the 1x swapchain, so it can't share the MSAA
    // scene depth (mismatched sample counts). Cleared per use; world occlusion isn't carried into
    // the HUD (the UI segment already clears depth even on the 1x path).
    ui_depth: Option<(wgpu::Texture, wgpu::TextureView)>,
    ui_depth_size: (u32, u32),
    // REN-TEMP-001H: rigid dynamic-object velocity (lazy; only exists once the temporal
    // path has actually asked for it).
    velocity_obj: Option<VelocityObjPass>,
    // Hi-Z depth pyramid + GPU-driven occlusion cull toggle (docs §5). The pyramid is built
    // from the post-prepass depth; the color-pass cull samples it. occlusion_enabled gates the
    // whole path (env WGR_GPU_OCCLUSION + ImGui Culling tab); when off, the color pass reuses
    // the main frustum-cull args (identical to the pre-occlusion behaviour).
    hiz: hiz::HiZ,
    occlusion_enabled: bool,

    shadow_pass_ubo: DynUbo, // one ShadowPassUbo per cascade
    // Per-caster data as one whole-buffer storage array (indexed by base_instance),
    // uploaded in a single write_buffer — replaces the old per-caster dynamic UBO writes.
    // Laid out per (cascade, bucket) so each bucket's instances are contiguous; a caster
    // in N cascades appears N times (its GPU data is cascade-independent, but bucketing
    // isn't). Built alongside `shadow_plan` in prepare_shadows.
    shadow_caster_ssbo: StorageArray,
    // Per-cascade instanced draw plan over `shadow_caster_ssbo` (built in prepare_shadows,
    // replayed in render_shadow_passes). Indexed [cascade][bucket].
    shadow_plan: Vec<Vec<ShadowBucket>>,
    shadow_caster_layout: wgpu::BindGroupLayout,
    shadow_caster_bind: Option<wgpu::BindGroup>,
    shadow_shader: wgpu::ShaderModule,
    shadow_layout: wgpu::PipelineLayout,
    shadow_skinned_layout: wgpu::PipelineLayout,
    shadow_pipelines: Option<ShadowPipelines>,
    shadow_target: Option<ShadowTarget>,
    // Bumped on shadow-target recreation so the camera bind group refreshes.
    shadow_gen: u64,
    // 1x1 stand-in bound while no shadow map exists (the layout always binds).
    dummy_shadow_view: wgpu::TextureView,
    // 1x1 stand-in for the GTAO target before the first ensure_depth (see its creation).
    dummy_ao_view: wgpu::TextureView,

    // Interior sky visibility (docs/interior-sky-visibility-plan.md §4). The map is a plain
    // Depth32Float target rendered by the SHADOW depth pipeline over the sky cull view's args —
    // no new pipeline, no new pass UBO layout: the ortho VP goes into the shadow pass UBO's
    // reserved slot (SKY_UBO_SLOT), so this is genuinely the reflection/cascade pattern again.
    interior_sky: SkyVisSettings,
    // Per-model sky-visibility bake (Stage 2). Off by default: it is a synchronous load-time
    // bake with none of §3d's caching or scheduling yet, so enabling it on a full model library
    // is the load stall that plan section warns about. WGR_SKY_BAKE_VOLUMES=1 opts in.
    sky_bake_enabled: bool,
    // Bakes submitted at registration whose readback has not completed yet; drained by
    // collect_sky_bakes() each frame. A model without its volume yet simply reads as
    // fully open sky — the pre-bake behaviour — until the result lands a frame or two later.
    sky_bakes_pending: Vec<(u32, sky_bake::PendingBake, Option<std::path::PathBuf>)>,
    sky_bake: Option<sky_bake::SkyBake>,
    // model index -> (volume, padded model-space AABB). CPU-side for now; the GPU atlas + the
    // per-fragment sample are the next step.
    sky_volumes: FxHashMap<u32, (Vec<[f32; 4]>, [f32; 3], [f32; 3])>,
    // Diagnostics awaiting a log sink (sky bake, staging refusals); drained by lib.rs each
    // frame. Gfx3d has no log sink of its own, and `eprintln!` never reaches --log-file.
    queued_log: Vec<String>,
    // How many per-frame staging uploads wgpu has refused (out of memory) since start, per
    // buffer. Counted rather than printed per occurrence: the failure mode is one OOM followed
    // by every subsequent frame failing the same way, so an unthrottled line is thousands of
    // identical rows in the log and a throttled one with no count hides the scale.
    staging_refusals: FxHashMap<&'static str, u64>,
    // The volumes packed for the GPU: `sky_volume_meta` is indexed by model id and holds
    // (bbox_min, offset) / (bbox_max, dims-code); `sky_volume_data` is every volume concatenated.
    // Rebuilt when a new model bakes, which is a load-time event, not a per-frame one.
    sky_volume_meta: StorageArray,
    sky_volume_data: StorageArray,
    sky_volumes_dirty: bool,
    // Depth ARRAY: one layer per sampled sky direction. (texture, per-layer render views,
    // the D2Array view the shader samples).
    interior_sky_target: Option<(wgpu::Texture, Vec<wgpu::TextureView>, wgpu::TextureView)>,
    // Bumped when the target is (re)allocated or dropped, so the camera bind group follows it.
    interior_sky_gen: u64,
    // This frame's snapped ortho view, Some only while the feature is live. Also the per-frame
    // gate the shader reads: no view -> reach reads 1 everywhere -> no darkening.
    interior_sky_view: Option<SkyVisView>,
    // Per-direction views, index-aligned with sky_vis::directions(). Only meaningful while
    // interior_sky_view is Some.
    interior_sky_views: [SkyVisView; sky_vis::DIRECTION_COUNT],
    // Group-1 draw binds over each sky cull view's records (same layout as the cascade ones).
    gpu_sky_group1: Vec<Option<wgpu::BindGroup>>,
    // 1x1 stand-in bound at @binding(12) whenever the map does not exist.
    dummy_interior_sky_view: wgpu::TextureView,

    // Compute skin bake (docs/compute-skin-bake-plan.md). WGR_SKIN_BAKE=0 disables it and
    // falls back to per-pass VS skinning (the skinned pipelines above); default on.
    skin_bake_enabled: bool,
    skin_bake_pipeline: wgpu::ComputePipeline,
    // group(0) = {in_v ro, in_s ro, palette ro, out rw}; rebuilt per dispatch (mesh
    // buffers differ). group(1) = BakeParams (dynamic-offset UBO, one slot per group).
    skin_bake_layout: wgpu::BindGroupLayout,
    skin_bake_params: DynUbo,
    // The whole palette as a flat STORAGE buffer (block b = matrices [b*128..b*128+128)),
    // uploaded once/frame when the bake is on (replaces the fallback dynamic-offset UBO).
    palette_buf: StorageArray,
    // Every baked instance's output verts, base_vertex-addressed; STORAGE (compute writes)
    // + VERTEX (every pass reads). Grow-only.
    skinned_vbuf: Option<wgpu::Buffer>,
    skinned_cap: u64,
    // This frame's bake plan (one dispatch per distinct skinned mesh+pose) and the
    // draw/caster-side lookup palette_slot -> baked base_vertex. Rebuilt in prepare_skin_bake.
    bake_groups: Vec<BakeGroup>,
    // Keyed by (palette_slot, mesh id): one skeleton (slot) is drawn as SEVERAL
    // meshes (body, head, weapon...), and every one of them needs its own baked
    // slice. The original slot-only key with first-seen-wins handed every sibling
    // mesh the FIRST mesh's baked verts — the full-screen triangle-shard corruption
    // that shipped (and was reverted) on 2026-08-30.
    skin_base_vertex: FxHashMap<(u32, u64), u32>,
    // group(0) skin-bake bind ({vbuf, skin, palette_buf, skinned_vbuf}) cached by mesh.
    // palette_buf/skinned_vbuf are whole-buffer and vbuf/skin are per-mesh-constant, so a
    // mesh's bind is stable frame-to-frame — rebuilt only when palette_buf or skinned_vbuf
    // is (re)allocated (rare growth), evicted on mesh destroy/reskin. This turns the
    // per-frame "one vkUpdateDescriptorSets per skinned mesh" into zero on steady frames.
    bake_bind_cache: FxHashMap<MeshKey, wgpu::BindGroup>,
    // REN-TEMP-001I: LAST frame's bake output (ping-ponged with skinned_vbuf every
    // enabled prepare_skin_bake), read by the skinned velocity pass as "previous pose".
    // The bind cache swaps alongside so each cache still pins the buffer it was built
    // against. `skinned_prev_valid` = the prev buffer really holds last frame's bake.
    skinned_vbuf_prev: Option<wgpu::Buffer>,
    skinned_cap_prev: u64,
    bake_bind_cache_prev: FxHashMap<MeshKey, wgpu::BindGroup>,
    skinned_prev_valid: bool,
    skinned_baked_this_frame: bool,

    // Merged geometry pool: one shared vertex buffer + one shared Uint32 index buffer
    // that every mesh suballocates into (docs/gpu-culling-and-depth-plan.md §2.1). Each
    // Mesh holds only pool offsets; draws address the pool via slice + ibase.
    pool: GeometryPool,

    // GPU-driven indirect draw (docs/gpu-culling-and-depth-plan.md Stage 2). When on, the
    // instancing plan's opaque-rigid buckets submit from a CPU-built indirect args buffer
    // instead of direct draw_indexed; off (flag or missing INDIRECT_FIRST_INSTANCE) keeps
    // the whole opaque set on the direct draw_one path.
    indirect_enabled: bool,
    // This frame's DrawIndexedIndirectArgs, one per indirect bucket, packed by
    // build_indirect in plan-op order (grow-only; INDIRECT for the draw + STORAGE for the
    // Stage-3 compute writer).
    indirect_args: Option<wgpu::Buffer>,
    indirect_args_cap: u64,

    // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3): the cull compute +
    // retained scene, the GPU-driven opaque draw pipeline, and its group-1 bind group
    // (instances/records/materials, rebuilt when the cull buffers grow). Gated by
    // WGR_GPU_DRIVEN; inert until C++ registers models/instances (Stage 3b-3).
    gpu_driven_enabled: bool,
    // Per-section registration source (mesh handle + mesh-local range + variant), parallel to
    // the cull's sections table and in the same append order. The pool can relocate a mesh's
    // vertices (VB release + recreate on LOD optimisation / shape reload changes its vbase), so
    // base_vertex / first_index are RE-RESOLVED from the current mesh alloc every frame in
    // prepare_cull — exactly as the CPU indirect + shadow paths do — instead of being captured
    // once at registration (which would leave a stale vbase pointing at freed/reused pool bytes).
    gpu_section_src: Vec<GpuSectionSrc>,
    // Diagnostic: last-reported count of sections whose mesh handle resolved to nothing
    // (stale/destroyed). u32::MAX = never reported. Logged under WGR_GPU_DEBUG when it changes.
    gpu_dbg_stale: u32,
    // MULTI_DRAW_INDIRECT_COUNT is available (desktop Vulkan/DX12; Metal lacks it). When set,
    // draw_gpu_driven trims the no-op tail via the GPU count buffer instead of dispatching the
    // full conservative capacity (3b-4).
    multi_draw_count_enabled: bool,
    // Engine-derived cull + LOD inputs (objectsZ / Camera::Left() / Scene::_lodInvWidth /
    // pixel_limit), pushed each frame from C++ (wgr_set_cull_params). Default is inert-safe
    // (draw everything at finest LOD within objects_z) until the first push.
    cull_inputs: cull::CullInputs,
    cull: cull::CullState,
    gpu_pipeline: wgpu::RenderPipeline,
    /// REN-OBJ-004: the main-view colour pipeline with the forced early depth test and depth
    /// writes off; Some only under WGR_OBJECT_EARLY_Z=1 on a device that supports it, and then
    /// it replaces gpu_pipeline for the prepassed main-view draw only.
    gpu_pipeline_early: Option<wgpu::RenderPipeline>,
    // Mirrored-view variant of gpu_pipeline. Reflection reverses triangle winding, so it
    // deliberately uses the opposite front face while retaining back-face culling.
    gpu_reflection_pipeline: wgpu::RenderPipeline,
    // Depth+normal prepass variant of gpu_pipeline (vs_gpu / fs_gpu_prepass): writes depth +
    // the view-space normal G-buffer so the GPU-driven set participates in the prepass.
    gpu_prepass_pipeline: wgpu::RenderPipeline,
    // REN-TEMP-001T retained-path vegetation velocity twin + its group(2) resources.
    gpu_velocity_pipeline: wgpu::RenderPipeline,
    gpu_vel_layout: wgpu::BindGroupLayout,
    gpu_vel_uniform: wgpu::Buffer,
    gpu_vel_bind: Option<wgpu::BindGroup>,
    gpu_vel_bound_gen: u64,
    // GPU-driven cascade shadow depth pipeline (§6 multi-view, vs_gpu_shadow / fs_gpu_shadow):
    // the retained set cast into each cascade's depth map. Group 0 = the shadow pass UBO.
    gpu_shadow_pipeline: wgpu::RenderPipeline,
    gpu_group1_layout: wgpu::BindGroupLayout,
    gpu_group1_bind: Option<wgpu::BindGroup>,
    gpu_reflection_group1_bind: Option<wgpu::BindGroup>,
    // Color-pass draw bind (instances + the OCCLUSION view's records + materials). Same layout
    // as gpu_group1_bind, only the records differ (the occlusion-culled color set vs the main
    // prepass set). None when occlusion is off; then the color draw reuses gpu_group1_bind.
    gpu_color_group1_bind: Option<wgpu::BindGroup>,
    // Per-cascade group-1 draw binds (instances + THAT cascade's records + materials). Parallel
    // to the cull's shadow views; rebuilt with gpu_group1_bind when a shared buffer grows.
    gpu_shadow_group1: Vec<Option<wgpu::BindGroup>>,

    // Cull-sphere DEBUG pass (ImGui Culling tab): instanced line-list wireframe of every
    // retained instance's frustum-cull sphere. Bind = instances + models; rebuilt with
    // gpu_group1_bind when a buffer grows.
    cull_debug_pipeline: wgpu::RenderPipeline,
    cull_debug_layout: wgpu::BindGroupLayout,
    cull_debug_bind: Option<wgpu::BindGroup>,

    meshes: SlotMap<MeshKey, Mesh>,
}

impl Gfx3d {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        device: &wgpu::Device,
        textures: &SharedTextures,
        surface_format: wgpu::TextureFormat,
        sample_count: u32,
        composer: &mut naga_oil::compose::Composer,
        skin_bake_enabled: bool,
        indirect_enabled: bool,
        gpu_driven_enabled: bool,
        multi_draw_count_enabled: bool,
    ) -> Self {
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_3d_shader",
            include_str!("shader3d.wgsl"),
            "gfx3d/shader3d.wgsl",
        );

        // Group 0 = camera UBO + shadow map + comparison sampler. World + material
        // are per-draw storage arrays indexed by instance_index (one upload each);
        // palette is one PALETTE_SIZE-matrix block per skinned draw (dynamic offset).
        let cameras = CameraGroup::new(device);
        let conform = ConformGroup::new(device);
        let world = StorageArray::new("wgr_3d_world_ssbo");
        let palette = DynUbo::new(
            device,
            "wgr_3d_palette_layout",
            (PALETTE_SIZE * std::mem::size_of::<WgrMat4>()) as u64,
            wgpu::ShaderStages::VERTEX,
        );
        let material = StorageArray::new("wgr_3d_material_ssbo");

        // DZ-003 -- 1x1 stand-in for Sky's reflection env map, so both object paths'
        // group(1) is valid before set_env_view runs (and on any frame Sky is absent).
        // Rgba16Float to match the real env target; the sampler wraps in U (the equirect
        // azimuth seam) and clamps V (the poles), same as water's.
        let dummy_env_view = device
            .create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_3d_dummy_env"),
                size: wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
            .create_view(&wgpu::TextureViewDescriptor::default());
        let env_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_3d_env_sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });

        // Group 1 for the lit pipelines. Binding 1 (material) is a whole-buffer
        // read-only storage array for both pipelines, indexed by instance_index.
        // Binding 0 differs: the plain pipeline binds the world storage array (also
        // instance-indexed, whole-buffer); the skinned pipeline binds the
        // dynamic-offset bone palette UBO. `slot0_dynamic` selects between them. The
        // shadow-depth pipelines keep their own palette layout, so are unaffected.
        let group1_layout = |label: &str, slot0_dynamic: Option<u64>| {
            let slot0 = match slot0_dynamic {
                Some(size) => wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: true,
                    min_binding_size: wgpu::BufferSize::new(size),
                },
                None => wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Storage { read_only: true },
                    has_dynamic_offset: false,
                    min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<ObjectGpu>() as u64),
                },
            };
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some(label),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::VERTEX,
                        ty: slot0,
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        // The direct vertex path reads the foliage class from the free
                        // sun_ambient.w lane to bend legacy canopy-card normals; fs_main
                        // continues to read the same material record for shading.
                        visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Storage { read_only: true },
                            has_dynamic_offset: false,
                            min_binding_size: wgpu::BufferSize::new(MATERIAL_SIZE),
                        },
                        count: None,
                    },
                    // DZ-003: Sky's equirect reflection env map + its sampler. Read only
                    // by draws whose material named an EnvironmentMap; every other draw
                    // never samples it, so this costs one binding and nothing else.
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: true },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 3,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                    // AST-012A mip feedback, the same buffer the GPU-driven path writes.
                    // BOTH object paths have to report or the numbers are a fiction: on
                    // perf_combat the per-draw path issues 2,093 of the 2,162 object draws,
                    // so instrumenting only the GPU-driven set would have called 878 resident
                    // textures "never sampled" when almost all of them were being sampled
                    // every frame by a shader that simply had nowhere to say so.
                    wgpu::BindGroupLayoutEntry {
                        binding: 4,
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
        };
        let group1_plain_layout = group1_layout("wgr_3d_group1_plain_layout", None);
        let group1_skinned_layout = group1_layout(
            "wgr_3d_group1_skinned_layout",
            Some((PALETTE_SIZE * std::mem::size_of::<WgrMat4>()) as u64),
        );

        // Groups 2/3 are the BINDLESS object-texture array + the 8-variant sampler array
        // (docs/bindless-textures-plan.md), bound once for the whole lit-mesh + prepass;
        // the per-instance texture/sampler indices ride the material array. The shadow
        // pipelines keep their own single-texture layouts.
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_3d_pipeline_layout"),
            bind_group_layouts: &[
                Some(&cameras.layout),
                Some(&group1_plain_layout),
                Some(&textures.bindless_layout),
                Some(&textures.sampler_array_layout),
                Some(&conform.layout),
            ],
            immediate_size: 0,
        });
        // Skinned layout swaps the per-draw world matrix (group 1 binding 0) for the
        // bone palette; groups 0/2/3 (camera/textures/samplers) are identical.
        let skinned_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_3d_skinned_pipeline_layout"),
            bind_group_layouts: &[
                Some(&cameras.layout),
                Some(&group1_skinned_layout),
                Some(&textures.bindless_layout),
                Some(&textures.sampler_array_layout),
                Some(&conform.layout),
            ],
            immediate_size: 0,
        });

        // Vertex attributes stored on the struct so pipeline variants can be
        // (re)built lazily; VertexBufferLayout only borrows them at build time.
        // Location 5 = per-vertex terrain-conform selector (0/1/2). Locations 6/7 carry
        // an optional authored tangent/binormal frame from ODOL; zero values retain the
        // derivative fallback. They stay clear of the skinned path's bone/weight inputs
        // at 3/4. Offsets are byte 32 (conform), 36 (tangent), and 48 (binormal).
        let vbuf_attrs = wgpu::vertex_attr_array![0 => Float32x3, 1 => Float32x3, 2 => Float32x2, 5 => Uint32,
                                     6 => Float32x3, 7 => Float32x3];
        // Skin buffer: 8 bytes/vertex — Uint8x4 bone indices + Unorm8x4 weights.
        let skin_attrs = wgpu::vertex_attr_array![3 => Uint8x4, 4 => Unorm8x4];

        let shadow_shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_shadow_depth_shader",
            include_str!("shadow_depth.wgsl"),
            "gfx3d/shadow_depth.wgsl",
        );
        let shadow_pass_ubo = DynUbo::new(
            device,
            "wgr_shadow_pass_layout",
            std::mem::size_of::<ShadowPassUbo>() as u64,
            wgpu::ShaderStages::VERTEX,
        );
        // Per-caster data is a whole-buffer read-only storage array indexed by
        // base_instance (VERTEX only — the fragment bakes its cutout threshold), bound
        // once per pass instead of one dynamic-offset UBO slot per caster.
        let shadow_caster_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_shadow_caster_layout"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(
                            std::mem::size_of::<ShadowCasterGpu>() as u64,
                        ),
                    },
                    count: None,
                }],
            });
        let shadow_caster_ssbo = StorageArray::new("wgr_shadow_caster_ssbo");
        // Group 4 = the terrain heightmap conform group (shared with the lit pipelines),
        // so the depth pass conforms ClipLand vegetation to the same ground per vertex.
        let shadow_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_shadow_pipeline_layout"),
            bind_group_layouts: &[
                Some(&shadow_pass_ubo.layout),
                Some(&shadow_caster_layout),
                Some(&textures.texture_layout),
                Some(&textures.sampler_layout),
                Some(&conform.layout),
            ],
            immediate_size: 0,
        });
        // Skinned depth pipelines swap the caster UBO (group 1) for the bone palette.
        let shadow_skinned_layout =
            device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_shadow_skinned_pipeline_layout"),
                bind_group_layouts: &[
                    Some(&shadow_pass_ubo.layout),
                    Some(&palette.layout),
                    Some(&textures.texture_layout),
                    Some(&textures.sampler_layout),
                    Some(&conform.layout),
                ],
                immediate_size: 0,
            });

        let dummy_shadow = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_shadow_dummy"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: SHADOW_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_shadow_view = dummy_shadow.create_view(&wgpu::TextureViewDescriptor {
            dimension: Some(wgpu::TextureViewDimension::D2Array),
            ..Default::default()
        });

        // Stand-in for frame @binding(11) before the first ensure_depth. Content is irrelevant —
        // the consumers gate on frame.gtao.x, which is 0 until the pass is enabled AND has run —
        // but the binding must exist from the first frame or every 3D pipeline fails validation.
        let dummy_ao = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_gtao_dummy"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: AO_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_ao_view = dummy_ao.create_view(&wgpu::TextureViewDescriptor::default());

        // Stand-in for frame @binding(12) whenever no sky-visibility map exists (feature off, or
        // before the first frame that renders one). Its CONTENT matters more than the AO dummy's:
        // it is sampled with a comparison sampler, and a cleared depth texture reads 0 = "an
        // occluder at the very top of the box", i.e. everything indoors. That is why the shader
        // gates on frame.skyvis.x instead of trusting the texture — see interior_sky_reach.
        let dummy_interior_sky = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_interior_sky_dummy"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: SHADOW_FORMAT,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_interior_sky_view =
            dummy_interior_sky.create_view(&wgpu::TextureViewDescriptor {
                dimension: Some(wgpu::TextureViewDimension::D2Array),
                ..Default::default()
            });

        // Compute skin bake (docs/compute-skin-bake-plan.md). group(0) = the four
        // storage buffers (source verts / skin data / palette / baked output), all
        // whole-buffer so min_binding_size is left open; group(1) = BakeParams.
        let skin_bake_shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_skin_bake_shader",
            include_str!("skin_bake.wgsl"),
            "gfx3d/skin_bake.wgsl",
        );
        // Runtime-sized arrays: min_binding_size is one element (4 B for the u32 vertex/
        // skin/output arrays, 64 B for the mat4 palette). All bound whole-buffer.
        let storage_arr = |read_only: bool, min: u64| wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage { read_only },
            has_dynamic_offset: false,
            min_binding_size: wgpu::BufferSize::new(min),
        };
        let skin_bake_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_skin_bake_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: storage_arr(true, 4),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: storage_arr(true, 4),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: storage_arr(true, 64),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: storage_arr(false, 4),
                    count: None,
                },
            ],
        });
        let skin_bake_params = DynUbo::new(
            device,
            "wgr_skin_bake_params",
            std::mem::size_of::<BakeParamsGpu>() as u64,
            wgpu::ShaderStages::COMPUTE,
        );
        let skin_bake_pipeline_layout =
            device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_skin_bake_pipeline_layout"),
                bind_group_layouts: &[Some(&skin_bake_layout), Some(&skin_bake_params.layout)],
                immediate_size: 0,
            });
        let skin_bake_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("wgr_skin_bake_pipeline"),
            layout: Some(&skin_bake_pipeline_layout),
            module: &skin_bake_shader,
            entry_point: Some("main"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            cache: None,
        });

        // Alpha-to-coverage for cutout foliage: needs MSAA; default-on there, WGR_FOLIAGE_A2C=0
        // opts out. Drives both the shader coverage path and alpha_to_coverage_enabled on the
        // cutout colour + prepass pipelines (per-draw and GPU-driven).
        let foliage_a2c = sample_count > 1
            && std::env::var("WGR_FOLIAGE_A2C")
                .map(|v| v != "0")
                .unwrap_or(true);

        // GPU-driven rendering (Stage 3): retained scene + cull compute + the opaque draw
        // pipeline. Groups 0/2/3 (camera, bindless textures, samplers) are shared with the
        // per-draw path; group 1 is instances/records/materials.
        let cull = cull::CullState::new(device);
        let gpu_group1_layout = cull::gpu_group1_layout(device);
        let gpu_vel_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_gpu_velocity_group2"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });
        let gpu_vel_uniform = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_gpu_velocity_uniform"),
            size: std::mem::size_of::<crate::temporal::GpuVegVelParams>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let gpu_velocity_pipeline = cull::build_gpu_velocity_pipeline(
            device,
            composer,
            &cameras.layout,
            &gpu_group1_layout,
            &textures.bindless_layout,
            &gpu_vel_layout,
            &conform.layout,
        );
        let (gpu_pipeline, gpu_prepass_pipeline) = cull::build_gpu_pipeline(
            device,
            composer,
            &cameras.layout,
            &gpu_group1_layout,
            &textures.bindless_layout,
            &textures.sampler_array_layout,
            &conform.layout,
            surface_format,
            sample_count,
            foliage_a2c,
            wgpu::FrontFace::Cw,
            false,
        );
        let gpu_pipeline_early = if object_early_z() {
            Some(
                cull::build_gpu_pipeline(
                    device,
                    composer,
                    &cameras.layout,
                    &gpu_group1_layout,
                    &textures.bindless_layout,
                    &textures.sampler_array_layout,
                    &conform.layout,
                    surface_format,
                    sample_count,
                    foliage_a2c,
                    wgpu::FrontFace::Cw,
                    true,
                )
                .0,
            )
        } else {
            None
        };
        let (gpu_reflection_pipeline, _) = cull::build_gpu_pipeline(
            device,
            composer,
            &cameras.layout,
            &gpu_group1_layout,
            &textures.bindless_layout,
            &textures.sampler_array_layout,
            &conform.layout,
            surface_format,
            // The planar target is resolution-scaled and mip-filtered before water samples
            // it. Main-scene 4x MSAA here multiplied the complete retained reflection pass for
            // no useful output quality; cutouts use their normal hard threshold at 1x.
            1,
            false,
            wgpu::FrontFace::Ccw,
            false,
        );
        // GPU-driven cascade shadow depth pipeline (§6 multi-view): the retained set cast into
        // each cascade's depth map. Group 0 is the shadow pass UBO (light-VP), so it shares the
        // gpu_group1/bindless/conform layouts with the colour path.
        let gpu_shadow_pipeline = cull::build_gpu_shadow_pipeline(
            device,
            composer,
            &shadow_pass_ubo.layout,
            &gpu_group1_layout,
            &textures.bindless_layout,
            &textures.sampler_array_layout,
            &conform.layout,
        );
        let cull_debug_layout = cull::cull_debug_layout(device);
        let cull_debug_pipeline = cull::build_cull_debug_pipeline(
            device,
            composer,
            &cameras.layout,
            &cull_debug_layout,
            surface_format,
            sample_count,
        );
        // MSAA depth resolves: built only when the scene is multisampled. Reduce the MSAA depth to
        // single-sample textures — nearest for the Hi-Z build, farthest for water's seabed depth.
        let depth_resolve =
            (sample_count > 1).then(|| DepthResolve::new(device, sample_count, false));
        let depth_resolve_far =
            (sample_count > 1).then(|| DepthResolve::new(device, sample_count, true));
        // Same MSAA-only condition: at 1x the prepass normal is already single-sample.
        let normal_resolve = (sample_count > 1).then(|| NormalResolve::new(device));
        let gtao = Gtao::new(device);
        let gtao_blur = GtaoBlur::new(device);

        Gfx3d {
            cameras,
            conform,
            world,
            palette,
            material,
            group1_plain_layout,
            group1_skinned_layout,
            group1_plain_bind: None,
            group1_skinned_bind: None,
            env_view: dummy_env_view,
            env_sampler,
            env_gen: u64::MAX,
            shader,
            plain_layout: pipeline_layout,
            skinned_layout,
            surface_format,
            sample_count,
            foliage_a2c,
            vbuf_attrs,
            skin_attrs,
            pipelines: FxHashMap::default(),
            prepass_pipelines: FxHashMap::default(),
            depth: None,
            normal: None,
            depth_size: (0, 0),
            gpu_color_group1_bind: None,
            depth_sample_view: None,
            water_depth_view: None,
            depth_gen: 0,
            depth_resolve,
            depth_resolve_far,
            normal_resolve,
            normal_sample_view: None,
            gtao_depth_mips: gtao_depth_mips::GtaoDepthMips::new(device),
            gtao,
            gtao_blur,
            gtao_settings: GtaoSettings::default(),
            gtao_scale: gtao_scale_from_env(),
            gtao_force_off: gtao_force_off_from_env(),
            material_debug_view: 0,
            material_debug_flags: 1 << 2, // compose Multi layers, matching the C++ default
            water_anim: [0.0, water_ripple_strength_from_env(), 0.0, 0.0],
            // MUST equal Engine::SkySettings::fogFarClose. Nothing pushes the sky settings at
            // startup -- the dev panel's Sky tab is the only caller of SetFogFarClose -- so this
            // IS the value a release build and every --no-dev run render with. A mismatch means
            // the shipped game never gets the setting the header says it has.
            fog_far: [
                fog_far_close_from_env().unwrap_or(FOG_FAR_CLOSE_DEFAULT),
                0.0,
                0.0,
                0.0,
            ],
            render_scale_lane: [0.0; 4],
            mip_feedback: MipFeedback::new(),
            mip_feedback_grew: false,
            mip_bias_override: std::env::var("WGR_MIP_BIAS")
                .ok()
                .and_then(|v| v.parse::<f32>().ok()),
            cam_gtao_proj: Vec::new(),
            ui_depth: None,
            ui_depth_size: (0, 0),
            velocity_obj: None,
            hiz: hiz::HiZ::new(device),
            // GPU Hi-Z occlusion: default on when GPU-driven is on (the point of this feature),
            // opt-out via WGR_GPU_OCCLUSION=0; also toggleable live from the ImGui Culling tab.
            occlusion_enabled: gpu_driven_enabled
                && std::env::var("WGR_GPU_OCCLUSION")
                    .map(|v| v != "0")
                    .unwrap_or(true),
            shadow_pass_ubo,
            shadow_caster_ssbo,
            shadow_plan: Vec::new(),
            shadow_caster_layout,
            shadow_caster_bind: None,
            shadow_shader,
            shadow_layout,
            shadow_skinned_layout,
            shadow_pipelines: None,
            shadow_target: None,
            shadow_gen: 0,
            dummy_shadow_view,
            dummy_ao_view,
            interior_sky: SkyVisSettings::default(),
            // ON by default so Stage 2 has volumes to read (owner call, 2026-08-05).
            // WGR_SKY_BAKE_VOLUMES=0 disables it — which is the switch to reach for if the
            // load-time bake (~20 ms per model, no disk cache yet) becomes intolerable before
            // §3d's caching lands.
            sky_bake_enabled: std::env::var("WGR_SKY_BAKE_VOLUMES")
                .map(|v| v != "0")
                .unwrap_or(true),
            sky_bake: None,
            sky_bakes_pending: Vec::new(),
            sky_volumes: FxHashMap::default(),
            // Announced through the GAME log (drained by lib.rs into the LogSink on the first
            // frame) so a captured road screenshot can be matched to which arm produced it.
            queued_log: vec![road_pixel_conform_log_line()],
            staging_refusals: FxHashMap::default(),
            sky_volume_meta: StorageArray::new("wgr_sky_volume_meta"),
            sky_volume_data: StorageArray::new("wgr_sky_volume_data"),
            sky_volumes_dirty: false,
            interior_sky_target: None,
            interior_sky_gen: 0,
            interior_sky_view: None,
            interior_sky_views: sky_vis::build_views(glam::Vec3::ZERO, &SkyVisSettings::default()),
            gpu_sky_group1: Vec::new(),
            dummy_interior_sky_view,
            skin_bake_enabled,
            skin_bake_pipeline,
            skin_bake_layout,
            skin_bake_params,
            palette_buf: StorageArray::new("wgr_skin_palette_ssbo"),
            skinned_vbuf: None,
            skinned_cap: 0,
            bake_groups: Vec::new(),
            skin_base_vertex: FxHashMap::default(),
            bake_bind_cache: FxHashMap::default(),
            skinned_vbuf_prev: None,
            skinned_cap_prev: 0,
            bake_bind_cache_prev: FxHashMap::default(),
            skinned_prev_valid: false,
            skinned_baked_this_frame: false,
            pool: GeometryPool::new(device),
            indirect_enabled,
            indirect_args: None,
            indirect_args_cap: 0,
            gpu_driven_enabled,
            gpu_section_src: Vec::new(),
            gpu_dbg_stale: u32::MAX,
            multi_draw_count_enabled,
            cull_inputs: cull::CullInputs::default(),
            cull,
            gpu_pipeline,
            gpu_pipeline_early,
            gpu_reflection_pipeline,
            gpu_prepass_pipeline,
            gpu_velocity_pipeline,
            gpu_vel_layout,
            gpu_vel_uniform,
            gpu_vel_bind: None,
            gpu_vel_bound_gen: u64::MAX,
            gpu_shadow_pipeline,
            gpu_group1_layout,
            gpu_group1_bind: None,
            gpu_reflection_group1_bind: None,
            gpu_shadow_group1: Vec::new(),
            cull_debug_pipeline,
            cull_debug_layout,
            cull_debug_bind: None,
            meshes: SlotMap::with_key(),
        }
    }

    // Blend state for a WgrBlend id (None = opaque, no colour blend).
    fn blend_state(blend: u8) -> Option<wgpu::BlendState> {
        match blend {
            b if b == WgrBlend::Alpha as u8 => Some(wgpu::BlendState {
                color: wgpu::BlendComponent {
                    src_factor: wgpu::BlendFactor::SrcAlpha,
                    dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                    operation: wgpu::BlendOperation::Add,
                },
                alpha: wgpu::BlendComponent {
                    src_factor: wgpu::BlendFactor::One,
                    dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                    operation: wgpu::BlendOperation::Add,
                },
            }),
            b if b == WgrBlend::Additive as u8 => Some(wgpu::BlendState {
                color: wgpu::BlendComponent {
                    src_factor: wgpu::BlendFactor::SrcAlpha,
                    dst_factor: wgpu::BlendFactor::One,
                    operation: wgpu::BlendOperation::Add,
                },
                alpha: wgpu::BlendComponent::OVER,
            }),
            b if b == WgrBlend::Shadow as u8 => Some(wgpu::BlendState {
                // Darken: color = dst*(1-srcA); keep src alpha for the next term.
                // Matches GL33's glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO).
                color: wgpu::BlendComponent {
                    src_factor: wgpu::BlendFactor::Zero,
                    dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                    operation: wgpu::BlendOperation::Add,
                },
                alpha: wgpu::BlendComponent {
                    src_factor: wgpu::BlendFactor::One,
                    dst_factor: wgpu::BlendFactor::Zero,
                    operation: wgpu::BlendOperation::Add,
                },
            }),
            _ => None,
        }
    }

    // Create the pipeline for `key` if it doesn't exist yet.
    fn ensure_pipeline(&mut self, device: &wgpu::Device, key: PipelineKey) {
        if self.pipelines.contains_key(&key) {
            return;
        }

        let module = &self.shader;
        let (vs_entry, layout) = if key.skinned {
            ("vs_skinned", &self.skinned_layout)
        } else {
            ("vs_main", &self.plain_layout)
        };

        let vbuf_layout = wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<WgrMeshVertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &self.vbuf_attrs,
        };
        let skin_layout = wgpu::VertexBufferLayout {
            array_stride: 8,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &self.skin_attrs,
        };
        let plain_buffers = [vbuf_layout.clone()];
        let skinned_buffers = [vbuf_layout, skin_layout];
        let buffers: &[wgpu::VertexBufferLayout] = if key.skinned {
            &skinned_buffers
        } else {
            &plain_buffers
        };

        // WgrDepthMode: 0 none, 1 test (no write), 2 test + write.
        let (test, mut write) = match key.depth {
            0 => (false, false),
            1 => (true, false),
            _ => (true, true),
        };
        // Prepassed opaque draws in the colour pass keep GreaterEqual but stop writing:
        // the prepass already holds the frontmost depth (decision 2). GreaterEqual (not
        // Equal) is robust to any sub-ULP VS drift; write-off just removes redundant writes.
        if key.depth_write_off {
            write = false;
        }
        // Shadows still use DepthBiasState (works well enough for them: drawn
        // depth-test-no-write on the surface). Decal/ZBias overlays instead bias in
        // the vertex shader (depth_bias below) — DepthBiasState's constant term is
        // unreliable / a no-op on float depth formats, so it did nothing for close
        // UI (notebook) and coplanar sign overlays.
        // Reversed-Z: nearer = larger depth, so a toward-camera bias is positive
        // (the forward-Z code used negatives). DepthBiasState's constant term is a
        // no-op on this backend's float depth anyway, but keep the sign correct for
        // formats/GPUs where it does apply.
        let bias = match key.offset {
            Offset::Shadow => wgpu::DepthBiasState {
                constant: 64,
                slope_scale: 1.0,
                clamp: 0.0,
            },
            _ => wgpu::DepthBiasState::default(),
        };

        // Vertex-shader depth bias in [0,1] NDC depth. WGR_DECAL_SCALE / WGR_ZBIAS_SCALE
        // multiply the base unit; ZBias also scales with its 1..3 level.
        const DEPTH_BIAS_UNIT: f32 = 1.0e-5;

        // DECAL is now a SEPARATE unit, because the ndc-squared fix silently rescaled it by four
        // orders of magnitude and nobody adjusted the constant to match.
        //
        // The history: the bias used to be a constant NDC offset. Under reversed-Z, depth is
        // near/z, so a constant NDC offset is a distance-DEPENDENT world pull — it dragged a road
        // ~95 m forward at 1 km and 716 m at 3 km, which is why roads appeared through mountains.
        // The fix multiplied by ndc squared, which is correct in SHAPE: it makes the pull a
        // constant in WORLD space. But it changed the magnitude at the same time, and the constant
        // was left at the value that had been tuned for the old meaning.
        //
        // With the ndc-squared form the world-space pull is `depth_bias * near`. This file's own
        // calibration comment upstream uses near = 0.1 m, so 1.0e-5 buys 1.0e-6 m — ONE MICRON.
        // That is not a small bias, it is no bias: the previous session traded a 716 m over-pull
        // for zero, which is exactly why applying it did not fix the see-through and why the road
        // agent's census found the mechanism still live.
        //
        // 5.0e-2 * 0.1 m = 0.005 m, which is ZRoadEpsilon (EngineWgpu.hpp:104) — the same
        // world-space lift GL33 and the CPU path give a road at ClipShape.cpp:240, and which wgpu
        // loses because GGpuTerrainConform makes the GPU conform pin the vertex flat
        // (shader3d.wgsl:295 assigns world_pos.y and discards pos.y). So this is not a tuned
        // magic number; it restores the documented parity value the wgpu path dropped.
        //
        // WGR_DECAL_SCALE still scales it, and now that env_f32 accepts 0 (see above) the
        // ablation arm works for the first time: WGR_DECAL_SCALE=0 is genuinely no bias, and
        // WGR_DECAL_SCALE=0.0002 reproduces the old 1e-5 behaviour for an A/B.
        //
        // ZBias is deliberately LEFT ALONE at the old unit even though the ndc-squared change
        // nulled it by the same factor. It plausibly has the identical defect — but it is applied
        // to coplanar geometry all over every world at three different levels, and correcting it
        // 5000x blind, on a night with no way to check for z-fighting across that surface area,
        // risks trading one reported bug for an unreported one on content nobody is looking at.
        // It needs its own measurement. Recorded rather than fixed.
        const DECAL_BIAS_UNIT: f32 = 5.0e-2;
        let depth_bias = match key.offset {
            Offset::Decal => decal_scale() * DECAL_BIAS_UNIT,
            Offset::ZBias(level) => level as f32 * zbias_scale() * DEPTH_BIAS_UNIT,
            _ => 0.0,
        } as f64;

        let alpha_ref = f32::from_bits(key.alpha_ref_bits) as f64;
        let is_shadow = if key.blend == WgrBlend::Shadow as u8 {
            1.0
        } else {
            0.0
        };
        // HDR path: the scene color target is Rgba16Float only when the HDR pipeline
        // is on, so it doubles as the `linear` shading signal (decode + no clamp).
        let linear = if self.surface_format == wgpu::TextureFormat::Rgba16Float {
            1.0
        } else {
            0.0
        };
        // Alpha-to-coverage for this pipeline: cutout foliage (opaque blend, alpha_ref > 0), not
        // the shadow-darken pass, and only under MSAA. Drives the shader's coverage path and the
        // pipeline's alpha_to_coverage_enabled below.
        let a2c = self.foliage_a2c && key.blend == WgrBlend::Opaque as u8 && alpha_ref > 0.0;
        // Alpha-blended draws (glass canopies) are flagged so the shared shading damps their diffuse
        // sky-irradiance ambient — a transparent surface isn't a diffuse reflector, and a full sky
        // wash blows out cockpit glass (and spikes auto-exposure). Only Alpha; Additive effects and
        // the opaque/cutout GPU-driven set stay at the default 0.
        let translucent = if key.blend == WgrBlend::Alpha as u8 {
            1.0
        } else {
            0.0
        };
        let constants = [
            ("alpha_ref", alpha_ref),
            ("is_shadow", is_shadow),
            ("depth_bias", depth_bias),
            ("linear", linear),
            ("a2c", if a2c { 1.0 } else { 0.0 }),
            ("foliage_screen_ao", foliage_screen_ao() as f64),
            ("count_fragments", count_fragments() as f64),
            ("translucent", translucent),

            ("emissive_night", emissive_night() as f64),
            ("cutout_mip_alpha", cutout_mip_alpha() as f64),
            ("pixel_lift", road_pixel_lift() as f64),
            (
                "alpha_kill_door",
                if std::env::var("WGR_ALPHA_KILL_DOOR").map(|v| v != "0").unwrap_or(false) {
                    1.0
                } else {
                    0.0
                },
            ),
            (
                "road_kill_door",
                if std::env::var("WGR_ROAD_KILL_DOOR").map(|v| v != "0").unwrap_or(false) {
                    1.0
                } else {
                    0.0
                },
            ),
            ("pixel_lift_max_frac", road_pixel_lift_max_frac() as f64),
            ("pixel_lift_per_m", road_pixel_lift_per_m() as f64),
        ];
        // OnSurface (Offset::Decal) draws on the plain path take the frag_depth-writing twin
        // so the road hugs the terrain at every PIXEL (WGR_ROAD_PIXEL_CONFORM). Plain only:
        // fs_surface reads `objects`-conformed varyings and the heightmap group, and the
        // skinned pipeline binds the bone palette at that slot; roads are never skinned.
        // Everything else keeps fs_main and its early-Z. Decal draws are never prepassed
        // (PipelineKey::prepassed requires Offset::None), so no prepass twin has to agree.
        let fs_entry = if key.offset == Offset::Decal && !key.skinned && road_pixel_conform() {
            "fs_surface"
        } else if !write && object_early_z() {
            // REN-OBJ-004: prepassed (write-off) colour draws test depth before shading.
            "fs_main_early"
        } else {
            "fs_main"
        };

        // Shadow draws exclude already-shadowed pixels via the stencil: test EQUAL
        // 0 (stencil is cleared to 0 each segment; opaque geometry leaves it 0) and
        // INCR on pass, so the first shadow polygon over a pixel darkens it and any
        // overlapping ones fail the test. Non-shadow pipelines leave the stencil
        // untouched (default = disabled).
        let stencil = if key.blend == WgrBlend::Shadow as u8 {
            let face = wgpu::StencilFaceState {
                compare: wgpu::CompareFunction::Equal,
                fail_op: wgpu::StencilOperation::Keep,
                depth_fail_op: wgpu::StencilOperation::Keep,
                pass_op: wgpu::StencilOperation::IncrementClamp,
            };
            wgpu::StencilState {
                front: face,
                back: face,
                read_mask: 0xff,
                write_mask: 0xff,
            }
        } else {
            wgpu::StencilState::default()
        };

        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_3d_pipeline"),
            layout: Some(layout),
            vertex: wgpu::VertexState {
                module,
                entry_point: Some(vs_entry),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &constants,
                    ..Default::default()
                },
                buffers,
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                front_face: wgpu::FrontFace::Cw,
                // WGR_NO_CULL=1 disables backface culling on the object pipelines.
                // Diagnostic only: a single-sided face whose winding the loader got
                // backwards is INVISIBLE, and from inside a cockpit that reads as a
                // transparent airframe rather than as missing geometry. One run with
                // this set separates "not drawn because culled" from every other
                // reason a surface can fail to appear.
                cull_mode: if std::env::var("WGR_NO_CULL").map(|v| v != "0").unwrap_or(false) {
                    None
                } else {
                    Some(wgpu::Face::Back)
                },
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: Some(write),
                depth_compare: Some(if test {
                    // Reversed-Z (see shader3d.wgsl): nearer geometry has the larger
                    // depth value, so the "keep closer" test is GreaterEqual.
                    wgpu::CompareFunction::GreaterEqual
                } else {
                    wgpu::CompareFunction::Always
                }),
                stencil,
                bias,
            }),
            multisample: wgpu::MultisampleState {
                count: self.sample_count,
                alpha_to_coverage_enabled: a2c,
                ..Default::default()
            },
            fragment: Some(wgpu::FragmentState {
                module,
                entry_point: Some(fs_entry),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &constants,
                    ..Default::default()
                },
                targets: &[Some(wgpu::ColorTargetState {
                    format: self.surface_format,
                    blend: Self::blend_state(key.blend),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        self.pipelines.insert(key, pipeline);
    }

    // Create the depth+normal prepass pipeline for `key` if absent. Reuses the colour
    // pass' VS entry + pipeline layout + vertex buffers + override constants (VS parity
    // is load-bearing — see the plan's hazards), writes depth GreaterEqual/write-ON and
    // the view-space normal into NORMAL_FORMAT via fs_prepass. All prepassed draws have
    // offset None, so depth_bias is 0 here, matching their colour VS exactly.
    fn ensure_prepass_pipeline(&mut self, device: &wgpu::Device, key: PrepassKey) {
        if self.prepass_pipelines.contains_key(&key) {
            return;
        }
        let module = &self.shader;
        let (vs_entry, layout) = if key.skinned {
            ("vs_skinned", &self.skinned_layout)
        } else {
            ("vs_main", &self.plain_layout)
        };
        let vbuf_layout = wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<WgrMeshVertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &self.vbuf_attrs,
        };
        let skin_layout = wgpu::VertexBufferLayout {
            array_stride: 8,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &self.skin_attrs,
        };
        let plain_buffers = [vbuf_layout.clone()];
        let skinned_buffers = [vbuf_layout, skin_layout];
        let buffers: &[wgpu::VertexBufferLayout] = if key.skinned {
            &skinned_buffers
        } else {
            &plain_buffers
        };
        let alpha_ref = f32::from_bits(key.alpha_ref_bits) as f64;
        // cutout_mip_alpha MUST match the colour pipeline's, or the prepass lays depth for
        // texels the colour pass discards (gpu_driven.wgsl's prepass_match rationale).
        let constants = [
            ("alpha_ref", alpha_ref),
            ("depth_bias", 0.0),
            ("cutout_mip_alpha", cutout_mip_alpha() as f64),
            ("count_fragments", count_fragments() as f64),
        ];
        // Cutout foliage under MSAA: the A2C prepass twin emits a vec4 whose .a carries coverage,
        // and the pipeline enables alpha_to_coverage so it writes depth to exactly the samples the
        // colour pass will shade. Pure-opaque prepass (alpha_ref == 0) keeps the vec2 fs_prepass.
        let a2c = self.foliage_a2c && alpha_ref > 0.0;
        let fs_entry = if a2c { "fs_prepass_a2c" } else { "fs_prepass" };
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_3d_prepass_pipeline"),
            layout: Some(layout),
            vertex: wgpu::VertexState {
                module,
                entry_point: Some(vs_entry),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &constants,
                    ..Default::default()
                },
                buffers,
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                front_face: wgpu::FrontFace::Cw,
                // WGR_NO_CULL=1 disables backface culling on the object pipelines.
                // Diagnostic only: a single-sided face whose winding the loader got
                // backwards is INVISIBLE, and from inside a cockpit that reads as a
                // transparent airframe rather than as missing geometry. One run with
                // this set separates "not drawn because culled" from every other
                // reason a surface can fail to appear.
                cull_mode: if std::env::var("WGR_NO_CULL").map(|v| v != "0").unwrap_or(false) {
                    None
                } else {
                    Some(wgpu::Face::Back)
                },
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: Some(true),
                // Reversed-Z: nearer geometry has the larger depth value.
                depth_compare: Some(wgpu::CompareFunction::GreaterEqual),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState {
                count: self.sample_count,
                alpha_to_coverage_enabled: a2c,
                ..Default::default()
            },
            fragment: Some(wgpu::FragmentState {
                module,
                entry_point: Some(fs_entry),
                compilation_options: wgpu::PipelineCompilationOptions {
                    constants: &constants,
                    ..Default::default()
                },
                targets: &[Some(wgpu::ColorTargetState {
                    format: NORMAL_FORMAT,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            multiview_mask: None,
            cache: None,
        });
        self.prepass_pipelines.insert(key, pipeline);
    }

    pub fn mesh_create(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        verts: &[WgrMeshVertex],
        indices: &[u32],
    ) -> u64 {
        // Suballocate into the shared geometry pool (Uint32 indices throughout).
        // Returns None (=> the 0 handle) for an empty mesh, matching the old behaviour.
        let gen_before = self.pool.generation();
        let Some(alloc) = self.pool.alloc(device, queue, verts, indices) else {
            return 0;
        };
        // A pool growth reallocates the vbuf that every cached skin-bake bind references
        // (binding 0), so drop the cache when the pool moved.
        if self.pool.generation() != gen_before {
            self.bake_bind_cache.clear();
            self.bake_bind_cache_prev.clear();
        }
        let (mut aabb_min, mut aabb_max) = ([f32::MAX; 3], [f32::MIN; 3]);
        for v in verts {
            let p = [v.pos.x, v.pos.y, v.pos.z];
            for a in 0..3 {
                aabb_min[a] = aabb_min[a].min(p[a]);
                aabb_max[a] = aabb_max[a].max(p[a]);
            }
        }
        let key = self.meshes.insert(Mesh {
            alloc,
            index_count: indices.len() as u32,
            vert_count: verts.len() as u32,
            skin: None,
            aabb_min,
            aabb_max,
        });
        key.data().as_ffi()
    }

    pub fn geometry_residency(&self) -> pool::GeometryResidency {
        self.pool.residency()
    }

    pub fn frame_submitted(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) {
        // REN-RES-002: the pool may reallocate DOWN here. A shrink moves the pool buffers
        // exactly as a growth does, so it invalidates the same cache — the skin-bake binds
        // hold the vbuf at binding 0. mesh_create already does this for the growth case.
        if self.pool.frame_submitted(device, queue) {
            self.bake_bind_cache.clear();
            self.bake_bind_cache_prev.clear();
        }
    }

    // Attach interleaved per-vertex skin data (4 bone indices + 4 weights).
    pub fn mesh_set_skin(
        &mut self,
        device: &wgpu::Device,
        handle: u64,
        bones: &[u8],
        weights: &[u8],
    ) {
        let key: MeshKey = KeyData::from_ffi(handle).into();
        let Some(mesh) = self.meshes.get_mut(key) else {
            return;
        };
        let n = mesh.vert_count as usize;
        if bones.len() < n * 4 || weights.len() < n * 4 {
            return;
        }
        // Interleave to 8 bytes/vertex: [b0 b1 b2 b3 w0 w1 w2 w3].
        let mut data = Vec::with_capacity(n * 8);
        for v in 0..n {
            data.extend_from_slice(&bones[v * 4..v * 4 + 4]);
            data.extend_from_slice(&weights[v * 4..v * 4 + 4]);
        }
        mesh.skin = Some(
            device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("wgr_3d_skin"),
                contents: &data,
                // STORAGE so the compute skin bake reads bones/weights; VERTEX so the
                // fallback VS-skinning path (WGR_SKIN_BAKE=0) can still bind it as attrs.
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::STORAGE,
            }),
        );
        // A cached skin-bake bind would reference the old (or absent) skin buffer.
        self.bake_bind_cache.remove(&key);
        self.bake_bind_cache_prev.remove(&key);
    }

    // Re-upload vertex data for an existing (dynamic) mesh, e.g. a skeletally
    // animated character whose vertices are CPU-transformed each frame. The
    // topology (indices) is unchanged; only positions/normals/uvs are rewritten.
    pub fn mesh_update(&mut self, queue: &wgpu::Queue, handle: u64, verts: &[WgrMeshVertex]) {
        let Some(mesh) = self.meshes.get(KeyData::from_ffi(handle).into()) else {
            return;
        };
        if verts.is_empty() || verts.len() as u32 > mesh.vert_count {
            return;
        }
        let vbase = mesh.alloc.vbase;
        self.pool.update_verts(queue, vbase, verts);
    }

    // REN-THR-009 — the three reads an off-handle upload needs, exposed without the
    // `&mut self` that `mesh_update` carries for no reason. `vbase` and `vert_count` are
    // fixed for the life of a mesh (the pool never compacts); the pool buffer is NOT, which
    // is why `pool_generation` is published alongside it and the renderer republishes at
    // the two sites where the epoch can move.
    pub fn mesh_upload_meta(&self, handle: u64) -> Option<(u32, u32)> {
        let mesh = self.meshes.get(KeyData::from_ffi(handle).into())?;
        Some((mesh.alloc.vbase, mesh.vert_count))
    }

    pub fn pool_generation(&self) -> u64 {
        self.pool.generation()
    }

    pub fn pool_vbuf(&self) -> &wgpu::Buffer {
        self.pool.vbuf()
    }

    pub fn mesh_destroy(&mut self, handle: u64) {
        if handle != 0 {
            let key: MeshKey = KeyData::from_ffi(handle).into();
            // Return the mesh's pool ranges to the free-list so a later load reuses them.
            if let Some(mesh) = self.meshes.remove(key) {
                self.pool
                    .free(&mesh.alloc, mesh.vert_count, mesh.index_count);
            }
            // Drop any cached skin-bake bind that referenced this mesh's skin buffer.
            self.bake_bind_cache.remove(&key);
            self.bake_bind_cache_prev.remove(&key);
        }
    }

    // The baked-vertex offset for a skinned draw/caster, or None when the skin bake is
    // off or this slot isn't skinned (docs/compute-skin-bake-plan.md). When Some, the
    // Draw-side routing: colour/prepass/shadow draws of baked skinned meshes read the
    // bake output through the rigid pipelines (identity world + base_vertex slice).
    // This was disconnected for half a day on 2026-08-30 while its "corruption" was
    // hunted — the actual bug was skin_bake.wgsl still writing the historical 9-word
    // vertex into a world of 17-word readers (see WORDS_PER_VERT there). Kept as a
    // separate seam from baked_base_vertex so the two consumers (draws vs velocity)
    // can be split again in one line if that ever needs re-testing.
    fn baked_draw_base(&self, palette_slot: u32, mesh: u64) -> Option<u32> {
        self.baked_base_vertex(palette_slot, mesh)
    }

    // Velocity-pass association only (collect_skin_bases + render_object_velocity).
    fn baked_base_vertex(&self, palette_slot: u32, mesh: u64) -> Option<u32> {
        if !self.skin_bake_enabled || palette_slot == NO_PALETTE {
            return None;
        }
        self.skin_base_vertex.get(&(palette_slot, mesh)).copied()
    }

    // REN-TEMP-001I: this frame's (object id, mesh) -> baked base_vertex, for the skinned
    // velocity association. Call after prepare_skin_bake; identified skinned draws only.
    pub fn collect_skin_bases(&self, draws: &[WgrDraw3D]) -> Vec<(u32, u64, u32)> {
        let mut out = Vec::new();
        for d in draws {
            let id = d.misc >> 16;
            if id == 0 || d.palette_slot == NO_PALETTE {
                continue;
            }
            if let Some(base) = self.baked_base_vertex(d.palette_slot, d.mesh) {
                out.push((id, d.mesh, base));
            }
        }
        out
    }

    // Build this frame's skin-bake plan (docs/compute-skin-bake-plan.md) from BOTH the
    // color draws and the shadow casters, deduped by palette_slot (1:1 with a mesh+pose),
    // upload the palette as one flat storage buffer, and grow the shared output vertex
    // buffer. Must run BEFORE prepare_shadows + prepare so those pack an identity world
    // for every baked entry. No-op (and clears state) when the bake is disabled.
    pub fn prepare_skin_bake(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        draws: &[WgrDraw3D],
        casters: &[WgrShadowCaster],
        palette: &[WgrMat4],
    ) {
        self.bake_groups.clear();
        self.skin_base_vertex.clear();
        // bake_bind_cache persists across frames (mesh-keyed); it is invalidated only on
        // buffer growth (below) or mesh destroy/reskin, not rebuilt per frame.
        if !self.skin_bake_enabled {
            return;
        }
        // REN-TEMP-001I: ping-pong the bake output. LAST frame's baked vertices become
        // this frame's previous pose (skinned velocity pass); the old prev becomes this
        // frame's write target (its stale contents are fully overwritten by the bake).
        std::mem::swap(&mut self.skinned_vbuf, &mut self.skinned_vbuf_prev);
        std::mem::swap(&mut self.skinned_cap, &mut self.skinned_cap_prev);
        std::mem::swap(&mut self.bake_bind_cache, &mut self.bake_bind_cache_prev);
        self.skinned_prev_valid = self.skinned_baked_this_frame && self.skinned_vbuf_prev.is_some();
        self.skinned_baked_this_frame = false;

        // Collect distinct skinned instances by (palette_slot, mesh). One palette
        // block is one skeleton's POSE, but that skeleton is drawn as several meshes
        // (body, head, weapon), each of which needs its own baked slice against the
        // same block. Deduping by slot alone corrupts every sibling mesh.
        let mut out_base: u32 = 0;
        let mut consider = |mesh_id: u64, palette_slot: u32, this: &mut Self| {
            if palette_slot == NO_PALETTE {
                return;
            }
            let Some(mesh) = this.meshes.get(KeyData::from_ffi(mesh_id).into()) else {
                return;
            };
            if mesh.skin.is_none() || mesh.vert_count == 0 {
                return;
            }
            if this.skin_base_vertex.contains_key(&(palette_slot, mesh_id)) {
                return;
            }
            let key = KeyData::from_ffi(mesh_id).into();
            this.skin_base_vertex.insert((palette_slot, mesh_id), out_base);
            this.bake_groups.push(BakeGroup {
                mesh: key,
                palette_base: palette_slot,
                out_base_vertex: out_base,
                instance_count: 1,
                vert_count: mesh.vert_count,
                in_base_vertex: mesh.alloc.vbase,
            });
            out_base += mesh.vert_count;
        };
        for d in draws {
            consider(d.mesh, d.palette_slot, self);
        }
        for c in casters {
            consider(c.mesh, c.palette_slot, self);
        }
        if self.bake_groups.is_empty() {
            return;
        }

        // Palette (all blocks) as a flat storage buffer, block b at b*128. Uploaded once
        // here for the compute bake; the fallback VS path's dynamic-offset UBO is skipped
        // in prepare() when the bake is on. `ensure` reports growth so the (whole-buffer)
        // cached binds referencing it can be dropped only when it actually moved.
        let mut buffers_grew = false;
        if !palette.is_empty() {
            buffers_grew |= self
                .palette_buf
                .ensure(device, std::mem::size_of_val(palette) as u64);
            queue.write_buffer(
                self.palette_buf.buf.as_ref().unwrap(),
                0,
                bytemuck::cast_slice(palette),
            );
        }

        // Grow the shared output vertex buffer (STORAGE for the compute write + VERTEX for
        // every pass' read). `out_base` is the total baked vertex count.
        let needed = out_base as u64 * BAKED_VERT_SIZE;
        if self.skinned_cap < needed || self.skinned_vbuf.is_none() {
            let cap = needed.next_power_of_two().max(64 * 1024);
            self.skinned_vbuf = Some(device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("wgr_skinned_vbuf"),
                size: cap,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::VERTEX,
                mapped_at_creation: false,
            }));
            self.skinned_cap = cap;
            buffers_grew = true;
        }
        // A cached group(0) bind pins the specific palette_buf/skinned_vbuf it was built
        // against; when either regrows they are stale, so flush.
        if buffers_grew {
            self.bake_bind_cache.clear();
            self.bake_bind_cache_prev.clear();
        }

        // All group params in ONE upload (the dynamic-offset UBO strides entries by
        // min_uniform_buffer_offset_alignment). Building the whole strided region in a
        // scratch buffer and writing it once replaces the per-group write_buffer that
        // showed up in captures as a copy-per-group.
        self.skin_bake_params.ensure(device, self.bake_groups.len());
        let pbuf = self.skin_bake_params.buf.as_ref().unwrap();
        let stride = self.skin_bake_params.stride as usize;
        let mut scratch = vec![0u8; self.bake_groups.len() * stride];
        for (i, g) in self.bake_groups.iter().enumerate() {
            let p = BakeParamsGpu {
                vert_count: g.vert_count,
                instance_count: g.instance_count,
                palette_base: g.palette_base,
                out_base_vertex: g.out_base_vertex,
                in_base_vertex: g.in_base_vertex,
                _pad: [0; 3],
            };
            let off = i * stride;
            scratch[off..off + std::mem::size_of::<BakeParamsGpu>()]
                .copy_from_slice(bytemuck::bytes_of(&p));
        }
        queue.write_buffer(pbuf, 0, &scratch);

        // Ensure a cached group(0) bind exists for each group's mesh (device available
        // here; skin_bake only records). Whole-buffer palette/output + per-mesh vbuf/skin,
        // so one bind serves every instance of a mesh and persists across frames. Build
        // missing ones into a temp Vec first, then insert — keeps the self borrows disjoint.
        let (Some(pal_buf), Some(out_buf)) =
            (self.palette_buf.buf.as_ref(), self.skinned_vbuf.as_ref())
        else {
            self.bake_groups.clear();
            self.skin_base_vertex.clear();
            return;
        };
        // The bake reads every mesh's rest pose from the shared pool vbuf (binding 0),
        // offset per-group by in_base_vertex; only the per-mesh skin buffer (binding 1)
        // differs between meshes. Whole-buffer, so the bind is stable frame-to-frame.
        let pool_vbuf = self.pool.vbuf();
        let mut new_binds: Vec<(MeshKey, wgpu::BindGroup)> = Vec::new();
        for g in &self.bake_groups {
            if self.bake_bind_cache.contains_key(&g.mesh)
                || new_binds.iter().any(|(k, _)| *k == g.mesh)
            {
                continue;
            }
            let Some(mesh) = self.meshes.get(g.mesh) else {
                continue;
            };
            let Some(skin) = mesh.skin.as_ref() else {
                continue;
            };
            let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_skin_bake_bind"),
                layout: &self.skin_bake_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: pool_vbuf.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: skin.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: pal_buf.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: out_buf.as_entire_binding(),
                    },
                ],
            });
            new_binds.push((g.mesh, bind));
        }
        for (k, b) in new_binds {
            self.bake_bind_cache.insert(k, b);
        }
        // REN-TEMP-001I: next frame's swap may trust this buffer as "previous pose".
        self.skinned_baked_this_frame = true;
    }

    // Record the compute skin-bake pass: one dispatch per BakeGroup, skinning its verts
    // into `skinned_vbuf`. Recorded FIRST in the frame encoder so wgpu's automatic
    // storage->vertex barrier covers every later read (shadows, prepass, forward). No-op
    // when the bake is off or the frame has no skinned geometry.
    pub fn skin_bake(&self, encoder: &mut wgpu::CommandEncoder) {
        if !self.skin_bake_enabled || self.bake_groups.is_empty() {
            return;
        }
        let Some(params_bind) = self.skin_bake_params.bind.as_ref() else {
            return;
        };
        encoder.push_debug_group("wgr_skin_bake");
        let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_skin_bake"),
            timestamp_writes: None,
        });
        cp.set_pipeline(&self.skin_bake_pipeline);
        // group(0) is per-mesh (cached); only rebind it when the mesh changes from the
        // previous dispatch. group(1) is the same buffer at a per-group dynamic offset —
        // a cheap offset rebind, not a descriptor update.
        let mut last_mesh: Option<MeshKey> = None;
        for (i, g) in self.bake_groups.iter().enumerate() {
            let Some(bind) = self.bake_bind_cache.get(&g.mesh) else {
                continue;
            };
            if last_mesh != Some(g.mesh) {
                cp.set_bind_group(0, bind, &[]);
                last_mesh = Some(g.mesh);
            }
            cp.set_bind_group(
                1,
                params_bind,
                &[(i as u64 * self.skin_bake_params.stride) as u32],
            );
            let threads = g.vert_count * g.instance_count;
            cp.dispatch_workgroups(threads.div_ceil(64), 1, 1);
        }
        drop(cp);
        encoder.pop_debug_group();
    }

    // (Re)create the depth target to match the surface
    pub fn ensure_depth(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let size = (width.max(1), height.max(1));
        if self.depth_size == size && self.depth.is_some() {
            return;
        }
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_3d_depth"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: self.sample_count,
            dimension: wgpu::TextureDimension::D2,
            format: DEPTH_FORMAT,
            // TEXTURE_BINDING so the depth aspect can be sampled: the Hi-Z copy pass reads it
            // directly at 1x, and the MSAA depth-resolve pass reads it multisampled at Nx.
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        // Depth-aspect view (Depth24PlusStencil8 must pick an aspect explicitly).
        let depth_aspect = texture.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_3d_depth_sample"),
            aspect: wgpu::TextureAspect::DepthOnly,
            ..Default::default()
        });
        if let Some(dr) = self.depth_resolve.as_mut() {
            // MSAA: the depth-aspect view above is multisampled — bind it as the resolve pass'
            // source, and hand its single-sample resolved output to the Hi-Z / sampling path.
            self.depth_sample_view = Some(dr.resize(device, size.0, size.1, &depth_aspect));
            // Water gets the farthest-sample resolve (its own target) off the same MSAA source.
            self.water_depth_view = self
                .depth_resolve_far
                .as_mut()
                .map(|dr_far| dr_far.resize(device, size.0, size.1, &depth_aspect));
        } else {
            // 1x: consumers sample the depth target's own depth aspect directly (exact — a single
            // sample, so no A2C edge poisoning; water and Hi-Z share it).
            self.depth_sample_view = Some(depth_aspect.clone());
            self.water_depth_view = Some(depth_aspect);
        }
        self.depth = Some((texture, view));
        // View-space normal G-buffer, matched to the depth size and sample count (it is the
        // prepass' colour attachment, co-rendered with the MSAA depth). TEXTURE_BINDING is
        // harmless now; SSAO will additionally need a resolve_target on the prepass normal
        // attachment to sample it single-sample (nothing samples it yet).
        let normal = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_3d_normal"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: self.sample_count,
            dimension: wgpu::TextureDimension::D2,
            format: NORMAL_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let normal_view = normal.create_view(&wgpu::TextureViewDescriptor::default());
        self.normal = Some((normal, normal_view));
        // Resolve target for GTAO, sized with its source. MSAA only; at 1x the prepass
        // normal above is already single-sample and normal_sample_view stays None.
        self.normal_sample_view = self.normal_resolve.as_mut().map(|nr| {
            let src = self
                .normal
                .as_ref()
                .expect("normal target just created")
                .1
                .clone();
            nr.resize(device, size.0, size.1, &src)
        });
        // GTAO reads the SINGLE-SAMPLE normal: the resolve under MSAA, the prepass target
        // itself at 1x. Depth is the nearest resolve, which is what AO wants (front surface)
        // and is already built for Hi-Z — the plan is explicit that this must be reused
        // rather than duplicated.
        self.gtao_depth_mips.resize(device, size.0, size.1);
        if let (Some(depth), Some(normal), Some(mips)) = (
            self.depth_sample_view.clone(),
            self.normal_sample_view
                .clone()
                .or_else(|| self.normal.as_ref().map(|(_, v)| v.clone())),
            self.gtao_depth_mips.view().cloned(),
        ) {
            // GTAO marches the mip chain; the blur still rejects on raw depth, which is exact
            // per-pixel and needs no chain.
            let ao = self.gtao.resize(device, size.0, size.1, &mips, &normal);
            self.gtao_blur
                .resize(device, size.0, size.1, &depth, &normal, &ao);
        }
        self.depth_size = size;
        self.depth_gen += 1;
        // The Hi-Z pyramid tracks this size; (re)allocated in prepare_cull (gated on occlusion
        // being active) so a live toggle picks up the current size without a resize.
    }

    // Single-sample UI-phase depth-stencil (MSAA only): the post-tonemap 2D composites to the
    // 1x swapchain and needs a matching-sample depth attachment for its (1x) pipelines. Sized
    // separately from the scene depth — under render scale (REN-TEMP-001B) the scene depth is
    // render-resolution while this one must match the OUTPUT-resolution swapchain.
    pub fn ensure_ui_depth(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let size = (width.max(1), height.max(1));
        if self.sample_count <= 1 {
            self.ui_depth = None;
            return;
        }
        if self.ui_depth.is_some() && self.ui_depth_size == size {
            return;
        }
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_3d_ui_depth"),
            size: wgpu::Extent3d {
                width: size.0,
                height: size.1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: DEPTH_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.ui_depth = Some((tex, view));
        self.ui_depth_size = size;
    }

    pub fn depth_view(&self) -> Option<&wgpu::TextureView> {
        self.depth.as_ref().map(|(_, v)| v)
    }

    // Farthest-sample sceen depth for water's seabed reconstruction (see the field comment). Under
    // MSAA this is stale until resolve_water_depth records the far-resolve; at 1x it's the live 1x
    // depth aspect. Paired with depth_gen() for bind-group rebuild tracking.
    pub fn water_depth_view(&self) -> Option<&wgpu::TextureView> {
        self.water_depth_view.as_ref()
    }

    // REN-TEMP-001M: the far-resolve's TEXTURE, for native (NGX) interop. None at 1x —
    // there the "resolved" view is the live depth aspect and DLSS gets its image from
    // the depth target instead (not wired yet; MSAA is the supported first path).
    pub fn water_depth_texture(&self) -> Option<&wgpu::Texture> {
        self.depth_resolve_far.as_ref().and_then(|d| d.texture.as_ref())
    }

    // Record the water far-resolve (MSAA depth → single-sample farthest). No-op at 1x (water_depth_view
    // is the live depth aspect). Called from the frame graph right before the water pass, independent
    // of Hi-Z occlusion (which owns the separate nearest resolve), so water always samples fresh depth.
    pub fn resolve_water_depth(&self, encoder: &mut wgpu::CommandEncoder) {
        if let Some(dr_far) = self.depth_resolve_far.as_ref() {
            dr_far.resolve(encoder);
        }
    }

    pub fn depth_gen(&self) -> u64 {
        self.depth_gen
    }

    // REN-TEMP-001H: re-draw the frame's MOVING rigid draws into the velocity target
    // (LoadOp::Load — the fullscreen camera reprojection is already there). `subset`
    // is (draws3d index, previous world relative to the previous camera), built by
    // Temporal::object_velocity_subset. Occlusion is a fragment-shader compare against
    // the far-depth resolve (this pass has no depth attachment: the scene depth is
    // multisampled, the velocity target is 1x). Mirrors draw_one's mesh addressing.
    #[allow(clippy::too_many_arguments)]
    pub fn render_object_velocity(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        draws: &[crate::ffi::WgrDraw3D],
        subset: &[(u32, [f32; 16])],
        skin_subset: &[(u32, u32)],
        veg_subset: &[(u32, f32)],
        uniform: &crate::temporal::ObjVelUniform,
        veg_uniform: Option<&crate::temporal::VegVelUniform>,
    ) {
        // Skinned entries are drawable only when both sides of the bake ping-pong exist.
        let skin_ready = !skin_subset.is_empty()
            && self.skinned_prev_valid
            && self.skinned_vbuf.is_some()
            && self.skinned_vbuf_prev.is_some();
        // One-shot proof the SKINNED velocity path went live (it never had before
        // 2026-08-30: the bake it reads was default-off, and the gate above failed
        // silently). Says so once in the capture stderr, then stays quiet.
        if skin_ready {
            static SKIN_LIVE: std::sync::Once = std::sync::Once::new();
            SKIN_LIVE.call_once(|| {
                eprintln!(
                    "[wgr] skinned velocity live: {} skinned draw(s) this frame (bake ping-pong valid)",
                    skin_subset.len()
                );
            });
        }
        let veg_ready = veg_uniform.is_some() && !veg_subset.is_empty();
        if subset.is_empty() && !skin_ready && !veg_ready {
            return;
        }
        let Some(depth) = self.water_depth_view.clone() else {
            return;
        };
        if self.velocity_obj.is_none() {
            self.velocity_obj = Some(VelocityObjPass::new(device));
        }
        // Per-entry payload = velocity_obj.wgsl `VelObj`: current world then previous.
        let mut data: Vec<f32> = Vec::with_capacity(subset.len() * 32);
        for (idx, prev_rel) in subset {
            let d = &draws[*idx as usize];
            data.extend_from_slice(&d.world);
            data.extend_from_slice(prev_rel);
        }
        let bytes: &[u8] = bytemuck::cast_slice(&data);
        let vp = self.velocity_obj.as_mut().expect("velocity obj pass");
        if !bytes.is_empty() {
            if (bytes.len() as u64) > vp.ssbo_cap {
                vp.ssbo_cap = (bytes.len() as u64).next_power_of_two();
                vp.ssbo = device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("wgr_velocity_obj_worlds"),
                    size: vp.ssbo_cap,
                    usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                });
                vp.bind = None;
            }
            queue.write_buffer(&vp.ssbo, 0, bytes);
        }
        queue.write_buffer(&vp.uniform, 0, bytemuck::bytes_of(uniform));
        // Skinned per-instance data (velocity_obj.wgsl `SkinInst`) + this frame's bind —
        // rebuilt every call, because the prev-pose buffer alternates by design.
        let mut skin_bind: Option<wgpu::BindGroup> = None;
        if skin_ready {
            let insts: Vec<[u32; 4]> = skin_subset
                .iter()
                .map(|(_, prev_base)| [*prev_base, 0, 0, 0])
                .collect();
            let ibytes: &[u8] = bytemuck::cast_slice(&insts);
            if (ibytes.len() as u64) > vp.skin_ssbo_cap {
                vp.skin_ssbo_cap = (ibytes.len() as u64).next_power_of_two();
                vp.skin_ssbo = device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("wgr_velocity_skin_insts"),
                    size: vp.skin_ssbo_cap,
                    usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                });
            }
            queue.write_buffer(&vp.skin_ssbo, 0, ibytes);
            skin_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_velocity_skin_bind"),
                layout: &vp.skin_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: vp.uniform.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::TextureView(&depth),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: vp.skin_ssbo.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 4,
                        resource: self
                            .skinned_vbuf_prev
                            .as_ref()
                            .expect("skin_ready implies prev buffer")
                            .as_entire_binding(),
                    },
                ],
            }));
        }
        let mut veg_bind: Option<wgpu::BindGroup> = None;
        if veg_ready {
            let mut vdata: Vec<f32> = Vec::with_capacity(veg_subset.len() * 20);
            for (idx, leaf) in veg_subset {
                let d = &draws[*idx as usize];
                vdata.extend_from_slice(&d.world);
                vdata.extend_from_slice(&[*leaf, 0.0, 0.0, 0.0]);
            }
            let vbytes: &[u8] = bytemuck::cast_slice(&vdata);
            if (vbytes.len() as u64) > vp.veg_ssbo_cap {
                vp.veg_ssbo_cap = (vbytes.len() as u64).next_power_of_two();
                vp.veg_ssbo = device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("wgr_velocity_veg_insts"),
                    size: vp.veg_ssbo_cap,
                    usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                });
            }
            queue.write_buffer(&vp.veg_ssbo, 0, vbytes);
            queue.write_buffer(
                &vp.veg_uniform,
                0,
                bytemuck::bytes_of(veg_uniform.expect("veg_ready")),
            );
            veg_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_velocity_veg_bind"),
                layout: &vp.veg_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: vp.uniform.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::TextureView(&depth),
                    },
                    wgpu::BindGroupEntry {
                        binding: 5,
                        resource: vp.veg_uniform.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 6,
                        resource: vp.veg_ssbo.as_entire_binding(),
                    },
                ],
            }));
        }
        if vp.bind.is_none() || vp.bound_depth_gen != self.depth_gen {
            vp.bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_velocity_obj_bind"),
                layout: &vp.layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: vp.uniform.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: vp.ssbo.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::TextureView(&depth),
                    },
                ],
            }));
            vp.bound_depth_gen = self.depth_gen;
        }
        // Buffer growth and bind caching above needed `vp` mutable; the draw loops
        // below interleave immutable `self` lookups (meshes, baked bases), so reborrow
        // immutably before recording.
        let vp = self.velocity_obj.as_ref().expect("velocity obj pass");
        encoder.push_debug_group("wgr_velocity_obj");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_velocity_obj"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: target,
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
        pass.set_index_buffer(self.pool.ibuf().slice(..), wgpu::IndexFormat::Uint32);
        if !subset.is_empty() {
            pass.set_pipeline(&vp.pipeline);
            pass.set_bind_group(0, vp.bind.as_ref().expect("velocity obj bind"), &[]);
            for (i, (idx, _)) in subset.iter().enumerate() {
                let d = &draws[*idx as usize];
                let key: MeshKey = slotmap::KeyData::from_ffi(d.mesh).into();
                let Some(mesh) = self.meshes.get(key) else {
                    continue;
                };
                // Rigid only, and stay inside the mesh's index range (mirrors draw_one).
                if d.index_begin + d.index_count > mesh.index_count {
                    continue;
                }
                let vert_off = mesh.alloc.vbase as u64 * BAKED_VERT_SIZE;
                pass.set_vertex_buffer(0, self.pool.vbuf().slice(vert_off..));
                let first = mesh.alloc.ibase + d.index_begin;
                pass.draw_indexed(first..first + d.index_count, 0, i as u32..i as u32 + 1);
            }
        }
        // REN-TEMP-001I: skinned draws — current pose from this frame's bake output
        // (sliced to the draw's baked base, exactly like draw_one), previous pose read
        // in-shader from the ping-ponged buffer by mesh-local vertex index.
        if let Some(skin_bind) = skin_bind.as_ref() {
            let cur_baked = self.skinned_vbuf.as_ref().expect("skin_ready implies cur");
            pass.set_pipeline(&vp.skin_pipeline);
            pass.set_bind_group(0, skin_bind, &[]);
            for (i, (idx, _)) in skin_subset.iter().enumerate() {
                let d = &draws[*idx as usize];
                let key: MeshKey = slotmap::KeyData::from_ffi(d.mesh).into();
                let Some(mesh) = self.meshes.get(key) else {
                    continue;
                };
                if d.index_begin + d.index_count > mesh.index_count {
                    continue;
                }
                let Some(cur_base) = self.baked_base_vertex(d.palette_slot, d.mesh) else {
                    continue;
                };
                pass.set_vertex_buffer(0, cur_baked.slice(cur_base as u64 * BAKED_VERT_SIZE..));
                let first = mesh.alloc.ibase + d.index_begin;
                pass.draw_indexed(first..first + d.index_count, 0, i as u32..i as u32 + 1);
            }
        }
        // REN-TEMP-001T: swaying vegetation — real motion vectors from the closed-form
        // sway evaluated at both frames' wind parameters. Drawn last so its (small)
        // deltas also override the camera-only reprojection under the canopy.
        if let Some(veg_bind) = veg_bind.as_ref() {
            pass.set_pipeline(&vp.veg_pipeline);
            pass.set_bind_group(0, veg_bind, &[]);
            for (i, (idx, _)) in veg_subset.iter().enumerate() {
                let d = &draws[*idx as usize];
                let key: MeshKey = slotmap::KeyData::from_ffi(d.mesh).into();
                let Some(mesh) = self.meshes.get(key) else {
                    continue;
                };
                if d.index_begin + d.index_count > mesh.index_count {
                    continue;
                }
                let vert_off = mesh.alloc.vbase as u64 * BAKED_VERT_SIZE;
                pass.set_vertex_buffer(0, self.pool.vbuf().slice(vert_off..));
                let first = mesh.alloc.ibase + d.index_begin;
                pass.draw_indexed(first..first + d.index_count, 0, i as u32..i as u32 + 1);
            }
        }
        drop(pass);
        encoder.pop_debug_group();
    }

    // Single-sample UI-phase depth (Some only under MSAA); the post-tonemap 2D uses it instead of
    // the multisampled scene depth so its 1x pipelines / swapchain target match.
    pub fn ui_depth_view(&self) -> Option<&wgpu::TextureView> {
        self.ui_depth.as_ref().map(|(_, v)| v)
    }

    // The prepass' view-space normal G-buffer view (the prepass colour attachment).
    pub fn normal_view(&self) -> Option<&wgpu::TextureView> {
        self.normal.as_ref().map(|(_, v)| v)
    }

    // The cascade shadow depth map as a D2Array view (or the 1x1 dummy when no shadows),
    // lent to the froxel fill so it can occlude the fog by objects + terrain (god rays).
    pub fn shadow_sample_view(&self) -> &wgpu::TextureView {
        self.shadow_target
            .as_ref()
            .map(|t| &t.sample_view)
            .unwrap_or(&self.dummy_shadow_view)
    }

    // Group-0 (camera UBO + shadow map) layout, shared with the terrain pipeline so
    // terrain reuses the world camera and shadow resources.
    pub fn camera_layout(&self) -> &wgpu::BindGroupLayout {
        &self.cameras.layout
    }

    /// The cascade light-VP dynamic UBO layout, lent to the grass shadow
    /// pipeline so blades land in the same depth array as scene casters.
    pub fn shadow_pass_layout(&self) -> &wgpu::BindGroupLayout {
        &self.shadow_pass_ubo.layout
    }

    // Camera bind group for the current frame (valid after `prepare`); index a
    // camera by `slot * camera_stride()` as the dynamic offset.
    pub fn camera_bind(&self) -> Option<&wgpu::BindGroup> {
        self.cameras.bind.as_ref()
    }

    pub fn camera_stride(&self) -> u64 {
        self.cameras.stride
    }

    fn ensure_shadow_target(&mut self, device: &wgpu::Device, res: u32, layers: u32) {
        if let Some(t) = &self.shadow_target {
            if t.res == res && t.layers == layers {
                return;
            }
        }
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_shadow_map"),
            size: wgpu::Extent3d {
                width: res,
                height: res,
                depth_or_array_layers: layers,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: SHADOW_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let layer_views = (0..layers)
            .map(|l| {
                tex.create_view(&wgpu::TextureViewDescriptor {
                    label: Some("wgr_shadow_layer"),
                    dimension: Some(wgpu::TextureViewDimension::D2),
                    base_array_layer: l,
                    array_layer_count: Some(1),
                    ..Default::default()
                })
            })
            .collect();
        let sample_view = tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_shadow_sample"),
            dimension: Some(wgpu::TextureViewDimension::D2Array),
            ..Default::default()
        });
        self.shadow_target = Some(ShadowTarget {
            tex,
            layer_views,
            sample_view,
            res,
            layers,
        });
        self.shadow_gen += 1;
    }

    fn ensure_shadow_pipelines(&mut self, device: &wgpu::Device) {
        if self.shadow_pipelines.is_some() {
            return;
        }
        let vbuf_layout = wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<WgrMeshVertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &self.vbuf_attrs,
        };
        let skin_layout = wgpu::VertexBufferLayout {
            array_stride: 8,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &self.skin_attrs,
        };
        let plain_buffers = [vbuf_layout.clone()];
        let skinned_buffers = [vbuf_layout, skin_layout];
        let build = |vs: &str, fs: Option<&str>, skinned: bool| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("wgr_shadow_depth_pipeline"),
                layout: Some(if skinned {
                    &self.shadow_skinned_layout
                } else {
                    &self.shadow_layout
                }),
                vertex: wgpu::VertexState {
                    module: &self.shadow_shader,
                    entry_point: Some(vs),
                    compilation_options: Default::default(),
                    buffers: if skinned {
                        &skinned_buffers
                    } else {
                        &plain_buffers
                    },
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    front_face: wgpu::FrontFace::Cw,
                    // No culling: single-sided walls/roofs must still cast.
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: SHADOW_FORMAT,
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
                fragment: fs.map(|entry| wgpu::FragmentState {
                    module: &self.shadow_shader,
                    entry_point: Some(entry),
                    compilation_options: Default::default(),
                    targets: &[],
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        self.shadow_pipelines = Some(ShadowPipelines {
            solid: build("vs_solid", None, false),
            alpha: build("vs_alpha", Some("fs_alpha"), false),
            skin_solid: build("vs_skin_solid", None, true),
            skin_alpha: build("vs_skin_alpha", Some("fs_skin_alpha"), true),
        });
    }

    pub fn prepare_shadows(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        pass: &WgrShadowPass,
        casters: &[WgrShadowCaster],
        grass: &Grass,
    ) {
        let count = pass.count.min(MAX_CASCADES);
        // The GPU-driven set casts its own shadows (draw_gpu_driven_shadow), so the target +
        // pass UBO must be set up even when there are no CPU casters this frame; only the CPU
        // caster bucketing below is skipped when `casters` is empty.
        let gpu_shadows = self.gpu_driven_enabled;
        if count == 0
            || pass.resolution == 0
            || (casters.is_empty() && !gpu_shadows && !grass.casts_shadows())
        {
            self.shadow_plan.clear();
            return;
        }
        self.ensure_shadow_target(device, pass.resolution, count);
        self.ensure_shadow_pipelines(device);

        self.shadow_pass_ubo.ensure(device, count as usize);
        let buf = self.shadow_pass_ubo.buf.as_ref().unwrap();
        for c in 0..count as usize {
            let entry = ShadowPassUbo {
                light_vp: pass.light_vp[c],
                cam_pos: pass.cam_pos,
            };
            queue.write_buffer(
                buf,
                c as u64 * self.shadow_pass_ubo.stride,
                bytemuck::bytes_of(&entry),
            );
        }

        // No CPU casters (GPU-driven set casts on its own): the target + pass UBO above are all
        // the GPU shadow draw needs, so skip the CPU bucketing entirely.
        self.shadow_plan.clear();
        if casters.is_empty() {
            return;
        }

        // Bucket casters per cascade into instanced draws (mirrors plan_3d for the color
        // pass). Depth-only casters are all order-independent, so within a cascade we
        // coalesce non-skinned casters by (mesh, section, alpha, texture, sampler) and
        // pack their GPU data contiguously — one instanced draw per bucket instead of one
        // draw per caster. Skinned casters can't instance (per-caster palette offset), so
        // each is its own count-1 bucket. The packed array is laid out per (cascade,
        // bucket); a caster in several cascades is packed once per cascade (its data is
        // cascade-independent, but its bucket position isn't).
        let mut caster_gpu: Vec<ShadowCasterGpu> = Vec::with_capacity(casters.len());
        let mut bucket_index: FxHashMap<ShadowBucketKey, usize> = FxHashMap::default();
        for c in 0..count as usize {
            // Buckets in first-seen order + their member caster indices.
            let mut buckets: Vec<(u32, Vec<u32>)> = Vec::new();
            bucket_index.clear();
            let mut cascade: Vec<ShadowBucket> = Vec::new();
            for (i, caster) in casters.iter().enumerate() {
                if caster.cascade_mask & (1 << c) == 0 || caster.index_count == 0 {
                    continue;
                }
                let Some(mesh) = self.meshes.get(KeyData::from_ffi(caster.mesh).into()) else {
                    continue;
                };
                if caster.index_begin + caster.index_count > mesh.index_count {
                    continue;
                }
                let alpha = caster.alpha_ref > 0.0;
                // Baked casters (docs/compute-skin-bake-plan.md) route through the RIGID
                // depth pipeline reading skinned_vbuf; `skinned` = the VS-skinning fallback.
                let baked = if mesh.skin.is_some() {
                    self.baked_draw_base(caster.palette_slot, caster.mesh)
                } else {
                    None
                };
                let skinned =
                    caster.palette_slot != NO_PALETTE && mesh.skin.is_some() && baked.is_none();
                if skinned {
                    // Skinned casters read the bone palette, not the SSBO; base_instance is
                    // unused by their shader, but pack an entry so the array stays dense.
                    let base = caster_gpu.len() as u32;
                    caster_gpu.push(ShadowCasterGpu {
                        world: caster.world,
                        conform0: caster.conform0,
                        conform2: caster.conform2,
                    });
                    cascade.push(ShadowBucket {
                        repr: i as u32,
                        base,
                        count: 1,
                    });
                } else if baked.is_some() {
                    // Baked caster: the rigid vs_solid/vs_alpha reads casters[instance].world,
                    // so pack an IDENTITY world (the pose's world is folded into the palette,
                    // already applied by the bake). Each pose is unique, so it can't coalesce
                    // with the rigid mesh's buffer — its own count-1 bucket.
                    let base = caster_gpu.len() as u32;
                    caster_gpu.push(ShadowCasterGpu {
                        world: IDENTITY_MAT4,
                        conform0: [0.0; 4],
                        conform2: [0.0; 4],
                    });
                    cascade.push(ShadowBucket {
                        repr: i as u32,
                        base,
                        count: 1,
                    });
                } else {
                    let key = ShadowBucketKey {
                        mesh: caster.mesh,
                        index_begin: caster.index_begin,
                        index_count: caster.index_count,
                        alpha,
                        texture_id: if alpha { caster.texture_id } else { 0 },
                        sampler: if alpha { caster.sampler.0 } else { 0 },
                    };
                    let bi = *bucket_index.entry(key).or_insert_with(|| {
                        buckets.push((i as u32, Vec::new()));
                        buckets.len() - 1
                    });
                    buckets[bi].1.push(i as u32);
                }
            }
            // Flush the cascade's coalesced buckets: pack each contiguously.
            for (repr, members) in buckets {
                let base = caster_gpu.len() as u32;
                let bcount = members.len() as u32;
                for &mi in &members {
                    let caster = &casters[mi as usize];
                    caster_gpu.push(ShadowCasterGpu {
                        world: caster.world,
                        conform0: caster.conform0,
                        conform2: caster.conform2,
                    });
                }
                cascade.push(ShadowBucket {
                    repr,
                    base,
                    count: bcount,
                });
            }
            self.shadow_plan.push(cascade);
        }
        // Upload the whole reordered array in ONE write_buffer (indexed in-shader by
        // base_instance) — replaces the per-caster dynamic-UBO write loop.
        let grew = self
            .shadow_caster_ssbo
            .ensure(device, std::mem::size_of_val(caster_gpu.as_slice()) as u64);
        let buf = self.shadow_caster_ssbo.buf.as_ref().unwrap();
        if !caster_gpu.is_empty() {
            queue.write_buffer(buf, 0, bytemuck::cast_slice(&caster_gpu));
        }
        if grew || self.shadow_caster_bind.is_none() {
            self.shadow_caster_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_shadow_caster_bind"),
                layout: &self.shadow_caster_layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: buf.as_entire_binding(),
                }],
            }));
        }
    }

    pub fn render_shadow_passes(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        textures: &SharedTextures,
        pass: &WgrShadowPass,
        casters: &[WgrShadowCaster],
        grass: &Grass,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        let count = pass.count.min(MAX_CASCADES);
        // The GPU-driven set casts on its own, so render even with no CPU casters (as long as
        // the target + pass UBO exist). Nothing to do only when both sources are empty.
        let gpu_shadows = self.gpu_driven_enabled;
        if count == 0 || (casters.is_empty() && !gpu_shadows && !grass.casts_shadows()) {
            return;
        }
        let (Some(target), Some(pass_bind)) = (
            self.shadow_target.as_ref(),
            self.shadow_pass_ubo.bind.as_ref(),
        ) else {
            return;
        };
        // CPU caster resources (absent when this frame has no CPU casters).
        let cpu = self
            .shadow_pipelines
            .as_ref()
            .zip(self.shadow_caster_bind.as_ref());

        // Which cascades get grass blades. C++ derives the mask from the cascade split
        // distances and the grass reach (see the note at the draw_shadow call below).
        //
        // WGR_GRASS_SHADOW_CASCADES OVERRIDES the mask rather than intersecting it, so
        // `=4` restores the old unconditional behaviour exactly and the optimisation can be
        // A/B'd on one binary. Intersecting would have made the "off" arm impossible to
        // express, which is how a change ends up shipped with no measured before-and-after.
        let grass_mask: u32 = {
            static ONCE: std::sync::OnceLock<Option<u32>> = std::sync::OnceLock::new();
            let forced = *ONCE.get_or_init(|| {
                std::env::var("WGR_GRASS_SHADOW_CASCADES")
                    .ok()
                    .and_then(|v| v.parse::<u32>().ok())
                    .map(|n| {
                        let n = n.min(MAX_CASCADES);
                        if n == 0 { 0 } else { (1u32 << n) - 1 }
                    })
            });
            forced.unwrap_or(pass.grass_cascade_mask)
        };
        for c in 0..count.min(target.layers) as usize {
            // PERF-005: one region PER CASCADE. Shadows were the largest single unattributed
            // suspect and a single "shadows" number cannot tell you which cascade to shrink —
            // cascade 0 covers metres and cascade 3 covers hundreds of them, so their costs and
            // their fixes are unrelated. Bracketed on the ENCODER around the whole cascade pass,
            // which needs no TIMESTAMP_QUERY_INSIDE_PASSES and captures the depth clear too.
            let region = crate::gpu_timers::SHADOW_CASCADE_REGIONS.get(c).copied();
            if let Some(r) = region {
                timers.cpu_begin(r);
                timers.begin(encoder, r);
            }
            let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_shadow_cascade"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &target.layer_views[c],
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

            // Group 0 (pass UBO, this cascade's light-VP) shares one layout across every
            // shadow pipeline and is bound first, so a later pipeline switch never
            // invalidates it — set it once per cascade instead of per draw.
            let pass_ubo_off = (c as u64 * self.shadow_pass_ubo.stride) as u32;
            rp.set_bind_group(0, pass_bind, &[pass_ubo_off]);

            // CPU casters for this cascade (the retained GPU set is drawn below; a section
            // owned by the GPU is suppressed CPU-side in AddShadowCaster, so no double-draw).
            if let (Some((pipes, caster_bind)), Some(conform_bind), Some(plan)) =
                (cpu, self.conform.bind.as_ref(), self.shadow_plan.get(c))
            {
                for bucket in plan {
                    let caster = &casters[bucket.repr as usize];
                    let Some(mesh) = self.meshes.get(KeyData::from_ffi(caster.mesh).into()) else {
                        continue;
                    };
                    let alpha = caster.alpha_ref > 0.0;
                    // Baked casters route through the rigid pipeline reading skinned_vbuf at
                    // base_vertex (identity world in the SSBO); `skin` = the VS-skinning fallback.
                    let baked = if caster.palette_slot != NO_PALETTE {
                        self.baked_draw_base(caster.palette_slot, caster.mesh)
                    } else {
                        None
                    };
                    let skin = if caster.palette_slot != NO_PALETTE && baked.is_none() {
                        mesh.skin.as_ref()
                    } else {
                        None
                    };
                    rp.set_pipeline(pipes.get(skin.is_some(), alpha));
                    if let Some(skin) = skin {
                        let Some(palette_bind) = self.palette.bind.as_ref() else {
                            continue;
                        };
                        rp.set_bind_group(
                            1,
                            palette_bind,
                            &[(caster.palette_slot as u64 * self.palette.stride) as u32],
                        );
                        rp.set_vertex_buffer(1, skin.slice(..));
                    } else {
                        // Whole-buffer storage bound once; each instance's slot travels as
                        // base_instance (read via @builtin(instance_index)) — no dynamic offset.
                        rp.set_bind_group(1, caster_bind, &[]);
                    }
                    rp.set_bind_group(
                        2,
                        textures.texture_bind(if alpha { caster.texture_id } else { 0 }),
                        &[],
                    );
                    rp.set_bind_group(3, textures.sampler_bind(caster.sampler.index()), &[]);
                    rp.set_bind_group(4, conform_bind, &[]);
                    // Baked casters pull baked verts from the shared skinned buffer at the
                    // baked slice offset; rigid/VS-skinned pull from the geometry pool at the
                    // mesh's vbase. Sliced to that byte offset with base_vertex 0 (as in
                    // draw_one); the index buffer is always the pool's Uint32 ibuf, its range
                    // offset by the mesh's ibase.
                    let (vbuf, vert_off) = match baked {
                        Some(bv) if self.skinned_vbuf.is_some() => (
                            self.skinned_vbuf.as_ref().unwrap(),
                            bv as u64 * BAKED_VERT_SIZE,
                        ),
                        _ => (self.pool.vbuf(), mesh.alloc.vbase as u64 * BAKED_VERT_SIZE),
                    };
                    rp.set_vertex_buffer(0, vbuf.slice(vert_off..));
                    rp.set_index_buffer(self.pool.ibuf().slice(..), wgpu::IndexFormat::Uint32);
                    let first = mesh.alloc.ibase + caster.index_begin;
                    rp.draw_indexed(
                        first..(first + caster.index_count),
                        0,
                        bucket.base..(bucket.base + bucket.count),
                    );
                }
            }

            // GPU-driven retained set casts into this cascade (no-op when GPU-driven is off,
            // or when the cascade has no survivors). Drawn last into the same depth attachment.
            self.draw_gpu_driven_shadow(&mut rp, textures, pass_ubo_off, c);
            // GRASS SHADOWS ARE THE ENTIRE SHADOW COST, AND MOST OF IT IS SPENT OFF-SCREEN.
            //
            // draw_shadow has no cascade awareness: it submits the near-ring blade instances
            // into whichever pass it is handed, so with 4 cascades it runs 4 times. Measured on
            // perf_abel @1920x1080, grass Ultra vs Off, the four cascades cost 4.443 ms vs
            // 0.121 -- objects are 0.12 ms of it and grass is the rest. The cost is flat in
            // shadow-map resolution (2048 vs 512: no change) and flat in caster triangle count
            // (1.4k vs 89k: no change) but linear in cascade COUNT, which is what vertex-bound
            // instancing looks like: the blades are transformed for every cascade and then
            // clipped away by the ones whose depth range the near ring never reaches.
            //
            // WGR_GRASS_SHADOW_CASCADES caps how many cascades receive them. Default 4 keeps
            // today's behaviour exactly; the point of the knob is to measure what the far
            // cascades actually contribute before changing what anyone sees.
            if (grass_mask & (1u32 << c)) != 0 {
                grass.draw_shadow(&mut rp, pass_bind, pass_ubo_off, timers);
            }
            // The pass must CLOSE before the encoder-level end timestamp — writing it while `rp`
            // is alive is a validation error, not a slightly wrong number.
            drop(rp);
            if let Some(r) = region {
                timers.end(encoder, r);
                timers.cpu_end(r);
            }
        }
    }

    // Synchronous readback of one cascade layer, row 0 = top; returns the
    // resolution, or 0 when unavailable.
    pub fn shadow_map_read(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        layer: u32,
        out: &mut [f32],
    ) -> u32 {
        let Some(target) = self.shadow_target.as_ref() else {
            return 0;
        };
        if layer >= target.layers || out.len() < (target.res * target.res) as usize {
            return 0;
        }
        if read_depth_layer(device, queue, &target.tex, target.res, layer, out) {
            target.res
        } else {
            0
        }
    }

    // Render a caller-supplied triangle soup with the solid shadow depth
    // pipeline into a scratch map and read it back (row 0 = top). Validates the
    // GPU depth path against the ShadowMath::CpuRasterDepth CPU reference.
    #[allow(clippy::too_many_arguments)]
    pub fn shadow_depth_probe(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        textures: &SharedTextures,
        light_vp: &[f32; 16],
        verts_xyz: &[f32],
        res: u32,
        out: &mut [f32],
    ) -> bool {
        let vert_count = verts_xyz.len() / 3;
        if vert_count == 0 || !vert_count.is_multiple_of(3) || vert_count > u16::MAX as usize {
            return false;
        }
        if out.len() < (res * res) as usize {
            return false;
        }
        self.ensure_shadow_pipelines(device);
        let pipes = self.shadow_pipelines.as_ref().unwrap();

        let verts: Vec<WgrMeshVertex> = verts_xyz
            .chunks_exact(3)
            .map(|p| WgrMeshVertex {
                pos: glam::Vec3::new(p[0], p[1], p[2]),
                norm: glam::Vec3::ZERO,
                uv: glam::Vec2::ZERO,
                conform: 0,
                tangent: glam::Vec3::ZERO,
                binormal: glam::Vec3::ZERO,
                uv1: glam::Vec2::ZERO,
            })
            .collect();
        let indices: Vec<u16> = (0..vert_count as u16).collect();
        let vbuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_probe_vbuf"),
            contents: bytemuck::cast_slice(&verts),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let ibuf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_probe_ibuf"),
            contents: bytemuck::cast_slice(&indices),
            usage: wgpu::BufferUsages::INDEX,
        });

        self.shadow_pass_ubo.ensure(device, 1);
        let pass_entry = ShadowPassUbo {
            light_vp: *light_vp,
            cam_pos: [0.0; 4],
        };
        queue.write_buffer(
            self.shadow_pass_ubo.buf.as_ref().unwrap(),
            0,
            bytemuck::bytes_of(&pass_entry),
        );
        let identity = ShadowCasterGpu {
            world: {
                let mut m = [0.0f32; 16];
                m[0] = 1.0;
                m[5] = 1.0;
                m[10] = 1.0;
                m[15] = 1.0;
                m
            },
            conform0: [0.0; 4],
            conform2: [0.0; 4], // mode 0: probe triangle is rigid, no conform
        };
        self.shadow_caster_ssbo
            .ensure(device, std::mem::size_of::<ShadowCasterGpu>() as u64);
        let caster_buf = self.shadow_caster_ssbo.buf.as_ref().unwrap();
        queue.write_buffer(caster_buf, 0, bytemuck::bytes_of(&identity));
        let caster_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_probe_caster_bind"),
            layout: &self.shadow_caster_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: caster_buf.as_entire_binding(),
            }],
        });

        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_probe_depth"),
            size: wgpu::Extent3d {
                width: res,
                height: res,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: SHADOW_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());

        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("wgr_probe"),
        });
        {
            let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_probe_pass"),
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
            rp.set_pipeline(&pipes.solid);
            rp.set_bind_group(0, self.shadow_pass_ubo.bind.as_ref().unwrap(), &[0]);
            rp.set_bind_group(1, &caster_bind, &[]);
            rp.set_bind_group(2, textures.texture_bind(0), &[]);
            rp.set_bind_group(3, textures.sampler_bind(0), &[]);
            rp.set_bind_group(4, self.conform.bind.as_ref().unwrap(), &[]);
            rp.set_vertex_buffer(0, vbuf.slice(..));
            rp.set_index_buffer(ibuf.slice(..), wgpu::IndexFormat::Uint16);
            rp.draw_indexed(0..vert_count as u32, 0, 0..1);
        }
        queue.submit(std::iter::once(encoder.finish()));

        read_depth_layer(device, queue, &tex, res, 0, out)
    }

    // Upload cameras, per-draw world matrices, and the skinned-draw bone palette;
    // regrow the dynamic UBOs. `palette` is a flat pool of PALETTE_SIZE-matrix
    // blocks, one per palette slot (world already pre-multiplied in on the C++ side).
    #[allow(clippy::too_many_arguments)]
    // DZ-003 -- adopt Sky's equirect reflection env map for the object paths' group(1),
    // mirroring Water::set_env_view. Returns whether the view actually changed, so the
    // caller rebuilds the bind groups that borrow it. The env texture is created once and
    // never resized, so `view_gen` is effectively constant and this flips exactly once.
    fn set_env_view(&mut self, env: &wgpu::TextureView, view_gen: u64) -> bool {
        if self.env_gen == view_gen {
            return false;
        }
        self.env_view = env.clone();
        self.env_gen = view_gen;
        true
    }

    pub fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        // Resolves each draw's texture handle to its bindless slot for packing into the
        // per-instance material (the fragment shader indexes the bindless arrays with it).
        textures: &SharedTextures,
        cameras: &[WgrCamera],
        draws: &[WgrDraw3D],
        // Slot -> draws index, the instancing plan's upload order (see plan_3d): a
        // bucket's instances occupy contiguous slots so one instanced draw covers them.
        // The per-draw storage arrays (world, material) are packed in this order and
        // read in-shader by @builtin(instance_index) == base_instance == slot.
        order: &[u32],
        palette: &[WgrMat4],
        lights: &[WgrLight],
        shadow_mask_view: &wgpu::TextureView,
        shadow_mask_gen: u64,
        shadow_mapping: &crate::terrain::TerrainShadowMap,
        heightmap_view: &wgpu::TextureView,
        heightmap_gen: u64,
        conform_params: &crate::terrain::TerrainConformParams,
        froxel_view: &wgpu::TextureView,
        sky_sh_buf: &wgpu::Buffer,
        skyvis_view: &wgpu::TextureView,
        // CLD-020 cloud sun-transmittance map, owned by the Sky pass.
        cloud_shadow_view: &wgpu::TextureView,
        // DZ-003 -- Sky's equirect reflection env map (linear radiance), the same view
        // water already reflects. Lent, not owned; bound into both object paths' group(1).
        env_view: &wgpu::TextureView,
        foliage: &crate::ffi::WgrFoliage,
        // (camera index, sea level). Only the reflected camera uses this conservative
        // above-water clip; main cameras retain their existing behaviour.
        reflection_clip: Option<(usize, f32)>,
        // The camera GTAO is computed for. Only this camera may READ the AO buffer — see the
        // per-camera gate in the upload loop below.
        gtao_camera: usize,
    ) {
        // Lend the terrain heightmap + its sampling params to the mesh conform group
        // (group 4) so vs_main can conform ClipLand vegetation to SurfaceY per vertex.
        self.conform
            .ensure(device, queue, heightmap_view, heightmap_gen, conform_params);
        // DZ-003: take Sky's reflection env map. The env texture is created once and never
        // resized, so gen 0 binds it exactly once and this is a no-op every frame after.
        // Both the per-draw group-1 binds (below) and the GPU-driven group-1 binds
        // (rebuild_gpu_group1) borrow it, so a change forces both to rebuild.
        let env_changed = self.set_env_view(env_view, 0);
        if env_changed {
            self.gpu_group1_bind = None;
        }
        // Frame-global lights into the group-0 storage buffer (shared with
        // terrain via the camera bind group). The per-camera count is in cam_pos.w.
        self.cameras.upload_lights(queue, lights);
        // Terrain sun-shadow world->UV mapping for the lit-mesh sampler (group 0).
        self.cameras.upload_mapping(queue, shadow_mapping);
        if !cameras.is_empty() {
            // Interior sky-visibility map + this frame's snapped ortho view, BEFORE the camera
            // bind group is built (it binds the map) and before the per-camera upload (it writes
            // the view's matrix). Keyed on the main scene camera — the map is a world-space
            // structure, so every camera in the frame reads the same one correctly.
            let main = cameras.get(gtao_camera).unwrap_or(&cameras[0]);
            let sky_cam = glam::Vec3::new(main.cam_pos[0], main.cam_pos[1], main.cam_pos[2]);
            self.prepare_interior_sky(device, sky_cam);
            // Bind the current shadow map (or the dummy while none exists); the
            // depth passes for this frame were prepared before this call, so the
            // target is final.
            let shadow_view = self
                .shadow_target
                .as_ref()
                .map(|t| &t.sample_view)
                .unwrap_or(&self.dummy_shadow_view);
            let interior_sky_view = self
                .interior_sky_target
                .as_ref()
                .map(|(_, _, v)| v)
                .unwrap_or(&self.dummy_interior_sky_view);
            self.cameras.ensure(
                device,
                cameras.len(),
                shadow_view,
                self.shadow_gen,
                shadow_mask_view,
                shadow_mask_gen,
                froxel_view,
                sky_sh_buf,
                skyvis_view,
                self.gtao.ao_view().unwrap_or(&self.dummy_ao_view),
                self.depth_gen,
                interior_sky_view,
                self.interior_sky_gen,
                cloud_shadow_view,
            );
            let buf = self.cameras.buf.as_ref().unwrap();
            for (i, c) in cameras.iter().enumerate() {
                let base = i as u64 * self.cameras.stride;
                queue.write_buffer(buf, base, bytemuck::bytes_of(c));
                // Append inv(proj·view) for depth→world unprojection (Frame.inv_view_proj).
                // Invert view and proj SEPARATELY in f64: the reversed-Z/infinite-far proj has
                // an ill-conditioned z-row and inverting the combined f32 matrix smears that
                // into x/y (same fix the sky uses, lib.rs). The view already has its translation
                // zeroed (geometry is camera-relative), so this maps NDC → camera-relative world.
                let view = glam::DMat4::from_cols_array(&c.view.map(f64::from));
                let proj = glam::DMat4::from_cols_array(&c.proj.map(f64::from));
                let inv_vp = (view.inverse() * proj.inverse()).as_mat4().to_cols_array();
                queue.write_buffer(
                    buf,
                    base + std::mem::size_of::<WgrCamera>() as u64,
                    bytemuck::cast_slice(&inv_vp),
                );
                // GTAO's projection terms. It reconstructs VIEW-space positions to match the
                // view-space normals the prepass wrote (mixing the two spaces turns whole faces
                // solid black — see gtao.wgsl), and does it from linear z, so it needs only the
                // two scale terms plus the near plane rather than an inverted matrix.
                //
                // near comes out of the projection's z column: this is a forward, infinite-far
                // projection, so proj[14] = -near.
                if self.cam_gtao_proj.len() <= i {
                    self.cam_gtao_proj.resize(i + 1, [1.0, 1.0, 0.1]);
                }
                self.cam_gtao_proj[i] = [c.proj[0], c.proj[5], -c.proj[14]];
                // Foliage knobs (frame.foliage / frame.foliageb) after inv_view_proj — same
                // append pattern; 32 B, matching the +32 in CameraGroup::new's bind_size.
                queue.write_buffer(
                    buf,
                    base + std::mem::size_of::<WgrCamera>() as u64 + 64,
                    bytemuck::bytes_of(foliage),
                );
                let clip = match reflection_clip {
                    Some((reflected_index, sea)) if i == reflected_index => [0.0, 1.0, 0.0, -sea],
                    _ => [0.0; 4],
                };
                queue.write_buffer(
                    buf,
                    base + std::mem::size_of::<WgrCamera>() as u64
                        + 64
                        + std::mem::size_of::<crate::ffi::WgrFoliage>() as u64,
                    bytemuck::cast_slice(&clip),
                );
                // GTAO gate + debug (frame.gtao). The gate is here rather than left implicit in
                // the AO texture because that texture keeps its last contents when the pass is
                // skipped — an ungated consumer would shade with a frozen AO buffer, which is far
                // harder to recognise than no AO at all.
                //
                // Gated PER CAMERA, not just per frame. GTAO is computed once, from the main
                // scene camera's depth buffer. Any other camera in the frame — the first-person
                // weapon segment (its own near/far, drawn after a depth clear, with no prepass),
                // cockpit/optics views, the planar reflection — covers the same pixels with
                // DIFFERENT geometry, so sampling by screen position there reads the AO of
                // whatever the main camera had behind it. In the debug view that makes the
                // weapon vanish into the world behind it; in the lit path it is a quieter wrong
                // ambient. Neither has a valid AO value available, so they get 1.0.
                let g = &self.gtao_settings;
                // The frame-UBO gate must follow the same force-off as render_gtao, or the shader
                // would keep sampling the AO target the pass no longer refreshes.
                let on = g.enabled && !self.gtao_force_off && i == gtao_camera;
                let gtao = [
                    if on { 1.0f32 } else { 0.0 },
                    if on { g.debug_mode.min(2) as f32 } else { 0.0 },
                    if on && g.bent_normal { 1.0 } else { 0.0 },
                    // NOT gated on `on`. The other three lanes describe a screen-space buffer
                    // that only the GTAO camera may read; this one is how much of the normal
                    // map's own deviation the ambient direction keeps, and that is true of
                    // every camera in the frame whether GTAO ran for it or not. Gating it here
                    // would leave the weapon segment and the planar reflection shading their
                    // normal-mapped surfaces flat while the main view did not.
                    ambient_normal_mapped(),
                ];
                queue.write_buffer(
                    buf,
                    base + std::mem::size_of::<WgrCamera>() as u64
                        + 64
                        + std::mem::size_of::<crate::ffi::WgrFoliage>() as u64
                        + 16,
                    bytemuck::cast_slice(&gtao),
                );
                // Interior sky visibility: every layer's ABSOLUTE-space ortho VP, the matching
                // sky directions, and the knobs.
                //
                // Not gated per camera, unlike GTAO immediately above, and the difference is
                // worth stating: GTAO is a SCREEN-space buffer, so it is only valid for the
                // camera that produced it. This map is a WORLD-space structure — the weapon
                // segment, a cockpit view and the planar reflection all sample it at their own
                // world positions and all get the right answer.
                let sv = &self.interior_sky;
                let on = self.interior_sky_active();
                let dirs = sky_vis::directions();
                let mut vp = [0.0f32; 16 * sky_vis::DIRECTION_COUNT];
                let mut dir = [0.0f32; 4 * sky_vis::DIRECTION_COUNT];
                for i in 0..sky_vis::DIRECTION_COUNT {
                    if on {
                        vp[i * 16..(i + 1) * 16]
                            .copy_from_slice(&self.interior_sky_views[i].view_proj.to_cols_array());
                    }
                    dir[i * 4..(i + 1) * 4].copy_from_slice(&dirs[i].to_array());
                }
                let (kernel_uv, bias_ndc) = if on {
                    (
                        self.interior_sky_views[0].kernel_uv,
                        self.interior_sky_views[0].bias_ndc,
                    )
                } else {
                    (0.0, 0.0)
                };
                let knobs = [
                    if on { 1.0f32 } else { 0.0 },
                    if on && sv.debug { 1.0 } else { 0.0 },
                    sv.strength,
                    sv.floor,
                ];
                let knobs_b = [kernel_uv, bias_ndc, sv.directional, 0.0];
                let sky_base = base
                    + std::mem::size_of::<WgrCamera>() as u64
                    + 64
                    + std::mem::size_of::<crate::ffi::WgrFoliage>() as u64
                    + 16
                    + 16;
                let dir_off = sky_base + 64 * sky_vis::DIRECTION_COUNT as u64;
                let knob_off = dir_off + 16 * sky_vis::DIRECTION_COUNT as u64;
                queue.write_buffer(buf, sky_base, bytemuck::cast_slice(&vp));
                queue.write_buffer(buf, dir_off, bytemuck::cast_slice(&dir));
                queue.write_buffer(buf, knob_off, bytemuck::cast_slice(&knobs));
                queue.write_buffer(buf, knob_off + 16, bytemuck::cast_slice(&knobs_b));
                // Baked volumes (Stage 2): gated on the runtime switch AND on a volume actually
                // existing, so the knob cannot darken anything before a bake has run.
                let baked_on = sv.baked && !self.sky_volumes.is_empty();
                let knobs_c = [
                    if baked_on { 1.0f32 } else { 0.0 },
                    sv.strength,
                    sv.floor,
                    if baked_on && sv.debug { 1.0 } else { 0.0 },
                ];
                queue.write_buffer(buf, knob_off + 32, bytemuck::cast_slice(&knobs_c));
                // Material Debug (frame.wgsl `matdbg`). Written unconditionally and derived
                // from bind_size rather than from the sky offsets above, which live inside a
                // conditional: the retained shader reads this every frame and a stale lane
                // would leave a debug view stuck on.
                let matdbg = [
                    self.material_debug_view as f32,
                    self.material_debug_flags as f32,
                    0.0f32,
                    0.0f32,
                ];
                queue.write_buffer(
                    buf,
                    base + self.cameras.bind_size - 80,
                    bytemuck::cast_slice(&matdbg),
                );
                // DZ-005 water surface animation (frame.wgsl `wateranim`).
                // Same reasoning as matdbg: unconditional, so a world that stops publishing
                // water params cannot leave the ripple frozen at a stale time.
                queue.write_buffer(
                    buf,
                    base + self.cameras.bind_size - 64,
                    bytemuck::cast_slice(&self.water_anim),
                );
                // Normal-map relief under ambient light (frame.wgsl `relief`), the LAST lane.
                //
                // Written for EVERY camera and never gated, exactly like the ambient normal-
                // mapping weight it works with: these describe how a SURFACE takes sky light,
                // not how a screen-space buffer may be read, so the weapon segment, a cockpit
                // view and the planar reflection must all shade their normal-mapped surfaces the
                // same way the main view does. Gating a surface property on the GTAO camera is
                // how you get a rifle whose receiver is flat while the wall behind it is not.
                let relief = [normal_cavity(), sky_specular(), 0.0f32, 0.0f32];
                queue.write_buffer(
                    buf,
                    base + self.cameras.bind_size - 48,
                    bytemuck::cast_slice(&relief),
                );
                // Far-fog closure (frame.wgsl `fogfar`). Unconditional for the same reason as
                // the three above: a camera that skipped it would read whatever the previous
                // frame left there.
                queue.write_buffer(
                    buf,
                    base + self.cameras.bind_size - 32,
                    bytemuck::cast_slice(&self.fog_far),
                );
                // REN-TEMP-001D render-scale texture LOD (frame.wgsl `renscale`), the LAST
                // lane. Unconditional like the four above, and for one more reason: the lane
                // must read 0/0 the moment the render scale returns to native, not whenever
                // the next camera happens to rewrite it.
                queue.write_buffer(
                    buf,
                    base + self.cameras.bind_size - 16,
                    bytemuck::cast_slice(&self.render_scale_lane),
                );
            }
        }
        // Track buffer regrowth so the combined group-1 bind groups (which borrow
        // these buffers) are rebuilt only when one actually moved.
        let mut world_grew = false;
        let mut palette_grew = false;
        let mut material_grew = false;
        if !order.is_empty() {
            // Pack the whole frame's world matrices + materials contiguously, one upload each,
            // indexed in-shader by instance_index. The pack order is the instancing plan's slot
            // order (not raw draw order): a bucket's instances land in a contiguous slot range so
            // one draw covers them all.
            //
            // write_buffer_with hands us a WRITE-ONLY view of wgpu's staging memory (it may be
            // write-combined, so reads are disallowed). We build each struct straight into that view
            // via into_chunks + write_iter — no intermediate scratch Vec and no second memcpy, which
            // is what plain write_buffer would cost (build a Vec, then memcpy it into staging).
            // obj_bytes/mat_bytes are exact multiples of the element size, so the chunk remainder is
            // always empty.
            let n = order.len();
            // A baked skinned draw routes through the rigid pipeline: its world is folded
            // into the palette (baked position is camera-relative world space already), so
            // pack an identity world + no conform so vs_main leaves the baked verts alone.
            let bake_on = self.skin_bake_enabled;
            let base_map = &self.skin_base_vertex;
            const OBJ_SZ: usize = std::mem::size_of::<ObjectGpu>();
            let obj_bytes = (n * OBJ_SZ) as u64;
            world_grew = self.world.ensure(device, obj_bytes);
            if let Some(sz) = wgpu::BufferSize::new(obj_bytes) {
                // `wgpu::Buffer` is a refcounted handle; cloning it (once per frame) releases
                // the borrow of `self.world` so the failure arm can invalidate it in place.
                let dst = self.world.buf.clone();
                let staged = dst.as_ref().and_then(|b| queue.write_buffer_with(b, 0, sz));
                match staged {
                    Some(mut view) => {
                        let (chunks, _rem) = view.slice(..).into_chunks::<OBJ_SZ>();
                        chunks.write_iter(order.iter().map(|&i| {
                            let d = &draws[i as usize];
                            // REN-TEMP-002: identity world for bake-covered draws —
                            // the whole WHY lives on object_gpu_for_draw.
                            let obj = object_gpu_for_draw(d, bake_on, base_map);
                            bytemuck::cast::<ObjectGpu, [u8; OBJ_SZ]>(obj)
                        }));
                    }
                    // `write_buffer_with` returns None when wgpu refuses the write: either the
                    // destination came back invalid from an out-of-memory `create_buffer`, or the
                    // staging allocation itself was refused. Both are OOM, and neither is worth
                    // unwinding the render thread for. Degrade: drop the (possibly poisoned)
                    // buffer so the next frame builds a fresh one, and clear the group-1 binds so
                    // this frame's direct-path objects are SKIPPED rather than drawn against
                    // whatever transforms the buffer happens to hold (draw_one already returns
                    // early on an absent bind).
                    None => {
                        if staging_strict() {
                            panic!("world staging view");
                        }
                        self.world.invalidate();
                        self.group1_plain_bind = None;
                        self.note_staging_refusal("world", obj_bytes, n);
                    }
                }
            }

            const MAT_SZ: usize = std::mem::size_of::<MaterialUbo>();
            let mat_bytes = (n * MAT_SZ) as u64;
            material_grew = self.material.ensure(device, mat_bytes);
            if let Some(sz) = wgpu::BufferSize::new(mat_bytes) {
                let dst = self.material.buf.clone();
                let staged = dst.as_ref().and_then(|b| queue.write_buffer_with(b, 0, sz));
                if staged.is_none() {
                    // Same OOM degrade as the world buffer above. The material array backs BOTH
                    // group-1 binds, so both go, and both come back on the next frame's `ensure`
                    // (the rebuild block below is itself guarded on the buffers being present, so
                    // it will not resurrect a bind over a dropped buffer).
                    if staging_strict() {
                        panic!("material staging view");
                    }
                    self.material.invalidate();
                    self.group1_plain_bind = None;
                    self.group1_skinned_bind = None;
                    self.note_staging_refusal("material", mat_bytes, n);
                }
                if let Some(mut view) = staged {
                    let (chunks, _rem) = view.slice(..).into_chunks::<MAT_SZ>();
                    chunks.write_iter(order.iter().map(|&i| {
                        let d = &draws[i as usize];
                        // Pack the bindless indices into the material's spare emissive.w
                        // (only emissive.rgb is read for shading): (tex_slot << 3) | sampler.
                        // The fragment shader unpacks it to index the bindless texture +
                        // sampler arrays, so texture/sampler need no per-draw bind and drop
                        // out of the instancing key (plan_3d).
                        let slot = textures.texture_slot(d.texture_id);
                        let packed = (slot << 3) | (d.sampler.index() as u32 & 0x7);
                        let mut emissive = d.mat_emissive;
                        emissive[3] = f32::from_bits(packed);
                        let normal_slot = textures.texture_slot(d.normal_texture_id);
                        let normal_packed = (normal_slot << 3) | (d.sampler.index() as u32 & 0x7);
                        bytemuck::cast::<MaterialUbo, [u8; MAT_SZ]>(MaterialUbo {
                            emissive,
                            sun_ambient: d.mat_sun_ambient,
                            sun_diffuse: d.mat_sun_diffuse,
                            light_diffuse: d.mat_light_diffuse,
                            light_ambient: d.mat_light_ambient,
                            specular: d.mat_specular,
                            // w is a small BITFIELD, not a bool: bit 0 = invert the normal
                            // map's Y (the material-debug toggle it already carried), bit 1 =
                            // DZ-003 reflective (the material named an EnvironmentMap), bit 2 =
                            // night emitter (a fixture whose emissive survives the night). Kept
                            // in this lane because WgrDraw3D is ABI-frozen and MaterialUbo's
                            // seven vec4s are otherwise fully spoken for.
                            normal_map: [
                                f32::from_bits(normal_packed),
                                (d.misc & 0xff) as f32,
                                if d.normal_texture_id != 0 { 1.0 } else { 0.0 },
                                (u32::from(d.misc & 0x100 != 0)
                                    | (u32::from(d.flags & crate::ffi::WGR_DRAW3D_REFLECTIVE != 0)
                                        << 1)
                                    | (u32::from(
                                        d.flags & crate::ffi::WGR_DRAW3D_NIGHT_EMITTER != 0,
                                    ) << 2)
                                    | (u32::from(d.flags & crate::ffi::WGR_DRAW3D_GLASS != 0)
                                        << 3)
                                    | (u32::from(d.flags & crate::ffi::WGR_DRAW3D_COCKPIT != 0)
                                        << 4)) as f32,
                            ],
                        })
                    }));
                }
            }
        }
        // One dynamic-UBO slot per PALETTE_SIZE-matrix block. A block is exactly
        // the UBO bind size, so slot s lives at s * stride. Uploaded regardless of the
        // compute bake: since 2026-08-30 the draws ALWAYS VS-skin (the bake feeds only
        // the velocity pass), so this UBO is live whenever skinned draws exist.
        let slots = palette.len() / PALETTE_SIZE;
        if slots > 0 {
            palette_grew = self.palette.ensure(device, slots);
            let buf = self.palette.buf.as_ref().unwrap();
            for s in 0..slots {
                let block = &palette[s * PALETTE_SIZE..(s + 1) * PALETTE_SIZE];
                queue.write_buffer(
                    buf,
                    s as u64 * self.palette.stride,
                    bytemuck::cast_slice(block),
                );
            }
        }

        // (Re)build the combined group-1 bind groups when a backing buffer moved
        // (or on first use). The plain bind needs the world + material buffers;
        // the skinned bind the palette + material buffers.
        // AST-012A: binding 4 of both binds. The buffer is allocated by ensure_mip_feedback,
        // which the renderer calls before this every frame — including when the instrument is
        // switched off, at one word, precisely so it is never None here. If it somehow were,
        // the binds are simply left as they are rather than rebuilt: an object draw with last
        // frame's bind is recoverable, an object draw with no bind at all is not.
        let mipfb_moved = self.mip_feedback_grew;
        let mipfb = self.mip_feedback.buffer().cloned();
        if let (Some(mipfb), Some(world_buf), Some(mat_buf)) = (
            mipfb.as_ref(),
            self.world.buf.as_ref(),
            self.material.buf.as_ref(),
        ) {
            if world_grew || material_grew || env_changed || mipfb_moved || self.group1_plain_bind.is_none() {
                self.group1_plain_bind = Some(build_group1_bind(
                    device,
                    &self.group1_plain_layout,
                    world_buf,
                    None,
                    mat_buf,
                    &self.env_view,
                    &self.env_sampler,
                    mipfb,
                    "wgr_3d_group1_plain_bind",
                ));
            }
        }
        if let (Some(mipfb), Some(pal_buf), Some(mat_buf)) = (
            mipfb.as_ref(),
            self.palette.buf.as_ref(),
            self.material.buf.as_ref(),
        ) {
            if palette_grew || material_grew || env_changed || mipfb_moved || self.group1_skinned_bind.is_none() {
                self.group1_skinned_bind = Some(build_group1_bind(
                    device,
                    &self.group1_skinned_layout,
                    pal_buf,
                    Some(self.palette.bind_size),
                    mat_buf,
                    &self.env_view,
                    &self.env_sampler,
                    mipfb,
                    "wgr_3d_group1_skinned_bind",
                ));
            }
        }

        // Build any pipeline variants this frame's draws need before the render
        // pass records them (pipeline creation needs &mut self; draw_one is &self).
        for d in draws {
            let has_skin = self
                .meshes
                .get(KeyData::from_ffi(d.mesh).into())
                .is_some_and(|m| m.skin.is_some());
            // A baked draw draws through the RIGID pipeline (identity world + baked verts),
            // so build the plain variant for it, not the skinned one.
            let baked = self.baked_draw_base(d.palette_slot, d.mesh).is_some() && has_skin;
            let skinned = d.palette_slot != NO_PALETTE && has_skin && !baked;
            let key = PipelineKey::from_draw(d, skinned);
            self.ensure_pipeline(device, key);
            // Opaque draws are prepassed: also build their colour write-off variant (used
            // in the prepassed segment) and their depth+normal prepass pipeline.
            if key.prepassed() {
                self.ensure_pipeline(device, key.with_write_off());
                self.ensure_prepass_pipeline(
                    device,
                    PrepassKey {
                        skinned,
                        alpha_ref_bits: key.alpha_ref_bits,
                    },
                );
            }
        }
    }

    // Build the frame's instancing plan from the command stream. Consecutive
    // "standard opaque" 3D draws (blend Opaque, no polygon offset, depth test+write,
    // not skinned) are order-independent, so within a maximal run of them we bucket by
    // (mesh, section, texture, sampler, camera, pipeline): every bucket collapses to
    // one instanced draw whose instances read their own world/conform/material from the
    // per-draw storage arrays via @builtin(instance_index). The upload order (`order`)
    // lays each bucket's instances in a contiguous slot range so base_instance covers
    // them. Anything not instanceable (transparent, decal/ZBias, skinned, non-standard
    // depth) is a barrier: it flushes the run and is emitted as a count-1 draw in place,
    // preserving draw order across it. Terrain / 2D / ClearDepth also flush and pass
    // through, so the plan mirrors the stream's ordering exactly.
    pub fn plan_3d(&self, cmds: &[WgrCmd], draws: &[WgrDraw3D]) -> Plan3d {
        // slot -> draws index (the storage-array pack order).
        let mut order: Vec<u32> = Vec::with_capacity(cmds.len());
        let mut ops: Vec<Plan3dOp> = Vec::with_capacity(cmds.len());
        // The current reorderable run: buckets in first-seen order + their members
        // (draws indices), and a key->bucket map to coalesce.
        let mut buckets: Vec<(u32, Vec<u32>)> = Vec::new();
        let mut bucket_index: FxHashMap<BucketKey, usize> = FxHashMap::default();

        // Emit the current run's buckets (each becomes one instanced draw over a
        // contiguous slot range) and reset it. Called at every barrier.
        fn flush_run(
            order: &mut Vec<u32>,
            ops: &mut Vec<Plan3dOp>,
            buckets: &mut Vec<(u32, Vec<u32>)>,
            bucket_index: &mut FxHashMap<BucketKey, usize>,
        ) {
            for (repr, members) in buckets.drain(..) {
                let base = order.len() as u32;
                let count = members.len() as u32;
                order.extend_from_slice(&members);
                ops.push(Plan3dOp::Draw3D {
                    draw: repr,
                    base,
                    count,
                    // Every bucket is instanceable opaque-rigid (the only path that
                    // buckets), so it is a candidate for the indirect draw path.
                    kind: DrawKind::IndirectEligible,
                });
            }
            bucket_index.clear();
        }

        for cmd in cmds {
            if cmd.kind == WgrCmdKind::Draw3D as u32 {
                let Some(d) = draws.get(cmd.arg as usize) else {
                    continue;
                };
                // Drops draws that render nothing (missing/invalid mesh, empty range),
                // exactly as draw_one would bail on them — they never reach the GPU, so
                // omitting them from the plan (and their storage slot) is equivalent.
                if d.index_count == 0 {
                    continue;
                }
                let Some(mesh) = self.meshes.get(KeyData::from_ffi(d.mesh).into()) else {
                    continue;
                };
                if d.index_begin + d.index_count > mesh.index_count {
                    continue;
                }
                let skinned = d.palette_slot != NO_PALETTE && mesh.skin.is_some();
                let pkey = PipelineKey::from_draw(d, false);
                let instanceable = !skinned
                    && d.blend == WgrBlend::Opaque
                    && pkey.offset == Offset::None
                    && d.depth == WgrDepthMode::TestWrite;
                if instanceable {
                    let key = BucketKey {
                        mesh: d.mesh,
                        index_begin: d.index_begin,
                        index_count: d.index_count,
                        camera: d.camera,
                        pipeline: pkey,
                    };
                    let bi = *bucket_index.entry(key).or_insert_with(|| {
                        buckets.push((cmd.arg, Vec::new()));
                        buckets.len() - 1
                    });
                    buckets[bi].1.push(cmd.arg);
                } else {
                    // Barrier draw: keep its position by flushing the run first, then
                    // emit it standalone (its own slot, count 1).
                    flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
                    let base = order.len() as u32;
                    order.push(cmd.arg);
                    ops.push(Plan3dOp::Draw3D {
                        draw: cmd.arg,
                        base,
                        count: 1,
                        // Barrier draw (transparent / decal / skinned / non-standard
                        // depth): always submitted directly via draw_one.
                        kind: DrawKind::Direct,
                    });
                }
            } else if cmd.kind == WgrCmdKind::Draw2D as u32 {
                flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
                ops.push(Plan3dOp::Draw2D(cmd.arg));
            } else if cmd.kind == WgrCmdKind::DrawTerrain as u32 {
                flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
                ops.push(Plan3dOp::Terrain(cmd.arg));
            } else if cmd.kind == WgrCmdKind::DrawWater as u32 {
                flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
                ops.push(Plan3dOp::Water(cmd.arg));
            } else if cmd.kind == WgrCmdKind::DrawGrass as u32 {
                flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
                ops.push(Plan3dOp::Grass(cmd.arg));
            } else if cmd.kind == WgrCmdKind::ClearDepth as u32 {
                flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
                ops.push(Plan3dOp::ClearDepth);
            } else if cmd.kind == WgrCmdKind::Resolve as u32 {
                flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
                ops.push(Plan3dOp::Resolve);
            }
        }
        flush_run(&mut order, &mut ops, &mut buckets, &mut bucket_index);
        Plan3d { order, ops }
    }

    // Build this frame's indirect draw args from the instancing plan (docs/
    // gpu-culling-and-depth-plan.md Stage 2). For each instanceable opaque-rigid bucket
    // (DrawKind::IndirectEligible) it writes one DrawIndexedIndirectArgs addressing the
    // shared pool — base_vertex = the mesh's vbase, first_index = its ibase + the section
    // start, first_instance = the bucket's base_instance — and upgrades the op to
    // Indirect(byte_offset). No-op (leaving buckets on the direct draw_one path) when
    // indirect is disabled. Mutates `ops` in place; call after plan_3d, before the replay.
    pub fn build_indirect(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        draws: &[WgrDraw3D],
        ops: &mut [Plan3dOp],
    ) {
        if !self.indirect_enabled {
            return;
        }
        let mut args: Vec<DrawIndexedIndirectArgs> = Vec::new();
        for op in ops.iter_mut() {
            let Plan3dOp::Draw3D {
                draw,
                base,
                count,
                kind,
            } = op
            else {
                continue;
            };
            if !matches!(kind, DrawKind::IndirectEligible) {
                continue;
            }
            let d = &draws[*draw as usize];
            // The bucket exists only because plan_3d validated the mesh + range; re-guard
            // (drop back to the direct path) rather than panic if the mesh is gone.
            let Some(mesh) = self.meshes.get(KeyData::from_ffi(d.mesh).into()) else {
                *kind = DrawKind::Direct;
                continue;
            };
            let offset = args.len() as u64 * INDIRECT_ARG_SIZE;
            args.push(DrawIndexedIndirectArgs {
                index_count: d.index_count,
                instance_count: *count,
                first_index: mesh.alloc.ibase + d.index_begin,
                base_vertex: mesh.alloc.vbase as i32,
                first_instance: *base,
            });
            *kind = DrawKind::Indirect(offset as u32);
        }
        if args.is_empty() {
            return;
        }
        let bytes = args.len() as u64 * INDIRECT_ARG_SIZE;
        self.ensure_indirect_args(device, bytes);
        queue.write_buffer(
            self.indirect_args.as_ref().unwrap(),
            0,
            bytemuck::cast_slice(&args),
        );
    }

    fn ensure_indirect_args(&mut self, device: &wgpu::Device, bytes: u64) {
        if self.indirect_args_cap >= bytes && self.indirect_args.is_some() {
            return;
        }
        let cap = bytes.next_power_of_two().max(4096);
        self.indirect_args = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_indirect_args"),
            size: cap,
            usage: wgpu::BufferUsages::INDIRECT
                | wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        self.indirect_args_cap = cap;
    }

    // Issue one (possibly instanced) indexed draw. `d` supplies the mesh, section,
    // texture, sampler, camera and pipeline shared by every instance; `base..base+count`
    // is the base_instance range, selecting each instance's world/conform/material slot
    // in the prepared storage arrays via @builtin(instance_index). `st` carries the
    // bind/pipeline/buffer state already set on `pass` so redundant re-binds are
    // skipped — three of the five bind groups (camera, world/material, conform) are
    // frame-constant, so within a run of 3D draws they are set once, not per draw.
    // The caller resets `st` (Pass3dState::default) whenever another pipeline runs on
    // the pass (terrain/2D) or a new pass begins, since that invalidates this state.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_one(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        d: &WgrDraw3D,
        base: u32,
        count: u32,
        st: &mut Pass3dState,
        mode: Pass3dMode,
    ) {
        if d.index_count == 0 {
            return;
        }
        let Some(camera_bind) = self.cameras.bind.as_ref() else {
            return;
        };
        let Some(conform_bind) = self.conform.bind.as_ref() else {
            return;
        };
        let Some(mesh) = self.meshes.get(KeyData::from_ffi(d.mesh).into()) else {
            return;
        };
        if d.index_begin + d.index_count > mesh.index_count {
            return;
        }
        // Compute skin bake (docs/compute-skin-bake-plan.md): a baked skinned draw routes
        // through the RIGID pipeline (identity world packed in prepare) reading the shared
        // baked vertex buffer at `base_vertex`, so it is NOT `skinned` for pipeline/bind
        // selection. Only the true VS-skinning fallback (WGR_SKIN_BAKE=0) sets `skinned`.
        let baked = if mesh.skin.is_some() {
            self.baked_draw_base(d.palette_slot, d.mesh)
        } else {
            None
        };
        if baked.is_some() && self.skinned_vbuf.is_none() {
            return;
        }
        let skinned = d.palette_slot != NO_PALETTE && mesh.skin.is_some() && baked.is_none();
        let base_key = PipelineKey::from_draw(d, skinned);
        // Pipeline by pass mode. Prepass draws ONLY the opaque set (self-filter here so
        // the caller can replay a whole segment); the colour pass flips the same set to
        // its write-off variant when the prepass already laid its depth (decision 2/4).
        let pipeline = match mode {
            Pass3dMode::Prepass => {
                if !base_key.prepassed() {
                    return;
                }
                let Some(p) = self.prepass_pipelines.get(&PrepassKey {
                    skinned,
                    alpha_ref_bits: base_key.alpha_ref_bits,
                }) else {
                    return;
                };
                p
            }
            Pass3dMode::Color { depth_write_off } => {
                let key = if depth_write_off && base_key.prepassed() {
                    base_key.with_write_off()
                } else {
                    base_key
                };
                let Some(p) = self.pipelines.get(&key) else {
                    return;
                };
                p
            }
        };
        // Bail (without touching `st`) if the group-1 backing the draw needs isn't ready.
        if skinned && self.group1_skinned_bind.is_none() {
            return;
        }
        if !skinned && self.group1_plain_bind.is_none() {
            return;
        }

        // A plain<->skinned switch changes the group-1 pipeline layout, which invalidates
        // bind groups 1..=4 in wgpu; drop their tracked state so they are re-bound.
        if st.last_skinned.is_some() && st.last_skinned != Some(skinned) {
            st.group1_plain = false;
            st.skinned_off = None;
            st.bindless = false;
            st.conform = false;
        }
        st.last_skinned = Some(skinned);

        let pipe_id = pipeline as *const wgpu::RenderPipeline as usize;
        if st.pipeline != Some(pipe_id) {
            pass.set_pipeline(pipeline);
            st.pipeline = Some(pipe_id);
        }

        // Group 0: camera UBO (dynamic offset). One camera for the whole 3D pass in
        // practice, so this binds once.
        let cam_off = (d.camera as u64 * self.cameras.stride) as u32;
        if st.cam_off != Some(cam_off) {
            pass.set_bind_group(0, camera_bind, &[cam_off]);
            st.cam_off = Some(cam_off);
        }

        // Group 1: plain = whole-buffer world/material (frame-constant, bind once);
        // skinned = bone palette at a per-draw dynamic offset.
        if skinned {
            let off = (d.palette_slot as u64 * self.palette.stride) as u32;
            if st.skinned_off != Some(off) {
                pass.set_bind_group(1, self.group1_skinned_bind.as_ref().unwrap(), &[off]);
                st.skinned_off = Some(off);
            }
            st.group1_plain = false;
            pass.set_vertex_buffer(1, mesh.skin.as_ref().unwrap().slice(..));
        } else if !st.group1_plain {
            pass.set_bind_group(1, self.group1_plain_bind.as_ref().unwrap(), &[]);
            st.group1_plain = true;
            st.skinned_off = None;
        }

        // Groups 2 (bindless object textures) + 3 (8-variant sampler array) are
        // frame-constant, bound once per run — the per-instance texture/sampler indices
        // ride the material array, so these are no longer per-draw.
        if !st.bindless {
            pass.set_bind_group(2, textures.bindless_bind(), &[]);
            pass.set_bind_group(3, textures.sampler_array_bind(), &[]);
            st.bindless = true;
        }

        // Group 4: conform heightmap (frame-constant, bind once).
        if !st.conform {
            pass.set_bind_group(4, conform_bind, &[]);
            st.conform = true;
        }

        // Vertex source at slot 0: baked draws pull from the shared skinned output buffer
        // at the baked slice offset; rigid/VS-skinned draws pull from the geometry pool
        // at the mesh's vbase. Either way the buffer is SLICED to that byte offset and
        // base_vertex is 0, so @builtin(vertex_index) stays mesh-local — attributes fetch
        // exactly as with the old per-mesh buffers, and a slot-1 skin buffer (VS-skinning
        // fallback) stays aligned at base_vertex 0. The index buffer is always the pool's
        // Uint32 ibuf (bound once per run); the draw range is offset by the mesh's ibase.
        let (vertex_buf, vert_off) = match baked {
            Some(bv) => (
                self.skinned_vbuf.as_ref().unwrap(),
                bv as u64 * BAKED_VERT_SIZE,
            ),
            None => (self.pool.vbuf(), mesh.alloc.vbase as u64 * BAKED_VERT_SIZE),
        };
        let vbuf_id = (vertex_buf as *const wgpu::Buffer as usize, vert_off);
        if st.vbuf != Some(vbuf_id) {
            pass.set_vertex_buffer(0, vertex_buf.slice(vert_off..));
            st.vbuf = Some(vbuf_id);
        }
        let ibuf = self.pool.ibuf();
        let ibuf_id = ibuf as *const wgpu::Buffer as usize;
        if st.ibuf != Some(ibuf_id) {
            pass.set_index_buffer(ibuf.slice(..), wgpu::IndexFormat::Uint32);
            st.ibuf = Some(ibuf_id);
        }

        let first = mesh.alloc.ibase + d.index_begin;
        pass.draw_indexed(first..(first + d.index_count), 0, base..(base + count));
    }

    // Submit one instanceable opaque-rigid bucket via the indirect args buffer (docs/
    // gpu-culling-and-depth-plan.md Stage 2). Mirrors draw_one's PLAIN path — the same
    // pipeline selection by pass mode plus the same frame-constant binds and Pass3dState
    // tracking, so it interleaves correctly with direct draw_one calls sharing that state
    // — but binds the pool buffers WHOLE (indirect can't slice per sub-draw; base_vertex /
    // first_index / first_instance all ride the args) and ends in draw_indexed_indirect.
    // `arg_offset` is the bucket's byte offset in `indirect_args` (DrawKind::Indirect).
    #[allow(clippy::too_many_arguments)]
    pub fn draw_indirect(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        d: &WgrDraw3D,
        arg_offset: u32,
        st: &mut Pass3dState,
        mode: Pass3dMode,
    ) {
        let (Some(camera_bind), Some(conform_bind), Some(args), Some(group1_plain)) = (
            self.cameras.bind.as_ref(),
            self.conform.bind.as_ref(),
            self.indirect_args.as_ref(),
            self.group1_plain_bind.as_ref(),
        ) else {
            return;
        };
        // Eligible buckets are always plain opaque-rigid (see plan_3d): never skinned,
        // never baked, so pipeline + bind selection is the plain, non-skinned path.
        let base_key = PipelineKey::from_draw(d, false);
        let pipeline = match mode {
            Pass3dMode::Prepass => {
                if !base_key.prepassed() {
                    return;
                }
                let Some(p) = self.prepass_pipelines.get(&PrepassKey {
                    skinned: false,
                    alpha_ref_bits: base_key.alpha_ref_bits,
                }) else {
                    return;
                };
                p
            }
            Pass3dMode::Color { depth_write_off } => {
                let key = if depth_write_off && base_key.prepassed() {
                    base_key.with_write_off()
                } else {
                    base_key
                };
                let Some(p) = self.pipelines.get(&key) else {
                    return;
                };
                p
            }
        };

        // A skinned->plain switch invalidated bind groups 1..=4 (different group-1 pipeline
        // layout); mirror draw_one so the shared Pass3dState stays coherent across paths.
        if st.last_skinned.is_some() && st.last_skinned != Some(false) {
            st.group1_plain = false;
            st.skinned_off = None;
            st.bindless = false;
            st.conform = false;
        }
        st.last_skinned = Some(false);

        let pipe_id = pipeline as *const wgpu::RenderPipeline as usize;
        if st.pipeline != Some(pipe_id) {
            pass.set_pipeline(pipeline);
            st.pipeline = Some(pipe_id);
        }

        let cam_off = (d.camera as u64 * self.cameras.stride) as u32;
        if st.cam_off != Some(cam_off) {
            pass.set_bind_group(0, camera_bind, &[cam_off]);
            st.cam_off = Some(cam_off);
        }
        if !st.group1_plain {
            pass.set_bind_group(1, group1_plain, &[]);
            st.group1_plain = true;
            st.skinned_off = None;
        }
        if !st.bindless {
            pass.set_bind_group(2, textures.bindless_bind(), &[]);
            pass.set_bind_group(3, textures.sampler_array_bind(), &[]);
            st.bindless = true;
        }
        if !st.conform {
            pass.set_bind_group(4, conform_bind, &[]);
            st.conform = true;
        }

        // Pool buffers bound WHOLE (offset 0): the args' base_vertex/first_index address
        // the mesh's slice, so slicing per sub-draw is neither possible nor needed.
        let vbuf = self.pool.vbuf();
        let vbuf_id = (vbuf as *const wgpu::Buffer as usize, 0u64);
        if st.vbuf != Some(vbuf_id) {
            pass.set_vertex_buffer(0, vbuf.slice(..));
            st.vbuf = Some(vbuf_id);
        }
        let ibuf = self.pool.ibuf();
        let ibuf_id = ibuf as *const wgpu::Buffer as usize;
        if st.ibuf != Some(ibuf_id) {
            pass.set_index_buffer(ibuf.slice(..), wgpu::IndexFormat::Uint32);
            st.ibuf = Some(ibuf_id);
        }

        pass.draw_indexed_indirect(args, arg_offset as u64);
    }

    // --- GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3) ---

    // Mutable access to the retained scene for the FFI registration path (Stage 3b-3).
    #[allow(dead_code)] // wired to the model/instance FFI in Stage 3b-3
    pub fn cull_scene(&mut self) -> &mut cull::CullState {
        &mut self.cull
    }

    // Resolve one section's pool addressing from the CURRENT mesh alloc (the pool can relocate
    // a mesh's geometry, so this is done fresh — at registration and again every frame). An
    // unknown mesh handle draws nothing (index_count = 0), keeping the tables parallel.
    fn resolve_section(&self, src: &GpuSectionSrc) -> cull::SectionGpu {
        let key: MeshKey = KeyData::from_ffi(src.mesh).into();
        match self.meshes.get(key) {
            // Bound WHOLE under indirect: base_vertex = the mesh's pool vbase, first_index =
            // its ibase + the section's mesh-local start (indices are 0-based mesh-local).
            Some(mesh) => cull::SectionGpu {
                first_index: mesh.alloc.ibase + src.index_begin,
                index_count: src.index_count,
                base_vertex: mesh.alloc.vbase,
                variant: src.variant,
            },
            None => cull::SectionGpu {
                first_index: 0,
                index_count: 0,
                base_vertex: 0,
                variant: src.variant,
            },
        }
    }

    // Re-resolve every registered section's pool addressing from the current mesh allocs and
    // push it into the cull's sections table. The pool relocates a mesh's geometry when its VB
    // is released + recreated (LOD optimisation / shape reload), which changes its vbase — the
    // CPU indirect + shadow paths avoid a stale base_vertex by resolving fresh each frame, and
    // this does the same for the GPU-driven set. No-op when nothing is registered.
    fn refresh_cull_sections(&mut self) {
        if self.gpu_section_src.is_empty() {
            return;
        }
        let mut stale = 0u32;
        let resolved: Vec<cull::SectionGpu> = self
            .gpu_section_src
            .iter()
            .map(|s| {
                let r = self.resolve_section(s);
                // A non-empty section that resolves to 0 indices => its mesh handle is gone.
                if s.index_count != 0 && r.index_count == 0 {
                    stale += 1;
                }
                r
            })
            .collect();
        if stale != self.gpu_dbg_stale {
            eprintln!(
                "[wgr] refresh_cull_sections: {}/{} sections have a STALE/destroyed mesh handle \
                 (resolve -> 0 indices); those draw nothing",
                stale,
                self.gpu_section_src.len(),
            );
            self.gpu_dbg_stale = stale;
        }
        self.cull.set_sections(&resolved);
    }

    // Register a GPU-driven model from the FFI descriptors (docs/gpu-culling-and-depth-plan.md
    // Stage 3b-3). Resolves each section's mesh handle to its shared-pool base_vertex/first_index
    // and each material's texture handle to a bindless slot, then appends to the retained tables.
    // A section whose mesh handle is unknown draws nothing (index_count = 0), keeping the
    // section/material/LOD arrays parallel. Returns the model id.
    pub fn register_model(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        bounding_sphere: f32,
        lods: &[WgrModelLod],
        sections: &[WgrModelSection],
        materials: &[WgrModelMaterial],
        textures: &SharedTextures,
        name: Option<&str>,
    ) -> u32 {
        let gpu_lods: Vec<cull::LodGpu> = lods
            .iter()
            .map(|l| cull::LodGpu {
                resolution: l.resolution,
                section_base: l.section_base,
                section_count: l.section_count,
                is_decal: l.is_decal,
            })
            .collect();
        let debug = std::env::var_os("WGR_GPU_DEBUG").is_some();
        let srcs: Vec<GpuSectionSrc> = sections
            .iter()
            .map(|s| GpuSectionSrc {
                mesh: s.mesh,
                index_begin: s.index_begin,
                index_count: s.index_count,
                variant: s.variant,
            })
            .collect();
        // Validate + optionally dump the registration-time resolution (diagnostics only; the
        // authoritative resolution happens each frame in refresh_cull_sections).
        for (k, s) in srcs.iter().enumerate() {
            let key: MeshKey = KeyData::from_ffi(s.mesh).into();
            match self.meshes.get(key) {
                Some(mesh) => {
                    let end = s.index_begin.saturating_add(s.index_count);
                    if end > mesh.index_count {
                        eprintln!(
                            "[wgr] SECTION OVERFLOW sec {k}: mesh {:#x} index_count={} but \
                             section wants [{}, {}) (vbase={} ibase={} vert_count={})",
                            s.mesh,
                            mesh.index_count,
                            s.index_begin,
                            end,
                            mesh.alloc.vbase,
                            mesh.alloc.ibase,
                            mesh.vert_count,
                        );
                    }
                    if debug && k < 8 {
                        eprintln!(
                            "[wgr] sec {k}: mesh {:#x} vbase={} first_index={} idx_count={} \
                             variant={} | mesh vert_count={} index_count={} local_begin={}",
                            s.mesh,
                            mesh.alloc.vbase,
                            mesh.alloc.ibase + s.index_begin,
                            s.index_count,
                            s.variant,
                            mesh.vert_count,
                            mesh.index_count,
                            s.index_begin,
                        );
                    }
                }
                None => eprintln!("[wgr] section mesh {:#x} NOT FOUND (draws nothing)", s.mesh),
            }
        }
        let gpu_sections: Vec<cull::SectionGpu> =
            srcs.iter().map(|s| self.resolve_section(s)).collect();
        self.gpu_section_src.extend_from_slice(&srcs);
        // `materials` is parallel to `sections` (one material per section), so the
        // section's flags word rides across here -- DZ-003's reflective bit lives in
        // WgrModelSection::flags because WgrModelMaterial is size-locked at 192 bytes.
        // zip() stops at the shorter of the two, which is the same defensive length rule
        // the section/material pairing already relies on.
        let gpu_materials: Vec<cull::SectionMaterialGpu> = materials
            .iter()
            .zip(sections.iter())
            .map(|(m, s)| cull::SectionMaterialGpu {
                emissive: m.emissive,
                ambient: m.ambient,
                diffuse: m.diffuse,
                specular: m.specular,
                texture_slot: textures.texture_slot(m.texture_id),
                sampler: m.sampler,
                alpha_ref: m.alpha_ref,
                specular_slot: textures.texture_slot(m.specular_texture_id),
                normal_slot: textures.texture_slot(m.normal_texture_id),
                mask_slot: textures.texture_slot(m.mask_texture_id),
                layer_slot: [
                    textures.texture_slot(m.layer_texture_id[0]),
                    textures.texture_slot(m.layer_texture_id[1]),
                    textures.texture_slot(m.layer_texture_id[2]),
                ],
                flags: s.flags,
                layer_normal_slot: [
                    textures.texture_slot(m.layer_normal_texture_id[0]),
                    textures.texture_slot(m.layer_normal_texture_id[1]),
                    textures.texture_slot(m.layer_normal_texture_id[2]),
                ],
                _pad_layer: [0; 3],
                layer_uv: m.layer_uv,
            })
            .collect();
        let model =
            self.cull
                .register_model(bounding_sphere, &gpu_lods, &gpu_sections, &gpu_materials);
        self.bake_model_sky_visibility(device, queue, model, lods, sections, name);
        model
    }

    // Bake this model's sky-visibility volume (docs/interior-sky-visibility-plan.md §3c) from its
    // LOD 0 geometry, straight out of the geometry pool.
    //
    // LOD 0 only: it is the silhouette the player stands inside, and a coarser LOD would bake a
    // building whose walls are in slightly the wrong place — the one error this whole approach
    // exists to avoid.
    //
    // §3d's requirements (content-hashed disk cache, background scheduling, a reach = 1 fallback
    // while a volume is missing) are NOT met yet, which is exactly why this is gated off by
    // default: a synchronous bake of a whole model library at load time is the load stall §3d
    // warns about.
    fn bake_model_sky_visibility(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        model: u32,
        lods: &[WgrModelLod],
        sections: &[WgrModelSection],
        name: Option<&str>,
    ) {
        if !self.sky_bake_enabled {
            return;
        }
        let Some(lod0) = lods.first() else {
            return;
        };
        let (mut lo, mut hi) = ([f32::MAX; 3], [f32::MIN; 3]);
        let mut ranges: Vec<sky_bake::PoolRange> = Vec::new();
        for i in 0..lod0.section_count {
            let Some(sec) = sections.get((lod0.section_base + i) as usize) else {
                continue;
            };
            let key: MeshKey = KeyData::from_ffi(sec.mesh).into();
            let Some(mesh) = self.meshes.get(key) else {
                continue;
            };
            for a in 0..3 {
                lo[a] = lo[a].min(mesh.aabb_min[a]);
                hi[a] = hi[a].max(mesh.aabb_max[a]);
            }
            ranges.push((
                mesh.alloc.ibase + sec.index_begin,
                sec.index_count,
                mesh.alloc.vbase as i32,
            ));
        }
        if ranges.is_empty() || lo[0] > hi[0] {
            return;
        }
        // A volume whose every voxel sees the sky is identical to no volume, and a
        // bush or roadside rock has no interior a 16k-voxel bake can discover — but it
        // costs exactly as much as a church. Models below this bounding-box volume are
        // not baked (they read as fully open sky, which is what their surfaces see).
        // WGR_SKY_BAKE_MIN_VOLUME_M3 overrides; 0 restores bake-everything.
        static MIN_VOLUME_M3: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
        let min_volume = *MIN_VOLUME_M3.get_or_init(|| {
            std::env::var("WGR_SKY_BAKE_MIN_VOLUME_M3")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(4.0)
        });
        let volume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
        if volume < min_volume {
            return;
        }
        // Derived-data cache (roadmap Phase 6): the bake is deterministic in the model
        // geometry, so a volume computed once is a volume forever. Keyed by the model
        // NAME plus its lod0 bounding box (the box moves whenever the geometry the bake
        // sees moves, which is the practical invalidation for repacked content) and a
        // format version. WGR_SKY_BAKE_CACHE=0 disables both read and write.
        let cache_path = name.and_then(|n| sky_bake_cache_path(n, &lo, &hi));
        if let Some(path) = cache_path.as_deref() {
            if let Some((vis, bmin, bmax)) = sky_bake_cache_read(path) {
                self.finish_sky_bake(model, vis, bmin, bmax);
                return;
            }
        }
        let bake = self
            .sky_bake
            .get_or_insert_with(|| sky_bake::SkyBake::new(device, BAKED_VERT_SIZE));
        let dirs = sky_bake::hemisphere_directions();
        let settings = sky_bake::BakeSettings::default();
        let Some(pending) = bake.bake_begin(
            device,
            queue,
            sky_bake::BakeSource::Pool {
                vbuf: self.pool.vbuf(),
                ibuf: self.pool.ibuf(),
                ranges: &ranges,
                bbox_min: lo,
                bbox_max: hi,
            },
            &dirs,
            &settings,
        ) else {
            return;
        };
        // Registration does NOT wait for the GPU round-trip (that wait was 61% of the
        // whole cold-model admission cost). collect_sky_bakes() picks the result up.
        self.sky_bakes_pending.push((model, pending, cache_path));
    }

    // Drain completed sky bakes. Called once per frame after submit; the map callbacks
    // fire during device polls/submissions, so results typically land 1-2 frames after
    // the registration that started them.
    pub fn collect_sky_bakes(&mut self, device: &wgpu::Device) {
        if self.sky_bakes_pending.is_empty() {
            return;
        }
        let _ = device.poll(wgpu::PollType::Poll);
        let mut i = 0;
        while i < self.sky_bakes_pending.len() {
            let (model, pending, _) = &self.sky_bakes_pending[i];
            let model = *model;
            match pending.try_take() {
                None => {
                    i += 1;
                    continue;
                }
                Some(None) => {
                    self.sky_bakes_pending.swap_remove(i);
                }
                Some(Some((vis, bmin, bmax))) => {
                    let (_, _, cache_path) = self.sky_bakes_pending.swap_remove(i);
                    if let Some(path) = cache_path.as_deref() {
                        sky_bake_cache_write(path, &vis, &bmin, &bmax);
                    }
                    self.finish_sky_bake(model, vis, bmin, bmax);
                }
            }
        }
    }

    fn finish_sky_bake(&mut self, model: u32, vis: Vec<[f32; 4]>, bmin: [f32; 3], bmax: [f32; 3]) {
        // Report the first few, because a bake that silently produces an all-open volume is
        // indistinguishable from a working one downstream — the same failure mode the per-frame
        // map's coverage check exists for. `enclosed` is the fraction of voxels that see less
        // than half the sky; a building with any interior must have some.
        if self.sky_volumes.len() < 8 {
            let enclosed = vis.iter().filter(|v| v[3] < 0.5).count() as f32 / vis.len() as f32;
            let mean = vis.iter().map(|v| v[3]).sum::<f32>() / vis.len() as f32;
            // Queued rather than printed: Gfx3d has no log sink, and eprintln! never reaches
            // --log-file, so a diagnostic written that way is missing exactly when it is read.
            self.queued_log.push(format!(
                    "[wgr] sky bake model {model}: {} voxels, mean vis {mean:.3}, {:.1}% enclosed,                      bbox {:.1}x{:.1}x{:.1} m",
                    vis.len(),
                    enclosed * 100.0,
                    bmax[0] - bmin[0],
                    bmax[1] - bmin[1],
                    bmax[2] - bmin[2],
            ));
        }
        self.sky_volumes.insert(model, (vis, bmin, bmax));
        self.sky_volumes_dirty = true;
    }

    pub fn register_crown_centres(&mut self, centres: &[[f32; 4]]) -> u32 {
        self.cull.register_crown_centres(centres)
    }

    pub fn instance_add(&mut self, inst: &WgrInstance) -> u32 {
        self.cull.instance_add(instance_to_gpu(inst))
    }

    pub fn instance_update(&mut self, slot: u32, inst: &WgrInstance) {
        self.cull.instance_update(slot, instance_to_gpu(inst));
    }

    pub fn instance_remove(&mut self, slot: u32) {
        self.cull.instance_remove(slot);
    }

    pub fn set_dynamic(&mut self, instances: &[WgrInstance]) {
        // Convert explicitly rather than transmuting the slice (keeps the FFI + GPU structs
        // decoupled).
        let gpu: Vec<cull::InstanceGpu> = instances.iter().map(instance_to_gpu).collect();
        self.cull.set_dynamic(&gpu);
    }

    // Upload the retained buffers + this frame's cull params, rebuilding the GPU-driven
    // group-1 bind if a buffer grew. No-op when GPU-driven rendering is off.
    // Store the engine's per-frame cull + LOD inputs (objectsZ / Camera::Left() /
    // Scene::_lodInvWidth / pixel_limit) for the next prepare_cull. Cheap; called once/frame.
    pub fn set_cull_inputs(
        &mut self,
        objects_z: f32,
        lod_scale: f32,
        lod_inv_width: f32,
        pixel_limit: f32,
    ) {
        self.cull_inputs = cull::CullInputs {
            objects_z,
            lod_scale,
            lod_inv_width,
            pixel_limit,
        };
    }

    /// This frame's engine cull + LOD inputs, exactly as last pushed by wgr_set_cull_params.
    ///
    /// Read by the FAR INSTANCE TIER, whose start rule is the COMPLEMENT of the object cull:
    /// it accepts precisely what these four numbers make the object cull drop. Exposed as a
    /// getter rather than passed through a second C ABI call because the C++ side already
    /// pushes them once per frame and a second copy could only ever disagree with this one.
    pub fn cull_inputs(&self) -> cull::CullInputs {
        self.cull_inputs
    }

    /// Drain queued diagnostics (sky bake, staging refusals) for the caller's log sink.
    pub fn take_queued_log(&mut self) -> Vec<String> {
        std::mem::take(&mut self.queued_log)
    }

    // Record one refused per-frame staging upload and queue a log line on the 1st, 10th,
    // 100th ... occurrence. Uses the same queue as the bake diagnostics because Gfx3d has no
    // log sink and `eprintln!` never reaches --log-file — which is exactly where this needs
    // to be visible, since the condition that produces it is only reproducible in a long run.
    fn note_staging_refusal(&mut self, which: &'static str, bytes: u64, draws: usize) {
        let count = self.staging_refusals.entry(which).or_insert(0);
        *count += 1;
        let n = *count;
        if !is_power_of_ten(n) {
            return;
        }
        self.queued_log.push(format!(
            "[wgr] {which} staging upload REFUSED by wgpu ({bytes} B for {draws} draws) \
             — out of GPU memory. Occurrence {n}. The buffer is dropped and rebuilt next \
             frame; this frame's direct-path objects are skipped rather than drawn with \
             stale transforms. WGR_STAGING_STRICT=1 restores the old panic."
        ));
    }

    pub fn set_interior_sky_settings(&mut self, s: SkyVisSettings) {
        self.interior_sky = s;
    }

    pub fn interior_sky_settings(&self) -> &SkyVisSettings {
        &self.interior_sky
    }

    // Whether the map exists and this frame's view was built — the single gate every consumer
    // (cull dispatch, depth pass, camera UBO, shader) reads. False leaves reach = 1 everywhere,
    // which is "no darkening", the correct absence behaviour for an occlusion term.
    pub fn interior_sky_active(&self) -> bool {
        self.interior_sky_view.is_some() && self.interior_sky_target.is_some()
    }

    // (Re)allocate the sky depth target and build this frame's snapped ortho view. Called from
    // prepare() BEFORE the camera bind group is built, because that bind group binds this
    // texture at @binding(12).
    //
    // Requires GPU-driven rendering: the map is drawn entirely from the cull compute's indirect
    // args, so with the CPU path there is nothing to render into it. Silently inert rather than
    // half-working — a map containing only some of the world would darken by accident.
    fn prepare_interior_sky(&mut self, device: &wgpu::Device, cam_pos: glam::Vec3) {
        if !(self.interior_sky.enabled && self.gpu_driven_enabled) {
            if self.interior_sky_target.is_some() {
                self.interior_sky_target = None;
                self.interior_sky_gen += 1;
            }
            self.interior_sky_view = None;
            return;
        }
        let res = self.interior_sky.resolution.max(1);
        let layers = sky_vis::DIRECTION_COUNT as u32;
        let stale = self
            .interior_sky_target
            .as_ref()
            .is_none_or(|(t, _, _)| t.width() != res || t.depth_or_array_layers() != layers);
        if stale {
            let tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("wgr_interior_sky_map"),
                size: wgpu::Extent3d {
                    width: res,
                    height: res,
                    depth_or_array_layers: layers,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: SHADOW_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            let layer_views = (0..layers)
                .map(|l| {
                    tex.create_view(&wgpu::TextureViewDescriptor {
                        label: Some("wgr_interior_sky_layer"),
                        dimension: Some(wgpu::TextureViewDimension::D2),
                        base_array_layer: l,
                        array_layer_count: Some(1),
                        ..Default::default()
                    })
                })
                .collect();
            let sample_view = tex.create_view(&wgpu::TextureViewDescriptor {
                label: Some("wgr_interior_sky_sample"),
                dimension: Some(wgpu::TextureViewDimension::D2Array),
                ..Default::default()
            });
            self.interior_sky_target = Some((tex, layer_views, sample_view));
            self.interior_sky_gen += 1;
        }
        self.interior_sky_views = sky_vis::build_views(cam_pos, &self.interior_sky);
        self.interior_sky_view = Some(self.interior_sky_views[0]);
    }

    // Record the sky-visibility cull. Recorded before render_interior_sky_pass so wgpu barriers
    // the compute writes -> that pass's indirect reads.
    pub fn cull_dispatch_interior_sky(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.interior_sky_active() {
            return;
        }
        timers.begin(encoder, crate::gpu_timers::Region::InteriorSkyCull);
        for i in 0..self.cull.sky_view_count() {
            self.cull.dispatch_sky(encoder, i);
        }
        timers.end(encoder, crate::gpu_timers::Region::InteriorSkyCull);
    }

    // Render the top-down depth map: one depth-only pass over the sky cull view's args, drawn by
    // the SAME GPU-driven shadow pipeline the cascades use (same depth format, same forward-Z
    // LessEqual convention, same group layouts) with the ortho VP supplied through the shadow
    // pass UBO's reserved slot.
    pub fn render_interior_sky_pass(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        textures: &SharedTextures,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.interior_sky_active() {
            return;
        }
        let (Some((_, layer_views, _)), Some(pass_bind)) = (
            self.interior_sky_target.as_ref(),
            self.shadow_pass_ubo.bind.as_ref(),
        ) else {
            return;
        };
        // One depth-only pass per sampled direction, all bracketed as a single timed region:
        // the cost that matters to a decision ("is sampling the dome affordable") is the whole
        // set, not any one layer.
        timers.begin(encoder, crate::gpu_timers::Region::InteriorSkyDraw);
        for (i, target_view) in layer_views.iter().enumerate() {
            let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("wgr_interior_sky_map"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: target_view,
                    depth_ops: Some(wgpu::Operations {
                        // Clear to the FAR plane: an untouched texel means "nothing between this
                        // point and the sky", so open sky is the default and only real geometry
                        // can take it away.
                        load: wgpu::LoadOp::Clear(1.0),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            let off = ((SKY_UBO_SLOT + i) as u64 * self.shadow_pass_ubo.stride) as u32;
            rp.set_bind_group(0, pass_bind, &[off]);
            self.draw_gpu_driven_depth(
                &mut rp,
                textures,
                off,
                self.gpu_sky_group1.get(i).and_then(|b| b.as_ref()),
                self.cull.sky_out_args(i),
                self.cull.sky_counter_buf(i),
            );
        }
        timers.end(encoder, crate::gpu_timers::Region::InteriorSkyDraw);
    }

    // Read back EVERY sky-map layer and report (resolution, per-direction fraction of texels
    // holding an occluder). Index 0 is the zenith; the rest are the tilted directions.
    //
    // Per layer, not just the zenith, because the tilted maps are the entire reason this feature
    // can see through a window — and a tilted layer that renders nothing is invisible in every
    // other signal: it clears to the far plane, every comparison passes, its reach reads 1, and
    // the result is simply the zenith-only behaviour wearing a five-direction costume.
    //
    // Synchronous and slow (a depth readback + device poll per layer); one-shot diagnostic only.
    pub fn interior_sky_map_coverage(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
    ) -> Option<(u32, [f32; sky_vis::DIRECTION_COUNT])> {
        let (tex, _, _) = self.interior_sky_target.as_ref()?;
        let res = tex.width();
        let mut px = vec![0.0f32; (res * res) as usize];
        let mut cov = [0.0f32; sky_vis::DIRECTION_COUNT];
        for (layer, slot) in cov.iter_mut().enumerate() {
            if !read_depth_layer(device, queue, tex, res, layer as u32, &mut px) {
                return None;
            }
            // Cleared texels hold exactly the far plane; anything less is geometry.
            let occluded = px.iter().filter(|d| **d < 0.999).count();
            *slot = occluded as f32 / px.len() as f32;
        }
        Some((res, cov))
    }

    // Which link of the sky-map chain is missing, for the one-shot diagnostic in lib.rs:
    // (cull views prepared, draw binds built, retained instances, sub-draws the ZENITH cull
    // emitted).
    //
    // The last number is the one that matters and the reason this is not just booleans: "the
    // args buffer exists" says nothing about whether anything survived into it, and an empty map
    // is equally consistent with a cull that rejected the world and a draw that never ran.
    pub fn interior_sky_debug_state(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
    ) -> (bool, bool, u32, u32) {
        // Read the ARGS, not the counters: the counter buffers carry no COPY_SRC, and the args
        // are the actual draw payload anyway. instance_count 0 is the unfilled tail.
        let words = (cull::CULL_VARIANT_COUNT as u64
            * self.cull.variant_capacity() as u64
            * (INDIRECT_ARG_SIZE / 4))
            .min(1 << 20);
        let survivors = self
            .cull
            .sky_out_args(0)
            .map(|a| {
                read_u32_buffer(device, queue, a, words)
                    .chunks_exact((INDIRECT_ARG_SIZE / 4) as usize)
                    .filter(|d| d[1] != 0)
                    .count() as u32
            })
            .unwrap_or(0);
        (
            self.cull.sky_out_args(0).is_some(),
            !self.gpu_sky_group1.is_empty(),
            self.cull.instance_count(),
            survivors,
        )
    }

    pub fn prepare_cull(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        cam: &WgrCamera,
        shadow: &WgrShadowPass,
        reflected_cam: Option<&WgrCamera>,
        reflection_detail_reduction: f32,
    ) {
        if !self.gpu_driven_enabled {
            return;
        }
        // Re-resolve section pool addressing from the current mesh allocs (a mesh's vbase can
        // move when its VB is recreated), so the cull emits correct base_vertex/first_index.
        self.refresh_cull_sections();
        let view = glam::Mat4::from_cols_array(&cam.view);
        let proj = glam::Mat4::from_cols_array(&cam.proj);
        let cam_pos = glam::Vec3::new(cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]);
        self.cull.set_params(cull::params_from_camera(
            view,
            proj,
            cam_pos,
            self.cull_inputs,
        ));
        if let Some(cam) = reflected_cam {
            let view = glam::Mat4::from_cols_array(&cam.view);
            let proj = glam::Mat4::from_cols_array(&cam.proj);
            let pos = glam::Vec3::new(cam.cam_pos[0], cam.cam_pos[1], cam.cam_pos[2]);
            // The planar target is both lower resolution and wider-FOV than the main view.
            // Reusing the main viewport's LOD metric made the reflection render model detail
            // that cannot occupy a pixel in its target. Correct the metric, shared by every
            // retained compatibility layer, while preserving draw distance and frustum rules.
            let reflection_inputs = self
                .cull_inputs
                .for_reduced_view(reflection_detail_reduction);
            self.cull.set_reflection_params(
                device,
                cull::params_from_camera(view, proj, pos, reflection_inputs),
            );
        } else {
            self.cull.clear_reflection_view();
        }
        // Color-pass Hi-Z occlusion view (§5): (re)size the pyramid to the depth target and set
        // the color params (same frustum/LOD as the main view + the occlusion tail). set_hiz(None)
        // when off leaves color_active() false, so the color draw falls back to the main args.
        if self.occlusion_enabled {
            let (vw, vh) = self.depth_size;
            self.hiz.ensure(device, vw, vh);
            self.cull.set_hiz(self.hiz.view().cloned());
            self.cull.set_color_params(cull::params_from_camera_occlude(
                view,
                proj,
                cam_pos,
                self.cull_inputs,
                [vw as f32, vh as f32],
                self.hiz.mips(),
                true,
            ));
        } else {
            self.cull.set_hiz(None);
        }
        // Shadow-cascade cull views (§6 multi-view): one per active cascade, each culling the
        // retained scene against that cascade's light frustum (LOD from the main view). The GPU
        // then casts survivors into the cascade depth map (draw_gpu_driven_shadow). count = 0
        // (shadows off) clears the views so no shadow dispatch runs.
        let n_cascades = (shadow.count as usize).min(MAX_CASCADES as usize);
        self.cull.set_shadow_view_count(device, n_cascades);
        let scam = glam::Vec3::new(shadow.cam_pos[0], shadow.cam_pos[1], shadow.cam_pos[2]);
        for c in 0..n_cascades {
            let lvp = glam::Mat4::from_cols_array(&shadow.light_vp[c]);
            self.cull.set_shadow_params(
                c,
                cull::params_from_shadow_cascade(lvp, scam, self.cull_inputs),
            );
        }
        // Interior sky-visibility view: its own ortho frustum over the same retained set. The
        // view itself was built in prepare() (the camera UBO needed it); here it becomes a cull
        // view + a pass-UBO slot.
        //
        // SPACES, the one thing that is easy to get silently wrong here: the GPU-driven depth VS
        // makes each vertex camera-relative before applying light_vp, and the cull's frustum test
        // is camera-relative too. sky_vis::build_view returns the ABSOLUTE-space matrix (that is
        // what the fragment shader needs, and it is the same for every camera in the frame), so
        // both consumers here take it right-multiplied by a +cam_pos translation.
        if self.interior_sky_view.is_some() {
            let dirs = sky_vis::directions();
            self.cull
                .set_sky_view_count(device, sky_vis::DIRECTION_COUNT);
            self.shadow_pass_ubo
                .ensure(device, SKY_UBO_SLOT + sky_vis::DIRECTION_COUNT);
            for i in 0..sky_vis::DIRECTION_COUNT {
                let vp_rel =
                    self.interior_sky_views[i].view_proj * glam::Mat4::from_translation(cam_pos);
                if let Some(buf) = self.shadow_pass_ubo.buf.as_ref() {
                    let entry = ShadowPassUbo {
                        light_vp: vp_rel.to_cols_array(),
                        cam_pos: [cam_pos.x, cam_pos.y, cam_pos.z, 0.0],
                    };
                    queue.write_buffer(
                        buf,
                        (SKY_UBO_SLOT + i) as u64 * self.shadow_pass_ubo.stride,
                        bytemuck::bytes_of(&entry),
                    );
                }
                let _ = dirs;
                self.cull.set_sky_params(
                    i,
                    cull::params_from_shadow_cascade(vp_rel, cam_pos, self.cull_inputs),
                );
            }
        } else {
            self.cull.set_sky_view_count(device, 0);
        }
        // Seed the dummies on the first call so the group-1 layout is satisfiable
        // immediately, bake or no bake.
        if self.sky_volume_meta.buf.is_none() {
            self.sky_volumes_dirty = true;
        }
        let volumes_grew = self.upload_sky_volumes(device, queue);
        let grew = self.cull.prepare(device, queue) || volumes_grew;
        let mipfb_grew = std::mem::take(&mut self.mip_feedback_grew);
        if grew
            || mipfb_grew
            || self.gpu_group1_bind.is_none()
            || self.gpu_shadow_group1.len() != n_cascades
            // The sky view is allocated on first enable and dropped on disable, so its records
            // buffer appears/disappears without any of the growth signals firing.
            || self.gpu_sky_group1.len() != self.cull.sky_view_count()
        {
            self.rebuild_gpu_group1(device);
        }
    }

    // Pack every baked volume into the two storage buffers the draw samples. Returns whether a
    // buffer moved, so the group-1 binds that borrow them are rebuilt.
    //
    // One flat data buffer with manual trilinear in the shader, rather than a 3D texture atlas:
    // a 3D texture caps out (2048 on the largest axis, so ~128 models at 16 voxels deep) and an
    // R8Unorm 3D storage texture is not a core format anyway. A buffer has neither limit, and
    // eight fetches is a small price for a term that only modulates ambient.
    fn upload_sky_volumes(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) -> bool {
        if !self.sky_volumes_dirty {
            return false;
        }
        self.sky_volumes_dirty = false;
        let d = sky_bake::BakeSettings::default().dims;
        let stride = (d[0] * d[1] * d[2]) as usize;
        let n_models = self.sky_volumes.keys().copied().max().unwrap_or(0) as usize + 1;
        // meta[model] = (min.xyz, offset_in_voxels) then (max.xyz, has_volume)
        let mut meta = vec![[0.0f32; 4]; n_models * 2];
        let mut data: Vec<[f32; 4]> = Vec::with_capacity(self.sky_volumes.len() * stride);
        for (&model, (vis, lo, hi)) in self.sky_volumes.iter() {
            if vis.len() != stride {
                continue;
            }
            let off = data.len();
            data.extend_from_slice(vis);
            let m = model as usize * 2;
            meta[m] = [lo[0], lo[1], lo[2], off as f32];
            meta[m + 1] = [hi[0], hi[1], hi[2], 1.0];
        }
        if data.is_empty() {
            // Never leave a zero-sized storage buffer: the bind group would fail validation and
            // take every GPU-driven draw down with it, feature enabled or not.
            data.push([0.0, 1.0, 0.0, 1.0]);
        }
        let mut grew = self
            .sky_volume_meta
            .ensure(device, (meta.len() * 16).max(16) as u64);
        grew |= self
            .sky_volume_data
            .ensure(device, (data.len() * 16).max(16) as u64);
        if let Some(b) = self.sky_volume_meta.buf.as_ref() {
            queue.write_buffer(b, 0, bytemuck::cast_slice(&meta));
        }
        if let Some(b) = self.sky_volume_data.buf.as_ref() {
            queue.write_buffer(b, 0, bytemuck::cast_slice(&data));
        }
        grew
    }

    fn rebuild_gpu_group1(&mut self, device: &wgpu::Device) {
        // The two sky-volume buffers are always present (upload_sky_volumes seeds a one-element
        // dummy when nothing is baked) so the shared layout never has an absent binding.
        let (Some(inst), Some(rec), Some(mat), Some(crown), Some(vmeta), Some(vdata), Some(mipfb)) = (
            self.cull.instance_buf(),
            self.cull.out_records(),
            self.cull.section_material_buf(),
            self.cull.crown_centre_buf(),
            self.sky_volume_meta.buf.as_ref(),
            self.sky_volume_data.buf.as_ref(),
            self.mip_feedback.buffer(),
        ) else {
            self.gpu_group1_bind = None;
            self.gpu_color_group1_bind = None;
            self.cull_debug_bind = None;
            return;
        };
        // Every view's group-1 bind is the SAME layout over the SAME shared buffers, differing
        // only in which cull view's records it points at — so build them all through one helper
        // rather than repeating the four-entry descriptor per view.
        let build = |label: &'static str, records: &wgpu::Buffer| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some(label),
                layout: &self.gpu_group1_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: inst.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: records.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: mat.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: crown.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 4,
                        resource: vmeta.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 5,
                        resource: vdata.as_entire_binding(),
                    },
                    // DZ-003: Sky's equirect reflection env map + sampler, for sections
                    // whose material named an EnvironmentMap. A 1x1 dummy until Sky lends
                    // the real view, so the layout is satisfied from the first frame.
                    wgpu::BindGroupEntry {
                        binding: 6,
                        resource: wgpu::BindingResource::TextureView(&self.env_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 7,
                        resource: wgpu::BindingResource::Sampler(&self.env_sampler),
                    },
                    // AST-012A mip feedback. Shared by every view's bind, so the shadow and
                    // reflection draws point at the same buffer even though only fs_gpu writes
                    // it — one buffer, one clear, one readback per frame.
                    wgpu::BindGroupEntry {
                        binding: 8,
                        resource: mipfb.as_entire_binding(),
                    },
                ],
            })
        };
        self.gpu_group1_bind = Some(build("wgr_gpu_driven_group1_bind", rec));
        self.gpu_reflection_group1_bind = self
            .cull
            .reflection_out_records()
            .map(|r| build("wgr_gpu_driven_reflection_group1_bind", r));
        // Interior sky-visibility views' records — one per sampled direction, empty when off.
        self.gpu_sky_group1 = (0..self.cull.sky_view_count())
            .map(|i| {
                self.cull
                    .sky_out_records(i)
                    .map(|r| build("wgr_gpu_driven_sky_group1_bind", r))
            })
            .collect();
        // Color-pass draw bind: instances + the occlusion view's records + shared materials.
        // Only when the color view is live (occlusion active); else the color draw reuses the
        // main bind.
        self.gpu_color_group1_bind = self
            .cull
            .color_out_records()
            .map(|r| build("wgr_gpu_driven_color_group1_bind", r));
        // Cull-sphere debug bind (instances + models) — rebuilt on the same buffer-growth signal.
        self.cull_debug_bind = self.cull.model_buf().map(|models| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_cull_debug_bind"),
                layout: &self.cull_debug_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: inst.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: models.as_entire_binding(),
                    },
                ],
            })
        });
        // Per-cascade shadow group-1 draw binds: instances + THAT cascade's records + the shared
        // materials. Same layout as the colour group-1, only the records differ per cascade.
        let n = self.cull.shadow_view_count();
        self.gpu_shadow_group1.clear();
        for c in 0..n {
            let bind = self
                .cull
                .shadow_out_records(c)
                .map(|r| build("wgr_gpu_driven_shadow_group1_bind", r));
            self.gpu_shadow_group1.push(bind);
        }
    }

    // Runtime toggle from the ImGui Culling tab: skip the GPU frustum test.
    pub fn set_cull_no_frustum(&mut self, no_frustum: bool) {
        self.cull.set_no_frustum(no_frustum);
    }

    // Cull-sphere debug pass: one instanced line-list wireframe sphere per retained instance at
    // the exact centre + radius the cull tests. Draws on top (depth compare Always, no write).
    // No-op when GPU-driven is off or the scene is empty.
    pub fn draw_cull_spheres(&self, pass: &mut wgpu::RenderPass<'_>, cam_off: u32) {
        if !self.gpu_driven_enabled {
            return;
        }
        let (Some(camera_bind), Some(bind)) =
            (self.cameras.bind.as_ref(), self.cull_debug_bind.as_ref())
        else {
            return;
        };
        let instances = self.cull.instance_count();
        if instances == 0 {
            return;
        }
        // 3 rings * SEG(32) segments * 2 endpoints = 192 line vertices per instance.
        const VERTS_PER_SPHERE: u32 = 3 * 32 * 2;
        pass.set_pipeline(&self.cull_debug_pipeline);
        pass.set_bind_group(0, camera_bind, &[cam_off]);
        pass.set_bind_group(1, bind, &[]);
        pass.draw(0..VERTS_PER_SPHERE, 0..instances);
    }

    // Record the cull compute dispatch (before the render passes that read its args). No-op
    // when GPU-driven rendering is off or nothing is registered.
    // --- PERF-005 object accounting (see cull.rs STATS_*) ---

    /// Latest harvested per-view counts: instances, records, draw args, triangles and the
    /// selected-LOD histogram.
    pub fn object_stats(&self) -> cull::CullStats {
        self.cull.stats()
    }

    /// Registered retained instances (the denominator the per-view survivor counts divide into).
    pub fn retained_instance_count(&self) -> u32 {
        self.cull.instance_count()
    }

    /// Stale instance handles refused since startup (ID-3), so the engine can report the
    /// count rather than only the first eight log lines.
    pub fn stale_instance_ops(&self) -> u64 {
        self.cull.stale_instance_ops()
    }

    /// Zero every cull view's stats slice. Must be recorded before the frame's first cull.
    pub fn begin_frame_object_stats(&self, encoder: &mut wgpu::CommandEncoder) {
        self.cull.begin_frame_stats(encoder);
    }

    /// Copy this frame's stats into the readback ring (after every cull dispatch, before submit).
    pub fn resolve_object_stats(&mut self, encoder: &mut wgpu::CommandEncoder) {
        self.cull.resolve_stats(encoder);
    }

    /// Drain completed stats readbacks (after queue.submit, alongside GpuTimers::harvest).
    pub fn harvest_object_stats(&mut self, device: &wgpu::Device) {
        self.cull.harvest_stats(device);
    }

    pub fn cull_dispatch(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.gpu_driven_enabled {
            return;
        }
        // PERF-005: encoder-level bracket (the cull is three standalone compute passes), so this
        // measures the whole COUNT -> EMIT -> SCATTER chain including the clears in front of it.
        timers.cpu_begin(crate::gpu_timers::Region::ObjCullMain);
        timers.begin(encoder, crate::gpu_timers::Region::ObjCullMain);
        self.cull.dispatch(encoder);
        timers.end(encoder, crate::gpu_timers::Region::ObjCullMain);
        timers.cpu_end(crate::gpu_timers::Region::ObjCullMain);
    }

    pub fn cull_dispatch_reflection(&self, encoder: &mut wgpu::CommandEncoder) {
        if self.gpu_driven_enabled {
            self.cull.dispatch_reflection(encoder);
        }
    }

    // Whether the color-pass Hi-Z occlusion path is live this frame: GPU-driven on, occlusion
    // enabled, and the color cull view prepared (Hi-Z bound). When false the color draw reuses
    // the main frustum-cull args. Consulted by lib.rs to gate the Hi-Z build + color dispatch.
    pub fn occlusion_active(&self) -> bool {
        self.gpu_driven_enabled && self.occlusion_enabled && self.cull.color_active()
    }

    // Runtime toggle (ImGui Culling tab / WGR_GPU_OCCLUSION): enable/disable GPU Hi-Z occlusion.
    // Takes effect next frame (prepare_cull (re)allocates or drops the color view).
    pub fn set_occlusion_enabled(&mut self, enabled: bool) {
        self.occlusion_enabled = enabled;
    }

    // Build this frame's Hi-Z pyramid from the post-prepass depth (§5). Recorded AFTER the depth
    // prepass render pass closes and BEFORE cull_dispatch_color. No-op unless occlusion is active.
    pub fn build_hiz(&mut self, device: &wgpu::Device, encoder: &mut wgpu::CommandEncoder) {
        if !self.occlusion_active() {
            return;
        }
        let Some(depth) = self.depth_sample_view.clone() else {
            return;
        };
        // MSAA: depth_sample_view is the resolved single-sample target, which is stale until the
        // resolve pass fills it from this frame's freshly-completed prepass depth. No-op at 1x
        // (depth_sample_view is the depth target's own aspect, already current).
        self.resolve_depth_sample(encoder);
        self.hiz.build(device, encoder, &depth);
    }

    // MSAA depth -> single-sample nearest (depth_sample_view). No-op at 1x, where
    // depth_sample_view is the depth target's own aspect and is already current. Both Hi-Z and
    // GTAO need this, and only one of them may be active, so it is its own call.
    fn resolve_depth_sample(&self, encoder: &mut wgpu::CommandEncoder) {
        if let Some(dr) = self.depth_resolve.as_ref() {
            dr.resolve(encoder);
        }
    }

    // Render-target size the GTAO pass works at (== the depth target).
    pub fn render_size(&self) -> (u32, u32) {
        self.depth_size
    }

    /// REN-OBJ-003: the last harvested object fragment census (mip-feedback header words 0..3).
    pub fn object_fragment_census(&self) -> [u32; 4] {
        self.mip_feedback.fragment_counts()
    }

    pub fn set_material_debug(&mut self, view: u32, flags: u32) {
        self.material_debug_view = view;
        self.material_debug_flags = flags;
    }

    /// DZ-005 — publish this frame's water surface animation into the frame UBO.
    ///
    /// `time` is the engine's own water clock (`WgrWaterParams.time` = `Glob.time`), so the
    /// ripple pauses exactly when the simulation does and two launches of the same test
    /// mission animate identically. The ripple strength is *not* taken from the caller: it is
    /// an A/B switch fixed at construction from `WGR_WATER_RIPPLE`, so a before/after capture
    /// is two runs of one binary with one term different, which is the only comparison that
    /// cannot drift.
    ///
    /// `flow_slot` is the bindless slot of the world's river flow map (0 = none; Chernarus
    /// ships none at all), and `flow_strength` its `StreamSpeedInfl`.
    /// Fraction of the draw distance at which distance fog reaches FULL. 1.0 restores the
    /// behaviour from before this existed (fog saturates only at the far plane); lower closes the
    /// horizon earlier, hiding the terrain grid's edge and the object cull ring. Clamped in the
    /// shader as well, so a garbage push cannot black the screen.
    pub fn set_fog_far_close(&mut self, close: f32) {
        self.fog_far[0] = close.clamp(0.3, 1.0);
    }

    // REN-TEMP-001D: render-scale texture LOD (frame.wgsl `renscale`). `scale` is
    // render_width / output_width; 1.0 (or anything >= 1.0) writes the all-zero native lane.
    // WGR_MIP_BIAS overrides the derived log2(scale) bias — the A/B knob for the
    // sharpness-vs-distant-shimmer trade under a temporal upscaler (0 = no bias, softest
    // and calmest; log2(0.67) = -0.58 is the textbook value and the default).
    pub fn set_mip_bias_override(&mut self, bias: Option<f32>) {
        self.mip_bias_override = bias;
    }

    pub fn mip_bias_effective(&self) -> f32 {
        self.render_scale_lane[0]
    }

    pub fn set_render_scale_lane(&mut self, scale: f32) {
        let over = self.mip_bias_override;
        self.render_scale_lane = if scale < 1.0 && scale > 0.0 {
            // Default = HALF the textbook log2(scale): the owner's measured sweet spot
            // on this content (2026-08-29) — full bias keeps distant grass/terrain above
            // the frequency the accumulator can settle and reads as far-field shimmer.
            // Still scale-derived per plan §8; WGR_MIP_BIAS pins an absolute value.
            let bias = over.unwrap_or_else(|| 0.5 * scale.log2()).clamp(-4.0, 0.0);
            // Gradient scale for textureSampleGrad paths mirrors the bias: 2^bias.
            [bias, bias.exp2(), 0.0, 0.0]
        } else {
            [0.0; 4]
        };
        // z/w belong to AST-012A and are rewritten every frame by the caller straight after
        // this; writing them here as zeros first is what keeps "feedback off" the state a
        // renderer that never calls the setter below ends up in.
    }

    /// AST-012A — allocate the mip-feedback buffer for `capacity` bindless slots and publish
    /// this frame's rotation pair into `renscale.zw`. Called once per frame BEFORE the cameras
    /// are uploaded (the pair travels in the camera uniform) and before the frame's first
    /// object draw.
    pub fn ensure_mip_feedback(&mut self, device: &wgpu::Device, capacity: u32) {
        // Allocated even when the instrument is OFF, at one word instead of the full table.
        // Binding 8 is part of the shared group-1 layout, so an absent buffer would leave every
        // GPU-driven bind group unbuildable and take the whole object draw with it — the
        // ablation arm must change what the shader writes, never whether it can run.
        let capacity = if self.mip_feedback.enabled() {
            capacity
        } else {
            1
        };
        if self.mip_feedback.ensure(device, capacity) {
            self.mip_feedback_grew = true;
        }
        self.mip_feedback.advance();
        let (group, groups) = self.mip_feedback.lane();
        self.render_scale_lane[2] = group as f32;
        self.render_scale_lane[3] = groups as f32;
    }

    /// Advance the rotation and zero the per-slot codes. Must be recorded into the frame
    /// encoder before the first object draw.
    pub fn begin_frame_mip_feedback(&mut self, encoder: &mut wgpu::CommandEncoder) {
        self.mip_feedback.begin_frame(encoder);
    }

    /// Copy this frame's codes into the readback ring (after the last object draw, before submit).
    pub fn resolve_mip_feedback(&mut self, encoder: &mut wgpu::CommandEncoder) {
        self.mip_feedback.resolve(encoder);
    }

    /// Drain completed mip-feedback readbacks (after queue.submit).
    pub fn harvest_mip_feedback(&mut self, device: &wgpu::Device) {
        self.mip_feedback.harvest(device);
    }

    pub fn mip_feedback_state(&self) -> &MipFeedback {
        &self.mip_feedback
    }

    /// The three OnSurface (road / decal) per-pixel depth-conform knobs, live from the dev
    /// panel. They ride the reserved `fogfar` yzw lanes, so nothing about the struct layout or
    /// the C ABI moves. 0 in any lane means "use the shipped default", which is what an
    /// unwritten camera and every headless run get.
    ///
    /// Live rather than pipeline override constants because they WERE constants: finding the
    /// value that closes a road on Malden cost one rebuild and one redeploy per sample.
    pub fn set_road_conform(&mut self, flat: f32, per_m: f32, max_frac: f32) {
        self.fog_far[1] = flat.clamp(0.0, 1.0);
        self.fog_far[2] = per_m.clamp(0.0, 0.2);
        // Wide on purpose. The DEFAULT is bounded at 0.04 (see Engine::RoadSettings), because
        // past it a road is pulled far enough forward to draw over houses; the control itself
        // is the user's to push.
        self.fog_far[3] = max_frac.clamp(0.0, 2.0);
    }

    pub fn set_water_anim(&mut self, time: f32, flow_slot: u32, flow_strength: f32) {
        self.water_anim[0] = time;
        self.water_anim[2] = flow_strength;
        self.water_anim[3] = flow_slot as f32;
    }

    pub fn gtao_settings(&self) -> &GtaoSettings {
        &self.gtao_settings
    }

    /// Screen pixels per marched AO sample (1 = per pixel, 2 = per 2x2 block).
    pub fn gtao_scale(&self) -> u32 {
        self.gtao_scale
    }

    /// Live override for the AO march resolution, for the dev panel's AO quality control.
    /// Clamped to 1..=2: the block-replicate + bilateral-upsample argument holds for a 2x2 block,
    /// and past that the denoise can no longer reconstruct a contact shadow it never sampled.
    pub fn set_gtao_scale(&mut self, scale: u32) {
        self.gtao_scale = scale.clamp(1, 2);
    }

    pub fn gtao_debug_on(&self) -> bool {
        self.gtao_settings.enabled && self.gtao_settings.debug_mode > 0
    }

    pub fn set_gtao_settings(&mut self, s: GtaoSettings) {
        self.gtao_settings = s;
    }

    // GTAO + its bilateral denoise (screen-space-ao-plan §3/§4), recorded after the depth+normal
    // prepass and before the forward colour pass. Reads the resolved single-sample depth/normal;
    // writes the AO target the ambient terms sample.
    //
    // `camera` selects which camera's unprojection to use and MUST be the one the prepass
    // rasterised with — the depth buffer this reads is that camera's.
    pub fn render_gtao(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        camera: usize,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        let s = self.gtao_settings;
        if !s.enabled || self.gtao_force_off {
            return;
        }
        let Some(&[proj_xx, proj_yy, near]) = self.cam_gtao_proj.get(camera) else {
            return;
        };
        let (w, h) = self.depth_size;
        // Timed in three parts because they answer different questions: PREP is the fixed setup
        // cost paid before any AO exists (and is partly shared with occlusion culling), COMPUTE
        // scales with slices x steps, and BLUR scales with its radius. One combined number would
        // hide which knob to reach for.
        timers.begin(encoder, crate::gpu_timers::Region::GtaoPrep);
        // Hi-Z may have resolved the depth already this frame, but it only runs when occlusion
        // culling is on. Recording it twice would be redundant GPU work, not a correctness bug;
        // skipping it when occlusion is off would make GTAO read a stale depth buffer, which is
        // the far worse failure and would look like AO lagging the camera by a frame.
        if !self.occlusion_active() {
            self.resolve_depth_sample(encoder);
        }
        // MSAA only: reduce the prepass normal to single-sample (sample 0). No-op at 1x, where
        // GTAO was bound to the prepass normal target directly.
        if let Some(nr) = self.normal_resolve.as_ref() {
            nr.resolve(encoder);
        }
        // Linear-view-Z chain from this frame's resolved depth. Must precede the GTAO dispatch;
        // wgpu barriers the storage writes -> GTAO's textureLoads.
        //
        // `top_mip` is both what the shader is told it may climb to AND how far the chain is
        // actually built: they are the same number by construction, so the build can never
        // produce a level nothing reads (at the shipped max_mip = 0 that is the whole reduce
        // chain) nor stop short of one the march would sample.
        let top_mip = s.max_mip.min(self.gtao_depth_mips.mips().saturating_sub(1));
        if let Some(depth) = self.depth_sample_view.clone() {
            self.gtao_depth_mips
                .build(device, queue, encoder, &depth, near, top_mip);
        }
        timers.end(encoder, crate::gpu_timers::Region::GtaoPrep);
        self.gtao.upload(
            queue,
            &GtaoParams {
                proj: [proj_xx, proj_yy, near, top_mip as f32],
                screen: [
                    w as f32,
                    h as f32,
                    1.0 / w.max(1) as f32,
                    1.0 / h.max(1) as f32,
                ],
                tuning: [
                    s.radius_m.max(0.01),
                    s.strength.max(0.0),
                    s.slices.max(1) as f32,
                    s.steps.max(1) as f32,
                ],
                limits: [
                    s.max_radius_px.max(2.0),
                    s.thickness.max(0.01),
                    // Must match the `scale` handed to `dispatch` below — see its comment.
                    self.gtao_scale.max(1) as f32,
                    0.0,
                ],
            },
        );
        self.gtao_blur.upload(
            queue,
            w,
            h,
            s.blur_radius,
            s.blur_depth_scale,
            s.blur_normal_power,
        );
        timers.begin(encoder, crate::gpu_timers::Region::GtaoCompute);
        self.gtao.dispatch(encoder, w, h, self.gtao_scale);
        timers.end(encoder, crate::gpu_timers::Region::GtaoCompute);
        timers.begin(encoder, crate::gpu_timers::Region::GtaoBlur);
        self.gtao_blur.dispatch(encoder, w, h);
        timers.end(encoder, crate::gpu_timers::Region::GtaoBlur);
    }

    // Record the color-pass occlusion cull (main_occlude), reading this frame's Hi-Z. Recorded
    // after build_hiz and before the color pass. No-op unless occlusion is active.
    pub fn cull_dispatch_color(&self, encoder: &mut wgpu::CommandEncoder) {
        if !self.occlusion_active() {
            return;
        }
        self.cull.dispatch_color(encoder);
    }

    // Record one cull dispatch per active shadow cascade (§6 multi-view), producing each
    // cascade's depth-pass indirect args. Recorded before render_shadow_passes so wgpu barriers
    // the compute writes -> the depth pass's indirect reads. No-op when GPU-driven is off.
    pub fn cull_dispatch_shadows(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        if !self.gpu_driven_enabled {
            return;
        }
        // PERF-005: ONE bracket over every cascade's cull, not one per cascade. The per-cascade
        // question this instrumentation exists to answer is about the DEPTH RENDER cost
        // (ShadowCascade0..3), which is where the geometry actually rasterises; splitting the
        // cull too would spend four more query pairs to resolve a sub-0.1 ms item.
        timers.cpu_begin(crate::gpu_timers::Region::ObjCullShadow);
        timers.begin(encoder, crate::gpu_timers::Region::ObjCullShadow);
        for c in 0..self.cull.shadow_view_count() {
            self.cull.dispatch_shadow(encoder, c);
        }
        timers.end(encoder, crate::gpu_timers::Region::ObjCullShadow);
        timers.cpu_end(crate::gpu_timers::Region::ObjCullShadow);
    }

    // Draw the GPU-driven retained set into cascade `c`'s depth map, INSIDE that cascade's
    // already-open depth render pass (see render_shadow_passes). One multi_draw per pipeline
    // variant over the cascade's cull args; the shadow pass UBO (group 0) supplies this
    // cascade's light-VP via the dynamic offset. No-op when GPU-driven is off or the cascade
    // has no bind/args yet.
    fn draw_gpu_driven_shadow(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        pass_ubo_off: u32,
        c: usize,
    ) {
        self.draw_gpu_driven_depth(
            pass,
            textures,
            pass_ubo_off,
            self.gpu_shadow_group1.get(c).and_then(|b| b.as_ref()),
            self.cull.shadow_out_args(c),
            self.cull.shadow_counter_buf(c),
        );
    }

    // Depth-only GPU-driven draw for ONE view whose VP lives in the shadow pass UBO: a shadow
    // cascade, or the interior sky-visibility map. Same pipeline, same group layouts, same
    // forward-Z convention — only the pass-UBO slot, the records bind and the indirect args
    // differ, which is exactly why the sky map needed no new pipeline.
    fn draw_gpu_driven_depth(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        pass_ubo_off: u32,
        group1: Option<&wgpu::BindGroup>,
        args: Option<&wgpu::Buffer>,
        counters: Option<&wgpu::Buffer>,
    ) {
        if !self.gpu_driven_enabled {
            return;
        }
        let (Some(pass_bind), Some(group1), Some(args)) =
            (self.shadow_pass_ubo.bind.as_ref(), group1, args)
        else {
            return;
        };
        pass.set_pipeline(&self.gpu_shadow_pipeline);
        pass.set_bind_group(0, pass_bind, &[pass_ubo_off]);
        pass.set_bind_group(1, group1, &[]);
        pass.set_bind_group(2, textures.bindless_bind(), &[]);
        pass.set_bind_group(3, textures.sampler_array_bind(), &[]);
        if let Some(conform_bind) = self.conform.bind.as_ref() {
            pass.set_bind_group(4, conform_bind, &[]);
        }
        pass.set_vertex_buffer(0, self.pool.vbuf().slice(..));
        pass.set_index_buffer(self.pool.ibuf().slice(..), wgpu::IndexFormat::Uint32);
        let cap = self.cull.variant_capacity();
        if self.multi_draw_count_enabled {
            let Some(counters) = counters else {
                return;
            };
            for v in 0..cull::CULL_VARIANT_COUNT {
                let offset = v as u64 * cap as u64 * INDIRECT_ARG_SIZE;
                pass.multi_draw_indexed_indirect_count(args, offset, counters, v as u64 * 4, cap);
            }
        } else {
            for v in 0..cull::CULL_VARIANT_COUNT {
                let offset = v as u64 * cap as u64 * INDIRECT_ARG_SIZE;
                pass.multi_draw_indexed_indirect(args, offset, cap);
            }
        }
    }

    // Draw the GPU-driven opaque set into the colour pass: one multi_draw per pipeline-
    // variant partition over the compute-produced indirect args. `cam_off` selects the
    // camera UBO slot (as in draw_one). Bound once; the pool buffers are shared. No-op until
    // the retained scene has data (empty args are instance_count = 0 no-op draws).
    // GPU-driven opaque COLOUR draw (fs_gpu). Uses the OCCLUSION-culled color args when the Hi-Z
    // path is active this frame, else the main frustum-cull args (identical pre-occlusion
    // behaviour). See draw_gpu_driven_impl.
    pub fn draw_gpu_driven(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        cam_off: u32,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        let (args, group1, counters) = if self.occlusion_active() {
            (
                self.cull.color_out_args(),
                self.gpu_color_group1_bind.as_ref(),
                self.cull.color_counter_buf(),
            )
        } else {
            (
                self.cull.out_args(),
                self.gpu_group1_bind.as_ref(),
                self.cull.counter_buf(),
            )
        };
        self.draw_gpu_driven_impl(
            pass,
            textures,
            cam_off,
            self.gpu_pipeline_early.as_ref().unwrap_or(&self.gpu_pipeline),
            args,
            group1,
            counters,
            Some((
                timers,
                [
                    crate::gpu_timers::Region::ObjColorSolid,
                    crate::gpu_timers::Region::ObjColorAlpha,
                ],
            )),
        );
    }

    // Draw only the reflected view's independently culled retained opaque scene. The mirrored
    // pipeline flips its front face; the normal main-camera args and bind are never reused.
    pub fn draw_gpu_driven_reflection(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        cam_off: u32,
    ) {
        self.draw_gpu_driven_impl(
            pass,
            textures,
            cam_off,
            &self.gpu_reflection_pipeline,
            self.cull.reflection_out_args(),
            self.gpu_reflection_group1_bind.as_ref(),
            self.cull
                .reflection_counter_buf()
                .unwrap_or(self.cull.counter_buf()),
            None,
        );
    }

    // GPU-driven depth+normal PREPASS draw (fs_gpu_prepass): the MAIN (frustum-only, occluder)
    // args — the prepass generates the depth the Hi-Z is built from, so it must draw the full
    // in-frustum set, never the occlusion-culled subset. Writes depth + the view-space normal
    // G-buffer so the GPU-driven set participates in the prepass (SSAO normals, early-Z).
    pub fn draw_gpu_driven_prepass(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        cam_off: u32,
        timers: &crate::gpu_timers::GpuTimers,
    ) {
        self.draw_gpu_driven_impl(
            pass,
            textures,
            cam_off,
            &self.gpu_prepass_pipeline,
            self.cull.out_args(),
            self.gpu_group1_bind.as_ref(),
            self.cull.counter_buf(),
            Some((
                timers,
                [
                    crate::gpu_timers::Region::ObjPrepassSolid,
                    crate::gpu_timers::Region::ObjPrepassAlpha,
                ],
            )),
        );
    }

    // REN-TEMP-001T: retained-path vegetation velocity — replays the prepass'
    // in-frustum indirect stream through the velocity twin pipeline into the velocity
    // target. Non-canopy instances collapse in the VS, so the cost is dominated by
    // the vegetation actually on screen.
    #[allow(clippy::too_many_arguments)]
    pub fn render_gpu_driven_velocity(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        textures: &SharedTextures,
        cam_off: u32,
        params: &crate::temporal::GpuVegVelParams,
    ) {
        if !self.gpu_driven_enabled {
            return;
        }
        let Some(depth_view) = self.water_depth_view.clone() else {
            return;
        };
        let (Some(camera_bind), Some(group1), Some(args)) = (
            self.cameras.bind.as_ref(),
            self.gpu_group1_bind.as_ref(),
            self.cull.out_args(),
        ) else {
            return;
        };
        let Some(conform_bind) = self.conform.bind.as_ref() else {
            return;
        };
        queue.write_buffer(&self.gpu_vel_uniform, 0, bytemuck::bytes_of(params));
        if self.gpu_vel_bind.is_none() || self.gpu_vel_bound_gen != self.depth_gen {
            self.gpu_vel_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("wgr_gpu_velocity_bind"),
                layout: &self.gpu_vel_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: self.gpu_vel_uniform.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: wgpu::BindingResource::TextureView(&depth_view),
                    },
                ],
            }));
            self.gpu_vel_bound_gen = self.depth_gen;
        }
        encoder.push_debug_group("wgr_gpu_veg_velocity");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_gpu_veg_velocity"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: target,
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
        pass.set_pipeline(&self.gpu_velocity_pipeline);
        pass.set_bind_group(0, camera_bind, &[cam_off]);
        pass.set_bind_group(1, group1, &[]);
        pass.set_bind_group(2, textures.bindless_bind(), &[]);
        pass.set_bind_group(3, self.gpu_vel_bind.as_ref().expect("built above"), &[]);
        pass.set_bind_group(4, conform_bind, &[]);
        pass.set_vertex_buffer(0, self.pool.vbuf().slice(..));
        pass.set_index_buffer(self.pool.ibuf().slice(..), wgpu::IndexFormat::Uint32);
        let cap = self.cull.variant_capacity();
        let counters = self.cull.counter_buf();
        for v in 0..cull::CULL_VARIANT_COUNT {
            let offset = v as u64 * cap as u64 * INDIRECT_ARG_SIZE;
            if self.multi_draw_count_enabled {
                pass.multi_draw_indexed_indirect_count(args, offset, counters, v as u64 * 4, cap);
            } else {
                pass.multi_draw_indexed_indirect(args, offset, cap);
            }
        }
        drop(pass);
        encoder.pop_debug_group();
    }

    #[allow(clippy::too_many_arguments)]
    fn draw_gpu_driven_impl(
        &self,
        pass: &mut wgpu::RenderPass<'_>,
        textures: &SharedTextures,
        cam_off: u32,
        pipeline: &wgpu::RenderPipeline,
        args: Option<&wgpu::Buffer>,
        group1: Option<&wgpu::BindGroup>,
        counters: &wgpu::Buffer,
        // PERF-005: optional per-VARIANT timer regions, [solid, alpha-cutout], matching the
        // variant partition the loop below already draws in. Passed in rather than derived
        // because this one impl serves the prepass, the colour pass and the planar reflection,
        // and only the first two are worth four query pairs — the reflection's objects are
        // already covered end-to-end by WGR_GPU_TIMER_PLANAR_OBJECTS.
        regions: Option<(
            &crate::gpu_timers::GpuTimers,
            [crate::gpu_timers::Region; 2],
        )>,
    ) {
        if !self.gpu_driven_enabled {
            return;
        }
        let (Some(camera_bind), Some(group1), Some(args)) =
            (self.cameras.bind.as_ref(), group1, args)
        else {
            return;
        };
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, camera_bind, &[cam_off]);
        pass.set_bind_group(1, group1, &[]);
        pass.set_bind_group(2, textures.bindless_bind(), &[]);
        pass.set_bind_group(3, textures.sampler_array_bind(), &[]);
        // Group 4: terrain-conform heightmap — vs_gpu conforms ClipLand instances to SurfaceY.
        if let Some(conform_bind) = self.conform.bind.as_ref() {
            pass.set_bind_group(4, conform_bind, &[]);
        }
        pass.set_vertex_buffer(0, self.pool.vbuf().slice(..));
        pass.set_index_buffer(self.pool.ibuf().slice(..), wgpu::IndexFormat::Uint32);
        let cap = self.cull.variant_capacity();
        if self.multi_draw_count_enabled {
            // Trim the no-op tail: draw min(counter[v], cap) sub-draws per variant, the GPU
            // count buffer supplying the actual survivor count (3b-4). Avoids dispatching the
            // full conservative capacity of instance_count = 0 no-ops each frame.
            for v in 0..cull::CULL_VARIANT_COUNT {
                let offset = v as u64 * cap as u64 * INDIRECT_ARG_SIZE;
                let r = regions.and_then(|(t, rs)| rs.get(v as usize).map(|r| (t, *r)));
                if let Some((t, r)) = r {
                    t.cpu_begin(r);
                    t.begin_pass(pass, r);
                }
                pass.multi_draw_indexed_indirect_count(args, offset, counters, v as u64 * 4, cap);
                if let Some((t, r)) = r {
                    t.end_pass(pass, r);
                    t.cpu_end(r);
                }
            }
        } else {
            // Conservative fallback (e.g. Metal, no MULTI_DRAW_INDIRECT_COUNT): one multi_draw
            // of `capacity` sub-draws per variant, the unfilled tail being instance_count = 0
            // no-ops.
            for v in 0..cull::CULL_VARIANT_COUNT {
                let offset = v as u64 * cap as u64 * INDIRECT_ARG_SIZE;
                let r = regions.and_then(|(t, rs)| rs.get(v as usize).map(|r| (t, *r)));
                if let Some((t, r)) = r {
                    t.cpu_begin(r);
                    t.begin_pass(pass, r);
                }
                pass.multi_draw_indexed_indirect(args, offset, cap);
                if let Some((t, r)) = r {
                    t.end_pass(pass, r);
                    t.cpu_end(r);
                }
            }
        }
    }
}

// Convert an FFI retained instance to the GPU layout. Converted field-by-field (rather than
// transmuted) to keep the FFI + GPU structs decoupled.
fn instance_to_gpu(inst: &WgrInstance) -> cull::InstanceGpu {
    cull::InstanceGpu {
        world: inst.world,
        center: inst.center,
        model: inst.model,
        flags: inst.flags,
        cull_radius: inst.cull_radius,
        _pad: inst._pad,
        // Terrain-conform plane (conform2.z = mode); the GPU-driven VS conforms per vertex.
        conform0: inst.conform0,
        conform1: inst.conform1,
        conform2: inst.conform2,
    }
}

// Coalesce key for instanceable draws: two draws merge into one instanced draw only
// when every field draw_one reads from the WgrDraw3D (other than the per-instance
// world/conform/material, which ride the storage arrays) is identical. Texture + sampler
// are NOT here: they're bindless (indexed per-instance from the material), so same-mesh
// draws with different textures/samplers merge into one instanced draw.
#[derive(PartialEq, Eq, Hash)]
struct BucketKey {
    mesh: u64,
    index_begin: u32,
    index_count: u32,
    camera: u32,
    pipeline: PipelineKey,
}

// How a Draw3D op is submitted (docs/gpu-culling-and-depth-plan.md Stage 2).
// `IndirectEligible` marks an instanceable opaque-rigid bucket at plan_3d time;
// build_indirect then either upgrades it to `Indirect(byte_offset)` into the indirect
// args buffer (when GPU-driven indirect is on) or leaves it eligible (falls back to the
// direct draw_one path in the replay). `Direct` is a barrier draw (transparent, decal,
// skinned/baked, non-standard depth) that always goes through draw_one.
#[derive(Clone, Copy)]
pub enum DrawKind {
    Direct,
    IndirectEligible,
    Indirect(u32),
}

// One replayable step in the instancing plan (see plan_3d). Draw3D carries the repr
// draw (mesh/section/texture/pipeline) plus the base_instance range of its instances.
pub enum Plan3dOp {
    ClearDepth,
    Draw2D(u32),  // batch index
    Terrain(u32), // terrain batch index
    Water(u32),   // water batch index
    Grass(u32),   // grass batch index
    Draw3D {
        draw: u32,
        base: u32,
        count: u32,
        kind: DrawKind,
    },
    // Scene->UI seam: tonemap the HDR target to the swapchain; ops after this are
    // display-referred UI (drawn straight to the swapchain).
    Resolve,
}

// The frame's instancing plan: `order[slot]` = the draws index whose world/material
// packs into that storage slot (base_instance), and `ops` replays the stream with 3D
// runs collapsed into instanced draws. Ownership is separate from Gfx3d so it can be
// held across the &mut prepare() borrow and consumed in the render loop.
pub struct Plan3d {
    pub order: Vec<u32>,
    pub ops: Vec<Plan3dOp>,
}

// Which pass draw_one records into (docs/depth-prepass-plan.md). Prepass = the
// depth+normal G-buffer (opaque set only, self-filtered). Color = the shading pass;
// `depth_write_off` is set for the prepassed segment so its opaque set draws
// GreaterEqual/write-off over the already-complete depth.
#[derive(Clone, Copy)]
pub enum Pass3dMode {
    Prepass,
    Color { depth_write_off: bool },
}

// Bind/pipeline/buffer state already set on a render pass, so draw_one can skip
// redundant re-binds across a run of 3D draws. Reset (Default) whenever another
// pipeline runs on the pass (terrain/2D) or a new render pass begins — both invalidate
// everything tracked here.
#[derive(Default)]
pub struct Pass3dState {
    pipeline: Option<usize>, // last render pipeline (pointer identity)
    last_skinned: Option<bool>,
    cam_off: Option<u32>,
    group1_plain: bool,         // plain group-1 (world/material) currently bound
    skinned_off: Option<u32>,   // skinned group-1 palette offset currently bound
    bindless: bool,             // groups 2/3 (bindless textures + sampler array) bound
    conform: bool,              // group-4 conform heightmap currently bound
    vbuf: Option<(usize, u64)>, // vertex buffer at slot 0 (pointer identity + slice byte offset)
    ibuf: Option<usize>,        // index buffer (pointer identity)
}

// MAT-048. The layer NORMALS travel four layouts that no compiler checks against each other:
// the C ABI struct, the GPU section-material struct, and its two WGSL mirrors. Getting the
// offsets wrong does not fail — it silently feeds layer_uv from the wrong bytes, which renders
// as white buildings.
#[test]
fn multi_layer_normals_keep_every_layout_in_step() {
    use crate::ffi::WgrModelMaterial;
    // Mirrors the C++ static_assert in include/wgpu_renderer.hpp.
    assert_eq!(
        std::mem::size_of::<WgrModelMaterial>(),
        216,
        "WgrModelMaterial grew by three u64 handles (192 -> 216); wgpu_renderer.hpp must agree"
    );
    assert_eq!(
        std::mem::offset_of!(WgrModelMaterial, layer_normal_texture_id),
        128,
        "layer normals must follow layer_texture_id[3], leaving every earlier lane put"
    );
    assert_eq!(std::mem::offset_of!(WgrModelMaterial, layer_uv), 152);

    // The GPU-side struct: WGSL 16-aligns the trailing array<vec4<f32>>, Rust does not, so
    // the pad words are explicit and the offsets are the contract.
    assert_eq!(
        std::mem::offset_of!(cull::SectionMaterialGpu, layer_normal_slot),
        104
    );
    assert_eq!(
        std::mem::offset_of!(cull::SectionMaterialGpu, layer_uv),
        128
    );
    assert_eq!(std::mem::size_of::<cull::SectionMaterialGpu>(), 192);

    // Both WGSL mirrors must declare the field, in the same place, with the same padding.
    for (name, src) in [
        ("gpu_driven.wgsl", include_str!("gpu_driven.wgsl")),
        (
            "gpu_driven_shadow.wgsl",
            include_str!("gpu_driven_shadow.wgsl"),
        ),
    ] {
        assert!(
            src.contains("layer_normal_slot: array<u32, 3>,"),
            "{name} must mirror SectionMaterialGpu::layer_normal_slot"
        );
        for pad in [
            "_pad_layer0: u32,",
            "_pad_layer1: u32,",
            "_pad_layer2: u32,",
        ] {
            assert!(
                src.contains(pad),
                "{name} needs three pad words after layer_normal_slot to 16-align layer_uv"
            );
        }
    }
}

// MAT-048. The bug this pins: the albedo blended up to four layers while the normal stayed
// layer 0's for the whole surface, so a wall whose mask selected plaster wore brick relief.
#[test]
fn multi_blends_layer_normals_with_the_same_weights_as_the_colours() {
    let src = include_str!("gpu_driven.wgsl");
    // One mask sample, one weight vector, used by both blends. If the normals ever grow their
    // own weights they can disagree with the colours per fragment, which is the same class of
    // bug in a subtler form.
    assert!(
        src.contains("layer_w = lw;"),
        "the colour blend must publish its final per-layer weights for the normal blend"
    );
    assert!(
        src.contains("blended + layer * lw[i]"),
        "colours must use the renormalised weights"
    );
    assert!(
        src.contains("layer_n * layer_w[i]"),
        "layer normals must be weighted by the SAME mask weights the colours use"
    );
    // Each layer's normal at its OWN layer's uv transform -- brick tiles far denser than the
    // plaster beside it, and a normal sampled at the wrong rate is worse than none.
    assert!(
        src.contains("let uvn = in.uv * sm.layer_uv[i + 1u].xy + sm.layer_uv[i + 1u].zw;"),
        "a layer normal must use its own colour layer's uv transform"
    );
    assert!(
        src.contains("let uv0 = in.uv * sm.layer_uv[0].xy + sm.layer_uv[0].zw;"),
        "layer 0's normal must use layer 0's transform, like layer 0's colour"
    );
    // Tangent-space VECTORS, not packed texels: `_nohq` is DXT5nm (X in A, Y in G, Z
    // reconstructed), and z is not linear in the stored channels, so a mix of raw texels
    // tilts the surface wrongly exactly at the mask transitions.
    assert!(
        src.contains("fn decode_nohq(texel: vec4<f32>) -> vec3<f32>"),
        "the NOHQ decode must be factored out so every layer is unpacked BEFORE blending"
    );
    // The layer sample feeds decode_nohq directly -- nothing accumulates a raw texel.
    let layer_sample =
        "textureSampleBias(textures[sm.layer_normal_slot[i]], samplers[sm.sampler_idx], uvn, frame.renscale.x)";
    assert!(
        src.contains(layer_sample),
        "each layer normal must be sampled at its own uv"
    );
    let decoded = src
        .split_once(layer_sample)
        .map(|(head, _)| head.trim_end().ends_with("decode_nohq("))
        .unwrap_or(false);
    assert!(
        decoded,
        "each layer's normal must be decoded at its sample site, not blended packed"
    );
    assert!(
        src.contains("normal_ts = normalize(mixed);"),
        "the weighted sum of unit normals must be renormalised"
    );
    // A layer that names no normal (a procedural color(...,DT) stage) keeps layer 0's normal
    // for its share; slot 0 is the white bindless fallback and would decode leaning hard +X.
    assert!(
        src.contains("if (sm.layer_normal_slot[i] == 0u || layer_w[i] <= 0.0) {"),
        "an unbound layer normal must fall back to layer 0 rather than sample slot 0"
    );
}

#[test]
fn gtao_depth_chain_reduces_toward_the_nearest_surface() {
    let src = include_str!("gtao_depth_mips.wgsl");
    let module = naga::front::wgsl::parse_str(src).expect("gtao_depth_mips.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("gtao_depth_mips.wgsl validate");

    // The reduction DIRECTION is the whole correctness question, and it is inverted relative to
    // the Hi-Z pyramid next door. Hi-Z min-reduces REVERSED-Z, which keeps the FARTHEST surface —
    // correct for occlusion culling, which must never cull something that might be visible. This
    // chain stores LINEAR z, so the same `min` keeps the NEAREST surface, which is what a horizon
    // search wants. Reusing Hi-Z here, or storing reversed-Z here, both silently under-occlude,
    // worse at every coarser mip — it would look like AO fading out with distance rather than
    // like a bug.
    assert!(
        src.contains("m = min(m,"),
        "the chain must min-reduce (nearest surface, because it stores LINEAR z)"
    );
    assert!(
        src.contains("params.proj.x / max(d, 1e-9)"),
        "mip0 must store LINEAR view z; a reversed-Z reduction is not a depth in any useful sense"
    );
    // Sky must not be able to win the min and invent an occluder at a silhouette.
    assert!(
        src.contains("select(SKY_Z, params.proj.x / max(d, 1e-9), d > 0.0)"),
        "cleared depth must reduce to the far sentinel, not to 0"
    );

    // And the march must actually climb the chain, otherwise the whole thing is dead weight and
    // the pixel-radius clamp is back to shortening the world radius.
    let gtao = include_str!("gtao.wgsl");
    assert!(
        gtao.contains("let mip = clamp(log2(max(step_px, 1.0)) - 1.0, 0.0, f32(max_mip));"),
        "the horizon march must step up a mip with distance"
    );
    // And the level must stay CONTINUOUS. The mip a tap wants scales with camera distance, so
    // rounding it here makes the level flip as the camera moves, the sampled depth jump, and the
    // AO pop — a flicker while moving and nothing at all while still. There is no temporal filter
    // to absorb that (plan §0), so the discontinuity has to not exist rather than be smoothed
    // later. This regressed once already, between the mip march landing and this test.
    assert!(
        gtao.contains("return mix(z_lo, z_hi, f);"),
        "the march must blend between neighbouring mips, not snap to one"
    );
}

#[test]
fn gtao_bent_normal_reaches_the_ambient_term() {
    // Stage 2 is only worth anything if the bent normal actually replaces the surface normal in
    // the sky-irradiance lookup. Every link in that chain is easy to leave half-connected, and a
    // half-connected version looks exactly like "Stage 2 does not help much".
    let frame = include_str!("../shaders/frame.wgsl");
    assert!(
        frame.contains("fn gtao_bent_normal_world("),
        "frame.wgsl must expose the bent normal in world space"
    );
    // View -> world by the transpose (frame.view is a rotation with translation zeroed).
    assert!(
        frame.contains("(vec4<f32>(normalize(bent_view), 0.0) * frame.view).xyz"),
        "the bent normal must be rotated out of VIEW space before sampling world-space SH"
    );
    for (name, src) in [
        (
            "shaders/shading.wgsl",
            include_str!("../shaders/shading.wgsl"),
        ),
        (
            "terrain/terrain.wgsl",
            include_str!("../terrain/terrain.wgsl"),
        ),
    ] {
        // The bent normal must still REACH the lookup now that LIT-020 steers the same value a
        // second time: the interior term wraps it (interior_sky_ambient_normal(world, bent))
        // rather than replacing it, so both occluders compose instead of one quietly winning.
        assert!(
            src.contains("sky_irradiance(gtao_bent_normal_world(")
                || src.contains("let amb_n = gtao_bent_normal_world(")
                || (src.contains("gtao_bent_normal_world(")
                    && src.contains("interior_sky_ambient_normal(")
                    && src.contains("sky_irradiance(amb_n)")),
            "{name} must sample sky irradiance along the bent normal, not the surface normal"
        );
    }
}

// The normal map must reach the DIRECTIONAL SKY AMBIENT.
//
// Measured failure this guards (Stratis stone wall, freefly 3094.20 2138.07 170.45 169.9 -6.3):
// with the sun on the wall, toggling WGR_MATERIAL_DISABLE_NORMAL moved 42.6% of the wall's pixels
// (mean |d| 11.18, wall mean level 65); at dawn it moved 6.5% (mean |d| 2.85, level 39). The
// normal map was reaching the picture only through the sun's N.L, because the ambient direction
// came from the GTAO bent normal (a DEPTH-buffer quantity at ~2 m scale, which cannot contain
// per-pixel detail and which REPLACES the shading normal when it is on) steered further by the
// interior term. Every link below is easy to leave half-connected, and half-connected looks
// exactly like "the normals do not really work".
#[test]
fn normal_map_reaches_the_directional_sky_ambient() {
    let shading = include_str!("../shaders/shading.wgsl");
    // shade() cannot recover the map's deviation from the mapped normal alone; the caller has to
    // hand it the pre-map basis.
    assert!(
        shading.contains("geo_normal: vec3<f32>,"),
        "shade() must take the geometric (pre-normal-map) normal"
    );
    // The steer is ROTATED by the map's deviation, not replaced by it and not blended away from
    // it. A blend would dilute LIT-020's interior steer; a replacement would delete the occlusion
    // direction the bent normal exists to carry.
    assert!(
        shading.contains("fn rotate_between("),
        "the map's deviation must be applied as a rotation"
    );
    assert!(
        shading.contains("let mapped_steer = rotate_between(n_geo_amb, nrm, amb_steer);")
            && shading.contains("amb_n = normalize(mix(amb_steer, mapped_steer, amb_map_w));")
            && shading.contains("sky_irradiance(amb_n)"),
        "sky irradiance must be sampled along the steer ROTATED by the normal map's deviation"
    );
    // Directionality only. If the STEER ever starts scaling the ambient it is double-counting the
    // occlusion amb_ao applies -- the exact mistake the plan's §6 forbids. The magnitude factors
    // that ARE allowed on this line are named explicitly, so a fifth one cannot arrive unnoticed:
    // amb_ao (the three independent occluders), `cavity` (the normal map's own micro-occlusion,
    // which is an occluder and multiplies for the same reason they do) and the sky-specular
    // split, which is a reallocation between two lookups and cannot change the total.
    assert!(
        shading.contains(
            "(sky_irradiance(amb_n) * sky_spec.w + sky_spec.rgb) * frame.sun_ambient.w * amb_ao * cavity;"
        ),
        "ambient magnitude must be amb_ao x cavity and an energy-conserving specular split; \
         the steer itself stays directional only"
    );
    // Foliage keeps its own treatment: a leaf card's normal is authored, not relief, and the
    // two-sided flip above leaves the vertex normal a 180-degree reference with an arbitrary
    // rotation axis.
    assert!(
        shading
            .contains("if (amb_map_w > 0.0 && !is_foliage && geo_len > 1e-4 && n_len >= 1e-4) {"),
        "the rotation must be skipped for foliage and for degenerate normals"
    );
    // Both object paths, or a normal-mapped wall shades differently depending on which one drew
    // it -- the split this codebase keeps paying for.
    for (name, src) in [
        ("gfx3d/shader3d.wgsl", include_str!("shader3d.wgsl")),
        ("gfx3d/gpu_driven.wgsl", include_str!("gpu_driven.wgsl")),
    ] {
        assert!(
            src.contains(
                "base.rgb, m, shading_normal, geometric_normal, in.world_pos, in.fog, dwx, dwy, linear,"
            ),
            "{name} must pass the geometric normal alongside the shading normal"
        );
    }
    // And the knob has to actually be published, or the shader reads a lane nobody writes and the
    // branch is dead at weight 0.
    let frame = include_str!("../shaders/frame.wgsl");
    assert!(
        shading.contains("clamp(frame.gtao.w, 0.0, 1.0)")
            && frame.contains("WGR_AMBIENT_NORMAL_MAPPED"),
        "the weight must ride in frame.gtao.w and be documented on the struct"
    );
    // Default ON: the fix ships enabled, and 0 is the restore-the-old-look arm.
    if std::env::var("WGR_AMBIENT_NORMAL_MAPPED").is_err() {
        assert_eq!(
            ambient_normal_mapped(),
            1.0,
            "the fix must be on by default; WGR_AMBIENT_NORMAL_MAPPED=0 is the A/B arm"
        );
    }
    // ... and be announced somewhere a captured run can prove it fired. The launcher discards
    // stderr, so the line has to name the knob and go to the game log (lib.rs forwards it).
    let line = ambient_normal_mapped_log_line();
    assert!(
        line.contains("WGR_AMBIENT_NORMAL_MAPPED"),
        "the log line must name the knob, got: {line}"
    );
    let lib = include_str!("../lib.rs");
    assert!(
        lib.contains("&crate::gfx3d::ambient_normal_mapped_log_line(),")
            && !lib.contains("eprintln!(\"{}\", crate::gfx3d::ambient_normal_mapped_log_line()"),
        "the announcement must go through the LogSink (game log), not eprintln! -- the launcher \
         discards stderr, so a stderr-only line cannot prove which arm a screenshot came from"
    );
}

// Rust mirrors of the two shader terms, so their PROPERTIES can be proven over a sweep rather
// than asserted about a string. Each is pinned to the WGSL it mirrors by the tests below, which
// is what stops the mirror drifting away from the shader it claims to describe.
#[cfg(test)]
fn cavity_ref(cos_deviation: f32, k: f32) -> f32 {
    if k <= 0.0 {
        return 1.0;
    }
    (0.5 + 0.5 * cos_deviation.max(0.0)).powf(k)
}

#[cfg(test)]
fn sky_spec_fresnel_ref(ndotv: f32, gloss: f32, mask: f32, gain: f32) -> f32 {
    if gain <= 0.0 {
        return 0.0;
    }
    const F0: f32 = 0.04;
    let f_grazing = gloss.clamp(0.0, 1.0).max(F0);
    let c = 1.0 - ndotv.clamp(0.0, 1.0);
    ((F0 + (f_grazing - F0) * c.powi(5)) * mask.clamp(0.0, 1.0) * gain).clamp(0.0, 1.0)
}

// CAVITY. The mechanism that makes a crevice darker than a face when there is no sun to do it.
//
// The failure mode this guards is not "it does nothing" — it is the far more likely "it does
// something, and that something is a brightness change". The measured complaint at hour 6 is
// about CONTRAST at a wall mean level of 38.8, so a term that lifts or drops the mean is a
// regression even if the relief improves, and a term that can exceed 1.0 anywhere is a term that
// invents light out of a texture.
#[test]
fn normal_map_cavity_only_darkens_the_ambient_and_is_inert_on_a_flat_map() {
    // 1. Never brighter than 1, monotone in the deviation, and EXACTLY 1 on a flat normal map at
    //    every strength — the guarantee that an unmapped or normal-map-less surface is
    //    bit-identical whatever the knob is set to.
    for k in [0.0f32, 0.5, 1.0, 2.0, 4.0, 8.0] {
        assert_eq!(
            cavity_ref(1.0, k),
            1.0,
            "a flat normal map (deviation 0) must be exactly 1.0 at strength {k}"
        );
        let mut prev = 1.0f32;
        for step in 0..=90 {
            let t = (step as f32).to_radians();
            let v = cavity_ref(t.cos(), k);
            assert!(
                (0.0..=1.0).contains(&v),
                "cavity must stay in [0,1]: {v} at {step} deg, k = {k}"
            );
            assert!(
                v <= prev + 1e-6,
                "cavity must fall monotonically with deviation: {v} > {prev} at {step} deg"
            );
            prev = v;
        }
    }
    // 2. k = 0 is the OFF arm and is exactly 1 at every deviation, so WGR_NORMAL_CAVITY=0 restores
    //    the previous look bit-for-bit rather than approximately.
    for step in 0..=90 {
        let t = (step as f32).to_radians();
        assert_eq!(cavity_ref(t.cos(), 0.0), 1.0, "k = 0 must be a hard no-op");
    }
    // 3. The shipped default has to be worth shipping AND cheap on the mean. At a typical `_nohq`
    //    deviation (~20 deg) it costs a few percent of the ambient; at a real crevice edge
    //    (45-60 deg) it is a fifth to a third. Those two numbers together are the whole design:
    //    contrast concentrated on the steep pixels, mean barely moved.
    let d = normal_cavity();
    let typical = cavity_ref(20.0f32.to_radians().cos(), d);
    let steep = cavity_ref(50.0f32.to_radians().cos(), d);
    assert!(
        (0.90..=0.98).contains(&typical),
        "the default must cost only a few percent on a typical texel, got {typical}"
    );
    assert!(
        steep < 0.80,
        "the default must visibly darken a steep facet (>20%), got {steep}"
    );
    // 4. And it must be the AMBIENT it multiplies, on both object branches and on terrain — never
    //    the sun (which already has N.L and would over-darken) and never as an addition.
    let shading = include_str!("../shaders/shading.wgsl");
    assert!(
        shading
            .contains("let cavity = select(normal_map_cavity(geo_normal, nrm), 1.0, is_foliage);"),
        "shade() must derive the cavity from the geometric/mapped normal pair, foliage exempt"
    );
    assert!(
        shading.contains("* amb_ao * cavity;")
            && shading
                .contains("m_sun_ambient * amb_ao * cavity + m_sun_diffuse * ndotl * sun_vis;"),
        "the cavity must multiply the ambient on BOTH the sky-lit and the legacy branch"
    );
    assert!(
        !shading.contains("ndotl * sun_vis * cavity") && !shading.contains("cavity * sun_vis"),
        "the cavity must never touch the direct sun term"
    );
    let terrain = include_str!("../terrain/terrain.wgsl");
    assert!(
        terrain.contains("* normal_map_cavity(geometric_n, n);"),
        "terrain must take the same term on its ambient — the dawn ground is flat for the same \
         reason the dawn wall is"
    );
    // 5. The shader must be the maths the mirror above claims it is.
    let frame = include_str!("../shaders/frame.wgsl");
    assert!(
        frame.contains("return pow(0.5 + 0.5 * c, k);")
            && frame.contains("let c = max(dot(geo_n * inverseSqrt(g2), n), 0.0);")
            && frame.contains("let k = frame.relief.x;"),
        "normal_map_cavity must be the half-space term (1 + cos)/2 raised to the knob, SATURATED \
         (not bailed out of) past the horizon — a bail leaves a bright step at 90 degrees"
    );
}

// SKY SPECULAR. The second per-pixel mechanism, and the one with a standing constraint attached:
// the owner's complaint is that sunlit walls blow out, so a term that ADDS ambient light is not
// acceptable however good the relief is. It is therefore built as a reallocation, and the
// property that makes that true — diffuse weight + specular weight == 1 — is proven here rather
// than asserted in a comment.
#[test]
fn sky_specular_reallocates_the_ambient_and_cannot_add_energy() {
    for &gloss in &[0.0f32, 0.25, 0.55, 0.83, 1.0] {
        for &mask in &[0.0f32, 0.3, 1.0] {
            for step in 0..=90 {
                let ndotv = (step as f32).to_radians().cos();
                let f = sky_spec_fresnel_ref(ndotv, gloss, mask, sky_specular());
                assert!(
                    (0.0..=1.0).contains(&f),
                    "the split weight must stay in [0,1]: {f}"
                );
                // The caller computes `diffuse * (1 - f) + spec * f`. If both lookups returned
                // the same radiance L, the result must be exactly L — that is what "cannot add
                // energy" means, and it is what keeps the wall's mean level where it was.
                let l = 42.0f32;
                let out = l * (1.0 - f) + l * f;
                assert!(
                    (out - l).abs() < 1e-4,
                    "the split must conserve energy: {out} != {l}"
                );
            }
        }
    }
    // Roughness ceiling. Schlick run to 1.0 at grazing paints a bright Fresnel rim on every
    // silhouette in the scene; Lazarov's max(gloss, F0) ceiling is what keeps a rough surface
    // rough. A material with NO authored specular (gloss 0) must stay at the flat dielectric 4%.
    let flat = sky_spec_fresnel_ref(0.0, 0.0, 1.0, 1.0);
    assert!(
        (flat - 0.04).abs() < 1e-4,
        "a gloss-0 material must keep a flat 4% reallocation even at grazing, got {flat}"
    );
    let glossy = sky_spec_fresnel_ref(0.0, 0.83, 1.0, 1.0);
    assert!(
        glossy > 0.7 && glossy <= 0.84,
        "a glossy material must reach most of its grazing reflectance, got {glossy}"
    );
    // Head-on it is 4% regardless of gloss: this is a GRAZING-angle mechanism and must not be
    // sold as anything else.
    for &gloss in &[0.0f32, 0.55, 1.0] {
        let head_on = sky_spec_fresnel_ref(1.0, gloss, 1.0, 1.0);
        assert!(
            (head_on - 0.04).abs() < 1e-4,
            "head-on reflectance must be F0 at gloss {gloss}, got {head_on}"
        );
    }
    // Gain 0 is a hard no-op: (0,0,0,1) leaves the caller's line bit-identical.
    assert_eq!(sky_spec_fresnel_ref(0.0, 1.0, 1.0, 0.0), 0.0);

    // Wiring. It must be driven by the MAPPED normal (or it has no per-pixel content at all),
    // sample the MIRROR direction (or it is just a second diffuse lookup), and ride inside the
    // same occlusion as the diffuse sky (or crevices acquire glints).
    let frame = include_str!("../shaders/frame.wgsl");
    assert!(
        frame.contains("let mirror = reflect(-view_dir, n);")
            && frame.contains("return vec4<f32>(sky_irradiance(mirror) * f, 1.0 - f);"),
        "the specular must sample the mirror direction and return its own complement as the \
         diffuse weight"
    );
    assert!(
        frame.contains("let f_grazing = max(clamp(gloss, 0.0, 1.0), SKY_SPEC_F0);"),
        "the grazing term must be ceilinged by gloss (Lazarov), not run to a mirror"
    );
    let shading = include_str!("../shaders/shading.wgsl");
    assert!(
        shading.contains("nrm, view_dir_sky, spec_power_to_gloss(mat.spec_power), mat.spec_mask);"),
        "the split must be driven by the MAPPED normal and the material's own gloss/spec mask"
    );
    assert!(
        shading.contains("(sky_irradiance(amb_n) * sky_spec.w + sky_spec.rgb) * frame.sun_ambient.w * amb_ao * cavity;"),
        "the specular must sit inside the same amb_ao x cavity occlusion as the diffuse sky"
    );
    // Terrain deliberately does NOT take it — a null result, recorded so a later reader does not
    // "fix" the omission. Ground has no material and therefore gloss 0, so it would buy the bare
    // 4% reallocation for a reflect() plus a second SH evaluation on the frame's heaviest fill.
    let terrain = include_str!("../terrain/terrain.wgsl");
    assert!(
        !terrain.contains("sky_specular_split"),
        "terrain must not pay for a sky specular it has no gloss to drive"
    );
}

// shader3d.wgsl and gpu_driven.wgsl draw the SAME sections -- which one runs depends only on
// whether a model registered for GPU-driven rendering -- so a difference between them renders the
// same geometry two ways for a reason unrelated to how it should look.
//
// This pins the one that actually diverged. Under alpha_to_coverage the fragment's alpha IS its
// sample mask, so an OPAQUE section (alpha_ref 0) must be forced to full coverage and must not
// reach the sharpener: a2c_coverage(a, 0) is not 1 for the many legacy textures whose alpha
// channel carries something other than coverage, and the result is holes punched in solid
// geometry. gpu_driven.wgsl had the guard from the start; shader3d.wgsl did not, and nothing
// noticed because both compile and both look plausible.
#[test]
fn both_object_shaders_force_full_coverage_on_opaque_sections() {
    for (name, src, aref) in [
        ("shader3d.wgsl", include_str!("shader3d.wgsl"), "alpha_ref"),
        (
            "gpu_driven.wgsl",
            include_str!("gpu_driven.wgsl"),
            "sm.alpha_ref",
        ),
    ] {
        let at = src
            .find("if (a2c > 0.5) {")
            .unwrap_or_else(|| panic!("{name} has no alpha-to-coverage branch"));
        let window = &src[at..(at + 2400).min(src.len())];
        assert!(
            window.contains(&format!("if ({aref} > 0.0) {{")),
            "{name} must gate the coverage sharpener on a cutout alpha reference"
        );
        assert!(
            window.contains("out_a = 1.0;"),
            "{name} must force full coverage on opaque sections; without it              alpha_to_coverage turns a non-coverage alpha channel into holes"
        );
    }
}

// The knobs must reach the GPU and be announced. `relief` is the LAST lane of the camera UBO and
// the two lanes before it are addressed BACKWARDS from bind_size, so appending it moved both —
// exactly the kind of edit that silently writes the water animation over the material-debug lane
// and leaves a debug view stuck on.
#[test]
fn normal_relief_knobs_reach_the_shader_and_the_game_log() {
    let frame = include_str!("../shaders/frame.wgsl");
    assert!(
        frame.contains("WGR_NORMAL_CAVITY") && frame.contains("WGR_SKY_SPECULAR"),
        "both knobs must be documented on the struct they ride in"
    );
    // The three trailing lanes, in order, each at its own offset. Pinned as a set: any one of
    // them alone looks right.
    let src = include_str!("mod.rs");
    for (offset, payload) in [
        ("bind_size - 80", "cast_slice(&matdbg)"),
        ("bind_size - 64", "cast_slice(&self.water_anim)"),
        ("bind_size - 48", "cast_slice(&relief)"),
        ("bind_size - 32", "cast_slice(&self.fog_far)"),
        ("bind_size - 16", "cast_slice(&self.render_scale_lane)"),
    ] {
        let at = src
            .find(offset)
            .unwrap_or_else(|| panic!("no camera-UBO write at {offset}"));
        let window = &src[at..(at + 200).min(src.len())];
        assert!(
            window.contains(payload),
            "the write at {offset} must carry {payload}; the three trailing lanes are addressed \
             backwards from bind_size, so appending one moves the others"
        );
    }
    // ...and the WGSL must declare them in that same order, or the offsets address the wrong
    // fields with no error anywhere.
    let matdbg_at = frame.find("    matdbg: vec4<f32>,").expect("matdbg lane");
    let water_at = frame
        .find("    wateranim: vec4<f32>,")
        .expect("wateranim lane");
    let relief_at = frame.find("    relief: vec4<f32>,").expect("relief lane");
    let fogfar_at = frame.find("    fogfar: vec4<f32>,").expect("fogfar lane");
    let renscale_at = frame
        .find("    renscale: vec4<f32>,")
        .expect("renscale lane");
    assert!(
        matdbg_at < water_at
            && water_at < relief_at
            && relief_at < fogfar_at
            && fogfar_at < renscale_at,
        "the WGSL lane order must match the upload offsets"
    );
    // Defaults: both ON, because both are constructed so they cannot brighten anything — the
    // cavity only multiplies and the specular only reallocates. 0 is the A/B arm for each.
    if std::env::var("WGR_NORMAL_CAVITY").is_err() {
        assert_eq!(normal_cavity(), 2.0, "cavity ships on; 0 is the A/B arm");
    }
    if std::env::var("WGR_SKY_SPECULAR").is_err() {
        assert_eq!(
            sky_specular(),
            1.0,
            "sky specular ships on; 0 is the A/B arm"
        );
    }
    // Announced through the GAME log naming both knobs, for the same reason as the ambient
    // normal-mapping line: the launcher discards stderr, so a stderr-only announcement cannot be
    // matched to a captured dawn screenshot.
    let line = normal_relief_log_line();
    assert!(
        line.contains("WGR_NORMAL_CAVITY") && line.contains("WGR_SKY_SPECULAR"),
        "the log line must name both knobs, got: {line}"
    );
    let lib = include_str!("../lib.rs");
    assert!(
        lib.contains("log.log(log_level::INFO, &crate::gfx3d::normal_relief_log_line());")
            && !lib.contains("eprintln!(\"{}\", crate::gfx3d::normal_relief_log_line()"),
        "the announcement must go through the LogSink (game log), not eprintln!"
    );
}

// WGR_ROAD_PIXEL_CONFORM: the road's DEPTH is re-seated on the terrain per FRAGMENT, on a
// separate entry point that only OnSurface (Offset::Decal) plain-path pipelines use. Guards the
// four ways this silently stops working: fs_main growing a frag_depth (early-Z off for every
// draw), fs_surface losing its heightmap taps, the pipeline selection drifting to some other
// key, and the heightmap group not being visible to the fragment stage at all.
#[test]
fn road_pixel_conform_reseats_onsurface_depth_per_fragment() {
    let shader = include_str!("shader3d.wgsl");
    // The plain colour fragment keeps fixed-function depth (early-Z on).
    assert!(
        shader.contains("fn fs_main(in: VsOut) -> @location(0) vec4<f32> {"),
        "fs_main must not write frag_depth; only the OnSurface twin does"
    );
    // The OnSurface twin writes frag_depth from the SAME height source the vertex conform
    // reads (surface_y_raster), walked along the view ray, on top of the vertex decal bias.
    let fs_surface = shader
        .find("fn fs_surface(in: VsOut) -> SurfaceOut {")
        .expect("fs_surface");
    let depth_fn = shader
        .find("fn surface_pixel_depth(in: VsOut) -> f32 {")
        .expect("surface_pixel_depth");
    assert!(
        fs_surface < depth_fn,
        "fs_surface must precede its depth helper in the source"
    );
    let depth_body = &shader[depth_fn..];
    let depth_body = &depth_body[..depth_body
        .find("\nfn shade_fragment")
        .expect("shade_fragment follows")];
    for needle in [
        "@builtin(frag_depth) depth: f32,",
        "surface_y_raster(p.xz + cam.xz, cam) - cam.y",
        "if (w <= 0.0 || hm_params.enabled < 0.5 || dist <= 1e-3) {",
        "let t = clamp(hy / dir.y, 0.5 * dist, 1.5 * dist);",
        "ndc = ndc + depth_bias * ndc * ndc;",
        "return mix(in.clip.z, clamp(ndc, 0.0, 1.0), w);",
    ] {
        assert!(
            shader.contains(needle) && (needle.starts_with('@') || depth_body.contains(needle)),
            "surface_pixel_depth must contain `{needle}`"
        );
    }
    // The pull is ALONG THE RAY toward the camera, not a vertical lift: looking up at a road
    // on a hillside, a vertical lift moves the point AWAY under reversed-Z and the terrain wins.
    assert!(
        depth_body
            .contains("let pulled = p * max(seated_dist - lift, 1e-3) / max(seated_dist, 1e-3);"),
        "the lift must move the seated point toward the camera along its view ray"
    );
    // The vertex path tells the fragment which vertices were pinned; skinned draws never are.
    assert!(
        shader.contains("@location(7) conform_w: f32,")
            && shader.contains("conform_w = 1.0;")
            && shader.contains("vec3<f32>(0.0), vec3<f32>(0.0), uv, instance, 0.0);"),
        "conform_w must be 1 for pinned (conform_sel 2) vertices and 0 for skinned draws"
    );
    // Pipeline selection: Offset::Decal, plain path, knob on -> fs_surface; nothing else.
    let this = include_str!("mod.rs");
    assert!(
        this.contains("let fs_entry = if key.offset == Offset::Decal && !key.skinned && road_pixel_conform() {")
            && this.contains("entry_point: Some(fs_entry),"),
        "only OnSurface plain-path colour pipelines may take the frag_depth entry point"
    );
    // The prepass never sees a Decal draw, so its fs_prepass twin needs no depth agreement.
    assert!(
        this.contains("&& self.offset == Offset::None") && this.contains("(\"depth_bias\", 0.0),"),
        "prepassed() must keep excluding Offset::Decal, or the prepass depth would disagree"
    );
    // The heightmap group must be visible to the fragment stage, or wgpu rejects the pipeline.
    let conform_layout = this
        .find("label: Some(\"wgr_3d_conform_layout\"),")
        .expect("conform layout");
    let conform_layout = &this[conform_layout..conform_layout + 1200];
    assert_eq!(
        conform_layout
            .matches("visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,")
            .count(),
        2,
        "both conform bindings (heightmap + params) must be VERTEX_FRAGMENT"
    );
    // The lift constant is published to the shader.
    assert!(
        this.contains("(\"pixel_lift\", road_pixel_lift() as f64),")
            && shader.contains("override pixel_lift: f32 = 0.02;"),
        "WGR_ROAD_PIXEL_LIFT must reach the pixel_lift override"
    );
    // Default ON; 0 is the vertex-only A/B arm.
    if std::env::var("WGR_ROAD_PIXEL_CONFORM").is_err() {
        assert!(
            road_pixel_conform(),
            "the fix ships on; WGR_ROAD_PIXEL_CONFORM=0 is the A/B arm"
        );
    }
    if std::env::var("WGR_ROAD_PIXEL_LIFT").is_err() {
        assert_eq!(
            road_pixel_lift(),
            0.02,
            "a few centimetres, not metres: the owner has seen roads through mountains"
        );
    }
    // Announced through the GAME log (queued in Gfx3d::new, drained by render_frame), naming
    // the knob, so a captured screenshot can be matched to its arm. Not stderr.
    let line = road_pixel_conform_log_line();
    assert!(
        line.contains("WGR_ROAD_PIXEL_CONFORM"),
        "the log line must name the knob, got: {line}"
    );
    assert!(
        this.contains("queued_log: vec![road_pixel_conform_log_line()],")
            && !this.contains("eprintln!(\"{}\", road_pixel_conform_log_line()"),
        "the announcement must ride the queued game log, not eprintln!"
    );
    let lib = include_str!("../lib.rs");
    assert!(
        lib.contains("for line in self.gfx3d.take_queued_log() {"),
        "render_frame must still drain the queued log into the LogSink"
    );
}

// The interior steer must WRAP the bent normal, not discard it. Written separately from the test
// above because the failure it guards is the opposite one: a later edit that drops
// gtao_bent_normal_world and passes the raw surface normal into the interior steer would still
// satisfy "the interior term is wired up" while silently deleting the screen-space term.
#[test]
fn interior_sky_steer_composes_with_the_bent_normal() {
    for (name, src) in [
        (
            "shaders/shading.wgsl",
            include_str!("../shaders/shading.wgsl"),
        ),
        (
            "terrain/terrain.wgsl",
            include_str!("../terrain/terrain.wgsl"),
        ),
    ] {
        assert!(
            src.contains("interior_sky_ambient_normal(")
                && !src.contains("interior_sky_ambient_normal(world_abs, nrm)")
                && !src
                    .contains("interior_sky_ambient_normal(in.world_pos + frame.cam_pos.xyz, n)"),
            "{name} must feed the BENT normal into the interior steer, not the raw surface normal"
        );
    }
    let frame = include_str!("../shaders/frame.wgsl");
    // The steer is what turns visibility into direction; without the reach weighting it is just
    // an expensive way to return the normal.
    assert!(
        frame.contains("interior_sky_reach_dir(world_abs, n, i) * facing"),
        "the steered direction must be weighted by each direction's own visibility"
    );
}

// Test-only access to the shared group(0) camera layout.
//
// Modules that bind it (terrain, grass, the far tier) can only build their REAL pipelines in
// a headless test if they can get the REAL layout: a hand-copied stand-in would pass while
// silently drifting from the thing production uses, which is the precise failure mode a
// device test exists to rule out.
#[cfg(test)]
pub(crate) fn camera_layout_for_tests(device: &wgpu::Device) -> wgpu::BindGroupLayout {
    CameraGroup::new(device).layout
}

#[cfg(test)]
mod staging_tests {
    use super::{StorageArray, is_power_of_ten};

    // The bug this guards: `ensure` caches the size it ASKED for, so once a `create_buffer`
    // comes back invalid (which is how wgpu reports out of memory) the same poisoned handle is
    // handed out for the rest of the process and every upload into it is refused. `invalidate`
    // is the only thing that lets the next frame try again — without it the second `ensure`
    // below returns false and nothing is ever rebuilt.
    #[test]
    fn invalidate_forces_the_next_ensure_to_rebuild() {
        let Some((device, _queue)) = crate::gfx3d::cull::tests::headless() else {
            return; // no GPU adapter in this environment
        };
        let mut array = StorageArray::new("wgr_test_storage");
        assert!(array.ensure(&device, 4096), "first ensure must create");
        assert!(
            !array.ensure(&device, 4096),
            "a satisfied capacity must not recreate"
        );
        array.invalidate();
        assert!(array.buf.is_none(), "invalidate must drop the handle");
        assert_eq!(
            array.cap, 0,
            "invalidate must forget the asked-for capacity"
        );
        assert!(
            array.ensure(&device, 4096),
            "after invalidate the next ensure must rebuild"
        );
        assert!(array.buf.is_some());
    }

    #[test]
    fn refusal_log_throttle_fires_on_decades_only() {
        let fired: Vec<u64> = (1..=1000).filter(|&n| is_power_of_ten(n)).collect();
        assert_eq!(fired, vec![1, 10, 100, 1000]);
        assert!(!is_power_of_ten(0));
    }
}

// REN-TEMP-001H/I: both velocity-object pipelines (rigid + skinned) are lazily built at
// runtime; this builds them on a real device so a WGSL/BGL mismatch fails HERE, not on
// the first frame someone enables WGR_TEMPORAL in-game. Skips silently with no adapter.
#[test]
fn velocity_obj_pipelines_build_on_a_real_device() {
    let instance =
        wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::default(),
        compatible_surface: None,
        force_fallback_adapter: false,
    })) else {
        return;
    };
    let Ok((device, _queue)) =
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))
    else {
        return;
    };
    let _pass = VelocityObjPass::new(&device);
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
}

// The 2026-08-30 lesson, pinned: skin_bake.wgsl and velocity_obj.wgsl hard-code the
// WgrMeshVertex word count in WGSL, where no compiler can check it against the Rust
// struct. When the vertex grew 9 -> 17 words, the bake silently wrote interleaved
// garbage for months of commits (default-off hid it), and the hunt cost a day. Any
// future vertex-layout change now fails HERE first, naming both files to update.
#[cfg(test)]
mod baked_vertex_stride_guard {
    #[test]
    fn wgsl_strides_match_the_rust_vertex() {
        let words = std::mem::size_of::<crate::ffi::WgrMeshVertex>() / 4;
        let tag = format!("WORDS_PER_VERT: u32 = {words}u");
        assert!(
            include_str!("skin_bake.wgsl").contains(&tag),
            "skin_bake.wgsl WORDS_PER_VERT != WgrMeshVertex ({words} words) — update the \
             shader's layout (positions/normals/uv/conform/tangent/binormal/uv1) too"
        );
        let vel_tag = format!("* {words}u");
        assert!(
            include_str!("velocity_obj.wgsl").contains(&vel_tag),
            "velocity_obj.wgsl prev_baked stride != WgrMeshVertex ({words} words)"
        );
    }
}

// ---- Sky-bake derived-data cache (module-level; no Gfx3d state) ---------------------------

// Bump when the bake output format or semantics change; old files are simply misses.
const SKY_BAKE_CACHE_VERSION: u32 = 1;

fn sky_bake_cache_enabled() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("WGR_SKY_BAKE_CACHE").map(|v| v != "0").unwrap_or(true))
}

fn sky_bake_cache_dir() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("LOCALAPPDATA")?;
    let dir = std::path::Path::new(&base).join("OpenPoseidon").join("skybake");
    std::fs::create_dir_all(&dir).ok()?;
    Some(dir)
}

fn sky_bake_cache_path(name: &str, lo: &[f32; 3], hi: &[f32; 3]) -> Option<std::path::PathBuf> {
    if !sky_bake_cache_enabled() {
        return None;
    }
    // FNV-1a over the lowered name + the lod0 bbox bits + the format version.
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    let mut eat = |b: u8| {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    };
    for b in name.bytes() {
        eat(b.to_ascii_lowercase());
    }
    for v in lo.iter().chain(hi.iter()) {
        for b in v.to_le_bytes() {
            eat(b);
        }
    }
    for b in SKY_BAKE_CACHE_VERSION.to_le_bytes() {
        eat(b);
    }
    Some(sky_bake_cache_dir()?.join(format!("{h:016x}.skv")))
}

fn sky_bake_cache_read(path: &std::path::Path) -> Option<(Vec<[f32; 4]>, [f32; 3], [f32; 3])> {
    let data = std::fs::read(path).ok()?;
    if data.len() < 16 || &data[0..4] != b"SKV1" {
        return None;
    }
    let count = u32::from_le_bytes(data[4..8].try_into().ok()?) as usize;
    let expected = 16 + 24 + count * 16;
    if data.len() != expected {
        return None;
    }
    let mut bmin = [0f32; 3];
    let mut bmax = [0f32; 3];
    let mut off = 16;
    for v in bmin.iter_mut().chain(bmax.iter_mut()) {
        *v = f32::from_le_bytes(data[off..off + 4].try_into().ok()?);
        off += 4;
    }
    let mut vis = Vec::with_capacity(count);
    for _ in 0..count {
        let mut e = [0f32; 4];
        for v in e.iter_mut() {
            *v = f32::from_le_bytes(data[off..off + 4].try_into().ok()?);
            off += 4;
        }
        vis.push(e);
    }
    Some((vis, bmin, bmax))
}

fn sky_bake_cache_write(path: &std::path::Path, vis: &[[f32; 4]], bmin: &[f32; 3], bmax: &[f32; 3]) {
    let mut data = Vec::with_capacity(16 + 24 + vis.len() * 16);
    data.extend_from_slice(b"SKV1");
    data.extend_from_slice(&(vis.len() as u32).to_le_bytes());
    data.extend_from_slice(&[0u8; 8]); // reserved
    for v in bmin.iter().chain(bmax.iter()) {
        data.extend_from_slice(&v.to_le_bytes());
    }
    for e in vis {
        for v in e {
            data.extend_from_slice(&v.to_le_bytes());
        }
    }
    // Write-then-rename so a crash mid-write can never leave a torn file a later run trusts.
    // The temp name must be PRIVATE to this writer: `path.with_extension("tmp")` is one name
    // for every process baking this key, so two writers interleave into a single file and the
    // rename then publishes the mixture -- which is the exact failure the rename was added to
    // prevent. pid plus a process-local counter makes each attempt its own file.
    static TMP_SEQ: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let seq = TMP_SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let tmp = path.with_extension(format!("tmp{}-{}", std::process::id(), seq));
    if std::fs::write(&tmp, &data).is_ok() && std::fs::rename(&tmp, path).is_err() {
        // A private temp that never got published is litter, not a cache entry.
        let _ = std::fs::remove_file(&tmp);
    }
}

// REN-TEMP-002, pinned: a bake-covered draw routes through the RIGID pipeline whose
// vertices are ALREADY world-transformed (the C++ palette premultiply), so its world
// SSBO entry must be identity. This decision lived inline in prepare() and was once
// left disabled (`false && bake_on`) when the draw routing it must mirror was
// reconnected — draws applied the world twice and every skinned character vanished
// off-screen under DLSS. These tests hold the seam so routing and packing cannot
// disagree silently again.
#[cfg(test)]
mod baked_world_packing {
    use super::*;

    fn draw(mesh: u64, palette_slot: u32) -> WgrDraw3D {
        // WgrDraw3D is ABI-frozen plain data; zeroed + the fields under test.
        let mut d: WgrDraw3D = unsafe { std::mem::zeroed() };
        d.mesh = mesh;
        d.palette_slot = palette_slot;
        d.world = [
            2.0, 0.0, 0.0, 0.0, //
            0.0, 2.0, 0.0, 0.0, //
            0.0, 0.0, 2.0, 0.0, //
            5.0, 6.0, 7.0, 1.0,
        ];
        d.conform0 = [1.0, 2.0, 3.0, 4.0];
        d
    }

    fn base_map(entries: &[(u32, u64)]) -> FxHashMap<(u32, u64), u32> {
        entries.iter().map(|&(s, m)| ((s, m), 0u32)).collect()
    }

    #[test]
    fn a_bake_covered_draw_packs_identity_world_and_no_conform() {
        let d = draw(7, 3);
        let obj = object_gpu_for_draw(&d, true, &base_map(&[(3, 7)]));
        assert_eq!(obj.world, IDENTITY_MAT4, "baked verts already carry the world");
        assert_eq!(obj.conform0, [0.0; 4], "conform is folded pre-bake too");
    }

    #[test]
    fn an_uncovered_skinned_draw_keeps_its_real_world() {
        // Skinned but not planned by prepare_skin_bake (e.g. bake map miss): the
        // VS-skinning fallback ignores the object world, but routing (baked_draw_base)
        // answers None for it, so the SSBO entry must stay the draw's own.
        let d = draw(7, 3);
        let obj = object_gpu_for_draw(&d, true, &base_map(&[(3, 8)]));
        assert_eq!(obj.world, d.world);
        assert_eq!(obj.conform0, d.conform0);
    }

    #[test]
    fn bake_off_packs_the_real_world_even_with_a_stale_map() {
        let d = draw(7, 3);
        let obj = object_gpu_for_draw(&d, false, &base_map(&[(3, 7)]));
        assert_eq!(obj.world, d.world);
    }

    #[test]
    fn a_rigid_draw_is_never_treated_as_baked() {
        let d = draw(7, NO_PALETTE);
        let obj = object_gpu_for_draw(&d, true, &base_map(&[(NO_PALETTE, 7)]));
        assert_eq!(obj.world, d.world);
    }

    // The key is (palette_slot, mesh), not palette_slot alone: one skeleton pose is
    // drawn as several sibling meshes, and keying by slot only once corrupted every
    // sibling (see prepare_skin_bake). Pin the compound key.
    #[test]
    fn sibling_meshes_on_one_palette_slot_are_keyed_independently() {
        let body = draw(7, 3);
        let head = draw(9, 3);
        let map = base_map(&[(3, 7)]);
        assert_eq!(object_gpu_for_draw(&body, true, &map).world, IDENTITY_MAT4);
        assert_eq!(object_gpu_for_draw(&head, true, &map).world, head.world);
    }
}
