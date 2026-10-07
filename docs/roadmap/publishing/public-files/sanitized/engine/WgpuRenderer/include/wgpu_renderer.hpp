/*
 * wgpu_renderer.hpp — C++ interface to the WGPU graphics backend.
 *
 * While this file contains C++ features, the actual exported symbols are all C ABI (extern "C"),
 * which the Rust side can implement using #[no_mangle] and #[repr(C)].
 *
 * THREADING CONTRACT: every function in this header forms an exclusive reference to
 * the renderer (except the wgr_uploader_* borrow, documented at its declaration), and
 * the Rust side spawns no threads of its own and guards nothing against concurrent
 * entry. So the rule is ONE THREAD AT A TIME, IN FRAME ORDER -- not "main thread
 * only": with the render thread on (WGR_RENDER_THREAD, default), wgr_render_frame and
 * the queued resource/instance/settings drains run on the worker, and the producer
 * may call in only while the worker holds no frame. EngineWgpu enforces that window
 * (WaitRenderIdle at InitDraw, closed at publish; REN-THR-012) and exposes it as
 * ProducerMayTouchRenderer(), which the texture paths check. Texture destruction is
 * immediate (no epoch -- see gfx3d/pool.rs for the one resource class that HAS one),
 * which stays safe only because of that ordering. Phase 4.2 of the post-DLSS roadmap
 * makes the Rust side check it too (design notes, row 3.10).
 */
#ifndef WGPU_RENDERER_HPP
#define WGPU_RENDERER_HPP

#include <cstdint>
#include <cstddef>

// Increment when an incompatible change is made to the public C ABI.  The
// engine checks this before creating a renderer so a matched build fails with a
// useful diagnostic instead of proceeding with incompatible assumptions.
//
// Bumping this by hand was never reliable: growing a shared struct is a silent,
// local-looking edit (add a lane, move one static_assert) and nobody remembers
// that it also invalidates every already-deployed binary of the OTHER language.
// WgrAbiCheck::layout_hash (see WgrLayoutHash() at the bottom of this file) now
// makes that automatic — any size change to any shared struct changes the hash
// and the handshake refuses the pair — so this number only has to move for
// changes the hash cannot see (an added/removed entry point, a semantic change
// to an existing one).
#define WGR_ABI_VERSION 22u // Terrain-following fog and coherent density modulation

// Required capabilities are negotiated as a bit set inside WgrAbiCheck. A
// newer engine can reject an older renderer DLL even if the shared layouts
// still happen to match.
#define WGR_ABI_FEATURE_BUILD_ID 0x00000001u
#define WGR_ABI_FEATURE_SAFE_DIAGNOSTICS 0x00000002u
#define WGR_ABI_FEATURE_RUNTIME_CAPABILITIES 0x00000004u
#define WGR_ABI_FEATURE_ANALYTIC_GLOW 0x00000008u

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
    WGR_TEXTURE_BC3 = 3, // DXT5
    /* RFG-047 -- the DXGI-era block formats. No PAA/PAC magic word exists for these;
     * they arrive only from a DDS/EDDS source (Enfusion content) and are handed to the
     * GPU compressed instead of being decoded to 32-bit. Same TEXTURE_COMPRESSION_BC
     * feature bit as BC1-BC3, so the same adapter gate covers them. */
    WGR_TEXTURE_BC4 = 4, /* one channel, 8 bytes / 4x4 block */
    WGR_TEXTURE_BC5 = 5, /* two channels, 16 bytes / 4x4 block; samples as (r, g, 0, 1) */
    WGR_TEXTURE_BC7 = 6  /* RGBA, 16 bytes / 4x4 block */
};

enum WgrBlend : uint32_t
{
    WGR_BLEND_OPAQUE = 0,
    WGR_BLEND_ALPHA = 1,
    WGR_BLEND_ADDITIVE = 2,
    WGR_BLEND_SHADOW = 3,
    WGR_BLEND_RELIEF_MULTIPLY = 4 // receiver multiplier + fog airlight compensation; keeps alpha
};

/* Depth-buffer interaction for a 2D/screen batch. Plain 2D and depth-disabled
 * meshes (sky: NoZBuf) use NONE; transparent / NoZWrite meshes test but don't
 * write; opaque pre-projected meshes (the laptop) test and write. */
enum WgrDepthMode : uint32_t
{
    WGR_DEPTH_NONE = 0,      // no test, no write
    WGR_DEPTH_TEST = 1,      // test (LessEqual), no write
    WGR_DEPTH_TEST_WRITE = 2, // test (LessEqual) + write
    /* SMK-037 SOFT PARTICLES. Same depth state as WGR_DEPTH_TEST -- test, never write --
       but the fragment stage additionally fades the sprite's alpha by how close its own
       depth is to the scene depth behind it, so a billboard meets a wall or the ground as a
       soft intersection instead of the razor edge the depth test alone produces.
       2D batches only. Submitted by EngineWgpu::DrawDecal for cloudlets, and only while
       wgr_set_soft_particles has been called with enabled != 0 -- with the path off the
       renderer treats this exactly as WGR_DEPTH_TEST, so it is always safe to send.
       Adds no struct and no size: WgrDraw2DBatch.depth is a plain uint32. */
    WGR_DEPTH_TEST_SOFT = 3
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
    float fog; /* 0..1 fog; negative = HDR gain; 2 = analytic halo, 3 = HDR emitter core. */
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
    /* Second UV set (`tex1`), or a copy of uv when the shape has none. Read by the
     * GPU-driven VS at @location(7) for Multi's mask/macro/AS stages (uvSource tex1). */
    WgrVec2 uv1;
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
    WGR_DRAW3D_ZBIAS_MASK = 0x300,

    /* DZ-003: this draw's material declared itself reflective (its shader family is
     * CalmWater, DayZ's water), so the fragment shader adds a Fresnel-weighted reflection
     * of the sky env map. Clear for every other material, which is all of them but water. */
    WGR_DRAW3D_REFLECTIVE = 2,

    /* This draw's material is a recognised light FIXTURE (lamp lens, runway/warning light):
     * its emissive is source radiance and stays on at night. Every other material's
     * `emmisive` lane is legacy fixed-function brightness compensation (Arma 2 vegetation
     * ships 2.6, Arma 3 tree crowns 1.0) and the shader fades it out with the sun, or the
     * whole forest self-illuminates at midnight. Same meaning as
     * WGR_MODEL_SECTION_NIGHT_EMITTER on the retained path. */
    WGR_DRAW3D_NIGHT_EMITTER = 4,

    /* MAT-051: this alpha-blended draw is classic GLASS (continuous-partial-alpha texture
     * over a fully opaque material -- a canopy pane, a periscope). The fragment shader
     * floors its output alpha so the pane still reads at normal incidence, where the
     * Fresnel sky term (WGR_DRAW3D_REFLECTIVE, set alongside) goes to ~F0 and an OFP-era
     * 11%-alpha pane would otherwise vanish from the inside view. */
    WGR_DRAW3D_GLASS = 8,
    /* MAT-052 second half: this draw is part of the first-person cockpit (the engine's
     * PassKindHint::Cockpit scope -- the camera vehicle's interior LOD. NOT the soldier's
     * own body or held weapon: those are PassKindHint::FirstPersonBody, drawn in the same
     * late pass but under the open sky, and damping them turned the sleeve dark and the
     * rifle's grazing-angle faces into a black patch via the view-facing normal flip.)
     * The lit shader takes the FLAT scene ambient for these instead of the
     * sky-dome irradiance: a cockpit interior is enclosed by the vehicle body, and the
     * outdoor sky term washed every interior plate to near-sky brightness -- which against
     * the sky reads as a transparent cockpit (T72 interior: fine looking down at terrain,
     * "transparent" against sky; UH-60 canopy beams likewise). GL33's fixed-function look
     * for the same surfaces IS the flat scene ambient, so this matches it. */
    WGR_DRAW3D_COCKPIT = 16,
    WGR_DRAW3D_MONITOR = 32,
    /* Enfusion `Cull none`: single-sided-authored surface drawn double-sided
     * (wire mesh, glass panes). The per-draw pipeline drops backface culling
     * for the draw; every other draw keeps it. */
    WGR_DRAW3D_DOUBLE_SIDED = 64,
    WGR_DRAW3D_NORMAL_RG = 128, // BC5 / compressed Enfusion normal XY, not NOHQ alpha/green
    WGR_DRAW3D_NATIVE_CAVITY = 1024, // 256/512 belong to Z bias
    /* Explicit producer proof: intact world-owner rigid source mesh, not a Person/proxy/
     * animated/local/road surface. Shader still checks the material and outward side.
     * New meaning in an unused flag bit, no struct/layout change. Old producers default dry. */
    WGR_DRAW3D_SNOW_RECEIVER = 2048,
    /* Static owned rigid ground candidate; fragment must prove bare-terrain proximity
     * and physical rain cover. No change to alpha, geometry or struct layout. */
    WGR_DRAW3D_GROUND_RECEIVER = 4096,
    /* MAT-053: an object's translucent section drawn in the back-to-front blend pass (MAT-048:
     * depth-test only). The world depth prepass lays depth for this draw's FULLY SOLID texels
     * (texture alpha >= 0.98) so a mostly-solid atlas that classified Blend (a floor, a camo net)
     * still occludes what is behind it; the colour pass keeps blending every texel. */
    WGR_DRAW3D_BLEND_SECTION = 8192,
    WGR_DRAW3D_BOOT_RELIEF = 16384 // exact authored footstep Mark on deformable soil, opt-in
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
    /* Shared lane (WgrDraw3D is ABI-frozen; size and layout hash unchanged):
     *   bits 0-7   material-debug view index (dev panel Materials tab)
     *   bit  8     material-debug invert-normal-Y
     *   bits 16-31 REN-TEMP-001H stable object id for rigid motion vectors
     *              (renderer keeps id -> previous world; 0 = no identity). */
    uint32_t misc;
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
    /* Raw local-light specular RGB, independent of sun radiance; w reserved.
     * Appended ABI extension: 272 -> 288 bytes, covered by the layout handshake. */
    WgrVec4 mat_local_specular;
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
    /* Per-section flags (WgrModelSectionFlags). Was dead padding; the reflective bit
     * rides here rather than in WgrModelMaterial, whose size is pinned by a static_assert. */
    uint32_t flags;
};

enum WgrModelSectionFlags : uint32_t
{
    /* DZ-003: this section's material declared itself reflective (shader family
     * CalmWater). Same meaning as WGR_DRAW3D_REFLECTIVE on the per-draw path. */
    WGR_MODEL_SECTION_REFLECTIVE = 1u,
    /* Recognised light fixture: emissive is real radiance and survives the night. See
     * WGR_DRAW3D_NIGHT_EMITTER. */
    WGR_MODEL_SECTION_NIGHT_EMITTER = 2u,
    /* Multi's mask stage is authored on uvSource "tex1": sample it with WgrMeshVertex::uv1. */
    WGR_MODEL_SECTION_MASK_UV1 = 4u,
    /* RFG-047: this section's normal map packs X in RED, not in alpha.
     *
     * `decode_nohq` reads (alpha, green) -- the DXT5nm convention of every `_nohq.paa` in
     * the OFP/Arma corpus. Enfusion's `_NMO` maps put x/y in (r, g); BC5 is a two-channel
     * format and can do nothing else, and Reforger's BC7 `_NMO` uses the remaining two
     * channels for metalness and occlusion, so reading X out of alpha reads OCCLUSION.
     *
     * Both compressed and decoded native DDS retain RG normal XY and the independent B/A
     * material channels. The bit keeps shader decoding identical in both upload modes. */
    WGR_MODEL_SECTION_NORMAL_RG = 8u,
    WGR_MODEL_SECTION_NATIVE_CAVITY = 16u,
    /* The global normal map (`GlobalNMOMap`, `UVSrcGlobNormal "UV set 2"`) is
     * painted in the object's own unwrap: sample it with uv1, unscaled. */
    WGR_MODEL_SECTION_GLOBALNMO_UV1 = 32u,
    /* Experimental native-only PBR channel preview. Never set for legacy,
     * converted, layered, cutout or non-original texture sources. */
    WGR_MODEL_SECTION_NATIVE_PBR_PREVIEW = 64u,
    /* Section intersects a known procedural clock selection or lacks its proof.
     * Snow colour/normal must both refuse it; existing alpha and draw ownership remain. */
    WGR_MODEL_SECTION_NO_OBJECT_SNOW = 128u
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
    /* MAT-048: the layers' OWN normal maps (RVMAT Stage12/13/14, paired with Stage1/2/3's
     * colours), 0 where the material names none. Without these the albedo blended up to four
     * layers while the normal stayed layer 0's for the whole surface -- plaster colour with
     * brick bumps on every wall whose mask selects a further layer. Each is sampled at its own
     * layer's UV transform (layer_uv[i + 1]), the same one its colour uses. */
    WgrTexture layer_normal_texture_id[3];
    /* Per layer, including layer 0 at index 0: (scale_u, scale_v, offset_u, offset_v).
     * RVMAT authors these transforms diagonally, so a full 3x3 is not carried. */
    float layer_uv[4][4];
    /* RFG-072: per layer, layer 0 at index 0: the colour the layer's tile is worn in, a
     * LINEAR multiplier (Enfusion's `Color_N`; identity for every other material). The
     * shared one-metre library tiles are colourless by design -- 436 tiles serve 2,889 of
     * Everon's structure materials -- and this is where each wall becomes its own. */
    float layer_colour[4][4];
    /* Enfusion's per-material normal-map intensity (`NormalPower`): a multiplier on
     * the decoded tangent-space XY, renormalised by the shader. 1.0 when the
     * source names none (every legacy material), so the default is a no-op. */
    float normal_power = 1.0f;
    /* Crown self-occlusion volume (`GeometryAOCenter/Height/Width/Intensity`):
     * xyz = trunk-axis centre in MODEL space, w = intensity (0 = off); second
     * lane x = cylinder height (reserved: the fitted falloff is radial-only),
     * y = cylinder radius. Zero lanes keep every material that names none
     * exactly where it was. */
    WgrVec4 crown_ao_p0;
    WgrVec4 crown_ao_p1;
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
    WGR_INSTANCE_CANOPY_FOREST = 4,
    WGR_INSTANCE_COHERENT_TREE_WIND = 8,
    WGR_INSTANCE_FAR_AUTHORED = 16,
    /* Private visible-page pilot: only the main camera's prepass/color culls
     * may emit this instance. The original object remains in other passes. */
    WGR_INSTANCE_MAIN_CAMERA_ONLY = 32,
    /* Original object complement for a private main-camera page: exclude this
     * instance only when the cull carries positive main-camera authority.
     * Unknown and other views retain the original. Both view bits together
     * are invalid and conservatively exclude the instance from every view. */
    WGR_INSTANCE_OTHER_VIEWS_ONLY = 64,
    /* Actual rigid owner rain/snow receiver. Child proxies and far-authoring DTOs do not
     * inherit the parent's authority. Independent of private page-view bits 32/64. */
    WGR_INSTANCE_SNOW_RECEIVER = 128,
    WGR_INSTANCE_GROUND_RECEIVER = 256
};

inline constexpr uint32_t WGR_INSTANCE_SURFACE_RECEIVERS =
    WGR_INSTANCE_SNOW_RECEIVER | WGR_INSTANCE_GROUND_RECEIVER;

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

/* Individual primary static tree crown proof (modes 0/2 only, never forest mode1):
 * conform0.y/z = loaded owner model minY/inverseHeight, conform2.w = positive
 * owner/geometry proof. conform1.x = bounded actual external crown roof-ray
 * exposure. Zero proof/exposure is pending/unknown; proxies/far DTOs do not
 * inherit it. These reserved lanes preserve the public struct layout. */

