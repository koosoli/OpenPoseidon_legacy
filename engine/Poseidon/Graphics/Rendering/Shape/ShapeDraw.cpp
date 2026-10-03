
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Dev/Diag/ScopedTimer.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp> // RFG-085 residency levers
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Pass1Submit.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
// AdapterBankTables holds Ref<TexMaterial>; its destruction here needs the full type.
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>

#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp>
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Edges.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>

#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>

#include <Poseidon/Core/Data3D.h>

#include <Poseidon/World/MapTypes.hpp>
#include <stdio.h>
#include <cmath>
#include <memory>
#include <string>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/V3Quads.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/Foundation/platform.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

extern bool DisableTextures;

namespace Poseidon
{

#ifdef _MSC_VER
#pragma warning(disable : 4355)
#endif

inline bool IsSpec(float resolution, float spec)
{
    return fabs(resolution - spec) < spec * 1e-3;
}

bool EnableHWTLState = true;

// Alpha routing asks the decoded texture's histogram, and for anti-aliased cutouts the
// histogram gives the wrong answer. `ClassifyAlpha` calls anything with >=2% partial
// alpha a blend, and an alpha-TESTED leaf with soft edges is mostly partial alpha at the
// edges: the shipped Arma 3 pine's leaf texture is 30.3% partial, the Enoch birch's 5.8%,
// and 44 of 50 plants_f `_ca` textures classify as blend. A blend section leaves the
// opaque pass for a back-to-front pass with no depth write, and every symptom follows --
// no shadows (nothing occludes or casts), a canopy that accumulates instead of
// depth-testing (measured mean 16.6 against a frame mean of 4.1 at 01:00), no crisp leaf
// edge at distance, and dark patches that move because interpenetrating cards sort
// order-dependently.
//
// MAT-048 footnote on "with no depth write". When this comment was written it described a
// system that did not exist: the pass it names (Scene::DrawObjectsAndShadowsPass2, the
// `GSectionFilter = BlendOnly` branch in World/Scene/SceneDraw.cpp) left depth-write ON and
// said so in its own comment, calling it "original parity". Two comments in two files
// described opposite behaviour and one of them had to be wrong; it was the SceneDraw one, in
// the sense that the behaviour it documented was the defect. That pass now really does draw
// depth-test-only, so this paragraph is accurate as written -- see
// `GBlendSectionDepthReadOnly` in Graphics/Rendering/BuildRenderPassDescriptor.hpp for why,
// and note that `WGR_COCKPIT_BLEND_LEGACY=1` puts the depth-write back, which would make this
// sentence wrong again for that run.
//
// CWA content is spared by accident of format: ClassifyTextureAlpha short-circuits 1-bit
// alpha straight to Cutout, so the engine has never been asked about an anti-aliased
// cutout before.
//
// The histogram is the wrong authority; the shader family is the right one. A family
// whose alpha is measured to be a cutout demotes Blend -> Cutout here. TreeAdvTrunk
// is deliberately different: its bark colour map carries a non-coverage alpha channel
// (the Stratis pinus nigra bark classifies as Blend), but it is solid geometry. Route it
// through the opaque pass without an alpha test; treating it as a leaf cutout can discard
// the entire trunk.
//
// Only the families actually measured are listed. Grass and SuperAToC are near-certain
// members -- the latter says alpha-to-coverage in its own name -- but neither has been
// measured here, and adding one is a single line once it has been.
enum class AuthoredAlphaRoute : uint8_t
{
    Default,
    Cutout,
    Opaque,
};

static AuthoredAlphaRoute ShaderFamilyAlphaRoute(const char* family)
{
    if (strcmpi("TreeAdv", family) == 0 || strcmpi("TreeAdvTrans", family) == 0)
        return AuthoredAlphaRoute::Cutout;
    if (strcmpi("TreeAdvTrunk", family) == 0)
        return AuthoredAlphaRoute::Opaque;
    return AuthoredAlphaRoute::Default;
}

// Resolving this means opening and parsing an RVMAT, which cannot happen per draw, so the
// answer is cached by material path -- including the negative answers, so a missing or
// unparseable material is not retried on every frame.
static AuthoredAlphaRoute SectionMaterialAlphaRoute(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return AuthoredAlphaRoute::Default;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path)
        return AuthoredAlphaRoute::Default;

    static std::unordered_map<std::string, AuthoredAlphaRoute> cache;
    static std::mutex                                           cacheMutex;
    const std::string                                           key(path);

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto                  found = cache.find(key);
        if (found != cache.end())
            return found->second;
    }

    AuthoredAlphaRoute route = AuthoredAlphaRoute::Default;
    try
    {
        if (Asset::Material::IsEmatPath(key))
        {
            // Reforger: a solid-surface `.emat` (walls, roofs, solid props) is
            // opaque even when its BCR roughness alpha classifies Blend
            // (RFG-033); foliage, glass, decals and everything else keep the
            // histogram's route, exactly as before.
            Asset::Material::EmatMaterial emat;
            if (Asset::Material::ReadEmatFile(key, emat) && Asset::Material::EmatIsOpaqueSolid(emat))
                route = AuthoredAlphaRoute::Opaque;
        }
        else
            route = ShaderFamilyAlphaRoute(Asset::Material::ParseRvMaterialFile(key).pixelShaderId.c_str());
    }
    catch (...)
    {
        // Unavailable material: keep the texture histogram's route.
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = route;
    return route;
}

// Whether a section's `.emat` authors real translucency: an `OpacityMap`
// plus `BlendMode AlphaBlend` on a non-solid surface (wire mesh, glass).
// Same cached shape as the answers above.
static bool SectionMaterialEmatBlends(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return false;
    static std::unordered_map<std::string, bool> cache;
    static std::mutex                            cacheMutex;
    const std::string                            key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto                  found = cache.find(key);
        if (found != cache.end())
            return found->second;
    }
    bool blends = false;
    try
    {
        Asset::Material::EmatMaterial emat;
        if (Asset::Material::ReadEmatFile(key, emat) && !emat.TextureOf("OpacityMap").empty() &&
            !Asset::Material::EmatIsOpaqueSolid(emat))
        {
            if (const auto* mode = emat.Find("BlendMode");
                mode && !mode->values.empty() && !mode->values[0].isNumber)
                blends = Asset::Material::EmatMaterial::NameMatches(mode->values[0].text, "AlphaBlend");
        }
    }
    catch (...)
    {
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = blends;
    return blends;
}

// The `.emat` shader family, cached by material path (empty when the section
// names none or it cannot be read). One file read per material, like the two
// answers above.
static std::string SectionMaterialEmatFamily(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return {};
    const char* path = surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return {};
    static std::unordered_map<std::string, std::string> cache;
    static std::mutex                                   cacheMutex;
    const std::string                                   key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto                  found = cache.find(key);
        if (found != cache.end())
            return found->second;
    }
    std::string family;
    try
    {
        Asset::Material::EmatMaterial emat;
        if (Asset::Material::ReadEmatFile(key, emat))
            family = emat.className;
    }
    catch (...)
    {
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = family;
    return family;
}

