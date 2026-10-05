use std::ffi::c_void;
use std::os::raw::c_char;
use std::panic::{AssertUnwindSafe, catch_unwind};

use crate::Renderer;
use crate::log::{LogSink, log_level};
use crate::textures::TextureFormat;

pub type WgrVec2 = glam::Vec2;
pub type WgrVec3 = glam::Vec3;
pub type WgrVec4 = [f32; 4];
pub type WgrMat4 = [f32; 16];

#[repr(C)]
pub struct WgrSlice<T> {
    pub data: *const T,
    pub len: u32,
}

impl<T> WgrSlice<T> {
    /// # Safety
    /// `data` must be null (only when `len` is 0) or point to at least `len`
    /// elements of `T` that outlive the returned slice.
    unsafe fn as_slice<'a>(&self) -> &'a [T] {
        if self.data.is_null() || self.len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.data, self.len as usize) }
        }
    }
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)]
pub enum WgrPlatform {
    Win32 = 0,
    Xlib = 1,
    Wayland = 2,
}

#[repr(C)]
pub struct WgrSurfaceDesc {
    pub platform: WgrPlatform,
    pub window: *mut c_void,
    pub display: *mut c_void,
    pub width: u32,
    pub height: u32,
}

#[repr(C)]
pub struct WgrLogCallbacks {
    pub log: Option<extern "C" fn(level: i32, msg: *const c_char, user: *mut c_void)>,
    pub user: *mut c_void,
}

#[repr(C)]
pub struct WgrAbiCheck {
    pub abi_version: u32,
    pub struct_size: u32,
    pub surface_desc_size: u32,
    pub log_callbacks_size: u32,
    pub frame_size: u32,
    pub required_features: u32,
    /// FNV-1a over the size of every struct that crosses this boundary — see
    /// `wgr_layout_hash()` and the identical list in `wgpu_renderer.hpp`.
    ///
    /// The three explicit sizes above cover only the handshake's own arguments and
    /// the frame descriptor, so a shared struct could grow on one side and the pair
    /// still shook hands. That is not hypothetical: `WgrSkyRuntime` went from 5 vec4
    /// to 7 (80 -> 112 bytes) when the moon disc gained `moon_params` + `moon_sun`,
    /// and an engine binary from before that change kept passing this check while
    /// this crate read the two new lanes off the end of an 80-byte stack object.
    pub layout_hash: u32,
}

const WGR_ABI_FEATURE_BUILD_ID: u32 = 0x0000_0001;
const WGR_ABI_FEATURE_SAFE_DIAGNOSTICS: u32 = 0x0000_0002;
const WGR_ABI_FEATURE_RUNTIME_CAPABILITIES: u32 = 0x0000_0004;
const WGR_ABI_FEATURE_ANALYTIC_GLOW: u32 = 0x0000_0008;
const WGR_ABI_SUPPORTED_FEATURES: u32 = WGR_ABI_FEATURE_BUILD_ID
    | WGR_ABI_FEATURE_SAFE_DIAGNOSTICS
    | WGR_ABI_FEATURE_RUNTIME_CAPABILITIES
    | WGR_ABI_FEATURE_ANALYTIC_GLOW;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrVertex2D {
    // pos.x/y = window pixels, pos.z = depth.
    pub pos: WgrVec3,
    pub rhw: f32,
    // 0..1 fog, negative HDR gain, 2 analytic halo, 3 HDR emitter core.
    pub fog: f32,
    pub uv: WgrVec2,
    pub color: u32,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)]
pub enum WgrBlend {
    Opaque = 0,
    Alpha = 1,
    Additive = 2,
    // Per-poly shadow darken: color = dst*(1-srcA). The fragment outputs black
    // with alpha = shadow strength.
    Shadow = 3,
    ReliefMultiply = 4,
}

// Mirror of the C++ `Sampler2DFlags` / GL33's `_samplerObjects` index. The bits
// double as the index into the renderer's 8 samplers.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct WgrSampler2D(pub u32);

impl WgrSampler2D {
    pub const CLAMP_U: u32 = 1;
    pub const CLAMP_V: u32 = 2;
    pub const POINT: u32 = 4;

    pub fn index(self) -> usize {
        self.0 as usize
    }
}

// Depth-buffer interaction for a batch
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)]
pub enum WgrDepthMode {
    None = 0,
    Test = 1,
    TestWrite = 2,
    // SMK-037: identical depth state to Test (test, never write); it selects the
    // soft-particle fragment stage instead. The engine submits it for cloudlet decals only,
    // and only while the Smoke tab's soft-particle lever is on. `WgrDraw2DBatch.depth` is a
    // plain u32, so this adds no ABI size and an older exe simply never sends it.
    TestSoft = 3,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrDraw2DBatch {
    pub texture_id: u64,
    pub first_vertex: u32,
    pub vertex_count: u32,
    pub blend: WgrBlend,
    pub sampler: WgrSampler2D,
    pub depth: u32,
}

// Object-space mesh vertex; matches the engine's SVertex (pos, normal, uv,
// terrain conform selector, optional authored tangent/binormal).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrMeshVertex {
    pub pos: WgrVec3,
    pub norm: WgrVec3,
    pub uv: WgrVec2,
    // Per-vertex terrain-conform selector (0 = rigid, 1 = ClipLandKeep, 2 = ClipLandOn),
    // read by vs_main at @location(5). Only meaningful when the draw's conform mode
    // selects the per-vertex heightmap path (individual ClipLand vegetation).
    pub conform: u32,
    pub tangent: WgrVec3,
    pub binormal: WgrVec3,
    // Second UV set (`tex1`), or a copy of uv when the shape has none. Read by the GPU-driven
    // VS at @location(7) for Multi's mask/macro/AS stages, which are authored on tex1.
    pub uv1: WgrVec2,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrDraw3D {
    pub mesh: u64,
    pub index_begin: u32,
    pub index_count: u32,
    pub texture_id: u64,
    pub normal_texture_id: u64,
    pub world: WgrMat4,
    pub blend: WgrBlend,
    pub sampler: WgrSampler2D,
    pub camera: u32,
    // Skinning: index of this draw's 128-matrix palette block in WgrFrame.palette
    // (block b spans matrices [b*128 .. b*128+128)). NO_PALETTE = not skinned.
    pub palette_slot: u32,
    pub depth: WgrDepthMode,
    // Alpha-test cutout threshold in [0,1]; a fragment is discarded when its
    // sampled alpha is below this. 0 disables the test.
    pub alpha_ref: f32,
    pub flags: u32,
    // A shared lane (WgrDraw3D is ABI-frozen; layout and hash unchanged by construction):
    //   bits 0-7  = material-debug view index   (dev panel Materials tab)
    //   bit  8    = material-debug invert-normal-Y
    //   bits 16-31 = REN-TEMP-001H stable object id for rigid motion vectors (renderer
    //               keeps id -> previous world). 0 = no identity — the safe default: an
    //               exe predating the id writes only the debug bits (or zero), and such
    //               draws fall back to camera-only velocity.
    pub misc: u32,
    // Per-draw material lighting, folded exactly like GL33's
    // UploadVSMaterialConstants (raw sun colour x material, sun-enable already
    // multiplied into the sun terms; emissive shows regardless). The lit shader
    // computes `emissive + sun_ambient + sun_diffuse * N.L`, clamps, x texture.
    // rgb used; the w lanes ride along for 16-byte std140 alignment.
    pub mat_emissive: WgrVec4,
    pub mat_sun_ambient: WgrVec4,
    pub mat_sun_diffuse: WgrVec4,
    // Material modulation for the frame-global point/spot lights (GL33's matDif /
    // matAmb before the per-light colour): raw material diffuse/ambient (eye
    // accommodation already in, night NOT — that rides the light colour). rgb used.
    pub mat_light_diffuse: WgrVec4,
    pub mat_light_ambient: WgrVec4,
    // Sun-only Blinn-Phong specular highlight, folded like GL33's c18: rgb = raw
    // sun diffuse x material specular (sun-enable folded in, so 0 when the sun is
    // off), w = specular power. The lit shader adds `rgb * pow(N.H, max(w,1))`
    // per-fragment when w > 0; w <= 0 means the material has no highlight.
    pub mat_specular: WgrVec4,
    // Terrain-conform plane for GPU vegetation (ForestPlain). When conform2.z (mode)
    // > 0 the vertex shader displaces this draw's vertices onto the ground exactly like
    // ForestPlain::Animate's two-triangle bilinear fit, so the shared forest mesh is
    // uploaded once undeformed instead of rewritten per instance. Zero (mode 0) for
    // every non-conformed draw. See terrain-conform-vegetation-roads-plan.
    pub conform0: WgrVec4, // inv_land_grid, -xf, -zf, bias(=BoundingCenter().y)
    pub conform1: WgrVec4, // y00, y10, d1000, d0100
    pub conform2: WgrVec4, // d1011, d0111, mode, _pad
    pub mat_local_specular: WgrVec4, // raw local-light specular; appended ABI extension
}

// One frame-global point or spot light, shared by every 3D draw + terrain (bound
// as a group-0 storage buffer). Positions are ABSOLUTE world space (not
// camera-relative like the geometry) so a single upload serves every camera; the
// shader reconstructs the camera-relative offset via the frame's cam_pos. Colours
// are pre-scaled by the sun's NightEffect on the CPU, so they fade out by day
// (GL33's night-only local lights). Mirrors GL33's per-draw VS light constants.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrLight {
    pub pos: WgrVec4, // xyz = world-absolute position, w = start-attenuation distance
    pub diffuse: WgrVec4, // rgb = diffuse * nightEffect
    pub ambient: WgrVec4, // rgb = ambient * nightEffect
    pub dir: WgrVec4, // xyz = beam direction (spot), w = isSpot (1) else 0
}

// --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) ---
//
// C++ registers each opaque-rigid LODShapeWithShadow once (its LODs + per-section geometry
// and material), then streams instances (spawns as slots, moves/destruction as updates,
// despawns as removes). The GPU cull compute walks the retained instances each frame and
// emits indirect draws; the CPU stops walking these objects per frame. Mirrored in
// wgpu_renderer.hpp (size-asserted there and below).

// One drawable section of a model LOD, for wgr_model_register. `mesh` + the index range
// address the shared geometry pool (resolved to base_vertex/first_index at registration);
// `variant` selects the pipeline-variant partition (0 = solid, 1 = alpha-cutout).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrModelSection {
    pub mesh: u64,
    pub index_begin: u32,
    pub index_count: u32,
    pub variant: u32,
    // Per-section flags (wgpu_renderer.hpp: WgrModelSectionFlags). Bit 0 =
    // WGR_MODEL_SECTION_REFLECTIVE, the material named an EnvironmentMap. Was dead
    // padding; the bit rides here because WgrModelMaterial is size-pinned by a const assert.
    pub flags: u32,
}

// WgrModelSection::flags bit 0: this section's material names an EnvironmentMap, so the
// GPU-driven fragment shader adds a Fresnel-weighted sky reflection (DZ-003).
pub const WGR_MODEL_SECTION_REFLECTIVE: u32 = 1;

// WgrDraw3D::flags bit 1: the per-draw twin of WGR_MODEL_SECTION_REFLECTIVE.
pub const WGR_DRAW3D_REFLECTIVE: u32 = 2;

// WgrModelSection::flags bit 1 / WgrDraw3D::flags bit 2: the material is a recognised light
// FIXTURE whose emissive is real radiance and must survive the night. Everything else's
// `emmisive` is legacy brightness compensation and the shaders fade it with the sun.
pub const WGR_MODEL_SECTION_NIGHT_EMITTER: u32 = 2;
// WgrModelSection::flags bit 2: Multi's mask stage is authored on uvSource "tex1".
pub const WGR_MODEL_SECTION_MASK_UV1: u32 = 4;
// WgrModelSection::flags bit 3 (RFG-047): this section's normal map is an Enfusion `_NMO`
// uploaded COMPRESSED (BC5/BC7), so its x/y are in (r, g) rather than the DXT5nm (a, g).
// Read by gpu_driven.wgsl's SECTION_NORMAL_RG; there is no Rust-side behaviour, the constant
// exists so both sides of the ABI name the bit in one place.
pub const WGR_MODEL_SECTION_NORMAL_RG: u32 = 8;
pub const WGR_MODEL_SECTION_NATIVE_CAVITY: u32 = 16;
// WgrModelSection::flags bit 5: the global normal map is painted in the
// object's own unwrap -- sample it with uv1, unscaled.
pub const WGR_MODEL_SECTION_GLOBALNMO_UV1: u32 = 32;
// Bounded Church fixed-roof proof excludes every clock-intersecting section.
pub const WGR_MODEL_SECTION_NO_OBJECT_SNOW: u32 = 128;
pub const WGR_DRAW3D_NIGHT_EMITTER: u32 = 4;
// WgrDraw3D::flags bit 3 (MAT-051): classic GLASS -- alpha-blended pane whose texture is
// continuous partial alpha. The shader floors output alpha so the pane reads head-on.
pub const WGR_DRAW3D_GLASS: u32 = 8;
// MAT-052: first-person cockpit draw -- shade with the flat scene ambient, not the sky dome.
pub const WGR_DRAW3D_COCKPIT: u32 = 16;
pub const WGR_DRAW3D_MONITOR: u32 = 32;
// WgrDraw3D::flags bit 6: Enfusion `Cull none` -- draw double-sided (the
// per-draw pipeline drops backface culling for the draw). Deliberately NOT in
// direct_material_flags: cull is pipeline state, not a shader constant.
pub const WGR_DRAW3D_DOUBLE_SIDED: u32 = 64;
pub const WGR_DRAW3D_NORMAL_RG: u32 = 128;
pub const WGR_DRAW3D_NATIVE_CAVITY: u32 = 1024;
pub const WGR_DRAW3D_SNOW_RECEIVER: u32 = 2048;
pub const WGR_DRAW3D_GROUND_RECEIVER: u32 = 4096;
pub const WGR_DRAW3D_BOOT_RELIEF: u32 = 16384;
// MAT-053: a translucent object section from the back-to-front blend pass (wgpu_renderer.hpp).
pub const WGR_DRAW3D_BLEND_SECTION: u32 = 8192;

// Per-section shading, parallel to a model's sections (one per section). The raw material is
// folded with the frame sun in the GPU-driven fragment shader (matching the per-draw path);
// `texture_id` is a wgr_texture_create handle, resolved to a bindless slot at registration.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrModelMaterial {
    pub emissive: WgrVec4,
    pub ambient: WgrVec4,
    pub diffuse: WgrVec4,
    pub specular: WgrVec4, // w = specular power
    pub texture_id: u64,
    // The section's per-texel specular map (RVMAT SpecularDetail: SMDI on Super/Skin, the
    // equivalents on the NormalMap* families), 0 when the material names none. Its G channel
    // scales `specular`, which is the constant that map was authored to modulate.
    pub specular_texture_id: u64,
    // The section's tangent-space normal map (RVMAT Stage1), 0 when the material names none.
    // Decoded the same way the per-draw path decodes it: X from A, Y from G, Z reconstructed.
    pub normal_texture_id: u64,
    pub sampler: u32,
    pub alpha_ref: f32,
    // Multi's four-layer masked blend; 0 mask = not layered, take layer 0 alone.
    pub mask_texture_id: u64,
    pub layer_texture_id: [u64; 3],
    // MAT-048: the layers' OWN normal maps (RVMAT Stage12/13/14, paired with Stage1/2/3's
    // colours), 0 where the material names none. Sampled at their own layer's UV transform.
    pub layer_normal_texture_id: [u64; 3],
    // Per layer, layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
    pub layer_uv: [[f32; 4]; 4],
    // RFG-072: per layer, layer 0 at index 0: the LINEAR colour multiplier the layer's tile
    // is worn in (Enfusion's `Color_N`), identity for every other material.
    pub layer_colour: [[f32; 4]; 4],
    // Enfusion's per-material normal-map intensity (`NormalPower`): multiplier on the
    // decoded tangent-space XY, renormalised by the shader. 1.0 = no-op default.
    pub normal_power: f32,
    // Crown self-occlusion volume: xyz = trunk-axis centre (model space),
    // w = intensity (0 = off); second lane x = height (reserved), y = radius.
    pub crown_ao_p0: [f32; 4],
    pub crown_ao_p1: [f32; 4],
}

// One drawable LOD level of a model: its FindSqrtLevel resolution threshold (`_resolutions[i]`)
// + the range of sections it draws (`section_base` is RELATIVE to this model's sections).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrModelLod {
    pub resolution: f32,
    pub section_base: u32,
    pub section_count: u32,
    pub is_decal: u32,
}

// One retained instance, filled directly by C++ and converted to InstanceGpu (gfx3d/cull.rs).
// `world` is the ABSOLUTE model->world transform (the GPU-driven VS subtracts cam_pos),
// `center.xyz` the world bounding-sphere center + `center.w` the uniform scale (both read by the
// cull compute), `model` the wgr_model_register id.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrInstance {
    pub world: WgrMat4,
    pub center: WgrVec4,
    pub model: u32,
    pub flags: u32,
    // Inflated frustum-cull radius (f32 bits) for terrain-conform instances; 0 = rigid.
    pub cull_radius: u32,
    pub _pad: u32,
    // Terrain-conform plane (mirrors WgrDraw3D::conform*). conform2.z = mode: 0 rigid,
    // 1 ForestPlain bilinear plane, 2 per-vertex ClipLand SurfaceY (conform0.x = bcSurfaceY).
    pub conform0: WgrVec4,
    pub conform1: WgrVec4,
    pub conform2: WgrVec4,
}

const INSTANCE_CPU_FACT_VERSION: u32 = 1;
const INSTANCE_CPU_FACT_BYTES: u32 = 176;
const INSTANCE_CPU_FACT_PRESENT: u32 = 1;
const INSTANCE_CPU_FACT_ABSENT: u32 = 2;
const INSTANCE_CPU_FACT_INVALID: u32 = 3;

/// Additive one-handle CPU fact. `Absent` requires caller-held historical
/// `Present` authority for the literal handle; this getter cannot prove birth.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrInstanceCpuFact {
    pub version: u32,
    pub bytes: u32,
    pub status: u32,
    pub queried_handle: u32,
    pub instance_epoch: u64,
    pub row: WgrInstance,
    pub resolved_slot: u32,
    pub reserved: u32,
}

const _: () = assert!(std::mem::size_of::<WgrInstanceCpuFact>() == INSTANCE_CPU_FACT_BYTES as usize);
const _: () = assert!(std::mem::offset_of!(WgrInstanceCpuFact, row) == 24);
const _: () = assert!(std::mem::offset_of!(WgrInstanceCpuFact, resolved_slot) == 168);

impl WgrInstanceCpuFact {
    fn from_lookup(handle: u32, epoch: u64, lookup: crate::gfx3d::cull::InstanceCpuLookup) -> Self {
        use crate::gfx3d::cull::InstanceCpuLookup;
        let mut fact = Self {
            version: INSTANCE_CPU_FACT_VERSION,
            bytes: INSTANCE_CPU_FACT_BYTES,
            status: INSTANCE_CPU_FACT_INVALID,
            queried_handle: handle,
            instance_epoch: epoch,
            row: WgrInstance {
                world: [0.0; 16], center: [0.0; 4], model: 0, flags: 0,
                cull_radius: 0, _pad: 0, conform0: [0.0; 4],
                conform1: [0.0; 4], conform2: [0.0; 4],
            },
            resolved_slot: u32::MAX,
            reserved: 0,
        };
        match lookup {
            InstanceCpuLookup::Invalid => {}
            InstanceCpuLookup::Absent => fact.status = INSTANCE_CPU_FACT_ABSENT,
            InstanceCpuLookup::Present { slot, row } => {
                fact.status = INSTANCE_CPU_FACT_PRESENT;
                fact.resolved_slot = slot;
                fact.row = WgrInstance {
                    world: row.world, center: row.center, model: row.model, flags: row.flags,
                    cull_radius: row.cull_radius, _pad: row._pad,
                    conform0: row.conform0, conform1: row.conform1, conform2: row.conform2,
                };
            }
        }
        fact
    }
}

// Live tonemap/look parameters, pushed from the ImGui Tonemap tab via
// wgr_set_tonemap. The Hable curve is fixed in the shader; these are exposure + the
// colour-grade block. Layout matches the `Params` uniform in tonemap.wgsl and the
// C++ `WgrTonemap` in wgpu_renderer.hpp exactly (12 f32).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrTonemap {
    pub exposure: f32,        // linear pre-curve multiplier
    pub mode: f32,            // 0 = passthrough (clamp), 1 = Hable
    pub encode: f32,          // 0 = write as-is, 1 = linear->sRGB encode
    pub temperature: f32,     // white balance warm(+)/cool(-)
    pub tint: f32,            // white balance magenta(+)/green(-)
    pub contrast: f32,        // post-curve contrast (1 = neutral)
    pub saturation: f32,      // post-curve saturation (1 = neutral)
    pub lift: f32,            // shadow lift (0 = neutral)
    pub gain: f32,            // post-curve overall multiply (1 = neutral)
    pub bloom_intensity: f32, // linear weight of the bloom added to the scene (0 = off)
    pub bloom_threshold: f32, // bloom soft-knee centre (scene-referred luminance)
    pub bloom_knee: f32,      // bloom soft-knee half-width
    // NV-001: night vision, as an IMAGE operation. See wgpu_renderer.hpp for why the legacy
    // light-colour filter could never have worked here.
    pub nv_strength: f32, // 0 = off, 1 = full goggles
    pub nv_gain: f32,     // linear amplification of the scene before the curve
    pub nv_noise: f32,    // sensor grain amount
    pub nv_vignette: f32, // tube edge darkening, 0 = none
}

impl Default for WgrTonemap {
    fn default() -> Self {
        // Neutral grade, Hable + sRGB-encode on.
        Self {
            exposure: 1.0,
            mode: 1.0,
            encode: 1.0,
            temperature: 0.0,
            tint: 0.0,
            contrast: 1.0,
            saturation: 1.0,
            lift: 0.0,
            gain: 1.0,
            bloom_intensity: 0.04,
            bloom_threshold: 1.0,
            bloom_knee: 0.5,
            nv_strength: 0.0,
            nv_gain: 40.0,
            nv_noise: 0.10,
            nv_vignette: 0.55,
        }
    }
}

// Eye-adaptation / auto-exposure parameters, pushed via wgr_set_exposure. Matches the
// `ExpParams` uniform in exposure.wgsl and the C++ `WgrExposure` (8 f32). Disabled by
// default so manual per-time-of-day exposure tuning is untouched until enabled.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrExposure {
    pub enabled: f32,   // 0 = off (scale eases to 1.0), 1 = auto-exposure on
    pub key: f32,       // target middle-grey luminance (higher = brighter)
    pub min_scale: f32, // clamp on the exposure multiplier
    pub max_scale: f32,
    pub rate: f32,       // per-frame ease toward the target (0..1)
    pub sky_weight: f32, // metering weight of the top of frame (sky) vs bottom (ground)
    pub _pad1: f32,
    pub _pad2: f32,
}

impl Default for WgrExposure {
    fn default() -> Self {
        Self {
            enabled: 0.0,
            key: 0.18,
            min_scale: 0.25,
            max_scale: 4.0,
            rate: 0.03,
            sky_weight: 0.3,
            _pad1: 0.0,
            _pad2: 0.0,
        }
    }
}

// Procedural sky parameters, pushed from the C++ side (per frame for the celestial
// fields, and on edit for the authored look) via wgr_set_sky. Celestial fields
// (sun/moon direction, night factor) come live from LightSun; the atmosphere +
// look fields are authored and tuned in the ImGui Sky tab. The renderer combines
// these with the per-frame inverse view-projection into the sky pass uniform. Layout
// matches the C++ `WgrSky` in wgpu_renderer.hpp exactly (7 vec4 = 112 bytes). See
// docs/procedural-sky-plan.md.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrSky {
    // xyz = unit direction TO the sun (up by day, down at night); w = sun radiance scale.
    pub sun_dir: WgrVec4,
    // xyz = unit direction TO the moon; w = moon phase (0.5 = full).
    pub moon_dir: WgrVec4,
    // xyz = Rayleigh scattering coefficients (per channel, 1/m); w = Rayleigh scale height (m).
    pub rayleigh: WgrVec4,
    // x = Mie scattering coeff (1/m), y = Mie anisotropy g [0,1), z = Mie scale height (m), w = turbidity.
    pub mie: WgrVec4,
    // xyz = ground albedo; w = night factor (0 day .. 1 night).
    pub ground_albedo: WgrVec4,
    // x = sun angular radius (rad), y = exposure (radiance->scene scale), z = planet radius (m),
    // w = atmosphere thickness (m).
    pub params: WgrVec4,
    // x = enabled (0 skips the pass), y = view ray sample count, z = light ray sample count, w = pad.
    pub control: WgrVec4,
    // xyz = scene fog colour the distant terrain fogs toward; w = horizon-haze strength
    // (0 = off). The sky blends toward this near the horizon so the fogged terrain band
    // and the sky meet without a seam (interim until aerial perspective, plan Stage 4).
    pub fog_color: WgrVec4,
    // Authored night-sky floor (plan Stage 6): a deep-blue radiance that fills in as the
    // sun drops below the horizon, so twilight/night settle into a believable blue
    // instead of the physical model's near-black. Blended in by sun altitude.
    // w = camera altitude above sea level (m): the aerial/sky raymarch starts here, so a
    // wrong value makes the march dive below the terrain when flying (huge fake density).
    pub night_zenith: WgrVec4, // xyz = night radiance at the zenith, w = camera altitude (m)
    pub night_horizon: WgrVec4, // xyz = night radiance at the horizon
    // x = sun_dir.y at/above which it is full day (night = 0), y = sun_dir.y at/below
    // which it is full night (night = 1), z = night intensity, w = far-fade range (m):
    // the aerial pass dissolves the terrain edge into the full sky as it nears this
    // distance (the fog/view range) so the horizon has no colour step. 0 = disabled.
    pub night_params: WgrVec4,
    // Volumetric clouds (plan Stage 5): a raymarched cloud shell composited inside
    // sky_radiance so it also appears in reflections + SH ambient. See sky.wgsl.
    pub cloud0: WgrVec4, // x = coverage [0,1], y = extinction (1/m), z = cloud bottom (m ASL), w = cloud top (m ASL)
    pub cloud1: WgrVec4, // x/y = wind world offset (m, RUNTIME, CPU-wrapped), z = shape scale (1/m), w = detail scale (1/m)
    pub cloud2: WgrVec4, // x = HG forward g, y = powder strength, z = ambient scale, w = max march distance (m)
    pub cloud3: WgrVec4, // x = weather scale (1/m), y = weather amount [0,1], z = warp scale (1/m), w = warp amount (m)
    // Evolution offsets (RUNTIME): x = shape, y = detail, z = weather drift, w = pad.
    pub cloud4: WgrVec4,
    // Moon disc (RUNTIME). x = angular RADIUS (rad), y = illuminated fraction (0 new .. 1 full),
    // z = disc radiance scale (irradiance; the shader divides by the solid angle so inflating
    // the disc for visibility lowers its radiance and total power holds), w = draw (0 = skip).
    pub moon_params: WgrVec4,
    // xyz = unit dir TO THE SUN AS SEEN FROM THE MOON — the vector that shades the lunar
    // sphere. Separate from sun_dir on purpose: that one may be the legacy sun and is
    // smoothed, and a terminator tilted wrong looks worse than no phase at all.
    // w = earthshine reflectance floor on the dark side.
    pub moon_sun: WgrVec4,
}

