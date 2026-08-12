/*
 * wgpu_renderer.hpp — C++ interface to the WGPU graphics backend.
 *
 * While this file contains C++ features, the actual exported symbols are all C ABI (extern "C"),
 * which the Rust side can implement using #[no_mangle] and #[repr(C)].
 */
#ifndef WGPU_RENDERER_HPP
#define WGPU_RENDERER_HPP

#include <cstdint>

// Increment when an incompatible change is made to the public C ABI.  The
// engine checks this before creating a renderer so a matched build fails with a
// useful diagnostic instead of proceeding with incompatible assumptions.
#define WGR_ABI_VERSION 4u

// Required capabilities are negotiated as a bit set inside WgrAbiCheck. A
// newer engine can reject an older renderer DLL even if the shared layouts
// still happen to match.
#define WGR_ABI_FEATURE_BUILD_ID 0x00000001u
#define WGR_ABI_FEATURE_SAFE_DIAGNOSTICS 0x00000002u
#define WGR_ABI_FEATURE_RUNTIME_CAPABILITIES 0x00000004u

enum WgrRuntimeCapability : uint32_t
{
    WGR_RUNTIME_CAP_BC_TEXTURES = 1u << 0,
    WGR_RUNTIME_CAP_PARTIALLY_BOUND = 1u << 1,
    WGR_RUNTIME_CAP_INDIRECT_FIRST_INSTANCE = 1u << 2,
    WGR_RUNTIME_CAP_MULTI_DRAW_COUNT = 1u << 3,
    WGR_RUNTIME_CAP_GPU_TIMESTAMPS = 1u << 4,
    WGR_RUNTIME_CAP_TIMESTAMPS_IN_PASSES = 1u << 5,
    WGR_RUNTIME_CAP_HDR = 1u << 6,
    WGR_RUNTIME_CAP_MSAA = 1u << 7,
};

#if defined(_WIN32) && !defined(WGR_STATIC)
  #define WGR_API __declspec(dllimport)
#else
  #define WGR_API
#endif

struct WgrRenderer;

// --- Math + handle aliases ---------------------------------------------------

struct WgrVec2
{
    float x, y;
};
struct WgrVec3
{
    float x, y, z;
};
struct WgrVec4
{
    float x, y, z, w;
};
struct WgrMat4
{
    float m[16]; // column-major
};

using WgrRgba8 = uint32_t;   // packed 0xAARRGGBB (engine PackedColor order)
using WgrTexture = uint64_t; // handle from wgr_texture_create; 0 = built-in white fallback
using WgrMesh = uint64_t;    // handle from wgr_mesh_create

template <typename T>
concept ContiguousContainer = requires(const T& c) {
    c.data();
    c.size();
};

template <typename T>
struct WgrSlice
{
    const T* data = nullptr;
    uint32_t length = 0;

    WgrSlice() = default;
    WgrSlice(const T* ptr, uint32_t count) : data(ptr), length(count) {}

    template <typename Container>
        requires ContiguousContainer<Container>
    WgrSlice(const Container& c) : data(c.data()), length(static_cast<uint32_t>(c.size())) { }
};

// --- Enums -------------------------------------------------------------------

/* Selects how WgrSurfaceDesc.window / .display are interpreted. */
enum WgrPlatform : int32_t
{
    WGR_PLATFORM_WIN32 = 0,   // window = HWND,         display unused
    WGR_PLATFORM_XLIB = 1,    // window = Window (XID), display = Display*
    WGR_PLATFORM_WAYLAND = 2  // window = wl_surface*,  display = wl_display*
};

enum WgrLogLevel : int32_t
{
    WGR_LOG_TRACE = 0,
    WGR_LOG_DEBUG = 1,
    WGR_LOG_INFO = 2,
    WGR_LOG_WARN = 3,
    WGR_LOG_ERROR = 4
};

enum WgrTextureFormat : int32_t
{
    WGR_TEXTURE_RGBA8 = 0,
    WGR_TEXTURE_BC1 = 1, // DXT1
    WGR_TEXTURE_BC2 = 2, // DXT3
    WGR_TEXTURE_BC3 = 3  // DXT5
};

enum WgrBlend : uint32_t
{
    WGR_BLEND_OPAQUE = 0,
    WGR_BLEND_ALPHA = 1,
    WGR_BLEND_ADDITIVE = 2,
    WGR_BLEND_SHADOW = 3
};

/* Depth-buffer interaction for a 2D/screen batch. Plain 2D and depth-disabled
 * meshes (sky: NoZBuf) use NONE; transparent / NoZWrite meshes test but don't
 * write; opaque pre-projected meshes (the laptop) test and write. */
enum WgrDepthMode : uint32_t
{
    WGR_DEPTH_NONE = 0,      // no test, no write
    WGR_DEPTH_TEST = 1,      // test (LessEqual), no write
    WGR_DEPTH_TEST_WRITE = 2 // test (LessEqual) + write
};

/* Selects what a WgrCmd does when the frame's command stream is replayed. */
enum WgrCmdKind : uint32_t
{
    WGR_CMD_DRAW_2D = 0,       // arg = index into WgrFrame.batches (its WgrDepthMode picks depth state)
    WGR_CMD_DRAW_3D = 1,       // arg = index into WgrFrame.draws3d
    WGR_CMD_CLEAR_DEPTH = 2,   // arg unused; starts a new depth-cleared segment
    WGR_CMD_DRAW_TERRAIN = 3,  // arg = index into WgrFrame.terrain_batches
    WGR_CMD_RESOLVE = 4,       // arg unused; tonemap the HDR scene to the swapchain, then draw UI display-referred
    WGR_CMD_DRAW_WATER = 5,    // arg = index into WgrFrame.water_batches
    WGR_CMD_DRAW_GRASS = 6     // arg = index into WgrFrame.grass_batches
};

// --- Surface / logging -------------------------------------------------------

struct WgrSurfaceDesc
{
    WgrPlatform platform;
    void* window;
    void* display;
    uint32_t width;
    uint32_t height;
};

struct WgrLogCallbacks
{
    /* `log` may be NULL; `message` is only valid for the duration of the call. */
    void (*log)(int32_t level, const char* message, void* user);
    void* user;
};

// --- Vertices ----------------------------------------------------------------

/* One screen-space vertex. `pos.x`/`pos.y` are window pixels (origin top-left),
 * `pos.z` the depth, `rhw` the reciprocal clip-w (perspective-correct interp),
 * `fog` the fog blend factor (1 = keep colour, 0 = full fog). Plain 2D uses
 * pos.z=0, rhw=1, fog=1. `color` is packed 0xAARRGGBB. */
struct WgrVertex2D
{
    WgrVec3 pos;
    float rhw;
    float fog;
    WgrVec2 uv;
    WgrRgba8 color;
};

/* One object-space mesh vertex; matches the engine's SVertex (pos, normal, uv, conform,
 * optional authored tangent/binormal). */
struct WgrMeshVertex
{
    WgrVec3 pos;
    WgrVec3 normal;
    WgrVec2 uv;
    /* Per-vertex terrain-conform selector (0 = rigid, 1 = ClipLandKeep, 2 = ClipLandOn),
     * read by vs_main at @location(5). Meaningful only when the draw's conform mode
     * selects the per-vertex heightmap path (individual ClipLand vegetation). */
    uint32_t conform;
    WgrVec3 tangent;
    WgrVec3 binormal;
};

// --- Draw records ------------------------------------------------------------

/* A contiguous run of triangle-list vertices sharing one texture + blend + depth
 * mode. `texture_id` 0 selects the built-in 1x1 white texture. */
struct WgrDraw2DBatch
{
    WgrTexture texture_id;
    uint32_t first_vertex; // index into WgrFrame.verts
    uint32_t vertex_count; // multiple of 3
    WgrBlend blend;
    uint32_t sampler; // bits: point<<2 | clampV<<1 | clampU
    WgrDepthMode depth;
};

/* Sentinel for WgrDraw3D::palette_slot: this draw is not skinned. */
#define WGR_NO_PALETTE 0xFFFFFFFFu

/* Capacity of the frame-global light store (WgrFrame::lights). Must match
 * MAX_LIGHTS in rust/src/gfx3d/mod.rs. The renderer clamps to this. */
#define WGR_MAX_LIGHTS 256

/* Bits for WgrDraw3D::flags. */
enum WgrDraw3DFlags : uint32_t
{
    /* Road / decal / footprint overlay: pull the draw toward the camera with a
     * polygon-offset (mirrors GL33's SetPolygonOffsetForDecals on OnSurface
     * routing) so it wins the depth test against the coplanar terrain. */
    WGR_DRAW3D_ON_SURFACE = 1,

    /* ZBias overlay level (1..3) in bits 8-9, for non-OnSurface geometry that the
     * engine biased via SetBias(level*5) (e.g. traffic-sign overlay faces). Gets a
     * stronger, level-scaled polygon-offset than a plain surface decal. */
    WGR_DRAW3D_ZBIAS_SHIFT = 8,
    WGR_DRAW3D_ZBIAS_MASK = 0x300
};

/* Matrices per palette block (the engine's own bone-palette cap). Each skinned
 * draw's palette occupies this many matrices in WgrFrame.palette. */
#define WGR_PALETTE_SIZE 128

/* A section [index_begin, index_begin+index_count) of `mesh`, textured with
 * `texture_id` (0 = built-in white), transformed by the camera-relative `world`
 * matrix. `camera` indexes WgrFrame.cameras. For skinned draws, `palette_slot`
 * indexes a 128-matrix block in WgrFrame.palette (world pre-multiplied in) and `world`
 * is ignored; WGR_NO_PALETTE = not skinned (use `world`). */
struct WgrDraw3D
{
    WgrMesh mesh;
    uint32_t index_begin;
    uint32_t index_count;
    WgrTexture texture_id;
    WgrTexture normal_texture_id; /* RVMAT Stage1, 0 when unavailable or overridden */
    WgrMat4 world;
    WgrBlend blend;
    uint32_t sampler;
    uint32_t camera;
    uint32_t palette_slot;
    WgrDepthMode depth;
    /* Alpha-test cutout threshold in [0,1]: a fragment is discarded when its
     * sampled alpha is below this. 0 disables the test (nothing discarded).
     * Mirrors GL33's per-draw alphaRef (IsAlpha ~1/255, IsTransparent 0xC0). */
    float alpha_ref;
    uint32_t flags; // WgrDraw3DFlags
    uint32_t _pad;
    /* Per-draw material lighting, folded exactly like GL33's
     * UploadVSMaterialConstants: raw MainLight diffuse/ambient x material, with
     * the sun-enable already multiplied into the sun terms (emissive shows
     * regardless). The lit shader computes emissive + sun_ambient +
     * sun_diffuse * N.L, clamps to [0,1], then multiplies the texture. Only rgb
     * is read; the w lanes ride along for 16-byte std140 alignment. */
    WgrVec4 mat_emissive;
    WgrVec4 mat_sun_ambient;
    WgrVec4 mat_sun_diffuse;
    /* Material modulation for the frame-global point/spot lights (GL33's matDif /
     * matAmb before the per-light colour): raw material diffuse/ambient (eye
     * accommodation already in, night NOT — that rides the light colour). rgb. */
    WgrVec4 mat_light_diffuse;
    WgrVec4 mat_light_ambient;
    /* Sun-only Blinn-Phong specular highlight, folded like GL33's c18: rgb = raw
     * sun diffuse x material specular (sun-enable folded in, so 0 when the sun is
     * off), w = specular power. The lit shader adds rgb * pow(N.H, max(w,1))
     * per-fragment when w > 0; w <= 0 means the material has no highlight. */
    WgrVec4 mat_specular;
    /* Terrain-conform plane for GPU vegetation (ForestPlain).
     * When conform2.z (mode) > 0 the vertex shader displaces this draw's vertices onto
     * the ground exactly like ForestPlain::Animate's two-triangle bilinear fit, so the
     * shared forest mesh is uploaded once undeformed instead of rewritten per instance.
     * All-zero (mode 0) for every non-conformed draw. */
    WgrVec4 conform0; /* inv_land_grid, -xf, -zf, bias(=BoundingCenter().y) */
    WgrVec4 conform1; /* y00, y10, d1000, d0100 */
    WgrVec4 conform2; /* d1011, d0111, mode(0=none,1=forest), _pad */
};