// Whether a section's `.emat` names `Cull none` (wire mesh, glass panes).
// Same cached shape as the two answers above: file read once per material.
static bool SectionMaterialCullNone(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return false;
    static std::unordered_map<std::string, bool> cache;
    static std::mutex                            cacheMutex;
    const std::string                            key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto                  found = cache.find(key);
        if (found != cache.end())
            return found->second;
    }
    bool cullNone = false;
    try
    {
        Asset::Material::EmatMaterial emat;
        if (Asset::Material::ReadEmatFile(key, emat))
        {
            if (const auto* cull = emat.Find("Cull");
                cull && !cull->values.empty() && !cull->values[0].isNumber)
                cullNone =
                    Asset::Material::EmatMaterial::NameMatches(cull->values[0].text, "none");
        }
    }
    catch (...)
    {
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = cullNone;
    return cullNone;
}

// The single place that decides whether a section belongs in the back-to-front pass.
static bool SectionIsBlend(const ShapeSectionInfo& sec)
{
    auto* tex = sec.properties.GetTexture();
    if (!tex || tex->GetAlphaClass() != Poseidon::AlphaStats::Blend)
        return false;
    // Legacy A1 foliage has no RVMAT to speak for it, so the histogram is the only voice in
    // the room and it mis-hears antialiased downscales as translucency. See
    // render::IsLegacyPlantCutoutTexture for the measured per-LOD split.
    if (render::IsLegacyPlantCutoutTexture(tex->Name()))
        return false;
    if (render::IsEnfusionCoverageTexture(tex->Name()))
    {
        // Authored translucency (`OpacityMap` + `BlendMode AlphaBlend`: wire
        // mesh, glass) blends even when holey; merged coverage WITH holes
        // (leaf cards) alpha-tests; hole-less coverage without an authored
        // answer (glass dirt) is translucency -- unless the material authors
        // otherwise, in which case it keeps the cutout route.
        if (SectionMaterialEmatBlends(sec.surfMat))
            return true;
        if (tex->HasAlphaHoles() || SectionMaterialAlphaRoute(sec.surfMat) != AuthoredAlphaRoute::Default)
            return false;
        return true;
    }
    return SectionMaterialAlphaRoute(sec.surfMat) == AuthoredAlphaRoute::Default;
}

// The complement half of the ownership contract, and the fix for the A1 "windows, blinds and
// interiors are gone" report (2026-08-27).
//
// IsGpuOwnedSectionSpec answers from the section SPEC alone. ClassifyGpuSection answers from
// the spec AND the texture's alpha histogram, and refuses anything the histogram calls Blend.
// For a section whose spec is plain opaque but whose texture is a translucent pane, the two
// disagree in the one direction that loses geometry: the GPU declines it as blend, the CPU
// skips it as GPU-owned, and NOBODY draws it.
//
// That is not a corner case, it is the whole A1 building generation. ODOL section flags are
// taken verbatim (ShapeAdapter.cpp:712) -- only the MLOD branch derives IsAlpha from the
// texture -- so `ca\buildings\hotel.p3d` carries flags=0x0 on all 24 LOD-0 sections while
// hotel_oknoa_ca.paa (10,496 tris of glass and Venetian blinds), hotel_door1_ca.paa and
// hotel_sign_ca.paa all classify BLEND. Every one of them fell in the hole.
//
// So: a section the retained path REFUSES stays the CPU's, whatever its spec bits say.
//
// Reforger is routed by the same authored route both sides consult
// (SectionMaterialAlphaRoute reads `.emat` as well as `.rvmat`), so an .emat
// section the GPU declines is complement-drawn by the CPU and foliage is never
// drawn twice. Until both sides read the same file, an .emat section keeps
// today's behaviour -- which, since the route reader landed, they do.
bool SectionRetainedRefuses(const ShapeSectionInfo& sec)
{
    if (!SectionIsBlend(sec))
        return false;
    const char* mat = sec.surfMat.IsNull() ? nullptr : sec.surfMat->GetName().Data();
    return !(mat && *mat && Asset::Material::IsEmatPath(mat));
}

bool SectionGpuOwned(const ShapeSectionInfo& sec)
{
    const int spec = sec.properties.Special();
    if (render::IsGpuOwnedSectionSpec(spec))
        return !SectionRetainedRefuses(sec);
    // IsAlpha spec. Owned only when the per-draw path would hard-test it in the opaque pass.
    const render::RenderPassDescriptor desc = render::BuildRenderPassDescriptor(render::SplitLegacy(spec));
    if (desc.blend != render::BlendMode::AlphaBlend || desc.surface != render::SurfaceMode::Default)
        return false;
    Texture* tex = sec.properties.GetTexture();
    if (!tex)
        return false;
    // Cull-none cutouts (wire mesh, glass) stay CPU-drawn double-sided: the
    // retained path culls backfaces and ClassifyGpuSection declines them the
    // same way, so no section is drawn twice or not at all. Vegetation is
    // excluded: its crossed cards were tuned for the culled GPU path.
    if (SectionMaterialCullNone(sec.surfMat) &&
        !Asset::Material::IsEmatVegetationFamily(SectionMaterialEmatFamily(sec.surfMat)))
    {
        const bool cutoutRoute =
            tex->GetAlphaClass() == Poseidon::AlphaStats::Cutout ||
            ((render::IsLegacyPlantCutoutTexture(tex->Name()) || render::IsEnfusionCoverageTexture(tex->Name())) &&
             tex->HasAlphaHoles());
        if (cutoutRoute)
            return false;
    }
    if (tex->GetAlphaClass() == Poseidon::AlphaStats::Cutout)
        return true;
    if (render::IsLegacyPlantCutoutTexture(tex->Name()))
        return true;
    // Merged coverage owns its section only when it alpha-tests (real holes)
    // or the material authors a non-blend route; hole-less coverage (glass)
    // and authored translucency (wire mesh) fall through to the IsAlpha/Blend
    // handling below so the CPU blends them.
    if (render::IsEnfusionCoverageTexture(tex->Name()) && !SectionMaterialEmatBlends(sec.surfMat) &&
        (tex->HasAlphaHoles() || SectionMaterialAlphaRoute(sec.surfMat) != AuthoredAlphaRoute::Default))
        return true;
    // RFG-075: an IsAlpha-spec section whose texture the histogram calls OPAQUE is owned.
    //
    // The spec comes from `tex->IsAlpha()` -- "the format carries an alpha channel" --
    // and every Reforger BC7 albedo does: its alpha is ROUGHNESS (measured 254.9 mean,
    // 99.8-100% partial), not coverage. So every tinted wall, roof and floor on Everon
    // arrived here with IsAlpha set and an Opaque class, matched none of the branches
    // above, and fell out the bottom to the per-section path -- which draws it opaque
    // anyway, and is the one path with no layer blend. Measured on Church_01: all ten
    // opaque LOD0 sections refused, 162 refusals in the run, every one of them an
    // `enft|` texture, every one for this reason.
    //
    // Opaque-class means the alpha test would pass everywhere; the two paths already
    // agree on how it looks. Owning it changes which shader draws it, not what it draws.
    if (tex->GetAlphaClass() == Poseidon::AlphaStats::Opaque)
    {
        // RFG-085: a live lever (Streaming tab), seeded from POSEIDON_OWN_OPAQUE_ALPHA=0 for
        // the old refusal. The run that first grew `wgr_geo_pool_vbuf` past the device with
        // this rule on was a 100k-model stress load, not this rule: measured with the first
        // error kept (RFG-082), the heap was 4.4 GB of pool, 4.4 GB of textures and 1 GB of
        // sky volumes before any of these sections registered.
        if (Poseidon::Dev::GResidencyLevers().ownOpaqueAlpha)
            return true;
    }
    // Blend histogram: owned when the material AUTHORS a cutout or an opaque route (TreeAdv leaf
    // cards and their bark, i.e. exactly when SectionIsBlend says it is not real translucency).
    // A solid-surface `.emat` (SectionMaterialAlphaRoute Opaque) is owned for the same
    // reason: its BCR alpha is roughness, so the GPU draws it solid with the layer
    // blend; every other `.emat` keeps today's behaviour (CPU-drawn).
    if (tex->GetAlphaClass() == Poseidon::AlphaStats::Blend)
    {
        return SectionMaterialAlphaRoute(sec.surfMat) != AuthoredAlphaRoute::Default;
    }
    return false;
}

