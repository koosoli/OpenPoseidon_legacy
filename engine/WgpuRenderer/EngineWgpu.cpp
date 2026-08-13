#include "EngineWgpu.hpp"
#include "TerrainWgpu.hpp"
#include "WaterWgpu.hpp"
#include "TextureBankWgpu.hpp"

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Debug/DebugOverlay.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Platform/GamePaths.hpp>
#include <Poseidon/Graphics/Core/MeshVertex.hpp>
#include <Poseidon/Graphics/Core/ZBiasMath.hpp>
#include <Poseidon/Graphics/Shared/SdlWindow.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Draw2DGeometry.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/MeshBuild.hpp>
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/RenderFlags.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp> // TexMaterial::Combine (GPU-driven material extract)
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ClipShape.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/MapTypes.hpp>                         // MapBush (spherical/canopy-normal flagging)
#include <Poseidon/World/Scene/Object.hpp>                     // Object accessors (GPU-driven retained scene)
#include <Poseidon/World/World.hpp>                            // live player/vehicle interaction for grass
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp> // rotor wash for grass
#include <Poseidon/World/Scene/ObjectClasses.hpp>              // ForestPlain (mode-1 conform exclusion)
#include <Poseidon/World/Terrain/Landscape.hpp>                // GLandscape->SurfaceY (GPU-driven conform bcSurfaceY)
#include <Poseidon/World/Weather/WindModel.hpp>                // the one wind authority (grass sway, cloud drift)
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>
#include <Poseidon/Graphics/Shared/ScreenshotWriter.hpp>
#include <Poseidon/Graphics/Textures/ProceduralTexture.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Foundation/Types/Memtype.h> // DWORD

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>

namespace Poseidon
{

// Engine's built-in software occlusion-buffer cull (Scene.cpp). GPU Hi-Z occlusion replaces it
// for the retained set, so it is forced off while GPU occlusion is active (see PushSceneCamera).
extern bool EnableObjOcc;

namespace
{

// Defined with the retained-section classifier below. The direct path needs the
// same authored TreeAdv alpha authority before it decides its per-pixel discard.
bool GpuSectionMaterialUsesLeafCards(const Ref<TexMaterial>& surfMat);
bool GpuSectionMaterialIsCutout(const Ref<TexMaterial>& surfMat, TextureWgpu* texture);
bool GpuTextureIsBarkOrTrunk(const TextureWgpu* texture);

// Narrowing conversion to the 32-bit width the FFI uses for counts / indices.
template <typename T>
constexpr uint32_t U32(T value)
{
    return uint32_t(value);
}

WgrSlice<WgrMeshVertex> AsMeshVerts(const std::vector<SVertex>& v)
{
    return {reinterpret_cast<const WgrMeshVertex*>(v.data()), U32(v.size())};
}

bool ContainsNoCase(const char* text, const std::string& part)
{
    if (!text || part.empty())
        return false;
    std::string haystack(text);
    std::string needle(part);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    return haystack.find(needle) != std::string::npos;
}

// Small imported lamp meshes use an emissive material as a legacy visibility marker. In HDR the
// same value is physical radiance and blooms into a sun-sized disc. Their physical illumination
// comes from paired static marker lights; keep the mesh itself at compact visible radiance.
// Neither value affects the paired point light's terrain spill.
//
// This is a TARGET PEAK, not a multiplier. Arma 2 and 3 author the emissive lane on a legacy
// 1e2..1e6 scale -- measured across data_f's 66 light materials, `lamp_searchlight.rvmat`
// (the material every A3 floodlight actually emits through) is 600000, and half the family
// sits above 1e4. No absolute multiplier can bring that into HDR range: even 600000 * 0.015
// overflows the f16 scene target and clears the 1.0 bloom knee by orders of magnitude. The
// retained path already clamps by peak (gpu_driven.wgsl, emissive * min(1, 0.50/peak)); this
// gives the direct path the same shape, per family.
float StaticEmitterRadianceTarget(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return 1.0f;
    const char* path = surfMat->GetName().Data();
    // The lamp HOUSINGS (`\lamps\`, `lamphalogen`) all measure emissive 0, so this arm only
    // ever bites through the searchlight lens the fixtures share.
    if (ContainsNoCase(path, "\\lamps\\") || ContainsNoCase(path, "lamphalogen") ||
        ContainsNoCase(path, "lamp_searchlight"))
        return 0.015f;
    return ContainsNoCase(path, "\\lights\\") || ContainsNoCase(path, "\\runwaylights\\") ||
                   ContainsNoCase(path, "warninglight")
               ? 0.08f
               : 1.0f;
}

// Scale that lands a material's emissive at its family's target peak. Materials already at or
// under the target are untouched -- which is every stock CWA material, all of whose CfgMaterials
// emissive values measure {0,0,0,0}.
float StaticEmitterRadianceScale(const Ref<TexMaterial>& surfMat, const Color& emissive)
{
    const float target = StaticEmitterRadianceTarget(surfMat);
    const float peak = std::max({emissive.R(), emissive.G(), emissive.B()});
    return peak > target ? target / peak : 1.0f;
}

// By-eye tonemap/grade presets keyed by time of day (hours). Sorted ascending; the
// day is interpolated between adjacent keys and clamped outside the range (night
// keys TBD — need eye adaptation + procedural sky). Exposure/gain seeded from the
// user's captured curve presets; the grade block starts neutral and is re-tuned via
// the ImGui Tonemap tab's copy-back. See engine/WgpuRenderer/docs/hdr-pipeline-plan.md.
struct TonemapKey
{
    float hour;
    Engine::TonemapSettings s;
};
// Fields: exposure, temperature, tint, contrast, saturation, lift, gain, hable, encode.
// Tuned by eye via the ImGui Tonemap tab (copy-back), 2026-07-07. The night preset is
// duplicated at 2:00 and 20:00 so the [first,last]-clamping interpolation wraps midnight
// (00:00-02:00 clamps to the 2:00 night key; 20:00-24:00 clamps to the 20:00 night key).
const TonemapKey kTonemapPresets[] = {
    {2.0f, {3.625f, 0.031f, 0.145f, 1.0f, 0.939f, 0.0f, 1.029f, true, true}},   // night
    {5.833f, {2.512f, 0.258f, 0.056f, 1.0f, 1.083f, 0.0f, 2.758f, true, true}}, // 5:50 dawn
    {12.0f, {3.625f, 0.045f, 0.040f, 1.0f, 1.119f, 0.0f, 0.945f, true, true}},  // noon
    {17.0f, {3.625f, 0.150f, 0.109f, 1.0f, 1.066f, 0.0f, 1.016f, true, true}},  // 17:00
    {18.5f, {3.625f, 0.107f, 0.145f, 1.0f, 1.142f, 0.0f, 1.061f, true, true}},  // 18:30 dusk
    {20.0f, {3.625f, 0.031f, 0.145f, 1.0f, 0.939f, 0.0f, 1.029f, true, true}},  // night
};

// By-eye procedural-sky atmosphere presets keyed by time of day (hours), interpolated
// like the tonemap keys. Only the fields the Sky tab captures vary; everything else
// (heights, radii, samples, night colours, band, and the user toggles) keeps its
// SkySettings default and is preserved live by UpdateAutoSky. rayleigh/mie are the raw
// 1/m coeffs (the Sky-tab print shows them x1e6). Night preset duplicated at 2:00/20:00
// to wrap midnight, same as the tonemap table. Tuned 2026-07-07.
struct SkyKey
{
    float hour;
    Engine::SkySettings s;
};
const SkyKey kSkyPresets[] = {
    {2.0f,
     {.rayleigh = {1.99e-6f, 9.08e-6f, 29.94e-6f},
      .mie = 20.94e-6f,
      .mieG = 0.752f,
      .turbidity = 2.44f,
      .ozone = 3.73f,
      .sunAngularRadius = 0.0051f,
      .sunIntensity = 2.07f,
      .exposure = 0.272f,
      .nightIntensity = 0.0002f}}, // night
    {5.833f,
     {.rayleigh = {5.80e-6f, 13.50e-6f, 33.10e-6f},
      .mie = 16.18e-6f,
      .mieG = 0.760f,
      .turbidity = 2.56f,
      .ozone = 1.55f,
      .sunAngularRadius = 0.0070f,
      .sunIntensity = 4.01f,
      .exposure = 0.329f,
      .nightIntensity = 0.0005f}}, // 5:50 dawn
    {12.0f,
     {.rayleigh = {5.80e-6f, 14.13e-6f, 48.89e-6f},
      .mie = 22.42e-6f,
      .mieG = 0.760f,
      .turbidity = 2.44f,
      .ozone = 1.00f,
      .sunAngularRadius = 0.0070f,
      .sunIntensity = 24.74f,
      .exposure = 0.909f,
      .nightIntensity = 0.0005f}}, // noon
    {17.0f,
     {.rayleigh = {1.99e-6f, 9.08e-6f, 48.89e-6f},
      .mie = 20.94e-6f,
      .mieG = 0.857f,
      .turbidity = 2.44f,
      .ozone = 1.55f,
      .sunAngularRadius = 0.0183f,
      .sunIntensity = 24.74f,
      .exposure = 0.909f,
      .nightIntensity = 0.0005f}}, // 17:00
    {18.5f,
     {.rayleigh = {1.99e-6f, 9.08e-6f, 29.94e-6f},
      .mie = 20.94e-6f,
      .mieG = 0.752f,
      .turbidity = 2.44f,
      .ozone = 3.73f,
      .sunAngularRadius = 0.0173f,
      .sunIntensity = 8.26f,
      .exposure = 0.385f,
      .nightIntensity = 0.0005f}}, // 18:30 dusk
    {20.0f,
     {.rayleigh = {1.99e-6f, 9.08e-6f, 29.94e-6f},
      .mie = 20.94e-6f,
      .mieG = 0.752f,
      .turbidity = 2.44f,
      .ozone = 3.73f,
      .sunAngularRadius = 0.0051f,
      .sunIntensity = 2.07f,
      .exposure = 0.272f,
      .nightIntensity = 0.0002f}}, // night
};

float LerpF(float a, float b, float t)
{
    return a + (b - a) * t;
}

// CPU port of sky.wgsl's transmittance integral, for a single ray from the camera altitude
// toward the sun: per-channel atmospheric transmittance (0 = sun below the horizon / fully
// occluded by the planet). Feeds sky-based scene lighting so the surface sun colour reddens
// at dusk and fades to zero at night, on the same physical radiance scale as the sky/fog.
Vector3 AtmosphereSunTransmittance(const Engine::SkySettings& s, float camAlt, Vector3 dirToSun)
{
    const float Rg = s.planetRadius;
    const float Rt = Rg + s.atmosphereHeight;
    const Vector3 origin(0.0f, Rg + (camAlt > 0.0f ? camAlt : 0.0f), 0.0f);
    dirToSun.Normalize();

    // near/far parametric hits of a sphere centred at the planet origin (miss -> returns false).
    auto raySphere = [](const Vector3& o, const Vector3& d, float r, float& t0, float& t1) -> bool
    {
        const float b = o.DotProduct(d);
        const float c = o.DotProduct(o) - r * r;
        const float disc = b * b - c;
        if (disc < 0.0f)
            return false;
        const float sq = std::sqrt(disc);
        t0 = -b - sq;
        t1 = -b + sq;
        return true;
    };

    float g0 = 0.0f, g1 = 0.0f;
    if (raySphere(origin, dirToSun, Rg, g0, g1) && g0 > 0.0f)
        return Vector3(0.0f, 0.0f, 0.0f); // planet-shadowed: sun is below the local horizon

    float a0 = 0.0f, a1 = 0.0f;
    if (!raySphere(origin, dirToSun, Rt, a0, a1))
        return Vector3(1.0f, 1.0f, 1.0f);
    const float tMax = a1;

    const int STEPS = 32;
    const float atmosH = s.atmosphereHeight;
    float odR = 0.0f, odM = 0.0f, odO = 0.0f, t = 0.0f;
    for (int i = 0; i < STEPS; i++)
    {
        const float nt = ((i + 0.5f) / STEPS) * tMax;
        const float dt = nt - t;
        t = nt;
        const Vector3 p = origin + dirToSun * t;
        const float alt = p.Size() - Rg;
        odR += std::exp(-alt / s.rayleighHeight) * dt;
        odM += std::exp(-alt / (s.mieHeight > 1.0f ? s.mieHeight : 1.0f)) * dt;
        const float ozo = 1.0f - std::fabs(alt - atmosH * 0.417f) / (atmosH * 0.25f);
        odO += (ozo > 0.0f ? ozo : 0.0f) * dt;
    }
    // Mie extinction ~1.11x scattering; Earth ozone absorption (1/m at peak). Matches sky.wgsl.
    const float MIE_EXT = 1.11f;
    const float ozoneAbs[3] = {0.650e-6f, 1.881e-6f, 0.085e-6f};
    const float tauR = s.rayleigh[0] * odR + s.mie * MIE_EXT * odM + ozoneAbs[0] * odO * s.ozone;
    const float tauG = s.rayleigh[1] * odR + s.mie * MIE_EXT * odM + ozoneAbs[1] * odO * s.ozone;
    const float tauB = s.rayleigh[2] * odR + s.mie * MIE_EXT * odM + ozoneAbs[2] * odO * s.ozone;
    return Vector3(std::exp(-tauR), std::exp(-tauG), std::exp(-tauB));
}

Engine::TonemapSettings LerpTonemap(const Engine::TonemapSettings& a, const Engine::TonemapSettings& b, float t)
{
    Engine::TonemapSettings r;
    r.exposure = LerpF(a.exposure, b.exposure, t);
    r.temperature = LerpF(a.temperature, b.temperature, t);
    r.tint = LerpF(a.tint, b.tint, t);
    r.contrast = LerpF(a.contrast, b.contrast, t);
    r.saturation = LerpF(a.saturation, b.saturation, t);
    r.lift = LerpF(a.lift, b.lift, t);
    r.gain = LerpF(a.gain, b.gain, t);
    r.hable = (t < 0.5f ? a : b).hable;
    r.encode = (t < 0.5f ? a : b).encode;
    return r;
}

Engine::TonemapSettings TonemapAtHour(float hour)
{
    const int n = int(sizeof(kTonemapPresets) / sizeof(kTonemapPresets[0]));
    if (hour <= kTonemapPresets[0].hour)
        return kTonemapPresets[0].s;
    if (hour >= kTonemapPresets[n - 1].hour)
        return kTonemapPresets[n - 1].s;
    for (int i = 0; i + 1 < n; ++i)
    {
        const TonemapKey& k0 = kTonemapPresets[i];
        const TonemapKey& k1 = kTonemapPresets[i + 1];
        if (hour >= k0.hour && hour < k1.hour)
            return LerpTonemap(k0.s, k1.s, (hour - k0.hour) / (k1.hour - k0.hour));
    }
    return kTonemapPresets[n - 1].s;
}

// Interpolate only the atmosphere fields the Sky-tab keyframes vary; every other field
// keeps `a`'s value (all keys share the same constants, so this is exact for them). The
// user toggles (skyLighting/ambient/aerialShadow/fogFalloff/enabled/samples) are preserved
// separately in UpdateAutoSky, so they are intentionally not touched here.
Engine::SkySettings LerpSky(const Engine::SkySettings& a, const Engine::SkySettings& b, float t)
{
    Engine::SkySettings r = a;
    r.rayleigh[0] = LerpF(a.rayleigh[0], b.rayleigh[0], t);
    r.rayleigh[1] = LerpF(a.rayleigh[1], b.rayleigh[1], t);
    r.rayleigh[2] = LerpF(a.rayleigh[2], b.rayleigh[2], t);
    r.mie = LerpF(a.mie, b.mie, t);
    r.mieG = LerpF(a.mieG, b.mieG, t);
    r.turbidity = LerpF(a.turbidity, b.turbidity, t);
    r.ozone = LerpF(a.ozone, b.ozone, t);
    r.sunAngularRadius = LerpF(a.sunAngularRadius, b.sunAngularRadius, t);
    r.sunIntensity = LerpF(a.sunIntensity, b.sunIntensity, t);
    r.exposure = LerpF(a.exposure, b.exposure, t);
    r.nightIntensity = LerpF(a.nightIntensity, b.nightIntensity, t);
    return r;
}

Engine::SkySettings SkyAtHour(float hour)
{
    const int n = int(sizeof(kSkyPresets) / sizeof(kSkyPresets[0]));
    if (hour <= kSkyPresets[0].hour)
        return kSkyPresets[0].s;
    if (hour >= kSkyPresets[n - 1].hour)
        return kSkyPresets[n - 1].s;
    for (int i = 0; i + 1 < n; ++i)
    {
        const SkyKey& k0 = kSkyPresets[i];
        const SkyKey& k1 = kSkyPresets[i + 1];
        if (hour >= k0.hour && hour < k1.hour)
            return LerpSky(k0.s, k1.s, (hour - k0.hour) / (k1.hour - k0.hour));
    }
    return kSkyPresets[n - 1].s;
}

void WgrLogThunk(int32_t level, const char* msg, void* /*user*/)
{
    if (!msg)
    {
        return;
    }
    switch (level)
    {
        case WGR_LOG_TRACE:
        case WGR_LOG_DEBUG:
            LOG_DEBUG(Graphics, "wgpu: {}", msg);
            break;
        case WGR_LOG_WARN:
            LOG_WARN(Graphics, "wgpu: {}", msg);
            break;
        case WGR_LOG_ERROR:
            LOG_ERROR(Graphics, "wgpu: {}", msg);
            break;
        case WGR_LOG_INFO:
        default:
            LOG_INFO(Graphics, "wgpu: {}", msg);
            break;
    }
}

void DescribeSurface(SDL_Window* window, WgrSurfaceDesc& desc)
{
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    desc.window = nullptr;
    desc.display = nullptr;
#ifdef _WIN32
    desc.platform = WGR_PLATFORM_WIN32;
    desc.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && std::strcmp(driver, "wayland") == 0)
    {
        desc.platform = WGR_PLATFORM_WAYLAND;
        desc.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        desc.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    }
    else
    {
        desc.platform = WGR_PLATFORM_XLIB;
        desc.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        const Sint64 xid = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        desc.window = reinterpret_cast<void*>(static_cast<uintptr_t>(xid));
    }
#endif
}

// Reserved palette index for unweighted vertices
constexpr int kReservedBone = WGR_PALETTE_SIZE - 1;

// FNV-1a 64 over raw bytes; used to detect unchanged vertex data so a redundant
// GPU re-upload can be skipped. Cheap relative to the staging copy + barrier it
// avoids, and only paid on meshes whose Update trigger already fires.
inline uint64_t HashVertices(const void* data, size_t bytes)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < bytes; i++)
    {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

class VertexBufferWgpu : public VertexBuffer
{
  public:
    WgrRenderer* renderer = nullptr;
    uint64_t mesh = 0;
    int vertexCount = 0;
    bool isDynamic = false;
    AutoArray<render::mesh::MeshSection> sections;

    // Content hash of the last vertex data actually uploaded, so Update can skip a
    // redundant GPU copy when the rebuilt vertices are byte-identical. VBDynamic
    // shapes (GetAllowAnimation) re-upload every frame they draw, and conformed
    // on-surface objects (roads/paths) dirty their buffer every frame in
    // Object::Animate even though their terrain-conformed geometry is static — both
    // produced a per-frame `wgr_3d_vbuf` staging copy + barrier for unchanged data.
    uint64_t lastUploadHash = 0;
    bool haveUploadHash = false;

    // GPU skinning state
    // `skinned` flips on once SetSkinData has uploaded the bind pose + weights
    // `palette` holds the current frame's model-space bone matrices
    bool skinned = false;
    int paletteCount = 0;
    std::vector<Matrix4> palette;

    VertexBufferWgpu(WgrRenderer* r, uint64_t m, int nv, bool dynamic)
        : renderer(r), mesh(m), vertexCount(nv), isDynamic(dynamic)
    {
    }
    ~VertexBufferWgpu() override
    {
        if (renderer && mesh)
        {
            wgr_mesh_destroy(renderer, mesh);
        }
    }

    // Re-upload vertex data when the shape's geometry actually changed. `bufferDirty`
    // is the canonical mutation signal (InvalidateBuffer, raised by the CPU Animate
    // deform); `dynamic` is a caller-forced refresh; the first upload always runs.
    // Skinned meshes stay at bind pose (the GPU transforms them), so Update is a no-op.
    //
    // A clean, already-uploaded buffer is skipped in O(1) — no rebuild, no hash. This is
    // the key: `isDynamic` (VBDynamic, every GetAllowAnimation shape) used to force a full
    // rebuild + FNV content-hash EVERY frame, even when nothing changed. That was added to
    // suppress a per-instance GPU upload storm, but once terrain-conformed vegetation moved
    // to GPU conform it stopped deforming on the CPU — so its shared mesh never re-dirties,
    // yet it was still rebuilt and rehashed every frame (the dominant per-frame CPU cost in
    // profiling) to avoid a sub-millisecond upload. Trusting bufferDirty removes that waste.
    // The hash below still guards the genuinely-dirty-but-unchanged case (a CPU-deformed
    // mesh that re-derives byte-identical geometry), skipping just the GPU copy.
    void Update(const Shape& src, bool dynamic) override
    {
        if (skinned || !renderer || !mesh)
        {
            return;
        }
        // Skip when the geometry hasn't changed since it was last made current. bufferDirty
        // (InvalidateBuffer) is raised by EVERY vertex-mutating path (Object deform,
        // Animation, RtAnimation), so it is the complete change signal. `isDynamic` and the
        // `dynamic` param are only "this buffer MAY animate" hints (matSource->GetAnimated),
        // true every frame for vegetation — they must NOT force a rebuild, or conformed veg
        // (whose CPU deform is skipped, so it never re-dirties) rebuilds + hashes identical
        // vertices every frame. A static buffer is filled at creation and can't be dirtied
        // (B-028); a dynamic one is current after its first Update. Either way a clean
        // buffer needs nothing. `dynamic` is intentionally ignored (it does not mean "the
        // verts changed" — only bufferDirty does).
        if (!bufferDirty && (!isDynamic || haveUploadHash))
        {
            return;
        }
        if (src.NVertex() != vertexCount)
        {
            return;
        }
        std::vector<SVertex> verts(static_cast<size_t>(vertexCount));
        // A terrain-conform plane is active only inside a conformed object's draw
        // (ForestPlain). Then upload the UNDEFORMED mesh (OrigPos/OrigNorm) and let the
        // vertex shader conform each instance: the base bytes are identical across all
        // instances of the shape, so the content-hash below collapses what used to be a
        // per-instance upload storm to a single copy. Non-conformed draws are unchanged.
        if (GCurrentConformPlane.active && src.OriginalPosValid())
        {
            render::mesh::BuildOrigVertices(src, verts.data());
        }
        else
        {
            render::mesh::BuildVertices(src, verts.data());
        }
        bufferDirty = false;
        const uint64_t hash = HashVertices(verts.data(), verts.size() * sizeof(SVertex));
        if (haveUploadHash && hash == lastUploadHash)
        {
            return;
        }
        lastUploadHash = hash;
        haveUploadHash = true;
        wgr_mesh_update(renderer, mesh, AsMeshVerts(verts));
    }

    // One-time: upload the bind pose (OrigPos/OrigNorm) + per-vertex bone indices
    // and weights, marking the mesh eligible for the skinned pipeline.
    void SetSkinData(const AnimationRTWeights& weightTable, const Shape& bindShape) override
    {
        if (skinned || !renderer || !mesh || bindShape.NVertex() != vertexCount)
        {
            return;
        }

        std::vector<SVertex> verts(static_cast<size_t>(vertexCount));
        std::vector<uint8_t> bones(static_cast<size_t>(vertexCount) * 4, 0);
        std::vector<uint8_t> weights(static_cast<size_t>(vertexCount) * 4, 0);
        const AnimationRTWeight* wData = weightTable.Data();
        for (int i = 0; i < vertexCount; i++)
        {
            const Vector3 p = bindShape.OrigPos(i);
            const Vector3 n = bindShape.OrigNorm(i);
            verts[i].pos = Vector3P(p.X(), p.Y(), p.Z());
            verts[i].norm = Vector3P(-n.X(), -n.Y(), -n.Z());
            verts[i].t0 = bindShape.UV(i);

            const AnimationRTWeight& w = wData[i];
            const int n4 = w.Size() < 4 ? w.Size() : 4;
            if (n4 <= 0)
            {
                // Unweighted: full weight on the reserved (world-only) bone
                bones[i * 4] = kReservedBone;
                weights[i * 4] = 255;
                continue;
            }
            for (int k = 0; k < n4; k++)
            {
                bones[i * 4 + k] = static_cast<uint8_t>(w[k].GetSel());
                // Scale weights from 0...1 to 0...255
                int q = static_cast<int>(w[k].GetWeight() * 255.0f + 0.5f);
                weights[i * 4 + k] = static_cast<uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
            }
        }

        wgr_mesh_update(renderer, mesh, AsMeshVerts(verts));
        wgr_mesh_set_skin(renderer, mesh, bones, weights);
        skinned = true;
    }

    void SetPalette(const Matrix4* mats, int count) override
    {
        if (count > kReservedBone)
        {
            count = kReservedBone; // never overwrite the reserved slot
        }
        paletteCount = count;
        palette.assign(mats, mats + count);
    }
};

} // namespace