/* One frame-global point or spot light, shared by every 3D draw + terrain (bound
 * as a group-0 storage buffer). Position is ABSOLUTE world space (not
 * camera-relative like the geometry) so one upload serves every camera; the
 * shader reconstructs the camera-relative offset via the frame's cam_pos.
 * Colours are pre-scaled by the sun's NightEffect on the CPU (fade out by day,
 * matching GL33's night-only local lights). Mirrors GL33's per-draw VS lights. */
struct WgrLight
{
    WgrVec4 pos;     /* xyz = world-absolute position, w = start-attenuation distance */
    WgrVec4 diffuse; /* rgb = diffuse * nightEffect */
    WgrVec4 ambient; /* rgb = ambient * nightEffect */
    WgrVec4 dir;     /* xyz = beam direction (spot), w = isSpot (1) else 0 */
};

/* --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) ---
 *
 * C++ registers each opaque-rigid LODShapeWithShadow once (its LODs + per-section
 * geometry and material) via wgr_model_register, then streams instances: static
 * clutter as add/update/remove slots, dynamics re-copied each frame via
 * wgr_set_dynamic. The GPU cull compute walks the retained instances each frame and
 * emits indirect draws, so the CPU stops walking these objects per frame. Layouts
 * mirror the Rust #[repr(C)] structs in rust/src/ffi.rs (size-asserted both sides). */

/* One drawable section of a model LOD. `mesh` + the index range address the shared
 * geometry pool (resolved to base_vertex/first_index at registration); `variant`
 * selects the pipeline-variant partition (0 = solid, 1 = alpha-cutout). */
struct WgrModelSection
{
    WgrMesh mesh;
    uint32_t index_begin;
    uint32_t index_count;
    uint32_t variant;
    uint32_t _pad;
};

/* Per-section shading, parallel to a model's sections (one per section). The RAW
 * material is folded with the frame sun in the GPU-driven fragment shader (matching
 * the per-draw path); `texture_id` is a wgr_texture_create handle, resolved to a
 * bindless slot at registration. */
struct WgrModelMaterial
{
    WgrVec4 emissive;
    WgrVec4 ambient;
    WgrVec4 diffuse;
    WgrVec4 specular; /* w = specular power */
    WgrTexture texture_id;
    /* The section's per-texel specular map (RVMAT SpecularDetail: SMDI on Super/Skin, the
     * equivalents on the NormalMap* families), 0 when the material names none. Its G channel
     * scales `specular`, which is the constant that map was authored to modulate. */
    WgrTexture specular_texture_id;
    /* The section's tangent-space normal map (RVMAT Stage1), 0 when the material names none.
     * Decoded the same way the per-draw path decodes it: X from A, Y from G, Z reconstructed. */
    WgrTexture normal_texture_id;
    uint32_t sampler;
    float alpha_ref;
    /* The Multi family is a four-layer masked blend: `mask_texture_id` selects between
     * layer 0 (texture_id / normal_texture_id above) and these three, each of which is
     * authored at its own UV scale -- on Takistan's brick houses the brick layer tiles
     * far more densely than the rock it sits on, and that difference is the detail the
     * surface reads as. 0 in the mask means "not layered", and the shader then takes
     * layer 0 alone, exactly as before. */
    WgrTexture mask_texture_id;
    WgrTexture layer_texture_id[3];
    /* Per layer, including layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
     * RVMAT authors these transforms diagonally, so a full 3x3 is not carried. */
    float layer_uv[4][4];
};

/* One drawable LOD level: its FindSqrtLevel resolution threshold (_resolutions[i]) +
 * the range of sections it draws (`section_base` is RELATIVE to this model's
 * sections). */
struct WgrModelLod
{
    float resolution;
    uint32_t section_base;
    uint32_t section_count;
    uint32_t is_decal;
};

/* One retained instance. Layout matches the GPU-side InstanceGpu exactly: `world` is
 * the ABSOLUTE model->world transform (the GPU-driven VS subtracts cam_pos),
 * `center.xyz` the world bounding-sphere center + `center.w` the uniform scale (both
 * read by the cull compute), `model` the wgr_model_register id. */
/* Bits for WgrInstance::flags (mirror INST_CANOPY_* in gpu_driven.wgsl). vs_gpu bends this
 * instance's cutout (leaf) normals toward a radial crown normal for rounded low-poly shading
 * (docs/foliage-translucency-plan.md Stage 3); bush vs tree only differ in the bend + crown-Y
 * knobs they select. FOREST (§9 Approach A) is a merged multi-tree mesh: its single instance
 * centre is meaningless per-tree, so instead of inst.center each vertex carries a per-tree crown
 * centre index (baked into the vertex `conform` word, indexing the wgr_register_crown_centres
 * table). It shares the TREE bend/crown-Y knobs. */
enum WgrInstanceFlags : uint32_t
{
    WGR_INSTANCE_CANOPY_BUSH = 1,
    WGR_INSTANCE_CANOPY_TREE = 2,
    WGR_INSTANCE_CANOPY_FOREST = 4
};

struct WgrInstance
{
    WgrMat4 world;
    WgrVec4 center;
    uint32_t model;
    uint32_t flags;
    /* Inflated frustum-cull radius (float bits) for terrain-conform instances, whose displaced
     * geometry escapes the flat model sphere; 0 = rigid (cull uses model bounding sphere). */
    uint32_t cull_radius;
    uint32_t _pad;
    /* Terrain-conform plane (mirrors WgrDraw3D::conform*). conform2.z = mode: 0 rigid,
     * 1 = ForestPlain bilinear plane, 2 = per-vertex ClipLand SurfaceY (conform0.x = bcSurfaceY). */
    WgrVec4 conform0;
    WgrVec4 conform1;
    WgrVec4 conform2;
};

static_assert(sizeof(WgrModelSection) == 24, "WgrModelSection must match Rust");
static_assert(sizeof(WgrModelMaterial) == 192, "WgrModelMaterial must match Rust");
static_assert(sizeof(WgrModelLod) == 16, "WgrModelLod must match Rust");
static_assert(sizeof(WgrInstance) == 144, "WgrInstance must match Rust");

/* Live tonemap/look parameters (from the ImGui Tonemap tab; pushed inside WgrRenderParams
 * via wgr_set_render_params). The Hable curve is fixed in the shader; these are exposure +
 * the colour-grade block. Layout matches the Rust WgrTonemap #[repr(C)] and tonemap.wgsl. */
struct WgrTonemap
{
    float exposure;    /* linear pre-curve multiplier */
    float mode;        /* 0 = passthrough (clamp), 1 = Hable */
    float encode;      /* 0 = write as-is, 1 = linear->sRGB encode */
    float temperature; /* white balance warm(+)/cool(-) */
    float tint;        /* white balance magenta(+)/green(-) */
    float contrast;    /* post-curve contrast (1 = neutral) */
    float saturation;  /* post-curve saturation (1 = neutral) */
    float lift;        /* shadow lift (0 = neutral) */
    float gain;        /* post-curve overall multiply (1 = neutral) */
    float bloom_intensity; /* linear weight of the bloom added to the scene (0 = off) */
    float bloom_threshold; /* bloom soft-knee centre (scene-referred luminance) */
    float bloom_knee;      /* bloom soft-knee half-width */
};

/* Eye-adaptation / auto-exposure parameters (pushed inside WgrRenderParams via
 * wgr_set_render_params). Layout matches the Rust WgrExposure #[repr(C)] and exposure.wgsl's
 * ExpParams (8 f32). */
struct WgrExposure
{
    float enabled;   /* 0 = off (scale eases to 1.0), 1 = auto-exposure on */
    float key;       /* target middle-grey luminance (higher = brighter) */
    float min_scale; /* clamp on the exposure multiplier */
    float max_scale;
    float rate;       /* per-frame ease toward the target (0..1) */
    float sky_weight; /* metering weight of the top of frame (sky) vs bottom (ground) */
    float _pad1;
    float _pad2;
};

/* Procedural sky UBO (renderer-internal assembly target). Its authored look half is written
 * from WgrSkyLook (wgr_set_render_params) and its per-frame celestial/camera half from
 * WgrSkyRuntime (wgr_set_sky_runtime); C++ no longer builds this struct directly. Layout
 * matches the Rust WgrSky #[repr(C)] (7 vec4). See docs/procedural-sky-plan.md and
 * docs/render-params-consolidation-plan.md. */
struct WgrSky
{
    WgrVec4 sun_dir;       /* xyz = unit dir TO the sun (up by day); w = sun radiance scale */
    WgrVec4 moon_dir;      /* xyz = unit dir TO the moon; w = moon phase (0.5 = full) */
    WgrVec4 rayleigh;      /* xyz = Rayleigh scattering coeff (1/m); w = Rayleigh scale height (m) */
    WgrVec4 mie;           /* x = Mie scattering coeff, y = Mie g, z = Mie scale height (m), w = turbidity */
    WgrVec4 ground_albedo; /* xyz = ground albedo; w = night factor (0 day .. 1 night) */
    WgrVec4 params;        /* x = sun angular radius (rad), y = exposure, z = planet radius (m), w = atmosphere (m) */
    WgrVec4 control;       /* x = enabled, y = view samples, z = light samples, w = pad */
    WgrVec4 fog_color;     /* xyz = scene fog colour; w = horizon-haze strength (0 = off) */
    /* Authored night-sky floor (plan Stage 6): a deep-blue radiance blended in by sun
     * altitude so twilight/night settle into blue instead of near-black. */
    WgrVec4 night_zenith;  /* xyz = night radiance at the zenith, w = camera altitude ASL (m; aerial/sky raymarch origin) */
    WgrVec4 night_horizon; /* xyz = night radiance at the horizon */
    WgrVec4 night_params;  /* x = full-day sun_dir.y, y = full-night sun_dir.y, z = intensity, w = far-fade range (m; aerial dissolves the terrain edge into sky by this dist, 0 = off) */
    /* Volumetric clouds (plan Stage 5): a raymarched cloud shell composited inside sky_radiance. */
    WgrVec4 cloud0; /* x = coverage [0,1], y = extinction (1/m), z = bottom (m ASL), w = top (m ASL) */
    WgrVec4 cloud1; /* x/y = wind world offset (m, RUNTIME, CPU-wrapped), z = shape scale (1/m), w = detail scale (1/m) */
    WgrVec4 cloud2; /* x = HG forward g, y = powder strength, z = ambient scale, w = max march distance (m) */
    WgrVec4 cloud3; /* x = weather scale (1/m), y = weather amount, z = warp scale (1/m), w = warp amount (m) */
    /* Cloud EVOLUTION offsets (m, RUNTIME, CPU-wrapped): x = shape, y = detail, z = weather drift.
     * Applied on the noise volume's third axis, so the field morphs in place — clouds form and
     * dissolve — instead of only translating with the wind. w = pad. */
    WgrVec4 cloud4;
};

/* --- Consolidated imgui-tweakable render params (docs/render-params-consolidation-plan.md) ---
 * Every ImGui-tweakable render parameter that crosses the FFI as a setter is pushed as one
 * WgrRenderParams block via wgr_set_render_params. Per-frame runtime the engine recomputes
 * (sun/moon dir, night factor, fog colour, camera altitude, fog range) rides WgrSkyRuntime,
 * pushed each frame via wgr_set_sky_runtime. The two write disjoint halves of the internal
 * WgrSky UBO (layout + sky shader unchanged). Layouts match the Rust #[repr(C)] structs. */