bool Shape::HasBlendSections() const
{
    if (_hasBlendSections < 0)
    {
        _hasBlendSections = 0;
        for (int i = 0; i < NSections(); i++)
        {
            if (SectionIsBlend(GetSection(i)))
            {
                _hasBlendSections = 1;
                break;
            }
        }
    }
    return _hasBlendSections != 0;
}

SectionClassFilter GSectionFilter = SectionClassFilter::All;
bool GSkipGpuOwnedSections = false;
ConformPlane GCurrentConformPlane;
bool GCurrentIsVegetation = false;
float GCurrentFoliageKind = 0.0f;
bool GGpuTerrainConform = false;

// WGR_ROAD_GPU_VB -- let an OnSurface level keep a vertex buffer when the backend conforms
// terrain on the GPU.
//
// LODShape::OptimizeRendering (below) denies a vertex buffer to any level whose Special()
// carries OnSurface. Every loader sets that bit from the geometry itself -- ShapeSetup.cpp:723
// and ShapeAdapter.cpp:465/720 OR it in as soon as ONE vertex is ClipLandOn -- so on every
// generation of content "OnSurface" means "has land-pinned vertices", which is roads first
// and foremost. In 2001 the deny was right: an OnSurface shape was re-split against the
// terrain on the CPU every frame (FaceArray::SurfaceSplit, ClipShape.cpp:241), so a static
// buffer had nothing to hold. Under a backend that conforms in the vertex shader it is the
// single structural reason a road never reaches DrawSectionTL: with `_buffer` null the T&L
// gate in Shape::Draw fails and the shape falls to FaceArray::Draw -> EngineWgpu::DrawSection
// (screen-space, 2D pipeline: no sun, no sRGB decode, no HDR, a different depth projection).
// Measured on Takistan (WGR_ROAD_TRACE=1): 42 ROAD_CPUPATH lines, every one `buffer=0
// tlAble=1`, and zero road textures in the T&L trace.
//
// RoadType (World/Scene/ObjectClasses.cpp, WLD-026) already sidesteps the deny for shapes
// it constructs, by swapping OnSurface for IsOnSurface before the buffer is built. This
// lever is the general form and the belt to that brace: it grants the buffer to any
// OnSurface level -- road models that bail out of RoadType because they carry a Geometry
// LOD, models that mix a few ClipLandOn vertices into ordinary geometry, and RoadType roads
// themselves whenever WGR_ROAD_LEGACY_SURFACE restores their OnSurface bit -- so that the
// draw path, not a per-class flag, decides how a land-pinned vertex is treated.
//
// Granting the buffer alone is NOT sufficient, and the two other halves live here too:
//   * the buffer must be VBDynamic. EngineWgpu::CreateVertexBuffer fills a new buffer with
//     BuildVertices (conform selector 0 on every vertex) and a static buffer is never
//     re-uploaded; only a dynamic buffer's FIRST VertexBufferWgpu::Update -- which runs in
//     BeginMeshTL, inside Object::Draw with the object's mode-2 conform plane published --
//     takes the BuildOrigVertices arm and uploads the per-vertex ClipLand selector the
//     vertex shader pins to SurfaceY. After that upload the early-out applies again, so this
//     is one upload of a ~10-vertex mesh per road MODEL, not per placement.
//   * OrigPos/OrigClip must be valid before that upload. BuildOrigVertices reads OrigClip,
//     and Object::Animate returns without SaveOriginalPos on an all-ClipLandOn shape ("will
//     be done during SurfaceSplit", Object.cpp:374). Saved in Shape::Draw, immediately
//     before BeginMeshTL, the first time a surface shape is drawn under an active conform
//     plane -- NOT at optimize time. The order matters: a road loaded after the bulk pass
//     is optimized by ShapeBank::New BEFORE RoadType's constructor normalises its clip
//     array (ObjectClasses.cpp:895), and SaveOriginalPos is a one-shot (no-op once valid),
//     so a snapshot taken at optimize time would freeze the pre-normalisation OrigClip and
//     RoadType's own SaveOriginalPos could no longer correct it. At first draw the shape is
//     undeformed (Animate skips the CPU deform while the plane is active) and every
//     load-time rewrite has already happened. Idempotent.
//
// Scope: only when GGpuTerrainConform is true (wgpu with WGR_GPU_CONFORM on). GL33 keeps
// the deny bit for bit: it has no GPU conform, its OnSurface geometry is conformed by the
// CPU split, and a buffer there would draw the road rigid at its authored height.
// WGR_ROAD_GPU_VB=0 restores the deny on wgpu as well.
static bool RoadGpuVertexBufferLeverEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_ROAD_GPU_VB");
        return !(v && v[0] != '\0' && std::strcmp(v, "0") == 0);
    }();
    return enabled;
}

static bool OnSurfaceLevelKeepsVertexBuffer()
{
    return GGpuTerrainConform && RoadGpuVertexBufferLeverEnabled();
}

// Announced from ShapeBank::OptimizeAll (every world load) rather than lazily from the first
// OnSurface level, so the log states the lever's setting even on a world where nothing
// exercises it -- a run whose only evidence is the absence of a line proves nothing.
static void LogRoadGpuVertexBufferLever()
{
    const char* v = std::getenv("WGR_ROAD_GPU_VB");
    LOG_INFO(Graphics,
             "ROAD_GPU_VB lever: {} (WGR_ROAD_GPU_VB={}, GPU terrain conform={}); OnSurface levels {} a vertex "
             "buffer",
             OnSurfaceLevelKeepsVertexBuffer() ? "ON" : "OFF", v ? v : "<unset>", GGpuTerrainConform ? 1 : 0,
             OnSurfaceLevelKeepsVertexBuffer() ? "keep" : "are denied (2001 behaviour)");
}

// WGR_ROAD_TRACE, read once. Shape::Draw runs per object per frame; the previous per-call
// std::getenv walked the process environment block under the CRT lock on every OnSurface
// draw that fell to the CPU path.
static bool RoadTraceRequested()
{
    static const bool on = []
    {
        const char* rt = std::getenv("WGR_ROAD_TRACE");
        return rt && std::strcmp(rt, "0") != 0;
    }();
    return on;
}