EngineWgpu::EngineWgpu(const GraphicsEngineParams& params) : _windowed(params.useWindow)
{
    // Neutral default material (TLMaterial's ctor only initialises specular), so a
    // draw reaching DrawSectionTL without a preceding SetMaterial lights sanely.
    _curMaterial.emmisive = HBlack;
    _curMaterial.ambient = HWhite;
    _curMaterial.diffuse = HWhite;
    _curMaterial.forcedDiffuse = HBlack;

    SdlGameWindowDesc wd;
    wd.title = "Poseidon [WGPU]";
    wd.width = params.width;
    wd.height = params.height;
    wd.useWindow = params.useWindow;
    wd.displayMode = params.displayMode;
    const SdlGameWindow win = CreateGameWindow(wd);
    if (!win.window)
    {
        return;
    }

    _window = win.window;
    _w = win.widthPx;
    _h = win.heightPx;
    _windowed = win.windowed;

    WgrSurfaceDesc desc{};
    DescribeSurface(_window, desc);
    desc.width = U32(_w > 0 ? _w : 1);
    desc.height = U32(_h > 0 ? _h : 1);

    const WgrLogCallbacks log{&WgrLogThunk, nullptr};
    LOG_INFO(Graphics, "Wgpu: creating renderer {} ({}x{}), crate v{}, build {}", GetRendererName().Data(), _w, _h,
             wgr_version(), wgr_build_id());

    const WgrAbiCheck abiCheck{WGR_ABI_VERSION,
                               sizeof(WgrAbiCheck),
                               sizeof(WgrSurfaceDesc),
                               sizeof(WgrLogCallbacks),
                               sizeof(WgrFrame),
                               WGR_ABI_FEATURE_BUILD_ID | WGR_ABI_FEATURE_SAFE_DIAGNOSTICS |
                                   WGR_ABI_FEATURE_RUNTIME_CAPABILITIES};
    const uint32_t runtimeAbi = wgr_abi_version();
    if (runtimeAbi != WGR_ABI_VERSION || !wgr_abi_validate(&abiCheck))
    {
        LOG_ERROR(Graphics, "Wgpu: ABI mismatch (engine {}, renderer {}); refusing renderer startup", WGR_ABI_VERSION,
                  runtimeAbi);
        SDL_DestroyWindow(_window);
        _window = nullptr;
        return;
    }

    _renderer = wgr_create(&desc, &log);
    if (!_renderer)
    {
        LOG_ERROR(Graphics, "Wgpu: wgr_create failed; backend unavailable");
        SDL_DestroyWindow(_window);
        _window = nullptr;
        return;
    }

    _wbank = new TextureBankWgpu(_renderer);
    SetGrassSettings(_grass);

    // This backend conforms terrain-clipped geometry (vegetation, roads' visuals) on
    // the GPU, so ClipLand objects publish a conform plane and skip their per-frame CPU
    // vertex deform. GL33 never sets this and keeps deforming on the CPU.
    GGpuTerrainConform = true;

    // Terrain is always drawn via the GPU heightmap path on this backend (the legacy
    // per-segment Shape path is GL33-only). It owns the sun-shadow mask, GPU conform, etc.
    _terrain = std::make_unique<TerrainWgpu>(*this, _renderer);

    // GPU water surface (flat CDLOD plane at sea level). Gated by WGR_GPU_WATER
    // (default on) so the legacy per-segment water mesh can be re-enabled during
    // bring-up; when null, Landscape::DrawWater falls through to the legacy path.
    bool gpuWater = true;
    if (const char* w = std::getenv("WGR_GPU_WATER"))
    {
        gpuWater = std::strtol(w, nullptr, 10) != 0;
    }
    if (gpuWater)
    {
        _water = std::make_unique<WaterWgpu>(*this, _renderer);
    }

    // Screen-space AO (GTAO). Default OFF; WGR_GTAO=1 enables it at startup and
    // WGR_GTAO_DEBUG=1 additionally boots straight into the raw greyscale AO view.
    //
    // This override has to live HERE, on the C++ side, not as a default in the Rust renderer:
    // PushRenderParams sends _ao every frame, so whatever the renderer defaults to is
    // overwritten immediately. The layer furthest from the renderer wins — a Rust-side env
    // gate would read as working and do nothing. Treat any value but "0" as on, matching the
    // other WGR_* gates.
    if (const char* ao = std::getenv("WGR_GTAO"))
    {
        _ao.enabled = std::strcmp(ao, "0") != 0;
    }
    if (const char* aod = std::getenv("WGR_GTAO_DEBUG"))
    {
        // 0 = off, 1 = AO greyscale, 2 = bent normal. Any non-numeric truthy value means 1.
        const long mode = std::strtol(aod, nullptr, 10);
        _ao.debugMode = int(mode < 0 ? 0 : (mode > 2 ? 2 : mode));
        if (mode == 0 && std::strcmp(aod, "0") != 0)
        {
            _ao.debugMode = 1;
        }
    }
    LOG_INFO(Graphics, "Wgpu: gtao gate: enabled={} debugMode={} radius={} slices={} steps={}", _ao.enabled,
             _ao.debugMode, _ao.radius, _ao.slices, _ao.steps);

    // Interior sky visibility (LIT-020). Default OFF; WGR_INTERIOR_SKY=1 enables it at startup
    // and WGR_INTERIOR_SKY_DEBUG=1 boots straight into the greyscale reach view. Same reasoning
    // as the GTAO gate directly above for why this lives on the C++ side.
    if (const char* is = std::getenv("WGR_INTERIOR_SKY"))
    {
        _interiorSky.enabled = std::strcmp(is, "0") != 0;
    }
    // Box half-size override. Bring-up needs this: the map covers only +-extent around the
    // camera, so on an empty stretch of terrain an EMPTY map is the correct answer, and telling
    // that apart from a broken one means widening the box until something has to appear.
    if (const char* ise = std::getenv("WGR_INTERIOR_SKY_EXTENT"))
    {
        const float v = std::strtof(ise, nullptr);
        if (v > 0.0f)
        {
            _interiorSky.extent = v;
        }
    }
    // The bake produces volumes at load time; applying them is a runtime switch. Both default
    // on, so this env var is now the way to turn the pair OFF (=0) rather than on.
    if (const char* isb = std::getenv("WGR_SKY_BAKE_VOLUMES"))
    {
        _interiorSky.baked = std::strcmp(isb, "0") != 0;
    }
    if (const char* isd = std::getenv("WGR_INTERIOR_SKY_DEBUG"))
    {
        _interiorSky.debug = std::strcmp(isd, "0") != 0;
    }
    // Through the log sink, not eprintln!/stderr: renderer diagnostics that must survive into
    // --log-file have to go through the sink or they are simply not there when you read the log.
    LOG_INFO(Graphics,
             "Wgpu: interior sky gate: enabled={} debug={} res={} extent={} height={} strength={} floor={} "
             "kernel={} bias={}",
             _interiorSky.enabled, _interiorSky.debug, _interiorSky.resolution, _interiorSky.extent,
             _interiorSky.height, _interiorSky.strength, _interiorSky.floorLevel, _interiorSky.kernel,
             _interiorSky.bias);

    // WGR_SHADOW_MAPS=1 enables cascaded shadow maps at startup (dev panel /
    // tri verbs can still toggle at runtime).
    if (const char* sm = std::getenv("WGR_SHADOW_MAPS"))
    {
        _smTuning.enabled = std::strtol(sm, nullptr, 10) != 0;
    }

    // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3b). When on, the
    // landscape/world hooks register shapes + stream retained instances and the CPU colour
    // draw of handed-over objects is suppressed; when off, every hook is a no-op and the CPU
    // path is unchanged.
    //
    // The parse must match the Rust gate for every value the user can actually set, which
    // `== "1"` did not. Rust enables on anything that is not "0", so WGR_GPU_DRIVEN=true (or
    // yes, or 2) turned the Rust path on while leaving this side off — the renderer then logs
    // "GPU-driven rendering enabled" and silently does nothing, because nothing registers a
    // retained scene. Treat any set value other than "0" as on, as Rust does.
    //
    // The DEFAULT deliberately still differs and must not be unified: unset leaves this false
    // while Rust is true. That is the plan's intended Stage 3 state — the Rust path is built
    // and inert until the C++ retained-scene feed (Stage 3b-3) lands.
    if (const char* gd = std::getenv("WGR_GPU_DRIVEN"))
    {
        _gpuDriven = std::strcmp(gd, "0") != 0;
    }

    // Capture automation selects the same canonical MaterialDebugSettings used
    // by Dev Tools. The test-model material slice presently runs through the
    // direct production WGPU path (set WGR_GPU_DRIVEN=0 for deterministic model
    // captures) while retained-material support remains a separate follow-up.
    if (const char* view = std::getenv("WGR_MATERIAL_DEBUG_VIEW"))
    {
        if (std::strcmp(view, "base") == 0)
            _materialDebug.view = MaterialDebugSettings::View::BaseColorOnly;
        else if (std::strcmp(view, "normal") == 0)
            _materialDebug.view = MaterialDebugSettings::View::FullMaterial;
        else if (std::strcmp(view, "normalviz") == 0)
            _materialDebug.view = MaterialDebugSettings::View::NormalMap;
        LOG_INFO(Graphics, "Wgpu: material debug view={}", view);
    }
    if (const char* disableNormal = std::getenv("WGR_MATERIAL_DISABLE_NORMAL"))
        _materialDebug.disableNormalMap = std::strcmp(disableNormal, "0") != 0;

    if (const char* invertNormalY = std::getenv("WGR_MATERIAL_INVERT_NORMAL_Y"))
        _materialDebug.invertNormalY = std::strcmp(invertNormalY, "0") != 0;
    if (const char* rvmatMatch = std::getenv("WGR_MATERIAL_DEBUG_RVMAT"))
        _materialDebugRvmatMatch = rvmatMatch;

    // Mirror the renderer's WGR_HDR gate so the ImGui Tonemap tab knows the HDR
    // resolve pass is live (the renderer owns the real gate; this is UI-only). The
    // grade is driven by the per-ToD presets by default; a WGR_* tonemap env override
    // switches to manual so env-based tuning still works. UpdateAutoTonemap (per
    // frame) otherwise owns _tonemap.
    if (const char* h = std::getenv("WGR_HDR"))
    {
        _hdrEnabled = std::strcmp(h, "0") != 0;
    }
    if (const char* e = std::getenv("WGR_EXPOSURE"))
    {
        const float v = std::strtof(e, nullptr);
        if (v > 0.0f)
        {
            _tonemap.exposure = v;
            _tonemapAuto = false;
        }
    }
    if (const char* m = std::getenv("WGR_TONEMAP"))
    {
        _tonemap.hable = std::strtol(m, nullptr, 10) != 0;
        _tonemapAuto = false;
    }
    if (const char* en = std::getenv("WGR_HDR_ENCODE"))
    {
        _tonemap.encode = std::strtol(en, nullptr, 10) != 0;
        _tonemapAuto = false;
    }
    PushRenderParams();
    PushSkyRuntime();
    // Once at startup so the WGR_MATERIAL_* env overrides read above reach the
    // retained path; after this only a Materials-tab change pushes it.
    PushMaterialDebug();

    Dev::DebugOverlay::InitForEngine(_window);
    _eventWindow.Attach(_window, _w, _h);
}

EngineWgpu::~EngineWgpu()
{
    // While the engine (and its renderer) is still alive so the overlay's
    // textures release cleanly; a later fallback engine can then re-init.
    Dev::DebugOverlay::Shutdown();
    _eventWindow.Detach();
    if (_wbank)
    {
        _wbank->Detach();
    }
    if (_renderer)
    {
        // Release the retained scene's owned pool meshes before tearing down the renderer.
        for (uint64_t mesh : _gpuMeshes)
        {
            wgr_mesh_destroy(_renderer, mesh);
        }
        _gpuMeshes.clear();
        wgr_destroy(_renderer);
        _renderer = nullptr;
    }
    if (_window)
    {
        SDL_DestroyWindow(_window);
        _window = nullptr;
    }
}

AbstractTextBank* EngineWgpu::TextBank()
{
    return _wbank;
}

RString EngineWgpu::GetDebugName() const
{
    return "Wgpu";
}

RString EngineWgpu::GetRendererName() const
{
    return "WGPU (Rust / wgpu)";
}

int EngineWgpu::Width() const
{
    return _w;
}

int EngineWgpu::Height() const
{
    return _h;
}

bool EngineWgpu::IsWindowed() const
{
    return _windowed;
}

bool EngineWgpu::CanBeWindowed() const
{
    return true;
}

bool EngineWgpu::SetSwapInterval(int interval)
{
    if (interval != 0 && interval != 1 && interval != -1)
    {
        return false;
    }
    if (!_renderer || !wgr_set_present_mode(_renderer, interval))
    {
        LOG_WARN(Graphics, "Wgpu: requested VSync interval {} is unavailable", interval);
        return false;
    }
    _swapInterval = interval;
    LOG_INFO(Graphics, "Wgpu: VSync interval set to {}", interval);
    return true;
}

void EngineWgpu::ResizeSurface(int w, int h)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }
    _w = w;
    _h = h;
    if (_renderer)
    {
        wgr_resize(_renderer, U32(w), U32(h));
    }
}

void EngineWgpu::OnWindowResized(int w, int h)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }
    ResizeSurface(w, h);
    FireResizePostHook(w, h);
}

namespace
{

WgrBlend BlendForSpec(int spec)
{
    const render::Backend b = render::SplitLegacy(spec).backend;
    if (render::Has(b, render::Backend::IsAlpha) || render::Has(b, render::Backend::IsAlphaFog) ||
        render::Has(b, render::Backend::IsTransparent))
    {
        return WGR_BLEND_ALPHA;
    }
    return WGR_BLEND_OPAQUE;
}

Sampler2DFlags SamplerForSpec(int spec)
{
    const render::Backend b = render::SplitLegacy(spec).backend;
    Sampler2DFlags s = Sampler2DFlags::None;
    if (render::Has(b, render::Backend::ClampU))
    {
        s |= Sampler2DFlags::ClampU;
    }
    if (render::Has(b, render::Backend::ClampV))
    {
        s |= Sampler2DFlags::ClampV;
    }
    if (render::Has(b, render::Backend::PointSampling))
    {
        s |= Sampler2DFlags::Point;
    }
    return s;
}

WgrDepthMode DepthForSpec(int spec)
{
    const render::Backend b = render::SplitLegacy(spec).backend;
    // NoZBuf (sky) skips depth entirely; NoZWrite (transparent meshes) tests but
    // doesn't occlude; otherwise opaque geometry tests and writes.
    if (render::Has(b, render::Backend::NoZBuf))
    {
        return WGR_DEPTH_NONE;
    }
    if (render::Has(b, render::Backend::NoZWrite))
    {
        return WGR_DEPTH_TEST;
    }
    return WGR_DEPTH_TEST_WRITE;
}

// Translate a built RenderPassDescriptor into the fields the 3D FFI carries.
// Reusing BuildRenderPassDescriptor keeps the wgpu backend's blend / depth /
// alpha-test / decal choices identical to GL33's.
WgrBlend BlendFromDesc(render::BlendMode b)
{
    switch (b)
    {
        case render::BlendMode::AlphaBlend:
            return WGR_BLEND_ALPHA;
        case render::BlendMode::Additive:
            return WGR_BLEND_ADDITIVE;
        case render::BlendMode::Shadow:
            return WGR_BLEND_SHADOW;
        case render::BlendMode::Opaque:
        default:
            return WGR_BLEND_OPAQUE;
    }
}

WgrDepthMode DepthFromDesc(render::DepthMode d)
{
    switch (d)
    {
        case render::DepthMode::Disabled:
            return WGR_DEPTH_NONE;
        case render::DepthMode::ReadOnly:
        case render::DepthMode::Shadow:
            return WGR_DEPTH_TEST;
        case render::DepthMode::Normal:
        default:
            return WGR_DEPTH_TEST_WRITE;
    }
}

// Cutout threshold in [0,1]; 0 when the draw isn't alpha-tested.
float AlphaRefFromDesc(const render::RenderPassDescriptor& desc)
{
    const bool test = desc.alpha == render::AlphaMode::Test || desc.alpha == render::AlphaMode::TestAndBlend;
    return test ? desc.alphaRef / 255.0f : 0.0f;
}

// True when nothing in the material can make the surface see-through, so any
// translucency would have to come from the texture. A fade driven by material
// alpha over an opaque texture is real and must keep its blend.
bool MaterialIsFullyOpaque(const TLMaterial& material)
{
    constexpr float kOpaque = 1.0f - 1.0f / 255.0f;
    return material.diffuse.A() >= kOpaque && material.ambient.A() >= kOpaque && material.emmisive.A() >= kOpaque &&
           material.forcedDiffuse.A() >= kOpaque;
}

// Pack an engine PackedColor into the FFI's 0xAARRGGBB WgrRgba8.
WgrRgba8 PackColor(PackedColor c)
{
    return U32(static_cast<DWORD>(c));
}

WgrVertex2D MakeVertex(float x, float y, float u, float v, PackedColor c)
{
    return WgrVertex2D{{x, y, 0.0f}, 1.0f, 1.0f, {u, v}, PackColor(c)};
}

WgrVertex2D MakeScreenVertex(const TLVertex& v)
{
    // Fog blend factor = specular alpha (GL33's vFogTC): 255 -> keep colour, 0 -> full fog.
    const float fog = v.specular.A8() / 255.0f;
    return WgrVertex2D{{v.pos[0], v.pos[1], v.pos[2]}, v.rhw, fog, {v.t0.u, v.t0.v}, PackColor(v.color)};
}

uint64_t ResolveTexture(const MipInfo& mip)
{
    if (auto* t = static_cast<TextureWgpu*>(mip._texture))
    {
        return t->EnsureUploaded();
    }
    return 0;
}

} // namespace

void EngineWgpu::InitDraw(bool clear, PackedColor color)
{
    _verts.clear();
    _batches.clear();
    _draws3d.clear();
    _cmds.clear();
    _palette.clear();
    _cameras.clear();
    _currentCamera = 0;
    _haveCamera = false;
    _swMesh = nullptr;
    _shadowCasters.clear();
    _smCascadesValid = false;
    _smEnabledFrame = ShadowMapsEnabled();
    if (clear)
    {
        _clear[0] = color.R8() / 255.0f;
        _clear[1] = color.G8() / 255.0f;
        _clear[2] = color.B8() / 255.0f;
        _clear[3] = 1.0f;
    }
    else
    {
        _clear[0] = _clear[1] = _clear[2] = 0.0f;
        _clear[3] = 1.0f;
    }
    if (_wbank)
    {
        _wbank->StartFrame();
    }
}

void EngineWgpu::Clear(bool clearZ, bool clearColor, PackedColor color)
{
    if (clearColor)
    {
        _clear[0] = color.R8() / 255.0f;
        _clear[1] = color.G8() / 255.0f;
        _clear[2] = color.B8() / 255.0f;
        _clear[3] = 1.0f;
    }
    if (clearZ)
    {
        _cmds.push_back(WgrCmd{WGR_CMD_CLEAR_DEPTH, 0});
    }
}

void EngineWgpu::FinishDraw()
{
    if (_wbank)
    {
        _wbank->FinishFrame();
    }
    // Engine::FinishDraw drives the frame counter (+timing) the test harness
    // readiness checks poll.
    Engine::FinishDraw();
}

void EngineWgpu::NextFrame()
{
    if (_renderer)
    {
        SyncWaterLookProfile();
        // Interpolate the per-time-of-day tonemap/grade + atmosphere presets for this frame
        // into _tonemap / _sky (auto mode only; manual override holds the tabs' values).
        UpdateAutoTonemap();
        UpdateAutoSky();
        UpdateSunGlareExposure();
        // Push the consolidated look block (picks up the auto-ToD updates; renderer diffs the
        // terrain sub-blocks) and then the per-frame sky runtime (celestial + camera).
        PushRenderParams();
        PushSkyRuntime();

        // Build + flatten the ImGui frame; SubmitOverlay fills the _overlay*
        // vectors the WgrFrame below points at. Composites over everything,
        // same placement as GL33's BackToFront (pre-present).
        Dev::DebugOverlay::NewFrame();
        Dev::DebugOverlay::Render();

        static_assert(sizeof(CameraEntry::proj) == 64 && sizeof(CameraEntry::view) == 64,
                      "CameraEntry matrices must be 16 floats to match WgrCamera");
        static_assert(sizeof(WgrCamera::proj) == 64 && sizeof(WgrCamera::view) == 64,
                      "WgrCamera matrices must be 16 floats");
        const Color& fog = FogColor();
        // Distance fog for the 3D path, matching GL33's BuildFrameState: the
        // scene's fog range drives a linear fog factor per vertex. Packed into
        // each camera UBO (see WgrCamera). The 2D/sky path fogs separately via
        // per-vertex specular alpha.
        float fogStart = 0.0f, fogInvRange = 0.0f, fogEnabled = 0.0f;
        if (GScene)
        {
            fogStart = GScene->GetFogMinRange();
            const float fogEnd = GScene->GetFogMaxRange();
            fogInvRange = (fogEnd > fogStart) ? 1.0f / (fogEnd - fogStart) : 0.0f;
            fogEnabled = 1.0f;
        }
        // Aerial-perspective mode (fogEnabled == 2): when the procedural sky drives the
        // HDR path, the renderer fogs the scene with a deferred atmosphere pass, so the
        // 3D shaders skip their flat fog-colour blend (they still use the distance
        // factor for shadow fade). Must match the renderer's aerial gate.
        if (_hdrEnabled && _sky.enabled && fogEnabled > 0.5f)
        {
            fogEnabled = 2.0f;
        }

        // Sun light for GPU-lit terrain, matching GL33's material upload:
        // light colour x eye accommodation, plus the raw travel direction
        // (GL33's sunDir vertex constant). At night/dawn the sun sits at or
        // below the horizon, which is what keeps terrain ambient-only dark
        // there — a made-up overhead direction reads as moonlight noon.
        Color sunDiffuse = HWhite;
        Color sunAmbient = HWhite;
        Vector3 sunDir(-0.4f, -0.85f, -0.3f);
        if (GScene && GScene->MainLight())
        {
            const Color accom = GetAccomodateEye();
            sunDiffuse = GScene->MainLight()->Diffuse() * accom;
            sunAmbient = GScene->MainLight()->Ambient() * accom;
            sunDir = GScene->MainLight()->Direction();
        }
        sunDir.Normalize();

        // Sky-based scene lighting (HDR + procedural sky + toggle): replace the legacy sun/
        // ambient with atmosphere-derived radiance on the physical scale, flagged to the shaders
        // via sun_diffuse.w so they skip the sRGB decode and take the unified path. The values
        // can far exceed 1, so they go straight to floats (bypassing any Color clamp).
        float sunLin[3] = {sunDiffuse.R(), sunDiffuse.G(), sunDiffuse.B()};
        float ambLin[3] = {sunAmbient.R(), sunAmbient.G(), sunAmbient.B()};
        float skyLit = 0.0f;
        if (_hdrEnabled && _sky.enabled && _sky.skyLighting && GScene && GScene->MainLight())
        {
            const float camAlt = GScene->GetCamera() ? static_cast<float>(GScene->GetCamera()->Position().Y()) : 0.0f;
            const Vector3 tr = AtmosphereSunTransmittance(_sky, camAlt, -GScene->MainLight()->SunDirection());
            const float rad = _sky.sunIntensity * _sky.exposure;
            sunLin[0] = tr.X() * rad;
            sunLin[1] = tr.Y() * rad;
            sunLin[2] = tr.Z() * rad;
            // Non-physical ambient fill (no GI yet, and none coming for a long while): the
            // engine's ToD-varying ambient used directly (gamma space, NOT sRGB-decoded — the
            // decode crushes midtones to ~6% of the sun and blackens interiors/shadowed faces),
            // scaled onto the sun's physical range. A generous, readable floor that still fades
            // toward night as the engine dims its ambient. Real sky irradiance replaces this later.
            const Color amb = GScene->MainLight()->Ambient();
            const float ambScale = _sky.skyAmbient * rad;
            ambLin[0] = amb.R() * ambScale;
            ambLin[1] = amb.G() * ambScale;
            ambLin[2] = amb.B() * ambScale;
            skyLit = 1.0f;

            // MOONLIGHT. On this path the directional radiance is the atmosphere's sun
            // transmittance, which is ~0 once the sun is below the horizon — so without
            // this the night scene is lit by ambient alone and the moon casts nothing,
            // whatever LightSun says. LightSun has already swung Direction()/
            // ShadowDirection() onto the moon (MoonLightAmount() > 0 is exactly when it
            // does), so only the RADIANCE is missing here.
            //
            // Reddened by the same atmospheric transmittance the sun uses, sampled along
            // the MOON's direction: a moon low over the horizon looks orange for the same
            // reason the sun does, and its light on the ground goes with it.
            const float moonAmount = GScene->MainLight()->MoonLightAmount();
            if (moonAmount > 0.0f && _sky.moonIntensity > 0.0f)
            {
                const Vector3 moonTr =
                    AtmosphereSunTransmittance(_sky, camAlt, -GScene->MainLight()->MoonDirection());
                // moonIntensity is the full-moon irradiance as a FRACTION of the sun's
                // (default 1/400 — see Engine::SkySettings). moonAmount already carries
                // the phase falloff and the horizon ramps.
                const float moonRad = rad * _sky.moonIntensity * moonAmount;
                // Slight blue bias, matching LightSun::MoonLightColor: the Purkinje shift
                // is not modelled anywhere downstream, so it has to live in the light.
                sunLin[0] += moonTr.X() * moonRad * 0.73f;
                sunLin[1] += moonTr.Y() * moonRad * 0.80f;
                sunLin[2] += moonTr.Z() * moonRad * 1.00f;
            }
        }

        // Cloud coverage makes the sun light diffuse: dim the directional beam toward ZERO and lift
        // the flat ambient as overcast rises, so a fully overcast sky has no sharp directional sun
        // (and, below, no sharp shadow map). smoothstep so thin clouds barely dim. `cloudDim` is
        // hoisted so it also fades the shadow darkness. The terrain/object ambient greys via the
        // env->SH bake (clouds are in the env map) so the ambient lift here is modest.
        float cloudDim = 0.0f;
        if (_sky.enabled && _sky.cloudCoverage > 0.0f)
        {
            // Saturate by ~0.8 coverage: the sky reads as fully overcast there, so the light should
            // be fully diffuse by then too (higher coverage only thickens the deck).
            const float ct = std::clamp((_sky.cloudCoverage - 0.1f) / (0.8f - 0.1f), 0.0f, 1.0f);
            cloudDim = ct * ct * (3.0f - 2.0f * ct); // smoothstep(0.1, 0.8, coverage)
            // Overcast leaves only a faint directional core (5%); the rest becomes ambient.
            const float sunFactor = 1.0f - 0.95f * cloudDim;
            sunLin[0] *= sunFactor;
            sunLin[1] *= sunFactor;
            sunLin[2] *= sunFactor;
            const float ambBoost = 1.0f + 0.5f * cloudDim;
            ambLin[0] *= ambBoost;
            ambLin[1] *= ambBoost;
            ambLin[2] *= ambBoost;
        }

        // Frame-global point/spot lights for the GPU-lit paths (objects + terrain).
        // Mirrors GL33's UploadVSLights, but ONE scene-wide list instead of a
        // per-draw selection: gather the scene's point/spot lights, colours
        // pre-scaled by NightEffect (so they vanish by day, like GL33's local
        // lights), positions in ABSOLUTE world space (the shader rebases per
        // camera). Imported worlds can have hundreds of static markers, so select
        // the best-fitting lights around the active camera before the fixed GPU
        // capacity is applied; insertion order is not a useful visibility proxy.
        _lights.clear();
        if (GScene && GScene->MainLight())
        {
            const float night = GScene->MainLight()->NightEffect();
            if (night > 0.0f)
            {
                struct LightCandidate
                {
                    float score;
                    WgrLight light;
                };
                std::vector<LightCandidate> candidates;
                const Vector3 cameraPos = GScene->GetCamera() ? GScene->GetCamera()->Position() : VZero;
                const int n = GScene->NLights();
                candidates.reserve(n);
                for (int i = 0; i < n; i++)
                {
                    Light* light = GScene->GetLight(i);
                    if (!light || !light->IsOn())
                    {
                        continue;
                    }
                    LightDescription desc;
                    light->GetDescription(desc);
                    const bool isSpot = desc.type == LTSpotLight;
                    if (desc.type != LTPoint && !isSpot)
                    {
                        // point + spot only; the sun is the directional main light
                        continue;
                    }
                    const Color dif = desc.diffuse * night;
                    const Color amb = desc.ambient * night;
                    Vector3 beam = desc.dir;
                    beam.Normalize();
                    WgrLight pl{};
                    pl.pos = {desc.pos.X(), desc.pos.Y(), desc.pos.Z(), desc.startAtten};
                    pl.diffuse = {dif.R(), dif.G(), dif.B(), 0.0f};
                    pl.ambient = {amb.R(), amb.G(), amb.B(), 0.0f};
                    pl.dir = {beam.X(), beam.Y(), beam.Z(), isSpot ? 1.0f : 0.0f};
                    // Prefer a light whose attenuation volume contains the camera. This
                    // stable, range-normalised score keeps nearby runway/flood lights in
                    // the shared terrain/object list even if a world loaded far-away lights
                    // first. Terrain fragments still apply their exact per-pixel falloff.
                    const float range = std::max(desc.startAtten * 10.0f, 1.0f);
                    const float score = desc.pos.Distance2(cameraPos) / (range * range);
                    candidates.push_back({score, pl});
                }
                std::sort(candidates.begin(), candidates.end(),
                          [](const LightCandidate& a, const LightCandidate& b) { return a.score < b.score; });
                const size_t selected = std::min(candidates.size(), size_t(WGR_MAX_LIGHTS));
                _lights.reserve(selected);
                for (size_t i = 0; i < selected; ++i)
                    _lights.push_back(candidates[i].light);
                if (std::getenv("WGR_STATIC_LIGHT_TRACE"))
                    LOG_INFO(Graphics, "Wgpu local-light selection: candidates={} selected={} night={:.3f}",
                             candidates.size(), selected, night);
            }
        }
        const float lightCount = static_cast<float>(_lights.size());

        // Cloud cover fades the cascade shadow toward none (in lockstep with the directional-sun
        // dimming above), so a fully overcast sky casts no sharp sun shadows. (Terrain self-shadow
        // and the froxel aerial shadow are separate paths — a fuller pass is deferred.)
        const float shadowStrength = (GetShadowFactor() / 256.0f) * (1.0f - cloudDim);
        const bool shadowActive = _smCascadesValid && !_shadowCasters.empty();
        // Sun-faded darkness (GL33 parity): full daylight uses tuning.darkness,
        // dusk ramps toward 1 (no darkening) as the sun sets. Cloud cover also ramps it toward 1
        // (1 = no shadow darkening) so an overcast sky casts no sharp cascade shadows — the sun is
        // diffuse, so its shadow should soften out in lockstep with the directional-sun dimming.
        float darkness = 1.0f - _smSunFactor * (1.0f - _smTuning.darkness);
        darkness = darkness + (1.0f - darkness) * cloudDim;

        // One-shot companion to the TreeAdv material trace.  The HDR sky-lit
        // path bypasses generic per-material lighting, so its frame inputs must
        // be measured independently when diagnosing a night-bright tree.
        if (std::getenv("WGR_TREEADV_LIGHT_TRACE") && GScene && GScene->MainLight() &&
            GScene->MainLight()->NightEffect() > 0.9f)
        {
            static bool loggedTreeAdvNightFrame = false;
            if (!loggedTreeAdvNightFrame)
            {
                loggedTreeAdvNightFrame = true;
                LOG_INFO(Graphics,
                         "TreeAdv night frame trace: night={:.3f} skyLit={} legacySun=[{:.4f},{:.4f},{:.4f}] "
                         "legacyAmbient=[{:.4f},{:.4f},{:.4f}] skySun=[{:.4f},{:.4f},{:.4f}] "
                         "skyAmbient=[{:.4f},{:.4f},{:.4f}] skyAmbientScale={:.3f} foliageAmbientBoost={:.3f}",
                         GScene->MainLight()->NightEffect(), skyLit > 0.5f ? 1 : 0, sunDiffuse.R(), sunDiffuse.G(),
                         sunDiffuse.B(), sunAmbient.R(), sunAmbient.G(), sunAmbient.B(), sunLin[0], sunLin[1],
                         sunLin[2], ambLin[0], ambLin[1], ambLin[2], _sky.skyAmbient, _foliage.ambientBoost);
            }
        }

        std::vector<WgrCamera> cameras(_cameras.size());
        for (size_t i = 0; i < _cameras.size(); i++)
        {
            std::memcpy(cameras[i].proj.m, &_cameras[i].proj, sizeof(cameras[i].proj.m));
            std::memcpy(cameras[i].view.m, &_cameras[i].view, sizeof(cameras[i].view.m));
            // .w carries the aerial fog distance-ramp exponent (frame::apply_fog); lower =
            // denser fog throughout, which reveals the volumetric terrain sun-shadowing.
            cameras[i].fog_color = {fog.R(), fog.G(), fog.B(), _sky.fogFalloff};
            cameras[i].params = {fogStart, fogInvRange, fogEnabled, shadowStrength};
            // cam_pos.w carries the active point-light count (the storage buffer
            // is fixed-capacity, so its length is not the count).
            cameras[i].cam_pos = {_cameras[i].pos[0], _cameras[i].pos[1], _cameras[i].pos[2], lightCount};
            // sun_diffuse.w = sky-lighting flag (1 = rgb are physical linear radiance, skip the
            // shader's sRGB decode; 0 = legacy gamma sun). sun_ambient/diffuse rgb are already the
            // right space for the active path (see the skyLit block above).
            cameras[i].sun_diffuse = {sunLin[0], sunLin[1], sunLin[2], skyLit};
            // sun_ambient.rgb = flat ambient fill (still used by water + the legacy path). On the
            // sky-lit path .w carries the skyAmbient knob, scaling the DIRECTIONAL SH sky-irradiance
            // ambient the lit-mesh/terrain shaders now use in place of the flat rgb.
            cameras[i].sun_ambient = {ambLin[0], ambLin[1], ambLin[2], skyLit > 0.5f ? _sky.skyAmbient : 0.0f};
            cameras[i].sun_dir_world = {sunDir.X(), sunDir.Y(), sunDir.Z(), 0.0f};
            if (shadowActive)
            {
                WgrCameraShadow& sb = cameras[i].shadow;
                for (int c = 0; c < _smCascades.count && c < 4; c++)
                {
                    std::memcpy(sb.cascade_vp[c].m, _smCascades.camRelVP[c].m.data(), sizeof(sb.cascade_vp[c].m));
                }
                sb.splits = {_smCascades.splitViewDist[0], _smCascades.splitViewDist[1], _smCascades.splitViewDist[2],
                             _smCascades.splitViewDist[3]};
                sb.omni_radius = {_smCascades.omniRadius[0], _smCascades.omniRadius[1], _smCascades.omniRadius[2],
                                  _smCascades.omniRadius[3]};
                sb.ctl = {static_cast<float>(_smCascades.count), static_cast<float>(_smCascades.omniCount),
                          _smTuning.fadeRange, _smTuning.biasBase};
                sb.ctlb = {1.0f / static_cast<float>(_smCascadeRes), darkness, _smTuning.normalOffset, _smTuning.pcf};
                sb.cam_fwd = {_cameras[i].dir[0], _cameras[i].dir[1], _cameras[i].dir[2], 0.0f};
                sb.sun_dir = {_smCascades.sunDir.x, _smCascades.sunDir.y, _smCascades.sunDir.z, 0.0f};
            }
        }

        WgrFrame frame{};
        frame.clear = {_clear[0], _clear[1], _clear[2], _clear[3]};
        frame.fog_color = {fog.R(), fog.G(), fog.B()};
        frame.cameras = cameras;
        frame.draws3d = _draws3d;
        frame.verts = _verts;
        frame.batches = _batches;
        frame.cmds = _cmds;
        frame.palette = _palette;
        if (shadowActive)
        {
            frame.shadow.count = U32(_smCascades.count);
            frame.shadow.omni_count = U32(_smCascades.omniCount);
            frame.shadow.resolution = U32(_smCascadeRes);
            for (int c = 0; c < _smCascades.count && c < 4; c++)
            {
                std::memcpy(frame.shadow.light_vp[c].m, _smCascades.camRelVP[c].m.data(),
                            sizeof(frame.shadow.light_vp[c].m));
            }
            // Casters are camera-relative to the camera captured in AddShadowCaster (NOT
            // _currentCamera, which may have advanced to a HUD/weapon camera by now); the
            // depth shader adds cam_pos back to sample the terrain conform at the right xz.
            frame.shadow.cam_pos = {_smCamPos[0], _smCamPos[1], _smCamPos[2], 0.0f};
            frame.shadow_casters = _shadowCasters;
        }
        frame.overlay_verts = _overlayVerts;
        frame.overlay_indices = _overlayIndices;
        frame.overlay_draws = _overlayDraws;
        frame.terrain_nodes = _terrainNodes;
        frame.terrain_batches = _terrainBatches;
        frame.lights = _lights;
        frame.water_nodes = _waterNodes;
        frame.water_batches = _waterBatches;
        frame.grass_batches = _grassBatches;
        wgr_render_frame(_renderer, &frame);
        FlushPendingScreenshot();
    }
    _verts.clear();
    _batches.clear();
    _draws3d.clear();
    _cmds.clear();
    _palette.clear();
    _cameras.clear();
    _shadowCasters.clear();
    _overlayVerts.clear();
    _overlayIndices.clear();
    _overlayDraws.clear();
    _terrainNodes.clear();
    _terrainBatches.clear();
    _waterNodes.clear();
    _waterBatches.clear();
    _grassBatches.clear();
    _grassSubmitted = false;
    _smCascadesValid = false;
    _haveCamera = false;
    _currentCamera = 0;
    EngineDummy::NextFrame();
}