static_assert(sizeof(WgrModelSection) == 24, "WgrModelSection must match Rust");
static_assert(sizeof(WgrModelMaterial) == 320, "WgrModelMaterial must match Rust");
static_assert(sizeof(WgrModelLod) == 16, "WgrModelLod must match Rust");
static_assert(sizeof(WgrInstance) == 144, "WgrInstance must match Rust");

/* Additive, read-only owner CPU fact for one literal retained instance handle.
 * Absent is usable as a removal ACK only after the caller observed Present for
 * the same handle. The eight-bit handle generation can alias after 256 reuses;
 * long-lived authority must also bind the caller's own instance birth. */
enum WgrInstanceCpuFactStatus : uint32_t {
    WGR_INSTANCE_CPU_FACT_UNSUPPORTED = 0,
    WGR_INSTANCE_CPU_FACT_PRESENT = 1,
    WGR_INSTANCE_CPU_FACT_ABSENT = 2,
    WGR_INSTANCE_CPU_FACT_INVALID = 3
};
struct WgrInstanceCpuFact {
    uint32_t version;          /* 1 */
    uint32_t bytes;            /* sizeof(WgrInstanceCpuFact) */
    uint32_t status;           /* WgrInstanceCpuFactStatus */
    uint32_t queried_handle;   /* literal input, including for Absent/Invalid */
    uint64_t instance_epoch;   /* current retained content epoch, not GPU completion */
    WgrInstance row;           /* actual CPU row if Present; zero otherwise */
    uint32_t resolved_slot;    /* actual raw slot if Present; UINT32_MAX otherwise */
    uint32_t reserved;
};
static_assert(sizeof(WgrInstanceCpuFact) == 176, "WgrInstanceCpuFact ABI mismatch");

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
    /* NV-001 night vision, applied in the tonemap pass rather than as a light colour.
     * The legacy filter (Engine::_accomodateEye = Color(0, 8, 0)) multiplied the SUN, which
     * at night contributes nothing -- and on the sky-lit path it was overwritten a few lines
     * later anyway. An image intensifier amplifies the IMAGE. 0 = off. */
    float nv_strength;  /* 0 = off, 1 = full goggles */
    float nv_gain;      /* linear amplification of the scene before the curve */
    float nv_noise;     /* sensor grain amount */
    float nv_vignette;  /* tube edge darkening, 0 = none */
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
    /* Moon disc (RUNTIME). Appended at the END so no existing lane moves.
     * x = moon angular RADIUS (rad, real, ~0.0045 and varying +/-6% over the month),
     * y = illuminated fraction (0 new .. 1 full),
     * z = disc radiance scale (irradiance; the shader divides by the solid angle),
     * w = draw the disc (0 = skip entirely). */
    WgrVec4 moon_params;
    /* xyz = unit direction TO THE SUN AS SEEN FROM THE MOON. Deliberately NOT sun_dir:
     * that one may be the legacy sun and is temporally smoothed, and the terminator's TILT
     * is the thing people notice when it is wrong. w = earthshine reflectance floor on the
     * dark side (0 = none). */
    WgrVec4 moon_sun;
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
    WgrVec4 night_params;  /* x = full-day sun_dir.y, y = full-night sun_dir.y, z = night intensity,
                            * w = REN-SKY-004: 1 = the planar water reflection uses its own cheap
                            * cloud march, 0 = the sky's own step count. LOOK struct only. */
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
    WgrVec4 moon_dir;  /* xyz = unit dir TO the moon; w = ILLUMINATED FRACTION (0 new .. 1 full).
                        * Was the legacy synodic-age float, which only ever indexed moon.p3d's
                        * texture animation — and that dome is suppressed on this backend. */
    WgrVec4 fog_color; /* xyz = scene fog colour; w = fog far-range (m) */
    WgrVec4 misc;      /* x = night factor (0..1), y = camera altitude ASL (m), z/w = wind offset */
    WgrVec4 cloud_evolve; /* x = shape, y = detail, z = weather drift (m, CPU-wrapped); w = pad */
    /* Moon disc. See WgrSky::moon_params / moon_sun for the field meanings — these are the
     * per-frame source the renderer folds into those UBO lanes. */
    WgrVec4 moon_params;
    WgrVec4 moon_sun;
    WgrVec4 layer_fog_weather; // captured fog, landscape-ready, camera-ready, reserved
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
    /* FOLIAGE-DUSK: exponent applied to `daylight` for the two foliage-only ambient terms
     * (1.0 = the legacy curve exactly; < 1 holds the golden hour up and still hits 0 at
     * night). Was a pad lane; see Poseidon::FoliageDuskCurve() and shading.wgsl. */
    float dusk_curve;
    /* VEG-SWAY — geometric wind on vegetation MODELS (trees/bushes/forest patches). Before this,
     * the wgpu path animated grass and nothing else: neither object vertex shader had any
     * time-varying term, so a forest stood dead still in a gale. These eight floats extend the
     * Frame UBO by exactly two vec4 lanes (frame.foliaged / frame.foliagee) — the bind size is
     * computed from sizeof(WgrFoliage), so extending here is enough on the Rust side.
     *
     * The wind vector is NOT re-derived in the renderer: it is the same World/Weather/WindModel
     * sample the grass field and the cloud deck use, pushed from EngineWgpu so all three lean
     * the same way. */
    float sway_strength;  /* master amplitude, metres of tip travel at 1 m above the pivot; 0 = OFF */
    float sway_speed;     /* rate multiplier on the trunk oscillation (1 = authored) */
    float wind_dir_x;     /* unit vector the wind travels TOWARD (world +X) */
    float wind_dir_z;     /* unit vector the wind travels TOWARD (world +Z) */
    float sway_time;      /* engine clock in seconds — Glob.time, so a pause freezes the canopy */
    float wind_gust;      /* WindSample::gustFraction, roughly [-1, 1] */
    float sway_leaf;      /* extra high-frequency flutter on cutout (leaf) sections only */
    float sway_stiffness; /* height exponent: 1 = linear lean, 2 = stiff trunk / loose crown */
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
    uint32_t probe;      /* REQUEST COUNTER: each increment makes the renderer log the map's
                          * occluder coverage on its next frame. The startup one-shot fires ~2 s
                          * in and never again, which only ever measured the loading screen. */
};

/* REN-GI-001 -- the irradiance probe volume (hybrid GI, stage 1). Mirrors the Rust WgrGi
 * #[repr(C)]; 68 bytes, no padding. Pushed every frame inside WgrRenderParams. */
struct WgrGi
{
    uint32_t enabled;        /* 0 = the analytic sky-dome ambient only (pre-GI behaviour) */
    uint32_t basis;          /* REN-GI-009: probe radiance basis. 0 = six-face ambient cube,
                              * 1 = first-order SH (a measured dead end, REN-GI-008),
                              * 2 = second-order SH (the default). Was a reserved debug word. */
    uint32_t rays;           /* rays per probe update (8..64) */
    uint32_t rsm_samples;    /* REN-GI-002: sun-proxy gather samples per probe (0 = off) */
    float    weight;         /* blend of the probe term over the sky-dome ambient, 0..1 */
    float    interior_mix;   /* how much of the per-pixel interior-sky AO still applies (0..1) */
    float    spacing;        /* probe spacing, metres */
    float    hysteresis;     /* per-update blend toward the new integral, 0.05..1 */
    float    ground_albedo[3];
    float    ground_gain;    /* scale on the sun bounce off the ground */
    float    wall_albedo;    /* neutral albedo for roof/wall hits from the dome maps */
    float    ray_length;     /* metres a probe ray looks for terrain */
    float    rsm_radius;     /* REN-GI-002: gather radius around the probe, metres */
    float    rsm_gain;       /* REN-GI-002: scale on the gathered sun bounce */
    float    indoor_sky;     /* REN-GI-010: how much a probe's own sky occlusion attenuates its
                              * LATERAL sky rays. 0 = the pre-fix integration exactly (nothing
                              * tested a sideways ray against a wall, so an indoor probe was
                              * nearly as bright as one outdoors); 1 = full. */
};

/* Picture Mode depth of field. Mirrors the Rust WgrDepthOfField #[repr(C)] and feeds dof.wgsl.
 * `near_plane` is here despite not being a look setting: the shader turns reversed-Z depth into
 * metres with `near / depth`, and the renderer has no other access to the projection. */
struct WgrDepthOfField
{
    uint32_t enabled;         /* 0 = the pass does not run at all and costs nothing */
    float focus_distance;     /* metres */
    float focus_range;        /* metres, half-width of the fully sharp band */
    float max_blur_pixels;    /* widest circle of confusion at 1080p, scaled to the real height */
    float background_scale;   /* blur behind the focal plane */
    float foreground_scale;   /* blur in front of it -- far more intrusive, hence separate */
    float transition;         /* 1/metres; how quickly the blur opens past the sharp band */
    float near_plane;         /* camera near plane, metres */
    float debug_view;         /* 0 = normal, 1 = circle of confusion, 2 = raw view distance */
    float sample_count;       /* samples per pixel; the entire cost of the pass */
    float bokeh_boost;        /* how strongly highlights outweigh their neighbours */
    float bokeh_threshold;    /* linear HDR luminance that counts as a highlight */
    float aperture_blades;    /* 0 = round, 5..9 = polygonal iris */
};

/* Every imgui-tweakable render parameter that crosses the FFI as a setter, in one block.
 * Append future look knobs here; do not add new FFI setters. */
struct WgrLayeredFog
{
    WgrVec4 control; // enabled, scattering albedo, HG anisotropy, reserved
    WgrVec4 layers[2]; // base/top ASL metres, linear boundary feather metres, extinction 1/m
    WgrVec4 terrain; // follow, reference ASL, valley strength, valley radius metres
    WgrVec4 patch; // strength, horizontal scale metres, vertical scale metres, coverage feather metres
};
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
    WgrDepthOfField     depth_of_field;
    WgrGi               gi;
    WgrLayeredFog       layered_fog;
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
    WgrVec4 cam_fwd;       // xyz = camera forward; w = contact filter (0 fixed, 1 full, 2 budget)
    WgrVec4 sun_dir;       // xyz = sun travel direction; w = tan(sun angular radius)
    /* LGT-010 local-light shadows. Kept SEPARATE from cascade_vp rather than extending it:
     * the cascades are the sun's, orthographic and selected by distance, while these are one
     * perspective view per shadowing spot selected by the light's own index. Sharing an array
     * would save 256 bytes and cost every reader the question "which kind is layer 5". */
    WgrMat4 local_vp[24];  // LGT-015: camera-relative local view-projections; a point light owns 6
    WgrVec4 local_ctl;     // {count, darkness, texel_size (1/res), first_layer}
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
     * every frame, unlike the shadow block's sun_dir.
     * w = SUN-ONLY daylight factor, 0 at night .. 1 by day (the moon is not in it,
     * unlike sun_diffuse): what the shaders key night-only behaviour on. */
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
    /* Bit i set = cascade i can receive grass-shadow fragments, so the grass blades are worth
     * submitting into it. Grass casts from the NEAR ring only (the mid ring casts solely on the
     * photo-tuft path), so a cascade whose NEAR plane already lies beyond the grass reach cannot
     * receive a single grass fragment -- yet the blades were being transformed for it anyway.
     * Measured on perf_abel @1920x1080: four cascades cost 4.443 ms with grass and 0.121 ms
     * without, and the cost is flat in shadow-map resolution and in caster triangle count but
     * linear in cascade COUNT. That is vertex-bound instancing being clipped away.
     * Conservative by construction: a bit is cleared only when the cascade is a frustum tier
     * whose near distance is provably past the reach. 0xF (all set) reproduces the old
     * behaviour exactly. Occupies what was a pad word, so the ABI is unchanged. */
    uint32_t grass_cascade_mask;
    WgrMat4 light_vp[4]; // camera-relative light view-projections (0..1 NDC z)
    // Camera world position: casters are camera-relative, so the depth shader adds
    // this back to reconstruct absolute world xz for surface_y (terrain conform).
    WgrVec4 cam_pos;
    /* LGT-010: shadow views for LOCAL lights, rendered into layers `count .. count+n-1` of
     * the same depth array the cascades use. A separate family rather than more cascades,
     * exactly as the interior sky-visibility map is a separate family: they are selected
     * differently and they are a different projection kind. */
    uint32_t local_count;
    /* LGT-026 -- the static-view cache. Bit 0: the engine's cache switch is on (the Lighting
     * tab / POSEIDON_LOCAL_SHADOW_CACHE). Bit 1: re-render every local view THIS frame whatever
     * the keys say -- the force-refresh switch, so a suspected stale tile is one keypress to
     * rule in or out rather than an investigation. Occupies what was a pad word, so the ABI is
     * unchanged; same precedent as grass_cascade_mask above. */
    uint32_t local_flags;
    /* LGT-026 -- bit k set = view k's LIGHT changed since the previous frame: it moved, its
     * cone/range/near changed, or a different light now occupies slot k. This is computed
     * ENGINE-side because that is where the light's parameters exist in absolute world space;
     * the renderer cannot see them, and everything it CAN see (the retained-set epoch, the
     * caster set, the depth target's shape) it adds itself. */
    uint32_t local_dirty_mask;
    uint32_t local_pad[1];
    WgrMat4 local_vp[24];
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
    /* RFG-065: master weight for the NATIVE Enfusion ground material (the `.emat`'s own
     * ScaleUV tiling and its middle-distance map). 0 = the legacy one-image-per-50-m-cell
     * route, byte for byte; 1 = the authored surface. Zero on every world that is not a
     * natively loaded Reforger one, because no material there sets `enfusion`. */
    float enfusion_ground;
    /* Alpine snowline (dev weather tab): permanent terrain cover above
     * snowline_height, ramping from first flakes to full cover over
     * snowline_range metres. snowline_height < 0 = off. Independent of the
     * snowfall deposit; pushed per frame like the wet band. */
    float snowline_height;
    float snowline_range;
    float snowline_depth;
    float _pad1; // Cosmetic accumulated rain [0,1] at offset 72 (WGSL rain_wetness); old name preserves initialisers.
    float _pad2; // Effective LIQUID rain [0,1], offset 76 (WGSL rain_strength); same byte layout.
    float _pad3; // Opt-in wet-soil colour diagnostic enum0..4 at offset80; default0, size unchanged.
    float _pad4;
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
    /* FNV-1a over the sizeof of EVERY struct that crosses this boundary — see
     * WgrLayoutHash() at the bottom of the file.
     *
     * The three explicit sizes above only cover the handshake's own arguments and
     * the frame descriptor, so a shared struct could grow on one side and the pair
     * still shook hands. That is not hypothetical: WgrSkyRuntime went from 5 vec4
     * to 7 (80 -> 112 bytes) when the moon disc gained moon_params + moon_sun, and
     * an engine binary from before that change kept passing this check while the
     * renderer read its two new lanes off the end of an 80-byte stack object. The
     * moon then never drew, with a negative illuminated fraction and a 49-degree
     * angular radius in the diagnostic, and nothing anywhere said "mismatched
     * binaries". One field that changes whenever any layout changes closes that. */
    uint32_t layout_hash;
};

/* A transient rotor-wash field. Unlike tracks this is rebuilt from the live
 * helicopter list each frame, so grass springs upright as the aircraft leaves. */
/* One smoke particle for the ground-shadow pass. Matches Rust sky::SmokeShadowBlob (32 bytes). */
struct WgrSmokeShadowBlob
{
    WgrVec4 pos_radius; /* xyz world position, w radius (m) */
    WgrVec4 density;    /* x optical density, y height above ground (m), zw unused */
};