/* Authored procedural-sky look (the ImGui Sky tab). No celestial/runtime fields. */
struct WgrSkyLook
{
    WgrVec4 rayleigh;      /* xyz = scattering coeff (1/m); w = scale height (m) */
    WgrVec4 mie;           /* x = coeff, y = g, z = scale height (m), w = turbidity */
    WgrVec4 ground_sun;    /* xyz = ground albedo; w = sun radiance scale (sunIntensity) */
    WgrVec4 params;        /* x = sun angular radius (rad), y = exposure, z = planet radius (m), w = atmosphere (m) */
    WgrVec4 control;       /* x = enabled, y = view samples, z = light samples, w = ozone */
    WgrVec4 night_zenith;  /* xyz = night radiance at the zenith; w = horizon-haze strength */
    WgrVec4 night_horizon; /* xyz = night radiance at the horizon; w = aerial-shadow strength */
    WgrVec4 night_params;  /* x = full-day sun_dir.y, y = full-night sun_dir.y, z = night intensity, w = pad */
    /* Cloud look (mirrors WgrSky::cloud0/1/2/3; cloud1.xy = wind offset is runtime, unused here). */
    WgrVec4 cloud0; /* x = coverage, y = extinction (1/m), z = bottom (m), w = top (m) */
    WgrVec4 cloud1; /* x/y unused (runtime wind offset), z = shape scale (1/m), w = detail scale (1/m) */
    WgrVec4 cloud2; /* x = HG forward g, y = powder, z = ambient scale, w = max distance (m) */
    WgrVec4 cloud3; /* x = weather scale (1/m), y = weather amount, z = warp scale (1/m), w = warp amount (m) */
};

/* Per-frame celestial + camera runtime (from LightSun / the camera). NOT an ImGui knob. */
struct WgrSkyRuntime
{
    WgrVec4 sun_dir;   /* xyz = unit dir TO the sun; w = pad */
    WgrVec4 moon_dir;  /* xyz = unit dir TO the moon; w = moon phase */
    WgrVec4 fog_color; /* xyz = scene fog colour; w = fog far-range (m) */
    WgrVec4 misc;      /* x = night factor (0..1), y = camera altitude ASL (m), z/w = wind offset */
    WgrVec4 cloud_evolve; /* x = shape, y = detail, z = weather drift (m, CPU-wrapped); w = pad */
};

/* Long-distance terrain sun-shadow sweep (was wgr_terrain_set_sun_shadow's args). */
struct WgrTerrainSunShadow
{
    float    strength;     /* 0 = disabled */
    uint32_t scale;        /* mask supersample factor — CHANGING THIS reallocates the mask */
    uint32_t max_steps;    /* march cap (steps * terrain_grid) */
    float    penumbra_deg; /* soft-edge half-width */
};

/* Terrain sky-visibility (sky-view factor) AO (was wgr_terrain_set_sky_visibility's args). */
struct WgrSkyVisibility
{
    float    strength;   /* 0 = disabled */
    float    contrast;   /* deepens the near-1 factor */
    float    floor;      /* minimum ambient in fully-occluded columns */
    float    radius_m;   /* horizon-scan reach (m) — CHANGING re-runs the CPU scan */
    uint32_t k_azimuths; /* scan direction count — CHANGING re-runs the scan */
    uint32_t downsample; /* scan coarseness — CHANGING re-runs the scan */
    uint32_t debug;      /* 1 = terrain outputs the factor as greyscale */
    uint32_t _pad;
};

/* Foliage lighting — emulated subsurface scattering + canopy normals for alpha-tested
 * vegetation (docs/foliage-translucency-plan.md). The scalars ride into the per-camera Frame
 * UBO and are read by the object shader's shade() for cutout/vegetation draws. */
struct WgrFoliage
{
    float trans_scale;    /* DICE transmission strength (dark-side / backlit lift) */
    float distortion;     /* transmission light-dir bend toward the normal (0..1) */
    float trans_power;    /* transmission lobe tightness (>= 1) */
    float wrap;           /* front terminator-wrap fill (0 = hard Lambert) */
    float ambient_boost;  /* SH ambient multiplier for foliage (1 = off), distance-faded */
    float normal_bend;    /* BUSH spherical-normal blend (0 = geometric, 1 = full radial) */
    float crown_y_offset; /* BUSH crown-centre Y lift for the spherical normal */
    float fill_fade_end;  /* camera distance (m) by which the SSS fill + ambient boost fade (0 = off) */
    float gi_strength;    /* cheap GI: scale ambient by terrain light level, 0 = off */
    float tree_bend;      /* TREE spherical-normal blend (leaf sections only; trunk unaffected) */
    float tree_crown_y;   /* TREE crown-centre Y lift (larger than bush — centre sits mid-trunk) */
    float _pad2;
};

/* Screen-space ambient occlusion (GTAO) — see docs/screen-space-ao-plan.md. Computed from the
 * depth+normal prepass into an R8 buffer, bilateral-denoised, and multiplied into the AMBIENT
 * term of terrain + objects (never the direct sun). Water is untouched. Default OFF. */
struct WgrGtao
{
    uint32_t enabled;           /* 0 = pass skipped entirely; consumers read AO = 1 */
    uint32_t debug;             /* raw view: 0 = off, 1 = AO greyscale, 2 = bent normal RGB */
    float    radius_m;          /* occlusion reach in WORLD metres (projected per pixel) */
    float    strength;          /* exponent on visibility; 1 = physical, >1 deepens */
    uint32_t slices;            /* azimuthal directions per pixel */
    uint32_t steps;             /* horizon-march steps per slice, per side */
    float    max_radius_px;     /* screen-radius clamp (cost bound for near geometry) */
    float    thickness;         /* falloff past the radius; rejects thin foreground occluders */
    float    blur_radius;       /* bilateral denoise half-width in taps */
    float    blur_depth_scale;  /* depth-difference rejection strength */
    float    blur_normal_power; /* normal-difference rejection exponent */
    uint32_t bent_normal;       /* 1 = steer sky irradiance by the bent normal (Stage 2) */
    uint32_t max_mip;           /* highest mip the horizon march may use; 0 = full res only */
};

/* Interior sky visibility (LIT-020) — see docs/interior-sky-visibility-plan.md. A top-down
 * orthographic depth map of the retained OBJECT set: a fragment with geometry above it loses sky
 * AMBIENT toward `floor`. Distinct from WgrSkyVisibility, which is the terrain heightfield's baked
 * sky-view factor and knows nothing about buildings. Direct sun and local lights are never
 * touched. Default OFF. */
struct WgrSkyVis
{
    uint32_t enabled;    /* 0 = no map, no cull view; consumers read reach = 1 */
    uint32_t debug;      /* 1 = draw the reach factor as greyscale instead of lighting with it */
    uint32_t resolution; /* depth-map edge in texels */
    float extent;        /* HALF the world box, metres (2048 tex / 64 m half = 6 cm/texel) */
    float height;        /* box half-height above/below the camera, metres */
    float strength;      /* 0 = inert, 1 = full attenuation */
    float floor;         /* minimum ambient multiplier in a sealed volume */
    float kernel;        /* softening kernel radius, metres */
    float bias;          /* depth bias, metres (stops open ground occluding itself) */
    float directional;   /* 0 = uniform dimming, 1 = ambient arrives from the open direction */
    uint32_t baked;      /* 1 = apply the per-model BAKED volumes (Stage 2) instead of the maps */
};

/* Every imgui-tweakable render parameter that crosses the FFI as a setter, in one block.
 * Append future look knobs here; do not add new FFI setters. */
struct WgrRenderParams
{
    WgrTonemap          tonemap;
    WgrExposure         exposure;
    WgrSkyLook          sky;
    WgrTerrainSunShadow terrain_sun_shadow;
    WgrSkyVisibility    sky_visibility;
    WgrFoliage          foliage;
    WgrGtao             gtao;
    WgrSkyVis           interior_sky;
};

/* Frame-global scalars carried in the camera UBO so the 3D shader can read them
 * without a 5th bind group (wgpu's default maxBindGroups is 4). Distinct concerns
 * (distance fog, shadow darkening) that happen to share this ride for that reason.
 * shadow_strength = GetShadowFactor()/256, read by WGR_BLEND_SHADOW draws. */
struct WgrFrameParams
{
    float fog_start;
    float fog_inv_range;
    float fog_enabled; // 0 = off, 1 = on
    float shadow_strength;
};

/* Per-camera cascaded-shadow sampling block, read by the lit 3D shaders. All
 * zeros (ctl.x = cascade count = 0 -> disabled) when shadow maps are off or for
 * UI/screen cameras; the depth pass itself is driven by WgrShadowPass, not this. */
struct WgrCameraShadow
{
    WgrMat4 cascade_vp[4]; // camera-relative light view-projections (0..1 NDC z)
    WgrVec4 splits;        // frustum tiers: far eye-depth per tier
    WgrVec4 omni_radius;   // omni tiers: camera-distance radius (0 = frustum tier)
    WgrVec4 ctl;           // {count, omni_count, fade_range, bias_const}
    WgrVec4 ctlb;          // {texel_size (1/res), darkness, normal_offset_scale, pcf}
    WgrVec4 cam_fwd;       // xyz = camera forward (eye-depth cascade select)
    WgrVec4 sun_dir;       // xyz = sun travel direction (normal-offset bias)
};

/* A view + projection pair, plus the frame-global params (see WgrFrameParams).
 * fog_color = rgb (+pad). */
struct WgrCamera
{
    WgrMat4 proj;
    WgrMat4 view;
    WgrVec4 fog_color;
    WgrFrameParams params;
    WgrCameraShadow shadow;
    /* World-space camera position. `view` has no translation (geometry is
     * camera-relative); terrain uses this to sample the world-space heightmap. */
    WgrVec4 cam_pos;
    /* Sun light for GPU-lit paths (terrain): rgb, pre-multiplied by the eye
     * accommodation — DoLightingColorized's DiffusePrecalc/AmbientPrecalc for a
     * white material. */
    WgrVec4 sun_diffuse;
    WgrVec4 sun_ambient;
    /* xyz = normalized MainLight()->Direction(): the sun's light TRAVEL
     * direction (downward by day, upward while the sun is below the horizon —
     * which is what keeps night terrain ambient-only). Same convention as
     * GL33's sunDir constant; shaders dot the normal with its negation. Valid
     * every frame, unlike the shadow block's sun_dir. */
    WgrVec4 sun_dir_world;
};

/* One shadow caster for the cascade depth passes: a section run
 * [index_begin, index_begin+index_count) of `mesh`, transformed by the
 * camera-relative `world` (or, when `palette_slot` is valid, GPU-skinned by that
 * palette block exactly like a WgrDraw3D). `alpha_ref` > 0 alpha-tests the
 * caster texture so cutout foliage casts a leaf silhouette instead of a blob. */
struct WgrShadowCaster
{
    WgrMesh mesh;
    uint32_t index_begin;
    uint32_t index_count;
    WgrMat4 world;
    WgrTexture texture_id; // sampled only when alpha_ref > 0; 0 = built-in white
    uint32_t palette_slot; // WGR_NO_PALETTE = rigid
    float alpha_ref;       // 0 = solid caster; > 0 = discard below (cutout)
    uint32_t sampler;
    uint32_t cascade_mask; // bit c set = render into cascade c
    // Terrain-conform plane for this caster (mirrors WgrDraw3D::conform*). Mode 2
    // (conform2.z) conforms ClipLand vegetation to SurfaceY per vertex in the depth
    // shader, so the shared shadow mesh is uploaded ONCE undeformed. 0 = rigid.
    WgrVec4 conform0; // x = bcSurfaceY
    WgrVec4 conform2; // z = mode
};

/* Cascade depth-pass parameters for one frame. The renderer draws
 * WgrFrame.shadow_casters into a `count`-layer depth array from these
 * camera-relative light view-projections before replaying the frame's command
 * stream. count = 0 disables the pass (and keeps last frame's map unused). */
struct WgrShadowPass
{
    uint32_t count; // cascade count (1..4); 0 = no shadow pass this frame
    uint32_t omni_count;
    uint32_t resolution; // depth-map side length per cascade
    uint32_t _pad;
    WgrMat4 light_vp[4]; // camera-relative light view-projections (0..1 NDC z)
    // Camera world position: casters are camera-relative, so the depth shader adds
    // this back to reconstruct absolute world xz for surface_y (terrain conform).
    WgrVec4 cam_pos;
};

/* One entry in the frame's submission-ordered command stream. */
struct WgrCmd
{
    WgrCmdKind kind;
    uint32_t arg;
};

// --- Terrain (GPU heightmap) -------------------------------------------------

/* Per-map terrain parameters, uploaded once with the heightmap. The heightmap is
 * an hm_width x hm_height R32Float texture of world heights sampled in the vertex
 * shader; `terrain_grid` is the world spacing between adjacent heightmap texels,
 * `land_grid` the coarser texture-cell spacing, `world_origin` the world-space xz
 * of texel (0,0). `data_scale` is currently unused (heights arrive in metres). */