void EngineWgpu::AppendTriangles(uint64_t texture, WgrBlend blend, Sampler2DFlags sampler, WgrDepthMode depth,
                                 std::span<const WgrVertex2D> verts)
{
    if (verts.empty())
    {
        return;
    }

    const uint32_t samplerBits = U32(sampler);
    const uint32_t first = U32(_verts.size());
    _verts.insert(_verts.end(), verts.begin(), verts.end());

    // Merge only when the previous command is this same batch; an intervening 3D
    // draw, depth clear, or any state change must break the run so submission
    // order and per-batch state are preserved.
    const bool canMerge = !_batches.empty() && !_cmds.empty() && _cmds.back().kind == WGR_CMD_DRAW_2D &&
                          _cmds.back().arg == _batches.size() - 1 && _batches.back().texture_id == texture &&
                          _batches.back().blend == blend && _batches.back().sampler == samplerBits &&
                          _batches.back().depth == depth;
    if (canMerge)
    {
        _batches.back().vertex_count += U32(verts.size());
        return;
    }
    WgrDraw2DBatch batch{};
    batch.texture_id = texture;
    batch.first_vertex = first;
    batch.vertex_count = U32(verts.size());
    batch.blend = blend;
    batch.sampler = samplerBits;
    batch.depth = depth;
    _batches.push_back(batch);
    _cmds.push_back(WgrCmd{WGR_CMD_DRAW_2D, U32(_batches.size() - 1)});
}

void EngineWgpu::Draw2D(const Draw2DPars& pars, const Rect2DAbs& rect, const Rect2DAbs& clip)
{
    if (!pars.mip.IsOK())
        return;

    Draw2DCorners c;
    if (!ClipDraw2DRect(pars, rect, clip, static_cast<float>(_w), static_cast<float>(_h), c))
    {
        return;
    }

    const WgrVertex2D tl = MakeVertex(c.xBeg, c.yBeg, c.uTL, c.vTL, pars.colorTL);
    const WgrVertex2D tr = MakeVertex(c.xEnd, c.yBeg, c.uTR, c.vTR, pars.colorTR);
    const WgrVertex2D br = MakeVertex(c.xEnd, c.yEnd, c.uBR, c.vBR, pars.colorBR);
    const WgrVertex2D bl = MakeVertex(c.xBeg, c.yEnd, c.uBL, c.vBL, pars.colorBL);
    const WgrVertex2D quad[6] = {tl, tr, br, tl, br, bl};

    AppendTriangles(ResolveTexture(pars.mip), BlendForSpec(pars.spec), SamplerForSpec(pars.spec), WGR_DEPTH_NONE, quad);
}

void EngineWgpu::DrawPoly(const MipInfo& mip, const Vertex2DAbs* vertices, int n, const Rect2DAbs& clip, int specFlags)
{
    if (!mip.IsOK() || n < 3)
    {
        return;
    }

    constexpr int maxN = 32;
    Vertex2DAbs scratch1[maxN];
    Vertex2DAbs scratch2[maxN];
    vertices = ClipPoly2D(vertices, n, clip, scratch1, scratch2);
    if (!vertices)
    {
        return;
    }
    if (n > maxN)
    {
        n = maxN;
    }

    const uint64_t tex = ResolveTexture(mip);
    const WgrBlend blend = BlendForSpec(specFlags);
    const Sampler2DFlags sampler = SamplerForSpec(specFlags);
    std::vector<WgrVertex2D> tris;
    tris.reserve(static_cast<size_t>(n - 2) * 3);
    auto conv = [](const Vertex2DAbs& v) { return MakeVertex(v.x, v.y, v.u, v.v, v.color); };
    for (int i = 1; i + 1 < n; i++)
    {
        tris.push_back(conv(vertices[0]));
        tris.push_back(conv(vertices[i]));
        tris.push_back(conv(vertices[i + 1]));
    }
    AppendTriangles(tex, blend, sampler, WGR_DEPTH_NONE, tris);
}

void EngineWgpu::DrawPoly(const MipInfo& mip, const Vertex2DPixel* vertices, int n, const Rect2DPixel& clip,
                          int specFlags)
{
    if (!mip.IsOK() || n < 3)
    {
        return;
    }

    constexpr int maxN = 32;
    Vertex2DPixel scratch1[maxN];
    Vertex2DPixel scratch2[maxN];
    vertices = ClipPoly2D(vertices, n, clip, scratch1, scratch2);
    if (!vertices)
    {
        return;
    }
    if (n > maxN)
    {
        n = maxN;
    }

    const float x2d = static_cast<float>(Left2D());
    const float y2d = static_cast<float>(Top2D());
    const uint64_t tex = ResolveTexture(mip);
    const WgrBlend blend = BlendForSpec(specFlags);
    const Sampler2DFlags sampler = SamplerForSpec(specFlags);
    std::vector<WgrVertex2D> tris;
    tris.reserve(static_cast<size_t>(n - 2) * 3);
    auto conv = [&](const Vertex2DPixel& v) { return MakeVertex(v.x + x2d, v.y + y2d, v.u, v.v, v.color); };
    for (int i = 1; i + 1 < n; i++)
    {
        tris.push_back(conv(vertices[0]));
        tris.push_back(conv(vertices[i]));
        tris.push_back(conv(vertices[i + 1]));
    }
    AppendTriangles(tex, blend, sampler, WGR_DEPTH_NONE, tris);
}

void EngineWgpu::DrawLine(const Line2DAbs& line, PackedColor c0, PackedColor c1, const Rect2DAbs& clip)
{
    Texture* tex = GPreloadedTextures.New(TextureLine);
    const MipInfo& mip = TextBank()->UseMipmap(tex, 1, 1);

    const int specFlags = NoZBuf | IsAlpha | ClampU | ClampV | IsAlphaFog;

    Vertex2DAbs vertices[4];
    LineToQuad(line, c0, c1, vertices);

    DrawPoly(mip, vertices, 4, clip, specFlags);
}

VertexBuffer* EngineWgpu::CreateVertexBuffer(const Shape& src, VBType type)
{
    if (!_renderer || src.NVertex() <= 0)
    {
        return nullptr;
    }

    const int nv = src.NVertex();
    const int ni = render::mesh::CountIndices(src);
    if (ni <= 0)
    {
        return nullptr;
    }

    static_assert(sizeof(SVertex) == sizeof(WgrMeshVertex), "SVertex must match WgrMeshVertex");
    std::vector<SVertex> verts(static_cast<size_t>(nv));
    render::mesh::BuildVertices(src, verts.data());
    std::vector<VertexIndex> indices(static_cast<size_t>(ni));
    render::mesh::BuildIndices(src, indices.data());

    const uint64_t mesh = wgr_mesh_create(
        _renderer, AsMeshVerts(verts), WgrSlice<uint16_t>{reinterpret_cast<const uint16_t*>(indices.data()), U32(ni)});

    if (!mesh)
    {
        return nullptr;
    }

    const bool dynamic = (type == VBDynamic || type == VBSmallDiscardable);
    auto* buf = new VertexBufferWgpu(_renderer, mesh, nv, dynamic);
    render::mesh::BuildSections(src, buf->sections);
    return buf;
}

void EngineWgpu::PushSceneCamera()
{
    CameraEntry entry{};

    Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (camera)
    {
        // Camera-relative: the view's translation is dropped (the per-object world
        // matrix is already offset by the camera position).  Mirrors GL33's
        // BuildFrameState so the same matrices reach the shader.
        ConvertMatrix(entry.view, camera->InverseScaled());
        entry.view._41 = 0;
        entry.view._42 = 0;
        entry.view._43 = 0;
        ConvertProjectionMatrix(entry.proj, camera->ProjectionNormal(), 0);
        // Infinite-far reversed-Z (wgpu only; GL33 keeps its finite forward-Z). The
        // shared projection puts the far plane at ~1.01x the fog range, so terrain drawn
        // out to the fog range sits right at the far plane, where reversed-Z depth packs
        // to ~0 — which the aerial-perspective pass reads as background sky and skips,
        // leaving a black seam at the horizon. An infinite far plane maps every finite
        // depth into (0,1], so terrain always has depth > 0 (gets fogged, not skipped),
        // and with this GPU's D32F depth it is the precision-optimal form — which also
        // eases distant z-fighting. Forward: z_view=cNear -> ndc.z=0, z_view=inf ->
        // ndc.z=1; the shader's reverse_z() then flips near->1, far->0. (_33=c, _43=d.)
        const float cNear = static_cast<float>(camera->ClipNear());
        entry.proj._33 = 1.0f;
        entry.proj._43 = -cNear;

        const Vector3 pos = camera->Position();
        entry.pos[0] = pos.X();
        entry.pos[1] = pos.Y();
        entry.pos[2] = pos.Z();
        const Vector3 dir = camera->Direction();
        entry.dir[0] = dir.X();
        entry.dir[1] = dir.Y();
        entry.dir[2] = dir.Z();

        // Feed the GPU-driven cull compute the SAME LOD/distance inputs the CPU path uses
        // (Scene::LevelFromDistance2): draw distance, the projection scale Camera::Left(), and
        // the per-frame _lodInvWidth (≈ lodCoef*2/screenWidth). Without these the cull ran with
        // lod_scale/lod_inv_width = 1, making resol2 ~1e6× too large (every model snapped to its
        // coarsest LOD within metres). pixel_limit mirrors LevelFromDistance2's 0.125.
        if (_gpuDriven && _renderer)
        {
            // Draw distance is Scene::GetObjectDrawDistance(), NOT the raw OBJECT_Z: objectsZ
            // is 2/3 of the view distance and doubles as the simulation radius, which put the
            // renderer's hard cull edge at a distance where distance fog is only ~30% applied
            // — so objects materialised out of nothing there (the flying pop-in). The shared
            // accessor anchors the render cull to the fog max range, where fog is 1.0, and is
            // read by the CPU path (Scene::LevelFromDistance2) from the same place so the two
            // culls cannot drift. WGR_OBJECT_DISTANCE_SCALE=1 restores the legacy distance.
            wgr_set_cull_params(_renderer, GScene->GetObjectDrawDistance(), static_cast<float>(camera->Left()),
                                GScene->GetLodInvWidth(), 0.125f);
            // GPU Hi-Z occlusion replaces the engine's software occlusion buffer for the retained
            // set; force the built-in off while it's on so the two don't both cull (and the CPU
            // stops rasterizing the occlusion buffer). Driven per frame so the default (occlusion
            // on) applies without opening the Culling tab; the checkbox flips _cullDebug.occlusion.
            EnableObjOcc = !_cullDebug.occlusion;
        }
    }

    _cameras.push_back(entry);
    _currentCamera = U32(_cameras.size() - 1);
    _haveCamera = true;
}

void EngineWgpu::EnsureCamera()
{
    if (!_haveCamera)
    {
        PushSceneCamera();
    }
}

ITerrainRenderer* EngineWgpu::GetTerrainRenderer()
{
    return _terrain.get();
}

void EngineWgpu::SubmitTerrain(std::span<const WgrTerrainNode> nodes)
{
    if (!_renderer || nodes.empty())
    {
        return;
    }
    EnsureCamera();

    WgrTerrainBatch batch{};
    batch.first_node = U32(_terrainNodes.size());
    batch.node_count = U32(nodes.size());
    batch.camera = _currentCamera;
    _terrainNodes.insert(_terrainNodes.end(), nodes.begin(), nodes.end());
    _terrainBatches.push_back(batch);

    WgrCmd cmd{};
    cmd.kind = WGR_CMD_DRAW_TERRAIN;
    cmd.arg = U32(_terrainBatches.size() - 1);
    _cmds.push_back(cmd);
}

IWaterRenderer* EngineWgpu::GetWaterRenderer()
{
    return _water.get();
}

void EngineWgpu::SubmitWater(std::span<const WgrWaterNode> nodes)
{
    if (!_renderer || nodes.empty())
    {
        return;
    }
    EnsureCamera();

    WgrWaterBatch batch{};
    batch.first_node = U32(_waterNodes.size());
    batch.node_count = U32(nodes.size());
    batch.camera = _currentCamera;
    _waterNodes.insert(_waterNodes.end(), nodes.begin(), nodes.end());
    _waterBatches.push_back(batch);

    WgrCmd cmd{};
    cmd.kind = WGR_CMD_DRAW_WATER;
    cmd.arg = U32(_waterBatches.size() - 1);
    _cmds.push_back(cmd);
}

void EngineWgpu::UpdateFrameCamera()
{
    PushSceneCamera();
}

void EngineWgpu::PrepareMeshTL(const LightList& /*lights*/, const Matrix4& modelToWorld,
                               const render::LegacySpec& /*spec*/)
{
    EnsureCamera();
    const CameraEntry& cam = _cameras[_currentCamera];

    ConvertMatrix(_world, modelToWorld);
    _world._41 -= cam.pos[0];
    _world._42 -= cam.pos[1];
    _world._43 -= cam.pos[2];

    _worldM = modelToWorld;
    _worldM.SetPosition(modelToWorld.Position() - Vector3(cam.pos[0], cam.pos[1], cam.pos[2]));
}

void EngineWgpu::BeginMeshTL(const Shape& sMesh, int spec, bool dynamic)
{
    _meshSpec = spec;
    _currentPaletteSlot = WGR_NO_PALETTE;

    auto* buf = static_cast<VertexBufferWgpu*>(sMesh.GetVertexBuffer());
    if (!buf)
    {
        return;
    }

    buf->Update(sMesh, dynamic);

    if (!buf->skinned || buf->paletteCount <= 0)
    {
        return;
    }

    // Feedback to the animation system: this mesh is GPU-skinned so its CPU skinning can be skipped next frame
    buf->drawnSkinned = true;
    _currentPaletteSlot = U32(_palette.size() / WGR_PALETTE_SIZE);
    const size_t base = _palette.size();
    _palette.resize(base + WGR_PALETTE_SIZE);
    for (int i = 0; i < buf->paletteCount; i++)
    {
        GfxMatrix g;
        ConvertMatrix(g, _worldM * buf->palette[i]);
        std::memcpy(_palette[base + i].m, &g, sizeof(_palette[base + i].m));
    }
    // Reserved slot: bare world, so unweighted verts get world*pos.
    GfxMatrix gw;
    ConvertMatrix(gw, _worldM);
    std::memcpy(_palette[base + kReservedBone].m, &gw, sizeof(_palette[base + kReservedBone].m));
}

void EngineWgpu::EndMeshTL(const Shape& /*sMesh*/)
{
    _currentPaletteSlot = WGR_NO_PALETTE;
}

void EngineWgpu::SetMaterial(const TLMaterial& mat, const LightList& /*lights*/, const render::LegacySpec& /*spec*/)
{
    // Capture only; DrawSectionTL folds it with the sun once it has the combined
    // mesh+section spec (for the DisableSun / sun-enable decision). Per-draw local
    // lights are ignored here — the wgpu path drives lights from a single
    // frame-global light store, not GL33's per-draw list.
    _curMaterial = mat;
}