/* SMK-038 VOLUMETRIC SMOKE. Live tuning for the froxel field + raymarch that can stand in
   for the 2001 cloudlet billboards. Its own struct and its own entry point rather than
   fields on an existing one: growing a struct that already crosses the ABI changes a size
   the handshake checks, and this is a whole subsystem's worth of knobs.

   The particles themselves arrive separately, through wgr_set_smoke_volume_blobs, in the
   same WgrSmokeShadowBlob layout the ground-shadow pass already uses -- one collection on
   the engine side feeds both. */
struct WgrSmokeVolumeParams
{
    /* 0 = legacy billboards only and this whole subsystem is inert; 1 = volumetric (the
       engine also stops drawing the billboards it injected); 2 = BOTH, a debug view that
       double-counts the smoke on purpose so the two can be compared in one frame. */
    uint32_t mode;
    float cell_size;      /* metres per froxel; grid is a fixed 128 x 64 x 128 cells */
    uint32_t march_steps; /* samples along the view ray */
    uint32_t sun_steps;   /* samples along the secondary march toward the sun, 0 = flat */
    float sun_step_len;   /* metres, first sun step (they widen geometrically) */
    float density;        /* multiplies the injected optical density */
    float extinction;     /* multiplies sigma_t at march time */
    float albedo;         /* single-scattering albedo; soot is dark, steam is near 1 */
    float anisotropy;     /* Henyey-Greenstein g; >0 forward-scatters, which rims a backlit plume */
    float self_shadow;    /* 0 = no sun march at all, 1 = its full effect */
    float jitter;         /* per-pixel step jitter; trades banding for noise */
    uint32_t scale;       /* 1 = march at render res, 2 = half res, ... */
    WgrVec4 sun_dir;      /* xyz = unit direction TOWARD the sun */
    WgrVec4 sun_radiance; /* rgb */
    WgrVec4 ambient;      /* rgb the smoke picks up from the sky */
};

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
    float density;  /* 0..1 retained fraction; 1..2 adds near/mid candidates */
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
     * 0..31 = that atlas layer (0 = the primary clump / first map clutter class,
     * 1..8 the local families on a loose-card world, up to 31 map classes). */
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
    /* Photo-card coverage: fraction of clutter-grid cells (1.11 m by default,
     * WGR_GRASS_CARD_SPACING) that grow a card, 0..1. With card_coverage_auto it
     * is only the fallback where the map's geography bake did not answer (every
     * OFP world); authored cells keep the map's own clutter density. */
    float card_coverage;
    /* Albedo tints applied after the existing colour work and before lighting.
     * `tint_procedural` covers procedural blades, ribbons and the far proxy;
     * `tint_photo` covers the photographed cards. White RGB = untinted.
     * Unused alpha input lanes: procedural[3] encodes gust waves (negative = old
     * gusts; 1 + local variation 0..1; zero = default full variation), photo[3]
     * is gust front size in metres (10..200; zero = default 45). Alpha does not
     * affect colour, coverage or blending. Struct layout is unchanged. */
    float tint_procedural[4];
    float tint_photo[4];
    /* 1 = authored cells keep the map's own density, card_coverage elsewhere;
     * 0 = card_coverage multiplies everywhere on top of the bake's thinning. */
    float card_coverage_auto;
    /* PROCEDURAL BLADES ONLY (near blades + mid ribbons). Defaults = the old look:
     * self shadow 0, contrast 1, hue variation 0, root shade 0.70. */
    float blade_self_shadow;
    float blade_contrast;
    float blade_hue_variation;
    float blade_root_shade;
    /* Photo card size multiplier; 1.0 = the stock card. */
    float card_scale;
    /* How fast the wind NOISE FIELDS travel, as a multiple of the shipped speeds.
     * 1.0 = the historical look; 0 is read as 1.0 so a zeroed struct is unchanged.
     * The scroll speeds in grass.wgsl are constants unrelated to the wind speed: at
     * the default 6 m/s wind the gust field crosses the world at ~48 m/s and the
     * tip flutter at ~101, i.e. eight and seventeen times the wind driving them,
     * which is what reads as blades shivering instead of leaning and releasing.
     * Occupies one of the two lanes that were `_pad_look`, so the struct layout and
     * every existing field offset are untouched. */
    float wind_scroll;
    /* RFG-090: nonzero = the NEAR ring draws blades even when photo cards are on, and
       the mid ring's cards start at the camera instead of at near_radius. On a natively
       loaded Reforger world the near ring had become the clutter-card ring (40 cards in
       a 1.5 m grid, no blades at all), so the world's own blade atlas (RFG-089) was never
       drawn. 0 = the previous behaviour everywhere. Takes the last `_pad_look` lane, so
       no offset moves. */
    float near_blades_only;
    /* Opaque bounding box per photo layer, (u0, u1, v0, v1) in texture space.
     * Renderer-derived from the uploaded atlas; ignored on input. */
    float photo_layer_bounds[36];
    /* Mean linear colour of each photo layer's covered texels, (r, g, b, 1). The
     * pivot the card contrast turns about, so contrast cannot double as a
     * brightness change. Renderer-derived; ignored on input. */
    float photo_layer_means[36];
    /* RFG-091: the ground colour Reforger's blade layer takes on. A PlantMat blends its
       atlas towards the satellite map (`SatMapLerp 0.8`); the native path has no satellite
       pages yet, so the caller passes the mean linear albedo of the surface behind the
       atlas as the tint and the PlantMat's lerp as the amount. Zero = untinted. */
    float native_tint[3];
    float native_tint_lerp;
};

// --- Water (GPU CDLOD surface) -----------------------------------------------

/* Per-map + per-frame water parameters (a small UBO). `world_origin`/`terrain_grid`/
 * `hm_width`/`hm_height` describe the terrain heightmap the sibling look plan samples
 * for shoreline depth (unused by the flat-plane geometry pass); `sea_level` is the
 * animated global sea height (Landscape::GetSeaLevel) and `time` the wave-animation
 * clock (seconds). The look block (wave_amp..alpha) is the live-tunable water look,
 * edited by the ImGui Water tab; `fade_start`/`fade_end` flatten wave detail with
 * distance (metres) to kill far-field moiré. All refreshed every frame. */
/* WRL-003 — maximum number of bounded water bodies (lakes / river reaches) the water shader can
 * draw in one frame. Metadata scales with this, nothing else does. */
#define WGR_WATER_MAX_BODIES 16u

/* Separate simulation-owned rainfall water; existing ocean ABI is unchanged.
 * domain = origin X/Z, spacing metres, simulation time.
 * control = integer width/height (as exact floats), liquid rain, enabled.
 * Grid RGBA = physical surface height, water depth, flow X/Z. */
struct WgrRainWaterParams { WgrVec4 domain; WgrVec4 control; uint64_t generation; uint64_t reserved; };
/* Fine rainfall publication is paired atomically with its coarse proxies and
 * an actually uploaded bare-terrain receipt. It never falls through to the old
 * grid setter. world_token is opaque CPU identity, not a dereferenceable pointer. */
struct WgrRainWaterSourceKey {
    uint64_t world_token, generation, height_revision;
    uint32_t terrain_range;
    float terrain_spacing;
};
struct WgrRainWaterFineCell {
    WgrVec4 rect; // lower X/Z, side metres, horizontal water head
    WgrVec4 corners; // actual heights 00, X1, Z1, 11; antidiagonal triangles
    WgrVec4 flow_depth; // signed flow X/Z, volume/area (NOT local depth), zero
    uint32_t parent, reserved[3];
};
enum : uint32_t {
    WGR_RAIN_WATER_FINE_BACKEND=1u, WGR_RAIN_WATER_SOURCE_READY=2u,
    WGR_RAIN_WATER_FINE_READY=4u, WGR_RAIN_WATER_FINE_MAX_CELLS=8192u
};
struct WgrRainWaterPublication {
    WgrRainWaterParams coarse;
    WgrRainWaterSourceKey source;
    uint64_t revision;
    uint32_t flags, reserved;
};

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
     * y/z/w were reserved and now carry the WAVE-foam intensity, its deep-water falloff and the
     * planar edge-fade band (see WaterWgpu.cpp). Appended at the struct end; sizeof 256 -> 272. */
    WgrVec4 underwater_gate;
    /* WRL-002 — shared optical model lanes (the Water tab "Optics" section).
     * x: 1 = the surface composites through water_optics (absorption + scattering extinction
     *    derived from the deep swatch and Colour clarity, single-scatter body glow, background
     *    composited in-shader with full coverage); 0 = the previous physical composite (separate
     *    Beer-Lambert curve, seabed-visibility falloff and shallow->deep lerp) for A/B. Only
     *    consulted while look_params.x (physical composite) is on.
     * y: 1 = whitecap sources (persistent foam injection, crest whitecaps, wind-torn spray) are
     *    gated by the latched wind speed (none below ~4 m/s, full at 12 m/s); 0 = crest geometry
     *    alone decides, as before.
     * z: local mean surface level under the camera (the containing body's, else sea level);
     * w: that body's wave scale (1.0 for the sea; 0 = lane not written, use sea_level).
     * Appended at the struct end; sizeof moves 272 -> 288 with the Rust side. */
    WgrVec4 optics_params;
    /* WRL-003 — water-body table, the per-body presentation contract in its minimal form.
     * Three lanes per body: [3i] = centre x, centre z, radius x, radius z (an ellipse; the
     * fragment shader discards outside it), [3i+2] = flow x, flow z, speed m/s (visual
     * advection only), [3i+1] = mean surface level, wave scale,
     * kind (1 lake, 2 river), profile (0 clear lake, 1 turbid lake, 2 slow river, 3 fast
     * shallow river). A node whose `body` lane is i + 1 draws entry i. Unused entries are
     * zero. Immutable per frame; weather and interaction inputs stay in the lanes above.
     * Appended at the struct end; sizeof moves 288 -> 1056 with the Rust side. */
    WgrVec4 bodies[WGR_WATER_MAX_BODIES * 3];
    /* TW-WATER W1 — which water implementation the renderer draws: 0 = Current OP (legacy),
     * 1 = Tidewater Native (the default, Tidewater Water Plan v5). Appended after the body table so no lane the
     * WGSL mirrors of WaterParams read moves; the renderer switches backends at a frame boundary
     * when this changes. WGR_WATER_BACKEND=0|1 in the environment overrides it for the session. */
    uint32_t water_backend;
    uint32_t water_backend_pad[3];
    /* TW-WATER — the Tidewater Native parameter group of the Water tab. Read only by that backend.
     * [0] x = spectrum amplitude scale (1), y = choppiness (0.9), z = swell scale (0.48),
     *     w = swell heading in degrees, world frame (5)
     * [1] x = foam coverage (1), y = foam intensity (1), z = screen-space reflections on (1),
     *     w = reflection strength (1)
     * [2] x = cascade size scale (1 = 733 / 157 / 33.3 / 7.1 m), y = water roughness (0.035),
     *     z = crest subsurface gain (1), w = backscatter (0.035)
     * [3] x = gust amount (1), y = slick amount (1), z = windrow amount (0.3), w = debug view (0)
     * The selector plus this group move sizeof 1056 -> 1136 with the Rust side (the layout hash
     * refuses a stale exe/DLL pair). */
    WgrVec4 tidewater[4];
    /* TW-WATER W3 — the Tidewater shoreline waves (ShoreWaves.js ShoreParams). Read only by that backend.
     * [0] x = shore waves on (1), y = wave period s (9), z = amplitude H/2 m (0.34), w = variation (0.55)
     * [1] x = breaker index gamma (0.78), y = break span (0.13), z = curl (1), w = run-up (1)
     * [2] x = surf-zone turbidity 1/m (0.16), y = shore simulation on (1, W3b), z = wet sand
     *     drying time s (28, ShoreSim dryTime), w = breaker lip opacity (1, W3d; 0 = no lips)
     * The renderer scales the amplitude with the swell scale (tidewater[0].z) relative to its default,
     * so weather and the reference seas move the surf too. sizeof 1136 -> 1184 with the Rust side.
     * (W3g: in the weather / preset modes the C++ side pre-divides the preset surf by that factor,
     * so the net amplitude is the preset's.) */
    WgrVec4 tidewater_shore[3];
    /* TW-WATER W3g — sea conditions (Tidewater's preset table, resolved on the C++ side).
     * x = local wind override m/s (0 = the weather wind, fft_wind_sea.z), y = local fetch km
     * (0 = Tidewater's 120), z = whitecaps (0.5), w = the conditions mode (diagnostic).
     * sizeof 1184 -> 1200 with the Rust side. */
    WgrVec4 tidewater_sea;
    /* TW-WATER W4 — Tidewater effects. x = breaker spray emission gain (1, Breakers `spray`;
     * 0 = no spray), y = spray sprite intensity (1, Spray `intensity`), z = water glow (W3j:
     * scale of the light scattered inside the water, 0; 1 = Tidewater's formula), w unused (0).
     * sizeof 1200 -> 1216 with the Rust side. */
    WgrVec4 tidewater_fx;
    /* TW-WATER W6 — the boats for the Tidewater wake simulation and bow spray (the two nearest
     * the camera; read only by that backend). Per boat i, eight lanes from [8i]:
     * [+0] x, z = hull centre (world), z/w = heading (world XZ of the model forward)
     * [+1] x, z = world velocity, z = thrust 0..1, w = stable id (float-exact, >= 2)
     * [+2] x = half beam m, y = half length m, z = design draft m, w = flags (1 present, 2 driven)
     * [+3] (W6i) x = design waterline height at the hull centre (world y), y = vertical velocity,
     *      z = freeboard m, w = CPU sea height at the propeller
     * [+4] (W6i) the model forward axis xyz (world, unit), w unused
     * [+5] (W6i) the model up axis xyz (world, unit), w unused
     * [+6] (W6i) CPU sea height at bow contacts 0-3, [+7] xy = contacts 4-5 (see
     *      PresentationSnapshot.cpp WakeSprayContactPoint), zw unused
     * sizeof 1216 -> 1472 with the Rust side (W6h: 1312). */
    WgrVec4 tidewater_wake[16];
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
    /* PERF-004 - main-view terrain; in-pass timestamps, append-only ABI slots. */
    WGR_GPU_TIMER_TERRAIN_PREPASS = 31,
    WGR_GPU_TIMER_TERRAIN_COLOR = 32,
    /* PERF-005 - object rendering. Measured on Arma 3's Stratis @1280x720 with a ~20K-object
     * working set: the GPU frame was 19.1 ms of which only ~4.5 ms was attributed (terrain,
     * water, AO, interior sky). These regions exist to name the other ~14.5 ms.
     *
     * Two of them are CONTAINERS that deliberately OVERLAP the leaves inside them. Never add a
     * container to its own leaves - the difference is the point (it is what bounds the cost of
     * the CPU-replayed Draw3D ops, which cannot be timed individually because one region holds
     * only one begin/end pair per frame). WgrGpuTimerIsContainer() below is the authority. */
    WGR_GPU_TIMER_OBJ_CULL_MAIN = 33,       // main-view COUNT/EMIT/SCATTER (prepass + occluder set)
    WGR_GPU_TIMER_OBJ_CULL_SHADOW = 34,     // every active cascade's cull dispatch chain
    WGR_GPU_TIMER_OBJ_CULL_COLOR = 35,      // Hi-Z pyramid build + the colour-pass occlusion cull
    WGR_GPU_TIMER_OBJ_PREPASS_SOLID = 36,   // GPU-driven depth+normal prepass, variant 0 (solid)
    WGR_GPU_TIMER_OBJ_PREPASS_ALPHA = 37,   // ... variant 1 (alpha-cutout foliage)
    WGR_GPU_TIMER_OBJ_COLOR_SOLID = 38,     // GPU-driven colour, variant 0 (solid)
    WGR_GPU_TIMER_OBJ_COLOR_ALPHA = 39,     // ... variant 1 (alpha-cutout foliage)
    WGR_GPU_TIMER_OBJ_PREPASS_SEGMENT = 40, // CONTAINER: the whole depth/normal prepass pass
    WGR_GPU_TIMER_OBJ_COLOR_SEGMENT = 41,   // CONTAINER: the whole 3D colour sub-pass
    /* One region PER CASCADE: a single "shadows" number cannot say which cascade to shrink,
     * and cascade 0 (metres) and cascade 3 (hundreds of metres) have unrelated fixes. */
    WGR_GPU_TIMER_SHADOW_CASCADE_0 = 42,
    WGR_GPU_TIMER_SHADOW_CASCADE_1 = 43,
    WGR_GPU_TIMER_SHADOW_CASCADE_2 = 44,
    WGR_GPU_TIMER_SHADOW_CASCADE_3 = 45,
    /* FAR INSTANCE TIER. The cull is a standalone compute pass, so it brackets on the encoder
     * like the water regions; the draw is an op inside the shared 3D colour sub-pass and so
     * needs in-pass timestamps (it reads "n/a" without them) AND is contained by that pass's
     * container region below. Kept as two rows because they answer different questions: the
     * sweep scales with the world's placement count, the draw with what survived sub-pixel. */
    WGR_GPU_TIMER_FAR_CULL = 46,
    WGR_GPU_TIMER_FAR_DRAW = 47,
    /* Attributing the 3D colour sub-pass. It measured 12.824 ms on perf_abel @1920x1080 while
     * every leaf inside it summed to 3.777, leaving 9.05 ms - 70% of the pass - named by
     * nothing. The unbracketed work was the cloud march, its composite, and the two replays. */
    WGR_GPU_TIMER_CLOUD_MARCH = 48,
    WGR_GPU_TIMER_CLOUD_COMPOSITE = 49,
    /* CONTAINERS. OpsPreWater also encloses terrain + grass. The CPU-replayed direct draws get
     * no region of their own (Plan3dOp::Draw3D is per-draw; a region is one bracket per frame),
     * so read them as OPS_PRE_WATER - TERRAIN_COLOR - GRASS_COLOR. */
    WGR_GPU_TIMER_OPS_PRE_WATER = 50,
    WGR_GPU_TIMER_OPS_POST_WATER = 51,
    /* The post chain and the UI -- everything between the last world draw and present. It was
     * 2.85 ms of a 23.08 ms frame at 1920x1080 (12.3%) with no region at all. All OUTERMOST:
     * these run after the colour sub-pass closes, not inside it. */
    WGR_GPU_TIMER_HDR_RESOLVE = 52,
    WGR_GPU_TIMER_GOD_RAYS = 53,
    WGR_GPU_TIMER_BLOOM = 54,
    WGR_GPU_TIMER_EXPOSURE_ADAPT = 55,
    WGR_GPU_TIMER_TONEMAP_RESOLVE = 56,
    WGR_GPU_TIMER_UI_2D = 57,
    /* Atmosphere (PERF-010): fixed-size per-frame work that had no region. */
    WGR_GPU_TIMER_SKY_LUTS = 58,
    WGR_GPU_TIMER_SKY_FROXEL = 59,
    WGR_GPU_TIMER_SKY_ENV = 60,
    WGR_GPU_TIMER_SKY_SH = 61,
    WGR_GPU_TIMER_SKY_DRAW = 62,
    /* Worker pacing (PERF-021): CPU-only rows, GPU column n/a. Outermost, and NOT part of
       FRAME_TOTAL -- they are the worker's swapchain wait and its present call. */
    WGR_GPU_TIMER_WORKER_ACQUIRE = 63,
    WGR_GPU_TIMER_WORKER_PRESENT = 64,
    WGR_GPU_TIMER_WORKER_SETUP = 65,
    WGR_GPU_TIMER_WORKER_SUBMIT = 66,
    WGR_GPU_TIMER_WORKER_HARVEST = 67,
    WGR_GPU_TIMER_GI_PROBES = 68,
    WGR_GPU_TIMER_GI_RSM = 69,
    /* PERF-024: the worker's encode, split four ways. CPU column only. */
    WGR_GPU_TIMER_WORKER_ENC_EARLY = 70,
    WGR_GPU_TIMER_WORKER_ENC_SHADOW = 71,
    WGR_GPU_TIMER_WORKER_ENC_MAIN = 72,
    WGR_GPU_TIMER_WORKER_ENC_TAIL = 73,
    WGR_GPU_TIMER_WORKER_ENC_SKY = 74,
    WGR_GPU_TIMER_WORKER_ENC_DRAW = 75,
    WGR_GPU_TIMER_WORKER_ENC_POST = 76,
    /* LGT-026: the LOCAL-LIGHT shadow views, all of them, as one number. The four
     * WGR_GPU_TIMER_SHADOW_CASCADE_* rows are indexed by cascade number and stop at
     * MAX_CASCADES, so every local view rendered into the tiled atlas layer was
     * UNTIMED -- 24 depth passes a frame that no region named. One region and not
     * one-per-view because the question this answers is "what does caching the
     * static ones buy", and that is a per-frame total. */
    WGR_GPU_TIMER_LOCAL_SHADOW = 77,
    /* TW-WATER W2 - the Tidewater Native ocean's compute (outermost, encoder-level). In that mode
     * the Current OP water rows read n/a; in Current OP mode these two do. */
    WGR_GPU_TIMER_TW_OCEAN_SPECTRUM = 78,
    WGR_GPU_TIMER_TW_OCEAN_FFT = 79,
    WGR_GPU_TIMER_TW_SHORE_SIM = 80, /* TW-WATER W3b: the shore simulation regions */
    WGR_GPU_TIMER_TW_WAKE = 81,      /* TW-WATER W6: the boat wake simulations */
    WGR_GPU_TIMER_TW_CAUSTICS = 82,  /* TW-WATER W7h: the caustic splats + mip chains */
    WGR_GPU_TIMER_TW_BREAKERS = 83,  /* TW-WATER W7h: crest finder, breaker spray, spray update */
    WGR_GPU_TIMER_WEATHER_COVER_NEAR_CULL = 84,
    WGR_GPU_TIMER_WEATHER_COVER_FAR_CULL = 85,
    WGR_GPU_TIMER_WEATHER_COVER_NEAR_DRAW = 86,
    WGR_GPU_TIMER_WEATHER_COVER_FAR_DRAW = 87,
    WGR_GPU_TIMER_LAYERED_FOG = 88,
    WGR_GPU_TIMER_REGION_COUNT = 89,
};