struct WgrTerrainParams
{
    WgrVec2 world_origin;
    float land_grid;
    float terrain_grid;
    uint32_t hm_width;
    uint32_t hm_height;
    uint32_t land_range; // land-cell count per axis
    float data_scale;
    /* Coast wet band (Stage 2c), pushed per frame via wgr_terrain_set_params. sea_level + time
     * (+ swash) move the damp intertidal line in lockstep with the water's edge; wet_height =
     * metres above the (swash-moved) sea level the band reaches, wet_darken = albedo multiplier
     * in the band (1 = off). Slope-gated in the shader; uses the SAME swash formula/params as
     * the water shader so the two register. */
    float sea_level;
    float time;
    float swash_speed;
    float swash_amp;
    float wet_height;
    float wet_darken;
    float _pad0;
    float _pad1;
};

/* One terrain node instance: the shared grid mesh placed at world-xz `origin`,
 * covering `size` x `size` world units, at level `lod`. `morph_start`/`morph_end`
 * are the camera-distance band over which the grid morphs toward its coarser parent. */
struct WgrTerrainNode
{
    WgrVec2 origin;
    float size;
    uint32_t lod;
    float morph_start;
    float morph_end;
};

/* A run [first_node, first_node+node_count) of WgrFrame.terrain_nodes drawn with
 * the shared grid mesh, transformed by camera `camera` (indexes WgrFrame.cameras). */
struct WgrTerrainBatch
{
    uint32_t first_node;
    uint32_t node_count;
    uint32_t camera;
    uint32_t _pad;
};

/* One procedural grass draw for `camera`. Grass placement and instances are owned
 * entirely by the Rust renderer; this batch only preserves the engine command
 * stream and chooses the scene camera. */
struct WgrGrassBatch
{
    uint32_t camera;
    uint32_t flags;
    uint32_t _pad0;
    uint32_t _pad1;
};

/* Walked-trail history. Each slot is one stamped footprint; the ring is consumed
 * by DISTANCE walked, not time, so this is the length of trail that survives
 * rather than a number of seconds. */
enum { WGR_GRASS_TRACK_COUNT = 256 };
enum { WGR_GRASS_DOWNWASH_COUNT = 4 };

/* One persistent player/vehicle impression. Age is measured in seconds and
 * faded on the GPU, so a trail remains after its source has moved away. */
struct WgrGrassTrack
{
    float x;
    float z;
    float radius;
    float age;
};

// Versioned size handshake performed before renderer construction. C++ fills
// every field; Rust rejects a missing or stale structure instead of accepting
// a binary pair that merely happens to link.
struct WgrAbiCheck
{
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t surface_desc_size;
    uint32_t log_callbacks_size;
    uint32_t frame_size;
    uint32_t required_features;
};

/* A transient rotor-wash field. Unlike tracks this is rebuilt from the live
 * helicopter list each frame, so grass springs upright as the aircraft leaves. */
struct WgrGrassDownwash
{
    float x;
    float z;
    float radius;
    float strength;
};

/* Live procedural-grass controls, edited through the developer Grass tab. */
struct WgrGrassParams
{
    float density;  /* retained candidate fraction, 0..1 */
    float spacing;  /* grid spacing in metres */
    float near_radius; /* dense-placement radius in metres */
    float enabled;  /* 0 = disabled, nonzero = enabled */
    float blade_height;   /* blade-height multiplier */
    float wind_strength;  /* test wind strength */
    float wind_direction; /* test wind direction, degrees */
    float far_radius;     /* coarse far-LOD radius */
    float interactor_x;   /* live player/vehicle world-space centre */
    float interactor_z;
    float interactor_radius;
    float interactor_strength;
    WgrGrassTrack tracks[WGR_GRASS_TRACK_COUNT];
    WgrGrassDownwash downwash[WGR_GRASS_DOWNWASH_COUNT];
    float debug_ignore_geography_exclusions;
    float clumping;        /* deterministic field-scale orientation/height/density variation */
    float color_variation; /* per-blade and field colour variation */
    float transmission;    /* backlit thin-blade scattering strength */
    float cast_shadows;    /* 0 = omit close grass from cascade shadow maps */
    float apply_fog;       /* 0 = leave procedural grass unfogged (diagnostic) */
    float density_noise_scale;    /* coverage-noise frequency (1/metres) */
    float density_noise_strength; /* 0 = uniform density; 1 = bare patches to dense clumps */
    /* Species mix, as fractions of all placed plants. Grass takes the remainder,
     * so weed + flower is clamped to <= 1. Selection is per clump, not per blade. */
    float weed_percent;
    float flower_percent;
    /* Blade width multiplier, 1.0 = stock. Wider blades give the near-LOD texture
     * pixels to land in: a 3 cm blade is only ~4 px on screen, where a 64 px-wide
     * texture averages to flat colour before it is ever drawn. */
    float blade_width_scale;
    /* 0 = mid LOD keeps the procedural ribbons (default); nonzero = photo tuft cards. */
    float use_photo_tuft;
    /* Grass albedo saturation about its luma. 1.0 = untouched, 0.0 = greyscale.
     * Applied to near blades, mid ribbons/clump cards and the far proxy alike. */
    float saturation;
    /* Sun-bleached patches: fraction of the field that dries toward straw, and
     * the patch size (noise frequency, 1/metres). */
    float dry_patches;
    float dry_patch_scale;
    float mid_radius;     /* opaque mid-clump radius (formerly reserved) */
    /* Blade shape controls. The four grass species used to share ONE silhouette,
     * so a field read as the same blade repeated; shape_variety blends from that
     * legacy behaviour (0) to eight distinct profiles (1), and the two jitters
     * add per-blade taper/bend spread on top. Continuous so the Grass tab can
     * A/B against the old look without a rebuild. */
    float shape_variety;
    float taper_jitter;
    float bend_jitter;
    /* Global scale on the near-LOD photo atlas, on top of its distance fade.
     * 0 = ignore the photo entirely and keep the procedural surface. */
    float blade_texture_strength;
    /* Alpha cut-out cards: silhouette from the texture instead of the quad, which
     * buys shape variety without more geometry but costs early-Z and adds
     * overdraw. Off by default; measure before adopting. card_widen widens the
     * quad so the cutout has material to remove. */
    float alpha_cards;
    float alpha_cutoff;
    float card_widen;
    /* How far a blade arcs over, as a multiple of its own height. The stock bend moved a tip
       5-19 cm on a ~0.8 m blade -- about ten degrees -- which read as a field of rigid spikes.
       Taller blades arc further, so this scales with height rather than being a fixed distance.
       0 restores the old rigid look. */
    float blade_arch;
    float clump_renderer;
    /* Photograph-only brightness trim; legacy ribbons and far grass ignore it. */
      float photo_tuft_brightness;
      /* Fraction of photographed clumps that uses the local mixed-grass atlas. */
      float photo_tuft_mix;
      /* World-space size of deterministic mixed-grass patches, in metres. */
      float photo_tuft_patch_size;
    /* PHOTOGRAPHED CLUMP CARDS ONLY (near + mid when use_photo_tuft is on).
     * Procedural ribbons, the procedural mid ring and the far coverage proxy are
     * deliberately untouched, so tuning cards cannot shift the whole field. */
    /* Contrast about the plate's own mid-luma; 1.0 = the raw photograph. The
     * source plates are flatly lit, which is why unmodified cards read flat. */
    float photo_contrast;
    /* Per-texel normal rebuilt from the plate's luma gradient, so individual
     * stems inside one card catch the sun differently. 0 = the old single flat
     * upright normal for the whole plate. */
    float photo_contour;
    /* Grass-on-grass shadowing: how strongly a clump is darkened by the cascade
     * the grass itself writes. 0 = none. */
    float photo_self_shadow;
    /* Ambient occlusion toward the clump root, where light does not reach. */
    float photo_root_ao;
    /* Near placement grid edge in cells. Derived by the renderer from radius and
     * spacing so near density is radius-independent; ignored on input. */
    float near_grid_dim;
    /* Mid placement grid edge in cells. Also renderer-derived; ignored on input. */
    float mid_grid_dim;
    /* Width in metres of the stochastic dissolve band at every LOD join: each ring
     * thins out across it as the next thickens, instead of one ring ending on a
     * hard circle. 0 restores the abrupt joins. */
    float lod_blend;
    /* Alpha test for the photographed cards. Deliberately separate from
     * `alpha_cutoff`, which belongs to the procedural cut-out path: the photo
     * plates come from JPEG opacity maps whose noisy edges are a flicker source. */
    float photo_alpha_cutoff;
    /* Force every photographed clump onto one atlas layer: -1 = normal selection,
     * 0 = the primary clump, 1..8 = one local family. */
    float photo_force_layer;
    /* Saturation of the photographed cards about their own luma. Separate from
     * the field-wide `saturation` so the plates can be matched to the procedural
     * grass instead of moving with it. 0.62 = the former constant. */
    float photo_saturation;
    /* How long a walked imprint survives, in seconds. It recovers over the last
     * third of this, so the trail thins out rather than vanishing. */
    float track_lifetime;
    /* How far a crushed plant is pressed down, as a fraction of its own height.
     * 0.55 is the long-standing value. Photographed cards scale this up
     * internally: a clump plate has to rotate much further than a blade before it
     * reads as flattened. */
    float imprint_depth;
    /* Relative selection weights for atlas layers 1..8. Not normalised; all-zero
     * falls back to the primary clump. */
    float photo_layer_weights[8];
    /* Per-LOD coverage multipliers on top of `density`, so the three rings can be
     * balanced independently. The near ring is where card overdraw is paid. */
    float near_density;
    float mid_density;
    float far_density;
    float _pad_density;
    /* Albedo tints applied after the existing colour work and before lighting.
     * `tint_procedural` covers procedural blades, ribbons and the far proxy;
     * `tint_photo` covers the photographed cards. White = untinted. */
    float tint_procedural[4];
    float tint_photo[4];
    /* Opaque bounding box per photo layer, (u0, u1, v0, v1) in texture space.
     * Renderer-derived from the uploaded atlas; ignored on input. */
    float photo_layer_bounds[36];
    /* Mean linear colour of each photo layer's covered texels, (r, g, b, 1). The
     * pivot the card contrast turns about, so contrast cannot double as a
     * brightness change. Renderer-derived; ignored on input. */
    float photo_layer_means[36];
};

// --- Water (GPU CDLOD surface) -----------------------------------------------

/* Per-map + per-frame water parameters (a small UBO). `world_origin`/`terrain_grid`/
 * `hm_width`/`hm_height` describe the terrain heightmap the sibling look plan samples
 * for shoreline depth (unused by the flat-plane geometry pass); `sea_level` is the
 * animated global sea height (Landscape::GetSeaLevel) and `time` the wave-animation
 * clock (seconds). The look block (wave_amp..alpha) is the live-tunable water look,
 * edited by the ImGui Water tab; `fade_start`/`fade_end` flatten wave detail with
 * distance (metres) to kill far-field moiré. All refreshed every frame. */