void Shape::Draw(class IAnimator* matSource, const LightList& lights, ClipFlags clip, int spec,
                 const Matrix4& transform, const Matrix4& invTransform)
{
#ifndef ACCESS_ONLY
    // if engine has T&L interface, use it
    // cannot use T&L on some surface types (OnSurface?)

    Engine* engine = GEngine;

    bool tlAble = ((spec & OnSurface) == 0 || engine->GetTLOnSurface()) && (clip & ClipUser0) == 0;

    if (engine->GetTL() && EnableHWTLState && tlAble && _buffer)
    {
        if (BeginFaces() < EndFaces() && NSections() > 0)
        {
            // ROAD_GPUPATH -- the positive twin of ROAD_CPUPATH below. A surface shape
            // (road / decal) whose draw entered the hardware T&L branch: it has a vertex
            // buffer and will reach DrawSectionTL, i.e. shader3d.wgsl and the mode-2 GPU
            // conform, IF the conform plane is active here. `conform` is that plane's mode
            // (2 = per-vertex ClipLand pin to SurfaceY, 0 = no plane published, so the
            // shape draws rigid at its authored height). The first such draw in a process
            // is logged unconditionally so a default run can prove roads took this path
            // at all; every further shape is logged once under WGR_ROAD_TRACE=1, so the
            // ROAD_GPUPATH and ROAD_CPUPATH counts of one run partition the surface set.
            if ((spec & (OnSurface | IsOnSurface)) != 0)
            {
                // The conform selector is baked from OrigClip by BuildOrigVertices in the
                // buffer's first Update (BeginMeshTL, below). Under an active plane this
                // shape is undeformed, so snapshot it now if nobody has yet (RoadType does
                // it for the shapes it constructs; the WGR_ROAD_GPU_VB lever relies on this
                // for everything else -- see its comment block for why not at optimize
                // time). No-op once valid.
                if (GCurrentConformPlane.active && OnSurfaceLevelKeepsVertexBuffer())
                {
                    SaveOriginalPos();
                }
                static bool loggedFirstGpuSurface = false;
                static std::unordered_set<const void*> loggedGpuShapes;
                const bool trace = RoadTraceRequested();
                if (!loggedFirstGpuSurface || (trace && loggedGpuShapes.insert(static_cast<const void*>(this)).second))
                {
                    LOG_INFO(Graphics,
                             "ROAD_GPUPATH shape={} nv={} buffer=1 tlAble=1 spec=0x{:x} conform={} origPos={} first={}",
                             static_cast<const void*>(this), NVertex(), unsigned(spec),
                             GCurrentConformPlane.active ? GCurrentConformPlane.mode : 0, OriginalPosValid() ? 1 : 0,
                             loggedFirstGpuSurface ? 0 : 1);
                    loggedFirstGpuSurface = true;
                }
            }
            GEngine->PrepareMeshTL(lights, transform, render::SplitLegacy(spec));
            if (spec & (OnSurface | IsOnSurface))
            {
                engine->SetBias(0x10);
            }
            else
            {
                int bias = (spec & ZBiasMask) / ZBiasStep;
                // max. bias value is 3
                engine->SetBias(bias * 5);
            }
            // prepare sections (if neccessary)
            // prepare lights and materials

            // check if shape is dynamic or not
            bool dynamic = matSource->GetAnimated(*this);
            engine->BeginMeshTL(*this, spec, dynamic);
            // check first face properties
            int secBeg = -1;
            int secEnd = -1;
            Texture* secTexture = (Texture*)-1;
            int secSpecial = -1;
            int secMaterial = -1;
            TexMaterial* secSurfMat = nullptr;

            // MAT-052 diagnostic (temporary): WGR_SECTION_SKIP_TRACE=<substring> names, once
            // per shape, every section of a matching shape with its texture, flags and the
            // reason it was skipped -- because a section that is never submitted is invisible
            // to every draw-side trace, and that gap is exactly where this bug lived.
            static const char* sectionTraceMatch = std::getenv("WGR_SECTION_SKIP_TRACE");
            const bool sectionTrace = [&]() -> bool
            {
                if (!sectionTraceMatch || !*sectionTraceMatch)
                    return false;
                // Shape has no back-pointer to its LODShape, so match on the sections'
                // own texture names: any section whose texture path contains the token
                // marks the whole shape for one dump.
                bool match = false;
                for (int si = 0; si < NSections() && !match; si++)
                {
                    const Texture* t = GetSection(si).properties.GetTexture();
                    match = t && t->Name() && strstr(t->Name(), sectionTraceMatch);
                }
                if (!match)
                    return false;
                static std::unordered_set<const void*> tracedOnce;
                return tracedOnce.insert(this).second;
            }();
            // Section denominator for the Pass1 submission sub-split. `sectionsSeen`
            // counts the walk, `sectionsDrawn` counts what survived the three skip tests
            // below -- a shape whose sections are nearly all skipped still pays the walk,
            // and only the pair of numbers can say so.
            if (Poseidon::Pass1Submit::Counting())
            {
                Poseidon::Pass1Submit::gSectionsSeen.fetch_add(static_cast<uint64_t>(NSections()),
                                                               std::memory_order_relaxed);
            }
            const bool p1sSections = Poseidon::Pass1Submit::Counting();
            for (int i = 0; i < NSections(); i++)
            {
                const ShapeSection& sec = GetSection(i);
                if (sectionTrace)
                {
                    const Texture* t = sec.properties.GetTexture();
                    const bool hid = (sec.properties.Special() & (IsHidden | IsHiddenProxy)) != 0;
                    const bool gpuOwned = GSkipGpuOwnedSections && SectionGpuOwned(sec);
                    const bool filtered = GSectionFilter != SectionClassFilter::All &&
                                          ((GSectionFilter == SectionClassFilter::BlendOnly) != SectionIsBlend(sec));
                    LOG_WARN(Graphics, "SECTRACE shape@{} sec {} tex={} special={:#x} {}{}{}",
                             static_cast<const void*>(this), i, t ? t->Name() : "<none>",
                             unsigned(sec.properties.Special()),
                             hid ? "SKIP-HIDDEN " : "", gpuOwned ? "SKIP-GPUOWNED " : "",
                             filtered ? "SKIP-FILTER " : "");
                }
                if (sec.properties.Special() & (IsHidden | IsHiddenProxy))
                {
                    continue;
                }

                // Partial GPU-driven object (wgpu §12): the GPU retained scene already drew
                // this shape's GPU-owned (opaque, Default-surface) sections into the colour +
                // prepass targets, so the CPU repaints only the complement (blend glass,
                // decals). Same predicate the wgpu backend registered the section with, so
                // owned sections are dropped exactly once — never double-drawn, never held.
                if (GSkipGpuOwnedSections && SectionGpuOwned(sec))
                {
                    continue;
                }

                // per-section transparency routing: in the opaque pass draw only
                // opaque+cutout sections, in the back-to-front pass only blend ones.
                if (GSectionFilter != SectionClassFilter::All)
                {
                    const bool isBlend = SectionIsBlend(sec);
                    if ((GSectionFilter == SectionClassFilter::BlendOnly) != isBlend)
                    {
                        continue;
                    }
                }

                if (p1sSections)
                {
                    Poseidon::Pass1Submit::gSectionsDrawn.fetch_add(1, std::memory_order_relaxed);
                }
                if (secBeg < 0)
                {
                    secBeg = i;
                    secEnd = i + 1;
                    secTexture = sec.properties.GetTexture();
                    secSpecial = sec.properties.Special();
                    secMaterial = sec.material;
                    secSurfMat = sec.surfMat;
                }
                else if (sec.properties.GetTexture() == secTexture && sec.properties.Special() == secSpecial &&
                         sec.material == secMaterial && sec.surfMat == secSurfMat && i == secEnd)
                {
                    // extend section
                    secEnd = i + 1;
                }
                else
                {
                    // flush section
                    TLMaterial mat;
                    matSource->GetMaterial(mat, GetSection(secBeg).material);
                    GetSection(secBeg).PrepareTL(mat, lights, spec);
                    GEngine->DrawSectionTL(*this, secBeg, secEnd);
                    // open another section
                    secBeg = i;
                    secEnd = i + 1;
                    secTexture = sec.properties.GetTexture();
                    secSpecial = sec.properties.Special();
                    secMaterial = sec.material;
                    secSurfMat = sec.surfMat;
                }
            }
            if (secEnd > secBeg)
            {
                int matIndex = GetSection(secBeg).material;
                TLMaterial mat;
                matSource->GetMaterial(mat, matIndex);
                // flush section
                GetSection(secBeg).PrepareTL(mat, lights, spec);

                GEngine->DrawSectionTL(*this, secBeg, secEnd);
            }

            engine->EndMeshTL(*this);
        }
    }
    else
    {
        // WGR_ROAD_TRACE: an OnSurface shape that fell to the CPU FaceArray::Draw branch. The
        // gate above is `GetTL() && EnableHWTLState && tlAble && _buffer`; GL33 and wgpu agree
        // on the first three (both return true from GetTL and GetTLOnSurface), so `_buffer` --
        // CreateVertexBuffer returning null -- is the ONLY structural place the two backends
        // can diverge for a road. A road that never appears in the DrawSectionTL trace but does
        // appear here is that divergence, and the absence is itself the answer.
        if (RoadTraceRequested() && (spec & (OnSurface | IsOnSurface)) != 0)
        {
            static std::unordered_set<const void*> loggedCpuShapes;
            if (loggedCpuShapes.insert(static_cast<const void*>(this)).second)
            {
                LOG_INFO(Graphics, "ROAD_CPUPATH shape={} nv={} buffer={} tlAble={} spec=0x{:x}",
                         static_cast<const void*>(this), NVertex(), _buffer ? 1 : 0, tlAble ? 1 : 0,
                         unsigned(spec));
            }
        }
        DoAssert(_face._sections.Size() == 0 || _face._sections[_face._sections.Size() - 1].end == EndFaces());

        // The section-class filter can split sections only on the hardware-T&L path
        // above; this non-TL fallback (OnSurface/ClipUser0 surfaces, or shapes with no
        // vertex buffer — e.g. the live tyre-track ribbon, rebuilt every frame) draws
        // the whole shape exactly once, in the pass DrawWholeShapeInPass routes it to:
        // surface overlays in the sorted on-surface (BlendOnly) pass so the road's
        // asphalt cannot repaint over them, everything else in the opaque pass.
        const bool surfaceOverlay = (spec & (OnSurface | IsOnSurface)) != 0 && HasBlendSections();
        if (DrawWholeShapeInPass(GSectionFilter, surfaceOverlay))
        {
            // custom T&L
            _face.Draw(matSource, lights, *this, clip, spec, transform, invTransform);
        }
    }
#endif
}