/* PERF-005 - region nesting, and the ONLY correct way to total this array.
 *
 * Several regions physically enclose others: the two segment containers enclose the terrain,
 * grass and GPU-driven object draws recorded inside their render passes, and each shadow
 * cascade encloses the grass blades drawn into that cascade's depth map. Adding every region
 * up therefore over-counts. The rule is:
 *
 *     frame total ~= sum of every region whose WgrGpuTimerContainedBy() is WGR_GPU_TIMER_NONE
 *
 * i.e. sum the OUTERMOST regions only. Getting this wrong is not a rounding error: on Everon's
 * 00training it double-counts grass-shadow (0.74 ms) and the whole colour sub-pass (2.13 ms),
 * producing a "total" larger than the frame, which reads as a measurement rather than a bug.
 * Returns the enclosing region, or WGR_GPU_TIMER_NONE when the region is outermost. */
/* Additive caller-owned CPU timing snapshot, separate from latest/harvested getters.
 * state: 0 unobserved, 1 successful real frame, 2 acquire skipped, 3 failed/panicked.
 * Token identifies this call only, NOT GPU completion. Existing ABI layouts remain unchanged. */
struct WgrRenderCallCpuTimings
{
    uint32_t struct_size, version, state, count;
    uint64_t call_token;
    /* acquire,present,setup,submit,harvest,early,shadow,main,tail,sky,draw,post.
     * Hierarchical CPU buckets overlap; -1 = unrecorded. */
    float milliseconds[12];
};
static_assert(sizeof(WgrRenderCallCpuTimings) == 72, "render-call CPU layout");
static_assert(offsetof(WgrRenderCallCpuTimings, call_token) == 16, "render-call token layout");
static_assert(offsetof(WgrRenderCallCpuTimings, milliseconds) == 24, "render-call buckets layout");
/* Private GPU COUNT diagnostic. The request samples main and cascade 0
 * independently; each getter reports its own status. The caller owns token
 * and source_generation; zero count is meaningful only with state ABSENT. */
struct WgrMainTargetCountRequest
{
    uint32_t struct_size, version;
    uint64_t token;
    uint32_t model_id, reserved;
    uint64_t source_generation;
};
struct WgrMainTargetCountResult
{
    uint32_t struct_size, version, state, submitted;
    uint64_t token;
    uint32_t model_id, count;
    uint64_t source_generation, cull_epoch;
};
static_assert(sizeof(WgrMainTargetCountRequest) == 32, "main COUNT request layout");
static_assert(sizeof(WgrMainTargetCountResult) == 48, "main COUNT result layout");
struct WgrMainTargetCompletionResult
{
    uint32_t struct_size, version, state, reserved;
    uint64_t token;
    uint32_t model_id, reserved2;
    uint64_t source_generation, cull_epoch;
};
static_assert(sizeof(WgrMainTargetCompletionResult) == 48, "main completion result layout");
enum : uint32_t { WGR_MAIN_TARGET_COMPLETION_COMPLETED = 3 };
enum : uint32_t
{
    WGR_MAIN_TARGET_COUNT_UNKNOWN = 0,
    WGR_MAIN_TARGET_COUNT_PRESENT = 1,
    WGR_MAIN_TARGET_COUNT_ABSENT = 2,
};
enum : uint32_t
{
    WGR_GPU_TIMER_NONE = 0xFFFFFFFFu
};

constexpr uint32_t WgrGpuTimerContainedBy(uint32_t region)
{
    switch (region)
    {
        /* Recorded inside the depth/normal prepass render pass. */
        case WGR_GPU_TIMER_TERRAIN_PREPASS:
        case WGR_GPU_TIMER_GRASS_PREPASS:
        case WGR_GPU_TIMER_OBJ_PREPASS_SOLID:
        case WGR_GPU_TIMER_OBJ_PREPASS_ALPHA:
            return WGR_GPU_TIMER_OBJ_PREPASS_SEGMENT;
        /* Recorded inside the 3D colour sub-pass. */
        case WGR_GPU_TIMER_TERRAIN_COLOR:
        case WGR_GPU_TIMER_GRASS_COLOR:
        case WGR_GPU_TIMER_OBJ_COLOR_SOLID:
        case WGR_GPU_TIMER_OBJ_COLOR_ALPHA:
        /* The far tier draws inside the same colour sub-pass, immediately after the GPU-driven
         * objects. Its CULL is outermost (a standalone compute pass) and is not listed here. */
        case WGR_GPU_TIMER_FAR_DRAW:
        /* Clouds march and composite inside the colour sub-pass, after water. The two replay
         * containers are also inside it -- a container may itself be contained; what matters
         * for the sum rule is only that neither is outermost. */
        case WGR_GPU_TIMER_CLOUD_MARCH:
        case WGR_GPU_TIMER_CLOUD_COMPOSITE:
        case WGR_GPU_TIMER_OPS_PRE_WATER:
        case WGR_GPU_TIMER_OPS_POST_WATER:
        /* WATER DRAW AND GOD RAYS WERE MISSING HERE UNTIL 2026-08-31, and the omission is
         * easy to make: unlike the rows above they open their OWN render pass on the
         * encoder rather than being bracketed inside one, so they look standalone at the
         * call site. Containment is about the TIMESTAMPS, not the pass structure, and both
         * sit inside the colour segment's bracket (lib.rs: the segment spans 4742-5134,
         * water draw is 4929-4970 and god rays is inside the same span).
         *
         * Reported as outermost they were double-counted by the documented sum rule. On
         * perf_water that made the "sum of outermost" 20.485 ms against a 20.322 ms frame
         * -- a NEGATIVE residual, i.e. the parts explaining more than the whole, which is
         * the same class of visible-nonsense the comment above this function was written
         * about. Correcting it turns the four measured scenes from {2.395, -0.163, 3.384,
         * 2.476} ms of unexplained time into {2.864, 3.037, 3.802, 2.771} -- all positive,
         * and near-constant across frames from 9 to 20 ms. See PERF-010. */
        case WGR_GPU_TIMER_WATER_DRAW:
        case WGR_GPU_TIMER_GOD_RAYS:
            return WGR_GPU_TIMER_OBJ_COLOR_SEGMENT;
        /* Grass casts into the cascade depth maps from INSIDE each cascade's render pass, so
         * its one region is already inside the per-cascade brackets. Attributed to cascade 0
         * because a region can name only one parent; grass shadows are a near-field effect and
         * cascade 0 is where nearly all of that cost lands. */
        case WGR_GPU_TIMER_GRASS_SHADOW:
            return WGR_GPU_TIMER_SHADOW_CASCADE_0;
        /* The whole-frame envelope: never part of a sum, it IS the thing summed against. */
        case WGR_GPU_TIMER_FRAME_TOTAL:
            return WGR_GPU_TIMER_FRAME_TOTAL;
        default:
            return WGR_GPU_TIMER_NONE;
    }
}

/* PERF-005 - per-frame object accounting (mirrors WgrObjectStats in rust/src/ffi.rs).
 * The per-view blocks are atomics written by cull.wgsl and read back through the same
 * non-blocking ring as the GPU timings, so they lag the displayed frame by ~2-3 frames.
 * `direct_*` is counted on the CPU during the colour replay and is current. */
struct WgrObjectStats
{
    uint32_t registered_instances; // retained instances the cull knows about (the denominator)
    uint32_t valid;                // 0 = no readback has landed yet; zeros then mean "unknown"

    /* Main view: frustum + distance + sub-pixel + LOD. This is the PREPASS / occluder set. */
    uint32_t main_instances;
    uint32_t main_records; // surviving (instance, section) pairs
    uint32_t main_draws;   // indirect args emitted == sections drawn
    uint32_t main_tris;

    /* Colour view: the main tests plus Hi-Z occlusion. All zero when occlusion is off, in
     * which case the colour pass reuses the main args and main_* is the truth for both. */
    uint32_t color_instances;
    uint32_t color_records;
    uint32_t color_draws;
    uint32_t color_tris;
    uint32_t color_draws_solid;
    uint32_t color_draws_alpha;
    uint32_t color_tris_solid;
    uint32_t color_tris_alpha;

    uint32_t shadow_instances[4];
    uint32_t shadow_draws[4];
    uint32_t shadow_tris[4];

    /* Selected-LOD histogram over the MAIN view's survivors: instances that chose LOD 0..6,
     * bucket 7 = "LOD 7 or coarser". This is the direct test of the standing hypothesis that
     * detailed building and vegetation LODs stay active too far out. */
    uint32_t main_lod_hist[8];
    /* REN-VEG-004: the main view's triangle total if lod_inv_width were scaled by
     * whatif_scale[i], over the same survivors -- the cost of a LOD-governor step before it
     * is taken. */
    uint32_t main_whatif_tris[4];
    float whatif_scale[4];
    /* REN-OBJ-003: object fragment census -- colour, colour cutout, prepass, prepass cutout.
     * REN-ATM-001 adds [4..7]: shaded fragments that got the aerial-perspective term, split
     * opaque / cutout / vegetation-cutout, then the ones that got none ([4]+[5]+[7] = shaded
     * total, so the array carries its own denominator).
     * Zeros unless WGR_OBJECT_COUNT_FRAGMENTS=1, whose timings are not quotable. */
    uint32_t fragments[8];