struct WgrWaterParams
{
    WgrVec2 world_origin;
    float terrain_grid;
    float sea_level;
    uint32_t hm_width;
    uint32_t hm_height;
    float time;
    float wave_amp;
    float wave_choppy;
    float wave_speed;
    float wave_scale;
    float fade_start;
    float fade_end;
    float warp_amp;
    float spec_power;
    float spec_intensity;
    float alpha;
    float shadow_dim;
    /* Depth-based colour + soft shoreline (Stage 2). color_ext = 1/m extinction: how fast the
     * body tint saturates shallow -> deep with the water column depth. coast_fade = metres of
     * column depth over which the shoreline ramps transparent -> opaque. */
    float color_ext;
    float coast_fade;
    /* rgb = shallow / deep body colour (gamma-space; decoded to linear on HDR). w unused. */
    WgrVec4 shallow_color;
    WgrVec4 deep_color;
    /* Coast foam + swash (Stage 2c). foam_width = m of column depth over which shoreline foam
     * fades out; foam_intensity scales it. swash_amp = m the near-shore waterline oscillates
     * in/out; swash_speed = cycles/s. Cosmetic (buoyancy stays on the flat plane). */
    float foam_width;
    float foam_intensity;
    float swash_amp;
    float swash_speed;
    /* Shared FFT ocean controls. fft_control = enabled, deterministic seed, minimum geometry
     * wavelength, pad. fft_wind_sea = wind x/z, speed (m/s), sea state (0..1). The four
     * cascade lengths are world metres and must remain stable across camera-origin changes.
     * WTR-001 — the "minimum geometry wavelength" lane (fft_control.z) was set once by the
     * C++ ctor to 12.0f but never read on the Rust/shader side, so it is repurposed as the
     * WTR freeze mask: WGR_WATER_FREEZE_* bits OR-ed together. 0.0 (no freeze) preserves the
     * legacy default (a harmless constant float the shaders ignore); the Rust side reads the
     * float's bit pattern as the mask only for its own dispatch skip, never as a wavelength. */
    WgrVec4 fft_control;
    WgrVec4 fft_wind_sea;
    WgrVec4 fft_cascade_lengths;
    /* Optional directed surface flow. xy is world-xz direction, z is metres/second and w is
     * WgrWaterKind. Zero is the established global ocean behavior. The current CDLOD API has
     * one global material, so river producers must not set this until per-water-body batches exist. */
    WgrVec4 flow_direction_speed;
    /* WTR-003 — water debug views (dev-only; the Water tab "Debug views" section). x is the
     * WgrWaterDebugView index (0 = normal shading); the fragment shader replaces its output
     * with the selected diagnostic when non-zero. y gates the GPU whitewater/spray billboard
     * pass and z controls its activity. w is the live viewport height in pixels, consumed by
     * the shader's per-cascade projected-pixel filtering. Appended at the struct end so the
     * existing lanes keep their offsets; the sizeof assert below moves 192 -> 208 in lockstep
     * with the Rust side. */
    WgrVec4 debug_params;
    /* WTR-LOOK — surface energy model + its gains (the Water tab "Surface look" section).
     * x selects the composite: 0 = legacy (capped Fresnel, 0.12x specular, SSS multiplied by the
     * body colour), 1 = physical (uncapped Fresnel, variance-filtered GGX sun glitter at full
     * radiance, SSS as its own light path). y/z/w are artist gains on glitter / subsurface
     * scattering / environment reflection, 1.0 = the model's own energy. Appended at the struct
     * end so every earlier lane keeps its offset; sizeof moves 208 -> 224 with the Rust side. */
    WgrVec4 look_params;
    /* WTR-LOOK — sea state, quality and shore-wave lanes.
     * x: 1 = the amplitude control drives a physically coupled sea state (cascade wind speed and
     *    tile lengths scale together, so a taller sea is also a LONGER sea), 0 = legacy uniform
     *    variance scaling (taller waves at an unchanged wavelength — steepness, not sea state).
     * y: residual spectrum amplitude the h0 pass should apply. 1.0 when the coupling above already
     *    carries the energy; the raw amplitude in legacy mode. Squared by the spectrum (variance).
     * z: 1 = low water quality (drops SSR, planar reflection, bicubic filtering and the two
     *    smallest cascades in the water fragment shader). 0 = full quality.
     * w: shore breaker gain. Appended at the struct end; sizeof moves 224 -> 240 with Rust. */
    WgrVec4 sea_params;
    /* Underwater tuning, all live from the Water tab's "Underwater effect" section so the look
     * can be dialled in without a rebuild. These do nothing while the effect is off.
     * x: engage band in metres. The compositor runs while the camera is below sea level + this,
     *    so it can still classify a view that straddles the surface. Raising it past the crest
     *    height engages the pass in open air, which costs the froxel and caustic dispatches for
     *    a frame that returns no water path.
     * y: density multiplier on the absorption. 1.0 = the tuned default.
     * z: colour bias, 0..1. 1 = absorption hue derived entirely from the authored deep swatch,
     *    so the volume matches the surface; 0 = the neutral (0.280, 0.065, 0.020) curve the
     *    effect used before, which had no relation to the water you swam into.
     * w: caustic gain, 1.0 = the tuned CAUSTIC_STRENGTH.
     * Appended at the struct end; sizeof moves 240 -> 256 with the Rust side. */
    WgrVec4 underwater_params;
    /* x: 1 = the underwater effect is enabled, 0 = off. THIS is what switches the compositor.
     *
     * It needs its own lane because the effect has two independent triggers and the Water tab
     * must own both. `fft_control.w` (eye submersion depth) drives the water shader's own
     * underwater tint, but the fullscreen compositor engages on EITHER that depth or the camera
     * being within the engage band of sea level — and the band test has no idea whether the
     * effect is wanted. Gating only the depth left the compositor running with the checkbox off,
     * because a submerged camera is always inside the band.
     * yzw reserved. Appended at the struct end; sizeof moves 256 -> 272 with the Rust side. */
    WgrVec4 underwater_gate;
};

struct WgrWaterCascadeConfig
{
    uint32_t enabled;
    uint32_t resolution; /* live FFT tier: 256, 512, or Godot-reference 1024 */
    float tile_length_x;
    float tile_length_y;
    float displacement_scale;
    float horiz_displacement_scale;
    float normal_scale;
    float foam_scale;
    float wind_speed;
    float wind_direction_rad;
    float fetch_meters;
    float water_depth_meters;
    float swell;
    float directional_spread;
    float short_wave_detail;
    float whitecap_threshold;
    uint32_t spectrum_seed;
    float phase_offset_seconds;
    /* Reserved for WTR-185 (reduced-rate cascade update scheduling with interpolation).
     * Currently unused: every enabled cascade evolves every frame. Kept in the ABI so the
     * scheduling work does not need a struct change later. */
    float update_rate_hz;
    float pad;
};

constexpr uint32_t WGR_MAX_WATER_INTERACTIONS = 48;

/* WTR-001 — deterministic water freeze mask, OR-ed into WgrWaterParams.fft_control.z.
 * The freeze flags reflect the dev-only `Engine::WaterSettings::Freeze` block (the Water
 * tab's Debug section). The Rust side reads the float's bit pattern in
 * `Water::update_interactions` and skips the matching dispatch, so a frozen frame repeats
 * its last water state instead of advancing through a no-advancement compute pass. */
enum WgrWaterFreezeBits : uint32_t
{
    WGR_WATER_FREEZE_FFT = 1u << 0,           // skip Fft::dispatch (spectrum holds at last h0/time)
    WGR_WATER_FREEZE_INTERACTION = 1u << 1,    // skip Interaction::dispatch
    WGR_WATER_FREEZE_FOAM = 1u << 2,          // skip Foam::dispatch
};

/* WTR-002 — GPU timestamp regions, the index contract of wgr_get_gpu_timings (mirrors
 * `Region` in rust/src/gpu_timers.rs — append only, never reorder). Regions marked
 * "reserved" name spec rows whose standalone pass doesn't exist yet; they always report
 * -1 ms ("n/a") so the tab rows + ABI are already in place when those passes land.
 * SSR/refraction remain fragment work inside WATER_DRAW. */
enum WgrGpuTimerRegion : uint32_t
{
    WGR_GPU_TIMER_SPECTRUM_INIT = 0,        // h0 spectrum generation (spectrum-dirty frames only)
    WGR_GPU_TIMER_SPECTRUM_EVOLVE = 1,      // per-frame spectrum evolution
    WGR_GPU_TIMER_FFT_HORIZONTAL = 2,       // FFT butterfly stages, axis 0
    WGR_GPU_TIMER_FFT_VERTICAL = 3,         // FFT butterfly stages, axis 1
    WGR_GPU_TIMER_FFT_COMPOSE = 4,          // displacement/dynamics/auxiliary composition
    WGR_GPU_TIMER_INTERACTION = 5,          // injection + propagation (one fused kernel today)
    WGR_GPU_TIMER_FOAM = 6,                 // persistent foam update
    WGR_GPU_TIMER_WHITEWATER = 7,           // reserved — no whitewater pass yet
    WGR_GPU_TIMER_PLANAR_SKY = 8,           // planar reflection: sky
    WGR_GPU_TIMER_PLANAR_TERRAIN = 9,       // planar reflection: terrain
    WGR_GPU_TIMER_PLANAR_OBJECTS = 10,      // planar reflection: reflected cull + objects
    WGR_GPU_TIMER_PLANAR_CLOUDS = 11,       // planar reflection: cloud march + composite
    WGR_GPU_TIMER_PLANAR_MIPS = 12,         // planar reflection mip generation
    WGR_GPU_TIMER_WATER_SSR = 13,           // reserved — in-shader inside WATER_DRAW
    WGR_GPU_TIMER_WATER_REFRACTION = 14,    // reserved — in-shader inside WATER_DRAW
    WGR_GPU_TIMER_WATER_DRAW = 15,          // water surface pass (includes SSR + refraction)
    WGR_GPU_TIMER_UNDERWATER_FROXEL = 16,   // camera frustum volume lighting compute
    WGR_GPU_TIMER_UNDERWATER_COMPOSITE = 17, // fullscreen waterline-aware compositor
    WGR_GPU_TIMER_CAUSTICS = 18,            // FFT-derived camera-centred caustic compute
    /* Water rows end here; the Water tab slices [0, WATER_REGION_COUNT). */
    WGR_GPU_TIMER_WATER_REGION_COUNT = 19,
    /* GRS-A — grass. The three placement dispatches are standalone compute passes and
     * bracket on the encoder. The draw rows share a render pass with the rest of the
     * 3D plan, so they need TIMESTAMP_QUERY_INSIDE_PASSES and read "n/a" without it. */
    WGR_GPU_TIMER_GRASS_PLACE_NEAR = 19,    // cs_place dispatch (512x512 candidates)
    WGR_GPU_TIMER_GRASS_PLACE_MID = 20,     // cs_place_mid dispatch (384x384)
    WGR_GPU_TIMER_GRASS_PLACE_FAR = 21,     // cs_place_far dispatch (384x384)
    WGR_GPU_TIMER_GRASS_PREPASS = 22,       // grass depth/normal prepass (near + mid)
    WGR_GPU_TIMER_GRASS_COLOR = 23,         // grass colour pass (far + mid + near)
    WGR_GPU_TIMER_GRASS_SHADOW = 24,        // near blades into the cascade depth map
    WGR_GPU_TIMER_FRAME_TOTAL = 25,         // all submitted frame work (excludes acquire/present pacing)
    /* LIT-020 — interior sky visibility. Appended after FRAME_TOTAL rather than inserted next to
     * it: these indices are the FFI contract the debug tabs slice by, so renumbering silently
     * relabels every existing row. */
    WGR_GPU_TIMER_INTERIOR_SKY_CULL = 26, // one cull dispatch chain per sampled sky direction
    WGR_GPU_TIMER_INTERIOR_SKY_DRAW = 27, // the per-direction depth passes that fill the map
    /* LIT-010 — screen-space AO, split so the three stages can be attributed separately. */
    WGR_GPU_TIMER_GTAO_PREP = 28,    // depth resolve + normal resolve + linear-Z mip chain
    WGR_GPU_TIMER_GTAO_COMPUTE = 29, // the horizon march (scales with slices x steps)
    WGR_GPU_TIMER_GTAO_BLUR = 30,    // bilateral denoise (scales with blur radius)
    WGR_GPU_TIMER_REGION_COUNT = 31,
};

/* GRS-A — grass instance accounting (mirrors WgrGrassStats in rust/src/ffi.rs).
 * Read back asynchronously from the three atomic placement counters, so the values
 * lag the displayed frame by the readback ring depth (~2-3 frames). `candidates`
 * are the fixed dispatch sizes, so accepted/candidates is the acceptance rate. */
struct WgrGrassStats
{
    uint32_t near_instances;
    uint32_t mid_instances;
    uint32_t far_instances;
    uint32_t near_candidates;
    uint32_t mid_candidates;
    uint32_t far_candidates;
    uint32_t near_vertices;
    uint32_t mid_vertices;
    uint32_t far_vertices;
};