namespace
{
// Composite cache key: name + reversed + shadow flags.  This
// replaces the previous linear-scan-with-bit-flag-matching: each
// (name, reversed, shadow) tuple maps 1:1 to a unique cached
// shape, so exact-tuple keying produces the same partition as
// the original bit-flag matcher.  Length budget: max P3D path
// ~128 + 4-char suffix; comfortably fits the asset paths in tree.
//
// Shared by New and Find so the two can never disagree about what "resident" means.
void BuildShapeCacheKey(const char* name, bool reversed, bool shadow, char (&lowName)[128], char (&cacheKey)[160])
{
    snprintf(lowName, sizeof(lowName), "%s", (const char*)name);
    strlwr(lowName);
    snprintf(cacheKey, sizeof(cacheKey), "%s|R%dS%d", lowName, reversed ? 1 : 0, shadow ? 1 : 0);
}

// Cache keys whose load is CURRENTLY RUNNING on this thread, innermost last.
//
// NewFromModel inserts into `_cache` only after the adapter has returned, but the adapter
// resolves the model's proxies while it runs (ShapeAdapter -> NewProxyObject -> Shapes.New).
// So for the whole of a cold load the shape being loaded is not yet findable, and a proxy
// that points back at an ancestor re-enters New for a key that still misses -- and loads it
// again, and again. DayZ's `dz\structures\wrecksehicles\wreck_offroad02_aban1.p3d` and
// its own `proxy\wreck_offroad02_aban1_proxy.p3d` are exactly such a pair: measured at 204
// alternating loads and ~407 repeated stack frames before the thread died on 0xC00000FD.
//
// thread_local, deliberately. This guards RE-ENTRANCY, which is per-thread by definition;
// it is not a lock and makes no claim about two threads loading the same shape, which the
// cache's own behaviour governs and which this change does not alter.
thread_local std::vector<std::string> GShapeLoadsInFlight;

bool ShapeLoadIsInFlight(const char* cacheKey)
{
    return std::find(GShapeLoadsInFlight.begin(), GShapeLoadsInFlight.end(), cacheKey) !=
           GShapeLoadsInFlight.end();
}

// Push on construction, pop on every exit (including a throw).
struct ShapeLoadInFlightScope
{
    explicit ShapeLoadInFlightScope(const char* cacheKey) { GShapeLoadsInFlight.emplace_back(cacheKey); }
    ~ShapeLoadInFlightScope() { GShapeLoadsInFlight.pop_back(); }
    ShapeLoadInFlightScope(const ShapeLoadInFlightScope&) = delete;
    ShapeLoadInFlightScope& operator=(const ShapeLoadInFlightScope&) = delete;
};
} // namespace

LODShapeWithShadow* ShapeBank::Find(const char* name, bool reversed, bool shadow) const
{
    char lowName[128];
    char cacheKey[160];
    BuildShapeCacheKey(name, reversed, shadow, lowName, cacheKey);
    // A dead slot (weak Link auto-nulled) reads as null here exactly as it does in New, which
    // then drops and reloads it. Find never mutates the cache.
    return _cache.Lookup(cacheKey);
}

LODShapeWithShadow* ShapeBank::New(const char* name, bool reversed, bool shadow)
{
    return NewFromModel(name, reversed, shadow, nullptr, nullptr);
}

thread_local uint32_t ShapeBank::_cpuOnlyLoadDepth = 0;
thread_local uint32_t ShapeBank::_worldModelDepth = 0;

ShapeBank::CpuOnlyLoadScope::CpuOnlyLoadScope(bool enabled)
{
    if (!enabled)
        return;
    POSEIDON_MAIN_THREAD_ONLY("ShapeBank::CpuOnlyLoadScope -- logical shape loads are owner-thread only");
    if (!Foundation::IsMainThread())
        return;
    ++_cpuOnlyLoadDepth;
    _active = true;
}

ShapeBank::CpuOnlyLoadScope::~CpuOnlyLoadScope()
{
    if (_active)
        --_cpuOnlyLoadDepth;
}