impl Default for WgrSky {
    fn default() -> Self {
        // Earth-like clear-sky defaults (metres). Sun straight up as a neutral seed;
        // C++ overwrites the celestial fields every frame from LightSun.
        Self {
            sun_dir: [0.0, 1.0, 0.0, 22.0],
            moon_dir: [0.0, -1.0, 0.0, 0.5],
            rayleigh: [5.8e-6, 13.5e-6, 33.1e-6, 8000.0],
            mie: [21e-6, 0.76, 1200.0, 1.0],
            ground_albedo: [0.1, 0.1, 0.1, 0.0],
            params: [0.0047, 1.0, 6_360_000.0, 60_000.0],
            control: [1.0, 16.0, 8.0, 0.0],
            fog_color: [0.7, 0.75, 0.8, 1.0],
            // Normalised colours (0..1, pickable); night_params.z scales to radiance.
            night_zenith: [0.15, 0.30, 0.80, 0.0],
            night_horizon: [0.35, 0.45, 0.90, 0.0],
            // Full day above +3 deg sun elevation, full night below -8 deg; intensity 0.02.
            night_params: [0.052, -0.139, 0.02, 0.0],
            // Clouds off by default (coverage 0) so the clear-sky look is unchanged until tuned.
            cloud0: [0.0, 0.06, 1200.0, 3500.0],
            // wind world offset (runtime), shape scale 1/9300, detail scale 1/1700 (incommensurate).
            cloud1: [0.0, 0.0, 1.0 / 9300.0, 1.0 / 1700.0],
            cloud2: [0.35, 1.0, 1.0, 60_000.0],
            // weather scale 1/16000, weather amount, warp scale 1/6000, warp amount (m).
            cloud3: [1.0 / 16_000.0, 0.4, 1.0 / 6_000.0, 900.0],
            // Evolution offsets are runtime; zero until the first sky-runtime push.
            cloud4: [0.0, 0.0, 0.0, 0.0],
            // Moon runtime. Mean angular radius as a neutral seed; the disc stays OFF
            // (w = 0) until C++ pushes a real radiance scale.
            moon_params: [0.00452, 1.0, 0.0, 0.0],
            moon_sun: [0.0, 1.0, 0.0, 0.0],
        }
    }
}

// --- Consolidated imgui-tweakable render params (docs/render-params-consolidation-plan.md) ---
//
// Every ImGui-tweakable render parameter that crosses the FFI as a *setter* is pushed as one
// `WgrRenderParams` block via wgr_set_render_params. Per-frame runtime the engine recomputes
// (sun/moon dir, night factor, fog colour, camera altitude, fog range) is NOT a knob and rides
// the small `WgrSkyRuntime` pushed each frame via wgr_set_sky_runtime. The two write disjoint
// halves of the same internal `WgrSky` UBO (layout + sky shader unchanged).

// Authored procedural-sky look (the ImGui Sky tab). No celestial/runtime fields. The renderer
// folds these into the WgrSky UBO's look slots; defaults mirror WgrSky::default()'s look fields.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrSkyLook {
    pub rayleigh: WgrVec4,     // xyz = scattering coeff (1/m); w = scale height (m)
    pub mie: WgrVec4,          // x = coeff, y = g, z = scale height (m), w = turbidity
    pub ground_sun: WgrVec4,   // xyz = ground albedo; w = sun radiance scale (sunIntensity)
    pub params: WgrVec4, // x = sun angular radius (rad), y = exposure, z = planet radius (m), w = atmosphere (m)
    pub control: WgrVec4, // x = enabled, y = view samples, z = light samples, w = ozone
    pub night_zenith: WgrVec4, // xyz = night radiance at the zenith; w = horizon-haze strength
    pub night_horizon: WgrVec4, // xyz = night radiance at the horizon; w = aerial-shadow strength
    // x = full-day sun_dir.y, y = full-night sun_dir.y, z = night intensity, w = REN-SKY-004:
    // 1 = the planar water reflection marches clouds with its own cheap pipeline, 0 = the sky's
    // own step count. LOOK struct only -- WgrSky::night_params.w is the fog range, untouched.
    pub night_params: WgrVec4,
    // Cloud look (mirrors WgrSky::cloud0/1/2/3; cloud1.xy = wind offset is runtime, ignored here).
    pub cloud0: WgrVec4, // x = coverage, y = extinction (1/m), z = bottom (m), w = top (m)
    pub cloud1: WgrVec4, // x/y unused (runtime wind offset), z = shape scale (1/m), w = detail scale (1/m)
    pub cloud2: WgrVec4, // x = HG forward g, y = powder, z = ambient scale, w = max distance (m)
    pub cloud3: WgrVec4, // x = weather scale (1/m), y = weather amount, z = warp scale (1/m), w = warp amount (m)
}

impl Default for WgrSkyLook {
    fn default() -> Self {
        Self {
            rayleigh: [5.8e-6, 13.5e-6, 33.1e-6, 8000.0],
            mie: [21e-6, 0.76, 1200.0, 1.0],
            ground_sun: [0.1, 0.1, 0.1, 22.0],
            params: [0.0047, 1.0, 6_360_000.0, 60_000.0],
            control: [1.0, 16.0, 8.0, 1.0],
            night_zenith: [0.15, 0.30, 0.80, 0.0],
            night_horizon: [0.35, 0.45, 0.90, 1.0],
            night_params: [0.052, -0.139, 0.02, 1.0],
            cloud0: [0.0, 0.06, 1200.0, 3500.0],
            cloud1: [0.0, 0.0, 1.0 / 9300.0, 1.0 / 1700.0],
            cloud2: [0.35, 1.0, 1.0, 60_000.0],
            cloud3: [1.0 / 16_000.0, 0.4, 1.0 / 6_000.0, 900.0],
        }
    }
}

// Per-frame celestial + camera runtime for the sky (from LightSun / the camera). NOT an ImGui
// knob. Folded into the WgrSky UBO's runtime slots by set_sky_runtime.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrSkyRuntime {
    pub sun_dir: WgrVec4,   // xyz = unit dir TO the sun; w = pad
    pub moon_dir: WgrVec4, // xyz = unit dir TO the moon; w = illuminated fraction (0 new .. 1 full)
    pub fog_color: WgrVec4, // xyz = scene fog colour; w = fog far-range (m)
    pub misc: WgrVec4,     // x = night factor (0..1), y = camera altitude ASL (m), z/w = pad
    // Cloud evolution offsets (m, CPU-wrapped like the wind offset): x = shape, y = detail,
    // z = weather/coverage drift, w = pad. Drifting the noise lookup makes clouds form and
    // dissolve in place rather than merely translating with the wind.
    pub cloud_evolve: WgrVec4,
    // x = moon angular radius (rad), y = illuminated fraction, z = disc radiance scale,
    // w = draw the disc (0 = skip).
    pub moon_params: WgrVec4,
    // xyz = unit dir TO the sun as seen from the moon; w = earthshine floor.
    pub moon_sun: WgrVec4,
    pub layer_fog_weather: WgrVec4, // captured fog amount, landscape-ready, camera-ready, reserved
}

// Long-distance terrain sun-shadow sweep (was wgr_terrain_set_sun_shadow's args).
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct WgrTerrainSunShadow {
    pub strength: f32,     // 0 = disabled
    pub scale: u32,        // mask supersample factor — CHANGING THIS reallocates the mask
    pub max_steps: u32,    // march cap (steps * terrain_grid)
    pub penumbra_deg: f32, // soft-edge half-width
}

impl Default for WgrTerrainSunShadow {
    fn default() -> Self {
        Self {
            strength: 1.0,
            scale: 2,
            max_steps: 512,
            penumbra_deg: 1.0,
        }
    }
}

// Terrain sky-visibility (sky-view factor) AO (was wgr_terrain_set_sky_visibility's args).
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct WgrSkyVisibility {
    pub strength: f32,   // 0 = disabled
    pub contrast: f32,   // deepens the near-1 factor
    pub floor: f32,      // minimum ambient in fully-occluded columns
    pub radius_m: f32,   // horizon-scan reach (m) — CHANGING re-runs the CPU scan
    pub k_azimuths: u32, // scan direction count — CHANGING re-runs the scan
    pub downsample: u32, // scan coarseness — CHANGING re-runs the scan
    pub debug: u32,      // 1 = terrain outputs the factor as greyscale
    pub _pad: u32,
}

impl Default for WgrSkyVisibility {
    fn default() -> Self {
        Self {
            strength: 0.70,
            contrast: 6.5,
            floor: 0.30,
            radius_m: 600.0,
            k_azimuths: 12,
            downsample: 2,
            debug: 0,
            _pad: 0,
        }
    }
}

// Foliage lighting — emulated subsurface scattering + canopy normals for alpha-tested
// vegetation (docs/foliage-translucency-plan.md). Scalars ride into the per-camera Frame UBO
// (frame.foliage / frame.foliageb), read by shade() on the sky-lit path when the draw is a
// cutout (Stage 1) / MapType vegetation (Stage 2). Two vec4 worth, packed for the shader:
//   foliage  = (trans_scale, distortion, trans_power, wrap)
//   foliageb = (ambient_boost, normal_bend[bush], crown_y_offset[bush], fill_fade_end)
//   foliagec = (gi_strength, tree_bend, tree_crown_y, dusk_curve)
#[repr(C)]
#[derive(Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrFoliage {
    pub trans_scale: f32,    // DICE transmission strength (dark-side / backlit lift)
    pub distortion: f32,     // transmission light-dir bend toward the normal (0..1)
    pub trans_power: f32,    // transmission lobe tightness (>= 1)
    pub wrap: f32,           // front terminator-wrap fill (0 = hard Lambert)
    pub ambient_boost: f32,  // SH ambient multiplier for foliage (1 = off), distance-faded
    pub normal_bend: f32,    // BUSH spherical-normal blend (0 = geometric, 1 = full radial)
    pub crown_y_offset: f32, // BUSH crown-centre Y lift for the spherical normal
    pub fill_fade_end: f32, // camera distance (m) by which the SSS fill + ambient boost fade (0 = off)
    // Cheap GI: scale foliage sky-ambient by the terrain's light level (1 - terrain sun-shadow) so
    // shadowed foliage stops glowing. 0 = off; residual at full shadow is (1 - gi_strength).
    pub gi_strength: f32,
    pub tree_bend: f32, // TREE spherical-normal blend (leaf sections only; trunk keeps its normal)
    pub tree_crown_y: f32, // TREE crown-centre Y lift (larger than a bush — centre sits mid-trunk)
    // FOLIAGE-DUSK: exponent applied to `daylight` (sun_dir_world.w) for the two FOLIAGE-ONLY
    // ambient terms in shade() and nowhere else. 1.0 = the pre-2026-09 curve exactly; < 1 keeps
    // the value near 1 through the golden hour and still reaches 0 at real night, because
    // pow(0, k) == 0. See shading.wgsl and Poseidon::FoliageDuskCurve().
    pub dusk_curve: f32,
    // VEG-SWAY - geometric wind on vegetation MODELS. Two further vec4 lanes
    // (frame.foliaged / frame.foliagee). CameraGroup::new sizes the UBO from
    // size_of::<WgrFoliage>() and the upload writes the struct whole, so growing it here is
    // the only Rust-side plumbing change needed.
    pub sway_strength: f32, // metres of tip travel per metre above the pivot; 0 = OFF
    pub sway_speed: f32,    // rate multiplier on the trunk oscillation
    pub wind_dir_x: f32,    // unit vector the wind travels TOWARD
    pub wind_dir_z: f32,
    pub sway_time: f32, // engine seconds (Glob.time) - a paused sim freezes the canopy
    pub wind_gust: f32, // WindSample::gustFraction
    pub sway_leaf: f32, // extra flutter on cutout (leaf) sections only
    pub sway_stiffness: f32, // height exponent (1 = linear lean, 2 = stiff trunk)
}

impl Default for WgrFoliage {
    fn default() -> Self {
        // Kept in sync with C++ Engine::FoliageSettings (the runtime source of truth, pushed every
        // frame). Dialled in by eye against the scene. Base Lambert stays unchanged (sunlit side
        // matches terrain); transmission + wrap lift only the dark/backlit side, faded with distance.
        Self {
            trans_scale: 0.54,
            distortion: 0.49,
            trans_power: 5.1,
            wrap: 0.5,
            ambient_boost: 2.5,
            normal_bend: 0.8,
            crown_y_offset: 0.27,
            fill_fade_end: 500.0,
            gi_strength: 0.44,
            tree_bend: 0.7,
            tree_crown_y: -0.52,
            dusk_curve: 0.35,
            // VEG-SWAY defaults, kept in sync with C++ Engine::FoliageSettings. The wind
            // vector defaults to the WindConditions base heading (atan2(2,4)) so a frame
            // rendered before the first push still leans somewhere sensible rather than
            // snapping east on frame 1.
            sway_strength: 0.055,
            sway_speed: 1.0,
            wind_dir_x: 0.894_427_2,
            wind_dir_z: 0.447_213_6,
            sway_time: 0.0,
            wind_gust: 0.0,
            sway_leaf: 1.0,
            sway_stiffness: 1.6,
        }
    }
}

// Screen-space ambient occlusion (GTAO), docs/screen-space-ao-plan.md. Rides the WgrRenderParams
// block rather than getting its own setter: that block is the project's answer to positional-arg
// ABI bugs (the sky-visibility feature ate two), and a struct in a struct keeps that property.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct WgrGtao {
    pub enabled: u32, // 0 = the whole pass is skipped and consumers read AO = 1
    pub debug: u32,   // raw debug view: 0 = off, 1 = AO greyscale, 2 = bent normal RGB
    pub radius_m: f32,
    pub strength: f32,
    pub slices: u32,
    pub steps: u32,
    pub max_radius_px: f32,
    pub thickness: f32,
    pub blur_radius: f32,
    pub blur_depth_scale: f32,
    pub blur_normal_power: f32,
    pub bent_normal: u32, // 1 = directional ambient via the bent normal (Stage 2)
    pub max_mip: u32,     // highest mip the horizon march may use; 0 = full res only
}

impl Default for WgrGtao {
    fn default() -> Self {
        // Kept in sync with gfx3d::GtaoSettings::default (the renderer's own frame-0 values) and
        // with C++ Engine::AoSettings (the runtime source of truth, pushed every frame).
        let d = crate::gfx3d::GtaoSettings::default();
        Self {
            enabled: d.enabled as u32,
            debug: d.debug_mode,
            radius_m: d.radius_m,
            strength: d.strength,
            slices: d.slices,
            steps: d.steps,
            max_radius_px: d.max_radius_px,
            thickness: d.thickness,
            blur_radius: d.blur_radius,
            blur_depth_scale: d.blur_depth_scale,
            blur_normal_power: d.blur_normal_power,
            bent_normal: d.bent_normal as u32,
            max_mip: d.max_mip,
        }
    }
}

// Every imgui-tweakable render parameter that crosses the FFI as a setter, pushed as one block.
// Passed by pointer only (never uploaded whole), so #[repr(C)] but not Pod. Append future look
// knobs here; do not add new FFI setters.
// Picture Mode depth of field. Rides WgrRenderParams for the reason the comment above that struct
// gives: knobs go there, not into new FFI setters. Layout matches the C++ `WgrDepthOfField` in
// wgpu_renderer.hpp and feeds dof.rs's DofSettings -- see dof.wgsl for what the numbers do.
//
// `near_plane` is here despite not being a look setting: the shader turns reversed-Z depth into
// metres with `near / depth`, and the renderer has no other access to the projection.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct WgrDepthOfField {
    pub enabled: u32,          // 0 = the pass does not run at all and costs nothing
    pub focus_distance: f32,   // metres
    pub focus_range: f32,      // metres, half-width of the fully sharp band
    pub max_blur_pixels: f32,  // widest circle of confusion at 1080p, scaled to the real height
    pub background_scale: f32, // blur behind the focal plane
    pub foreground_scale: f32, // blur in front of it -- far more intrusive, hence separate
    pub transition: f32,       // 1/metres; how quickly the blur opens past the sharp band
    pub near_plane: f32,       // camera near plane, metres
    pub debug_view: f32,       // 0 = normal, 1 = circle of confusion, 2 = raw view distance
    pub sample_count: f32,     // samples per pixel; the entire cost of the pass
    pub bokeh_boost: f32,      // how strongly highlights outweigh their neighbours
    pub bokeh_threshold: f32,  // linear HDR luminance that counts as a highlight
    pub aperture_blades: f32,  // 0 = round, 5..9 = polygonal iris
}

impl Default for WgrDepthOfField {
    fn default() -> Self {
        Self {
            enabled: 0,
            focus_distance: 12.0,
            focus_range: 4.0,
            max_blur_pixels: 24.0,
            background_scale: 1.0,
            foreground_scale: 0.6,
            transition: 0.05,
            near_plane: 0.05,
            debug_view: 0.0,
            sample_count: 48.0,
            bokeh_boost: 3.0,
            bokeh_threshold: 0.7,
            aperture_blades: 0.0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrLayeredFog { pub control: [f32;4], pub layers: [[f32;4];2], pub terrain: [f32;4], pub patch: [f32;4], }
impl Default for WgrLayeredFog {
    fn default()->Self {Self {control:[0.0,0.95,0.6,0.0],layers:[[0.0,35.0,5.0,0.008],[70.0,100.0,5.0,0.003]],terrain:[0.0;4],patch:[0.0;4]}}
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrRenderParams {
    pub tonemap: WgrTonemap,
    pub exposure: WgrExposure,
    pub sky: WgrSkyLook,
    pub terrain_sun_shadow: WgrTerrainSunShadow,
    pub sky_visibility: WgrSkyVisibility,
    pub foliage: WgrFoliage,
    pub gtao: WgrGtao,
    pub interior_sky: WgrSkyVis,
    pub depth_of_field: WgrDepthOfField,
    pub gi: WgrGi,
    pub layered_fog: WgrLayeredFog,
}

// REN-GI-001 -- the irradiance probe volume's parameters (see gfx3d/gi.rs). 64 bytes.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrGi {
    pub enabled: u32,
    /// REN-GI-009: 0 = ambient cube, 1 = SH-1, 2 = SH-2. Was a reserved debug word.
    pub basis: u32,
    pub rays: u32,
    pub rsm_samples: u32,
    pub weight: f32,
    pub interior_mix: f32,
    pub spacing: f32,
    pub hysteresis: f32,
    pub ground_albedo: [f32; 3],
    pub ground_gain: f32,
    pub wall_albedo: f32,
    pub ray_length: f32,
    pub rsm_radius: f32,
    pub rsm_gain: f32,
    /// REN-GI-010: how much a probe's own sky occlusion attenuates its LATERAL sky rays.
    /// 0 = the pre-fix integration exactly (nothing ever tested a sideways ray against a wall,
    /// so an indoor probe was nearly as bright as one on the lawn); 1 = full.
    pub indoor_sky: f32,
}

// Interior sky visibility (LIT-020, docs/interior-sky-visibility-plan.md). Distinct from
// `WgrSkyVisibility` above, which is the TERRAIN heightfield's baked sky-view factor: this one is
// the top-down depth map of the retained OBJECT set, and it is what tells the renderer it is
// indoors. Rides WgrRenderParams for the same reason WgrGtao does.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct WgrSkyVis {
    pub enabled: u32,     // 0 = no map rendered, no cull view, consumers read reach = 1
    pub debug: u32,       // 1 = draw the reach factor as greyscale instead of lighting with it
    pub resolution: u32,  // depth-map edge in texels
    pub extent: f32,      // HALF the world box, metres (1024 tex / 128 m half = 25 cm/texel)
    pub height: f32,      // box half-height above/below the camera, metres
    pub strength: f32,    // 0 = inert, 1 = full attenuation
    pub floor: f32,       // minimum ambient multiplier in a sealed volume
    pub kernel: f32,      // softening kernel radius, metres
    pub bias: f32,        // depth bias, metres (stops open ground occluding itself)
    pub directional: f32, // 0 = uniform dimming, 1 = steer ambient fully along the open direction
    // Stage 2: APPLY the per-model baked volumes. Separate from `enabled` because the two are
    // different implementations of the same idea and the point of having both is to A/B them.
    // Producing the volumes still needs WGR_SKY_BAKE_VOLUMES at startup (the bake is load-time);
    // this only decides whether the shading reads them.
    pub baked: u32,
    // A REQUEST COUNTER, not a flag: every increment asks the renderer to log the map-coverage
    // diagnostic on the next frame it renders. The startup one-shot in lib.rs fires ~2 s in and
    // can never fire again, so the only measurement anyone ever had was of the loading screen --
    // taken before the object stream has admitted the neighbourhood the player is standing in.
    // A counter rather than a bool because render params are pushed every frame: a bool would
    // have to be cleared by a second push and could be missed between them.
    pub probe: u32,
}

impl Default for WgrSkyVis {
    fn default() -> Self {
        // Kept in sync with gfx3d::sky_vis::SkyVisSettings::default (the renderer's frame-0
        // values) and with the C++ side, which pushes every frame and therefore wins.
        let d = crate::gfx3d::sky_vis::SkyVisSettings::default();
        Self {
            enabled: d.enabled as u32,
            debug: d.debug as u32,
            resolution: d.resolution,
            extent: d.extent,
            height: d.height,
            strength: d.strength,
            floor: d.floor,
            kernel: d.kernel,
            bias: d.bias,
            directional: d.directional,
            baked: d.baked as u32,
            probe: d.probe,
        }
    }
}

pub const NO_PALETTE: u32 = 0xFFFF_FFFF;

// WgrInstance::flags is passed through untouched (cull ignores it, Rust never interprets it); its
// bits live in the C++ producer (WgrInstanceFlags) + the shader consumer (INST_CANOPY_BUSH/_TREE in
// gpu_driven.wgsl). Bits 0/1 (bush/tree canopy) drive vs_gpu's spherical-normal blend.

// Bits for WgrDraw3D::flags (mirror WgrDraw3DFlags in wgpu_renderer.hpp).
pub const DRAW3D_ON_SURFACE: u32 = 1;
// ZBias overlay level (1..3) in bits 8-9.
pub const DRAW3D_ZBIAS_SHIFT: u32 = 8;
pub const DRAW3D_ZBIAS_MASK: u32 = 0x300;

// Frame-global scalars carried in the camera UBO (no room for a 5th bind group).
// Distinct concerns (distance fog, shadow darkening) sharing the ride.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrFrameParams {
    pub fog_start: f32,
    pub fog_inv_range: f32,
    pub fog_enabled: f32, // 0 = off, 1 = on
    pub shadow_strength: f32,
}

// Per-camera cascaded-shadow sampling block (lit-pass side). All zeros
// (ctl.x = cascade count = 0 -> disabled) when shadow maps are off or for
// UI/screen cameras.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrCameraShadow {
    pub cascade_vp: [WgrMat4; 4],
    pub splits: WgrVec4,      // frustum tiers: far eye-depth per tier
    pub omni_radius: WgrVec4, // omni tiers: camera-distance radius (0 = frustum tier)
    pub ctl: WgrVec4,         // {count, omni_count, fade_range, bias_const}
    pub ctlb: WgrVec4,        // {texel_size (1/res), darkness, normal_offset_scale, pcf}
    pub cam_fwd: WgrVec4,     // xyz = camera forward; w = contact mode (0 fixed, 1 full, 2 budget)
    pub sun_dir: WgrVec4,     // xyz = sun travel direction; w = tan(sun angular radius)
    // LGT-010: one perspective view-projection per shadowing spot light, kept separate from
    // the sun's cascades because they are selected differently (by light index, not by
    // distance) and are a different projection kind.
    pub local_vp: [WgrMat4; 24],
    pub local_ctl: WgrVec4, // {count, darkness, texel_size (1/res), first_layer}
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrCamera {
    pub proj: WgrMat4,
    pub view: WgrMat4,
    // fog_color = rgb + pad
    pub fog_color: WgrVec4,
    pub params: WgrFrameParams,
    pub shadow: WgrCameraShadow,
    // World-space camera position (view drops its translation; geometry is
    // camera-relative). GPU terrain uses it for heightmap sampling.
    pub cam_pos: WgrVec4,
    // Sun light for GPU-lit paths (terrain): rgb, pre-multiplied by the eye
    // accommodation on the C++ side.
    pub sun_diffuse: WgrVec4,
    pub sun_ambient: WgrVec4,
    // xyz = normalized sun light TRAVEL direction (GL33's sunDir convention:
    // shaders dot the normal with its negation); valid every frame, unlike the
    // shadow block's sun_dir.
    pub sun_dir_world: WgrVec4,
}

// One shadow caster for the cascade depth passes: a section run of `mesh`,
// transformed by the camera-relative `world` (or skinned via `palette_slot`).
// alpha_ref > 0 alpha-tests the caster texture (cutout foliage silhouettes).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrShadowCaster {
    pub mesh: u64,
    pub index_begin: u32,
    pub index_count: u32,
    pub world: WgrMat4,
    pub texture_id: u64,   // sampled only when alpha_ref > 0; 0 = built-in white
    pub palette_slot: u32, // NO_PALETTE = rigid
    pub alpha_ref: f32,    // 0 = solid caster; > 0 = discard below (cutout)
    pub sampler: WgrSampler2D,
    pub cascade_mask: u32, // bit c set = render into cascade c
    // Terrain-conform plane for this caster (mirrors WgrDraw3D::conform*). Mode 2
    // (conform2.z) conforms ClipLand vegetation to SurfaceY per vertex in the depth
    // shader, so the shared shadow mesh is uploaded ONCE undeformed. 0 = rigid.
    pub conform0: WgrVec4, // x = bcSurfaceY
    pub conform2: WgrVec4, // z = mode
}

