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
const WGR_ABI_SUPPORTED_FEATURES: u32 = WGR_ABI_FEATURE_BUILD_ID
    | WGR_ABI_FEATURE_SAFE_DIAGNOSTICS
    | WGR_ABI_FEATURE_RUNTIME_CAPABILITIES;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct WgrVertex2D {
    // pos.x/y = window pixels, pos.z = depth.
    pub pos: WgrVec3,
    pub rhw: f32,
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
    pub _pad: u32,
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
    // padding; the bit rides here because WgrModelMaterial is size-locked at 192 bytes.
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
pub const WGR_DRAW3D_NIGHT_EMITTER: u32 = 4;

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
    // Per layer, layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
    pub layer_uv: [[f32; 4]; 4],
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
    pub night_params: WgrVec4, // x = full-day sun_dir.y, y = full-night sun_dir.y, z = night intensity, w = pad
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
            night_params: [0.052, -0.139, 0.02, 0.0],
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
    pub moon_dir: WgrVec4,  // xyz = unit dir TO the moon; w = illuminated fraction (0 new .. 1 full)
    pub fog_color: WgrVec4, // xyz = scene fog colour; w = fog far-range (m)
    pub misc: WgrVec4,      // x = night factor (0..1), y = camera altitude ASL (m), z/w = pad
    // Cloud evolution offsets (m, CPU-wrapped like the wind offset): x = shape, y = detail,
    // z = weather/coverage drift, w = pad. Drifting the noise lookup makes clouds form and
    // dissolve in place rather than merely translating with the wind.
    pub cloud_evolve: WgrVec4,
    // x = moon angular radius (rad), y = illuminated fraction, z = disc radiance scale,
    // w = draw the disc (0 = skip).
    pub moon_params: WgrVec4,
    // xyz = unit dir TO the sun as seen from the moon; w = earthshine floor.
    pub moon_sun: WgrVec4,
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
//   foliagec = (gi_strength, tree_bend, tree_crown_y, _pad)
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
    pub _pad2: f32,
    // VEG-SWAY - geometric wind on vegetation MODELS. Two further vec4 lanes
    // (frame.foliaged / frame.foliagee). CameraGroup::new sizes the UBO from
    // size_of::<WgrFoliage>() and the upload writes the struct whole, so growing it here is
    // the only Rust-side plumbing change needed.
    pub sway_strength: f32, // metres of tip travel per metre above the pivot; 0 = OFF
    pub sway_speed: f32,    // rate multiplier on the trunk oscillation
    pub wind_dir_x: f32,    // unit vector the wind travels TOWARD
    pub wind_dir_z: f32,
    pub sway_time: f32,      // engine seconds (Glob.time) - a paused sim freezes the canopy
    pub wind_gust: f32,      // WindSample::gustFraction
    pub sway_leaf: f32,      // extra flutter on cutout (leaf) sections only
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
            _pad2: 0.0,
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
    pub cam_fwd: WgrVec4,     // xyz = camera forward (eye-depth cascade select)
    pub sun_dir: WgrVec4,     // xyz = sun travel direction (normal-offset bias)
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
    pub _pad: u32,
    pub light_vp: [WgrMat4; 4], // camera-relative light view-projections (0..1 NDC z)
    // Camera world position: casters are camera-relative, so the depth shader adds
    // this back to reconstruct absolute world xz for surface_y (terrain conform).
    pub cam_pos: WgrVec4,
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
    pub _pad0: f32,
    pub _pad1: f32,
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
    pub _pad1: u32,
    pub satellite_uv: WgrTerrainUv,
    pub mask_uv: WgrTerrainUv,
    pub surface_uvs: [WgrTerrainUv; WGR_TERRAIN_SURFACE_SLOTS],
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
    /// 0 = the primary clump, 1..8 = one local family. The direct way to find
    /// which source plate is responsible for a flicker.
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
    pub _pad_density: f32,
    /// Albedo tints, applied after the existing colour work and before lighting.
    /// `tint_procedural` covers procedural blades, ribbons and the far proxy;
    /// `tint_photo` covers the photographed cards. White = untinted.
    pub tint_procedural: [f32; 4],
    pub tint_photo: [f32; 4],
    /// Opaque bounding box per photo layer, (u0, u1, v0, v1) in texture space.
    /// Renderer-derived from the uploaded atlas; ignored on input.
    pub photo_layer_bounds: [f32; 36],
    /// Mean linear colour of each photo layer's covered texels, (r, g, b, 1).
    /// The pivot the card contrast turns about, so contrast cannot double as a
    /// brightness change. Renderer-derived; ignored on input.
    pub photo_layer_means: [f32; 36],
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
    // band). yzw reserved. (sizeof 256 -> 272, matching C++.)
    pub underwater_gate: WgrVec4,
}

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
    pub _shore_pad: f32,
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
const _: () = assert!(std::mem::size_of::<WgrDraw3D>() == 272);
const _: () = assert!(std::mem::size_of::<WgrLight>() == 64);
const _: () = assert!(std::mem::size_of::<WgrModelSection>() == 24);
const _: () = assert!(std::mem::size_of::<WgrModelMaterial>() == 192);
const _: () = assert!(std::mem::size_of::<WgrModelLod>() == 16);
const _: () = assert!(std::mem::size_of::<WgrInstance>() == 144);
const _: () = assert!(std::mem::size_of::<WgrTonemap>() == 48);
// WgrSky 16 vec4 -> 18 (was 256 bytes) and WgrSkyRuntime 5 vec4 -> 7 (was 80): the moon
// disc added moon_params + moon_sun to both. These two asserts, the matching
// static_asserts in wgpu_renderer.hpp, and sky/mod.rs's field-ORDER test are the set that
// has to move together — any one of them left behind is silent garbage in the shader.
const _: () = assert!(std::mem::size_of::<WgrSky>() == 288);
const _: () = assert!(std::mem::size_of::<WgrSkyLook>() == 192);
const _: () = assert!(std::mem::size_of::<WgrSkyRuntime>() == 112);
const _: () = assert!(std::mem::size_of::<WgrTerrainSunShadow>() == 16);
// 20 header words, then satellite/mask UVs and six per-slot UVs of two vec4
// each. The WGSL `TerrainMaterial` in terrain.wgsl mirrors this byte for byte.
const _: () = assert!(std::mem::size_of::<WgrTerrainUv>() == 32);
const _: () = assert!(std::mem::size_of::<WgrTerrainMaterial>() == 336);
const _: () = assert!(std::mem::size_of::<WgrSkyVisibility>() == 32);
const _: () = assert!(std::mem::size_of::<WgrFoliage>() == 80);
const _: () = assert!(std::mem::size_of::<WgrGtao>() == 52);
const _: () = assert!(std::mem::size_of::<WgrSkyVis>() == 44);
const _: () = assert!(std::mem::size_of::<WgrRenderParams>() == 496);
const _: () = assert!(std::mem::size_of::<WgrFrameParams>() == 16);
const _: () = assert!(std::mem::size_of::<WgrCameraShadow>() == 352);
const _: () = assert!(std::mem::size_of::<WgrCamera>() == 576);
const _: () = assert!(std::mem::size_of::<WgrShadowCaster>() == 136);
const _: () = assert!(std::mem::size_of::<WgrShadowPass>() == 288);
const _: () = assert!(std::mem::size_of::<WgrCmd>() == 8);
const _: () = assert!(std::mem::size_of::<WgrOverlayVertex>() == 20);
const _: () = assert!(std::mem::size_of::<WgrOverlayDraw>() == 40);
const _: () = assert!(std::mem::size_of::<WgrTerrainParams>() == 64);
const _: () = assert!(std::mem::size_of::<WgrTerrainNode>() == 24);
const _: () = assert!(std::mem::size_of::<WgrTerrainBatch>() == 16);
const _: () = assert!(std::mem::size_of::<WgrGrassBatch>() == 16);
const _: () = assert!(std::mem::size_of::<WgrGrassTrack>() == 16);
const _: () = assert!(std::mem::size_of::<WgrGrassDownwash>() == 16);
// 1840 bytes; the photo-card look block, the placement block, the layer-mix
// block and eight per-layer weights complete five further vec4s
// (GrassParams.photo / .place / .photo_mix / .layer_weights[2]).
const _: () = assert!(std::mem::size_of::<WgrGrassParams>() == 4736);
const _: () = assert!(std::mem::size_of::<WgrWaterParams>() == 272);
const _: () = assert!(std::mem::size_of::<WgrWaterNode>() == 40);
const _: () = assert!(std::mem::size_of::<WgrWaterBatch>() == 16);
const _: () = assert!(std::mem::size_of::<WgrWaterInteractionEvent>() == 64);
const _: () = assert!(std::mem::align_of::<WgrWaterInteractionEvent>() == 16);
const _: () = assert!(std::mem::size_of::<WgrWaterInteractionParams>() == 96);
const _: () = assert!(std::mem::align_of::<WgrWaterInteractionParams>() == 16);
const _: () = assert!(std::mem::size_of::<WgrSlice<WgrCamera>>() == 16);
const _: () = assert!(std::mem::size_of::<WgrFrame>() == 576);
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
    h = hash_size(h, sz::<WgrWaterNode>());
    h = hash_size(h, sz::<WgrWaterBatch>());
    h = hash_size(h, sz::<WgrWaterInteractionEvent>());
    h = hash_size(h, sz::<WgrWaterInteractionParams>());
    h = hash_size(h, sz::<WgrSurfaceDesc>());
    h = hash_size(h, sz::<WgrLogCallbacks>());
    h = hash_size(h, sz::<WgrFrame>());
    h = hash_size(h, sz::<WgrFarInstance>());
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
            .unwrap_or(6)
    } else {
        6
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
        assert_eq!(super::wgr_abi_version(), 6);
    }

    #[test]
    fn matching_abi_declaration_is_accepted() {
        assert_eq!(unsafe { super::wgr_abi_validate(&good_check()) }, 1);
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
    indices: WgrSlice<u16>,
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
) -> u32 {
    if renderer.is_null() {
        return WGR_INVALID_MODEL;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let lods = unsafe { lods.as_slice() };
        let sections = unsafe { sections.as_slice() };
        let materials = unsafe { materials.as_slice() };
        renderer.model_register(bounding_sphere, lods, sections, materials)
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
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
        let params = unsafe { *params };
        let count = params.hm_width as usize * params.hm_height as usize;
        let heights = unsafe { std::slice::from_raw_parts(heights, count) };
        renderer.terrain_set_heightmap(heights, params);
    }));
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
    if renderer.is_null() || frame.is_null() {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let renderer = unsafe { &mut *renderer };
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
        ) {
            Ok(()) => 0,
            Err(e) => {
                renderer
                    .log
                    .log(log_level::ERROR, &format!("render_frame: {e}"));
                -2
            }
        }
    }))
    .unwrap_or(-3)
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

    /// CPU-replayed object path (never reaches the cull shader).
    pub direct_calls: u32,
    pub direct_indirect_calls: u32,
    pub direct_instances: u32,
    pub direct_tris: u32,
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
        let (cull, direct, registered) = renderer.object_stats();
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
            direct_calls: direct.direct_calls,
            direct_indirect_calls: direct.indirect_calls,
            direct_instances: direct.instances,
            direct_tris: direct.tris,
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

/// Second cloud layer (high cirrus at ~7 km): 0 = off, 1 = flat sheet, 2 = thin-shell volumetric
/// march. Its own entry point for the same reason as `wgr_set_cloud_shadow_strength` above —
/// WgrSkyLook's size is part of the ABI handshake, and this is one enum. Values above 2 clamp.
///
/// # Safety
/// Material Debug state for the RETAINED path (dev panel, Materials tab).
/// `view`: 0 full material, 1 base colour only, 2 decoded normal.
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