void EngineWgpu::DrawSectionTL(const Shape& sMesh, int beg, int end)
{
    if (!_renderer)
    {
        return;
    }

    auto* buf = static_cast<VertexBufferWgpu*>(sMesh.GetVertexBuffer());
    if (!buf || buf->sections.Size() == 0 || end <= beg || end > buf->sections.Size())
    {
        return;
    }

    const render::mesh::MeshSection& siBeg = buf->sections[beg];
    const render::mesh::MeshSection& siEnd = buf->sections[end - 1];
    const int indexCount = siEnd.end - siBeg.beg;
    if (indexCount <= 0)
    {
        return;
    }

    uint64_t tex = 0;
    Poseidon::AlphaStats::Kind alphaClass = Poseidon::AlphaStats::Opaque;
    if (auto* t = static_cast<TextureWgpu*>(sMesh.GetSection(beg).properties.GetTexture()))
    {
        tex = t->EnsureUploaded();
        alphaClass = t->GetAlphaClass();
        // WGR_ROAD_TRACE: one line per distinct texture reaching the immediate path.
        // Answers three questions at once, which a material-only dump cannot:
        //  (a) which PrepareTL branch ran (surfMat -> Combine, or the object material);
        //  (b) the FOLDED TLMaterial that reached SetMaterial -- a zero diffuse, or
        //      DisableSun in specFlags, would be the missing sun term the captures measured;
        //  (c) the vertex normal in this shape BEFORE MeshBuild negates it.
        // (c) is the discriminator. Shape::Norm() is +Y for the A2 road models decoded out
        // of roads_e, MeshBuild.cpp:44 negates it, and shading.wgsl clamps
        // max(dot(N, -sun_dir_world), 0) -- so a +Y source normal yields no sun term at all.
        // Note GL33 shares BuildVertices AND the same clamp, so a plain sign error would
        // band both backends; it does not, which is why the PATH matters as much as the
        // values. A road that never appears in this log took the CPU FaceArray::Draw branch
        // instead, and that absence is itself the answer.
        if (const char* rt = std::getenv("WGR_ROAD_TRACE"); rt && std::strcmp(rt, "0") != 0)
        {
            const char* rname = t->Name();
            if (rname && *rname)
            {
                static std::unordered_set<std::string> loggedRoadSections;
                if (loggedRoadSections.insert(rname).second)
                {
                    const ShapeSection& sec = sMesh.GetSection(beg);
                    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                    const int nv = sMesh.NVertex();
                    const int take = nv < 32 ? nv : 32;
                    for (int v = 0; v < take; v++)
                    {
                        nx += sMesh.Norm(v).X();
                        ny += sMesh.Norm(v).Y();
                        nz += sMesh.Norm(v).Z();
                    }
                    if (take > 0)
                    {
                        nx /= float(take);
                        ny /= float(take);
                        nz /= float(take);
                    }
                    LOG_INFO(Graphics,
                             "ROAD_TRACE tex={} branch={} nv={} meanNorm=({:.3f},{:.3f},{:.3f}) "
                             "amb=({:.3f},{:.3f},{:.3f}) dif=({:.3f},{:.3f},{:.3f}) "
                             "emi=({:.3f},{:.3f},{:.3f}) fdif=({:.3f},{:.3f},{:.3f}) specPow={} "
                             "specFlags=0x{:x} secSpec=0x{:x}",
                             rname, sec.surfMat ? "Combine(surfMat)" : "object-material", nv, nx, ny, nz,
                             _curMaterial.ambient.R(), _curMaterial.ambient.G(), _curMaterial.ambient.B(),
                             _curMaterial.diffuse.R(), _curMaterial.diffuse.G(), _curMaterial.diffuse.B(),
                             _curMaterial.emmisive.R(), _curMaterial.emmisive.G(), _curMaterial.emmisive.B(),
                             _curMaterial.forcedDiffuse.R(), _curMaterial.forcedDiffuse.G(),
                             _curMaterial.forcedDiffuse.B(), _curMaterial.specularPower,
                             unsigned(_curMaterial.specFlags), unsigned(sec.properties.Special()));
                }
            }
        }
        // One-shot, opt-in probe for the original CWA PAC/P8 vegetation route.  The material
        // base-colour debug view must be allowed to identify whether a black canopy originates
        // in the texture upload or in lighting, without changing either path.  Kept general so
        // it can also diagnose other legacy-only content; keyed by the virtual texture path.
        if (const char* trace = std::getenv("WGR_LEGACY_TEXTURE_TRACE"); trace && std::strcmp(trace, "0") != 0)
        {
            const char* name = t->Name();
            if (name && *name)
            {
                static std::unordered_set<std::string> loggedLegacyTextures;
                if (loggedLegacyTextures.insert(name).second)
                {
                    const PacLevelMem& mip = t->AMipmap(0);
                    LOG_INFO(Graphics,
                             "Legacy texture trace: path={} handle={} size={}x{} mips={} srcFormat={} dstFormat={} "
                             "alpha={} transparent={} alphaClass={} holes={}",
                             name, tex, t->AWidth(0), t->AHeight(0), t->ANMipmaps(), int(mip.SrcFormat()),
                             int(mip.DstFormat()), t->IsAlpha(), t->IsTransparent(), int(alphaClass),
                             t->HasAlphaHoles());
                }
            }
        }
    }

    const int sectionSpec = sMesh.GetSection(beg).properties.Special();
    const int effectiveSpec = _meshSpec | sectionSpec;
    const render::LegacySpec splitSpec = render::SplitLegacy(effectiveSpec);

    render::BuildContext ctx;
    ctx.isIn3DPass = true;
    ctx.shadowAlphaRef = static_cast<std::uint8_t>(std::min(255, (GetShadowFactor() * 7) >> 4));
    const render::RenderPassDescriptor desc = render::BuildRenderPassDescriptor(splitSpec, ctx);

    if (_smEnabledFrame && desc.blend == render::BlendMode::Shadow)
    {
        // Skip legacy shadows when CSM is enabled
        return;
    }

    WgrDraw3D d{};
    d.mesh = buf->mesh;
    d.index_begin = U32(siBeg.beg);
    d.index_count = U32(indexCount);
    d.texture_id = tex;
    // Resolve the real RVMAT through the same virtual-path-aware loader that the
    // model/material production path uses. Only Stage1 is consumed in this first
    // slice; the remaining controls are intentionally diagnostic no-ops until
    // their contributions are implemented one at a time.
    // The draw-time Shape has no source-model name.  Its address is nevertheless
    // stable for the frame and correlates with the opt-in ShapeAdapter trace;
    // section numbers alone repeat across unrelated visible and shadow LODs.
    const ShapeSection& debugSection = sMesh.GetSection(beg);
    const char* debugMaterialPath = debugSection.surfMat ? debugSection.surfMat->GetName().Data() : nullptr;
    if (_materialDebugRvmatMatch == "__trace__")
    {
        const auto shapeId = reinterpret_cast<uintptr_t>(&sMesh);
        const std::string traceKey = std::to_string(shapeId) + ":" + std::to_string(beg) + ":" +
                                     (debugMaterialPath ? debugMaterialPath : "<none>");
        static std::unordered_set<std::string> loggedSectionTraces;
        if (loggedSectionTraces.insert(traceKey).second)
            // Size and extent are here because a materialless section is only worth
            // chasing when it is big: sort the trace by `indices` or by bbox volume
            // and the offender is at the top. `shape=` joins either to the load-time
            // ShapeAdapter trace (a directly drawn LOD) or to the "ShadowCache copy
            // trace" line (a runtime SurfaceSplit / projected-shadow copy).
            LOG_INFO(Graphics,
                     "Wgpu material trace: shape={} section={} RVMAT={} indices={} sections={} min=({},{},{}) "
                     "max=({},{},{})",
                     static_cast<const void*>(&sMesh), beg, debugMaterialPath ? debugMaterialPath : "<none>",
                     indexCount, sMesh.NSections(), sMesh.Min().X(), sMesh.Min().Y(), sMesh.Min().Z(),
                     sMesh.Max().X(), sMesh.Max().Y(), sMesh.Max().Z());
    }
    const bool unassignedMaterialTarget = _materialDebugRvmatMatch == "__unassigned__" && !debugSection.surfMat;
    const bool materialDebugTarget = unassignedMaterialTarget || _materialDebugRvmatMatch == "*" ||
                                     ContainsNoCase(debugMaterialPath, _materialDebugRvmatMatch);
    // Without a diagnostic selector, authored section RVMATs are production
    // bindings. Unknown families and procedural stages remain intentionally
    // unbound after translation; there is no suffix-based fallback here.
    const bool productionMaterialTarget = _materialDebugRvmatMatch.empty() && debugSection.surfMat;
    const bool materialBindingTarget = materialDebugTarget || productionMaterialTarget;
    d.normal_texture_id = 0;
    d._pad = materialDebugTarget
                 ? static_cast<uint32_t>(_materialDebug.view) | (_materialDebug.invertNormalY ? 0x100u : 0u)
                 : 0;
    _materialDebugInfo = {};
    if (unassignedMaterialTarget)
    {
        // Some official source MLODs intentionally carry only per-face legacy
        // textures on their visible LOD.  A controlled smoke needs to distinguish
        // that missing RVMAT from a failed texture/normal binding, so permit a
        // pair of explicit local fixture paths on those unassigned sections only.
        const char* basePath = std::getenv("WGR_MATERIAL_DEBUG_BASE");
        const char* normalPath = std::getenv("WGR_MATERIAL_DEBUG_NORMAL");
        if (basePath && *basePath)
        {
            Ref<Texture> baseTexture = _wbank->Load(basePath);
            auto* wgpuBaseTexture = static_cast<TextureWgpu*>(baseTexture.GetRef());
            if (const uint64_t handle = wgpuBaseTexture ? wgpuBaseTexture->EnsureUploaded() : 0)
            {
                d.texture_id = handle;
                static std::unordered_set<std::string> loggedUnassignedBaseFixtures;
                if (loggedUnassignedBaseFixtures.insert(basePath).second)
                    LOG_INFO(Graphics, "Wgpu material debug: unassigned Stage0 fixture resolved path={}", basePath);
            }
        }
        const bool enableFixtureNormal =
            _materialDebug.view == MaterialDebugSettings::View::NormalMap ||
            (_materialDebug.view == MaterialDebugSettings::View::FullMaterial && !_materialDebug.disableNormalMap);
        if (enableFixtureNormal && normalPath && *normalPath)
        {
            Ref<Texture> normalTexture = _wbank->Load(normalPath);
            auto* wgpuNormalTexture = static_cast<TextureWgpu*>(normalTexture.GetRef());
            if (const uint64_t handle = wgpuNormalTexture ? wgpuNormalTexture->EnsureUploaded() : 0)
            {
                d.normal_texture_id = handle;
                static std::unordered_set<std::string> loggedUnassignedNormalFixtures;
                if (loggedUnassignedNormalFixtures.insert(normalPath).second)
                    LOG_INFO(Graphics, "Wgpu material debug: unassigned Stage1 fixture resolved path={}", normalPath);
            }
        }
    }
    else if (const ShapeSection& section = sMesh.GetSection(beg); materialBindingTarget && section.surfMat)
    {
        const char* materialPath = section.surfMat->GetName().Data();
        if (materialPath && *materialPath)
        {
            if (materialDebugTarget)
            {
                _materialDebugInfo.active = true;
                _materialDebugInfo.rvmatPath = materialPath;
            }
            // Keep screenshot automation evidence tied to the material actually
            // selected by the model.  This is deliberately emitted only for an
            // explicit debug match, so ordinary gameplay does not log sections.
            static std::unordered_set<std::string> loggedMaterialDebugTargets;
            if (materialDebugTarget && loggedMaterialDebugTargets.insert(materialPath).second)
                LOG_INFO(Graphics, "Wgpu material debug: target RVMAT {}", materialPath);
            try
            {
                auto cached = _materialBindings.find(materialPath);
                if (cached == _materialBindings.end())
                {
                    try
                    {
                        const auto source = Asset::Material::ParseRvMaterialFile(materialPath);
                        cached = _materialBindings
                                     .emplace(materialPath, Asset::Material::TranslateSemantics(
                                                                source, Asset::Material::TranslateMaterial(source)))
                                     .first;
                    }
                    catch (...)
                    {
                        // An unavailable optional material must not reopen and
                        // throw on every draw. Its inert cached entry preserves
                        // the legacy section texture/fallback for this mission.
                        _materialBindings.emplace(materialPath, Asset::Material::TranslatedMaterial{});
                        throw;
                    }
                }
                const auto& translated = cached->second;
                // One-shot diagnostic for the TreeAdv-family night-lighting investigation.
                // The source fields have family-specific meanings (not generic emissive), so
                // capture them verbatim before a family shader starts consuming them.
                if (std::getenv("WGR_TREEADV_LIGHT_TRACE") &&
                    (translated.shaderFamily == "TreeAdv" || translated.shaderFamily == "TreeAdvTrunk" ||
                     translated.shaderFamily == "TreeAdvTrans"))
                {
                    static std::unordered_set<std::string> loggedTreeAdvMaterials;
                    if (loggedTreeAdvMaterials.insert(materialPath).second)
                    {
                        LOG_INFO(Graphics,
                                 "TreeAdv material trace: path={} family={} ambient=[{:.3f},{:.3f},{:.3f},{:.3f}] "
                                 "diffuse=[{:.3f},{:.3f},{:.3f},{:.3f}] forced=[{:.3f},{:.3f},{:.3f},{:.3f}] "
                                 "emissive=[{:.3f},{:.3f},{:.3f},{:.3f}] specular=[{:.3f},{:.3f},{:.3f},{:.3f}] "
                                 "power={:.3f}",
                                 materialPath, translated.shaderFamily, translated.ambient[0], translated.ambient[1],
                                 translated.ambient[2], translated.ambient[3], translated.diffuse[0],
                                 translated.diffuse[1], translated.diffuse[2], translated.diffuse[3],
                                 translated.forcedDiffuse[0], translated.forcedDiffuse[1], translated.forcedDiffuse[2],
                                 translated.forcedDiffuse[3], translated.emissive[0], translated.emissive[1],
                                 translated.emissive[2], translated.emissive[3], translated.specular[0],
                                 translated.specular[1], translated.specular[2], translated.specular[3],
                                 translated.specularPower);
                    }
                }
                if (materialDebugTarget)
                    _materialDebugInfo.shaderFamily = translated.shaderFamily.c_str();
                // A P3DM face may name only an RVMAT and leave its legacy
                // texture field blank.  For the explicit material-debug slice,
                // make Stage0 the draw's base texture so a normal-map smoke
                // exercises textured shading rather than a white fallback.
                if (const auto* base = translated.Find(Asset::Material::MaterialSlot::BaseColour);
                    base && !base->texture.isProcedural && !base->texture.path.empty())
                {
                    Ref<Texture> baseTexture = _wbank->Load(base->texture.path.canonical().c_str());
                    auto* wgpuBaseTexture = static_cast<TextureWgpu*>(baseTexture.GetRef());
                    const uint64_t baseHandle = wgpuBaseTexture ? wgpuBaseTexture->EnsureUploaded() : 0;
                    if (baseHandle)
                    {
                        d.texture_id = baseHandle;
                        static std::unordered_set<std::string> loggedBaseBindings;
                        if (loggedBaseBindings.insert(materialPath).second)
                            LOG_INFO(Graphics, "Wgpu material debug: Stage0 BaseColour resolved path={} uv={}",
                                     base->texture.path.canonical(), base->uvSource);
                    }
                    else
                    {
                        // A model whose face texture is a `_mlod_co` placeholder has no usable
                        // fallback: if this bind fails the section draws white, which is what the
                        // Arma 3 castle and brick props do. Logging only the success path is why
                        // that was invisible, so say so once per material.
                        static std::unordered_set<std::string> loggedBaseFailures;
                        if (loggedBaseFailures.insert(materialPath).second)
                            LOG_WARN(Graphics,
                                     "Wgpu material: Stage0 BaseColour did NOT bind for {} (path={} loaded={} "
                                     "uploaded=0) -- section keeps its face texture",
                                     materialPath, base->texture.path.canonical(), wgpuBaseTexture ? 1 : 0);
                    }
                }
                else
                {
                    static std::unordered_set<std::string> loggedNoBaseSlot;
                    if (loggedNoBaseSlot.insert(materialPath).second)
                        LOG_INFO(Graphics,
                                 "Wgpu material: no bindable Stage0 for {} (present={} procedural={} emptyPath={})",
                                 materialPath, base ? 1 : 0, base && base->texture.isProcedural ? 1 : 0,
                                 base && base->texture.path.empty() ? 1 : 0);
                }
                if (const auto* normal = translated.Find(Asset::Material::MaterialSlot::NormalMap))
                {
                    _materialDebugInfo.normalTexturePath = normal->texture.path.canonical().c_str();
                    _materialDebugInfo.normalUvSource = normal->uvSource.c_str();
                    const bool inspectNormal = _materialDebug.view == MaterialDebugSettings::View::NormalMap;
                    const bool enableNormal =
                        inspectNormal || (_materialDebug.view == MaterialDebugSettings::View::FullMaterial &&
                                          !_materialDebug.disableNormalMap);
                    if (normal->texture.isProcedural)
                    {
                        _materialDebugInfo.normalStatus = "procedural Stage1 is not yet sampled";
                    }
                    else if (normal->texture.path.empty())
                    {
                        _materialDebugInfo.normalStatus = "Stage1 has no loadable texture path";
                    }
                    else
                    {
                        Ref<Texture> texture = _wbank->Load(normal->texture.path.canonical().c_str());
                        auto* wgpuTexture = static_cast<TextureWgpu*>(texture.GetRef());
                        const uint64_t normalHandle = wgpuTexture ? wgpuTexture->EnsureUploaded() : 0;
                        _materialDebugInfo.normalResolved = normalHandle != 0;
                        _materialDebugInfo.normalStatus = normalHandle
                                                              ? "resolved and bound (NOHQ: X=A, Y=G, Z=reconstructed)"
                                                              : "texture failed to resolve/upload";
                        if (normalHandle)
                        {
                            static std::unordered_set<std::string> loggedNormalBindings;
                            if (loggedNormalBindings.insert(materialPath).second)
                                LOG_INFO(Graphics,
                                         "Wgpu material debug: Stage1 NormalMap resolved path={} uv={} "
                                         "decode=X=A,Y=G,Z=reconstructed",
                                         normal->texture.path.canonical(), normal->uvSource);
                        }
                        if (enableNormal)
                            d.normal_texture_id = normalHandle;
                    }
                }
                else
                    _materialDebugInfo.normalStatus = "no translated NormalMap stage";
            }
            catch (const std::exception& error)
            {
                _materialDebugInfo.normalStatus = error.what();
            }
        }
    }
    else if (_materialDebug.legacyEnhancement)
    {
        // MAT-LEGACY: optional modern materials for original OFP/CWA assets.
        //
        // CWA content has no RVMATs at all -- a section names a texture and nothing else -- so the
        // whole authored-material path above skips it and those surfaces render exactly as they did
        // in 2001. This gives them the SAME modern material representation the Real Virtuality
        // assets use, without touching a single original file: for a section's texture `X.pac`, if
        // a sibling `X_nohq.paa` exists it is bound as that section's normal map, through the very
        // same slot and the same NOHQ decode (X in A, Y in G, Z reconstructed) the RVMAT path uses.
        //
        // Deliberately a naming convention rather than a sidecar registry: it needs no index to
        // load or keep in sync, an enhancement pack is just files dropped next to the originals (or
        // ahead of them in the PBO search order), and an untouched installation has none of them so
        // every lookup misses and rendering is bit-identical to before.
        //
        // Both answers are cached, because the miss is the common case on stock content and a
        // filesystem probe per section per frame would be absurd.
        if (const Texture* faceTex = sMesh.GetSection(beg).properties.GetTexture())
        {
            if (const char* texName = faceTex->Name(); texName && *texName)
            {
                const uint64_t handle = LegacyEnhancementNormal(texName);
                if (handle != 0 && !_materialDebug.disableNormalMap)
                    d.normal_texture_id = handle;
            }
        }
    }
    std::memcpy(d.world.m, &_world, sizeof(d.world.m));
    // Terrain conform: publish this instance's bilinear plane so the vertex
    // shader conforms the (undeformed) shared mesh, matching ForestPlain::Animate. d is
    // zero-initialised, so conform2.z (mode) stays 0 for every non-conformed draw.
    if (GCurrentConformPlane.active)
    {
        const ConformPlane& cf = GCurrentConformPlane;
        if (cf.mode == 2)
        {
            // Individual ClipLand vegetation: the vertex shader samples SurfaceY per
            // vertex (mesh conform group) using bcSurfaceY; the plane fields are unused.
            d.conform0 = {cf.bcSurfaceY, 0.0f, 0.0f, 0.0f};
            d.conform1 = {0.0f, 0.0f, 0.0f, 0.0f};
            d.conform2 = {0.0f, 0.0f, 2.0f, 0.0f};
        }
        else
        {
            // ForestPlain bilinear plane (mode 1).
            d.conform0 = {cf.invLandGrid, -cf.xf, -cf.zf, cf.bias};
            d.conform1 = {cf.y00, cf.y10, cf.d1000, cf.d0100};
            d.conform2 = {cf.d1011, cf.d0111, 1.0f, 0.0f};
        }
    }
    // AlphaClass draw-mode override (design: AlphaStats — "Opaque and Cutout occlude: write
    // depth, alpha-test the holes"). BuildRenderPassDescriptor only sees the section SPEC, so a
    // face authored IsAlpha (-> AlphaBlend) over a Cutout-class texture (1-bit / punch-through
    // holes: badges, vehicle grills, fences) would soft-blend its filtered edges against whatever
    // is already in the framebuffer. It draws in the OPAQUE pass (SectionClassFilter routes
    // Cutout there), before the geometry behind it, so that "whatever" is the sky fill — the
    // long-standing see-through grill/badge bug. Force the Cutout class to a hard alpha-test
    // (opaque blend, mid alpha-ref): the holes discard and the solid occludes + depth-writes,
    // so the geometry behind shows through the holes and MSAA A2C still anti-aliases the edges.
    render::BlendMode blend = desc.blend;
    float alphaRef = AlphaRefFromDesc(desc);
    // TreeAdv leaf cards are authored with antialiased alpha, which the texture
    // histogram calls Blend. Their RVMAT family is nevertheless alpha-tested;
    // without this matching discard the black transparent texels depth-write in
    // the opaque fallback and completely hide the pine's trunk behind its cards.
    // TreeAdvTrunk is intentionally excluded: its bark map has non-coverage alpha
    // and must remain a solid, untested draw.
    auto* texture = static_cast<TextureWgpu*>(sMesh.GetSection(beg).properties.GetTexture());
    const bool materialUsesLeafCards =
        alphaClass == Poseidon::AlphaStats::Blend && GpuSectionMaterialUsesLeafCards(debugSection.surfMat);
    const bool materialCutout = materialUsesLeafCards && GpuSectionMaterialIsCutout(debugSection.surfMat, texture);
    if (GpuTextureIsBarkOrTrunk(texture))
    {
        // The common legacy section descriptor itself can carry a stale alpha
        // reference even for TreeAdvTrunk.  Texture role is authoritative for
        // these bark maps, so remove that inherited discard unconditionally.
        blend = render::BlendMode::Opaque;
        alphaRef = 0.0f;
    }
    else if (materialUsesLeafCards && !materialCutout)
    {
        // A close tree LOD may use the generic TreeAdv material for BOTH its needle cards and
        // its bark. The bark texture has a non-coverage alpha channel, so a leaf alpha test
        // removes it. It still belongs in the opaque pass, just without the discard.
        blend = render::BlendMode::Opaque;
        alphaRef = 0.0f;
    }
    else if (tex != 0 && (alphaClass == Poseidon::AlphaStats::Cutout || materialCutout))
    {
        // A cutout classification means the source has binary transparent texels, irrespective
        // of whether the P3D section remembered to set IsAlpha.  Some legacy sections are
        // authored as opaque over a DXT1/PAC cutout; they still need to discard transparent
        // texels rather than sampling that texture's black background.  A hard discard is the
        // appropriate common mode for every Cutout texture: keep the blend opaque and only
        // raise the alpha reference to the binary midpoint.
        blend = render::BlendMode::Opaque;
        alphaRef = std::max(alphaRef, 0.5f);
    }
    else if (blend == render::BlendMode::AlphaBlend && tex != 0 && alphaClass == Poseidon::AlphaStats::Opaque &&
             MaterialIsFullyOpaque(_curMaterial))
    {
        // The same class of bug as the Cutout case above, one step further out.
        // Backend::IsAlpha means "this face's texture has an alpha channel", not
        // "blend this face" -- BuildRenderPassDescriptor maps it to AlphaBlend
        // because it only sees the section SPEC. When the texture then turns out
        // to carry no partial *and* no clear alpha (AlphaStats::Opaque -- e.g.
        // OFP head textures, DXT1 with a fully opaque alpha channel), there is
        // nothing for blending to do except soft-blend the surface against
        // whatever is already in the framebuffer. That is the translucent-faces
        // report: a solid head rendered semi-transparent.
        //
        // Three conditions, all necessary. A texture must be present, because
        // alphaClass defaults to Opaque when there is none and an untextured
        // AlphaBlend draw is a genuine colour fade. The texture must classify
        // Opaque, so Cutout and Blend keep their own routing. And the material
        // must be fully opaque, because a fade driven by material alpha over an
        // opaque texture is real translucency and must keep blending.
        blend = render::BlendMode::Opaque;
        alphaRef = 0.0f;
    }
    d.blend = BlendFromDesc(blend);
    d.sampler = U32(SamplerForSpec(effectiveSpec));
    d.depth = DepthFromDesc(desc.depth);
    d.alpha_ref = alphaRef;
    if (const char* trace = std::getenv("WGR_LEGACY_TEXTURE_TRACE"); trace && std::strcmp(trace, "0") != 0)
    {
        if (const Texture* t = sMesh.GetSection(beg).properties.GetTexture())
        {
            const char* name = t->Name();
            if (name && *name)
            {
                static std::unordered_set<std::string> loggedLegacyDrawModes;
                if (loggedLegacyDrawModes.insert(name).second)
                    LOG_INFO(Graphics,
                             "Legacy texture draw trace: path={} vegetation={} alphaClass={} alphaRef={:.2f} blend={}",
                             name, GCurrentIsVegetation, int(alphaClass), d.alpha_ref, int(d.blend));
            }
        }
    }
    // Polygon-offset selection (ignored for shadows, which get their own offset):
    //  - OnSurface routing (roads / footprint decals): the light decal offset.
    //  - Otherwise ZBias overlay faces (traffic-sign decals etc.) flagged via
    //    SetBias(level*5): a stronger, level-scaled offset. GL33 leaves these to a
    //    biased projection it never actually applies for HW-T&L, so they z-fight in
    //    wgpu; the level-scaled offset resolves them without over-biasing roads.
    d.flags = 0;
    if (desc.blend != render::BlendMode::Shadow)
    {
        if (desc.surface == render::SurfaceMode::OnSurface)
        {
            d.flags |= WGR_DRAW3D_ON_SURFACE;
        }
        else if (_bias > 0)
        {
            const uint32_t level = std::clamp(_bias / 5, 1, 3);
            d.flags |= level << WGR_DRAW3D_ZBIAS_SHIFT;
        }
    }
    d.camera = _currentCamera;
    d.palette_slot = _currentPaletteSlot;

    // Per-material sun lighting, folded exactly like GL33's
    // UploadVSMaterialConstants: raw MainLight colour x captured material, with
    // the sun-enable (!DisableSun) multiplied into the sun terms (emissive shows
    // regardless). The lit shader does emissive + sun_ambient + sun_diffuse * N.L,
    // clamped, x texture — the per-fragment analogue of GL33's VSNormal + PSNormal.
    Color sunDif = HWhite;
    Color sunAmb = HWhite;
    if (GScene && GScene->MainLight())
    {
        sunDif = GScene->MainLight()->Diffuse();
        sunAmb = GScene->MainLight()->Ambient();
    }
    const float sunEn = render::Has(splitSpec.material, render::Material::DisableSun) ? 0.0f : 1.0f;
    const Color diffuse = sunDif * _curMaterial.diffuse;
    const Color ambient = sunAmb * _curMaterial.ambient + sunDif * _curMaterial.forcedDiffuse;
    // Imported plant materials used the legacy emissive lane as fixed-function
    // brightness compensation. In the HDR path that becomes real radiance and
    // leaves forests visibly glowing at night. Retain only a small starlight
    // residual, matching the retained path; authored emissive on fixtures stays.
    if (GCurrentIsVegetation)
    {
        constexpr float vegetationLegacyEmissive = 0.10f;
        const Color emissive = _curMaterial.emmisive;
        d.mat_emissive = {emissive.R() * vegetationLegacyEmissive, emissive.G() * vegetationLegacyEmissive,
                          emissive.B() * vegetationLegacyEmissive, emissive.A()};
    }
    else
    {
        const Color emissive = _curMaterial.emmisive;
        const float scale = StaticEmitterRadianceScale(debugSection.surfMat, emissive);
        d.mat_emissive = {emissive.R() * scale, emissive.G() * scale, emissive.B() * scale, emissive.A()};
    }
    d.mat_sun_ambient = {ambient.R() * sunEn, ambient.G() * sunEn, ambient.B() * sunEn, ambient.A() * sunEn};
    d.mat_sun_diffuse = {diffuse.R() * sunEn, diffuse.G() * sunEn, diffuse.B() * sunEn, diffuse.A() * sunEn};
    // Point/spot-light material modulation (GL33's matDif/matAmb): the raw material
    // diffuse/ambient (accommodation in, night rides the per-light colour).
    const Color& lightDif = _curMaterial.diffuse;
    const Color& lightAmb = _curMaterial.ambient;
    d.mat_light_diffuse = {lightDif.R(), lightDif.G(), lightDif.B(), lightDif.A()};
    d.mat_light_ambient = {lightAmb.R(), lightAmb.G(), lightAmb.B(), lightAmb.A()};
    // Sun-only Blinn-Phong specular, folded exactly like GL33's c18 (specCol =
    // sun->Diffuse() * mat.specular, power in w). The shader adds it per-fragment,
    // gated on w > 0, so fold the sun-enable into the colour and leave w = power.
    const Color specCol = sunDif * _curMaterial.specular;
    const float specPow = float(_curMaterial.specularPower);
    const float specEn = (specPow > 0.0f) ? sunEn : 0.0f;
    d.mat_specular = {specCol.R() * specEn, specCol.G() * specEn, specCol.B() * specEn, specPow};
    // Foliage class for the direct WGPU path: only real plants get the leaf look, and the vertex
    // shader receives tree vs bush so it can use the matching radial-crown normal. This rides the
    // otherwise unused .w lane; fs_main treats any positive value as vegetation.
    d.mat_sun_ambient.w = GCurrentFoliageKind;

    _draws3d.push_back(d);
    _cmds.push_back(WgrCmd{WGR_CMD_DRAW_3D, U32(_draws3d.size() - 1)});
}

// --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) ---

namespace
{

// Classify one section of a graphical LOD for the GPU-driven path: fill its geometry range
// (resolved to the shared pool by the Rust side) + RAW material + variant (0 = solid, 1 =
// alpha-cutout), returning false if the section is NOT eligible (transparent, additive,
// on-surface decal, or empty) — which makes the whole shape stay on the CPU path. Mirrors
// DrawSectionTL's texture/sampler/alpha/material derivation, minus the sun fold (done
// in-shader) and the per-object/mesh spec (registration is per-shape, object spec ~0 for
// static clutter).
//
// Texture alpha statistics cannot distinguish a blended surface from an anti-aliased
// alpha-tested leaf card. Shape::Draw resolves that ambiguity from the authored RVMAT shader
// family; retained sections must use the same authority or TreeAdv pines become solid cards.
bool GpuSectionMaterialUsesLeafCards(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
    {
        return false;
    }
    const char* path = surfMat->GetName().Data();
    if (!path || !*path)
    {
        return false;
    }

    static std::unordered_map<std::string, bool> cache;
    static std::mutex cacheMutex;
    const std::string key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto found = cache.find(key); found != cache.end())
        {
            return found->second;
        }
    }

    bool cutout = false;
    try
    {
        const std::string& family = Asset::Material::ParseRvMaterialFile(key).pixelShaderId;
        cutout = family == "TreeAdv" || family == "TreeAdvTrans";
    }
    catch (...)
    {
        // Preserve the texture classification when an optional authored material is unavailable.
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = cutout;
    return cutout;
}

// A3's closest pine LOD uses TreeAdv for both the crown cards and the real bark
// section.  The bark's alpha is a packed/non-coverage channel, and happens to
// contain clear texels, so alpha statistics alone cannot tell those two roles
// apart.  The texture's authored role can: every A3 bark map lives in _bark or
// carries a bark/trunk name.  Keep it opaque while the actual polyplane cards
// retain the TreeAdv alpha test.
bool GpuTextureIsBarkOrTrunk(const TextureWgpu* texture)
{
    const char* path = texture ? texture->Name() : nullptr;
    return path && (ContainsNoCase(path, "\\_bark\\") || ContainsNoCase(path, "bark_") ||
                    ContainsNoCase(path, "_bark") || ContainsNoCase(path, "\\trunk") ||
                    ContainsNoCase(path, "_trunk"));
}

bool GpuSectionMaterialIsCutout(const Ref<TexMaterial>& surfMat, TextureWgpu* texture)
{
    return texture != nullptr && !GpuTextureIsBarkOrTrunk(texture) && texture->HasAlphaHoles() &&
           GpuSectionMaterialUsesLeafCards(surfMat);
}

bool ClassifyGpuSection(const Shape& s, uint64_t mesh, const AutoArray<render::mesh::MeshSection>& secs, int i,
                        WgrModelSection& secOut, WgrModelMaterial& matOut)
{
    if (i >= secs.Size())
    {
        return false;
    }
    const ShapeSection& sec = s.GetSection(i);
    const int spec = sec.properties.Special();
    const render::LegacySpec split = render::SplitLegacy(spec);
    render::BuildContext ctx;
    ctx.isIn3DPass = true;
    ctx.shadowAlphaRef = 0;
    const render::RenderPassDescriptor desc = render::BuildRenderPassDescriptor(split, ctx);
    // Plain opaque surfaces only. Transparent/additive/shadow blends and on-surface decals
    // (roads/footprints, which need a polygon-offset) are the CPU complement — the SAME
    // predicate Shape::Draw uses to skip GPU-owned sections when it draws a Partial object,
    // so the two can never disagree (double-draw / hole). See IsGpuOwnedSection.
    if (!render::IsGpuOwnedSection(desc))
    {
        return false;
    }

    const render::mesh::MeshSection& ms = secs[i];
    const int indexCount = ms.end - ms.beg;
    if (indexCount <= 0)
    {
        return false;
    }
    secOut.mesh = mesh;
    secOut.index_begin = U32(ms.beg);
    secOut.index_count = U32(indexCount);
    uint64_t tex = 0;
    Poseidon::AlphaStats::Kind alphaClass = Poseidon::AlphaStats::Opaque;
    auto* texture = static_cast<TextureWgpu*>(sec.properties.GetTexture());
    if (texture)
    {
        tex = texture->EnsureUploaded();
        alphaClass = texture->GetAlphaClass();
    }
    const bool materialCutout =
        alphaClass == Poseidon::AlphaStats::Blend && GpuSectionMaterialIsCutout(sec.surfMat, texture);
    // Genuine blended surfaces stay on the CPU's back-to-front complement. Only foliage whose
    // authored shader explicitly identifies it as alpha-tested can stay in the retained pass.
    if (alphaClass == Poseidon::AlphaStats::Blend && !materialCutout)
    {
        return false;
    }
    // Retained sections must use the same texture-derived cutout rule as
    // DrawSectionTL.  Arma vegetation often stores punch-through alpha in a
    // DXT1/PAC texture while its P3D section itself is marked opaque.  Without
    // this override the retained path draws each leaf card's dark background as
    // a solid rectangle, even though the CPU path correctly discards it.
    float alphaRef = AlphaRefFromDesc(desc);
    if (tex != 0 && (alphaClass == Poseidon::AlphaStats::Cutout || materialCutout))
    {
        alphaRef = std::max(alphaRef, 0.5f);
    }
    secOut.variant = alphaRef > 0.0f ? 1u : 0u;
    secOut._pad = 0;
    // Raw per-section material (folded with the sun in the GPU-driven FS). Base = the
    // shading-type material at neutral accommodation (HDR auto-exposure handles adaptation),
    // then modulated by the section surface material. forcedDiffuse is dropped (the FS has no
    // term for it; ~always black on static clutter).
    TLMaterial m;
    CreateMaterial(m, HWhite, sec.material);
    if (sec.surfMat)
    {
        TLMaterial mod;
        sec.surfMat->Combine(mod, m);
        m = mod;
    }
    const float emitterScale = StaticEmitterRadianceScale(sec.surfMat, m.emmisive);
    matOut.emissive = {m.emmisive.R() * emitterScale, m.emmisive.G() * emitterScale,
                       m.emmisive.B() * emitterScale, m.emmisive.A()};
    matOut.ambient = {m.ambient.R(), m.ambient.G(), m.ambient.B(), m.ambient.A()};
    matOut.diffuse = {m.diffuse.R(), m.diffuse.G(), m.diffuse.B(), m.diffuse.A()};
    matOut.specular = {m.specular.R(), m.specular.G(), m.specular.B(), float(m.specularPower)};
    matOut.texture_id = tex;
    matOut.sampler = U32(SamplerForSpec(spec));
    matOut.alpha_ref = alphaRef;
    return true;
}