// Cascade depth-pass parameters for one frame; count = 0 disables the pass.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrShadowPass {
    pub count: u32, // cascade count (1..4); 0 = no shadow pass this frame
    pub omni_count: u32,
    pub resolution: u32, // depth-map side length per cascade
    /// Bit i set = cascade i can receive grass-shadow fragments. See the C header for why;
    /// 0xF reproduces the old unconditional behaviour.
    pub grass_cascade_mask: u32,
    pub light_vp: [WgrMat4; 4], // camera-relative light view-projections (0..1 NDC z)
    // Camera world position: casters are camera-relative, so the depth shader adds
    // this back to reconstruct absolute world xz for surface_y (terrain conform).
    pub cam_pos: WgrVec4,
    // LGT-010: shadow views for LOCAL lights, rendered into layers count..count+n-1 of the
    // same depth array as the cascades.
    pub local_count: u32,
    /// LGT-026 cache control. Bit 0 = the engine's cache switch is on, bit 1 = re-render every
    /// local view this frame regardless. Occupies what was a pad word, so the ABI is unchanged
    /// (the `grass_cascade_mask` precedent above).
    pub local_flags: u32,
    /// LGT-026. Bit k set = view k's LIGHT changed since last frame -- it moved, its cone or
    /// range changed, or a different light now occupies the slot. Computed engine-side, where
    /// the light's parameters live in ABSOLUTE world space; the renderer adds the reasons only
    /// it can see (the retained-set epoch, the caster set, the target's shape).
    pub local_dirty_mask: u32,
    pub local_pad: [u32; 1],
    pub local_vp: [WgrMat4; 24],
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WgrCmdKind {
    Draw2D = 0,
    Draw3D = 1,
    ClearDepth = 2,
    DrawTerrain = 3,
    // Scene complete: resolve (tonemap) the HDR target to the swapchain. Everything
    // after this command is display-referred UI, drawn straight to the swapchain.
    // No-op on the LDR-direct path. Emitted at the engine's scene->UI seam.
    Resolve = 4,
    DrawWater = 5,
    DrawGrass = 6,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrCmd {
    pub kind: u32,
    pub arg: u32,
}

// Static per-map terrain parameters, uploaded with the heightmap. See
// wgpu_renderer.hpp for field semantics.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrTerrainParams {
    pub world_origin: WgrVec2,
    pub land_grid: f32,
    pub terrain_grid: f32,
    pub hm_width: u32,
    pub hm_height: u32,
    pub land_range: u32,
    pub data_scale: f32,
    // Coast wet band (Stage 2c), pushed per frame via wgr_terrain_set_params. sea_level + time
    // (+ swash) move the damp intertidal line in lockstep with the water's edge; wet_height =
    // metres above the (swash-moved) sea level the band reaches, wet_darken = albedo multiplier
    // in the band (1 = off). Slope-gated in the shader so cliffs stay dry. Uses the SAME swash
    // formula + params as the water shader, so the two register.
    pub sea_level: f32,
    pub time: f32,
    pub swash_speed: f32,
    pub swash_amp: f32,
    pub wet_height: f32,
    pub wet_darken: f32,
    // RFG-065: master weight for the NATIVE Enfusion ground material (the `.emat`'s own
    // ScaleUV tiling and its middle-distance map). 0 = the legacy one-image-per-50-m-cell
    // route, byte for byte. Zero on every world that is not a natively loaded Reforger one.
    pub enfusion_ground: f32,
    // Alpine snowline (dev weather tab): permanent terrain cover above
    // snowline_height, ramping over snowline_range metres. < 0 = off.
    pub snowline_height: f32,
    pub snowline_range: f32,
    pub snowline_depth: f32,
    pub _pad1: f32, // Cosmetic rain wetness [0,1], offset 72; WGSL rain_wetness. Preserve old initialisers.
    pub _pad2: f32, // Effective liquid rain [0,1], offset 76; WGSL rain_strength.
    pub _pad3: f32, // Opt-in wet-soil diagnostic enum0..4 at offset80; default0, no layout change.
    pub _pad4: f32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrTerrainUv {
    pub u: [f32; 4],
    pub v: [f32; 4],
}

/// Surface slots in a terrain material's stage stack. Arma 2 / OA's `TerrainX`
/// fills all six (Stage4..Stage14); Arma 3's `TerrainSNX` fills five and spends
/// Stage14 on the whole-tile normal map instead.
pub const WGR_TERRAIN_SURFACE_SLOTS: usize = 6;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrTerrainMaterial {
    pub legacy: u32,
    pub satellite: u32,
    pub mask: u32,
    // The whole tile's normal, sharing the satellite's frame (Arma 3 Stage14;
    // Arma 2 spends that stage on its sixth surface instead). 0 = none.
    pub tile_normal: u32,
    // Surface colours by SLOT, not by encounter order: slot k is Stage(4 + 2k).
    // 0 where the source leaves that stage empty -- a hole keeps its index,
    // because the LCA mask selects a slot.
    pub surfaces: [u32; WGR_TERRAIN_SURFACE_SLOTS],
    // Tangent-space normals parallel to `surfaces`. 0 = none.
    pub surface_normals: [u32; WGR_TERRAIN_SURFACE_SLOTS],
    pub surface_count: u32,
    pub uv_source_mask: u32,
    pub _pad0: u32,
    pub legacy_detail_normal: u32,
    pub satellite_uv: WgrTerrainUv,
    pub mask_uv: WgrTerrainUv,
    pub surface_uvs: [WgrTerrainUv; WGR_TERRAIN_SURFACE_SLOTS],
    // RFG-065 -- a NATIVELY loaded Enfusion (Reforger) surface, read from the palette
    // entry's own `.emat`. `surface_count` stays 0 there (no LCA mask, no satellite), so
    // these modify the LEGACY branch rather than the authored one. All zero elsewhere.
    // See wgpu_renderer.hpp for what each field is and the corpus counts behind it.
    pub enfusion: u32,
    pub detail_scale: f32,
    pub middle: u32,
    pub middle_scale: f32,
    pub middle_blend: f32,
    pub detail_max: f32,
    pub detail_fade: f32,
    pub legacy_detail_scale: f32,
    // MiddleColor, a LINEAR multiplier on the middle map's texel; 1,1,1 when absent.
    // Own 16-byte row because the WGSL side is a vec3.
    pub middle_color: [f32; 3],
    pub puddle_flags: u32, // Former padding: legacy/native bit 0, authored slot bits 8..13.
}

// One terrain node (shared grid mesh at world-xz `origin`, `size` wide, level
// `lod`). Uploaded as instance-step vertex data.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrTerrainNode {
    pub origin: WgrVec2,
    pub size: f32,
    pub lod: u32,
    pub morph_start: f32,
    pub morph_end: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrTerrainBatch {
    pub first_node: u32,
    pub node_count: u32,
    pub camera: u32,
    pub _pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrGrassBatch {
    pub camera: u32,
    pub flags: u32,
    pub _pad0: u32,
    pub _pad1: u32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrGrassTrack {
    pub x: f32,
    pub z: f32,
    pub radius: f32,
    pub age: f32,
}

// Walked-trail history. Each slot is one stamped footprint; the ring is
// consumed by DISTANCE walked, not time, so this is the length of trail that
// survives rather than a number of seconds.
pub const WGR_GRASS_TRACK_COUNT: usize = 256;
pub const WGR_GRASS_DOWNWASH_COUNT: usize = 4;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrGrassDownwash {
    pub x: f32,
    pub z: f32,
    pub radius: f32,
    pub strength: f32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrGrassParams {
    pub density: f32,
    pub spacing: f32,
    pub near_radius: f32,
    pub enabled: f32,
    pub blade_height: f32,
    pub wind_strength: f32,
    pub wind_direction: f32,
    pub far_radius: f32,
    pub interactor_x: f32,
    pub interactor_z: f32,
    pub interactor_radius: f32,
    pub interactor_strength: f32,
    pub tracks: [WgrGrassTrack; WGR_GRASS_TRACK_COUNT],
    pub downwash: [WgrGrassDownwash; WGR_GRASS_DOWNWASH_COUNT],
    pub debug_ignore_geography_exclusions: f32,
    pub clumping: f32,
    pub color_variation: f32,
    pub transmission: f32,
    pub cast_shadows: f32,
    pub apply_fog: f32,
    pub density_noise_scale: f32,
    pub density_noise_strength: f32,
    pub weed_percent: f32,
    pub flower_percent: f32,
    pub blade_width_scale: f32,
    pub use_photo_tuft: f32,
    pub saturation: f32,
    pub dry_patches: f32,
    pub dry_patch_scale: f32,
    pub mid_radius: f32,
    // Shape and card controls. Trailing vec4 pair keeps the UBO 16-byte aligned.
    //   shape_mix = (variety, taper jitter, bend jitter, blade texture strength)
    //   cards     = (alpha cards on, alpha cutoff, card widening, spare)
    pub shape_variety: f32,
    pub taper_jitter: f32,
    pub bend_jitter: f32,
    pub blade_texture_strength: f32,
    pub alpha_cards: f32,
    pub alpha_cutoff: f32,
    pub card_widen: f32,
    /// How far a blade arcs over, scaled by its own height. 0 = rigid spikes.
    pub blade_arch: f32,
    pub clump_renderer: f32,
    pub photo_tuft_brightness: f32,
    pub photo_tuft_mix: f32,
    pub photo_tuft_patch_size: f32,
    // Photographed-clump look controls. These affect ONLY the photo card path
    // (near + mid when `use_photo_tuft` is on); procedural ribbons and the far
    // proxy are deliberately untouched so tuning cards cannot shift the field.
    /// Contrast about the plate's own mid-luma. 1.0 = the raw photograph.
    pub photo_contrast: f32,
    /// Strength of the per-texel normal rebuilt from the plate's luma gradient.
    /// 0 = the old flat upright normal, which is what made cards read as paper.
    pub photo_contour: f32,
    /// How strongly a clump is shadowed by the cascade the grass itself writes,
    /// i.e. grass-on-grass shadowing. 0 = none.
    pub photo_self_shadow: f32,
    /// Ambient occlusion toward the clump root, where light does not reach.
    pub photo_root_ao: f32,
    /// Near placement grid edge, in cells. The renderer derives it from radius
    /// and spacing so near DENSITY is radius-independent; ignored on input.
    pub near_grid_dim: f32,
    /// Mid placement grid edge, in cells. Also renderer-derived; ignored on input.
    pub mid_grid_dim: f32,
    /// Width in metres of the stochastic dissolve band at every LOD join. Each
    /// ring thins out across the band as the next thickens, instead of one ring
    /// stopping on a hard circle. 0 restores the abrupt joins.
    pub lod_blend: f32,
    /// Alpha test for the photographed cards. Separate from `alpha_cutoff`
    /// (which belongs to the procedural cut-out path) because the photo plates
    /// come from JPEG opacity maps whose noisy edges are a flicker source worth
    /// being able to trim at runtime.
    pub photo_alpha_cutoff: f32,
    /// Force every photographed clump to one atlas layer: -1 = normal selection,
    /// 0..31 = that atlas layer (0 = the primary clump / first map clutter class,
    /// 1..8 the local families on a loose-card world, up to 31 map classes). The
    /// direct way to find which source plate is responsible for a flicker.
    pub photo_force_layer: f32,
    /// Saturation of the photographed cards about their own luma. Separate from
    /// the field-wide `saturation` so the plates can be matched to the
    /// procedural grass instead of moving with it. 0.62 = the former constant.
    pub photo_saturation: f32,
    /// How long a walked imprint survives, in seconds. It recovers over the last
    /// third of this, so the trail thins out rather than vanishing.
    pub track_lifetime: f32,
    /// How far a crushed plant is pressed down, as a fraction of its own height.
    /// 0.55 is the long-standing value. Photographed cards scale this up
    /// internally: a clump plate has to rotate much further than a blade before
    /// it reads as flattened.
    pub imprint_depth: f32,
    /// Selection weights for atlas layers 1..8 (the local families). Relative,
    /// not normalised; all-zero falls back to the primary clump.
    pub photo_layer_weights: [f32; 8],
    /// Per-LOD coverage multipliers on top of `density`, so the three rings can
    /// be balanced independently. The near ring is where card overdraw is paid.
    pub near_density: f32,
    pub mid_density: f32,
    pub far_density: f32,
    /// Photo-card coverage: the fraction of clutter-grid cells (card_spacing,
    /// 1.11 m by default) that grow a card, 0..1. With `card_coverage_auto` set
    /// it applies only where the map's geography bake did NOT answer (OFP
    /// worlds, unmasked cells); authored cells keep the map's own density.
    /// 0.24 is the owner's fallback for worlds whose clutter density is unknown.
    pub card_coverage: f32,
    /// Albedo tints, applied after the existing colour work and before lighting.
    /// `tint_procedural` covers procedural blades, ribbons and the far proxy;
    /// `tint_photo` covers the photographed cards. White = untinted.
    /// RGB albedo tint. Unused input alpha carries gust waves: negative = old
    /// gusts, 1 + variation 0..1, zero = new default (full local variation).
    pub tint_procedural: [f32; 4],
    /// RGB albedo tint. Unused input alpha carries gust front size in metres,
    /// 10..200; zero = new default 45m. Renderer alpha remains pinned to one.
    pub tint_photo: [f32; 4],
    /// 1 = authored cells use the map's own clutter density and `card_coverage`
    /// is the fallback elsewhere; 0 = `card_coverage` multiplies everywhere, on
    /// top of the bake's relative thinning.
    pub card_coverage_auto: f32,
    /// PROCEDURAL BLADES ONLY (near blades and the mid ribbons; cards and the far
    /// proxy ignore these). Defaults reproduce the pre-existing look exactly.
    /// Blade-on-blade self shadow: height-based occlusion inside a tuft plus a
    /// sample of the cascade the grass itself writes. 0 = off (the old look).
    pub blade_self_shadow: f32,
    /// Contrast about the procedural palette's mid-tone. 1.0 = untouched.
    pub blade_contrast: f32,
    /// Per-patch hue drift (warm straw <-> cool blue-green) on top of the
    /// existing luminance-only colour variation. 0 = none (the old look).
    pub blade_hue_variation: f32,
    /// Root-to-tip darkening. 0.70 = the long-standing `mix(0.30, 1.0, t*t)`.
    pub blade_root_shade: f32,
    /// Photo card SIZE multiplier, 1.0 = the stock card (0.5-2.3 m tall, wider
    /// than tall). Arma's own clutter plants are ~0.3-0.9 m, so the stock card is
    /// two to three times the plant it stands for; this is the lever for that.
    pub card_scale: f32,
    /// Wind noise-field scroll multiplier; 0 reads as 1.0 (the shipped look).
    /// See WGR_GRASS_PARAMS `wind_scroll` in wgpu_renderer.h. Takes one of the
    /// two former `_pad_look` lanes, so no offset moves.
    pub wind_scroll: f32,
    /// RFG-090: see `near_blades_only` in wgpu_renderer.h -- blades on the near ring
    /// with cards on the mid ring from the camera outward. Last `_pad_look` lane.
    pub near_blades_only: f32,
    /// Opaque bounding box per photo layer, (u0, u1, v0, v1) in texture space.
    /// Renderer-derived from the uploaded atlas; ignored on input.
    pub photo_layer_bounds: [f32; 36],
    /// Mean linear colour of each photo layer's covered texels, (r, g, b, 1).
    /// The pivot the card contrast turns about, so contrast cannot double as a
    /// brightness change. Renderer-derived; ignored on input.
    pub photo_layer_means: [f32; 36],
    /// RFG-091: see `native_tint` in wgpu_renderer.h.
    pub native_tint: [f32; 3],
    pub native_tint_lerp: f32,
}

// Additive rainfall grid API. The ocean/backends retain their existing layout.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrRainWaterParams { pub domain: WgrVec4, pub control: WgrVec4, pub generation:u64, pub reserved:u64 }
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrRainWaterSourceKey {
    pub world_token:u64, pub generation:u64, pub height_revision:u64,
    pub terrain_range:u32, pub terrain_spacing:f32,
}
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrRainWaterFineCell {
    pub rect:WgrVec4, pub corners:WgrVec4, pub flow_depth:WgrVec4,
    pub parent:u32, pub reserved:[u32;3],
}
pub const WGR_RAIN_WATER_FINE_BACKEND:u32=1;
pub const WGR_RAIN_WATER_SOURCE_READY:u32=2;
pub const WGR_RAIN_WATER_FINE_READY:u32=4;
pub const WGR_RAIN_WATER_FINE_MAX_CELLS:u32=8192;
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrRainWaterPublication {
    pub coarse:WgrRainWaterParams, pub source:WgrRainWaterSourceKey,
    pub revision:u64, pub flags:u32, pub reserved:u32,
}

// Per-map + per-frame water parameters (a small UBO). See wgpu_renderer.hpp.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrWaterParams {
    pub world_origin: WgrVec2,
    pub terrain_grid: f32,
    pub sea_level: f32,
    pub hm_width: u32,
    pub hm_height: u32,
    pub time: f32,
    // Live look params (edited by the Water ImGui tab). See wgpu_renderer.hpp.
    pub wave_amp: f32,
    pub wave_choppy: f32,
    pub wave_speed: f32,
    pub wave_scale: f32,
    pub fade_start: f32,
    pub fade_end: f32,
    pub warp_amp: f32,
    pub spec_power: f32,
    pub spec_intensity: f32,
    pub alpha: f32,
    pub shadow_dim: f32,
    // Depth-based colour + soft shoreline (Stage 2). color_ext = 1/m extinction: how fast the
    // body tint saturates from shallow -> deep with the water column depth. coast_fade = metres
    // of column depth over which the shoreline ramps transparent -> opaque.
    pub color_ext: f32,
    pub coast_fade: f32,
    // rgb = shallow / deep body colour (gamma-space; the shader decodes to linear on HDR). w unused.
    pub shallow_color: WgrVec4,
    pub deep_color: WgrVec4,
    // Coast foam + swash (Stage 2c). foam_width = m of column depth over which shoreline foam
    // fades out; foam_intensity scales it. swash_amp = m the near-shore waterline oscillates in/
    // out; swash_speed = cycles/s. All cosmetic (buoyancy stays on the flat plane).
    pub foam_width: f32,
    pub foam_intensity: f32,
    pub swash_amp: f32,
    pub swash_speed: f32,
    pub fft_control: WgrVec4,
    pub fft_wind_sea: WgrVec4,
    pub fft_cascade_lengths: WgrVec4,
    pub flow_direction_speed: WgrVec4,
    // WTR-003 — water debug views. x = WgrWaterDebugView index (0 = normal shading); the
    // fragment shader swaps its output for the selected diagnostic. yzw reserved. Appended
    // at the end so existing lane offsets are unchanged (sizeof 192 -> 208, matching C++).
    pub debug_params: WgrVec4,
    // WTR-LOOK — x = energy model (0 legacy, 1 physical), y = glitter gain, z = SSS gain,
    // w = environment-reflection gain. Appended at the end (sizeof 208 -> 224, matching C++).
    pub look_params: WgrVec4,
    // WTR-LOOK — x = physical sea-state coupling on/off, y = residual spectrum amplitude,
    // z = low water quality, w = shore breaker gain. (sizeof 224 -> 240, matching C++.)
    pub sea_params: WgrVec4,
    // Underwater tuning, live from the Water tab. x = engage band in metres (the compositor
    // runs while the camera is below sea level + this, so a straddling view can still be
    // classified), y = absorption density multiplier, z = colour bias 0..1 (1 = absorption hue
    // from the authored deep swatch, 0 = the old neutral curve), w = caustic gain.
    // (sizeof 240 -> 256, matching C++.)
    pub underwater_params: WgrVec4,
    // x = underwater effect enabled (1) or off (0). Gates the compositor itself; the depth in
    // fft_control.w only gates the water shader's own tint, so without this lane the pass kept
    // running with the effect switched off (a submerged camera is always inside the engage
    // band). yzw carry wave-foam intensity / deep falloff / planar edge fade. (sizeof 256 -> 272.)
    pub underwater_gate: WgrVec4,
    // WRL-002 — x = shared optical model on (1) / previous physical composite (0), consulted
    // only while look_params.x is on; y = whitecap wind gate on (1) / crest-only (0).
    // z = local surface level under the camera, w = its wave scale (0 = lane unused).
    // (sizeof 272 -> 288, matching C++.)
    pub optics_params: WgrVec4,
    // WRL-003 — body table: [3i] = ellipse (cx, cz, rx, rz), [3i+1] = (level, wave scale, kind,
    // profile), [3i+2] = (flow x, flow z, speed, 0). See wgpu_renderer.hpp. (sizeof 288 -> 1056.)
    pub bodies: [WgrVec4; WGR_WATER_MAX_BODIES * 3],
    // TW-WATER W1 — which water implementation renders: 0 = Current OP (legacy), 1 = Tidewater (default)
    // Native. Appended after the body table so no WGSL-mirrored lane moves; the WGSL mirrors of
    // WaterParams end at `bodies` and never read it. Padded to a vec4 for the uniform buffer.
    pub water_backend: u32,
    pub water_backend_pad: [u32; 3],
    // TW-WATER — the Tidewater Native parameter group from the Water tab (read only by that
    // backend; Current OP never looks at it). See wgpu_renderer.hpp for the lane meanings.
    // (sizeof 1056 -> 1136 with the selector above, matching C++.)
    pub tidewater: [WgrVec4; 4],
    // TW-WATER W3 — the Tidewater shoreline-wave group (ShoreWaves.js ShoreParams); lane meanings in
    // wgpu_renderer.hpp. (sizeof 1136 -> 1184, matching C++.)
    pub tidewater_shore: [WgrVec4; 3],
    // TW-WATER W3g — sea conditions: local wind override, fetch, whitecaps, mode (see
    // wgpu_renderer.hpp). (sizeof 1184 -> 1200, matching C++.)
    pub tidewater_sea: WgrVec4,
    // TW-WATER W4 — effects: spray emission gain, spray sprite intensity, unused x2 (see
    // wgpu_renderer.hpp). (sizeof 1200 -> 1216, matching C++.)
    pub tidewater_fx: WgrVec4,
    // TW-WATER W6 — the boats for the wake simulation and (W6i) bow spray, 8 lanes per boat (see
    // wgpu_renderer.hpp). (sizeof 1216 -> 1472, matching C++.)
    pub tidewater_wake: [WgrVec4; 16],
}

pub const WGR_WATER_MAX_BODIES: usize = 16;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrWaterCascadeConfig {
    pub enabled: u32,
    pub resolution: u32,
    pub tile_length_x: f32,
    pub tile_length_y: f32,
    pub displacement_scale: f32,
    pub horiz_displacement_scale: f32,
    pub normal_scale: f32,
    pub foam_scale: f32,
    pub wind_speed: f32,
    pub wind_direction_rad: f32,
    pub fetch_meters: f32,
    pub water_depth_meters: f32,
    pub swell: f32,
    pub directional_spread: f32,
    pub short_wave_detail: f32,
    pub whitecap_threshold: f32,
    pub spectrum_seed: u32,
    pub phase_offset_seconds: f32,
    pub update_rate_hz: f32,
    pub pad: f32,
}

const _: () = assert!(std::mem::size_of::<WgrWaterCascadeConfig>() == 80);

pub const MAX_WATER_INTERACTIONS: usize = 48;
#[repr(C, align(16))]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrWaterInteractionEvent {
    pub position_radius: WgrVec4,
    pub velocity_kind: WgrVec4,
    pub time_life_foam_mass: WgrVec4,
    pub direction_depth_flags: WgrVec4,
}
#[repr(C, align(16))]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrWaterInteractionParams {
    pub domain: WgrVec4,
    pub previous_domain: WgrVec4,
    pub grid: WgrVec4,
    pub physics: WgrVec4,
    pub misc: WgrVec4,
    pub weather: WgrVec4,
}

// One water node (shared grid mesh at world-xz `origin`, `size` wide, level `lod`).
// Byte-identical to WgrTerrainNode; uploaded as instance-step vertex data.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrWaterNode {
    pub origin: WgrVec2,
    pub size: f32,
    pub lod: u32,
    pub morph_start: f32,
    pub morph_end: f32,
    pub shore_direction: WgrVec2,
    pub shore_factor: f32,
    // WRL-003: 0 = ocean plane, i + 1 = WgrWaterParams.bodies entry i.
    pub body: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrWaterBatch {
    pub first_node: u32,
    pub node_count: u32,
    pub camera: u32,
    pub _pad: u32,
}

// Overlay (dev panel / ImGui) vertex: framebuffer pixels, top-left origin.
// `color` is RGBA with R in the low byte (ImGui packing, NOT the engine order).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrOverlayVertex {
    pub pos: WgrVec2,
    pub uv: WgrVec2,
    pub color: u32,
}

// One scissored overlay draw over the frame's overlay index/vertex slices.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrOverlayDraw {
    pub clip: WgrVec4, // {x0, y0, x1, y1} pixels
    pub texture_id: u64,
    pub first_index: u32,
    pub index_count: u32,
    pub base_vertex: u32,
    pub _pad: u32,
}

// --- FAR INSTANCE TIER -------------------------------------------------------
//
// One cheap proxy per AUTHORED PLACEMENT, uploaded once per world and never turned into an
// Object. Beyond the object residency window (~900 m on Everon, ~550-711 m on Chernarus) a
// placement is not merely coarse — it does not exist, so the world is bare ground until the
// camera closes on it. Sweeping the whole set on the GPU is affordable (measured: 1.2M
// placements 0.05-0.14 ms, 3.65M 0.15-0.23 ms), which is what makes the tier possible at all.
//
// `pos` is ABSOLUTE authored world space: everything downstream subtracts the camera first,
// because at kilometre-scale coordinates an absolute frustum test shifts the frustum by cam_pos.
// Mirrored in wgpu_renderer.hpp (size-asserted on both sides and in the layout hash).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrFarInstance {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub height: f32,
    pub radius: f32,
    pub colour: u32,
    pub flags: u32, // bit0: 0 = card (vegetation), 1 = prism (structure)
    pub pad: u32,
}

/// `WgrFarInstance::flags` bit 0 — draw an extruded prism instead of a camera-facing card.
/// Cards are ~4x cheaper to draw (2 triangles against 12), so this is set only where a box
/// reads better than a billboard.
pub const WGR_FAR_FLAG_PRISM: u32 = 1;

#[repr(C)]
pub struct WgrFrame {
    pub clear: WgrVec4,
    pub fog_color: WgrVec3,
    pub cameras: WgrSlice<WgrCamera>,
    pub draws3d: WgrSlice<WgrDraw3D>,
    pub verts: WgrSlice<WgrVertex2D>,
    pub batches: WgrSlice<WgrDraw2DBatch>,
    pub cmds: WgrSlice<WgrCmd>,
    // Bone-matrix pool for skinned draws: one 128-matrix block per palette slot,
    // world already pre-multiplied in (palette[i] = world * boneMatrix[i]). Length is a
    // multiple of 128.
    pub palette: WgrSlice<WgrMat4>,
    // Cascaded-shadow depth pass: rendered before the command stream when
    // shadow.count > 0 and shadow_casters is non-empty.
    pub shadow: WgrShadowPass,
    pub shadow_casters: WgrSlice<WgrShadowCaster>,
    // Overlay (dev panel): alpha-blended over the finished frame, no depth.
    pub overlay_verts: WgrSlice<WgrOverlayVertex>,
    pub overlay_indices: WgrSlice<u16>,
    pub overlay_draws: WgrSlice<WgrOverlayDraw>,
    // GPU terrain nodes, drawn on WGR_CMD_DRAW_TERRAIN.
    pub terrain_nodes: WgrSlice<WgrTerrainNode>,
    pub terrain_batches: WgrSlice<WgrTerrainBatch>,
    // Frame-global point/spot lights (<= 256), uploaded once into the group-0
    // storage buffer shared by 3D draws + terrain. The per-camera light count
    // rides in WgrCamera::cam_pos.w.
    pub lights: WgrSlice<WgrLight>,
    // GPU water nodes, drawn on WGR_CMD_DRAW_WATER.
    pub water_nodes: WgrSlice<WgrWaterNode>,
    pub water_batches: WgrSlice<WgrWaterBatch>,
    pub grass_batches: WgrSlice<WgrGrassBatch>,
}