    /* The CPU-replayed object path (transparent, decal, skinned/baked, non-standard depth).
     * These never reach the cull shader, so they are invisible in every count above. */
    uint32_t direct_calls;
    uint32_t direct_indirect_calls;
    uint32_t direct_instances;
    uint32_t direct_tris;
    /* Stale instance handles refused since startup (ID-3). Nonzero means a C++-side
     * identity bug -- a handle outliving its instance -- not a renderer fault. */
    uint64_t stale_instance_ops;
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

/* Shared dynamic WGPU residency lower bound. Counts renderer-owned resource
 * payload/capacity; WebGPU does not expose driver-private alignment/metadata. */
/* AST-012A — the shape of one GPU mip-feedback harvest (see wgr_get_mip_feedback). */
struct WgrMipFeedbackStats
{
    uint64_t frame;    /* renderer frame index at the query — the age clock's origin */
    uint64_t harvests; /* completed readbacks since startup; 0 = the instrument never sampled */
    uint32_t slots;    /* bindless slots covered */
    uint32_t observed; /* of those, how many carry any observation */
    uint32_t groups;   /* frames per full rotation of the texture table; 0 = disabled */
    uint32_t reserved;
};

struct WgrMemoryStats
{
    uint64_t tracked_bytes;
    uint64_t budget_bytes;
    uint64_t object_texture_bytes;
    uint64_t geometry_live_bytes;
    uint64_t geometry_capacity_bytes;
    uint64_t geometry_retired_bytes;
    uint32_t object_texture_count;
    uint32_t over_budget;
    uint64_t object_texture_retired_bytes; // submitted payload awaiting queue completion
    uint64_t backend_allocation_bytes; // all backend buffer/texture allocations; zero if unavailable
};

// Readonly live pool-range attribution. Referenced row bytes overlap; union is unique.
struct WgrGeometryAllocationRow
{
    uint64_t vertex_bytes, index_bytes;
    uint32_t model_id, lod_index;
    float resolution;
    uint32_t section_count, live_meshes, missing_meshes;
};
static_assert(sizeof(WgrGeometryAllocationRow) == 40);
struct WgrGeometryAllocationSummary
{
    uint64_t unique_vertex_bytes, unique_index_bytes;
    uint64_t pool_live_bytes, pool_capacity_bytes, pool_retired_bytes;
    uint32_t unique_meshes, models_visited, lods_total, section_visits;
    uint32_t missing_models, missing_meshes, invalid_ranges, truncated, rows_written;
    // Unique allocations shared across distinct inspected (model, LOD) rows.
    // No global ownership/exclusive/reclaimable interpretation.
    uint64_t overlap_within_inspected_meshes;
    uint64_t overlap_within_inspected_vertex_bytes, overlap_within_inspected_index_bytes;
};
static_assert(sizeof(WgrGeometryAllocationSummary) == 104);
static_assert(offsetof(WgrGeometryAllocationSummary, overlap_within_inspected_meshes) == 80);
static_assert(offsetof(WgrGeometryAllocationSummary, overlap_within_inspected_index_bytes) == 96);
// Additive optional owner diagnostic. State0 invalid,1 present generational mesh,
// 2 absent. Bytes are exact source ranges, not driver/fence/reclaimable bytes.
struct WgrMeshHandleFact
{
    uint64_t mesh_handle, vertex_bytes, index_bytes;
    uint32_t state, reserved;
};
struct WgrMeshHandleFactSummary
{
    uint32_t handles_requested, handles_inspected, present, absent, invalid, duplicate_handles;
    uint32_t complete, flags;
    uint64_t unique_vertex_bytes, unique_index_bytes;
    uint64_t live_mesh_records, pool_live_bytes, pool_generation;
    uint32_t record_scope_valid, reserved; // mesh records/pool ranges only; not owners or GPU completion
};
static_assert(sizeof(WgrMeshHandleFact) == 32);
static_assert(sizeof(WgrMeshHandleFactSummary) == 80);
static_assert(offsetof(WgrMeshHandleFactSummary, unique_vertex_bytes) == 32);
static_assert(offsetof(WgrMeshHandleFactSummary, live_mesh_records) == 48);
static_assert(offsetof(WgrMeshHandleFactSummary, record_scope_valid) == 72);
// Optional bounded snapshot fence. ACK acknowledges preceding queue work only;
// no device-memory release or future-reference/reclaimability guarantee.
struct WgrMeshSnapshotAck
{
    uint64_t ticket, epoch, pool_generation, main_submission_serial;
    uint64_t pool_live_bytes, pool_capacity_bytes, pool_retired_bytes;
    uint32_t state, row_count;
};
static_assert(sizeof(WgrMeshSnapshotAck) == 64);
static_assert(offsetof(WgrMeshSnapshotAck, state) == 56);
// On-demand registered model/LOD section references only. No current-frame,
// uploader, in-flight, device-free or suballocation ownership guarantee.
struct WgrRegisteredMeshRef
{
    uint64_t mesh, distinct_models, distinct_model_lods, section_occurrences;
    uint32_t flags, reserved;
};
struct WgrRegisteredMeshRefSummary
{
    uint64_t models_visited, lods_visited, sections_visited, missing_records;
    uint32_t rows_written, complete, refusal_flags, reserved;
};
static_assert(sizeof(WgrRegisteredMeshRef) == 40);
static_assert(sizeof(WgrRegisteredMeshRefSummary) == 48);
static_assert(offsetof(WgrRegisteredMeshRefSummary, rows_written) == 32);
struct WgrLodDemandRow { uint32_t model_id, lod_count, lod_mask, state; };
struct WgrLodDemandReport
{
    uint64_t epoch, current_frame, last_sample_frame;
    uint32_t status, frames_requested, frames_attempted, frames_sampled, dropped_frames;
    uint32_t map_failures, dropped_dispatches, dispatched_views, pass_mask, row_count;
    WgrLodDemandRow rows[8];
};
static_assert(sizeof(WgrLodDemandRow) == 16);
static_assert(sizeof(WgrLodDemandReport) == 192);
static_assert(offsetof(WgrLodDemandReport, rows) == 64);

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
enum WgrWaterInteractionFlags : uint32_t { WGR_WATER_INTERACTION_PENDING_IMPULSE = 1u << 0, WGR_WATER_INTERACTION_CAPSULE = 1u << 8, WGR_WATER_INTERACTION_PLAYER_WADING = 1u << 9, WGR_WATER_INTERACTION_PLAYER_SWIMMING = 1u << 10, WGR_WATER_INTERACTION_LEFT_SIDE = 1u << 11, WGR_WATER_INTERACTION_LARGE_BODY = 1u << 12, WGR_WATER_INTERACTION_ROTOR = 1u << 13 };
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
    /* WRL-003 — which water body this node draws: 0 = the global ocean plane at
     * WgrWaterParams.sea_level, i + 1 = WgrWaterParams.bodies entry i (its own level, wave
     * scale, containment ellipse and profile). Was a padding lane, so every existing
     * producer already writes the ocean value. */
    float body;
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

// --- Far instance tier -------------------------------------------------------

/* One far-field proxy, uploaded ONCE per world from the authored placement rows.
 *
 * This tier exists because the object residency window stops well short of the
 * view distance (~900 m on Everon, ~550-711 m on Chernarus): beyond it a
 * placement is never instantiated as an Object at all, so the world is bare
 * ground and assets pop in as you fly. A proxy is fed straight from the
 * placement row and never becomes an Object, so the whole island can be
 * represented for the cost of one compute sweep (measured: 1.2M placements in
 * 0.05-0.14 ms, 3.65M in 0.15-0.23 ms).
 *
 * `x/y/z` is the ABSOLUTE authored world position (the cull and both vertex
 * shaders subtract the camera before any test or transform -- at kilometre-scale
 * coordinates an absolute frustum test shifts the frustum by cam_pos).
 * `height`/`radius` come from the model's ModelInfo bbox, `colour` from its
 * shape average colour (packed 0xAARRGGBB, the engine's PackedColor order). */
struct WgrFarInstance
{
    float x, y, z;
    float height;
    float radius;
    uint32_t colour;
    uint32_t flags; /* bit0: 0 = card (vegetation), 1 = prism (structure) */
    uint32_t pad;
};

/* WgrFarInstance::flags bit 0: draw this proxy as an extruded prism (a
 * structure's silhouette) rather than a camera-facing card. Cards cost ~4x less
 * to draw (2 triangles vs 12), so this bit is set only where a box actually
 * reads better than a billboard. */
#define WGR_FAR_FLAG_PRISM 1u

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
static_assert(sizeof(WgrMeshVertex) == 68, "WgrMeshVertex must match the engine SVertex layout");
static_assert(sizeof(WgrDraw2DBatch) == 32, "WgrDraw2DBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrDraw3D) == 288, "WgrDraw3D layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrLight) == 64, "WgrLight layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTonemap) == 64, "WgrTonemap layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrExposure) == 32, "WgrExposure layout must match the Rust #[repr(C)] struct");
/* 18 vec4 = 288 bytes. Was 256 (16 vec4) before the moon disc added moon_params + moon_sun. */
static_assert(sizeof(WgrSky) == 288, "WgrSky layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSkyLook) == 192, "WgrSkyLook layout must match the Rust #[repr(C)] struct");
/* 7 vec4 = 112 bytes. Was 80 (5 vec4) before the moon disc added moon_params + moon_sun.
 * This assert and sky/mod.rs's rust_and_wgsl_sky_structs_declare_the_same_fields_in_the_same_order
 * are the ONLY things standing between a lane added on one side of the FFI and silent garbage
 * in the shader — keep them in step. */
static_assert(sizeof(WgrSkyRuntime) == 128, "WgrSkyRuntime layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTerrainSunShadow) == 16, "WgrTerrainSunShadow layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSkyVisibility) == 32, "WgrSkyVisibility layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrFoliage) == 80, "WgrFoliage layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGtao) == 52, "WgrGtao layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSkyVis) == 48, "WgrSkyVis layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrDepthOfField) == 52, "WgrDepthOfField layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrLayeredFog) == 80, "WgrLayeredFog must match Rust");
static_assert(offsetof(WgrRenderParams, layered_fog) == 636);
static_assert(sizeof(WgrRenderParams) == 716, "WgrRenderParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrFrameParams) == 16, "WgrFrameParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrCameraShadow) == 352 + 24 * 64 + 16,
              "WgrCameraShadow layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrCamera) == 576 + 24 * 64 + 16,
              "WgrCamera layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrShadowCaster) == 136, "WgrShadowCaster layout must match the Rust #[repr(C)] struct");
// LGT-010 grew this by local_count + 3 pad words + four mat4: 288 + 16 + 256.
static_assert(sizeof(WgrShadowPass) == 288 + 16 + 24 * 64,
              "WgrShadowPass layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrCmd) == 8, "WgrCmd layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrOverlayVertex) == 20, "WgrOverlayVertex layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrOverlayDraw) == 40, "WgrOverlayDraw layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTerrainParams) == 88, "WgrTerrainParams layout must match the Rust #[repr(C)] struct");
static_assert(offsetof(WgrTerrainParams, _pad3) == 80, "Wet-soil diagnostic reuses exact terrain padding offset");
static_assert(sizeof(WgrTerrainNode) == 24, "WgrTerrainNode layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrTerrainBatch) == 16, "WgrTerrainBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGrassBatch) == 16, "WgrGrassBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGrassTrack) == 16, "WgrGrassTrack layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrGrassDownwash) == 16, "WgrGrassDownwash layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSmokeShadowBlob) == 32, "WgrSmokeShadowBlob layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrSmokeVolumeParams) == 96, "WgrSmokeVolumeParams layout must match the Rust #[repr(C)] struct");
  static_assert(sizeof(WgrGrassParams) == 4784, "WgrGrassParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrWaterParams) == 1472, "WgrWaterParams layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrRainWaterParams) == 48, "WgrRainWaterParams must match Rust");
static_assert(sizeof(WgrRainWaterSourceKey)==32 && offsetof(WgrRainWaterSourceKey,terrain_range)==24);
static_assert(sizeof(WgrRainWaterFineCell)==64 && offsetof(WgrRainWaterFineCell,parent)==48);
static_assert(sizeof(WgrRainWaterPublication)==96 && offsetof(WgrRainWaterPublication,source)==48 &&
    offsetof(WgrRainWaterPublication,revision)==80 && offsetof(WgrRainWaterPublication,flags)==88);
static_assert(sizeof(WgrWaterNode) == 40, "WgrWaterNode layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrWaterBatch) == 16, "WgrWaterBatch layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrWaterInteractionEvent) == 64 && alignof(WgrWaterInteractionEvent) == 16, "WgrWaterInteractionEvent must match Rust");
static_assert(sizeof(WgrWaterInteractionParams) == 96 && alignof(WgrWaterInteractionParams) == 16, "WgrWaterInteractionParams must match Rust");
static_assert(sizeof(WgrFarInstance) == 32, "WgrFarInstance layout must match the Rust #[repr(C)] struct");
// WgrFrame embeds WgrShadowPass, so LGT-010 grew it by the same 272 bytes.
static_assert(sizeof(WgrFrame) == 576 + 16 + 24 * 64,
              "WgrFrame layout must match the Rust #[repr(C)] struct");
static_assert(sizeof(WgrAbiCheck) == 28, "WgrAbiCheck layout must match Rust");

// --- Functions ---------------------------------------------------------------


// Additive fixture-only ABI; existing ABI15 structures remain unchanged.
// Recorded indirect commands, NOT survivor/pixel/queue completion or all-pass proof.
// Optional additive camera tuple v1: unjittered original main scene camera from a successful returned frame.
// status1 captured,2 no source/index,3 nonfinite,4 viewport,5 non1:1,6 serial exhaustion.
// source1 terrain/2 Draw3D/3 water/4 grass; source0 fallback is refused. Full main attachment viewport origin0.
struct WgrMainCameraTuple {
 uint32_t version,struct_bytes,status,camera_index; uint64_t generation;
 uint32_t source,render_width,render_height,output_width,output_height,reserved;
 float camera_position[4],projection[16],view[16];
};
static_assert(sizeof(WgrMainCameraTuple)==192);
static_assert(offsetof(WgrMainCameraTuple,generation)==16);
static_assert(offsetof(WgrMainCameraTuple,projection)==64);
// Getter0 unavailable/disabled,1 copied,2 invalid; output untouched on0/2. Never waits/polls.
struct WgrGeometryPassFacts {
 uint32_t version,struct_bytes,enabled,capabilities,required,pending,recorded_this_frame,reserved;
 uint64_t frame,instance_epoch,cascade_epoch,cascade_frame,gi_rsm_epoch,gi_rsm_frame;
 uint32_t local_count,local_draw_mask,local_valid_mask,interior_draw_mask,interior_valid_mask,cascade_draw_mask,cascade_count,reserved2;
 uint64_t local_epochs[24],local_frames[24],interior_epochs[5],interior_frames[5];
 uint64_t reflection_epoch,reflection_frame; // v2 append; previous member offsets unchanged
};
static_assert(sizeof(WgrGeometryPassFacts)==592);
static_assert(offsetof(WgrGeometryPassFacts,local_epochs)==112);
static_assert(offsetof(WgrGeometryPassFacts,interior_epochs)==496);
static_assert(offsetof(WgrGeometryPassFacts,reflection_epoch)==576);
static_assert(offsetof(WgrGeometryPassFacts,reflection_frame)==584);