// Build a retained instance from an object: absolute world transform (the GPU-driven VS
// subtracts cam_pos), world bounding-sphere center + uniform scale (both read by the cull).
// `cp` is the terrain-conform plane (cp.mode: 0 rigid, 1 ForestPlain bilinear plane, 2 ClipLand
// per-vertex SurfaceY); packed into conform0/1/2 exactly like the per-draw path (WgrDraw3D).
WgrInstance BuildGpuInstance(const Object& obj, uint32_t model, const ConformPlane& cp)
{
    WgrInstance inst{};
    GfxMatrix g;
    ConvertMatrix(g, obj.Transform()); // absolute model->world (NOT camera-relative here)
    std::memcpy(inst.world.m, &g, sizeof(inst.world.m));
    // Cull-sphere center = the object ORIGIN (Transform.Position()), NOT Transform*BoundingCenter.
    // LODShape::CalculateBoundingSphere physically re-centers the stored vertices around the
    // vertex-space origin (ShapeLOD.cpp: `pos -= changeBoundingCenter`), and `_boundingSphere` is
    // the radius about THAT origin — which the VS draws at Transform.Position(). BoundingCenter is
    // only the offset back to pre-recenter coords; using it here displaces the sphere from the
    // drawn geometry and culls offset-origin objects too early. Matches the legacy cull
    // (SceneDraw.cpp: center = trans.Position(), radius = BoundingSphere()*Scale).
    Vector3 center = obj.Transform().Position();
    inst.model = model;
    // Terrain-conform plane -> conform0/1/2, matching DrawSectionTL's per-draw fold.
    if (cp.mode == 1)
    {
        inst.conform0 = {cp.invLandGrid, -cp.xf, -cp.zf, cp.bias};
        inst.conform1 = {cp.y00, cp.y10, cp.d1000, cp.d0100};
        inst.conform2 = {cp.d1011, cp.d0111, 1.0f, 0.0f};
    }
    else if (cp.mode == 2)
    {
        inst.conform0 = {cp.bcSurfaceY, 0.0f, 0.0f, 0.0f};
        inst.conform2 = {0.0f, 0.0f, 2.0f, 0.0f};
    }
    // else rigid: conform2.z stays 0 (zero-initialised).

    // Cull sphere == the CPU's exactly: raw Transform.Position() + the model bounding sphere
    // (Scene.cpp:801). No conforming, no snapping, no inflation — CULLDUMP proved every
    // "disappears at horizon pitch" object was a STALE registration (the object moved 4-17 m
    // down after SceneObjectCreated captured it; the live Position is correct), which the
    // GpuDrivenObject drift check now self-heals. Inflating the radius to reach the terrain was
    // the wrong tool: it just blurs the cull, and a stale centre + stale registration-time
    // SurfaceY inflate to a sphere that misses the real ground anyway.
    inst.center = {center.X(), center.Y(), center.Z(), obj.Scale()};

    // Spherical/canopy normals (docs/foliage-translucency-plan.md Stage 3 + §9): flag vegetation so
    // vs_gpu bends its cutout (leaf) normals toward a radial crown normal, shading the low-poly
    // canopy as a rounded volume (fixes back-facing cards that stay dark in full sun). Bush and tree
    // are distinguished so each picks its own bend + crown-Y lift (a tree's bounding-sphere centre
    // sits mid-trunk, so it wants a larger lift; the trunk sections are solid, not cutout, so they
    // keep their real normal). A FOREST is a merged multi-tree mesh whose single centre is
    // meaningless per-tree, so it carries per-vertex crown centres baked in RegisterGpuModel (§9
    // Approach A) and reuses the tree bend/crown-Y knobs — no longer excluded.
    if (const LODShapeWithShadow* s = obj.GetShape(); s)
    {
        const MapType mt = s->GetMapType();
        if (mt == MapBush)
        {
            inst.flags |= WGR_INSTANCE_CANOPY_BUSH;
        }
        else if (mt == MapTree || mt == MapSmallTree)
        {
            inst.flags |= WGR_INSTANCE_CANOPY_TREE;
        }
        else if (mt == MapForestBorder || mt == MapForestTriangle || mt == MapForestSquare)
        {
            inst.flags |= WGR_INSTANCE_CANOPY_FOREST;
        }
    }

    // WGR_CONFORM_DEBUG: dump each retained instance's conform mode + how far its origin
    // floats above the terrain, so a "floating tree" can be identified by shape name and its
    // mode (0 rigid / 1 forest / 2 clipland) checked against whether the CPU would conform it.
    static const bool dbg = std::getenv("WGR_CONFORM_DEBUG") != nullptr;
    if (dbg)
    {
        static int dbgCount = 0; // registration is single-threaded (world load)
        if (dbgCount++ < 60)
        {
            const float surf = GLandscape ? GLandscape->SurfaceY(center.X(), center.Z()) : 0.0f;
            const LODShapeWithShadow* s = obj.GetShape();
            LOG_INFO(Graphics, "CONFORM name={} mode={} posY={} surfY={} above={} scale={}", s ? s->Name() : "?",
                     cp.mode, center.Y(), surf, center.Y() - surf, obj.Scale());
        }
    }
    return inst;
}

// §12d: a proxy child instance — rigid interior furniture at an explicit COMPOSED world
// transform (parentTransform * proxyLocalTransform), which is static for a static parent. No
// conform (mode 0). Cull sphere center = the composed origin, radius = the proxy model's own
// bounding sphere * the composed scale (registered with wgr_model_register), matching
// BuildGpuInstance and the CPU cull.
static WgrInstance BuildGpuProxyInstance(const Matrix4& world, uint32_t model)
{
    WgrInstance inst{};
    GfxMatrix g;
    ConvertMatrix(g, world);
    std::memcpy(inst.world.m, &g, sizeof(inst.world.m));
    inst.model = model;
    const Vector3 center = world.Position();
    inst.center = {center.X(), center.Y(), center.Z(), world.Scale()};
    return inst;
}

// bcSurfaceY for a conform (ClipLand) instance: the surface height at the object's ground
// reference, matching Object::PublishConformPlane (mode 2). 0 for non-conform / no landscape.
static float GpuConformBcSurfaceY(const Object& obj, const LODShapeWithShadow& shape)
{
    if (!GLandscape)
    {
        return 0.0f;
    }
    Matrix4Val toWorld = obj.Transform();
    Vector3 bc(VFastTransform, toWorld, -shape.BoundingCenter());
    return GLandscape->SurfaceY(bc[0], bc[2]);
}

// Terrain-conform plane for a retained instance. ForestPlain (forests + shrub groups) uses a
// MODE-1 bilinear land-grid plane (its cached ConformPlane; skewed t1/t2 squares bake conform
// into the transform and report rigid). Individual ClipLand vegetation/fences use MODE 2
// (per-vertex SurfaceY). `clipLandConform` = the shape carries ClipLand hints (RegisterGpuModel
// recorded it in _gpuConformShapes). Anything else is rigid (mode 0).
static ConformPlane GpuConformFor(Object& obj, const LODShapeWithShadow& shape, bool clipLandConform)
{
    ConformPlane cp{};
    cp.active = false;
    cp.mode = 0; // rigid by default (struct default is mode 1)
    if (ForestPlain* fp = dyn_cast<ForestPlain>(&obj))
    {
        ConformPlane fpc;
        if (fp->GpuConformPlane(fpc)) // false for skewed squares -> stays rigid
        {
            cp = fpc; // mode 1
        }
        return cp;
    }
    if (clipLandConform)
    {
        cp.mode = 2;
        cp.bcSurfaceY = GpuConformBcSurfaceY(obj, shape);
    }
    return cp;
}

// §9 Approach A: flood-fill a merged forest LOD mesh into per-tree connected components so each
// tree gets its own crown centre for spherical (radial) normals. A forest is one authored mesh
// with all trees' cards baked into a single vertex/index table (no per-tree placement), so the
// single instance centre is meaningless per tree. Two vertices are in the same component if they
// share a triangle OR sit at the (near-)same position — the position weld reconnects UV/normal
// seam duplicates that Optimize/SortVertices leaves as distinct indices. Fills `compOut[i]` with
// each vertex's dense component id and appends each component's MODEL-space centroid to
// `centresOut`; returns the component count. Risk (accepted, plan-noted): two trees whose canopies
// touch closely enough to share a welded vertex merge into one component.
//
// The per-tree centroid is exactly the pivot future tree-sway will animate around, so this data is
// reusable beyond lighting.
static uint32_t BuildForestCrownComponents(const std::vector<SVertex>& verts, const std::vector<VertexIndex>& indices,
                                           std::vector<WgrVec4>& centresOut, std::vector<uint32_t>& compOut)
{
    const uint32_t nv = U32(verts.size());
    compOut.assign(nv, 0);
    if (nv == 0)
    {
        return 0;
    }

    // Union-find with path halving.
    std::vector<uint32_t> parent(nv);
    for (uint32_t i = 0; i < nv; i++)
    {
        parent[i] = i;
    }
    auto find = [&](uint32_t x)
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](uint32_t a, uint32_t b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
        {
            parent[a] = b;
        }
    };

    // Position weld: union vertices quantised to the same 1 mm cell (tight, so only true seam
    // duplicates merge — distinct trees are metres apart).
    constexpr double eps = 1.0e-3;
    std::map<std::array<int64_t, 3>, uint32_t> weld;
    for (uint32_t i = 0; i < nv; i++)
    {
        const std::array<int64_t, 3> key{std::llround(verts[i].pos.X() / eps), std::llround(verts[i].pos.Y() / eps),
                                         std::llround(verts[i].pos.Z() / eps)};
        auto [it, inserted] = weld.try_emplace(key, i);
        if (!inserted)
        {
            unite(i, it->second);
        }
    }

    // Triangle adjacency: the three corners of a face are one component.
    for (size_t t = 0; t + 3 <= indices.size(); t += 3)
    {
        const uint32_t a = uint32_t(uint16_t(indices[t]));
        const uint32_t b = uint32_t(uint16_t(indices[t + 1]));
        const uint32_t c = uint32_t(uint16_t(indices[t + 2]));
        if (a < nv && b < nv && c < nv)
        {
            unite(a, b);
            unite(a, c);
        }
    }

    // Densify roots -> component ids and accumulate centroids (double sums for stability).
    std::map<uint32_t, uint32_t> rootToComp;
    std::vector<double> sx, sy, sz;
    std::vector<uint32_t> count;
    for (uint32_t i = 0; i < nv; i++)
    {
        const uint32_t root = find(i);
        uint32_t comp;
        if (auto it = rootToComp.find(root); it != rootToComp.end())
        {
            comp = it->second;
        }
        else
        {
            comp = U32(sx.size());
            rootToComp.emplace(root, comp);
            sx.push_back(0.0);
            sy.push_back(0.0);
            sz.push_back(0.0);
            count.push_back(0);
        }
        compOut[i] = comp;
        sx[comp] += verts[i].pos.X();
        sy[comp] += verts[i].pos.Y();
        sz[comp] += verts[i].pos.Z();
        count[comp]++;
    }

    const uint32_t ncomp = U32(sx.size());
    for (uint32_t c = 0; c < ncomp; c++)
    {
        const double inv = count[c] > 0 ? 1.0 / double(count[c]) : 0.0;
        centresOut.push_back(WgrVec4{float(sx[c] * inv), float(sy[c] * inv), float(sz[c] * inv), 0.0f});
    }
    return ncomp;
}

} // namespace

// How much of a material's flat specular constant to apply.
//
// In Arma 3 that constant is what a per-texel specular map scales -- SMDI on the
// Super and Skin families, and the equivalents the NormalMap*Specular* families
// name. Applying the constant with no map is the maximum the material could ever
// reach on any texel, which lit whole rooftops as if every point were a mirror.
//
// A real specular map averages well below white, so scaling the constant
// approximates its mean far better than either extreme: applying it in full
// blows out, and dropping it entirely removes glint the surfaces should have.
// The factor is deliberately tunable (WGR_MATERIAL_SPEC_UNMAPPED) because the
// right value is a look judgement, not a measurement.
//
// Once the map IS bound, this steps aside completely and returns 1.0: the map is
// the thing the constant was authored to be scaled by, and an approximation of a
// quantity applied on top of the quantity itself would darken it twice.
//
// Returns 1.0 for anything that is not a translated RVMAT, so OFP's CfgMaterials
// surfaces are untouched.
float EngineWgpu::MaterialSpecularScale(const ShapeSection& section, uint64_t specularMap)
{
    if (specularMap)
        return 1.0f;
    if (!section.surfMat)
        return 1.0f;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return 1.0f;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end() || !cached->second.schemaKnown)
        return 1.0f; // ResolveMaterialBaseColour fills the cache first
    static const float scale = []
    {
        if (const char* v = std::getenv("WGR_MATERIAL_SPEC_UNMAPPED"))
            return std::clamp(std::strtof(v, nullptr), 0.0f, 1.0f);
        return 0.25f;
    }();
    return scale;
}

// The per-texel specular map a section's RVMAT names, or 0 when it names none.
//
// Same shape as ResolveMaterialBaseColour and for the same reason: the retained
// GPU-driven set owns essentially every Stratis section, so a lookup that lives
// only in the per-draw path reaches almost nothing.
//
// A procedural SMDI stand-in is a genuine constant (`color(1,0,1,0,SMDI)` is a
// specular level of zero) and is generated like any other, so it binds here too --
// which matters, because on the Super family they outnumber the file maps.
uint64_t EngineWgpu::ResolveMaterialSpecularMap(const ShapeSection& section)
{
    if (!section.surfMat)
        return 0;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return 0;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return 0; // ResolveMaterialBaseColour fills the cache first

    // Once per material, so the change is measurable the same way the albedo coverage
    // was: how many materials carry a bright specular constant and still have no map
    // is exactly the population the temporary approximation exists for.
    const auto report = [&](uint64_t handle, const char* why, const std::string& detail)
    {
        static std::unordered_set<std::string> logged;
        if (logged.insert(materialPath).second)
        {
            const auto& constant = cached->second.specular;
            LOG_INFO(Graphics, "Wgpu material: specular map {} for {} family={} constant={:.2f} ({}{}{})",
                     handle ? "bound" : "NONE", materialPath, cached->second.shaderFamily,
                     std::max({constant[0], constant[1], constant[2]}), why, detail.empty() ? "" : " ", detail);
        }
        return handle;
    };

    const auto* spec = cached->second.Find(Asset::Material::MaterialSlot::SpecularDetail);
    if (!spec)
        return report(0, "no SpecularDetail slot", "");
    std::string loadName = spec->texture.path.canonical();
    if (spec->texture.isProcedural)
    {
        if (!render::procedural::Parse(spec->texture.raw.c_str()).generated)
            return report(0, "procedural lookup", spec->texture.raw);
        loadName = spec->texture.raw;
    }
    else if (loadName.empty())
    {
        return report(0, "empty path", "");
    }
    Ref<Texture> texture = _wbank->Load(loadName.c_str());
    auto*        wgpuTexture = static_cast<TextureWgpu*>(texture.GetRef());
    const uint64_t handle = wgpuTexture ? wgpuTexture->EnsureUploaded() : 0;
    return report(handle, handle ? "" : "upload failed", loadName);
}

// The tangent-space normal map a section's RVMAT names, or 0 when it names none.
//
// Third of the same shape, and the same asymmetry: DrawSectionTL has bound Stage1
// since the normal-map work landed, while ClassifyGpuSection never did. GPU-driven
// is on by default and the retained set owns essentially every world section, so
// until now almost nothing in a rendered world was normal-mapped -- on any
// generation. The Arma 2 corpus made it obvious because its surfaces are rougher,
// but Stratis was equally flat.
//
// A procedural Stage1 is a genuine constant (`#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)`
// is a flat normal) and generates like any other, so it binds here too.
uint64_t EngineWgpu::ResolveMaterialNormalMap(const ShapeSection& section)
{
    // NOT honoured here any more. Baking the toggle into the material record meant it
    // only applied to shapes registered AFTER the flip, and nothing re-registers a live
    // one -- so on a loaded world it did nothing at all. The retained shader now reads it
    // per fragment from the frame UBO (PushMaterialDebug), which also keeps the record
    // truthful: a record that had dropped its normal slot could not get it back when the
    // toggle was flipped off again.
    // MAT-LEGACY in the retained set. Original OFP/CWA content has no RVMAT at all, so
    // everything below finds nothing and those surfaces shade as they did in 2001. The
    // `<texture>_nohq.paa` enhancement is the one mechanism that can give them a normal
    // map -- and it lived only in DrawSectionTL, which draws a handful of sections a
    // frame, so an enhancement pack was invisible in the actual world for the same reason
    // the authored normal maps were. Stock content has no such files and still resolves to
    // 0 here, at the cost of one cached hash lookup.
    const auto legacy = [&]() -> uint64_t
    {
        if (!_materialDebug.legacyEnhancement)
            return 0;
        auto* texture = section.properties.GetTexture();
        const char* name = texture ? texture->Name() : nullptr;
        return name && *name ? LegacyEnhancementNormal(name) : 0;
    };
    if (!section.surfMat)
        return legacy();
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return legacy();
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return 0; // ResolveMaterialBaseColour fills the cache first

    const auto report = [&](uint64_t handle, const char* why, const std::string& detail)
    {
        static std::unordered_set<std::string> logged;
        if (logged.insert(materialPath).second)
            LOG_INFO(Graphics, "Wgpu material: normal map {} for {} family={} ({}{}{})",
                     handle ? "bound" : "NONE", materialPath, cached->second.shaderFamily, why,
                     detail.empty() ? "" : " ", detail);
        return handle;
    };

    const auto* normal = cached->second.Find(Asset::Material::MaterialSlot::NormalMap);
    if (!normal)
    {
        // A material that names no normal can still have a sibling enhancement.
        if (const uint64_t enhanced = legacy())
            return report(enhanced, "legacy enhancement", "");
        return report(0, "no NormalMap slot", "");
    }
    std::string loadName = normal->texture.path.canonical();
    if (normal->texture.isProcedural)
    {
        if (!render::procedural::Parse(normal->texture.raw.c_str()).generated)
            return report(0, "procedural lookup", normal->texture.raw);
        loadName = normal->texture.raw;
    }
    else if (loadName.empty())
    {
        return report(0, "empty path", "");
    }
    Ref<Texture> texture = _wbank->Load(loadName.c_str());
    auto*        wgpuTexture = static_cast<TextureWgpu*>(texture.GetRef());
    const uint64_t handle = wgpuTexture ? wgpuTexture->EnsureUploaded() : 0;
    return report(handle, handle ? "" : "upload failed", loadName);
}

// Multi's mask and its three further surface layers, each with its own UV transform.
//
// MAT-039: the family is a four-layer masked blend and only layer 0 was ever bound, so a
// brick house rendered as the rock underneath it. Each layer is authored at its own
// TexGen -- brick tiles far more densely than rock -- and that scale difference is the
// detail the surface reads as, so the transform travels with the layer.
//
// Returns false for every other family, leaving mask_texture_id 0, which the shader
// reads as "not layered" and takes layer 0 alone.
bool EngineWgpu::ResolveMaterialLayers(const ShapeSection& section, WgrModelMaterial& material)
{
    // Identity for every layer, so a material that resolves nothing still samples at the
    // section's own UV rather than at zero.
    for (auto& uv : material.layer_uv)
    {
        uv[0] = 1.0f;
        uv[1] = 1.0f;
        uv[2] = 0.0f;
        uv[3] = 0.0f;
    }
    // Same as the normal map above: composeMultiLayers is a presentation switch honoured
    // per fragment by the retained shader, not baked into the record here.
    if (!section.surfMat)
        return false;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return false;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return false; // ResolveMaterialBaseColour fills the cache first

    const auto layerUv = [&](Asset::Material::MaterialSlot slot, size_t index)
    {
        const auto* entry = cached->second.Find(slot);
        if (!entry || !entry->uvTransform.present)
            return;
        // RVMAT authors these diagonally; the off-diagonal terms are zero in every
        // observed Multi material, so the scale/offset pair is lossless here.
        material.layer_uv[index][0] = entry->uvTransform.aside[0];
        material.layer_uv[index][1] = entry->uvTransform.up[1];
        material.layer_uv[index][2] = entry->uvTransform.pos[0];
        material.layer_uv[index][3] = entry->uvTransform.pos[1];
    };
    const auto bind = [&](Asset::Material::MaterialSlot slot) -> uint64_t
    {
        const auto* entry = cached->second.Find(slot);
        if (!entry || entry->texture.isProcedural || entry->texture.path.empty())
            return 0;
        Ref<Texture> texture = _wbank->Load(entry->texture.path.canonical().c_str());
        auto* wgpuTexture = static_cast<TextureWgpu*>(texture.GetRef());
        return wgpuTexture ? wgpuTexture->EnsureUploaded() : 0;
    };

    using Slot = Asset::Material::MaterialSlot;
    const uint64_t mask = bind(Slot::Mask);
    if (!mask)
        return false; // no mask, no blend to drive -- layer 0 stands alone

    material.mask_texture_id = mask;
    layerUv(Slot::BaseColour, 0);
    const Slot colours[3] = {Slot::LayerColour1, Slot::LayerColour2, Slot::LayerColour3};
    for (size_t i = 0; i < 3; ++i)
    {
        material.layer_texture_id[i] = bind(colours[i]);
        layerUv(colours[i], i + 1);
    }

    static std::unordered_set<std::string> logged;
    if (logged.insert(materialPath).second)
        LOG_INFO(Graphics, "Wgpu material: composed {} layers for {} family={} (uv scales {:.1f} {:.1f} {:.1f} {:.1f})",
                 1 + (material.layer_texture_id[0] != 0) + (material.layer_texture_id[1] != 0) +
                     (material.layer_texture_id[2] != 0),
                 materialPath, cached->second.shaderFamily, material.layer_uv[0][0], material.layer_uv[1][0],
                 material.layer_uv[2][0], material.layer_uv[3][0]);
    return true;
}

// The albedo a section's RVMAT names, or 0 when it names none.
//
// An Arma 3 structure commonly carries a placeholder face texture (`*_mlod_co`)
// with the real albedo living in the material, so a draw that uses only the face
// texture renders white. DrawSectionTL has always resolved this inline, but the
// GPU-driven retained set is built by ClassifyGpuSection, which never did -- and
// GPU-driven is on by default, so on Stratis essentially every object went down
// the path without it. Measured: 6,066 sections reach the adapter with a material
// attached, while only 7 sections a frame pass through DrawSectionTL.
uint64_t EngineWgpu::ResolveMaterialBaseColour(const ShapeSection& section)
{
    if (!section.surfMat)
        return 0;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return 0;

    auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
    {
        try
        {
            const auto source = Asset::Material::ParseRvMaterialFile(materialPath);
            cached =
                _materialBindings
                    .emplace(materialPath,
                             Asset::Material::TranslateSemantics(source, Asset::Material::TranslateMaterial(source)))
                    .first;
        }
        catch (...)
        {
            // Cache the miss: an unavailable material must not reopen and throw
            // once per section, every rebuild.
            _materialBindings.emplace(materialPath, Asset::Material::TranslatedMaterial{});
            return 0;
        }
    }

    // Say why, once per material. There are four distinct ways to come back with
    // nothing and they need different fixes: an unsupported shader family leaves
    // no BaseColour slot at all, a procedural stage has no file to load, and a
    // path that loads but uploads 0 is a texture-format problem.
    //
    // The fallback is reported with it, because "no albedo" is only a DEFECT when
    // the face texture cannot stand in. Most Arma 3 families never carry a Stage0:
    // Super declares one in 43 of 2,853, and TreeAdv in none of 369 -- for those
    // the face texture IS the colour and this path is the normal outcome, so
    // counting them as failures overstates the gap by an order of magnitude.
    //
    // What decides it is whether the fallback UPLOADS, not what it is called. An
    // `*_mlod_co` name reads like an author placeholder, and the ones Arma 3 ships
    // are megabyte-sized baked textures that render perfectly well -- so the name
    // is reported as a separate note and never as the verdict.
    const auto reject = [&](const char* why, const std::string& detail) -> uint64_t
    {
        static std::unordered_set<std::string> logged;
        if (!logged.insert(materialPath).second)
            return 0;
        auto*       face     = static_cast<TextureWgpu*>(section.properties.GetTexture());
        const char* faceName = face && face->GetName().Data() ? face->GetName().Data() : "";
        const char* fallback = "none";
        if (*faceName)
        {
            fallback = face->EnsureUploaded() != 0 ? "face-ok" : "face-failed";
        }
        LOG_WARN(Graphics, "Wgpu material: no albedo for {} ({}{}{}) family={} fallback={} mlodName={} face={}",
                 materialPath, why, detail.empty() ? "" : " ", detail, cached->second.shaderFamily, fallback,
                 ContainsNoCase(faceName, "_mlod_") ? 1 : 0, faceName);
        return 0;
    };

    const auto* base = cached->second.Find(Asset::Material::MaterialSlot::BaseColour);
    if (!base)
        return reject("no BaseColour slot", "");

    // A procedural stage is generated from its own name rather than loaded. The
    // constant-colour ones are a real albedo -- `mil_wallbig_multi` names its whole
    // surface that way -- so they are handed to the bank like any other texture.
    // The lookup-table generators (fresnel, irradiance) still have no
    // implementation and are reported as such rather than lumped together.
    std::string loadName = base->texture.path.canonical();
    if (base->texture.isProcedural)
    {
        if (!render::procedural::Parse(base->texture.raw.c_str()).generated)
            return reject("procedural lookup", base->texture.raw);
        loadName = base->texture.raw;
    }
    else if (base->texture.path.empty())
    {
        return reject("empty path", "");
    }

    Ref<Texture> baseTexture = _wbank->Load(loadName.c_str());
    auto* wgpuBaseTexture = static_cast<TextureWgpu*>(baseTexture.GetRef());
    const uint64_t handle = wgpuBaseTexture ? wgpuBaseTexture->EnsureUploaded() : 0;
    if (!handle)
        return reject(wgpuBaseTexture ? "uploaded 0" : "load failed", loadName);
    return handle;
}