enum WgrWaterKind : uint32_t { WGR_WATER_KIND_OCEAN = 0, WGR_WATER_KIND_RIVER = 1 };
/* WTR-003 — water debug view selector, written to WgrWaterParams.debug_params.x. The water
 * fragment shader maps these to on-surface diagnostics; 0 is normal shading. Views whose
 * backing pass does not exist yet (the whitewater pool/overflow diagnostics) are listed
 * for a stable UI but render black until those passes land. The
 * ordering mirrors the Water tab combo and the shader's debug_view() switch. */
enum WgrWaterDebugView : uint32_t
{
    WGR_WATER_DEBUG_OFF = 0,                 // normal shading
    WGR_WATER_DEBUG_FFT_DISPLACEMENT = 1,    // |displacement.xyz| summed over cascades
    WGR_WATER_DEBUG_FFT_HORIZONTAL = 2,      // horizontal displacement magnitude (xz)
    WGR_WATER_DEBUG_FFT_VERTICAL = 3,        // vertical displacement (y), signed heatmap
    WGR_WATER_DEBUG_FFT_SLOPE = 4,           // |dynamics.xy| slope magnitude
    WGR_WATER_DEBUG_JACOBIAN = 5,            // auxiliary.x Jacobian (1 = unfolded)
    WGR_WATER_DEBUG_COMPRESSION = 6,         // auxiliary.y horizontal compression
    WGR_WATER_DEBUG_CURVATURE = 7,           // auxiliary.z positive curvature
    WGR_WATER_DEBUG_CREST_ENERGY = 8,        // displacement.w crest energy
    WGR_WATER_DEBUG_SLOPE_VARIANCE = 9,      // auxiliary.w resolved slope variance
    WGR_WATER_DEBUG_MATERIAL_COORD = 10,     // undisplaced base xz (uv of the domain)
    WGR_WATER_DEBUG_DISPLACED_COORD = 11,    // displaced world xz
    WGR_WATER_DEBUG_INTERACTION_HEIGHT = 12, // interaction field .r (signed heatmap)
    WGR_WATER_DEBUG_INTERACTION_VELOCITY = 13, // interaction field .g (signed heatmap)
    WGR_WATER_DEBUG_INTERACTION_FOAM = 14,   // interaction field .b aeration
    WGR_WATER_DEBUG_FOAM_SOURCE = 15,        // breaker source (fft gates + aeration)
    WGR_WATER_DEBUG_FOAM_HISTORY = 16,       // persistent foam coverage (history .r)
    WGR_WATER_DEBUG_SURFACE_VELOCITY = 17,   // interaction velocity as a flow vector
    WGR_WATER_DEBUG_WATER_DEPTH = 18,        // reconstructed water-column depth
    WGR_WATER_DEBUG_CAMERA_DISTANCE = 19,    // camera-to-surface distance
    WGR_WATER_DEBUG_SSR_COLOR = 20,          // screen-space reflection colour
    WGR_WATER_DEBUG_SSR_CONFIDENCE = 21,     // SSR hit weight
    WGR_WATER_DEBUG_PLANAR_COLOR = 22,       // planar reflection colour
    WGR_WATER_DEBUG_PLANAR_VALIDITY = 23,    // planar reflection validity
    WGR_WATER_DEBUG_SKY_REFLECTION = 24,     // directional sky/cloud reflection
    WGR_WATER_DEBUG_REFLECTION_SOURCE = 25,  // final reflection-source selection (rgb coded)
    WGR_WATER_DEBUG_REFRACTION_RAY = 26,     // refraction uv offset (pixel space)
    WGR_WATER_DEBUG_REFRACTION_VALIDITY = 27,// refracted scene hit validity
    WGR_WATER_DEBUG_REFRACTION_PATH = 28,    // refraction path length (column depth)
    WGR_WATER_DEBUG_TRANSMITTANCE = 29,      // RGB transmittance
    WGR_WATER_DEBUG_UNDERWATER_EXTINCTION = 30,  // froxel RGB transmission
    WGR_WATER_DEBUG_UNDERWATER_INSCATTER = 31,   // froxel in-scattered radiance
    WGR_WATER_DEBUG_GODRAY_VISIBILITY = 32,      // terrain + cascade shadow visibility
    WGR_WATER_DEBUG_CAUSTIC_INTENSITY = 33,      // FFT-derived caustic intensity
    WGR_WATER_DEBUG_WHITEWATER_STATE = 34,       // reserved — no whitewater pass yet
    WGR_WATER_DEBUG_WHITEWATER_POOL = 35,        // reserved — no whitewater pass yet
    WGR_WATER_DEBUG_PARTICLE_OVERFLOW = 36,      // reserved — no whitewater pass yet
    WGR_WATER_DEBUG_SURFACE_SPEED = 37,          // WTR-012 — |interaction velocity| heatmap
    /* WTR-012 — |interaction height| heatmap. Named PREV_DISP_DELTA when it was believed to show
     * a previous-displacement delta; nothing stores one (the interaction field is height,
     * velocity, foam, unused), so it actually drew the velocity channel again on a second scale
     * while the height channel had no view. The name is kept for ABI stability — this is a
     * wire-visible enum — and corrected here and in the overlay label. */
    WGR_WATER_DEBUG_PREV_DISP_DELTA = 38,
    WGR_WATER_DEBUG_WTR40_DIR_SKY = 39,          // WTR-040 — directional atmosphere only
    WGR_WATER_DEBUG_WTR40_DIR_CLOUDS = 40,       // WTR-040 — directional cloud contribution
    WGR_WATER_DEBUG_WTR40_PLANAR_SKY = 41,       // WTR-040 — planar sky only
    WGR_WATER_DEBUG_WTR40_PLANAR_CLOUDS = 42,    // WTR-040 — planar cloud contribution
    WGR_WATER_DEBUG_WTR40_PLANAR_GEOM = 43,      // WTR-040 — planar terrain/objects only
    WGR_WATER_DEBUG_WTR40_PLANAR_VALIDITY = 44,  // WTR-040 — planar geometry validity mask
    WGR_WATER_DEBUG_WTR40_SSR_ONLY = 45,         // WTR-040 — SSR only
    WGR_WATER_DEBUG_WTR40_OWNER_BADGE = 46,      // WTR-040 — final reflection owner (R=SSR, B=planar, G=directional)
    WGR_WATER_DEBUG_VIEW_COUNT = 47,
};
enum WgrWaterInteractionKind : uint32_t { WGR_WATER_INTERACTION_BULLET = 0, WGR_WATER_INTERACTION_OBJECT = 1, WGR_WATER_INTERACTION_PLAYER = 2, WGR_WATER_INTERACTION_EXPLOSION = 3, WGR_WATER_INTERACTION_FOOTSTEP = 4, WGR_WATER_INTERACTION_CONTINUOUS = 5 };
enum WgrWaterInteractionFlags : uint32_t { WGR_WATER_INTERACTION_PENDING_IMPULSE = 1u << 0, WGR_WATER_INTERACTION_CAPSULE = 1u << 8, WGR_WATER_INTERACTION_PLAYER_WADING = 1u << 9, WGR_WATER_INTERACTION_PLAYER_SWIMMING = 1u << 10, WGR_WATER_INTERACTION_LEFT_SIDE = 1u << 11, WGR_WATER_INTERACTION_LARGE_BODY = 1u << 12 };
struct alignas(16) WgrWaterInteractionEvent { WgrVec4 position_radius, velocity_kind, time_life_foam_mass, direction_depth_flags; };
struct alignas(16) WgrWaterInteractionParams { WgrVec4 domain, previous_domain, grid, physics, misc, weather; };

/* One water node instance: byte-identical to WgrTerrainNode (the shared grid mesh
 * placed at world-xz `origin`, `size` x `size`, level `lod`, morphing over the
 * `morph_start`/`morph_end` camera-distance band). A distinct type so the two can
 * evolve independently (the look plan adds per-node flags). */
struct WgrWaterNode
{
    WgrVec2 origin;
    float size;
    uint32_t lod;
    float morph_start;
    float morph_end;
    /* CPU-derived direction from the nearby shallow-water tile toward the closest
     * shore, plus a 0..1 shallow/coast weight.  This lets the vertex shader add a
     * shoreward breaker train without rotating the global open-ocean FFT field. */
    WgrVec2 shore_direction;
    float shore_factor;
    float _shore_pad;
};

/* A run [first_node, first_node+node_count) of WgrFrame.water_nodes drawn with the
 * shared grid mesh, transformed by camera `camera` (indexes WgrFrame.cameras). */
struct WgrWaterBatch
{
    uint32_t first_node;
    uint32_t node_count;
    uint32_t camera;
    uint32_t _pad;
};

/* Overlay (dev panel / ImGui) vertex: framebuffer pixels, top-left origin.
 * `color` is RGBA with R in the low byte (ImGui packing, NOT WgrRgba8). */
struct WgrOverlayVertex
{
    WgrVec2 pos;
    WgrVec2 uv;
    uint32_t color;
};

/* One scissored overlay draw: `index_count` indices from
 * WgrFrame.overlay_indices starting at `first_index`, offset by `base_vertex`
 * into WgrFrame.overlay_verts, clipped to `clip` = {x0, y0, x1, y1} pixels. */
struct WgrOverlayDraw
{
    WgrVec4 clip;
    WgrTexture texture_id; // 0 = built-in white
    uint32_t first_index;
    uint32_t index_count;
    uint32_t base_vertex;
    uint32_t _pad;
};

// --- Frame -------------------------------------------------------------------

/* Everything needed to render + present one frame. The renderer clears to
 * `clear` (+depth), then replays `cmds` in submission order: each 2D batch and
 * 3D draw renders interleaved exactly as recorded, so 3D UI elements land
 * between their 2D background and foreground. WGR_CMD_CLEAR_DEPTH starts a new
 * segment with a freshly cleared depth buffer (colour preserved). 3D draws are
 * depth-tested and transformed by `cameras[draw.camera]`. `fog_color` is what
 * each vertex's `fog` blends toward. Any slice may be empty. */
struct WgrFrame
{
    WgrVec4 clear;
    WgrVec3 fog_color;

    WgrSlice<WgrCamera> cameras;
    WgrSlice<WgrDraw3D> draws3d;
    WgrSlice<WgrVertex2D> verts;
    WgrSlice<WgrDraw2DBatch> batches;
    WgrSlice<WgrCmd> cmds;
    /* Bone-matrix pool for skinned draws: one 128-matrix block per palette slot,
     * world already pre-multiplied in (palette[i] = world * boneMatrix[i]). Length is a
     * multiple of 128. Empty if no skinned draws. */
    WgrSlice<WgrMat4> palette;

    /* Cascaded-shadow depth pass: rendered before the command stream when
     * shadow.count > 0 and shadow_casters is non-empty. */
    WgrShadowPass shadow;
    WgrSlice<WgrShadowCaster> shadow_casters;

    /* Overlay (dev panel): alpha-blended over the finished frame, no depth. */
    WgrSlice<WgrOverlayVertex> overlay_verts;
    WgrSlice<uint16_t> overlay_indices;
    WgrSlice<WgrOverlayDraw> overlay_draws;

    /* GPU terrain nodes, drawn on WGR_CMD_DRAW_TERRAIN. The heightmap + ground
     * textures are uploaded separately via wgr_terrain_*. */
    WgrSlice<WgrTerrainNode> terrain_nodes;
    WgrSlice<WgrTerrainBatch> terrain_batches;

    /* Frame-global point/spot lights (<= 256), uploaded once into the group-0
     * storage buffer shared by 3D draws + terrain. The per-camera light count
     * rides in WgrCamera::cam_pos.w. */
    WgrSlice<WgrLight> lights;

    /* GPU water nodes, drawn on WGR_CMD_DRAW_WATER. Placement params (incl. the
     * per-frame sea level) are uploaded separately via wgr_water_set_params. */
    WgrSlice<WgrWaterNode> water_nodes;
    WgrSlice<WgrWaterBatch> water_batches;

    /* Procedural GPU grass, drawn on WGR_CMD_DRAW_GRASS. Static placement
     * metadata is uploaded separately via wgr_grass_set_geography. */
    WgrSlice<WgrGrassBatch> grass_batches;
};

// --- Layout guards (mirror rust/src/ffi.rs) ----------------------------------