void ShapeBank::EnsureVisualRendering(LODShapeWithShadow* shape, bool worldModel)
{
    if (!shape || CpuOnlyLoadActive() || !shape->DeferredVisualRendering())
        return;
    POSEIDON_MAIN_THREAD_ONLY("ShapeBank::EnsureVisualRendering -- visual shape promotion is owner-thread only");
    if (!Foundation::IsMainThread())
        return;
    shape->_logicalVisualRequested = true;
    shape->_logicalWorldModel |= worldModel || _worldModelScope || _worldModelDepth != 0;
    if (!_bulkOptimizeDone || !GEngine ||
        shape->_logicalVisualState == LODShapeWithShadow::LogicalVisualState::Promoting)
        return;
    shape->_logicalVisualState = LODShapeWithShadow::LogicalVisualState::Promoting;
    try
    {
        for (int level = 0; level < shape->NLevels(); ++level)
        {
            Shape* lod = shape->Level(level);
            if (!lod)
                continue;
            for (int proxy = 0; proxy < lod->NProxies(); ++proxy)
            {
                Object* object = lod->Proxy(proxy).obj;
                if (object)
                    EnsureVisualRendering(object->GetShape(), shape->_logicalWorldModel);
            }
        }
        if (shape->_logicalWorldModel)
            GEngine->WorldShapeLoaded(shape);
        OptimizeOneShape(shape);
        shape->_logicalVisualState = LODShapeWithShadow::LogicalVisualState::None;
        shape->_logicalWorldModel = false;
        shape->_logicalVisualRequested = false;
    }
    catch (...)
    {
        shape->_logicalVisualState = LODShapeWithShadow::LogicalVisualState::Deferred;
        // A later LOD may fail after an earlier LOD acquired its buffer. The
        // retry must start without those buffers (ConvertToVBuffer asserts this).
        // Successfully promoted proxy children remain independently owned.
        for (int level = 0; level < shape->NLevels(); ++level)
            if (Shape* lod = shape->Level(level))
                lod->ReleaseVBuffer();
        throw;
    }
}