uint32_t EngineWgpu::RegisterGpuModel(LODShapeWithShadow* shape)
{
    if (auto found = _gpuModels.find(shape); found != _gpuModels.end())
    {
        return found->second; // already scanned (WGR_INVALID_MODEL if it was ineligible)
    }

    std::vector<WgrModelLod> lods;
    std::vector<WgrModelSection> sections;
    std::vector<WgrModelMaterial> materials;
    bool eligible = true;
    bool isConform = false;
    // §9 Approach A: a merged forest mesh (whole-shape MapType) gets per-tree crown centres baked
    // per LOD so vs_gpu can bend its cutout normals radially per tree (its single instance centre
    // is meaningless per-tree). Individual trees/bushes keep using inst.center (BuildGpuInstance).
    const MapType mapType = shape->GetMapType();
    const bool isForest = mapType == MapForestBorder || mapType == MapForestTriangle || mapType == MapForestSquare;
    // §12 partial coverage. `hasProxies` = some level carries interior furniture proxies (drawn
    // by the CPU Object::DrawProxies); `hasComplement` = some visible section is NOT GPU-owned
    // (blend glass / on-surface decal, drawn by the CPU with GSkipGpuOwnedSections set). Either
    // makes the object Partial: the GPU draws the owned opaque geometry, the CPU repaints the
    // rest. Neither => Full (the GPU draws the whole object; the CPU draw is suppressed).
    bool hasProxies = false;
    bool hasComplement = false;

    for (int level = 0; level < shape->NLevels() && eligible; level++)
    {
        if (!shape->IsNormalLevel(level)) // skip special (>=900) non-graphical levels
        {
            continue;
        }
        Shape* s = shape->LevelOpaque(level);
        if (!s || s->NVertex() <= 0 || s->NSections() <= 0)
        {
            continue;
        }
        // Proxy-bearing shapes (buildings with interior furniture proxies): §12 partial
        // suppression keeps the proxies on the CPU (Object::DrawProxies, which the CPU
        // complement draw still runs) while the parent's own opaque geometry goes to the GPU.
        // So proxies no longer disqualify the model — they just force Partial coverage. The
        // hidden, texture-nulled proxy-MARKER sections are IsHidden and skipped below (in both
        // this registration and the CPU Shape::Draw), so they never bake as white triangles.
        if (s->NProxies() > 0)
        {
            hasProxies = true;
        }
        // Does this level contribute any GPU-owned (opaque, Default-surface) section? If none,
        // the whole level is CPU-drawn (all blend/decal) — don't build a pool mesh for it, just
        // note the complement so the object stays Partial and the CPU keeps drawing it.
        bool levelHasOwned = false;
        for (int i = 0; i < s->NSections(); i++)
        {
            const ShapeSection& shSec = s->GetSection(i);
            if (shSec.properties.Special() & (IsHidden | IsHiddenProxy))
            {
                continue;
            }
            // TreeAdv leaf cards have antialiased alpha, so their texture histogram is Blend
            // even though the authored family uses alpha-tested foliage. The retained pipeline
            // has no transparent foliage variant yet; forcing that data into its opaque/cutout
            // path produced either solid sheets or black holes. Leave the entire model on the
            // established direct path until retained alpha-to-coverage matches it exactly. A
            // whole-model fallback is necessary: CPU partial suppression is spec-based and
            // cannot safely hand off the trunk while keeping these opaque-spec leaf cards.
            if (auto* texture = static_cast<TextureWgpu*>(shSec.properties.GetTexture());
                texture && texture->GetAlphaClass() == Poseidon::AlphaStats::Blend &&
                GpuSectionMaterialIsCutout(shSec.surfMat, texture))
            {
                eligible = false;
                break;
            }
            // The retained material buffer currently carries imported lamp emissive values through a
            // separate update path, so its HDR result can bypass the compact-emitter cap applied by
            // DrawSectionTL. These are tiny static helper meshes (not general scenery); retaining
            // the complete model on the direct path keeps the presentation cap authoritative while
            // the paired WGPU point light continues to illuminate the surroundings.
            if (StaticEmitterRadianceTarget(shSec.surfMat) < 1.0f)
            {
                eligible = false;
                break;
            }
            if (render::IsGpuOwnedSectionSpec(shSec.properties.Special()))
            {
                levelHasOwned = true;
            }
            else
            {
                hasComplement = true; // a visible non-owned section => CPU complement draw
            }
        }
        if (!eligible)
        {
            break;
        }
        if (!levelHasOwned)
        {
            continue;
        }
        // Terrain-conformed shapes (ClipLand fences / individual vegetation): the GPU-driven
        // vertex shader conforms them to SurfaceY per vertex (mode 2, matching the per-draw
        // path), so they're eligible. Mark the shape conform — its instances carry conform2.z=2
        // + bcSurfaceY. Mirrors Object::PublishConformPlane's ClipLand gate.
        const bool levelConform = (s->GetOrHints() & (ClipLandKeep | ClipLandOn)) != 0;
        if (levelConform)
        {
            isConform = true;
            // The mode-2 conform selector is baked from OrigClip, and the shader deforms the
            // ORIGINAL (undeformed) geometry — so OrigPos/OrigClip must be valid. Object::Animate
            // saves them, but that runs at DRAW time, AFTER this load-time registration, so
            // OriginalPosValid() is still false here and BuildOrigVertices would fall back to the
            // non-conform BuildVertices (conform_sel=0 -> the instance floats rigid). Save them
            // now from the current (undeformed, ClipLand-clips-intact) geometry. Idempotent.
            s->SaveOriginalPos();
        }
        // OWN the geometry: create a DEDICATED pool mesh from the shape's data rather than
        // borrowing the shape's transient `_buffer`. ShapeBank::OptimizeAll releases every
        // shape vertex buffer during world load (after object placement, i.e. after we
        // register), which would leave a borrowed handle stale (destroyed) — the geometry
        // would then read freed/reused pool bytes (soup) or nothing. A mesh we create here is
        // owned by the retained scene (`_gpuMeshes`) and untouched by that release.
        const int nv = s->NVertex();
        const int ni = render::mesh::CountIndices(*s);
        if (ni <= 0)
        {
            continue;
        }
        std::vector<SVertex> verts(static_cast<size_t>(nv));
        // Conform (ClipLand mode-2) shapes: bake the per-vertex conform selector from OrigClip
        // AND upload the UNDEFORMED original geometry, exactly like the per-draw conform path
        // (BuildOrigVertices). Plain BuildVertices hardcodes conform=0, so mode-2 instances
        // never conform (they float rigid). OriginalPosValid guards shapes without orig data.
        if (levelConform && s->OriginalPosValid())
        {
            render::mesh::BuildOrigVertices(*s, verts.data());
        }
        else
        {
            render::mesh::BuildVertices(*s, verts.data());
        }
        std::vector<VertexIndex> indices(static_cast<size_t>(ni));
        render::mesh::BuildIndices(*s, indices.data());
        // §9 Approach A: for forests, flood-fill this LOD into per-tree components and bake each
        // vertex's crown-centre index into its conform word (forests are mode 0/1 conform, which
        // never read the ClipLand per-vertex selector, so the word is free). vs_gpu reads it to get
        // a per-tree radial-normal centre from the crown_centres table instead of inst.center.
        if (isForest)
        {
            std::vector<WgrVec4> centres;
            std::vector<uint32_t> comp;
            const uint32_t ncomp = BuildForestCrownComponents(verts, indices, centres, comp);
            if (ncomp > 0)
            {
                const uint32_t base =
                    wgr_register_crown_centres(_renderer, WgrSlice<WgrVec4>{centres.data(), U32(centres.size())});
                for (size_t vi = 0; vi < verts.size(); vi++)
                {
                    verts[vi].conform = base + comp[vi];
                }
            }
        }
        const uint64_t mesh =
            wgr_mesh_create(_renderer, AsMeshVerts(verts),
                            WgrSlice<uint16_t>{reinterpret_cast<const uint16_t*>(indices.data()), U32(ni)});
        if (!mesh)
        {
            eligible = false;
            break;
        }
        _gpuMeshes.push_back(mesh);
        AutoArray<render::mesh::MeshSection> secs;
        render::mesh::BuildSections(*s, secs);

        const uint32_t base = U32(sections.size());
        uint32_t count = 0;
        for (int i = 0; i < s->NSections(); i++)
        {
            // Hidden sections (e.g. texture-nulled proxy markers) are skipped by the CPU path
            // (Shape::Draw) and the wgpu shadow/per-object paths (skipMask); baking them would
            // draw untextured white geometry. Skip — not ineligible, just not drawn.
            if (s->GetSection(i).properties.Special() & (IsHidden | IsHiddenProxy))
            {
                continue;
            }
            WgrModelSection sec{};
            WgrModelMaterial mat{};
            // ClassifyGpuSection returns false for a non-owned section (blend/decal — the CPU
            // complement, already tallied in hasComplement above) or a degenerate one (empty
            // index range). Either way the GPU doesn't draw it: skip, don't fail the model.
            if (!ClassifyGpuSection(*s, mesh, secs, i, sec, mat))
            {
                continue;
            }
            // The RVMAT's albedo wins over the section's face texture, which on
            // Arma 3 structures is usually a `*_mlod_co` placeholder. Applied here
            // rather than inside ClassifyGpuSection because that is a free
            // function with no access to the material cache or texture bank.
            if (const uint64_t baseColour = ResolveMaterialBaseColour(s->GetSection(i)))
            {
                mat.texture_id = baseColour;
            }
            // The per-texel specular map, when the material names one. With it bound
            // the flat constant is applied in full and the map scales it per texel,
            // which is what the constant means; without one the approximation below
            // stands in. Ordering matters: ResolveMaterialBaseColour above is what
            // populates the translation cache both of these read.
            mat.specular_texture_id = ResolveMaterialSpecularMap(s->GetSection(i));
            // Stage1. Same ordering dependency: the translation cache is populated
            // by ResolveMaterialBaseColour above.
            mat.normal_texture_id = ResolveMaterialNormalMap(s->GetSection(i));
            // Multi's further layers, same ordering dependency on the translation cache.
            ResolveMaterialLayers(s->GetSection(i), mat);
            if (const float specScale = MaterialSpecularScale(s->GetSection(i), mat.specular_texture_id);
                specScale < 1.0f)
            {
                mat.specular.x *= specScale;
                mat.specular.y *= specScale;
                mat.specular.z *= specScale;
            }
            sections.push_back(sec);
            materials.push_back(mat);
            count++;
        }
        // Only register a LOD that actually has owned geometry (levelHasOwned guaranteed >=1
        // candidate, but it may have been degenerate). Empty owned LODs are CPU-drawn instead.
        if (count > 0)
        {
            lods.push_back(WgrModelLod{shape->Resolution(level), base, count, 0});
        }
    }

    uint32_t model = WGR_INVALID_MODEL;
    if (eligible && !lods.empty() && !sections.empty())
    {
        model =
            wgr_model_register(_renderer, shape->BoundingSphere(), WgrSlice<WgrModelLod>{lods.data(), U32(lods.size())},
                               WgrSlice<WgrModelSection>{sections.data(), U32(sections.size())},
                               WgrSlice<WgrModelMaterial>{materials.data(), U32(materials.size())});
    }
    _gpuModels[shape] = model;
    // Cache coverage so SceneObjectCreated can tag each instance and GpuDrivenCoverage can tell
    // the scene loop whether to suppress the whole CPU draw or only the GPU-owned sections.
    _gpuModelCoverage[shape] = (hasProxies || hasComplement) ? GpuDrawCoverage::Partial : GpuDrawCoverage::Full;
    // §12d-full: remember complement-bearing shapes so a proxies-only Partial can be upgraded to
    // Full once its proxies are all GPU-driven, but a complement-bearing one never is.
    if (hasComplement)
    {
        _gpuModelComplement.insert(shape);
    }
    if (model != WGR_INVALID_MODEL && isConform)
    {
        _gpuConformShapes.insert(shape);
    }
    return model;
}

// §12d: register the parent's eligible interior furniture proxies as GPU child instances. A
// proxy rides the parent's transform (world = parentTransform * proxyLocalTransform), so for a
// static parent it is a static instance. Only Full-coverage proxy shapes (self-contained: no
// transparent/decal sections, no nested proxies of their own) are taken — the rest keep drawing
// on the CPU via Object::DrawProxies, which skips the ones registered here.
bool EngineWgpu::EmitGpuProxies(Object* parent, LODShapeWithShadow* shape)
{
    // Reference LOD = the finest graphical level that actually carries proxies (furniture lives
    // on the detailed interior LOD; coarser LODs usually have none). We register only this LOD's
    // proxies; the GPU child instances then cull by their own distance, and DrawProxies at a
    // different (coarser) LOD draws whatever proxies that LOD has, unregistered.
    int refLevel = -1;
    for (int level = 0; level < shape->NLevels(); level++)
    {
        if (!shape->IsNormalLevel(level))
        {
            continue;
        }
        Shape* s = shape->LevelOpaque(level);
        if (s && s->NProxies() > 0)
        {
            refLevel = level;
            break;
        }
    }
    if (refLevel < 0)
    {
        return true; // no proxies -> nothing keeps the parent off Full
    }
    Shape* s = shape->LevelOpaque(refLevel);
    GpuProxySet set;
    set.refLevel = refLevel;
    const Matrix4 parentT = parent->Transform();
    bool allEligible = true; // every proxy moved to the GPU? (gates the parent's Full-downgrade)
    for (int i = 0; i < s->NProxies(); i++)
    {
        const ProxyObject& proxy = s->Proxy(i);
        LODShapeWithShadow* pshape = proxy.obj ? proxy.obj->GetShape() : nullptr;
        // Full coverage == the proxy shape has no CPU complement and no nested proxies, so the
        // GPU can draw it whole; anything Partial/ineligible would need its own complement/proxy
        // handling (not done for nested proxies) -> that proxy stays on the CPU, and the parent
        // therefore keeps its CPU draw (not eligible for the §12d-full Full-downgrade).
        const uint32_t pmodel = pshape ? RegisterGpuModel(pshape) : WGR_INVALID_MODEL;
        const auto covIt = pshape ? _gpuModelCoverage.find(pshape) : _gpuModelCoverage.end();
        const bool eligible =
            pmodel != WGR_INVALID_MODEL && covIt != _gpuModelCoverage.end() && covIt->second == GpuDrawCoverage::Full;
        if (!eligible)
        {
            allEligible = false;
            continue; // this proxy stays on the CPU (Object::DrawProxies draws it)
        }
        const Matrix4 world = parentT * proxy.obj->Transform();
        const WgrInstance inst = BuildGpuProxyInstance(world, pmodel);
        const uint32_t slot = wgr_instance_add(_renderer, &inst);
        set.children.push_back(GpuProxyChild{i, slot, pmodel});
    }
    if (!set.children.empty())
    {
        _gpuProxies[parent] = std::move(set);
    }
    return allEligible;
}

void EngineWgpu::RemoveGpuProxies(const Object* parent)
{
    auto it = _gpuProxies.find(parent);
    if (it == _gpuProxies.end())
    {
        return;
    }
    for (const auto& child : it->second.children)
    {
        wgr_instance_remove(_renderer, child.slot);
    }
    _gpuProxies.erase(it);
}

void EngineWgpu::SceneObjectCreated(Object* obj)
{
    if (!_gpuDriven || !_renderer || !obj)
    {
        return;
    }
    // First cut: static, intact, opaque-rigid objects only. Non-static (dynamics), already
    // handed over, and destroyed / mid-destruction objects stay on the CPU path (the GPU path
    // has no destroyed-variant geometry yet).
    if (!obj->Static() || obj->IsDestroyed() || _gpuInstances.count(obj) != 0)
    {
        return;
    }
    LODShapeWithShadow* shape = obj->GetShape();
    if (!shape)
    {
        return;
    }
    // Material Debug's first slice deliberately exercises the ordinary direct
    // WGPU section path. Keep only the explicit test RVMAT on that path while
    // the rest of the scene, including foliage, retains its production GPU-driven
    // submission. This avoids a diagnostic rock launch accidentally changing the
    // renderer path for every tree in the map.
    if (!_materialDebugRvmatMatch.empty())
    {
        for (int level = 0; level < shape->NLevels(); ++level)
        {
            const Shape* lod = shape->Level(level);
            if (!lod)
                continue;
            for (int section = 0; section < lod->NSections(); ++section)
            {
                const ShapeSection& sec = lod->GetSection(section);
                if (sec.surfMat && ContainsNoCase(sec.surfMat->GetName().Data(), _materialDebugRvmatMatch))
                    return;
            }
        }
    }
    const uint32_t model = RegisterGpuModel(shape);
    if (model == WGR_INVALID_MODEL)
    {
        return; // ineligible shape -> object drawn by the CPU path
    }
    // Terrain conform: ForestPlain -> mode-1 plane, ClipLand veg/fences -> mode-2, else rigid.
    const ConformPlane cp = GpuConformFor(*obj, *shape, _gpuConformShapes.count(shape) != 0);
    const WgrInstance inst = BuildGpuInstance(*obj, model, cp);
    const uint32_t slot = wgr_instance_add(_renderer, &inst);
    // Full (whole object on the GPU) vs Partial (GPU owns the opaque geometry; the CPU still
    // draws proxies + blend/decal sections) — base decision per shape in RegisterGpuModel.
    const auto covIt = _gpuModelCoverage.find(shape);
    GpuDrawCoverage cov = covIt != _gpuModelCoverage.end() ? covIt->second : GpuDrawCoverage::Full;
    // §12d: a Partial object may carry interior furniture proxies — move the eligible ones onto
    // the GPU as child instances (only Partial objects have proxies; Full ones never do).
    if (cov == GpuDrawCoverage::Partial)
    {
        const bool allProxiesGpu = EmitGpuProxies(obj, shape);
        // §12d-full: if the ONLY reason this was Partial is proxies (no CPU complement) and every
        // proxy moved to the GPU, the whole object is now GPU-drawn -> Full, so DrawSortObject
        // skips its CPU Object::Draw entirely (the last per-building CPU walk goes away).
        if (allProxiesGpu && _gpuModelComplement.count(shape) == 0)
        {
            cov = GpuDrawCoverage::Full;
        }
    }
    _gpuInstances[obj] = GpuInstance{model, slot, obj->Transform().Position(), cp.mode, cov};
    // Mirror the coverage onto the Object so GpuDrivenCoverage() is an O(1) field read (see there).
    obj->SetGpuCoverage(static_cast<int>(cov));
}

void EngineWgpu::SceneObjectRemoved(Object* obj)
{
    if (!_renderer)
    {
        return;
    }
    RemoveGpuProxies(obj); // §12d: drop the object's furniture child instances (no-op if none)
    auto it = _gpuInstances.find(obj);
    if (it == _gpuInstances.end())
    {
        return;
    }
    wgr_instance_remove(_renderer, it->second.slot);
    _gpuInstances.erase(it);
    obj->SetGpuCoverage(static_cast<int>(GpuDrawCoverage::None)); // back to the CPU path
}

void EngineWgpu::SceneObjectMoved(Object* obj)
{
    if (!_renderer)
    {
        return;
    }
    auto it = _gpuInstances.find(obj);
    if (it == _gpuInstances.end())
    {
        return;
    }
    LODShapeWithShadow* shape = obj->GetShape();
    // A destroyed object (or a shape swap) can no longer ride the intact GPU model: drop it
    // back to the CPU path, which draws the destroyed/animated geometry.
    if (!shape || obj->IsDestroyed())
    {
        wgr_instance_remove(_renderer, it->second.slot);
        _gpuInstances.erase(it);
        obj->SetGpuCoverage(static_cast<int>(GpuDrawCoverage::None)); // dropped to the CPU path
        RemoveGpuProxies(obj); // §12d: furniture goes with the parent (CPU draws destroyed geo)
        return;
    }
    // The conform plane depends on position (mode-1 land cell, mode-2 bcSurfaceY), so recompute
    // it on move. ForestPlain's cached plane is keyed to Position(); a moved forest would need
    // reinvalidation, but static clutter effectively never moves — recompute defensively.
    const ConformPlane cp = GpuConformFor(*obj, *shape, _gpuConformShapes.count(shape) != 0);
    const WgrInstance inst = BuildGpuInstance(*obj, it->second.model, cp);
    wgr_instance_update(_renderer, it->second.slot, &inst);
    it->second.pos = obj->Transform().Position();
    it->second.mode = cp.mode;
    // §12d: the parent moved, so its furniture children's composite transforms change too.
    auto pit = _gpuProxies.find(obj);
    if (pit != _gpuProxies.end())
    {
        Shape* s = shape->LevelOpaque(pit->second.refLevel);
        const Matrix4 parentT = obj->Transform();
        for (const auto& child : pit->second.children)
        {
            if (!s || child.proxyIndex >= s->NProxies())
            {
                continue;
            }
            const ProxyObject& proxy = s->Proxy(child.proxyIndex);
            if (!proxy.obj)
            {
                continue;
            }
            const Matrix4 world = parentT * proxy.obj->Transform();
            const WgrInstance pinst = BuildGpuProxyInstance(world, child.model);
            wgr_instance_update(_renderer, child.slot, &pinst);
        }
    }
}

GpuDrawCoverage EngineWgpu::GpuDrivenCoverage(const Object* obj) const
{
    // O(1) field read (Object::_gpuCoverage), stamped by Scene{ObjectCreated,Removed,Moved}. This
    // replaced a per-object unordered_map<Object*> probe that Scene::ObjectForDrawing (the divert)
    // calls for EVERY visible object — profiled at ~2.4 s/frame-budget of pure hash cache misses,
    // the single hottest CPU cost. The retained scene is now trusted correct-by-construction (all
    // transform movers fire SceneObjectMoved), so the old per-lookup drift tripwire + self-heal is
    // dropped with it — a stale transform is a hook bug to fix at the source, not to paper over on
    // every frame's whole visible set.
    if (!_gpuDriven || !obj)
    {
        return GpuDrawCoverage::None;
    }
    return static_cast<GpuDrawCoverage>(obj->GetGpuCoverage());
}

bool EngineWgpu::GpuDrivenProxy(const Object* parent, int level, int proxyIndex) const
{
    if (!_gpuDriven)
    {
        return false;
    }
    // Proxies are registered from one reference LOD only; at any other draw LOD they are not on
    // the GPU (that LOD's proxy list is different / empty). Furniture count per building is small,
    // so a linear scan is fine.
    auto it = _gpuProxies.find(parent);
    if (it == _gpuProxies.end() || it->second.refLevel != level)
    {
        return false;
    }
    for (const auto& child : it->second.children)
    {
        if (child.proxyIndex == proxyIndex)
        {
            return true;
        }
    }
    return false;
}

void EngineWgpu::SuppressWorldObjects(bool suppress)
{
    // Only the GPU-driven path retains a GPU-resident world set that draws independently of
    // the per-frame 3D lists; the CPU path already stops when World skips its 3D block.
    if (_gpuDriven && _renderer)
    {
        wgr_set_suppress_world_objects(_renderer, suppress);
    }
}

void EngineWgpu::SetCullDebugSettings(const CullDebugSettings& s)
{
    _cullDebug = s;
    _cullDebug.dumpNearby = false; // momentary button, never stored
    if (_gpuDriven && _renderer)
    {
        wgr_set_cull_debug(_renderer, s.drawSpheres, s.disableFrustum, s.occlusion);
    }
    if (s.dumpNearby && _gpuDriven)
    {
        // Log the retained instances near the camera: what the GPU cull buffer holds
        // (registration-time pos/mode) vs the object's LIVE Position() vs the terrain surface.
        // liveY-surf >> 0 on a mode-2 bush = a floating placement the conform hides (the
        // disappearing-object cause); stored != live = a stale registration (SetTransform after
        // AddObject without a Moved hook) — the floating-tree suspect.
        const Camera* cam = GScene ? GScene->GetCamera() : nullptr;
        if (cam && GLandscape)
        {
            const Vector3 camPos = cam->Position();
            int logged = 0;
            for (const auto& [obj, gi] : _gpuInstances)
            {
                const Vector3 live = obj->Transform().Position();
                if (live.Distance2(camPos) > Square(60.0f) || logged >= 48)
                {
                    continue;
                }
                logged++;
                const float surf = GLandscape->SurfaceY(live.X(), live.Z());
                const LODShapeWithShadow* shape = obj->GetShape();
                LOG_INFO(Graphics,
                         "CULLDUMP name={} mode={} dist={} liveY={} storedY={} surf={} above={} stale={} r={}",
                         shape ? shape->Name() : "?", gi.mode, live.Distance(camPos), live.Y(), gi.pos.Y(), surf,
                         live.Y() - surf, live.Distance(gi.pos), obj->GetRadius());
            }
            LOG_INFO(Graphics, "CULLDUMP done: {} instances within 60m (cap 48)", logged);
        }
    }
}

void EngineWgpu::SetShadowCascades(const shadow::CascadeSet& cascades, int resolution)
{
    _smCascades = cascades;
    _smCascadeRes = resolution;
    _smCascadesValid = _renderer != nullptr && cascades.count > 0 && resolution > 0;
}

void EngineWgpu::AddShadowCaster(const Shape& sMesh, const Matrix4& modelToWorld)
{
    if (!_renderer || !_smCascadesValid)
    {
        return;
    }
    auto* buf = static_cast<VertexBufferWgpu*>(sMesh.GetVertexBuffer());
    if (!buf || buf->sections.Size() == 0)
    {
        return;
    }
    buf->Update(sMesh, false);

    EnsureCamera();
    const CameraEntry& cam = _cameras[_currentCamera];
    // Remember the camera these casters are made relative to, so the depth pass's terrain
    // conform reconstructs absolute world xz with the RIGHT origin (see _smCamPos).
    _smCamPos[0] = cam.pos[0];
    _smCamPos[1] = cam.pos[1];
    _smCamPos[2] = cam.pos[2];
    GfxMatrix world;
    ConvertMatrix(world, modelToWorld);
    world._41 -= cam.pos[0];
    world._42 -= cam.pos[1];
    world._43 -= cam.pos[2];

    // Skinned caster: append a palette block (mirrors BeginMeshTL) so the depth
    // pass poses it on the GPU; the scene's Animate() set buf->palette just
    // before this call.
    uint32_t paletteSlot = WGR_NO_PALETTE;
    if (buf->skinned && buf->paletteCount > 0)
    {
        buf->drawnSkinned = true;
        Matrix4 worldRel = modelToWorld;
        worldRel.SetPosition(modelToWorld.Position() - Vector3(cam.pos[0], cam.pos[1], cam.pos[2]));
        paletteSlot = U32(_palette.size() / WGR_PALETTE_SIZE);
        const size_t base = _palette.size();
        _palette.resize(base + WGR_PALETTE_SIZE);
        for (int i = 0; i < buf->paletteCount; i++)
        {
            GfxMatrix g;
            ConvertMatrix(g, worldRel * buf->palette[i]);
            std::memcpy(_palette[base + i].m, &g, sizeof(_palette[base + i].m));
        }
        GfxMatrix gw;
        ConvertMatrix(gw, worldRel);
        std::memcpy(_palette[base + kReservedBone].m, &gw, sizeof(_palette[base + kReservedBone].m));
    }

    constexpr int skipMask = NoShadow | ShadowDisabled | IsHidden | IsHiddenProxy;
    constexpr int alphaMask = IsAlpha | IsTransparent;

    // Terrain conform: SceneShadowPass publishes a mode-2 plane for individual ClipLand
    // vegetation, so the depth shader conforms this shared, undeformed mesh per vertex to
    // SurfaceY (matching the color pass and Object::Animate) instead of the CPU rewriting
    // the shadow buffer per instance. Same per-vertex mechanism as DrawSectionTL's mode 2;
    // buf->Update above uploaded OrigPos, collapsing the per-instance upload storm.
    WgrVec4 conform0{};
    WgrVec4 conform2{};
    if (GCurrentConformPlane.active && GCurrentConformPlane.mode == 2)
    {
        conform0 = {GCurrentConformPlane.bcSurfaceY, 0.0f, 0.0f, 0.0f};
        conform2 = {0.0f, 0.0f, 2.0f, 0.0f};
    }

    WgrShadowCaster run{};
    bool haveRun = false;
    auto flush = [&]
    {
        if (haveRun)
        {
            _shadowCasters.push_back(run);
            haveRun = false;
        }
    };

    for (int i = 0; i < buf->sections.Size(); i++)
    {
        const auto& props = sMesh.GetSection(i).properties;
        const shadow::CasterMode mode = shadow::ClassifyShadowCaster(props.Special(), skipMask, alphaMask);
        if (mode == shadow::CasterMode::Skip)
        {
            flush();
            continue;
        }
        // Partial GPU-driven object (SceneShadowPass sets GSkipGpuOwnedSections): the GPU-owned
        // opaque sections are cast by the retained shadow set (draw_gpu_driven_shadow), so drop
        // them here — the SAME predicate ClassifyGpuSection / Shape::Draw use, so CPU + GPU
        // casters never overlap or leave a hole.
        if (GSkipGpuOwnedSections && render::IsGpuOwnedSectionSpec(props.Special()))
        {
            flush();
            continue;
        }
        uint64_t tex = 0;
        if (mode == shadow::CasterMode::AlphaTest)
        {
            if (auto* t = static_cast<TextureWgpu*>(props.GetTexture()))
            {
                tex = t->EnsureUploaded();
            }
        }
        // Cutout caster without a texture casts solid (GL33 parity).
        const float alphaRef = tex ? 0.5f : 0.0f;
        const render::mesh::MeshSection& si = buf->sections[i];
        if (haveRun && run.alpha_ref == alphaRef && run.texture_id == tex &&
            run.index_begin + run.index_count == U32(si.beg))
        {
            run.index_count += U32(si.end - si.beg);
            continue;
        }
        flush();
        run = WgrShadowCaster{};
        run.mesh = buf->mesh;
        run.index_begin = U32(si.beg);
        run.index_count = U32(si.end - si.beg);
        std::memcpy(run.world.m, &world, sizeof(run.world.m));
        run.texture_id = tex;
        run.palette_slot = paletteSlot;
        run.alpha_ref = alphaRef;
        run.sampler = 0;
        run.cascade_mask = 0xF;
        run.conform0 = conform0;
        run.conform2 = conform2;
        haveRun = true;
    }
    flush();
}

bool EngineWgpu::DumpShadowMap(const char* path)
{
    if (!_renderer || !path)
    {
        return false;
    }
    const int res = _smTuning.resolution;
    if (res <= 0)
    {
        return false;
    }
    std::vector<float> depth(static_cast<size_t>(res) * res, 1.0f);
    const uint32_t got = wgr_shadow_map_read(_renderer, 0, depth.data(), U32(depth.size()));
    if (!got)
    {
        return false;
    }
    // Same grayscale mapping as GL33's dump; wgpu rows are already top-down.
    std::vector<uint8_t> gray(static_cast<size_t>(got) * got);
    for (size_t i = 0; i < gray.size(); i++)
    {
        const float d = depth[i];
        gray[i] =
            (d >= 0.999f) ? static_cast<uint8_t>(35) : static_cast<uint8_t>((0.15f + (1.0f - d) * 0.85f) * 255.0f);
    }
    return PNGWriter::WritePNG(path, static_cast<int>(got), static_cast<int>(got), 1, gray.data());
}

bool EngineWgpu::ShadowDepthProbe(const float* lightVP16, const float* triXYZ, int vertCount, int res, float* outDepth)
{
    if (!_renderer || !lightVP16 || !triXYZ || !outDepth || vertCount <= 0 || res <= 0)
    {
        return false;
    }
    std::vector<float> top(static_cast<size_t>(res) * res);
    if (!wgr_shadow_depth_probe(_renderer, lightVP16, triXYZ, U32(vertCount), U32(res), top.data()))
    {
        return false;
    }
    // The convention is row 0 = bottom; the wgpu readback is top-down.
    for (int y = 0; y < res; y++)
    {
        std::memcpy(outDepth + static_cast<size_t>(y) * res, top.data() + static_cast<size_t>(res - 1 - y) * res,
                    static_cast<size_t>(res) * sizeof(float));
    }
    return true;
}