// Layouts must match wgpu_renderer.hpp exactly (the C++ side static_asserts the same).
const _: () = assert!(std::mem::size_of::<WgrVertex2D>() == 32);
const _: () = assert!(std::mem::size_of::<WgrDraw2DBatch>() == 32);
const _: () = assert!(std::mem::size_of::<WgrMeshVertex>() == 68);
const _: () = assert!(std::mem::size_of::<WgrDraw3D>() == 288);
const _: () = assert!(std::mem::size_of::<WgrLight>() == 64);
const _: () = assert!(std::mem::size_of::<WgrModelSection>() == 24);
const _: () = assert!(std::mem::size_of::<WgrModelMaterial>() == 320);
const _: () = assert!(std::mem::size_of::<WgrModelLod>() == 16);
const _: () = assert!(std::mem::size_of::<WgrInstance>() == 144);
const _: () = assert!(std::mem::size_of::<WgrTonemap>() == 64);
// WgrSky 16 vec4 -> 18 (was 256 bytes) and WgrSkyRuntime 5 vec4 -> 7 (was 80): the moon
// disc added moon_params + moon_sun to both. These two asserts, the matching
// static_asserts in wgpu_renderer.hpp, and sky/mod.rs's field-ORDER test are the set that
// has to move together — any one of them left behind is silent garbage in the shader.
const _: () = assert!(std::mem::size_of::<WgrSky>() == 288);
const _: () = assert!(std::mem::size_of::<WgrSkyLook>() == 192);
const _: () = assert!(std::mem::size_of::<WgrSkyRuntime>() == 128);
const _: () = assert!(std::mem::size_of::<WgrTerrainSunShadow>() == 16);
// 20 header words, then satellite/mask UVs and six per-slot UVs of two vec4
// each. The WGSL `TerrainMaterial` in terrain.wgsl mirrors this byte for byte.
const _: () = assert!(std::mem::size_of::<WgrTerrainUv>() == 32);
const _: () = assert!(std::mem::size_of::<WgrTerrainMaterial>() == 384);
const _: () = assert!(std::mem::size_of::<WgrSkyVisibility>() == 32);
const _: () = assert!(std::mem::size_of::<WgrFoliage>() == 80);
const _: () = assert!(std::mem::size_of::<WgrGtao>() == 52);
const _: () = assert!(std::mem::size_of::<WgrSkyVis>() == 48); // + probe counter
const _: () = assert!(std::mem::size_of::<WgrDepthOfField>() == 52);
const _: () = assert!(std::mem::size_of::<WgrLayeredFog>() == 80);
const _: () = assert!(std::mem::offset_of!(WgrRenderParams, layered_fog) == 636);
const _: () = assert!(std::mem::size_of::<WgrRenderParams>() == 716);
const _: () = assert!(std::mem::size_of::<WgrFrameParams>() == 16);
// LGT-010 grew this by four mat4 (local_vp) and one vec4 (local_ctl): 352 + 256 + 16.
// LGT-015 took local_vp from four to sixteen (+768): a point light needs six of them.
const _: () = assert!(std::mem::size_of::<WgrCameraShadow>() == 1904);
const _: () = assert!(std::mem::size_of::<WgrCamera>() == 2128);
const _: () = assert!(std::mem::size_of::<WgrShadowCaster>() == 136);
// LGT-010 grew this by local_count + 3 pad words + four mat4: 288 + 16 + 256.
// LGT-015: four mat4 -> sixteen (+768).
const _: () = assert!(std::mem::size_of::<WgrShadowPass>() == 1840);
const _: () = assert!(std::mem::size_of::<WgrCmd>() == 8);
const _: () = assert!(std::mem::size_of::<WgrOverlayVertex>() == 20);
const _: () = assert!(std::mem::size_of::<WgrOverlayDraw>() == 40);
// Alpine snowline grew this by height + range + depth + 3 pad words: 64 + 24.
const _: () = assert!(std::mem::size_of::<WgrTerrainParams>() == 88);
const _: () = assert!(std::mem::size_of::<WgrTerrainNode>() == 24);
const _: () = assert!(std::mem::size_of::<WgrTerrainBatch>() == 16);
const _: () = assert!(std::mem::size_of::<WgrGrassBatch>() == 16);
const _: () = assert!(std::mem::size_of::<WgrGrassTrack>() == 16);
const _: () = assert!(std::mem::size_of::<WgrGrassDownwash>() == 16);
// 1840 bytes; the photo-card look block, the placement block, the layer-mix
// block and eight per-layer weights complete five further vec4s
// (GrassParams.photo / .place / .photo_mix / .layer_weights[2]). 4736 + 32 for
// the card-coverage / procedural-look pair (GrassParams.card_look / .blade_look).
const _: () = assert!(std::mem::size_of::<WgrGrassParams>() == 4784);
const _: () = assert!(std::mem::size_of::<WgrWaterParams>() == 1472);
const _: () = assert!(std::mem::size_of::<WgrRainWaterParams>() == 48);
const _: () = assert!(std::mem::size_of::<WgrRainWaterSourceKey>()==32 && std::mem::offset_of!(WgrRainWaterSourceKey,terrain_range)==24);
const _: () = assert!(std::mem::size_of::<WgrRainWaterFineCell>()==64 && std::mem::offset_of!(WgrRainWaterFineCell,parent)==48);
const _: () = assert!(std::mem::size_of::<WgrRainWaterPublication>()==96 && std::mem::offset_of!(WgrRainWaterPublication,source)==48 &&
    std::mem::offset_of!(WgrRainWaterPublication,revision)==80 && std::mem::offset_of!(WgrRainWaterPublication,flags)==88);
const _: () = assert!(std::mem::size_of::<WgrWaterNode>() == 40);
const _: () = assert!(std::mem::size_of::<WgrWaterBatch>() == 16);
const _: () = assert!(std::mem::size_of::<WgrWaterInteractionEvent>() == 64);
const _: () = assert!(std::mem::align_of::<WgrWaterInteractionEvent>() == 16);
const _: () = assert!(std::mem::size_of::<WgrWaterInteractionParams>() == 96);
const _: () = assert!(std::mem::align_of::<WgrWaterInteractionParams>() == 16);
const _: () = assert!(std::mem::size_of::<WgrSlice<WgrCamera>>() == 16);
const _: () = assert!(std::mem::size_of::<WgrFrame>() == 2128);
const _: () = assert!(std::mem::size_of::<WgrFarInstance>() == 32);
const _: () = assert!(std::mem::size_of::<WgrAbiCheck>() == 28);

// --- Cross-language layout hash ----------------------------------------------
//
// FNV-1a over the size of every struct that crosses the C ABI, in a fixed order
// that wgpu_renderer.hpp's WgrLayoutHash() repeats exactly. Both sides compute it
// from their OWN definitions, so the number only agrees when the definitions do,
// and wgr_abi_validate refuses the pair when it does not.
//
// The per-struct asserts above catch a layout that drifts within ONE build. This
// catches a layout that drifted BETWEEN two builds — an engine .exe and a renderer
// .dll that were compiled at different commits and then deployed together, which
// links fine, shakes hands fine, and silently reads garbage.
//
// Adding a new shared struct? Append it to BOTH lists.
const fn hash_size(mut h: u32, size: usize) -> u32 {
    let s = size as u32;
    let mut i = 0;
    while i < 4 {
        h ^= (s >> (i * 8)) & 0xFF;
        h = h.wrapping_mul(16777619); // FNV-1a 32-bit prime
        i += 1;
    }
    h
}

const fn wgr_layout_hash() -> u32 {
    use std::mem::size_of as sz;
    let mut h = 2166136261u32; // FNV-1a 32-bit offset basis
    h = hash_size(h, sz::<WgrVertex2D>());
    h = hash_size(h, sz::<WgrDraw2DBatch>());
    h = hash_size(h, sz::<WgrMeshVertex>());
    h = hash_size(h, sz::<WgrDraw3D>());
    h = hash_size(h, sz::<WgrLight>());
    h = hash_size(h, sz::<WgrModelSection>());
    h = hash_size(h, sz::<WgrModelMaterial>());
    h = hash_size(h, sz::<WgrModelLod>());
    h = hash_size(h, sz::<WgrInstance>());
    h = hash_size(h, sz::<WgrTonemap>());
    h = hash_size(h, sz::<WgrExposure>());
    h = hash_size(h, sz::<WgrSky>());
    h = hash_size(h, sz::<WgrSkyLook>());
    h = hash_size(h, sz::<WgrSkyRuntime>());
    h = hash_size(h, sz::<WgrTerrainSunShadow>());
    h = hash_size(h, sz::<WgrTerrainUv>());
    h = hash_size(h, sz::<WgrTerrainMaterial>());
    h = hash_size(h, sz::<WgrSkyVisibility>());
    h = hash_size(h, sz::<WgrFoliage>());
    h = hash_size(h, sz::<WgrGtao>());
    h = hash_size(h, sz::<WgrSkyVis>());
    h = hash_size(h, sz::<WgrDepthOfField>());
    h = hash_size(h, sz::<WgrRenderParams>());
    h = hash_size(h, sz::<WgrFrameParams>());
    h = hash_size(h, sz::<WgrCameraShadow>());
    h = hash_size(h, sz::<WgrCamera>());
    h = hash_size(h, sz::<WgrShadowCaster>());
    h = hash_size(h, sz::<WgrShadowPass>());
    h = hash_size(h, sz::<WgrCmd>());
    h = hash_size(h, sz::<WgrOverlayVertex>());
    h = hash_size(h, sz::<WgrOverlayDraw>());
    h = hash_size(h, sz::<WgrTerrainParams>());
    h = hash_size(h, sz::<WgrTerrainNode>());
    h = hash_size(h, sz::<WgrTerrainBatch>());
    h = hash_size(h, sz::<WgrGrassBatch>());
    h = hash_size(h, sz::<WgrGrassTrack>());
    h = hash_size(h, sz::<WgrGrassDownwash>());
    h = hash_size(h, sz::<WgrGrassParams>());
    h = hash_size(h, sz::<WgrWaterParams>());
    h = hash_size(h, sz::<WgrRainWaterParams>());
    h = hash_size(h, sz::<WgrRainWaterSourceKey>());
    h = hash_size(h, sz::<WgrRainWaterFineCell>());
    h = hash_size(h, sz::<WgrRainWaterPublication>());
    h = hash_size(h, sz::<WgrWaterNode>());
    h = hash_size(h, sz::<WgrWaterBatch>());
    h = hash_size(h, sz::<WgrWaterInteractionEvent>());
    h = hash_size(h, sz::<WgrWaterInteractionParams>());
    h = hash_size(h, sz::<WgrSurfaceDesc>());
    h = hash_size(h, sz::<WgrLogCallbacks>());
    h = hash_size(h, sz::<WgrFrame>());
    h = hash_size(h, sz::<WgrFarInstance>());
    // An OUT struct the engine allocates and the DLL fills. It was missing from this set,
    // so a size disagreement between exe and DLL would have been written past the caller's
    // buffer with the handshake reporting no problem at all -- the silent-mismatch failure
    // rule 0 exists for, in the one direction where it corrupts the caller's stack.
    h = hash_size(h, sz::<WgrObjectStats>());
    h = hash_size(h, sz::<WgrSmokeVolumeParams>()); // SMK-038
    h = hash_size(h, sz::<WgrMemoryStats>());
    h = hash_size(h, sz::<WgrGeometryAllocationRow>());
    h = hash_size(h, sz::<WgrGeometryAllocationSummary>());
    h = hash_size(h, sz::<WgrLodDemandRow>());
    h = hash_size(h, sz::<WgrLodDemandReport>());
    h = hash_size(h, sz::<WgrMeshHandleFactSummary>());
    h = hash_size(h, sz::<WgrLayeredFog>()); // ABI21: append-only standalone shared config
    h
}

pub type WgrRenderer = Renderer;

#[unsafe(no_mangle)]
pub extern "C" fn wgr_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}

/// Version of the public C ABI declared in `wgpu_renderer.hpp`.
///
/// This intentionally has a separate integer from the crate's package version:
/// a compatible implementation update must not force a C++ rebuild, while an
/// ABI change must be made explicit at both sides of the boundary.
#[unsafe(no_mangle)]
pub extern "C" fn wgr_abi_version() -> u32 {
    if cfg!(debug_assertions) {
        option_env!("WGR_TEST_ABI_VERSION")
            .and_then(|value| value.parse::<u32>().ok())
            .unwrap_or(22)
    } else {
        22
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn wgr_build_id() -> *const c_char {
    concat!(env!("WGR_BUILD_ID"), "\0").as_ptr() as *const c_char
}

/// Validate the C++ side's versioned ABI declaration before accepting any
/// renderer state. Returns 1 only for an exact, known-compatible layout.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_abi_validate(check: *const WgrAbiCheck) -> i32 {
    let Some(check) = (unsafe { check.as_ref() }) else {
        return 0;
    };
    (check.abi_version == wgr_abi_version()
        && check.struct_size == std::mem::size_of::<WgrAbiCheck>() as u32
        && check.surface_desc_size == std::mem::size_of::<WgrSurfaceDesc>() as u32
        && check.log_callbacks_size == std::mem::size_of::<WgrLogCallbacks>() as u32
        && check.frame_size == std::mem::size_of::<WgrFrame>() as u32
        && check.layout_hash == wgr_layout_hash()
        && (check.required_features & !WGR_ABI_SUPPORTED_FEATURES) == 0) as i32
}

/// The renderer's own cross-language layout hash, for reading a mismatched pair
/// out of a shipped DLL (dumpbin / GetProcAddress) when the engine log has only
/// reported its own number.
///
/// Deliberately NOT declared in wgpu_renderer.hpp: the engine links this DLL
/// implicitly, so an entry point the engine imports must exist in every DLL it
/// might meet, and a missing one kills the process at load before any handshake
/// can report anything. The gate stays `wgr_abi_validate`.
#[unsafe(no_mangle)]
pub extern "C" fn wgr_abi_layout_hash() -> u32 {
    wgr_layout_hash()
}

// REN-TEMP-001 §6.7 — live temporal/upscaler tuning (dev panel). Plain data, not part
// of the hashed layout set: an older exe simply never calls these exports.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrTemporalTuning {
    pub temporal_on: u32,
    // Request only: effective when the WGR_DLSS hal device route is up (that part is
    // decided at startup and cannot change live).
    pub dlss_on: u32,
    pub render_scale_pct: u32, // 50..=100
    pub jitter_phases: u32,    // 0 = auto (frozen at native, cycling when upscaling; 1 freezes always)
    pub mip_bias: f32,         // > 0 = auto (half log2(scale)); else clamped [-4, 0]
    pub reactive_sky: f32,
    pub reactive_water: f32,
    // bit0 = DLSS auto-exposure, bit1 = jitter-Y flip, bit2 = MV render-space,
    // bit3 = MV flip, bit4 = FSR1 enabled, bit5 = RCAS sharpen over DLSS output.
    pub flags: u32,
    /// RCAS sharpness in stops (0 = sharpest, 2 = mildest).
    pub fsr_sharpness: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrTemporalInfo {
    pub render_width: u32,
    pub render_height: u32,
    pub output_width: u32,
    pub output_height: u32,
    pub temporal_active: u32,
    pub dlss_route: u32,  // hal device route up (startup decision)
    pub dlss_active: u32, // actually evaluated last frame
    pub dlss_quality: i32, // NGX PerfQuality value, -1 = none
    pub jitter_x: f32,
    pub jitter_y: f32,
    pub mip_bias_effective: f32,
    pub reset_this_frame: u32,
    /// Active MSAA sample count of the scene targets (1 = off). Startup-fixed; the
    /// menu/panel setting applies on the next launch (WGR_MSAA is the channel).
    pub msaa_samples: u32,
    /// Which upscaler produced the last frame: 0 none/native, 1 DLSS, 2 FSR1,
    /// 3 bilinear.
    pub active_upscaler: u32,
}

/// Push live temporal/upscaler tuning (dev panel). Null-safe.
///
/// # Safety
/// `renderer` must be live; `tuning` must point to one valid struct or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_temporal_tuning(
    renderer: *mut WgrRenderer,
    tuning: *const WgrTemporalTuning,
) {
    if renderer.is_null() || tuning.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let tuning = unsafe { *tuning };
        renderer.set_temporal_tuning(&tuning);
    }));
}

/// Read the current temporal state for display. Null-safe; zeroes on error.
///
/// # Safety
/// `renderer` must be live; `info` must point to writable storage for one struct.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_temporal_info(
    renderer: *mut WgrRenderer,
    info: *mut WgrTemporalInfo,
) {
    if renderer.is_null() || info.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let out = renderer.temporal_info();
        unsafe { *info = out };
    }));
}

/// SMK-037: push the live soft-particle knobs (dev panel / env seed). Null-safe.
///
/// `enabled` gates the whole path renderer-side: with it 0 no depth snapshot is recorded and
/// any batch still marked WGR_DEPTH_TEST_SOFT falls back to the plain depth-tested pipeline.
///
/// # Safety
/// `renderer` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_soft_particles(
    renderer: *mut WgrRenderer,
    enabled: u32,
    fade_metres: f32,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.set_soft_particles(enabled != 0, fade_metres);
    }));
}

/// SMK-038: live tuning for the volumetric smoke field. Mirrors `WgrSmokeVolumeParams`
/// exactly (96 bytes) and is part of the layout hash.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrSmokeVolumeParams {
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
    pub scale: u32,
    pub sun_dir: WgrVec4,
    pub sun_radiance: WgrVec4,
    pub ambient: WgrVec4,
}

/// Push the volumetric-smoke tuning. Null-safe; with `mode` 0 the subsystem is inert.
///
/// # Safety
/// `renderer` must be live; `params` must point to one valid struct or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_smoke_volume(
    renderer: *mut WgrRenderer,
    params: *const WgrSmokeVolumeParams,
) {
    if renderer.is_null() || params.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let p = unsafe { *params };
        unsafe { &mut *renderer }.set_smoke_volume(&p);
    }));
}

/// This frame's smoke particles for froxel injection. Null-safe.
///
/// # Safety
/// `blobs` must point at `count` valid `WgrSmokeShadowBlob`s, or be null with `count == 0`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_smoke_volume_blobs(
    renderer: *mut WgrRenderer,
    blobs: *const WgrSmokeShadowBlob,
    count: u32,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let slice: &[WgrSmokeShadowBlob] = if blobs.is_null() || count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(blobs, count as usize) }
        };
        // Byte-identical layouts (both 32 bytes, two vec4s), asserted on the C++ side.
        let as_vol: &[crate::smoke_volume::SmokeBlob] = unsafe {
            std::slice::from_raw_parts(
                slice.as_ptr() as *const crate::smoke_volume::SmokeBlob,
                slice.len(),
            )
        };
        unsafe { &mut *renderer }.set_smoke_volume_blobs(as_vol);
    }));
}

/// Accepted particle count (low 32) and frames marched (high 32). Zero on a null renderer.
///
/// # Safety
/// `renderer` must be live or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_smoke_volume_stats(renderer: *const WgrRenderer) -> u64 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let r = unsafe { &*renderer };
        u64::from(r.smoke_volume.last_blob_count) | (r.smoke_volume.frames_marched << 32)
    }))
    .unwrap_or(0)
}

/// Queue one capture of the next fully composited swapchain frame.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_screenshot_request(renderer: *mut WgrRenderer) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.request_screenshot()
    }));
}

/// Copy the latest requested capture as tightly packed RGBA8. Call once with
/// null output to query dimensions, then again with `width * height * 4` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_screenshot_take(
    renderer: *mut WgrRenderer,
    out: *mut u8,
    out_len: u32,
    width: *mut u32,
    height: *mut u32,
) -> u32 {
    if renderer.is_null() || width.is_null() || height.is_null() || (out.is_null() && out_len != 0)
    {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let out = if out.is_null() {
            &mut []
        } else {
            unsafe { std::slice::from_raw_parts_mut(out, out_len as usize) }
        };
        unsafe { &mut *renderer }
            .take_screenshot(out, unsafe { &mut *width }, unsafe { &mut *height })
    }))
    .unwrap_or(0)
}

#[cfg(test)]
mod abi_tests {
    #[test]
    fn old_geometry_pass_layout_is_refused_before_renderer_or_output_access() {
        // No renderer exists: a nonnull aligned sentinel must never be dereferenced
        // for a refused layout. Writable output canaries must likewise stay unchanged.
        let renderer = std::ptr::NonNull::<super::WgrRenderer>::dangling().as_ptr();
        let mut output = [0xa5a5_a5a5_a5a5_a5a5u64; 74];
        let original = output;
        for (bytes, version) in [(576, 1), (592, 1), (576, 2), (591, 2)] {
            assert_eq!(unsafe {super::wgr_geometry_pass_facts(renderer,
                output.as_mut_ptr().cast(), bytes, version)}, 2);
            assert_eq!(output, original);
        }
    }
    #[test]
    fn geometry_report_layout_and_invalid_request_are_bounded() {
        assert_eq!(std::mem::size_of::<super::WgrGeometryAllocationRow>(), 40);
        assert_eq!(std::mem::offset_of!(super::WgrGeometryAllocationRow, model_id), 16);
        assert_eq!(std::mem::size_of::<super::WgrGeometryAllocationSummary>(), 104);
        assert_eq!(std::mem::offset_of!(super::WgrGeometryAllocationSummary, unique_meshes), 40);
        assert_eq!(std::mem::offset_of!(super::WgrGeometryAllocationSummary, overlap_within_inspected_meshes), 80);
        assert_eq!(std::mem::offset_of!(super::WgrGeometryAllocationSummary, overlap_within_inspected_index_bytes), 96);
        assert_eq!(unsafe { super::wgr_get_geometry_allocation_report(
            std::ptr::null_mut(), super::WgrSlice { data: std::ptr::null(), len: 0 },
            1, 1, std::ptr::null_mut(), 1, std::ptr::null_mut()) }, 0);
    }
    // A well-formed declaration from a same-commit engine build.
    fn good_check() -> super::WgrAbiCheck {
        super::WgrAbiCheck {
            abi_version: super::wgr_abi_version(),
            struct_size: std::mem::size_of::<super::WgrAbiCheck>() as u32,
            surface_desc_size: std::mem::size_of::<super::WgrSurfaceDesc>() as u32,
            log_callbacks_size: std::mem::size_of::<super::WgrLogCallbacks>() as u32,
            frame_size: std::mem::size_of::<super::WgrFrame>() as u32,
            required_features: super::WGR_ABI_FEATURE_BUILD_ID,
            layout_hash: super::wgr_abi_layout_hash(),
        }
    }

    #[test]
    fn exported_abi_version_matches_the_public_contract() {
        assert_eq!(super::wgr_abi_version(), 21); // Paired cave/hole/swimmer integration
    }

    #[test]
    fn old_mesh_fact_summary_pair_is_refused_and_new_layout_is_exact() {
        assert_eq!(std::mem::size_of::<super::WgrMeshHandleFactSummary>(),80);
        assert_eq!(std::mem::offset_of!(super::WgrMeshHandleFactSummary,live_mesh_records),48);
        assert_eq!(std::mem::offset_of!(super::WgrMeshHandleFactSummary,record_scope_valid),72);
        let mut check=good_check(); check.abi_version=14;
        assert_eq!(unsafe { super::wgr_abi_validate(&check) },0);
    }
    #[test]
    fn pre_tidewater_version_is_refused_even_with_current_layout_hash() {
        let mut check = good_check();
        check.abi_version = 16;
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 0);
    }

    #[test]
    fn pre_sea_and_effect_lanes_version_is_refused_even_with_current_hash() {
        let mut check = good_check();
        check.abi_version = 17;
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 0);
        assert_eq!(std::mem::size_of::<super::WgrWaterParams>(), 1472);
        assert_eq!(std::mem::offset_of!(super::WgrWaterParams, tidewater_sea), 1184);
        assert_eq!(std::mem::offset_of!(super::WgrWaterParams, tidewater_fx), 1200);
        assert_eq!(std::mem::offset_of!(super::WgrWaterParams, tidewater_wake), 1216);
    }

    #[test]
    fn matching_abi_declaration_is_accepted() {
        assert_eq!(unsafe { super::wgr_abi_validate(&good_check()) }, 1);
    }

    #[test]
    fn pre_overlap_summary_abi_is_refused_before_report_writes() {
        let mut check = good_check();
        check.abi_version = 12; // old 80-byte allocation summary
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 0);
    }
    #[test]
    fn lod_demand_layout_and_invalid_request_are_bounded() {
        assert_eq!(std::mem::size_of::<super::WgrLodDemandRow>(), 16);
        assert_eq!(std::mem::size_of::<super::WgrLodDemandReport>(), 192);
        assert_eq!(std::mem::offset_of!(super::WgrLodDemandReport, rows), 64);
        assert_eq!(unsafe { super::wgr_start_lod_demand(std::ptr::null_mut(), 1,
            super::WgrSlice { data: std::ptr::null(), len: 0 }, 1) }, 2);
        assert_eq!(unsafe { super::wgr_poll_lod_demand(std::ptr::null_mut(), 1, std::ptr::null_mut()) }, 0);
        let mut check = good_check(); check.abi_version = 13;
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 0);
    }

    #[test]
    fn mismatched_abi_layout_is_refused() {
        let mut check = good_check();
        check.frame_size = 0;
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 0);
    }

    // The regression this whole handshake exists for: an engine binary built before
    // a shared struct grew (WgrSkyRuntime, 80 -> 112 bytes when the moon disc landed)
    // used to pass, and the renderer then read the moon's radius and illuminated
    // fraction off the end of an 80-byte stack object. Any size drift in ANY shared
    // struct now moves the hash, so the pair is refused instead of drawing garbage.
    #[test]
    fn drifted_shared_struct_layout_is_refused() {
        let mut check = good_check();
        check.layout_hash = super::hash_size(super::wgr_abi_layout_hash(), 80);
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 0);
    }

    #[test]
    fn unsupported_abi_feature_is_refused() {
        let mut check = good_check();
        check.required_features = 0x8000_0000;
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 0);
    }

    #[test]
    fn analytic_glow_semantics_are_negotiated() {
        let mut check = good_check();
        check.required_features = super::WGR_ABI_SUPPORTED_FEATURES;
        assert_eq!(unsafe { super::wgr_abi_validate(&check) }, 1);
        assert_ne!(check.required_features & super::WGR_ABI_FEATURE_ANALYTIC_GLOW, 0);
    }
}

/// # Safety
/// `desc` must point to a valid `WgrSurfaceDesc` and `log` to a valid
/// `WgrLogCallbacks` or be null. The window in `desc` must outlive the renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_create(
    desc: *const WgrSurfaceDesc,
    log: *const WgrLogCallbacks,
) -> *mut WgrRenderer {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(desc) = (unsafe { desc.as_ref() }) else {
            return std::ptr::null_mut();
        };
        let sink = match unsafe { log.as_ref() } {
            Some(l) => LogSink {
                cb: l.log,
                user: l.user,
            },
            None => LogSink::none(),
        };
        match Renderer::new(desc, sink) {
            Ok(renderer) => {
                sink.log(log_level::INFO, "wgpu renderer created");
                Box::into_raw(Box::new(renderer))
            }
            Err(e) => {
                sink.log(
                    log_level::ERROR,
                    &format!("wgpu renderer creation failed: {e}"),
                );
                std::ptr::null_mut()
            }
        }
    }));
    match result {
        Ok(renderer) => renderer,
        Err(panic) => {
            let sink = match unsafe { log.as_ref() } {
                Some(l) => LogSink {
                    cb: l.log,
                    user: l.user,
                },
                None => LogSink::none(),
            };
            let message = if let Some(text) = panic.downcast_ref::<String>() {
                text.as_str()
            } else if let Some(text) = panic.downcast_ref::<&str>() {
                text
            } else {
                "unknown panic payload"
            };
            sink.log(
                log_level::ERROR,
                &format!("wgpu renderer creation panicked: {message}"),
            );
            std::ptr::null_mut()
        }
    }
}

/// # Safety
/// `renderer` must be a live pointer from `wgr_create` (not yet destroyed), or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_destroy(renderer: *mut WgrRenderer) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(renderer) });
    }));
}

/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_resize(renderer: *mut WgrRenderer, width: u32, height: u32) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.resize(width, height);
    }));
}

/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_present_mode(renderer: *mut WgrRenderer, interval: i32) -> i32 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.set_present_mode(interval)
    }))
    .unwrap_or(false) as i32
}

/// Flag for wgr_texture_create: generate the rest of the mip chain from level 0
/// with a box filter (RGBA8 with mip_count 1 only). Must match
/// WGR_TEXTURE_GEN_MIPS.
pub const TEXTURE_GEN_MIPS: u32 = 1;

/// # Safety
/// `renderer` must be live; `data` must point to at least `byte_len` bytes
/// (holding `mip_count` tightly packed mip levels), or be null (in which case 0
/// is returned).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_texture_create(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    format: i32,
    mip_count: u32,
    flags: u32,
    data: *const u8,
    byte_len: u32,
) -> u64 {
    if renderer.is_null() || data.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Some(fmt) = TextureFormat::from_i32(format) else {
            return 0;
        };
        let renderer = unsafe { &mut *renderer };
        let slice = unsafe { std::slice::from_raw_parts(data, byte_len as usize) };
        renderer.texture_create(
            width,
            height,
            fmt,
            mip_count,
            flags & TEXTURE_GEN_MIPS != 0,
            slice,
        )
    }))
    .unwrap_or(0)
}

/// Reserve a bindless slot that outlives any single upload (REN-RES-001). Returns 0 when
/// the bindless array is at cap; the caller then just uses `wgr_texture_create`.
///
/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_texture_slot_acquire(renderer: *mut WgrRenderer) -> u32 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.texture_slot_acquire()
    }))
    .unwrap_or(0)
}

/// Return a slot lease. Only safe once no registered model can still reference the slot,
/// i.e. at the death of the logical texture, never at its eviction.
///
/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_texture_slot_release(renderer: *mut WgrRenderer, slot: u32) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.texture_slot_release(slot);
    }));
}

/// As `wgr_texture_create`, but takes the bindless index `slot` (from
/// `wgr_texture_slot_acquire`) instead of allocating a fresh one, so a re-upload keeps the
/// index already baked into every registered model's material. `slot` 0, or a slot that is
/// not leased, behaves exactly as `wgr_texture_create`.
///
/// # Safety
/// `renderer` must be live; `data` must point to at least `byte_len` bytes (holding
/// `mip_count` tightly packed mip levels), or be null (in which case 0 is returned).
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn wgr_texture_create_in_slot(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    format: i32,
    mip_count: u32,
    flags: u32,
    data: *const u8,
    byte_len: u32,
    slot: u32,
) -> u64 {
    if renderer.is_null() || data.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Some(fmt) = TextureFormat::from_i32(format) else {
            return 0;
        };
        let renderer = unsafe { &mut *renderer };
        let slice = unsafe { std::slice::from_raw_parts(data, byte_len as usize) };
        renderer.texture_create_in_slot(
            width,
            height,
            fmt,
            mip_count,
            flags & TEXTURE_GEN_MIPS != 0,
            slice,
            slot,
        )
    }))
    .unwrap_or(0)
}