// Private v2 36-lane COUNT diagnostic. required is the frozen applicable set:
// main, up to 4 solar, up to 24 local, 5 interior, GI and planar reflection.
// status 0 awaits a valid exact frame plan and final queue completion;
// 1=all required views known, 2=Unknown remains among required views.
// Cached facts keep their original token but must match the current target and
// exact published image. No state certifies pixels or Fine release/reclaim.
// pending_samples counts current-request copies still awaiting map completion;
// dropped_samples is UINT32_MAX because these existing rings do not count drops.
struct WgrViewReferenceFacts {
 uint32_t version,struct_bytes,status,view_count;
 uint64_t render_token,instance_epoch,required,present,absent,unknown,cached;
 uint32_t pending_samples,dropped_samples;
 uint32_t target_model_id,target_instance_handle; // handle 0 = all instances of model
 uint64_t target_source_generation; // nonzero, exact source-content generation
};
static_assert(sizeof(WgrViewReferenceFacts)==96);
static_assert(offsetof(WgrViewReferenceFacts,render_token)==16);
static_assert(offsetof(WgrViewReferenceFacts,required)==32);
static_assert(offsetof(WgrViewReferenceFacts,pending_samples)==72);
static_assert(offsetof(WgrViewReferenceFacts,target_model_id)==80);
static_assert(offsetof(WgrViewReferenceFacts,target_source_generation)==88);
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

    /* REN-RES-001 -- bindless slot leases.
     *
     * `wgr_model_register` resolves each material's texture handle to a dense bindless
     * slot ONCE and bakes that integer into the retained material table, which is
     * append-only: a registered model's slot index is final. Destroying a texture and
     * re-uploading it therefore used to break the model two ways at once -- it fell to
     * the white fallback forever, AND its old slot went to the next unrelated upload, so
     * it could come back wearing a stranger's texture.
     *
     * A LEASE fixes that without any shader change. It reserves a slot for one logical
     * texture (one TextureWgpu, i.e. one texture NAME) rather than for one upload of it:
     * while leased, the slot is never recycled, an eviction merely points it at the white
     * fallback, and the re-upload reclaims the SAME index. Models never learn a new
     * number because the number never changes.
     *
     * Lifetime: acquire once before the first upload, release at the death of the texture
     * OBJECT -- never at its eviction. Releasing early returns the slot to the pool where
     * an unrelated texture can take it, which is exactly the bug this removes. */

    /* Reserve a slot. Returns 0 when the bindless array is at cap; that is not an error,
     * the caller simply falls back to wgr_texture_create. */
    WGR_API uint32_t wgr_texture_slot_acquire(WgrRenderer* renderer);

    /* Return a lease to the pool. See the lifetime note above. */
    WGR_API void wgr_texture_slot_release(WgrRenderer* renderer, uint32_t slot);

    /* As wgr_texture_create, but uploads into `slot` instead of allocating a fresh one.
     * A `slot` of 0, or one that is not leased, behaves exactly as wgr_texture_create. */
    WGR_API WgrTexture wgr_texture_create_in_slot(WgrRenderer* renderer, uint32_t width, uint32_t height,
                                                  int32_t format, uint32_t mip_count, uint32_t flags,
                                                  const uint8_t* data, uint32_t byte_length, uint32_t slot);

    /* Replace the pixels of an existing RGBA8 texture. */
    WGR_API void wgr_texture_update(WgrRenderer* renderer, WgrTexture id, const uint8_t* rgba, uint32_t byte_length);

    WGR_API void wgr_texture_destroy(WgrRenderer* renderer, WgrTexture id);

    /* Create a static mesh from interleaved vertices + 16-bit triangle-list
     * indices; returns a non-zero handle, or 0 on failure. */
    WGR_API WgrMesh wgr_mesh_create(WgrRenderer* renderer, WgrSlice<WgrMeshVertex> verts, WgrSlice<uint32_t> indices);

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

    /* --- REN-THR-009: the upload path that does not need the renderer handle --- */

    /* Opaque. C++ only ever holds the pointer and hands it back. */
    struct WgrUploader;

    /* Borrow the renderer's upload surface. Unlike every other mesh entry point, the
     * calls below do NOT form an exclusive reference to the renderer, so they are the
     * one producer-side mutation that a thread other than the renderer's may make.
     *
     * LIFETIME CONTRACT -- stated here the way the rest of this ABI states its
     * null-tolerance, because this one crosses a thread boundary:
     *
     *   1. The returned pointer is valid until wgr_destroy and is stable for the
     *      renderer's whole life. It is a borrow, not an owner: nothing to release.
     *      Returns null for a null renderer.
     *   2. It may be used from any thread, concurrently with itself.
     *   3. It may NOT be used concurrently with any entry point that takes
     *      WgrRenderer* (62 of the 79 form an exclusive reference -- REN-THR-001).
     *      That is trivially satisfied while everything is serial; once a render
     *      thread exists, this is the rule that puts uploads on the producer and
     *      everything else on the consumer.
     *   4. Mesh lifetime is producer-owned and single-threaded. Updating and
     *      destroying the SAME handle from two threads at once is forbidden, not
     *      merely unspecified -- no lock defends it, deliberately (REN-THR-009). */
    WGR_API const WgrUploader* wgr_uploader_get(WgrRenderer* renderer);

    /* wgr_mesh_update's semantics exactly -- topology unchanged, vertex count must not
     * exceed the mesh's allocated count -- without the renderer. An unknown handle,
     * including one already passed to wgr_uploader_mesh_destroy, is ignored. Returns 1
     * if a write was issued, 0 otherwise. */
    WGR_API uint32_t wgr_uploader_mesh_update(const WgrUploader* uploader, WgrMesh id,
                                              WgrSlice<WgrMeshVertex> verts);

    /* Park a mesh destroy for the renderer to run at the top of its next frame. It
     * cannot happen here: the real destroy drops skin-bake bind groups the draw path
     * reads and returns ranges to the geometry pool's free lists, neither of which may
     * change mid-frame. The handle stops accepting uploads immediately. */
    WGR_API void wgr_uploader_mesh_destroy(const WgrUploader* uploader, WgrMesh id);

    /* --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) --- */

    /* Sentinel returned by wgr_model_register on failure. */
    constexpr uint32_t WGR_INVALID_MODEL = 0xFFFFFFFFu;

    /* Register one opaque-rigid model for GPU-driven rendering. `lods`, `sections`, and
     * `materials` describe a single LODShapeWithShadow: `sections` and `materials` are
     * parallel (one material per section) and each lods[i].section_base indexes
     * `sections` relative to this model. Section mesh handles are resolved to the
     * shared geometry pool. Returns the model id (for wgr_instance_add) or
     * WGR_INVALID_MODEL on error. Call once per shape. */
    /* `name` (nullable, UTF-8, borrowed for the call): a stable identity for the model,
     * used to key the on-disk sky-visibility bake cache. Passing null only disables the
     * cache for this model. */
    /* Retire a model registration after removing its owners. IDs are never reused.
       Mesh storage has a separate lifetime and is released by wgr_mesh_destroy. */
    WGR_API void wgr_model_retire(WgrRenderer* renderer, uint32_t model);
    WGR_API uint32_t wgr_model_register(WgrRenderer* renderer, float bounding_sphere, WgrSlice<WgrModelLod> lods,
                                        WgrSlice<WgrModelSection> sections, WgrSlice<WgrModelMaterial> materials,
                                        const char* name);

    /* Register a batch of per-tree crown centres (MODEL space) into the global crown-centre
     * table and return the base index of this batch (foliage-translucency-plan.md §9 Approach A).
     * Each centre's .xyz is a tree's canopy centroid in the shape's vertex space (.w unused).
     * The caller bakes `base + local_component_index` into each forest vertex's `conform` word;
     * vs_gpu reads the table with that index to get a per-tree radial-normal centre instead of the
     * whole-mesh inst.center. Call once per forest LOD level during model registration. */
    WGR_API uint32_t wgr_register_crown_centres(WgrRenderer* renderer, WgrSlice<WgrVec4> centres);

    /* Add a static retained instance; returns its stable slot (recycled from removed
     * slots). Update it in place with wgr_instance_update (a move, or a destruction-
     * phase change), remove it with wgr_instance_remove.
     *
     * ABI v7: the returned u32 is an opaque HANDLE (generation-checked on the Rust
     * side), not a raw slot index. 0 = failure, unambiguously. A handle kept past its
     * instance's removal is refused (and counted) instead of addressing whatever
     * reused the slot -- treat it as a value, never do arithmetic on it. */
    WGR_API uint32_t wgr_instance_add(WgrRenderer* renderer, const WgrInstance* inst);
    WGR_API void wgr_instance_update(WgrRenderer* renderer, uint32_t slot, const WgrInstance* inst);
    WGR_API void wgr_instance_remove(WgrRenderer* renderer, uint32_t slot);

    /* Exact bytes=176/version=1. Returns the fact's status; 0 on unsupported
     * layout/null/panic, leaving output untouched. Owner-thread read only: no
     * GPU wait, table scan, stale-operation mutation, or render completion. */
    WGR_API uint32_t wgr_instance_cpu_fact(const WgrRenderer* renderer, uint32_t handle,
                                           WgrInstanceCpuFact* out, uint32_t bytes, uint32_t version);

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

    /* FAR INSTANCE TIER — replace the whole far-proxy set. Called ONCE per world, right after
     * the authored placement list is complete: every placement gets a proxy, including the
     * overwhelming majority that will never be inside the object residency window. The renderer
     * copies the slice into one GPU buffer and owns it from there (cull + draw), so nothing on
     * the C++ side walks these per frame. Passing an empty slice releases the set. */
    WGR_API void wgr_far_set_instances(WgrRenderer* renderer, WgrSlice<WgrFarInstance> instances);

    /* FAR INSTANCE TIER — per-frame knobs. `near_cutoff_m` is where the tier STARTS (proxies
     * closer than this are the real objects' job, so it should be the object draw distance);
     * `far_distance_m` is where it stops; `pixel_limit` is the sub-pixel rejection threshold in
     * PIXELS OF PROJECTED HEIGHT and is the single biggest win in the whole tier (measured on an
     * aircraft view: 421,323 -> 22,135 surviving instances, 0.314 -> 0.025 ms of draw at 2 px);
     * `enabled` 0 makes the tier inert without releasing its buffer. Call once per frame for the
     * main scene camera. */
    WGR_API void wgr_far_set_params(WgrRenderer* renderer, float near_cutoff_m, float far_distance_m,
                                    float pixel_limit, uint32_t enabled);

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
    /* Experimental snow: four header floats followed by a 512x512 deficit window. */
    WGR_API void wgr_terrain_set_snow(WgrRenderer* renderer, const float* data, uint32_t count);
    // Separate signed 12.5cm mud height view; the snow data and public byte layouts remain intact.
    WGR_API void wgr_terrain_set_mud(WgrRenderer* renderer, const float* data, uint32_t count);
    // Separate signed sand view; positive loose rims are preserved.
    WGR_API void wgr_terrain_set_sand(WgrRenderer* renderer, const float* data, uint32_t count);
    /* Sinkhole W1: terrain holes for this frame. `edges` = n_edges x {nx, nz, d, last} in world X/Z; a point
     * is inside an area when nx*x + nz*z + d >= 0 for every edge of it, last = 1 closes an area. At most 64
     * records are kept; n_edges = 0 clears them. Optional {0,0,worldCeilingY,2} BEFORE a polygon bounds
     * its terrain/grass cut to Y <= ceiling; it consumes one record. Legacy polygons remain unbounded.
     * Bounded horizontal openings are not vertical skylight apertures. */
    WGR_API void wgr_terrain_set_holes(WgrRenderer* renderer, const float* edges, uint32_t n_edges);
    /* Sinkhole W1b: how far underground the camera is, 0 (open air) .. 1 (deep in a cave). Scales the
     * outdoor-air effects (sun shafts, surface scattering) away and lets eye adaptation open up
     * (WGR_CAVE_EXPOSURE_MAX). */
    WGR_API void wgr_set_camera_underground(WgrRenderer* renderer, float underground);

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
        uint32_t _pad0; // 1 = generated legacy far satellite; 0 = authored/ordinary.
        uint32_t legacy_detail_normal; // Optional legacy terrain NOHQ layer, 0 = absent.
        WgrTerrainUv satellite_uv;
        WgrTerrainUv mask_uv;
        WgrTerrainUv surface_uvs[WGR_TERRAIN_SURFACE_SLOTS];
        /* RFG-065 -- a NATIVELY loaded Enfusion (Reforger) surface, read from the
         * palette entry's own `.emat`. Appended rather than folded into the fields
         * above because none of them fits: an Enfusion surface has no LCA mask and no
         * satellite, so `surface_count` stays 0 and the legacy branch is what these
         * modify. All zero on every other world.
         *
         *   enfusion         1 = this palette entry resolved an `.emat`; 0 = not one
         *   detail_scale     1 / ScaleUV, in repeats per metre of WORLD xz. The legacy
         *                    route maps one image onto each 12.5 m land cell; the corpus
         *                    authors 2..8 m (median 4, n=36 of 51), so the ground is
         *                    drawn 12.5/ScaleUV too coarse -- 3.1x at the median -- and
         *                    its period sits exactly on the land grid.
         *   middle           BCRMiddleMap's layer index, 0 = none
         *   middle_scale     1 / MiddleScaleUV (20..150 m, n=29)
         *   middle_blend     MiddleBCRBlend clamped to 0..1 (12 of 37 author 0)
         *   detail_max       DetailMaxDistance: metres at which the detail map is gone
         *   detail_fade      DetailBlendDistance: width of the crossfade below it
         *   middle_color     MiddleColor, a multiplier applied to the middle map's
         *                    texel in LINEAR (the rule RFG-071 measured for Color_N).
         *                    1,1,1 when the material names none. The middle maps that
         *                    come with one are brightness tiles -- Dirt_01_Middle_BCR
         *                    is 216/216/216 -- and without it they are light grey
         *                    where the material says dark brown. Sits on its own
         *                    16-byte row (WGSL vec3 alignment); the pad closes it. */
        uint32_t enfusion;
        float detail_scale;
        uint32_t middle;
        float middle_scale;
        float middle_blend;
        float detail_max;
        float detail_fade;
        float legacy_detail_scale; // Repeats/metre for legacy_detail_normal; 0 = absent.
        float middle_color[3];
        uint32_t puddle_flags; // Former padding: bit 0 ground receiver; bit 1 legacy/native soft soil; bits 8..13 authored soft-soil slots.
    };
    /* 20 header words, then the satellite/mask UVs and six per-slot UVs of two
     * vec4 each, then the Enfusion block. The WGSL `TerrainMaterial` in terrain.wgsl
     * must be kept to the same 384 bytes: a skew here renders as the wrong ground,
     * not as an error. */
    static_assert(sizeof(WgrTerrainUv) == 32, "WgrTerrainUv layout must match the Rust #[repr(C)] struct");
    static_assert(sizeof(WgrTerrainMaterial) == 384,
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
    /* Empty cells preserve a matching revision; changed grid/revision requires
     * width*height cells. enabled=0 clears presentation, never simulation. */
    WGR_API void wgr_rain_water_set_grid(WgrRenderer* renderer, const WgrRainWaterParams* params,
        const WgrVec4* cells, uint32_t count, uint64_t revision);
    /* These two additions require the paired fine consumer. Source receipt is
     * published only after an actual matching heightmap upload. Empty slices
     * preserve only a complete exact-key publication; disabled/stale clears both.
     * Fine rectangles exclude coarse proxies at actual world coordinates. */
    WGR_API void wgr_rain_water_set_source(WgrRenderer* renderer, const WgrRainWaterSourceKey* source);
    WGR_API void wgr_rain_water_set_publication(WgrRenderer* renderer, const WgrRainWaterPublication* publication,
        const WgrVec4* coarse, uint32_t coarse_count, const WgrRainWaterFineCell* fine, uint32_t fine_count);

    /* Set per-cascade configuration (0..7). Spectrum initialisation regenerates when spectrum parameters change. */
    WGR_API void wgr_water_set_cascade_config(WgrRenderer* renderer, uint32_t index, const WgrWaterCascadeConfig* config);
    WGR_API void wgr_water_set_interaction_params(WgrRenderer* renderer, const WgrWaterInteractionParams* params);
    WGR_API void wgr_water_submit_interactions(WgrRenderer* renderer, const WgrWaterInteractionEvent* events, uint32_t count);
    /* Sinkhole W3: the Tidewater water's drawn surface around the camera (the W9a probe grid, read back
     * 1-3 frames late): up to `cap` displaced vertices (x, y, z) into `xyz`, the grid lanes (first
     * lattice x, z, step, texels per side) into `lanes[4]`, the readback's age into `age_ms`. Returns
     * the texel count, 0 when there is none. Any thread. */
    WGR_API uint32_t wgr_water_drawn_grid(float* xyz, uint32_t cap, float* lanes, float* age_ms);

    /* Render + present one frame. Returns 0 on success (incl. transient skipped
     * frames), negative on error. */
    WGR_API int32_t wgr_render_frame(WgrRenderer* renderer, const WgrFrame* frame);

    /* Same render call with copied CPU facts; exact72/v1 and nonzero token required.
     * -4 refuses foreign layout before any input/output dereference or rendering. */
    WGR_API int32_t wgr_render_frame_cpu_timings(WgrRenderer*, const WgrFrame*, uint64_t call_token,
        WgrRenderCallCpuTimings*, uint32_t output_size, uint32_t output_version);

    /* Diagnostic opt-in; exact v1 sizes required before pointer dereference.
     * submitted=1 means main colour indirect draws were recorded in a closed pass
     * and the main encoder containing COUNT copy was accepted; it does not prove pixels.
     * render status 0 alone also includes a skipped surface acquisition.
     * The result state remains UNKNOWN until a later nonblocking fact poll. */
    WGR_API int32_t wgr_render_frame_main_target_count(WgrRenderer*, const WgrFrame*,
        const WgrMainTargetCountRequest*, uint32_t request_size, uint32_t request_version,
        WgrMainTargetCountResult*, uint32_t output_size, uint32_t output_version);
    WGR_API uint32_t wgr_main_target_count_fact(WgrRenderer*, WgrMainTargetCountResult*,
        uint32_t output_size, uint32_t output_version);
    /* Exact opt-in main request only. state=3 is completion of preceding queue
     * submissions, not pixel visibility, all-view proof or Fine release. */
    WGR_API uint32_t wgr_main_target_completion_fact(WgrRenderer*, WgrMainTargetCompletionResult*,
        uint32_t output_size, uint32_t output_version);
    /* Private v2 all-view snapshot; one nonblocking renderer borrow and no new GPU copy.
     * Returns 0 for foreign layout without touching output. Diagnostic only. */
    WGR_API uint32_t wgr_view_reference_facts(WgrRenderer*, WgrViewReferenceFacts*,
        uint32_t output_size, uint32_t output_version);
    /* Same opt-in request, separate cascade-0 cull counter. UNKNOWN unless its
     * depth pass closed and the copied sample was committed after final submit.
     * A view-level zero does not prove pixels or permit residency release. */
    WGR_API uint32_t wgr_shadow0_target_count_fact(WgrRenderer*, WgrMainTargetCountResult*,
        uint32_t output_size, uint32_t output_version);
    /* Indexed solar cascade (0..3) variant of the same diagnostic. Invalid
     * indices fail without touching output; inactive sun views remain UNKNOWN. */
    WGR_API uint32_t wgr_shadow_target_count_fact(WgrRenderer*, uint32_t cascade,
        WgrMainTargetCountResult*, uint32_t output_size, uint32_t output_version);
    /* GI sun-proxy sky view 5, same opt-in request and v1 result layout.
     * UNKNOWN on cached/no-refresh RSM, disabled GI or missing cull/draw/submit.
     * No pixel proof or residency-release authority. */
    WGR_API uint32_t wgr_gi_target_count_fact(WgrRenderer*, WgrMainTargetCountResult*,
        uint32_t output_size, uint32_t output_version);
    /* Local shadow tile 0, distinct from solar cascade 0 even at night.
     * With the opt-in publication witness, a mapped COUNT stays attached to
     * the exact cached tile and retains its original request token. A fresh
     * cull and closed indirect depth draw are required to bind it. Missing
     * tile publication or skipped draws stay UNKNOWN. No residency-release authority. */
    WGR_API uint32_t wgr_local_shadow0_target_count_fact(WgrRenderer*, WgrMainTargetCountResult*,
        uint32_t output_size, uint32_t output_version);
    /* Planar reflection's independently culled retained objects. UNKNOWN when
     * the mirror is disabled/skipped or its own early encoder did not submit
     * a closed indirect draw. Diagnostic only, never residency authority. */
    WGR_API uint32_t wgr_reflection_target_count_fact(WgrRenderer*, WgrMainTargetCountResult*,
        uint32_t output_size, uint32_t output_version);
    /* Interior sky-visibility zenith view 0, distinct from GI sky view 5.
     * With the opt-in publication witness, a mapped count stays attached to
     * its exact cached generation and keeps its original request token. Other
     * cached/skipped layers without that exact publication stay UNKNOWN.
     * No residency-release authority. */
    WGR_API uint32_t wgr_sky0_target_count_fact(WgrRenderer*, WgrMainTargetCountResult*,
        uint32_t output_size, uint32_t output_version);
    /* Interior directions 1..4 use separate words in the bounded batch ring.
     * With the opt-in publication witness, a mapped count remains attached to
     * its exact cached direction/generation/view/source and original token.
     * Without it, only a fresh cull and closed indirect draw can return COUNT.
     * Failed or draw-less views stay UNKNOWN. No residency-release authority. */
    WGR_API uint32_t wgr_interior_target_count_fact(WgrRenderer*, uint32_t direction,
        WgrMainTargetCountResult*, uint32_t output_size, uint32_t output_version);
    /* Local shadow tiles 1..23 use private words in the bounded batch ring.
     * With the opt-in publication witness, a mapped COUNT remains attached to
     * its exact cached tile, atlas generation/shape, day/night cull index,
     * source and original token. Failed or draw-less tiles stay UNKNOWN.
     * No residency-release authority. */
    WGR_API uint32_t wgr_local_shadow_target_count_fact(WgrRenderer*, uint32_t tile,
        WgrMainTargetCountResult*, uint32_t output_size, uint32_t output_version);

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

    /* PERF-005 — per-region CPU ENCODE times, same WgrGpuTimerRegion indices as
     * wgr_get_gpu_timings; -1 = the region was not recorded this frame. Needs no adapter
     * feature and is never asynchronous — this is the wall-clock cost of RECORDING the region
     * into the command encoder, which on a GPU-driven renderer is a very different number from
     * the GPU cost. Returns the region count written, 0 on a null renderer/output. */
    WGR_API uint32_t wgr_get_cpu_timings(WgrRenderer* renderer, float* out_ms, uint32_t out_len);

    /* PERF-005 — this frame's object accounting. Returns 1 on success, 0 when unavailable. */
    WGR_API uint32_t wgr_get_object_stats(WgrRenderer* renderer, WgrObjectStats* out);

    /* LGT-026 — how many local-light shadow views the last frame re-rendered, and how many it
     * re-used from the cache. A correct cache changes nothing anyone can SEE, which is exactly
     * what makes a broken one hard to notice, so this pair is the only way to tell a working
     * cache from one that never hits. Returns 1 on success, 0 when unavailable. */
    WGR_API uint32_t wgr_get_local_shadow_stats(WgrRenderer* renderer, uint32_t* rendered, uint32_t* cached);

    /* Current dynamic-resource residency accounting. Returns 1 on success. */
    WGR_API uint32_t wgr_get_memory_stats(WgrRenderer* renderer, WgrMemoryStats* out);
    // Status0 disabled/1 copied/2 invalid; size592/version2 required; exclusive renderer owner.
    WGR_API uint32_t wgr_geometry_pass_facts(WgrRenderer*,WgrGeometryPassFacts*,uint32_t bytes,uint32_t version);
    WGR_API uint32_t wgr_main_camera_tuple(WgrRenderer*,WgrMainCameraTuple*,uint32_t bytes,uint32_t version);
    WGR_API uint32_t wgr_get_geometry_allocation_report(WgrRenderer* renderer,
        WgrSlice<uint32_t> models, uint32_t max_rows, uint32_t max_visits,
        WgrGeometryAllocationRow* rows, uint32_t row_capacity, WgrGeometryAllocationSummary* summary);
    // Explicit owner-only snapshot, max8192. Does not drain deferred retirement,
    // submit commands or wait. Zero return invalid/unavailable; no freeing proof.
    WGR_API uint32_t wgr_get_mesh_handle_facts(WgrRenderer*, WgrSlice<uint64_t> handles,
        WgrMeshHandleFact* facts, uint32_t fact_capacity, WgrMeshHandleFactSummary* summary);
    // Status0 disabled,1 collected (possibly incomplete),2 invalid; at most8192
    // unique nonzero full handles. Disabled/invalid leaves outputs untouched.
    WGR_API uint32_t wgr_registered_mesh_refs(WgrRenderer*, WgrSlice<uint64_t> handles,
        WgrRegisteredMeshRef* rows, uint32_t row_capacity, WgrRegisteredMeshRefSummary* summary);
    // States: disabled0,queued1,submitted2,acknowledged3,cancelled4,invalid5,busy6,unknown7.
    // Request captures immutable actual records; next MAIN submission fences them.
    // Poll accepts null facts/capacity0 for summary-only, never polls/waits itself.
    WGR_API uint32_t wgr_request_mesh_snapshot_ack(WgrRenderer*, uint64_t epoch, WgrSlice<uint64_t> handles,
        WgrMeshHandleFact* facts, uint32_t fact_capacity, WgrMeshHandleFactSummary* summary, WgrMeshSnapshotAck* ack);
    WGR_API uint32_t wgr_poll_mesh_snapshot_ack(WgrRenderer*, uint64_t ticket,
        WgrMeshHandleFact* facts, uint32_t fact_capacity, WgrMeshHandleFactSummary* summary, WgrMeshSnapshotAck* ack);
    WGR_API uint32_t wgr_cancel_mesh_snapshot_ack(WgrRenderer*, uint64_t ticket);
    // Explicit owner-only diagnostics; no blocking GPU wait, no asset/ownership proof.
    WGR_API uint32_t wgr_start_lod_demand(WgrRenderer*, uint64_t epoch, WgrSlice<uint32_t> models, uint32_t frames);
    WGR_API uint32_t wgr_poll_lod_demand(WgrRenderer*, uint64_t epoch, WgrLodDemandReport*);
    WGR_API void wgr_cancel_lod_demand(WgrRenderer*, uint64_t epoch);

    /* AST-012A — the latest GPU mip feedback: for each bindless texture slot, the finest mip
     * level any fragment actually asked for. `out_mip[slot]` gets that level, or 0xFF when the
     * slot has never been observed; `out_age[slot]` gets how many frames ago it was harvested,
     * saturating at 0xFFFF. Either output may be null. Returns the slots written.
     *
     * Non-blocking, and deliberately approximate in TIME: the values arrive through an async
     * readback ring, and only one rotation group of the texture table writes per frame, so a
     * slot's answer can be a whole rotation old. `stats->harvests == 0` means no readback has
     * ever completed — report that as "no sample", never as "nothing was sampled". */
    WGR_API uint32_t wgr_get_mip_feedback(WgrRenderer* renderer, WgrMipFeedbackStats* stats,
                                          uint8_t* out_mip, uint16_t* out_age, uint32_t max_slots);

    /* Push the consolidated ImGui-tweakable render params (tonemap, exposure, sky look,
     * terrain sun-shadow, sky-visibility) in one block. The two terrain setters are diffed
     * renderer-side against the last block, so a per-frame push doesn't thrash the sweep/scan.
     * See docs/render-params-consolidation-plan.md. */
    /* REN-TEMP-001 §6.7 — live temporal/upscaler tuning (dev panel). Not part of the
 * hashed layout set: an older DLL simply lacks the exports. */
typedef struct WgrTemporalTuning
{
    uint32_t temporal_on;
    uint32_t dlss_on;          /* request; effective only on a WGR_DLSS launch */
    uint32_t render_scale_pct; /* 50..100 */
    uint32_t jitter_phases;    /* 0 = auto */
    float mip_bias;            /* > 0 = auto (half log2(scale)); else clamped [-4, 0] */
    float reactive_sky;
    float reactive_water;
    uint32_t flags; /* bit0 auto-exposure, bit1 jitter-Y flip, bit2 MV render-space, bit3 MV flip, bit4 FSR1 on */
    /* RCAS sharpness in stops (0 = sharpest, 2 = mildest). */
    float fsr_sharpness;
} WgrTemporalTuning;

typedef struct WgrTemporalInfo
{
    uint32_t render_width, render_height, output_width, output_height;
    uint32_t temporal_active, dlss_route, dlss_active;
    int32_t dlss_quality;
    float jitter_x, jitter_y;
    float mip_bias_effective;
    uint32_t reset_this_frame;
    /// Active MSAA sample count of the scene targets (1 = off). Startup-fixed; the
    /// menu/panel setting applies on the next launch (WGR_MSAA is the channel).
    uint32_t msaa_samples;
    /* 0 none/native, 1 DLSS, 2 FSR1, 3 bilinear. */
    uint32_t active_upscaler;
} WgrTemporalInfo;

/* SMK-037: live soft-particle state. `enabled` gates the whole renderer-side path -- the
   per-frame scene-depth snapshot is recorded only while it is set, and with it clear any
   batch still carrying WGR_DEPTH_TEST_SOFT draws exactly as WGR_DEPTH_TEST did. `fade_metres`
   is the separation at which a sprite reaches full opacity (clamped 0.05..20).
   Its own entry point rather than a field in an existing struct: growing one of those changes
   a size that is part of the ABI handshake. */
WGR_API void wgr_set_soft_particles(WgrRenderer* renderer, uint32_t enabled, float fade_metres);

/* SMK-038: push the volumetric-smoke tuning (dev panel / env seed). With mode 0 nothing in
   the subsystem runs and nothing is allocated. */
WGR_API void wgr_set_smoke_volume(WgrRenderer* renderer, const WgrSmokeVolumeParams* params);

/* SMK-038: this frame's smoke particles for injection into the froxel field. Same layout as
   the ground-shadow blobs, and the engine fills one array for both. Capped renderer-side
   (2048); wgr_get_smoke_volume_blobs reports how many were ACCEPTED, so a clipped plume is
   visible in the panel rather than silent. Empty is a valid publish (clears the field). */
WGR_API void wgr_set_smoke_volume_blobs(WgrRenderer* renderer, const WgrSmokeShadowBlob* blobs, uint32_t count);

/* SMK-038: particles accepted last publish, in the low 32 bits; frames the march actually
   ran, in the high 32. A counter that can say "switched on and nothing arrived" apart from
   "switched on and marching" -- the distinction a screenshot cannot make. */
WGR_API uint64_t wgr_get_smoke_volume_stats(const WgrRenderer* renderer);

WGR_API void wgr_set_temporal_tuning(WgrRenderer* renderer, const WgrTemporalTuning* tuning);
WGR_API void wgr_get_temporal_info(WgrRenderer* renderer, WgrTemporalInfo* info);

WGR_API void wgr_set_render_params(WgrRenderer* renderer, const WgrRenderParams* params);

    /* AO march resolution: 1 = one GTAO horizon march per pixel, 2 = one per 2x2 block, denoised
       back to full resolution by the AO bilateral filter (which already rejects on full-res depth
       and normals, so it doubles as the upsample). Clamped to 1..=2.
       DEFAULT 2 — the per-pixel march is ~72 depth taps per pixel and measured as the frame's most
       resolution-hungry pass. Its own entry point, not a WgrGtao field, because C++ pushes that
       struct every frame and this default belongs to the renderer; WGR_GTAO_SCALE seeds it. */
    WGR_API void wgr_set_gtao_scale(WgrRenderer* renderer, uint32_t scale);
    WGR_API uint32_t wgr_get_gtao_scale(const WgrRenderer* renderer);

    /* RFG-085: the sky-bake gate. A model's baked sky-visibility volume (256 KB each) is kept
       only when at least `min_enclosed` of its voxels see less than half the sky -- an open
       volume reads as "no volume" in the shader -- and only while the kept total is under
       `budget_mb`. Measured on a 100k-model Everon: 1,024 MB of volumes before the gate,
       256 MB after (1,024 kept, 820 open, 656 over budget). */
    WGR_API void wgr_set_sky_bake_gate(WgrRenderer* renderer, float min_enclosed, uint32_t budget_mb);

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
    /* Photo-card placement grid (m). 1.11 = the map's own clutter grid; lower packs cards
     * denser than the map declares, up to the procedural blade density. Clamped 0.16..4.0. */
    WGR_API void wgr_grass_set_card_spacing(WgrRenderer* renderer, float spacing);
    /* RFG-090: photo-card tone normalisation (WGR_GRASS_CARD_TONE) on/off. Off while the
       clutter atlas is a natively loaded Reforger world's own albedo. */
    WGR_API void wgr_grass_set_card_tone_enabled(WgrRenderer* renderer, uint32_t enabled);
    /* Smoke -> ground shadow: this frame's particles, splatted into the cloud shadow map so
     * every lit surface darkens under a plume. blobs may be null with count 0 to clear. */
    WGR_API void wgr_set_smoke_shadow(WgrRenderer* renderer, const WgrSmokeShadowBlob* blobs, uint32_t count,
                                      float strength);

    /* Second cloud layer (the high cirrus sheet at ~7 km): 0 = off, 1 = flat sheet (one
       ray/sheet intersection), 2 = thin-shell volumetric march. Values above 2 clamp.
       Its own entry point rather than a WgrSkyLook lane for the same reason as the cloud
       shadow above: that struct's size is part of the ABI handshake. Overridable at startup
       with the WGR_CIRRUS environment variable, which is how the two modes get priced from a
       --benchmark run (that runs --no-dev, so the Sky tab is not reachable). */
    WGR_API void wgr_set_cirrus_mode(WgrRenderer* renderer, uint32_t mode);

    /* Second cloud layer SHAPE. `puffiness` 0 = drawn-out fibrous veil (cirrus fibratus) ..
       1 = lumpy cirrocumulus; 0.5 reproduces the look the layer shipped with. `variation` is how
       far it drifts either side of that on its own, on the world's weather-drift clock
       (0 = perfectly steady). Both clamp to 0..1, and neither takes effect instantly — the
       renderer eases toward the value they imply so a slider drag cannot step the sky.
       Overridable at startup with WGR_CIRRUS_PUFF=<puffiness>[,<variation>[,<phase turns>]];
       the phase is how a screenshot capture (no dev panel) selects a point in the variation
       cycle, which is otherwise an hour long and always sampled at the same instant. */
    WGR_API void wgr_set_cirrus_puffiness(WgrRenderer* renderer, float puffiness, float variation);

    /* Second cloud layer LOOK: how MUCH of it there is, and how much it resembles the main deck.

       `amount` 0 = a handful of separated wisps .. 0.5 = exactly the coverage the layer shipped
       with .. 1 = a continuous cirrostratus veil. Coverage is what moves furthest; optical depth
       follows slightly, because a sky filling with cirrus really does thicken as it fills. This
       is the quantity control that wgr_set_cirrus_mode's on/off was not.

       `matchDeck` 0 = high ice cloud (the layer as it has always been) .. 1 = as close to the
       CUMULUS DECK below as a shell march gets: the shell descends to just above the deck's top,
       feature size follows the deck's own shape scale, the shear stretch and fibre striation
       relax to isotropic cells, the shell deepens, optical depth rises, the ice halo phase gives
       way to the deck's, and a cheap vertical self-shading term appears so the result reads as
       solid cloud rather than glowing fog. Every target is read from the deck's LIVE parameters,
       so "similar" tracks whatever the weather is doing; the layer is clamped so it can never
       sink into or below the deck, because then it would stop being a second layer.

       Both clamp to 0..1 and both are eased by the renderer (matchDeck moves the layer's altitude
       by kilometres, so an un-eased drag would teleport a cloud deck across the sky). Same
       reasoning as wgr_set_cirrus_mode for having its own entry point. Overridable at startup
       with WGR_CIRRUS_LOOK=<amount>[,<match>], for --no-dev benchmark and capture runs. */
    WGR_API void wgr_set_cirrus_look(WgrRenderer* renderer, float amount, float matchDeck);

    /* SECOND cloud layer EDGE SOFTNESS (0 .. 1).

       0 = the layer exactly as it rendered before this existed: the sheet noise is remapped
       linearly about the coverage threshold into a hard clamp, so a cloud reaches useful density
       within a short distance of its boundary and has a definite silhouette. 1 = density is held
       down through a much wider band either side of that threshold, so the transmittance falls off
       over a longer path and the cloud gains a deep, soft fringe. High cloud is ice, and its edges
       sublimate rather than condense, so a fringe is what it should have.

       WEIGHTED BY LOCAL COVERAGE, so it reaches the big merged banks and leaves isolated wisps
       defined. The layer's mean density is restored as the exponent rises, so this stays a shape
       control and does not double as a brightness one.

       Overridable at startup with WGR_CIRRUS_SOFT=<0..1>. */
    WGR_API void wgr_set_cirrus_softness(WgrRenderer* renderer, float softness);

    /* Fraction of the draw distance at which distance fog reaches FULL (0.3 .. 1.0).

       1.0 is the behaviour from before this existed: the aerial-perspective ramp is pinned to
       reach 1.0 exactly AT the far plane and nowhere before it. That is also exactly where the
       terrain grid stops, where Scene::GetObjectDrawDistance puts the object cull ring, and where
       the map itself ends -- so anything still visible there reads as the world being cut off.
       Lower values move full fog inward, so everything from there out is uniformly the sky's own
       airlight and there is no edge left to see.

       NOT the same control as the fog falloff exponent in WgrSkyLook: that shapes how fast the
       ramp climbs, so lowering it to thicken the far field also thickens the near and mid field.
       This one only ever adds coverage at the far end, which is why it cannot reintroduce the
       object pop-in that anchoring the cull to the fog range was there to fix. */
    WGR_API void wgr_set_fog_far_close(WgrRenderer* renderer, float close);

    /* Road / decal per-pixel depth conform. `flat` is a fixed along-ray lift in metres,
     * `per_m` the same per metre of distance BEFORE the grazing division, `max_frac` the
     * ceiling as a fraction of the distance. 0 in any field = the shipped default. Rides the
     * reserved fogfar lanes, so no struct layout and no ABI version moves. */
    WGR_API void wgr_set_road_conform(WgrRenderer* renderer, float flat, float per_m, float max_frac);

    /* Volumetric sun shafts (crepuscular rays / god rays).

       A WORLD-SPACE march, not a screen-space radial blur. Each low-resolution view ray is walked
       and asked "is the sun visible from here", against three maps that already exist and are
       rebuilt every frame: the CLD-020 cloud sun-transmittance map (so a cloud bank crossing the
       sun reshapes the shafts — the point of the feature), the long-range terrain shadow-ceiling
       mask, and the cascade shadow map (buildings, tree crowns). No new geometry or shadow pass.

       Consequences of being world-space rather than screen-space, both of which are requirements
       and not accidents: the sun's SCREEN position never enters the shader, so an off-screen sun
       produces no edge halo; and the fade-out below the horizon / behind the camera comes from the
       sun-elevation ramp and the forward-peaked phase function rather than a special case.

       `density` = scattering coefficient, 1/m (the engine's clear-day Mie is 6e-6).
       `distance` = march cap, metres. `g` = Henyey-Greenstein anisotropy (higher = tighter beam).
       `cloudInfluence` = how strongly the deck shapes the shafts; 0 = geometry only, for A/B.
       `steps` and `resDiv` are the cost knobs — `resDiv` is the screen-resolution divisor for the
       march target (2 = half in each axis, the default). Everything clamps.

       Composited into the LINEAR HDR scene before bloom / auto-exposure / tonemap, so shafts
       bloom and roll off like any other light; there is no effect on the LDR-direct path.

       Own entry point rather than a WgrSkyLook lane, for the reason given on wgr_set_cirrus_mode:
       that struct's size is part of the ABI handshake. Overridable at startup with
       WGR_GODRAYS=<enabled>[,<intensity>[,<density x1e-6>[,<steps>[,<res divisor>]]]] — which is
       how a --no-dev capture ablates it, since the Sky tab is not reachable there. */
    WGR_API void wgr_set_god_rays(WgrRenderer* renderer, uint32_t enabled, float intensity, float density,
                                  float distance, float g, float cloudInfluence, uint32_t steps, uint32_t resDiv);

    /* Material Debug state for the RETAINED (GPU-driven) path — the dev panel's
     * Materials tab. `view`: 0 full material, 1 base colour only, 2 decoded normal.
     * `flags`: bit0 disable normal map, bit1 invert normal Y, bit2 compose Multi
     * layers (SET = compose, which is the default).
     *
     * Presentation switches, read per fragment. They are deliberately NOT baked
     * into the material records at registration: doing that is why the toggles
     * appeared dead on a world that was already loaded, since the retained set
     * resolves its textures once and nothing re-registers a live instance. */
    WGR_API void wgr_set_material_debug(WgrRenderer* renderer, uint32_t view, uint32_t flags);

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

// --- Cross-language layout hash ----------------------------------------------
//
// FNV-1a over the size of every struct that crosses the C ABI, in a fixed order
// that rust/src/ffi.rs::wgr_layout_hash() repeats exactly. The two sides compute
// it independently from their OWN definitions, so the number only agrees when the
// two definitions agree.
//
// This lives here (not in the static_assert block above) because it must be
// declared after WgrTerrainUv / WgrTerrainMaterial, which are declared inside the
// extern "C" block for proximity to wgr_terrain_set_materials.
//
// What it catches: any struct that changes SIZE on one side only — which is what
// every field addition to these blocks has been. What it does not catch: a
// same-size field reorder (swap two floats). For the sky UBO that gap is covered
// separately by sky/mod.rs's field-ORDER test against the WGSL.
//
// Adding a new shared struct? Append it to BOTH lists. Appending (rather than
// inserting) is not required for correctness — the two lists only have to match
// each other — but it keeps diffs readable.
constexpr uint32_t WgrHashSize(uint32_t h, uint32_t size)
{
    for (int i = 0; i < 4; ++i)
    {
        h ^= (size >> (i * 8)) & 0xFFu;
        h *= 16777619u; // FNV-1a 32-bit prime; unsigned wraparound is well-defined
    }
    return h;
}

constexpr uint32_t WgrLayoutHash()
{
    uint32_t h = 2166136261u; // FNV-1a 32-bit offset basis
    h = WgrHashSize(h, sizeof(WgrVertex2D));
    h = WgrHashSize(h, sizeof(WgrDraw2DBatch));
    h = WgrHashSize(h, sizeof(WgrMeshVertex));
    h = WgrHashSize(h, sizeof(WgrDraw3D));
    h = WgrHashSize(h, sizeof(WgrLight));
    h = WgrHashSize(h, sizeof(WgrModelSection));
    h = WgrHashSize(h, sizeof(WgrModelMaterial));
    h = WgrHashSize(h, sizeof(WgrModelLod));
    h = WgrHashSize(h, sizeof(WgrInstance));
    h = WgrHashSize(h, sizeof(WgrTonemap));
    h = WgrHashSize(h, sizeof(WgrExposure));
    h = WgrHashSize(h, sizeof(WgrSky));
    h = WgrHashSize(h, sizeof(WgrSkyLook));
    h = WgrHashSize(h, sizeof(WgrSkyRuntime));
    h = WgrHashSize(h, sizeof(WgrTerrainSunShadow));
    h = WgrHashSize(h, sizeof(WgrTerrainUv));
    h = WgrHashSize(h, sizeof(WgrTerrainMaterial));
    h = WgrHashSize(h, sizeof(WgrSkyVisibility));
    h = WgrHashSize(h, sizeof(WgrFoliage));
    h = WgrHashSize(h, sizeof(WgrGtao));
    h = WgrHashSize(h, sizeof(WgrSkyVis));
    h = WgrHashSize(h, sizeof(WgrDepthOfField));
    h = WgrHashSize(h, sizeof(WgrRenderParams));
    h = WgrHashSize(h, sizeof(WgrFrameParams));
    h = WgrHashSize(h, sizeof(WgrCameraShadow));
    h = WgrHashSize(h, sizeof(WgrCamera));
    h = WgrHashSize(h, sizeof(WgrShadowCaster));
    h = WgrHashSize(h, sizeof(WgrShadowPass));
    h = WgrHashSize(h, sizeof(WgrCmd));
    h = WgrHashSize(h, sizeof(WgrOverlayVertex));
    h = WgrHashSize(h, sizeof(WgrOverlayDraw));
    h = WgrHashSize(h, sizeof(WgrTerrainParams));
    h = WgrHashSize(h, sizeof(WgrTerrainNode));
    h = WgrHashSize(h, sizeof(WgrTerrainBatch));
    h = WgrHashSize(h, sizeof(WgrGrassBatch));
    h = WgrHashSize(h, sizeof(WgrGrassTrack));
    h = WgrHashSize(h, sizeof(WgrGrassDownwash));
    h = WgrHashSize(h, sizeof(WgrGrassParams));
    h = WgrHashSize(h, sizeof(WgrWaterParams));
    h = WgrHashSize(h, sizeof(WgrRainWaterParams));
    h = WgrHashSize(h, sizeof(WgrRainWaterSourceKey));
    h = WgrHashSize(h, sizeof(WgrRainWaterFineCell));
    h = WgrHashSize(h, sizeof(WgrRainWaterPublication));
    h = WgrHashSize(h, sizeof(WgrWaterNode));
    h = WgrHashSize(h, sizeof(WgrWaterBatch));
    h = WgrHashSize(h, sizeof(WgrWaterInteractionEvent));
    h = WgrHashSize(h, sizeof(WgrWaterInteractionParams));
    h = WgrHashSize(h, sizeof(WgrSurfaceDesc));
    h = WgrHashSize(h, sizeof(WgrLogCallbacks));
    h = WgrHashSize(h, sizeof(WgrFrame));
    h = WgrHashSize(h, sizeof(WgrFarInstance));
    // An OUT struct the engine allocates and the DLL fills. It was missing from this set, so
    // a size disagreement between exe and DLL would have been written past this side's buffer
    // with the handshake reporting no problem -- a silent mismatch that corrupts the caller.
    h = WgrHashSize(h, sizeof(WgrObjectStats));
    h = WgrHashSize(h, sizeof(WgrSmokeVolumeParams)); // SMK-038
    h = WgrHashSize(h, sizeof(WgrMemoryStats));
    h = WgrHashSize(h, sizeof(WgrGeometryAllocationRow));
    h = WgrHashSize(h, sizeof(WgrGeometryAllocationSummary));
    h = WgrHashSize(h, sizeof(WgrLodDemandRow));
    h = WgrHashSize(h, sizeof(WgrLodDemandReport));
    h = WgrHashSize(h, sizeof(WgrMeshHandleFactSummary));
    h = WgrHashSize(h, sizeof(WgrLayeredFog)); // ABI21: append-only standalone shared config
    return h;
}

#endif // WGPU_RENDERER_HPP