LODShapeWithShadow* ShapeBank::NewFromModel(const char* name, bool reversed, bool shadow,
                                            std::shared_ptr<Model::Model> preparsed, LoadTiming* timing,
                                            LODShapeWithShadow* preadapted)
{
    // Roadmap 4.1. ObjectStreamPrepare.hpp lists this whole entry point -- the cache
    // insert, the adapter it may run, and the vertex-buffer build under it -- as MAIN
    // THREAD ONLY, and the streamed-admission path now hands it worker-produced IR
    // (the `preparsed`/`preadapted` arguments) from the main thread. That is the exact
    // arrangement a future slice could get wrong by calling one function too early, so
    // the rule reports rather than relying on the header being reread.
    POSEIDON_MAIN_THREAD_ONLY("ShapeBank::NewFromModel -- the model bank, AssetCache and vertex-buffer "
                              "creation are main-thread only; workers produce IR (ModelCache::LoadLooseFile) "
                              "and hand it here");
    // Own transferred worker geometry immediately, including cache-hit and
    // throwing tail/registration paths. Cache links are weak and null on delete.
    std::unique_ptr<LODShapeWithShadow> ownedShape(preadapted);
    char lowName[128];
    char cacheKey[160];
    BuildShapeCacheKey(name, reversed, shadow, lowName, cacheKey);

    int remNeeded = 0;
    if (reversed)
    {
        remNeeded |= REM_REVERSED;
    }
    if (!shadow)
    {
        remNeeded |= REM_NOSHADOW;
    }

    if (LODShapeWithShadow* existing = _cache.Lookup(cacheKey))
    {
        if (timing)
            timing->cacheHit = true;
        ownedShape.reset(); // someone else installed it first; ours is redundant
        EnsureVisualRendering(existing);
        return existing;
    }
    // Proxy cycle: this key is already being loaded further up this call stack, so the miss
    // above is not "not loaded", it is "not loaded YET" -- and loading it again is what turns
    // a cyclic proxy graph into a stack overflow. Hand back an empty shape instead. It is the
    // same object this function already falls back to when a model cannot be decoded, so every
    // caller is prepared for it; returning null is NOT an option, because NewProxyObject feeds
    // the result straight into NewObject, which dereferences it.
    //
    // Cutting the cycle here rather than "resolving" it by publishing the half-built parent is
    // the conservative half of the choice: a proxy that refers to its own ancestor cannot be
    // expanded into finite geometry under any policy, so the only question is whether the
    // engine notices or dies. Publishing a partially-adapted shape would hand every OTHER
    // caller an object whose sections are not yet built.
    if (ShapeLoadIsInFlight(cacheKey))
    {
        static std::unordered_set<std::string> reported;
        if (reported.insert(cacheKey).second)
        {
            LOG_WARN(Graphics,
                     "Proxy cycle: {} contains a proxy that resolves back to itself; the inner "
                     "reference is dropped (empty shape). Nesting depth {}.",
                     lowName, GShapeLoadsInFlight.size());
        }
        ownedShape.reset(); // a cyclic-proxy load cannot adopt a shape from outside
        return new LODShapeWithShadow();
    }
    const ShapeLoadInFlightScope inFlight(cacheKey);
    // Lookup returned nullptr — either the key was never inserted, or a
    // previously-inserted shape was destroyed and its weak Link auto-nulled.
    // In the latter case the slot is still keyed but dead; drop it so the
    // upcoming Insert allocates a fresh slot instead of being short-circuited
    // by the "key already present" idempotency path in AssetCache::Insert.
    _cache.Remove(cacheKey);
    // Phase A measurement: cache-miss model load is one of the
    // top-three first-touch hitch sources (the other two are music
    // decode in WaveOAL and per-mip texture upload in
    // GlobLoadTexture).
    const auto _perfShapeLoadStart = ::Poseidon::Dev::Perf::Now();

    // The legacy LODShape loader only understands the old SP3X MLOD layout.
    // Arma samples use P3DM MLOD, which the canonical Model loader decodes
    // before ShapeAdapter expands it into the runtime vertex buffer.  Routing
    // both recognised P3D encodings through that path keeps test-model (and
    // normal-map smoke tests) from silently producing an empty mesh.
    LODShapeWithShadow* shape = nullptr;
    std::shared_ptr<const ModelCompressedSourceBirth> parsedSourceBirth;
    bool freshCanonicalAdapter = false;
    if (preadapted)
    {
        // Stage 3 of the async admission: a preparer worker already ran the tables-mode
        // conversion. Only the bank work it deferred remains -- proxies here, cache
        // insert + optimize below, all main-thread.
        shape = preadapted;
        // The worker stopped before the ODOL tail; run it here, in order (proxies
        // first, then OptimizeShapes and the rest), before OptimizeOneShape below.
        if (preparsed)
            Poseidon::Model::ShapeAdapter::FinishOdolAdapterTail(shape, *preparsed, reversed);
        if (timing)
        {
            timing->preparsed = true;
            timing->canonical = true;
        }
    }
    else
    {
        // The parse is the only stage of a cold load that does not touch engine globals, and
        // it is the stage a caller may have already done on a worker thread (Landscape's
        // asynchronous streamed admission hands the IR in as `preparsed`). When it did, no
        // ModelCache is consulted at all; when it did not, this is byte-for-byte the old path.
        const auto parseStart = ::Poseidon::Dev::Perf::Now();
        std::shared_ptr<Model::Model> model = std::move(preparsed);
        const bool arrivedPreparsed = model != nullptr;
        if (!arrivedPreparsed)
        {
            Poseidon::ModelCache cache;
            model = cache.load(lowName, &parsedSourceBirth);
        }
        if (timing)
        {
            timing->preparsed = arrivedPreparsed;
            timing->parseMs = arrivedPreparsed ? 0.0 : ::Poseidon::Dev::Perf::ElapsedMs(parseStart);
        }
        const auto adaptStart = ::Poseidon::Dev::Perf::Now();
        // OFP MLODs (every LOD "SP3X") go through the original loader, as they do in Malprave and
        // CWA 3.50: the adapter's SP3X conversion was never looked at (AST-018), and it tears
        // hand-weighted soldiers apart -- the fml_vorhees Jasons drew as a cloud of black spikes
        // through it, on wgpu and GL33, with any stock or custom moves, and correctly through
        // this loader. Arma's P3DM is what the adapter route exists for and is unchanged.
        // POSEIDON_SP3X_ADAPTER=1 sends SP3X through the adapter again.
        static const bool sp3xAdapter = []
        {
            const char* v = std::getenv("POSEIDON_SP3X_ADAPTER");
            return v && *v == '1';
        }();
        bool sp3xOnly = !sp3xAdapter && model && model->sourceFormat == "MLOD" && !model->lodLevels.empty();
        if (sp3xOnly)
        {
            for (const auto& lod : model->lodLevels)
            {
                if (lod.sourceEncoding != "SP3X")
                {
                    sp3xOnly = false;
                    break;
                }
            }
        }
        if (model && (model->sourceFormat == "ODOL" || model->sourceFormat == "MLOD") && !sp3xOnly)
        {
            LOG_INFO(Graphics, "Shape loader: canonical {} adapter for {}{}", model->sourceFormat, lowName,
                     arrivedPreparsed ? " (pre-parsed)" : "");
            // ShapeAdapter sets shape name from model.sourcePath — must use
            // the original backslash path to match ShapeBank lookup key
            model->sourcePath = lowName;
            // The banks resolve first, into tables the conversion consumes -- the
            // same call order the worker-side conversion will use, so this path
            // and that one cannot drift apart.
            Poseidon::Model::ShapeAdapter::AdapterBankTables tables;
            if (parsedSourceBirth && model->sourceFormat == "ODOL" &&
                !CpuOnlyLoadActive() && !reversed && shadow)
            {
                ArchiveSourceBinding::RetailPacReadScope primarySource(lowName);
                Poseidon::Model::ShapeAdapter::BuildAdapterBankTables(*model, tables);
                shape = Poseidon::Model::ShapeAdapter::convertToLODShape(*model, reversed, &tables);
            }
            else
            {
                Poseidon::Model::ShapeAdapter::BuildAdapterBankTables(*model, tables);
                shape = Poseidon::Model::ShapeAdapter::convertToLODShape(*model, reversed, &tables);
            }
            ownedShape.reset(shape);
            freshCanonicalAdapter = shape && parsedSourceBirth && !arrivedPreparsed &&
                model->sourceFormat == "ODOL";
            if (timing)
                timing->canonical = true;
        }
        else
        {
            LOG_INFO(Graphics, "Shape loader: legacy path for {} (canonical format={})", lowName,
                     model ? model->sourceFormat : "<unavailable>");
            shape = new LODShapeWithShadow(lowName, reversed);
            ownedShape.reset(shape);
        }
        if (timing)
            timing->adaptMs = ::Poseidon::Dev::Perf::ElapsedMs(adaptStart);
    }
    if (!shape)
    {
        ownedShape = std::make_unique<LODShapeWithShadow>();
        shape = ownedShape.get();
    }

    shape->SetRemarks(shape->Remarks() | remNeeded);
    if (!shadow)
    {
        shape->OrSpecial(NoShadow);
    }
    if (CpuOnlyLoadActive())
    {
        shape->_logicalVisualState = LODShapeWithShadow::LogicalVisualState::Deferred;
        shape->_logicalWorldModel = _worldModelScope || _worldModelDepth != 0;
    }
    if (freshCanonicalAdapter && !CpuOnlyLoadActive() && !reversed && shadow)
    {
        // First publication of this freshly adapted Shape. A cached or
        // worker-prepared Shape cannot acquire source authority here.
        static uint64_t nextCompressedBirthId = 1;
        if (nextCompressedBirthId && nextCompressedBirthId != UINT64_MAX)
        {
            shape->_compressedModelSourceBirth = std::move(parsedSourceBirth);
            shape->_compressedModelSourceBirthId = nextCompressedBirthId++;
        }
    }
    _cache.Insert(cacheKey, Link<LODShapeWithShadow>(shape));

    // A shape loaded after world load missed OptimizeAll's bulk pass, so nothing
    // would ever build its vertex buffers and Shape::Draw's T&L gate would reject
    // it for the rest of its life.  Optimize it now.  Before the bulk pass this is
    // deliberately skipped: OptimizeAll releases and rebuilds every buffer anyway,
    // so doing it per shape during map load would be pure duplicated work.
    if (_bulkOptimizeDone && !CpuOnlyLoadActive())
    {
        const auto optimizeStart = ::Poseidon::Dev::Perf::Now();
        // opt= in the cold-model row includes both retained registration (which
        // may resolve many textures) and legacy vertex-buffer optimization.
        // Split exactly one DayZ fixture only when explicitly requested; the
        // ordinary path keeps its original two calls and clock count.
        static const bool dayzSplit = [] {
            const char* flag = std::getenv("WGR_OBJECT_STREAM_DAYZ_SHAPE_SPLIT");
            return flag && std::strcmp(flag, "1") == 0;
        }();
        static bool dayzSplitLogged = false; // NewFromModel is main-thread-only.
        if (dayzSplit && !dayzSplitLogged && timing &&
            std::strcmp(lowName, R"(dz\structures\residential\tenements\tenement_small.p3d)") == 0)
        {
            const bool worldCalled = _worldModelScope && GEngine;
            if (worldCalled)
                GEngine->WorldShapeLoaded(shape);
            const double worldRegisterMs = ::Poseidon::Dev::Perf::ElapsedMs(optimizeStart);
            const auto legacyStart = ::Poseidon::Dev::Perf::Now();
            OptimizeOneShape(shape);
            const double legacyOptimizeMs = ::Poseidon::Dev::Perf::ElapsedMs(legacyStart);
            dayzSplitLogged = true;
            LOG_INFO(Graphics, "DayZ shape install split: model={} worldCalled={} worldRegisterMs={:.3f} legacyOptimizeMs={:.3f} totalOptimizeMs={:.3f}",
                     lowName, worldCalled, worldRegisterMs, legacyOptimizeMs,
                     ::Poseidon::Dev::Perf::ElapsedMs(optimizeStart));
        }
        else
        {
            // RFG-099: let the retained renderer own the mesh first.
            if (_worldModelScope && GEngine)
                GEngine->WorldShapeLoaded(shape);
            OptimizeOneShape(shape);
        }
        if (timing)
            timing->optimizeMs = ::Poseidon::Dev::Perf::ElapsedMs(optimizeStart);
    }

    const double _perfShapeLoadMs = ::Poseidon::Dev::Perf::ElapsedMs(_perfShapeLoadStart);
    if (_perfShapeLoadMs >= 1.0)
    {
        LOG_DEBUG(Graphics, "PERF: ShapeBank::New {} took {:.2f}ms ({} LODs)", lowName, _perfShapeLoadMs,
                  shape->NLevels());
    }
    ::Poseidon::Dev::Perf::EmitTraceEventAssetNum(Poseidon::Foundation::LogCategory::Graphics, "ShapeBank::New",
                                                  _perfShapeLoadStart, static_cast<const char*>(lowName), "lods",
                                                  shape->NLevels());
    return ownedShape.release();
}