/// # Safety
/// `renderer` must be live; `data` must point to at least `byte_len` bytes, or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_texture_update(
    renderer: *mut WgrRenderer,
    id: u64,
    data: *const u8,
    byte_len: u32,
) {
    if renderer.is_null() || data.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let slice = unsafe { std::slice::from_raw_parts(data, byte_len as usize) };
        renderer.texture_update(id, slice);
    }));
}

/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_texture_destroy(renderer: *mut WgrRenderer, id: u64) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.texture_destroy(id);
    }));
}

/// # Safety
/// `renderer` must be live; `verts`/`indices` must each be a valid slice (data
/// valid for its length, or null with length 0; 0 is returned if either empty).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_mesh_create(
    renderer: *mut WgrRenderer,
    verts: WgrSlice<WgrMeshVertex>,
    indices: WgrSlice<u32>,
) -> u64 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let verts = unsafe { verts.as_slice() };
        let indices = unsafe { indices.as_slice() };
        renderer.mesh_create(verts, indices)
    }))
    .unwrap_or(0)
}

/// # Safety
/// `renderer` must be live; `verts` must be a valid slice (its data valid for its
/// length, or null with length 0). `id` must be a handle returned by
/// `wgr_mesh_create` (unknown handles are ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_mesh_update(
    renderer: *mut WgrRenderer,
    id: u64,
    verts: WgrSlice<WgrMeshVertex>,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let verts = unsafe { verts.as_slice() };
        renderer.mesh_update(id, verts);
    }));
}

/// Attach per-vertex skinning data to an existing mesh: 4 bone indices and 4
/// quantised weights per vertex (each `4 * vert_count` bytes). Weights are
/// `Unorm8x4` (0..255 -> 0..1) and should sum to ~1 per vertex.
///
/// # Safety
/// `renderer` must be live; `bones` and `weights` must each be a valid slice of
/// `4 * vert_count` bytes (data valid for its length, or null with length 0).
/// `id` must be a `wgr_mesh_create` handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_mesh_set_skin(
    renderer: *mut WgrRenderer,
    id: u64,
    bones: WgrSlice<u8>,
    weights: WgrSlice<u8>,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let bones = unsafe { bones.as_slice() };
        let weights = unsafe { weights.as_slice() };
        renderer.mesh_set_skin(id, bones, weights);
    }));
}

/// The `Send + Sync` upload surface for CPU-path meshes (REN-THR-009). Opaque on both
/// sides of the ABI: C++ only ever holds the pointer and hands it back.
#[repr(C)]
pub struct WgrUploader {
    _opaque: [u8; 0],
}

// The pointer C++ holds is the `Arc`'s target reinterpreted; these two are the only places
// that know it. Kept as functions rather than inline casts so the round trip reads once.
fn uploader_as_ptr(u: &std::sync::Arc<crate::uploader::Uploader>) -> *const WgrUploader {
    std::sync::Arc::as_ptr(u).cast::<WgrUploader>()
}

/// # Safety
/// `p` must be non-null and must have come from `uploader_as_ptr` on a live renderer.
unsafe fn uploader_from_ptr<'a>(p: *const WgrUploader) -> &'a crate::uploader::Uploader {
    unsafe { &*p.cast::<crate::uploader::Uploader>() }
}

/// Borrow the renderer's upload surface. Unlike every other mesh entry point this forms a
/// SHARED `&*renderer`, and the returned pointer can then be used without touching the
/// renderer at all.
///
/// Lifetime contract — the same discipline the rest of this ABI documents for
/// null-tolerance, written out because this one crosses a thread boundary:
///
/// 1. The pointer is valid until `wgr_destroy` and is stable for the renderer's whole
///    life. It is a borrow, not an owner: there is nothing to release.
/// 2. It may be used from any thread, concurrently with itself.
/// 3. It may NOT be used concurrently with any entry point that forms `&mut *renderer`
///    (which is 62 of the 79 — see REN-THR-001). Today that is trivially satisfied because
///    everything is serial; when a render thread exists, this is the rule that puts uploads
///    on the producer and everything else on the consumer.
/// 4. Mesh lifetime is producer-owned and single-threaded: update and destroy on the SAME
///    handle from two threads at once is forbidden, not merely unspecified.
///
/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null (returns null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_uploader_get(renderer: *mut WgrRenderer) -> *const WgrUploader {
    if renderer.is_null() {
        return std::ptr::null();
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        uploader_as_ptr(renderer.uploader())
    }))
    .unwrap_or(std::ptr::null())
}

/// Re-upload vertex data for an existing mesh without the renderer handle. Semantics are
/// `wgr_mesh_update`'s exactly: topology unchanged, the vertex count must not exceed the
/// mesh's allocated count, and an unknown handle (including one already passed to
/// `wgr_uploader_mesh_destroy`) is ignored. Returns 1 if a write was issued, 0 otherwise.
///
/// # Safety
/// `uploader` must be a pointer from `wgr_uploader_get` whose renderer is still live, or
/// null. `verts` must be a valid slice (data valid for its length, or null with length 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_uploader_mesh_update(
    uploader: *const WgrUploader,
    id: u64,
    verts: WgrSlice<WgrMeshVertex>,
) -> u32 {
    if uploader.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let uploader = unsafe { uploader_from_ptr(uploader) };
        let verts = unsafe { verts.as_slice() };
        u32::from(uploader.mesh_update(id, verts))
    }))
    .unwrap_or(0)
}

/// Park a mesh destroy for the renderer to run at the top of its next frame. It cannot be
/// done here: `Gfx3d::mesh_destroy` removes `bake_bind_cache` entries the draw path reads
/// and returns ranges to the geometry pool's free lists, neither of which may change while
/// a frame is in flight. The handle's upload metadata is dropped immediately, so a later
/// `wgr_uploader_mesh_update` on it is a no-op rather than a write into reused ranges.
///
/// # Safety
/// `uploader` must be a pointer from `wgr_uploader_get` whose renderer is still live, or
/// null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_uploader_mesh_destroy(uploader: *const WgrUploader, id: u64) {
    if uploader.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { uploader_from_ptr(uploader) }.enqueue_destroy(id);
    }));
}

/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_mesh_destroy(renderer: *mut WgrRenderer, id: u64) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.mesh_destroy(id);
    }));
}

// --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) ---

/// Sentinel returned by `wgr_model_register` on failure.
pub const WGR_INVALID_MODEL: u32 = u32::MAX;

/// Retire a model's retained drawing and lighting state. IDs are not reused.
/// # Safety
/// `renderer` must be live or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_model_retire(renderer: *mut WgrRenderer, model: u32) {
    if renderer.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.gfx3d.retire_model(model);
    }));
}

/// Register one opaque-rigid model for GPU-driven rendering. `lods`, `sections`, and
/// `materials` describe a single LODShapeWithShadow: `sections` and `materials` are parallel
/// (one material per section) and each `lods[i].section_base` indexes `sections` relative to
/// this model. Section mesh handles are resolved to the shared geometry pool. Returns the
/// model id (for `wgr_instance_add`) or `WGR_INVALID_MODEL` on error. Call once per shape.
///
/// # Safety
/// `renderer` must be live; `lods`/`sections`/`materials` must each be a valid slice (data
/// valid for its length, or null with length 0). Section mesh handles and material texture
/// handles must be live `wgr_mesh_create` / `wgr_texture_create` handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_model_register(
    renderer: *mut WgrRenderer,
    bounding_sphere: f32,
    lods: WgrSlice<WgrModelLod>,
    sections: WgrSlice<WgrModelSection>,
    materials: WgrSlice<WgrModelMaterial>,
    name: *const std::os::raw::c_char,
) -> u32 {
    if renderer.is_null() {
        return WGR_INVALID_MODEL;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let lods = unsafe { lods.as_slice() };
        let sections = unsafe { sections.as_slice() };
        let materials = unsafe { materials.as_slice() };
        let name = if name.is_null() {
            None
        } else {
            unsafe { std::ffi::CStr::from_ptr(name) }.to_str().ok()
        };
        renderer.model_register(bounding_sphere, lods, sections, materials, name)
    }))
    .unwrap_or(WGR_INVALID_MODEL)
}

/// Register a batch of per-tree crown centres (model space) and return the base index of this
/// batch in the global crown-centre table (foliage-translucency-plan.md §9 Approach A). The
/// caller bakes `base + local_component_index` into each forest vertex's `conform` word.
///
/// # Safety
/// `renderer` must be live; `centres` must be a valid slice (data valid for its length, or null
/// with length 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_register_crown_centres(
    renderer: *mut WgrRenderer,
    centres: WgrSlice<WgrVec4>,
) -> u32 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let centres = unsafe { centres.as_slice() };
        renderer.register_crown_centres(centres)
    }))
    .unwrap_or(0)
}

/// Add a static retained instance; returns its stable slot (recycled from removed slots).
///
/// # Safety
/// `renderer` must be live; `inst` must point to a valid `WgrInstance`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_instance_add(
    renderer: *mut WgrRenderer,
    inst: *const WgrInstance,
) -> u32 {
    if renderer.is_null() || inst.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.instance_add(unsafe { &*inst })
    }))
    .unwrap_or(0)
}

/// Update a static instance in place (a move, or a destruction-phase change).
///
/// # Safety
/// `renderer` must be live; `inst` must point to a valid `WgrInstance`; `slot` must be a
/// slot returned by `wgr_instance_add` (stale slots are ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_instance_update(
    renderer: *mut WgrRenderer,
    slot: u32,
    inst: *const WgrInstance,
) {
    if renderer.is_null() || inst.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.instance_update(slot, unsafe { &*inst });
    }));
}

/// Remove a static instance (recycles its slot).
///
/// # Safety
/// `renderer` must be live; `slot` must be a `wgr_instance_add` slot (stale slots ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_instance_remove(renderer: *mut WgrRenderer, slot: u32) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.instance_remove(slot);
    }));
}

/// Read one exact retained CPU instance row through the real renderer cull table.
/// No GPU readback, frame dispatch, allocation, scan, or stale-operation mutation.
/// Return 0 and leave output untouched for unsupported layout, null pointers or panic;
/// otherwise return the fact status (Present/Absent/Invalid).
///
/// # Safety
/// `renderer` must be live and exclusively owner accessed. `out` must point to
/// a writable, correctly aligned, non-aliasing 176-byte fact allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_instance_cpu_fact(
    renderer: *const WgrRenderer, handle: u32, out: *mut WgrInstanceCpuFact,
    bytes: u32, version: u32,
) -> u32 {
    if renderer.is_null() || out.is_null() || bytes != INSTANCE_CPU_FACT_BYTES ||
        version != INSTANCE_CPU_FACT_VERSION { return 0; }
    catch_unwind(AssertUnwindSafe(|| {
        let (epoch, lookup) = unsafe { &*renderer }.gfx3d.instance_cpu_lookup(handle);
        let fact = WgrInstanceCpuFact::from_lookup(handle, epoch, lookup);
        unsafe { *out = fact; }
        fact.status
    })).unwrap_or(0)
}

#[cfg(test)]
mod instance_cpu_fact_tests {
    use super::*;
    use bytemuck::Zeroable;
    use crate::gfx3d::cull::{InstanceCpuLookup, InstanceGpu};

    #[test]
    fn fixed_layout_and_full_row_bits_match_retained_gpu_row() {
        assert_eq!(std::mem::size_of::<WgrInstance>(), 144);
        assert_eq!(std::mem::size_of::<WgrInstanceCpuFact>(), 176);
        assert_eq!(std::mem::offset_of!(WgrInstanceCpuFact, row), 24);
        assert_eq!(std::mem::offset_of!(WgrInstanceCpuFact, resolved_slot), 168);
        let mut row = InstanceGpu::zeroed();
        row.world[0] = f32::from_bits(0x3f80_0001);
        row.world[12] = -42.5;
        row.center = [7.0, 8.0, 9.0, 3.0];
        row.model = 123;
        row.flags = 32;
        row.cull_radius = 0x4123_4567;
        row._pad = 0x5566_7788;
        row.conform0 = [1.0, 2.0, 3.0, 4.0];
        row.conform1 = [5.0, 6.0, 7.0, 8.0];
        row.conform2 = [9.0, 10.0, 11.0, 12.0];
        let fact = WgrInstanceCpuFact::from_lookup(0x1200_0042, 991,
            InstanceCpuLookup::Present { slot: 65, row });
        assert_eq!((fact.version, fact.bytes, fact.status, fact.queried_handle),
            (1, 176, INSTANCE_CPU_FACT_PRESENT, 0x1200_0042));
        assert_eq!((fact.instance_epoch, fact.resolved_slot, fact.reserved), (991, 65, 0));
        let ffi_words: &[u32; 36] = unsafe { &*((&fact.row as *const WgrInstance).cast::<[u32; 36]>()) };
        let gpu_words: &[u32; 36] = bytemuck::cast_ref(&row);
        assert_eq!(ffi_words, gpu_words, "every 144-byte scalar and float bit must survive");
    }

    #[test]
    fn invalid_layout_leaves_output_untouched_and_nonpresent_rows_are_zero() {
        let absent = WgrInstanceCpuFact::from_lookup(91, 123, InstanceCpuLookup::Absent);
        let invalid = WgrInstanceCpuFact::from_lookup(0, 123, InstanceCpuLookup::Invalid);
        assert_eq!((absent.status, absent.queried_handle, absent.instance_epoch),
            (INSTANCE_CPU_FACT_ABSENT, 91, 123));
        assert_eq!((invalid.status, invalid.queried_handle), (INSTANCE_CPU_FACT_INVALID, 0));
        assert_eq!(absent.resolved_slot, u32::MAX);
        assert_eq!(invalid.resolved_slot, u32::MAX);
        let absent_row: &[u32; 36] = unsafe { &*((&absent.row as *const WgrInstance).cast::<[u32; 36]>()) };
        let invalid_row: &[u32; 36] = unsafe { &*((&invalid.row as *const WgrInstance).cast::<[u32; 36]>()) };
        assert_eq!(*absent_row, [0; 36]);
        assert_eq!(*invalid_row, [0; 36]);
        let renderer = std::ptr::NonNull::<WgrRenderer>::dangling().as_ptr();
        let mut output = absent;
        for (bytes, version) in [(175, 1), (176, 0), (177, 1)] {
            assert_eq!(unsafe { wgr_instance_cpu_fact(renderer, 12, &mut output, bytes, version) }, 0);
            assert_eq!((output.status, output.queried_handle, output.instance_epoch),
                (INSTANCE_CPU_FACT_ABSENT, 91, 123));
        }
        assert_eq!(unsafe { wgr_instance_cpu_fact(std::ptr::null(), 12, &mut output, 176, 1) }, 0);
        assert_eq!(unsafe { wgr_instance_cpu_fact(renderer, 12, std::ptr::null_mut(), 176, 1) }, 0);
    }
}

/// Replace the whole dynamic instance set for this frame (the churny set the CPU already
/// walks for simulation: vehicles, units, ...). Re-copied wholesale each frame.
///
/// # Safety
/// `renderer` must be live; `instances` must be a valid slice (data valid for its length, or
/// null with length 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_dynamic(
    renderer: *mut WgrRenderer,
    instances: WgrSlice<WgrInstance>,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let instances = unsafe { instances.as_slice() };
        renderer.set_dynamic(instances);
    }));
}

/// Push this frame's engine-derived GPU-driven cull + LOD inputs (the real
/// Scene::LevelFromDistance2 values): `objects_z` = ENGINE_CONFIG.objectsZ draw distance,
/// `lod_scale` = Camera::Left() (projection tan(halfFovX)), `lod_inv_width` =
/// Scene::GetLodInvWidth() (≈ lodCoef*2/screenWidth), `pixel_limit` = the legacy 0.125 sub-pixel
/// threshold. No-op unless GPU-driven rendering is enabled. Call once per frame for the main
/// scene camera (e.g. from PushSceneCamera).
///
/// # Safety
/// `renderer` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_cull_params(
    renderer: *mut WgrRenderer,
    objects_z: f32,
    lod_scale: f32,
    lod_inv_width: f32,
    pixel_limit: f32,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.set_cull_inputs(objects_z, lod_scale, lod_inv_width, pixel_limit);
    }));
}

/// Per-frame gate for the retained GPU-driven world set. When `suppress` is nonzero the
/// renderer skips the GPU-driven object draws (colour + prepass) for the frame, so the
/// editor/loading/shutdown frames letterbox to black instead of leaking clutter behind the
/// 2D UI. Resources stay resident; only the draw submission is skipped. No-op unless
/// GPU-driven rendering is enabled. Call every frame (C++ sets the current state).
///
/// # Safety
/// `renderer` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_suppress_world_objects(
    renderer: *mut WgrRenderer,
    suppress: bool,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.set_suppress_world_objects(suppress);
    }));
}

/// FAR INSTANCE TIER — replace the whole far-proxy set (one row per AUTHORED PLACEMENT).
///
/// Called ONCE per world, straight after the placement list is complete. The renderer copies
/// the slice into a single GPU buffer and owns it from there: the per-frame cost is one compute
/// sweep, and nothing on the C++ side walks these again. An empty slice releases the set.
///
/// # Safety
/// `renderer` must be live; `instances` must be a valid slice (data valid for its length, or
/// null with length 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_far_set_instances(
    renderer: *mut WgrRenderer,
    instances: WgrSlice<WgrFarInstance>,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let instances = unsafe { instances.as_slice() };
        renderer.far_set_instances(instances);
    }));
}

/// FAR INSTANCE TIER — per-frame knobs. `near_cutoff_m` is where the tier starts (it should be
/// the object draw distance, so proxies begin exactly where real objects stop); `far_distance_m`
/// where it ends; `pixel_limit` the sub-pixel rejection threshold in pixels of projected height,
/// which is the tier's biggest single win (an aircraft view went 421,323 -> 22,135 instances and
/// 0.314 -> 0.025 ms of draw at 2 px); `enabled` 0 makes the tier inert without releasing its
/// buffer. Call once per frame for the main scene camera.
///
/// # Safety
/// `renderer` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_far_set_params(
    renderer: *mut WgrRenderer,
    near_cutoff_m: f32,
    far_distance_m: f32,
    pixel_limit: f32,
    enabled: u32,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.far_set_params(near_cutoff_m, far_distance_m, pixel_limit, enabled != 0);
    }));
}

/// Debug/feature toggles for the GPU-driven cull (ImGui Culling tab): draw the per-instance
/// cull-sphere wireframes, skip the GPU frustum test, and enable GPU Hi-Z occlusion culling
/// (§5). No-op unless GPU-driven rendering is enabled.
///
/// # Safety
/// `renderer` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_cull_debug(
    renderer: *mut WgrRenderer,
    draw_spheres: bool,
    no_frustum: bool,
    occlusion: bool,
) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.set_cull_debug(draw_spheres, no_frustum, occlusion);
    }));
}

/// Upload (or replace) the terrain heightmap + params. See wgpu_renderer.hpp.
///
/// # Safety
/// `renderer` must be live; `params` must point to a valid `WgrTerrainParams`;
/// `heights` must point to at least `hm_width * hm_height` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_heightmap(
    renderer: *mut WgrRenderer,
    heights: *const f32,
    params: *const WgrTerrainParams,
) {
    if renderer.is_null() || heights.is_null() || params.is_null() {
        return;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let params = unsafe { *params };
        let count = params.hm_width as usize * params.hm_height as usize;
        let heights = unsafe { std::slice::from_raw_parts(heights, count) };
        renderer.terrain_set_heightmap(heights, params);
    }));
    if let Err(payload) = result {
        let message = payload.downcast_ref::<String>().map(String::as_str)
            .or_else(|| payload.downcast_ref::<&str>().copied())
            .unwrap_or("unknown panic");
        unsafe { &*renderer }.log.log(crate::log::log_level::ERROR,
            &format!("Terrain heightfield upload failed: {message}"));
    }
}

/// Set/refresh the water placement params (incl. the animated sea level). See
/// wgpu_renderer.hpp.
///
/// # Safety
/// `renderer` must be live; `params` must point to one valid `WgrWaterParams` or
/// be null (in which case the call is ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_water_set_params(
    renderer: *mut WgrRenderer,
    params: *const WgrWaterParams,
) {
    if renderer.is_null() || params.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let params = unsafe { *params };
        renderer.water_set_params(params);
    }));
}

/// # Safety
/// Live renderer, valid params and `count` consecutive RGBA cells when nonzero.
/// The renderer copies all cell bytes synchronously before this call returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_rain_water_set_grid(renderer:*mut WgrRenderer,
    params:*const WgrRainWaterParams,cells:*const WgrVec4,count:u32,revision:u64) {
    if renderer.is_null() || params.is_null() || count>1024*1024 || (count>0&&cells.is_null()) {return}
    let _=catch_unwind(AssertUnwindSafe(|| {
        let renderer=unsafe {&mut *renderer};let params=unsafe {*params};
        let cells=if count==0 {&[]} else {unsafe {std::slice::from_raw_parts(cells,count as usize)}};
        renderer.rain_water_set_grid(params,cells,revision);
    }));
}

/// # Safety
/// Live exclusive renderer and one valid source receipt after the matching
/// actual bare-heightmap upload. The receipt is copied synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_rain_water_set_source(renderer:*mut WgrRenderer,source:*const WgrRainWaterSourceKey) {
    if renderer.is_null()||source.is_null(){return}
    let _=catch_unwind(AssertUnwindSafe(|| {
        let renderer=unsafe {&mut *renderer};let source=unsafe {*source};
        renderer.rain_water_set_source(source);
    }));
}

/// # Safety
/// Live exclusive renderer, valid publication and both consecutive slices.
/// The paired consumer copies/validates everything before returning; it never
/// forwards fine ownership to the legacy coarse setter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_rain_water_set_publication(renderer:*mut WgrRenderer,
    publication:*const WgrRainWaterPublication,coarse:*const WgrVec4,coarse_count:u32,
    fine:*const WgrRainWaterFineCell,fine_count:u32) {
    if renderer.is_null()||publication.is_null(){return}
    let slices_valid=coarse_count<=1024*1024&&fine_count<=WGR_RAIN_WATER_FINE_MAX_CELLS&&
        (coarse_count==0||!coarse.is_null())&&(fine_count==0||!fine.is_null());
    let _=catch_unwind(AssertUnwindSafe(|| {
        let renderer=unsafe {&mut *renderer};let mut publication=unsafe {*publication};
        if !slices_valid {
            publication.coarse.control[3]=0.;publication.flags&=!WGR_RAIN_WATER_SOURCE_READY;
            renderer.rain_water_set_publication(publication,&[],&[]);return;
        }
        let coarse=if coarse_count==0{&[]}else{unsafe {std::slice::from_raw_parts(coarse,coarse_count as usize)}};
        let fine=if fine_count==0{&[]}else{unsafe {std::slice::from_raw_parts(fine,fine_count as usize)}};
        renderer.rain_water_set_publication(publication,coarse,fine);
    }));
}

/// # Safety
/// `renderer` must be live; `config` must point to one valid `WgrWaterCascadeConfig` or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_water_set_cascade_config(
    renderer: *mut WgrRenderer,
    index: u32,
    config: *const WgrWaterCascadeConfig,
) {
    if renderer.is_null() || config.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let config = unsafe { *config };
        renderer.water_set_cascade_config(index, config);
    }));
}

/// # Safety
/// `renderer` must be live and `params` must point to one valid interaction parameter block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_water_set_interaction_params(
    renderer: *mut WgrRenderer,
    params: *const WgrWaterInteractionParams,
) {
    if renderer.is_null() || params.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.water_set_interaction_params(unsafe { *params })
    }));
}

/// Sinkhole W3: the Tidewater water's drawn surface around the camera (the W9a probe grid, read back
/// 1-3 frames late): `cap` texels of displaced vertex (x, y, z) into `xyz` (3 floats each), the grid's
/// lanes (first lattice x, z, step, size) into `lanes` and the readback's age into `age_ms`. Returns the
/// texel count, or 0 when there is none (another water mode, no frame yet, `cap` too small). Any thread.
///
/// # Safety
/// `xyz` must hold `cap * 3` floats, `lanes` 4 and `age_ms` 1.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_water_drawn_grid(xyz: *mut f32, cap: u32, lanes: *mut f32, age_ms: *mut f32) -> u32 {
    if xyz.is_null() || lanes.is_null() || age_ms.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let out = unsafe { std::slice::from_raw_parts_mut(xyz as *mut [f32; 3], cap as usize) };
        let mut l = [0.0f32; 4];
        let mut age = 0.0f32;
        let n = crate::water_tw::probe::drawn_grid(out, &mut l, &mut age);
        if n > 0 {
            unsafe {
                std::ptr::copy_nonoverlapping(l.as_ptr(), lanes, 4);
                *age_ms = age;
            }
        }
        n as u32
    }))
    .unwrap_or(0)
}

/// # Safety
/// `renderer` must be live; `events` must point to `count` records unless `count` is zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_water_submit_interactions(
    renderer: *mut WgrRenderer,
    events: *const WgrWaterInteractionEvent,
    count: u32,
) {
    if renderer.is_null() || (events.is_null() && count != 0) {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let count = (count as usize).min(MAX_WATER_INTERACTIONS);
        let events = if count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(events, count) }
        };
        unsafe { &mut *renderer }.water_submit_interactions(events);
    }));
}

/// Refresh the terrain params UBO without re-uploading the heightmap — cheap, called
/// every frame to animate the coast wet band (sea_level/time/swash/wet_*). See wgpu_renderer.hpp.
///
/// # Safety
/// `renderer` must be live; `params` must point to one valid `WgrTerrainParams` or be null
/// (in which case the call is ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_params(
    renderer: *mut WgrRenderer,
    params: *const WgrTerrainParams,
) {
    if renderer.is_null() || params.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let params = unsafe { *params };
        renderer.terrain_set_params(params);
    }));
}

/// Upload a mission-local snow view. Four header floats and 512x512 deficits.
/// # Safety
/// `data` must address `count` floats; renderer must be live and exclusively owned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_snow(renderer: *mut WgrRenderer, data: *const f32, count: u32) {
    if renderer.is_null() || data.is_null() || count != 4 + 512 * 512 { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let r = unsafe { &mut *renderer };
        let values = unsafe { std::slice::from_raw_parts(data, count as usize) };
        r.terrain.set_snow(&r.queue, values);
    }));
}

/// Upload a signed mission-local mud view, independently of the snow deposit.
/// # Safety
/// `data` must address `count` floats; renderer must be live and exclusively owned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_mud(renderer: *mut WgrRenderer, data: *const f32, count: u32) {
    if renderer.is_null() || data.is_null() || count != 4 + 512 * 512 { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let r = unsafe { &mut *renderer };
        let values = unsafe { std::slice::from_raw_parts(data, count as usize) };
        r.terrain.set_mud(&r.queue, values);
    }));
}

/// Upload a signed sand view, including positive displaced-grain rims.
/// # Safety
/// `data` must address `count` floats; renderer must be live and exclusively owned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_sand(renderer: *mut WgrRenderer, data: *const f32, count: u32) {
    if renderer.is_null() || data.is_null() || count != 4 + 512 * 512 { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let r = unsafe { &mut *renderer };
        let values = unsafe { std::slice::from_raw_parts(data, count as usize) };
        r.terrain.set_sand(&r.queue, values);
    }));
}

/// Sinkhole W1: terrain holes, `n_edges` x {nx, nz, d, last} in world X/Z (see wgpu_renderer.hpp).
/// # Safety
/// `edges` must address `n_edges * 4` floats (or be null with `n_edges` 0); renderer must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_holes(renderer: *mut WgrRenderer, edges: *const f32, n_edges: u32) {
    if renderer.is_null() || (edges.is_null() && n_edges != 0) { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let r = unsafe { &mut *renderer };
        let values: &[f32] = if n_edges == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(edges, n_edges.min(64) as usize * 4) }
        };
        r.terrain.set_holes(&r.queue, values);
    }));
}