static_assert(sizeof(WgrVec2) == 8, "WgrVec2 must be 2 floats");
static_assert(sizeof(WgrVec3) == 12, "WgrVec3 must be 3 floats");
static_assert(sizeof(WgrVec4) == 16, "WgrVec4 must be 4 floats");
static_assert(sizeof(WgrMat4) == 64, "WgrMat4 must be 16 floats");
static_assert(sizeof(WgrSurfaceDesc) == 32, "WgrSurfaceDesc must match Rust on 64-bit targets");
static_assert(sizeof(WgrLogCallbacks) == 16, "WgrLogCallbacks must match Rust on 64-bit targets");
static_assert(sizeof(WgrSlice<WgrCamera>) == 16 && alignof(WgrSlice<WgrCamera>) == 8,
              "WgrSlice must be a { pointer, u32 } with 8-byte alignment");
static_assert(sizeof(WgrBlend) == 4, "WgrBlend must be 4 bytes to match the Rust #[repr(u32)] enum");
static_assert(sizeof(WgrVertex2D) == 32, "WgrVertex2D layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrMeshVertex) == 60, "WgrMeshVertex must match the engine SVertex layout");
static_assert(sizeof(WgrDraw2DBatch) == 32, "WgrDraw2DBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrDraw3D) == 272, "WgrDraw3D layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrLight) == 64, "WgrLight layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTonemap) == 48, "WgrTonemap layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrExposure) == 32, "WgrExposure layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSky) == 256, "WgrSky layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSkyLook) == 192, "WgrSkyLook layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSkyRuntime) == 80, "WgrSkyRuntime layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTerrainSunShadow) == 16, "WgrTerrainSunShadow layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSkyVisibility) == 32, "WgrSkyVisibility layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrFoliage) == 48, "WgrFoliage layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGtao) == 52, "WgrGtao layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSkyVis) == 44, "WgrSkyVis layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrRenderParams) == 464, "WgrRenderParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrFrameParams) == 16, "WgrFrameParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrCameraShadow) == 352, "WgrCameraShadow layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrCamera) == 576, "WgrCamera layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrShadowCaster) == 136, "WgrShadowCaster layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrShadowPass) == 288, "WgrShadowPass layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrCmd) == 8, "WgrCmd layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrOverlayVertex) == 20, "WgrOverlayVertex layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrOverlayDraw) == 40, "WgrOverlayDraw layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTerrainParams) == 64, "WgrTerrainParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTerrainNode) == 24, "WgrTerrainNode layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTerrainBatch) == 16, "WgrTerrainBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGrassBatch) == 16, "WgrGrassBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGrassTrack) == 16, "WgrGrassTrack layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGrassDownwash) == 16, "WgrGrassDownwash layout must match the Rust #[repr(C)] struct");
  static_assert(sizeof(WgrGrassParams) == 4736, "WgrGrassParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrWaterParams) == 272, "WgrWaterParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrWaterNode) == 40, "WgrWaterNode layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrWaterBatch) == 16, "WgrWaterBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrWaterInteractionEvent) == 64 && alignof(WgrWaterInteractionEvent) == 16, "WgrWaterInteractionEvent must match Rust");