void Shape::ConvertToVBuffer(VBType type)
{
    if (NVertex() <= 0)
    {
        return;
    }
    if (!ENGINE_CONFIG.enableHWTL)
    {
        return;
    }
    DoAssert(!_buffer);
    if (!_buffer)
    {
        _buffer = GEngine->CreateVertexBuffer(*this, type);
    }
}

void Shape::ReleaseVBuffer()
{
    _buffer.Free();
}

void ShapeBank::ReleaseAllVBuffers()
{
    ForEach(
        [](LODShapeWithShadow& shape)
        {
            for (int l = 0; l < shape.NLevels(); l++)
            {
                Shape* level = shape.Level(l);
                level->ReleaseVBuffer();
            }
        });
}

void LODShape::OptimizeRendering()
{
    LODShape* shape = this;
    bool reload = false;
    for (int l = 0; l < shape->NLevels(); l++)
    {
        Shape* level = shape->Level(l);
        bool optimizeHW = false;
        bool optimizeSSE = false;
        // Set when the WGR_ROAD_GPU_VB lever (see RoadGpuVertexBufferLeverEnabled) is what
        // keeps this level's buffer: it then has to be dynamic and carry saved OrigPos.
        bool onSurfaceKept = false;
        if (ENGINE_CONFIG.enableHWTL)
        {
            optimizeHW = level->NVertex() > 0 && level->NFaces() > 0;
        }
        if (ENGINE_CONFIG.enablePIII)
        {
            optimizeSSE = !shape->GetAllowAnimation() && level->NVertex() >= 16;
        }
        if (optimizeHW || optimizeSSE)
        {
            ClipFlags globalLight = shape->GetAndHints() & ClipLightMask;
            if (globalLight != (shape->GetOrHints() & ClipLightMask))
            {
                globalLight = 0;
            }
            switch (globalLight)
            {
                case ClipLightCloud:
                case ClipLightSky:
                case ClipLightStars:
                case ClipLightLine:
                    optimizeHW = optimizeSSE = false;
            }
            if (level->Special() & IsLight)
            {
                optimizeHW = optimizeSSE = false;
            }
            if (level->Special() & OnSurface)
            {
                // The SSE quad path is a CPU transform path and never wanted here.
                optimizeSSE = false;
                if (optimizeHW && OnSurfaceLevelKeepsVertexBuffer())
                {
                    onSurfaceKept = true;
                }
                else
                {
                    optimizeHW = false;
                }
            }
        }
        // no optimize on geometry levels
        if (shape->Resolution(l) > 900)
        {
            // ignore some special levels
            if (l == shape->FindFireGeometryLevel() || l == shape->FindGeometryLevel() ||
                l == shape->FindViewGeometryLevel() || l == shape->FindViewPilotGeometryLevel() ||
                l == shape->FindViewCargoGeometryLevel() || l == shape->FindViewCommanderGeometryLevel() ||
                l == shape->FindViewGunnerGeometryLevel() || l == shape->FindMemoryLevel() || l == shape->FindPaths() ||
                l == shape->FindLandContactLevel() || l == shape->FindRoadwayLevel())
            {
                continue;
            }
        }
#if USE_QUADS
        if (optimizeSSE)
        {
            if (level->PosQuad().Size() <= 0)
            {
                level->ConvertToQArray();
            }
        }
#endif
        if (optimizeHW)
        {
            const RStringB tentString("tent");
            VBType type = shape->GetAllowAnimation() ? VBDynamic : VBStatic;
            if (shape->GetPropertyDammage() == tentString)
            {
                type = VBDynamic;
            }
            if (onSurfaceKept)
            {
                // Dynamic so the first Update (inside a conform-plane draw) uploads the
                // per-vertex ClipLand selector; Shape::Draw saves OrigPos right before that
                // Update. Both reasons are in the lever's comment block above.
                type = VBDynamic;
                // Once per (shape, level) per optimize pass: the proof that the lever changed
                // an outcome. A level that reaches here already free of OnSurface (RoadType's
                // IsOnSurface swap) is not logged, so a default Takistan run lists only the
                // shapes THIS lever rescued; the ROAD_GPUPATH line in Shape::Draw covers both.
                LOG_INFO(Graphics,
                         "ROAD_GPU_VB granted: {} lod={} level={} nv={} faces={} spec=0x{:x} hints(and=0x{:x} "
                         "or=0x{:x}) allClipLandOn={} -> VBDynamic",
                         shape->Name(), l, static_cast<const void*>(level), level->NVertex(), level->NFaces(),
                         unsigned(level->Special()), unsigned(level->GetAndHints()), unsigned(level->GetOrHints()),
                         (level->GetAndHints() & ClipLandMask) == ClipLandOn ? 1 : 0);
            }
            level->ConvertToVBuffer(type);
        }
        else
        {
            // check if there are some normal arrays
            if (level->NVertex() == 0)
            {
// back conversion not possible
#if USE_QUADS
                if (level->GetVertexBuffer() || level->PosQuad().Size() > 0)
#else
                if (level->GetVertexBuffer())
#endif
                {
                    // shape is not empty
                    // we need to reload shape
                    reload = true;
                }
            }
        }
    } // for (l)
    if (reload)
    {
        Fail("Reload not possible");
        RptF("Reloading %s", shape->Name());
    }
}

void ShapeBank::OptimizeAll()
{
    // first of all: flush all vertex buffers
    // recreate vertex buffers
    if (ENGINE_CONFIG.enableHWTL)
    {
        ReleaseAllVBuffers();
        LOG_DEBUG(Graphics, "ShapeBank::OptimizeAll");
    }
    LogRoadGpuVertexBufferLever();
    ForEach([](LODShapeWithShadow& shape)
    {
        if (!shape.DeferredVisualRendering())
            shape.OptimizeRendering();
    });
    // From here on, a newly loaded shape has to build its own buffers (ShapeBank::New).
    _bulkOptimizeDone = true;
    ForEach([this](LODShapeWithShadow& shape)
    {
        if (shape._logicalVisualRequested)
            EnsureVisualRendering(&shape);
    });
}

void ShapeBank::OptimizeOneShape(LODShape* shape)
{
    if (CpuOnlyLoadActive())
        return;
    const auto* logical = dynamic_cast<const LODShapeWithShadow*>(shape);
    if (logical && logical->_logicalVisualState == LODShapeWithShadow::LogicalVisualState::Deferred)
        return;
    if (shape)
    {
        shape->OptimizeRendering();
    }
}

void ShapeBank::Clear()
{
    _cache.Clear();
    // The next world populates a fresh cache before its own bulk optimization
    // pass.  Do not optimize every incoming shape individually in that window.
    _bulkOptimizeDone = false;
}

ShapeBank Shapes;

void PrepareTexture(Texture* texture, float z2, int special, float areaOTex);

void Shape::PrepareTextures(float z2, int special) const
{
#ifndef ACCESS_ONLY
    // hint mipmap for all textures used on this shape
    for (int t = 0; t < _textures.Size(); t++)
    {
        Texture* txt = _textures[t];
        if (txt)
        {
            float areaOTex = _areaOTex[t];
            PrepareTexture(txt, z2, special, areaOTex);
        }
    }
#endif
}

} // namespace Poseidon