/// Sinkhole W1b: how far underground the camera is, 0 (open air) .. 1 (deep in a cave). Scales
/// the outdoor-air effects (sun shafts, surface scattering) away and lets eye adaptation open up.
/// # Safety
/// renderer must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_camera_underground(renderer: *mut WgrRenderer, underground: f32) {
    if renderer.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let r = unsafe { &mut *renderer };
        r.set_camera_underground(underground);
    }));
}

/// Set the terrain ground layers as a list of wgr_texture_create handles (one
/// per Landscape texture index). See wgpu_renderer.hpp.
///
/// # Safety
/// `renderer` must be live; `handles` must point to at least `count` `u64`s, or
/// be null (in which case the call is ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_ground_layers(
    renderer: *mut WgrRenderer,
    handles: *const u64,
    count: u32,
) {
    if renderer.is_null() || handles.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let slice = unsafe { std::slice::from_raw_parts(handles, count as usize) };
        renderer.terrain_set_ground_layers(slice);
    }));
}

/// Upload the per-land-cell texture index map (R16Uint). See wgpu_renderer.hpp.
///
/// # Safety
/// `renderer` must be live; `indices` must point to at least `width * height`
/// `u16`s, or be null (in which case the call is ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_index_map(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    indices: *const u16,
) {
    if renderer.is_null() || indices.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let count = width as usize * height as usize;
        let slice = unsafe { std::slice::from_raw_parts(indices, count) };
        renderer.terrain_set_index_map(width, height, slice);
    }));
}

/// Upload authored OPRW25 terrain material descriptors. See wgpu_renderer.hpp.
///
/// # Safety
/// `materials` must point to at least `count` descriptors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_materials(
    renderer: *mut WgrRenderer,
    materials: *const WgrTerrainMaterial,
    count: u32,
) {
    if renderer.is_null() || materials.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let slice = unsafe { std::slice::from_raw_parts(materials, count as usize) };
        renderer.terrain_set_materials(slice);
    }));
}

/// Upload per-land-cell geography flags for GPU grass placement.  The values are
/// `GeographyInfo::packed`, one `u32` for every landscape cell.
///
/// # Safety
/// `renderer` must be live; `values` must point to at least `width * height` `u32`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_set_geography(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    values: *const u32,
) {
    if renderer.is_null() || values.is_null() || width == 0 || height == 0 {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let count = width as usize * height as usize;
        let values = unsafe { std::slice::from_raw_parts(values, count) };
        renderer.grass_set_geography(width, height, values);
    }));
}

/// GRS-E — upload the photographed grass-tuft texture for the mid LOD's crossed
/// cards. `rgba` is `width * height` RGBA8 texels.
///
/// # Safety
/// `renderer` must be live; `rgba` must point to at least `width * height * 4` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_set_tuft(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    rgba: *const u8,
) {
    if renderer.is_null() || rgba.is_null() || width == 0 || height == 0 {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let bytes = width as usize * height as usize * 4;
        let rgba = unsafe { std::slice::from_raw_parts(rgba, bytes) };
        renderer.grass_set_tuft(width, height, rgba);
    }));
}

/// Upload matching photographed clump layers. The renderer selects a stable
/// layer per world-space patch; layer zero remains the primary A3 clump.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_set_tufts(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    layers: u32,
    rgba: *const u8,
) {
    if renderer.is_null() || rgba.is_null() || width == 0 || height == 0 || layers == 0 {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let bytes = width as usize * height as usize * layers as usize * 4;
        let rgba = unsafe { std::slice::from_raw_parts(rgba, bytes) };
        renderer.grass_set_tufts(width, height, layers, rgba);
    }));
}

/// Whether a photographed clump atlas was actually uploaded. Returns 0 when the
/// optional assets are absent, in which case the renderer keeps procedural grass
/// no matter what `use_photo_tuft` asks for -- the Grass tab reports that rather
/// than leaving a ticked box that does nothing.
///
/// # Safety
/// `renderer` must be a live renderer pointer or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_have_photo_clumps(renderer: *const WgrRenderer) -> u32 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        u32::from(renderer.grass_have_photo_clumps())
    }))
    .unwrap_or(0)
}

/// Upload the opaque, photographed blade-surface texture array used by the
/// near grass geometry. `rgba` is layer-major RGBA8 with `layers` images of
/// identical `width * height` dimensions.
///
/// # Safety
/// `renderer` must be live; `rgba` must point to at least
/// `width * height * layers * 4` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_set_blade_atlas(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    layers: u32,
    rgba: *const u8,
) {
    if renderer.is_null() || rgba.is_null() || width == 0 || height == 0 || layers == 0 {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let bytes = width as usize * height as usize * layers as usize * 4;
        let rgba = unsafe { std::slice::from_raw_parts(rgba, bytes) };
        renderer.grass_set_blade_atlas(width, height, layers, rgba);
    }));
}

/// Update live procedural-grass controls from the developer Grass tab.
///
/// # Safety
/// `renderer` must be live and `params` must point to one valid WgrGrassParams.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_set_params(
    renderer: *mut WgrRenderer,
    params: *const WgrGrassParams,
) {
    if renderer.is_null() || params.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.grass_set_params(unsafe { *params });
    }));
}

/// Upload the per-grid-point ground UV jitter map (Rg8Snorm). See
/// wgpu_renderer.hpp.
///
/// # Safety
/// `renderer` must be live; `offsets` must point to at least
/// `2 * width * height` `i8`s, or be null (in which case the call is ignored).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_jitter_map(
    renderer: *mut WgrRenderer,
    width: u32,
    height: u32,
    offsets: *const i8,
) {
    if renderer.is_null() || offsets.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let count = 2 * width as usize * height as usize;
        let slice = unsafe { std::slice::from_raw_parts(offsets, count) };
        renderer.terrain_set_jitter_map(width, height, slice);
    }));
}

// The terrain sun-shadow + sky-visibility knobs are pushed through the consolidated
// WgrRenderParams block (wgr_set_render_params), which fans out (with diffing) to
// Renderer::terrain_set_sun_shadow / terrain_set_sky_visibility. See
// docs/render-params-consolidation-plan.md.

/// Set the terrain detail noise texture to a wgr_texture_create handle. See
/// wgpu_renderer.hpp.
///
/// # Safety
/// `renderer` must be a live pointer from `wgr_create`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_terrain_set_detail_layer(renderer: *mut WgrRenderer, handle: u64) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &mut *renderer }.terrain_set_detail_layer(handle);
    }));
}

/// # Safety
/// `renderer` and `frame` must be live pointers. Each slice in `*frame` must be
/// valid for its `len` (or null with len 0). Indices carried by `frame.cmds` /
/// `frame.draws3d` (batch, draw, camera) must be in range for their slices.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_render_frame(
    renderer: *mut WgrRenderer,
    frame: *const WgrFrame,
) -> i32 {
    unsafe { render_frame_impl(renderer, frame, None, None, None) }
}

pub use crate::gpu_timers::RenderCallCpuTimings as WgrRenderCallCpuTimings;

/// Additive opt-in render-call diagnostic. Existing render/get-latest APIs are unchanged.
/// Exact size/version/nonzero token required BEFORE dereferencing renderer, frame or output.
/// Returns ordinary render status; -4 means incompatible diagnostic layout (nothing written).
/// # Safety
/// Valid inputs satisfy wgr_render_frame's contract. With matching size/version, output must
/// be aligned writable storage for exactly one WgrRenderCallCpuTimings and not alias inputs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_render_frame_cpu_timings(
    renderer: *mut WgrRenderer, frame: *const WgrFrame, call_token: u64,
    output: *mut WgrRenderCallCpuTimings, output_size: u32, output_version: u32,
) -> i32 {
    if output.is_null() || output_size != std::mem::size_of::<WgrRenderCallCpuTimings>() as u32
        || output_version != WgrRenderCallCpuTimings::VERSION || call_token == 0 {
        return -4;
    }
    let output = unsafe { &mut *output };
    *output = WgrRenderCallCpuTimings::new(call_token);
    unsafe { render_frame_impl(renderer, frame, Some(output), None, None) }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgrMainTargetCountRequest {
    pub struct_size: u32,
    pub version: u32,
    pub token: u64,
    pub model_id: u32,
    pub reserved: u32,
    pub source_generation: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrMainTargetCountResult {
    pub struct_size: u32,
    pub version: u32,
    pub state: u32,
    pub submitted: u32,
    pub token: u64,
    pub model_id: u32,
    pub count: u32,
    pub source_generation: u64,
    pub cull_epoch: u64,
}

/// Separate additive diagnostic: the exact main COUNT request's final queue
/// submission completed. It does not certify pixels, other views or release.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct WgrMainTargetCompletionResult {
    pub struct_size: u32,
    pub version: u32,
    pub state: u32, // 0 unknown, 1 armed, 2 submitted, 3 completed
    pub reserved: u32,
    pub token: u64,
    pub model_id: u32,
    pub reserved2: u32,
    pub source_generation: u64,
    pub cull_epoch: u64,
}
impl WgrMainTargetCompletionResult {
    const VERSION: u32 = 1;
    fn empty() -> Self { Self { struct_size: std::mem::size_of::<Self>() as u32,
        version: Self::VERSION, ..Self::default() } }
}

impl WgrMainTargetCountResult {
    const VERSION: u32 = 1;
    fn empty() -> Self {
        Self { struct_size: std::mem::size_of::<Self>() as u32,
            version: Self::VERSION, ..Self::default() }
    }
    fn from_fact(fact: crate::gfx3d::cull::MainCountFact) -> Self {
        Self { state: match fact.status {
                crate::gfx3d::cull::MainCountStatus::Unknown => 0,
                crate::gfx3d::cull::MainCountStatus::Present => 1,
                crate::gfx3d::cull::MainCountStatus::Absent => 2,
            },
            submitted: u32::from(fact.status != crate::gfx3d::cull::MainCountStatus::Unknown),
            token: fact.identity.token, model_id: fact.identity.model_id,
            count: fact.count, source_generation: fact.identity.source_generation,
            cull_epoch: fact.identity.cull_epoch, ..Self::empty() }
    }
}

#[cfg(test)]
mod main_target_count_ffi_tests {
    use super::*;
    #[test]
    fn versioned_request_rejects_foreign_layout_before_renderer_access() {
        assert_eq!(std::mem::size_of::<WgrMainTargetCountRequest>(), 32);
        assert_eq!(std::mem::size_of::<WgrMainTargetCountResult>(), 48);
        assert_eq!(std::mem::size_of::<WgrMainTargetCompletionResult>(), 48);
        let renderer = 1usize as *mut WgrRenderer;
        let frame = 1usize as *const WgrFrame;
        let request = WgrMainTargetCountRequest { struct_size: 32, version: 1,
            token: 7, model_id: 3, reserved: 0, source_generation: 11 };
        let mut output = WgrMainTargetCountResult { token: 99,
            ..WgrMainTargetCountResult::empty() };
        let mut completion = WgrMainTargetCompletionResult { token: 99,
            ..WgrMainTargetCompletionResult::empty() };
        let mut views = crate::gfx3d::view_reference_facts::WgrViewReferenceFacts {
            render_token: 99, ..Default::default() };
        unsafe {
            assert_eq!(wgr_main_target_completion_fact(renderer, &mut completion, 47, 1), 0);
            assert_eq!(wgr_main_target_completion_fact(renderer, &mut completion, 48, 2), 0);
            assert_eq!(wgr_main_target_completion_fact(std::ptr::null_mut(), &mut completion, 48, 1), 0);
            assert_eq!(wgr_view_reference_facts(renderer, &mut views, 95, 2), 0);
            assert_eq!(wgr_view_reference_facts(renderer, &mut views, 96, 1), 0);
            assert_eq!(wgr_view_reference_facts(std::ptr::null_mut(), &mut views, 96, 2), 0);
            assert_eq!(wgr_render_frame_main_target_count(renderer, frame, &request, 31, 1,
                &mut output, 48, 1), -4);
            assert_eq!(wgr_render_frame_main_target_count(renderer, frame, &request, 32, 1,
                &mut output, 47, 1), -4);
            assert_eq!(wgr_render_frame_main_target_count(renderer, frame, &request, 32, 2,
                &mut output, 48, 1), -4);
            let invalid = WgrMainTargetCountRequest { source_generation: 0, ..request };
            assert_eq!(wgr_render_frame_main_target_count(renderer, frame, &invalid, 32, 1,
                &mut output, 48, 1), -4);
            assert_eq!(wgr_main_target_count_fact(renderer, &mut output, 47, 1), 0);
            assert_eq!(wgr_shadow0_target_count_fact(renderer, &mut output, 47, 1), 0);
            for cascade in 0..4 {
                assert_eq!(wgr_shadow_target_count_fact(renderer, cascade,
                    &mut output, 47, 1), 0);
            }
            assert_eq!(wgr_shadow_target_count_fact(renderer, 4, &mut output, 48, 1), 0);
            assert_eq!(wgr_gi_target_count_fact(renderer, &mut output, 47, 1), 0);
            assert_eq!(wgr_local_shadow0_target_count_fact(renderer, &mut output, 47, 1), 0);
            assert_eq!(wgr_reflection_target_count_fact(renderer, &mut output, 47, 1), 0);
            assert_eq!(wgr_sky0_target_count_fact(renderer, &mut output, 47, 1), 0);
            assert_eq!(wgr_interior_target_count_fact(renderer, 1, &mut output, 47, 1), 0);
            assert_eq!(wgr_interior_target_count_fact(renderer, 5, &mut output, 48, 1), 0);
            assert_eq!(wgr_local_shadow_target_count_fact(renderer, 1, &mut output, 47, 1), 0);
            assert_eq!(wgr_local_shadow_target_count_fact(renderer, 24, &mut output, 48, 1), 0);
        }
        assert_eq!(output.token, 99);
        assert_eq!(completion.token, 99);
        assert_eq!(views.render_token, 99);
    }
}

/// Versioned, opt-in main COUNT render call. Invalid layouts or identity return
/// -4 before renderer/frame dereference. A successful skipped acquire has status
/// zero but submitted zero; COUNT remains Unknown until a later getter poll.
/// # Safety
/// With exact layouts, pointers follow wgr_render_frame's input contract and
/// output points to one aligned, writable, non-aliasing result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_render_frame_main_target_count(
    renderer: *mut WgrRenderer, frame: *const WgrFrame,
    request: *const WgrMainTargetCountRequest, request_size: u32, request_version: u32,
    output: *mut WgrMainTargetCountResult, output_size: u32, output_version: u32,
) -> i32 {
    if request.is_null() || output.is_null() ||
        request_size != std::mem::size_of::<WgrMainTargetCountRequest>() as u32 ||
        request_version != 1 ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return -4; }
    let request = unsafe { *request };
    if request.struct_size != request_size || request.version != request_version ||
        request.reserved != 0 || request.token == 0 || request.token == u64::MAX ||
        request.model_id == u32::MAX || request.source_generation == 0 { return -4; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult { token: request.token,
        model_id: request.model_id, source_generation: request.source_generation,
        ..WgrMainTargetCountResult::empty() };
    unsafe { render_frame_impl(renderer, frame, None, Some(request), Some(output)) }
}

/// Nonblocking poll of the latest private main COUNT result. Returns one for
/// a valid renderer/layout, including an Unknown pending or failed sample.
/// # Safety
/// `renderer` must be live; `output` must be aligned writable storage of the
/// exact declared size and must not alias the renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_main_target_count_fact(
    renderer: *mut WgrRenderer, output: *mut WgrMainTargetCountResult,
    output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.main_target_count_fact(&renderer.device);
        if let Some((token, reason, expected_revision, current_revision, expected_epoch, current_epoch)) =
            renderer.gfx3d.take_main_count_probe_event(0) {
            let last_mutation = renderer.gfx3d.retained_target_last_mutation();
            renderer.log.log(log_level::INFO,
                &format!("private COUNT probe main token={token} reason={reason} revision={expected_revision}/{current_revision} epoch={expected_epoch}/{current_epoch} lastMutation={last_mutation} (reason: 0=no-copy 1=map-pending 2=map-failed 3=epoch 4=target-revision 5=ready 6=superseded; mutation: 0=unknown 1=rows 2=sections 3=skin 4=mesh 5=texture 6=palette 7=uploader 8=reselect; last observed mutation, no Fine release)"));
        }
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

/// Read-only, nonblocking poll of the optional exact-request completion witness.
/// Output remains Unknown on invalidation, replacement, failure or epoch change.
/// # Safety
/// `renderer` must be live; `output` must be aligned writable storage of the
/// exact declared size and must not alias the renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_main_target_completion_fact(
    renderer: *mut WgrRenderer, output: *mut WgrMainTargetCompletionResult,
    output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() ||
        output_size != std::mem::size_of::<WgrMainTargetCompletionResult>() as u32 ||
        output_version != WgrMainTargetCompletionResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCompletionResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let fact = unsafe { &*renderer }.gfx3d.main_target_completion_fact();
        output.state = fact.state;
        output.token = fact.identity.token;
        output.model_id = fact.identity.model_id;
        output.source_generation = fact.identity.source_generation;
        output.cull_epoch = fact.identity.cull_epoch;
    })).is_ok() as u32
}

/// Private, nonblocking all-36-view COUNT snapshot from one renderer borrow.
/// An absent bit describes only a target-model cull COUNT, never pixels or
/// permission to release/reclaim Fine geometry. The required mask is frozen
/// from the exact frame plan; enabled but skipped views stay Unknown.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_view_reference_facts(
    renderer: *mut WgrRenderer,
    output: *mut crate::gfx3d::view_reference_facts::WgrViewReferenceFacts,
    output_size: u32, output_version: u32,
) -> u32 {
    use crate::gfx3d::view_reference_facts::{valid_layout, WgrViewReferenceFacts};
    if renderer.is_null() || output.is_null() || !valid_layout(output_size, output_version) {
        return 0;
    }
    let output = unsafe { &mut *output };
    *output = WgrViewReferenceFacts::default();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        // The off-handle uploader can issue CPU queue writes without this
        // renderer borrow. Its exclusive Published lock waits for an in-flight
        // writer's read guard; sample on both sides so a write overlapping this
        // private getter advances the optional target witness. No GPU wait;
        // stale COUNT classifications fail closed against the stamped revision.
        let track_uploader = renderer.gfx3d.retained_target_revision().is_some();
        if track_uploader {
            let (serial, _) = renderer.uploader.enable_mutation_serial_snapshot();
            renderer.gfx3d.observe_uploader_mutation_serial(serial);
        }
        let revision_before = renderer.gfx3d.retained_target_revision();
        *output = renderer.gfx3d.view_reference_facts(&renderer.device);
        if track_uploader {
            if let Some(serial) = renderer.uploader.mutation_serial_snapshot() {
                renderer.gfx3d.observe_uploader_mutation_serial(serial);
            }
            // A write overlapping aggregation may invalidate a count after it was
            // copied into the output. Never export that stale Absent classification.
            if renderer.gfx3d.retained_target_revision() != revision_before {
                *output = WgrViewReferenceFacts::default();
            }
        }
    })).is_ok() as u32
}

/// Nonblocking cascade-0 COUNT from the same explicit main diagnostic request.
/// A result stays Unknown unless cascade 0 culled, drew into a closed depth
/// pass, and the copy-bearing final encoder was submitted and committed.
/// This is a view-level count, never a pixel or residency-release proof.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_shadow0_target_count_fact(
    renderer: *mut WgrRenderer, output: *mut WgrMainTargetCountResult,
    output_size: u32, output_version: u32,
) -> u32 {
    unsafe { wgr_shadow_target_count_fact(renderer, 0, output, output_size, output_version) }
}

/// Nonblocking target-model COUNT for one solar cascade (0..3). The exact
/// request identity and cull epoch are shared with the main diagnostic, but
/// each cascade has its own counter, staging ring, and independent answer.
/// An inactive sun view, including a night-time local shadow tile, is Unknown.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_shadow_target_count_fact(
    renderer: *mut WgrRenderer, cascade: u32,
    output: *mut WgrMainTargetCountResult,
    output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() ||
        cascade >= 4 ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.shadow_target_count_fact(&renderer.device, cascade as usize);
        if let Some((token, reason, expected_revision, current_revision, expected_epoch, current_epoch)) =
            renderer.gfx3d.take_main_count_probe_event(1 + cascade as usize) {
            let last_mutation = renderer.gfx3d.retained_target_last_mutation();
            renderer.log.log(log_level::INFO,
                &format!("private COUNT probe solar-{cascade} token={token} reason={reason} revision={expected_revision}/{current_revision} epoch={expected_epoch}/{current_epoch} lastMutation={last_mutation} (reason: 0=no-copy 1=map-pending 2=map-failed 3=epoch 4=target-revision 5=ready 6=superseded; mutation: 0=unknown 1=rows 2=sections 3=skin 4=mesh 5=texture 6=palette 7=uploader 8=reselect; last observed mutation, no Fine release)"));
        }
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

/// Nonblocking exact target-model COUNT from GI's sun-proxy sky view 5.
/// A cached RSM, disabled GI, missed dispatch/draw, or failed submit remains
/// Unknown. This is a view-level diagnostic, not pixel or residency proof.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_gi_target_count_fact(
    renderer: *mut WgrRenderer, output: *mut WgrMainTargetCountResult,
    output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.gi_target_count_fact(&renderer.device);
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

/// Nonblocking exact target-model COUNT for local shadow tile 0. Its cull index
/// follows the active solar cascades and can be zero at night. With the opt-in
/// publication witness, mapped COUNT follows its exact cached tile image.
/// Missing fresh cull/draw/closed pass and failed submit remain Unknown.
/// This is a diagnostic view count, never pixel or residency-release proof.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_local_shadow0_target_count_fact(
    renderer: *mut WgrRenderer, output: *mut WgrMainTargetCountResult,
    output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.local0_target_count_fact(&renderer.device);
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

/** Nonblocking exact target-model COUNT for the independently culled planar
 * reflection. Only an active, closed indirect object pass in its separately
 * submitted encoder can publish a result. Disabled/skipped views stay Unknown.
 * Diagnostic only; neither pixels nor residency release are certified.
 * # Safety
 * Same live renderer and exact writable output contract as the main getter.
 */
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_reflection_target_count_fact(
    renderer: *mut WgrRenderer, output: *mut WgrMainTargetCountResult,
    output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.reflection_target_count_fact(&renderer.device);
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

/// Nonblocking exact target-model COUNT for interior sky zenith view 0.
/// With the opt-in interior publication witness, a freshly culled, closed and
/// submitted layer binds the mapped COUNT to its exact cache generation. A
/// matching cached layer retains that fact and its original request token;
/// refresh, abort and source/key changes return Unknown. Without the witness,
/// only the fresh request may publish. GI sun-proxy view 5 cannot witness sky0.
/// Diagnostic only; neither pixels nor residency release are certified.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_sky0_target_count_fact(
    renderer: *mut WgrRenderer, output: *mut WgrMainTargetCountResult,
    output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.sky0_target_count_fact(&renderer.device);
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

/// Private nonblocking COUNT for interior directions 1..4. With the opt-in
/// interior publication witness, each mapped batch word is bound to its exact
/// published direction, cache/texture generation, view, epoch and source.
/// Otherwise only a fresh cull and closed indirect draw can produce a fact.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_interior_target_count_fact(
    renderer: *mut WgrRenderer, direction: u32,
    output: *mut WgrMainTargetCountResult, output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() || !(1..=4).contains(&direction) ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.batch_target_count_fact(&renderer.device, (direction - 1) as usize);
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

/// Private nonblocking COUNT for local shadow tiles 1..23. With the opt-in
/// publication witness, a mapped word remains bound to its exact tile, atlas
/// generation/shape, day/night cull route, source and original token.
/// # Safety
/// Same live renderer and exact writable output contract as the main getter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_local_shadow_target_count_fact(
    renderer: *mut WgrRenderer, tile: u32,
    output: *mut WgrMainTargetCountResult, output_size: u32, output_version: u32,
) -> u32 {
    if renderer.is_null() || output.is_null() || !(1..=23).contains(&tile) ||
        output_size != std::mem::size_of::<WgrMainTargetCountResult>() as u32 ||
        output_version != WgrMainTargetCountResult::VERSION { return 0; }
    let output = unsafe { &mut *output };
    *output = WgrMainTargetCountResult::empty();
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let fact = renderer.gfx3d.local_target_count_fact(&renderer.device, tile as usize);
        *output = WgrMainTargetCountResult::from_fact(fact);
    })).is_ok() as u32
}

unsafe fn render_frame_impl(
    renderer: *mut WgrRenderer, frame: *const WgrFrame,
    mut call_timings: Option<&mut WgrRenderCallCpuTimings>,
    main_count_request: Option<WgrMainTargetCountRequest>,
    mut main_count_output: Option<&mut WgrMainTargetCountResult>,
) -> i32 {
    if renderer.is_null() || frame.is_null() {
        if let Some(facts) = call_timings { facts.fail(); }
        return -1;
    }
    let mut main_count_submitted = false;
    let status = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        if let Some(tracker)=renderer.demand_view_snapshot.as_mut(){tracker.begin();}
        if main_count_request.is_some() { renderer.gfx3d.begin_view_reference_request(); }
        let frame = unsafe { &*frame };
        let cameras = unsafe { frame.cameras.as_slice() };
        let draws3d = unsafe { frame.draws3d.as_slice() };
        let verts = unsafe { frame.verts.as_slice() };
        let batches = unsafe { frame.batches.as_slice() };
        let cmds = unsafe { frame.cmds.as_slice() };
        let palette = unsafe { frame.palette.as_slice() };
        let shadow_casters = unsafe { frame.shadow_casters.as_slice() };
        let overlay_verts = unsafe { frame.overlay_verts.as_slice() };
        let overlay_indices = unsafe { frame.overlay_indices.as_slice() };
        let overlay_draws = unsafe { frame.overlay_draws.as_slice() };
        let terrain_nodes = unsafe { frame.terrain_nodes.as_slice() };
        let terrain_batches = unsafe { frame.terrain_batches.as_slice() };
        let lights = unsafe { frame.lights.as_slice() };
        let water_nodes = unsafe { frame.water_nodes.as_slice() };
        let water_batches = unsafe { frame.water_batches.as_slice() };
        let grass_batches = unsafe { frame.grass_batches.as_slice() };
        match renderer.render_frame(
            frame.clear,
            frame.fog_color.to_array(),
            cameras,
            draws3d,
            verts,
            batches,
            cmds,
            palette,
            &frame.shadow,
            shadow_casters,
            overlay_verts,
            overlay_indices,
            overlay_draws,
            terrain_nodes,
            terrain_batches,
            lights,
            water_nodes,
            water_batches,
            grass_batches,
            call_timings.as_deref_mut(),
            main_count_request.map(|request|
                (request.token, request.model_id, request.source_generation)),
        ) {
            Ok(submitted) => {
                // Selected interior-sky, GI RSM, and local depth images become published cache entries
                // only after the final queue submit and successful render return.
                renderer.gfx3d.commit_interior_publications();
                renderer.gfx3d.commit_gi_rsm_publication();
                renderer.gfx3d.commit_local_publications();
                if let Some(request) = main_count_request {
                    if submitted {
                        main_count_submitted = true;
                        renderer.gfx3d.main_target_count_commit(request.token);
                    } else { renderer.gfx3d.main_target_count_abort(request.token); }
                }
                0
            },
            Err(e) => {
                renderer
                    .log
                    .log(log_level::ERROR, &format!("render_frame: {e}"));
                -2
            }
        }
    }))
    .unwrap_or(-3);
    // Publish only after the outer render/commit closure returned actual FFI
    // success. Any renderer error/panic aborts the candidate and old completion.
    if let Some(tracker)=unsafe{&mut *renderer}.demand_view_snapshot.as_mut() {
        if status==0 {tracker.complete();} else {tracker.abort();}
    }
    if status != 0 { if let Some(facts) = call_timings { facts.fail(); } }
    if status != 0 && !renderer.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            unsafe { &mut *renderer }.gfx3d.abort_interior_publications();
        }));
        let _ = catch_unwind(AssertUnwindSafe(|| {
            unsafe { &mut *renderer }.gfx3d.abort_gi_rsm_publication();
        }));
        let _ = catch_unwind(AssertUnwindSafe(|| {
            unsafe { &mut *renderer }.gfx3d.abort_local_publications();
        }));
    }
    if let Some(request) = main_count_request {
        if status != 0 && !renderer.is_null() {
            let _ = catch_unwind(AssertUnwindSafe(|| {
                unsafe { &mut *renderer }.gfx3d.main_target_count_abort(request.token);
            }));
        }
        if let Some(output) = main_count_output.as_deref_mut() {
            output.submitted = u32::from(status == 0 && main_count_submitted);
            if status == 0 && main_count_submitted && !renderer.is_null() {
                let fact = unsafe { &*renderer }.gfx3d.main_target_count_snapshot();
                if fact.identity.token == request.token { output.cull_epoch = fact.identity.cull_epoch; }
            }
        }
    }
    status
}