uint64_t EngineWgpu::OverlayTextureCreate(int w, int h, const uint8_t* rgba)
{
    if (!_renderer || w <= 0 || h <= 0 || !rgba)
    {
        return 0;
    }
    return wgr_texture_create(_renderer, U32(w), U32(h), WGR_TEXTURE_RGBA8, 1, 0, rgba, U32(w) * U32(h) * 4);
}

void EngineWgpu::OverlayTextureUpdate(uint64_t texture, int w, int h, const uint8_t* rgba)
{
    if (!_renderer || w <= 0 || h <= 0 || !rgba)
    {
        return;
    }
    wgr_texture_update(_renderer, texture, rgba, U32(w) * U32(h) * 4);
}

void EngineWgpu::OverlayTextureDestroy(uint64_t texture)
{
    if (_renderer)
    {
        wgr_texture_destroy(_renderer, texture);
    }
}

void EngineWgpu::SubmitOverlay(const OverlayVertex* verts, int vertCount, const uint16_t* indices, int indexCount,
                               const OverlayDrawCmd* cmds, int cmdCount)
{
    static_assert(sizeof(OverlayVertex) == sizeof(WgrOverlayVertex), "overlay vertex layouts must match");
    _overlayVerts.clear();
    _overlayIndices.clear();
    _overlayDraws.clear();
    if (!verts || !indices || !cmds || vertCount <= 0 || indexCount <= 0 || cmdCount <= 0)
    {
        return;
    }
    const auto* wv = reinterpret_cast<const WgrOverlayVertex*>(verts);
    _overlayVerts.assign(wv, wv + vertCount);
    _overlayIndices.assign(indices, indices + indexCount);
    _overlayDraws.resize(cmdCount);
    for (size_t i = 0; i < _overlayDraws.size(); i++)
    {
        const OverlayDrawCmd& c = cmds[i];
        WgrOverlayDraw& d = _overlayDraws[i];
        d.clip = {c.clip[0], c.clip[1], c.clip[2], c.clip[3]};
        d.texture_id = c.texture;
        d.first_index = c.firstIndex;
        d.index_count = c.indexCount;
        d.base_vertex = c.baseVertex;
        d._pad = 0;
    }
}

void EngineWgpu::GetZCoefs(float& zAdd, float& zMult)
{
    // The software-T&L path (footsteps, tyre tracks, UI overlays) is depth-tested
    // against GPU-projected geometry; wgpu needs ~16x GL33's software z-bias to
    // clear the CPU-vs-GPU depth divergence (empirically). WGR_SW_ZBIAS_MULT tunes it.
    static const float mult = []
    {
        const char* v = std::getenv("WGR_SW_ZBIAS_MULT");
        const float m = v ? std::strtof(v, nullptr) : 16.0f;
        return m > 0.0f ? m : 16.0f;
    }();
    const auto c = render::zbias::SoftwareCoefs(_bias);
    zAdd = c.zAdd * mult;
    zMult = 1.0f - (1.0f - c.zMult) * mult;
}

void EngineWgpu::SetTonemapSettings(const TonemapSettings& s)
{
    _tonemap = s;
    PushRenderParams();
}

void EngineWgpu::SetExposureSettings(const ExposureSettings& s)
{
    _exposure = s;
    PushRenderParams();
}

float EngineWgpu::GetAutoExposureScale() const
{
    return _renderer ? wgr_get_exposure_scale(_renderer) : 1.0f;
}

int EngineWgpu::GetWaterGpuTimings(float* outMs, int maxCount) const
{
    if (!_renderer || !outMs || maxCount <= 0)
        return 0;
    return (int)wgr_get_gpu_timings(_renderer, outMs, (uint32_t)maxCount);
}

// MAT-LEGACY: resolve `<texture stem>_nohq.paa` for an original OFP/CWA texture, upload it, and
// return its bindless handle -- or 0 when no enhancement exists, which is the stock-content case.
//
// Cached by texture name INCLUDING the misses. A stock install misses on every section of every
// frame, and a filesystem probe per section per frame is not a thing to do; a miss costs one hash
// lookup after the first.
uint64_t EngineWgpu::LegacyEnhancementNormal(const char* textureName)
{
    const std::string key(textureName);
    if (const auto found = _legacyEnhancementCache.find(key); found != _legacyEnhancementCache.end())
        return found->second;

    uint64_t handle = 0;
    // Strip the extension, append the suffix. `.pac` and `.paa` both appear in CWA data; the
    // enhancement is always a .paa because that is what the texture bank loads.
    const size_t dot = key.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
    {
        const std::string candidate = key.substr(0, dot) + "_nohq.paa";
        // Probe before loading: TextureBank::Load on a missing path yields a placeholder rather
        // than nothing, and binding a placeholder as a normal map would flatten the surface
        // instead of leaving it alone.
        if (QIFStream::FileExists(candidate.c_str()))
        {
            Ref<Texture> tex = _wbank->Load(candidate.c_str());
            if (auto* wgpuTex = static_cast<TextureWgpu*>(tex.GetRef()))
                handle = wgpuTex->EnsureUploaded();
            if (handle != 0)
                LOG_INFO(Graphics, "Wgpu legacy material: {} enhanced by {}", key, candidate);
        }
    }
    _legacyEnhancementCache.emplace(key, handle);
    return handle;
}

const char* EngineWgpu::GetWaterGpuTimingName(int region) const
{
    // Ordered by WgrGpuTimerRegion — the wgr_get_gpu_timings index contract (append only).
    static const char* const kNames[WGR_GPU_TIMER_REGION_COUNT] = {
        "Spectrum init",                  // WGR_GPU_TIMER_SPECTRUM_INIT (spectrum-dirty frames only)
        "Spectrum evolve",                // WGR_GPU_TIMER_SPECTRUM_EVOLVE
        "FFT horizontal",                 // WGR_GPU_TIMER_FFT_HORIZONTAL
        "FFT vertical",                   // WGR_GPU_TIMER_FFT_VERTICAL
        "FFT compose",                    // WGR_GPU_TIMER_FFT_COMPOSE
        "Interaction (inject+propagate)", // WGR_GPU_TIMER_INTERACTION (one fused kernel today)
        "Foam update",                    // WGR_GPU_TIMER_FOAM
        "Whitewater",                     // WGR_GPU_TIMER_WHITEWATER (reserved — no pass yet)
        "Planar: sky",                    // WGR_GPU_TIMER_PLANAR_SKY
        "Planar: terrain",                // WGR_GPU_TIMER_PLANAR_TERRAIN
        "Planar: objects",                // WGR_GPU_TIMER_PLANAR_OBJECTS (incl. reflected cull)
        "Planar: clouds",                 // WGR_GPU_TIMER_PLANAR_CLOUDS
        "Planar: mips",                   // WGR_GPU_TIMER_PLANAR_MIPS
        "Water SSR",                      // WGR_GPU_TIMER_WATER_SSR (reserved — in-shader in Water draw)
        "Water refraction",               // WGR_GPU_TIMER_WATER_REFRACTION (reserved — in-shader in Water draw)
        "Water draw",                     // WGR_GPU_TIMER_WATER_DRAW (incl. SSR + refraction cost)
        "Underwater froxel",              // WGR_GPU_TIMER_UNDERWATER_FROXEL (reserved — no pass yet)
        "Underwater composite",           // WGR_GPU_TIMER_UNDERWATER_COMPOSITE (incl. caustics)
        "Caustics",                       // WGR_GPU_TIMER_CAUSTICS (reserved — rides the shaders)
        "Place near (compute)",           // WGR_GPU_TIMER_GRASS_PLACE_NEAR
        "Place mid (compute)",            // WGR_GPU_TIMER_GRASS_PLACE_MID
        "Place far (compute)",            // WGR_GPU_TIMER_GRASS_PLACE_FAR
        "Grass prepass",                  // WGR_GPU_TIMER_GRASS_PREPASS (needs in-pass timestamps)
        "Grass colour",                   // WGR_GPU_TIMER_GRASS_COLOR (needs in-pass timestamps)
        "Grass shadow",                   // WGR_GPU_TIMER_GRASS_SHADOW (needs in-pass timestamps)
        "GPU frame total",                // WGR_GPU_TIMER_FRAME_TOTAL (all submitted work)
        "Interior sky: cull",             // WGR_GPU_TIMER_INTERIOR_SKY_CULL (one chain per direction)
        "Interior sky: depth maps",       // WGR_GPU_TIMER_INTERIOR_SKY_DRAW (all directions)
        "AO prep (resolve + mips)",       // WGR_GPU_TIMER_GTAO_PREP
        "AO horizon march",               // WGR_GPU_TIMER_GTAO_COMPUTE
        "AO bilateral blur",              // WGR_GPU_TIMER_GTAO_BLUR
    };
    return (region >= 0 && region < (int)WGR_GPU_TIMER_REGION_COUNT) ? kNames[region] : "";
}

bool EngineWgpu::GetGrassStats(GrassStatsOut& out) const
{
    if (!_renderer)
        return false;
    WgrGrassStats s{};
    if (wgr_get_grass_stats(_renderer, &s) == 0)
        return false;
    out.nearInstances = s.near_instances;
    out.midInstances = s.mid_instances;
    out.farInstances = s.far_instances;
    out.nearCandidates = s.near_candidates;
    out.midCandidates = s.mid_candidates;
    out.farCandidates = s.far_candidates;
    out.nearVertices = s.near_vertices;
    out.midVertices = s.mid_vertices;
    out.farVertices = s.far_vertices;
    return true;
}

void EngineWgpu::UpdateAutoTonemap()
{
    if (!_renderer || !_hdrEnabled || !_tonemapAuto)
        return;
    // Bloom is a global look setting, not part of the per-ToD keyframes, so carry the
    // current values across the auto overwrite (else the Tonemap tab's bloom sliders
    // would be reset every frame in auto mode).
    const float bi = _tonemap.bloomIntensity, bt = _tonemap.bloomThreshold, bk = _tonemap.bloomKnee;
    _tonemap = TonemapAtHour(Glob.clock.GetTimeOfDay() * 24.0f);
    _tonemap.bloomIntensity = bi;
    _tonemap.bloomThreshold = bt;
    _tonemap.bloomKnee = bk;
    // The push happens in NextFrame after UpdateAutoSky (PushRenderParams).
}

void EngineWgpu::UpdateSunGlareExposure()
{
    if (!_renderer || !_hdrEnabled || !GScene || !GScene->GetCamera() || !GScene->MainLight())
    {
        _sunGlareExposure = 1.0f;
        return;
    }

    Vector3 view = GScene->GetCamera()->Direction();
    // LightSun stores light travel direction; the visible sun lies opposite it.
    Vector3 sun = -GScene->MainLight()->SunDirection();
    view.Normalize();
    sun.Normalize();

    // Begins within ~14 degrees and peaks only in the central ~2 degrees. Sun below
    // the horizon never triggers accommodation, even if the camera faces that way.
    float focus = 0.0f;
    if (sun.Y() > 0.0f)
    {
        const float dot = std::clamp(view.DotProduct(sun), -1.0f, 1.0f);
        const float t = std::clamp((dot - 0.970f) / (0.9994f - 0.970f), 0.0f, 1.0f);
        focus = t * t * (3.0f - 2.0f * t);
    }
    const float target = 1.0f - 0.07f * focus;

    // Frame-rate-independent temporal response: darker over ~0.8 s, and recovers
    // more gradually (~1.8 s), avoiding an exposure pop while panning.
    const float dt = std::clamp(static_cast<float>(GetLastFrameDuration()) * 0.001f, 0.001f, 0.050f);
    const float seconds = (target < _sunGlareExposure) ? 0.8f : 1.8f;
    const float alpha = 1.0f - std::exp(-dt / seconds);
    _sunGlareExposure += (target - _sunGlareExposure) * alpha;
}

void EngineWgpu::SubmitGrass()
{
    if (!_renderer || _grassSubmitted)
    {
        return;
    }
    // Player/vehicle crushing is live world state, so refresh this small UBO
    // once per submitted grass frame rather than only when a dev slider moves.
    SetGrassSettings(_grass);
    EnsureCamera();
    WgrGrassBatch batch{};
    batch.camera = _currentCamera;
    _grassBatches.push_back(batch);
    _cmds.push_back(WgrCmd{WGR_CMD_DRAW_GRASS, U32(_grassBatches.size() - 1)});
    _grassSubmitted = true;
}

void EngineWgpu::SetGrassParams(float, float, float, float)
{
    // Landscape::DrawGround invokes this only for its alpha GrassTexture overlay
    // layers.  This is the authoritative per-frame signal that the active world
    // actually uses grass, unlike the opaque terrain submission (which also occurs
    // for desert, rock, and road-heavy maps).
    SubmitGrass();
}

uint32_t EngineWgpu::GetRuntimeCapabilityFlags() const
{
    if (!_renderer)
        return 0;
#ifdef _WIN32
    // Keep this optional at the PE import boundary. A deliberately mismatched
    // older DLL must reach wgr_abi_validate and be refused with its diagnostic,
    // rather than failing Windows module loading because this new helper export
    // is absent.
    using GetRuntimeCapabilitiesFn = uint32_t (*)(WgrRenderer*);
    const HMODULE module = GetModuleHandleA("wgpu_renderer.dll");
    const auto getCapabilities =
        module ? reinterpret_cast<GetRuntimeCapabilitiesFn>(GetProcAddress(module, "wgr_get_runtime_capabilities"))
               : nullptr;
    return getCapabilities ? getCapabilities(_renderer) : 0;
#else
    return 0;
#endif
}

void EngineWgpu::Screenshot(RString filename)
{
    _pendingScreenshotPath = filename;
    if (_renderer)
        wgr_screenshot_request(_renderer);
}

void EngineWgpu::FlushPendingScreenshot()
{
    if (!_renderer || _pendingScreenshotPath.GetLength() == 0)
        return;

    uint32_t width = 0, height = 0;
    wgr_screenshot_take(_renderer, nullptr, 0, &width, &height);
    const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
    if (width == 0 || height == 0 || bytes > UINT32_MAX)
        return;
    std::vector<uint8_t> rgba(static_cast<size_t>(bytes));
    if (wgr_screenshot_take(_renderer, rgba.data(), static_cast<uint32_t>(rgba.size()), &width, &height) != bytes)
        return;

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (size_t src = 0, dst = 0; src < rgba.size(); src += 4, dst += 3)
    {
        rgb[dst] = rgba[src];
        rgb[dst + 1] = rgba[src + 1];
        rgb[dst + 2] = rgba[src + 2];
    }
    ScreenshotWriter::WriteRGB(_pendingScreenshotPath, static_cast<int>(width), static_cast<int>(height), rgb.data());
    _pendingScreenshotPath = "";
}

void EngineWgpu::SetWaterSettings(const WaterSettings& s)
{
    _waterLook = s;
    _waterLookDirty = true;
}

void EngineWgpu::SyncWaterLookProfile()
{
    if (!GLandscape)
        return;
    const std::string map = GLandscape->GetName();
    if (map.empty())
        return;
    auto pathFor = [](const std::string& name)
    {
        std::string safe;
        for (char c : name)
            safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
        return std::filesystem::path(GamePaths::Instance().UserDir()) / "water-look" / (safe + ".cfg");
    };
    auto save = [&](const std::string& name)
    {
        if (!_waterLookDirty || name.empty())
            return;
        const auto path = pathFor(name);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            LOG_WARN(Graphics, "Water look: cannot write '{}'", path.string());
            return;
        }
        const auto& s = _waterLook;
        // v2 adds the optics/quality/sea/underwater groups. The reader keys off the group
        // names and ignores unknown ones, so a v1 profile still loads — its missing groups
        // simply keep the code defaults, which is the behaviour v1 had for them anyway.
        out << "v 2\nwave " << s.waveAmp << ' ' << s.waveChoppy << ' ' << s.waveSpeed << ' ' << s.waveScale << '\n';
        out << "colour " << s.shallowColor[0] << ' ' << s.shallowColor[1] << ' ' << s.shallowColor[2] << ' '
            << s.deepColor[0] << ' ' << s.deepColor[1] << ' ' << s.deepColor[2] << '\n';
        out << "coast " << s.colorExt << ' ' << s.coastFade << ' ' << s.foamWidth << ' ' << s.foamIntensity << ' '
            << s.swashAmp << ' ' << s.swashSpeed << ' ' << s.wetHeight << ' ' << s.wetDarken << '\n';
        out << "surface " << s.glitterGain << ' ' << s.sssGain << ' ' << s.reflectionGain << '\n';
        // v2 groups. Everything below was absent from the profile, and because a map change
        // resets _waterLook to WaterSettings{} before reloading it, "absent" meant "silently
        // reverts to the code default whenever the map changes". That is why the quality and
        // underwater controls did not appear to stick.
        //
        // If you add a field to WaterSettings that the Water tab exposes, add it here too.
        // The profile is a hand-maintained subset and gives no warning when it falls behind.
        out << "optics " << s.alpha << ' ' << s.specPower << ' ' << s.specIntensity << ' ' << s.shadowDim << ' '
            << s.warpAmp << ' ' << s.fadeStart << ' ' << s.fadeEnd << '\n';
        out << "quality " << (s.lowQuality ? 1 : 0) << ' ' << s.geometryQuality << ' ' << s.fftResolution << ' '
            << s.cascadePreset << '\n';
        out << "sea " << (s.seaStateCoupling ? 1 : 0) << ' ' << s.shoreWaveGain << ' ' << (s.physicalLook ? 1 : 0)
            << '\n';
        out << "underwater " << (s.underwaterEffect ? 1 : 0) << ' ' << s.underwaterEnterDepth << ' '
            << s.underwaterExitDepth << ' ' << s.underwaterEngageBand << ' ' << s.underwaterDensity << ' '
            << s.underwaterColorBias << ' ' << s.underwaterCausticGain << '\n';
        if (out.good())
            _waterLookDirty = false;
    };
    if (map == _waterLookMap)
    {
        save(map);
        return;
    }
    save(_waterLookMap);
    _waterLook = WaterSettings{};
    std::ifstream in(pathFor(map));
    std::string key;
    while (in >> key)
    {
        if (key == "v")
        {
            int v;
            in >> v;
        }
        else if (key == "wave")
            in >> _waterLook.waveAmp >> _waterLook.waveChoppy >> _waterLook.waveSpeed >> _waterLook.waveScale;
        else if (key == "colour")
            in >> _waterLook.shallowColor[0] >> _waterLook.shallowColor[1] >> _waterLook.shallowColor[2] >>
                _waterLook.deepColor[0] >> _waterLook.deepColor[1] >> _waterLook.deepColor[2];
        else if (key == "coast")
            in >> _waterLook.colorExt >> _waterLook.coastFade >> _waterLook.foamWidth >> _waterLook.foamIntensity >>
                _waterLook.swashAmp >> _waterLook.swashSpeed >> _waterLook.wetHeight >> _waterLook.wetDarken;
        else if (key == "surface")
            in >> _waterLook.glitterGain >> _waterLook.sssGain >> _waterLook.reflectionGain;
        else if (key == "optics")
            in >> _waterLook.alpha >> _waterLook.specPower >> _waterLook.specIntensity >> _waterLook.shadowDim >>
                _waterLook.warpAmp >> _waterLook.fadeStart >> _waterLook.fadeEnd;
        else if (key == "quality")
        {
            int low = 0;
            in >> low >> _waterLook.geometryQuality >> _waterLook.fftResolution >> _waterLook.cascadePreset;
            _waterLook.lowQuality = low != 0;
        }
        else if (key == "sea")
        {
            int coupling = 0;
            int physical = 0;
            in >> coupling >> _waterLook.shoreWaveGain >> physical;
            _waterLook.seaStateCoupling = coupling != 0;
            _waterLook.physicalLook = physical != 0;
        }
        else if (key == "underwater")
        {
            int on = 0;
            in >> on >> _waterLook.underwaterEnterDepth >> _waterLook.underwaterExitDepth >>
                _waterLook.underwaterEngageBand >> _waterLook.underwaterDensity >> _waterLook.underwaterColorBias >>
                _waterLook.underwaterCausticGain;
            _waterLook.underwaterEffect = on != 0;
        }
        else
            in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    _waterLookMap = map;
    _waterLookDirty = false;
}

void EngineWgpu::AddGrassImpact(Vector3Par position, float radius)
{
    if (!_renderer || radius <= 0.01f)
    {
        return;
    }
    // Reuse the established recovering track ring. Its radial direction makes
    // an explosion flatten blades outward from the point of impact.
    _grassTracks[_nextGrassTrack] = WgrGrassTrack{position.X(), position.Z(), std::clamp(radius, 0.5f, 16.0f), 0.0f};
    _nextGrassTrack = (_nextGrassTrack + 1) % _grassTracks.size();
}

void EngineWgpu::SetGrassSettings(const GrassSettings& settings)
{
    _grass = settings;
    if (!_renderer)
    {
        return;
    }
    const float dt = std::clamp(static_cast<float>(GetLastFrameDuration()) * 0.001f, 0.001f, 0.100f);
    const float trackLifetime = std::clamp(settings.trackLifetime, 5.0f, 3600.0f);
    for (WgrGrassTrack& track : _grassTracks)
    {
        track.age = std::min(track.age + dt, trackLifetime);
    }

    WgrGrassParams params{};
    params.density = std::clamp(settings.density, 0.0f, 1.0f);
    const float densityBoost = std::clamp(settings.densityBoost, 1.0f, 64.0f);
    const float requestedNearRadius = std::clamp(settings.radius, 8.0f, 200.0f);
    // Spacing is the density control and NOTHING else. It used to be floored at
    // `radius / 255` so a fixed 512-cell grid could always span the radius, which
    // meant every metre of extra reach thinned the field by the same factor it
    // widened it: past ~30 m the near ring held a constant number of clumps, the
    // slider appeared dead, and the disc around the player was visibly sparser
    // than the fixed-spacing mid ring beyond it. The renderer now sizes the
    // placement grid from the radius instead (see NEAR_GRID_MAX in grass/mod.rs).
    params.spacing = std::clamp(settings.spacing / std::sqrt(densityBoost), 0.02f, 0.75f);
    params.near_radius = requestedNearRadius;
    params.enabled = settings.enabled ? 1.0f : 0.0f;
    params.blade_height = std::clamp(settings.height, 0.10f, 3.00f);
    params.wind_strength = std::clamp(settings.windStrength, 0.0f, 3.0f);
    params.wind_direction = settings.windDirection;
    // Match the simulation's actual weather vector (the same one used by
    // smoke, cloth and parachutes). The developer strength is deliberately
    // retained as a gain so artists can test gust visibility without
    // changing mission weather, and becomes the complete manual control
    // when live wind is turned off in the Grass panel.
    //
    // Read from the wind AUTHORITY (World/Weather/WindModel), not from
    // Landscape's legacy 5-second random walk. Two things change for free:
    // the sway no longer steps every five seconds (the closed form is smooth,
    // so a gust builds and decays instead of snapping), and it is the same
    // vector the ocean spectrum, the cloud deck and the smoke sources use, so
    // the field leans the way the sea is running.
    if (settings.useLiveWind && GWind.IsActive())
    {
        const WindSample& wind = GWind.Sample();
        if (wind.speed > 0.001f)
        {
            params.wind_direction = wind.directionRad * (180.0f / 3.14159265358979323846f);
            params.wind_strength = std::clamp(wind.speed * 0.20f * settings.windStrength, 0.0f, 3.0f);
        }
    }
    // The far ring must start beyond the mid ring, whose reach the shader
    // derives from `near_radius` (capped at ~64 m by the mid placement grid).
    // Clamping the far radius to `settings.radius` -- as this did while both
    // came from one slider -- made the far accept band empty for every
    // radius below ~65 m, including the 60 m default: the outer LOD ran its
    // full candidate dispatch every frame and emitted nothing.
    // 0 disables the outer ring entirely (and skips its dispatch); any other
    // value is floored past the mid ring so it can never land in the dead band.
    const float midReach = std::clamp(settings.midRadius, params.near_radius + 10.0f, 1000.0f);
    params.mid_radius = midReach;
    params.far_radius = settings.farRadius <= 0.0f ? 0.0f : std::clamp(settings.farRadius, midReach + 8.0f, 5000.0f);
    params.density_noise_scale = std::clamp(settings.densityNoiseScale, 0.002f, 0.5f);
    params.density_noise_strength = std::clamp(settings.densityNoiseStrength, 0.0f, 1.0f);
    params.use_photo_tuft = settings.midPhotoTuft ? 1.0f : 0.0f;
    params.blade_width_scale = std::clamp(settings.bladeWidth, 0.25f, 6.0f);
    params.saturation = std::clamp(settings.saturation, 0.0f, 2.0f);
    params.dry_patches = std::clamp(settings.dryPatches, 0.0f, 1.0f);
    params.dry_patch_scale = std::clamp(settings.dryPatchScale, 0.002f, 0.3f);
    params.shape_variety = std::clamp(settings.shapeVariety, 0.0f, 1.0f);
    params.taper_jitter = std::clamp(settings.taperJitter, 0.0f, 1.0f);
    params.bend_jitter = std::clamp(settings.bendJitter, 0.0f, 1.0f);
    params.blade_texture_strength = std::clamp(settings.bladeTextureStrength, 0.0f, 1.0f);
    params.alpha_cards = settings.alphaCards ? 1.0f : 0.0f;
    params.alpha_cutoff = std::clamp(settings.alphaCutoff, 0.05f, 0.95f);
    params.card_widen = std::clamp(settings.cardWiden, 1.0f, 4.0f);
    params.blade_arch = std::clamp(settings.bladeArch, 0.0f, 3.0f);
    params.clump_renderer = settings.clumpRenderer ? 1.0f : 0.0f;
    params.photo_tuft_brightness = std::clamp(settings.photoTuftBrightness, 0.5f, 2.5f);
    params.photo_tuft_mix = std::clamp(settings.photoTuftMix, 0.0f, 1.0f);
    params.photo_tuft_patch_size = std::clamp(settings.photoTuftPatchSize, 4.0f, 80.0f);
    params.photo_contrast = std::clamp(settings.photoContrast, 0.5f, 2.5f);
    params.photo_contour = std::clamp(settings.photoContour, 0.0f, 2.0f);
    params.photo_self_shadow = std::clamp(settings.photoSelfShadow, 0.0f, 1.0f);
    params.photo_root_ao = std::clamp(settings.photoRootAo, 0.0f, 1.0f);
    params.photo_alpha_cutoff = std::clamp(settings.photoAlphaCutoff, 0.05f, 0.95f);
    params.photo_saturation = std::clamp(settings.photoSaturation, 0.0f, 2.0f);
    params.track_lifetime = trackLifetime;
    params.imprint_depth = std::clamp(settings.imprintDepth, 0.0f, 0.95f);
    params.photo_force_layer =
        settings.photoForceLayer < 0 ? -1.0f : static_cast<float>(std::min(settings.photoForceLayer, 8));
    for (size_t i = 0; i < std::size(params.photo_layer_weights); ++i)
    {
        params.photo_layer_weights[i] = std::clamp(settings.photoLayerWeights[i], 0.0f, 1.0f);
    }
    params.lod_blend = std::clamp(settings.lodBlend, 0.0f, 40.0f);
    params.near_density = std::clamp(settings.nearDensity, 0.0f, 1.0f);
    params.mid_density = std::clamp(settings.midDensity, 0.0f, 1.0f);
    params.far_density = std::clamp(settings.farDensity, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i)
    {
        params.tint_procedural[i] = std::clamp(settings.tintProcedural[i], 0.0f, 4.0f);
        params.tint_photo[i] = std::clamp(settings.tintPhoto[i], 0.0f, 4.0f);
    }
    params.tint_procedural[3] = 1.0f;
    params.tint_photo[3] = 1.0f;
    params.weed_percent = std::clamp(settings.weedPercent, 0.0f, 1.0f);
    params.flower_percent = std::clamp(settings.flowerPercent, 0.0f, 1.0f - params.weed_percent);
    // Everon/Eden's legacy geography marks normal terrain as excluded. The
    // terrain renderer identifies it from the actual uploaded WRP, so grass
    // works without requiring the user to discover a diagnostic checkbox.
    params.debug_ignore_geography_exclusions =
        (settings.ignoreGeographyExclusions || (_terrain && _terrain->GrassNeedsCompatibilityOverride())) ? 1.0f : 0.0f;
    params.clumping = std::clamp(settings.clumping, 0.0f, 1.0f);
    params.color_variation = std::clamp(settings.colorVariation, 0.0f, 1.0f);
    params.transmission = std::clamp(settings.transmission, 0.0f, 1.0f);
    params.cast_shadows = settings.castShadows ? 1.0f : 0.0f;
    params.apply_fog = settings.applyFog ? 1.0f : 0.0f;
    // CameraOn is the controlled entity in normal first/third-person play:
    // the player on foot, or their occupied car/tank. Its visible size gives
    // vehicles a wider flattened footprint without special vehicle classes.
    if (GWorld && GWorld->CameraOn())
    {
        const Object* interactor = GWorld->CameraOn();
        const Vector3 pos = interactor->Position();
        params.interactor_x = pos.X();
        params.interactor_z = pos.Z();
        params.interactor_radius = std::clamp(interactor->VisibleSize() * 0.55f, 1.1f, 8.0f);
        params.interactor_strength = 1.0f;
        // CameraOn is the occupied vehicle in normal play. Give a powered
        // player helicopter an explicit, large crush field here rather than
        // depending solely on the broader world-vehicle query below. This
        // also covers the first few frames of takeoff before the helicopter
        // has settled into the distributed vehicle list.
        // CameraOn is normally a Soldier, not a helicopter. dyn_cast is an
        // asserting checked cast in this codebase, so use RTTI's nullable
        // form for this optional rotor-wash branch.
        if (const Helicopter* helicopter = dynamic_cast<const Helicopter*>(interactor);
            helicopter && helicopter->RotorSpeed() > 0.02f)
        {
            _lastGrassRotor = const_cast<Helicopter*>(helicopter);
            const float rotorSpeed = std::clamp(helicopter->RotorSpeed(), 0.0f, 1.0f);
            // Values in (1, 1.5] are a renderer-local rotor-wash marker.
            // The shader decodes the fractional part as actual RPM, making
            // both bending and flutter rise smoothly as the rotor spools up.
            params.interactor_radius = 8.0f + 17.0f * rotorSpeed;
            params.interactor_strength = 1.0f + 0.5f * rotorSpeed;
        }
        _grassTrackSampleTime += dt;
        // Wider stamp spacing uses the existing soft-radius falloff to keep
        // a continuous trail while preserving substantially more history in
        // the fixed GPU record budget.
        const float trackSpacing = std::max(0.75f, params.interactor_radius * 0.50f);
        const float moved2 = _haveGrassTrackPos ? (pos - _lastGrassTrackPos).SquareSizeXZ() : 1e9f;
        // Stamp on DISTANCE MOVED, not elapsed time. The old 0.22 s fallback
        // fired while standing still, so a stationary player overwrote the whole
        // ring in a few seconds and erased the trail they had just walked --
        // which is why imprints appeared to expire far sooner than their stated
        // lifetime. Where the player actually is now is covered by the live
        // interactor; the ring only has to record where they have BEEN. The time
        // term survives as a slow floor so a very slow crawl still leaves a mark.
        const bool movedEnough = moved2 >= trackSpacing * trackSpacing;
        const bool crawling = _grassTrackSampleTime >= 1.5f && moved2 >= 0.04f;
        if (!_haveGrassTrackPos || movedEnough || crawling)
        {
            _grassTracks[_nextGrassTrack] = WgrGrassTrack{pos.X(), pos.Z(), params.interactor_radius, 0.0f};
            _nextGrassTrack = (_nextGrassTrack + 1) % _grassTracks.size();
            _lastGrassTrackPos = pos;
            _grassTrackSampleTime = 0.0f;
            _haveGrassTrackPos = true;
        }
    }
    // Rotor wash is intentionally transient. Keep the four closest powered
    // helicopters whose downwash can reach the camera's grass region. Checking
    // the rotor state rather than Airborne() makes the effect begin while the
    // skids are still touching the ground, where takeoff is most noticeable.
    if (GWorld && GLandscape)
    {
        struct Candidate
        {
            float distance2;
            WgrGrassDownwash wash;
        };
        std::array<Candidate, WGR_GRASS_DOWNWASH_COUNT> nearest{};
        for (Candidate& entry : nearest)
            entry.distance2 = std::numeric_limits<float>::infinity();
        const Vector3 cameraPos = GWorld->CameraOn() ? GWorld->CameraOn()->Position() : VZero;
        auto addDownwash = [&](const Helicopter* helicopter)
        {
            if (!helicopter)
                return;
            const float rotorSpeed = std::clamp(helicopter->RotorSpeed(), 0.0f, 1.0f);
            // Rotor inertia survives leaving the aircraft and engine shutoff,
            // so retain the transient wash until the blades actually stop.
            if (rotorSpeed <= 0.02f)
                return;
            const Vector3 pos = helicopter->Position();
            const float height = std::max(0.0f, pos.Y() - GLandscape->SurfaceY(pos.X(), pos.Z()));
            if (height > 65.0f)
                return;
            const float radius = (10.0f + height * 0.40f) * (0.45f + 0.55f * rotorSpeed);
            // Keep enough force at the upper edge of the range to visibly
            // press the blades flat rather than merely sway their tips.
            const float strength = std::clamp(1.10f - height / 260.0f, 0.85f, 1.0f) * rotorSpeed * rotorSpeed;
            const float distance2 = (pos - cameraPos).SquareSizeXZ();
            int slot = 0;
            for (int j = 1; j < WGR_GRASS_DOWNWASH_COUNT; ++j)
                if (nearest[j].distance2 > nearest[slot].distance2)
                    slot = j;
            if (distance2 < nearest[slot].distance2)
                nearest[slot] = Candidate{distance2, {pos.X(), pos.Z(), radius, strength}};
        };
        bool lastRotorWasListed = false;
        for (int i = 0; i < GWorld->NVehicles(); ++i)
        {
            const Helicopter* helicopter = dynamic_cast<const Helicopter*>(GWorld->GetVehicle(i));
            if (helicopter == _lastGrassRotor)
                lastRotorWasListed = true;
            addDownwash(helicopter);
        }
        // A helicopter that the player has just left can momentarily be
        // absent from the distributed list. Keep using its live rotor RPM
        // through the weak link during that handoff.
        if (!lastRotorWasListed)
            addDownwash(_lastGrassRotor);
        for (int i = 0; i < WGR_GRASS_DOWNWASH_COUNT; ++i)
            params.downwash[i] = nearest[i].wash;
    }
    std::copy(_grassTracks.begin(), _grassTracks.end(), params.tracks);
    wgr_grass_set_params(_renderer, &params);
}

