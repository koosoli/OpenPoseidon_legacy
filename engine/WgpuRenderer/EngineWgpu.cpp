#include "MeshRecordClosure.hpp"
#include <Poseidon/Dev/Diag/StreamingDiag.hpp> // RFG-085 residency levers
#include <Poseidon/World/Scene/MapThings.hpp>    // RFG-095 hidden proxies
#include <Poseidon/Dev/Diag/FixedStepStats.hpp> // REN-THR-004: ticks per frame beside the frame hash
#include <Poseidon/Dev/Diag/FramePaceTrace.hpp> // PACE-001: per-frame pacing trace
#include "EngineWgpu.hpp"
#include "GeometryOwnerLedger.hpp"
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <atomic>
#include <stdexcept>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Core/BuildInfo.hpp>
#include "TerrainWgpu.hpp"
#include "WaterWgpu.hpp"
#include "TextureBankWgpu.hpp"

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Debug/DebugOverlay.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Platform/GamePaths.hpp>
#include <Poseidon/Graphics/Core/FoliageDusk.hpp>
#include <Poseidon/Graphics/Core/MeshVertex.hpp>
#include <Poseidon/Graphics/Core/ZBiasMath.hpp>
#include <Poseidon/Graphics/Core/ParkedModelRefill.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/LegacyGlass.hpp>
#include <Poseidon/Graphics/Shared/SdlWindow.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Graphics/Rendering/Effects/SmokeVolumetric.hpp> // SMK-037 soft particles
#include <Poseidon/Graphics/Rendering/Primitives/Draw2DGeometry.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/MeshBuild.hpp>
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/AsciiSearch.hpp>
#include <Poseidon/World/Scene/Pass1Submit.hpp>
#include <Poseidon/World/Entities/Infantry/Head.hpp> // FACE-001 diagnostics
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/RenderFlags.hpp>
#include <Poseidon/Graphics/Rendering/DecalDepth.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp> // TexMaterial::Combine (GPU-driven material extract)
#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ClipShape.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ObjectAdmitProfile.hpp> // streamed-admission split timers
#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp> // per-frame local-light gauges (Forward+ gate evidence)
#include <Poseidon/Graphics/Shadow/ShadowBasis.hpp>
#include <Poseidon/Graphics/Shadow/LocalShadowReach.hpp>
#include <Poseidon/Graphics/Rendering/Frame/PresentationSnapshot.hpp> // RenderSnapshot S1: the frame's environment block
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/MapTypes.hpp>                         // MapBush (spherical/canopy-normal flagging)
#include <Poseidon/Graphics/Rendering/Shape/TreeWind.hpp>
#include <Poseidon/World/Scene/Object.hpp>                     // Object accessors (GPU-driven retained scene)
#include <Poseidon/World/World.hpp>                            // live player/vehicle interaction for grass
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp> // rotor wash for grass
#include <Poseidon/World/Scene/ObjectClasses.hpp>              // ForestPlain (mode-1 conform exclusion)
#include <Poseidon/World/Terrain/Landscape.hpp>                // GLandscape->SurfaceY (GPU-driven conform bcSurfaceY)
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Effects/SmokeSystem.hpp>                // the one wind authority (grass sway, cloud drift)
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>
#include <Poseidon/Graphics/Shared/ScreenshotWriter.hpp>
#include <Poseidon/Graphics/Textures/ProceduralTexture.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp>
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
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator> // std::prev (parked-model LRU)
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
bool GpuEmatIsOpaqueSolid(const Ref<TexMaterial>& surfMat);
bool GpuEmatCullNone(const Ref<TexMaterial>& surfMat);
std::string GpuEmatFamily(const Ref<TexMaterial>& surfMat);
bool GpuEmatIsGlass(const Ref<TexMaterial>& surfMat);
// Whether a section's `.emat` authors real translucency: an `OpacityMap`
// plus `BlendMode AlphaBlend` on a non-solid surface (wire mesh, glass).
// A roughness-alpha BCR can look the same in the histogram; the OpacityMap
// is what says the alpha is coverage to blend rather than data, and the
// solid-family exclusion keeps opaque walls out even in odd combinations.
bool GpuEmatBlends(const Ref<TexMaterial>& surfMat);
bool GpuTextureIsBarkOrTrunk(const TextureWgpu* texture);
bool GpuSectionMaterialUsesLeafCardsUncached(const Ref<TexMaterial>& surfMat);
bool GpuTextureIsBarkOrTrunkUncached(const TextureWgpu* texture);

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

// MAT-043: which translated stage supplies a material's base colour.
//
// Stage0 when the material authors one. When it does not, the first authored LAYER colour
// stands in. `Multi` is a four-layer masked blend and MAT-039's compromise is already "one
// layer stands in for the base" -- it just assumed the authored layer is layer 0. Twelve of
// the 1,049 Multi materials in the A2/OA corpus declare Stage1..Stage14 and no Stage0, so
// BaseColour is genuinely absent while three good layer colours sit beside it. Those sections
// also carry no face texture (Arma 2 keeps the albedo in the RVMAT), so giving up left the
// section untextured -- flat light grey, near-black once lit.
//
// This lives in one place because the retained path (ResolveMaterialBaseColour) and the CPU
// direct path (DrawSectionTL) each had their OWN BaseColour lookup, so fixing one silently
// left the other resolving the same material differently.
//
// It can only fire where nothing would otherwise bind, so it cannot change a surface that
// resolves today.
static const Asset::Material::TranslatedSlot* EffectiveBaseColourStage(
    const Asset::Material::TranslatedMaterial& translated, const char*& whichStage)
{
    using Slot = Asset::Material::MaterialSlot;
    whichStage = "Stage0";
    if (const auto* base = translated.Find(Slot::BaseColour))
    {
        return base;
    }
    struct LayerFallback
    {
        Slot        slot;
        const char* label;
    };
    static constexpr LayerFallback kLayerFallback[] = {
        {Slot::LayerColour1, "Stage1"}, {Slot::LayerColour2, "Stage2"}, {Slot::LayerColour3, "Stage3"}};
    for (const auto& candidate : kLayerFallback)
    {
        const auto* layer = translated.Find(candidate.slot);
        if (layer && !layer->texture.isProcedural && !layer->texture.path.empty())
        {
            whichStage = candidate.label;
            return layer;
        }
    }
    return nullptr;
}

// RFG-060: a face texture named `enfa|coverage|colour` is the ONLY complete albedo the
// section has, and the material's Stage0 must not replace it.
//
// The two Reforger paths diverge here and nowhere else. Both run the same converter over
// the same `.xob`, so both get the same mesh, the same UV set 0 and the same face texture
// name; what differs is what the MATERIAL slot points at:
//
//   converted (`xob export`)  BCRMap -> `..._bcr_ca.paa`, the baked albedo with the
//                             OpacityMap already folded into its alpha (MakeDeployedEmat
//                             drops OpacityMap and writes `AlphaTest 1` in its place).
//   native (LandLoadEnfusion) BCRMap -> the RAW `..._BCR.edds` inside the `.pak`, because
//                             the section names the SOURCE `.emat` (RFG-034) and nothing
//                             rewrites its keys.
//
// Measured on `Assets/Vegetation/Bush/Corylus_Avellana/b_corylus_avellana_1.xob`, LOD0
// section 0: face texture `enfa|...polyplane_corylus_avellana_A.edds|...
// polyplane_corylus_avellana_BCR.edds`, material `..._1_polyplane.emat` naming
// `BCRMap ...polyplane_corylus_avellana_BCR.edds` + `OpacityMap ..._A.edds`. The colour
// file is 512x1024, the coverage 256x512, and the two live in separate files exactly
// because Enfusion never keeps opacity in a BCR: BCR alpha is ROUGHNESS, measured 99.8
// to 100% partial across the corpus (EmatMaterialAdapter.hpp's own census).
//
// So the override below bound an albedo whose alpha is roughness, while the section's
// cutout decision and `alpha_ref` came from the face texture and stayed set. An alpha
// test against roughness discards almost nothing -- and a polyplane's per-face UV span is
// a whole leaf CLUSTER (median 0.32 x 0.24 of the atlas on that model, not one leaf), so
// what survives is the cluster image stretched over an undiscarded quad. That is the
// "large hard polygons with a leaf texture on them" report, and the converted models were
// immune only because their deployed `.emat` happens to name the folded `_ca.paa`.
//
// EmatMaterialAdapter.hpp says this in as many words where it declines to write a Stage0
// into a generated RVMAT: "moving the colour into the material would quietly turn every
// alpha-tested leaf card back into an opaque rectangle." The composite name is the same
// statement from the other side -- it exists precisely because the coverage is a second
// file -- so a section carrying one keeps it.
//
// Narrow on purpose: it can only fire where the face texture IS a composite, which is
// built in exactly one place (LandLoadEnfusion's MountMaterialSink::TexturePath) out of
// the same material's own BCRMap and OpacityMap. There is no case where the material's
// bare BCRMap is a better albedo than the composite built from it.
static bool FaceTextureIsEnfusionComposite(const ShapeSection& section)
{
    Texture* face = section.properties.GetTexture();
    const char* name = face != nullptr ? face->Name() : nullptr;
    // RFG-070: an `enft|` tint wraps the composite, and the protection has to survive it.
    // The material's Stage0 is the RAW `_BCR.edds` -- untinted as well as uncovered -- so
    // letting it win here throws away the layer colour at the same moment it throws away
    // the coverage.
    return name != nullptr && std::strncmp(Poseidon::Enfusion::SkipLayerTint(name), "enfa|", 5) == 0;
}

// PERF-015 A/B lever, and it governs BOTH halves of the per-section classification cost:
// this search's allocations, and the material/texture classification caches below. One
// switch, because they are one change -- measuring them apart would report two numbers
// neither of which is what the frame does. `WGR_SECTION_CLASS_CACHE=0` restores the
// 2026-09-01 behaviour exactly, in the same binary, so the A/B needs no rebuild.
static bool SectionClassCacheEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SECTION_CLASS_CACHE");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

// The pre-PERF-015 spelling, kept ONLY so the lever can measure what it removed. Two heap
// allocations and two full lowercase passes per question asked; the classifiers below ask
// between two and six each, per section, per frame.
bool ContainsNoCaseAllocating(const char* text, const std::string& part)
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

bool ContainsNoCase(const char* text, const std::string& part)
{
    return SectionClassCacheEnabled() ? render::ContainsNoCaseAscii(text, part)
                                      : ContainsNoCaseAllocating(text, part);
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
float StaticEmitterRadianceTargetUncached(const Ref<TexMaterial>& surfMat)
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

// A/B levers for the two whole-model retained-path bails added for the A1 window / legacy
// vegetation work (2026-08-27). Each pushes an ENTIRE model onto the direct WGPU path, which is
// a much larger change than the section it is aimed at, so both must be switchable inside ONE
// binary -- `WGR_WINDOW_FALLBACK=0` / `WGR_PLANTS_FALLBACK=0` -- or an A/B costs a rebuild and a
// redeploy and stops being run. Read once: consulted per model registration, not per draw.
// OFF by default, and this is the fix for the 2026-08-27 ghost-building report. Measured A/B in
// one binary at Takistan (5081.64, 6077.97, 147.29 / az 88.5): with the bail ON the whole
// `mil_barracks_i_ep1` -- walls, roof, soffit -- composites the background through itself; with
// `WGR_WINDOW_FALLBACK=0` it is solid concrete. Same shape on Stratis.
//
// It was not the window heuristic being slightly too wide. It fired on
// `ca\structures_e\data\windows\windowwire_doors_mil_co.paa`, an OPAQUE texture that is a window
// only by virtue of its DIRECTORY -- but tightening the match does not help, because the same
// model also carries `alfa_windows_mil_ca.paa`, a genuine Blend pane that any honest window test
// must match. There is no predicate over texture name or alpha class that separates the A1 hotel
// (which needs the bail) from this barracks (which is destroyed by it).
//
// The real defect is one level down and is NOT fixed here: the direct WGPU path draws these
// models translucent at close range while the retained path draws them solid. Established, so
// the next session does not re-derive it:
//   * every section of the model reports special=0x0 -> BlendMode::Opaque, DepthMode::Normal
//     (WGR_SECTION_SKIP_TRACE=alfa_windows_mil), so it is not an IsAlpha spec bit;
//   * zero UNTEXTURED draws, so it is not the white fallback;
//   * unchanged by WGR_COCKPIT_BLEND_LEGACY=1, WGR_GLASS_SHEEN=0, WGR_MULTI_LAYER_NORMALS=0
//     and by WGR_GPU_DRIVEN=0, so it is neither the blend-revisit depth mode, the MAT-051
//     glass alpha floor, the Multi layer composition, nor GPU-driven bookkeeping;
//   * the same model renders solid in `--viewer` and solid in-world from 15 m, so it is
//     close-range / LOD0 specific, not a model or material fault.
// Until that is understood, no model belongs on the direct path wholesale. Set
// `WGR_WINDOW_FALLBACK=1` to get the A1 hotel window fills back and the ghost buildings with
// them.
static bool WindowSectionFallbackEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_WINDOW_FALLBACK");
        return v && std::strcmp(v, "0") != 0;
    }();
    return enabled;
}

// OFF by default: SUPERSEDED, not reverted. The bail existed because A1 `ca\plants\` canopies
// vanished under retained ownership, and pushing the model to the direct path brought them back
// -- but see-through, which is the "some tree canopies have become transparent" half of the same
// 2026-08-27 report. Both symptoms were one cause: those cards are authored alpha-TESTED and the
// alpha histogram mis-reads their downscaled LODs as translucent, so each LOD landed on a
// different side of the Blend boundary (render::IsLegacyPlantCutoutTexture carries the measured
// per-LOD split). Fixing the routing fixes both paths, so nothing needs to leave the retained
// set: measured at Sahrani (7523.21, 8951.72, 9.21 / az 117.9) with WGR_PLANTS_FALLBACK=0, the
// canopy is now full AND opaque -- better than either state the bail could produce.
// `WGR_PLANTS_FALLBACK=1` restores it if a plant family turns up that the routing fix misses.
static bool LegacyPlantFallbackEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_PLANTS_FALLBACK");
        return v && std::strcmp(v, "0") != 0;
    }();
    return enabled;
}

// BI uses Czech `okno`/`okna` for windows. Keep this spelling-based classification here so the
// retained-path admission decision and the night-emissive policy cannot slowly diverge.
bool MaterialIsWindowUncached(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    const char* path = surfMat->GetName().Data();
    return ContainsNoCase(path, "okn") || ContainsNoCase(path, "window");
}

// PERF-015 -- the per-material answers, computed once per distinct material instead of once
// per section per frame.
//
// Every classification above is a PURE FUNCTION OF THE MATERIAL'S PATH TEXT. Recomputing it
// per section per frame is what made these the largest single term in `land:obj` on
// Chernarus+: StaticEmitterRadianceTarget alone asks six case-insensitive questions, each of
// which built and lowercased two std::strings, and DrawSectionTL reaches it TWICE per section
// (once through the emissive scale, once through MaterialEmitsAtNight) on top of the window
// test and the bark test.
//
// KEYED ON THE MATERIAL'S ADDRESS, VALIDATED ON ITS PATH. The address alone would be unsafe:
// a material can be released and another allocated at the same address, and the entry would
// then answer for the wrong asset. Storing the path settles that without depending on object
// lifetimes at all -- the answers are a function of the path, so an entry whose path still
// compares equal IS the correct answer whatever object now occupies the address, and a
// mismatch simply recomputes. The cache is therefore correct by construction rather than by
// an argument about when materials are freed.
//
// thread_local, so the per-section path takes no lock. The string-keyed leaf-card cache this
// fronts holds a mutex, and a mutex acquisition per section per frame is exactly the class of
// cost being removed here.
struct MaterialClass
{
    std::string path;
    float       emitterTarget = 1.0f;
    bool        isWindow = false;
    bool        leafCards = false;
    bool        leafCardsResolved = false;

    // PERF-016 -- the per-material half of DrawSectionTL's binding block, resolved once and
    // replayed per section. Everything here is a pure function of the material's path (the
    // translated material a path loads to is deterministic for a run), so the same
    // address-keyed / path-validated contract as the fields above covers it. What is
    // deliberately NOT cached: texture handles. The bank Load() + EnsureUploaded() pair runs
    // per section on the fast path too, because that is what keeps residency honest -- Load
    // marks the texture used, EnsureUploaded re-uploads after an eviction, and a cached
    // handle would silently outlive both.
    enum BaseKind : uint8_t
    {
        BaseNone = 0,     // no bindable Stage0: section keeps its face texture
        BaseImage = 1,    // a real image stage: always bind
        BaseConstant = 2, // procedural constant: bind only when the section has no face texture
                          // (or the material is CalmWater) -- the DZ-006 precedence
    };
    bool     bindResolved = false;
    BaseKind baseKind = BaseNone;
    bool     isCalmWater = false;
    bool     reflective = false; // shaderFamily == CalmWater (DZ-003)
    bool     hasNormal = false;
    RStringB baseLoadName;
    RStringB normalLoadName;
    // The `.emat` `Color` surface tint (MatPBRBasic wire mesh), as linear
    // RGB. Identity until a bind resolve records it; the legacy TexMaterial
    // path never carried it, so without this the BCR albedo draws untinted
    // (NetFence_Wire's near-white metal instead of its dark authoring).
    float tint[3] = {1.0f, 1.0f, 1.0f};
};

// PERF-016 A/B lever. ON by default; `WGR_SECTION_BIND_CACHE=0` restores the per-section
// re-resolution exactly, in the same binary. Independent of WGR_SECTION_CLASS_CACHE only
// downward: the bind cache lives in the MaterialClass slot, so turning the class cache off
// turns this off too (MaterialClassSlot returns null and every caller falls back).
static bool SectionBindCacheEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SECTION_BIND_CACHE");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

// Null when the cache does not apply (lever off, no material, no path); callers fall back to
// the uncached form, which returns the same answer for those inputs.
MaterialClass* MaterialClassSlot(const Ref<TexMaterial>& surfMat)
{
    if (!SectionClassCacheEnabled() || surfMat.IsNull())
        return nullptr;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path)
        return nullptr;
    thread_local std::unordered_map<const void*, MaterialClass> cache;
    MaterialClass& slot = cache[static_cast<const void*>(surfMat.GetRef())];
    Pass1Submit::gSecClassCalls.fetch_add(1, std::memory_order_relaxed);
    if (slot.path.empty() || slot.path != path)
    {
        Pass1Submit::gSecClassMisses.fetch_add(1, std::memory_order_relaxed);
        slot.path = path;
        slot.emitterTarget = StaticEmitterRadianceTargetUncached(surfMat);
        slot.isWindow = MaterialIsWindowUncached(surfMat);
        slot.leafCardsResolved = false;
        // PERF-016: the binding half re-resolves too -- it is a function of the same path.
        slot.bindResolved = false;
        slot.baseKind = MaterialClass::BaseNone;
        slot.isCalmWater = false;
        slot.reflective = false;
        slot.hasNormal = false;
        slot.baseLoadName = RStringB();
        slot.normalLoadName = RStringB();
        slot.tint[0] = slot.tint[1] = slot.tint[2] = 1.0f;
    }
    return &slot;
}

float StaticEmitterRadianceTarget(const Ref<TexMaterial>& surfMat)
{
    if (const MaterialClass* cached = MaterialClassSlot(surfMat))
        return cached->emitterTarget;
    return StaticEmitterRadianceTargetUncached(surfMat);
}

bool MaterialIsWindow(const Ref<TexMaterial>& surfMat)
{
    if (const MaterialClass* cached = MaterialClassSlot(surfMat))
        return cached->isWindow;
    return MaterialIsWindowUncached(surfMat);
}

// Is this material's emissive REAL light -- a fixture that should still glow at midnight? Only
// the fixture families StaticEmitterRadianceTarget already recognises, plus lit windows. Every
// other `emmisive` in the imported corpora is fixed-function brightness compensation (Arma 2 OA
// vegetation ships {2.6,2.3,2.0}, the Arma 3 pine crown 1.0), and the shaders fade it with the
// sun (shader3d.wgsl / gpu_driven.wgsl `emissive_night`); the flag this feeds is what exempts a
// lamp from that fade. Owner-visible symptom this closes: forests self-illuminating at night on
// Arma 2, Arma 3 and DayZ alike, surviving WGR_AUTO_EXPOSURE=0.
bool MaterialEmitsAtNight(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    if (StaticEmitterRadianceTarget(surfMat) < 1.0f)
        return true;
    return MaterialIsWindow(surfMat);
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

// UP-126: VBSmallDiscardable is still classified as dynamic by the WGPU bridge, but
// changing that contract without evidence risks stale animated shadows and rotor poses.
// This opt-in census measures the work at the only authoritative boundary: Update(),
// where a dirty mesh is rebuilt, hashed, and either uploaded or suppressed. Keeping the
// branch behind one startup-time flag means normal play pays neither clocks nor atomics.
struct VbUpdateCounters
{
    std::atomic<uint64_t> createRequests{0};
    std::atomic<uint64_t> createBytes{0};
    std::atomic<uint64_t> destroyRequests{0};
    std::atomic<uint64_t> cancelledCreates{0};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> cleanSkips{0};
    std::atomic<uint64_t> sharedSkips{0};
    std::atomic<uint64_t> skinnedSkips{0};
    std::atomic<uint64_t> builds{0};
    std::atomic<uint64_t> hashSkips{0};
    std::atomic<uint64_t> pendingRewrites{0};
    std::atomic<uint64_t> uploads{0};
    std::atomic<uint64_t> refusedUploads{0};
    std::atomic<uint64_t> uploadBytes{0};
    std::atomic<uint64_t> updateNs{0};
    std::atomic<uint64_t> updateMaxNs{0};
    std::atomic<uint64_t> hashNs{0};
    std::atomic<uint64_t> hashMaxNs{0};
};

constexpr size_t kVbTypeCount = 4;
std::array<VbUpdateCounters, kVbTypeCount> GVbUpdateCounters;

bool VbUpdateStatsEnabled()
{
    static const bool enabled = []
    {
        const char* value = std::getenv("WGR_VB_UPDATE_STATS");
        return value != nullptr && std::atoi(value) != 0;
    }();
    return enabled;
}

void AtomicMax(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t old = target.load(std::memory_order_relaxed);
    while (old < value && !target.compare_exchange_weak(old, value, std::memory_order_relaxed))
    {
    }
}

class ScopedVbUpdateTimer
{
  public:
    explicit ScopedVbUpdateTimer(VbUpdateCounters* counters) : _counters(counters)
    {
        if (_counters)
        {
            _counters->calls.fetch_add(1, std::memory_order_relaxed);
            _start = std::chrono::steady_clock::now();
        }
    }

    ~ScopedVbUpdateTimer()
    {
        if (!_counters)
        {
            return;
        }
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _start).count());
        _counters->updateNs.fetch_add(ns, std::memory_order_relaxed);
        AtomicMax(_counters->updateMaxNs, ns);
    }

  private:
    VbUpdateCounters* _counters = nullptr;
    std::chrono::steady_clock::time_point _start{};
};

void RecordVbHashTime(VbUpdateCounters* counters, std::chrono::steady_clock::time_point start)
{
    if (!counters)
    {
        return;
    }
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    counters->hashNs.fetch_add(ns, std::memory_order_relaxed);
    AtomicMax(counters->hashMaxNs, ns);
}

void LogVbUpdateStats()
{
    if (!VbUpdateStatsEnabled())
    {
        return;
    }

    static constexpr const char* names[kVbTypeCount] = {
        "dynamic", "static", "big-discardable", "small-discardable"};
    for (size_t i = 0; i < kVbTypeCount; ++i)
    {
        VbUpdateCounters& c = GVbUpdateCounters[i];
        const uint64_t creates = c.createRequests.exchange(0, std::memory_order_relaxed);
        const uint64_t createBytes = c.createBytes.exchange(0, std::memory_order_relaxed);
        const uint64_t destroys = c.destroyRequests.exchange(0, std::memory_order_relaxed);
        const uint64_t cancelled = c.cancelledCreates.exchange(0, std::memory_order_relaxed);
        const uint64_t calls = c.calls.exchange(0, std::memory_order_relaxed);
        const uint64_t clean = c.cleanSkips.exchange(0, std::memory_order_relaxed);
        const uint64_t shared = c.sharedSkips.exchange(0, std::memory_order_relaxed);
        const uint64_t skinned = c.skinnedSkips.exchange(0, std::memory_order_relaxed);
        const uint64_t builds = c.builds.exchange(0, std::memory_order_relaxed);
        const uint64_t hashSkips = c.hashSkips.exchange(0, std::memory_order_relaxed);
        const uint64_t pending = c.pendingRewrites.exchange(0, std::memory_order_relaxed);
        const uint64_t uploads = c.uploads.exchange(0, std::memory_order_relaxed);
        const uint64_t refused = c.refusedUploads.exchange(0, std::memory_order_relaxed);
        const uint64_t bytes = c.uploadBytes.exchange(0, std::memory_order_relaxed);
        const uint64_t updateNs = c.updateNs.exchange(0, std::memory_order_relaxed);
        const uint64_t updateMaxNs = c.updateMaxNs.exchange(0, std::memory_order_relaxed);
        const uint64_t hashNs = c.hashNs.exchange(0, std::memory_order_relaxed);
        const uint64_t hashMaxNs = c.hashMaxNs.exchange(0, std::memory_order_relaxed);
        if (creates == 0 && destroys == 0 && calls == 0 && uploads == 0 && pending == 0)
        {
            continue;
        }
        LOG_INFO(Graphics,
                 "VB update [{}] create={} create-bytes={} destroy={} cancel-create={} | "
                 "calls={} clean={} shared={} skinned={} builds={} hash-skip={} "
                 "pending={} uploads={} refused={} bytes={} | update={:.3f} ms max={:.3f} ms | "
                 "hash={:.3f} ms max={:.3f} ms",
                 names[i], creates, createBytes, destroys, cancelled, calls, clean, shared, skinned, builds,
                 hashSkips, pending, uploads, refused, bytes,
                 static_cast<double>(updateNs) / 1.0e6, static_cast<double>(updateMaxNs) / 1.0e6,
                 static_cast<double>(hashNs) / 1.0e6, static_cast<double>(hashMaxNs) / 1.0e6);
    }
}

// Only the immutable ledger-ON factory creates this slot type. shared_ptr's
// converting constructor retains the derived deleter; normal slots need no vptr
// or diagnostic pointer. No worker/draw probes or RTTI classify a base slot.
struct AttributedCpuMeshSlot final : CpuMeshSlot
{
    std::shared_ptr<render::StandaloneMeshRetirementWitness> standaloneWitness;
};
class VertexBufferWgpu : public VertexBuffer
{
  public:
    WgrRenderer* renderer = nullptr;
    // REN-THR-009 — the per-frame vertex re-upload and the destroy go through here, not
    // through `renderer`. Borrowed from the renderer once at creation and valid until
    // wgr_destroy; see the lifetime contract on wgr_uploader_get. Never released.
    //
    // `renderer` is still held for the destroy fallback below (and for the null test that
    // stands in for "is there a backend at all").
    //
    // REN-THR-011 — SetSkinData no longer calls the exclusive handle. The audit found
    // wgr_mesh_set_skin structural (it creates a GPU buffer, mutates the mesh record, and
    // drops both skin-bake bind-cache entries the draw path reads), and one-time rather
    // than per-frame — VertexBufferWgpu's own `skinned` guard makes it a creation-time
    // call. It belongs with wgr_mesh_create, and wgr_mesh_create is now queued, so it is
    // queued too, through `link`.
    const WgrUploader* uploader = nullptr;
    // The mesh's identity. Shared with the queued create and with any draw recorded before
    // that create was drained; see CpuMeshSlot for why this is a slot and not a handle.
    std::shared_ptr<CpuMeshSlot> slot;
    std::shared_ptr<CpuMeshLink> link;
    int vertexCount = 0;
    bool isDynamic = false;
    VBType bufferType = VBStatic;
    AutoArray<render::mesh::MeshSection> sections;

    // Content hash of the last vertex data actually uploaded, so Update can skip a
    // redundant GPU copy when the rebuilt vertices are byte-identical. VBDynamic
    // shapes (GetAllowAnimation) re-upload every frame they draw, and conformed
    // on-surface objects (roads/paths) dirty their buffer every frame in
    // Object::Animate even though their terrain-conformed geometry is static — both
    // produced a per-frame `wgr_3d_vbuf` staging copy + barrier for unchanged data.
    uint64_t lastUploadHash = 0;
    bool haveUploadHash = false;
    // Queued draws must not all observe the last helicopter's CPU pose.
    // Copies belong to this shape buffer and are reused only in a later frame.
    std::vector<std::unique_ptr<VertexBufferWgpu>> poseCopies;
    uint64_t poseFrame = 0;
    size_t nextPoseCopy = 0;

    // GPU skinning state
    // `skinned` flips on once SetSkinData has uploaded the bind pose + weights
    // `palette` holds the current frame's model-space bone matrices
    bool skinned = false;
    // FACE-001: true while the last Update uploaded a CPU-posed frame of a skinned mesh,
    // so the next non-posed Update knows it owes the bind pose a restore.
    bool wasCpuPosed = false;
    int paletteCount = 0;
    std::vector<Matrix4> palette;

    // The renderer handle, or 0 while the queued create has not been drained. Every use site
    // must be able to cope with 0 — that is the whole shape of REN-THR-011.
    uint64_t RendererMesh() const { return slot ? slot->renderer : 0; }

    VertexBufferWgpu(WgrRenderer* r, const WgrUploader* up, std::shared_ptr<CpuMeshSlot> s,
                     std::shared_ptr<CpuMeshLink> l, int nv, bool dynamic, VBType type)
        : renderer(r), uploader(up), slot(std::move(s)), link(std::move(l)), vertexCount(nv), isDynamic(dynamic),
          bufferType(type)
    {
    }
    ~VertexBufferWgpu() override
    {
        if (!slot)
        {
            return;
        }
        // NOT counted by EngineWgpu::_ops: this destructor holds its own renderer
        // pointer and runs wherever the last reference is dropped, which is a producer-
        // side mutation site outside the engine object entirely. REN-THR-001's list of
        // ten call sites missed it for exactly that reason.
        //
        // REN-THR-009: it is now parked rather than done. The real destroy drops both
        // skin-bake bind-cache entries (which the draw path reads) and returns this mesh's
        // vertex/index ranges to the geometry pool's free lists, where a later create can
        // be handed them — neither may happen while a frame is in flight. The renderer
        // drains the parked handles at the top of its own frame. The handle stops
        // accepting uploads immediately, at enqueue, not at drain.
        //
        // REN-THR-011: `dead` is set unconditionally and FIRST, because the interesting case
        // is the one where there is nothing to destroy — a buffer created and released inside
        // a single draw phase, before the drain that would have defined its mesh ever ran.
        // VBSmallDiscardable shadow meshes do exactly this. Cancelling the queued create is
        // strictly better than fulfilling it and then destroying it: no pool allocation, no
        // bind-cache clear, no generation bump for a mesh nobody can name any more.
        slot->dead = true;
        if (VbUpdateStatsEnabled())
        {
            const size_t typeIndex = static_cast<size_t>(bufferType);
            if (typeIndex < kVbTypeCount)
            {
                GVbUpdateCounters[typeIndex].destroyRequests.fetch_add(1, std::memory_order_relaxed);
                if (slot->renderer == 0)
                {
                    GVbUpdateCounters[typeIndex].cancelledCreates.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        uint64_t sharedRetirement = 0;
        if (slot->sharedLifetime.Release(sharedRetirement))
        {
            // One allocation serves every CPU borrower. Only the last owner
            // returns a retirement handle; dispatch is gated against shutdown.
            slot->sharedLifetime.Dispatch(sharedRetirement, [&](uint64_t handle) {
                if (uploader) wgr_uploader_mesh_destroy(uploader, handle);
                else if (renderer) wgr_mesh_destroy(renderer, handle);
            });
            std::vector<SVertex>().swap(slot->verts);
            std::vector<VertexIndex>().swap(slot->indices);
            return;
        }
        if (slot->renderer == 0)
        {
            std::vector<SVertex>().swap(slot->verts);
            std::vector<VertexIndex>().swap(slot->indices);
            return;
        }
        if (uploader)
        {
            wgr_uploader_mesh_destroy(uploader, slot->renderer);
        }
        else if (renderer)
        {
            wgr_mesh_destroy(renderer, slot->renderer);
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
        VbUpdateCounters* diag = nullptr;
        if (VbUpdateStatsEnabled())
        {
            const size_t typeIndex = static_cast<size_t>(bufferType);
            if (typeIndex < kVbTypeCount)
            {
                diag = &GVbUpdateCounters[typeIndex];
            }
        }
        ScopedVbUpdateTimer updateTimer(diag);
        if (!renderer || !slot)
        {
            return;
        }
        // FACE-001: a skinned mesh normally stays at bind pose because the GPU poses it.
        // While `cpuPosed` is set the animation system poses it on the CPU instead (the
        // soldier face rig writes vertex offsets no bone matrix can express), so the posed
        // bytes MUST be uploaded and the draw must not re-skin them. When the request drops
        // the mesh has to go back to holding its bind pose, or the skinned pipeline would
        // pose an already-posed mesh for the rest of its life.
        if (skinned)
        {
            if (!cpuPosed)
            {
                if (wasCpuPosed && src.OriginalPosValid() && src.NVertex() == vertexCount && slot->sharedProducer == 0)
                {
                    wasCpuPosed = false;
                    Poseidon::GFaceDiag().updRestore++;
                    std::vector<SVertex> bind(static_cast<size_t>(vertexCount));
                    render::mesh::BuildOrigVertices(src, bind.data());
                    haveUploadHash = false;
                    bufferDirty = false;
                    if (slot->renderer == 0)
                    {
                        slot->verts = std::move(bind);
                    }
                    else if (uploader)
                    {
                        wgr_uploader_mesh_update(uploader, slot->renderer, AsMeshVerts(bind));
                    }
                    else
                    {
                        wgr_mesh_update(renderer, slot->renderer, AsMeshVerts(bind));
                    }
                }
                if (diag) { diag->skinnedSkips.fetch_add(1, std::memory_order_relaxed); }
                Poseidon::GFaceDiag().updSkinSkip++;
                return;
            }
            wasCpuPosed = true;
            Poseidon::FaceAnimDiag& fd = Poseidon::GFaceDiag();
            fd.updSkinPosed++;
            fd.updBytes += vertexCount;
            if (Poseidon::FaceDebugEnabled() && src.OriginalPosValid())
            {
                // The one oracle that cannot lie about whether the CPU pose is real: how far
                // the shape's live vertices sit from its bind pose at the moment of upload.
                float worst = 0;
                for (int i = 0; i < src.NVertex(); i++)
                {
                    const float d2 = (src.Pos(i) - src.OrigPos(i)).SquareSize();
                    if (d2 > worst)
                    {
                        worst = d2;
                    }
                }
                const double w = std::sqrt(static_cast<double>(worst));
                if (w > fd.poseMaxDelta)
                {
                    fd.poseMaxDelta = w;
                }
            }
        }
        if (slot->sharedProducer != 0)
        {
            // RFG-088: the bytes are the retained model's; a static level never changes them.
            bufferDirty = false;
            if (diag) { diag->sharedSkips.fetch_add(1, std::memory_order_relaxed); }
            if (cpuPosed) { Poseidon::GFaceDiag().updShared++; }
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
            if (diag) { diag->cleanSkips.fetch_add(1, std::memory_order_relaxed); }
            if (cpuPosed) { Poseidon::GFaceDiag().updClean++; }
            return;
        }
        if (src.NVertex() != vertexCount)
        {
            if (cpuPosed) { Poseidon::GFaceDiag().updCount++; }
            return;
        }
        std::vector<SVertex> verts(static_cast<size_t>(vertexCount));
        if (diag) { diag->builds.fetch_add(1, std::memory_order_relaxed); }
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
        // FACE-001 kill door: POSEIDON_FACE_LIFT displaces every uploaded vertex of a
        // CPU-posed mesh by that many metres in +Z. If the character does not visibly move,
        // the bytes this function uploads are not the bytes the draw reads, and no amount of
        // arguing about the face selections matters.
        if (cpuPosed)
        {
            static const float lift = []
            {
                const char* e = std::getenv("POSEIDON_FACE_LIFT");
                return e ? static_cast<float>(std::atof(e)) : 0.0f;
            }();
            if (lift != 0.0f)
            {
                for (auto& v : verts)
                {
                    v.pos = Vector3P(v.pos.X(), v.pos.Y(), v.pos.Z() + lift);
                }
            }
            Poseidon::GFaceDiag().drawVerts = vertexCount;
        }
        bufferDirty = false;
        const auto hashStart = diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const uint64_t hash = HashVertices(verts.data(), verts.size() * sizeof(SVertex));
        RecordVbHashTime(diag, hashStart);
        if (haveUploadHash && hash == lastUploadHash)
        {
            if (diag) { diag->hashSkips.fetch_add(1, std::memory_order_relaxed); }
            if (cpuPosed) { Poseidon::GFaceDiag().updHash++; }
            return;
        }
        lastUploadHash = hash;
        haveUploadHash = true;
        if (cpuPosed) { Poseidon::GFaceDiag().updSent++; }
        // REN-THR-009 — the one per-frame producer-side mutation that does NOT go through
        // the exclusive renderer handle. The audit found this path touches nothing
        // structural: a shared slotmap lookup, two field reads, and one
        // Queue::write_buffer into a vertex range that can never move. Queuing it instead
        // would copy a full vertex array every frame for every skinned character, which is
        // the wrong answer and the reason REN-THR-006 left this row of its table alone.
        //
        // REN-THR-011 — unless the create has not been drained yet, in which case there is no
        // mesh to upload into and the right move is to rewrite what the create will upload.
        // The end state is byte-identical to today's create-then-update pair, one drain later,
        // and it is what keeps the conformed-road case exact: that road's FIRST Update is the
        // one that replaces BuildVertices bytes with BuildOrigVertices bytes carrying the
        // ClipLand selector, and it runs in the same draw phase as the create.
        if (slot->renderer == 0)
        {
            if (diag) { diag->pendingRewrites.fetch_add(1, std::memory_order_relaxed); }
            if (cpuPosed) { Poseidon::GFaceDiag().updPending++; }
            slot->verts = std::move(verts);
            return;
        }
        if (uploader)
        {
            const uint32_t okUp = wgr_uploader_mesh_update(uploader, slot->renderer, AsMeshVerts(verts));
            if (diag)
            {
                if (okUp != 0)
                {
                    diag->uploads.fetch_add(1, std::memory_order_relaxed);
                    diag->uploadBytes.fetch_add(verts.size() * sizeof(SVertex), std::memory_order_relaxed);
                }
                else
                {
                    diag->refusedUploads.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (cpuPosed)
            {
                Poseidon::GFaceDiag().updUploader++;
                if (okUp == 0) { Poseidon::GFaceDiag().updRefused++; }
            }
        }
        else
        {
            if (diag)
            {
                diag->uploads.fetch_add(1, std::memory_order_relaxed);
                diag->uploadBytes.fetch_add(verts.size() * sizeof(SVertex), std::memory_order_relaxed);
            }
            if (cpuPosed) { Poseidon::GFaceDiag().updDirect++; }
            wgr_mesh_update(renderer, slot->renderer, AsMeshVerts(verts));
        }
    }

    // One-time: upload the bind pose (OrigPos/OrigNorm) + per-vertex bone indices
    // and weights, marking the mesh eligible for the skinned pipeline.
    void SetSkinData(const AnimationRTWeights& weightTable, const Shape& bindShape) override
    {
        if (slot && slot->sharedProducer != 0)
        {
            // RFG-088: skinning rewrites the mesh, and this one belongs to the retained model.
            // Retained registration refuses non-static objects, so a shared level is a static
            // one and this should not be reached; logged rather than assumed.
            static std::atomic<uint32_t> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
                LOG_WARN(Graphics, "Wgpu: SetSkinData on a buffer sharing a retained mesh -- skin ignored");
            return;
        }
        if (skinned || !renderer || !slot || bindShape.NVertex() != vertexCount)
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

        // REN-THR-011 — queued, not called. Both halves are structural: the update writes the
        // bind pose the skinned pipeline reads, and set_skin allocates a new GPU buffer and
        // drops both bake_bind_cache entries the draw path reads, neither of which may land
        // while a frame is in flight. They ride ONE op so their order can never come apart,
        // and the op sits behind this mesh's create in the same FIFO queue.
        //
        // `skinned` flips here, at enqueue, not at drain — deliberately. It is what makes
        // BeginMeshTL and AddShadowCaster attach a bone palette to draws recorded later in
        // THIS draw phase, and those draws are rendered after the drain that applies the skin.
        // Flipping it at the drain instead would cost every skinned mesh one frame of drawing
        // at bind pose with no palette, which is a visible pop.
        if (!link || !link->engine)
        {
            // No engine to queue against (teardown). Leave `skinned` false so a later call
            // retries rather than marking a mesh skinned that will never receive weights.
            return;
        }
        link->engine->EnqueueCpuMeshSetSkin(slot, std::move(verts), std::move(bones), std::move(weights));
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

// Only ON buffers carry diagnostic lifetime storage. The default buffer's size,
// allocation and destruction path remain unchanged.
class AttributedVertexBufferWgpu final : public VertexBufferWgpu
{
    std::shared_ptr<render::GeometryOwnerLedger> _ledger;
    const uint64_t _epoch, _borrower;
    std::shared_ptr<AttributedCpuMeshSlot> _attributedSlot;
public:
    AttributedVertexBufferWgpu(WgrRenderer* r, const WgrUploader* up, std::shared_ptr<AttributedCpuMeshSlot> s,
        std::shared_ptr<CpuMeshLink> l, int nv, bool dynamic, VBType type,
        std::shared_ptr<render::GeometryOwnerLedger> ledger, uint64_t epoch, uint64_t borrower)
        : VertexBufferWgpu(r, up, s, std::move(l), nv, dynamic, type),
          _ledger(std::move(ledger)), _epoch(epoch), _borrower(borrower), _attributedSlot(std::move(s)) {}
    ~AttributedVertexBufferWgpu() override
    {
        // Numeric immutable identity only: this may run outside the owner drain.
        _ledger->QueueCpuBorrowerDetached(_epoch, _borrower);
        if (!slot) return;
        // Diagnostic subclass only: cancel a queued create before releasing its
        // shared association. The unchanged base destructor repeats Release0
        // and performs normal stats/payload cleanup, never a second dispatch.
        slot->dead = true;
        const uint64_t producer = slot->sharedLifetime.Producer();
        uint64_t retirement = 0;
        bool dispatched = false;
        if (slot->sharedLifetime.Release(retirement))
            slot->sharedLifetime.Dispatch(retirement, [&](uint64_t handle) {
                if (uploader) wgr_uploader_mesh_destroy(uploader, handle);
                else if (renderer) wgr_mesh_destroy(renderer, handle);
                else return;
                dispatched = true;
            });
        // Metadata publication is outside both lifetime/epoch dispatch locks.
        if (dispatched) _ledger->QueueAllocationRetirementScheduled(_epoch, producer);
        else if (_attributedSlot->standaloneWitness)
        {
            slot->sharedLifetime.SuppressHandledStandaloneRetirement();
            const auto retirement = _attributedSlot->standaloneWitness->Release();
            _attributedSlot->standaloneWitness->Dispatch(retirement, [&](uint64_t handle) {
                if (uploader) wgr_uploader_mesh_destroy(uploader, handle);
                else if (renderer) wgr_mesh_destroy(renderer, handle);
                else return;
                dispatched = true;
            });
            if (dispatched) _ledger->QueueAllocationRetirementScheduled(_epoch, _attributedSlot->standaloneWitness->Producer());
        }
    }
};

} // namespace

EngineWgpu::EngineWgpu(const GraphicsEngineParams& params) : _windowed(params.useWindow)
{
    const char* ownerLedger = std::getenv("WGR_GEOMETRY_OWNER_LEDGER");
    if (ownerLedger && std::strcmp(ownerLedger, "1") == 0)
    {
        _geometryOwnerLedger = std::make_shared<render::GeometryOwnerLedger>(render::GeometryOwnerLedger::Limits{});
        try {
            _standaloneWitnessBudget = std::make_shared<render::StandaloneMeshWitnessBudget>();
            if (!_sharedMeshEpoch) _sharedMeshEpoch = std::make_shared<render::SharedMeshEpoch>();
        }
        catch (...) { /* Optional witness refusal remains incomplete, original destruction fallback. */ }
        _geometryOwnerLedger->BeginEpoch(_geometryOwnerEpoch, true);
        const char* meshAck = std::getenv("WGR_GEOMETRY_MESH_ACK");
        _geometryMeshAckEnabled = meshAck && std::strcmp(meshAck, "1") == 0;
        const char* registeredRefs = std::getenv("WGR_GEOMETRY_REGISTERED_REFS");
        _geometryRegisteredRefsEnabled = registeredRefs && std::strcmp(registeredRefs, "1") == 0;
    }
    _cpuMeshLink->engine = this; // see CpuMeshLink; nulled in ~EngineWgpu
    s_self = this;
    {
        const char* lw = std::getenv("WGR_LAZY_WINDOW");
        _lazyWindow = !(lw != nullptr && lw[0] == '0');
        LOG_INFO(Graphics, "Wgpu producer window: {} (WGR_LAZY_WINDOW)",
                 _lazyWindow ? "lazy -- opened by the first direct renderer call, not at InitDraw"
                             : "eager -- InitDraw waits for the render thread (REN-THR-012)");
    }
    // Picture Mode depth of field can be armed from the environment as well as from the tab.
    // Same reason the ballistics recorder needed it: a feature reachable only by clicking cannot
    // be captured by a script, attached to a bug report, or checked by the smoke harness -- and
    // an unreachable feature is one nobody can show anybody else.
    //   POSEIDON_DOF=1  POSEIDON_DOF_FOCUS=<m>  POSEIDON_DOF_BLUR=<px>  POSEIDON_DOF_BAND=<m>
    // ON BY DEFAULT. Opt out with WGR_RENDER_THREAD=0, or live in the dev panel's Perf tab.
    //
    // It stayed opt-in while it was being proven, and it is proven: 901 of 901 scene frames
    // byte-identical to the inline path, aligned by simulation tick, across three independent
    // pairs; PERF-018 measured frame mean 43.50 -> 33.91 ms and p95 46.97 -> 36.07 on
    // perf_combat with the per-arm ranges not overlapping. A gain that size sitting behind an
    // environment variable nobody sets is a gain nobody has.
    //
    // WGR_RENDER_OVERLAP is still read, but now only to turn overlap OFF. The thread without
    // the overlap is a stack change that costs a handoff and buys nothing -- it exists so the
    // mechanics can be proven separately, not as a configuration to reach by accident.
    {
        const char* threadEnv = std::getenv("WGR_RENDER_THREAD");
        const bool  threadOn = threadEnv == nullptr || threadEnv[0] == 0 || threadEnv[0] != '0';
        if (threadOn)
        {
            StartRenderThread();
            _renderThreadRequested = true;
            const char* overlapEnv = std::getenv("WGR_RENDER_OVERLAP");
            _overlapRequested = overlapEnv == nullptr || overlapEnv[0] == 0 || overlapEnv[0] != '0';
        }
    }
    if (const char* v = std::getenv("WGR_FRAME_HASH"); v && v[0] && v[0] != '0')
    {
        _frameHashOn = true;
        if (const char* f = std::getenv("WGR_FRAME_HASH_FIRST"); f && f[0])
            _frameHashFirst = static_cast<uint32_t>(std::max(0, std::atoi(f)));
        if (const char* n = std::getenv("WGR_FRAME_HASH_COUNT"); n && n[0])
            _frameHashWindow = static_cast<uint32_t>(std::max(1, std::atoi(n)));
        LOG_INFO(Graphics, "Wgpu frame hash: ON -- every published frame is walked byte for byte "
                           "(REN-THR-001 equivalence oracle); this is not a play setting");
    }
    if (const char* v = std::getenv("POSEIDON_DOF"); v && v[0] && v[0] != '0')
    {
        _dof.enabled = true;
        if (const char* f = std::getenv("POSEIDON_DOF_FOCUS"))
        {
            _dof.focusDistance = float(atof(f));
        }
        if (const char* b = std::getenv("POSEIDON_DOF_BLUR"))
        {
            _dof.maxBlurPixels = float(atof(b));
        }
        if (const char* r = std::getenv("POSEIDON_DOF_BAND"))
        {
            _dof.focusRange = float(atof(r));
        }
        if (const char* d = std::getenv("POSEIDON_DOF_DEBUG"))
        {
            _dof.debugView = atoi(d);
        }
        if (const char* q = std::getenv("POSEIDON_DOF_SAMPLES"))
        {
            _dof.sampleCount = atoi(q);
        }
        if (const char* k = std::getenv("POSEIDON_DOF_BOKEH"))
        {
            _dof.bokehBoost = float(atof(k));
        }
        if (const char* k = std::getenv("POSEIDON_DOF_BLADES"))
        {
            _dof.apertureBlades = float(atof(k));
        }
    }
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

    // DO THE TWO BINARIES COME FROM THE SAME COMMIT?
    //
    // The ABI check below is necessary and NOT sufficient. It compares a hand-maintained ABI
    // version and a struct-layout hash, both of which change only when a struct changes -- so a
    // wgpu_renderer.dll a hundred commits out of date passes it cleanly and simply lacks a
    // hundred commits of fixes. Several worktrees deploy into one game folder and they all share
    // this one DLL, so that is not a hypothetical.
    //
    // It happened on 2026-08-22: a DLL 152 commits behind the installed exe was copied over a
    // matched pair. Nothing failed. Roads stopped going dark at night, house textures came apart,
    // and vegetation went white -- and all three were investigated as fresh rendering regressions,
    // with shader ablations run to explain them, before anyone compared file dates.
    //
    // So this compares the two BUILD IDs directly and says so, loudly, in the game log -- which is
    // the only channel that survives (the launcher discards stderr). A WARNING and not a refusal:
    // building one side while iterating is legitimate and common, and blocking it would just get
    // the check disabled. What is not legitimate is doing it WITHOUT KNOWING, and after this line
    // that is no longer possible.
    // PROVENANCE, recorded rather than enforced.
    //
    // Both binaries carry a build id and NEITHER is reliably fresh: the DLL's is stamped when
    // cargo compiles the cdylib, and cargo skips that on a C++-only change; the exe's is stamped
    // when CMake configures. So "the ids differ" does not mean "the binaries are mismatched", and
    // a warning built on that comparison would fire on ordinary iteration and be ignored within a
    // day -- which is worse than not having it, because it would not be believed on the day it
    // mattered.
    //
    // Enforcement therefore lives in scripts/Deploy.ps1, which can run git: it reads
    // DEPLOYED-FROM.txt and refuses to install a build that is a STRICT ancestor of what is
    // already there. What belongs here is the thing that has to be true of every log, forever:
    // that the two ids are written down, so "which two binaries produced this capture?" is always
    // answerable after the fact.
    //
    // This exists because on 2026-08-22 that question had no answer. A DLL 152 commits behind the
    // installed exe ran for hours; roads stopped going dark at night, textures came apart and
    // vegetation went white, and all three were investigated as fresh rendering regressions --
    // with shader ablations run to explain them -- before anyone compared the file dates.
    LOG_INFO(Graphics, "Wgpu: build provenance — exe {}, wgpu_renderer.dll {} (see DEPLOYED-FROM.txt)",
             BuildInfo::GitSha, wgr_build_id());

    const WgrAbiCheck abiCheck{WGR_ABI_VERSION,
                               sizeof(WgrAbiCheck),
                               sizeof(WgrSurfaceDesc),
                               sizeof(WgrLogCallbacks),
                               sizeof(WgrFrame),
                               WGR_ABI_FEATURE_BUILD_ID | WGR_ABI_FEATURE_SAFE_DIAGNOSTICS |
                                   WGR_ABI_FEATURE_RUNTIME_CAPABILITIES | WGR_ABI_FEATURE_ANALYTIC_GLOW,
                               WgrLayoutHash()};
    const uint32_t runtimeAbi = wgr_abi_version();
    if (runtimeAbi != WGR_ABI_VERSION || !wgr_abi_validate(&abiCheck))
    {
        // Name BOTH layout hashes and the renderer's build id: the pairing this rejects
        // is almost always "the .exe and the .dll were copied from different builds",
        // and those numbers are what identify which half is stale.
        //
        // The renderer's hash is fetched through GetProcAddress, not the import table,
        // for the same reason GetRuntimeCapabilityFlags does it: an older DLL that
        // lacks the export must still reach this diagnostic instead of failing Windows
        // module loading with a bare "Entry Point Not Found".
        uint32_t rendererLayout = 0;
#ifdef _WIN32
        using LayoutHashFn = uint32_t (*)();
        if (const HMODULE module = GetModuleHandleA("wgpu_renderer.dll"))
        {
            if (const auto layoutHash = reinterpret_cast<LayoutHashFn>(GetProcAddress(module, "wgr_abi_layout_hash")))
            {
                rendererLayout = layoutHash();
            }
        }
#endif
        LOG_ERROR(Graphics,
                  "Wgpu: ABI mismatch (engine abi {} layout {:#010x}, renderer abi {} layout {:#010x} build {}); "
                  "refusing renderer startup. The engine binary and wgpu_renderer.dll must come "
                  "from the SAME build — deploy both, not one.",
                  WGR_ABI_VERSION, WgrLayoutHash(), runtimeAbi, rendererLayout, wgr_build_id());
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

    _gpuMemoryProbe.Register(
        "WGPU.Dynamic", 0.35f,
        // REN-THR-013: the worker's snapshot, not the live handle (these run on the producer).
        [] {
            WgrMemoryStats stats{};
            return LastMemoryStats(stats) ? static_cast<size_t>(stats.tracked_bytes) : 0;
        },
        [] {
            WgrMemoryStats stats{};
            return LastMemoryStats(stats) ? static_cast<size_t>(stats.budget_bytes) : 0;
        },
        [] {
            WgrMemoryStats stats{};
            return LastMemoryStats(stats) ? static_cast<size_t>(stats.object_texture_count) : 0;
        });

    _wbank = new TextureBankWgpu(_renderer);

    // RFG-047: BC4/BC5/BC7 go to the GPU as blocks when the adapter can take them.
    //
    // The gate is the adapter's OWN answer (capability bit 0 = TEXTURE_COMPRESSION_BC,
    // lib.rs), not a guess from the vendor string: an adapter without it would refuse the
    // texture and return handle 0, i.e. a white object with no error. When it is absent the
    // switch stays off and every BC4/5/7 file takes the RFG-016 decode exactly as before.
    //
    // Ordered AFTER the bank is constructed and BEFORE any world loads, which is the window
    // in which the choice is free: a texture already uploaded keeps the form it was loaded
    // in, so the dev-panel toggle below is honest only for what is loaded after it.
    // POSEIDON_ENFUSION_COMPRESSED=0 pins it off for a capture that must not have it; the
    // dev panel and graphics.cfg are the supported controls.
    {
        const bool bcCapable = (GetRuntimeCapabilityFlags() & 1u) != 0u;
        const char* env = std::getenv("POSEIDON_ENFUSION_COMPRESSED");
        const bool wanted = (env == nullptr || env[0] == 0) ? _materialDebug.compressedEnfusionTextures
                                                            : (env[0] != '0');
        _materialDebug.compressedEnfusionTextures = wanted;
        SetDdsCompressedPassthrough(bcCapable && wanted);
        LOG_INFO(Graphics, "Wgpu: Enfusion compressed textures (BC7/BC5/BC4) {} (adapter BC support: {})",
                 (bcCapable && wanted) ? "ON" : "OFF", bcCapable ? "yes" : "NO -- decode path forced");
    }

    // MAT-NET-001: the netting depth-write lever. The dev panel is the supported control;
    // this exists because the panel holds the mouse, so an automated A/B capture cannot
    // tick the box -- and "things behind netting look strange" is a claim that has to be
    // shown in two frames from one binary, not argued.
    {
        if (const char* env = std::getenv("POSEIDON_NET_DEPTH"))
        {
            _materialDebug.coverageMaskDepth = (env[0] != '0');
        }
        LOG_INFO(Graphics, "Wgpu: netting drawn as a depth-writing cutout (MAT-NET-001) {}",
                 _materialDebug.coverageMaskDepth ? "ON" : "OFF");
    }

    // Procedural-blade definition strength, WGR_GRASS_CONTRAST. One number scales
    // the whole blade-look group away from the legacy flat wash: 0 is the exact
    // pre-2026-08 look (no self shadow, contrast 1, no hue drift, root 0.70), 1 is
    // the shipped GrassSettings defaults, and up to 2 doubles the deviation. It is
    // read once here as the session's DEFAULT; the Grass-tab sliders edit the same
    // fields afterwards and win, which is the house env-then-slider contract.
    if (const char* contrastEnv = std::getenv("WGR_GRASS_CONTRAST"))
    {
        const float s = std::clamp(std::strtof(contrastEnv, nullptr), 0.0f, 2.0f);
        const GrassSettings stock; // shipped defaults = strength 1
        _grass.bladeSelfShadow = stock.bladeSelfShadow * s;
        _grass.bladeContrast = 1.0f + (stock.bladeContrast - 1.0f) * s;
        _grass.bladeHueVariation = stock.bladeHueVariation * s;
        _grass.bladeRootShade = 0.70f + (stock.bladeRootShade - 0.70f) * s;
        LOG_INFO(Graphics,
                 "Wgpu grass blade definition: WGR_GRASS_CONTRAST={:.2f} -> selfShadow {:.2f}, contrast {:.2f}, "
                 "hueVariation {:.2f}, rootShade {:.2f}",
                 s, _grass.bladeSelfShadow, _grass.bladeContrast, _grass.bladeHueVariation, _grass.bladeRootShade);
    }
    SetGrassSettings(_grass);

    // This backend conforms terrain-clipped geometry (vegetation, roads' visuals) on
    // the GPU, so ClipLand objects publish a conform plane and skip their per-frame CPU
    // vertex deform. GL33 never sets this and keeps deforming on the CPU.
    // WGR_GPU_CONFORM=0 forces the GL33 behaviour (CPU deform, no conform plane). This is
    // the ONLY per-draw code path a road can reach on wgpu and not on GL33, so it is the
    // ablation that separates "the roads look wrong because of GPU conform" from everything
    // else -- and GL33 is known-clean at the same camera. Any value but "0" is on, matching
    // the other WGR_* gates.
    GGpuTerrainConform = true;
    if (const char* gc = std::getenv("WGR_GPU_CONFORM"))
    {
        GGpuTerrainConform = std::strcmp(gc, "0") != 0;
    }
    LOG_INFO(Graphics, "Wgpu: GPU terrain conform: {}", GGpuTerrainConform);

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
    // REN-GI-001: the probe volume. ON by default; WGR_GI=0 is the pre-GI ambient exactly.
    if (const char* gi = std::getenv("WGR_GI"))
    {
        _gi.enabled = std::strcmp(gi, "0") != 0;
    }
    if (const char* giw = std::getenv("WGR_GI_WEIGHT"))
    {
        _gi.weight = std::clamp(std::strtof(giw, nullptr), 0.0f, 1.0f);
    }
    // REN-GI-002: the sun proxy (the coloured first bounce). WGR_GI_RSM=0 leaves stage 1's
    // sky/terrain probes alone, which is the A/B for the bounce on its own.
    if (const char* gir = std::getenv("WGR_GI_RSM"))
    {
        _gi.rsmSamples = std::strcmp(gir, "0") == 0 ? 0 : std::clamp(std::atoi(gir), 4, 64);
    }
    if (const char* gs = std::getenv("WGR_GI_SPACING"))
    {
        _gi.spacing = std::clamp(std::strtof(gs, nullptr), 1.0f, 16.0f);
    }
    if (const char* gm = std::getenv("WGR_GI_INTERIOR_MIX"))
    {
        _gi.interiorMix = std::clamp(std::strtof(gm, nullptr), 0.0f, 1.0f);
    }
    // REN-GI-010: WGR_GI_INDOOR_SKY=0 is the pre-fix probe integration exactly, for the A/B.
    if (const char* gis = std::getenv("WGR_GI_INDOOR_SKY"))
    {
        _gi.indoorSky = std::clamp(std::strtof(gis, nullptr), 0.0f, 1.0f);
    }
    if (const char* gb = std::getenv("WGR_GI_BASIS"))
    {
        // REN-GI-009: `cube` restores the six-face ambient cube exactly, `sh` the first-order
        // version REN-GI-008 measured as a dead end. Default is second order.
        const std::string_view v(gb);
        _gi.basis = (v == "cube" || v == "0") ? 0 : (v == "sh" || v == "sh1" || v == "1") ? 1 : 2;
    }
    if (const char* gg = std::getenv("WGR_GI_RSM_GAIN"))
    {
        _gi.rsmGain = std::max(0.0f, std::strtof(gg, nullptr)); // A/B: is the bounce small or zero?
    }
    LOG_INFO(Graphics,
             "Wgpu GI probe volume: {} (WGR_GI), weight {:.2f}, spacing {:.1f} m, {} rays; sun proxy {} "
             "(WGR_GI_RSM, {} samples, radius {:.0f} m, gain {:.2f}); basis {}; indoor sky occlusion {:.2f}",
             _gi.enabled ? "on" : "OFF", _gi.weight, _gi.spacing, _gi.rays,
             _gi.rsmSamples > 0 ? "on" : "OFF", _gi.rsmSamples, _gi.rsmRadius, _gi.rsmGain,
             _gi.basis == 0 ? "ambient cube" : (_gi.basis == 1 ? "SH-1" : "SH-2"), _gi.indoorSky);
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

    if (const char* value = std::getenv("WGR_CONTACT_SHADOWS"))
        _smTuning.contactShadows = static_cast<int>(std::clamp(std::strtol(value, nullptr, 10), 0L, 2L));
    if (const char* value = std::getenv("WGR_SUN_ANGULAR_RADIUS")) {
        const float radius = std::strtof(value, nullptr);
        if (std::isfinite(radius)) _smTuning.sunAngularRadius = std::clamp(radius, 0.0f, 8.0f);
    }
    LOG_INFO(Graphics, "CSM contact shadows: mode={} sun angular radius={} degrees (prototype)",
             _smTuning.contactShadows, _smTuning.sunAngularRadius);

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
        if (std::strcmp(view, "full") == 0)
            _materialDebug.view = MaterialDebugSettings::View::FullMaterial;
        else if (std::strcmp(view, "base") == 0)
            _materialDebug.view = MaterialDebugSettings::View::BaseColorOnly;
        else if (std::strcmp(view, "normal") == 0)
            _materialDebug.view = MaterialDebugSettings::View::NormalMap;
        else if (std::strcmp(view, "normalviz") == 0)
            _materialDebug.view = MaterialDebugSettings::View::NormalMap;
        else if (std::strcmp(view, "ao") == 0 || std::strcmp(view, "ambient") == 0)
            _materialDebug.view = MaterialDebugSettings::View::AmbientShadow;
        else if (std::strcmp(view, "specular") == 0 || std::strcmp(view, "gloss") == 0)
            _materialDebug.view = MaterialDebugSettings::View::SpecularGloss;
        else if (std::strcmp(view, "uv") == 0)
            _materialDebug.view = MaterialDebugSettings::View::UvSource;
        else if (std::strcmp(view, "lit") == 0)
            _materialDebug.view = MaterialDebugSettings::View::LightingOnly;
        LOG_INFO(Graphics, "Wgpu: material debug view={}", view);
    }
    if (const char* disableNormal = std::getenv("WGR_MATERIAL_DISABLE_NORMAL"))
        _materialDebug.disableNormalMap = std::strcmp(disableNormal, "0") != 0;

    if (const char* invertNormalY = std::getenv("WGR_MATERIAL_INVERT_NORMAL_Y"))
        _materialDebug.invertNormalY = std::strcmp(invertNormalY, "0") != 0;
    // DZ-007 -- the same field the dev panel's "Disable Fresnel / Environment" checkbox
    // writes, settable from a capture so the field-to-pixel path is EVIDENCE rather than an
    // assertion. It is the identical `_materialDebug` member and the identical
    // PushMaterialDebug upload; the checkbox differs only in writing it mid-run, which the
    // per-fragment read out of the frame UBO is designed for.
    if (const char* disableFresnelEnv = std::getenv("WGR_MATERIAL_DISABLE_FRESNEL_ENV"))
        _materialDebug.disableFresnelEnvironment = std::strcmp(disableFresnelEnv, "0") != 0;
    // Same field as the panel's "Disable SMDI / Specular-Gloss", for a capture A/B.
    if (const char* disableSpecular = std::getenv("WGR_MATERIAL_DISABLE_SPECULAR"))
        _materialDebug.disableSpecularGloss = std::strcmp(disableSpecular, "0") != 0;
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
    // BLOOM A/B levers. Unlike the grade fields above these do NOT switch the per-ToD auto
    // mode off, because bloom is deliberately carried across the auto overwrite
    // (UpdateAutoTonemap, :5651-5658) — so setting one here holds for the whole run while the
    // time-of-day grade keeps interpolating normally. Unset changes nothing.
    //
    // They exist for the "white orb" report: a compact blown-out disc with a wide soft halo has
    // three possible authors and ONE capture separates them. `WGR_BLOOM_INTENSITY=0` drops the
    // pyramid out of the resolve (tonemap.wgsl:118, `color += bloom * bloom_intensity`) without
    // touching the scene the pyramid was built from, so:
    //   * halo gone, a small hard-edged bright patch left -> a SURFACE/GEOMETRY blow-out. The
    //     residual is the emitter's true screen size, which names the object (a 5 m prop and a
    //     0.3 m lamp lens are not confusable once bloom is off).
    //   * the orb disappears entirely -> a modest source that only bloom makes conspicuous;
    //     the fix is threshold/knee, not the source.
    //   * the orb is unchanged -> it is drawn AFTER the resolve (a 2D / flare sprite), which no
    //     HDR-side lever can reach at all.
    // The default 1.0 threshold is scene-referred, and sunlit ground already sits near
    // sunIntensity(22) x transmittance, so almost every lit surface clears it; raising
    // WGR_BLOOM_THRESHOLD is how you ask "is this source actually brighter than the terrain?".
    if (const char* bi = std::getenv("WGR_BLOOM_INTENSITY"))
    {
        _tonemap.bloomIntensity = std::max(0.0f, std::strtof(bi, nullptr));
    }
    if (const char* bt = std::getenv("WGR_BLOOM_THRESHOLD"))
    {
        _tonemap.bloomThreshold = std::max(0.0f, std::strtof(bt, nullptr));
    }
    if (const char* bk = std::getenv("WGR_BLOOM_KNEE"))
    {
        _tonemap.bloomKnee = std::max(1.0e-4f, std::strtof(bk, nullptr));
    }
    // AUTO-EXPOSURE (eye adaptation) levers. The chain is complete and has been since the HDR
    // pass landed — luminance pyramid -> fs_adapt -> exposure scale (exposure.wgsl /
    // exposure.rs). It is `enabled = false` by DELIBERATE default (Engine.hpp:1758-1762: its 4x
    // range "can flash the whole scene when a player turns from dark terrain toward a bright sky
    // or water glint" — someone tried it and backed it out), and until now the ONLY way to switch
    // it on was the ImGui Tonemap tab, i.e. a human at the keyboard. That made it impossible to
    // A/B from a capture harness at all. See RND-039.
    //
    // These levers deliberately change NO default: an unset variable leaves ExposureSettings
    // exactly as constructed. They exist so the pumping the original author feared can be
    // MEASURED rather than argued about.
    //
    // WGR_EXPOSURE (read above) is a DIFFERENT knob and the two are routinely confused: it is a
    // manual pre-curve multiplier that also drops the per-ToD auto grade. Auto-exposure is a
    // multiplicative scale ON TOP of whatever exposure the tonemap ends up with, so the two
    // compose (manual base x adaptive scale) rather than override each other.
    if (const char* ae = std::getenv("WGR_AUTO_EXPOSURE"))
    {
        _exposure.enabled = std::strtol(ae, nullptr, 10) != 0;
    }
    if (const char* k = std::getenv("WGR_AUTO_EXPOSURE_KEY"))
    {
        const float v = std::strtof(k, nullptr);
        if (v > 0.0f)
            _exposure.key = v;
    }
    if (const char* mn = std::getenv("WGR_AUTO_EXPOSURE_MIN"))
    {
        const float v = std::strtof(mn, nullptr);
        if (v > 0.0f)
            _exposure.minScale = v;
    }
    if (const char* mx = std::getenv("WGR_AUTO_EXPOSURE_MAX"))
    {
        const float v = std::strtof(mx, nullptr);
        if (v > 0.0f)
            _exposure.maxScale = v;
    }
    if (const char* r = std::getenv("WGR_AUTO_EXPOSURE_RATE"))
    {
        // Per-FRAME ease, as the field is documented. Clamped into (0,1]: 0 would freeze
        // adaptation at its initial scale and read as "auto-exposure does nothing".
        _exposure.rate = std::clamp(std::strtof(r, nullptr), 1.0e-4f, 1.0f);
    }
    if (const char* sw = std::getenv("WGR_AUTO_EXPOSURE_SKYWEIGHT"))
    {
        // 1.0 = uniform metering. The 0.3 default DOWN-weights the top of the frame so a bright
        // sky cannot over-darken the ground (exposure.wgsl meter_weight) — which is exactly why
        // "look at the sun and the iris closes" does not happen today. Raise this to test that,
        // and expect it to also darken on a plain look-up at empty sky: uv.y is a screen-space
        // proxy for the sun, not the sun. RND-039 §"What would actually satisfy it".
        const float v = std::strtof(sw, nullptr);
        if (v >= 0.0f)
            _exposure.skyWeight = v;
    }
    if (const char* tau = std::getenv("WGR_AUTO_EXPOSURE_TAU"))
    {
        // An OVERRIDE of the authored `ExposureSettings::rateTau`, which is now the default
        // source (0.4 s) because auto-exposure itself is default-on as of 2026-08-16. The
        // time-constant form is what makes that safe to ship: `rate` as authored is
        // per-FRAME, so adaptation runs more than twice as fast on a 45 fps world as on a
        // 20 fps one, and a default-on feature tuned that way is tuned against whichever
        // world it happened to be measured on (RND-039).
        //
        // `WGR_AUTO_EXPOSURE_TAU=0` is a real setting, not a way of spelling "unset": it
        // selects the legacy per-frame `rate` so the two eases can be captured against each
        // other without a rebuild. Hence a NEGATIVE unset sentinel.
        const float v = std::strtof(tau, nullptr);
        _autoExposureTau = v > 0.0f ? v : 0.0f;
    }
    PushRenderParams();
    PushSkyRuntime();
    // Once at startup so the WGR_MATERIAL_* env overrides read above reach the
    // retained path; after this only a Materials-tab change pushes it.
    PushMaterialDebug();

    Dev::DebugOverlay::InitForEngine(_window);
    _eventWindow.Attach(_window, _w, _h);

    // The parked-model LRU (WGR_GPU_MODEL_PARK) keys registrations by shape past the last live
    // Object. That is only safe if a dying shape tells us: without this, a shape the streamer's
    // evict cache dropped would leave its address in the maps and a later shape allocated at
    // the same address would "unpark" a stranger's meshes.
    LODShape::SetDestroyListener(
        [](const LODShape* shape, void* self) { static_cast<EngineWgpu*>(self)->OnShapeDestroyed(shape); }, this);
}

struct EngineWgpu::SharedMeshFixtureState
{
    // Buffer ownership changes on the main owner only. The renderer sees only
    // numeric request fields and writes a small copied report under this mutex.
    std::mutex mutex;
    std::unique_ptr<VertexBuffer> older, newer;
    std::shared_ptr<render::SharedRetainedMeshLifetime> lifetime;
    std::shared_ptr<render::StandaloneMeshRetirementWitness> standaloneWitness;
    bool standalone = false;
    SharedMeshFixtureReport report;
    uint64_t producer = 0, knownBytes = 0;
    bool producerReleased = false;
};

EngineWgpu::~EngineWgpu()
{
    StopRenderThread();
    _sharedMeshFixture.reset();
    // Serializes against late shared-buffer destroy enqueues before wgr_destroy.
    if (_sharedMeshEpoch) _sharedMeshEpoch->Close();
    if (_geometryOwnerLedger) _geometryOwnerLedger->CloseEpoch(_geometryOwnerEpoch);
    ClearFarObjects();
    s_self = nullptr;
    // FIRST: every VertexBufferWgpu still alive holds this link, and Shapes outlive the
    // engine on some teardown orders. Nulling it here turns a late SetSkinData into a no-op
    // instead of a call through a destroyed engine (REN-THR-011).
    _cpuMeshLink->engine = nullptr;
    LODShape::SetDestroyListener(nullptr, nullptr);
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
        // The worker must be gone before anything the renderer owns is torn down: it is the
        // only other thread that touches the handle.
        StopRenderThread();
        // Release the retained scene's owned pool meshes before tearing down the renderer.
        // _gpuMeshes holds PRODUCER handles (REN-THR-008); translate through the drain map.
        // A handle with no mapping is a create that never drained -- it never reached the
        // renderer, so there is nothing to destroy.
        for (uint64_t mesh : _gpuMeshes)
        {
            if (const auto it = _meshIdOf.find(mesh); it != _meshIdOf.end())
            {
                _ops.meshDestroy++;
                wgr_mesh_destroy(_renderer, it->second);
            }
        }
        _gpuMeshes.clear();
        _meshIdOf.clear();
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
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)
    // POSEIDON_VSYNC=<0|1|-1> pins the interval for a benchmark run without touching the
    // owner's saved setting. Every CPU-frame number taken under Fifo on a 144 Hz display is
    // quantised to 6.9 ms steps (a 19 ms GPU frame reads as 20.8 or 27.8), which is how the
    // REN-THR-013 A/B first measured nothing: the difference hid in the quantisation.
    if (const char* pin = std::getenv("POSEIDON_VSYNC"); pin != nullptr && pin[0] != 0)
    {
        const int pinned = std::atoi(pin);
        if (pinned != interval)
            LOG_INFO(Graphics, "Wgpu: VSync interval {} requested, pinned to {} by POSEIDON_VSYNC", interval, pinned);
        interval = pinned;
    }
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

// LGT-010: which local light views this caster belongs in. A sphere-vs-point test against
// each shadowing light's reach -- deliberately generous (it ignores the cone, so a caster
// beside the beam is still submitted) because the cost of a false positive is one clipped
// draw and the cost of a false negative is a missing shadow, which is a visible bug.
float EngineWgpu::LocalShadowReachFor(int k) const
{
    static const bool matchLight = [] {
        const char* value = std::getenv("WGR_MATCH_LOCAL_SHADOW_REACH");
        return !value || std::atoi(value) != 0;
    }();
    const auto& settings = Poseidon::Dev::GLocalShadowSettings();
    const auto& light = _localShadowLights[k];
    return Poseidon::shadow::LocalShadowReach(light.startAtten, light.endAttenScale,
                                            settings.rangeScale, settings.rangeMax, matchLight);
}

Poseidon::shadow::Mat4 EngineWgpu::LocalShadowVP(int k, const float camPos[3]) const
{
    const Poseidon::Dev::LocalShadowSettings& ls = Poseidon::Dev::GLocalShadowSettings();
    const LocalShadowLight& L = _localShadowLights[k];
    // The light's own reach. Past it the light contributes nothing, so rendering casters
    // further away spends depth on invisible geometry.
    const float reach = LocalShadowReachFor(k);
    const Poseidon::shadow::Vec3 pos{L.pos[0], L.pos[1], L.pos[2]};
    const Poseidon::shadow::Vec3 cam{camPos[0], camPos[1], camPos[2]};
    if (L.cubeFace >= 0)
    {
        return Poseidon::shadow::PointFaceVP(pos, L.cubeFace, std::max(ls.nearD, 0.01f), reach, cam);
    }
    // A cone angle of 0 means the source never told us one; fall back to something a
    // lamp-shaped light plausibly has rather than to a pinhole, which would clip its own
    // pool away.
    const float cone = L.coneOuter > 0.01f ? L.coneOuter : 0.45f;
    return Poseidon::shadow::SpotLightVP(pos, {L.dir[0], L.dir[1], L.dir[2]}, cone, std::max(ls.nearD, 0.01f), reach,
                                         cam);
}

// LGT-026: the identity of view k's light, as one number. Quantised before hashing -- a
// light's position arrives through a chain of float arithmetic and the last bits of it move
// on their own; a key that never matches is a cache that never caches, and it reports itself
// as working. A centimetre and a milliradian are both far below what a shadow map at these
// texel densities resolves.
uint64_t EngineWgpu::LocalShadowLightKey(int k) const
{
    const Poseidon::Dev::LocalShadowSettings& ls = Poseidon::Dev::GLocalShadowSettings();
    const LocalShadowLight& L = _localShadowLights[k];
    auto mix = [](uint64_t h, uint64_t v) -> uint64_t { return (h ^ v) * 0x00000100000001B3ULL; };
    auto q = [](float v, float scale)
    { return static_cast<uint64_t>(static_cast<int64_t>(std::lround(static_cast<double>(v) * scale))); };
    uint64_t h = 0xcbf29ce484222325ULL;
    h = mix(h, q(L.pos[0], 100.0f));
    h = mix(h, q(L.pos[1], 100.0f));
    h = mix(h, q(L.pos[2], 100.0f));
    h = mix(h, q(L.dir[0], 1000.0f));
    h = mix(h, q(L.dir[1], 1000.0f));
    h = mix(h, q(L.dir[2], 1000.0f));
    h = mix(h, q(L.coneOuter, 1000.0f));
    h = mix(h, q(L.startAtten, 100.0f));
    h = mix(h, q(LocalShadowReachFor(k), 100.0f));
    h = mix(h, static_cast<uint64_t>(L.cubeFace + 1));
    // The settings the matrix is BUILT from. Change the range or the near plane in the Lighting
    // tab and every cached tile is wrong; without this the sliders would appear to do nothing.
    h = mix(h, q(ls.nearD, 1000.0f));
    h = mix(h, q(ls.rangeScale, 1000.0f));
    h = mix(h, q(ls.rangeMax, 100.0f));
    h = mix(h, q(ls.selfRadius, 1000.0f));
    // Never 0: 0 is the "slot was empty last frame" sentinel.
    return h | 1ULL;
}

// NV-001: force the goggles on without a player wearing any. Read once -- this is a capture
// and debugging lever, not a gameplay path.
bool EngineWgpu::NightVisionForced()
{
    static const bool forced = [] {
        const char* e = std::getenv("POSEIDON_NV_FORCE");
        return e && std::atoi(e) != 0;
    }();
    return forced;
}

uint32_t EngineWgpu::LocalShadowCasterMask(float relX, float relY, float relZ) const
{
    if (_localShadowCount <= 0)
    {
        return 0u;
    }
    const Poseidon::Dev::LocalShadowSettings& ls = Poseidon::Dev::GLocalShadowSettings();
    const int firstBit = _smCascades.count;
    uint32_t mask = 0u;
    for (int k = 0; k < _localShadowCount && k < kMaxLocalShadows; k++)
    {
        const LocalShadowLight& L = _localShadowLights[k];
        const float reach = LocalShadowReachFor(k);
        const float dx = (relX + _smCamPos[0]) - L.pos[0];
        const float dy = (relY + _smCamPos[1]) - L.pos[1];
        const float dz = (relZ + _smCamPos[2]) - L.pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        // LGT-016: too close is as disqualifying as too far. See LocalShadowSettings.
        const float self2 = ls.selfRadius * ls.selfRadius;
        if (d2 <= reach * reach && d2 >= self2)
        {
            mask |= 1u << (firstBit + k);
        }
    }
    return mask;
}

void EngineWgpu::ResizeSurface(int w, int h)
{
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)
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
    // LGT-027: a light sprite ADDS. This case was missing, and it is the reason a bulb could
    // not be made to glow by any amount of tuning: SrcAlpha/OneMinusSrcAlpha converges on the
    // sprite's own colour, so a grey billboard drawn over a night scene reads as fog and
    // drawing it larger reads as a bigger patch of fog -- which is exactly what the owner and
    // I kept seeing (the LGT-023 saucer, and again at 0.45 m here).
    //
    // The engine already SAYS so: BuildRenderPassDescriptor gives Backend::IsLight
    // BlendMode::Additive, and GL33 consumes that through PassId::Light. The wgpu path never
    // read it -- BlendForSpec only ever asked about the three alpha bits. One missing case,
    // and it made an entire class of effect impossible on this backend.
    if (render::Has(b, render::Backend::IsLight))
    {
        return WGR_BLEND_ADDITIVE;
    }
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

// MAT-NET-001: is this Blend-classified histogram actually a punch-through COVERAGE MASK
// whose edges were antialiased (and, in ARGB4444, quantised to 16 alpha levels) rather than
// a genuinely translucent surface?
//
// Two conditions, and they are the mirror image of the MAT-051 glass rule (pctClear < 5 and
// pctPartial > 80): a mask has real HOLES -- a substantial share of texels at exactly a=0 --
// and its partial texels are a MINORITY, because they are the perimeter of those holes and
// not the body of the surface. Glass, smoke, halos and material-driven fades fail both.
//
// Thresholds against the measured stock CWA corpus (Dta/Data.pbo, PoseidonTools image
// inspect). Masks, all accepted: pletivo_opak 88.0/10.6, pletivo_opak_dark32 81.1/15.3,
// barbedwire_plot 85.1/8.7, barbedwire 76.5/21.7, barbedwire2 72.1/26.1, drat_svetlo_s
// 57.7/21.8, m1a1_dratram_side 73.2/14.4, m1a1_dratram_back 54.6/14.0, maskovaci_sit_new
// 39.9/32.7 (clear% / partial%). Rejected, and correctly: uh60_skla_ca canopy glass
// (<5 clear, 99 partial) and mask_sit_2x1_ridka.paa (15.4/49.3) -- the latter is a real
// mask but half its area is partial, so it stays on the conservative side of the line
// rather than widening the rule until glass falls in.
bool IsCoverageMaskAlpha(const Poseidon::AlphaStats& st)
{
    return st.pctClear >= 25.0 && st.pctPartial <= 40.0;
}

// True when nothing in the material can make the surface see-through, so any
// translucency would have to come from the texture. A fade driven by material
// alpha over an opaque texture is real and must keep its blend.
bool MaterialIsFullyOpaque(const TLMaterial& material)
{
    constexpr float kOpaque = 1.0f - 1.0f / 255.0f;
    // `forcedDiffuse` is deliberately NOT tested, and testing it was the bug.
    //
    // It is an ADDITIVE lighting term, not an opacity: Real Virtuality's own default is
    // `forcedDiffuse[]={0,0,0,0}` (see MakeSuperRvmatText, which writes exactly that beside
    // `ambient`/`diffuse` of {1,1,1,1}). So alpha 0 is the ORDINARY value for it, and requiring
    // alpha 1 made this guard answer "not opaque" for essentially every stock material -- which
    // means the override it protects, the one that stops a solid surface being soft-blended
    // against the framebuffer, almost never fired.
    //
    // Measured on the OFP M16 in the player's hands, which is where the owner reported it: four
    // sections, all four with an Opaque-classified texture and dif/amb/emi alpha 1.000. The two
    // that draw ALPHA-BLENDED have forcedDiffuse.A()=0.000; the two that draw opaque have 1.000.
    // Nothing else differs. Pointed at the sky the blended pair let it through, which is the
    // see-through rifle.
    //
    // The case the guard exists to protect is untouched: a genuine fade is driven by the
    // DIFFUSE/AMBIENT alpha (ObjectColor, setObjectTexture), and those are still required to be
    // opaque here, so material-driven translucency keeps its blend.
    return material.diffuse.A() >= kOpaque && material.ambient.A() >= kOpaque && material.emmisive.A() >= kOpaque;
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

// WGR_SHADOW_LEGACY_GATE=1 restores the pre-fix cascade behaviour EXACTLY: the old
// `!_shadowCasters.empty()` gate in NextFrame AND the old write-only-in-AddShadowCaster
// handling of `_smCamPos`. Both, from one switch, because they are one behaviour: the old
// gate is what made the stale `_smCamPos` harmless (see SetShadowCascades), so restoring
// either alone would A/B a state that never shipped. The FIXED behaviour is the default.
bool ShadowLegacyGateEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SHADOW_LEGACY_GATE");
        return v && v[0] != '0';
    }();
    return on;
}

// WGR_SHADOW_GATE_TRACE=1 — log the cascade gate's inputs whenever its verdict flips. Separate
// from the legacy switch on purpose: the trace has to be usable on BOTH arms of the A/B, so
// folding it into WGR_SHADOW_LEGACY_GATE would leave the arm that reproduces the bug silent.
bool ShadowGateTraceEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SHADOW_GATE_TRACE");
        return v && v[0] != '0';
    }();
    return on;
}

// WGR_SHADOW_FORCE_NO_CPU_CASTERS=1 — diagnostic; see the gate in NextFrame for why it exists.
// It empties the CPU caster list to force the exact condition the fix targets, because no natural
// exterior pose produced it. Diagnostic only: it discards real casters.
bool ShadowForceNoCpuCastersEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SHADOW_FORCE_NO_CPU_CASTERS");
        return v && v[0] != '0';
    }();
    return on;
}

} // namespace

void EngineWgpu::InitDraw(bool clear, PackedColor color)
{
    // The producer's renderer-touching window opens here and closes at publish. Everything
    // between the two may call the renderer directly; nothing before it may (REN-THR-012).
    // REN-THR-013: with the lazy window the traversal overlaps the worker until something
    // actually needs the renderer handle (AcquireProducerWindow); WGR_LAZY_WINDOW=0 is the
    // REN-THR-012 behaviour, the whole traversal after the worker.
    if (!_lazyWindow)
        WaitRenderIdle();
    // RenderSnapshot S1 / Phase 5 step 1: the WORLD publishes the frame's environment
    // block at its simulation/draw boundary (World::Simulate, right before it calls
    // InitDraw). This is only the FALLBACK for frames no world drives (main menu
    // without a world, tools) — it captures solely when nothing has since the last
    // FinishDraw, so a dead world's values never leak into a later frame.
    render::frame::CapturePresentationSnapshotIfStale();
    ++_cpuPoseFrame;
    _currentCpuPose = nullptr;
    _currentCpuPoseSource = nullptr;
    _verts.clear();
    _batches.clear();
    _draws3d.clear();
    // Indices into _draws3d; they must die with it or they patch a stranger (REN-THR-011).
    _draws3dPendingMesh.clear();
    _cmds.clear();
    _palette.clear();
    _cameras.clear();
    _currentCamera = 0;
    _haveCamera = false;
    _swMesh = nullptr;
    _shadowCasters.clear();
    _castersPendingMesh.clear(); // indices into it (REN-THR-011)
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
        _wbank->UpdateAdaptiveTextureDetail();
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
    // POSEIDON_SNAPSHOT_VALIDATE=1: end-of-draw is the far edge of the interval the
    // snapshot promises to be constant over; the re-read happens here, after every
    // consumer has run. No-op unless the env var is set.
    render::frame::ValidatePresentationSnapshot();
    // The frame's snapshot is consumed; next frame re-captures (world-side, or the
    // InitDraw fallback above).
    render::frame::RetirePresentationSnapshot();
    if (_wbank)
    {
        _wbank->FinishFrame();
    }
    // Engine::FinishDraw drives the frame counter (+timing) the test harness
    // readiness checks poll.
    Engine::FinishDraw();
}

// How many times the immediate per-section path actually runs in a frame. Whether hoisting the
// per-section std::getenv() out of DrawSectionTL is worth anything depends entirely on this
// number, and nothing reported it. WGR_DRAW_SECTION_STATS=1 emits one row per frame.
static long long GDrawSectionCalls = 0;
static bool DrawSectionStatsEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_DRAW_SECTION_STATS");
        return v && std::strcmp(v, "0") != 0;
    }();
    return on;
}
static void LogDrawSectionStats()
{
    if (!DrawSectionStatsEnabled())
    {
        return;
    }
    LOG_INFO(Graphics, "DRAW_SECTION_STATS frame drawSectionTL={}", GDrawSectionCalls);
    GDrawSectionCalls = 0;
}

// REN-THR-006: the settings drain. Everything the producer latched since the last frame is
// pushed here, at the one point per frame where the render thread's ownership of the handle
// will begin. Order between entries is irrelevant by construction -- each writes disjoint
// renderer state -- and order within an entry is last-write-wins, which is what back-to-back
// direct calls already meant.
// REN-THR-016: takes the FRAME's queues, never the members. With overlap the producer is
// already enqueueing the next frame into the members while this runs -- the second overlap
// crash was exactly that, inside DrainResourceOps hashing a map the producer was growing.
void EngineWgpu::ApplyPendingRendererSettings(PendingRendererSettings& p)
{
    if (!_renderer)
        return;
    if (p.suppressWorldDirty)
    {
        wgr_set_suppress_world_objects(_renderer, p.suppressWorld);
        p.suppressWorldDirty = false;
    }
    if (p.cullDebugDirty)
    {
        wgr_set_cull_debug(_renderer, p.cullDrawSpheres, p.cullDisableFrustum, p.cullOcclusion);
        p.cullDebugDirty = false;
    }
    if (p.temporalDirty)
    {
        wgr_set_temporal_tuning(_renderer, &p.temporal);
        p.temporalDirty = false;
    }
    if (p.grassSpacingDirty)
    {
        wgr_grass_set_card_spacing(_renderer, p.grassCardSpacing);
        p.grassSpacingDirty = false;
    }
    if (p.smokeVolumeDirty)
    {
        // SMK-038. Params first, then the particles: with mode 0 the renderer drops both on
        // the floor, so the order only matters for readability.
        wgr_set_smoke_volume(_renderer, &p.smokeVolume);
        wgr_set_smoke_volume_blobs(_renderer, p.smokeVolumeBlobs.empty() ? nullptr : p.smokeVolumeBlobs.data(),
                                   static_cast<uint32_t>(p.smokeVolumeBlobs.size()));
        p.smokeVolumeDirty = false;
        // Read the counters straight back: they are the only thing that separates
        // "selected but no smoke reached the field" from "selected and marching".
        const uint64_t stats = wgr_get_smoke_volume_stats(_renderer);
        SmokeVolumetricParams& vol = GSmokeVolumetric();
        vol.blobs = static_cast<long long>(stats & 0xffffffffu);
        vol.framesMarched = static_cast<long long>(stats >> 32);
    }
    {
        // SMK-037: the soft-particle lever, read straight off GSmokeSoftParticles rather than
        // routed through PendingRendererSettings -- growing that struct is a wider change than
        // two scalars need, and a torn read of a bool and a float here costs one frame of a
        // slider drag. Pushed only when they move, so a settled panel calls nothing.
        const SmokeSoftParticles& soft = GSmokeSoftParticles();
        static int lastSoftEnabled = -1;
        static float lastSoftFade = -1.0f;
        const int wantSoft = soft.enabled ? 1 : 0;
        if (wantSoft != lastSoftEnabled || soft.fade != lastSoftFade)
        {
            lastSoftEnabled = wantSoft;
            lastSoftFade = soft.fade;
            wgr_set_soft_particles(_renderer, static_cast<uint32_t>(wantSoft), soft.fade);
        }
    }
    {
        // RFG-090: a Reforger clutter atlas is authored albedo; the photo-plate tone
        // normalisation bleached it (measured: a field of white nettles at luma 0.63).
        const bool enfusionCards = _terrain != nullptr && _terrain->GrassHasMapClutterAtlas();
        static int lastEnfusionCards = -1;
        if (static_cast<int>(enfusionCards) != lastEnfusionCards)
        {
            lastEnfusionCards = static_cast<int>(enfusionCards);
            wgr_grass_set_card_tone_enabled(_renderer, enfusionCards ? 0u : 1u);
            LOG_INFO(Graphics, "Wgpu grass cards: photo tone normalisation {} ({})", enfusionCards ? "OFF" : "on",
                     enfusionCards ? "Reforger clutter atlas is authored albedo" : "photo plates");
        }
    }
    if (p.grassParamsDirty)
    {
        wgr_grass_set_params(_renderer, &p.grassParams);
        p.grassParamsDirty = false;
    }
    if (p.cullParamsDirty)
    {
        wgr_set_cull_params(_renderer, p.cullDrawDistance, p.cullCamLeft, p.cullLodInvWidth, p.cullPixelLimit);
        p.cullParamsDirty = false;
    }
    if (p.farParamsDirty)
    {
        wgr_far_set_params(_renderer, p.farNearCutoff, p.farDistance, p.farPixelLimit, p.farEnabled);
        p.farParamsDirty = false;
    }
    // REN-THR-013
    if (p.renderParamsDirty)
    {
        wgr_set_render_params(_renderer, &p.renderParams);
        p.renderParamsDirty = false;
    }
    if (p.skyExtrasDirty)
    {
        // Frame-owned copy: the producer may already be editing the next frame.
        const SkySettings& s = p.skyExtras;
        wgr_set_cirrus_mode(_renderer, s.cirrusEnabled ? (s.cirrusVolumetric ? 2u : 1u) : 0u);
        wgr_set_cirrus_puffiness(_renderer, s.cirrusPuffiness, s.cirrusPuffVariation);
        wgr_set_cirrus_look(_renderer, s.cirrusAmount, s.cirrusMatchDeck);
        wgr_set_cirrus_softness(_renderer, s.cirrusSoftness);
        wgr_set_fog_far_close(_renderer, s.fogFarClose);
        wgr_set_god_rays(_renderer, s.godRays ? 1u : 0u, s.godRayIntensity, s.godRayDensity,
                         s.godRayDistance, s.godRayG, s.godRayCloudInfluence,
                         static_cast<uint32_t>(std::max(s.godRaySteps, 0)),
                         static_cast<uint32_t>(std::max(s.godRayResDiv, 0)));
        p.skyExtrasDirty = false;
        if (std::getenv("WGR_SETTINGS_QUEUE_TRACE"))
            LOG_INFO(Graphics, "Settings queue applied: sky cirrus={} rays={} intensity={} fogClose={} softness={}",
                     s.cirrusEnabled, s.godRays, s.godRayIntensity, s.fogFarClose, s.cirrusSoftness);
    }
    if (p.roadConformDirty)
    {
        wgr_set_road_conform(_renderer, p.roadConform.liftFlat, p.roadConform.liftPerMetre,
                             p.roadConform.liftMaxFrac);
        p.roadConformDirty = false;
        if (std::getenv("WGR_SETTINGS_QUEUE_TRACE"))
            LOG_INFO(Graphics, "Settings queue applied: road lift={}", p.roadConform.liftFlat);
    }
    if (p.skyRuntimeDirty)
    {
        wgr_set_sky_runtime(_renderer, &p.skyRuntime);
        p.skyRuntimeDirty = false;
    }
    if (p.terrainParamsDirty)
    {
        wgr_terrain_set_params(_renderer, &p.terrainParams);
        p.terrainParamsDirty = false;
    }
    if (!p.snowData.empty())
    {
        wgr_terrain_set_snow(_renderer, p.snowData.data(), static_cast<uint32_t>(p.snowData.size()));
        p.snowData.clear();
    }
    if (p.smokeDirty)
    {
        wgr_set_smoke_shadow(_renderer, p.smokeBlobs.empty() ? nullptr : p.smokeBlobs.data(),
                             U32(p.smokeBlobs.size()), p.smokeStrength);
        p.smokeDirty = false;
    }
    if (p.skyScalarsDirty)
    {
        wgr_set_cloud_shadow_strength(_renderer, p.cloudShadowStrength);
        wgr_set_star_intensity(_renderer, p.starIntensity);
        wgr_set_lens_flare(_renderer, p.lensFlare);
        p.skyScalarsDirty = false;
    }
    {
        // RFG-085: the sky-bake gate follows the Streaming tab's levers. Pushed only when a
        // value changed, so the steady state costs one comparison per frame.
        const Poseidon::Dev::ResidencyLevers& levers = Poseidon::Dev::GResidencyLevers();
        static float lastMinEnclosed = -1.0f;
        static int   lastBudgetMb = -1;
        if (levers.skyBakeMinEnclosed != lastMinEnclosed || levers.skyVolumeBudgetMb != lastBudgetMb)
        {
            lastMinEnclosed = levers.skyBakeMinEnclosed;
            lastBudgetMb = levers.skyVolumeBudgetMb;
            wgr_set_sky_bake_gate(_renderer, lastMinEnclosed, static_cast<uint32_t>(std::max(lastBudgetMb, 0)));
        }
    }
    // PERF-022
    if (p.planarPadDirty)
    {
        wgr_set_planar_reflection_pad(_renderer, p.planarReflectionPad);
        p.planarPadDirty = false;
    }
    if (p.waterParamsDirty)
    {
        wgr_water_set_params(_renderer, &p.waterParams);
        p.waterParamsDirty = false;
    }
    if (p.waterInteractionDirty)
    {
        wgr_water_set_interaction_params(_renderer, &p.waterInteraction);
        p.waterInteractionDirty = false;
    }
    if (!p.waterEvents.empty())
    {
        wgr_water_submit_interactions(_renderer, p.waterEvents.data(), U32(p.waterEvents.size()));
        p.waterEvents.clear();
    }
}

uint32_t EngineWgpu::EnqueueInstanceAdd(const WgrInstance& inst)
{
    uint32_t handle;
    if (!_instFreeHandles.empty())
    {
        handle = _instFreeHandles.back();
        _instFreeHandles.pop_back();
    }
    else
    {
        handle = _nextInstanceHandle++;
    }
    _instanceOps.push_back(InstanceOp{InstanceOp::Add, handle, inst});
    return handle;
}

void EngineWgpu::EnqueueInstanceUpdate(uint32_t handle, const WgrInstance& inst)
{
    _instanceOps.push_back(InstanceOp{InstanceOp::Update, handle, inst});
}

void EngineWgpu::EnqueueInstanceRemove(uint32_t handle)
{
    _instanceOps.push_back(InstanceOp{InstanceOp::Remove, handle, {}});
    // Freed at enqueue, translated at drain: FIFO order guarantees the remove reads the old
    // slot before any same-frame reuse of this handle writes the new one.
    _instFreeHandles.push_back(handle);
}


// RFG-087: what fills the geometry pool, by producer. A pool that grows to 2,176 MB on a
// world with 800 retained models is not being filled by those models -- and the retained
// registration is the only place anyone had been looking. Counted at the drain, where every
// wgr_mesh_create passes, in the two kinds the drain distinguishes: the retained model path
// (MeshCreate) and the CPU direct path's vertex buffers (CpuMeshCreate), which every shape
// the direct path draws creates, per LOD, whether or not it was ever "registered".
namespace
{
struct GeometryPoolFill
{
    // 0 = retained model meshes, 1 = direct-path buffers with their own pool bytes,
    // 2 = direct-path buffers SHARING a retained mesh (RFG-088): bytes not spent.
    std::atomic<uint64_t> count[3]{};
    std::atomic<uint64_t> bytes[3]{};
    std::atomic<uint64_t> largest[3]{};
    void Note(int kind, uint64_t b)
    {
        count[kind].fetch_add(1, std::memory_order_relaxed);
        bytes[kind].fetch_add(b, std::memory_order_relaxed);
        uint64_t seen = largest[kind].load(std::memory_order_relaxed);
        while (b > seen && !largest[kind].compare_exchange_weak(seen, b, std::memory_order_relaxed))
        {
        }
    }
};
GeometryPoolFill GGeometryPoolFill;
} // namespace

void EngineWgpu::FillGeometryMeshFacts(GeometryAllocationReport& report)
{
    if (!_geometryOwnerLedger) return;
    report.rendererMeshFactsEnabled = true;
    try
    {
        const auto witnessStats = _standaloneWitnessBudget ? _standaloneWitnessBudget->Snapshot() : render::StandaloneMeshWitnessBudget::Stats{};
        // Refusal is informational: an unused shared-buffer witness can be
        // refused without omitting any standalone retirement evidence.
        const auto handles = _geometryOwnerLedger->SnapshotHandles();
        const auto& owners = handles.ownerReport;
        report.persistentAttributionEnabled = true;
        report.persistentAttributionComplete = owners.attributionComplete;
        report.persistentAttributionEpoch = owners.epoch;
        report.persistentIncompleteReasons = owners.incompleteReasons;
        report.producerEstimatedVertexBytes = owners.observedProducerEstimatedVertexBytes;
        report.producerEstimatedIndexBytes = owners.observedProducerEstimatedIndexBytes;
        report.persistentCpuReferencedVertexBytes = owners.cpuReferencedVertexBytes;
        report.persistentCpuReferencedIndexBytes = owners.cpuReferencedIndexBytes;
        report.persistentRetainedSharedCpuVertexBytes = owners.retainedSharedWithCpuVertexBytes;
        report.persistentRetainedSharedCpuIndexBytes = owners.retainedSharedWithCpuIndexBytes;
        report.persistentStandaloneCpuVertexBytes = owners.standaloneCpuVertexBytes;
        report.persistentStandaloneCpuIndexBytes = owners.standaloneCpuIndexBytes;
        report.persistentTrackedAllocations = owners.trackedAllocations;
        report.persistentTrackedBorrowerRecords = owners.trackedBorrowerRecords;
        report.persistentCpuBorrowers = owners.liveCpuBorrowers;
        report.persistentModelLodLinks = owners.modelLodLinks;
        report.persistentMailboxPending = owners.queuedDetachEvents;
        report.persistentMailboxDropped = owners.droppedDetachEvents;
        report.persistentCancelledCreates = owners.cancelledBeforeCreate;
        report.persistentPrunedAllocations = owners.cumulativePrunedAllocations;
        report.persistentPrunedBorrowers = owners.cumulativePrunedBorrowers;
        report.persistentRetirementCandidates = owners.queuedRetirementCandidates;
        report.persistentCleanupKnownCapacityBytes = owners.candidateKnownCapacityBytes;
        report.persistentDetachedHistory = owners.queuedDetachedHistory;
        report.persistentBorrowerKnownRecordBytes = owners.borrowerKnownRecordBytes;
        report.persistentSelectedDiagnosticPins = owners.selectedDiagnosticPins;
        report.persistentStandaloneWitnessLive = witnessStats.live;
        report.persistentStandaloneWitnessKnownBytes = witnessStats.bytes;
        report.persistentStandaloneWitnessPeakKnownBytes = witnessStats.peak;
        report.persistentStandaloneWitnessRefused = witnessStats.refused;
        report.persistentOwnerEstimatedBytes = owners.persistentOwnerBytes;
        report.persistentZeroOwnerEstimatedBytes = owners.zeroPersistentOwnerBytes;
        report.persistentReclaimableBytes = 0;
        report.ownershipCoverageComplete = false;
        report.rendererMeshFactEpoch = handles.epoch;
        report.rendererMeshFactsRequested = handles.trackedAllocations;
        report.rendererMeshFactsTruncated = handles.truncated;
        if (!handles.attributionComplete) report.persistentAttributionComplete = false;
        report.persistentIncompleteReasons |= handles.incompleteReasons;
        if (handles.epoch != owners.epoch || handles.epoch != _geometryOwnerEpoch)
            ++report.rendererMeshFactsUnknownMappings;
        // SnapshotHandles has its own hard cap; keep the foreign-call bound
        // explicit too, and never turn a copied prefix into complete evidence.
        if (handles.records.size() > 8192)
        {
            report.rendererMeshFactsTruncated = true;
            return;
        }
        std::vector<uint64_t> ids;
        ids.reserve(handles.records.size());
        for (const auto& record : handles.records)
        {
            if (record.epoch != handles.epoch || handles.epoch != _geometryOwnerEpoch || !record.rendererHandle)
            {
                ++report.rendererMeshFactsUnknownMappings;
                ids.push_back(0);
            }
            else ids.push_back(record.rendererHandle);
            if (record.scheduledRetired) ++report.rendererMeshFactsScheduledRetired;
        }
        std::vector<WgrMeshHandleFact> facts(ids.size());
        WgrMeshHandleFactSummary totals{};
        ++report.rendererMeshFactCalls;
        const bool valid = wgr_get_mesh_handle_facts(_renderer,
            WgrSlice<uint64_t>{ids.data(), U32(ids.size())}, facts.data(), U32(facts.size()), &totals) != 0;
        if (!valid || totals.handles_requested != ids.size() || totals.handles_inspected > facts.size() ||
            uint64_t(totals.present) + totals.absent + totals.invalid != totals.handles_inspected ||
            totals.duplicate_handles > totals.handles_inspected || totals.complete > 1 || (totals.flags & ~3u) != 0 ||
            totals.record_scope_valid != 1 || totals.reserved != 0) return;
        report.rendererMeshFactsValid = true;
        report.rendererMeshFactsInspected = totals.handles_inspected;
        report.rendererMeshFactsPresent = totals.present;
        report.rendererMeshFactsAbsent = totals.absent;
        report.rendererMeshFactsInvalid = totals.invalid;
        report.rendererMeshFactsDuplicateHandles = totals.duplicate_handles;
        report.rendererMeshFactVertexBytes = totals.unique_vertex_bytes;
        report.rendererMeshFactIndexBytes = totals.unique_index_bytes;
        report.rendererMeshLiveRecords = totals.live_mesh_records;
        report.rendererMeshRecordPoolGeneration = totals.pool_generation;
        report.rendererMeshRecordPoolLiveBytes = totals.pool_live_bytes;
        report.rendererMeshFactsTruncated = report.rendererMeshFactsTruncated || (totals.flags & 1) != 0;
        for (size_t i = 0; i < totals.handles_inspected; ++i)
        {
            const auto& fact = facts[i];
            const auto& record = handles.records[i];
            if (fact.mesh_handle != ids[i] || fact.reserved != 0 || fact.state > 2)
            {
                ++report.rendererMeshFactsUnknownMappings;
                continue;
            }
            if (fact.state == 1 && record.scheduledRetired)
                ++report.rendererMeshFactsPresentScheduledRetired;
            if (fact.state == 2)
            {
                if (record.scheduledRetired) ++report.rendererMeshFactsAbsentScheduledRetired;
                if (record.producerOwned || record.cpuBorrowers || record.modelLodLinks)
                    ++report.rendererMeshFactsAbsentWithPersistentOwners;
            }
        }
        report.rendererMeshFactsComplete = handles.attributionComplete && !handles.truncated &&
            handles.queuedDetachEvents == 0 && totals.complete == 1 && totals.flags == 0 &&
            totals.handles_inspected == handles.trackedAllocations && totals.duplicate_handles == 0 &&
            report.rendererMeshFactsUnknownMappings == 0 && report.rendererMeshFactsAbsentWithPersistentOwners == 0 &&
            report.rendererMeshFactsPresentScheduledRetired == 0;
        const auto closure = render::EvaluateMeshRecordClosure({totals.record_scope_valid == 1,
            totals.complete == 1 && report.rendererMeshFactsUnknownMappings == 0,
            totals.handles_requested, totals.handles_inspected, totals.present, totals.invalid,
            totals.duplicate_handles, totals.flags, totals.unique_vertex_bytes, totals.unique_index_bytes,
            totals.live_mesh_records, totals.pool_live_bytes});
        report.rendererMeshRecordScopeValid = closure.scopeValid;
        report.rendererMeshRecordClosureComplete = closure.recordsComplete;
        report.rendererMeshPoolResidualValid = closure.residualValid;
        report.rendererMeshPoolUnattributedBytes = closure.unattributedBytes;
        if (_geometryRegisteredRefsEnabled)
        {
            report.rendererRegisteredRefsEnabled = true;
            std::vector<WgrRegisteredMeshRef> references(ids.size());
            WgrRegisteredMeshRefSummary census{};
            ++report.rendererRegisteredRefsCalls;
            const uint32_t status = wgr_registered_mesh_refs(_renderer,
                WgrSlice<uint64_t>{ids.data(), static_cast<uint32_t>(ids.size())}, references.data(),
                static_cast<uint32_t>(references.size()), &census);
            // Both observations are made consecutively on this renderer owner;
            // no draw/submission or model mutation occurs between these calls.
            // This does not freeze the independently owned uploader or GPU work.
            bool referenceValid = status == 1 && census.rows_written == ids.size() && census.reserved == 0 &&
                census.complete <= 1 && (census.refusal_flags & ~7u) == 0 &&
                bool(census.complete) == (census.refusal_flags == 0) &&
                census.models_visited <= 8192 && census.lods_visited <= 262144 && census.sections_visited <= 1048576;
            uint64_t referenced = 0, modelRefs = 0, lodRefs = 0, sections = 0, missing = 0;
            for (size_t i = 0; referenceValid && i < references.size(); ++i)
            {
                const auto& row = references[i];
                referenceValid = row.mesh == ids[i] && row.reserved == 0 && (row.flags & ~1u) == 0 &&
                    row.distinct_models <= row.distinct_model_lods && row.distinct_model_lods <= row.section_occurrences &&
                    row.section_occurrences <= census.sections_visited &&
                    bool(row.flags & 1u) == (row.section_occurrences != 0 && facts[i].state == 2);
                if (!referenceValid) break;
                referenced += row.section_occurrences != 0;
                modelRefs += row.distinct_models; lodRefs += row.distinct_model_lods;
                sections += row.section_occurrences; missing += (row.flags & 1u) != 0;
                if (sections > census.sections_visited || missing > census.missing_records) referenceValid = false;
            }
            if (referenceValid && missing == census.missing_records)
            {
                report.rendererRegisteredRefsValid = true;
                report.rendererRegisteredRefsComplete = census.complete == 1;
                report.rendererRegisteredRefsRows = census.rows_written;
                report.rendererRegisteredModelsVisited = census.models_visited;
                report.rendererRegisteredLodsVisited = census.lods_visited;
                report.rendererRegisteredSectionsVisited = census.sections_visited;
                report.rendererRegisteredMissingRecords = census.missing_records;
                report.rendererRegisteredRefusalFlags = census.refusal_flags;
                report.rendererRegisteredReferencedMeshes = referenced;
                report.rendererRegisteredModelRefs = modelRefs;
                report.rendererRegisteredModelLodRefs = lodRefs;
                report.rendererRegisteredSectionOccurrences = sections;
                for (const auto& row : references)
                {
                    if (report.rendererRegisteredRefSamples.size() == 8) break;
                    if (row.section_occurrences) report.rendererRegisteredRefSamples.push_back(
                        {row.mesh, row.distinct_models, row.distinct_model_lods, row.section_occurrences, row.flags});
                }
            }
        }
        if (_geometryMeshAckEnabled)
        {
            report.rendererMeshAckEnabled = true;
            if (report.rendererMeshFactsComplete)
            {
                std::vector<WgrMeshHandleFact> ackFacts(ids.size());
                WgrMeshHandleFactSummary ackTotals{};
                WgrMeshSnapshotAck ack{};
                const uint32_t state = wgr_request_mesh_snapshot_ack(_renderer, handles.epoch,
                    WgrSlice<uint64_t>{ids.data(), static_cast<uint32_t>(ids.size())}, ackFacts.data(),
                    static_cast<uint32_t>(ackFacts.size()), &ackTotals, &ack);
                report.rendererMeshAckState = state;
                bool sameCut = state == 1 && ack.state == state && ack.ticket && ack.epoch == handles.epoch &&
                    ack.row_count == facts.size() && ackTotals.complete == 1 && ackTotals.flags == 0 &&
                    ackTotals.handles_requested == totals.handles_requested &&
                    ackTotals.handles_inspected == totals.handles_inspected && ackTotals.present == totals.present &&
                    ackTotals.absent == totals.absent && ackTotals.invalid == 0 && ackTotals.duplicate_handles == 0 &&
                    ackTotals.unique_vertex_bytes == totals.unique_vertex_bytes && ackTotals.unique_index_bytes == totals.unique_index_bytes &&
                    ackTotals.record_scope_valid == totals.record_scope_valid && ackTotals.reserved == 0 &&
                    ackTotals.live_mesh_records == totals.live_mesh_records && ackTotals.pool_live_bytes == totals.pool_live_bytes &&
                    ackTotals.pool_generation == totals.pool_generation && ack.pool_generation == totals.pool_generation &&
                    ack.pool_live_bytes == totals.pool_live_bytes;
                for (size_t i = 0; sameCut && i < facts.size(); ++i)
                    sameCut = ackFacts[i].mesh_handle == facts[i].mesh_handle && ackFacts[i].state == facts[i].state &&
                        ackFacts[i].vertex_bytes == facts[i].vertex_bytes && ackFacts[i].index_bytes == facts[i].index_bytes && ackFacts[i].reserved == 0;
                if (sameCut)
                {
                    report.rendererMeshAckValid = true;
                    report.rendererMeshAckTicket = ack.ticket; report.rendererMeshAckEpoch = ack.epoch;
                    report.rendererMeshAckPoolGeneration = ack.pool_generation;
                    report.rendererMeshAckRequested = ackTotals.handles_requested;
                    report.rendererMeshAckPresent = ackTotals.present; report.rendererMeshAckAbsent = ackTotals.absent;
                    report.rendererMeshAckVertexBytes = ackTotals.unique_vertex_bytes; report.rendererMeshAckIndexBytes = ackTotals.unique_index_bytes;
                    report.rendererMeshAckPoolLiveBytes = ack.pool_live_bytes;
                    report.rendererMeshAckPoolCapacityBytes = ack.pool_capacity_bytes; report.rendererMeshAckPoolRetiredBytes = ack.pool_retired_bytes;
                }
                else if (ack.ticket) wgr_cancel_mesh_snapshot_ack(_renderer, ack.ticket);
            }
        }
    }
    catch (...)
    {
        // An optional metadata allocation failure cannot alter the normal frame
        // or lifetime policy. No prefix is evidence of complete range attribution.
        _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Capacity);
        report.persistentAttributionComplete = false;
        report.persistentIncompleteReasons |= static_cast<uint64_t>(render::GeometryOwnerLedger::Reason::Capacity);
        report.rendererMeshFactsComplete = false;
        report.rendererMeshFactsTruncated = true;
    }
}

void EngineWgpu::DrainResourceOps(std::vector<ResourceOp>& ops)
{
    if (_geometryOwnerLedger) _geometryOwnerLedger->DrainDetachMailbox();
    if (ops.empty())
        return;
    if (!_renderer)
    {
        for (const auto& op : ops)
            if (op.kind == ResourceOp::GeometryReport)
            {
                auto failed = std::make_shared<GeometryAllocationReport>();
                failed->requestId = op.geometryReport->requestId;
                _geometryReportUnpublished = std::move(failed);
            }
            else if (op.kind == ResourceOp::LodDemandStart || op.kind == ResourceOp::LodDemandCancel)
            {
                auto failed = std::make_shared<LodDemandReport>(); failed->requestId = op.lodDemand->epoch;
                _lodDemandUnpublished = std::move(failed); _lodDemandOwnerStatus = GeometryReportStatus::Failed;
            }
        ops.clear();
        return;
    }
    const auto drainStart = std::chrono::steady_clock::now();
    const size_t operationCount = ops.size();
    std::array<uint32_t, 7> kindCounts{};
    for (ResourceOp& op : ops)
    {
        const size_t kind = static_cast<size_t>(op.kind);
        if (kind < kindCounts.size())
            ++kindCounts[kind];
        switch (op.kind)
        {
        case ResourceOp::MeshCreate:
        {
            const uint64_t ownerVertexBytes = _geometryOwnerLedger ? op.verts.size() * sizeof(SVertex) : 0;
            const uint64_t ownerIndexBytes = _geometryOwnerLedger ? op.indices.size() * sizeof(uint32_t) : 0;
            // Crown centres first: their base is baked into the vertex data, which is why
            // the registration could never be queued on its own (REN-THR-008).
            if (!op.crownCentres.empty())
            {
                const uint32_t base = wgr_register_crown_centres(
                    _renderer, WgrSlice<WgrVec4>{op.crownCentres.data(), U32(op.crownCentres.size())});
                for (size_t vi = 0; vi < op.verts.size() && vi < op.crownComp.size(); vi++)
                {
                    op.verts[vi].conform = base + op.crownComp[vi];
                }
            }
            _meshIdOf[op.mesh] = wgr_mesh_create(
                _renderer, AsMeshVerts(op.verts),
                WgrSlice<uint32_t>{reinterpret_cast<const uint32_t*>(op.indices.data()), U32(op.indices.size())});
            if (_geometryOwnerLedger)
            {
                _geometryOwnerLedger->AllocationCreated(_geometryOwnerEpoch, op.mesh, ownerVertexBytes, ownerIndexBytes,
                    render::GeometryOwnerLedger::ProducerKind::Retained, _meshIdOf[op.mesh]);
                if (!_meshIdOf[op.mesh])
                    _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Protocol);
            }
            GGeometryPoolFill.Note(0, op.verts.size() * sizeof(SVertex) + op.indices.size() * sizeof(uint32_t));
            break;
        }
        case ResourceOp::MeshDestroy:
        {
            const auto it = _meshIdOf.find(op.mesh);
            if (it != _meshIdOf.end())
            {
                // All CPU borrowers, including queued creates, participate in
                // the same lifetime. The newest weak slot is not an owner census.
                bool handedOver = false;
                std::shared_ptr<render::SharedRetainedMeshLifetime> lifetime;
                {
                    std::lock_guard lock(_sharedMeshLifetimeMutex);
                    if (const auto sh = _sharedMeshLifetime.find(op.mesh); sh != _sharedMeshLifetime.end())
                    { lifetime = sh->second; _sharedMeshLifetime.erase(sh); }
                }
                if (lifetime)
                {
                    const uint64_t retirement = lifetime->ReleaseProducer(it->second);
                    handedOver = retirement == 0;
                    lifetime->Dispatch(retirement, [&](uint64_t handle) { wgr_mesh_destroy(_renderer, handle); });
                }
                else wgr_mesh_destroy(_renderer, it->second);
                if (_geometryOwnerLedger)
                {
                    _geometryOwnerLedger->ProducerOwnershipReleased(_geometryOwnerEpoch, op.mesh);
                    if (!handedOver) _geometryOwnerLedger->AllocationRetired(_geometryOwnerEpoch, op.mesh);
                }
                _meshIdOf.erase(it);
            }
            else
            {
                std::shared_ptr<render::SharedRetainedMeshLifetime> lifetime;
                {
                    std::lock_guard lock(_sharedMeshLifetimeMutex);
                    if (const auto sh = _sharedMeshLifetime.find(op.mesh); sh != _sharedMeshLifetime.end())
                    { lifetime = sh->second; _sharedMeshLifetime.erase(sh); }
                }
                if (lifetime) lifetime->ReleaseProducer(0); // No allocation was created.
            }
            break;
        }
        case ResourceOp::CpuMeshCreate:
        {
            // Source IDs are minted/enqueued on one producer in monotonic FIFO
            // order. Advance only after attach/cancel bookkeeping, preserving
            // detach-before-create tombstones until that operation settles.
            struct SettledCpuCreate {
                render::GeometryOwnerLedger* ledger; uint64_t epoch,id;
                ~SettledCpuCreate() { if(ledger) ledger->NoteCpuCreateSettled(epoch,id); }
            } settled{_geometryOwnerLedger.get(),_geometryOwnerEpoch,op.cpu?op.cpu->producer:op.mesh};
            // REN-THR-011. `dead` means the VertexBufferWgpu was destroyed before this drain
            // ran, so nothing can name the mesh any more: skipping the create is the whole
            // handling of that case, and it is cheaper than creating and then destroying.
            if (!op.cpu || op.cpu->dead)
            {
                if (_geometryOwnerLedger) _geometryOwnerLedger->NoteCancelledBeforeCreate(_geometryOwnerEpoch);
                break;
            }
            if (op.cpu->sharedProducer != 0)
            {
                // RFG-088: draw through the retained mesh if it still exists; its create
                // was queued before this one, so `_meshIdOf` already names it.
                if (const auto shared = _meshIdOf.find(op.cpu->sharedProducer);
                    shared != _meshIdOf.end() && op.cpu->sharedLifetime.Bind(shared->second))
                {
                    op.cpu->renderer = shared->second;
                    if (_geometryOwnerLedger)
                        _geometryOwnerLedger->CpuBorrowerAttached(_geometryOwnerEpoch, op.cpu->producer, op.cpu->sharedProducer);
                    GGeometryPoolFill.Note(2, op.cpu->verts.size() * sizeof(SVertex) +
                                                  op.cpu->indices.size() * sizeof(uint32_t));
                    std::vector<SVertex>().swap(op.cpu->verts);
                    std::vector<VertexIndex>().swap(op.cpu->indices);
                    break;
                }
                const uint64_t retirement = op.cpu->sharedLifetime.FallBackToStandalone();
                bool dispatched = false;
                op.cpu->sharedLifetime.Dispatch(retirement, [&](uint64_t handle) {
                    wgr_mesh_destroy(_renderer, handle); dispatched = true;
                });
                if (dispatched && _geometryOwnerLedger)
                    _geometryOwnerLedger->AllocationRetired(_geometryOwnerEpoch, op.cpu->sharedProducer);
                op.cpu->sharedProducer = 0; // retained mapping is gone: exact original payload fallback
            }
            const auto witness = _geometryOwnerLedger ?
                static_cast<AttributedCpuMeshSlot*>(op.cpu.get())->standaloneWitness : nullptr;
            if (_geometryOwnerLedger && !witness)
                _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Capacity);
            GGeometryPoolFill.Note(1, op.cpu->verts.size() * sizeof(SVertex) + op.cpu->indices.size() * sizeof(uint32_t));
            op.cpu->renderer =
                wgr_mesh_create(_renderer, AsMeshVerts(op.cpu->verts),
                                WgrSlice<uint32_t>{reinterpret_cast<const uint32_t*>(op.cpu->indices.data()),
                                                   U32(op.cpu->indices.size())});
            if (_geometryOwnerLedger)
            {
                _geometryOwnerLedger->AllocationCreated(_geometryOwnerEpoch, op.cpu->producer,
                    op.cpu->verts.size() * sizeof(SVertex), op.cpu->indices.size() * sizeof(uint32_t),
                    render::GeometryOwnerLedger::ProducerKind::Cpu, op.cpu->renderer);
                _geometryOwnerLedger->CpuBorrowerAttached(_geometryOwnerEpoch, op.cpu->producer, op.cpu->producer);
                _geometryOwnerLedger->ProducerOwnershipReleased(_geometryOwnerEpoch, op.cpu->producer);
                if (!op.cpu->renderer)
                    _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Protocol);
            }
            // The payload has done its job; a CPU mesh's later uploads go through the
            // uploader, not through the slot. Release rather than clear -- these are full
            // vertex and index arrays and there are thousands of live CPU meshes.
            if (witness)
            {
                const auto retirement = witness->Bind(op.cpu->renderer);
                bool dispatched = false;
                witness->Dispatch(retirement, [&](uint64_t handle) {
                    wgr_mesh_destroy(_renderer, handle); dispatched = true;
                });
                if (dispatched && _geometryOwnerLedger)
                    _geometryOwnerLedger->QueueAllocationRetirementScheduled(_geometryOwnerEpoch, op.cpu->producer);
            }
            std::vector<SVertex>().swap(op.cpu->verts);
            std::vector<VertexIndex>().swap(op.cpu->indices);
            break;
        }
        case ResourceOp::MeshSetSkin:
        {
            // Behind its mesh's create in the same FIFO queue, so `renderer` is set by the
            // time this runs -- unless the create was skipped (dead) or never enqueued, in
            // which case there is nothing to skin and silence is correct: the buffer is gone.
            if (!op.cpu || op.cpu->renderer == 0)
            {
                break;
            }
            wgr_mesh_update(_renderer, op.cpu->renderer, AsMeshVerts(op.verts));
            wgr_mesh_set_skin(_renderer, op.cpu->renderer, op.bones, op.weights);
            break;
        }
        case ResourceOp::ModelRetire:
        {
            if (_geometryOwnerLedger) _geometryOwnerLedger->ModelRetired(_geometryOwnerEpoch, op.model);
            if (const auto it = _modelIdOf.find(op.model); it != _modelIdOf.end())
            {
                wgr_model_retire(_renderer, it->second);
                _modelIdOf.erase(it);
            }
            break;
        }
        case ResourceOp::ModelRegister:
        {
            if (_geometryOwnerLedger)
            {
                // Capture producer identities before translating sections to renderer
                // handles; duplicates within one LOD are one persistent owner link.
                // Bound diagnostic work independently of link insertion: duplicate
                // sections and empty LODs must not evade the attribution limit.
                constexpr uint32_t maxDiagnosticVisits = 32768;
                uint32_t diagnosticVisits = 0;
                bool diagnosticLimited = false;
                for (uint32_t lod = 0; lod < op.lods.size() && !diagnosticLimited; ++lod)
                {
                    if (diagnosticVisits == maxDiagnosticVisits)
                    {
                        _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Capacity);
                        break;
                    }
                    ++diagnosticVisits;
                    const auto& level = op.lods[lod];
                    const uint64_t end = uint64_t(level.section_base) + level.section_count;
                    if (end > op.sections.size())
                    {
                        _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Protocol);
                        continue;
                    }
                    for (uint64_t section = level.section_base; section < end; ++section)
                    {
                        if (diagnosticVisits == maxDiagnosticVisits)
                        {
                            _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Capacity);
                            diagnosticLimited = true;
                            break;
                        }
                        ++diagnosticVisits;
                        _geometryOwnerLedger->ModelLodAttached(_geometryOwnerEpoch, op.model, lod, op.sections[section].mesh);
                    }
                }
            }
            // The sections carry producer mesh handles; the renderer gets real ones.
            for (WgrModelSection& sec : op.sections)
            {
                const auto it = _meshIdOf.find(sec.mesh);
                sec.mesh = it != _meshIdOf.end() ? it->second : 0;
            }
            _modelIdOf[op.model] = wgr_model_register(
                _renderer, op.sphereRadius, WgrSlice<WgrModelLod>{op.lods.data(), U32(op.lods.size())},
                WgrSlice<WgrModelSection>{op.sections.data(), U32(op.sections.size())},
                WgrSlice<WgrModelMaterial>{op.materials.data(), U32(op.materials.size())}, op.name.c_str());
            if (_geometryOwnerLedger && _modelIdOf[op.model] == WGR_INVALID_MODEL)
            {
                _geometryOwnerLedger->ModelRetired(_geometryOwnerEpoch, op.model);
                _geometryOwnerLedger->MarkIncomplete(_geometryOwnerEpoch, render::GeometryOwnerLedger::Reason::Protocol);
            }
            break;
        }
        case ResourceOp::GeometryReport:
        {
            // Only the renderer drain accesses actual IDs/allocations. Never wait for
            // this request on the producer; a later poll reads the immutable response.
            GeometryAllocationReport report;
            report.requestId = op.geometryReport->requestId;
            report.maxModels = op.geometryReport->maxModels;
            report.maxRows = op.geometryReport->rows;
            report.maxSectionVisits = op.geometryReport->visits;
            report.registryEntriesAtRequest = op.geometryReport->registryEntries;
            report.registryEntriesVisited = op.geometryReport->registryEntriesVisited;
            report.ineligibleEntriesSkipped = op.geometryReport->ineligibleEntriesSkipped;
            report.modelsRequested = static_cast<uint32_t>(op.geometryReport->models.size());
            report.unresolvedNames = op.geometryReport->unresolvedNames;
            std::vector<uint32_t> ids;
            ids.reserve(op.geometryReport->models.size());
            for (const auto& entry : op.geometryReport->models)
            {
                const auto found = _modelIdOf.find(entry.first);
                ids.push_back(found == _modelIdOf.end() ? WGR_INVALID_MODEL : found->second);
            }
            std::vector<WgrGeometryAllocationRow> rows(op.geometryReport->rows);
            WgrGeometryAllocationSummary summary{};
            const bool valid = wgr_get_geometry_allocation_report(_renderer,
                WgrSlice<uint32_t>{ids.data(), static_cast<uint32_t>(ids.size())},
                op.geometryReport->rows, op.geometryReport->visits, rows.data(), static_cast<uint32_t>(rows.size()), &summary) != 0;
            if (valid && summary.rows_written <= rows.size())
            {
                report.valid = true;
                report.uniqueVertexBytes = summary.unique_vertex_bytes;
                report.uniqueIndexBytes = summary.unique_index_bytes;
                report.overlapWithinInspectedMeshes = summary.overlap_within_inspected_meshes;
                report.overlapWithinInspectedVertexBytes = summary.overlap_within_inspected_vertex_bytes;
                report.overlapWithinInspectedIndexBytes = summary.overlap_within_inspected_index_bytes;
                report.ownershipCoverageComplete = false; // inspected retained LODs are not all mesh owners
                report.poolLiveBytes = summary.pool_live_bytes;
                report.poolCapacityBytes = summary.pool_capacity_bytes;
                report.poolRetiredBytes = summary.pool_retired_bytes;
                report.modelsVisited = summary.models_visited;
                report.lodsInVisitedModels = summary.lods_total;
                report.sectionVisits = summary.section_visits;
                report.uniqueMeshes = summary.unique_meshes;
                report.missingModels = summary.missing_models;
                report.missingMeshes = summary.missing_meshes;
                report.invalidRanges = summary.invalid_ranges;
                report.truncated = summary.truncated != 0 || op.geometryReport->registryEntries > op.geometryReport->registryEntriesVisited;
                report.rows.reserve(summary.rows_written);
                for (uint32_t i = 0; i < summary.rows_written; ++i)
                {
                    const auto& raw = rows[i];
                    const auto found = std::find(ids.begin(), ids.end(), raw.model_id);
                    if (found == ids.end()) { ++report.missingModels; continue; }
                    const auto& identity = op.geometryReport->models[static_cast<size_t>(found - ids.begin())];
                    GeometryAllocationRow row;
                    row.asset = identity.second;
                    row.producerModel = identity.first;
                    row.rendererModel = raw.model_id;
                    row.lodIndex = raw.lod_index;
                    row.resolution = raw.resolution;
                    row.vertexBytes = raw.vertex_bytes;
                    row.indexBytes = raw.index_bytes;
                    row.sections = raw.section_count;
                    row.liveMeshes = raw.live_meshes;
                    row.missingMeshes = raw.missing_meshes;
                    report.rows.push_back(std::move(row));
                }
            }
            // Publication happens at the existing end-frame snapshot boundary via
            // try-lock; even a diagnostic poll cannot make the renderer wait.
            _geometryReportUnpublished = std::make_shared<GeometryAllocationReport>(std::move(report));
            // Allocate the publication packet before optional fence acquisition,
            // so allocation failure cannot leak an accepted diagnostic ticket.
            if (_geometryOwnerLedger) FillGeometryMeshFacts(*_geometryReportUnpublished);
            break;
        }
        case ResourceOp::SharedMeshFixtureObserve:
        {
            auto& fixture = *op.sharedFixture;
            // request0 is an explicit private-fixture metadata unpin operation,
            // never a renderer fact request/public completed observation.
            if(!op.sharedFixtureRequest) {
                if(_geometryOwnerLedger) _geometryOwnerLedger->SetSelectedDiagnosticPin(_geometryOwnerEpoch,fixture.producer,false);
                break;
            }
            if(_geometryOwnerLedger) _geometryOwnerLedger->SetSelectedDiagnosticPin(_geometryOwnerEpoch,fixture.producer,true);
            uint64_t handle = op.mesh;
            if (!handle)
                if (const auto found = _meshIdOf.find(fixture.producer); found != _meshIdOf.end()) handle = found->second;
            if (!handle && fixture.standaloneWitness) handle = fixture.standaloneWitness->Snapshot().handle;
            WgrMeshHandleFact fact{};
            WgrMeshHandleFactSummary summary{};
            const bool valid = handle && wgr_get_mesh_handle_facts(_renderer,
                WgrSlice<uint64_t>{&handle, 1}, &fact, 1, &summary) != 0 &&
                summary.handles_requested == 1 && summary.handles_inspected == 1 && summary.complete == 1 &&
                summary.flags == 0 && summary.duplicate_handles == 0 && fact.mesh_handle == handle &&
                fact.reserved == 0 && fact.state <= 2;
            const auto standaloneState = fixture.standaloneWitness ? fixture.standaloneWitness->Snapshot() : render::StandaloneMeshRetirementWitness::State{};
            const auto owners = fixture.lifetime ? fixture.lifetime->SnapshotOwners() :
                render::SharedRetainedMeshLifetime::Owners{standaloneState.handle && !standaloneState.released ? 1u : 0u, false};
            const auto witnessStats = _standaloneWitnessBudget ? _standaloneWitnessBudget->Snapshot() : render::StandaloneMeshWitnessBudget::Stats{};
            render::GeometryOwnerLedger::HandleRecord ledgerRecord;
            const bool ledgerObserved = _geometryOwnerLedger &&
                _geometryOwnerLedger->SnapshotHandle(_geometryOwnerEpoch, fixture.producer, ledgerRecord);
            std::lock_guard lock(fixture.mutex);
            if (fixture.report.requestId != op.sharedFixtureRequest) break;
            fixture.report.cpuBorrowers = uint32_t(owners.boundBorrowers);
            fixture.report.producerOwned = owners.producerOwned;
            fixture.report.ledgerEnabled = bool(_geometryOwnerLedger);
            fixture.report.ledgerRecordObserved = ledgerObserved;
            fixture.report.ledgerScheduledRetired = ledgerObserved && ledgerRecord.scheduledRetired;
            fixture.report.ledgerProducerOwned = ledgerObserved && ledgerRecord.producerOwned;
            fixture.report.ledgerCpuBorrowers = ledgerObserved ? ledgerRecord.cpuBorrowers : 0;
            fixture.report.witnessKnownMetadataBytes = witnessStats.bytes;
            fixture.report.witnessPeakKnownMetadataBytes = witnessStats.peak;
            fixture.report.witnessCapacityRefused = witnessStats.refused;
            fixture.report.observedRequestId = op.sharedFixtureRequest;
            fixture.report.rendererHandle = handle;
            fixture.report.meshState = valid ? fact.state : 0;
            fixture.report.status = valid ? GeometryReportStatus::Ready : GeometryReportStatus::Failed;
            break;
        }
        case ResourceOp::LodDemandStart:
        {
            const auto& request = *op.lodDemand;
            const auto result = wgr_start_lod_demand(_renderer, request.epoch,
                WgrSlice<uint32_t>{request.models.data(), static_cast<uint32_t>(request.models.size())}, request.frames);
            _lodDemandOwnerEpoch = result == 1 ? request.epoch : 0;
            _lodDemandOwnerStatus = result == 1 ? GeometryReportStatus::Pending : result == 0 ? GeometryReportStatus::Unsupported :
                result == 3 ? GeometryReportStatus::Busy : GeometryReportStatus::Invalid;
            if (result != 1)
            {
                auto failed = std::make_shared<LodDemandReport>(); failed->requestId = request.epoch;
                _lodDemandUnpublished = std::move(failed);
            }
            break;
        }
        case ResourceOp::LodDemandCancel:
            wgr_cancel_lod_demand(_renderer, op.lodDemand->epoch);
            break;
        }
    }
    if (_geometryOwnerLedger) _geometryOwnerLedger->DrainDetachMailbox();
    ops.clear();
    const double drainMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drainStart).count();
    if (_slowBlockMs > 0.0 && drainMs >= _slowBlockMs)
    {
        LOG_INFO(Graphics,
                 "Wgpu resource drain: {:.1f} ms | ops={} meshCreate={} meshDestroy={} modelRegister={} "
                 "cpuMeshCreate={} meshSetSkin={}",
                 drainMs, operationCount, kindCounts[ResourceOp::MeshCreate], kindCounts[ResourceOp::MeshDestroy],
                 kindCounts[ResourceOp::ModelRegister], kindCounts[ResourceOp::CpuMeshCreate],
                 kindCounts[ResourceOp::MeshSetSkin]);
    }
    // The overlay drain is NOT called from here any more. It is one step of the consumer
    // block's explicit sequence in NextFrame, where the order it depends on is visible --
    // a nested call made the order a property of two functions instead of one list.
}

void EngineWgpu::EnqueueCpuMeshSetSkin(std::shared_ptr<CpuMeshSlot> slot, std::vector<SVertex> verts,
                                       std::vector<uint8_t> bones, std::vector<uint8_t> weights)
{
    ResourceOp op;
    op.kind = ResourceOp::MeshSetSkin;
    op.mesh = slot ? slot->producer : 0;
    op.cpu = std::move(slot);
    op.verts = std::move(verts);
    op.bones = std::move(bones);
    op.weights = std::move(weights);
    _resourceOps.push_back(std::move(op)); // producer side: enqueues into the members
}

// REN-THR-011 -- see PendingMeshRef in the header for why this exists and why it runs here.
// Called from NextFrame after every drain and before the frame is described, so what is
// published (and hashed) carries renderer handles exactly as a direct wgr_mesh_create would
// have left them. Costs nothing on a frame that created no CPU mesh, which is most of them.
// Takes the arrays explicitly because it must run AFTER the drains -- which is after the
// producer's vectors have been swapped into the ring slot, so the records to patch are the
// BACKING's, not `_draws3d`'s. Getting that order wrong is not caught by the equivalence
// gate: both runs drop the same draws and compare equal, which is exactly what happened
// when the drains first moved (2 records a frame silently dropped, visible only in this
// function's own log line).
void EngineWgpu::PatchPendingMeshHandles(std::vector<WgrDraw3D>& draws, std::vector<WgrShadowCaster>& casters,
                                         std::vector<PendingMeshRef>& drawRefs,
                                         std::vector<PendingMeshRef>& casterRefs)
{
    for (const PendingMeshRef& ref : drawRefs)
    {
        if (ref.index >= draws.size() || !ref.slot)
            continue;
        // Still 0 means the create was cancelled (the buffer died inside this same draw
        // phase). The renderer ignores an unknown mesh, so a 0 here drops the draw -- which
        // is what happened before this change too, by a longer road: the destructor's
        // deferred destroy is drained at the TOP of wgr_render_frame, so the handle the draw
        // carried was already freed by the time the draw was read.
        draws[ref.index].mesh = ref.slot->renderer;
        if (ref.slot->renderer == 0)
            _droppedUndefinedMeshDraws++;
    }
    for (const PendingMeshRef& ref : casterRefs)
    {
        if (ref.index >= casters.size() || !ref.slot)
            continue;
        casters[ref.index].mesh = ref.slot->renderer;
        if (ref.slot->renderer == 0)
            _droppedUndefinedMeshDraws++;
    }
    drawRefs.clear();
    casterRefs.clear();
    if (_droppedUndefinedMeshDraws != 0)
    {
        static uint32_t reported = 0;
        if (reported++ < 8)
        {
            LOG_INFO(Graphics,
                     "Wgpu CPU mesh: {} draw records named a mesh whose create was cancelled "
                     "(buffer released inside the same draw phase); dropped",
                     _droppedUndefinedMeshDraws);
        }
        _droppedUndefinedMeshDraws = 0;
    }
}

// Separate from DrainResourceOps because it has a second legitimate drain point: ImGui
// creates its font atlas from INSIDE NextFrame (DebugOverlay::NewFrame runs after the main
// drains), and SubmitOverlay translates handles in the same frame -- so SubmitOverlay
// drains any ops enqueued since. Both call sites are producer-serial today and both sit at
// what will be the render thread's frame boundary.
void EngineWgpu::DrainOverlayTexOps(std::vector<OverlayTexOp>& ops)
{
    if (!_renderer)
    {
        ops.clear();
        return;
    }
    for (OverlayTexOp& op : ops)
    {
        switch (op.kind)
        {
        case OverlayTexOp::Create:
            _overlayTexIdOf[op.handle] = wgr_texture_create(_renderer, U32(op.w), U32(op.h), WGR_TEXTURE_RGBA8, 1, 0,
                                                            op.rgba.data(), U32(op.rgba.size()));
            break;
        case OverlayTexOp::Update:
            if (const auto it = _overlayTexIdOf.find(op.handle); it != _overlayTexIdOf.end())
                wgr_texture_update(_renderer, it->second, op.rgba.data(), U32(op.rgba.size()));
            break;
        case OverlayTexOp::Destroy:
            if (const auto it = _overlayTexIdOf.find(op.handle); it != _overlayTexIdOf.end())
            {
                wgr_texture_destroy(_renderer, it->second);
                _overlayTexIdOf.erase(it);
            }
            break;
        }
    }
    ops.clear();
}

// REN-THR-013: overlay draws carry producer texture handles out of SubmitOverlay; this
// resolves them once the drain has created whatever the frame refers to. An unknown handle
// draws WHITE and says so, because a white panel is otherwise indistinguishable from a UI
// that legitimately has no texture.
void EngineWgpu::TranslateOverlayDrawHandles(std::vector<WgrOverlayDraw>& draws)
{
    for (WgrOverlayDraw& d : draws)
    {
        if (d.texture_id == 0)
            continue;
        const auto it = _overlayTexIdOf.find(d.texture_id);
        if (it != _overlayTexIdOf.end())
        {
            d.texture_id = it->second;
            continue;
        }
        d.texture_id = 0;
        static uint32_t reported = 0;
        if (reported++ < 8)
            LOG_ERROR(Graphics, "ANOMALY overlay draw carries unknown producer texture handle {} -- drawn white",
                      d.texture_id);
    }
}

void EngineWgpu::DrainInstanceOps(std::vector<InstanceOp>& ops)
{
    constexpr uint32_t InvalidInstanceSlot = ~uint32_t{0};
    if (ops.empty())
        return;
    if (!_renderer)
    {
        // No renderer, nothing to translate into; dropping the ops matches what the old
        // direct calls did behind their own null checks.
        ops.clear();
        return;
    }
    for (const InstanceOp& op : ops)
    {
        // Only the consumer owns this table: enqueue can overlap this drain.
        // Failed model translation must not leave a reused handle pointing at
        // another live instance (nor make a later remove index past the table).
        if (op.kind == InstanceOp::Add)
        {
            if (op.handle >= _instSlotOf.size())
                _instSlotOf.resize(static_cast<size_t>(op.handle) + 1, InvalidInstanceSlot);
            _instSlotOf[op.handle] = InvalidInstanceSlot;
        }
        else if (op.handle >= _instSlotOf.size() || _instSlotOf[op.handle] == InvalidInstanceSlot)
            continue;
        switch (op.kind)
        {
        case InstanceOp::Add:
        case InstanceOp::Update:
        {
            // The payload's model field is a PRODUCER handle; the renderer gets the real id.
            // An untranslatable one is a bug on this side of the boundary and must be loud:
            // passing it through would draw a stranger's model, which is precisely the
            // failure mode the ids exist to prevent.
            WgrInstance inst = op.inst;
            const auto it = _modelIdOf.find(inst.model);
            if (it == _modelIdOf.end())
            {
                static uint32_t reported = 0;
                if (reported++ < 8)
                    LOG_ERROR(Graphics,
                              "ANOMALY instance op carries unknown producer model handle {} -- op dropped",
                              inst.model);
                break;
            }
            inst.model = it->second;
            if (op.kind == InstanceOp::Add)
                _instSlotOf[op.handle] = wgr_instance_add(_renderer, &inst);
            else
                wgr_instance_update(_renderer, _instSlotOf[op.handle], &inst);
            break;
        }
        case InstanceOp::Remove:
            wgr_instance_remove(_renderer, _instSlotOf[op.handle]);
            _instSlotOf[op.handle] = InvalidInstanceSlot;
            break;
        }
    }
    ops.clear();
}

// REN-THR-014: THE CONSUMER BLOCK, as one function. Every call in it touches the renderer
// handle and nothing outside it does. The order is load-bearing and each step says why:
// settings configure what the draws will do; resources precede instances because a queued
// instance add may name a model whose registration is queued behind it; overlay textures
// precede the draw translation that resolves them; the CPU-mesh patch follows the drains
// that create what it resolves and precedes the hash that walks what it patches.
void EngineWgpu::RunConsumerBlock(WgrFrame& frame, PushFrameBacking& backing, FrameQueues& q)
{
    // PERF-018 left one question open and it is the biggest number in the frame: a ~300 ms
    // stall recurring roughly once per 250 frames, unchanged by overlap, sitting inside what
    // the profiler calls `swap`. `swap` is this whole block, not the present call, so the
    // profiler cannot say which part of it -- a texture upload burst and a driver pipeline
    // compile look identical from outside.
    //
    // Six clocks, and a line only when the block overruns. Threshold in ms via
    // WGR_SLOW_BLOCK_MS (default 100); set it to 0 to switch the report off entirely. The
    // steady-state cost is six steady_clock reads on a block that costs milliseconds.
    // Threshold read once into the member so BOTH reports -- this one and the producer's
    // in NextFrame -- share it and cannot drift apart.
    [[maybe_unused]] static const bool once = [this]
    {
        const char* v = std::getenv("WGR_SLOW_BLOCK_MS");
        _slowBlockMs = (v != nullptr && v[0] != 0) ? std::atof(v) : 100.0;
        return true;
    }();

    const auto blockStart = std::chrono::steady_clock::now();
    ApplyPendingRendererSettings(q.settings);
    const auto afterSettings = std::chrono::steady_clock::now();
    const size_t resourceOpCount = q.resourceOps.size();
    DrainResourceOps(q.resourceOps);
    const auto afterResources = std::chrono::steady_clock::now();
    DrainInstanceOps(q.instanceOps);
    DrainOverlayTexOps(q.overlayTexOps);
    TranslateOverlayDrawHandles(backing.overlayDraws);
    PatchPendingMeshHandles(backing.draws3d, backing.shadowCasters, q.draws3dPendingMesh, q.castersPendingMesh);
    const auto afterDrains = std::chrono::steady_clock::now();

    HashPublishedFrame(frame, backing);
    const auto afterHash = std::chrono::steady_clock::now();
    wgr_render_frame(_renderer, &frame);
    if (_geometryOwnerLedger)
    {
        // Once per real renderer frame, after deferred uploader destroys drain.
        // Metadata only: at most32 requested full-generation handles; the
        // existing fact API also computes its allocator free-span metadata.
        std::array<render::GeometryOwnerLedger::HandleRecord,32> candidates;
        const auto count=_geometryOwnerLedger->TakeRetirementCandidates(_geometryOwnerEpoch,candidates.data(),candidates.size());
        if(count)
        {
            std::array<uint64_t,32> ids{}; std::array<WgrMeshHandleFact,32> rows{};
            for(size_t i=0;i<count;++i) ids[i]=candidates[i].rendererHandle;
            WgrMeshHandleFactSummary summary{};
            bool valid=wgr_get_mesh_handle_facts(_renderer,{ids.data(),U32(count)},rows.data(),U32(count),&summary)!=0 &&
                summary.handles_requested==count && summary.handles_inspected==count && summary.complete==1 &&
                summary.flags==0 && summary.invalid==0 && summary.duplicate_handles==0 && summary.record_scope_valid==1 &&
                summary.reserved==0 && uint64_t(summary.present)+summary.absent==count;
            for(size_t i=0;valid && i<count;++i)
                valid=rows[i].mesh_handle==ids[i] && rows[i].reserved==0 && (rows[i].state==1 || rows[i].state==2) &&
                    (rows[i].state!=2 || (rows[i].vertex_bytes==0 && rows[i].index_bytes==0));
            for(size_t i=0;i<count;++i)
                _geometryOwnerLedger->ConfirmRetirementObservation(_geometryOwnerEpoch,candidates[i].allocationId,
                    ids[i],valid && rows[i].state==2);
        }
    }
    const auto afterRender = std::chrono::steady_clock::now();
    PublishRendererStats();
    const auto blockEnd = std::chrono::steady_clock::now();

    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    _rtLastBusyMs = ms(blockStart, blockEnd);
    if (Dev::FramePaceTraceEnabled())
    {
        // PACE-001: the worker's side of the frame. Acquire/submit/present are this frame's
        // CPU-side timers from the renderer (wgr_get_cpu_timings, read by PublishRendererStats
        // just above); the GPU total is the latest HARVESTED frame, which lags by one or two.
        static uint32_t seq = 0;
        Dev::FramePaceWorkerRow row;
        row.seq = ++seq;
        row.tEndMs = Dev::FramePaceNowMs();
        row.tStartMs = row.tEndMs - _rtLastBusyMs;
        row.idleBeforeMs = static_cast<float>(_rtLastIdleMs);
        row.renderMs = static_cast<float>(ms(afterHash, afterRender));
        {
            std::lock_guard<std::mutex> lock(_memStatsMutex);
            const auto cpuAt = [&](int i) { return _publishedCpuTimingCount > i ? _publishedCpuTimings[i] : -1.0f; };
            row.acquireMs = cpuAt(WGR_GPU_TIMER_WORKER_ACQUIRE);
            row.submitMs = cpuAt(WGR_GPU_TIMER_WORKER_SUBMIT);
            row.presentMs = cpuAt(WGR_GPU_TIMER_WORKER_PRESENT);
            row.gpuFrameMs = _publishedGpuTimingCount > WGR_GPU_TIMER_FRAME_TOTAL
                                 ? _publishedGpuTimings[WGR_GPU_TIMER_FRAME_TOTAL]
                                 : -1.0f;
        }
        Dev::FramePaceTraceWorker(row);
    }

    if (_slowBlockMs > 0.0 && _rtLastBusyMs >= _slowBlockMs)
    {
        // The op COUNTS as well as the times. A 300 ms resource drain of four ops and a
        // 300 ms drain of four thousand are different bugs, and the times alone cannot
        // tell them apart.
        LOG_INFO(Graphics,
                 "Wgpu slow block: total={:.1f} ms | settings={:.1f} resources={:.1f} (ops={}) "
                 "drains={:.1f} hash={:.1f} render={:.1f} stats={:.1f} | d3d={} vert={} casters={} | idle_before={:.1f}",
                 _rtLastBusyMs, ms(blockStart, afterSettings), ms(afterSettings, afterResources),
                 resourceOpCount, ms(afterResources, afterDrains), ms(afterDrains, afterHash),
                 ms(afterHash, afterRender), ms(afterRender, blockEnd), frame.draws3d.length,
                 frame.verts.length, frame.shadow_casters.length, _rtLastIdleMs);
    }
}

// Window of one second, so the readout is stable enough to read while the game runs.
void EngineWgpu::AccumulateRenderThreadTiming(double busyMs, double waitMs, double frameMs)
{
    _rtBusyMsAccum += busyMs;
    _rtWaitMsAccum += waitMs;
    _rtWindowMsAccum += frameMs;
    _rtWindowFrames++;
    // PERF-022: `frameMs` is NextFrame's own duration (~1 ms under overlap), not the frame's,
    // so a 1000 ms window of it never closed inside a 900-frame capture and every reader saw
    // the zeros the fields were initialised with. The window is wall time now.
    static auto windowStart = std::chrono::steady_clock::now();
    const auto nowWall = std::chrono::steady_clock::now();
    if (std::chrono::duration<double, std::milli>(nowWall - windowStart).count() < 1000.0)
        return;
    windowStart = nowWall;
    const double invFrames = 1.0 / static_cast<double>(_rtWindowFrames);
    _rtBusyMs = static_cast<float>(_rtBusyMsAccum * invFrames);
    _rtWaitMs = static_cast<float>(_rtWaitMsAccum * invFrames);
    const double window = _rtWindowMsAccum > 0.0 ? _rtWindowMsAccum : 1.0;
    _rtBusyFrac = static_cast<float>(_rtBusyMsAccum / window);
    // The producer's own busy time is the window minus what it spent BLOCKED on the worker.
    _rtMainFrac = static_cast<float>((_rtWindowMsAccum - _rtWaitMsAccum) / window);
    _rtLazyWaitMs = static_cast<float>(_rtLazyWaitMsAccum * invFrames);
    _rtLazyWaitsPerFrame = static_cast<float>(_rtLazyWaitCountAccum * invFrames);
    _rtLazyWaitMsAccum = 0.0;
    _rtLazyWaitCountAccum = 0;
    // PERF-021 follow-up: the capture JSON read these as 0 while the tab showed them; one
    // line a second says what this window actually computed.
    static uint32_t windows = 0;
    if ((windows++ % 10) == 0)
        LOG_INFO(Graphics,
                 "Wgpu render thread window: frames={} busy={:.2f} ms wait={:.2f} lazyWait={:.2f} ms x{:.2f}/frame",
                 _rtWindowFrames, _rtBusyMs, _rtWaitMs, _rtLazyWaitMs, _rtLazyWaitsPerFrame);
    _rtBusyMsAccum = _rtWaitMsAccum = _rtWindowMsAccum = 0.0;
    _rtWindowFrames = 0;
    _renderLagShown = static_cast<float>(_renderLagFrames);
}

Engine::RenderThreadInfo EngineWgpu::GetRenderThreadInfo() const
{
    RenderThreadInfo info;
    info.available = _renderer != nullptr;
    info.enabled = _renderThreadOn;
    info.workerBusyMs = _rtBusyMs;
    info.mainWaitMs = _rtWaitMs;
    // Worker time the producer did NOT wait for. Zero in lockstep by construction, which is
    // the honest reading: the mode exists to prove the mechanics, not to overlap.
    info.overlapMs = _rtBusyMs > _rtWaitMs ? _rtBusyMs - _rtWaitMs : 0.0f;
    info.workerBusyFrac = _rtBusyFrac;
    info.mainBusyFrac = _rtMainFrac;
    info.overlapAvailable = _renderThreadOn;
    info.overlapEnabled = _overlapOn;
    info.lagFrames = _renderLagShown;
    info.queueWaitMs = _rtWaitMs;
    info.lazyWaitMs = _rtLazyWaitMs;
    info.lazyWaitsPerFrame = _rtLazyWaitsPerFrame;
    info.lazyOpener = _rtLazyOpenerShown;
    return info;
}

// REN-THR-016: the producer's queues become the frame's. Moves, not copies -- the vectors
// keep their capacity and come back on the slot's next turn.
void EngineWgpu::MoveQueuesInto(FrameQueues& q)
{
    q.settings = _pendingSet;
    _pendingSet = PendingRendererSettings{};
    q.resourceOps.swap(_resourceOps);
    _resourceOps.clear();
    q.instanceOps.swap(_instanceOps);
    _instanceOps.clear();
    q.overlayTexOps.swap(_overlayTexOps);
    _overlayTexOps.clear();
    q.draws3dPendingMesh.swap(_draws3dPendingMesh);
    _draws3dPendingMesh.clear();
    q.castersPendingMesh.swap(_castersPendingMesh);
    _castersPendingMesh.clear();
}

// REN-THR-012. The queues carry mesh/model/texture/instance ops and the pending settings to
// the worker, but the producer still makes direct `wgr_*` calls every frame -- terrain
// params from DrawTerrain, render params and sky runtime from NextFrame, far-tier instances,
// texture slot leases during cold admission, the stats readers behind the dev panel -- and
// every one of them forms an exclusive reference to the renderer on the Rust side, exactly
// as wgr_render_frame does on the worker. Under overlap the producer used to wait only for
// its ring SLOT, so it made those calls while the worker was inside the previous frame.
//
// The fix is a boundary, not a lock: the producer waits for the worker to be idle at the
// top of InitDraw, so the simulation (which makes no direct calls; its mutations are the
// queued instance ops) still overlaps the render, and the draw traversal, which does, runs
// after it. What that costs is measured in REN-THR-012; what it buys is that the quiescent
// flag below is a rule the texture paths can check rather than a sentence in a header.
static std::atomic<bool> g_rendererQuiescent{true};

bool EngineWgpu::ProducerMayTouchRenderer()
{
    return g_rendererQuiescent.load(std::memory_order_acquire);
}

EngineWgpu* EngineWgpu::s_self = nullptr;

bool EngineWgpu::TryAcquireProducerWindow()
{
    EngineWgpu* self = s_self;
    // Model registrations queued during world admission still carry current upload
    // handles. Let the consumer bake their leases before replacing any image.
    if (!self || !self->_resourceOps.empty()) return false;
    if (ProducerMayTouchRenderer()) return true;
    {
        std::unique_lock<std::mutex> lock(self->_renderMutex, std::try_to_lock);
        if (!lock.owns_lock() || self->_renderHasWork || self->_renderBusySlot >= 0) return false;
        // Only this producer can publish another frame; idle remains idle on unlock.
    }
    self->WaitRenderIdle(); // now nonblocking; also applies pending producer settings
    return true;
}

void EngineWgpu::AcquireProducerWindow(const char* what)
{
    if (g_rendererQuiescent.load(std::memory_order_acquire))
        return;
    EngineWgpu* self = s_self;
    if (self == nullptr)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    self->WaitRenderIdle();
    const double lazyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    self->_rtLazyWaitMsAccum += lazyMs;
    self->_rtLazyWaitMsFrame += lazyMs;
    self->_rtLazyWaitCountAccum++;
    if (self->_rtLazyOpenerFrame == nullptr)
        self->_rtLazyOpenerFrame = what;
    // Named in the log too, so a run's log says who opened the window (first 20, then 1 in 600).
    static std::atomic<uint32_t> logged{0};
    const uint32_t n = logged.fetch_add(1, std::memory_order_relaxed);
    if (n < 20 || (n % 600) == 0)
        LOG_INFO(Graphics, "Wgpu producer window opened late by '{}' (wait {:.2f} ms, opening #{})", what,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), n + 1);
}

bool EngineWgpu::LastMemoryStats(WgrMemoryStats& out)
{
    EngineWgpu* self = s_self;
    if (self == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(self->_memStatsMutex);
    out = self->_lastMemStats;
    return self->_lastMemStatsValid;
}

void EngineWgpu::QueueTerrainParams(const WgrTerrainParams& p)
{
    _pendingSet.terrainParams = p;
    _pendingSet.terrainParamsDirty = true;
}

void EngineWgpu::QueueWaterParams(const WgrWaterParams& p)
{
    _pendingSet.waterParams = p;
    _pendingSet.waterParamsDirty = true;
}

void EngineWgpu::QueuePlanarReflectionPad(float pad)
{
    _pendingSet.planarReflectionPad = pad;
    _pendingSet.planarPadDirty = true;
}

void EngineWgpu::QueueWaterInteraction(const WgrWaterInteractionParams& p, const WgrWaterInteractionEvent* events,
                                       uint32_t count)
{
    _pendingSet.waterInteraction = p;
    _pendingSet.waterInteractionDirty = true;
    if (events != nullptr && count > 0)
        _pendingSet.waterEvents.insert(_pendingSet.waterEvents.end(), events, events + count);
}

void EngineWgpu::WaitRenderIdle()
{
    if (!_renderThreadOn)
    {
        g_rendererQuiescent.store(true, std::memory_order_release);
        ApplyPendingRendererSettings(_pendingSet);
        return;
    }
    const auto waitStart = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> lock(_renderMutex);
        _renderCv.wait(lock, [this] { return !_renderHasWork && _renderBusySlot < 0; });
    }
    _rtPreDrawWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    g_rendererQuiescent.store(true, std::memory_order_release);
    // REN-THR-013: the stop-the-world callers (MSAA, resize, screenshots) read the renderer
    // right after this; the queued per-frame pushes must already be in it.
    ApplyPendingRendererSettings(_pendingSet);
}

void EngineWgpu::RenderThreadMain()
{
    for (;;)
    {
        WgrFrame* frame = nullptr;
        PushFrameBacking* backing = nullptr;
        const auto idleStart = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lock(_renderMutex);
            _renderCv.wait(lock, [this] { return _renderHasWork || _renderQuit; });
            if (_renderQuit && !_renderHasWork)
                return;
            frame = _renderFrame;
            backing = _renderBacking;
            _renderBusySlot = _renderSubmitSlot;
            // CLAIMED, not finished. Clearing the flag here is what lets the producer build the
            // next frame while this one renders; clearing it after the render instead would make
            // the producer wait for the whole block and there would be no overlap at all.
            _renderHasWork = false;
            _renderFrame = nullptr;
            _renderBacking = nullptr;
        }
        _renderCv.notify_one();
        // PERF-021: how long the worker sat waiting for a frame before this block, so the
        // slow-block line can say whether the worker is starved or the bottleneck.
        _rtLastIdleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - idleStart).count();
        // The slot is the worker's for the duration: the producer waits before reusing it.
        if (frame && backing && _renderBusySlot >= 0)
            RunConsumerBlock(*frame, *backing, _queueRing[_renderBusySlot]);
        {
            std::lock_guard<std::mutex> lock(_renderMutex);
            _renderBusySlot = -1;
        }
        _renderCv.notify_one();
    }
}

void EngineWgpu::StartRenderThread()
{
    if (_renderThread.joinable())
        return;
    // CLEAR THE HANDOFF BEFORE SPAWNING. `_renderQuit` is set by StopRenderThread and was
    // never cleared here, so a worker started a second time saw "quit" on its first wait and
    // returned immediately -- leaving `_renderThread.joinable()` true, `_renderThreadOn` true
    // and nothing on the other end of the queue. Toggling the thread off and on again in the
    // dev panel then crashed. The rest of the handoff is reset for the same reason: a stop
    // that lands mid-frame can leave a slot marked busy and a stale frame pointer, and the
    // next start must not inherit either.
    {
        std::lock_guard<std::mutex> lock(_renderMutex);
        _renderQuit = false;
        _renderHasWork = false;
        _renderFrame = nullptr;
        _renderBacking = nullptr;
        _renderBusySlot = -1;
        _renderSubmitSlot = -1;
    }
    _renderThreadOn = true;
    _renderThread = std::thread([this] { RenderThreadMain(); });
    LOG_INFO(Graphics,
             "Wgpu render thread: ON (WGR_RENDER_THREAD). Lockstep -- the producer waits for "
             "each frame, so this is a stack change, not overlap.");
}

void EngineWgpu::StopRenderThread()
{
    if (!_renderThread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(_renderMutex);
        _renderQuit = true;
    }
    _renderCv.notify_all();
    _renderThread.join();
    _renderThreadOn = false;
    _overlapOn = false;
    g_rendererQuiescent.store(true, std::memory_order_release);
    // Hand the inline path a clean slate. After the join nobody else touches these, but a
    // stop that happened while a frame was in flight leaves `_renderBusySlot` set, and the
    // producer's slot wait would then block on a worker that no longer exists.
    {
        std::lock_guard<std::mutex> lock(_renderMutex);
        _renderQuit = false;
        _renderHasWork = false;
        _renderFrame = nullptr;
        _renderBacking = nullptr;
        _renderBusySlot = -1;
        _renderSubmitSlot = -1;
    }
    _renderCv.notify_all();
}

void EngineWgpu::NextFrame()
{
    // RFG-048: fire the interior-sky probe from a HEADLESS run.
    //
    // The measurement lives behind a dev-panel button, which a screenshot harness
    // cannot press -- so the one question it answers ("is the house even on the GPU
    // path?") was unanswerable without a person at the keyboard. Counting frames here
    // rather than seconds because the world streams in and an early probe measures the
    // loading screen, which is exactly the mistake this diagnostic was rebuilt to stop
    // making.
    {
        static const int probeFrame = []
        {
            const char* v = std::getenv("POSEIDON_INTERIOR_SKY_PROBE");
            return (v != nullptr && *v != 0) ? std::atoi(v) : 0;
        }();
        static int frames = 0;
        if (probeFrame > 0 && ++frames == probeFrame)
            ProbeInteriorSkyMap();
    }

    if (_renderer)
    {
        // REN-THR-015: apply a panel toggle HERE -- top of the frame, nothing in flight,
        // nobody holding the handle. Anywhere else would mean stopping a worker mid-frame.
        if (_renderThreadRequested != _renderThreadOn)
        {
            if (_renderThreadRequested)
                StartRenderThread();
            else
                StopRenderThread();
        }
        if (_overlapRequested != _overlapOn)
        {
            // Leaving overlap has to drain first: the producer is about to start waiting for
            // every frame again, and a worker still holding a slot would make the first such
            // wait a deadlock rather than a wait.
            if (_overlapOn)
            {
                std::unique_lock<std::mutex> lock(_renderMutex);
                _renderCv.wait(lock, [this] { return !_renderHasWork; });
            }
            _overlapOn = _renderThreadOn && _overlapRequested;
        }
        const auto frameStartForRt = std::chrono::steady_clock::now();
        double waitMs = _rtPreDrawWaitMs; // the InitDraw wait belongs to this frame
        _rtPreDrawWaitMs = 0.0;
        // PACE-001: the frame trace wants the waits SEPARATELY, which the accounting above
        // folds together. Lazy waits happened during the traversal, i.e. since the last NextFrame.
        const double preDrawWaitForTrace = waitMs;
        const double lazyWaitForTrace = _rtLazyWaitMsFrame;
        _rtLazyWaitMsFrame = 0.0;
        double slotWaitForTrace = 0.0;
        double lockstepWaitForTrace = 0.0;
        if (_rtLazyOpenerFrame != nullptr)
            _rtLazyOpenerShown = _rtLazyOpenerFrame;
        _rtLazyOpenerFrame = nullptr;
        SyncWaterLookProfile();
        const auto tLook = std::chrono::steady_clock::now();
        // Interpolate the per-time-of-day tonemap/grade + atmosphere presets for this frame
        // into _tonemap / _sky (auto mode only; manual override holds the tabs' values).
        UpdateAutoTonemap();
        UpdateAutoSky();
        UpdateSunGlareExposure();
        // Derives this frame's adaptation ease from the time constant (ExposureSettings::rateTau,
        // or the WGR_AUTO_EXPOSURE_TAU override). No-op only when that resolves to <= 0, which
        // restores the authored per-frame `rate`. Must run BEFORE PushRenderParams, which reads
        // the derived value.
        UpdateAutoExposureRate();
        // Push the consolidated look block (picks up the auto-ToD updates; renderer diffs the
        // terrain sub-blocks) and then the per-frame sky runtime (celestial + camera).
        PushRenderParams();
        PushSkyRuntime();

        // Build + flatten the ImGui frame; SubmitOverlay fills the _overlay*
        // vectors the WgrFrame below points at. Composites over everything,
        // same placement as GL33's BackToFront (pre-present).
        const auto tParams = std::chrono::steady_clock::now();
        Dev::DebugOverlay::NewFrame();
        Dev::DebugOverlay::Render();
        const auto tOverlay = std::chrono::steady_clock::now();

        static_assert(sizeof(CameraEntry::proj) == 64 && sizeof(CameraEntry::view) == 64,
                      "CameraEntry matrices must be 16 floats to match WgrCamera");
        static_assert(sizeof(WgrCamera::proj) == 64 && sizeof(WgrCamera::view) == 64,
                      "WgrCamera matrices must be 16 floats");
        const Color& fog = FogColor();
        // Distance fog for the 3D path, matching GL33's BuildFrameState: the
        // scene's fog range drives a linear fog factor per vertex. Packed into
        // each camera UBO (see WgrCamera). The 2D/sky path fogs separately via
        // per-vertex specular alpha.
        // RenderSnapshot S1: environment inputs come from the frame's captured block,
        // not live globals, from here to the end of NextFrame.
        const render::frame::PresentationSnapshot& snap = render::frame::GPresentationSnapshot();
        float fogStart = 0.0f, fogInvRange = 0.0f, fogEnabled = 0.0f;
        if (snap.hasScene)
        {
            fogStart = snap.fogMinRange;
            const float fogEnd = snap.fogMaxRange;
            fogInvRange = (fogEnd > fogStart) ? 1.0f / (fogEnd - fogStart) : 0.0f;
            // Zero weather fog means a clear view, not a mandatory cull-distance veil.
            fogEnabled = (!snap.hasLandscape || snap.weatherFog > 0.0f) ? 1.0f : 0.0f;
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
        if (snap.hasMainLight)
        {
            const Color accom = GetAccomodateEye();
            sunDiffuse = snap.sunDiffuse * accom;
            sunAmbient = snap.sunAmbient * accom;
            sunDir = snap.sunTravelDir;
        }
        sunDir.Normalize();

        // Sky-based scene lighting (HDR + procedural sky + toggle): replace the legacy sun/
        // ambient with atmosphere-derived radiance on the physical scale, flagged to the shaders
        // via sun_diffuse.w so they skip the sRGB decode and take the unified path. The values
        // can far exceed 1, so they go straight to floats (bypassing any Color clamp).
        float sunLin[3] = {sunDiffuse.R(), sunDiffuse.G(), sunDiffuse.B()};
        float ambLin[3] = {sunAmbient.R(), sunAmbient.G(), sunAmbient.B()};
        float skyLit = 0.0f;
        // "Is the SUN up" for the shaders, 0 at night .. 1 by day, published in sun_dir_world.w.
        // The shaders used to derive it from the peak of sun_diffuse, which the moon term below
        // is ADDED to -- so a full moon read as ~40% daylight and switched the legacy-emissive
        // fade and the foliage night floor back toward their daytime values. Computed from the
        // sun alone here, before the moon joins.
        float daylight = 1.0f - (snap.hasMainLight ? snap.nightEffect : 0.0f);
        if (_hdrEnabled && _sky.enabled && _sky.skyLighting && snap.hasMainLight)
        {
            const float camAlt = snap.hasCamera ? static_cast<float>(snap.cameraPos.Y()) : 0.0f;
            const Vector3 tr = AtmosphereSunTransmittance(_sky, camAlt, -snap.sunDirection);
            const float rad = _sky.sunIntensity * _sky.exposure;
            sunLin[0] = tr.X() * rad;
            sunLin[1] = tr.Y() * rad;
            sunLin[2] = tr.Z() * rad;
            {
                // The same (0.002, 0.04) ramp on the atmospheric sun the shaders had, so the
                // day/dusk look is unchanged; only the moon no longer counts.
                const float peak = std::max({sunLin[0], sunLin[1], sunLin[2]});
                const float t = std::clamp((peak - 0.002f) / (0.04f - 0.002f), 0.0f, 1.0f);
                daylight = t * t * (3.0f - 2.0f * t);
            }
            // Non-physical ambient fill (no GI yet, and none coming for a long while): the
            // engine's ToD-varying ambient used directly (gamma space, NOT sRGB-decoded — the
            // decode crushes midtones to ~6% of the sun and blackens interiors/shadowed faces),
            // scaled onto the sun's physical range. A generous, readable floor that still fades
            // toward night as the engine dims its ambient. Real sky irradiance replaces this later.
            const Color amb = snap.sunAmbient;
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
            const float moonAmount = snap.moonLightAmount;
            if (moonAmount > 0.0f && _sky.moonIntensity > 0.0f)
            {
                const Vector3 moonTr = AtmosphereSunTransmittance(_sky, camAlt, -snap.moonDirection);
                // moonIntensity is the full-moon irradiance as a FRACTION of the sun's
                // (default 1/400 — see Engine::SkySettings). moonAmount already carries
                // the phase falloff and the horizon ramps.
                // moonLightGain lifts the LIGHT only; the disc keeps moonIntensity (PushFrame's
                // moonDiscScale), so the ground can catch the moon without the disc blooming.
                static const float moonLightGainEnv = [] {
                    const char* v = std::getenv("WGR_MOON_LIGHT_GAIN");
                    return v ? static_cast<float>(std::atof(v)) : -1.0f;
                }();
                const float moonGain = moonLightGainEnv > 0.0f ? moonLightGainEnv : _sky.moonLightGain;
                const float moonRad = rad * _sky.moonIntensity * moonGain * moonAmount;
                // Blue bias, the Purkinje shift: it is not modelled anywhere downstream, so it
                // has to live in the light. Stronger than LightSun::MoonLightColor's because the
                // night tonemap key is warm and Takistan's tan ground measured (10.4, 7.8, 7.0)
                // under the previous 0.73/0.80/1.00 -- brown, not the "bluish light" the owner
                // asked for.
                sunLin[0] += moonTr.X() * moonRad * 0.50f;
                sunLin[1] += moonTr.Y() * moonRad * 0.66f;
                sunLin[2] += moonTr.Z() * moonRad * 1.10f;
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
        // RenderSnapshot S3: the selection walk moved into CapturePresentationSnapshot --
        // the world scores and sorts its own lights, this side only converts to the ABI
        // struct and applies its capacity clamp.
        //
        // WGR_MAX_ACTIVE_LIGHTS: the A/B lever behind the Forward+ decision (B.1).
        // 0 removes every local light from the flat loop, so the GPU frame delta on
        // a night scene IS the lighting cost the roadmap's gate asks for. Default is
        // the full fixed capacity -- today's behaviour.
        {
            static const size_t envCap = []
            {
                const char* v = std::getenv("WGR_MAX_ACTIVE_LIGHTS");
                if (!v)
                    return size_t(WGR_MAX_LIGHTS);
                return size_t(std::clamp<long>(std::strtol(v, nullptr, 10), 0, WGR_MAX_LIGHTS));
            }();
            const size_t selected = std::min(size_t(snap.lightCount), envCap);
            _lights.reserve(selected);
            // LGT-010: how many of this frame's lights may own a shadow layer. The snapshot
            // has already sorted by score (near/bright first), so taking the first spots we
            // meet takes the ones that matter -- and it is STABLE frame to frame for the
            // same reason the light list is.
            Poseidon::Dev::LocalShadowSettings& localShadow = Poseidon::Dev::GLocalShadowSettings();
            // LGT-015: maxLights counts LIGHTS; slots are views. A spot spends one, a point
            // spends six (its cube). Both budgets bind, so a frame gets whichever runs out
            // first -- which is what stops one point light near the camera from silently
            // consuming every view.
            const int localShadowWanted = localShadow.enabled ? std::max(localShadow.maxLights, 0) : 0;
            int localShadowSlots = 0;
            int localShadowLights = 0;
            for (size_t i = 0; i < selected; ++i)
            {
                const render::frame::PresentationSnapshot::LocalLight& src = snap.lights[i];
                WgrLight pl{};
                pl.pos = {src.pos.X(), src.pos.Y(), src.pos.Z(), src.startAtten};
                // LGT-011: the light's OWN cone, in the two alpha channels that were sent as
                // zero. The shader had 12 and 8 degrees hardcoded for every spot in the game,
                // so a headlight authored with a 36 degree beam lit a 12 degree pinhole --
                // which is why spot lights read as "they light almost nothing", and why a
                // shadow map sized to the real cone did not line up with the lit pool.
                // cos^2 rather than the angle: that is the form lights_contrib compares in.
                const float cosO = std::cos(std::clamp(src.coneOuter, 0.0f, 1.55f));
                pl.diffuse = {src.diffuse.R(), src.diffuse.G(), src.diffuse.B(),
                              src.coneOuter > 0.001f ? cosO * cosO : 0.0f};
                // LGT-017: ambient.w carried cos^2 of the AUTHORED INNER ANGLE, which the
                // beam no longer uses -- the flat top it described is exactly what made a
                // headlight read as a disc with an edge on a wall. The channel now carries
                // the beam's shaping exponent instead, so the profile is per light and the
                // 64-byte struct is untouched.
                pl.ambient = {src.ambient.R(), src.ambient.G(), src.ambient.B(),
                              Poseidon::Dev::GSpotBeam().shape};
                // LGT-010: dir.w is the light KIND, and it carries the shadow slot with it
                // rather than growing WgrLight -- that struct is size-asserted at 64 bytes
                // on both sides of the ABI, and the shader's only test is `> 0.5` for
                // "is a spot", which 2 + slot satisfies unchanged.
                //   0        = point, no shadow
                //   1        = spot, no shadow
                //   2 + slot = spot, shadowed by local view `slot`
                float kind = src.spot ? 1.0f : 0.0f;
                const int needSlots = src.spot ? 1 : 6;
                if (localShadowLights < localShadowWanted && localShadowSlots + needSlots <= kMaxLocalShadows)
                {
                    const int first = localShadowSlots;
                    localShadowSlots += needSlots;
                    ++localShadowLights;
                    // The encoding, and why a point cannot share the spot's. dir.w > 0.5 is
                    // what MAKES a light a spot in lights_contrib, so a point light could not
                    // be given a positive slot without turning into a cone -- which is exactly
                    // the regression LGT-014 undid. Points go negative instead.
                    //   0          point, no shadow
                    //   1          spot, no shadow
                    //   2 + slot   spot shadowed by view `slot`
                    //   -(1+first) point shadowed by the cube in views first..first+5
                    kind = src.spot ? 2.0f + static_cast<float>(first) : -(1.0f + static_cast<float>(first));
                    for (int f = 0; f < needSlots; ++f)
                    {
                        LocalShadowLight& L = _localShadowLights[first + f];
                        L.pos[0] = src.pos.X(), L.pos[1] = src.pos.Y(), L.pos[2] = src.pos.Z();
                        L.dir[0] = src.dir.X(), L.dir[1] = src.dir.Y(), L.dir[2] = src.dir.Z();
                        L.coneOuter = src.coneOuter;
                        L.startAtten = src.startAtten;
                        L.endAttenScale = src.spot ? 10.0f : src.endAttenScale;
                        L.cubeFace = src.spot ? -1 : f;
                    }
                }
                // LGT-014: a POINT light's dir.xyz is unused by the shader, so the reach
                // multiplier rides in dir.x rather than growing the 64-byte light struct.
                // A spot keeps its axis there and keeps the historical 10x reach.
                pl.dir = src.spot ? WgrVec4{src.dir.X(), src.dir.Y(), src.dir.Z(), kind}
                                  : WgrVec4{src.endAttenScale, 0.0f, 0.0f, kind};
                _lights.push_back(pl);
            }
            _localShadowCount = localShadowSlots;
            if (std::getenv("WGR_LOCAL_REACH_TRACE"))
            {
                static unsigned int traceFrame = 0;
                if ((traceFrame++ % 120) == 0)
                    for (int k = 0; k < _localShadowCount; ++k)
                        if (_localShadowLights[k].cubeFace <= 0)
                            LOG_INFO(Graphics, "Local reach: slot={} start={} endScale={} shadow={} pos=({},{},{})",
                                     k, _localShadowLights[k].startAtten, _localShadowLights[k].endAttenScale,
                                     LocalShadowReachFor(k), _localShadowLights[k].pos[0],
                                     _localShadowLights[k].pos[1], _localShadowLights[k].pos[2]);
            }
            localShadow.candidatesSeen += static_cast<long long>(snap.lightCandidates);
            localShadow.lightsShadowed += localShadowSlots;
            if (localShadowSlots > 0)
            {
                ++localShadow.framesWithShadow;
            }
            {
                // A gauge that can say "the pass ran and nothing qualified" separately from
                // "the pass never ran" -- the trap this project keeps rediscovering.
                // Keyed on the SPOT count as well as the slot count. Keying on slots alone
                // said "0 of 0" once and then stayed silent while the world filled with
                // lights -- so it could not tell "no light qualified" from "no light exists",
                // which is the whole reason this gauge is here.
                int spots = 0;
                for (size_t j = 0; j < selected; ++j)
                {
                    if (snap.lights[j].spot)
                        ++spots;
                }
                static int saidCount = -1;
                static int saidSpots = -1;
                static bool saidEnabled = false;
                if (saidCount != localShadowSlots || saidSpots != spots || saidEnabled != localShadow.enabled)
                {
                    saidCount = localShadowSlots;
                    saidSpots = spots;
                    saidEnabled = localShadow.enabled;
                    LOG_INFO(Graphics,
                             "LGT-010 local shadows: {} -- {} of {} lights in the frame are spots, {} own a "
                             "shadow view (want {}, {} candidates, nightEffect {:.3f}, sunY {:.3f}, sceneLights {})",
                             localShadow.enabled ? "ON" : "off", spots, selected, localShadowSlots,
                             localShadowWanted, snap.lightCandidates, snap.nightEffect, snap.sunDirection.Y(),
                             GScene != nullptr ? GScene->NLights() : -1);
                }
            }
            Poseidon::Dev::GLightCounters().candidates = snap.lightCandidates;
            Poseidon::Dev::GLightCounters().cap = static_cast<uint32_t>(envCap);
            if (!_lights.empty() && std::getenv("WGR_STATIC_LIGHT_TRACE"))
                LOG_INFO(Graphics, "Wgpu local-light selection: candidates={} selected={} night={:.3f}",
                         snap.lightCandidates, selected, snap.nightEffect);
        }
        // Daytime frames skip the selection walk entirely; the gauges must still say so
        // rather than exhibit last night's numbers.
        Poseidon::Dev::GLightCounters().selected = static_cast<uint32_t>(_lights.size());
        if (_lights.empty())
            Poseidon::Dev::GLightCounters().candidates = 0;
        const float lightCount = static_cast<float>(_lights.size());

        // Cloud cover fades the cascade shadow toward none (in lockstep with the directional-sun
        // dimming above), so a fully overcast sky casts no sharp sun shadows. (Terrain self-shadow
        // and the froxel aerial shadow are separate paths — a fuller pass is deferred.)
        const float shadowStrength = (GetShadowFactor() / 256.0f) * (1.0f - cloudDim);
        // TWO independent caster sets feed the one cascade depth map, and EITHER is enough:
        //
        //  - `_shadowCasters`, submitted from C++ (AddShadowCaster, below at :4974).
        //  - the GPU-driven retained set, which casts ITSELF in Rust — `draw_gpu_driven_shadow`
        //    (gfx3d/mod.rs:6961), called once per cascade from inside that cascade's already-open
        //    depth pass (mod.rs:4689).
        //
        // The gate was `_smCascadesValid && !_shadowCasters.empty()`, and that is the bug. This
        // boolean does not merely skip a caster upload: it also decides whether `cameras[i].shadow`
        // is filled below, which is the ONLY way the colour pass learns the cascade matrices. So
        // an empty CPU caster list does not degrade to "GPU casters only" — it turns the entire
        // cascade system off for every receiver in the frame, discarding shadows that the retained
        // set had rendered perfectly well. The Rust side is already written for exactly this case
        // and has been since the §6 multi-view slice: `prepare_shadows` (mod.rs:4397-4431) sets up
        // the shadow target and pass UBO whenever GPU-driven is on and skips ONLY the CPU
        // bucketing when `casters.is_empty()`. Rust was ready; C++ was never sending the frame.
        //
        // The list empties in ordinary play, not in some contrived corner:
        // SceneShadowPass.cpp:540 deliberately `continue`s on every `GpuDrawCoverage::Full` object
        // (the retained set casts its own), and `Scene::ObjectForDrawing` diverts Full objects out
        // of `_drawMergers` upstream, so a view whose visible set is ENTIRELY retained submits zero
        // CPU casters. An interior does that (the building is the visible set, and a building whose
        // proxies all moved to the GPU is Full — SceneObjectCreated at :4752). So does any Reforger
        // world, where the .emat adapter leaves `pixelShaderId` empty and the foliage bail at :4051
        // therefore never fires, letting ALL vegetation register as well. That is the owner's
        // report — sun on an interior hall floor, walls not casting — and it does not need
        // WGR_TREEADV_RETAINED to be on to happen.
        //
        // `_gpuDriven && !_gpuInstances.empty()` is the honest C++-visible form of "the retained
        // set can cast". C++ cannot know how many instances survive a given cascade's cull — that
        // is decided by the cull compute on the GPU — and it does not need to: a non-empty retained
        // set is precisely the condition under which draw_gpu_driven_shadow can produce anything,
        // and the two failure modes are not symmetric. Too eager costs one depth pass over an empty
        // indirect arg list; too strict costs every shadow in the frame. `_gpuDriven` mirrors the
        // `gpu_driven_enabled` flag `draw_gpu_driven_depth` tests first (mod.rs:6991), so the two
        // sides cannot disagree about whether the path exists.
        //
        // WGR_SHADOW_LEGACY_GATE=1 restores the old gate (and the old `_smCamPos` handling with it).
        const bool gpuDrivenShadowCasters = _gpuDriven && !_gpuInstances.empty();
        // GRASS is a third independent caster set, and the Rust side already knew that: its own
        // gate is `casters.is_empty() && !gpu_shadows && !grass.casts_shadows()` (gfx3d/mod.rs:4404).
        // Mirroring only the gpu_shadows term left a frame with no CPU casters and an empty
        // retained set still losing grass shadows, purely because C++ declined to send the frame
        // that Rust was ready to render. Same defect as the main one, one set over.
        const bool grassShadowCasters = _grass.castShadows && !_grassBatches.empty();
        // WGR_SHADOW_FORCE_NO_CPU_CASTERS=1 — a DIAGNOSTIC that empties the CPU caster list right
        // before the gate reads it.
        //
        // It exists because the natural experiment could not be run. The claim behind this whole
        // fix is "a view whose visible set is entirely retained submits zero CPU casters, and that
        // silently disables every cascade". Four A/B arms on Takistan and Everon measured
        // cpuCasters at 7 and 5 — never 0 — so an ordinary exterior always keeps a handful of
        // non-retained objects alive and the old gate stayed open. The bug never reproduced, which
        // left the fix unverified rather than confirmed (see SHD-001).
        //
        // Hunting for a pose that empties the list naturally (an interior) is expensive and
        // uncertain. Forcing the CONSEQUENCE is cheap and tests the actual claim: with the list
        // emptied, the legacy gate must lose every shadow in the frame and the fixed gate must
        // keep them via draw_gpu_driven_shadow. That is a direct test of the mechanism rather than
        // a search for a view that exhibits it.
        //
        // Diagnostic only: it throws away real casters, so anything not retained stops casting.
        // Never a default, and never a performance arm.
        if (ShadowForceNoCpuCastersEnabled())
        {
            _shadowCasters.clear();
            _castersPendingMesh.clear(); // indices into it (REN-THR-011)
        }
        // LGT-010: the shadow pass is the SUN's pass, and at night there is no sun -- the
        // cascades are not built, `_smCascadesValid` is false, and the whole pass is skipped.
        // Local light shadows live in the same depth array and the same pass, so they were
        // silently impossible for exactly the hours they exist for. This is what that cost:
        // the depth side never learned the local view count, no layer was allocated, and the
        // shader sampled a layer that does not exist -- which is not "no shadow", it is
        // whatever a sampler returns for an out-of-range layer, and it read as grain.
        //
        // So the pass runs when EITHER family has work. The sun's own cascade count is
        // separately zeroed below when its cascades are invalid, so a night frame draws the
        // local views and no sun cascades rather than sun cascades with stale matrices.
        const bool localShadowsWanted = _localShadowCount > 0;
        const bool sunShadowActive =
            _smCascadesValid && (!_shadowCasters.empty() ||
                                 (!ShadowLegacyGateEnabled() && (gpuDrivenShadowCasters || grassShadowCasters)));
        const bool shadowActive = sunShadowActive || localShadowsWanted;

        // WGR_SHADOW_GATE_TRACE=1 — the one measurement that decides whether the gate above was
        // ever the owner's bug. Everything in the comment block is static reading; nobody has
        // observed `_shadowCasters.size()` in the failing shot.
        //
        // The distinction this has to resolve is sharp, and it is why the sizes are logged rather
        // than just the verdict: the old gate is a KNIFE-EDGE at exactly zero CPU casters, not a
        // slope. One surviving road, corpse or unit anywhere in view flips it back on and every
        // building casts normally. So "buildings never cast anywhere, in a scene that still has
        // other shadows" would NOT be explained by this gate, and seeing casters=3 in the failing
        // shot refutes the whole theory and moves the fault into the GPU-driven cascade path.
        // A verdict-only log could not tell those two apart.
        //
        // Logged on TRANSITION, not per frame: the interesting event is the flip (and a per-frame
        // line at 40 fps buries it). `legacy` is echoed so a capture's log states which arm
        // produced it rather than relying on the meta.json env block being read correctly.
        if (ShadowGateTraceEnabled())
        {
            static int lastTraced = -1;
            const int state = shadowActive ? 1 : 0;
            if (state != lastTraced)
            {
                lastTraced = state;
                LOG_INFO(Graphics,
                         "Wgpu shadow gate: active={} cpuCasters={} gpuInstances={} grassBatches={} "
                         "cascadesValid={} gpuDriven={} legacy={} forcedNoCpu={}",
                         shadowActive, _shadowCasters.size(), _gpuInstances.size(), _grassBatches.size(),
                         _smCascadesValid, _gpuDriven, ShadowLegacyGateEnabled(),
                         ShadowForceNoCpuCastersEnabled());
            }
        }
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
            // .w = the sun-only daylight factor (see `daylight` above).
            cameras[i].sun_dir_world = {sunDir.X(), sunDir.Y(), sunDir.Z(), daylight};
            if (shadowActive)
            {
                WgrCameraShadow& sb = cameras[i].shadow;
                for (int c = 0; c < _smCascades.count && c < 4; c++)
                {
                    std::memcpy(sb.cascade_vp[c].m, _smCascades.camRelVP[c].m.data(), sizeof(sb.cascade_vp[c].m));
                }
                sb.splits = {_smCascades.splitViewDist[0], _smCascades.splitViewDist[1], _smCascades.splitViewDist[2],
                             _smCascades.splitViewDist[3]};
                // RFG-077: say where the object cascades end, once per change. "Are there
                // tree shadows on that hillside" has been answered from the default
                // (1200 m) and from a grass-cascade line that is not this; the number that
                // decides it is the last split the terrain and objects actually sample.
                {
                    static float saidSplits[4] = {-1.f, -1.f, -1.f, -1.f};
                    const float* sv = _smCascades.splitViewDist;
                    if (saidSplits[0] != sv[0] || saidSplits[1] != sv[1] || saidSplits[2] != sv[2] || saidSplits[3] != sv[3])
                    {
                        for (int k = 0; k < 4; ++k)
                            saidSplits[k] = sv[k];
                        LOG_INFO(Graphics, "Wgpu object shadow cascades: splits {:.0f} / {:.0f} / {:.0f} / {:.0f} m",
                                 sv[0], sv[1], sv[2], sv[3]);
                    }
                }
                sb.omni_radius = {_smCascades.omniRadius[0], _smCascades.omniRadius[1], _smCascades.omniRadius[2],
                                  _smCascades.omniRadius[3]};
                // LGT-010: the SUN's cascade count for the SAMPLING side must be zero on a
                // frame where the sun has no valid cascades. Otherwise shadow_strength keeps
                // reading cascade layer 0 -- which is exactly the layer a local light's view
                // now renders into on a night frame -- and every surface in the world came
                // back occluded. That is what turned the torch test from "grain" into "the
                // whole lit region is black": not the local shadow at all, the SUN's,
                // sampling the light's map by mistake.
                sb.ctl = {static_cast<float>(sunShadowActive ? _smCascades.count : 0),
                          static_cast<float>(_smCascades.omniCount), _smTuning.fadeRange, _smTuning.biasBase};
                sb.ctlb = {1.0f / static_cast<float>(_smCascadeRes), darkness, _smTuning.normalOffset, _smTuning.pcf};
                sb.cam_fwd = {_cameras[i].dir[0], _cameras[i].dir[1], _cameras[i].dir[2],
                              static_cast<float>(std::clamp(_smTuning.contactShadows, 0, 2))};
                sb.sun_dir = {_smCascades.sunDir.x, _smCascades.sunDir.y, _smCascades.sunDir.z,
                              std::tan(std::clamp(_smTuning.sunAngularRadius, 0.0f, 8.0f) * (3.14159265f / 180.0f))};
                // LGT-010: one perspective view per shadowing spot. The light list was built
                // for THIS frame just above, so `_localShadowCount` is already final here.
                {
                    const Poseidon::Dev::LocalShadowSettings& ls = Poseidon::Dev::GLocalShadowSettings();
                    // LGT-025. This is inside the per-CAMERA loop, and it used to build every
                    // camera's lookup matrices around `_smCamPos` -- the origin the depth pass
                    // and the casters use. For the main view those coincide and it worked. For
                    // any SECOND camera in the frame they do not, and a receiver that samples a
                    // matrix built around a different origin misses by the delta between them:
                    // not a small error, a total one, so every local light reads as fully
                    // occluded and the lit world goes black. That is the owner's "es
                    // funktioniert nicht immer", and his own guess at the trigger was exactly
                    // right -- "der spieler unten ist aber freefly oben" is two cameras.
                    // The absolute projection is identical either way; only the basis it is
                    // written in changes, so the depth pass keeps _smCamPos below and each
                    // camera gets its own here.
                    const float* localShadowBasis = _cameras[i].pos;
                    static bool reportedBasisMismatch = false;
                    if (Poseidon::shadow::LocalShadowBasisMismatch(
                            localShadowBasis, cameras[i].cam_pos.x, cameras[i].cam_pos.y,
                            cameras[i].cam_pos.z, _localShadowCount) && !reportedBasisMismatch)
                    {
                        reportedBasisMismatch = true;
                        LOG_ERROR(Graphics,
                                  "LGT-025: camera {} local-shadow lookup basis mismatch: "
                                  "camera=[{},{},{}] basis=[{},{},{}] views={}",
                                  i, cameras[i].cam_pos.x, cameras[i].cam_pos.y, cameras[i].cam_pos.z,
                                  localShadowBasis[0], localShadowBasis[1], localShadowBasis[2],
                                  _localShadowCount);
                    }
                    for (int k = 0; k < _localShadowCount && k < kMaxLocalShadows; k++)
                    {
                        const Poseidon::shadow::Mat4 vp = LocalShadowVP(k, localShadowBasis);
                        std::memcpy(sb.local_vp[k].m, vp.m.data(), sizeof(sb.local_vp[k].m));
                    }
                    // {count, darkness, texel_size, first_layer}. first_layer is where the
                    // local views begin in the shared depth array, i.e. after the cascades.
                    // first_layer MUST be the same number the depth pass uses, and that is
                    // `frame.shadow.count` -- which is ZERO on a night frame, because the sun
                    // has no cascades then. Using _smCascades.count here instead sent the
                    // shader to layer 4 while the renderer had put the light's map in layer 0,
                    // and sampling a layer that does not exist reads as "occluded everywhere".
                    // One number, one source.
                    sb.local_ctl = {static_cast<float>(_localShadowCount), ls.darkness,
                                    1.0f / static_cast<float>(_smCascadeRes > 0 ? _smCascadeRes
                                                                              : std::max(_smTuning.resolution, 1024)),
                                    static_cast<float>(sunShadowActive ? _smCascades.count : 0)};
                }
            }
        }

        // PUBLISH: hand the frame's storage to a ring slot before describing it.
        //
        // `WgrFrame` is slices -- pointer and length, non-owning -- so whatever it points at
        // must outlive the consumer's use of it and must not be written meanwhile. Today the
        // consumer is `wgr_render_frame` on this same thread, so pointing at the members
        // works; the moment a render thread exists it does not, and `cameras` fails first
        // because it is a LOCAL that dies when this function returns.
        //
        // Swapping costs nothing: the producer gets back the slot's vectors, which still own
        // their capacity, and refills them next frame. See PushFrameBacking for why the ring
        // is two deep.
        // Roll the mutation counters at the publish point, because that is where the
        // producer/consumer cut will be: everything counted here happened while the
        // PREVIOUS frame was the renderer's, which is precisely the traffic a queue has to
        // carry once the two stop running in lockstep.
        _opsTotal.Add(_ops);
        _opsPeak.Max(_ops);
        _opsFrames++;
        _ops = MutationOps{};

        const int publishSlot = _pushSlot;
        if (_renderThreadOn && _overlapOn)
        {
            // The producer may run at most one frame ahead: it must not swap into a slot the
            // worker still holds. Blocking here IS "the renderer cannot keep up", so it is
            // timed and shown rather than hidden.
            const auto slotWaitStart = std::chrono::steady_clock::now();
            {
                std::unique_lock<std::mutex> lock(_renderMutex);
                _renderLagFrames = _renderBusySlot >= 0 ? 1u : 0u;
                // TWO conditions, and the second one is easy to miss. The slot must be free --
                // that is the ring. But the SUBMISSION must also have been claimed: _renderFrame
                // and _renderSubmitSlot are single variables, so publishing twice before the
                // worker wakes overwrites the first frame and it is never drawn. Nothing reports
                // that; the frames simply are not there. It cost 50% of the frames under overlap
                // and was invisible until two runs were aligned by publication tick, because
                // every frame that DID survive was byte-identical to the inline path.
                _renderCv.wait(lock, [this, publishSlot]
                               { return !_renderHasWork && _renderBusySlot != publishSlot; });
            }
            slotWaitForTrace =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - slotWaitStart).count();
            waitMs += slotWaitForTrace;
        }
        PushFrameBacking& backing = _pushRing[publishSlot];
        _pushSlot = (_pushSlot + 1) % kPushFrameRing;
        backing.totalTicks = Dev::LastFixedStepFrame().totalTicks;
        backing.steps = static_cast<std::uint32_t>(Dev::LastFixedStepFrame().steps);
        backing.verts.swap(_verts);
        backing.batches.swap(_batches);
        backing.draws3d.swap(_draws3d);
        backing.cmds.swap(_cmds);
        backing.palette.swap(_palette);
        backing.lights.swap(_lights);
        backing.shadowCasters.swap(_shadowCasters);
        backing.overlayVerts.swap(_overlayVerts);
        backing.overlayIndices.swap(_overlayIndices);
        backing.overlayDraws.swap(_overlayDraws);
        backing.terrainNodes.swap(_terrainNodes);
        backing.terrainBatches.swap(_terrainBatches);
        backing.waterNodes.swap(_waterNodes);
        backing.waterBatches.swap(_waterBatches);
        backing.grassBatches.swap(_grassBatches);
        backing.cameras = std::move(cameras);
        // What the producer knew about the retained set when it built this frame. The
        // consumer must never ask `_gpuInstances` itself -- see PushFrameBacking.
        backing.retainedInstances = static_cast<uint32_t>(_gpuInstances.size());

        // Lives in the ring slot, not on this stack: with overlap the producer returns
        // from NextFrame while the worker is still reading it.
        WgrFrame& frame = _queueRing[publishSlot].published;
        frame = WgrFrame{};
        frame.clear = {_clear[0], _clear[1], _clear[2], _clear[3]};
        frame.fog_color = {fog.R(), fog.G(), fog.B()};
        frame.cameras = backing.cameras;
        frame.draws3d = backing.draws3d;
        frame.verts = backing.verts;
        frame.batches = backing.batches;
        frame.cmds = backing.cmds;
        frame.palette = backing.palette;
        if (shadowActive)
        {
            // Zero when the sun has no valid cascades: the local views must not drag stale
            // cascade matrices into the pass with them.
            frame.shadow.count = sunShadowActive ? U32(_smCascades.count) : 0u;
            frame.shadow.omni_count = U32(_smCascades.omniCount);
            // `_smCascadeRes` is written only by SetShadowCascades, which does not run at
            // night -- so it is 0 then, and `pass.resolution == 0` made prepare_shadows
            // return before it could allocate a single layer. That is the LAST of the gates
            // this feature had to pass through, and like the others it exists because the
            // shadow pass was only ever the sun's.
            const int shadowRes = _smCascadeRes > 0 ? _smCascadeRes : std::max(_smTuning.resolution, 1024);
            frame.shadow.resolution = U32(shadowRes);
            // Which cascades can actually receive grass-shadow fragments.
            //
            // Grass::draw_shadow submits the near-ring blades into whatever pass it is handed,
            // so with 4 cascades the blades are transformed four times -- and the far cascades
            // then clip every one of them away, because the near ring stops at `radius` metres
            // and their depth range starts past it. Measured on perf_abel @1920x1080, the four
            // cascades cost 4.443 ms with grass and 0.121 ms without; the cost is flat in
            // shadow-map resolution (2048 vs 512) and in caster triangles (1.4k vs 89k) but
            // linear in cascade count, which is what wasted vertex work looks like.
            //
            // CONSERVATIVE: a bit is cleared only where the cascade provably cannot receive a
            // fragment -- a frustum tier whose NEAR distance is already past the reach. Omni
            // tiers are camera-centred spheres covering 0..radius, so they always keep grass.
            // Anything uncertain keeps its bit, so this cannot change what is drawn.
            uint32_t grassMask = 0xFu;
            float reachLogged = 0.0f;
            if (_grass.enabled && _grass.castShadows)
            {
                // The mid ring casts only on the photo-tuft path; otherwise the near ring is
                // the whole of the grass shadow caster set.
                float reach = _grassPhotoTuftActive ? std::max(_grass.radius, _grass.midRadius)
                                                    : _grass.radius;
                // SUN-ANGLE MARGIN. A blade standing at the edge of the ring casts a shadow that
                // lands FURTHER than the ring, and at a low sun it lands much further: the
                // horizontal run is height / tan(elevation), which at 5 degrees is ~11x the blade
                // height. Cutting the cascade at the ring radius alone would drop those shadows at
                // dawn and dusk while looking perfect at noon -- the worst kind of regression,
                // because it is invisible in every screenshot taken at midday.
                //
                // sunDir is normalised, so |y| is sin(elevation) and sqrt(1-y^2) is cos.
                const float sy = std::fabs(_smCascades.sunDir.y);
                const float horiz = std::sqrt(std::max(0.0f, 1.0f - sy * sy));
                // Below this the sun is at the horizon, shadows are effectively infinite and the
                // terrain shadow pass has taken over anyway; keep every cascade rather than guess.
                constexpr float kMinSin = 0.02f;
                const float bladeH = std::max(_grass.height, 0.1f);
                reach += (sy > kMinSin) ? std::min(bladeH * horiz / sy, 200.0f) : 1e9f;
                reachLogged = reach;
                float nearDist = 0.0f;
                for (int c = 0; c < _smCascades.count && c < 4; c++)
                {
                    const bool omni = c < _smCascades.omniCount || _smCascades.omniRadius[c] > 0.0f;
                    if (!omni && nearDist >= reach)
                        grassMask &= ~(1u << c);
                    nearDist = _smCascades.splitViewDist[c];
                }
            }
            else
            {
                grassMask = 0u; // grass casts nothing at all; save the submissions outright
            }
            frame.shadow.grass_cascade_mask = grassMask;
            {
                static uint32_t lastMask = 0xFFFFFFFFu;
                if (grassMask != lastMask)
                {
                    lastMask = grassMask;
                    LOG_INFO(Graphics, "Wgpu grass shadow cascades: mask=0x{:X} (reach {:.1f} m, {} cascades)",
                             grassMask, reachLogged, _smCascades.count);
                }
            }
            for (int c = 0; c < _smCascades.count && c < 4; c++)
            {
                std::memcpy(frame.shadow.light_vp[c].m, _smCascades.camRelVP[c].m.data(),
                            sizeof(frame.shadow.light_vp[c].m));
            }
            // Casters are camera-relative to the camera captured in AddShadowCaster (NOT
            // _currentCamera, which may have advanced to a HUD/weapon camera by now); the
            // depth shader adds cam_pos back to sample the terrain conform at the right xz.
            frame.shadow.cam_pos = {_smCamPos[0], _smCamPos[1], _smCamPos[2], 0.0f};
            // LGT-010: the same spot matrices the camera block got, handed to the DEPTH pass
            // so it knows where to render from. Recomputed rather than copied out of a camera:
            // `cameras[i]` is a per-view array and the shadow pass is one per frame, so
            // reaching into it would silently pick whichever view happened to be last.
            {
                const Poseidon::Dev::LocalShadowSettings& ls = Poseidon::Dev::GLocalShadowSettings();
                frame.shadow.local_count = U32(_localShadowCount);
                {
                    // Does the DEPTH side get the same count the sampling side did? A layer
                    // that was never allocated reads back as a failure, and a shader that
                    // samples a missing layer returns something -- which is indistinguishable
                    // from a wrong shadow.
                    static int said = -1;
                    if (said != _localShadowCount)
                    {
                        said = _localShadowCount;
                        LOG_INFO(Graphics, "LGT-010 depth pass: local_count {} cascades {}", _localShadowCount,
                                 _smCascades.count);
                    }
                }
                for (int k = 0; k < _localShadowCount && k < kMaxLocalShadows; k++)
                {
                    // LGT-025: the DEPTH pass renders casters that were made relative to
                    // _smCamPos, so its matrices stay in that basis. See the per-camera site.
                    const Poseidon::shadow::Mat4 vp = LocalShadowVP(k, _smCamPos);
                    std::memcpy(frame.shadow.local_vp[k].m, vp.m.data(), sizeof(frame.shadow.local_vp[k].m));
                }
                // LGT-026: the cache switches, and this frame's per-light verdict. Everything
                // here is ABSOLUTE and camera-independent on purpose -- the camera moving is
                // NOT an invalidation of the depth MAP. `local_vp[k]` above is camera-relative,
                // so it changes whenever the camera does, but the depth pass composes it with
                // caster worlds expressed in the SAME origin and the two camera terms cancel:
                // the depth written for a given absolute point is the same whatever origin it
                // was written in. What genuinely IS camera-relative is the per-camera LOOKUP
                // matrix a few hundred lines up, and that is rebuilt from scratch every frame
                // regardless, being a matrix multiply and not a depth pass. Hashing local_vp
                // here would have made every frame with a moving camera a full re-render, which
                // is the one case a village most needs the cache for.
                uint32_t flags = 0u;
                if (ls.cacheStatic)
                    flags |= 1u; // WgrShadowPass::local_flags bit 0
                if (ls.forceRefresh)
                    flags |= 2u; // bit 1 -- re-render everything this frame
                frame.shadow.local_flags = flags;
                uint32_t dirty = 0u;
                for (int k = 0; k < kMaxLocalShadows; k++)
                {
                    const uint64_t key = (k < _localShadowCount) ? LocalShadowLightKey(k) : 0ULL;
                    if (key != _localShadowKey[k])
                    {
                        // A slot whose light changed at all -- moved, re-aimed, re-ranged, or
                        // simply occupied by a DIFFERENT light after the budget re-sorted.
                        dirty |= (1u << k);
                        _localShadowKey[k] = key;
                    }
                }
                frame.shadow.local_dirty_mask = dirty;
            }
            frame.shadow_casters = backing.shadowCasters;
        }
        frame.overlay_verts = backing.overlayVerts;
        frame.overlay_indices = backing.overlayIndices;
        frame.overlay_draws = backing.overlayDraws;
        frame.terrain_nodes = backing.terrainNodes;
        frame.terrain_batches = backing.terrainBatches;
        frame.lights = backing.lights;
        frame.water_nodes = backing.waterNodes;
        frame.water_batches = backing.waterBatches;
        frame.grass_batches = backing.grassBatches;
        // REN-THR-013: THE CONSUMER BLOCK. Everything from here to PublishRendererStats
        // touches the renderer handle and nothing else does; this is the span a render
        // thread will own, and it is placed here rather than at the top of NextFrame so
        // that moving it needs no reordering.
        //
        // Order inside it is load-bearing. Settings first (they configure what the draws
        // will do). Then resources before instances, because a queued instance add may name
        // a model whose queued registration precedes it, and that is the only cross-queue
        // dependency the producer can create. Then the overlay textures, and only then the
        // translation of the overlay draws that refer to them.
        // BOTH paths, not just the threaded one. The drains read the FRAME's queues now, so
        // an inline frame whose queues were never moved drains empty vectors and publishes a
        // frame with none of its settings, instances or overlay ops applied -- which is what
        // the first version did, and the equivalence gate caught it as `bat` differing in
        // every frame between the inline and threaded paths.
        MoveQueuesInto(_queueRing[publishSlot]);
        if (_renderThreadOn)
        {
            {
                std::lock_guard<std::mutex> lock(_renderMutex);
                _renderFrame = &frame;
                _renderBacking = &backing;
                _renderSubmitSlot = publishSlot;
                _renderHasWork = true;
            }
            g_rendererQuiescent.store(false, std::memory_order_release);
            _renderCv.notify_one();
            if (!_overlapOn)
            {
                // LOCKSTEP: wait for the whole block. Overlap measures zero by construction,
                // which is the point -- the mode proves the mechanics, it does not hide work.
                const auto waitStart = std::chrono::steady_clock::now();
                {
                    std::unique_lock<std::mutex> lock(_renderMutex);
                    // Finished, not merely claimed -- lockstep means the producer owns the
                    // renderer again before it returns.
                    _renderCv.wait(lock, [this] { return !_renderHasWork && _renderBusySlot < 0; });
                }
                g_rendererQuiescent.store(true, std::memory_order_release);
                waitMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
                lockstepWaitForTrace = waitMs;
            }
        }
        else
        {
            RunConsumerBlock(frame, backing, _queueRing[publishSlot]);
        }
        const auto tEnd = std::chrono::steady_clock::now();
        const double frameMs = std::chrono::duration<double, std::milli>(tEnd - frameStartForRt).count();
        AccumulateRenderThreadTiming(_rtLastBusyMs, waitMs, frameMs);
        if (Dev::FramePaceTraceEnabled())
        {
            Dev::FramePaceProducerRow row;
            row.frame = _frameCounter;
            row.tPublishMs = Dev::FramePaceNowMs();
            row.preDrawWaitMs = static_cast<float>(preDrawWaitForTrace);
            row.lazyWaitMs = static_cast<float>(lazyWaitForTrace);
            row.slotWaitMs = static_cast<float>(slotWaitForTrace);
            row.lockstepWaitMs = static_cast<float>(lockstepWaitForTrace);
            row.renderThread = _renderThreadOn ? 1 : 0;
            row.overlap = _overlapOn ? 1 : 0;
            Dev::FramePaceTraceProducer(row);
        }

        LogObjectPerfRow();
        LogDrawSectionStats();
        const auto tLogs = std::chrono::steady_clock::now();
        if (_wbank)
        {
            // BEFORE the trim: this is what makes _lastUsedFrame mean anything on the retained
            // path, and the LRU immediately below is its only consumer.
            MarkRetainedModelLiveness();
            _wbank->TrimResidency();
            _lastTrimMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tLogs).count();
            // Immediately after the trim and before the next frame's admits, which is the only
            // window where dropping a live registration is safe: this frame is already recorded.
            const auto tBeforeInvalidate = std::chrono::steady_clock::now();
            const uint32_t stale = InvalidateStaleGpuModels();
            _lastInvalidateMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBeforeInvalidate).count();
            _lastInvalidateStale = stale;
            if (stale)
                LOG_INFO(Graphics, "Wgpu model residency: {} models re-registering after a texture eviction",
                         stale);
        }
        if (std::getenv("WGR_RESIDENCY_TRACE") &&
            (++_residencyTraceFrame == 30 || (_residencyTraceFrame % 120) == 0))
        {
            GpuMemoryStatsOut stats{};
            if (GetGpuMemoryStats(stats))
            {
                LOG_INFO(Graphics,
                         "Wgpu residency: tracked={} budget={} textures={} textureBytes={} "
                         "geometryLive={} geometryCapacity={} geometryRetired={} overBudget={} backendAllocations={}",
                         stats.trackedBytes, stats.budgetBytes, stats.objectTextureCount,
                         stats.objectTextureBytes, stats.geometryLiveBytes,
                         stats.geometryCapacityBytes, stats.geometryRetiredBytes,
                         stats.overBudget ? 1 : 0, stats.backendAllocationBytes);
            }
        }
        FlushPendingScreenshot();
        // The WHOLE of NextFrame, reported at its true end. The first two versions of this
        // stopped at AccumulateRenderThreadTiming and found nothing above 80 ms while the
        // profiler's `swap` -- which is this entire function -- still peaked near 300 ms.
        // Everything past that point was simply unmeasured, TrimResidency included, and a
        // timer that stops before the suspect cannot clear it.
        if (_slowBlockMs > 0.0)
        {
            const auto   nowEnd = std::chrono::steady_clock::now();
            const double wholeMs = std::chrono::duration<double, std::milli>(nowEnd - frameStartForRt).count();
            if (wholeMs >= _slowBlockMs)
            {
                const auto ms = [](auto a, auto b)
                { return std::chrono::duration<double, std::milli>(b - a).count(); };
                LOG_INFO(Graphics,
                         "Wgpu slow frame: whole={:.1f} ms | look={:.1f} params={:.1f} overlay={:.1f} "
                         "build={:.1f} slotWait={:.1f} block={:.1f} logs={:.1f} trim={:.1f} invalidate={:.1f} "
                         "(stale={}) rest={:.1f} | thread={} overlap={}",
                         wholeMs, ms(frameStartForRt, tLook), ms(tLook, tParams), ms(tParams, tOverlay),
                         // Subtract the consumer block ONLY when this thread ran it. Under
                         // overlap _rtLastBusyMs belongs to the worker and to another frame,
                         // and subtracting it printed a negative duration -- which is how the
                         // mistake announced itself.
                         ms(tOverlay, tEnd) - waitMs - (_overlapOn ? 0.0 : _rtLastBusyMs), waitMs,
                         _rtLastBusyMs, ms(tEnd, tLogs), _lastTrimMs, _lastInvalidateMs,
                         _lastInvalidateStale, ms(tLogs, nowEnd) - _lastTrimMs - _lastInvalidateMs,
                         _renderThreadOn ? 1 : 0, _overlapOn ? 1 : 0);
            }
            _lastTrimMs = 0.0;
            _lastInvalidateMs = 0.0;
            _lastInvalidateStale = 0;
        }

    }
    _verts.clear();
    _batches.clear();
    _draws3d.clear();
    _cmds.clear();
    _palette.clear();
    _cameras.clear();
    _shadowCasters.clear();
    // Already emptied by PatchPendingMeshHandles on a frame that published; cleared again
    // here so a frame that returned early (no renderer) cannot carry indices forward.
    _draws3dPendingMesh.clear();
    _castersPendingMesh.clear();
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

// Called only at a fresh monotonic producer admission, never by a borrower.
// Group allocation is outside the registry lock; failure disables only sharing.
bool EngineWgpu::PrepareSharedMeshLifetime(uint64_t producer)
{
    try
    {
        if (!producer) return false;
        if (!_sharedMeshEpoch) _sharedMeshEpoch = std::make_shared<render::SharedMeshEpoch>();
        auto group = std::make_shared<render::SharedRetainedMeshLifetime>(producer, _sharedMeshEpoch);
        std::lock_guard lock(_sharedMeshLifetimeMutex);
        return _sharedMeshLifetime.emplace(producer, std::move(group)).second;
    }
    catch (...) { return false; }
}

void EngineWgpu::AbandonUnpublishedSharedMeshLifetime(uint64_t producer)
{
    std::shared_ptr<render::SharedRetainedMeshLifetime> lifetime;
    {
        std::lock_guard lock(_sharedMeshLifetimeMutex);
        if (const auto found = _sharedMeshLifetime.find(producer); found != _sharedMeshLifetime.end())
        { lifetime = found->second; _sharedMeshLifetime.erase(found); }
    }
    if (lifetime) lifetime->ReleaseProducer(0);
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

    // REN-THR-011 — queued, and INFALLIBLE, for the reason REN-THR-001 established and the
    // retained path at RegisterGpuModel already relies on: the only failure wgr_mesh_create
    // ever had is empty input (pool.rs:468 — everything after it grows the pool and succeeds,
    // with no exhaustion case and no fallible device call). Both emptiness conditions are
    // checked above, producer-side, without the pool and without the renderer, so the queued
    // create cannot fail and there is no returned id left to test. That matters here more
    // than it did there: `_buffer` being null is, per ShapeDraw.cpp:508, "the ONLY structural
    // place the two backends can diverge" for a road, so a create that cannot fail removes
    // the divergence rather than deferring it.
    _ops.meshCreate++;
    std::shared_ptr<AttributedCpuMeshSlot> attributedSlot;
    std::shared_ptr<CpuMeshSlot> slot;
    if (_geometryOwnerLedger) { attributedSlot=std::make_shared<AttributedCpuMeshSlot>(); slot=attributedSlot; }
    else slot=std::make_shared<CpuMeshSlot>();
    slot->producer = _nextMeshHandle++; // one handle space with the retained meshes
    slot->type = type;
    slot->verts = std::move(verts);
    slot->indices = std::move(indices);
    if (VbUpdateStatsEnabled())
    {
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex < kVbTypeCount)
        {
            GVbUpdateCounters[typeIndex].createRequests.fetch_add(1, std::memory_order_relaxed);
            const uint64_t bytes = slot->verts.size() * sizeof(SVertex) + slot->indices.size() * sizeof(VertexIndex);
            GVbUpdateCounters[typeIndex].createBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    const WgrUploader* uploader = wgr_uploader_get(_renderer);
    // Covers enqueue/construction/section-building exceptions. A queued create
    // sees dead, and any acquired shared borrower debt is released exactly once.
    struct ConstructionRollback
    {
        std::shared_ptr<CpuMeshSlot> slot;
        WgrRenderer* renderer;
        const WgrUploader* uploader;
        std::shared_ptr<render::GeometryOwnerLedger> ledger;
        std::shared_ptr<AttributedCpuMeshSlot> attributedSlot;
        uint64_t epoch;
        bool committed = false;
        ~ConstructionRollback()
        {
            if (committed) return;
            slot->dead = true;
            if (ledger) ledger->QueueCpuBorrowerDetached(epoch, slot->producer);
            // Do not rely on the create still being undrained during exception
            // unwinding. A concurrently bound real handle must retire too.
            const auto dispatch = [&](uint64_t handle) {
                if (uploader) wgr_uploader_mesh_destroy(uploader, handle);
                else if (renderer) wgr_mesh_destroy(renderer, handle);
            };
            uint64_t retirement = 0;
            bool actual = false;
            if (slot->sharedLifetime.Release(retirement))
                slot->sharedLifetime.Dispatch(retirement, [&](uint64_t handle) { dispatch(handle); actual = uploader || renderer; });
            if (actual && ledger) ledger->QueueAllocationRetirementScheduled(epoch, slot->sharedLifetime.Producer());
            if (attributedSlot && attributedSlot->standaloneWitness)
            {
                slot->sharedLifetime.SuppressHandledStandaloneRetirement();
                actual = false;
                attributedSlot->standaloneWitness->Dispatch(attributedSlot->standaloneWitness->Release(), [&](uint64_t handle) { dispatch(handle); actual = uploader || renderer; });
                if (actual && ledger) ledger->QueueAllocationRetirementScheduled(epoch, attributedSlot->standaloneWitness->Producer());
            }
        }
    } rollback{slot, _renderer, uploader, _geometryOwnerLedger, attributedSlot, _geometryOwnerEpoch};
    // RFG-088: a STATIC buffer for a level the retained path already holds draws through that
    // mesh. Measured on native Everon at 800 retained models: the retained meshes were 759 MB
    // and the direct-path buffers 880 MB of the same pool, the largest mesh 12.9 MB on both
    // sides -- every Partial building (interior proxies, a glass complement) uploaded its
    // LODs twice, and the pool grew to 2,176 MB, which is the allocation the device refused.
    // The vertices are still built here: if the retained mesh is gone by the time the drain
    // runs, the slot falls back to its own copy (drain, CpuMeshCreate).
    if (type != VBDynamic && type != VBSmallDiscardable && Poseidon::Dev::GResidencyLevers().shareRetainedMeshes)
    {
        if (const auto shared = _retainedMeshOfLevel.find(&src); shared != _retainedMeshOfLevel.end())
        {
            std::shared_ptr<render::SharedRetainedMeshLifetime> lifetime;
            {
                std::lock_guard lock(_sharedMeshLifetimeMutex);
                if (const auto found = _sharedMeshLifetime.find(shared->second); found != _sharedMeshLifetime.end()) lifetime = found->second;
            }
            // Lookup only: a renderer-retired producer can never be recreated
            // by a later borrower. Ticket allocation failure uses exact fallback.
            if (slot->sharedLifetime.Attach(lifetime)) slot->sharedProducer = shared->second;
        }
    }
    if (_geometryOwnerLedger)
    {
        if (_standaloneWitnessBudget) attributedSlot->standaloneWitness = _standaloneWitnessBudget->Create(slot->producer, _sharedMeshEpoch);
    }
    // RFG-097 diagnostic: the first large buffers that do NOT share, by type, so "1,572 MB
    // of direct-path buffers on a streamed world" can be attributed to a producer.
    {
        static std::atomic<uint32_t> reported{0};
        const size_t bytes = slot->verts.size() * sizeof(SVertex) + slot->indices.size() * sizeof(uint32_t);
        if (slot->sharedProducer == 0 && bytes >= (1u << 20) && reported.fetch_add(1, std::memory_order_relaxed) < 12)
        {
            const char* tex = nullptr;
            if (src.NSections() > 0)
                if (Texture* t = src.GetSection(0).properties.GetTexture())
                    tex = t->Name();
            // Which registered model (if any) owns this level, and under which accessor?
            std::string owner = "level in NO registered model";
            for (const auto& [mshape, mid] : _gpuModels)
            {
                for (int l = 0; l < mshape->NLevels(); l++)
                {
                    const bool asLevel = mshape->Level(l) == &src;
                    const bool asOpaque = mshape->LevelOpaque(l) == &src;
                    if (asLevel || asOpaque)
                    {
                        owner = std::string("owner='") + mshape->Name() + "' level=" + std::to_string(l) +
                                (asLevel ? " via Level()" : "") + (asOpaque ? " via LevelOpaque()" : "") +
                                " meshes-registered=" + std::to_string(_gpuModelMeshes.count(mshape));
                        break;
                    }
                }
                if (owner[0] == 'o')
                    break;
            }
            LOG_INFO(Graphics, "Wgpu CPU buffer (own copy) owner: {}", owner);
            LOG_INFO(Graphics, "Wgpu CPU buffer (own copy): {:.1f} MB nv={} type={} sections={} first tex='{}' retained-level-known={}",
                     bytes / 1048576.0, nv, static_cast<int>(type), src.NSections(), tex ? tex : "-",
                     _retainedMeshOfLevel.count(&src) ? "yes" : "no");
        }
    }
    {
        ResourceOp op;
        op.kind = ResourceOp::CpuMeshCreate;
        op.mesh = slot->producer;
        op.cpu = slot;
        _resourceOps.push_back(std::move(op));
    }

    const bool dynamic = (type == VBDynamic || type == VBSmallDiscardable);
    // REN-THR-009 — borrowed once, here, and held for the buffer's life. Valid until
    // wgr_destroy (it is a borrow of a refcounted upload surface, not an owner), so there
    // is nothing to release in the destructor. Null only if the renderer is null, which
    // the guard at the top of this function has already ruled out; the buffer falls back
    // to the renderer handle in that case rather than silently dropping uploads.
    std::unique_ptr<VertexBufferWgpu> buf;
    if (_geometryOwnerLedger)
    {
        const uint64_t borrower = slot->producer;
        buf.reset(new AttributedVertexBufferWgpu(_renderer, uploader, attributedSlot, _cpuMeshLink, nv, dynamic, type,
                                            _geometryOwnerLedger, _geometryOwnerEpoch, borrower));
    }
    else
        buf.reset(new VertexBufferWgpu(_renderer, uploader, slot, _cpuMeshLink, nv, dynamic, type));
    render::mesh::BuildSections(src, buf->sections);
    rollback.committed = true;
    return buf.release();
}

Engine::SharedMeshFixtureReport EngineWgpu::ControlSharedMeshLifetimeFixture(SharedMeshFixtureAction action, const Shape* source)
{
    static const bool enabled = [] { const char* v = std::getenv("WGR_SHARED_MESH_LIFETIME_FIXTURE"); return v && std::strcmp(v, "1") == 0; }();
    SharedMeshFixtureReport result;
    if (!enabled) return result;
    result.status = GeometryReportStatus::Invalid;
    if (!Foundation::IsMainThread() || !_renderer) return result;
    const auto observe = [&] {
        auto& fixture = *_sharedMeshFixture;
        if (!fixture.lifetime && !fixture.standaloneWitness) return false;
        ResourceOp op; op.kind = ResourceOp::SharedMeshFixtureObserve; op.sharedFixture = _sharedMeshFixture;
        {
            std::lock_guard lock(fixture.mutex);
            if (fixture.report.requestId == std::numeric_limits<uint64_t>::max()) return false;
            op.sharedFixtureRequest = fixture.report.requestId + 1;
            op.mesh = fixture.report.rendererHandle;
        }
        _resourceOps.push_back(std::move(op));
        std::lock_guard lock(fixture.mutex);
        ++fixture.report.requestId;
        fixture.report.status = GeometryReportStatus::Pending;
        fixture.report.cpuBorrowers = 0; // Only the completed renderer cut supplies actual bound owners.
        fixture.report.producerOwned = false;
        return true;
    };
    try
    {
        if (action == SharedMeshFixtureAction::Begin || action == SharedMeshFixtureAction::BeginStandalone)
        {
            if (_sharedMeshFixture) { result.status = GeometryReportStatus::Busy; return result; }
            if (!source || source->NVertex() <= 0 || source->NVertex() > 4096 || source->NSections() < 0 || source->NSections() > 128 ||
                !Dev::GResidencyLevers().shareRetainedMeshes) return result;
            // Bound original polygon/section metadata walks before invoking
            // builders; vertex/section counts alone do not bound duplicate faces.
            size_t faceVisits = 0;
            for (Offset o = source->BeginFaces(); o < source->EndFaces();)
            {
                if (++faceVisits > 4096) return result;
                const auto& face = source->Face(o);
                if (face.N() < 3 || face.N() > 32) return result;
                for (int v = 0; v < face.N(); ++v)
                    if (face.GetVertex(v) >= source->NVertex()) return result;
                Offset next = o; source->NextFace(next);
                if (!(o < next) || source->EndFaces() < next) return result;
                o = next;
            }
            size_t sectionVisits = 0;
            for (int i = 0; i < source->NSections(); ++i)
            {
                const auto& section = source->GetSection(i);
                if (section.beg < source->BeginFaces() || source->EndFaces() < section.end || section.end < section.beg) return result;
                for (Offset o = section.beg; o < section.end;)
                {
                    if (++sectionVisits > 4096) return result;
                    Offset next = o; source->NextFace(next);
                    if (!(o < next) || section.end < next) return result;
                    o = next;
                }
            }
            const int indices = render::mesh::CountIndices(*source);
            if (indices <= 0 || indices > 16384) return result;
            const uint64_t payload = uint64_t(source->NVertex()) * sizeof(SVertex) + uint64_t(indices) * sizeof(VertexIndex);
            const uint64_t known = payload * 3 + uint64_t(source->NSections()) * sizeof(render::mesh::MeshSection) * 2 + 16384;
            if (known > 1024 * 1024) return result;
            _sharedMeshFixture = std::make_shared<SharedMeshFixtureState>();
            auto& fixture = *_sharedMeshFixture;
            if (action == SharedMeshFixtureAction::BeginStandalone)
            {
                if (!_geometryOwnerLedger || !_standaloneWitnessBudget) throw std::runtime_error("standalone fixture requires owner ledger");
                fixture.standalone = fixture.report.standalone = true;
                fixture.producerReleased = true; // No retained producer was fabricated.
                fixture.older.reset(CreateVertexBuffer(*source, VBDynamic));
                if (!fixture.older) throw std::runtime_error("standalone fixture create failed");
                const auto* buffer = static_cast<const VertexBufferWgpu*>(fixture.older.get());
                fixture.producer = buffer->slot->producer;
                fixture.standaloneWitness = static_cast<const AttributedCpuMeshSlot*>(buffer->slot.get())->standaloneWitness;
                if (!fixture.standaloneWitness) throw std::runtime_error("standalone witness refused");
                const uint64_t actualKnown = uint64_t(buffer->slot->verts.capacity()) * sizeof(SVertex) +
                    uint64_t(buffer->slot->indices.capacity()) * sizeof(VertexIndex) +
                    uint64_t(buffer->sections.MaxSize()) * sizeof(render::mesh::MeshSection) +
                    render::StandaloneMeshWitnessBudget::Charge + 16384;
                if (actualKnown > 1024 * 1024) throw std::runtime_error("standalone fixture capacity refused");
                fixture.knownBytes = fixture.report.knownPayloadBytes = actualKnown;
                fixture.report.producer = fixture.producer;
                fixture.report.sourceCopied = fixture.report.privateAllocation = true;
                if (!observe()) throw std::runtime_error("standalone observation failed");
                std::lock_guard lock(fixture.mutex);
                return fixture.report;
            }
            fixture.producer = _nextMeshHandle++;
            fixture.knownBytes = known;
            fixture.report.producer = fixture.producer;
            fixture.report.knownPayloadBytes = known;
            fixture.report.sourceCopied = true;
            fixture.report.privateAllocation = true;
            ResourceOp create; create.kind = ResourceOp::MeshCreate; create.mesh = fixture.producer;
            create.verts.resize(size_t(source->NVertex())); create.indices.resize(size_t(indices));
            render::mesh::BuildVertices(*source, create.verts.data()); render::mesh::BuildIndices(*source, create.indices.data());
            const uint64_t retainedCapacity = create.verts.capacity() * sizeof(SVertex) + create.indices.capacity() * sizeof(VertexIndex);
            if (retainedCapacity > payload) throw std::runtime_error("fixture capacity refused");
            if (!PrepareSharedMeshLifetime(fixture.producer)) throw std::runtime_error("fixture lifetime refused");
            try { _resourceOps.push_back(std::move(create)); }
            catch (...) { AbandonUnpublishedSharedMeshLifetime(fixture.producer); throw; }
            const auto previous = _retainedMeshOfLevel.find(source);
            const bool hadPrevious = previous != _retainedMeshOfLevel.end();
            const uint64_t previousProducer = hadPrevious ? previous->second : 0;
            // Restore exact original registration in this same non-yielding
            // owner operation, including either CreateVertexBuffer exception.
            struct Restore
            {
                std::unordered_map<const Shape*, uint64_t>& map; const Shape* source;
                bool existed; uint64_t producer;
                ~Restore() { if (existed) map.find(source)->second = producer; else map.erase(source); }
            } restore{_retainedMeshOfLevel, source, hadPrevious, previousProducer};
            _retainedMeshOfLevel[source] = fixture.producer;
            fixture.older.reset(CreateVertexBuffer(*source, VBStatic));
            fixture.newer.reset(CreateVertexBuffer(*source, VBStatic));
            {
                std::lock_guard lock(_sharedMeshLifetimeMutex);
                if (const auto found = _sharedMeshLifetime.find(fixture.producer); found != _sharedMeshLifetime.end()) fixture.lifetime = found->second;
            }
            if (!fixture.older || !fixture.newer || !fixture.lifetime) throw std::runtime_error("fixture create failed");
            const auto* older = static_cast<const VertexBufferWgpu*>(fixture.older.get());
            const auto* newer = static_cast<const VertexBufferWgpu*>(fixture.newer.get());
            const auto charge = [](const VertexBufferWgpu& buffer) {
                return uint64_t(buffer.slot->verts.capacity()) * sizeof(SVertex) +
                    uint64_t(buffer.slot->indices.capacity()) * sizeof(VertexIndex) +
                    uint64_t(buffer.sections.MaxSize()) * sizeof(render::mesh::MeshSection);
            };
            const uint64_t actualKnown = retainedCapacity + charge(*older) + charge(*newer) + 16384;
            if (actualKnown > 1024 * 1024) throw std::runtime_error("fixture capacity refused");
            fixture.knownBytes = fixture.report.knownPayloadBytes = actualKnown;
            if (!observe()) throw std::runtime_error("fixture observation failed");
        }
        else
        {
            if (!_sharedMeshFixture) return result;
            auto& fixture = *_sharedMeshFixture;
            if (action == SharedMeshFixtureAction::Snapshot)
            { std::lock_guard lock(fixture.mutex); return fixture.report; }
            if (action == SharedMeshFixtureAction::DropNewest) fixture.newer.reset();
            else if (action == SharedMeshFixtureAction::DropOldest) fixture.older.reset();
            else if (action == SharedMeshFixtureAction::ReleaseProducer || action == SharedMeshFixtureAction::Abort)
            {
                if (!fixture.producerReleased)
                {
                    ResourceOp destroy; destroy.kind = ResourceOp::MeshDestroy; destroy.mesh = fixture.producer;
                    _resourceOps.push_back(std::move(destroy)); fixture.producerReleased = true;
                }
                if (action == SharedMeshFixtureAction::Abort)
                {
                    if(_geometryOwnerLedger) {
                        ResourceOp unpin; unpin.kind=ResourceOp::SharedMeshFixtureObserve; unpin.sharedFixture=_sharedMeshFixture;
                        unpin.sharedFixtureRequest=0; _resourceOps.push_back(std::move(unpin));
                    }
                    fixture.newer.reset(); fixture.older.reset(); _sharedMeshFixture.reset(); result.status=GeometryReportStatus::Ready; return result;
                }
            }
            else if (action != SharedMeshFixtureAction::Observe) return result;
            if (!observe()) return result;
        }
        std::lock_guard lock(_sharedMeshFixture->mutex);
        return _sharedMeshFixture->report;
    }
    catch (...)
    {
        // Keep the bounded state available for an explicit Abort/retry; no
        // source/Shape pointer survives and no failed fixture is reported Ready.
        if (_sharedMeshFixture) { std::lock_guard lock(_sharedMeshFixture->mutex); _sharedMeshFixture->report.status = GeometryReportStatus::Failed; }
        result.status = GeometryReportStatus::Failed;
        return result;
    }
}

namespace
{
// FAR INSTANCE TIER — its four knobs, read from the environment ONCE.
//
// Read once and not per frame so that a capture cannot drift halfway through, and so the
// whole tier is revertible from ONE binary: WGR_FAR_TIER=0 leaves every other code path
// (the ABI entry points, the load-time build, the timer regions) exactly where it is and
// simply stops the sweep, which is what makes an A/B of it a measurement rather than a
// comparison of two builds.
struct FarTierSettings
{
    bool enabled = true;
    // Negative = let the active start rule pick its own default (see the resolution site).
    // An explicit value is a HARD FLOOR under the complement rule, not the start rule.
    float nearCutoff = -1.0f;
    float distance = 12000.0f;
    // PIXELS of projected height. The measured elbow: 2 px took an aircraft view from
    // 421,323 surviving instances to 22,135 and its draw from 0.314 ms to 0.025 ms. 0
    // disables the test entirely, which is the A/B for it.
    float pixelLimit = 2.0f;
    // WGR_FAR_MODE=distance restores the pre-complement pure-distance start rule, so the two
    // rules can be A/B'd from ONE binary. The renderer reads the same variable for the
    // predicate itself (far/mod.rs mode_from_env); this side needs it only to pick the
    // matching near-cutoff default. Passing it across instead would cost a new
    // wgr_far_set_params parameter and a WGR_ABI_VERSION bump.
    bool complement = true;
};

// The complement rule's default near cutoff — a HARD FLOOR under the accept test, in metres.
// Small on purpose. Under the old distance rule this number WAS the start rule and had to be
// hundreds of metres; under the complement it only answers "how close may a crude box get to
// the eye", and the complement itself is what keeps a proxy off a live model. Leaving it at a
// fraction of the draw distance would re-open the gap the complement exists to close, since
// everything the object cull drops inside that radius would go back to being drawn by nothing.
constexpr float FAR_TIER_HARD_FLOOR_M = 40.0f;

const FarTierSettings& FarSettings()
{
    static const FarTierSettings settings = []
    {
        FarTierSettings s;
        if (const char* on = std::getenv("WGR_FAR_TIER"))
            s.enabled = std::strcmp(on, "0") != 0;
        if (const char* cut = std::getenv("WGR_FAR_NEAR_CUTOFF"))
            s.nearCutoff = static_cast<float>(std::atof(cut));
        if (const char* dist = std::getenv("WGR_FAR_DISTANCE"))
            s.distance = static_cast<float>(std::atof(dist));
        if (const char* px = std::getenv("WGR_FAR_PIXEL_LIMIT"))
            s.pixelLimit = static_cast<float>(std::atof(px));
        if (const char* mode = std::getenv("WGR_FAR_MODE"))
        {
            // Case-insensitive by hand rather than _stricmp/strcasecmp: this translation unit
            // builds for Linux as well, and the two platforms spell that function differently.
            std::string lowered(mode);
            for (char& c : lowered)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.complement = lowered != "distance";
        }
        return s;
    }();
    return settings;
}

// Fraction of Scene::GetObjectDrawDistance() at which the far tier begins when no
// explicit cutoff is set. See the resolution site for why this is not 1.0.
float FarTierNearCutoffFraction()
{
    static const float fraction = []
    {
        const char* v = std::getenv("WGR_FAR_NEAR_FRACTION");
        const float parsed = v ? static_cast<float>(std::atof(v)) : 0.45f;
        return (parsed > 0.0f && parsed <= 1.0f) ? parsed : 0.45f;
    }();
    return fraction;
}
} // namespace

// FAR INSTANCE TIER — hand the renderer the whole proxy set for a world.
//
// The layouts are checked rather than repacked: at a few million rows a field-by-field copy
// would be a visible chunk of the load, and the two structs exist to be the same 32 bytes.
void EngineWgpu::SetFarProxyInstances(const FarProxyInstance* data, size_t count)
{
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)
    static_assert(sizeof(FarProxyInstance) == sizeof(WgrFarInstance),
                  "Engine::FarProxyInstance must be the WgrFarInstance ABI record");
    static_assert(offsetof(FarProxyInstance, height) == offsetof(WgrFarInstance, height) &&
                      offsetof(FarProxyInstance, radius) == offsetof(WgrFarInstance, radius) &&
                      offsetof(FarProxyInstance, colour) == offsetof(WgrFarInstance, colour) &&
                      offsetof(FarProxyInstance, flags) == offsetof(WgrFarInstance, flags),
                  "Engine::FarProxyInstance fields must sit where WgrFarInstance's do");
    if (!_renderer)
        return;
    if (!FarSettings().enabled)
    {
        // Do not upload at all when the tier is off: the source rows are ~117 MB on a world
        // the size of Chernarus, and paying that for a feature that will never sweep them
        // would make WGR_FAR_TIER=0 a different memory profile as well as a different
        // picture, which spoils it as a control.
        LOG_INFO(Graphics, "Wgpu far tier: disabled by WGR_FAR_TIER=0, {} proxies not uploaded", count);
        return;
    }
    if (data == nullptr || count == 0)
    {
        wgr_far_set_instances(_renderer, WgrSlice<WgrFarInstance>{nullptr, 0});
        return;
    }
    wgr_far_set_instances(_renderer,
                          WgrSlice<WgrFarInstance>{reinterpret_cast<const WgrFarInstance*>(data), U32(count)});
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
        // Pushed whenever there is a renderer, NOT only when _gpuDriven. These four numbers
        // describe the ENGINE's object cull, and the far instance tier now derives its start
        // rule from them (it accepts exactly what they make the object cull drop) — so gating
        // them on the GPU-driven path, which is OFF by default here, left the far tier
        // complementing CullInputs::default() instead of the live cull. When the GPU-driven
        // path is off the cull dispatch is a no-op and nothing else reads them, so this is a
        // data push and not a behaviour change for that path.
        if (_renderer)
        {
            // Draw distance is Scene::GetObjectDrawDistance(), NOT the raw OBJECT_Z: objectsZ
            // is 2/3 of the view distance and doubles as the simulation radius, which put the
            // renderer's hard cull edge at a distance where distance fog is only ~30% applied
            // — so objects materialised out of nothing there (the flying pop-in). The shared
            // accessor anchors the render cull to the fog max range, where fog is 1.0, and is
            // read by the CPU path (Scene::LevelFromDistance2) from the same place so the two
            // culls cannot drift. WGR_OBJECT_DISTANCE_SCALE=1 restores the legacy distance.
            // Draw distance + lodInvWidth come from the presentation snapshot (captured at
            // InitDraw): they are frame-constant scene state, unlike the camera pulls above,
            // which UpdateProjection() legitimately re-reads mid-draw (MAT-052).
            // SNAPSHOT ONLY -- no live fallback. This read used to be
            // `snap.hasScene ? snap.objectDrawDistance : GScene->GetObjectDrawDistance()`,
            // and the fallback was three things at once, all bad:
            //
            //   * a live read of world state from the render path, which is precisely what
            //     Phase 5 exists to remove and what becomes a data race the moment the
            //     renderer has its own thread -- `_lodInvWidth` is written DURING the draw by
            //     the framerate governor (SceneDraw.cpp:1176, :1198), so it is the one field
            //     POSEIDON_SNAPSHOT_VALIDATE=1 reports as an intra-frame mutator;
            //   * unreachable in the good case, since `hasScene` is exactly "GScene was
            //     non-null at capture";
            //   * a null dereference in the bad one -- if GScene was null at capture and is
            //     still null now, the fallback dereferences it.
            //
            // With no scene there is nothing to cull, so skipping the push is the honest
            // behaviour rather than pushing zeroes and culling everything.
            const auto& snap = render::frame::GPresentationSnapshot();
            if (snap.hasScene)
            {
                _pendingSet.cullDrawDistance = snap.objectDrawDistance;
                _pendingSet.cullCamLeft = static_cast<float>(camera->Left());
                _pendingSet.cullLodInvWidth = snap.lodInvWidth;
                _pendingSet.cullPixelLimit = 0.125f;
                _pendingSet.cullParamsDirty = true;
            }
        }
        if (_gpuDriven && _renderer)
        {
            // GPU Hi-Z occlusion replaces the engine's software occlusion buffer for the retained
            // set; force the built-in off while it's on so the two don't both cull (and the CPU
            // stops rasterizing the occlusion buffer). Driven per frame so the default (occlusion
            // on) applies without opening the Culling tab; the checkbox flips _cullDebug.occlusion.
            EnableObjOcc = !_cullDebug.occlusion;
        }

        // FAR INSTANCE TIER — its per-frame knobs, pushed beside the cull params because
        // they answer the same question from the other end: those say where real objects
        // stop being drawn, these say what stands in past that point.
        //
        // The near cutoff DEFAULTS to the object draw distance for exactly that reason. Any
        // other default leaves either a band where a proxy is drawn over the real model, or
        // a band where neither is drawn — and the second one looks identical to the bug this
        // tier exists to fix, which would make it impossible to tell whether the tier is
        // working.
        if (_renderer)
        {
            // `farTier`, not `far`: MSVC's <windows.h> still defines `far` as a segment
            // keyword macro, so a local of that name is a parse error under clang-cl.
            const FarTierSettings& farTier = FarSettings();
            // THE START RULE IS NO LONGER A DISTANCE. It is the complement of the real
            // object cull, evaluated per placement in far.wgsl from the very cull params
            // pushed above. What is resolved here is only the HARD FLOOR beneath it.
            //
            // Why no distance works, measured against the real 1.2M Everon placement set
            // with exact per-model bounds (experiments/farfield/cutoff_analysis.py). At
            // the governor's fine rail a 150 m seam put a proxy on top of a live model
            // for 92.1% of placements; pulling the seam out to the 880 m fog edge cut
            // that to 0% but left 5.9% drawn by nothing. At the COARSE rail the same
            // 880 m seam left 36.2% drawn by nothing -- that is the reported pop-in, and
            // it is a different number at every framerate, because the real cull is
            // size-dependent and its coefficient rides the governor. The complement
            // measures 0% overlap and 0% gap at both rails, by construction.
            //
            // The floor still matters: the complement legitimately accepts a placement a
            // few metres away whose real model the sub-pixel cull dropped, and a crude
            // box that close is compared by eye against the real geometry beside it. A
            // few tens of metres is enough to own that band, and WGR_FAR_NEAR_CUTOFF
            // overrides it. Under WGR_FAR_MODE=distance the old fraction-of-draw-distance
            // start rule comes back unchanged, so the A/B is one variable and no rebuild.
            float nearCutoff = farTier.nearCutoff;
            if (nearCutoff < 0.0f)
            {
                const auto& snapFar = render::frame::GPresentationSnapshot();
                const float farDrawDistance =
                    snapFar.hasScene ? snapFar.objectDrawDistance : GScene->GetObjectDrawDistance();
                nearCutoff = farTier.complement ? FAR_TIER_HARD_FLOOR_M
                                                : farDrawDistance * FarTierNearCutoffFraction();
            }
            _pendingSet.farNearCutoff = nearCutoff;
            _pendingSet.farDistance = farTier.distance;
            _pendingSet.farPixelLimit = farTier.pixelLimit;
            _pendingSet.farEnabled = farTier.enabled ? 1u : 0u;
            _pendingSet.farParamsDirty = true;
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

// MAT-052 -- THE transparent cockpit. Transport::Draw brackets every interior
// (zSpace) draw with SetClipRange(0.01, 50) + UpdateProjection() so the canopy
// frame -- which hugs the pilot's head, well inside the world near plane -- is
// not near-clipped. GL33 implements this hook (EngineGL33_Mesh.cpp); wgpu
// inherited the base no-op, so the cockpit's projection never reached the GPU
// and every polygon nearer than the world near plane was clipped away: canopy
// beams, pillars, roof, on every helicopter of every generation, base game
// included. The dashboard (~0.5 m) and the canopy glass survive, which is what
// made this read as "the cockpit is transparent" rather than "geometry is
// missing" -- through the missing beams you see glass tint over sky, terrain,
// and the volumetric cloud deck.
//
// A new camera entry with the CURRENT projection is exactly what the multi-
// camera plumbing already does for HUD and weapon views; the epilogue's second
// UpdateProjection (clip range restored) pushes the world projection back for
// anything drawn after. Found by bisect (good 34c44ac5, first-bad aa24c32b)
// plus a section-level trace proving all 60 cockpit sections were SUBMITTED
// with sane state -- the fragments died in clipping, one stage no draw-level
// door can reach.
void EngineWgpu::UpdateProjection()
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
    _currentCpuPose = nullptr;
    _currentCpuPoseSource = nullptr;

    auto* buf = static_cast<VertexBufferWgpu*>(sMesh.GetVertexBuffer());
    if (!buf)
    {
        return;
    }

    const auto* object = static_cast<const Object*>(GetDrawObject());
    static const bool captureHelicopterPose = []
    {
        const char* v = std::getenv("WGR_HELI_POSE_SNAPSHOTS");
        return !(v && v[0] == '0');
    }();
    // Destruction morphs share their source shape with intact copies. Later
    // draws (including shadow views) must not overwrite a queued wreck pose.
    const bool destructionPose = object && object->IsDestroyed() && object->GetDestructType() != DestructTree;
    const bool helicopterPose = captureHelicopterPose && object && dyn_cast<const Helicopter>(object);
    // A newly streamed LOD can still have a static source buffer even though
    // this object is destroyed; the private copy must nevertheless be dynamic.
    if ((destructionPose || (helicopterPose && buf->isDynamic)) && !buf->skinned)
    {
        AcquireProducerWindow(destructionPose ? "destruction CPU pose" : "helicopter CPU pose");
        if (buf->poseFrame != _cpuPoseFrame)
        {
            buf->poseFrame = _cpuPoseFrame;
            buf->nextPoseCopy = 0;
        }
        const size_t index = buf->nextPoseCopy++;
        if (index == buf->poseCopies.size())
            buf->poseCopies.emplace_back(static_cast<VertexBufferWgpu*>(CreateVertexBuffer(sMesh, VBDynamic)));
        auto* copy = buf->poseCopies[index].get();
        if (copy)
        {
            copy->bufferDirty = true;
            copy->Update(sMesh, true);
            _currentCpuPose = copy;
            _currentCpuPoseSource = &sMesh;
            return;
        }
    }
    buf->Update(sMesh, dynamic);

    // FACE-001: while the mesh is CPU-posed it carries its own final vertices; handing it a
    // palette as well would pose it twice.
    if (buf->cpuPosed)
    {
        Poseidon::GFaceDiag().drawPosed++;
    }
    if (!buf->skinned || buf->cpuPosed || buf->paletteCount <= 0)
    {
        return;
    }
    Poseidon::GFaceDiag().drawPalette++;

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
    _currentCpuPose = nullptr;
    _currentCpuPoseSource = nullptr;
}

void EngineWgpu::SetMaterial(const TLMaterial& mat, const LightList& /*lights*/, const render::LegacySpec& /*spec*/)
{
    // Capture only; DrawSectionTL folds it with the sun once it has the combined
    // mesh+section spec (for the DisableSun / sun-enable decision). Per-draw local
    // lights are ignored here — the wgpu path drives lights from a single
    // frame-global light store, not GL33's per-draw list.
    _curMaterial = mat;
}

// DrawSectionTL runs once per contiguous same-material section run per object draw, i.e. tens
// of thousands of times a frame on a populated world. Two things inside it were being paid at
// that rate for diagnostics that are off:
//
//  * std::getenv(). On Windows the CRT takes a lock and walks the whole process environment
//    block; the answer cannot change during a run, so it is read once here instead.
//  * the one-shot "already logged this material" probes, which hashed a std::string of the
//    RVMAT path on every section of every draw for the entire session, long after their single
//    line had been emitted. They are re-keyed on the address of the material cache's own key
//    string (see the call site) so they admit exactly the same first-occurrences.
//
// WGR_DRAW_SECTION_ENV_CACHE=0 restores both, so the cost of either can be measured from one
// binary without a rebuild.
static bool DrawSectionEnvCacheEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_DRAW_SECTION_ENV_CACHE");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

static bool EnvFlagSet(const char* name)
{
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "0") != 0;
}

static bool RoadTraceEnabled()
{
    static const bool cached = EnvFlagSet("WGR_ROAD_TRACE");
    return DrawSectionEnvCacheEnabled() ? cached : EnvFlagSet("WGR_ROAD_TRACE");
}

static bool LegacyTextureTraceEnabled()
{
    static const bool cached = EnvFlagSet("WGR_LEGACY_TEXTURE_TRACE");
    return DrawSectionEnvCacheEnabled() ? cached : EnvFlagSet("WGR_LEGACY_TEXTURE_TRACE");
}

static bool TreeAdvLightTraceEnabled()
{
    // Historically tested with plain getenv() != nullptr, so an empty or "0" value still
    // enabled it. Keep that exact truth test in both arms.
    static const bool cached = std::getenv("WGR_TREEADV_LIGHT_TRACE") != nullptr;
    return DrawSectionEnvCacheEnabled() ? cached : (std::getenv("WGR_TREEADV_LIGHT_TRACE") != nullptr);
}

void EngineWgpu::DrawSectionTL(const Shape& sMesh, int beg, int end)
{
    if (DrawSectionStatsEnabled())
    {
        ++GDrawSectionCalls;
    }
    // PERF-016 attribution (WGR_PASS1_STATS): the whole body, subdivided further below.
    // The Scope self-gates on the Pass1 submission loop, so Pass2 / shadow calls stay out.
    Pass1Submit::Scope p1sTL(Pass1Submit::gSecTLNs);
    if (p1sTL.sink)
    {
        Pass1Submit::gSecTLCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (!_renderer)
    {
        return;
    }

    auto* buf = static_cast<VertexBufferWgpu*>(_currentCpuPose && _currentCpuPoseSource == &sMesh
                                                 ? _currentCpuPose : sMesh.GetVertexBuffer());
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
    // The `.emat` `Color` surface tint for this draw (identity unless the
    // binding block below records/replays one). Applied at the sun fold.
    float ematTint[3] = {1.0f, 1.0f, 1.0f};
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
        //
        // SCOPE, measured 2026-09-04 and recorded because the sentence above was read as a
        // statement about roads in general and sent a session after the wrong bug: that is
        // an ARMA 2 (roads_e) finding and it does NOT hold for the original OFP/CWA roads.
        // Every LOD of every road-surface model in O.pbo -- asf*, sil*, ces*, kos*,
        // kr_new_* , 102 LODs over 43 models, no exception -- stores meanNorm = (0, -1, 0), already
        // DOWN in the file, so MeshBuild's negation hands the shader (0, +1, 0) and the sun
        // term on Everon/Malden/Kolgujev is correct by construction. (Decoded straight out
        // of the ODOL v7 vertex table: clip/uv/pos/norm arrays, each a count followed by its
        // data, all four counts equal to NVertex.) A dark road on those maps is therefore
        // never this; look at the ambient path instead.
        // Note GL33 shares BuildVertices AND the same clamp, so a plain sign error would
        // band both backends; it does not, which is why the PATH matters as much as the
        // values. A road that never appears in this log took the CPU FaceArray::Draw branch
        // instead, and that absence is itself the answer.
        if (RoadTraceEnabled())
        {
            const char* rname = t->Name();
            if (rname && *rname)
            {
                // Key on the drawn SHAPE, not the texture. A light and a dark trapezoid share
                // dirt_tak_ca.paa, so a texture key collapses them onto one line and can never
                // see a per-draw difference -- which is exactly what the measurements now say
                // this is: identical materials, identical normals, identical section spec, one
                // path, and still alternating.
                static std::unordered_set<const void*> loggedRoadShapes;
                if (loggedRoadShapes.insert(static_cast<const void*>(&sMesh)).second)
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
                             "specFlags=0x{:x} secSpec=0x{:x}"
                             // Per-DRAW state. Materials, normals and section spec are already
                             // known to be identical across light and dark trapezoids, so the
                             // discriminator has to be here: the spec BeginMeshTL stored (i.e.
                             // the specAdj Object::Draw computed), the model-to-world the draw
                             // was handed, and whether a vertex buffer existed at all -- the
                             // last being the only structural place GL33 and wgpu can diverge
                             // for a road. camRelPos + the camera position gives absolute world
                             // XZ, which is how a line is matched to a trapezoid on screen.
                             " | meshSpec=0x{:x} vbuf={} camRelPos=({:.1f},{:.1f},{:.1f}) scale={:.4f}",
                             rname, sec.surfMat ? "Combine(surfMat)" : "object-material", nv, nx, ny, nz,
                             _curMaterial.ambient.R(), _curMaterial.ambient.G(), _curMaterial.ambient.B(),
                             _curMaterial.diffuse.R(), _curMaterial.diffuse.G(), _curMaterial.diffuse.B(),
                             _curMaterial.emmisive.R(), _curMaterial.emmisive.G(), _curMaterial.emmisive.B(),
                             _curMaterial.forcedDiffuse.R(), _curMaterial.forcedDiffuse.G(),
                             _curMaterial.forcedDiffuse.B(), _curMaterial.specularPower,
                             unsigned(_curMaterial.specFlags), unsigned(sec.properties.Special()),
                             unsigned(_meshSpec), buf ? 1 : 0, _worldM.Position().X(),
                             _worldM.Position().Y(), _worldM.Position().Z(), _worldM.Scale());
                }
            }
        }
        // One-shot, opt-in probe for the original CWA PAC/P8 vegetation route.  The material
        // base-colour debug view must be allowed to identify whether a black canopy originates
        // in the texture upload or in lighting, without changing either path.  Kept general so
        // it can also diagnose other legacy-only content; keyed by the virtual texture path.
        if (LegacyTextureTraceEnabled())
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

    Pass1Submit::Scope p1sDesc(Pass1Submit::gSecDescNs);
    const int sectionSpec = sMesh.GetSection(beg).properties.Special();
    const int effectiveSpec = _meshSpec | sectionSpec;
    const render::LegacySpec splitSpec = render::SplitLegacy(effectiveSpec);

    render::BuildContext ctx;
    ctx.isIn3DPass = true;
    ctx.shadowAlphaRef = static_cast<std::uint8_t>(std::min(255, (GetShadowFactor() * 7) >> 4));
    const render::RenderPassDescriptor desc = render::BuildRenderPassDescriptor(splitSpec, ctx);
    p1sDesc.Close();

    if (_smEnabledFrame && desc.blend == render::BlendMode::Shadow)
    {
        // Skip legacy shadows when CSM is enabled
        return;
    }

    WgrDraw3D d{};
    // REN-THR-011: the real handle for every mesh created in an earlier frame; 0 for one
    // created earlier in THIS draw phase, patched at the publish point after the drain.
    d.mesh = buf->RendererMesh();
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
    // `__trace__` is an OBSERVER, not a selector: it must not change what is drawn.
    //
    // It used to, and that silently invalidated every appearance judgement made in a
    // trace run. `productionMaterialTarget` below requires an EMPTY selector, so simply
    // setting WGR_MATERIAL_DEBUG_RVMAT=__trace__ turned production material binding OFF
    // for the whole CPU direct path -- every section drawn there came out untextured.
    // "It renders flat white under __trace__" was therefore a property of the flag, not
    // of the geometry. Treat the trace sentinel as no selector at all.
    const bool diagnosticSelector = !_materialDebugRvmatMatch.empty() && _materialDebugRvmatMatch != "__trace__";
    // Without a diagnostic selector, authored section RVMATs are production
    // bindings. Unknown families and procedural stages remain intentionally
    // unbound after translation; there is no suffix-based fallback here.
    const bool productionMaterialTarget = !diagnosticSelector && debugSection.surfMat;
    const bool materialBindingTarget = materialDebugTarget || productionMaterialTarget;
    d.normal_texture_id = 0;
    d.flags &= ~(WGR_DRAW3D_NORMAL_RG | WGR_DRAW3D_NATIVE_CAVITY);
    // Shared lane: material-debug bits 0-8, REN-TEMP-001H object id bits 16-31.
    d.misc = (materialDebugTarget
                  ? static_cast<uint32_t>(_materialDebug.view) | (_materialDebug.invertNormalY ? 0x100u : 0u)
                  : 0) |
             (CurrentDrawObjectId() << 16);
    _materialDebugInfo = {};
    Pass1Submit::Scope p1sBind(Pass1Submit::gSecBindNs);
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
                if (wgpuNormalTexture->NormalIsRgPacked())
                    d.flags |= WGR_DRAW3D_NORMAL_RG;
                if (wgpuNormalTexture->NormalHasNativeCavity())
                    d.flags |= WGR_DRAW3D_NATIVE_CAVITY;
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
            Pass1Submit::gSecBindCalls.fetch_add(1, std::memory_order_relaxed);
        }
        // PERF-016 fast path: this material's binding answers were resolved once below and
        // are a pure function of its path, so replay them. Only the bank Load() +
        // EnsureUploaded() pair stays per-section -- Load marks the texture used and
        // EnsureUploaded repairs an eviction, which is what keeps residency honest without
        // caching a handle. Any material-debug selector keeps every section on the full
        // path below, so every diagnostic behaves exactly as before.
        bool bindServed = false;
        // RFG-060: the face texture already carries this material's coverage; see
        // FaceTextureIsEnfusionComposite. Asked once per section and used by both the
        // cached and the full binding path below, which must agree or the same leaf card
        // draws differently depending on whether the cache was warm.
        const bool faceIsComposite = FaceTextureIsEnfusionComposite(section);
        if (materialPath && *materialPath && !materialDebugTarget && SectionBindCacheEnabled())
        {
            if (MaterialClass* bindSlot = MaterialClassSlot(section.surfMat); bindSlot && bindSlot->bindResolved)
            {
                bindServed = true;
                Pass1Submit::gSecBindFast.fetch_add(1, std::memory_order_relaxed);
                if (!faceIsComposite &&
                    (bindSlot->baseKind == MaterialClass::BaseImage ||
                     (bindSlot->baseKind == MaterialClass::BaseConstant &&
                      (d.texture_id == 0 || bindSlot->isCalmWater))))
                {
                    Ref<Texture> baseTexture = _wbank->Load(bindSlot->baseLoadName);
                    auto* wgpuBaseTexture = static_cast<TextureWgpu*>(baseTexture.GetRef());
                    if (const uint64_t baseHandle = wgpuBaseTexture ? wgpuBaseTexture->EnsureUploaded() : 0)
                    {
                        d.texture_id = baseHandle;
                    }
                }
                if (bindSlot->hasNormal)
                {
                    // Loaded even when normal maps are disabled, exactly like the full path:
                    // the Load is what keeps the texture's residency mark fresh.
                    Ref<Texture> normalTexture = _wbank->Load(bindSlot->normalLoadName);
                    auto* wgpuNormalTexture = static_cast<TextureWgpu*>(normalTexture.GetRef());
                    const uint64_t normalHandle = wgpuNormalTexture ? wgpuNormalTexture->EnsureUploaded() : 0;
                    const bool enableNormal =
                        _materialDebug.view == MaterialDebugSettings::View::NormalMap ||
                        (_materialDebug.view == MaterialDebugSettings::View::FullMaterial &&
                         !_materialDebug.disableNormalMap);
                    if (enableNormal && normalHandle)
                    {
                        d.normal_texture_id = normalHandle;
                        if (wgpuNormalTexture->NormalIsRgPacked())
                            d.flags |= WGR_DRAW3D_NORMAL_RG;
                        if (wgpuNormalTexture->NormalHasNativeCavity())
                            d.flags |= WGR_DRAW3D_NATIVE_CAVITY;
                    }
                }
                ematTint[0] = bindSlot->tint[0];
                ematTint[1] = bindSlot->tint[1];
                ematTint[2] = bindSlot->tint[2];
            }
        }
        if (materialPath && *materialPath && !bindServed)
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
                        cached = _materialBindings
                                     .emplace(materialPath, Asset::Material::LoadTranslatedMaterial(materialPath))
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
                // One line per distinct RVMAT, without hashing the RVMAT path on every section
                // of every draw for the rest of the session.
                //
                // `cached->first` is the material cache's own key: exactly one std::string per
                // distinct path, allocated once and never moved (unordered_map nodes are stable
                // across rehash), and the entry outlives every draw that can reach it. Its
                // ADDRESS is therefore a perfect stand-in for the path text -- the pointer set
                // admits precisely the first-occurrences the string set admitted -- and a
                // pointer hash replaces a 40-to-60 byte string hash in the per-section path.
                const void* materialLogKey = static_cast<const void*>(&cached->first);
                const bool fastLogKeys = DrawSectionEnvCacheEnabled();
                auto logOnceForMaterial =
                    [&](std::unordered_set<const void*>& byKey, std::unordered_set<std::string>& byPath)
                { return fastLogKeys ? byKey.insert(materialLogKey).second : byPath.insert(materialPath).second; };
                // One-shot diagnostic for the TreeAdv-family night-lighting investigation.
                // The source fields have family-specific meanings (not generic emissive), so
                // capture them verbatim before a family shader starts consuming them.
                if (TreeAdvLightTraceEnabled() &&
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
                const char* cpuBaseFrom = "Stage0";
                const auto* base = EffectiveBaseColourStage(translated, cpuBaseFrom);
                const bool baseIsImage = base && !base->texture.isProcedural && !base->texture.path.empty();
                // DZ-006: a procedural CONSTANT colour is a real albedo when the section has no
                // face texture of its own, and DayZ water is exactly that case. Every DayZ water
                // quad -- Enoch's `water_bliss` ponds and rivers and Chernarus's older `water`
                // ponds alike -- names the SAME face texture, `dz\surfaces\data\roadway\
                // water_ext.paa`. That is a surface-TYPE marker (it decides footstep sounds and
                // penetration), not an albedo, and it does not upload; the colour lives in the
                // `CalmWater` material as a constant instead. Enoch's two `.emat` files are the
                // only ones DayZ ships and they agree exactly on it.
                //
                // Refusing procedural stages outright left those sections on the WHITE bindless
                // fallback, and DZ-003's reflection then added a sky on top of white: Chernarus's
                // 4,044 water placements rendered as glowing snow patches. Enoch was unaffected
                // only because its water reaches the RETAINED path, whose RegisterGpuModel has
                // accepted constants since MAT-045.
                //
                // Same precedence as that path, so nothing else changes: `d.texture_id != 0`
                // means the section already has a face texture, and a constant never overrides a
                // real image (MAT-045 -- on Arma 3 structures the constant is the placeholder and
                // the face texture is the baked albedo).
                //
                // `CalmWater` is the ONE measured exception, and it is an exception to MAT-045's
                // premise rather than to its rule. MAT-045 assumes the face texture is the baked
                // albedo. On DayZ water it is not: `water_ext.paa` lives in
                // `dz\surfaces\data\roadway\`, the ROADWAY/surface-type set (it decides footstep
                // sounds and traction), beside `clean_water_ext.paa` and `still_water_ext.paa`.
                // It is a 128x128 DXT1 at mean RGB (42, 49, 86) -- four times brighter than the
                // authored water colour (0.0392, 0.0549, 0.0667) and stretched over a 50 m quad,
                // which is why the reflection blew it out. Every single DayZ water p3d names it,
                // on both worlds and both addons, so the face texture carries no per-surface
                // information at all and the constant is the only real albedo on offer.
                const bool baseIsConstant = base && base->texture.isProcedural &&
                                            (d.texture_id == 0 || Asset::Material::IsCalmWater(translated)) &&
                                            render::procedural::Parse(base->texture.raw.c_str()).generated;
                if (faceIsComposite && (baseIsImage || baseIsConstant))
                {
                    static std::unordered_set<const void*> loggedCompositeKept;
                    static std::unordered_set<std::string> loggedCompositeKeptByPath;
                    if (logOnceForMaterial(loggedCompositeKept, loggedCompositeKeptByPath))
                        LOG_INFO(Graphics,
                                 "Wgpu material: Stage0 NOT overriding an enfa| composite face texture rvmat={} "
                                 "stage0={} face={}",
                                 materialPath, base->texture.raw,
                                 section.properties.GetTexture() != nullptr
                                         && section.properties.GetTexture()->Name() != nullptr
                                     ? section.properties.GetTexture()->Name()
                                     : "");
                }
                else if (baseIsImage || baseIsConstant)
                {
                    // A procedural stage is GENERATED from its own name and has no path, so the
                    // bank is handed the raw declaration; ResolveMaterialBaseColour does the same.
                    const std::string loadName = baseIsConstant ? base->texture.raw : base->texture.path.canonical();
                    Ref<Texture> baseTexture = _wbank->Load(loadName.c_str());
                    auto* wgpuBaseTexture = static_cast<TextureWgpu*>(baseTexture.GetRef());
                    const uint64_t baseHandle = wgpuBaseTexture ? wgpuBaseTexture->EnsureUploaded() : 0;
                    if (baseHandle)
                    {
                        d.texture_id = baseHandle;
                        static std::unordered_set<const void*> loggedBaseBindings;
                        static std::unordered_set<std::string> loggedBaseBindingsByPath;
                        if (logOnceForMaterial(loggedBaseBindings, loggedBaseBindingsByPath))
                            LOG_INFO(Graphics, "Wgpu material debug: Stage0 BaseColour resolved path={} uv={}",
                                     loadName, base->uvSource);
                    }
                    else
                    {
                        // A model whose face texture is a `_mlod_co` placeholder has no usable
                        // fallback: if this bind fails the section draws white, which is what the
                        // Arma 3 castle and brick props do. Logging only the success path is why
                        // that was invisible, so say so once per material.
                        static std::unordered_set<const void*> loggedBaseFailures;
                        static std::unordered_set<std::string> loggedBaseFailuresByPath;
                        if (logOnceForMaterial(loggedBaseFailures, loggedBaseFailuresByPath))
                            LOG_WARN(Graphics,
                                     "Wgpu material: Stage0 BaseColour did NOT bind for {} (path={} loaded={} "
                                     "uploaded=0) -- section keeps its face texture",
                                     materialPath, loadName, wgpuBaseTexture ? 1 : 0);
                    }
                }
                else
                {
                    static std::unordered_set<const void*> loggedNoBaseSlot;
                    static std::unordered_set<std::string> loggedNoBaseSlotByPath;
                    if (logOnceForMaterial(loggedNoBaseSlot, loggedNoBaseSlotByPath))
                        LOG_INFO(Graphics,
                                 "Wgpu material: no bindable Stage0 for {} (present={} procedural={} emptyPath={} "
                                 "faceTexture={})",
                                 materialPath, base ? 1 : 0, base && base->texture.isProcedural ? 1 : 0,
                                 base && base->texture.path.empty() ? 1 : 0, d.texture_id != 0 ? 1 : 0);
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
                        _materialDebugInfo.normalStatus =
                            normalHandle ? (wgpuTexture->NormalIsRgPacked()
                                                ? "resolved and bound (RG normal)"
                                                : "resolved and bound (NOHQ: X=A, Y=G, Z=reconstructed)")
                                         : "texture failed to resolve/upload";
                        if (normalHandle)
                        {
                            static std::unordered_set<const void*> loggedNormalBindings;
                            static std::unordered_set<std::string> loggedNormalBindingsByPath;
                            if (logOnceForMaterial(loggedNormalBindings, loggedNormalBindingsByPath))
                                LOG_INFO(Graphics,
                                         "Wgpu material debug: Stage1 NormalMap resolved path={} uv={} "
                                         "decode={}",
                                         normal->texture.path.canonical(), normal->uvSource,
                                         wgpuTexture->NormalIsRgPacked() ? "X=R,Y=G" : "X=A,Y=G");
                        }
                        if (enableNormal)
                        {
                            d.normal_texture_id = normalHandle;
                            if (normalHandle && wgpuTexture->NormalIsRgPacked())
                                d.flags |= WGR_DRAW3D_NORMAL_RG;
                            if (normalHandle && wgpuTexture->NormalHasNativeCavity())
                                d.flags |= WGR_DRAW3D_NATIVE_CAVITY;
                        }
                    }
                }
                else
                    _materialDebugInfo.normalStatus = "no translated NormalMap stage";
                // PERF-016: record the per-material answers so the next section of this
                // material replays them above without the map find, the slot scans, or the
                // std::string builds. Production bindings only -- a debug selector keeps
                // every section on this full path -- and only after the whole resolution
                // succeeded, so an exception leaves nothing half-cached.
                if (!materialDebugTarget && SectionBindCacheEnabled())
                {
                    if (MaterialClass* bindSlot = MaterialClassSlot(section.surfMat))
                    {
                        bindSlot->baseKind = MaterialClass::BaseNone;
                        bindSlot->baseLoadName = RStringB();
                        if (baseIsImage)
                        {
                            bindSlot->baseKind = MaterialClass::BaseImage;
                            bindSlot->baseLoadName = RStringB(base->texture.path.canonical().c_str());
                        }
                        else if (base && base->texture.isProcedural &&
                                 render::procedural::Parse(base->texture.raw.c_str()).generated)
                        {
                            // The section-dependent half of DZ-006 (face texture present,
                            // CalmWater exception) is re-applied per section above; only the
                            // parseability of the constant is per-material.
                            bindSlot->baseKind = MaterialClass::BaseConstant;
                            bindSlot->baseLoadName = RStringB(base->texture.raw.c_str());
                        }
                        bindSlot->isCalmWater = Asset::Material::IsCalmWater(translated);
                        bindSlot->reflective = translated.shaderFamily == "CalmWater";
                        const auto* normalSlot = translated.Find(Asset::Material::MaterialSlot::NormalMap);
                        bindSlot->hasNormal =
                            normalSlot && !normalSlot->texture.isProcedural && !normalSlot->texture.path.empty();
                        bindSlot->normalLoadName = bindSlot->hasNormal
                                                       ? RStringB(normalSlot->texture.path.canonical().c_str())
                                                       : RStringB();
                        // The `.emat` `Color` surface tint (NetFence_Wire's
                        // dark authoring over a near-white BCR tile). A pure
                        // function of the material path like the rest here.
                        // NATIVE emats only: a legacy RVMAT's diffuse already
                        // reached _curMaterial through surfMat->Combine, so
                        // recording it here as well would tint twice.
                        if (Asset::Material::IsEmatPath(materialPath))
                        {
                            bindSlot->tint[0] = translated.diffuse[0];
                            bindSlot->tint[1] = translated.diffuse[1];
                            bindSlot->tint[2] = translated.diffuse[2];
                        }
                        bindSlot->bindResolved = true;
                        Pass1Submit::gSecBindResolves.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // This draw's tint: recorded above on the resolve that just ran
                // (bindSlot aliases the same slot when caching is enabled).
                if (MaterialClass* tintSlot = MaterialClassSlot(section.surfMat);
                    tintSlot && tintSlot->bindResolved)
                {
                    ematTint[0] = tintSlot->tint[0];
                    ematTint[1] = tintSlot->tint[1];
                    ematTint[2] = tintSlot->tint[2];
                }
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
        // The sibling naming convention remains the fallback. The shipped material catalog can
        // also supply an explicit normal path from its own PBO without replacing an original bank.
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
    static const bool traceDirectNormals = std::getenv("WGR_DIRECT_NORMAL_TRACE") != nullptr;
    if ((d.flags & WGR_DRAW3D_NORMAL_RG) != 0 && traceDirectNormals)
    {
        static unsigned int samples = 0;
        if (samples++ < 16)
            LOG_INFO(Graphics, "Direct RG normal bound: handle={} section={}", d.normal_texture_id, beg);
    }
    static const bool legacyDirectNormals = []
    {
        const char* value = std::getenv("WGR_LEGACY_DIRECT_NORMALS");
        return value && value[0] == '1';
    }();
    if (legacyDirectNormals)
        d.flags &= ~(WGR_DRAW3D_NORMAL_RG | WGR_DRAW3D_NATIVE_CAVITY);
    // DZ-003: the per-draw twin of the retained set's WGR_MODEL_SECTION_REFLECTIVE. A
    // material naming an EnvironmentMap gets a Fresnel-weighted sky reflection; every
    // other material is untouched. Folded into d.flags below alongside surface flags.
    //
    // DZ-006 gate, and it is a SAFETY property rather than a tweak: never add a reflection to a
    // surface whose albedo did not bind. `d.texture_id == 0` puts the section on the white
    // bindless fallback, and a sky reflection mixed into white is a bright wrong answer that
    // reads as snow, not water -- much harder to recognise as a defect than the dull unreflective
    // look it replaces. Ordered after MaterialIsReflective so its one-line-per-material
    // diagnostic still runs and the log still says which materials asked.
    p1sBind.Close();
    Pass1Submit::Scope p1sRefl(Pass1Submit::gSecReflNs);
    const bool reflectiveMaterial =
        materialBindingTarget && MaterialIsReflective(sMesh.GetSection(beg)) && d.texture_id != 0;
    if (materialBindingTarget && d.texture_id == 0 && MaterialIsReflective(sMesh.GetSection(beg)))
    {
        const char* rp = debugMaterialPath ? debugMaterialPath : "<none>";
        static std::unordered_set<std::string> loggedUnboundReflective;
        if (loggedUnboundReflective.insert(rp).second)
            LOG_WARN(Graphics,
                     "Wgpu material: reflection SUPPRESSED for {} -- its base colour did not bind, "
                     "so the section is on the white fallback and a reflection would blow it out",
                     rp);
    }
    p1sRefl.Close();
    // MAT-050 -- THE MISSING DIAGNOSTIC, and the correction that goes with it.
    //
    // Until now there was NO untextured warning on this path at all. The retained path has had
    // one since MAT-043 (`section draws UNTEXTURED`, in RegisterGpuModel), but a model is forced
    // OFF the retained path by the emitter bail (`StaticEmitterRadianceTarget(...) < 1.0f`),
    // which is exactly what every `\lamps\`, `\lights\`, `\runwaylights\` and `warninglight`
    // material matches -- so a lamp or a floodlight mast could never produce the one diagnostic
    // that would have named it, and the only model in a whole Stratis run to report UNTEXTURED
    // was an 18-face training plank that happened to stay retained.
    //
    // The two lines that DO exist here are per-MATERIAL, not per-section: `no bindable Stage0`
    // and `no albedo for` both dedup on the RVMAT path and fire at most once. A material whose
    // FIRST section carries a face texture logs `faceTexture=1` and is then silent forever --
    // including for every later section of that same material that has no face texture. That is
    // not a small blind spot: of the 2,595 graphical sections in the loaded Stratis corpus, 430
    // (600,873 faces, 45.6% of the world's geometry) carry no face texture at all and are
    // resolved by the material alone. This says, per section, when that resolution came back
    // with nothing -- and says WHY, which of "no material", "material bound nothing", "face
    // texture named but failed to upload" it was.
    //
    // Deduped on (shape address, section, RVMAT), matching the WGR_MATERIAL_DEBUG_RVMAT trace's
    // key so the two logs join. The COUNT is unconditional and reported on powers of two, for
    // the reason the per-material lines are insufficient: first occurrences are not a
    // denominator, and "how much of the screen is this" is the only question that decides
    // whether it matters.
    if (d.texture_id == 0)
    {
        ++_untexturedDirectDraws;
        // Scope guard (MAT-050): `debugMaterialPath` is non-null exactly when this section
        // names an RVMAT, which is the population whose white is a FAILURE rather than the
        // legacy multiply identity. A materialless CWA section is left on white unless
        // WGR_UNTEXTURED_ALBEDO=all explicitly asks otherwise.
        const bool sectionNamesMaterial = debugMaterialPath && *debugMaterialPath;
        const uint64_t neutral = UntexturedAlbedoHandle(sectionNamesMaterial);
        // Pointer-derived key, no string built: this runs per untextured section per FRAME, and
        // the pre-existing per-material logs went to pointer keys (WGR_DRAW_SECTION_ENV_CACHE)
        // for exactly this reason. A Shape's address is stable for the frame and the (address,
        // section) pair is what the WGR_MATERIAL_DEBUG_RVMAT=__trace__ log keys on too.
        const uint64_t key =
            reinterpret_cast<uintptr_t>(&sMesh) ^ (static_cast<uint64_t>(static_cast<uint32_t>(beg)) << 48);
        static std::unordered_set<uint64_t> loggedUntexturedDirect;
        // Capped, because a diagnostic that drowns the log it lives in is not readable. The
        // per-section identity of the FIRST 64 is what locates the offender; past that the
        // only useful fact is the size of the population, and the power-of-two counter below
        // carries that at a fixed cost. (A Shape's address is stable while its model stays
        // loaded, but it is an address, so a reload can re-key an already-reported section --
        // one more reason not to let the count of these lines grow without bound.)
        //
        // The insert is UNCONDITIONAL so `distinct sections` in the counter line stays a real
        // denominator; only the naming is capped.
        constexpr size_t untexturedDirectLogCap = 64;
        const bool firstForSection = loggedUntexturedDirect.insert(key).second;
        if (firstForSection && loggedUntexturedDirect.size() <= untexturedDirectLogCap)
        {
            auto*       faceTexture = static_cast<TextureWgpu*>(sMesh.GetSection(beg).properties.GetTexture());
            const char* faceName = faceTexture && faceTexture->Name() ? faceTexture->Name() : "";
            // Four distinct causes, and they need four different fixes. Naming which one it
            // was is the whole point: "untextured" alone sent the last investigation to the
            // texture files, where nothing was wrong.
            const char* why = "no face texture and the RVMAT bound no albedo";
            if (!debugMaterialPath && !faceTexture)
                why = "section has neither an RVMAT nor a face texture";
            else if (!debugMaterialPath)
                why = "no RVMAT, and the face texture did not upload";
            else if (*faceName)
                why = "RVMAT bound no albedo, and the named face texture did not upload";
            else if (faceTexture)
                why = "RVMAT bound no albedo, and the face texture has an empty name";
            LOG_WARN(Graphics,
                     "Wgpu material: per-draw section draws UNTEXTURED (fallback={}) shape={} section={} rvmat={} "
                     "face={} indices={} why=\"{}\" untexturedDirectDraws={}",
                     neutral ? "neutral" : "none", static_cast<const void*>(&sMesh), beg,
                     debugMaterialPath ? debugMaterialPath : "<none>", *faceName ? faceName : "<none>", indexCount, why,
                     _untexturedDirectDraws);
        }
        else if ((_untexturedDirectDraws & (_untexturedDirectDraws - 1)) == 0)
        {
            LOG_WARN(Graphics, "Wgpu material: per-draw UNTEXTURED draws so far: {} (distinct sections: {})",
                     _untexturedDirectDraws, loggedUntexturedDirect.size());
        }
        // Substitute the neutral stand-in. Placed AFTER `reflectiveMaterial` above, so a
        // fallback albedo still counts as "did not bind" for the DZ-006 reflection guard.
        if (neutral)
        {
            d.texture_id = neutral;
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
    Pass1Submit::Scope p1sAlpha(Pass1Submit::gSecAlphaNs);
    render::BlendMode blend = desc.blend;
    float alphaRef = AlphaRefFromDesc(desc);
    // MAT-NET-001: set when the coverage-mask rule below re-routes a Blend-classified
    // punch-through mask (netting) to a depth-WRITING alpha test. The blend revisit pass
    // has already demoted `desc.depth` to ReadOnly (GBlendSectionDepthReadOnly), so the
    // blend flip alone would leave the surface out of the depth buffer, which is the whole
    // bug; the depth mode has to be restored explicitly.
    bool coverageMaskDepthWrite = false;
    // TreeAdv leaf cards are authored with antialiased alpha, which the texture
    // histogram calls Blend. Their RVMAT family is nevertheless alpha-tested;
    // without this matching discard the black transparent texels depth-write in
    // the opaque fallback and completely hide the pine's trunk behind its cards.
    // TreeAdvTrunk is intentionally excluded: its bark map has non-coverage alpha
    // and must remain a solid, untested draw.
    auto* texture = static_cast<TextureWgpu*>(sMesh.GetSection(beg).properties.GetTexture());
    const bool materialUsesLeafCards =
        alphaClass == Poseidon::AlphaStats::Blend && GpuSectionMaterialUsesLeafCards(debugSection.surfMat);
    // An OpacityMap material that authors `BlendMode AlphaBlend` (wire mesh,
    // glass) blends; testing it would keep what the author meant translucent.
    const bool materialBlends = GpuEmatBlends(debugSection.surfMat);
    const bool materialCutout =
        materialUsesLeafCards && !materialBlends && GpuSectionMaterialIsCutout(debugSection.surfMat, texture);
    if (GpuTextureIsBarkOrTrunk(texture))
    {
        // The common legacy section descriptor itself can carry a stale alpha
        // reference even for TreeAdvTrunk.  Texture role is authoritative for
        // these bark maps, so remove that inherited discard unconditionally.
        blend = render::BlendMode::Opaque;
        alphaRef = 0.0f;
    }
    else if (materialUsesLeafCards && !materialCutout && !materialBlends)
    {
        // A close tree LOD may use the generic TreeAdv material for BOTH its needle cards and
        // its bark. The bark texture has a non-coverage alpha channel, so a leaf alpha test
        // removes it. It still belongs in the opaque pass, just without the discard.
        // Authored translucency (OpacityMap + BlendMode AlphaBlend: wire mesh, glass)
        // is excluded: forcing it opaque here drew NetFence_Wire as a solid sheet.
        blend = render::BlendMode::Opaque;
        alphaRef = 0.0f;
    }
    else if (tex != 0 && (alphaClass == Poseidon::AlphaStats::Cutout || materialCutout ||
                          (texture && (render::IsLegacyPlantCutoutTexture(texture->Name()) ||
                                       (render::IsEnfusionCoverageTexture(texture->Name()) &&
                                        texture->HasAlphaHoles() && !materialBlends)))))
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
    else if (blend == render::BlendMode::AlphaBlend && tex != 0 && texture && alphaClass == Poseidon::AlphaStats::Blend &&
             !texture->HasAlphaHoles() && MaterialIsFullyOpaque(_curMaterial) &&
             GpuEmatIsOpaqueSolid(debugSection.surfMat))
    {
        // The Blend-class twin of the Opaque fix above, for the sections the
        // retained path declines: a solid Enfusion wall/roof over a
        // roughness-alpha BCR (RFG-033) is opaque, not translucent. The
        // no-holes test is what keeps decals and cutouts out -- their alpha
        // has real clear texels -- and the family allow-list inside
        // GpuEmatIsOpaqueSolid keeps glass and foliage out.
        blend = render::BlendMode::Opaque;
        alphaRef = 0.0f;
    }
    else if (_materialDebug.coverageMaskDepth && blend == render::BlendMode::AlphaBlend && tex != 0 && texture &&
             alphaClass == Poseidon::AlphaStats::Blend && MaterialIsFullyOpaque(_curMaterial) &&
             IsCoverageMaskAlpha(texture->AlphaStatsView()))
    {
        // MAT-NET-001, the owner's "things behind transparent netting look strange".
        //
        // Netting (chain-link, barbed wire, camo net) is a punch-through MASK whose edges were
        // antialiased and then quantised into ARGB4444's 16 alpha levels. The histogram sees
        // those edge texels as partial alpha and calls the whole texture Blend, so the section
        // goes to the back-to-front revisit pass, which draws depth-test-only. Measured on the
        // stock CWA corpus (Dta/Data.pbo), every one of them:
        //     pletivo_opak.paa       (pletivo.p3d, the chain-link panel)  88.0% clear, 10.6% partial
        //     pletivo_opak_dark32    (pletivo_wired.p3d)                  81.1% clear, 15.3% partial
        //     barbedwire_plot.paa    (pletivo_wired.p3d)                  85.1% clear,  8.7% partial
        //     maskovaci_sit_new.paa  (camo net)                           39.9% clear, 32.7% partial
        //     m1a1_dratram_side.paa  (tank wire screen)                   73.2% clear, 14.4% partial
        // and mask_sit_2x1_ridka has a DXT1 twin that already classifies CUTOUT -- the same
        // mask, authored twice, routed two different ways purely by container.
        //
        // Not writing depth is not merely a sorting nicety here: the temporal upscaler's
        // velocity is reconstructed from the DEPTH BUFFER ALONE (rust/src/temporal.wgsl
        // fs_velocity), so a netting pixel is handed the motion of whatever is BEHIND it. A
        // fence at 3 m over a background at 100 m parallaxes ~30x faster than the velocity it
        // is given, and the history-control mask (fs_history_control) covers only sky and
        // water, so DLSS is told to trust that history fully. The wire pattern smears across
        // whatever is seen through it -- worst at the 67% render scale the DLSS route enables
        // by default. GTAO has the same shape of error: gtao_ao() is a screen-space fetch, so
        // the wire is shaded with the ambient occlusion of the geometry behind it.
        //
        // The rule is the mirror image of the MAT-051 glass rule below (pctClear < 5, mostly
        // partial): a real mask has a substantial share of FULLY CLEAR texels and only a
        // minority of partial ones -- the partials are the perimeter of the holes, not the
        // body of the surface. Glass, smoke and genuine fades fail it on both counts.
        // Treat it exactly as the Cutout class above is treated -- hard discard, opaque
        // blend, MSAA alpha-to-coverage still antialiases the wire -- and restore the depth
        // WRITE the revisit pass took away, which is the half that fixes the depth consumers.
        blend = render::BlendMode::Opaque;
        alphaRef = std::max(alphaRef, 0.5f);
        coverageMaskDepthWrite = true;
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
    d.depth = coverageMaskDepthWrite ? WGR_DEPTH_TEST_WRITE : DepthFromDesc(desc.depth);
    d.alpha_ref = alphaRef;
    p1sAlpha.Close();
    if (LegacyTextureTraceEnabled())
    {
        if (const Texture* t = sMesh.GetSection(beg).properties.GetTexture())
        {
            const char* name = t->Name();
            if (name && *name)
            {
                static std::unordered_set<std::string> loggedLegacyDrawModes;
                if (loggedLegacyDrawModes.insert(name).second)
                    LOG_INFO(Graphics,
                             "Legacy texture draw trace: path={} vegetation={} alphaClass={} alphaRef={:.2f} blend={} depth={} cam={} cmd={} "
                             // The four alphas MaterialIsFullyOpaque tests, because a surface that
                             // reaches AlphaBlend over an Opaque-classified texture was refused by
                             // exactly this guard and the line could not say which of them did it.
                             "matA=dif{:.3f}/amb{:.3f}/emi{:.3f}/forced{:.3f} fullyOpaque={} "
                             "ambRGB={:.2f},{:.2f},{:.2f} fdRGB={:.2f},{:.2f},{:.2f} emiRGB={:.2f},{:.2f},{:.2f}",
                             name, GCurrentIsVegetation, int(alphaClass), d.alpha_ref, int(d.blend), int(d.depth), _currentCamera, _cmds.size(),
                             _curMaterial.diffuse.A(), _curMaterial.ambient.A(), _curMaterial.emmisive.A(),
                             _curMaterial.forcedDiffuse.A(), MaterialIsFullyOpaque(_curMaterial) ? 1 : 0,
                             _curMaterial.ambient.R(), _curMaterial.ambient.G(), _curMaterial.ambient.B(),
                             _curMaterial.forcedDiffuse.R(), _curMaterial.forcedDiffuse.G(),
                             _curMaterial.forcedDiffuse.B(), _curMaterial.emmisive.R(), _curMaterial.emmisive.G(),
                             _curMaterial.emmisive.B());
            }
        }
    }
    // Polygon-offset selection (ignored for shadows, which get their own offset):
    //  - OnSurface routing (roads / footprint decals): the light decal offset.
    //  - Otherwise ZBias overlay faces (traffic-sign decals etc.) flagged via
    //    SetBias(level*5): a stronger, level-scaled offset. GL33 leaves these to a
    //    biased projection it never actually applies for HW-T&L, so they z-fight in
    //    wgpu; the level-scaled offset resolves them without over-biasing roads.
    // d was zero-initialised; preserve the normal-channel bit from material binding.
    // The LLDR's authored translucent screen is a display, not an open hole.
    if (texture && texture->Name() &&
        std::strstr(texture->Name(), "lldr_monitor.paa") != nullptr)
    {
        d.flags |= WGR_DRAW3D_MONITOR;
    }
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
    // MAT-051: classic glass gets the sky-reflection sheen. An OFP/A1/A3
    // canopy pane is an alpha-BLENDED section whose texture is almost entirely
    // PARTIAL alpha (uh60_skla_ca: 99% partial, mean 29/255) over a fully
    // opaque material — an ~11% tint that composites into invisibility against
    // sky, which is the owner's "cockpit is transparent" on every bridged
    // helicopter. GL33 reads because its path lights the pane with a specular
    // sheen; here the same job is done by the existing DZ-003 reflective term
    // (Schlick at ENV_F0), which is also what glass physically is: a
    // sky-mirror. The key is the alpha HISTOGRAM, not the class alone:
    //   - pctClear < 5   excludes light flares and halos (mostly clear texels),
    //   - pctPartial > 80 demands genuine continuous translucency,
    //   - aMean < 128    excludes decals that are mostly solid with soft seams,
    //   - MaterialIsFullyOpaque keeps material-driven fades (real translucency
    //     effects) out — the same guard the opaque-head fix above uses.
    // DZ-003's warning about env-SLOT keying (157 ordinary Super materials
    // name an env map) does not apply: no material slot is consulted at all.
    // WGR_GLASS_SHEEN=0 restores the old look for an A/B in the same binary.
    bool glassSheen = false;
    {
        static const bool sheenEnabled = []
        {
            const char* v = std::getenv("WGR_GLASS_SHEEN");
            return !(v && std::strcmp(v, "0") == 0);
        }();
        if (sheenEnabled && !reflectiveMaterial && texture && tex != 0 &&
            alphaClass == Poseidon::AlphaStats::Blend && blend == render::BlendMode::AlphaBlend &&
            MaterialIsFullyOpaque(_curMaterial))
        {
            const Poseidon::AlphaStats& st = texture->AlphaStatsView();
            glassSheen = Poseidon::LegacyGlassAlphaIsGlazing(desc.surface == render::SurfaceMode::OnSurface,
                                                            st.pctClear, st.pctPartial, st.aMean);
        }
    }
    // DZ-003, resolved above (see reflectiveMaterial).
    if (reflectiveMaterial || glassSheen)
    {
        d.flags |= WGR_DRAW3D_REFLECTIVE;
    }
    // MAT-051 second half (owner: "cockpit is still transparent" from the pilot
    // seat): the sheen is Fresnel-weighted, so it vanishes at normal incidence
    // -- exactly the inside view. The GLASS bit floors the pane's output alpha
    // in the shader so it keeps a minimum head-on presence. Glass only; water
    // (reflectiveMaterial) keeps its own alpha untouched.
    if (glassSheen)
    {
        d.flags |= WGR_DRAW3D_GLASS;
    }
    // Enfusion `Cull none`: single-sided-authored surface (wire mesh, glass)
    // drawn double-sided. The per-draw pipeline drops backface culling for the
    // draw; the retained path keeps culling (its sections are closed solids).
    if (GpuEmatCullNone(debugSection.surfMat))
    {
        d.flags |= WGR_DRAW3D_DOUBLE_SIDED;
    }
    // MAT-052 second half: the engine scopes ENCLOSED first-person interiors with
    // PassKindHint::Cockpit (the camera vehicle's inside LOD in DrawSortObject).
    // Carry that scope to the shader so the interior takes a damped sky ambient --
    // see WGR_DRAW3D_COCKPIT in wgpu_renderer.hpp for the full story. The soldier's
    // own body and held weapon carry PassKindHint::FirstPersonBody instead: same late
    // pass, but they stand in open sky and must NOT be damped or view-normal-flipped.
    if (GetPassKindHint() == render::PassKindHint::Cockpit)
    {
        d.flags |= WGR_DRAW3D_COCKPIT;
    }
    d.camera = _currentCamera;
    d.palette_slot = _currentPaletteSlot;

    // Per-material sun lighting, folded exactly like GL33's
    // UploadVSMaterialConstants: raw MainLight colour x captured material, with
    // the sun-enable (!DisableSun) multiplied into the sun terms (emissive shows
    // regardless). The lit shader does emissive + sun_ambient + sun_diffuse * N.L,
    // clamped, x texture — the per-fragment analogue of GL33's VSNormal + PSNormal.
    Pass1Submit::Scope p1sLight(Pass1Submit::gSecLightNs);
    Color sunDif = HWhite;
    Color sunAmb = HWhite;
    {
        // RenderSnapshot S1: the frame's captured sun colours, not a mid-draw live read.
        const render::frame::PresentationSnapshot& matSnap = render::frame::GPresentationSnapshot();
        if (matSnap.hasMainLight)
        {
            sunDif = matSnap.sunDiffuse;
            sunAmb = matSnap.sunAmbient;
        }
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
        // WGR_NO_EMISSIVE=1 zeroes the sun-independent term for everything on this
        // path. Diagnostic only: emissive is the one lane that survives sunset, so
        // if a surface stays lit at midnight this gate says in one run whether the
        // emissive lane is what is lighting it. It gates the VALUE only -- the
        // night-emitter FLAG below is a separate decision about which fixtures keep
        // their emissive after dark, and ablating one must not silently move the other.
        static const bool suppressEmissive = []
        {
            const char* value = std::getenv("WGR_NO_EMISSIVE");
            return value != nullptr && value[0] != '\0' && value[0] != '0';
        }();
        const float emissiveGate = suppressEmissive ? 0.0f : 1.0f;
        d.mat_emissive = {emissive.R() * scale * emissiveGate, emissive.G() * scale * emissiveGate,
                          emissive.B() * scale * emissiveGate, emissive.A()};
        // Fixtures keep their emissive through the night; everything else (vegetation
        // included) has it faded out with the sun in the shader.
        if (MaterialEmitsAtNight(debugSection.surfMat))
        {
            d.flags |= WGR_DRAW3D_NIGHT_EMITTER;
        }
    }
    d.mat_sun_ambient = {ambient.R() * sunEn * ematTint[0], ambient.G() * sunEn * ematTint[1],
                         ambient.B() * sunEn * ematTint[2], ambient.A() * sunEn};
    d.mat_sun_diffuse = {diffuse.R() * sunEn * ematTint[0], diffuse.G() * sunEn * ematTint[1],
                         diffuse.B() * sunEn * ematTint[2], diffuse.A() * sunEn};
    // Point/spot-light material modulation (GL33's matDif/matAmb): the raw material
    // diffuse/ambient (accommodation in, night rides the per-light colour).
    // The `.emat` `Color` tint rides all four albedo lanes, sun and local:
    // it is the surface's own colour, not a property of any one light.
    const Color& lightDif = _curMaterial.diffuse;
    const Color& lightAmb = _curMaterial.ambient;
    d.mat_light_diffuse = {lightDif.R() * ematTint[0], lightDif.G() * ematTint[1], lightDif.B() * ematTint[2],
                           lightDif.A()};
    d.mat_light_ambient = {lightAmb.R() * ematTint[0], lightAmb.G() * ematTint[1], lightAmb.B() * ematTint[2],
                           lightAmb.A()};
    // Sun-only Blinn-Phong specular, folded exactly like GL33's c18 (specCol =
    // sun->Diffuse() * mat.specular, power in w). The shader adds it per-fragment,
    // gated on w > 0, so fold the sun-enable into the colour and leave w = power.
    const Color specCol = sunDif * _curMaterial.specular;
    const float specPow = float(_curMaterial.specularPower);
    const float specEn = (specPow > 0.0f) ? sunEn : 0.0f;
    d.mat_specular = {specCol.R() * specEn, specCol.G() * specEn, specCol.B() * specEn, specPow};
    const Color& localSpec = _curMaterial.specular;
    d.mat_local_specular = {localSpec.R(), localSpec.G(), localSpec.B(), 0.0f};
    // Foliage class for the direct WGPU path: only real plants get the leaf look, and the vertex
    // shader receives tree vs bush so it can use the matching radial-crown normal. This rides the
    // otherwise unused .w lane; fs_main treats any positive value as vegetation.
    d.mat_sun_ambient.w = GCurrentFoliageKind;
    p1sLight.Close();

    // WGR_KILL_TEX=<comma-separated substrings>: DROP every draw whose texture path
    // contains one of them. Diagnostic only, and the decisive one: whatever vanishes
    // from the frame is what this path was drawing there -- a question no additive
    // trace can answer, because the trace names every texture in the frame without
    // saying where any of them landed. Binary-search a texture list against a
    // screenshot and the surface identifies itself in a handful of runs; a surface
    // that does NOT change under this door was never drawn by this path at all,
    // which is the observation that cracked MAT-052 after five shading theories
    // died. Never set in normal play.
    {
        static const std::string tintTex = []
        {
            const char* v = std::getenv("WGR_KILL_TEX");
            return std::string(v ? v : "");
        }();
        if (!tintTex.empty())
        {
            if (const Texture* t = sMesh.GetSection(beg).properties.GetTexture())
            {
                const char* name = t->Name();
                if (name && *name)
                {
                    std::string lower(name);
                    for (char& c : lower)
                        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    size_t from = 0;
                    while (from <= tintTex.size())
                    {
                        const size_t to = tintTex.find(',', from);
                        const std::string token = tintTex.substr(from, to - from);
                        if (!token.empty() && lower.find(token) != std::string::npos)
                        {
                            return;
                        }
                        if (to == std::string::npos)
                            break;
                        from = to + 1;
                    }
                }
            }
        }
    }

    _draws3d.push_back(d);
    // REN-THR-011 -- remembered by INDEX and patched after the drain. Registered here rather
    // than where d.mesh was filled because several doors above (WGR_KILL_TEX among them) can
    // still drop the draw, and an entry for a record that was never pushed would patch a
    // stranger. push_back only appends, so the index stays valid until the frame is published.
    if (d.mesh == 0 && buf->slot)
    {
        _draws3dPendingMesh.push_back(PendingMeshRef{U32(_draws3d.size() - 1), buf->slot});
    }
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
// PERF-015 front cache. The string-keyed cache below is correct but costs a std::string
// construction, a mutex acquisition and a string hash on EVERY section of EVERY draw -- it
// removed the RVMAT parse, not the lookup. This removes the lookup too, and falls through to
// it (and so to the parse) on the first section of each distinct material.
bool GpuSectionMaterialUsesLeafCards(const Ref<TexMaterial>& surfMat)
{
    if (MaterialClass* cached = MaterialClassSlot(surfMat))
    {
        if (!cached->leafCardsResolved)
        {
            cached->leafCards = GpuSectionMaterialUsesLeafCardsUncached(surfMat);
            cached->leafCardsResolved = true;
        }
        return cached->leafCards;
    }
    return GpuSectionMaterialUsesLeafCardsUncached(surfMat);
}

bool GpuSectionMaterialUsesLeafCardsUncached(const Ref<TexMaterial>& surfMat)
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
        if (Asset::Material::IsEmatPath(key))
        {
            // Reforger: the section names an `.emat`, which ParseRvMaterialFile cannot
            // read (it throws, and every Everon section landed in the catch below with
            // `cutout=false`). Its own class answers the same question TreeAdv answers
            // for RV -- see EmatUsesLeafCards for what counts and the measurement.
            Asset::Material::EmatMaterial emat;
            cutout = Asset::Material::ReadEmatFile(key, emat) && Asset::Material::EmatUsesLeafCards(emat);
        }
        else
        {
            const std::string& family = Asset::Material::ParseRvMaterialFile(key).pixelShaderId;
            cutout = family == "TreeAdv" || family == "TreeAdvTrans";
        }
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
bool GpuTextureIsBarkOrTrunkUncached(const TextureWgpu* texture)
{
    const char* path = texture ? texture->Name() : nullptr;
    return path && (ContainsNoCase(path, "\\_bark\\") || ContainsNoCase(path, "bark_") ||
                    ContainsNoCase(path, "_bark") || ContainsNoCase(path, "\\trunk") ||
                    ContainsNoCase(path, "_trunk"));
}

// PERF-015: five substring scans of a full texture path, per section, per frame. Same
// address-keyed / path-validated scheme as MaterialClassSlot, for the same reason.
bool GpuTextureIsBarkOrTrunk(const TextureWgpu* texture)
{
    const char* path = texture ? texture->Name() : nullptr;
    if (!SectionClassCacheEnabled() || !path || !*path)
        return GpuTextureIsBarkOrTrunkUncached(texture);
    struct TextureClass
    {
        std::string path;
        bool        barkOrTrunk = false;
    };
    thread_local std::unordered_map<const void*, TextureClass> cache;
    TextureClass& slot = cache[static_cast<const void*>(texture)];
    Pass1Submit::gSecClassCalls.fetch_add(1, std::memory_order_relaxed);
    if (slot.path.empty() || slot.path != path)
    {
        Pass1Submit::gSecClassMisses.fetch_add(1, std::memory_order_relaxed);
        slot.path = path;
        slot.barkOrTrunk = GpuTextureIsBarkOrTrunkUncached(texture);
    }
    return slot.barkOrTrunk;
}

bool GpuSectionMaterialIsCutout(const Ref<TexMaterial>& surfMat, TextureWgpu* texture)
{
    return texture != nullptr && !GpuTextureIsBarkOrTrunk(texture) && texture->HasAlphaHoles() &&
           GpuSectionMaterialUsesLeafCards(surfMat);
}

// The `.emat` shader family, cached by material path (empty when the section
// names none or it cannot be read). One file read per material, like the
// leaf-cards answer above.
std::string GpuEmatFamily(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return {};
    const char* path = surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return {};
    static std::unordered_map<std::string, std::string> cache;
    static std::mutex cacheMutex;
    const std::string key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto found = cache.find(key); found != cache.end())
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

// Whether a section's `.emat` is classic glass (dirt coverage over a pane):
// an OpacityMap is present, so EmatUsesLeafCards claims it, but its
// BlendMode is AlphaBlend and it must blend, never test opaque.
bool GpuEmatIsGlass(const Ref<TexMaterial>& surfMat)
{
    const std::string family = GpuEmatFamily(surfMat);
    return !family.empty() && Asset::Material::EmatMaterial::NameMatches(family, "MatPBRBasicGlass");
}

bool GpuEmatBlends(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return false;
    static std::unordered_map<std::string, bool> cache;
    static std::mutex cacheMutex;
    const std::string key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto found = cache.find(key); found != cache.end())
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

// Whether a section's `.emat` is a solid building/prop surface (see
// EmatIsOpaqueSolid): its BCR alpha is roughness, never coverage. Cached by
// material path like the leaf-cards answer above, for the same reason.
bool GpuEmatIsOpaqueSolid(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return false;
    static std::unordered_map<std::string, bool> cache;
    static std::mutex cacheMutex;
    const std::string key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto found = cache.find(key); found != cache.end())
            return found->second;
    }
    bool solid = false;
    try
    {
        Asset::Material::EmatMaterial emat;
        solid = Asset::Material::ReadEmatFile(key, emat) && Asset::Material::EmatIsOpaqueSolid(emat);
    }
    catch (...)
    {
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = solid;
    return solid;
}

// Whether a section's `.emat` names `Cull none` (wire mesh, glass panes).
// Same cached shape as the two answers above.
bool GpuEmatCullNone(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return false;
    static std::unordered_map<std::string, bool> cache;
    static std::mutex cacheMutex;
    const std::string key(path);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto found = cache.find(key); found != cache.end())
            return found->second;
    }
    bool cullNone = false;
    try
    {
        Asset::Material::EmatMaterial emat;
        if (Asset::Material::ReadEmatFile(key, emat))
        {
            if (const auto* cull = emat.Find("Cull"); cull && !cull->values.empty() && !cull->values[0].isNumber)
                cullNone = Asset::Material::EmatMaterial::NameMatches(cull->values[0].text, "none");
        }
    }
    catch (...)
    {
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = cullNone;
    return cullNone;
}


// RFG-074: name the classifier's refusals, once per (reason, texture). Every one of
// these was a bare `return false`, and the church's ten opaque wall sections went
// through one of them unseen while the census reported the layer path healthy.
static void TraceClassifyReject(const ShapeSection& sec, const char* why)
{
    static std::unordered_set<std::string> said;
    auto* face = static_cast<TextureWgpu*>(sec.properties.GetTexture());
    const char* faceName = face && face->GetName().Data() ? face->GetName().Data() : "(none)";
    const char* mat = sec.surfMat && sec.surfMat->GetName().Data() ? sec.surfMat->GetName().Data() : "(null)";
    if (said.insert(std::string(why) + "|" + faceName).second)
        LOG_INFO(Graphics, "Wgpu material: classifier REFUSED ({}) surfMat='{}' face='{}'", why, mat, faceName);
}

// One line per `.emat` behind a Blend-class section, saying WHY the opaque
// route took or declined it: family, solid-surface verdict, alpha holes, the
// folded material alphas, and the texture class. The transparent-wall/rock
// question ("which guard failed?") is answered here, not by guessing.
static void TraceEmatBlendDecision(const ShapeSection& sec, TextureWgpu* texture, const TLMaterial& m, bool accepted)
{
    if (sec.surfMat.IsNull())
        return;
    const char* path = sec.surfMat->GetName().Data();
    if (!path || !*path || !Asset::Material::IsEmatPath(path))
        return;
    static std::unordered_set<std::string> said;
    if (!said.insert(std::string(path) + (accepted ? "|a" : "|r")).second)
        return;
    const auto alphaClass = texture ? texture->GetAlphaClass() : Poseidon::AlphaStats::Opaque;
    LOG_INFO(Graphics,
             "Wgpu material: {} opaque-roughness route for '{}' family='{}' solid={} holes={} class={} "
             "matAlpha={:.2f},{:.2f},{:.2f}",
             accepted ? "ACCEPTED" : "DECLINED", path, GpuEmatFamily(sec.surfMat).c_str(),
             GpuEmatIsOpaqueSolid(sec.surfMat) ? 1 : 0, texture && texture->HasAlphaHoles() ? 1 : 0,
             alphaClass == Poseidon::AlphaStats::Blend ? "blend"
             : alphaClass == Poseidon::AlphaStats::Cutout ? "cutout"
                                                          : "opaque",
             m.diffuse.A(), m.ambient.A(), m.emmisive.A());
}

bool ClassifyGpuSection(const Shape& s, uint64_t mesh, const AutoArray<render::mesh::MeshSection>& secs, int i,
                        WgrModelSection& secOut, WgrModelMaterial& matOut)
{
    if (i >= secs.Size())
    {
        // (index out of range: no section to name)

        return false;
    }
    const ShapeSection& sec = s.GetSection(i);
    const int spec = sec.properties.Special();
    const render::LegacySpec split = render::SplitLegacy(spec);
    render::BuildContext ctx;
    ctx.isIn3DPass = true;
    ctx.shadowAlphaRef = 0;
    const render::RenderPassDescriptor desc = render::BuildRenderPassDescriptor(split, ctx);
    // The same ownership question Shape::Draw's skip and RegisterGpuModel ask (ClipShape.hpp).
    // Answering it differently here is how a section ends up drawn by nobody.
    if (!SectionGpuOwned(sec))
    {
        TraceClassifyReject(sec, "SectionGpuOwned false");

        return false;
    }

    const render::mesh::MeshSection& ms = secs[i];
    const int indexCount = ms.end - ms.beg;
    if (indexCount <= 0)
    {
        TraceClassifyReject(sec, "no indices");

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
    // Legacy A1 plants have no authored shader to consult, so the same per-LOD histogram slip
    // that fooled SectionIsBlend would push half a canopy's LODs out of the retained set.
    // Treat them as the alpha-tested cards they are, on BOTH paths, or the two disagree and a
    // section is drawn twice or not at all (see IsGpuOwnedSection).
    const bool legacyPlantCutout = texture && (render::IsLegacyPlantCutoutTexture(texture->Name()) ||
                                               (render::IsEnfusionCoverageTexture(texture->Name()) &&
                                                texture->HasAlphaHoles() && !GpuEmatBlends(sec.surfMat)));
    const bool materialCutout =
        alphaClass == Poseidon::AlphaStats::Blend && !GpuEmatBlends(sec.surfMat) &&
        GpuSectionMaterialIsCutout(sec.surfMat, texture);
    // Mirrors DrawSectionTL's three rules, in the same order, so the retained path shades a tree
    // exactly as the per-draw path did (2026-09-02, the pine-trunk regression):
    //  1. bark / trunk textures are SOLID whatever their alpha channel says -- their alpha is not
    //     coverage, and testing it is what hid the trunk behind its own cards;
    //  2. a Blend-class texture under an authored leaf-card material with no alpha holes (bark
    //     under the generic TreeAdv material) is drawn opaque without a discard;
    //  3. Cutout-class, legacy plant cutout, leaf-card cutout and IsAlpha-spec cutout textures
    //     are hard-tested at 0.5.
    //  4. A solid Enfusion wall/roof over a roughness-alpha BCR (RFG-033, hole-less,
    //     opaque-solid family) is drawn opaque without a discard -- same shape as 2.
    // A Blend-class texture that is none of those is real translucency; declined
    // here, complement-drawn, and SectionGpuOwned refuses it the same way so no
    // section is drawn twice or not at all.
    const bool bark = texture && GpuTextureIsBarkOrTrunk(texture);
    // Glass excluded: its OpacityMap claims leaf-cards, but its BlendMode is
    // AlphaBlend and it must blend, never test opaque (P10 greenhouse). The
    // same holds for any OpacityMap material that authors blending (wire
    // mesh): cutout would test what the author meant to blend.
    const bool blendOpaqueRoute = alphaClass == Poseidon::AlphaStats::Blend && !bark && !materialCutout &&
                                  !legacyPlantCutout && !GpuEmatIsGlass(sec.surfMat) && !GpuEmatBlends(sec.surfMat) &&
                                  GpuSectionMaterialUsesLeafCards(sec.surfMat);
    // Raw per-section material first: the alpha guards below consult it, mirroring
    // the per-draw path (whose Opaque-class branch this twins). Base = the
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
    // A Blend-class texture that is none of those is real translucency -- except a
    // solid Enfusion wall/roof over a roughness-alpha BCR (RFG-033): opaque, like
    // bark. The no-holes test keeps decals and cutouts out, the family
    // allow-list keeps glass and foliage out, and SectionGpuOwned answers the
    // same question on the CPU side so no section is drawn twice or not at all.
    const bool enfusionOpaqueRoughness = alphaClass == Poseidon::AlphaStats::Blend && !bark && !materialCutout &&
                                         !legacyPlantCutout && !blendOpaqueRoute && tex != 0 && texture &&
                                         !texture->HasAlphaHoles() && MaterialIsFullyOpaque(m) &&
                                         GpuEmatIsOpaqueSolid(sec.surfMat);
    if (alphaClass == Poseidon::AlphaStats::Blend && !bark && !materialCutout && !legacyPlantCutout &&
        !blendOpaqueRoute && !enfusionOpaqueRoughness)
    {
        TraceClassifyReject(sec, "blend/opaque route");
        TraceEmatBlendDecision(sec, texture, m, false);
        return false;
    }
    if (enfusionOpaqueRoughness)
        TraceEmatBlendDecision(sec, texture, m, true);
    float alphaRef = AlphaRefFromDesc(desc);
    if (bark || blendOpaqueRoute || enfusionOpaqueRoughness)
    {
        alphaRef = 0.0f;
    }
    else if (tex != 0 && alphaClass == Poseidon::AlphaStats::Opaque && MaterialIsFullyOpaque(m))
    {
        // The per-draw Opaque-class guard's missing twin: an IsAlpha spec over an
        // opaque texture of a fully opaque material is not a cutout. Without it
        // the spec's Test+AlphaBlend desc promotes the section to cutout variant
        // and the shader discards/A2Cs it by its roughness.
        alphaRef = 0.0f;
    }
    else if (tex != 0 && (alphaClass == Poseidon::AlphaStats::Cutout || materialCutout || legacyPlantCutout ||
                          desc.blend == render::BlendMode::AlphaBlend))
    {
        alphaRef = std::max(alphaRef, 0.5f);
    }
    // Cull-none cutouts (wire mesh) stay CPU-drawn: the material is authored
    // double-sided and the retained path culls backfaces. Vegetation is
    // excluded: its crossed cards were tuned for the culled GPU path, and the
    // per-draw foliage shading is not its twin. SectionGpuOwned declines them
    // the same way, so no section is drawn twice or not at all.
    const std::string cullFam = GpuEmatFamily(sec.surfMat);
    const bool cullVeg =
        !cullFam.empty() && Asset::Material::IsEmatVegetationFamily(cullFam);
    if (alphaRef > 0.0f && !cullVeg && GpuEmatCullNone(sec.surfMat))
    {
        TraceClassifyReject(sec, "cull-none cutout (CPU double-sided)");
        return false;
    }
    secOut.variant = alphaRef > 0.0f ? 1u : 0u;
    // Flags are filled by the caller (RegisterGpuModel): the reflective bit needs the
    // material translation cache, which is an EngineWgpu member this free function
    // cannot reach.
    secOut.flags = 0;
    // `m` was built above (the alpha guards consult it); folded with the sun below.
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
            if (IsNativeTreeWindModel(s->Name()))
                inst.flags |= WGR_INSTANCE_COHERENT_TREE_WIND;
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

// WGR_INSTANCE_TRACE: audit the retained scene's INSTANCE data.
//
// A retained instance is the one place where an ordinary, correctly registered model can be
// turned into an enormous stretched sheet: the vertex shader multiplies the mesh by this
// matrix and then displaces it by the conform words. A degenerate, mirrored or non-finite
// matrix, or a mode-1 plane carrying garbage coefficients, collapses the geometry toward a
// plane of arbitrary size -- and none of that is visible to the DrawSectionTL material trace,
// because the retained path never goes there.
//
// Rather than dumping every instance and asking for a sort, this classifies: anything that
// trips a bound is prefixed ANOMALY, and anything with a large world radius is logged too (on
// Takistan almost nothing legitimately exceeds ~50 m). `grep ANOMALY` should be the search.
static bool GpuInstanceFinite(float v)
{
    return v == v && v > -1e30f && v < 1e30f;
}

static void TraceGpuInstance(const char* where, const char* name, const Matrix4& world, float modelRadius,
                             const ConformPlane& cp, uint32_t model)
{
    static const bool trace = std::getenv("WGR_INSTANCE_TRACE") != nullptr;
    if (!trace)
    {
        return;
    }

    const Vector3 aside = world.DirectionAside();
    const Vector3 up = world.DirectionUp();
    const Vector3 dir = world.Direction();
    const Vector3 pos = world.Position();
    const float la = aside.Size(), lu = up.Size(), ld = dir.Size();
    const float det = aside * up.CrossProduct(dir); // signed volume of the basis
    const float scale = world.Scale();
    const float worldRadius = modelRadius * scale;

    bool anomaly = false;
    for (float v : {la, lu, ld, det, scale, worldRadius, pos.X(), pos.Y(), pos.Z()})
    {
        anomaly = anomaly || !GpuInstanceFinite(v);
    }
    // Degenerate (collapses the mesh onto a plane -> a sheet), mirrored, or wildly scaled.
    anomaly = anomaly || det <= 1e-6f || det > 1e4f;
    anomaly = anomaly || la < 1e-4f || lu < 1e-4f || ld < 1e-4f;
    anomaly = anomaly || la > 100.0f || lu > 100.0f || ld > 100.0f;
    // A bounding sphere no placed Takistan object should reach.
    anomaly = anomaly || worldRadius > 200.0f;
    // Mode-1 conform coefficients are land-grid heights; garbage here displaces vertices by
    // arbitrary amounts, which is the other way to manufacture a sheet.
    if (cp.mode == 1)
    {
        for (float v : {cp.invLandGrid, cp.xf, cp.zf, cp.y00, cp.y10, cp.d1000, cp.d0100, cp.d1011, cp.d0111, cp.bias})
        {
            anomaly = anomaly || !GpuInstanceFinite(v);
        }
        anomaly = anomaly || std::fabs(cp.y00) > 5000.0f || std::fabs(cp.y10) > 5000.0f;
    }

    if (!anomaly && worldRadius < 50.0f)
    {
        return; // ordinary small instance -- not worth a line
    }
    LOG_INFO(Graphics,
             "{} GpuInstance {} name={} model={} pos=({},{},{}) radius={} scale={} det={} axes=({},{},{}) "
             "conformMode={} plane=(y00={} y10={} d1000={} d0100={} bias={})",
             anomaly ? "ANOMALY" : "large", where, name, model, pos.X(), pos.Y(), pos.Z(), worldRadius, scale, det, la,
             lu, ld, cp.mode, cp.y00, cp.y10, cp.d1000, cp.d0100, cp.bias);
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
uint64_t EngineWgpu::ResolveMaterialNormalMap(const ShapeSection& section, bool* rgPackedOut,
                                            bool* nativeCavityOut)
{
    if (nativeCavityOut != nullptr)
        *nativeCavityOut = false;
    if (rgPackedOut != nullptr)
        *rgPackedOut = false;
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
    // RFG-047: asked AFTER the upload, because the answer depends on the form the texture
    // was loaded in and only a resolved source can say. A handle of 0 leaves it false --
    // the section is on the white fallback and has no normal to decode either way.
    if (rgPackedOut != nullptr && handle != 0 && wgpuTexture != nullptr)
        *rgPackedOut = wgpuTexture->NormalIsRgPacked();
    if (nativeCavityOut != nullptr && handle != 0 && wgpuTexture != nullptr)
        *nativeCavityOut = wgpuTexture->NormalHasNativeCavity();
    return report(handle, handle ? "" : "upload failed", loadName);
}

// Enfusion `NormalPower`: the material's authored normal-map intensity. Same cache
// ordering as the sibling resolvers above. The shader scales the decoded tangent
// XY by this and renormalises, so the 1.0 default (every legacy material, every
// .emat that names none) renders bit-identically with or without the support.
float EngineWgpu::ResolveMaterialNormalPower(const ShapeSection& section)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_MATERIAL_NORMAL_POWER");
        return !v || v[0] != '0';
    }();
    if (!enabled)
        return 1.0f;
    if (!section.surfMat)
        return 1.0f;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return 1.0f;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return 1.0f; // ResolveMaterialBaseColour fills the cache first
    return cached->second.normalPower;
}

// Crown self-occlusion volume, packed for the section record. Zero lanes when
// the material names no volume, so every legacy section and every silent .emat
// renders exactly as before.
void EngineWgpu::ResolveMaterialCrownAo(const ShapeSection& section, float aoP0[4], float aoP1[4])
{
    aoP0[0] = aoP0[1] = aoP0[2] = aoP0[3] = 0.0f;
    aoP1[0] = aoP1[1] = aoP1[2] = aoP1[3] = 0.0f;
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_FOLIAGE_CROWN_AO");
        return v && v[0] == '1';
    }();
    if (!enabled)
        return;
    if (!section.surfMat)
        return;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return; // ResolveMaterialBaseColour fills the cache first
    const auto& translated = cached->second;
    if (translated.crownAoIntensity <= 0.0f || translated.crownAoWidth <= 0.0f)
        return;
    aoP0[0] = translated.crownAoCenter[0];
    aoP0[1] = translated.crownAoCenter[1];
    aoP0[2] = translated.crownAoCenter[2];
    aoP0[3] = translated.crownAoIntensity;
    aoP1[0] = translated.crownAoHeight;
    aoP1[1] = translated.crownAoWidth;
}

// The environment (reflection) map a section's material names, or 0 when it names none.
//
// DZ-003. Fourth of the same shape as ResolveMaterialNormalMap above, and the reason it
// exists is DayZ water: a pond or a river authors `Color 0.0392 0.0549 0.0667` -- RGB
// (10,14,17) -- and gets its ENTIRE appearance from reflection (`EnvironmentMap`,
// `ReflectionColor`, `FresnelRange` in the `.emat`). Nothing bound an environment map for
// model surfaces, so those surfaces drew as flat near-black quads: correct data, missing
// term.
//
// The handle itself is not what the shader samples -- the reflection is taken from the
// sky's own equirect env map, which is a live render of THIS sky rather than an authored
// snapshot of some other one. What this resolves is the SIGNAL: a material that names an
// environment map is declaring "this surface is reflective", and that is what rides to
// the GPU (one bit in WgrModelSection::flags / WgrDraw3D::flags). Returning the handle
// rather than a bool keeps the shape identical to its three siblings and leaves the
// authored texture available the day a per-material env map is worth binding.
uint64_t EngineWgpu::ResolveMaterialEnvironmentMap(const ShapeSection& section)
{
    if (!section.surfMat)
        return 0;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return 0;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return 0; // ResolveMaterialBaseColour fills the cache first

    const auto report = [&](uint64_t handle, const char* why, const std::string& detail)
    {
        static std::unordered_set<std::string> logged;
        if (logged.insert(materialPath).second)
            LOG_INFO(Graphics, "Wgpu material: environment map {} for {} family={} ({}{}{})", handle ? "bound" : "NONE",
                     materialPath, cached->second.shaderFamily, why, detail.empty() ? "" : " ", detail);
        return handle;
    };

    const auto* env = cached->second.Find(Asset::Material::MaterialSlot::Environment);
    if (!env)
        return 0; // Silent: the overwhelming majority of materials name none.
    std::string loadName = env->texture.path.canonical();
    if (env->texture.isProcedural)
    {
        if (!render::procedural::Parse(env->texture.raw.c_str()).generated)
            return report(0, "procedural lookup", env->texture.raw);
        loadName = env->texture.raw;
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

// True when a section's surface should carry a Fresnel-weighted environment reflection.
//
// DZ-003. The trigger is the material's shader FAMILY being CalmWater, and ONLY that.
//
// Why the family: a DayZ water rvmat is a 79-byte stub naming that family and nothing
// else, and it is a family the engine is expected to implement -- not a material that
// failed to describe itself. Chernarus's `dz\water\` addon ships 47 rvmats and ZERO
// `.emat` files, yet its `ponds/data/water_lake.rvmat` is byte-for-byte the same stub as
// Enoch's pond. Chernarus has ~4,044 water placements (670 ponds + 3,374 streams/beds)
// against Enoch's ~1,238, so keying on the `.emat` -- or on the Environment slot it
// fills -- would light up the smaller map and leave the larger one a flat dark hole.
//
// Why NOT "the material names an EnvironmentMap", which was the first thing tried:
// MEASURED on one Enoch view, 157 perfectly ordinary `Super` materials -- boulders,
// tree stumps, anthills -- name `dz\data\data\env_land_co.paa` in their Environment slot.
// In Real Virtuality that stage is a faint, specular-masked tint; treating it as "this
// surface is a mirror" and mixing the sky in by Schlick turned every rock in the frame
// bright and hard-edged (grazing angles drive Schlick to 1). The slot says "has an
// environment stage", not "is reflective", and one capture was enough to show the
// difference. A material that wants this treatment can still get it by naming the family.
//
// What a surface does NOT yet get from a present `.emat`: its `ReflectionColor` and
// `FresnelRange` are parsed and sit in the EmatMaterial, but carrying them to the GPU
// needs lanes this ABI does not have free, and Chernarus has no values to carry anyway.
// Both maps therefore use the same built-in water constant in the shader (ENV_F0 = 0.02,
// the air/water normal-incidence reflectance). A stated approximation, not a measurement
// of DayZ's own numbers.
bool EngineWgpu::MaterialIsReflective(const ShapeSection& section)
{
    // A/B switch. The reflective bit is baked into the section record at model
    // registration, so this is read once and cannot be flipped on a loaded world -- which
    // is exactly what a before/after capture wants: two runs of the SAME binary, same
    // camera, one term different. WGR_ENV_REFLECTION=0 restores the pre-DZ-003 look.
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_ENV_REFLECTION");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    if (!enabled)
        return false;
    if (!section.surfMat)
        return false;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return false;
    // PERF-016: DrawSectionTL reaches this twice per section, and the map find below hashes
    // the full path string each time. The answer is recorded with the material's other
    // per-path classifications at binding resolution; replay it when it is there. The
    // fallback below is what fills it (bindResolved is set only after the binding block has
    // run, which is also when the map entry this reads is guaranteed to exist).
    if (SectionBindCacheEnabled())
    {
        if (const MaterialClass* bindSlot = MaterialClassSlot(section.surfMat); bindSlot && bindSlot->bindResolved)
        {
            if (!bindSlot->reflective)
                return false;
        }
        else
        {
            const auto uncached = _materialBindings.find(materialPath);
            if (uncached == _materialBindings.end() || uncached->second.shaderFamily != "CalmWater")
                return false;
        }
    }
    else
    {
        const auto cached = _materialBindings.find(materialPath);
        if (cached == _materialBindings.end())
            return false; // ResolveMaterialBaseColour fills the cache first

        // The `.emat` substitution deliberately keeps the rvmat's own family string rather
        // than inventing one (EmatMaterialAdapter::TranslateEmat), so this test reads the
        // same on a water_bliss surface and on a bare water one.
        if (cached->second.shaderFamily != "CalmWater")
            return false;
    }
    static std::unordered_set<std::string> logged;
    if (logged.insert(materialPath).second)
    {
        // Whether this water surface brought its own authored environment map is real
        // information -- Enoch's `water_bliss` ponds do (via the `.emat`), Chernarus's
        // bare `water` ponds do not -- and it is what a future per-material env map would
        // bind. It does not change the decision above; both reflect the live sky.
        const uint64_t authored = ResolveMaterialEnvironmentMap(section);
        LOG_INFO(Graphics, "Wgpu material: reflective (family=CalmWater, authored env map {}) for {}",
                 authored ? "present" : "absent", materialPath);
    }
    return true;
}

// RFG-072: the census the owner asked for, with its denominator. Over DISTINCT `MatPBRMulti`
// materials (counted once each, on the first section that resolves them): how many reach
// the four-layer blend shader and how many collapse to a single image, and why. A section
// whose material names no mask has nothing to blend; one whose mask failed to load is a
// texture-format problem; one whose shape carries no second UV set blends on uv0 and
// tiles the mask across the wall. Printed from RegisterGpuModel, which knows the shape.
struct MultiLayerCensus
{
    size_t multi = 0;            //!< distinct MatPBRMulti materials seen
    size_t composed = 0;         //!< reached the blend (mask bound)
    size_t composedTinted = 0;   //!< of those, wearing per-layer Color_N
    size_t composedUv1 = 0;      //!< of those, mask on the second UV set (per shape+material)
    size_t composedUv0NoSet = 0; //!< mask wants uv1 but the shape has no second set -> uv0
    size_t composedUv0 = 0;      //!< material itself puts the mask on uv0
    size_t singleNoMask = 0;     //!< fell back: names no MaskMap
    size_t singleMaskFailed = 0; //!< fell back: MaskMap named but did not upload
};
static MultiLayerCensus GMultiLayerCensus;

// MAT-048 A/B lever. WGR_MULTI_LAYER_NORMALS=0 binds no layer normal maps at all, which
// restores the pre-MAT-048 behaviour exactly: layer 0's normal shades the whole surface no
// matter which layer the mask selects. Read once and announced through the GAME LOG (the
// launcher discards stderr, so a stderr note would prove nothing about the running binary).
static bool MultiLayerNormalsEnabled()
{
    static const bool enabled = []
    {
        bool on = true;
        if (const char* v = std::getenv("WGR_MULTI_LAYER_NORMALS"))
            on = !(v[0] == '0' && v[1] == '\0');
        LOG_INFO(Graphics, "Wgpu material: Multi layer normal maps {} (WGR_MULTI_LAYER_NORMALS={})",
                 on ? "ENABLED" : "DISABLED (layer 0 normal everywhere)", on ? "1" : "0");
        return on;
    }();
    return enabled;
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
    // RFG-072: and identity colour, so a material with no `Color_N` multiplies by one.
    for (auto& colour : material.layer_colour)
        colour[0] = colour[1] = colour[2] = colour[3] = 1.0f;
    // Same as the normal map above: composeMultiLayers is a presentation switch honoured
    // per fragment by the retained shader, not baked into the record here.
    // RFG-074: every early exit here says so, once per material. They were silent, and
    // a silent exit is how a MatPBRMulti church wall drew for an afternoon as one tinted
    // brick tile while the layer census reported itself healthy -- it only ever counted
    // the sections that got PAST these lines.
    static std::unordered_set<std::string> saidSkip;
    const auto skip = [&](const char* why, const char* what)
    {
        if (saidSkip.insert(std::string(why) + "|" + what).second)
            LOG_INFO(Graphics, "Wgpu material: layers SKIPPED ({}) for {}", why, what);
    };
    if (!section.surfMat)
    {
        auto* face = static_cast<TextureWgpu*>(section.properties.GetTexture());
        skip("section has no surfMat", face && face->GetName().Data() ? face->GetName().Data() : "(no face texture)");
        return false;
    }
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
    {
        skip("surfMat has no name", "(unnamed)");
        return false;
    }
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
    {
        skip("no base-colour binding yet", materialPath);
        return false; // ResolveMaterialBaseColour fills the cache first
    }

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
    // RFG-072 census, over DISTINCT materials of the native family: how many reach the
    // blend and how many fall back to one image, and why. The flag site in
    // RegisterGpuModel adds the UV-set half and prints the line.
    const bool nativeMulti = cached->second.shaderFamily == "MatPBRMulti";
    static std::unordered_set<std::string> censused;
    const bool firstSeen = nativeMulti && censused.insert(materialPath).second;
    if (firstSeen)
        ++GMultiLayerCensus.multi;
    const uint64_t mask = bind(Slot::Mask);
    if (!mask)
    {
        // RFG-074: the fourth silent exit, and the one the church wall took. Reported
        // for EVERY family, because the census above only counts "MatPBRMulti" and a
        // material whose family string came out as anything else vanished here unseen.
        static std::unordered_set<std::string> saidNoMask;
        if (saidNoMask.insert(materialPath).second)
            LOG_INFO(Graphics, "Wgpu material: layers SKIPPED (mask handle 0; slot {}) for {} family='{}'",
                     cached->second.Find(Slot::Mask) == nullptr ? "absent" : "present, upload failed", materialPath,
                     cached->second.shaderFamily);
        if (firstSeen)
        {
            if (cached->second.Find(Slot::Mask) == nullptr)
                ++GMultiLayerCensus.singleNoMask;
            else
                ++GMultiLayerCensus.singleMaskFailed;
        }
        return false; // no mask, no blend to drive -- layer 0 stands alone
    }
    if (firstSeen)
        ++GMultiLayerCensus.composed;

    material.mask_texture_id = mask;
    layerUv(Slot::BaseColour, 0);
    const Slot colours[3] = {Slot::LayerColour1, Slot::LayerColour2, Slot::LayerColour3};
    // MAT-048: each further layer's OWN normal map, paired with its colour. kMulti maps
    // Stage11 to NormalMap (layer 0) and Stage12/13/14 to these -- measured on
    // lighthouse_multi_1.rvmat, whose Stage11-14 are ind_brick_nohq / plaster01_4x4_nohq /
    // concrete_floor_nohq / a procedural color(0.5,0.5,0.5,1,DT), pairing one-for-one with
    // Stage0-3's colours. Binding only the colours blended the albedo across four layers
    // while every fragment kept layer 0's bumps, so a plastered wall read as brick relief.
    const Slot normals[3] = {Slot::LayerNormal1, Slot::LayerNormal2, Slot::LayerNormal3};
    for (size_t i = 0; i < 3; ++i)
    {
        material.layer_texture_id[i] = bind(colours[i]);
        // A layer's normal MUST be sampled at its colour's transform, not the section's raw
        // uv: brick tiles ~4x denser than the plaster beside it, and a normal sampled at the
        // wrong rate is worse than none. bind() returns 0 for a procedural stage
        // (color(...,DT)), which the shader reads as "this layer keeps layer 0's normal".
        material.layer_normal_texture_id[i] = MultiLayerNormalsEnabled() ? bind(normals[i]) : 0;
        layerUv(colours[i], i + 1);
    }
    // RFG-072: each layer's `Color_N` as a linear multiplier, recorded by the adapter only
    // for a material the native mount served (the converting exporter bakes the colour
    // into its `.paa`). Under the Materials-tab switch; off leaves the identity above.
    if (Poseidon::Enfusion::MultiLayersEnabled())
    {
        const Slot tinted[4] = {Slot::BaseColour, Slot::LayerColour1, Slot::LayerColour2, Slot::LayerColour3};
        for (size_t i = 0; i < 4; ++i)
        {
            const auto* entry = cached->second.Find(tinted[i]);
            if (!entry || !entry->tintPresent)
                continue;
            for (int c = 0; c < 4; ++c)
                material.layer_colour[i][c] = entry->tint[c];
            if (firstSeen && i == 0)
                ++GMultiLayerCensus.composedTinted;
        }
    }

    static std::unordered_set<std::string> logged;
    if (logged.insert(materialPath).second)
    {
        const auto* maskEntry = cached->second.Find(Slot::Mask);
        LOG_INFO(Graphics,
                 "Wgpu material: composed {} layers ({} layer normals) for {} family={} (uv scales {:.1f} {:.1f} "
                 "{:.1f} {:.1f}; mask uvSource={}; layer colours {:.2f},{:.2f},{:.2f} / {:.2f},{:.2f},{:.2f} / "
                 "{:.2f},{:.2f},{:.2f} / {:.2f},{:.2f},{:.2f})",
                 1 + (material.layer_texture_id[0] != 0) + (material.layer_texture_id[1] != 0) +
                     (material.layer_texture_id[2] != 0),
                 (material.layer_normal_texture_id[0] != 0) + (material.layer_normal_texture_id[1] != 0) +
                     (material.layer_normal_texture_id[2] != 0),
                 materialPath, cached->second.shaderFamily, material.layer_uv[0][0], material.layer_uv[1][0],
                 material.layer_uv[2][0], material.layer_uv[3][0],
                 maskEntry && !maskEntry->uvSource.empty() ? maskEntry->uvSource.c_str() : "tex",
                 material.layer_colour[0][0], material.layer_colour[0][1], material.layer_colour[0][2],
                 material.layer_colour[1][0], material.layer_colour[1][1], material.layer_colour[1][2],
                 material.layer_colour[2][0], material.layer_colour[2][1], material.layer_colour[2][2],
                 material.layer_colour[3][0], material.layer_colour[3][1], material.layer_colour[3][2]);
    }
    return true;
}

// Does the material's Multi mask stage name uvSource "tex1"? Measured on
// ca\structures_e\wall\wall_l\data\walls_l3.rvmat: TexGen0-3 (the four layers) are "tex",
// TexGen4 (mask, macro, ambient shadow) is "tex1". Sampling the mask on the tiling uv painted
// its plaster/dirt/metal regions as rectangles across the wall -- the owner's "gray stuff on
// the wall texture" (Takistan 5418.46 6244.20 144.15 az 245 el -1), which is the same
// mechanism on every Multi material in Arma 2/3 and DayZ.
bool EngineWgpu::MaterialMaskUsesUv1(const ShapeSection& section)
{
    // A/B, because "the mask is in the wrong frame" and "the second UV set is not what
    // the mask wants" produce the same streaked surface and only one of them is fixed by
    // exporting the channel. WGR_MATERIAL_MASK_UV1=0 pins every mask to uv0, =1 pins it
    // to uv1; unset keeps the material's own answer.
    static const int forced = []
    {
        const char* v = std::getenv("WGR_MATERIAL_MASK_UV1");
        return v ? (std::strcmp(v, "0") == 0 ? 0 : 1) : -1;
    }();
    if (forced >= 0)
        return forced == 1;
    if (!section.surfMat)
        return false;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return false;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return false;
    const auto* mask = cached->second.Find(Asset::Material::MaterialSlot::Mask);
    if (!mask)
        return false;
    const std::string& src = mask->uvSource;
    return src.size() == 4 && (src[0] == 't' || src[0] == 'T') && (src[1] == 'e' || src[1] == 'E') &&
           (src[2] == 'x' || src[2] == 'X') && src[3] == '1';
}

// Does the material's global normal map want the second UV set
// (`UVSrcGlobNormal "UV set 2"`)? Same shape as MaterialMaskUsesUv1 above,
// reading the translated answer rather than a slot.
bool EngineWgpu::MaterialNormalUsesUv1(const ShapeSection& section)
{
    if (!section.surfMat)
        return false;
    const char* materialPath = section.surfMat->GetName().Data();
    if (!materialPath || !*materialPath)
        return false;
    const auto cached = _materialBindings.find(materialPath);
    if (cached == _materialBindings.end())
        return false;
    return cached->second.globalNormalUv1;
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
uint64_t EngineWgpu::ResolveMaterialBaseColour(const ShapeSection& section, bool* fromConstant)
{
    if (fromConstant)
        *fromConstant = false;
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
            cached =
                _materialBindings.emplace(materialPath, Asset::Material::LoadTranslatedMaterial(materialPath)).first;
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

    const char* baseFrom = "Stage0";
    const auto* base = EffectiveBaseColourStage(cached->second, baseFrom);

    if (base && std::strcmp(baseFrom, "Stage0") != 0)
    {
        static std::unordered_set<std::string> loggedLayerBase;
        if (loggedLayerBase.insert(materialPath).second)
            LOG_INFO(Graphics, "Wgpu material: {} has no Stage0; using {} as the base layer (family={})", materialPath,
                     baseFrom, cached->second.shaderFamily);
    }
    if (!base)
        return reject("no BaseColour slot and no layer colour", "");

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
        if (fromConstant)
            *fromConstant = true;
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

// MAT-050 -- an unresolved albedo must not be the brightest thing in the frame.
//
// WHAT WAS MEASURED, and why this is not a cosmetic preference. Parsing the ODOL 73 section
// tables of the 285 models a Stratis capture actually loads gives 2,595 graphical sections:
//
//    face texture only          2,060 sections     682,477 faces
//    RVMAT Stage0 only            430 sections     600,873 faces   <-- no face texture at all
//    both                          90 sections      38,127 faces
//    NEITHER                       15 sections          52 faces
//
// So 45.6% of the world's faces have NO legacy face texture and depend entirely on the
// material resolver below. When that resolver returns 0 the section keeps texture_id 0,
// SharedTextures::texture_slot maps 0 to bindless slot 0, and slot 0 is the 1x1 opaque WHITE
// texel -- albedo 1.0. In a daylight frame that is brighter than anything authored (of 4,273
// Arma 3 textures only 35 average >= 250, every one of them UI art or the sun sprite), so the
// failure does not present as "untextured"; it presents as a light source. Two owner reports,
// one cause: a 2 px x 18 px mast at exactly (255,255,255) blooming into a disc, and building
// far-LODs -- which are disproportionately the Stage0-only ones -- "looking wrong".
//
// WHICH GENERATION THIS IS ABOUT, because the same symptom has two causes and merging them
// would hide one of them. Everything measured above is ARMA 3 (Stratis): the material EXISTS,
// LoadTranslatedMaterial reads it, and it still yields no bindable Stage0 -- a resolution
// failure inside a material the engine understands. The REFORGER "white sticks" report is a
// different statement of the same pixel: 100% of Everon's materials are `.emat`, none of them
// opens (the section keeps the raw Enfusion path while the textures were rewritten to the
// deployed root), and `TranslateEmat` reads DayZ's `AlbedoMap`/`NormalMap` keys while only
// 2.2% of 10,312 real Reforger `.emat` files name any key it reads -- the real ones are
// `BCRMap`, `NMOMap`, `MaskMap`. On Everon white is therefore the EXPECTED output of a
// material the engine cannot read at all, and grey here would only repaint that; the fix for
// Reforger is in the .emat path (another session owns those files), not in this function.
// This code changes what an unresolved section LOOKS like on both, and says so in the log on
// both; it fixes the CAUSE on neither -- it is a floor under the symptom plus the diagnostic
// that was missing.
//
// The correction is applied HERE and not to slot 0 itself. Slot 0 must stay white: it is the
// multiply identity for every draw that legitimately carries no texture and puts its colour in
// the material (2D fills, DrawLine, procedural constants), and repainting it would move pixels
// that are correct today. This knows something slot 0 cannot -- that the draw is a 3D object
// section whose albedo was supposed to resolve and did not.
//
// 128,128,128 is not a taste call either: the Arma 3 `*_mlod_co` placeholders, the closest
// thing the corpus has to "a surface with no detail", average 100-150. A neutral mid-grey is
// what an untextured object is SUPPOSED to look like, and it sits ~4x below white in linear
// light -- under the bloom knee, so the orb cannot form even where the section stays wrong.
//
// SCOPE, and it is what makes the knob safe to turn on at all.
//
// White is not always wrong. A section that carries NO authored material is the original 2001
// case: it has no texture BY DESIGN and its colour comes from the folded TLMaterial, where the
// white texel is the multiply identity. Repainting those grey would halve every untextured
// surface in Cold War Assault -- the game this engine is actually for -- to fix an Arma 3 bug.
//
// A section that NAMES AN RVMAT is a different statement: the material was supposed to supply
// an albedo and did not, so 1.0 is not an identity, it is a failure wearing the brightest value
// the format allows. Only those are in scope when the knob is on. Every case in the measured
// Stratis corpus is of that kind.
//
// AND THE DEFAULT IS STILL OFF, deliberately, even with that scope. The two populations are
// separated by a heuristic ("names an RVMAT"), not by a proof, and the correction is a pixel
// change to a shipping game whose content this session cannot measure -- CWA's own PBOs carry
// no RVMATs, but no run has yet demonstrated that no CWA-era section reaching this engine names
// one. A wrong grey on a legacy surface is silent, permanent and indistinguishable from art;
// a wrong white is loud and already understood. So the WHITE ships and the fix is opt-in until
// a capture pair says otherwise.
//
// What ships unconditionally is the DIAGNOSTIC below it: the per-draw UNTEXTURED warning that
// never existed on this path, and the counters. Those change no pixel.
//
// WGR_UNTEXTURED_ALBEDO:
//   unset / "0" / "off" / "white"   leave texture_id 0 -- the exact pre-MAT-050 white (DEFAULT)
//   "1" / "on"                      128,128,128 on RVMAT-bearing sections only (the fix)
//   "all"                           the same, and ALSO sections with no authored material
//                                   (legacy CWA content included -- this is the one that can
//                                   move shipped pixels, so it is never implied)
//   "R,G,B[,A]"                     any 0-255 tuple, RVMAT scope; append ",all" for the wider
//                                   one. "255,0,255" makes every unresolved OBJECT section
//                                   magenta and, unlike WGR_TEXTURE_FALLBACK_DEBUG, covers
//                                   ONLY those -- so a magenta pixel means "this section's
//                                   albedo did not resolve", with none of the ambiguity of the
//                                   2D fills and constant-colour draws that also share slot 0.
namespace
{
struct UntexturedAlbedoConfig
{
    bool enabled = false;             // DEFAULT: today's behaviour, no pixel moves
    bool includeMaterialless = false; // "all": also correct sections with no RVMAT
    uint8_t rgba[4] = {128, 128, 128, 255};
    std::string raw = "<unset>"; // echoed in the announcement, so the log says what was asked
};

const UntexturedAlbedoConfig& UntexturedAlbedo()
{
    static const UntexturedAlbedoConfig cfg = []
    {
        UntexturedAlbedoConfig out;
        const char* v = std::getenv("WGR_UNTEXTURED_ALBEDO");
        if (!v || !*v)
            return out;
        out.raw = v;
        // Lowercased once and compared by value. This translation unit deliberately avoids
        // _stricmp/strcasecmp -- it builds for Linux too and the two platforms spell that
        // function differently (see the same note in FarTierSettings).
        std::string lowered(v);
        for (char& c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowered == "0" || lowered == "off" || lowered == "white" || lowered == "none")
            return out; // an explicit restatement of the default
        out.enabled = true;
        int channel[4] = {out.rgba[0], out.rgba[1], out.rgba[2], 255};
        int parsed = 0;
        for (std::size_t at = 0; at < lowered.size();)
        {
            const std::size_t comma = lowered.find(',', at);
            std::string token = lowered.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            at = (comma == std::string::npos) ? lowered.size() : comma + 1;
            const std::size_t first = token.find_first_not_of(" \t");
            const std::size_t last = token.find_last_not_of(" \t");
            token = (first == std::string::npos) ? std::string() : token.substr(first, last - first + 1);
            if (token == "all")
            {
                // Scanned as a TOKEN, not a substring: a substring search would have made
                // any future value containing those three letters silently widen the scope.
                out.includeMaterialless = true;
                continue;
            }
            if (parsed >= 4 || token.empty() || token.find_first_not_of("0123456789") != std::string::npos)
                continue; // "1", "on", anything non-numeric: a switch, not a colour
            channel[parsed++] = int(std::clamp<long>(std::strtol(token.c_str(), nullptr, 10), 0, 255));
        }
        // A LONE number is a switch, not a colour: reading "1" as red would be a trap.
        if (parsed >= 3)
            for (int i = 0; i < 4; i++)
                out.rgba[i] = static_cast<uint8_t>(channel[i]);
        return out;
    }();
    return cfg;
}
} // namespace

uint64_t EngineWgpu::UntexturedAlbedoHandle(bool sectionHasMaterial)
{
    const UntexturedAlbedoConfig& cfg = UntexturedAlbedo();
    // ANNOUNCE ON THE FIRST UNTEXTURED SECTION, on or off. This function is only reached when
    // a section genuinely resolved no albedo, so the line's PRESENCE is the proof that the
    // condition occurred and its text is the proof of what was done about it. Without it a
    // capture that looks unchanged has two readings -- "nothing was untextured" and "the knob
    // was never set" -- and that ambiguity is exactly what made the WGR_TEXTURE_FALLBACK_DEBUG
    // magenta A/B unreadable (zero magenta pixels, no way to tell which). Same reasoning, same
    // remedy, as announce_fallback_texel in rust/src/textures.rs.
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        LOG_INFO(Graphics,
                 "Wgpu material: at least one section resolved NO albedo this run; untextured-albedo "
                 "correction is {} (WGR_UNTEXTURED_ALBEDO={}) colour=rgba({},{},{},{}) scope={}",
                 cfg.enabled ? "ON" : "OFF -- those sections keep the 1.0 white at bindless slot 0",
                 cfg.raw, int(cfg.rgba[0]), int(cfg.rgba[1]), int(cfg.rgba[2]), int(cfg.rgba[3]),
                 cfg.includeMaterialless ? "every untextured section (\"all\")"
                                         : "sections that name an RVMAT");
    }
    if (!cfg.enabled || !_wbank)
        return 0;
    // THE SCOPE GUARD. A section with no authored material is the legacy multiply-identity
    // case and is left alone unless "all" was asked for; see the block comment above.
    if (!sectionHasMaterial && !cfg.includeMaterialless)
        return 0;
    if (_untexturedAlbedoHandle)
        return _untexturedAlbedoHandle;
    // CreateDynamic uploads immediately and never goes through the file source, so this
    // cannot fail the way a missing .paa can, and it is exempt from the texture LRU
    // (TrimResidency skips IsDynamicTexture).
    _untexturedAlbedo = _wbank->CreateDynamic(1, 1, cfg.rgba, sizeof(cfg.rgba), false);
    if (auto* texture = static_cast<TextureWgpu*>(_untexturedAlbedo.GetRef()))
        _untexturedAlbedoHandle = texture->GpuHandle();
    LOG_INFO(Graphics,
             "Wgpu material: untextured-section albedo bound as rgba({},{},{},{}) handle={} "
             "(unset WGR_UNTEXTURED_ALBEDO to restore the 1.0 white fallback)",
             int(cfg.rgba[0]), int(cfg.rgba[1]), int(cfg.rgba[2]), int(cfg.rgba[3]), _untexturedAlbedoHandle);
    return _untexturedAlbedoHandle;
}

namespace
{
// WGR_GPU_MODEL_PARK: how many released-but-alive shapes keep their GPU registration (meshes,
// baked materials, model id) instead of being torn down on the last instance's removal.
// Default 256, matching the streamer's evict cache (WGR_OBJECT_STREAM_EVICT_CACHE), which is
// what keeps the SHAPE alive: a shape the cache drops dies, and its parked entry dies with it
// (OnShapeDestroyed), so the park can never outgrow what the cache holds. 0 = old behaviour
// (destroy on last release; every re-admission rebuilds). The measured motivation: on the
// 30 m/s Everon traverse the evict cache scored hits=274 (RND-040), and each of those hits
// still paid a full RegisterGpuModel -- BuildVertices, wgr_mesh_create, section
// classification -- because ReleaseGpuModel had erased the registration the cache's LODShape
// was keeping warm.
bool GpuModelParkRefillEnabled()
{
    static const bool on = []
    {
        const char* value = std::getenv("WGR_GPU_MODEL_PARK_REFILL");
        return value && std::strcmp(value, "1") == 0;
    }();
    return on;
}

size_t GpuModelParkLimit()
{
    static const size_t n = []
    {
        const char* v = std::getenv("WGR_GPU_MODEL_PARK");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 256;
        return static_cast<size_t>(std::clamp<long>(parsed, 0, 100000));
    }();
    return n;
}

// Timing helper for the admission split. Zero cost unless a scope is open.
struct AdmitTimer
{
    double* bucket;
    std::chrono::steady_clock::time_point start;
    explicit AdmitTimer(double* target)
        : bucket(render::GObjectAdmitProfile.active ? target : nullptr),
          start(bucket ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{})
    {
    }
    ~AdmitTimer()
    {
        if (bucket)
            *bucket += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
};
} // namespace

// Section-material coverage over everything ever baked onto the retained path, so "are the
// buildings textured?" is answerable as a count with a denominator instead of by eye.
//
// These are NOT redundant with MAT-050's `_untexturedRetainedSections`, which counts the same
// numerator: what is missing there is a DENOMINATOR and a triangle-weighted view. 21 untextured
// sections out of 30 is a broken world; 21 out of 30,000 is noise, and the per-model UNTEXTURED
// warning cannot tell you which because it is deduplicated by (model, rvmat) -- deliberately, so
// the log stays readable, which is exactly what makes it useless as a count.
//
// Reported next to the object counts each period.
static uint64_t g_bakedSections = 0;
static uint64_t g_bakedSectionsUntextured = 0;
static uint64_t g_bakedSectionTris = 0;
static uint64_t g_bakedSectionTrisUntextured = 0;

const char* EngineWgpu::AdmitRejectName(AdmitReject r)
{
    switch (r)
    {
        case AdmitReject::Admitted: return "admitted";
        case AdmitReject::NotStatic: return "not-static";
        case AdmitReject::Destroyed: return "destroyed";
        case AdmitReject::AlreadyIn: return "already-retained";
        case AdmitReject::NoShape: return "no-shape";
        case AdmitReject::MatDebugRvmat: return "material-debug-rvmat";
        case AdmitReject::LegacyPlants: return "legacy-a1-plants";
        case AdmitReject::WindowFallback: return "window-fallback";
        case AdmitReject::FoliageBail: return "foliage-bail";
        case AdmitReject::StaticEmitter: return "static-emitter";
        case AdmitReject::IndexOverflow: return "index-overflow";
        case AdmitReject::EmptyMesh: return "empty-mesh";
        case AdmitReject::NoOwnedSection: return "no-owned-section";
        default: return "?";
    }
}

// One tally per offer. `shapeLevel` marks the FIRST time a shape reaches this reason, so the
// per-shape column counts assets and the per-object column counts placements -- two numbers that
// differ by three orders of magnitude on a Reforger world and answer different questions ("which
// asset is refused" vs "how much of the map does it cost").
void EngineWgpu::NoteAdmitReject(AdmitReject r, const LODShapeWithShadow* shape, bool shapeLevel)
{
    const size_t i = static_cast<size_t>(r);
    if (i >= static_cast<size_t>(AdmitReject::Count))
        return;
    ++_admitObjects[i];
    if (shapeLevel)
    {
        ++_admitShapes[i];
        if (_admitFirstShape[i].empty() && shape && shape->Name())
            _admitFirstShape[i] = shape->Name();
    }
}

// The census, as one block. Printed on demand (the Interior Sky tab's probe button) rather than
// at a fixed frame: on a streamed world the interesting number is what admission has done by the
// time you are standing somewhere, and a load-time line cannot know that.
void EngineWgpu::LogAdmissionCensus() const
{
    uint32_t offered = 0;
    for (size_t i = 0; i < static_cast<size_t>(AdmitReject::Count); ++i)
        offered += _admitObjects[i];
    LOG_INFO(Graphics, "Retained admission census: {} objects offered, {} admitted ({:.1f}%), {} distinct shapes",
             offered, _admitObjects[0], offered ? 100.0f * float(_admitObjects[0]) / float(offered) : 0.0f,
             _gpuModels.size());
    for (size_t i = 1; i < static_cast<size_t>(AdmitReject::Count); ++i)
    {
        if (_admitObjects[i] == 0 && _admitShapes[i] == 0)
            continue;
        LOG_INFO(Graphics, "  refused {:>20}: {} objects ({:.1f}%), {} shapes; first: {}",
                 AdmitRejectName(static_cast<AdmitReject>(i)), _admitObjects[i],
                 offered ? 100.0f * float(_admitObjects[i]) / float(offered) : 0.0f, _admitShapes[i],
                 _admitFirstShape[i].empty() ? "-" : _admitFirstShape[i].c_str());
    }
    if (offered == 0)
    {
        LOG_WARN(Graphics,
                 "Retained admission census: NOTHING was ever offered. SceneObjectCreated never "
                 "ran with a live renderer -- the world was placed before the GPU-driven path "
                 "existed, and nothing re-offers it. That is an ordering bug, not a filter.");
    }
}

void EngineWgpu::PrepareNativeNormalSources(LODShapeWithShadow* shape)
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_NATIVE_NORMAL_PREFETCH");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!enabled || !_wbank || !shape || !ContainsNoCase(shape->Name(), ".xob")) return;
    // Main-thread source preparation only. No EnsureUploaded/slot allocation:
    // the existing bank owns the source and the normal binding below consumes it.
    const auto began = std::chrono::steady_clock::now();
    std::unordered_set<std::string> seen;
    uint32_t loaded = 0;
    for (int level = 0; level < shape->NLevels(); ++level)
    {
        if (!shape->IsNormalLevel(level)) continue;
        Shape* s = shape->LevelOpaque(level);
        if (!s) continue;
        for (int i = 0; i < s->NSections(); ++i)
        {
            const auto& section = s->GetSection(i);
            if ((section.properties.Special() & (IsHidden | IsHiddenProxy)) || !section.surfMat) continue;
            const char* path = section.surfMat->GetName().Data();
            if (!path || !ContainsNoCase(path, ".emat") || !seen.insert(path).second) continue;
            auto cached = _materialBindings.find(path);
            if (cached == _materialBindings.end())
            {
                try {
                    cached = _materialBindings.emplace(path, Asset::Material::LoadTranslatedMaterial(path)).first;
                } catch (...) {
                    _materialBindings.emplace(path, Asset::Material::TranslatedMaterial{});
                    continue;
                }
            }
            const auto* normal = cached->second.Find(Asset::Material::MaterialSlot::NormalMap);
            if (!normal || normal->texture.isProcedural || normal->texture.path.empty()) continue;
            const auto name = normal->texture.path.canonical();
            if (_wbank->Find(name.c_str()) >= 0) continue;
            _wbank->Load(name.c_str());
            ++loaded;
        }
    }
    if (loaded)
        LOG_INFO(Graphics, "Native normal prefetch: model={} sources={} ms={:.3f}", shape->Name(), loaded,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
}

void EngineWgpu::CaptureWarmTextureReads(const LODShapeWithShadow* shape,
    std::vector<WarmTextureRead>& reads, size_t& workRemaining) const
{
    if (!ObjectStreamPreparer::WarmTextureJobsEnabled() || !Foundation::IsMainThread() ||
        !_gpuDriven || !_renderer || !shape || !workRemaining) return;
    // Diagnostic only for this already opt-in owner path. No clocks, new source
    // capture, extra bank lookup or changes to the existing shared visit budget.
    const size_t initialWork = workRemaining, initialReads = reads.size();
    size_t inspected = 0, ineligible = 0, noCurrentSource = 0, sizeOrKeyRefused = 0, lodVisits = 0;
    auto report = [&](bool recorded) {
        static unsigned rows = 0; // owner-only; saturated to bound log/format work
        if (rows < 64)
        {
            ++rows;
            LOG_INFO(World, "Warm texture capture: path={} inspected={} hotOrIneligible={} noCurrentSource={} sizeOrKeyRefused={} accepted={} lodVisits={} workStart={} workLeft={} workCapped={}",
                recorded ? "recorded" : "sections", inspected, ineligible, noCurrentSource, sizeOrKeyRefused,
                reads.size() - initialReads, lodVisits, initialWork, workRemaining, workRemaining == 0);
        }
        else if (rows == 64)
        {
            ++rows;
            LOG_INFO(World, "Warm texture capture: log truncated after 64 opt-in calls; this prefix is not full capture coverage or a source-freshness proof");
        }
    };
    auto inspect = [&](const TextureWgpu* texture)
    {
        if (!workRemaining || reads.size() >= 64) return;
        --workRemaining; ++inspected;
        if (!texture || !texture->CanWarmPrepare()) { ++ineligible; return; }
        auto source = texture->CurrentArchiveSourceBinding();
        if (!source) { ++noCurrentSource; return; }
        if (source->Request().bytes > 16 * 1024 * 1024) { ++sizeOrKeyRefused; return; }
        const std::string key = render::PreparedTextureStore::Key(texture->SourceName());
        if (key.empty() || key.size() > 8191 ||
            std::any_of(reads.begin(), reads.end(), [&](const auto& prior) { return prior.key == key; })) { ++sizeOrKeyRefused; return; }
        reads.push_back({key, std::move(source)});
    };
    if (const auto found = _gpuModelTextures.find(shape); found != _gpuModelTextures.end())
    {
        // Only already-recorded bindings; includes material stages if they were
        // resolved during an earlier registration. No new resolution here.
        for (const auto* texture : found->second)
        {
            if (!workRemaining || reads.size() >= 64) break;
            inspect(texture);
        }
        report(true);
        return;
    }
    // Recreated registrations: primary textures of existing normal LOD sections.
    // Lazy/unavailable LODs, unresolved material stages and proxies retain fallback.
    for (int level = 0; level < shape->NLevels() && level < 64 && workRemaining && reads.size() < 64; ++level)
    {
        --workRemaining; ++lodVisits;
        if (!shape->IsNormalLevel(level)) continue;
        const Shape* lod = shape->LevelOpaque(level);
        if (!lod) continue;
        for (int section = 0; section < lod->NSections() && workRemaining && reads.size() < 64; ++section)
            inspect(dynamic_cast<const TextureWgpu*>(lod->GetSection(section).properties.GetTexture()));
    }
    report(false);
}

render::RegistrationCost EngineWgpu::QueryRegistrationCost(const LODShapeWithShadow* shape) const
{
    if (!Foundation::IsMainThread()) return render::RegistrationCost::Unknown;
    if (!_gpuDriven || !_renderer) return render::RegistrationCost::Reusable;
    if (!shape) return render::RegistrationCost::Unknown;
    // At most one parent plus 128 direct children, each with <=128 binding checks.
    // No recursion, section/vertex scans, native calls, or mutable residency touches.
    const auto ownRegistration = [&](const LODShapeWithShadow* key)
    {
        const auto model = _gpuModels.find(key);
        if (model == _gpuModels.end()) return render::RegistrationCost::NeedsRegistration;
        const bool parked = _gpuModelParkedIndex.find(key) != _gpuModelParkedIndex.end();
        const auto handles = _gpuModelTextureHandles.find(key);
        const auto textures = _gpuModelTextures.find(key);
        const bool complete = handles != _gpuModelTextureHandles.end() &&
            textures != _gpuModelTextures.end() && handles->second.size() == textures->second.size();
        const size_t count = complete ? handles->second.size() : 0;
        // RegisterGpuModel itself does not emit nested proxies for this shape.
        return render::ClassifyRegistrationCost(true, model->second == WGR_INVALID_MODEL,
            false, parked, complete, count, [&](size_t i)
            {
                const TextureWgpu* texture = textures->second[i];
                if (!texture || !texture->GpuHandle()) return false;
                const auto& captured = handles->second[i];
                return captured.handle == texture->GpuHandle() ||
                    captured.SameSlot(texture->GpuHandle(), texture->SlotLease());
            });
    };
    const auto parentCost = ownRegistration(shape);
    if (parentCost != render::RegistrationCost::Reusable) return parentCost;
    const auto model = _gpuModels.find(shape);
    if (model->second == WGR_INVALID_MODEL) return render::RegistrationCost::Reusable;
    const auto coverage = _gpuModelCoverage.find(shape);
    if (coverage == _gpuModelCoverage.end()) return render::RegistrationCost::Unknown;
    if (coverage->second == GpuDrawCoverage::Full) return render::RegistrationCost::Reusable;
    if (coverage->second != GpuDrawCoverage::Partial) return render::RegistrationCost::Unknown;
    return render::ClassifyDirectProxyRegistrationCost(shape->NLevels(),
        [&](int level) { return shape->IsNormalLevel(level); },
        [&](int level)
        {
            const Shape* lod = shape->LevelOpaque(level); // Raw getter, no lazy load.
            return lod ? lod->NProxies() : 0;
        },
        [&](int level, int index)
        {
            const ProxyObject& proxy = shape->LevelOpaque(level)->Proxy(index);
            const LODShapeWithShadow* child = proxy.obj ? proxy.obj->GetShape() : nullptr;
            // ProxyHidden is a find/count-only owner lookup. Match EmitGpuProxies' skip.
            if (!child || MapThings::ProxyHidden(shape, child)) return render::RegistrationCost::Reusable;
            return ownRegistration(child);
        });
}

uint32_t EngineWgpu::RegisterGpuModel(LODShapeWithShadow* shape)
{
    if (auto found = _gpuModels.find(shape); found != _gpuModels.end())
    {
        // Already registered -- live, or PARKED. A parked entry is valid only while the
        // texture handles baked into its materials are the ones the textures still hold; a
        // TrimResidency eviction and re-upload in between hands out a new slot the baked
        // material never learns (see the residency note above EnsureUploaded), so prove it
        // before reusing. Stale -> destroy and fall through to a fresh registration.
        if (const auto parked = _gpuModelParkedIndex.find(shape); parked != _gpuModelParkedIndex.end())
        {
            bool handlesLive = true;
            const bool refillEnabled = GpuModelParkRefillEnabled();
            const auto refs = refillEnabled ? _gpuModelRefs.find(shape) : _gpuModelRefs.end();
            const render::ParkedRefillContext refillContext{
                refillEnabled, true, refillEnabled && RetainedModelIsLive(shape),
                refs == _gpuModelRefs.end() ? 0u : refs->second};
            uint32_t activationRefills = 0;
            bool refillFailed = false, leaseChanged = false;
            if (const auto handles = _gpuModelTextureHandles.find(shape); handles != _gpuModelTextureHandles.end())
            {
                const auto textures = _gpuModelTextures.find(shape);
                const size_t n = handles->second.size();
                if (textures == _gpuModelTextures.end() || textures->second.size() != n)
                    handlesLive = false;
                for (size_t i = 0; handlesLive && i < n; i++)
                {
                    TextureWgpu* texture = textures->second[i];
                    if (refillEnabled && texture && handles->second[i].lease != 0 &&
                        texture->GpuHandle() != handles->second[i].handle)
                        leaseChanged |= handles->second[i].lease != texture->SlotLease();
                    if (refillEnabled && refillContext.references == 0 &&
                        texture && texture->GpuHandle() == 0)
                    {
                        const render::ParkedImageBinding image{
                            handles->second[i].handle, handles->second[i].lease, texture->GpuHandle(),
                            texture->SlotLease(), true, texture->IsDynamicTexture(), texture->HasReloadableSource()};
                        if (render::CanRefillParkedImageActivation(refillContext, image))
                        {
                            uint64_t uploaded = 0;
                            try { uploaded = texture->EnsureUploaded(); }
                            catch (...) { refillFailed = true; }
                            if (render::RefilledParkedImageMatches(image, uploaded, texture->GpuHandle(),
                                                                  texture->SlotLease()))
                            {
                                handles->second[i].handle = uploaded;
                                ++activationRefills;
                            }
                            else
                            {
                                refillFailed = true;
                                handlesLive = false;
                            }
                        }
                        else if (image.capturedLease != 0 && image.capturedLease != image.currentLease)
                            leaseChanged = true;
                    }
                    if (texture && handles->second[i].SameSlot(texture->GpuHandle(), texture->SlotLease()))
                        handles->second[i].handle = texture->GpuHandle(); // shared refill or detail replacement
                    if (!texture || texture->GpuHandle() != handles->second[i].handle)
                        handlesLive = false;
                }
            }
            else if (refillEnabled)
                handlesLive = false;
            if (refillEnabled)
            {
                _parkRefillActivationImages += activationRefills;
                _parkRefillActivationFailures += refillFailed ? 1 : 0;
                _parkRefillLeaseChangedStale += leaseChanged ? 1 : 0;
                if ((activationRefills || refillFailed || leaseChanged) && std::getenv("WGR_MODEL_RESIDENCY_TRACE"))
                    LOG_INFO(Graphics,
                             "Wgpu parked refill activation: model={} images={} reused={} failed={} leaseChanged={} "
                             "totals deferredChecks={} activationImages={} activationFailures={} leaseChangedStale={}",
                             shape->Name(), activationRefills, handlesLive, refillFailed, leaseChanged,
                             _parkRefillDormantModels, _parkRefillActivationImages,
                             _parkRefillActivationFailures, _parkRefillLeaseChangedStale);
            }
            if (handlesLive)
            {
                UnparkGpuModel(shape);
                if (render::GObjectAdmitProfile.active)
                    ++render::GObjectAdmitProfile.parkHits;
                if (std::getenv("WGR_MODEL_RESIDENCY_TRACE"))
                    LOG_INFO(Graphics, "Wgpu model residency unpark: model={} parked={}", shape->Name(),
                             _gpuModelParked.size());
                return found->second;
            }
            if (render::GObjectAdmitProfile.active)
                ++render::GObjectAdmitProfile.parkStale;
            if (std::getenv("WGR_MODEL_RESIDENCY_TRACE"))
                LOG_INFO(Graphics, "Wgpu model residency unpark STALE: model={} -- textures moved, rebuilding",
                         shape->Name());
            DestroyGpuModel(shape); // erases every map entry; registration proceeds below
        }
        else
        {
            return found->second; // already scanned (WGR_INVALID_MODEL if it was ineligible)
        }
    }

    PrepareNativeNormalSources(shape);
    std::vector<TextureWgpu*> modelTextures;
    std::vector<uint64_t> modelMeshes;
    if (_wbank)
        _wbank->BeginModelTextureCapture(modelTextures);
    struct TextureCaptureScope
    {
        TextureBankWgpu* bank;
        ~TextureCaptureScope()
        {
            if (bank)
                bank->EndModelTextureCapture();
        }
    } textureCaptureScope{_wbank};

    std::vector<WgrModelLod> lods;
    std::vector<WgrModelSection> sections;
    std::vector<WgrModelMaterial> materials;
    bool eligible = true;
    // Which bail turned `eligible` off, remembered so the census can attribute this shape's
    // placements without re-running registration. Set beside every `eligible = false`.
    AdmitReject rejectWhy = AdmitReject::Admitted;
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

    // Legacy A1 vegetation under `ca\plants` is present in the object stream, but several
    // card/branch sections disappear after retained-model ownership (visible around Sara
    // 8249.15, 9021.16). Direct WGPU draws the same models completely. Preserve visual parity
    // for this legacy family while the retained TreeAdv/card import is still incomplete;
    // later-generation vegetation and the rest of the world remain GPU-driven.
    if (LegacyPlantFallbackEnabled() && ContainsNoCase(shape->Name(), "ca\\plants\\"))
    {
        static std::unordered_set<std::string> loggedLegacyPlantFallback;
        if (loggedLegacyPlantFallback.insert(std::string(shape->Name())).second)
            LOG_INFO(Graphics, "GpuModel vegetation fallback: model={} reason=legacy-a1-plants", shape->Name());
        eligible = false;
        rejectWhy = AdmitReject::LegacyPlants;
    }

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
            // A1 window sections are not consistently classified: `hotel_oknoa_ca.paa`, for
            // example, contains the hotel's glass and Venetian blinds but does not enter the
            // Cutout bucket. The retained path currently drops that entire section. A named
            // window texture/material is therefore sufficient to keep the WHOLE model on the
            // proven direct path; partial suppression is spec-based and would otherwise hide
            // the direct window draw behind retained opaque walls.
            if (auto* texture = static_cast<TextureWgpu*>(shSec.properties.GetTexture());
                WindowSectionFallbackEnabled() && texture &&
                (MaterialIsWindow(shSec.surfMat) || ContainsNoCase(texture->Name(), "okn") ||
                 ContainsNoCase(texture->Name(), "window")))
            {
                static std::unordered_set<std::string> loggedWindowFallback;
                if (loggedWindowFallback.insert(std::string(shape->Name() ? shape->Name() : "?")).second)
                {
                    LOG_INFO(Graphics, "GpuModel window fallback: model={} lod={} section={} texture={}",
                             shape->Name(), level, i, texture->Name());
                }
                eligible = false;
                rejectWhy = AdmitReject::WindowFallback;
                break;
            }
            // TreeAdv leaf cards have antialiased alpha, so their texture histogram is Blend
            // even though the authored family uses alpha-tested foliage. The retained pipeline
            // has no transparent foliage variant yet; forcing that data into its opaque/cutout
            // path produced either solid sheets or black holes. Leave the entire model on the
            // established direct path until retained alpha-to-coverage matches it exactly. A
            // whole-model fallback is necessary: CPU partial suppression is spec-based and
            // cannot safely hand off the trunk while keeping these opaque-spec leaf cards.
            //
            // ---------------------------------------------------------------------------
            // THIS BAIL IS THE SINGLE LARGEST CPU COST ON ARMA 3, AND ITS PRECONDITION HAS
            // SINCE BEEN MET.
            //
            // Measured on Stratis: it rejects the WHOLE MODEL for 88,084 of 163,953 resident
            // placements -- every land vegetation model in the world, 53.8% of it. Those
            // objects never register, so they never reach the coverage test at all; they fall
            // to GpuDrawCoverage::None by default and ride the CPU draw path. That is the
            // 11,974 objects/frame in Pass1 costing 37.6 ms, against Everon's 1 object and
            // 0.068 ms. Arithmetic closes to 0.8%: 163,953 - 88,084 = 75,869 predicted
            // registered vs 75,274 measured. The control is the underwater bw_set* corals --
            // same TreeAdv material family, but an OPAQUE texture, so this test does not fire
            // and they do register.
            //
            // Everon is immune only because every Reforger material resolves through the
            // .emat adapter with an empty pixelShaderId, so GpuSectionMaterialUsesLeafCards
            // can never match. It is not that Reforger foliage is handled better.
            //
            // The comment above says to leave it "until retained alpha-to-coverage matches it
            // exactly". That landed: gfx3d/mod.rs drives alpha_to_coverage_enabled on the
            // cutout colour AND prepass pipelines for the per-draw and GPU-driven paths alike,
            // default-on under MSAA (WGR_FOLIAGE_A2C). ClassifyGpuSection already accepts this
            // exact case per section (variant=1, alphaRef=0.5). So the bail is stale.
            //
            // NOW ON BY DEFAULT, on measured frame time AND checked pixels.
            //
            // Stratis, settled, identical pose: registration 75,274 -> 163,373, land:obj
            // 42.702 -> 0.185 ms, and with the LOD governor free to do its job on the
            // now-GPU-bound frame, 17.24 -> 44.69 fps. The captures show MORE vegetation than
            // the baseline, at both an aerial and a ground-level pose, with neither of the
            // artefacts this guard existed for -- no solid sheets, no black holes.
            //
            // WGR_TREEADV_RETAINED=0 restores the bail. Reach for it if foliage renders as
            // sheets or holes on content not tested here.
            //
            // ONE REAL PRECONDITION: alpha-to-coverage needs MSAA. With sample_count == 1
            // gfx3d/mod.rs leaves foliage_a2c off, and lifting the bail there reintroduces
            // exactly the artefact it was added for. MSAA is on by default (the benchmarked
            // runs log msaa=4x), so the default pairing is sound -- but a build or config that
            // disables MSAA should set this to 0 as well. That coupling is not enforced in
            // code because this side cannot see the renderer's sample count without a new ABI
            // call, and inventing one to express a dependency between two defaults is worse
            // than saying so here.
            // REVERTED TO OFF. It shipped on for part of one session and the owner reported
            // three separate visual regressions, all consistent with this one flag:
            //
            //  - buildings stopped casting shadows and interiors were lit through their walls.
            //    EngineWgpu.cpp:1460 gates the cascades on `!_shadowCasters.empty()`, and
            //    SceneShadowPass.cpp skips every GpuDrawCoverage::Full object when filling
            //    that list. Moving foliage to the retained path moves it to Full, which can
            //    empty the list and disable the cascade system wholesale.
            //    THAT GATE IS NOW FIXED (:1514): the cascades also stay on for a non-empty
            //    retained set, so this flag can no longer take the shadows out by itself.
            //    Unbuilt/unverified at the time of writing, and it does NOT clear the other
            //    two regressions below, which are why this flag stays off.
            //  - pine trunks disappeared again -- a fix that already exists, at
            //    EngineWgpu.cpp:2711-2727, and lives on the CPU PER-DRAW path: without its
            //    matching discard "the black transparent texels depth-write in the opaque
            //    fallback and completely hide the pine's trunk behind its cards". The
            //    retained path has no equivalent, so retaining the model loses the fix.
            //  - near trees kept a coarse LOD. RegisterGpuModel skips levels with no
            //    GPU-owned section, so a model whose LOD 0 is all blend leaf-cards has a
            //    COARSER shape level as its finest registered LOD, permanently.
            //
            // The measured prize is real -- land:obj 42.702 -> 0.185 ms and 17.24 -> 44.69 fps
            // on Stratis -- but it is one world's framerate against correct shadows, trunks
            // and near detail on every world. It goes back behind the flag until the retained
            // path can carry the trunk discard, be counted as a shadow caster, and register
            // its finest level.
            //
            // WGR_TREEADV_RETAINED=1 opts back in. Do not make it the default again without
            // checking an interior for shadows and a pine for its trunk.
            //
            // 2026-09-02: the three regressions were addressed (below) and the default was flipped
            // on for one deploy. MEASURED, and put back to OFF the same hour: on Chernarus+ the
            // CPU prize is real (coverage none 4987 -> 150, land:obj 7.65 -> 1.05 ms) but the GPU
            // frame went 30.3 -> 49.4 ms (fps 32 -> 18): the retained path draws the trees at
            // 30.5 M triangles where the per-draw path drew 3.8 M, because the per-draw LOD is
            // governed by frame time (_lodInvWidth feedback) and the GPU-driven LOD is not
            // (cull.wgsl: "fixed, not fed back"). Until the GPU cull honours the same governor,
            // retaining the vegetation trades a CPU millisecond for ten GPU milliseconds.
            // WGR_TREEADV_RETAINED=1 is the lever, and the fixes below stay in so that when it
            // is on, trunks, shadows and near LODs are right. What the flip WOULD fix:
            //  - shadows: the cascade gate was fixed (see :2766) before this;
            //  - trunks: ClassifyGpuSection now applies DrawSectionTL's bark rule (solid, no
            //    alpha test) and its opaque-route rule, so a Blend-histogram bark section under a
            //    TreeAdv material is drawn opaque by the GPU instead of being owned-but-declined;
            //  - near LOD: SectionGpuOwned (ClipShape.hpp) owns IsAlpha-spec leaf cards over
            //    cutout textures, so a LOD whose sections are all such cards registers.
            // Chernarus+ measured 77% of visible objects as CPU-drawn vegetation with the bail on
            // (WLD-026 follow-up); Stratis measured land:obj 42.7 -> 0.185 ms without it.
            // WGR_TREEADV_RETAINED=0 restores the bail, whole-model, exactly as before.
            static const bool treeAdvRetained = []
            {
                const char* v = std::getenv("WGR_TREEADV_RETAINED");
                return v && v[0] != '0';
            }();
            if (auto* texture = static_cast<TextureWgpu*>(shSec.properties.GetTexture());
                texture && texture->GetAlphaClass() == Poseidon::AlphaStats::Blend &&
                GpuSectionMaterialIsCutout(shSec.surfMat, texture))
            {
                // Named once per model, so a run says WHICH models this costs rather than
                // only how many. Mirrors the loggedUntextured dedup pattern below.
                static std::unordered_set<std::string> loggedFoliageBail;
                if (loggedFoliageBail.insert(std::string(shape->Name() ? shape->Name() : "?")).second)
                {
                    LOG_INFO(Graphics, "GpuModel foliage bail: model={} lod={} section={} tex={} retained={}",
                             shape->Name(), level, i, texture->Name(), treeAdvRetained ? 1 : 0);
                }
                if (!treeAdvRetained)
                {
                    eligible = false;
                    rejectWhy = AdmitReject::FoliageBail;
                    break;
                }
            }
            // The retained material buffer currently carries imported lamp emissive values through a
            // separate update path, so its HDR result can bypass the compact-emitter cap applied by
            // DrawSectionTL. These are tiny static helper meshes (not general scenery); retaining
            // the complete model on the direct path keeps the presentation cap authoritative while
            // the paired WGPU point light continues to illuminate the surroundings.
            if (StaticEmitterRadianceTarget(shSec.surfMat) < 1.0f)
            {
                eligible = false;
                rejectWhy = AdmitReject::StaticEmitter;
                break;
            }
            // SectionRetainedRefuses is the same subtraction Shape::Draw's skip applies, and it
            // has to be here too: coverage is decided from this test, and a section the GPU
            // declines on TEXTURE grounds over an opaque SPEC would otherwise leave the object
            // Full -- which suppresses the CPU draw wholesale, so the complement that was
            // supposed to repaint it never runs. That is how A1's hotel lost 10,496 triangles
            // of glass and blinds. See ClipShape.hpp for the full contract.
            if (SectionGpuOwned(shSec))
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
        std::vector<VertexIndex> indices(static_cast<size_t>(ni));
        {
            AdmitTimer buildTimer(&render::GObjectAdmitProfile.meshBuildMs);
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
            render::mesh::BuildIndices(*s, indices.data());
        }
        // §9 Approach A: for forests, flood-fill this LOD into per-tree components and bake each
        // vertex's crown-centre index into its conform word (forests are mode 0/1 conform, which
        // never read the ClipLand per-vertex selector, so the word is free). vs_gpu reads it to get
        // a per-tree radial-normal centre from the crown_centres table instead of inst.center.
        // Captured for the queued create rather than registered here: the table BASE the
        // registration returns is baked into vertex data, so the patch happens at the drain,
        // where the real base exists (REN-THR-008).
        std::vector<WgrVec4> crownCentres;
        std::vector<uint32_t> crownComp;
        if (isForest)
        {
            BuildForestCrownComponents(verts, indices, crownCentres, crownComp);
        }
        // Both halves of this are 32-bit now: VertexIndex (Core/Types.hpp) and the
        // wgr_mesh_create index slice. The pool buffer was always Uint32 and merely
        // widened on upload, so the old uint16_t FFI slice was a ceiling nothing on
        // the GPU side asked for -- it capped a LOD at 65535 vertices and, above
        // that, BuildIndices truncated and the mesh drew triangles between unrelated
        // vertices (the stretched-sheet failure).
        //
        // The guard stays, at the type's real limit, because the failure mode above
        // is silent and a future narrowing must not reintroduce it quietly.
        static_assert(sizeof(VertexIndex) == sizeof(uint32_t),
                      "wgr_mesh_create takes a u32 index slice; keep VertexIndex in step");
        constexpr int64_t kMaxMeshVertices = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
        if (static_cast<int64_t>(nv) > kMaxMeshVertices)
        {
            LOG_ERROR(Graphics,
                      "ANOMALY GpuModel index overflow: name={} lod={} vertices={} (ceiling {}) -- "
                      "indices wrap; refusing the retained path for this shape",
                      shape->Name(), level, nv, kMaxMeshVertices);
            eligible = false;
            rejectWhy = AdmitReject::IndexOverflow;
            break;
        }
        if (std::getenv("WGR_INSTANCE_TRACE"))
        {
            LOG_INFO(Graphics, "GpuModel lod name={} lod={} res={} vertices={} indices={} sections={} radius={}",
                     shape->Name(), level, shape->Resolution(level), nv, ni, s->NSections(), shape->BoundingSphere());
        }
        // The empty-input check is the ONLY failure wgr_mesh_create had (REN-THR-001,
        // pool.rs:468), so with it done here the queued create cannot fail and the
        // producer handle can stand in for the id everywhere downstream.
        if (verts.empty() || ni <= 0)
        {
            eligible = false;
            rejectWhy = AdmitReject::EmptyMesh;
            break;
        }
        uint64_t mesh = 0;
        {
            AdmitTimer createTimer(&render::GObjectAdmitProfile.meshCreateMs);
            _ops.meshCreate++;
            ResourceOp op;
            op.kind = ResourceOp::MeshCreate;
            op.mesh = _nextMeshHandle++;
            op.verts = std::move(verts);
            op.indices = std::move(indices);
            op.crownCentres = std::move(crownCentres);
            op.crownComp = std::move(crownComp);
            mesh = op.mesh;
            if (!isForest && !(levelConform && s->OriginalPosValid())) PrepareSharedMeshLifetime(mesh);
            try { _resourceOps.push_back(std::move(op)); }
            catch (...) { AbandonUnpublishedSharedMeshLifetime(mesh); throw; }
        }
        _gpuMeshes.insert(mesh);
        modelMeshes.push_back(mesh);
        // RFG-088: this level's retained mesh holds exactly the bytes CreateVertexBuffer
        // would build for it, unless it was conformed (BuildOrigVertices) or crown-patched.
        if (!isForest && !(levelConform && s->OriginalPosValid()))
        {
            _retainedMeshOfLevel[s] = mesh;
        }
        if (render::GObjectAdmitProfile.active)
            ++render::GObjectAdmitProfile.meshes;
        AutoArray<render::mesh::MeshSection> secs;
        {
            AdmitTimer buildTimer(&render::GObjectAdmitProfile.meshBuildMs);
            render::mesh::BuildSections(*s, secs);
        }

        const uint32_t base = U32(sections.size());
        uint32_t count = 0;
        // The section loop: classification (which uploads the face texture and classifies its
        // alpha), material resolution (.rvmat/.emat reads on first sight, their maps' uploads),
        // and the flags. Inclusive of the texture buckets TextureWgpu fills; LandSave subtracts.
        AdmitTimer sectionTimer(&render::GObjectAdmitProfile.sectionMs);
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
                // RFG-074: for the traced shape, say which sections the classifier
                // REJECTED and what they carried -- these never reach the layer path.
                {
                    static const std::string traceShape2 = []
                    {
                        const char* v = std::getenv("POSEIDON_TRACE_SHAPE");
                        return std::string(v != nullptr ? v : "");
                    }();
                    if (!traceShape2.empty() && shape != nullptr && shape->Name() != nullptr &&
                        std::string(shape->Name()).find(traceShape2) != std::string::npos)
                    {
                        const ShapeSection& rs = s->GetSection(i);
                        auto* face = static_cast<TextureWgpu*>(rs.properties.GetTexture());
                        LOG_INFO(Graphics, "Wgpu material: TRACE-REJECT {} level {} section {} alpha={} surfMat='{}' face='{}'",
                                 shape->Name(), level, i, face ? static_cast<int>(face->GetAlphaClass()) : -1,
                                 rs.surfMat && rs.surfMat->GetName().Data() ? rs.surfMat->GetName().Data() : "(null)",
                                 face && face->GetName().Data() ? face->GetName().Data() : "(none)");
                    }
                }
                continue;
            }
            // The RVMAT's albedo wins over the section's face texture, which on
            // Arma 3 structures is usually a `*_mlod_co` placeholder. Applied here
            // rather than inside ClassifyGpuSection because that is a free
            // function with no access to the material cache or texture bank.
            // Model-space size metrics for one section, computed only when we are about to
            // report it (walking every section of every model would cost real load time).
            const auto sectionExtent = [&](int secIndex, int& facesOut, float& diagOut, float& maxFaceOut,
                                           int& bigFacesOut, float* loOut, float* hiOut)
            {
                const ShapeSection& sx = s->GetSection(secIndex);
                float lo[3] = {1e30f, 1e30f, 1e30f};
                float hi[3] = {-1e30f, -1e30f, -1e30f};
                int   faces = 0;
                float maxFace = 0.0f;
                int   bigFaces = 0;
                for (Offset f = sx.beg; f < sx.end; s->NextFace(f))
                {
                    const Poly& poly = s->Face(f);
                    if (poly.N() < 3)
                    {
                        continue;
                    }
                    faces++;
                    float flo[3] = {1e30f, 1e30f, 1e30f};
                    float fhi[3] = {-1e30f, -1e30f, -1e30f};
                    for (int v = 0; v < poly.N(); v++)
                    {
                        Vector3Val p = s->Pos(poly.GetVertex(v));
                        for (int a = 0; a < 3; a++)
                        {
                            lo[a] = floatMin(lo[a], p[a]);
                            hi[a] = floatMax(hi[a], p[a]);
                            flo[a] = floatMin(flo[a], p[a]);
                            fhi[a] = floatMax(fhi[a], p[a]);
                        }
                    }
                    const float faceDiag = Vector3(fhi[0] - flo[0], fhi[1] - flo[1], fhi[2] - flo[2]).Size();
                    maxFace = floatMax(maxFace, faceDiag);
                    if (faceDiag >= 3.0f)
                    {
                        bigFaces++;
                    }
                }
                facesOut = faces;
                diagOut = faces > 0 ? Vector3(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]).Size() : 0.0f;
                maxFaceOut = maxFace;
                bigFacesOut = bigFaces;
                for (int a = 0; a < 3; a++)
                {
                    loOut[a] = lo[a];
                    hiOut[a] = hi[a];
                }
            };

            // MAT-045: a procedural CONSTANT colour must not override a real face texture.
            //
            // The rule above it ("the RVMAT's albedo wins over the section's face texture,
            // which on Arma 3 structures is usually a `*_mlod_co` placeholder") is right when
            // the RVMAT names an image. It is exactly backwards when the RVMAT's base stage is
            // a procedural constant: there the CONSTANT is the authoring placeholder and the
            // face texture is the real baked albedo.
            //
            // Measured on `house_l_8_ep1` LOD 4: `housel_08_mlod.rvmat`
            // (PixelShaderID NormalMapSpecularDIMap) declares Stage0
            // `#(argb,8,8,3)color(0,0,0,1,CO)` -- pure black -- while the section's face carries
            // `house_l_8_mlod_co.paa`. Binding the constant painted a single 13.45 m face solid
            // black: flat, one constant normal, untextured-looking, unlit-looking, and immune
            // to every lighting toggle, because the albedo is literally zero.
            //
            // The rule is about information content, not about the colour value: a constant
            // carries no spatial detail, so a real image always beats it. A constant still
            // binds when the section has NO face texture -- `mil_wallbig_multi` really does
            // name its whole surface that way, and that case is unaffected.
            //
            // The CPU direct path already refuses procedural base stages outright
            // (DrawSectionTL tests `!base->texture.isProcedural`), which is why the same
            // geometry drew light there and black here under WGR_GPU_DRIVEN=0 vs 1.
            //
            // WGR_CONSTANT_BASE_OVERRIDE=1 restores the old precedence for A/B.
            static const bool constantOverridesFace = []
            {
                const char* v = std::getenv("WGR_CONSTANT_BASE_OVERRIDE");
                return v && std::strcmp(v, "0") != 0;
            }();
            bool           baseFromConstant = false;
            // MAT-050: set when nothing resolved this section's albedo and the neutral 1x1
            // stand-in was substituted. A substituted albedo is NOT a real one, so anything
            // downstream that keys off "has a texture" must keep treating it as absent --
            // notably the DZ-003 reflection, which on a fallback albedo produces the same
            // blown-out surface the `mat.texture_id != 0` guard below was added to prevent.
            bool           sectionUntextured = false;
            const uint64_t baseColour = ResolveMaterialBaseColour(s->GetSection(i), &baseFromConstant);
            const bool     faceHasTexture = mat.texture_id != 0;
            // Suppressed: the RVMAT offered only a constant and the face has the real image.
            // DZ-006: `CalmWater` is the measured exception, kept in step with DrawSectionTL's
            // copy (which carries the full evidence). Every DayZ water p3d names the same
            // roadway surface-type texture on its visible face; the material's constant is the
            // only real albedo, so here the constant wins even when the face has a texture.
            const bool calmWaterConstant = [&]
            {
                if (!baseFromConstant)
                    return false;
                const ShapeSection& secW = s->GetSection(i);
                const char* mpW = secW.surfMat ? secW.surfMat->GetName().Data() : nullptr;
                if (!mpW || !*mpW)
                    return false;
                const auto cachedW = _materialBindings.find(mpW);
                return cachedW != _materialBindings.end() && Asset::Material::IsCalmWater(cachedW->second);
            }();
            // RFG-060: an `enfa|coverage|colour` face texture is the only complete albedo
            // the section has -- the material's Stage0 is the same image WITHOUT its
            // coverage. See FaceTextureIsEnfusionComposite.
            const bool faceIsComposite = FaceTextureIsEnfusionComposite(s->GetSection(i));
            if (faceIsComposite && baseColour != 0)
            {
                const ShapeSection& secX = s->GetSection(i);
                const char* mpX = secX.surfMat ? secX.surfMat->GetName().Data() : "<none>";
                static std::unordered_set<std::string> loggedCompositeKept;
                if (loggedCompositeKept.insert(std::string(shape->Name()) + "|" + (mpX ? mpX : "")).second)
                    LOG_INFO(Graphics,
                             "Wgpu material: Stage0 NOT overriding an enfa| composite face texture model={} lod={} "
                             "section={} rvmat={}",
                             shape->Name(), level, i, mpX);
            }
            const bool constantYieldsToFace =
                baseColour != 0 && baseFromConstant && faceHasTexture && !constantOverridesFace && !calmWaterConstant;
            if (faceIsComposite)
            {
                // Keep the composite. Nothing else in this block applies.
            }
            else if (constantYieldsToFace)
            {
                const ShapeSection& secC = s->GetSection(i);
                const char*         mpC = secC.surfMat ? secC.surfMat->GetName().Data() : "<none>";
                static std::unordered_set<std::string> loggedConstantKept;
                if (loggedConstantKept.insert(std::string(shape->Name()) + "|" + (mpC ? mpC : "")).second)
                    LOG_INFO(Graphics,
                             "Wgpu material: constant-colour base NOT overriding face texture model={} lod={} "
                             "section={} rvmat={}",
                             shape->Name(), level, i, mpC);
            }
            else if (baseColour)
            {
                mat.texture_id = baseColour;
                // A LARGE face whose albedo is a procedural CONSTANT colour is the other way
                // to get a flat, untextured-looking, constant-normal surface -- and it is
                // invisible to the UNTEXTURED warning, because a constant-colour stage binds a
                // real texture handle. ResolveMaterialBaseColour accepts those deliberately
                // (`mil_wallbig_multi` names its whole surface that way), which is correct, but
                // it means "has a texture" does not imply "looks textured". Reported only for
                // faces big enough to matter on screen, so the list stays short.
                if (const ShapeSection& secP = s->GetSection(i); secP.surfMat)
                {
                    const char* mp = secP.surfMat->GetName().Data();
                    if (const auto cachedP = _materialBindings.find(mp ? mp : ""); cachedP != _materialBindings.end())
                    {
                        const char* whichStage = "Stage0";
                        const auto* baseStage = EffectiveBaseColourStage(cachedP->second, whichStage);
                        if (baseStage && baseStage->texture.isProcedural)
                        {
                            int   pFaces = 0, pBig = 0;
                            float pDiag = 0.0f, pMax = 0.0f, pLo[3], pHi[3];
                            sectionExtent(i, pFaces, pDiag, pMax, pBig, pLo, pHi);
                            if (pMax >= 3.0f)
                            {
                                static std::unordered_set<std::string> loggedFlatBase;
                                if (loggedFlatBase.insert(std::string(shape->Name()) + "|" + mp).second)
                                    LOG_WARN(Graphics,
                                             "Wgpu material: LARGE face with CONSTANT-colour albedo model={} lod={} "
                                             "section={} rvmat={} stage={} raw={} faces={} diag={} maxFace={} "
                                             "bigFaces={} min=({},{},{}) max=({},{},{})",
                                             shape->Name(), level, i, mp, whichStage, baseStage->texture.raw, pFaces,
                                             pDiag, pMax, pBig, pLo[0], pLo[1], pLo[2], pHi[0], pHi[1], pHi[2]);
                            }
                        }
                    }
                }
            }
            else if (mat.texture_id == 0)
            {
                sectionUntextured = true;
                // MAT-043: the terminal case. No RVMAT albedo AND no face texture to stand
                // in, so this section will draw with the no-texture default -- flat light
                // grey, near-black once lit. `fallback=none` in ResolveMaterialBaseColour's
                // warning is what found the fort rampart in a single grep, but it names only
                // the RVMAT, and an RVMAT is shared across models. Name the model, LOD and
                // section too, so the next one is identified without a bisection hunt.
                const ShapeSection& sec2 = s->GetSection(i);
                const char* rvmat = sec2.surfMat ? sec2.surfMat->GetName().Data() : "<none>";
                // Model-space extent of the offending section. Size is what decides whether an
                // untextured section is a cosmetic speck or the thing filling the screen, and a
                // recolouring probe cannot be used to find out -- the debug-RVMAT selector also
                // diverts the object off the retained path, so it changes two things at once.
                // This perturbs nothing: combine it with the instance position from
                // WGR_INSTANCE_TRACE to place the section in the world.
                // `diag` is the span of the section's SCATTER, not the size of anything the GPU
                // actually draws, and reading it as an object size is the trap here: 36
                // one-metre proxy markers spread through a mosque report diag=26 while every
                // triangle is a speck. `maxFace` is the largest single face -- the number that
                // decides "speck or slab" -- and `bigFaces` counts how many could cover a chunk
                // of screen on their own.
                int   secFaces = 0, bigFaces = 0;
                float diag = 0.0f, maxFace = 0.0f, lo[3], hi[3];
                sectionExtent(i, secFaces, diag, maxFace, bigFaces, lo, hi);
                // MAT-050: bind the neutral stand-in rather than leaving the section on white.
                // Counted unconditionally -- the WARNING is deduped per (model, rvmat), so it
                // reports first occurrences, and first occurrences are not a denominator.
                ++_untexturedRetainedSections;
                // Same event, counted again for the coverage line's numerator -- which,
                // unlike this member, is reported against a denominator and a triangle
                // weight. See the g_bakedSection* block above RegisterGpuModel.
                ++g_bakedSectionsUntextured;
                g_bakedSectionTrisUntextured += sec.index_count / 3;
                // Scope guard (MAT-050), the twin of DrawSectionTL's: only a section that
                // NAMED an RVMAT is corrected by default. `sec2.surfMat` is null exactly when
                // `rvmat` above degraded to "<none>".
                const bool sectionNamesMaterial = !sec2.surfMat.IsNull() && rvmat && *rvmat;
                const uint64_t neutral = UntexturedAlbedoHandle(sectionNamesMaterial);
                if (neutral)
                    mat.texture_id = neutral;
                static std::unordered_set<std::string> loggedUntextured;
                if (loggedUntextured.insert(std::string(shape->Name()) + "|" + rvmat).second)
                    LOG_WARN(Graphics,
                             "Wgpu material: section draws UNTEXTURED (fallback={}) model={} lod={} section={} "
                             "rvmat={} faces={} diag={} maxFace={} bigFaces={} min=({},{},{}) max=({},{},{}) "
                             "untexturedRetainedSections={}",
                             neutral ? "neutral" : "none", shape->Name(), level, i, rvmat, secFaces, diag, maxFace,
                             bigFaces, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], _untexturedRetainedSections);
            }
            // The per-texel specular map, when the material names one. With it bound
            // the flat constant is applied in full and the map scales it per texel,
            // which is what the constant means; without one the approximation below
            // stands in. Ordering matters: ResolveMaterialBaseColour above is what
            // populates the translation cache both of these read.
            mat.specular_texture_id = ResolveMaterialSpecularMap(s->GetSection(i));
            // Stage1. Same ordering dependency: the translation cache is populated
            // by ResolveMaterialBaseColour above.
            bool normalRgPacked = false;
            bool nativeCavity = false;
            mat.normal_texture_id = ResolveMaterialNormalMap(s->GetSection(i), &normalRgPacked, &nativeCavity);
            // Authored normal intensity rides the same record; the 1.0 default keeps
            // every material that names none exactly where it was.
            mat.normal_power = ResolveMaterialNormalPower(s->GetSection(i));
            // Crown self-occlusion volume; zero lanes when unnamed (no-op).
            {
                float aoP0[4], aoP1[4];
                ResolveMaterialCrownAo(s->GetSection(i), aoP0, aoP1);
                mat.crown_ao_p0 = {aoP0[0], aoP0[1], aoP0[2], aoP0[3]};
                mat.crown_ao_p1 = {aoP1[0], aoP1[1], aoP1[2], aoP1[3]};
            }
            if (nativeCavity)
            {
                sec.flags |= WGR_MODEL_SECTION_NATIVE_CAVITY;
                static bool reportedNativeCavity = false;
                if (!reportedNativeCavity)
                {
                    LOG_INFO(Graphics, "Wgpu native foliage: preserved NTC cavity bound; shader opt-in WGR_NATIVE_LEAF_CAVITY=1");
                    reportedNativeCavity = true;
                }
            }
            // RFG-047: tell the shader which two channels the normal is in. Only ever set
            // for a compressed Enfusion `_NMO`; a decoded one keeps RFG-045's R-into-A copy
            // and leaves this clear, so the two forms of the same file shade identically.
            if (normalRgPacked)
            {
                sec.flags |= WGR_MODEL_SECTION_NORMAL_RG;
            }
            // DZ-003: an EnvironmentMap in the material is the declaration "this surface is
            // reflective". Rides in the section's flags word (WgrModelMaterial is
            // size-locked); the shader adds a Fresnel-weighted sky reflection for it and
            // changes nothing for every material that names none. Same cache ordering.
            //
            // DZ-006: and only when the albedo actually bound. mat.texture_id == 0 leaves the
            // section on the white bindless fallback, where a sky reflection produces a bright
            // wrong surface instead of a dull one. See the per-draw twin in DrawSectionTL.
            // Fixture emissive survives the night; legacy compensation emissive does not
            // (gpu_driven.wgsl `emissive_night`, the twin of the per-draw flag).
            if (MaterialEmitsAtNight(s->GetSection(i).surfMat))
            {
                sec.flags |= WGR_MODEL_SECTION_NIGHT_EMITTER;
            }
            if (MaterialIsReflective(s->GetSection(i)))
            {
                if (mat.texture_id != 0 && !sectionUntextured)
                {
                    sec.flags |= WGR_MODEL_SECTION_REFLECTIVE;
                }
                else
                {
                    const ShapeSection& secR = s->GetSection(i);
                    const char* mpR = secR.surfMat ? secR.surfMat->GetName().Data() : "<none>";
                    static std::unordered_set<std::string> loggedUnboundReflective;
                    if (loggedUnboundReflective.insert(mpR ? mpR : "").second)
                        LOG_WARN(Graphics,
                                 "Wgpu material: reflection SUPPRESSED for {} -- its base colour did not bind, "
                                 "so the section is on the white fallback and a reflection would blow it out",
                                 mpR);
                }
            }
            // Multi's further layers, same ordering dependency on the translation cache.
            // RFG-074: name every shape whose sections reach the layer resolver, once.
            // Whether an object is on the retained path has been argued from admission
            // percentages; this answers it per shape, by name, at the only call site.
            {
                static std::unordered_set<std::string> saidClassified;
                const char* nm = (shape != nullptr && shape->Name() != nullptr) ? shape->Name() : "(unnamed)";
                if (saidClassified.insert(nm).second)
                    LOG_INFO(Graphics, "Wgpu material: retained classifier reached shape {}", nm);
            }
            // RFG-074: for one named shape, print every (level, section) with the surfMat
            // name the classifier actually sees. The church's LOD0 materials never appear
            // in any material log while its coarse-level materials do, and no earlier
            // exit accounts for it -- so the classifier is asked directly what it holds.
            {
                static const std::string traceShape = []
                {
                    const char* v = std::getenv("POSEIDON_TRACE_SHAPE");
                    return std::string(v != nullptr ? v : "");
                }();
                if (!traceShape.empty() && shape != nullptr && shape->Name() != nullptr &&
                    std::string(shape->Name()).find(traceShape) != std::string::npos)
                {
                    const ShapeSection& sec = s->GetSection(i);
                    auto* face = static_cast<TextureWgpu*>(sec.properties.GetTexture());
                    LOG_INFO(Graphics, "Wgpu material: TRACE {} level {} section {} surfMat='{}' face='{}'",
                             shape->Name(), level, i,
                             sec.surfMat && sec.surfMat->GetName().Data() ? sec.surfMat->GetName().Data() : "(null)",
                             face && face->GetName().Data() ? face->GetName().Data() : "(none)");
                }
            }
            ResolveMaterialLayers(s->GetSection(i), mat);
            // Multi's mask lives on the second UV set in the BI corpora (TexGen uvSource
            // "tex1") and in Reforger's (`UVSrcGlobalMaps "UV set 2"`, RFG-072); the shader
            // samples it with uv1 when told so -- and ONLY when the shape carries a second
            // set. Without one MeshBuild copies uv0 into uv1, so the flag would change nothing
            // on a converted model exported without `--second-uv`, and the census below would
            // claim a uv1 blend that never happened.
            if (mat.mask_texture_id != 0)
            {
                const bool wantsUv1 = MaterialMaskUsesUv1(s->GetSection(i));
                const bool hasUv1 = s->HasUV1();
                if (wantsUv1 && hasUv1)
                {
                    sec.flags |= WGR_MODEL_SECTION_MASK_UV1;
                }
                // The global normal map rides the same second set when the
                // material says so (`UVSrcGlobNormal`); sampled raw, unscaled,
                // since the tiling transform belongs to the colour tile.
                if (mat.normal_texture_id != 0 && MaterialNormalUsesUv1(s->GetSection(i)) && s->HasUV1())
                {
                    sec.flags |= WGR_MODEL_SECTION_GLOBALNMO_UV1;
                }
                // RFG-072 census: the UV half, per distinct shape+material, and the line.
                const ShapeSection& secM = s->GetSection(i);
                const char* mpM = secM.surfMat ? secM.surfMat->GetName().Data() : nullptr;
                static std::unordered_set<std::string> censused;
                if (mpM && censused.insert(mpM).second)
                {
                    const auto cachedM = _materialBindings.find(mpM);
                    if (cachedM != _materialBindings.end() && cachedM->second.shaderFamily == "MatPBRMulti")
                    {
                        if (wantsUv1 && hasUv1)
                            ++GMultiLayerCensus.composedUv1;
                        else if (wantsUv1)
                            ++GMultiLayerCensus.composedUv0NoSet;
                        else
                            ++GMultiLayerCensus.composedUv0;
                        const MultiLayerCensus& c = GMultiLayerCensus;
                        const size_t n = c.multi;
                        // Once at 1, 2, 4, 8, ... and every 50th after, so a load of 300
                        // materials prints a dozen lines and the last one is the total.
                        if ((n & (n - 1)) == 0 || n % 50 == 0)
                            LOG_INFO(Graphics,
                                     "Wgpu material: MatPBRMulti census -- {} distinct materials: {} reach the layer "
                                     "blend ({} mask on UV set 2, {} on uv0 because the shape has no second set, {} "
                                     "on uv0 by the material; {} wear Color_N per layer), {} fall back to one image "
                                     "({} name no MaskMap, {} MaskMap failed to upload) [switch {}]",
                                     n, c.composed, c.composedUv1, c.composedUv0NoSet, c.composedUv0, c.composedTinted,
                                     c.singleNoMask + c.singleMaskFailed, c.singleNoMask, c.singleMaskFailed,
                                     Poseidon::Enfusion::MultiLayersEnabled() ? "on" : "OFF");
                    }
                }
            }
            if (const float specScale = MaterialSpecularScale(s->GetSection(i), mat.specular_texture_id);
                specScale < 1.0f)
            {
                mat.specular.x *= specScale;
                mat.specular.y *= specScale;
                mat.specular.z *= specScale;
            }
            // A/B probe for the DayZ distant-foliage colour (audit 2026-08-15). DayZ's coarse
            // tree LODs bind a near-WHITE canopy billboard and expect the TreeAdv shader to
            // colourise it -- the colour lives in `treeColorization[]`, the WorldPos-mapped
            // Stage3 map, and the `ambient[]` tint, none of which are generic lighting terms.
            // The renderer has no TreeAdv colorization, so it multiplies a white albedo by a
            // literal ambient of [1, 1, 0.3] and every distant beech reads saturated yellow.
            // Off by default: this only NEUTRALISES the tint, it does not implement the family,
            // so it is a measurement lever, not a fix. WGR_TREEADV_NEUTRAL_AMBIENT=1 enables it.
            static const bool treeAdvNeutralAmbient = []
            {
                const char* v = std::getenv("WGR_TREEADV_NEUTRAL_AMBIENT");
                return v && std::strcmp(v, "0") != 0;
            }();
            if (treeAdvNeutralAmbient)
            {
                const ShapeSection& secT = s->GetSection(i);
                const char* mpT = secT.surfMat ? secT.surfMat->GetName().Data() : nullptr;
                if (mpT && *mpT)
                {
                    if (const auto cachedT = _materialBindings.find(mpT); cachedT != _materialBindings.end() &&
                        cachedT->second.shaderFamily.compare(0, 7, "TreeAdv") == 0)
                    {
                        const float grey = (mat.ambient.x + mat.ambient.y + mat.ambient.z) / 3.0f;
                        mat.ambient = {grey, grey, grey, mat.ambient.w};
                    }
                }
            }
            ++g_bakedSections;
            g_bakedSectionTris += sec.index_count / 3;
            sections.push_back(sec);
            materials.push_back(mat);
            count++;
            if (render::GObjectAdmitProfile.active)
                ++render::GObjectAdmitProfile.sections;
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
        AdmitTimer ffiTimer(&render::GObjectAdmitProfile.modelRegisterMs);
        // REN-BUG 2026-08-31: a scripted counter insertion landed BETWEEN `model =` and the
        // call, making `model = _ops.modelRegister++;` -- every registered model's handle
        // became the counter value and the real id was discarded. It survived the Abel bench
        // because on a fresh run both sequences count 0,1,2,... in step; on Everon, where
        // eviction re-registers models out of order, every building drew with a stranger's
        // id or none: walls gone, floating windows and ivy. The counter goes on its own
        // statement, ABOVE the assignment, where it can never eat one again.
        _ops.modelRegister++;
        ResourceOp op;
        op.kind = ResourceOp::ModelRegister;
        // WGR_INVALID_MODEL must never be allocated as a handle, or an ineligible shape and a
        // real one would collide in _modelIdOf.
        if (_nextModelHandle == WGR_INVALID_MODEL)
            _nextModelHandle++;
        op.model = _nextModelHandle++;
        op.sphereRadius = shape->BoundingSphere();
        op.lods = std::move(lods);
        op.sections = std::move(sections); // .mesh fields are producer handles; drain translates
        op.materials = std::move(materials);
        op.name = static_cast<const char*>(shape->Name());
        model = op.model;
        _resourceOps.push_back(std::move(op));
    }
    // The census's shape-level verdict. `eligible` with nothing to draw is its OWN reason and
    // the one most likely to be the answer on imported content: every bail above is narrow and
    // env-gated, while "no section survived ClassifyGpuSection" is what a whole material family
    // the classifier does not recognise looks like.
    if (model == WGR_INVALID_MODEL && rejectWhy == AdmitReject::Admitted)
    {
        rejectWhy = AdmitReject::NoOwnedSection;
        // "No section survived" is one step short of an answer: it says the classifier refused
        // everything without saying on what grounds, and the grounds are four different
        // predicates over the section spec, the texture's alpha histogram and the material path.
        // Dump LOD 0 for the first few shapes that land here so the run that reports the
        // histogram also names the predicate -- otherwise this costs a second session and a
        // second world load to learn what one printf already knew.
        static int loggedNoOwned = 0;
        if (loggedNoOwned < 3)
        {
            ++loggedNoOwned;
            const Shape* s0 = shape->NLevels() > 0 ? shape->Level(0) : nullptr;
            LOG_WARN(Graphics, "GpuModel no-owned-section: {} lods={} lod0 sections={}", shape->Name(),
                     shape->NLevels(), s0 ? s0->NSections() : -1);
            for (int i = 0; s0 && i < s0->NSections() && i < 8; ++i)
            {
                const ShapeSection& shSec = s0->GetSection(i);
                auto* sectionTexture = static_cast<TextureWgpu*>(shSec.properties.GetTexture());
                const int spec = shSec.properties.Special();
                const render::RenderPassDescriptor desc =
                    render::BuildRenderPassDescriptor(render::SplitLegacy(spec));
                LOG_WARN(Graphics,
                         "  section {}: special=0x{:x} blend={} surface={} tex={} alpha={} mat={} owned={}", i, spec,
                         static_cast<int>(desc.blend), static_cast<int>(desc.surface),
                         sectionTexture ? sectionTexture->Name() : "(none)",
                         sectionTexture ? static_cast<int>(sectionTexture->GetAlphaClass()) : -1,
                         shSec.surfMat.IsNull() ? "(none)" : shSec.surfMat->GetName().Data(),
                         SectionGpuOwned(shSec) ? 1 : 0);
            }
        }
    }
    _gpuModelReject[shape] = rejectWhy;
    _gpuModels[shape] = model;
    // Cache coverage so SceneObjectCreated can tag each instance and GpuDrivenCoverage can tell
    // the scene loop whether to suppress the whole CPU draw or only the GPU-owned sections.
    _gpuModelCoverage[shape] = (hasProxies || hasComplement) ? GpuDrawCoverage::Partial : GpuDrawCoverage::Full;
    // MAT-051 diagnostic (temporary): WGR_COVERAGE_TRACE=1 prints each shape's
    // classification with its complement inputs, so a glass-owning vehicle
    // stamped Full names itself instead of silently losing its canopy.
    if (const char* trace = std::getenv("WGR_COVERAGE_TRACE"); trace && *trace == '1')
    {
        LOG_WARN(Graphics, "GpuModel coverage: {} -> {} (hasProxies={} hasComplement={} lods={} sections={})",
                 shape->Name(), (hasProxies || hasComplement) ? "Partial" : "Full", hasProxies ? 1 : 0,
                 hasComplement ? 1 : 0, lods.size(), sections.size());
    }
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
    if (model != WGR_INVALID_MODEL)
    {
        // The handles baked into `materials` are the ones these textures hold RIGHT NOW (every
        // EnsureUploaded above ran on this thread, before this line). Recorded so a later
        // unpark can prove nothing moved.
        std::vector<ModelTextureBinding> handles;
        handles.reserve(modelTextures.size());
        for (const TextureWgpu* texture : modelTextures)
            handles.push_back({texture ? texture->GpuHandle() : 0, texture ? texture->SlotLease() : 0});
        _gpuModelTextureHandles[shape] = std::move(handles);
        _gpuModelMeshes[shape] = std::move(modelMeshes);
        _gpuModelTextures[shape] = std::move(modelTextures);
    }
    else
    {
        // A late eligibility failure may follow successful LOD uploads. They
        // never belong to a registered model, so return their pool ranges now.
        for (uint64_t mesh : modelMeshes)
        {
            _ops.meshDestroy++;
            ResourceOp op;
            op.kind = ResourceOp::MeshDestroy;
            op.mesh = mesh;
            _resourceOps.push_back(std::move(op));
            _gpuMeshes.erase(mesh);
        }
    }
    return model;
}

namespace
{
// Opt-in revert of the model-release texture eviction described at the call site.
bool EvictTexturesOnModelRelease()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_EVICT_ON_MODEL_RELEASE");
        return v && *v && *v != '0';
    }();
    return on;
}

bool IsTerrainPageTexture(const TextureWgpu* texture)
{
    const char* name = texture ? texture->Name() : nullptr;
    if (!name)
        return false;
    return ContainsNoCase(name, "\\layers\\") || ContainsNoCase(name, "/layers/") ||
           ContainsNoCase(name, "_lca.") || ContainsNoCase(name, "_lco.");
}
} // namespace

void EngineWgpu::RetainGpuModel(const LODShapeWithShadow* shape)
{
    if (!shape)
        return;
    uint32_t& refs = _gpuModelRefs[shape];
    if (refs++ != 0)
        return;
    if (const auto found = _gpuModelTextures.find(shape); found != _gpuModelTextures.end())
        for (TextureWgpu* texture : found->second)
            if (texture)
                texture->PinResident();
}

void EngineWgpu::ReleaseGpuModel(const LODShapeWithShadow* shape)
{
    if (!shape || !_renderer)
        return;
    const auto refIt = _gpuModelRefs.find(shape);
    if (refIt == _gpuModelRefs.end() || refIt->second == 0)
        return;
    if (--refIt->second != 0)
        return;
    _gpuModelRefs.erase(refIt);

    uint32_t evictedTextures = 0;
    if (const auto found = _gpuModelTextures.find(shape); found != _gpuModelTextures.end())
    {
        for (TextureWgpu* texture : found->second)
        {
            if (!texture)
                continue;
            texture->UnpinResident();
            // Unpin only. Releasing a MODEL is not a reason to destroy a TEXTURE.
            //
            // This used to evict here, which turned a model-lifetime event into a
            // texture eviction with no budget check and no grace period -- it
            // pre-empted TextureBankWgpu::TrimResidency, which already makes exactly
            // this decision correctly (budget-gated, ordered by last-used frame, with
            // a 120-frame grace). The LRU was not choosing badly; it was starved,
            // because RetainGpuModel pins every texture of every registered model and
            // TrimResidency skips anything pinned. Measured on DayZ: 1-35 eviction
            // candidates against 1,043 distinct models in view.
            //
            // The cost of evicting here lands on the way back: EvictGpu clears
            // _uploadTried, and the re-upload re-reads the file once per mip level
            // (TextureSourcePac::GetMipmapData opens the stream per level), so flying
            // out of an area and back re-transcodes every mip of every texture.
            //
            // Dropping to zero pins now simply makes the texture an eviction
            // CANDIDATE, and the grace period naturally covers a fly-out-and-back.
            // WGR_EVICT_ON_MODEL_RELEASE=1 restores the old behaviour so both arms of
            // an A/B come from one binary.
            if (EvictTexturesOnModelRelease())
            {
                // Terrain owns its own page lifetime and may share a material image
                // with an object. Never invalidate a handle held by its Rust bind.
                // A texture may also feed an active direct draw, which has no
                // retained-model pin. EnsureUploaded touches those every frame.
                // Keep two submissions of safety so removing a retained model early
                // in this frame cannot invalidate a direct draw's handle.
                const bool directUseQuiescent =
                    _wbank && _wbank->ResidencyFrame() >= texture->LastUsedFrame() + 2;
                if (texture->ResidentPins() == 0 && directUseQuiescent && !IsTerrainPageTexture(texture) &&
                    texture->GpuHandle())
                {
                    texture->EvictGpu();
                    ++evictedTextures;
                }
            }
        }
        // (Not erased here any more: the texture list stays with the registration, parked or
        // destroyed below.)
    }

    // PARK, do not destroy (WGR_GPU_MODEL_PARK). The registration -- pool meshes, baked
    // materials, model id -- stays keyed by the shape; if this shape is re-admitted while it
    // is still alive (the streamer's evict cache is what keeps it so), RegisterGpuModel finds it
    // and the whole rebuild is skipped. Bounded: the oldest parked entry is destroyed when the
    // limit is reached; a shape's death destroys its own (OnShapeDestroyed). With the old
    // eviction opt-in above (which destroys the textures' GPU copies) parking is pointless --
    // the unpark would find every handle stale -- so it destroys as before.
    const size_t parkLimit = GpuModelParkLimit();
    if (parkLimit > 0 && !EvictTexturesOnModelRelease() && _gpuModels.count(shape) != 0)
    {
        while (_gpuModelParked.size() >= parkLimit)
        {
            const LODShapeWithShadow* victim = _gpuModelParked.front();
            DestroyGpuModel(victim); // pops it from the park too
        }
        _gpuModelParked.push_back(shape);
        _gpuModelParkedIndex[shape] = std::prev(_gpuModelParked.end());
        if (std::getenv("WGR_MODEL_RESIDENCY_TRACE"))
            LOG_INFO(Graphics, "Wgpu model residency park: model={} parked={}/{}", shape->Name(),
                     _gpuModelParked.size(), parkLimit);
        return;
    }

    const uint32_t releasedMeshes = DestroyGpuModelMeshes(shape);
    _gpuModelTextures.erase(shape);
    _gpuModelTextureHandles.erase(shape);
    _gpuModels.erase(shape);
    _gpuModelCoverage.erase(shape);
    _gpuModelComplement.erase(shape);
    _gpuConformShapes.erase(shape);
    if (std::getenv("WGR_MODEL_RESIDENCY_TRACE") && (releasedMeshes || evictedTextures))
        LOG_INFO(Graphics, "Wgpu model residency release: model={} meshes={} textures={}", shape->Name(),
                 releasedMeshes, evictedTextures);
}

namespace
{
// How many frames a full pass over the retained instance table is spread across. 30 at 60 fps
// is half a second, comfortably inside TrimResidency's 120-frame grace, so a model that is in
// range gets its textures marked long before they could age out. Lowering it costs CPU per
// frame; raising it past the grace would let a live model's textures die of the lag alone.
constexpr size_t kRetainedLivenessSweepFrames = 30;
// Never walk fewer than this many buckets per frame: on a small table the division above would
// otherwise round to a crawl for no saving at all.
constexpr size_t kRetainedLivenessMinBuckets = 256;

// Multiplier on the object draw distance for the "in range" test. Slightly over 1 because the
// draw distance is measured to an object's centre while the cull works on its bounds, and
// because the streamer admits a little beyond what it draws; the cost of the margin is a few
// extra resident textures, the cost of being short is a white wall at the edge of view.
float RetainedLivenessRangeScale()
{
    static const float scale = []
    {
        if (const char* v = std::getenv("WGR_RETAINED_LIVENESS_MARGIN"))
            return std::clamp(static_cast<float>(std::atof(v)), 1.0f, 8.0f);
        return 1.15f;
    }();
    return scale;
}

// WGR_RETAINED_LIVENESS=0 turns the whole signal off, which puts TrimResidency back on the
// unconditional pin exemption. Both arms of the A/B therefore come out of one binary -- the
// same discipline WGR_EVICT_ON_MODEL_RELEASE follows above.
// DEFAULT OFF since 2026-08-31, and the reason is a measurement that contradicts the
// premise this was built on. The signal itself is sound and stays in the binary; what
// is NOT established is that relaxing the pin exemption buys anything.
//
// The white buildings this was meant to cure are not caused by texture eviction at
// all. Measured on the 30 m/s Everon traverse, same pose, four arms:
//   * WGR_DYNAMIC_VRAM_MB=900, liveness ON   -> 60 evictions, every row pinned=0, white
//   * WGR_DYNAMIC_VRAM_MB=900, liveness OFF  -> 13 evictions, every row pinned=0, white
//   * WGR_DYNAMIC_VRAM_MB=900 + grace=36000  ->  0 evictions,                    white
//   * default budget (5611 MB probed)        ->  0 evictions, over budget false, white
// and the same blocks stay white with WGR_FAR_TIER=0, so they are neither far-tier
// stand-ins nor evicted textures. The `draws UNTEXTURED` count is identical (70) in
// every arm including the unpressured one: it is a constant property of this world at
// this distance, not a residency symptom.
//
// So the pin exemption would be a relaxation of a safety check bought with an unproven
// benefit, on the same night a wrong relaxation of a different check made soldiers
// invisible. It ships OFF until someone shows an eviction that a live model actually
// needed. WGR_RETAINED_LIVENESS=1 turns it on for that experiment.
bool RetainedLivenessEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_RETAINED_LIVENESS");
        return v && *v == '1';
    }();
    return on;
}
} // namespace

bool EngineWgpu::RetainedModelIsLive(const LODShapeWithShadow* shape) const
{
    const auto it = _livenessMarked.find(shape);
    if (it == _livenessMarked.end())
        return false;
    // Marks carry the id of the pass that was IN PROGRESS when they were made, i.e.
    // _livenessPass + 1. Accepting >= _livenessPass therefore accepts the pass now running and
    // the last completed one, and rejects anything older.
    return it->second >= _livenessPass;
}

void EngineWgpu::MarkRetainedModelLiveness()
{
    // Every early return clears the flag rather than leaving it set: TrimResidency may only
    // evict a pinned texture while this sweep is actually feeding it, and "the sweep stopped
    // running" must not read as "everything is dead".
    if (!_gpuDriven || !_wbank || !RetainedLivenessEnabled() || _gpuInstances.empty())
    {
        if (_wbank)
            _wbank->SetRetainedLivenessActive(false);
        return;
    }
    const render::frame::PresentationSnapshot& snap = render::frame::GPresentationSnapshot();
    if (!snap.valid || !snap.hasCamera || !snap.hasScene || snap.objectDrawDistance <= 0.0f)
    {
        _wbank->SetRetainedLivenessActive(false);
        return;
    }
    const Vector3 camPos = snap.cameraPos;
    const float range = snap.objectDrawDistance * RetainedLivenessRangeScale();
    const float rangeSq = range * range;

    // THE VALVE. A pass can be restarted from the top by a rehash, and a table that keeps
    // rehashing (heavy streaming, world load) can keep restarting it. If passes stop COMPLETING
    // for longer than the age the marks are trusted to, the marks are no longer evidence and
    // the pin exemption must come back -- otherwise the LRU would read "not marked lately" off
    // a sweep that simply never got there and evict a building in plain view.
    const uint64_t maxMarkAge = 2 * kRetainedLivenessSweepFrames;
    if (_livenessPass != 0 && _wbank->ResidencyFrame() > _livenessPassFrame + maxMarkAge)
        _wbank->SetRetainedLivenessActive(false);

    // A rehash moves every entry, so a cursor into the old bucket layout means nothing. Restart
    // the pass; the table settles quickly and passes then run to completion.
    const size_t buckets = _gpuInstances.bucket_count();
    if (buckets != _livenessBuckets)
    {
        _livenessBuckets = buckets;
        _livenessCursor = 0;
    }
    if (buckets == 0)
        return;
    const size_t slice = (buckets + kRetainedLivenessSweepFrames - 1) / kRetainedLivenessSweepFrames;
    const size_t perFrame = std::max(kRetainedLivenessMinBuckets, slice);
    const bool haveProxies = !_gpuProxies.empty();

    auto markShape = [&](const LODShapeWithShadow* shape)
    {
        if (!shape)
            return;
        uint64_t& marked = _livenessMarked[shape];
        if (marked == _livenessPass + 1)
            return; // already marked in the pass now running
        marked = _livenessPass + 1;
        if (const auto found = _gpuModelTextures.find(shape); found != _gpuModelTextures.end())
            for (TextureWgpu* texture : found->second)
                _wbank->NoteTextureUse(texture);
    };

    // Far instances have no Object entry; retain their shared material liveness.
    for (const auto& [shape, count] : _farGpuModelRefs)
        markShape(shape);

    const size_t end = std::min(buckets, _livenessCursor + perFrame);
    for (; _livenessCursor < end; ++_livenessCursor)
    {
        for (auto it = _gpuInstances.begin(_livenessCursor); it != _gpuInstances.end(_livenessCursor); ++it)
        {
            // Registration position, not a live Position(): only static objects are admitted to
            // the retained path, and SceneObjectMoved drops anything that stops being static.
            if ((it->second.pos - camPos).SquareSize() > rangeSq)
                continue;
            markShape(it->second.shape);
            // A parent's furniture children are separate registrations with their own pins, and
            // they are only ever as visible as the parent that carries them.
            if (haveProxies)
                if (const auto pit = _gpuProxies.find(it->first); pit != _gpuProxies.end())
                    for (const auto& child : pit->second.children)
                        markShape(child.shape);
        }
    }
    if (_livenessCursor >= buckets)
    {
        _livenessCursor = 0;
        ++_livenessPass;
        _livenessPassFrame = _wbank->ResidencyFrame();
        // One complete pass is the precondition TrimResidency waits on: from here on every
        // registered model in the working set has had a chance to speak for its textures, so a
        // stale _lastUsedFrame is evidence rather than an artefact of never having been asked.
        _wbank->SetRetainedLivenessActive(true, maxMarkAge);
        if (std::getenv("WGR_MODEL_RESIDENCY_TRACE"))
            LOG_INFO(Graphics, "Wgpu retained liveness: pass={} instances={} marked={} range={:.0f}m", _livenessPass,
                     _gpuInstances.size(), _livenessMarked.size(), range);
    }
}

uint32_t EngineWgpu::InvalidateStaleGpuModels()
{
    // Same staleness test the parked path uses, applied to every registration: the handles
    // baked into the model must still be the ones the textures hold.
    //
    // REFILL FIRST, destroy second. Before the liveness sweep a mismatch could only mean "a
    // texture this model needs was evicted and nothing will ever bring it back", so tearing the
    // registration down and rebuilding it on the next admit was the only repair available. Now
    // that TrimResidency is allowed to evict a PINNED texture, a mismatch is the normal way a
    // model comes back into range, and the cheap repair is to re-upload: REN-RES-001's slot
    // lease means the new upload lands in the same bindless index, so the baked material is
    // correct again the moment the handle exists and no re-registration is needed.
    //
    // The recorded handle is then updated to the new one. That is only sound because of the
    // lease -- it asserts "the slot behind this handle is the slot the material baked" -- so it
    // is done ONLY for a texture that actually holds a lease (SlotLease() != 0). A texture that
    // never got one (bindless array at cap) falls back to per-upload slots, where the baked
    // index really has moved, and it must go down the destroy path exactly as before.
    std::vector<const LODShapeWithShadow*> stale;
    uint32_t refilled = 0, sharedRefills = 0, deferredParked = 0, leaseChangedParked = 0;
    const bool parkRefillEnabled = GpuModelParkRefillEnabled();
    for (auto& [shape, handles] : _gpuModelTextureHandles)
    {
        const auto textures = _gpuModelTextures.find(shape);
        if (textures == _gpuModelTextures.end() || textures->second.size() != handles.size())
        {
            stale.push_back(shape);
            continue;
        }
        // Only a model the sweep says is in range is worth re-uploading; refilling one that is
        // out of range would undo the eviction the same frame it happened and thrash the file
        // reads (EvictGpu clears _uploadTried, so a re-upload re-reads every mip).
        const bool live = RetainedModelIsLive(shape);
        bool broken = false;
        bool deferred = false, leaseChanged = false;
        const auto refs = parkRefillEnabled ? _gpuModelRefs.find(shape) : _gpuModelRefs.end();
        const render::ParkedRefillContext refillContext{
            parkRefillEnabled, parkRefillEnabled && _gpuModelParkedIndex.count(shape) != 0, live,
            refs == _gpuModelRefs.end() ? 0u : refs->second};
        for (size_t i = 0; i < handles.size(); i++)
        {
            TextureWgpu* texture = textures->second[i];
            if (!texture)
            {
                broken = true;
                break;
            }
            if (texture->GpuHandle() == handles[i].handle)
                continue;
            // Another model may already have refilled this shared texture. Its image
            // handle changes, but the baked material slot does not. Require the lease
            // captured at registration: acquiring a NEW lease after an unleased upload
            // is not proof that the old material points there.
            if (handles[i].SameSlot(texture->GpuHandle(), texture->SlotLease()))
            {
                handles[i].handle = texture->GpuHandle();
                ++sharedRefills;
                continue;
            }
            if (parkRefillEnabled && refillContext.parked && refillContext.references == 0 &&
                !refillContext.live && texture->GpuHandle() == 0)
            {
                const render::ParkedImageBinding image{
                    handles[i].handle, handles[i].lease, texture->GpuHandle(), texture->SlotLease(), true,
                    texture->IsDynamicTexture(), texture->HasReloadableSource()};
                if (render::CanDeferParkedImage(refillContext, image))
                {
                    // Keep only the existing bounded parked registration. No image
                    // upload, new retain/pin, or material rebake while out of range.
                    deferred = true;
                    continue;
                }
                leaseChanged |= image.capturedLease != 0 && image.capturedLease != image.currentLease;
            }
            else if (parkRefillEnabled && refillContext.parked && handles[i].lease != 0)
                leaseChanged |= handles[i].lease != texture->SlotLease();
            if (live && texture->GpuHandle() == 0 && handles[i].handle != 0 &&
                handles[i].lease != 0 && texture->SlotLease() == handles[i].lease)
            {
                const uint64_t handle = texture->EnsureUploaded();
                if (handle != 0)
                {
                    handles[i].handle = handle; // same slot, new handle -- see the lease note above
                    ++refilled;
                    continue;
                }
            }
            broken = true;
            break;
        }
        if (broken)
            stale.push_back(shape);
        else if (deferred)
            ++deferredParked;
        if (broken && leaseChanged)
            ++leaseChangedParked;
    }
    if (parkRefillEnabled)
    {
        _parkRefillDormantModels += deferredParked;
        _parkRefillLeaseChangedStale += leaseChangedParked;
        if ((deferredParked || leaseChangedParked) && std::getenv("WGR_MODEL_RESIDENCY_TRACE"))
            LOG_INFO(Graphics,
                     "Wgpu parked refill deferred: models={} leaseChangedStale={} totals deferredChecks={} "
                     "activationImages={} activationFailures={} leaseChangedStale={}",
                     deferredParked, leaseChangedParked, _parkRefillDormantModels,
                     _parkRefillActivationImages, _parkRefillActivationFailures, _parkRefillLeaseChangedStale);
    }
    if (refilled)
        LOG_INFO(Graphics, "Wgpu model residency: {} evicted textures re-uploaded into their leased slots", refilled);
    if (sharedRefills && std::getenv("WGR_RESIDENCY_TRACE"))
        LOG_INFO(Graphics, "Wgpu model residency: {} shared texture replacements retained their baked slots", sharedRefills);
    // Collected first: DestroyGpuModel erases from the very maps being walked.
    for (const LODShapeWithShadow* shape : stale)
        DestroyGpuModel(shape);
    return static_cast<uint32_t>(stale.size());
}

uint32_t EngineWgpu::DestroyGpuModelMeshes(const LODShapeWithShadow* shape)
{
    // All callers destroy this registration; retire its lighting allocation too.
    // FIFO ordering covers registrations that have not reached the render thread yet.
    if (const auto it = _gpuModels.find(shape); _renderer && it != _gpuModels.end() && it->second != WGR_INVALID_MODEL)
    {
        ResourceOp op;
        op.kind = ResourceOp::ModelRetire;
        op.model = it->second;
        _resourceOps.push_back(std::move(op));
    }
    uint32_t releasedMeshes = 0;
    if (const auto found = _gpuModelMeshes.find(shape); found != _gpuModelMeshes.end())
    {
        // RFG-088: the level entries that named these meshes go first, so a CPU buffer created
        // from now on builds its own copy rather than sharing a mesh that is being destroyed.
        for (auto it = _retainedMeshOfLevel.begin(); it != _retainedMeshOfLevel.end();)
        {
            const bool gone = std::find(found->second.begin(), found->second.end(), it->second) != found->second.end();
            it = gone ? _retainedMeshOfLevel.erase(it) : std::next(it);
        }
        for (uint64_t mesh : found->second)
        {
            // Same scripted insertion broke this brace-less if: the counter became the
            // guarded statement and the destroy ran unconditionally. wgr_mesh_destroy
            // tolerates a null renderer so nothing visible came of it, but braces now,
            // so the next inserted line cannot change the meaning again.
            if (_renderer)
            {
                _ops.meshDestroy++;
                ResourceOp op;
                op.kind = ResourceOp::MeshDestroy;
                op.mesh = mesh;
                _resourceOps.push_back(std::move(op));
            }
            _gpuMeshes.erase(mesh);
            ++releasedMeshes;
        }
        _gpuModelMeshes.erase(found);
    }
    return releasedMeshes;
}

void EngineWgpu::UnparkGpuModel(const LODShapeWithShadow* shape)
{
    const auto it = _gpuModelParkedIndex.find(shape);
    if (it == _gpuModelParkedIndex.end())
        return;
    _gpuModelParked.erase(it->second);
    _gpuModelParkedIndex.erase(it);
}

// Tear down everything registered for `shape`, parked or not. Never dereferences the shape:
// OnShapeDestroyed calls this from ~LODShape with a dying pointer that is a key and nothing
// else. Textures are unpinned only if this shape still holds a retain count (a parked entry
// has none).
void EngineWgpu::DestroyGpuModel(const LODShapeWithShadow* shape)
{
    UnparkGpuModel(shape);
    if (const auto refs = _gpuModelRefs.find(shape); refs != _gpuModelRefs.end())
    {
        if (refs->second > 0)
            if (const auto found = _gpuModelTextures.find(shape); found != _gpuModelTextures.end())
                for (TextureWgpu* texture : found->second)
                    if (texture)
                        texture->UnpinResident();
        _gpuModelRefs.erase(refs);
    }
    DestroyGpuModelMeshes(shape);
    _gpuModelTextures.erase(shape);
    _gpuModelTextureHandles.erase(shape);
    _gpuModels.erase(shape);
    _gpuModelCoverage.erase(shape);
    _gpuModelComplement.erase(shape);
    _gpuConformShapes.erase(shape);
    // The liveness map is keyed by the same shape pointer and would otherwise accumulate one
    // entry per shape ever registered, including dead ones whose addresses can be reused.
    _livenessMarked.erase(shape);
}

void EngineWgpu::OnShapeDestroyed(const LODShape* shape)
{
    // LODShapeWithShadow derives from LODShape by single inheritance, so the base subobject IS
    // the object's address and the maps' keys compare equal to it. The static_cast here is a
    // pointer-value cast on an object that is mid-destruction and is not dereferenced.
    const auto* key = static_cast<const LODShapeWithShadow*>(shape);
    if (_gpuModels.count(key) == 0)
        return; // the common case: a shape this renderer never registered
    if (std::getenv("WGR_MODEL_RESIDENCY_TRACE"))
    {
        const auto refs = _gpuModelRefs.find(key);
        LOG_INFO(Graphics, "Wgpu model residency shape died: parked={} live-refs={}",
                 _gpuModelParkedIndex.count(key) ? 1 : 0, refs != _gpuModelRefs.end() ? refs->second : 0u);
    }
    DestroyGpuModel(key);
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
        // RFG-095: a proxy that MapThings replaced with a Thing is drawn by that Thing and by
        // nothing else. Object::DrawProxies already skipped it; this path did not, so every
        // chair the owner shot left a copy of itself standing where the GPU child instance
        // still drew it. Not an eligibility failure either: the parent may still go Full.
        if (pshape != nullptr && MapThings::ProxyHidden(shape, pshape))
        {
            continue;
        }
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
        _ops.instanceAdd++;
        const uint32_t slot = EnqueueInstanceAdd(inst);
        RetainGpuModel(pshape);
        if (render::GObjectAdmitProfile.active)
            ++render::GObjectAdmitProfile.proxyModels;
        set.children.push_back(GpuProxyChild{i, slot, pmodel, pshape});
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
        _ops.instanceRemove++;
        EnqueueInstanceRemove(child.slot);
        ReleaseGpuModel(child.shape);
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
        NoteAdmitReject(!obj->Static() ? AdmitReject::NotStatic
                                       : (obj->IsDestroyed() ? AdmitReject::Destroyed : AdmitReject::AlreadyIn),
                        obj->GetShape(), false);
        return;
    }
    LODShapeWithShadow* shape = obj->GetShape();
    if (!shape)
    {
        NoteAdmitReject(AdmitReject::NoShape, nullptr, false);
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
                {
                    NoteAdmitReject(AdmitReject::MatDebugRvmat, shape, _gpuModels.count(shape) == 0);
                    return;
                }
            }
        }
    }
    AdmitTimer sceneTimer(&render::GObjectAdmitProfile.sceneMs);
    // Whether THIS call is the first sight of the shape decides the census's per-shape column:
    // registration is cached, so every later placement of the same asset is an object-level
    // event only. Asked before RegisterGpuModel, which populates the map.
    const bool firstSight = _gpuModelReject.count(shape) == 0;
    uint32_t model = WGR_INVALID_MODEL;
    {
        AdmitTimer registerTimer(&render::GObjectAdmitProfile.registerMs);
        model = RegisterGpuModel(shape);
    }
    if (model == WGR_INVALID_MODEL)
    {
        const auto why = _gpuModelReject.find(shape);
        NoteAdmitReject(why != _gpuModelReject.end() ? why->second : AdmitReject::NoOwnedSection, shape, firstSight);
        return; // ineligible shape -> object drawn by the CPU path
    }
    NoteAdmitReject(AdmitReject::Admitted, shape, firstSight);
    // Terrain conform: ForestPlain -> mode-1 plane, ClipLand veg/fences -> mode-2, else rigid.
    const ConformPlane cp = GpuConformFor(*obj, *shape, _gpuConformShapes.count(shape) != 0);
    const WgrInstance inst = BuildGpuInstance(*obj, model, cp);
    TraceGpuInstance("created", shape->Name(), obj->Transform(), shape->BoundingSphere(), cp, model);
    _ops.instanceAdd++;
    const uint32_t slot = EnqueueInstanceAdd(inst);
    RetainGpuModel(shape);
    // Full (whole object on the GPU) vs Partial (GPU owns the opaque geometry; the CPU still
    // draws proxies + blend/decal sections) — base decision per shape in RegisterGpuModel.
    const auto covIt = _gpuModelCoverage.find(shape);
    GpuDrawCoverage cov = covIt != _gpuModelCoverage.end() ? covIt->second : GpuDrawCoverage::Full;
    // §12d: a Partial object may carry interior furniture proxies — move the eligible ones onto
    // the GPU as child instances (only Partial objects have proxies; Full ones never do).
    if (cov == GpuDrawCoverage::Partial)
    {
        AdmitTimer proxiesTimer(&render::GObjectAdmitProfile.proxiesMs);
        const bool allProxiesGpu = EmitGpuProxies(obj, shape);
        // §12d-full: if the ONLY reason this was Partial is proxies (no CPU complement) and every
        // proxy moved to the GPU, the whole object is now GPU-drawn -> Full, so DrawSortObject
        // skips its CPU Object::Draw entirely (the last per-building CPU walk goes away).
        if (allProxiesGpu && _gpuModelComplement.count(shape) == 0)
        {
            cov = GpuDrawCoverage::Full;
        }
    }
    _gpuInstances[obj] = GpuInstance{model, slot, shape, obj->Transform().Position(), cp.mode, cov};
    // Mirror the coverage onto the Object so GpuDrivenCoverage() is an O(1) field read (see there).
    obj->SetGpuCoverage(static_cast<int>(cov));
}

bool EngineWgpu::SceneFarObjectCreated(uint32_t id, LODShapeWithShadow* shape, const Matrix4& transform)
{
    if (!_gpuDriven || !_renderer || !shape)
        return false;
    if (_farGpuInstances.count(id))
        return true;
    const uint32_t model = RegisterGpuModel(shape);
    if (model == WGR_INVALID_MODEL || _gpuConformShapes.count(shape))
        return false;
    WgrInstance inst = BuildGpuProxyInstance(transform, model);
    inst.flags = WGR_INSTANCE_FAR_AUTHORED;
    const MapType type = shape->GetMapType();
    if (type == MapBush)
        inst.flags |= WGR_INSTANCE_CANOPY_BUSH;
    else if (type == MapTree || type == MapSmallTree)
    {
        inst.flags |= WGR_INSTANCE_CANOPY_TREE;
        if (IsNativeTreeWindModel(shape->Name()))
            inst.flags |= WGR_INSTANCE_COHERENT_TREE_WIND;
    }
    const uint32_t slot = EnqueueInstanceAdd(inst);
    ++_ops.instanceAdd;
    RetainGpuModel(shape);
    ++_farGpuModelRefs[shape];
    _farGpuInstances.emplace(id, FarGpuInstance{slot, shape});
    return true;
}

void EngineWgpu::SceneFarObjectRemoved(uint32_t id)
{
    const auto found = _farGpuInstances.find(id);
    if (found == _farGpuInstances.end())
        return;
    const auto* shape = found->second.shape.GetRef();
    EnqueueInstanceRemove(found->second.slot);
    ++_ops.instanceRemove;
    if (--_farGpuModelRefs[shape] == 0)
        _farGpuModelRefs.erase(shape);
    ReleaseGpuModel(shape);
    _farGpuInstances.erase(found);
}

void EngineWgpu::ClearFarObjects()
{
    while (!_farGpuInstances.empty())
        SceneFarObjectRemoved(_farGpuInstances.begin()->first);
}

// REN-TEMP-001H: pointer -> stable 16-bit draw-object id, allocated on first sight.
// Cached per bracket (the same object submits many sections in a row). 0 is reserved
// for "no identity"; the 16-bit counter wraps long after the ids it re-issues have
// been evicted.
// REN-TEMP-001 §6.7 — dev-panel Temporal tab bridge.
void EngineWgpu::SetTemporalTuning(const TemporalSettings& t)
{
    _temporalTuning = t;
    if (!_renderer)
        return;
    WgrTemporalTuning w{};
    w.temporal_on = t.temporalOn ? 1u : 0u;
    w.dlss_on = t.dlssOn ? 1u : 0u;
    w.render_scale_pct = static_cast<uint32_t>(std::clamp(t.renderScalePct, 50, 200));
    w.jitter_phases = static_cast<uint32_t>(std::max(0, t.jitterPhases));
    // > 0 is the renderer's "auto" sentinel (real biases are <= 0).
    w.mip_bias = t.mipBiasAuto ? 1.0f : std::clamp(t.mipBias, -4.0f, 0.0f);
    w.reactive_sky = std::clamp(t.reactiveSky, 0.0f, 1.0f);
    w.reactive_water = std::clamp(t.reactiveWater, 0.0f, 1.0f);
    w.flags = (t.dlssAutoExposure ? 1u : 0u) | (t.jitterYFlip ? 2u : 0u) |
              (t.mvRenderSpace ? 4u : 0u) | (t.mvFlip ? 8u : 0u) | (t.fsrOn ? 16u : 0u) |
              (t.dlssSharpen ? 32u : 0u);
    w.fsr_sharpness = std::clamp(t.fsrSharpness, 0.0f, 2.0f);
    _pendingSet.temporal = w;
    _pendingSet.temporalDirty = true;
}

Engine::TemporalInfo EngineWgpu::GetTemporalInfo()
{
    TemporalInfo out{};
    AcquireProducerWindow("temporal info");
    if (!_renderer)
        return out;
    WgrTemporalInfo w{};
    wgr_get_temporal_info(_renderer, &w);
    out.renderWidth = static_cast<int>(w.render_width);
    out.renderHeight = static_cast<int>(w.render_height);
    out.outputWidth = static_cast<int>(w.output_width);
    out.outputHeight = static_cast<int>(w.output_height);
    out.temporalActive = w.temporal_active != 0;
    out.dlssRoute = w.dlss_route != 0;
    out.dlssActive = w.dlss_active != 0;
    out.dlssQuality = w.dlss_quality;
    out.jitterX = w.jitter_x;
    out.jitterY = w.jitter_y;
    out.mipBiasEffective = w.mip_bias_effective;
    out.resetThisFrame = w.reset_this_frame != 0;
    out.msaaSamples = static_cast<int>(w.msaa_samples);
    out.activeUpscaler = static_cast<int>(w.active_upscaler);
    return out;
}

// The renderer's own sentence for "why is DLSS inactive" (dlss_status.rs). Fetched
// through GetProcAddress like GetRuntimeCapabilityFlags: the export is optional at the
// PE import boundary, so an older wgpu_renderer.dll still loads and merely reports
// nothing here.
std::string EngineWgpu::DlssStatusReason()
{
    if (!_renderer)
        return {};
#ifdef _WIN32
    using GetDlssStatusFn = uint32_t (*)(WgrRenderer*, char*, size_t);
    const HMODULE module = GetModuleHandleA("wgpu_renderer.dll");
    const auto getStatus =
        module ? reinterpret_cast<GetDlssStatusFn>(GetProcAddress(module, "wgr_get_dlss_status")) : nullptr;
    if (!getStatus)
        return {};
    char buf[512];
    const uint32_t len = getStatus(_renderer, buf, sizeof(buf));
    if (len == 0)
        return {};
    buf[sizeof(buf) - 1] = 0;
    return buf;
#else
    return {};
#endif
}

// The renderer builds every multisampled pipeline against a launch-time sample count,
// so this cannot apply live. GameApplication seeds WGR_MSAA from graphics.cfg BEFORE
// the engine exists; here we only record the wish and say when it lands.
void EngineWgpu::SetMsaaSamples(int samples)
{
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)
    if (samples != 0 && samples != 2 && samples != 4 && samples != 8)
        return;
    _msaaRequested = samples;
    if (!_renderer)
        return;
    WgrTemporalInfo w{};
    wgr_get_temporal_info(_renderer, &w);
    const int active = static_cast<int>(w.msaa_samples) <= 1 ? 0 : static_cast<int>(w.msaa_samples);
    if (active != samples)
        LOG_INFO(Graphics, "Wgpu: MSAA {} requested (active {}); takes effect on the next launch",
                 samples, active);
}

// graphics.cfg renderScale: 1.0 = AUTO — leave the renderer's own default alone
// (which is DLSS Quality at 67% when the route is up, else native). Anything else is
// an explicit pin, routed through the live temporal tuning like the dev panel does.
void EngineWgpu::SetRenderScale(float scale)
{
    if (!_renderer || !_hdrEnabled)
        return;
    if (scale > 0.99f && scale < 1.01f)
        return;
    TemporalSettings t = _temporalTuning;
    t.renderScalePct = std::clamp(static_cast<int>(scale * 100.0f + 0.5f), 50, 200);
    SetTemporalTuning(t);
}

uint32_t EngineWgpu::CurrentDrawObjectId()
{
    // ID-1 (render-snapshot design §4): the identity lives ON the Object now -- one
    // field read instead of the unordered_map probe this function used to be, and a
    // recycled Object* can no longer inherit a dead object's motion history, because
    // the id dies with the object rather than with an erase call. The draw-object hint
    // is always the Object the scene's DrawObjectGuard bracketed (SceneDraw.cpp).
    //
    // The wire lane in WgrDraw3D.misc is 16 bits with 0 reserved; map the 32-bit id
    // onto 1..65535. Two LIVE objects 65535 ids apart can alias a history slot -- the
    // same wrap window the old allocator had, now bounded by object-creation volume
    // instead of map churn, and removed for good only when the ABI grows a wider lane.
    const void* obj = GetDrawObject();
    if (!obj)
    {
        return 0;
    }
    return (static_cast<const Object*>(obj)->RenderId() % 0xFFFFu) + 1u;
}

void EngineWgpu::SceneObjectRemoved(Object* obj)
{
    if (!_renderer)
    {
        return;
    }
    // (The draw-object identity used to be erased here; since ID-1 it lives on the
    // Object itself and dies with it -- nothing to clean up.)
    RemoveGpuProxies(obj); // §12d: drop the object's furniture child instances (no-op if none)
    auto it = _gpuInstances.find(obj);
    if (it == _gpuInstances.end())
    {
        return;
    }
    _ops.instanceRemove++;
    EnqueueInstanceRemove(it->second.slot);
    const LODShapeWithShadow* shape = it->second.shape;
    _gpuInstances.erase(it);
    ReleaseGpuModel(shape);
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
    if (!shape || shape != it->second.shape || obj->IsDestroyed())
    {
        _ops.instanceRemove++;
        EnqueueInstanceRemove(it->second.slot);
        const LODShapeWithShadow* residentShape = it->second.shape;
        _gpuInstances.erase(it);
        obj->SetGpuCoverage(static_cast<int>(GpuDrawCoverage::None)); // dropped to the CPU path
        RemoveGpuProxies(obj); // §12d: furniture goes with the parent (CPU draws destroyed geo)
        ReleaseGpuModel(residentShape);
        return;
    }
    // The conform plane depends on position (mode-1 land cell, mode-2 bcSurfaceY), so recompute
    // it on move. ForestPlain's cached plane is keyed to Position(); a moved forest would need
    // reinvalidation, but static clutter effectively never moves — recompute defensively.
    const ConformPlane cp = GpuConformFor(*obj, *shape, _gpuConformShapes.count(shape) != 0);
    const WgrInstance inst = BuildGpuInstance(*obj, it->second.model, cp);
    TraceGpuInstance("updated", shape->Name(), obj->Transform(), shape->BoundingSphere(), cp, it->second.model);
    _ops.instanceUpdate++;
    EnqueueInstanceUpdate(it->second.slot, inst);
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
            _ops.instanceUpdate++;
            EnqueueInstanceUpdate(child.slot, pinst);
        }
    }
}

// RFG-095b: re-emit a registered parent's GPU proxies from the CURRENT hidden set.
//
// Buildings are registered (SceneObjectCreated -> EmitGpuProxies) while the landscape adds
// them; MapThings promotes their furniture proxies to Things and hides the originals only
// afterwards (Landscape::InitObjectVehicles). The hidden check in EmitGpuProxies therefore
// saw an empty set, and every promoted chair kept a GPU twin at its authored spot: shoot the
// Thing and "a copy" stays behind, z-fighting with it until it moves (the owner's report).
void EngineWgpu::SceneObjectProxiesChanged(Object* obj)
{
    if (!_renderer || !obj)
    {
        return;
    }
    auto it = _gpuInstances.find(obj);
    if (it == _gpuInstances.end())
    {
        return;
    }
    LODShapeWithShadow* shape = obj->GetShape();
    if (!shape || shape != it->second.shape)
    {
        return;
    }
    RemoveGpuProxies(obj);
    const auto covIt = _gpuModelCoverage.find(shape);
    GpuDrawCoverage cov = covIt != _gpuModelCoverage.end() ? covIt->second : GpuDrawCoverage::Full;
    if (cov == GpuDrawCoverage::Partial)
    {
        const bool allProxiesGpu = EmitGpuProxies(obj, shape);
        if (allProxiesGpu && _gpuModelComplement.count(shape) == 0)
        {
            cov = GpuDrawCoverage::Full;
        }
    }
    it->second.coverage = cov;
    obj->SetGpuCoverage(static_cast<int>(cov));
}

// RFG-099: register a freshly loaded world model BEFORE ShapeBank optimises it, so the CPU
// vertex buffers OptimizeRendering builds find their level in _retainedMeshOfLevel and share
// the retained mesh (RFG-088) instead of uploading a private copy. Only world models arrive
// here (ShapeBank::WorldModelScope); animated shapes never take the retained path anyway.
void EngineWgpu::WorldShapeLoaded(LODShapeWithShadow* shape)
{
    if (!_gpuDriven || !_renderer || !shape || shape->GetAllowAnimation())
    {
        return;
    }
    if (!Poseidon::Dev::GResidencyLevers().shareRetainedMeshes)
    {
        return;
    }
    RegisterGpuModel(shape);
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
    //
    // Latched, not called: World::Simulate fires this MID-TICK, which is exactly the class
    // of producer-side renderer mutation the drain exists to absorb (REN-THR-006).
    if (_gpuDriven && _renderer)
    {
        _pendingSet.suppressWorld = suppress;
        _pendingSet.suppressWorldDirty = true;
    }
}

void EngineWgpu::SetCullDebugSettings(const CullDebugSettings& s)
{
    _cullDebug = s;
    _cullDebug.dumpNearby = false; // momentary button, never stored
    if (_gpuDriven && _renderer)
    {
        _pendingSet.cullDrawSpheres = s.drawSpheres;
        _pendingSet.cullDisableFrustum = s.disableFrustum;
        _pendingSet.cullOcclusion = s.occlusion;
        _pendingSet.cullDebugDirty = true;
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

    // Capture the cascade origin HERE as well as in AddShadowCaster, so it exists on frames
    // that add no CPU caster at all.
    //
    // `_smCamPos` is the origin every camera-relative shadow quantity is measured from. It
    // travels as `frame.shadow.cam_pos` and is consumed twice on the Rust side: the depth VS
    // adds it back to reconstruct absolute world xz for the terrain conform, and every shadow
    // cascade's CULL frustum is built around it (`params_from_shadow_cascade`, gfx3d/mod.rs:6435).
    // It used to be written ONLY inside AddShadowCaster, below the early-return at :4976 — so a
    // frame with no CPU casters left it holding whatever position the last frame that HAD one
    // wrote, arbitrarily far away. That was survivable only because the old NextFrame gate then
    // refused to send the shadow frame at all on exactly those frames. Now that the retained set
    // can drive the frame on its own, a stale origin would displace the whole GPU-driven cascade
    // cull and its conform — so the gate fix REQUIRES this one; neither ships alone.
    //
    // This does not weaken the warnings at the AddShadowCaster write and at the
    // `frame.shadow.cam_pos` use in NextFrame — that the origin must be the camera the CASTERS
    // were made relative to, and must NOT be re-read from `_currentCamera` at frame end, when it
    // may have advanced to a HUD/optics camera. It is the identical camera read strictly earlier
    // in the same call chain: Scene::RenderShadowMapDepthPassGpu (SceneShadowPass.cpp:500) calls
    // SetShadowCascades and then every AddShadowCaster from one body with no camera push between
    // them, and it is the very camera BuildCascadeSet fitted the cascades around — Scene::
    // GetCamera(), which is also exactly what PushSceneCamera stores in CameraEntry::pos (:1975).
    // So on any frame that has a caster this writes a bit-identical value to the one
    // AddShadowCaster then writes over it; the only new frames are the ones that had none.
    //
    // InitDraw, where the audit first proposed putting this, cannot work: it CLEARS `_cameras`
    // and `_haveCamera` (:1188-1190), so there is no current camera at that point in the frame.
    // Forcing one there would both invent the origin and change camera indexing for the frame.
    //
    // Gated on `_smCascadesValid` so EnsureCamera() cannot push a camera on a frame where the
    // shadow system is off (in practice the scene camera is long since pushed by the time the
    // pass2 shadow block runs — SceneDraw.cpp:2323 — so this is a no-op read, not a push).
    // LGT-010: ...OR when a local light wants a shadow view. Both writers of `_smCamPos` hung
    // on `_smCascadesValid`, which is false at night -- so the origin every camera-relative
    // shadow matrix is built against was STALE for exactly the hours a spot light exists for,
    // and the local views projected the world from somewhere the camera no longer was. That
    // is what made every lit surface read as occluded.
    //
    // `_localShadowCount` is last frame's here (the light list is assembled later in the
    // frame). That is deliberate and harmless: it changes when a light comes or goes, not
    // per frame, and a one-frame lag on an ORIGIN is a metre at walking speed -- against the
    // alternative of a stale origin from hours ago.
    if ((_smCascadesValid || _localShadowCount > 0) && !ShadowLegacyGateEnabled())
    {
        EnsureCamera();
        const CameraEntry& cam = _cameras[_currentCamera];
        _smCamPos[0] = cam.pos[0];
        _smCamPos[1] = cam.pos[1];
        _smCamPos[2] = cam.pos[2];
    }
}

void EngineWgpu::AddShadowCaster(const Shape& sMesh, const Matrix4& modelToWorld)
{
    // LGT-010: a local light's shadow needs casters too, and at night `_smCascadesValid` is
    // false -- so this returned before submitting a single one. The pass then rendered empty
    // local layers, which a LessEqual compare against a 1.0 clear reads as "lit everywhere":
    // the feature could not have worked at night whatever the shader did.
    if (!_renderer || (!_smCascadesValid && _localShadowCount <= 0))
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
    //
    // Kept as-is now that SetShadowCascades also writes it. This is the AUTHORITATIVE write —
    // it is made from the same `cam` the caster world matrices on the next lines are offset by,
    // so it cannot drift from them no matter what a future caller does between the two calls.
    // SetShadowCascades' write is the fallback that covers the zero-caster frame; on a frame
    // with casters it writes the same value and this overwrites it with itself.
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
    if (buf->skinned && !buf->cpuPosed && buf->paletteCount > 0) // FACE-001
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
            // REN-THR-011 -- and this is the call site that decided the design. SceneShadowPass
            // calls ConvertToVBuffer and then AddShadowCaster on consecutive lines (:574, :600),
            // so a caster naming a mesh whose create has NOT been drained is the ordinary case
            // for every newly visible shadowed object, not a corner. Same index-and-patch as
            // the draw records.
            if (run.mesh == 0 && buf->slot)
            {
                _castersPendingMesh.push_back(PendingMeshRef{U32(_shadowCasters.size() - 1), buf->slot});
            }
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
        run.mesh = buf->RendererMesh();
        run.index_begin = U32(si.beg);
        run.index_count = U32(si.end - si.beg);
        std::memcpy(run.world.m, &world, sizeof(run.world.m));
        run.texture_id = tex;
        run.palette_slot = paletteSlot;
        run.alpha_ref = alphaRef;
        run.sampler = 0;
        // LGT-010: bits 0..3 are the sun's cascades; bit (cascadeCount + k) is local light
        // k. A caster is masked into a light's view only when it is within that light's
        // reach -- without this every object in the world would be transformed into every
        // light view, which is exactly the cost that makes people say shadowed local lights
        // are impossible.
        // The caster's transform here is CAMERA-RELATIVE (see the note on shadow.cam_pos);
        // the lights are absolute. Compare in one space or the mask is silently always empty.
        run.cascade_mask = 0xF | LocalShadowCasterMask(run.world.m[12], run.world.m[13], run.world.m[14]);
        run.conform0 = conform0;
        run.conform2 = conform2;
        haveRun = true;
    }
    flush();
}

bool EngineWgpu::DumpShadowMap(const char* path)
{
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)
    if (!_renderer || !path)
    {
        return false;
    }
    // The TARGET's resolution, not the tuning's. shadow_map_read refuses when the buffer is
    // smaller than the layer, and those two numbers are allowed to differ -- which made every
    // dump return "FAILED" with nothing to say why.
    const int res = std::max(_smCascadeRes, _smTuning.resolution);
    if (res <= 0)
    {
        return false;
    }
    std::vector<float> depth(static_cast<size_t>(res) * res, 1.0f);
    // LGT-010: POSEIDON_SHADOW_DUMP_LAYER picks which layer of the array is dumped. 0..3 are
    // the sun's cascades; `_smCascades.count + k` is local light k. Reading the LIGHT's own
    // depth map is the only way to tell "the shadow term is wrong" from "nothing was ever
    // rendered into the map it samples", and those two look identical on screen.
    static const uint32_t dumpLayer = []
    {
        const char* v = std::getenv("POSEIDON_SHADOW_DUMP_LAYER");
        return v ? static_cast<uint32_t>(std::max(0, std::atoi(v))) : 0u;
    }();
    const uint32_t got = wgr_shadow_map_read(_renderer, dumpLayer, depth.data(), U32(depth.size()));
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
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)
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
    _ops.textureOps++;
    OverlayTexOp op;
    op.kind = OverlayTexOp::Create;
    op.handle = _nextOverlayTexHandle++;
    op.w = w;
    op.h = h;
    op.rgba.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
    const uint64_t handle = op.handle;
    _overlayTexOps.push_back(std::move(op));
    return handle;
}

void EngineWgpu::OverlayTextureUpdate(uint64_t texture, int w, int h, const uint8_t* rgba)
{
    if (!_renderer || w <= 0 || h <= 0 || !rgba)
    {
        return;
    }
    _ops.textureOps++;
    OverlayTexOp op;
    op.kind = OverlayTexOp::Update;
    op.handle = texture;
    op.w = w;
    op.h = h;
    op.rgba.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
    _overlayTexOps.push_back(std::move(op));
}

void EngineWgpu::OverlayTextureDestroy(uint64_t texture)
{
    if (_renderer)
    {
        _ops.textureOps++;
        OverlayTexOp op;
        op.kind = OverlayTexOp::Destroy;
        op.handle = texture;
        _overlayTexOps.push_back(std::move(op));
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
        // REN-THR-013: the PRODUCER handle is stored as-is and translated at the drain,
        // not here. SubmitOverlay runs on the producer during frame build; the drain runs
        // where the renderer handle is owned, and once that is another thread the map this
        // used to read is not the producer's to read.
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

// The C++ half of the interior-sky measurement (Engine::ProbeInteriorSkyMap).
//
// The renderer's own line says how much of the depth map holds an occluder. On its own that
// number cannot tell "the box is empty" from "the cull kept nothing" from "the draw rasterised
// nothing", because all three print 0.00%. THIS half answers the first of the three from the
// producer side, where the answer is a plain loop over the retained table: how many instances
// the engine put on the GPU path inside the same box, and what they are. A house that reads as
// zero here was never handed to the renderer at all, and no amount of shader work will find it.
void EngineWgpu::ProbeInteriorSkyMap()
{
    ++_interiorSkyProbe;
    PushRenderParams(); // carries the new counter; the renderer logs on its next frame
    LogAdmissionCensus();

    const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
    if (!camera)
    {
        LOG_INFO(Graphics, "Interior sky probe: no scene camera; renderer half will still log");
        return;
    }
    const Vector3 camPos = camera->Position();
    // The SAME box the map covers, so the two halves of the answer describe one volume. `extent`
    // is a HALF-extent and the box is axis-aligned, which is why this is a box test and not a
    // sphere: a sphere of radius `extent` would count corners the map never sees.
    const float extent = _interiorSky.extent;
    int inBox = 0;
    int within12 = 0;
    // The three nearest, by square distance, so the line names what is actually overhead rather
    // than whatever the hash order happened to reach first.
    const char* nearName[3] = {nullptr, nullptr, nullptr};
    float nearD2[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    for (const auto& [obj, inst] : _gpuInstances)
    {
        const Vector3 d = inst.pos - camPos;
        if (std::fabs(d.X()) > extent || std::fabs(d.Z()) > extent)
            continue;
        ++inBox;
        const float d2 = d.SquareSize();
        if (d2 < 144.0f)
            ++within12;
        const char* name = inst.shape ? inst.shape->Name() : "?";
        for (int i = 0; i < 3; ++i)
        {
            if (d2 < nearD2[i])
            {
                for (int j = 2; j > i; --j)
                {
                    nearD2[j] = nearD2[j - 1];
                    nearName[j] = nearName[j - 1];
                }
                nearD2[i] = d2;
                nearName[i] = name;
                break;
            }
        }
    }
    LOG_INFO(Graphics,
             "Interior sky probe at [{:.0f} {:.0f} {:.1f}]: retained instances total={} in the "
             "{:.0f} m box={} within 12 m={}; nearest: {} ({:.1f} m), {} ({:.1f} m), {} ({:.1f} m)",
             camPos.X(), camPos.Z(), camPos.Y(), _gpuInstances.size(), extent, inBox, within12,
             nearName[0] ? nearName[0] : "-", nearD2[0] < FLT_MAX ? std::sqrt(nearD2[0]) : -1.0f,
             nearName[1] ? nearName[1] : "-", nearD2[1] < FLT_MAX ? std::sqrt(nearD2[1]) : -1.0f,
             nearName[2] ? nearName[2] : "-", nearD2[2] < FLT_MAX ? std::sqrt(nearD2[2]) : -1.0f);
    if (inBox == 0)
    {
        LOG_WARN(Graphics,
                 "Interior sky probe: NOTHING retained inside the box. The map is correct and "
                 "empty -- the building around the camera never reached the GPU-driven path, so "
                 "neither the per-frame maps nor the Stage 2 bake can see it. Chase admission "
                 "(SceneObjectCreated / RegisterGpuModel), not the shader.");
    }
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
    AcquireProducerWindow("exposure scale");
    return _renderer ? wgr_get_exposure_scale(_renderer) : 1.0f;
}

int EngineWgpu::GetWaterGpuTimings(float* outMs, int maxCount) const
{
    if (!_renderer || !outMs || maxCount <= 0)
        return 0;
    // PERF-022: the worker's snapshot (PublishRendererStats), never the live handle.
    std::lock_guard<std::mutex> lock(_memStatsMutex);
    const int n = std::min(maxCount, std::min(_publishedGpuTimingCount, (int)WGR_GPU_TIMER_REGION_COUNT));
    std::memcpy(outMs, _publishedGpuTimings, sizeof(float) * (size_t)n);
    return n;
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
    // Shared with legacy terrain: identical mounted-PBO lookup and albedo-only rules.
    Ref<Texture> tex = _wbank->LoadLegacyEnhancementNormal(textureName);
    if (auto* wgpuTex = static_cast<TextureWgpu*>(tex.GetRef()))
    {
        handle = wgpuTex->EnsureUploaded();
        if (handle != 0)
            LOG_INFO(Graphics, "Wgpu legacy material: {} enhanced by {}", key, (const char*)tex->Name());
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
        "Terrain: depth+normal prepass",   // WGR_GPU_TIMER_TERRAIN_PREPASS
        "Terrain: main colour",            // WGR_GPU_TIMER_TERRAIN_COLOR
        "Objects: cull (main)",            // WGR_GPU_TIMER_OBJ_CULL_MAIN
        "Objects: cull (shadow cascades)", // WGR_GPU_TIMER_OBJ_CULL_SHADOW
        "Objects: Hi-Z + occlusion cull",  // WGR_GPU_TIMER_OBJ_CULL_COLOR
        "Objects: prepass solid",          // WGR_GPU_TIMER_OBJ_PREPASS_SOLID
        "Objects: prepass alpha-test",     // WGR_GPU_TIMER_OBJ_PREPASS_ALPHA
        "Objects: colour solid",           // WGR_GPU_TIMER_OBJ_COLOR_SOLID
        "Objects: colour alpha-test",      // WGR_GPU_TIMER_OBJ_COLOR_ALPHA
        "[container] Prepass pass total",  // WGR_GPU_TIMER_OBJ_PREPASS_SEGMENT (overlaps leaves)
        "[container] 3D colour sub-pass",  // WGR_GPU_TIMER_OBJ_COLOR_SEGMENT (overlaps leaves)
        "Shadow cascade 0",                // WGR_GPU_TIMER_SHADOW_CASCADE_0
        "Shadow cascade 1",                // WGR_GPU_TIMER_SHADOW_CASCADE_1
        "Shadow cascade 2",                // WGR_GPU_TIMER_SHADOW_CASCADE_2
        "Shadow cascade 3",                // WGR_GPU_TIMER_SHADOW_CASCADE_3
        "Far tier: cull (compute)",        // WGR_GPU_TIMER_FAR_CULL (sweeps every placement)
        "Far tier: proxy draw",            // WGR_GPU_TIMER_FAR_DRAW (needs in-pass timestamps)
        "Sky: cloud march",                // WGR_GPU_TIMER_CLOUD_MARCH
        "Sky: cloud composite",            // WGR_GPU_TIMER_CLOUD_COMPOSITE
        "[container] ops pre-water",       // WGR_GPU_TIMER_OPS_PRE_WATER (incl. terrain + grass)
        "[container] ops post-water",      // WGR_GPU_TIMER_OPS_POST_WATER (transparents)
        "Post: HDR MSAA resolve",          // WGR_GPU_TIMER_HDR_RESOLVE
        "Post: god rays",                  // WGR_GPU_TIMER_GOD_RAYS
        "Post: bloom",                     // WGR_GPU_TIMER_BLOOM
        "Post: exposure adapt",            // WGR_GPU_TIMER_EXPOSURE_ADAPT
        "Post: tonemap",                   // WGR_GPU_TIMER_TONEMAP_RESOLVE
        "UI / 2D",                         // WGR_GPU_TIMER_UI_2D
        "Sky: LUTs",                       // WGR_GPU_TIMER_SKY_LUTS
        "Sky: aerial froxel",              // WGR_GPU_TIMER_SKY_FROXEL
        "Sky: env map",                    // WGR_GPU_TIMER_SKY_ENV
        "Sky: SH irradiance",              // WGR_GPU_TIMER_SKY_SH
        "Sky: draw",                       // WGR_GPU_TIMER_SKY_DRAW
        "Worker: swapchain acquire",       // WGR_GPU_TIMER_WORKER_ACQUIRE (CPU column only)
        "Worker: present",                 // WGR_GPU_TIMER_WORKER_PRESENT (CPU column only)
        "Worker: frame setup",             // WGR_GPU_TIMER_WORKER_SETUP (CPU column only)
        "Worker: queue submit",            // WGR_GPU_TIMER_WORKER_SUBMIT (CPU column only)
        "Worker: readback harvest",        // WGR_GPU_TIMER_WORKER_HARVEST (CPU column only)
        "GI: probe update",                // WGR_GPU_TIMER_GI_PROBES (REN-GI-001)
        "GI: sun proxy",                   // WGR_GPU_TIMER_GI_RSM (REN-GI-002)
        "Worker enc: early",               // WGR_GPU_TIMER_WORKER_ENC_EARLY (CPU column only)
        "Worker enc: shadows",             // WGR_GPU_TIMER_WORKER_ENC_SHADOW (CPU column only)
        "Worker enc: main",                // WGR_GPU_TIMER_WORKER_ENC_MAIN (CPU column only)
        "Worker enc: tail",                // WGR_GPU_TIMER_WORKER_ENC_TAIL (CPU column only)
        "Worker enc: sky",                 // WGR_GPU_TIMER_WORKER_ENC_SKY (CPU column only)
        "Worker enc: draw",                // WGR_GPU_TIMER_WORKER_ENC_DRAW (CPU column only)
        "Worker enc: post",                // WGR_GPU_TIMER_WORKER_ENC_POST (CPU column only)
        "Shadows: local lights",           // WGR_GPU_TIMER_LOCAL_SHADOW (LGT-026)
    };
    return (region >= 0 && region < (int)WGR_GPU_TIMER_REGION_COUNT) ? kNames[region] : "";
}

// PERF-005 — the region-nesting rule exists in TWO headers: WgrGpuTimerContainedBy (the C ABI,
// for consumers of wgr_get_gpu_timings that have no Engine.hpp) and Engine::GpuTimerContainedBy
// (backend-agnostic, used by the capture-metrics JSON writer). If they ever disagree, one
// reporter double-counts and the other does not, and the resulting "totals" look like
// measurements rather than the bug they are. This is the one place that includes both headers,
// so the agreement is checked here — at COMPILE time, for every region, so the tables cannot
// drift silently.
namespace
{
constexpr bool NestingTablesAgree()
{
    for (int r = 0; r < Engine::kGpuRegionEnd; ++r)
    {
        const uint32_t abi = WgrGpuTimerContainedBy(static_cast<uint32_t>(r));
        const int engine = Engine::GpuTimerContainedBy(r);
        const bool abiOuter = (abi == WGR_GPU_TIMER_NONE);
        const bool engineOuter = (engine == Engine::kNoGpuRegion);
        if (abiOuter != engineOuter)
            return false;
        if (!abiOuter && static_cast<int>(abi) != engine)
            return false;
    }
    return true;
}
static_assert(NestingTablesAgree(),
              "WgrGpuTimerContainedBy (wgpu_renderer.hpp) and Engine::GpuTimerContainedBy "
              "(Engine.hpp) disagree about GPU timer region nesting; a reporter would double-count");
} // namespace

// PERF-005 — the periodic object-cost row. Deliberately NOT in WaterWgpu.cpp beside the other
// GPU-timing rows: those sit inside the water CDLOD selection and are gated on the water
// pipeline having run, so on a dry world (and on a paused/frozen sea) they never print. Object
// cost is exactly what you want measured on worlds with no water at all, so this lives on the
// unconditional frame path with its own budget.
//
// Rate: one burst every 2 s, matching the water rows. At 50+ FPS an unlimited row produced ~25
// lines/second in the equivalent water case, which floods the log the capture is read from.
namespace
{
// FNV-1a over raw bytes. Chosen because the property this needs is "two runs that published
// different frames must disagree", not cryptographic strength -- a collision here costs a
// missed difference in a development gate, not a security property.
inline uint64_t HashBytes(uint64_t h, const void* p, size_t n)
{
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

template <typename T>
inline uint64_t HashSlice(uint64_t h, const WgrSlice<T>& s)
{
    // Length first, so an empty slice and a missing one are distinguishable and so two
    // adjacent slices cannot be confused for one longer one.
    const uint32_t n = s.length;
    h = HashBytes(h, &n, sizeof(n));
    if (s.data && n)
        h = HashBytes(h, s.data, static_cast<size_t>(n) * sizeof(T));
    return h;
}
} // namespace

// The frame as the renderer will see it, hashed at the moment of publication. See the
// declaration for why this exists; the short version is that every remaining Phase 5 step
// restructures the producer, and "it still publishes the same frame" is the only claim that
// makes such a restructuring provable rather than plausible.
void EngineWgpu::HashPublishedFrame(const WgrFrame& frame, const PushFrameBacking& backing)
{
    if (!_frameHashOn)
        return;

    // Per SLICE, not just the fold. A single number says two runs differ; it cannot say
    // which subsystem they differ in, and the slice that parts first names the suspect
    // directly -- draws3d is the LOD governor's output, terrain_nodes the terrain's,
    // grass_batches the grass placement's. The fold is built from the same values so the
    // whole-frame figure is unchanged.
    constexpr uint64_t kBasis = 14695981039346656037ull;
    uint64_t sl[kFrameHashSlices];
    sl[0] = HashBytes(HashBytes(kBasis, &frame.clear, sizeof(frame.clear)), &frame.fog_color,
                      sizeof(frame.fog_color));
    sl[1] = HashSlice(kBasis, frame.cameras);
    // SIM-816: the draw records carry mesh and texture HANDLES, and handle values ride
    // asset-load completion order -- two bit-identical simulations legitimately label the
    // same content differently. WGR_FRAME_HASH_MASK_IDS=1 hashes draws3d with those three
    // fields zeroed, so "same frames" means "same content". Everything else in the record
    // (transforms, materials, blend state, flags) still counts.
    static const bool maskIds = []
    {
        const char* v = std::getenv("WGR_FRAME_HASH_MASK_IDS");
        return v && v[0] && v[0] != '0';
    }();
    if (maskIds)
    {
        uint64_t h2 = kBasis;
        const uint32_t n = frame.draws3d.length;
        h2 = HashBytes(h2, &n, sizeof(n));
        for (uint32_t i = 0; i < n; i++)
        {
            WgrDraw3D d = frame.draws3d.data[i];
            d.mesh = 0;
            d.texture_id = 0;
            d.normal_texture_id = 0;
            // An index into the palette, whose order is an artefact -- see sl[6].
            d.palette_slot = 0;
            h2 = HashBytes(h2, &d, sizeof(d));
        }
        sl[2] = h2;
    }
    else
    {
        sl[2] = HashSlice(kBasis, frame.draws3d);
    }
    sl[3] = HashSlice(kBasis, frame.verts);
    sl[4] = HashSlice(kBasis, frame.batches);
    sl[5] = HashSlice(kBasis, frame.cmds);
    // The palette is an ARRAY WHOSE ORDER IS AN ARTEFACT. Slots are handed out in draw
    // order (`_currentPaletteSlot = _palette.size() / WGR_PALETTE_SIZE`), and draw order
    // follows the order objects were inserted into the scene -- which rides asynchronous
    // load completion. Two runs with a bit-identical simulation can therefore publish the
    // same bone matrices under permuted slot numbers, and comparing the array positionally
    // reports every frame as different. Measured: the simulation identical across all 902
    // ticks while `pal` and `shad` differed in all 901 scene frames.
    //
    // Under masking the palette is hashed as a MULTISET -- per-entry hashes, sorted -- so a
    // permutation compares equal and a changed matrix still does not. Same rule as the
    // handles: hash what is drawn, not the order it happened to be recorded in.
    if (maskIds)
    {
        const uint32_t np = frame.palette.length;
        std::vector<uint64_t> entries(np);
        for (uint32_t i = 0; i < np; i++)
            entries[i] = HashBytes(kBasis, &frame.palette.data[i], sizeof(WgrMat4));
        std::sort(entries.begin(), entries.end());
        uint64_t h6 = HashBytes(kBasis, &np, sizeof(np));
        for (const uint64_t e : entries)
            h6 = HashBytes(h6, &e, sizeof(e));
        sl[6] = h6;
    }
    else
    {
        sl[6] = HashSlice(kBasis, frame.palette);
    }
    // shadow_casters carries a mesh AND a texture handle too, and masking draws3d while
    // leaving these unmasked made the oracle report a phantom regression: under machine load
    // (an agent compiling) the two runs assigned handles in a different order, sl[7] moved in
    // every frame, and the gate failed for a change that had touched neither the renderer nor
    // the simulation -- which was bit-identical in the same pair. Same rule as draws3d: hash
    // what is drawn, not what it is labelled.
    if (maskIds)
    {
        // A MULTISET, like the palette. The caster array is in draw order, and draw order
        // follows scene insertion order, which rides asynchronous load completion. Order
        // carries no meaning here: every caster renders into a depth-only map, so permuting
        // them cannot change a pixel. Masking the handles alone was not enough -- measured,
        // `shad` still differed in two attempts of three with the simulation identical.
        const uint32_t n7 = frame.shadow_casters.length;
        std::vector<uint64_t> casters(n7);
        for (uint32_t i = 0; i < n7; i++)
        {
            WgrShadowCaster c = frame.shadow_casters.data[i];
            c.mesh = 0;
            c.texture_id = 0;
            c.palette_slot = 0;
            // Index range into the shared geometry pool: allocated at mesh-create time, so
            // it is a label the load order assigns, exactly like the mesh handle itself.
            c.index_begin = 0;
            casters[i] = HashBytes(kBasis, &c, sizeof(c));
        }
        std::sort(casters.begin(), casters.end());
        uint64_t h7 = HashBytes(kBasis, &n7, sizeof(n7));
        for (const uint64_t e : casters)
            h7 = HashBytes(h7, &e, sizeof(e));
        sl[7] = h7;
    }
    else
    {
        sl[7] = HashSlice(kBasis, frame.shadow_casters);
    }
    sl[8] = HashSlice(kBasis, frame.overlay_verts);
    sl[9] = HashSlice(kBasis, frame.overlay_indices);
    sl[10] = HashSlice(kBasis, frame.overlay_draws);
    sl[11] = HashSlice(kBasis, frame.terrain_nodes);
    sl[12] = HashSlice(kBasis, frame.terrain_batches);
    sl[13] = HashSlice(kBasis, frame.lights);
    sl[14] = HashSlice(kBasis, frame.water_nodes);
    sl[15] = HashSlice(kBasis, frame.water_batches);
    sl[16] = HashSlice(kBasis, frame.grass_batches);

    uint64_t h = kBasis;
    for (uint64_t v : sl)
        h = HashBytes(h, &v, sizeof(v));

    // Frames that differ from their predecessor. If this stays at zero while the camera
    // moves, the hash is not reading the frame and every "equivalent" verdict it gives is
    // vacuous -- the same trap SIM-808 records for the simulation timeline.
    // The first frames individually, by index. A whole-run fold cannot be compared between
    // two runs at all -- they do not even publish the same NUMBER of frames, because the run
    // is ended by a wall clock and the simulation takes a variable number of fixed steps per
    // frame. Per-index hashes over the opening frames are the part that IS comparable, and
    // how far down this list two runs agree is the measurement of how far producer
    // determinism actually extends.
    // A WINDOW, not a prefix. The opening frames are the mission loading, where the same
    // frame is republished a variable number of times while I/O completes -- comparing two
    // runs there compares how long the disk took, not what the producer built. The window
    // start is settable so the comparison can be moved past whatever the scene's load costs.
    if (_frameHashCount >= _frameHashFirst && _frameHashCount < _frameHashFirst + _frameHashWindow)
    {
        // REN-THR-004 asks two questions of the parting frame that a hash cannot answer.
        // LENGTHS: same length either side means the bytes changed (a colour, an alpha, a
        // transform); a different length means geometry appeared or vanished, and those are
        // different bugs. STEPS: how many fixed simulation ticks this frame ran -- if the
        // parting frame is where two runs first take a different number, the one-frame lead
        // of the 2D path over the world is explained and needs no further candidate.
        LOG_INFO(Graphics, "Wgpu frame len[{}]: tick={} steps={} vert={} bat={} cmd={} d3d={} tn={} grass={}",
                 _frameHashCount, backing.totalTicks, backing.steps,
                 frame.verts.length, frame.batches.length,
                 frame.cmds.length, frame.draws3d.length, frame.terrain_nodes.length,
                 frame.grass_batches.length);
        LOG_INFO(Graphics,
                 "Wgpu frame hash: frame[{}]={:#018x} | scal={:08x} cam={:08x} d3d={:08x} vert={:08x} "
                 "bat={:08x} cmd={:08x} pal={:08x} shad={:08x} ovv={:08x} ovi={:08x} ovd={:08x} "
                 "tn={:08x} tb={:08x} lit={:08x} wn={:08x} wb={:08x} grass={:08x}",
                 _frameHashCount, h, static_cast<uint32_t>(sl[0]), static_cast<uint32_t>(sl[1]),
                 static_cast<uint32_t>(sl[2]), static_cast<uint32_t>(sl[3]), static_cast<uint32_t>(sl[4]),
                 static_cast<uint32_t>(sl[5]), static_cast<uint32_t>(sl[6]), static_cast<uint32_t>(sl[7]),
                 static_cast<uint32_t>(sl[8]), static_cast<uint32_t>(sl[9]), static_cast<uint32_t>(sl[10]),
                 static_cast<uint32_t>(sl[11]), static_cast<uint32_t>(sl[12]), static_cast<uint32_t>(sl[13]),
                 static_cast<uint32_t>(sl[14]), static_cast<uint32_t>(sl[15]), static_cast<uint32_t>(sl[16]));
    }

    if (_frameHashCount != 0 && h != _frameHashLast)
        _frameHashDistinct++;
    _frameHashLast = h;
    _frameHashRun = HashBytes(_frameHashRun ? _frameHashRun : 14695981039346656037ull, &h, sizeof(h));
    _frameHashCount++;
}

void EngineWgpu::LogObjectPerfRow()
{
    static std::chrono::steady_clock::time_point lastLog{};
    const auto now = std::chrono::steady_clock::now();
    if ((now - lastLog) < std::chrono::seconds(2))
        return;
    lastLog = now;

    LogVbUpdateStats();

    float gpu[kGpuRegionEnd] = {};
    const int n = GetWaterGpuTimings(gpu, kGpuRegionEnd);
    if (n < kGpuRegionEnd)
        return;
    float cpu[kGpuRegionEnd] = {};
    const int cpuN = GetCpuTimings(cpu, kGpuRegionEnd);
    // -1 means "did not run this frame" and must never be summed as if it were 0 ms of work —
    // that is how a skipped pass silently becomes part of an "accounted for" total.
    const auto pos = [](float v) { return v > 0.0f ? v : 0.0f; };
    const auto cpuOf = [&](int r) { return (r < cpuN) ? cpu[r] : -1.0f; };

    const float cullTotal = pos(gpu[WGR_GPU_TIMER_OBJ_CULL_MAIN]) + pos(gpu[WGR_GPU_TIMER_OBJ_CULL_SHADOW]) +
                            pos(gpu[WGR_GPU_TIMER_OBJ_CULL_COLOR]);
    float cascades = 0.0f;
    for (int c = kShadowCascadeGpuRegionBegin; c < kShadowCascadeGpuRegionEnd; ++c)
        cascades += pos(gpu[c]);
    // Frame accounting: sum ONLY the outermost regions (WgrGpuTimerContainedBy == NONE). Adding
    // every region instead double-counts grass-shadow inside the cascades and the whole colour
    // sub-pass inside its own leaves, which on Everon's 00training produced a "total" 1.5 ms
    // larger than the frame it was supposed to explain.
    float outerSum = 0.0f;
    for (int r = 0; r < kGpuRegionEnd; ++r)
    {
        if (r == kFrameGpuRegionTotal || GpuTimerContainedBy(r) != kNoGpuRegion)
            continue;
        outerSum += pos(gpu[r]);
    }
    const float frame = pos(gpu[WGR_GPU_TIMER_FRAME_TOTAL]);
    LOG_INFO(Graphics, "Frame accounting: attributed={:.3f} of frame={:.3f} ms | residual={:.3f} ms ({:.1f}%)",
             outerSum, frame, frame - outerSum, frame > 0.0f ? (frame - outerSum) / frame * 100.0f : 0.0f);

    LOG_INFO(Graphics,
             "Objects GPU ms: cull_main={:.3f} cull_shadow={:.3f} cull_hiz={:.3f} cull_total={:.3f} | "
             "prepass_solid={:.3f} prepass_alpha={:.3f} colour_solid={:.3f} colour_alpha={:.3f}",
             gpu[WGR_GPU_TIMER_OBJ_CULL_MAIN], gpu[WGR_GPU_TIMER_OBJ_CULL_SHADOW],
             gpu[WGR_GPU_TIMER_OBJ_CULL_COLOR], cullTotal, gpu[WGR_GPU_TIMER_OBJ_PREPASS_SOLID],
             gpu[WGR_GPU_TIMER_OBJ_PREPASS_ALPHA], gpu[WGR_GPU_TIMER_OBJ_COLOR_SOLID],
             gpu[WGR_GPU_TIMER_OBJ_COLOR_ALPHA]);
    LOG_INFO(Graphics,
             "Shadow GPU ms: c0={:.3f} c1={:.3f} c2={:.3f} c3={:.3f} cascades_total={:.3f} | frame={:.3f}",
             gpu[WGR_GPU_TIMER_SHADOW_CASCADE_0], gpu[WGR_GPU_TIMER_SHADOW_CASCADE_1],
             gpu[WGR_GPU_TIMER_SHADOW_CASCADE_2], gpu[WGR_GPU_TIMER_SHADOW_CASCADE_3], cascades,
             gpu[WGR_GPU_TIMER_FRAME_TOTAL]);
    // Containers overlap their leaves; printed with the leaf sum beside them so the difference
    // (the CPU-replayed Draw3D ops, which cannot be timed individually) is readable directly.
    const float prepassLeaves = pos(gpu[WGR_GPU_TIMER_OBJ_PREPASS_SOLID]) +
                                pos(gpu[WGR_GPU_TIMER_OBJ_PREPASS_ALPHA]) +
                                pos(gpu[WGR_GPU_TIMER_TERRAIN_PREPASS]) + pos(gpu[WGR_GPU_TIMER_GRASS_PREPASS]);
    const float colourLeaves = pos(gpu[WGR_GPU_TIMER_OBJ_COLOR_SOLID]) + pos(gpu[WGR_GPU_TIMER_OBJ_COLOR_ALPHA]) +
                               pos(gpu[WGR_GPU_TIMER_TERRAIN_COLOR]) + pos(gpu[WGR_GPU_TIMER_GRASS_COLOR]);
    LOG_INFO(Graphics,
             "Segment GPU ms: prepass_pass={:.3f} (leaves={:.3f} residual={:.3f}) | "
             "colour_pass={:.3f} (leaves={:.3f} residual={:.3f})",
             gpu[WGR_GPU_TIMER_OBJ_PREPASS_SEGMENT], prepassLeaves,
             pos(gpu[WGR_GPU_TIMER_OBJ_PREPASS_SEGMENT]) - prepassLeaves,
             gpu[WGR_GPU_TIMER_OBJ_COLOR_SEGMENT], colourLeaves,
             pos(gpu[WGR_GPU_TIMER_OBJ_COLOR_SEGMENT]) - colourLeaves);
    LOG_INFO(Graphics,
             "Objects CPU ms (encode): cull_main={:.3f} cull_shadow={:.3f} cull_hiz={:.3f} "
             "prepass_pass={:.3f} colour_pass={:.3f} cascades={:.3f}/{:.3f}/{:.3f}/{:.3f}",
             cpuOf(WGR_GPU_TIMER_OBJ_CULL_MAIN), cpuOf(WGR_GPU_TIMER_OBJ_CULL_SHADOW),
             cpuOf(WGR_GPU_TIMER_OBJ_CULL_COLOR), cpuOf(WGR_GPU_TIMER_OBJ_PREPASS_SEGMENT),
             cpuOf(WGR_GPU_TIMER_OBJ_COLOR_SEGMENT), cpuOf(WGR_GPU_TIMER_SHADOW_CASCADE_0),
             cpuOf(WGR_GPU_TIMER_SHADOW_CASCADE_1), cpuOf(WGR_GPU_TIMER_SHADOW_CASCADE_2),
             cpuOf(WGR_GPU_TIMER_SHADOW_CASCADE_3));

    // REN-THR-001: how much renderer state the producer mutates outside NextFrame, which is
    // the traffic a command queue would have to carry. Mean AND peak, with the frame count
    // that is their denominator -- streaming is bursty, and a queue is sized by its worst
    // frame. Mesh destroys from VertexBufferWgpu's destructor are NOT in these numbers.
    if (_frameHashOn && _frameHashCount > 0)
    {
        LOG_INFO(Graphics,
                 "Wgpu frame hash: run={:#018x} last={:#018x} frames={} changed={} -- 'changed' is "
                 "how many differed from their predecessor; zero while the view moves means the "
                 "hash is not reading the frame and any equivalence verdict from it is vacuous",
                 _frameHashRun, _frameHashLast, _frameHashCount, _frameHashDistinct);
    }
    if (_opsFrames > 0)
    {
        const auto mean = [&](uint32_t total) { return static_cast<double>(total) / _opsFrames; };
        LOG_INFO(Graphics,
         "Geometry pool fill (cumulative): retained models {} meshes {:.0f} MB (largest {:.1f} MB) | direct-path "
                 "buffers {} meshes {:.0f} MB (largest {:.1f} MB) | shared with a retained mesh {} buffers, {:.0f} MB "
                 "not spent",
                 GGeometryPoolFill.count[0].load(), GGeometryPoolFill.bytes[0].load() / 1048576.0,
                 GGeometryPoolFill.largest[0].load() / 1048576.0, GGeometryPoolFill.count[1].load(),
                 GGeometryPoolFill.bytes[1].load() / 1048576.0, GGeometryPoolFill.largest[1].load() / 1048576.0,
                 GGeometryPoolFill.count[2].load(), GGeometryPoolFill.bytes[2].load() / 1048576.0);
        LOG_INFO(Graphics,
                 "Renderer mutation ops over {} frames -- mean/peak: inst add={:.2f}/{} update={:.2f}/{} "
                 "remove={:.2f}/{} | mesh create={:.2f}/{} destroy={:.2f}/{} | model reg={:.2f}/{} | "
                 "texture={:.2f}/{}",
                 _opsFrames, mean(_opsTotal.instanceAdd), _opsPeak.instanceAdd, mean(_opsTotal.instanceUpdate),
                 _opsPeak.instanceUpdate, mean(_opsTotal.instanceRemove), _opsPeak.instanceRemove,
                 mean(_opsTotal.meshCreate), _opsPeak.meshCreate, mean(_opsTotal.meshDestroy), _opsPeak.meshDestroy,
                 mean(_opsTotal.modelRegister), _opsPeak.modelRegister, mean(_opsTotal.textureOps),
                 _opsPeak.textureOps);
        _opsTotal = MutationOps{};
        _opsPeak = MutationOps{};
        _opsFrames = 0;
    }

    ObjectStatsOut o;
    if (!GetObjectStats(o))
        return;
    if (!o.valid)
    {
        LOG_INFO(Graphics, "Objects counts: readback pending (registered={})", o.registeredInstances);
        return;
    }
    // RFG-097: the retained admission census beside the counts, so a stream that admits
    // objects after the first frame says what it refused and why, while the run is alive.
    LogAdmissionCensus();
    LOG_INFO(Graphics,
             "Objects counts: registered={} | main inst={} sections={} tris={} | "
             "colour inst={} sections={} (solid={} alpha={}) tris={} (solid={} alpha={}) | "
             "direct calls={} indirect={} tris={}",
             o.registeredInstances, o.mainInstances, o.mainDraws, o.mainTris, o.colorInstances, o.colorDraws,
             o.colorDrawsSolid, o.colorDrawsAlpha, o.colorTris, o.colorTrisSolid, o.colorTrisAlpha, o.directCalls,
             o.directIndirectCalls, o.directTris);
    if (o.staleInstanceOps != 0)
    {
        // ID-3's counter, now readable. It only ever logged its first eight occurrences, so
        // until this line a sustained identity bug went quiet after the eighth. Nonzero is a
        // handle outliving its instance on THIS side of the FFI.
        LOG_ERROR(Graphics,
                  "ANOMALY stale instance handles refused since startup: {} -- a C++ handle "
                  "outlived its instance; the renderer refused the write rather than "
                  "corrupting whatever now occupies the slot",
                  o.staleInstanceOps);
    }
    LOG_INFO(Graphics, "Objects shadow counts: c0={}/{} c1={}/{} c2={}/{} c3={}/{} (instances/tris)",
             o.shadowInstances[0], o.shadowTris[0], o.shadowInstances[1], o.shadowTris[1], o.shadowInstances[2],
             o.shadowTris[2], o.shadowInstances[3], o.shadowTris[3]);
    // Texture coverage over every section ever baked, with its denominator. The UNTEXTURED
    // warnings above are deduplicated by model|rvmat and therefore cannot be counted; this row
    // can. Triangles are reported alongside sections because a section is not a unit of area --
    // 21 untextured container-interior sections and 21 untextured building walls read the same
    // in a section count and nothing alike on screen.
    LOG_INFO(Graphics,
             "Objects material coverage: baked sections={} untextured={} ({:.3f}%) | tris={} untextured={} ({:.3f}%)",
             g_bakedSections, g_bakedSectionsUntextured,
             g_bakedSections ? 100.0 * double(g_bakedSectionsUntextured) / double(g_bakedSections) : 0.0,
             g_bakedSectionTris, g_bakedSectionTrisUntextured,
             g_bakedSectionTris ? 100.0 * double(g_bakedSectionTrisUntextured) / double(g_bakedSectionTris) : 0.0);
    // The LOD histogram is the row that tests the standing hypothesis directly: if most distant
    // objects sit in lod0, detail is being held too far out and that is the finding.
    LOG_INFO(Graphics, "Objects LOD histogram (main view): lod0={} lod1={} lod2={} lod3={} lod4={} lod5={} lod6={} lod7+={}",
             o.mainLodHist[0], o.mainLodHist[1], o.mainLodHist[2], o.mainLodHist[3], o.mainLodHist[4],
             o.mainLodHist[5], o.mainLodHist[6], o.mainLodHist[7]);
}

int EngineWgpu::GetCpuTimings(float* outMs, int maxCount) const
{
    if (!_renderer || !outMs || maxCount <= 0)
        return 0;
    std::lock_guard<std::mutex> lock(_memStatsMutex); // PERF-022: the worker's snapshot
    const int n = std::min(maxCount, std::min(_publishedCpuTimingCount, (int)WGR_GPU_TIMER_REGION_COUNT));
    std::memcpy(outMs, _publishedCpuTimings, sizeof(float) * (size_t)n);
    return n;
}

// REN-THR-012: the published copies. Readers get the last COMPLETED frame's counters,
// which is what they saw before -- the async readbacks land during the frame, so a live
// read from a dev panel was already showing a frame that had finished.
bool EngineWgpu::GetObjectStats(ObjectStatsOut& out) const
{
    if (!_publishedObjectStatsValid)
        return false;
    out = _publishedObjectStats;
    return true;
}

bool EngineWgpu::GetGrassStats(GrassStatsOut& out) const
{
    if (!_publishedGrassStatsValid)
        return false;
    out = _publishedGrassStats;
    return true;
}

bool EngineWgpu::GetGpuMemoryStats(GpuMemoryStatsOut& out) const
{
    if (!_publishedMemoryStatsValid)
        return false;
    out = _publishedMemoryStats;
    return true;
}

Engine::GeometryReportStatus EngineWgpu::RequestGeometryAllocationReport(uint32_t maxModels,
    uint32_t maxRows, uint32_t maxSectionVisits, uint64_t& requestId)
{
    requestId = 0;
    if (!Foundation::IsMainThread()) return GeometryReportStatus::Invalid;
    if (!maxModels || maxModels > 256 || !maxRows || maxRows > 2048 ||
        !maxSectionVisits || maxSectionVisits > 65536) return GeometryReportStatus::Invalid;
    if (!_renderer) return GeometryReportStatus::Unsupported;
    // Producer/owner operation, just like registration. Does not acquire a renderer window.
    std::unique_lock<std::mutex> lock(_geometryReportMutex, std::try_to_lock);
    if (!lock.owns_lock() || _geometryReportStatus == GeometryReportStatus::Pending)
        return GeometryReportStatus::Busy;
    if (_geometryReportNextRequest == std::numeric_limits<uint64_t>::max())
        return GeometryReportStatus::Invalid;
    ResourceOp op;
    op.kind = ResourceOp::GeometryReport;
    op.geometryReport = std::make_unique<ResourceOp::GeometryReportRequest>();
    op.geometryReport->requestId = ++_geometryReportNextRequest;
    op.geometryReport->maxModels = maxModels;
    op.geometryReport->rows = maxRows;
    op.geometryReport->visits = maxSectionVisits;
    op.geometryReport->registryEntries = static_cast<uint32_t>(std::min<size_t>(_gpuModels.size(), UINT32_MAX));
    op.geometryReport->models.reserve(std::min<size_t>(_gpuModels.size(), maxModels));
    for (const auto& [shape, model] : _gpuModels)
    {
        if (op.geometryReport->registryEntriesVisited == maxModels) break;
        ++op.geometryReport->registryEntriesVisited;
        // Rejected shapes are cached as INVALID to avoid repeated classification.
        // They have no retained renderer model and must not be reported as missing.
        if (model == WGR_INVALID_MODEL) {
            ++op.geometryReport->ineligibleEntriesSkipped;
            continue;
        }
        // Snapshot name+monotonic producer ID now; never carry the Shape pointer to drain.
        const char* name = shape ? static_cast<const char*>(shape->Name()) : nullptr;
        size_t length = 0;
        if (name) while (length < 1024 && name[length]) ++length;
        if (!length || length == 1024) ++op.geometryReport->unresolvedNames;
        op.geometryReport->models.emplace_back(model, name ? std::string(name, length) : std::string{});
    }
    requestId = op.geometryReport->requestId;
    _geometryReport.reset();
    _geometryReportRequest = requestId;
    _geometryReportStatus = GeometryReportStatus::Pending;
    _resourceOps.push_back(std::move(op));
    return GeometryReportStatus::Pending;
}

Engine::GeometryReportStatus EngineWgpu::PollGeometryAllocationReport(uint64_t requestId,
    GeometryAllocationReport& out) const
{
    if (!requestId) return GeometryReportStatus::Invalid;
    std::unique_lock<std::mutex> lock(_geometryReportMutex, std::try_to_lock);
    if (!lock.owns_lock()) return GeometryReportStatus::Pending;
    if (_geometryReportRequest != requestId) return GeometryReportStatus::Invalid;
    const auto status = _geometryReportStatus;
    const auto snapshot = _geometryReport;
    lock.unlock();
    if (status == GeometryReportStatus::Ready && snapshot) out = *snapshot;
    return status;
}

Engine::GeometryReportStatus EngineWgpu::RequestLodDemandReport(const std::vector<uint32_t>& models, uint32_t frames, uint64_t& requestId)
{
    requestId = 0;
    if (!Foundation::IsMainThread() || models.empty() || models.size() > 8 || !frames || frames > 64) return GeometryReportStatus::Invalid;
    for (size_t i = 0; i < models.size(); ++i)
        if (models[i] == WGR_INVALID_MODEL || std::find(models.begin(), models.begin() + i, models[i]) != models.begin() + i) return GeometryReportStatus::Invalid;
    const char* flag = std::getenv("WGR_GEOMETRY_LOD_FEEDBACK");
    if (!_renderer || !flag || std::strcmp(flag, "1")) return GeometryReportStatus::Unsupported;
    std::unique_lock<std::mutex> lock(_lodDemandMutex, std::try_to_lock);
    if (!lock.owns_lock() || _lodDemandStatus == GeometryReportStatus::Pending) return GeometryReportStatus::Busy;
    if (_lodDemandNextRequest == std::numeric_limits<uint64_t>::max()) return GeometryReportStatus::Invalid;
    ResourceOp op; op.kind = ResourceOp::LodDemandStart; op.lodDemand = std::make_unique<ResourceOp::LodDemandRequest>();
    op.lodDemand->epoch = ++_lodDemandNextRequest; op.lodDemand->frames = frames; op.lodDemand->models = models;
    requestId = _lodDemandRequest = op.lodDemand->epoch; _lodDemandStatus = GeometryReportStatus::Pending; _lodDemandReport.reset();
    _resourceOps.push_back(std::move(op)); return GeometryReportStatus::Pending;
}
Engine::GeometryReportStatus EngineWgpu::PollLodDemandReport(uint64_t requestId, LodDemandReport& out) const
{
    if (!requestId) return GeometryReportStatus::Invalid;
    std::unique_lock<std::mutex> lock(_lodDemandMutex, std::try_to_lock);
    if (!lock.owns_lock()) return GeometryReportStatus::Pending;
    if (_lodDemandRequest != requestId) return GeometryReportStatus::Invalid;
    if (_lodDemandReport) out = *_lodDemandReport;
    return _lodDemandStatus;
}
Engine::GeometryReportStatus EngineWgpu::CancelLodDemandReport(uint64_t requestId)
{
    if (!Foundation::IsMainThread() || !requestId) return GeometryReportStatus::Invalid;
    std::unique_lock<std::mutex> lock(_lodDemandMutex, std::try_to_lock);
    if (!lock.owns_lock()) return GeometryReportStatus::Busy;
    if (_lodDemandRequest != requestId) return GeometryReportStatus::Invalid;
    if (_lodDemandStatus != GeometryReportStatus::Pending) return _lodDemandStatus;
    ResourceOp op; op.kind = ResourceOp::LodDemandCancel; op.lodDemand = std::make_unique<ResourceOp::LodDemandRequest>();
    op.lodDemand->epoch = requestId; _resourceOps.push_back(std::move(op)); return GeometryReportStatus::Pending;
}

// Called once per frame, right after wgr_render_frame, where the readbacks have just
// landed. Three FFI calls a frame instead of one per consumer.
void EngineWgpu::PublishRendererStats()
{
    if (_lodDemandOwnerEpoch && _renderer)
    {
        WgrLodDemandReport raw{};
        if (wgr_poll_lod_demand(_renderer, _lodDemandOwnerEpoch, &raw) && raw.row_count <= 8)
        {
            auto report = std::make_shared<LodDemandReport>();
            report->requestId = raw.epoch; report->currentFrame = raw.current_frame; report->lastSampleFrame = raw.last_sample_frame;
            report->samplingStatus = raw.status; report->framesRequested = raw.frames_requested;
            report->framesAttempted = raw.frames_attempted; report->framesSampled = raw.frames_sampled;
            report->droppedFrames = raw.dropped_frames; report->mapFailures = raw.map_failures;
            report->droppedDispatches = raw.dropped_dispatches; report->dispatchedViews = raw.dispatched_views; report->passMask = raw.pass_mask;
            for (uint32_t i = 0; i < raw.row_count; ++i)
                report->rows.push_back({raw.rows[i].model_id, raw.rows[i].lod_count, raw.rows[i].lod_mask, raw.rows[i].state});
            _lodDemandUnpublished = std::move(report);
            _lodDemandOwnerStatus = raw.status == 1 ? GeometryReportStatus::Pending : GeometryReportStatus::Ready;
            if (raw.status != 1) _lodDemandOwnerEpoch = 0;
        }
    }
    if (_lodDemandUnpublished)
    {
        std::unique_lock<std::mutex> lock(_lodDemandMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            if (_lodDemandRequest == _lodDemandUnpublished->requestId)
            { _lodDemandReport = std::move(_lodDemandUnpublished); _lodDemandStatus = _lodDemandOwnerStatus; }
            _lodDemandUnpublished.reset();
        }
    }
    bool geometryPacketReady = true;
    if (_geometryReportUnpublished && _geometryReportUnpublished->rendererMeshAckTicket &&
        (_geometryReportUnpublished->rendererMeshAckState == 1 || _geometryReportUnpublished->rendererMeshAckState == 2))
    {
        auto& report = *_geometryReportUnpublished;
        WgrMeshSnapshotAck ack{};
        WgrMeshHandleFactSummary totals{};
        const uint32_t state = wgr_poll_mesh_snapshot_ack(_renderer, report.rendererMeshAckTicket,
            nullptr, 0, &totals, &ack);
        const bool sameCut = ack.ticket == report.rendererMeshAckTicket && ack.epoch == report.rendererMeshAckEpoch &&
            ack.state == state && ack.row_count == report.rendererMeshAckRequested && totals.complete == 1 && totals.flags == 0 &&
            totals.handles_requested == report.rendererMeshAckRequested && totals.handles_inspected == report.rendererMeshAckRequested &&
            totals.present == report.rendererMeshAckPresent && totals.absent == report.rendererMeshAckAbsent &&
            totals.invalid == 0 && totals.duplicate_handles == 0 &&
            totals.unique_vertex_bytes == report.rendererMeshAckVertexBytes && totals.unique_index_bytes == report.rendererMeshAckIndexBytes &&
            totals.record_scope_valid == 1 && totals.reserved == 0 &&
            totals.live_mesh_records == report.rendererMeshLiveRecords && totals.pool_live_bytes == report.rendererMeshRecordPoolLiveBytes &&
            totals.pool_generation == report.rendererMeshRecordPoolGeneration &&
            ack.pool_generation == report.rendererMeshAckPoolGeneration && ack.pool_live_bytes == report.rendererMeshAckPoolLiveBytes &&
            ack.pool_capacity_bytes == report.rendererMeshAckPoolCapacityBytes && ack.pool_retired_bytes == report.rendererMeshAckPoolRetiredBytes;
        // A failed cut validation is terminal even when the foreign state was
        // pending; publication-lock retry must not re-poll a cancelled ticket.
        report.rendererMeshAckState = sameCut ? state : 5;
        if (sameCut && (state == 1 || state == 2)) geometryPacketReady = false;
        else
        {
            report.rendererMeshAckValid = sameCut && state == 3 && ack.main_submission_serial != 0;
            report.rendererMeshAckSubmission = ack.main_submission_serial;
            wgr_cancel_mesh_snapshot_ack(_renderer, report.rendererMeshAckTicket);
        }
    }
    if (_geometryReportUnpublished && geometryPacketReady)
    {
        std::unique_lock<std::mutex> lock(_geometryReportMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            if (_geometryReportRequest == _geometryReportUnpublished->requestId)
            {
                _geometryReport = std::move(_geometryReportUnpublished);
                _geometryReportStatus = _geometryReport->valid
                    ? GeometryReportStatus::Ready : GeometryReportStatus::Failed;
            }
            _geometryReportUnpublished.reset();
        }
    }
    _publishedObjectStatsValid = GetObjectStatsFromRenderer(_publishedObjectStats);
    // LGT-026: how many local shadow views actually re-rendered last frame, and how many were
    // re-used. The whole point of the cache is a number, and without this one "it is cached"
    // would be an assertion rather than a measurement -- the shadows look identical either way,
    // which is exactly the property that makes a broken cache hard to notice.
    if (_renderer)
    {
        uint32_t rendered = 0, cached = 0;
        if (wgr_get_local_shadow_stats(_renderer, &rendered, &cached) != 0)
        {
            Poseidon::Dev::LocalShadowSettings& ls = Poseidon::Dev::GLocalShadowSettings();
            ls.viewsRendered = static_cast<int>(rendered);
            ls.viewsCached = static_cast<int>(cached);
            static const bool kSay = []
            {
                const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_STATS");
                return e != nullptr && std::atoi(e) != 0;
            }();
            if (kSay)
            {
                static int saidR = -1;
                static int saidC = -1;
                if (saidR != ls.viewsRendered || saidC != ls.viewsCached)
                {
                    saidR = ls.viewsRendered;
                    saidC = ls.viewsCached;
                    LOG_INFO(Graphics, "LGT-026 local shadow views: {} rendered, {} re-used (cache {}{})",
                             ls.viewsRendered, ls.viewsCached, ls.cacheStatic ? "on" : "off",
                             ls.forceRefresh ? ", FORCED refresh" : "");
                }
            }
        }
    }
    _publishedGrassStatsValid = GetGrassStatsFromRenderer(_publishedGrassStats);
    _publishedMemoryStatsValid = GetGpuMemoryStatsFromRenderer(_publishedMemoryStats);
    {
        WgrMemoryStats raw{};
        const bool ok = _renderer != nullptr && wgr_get_memory_stats(_renderer, &raw) != 0;
        float gpu[WGR_GPU_TIMER_REGION_COUNT] = {};
        float cpu[WGR_GPU_TIMER_REGION_COUNT] = {};
        const int gpuN = _renderer ? (int)wgr_get_gpu_timings(_renderer, gpu, WGR_GPU_TIMER_REGION_COUNT) : 0;
        const int cpuN = _renderer ? (int)wgr_get_cpu_timings(_renderer, cpu, WGR_GPU_TIMER_REGION_COUNT) : 0;
        std::lock_guard<std::mutex> lock(_memStatsMutex);
        _lastMemStats = raw;
        _lastMemStatsValid = ok;
        std::memcpy(_publishedGpuTimings, gpu, sizeof(gpu));
        std::memcpy(_publishedCpuTimings, cpu, sizeof(cpu));
        _publishedGpuTimingCount = gpuN;
        _publishedCpuTimingCount = cpuN;
        // LGT-026: the local-shadow region, to the log. Every other region is readable only
        // through the Objects tab, and a capture harness has no way to open a tab -- which is
        // how 24 depth passes a frame stayed unmeasured for as long as they did. Gated, and a
        // running mean rather than one frame: a single harvested sample of a sub-millisecond
        // region is mostly noise.
        static const bool kSayGpu = []
        {
            const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_STATS");
            return e != nullptr && std::atoi(e) != 0;
        }();
        if (kSayGpu && gpuN > (int)WGR_GPU_TIMER_LOCAL_SHADOW)
        {
            static double sum = 0.0;
            static double sumFrame = 0.0;
            static int n = 0;
            // -1 is "the region did not run this frame", and with the cache on that is not a
            // missing sample: it is a frame that spent NOTHING on local shadow views, which is
            // exactly the number the A/B is about. Reading it as absent would have left the
            // cached arm with no row at all -- a saving that reports as silence.
            const float raw = gpu[WGR_GPU_TIMER_LOCAL_SHADOW];
            const float ms = raw >= 0.0f ? raw : 0.0f;
            const float frameMs = gpu[kFrameGpuRegionTotal];
            if (frameMs >= 0.0f)
            {
                sum += ms;
                sumFrame += (frameMs >= 0.0f ? frameMs : 0.0f);
                if (++n >= 120)
                {
                    LOG_INFO(Graphics, "LGT-026 local shadow GPU: {:.3f} ms mean over {} samples (frame {:.3f} ms)",
                             sum / n, n, sumFrame / n);
                    sum = 0.0;
                    sumFrame = 0.0;
                    n = 0;
                }
            }
        }
    }
}

bool EngineWgpu::GetObjectStatsFromRenderer(ObjectStatsOut& out) const
{
    if (!_renderer)
        return false;
    WgrObjectStats s{};
    if (wgr_get_object_stats(_renderer, &s) == 0)
        return false;
    out.registeredInstances = s.registered_instances;
    out.valid = s.valid != 0;
    out.mainInstances = s.main_instances;
    out.mainRecords = s.main_records;
    out.mainDraws = s.main_draws;
    out.mainTris = s.main_tris;
    out.colorInstances = s.color_instances;
    out.colorRecords = s.color_records;
    out.colorDraws = s.color_draws;
    out.colorTris = s.color_tris;
    out.colorDrawsSolid = s.color_draws_solid;
    out.colorDrawsAlpha = s.color_draws_alpha;
    out.colorTrisSolid = s.color_tris_solid;
    out.colorTrisAlpha = s.color_tris_alpha;
    for (int c = 0; c < 4; ++c)
    {
        out.shadowInstances[c] = s.shadow_instances[c];
        out.shadowDraws[c] = s.shadow_draws[c];
        out.shadowTris[c] = s.shadow_tris[c];
    }
    for (int l = 0; l < 8; ++l)
        out.mainLodHist[l] = s.main_lod_hist[l];
    for (int k = 0; k < 4; ++k)
    {
        out.mainWhatIfTris[k] = s.main_whatif_tris[k];
        out.whatIfScale[k] = s.whatif_scale[k];
    }
    // REN-ATM-001 widened the census to eight words; it is no longer the same length as the
    // what-if arrays above and must not share their loop.
    for (int k = 0; k < 8; ++k)
        out.fragments[k] = s.fragments[k];
    out.directCalls = s.direct_calls;
    out.directIndirectCalls = s.direct_indirect_calls;
    out.directInstances = s.direct_instances;
    out.directTris = s.direct_tris;
    out.staleInstanceOps = s.stale_instance_ops;
    return true;
}

bool EngineWgpu::GetGrassStatsFromRenderer(GrassStatsOut& out) const
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

bool EngineWgpu::GetGpuMemoryStatsFromRenderer(GpuMemoryStatsOut& out) const
{
    if (!_renderer)
        return false;
    WgrMemoryStats stats{};
    if (wgr_get_memory_stats(_renderer, &stats) == 0)
        return false;
    out.trackedBytes = stats.tracked_bytes;
    out.budgetBytes = stats.budget_bytes;
    out.objectTextureBytes = stats.object_texture_bytes;
    out.geometryLiveBytes = stats.geometry_live_bytes;
    out.geometryCapacityBytes = stats.geometry_capacity_bytes;
    out.geometryRetiredBytes = stats.geometry_retired_bytes;
    out.objectTextureRetiredBytes = stats.object_texture_retired_bytes;
    out.backendAllocationBytes = stats.backend_allocation_bytes;
    out.objectTextureCount = stats.object_texture_count;
    out.overBudget = stats.over_budget != 0;
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
    _tonemap = TonemapAtHour(render::frame::GPresentationSnapshot().timeOfDay * 24.0f);
    _tonemap.bloomIntensity = bi;
    _tonemap.bloomThreshold = bt;
    _tonemap.bloomKnee = bk;
    // The push happens in NextFrame after UpdateAutoSky (PushRenderParams).
}

// The framerate-independent adaptation ease, and as of 2026-08-16 the DEFAULT one, because
// auto-exposure itself is now default-on (Engine.hpp, ExposureSettings::enabled).
//
// `ExposureSettings::rate` is authored as a PER-FRAME ease and is framerate-dependent: at
// 0.03/frame adaptation settles in ~1.5 s at 20 fps and ~0.7 s at 45 fps, so the two worlds
// measured this session adapt at more than 2x different speeds from the same number. Shipping
// a default-on feature on that ease would mean shipping a feel tuned against whichever world it
// was measured on, and it is the one defect RND-039 flagged as disqualifying for a default.
//
// alpha = 1 - exp(-dt/tau) is the standard correction: the same settling TIME at any framerate.
// The time constant comes from `ExposureSettings::rateTau` (0.4 s authored, live on the Tonemap
// tab's "Adapt time (s)"), and WGR_AUTO_EXPOSURE_TAU overrides it -- including with 0, which
// selects the legacy per-frame `rate` and is the A/B arm for this change.
//
// dt is measured here from steady_clock rather than taken from the engine because EngineWgpu has
// no frame-delta of its own.
float EngineWgpu::EffectiveAutoExposureTau() const
{
    // Read fresh every frame: `_exposure` is replaced wholesale by SetExposureSettings when the
    // dev panel moves a slider, so caching this would make "Adapt time (s)" take effect only
    // after a restart -- exactly the kind of silently-inert control this session is fixing
    // elsewhere.
    return _autoExposureTau >= 0.0f ? _autoExposureTau : _exposure.rateTau;
}

void EngineWgpu::UpdateAutoExposureRate()
{
    const float tau = EffectiveAutoExposureTau();
    if (tau <= 0.0f)
    {
        _autoExposureRate = 0.0f; // PushRenderParams falls back to the authored per-frame rate
        _autoExposureLast = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (_autoExposureLast.time_since_epoch().count() == 0)
    {
        // First frame in tau mode: no delta yet. Use the authored rate for one frame rather
        // than inventing a dt (a guessed first dt shows up as a visible one-frame step).
        _autoExposureLast = now;
        _autoExposureRate = 0.0f;
        return;
    }
    const float dt = std::chrono::duration<float>(now - _autoExposureLast).count();
    _autoExposureLast = now;
    // A long hitch (level load, alt-tab) must not snap the exposure: cap the step at 0.25 s so
    // the worst case is a quarter-second of adaptation, not an instant jump to the new target.
    const float step = std::clamp(dt, 0.0f, 0.25f);
    _autoExposureRate = std::clamp(1.0f - std::exp(-step / tau), 1.0e-4f, 1.0f);
}

void EngineWgpu::UpdateSunGlareExposure()
{
    const render::frame::PresentationSnapshot& glareSnap = render::frame::GPresentationSnapshot();
    if (!_renderer || !_hdrEnabled || !glareSnap.hasCamera || !glareSnap.hasMainLight)
    {
        _sunGlareExposure = 1.0f;
        return;
    }

    Vector3 view = glareSnap.cameraDir;
    // LightSun stores light travel direction; the visible sun lies opposite it.
    Vector3 sun = -glareSnap.sunDirection;
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
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)
    _pendingScreenshotPath = filename;
    if (_renderer)
        wgr_screenshot_request(_renderer);
}

void EngineWgpu::FlushPendingScreenshot()
{
    // PERF-021: the wait belongs AFTER the pending check. This runs every frame from
    // NextFrame, right after publish, and REN-THR-012 put the wait first -- so the producer
    // blocked until the worker had finished the frame it had just been handed, every frame,
    // and "overlap" was lockstep with extra steps (worker idle ~10 ms and the producer
    // waiting ~7.5 ms per frame on perf_combat, both measured). WGR_FLUSH_WAIT_EAGER=1 keeps
    // the old order for the A/B.
    static const bool eager = [] {
        const char* v = std::getenv("WGR_FLUSH_WAIT_EAGER");
        return v != nullptr && v[0] == '1';
    }();
    if (eager)
        WaitRenderIdle();
    // LGT-010: dump a shadow-array layer to a PNG once, N frames in. Driven from the
    // environment rather than the mission script, because the script route
    // (triShadowSceneDump) sits behind a mission whose own init exits early and produced
    // nothing twice. This runs from NextFrame on the producer thread, next to the
    // screenshot path, so it is a legal place to take the exclusive renderer handle.
    {
        static const char* dumpPath = std::getenv("POSEIDON_SHADOW_DUMP_PNG");
        static const int dumpFrame = [] {
            const char* v = std::getenv("POSEIDON_SHADOW_DUMP_FRAME");
            return v ? std::max(1, std::atoi(v)) : 600;
        }();
        static int frames = 0;
        static bool done = false;
        if (dumpPath != nullptr && !done && ++frames >= dumpFrame)
        {
            done = true;
            const bool ok = DumpShadowMap(dumpPath);
            LOG_INFO(Graphics, "LGT-010 shadow layer dump -> '{}' {}", dumpPath, ok ? "written" : "FAILED");
        }
    }
    if (!_renderer || _pendingScreenshotPath.GetLength() == 0)
        return;
    WaitRenderIdle(); // exclusive renderer handle below (REN-THR-012)

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
        // v3 (WRL-002): the `colour` group of a v2 profile is IGNORED on load. Every v2 file
        // on disk was an automatic snapshot of the code defaults at the time (the Water tab
        // saves whatever it holds on any edit, so the swatches were written unchanged), and
        // that pinned the old ultramarine deep swatch over any later default. A v3 profile's
        // colours were written by this code and are honoured.
        out << "v 5\nwave " << s.waveAmp << ' ' << s.waveChoppy << ' ' << s.waveSpeed << ' ' << s.waveScale << '\n';
        out << "colour " << s.shallowColor[0] << ' ' << s.shallowColor[1] << ' ' << s.shallowColor[2] << ' '
            << s.deepColor[0] << ' ' << s.deepColor[1] << ' ' << s.deepColor[2] << '\n';
        out << "coast " << s.colorExt << ' ' << s.coastFade << ' ' << s.foamWidth << ' ' << s.foamIntensity << ' '
            << s.swashAmp << ' ' << s.swashSpeed << ' ' << s.wetHeight << ' ' << s.wetDarken << '\n';
        out << "whitecaps " << s.waveFoamIntensity << ' ' << s.waveFoamDeepFalloff << ' '
            << (s.whitecapWindGate ? 1 : 0) << '\n';
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
    int version = 0;
    while (in >> key)
    {
        if (key == "v")
        {
            in >> version;
        }
        else if (key == "wave")
        {
            in >> _waterLook.waveAmp >> _waterLook.waveChoppy >> _waterLook.waveSpeed >> _waterLook.waveScale;
            // v2 snapshot rule: a value equal to the OLD code default was never tuned, only
            // auto-saved, so the current default wins for it (owner default changes must reach
            // the install). A value that differs (Everon's amplitude 0.42) is honoured.
            if (version < 3 && _waterLook.waveScale == 1.0f)
                _waterLook.waveScale = WaterSettings{}.waveScale;
        }
        else if (key == "colour")
        {
            float c[6] = {};
            in >> c[0] >> c[1] >> c[2] >> c[3] >> c[4] >> c[5];
            if (version >= 3)
            {
                std::copy(c, c + 3, _waterLook.shallowColor);
                std::copy(c + 3, c + 6, _waterLook.deepColor);
            }
            else
            {
                LOG_INFO(Graphics,
                         "Water look: ignoring the v{} colour group of '{}' (an automatic snapshot of old defaults); "
                         "the code defaults apply until the swatches are edited in the Water tab",
                         version, map);
            }
        }
        else if (key == "coast")
            in >> _waterLook.colorExt >> _waterLook.coastFade >> _waterLook.foamWidth >> _waterLook.foamIntensity >>
                _waterLook.swashAmp >> _waterLook.swashSpeed >> _waterLook.wetHeight >> _waterLook.wetDarken;
        else if (key == "surface")
        {
            in >> _waterLook.glitterGain >> _waterLook.sssGain >> _waterLook.reflectionGain;
            // v5: reflectionGain in a v4-or-older file is an automatic snapshot of the code
            // default of the day (the Water tab saves the whole subset on any edit), so it
            // pinned 0.7 over the new 0.22 on every install that had ever opened the tab.
            // Same failure the v3 colour rule exists for: a default that cannot reach the
            // owner is a default that was never changed, and nothing warns you.
            if (version < 5)
            {
                _waterLook.reflectionGain = WaterSettings{}.reflectionGain;
                LOG_INFO(Graphics,
                         "Water look: ignoring the v{} reflection gain of '{}' (an automatic snapshot of the old "
                         "default); the code default {:.2f} applies until it is edited in the Water tab",
                         version, map, _waterLook.reflectionGain);
            }
        }
        else if (key == "whitecaps")
        {
            int windGate = 1;
            in >> _waterLook.waveFoamIntensity >> _waterLook.waveFoamDeepFalloff >> windGate;
            _waterLook.whitecapWindGate = windGate != 0;
        }
        else if (key == "optics")
        {
            in >> _waterLook.alpha >> _waterLook.specPower >> _waterLook.specIntensity >> _waterLook.shadowDim >>
                _waterLook.warpAmp >> _waterLook.fadeStart >> _waterLook.fadeEnd;
            if (version < 3 && _waterLook.specPower == 11.0f)
                _waterLook.specPower = WaterSettings{}.specPower;
        }
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
            // v4: the on/off flag of an older profile was an automatic snapshot of the old
            // default (off); the current default applies. Thresholds and tuning are kept.
            if (version >= 4)
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
    // CATCH-ALL WORLD: one terrain material means no per-surface clutter classification at all
    // (TerrainWgpu::UploadGeography enables the single layer as a catch-all, 65536/65536 cells on
    // Reforger's Everon), so the stock density and height read as a knee-high meadow over the
    // whole map. Scale both by the conservative catch-all defaults; 1.0-neutral on every world
    // with a real classification. WGR_GRASS_CATCHALL=0 disables the reduction for an A/B.
    // Announced on transition: grep the game log for "Wgpu grass catch-all".
    static const bool catchAllEnv = [] {
        const char* v = std::getenv("WGR_GRASS_CATCHALL");
        return v == nullptr || *v != '0';
    }();
    const int surfaceCount = _terrain ? _terrain->GrassSurfaceCount() : 0;
    const bool catchAllWorld = catchAllEnv && surfaceCount == 1;
    const float catchAllCoverage = catchAllWorld ? std::clamp(settings.catchAllCoverage, 0.02f, 1.0f) : 1.0f;
    const float catchAllHeight = catchAllWorld ? std::clamp(settings.catchAllHeight, 0.10f, 1.0f) : 1.0f;
    {
        static int lastCatchAll = -1;
        const int state = catchAllWorld ? 1 : 0;
        if (state != lastCatchAll)
        {
            lastCatchAll = state;
            LOG_INFO(Graphics,
                     "Wgpu grass catch-all: {} ({} terrain material(s), WGR_GRASS_CATCHALL={}); coverage x{:.2f}, "
                     "blade height x{:.2f}",
                     catchAllWorld ? "ACTIVE -- single-material world, reduced defaults" : "off",
                     surfaceCount, catchAllEnv ? 1 : 0, catchAllCoverage, catchAllHeight);
        }
    }
    params.density = std::clamp(settings.density, 0.0f, 1.0f) * catchAllCoverage;
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
    params.blade_height = std::clamp(settings.height * catchAllHeight, 0.10f, 3.00f);
    params.wind_strength = std::clamp(settings.windStrength, 0.0f, 3.0f);
    params.wind_direction = settings.windDirection;
    // Manual by default here; the live-wind block below overrides it when windScrollAuto is
    // set, which is what keeps the shipped game at or below 1.0.
    params.wind_scroll = std::clamp(settings.windScroll, 0.02f, 4.0f);
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
    const render::frame::PresentationSnapshot& grassWindSnap = render::frame::GPresentationSnapshot();
    if (settings.useLiveWind && grassWindSnap.windActive)
    {
        if (grassWindSnap.windSpeed > 0.001f)
        {
            params.wind_direction = grassWindSnap.windDirectionRad * (180.0f / 3.14159265358979323846f);
            params.wind_strength =
                std::clamp(grassWindSnap.windSpeed * 0.20f * settings.windStrength, 0.0f, 3.0f);
            // Gust travel from the wind that causes it: linear in speed, reaching 1.0 only
            // at a storm. Capped there rather than allowed to run on -- past 1 the field
            // crosses the world faster than the shipped look and begins to strobe, and
            // nothing the weather can produce should get there. The dev slider still can,
            // by turning windScrollAuto off.
            if (settings.windScrollAuto)
            {
                params.wind_scroll = std::clamp(grassWindSnap.windSpeed / GrassSettings::kWindScrollFullSpeed,
                                                GrassSettings::kWindScrollFloor, 1.0f);
            }
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
    // WHICH grass: GrassSettings::clutterSource (Engine.hpp). Auto draws the map's own plants
    // (CfgSurfaces -> CfgSurfaceCharacters -> clutter .p3d -> per-surface atlas, baked per
    // cell) wherever the map built an atlas -- 25 cards over 26 layers on Takistan -- and the
    // procedural blades everywhere else, which is every OFP world (they build no atlas). The
    // owner asked for the map grass as the default and his own implementation selectable, and
    // for the panel to say which is on screen; the earlier env-only enable bypassed the checkbox.
    // Owner report on the card path (Takistan 6970.40 5700.31 352.57 az 20 el -18):
    // "flickering, far too dense". Both are the renderer's: cards now place on their own grid
    // (card_placement_spacing in grass.wgsl) instead of the 0.16 m blade grid, and their alpha
    // test runs alpha-to-coverage under MSAA (WGR_GRASS_CARD_A2C) instead of a hard discard.
    // WGR_GRASS_CLUTTER_SOURCE=0|1|2 overrides the setting for an A/B without touching the panel.
    static const int clutterSourceEnv = [] {
        const char* v = std::getenv("WGR_GRASS_CLUTTER_SOURCE");
        return v ? static_cast<int>(std::strtol(v, nullptr, 10)) : -1;
    }();
    int clutterSource = clutterSourceEnv >= 0 ? clutterSourceEnv : settings.clutterSource;
    if (settings.midPhotoTuft && clutterSourceEnv < 0)
    {
        clutterSource = 2; // legacy checkbox / saved setting
    }
    // RFG-062: the native Enfusion binding is a WORLD-side fact -- it decides what the
    // atlas and the geography bits contain -- so it is applied to the terrain rather
    // than pushed into the per-frame params. The call is a no-op unless the flag moved.
    if (_terrain != nullptr)
    {
        _terrain->SetEnfusionClutterEnabled(settings.enfusionClutter);
        _terrain->SetEnfusionBladesEnabled(settings.enfusionBlades);
        _terrain->SetEnfusionGroundEnabled(settings.enfusionGround);
    }
    const bool haveMapAtlas = _terrain && _terrain->GrassHasMapClutterAtlas();
    const bool mapClutter = clutterSource == 0 && haveMapAtlas;
    const bool photoTuft = mapClutter || clutterSource == 2;
    params.use_photo_tuft = photoTuft ? 1.0f : 0.0f;
    // RFG-090: with Reforger's blade atlas on the near ring, the ring draws BLADES and the
    // clutter cards move to the mid ring from the camera outward. Measured before this:
    // near 40 of 6,400 candidates placed (cards, 1.5 m grid) and no blade anywhere.
    // The lane carries the PlantMat's authored blade height (m); 0 = off.
    params.near_blades_only = _terrain != nullptr ? std::max(_terrain->GrassEnfusionBladeHeight(), 0.0f) : 0.0f;
    if (params.near_blades_only > 0.0f && params.near_blades_only < 0.01f)
        params.near_blades_only = 0.01f;
    // RFG-091: and the ground colour those blades lean towards.
    params.native_tint_lerp = _terrain != nullptr ? _terrain->GrassEnfusionBladeTint(params.native_tint) : 0.0f;
    _grassPhotoTuftActive = photoTuft;
    // Announce through the game log, on transition. A silent auto-enable is indistinguishable
    // from a broken one: the previous capture pair moved 229 pixels and could not tell whether
    // the cards looked the same or the flag never arrived.
    {
        static int lastTuft = -1;
        const int state = (params.use_photo_tuft > 0.5f ? 1 : 0) | (mapClutter ? 2 : 0);
        if (state != lastTuft)
        {
            lastTuft = state;
            LOG_INFO(Graphics, "Wgpu grass tuft: use_photo_tuft={} (clutterSource={} mapAtlas={} midPhotoTuft={})",
                     params.use_photo_tuft, clutterSource, haveMapAtlas ? 1 : 0, settings.midPhotoTuft ? 1 : 0);
        }
    }
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
    // RFG-090: the 0.625 plate brightness is for photographed A3 clumps; a Reforger clutter
    // atlas is authored albedo and keeps its own values.
    if (_terrain != nullptr && _terrain->GrassHasMapClutterAtlas())
        params.photo_tuft_brightness = 1.0f;
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
    // 0..31: the map clutter atlas has 32 layers, and finding the one plate that flickers means
    // being able to force any of them, not just the nine loose-card slots.
    params.photo_force_layer =
        settings.photoForceLayer < 0 ? -1.0f : static_cast<float>(std::min(settings.photoForceLayer, 31));
    for (size_t i = 0; i < std::size(params.photo_layer_weights); ++i)
    {
        params.photo_layer_weights[i] = std::clamp(settings.photoLayerWeights[i], 0.0f, 1.0f);
    }
    params.lod_blend = std::clamp(settings.lodBlend, 0.0f, 40.0f);
    // Card density is no longer a coverage hack here: the renderer places cards on their own
    // grid (card_placement_spacing in grass.wgsl, WGR_GRASS_CARD_SPACING) shared by the near
    // and mid rings, so nearDensity is a plain multiplier again. The former "0.15 when cards
    // auto-enable" override would now put the near ring at 0.15 against the mid ring's 0.95.
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
    // Photo-card coverage (fraction of 1.11 m clutter cells that grow a card) and whether the
    // map's own authored density wins where the bake answered. Cards only; blades ignore it.
    params.card_coverage = std::clamp(settings.photoCoverage, 0.01f, 1.0f);
    params.card_coverage_auto = settings.photoCoverageAuto ? 1.0f : 0.0f;
    params.card_scale = std::clamp(settings.photoCardScale, 0.25f, 2.0f);
    // Card PLACEMENT GRID. Its own entry point rather than a WgrGrassParams field because
    // that struct's size is part of the ABI handshake (same reason as the cloud-shadow
    // strength). Coverage above is a fraction of THIS grid, so the grid is the ceiling
    // coverage cannot pass -- lowering it is what lets the photographed clumps reach the
    // procedural rings' density.
    if (_renderer != nullptr)
    {
        // WGR_GRASS_CARD_SPACING still wins if set, so the startup lever the
        // renderer documents keeps working and an A/B needs no panel.
        static const float envOverride = []
        {
            const char* v = std::getenv("WGR_GRASS_CARD_SPACING");
            return v != nullptr ? static_cast<float>(std::atof(v)) : -1.0f;
        }();
        // Density multiplier wins when it is above 1: it is what the panel drives.
        // spacing = clutterGrid / sqrt(density), so 20x density is 1.11/4.47 = 0.248 m.
        // POSEIDON_GRASS_CARD_DENSITY=<x> forces the multiplier, so the exact code
        // path the panel drives can be exercised without the panel.
        static const float densityEnv = []
        {
            const char* v = std::getenv("POSEIDON_GRASS_CARD_DENSITY");
            return v != nullptr ? static_cast<float>(std::atof(v)) : -1.0f;
        }();
        const float density =
            densityEnv > 0.0f ? densityEnv : std::max(settings.photoCardDensity, 1.0f);
        const float fromDensity = 1.11f / std::sqrt(density);
        const float grid = envOverride > 0.0f ? envOverride
                                              : (density > 1.001f ? fromDensity : settings.photoCardSpacing);
        const float applied = std::clamp(grid, 0.06f, 4.0f);
        _pendingSet.grassCardSpacing = applied;
        _pendingSet.grassSpacingDirty = true;
        // Say what arrived, on change only. SubmitGrass re-pushes this every frame,
        // so an unrate-limited line would be one per frame; a line ONLY when the
        // number moves is exactly the signal "did my slider reach the renderer".
        {
            static float lastApplied = -1.0f;
            if (std::fabs(applied - lastApplied) > 1e-4f)
            {
                lastApplied = applied;
                LOG_INFO(Graphics,
                         "Wgpu grass card density: {:.1f}x -> grid {:.3f} m (setting {:.1f}x, spacing {:.3f}, "
                         "env {:.3f}/{:.3f})",
                         density, applied, settings.photoCardDensity, settings.photoCardSpacing, densityEnv,
                         envOverride);
            }
        }
    }
    // Procedural blade look; defaults are the pre-existing shading (see GrassSettings).
    params.blade_self_shadow = std::clamp(settings.bladeSelfShadow, 0.0f, 1.0f);
    params.blade_contrast = std::clamp(settings.bladeContrast, 0.25f, 3.0f);
    params.blade_hue_variation = std::clamp(settings.bladeHueVariation, 0.0f, 1.0f);
    params.blade_root_shade = std::clamp(settings.bladeRootShade, 0.0f, 0.95f);
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
    // RenderSnapshot S2: the interactor and the rotor sources come from the frame's
    // captured block -- the world walks its own vehicle list once, and this path no
    // longer touches GWorld/GLandscape or runs RTTI.
    const render::frame::PresentationSnapshot& grassSnap = render::frame::GPresentationSnapshot();
    if (grassSnap.hasInteractor)
    {
        const Vector3 pos = grassSnap.interactorPos;
        params.interactor_x = pos.X();
        params.interactor_z = pos.Z();
        params.interactor_radius = std::clamp(grassSnap.interactorVisibleSize * 0.55f, 1.1f, 8.0f);
        params.interactor_strength = 1.0f;
        // The occupied vehicle in normal play. A powered player helicopter gets an
        // explicit, large crush field here rather than depending solely on the
        // rotor-source list below; this also covers the first few frames of takeoff.
        if (grassSnap.interactorRotor && grassSnap.interactorRotorSpeed > 0.02f)
        {
            const float rotorSpeed = grassSnap.interactorRotorSpeed;
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
    // Rotor wash is intentionally transient. Keep the four closest rotor sources
    // whose downwash can reach the camera's grass region; the snapshot's list is
    // spinning-rotor helicopters only (rotor state, not Airborne(), so the effect
    // begins while the skids still touch the ground) and already includes the
    // occupied one -- the old just-dismounted weak-link special case is gone.
    {
        struct Candidate
        {
            float distance2;
            WgrGrassDownwash wash;
        };
        std::array<Candidate, WGR_GRASS_DOWNWASH_COUNT> nearest{};
        for (Candidate& entry : nearest)
            entry.distance2 = std::numeric_limits<float>::infinity();
        const Vector3 cameraPos = grassSnap.hasInteractor ? grassSnap.interactorPos : VZero;
        for (int i = 0; i < grassSnap.rotorSourceCount; ++i)
        {
            const render::frame::PresentationSnapshot::RotorSource& source = grassSnap.rotorSources[i];
            const float height = std::max(0.0f, source.y - source.groundY);
            if (height > 65.0f)
                continue;
            const float rotorSpeed = source.rotorSpeed;
            const float radius = (10.0f + height * 0.40f) * (0.45f + 0.55f * rotorSpeed);
            // Keep enough force at the upper edge of the range to visibly
            // press the blades flat rather than merely sway their tips.
            const float strength = std::clamp(1.10f - height / 260.0f, 0.85f, 1.0f) * rotorSpeed * rotorSpeed;
            const float dx = source.x - cameraPos.X();
            const float dz = source.z - cameraPos.Z();
            const float distance2 = dx * dx + dz * dz;
            int slot = 0;
            for (int j = 1; j < WGR_GRASS_DOWNWASH_COUNT; ++j)
                if (nearest[j].distance2 > nearest[slot].distance2)
                    slot = j;
            if (distance2 < nearest[slot].distance2)
                nearest[slot] = Candidate{distance2, {source.x, source.z, radius, strength}};
        }
        for (int i = 0; i < WGR_GRASS_DOWNWASH_COUNT; ++i)
            params.downwash[i] = nearest[i].wash;
    }
    std::copy(_grassTracks.begin(), _grassTracks.end(), params.tracks);
    _pendingSet.grassParams = params;
    _pendingSet.grassParamsDirty = true;
    // PLACEMENT WITNESS. A capture pair that "looks the same" cannot tell an inert lever from a
    // field the placement pass rejected outright (this pose sits on a hard-object cell, the
    // world's geography excludes it, ...). The compacted counters say which: log them when the
    // field first appears, whenever it empties or refills, and while it is empty every ~10 s.
    // The renderer's own card-state line ("Wgpu grass cards: drawn=...") arrives through the same
    // log from grass/mod.rs via lib.rs, so a run carries both halves of the proof.
    {
        static int lastPlaced = -1;      // -1 unknown, 0 empty, 1 placing
        static float sinceReport = 0.0f;
        GrassStatsOut stats;
        if (params.enabled > 0.5f && GLandscape != nullptr && _terrain && GetGrassStats(stats))
        {
            const unsigned placed = stats.nearInstances + stats.midInstances + stats.farInstances;
            const int state = placed > 0 ? 1 : 0;
            sinceReport += dt;
            const bool emptyReminder = state == 0 && sinceReport >= 10.0f;
            if (state != lastPlaced || emptyReminder)
            {
                lastPlaced = state;
                sinceReport = 0.0f;
                LOG_INFO(Graphics,
                         "Wgpu grass placement: near {}/{} mid {}/{} far {}/{} instances/candidates{}",
                         stats.nearInstances, stats.nearCandidates, stats.midInstances, stats.midCandidates,
                         stats.farInstances, stats.farCandidates,
                         placed == 0 ? " -- NOTHING PLACED at this camera (geography exclusions, surface "
                                       "selection, sea level or slope rejected every candidate; not a look setting)"
                                     : "");
            }
        }
    }
}

bool EngineWgpu::HasGrassPhotoClumps() const
{
    AcquireProducerWindow("grass photo clumps");
    return _renderer != nullptr && wgr_grass_have_photo_clumps(_renderer) != 0;
}

bool EngineWgpu::HasGrassMapClutter() const
{
    return _terrain && _terrain->GrassHasMapClutterAtlas();
}

bool EngineWgpu::HasEnfusionSurfaces() const
{
    return _terrain && _terrain->HasEnfusionSurfaces();
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
    const SkySettings k = SkyAtHour(render::frame::GPresentationSnapshot().timeOfDay * 24.0f);
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
    QueueSkyExtras();
    PushRenderParams();
}

namespace
{
/// The camera near plane, in metres, for the depth-of-field shader.
///
/// Not a look setting and not something the renderer can know on its own: the DoF pass turns
/// reversed-Z depth into a distance with `near / depth`, and the projection lives on the scene
/// camera. Read per push rather than cached because a scope, a vehicle interior or a cutscene can
/// each change it mid-mission, and a stale near plane silently rescales every focus distance.
float DepthOfFieldNearPlane()
{
    if (GLOB_SCENE)
    {
        if (const Camera* camera = GLOB_SCENE->GetCamera())
        {
            const float n = float(camera->Near());
            if (n > 1e-4f)
            {
                return n;
            }
        }
    }
    return 0.05f; // the engine's own default, and harmless: it only scales the depth mapping
}
} // namespace

void EngineWgpu::SetDepthOfFieldSettings(const DepthOfFieldSettings& s)
{
    _dof = s;
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
        // NV-001. The whole reason night vision showed "no green, just darkness": the legacy
        // path multiplied _accomodateEye onto the SUN colour a few thousand lines above, and
        // the sky-lit branch immediately overwrote sunLin[] with the atmospheric radiance --
        // so the filter was computed and discarded. Even surviving, it could not have worked:
        // at night the sun contributes nothing, and local lights, moonlight and ambient never
        // saw it. An intensifier amplifies the IMAGE, so it belongs in the tonemap.
        //
        // POSEIDON_NV_FORCE=1 turns the goggles on without a player wearing any, which is the
        // only way to capture this from the scripted harness.
        (GetNightVision() || NightVisionForced()) ? 1.0f : 0.0f,
        _tonemap.nvGain,
        _tonemap.nvNoise,
        _tonemap.nvVignette,
    };

    // Picture Mode depth of field. The near plane is not a look setting but has to travel with
    // the rest: the shader turns reversed-Z depth into metres with `near / depth` and the renderer
    // has no other access to the projection.
    p.depth_of_field = {
        _dof.enabled ? 1u : 0u,
        _dof.focusDistance,
        _dof.focusRange,
        _dof.maxBlurPixels,
        _dof.backgroundScale,
        _dof.foregroundScale,
        _dof.transition,
        DepthOfFieldNearPlane(),
        float(_dof.debugView),
        float(_dof.sampleCount),
        _dof.bokehBoost,
        _dof.bokehThreshold,
        _dof.apertureBlades,
    };

    p.exposure = {
        _exposure.enabled ? 1.0f : 0.0f,
        _exposure.key,
        _exposure.minScale,
        _exposure.maxScale,
        // The framerate-INDEPENDENT ease derived from the time constant, which is now the
        // normal case (ExposureSettings::rateTau defaults to 0.4 s). UpdateAutoExposureRate
        // leaves this 0 when the effective tau is <= 0 -- i.e. only when the authored rateTau
        // is 0 or WGR_AUTO_EXPOSURE_TAU=0 asked for the legacy per-frame ease -- and also on
        // the first frame in tau mode, where there is no measured dt yet.
        _autoExposureRate > 0.0f ? _autoExposureRate : _exposure.rate,
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
                          _sky.nightIntensity, _sky.cloudReflectionCheap ? 1.0f : 0.0f};
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
    if (_sky.cloudCoverageFromWeather && render::frame::GPresentationSnapshot().hasLandscape)
    {
        const float overcast = render::frame::GPresentationSnapshot().overcast;
        const float t = overcast < 0.0f ? 0.0f : (overcast > 1.0f ? 1.0f : overcast);
        _sky.cloudCoverage = _sky.cloudCoverageClear + (_sky.cloudCoverageFull - _sky.cloudCoverageClear) * t;
    }
    // WGR_CLOUD_COVERAGE=<0..1> pins coverage and overrides the weather drive above. The Sky tab
    // can already do this, but a --no-dev capture has no Sky tab, and the cloud deck occludes the
    // sun and moon discs per ray -- so with no way to force a clear sky from a command line, a
    // capture of anything celestial is a coin flip against the mission's overcast.
    if (const char* cc = std::getenv("WGR_CLOUD_COVERAGE"))
    {
        const float v = std::strtof(cc, nullptr);
        _sky.cloudCoverage = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    p.sky.cloud0 = {_sky.cloudCoverage, _sky.cloudDensity, _sky.cloudBottom, _sky.cloudTop};
    p.sky.cloud1 = {0.0f, 0.0f, shapeScale, detailScale};
    p.sky.cloud2 = {_sky.cloudHgG, _sky.cloudPowder, _sky.cloudAmbient, _sky.cloudMaxDist};
    p.sky.cloud3 = {weatherScale, _sky.cloudWeatherAmount, warpScale, _sky.cloudWarpAmount};

    // CLD-020 cloud shadows. Pushed through its own entry point rather than a WgrSkyLook lane:
    // the look struct's size is part of the ABI handshake, and this is one float.
    if (_renderer != nullptr)
    {
        // Smoke -> ground shadow: this frame's particles into the cloud shadow map.
        // Published here because this runs once per frame right before the sky
        // pass consumes the map. Empty is a valid publish (clears the shadow).
        {
            static std::vector<SmokeSystem::ShadowBlob> blobs;
            static std::vector<WgrSmokeShadowBlob> packed;
            GSmokeSystem.CollectShadowBlobs(blobs, 512);
            packed.resize(blobs.size());
            for (std::size_t i = 0; i < blobs.size(); ++i)
            {
                packed[i].pos_radius = {blobs[i].x, blobs[i].y, blobs[i].z, blobs[i].radius};
                packed[i].density = {blobs[i].density, blobs[i].heightAboveGround, 0.0f, 0.0f};
            }
            _pendingSet.smokeBlobs.assign(packed.begin(), packed.end());
            _pendingSet.smokeStrength = GSmokeSystem.GroundShadowStrength();
            _pendingSet.smokeDirty = true;

            // SMK-038: the same particles for froxel injection, but collected with `force`
            // (the ground-shadow strength gate has nothing to do with whether the volumetric
            // system wants them) and at the injection cap, which is four times the shadow
            // pass's -- injection costs per particle, the shadow pass costs per particle PER
            // SHADOW TEXEL. Collected only while the volumetric system is selected, so
            // choosing Legacy costs a comparison.
            SmokeVolumetricParams& vol = GSmokeVolumetric();
            if (vol.mode != SmokeSystemMode::Legacy)
            {
                static std::vector<SmokeSystem::ShadowBlob> volBlobs;
                GSmokeSystem.CollectShadowBlobs(volBlobs, 2048, true);
                _pendingSet.smokeVolumeBlobs.resize(volBlobs.size());
                for (std::size_t i = 0; i < volBlobs.size(); ++i)
                {
                    _pendingSet.smokeVolumeBlobs[i].pos_radius = {volBlobs[i].x, volBlobs[i].y, volBlobs[i].z,
                                                                  volBlobs[i].radius};
                    _pendingSet.smokeVolumeBlobs[i].density = {volBlobs[i].density, volBlobs[i].heightAboveGround,
                                                               0.0f, 0.0f};
                }
            }
            else
            {
                _pendingSet.smokeVolumeBlobs.clear();
            }
            // The sun the smoke is lit by is the one SMK-035 already shades the billboards
            // with, so the two systems cannot disagree about the time of day.
            WgrSmokeVolumeParams vp{};
            vp.mode = static_cast<uint32_t>(vol.mode);
            vp.cell_size = vol.cellSize;
            vp.march_steps = static_cast<uint32_t>(vol.marchSteps);
            vp.sun_steps = static_cast<uint32_t>(vol.sunSteps);
            vp.sun_step_len = vol.sunStepLen;
            vp.density = vol.density;
            vp.extinction = vol.extinction;
            vp.albedo = vol.albedo;
            vp.anisotropy = vol.anisotropy;
            vp.self_shadow = vol.selfShadow;
            vp.jitter = vol.jitter;
            vp.scale = static_cast<uint32_t>(vol.scale);
            if (GLOB_SCENE != nullptr && GLOB_SCENE->MainLight() != nullptr)
            {
                const LightSun* sun = GLOB_SCENE->MainLight();
                const Vector3 toSun = -sun->Direction();
                vp.sun_dir = {toSun.X(), toSun.Y(), toSun.Z(), 0.0f};
                const LegacySmokeShading& sh = GLegacySmokeShading();
                const Color diffuse = sun->Diffuse() * sh.diffuse;
                const Color ambient = sun->Ambient() * sh.ambient;
                vp.sun_radiance = {diffuse.R(), diffuse.G(), diffuse.B(), 0.0f};
                vp.ambient = {ambient.R(), ambient.G(), ambient.B(), 0.0f};
            }
            else
            {
                vp.sun_dir = {0.0f, 1.0f, 0.0f, 0.0f};
                vp.sun_radiance = {1.0f, 1.0f, 1.0f, 0.0f};
                vp.ambient = {0.2f, 0.2f, 0.24f, 0.0f};
            }
            _pendingSet.smokeVolume = vp;
            _pendingSet.smokeVolumeDirty = true;
        }
        _pendingSet.cloudShadowStrength = std::clamp(_sky.cloudShadowStrength, 0.0f, 1.0f);
        _pendingSet.starIntensity = std::clamp(_sky.starIntensity, 0.0f, 4.0f);
        _pendingSet.lensFlare = std::clamp(_sky.lensFlare, 0.0f, 4.0f);
        _pendingSet.skyScalarsDirty = true;
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

    // VEG-SWAY — geometric wind on vegetation MODELS. Resolved here, next to the lighting
    // knobs it ships alongside, because this is the one place that already holds both the
    // foliage settings and the engine clock.
    //
    // The wind VECTOR is read from the AUTHORITY (World/Weather/WindModel), exactly as the
    // grass field at PushGrass and the cloud deck below do — so canopy, grass and cloud all
    // lean the same way instead of each inventing air. What is local to foliage is only the
    // RESPONSE: amplitude scaled by speed against a reference, which is what makes a calm day
    // barely stir and a squall visibly work the canopy.
    float swayAmp = std::max(_foliage.swayStrength, 0.0f);
    float swayDirX = 1.0f;
    float swayDirZ = 0.0f;
    float swayGust = 0.0f;
    {
        // A/B override for automation. There is no CLI flag for wind strength anywhere in this
        // engine (the grass sliders are dev-panel only), and a motion feature cannot be proven
        // without a wind-OFF control frame — so expose one the same way the renderer already
        // exposes WGR_FOLIAGE_A2C. `POSEIDON_VEG_SWAY=0` is the control; any other value is a
        // multiplier on the authored amplitude.
        static const float envScale = []() -> float {
            const char* v = std::getenv("POSEIDON_VEG_SWAY");
            if (v == nullptr || *v == '\0')
                return 1.0f;
            char* end = nullptr;
            const float parsed = std::strtof(v, &end);
            if (end == v)
                return 1.0f;
            LOG_INFO(Graphics, "VEG-SWAY: POSEIDON_VEG_SWAY={} — vegetation sway amplitude scaled by {:.3f}", v,
                     parsed);
            return std::max(parsed, 0.0f);
        }();
        swayAmp *= envScale;
        // Calm-wind FLOOR on the live-wind response. The response is speed / swayWindReference
        // (6 m/s); WindModel's clear-sky mean is calmSpeed = 1.5 m/s, so on any overcast-0
        // world the amplitude collapses to 0.25 x 0.35 m = 9 cm of tip travel at the 12 m
        // reference height -- one or two pixels at a normal viewing distance. That is why the
        // canopy reads as "no wind animation" on a clear day (owner report, Stratis pines) while
        // the SAME shader term is plainly visible on a tall model under an overcast front. The
        // gate (MapType -> kind / INST_CANOPY_*) was measured correct for Arma 3: every
        // plants_f tree authors `map=tree` and every b_ bush `map=bush`, in the geometry LOD
        // PropertyValue consults first.
        //
        // The floor is a fraction of the reference response, so at 0.5 a calm day still
        // stirs the canopy at half the reference amplitude, and a stiffening wind takes over
        // above 3 m/s exactly as before. Real trees are never perfectly still; a hard zero at
        // calm was a modelling choice, not a measurement. WGR_TREE_SWAY_CALM_FLOOR=0 restores
        // the pure-linear response for an A/B; values are clamped to [0, 2.5].
        static const float calmFloor = []() -> float {
            const char* v = std::getenv("WGR_TREE_SWAY_CALM_FLOOR");
            if (v == nullptr || *v == '\0')
                return 0.5f;
            char* end = nullptr;
            const float parsed = std::strtof(v, &end);
            if (end == v)
                return 0.5f;
            LOG_INFO(Graphics, "VEG-SWAY: WGR_TREE_SWAY_CALM_FLOOR={} — calm-wind response floor {:.3f}", v,
                     parsed);
            return std::clamp(parsed, 0.0f, 2.5f);
        }();
        float windResponse = 1.0f;
        float windSpeed = -1.0f; // -1 = live wind not consulted (authored steady breeze)
        const render::frame::PresentationSnapshot& windSnap = render::frame::GPresentationSnapshot();
        if (_foliage.swayUseLiveWind && windSnap.windActive)
        {
            const float reference = std::max(_foliage.swayWindReference, 0.1f);
            windResponse = std::clamp(std::max(windSnap.windSpeed / reference, calmFloor), 0.0f, 2.5f);
            windSpeed = windSnap.windSpeed;
            swayAmp *= windResponse;
            if (windSnap.windSpeed > 0.001f)
            {
                swayDirX = std::cos(windSnap.windDirectionRad);
                swayDirZ = std::sin(windSnap.windDirectionRad);
            }
            swayGust = std::clamp(windSnap.windGustFraction, -1.0f, 1.0f);
        }
        // WGR_SWAY_TRACE=1: log the RESOLVED amplitude the shaders will read, once a second.
        // Sway is motion, so a still capture cannot show whether the term is live; this line
        // can. amp is metres of tip travel at 12 m (frame.foliaged.x); amp == 0 means both
        // vertex shaders branch the whole term out regardless of any model's gate.
        static const bool swayTrace = std::getenv("WGR_SWAY_TRACE") != nullptr;
        if (swayTrace)
        {
            static float lastLog = -1e9f;
            const float now = windSnap.timeSeconds;
            if (now - lastLog >= 1.0f || now < lastLog)
            {
                lastLog = now;
                LOG_INFO(Graphics,
                         "VEG-SWAY: amp={:.4f} strength={:.3f} env={:.3f} windSpeed={:.2f} response={:.3f} "
                         "floor={:.3f} gust={:.3f} dir=({:.2f},{:.2f}) liveWind={} windActive={}",
                         swayAmp, _foliage.swayStrength, envScale, windSpeed, windResponse, calmFloor, swayGust,
                         swayDirX, swayDirZ, _foliage.swayUseLiveWind ? 1 : 0, windSnap.windActive ? 1 : 0);
            }
        }
    }
    // The SAME clock the water surface and the cloud deck run on (Glob.time), and frozen by the
    // same switch — so a deterministic test frame stays deterministic with sway on, and a paused
    // simulation freezes the canopy instead of leaving it stirring over a stopped world.
    const float swayTime =
        _waterLook.freeze.freezeTime ? _waterLook.freeze.fixedTime : render::frame::GPresentationSnapshot().timeSeconds;

    // WGR_FOLIAGE_ABLATE=1 zeroes the leaf-lighting model for ONE frame's worth of A/B: the
    // subsurface fill, the terminator wrap, the ambient boost and -- the one that matters most
    // for "the canopy is completely flat" -- the SPHERICAL NORMALS. treeBend/normalBend replace
    // each leaf card's geometric normal with a radial one about the crown centre, which is what
    // makes a canopy read as one smooth ball instead of a stack of separately-lit cards. The
    // sway terms are deliberately NOT touched: this ablates lighting, not motion.
    //
    // An ablation, default off, so the comparison costs an env var instead of a rebuild. It is
    // not a quality setting and must not become one -- the knobs exist because a low-poly card
    // canopy DOES split into a hard lit/black pair without them.
    static const bool foliageAblate = []
    {
        const char* v = std::getenv("WGR_FOLIAGE_ABLATE");
        return v && v[0] != '0';
    }();
    // Granular A/B companions to the ablation above: override ONE knob so a wash
    // can be attributed to ambient boost vs spherical bends instead of only being
    // switched off wholesale. Unset = configured default. NaN = unset.
    static const float foliageAmbientBoostOverride = []
    {
        const char* v = std::getenv("WGR_FOLIAGE_AMBIENT_BOOST");
        if (v == nullptr || v[0] == '\0') return std::numeric_limits<float>::quiet_NaN();
        const float parsed = std::strtof(v, nullptr);
        return std::isfinite(parsed) ? parsed : std::numeric_limits<float>::quiet_NaN();
    }();
    static const float foliageTreeBendOverride = []
    {
        const char* v = std::getenv("WGR_FOLIAGE_TREE_BEND");
        if (v == nullptr || v[0] == '\0') return std::numeric_limits<float>::quiet_NaN();
        const float parsed = std::strtof(v, nullptr);
        return std::isfinite(parsed) ? parsed : std::numeric_limits<float>::quiet_NaN();
    }();
    static const float foliageWrapOverride = []
    {
        const char* v = std::getenv("WGR_FOLIAGE_WRAP");
        if (v == nullptr || v[0] == '\0') return std::numeric_limits<float>::quiet_NaN();
        const float parsed = std::strtof(v, nullptr);
        return std::isfinite(parsed) ? parsed : std::numeric_limits<float>::quiet_NaN();
    }();
    static const float foliageTransOverride = []
    {
        const char* v = std::getenv("WGR_FOLIAGE_TRANS_SCALE");
        if (v == nullptr || v[0] == '\0') return std::numeric_limits<float>::quiet_NaN();
        const float parsed = std::strtof(v, nullptr);
        return std::isfinite(parsed) ? parsed : std::numeric_limits<float>::quiet_NaN();
    }();
    const float fTrans = foliageAblate ? 0.0f : (std::isnan(foliageTransOverride) ? _foliage.transScale : foliageTransOverride);
    const float fWrap = foliageAblate ? 0.0f : (std::isnan(foliageWrapOverride) ? _foliage.wrap : foliageWrapOverride);
    // Native dense crowns carry their own depth: the configured lift flattens them
    // (Reference-spot A/B: ambientBoost 1.6 = flat bright canopy, 1.0 = modeled
    // light/shade). Legacy OFP sparse trees keep the configured values untouched.
    const bool nativeDenseCrowns =
        GLandscape != nullptr && GLandscape->GetEnfusionSurfaceCount() > 0;
    const float fAmbBoost = foliageAblate                ? 1.0f
                            : !std::isnan(foliageAmbientBoostOverride) ? foliageAmbientBoostOverride
                            : nativeDenseCrowns                        ? 1.0f
                                                                       : _foliage.ambientBoost;
    const float fNormalBend = foliageAblate ? 0.0f : _foliage.normalBend;
    const float fTreeBend = foliageAblate ? 0.0f : (std::isnan(foliageTreeBendOverride) ? _foliage.treeBend : foliageTreeBendOverride);
    // FOLIAGE-DUSK (Poseidon::FoliageDuskCurve, dev panel: Foliage > "Dusk curve"). 1.0 is the
    // untouched daylight ramp, so the ablation takes it too.
    const float fDuskCurve = foliageAblate ? 1.0f : Poseidon::FoliageDuskCurve();

    // Foliage lighting (emulated leaf SSS for alpha-tested vegetation); rides the Frame UBO.
    p.foliage = {
        fTrans,                _foliage.distortion,  _foliage.transPower,   fWrap,
        fAmbBoost,             fNormalBend,          _foliage.crownYOffset, _foliage.fillFadeEnd,
        _foliage.giStrength,   fTreeBend,            _foliage.treeCrownY,   fDuskCurve,
        swayAmp,               _foliage.swaySpeed,   swayDirX,              swayDirZ,
        swayTime,              swayGust,             _foliage.swayLeafFlutter,
        std::clamp(_foliage.swayStiffness, 0.25f, 4.0f),
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
        _interiorSkyProbe,
    };

    p.gi = {
        _gi.enabled ? 1u : 0u,
        static_cast<uint32_t>(std::clamp(_gi.basis, 0, 2)),
        static_cast<uint32_t>(std::clamp(_gi.rays, 8, 64)),
        static_cast<uint32_t>(std::clamp(_gi.rsmSamples, 0, 64)),
        _gi.weight,
        _gi.interiorMix,
        _gi.spacing,
        _gi.hysteresis,
        {_gi.groundAlbedo[0], _gi.groundAlbedo[1], _gi.groundAlbedo[2]},
        _gi.groundGain,
        _gi.wallAlbedo,
        _gi.rayLength,
        _gi.rsmRadius,
        _gi.rsmGain,
        _gi.indoorSky,
    };
    // REN-THR-013: queued; the worker applies it before wgr_render_frame.
    _pendingSet.renderParams = p;
    _pendingSet.renderParamsDirty = true;
}

void EngineWgpu::PushSkyRuntime()
{
    if (!_renderer)
        return;

    // Live celestial targets from LightSun. The legacy dome placed each body at
    // camPos - astronomicalDir * range, so the view direction TOWARD it is the
    // negated astronomical direction (up by day, below the horizon at night).
    const render::frame::PresentationSnapshot& skySnap = render::frame::GPresentationSnapshot();
    Vector3 tSun(0.0f, 1.0f, 0.0f);
    Vector3 tMoon(0.0f, -1.0f, 0.0f);
    float tPhase = 0.5f;
    float tNight = 0.0f;
    if (skySnap.hasMainLight)
    {
        tSun = -skySnap.sunDirection;
        tMoon = -skySnap.moonDirection;
        tPhase = skySnap.moonPhase;
        tNight = skySnap.nightEffect;
    }
    tSun.Normalize();
    tMoon.Normalize();
    const Color tFog = FogColor();

    // Ease the coarse LightSun-driven inputs toward their live values so the sun disc +
    // horizon haze move smoothly instead of snapping every few seconds. Snap on the first
    // push and on large jumps (teleport / time-skip) to avoid a slow sweep.
    //
    // THE RATE IS A TIME CONSTANT, NOT A PER-FRAME FRACTION. This was `alpha = 0.1f`
    // applied once per frame, which makes the settling time a count of FRAMES: tau is ten
    // frames, so the sky eased over 0.33 s at 30 fps and 0.083 s at 120 fps -- four times
    // faster on a faster machine, for the same in-game event. render-snapshot-design.md
    // §3b lists this under 5.1's "previous PRESENTED frame" contract as something to fix
    // before interpolation; it is also simply wrong today, and visible as the sun disc and
    // haze reacting at different speeds on different hardware.
    //
    // kSkySmoothTau is chosen so the behaviour at 60 fps is UNCHANGED to three decimals:
    // 1 - exp(-(1/60)/0.1582) = 0.0999. A machine that was at 60 fps sees what it saw.
    //
    constexpr float kSkySmoothTau = 0.1582f;
    const auto skyNow = std::chrono::steady_clock::now();
    float alpha = 1.0f; // no previous step: snap
    if (_skySmoothAt.time_since_epoch().count() != 0)
    {
        // Capped at one second. A loading screen or an alt-tab produces a dt of many
        // seconds, and while alpha saturating to 1 there is harmless (it snaps, which is
        // what you want after a stall), the cap keeps the value finite and the intent
        // legible rather than relying on exp() underflow to do it.
        const float dt = std::min(std::chrono::duration<float>(skyNow - _skySmoothAt).count(), 1.0f);
        alpha = 1.0f - std::exp(-dt / kSkySmoothTau);
    }
    _skySmoothAt = skyNow;
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
    const float camAlt = skySnap.hasCamera ? static_cast<float>(skySnap.cameraPos.Y()) : 0.0f;
    // fog far-range: the aerial pass dissolves the terrain edge into the full sky as it nears
    // the fog/view distance, hiding the horizon colour step. 0 when there's no scene.
    const float fogFar = skySnap.hasScene ? skySnap.fogMaxRange : 0.0f;

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
    if (skySnap.hasMainLight)
    {
        moonIllum = std::clamp(skySnap.moonIllumination, 0.0f, 1.0f);
        moonRadius = std::max(skySnap.moonAngularRadius, 1.0e-5f);
        moonSunDir = skySnap.moonSunDirection;
        // Disc SURFACE radiance, not total brightness. MoonBrightness() is the empirical
        // lunar phase function — the TOTAL light the disc sends us — so dividing by the lit
        // AREA recovers the per-unit-area radiance. That division is what makes a quarter
        // moon's lit half genuinely dimmer per unit area than a full moon's, which is the
        // real opposition effect and not an artistic choice.
        const float k = std::max(moonIllum, 0.02f);
        moonDiscScale = _sky.sunIntensity * _sky.exposure * _sky.moonIntensity *
                        (skySnap.moonBrightness / k) * std::max(_sky.moonBrightnessScale, 0.0f);
        // Retire the night-vision inflation as the sun comes up. NightEffect is 0 in
        // daylight and 1 at night, so this is exactly 1.0 at night -- the night moon is
        // unchanged to the bit -- and moonDaylightScale at noon. Without it the disc kept
        // its full night radiance in a daylit sky and read as a second sun.
        const float nightFactor = std::clamp(skySnap.nightEffect, 0.0f, 1.0f);
        const float daylightFade = std::max(_sky.moonDaylightScale, 0.0f);
        moonDiscScale *= daylightFade + (1.0f - daylightFade) * nightFactor;
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
                              : static_cast<double>(render::frame::GPresentationSnapshot().timeSeconds);
    // The deck's scroll velocity. Driven by the world's wind authority when
    // cloudWindFromWeather is on, so an overcast front that stiffens the wind
    // also runs the clouds — the same number that raises the sea state, leans
    // the grass and carries the smoke.
    double windVelX = static_cast<double>(_sky.cloudWind[0]);
    double windVelZ = static_cast<double>(_sky.cloudWind[1]);
    if (_sky.cloudWindFromWeather && skySnap.windActive)
    {
        // Mean wind, not the instantaneous gust: a 3 km-wide cloud bank does not
        // accelerate on a seven-second gust, and feeding gusts here would make
        // the whole sky shudder. Aloft scale converts surface wind to cloud-base
        // wind (see cloudWindAloftScale).
        const double aloft =
            static_cast<double>(std::max(_sky.cloudWindAloftScale, 0.0f)) * skySnap.windMeanSpeed;
        windVelX = aloft * std::cos(skySnap.windMeanDirectionRad);
        windVelZ = aloft * std::sin(skySnap.windMeanDirectionRad);
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
    _pendingSet.skyRuntime = rt; // REN-THR-013: queued
    _pendingSet.skyRuntimeDirty = true;
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

// Screen-space billboard: the primitive every cloudlet is drawn with. Object::DrawDecal
// projects the puff to a screen position + rhw + half-extents and calls this; the same
// call carries the SceneDraw volume-light and flare sprites. Never went through
// DrawSection, so with the inherited EngineDummy body -- which is empty -- ALL smoke,
// dust, exhaust, muzzle clouds, water spray and light halos drew nothing under wgpu.
// The simulation was fine and the submission counts looked healthy; the quad simply
// never reached a pipeline. Same class of bug as DrawLine below.
//
// Mirrors EngineGL33::DrawDecal (EngineGL33_DrawShared.cpp): clip the quad to the
// viewport (adjusting UVs so the texture does not stretch), carry z + rhw through so
// the 2D shader depth-tests it against the world in reversed-Z, and derive the fog
// factor from IsAlphaFog exactly as GL33 packs it into TLVertex.specular. Depth is
// TESTED and not written -- smoke is occluded by a wall and never occludes anything.
bool EngineWgpu::DrawLightGlow(Vector3Par screen, float sizeX, float sizeY, ColorVal color, bool core)
{
    static const bool enabled = [] {
        const char* value = std::getenv("POSEIDON_ANALYTIC_GLOW");
        return !(value && value[0]=='0');
    }();
    if (!enabled) return false;
    if (!(sizeX > 0) || !(sizeY > 0)) return true;
    const auto packed = PackColor(PackedColor(color));
    auto vertex = [&](float x,float y,float u,float v) {
        return WgrVertex2D{{x,y,0.0f},1.0f,core ? 3.0f : 2.0f,{u,v},packed};
    };
    const auto a=vertex(screen.X()-sizeX,screen.Y()-sizeY,0,0);
    const auto b=vertex(screen.X()+sizeX,screen.Y()-sizeY,1,0);
    const auto c=vertex(screen.X()+sizeX,screen.Y()+sizeY,1,1);
    const auto d=vertex(screen.X()-sizeX,screen.Y()+sizeY,0,1);
    const WgrVertex2D quad[6]={a,b,c,a,c,d};
    // Texture zero is the white fallback, not an authored glow bitmap.
    // Clipping preserves the full quad's radial coordinates.
    AppendTriangles(0,WGR_BLEND_ADDITIVE,SamplerForSpec(ClampU | ClampV),WGR_DEPTH_NONE,quad);
    return true;
}

void EngineWgpu::DrawDecal(Vector3Par screen, float rhw, float sizeX, float sizeY, PackedColor color,
                           const MipInfo& mip, int specFlags)
{
    if (!mip.IsOK() || sizeX <= 0.0f || sizeY <= 0.0f)
    {
        return;
    }

    const float w = static_cast<float>(_w);
    const float h = static_cast<float>(_h);

    float xBeg = screen.X() - sizeX;
    float xEnd = screen.X() + sizeX;
    float yBeg = screen.Y() - sizeY;
    float yEnd = screen.Y() + sizeY;
    float uBeg = 0.0f, uEnd = 1.0f;
    float vBeg = 0.0f, vEnd = 1.0f;

    // Clip to the viewport, sliding the UVs by the clipped fraction so a puff half off
    // the left edge shows its right half at the correct scale rather than being
    // squashed into what remains. GL33 does exactly this.
    if (xBeg < 0.0f)
    {
        uBeg = -xBeg / (2.0f * sizeX);
        xBeg = 0.0f;
    }
    if (xEnd > w)
    {
        uEnd = 1.0f - (xEnd - w) / (2.0f * sizeX);
        xEnd = w;
    }
    if (yBeg < 0.0f)
    {
        vBeg = -yBeg / (2.0f * sizeY);
        yBeg = 0.0f;
    }
    if (yEnd > h)
    {
        vEnd = 1.0f - (yEnd - h) / (2.0f * sizeY);
        yEnd = h;
    }
    if (xBeg >= xEnd || yBeg >= yEnd)
    {
        return;
    }

    // Colour / fog split, as GL33 packs it into TLVertex (colour + specular alpha):
    //   IsAlphaFog: the caller has ALREADY folded fog into the alpha (see
    //     Object::DrawDecal, "colorI.SetA((1 - fog) * colorI.A())"), so pass the colour
    //     through and apply no fog blend at all -- fog = 1 keeps the colour.
    //   otherwise: the caller put (1 - visibility) in the alpha channel; the colour is
    //     drawn opaque and the alpha channel is the fog factor toward the fog colour.
    // Object::DrawDecal always sets IsAlphaFog for cloudlets, so the first branch is
    // the one smoke takes; the second exists so light halos match GL33.
    TLVertex proto;
    if (specFlags & IsAlphaFog)
    {
        proto.color = color;
        proto.specular = PackedColor(0xff000000u);
    }
    else
    {
        proto.specular = PackedColor(0xff000000u - (color & 0xff000000u));
        proto.color = PackedColor(color | 0xff000000u);
    }
    proto.rhw = rhw;
    proto.pos[2] = screen.Z();

    auto corner = [&](float x, float y, float u, float v)
    {
        TLVertex tl = proto;
        tl.pos[0] = x;
        tl.pos[1] = y;
        tl.t0.u = u;
        tl.t0.v = v;
        return MakeScreenVertex(tl);
    };

    const WgrVertex2D v0 = corner(xBeg, yBeg, uBeg, vBeg);
    const WgrVertex2D v1 = corner(xEnd, yBeg, uEnd, vBeg);
    const WgrVertex2D v2 = corner(xEnd, yEnd, uEnd, vEnd);
    const WgrVertex2D v3 = corner(xBeg, yEnd, uBeg, vEnd);
    const WgrVertex2D quad[6] = {v0, v1, v2, v0, v2, v3};

    // Not DepthForSpec(specFlags): the cloudlet specials carry NoZWrite, which
    // DepthForSpec would honour, but a decal is a screen-space sprite standing in for
    // a volume -- it must be TESTED against the world (so a wall in front hides it)
    // and must never WRITE (so it does not punch a hole in whatever is drawn behind
    // it later in the alpha pass). That is a fixed policy, not a per-spec choice.
    //
    // Flares are the exception: Scene::DrawFlare explicitly sets NoZBuf after
    // checking source visibility. Its synthetic z=1 is not a world depth, and
    // testing it (or soft-fading it) hides the flare behind every opaque surface.
    SmokeSoftParticles& soft = GSmokeSoftParticles();
    const auto policy = render::SelectDecalDepth(specFlags, soft.enabled);
    WgrDepthMode decalDepth = WGR_DEPTH_TEST;
    if (policy == render::DecalDepth::None)
        decalDepth = WGR_DEPTH_NONE;
    else if (policy == render::DecalDepth::Soft)
    {
        decalDepth = WGR_DEPTH_TEST_SOFT;
        ++soft.batches;
    }
    AppendTriangles(ResolveTexture(mip), BlendForSpec(specFlags), SamplerForSpec(specFlags), decalDepth, quad);
}

// Rotated sibling of DrawDecal: same immediate 2D batch, same depth policy
// (TESTED against the world, never writing), but the quad's long axis follows
// an explicit screen-space direction instead of the viewport axes. Rain
// streaks are the consumer -- their direction on screen IS the wind, and a
// bead chain only ever approximated it. The bounding box is clipped coarsely
// (reject, no UV slide): the rain texture is uniform noise, so a partially
// off-screen streak loses nothing by being dropped whole, unlike a puff whose
// visible half must keep its texture scale.
void EngineWgpu::DrawDecalRotated(Vector3Par screen, float rhw, float sizeX, float sizeY, PackedColor color,
                                  const MipInfo& mip, int specFlags, float dirX, float dirY, float radianceScale)
{
    if (!mip.IsOK() || sizeX <= 0.0f || sizeY <= 0.0f)
    {
        return;
    }

    const float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
    if (dirLen < 1e-4f)
    {
        // No usable direction: the axis-aligned sprite is the honest answer.
        DrawDecal(screen, rhw, sizeX, sizeY, color, mip, specFlags);
        return;
    }
    const float ax = dirX / dirLen; // long axis, along the fall
    const float ay = dirY / dirLen;
    const float px = -ay; // perpendicular, across the streak
    const float py = ax;

    const float w = static_cast<float>(_w);
    const float h = static_cast<float>(_h);
    const float extX = std::fabs(ax) * sizeY + std::fabs(px) * sizeX;
    const float extY = std::fabs(ay) * sizeY + std::fabs(py) * sizeX;
    if (screen.X() + extX < 0.0f || screen.X() - extX > w || screen.Y() + extY < 0.0f || screen.Y() - extY > h)
    {
        return;
    }

    // Colour / fog split, identical to DrawDecal's contract.
    TLVertex proto;
    if (specFlags & IsAlphaFog)
    {
        proto.color = color;
        proto.specular = PackedColor(0xff000000u);
    }
    else
    {
        proto.specular = PackedColor(0xff000000u - (color & 0xff000000u));
        proto.color = PackedColor(color | 0xff000000u);
    }
    proto.rhw = rhw;
    proto.pos[2] = screen.Z();

    auto corner = [&](float along, float across, float u, float v)
    {
        TLVertex tl = proto;
        tl.pos[0] = screen.X() + ax * along + px * across;
        tl.pos[1] = screen.Y() + ay * along + py * across;
        tl.t0.u = u;
        tl.t0.v = v;
        auto vertex = MakeScreenVertex(tl);
        // Alpha-fogged precipitation already carries its distance fade in alpha.
        // A negative fog lane transports HDR gain without quantising it to RGBA8.
        if ((specFlags & IsAlphaFog) && radianceScale != 1.0f)
            vertex.fog = -std::max(radianceScale, 1e-4f);
        return vertex;
    };

    const WgrVertex2D v0 = corner(-sizeY, -sizeX, 0.0f, 0.0f);
    const WgrVertex2D v1 = corner(+sizeY, -sizeX, 1.0f, 0.0f);
    const WgrVertex2D v2 = corner(+sizeY, +sizeX, 1.0f, 1.0f);
    const WgrVertex2D v3 = corner(-sizeY, +sizeX, 0.0f, 1.0f);
    const WgrVertex2D quad[6] = {v0, v1, v2, v0, v2, v3};

    const WgrDepthMode decalDepth = render::SelectDecalDepth(specFlags, false) == render::DecalDepth::None
                                       ? WGR_DEPTH_NONE : WGR_DEPTH_TEST;
    AppendTriangles(ResolveTexture(mip), BlendForSpec(specFlags), SamplerForSpec(specFlags), decalDepth, quad);
}

// 3D line primitive: Object::DrawLines projects the shape, then submits every edge
// through here (Object.cpp, GEngine->DrawLine). It never goes through DrawSection, so
// with the inherited EngineDummy body -- which is empty -- the whole world-space
// diagnostic line family drew nothing under wgpu: ballistics trails, tracers, AI path
// debug, DrawDiagLine / DiagBBox, the viewer floor grid, helicopter debug rects.
//
// Mirrors EngineGL33::DrawLine (EngineGL33_2D.cpp): expand the screen-space segment
// into a 3px-wide quad along its perpendicular, texture it with the preloaded line
// texture, and submit it as a depth-TESTED, non-depth-writing 2D batch -- occluded by
// the world, never occluding it, which is what a diagnostic trail wants.
//
// Object::DrawLines does NOT call PrepareTriangle, so _swTexture / _swBlend /
// _swSampler / _swDepth still hold the previous DrawSection's material here. Every
// piece of state below is therefore resolved locally; reusing the _sw* fields would
// paint the line with whatever texture the last section happened to leave behind.
void EngineWgpu::DrawLine(int beg, int end)
{
    if (!_swMesh || !_renderer)
    {
        return;
    }

    const TLVertex& tv0 = _swMesh->GetVertex(beg);
    const TLVertex& tv1 = _swMesh->GetVertex(end);

    Texture* texture = GPreloadedTextures.New(TextureLine);
    const MipInfo& mip = TextBank()->UseMipmap(texture, 1, 1);
    if (!mip.IsOK())
    {
        return;
    }

    // Same spec GL33 passes: alpha-blended, clamped, and NoZWrite -> WGR_DEPTH_TEST.
    const int specFlags = NoZWrite | IsAlpha | ClampU | ClampV | IsAlphaFog;

    float x0 = tv0.pos[0], y0 = tv0.pos[1];
    float x1 = tv1.pos[0], y1 = tv1.pos[1];
    const float z0 = tv0.pos[2], z1 = tv1.pos[2];
    const float w0 = tv0.rhw, w1 = tv1.rhw;

    const float dx = x1 - x0, dy = y1 - y0;
    const float dSize2 = dx * dx + dy * dy;
    const float invDSize = dSize2 > 0 ? InvSqrt(dSize2) : 1.0f;
    const float dSize = dSize2 * invDSize;

    // Direction perpendicular to (dx,dy); the line is a quad of this fixed pixel width.
    const float pdx = +dy * invDSize, pdy = -dx * invDSize;
    const float lineW = 3.0f;
    x0 -= pdx * (lineW * 0.5f);
    x1 -= pdx * (lineW * 0.5f);
    y0 -= pdy * (lineW * 0.5f);
    y1 -= pdy * (lineW * 0.5f);
    const float x0Side = x0 + pdx * lineW, y0Side = y0 + pdy * lineW;
    const float x1Side = x1 + pdx * lineW, y1Side = y1 + pdy * lineW;

    Vertex2DAbs vertices[4];
    vertices[0].x = x0, vertices[0].y = y0, vertices[0].z = z0, vertices[0].w = w0;
    vertices[0].u = 0, vertices[0].v = 0.25f, vertices[0].color = tv0.color;

    vertices[1].x = x0Side, vertices[1].y = y0Side, vertices[1].z = z0, vertices[1].w = w0;
    vertices[1].u = 0, vertices[1].v = 1, vertices[1].color = tv0.color;

    vertices[2].x = x1Side, vertices[2].y = y1Side, vertices[2].z = z1, vertices[2].w = w1;
    vertices[2].u = dSize, vertices[2].v = 1, vertices[2].color = tv1.color;

    vertices[3].x = x1, vertices[3].y = y1, vertices[3].z = z1, vertices[3].w = w1;
    vertices[3].u = dSize, vertices[3].v = 0.25f, vertices[3].color = tv1.color;

    // Clipped here rather than through DrawPoly: the 2D DrawPoly flattens z/rhw and
    // forces WGR_DEPTH_NONE, which would put the trail in front of the whole world.
    // Clip2D interpolates z, w, u, v and colour, so depth survives the clip.
    constexpr int maxN = 16;
    Vertex2DAbs scratch1[maxN];
    Vertex2DAbs scratch2[maxN];
    int n = 4;
    const Rect2DAbs clip(0, 0, static_cast<float>(_w), static_cast<float>(_h));
    const Vertex2DAbs* clipped = ClipPoly2D(vertices, n, clip, scratch1, scratch2);
    if (!clipped || n < 3)
    {
        return;
    }
    if (n > maxN)
    {
        n = maxN;
    }

    // fog = 1: unfogged, matching GL33 stuffing specular 0xff000000 into the screen vertex.
    const auto conv = [](const Vertex2DAbs& v)
    { return WgrVertex2D{{v.x, v.y, v.z}, v.w, 1.0f, {v.u, v.v}, PackColor(v.color)}; };

    std::vector<WgrVertex2D> tris;
    tris.reserve(static_cast<size_t>(n - 2) * 3);
    for (int i = 1; i + 1 < n; i++)
    {
        tris.push_back(conv(clipped[0]));
        tris.push_back(conv(clipped[i]));
        tris.push_back(conv(clipped[i + 1]));
    }

    AppendTriangles(ResolveTexture(mip), BlendForSpec(specFlags), SamplerForSpec(specFlags), DepthForSpec(specFlags),
                    tris);
}

// 3D point primitive (stars, laser dots): Object::DrawPoints projects the shape and
// submits the whole vertex range here. Unlike DrawLines it DOES call PrepareTriangle
// first, so the _sw* material captured there is the right state to draw with.
//
// Mirrors EngineGL33::DrawPoints (EngineGL33_DrawShared.cpp): each point becomes a
// 2x2-pixel quad whose corner alphas carry the sub-pixel position, which is what makes
// a one-pixel star fade smoothly instead of snapping between pixels.
void EngineWgpu::DrawPoints(int beg, int end)
{
    if (!_swMesh || !_renderer)
    {
        return;
    }

    const auto fracAlpha = [](float a)
    {
        int ia = toInt(a);
        saturate(ia, 0, 255);
        return ia;
    };

    std::vector<WgrVertex2D> tris;
    for (int i = beg; i < end; i++)
    {
        if (_swMesh->Clip(i) & ClipAll)
        {
            continue;
        }
        const TLVertex& v = _swMesh->GetVertex(i);
        const PackedColor color = v.color;
        if (color.A8() < 8)
        {
            continue; // do not draw stars that are not visible
        }

        const int xI = toIntFloor(v.pos[0]);
        const int yI = toIntFloor(v.pos[1]);
        const float xFrac = v.pos[0] - xI;
        const float yFrac = v.pos[1] - yI;
        const float ixFrac = 1 - xFrac;
        const float iyFrac = 1 - yFrac;
        const float a = color.A8();

        TLVertex vs[4];
        vs[0] = v; // TL
        vs[0].pos[0] = xI + 0.5f;
        vs[0].pos[1] = yI + 0.5f;
        vs[0].color = PackedColorRGB(color, fracAlpha(ixFrac * iyFrac * a));
        // Screen-space points must not be fogged: MakeScreenVertex reads the fog blend
        // from specular alpha, so 0xff000000 = keep colour (GL33 parity).
        vs[0].specular = PackedColor(0xff000000);

        vs[1] = vs[0]; // TR
        vs[1].pos[0] = xI + 2.5f;
        vs[1].color = PackedColorRGB(color, fracAlpha(xFrac * iyFrac * a));

        vs[2] = vs[1]; // BR
        vs[2].pos[1] = yI + 2.5f;
        vs[2].color = PackedColorRGB(color, fracAlpha(xFrac * yFrac * a));

        vs[3] = vs[2]; // BL
        vs[3].pos[0] = vs[0].pos[0];
        vs[3].color = PackedColorRGB(color, fracAlpha(ixFrac * yFrac * a));

        const WgrVertex2D tl = MakeScreenVertex(vs[0]);
        const WgrVertex2D br = MakeScreenVertex(vs[2]);
        tris.push_back(tl);
        tris.push_back(MakeScreenVertex(vs[1]));
        tris.push_back(br);
        tris.push_back(tl);
        tris.push_back(br);
        tris.push_back(MakeScreenVertex(vs[3]));
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