/// Debug: read back the current auto-exposure scale (blocking GPU sync — dev panel
/// only). Returns 1.0 if the renderer is null or the HDR path is off.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_exposure_scale(renderer: *mut WgrRenderer) -> f32 {
    if renderer.is_null() {
        return 1.0;
    }
    let renderer = unsafe { &*renderer };
    renderer.exposure_scale()
}

/// WTR-002 — copy the latest completed-frame GPU pass timings into `out_ms` (milliseconds
/// per region, indexed by `WgrGpuTimerRegion`; -1 = the pass never ran / is reserved).
/// Non-blocking (values are harvested asynchronously each frame). Returns the region
/// count written (min of WGR_GPU_TIMER_REGION_COUNT and `out_len`), or 0 when the
/// renderer is null or the adapter lacks timestamp queries.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null; `out_ms` must point to at least
/// `out_len` floats, or be null (in which case 0 is returned).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_gpu_timings(
    renderer: *mut WgrRenderer,
    out_ms: *mut f32,
    out_len: u32,
) -> u32 {
    if renderer.is_null() || out_ms.is_null() || out_len == 0 {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        let out = unsafe { std::slice::from_raw_parts_mut(out_ms, out_len as usize) };
        let count = renderer.gpu_timings(out);
        count.min(out_len)
    }))
    .unwrap_or(0)
}

/// PERF-005 — copy the latest per-region CPU ENCODE times into `out_ms`, indexed by the SAME
/// `WgrGpuTimerRegion` values as `wgr_get_gpu_timings`. -1 = the region was not recorded this
/// frame. Unlike the GPU rows this needs no adapter feature and is never asynchronous: it is
/// this frame's wall-clock cost of RECORDING the region into the command encoder, which on a
/// GPU-driven renderer is a very different (and usually much smaller) number than the GPU cost.
/// Returns the region count written, or 0 for a null renderer/output.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null; `out_ms` must point to at least
/// `out_len` floats, or be null (in which case 0 is returned).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_cpu_timings(
    renderer: *mut WgrRenderer,
    out_ms: *mut f32,
    out_len: u32,
) -> u32 {
    if renderer.is_null() || out_ms.is_null() || out_len == 0 {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        let out = unsafe { std::slice::from_raw_parts_mut(out_ms, out_len as usize) };
        renderer.cpu_timings(out).min(out_len)
    }))
    .unwrap_or(0)
}

/// PERF-005 — per-frame object accounting, mirroring `WgrObjectStats` in wgpu_renderer.hpp.
/// The per-view blocks come from atomics written by cull.wgsl and read back through the same
/// non-blocking ring as the GPU timings (so they lag the displayed frame by ~2-3 frames); the
/// `direct_*` block is counted on the CPU during the colour replay and is current.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrObjectStats {
    /// Retained instances registered with the cull (the denominator for the survivor counts).
    pub registered_instances: u32,
    /// 1 once a readback has landed. Zero counts with valid = 0 mean "not measured yet",
    /// which is a different statement from "nothing drew".
    pub valid: u32,

    /// Main view = frustum + distance + LOD only. This is the PREPASS / occluder set.
    pub main_instances: u32,
    pub main_records: u32,
    pub main_draws: u32,
    pub main_tris: u32,

    /// Colour view = the main tests plus Hi-Z occlusion. All zero when occlusion is off,
    /// in which case the colour pass reuses the main view's args and main_* is the truth.
    pub color_instances: u32,
    pub color_records: u32,
    pub color_draws: u32,
    pub color_tris: u32,
    pub color_draws_solid: u32,
    pub color_draws_alpha: u32,
    pub color_tris_solid: u32,
    pub color_tris_alpha: u32,

    /// Per shadow cascade (index = cascade). Cascades beyond the active count read zero.
    pub shadow_instances: [u32; 4],
    pub shadow_draws: [u32; 4],
    pub shadow_tris: [u32; 4],

    /// Selected-LOD histogram over the MAIN view's survivors: how many instances chose LOD 0,
    /// 1, ... 6, and bucket 7 = "LOD 7 or coarser". The direct test of whether detailed
    /// building/vegetation LODs stay active too far out.
    pub main_lod_hist: [u32; 8],
    /// REN-VEG-004: the main view's triangle total if lod_inv_width were scaled by
    /// whatif_scale[i], over the same survivors. What the LOD governor steers on.
    pub main_whatif_tris: [u32; 4],
    pub whatif_scale: [f32; 4],
    /// REN-OBJ-003: object fragment census (colour, colour cutout, prepass, prepass cutout);
    /// zeros unless WGR_OBJECT_COUNT_FRAGMENTS=1 (then timings are not quotable).
    /// REN-ATM-001 owns words 4..7: shaded fragments that received the aerial-perspective term,
    /// split opaque / cutout / vegetation-cutout, then the ones that received none. Word map and
    /// reasoning in gpu_driven.wgsl.
    pub fragments: [u32; 8],

    /// CPU-replayed object path (never reaches the cull shader).
    pub direct_calls: u32,
    pub direct_indirect_calls: u32,
    pub direct_instances: u32,
    pub direct_tris: u32,

    /// Instance handles used after their instance was removed, refused and counted since
    /// startup (ID-3). The table has counted these all along; nothing outside its own unit
    /// tests could read the number, and only the first eight were ever logged -- so a
    /// sustained identity bug was invisible after the eighth occurrence. Nonzero is a
    /// C++-side bug, never a renderer one.
    pub stale_instance_ops: u64,
}

/// LGT-026 — how many local-light shadow views the last frame RE-RENDERED, and how many it
/// re-used from the cache. Returns 1 on success, 0 when the renderer or either out pointer is
/// null. The cache is invisible in the picture by construction (a correct cache changes
/// nothing anyone can see), so this pair is the only way to tell a working one from a broken
/// one, or from one that never hits.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null; `rendered` and `cached` must point at
/// valid `u32`s or be null (in which case 0 is returned).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_local_shadow_stats(
    renderer: *mut WgrRenderer,
    rendered: *mut u32,
    cached: *mut u32,
) -> u32 {
    if renderer.is_null() || rendered.is_null() || cached.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        let (r, c) = renderer.local_shadow_view_stats();
        unsafe {
            *rendered = r;
            *cached = c;
        }
        1
    }))
    .unwrap_or(0)
}

/// Fill `out` with this frame's object accounting. Returns 1 on success, 0 when the
/// renderer or `out` is null.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null; `out` must point to a valid
/// `WgrObjectStats`, or be null (in which case 0 is returned).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_object_stats(
    renderer: *mut WgrRenderer,
    out: *mut WgrObjectStats,
) -> u32 {
    if renderer.is_null() || out.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        use crate::gfx3d::cull::{STATS_VIEW_COLOR, STATS_VIEW_MAIN, STATS_VIEW_SHADOW0};
        let renderer = unsafe { &*renderer };
        let (cull, direct, registered, fragments) = renderer.object_stats();
        let main = cull.views[STATS_VIEW_MAIN];
        let color = cull.views[STATS_VIEW_COLOR];
        let mut s = WgrObjectStats {
            registered_instances: registered,
            valid: u32::from(cull.valid),
            main_instances: main.instances,
            main_records: main.records,
            main_draws: main.draws,
            main_tris: main.tris,
            color_instances: color.instances,
            color_records: color.records,
            color_draws: color.draws,
            color_tris: color.tris,
            color_draws_solid: color.draws_solid,
            color_draws_alpha: color.draws_alpha,
            color_tris_solid: color.tris_solid,
            color_tris_alpha: color.tris_alpha,
            main_lod_hist: main.lod_hist,
            main_whatif_tris: main.whatif_tris,
            whatif_scale: crate::gfx3d::cull::WHATIF_SCALES,
            fragments,
            direct_calls: direct.direct_calls,
            direct_indirect_calls: direct.indirect_calls,
            direct_instances: direct.instances,
            direct_tris: direct.tris,
            stale_instance_ops: renderer.stale_instance_ops(),
            ..Default::default()
        };
        for c in 0..4usize {
            let v = cull.views[STATS_VIEW_SHADOW0 + c];
            s.shadow_instances[c] = v.instances;
            s.shadow_draws[c] = v.draws;
            s.shadow_tris[c] = v.tris;
        }
        unsafe { *out = s };
        1
    }))
    .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_runtime_capabilities(renderer: *mut WgrRenderer) -> u32 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        unsafe { &*renderer }.runtime_capabilities()
    }))
    .unwrap_or(0)
}

/// The DLSS status sentence for the dev panel and the capture metrics: "active", or
/// WHY DLSS is inactive (dlss_status.rs). Copies a NUL-terminated string into `buf`
/// (truncated to `cap`) and returns the full length in bytes; 0 when nothing was
/// written. Optional export — the engine fetches it through GetProcAddress, like
/// `wgr_get_runtime_capabilities`, so an older DLL still loads.
///
/// # Safety
/// `renderer` must be live or null; `buf` must be writable for `cap` bytes or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_dlss_status(
    renderer: *mut WgrRenderer,
    buf: *mut c_char,
    cap: usize,
) -> u32 {
    if renderer.is_null() || buf.is_null() || cap == 0 {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let text = unsafe { &*renderer }.dlss_status_text();
        let bytes = text.as_bytes();
        let n = bytes.len().min(cap - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0;
        }
        bytes.len() as u32
    }))
    .unwrap_or(0)
}

/// GRS-A — grass instance accounting for the Grass tab, mirroring `WgrGrassStats`
/// in wgpu_renderer.hpp. Counts come from a non-blocking readback of the three
/// atomic placement counters, so they lag the displayed frame by the ring depth
/// (~2-3 frames), exactly like the GPU timings.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrGrassStats {
    pub near_instances: u32,
    pub mid_instances: u32,
    pub far_instances: u32,
    pub near_candidates: u32,
    pub mid_candidates: u32,
    pub far_candidates: u32,
    pub near_vertices: u32,
    pub mid_vertices: u32,
    pub far_vertices: u32,
}

/// Dynamic WGPU residency lower bound. The byte fields count portable resource
/// payload/capacity owned by the renderer; driver-private alignment and metadata
/// are not observable through WebGPU and are therefore not included.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrMemoryStats {
    pub tracked_bytes: u64,
    pub budget_bytes: u64,
    pub object_texture_bytes: u64,
    pub geometry_live_bytes: u64,
    pub geometry_capacity_bytes: u64,
    pub geometry_retired_bytes: u64,
    pub object_texture_count: u32,
    pub over_budget: u32,
    pub object_texture_retired_bytes: u64,
    pub backend_allocation_bytes: u64, // buffers + textures, including non-streamed render resources
}

/// Whole live mesh ranges referenced by one retained drawable LOD. Rows can overlap.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrGeometryAllocationRow {
    pub vertex_bytes: u64,
    pub index_bytes: u64,
    pub model_id: u32,
    pub lod_index: u32,
    pub resolution: f32,
    pub section_count: u32,
    pub live_meshes: u32,
    pub missing_meshes: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrGeometryAllocationSummary {
    pub unique_vertex_bytes: u64,
    pub unique_index_bytes: u64,
    pub pool_live_bytes: u64,
    pub pool_capacity_bytes: u64,
    pub pool_retired_bytes: u64,
    pub unique_meshes: u32,
    pub models_visited: u32,
    pub lods_total: u32, // only models actually visited, not whole renderer inventory
    pub section_visits: u32,
    pub missing_models: u32,
    pub missing_meshes: u32,
    pub invalid_ranges: u32,
    pub truncated: u32,
    pub rows_written: u32,
    /// Unique allocations referenced by >1 distinct (model, LOD) among fully
    /// inspected rows only. Not global ownership, exclusive or reclaimable bytes.
    pub overlap_within_inspected_meshes: u64,
    pub overlap_within_inspected_vertex_bytes: u64,
    pub overlap_within_inspected_index_bytes: u64,
}

/// Additive owner-only diagnostic types, separate from the allocation-report ABI.
/// State: 0 invalid, 1 present generational record, 2 absent. Absence is not a
/// queue/fence/device-memory retirement acknowledgement.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrMeshHandleFact {
    pub mesh_handle: u64,
    pub vertex_bytes: u64,
    pub index_bytes: u64,
    pub state: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrMeshHandleFactSummary {
    pub handles_requested: u32,
    pub handles_inspected: u32,
    pub present: u32,
    pub absent: u32,
    pub invalid: u32,
    pub duplicate_handles: u32,
    pub complete: u32,
    pub flags: u32, // 1 output/work truncation, 2 byte-sum overflow
    pub unique_vertex_bytes: u64,
    pub unique_index_bytes: u64,
    pub live_mesh_records: u64,
    pub pool_live_bytes: u64,
    pub pool_generation: u64,
    pub record_scope_valid: u32,
    pub reserved: u32,
}

/// Requested full-generation handle's registered section references. flags bit
/// 0 means it has observed registered references but its current mesh record is
/// absent. Absent unreferenced handles are unflagged. Zero refs do NOT mean unowned.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrRegisteredMeshRef {
    pub mesh: u64,
    pub distinct_models: u64,
    pub distinct_model_lods: u64,
    pub section_occurrences: u64,
    pub flags: u32,
    pub reserved: u32,
}

/// Complete ONLY for current model/LOD/section census within declared budgets;
/// not a global lifetime/ownership proof. missing_records counts distinct
/// REQUESTED referenced handles absent by full generation, not occurrences.
/// refusal_flags: bit0 visit capacity, bit1 invalid span, bit2 count overflow.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrRegisteredMeshRefSummary {
    pub models_visited: u64,
    pub lods_visited: u64,
    pub sections_visited: u64,
    pub missing_records: u64,
    pub rows_written: u32,
    pub complete: u32,
    pub refusal_flags: u32,
    pub reserved: u32,
}

/// Immutable owner-cut metadata plus asynchronous queue barrier state. Epoch is
/// the caller's diagnostic owner epoch; pool generation is the actual pool epoch.
/// Acknowledged covers preceding queue work, NOT device free or future references.
/// States: Disabled0, Queued1, Submitted2, Acknowledged3, Cancelled4,
/// Invalid5, Busy6, Unknown7. New optional type; existing ABI14 types unchanged.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrMeshSnapshotAck {
    pub ticket: u64,
    pub epoch: u64,
    pub pool_generation: u64,
    pub main_submission_serial: u64,
    pub pool_live_bytes: u64,
    pub pool_capacity_bytes: u64,
    pub pool_retired_bytes: u64,
    pub state: u32,
    pub row_count: u32,
}

/// Monotonic renderer handles only; not an asset identity or ownership proof.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrLodDemandRow {
    pub model_id: u32,
    pub lod_count: u32,
    pub lod_mask: u32,
    pub state: u32, // 0 observed/live, 1 missing, 2 retired, 3 unsupported LOD count
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrLodDemandReport {
    pub epoch: u64,
    pub current_frame: u64,
    pub last_sample_frame: u64,
    pub status: u32, // 1 sampling/draining, 2 completed, 3 cancelled
    pub frames_requested: u32,
    pub frames_attempted: u32,
    pub frames_sampled: u32,
    pub dropped_frames: u32,
    pub map_failures: u32,
    pub dropped_dispatches: u32,
    pub dispatched_views: u32,
    pub pass_mask: u32, // actually dispatched prepass/color/shadow/reflection/interior: bits0..4
    pub row_count: u32,
    pub rows: [WgrLodDemandRow; 8],
}

/// Explicit renderer-owner-only request. Zero=unsupported,1=started,2=invalid,3=busy.
/// # Safety
/// Renderer and readable models must be valid and exclusively owner accessed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_start_lod_demand(renderer: *mut WgrRenderer, epoch: u64,
    models: WgrSlice<u32>, frames: u32) -> u32 {
    if renderer.is_null() || models.data.is_null() || models.len == 0 || models.len > 8 || epoch == 0 || frames == 0 || frames > 64 { return 2; }
    catch_unwind(AssertUnwindSafe(|| {
        let r = unsafe { &mut *renderer };
        r.gfx3d.start_lod_demand(&r.device, epoch, unsafe { std::slice::from_raw_parts(models.data, models.len as usize) }, frames)
    })).unwrap_or(2)
}
/// # Safety
/// Renderer must be exclusively owner accessed; out must be valid writable memory.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_poll_lod_demand(renderer: *mut WgrRenderer, epoch: u64, out: *mut WgrLodDemandReport) -> u32 {
    if renderer.is_null() || out.is_null() || epoch == 0 { return 0; }
    catch_unwind(AssertUnwindSafe(|| {
        let Some(report) = unsafe { &*renderer }.gfx3d.lod_demand_report(epoch) else { return 0; };
        unsafe { *out = report; } 1
    })).unwrap_or(0)
}
/// # Safety
/// Renderer must be valid and exclusively owner accessed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_cancel_lod_demand(renderer: *mut WgrRenderer, epoch: u64) {
    if renderer.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe { &mut *renderer }.gfx3d.cancel_lod_demand(epoch)));
}

/// Read-only renderer-owner snapshot. No waits/readback; bounded metadata visits only.
/// Returns zero for invalid input. Pool capacity/retired bytes are global, not per asset.
/// # Safety
/// Renderer must be exclusively renderer-owner accessed. Models, rows and summary must
/// be valid readable/writable allocations of their declared lengths, with no aliasing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_geometry_allocation_report(
    renderer: *mut WgrRenderer, models: WgrSlice<u32>, max_rows: u32, max_visits: u32,
    rows: *mut WgrGeometryAllocationRow, row_capacity: u32,
    summary: *mut WgrGeometryAllocationSummary,
) -> u32 {
    if renderer.is_null() || summary.is_null() || rows.is_null() || models.len > 256 ||
        (models.len != 0 && models.data.is_null()) || max_rows == 0 || max_rows > 2048 ||
        max_visits == 0 || max_visits > 65536 || row_capacity < max_rows { return 0 }
    catch_unwind(AssertUnwindSafe(|| {
        let ids = if models.len == 0 { &[][..] } else {
            unsafe { std::slice::from_raw_parts(models.data, models.len as usize) }
        };
        let (result, totals) = unsafe { &*renderer }.gfx3d.geometry_allocation_report(
            ids, max_rows as usize, max_visits as usize);
        unsafe { std::ptr::copy_nonoverlapping(result.as_ptr(), rows, result.len()); *summary = totals; }
        1
    })).unwrap_or(0)
}

/// Explicit read-only renderer-owner probe, hard capped at 8192 handles. No wait,
/// deferred-destroy drain, queue submission or lifetime mutation. Present byte
/// counts describe actual current source ranges, not device-memory ownership.
/// # Safety
/// Renderer must be exclusively owner accessed. Inputs/outputs must be valid,
/// disjoint allocations of their declared lengths; summary must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_mesh_handle_facts(
    renderer: *mut WgrRenderer, handles: WgrSlice<u64>, facts: *mut WgrMeshHandleFact,
    fact_capacity: u32, summary: *mut WgrMeshHandleFactSummary,
) -> u32 {
    if renderer.is_null() || summary.is_null() || handles.len > 8192 || fact_capacity > 8192 ||
        fact_capacity < handles.len || (handles.len != 0 && (handles.data.is_null() || facts.is_null())) { return 0; }
    catch_unwind(AssertUnwindSafe(|| {
        let ids = if handles.len == 0 { &[][..] } else {
            unsafe { std::slice::from_raw_parts(handles.data, handles.len as usize) }
        };
        let out = if handles.len == 0 { &mut [][..] } else {
            unsafe { std::slice::from_raw_parts_mut(facts, handles.len as usize) }
        };
        let totals = unsafe { &*renderer }.gfx3d.mesh_handle_facts(ids, out);
        unsafe { *summary = totals; }
        1
    })).unwrap_or(0)
}

/// Startup opt-in WGR_GEOMETRY_REGISTERED_REFS=1. Additive optional ABI: no
/// existing report layout/hash changes. 0 Disabled, 1 Collected (may incomplete),
/// 2 Invalid/refused. Disabled/invalid outputs untouched. No queue wait/poll or
/// resource mutation. Caller must separately enforce owner-ledger prerequisite.
/// # Safety
/// Exclusive renderer owner; declared slices/output summary valid and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_registered_mesh_refs(
    renderer: *mut WgrRenderer, handles: WgrSlice<u64>, rows: *mut WgrRegisteredMeshRef,
    capacity: u32, summary: *mut WgrRegisteredMeshRefSummary,
) -> u32 {
    if renderer.is_null() || summary.is_null() || handles.len > 8192 || capacity > 8192 ||
        capacity < handles.len || (handles.len != 0 && (handles.data.is_null() || rows.is_null())) { return 2; }
    catch_unwind(AssertUnwindSafe(|| {
        let ids = if handles.len == 0 { &[][..] } else {
            unsafe { std::slice::from_raw_parts(handles.data, handles.len as usize) }
        };
        let output = if handles.len == 0 { &mut [][..] } else {
            unsafe { std::slice::from_raw_parts_mut(rows, handles.len as usize) }
        };
        unsafe { &*renderer }.gfx3d.registered_mesh_refs(ids, output, unsafe { &mut *summary })
    })).unwrap_or(2)
}

/// Request a bounded immutable mesh snapshot and a barrier at the NEXT main
/// submission. Direct state return; errors leave rows/summary untouched and set
/// ack.state when ack is valid. Exact startup WGR_GEOMETRY_MESH_ACK=1 only.
/// # Safety
/// Exclusive renderer owner. All declared inputs/outputs are valid disjoint
/// allocations; non-null summary/ack required. No queue waits or lifetime policy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_request_mesh_snapshot_ack(
    renderer: *mut WgrRenderer, epoch: u64, handles: WgrSlice<u64>,
    facts: *mut WgrMeshHandleFact, fact_capacity: u32,
    summary: *mut WgrMeshHandleFactSummary, ack: *mut WgrMeshSnapshotAck,
) -> u32 {
    if !ack.is_null() { unsafe { *ack = WgrMeshSnapshotAck { state: 5, ..Default::default() }; } }
    if renderer.is_null() || summary.is_null() || ack.is_null() || epoch == 0 ||
        handles.len > 8192 || fact_capacity > 8192 || fact_capacity < handles.len ||
        (handles.len != 0 && (handles.data.is_null() || facts.is_null())) { return 5; }
    catch_unwind(AssertUnwindSafe(|| {
        let ids = if handles.len == 0 { &[][..] } else {
            unsafe { std::slice::from_raw_parts(handles.data, handles.len as usize) }
        };
        let out = if handles.len == 0 { &mut [][..] } else {
            unsafe { std::slice::from_raw_parts_mut(facts, handles.len as usize) }
        };
        unsafe { &mut *renderer }.gfx3d.request_mesh_snapshot_ack(epoch, ids, out,
            unsafe { &mut *summary }, unsafe { &mut *ack })
    })).unwrap_or_else(|_| { unsafe { *ack = WgrMeshSnapshotAck { state: 5, ..Default::default() }; } 5 })
}

/// Poll only the immutable captured snapshot and callback state. No fresh mesh
/// lookup, submission, device poll or wait. facts=null/capacity0 means summary-only.
/// # Safety
/// Same exclusive owner/disjoint-output requirements as request. When supplied,
/// facts must have capacity for every captured row; no partial output is returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_poll_mesh_snapshot_ack(
    renderer: *mut WgrRenderer, ticket: u64, facts: *mut WgrMeshHandleFact,
    fact_capacity: u32, summary: *mut WgrMeshHandleFactSummary, ack: *mut WgrMeshSnapshotAck,
) -> u32 {
    if !ack.is_null() { unsafe { *ack = WgrMeshSnapshotAck { state: 5, ..Default::default() }; } }
    if renderer.is_null() || summary.is_null() || ack.is_null() || ticket == 0 ||
        fact_capacity > 8192 || (facts.is_null() && fact_capacity != 0) { return 5; }
    catch_unwind(AssertUnwindSafe(|| {
        let out = if facts.is_null() && fact_capacity == 0 { None } else {
            Some(if fact_capacity == 0 { &mut [][..] } else {
                unsafe { std::slice::from_raw_parts_mut(facts, fact_capacity as usize) }
            })
        };
        unsafe { &*renderer }.gfx3d.poll_mesh_snapshot_ack(ticket, out,
            unsafe { &mut *summary }, unsafe { &mut *ack })
    })).unwrap_or_else(|_| { unsafe { *ack = WgrMeshSnapshotAck { state: 5, ..Default::default() }; } 5 })
}

/// Release/cancel a ticket. A late callback only changes its detached atomic,
/// never a newer ticket. Last three cancelled IDs remain distinguishable.
/// # Safety
/// Renderer is exclusively owner-accessed; no callback reentry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_cancel_mesh_snapshot_ack(renderer: *mut WgrRenderer, ticket: u64) -> u32 {
    if renderer.is_null() || ticket == 0 { return 5; }
    catch_unwind(AssertUnwindSafe(|| unsafe { &mut *renderer }.gfx3d.cancel_mesh_snapshot_ack(ticket))).unwrap_or(5)
}

/// Fill `out` with the latest grass instance counts. Returns 1 on success, 0 when
/// the renderer or `out` is null.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null; `out` must point to a valid
/// `WgrGrassStats`, or be null (in which case 0 is returned).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_grass_stats(
    renderer: *mut WgrRenderer,
    out: *mut WgrGrassStats,
) -> u32 {
    if renderer.is_null() || out.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        let s = renderer.grass_stats();
        unsafe {
            *out = WgrGrassStats {
                near_instances: s.near_instances,
                mid_instances: s.mid_instances,
                far_instances: s.far_instances,
                near_candidates: s.near_candidates,
                mid_candidates: s.mid_candidates,
                far_candidates: s.far_candidates,
                near_vertices: s.near_vertices,
                mid_vertices: s.mid_vertices,
                far_vertices: s.far_vertices,
            };
        }
        1
    }))
    .unwrap_or(0)
}

/// Fill `out` with the current dynamic WGPU residency accounting. Returns 1 on
/// success and 0 for a null renderer/output pointer.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null; `out` must point to one
/// writable `WgrMemoryStats` or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_memory_stats(
    renderer: *mut WgrRenderer,
    out: *mut WgrMemoryStats,
) -> u32 {
    if renderer.is_null() || out.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        unsafe { *out = renderer.memory_stats() };
        1
    }))
    .unwrap_or(0)
}

/// AST-012A — the shape of one mip-feedback harvest, alongside the per-slot array.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgrMipFeedbackStats {
    /// Renderer frame index at the moment of the query (the age clock's origin).
    pub frame: u64,
    /// Readbacks that actually completed and were decoded since startup. ZERO means the
    /// instrument never produced a sample, which is a different statement from "every slot
    /// reads unobserved" and must not be reported as the latter.
    pub harvests: u64,
    /// Bindless slots the array covers.
    pub slots: u32,
    /// Of those, how many carry any observation at all.
    pub observed: u32,
    /// Frames one full rotation of the texture table takes; 0 = feedback disabled.
    pub groups: u32,
    pub _pad: u32,
}