const Helicopter* EngineWgpu::LastGrassRotor() const
{
    return _lastGrassRotor;
}

bool EngineWgpu::HasGrassPhotoClumps() const
{
    return _renderer != nullptr && wgr_grass_have_photo_clumps(_renderer) != 0;
}

int EngineWgpu::GetGrassPhotoFamilyCount() const
{
    return _terrain ? _terrain->GrassPhotoFamilyCount() : 0;
}

const char* EngineWgpu::GetGrassPhotoFamilyName(int index) const
{
    return _terrain ? _terrain->GrassPhotoFamilyName(index) : "";
}

int EngineWgpu::GetGrassSurfaceCount() const
{
    return _terrain ? _terrain->GrassSurfaceCount() : 0;
}

const char* EngineWgpu::GetGrassLoadedMapName() const
{
    return _terrain ? _terrain->GrassLoadedMapName() : "";
}

const char* EngineWgpu::GetGrassSurfaceName(int index) const
{
    return _terrain ? _terrain->GrassSurfaceName(index) : "";
}

bool EngineWgpu::IsGrassSurfaceEnabled(int index) const
{
    return _terrain && _terrain->GrassSurfaceEnabled(index);
}

void EngineWgpu::SetGrassSurfaceEnabled(int index, bool enabled)
{
    if (_terrain)
    {
        _terrain->SetGrassSurfaceEnabled(index, enabled);
    }
}

void EngineWgpu::UpdateAutoSky()
{
    if (!_renderer || !_hdrEnabled || !_sky.autoToD)
        return;
    // Copy ONLY the keyframed atmosphere fields from the interpolated preset into _sky;
    // everything else (density heights, night colours/band, samples, planet/atmosphere
    // geometry, and the user toggles: sky lighting, ambient, aerial shadow, fog falloff,
    // enabled) stays exactly as the Sky tab left it. Kept in sync with LerpSky's field set.
    const SkySettings k = SkyAtHour(Glob.clock.GetTimeOfDay() * 24.0f);
    _sky.exposure = k.exposure;
    _sky.sunIntensity = k.sunIntensity;
    _sky.sunAngularRadius = k.sunAngularRadius;
    _sky.rayleigh[0] = k.rayleigh[0];
    _sky.rayleigh[1] = k.rayleigh[1];
    _sky.rayleigh[2] = k.rayleigh[2];
    _sky.mie = k.mie;
    _sky.mieG = k.mieG;
    _sky.ozone = k.ozone;
    _sky.turbidity = k.turbidity;
    _sky.nightIntensity = k.nightIntensity;
}

void EngineWgpu::SetSkySettings(const SkySettings& s)
{
    _sky = s;
    PushRenderParams();
}

void EngineWgpu::PushRenderParams()
{
    if (!_renderer)
        return;

    WgrRenderParams p{};

    p.tonemap = {
        // Looking directly at the sun closes the eye down by at most 7%. The authored
        // exposure remains the primary HDR look and cannot be permanently overwritten.
        _tonemap.exposure * _sunGlareExposure,
        _tonemap.hable ? 1.0f : 0.0f,
        _tonemap.encode ? 1.0f : 0.0f,
        _tonemap.temperature,
        _tonemap.tint,
        _tonemap.contrast,
        _tonemap.saturation,
        _tonemap.lift,
        _tonemap.gain,
        _tonemap.bloomIntensity,
        _tonemap.bloomThreshold,
        _tonemap.bloomKnee,
    };

    p.exposure = {
        _exposure.enabled ? 1.0f : 0.0f,
        _exposure.key,
        _exposure.minScale,
        _exposure.maxScale,
        _exposure.rate,
        _exposure.skyWeight,
        0.0f,
        0.0f,
    };

    // Authored sky look (no celestial/runtime — that rides WgrSkyRuntime in PushSkyRuntime).
    const float haze = GScene ? _sky.horizonHaze : 0.0f;
    const float deg2rad = 3.14159265f / 180.0f;
    p.sky.rayleigh = {_sky.rayleigh[0], _sky.rayleigh[1], _sky.rayleigh[2], _sky.rayleighHeight};
    p.sky.mie = {_sky.mie, _sky.mieG, _sky.mieHeight, _sky.turbidity};
    p.sky.ground_sun = {_sky.ground[0], _sky.ground[1], _sky.ground[2], _sky.sunIntensity};
    p.sky.params = {_sky.sunAngularRadius, _sky.exposure, _sky.planetRadius, _sky.atmosphereHeight};
    p.sky.control = {_sky.enabled ? 1.0f : 0.0f, static_cast<float>(_sky.viewSamples),
                     static_cast<float>(_sky.lightSamples), _sky.ozone};
    p.sky.night_zenith = {_sky.nightZenith[0], _sky.nightZenith[1], _sky.nightZenith[2], haze};
    // night_horizon.w = the froxel fog terrain sun-shadow strength (0 = off; 1 = physical).
    p.sky.night_horizon = {_sky.nightHorizon[0], _sky.nightHorizon[1], _sky.nightHorizon[2], _sky.aerialShadow};
    // Blend band expressed as sun_dir.y (= sin elevation) so the shader compares directly.
    p.sky.night_params = {std::sin(_sky.nightStartDeg * deg2rad), std::sin(_sky.nightEndDeg * deg2rad),
                          _sky.nightIntensity, 0.0f};
    // Cloud look. cloud1.xy = wind WORLD offset is a runtime field (filled by PushSkyRuntime), so
    // only the shape/detail scales (z,w) are look fields here; scale = 1/size (guard size > 0).
    const float shapeScale = 1.0f / (_sky.cloudShapeSize > 1.0f ? _sky.cloudShapeSize : 1.0f);
    const float detailScale = 1.0f / (_sky.cloudDetailSize > 1.0f ? _sky.cloudDetailSize : 1.0f);
    const float weatherScale = 1.0f / (_sky.cloudWeatherSize > 1.0f ? _sky.cloudWeatherSize : 1.0f);
    const float warpScale = 1.0f / (_sky.cloudWarpSize > 1.0f ? _sky.cloudWarpSize : 1.0f);
    // Cloud coverage from the WORLD's overcast, so Zeus, the `weather` console command and
    // mission weather all actually move the sky. Before this they were unconnected: Zeus reported
    // an overcast that nothing rendered — the "reads back correctly but doesn't change the sky"
    // note in the roadmap.
    //
    // MUST run before cloud0 is packed below, or the driven value lands a frame late. Writes
    // _sky.cloudCoverage so the Sky tab's slider displays what is actually in effect; that slider
    // is disabled while this toggle is on, so nothing fights over the field.
    if (_sky.cloudCoverageFromWeather && GLandscape)
    {
        const float overcast = GLandscape->GetOvercast();
        const float t = overcast < 0.0f ? 0.0f : (overcast > 1.0f ? 1.0f : overcast);
        _sky.cloudCoverage = _sky.cloudCoverageClear + (_sky.cloudCoverageFull - _sky.cloudCoverageClear) * t;
    }

    p.sky.cloud0 = {_sky.cloudCoverage, _sky.cloudDensity, _sky.cloudBottom, _sky.cloudTop};
    p.sky.cloud1 = {0.0f, 0.0f, shapeScale, detailScale};
    p.sky.cloud2 = {_sky.cloudHgG, _sky.cloudPowder, _sky.cloudAmbient, _sky.cloudMaxDist};
    p.sky.cloud3 = {weatherScale, _sky.cloudWeatherAmount, warpScale, _sky.cloudWarpAmount};

    // CLD-020 cloud shadows. Pushed through its own entry point rather than a WgrSkyLook lane:
    // the look struct's size is part of the ABI handshake, and this is one float.
    if (_renderer != nullptr)
    {
        wgr_set_cloud_shadow_strength(_renderer, std::clamp(_sky.cloudShadowStrength, 0.0f, 1.0f));
        wgr_set_star_intensity(_renderer, std::clamp(_sky.starIntensity, 0.0f, 4.0f));
        wgr_set_lens_flare(_renderer, std::clamp(_sky.lensFlare, 0.0f, 4.0f));
    }

    // Long-distance terrain sun-shadow (wgpu-only); strength 0 = disabled. The renderer
    // diffs this sub-block so a per-frame push doesn't re-run the sweep.
    p.terrain_sun_shadow = {
        _smTuning.terrainShadowEnabled ? _smTuning.terrainShadowStrength : 0.0f,
        _smTuning.terrainShadowScale < 1 ? 1u : uint32_t(_smTuning.terrainShadowScale),
        _smTuning.terrainShadowSteps < 1 ? 1u : uint32_t(_smTuning.terrainShadowSteps),
        _smTuning.terrainShadowPenumbra,
    };

    // Sky-visibility ambient occlusion (wgpu-only); strength 0 = disabled. Radius/azimuths/
    // downsample re-run the scan renderer-side only when they change (also diffed).
    p.sky_visibility = {
        _smTuning.terrainSkyVisEnabled ? _smTuning.terrainSkyVisStrength : 0.0f,
        _smTuning.terrainSkyVisContrast,
        _smTuning.terrainSkyVisFloor,
        _smTuning.terrainSkyVisRadius,
        _smTuning.terrainSkyVisAzimuths < 1 ? 1u : uint32_t(_smTuning.terrainSkyVisAzimuths),
        _smTuning.terrainSkyVisDownsample < 1 ? 1u : uint32_t(_smTuning.terrainSkyVisDownsample),
        _smTuning.terrainSkyVisDebug ? 1u : 0u,
        0u,
    };

    // Foliage lighting (emulated leaf SSS for alpha-tested vegetation); rides the Frame UBO.
    p.foliage = {
        _foliage.transScale,   _foliage.distortion, _foliage.transPower,   _foliage.wrap,
        _foliage.ambientBoost, _foliage.normalBend, _foliage.crownYOffset, _foliage.fillFadeEnd,
        _foliage.giStrength,   _foliage.treeBend,   _foliage.treeCrownY,   0.0f,
    };

    // Screen-space AO (GTAO). `enabled` gates the whole pass renderer-side; the debug view is
    // additionally gated on it here so leaving debug on with the effect off can't blank the world.
    p.gtao = {
        _ao.enabled ? 1u : 0u,
        _ao.enabled ? uint32_t(_ao.debugMode < 0 ? 0 : (_ao.debugMode > 2 ? 2 : _ao.debugMode)) : 0u,
        _ao.radius,
        _ao.strength,
        _ao.slices < 1 ? 1u : uint32_t(_ao.slices),
        _ao.steps < 1 ? 1u : uint32_t(_ao.steps),
        _ao.maxRadiusPixels,
        _ao.thickness,
        _ao.blurRadius,
        _ao.blurDepthScale,
        _ao.blurNormalPower,
        _ao.bentNormal ? 1u : 0u,
        uint32_t(_ao.maxMip < 0 ? 0 : _ao.maxMip),
    };

    p.interior_sky = {
        _interiorSky.enabled ? 1u : 0u,
        // Debug only means anything while the map exists, so gate it on `enabled` — otherwise
        // turning the effect off with the debug view up leaves the world drawn in flat white.
        (_interiorSky.enabled && _interiorSky.debug) ? 1u : 0u,
        uint32_t(_interiorSky.resolution < 256 ? 256 : _interiorSky.resolution),
        _interiorSky.extent,
        _interiorSky.height,
        _interiorSky.strength,
        _interiorSky.floorLevel,
        _interiorSky.kernel,
        _interiorSky.bias,
        _interiorSky.directional,
        _interiorSky.baked ? 1u : 0u,
    };

    wgr_set_render_params(_renderer, &p);
}

void EngineWgpu::PushSkyRuntime()
{
    if (!_renderer)
        return;

    // Live celestial targets from LightSun. The legacy dome placed each body at
    // camPos - astronomicalDir * range, so the view direction TOWARD it is the
    // negated astronomical direction (up by day, below the horizon at night).
    Vector3 tSun(0.0f, 1.0f, 0.0f);
    Vector3 tMoon(0.0f, -1.0f, 0.0f);
    float tPhase = 0.5f;
    float tNight = 0.0f;
    if (GScene && GScene->MainLight())
    {
        LightSun* sun = GScene->MainLight();
        tSun = -sun->SunDirection();
        tMoon = -sun->MoonDirection();
        tPhase = sun->MoonPhase();
        tNight = sun->NightEffect();
    }
    tSun.Normalize();
    tMoon.Normalize();
    const Color tFog = FogColor();

    // Ease the coarse LightSun-driven inputs toward their live values each frame so the
    // sun disc + horizon haze move smoothly instead of snapping every few seconds. Snap
    // on the first push and on large jumps (teleport / time-skip) to avoid a slow sweep.
    const float alpha = 0.1f;
    if (!_skyInit || tSun.DotProduct(_skySunDir) < 0.5f)
    {
        _skySunDir = tSun;
        _skyMoonDir = tMoon;
        _skyMoonPhase = tPhase;
        _skyNight = tNight;
        _skyFog[0] = tFog.R();
        _skyFog[1] = tFog.G();
        _skyFog[2] = tFog.B();
        _skyInit = true;
    }
    else
    {
        _skySunDir = _skySunDir + (tSun - _skySunDir) * alpha;
        _skySunDir.Normalize();
        _skyMoonDir = _skyMoonDir + (tMoon - _skyMoonDir) * alpha;
        _skyMoonDir.Normalize();
        _skyMoonPhase += (tPhase - _skyMoonPhase) * alpha;
        _skyNight += (tNight - _skyNight) * alpha;
        _skyFog[0] += (tFog.R() - _skyFog[0]) * alpha;
        _skyFog[1] += (tFog.G() - _skyFog[1]) * alpha;
        _skyFog[2] += (tFog.B() - _skyFog[2]) * alpha;
    }

    // Camera altitude ASL feeds the aerial/sky raymarch origin: with the old fixed 200 m
    // the march dived below the terrain when flying (fake density -> grey wash). Sea level
    // is y = 0 in OFP, so the camera's world Y is the altitude directly.
    Camera* skyCam = GScene ? GScene->GetCamera() : nullptr;
    const float camAlt = skyCam ? static_cast<float>(skyCam->Position().Y()) : 0.0f;
    // fog far-range: the aerial pass dissolves the terrain edge into the full sky as it nears
    // the fog/view distance, hiding the horizon colour step. 0 when there's no scene.
    const float fogFar = GScene ? GScene->GetFogMaxRange() : 0.0f;

    WgrSkyRuntime rt{};
    rt.sun_dir = {_skySunDir.X(), _skySunDir.Y(), _skySunDir.Z(), 0.0f};
    // MOON. Everything here comes straight from LightSun's ephemeris, UNSMOOTHED: the
    // illuminated fraction and the angular radius move on a monthly timescale and the
    // sub-solar direction moves ~0.04 deg per minute, so there is nothing to pop.
    // (_skyMoonDir IS smoothed, like the sun, because the 20-second Recalculate cadence
    // would otherwise step the disc across the sky.)
    float moonIllum = 1.0f;
    float moonRadius = 0.00452f;
    float moonDiscScale = 0.0f;
    float moonEarthshine = 0.0f;
    Vector3 moonSunDir(0.0f, 1.0f, 0.0f);
    if (GScene && GScene->MainLight())
    {
        const LightSun* ml = GScene->MainLight();
        moonIllum = std::clamp(ml->MoonIllumination(), 0.0f, 1.0f);
        moonRadius = std::max(ml->MoonAngularRadius(), 1.0e-5f);
        moonSunDir = ml->MoonSunDirection();
        // Disc SURFACE radiance, not total brightness. MoonBrightness() is the empirical
        // lunar phase function — the TOTAL light the disc sends us — so dividing by the lit
        // AREA recovers the per-unit-area radiance. That division is what makes a quarter
        // moon's lit half genuinely dimmer per unit area than a full moon's, which is the
        // real opposition effect and not an artistic choice.
        const float k = std::max(moonIllum, 0.02f);
        moonDiscScale = _sky.sunIntensity * _sky.exposure * _sky.moonIntensity *
                        (ml->MoonBrightness() / k) * std::max(_sky.moonBrightnessScale, 0.0f);
        // Earthshine is brightest when the moon is thinnest (the Earth is then "full" as seen
        // from the moon). A reflectance floor added to the BRDF before the radiance scale, so
        // it tracks the disc's overall level instead of floating free of it.
        moonEarthshine = 0.010f * (1.0f - moonIllum);
    }
    moonSunDir.Normalize();
    const float moonDraw =
        (_sky.moonIntensity > 0.0f && _sky.moonBrightnessScale > 0.0f && moonDiscScale > 0.0f) ? 1.0f : 0.0f;

    rt.moon_dir = {_skyMoonDir.X(), _skyMoonDir.Y(), _skyMoonDir.Z(), moonIllum};
    rt.moon_params = {moonRadius, moonIllum, moonDiscScale, moonDraw};
    rt.moon_sun = {moonSunDir.X(), moonSunDir.Y(), moonSunDir.Z(), moonEarthshine};
    rt.fog_color = {_skyFog[0], _skyFog[1], _skyFog[2], fogFar};
    // misc.zw = cloud wind WORLD offset (metres) = windVel * time, wrapped to a large period in
    // DOUBLE precision so it stays bounded (the shader scales it into the noise coord and the Repeat
    // sampler wraps; a bounded offset keeps that coord precise however long the world has run). The
    // wrap reseat is a sub-tile shift once per ~kWindWrap/speed seconds (hours) — imperceptible.
    // Reuses the scene sim clock the terrain/water animation runs on, so clouds pause with the sim.
    // WTR-001 — when the Water tab Debug "Freeze clouds" switch is on, substitute the water debug
    // fixed time so the cloud shell holds at the same world offset as the test frame (the wind
    // formula is otherwise driven by Glob.time, which freezes in lockstep via the freezeTime bit).
    constexpr double kWindWrap = 100000.0; // m
    const double cloudT = (_waterLook.freeze.freezeClouds || _waterLook.freeze.freezeTime)
                              ? static_cast<double>(_waterLook.freeze.fixedTime)
                              : static_cast<double>(Glob.time.toFloat());
    // The deck's scroll velocity. Driven by the world's wind authority when
    // cloudWindFromWeather is on, so an overcast front that stiffens the wind
    // also runs the clouds — the same number that raises the sea state, leans
    // the grass and carries the smoke.
    double windVelX = static_cast<double>(_sky.cloudWind[0]);
    double windVelZ = static_cast<double>(_sky.cloudWind[1]);
    if (_sky.cloudWindFromWeather && GWind.IsActive())
    {
        // Mean wind, not the instantaneous gust: a 3 km-wide cloud bank does not
        // accelerate on a seven-second gust, and feeding gusts here would make
        // the whole sky shudder. Aloft scale converts surface wind to cloud-base
        // wind (see cloudWindAloftScale).
        const WindSample& wind = GWind.Sample();
        const double aloft = static_cast<double>(std::max(_sky.cloudWindAloftScale, 0.0f)) * wind.meanSpeed;
        windVelX = aloft * std::cos(wind.meanDirectionRad);
        windVelZ = aloft * std::sin(wind.meanDirectionRad);
    }
    // INTEGRATE the offset. `velocity * elapsed` is only correct while velocity is
    // constant; the moment the wind changes, that form rewrites the whole history
    // and the deck jumps by kilometres. Accumulating v*dt keeps it continuous
    // through every wind change. dt comes from the same cloud clock, so the
    // Water tab's freeze switches (which hold cloudT) still hold the deck still.
    if (!_cloudOffsetInit)
    {
        _cloudOffsetInit = true;
        _cloudOffsetClock = cloudT;
        // Preserve the historical starting phase so an untouched sky looks the
        // same at mission start as it did before the offset became integrated.
        _cloudOffsetX = std::fmod(windVelX * cloudT, kWindWrap);
        _cloudOffsetZ = std::fmod(windVelZ * cloudT, kWindWrap);
    }
    // Clamp dt: a time-of-day skip or a mission load must not fling the deck.
    const double cloudDt = std::clamp(cloudT - _cloudOffsetClock, 0.0, 0.5);
    _cloudOffsetClock = cloudT;
    _cloudOffsetX = std::fmod(_cloudOffsetX + windVelX * cloudDt, kWindWrap);
    _cloudOffsetZ = std::fmod(_cloudOffsetZ + windVelZ * cloudDt, kWindWrap);
    const double windX = _cloudOffsetX;
    const double windZ = _cloudOffsetZ;
    rt.misc = {_skyNight, camAlt, static_cast<float>(windX), static_cast<float>(windZ)};
    // Cloud EVOLUTION offsets, on the same clock and the same wrap as the wind above (so they
    // freeze together under the Water tab's freeze switches). Wind translates the field; these
    // walk it through the noise volume's third axis, which is what makes clouds form and dissolve
    // in place. Detail is deliberately faster than shape and the weather drift slower — small
    // wisps change before the banks do, and where it is cloudy at all changes slowest of the
    // three, which is the order real weather does it in.
    const double evo = static_cast<double>(_sky.cloudEvolve);
    const double evoShape = std::fmod(evo * cloudT, kWindWrap);
    const double evoDetail = std::fmod(evo * 2.0 * cloudT, kWindWrap);
    const double evoWeather = std::fmod(evo * 0.5 * cloudT, kWindWrap);
    rt.cloud_evolve = {static_cast<float>(evoShape), static_cast<float>(evoDetail), static_cast<float>(evoWeather),
                       0.0f};
    wgr_set_sky_runtime(_renderer, &rt);
}

void EngineWgpu::ResolveSceneToDisplay()
{
    // The renderer tonemaps the HDR scene at this marker and draws everything after
    // it display-referred. Harmless on the LDR-direct path (the renderer ignores it).
    if (_renderer)
        _cmds.push_back(WgrCmd{WGR_CMD_RESOLVE, 0});
}

void EngineWgpu::PrepareMesh(const render::LegacySpec& /*spec*/)
{
    _swMesh = nullptr;
}

void EngineWgpu::BeginMesh(TLVertexTable& mesh, const render::LegacySpec& /*spec*/)
{
    _swMesh = &mesh;
}

void EngineWgpu::EndMesh(TLVertexTable& /*mesh*/)
{
    _swMesh = nullptr;
}

void EngineWgpu::PrepareTriangle(const MipInfo& mip, int specFlags)
{
    _swTexture = ResolveTexture(mip);
    _swBlend = BlendForSpec(specFlags);
    _swSampler = SamplerForSpec(specFlags);
    _swDepth = DepthForSpec(specFlags);
}

void EngineWgpu::DrawSection(const FaceArray& face, Offset beg, Offset end)
{
    if (!_swMesh)
    {
        return;
    }
    const TLVertex* verts = _swMesh->VertexData();

    std::vector<WgrVertex2D> tris;
    for (Offset i = beg; i < end; face.Next(i))
    {
        const Poly& f = face[i];
        const int n = f.N();
        if (n < 3)
        {
            continue;
        }
        const VertexIndex* idx = f.GetVertexList();
        const WgrVertex2D v0 = MakeScreenVertex(verts[idx[0]]);
        for (int k = 1; k + 1 < n; k++)
        {
            tris.push_back(v0);
            tris.push_back(MakeScreenVertex(verts[idx[k]]));
            tris.push_back(MakeScreenVertex(verts[idx[k + 1]]));
        }
    }
    AppendTriangles(_swTexture, _swBlend, _swSampler, _swDepth, tris);
}

Engine* CreateEngineWgpu(const GraphicsEngineParams& params)
{
    auto* engine = new EngineWgpu(params);
    if (!engine->IsValid())
    {
        delete engine;
        return nullptr;
    }
    return engine;
}

} // namespace Poseidon