static_assert(sizeof(WgrWaterInteractionParams) == 96 && alignof(WgrWaterInteractionParams) == 16, "WgrWaterInteractionParams must match Rust");
static_assert(sizeof(WgrFrame) == 576, "WgrFrame layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrAbiCheck) == 24, "WgrAbiCheck layout must match Rust");

// --- Functions ---------------------------------------------------------------

extern "C"
{
    WGR_API const char* wgr_version(void);
    WGR_API uint32_t wgr_abi_version(void);
    WGR_API const char* wgr_build_id(void);
    WGR_API int32_t wgr_abi_validate(const WgrAbiCheck* check);
    WGR_API void wgr_screenshot_request(WgrRenderer* renderer);
    WGR_API uint32_t wgr_screenshot_take(WgrRenderer* renderer, uint8_t* out, uint32_t out_len, uint32_t* width,
                                          uint32_t* height);

    /* Returns NULL on failure (reason reported via `log` if supplied). `log` may be NULL. */
    WGR_API WgrRenderer* wgr_create(const WgrSurfaceDesc* desc, const WgrLogCallbacks* log);

    WGR_API void wgr_destroy(WgrRenderer* renderer);
    WGR_API void wgr_resize(WgrRenderer* renderer, uint32_t width, uint32_t height);
    /* Presentation interval: 0 = immediate/no VSync, 1 = FIFO/VSync, -1 = adaptive. */
    WGR_API int32_t wgr_set_present_mode(WgrRenderer* renderer, int32_t interval);

    /* Upload a texture in `format` (WgrTextureFormat); returns a non-zero id, or
     * 0 on failure. `data` holds `mip_count` tightly packed mip levels, level i
     * sized for (max(1, width>>i), max(1, height>>i)): RGBA8 = w*h*4 per level;
     * BC* = the block-payload size (ceil(w/4)*ceil(h/4) * 8 for BC1, * 16 for
     * BC2/BC3). `byte_length` is the total. Pass WGR_TEXTURE_GEN_MIPS in `flags`
     * (RGBA8, mip_count 1 only) to generate the rest of the chain with a box
     * filter. */
    WGR_API WgrTexture wgr_texture_create(WgrRenderer* renderer, uint32_t width, uint32_t height, int32_t format,
                                          uint32_t mip_count, uint32_t flags, const uint8_t* data,
                                          uint32_t byte_length);

    constexpr uint32_t WGR_TEXTURE_GEN_MIPS = 1;

    /* Replace the pixels of an existing RGBA8 texture. */
    WGR_API void wgr_texture_update(WgrRenderer* renderer, WgrTexture id, const uint8_t* rgba, uint32_t byte_length);

    WGR_API void wgr_texture_destroy(WgrRenderer* renderer, WgrTexture id);

    /* Create a static mesh from interleaved vertices + 16-bit triangle-list
     * indices; returns a non-zero handle, or 0 on failure. */
    WGR_API WgrMesh wgr_mesh_create(WgrRenderer* renderer, WgrSlice<WgrMeshVertex> verts, WgrSlice<uint16_t> indices);

    /* Re-upload vertex data for an existing mesh (dynamic / animated shapes).
     * The topology (indices) is unchanged; the vertex count must not exceed the
     * mesh's original vertex count. */
    WGR_API void wgr_mesh_update(WgrRenderer* renderer, WgrMesh id, WgrSlice<WgrMeshVertex> verts);

    /* Attach per-vertex skinning data to a mesh: 4 bone indices + 4 quantised
     * weights per vertex (each buffer `4 * vert_count` bytes). Weights are
     * Unorm8x4 (0..255 -> 0..1) and should sum to ~1 per vertex. */
    WGR_API void wgr_mesh_set_skin(WgrRenderer* renderer, WgrMesh id, WgrSlice<uint8_t> bones,
                                   WgrSlice<uint8_t> weights);

    WGR_API void wgr_mesh_destroy(WgrRenderer* renderer, WgrMesh id);

    /* --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) --- */

    /* Sentinel returned by wgr_model_register on failure. */
    constexpr uint32_t WGR_INVALID_MODEL = 0xFFFFFFFFu;

    /* Register one opaque-rigid model for GPU-driven rendering. `lods`, `sections`, and
     * `materials` describe a single LODShapeWithShadow: `sections` and `materials` are
     * parallel (one material per section) and each lods[i].section_base indexes
     * `sections` relative to this model. Section mesh handles are resolved to the
     * shared geometry pool. Returns the model id (for wgr_instance_add) or
     * WGR_INVALID_MODEL on error. Call once per shape. */
    WGR_API uint32_t wgr_model_register(WgrRenderer* renderer, float bounding_sphere, WgrSlice<WgrModelLod> lods,
                                        WgrSlice<WgrModelSection> sections, WgrSlice<WgrModelMaterial> materials);

    /* Register a batch of per-tree crown centres (MODEL space) into the global crown-centre
     * table and return the base index of this batch (foliage-translucency-plan.md §9 Approach A).
     * Each centre's .xyz is a tree's canopy centroid in the shape's vertex space (.w unused).
     * The caller bakes `base + local_component_index` into each forest vertex's `conform` word;
     * vs_gpu reads the table with that index to get a per-tree radial-normal centre instead of the
     * whole-mesh inst.center. Call once per forest LOD level during model registration. */
    WGR_API uint32_t wgr_register_crown_centres(WgrRenderer* renderer, WgrSlice<WgrVec4> centres);

    /* Add a static retained instance; returns its stable slot (recycled from removed
     * slots). Update it in place with wgr_instance_update (a move, or a destruction-
     * phase change), remove it with wgr_instance_remove. */
    WGR_API uint32_t wgr_instance_add(WgrRenderer* renderer, const WgrInstance* inst);
    WGR_API void wgr_instance_update(WgrRenderer* renderer, uint32_t slot, const WgrInstance* inst);
    WGR_API void wgr_instance_remove(WgrRenderer* renderer, uint32_t slot);

    /* Replace the whole dynamic instance set for this frame (the churny set the CPU
     * already walks for simulation: vehicles, units, ...). Re-copied wholesale each
     * frame. */
    WGR_API void wgr_set_dynamic(WgrRenderer* renderer, WgrSlice<WgrInstance> instances);

    /* Push this frame's engine-derived cull + LOD inputs (the real Scene::LevelFromDistance2
     * values): `objects_z` = ENGINE_CONFIG.objectsZ draw distance, `lod_scale` = Camera::Left()
     * (projection tan(halfFovX)), `lod_inv_width` = Scene::GetLodInvWidth()
     * (~ lodCoef*2/screenWidth), `pixel_limit` = the legacy 0.125 sub-pixel threshold. No-op
     * unless GPU-driven rendering is enabled. Call once per frame for the main scene camera. */
    WGR_API void wgr_set_cull_params(WgrRenderer* renderer, float objects_z, float lod_scale,
                                     float lod_inv_width, float pixel_limit);

    /* Per-frame gate for the retained GPU-driven world set. When `suppress` is true the
     * renderer skips the GPU-driven object draws (colour + prepass) for the frame, so the
     * editor / loading / shutdown frames letterbox to black instead of leaking clutter behind
     * the 2D UI. Resources stay resident; only the draw submission is skipped. No-op unless
     * GPU-driven rendering is enabled. Call every frame with the current state. */
    WGR_API void wgr_set_suppress_world_objects(WgrRenderer* renderer, bool suppress);

    /* Debug/feature toggles for the GPU-driven cull (ImGui Culling tab): `draw_spheres` renders
     * the per-instance frustum-cull sphere wireframes on top of the scene; `no_frustum` skips the
     * GPU frustum test entirely (a "is the cull dropping it?" discriminator); `occlusion` enables
     * GPU Hi-Z occlusion culling (docs/gpu-culling-and-depth-plan.md §5 — the color pass draws
     * only the retained objects not hidden by the depth-prepass occluders). No-op unless GPU-driven
     * rendering is enabled. */
    WGR_API void wgr_set_cull_debug(WgrRenderer* renderer, bool draw_spheres, bool no_frustum, bool occlusion);

    /* Upload (or replace) the terrain heightmap: `heights` is
     * params->hm_width * params->hm_height row-major world-height floats (row 0 =
     * texel z=0). Creates the R32Float heightmap texture + params UBO, once per
     * map load. */
    WGR_API void wgr_terrain_set_heightmap(WgrRenderer* renderer, const float* heights,
                                           const WgrTerrainParams* params);

    /* Refresh the terrain params UBO without re-uploading the heightmap. Cheap; called every
     * frame to animate the coast wet band (sea_level / time / swash / wet_*). */
    WGR_API void wgr_terrain_set_params(WgrRenderer* renderer, const WgrTerrainParams* params);

    /* Set the terrain ground layers: `handles[i]` is the wgr_texture_create
     * handle for Landscape texture index i (0 = the built-in white fallback).
     * Layers keep their native size/format/mips; the fragment shader samples
     * them through a bindless binding_array, indexed per land cell by
     * wgr_terrain_set_index_map. At most WGR_TERRAIN_MAX_GROUND_LAYERS are
     * used; the index-map upload must clamp cell indices to the same bound. */
    WGR_API void wgr_terrain_set_ground_layers(WgrRenderer* renderer, const uint64_t* handles, uint32_t count);

    // The budget is dominated by per-tile images, so it scales with map AREA:
    // every satellite segment contributes its own `_lco` and `_lca`, and the
    // surface set is a small shared remainder. Measured, authored images bound:
    //
    //   Stratis    (A3,  8x8 tiles)     706
    //   Takistan   (OA, 484 tiles)      998
    //   Zargabad   (OA, ~480 tiles)     988
    //   Chernarus  (A2, 1024 tiles)   2,076
    //
    // 512 discarded 194 of Stratis's. 1024 cleared both Operation Arrowhead
    // worlds by a hair and then dropped 1,052 of Chernarus's -- leaving 498 of
    // its 1,024 materials incomplete, i.e. base Arma 2's flagship terrain half
    // untextured. 4,096 covers every world in the corpus with room to spare and
    // still sits under the renderer's 8,192 bindless-texture device requirement,
    // which is requested unconditionally and already sized by the object array.
    constexpr uint32_t WGR_TERRAIN_MAX_GROUND_LAYERS = 4096;

    /* Upload the per-land-cell texture index map: a `width` x `height` (= land
     * range per axis) R16Uint texture where each texel's bits 0-14 are the
     * ground-layer index for that land cell (row 0 = cell z=0; index 0 = sea).
     * Bit 15 marks a clamped transition tile: its texture maps exactly once onto
     * the cell with edges extended (GL33's ClampU|ClampV) instead of tiling.
     * `indices` is width*height uint16s. */
    WGR_API void wgr_terrain_set_index_map(WgrRenderer* renderer, uint32_t width, uint32_t height,
                                           const uint16_t* indices);

    /* One authored terrain material. Slots are indices into the ground binding
     * array: satellite, LCA selector mask, then the surface stack by stage
     * position. `surface_count` is 0 for legacy single-texture terrain. */
    struct WgrTerrainUv
    {
        float u[4];
        float v[4];
    };
/* Surface slots in a terrain material's stage stack. Arma 2 / OA's TerrainX
 * fills all six (Stage4..Stage14); Arma 3's TerrainSNX fills five and spends
 * Stage14 on `tile_normal`. Slots are addressed by stage position, so an unused
 * slot is a hole that keeps its index -- the LCA mask selects by slot. */
#define WGR_TERRAIN_SURFACE_SLOTS 6

    struct WgrTerrainMaterial
    {
        // Original per-cell colour texture. This remains the default renderer
        // route; `satellite` below is the authored Stage0 image.
        uint32_t legacy;
        uint32_t satellite;
        uint32_t mask;
        /* The whole tile's normal, sharing the satellite's frame (Arma 3
         * Stage14; Arma 2 has none). 0 = none, and slot 0 is the bindless white
         * fallback, so it is tested rather than sampled blind. */
        uint32_t tile_normal;
        /* Surface colours by slot, 0 where the source leaves the stage empty. */
        uint32_t surfaces[WGR_TERRAIN_SURFACE_SLOTS];
        /* Tangent-space normals parallel to `surfaces`: the `_nopx` map authored
         * on the odd stage before each colour. 0 = none. */
        uint32_t surface_normals[WGR_TERRAIN_SURFACE_SLOTS];
        // One past the highest slot the source names; holes below it are kept.
        uint32_t surface_count;
        // Bit N selects worldPos for UV N; clear selects the terrain mesh's
        // local `tex` coordinates. Slots: satellite, mask, then surfaces 0..5.
        uint32_t uv_source_mask;
        uint32_t _pad0;
        uint32_t _pad1;
        WgrTerrainUv satellite_uv;
        WgrTerrainUv mask_uv;
        WgrTerrainUv surface_uvs[WGR_TERRAIN_SURFACE_SLOTS];
    };
    /* 20 header words, then the satellite/mask UVs and six per-slot UVs of two
     * vec4 each. The WGSL `TerrainMaterial` in terrain.wgsl must be kept to the
     * same 336 bytes: a skew here renders as the wrong ground, not as an error. */
    static_assert(sizeof(WgrTerrainUv) == 32, "WgrTerrainUv layout must match the Rust #[repr(C)] struct");
    static_assert(sizeof(WgrTerrainMaterial) == 336,
                  "WgrTerrainMaterial layout must match the Rust #[repr(C)] struct");
    WGR_API void wgr_terrain_set_materials(WgrRenderer* renderer, const WgrTerrainMaterial* materials,
                                           uint32_t count);

    /* Upload the per-grid-point ground UV jitter map: a `width` x `height`
     * (= land range per axis) Rg8Snorm texture holding each land grid point's
     * random texture UV offset (Landscape::_random, at most +-0.7). The fragment
     * shader interpolates it bilinearly across each cell and adds it to the
     * ground tiling UV, replicating GL33's per-vertex jitter. `offsets` is
     * width*height (u, v) int8 pairs (snorm: value / 127). */
    WGR_API void wgr_terrain_set_jitter_map(WgrRenderer* renderer, uint32_t width, uint32_t height,
                                            const int8_t* offsets);

    /* Set the high-frequency detail noise texture tiled over the terrain
     * (OFP's `CfgDetailTextures.detail`) to a wgr_texture_create handle; its
     * alpha channel modulates the blended ground colour (rgb *= 2*detail.a).
     * Handle 0 is ignored (the neutral built-in stand-in stays). */
    WGR_API void wgr_terrain_set_detail_layer(WgrRenderer* renderer, WgrTexture handle);

    /* Upload one GeographyInfo::packed value per land cell. Grass uses the
     * existing authoritative water/road/forest/obstacle classification before
     * attempting any optional artist-authored exclusion masks. */
    WGR_API void wgr_grass_set_geography(WgrRenderer* renderer, uint32_t width, uint32_t height,
                                         const uint32_t* geography);
    WGR_API void wgr_grass_set_params(WgrRenderer* renderer, const WgrGrassParams* params);

    /* GRS-E — upload the photographed grass-tuft texture used by the mid LOD's crossed
     * cards. `rgba` is width*height RGBA8 (the game's own PAA/PAC decoded through
     * DecodePAABuffer). Cutout alpha: the mid fragment shader alpha-tests it. Passing
     * width or height 0 clears it, and the mid ring falls back to procedural ribbons. */
    WGR_API void wgr_grass_set_tuft(WgrRenderer* renderer, uint32_t width, uint32_t height,
                                    const uint8_t* rgba);
    /* Layer-major RGBA8 photo-clump atlas. Each layer is one local grass family. */
    WGR_API void wgr_grass_set_tufts(WgrRenderer* renderer, uint32_t width, uint32_t height, uint32_t layers,
                                     const uint8_t* rgba);
    /* 1 when a photographed clump atlas was uploaded. 0 means the optional assets
     * are absent, and the renderer keeps procedural grass regardless of
     * `use_photo_tuft` -- there is no configuration in which a missing asset is a
     * failure rather than a fallback. */
    WGR_API uint32_t wgr_grass_have_photo_clumps(const WgrRenderer* renderer);

    /* Upload opaque, modern-PNG blade-surface layers for the near grass geometry.
     * `rgba` is layer-major, with `layers` same-sized width*height RGBA8 images.
     * The blade mesh supplies the silhouette, so alpha is ignored and no discard
     * is enabled by this path. */
    WGR_API void wgr_grass_set_blade_atlas(WgrRenderer* renderer, uint32_t width, uint32_t height,
                                           uint32_t layers, const uint8_t* rgba);

    /* The terrain sun-shadow and sky-visibility knobs are pushed through the consolidated
     * WgrRenderParams block (wgr_set_render_params), not their own setters. See below and
     * docs/render-params-consolidation-plan.md. */

    /* Set/refresh the water placement params (see WgrWaterParams). Cheap; called on
     * map load and each frame to update the animated `sea_level`. */
    WGR_API void wgr_water_set_params(WgrRenderer* renderer, const WgrWaterParams* params);

    /* Set per-cascade configuration (0..7). Spectrum initialisation regenerates when spectrum parameters change. */
    WGR_API void wgr_water_set_cascade_config(WgrRenderer* renderer, uint32_t index, const WgrWaterCascadeConfig* config);
    WGR_API void wgr_water_set_interaction_params(WgrRenderer* renderer, const WgrWaterInteractionParams* params);
    WGR_API void wgr_water_submit_interactions(WgrRenderer* renderer, const WgrWaterInteractionEvent* events, uint32_t count);

    /* Render + present one frame. Returns 0 on success (incl. transient skipped
     * frames), negative on error. */
    WGR_API int32_t wgr_render_frame(WgrRenderer* renderer, const WgrFrame* frame);

    /* Debug: read back the current auto-exposure scale (blocking GPU sync; dev panel only). */
    WGR_API float wgr_get_exposure_scale(WgrRenderer* renderer);

    /* WTR-002 — copy the latest completed-frame GPU pass timings into `out_ms`
     * (milliseconds per region, indexed by WgrGpuTimerRegion; -1 = pass never ran /
     * reserved). Non-blocking — values are harvested asynchronously by the renderer
     * each frame. Returns the region count written (min of WGR_GPU_TIMER_REGION_COUNT
     * and out_len), or 0 when the adapter lacks timestamp queries. */
    WGR_API uint32_t wgr_get_gpu_timings(WgrRenderer* renderer, float* out_ms, uint32_t out_len);

    /* GRS-A — latest grass instance counts. Returns 1 on success, 0 when unavailable. */
    WGR_API uint32_t wgr_get_grass_stats(WgrRenderer* renderer, WgrGrassStats* out);

    /* Push the consolidated ImGui-tweakable render params (tonemap, exposure, sky look,
     * terrain sun-shadow, sky-visibility) in one block. The two terrain setters are diffed
     * renderer-side against the last block, so a per-frame push doesn't thrash the sweep/scan.
     * See docs/render-params-consolidation-plan.md. */
    WGR_API void wgr_set_render_params(WgrRenderer* renderer, const WgrRenderParams* params);

    /* CLD-020: strength of the cloud shadow the deck casts on terrain, objects, grass and water.
       0 = off (every surface reads fully lit); 1 = the full computed transmittance. Its own entry
       point rather than a field in WgrSkyLook, because growing that struct changes a size the ABI
       handshake checks and this is one float. */
    /* How much wider the planar water reflection's frustum is than the screen's. 1 = the old
       behaviour, where a grazing reflection ran off the edge of the reflection target and the
       reflected clouds ended in a visible line. Higher covers more angle at lower resolution. */
    WGR_API void wgr_set_planar_reflection_pad(WgrRenderer* renderer, float pad);

    /* Brightness of the procedural star field. 0 = none. Gated to night by sun altitude in the
       shader, so it never affects a daytime sky. */
    WGR_API void wgr_set_lens_flare(WgrRenderer* renderer, float intensity);
    WGR_API void wgr_set_star_intensity(WgrRenderer* renderer, float intensity);

    WGR_API void wgr_set_cloud_shadow_strength(WgrRenderer* renderer, float strength);

    /* Push the per-frame sky runtime (celestial dir/phase, night factor, fog colour, camera
     * altitude, fog range) — the runtime half of the sky UBO; the look half comes from
     * wgr_set_render_params. */
    WGR_API void wgr_set_sky_runtime(WgrRenderer* renderer, const WgrSkyRuntime* params);

    /* Read one cascade layer of the shadow depth map back as row-major floats
     * (row 0 = the top texture row). Returns the map resolution (side length),
     * or 0 when no map has been rendered / `layer` is out of range / `out_len`
     * is smaller than resolution². Debug/test hook (DumpShadowMap). */
    WGR_API uint32_t wgr_shadow_map_read(WgrRenderer* renderer, uint32_t layer, float* out, uint32_t out_len);

    /* Render `vert_count` triangle-list vertices (xyz, 3 floats each) through
     * the shadow depth pipeline with the given column-major light
     * view-projection into a scratch res*res depth map, and read it back into
     * `out` (res*res floats, row 0 = top). Returns 1 on success. Debug/test
     * hook (ShadowDepthProbe: CPU-reference parity for the depth path). */
    WGR_API int32_t wgr_shadow_depth_probe(WgrRenderer* renderer, const float* light_vp16, const float* tri_xyz,
                                           uint32_t vert_count, uint32_t res, float* out);

} // extern "C"

#endif // WGPU_RENDERER_HPP