/// AST-012A — copy the latest per-bindless-slot mip feedback out of the renderer.
///
/// `out_mip[slot]` receives the finest mip level any fragment asked of that slot, or 0xFF when
/// the slot has never been observed. `out_age[slot]` receives how many frames ago that
/// observation was harvested, saturating at 0xFFFF. Either output may be null.
///
/// Non-blocking: the values come from a readback ring that is mapped asynchronously, so they
/// trail the current frame by a few frames and, because only one rotation group writes per
/// frame, a given slot's answer can be a full rotation old. Both facts are fine for a residency
/// decision whose grace is measured in hundreds of frames, and neither is fine for anything
/// that must be exact this frame.
///
/// Returns the number of slots written.
///
/// # Safety
/// `renderer` must be a live `WgrRenderer` or null. `out_mip` / `out_age`, when non-null, must
/// each point at `max_slots` writable elements. `stats`, when non-null, at one WgrMipFeedbackStats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_mip_feedback(
    renderer: *mut WgrRenderer,
    stats: *mut WgrMipFeedbackStats,
    out_mip: *mut u8,
    out_age: *mut u16,
    max_slots: u32,
) -> u32 {
    if renderer.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        let fb = renderer.gfx3d.mip_feedback_state();
        let frame = fb.frame();
        let n = max_slots.min(fb.capacity());
        let mut observed = 0u32;
        for slot in 0..n {
            let obs = fb.observation(slot);
            let seen = obs.frame != 0;
            if seen {
                observed += 1;
            }
            if !out_mip.is_null() {
                let v = if seen { obs.desired_mip.min(254) as u8 } else { 0xFF };
                unsafe { *out_mip.add(slot as usize) = v };
            }
            if !out_age.is_null() {
                let v = if seen {
                    frame.saturating_sub(obs.frame).min(0xFFFF) as u16
                } else {
                    0xFFFF
                };
                unsafe { *out_age.add(slot as usize) = v };
            }
        }
        if !stats.is_null() {
            unsafe {
                *stats = WgrMipFeedbackStats {
                    frame,
                    harvests: fb.harvest_count(),
                    slots: n,
                    observed,
                    groups: fb.lane().1,
                    _pad: 0,
                }
            };
        }
        n
    }))
    .unwrap_or(0)
}

/// Push the consolidated ImGui-tweakable render params (tonemap, exposure, sky look, terrain
/// sun-shadow, sky-visibility) in one block. Fans out to the per-subsystem state; the terrain
/// setters are diffed against the last block so a per-frame push doesn't thrash the sweep/scan.
/// See docs/render-params-consolidation-plan.md.
///
/// # Safety
/// `renderer` must be live; `params` must point to one valid `WgrRenderParams` or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_render_params(
    renderer: *mut WgrRenderer,
    params: *const WgrRenderParams,
) {
    if renderer.is_null() || params.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let params = unsafe { *params };
        renderer.set_render_params(params);
    }));
}

/// Set the AO march resolution: 1 = one horizon march per pixel, 2 = one per 2x2 block,
/// bilateral-upsampled back to full resolution by the AO denoise. Clamped to 1..=2.
///
/// Deliberately its own entry point rather than a field of `WgrGtao`. That struct is rebuilt and
/// pushed by C++ every frame, so a value carried in it would be owned by whatever C++ happens to
/// hold — and the shipped default here (2) is the renderer's, chosen because the per-pixel march
/// measured as the frame's most resolution-hungry pass. `WGR_GTAO_SCALE` seeds it; this call is
/// what a dev-panel AO quality control drives.
///
/// # Safety
/// `renderer` must be a live renderer handle or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_gtao_scale(renderer: *mut WgrRenderer, scale: u32) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.set_gtao_scale(scale);
    }));
}

/// RFG-085: the sky-bake gate (`Gfx3d::finish_sky_bake`): a model's sky-visibility volume is
/// kept only when at least `min_enclosed` of its voxels see less than half the sky, and only
/// while the kept volumes total under `budget_mb`. Seeded from WGR_SKY_BAKE_MIN_ENCLOSED /
/// WGR_SKY_VOLUME_MB; the dev panel drives it from here.
///
/// # Safety
/// `renderer` must be a live renderer handle or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_sky_bake_gate(renderer: *mut WgrRenderer, min_enclosed: f32, budget_mb: u32) {
    if renderer.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        renderer.set_sky_bake_gate(min_enclosed, budget_mb);
    }));
}

/// The current AO march resolution (see `wgr_set_gtao_scale`), so a dev panel can show the live
/// value rather than a control that drifts out of sync with what the renderer is doing.
///
/// # Safety
/// `renderer` must be a live renderer handle or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_get_gtao_scale(renderer: *const WgrRenderer) -> u32 {
    if renderer.is_null() {
        return 1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &*renderer };
        renderer.gtao_scale()
    }))
    .unwrap_or(1)
}

/// Push the per-frame sky runtime (celestial direction/phase, night factor, fog colour, camera
/// altitude, fog range). Writes the runtime half of the sky UBO; the authored look half comes
/// from wgr_set_render_params. See docs/render-params-consolidation-plan.md.
///
/// # Safety
/// `renderer` must be live; `params` must point to one valid `WgrSkyRuntime` or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_sky_runtime(
    renderer: *mut WgrRenderer,
    params: *const WgrSkyRuntime,
) {
    if renderer.is_null() || params.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    let params = unsafe { &*params };
    log_moon_runtime(renderer, params);
    renderer.set_sky_runtime(*params);
}

// Report what the MOON half of the sky runtime actually looks like as it crosses the FFI.
//
// This exists because "the disc is never drawn" and "the disc is drawn somewhere I am not
// looking" produce identical screenshots, and nothing else in the pipeline distinguishes
// them: every other WGR_* gate in this codebase logs what it resolved to, and the moon did
// not. Logging HERE rather than on the C++ side is deliberate — this is the last point
// before the values become a GPU buffer, so a line here separates "C++ computed the wrong
// thing" from "the shader ignored the right thing" with no ambiguity left.
//
// Rate-limited to changes (with a coarse epsilon) so a static night scene prints once
// rather than sixty times a second, but a converging smoothed direction still shows its
// trajectory.
fn log_moon_runtime(renderer: &WgrRenderer, rt: &WgrSkyRuntime) {
    use std::sync::Mutex;
    static LAST: Mutex<Option<[f32; 8]>> = Mutex::new(None);

    let now = [
        rt.moon_dir[0],
        rt.moon_dir[1],
        rt.moon_dir[2],
        rt.moon_params[0],
        rt.moon_params[1],
        rt.moon_params[2],
        rt.moon_params[3],
        rt.moon_sun[3],
    ];
    let Ok(mut last) = LAST.lock() else {
        return;
    };
    let changed = match *last {
        None => true,
        // The draw flag is the single most diagnostic bit, so any flip always prints.
        Some(p) => {
            p[6] != now[6]
                || p.iter()
                    .zip(now.iter())
                    .any(|(a, b)| (a - b).abs() > a.abs().max(1.0e-3) * 0.02)
        }
    };
    if !changed {
        return;
    }
    *last = Some(now);
    drop(last);

    // Azimuth from north toward east and elevation, matching both the engine's freefly
    // readout (atan2(dir.x, dir.z)) and WGR_MOON_AZEL, so the number printed here can be
    // compared directly with the one that was asked for.
    let (mx, my, mz) = (rt.moon_dir[0], rt.moon_dir[1], rt.moon_dir[2]);
    let mut az = mx.atan2(mz).to_degrees();
    if az < 0.0 {
        az += 360.0;
    }
    let horiz = (mx * mx + mz * mz).sqrt().max(1.0e-6);
    let el = my.atan2(horiz).to_degrees();
    let sun_el = rt.sun_dir[1].asin().to_degrees();

    // Cloud coverage rides the LOOK half of the same UBO. It is printed here because a fully
    // drawn disc behind an overcast deck and a disc that was never drawn look identical from
    // outside: fs_cloud_composite multiplies the sky (disc included) by the deck's
    // transmittance, so a thick deck hides the moon completely and correctly.
    let coverage = renderer.sky_params.cloud0[0];
    let night = renderer.sky_params.ground_albedo[3];

    renderer.log.log(
        log_level::INFO,
        &format!(
            "moon: dir=({:.4},{:.4},{:.4}) az={:.1} el={:.1} | lit={:.3} radius={:.5}rad ({:.3}deg) \
             discScale={:.6} draw={} earthshine={:.4} | sunEl={:.1} night={:.2} cloudCover={:.2}",
            mx,
            my,
            mz,
            az,
            el,
            rt.moon_params[1],
            rt.moon_params[0],
            rt.moon_params[0].to_degrees(),
            rt.moon_params[2],
            if rt.moon_params[3] >= 0.5 { "YES" } else { "NO" },
            rt.moon_sun[3],
            sun_el,
            night,
            coverage,
        ),
    );
}

/// How much wider the planar water reflection's frustum is than the screen's. 1 = the old
/// behaviour, where a grazing reflection ran off the edge of the reflection target and the
/// reflected clouds ended in a visible line across the water. Higher covers more angle at
/// proportionally lower angular resolution.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_planar_reflection_pad(renderer: *mut WgrRenderer, pad: f32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_planar_reflection_pad(pad);
}

/// Brightness of the procedural star field. 0 = none. Gated to night by sun altitude in the
/// shader, so this never affects a daytime sky.
///
/// Lens-flare gain. 0 disables the effect; the shader early-outs on it.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_lens_flare(renderer: *mut WgrRenderer, intensity: f32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_lens_flare(intensity);
}

/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_star_intensity(renderer: *mut WgrRenderer, intensity: f32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_star_intensity(intensity);
}

/// CLD-020: strength of the cloud shadow cast on the ground. 0 = off, 1 = the deck's full
/// computed transmittance. A separate entry point rather than a new field in WgrSkyLook,
/// because growing that struct changes a size the ABI handshake checks, and this is one float.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_cloud_shadow_strength(renderer: *mut WgrRenderer, strength: f32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_cloud_shadow_strength(strength);
}

/// Photo-card placement grid in metres. The map's own clutter grid (1.11 m on the A2/A3
/// worlds) is the default and the natural maximum density for "one card per clutter
/// object"; going below it packs more cards than the map declares, which is what makes
/// the photographed clumps reach procedural-grass density. Clamped to 0.16..4.0.
///
/// Its own entry point rather than a WgrGrassParams field: that struct's size is part of
/// the ABI handshake, and this is one float.
///
/// # Safety
/// `renderer` must be a live renderer or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_set_card_spacing(renderer: *mut WgrRenderer, spacing: f32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_grass_card_spacing(spacing);
}

/// RFG-090: photo-card tone normalisation on/off. Off when the clutter atlas is a Reforger
/// world's own BCR albedo, which needs no plate normalisation and was being bleached by it.
///
/// # Safety
/// `renderer` must be a live renderer or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_grass_set_card_tone_enabled(renderer: *mut WgrRenderer, enabled: u32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_grass_card_tone_enabled(enabled != 0);
}

/// One smoke particle for the ground-shadow pass. Layout matches
/// `sky::SmokeShadowBlob` and the C `WgrSmokeShadowBlob` exactly (32 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WgrSmokeShadowBlob {
    /// xyz world position, w radius (m)
    pub pos_radius: [f32; 4],
    /// x optical density, y height above ground (m), zw unused
    pub density: [f32; 4],
}

/// Publish this frame's smoke particles to the ground-shadow pass. `blobs` may be null with
/// `count` 0 to clear. Capped at `sky::SMOKE_SHADOW_MAX_BLOBS`; the caller is expected to
/// pre-select the densest / nearest particles if it has more.
///
/// # Safety
/// `blobs` must point at `count` valid `WgrSmokeShadowBlob`s, or be null with `count == 0`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_smoke_shadow(
    renderer: *mut WgrRenderer,
    blobs: *const WgrSmokeShadowBlob,
    count: u32,
    strength: f32,
) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    let slice: &[WgrSmokeShadowBlob] = if blobs.is_null() || count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(blobs, count as usize) }
    };
    // Same layout; reinterpret rather than copy.
    let as_sky: &[crate::sky::SmokeShadowBlob] = unsafe {
        std::slice::from_raw_parts(
            slice.as_ptr() as *const crate::sky::SmokeShadowBlob,
            slice.len(),
        )
    };
    renderer.set_smoke_shadow(as_sky, strength);
}

/// Second cloud layer (high cirrus at ~7 km): 0 = off, 1 = flat sheet, 2 = thin-shell volumetric
/// march. Its own entry point for the same reason as `wgr_set_cloud_shadow_strength` above —
/// WgrSkyLook's size is part of the ABI handshake, and this is one enum. Values above 2 clamp.
///
/// # Safety
/// Material Debug state for the RETAINED path (dev panel, Materials tab).
/// `view`: 0 full material, 1 base colour only, 2 decoded normal, 3 screen AO,
/// 4 specular/gloss, 5 UV source, 6 lighting only.
/// `flags`: bit0 disable normal map, bit1 invert normal Y, bit2 compose Multi layers.
///
/// These are presentation switches read per fragment, NOT baked into the material
/// records -- baking them is why the toggles did nothing on an already-loaded world.
///
/// # Safety
/// `renderer` must be a live renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_material_debug(renderer: *mut WgrRenderer, view: u32, flags: u32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_material_debug(view, flags);
}

/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_cirrus_mode(renderer: *mut WgrRenderer, mode: u32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_cirrus_mode(mode);
}

/// Second cloud layer SHAPE: `puffiness` 0 = drawn-out fibrous veil .. 1 = lumpy cirrocumulus
/// (0.5 is the look the layer shipped with), `variation` = how far it is allowed to wander from
/// that on its own over the world's weather clock (0 = perfectly steady). Both clamp to 0..1.
///
/// Its own entry point rather than a WgrSkyLook lane, for the reason given on
/// `wgr_set_cirrus_mode`: that struct's size is part of the ABI handshake.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_cirrus_puffiness(
    renderer: *mut WgrRenderer,
    puffiness: f32,
    variation: f32,
) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_cirrus_puffiness(puffiness, variation);
}

/// Second cloud layer LOOK — the two controls that decide HOW MUCH of it there is and how much it
/// resembles the cumulus deck below. Both clamp to 0..1 and both are eased by the renderer, so a
/// slider drag cannot step the sky.
///
/// `amount`: 0 = a handful of separated wisps in an otherwise empty upper sky, 0.5 = exactly the
/// coverage the layer shipped with, 1 = a continuous cirrostratus veil with only thin breaks.
/// This is the quantity control that `wgr_set_cirrus_mode`'s on/off was not: coverage moves
/// furthest, optical depth follows slightly (a sky filling with cirrus does thicken as it fills).
///
/// `match_deck`: 0 = high ice cloud, which is what this layer has always been; 1 = as close to the
/// CUMULUS DECK as a shell march gets — altitude drops to just above the deck's top, feature size
/// follows the deck's own shape scale, the shear stretch and fibre striation relax to isotropic
/// cells, the shell deepens, optical depth rises and the ice phase function gives way to the
/// deck's. Every target is read from the deck's LIVE parameters, so "similar" tracks the weather
/// rather than a second set of constants. The layer can never sink into or below the deck.
///
/// Its own entry point rather than a WgrSkyLook lane, for the reason given on
/// `wgr_set_cirrus_mode`: that struct's size is part of the ABI handshake. Overridable at startup
/// with `WGR_CIRRUS_LOOK=<amount>[,<match>]`, which is how a `--no-dev` benchmark or screenshot
/// capture reaches controls the Sky tab would otherwise own exclusively.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_cirrus_look(
    renderer: *mut WgrRenderer,
    amount: f32,
    match_deck: f32,
) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_cirrus_look(amount, match_deck);
}

/// SECOND cloud layer EDGE SOFTNESS. 0 = the layer exactly as it rendered before this existed:
/// the sheet noise is remapped linearly about the coverage threshold into a hard clamp, so a cloud
/// reaches useful density within a short distance of its boundary and gets a definite silhouette.
/// 1 = the density is held down through a much wider band either side of that threshold, so the
/// transmittance falls off over a longer path and the cloud gains a deep, soft fringe.
///
/// Weighted by LOCAL COVERAGE, so it reaches the big merged banks and leaves isolated wisps
/// defined — "the bigger ones should be fuzzier", which is the request it was written for. The
/// layer's mean density is restored as the exponent rises, so this stays a shape control rather
/// than doubling as a brightness one. Clamps to 0..1 and is eased by the renderer.
///
/// Overridable at startup with `WGR_CIRRUS_SOFT=<0..1>` for `--no-dev` capture and benchmark runs.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_cirrus_softness(renderer: *mut WgrRenderer, softness: f32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_cirrus_softness(softness);
}

/// Fraction of the draw distance at which distance fog reaches FULL. 1.0 is the behaviour from
/// before this existed: fog saturates only exactly at the far plane, so the terrain grid's edge,
/// the object cull ring and the map boundary all sit right at the point where it finally closes,
/// and any of them showing through reads as the world ending. Lower moves full fog inward so
/// everything past it is uniformly the sky's own airlight.
///
/// This is NOT the same control as the falloff exponent (`WgrSkyLook`'s fog falloff): that shapes
/// how fast the ramp climbs, and lowering it to thicken the far field also thickens the near and
/// mid field. Clamped to [0.3, 1.0] here and again in the shader.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_fog_far_close(renderer: *mut WgrRenderer, close: f32) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_fog_far_close(close);
}

/// Road / decal per-pixel depth conform: flat lift (m), lift per metre of distance (before the
/// grazing division), and the ceiling as a fraction of distance. 0 in a field = shipped default.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_set_road_conform(
    renderer: *mut WgrRenderer,
    flat: f32,
    per_m: f32,
    max_frac: f32,
) {
    let Some(renderer) = (unsafe { renderer.as_mut() }) else {
        return;
    };
    renderer.set_road_conform(flat, per_m, max_frac);
}

/// Volumetric sun shafts (crepuscular rays / god rays), from the dev panel's Sky tab.
///
/// A world-space march, not a screen-space radial blur: the shafts are occluded by the CLD-020
/// cloud transmittance map (so a bank crossing the sun reshapes them), by the terrain sun-shadow
/// ceiling, and by the cascade shadow map. Nothing here references the sun's SCREEN position, so
/// there is no off-screen-sun halo to suppress, and the effect fades out on its own below the
/// horizon and behind the camera (elevation fade + a forward-peaked phase function).
///
/// `density` is the scattering coefficient in 1/m (the engine's clear-day Mie is 6e-6);
/// `distance` is the march cap in metres; `g` is the Henyey-Greenstein anisotropy;
/// `cloud_influence` scales how much the deck shapes the shafts (0 = geometry only, for A/B);
/// `steps` and `res_div` are the two cost knobs — `res_div` is the screen-resolution divisor for
/// the march target (2 = half in each axis). All values clamp.
///
/// Its own entry point rather than a WgrSkyLook lane, for the reason given on
/// `wgr_set_cirrus_mode`: that struct's size is part of the ABI handshake.
///
/// # Safety
/// `renderer` must be a live renderer from `wgr_create`.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn wgr_set_god_rays(
    renderer: *mut WgrRenderer,
    enabled: u32,
    intensity: f32,
    density: f32,
    distance: f32,
    g: f32,
    cloud_influence: f32,
    steps: u32,
    res_div: u32,
) {
    if renderer.is_null() {
        return;
    }
    let renderer = unsafe { &mut *renderer };
    renderer.set_god_rays(crate::godrays::GodRaySettings {
        enabled: enabled != 0,
        intensity,
        density,
        distance,
        g,
        cloud_influence,
        steps,
        res_div,
    });
}

/// Read one cascade layer of the shadow depth map back as row-major floats
/// (row 0 = top). Returns the map resolution (side length), or 0 when no map
/// exists / `layer` is out of range / `out_len` is too small.
///
/// # Safety
/// `renderer` must be live; `out` must point to at least `out_len` floats, or
/// be null (in which case 0 is returned).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_shadow_map_read(
    renderer: *mut WgrRenderer,
    layer: u32,
    out: *mut f32,
    out_len: u32,
) -> u32 {
    if renderer.is_null() || out.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let out = unsafe { std::slice::from_raw_parts_mut(out, out_len as usize) };
        renderer.shadow_map_read(layer, out)
    }))
    .unwrap_or(0)
}

/// Render a triangle soup through the shadow depth pipeline into a scratch
/// res*res map and read it back (row 0 = top). Returns 1 on success.
///
/// # Safety
/// `renderer` must be live; `light_vp16` must point to 16 floats, `tri_xyz` to
/// `3 * vert_count` floats, and `out` to `res * res` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_shadow_depth_probe(
    renderer: *mut WgrRenderer,
    light_vp16: *const f32,
    tri_xyz: *const f32,
    vert_count: u32,
    res: u32,
    out: *mut f32,
) -> i32 {
    if renderer.is_null() || light_vp16.is_null() || tri_xyz.is_null() || out.is_null() || res == 0
    {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let vp: &[f32; 16] = unsafe { &*(light_vp16 as *const [f32; 16]) };
        let verts = unsafe { std::slice::from_raw_parts(tri_xyz, vert_count as usize * 3) };
        let out = unsafe { std::slice::from_raw_parts_mut(out, (res * res) as usize) };
        renderer.shadow_depth_probe(vp, verts, res, out) as i32
    }))
    .unwrap_or(0)
}


/// Fixture-only copied renderer-owner facts. 0 disabled,1 collected,2 invalid.
/// Exact new structure size/version checked BEFORE output access; old layouts untouched.
/// Recorded commands do not prove survivors, pixels, submission completion or device freeing.
/// # Safety
/// Exclusive renderer-owner access; nonnull output must be writable for exactly `bytes`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_geometry_pass_facts(renderer:*mut WgrRenderer,out:*mut crate::gfx3d::geometry_pass_facts::WgrGeometryPassFacts,bytes:u32,version:u32)->u32 {
 if renderer.is_null() ||out.is_null() || !crate::gfx3d::geometry_pass_facts::valid_layout(bytes,version) {return 2;}
 catch_unwind(AssertUnwindSafe(|| {
  let Some(facts)=unsafe{&*renderer}.gfx3d.geometry_pass_facts() else{return 0;};
  unsafe{*out=facts;}1
 })).unwrap_or(2)
}

#[cfg(test)]
mod render_call_cpu_diagnostic_tests {
    use super::*;
    #[test]
    fn render_call_cpu_layout_and_invalid_ffi_inputs_are_transactional() {
        let old_pair = WgrAbiCheck { abi_version: 15, struct_size: std::mem::size_of::<WgrAbiCheck>() as u32, surface_desc_size: std::mem::size_of::<WgrSurfaceDesc>() as u32, log_callbacks_size: std::mem::size_of::<WgrLogCallbacks>() as u32, frame_size: std::mem::size_of::<WgrFrame>() as u32, required_features: WGR_ABI_FEATURE_BUILD_ID, layout_hash: wgr_abi_layout_hash() };
        assert_eq!(unsafe { wgr_abi_validate(&old_pair) }, 0);
        assert_eq!(std::mem::size_of::<WgrRenderCallCpuTimings>(), 72);
        assert_eq!(std::mem::offset_of!(WgrRenderCallCpuTimings, call_token), 16);
        assert_eq!(std::mem::offset_of!(WgrRenderCallCpuTimings, milliseconds), 24);
        let sentinel = 1usize as *mut WgrRenderer;
        let frame = 1usize as *const WgrFrame;
        let mut output = WgrRenderCallCpuTimings::new(99);
        let saved = output;
        unsafe {
            assert_eq!(wgr_render_frame_cpu_timings(sentinel, frame, 1, &mut output, 71, 1), -4);
            assert_eq!(wgr_render_frame_cpu_timings(sentinel, frame, 1, &mut output, 72, 0), -4);
            assert_eq!(wgr_render_frame_cpu_timings(sentinel, frame, 0, &mut output, 72, 1), -4);
            assert_eq!(wgr_render_frame_cpu_timings(sentinel, frame, 1, std::ptr::null_mut(), 72, 1), -4);
            assert_eq!(output, saved); // foreign layout/token never writes or dereferences handle.
            assert_eq!(wgr_render_frame_cpu_timings(std::ptr::null_mut(), std::ptr::null(), 7, &mut output, 72, 1), -1);
        }
        assert_eq!((output.state, output.count, output.call_token), (3, 0, 7));
        assert_eq!(output.milliseconds, [-1.0; 12]);
    }
}


/// Additive optional camera tuple v1. Existing ABI18 layouts/hash are unchanged.
/// 0 unavailable/disabled,1 copied,2 invalid arguments/panic. Output untouched on0/2.
/// # Safety
/// Exclusive renderer-owner access and192 writable output bytes. No polling/wait/renderer mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_main_camera_tuple(renderer:*mut WgrRenderer,out:*mut crate::main_camera_tuple::WgrMainCameraTuple,bytes:u32,version:u32)->u32 {
    if renderer.is_null()||out.is_null()||bytes!=crate::main_camera_tuple::BYTES||version!=crate::main_camera_tuple::VERSION{return 2;}
    catch_unwind(AssertUnwindSafe(||{
        let Some(tuple)=unsafe{&*renderer}.main_camera_tuple.as_ref().and_then(|t|t.snapshot()) else{return 0;};
        unsafe{*out=tuple;}1
    })).unwrap_or(2)
}

/// Additive default-off CPU demand-view tuples, not COUNT/cache/GPU release proof.
/// 0 disabled/no completed attempt,1 copied,2 invalid arguments/panic. Output
/// untouched on0/2. Preflight argument refusal is not a renderer-frame attempt.
/// # Safety
/// Exclusive owner access; output must be aligned writable8096B without aliasing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wgr_demand_view_snapshot(renderer:*mut WgrRenderer,
    out:*mut crate::demand_view_snapshot::WgrDemandViewSnapshot,bytes:u32,version:u32)->u32 {
    if renderer.is_null()||out.is_null()||!crate::demand_view_snapshot::valid_layout(bytes,version){return 2;}
    catch_unwind(AssertUnwindSafe(|| {
        let Some(snapshot)=unsafe{&*renderer}.demand_view_snapshot.as_ref().and_then(|t|t.snapshot()) else{return 0;};
        unsafe{*out=*snapshot;}1
    })).unwrap_or(2)
}
#[cfg(test)]
mod demand_view_snapshot_getter_tests {
    use super::*;
    #[test] fn demand_view_snapshot_layout_preflight_never_dereferences_invalid_renderer_or_writes_output() {
        let mut output=crate::demand_view_snapshot::WgrDemandViewSnapshot{generation:79,..Default::default()};
        let before=output;let sentinel=1usize as *mut WgrRenderer;
        assert_eq!(unsafe{wgr_demand_view_snapshot(sentinel,&mut output,8095,1)},2);
        assert_eq!(unsafe{wgr_demand_view_snapshot(sentinel,&mut output,8096,0)},2);
        assert_eq!(unsafe{wgr_demand_view_snapshot(std::ptr::null_mut(),&mut output,8096,1)},2);
        assert_eq!(unsafe{wgr_demand_view_snapshot(sentinel,std::ptr::null_mut(),8096,1)},2);
        assert_eq!(output,before);
        assert_eq!(std::mem::offset_of!(crate::demand_view_snapshot::WgrDemandViewSnapshot,rows),96);
    }
}

#[cfg(test)]
mod main_camera_tuple_getter_tests {
    use super::*;
    #[test] fn invalid_layout_and_null_leave_output_untouched() {
        let mut output=crate::main_camera_tuple::WgrMainCameraTuple{generation:77,..Default::default()};
        let before=output;let sentinel=1usize as *mut WgrRenderer;
        assert_eq!(unsafe{wgr_main_camera_tuple(sentinel,&mut output,191,1)},2);
        assert_eq!(unsafe{wgr_main_camera_tuple(sentinel,&mut output,192,0)},2);
        assert_eq!(unsafe{wgr_main_camera_tuple(std::ptr::null_mut(),&mut output,192,1)},2);
        assert_eq!(unsafe{wgr_main_camera_tuple(sentinel,std::ptr::null_mut(),192,1)},2);
        assert_eq!(output,before);
    }
}
