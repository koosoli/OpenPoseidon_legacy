#include <Poseidon/Graphics/Textures/ColdPaaHandoff.hpp>
#include "TextureWgpu.hpp"
#include "TextureBankWgpu.hpp"
#include "EngineWgpu.hpp" // ProducerMayTouchRenderer (REN-THR-012)

#include <Poseidon/Graphics/Core/MipmapLayout.hpp>
#include <Poseidon/Graphics/Textures/BlockCompression.hpp>
#include <Poseidon/Graphics/Textures/AlphaShapeAnalysis.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp> // IsEnfusionRgNormalName (RFG-047)
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/NativeNormalChannels.hpp>
#include <Poseidon/Graphics/Textures/HotSourceProofRetirementPolicy.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp> // SkipLayerTint (RFG-070)
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>     // worker-prepared mip chains
#include <Poseidon/Graphics/Textures/ProceduralTexture.hpp>
#include <Poseidon/Graphics/Textures/DeferredProceduralAdmission.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ObjectAdmitProfile.hpp> // per-admission split timers
#include <Poseidon/World/Terrain/ObjectTextureUploadTrace.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp> // shared streaming counters (panel + capture)
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/World/Terrain/WarmTextureProvenance.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/IO/Streams/RetailRigidAssetProfile.hpp>

#include <Poseidon/Graphics/Textures/Bc3Encoder.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/MuzzleFlashAppearance.hpp>


#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Poseidon
{

int TextureMipBias(); // shared with the ordinary block upload below

namespace
{

uint64_t NowUs();

// Diagnostic-only, cached startup option shared with terrain page rebuilds.
// Source family matches TerrainWgpu's page-eviction classifier; no policy use.
bool TerrainPageUploadTimingsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_TERRAIN_PAGE_TIMINGS");
        return value && value[0] == '1';
    }();
    return enabled;
}
bool TerrainColdUploadTimingsEnabled(const char* name)
{
    if (!TerrainPageUploadTimingsEnabled() || !name) return false;
    for (const char* at = name; *at; ++at)
        if (strnicmp(at, "\\layers\\", 8) == 0 || strnicmp(at, "/layers/", 8) == 0 ||
            strnicmp(at, "_lca.", 5) == 0 || strnicmp(at, "_lco.", 5) == 0) return true;
    return false;
}
struct TerrainColdUploadTiming
{
    bool enabled;
    const char* source;
    uint64_t began;
    double preparedTakeMs = 0.0, blockReadMs = 0.0, compressedCreateOwnerMs = 0.0;
    double fallbackReadDecodeMs = 0.0, fallbackCreateOwnerMs = 0.0, fallbackOwnerMs = 0.0;
    bool preparedTaken = false, preparedUsed = false, uploaded = false;
    explicit TerrainColdUploadTiming(const char* name)
        : enabled(TerrainColdUploadTimingsEnabled(name)), source(name), began(enabled ? NowUs() : 0) {}
    ~TerrainColdUploadTiming()
    {
        if (!enabled) return;
        const double totalMs = (NowUs() - began) / 1000.0;
        if (totalMs < 2.0) return;
        // First 128 slow cold page uploads per process, not a top-N ranking.
        // Saturation avoids any unbounded counter wrap/log flood.
        static std::atomic<unsigned> logged{0};
        unsigned slot = logged.load(std::memory_order_relaxed);
        while (slot < 129 && !logged.compare_exchange_weak(slot, slot + 1, std::memory_order_relaxed)) {}
        if (slot == 128)
            LOG_INFO(Graphics, "Wgpu terrain cold upload timings truncated: limit=128 thresholdMs=2");
        if (slot >= 128) return;
        LOG_INFO(Graphics,
            "Wgpu terrain cold upload timing: source={} preparedTaken={} preparedUsed={} uploaded={} "
            "preparedTakeMs={:.3f} blockReadOwnerMs={:.3f} compressedCreateOwnerMs={:.3f} "
            "fallbackReadDecodeMs={:.3f} fallbackCreateOwnerMs={:.3f} fallbackOwnerMs={:.3f} totalOwnerMs={:.3f}",
            source, preparedTaken, preparedUsed, uploaded, preparedTakeMs, blockReadMs,
            compressedCreateOwnerMs, fallbackReadDecodeMs, fallbackCreateOwnerMs, fallbackOwnerMs, totalMs);
    }
};


// No clock reads outside an existing admission profile. Stop permits consecutive
// phases without extending a bucket over later work; destruction covers failures.
class AlphaProfilePhase
{
    double* _bucket;
    uint64_t _began;
public:
    explicit AlphaProfilePhase(double render::ObjectAdmitProfile::* bucket)
        : _bucket(render::GObjectAdmitProfile.active ? &(render::GObjectAdmitProfile.*bucket) : nullptr),
          _began(_bucket ? NowUs() : 0) {}
    ~AlphaProfilePhase() { Stop(); }
    void Stop()
    {
        if (_bucket)
        {
            *_bucket += (NowUs() - _began) / 1000.0;
            _bucket = nullptr;
        }
    }
};

bool IsPaaName(const char* name)
{
    const char* ext = name ? strrchr(name, '.') : nullptr;
    return ext && strcmpi(ext, ".paa") == 0;
}

PacFormat BasicFormat(const char* name)
{
    return IsPaaName(name) ? PacARGB4444 : PacARGB1555;
}

PacFormat DstFormat(PacFormat srcFormat)
{
    switch (srcFormat)
    {
        case PacP8:
            return PacARGB1555;
        default:
            return srcFormat;
    }
}

int BcFormatFor(PacFormat fmt)
{
    switch (fmt)
    {
        case PacDXT1:
            return WGR_TEXTURE_BC1;
        case PacDXT2:
        case PacDXT3:
            return WGR_TEXTURE_BC2;
        case PacDXT4:
        case PacDXT5:
            return WGR_TEXTURE_BC3;
        // RFG-047. These three reach here only from a DDS/EDDS source with the compressed
        // pass-through on; no PAA can name them. Everything downstream in EnsureUploaded is
        // format-agnostic -- the sizes come from render::mipmap::ComputeLayout, which knows
        // their block sizes -- so adding them here is the whole of the upload change.
        case PacBC4:
            return WGR_TEXTURE_BC4;
        case PacBC5:
            return WGR_TEXTURE_BC5;
        case PacBC7:
            return WGR_TEXTURE_BC7;
        default:
            return -1;
    }
}

// The PAA magic of a block-compressed file, as the worker-side chain reader reports it, back
// to the PacFormat the bank's header parse produced for the same file (PacFormatFromDesc's
// DXT arm). Only the DXT magics: a prepared chain is DXT by construction.
PacFormat PacFormatFromMagic(uint16_t magic)
{
    switch (magic)
    {
        case 0xFF01:
            return PacDXT1;
        case 0xFF02:
            return PacDXT2;
        case 0xFF03:
            return PacDXT3;
        case 0xFF04:
            return PacDXT4;
        case 0xFF05:
            return PacDXT5;
        default:
            return PacFormatN;
    }
}

// ---------------------------------------------------------------------------------------
// MAT-049 — the alpha SHAPE guard: where a texel's alpha sits, not just how much of it.
//
// `ClassifyAlpha` (Graphics/Textures/PAADecoder.cpp) is a pure histogram: >= 2% partial
// alpha => Blend, else >= 2% clear alpha => Cutout, else Opaque.  It has one structural
// escape hatch — the all-clear (aMax == 0) demotion — and its own comment names what is
// missing: "separating packed detail from an aggressive foliage mask needs a real
// perimeter/speckle metric and a policy decision, not a guard."  This is that metric.
//
// WHY IT SUDDENLY MATTERS.  On CWA the histogram was never consulted: every alpha-bearing
// texture in the retail corpus is DXT1, ARGB4444, AI88 or P8, and `ClassifyTextureAlpha`
// short-circuits 1-bit alpha to Cutout without decoding anything (MAT-048's census: 5,397
// DXT1, exactly ONE DXT5 in 6,976 textures).  The imported corpora invert that.  DayZ and
// Reforger are BC3-dominated, `TextureSourceDDS::Init` sets `_hasAlpha` from the FORMAT
// alone (`DdsImport.cpp:135` — DXT3/DXT5 => true, unconditionally), and until the block
// scan landed the old whole-file path could not decode a DDS at all and returned a default
// `AlphaStats` whose kind is Opaque.  So every BC2/BC3 texture in those worlds classified
// Opaque by accident, and now classifies by content.  The classifier is being asked
// questions it has never been asked before, and the two answers it gets wrong are:
//
//   1. DITHERED PACKED ALPHA read as a cutout mask.  `_BCR`-family rock textures carry
//      detail, not coverage, in alpha.  `granite_05_bcr_ca` is 24.1% alpha-zero scattered
//      as a dither, which the histogram calls Cutout; the draw path then forces a hard
//      discard (EngineWgpu.cpp:2977-2987, `alphaRef = max(alphaRef, 0.5)`) and punches a
//      quarter of the rock's texels out.  The owner's "stones are transparent".
//
//   2. AN ANTI-ALIASED MASK read as translucency.  A cutout authored with soft edges is
//      mostly partial alpha AT those edges — the shipped Arma 3 pine leaf is 30.3% partial,
//      the Enoch birch 5.8%, and 44 of 50 `plants_f` `_ca` textures trip the 2% floor
//      (ShapeDraw.cpp:67-75, which has complained about exactly this since it was written;
//      the DayZ leaf cards measure 11.7-12.2% partial).  Blend is not a shading tweak:
//      `SectionIsBlend` (ShapeDraw.cpp:156) pulls the whole section OUT of the opaque pass
//      into `Scene::DrawObjectsAndShadowsPass2`'s back-to-front pass, which sorts per OBJECT
//      by far extent, does not sort sections within an object at all, and (since MAT-048)
//      does not write depth.  A section that lands there stops occluding, stops casting, and
//      stops being eligible for the GPU-driven retained path.
//
// Note that (2) is INVISIBLE to a pass-descriptor census.  `BuildRenderPassDescriptor` sees
// only the section SPEC, so it will keep reporting `PassKind::WorldCutout` for a section that
// `SectionIsBlend` is simultaneously routing to the blend pass.  Measuring the descriptor
// does not exclude the alpha class, and a census that stops at the descriptor has not.
//
// WHAT THIS IS *NOT*, recorded so it is not re-proposed: the DayZ rail track "piano" banding.
// That was the leading candidate for (2) and the census killed it.  All 57 `Rail_Track*.p3d`
// reference PAA, not EDDS, so the block scan cannot reach them; their only albedo with any
// transparency is `embankment_co.paa`, which is DXT1 (1-bit, short-circuited to Cutout
// without a decode) and 1.47% clear — below the 2% floor — and whose clear texels are 94%
// one contiguous full-width strip of dead atlas gutter, not a silhouette.  Nor is the
// 0xc0 alpha-test reference implicated: the count of texels in the 128..192 band that a
// threshold of 192 discards and 128 keeps is 0.000% on that texture, and 0 across all 133
// converted Reforger `_ca.paa`.  Both suspects are excluded by measurement; the banding is
// geometry or lighting, and WLD-021's three-sun-angle sampling is the test that decides it.
//
// THE TWO METRICS.  Both are one extra pass over the same top-mip alpha the histogram just
// walked, and both are run only when they could change the verdict.
//
//   * CONTIGUITY, for (1).  The MEAN number of a texel's four orthogonal neighbours that are
//     also clear, averaged over the clear texels (0..4; out of bounds counts as not-clear).
//     A real mask's holes are contiguous, so most clear texels are hole interior and score
//     near 4.  A dither is isolated texels by construction and scores low.
//
//     The threshold is not guessed, it is calibrated against a census of the actual corpora
//     (2026-08-16).  Over ALL 133 converted Reforger `*_ca.paa`: p5 = 3.483, median = 3.894,
//     and 129 of 133 score >= 3.0.  Adding BI-authored DayZ cutouts as controls:
//
//       chain-link `metal_fence_wire_ca`  3.959     `granite_05_bcr_ca`        2.245
//       wire mesh  `wire_protection_ca`   3.935     `anthill_01_bcr_ca`        2.729
//       leaf card  `t_betula2w_1_ca`      3.860     `st_metal_rust_rough01_..` 1.941
//       railing    `woodenfence_01_..`    3.388     `st_plaster_bare_01_bcr_ca`1.169
//       worst genuine cutout in corpus    3.267
//
//     Those are the ONLY four textures in the corpus below 3.0, and all four are `_BCR`
//     textures whose alpha is a conversion artefact (see the note below).  3.0 therefore
//     separates every genuine cutout from every artefact with ~1.0 of margin on a 0..4 scale.
//
//     WHY THE GRANITE ALPHA EXISTS AT ALL, since it decides whether this guard is a
//     workaround or the fix.  The Reforger source `Granite_05_BCR.edds` has NO clear texel:
//     alpha min 101, mean 239, and `_BCR` means base-colour + roughness, so that channel is
//     roughness.  The asset ships no `_A` opacity map.  The converter nevertheless emitted a
//     PAA whose alpha is 24.1% zero, and that alpha is uncorrelated with the source — mean
//     source alpha is 238.8 under the texels it marked clear and 239.0 under the texels it
//     marked opaque, i.e. indistinguishable.  It is noise.  The real fix is in the converter
//     (`_BCR` alpha must not become an opacity mask when no `_A` map exists); this guard is
//     the renderer refusing to act on it, which is worth having anyway because the installed
//     tree is already full of such files.  MAT-047's FLAG-tagg patch is what made them
//     visible: before it, granite had no taggs, `hasAlpha` was false and the pixels were
//     never examined.
//
//   * BAND WIDTH, for (2).  Count the mask perimeter P (adjacent clear/non-clear texel
//     pairs) and divide the partial-texel count by it.  That is the mean thickness, in
//     texels, of the partial-alpha band.  An anti-aliased edge is 1-3 texels of gradient
//     wrapped around a long perimeter, so the ratio is small.  Genuine translucency is an
//     AREA of partial alpha, not a rim: cockpit glass measures ~18% mean alpha across
//     essentially every texel with no holes at all (MAT-048 read TAGG AVGC 46/255 off
//     `apach_in_skla.paa`), so its perimeter is zero and the ratio is unbounded.
//
// GLASS IS PROTECTED TWICE, deliberately, because demoting a cockpit pane to a hard alpha
// test would be a far worse bug than the one being fixed: the Blend demotion requires
// >= 2% CLEAR texels, and then requires the partial band to be thin.  Glass fails both —
// it is partial everywhere with no holes to have a perimeter around.  Measured on the
// corpus: DayZ's `window_set_ca.paa` is 0.00% clear / 98.62% partial, and CWA's Apache
// `apach_in_skla.paa` is TAGG AVGC 46/255 across the pane.  Either test alone keeps them
// blending; neither is ever reached, because the `pctClear >= 2.0` gate returns first.
//
// The rule is applied HERE, in the wgpu texture, rather than in `ClassifyAlpha` — that
// function is shared with GL33 and with the offline tools, and this is a renderer routing
// policy, not a truth about the file.  `WGR_ALPHA_SHAPE_GUARD=0` disables it wholesale;
// both thresholds are separately overridable so a calibration run needs no rebuild.
using AlphaShape = AlphaShapeAnalysis;

double EnvDouble(const char* key, double fallback)
{
    const char* v = std::getenv(key);
    if (!v || !*v)
    {
        return fallback;
    }
    char* end = nullptr;
    const double parsed = std::strtod(v, &end);
    return (end && end != v) ? parsed : fallback;
}

bool AlphaShapeGuardEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_ALPHA_SHAPE_GUARD");
        return !v || v[0] != '0';
    }();
    return enabled;
}

// The Blend -> Cutout half is DEFAULT OFF, and the asymmetry is deliberate.
//
// The Cutout -> Opaque half fixes a reported bug (the perforated granite) on four measured
// textures, in the direction that keeps a surface visible; it is on.  This half fixes a bug
// nobody reported.  It is real — ShapeDraw.cpp:67-88 has described its symptoms (no shadow,
// accumulating canopy, order-dependent dark patches) since it was written, and
// `SectionMaterialAlphaRoute` already applies exactly this demotion to the TreeAdv families
// by RVMAT — but by histogram it would reach ALL of them at once: the DayZ leaf cards
// measure 76.8% clear / 11.7% partial with a band width near 1, so every one flips pass on
// the first run.  Moving all foliage from the back-to-front pass into the opaque pass with a
// hard 0.5 discard is a large visual change that wants its own before/after capture, not a
// side effect of a stones fix.
//
// `WGR_ALPHA_MASK_DEMOTE=1` turns it on.  The counter and the trace below report it either
// way, so a run can measure how many textures it WOULD move before anyone enables it.
bool AlphaMaskDemoteEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_ALPHA_MASK_DEMOTE");
        return v && v[0] != '0';
    }();
    return enabled;
}

// Pure pixel analysis; policy and its counters remain in the renderer below.
AlphaShape MeasureAlphaShape(const uint8_t* rgba, int w, int h, size_t stride, size_t offset)
{
    return MeasureAlphaShapeAnalysis(rgba, w, h, stride, offset);
}

// How often each demotion fired, reported alongside the streaming stats.  A run in which
// these are both zero has not exercised the guard, which is the thing a null A/B most needs
// to be able to say about itself (MAT-048's lesson: a lever that cannot fire produces a null
// that means nothing).
std::atomic<uint64_t> GAlphaSpeckleDemotions{0}; // Cutout -> Opaque (dithered packed alpha)
std::atomic<uint64_t> GAlphaMaskDemotions{0};    // Blend  -> Cutout (anti-aliased mask)

bool AlphaShapeTraceEnabled()
{
    static const bool enabled = std::getenv("WGR_ALPHA_SHAPE_TRACE") != nullptr;
    return enabled;
}

// Applied to every freshly computed `AlphaStats`, on both scan paths, so the block scan and
// the whole-file decode cannot disagree about routing.  Returns the shape it measured (or an
// unmeasured one when the histogram verdict was never in question) purely so callers can log
// it; the verdict is written straight into `stats.kind`.
AlphaShape RefineAlphaClassByShape(AlphaStats& stats, const uint8_t* rgba, int w, int h, const char* name,
                                  size_t stride = 4, size_t offset = 3, const AlphaShape* prepared = nullptr)
{
    AlphaShape shape;
    if (!AlphaShapeGuardEnabled())
    {
        return shape;
    }

    static const double neighboursMin = EnvDouble("WGR_ALPHA_NEIGHBOURS_MIN", 3.0);
    static const double bandWidthMax = EnvDouble("WGR_ALPHA_BAND_MAX", 4.0);

    const bool couldBeSpeckle = stats.kind == AlphaStats::Cutout && stats.pctClear > 0.0;
    const bool couldBeAntialiasedMask = stats.kind == AlphaStats::Blend && stats.pctClear >= 2.0;
    if (!couldBeSpeckle && !couldBeAntialiasedMask)
    {
        return shape; // glass, solid colour, and every already-Opaque texture exit here
    }

    shape = prepared ? *prepared : MeasureAlphaShape(rgba, w, h, stride, offset);
    if (!shape.measured)
    {
        return shape;
    }

    const AlphaStats::Kind before = stats.kind;
    if (couldBeSpeckle && shape.meanClearNeighbours < neighboursMin)
    {
        // Dispersed clear texels: packed data in the alpha lane, not coverage.  Opaque is
        // the safe failure for the same reason ClassifyAlpha's all-clear guard says it is —
        // a wrongly-opaque surface looks wrong, a wrongly-perforated one looks broken.
        stats.kind = AlphaStats::Opaque;
        GAlphaSpeckleDemotions.fetch_add(1, std::memory_order_relaxed);
    }
    else if (couldBeAntialiasedMask && shape.partialBandWidth <= bandWidthMax &&
             shape.meanClearNeighbours >= neighboursMin)
    {
        // Counted whether or not it is applied, so a run can size this change before taking
        // it (see `AlphaMaskDemoteEnabled`).
        GAlphaMaskDemotions.fetch_add(1, std::memory_order_relaxed);
        // The contiguity test is required on THIS branch too, and it is not symmetry for its
        // own sake.  A bark colour map carries a non-coverage alpha channel (the Stratis
        // pinus nigra bark classifies Blend) and can trip the 2%-clear floor with scattered
        // near-zero texels.  Demoting it to Cutout would hand it to the draw path's
        // `alphaClass == Cutout` branch (EngineWgpu.cpp:2977), which raises alphaRef to 0.5 —
        // and a hard discard on bark removes the trunk, the exact failure ShapeDraw.cpp's
        // TreeAdvTrunk note exists to prevent.  Requiring the clear texels to form real,
        // contiguous holes keeps scattered packed alpha out of the mask class entirely.
        // A thin partial rim around real holes is an anti-aliased cutout.  Cutout keeps the
        // section in the opaque pass, where it occludes, depth-writes, casts a shadow and is
        // eligible for the GPU-driven retained path — and MSAA alpha-to-coverage still
        // anti-aliases the edge the author drew.
        if (AlphaMaskDemoteEnabled())
        {
            stats.kind = AlphaStats::Cutout;
        }
    }
    if (AlphaShapeTraceEnabled())
    {
        LOG_INFO(Graphics,
                 "Wgpu alpha shape: {} {}x{} clear={:.1f}% partial={:.1f}% clearNb={:.3f} clustered={:.3f} "
                 "bandWidth={:.2f} {} -> {}",
                 name ? name : "<unnamed>", w, h, stats.pctClear, stats.pctPartial, shape.meanClearNeighbours,
                 shape.clusteredClearFrac, shape.partialBandWidth, AlphaKindName(before), AlphaKindName(stats.kind));
    }
    return shape;
}

// ---------------------------------------------------------------------------------------
// Format-forced alpha: the translucent cockpit BODY (owner report, 2026-08-16).
//
// `TextureWgpu::Init` calls `ForceAlpha()` for every ARGB4444 / AI88 / ARGB8888 texture, so
// `hasAlpha` is true for all of them and `GetAlphaClass` decodes and consults the histogram
// even when the file carries no FLAG tagg — the only authored alpha signal (MAT-047). The
// histogram then applies its >= 2%-partial rule, and a solid surface with a soft seam or a
// couple of anti-aliased decal edges is called Blend. Blend is not a shading tweak: the
// section leaves the opaque pass for the back-to-front pass, which sorts per OBJECT, not per
// section, and (since MAT-048) does not write depth — so a cockpit whose panels all land
// there composites in mesh order and the far wall shows through the near door.
//
// MEASURED on the retail install (2026-08-16; every ARGB4444/AI88 PAA in AddOns/*.pbo and
// DTA/*.pbo decoded — 762 ARGB4444, 731 AI88, matching MAT-048's format census exactly).
// Not one AI88 file carries any tagg; 31 of the ARGB4444 lack FLAG. Of those 762 no-FLAG
// files, 641 classify Blend by histogram. The Kiowa (oh58.pbo), which is the repro:
//
//                        clear%  partial%  mid%(16..239)  mean   verdict wanted
//   kiowa_inspanl        9.7      2.2       1.4           227    body  -> Cutout
//   kiowa_insidtga      50.6      4.2       2.6           122    body  -> Cutout
//   kiowa_expanel        9.5      5.9       4.5           221    body  -> Cutout
//   kiowa_inspanr       10.0      7.3       5.5           215    body  -> Cutout
//   kiowa_in_doors       0.0      0.0       0.0           255    (already Opaque)
//   oh58_sklotest        0.0    100.0     100.0           158    glass -> Blend
//   oh58_sklotestl       0.0    100.0      91.9            37    glass -> Blend (tinted)
//   oh58_sklotest_c     53.3     46.7      45.8            79    glass -> Blend (pane + frame)
//   oh58_sklotestl_c    54.7     45.3      43.4            26    glass -> Blend
//
// (An earlier handover quoted the body textures at "mean alpha 10-23/255"; that was wrong —
// they are 122-255. They are not faint, they are solid with a few soft texels, which is why
// Opaque/Cutout is the right class and blending at their real alpha merely looked opaque in
// the original engine, whose alpha pass wrote depth.)
//
// The two populations do not overlap on the MID band. Corpus-wide, among no-FLAG textures
// with no glass token in the name, the highest "solid" surfaces are the Cobra dashboards
// (cobra_palubka_tl/tr/br/bl: 9.9-19.7% mid, 0% clear, mean 217-235) and uh60_pilot_palubka1
// (18.3% mid, 36.2% partial); the lowest genuinely translucent surfaces are
// ah-1_kabina_predo (34.8% mid, 51.7% partial), lldr_monitor (70.8%), uh60_cargo_dvereo
// (91.7%), zsu_dri_pruzor2 (96.9%). 25% mid sits between them with ~5 points of margin on
// each side, and >= 50% partial is a second, independent escape for gradients whose partial
// texels sit near 0 or 255 (a majority-partial texture cannot be a solid body).
//
// Mean alpha is deliberately NOT a criterion: tinted glass has mean 26-37 (oh58_sklotestl,
// sk_predni_sklo, sklo.paa) while the body sits at 122-255, so a mean floor would keep the
// wrong side.
//
// THE RULE, applied only when the alpha was forced by the FORMAT (no FLAG tagg): demote
//
//   Blend -> (pctClear >= 2 ? Cutout : Opaque)
//
// iff ALL of
//   pctMid     <  25   (WGR_FORMAT_ALPHA_MID_MIN)      not translucent by area
//   pctPartial <  50   (WGR_FORMAT_ALPHA_PARTIAL_MIN)  not a majority-partial gradient
//   pctOpaque  >= 40   (WGR_FORMAT_ALPHA_OPAQUE_MIN)   a solid body IS mostly a==255
//   no glass token in the base name
//
// i.e. only a texture that is demonstrably a solid surface with fringe partials is pulled
// back; everything ambiguous keeps the histogram's Blend, because a wrongly-opaque pane is a
// worse failure than a wrongly-ordered body (the pre-regression engine drew these bodies
// blended for 25 years and nobody noticed -- with per-object sorting they merely look
// opaque-ish; an opaque windscreen is unmissable). The pctOpaque floor is what keeps
// effects sheets out of it: the fired.* muzzle flashes (52-72% partial), mrak_war_* war
// clouds, night_sum.* halos, kuzel_svetla light cones, lightcircle, halo, obrshadow/
// krizshadow blob shadows and slunce (the sun sprite) all have pctOpaque well under 40 and
// stay Blend. Demotion count on the full corpus with these values: 86 of 641 no-FLAG
// histogram-Blend textures (63 -> Cutout, 23 -> Opaque); the list is cockpit interior
// sheets, weapon-sight reticle housings, dashboards, 2D map/HUD icons (which never consult
// GetAlphaClass), and mesic.01-12 -- the moon-phase sprites, disc 46.7% opaque inside a
// 48.1% clear surround with a 5.2% partial rim, for which Cutout is arguably the authored
// shape anyway (its soft halo is a separate texture, halo.paa, which stays Blend).
//
// FLAG-tagged textures are untouched: the authored bit is trusted (MAT-047), so ARGB4444
// glass with FLAG=1 (kiowa_topsklo, apach_in_skla) never reaches this.
//
// Glass tokens (OFP is Czech-named): "sklo" (glass; sklotest, topsklo, kab_sklo, notasskloa..f,
// scud_sklo*), "skla" (genitive: apach_in_skla, jeep_ciferniky_skla) — excluding "sklad"
// (warehouse), "okno"/"okna" (window: kabina_topokno*, gunner_okna*, m60_com_okna), "glass"
// (jepp4x4_glass*, ian_doorglass). Verified against every file name in the retail PBOs: the
// only near-misses are "prasklina" (crack — not matched, "skli") and the "okn?" building
// windows in O.pbo/Hous, all FLAG-tagged so out of scope. The token exists for panes whose
// histogram is dominated by their FRAME cutout — ah-1_kabina_zadoknoc (23.2% mid, 49.7%
// clear), scud_sklo4 (1.2% mid, 64.9% clear), uh60_gunner_oknac (16.6% mid) — which the
// numbers alone would demote.
//
// WGR_COCKPIT_FORCEALPHA_LEGACY=1 disables the rule (today's behaviour) for the A/B.
// WGR_ALPHA_SHAPE_TRACE=1 logs every demotion; the count is in the stream-stats line.
std::atomic<uint64_t> GFormatAlphaDemotions{0}; // Blend -> Cutout/Opaque (format-forced alpha)

bool CockpitForceAlphaLegacyEnabled()
{
    // Default ON since MAT-052 (2026-08-26): the demotion below existed to fix
    // "cockpit bodies translucent on WGPU" -- which turned out to be the cockpit
    // near-clipping (7e9693c4) and the god-ray stale-depth composite (93e66bd2),
    // both misattributed to alpha routing. With those fixed, the demotion's only
    // measured effect was to turn authored per-region alpha OPAQUE: the AH-1 and
    // Mi-17 instrument COVER GLASS shares its AI88 texture with the solid side
    // panels (alpha 255 on the panels, partial on the lids), and forcing the
    // texture Opaque sealed every gauge under a dark lid while the dials sat
    // intact underneath. Exterior line-up measured unchanged with the policy off
    // (diff = grass sway only). WGR_COCKPIT_FORCEALPHA_LEGACY=0 re-enables the
    // demotion for the A/B.
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_COCKPIT_FORCEALPHA_LEGACY");
        return !v || v[0] != '0';
    }();
    return enabled;
}

bool TextureNameLooksLikeGlass(const char* name)
{
    if (!name || !*name)
    {
        return false;
    }
    const char* base = name;
    for (const char* p = name; *p; p++)
    {
        if (*p == '\\' || *p == '/')
        {
            base = p + 1;
        }
    }
    std::string lower(base);
    for (char& c : lower)
    {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (lower.find("sklo") != std::string::npos || lower.find("glass") != std::string::npos ||
        lower.find("okno") != std::string::npos || lower.find("okna") != std::string::npos)
    {
        return true;
    }
    // "skla" is glass (genitive) unless it is the start of "sklad" (warehouse).
    for (size_t at = lower.find("skla"); at != std::string::npos; at = lower.find("skla", at + 1))
    {
        if (lower.compare(at, 5, "sklad") != 0)
        {
            return true;
        }
    }
    return false;
}

// Does the policy above also cover a texture whose PAA carries a FLAG tagg?
//
// It used to run only when the alpha channel was implied by the FORMAT, i.e. when the file
// carried no tagg. That exemption reads the tagg as "blend this surface", and it is not: the
// tagg says the texture HAS AN ALPHA CHANNEL. EngineWgpu's own draw-mode override says the same
// thing in the same words ("Backend::IsAlpha means `this face's texture has an alpha channel`,
// not `blend this face`"), and MAT-049 -- helicopters translucent on WGPU, solid on GL33, 951 of
// 4,184 CWA textures classifying BLEND -- is the population that exemption leaves behind. So is
// the owner's rifle going see-through against the sky.
//
// The tagg keeps the job it is authoritative for. The demotion target is
// `pctClear >= 2 ? Cutout : Opaque`, so a tagged CUTOUT still lands on Cutout and MAT-047's
// finding (a tagg-less DXT5 cutout classifies Opaque and draws its transparent texels solid) is
// untouched. What changes is only whether a tagged, demonstrably-solid body is allowed to blend.
//
// DEFAULT OFF, and the reason is the limit of the evidence rather than a doubt about the idea.
// The three thresholds were measured over textures with NO tagg; applying them to the tagged
// population borrows margins established somewhere else. Measured with it on, OFP Everon village:
// 30 demotions, every one a solid object -- roads (asfaltka, silnice), doors (vrata_l/p,
// branka_green, dvere_domovni), fences (planky2000), the ladder rungs (zebrik_sprusel), pallets,
// signs (ukazatel_smer), the church clock face (kostel2_cifernik), the bell, the medic cross,
// the hangar louvres -- with mid% between 2.1 and 15.4 against a threshold of 25, and not one
// glass, canopy, cockpit or pruzor texture in the set. That is encouraging and it is not the
// helicopter corpus MAT-049 is actually about. `WGR_ALPHA_POLICY_TAGGED=1` is the A/B; validate
// it against aircraft canopies before making it the default.
//
// NOTE it is NOT what fixed the owner's see-through rifle -- the M16's textures classify Opaque
// and this policy never touches them. See MaterialIsFullyOpaque in EngineWgpu.cpp for that.
static bool FormatAlphaPolicyCoversTagged()
{
    static const bool covers = []
    {
        const char* value = std::getenv("WGR_ALPHA_POLICY_TAGGED");
        return value && std::strcmp(value, "0") != 0;
    }();
    return covers;
}

void ApplyFormatAlphaPolicy(AlphaStats& stats, const char* name)
{
    if (stats.kind != AlphaStats::Blend || CockpitForceAlphaLegacyEnabled())
    {
        return;
    }
    static const double midMin = EnvDouble("WGR_FORMAT_ALPHA_MID_MIN", 25.0);
    static const double partialMin = EnvDouble("WGR_FORMAT_ALPHA_PARTIAL_MIN", 50.0);
    static const double opaqueMin = EnvDouble("WGR_FORMAT_ALPHA_OPAQUE_MIN", 40.0);
    const bool glassByHistogram = stats.pctMid >= midMin || stats.pctPartial >= partialMin;
    const bool mostlySolid = stats.pctOpaque >= opaqueMin;
    if (glassByHistogram || !mostlySolid || TextureNameLooksLikeGlass(name))
    {
        return;
    }
    const AlphaStats::Kind before = stats.kind;
    stats.kind = stats.pctClear >= 2.0 ? AlphaStats::Cutout : AlphaStats::Opaque;
    GFormatAlphaDemotions.fetch_add(1, std::memory_order_relaxed);
    if (AlphaShapeTraceEnabled())
    {
        LOG_INFO(Graphics,
                 "Wgpu format-alpha: {} clear={:.1f}% partial={:.1f}% mid={:.1f}% opaque={:.1f}% mean={} {} -> {}",
                 name ? name : "<unnamed>", stats.pctClear, stats.pctPartial, stats.pctMid, stats.pctOpaque,
                 stats.aMean, AlphaKindName(before), AlphaKindName(stats.kind));
    }
}

// Decode the top (full-resolution) mip once and classify its alpha channel — mirrors
// TextureGL33::ScanTopMipAlphaClass. Reads the texture bytes through the VFS (works for
// PBO-packed textures) and decodes via the shared DecodePAABuffer, which handles every
// PAA/PAC pixel format. Top mip on purpose: a smaller mip blurs a cutout's crisp 0/255 holes
// into false partial-alpha, mis-routing a pole/fence to the blend pass.
AlphaStats ScanTopMipAlphaStats(const char* name)
{
    if (!name)
    {
        return {};
    }
    AlphaProfilePhase readPhase(&render::ObjectAdmitProfile::alphaSourceReadMs);
    QIFStream in;
    GFileServer->Open(in, name);
    const int size = in.fail() ? 0 : in.rest();
    if (size <= 0)
    {
        return {};
    }
    AUTO_STATIC_ARRAY(char, fileData, 256 * 1024);
    fileData.Realloc(size);
    fileData.Resize(size);
    in.read(fileData.Data(), size);
    readPhase.Stop();

    const size_t len = strlen(name);
    const bool isPaa = len >= 4 && (name[len - 1] == 'a' || name[len - 1] == 'A'); // .paa vs .pac
    AlphaProfilePhase decodePhase(&render::ObjectAdmitProfile::alphaDecodeMs);
    const DecodedImage img = DecodePAABuffer(fileData.Data(), static_cast<size_t>(size), isPaa);
    decodePhase.Stop();
    if (!img.valid())
    {
        return {};
    }
    AlphaProfilePhase histogramPhase(&render::ObjectAdmitProfile::alphaHistogramMs);
    AlphaStats stats = ClassifyAlpha(img.rgba.data(), static_cast<size_t>(img.width) * static_cast<size_t>(img.height));
    histogramPhase.Stop();
    AlphaProfilePhase shapePhase(&render::ObjectAdmitProfile::alphaShapeMs);
    RefineAlphaClassByShape(stats, img.rgba.data(), img.width, img.height, name);
    return stats;
}

// The block formats that carry a multi-bit alpha channel, i.e. exactly the ones that can
// reach the scan above. DXT1 is excluded here for the same reason GetAlphaClass excludes
// it: one bit of alpha is a punch-through, never a blend, so it is decided from the format
// alone and never decoded.
bool IsBlockAlphaFormat(PacFormat fmt)
{
    switch (fmt)
    {
        case PacDXT2:
        case PacDXT3:
        case PacDXT4:
        case PacDXT5:
            return true;
        default:
            return false;
    }
}

// Alpha stats for a BC2/BC3 top mip, taken straight off the compressed blocks.
//
// The classification only ever looks at byte 3 of a texel, but the path that produced it
// decoded all four: DecodePAABuffer runs decompressDXT3/decompressDXT5, which expand both
// colour endpoints, interpolate the 2- or 4-entry colour palette and memcpy 16 RGBA texels
// per block into a full-resolution image -- and then ClassifyAlpha reads one byte in four
// and throws the rest away. Three quarters of that work exists to be discarded.
//
// Here only the alpha half of each block is decoded into a one-byte-per-texel plane. That is not an approximation of the old answer, it IS the old
// answer: BC3's alpha half is a BC4 block, and DecodeBc4Block builds the same 8-entry
// palette by the same rounding (BlockCompression.cpp) that decompressDXT5 builds inline;
// BC2's is the same low-nibble-first `(n<<4)|n` expansion decompressDXT3 does. The same
// bytes then go through the same histogram and shape policy, so the verdict cannot move.
//
// Still the TOP mip -- the buffer is full resolution and every texel is classified. Nothing
// here samples a smaller level; the crisp 0/255 holes a cutout needs are all present.
bool ClassifyAlphaFromBlocks(const uint8_t* blocks, size_t size, int w, int h, PacFormat fmt, AlphaStats& out,
                             const char* name)
{
    if (!blocks || !IsBlockAlphaFormat(fmt) || w <= 0 || h <= 0)
    {
        return false;
    }
    std::vector<uint8_t> alpha;
    AlphaProfilePhase decodePhase(&render::ObjectAdmitProfile::alphaDecodeMs);
    if (!DecodeBlockAlpha(blocks, size, w, h, fmt == PacDXT4 || fmt == PacDXT5, alpha))
        return false;
    decodePhase.Stop();
    AlphaProfilePhase histogramPhase(&render::ObjectAdmitProfile::alphaHistogramMs);
    out = ClassifyAlphaSamples(alpha.data(), alpha.size(), 1);
    histogramPhase.Stop();
    AlphaProfilePhase shapePhase(&render::ObjectAdmitProfile::alphaShapeMs);
    RefineAlphaClassByShape(out, alpha.data(), w, h, name, 1, 0);
    return true;
}

// The top mip's blocks, handed from the upload that just inflated them to the
// classification that would otherwise read them again.
//
// Both wgpu call sites (EngineWgpu's immediate path and ClassifyGpuSection) do
// `EnsureUploaded()` and then `GetAlphaClass()` on the very next line, so the blocks the
// upload has in hand are still the right ones one call later. One slot, thread-local:
// this is a handoff, not a cache. A miss -- classified on a different thread, classified
// long after some other texture uploaded, or never uploaded at all -- costs nothing and
// falls back to reading the one level from the file, which is correct, only slower.
//
// Storing it is a vector move, so a texture that is uploaded and never classified pays
// nothing. That matters: computing the stats eagerly at upload time would instead have
// added a scan for every alpha-format texture nobody ever asks about.
//
// Keyed by texture NAME, not by the TextureWgpu address. An address is not an identity
// here -- a texture released between the park and the claim frees it, and the next
// allocation can land on it -- and a stale hit would hand one file's holes to another. The
// name plus the level's dimensions and format names the bytes themselves, so a hit is a
// hit on the same file or it is not a hit.
struct TopMipHandoff
{
    std::string name;
    std::vector<uint8_t> blocks;
    size_t topSize = 0;
    int w = 0;
    int h = 0;
    PacFormat format = PacFormatN;

    bool Matches(const char* forName, int forW, int forH, PacFormat forFormat) const
    {
        return topSize > 0 && forName && !name.empty() && format == forFormat && w == forW && h == forH &&
               name == forName;
    }

    void Release()
    {
        name.clear();
        topSize = 0;
        std::vector<uint8_t>().swap(blocks);
    }
};

thread_local TopMipHandoff GTopMipHandoff;

// ---------------------------------------------------------------------------------------
// Streaming attribution.
//
// The Everon fill-in costs ~1.1 ms of CPU per object created and is unattributed WITHIN
// itself: texture upload, the synchronous ODOL parse and object construction are all inside
// the same number, so "textures are the problem" has been a hypothesis, not a measurement.
// This splits the texture side of it into the three things it actually does — read the mip
// chain, hand the blocks to wgpu, and (only for a multi-bit-alpha format) decode the top mip
// a second time to classify its alpha — and reports them as totals, not as a per-frame region.
//
// Cumulative on purpose. The transient is ~81 s long on Everon; what is wanted is "of the
// whole fill-in, how much was texture work", which a per-frame timer cannot answer once the
// frames stop being interesting.
//
// ON by default since the counters started feeding the dev panel's Streaming tab and the
// --capture-metrics sidecar (they used to be a log row only); WGR_TEXTURE_STREAM_STATS=0
// opts out. The cost when on is a steady_clock pair per REAL upload -- tens of nanoseconds
// against a call that just did file I/O and a GPU upload; the resident fast path still pays
// nothing because it returns before any of this.
//
// Storage lives in Poseidon::Dev (StreamingDiag.hpp) so the panel and the capture writer
// can read it without a link edge back into this library; the fields and semantics are
// unchanged from the file-local struct this replaces.
Poseidon::Dev::TextureStreamCounters& GTextureStreamStats = Poseidon::Dev::GTextureStreamCounters();

// RFG-083: what the resident object textures ARE, by size, so a residency number can be
// read. "1,703 textures, 3.63 GB" says nothing about whether the answer is a mip bias
// (a few huge textures) or streaming (many modest ones); the bucket line does.
// Counted at creation, so it is the set that reached the GPU, not the set on disk.
namespace
{
constexpr int          kUploadBuckets = 6; // <=256, 512, 1024, 2048, 4096, >4096
// Two rows: block-compressed uploads and RGBA8 ones, because they differ 4x in bytes at
// the same size and the fix differs too (a BC texture's answer is a mip bias, an RGBA one's
// is to stop uploading it uncompressed).
std::atomic<uint64_t>  GUploadBucketCount[2][kUploadBuckets];
std::atomic<uint64_t>  GUploadBucketBytes[2][kUploadBuckets];
void NoteUploadSize(int w, int h, uint64_t bytes, bool blockCompressed, const char* name = nullptr)
{
    const int side = std::max(w, h);
    // RFG-093: the first few LARGE uncompressed uploads by name, because the histogram can
    // only say "100 of the 1024 class are RGBA8" and not which family they are.
    if (!blockCompressed && side >= 1024 && name != nullptr)
    {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
            LOG_INFO(Graphics, "Wgpu texture: RGBA8 upload {}x{} '{}'", w, h, name);
    }
    const int b = side <= 256 ? 0 : side <= 512 ? 1 : side <= 1024 ? 2 : side <= 2048 ? 3 : side <= 4096 ? 4 : 5;
    const int row = blockCompressed ? 0 : 1;
    GUploadBucketCount[row][b].fetch_add(1, std::memory_order_relaxed);
    GUploadBucketBytes[row][b].fetch_add(bytes, std::memory_order_relaxed);
}
} // namespace

// RFG-092: an `enfa|coverage|colour` leaf composite is a BC7 albedo whose alpha was replaced
// by the coverage map, and it went to the GPU as RGBA8 with generated mips -- 5.6 MB for a
// 1024^2 texture that is 1.4 MB as a block format. Measured on native Everon at the owner's
// configuration: 141 of them, 693 MB, the last uncompressed uploads left after RFG-086;
// 237 / 1,166 MB under the 100k stress. BC3 is RGB + an interpolated alpha block, which is
// exactly what a cutout wants; the chain is built here (box filter, alpha included, so the
// coverage thins at distance the way the GPU's own mips did) and every level encoded with
// stb_dxt. Not BC7: no encoder in the tree, and a leaf card does not need its quality.
namespace
{
bool CompositeWantsBc3(const char* name)
{
    return name != nullptr && std::strncmp(name, "enfa|", 5) == 0 &&
           Poseidon::Dev::GResidencyLevers().compressComposites;
}
} // namespace

std::string TextureUploadHistogramLine()
{
    static const char* names[kUploadBuckets] = {"<=256", "512", "1024", "2048", "4096", ">4096"};
    std::string        out = "object texture uploads by size (count / MB):";
    for (int row = 0; row < 2; row++)
    {
        out += row == 0 ? " BC:" : " | RGBA8:";
        for (int b = 0; b < kUploadBuckets; b++)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " %s=%llu/%.0f", names[b],
                          (unsigned long long)GUploadBucketCount[row][b].load(std::memory_order_relaxed),
                          GUploadBucketBytes[row][b].load(std::memory_order_relaxed) / 1048576.0);
            out += buf;
        }
    }
    return out;
}

bool TextureStreamStatsEnabled()
{
    // getenv walks the process environment block and costs ~2.1 us per call — measured on
    // this machine, and the reason WGR_DRAW_SECTION_ENV_CACHE exists. Read once.
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_TEXTURE_STREAM_STATS");
        return !(v && *v == '0');
    }();
    return enabled;
}

uint64_t NowUs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Emitted on a power-of-two upload count: dense while the world is filling in (which is the
// part being attributed) and near-silent once it has settled, without a frame hook or a
// timer of its own.
void ReportTextureStreamStats(uint64_t uploads)
{
    if ((uploads & (uploads - 1)) != 0)
    {
        return;
    }
    const uint64_t readUs = GTextureStreamStats.readUs.load(std::memory_order_relaxed);
    const uint64_t createUs = GTextureStreamStats.createUs.load(std::memory_order_relaxed);
    const uint64_t fallbackUs = GTextureStreamStats.fallbackUs.load(std::memory_order_relaxed);
    const uint64_t alphaUs = GTextureStreamStats.alphaUs.load(std::memory_order_relaxed);
    LOG_INFO(Graphics,
             "Wgpu texture stream stats: uploads={} mipOpens={} blockMB={:.1f} readMs={:.1f} "
             "createMs={:.1f} fallbacks={} fallbackMs={:.1f} alphaScans={} alphaBlockScans={} "
             "alphaHandoffs={} alphaSpeckleDemotions={} alphaMaskDemotions={} formatAlphaDemotions={} "
             "alphaMs={:.1f} totalMs={:.1f}",
             uploads, TextureMipOpenCount(),
             static_cast<double>(GTextureStreamStats.blockBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
             readUs / 1000.0, createUs / 1000.0, GTextureStreamStats.fallbacks.load(std::memory_order_relaxed),
             fallbackUs / 1000.0, GTextureStreamStats.alphaScans.load(std::memory_order_relaxed),
             GTextureStreamStats.alphaBlockScans.load(std::memory_order_relaxed),
             GTextureStreamStats.alphaHandoffs.load(std::memory_order_relaxed),
             GAlphaSpeckleDemotions.load(std::memory_order_relaxed),
             GAlphaMaskDemotions.load(std::memory_order_relaxed),
             GFormatAlphaDemotions.load(std::memory_order_relaxed), alphaUs / 1000.0,
             (readUs + createUs + fallbackUs + alphaUs) / 1000.0);
    LOG_INFO(Graphics, "Wgpu {}", TextureUploadHistogramLine());
}

} // namespace

TextureWgpu::TextureWgpu(TextureBankWgpu* bank) : _bank(bank) {}

struct TextureWgpu::DeferredGenerated
{
    std::string birthName;
    std::vector<uint8_t> rgba;
    std::shared_ptr<render::procedural::admission::Ledger> ledger;
    std::unique_ptr<render::procedural::admission::Ledger::Charge> charge;
};

// REN-THR-012. These two are the texture paths that reach the renderer directly rather than
// through the frame queues, and a texture can die (or be leased a slot) from anywhere a
// reference drops. If that ever happens between publish and the next InitDraw while the
// render thread is inside a frame, it is a data race with wgr_render_frame, and this is the
// line that says so instead of letting it pass silently. Logged for the first few, counted
// for the rest.
static std::atomic<uint32_t> g_rendererRuleViolations{0};
// REN-THR-013: no longer a check that logs, an ACQUIRE that waits. The window opens here, on
// the first call that needs the handle, and the engine counts who opened it.
static void CheckProducerMayTouchRenderer(const char* what)
{
    if (EngineWgpu::ProducerMayTouchRenderer())
        return;
    g_rendererRuleViolations.fetch_add(1, std::memory_order_relaxed);
    auto& admit = Poseidon::render::GObjectAdmitProfile;
    const uint64_t waitBegan = admit.active ? NowUs() : 0;
    EngineWgpu::AcquireProducerWindow(what);
    if (admit.active)
        admit.textureWindowWaitMs += (NowUs() - waitBegan) / 1000.0;
}

TextureWgpu::~TextureWgpu()
{
    if (_bank)
    {
        if (WgrRenderer* r = _bank->Renderer())
        {
            CheckProducerMayTouchRenderer("texture destroy");
            if (_gpuHandle)
            {
                CheckProducerMayTouchRenderer("texture destroy");
                wgr_texture_destroy(r, _gpuHandle);
            }
            // The lease outlives every upload and dies with the texture OBJECT. This is
            // the ONLY place it may be returned: releasing it on eviction would put the
            // slot back in the pool for an unrelated texture, which is precisely the
            // identity theft REN-RES-001 removes.
            if (_slotLease)
            {
                wgr_texture_slot_release(r, _slotLease);
                _slotLease = 0;
            }
        }
    }
}

// Reserve this texture's bindless slot on first use. 0 (array at cap) is not an error:
// wgr_texture_create_in_slot then behaves exactly as wgr_texture_create.
uint32_t TextureWgpu::AcquireSlotLease()
{
    if (_slotLease == 0 && !_dynamic)
    {
        if (WgrRenderer* r = _bank ? _bank->Renderer() : nullptr)
        {
            CheckProducerMayTouchRenderer("texture slot acquire");
            _slotLease = wgr_texture_slot_acquire(r);
        }
    }
    return _slotLease;
}

int TextureWgpu::Init()
{
    _hotSourceProofRetireAttempted = false;
    // A procedural texture has no file behind it: `#(argb,8,8,3)color(...)` is
    // generated from its own name. Nothing in the file-source chain can open one,
    // so without this every such reference fails to upload -- which is why Arma 3's
    // landing and warning lights, whose whole appearance is an authored flat
    // colour, had nothing to draw.
    if (auto generated = render::procedural::Parse(Name()); generated.generated)
    {
        render::procedural::EncodeNormalUpload(generated);
        // Exact-target owner capture only. The ordinary procedural path below is
        // byte-for-byte the old eager InitDynamic path when no scope is active.
        if (auto ledger = render::procedural::admission::CurrentCapture)
        {
            const char* birthName = Name();
            const size_t nameBytes = birthName ? std::strlen(birthName) + 1 : 0;
            if (Foundation::IsMainThread() && birthName && nameBytes <= render::procedural::admission::MaxBirthNameBytes &&
                generated.rgba.capacity() <= render::procedural::admission::MaxPendingBytes -
                                             sizeof(DeferredGenerated) - nameBytes)
            {
                try
                {
                    auto pending = std::make_unique<DeferredGenerated>();
                    pending->birthName = birthName;
                    pending->ledger = ledger;
                    // Charge actual retained vector/string capacity, not only
                    // their logical size. Pixel ownership moves only after the
                    // final potentially throwing operation has succeeded.
                    const size_t nameCapacity = pending->birthName.capacity() + 1;
                    if (nameCapacity <= render::procedural::admission::MaxPendingBytes - sizeof(DeferredGenerated) &&
                        generated.rgba.capacity() <= render::procedural::admission::MaxPendingBytes - sizeof(DeferredGenerated) - nameCapacity)
                    {
                        const size_t chargedBytes = sizeof(DeferredGenerated) + nameCapacity + generated.rgba.capacity();
                        if (auto charge = ledger->Reserve(chargedBytes))
                        {
                            pending->charge = std::move(charge);
                            pending->rgba = std::move(generated.rgba);
                            _deferredGenerated = std::move(pending);
                            _dynamic = true;
                            _w = generated.width;
                            _h = generated.height;
                            _nMipmaps = 1;
                            ledger->NoteDeferred();
                            return 0;
                        }
                    }
                    else
                        ledger->NoteImmediateFallback();
                }
                catch (...)
                {
                    ledger->NoteImmediateFallback();
                }
            }
            else
            {
                ledger->NoteImmediateFallback();
            }
        }
        InitDynamic(generated.width, generated.height, generated.rgba.data(),
                    static_cast<uint32_t>(generated.rgba.size()));
        return _gpuHandle ? 0 : -1;
    }

    PacFormat format = BasicFormat(Name());

    const auto loadSource = [&]()
    {
        static const bool lazyTint = [] {
            const char* v = std::getenv("WGR_LAZY_BCR_TINT");
            // Exact mip/corpus tests and paired native routes preserve the GPU
            // image while avoiding duplicate decoded CPU chains. 0 is the A/B control.
            return !v || v[0] != '0';
        }();
        float rgb[3];
        const char* innerName = nullptr;
        if (lazyTint && _bank && Poseidon::Dev::GResidencyLevers().layerTintInShader &&
            Enfusion::LayerTintLinearMultiply() && Enfusion::SplitLayerTintName(SourceName(), rgb, &innerName) &&
            innerName && *innerName)
        {
            Ref<Texture> inner = _bank->Load(innerName);
            auto* texture = inner ? static_cast<TextureWgpu*>(inner.GetRef()) : nullptr;
            auto* source = texture ? dynamic_cast<TextureSourceDDS*>(static_cast<ITextureSource*>(texture->_src)) : nullptr;
            if (source)
            {
                auto view = source->CreateLinearTintView(rgb, _mipmaps, MAX_MIPMAPS);
                if (view)
                {
                    _tintDelegate = inner;
                    _src = view.release();
                    _cpuTintView = true;
                    static std::atomic<unsigned> reported{0};
                    if (reported.fetch_add(1, std::memory_order_relaxed) < 8)
                        LOG_INFO(Graphics, "Wgpu lazy BCR tint: '{}' borrows '{}' (no decoded CPU chain)", Name(), innerName);
                    return;
                }
            }
        }
        ITextureSourceFactory* factory = SelectTextureSourceFactory(SourceName());
        if (factory && factory->Check(SourceName()))
            _src = factory->Create(SourceName(), _mipmaps, MAX_MIPMAPS);
    };
    loadSource();
    if (!_src && _sourceName.GetLength())
    {
        LOG_WARN(Graphics, "Default material: cannot load {}; falling back to {}", SourceName(), Name());
        _sourceName = "";
        loadSource();
    }
    if (!_src)
    {
        _nMipmaps = 0;
        return -1;
    }

    format = _src->GetFormat();
    // An original Enfusion BCR's fourth channel is roughness data. The DDS
    // source declares it non-coverage, and forcing alpha here would otherwise
    // send decoded BCR pixels through the cutout/blend classifier. enfa|
    // composites are excluded: their fourth channel was replaced by coverage.
    if ((format == PacARGB4444 || format == PacAI88 || format == PacARGB8888) &&
        !Enfusion::IsOriginalBcrRoughnessName(Name()))
    {
        // Original-engine behaviour (TextureD3D::Init did the same): a multi-bit-alpha
        // FORMAT is treated as an alpha texture whether or not the file carries a FLAG
        // tagg. Kept, because IsAlpha() also feeds the 2D path, fonts and icons, which are
        // AI88 with no taggs at all. But remember that the file itself said nothing, so
        // GetAlphaClass can decline to BLEND on the format's say-so alone.
        _alphaForcedByFormat = !_src->IsAlpha();
        _src->ForceAlpha();
    }

    const PacFormat dFormat = DstFormat(format);

    const int nMipmaps = _src->GetMipmapCount();
    int i = 0;
    for (; i < nMipmaps; i++)
    {
        PacLevelMem& mip = _mipmaps[i];
        mip.SetDestFormat(dFormat, 8);
        if (mip._w < 2 || mip._h < 2)
        {
            break;
        }
    }
    _nMipmaps = i;

    _w = _mipmaps[0]._w;
    _h = _mipmaps[0]._h;
    return 0;
}

void TextureWgpu::RetireHotSourceProof()
{
    static const bool enabled = [] {
        const char* retire = std::getenv("WGR_OBJECT_STREAM_HOT_SOURCE_PROOF_RETIRE");
        const char* source = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURES");
        const char* jobs = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS");
        return retire && std::strcmp(retire, "1") == 0 && source && std::strcmp(source, "1") == 0 &&
            jobs && std::strcmp(jobs, "1") == 0;
    }();
    if (!enabled) return; // No virtual call, snapshot, log or clock on OFF path.
    if (!render::HotSourceProofRetirementPolicy::Reserve(true, Foundation::IsMainThread(),
        _gpuHandle, _alphaClass, _src.NotNull(), _dynamic,
        _tintDelegate.GetRef() != nullptr || _cpuTintView, _hotSourceProofRetireAttempted)) return;
    // Observe numeric debt only: a temporary GetArchiveSourceBinding copy would itself pin it.
    const auto before = ArchiveSourceBinding::SnapshotStats();
    if (!_src->ReleaseArchiveSourceBinding()) return;
    const auto after = ArchiveSourceBinding::SnapshotStats();
    static unsigned rows = 0; // Owner-only actual PAC owner drops; process prefix, not coverage.
    if (rows < 64)
    {
        ++rows;
        const char* sourceName = SourceName();
        size_t nameBytes = 0;
        if (sourceName) while (nameBytes < 8192 && sourceName[nameBytes]) ++nameBytes;
        if (!sourceName || nameBytes == 8192) sourceName = "<bounded-name-refused>";
        LOG_INFO(Graphics, "Hot source proof retired: source={} ownInitProofDropped=true alphaClass={} gpuUploaded=true liveBindingsBefore={} liveBindingsAfter={} knownBindingBytesBefore={} knownBindingBytesAfter={} aliasDebtMayRemain=true row={} limit=64",
            sourceName, int(_alphaClass), before.liveBindings, after.liveBindings,
            before.knownCppBytes, after.knownCppBytes, rows);
    }
    else if (rows == 64)
    {
        ++rows;
        LOG_INFO(Graphics, "Hot source proof retired: log truncated after 64 owner drops; aliases may retain debt, no physical-memory or source-freshness proof");
    }
}

bool TextureWgpu::HasInitializedArchiveBinding() const
{
    return Foundation::IsMainThread() && _src && !!_src->GetArchiveSourceBinding();
}

std::shared_ptr<const ArchiveSourceBinding> TextureWgpu::CurrentArchiveSourceBinding() const
{
    if (!Foundation::IsMainThread() || !_src) return {};
    auto binding = _src->GetArchiveSourceBinding();
    if (!binding) return {};
    const char* name = SourceName();
    if (!name) return {};
    size_t length = 0;
    while (length < 8192 && name[length]) ++length;
    if (!length || length == 8192) return {};
    QFBank* bank = QIFStreamB::AutoBank(name); // Prefix selection only, no loading.
    if (!bank) return {};
    const size_t prefix = static_cast<size_t>(bank->GetPrefix().GetLength());
    if (prefix >= length || !bank->MatchesMountedMember(name + prefix, binding->Request())) return {};
    return binding;
}

bool TextureWgpu::InitRetailPacSource()
{
    if (!Foundation::IsMainThread() || !ArchiveSourceBinding::RetailPacReadScope::MatchesLogicalName(Name()) ||
        !_bank || _src || _gpuHandle || _uploadTried || _dynamic || _deferredGenerated) return false;
    return Init() == 0 && _src && _nMipmaps > 0 && !!CurrentArchiveSourceBinding();
}

bool TextureWgpu::CaptureRetailPacRead(render::ColdPaaRead& out) const
{
    out = render::ColdPaaRead{};
    const auto* profile = RetailRigidAssetProfile::Selected();
    const char* sourceName = SourceName();
    if (!profile || !ArchiveSourceBinding::RetailPacReadScope::Enabled() ||
        !Foundation::IsMainThread() || !sourceName || !profile->MatchesTexturePath(sourceName) ||
        _gpuHandle || _uploadTried || _dynamic || _tintDelegate || _cpuTintView || !_src ||
        _nMipmaps <= 0 || _nMipmaps > 7 || _src->GetFormat() != PacDXT1) return false;
    auto source = CurrentArchiveSourceBinding();
    if (!source || !profile->MatchesTextureArchive(source->Request().archive) ||
        source->Request().bytes > render::RetailPacPrepared::MaxSourceBytes) return false;
    render::ColdPaaRead candidate;
    candidate.key = SourceName();
    candidate.source = std::move(source);
    candidate.magic = 0xff01;
    candidate.count = static_cast<size_t>(_nMipmaps);
    size_t total = 0;
    for (int i = 0; i < _nMipmaps; ++i)
    {
        const auto& mip = _mipmaps[i];
        if (mip.SrcFormat() != PacDXT1 || mip.DstFormat() != PacDXT1 || mip._start < 0 ||
            mip._w <= 0 || mip._h <= 0) return false;
        const size_t bytes = static_cast<size_t>(render::mipmap::ComputeLayout(PacDXT1, mip._w, mip._h).dataSize);
        if (!bytes || bytes > render::RetailPacPrepared::MaxBlockBytes - total) return false;
        total += bytes;
        candidate.levels[i] = {mip._w, mip._h, bytes, static_cast<size_t>(mip._start)};
    }
    if (!candidate.Valid()) return false;
    out = std::move(candidate);
    return true;
}

uint64_t TextureWgpu::UploadRetailPacPrepared(const render::RetailPacPrepared& prepared,
                                               RetailPacUploadWitness& witness)
{
    witness = RetailPacUploadWitness{};
    if (!ArchiveSourceBinding::RetailPacReadScope::Enabled() || !Foundation::IsMainThread() ||
        !_bank || !_bank->Renderer() || _gpuHandle || _uploadTried || _dynamic || _deferredGenerated ||
        _tintDelegate || _cpuTintView || !prepared.Valid()) return 0;
    render::ColdPaaRead current;
    if (!CaptureRetailPacRead(current) || current.key != prepared._header.key ||
        !current.source->Request().SameArchiveMember(prepared._header.source->Request()) ||
        current.magic != prepared._header.magic || current.count != prepared._header.count) return 0;
    witness.currentInit = true;
    for (size_t i = 0; i < current.count; ++i)
    {
        const auto& a = current.levels[i];
        const auto& b = prepared._header.levels[i];
        if (a.width != b.width || a.height != b.height || a.bytes != b.bytes || a.header != b.header)
            return 0;
    }
    witness.layoutMatched = true;
    if (!current.source->Request().CopyMemberIdentity(witness.member)) return 0;
    witness.birth = current.source;
    witness.sourceBytes = current.source->Request().bytes;
    witness.rawSha256 = prepared._rawSha256;

    const int firstLevel = std::min(TextureMipBias(), _nMipmaps - 1);
    if (firstLevel < 0 || firstLevel >= _nMipmaps) return 0;
    const int count = _nMipmaps - firstLevel;
    std::vector<uint8_t> upload;
    AlphaStats preparedAlpha;
    try
    {
        // BC1 is a one-bit format, so ordinary GetAlphaClass never scans its
        // pixels; HasAlphaHoles consequently has no histogram. Derive that
        // missing fact from the exact retained top mip, even when mip bias
        // starts the GPU upload lower. This decoder is pure and cannot reopen
        // the PAC by name. At the 128 KiB BC1 block cap RGBA scratch is <=1 MiB.
        const auto& alphaTop = prepared._chain.levels.front();
        const int alphaW = _mipmaps[0]._w, alphaH = _mipmaps[0]._h;
        constexpr size_t maxAlphaPixels = render::RetailPacPrepared::MaxBlockBytes * 2;
        if (alphaW <= 0 || alphaH <= 0 || static_cast<size_t>(alphaW) > maxAlphaPixels / static_cast<size_t>(alphaH) ||
            alphaTop.size != static_cast<size_t>(render::mipmap::ComputeLayout(PacDXT1, alphaW, alphaH).dataSize) ||
            alphaTop.offset > prepared._chain.blocks.size() ||
            alphaTop.size > prepared._chain.blocks.size() - alphaTop.offset) return 0;
        const size_t alphaPixels = static_cast<size_t>(alphaW) * static_cast<size_t>(alphaH);
        std::vector<uint8_t> rgba(alphaPixels * 4);
        if (!ConvertPixels(prepared._chain.blocks.data() + alphaTop.offset, rgba.data(), alphaW, alphaH,
                PixelFormat::DXT1, PixelFormat::RGBA8888)) return 0;
        preparedAlpha = ClassifyAlpha(rgba.data(), alphaPixels);
        std::vector<uint8_t>().swap(rgba);
        size_t total = 0;
        for (int i = firstLevel; i < _nMipmaps; ++i)
        {
            const auto& level = prepared._chain.levels[static_cast<size_t>(i)];
            if (level.size > render::RetailPacPrepared::MaxBlockBytes - total) return 0;
            total += level.size;
        }
        if (!total || total > UINT32_MAX) return 0;
        upload.resize(total);
        size_t offset = 0;
        for (int i = firstLevel; i < _nMipmaps; ++i)
        {
            const auto& level = prepared._chain.levels[static_cast<size_t>(i)];
            std::memcpy(upload.data() + offset, prepared._chain.blocks.data() + level.offset, level.size);
            offset += level.size;
        }
        witness.uploadedSha256 = Foundation::Sha256::Of(upload.data(), upload.size());
    }
    catch (...) { return 0; }
    // Repeat the current-mount check after pure preparation and immediately
    // before the only GPU call. A changed source never reaches create.
    auto beforeCreate = CurrentArchiveSourceBinding();
    if (!beforeCreate || !beforeCreate->Request().SameArchiveMember(witness.birth->Request())) return 0;
    WgrRenderer* renderer = _bank->Renderer();
    CheckProducerMayTouchRenderer("retail PAC texture slot acquire");
    const uint32_t slot = AcquireSlotLease();
    if (!slot) return 0; // no stable reusable slot; lease state stays charged if acquired
    witness.slotLease = slot;
    CheckProducerMayTouchRenderer("retail PAC texture create");
    // Slot acquisition may wait for a producer window. Refuse if that interval
    // changed the loaded bank, before any texture resource is created. Acquire
    // the window before this final check so no wait follows source validation.
    beforeCreate = CurrentArchiveSourceBinding();
    if (!beforeCreate || !beforeCreate->Request().SameArchiveMember(witness.birth->Request())) return 0;
    const auto& top = _mipmaps[firstLevel];
    witness.firstLevel = static_cast<uint32_t>(firstLevel);
    witness.levels = static_cast<uint32_t>(count);
    witness.width = static_cast<uint32_t>(top._w);
    witness.height = static_cast<uint32_t>(top._h);
    witness.uploadedBytes = upload.size();
    GTextureStreamStats.frameUploadBytes.fetch_add(upload.size(), std::memory_order_relaxed);
    witness.createAttempted = true;
    const uint64_t created = wgr_texture_create_in_slot(renderer, witness.width, witness.height,
        WGR_TEXTURE_BC1, witness.levels, 0, upload.data(), static_cast<uint32_t>(upload.size()), slot);
    GTextureStreamStats.frameUploads.fetch_add(1, std::memory_order_relaxed);
    witness.created = created != 0;
    if (!created) return 0; // ordinary retry remains possible; slot lease remains owned
    const auto afterCreate = CurrentArchiveSourceBinding();
    witness.postMountMatched = afterCreate &&
        afterCreate->Request().SameArchiveMember(witness.birth->Request());
    if (!witness.postMountMatched)
    {
        CheckProducerMayTouchRenderer("retail PAC stale texture retire");
        wgr_texture_destroy(renderer, created);
        witness.retiredAfterMismatch = true;
        return 0;
    }
    _gpuHandle = created;
    _uploadTried = true;
    _uploadedFirstLevel = firstLevel;
    _w = top._w; _h = top._h;
    _alphaStats = preparedAlpha;
    _alphaStatsScanned = true;
    if (_alphaClass < 0)
        _alphaClass = static_cast<signed char>(ClassifyTextureAlpha(
            _src->IsAlpha(), _src->IsTransparent(), true, nullptr));
    witness.alphaFromPrepared = true;
    witness.alphaTopPixels = static_cast<uint64_t>(_mipmaps[0]._w) * static_cast<uint64_t>(_mipmaps[0]._h);
    witness.alphaClearPercent = _alphaStats.pctClear;
    witness.handle = created;
    NoteUploadSize(_w, _h, upload.size(), true);
    return created;
}

bool TextureWgpu::ScanTopMipAlphaBlocks(AlphaScanPath& path)
{
    // WGR_ALPHA_BLOCK_SCAN=0 falls back to the original whole-file decode.
    //
    // This shipped without an A/B, which was a mistake: the alpha CLASS decides whether a
    // section draws opaque, cutout or blend, so a wrong verdict here is a visual bug several
    // layers away from the texture code, and there was no way to rule this out in one run.
    // The owner has since reported translucent helicopter cockpits -- glass is precisely a
    // multi-bit-alpha blend section -- and this is the first thing that should be excluded.
    //
    // The block path is believed exact, not approximate: BC3's alpha half is a BC4 block and
    // BC2's is a low-nibble-first expansion, both feeding the SAME unmodified ClassifyAlpha,
    // and it was verified byte-identical on every AlphaStats field across six real DXT5
    // textures. But "verified on six" is not "verified on the one that broke", and a lever
    // costs nothing.
    static const bool blockScanEnabled = []
    {
        const char* v = std::getenv("WGR_ALPHA_BLOCK_SCAN");
        return !v || v[0] != '0';
    }();
    if (!blockScanEnabled)
    {
        return false; // caller falls through to ScanTopMipAlphaStats, the original path
    }
    if (!_src || _nMipmaps <= 0)
    {
        return false;
    }
    const PacFormat fmt = _src->GetFormat();
    // The chain is read in the source's own format only when the destination matches it,
    // which is what DstFormat() does for every DXT format. If that ever stops being true
    // the bytes would not be blocks, so check rather than assume.
    if (!IsBlockAlphaFormat(fmt) || _mipmaps[0].DstFormat() != fmt)
    {
        return false;
    }
    const int w = _mipmaps[0]._w;
    const int h = _mipmaps[0]._h;
    if (w <= 0 || h <= 0)
    {
        return false;
    }

    // The explicit cold handoff owns these exact Init-bound bytes during shape admission.
    // Borrow only the top mip; preserve the full chain for the subsequent GPU upload.
    // Refusal/cancellation falls through to the existing source-reader policy.
    if (render::ColdPaaHandoffEnabled() && render::ColdPaaAdmissionScope::HasOwnedPayload())
    {
        render::ColdPaaRead header;
        AlphaStats candidate;
        if (CaptureColdPaaRead(header) && render::ColdPaaAdmissionScope::PeekTopAlpha(
                header.key, header.source, header, [&](const uint8_t* blocks, size_t bytes) {
                    return ClassifyAlphaFromBlocks(blocks, bytes, w, h, fmt, candidate, Name());
                }))
        {
            _alphaStats = candidate;
            path = AlphaScanPath::ColdOwned;
            GTextureStreamStats.coldAlphaPeeks.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    // Only the explicit PBO-texture experiment performs a lookup/native capture.
    // Facts are usable before upload; the raw chain remains available to upload.
    static const bool pboTextures = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_PBO_TEXTURES");
        return value && value[0] == '1';
    }();
    if (pboTextures && GUseFileBanks && render::PreparedTextureStore::Enabled())
    {
        auto& store = render::PreparedTextureStore::Instance();
        const auto key = render::PreparedTextureStore::Key(SourceName());
        if (store.HasAlphaFacts(key))
            if (auto* bank = QIFStreamB::AutoBank(key.c_str()))
                if (auto current = bank->CaptureReadRequest(key.c_str() + bank->GetPrefix().GetLength(), true))
                {
                    const int bytes = render::mipmap::ComputeLayout(fmt, w, h).dataSize;
                    render::PreparedAlphaFacts facts;
                    const uint16_t magic = fmt == PacDXT2 ? 0xFF02 : fmt == PacDXT3 ? 0xFF03 :
                                           fmt == PacDXT4 ? 0xFF04 : 0xFF05;
                    if (bytes > 0 && _mipmaps[0].SrcFormat() == fmt &&
                        store.CopyAlphaFacts(key, *current, magic, w, h, static_cast<size_t>(bytes), _mipmaps[0]._start, facts))
                    {
                        _alphaStats = facts.histogram;
                        AlphaProfilePhase shapePhase(&render::ObjectAdmitProfile::alphaShapeMs);
                        RefineAlphaClassByShape(_alphaStats, nullptr, w, h, Name(), 1, 0, &facts.shape);
                        path = AlphaScanPath::PreparedFacts;
                        return true;
                    }
                }
    }

    TopMipHandoff& handoff = GTopMipHandoff;
    if (handoff.Matches(SourceName(), w, h, fmt) &&
        ClassifyAlphaFromBlocks(handoff.blocks.data(), handoff.topSize, w, h, fmt, _alphaStats, Name()))
    {
        handoff.Release();
        path = AlphaScanPath::Handoff;
        return true;
    }

    // No handoff: read the ONE level being classified. The whole-file scan this replaces
    // read every level and then decoded only this one.
    const int need = render::mipmap::ComputeLayout(fmt, w, h).dataSize;
    if (need <= 0)
    {
        return false;
    }
    std::vector<uint8_t> blocks(static_cast<size_t>(need));
    const MipmapRead read{blocks.data(), &_mipmaps[0], 0};
    AlphaProfilePhase readPhase(&render::ObjectAdmitProfile::alphaSourceReadMs);
    if (!_src->GetMipmapChain(&read, 1))
    {
        return false;
    }
    readPhase.Stop();
    if (!ClassifyAlphaFromBlocks(blocks.data(), blocks.size(), w, h, fmt, _alphaStats, Name()))
    {
        return false;
    }
    path = AlphaScanPath::BlockRead;
    return true;
}

// Three-way alpha classification for the section-sort renderer (opaque/cutout occlude;
// blend defers to the back-to-front pass). Mirrors TextureGL33::GetAlphaClass: cheap header
// flags decide it outright except for a multi-bit-alpha format, which needs the top-mip decode
// to tell cutout from blend. Cached in _alphaClass. Dynamic textures keep the base Opaque.
AlphaStats::Kind TextureWgpu::GetAlphaClass()
{
    if (_cpuTintView)
    {
        // Linear tint changes RGB only. Ask the same inner image used by the
        // GPU, not a newly decoded top mip of each colour variant.
        auto* inner = static_cast<TextureWgpu*>(_tintDelegate.GetRef());
        const auto kind = inner->GetAlphaClass();
        _alphaStats = inner->_alphaStats;
        _alphaStatsScanned = inner->_alphaStatsScanned;
        _alphaClass = static_cast<signed char>(kind);
        RetireHotSourceProof(); // Delegated view does not forward retirement; inner manages its own proof.
        return kind;
    }
    if (_alphaClass >= 0)
    {
        RetireHotSourceProof();
        return static_cast<AlphaStats::Kind>(_alphaClass);
    }
    // RFG-024: no source yet means NOT YET KNOWN, not opaque.
    //
    // The answer below is cached at the end of this function, so a caller that asks
    // before Init() has run pins the texture to Opaque for the rest of its life --
    // and the callers are ordinary ones: SectionGpuOwned asks while a shape is being
    // built, which for a natively loaded world happens before the texture is
    // touched. A leaf card that gets asked at the wrong moment is a solid quad
    // afterwards no matter what its alpha says, with nothing logged anywhere.
    //
    // Returning without caching costs one extra classification later and cannot be
    // wrong; caching a guess cannot be undone.
    if (!_src)
    {
        return AlphaStats::Opaque;
    }

    // RFG-055: an Enfusion OpacityMap is COVERAGE. It is never a blend.
    //
    // The composite `enfa|opacity|colour` name exists precisely because Enfusion keeps
    // a leaf's coverage in its own `..._A.edds` beside the `_BCR`. The material census
    // behind RFG-019 counted 1,715 such files and found every one bound as coverage and
    // none as a blend -- so this is the material's own statement, not an inference from
    // the pixels.
    //
    // The histogram cannot reach that conclusion on its own. A Reforger leaf card is an
    // ANTI-ALIASED cutout: its edge pixels carry partial alpha on purpose, measured at
    // 6.6% of the texture, and a classifier that reads partial alpha as "blend" calls it
    // one. Blended, the cards draw back-to-front with no alpha-to-coverage, so a stand of
    // birch loses its edges and washes out into a pale, flat mass -- the thing that keeps
    // being reported as the trees looking wrong, after the coverage itself was fixed.
    // RFG-070: look through a tint wrapper; tint changes RGB only, while the
    // composite's separate OpacityMap remains a cutout even with soft edges.
    if (Enfusion::IsOpacityCompositeName(Name()))
    {
        _alphaClass = static_cast<int>(AlphaStats::Cutout);
        RetireHotSourceProof();
        return AlphaStats::Cutout;
    }

    AlphaStats::Kind kind = AlphaStats::Opaque;
    {
        const bool hasAlpha = _src->IsAlpha();
        const bool chroma = _src->IsTransparent();
        const bool oneBit = _src->GetFormat() == PacDXT1; // 1-bit alpha: punch-through only
        // Only multi-bit-alpha formats need the (cached) decode to tell cutout from blend.
        AlphaStats decoded;
        const AlphaStats* decodedPtr = nullptr;
        if (hasAlpha && !oneBit)
        {
            if (!_alphaStatsScanned)
            {
                // The one part of the texture cost that buys no pixels, so it is timed
                // separately. It used to be a whole second pass over the file: read every
                // level, decode the FULL-resolution one to RGBA8, walk every pixel's alpha
                // and discard the three colour bytes in four that the decode produced.
                //
                // A block format now answers it off the compressed alpha alone, from the
                // level the upload already inflated where it can. Everything else still
                // takes the old path, unchanged: no format loses its answer, and the answer
                // it gets is the same one.
                const bool timed = TextureStreamStatsEnabled();
                render::ObjectAdmitProfile& admit = render::GObjectAdmitProfile;
                const bool profiled = admit.active;
                const uint64_t began = (timed || profiled) ? NowUs() : 0;
                AlphaScanPath path = AlphaScanPath::FullDecode;
                if (!ScanTopMipAlphaBlocks(path))
                {
                    // RFG-025: scan the SOURCE when it has the pixels, not the file.
                    //
                    // ScanTopMipAlphaStats re-opens by name, which assumes the name is
                    // a file. It is not always: a composite `enfa|coverage|colour`
                    // name (RFG-023) exists precisely because the image is two files
                    // merged, and re-reading it finds nothing, reports no alpha, and
                    // the leaf becomes a solid quad. The source has already done the
                    // work; ask it.
                    bool scannedFromSource = false;
                    if (_src != nullptr && _nMipmaps > 0 && _mipmaps[0].DstFormat() == PacARGB8888 && _w > 0 &&
                        _h > 0)
                    {
                        std::vector<uint8_t> top(static_cast<size_t>(_w) * static_cast<size_t>(_h) * 4);
                        AlphaProfilePhase readPhase(&render::ObjectAdmitProfile::alphaSourceReadMs);
                        const bool readOk = _src->GetMipmapData(top.data(), _mipmaps[0], 0);
                        readPhase.Stop();
                        if (readOk)
                        {
                            AlphaProfilePhase histogramPhase(&render::ObjectAdmitProfile::alphaHistogramMs);
                            _alphaStats = ClassifyAlpha(top.data(), static_cast<size_t>(_w) * static_cast<size_t>(_h));
                            scannedFromSource = true;
                        }
                    }
                    if (!scannedFromSource)
                        _alphaStats = ScanTopMipAlphaStats(SourceName());
                }
                if (profiled)
                {
                    ++admit.alphaScans;
                    if (path == AlphaScanPath::Handoff) ++admit.alphaHandoffScans;
                    else if (path == AlphaScanPath::BlockRead) ++admit.alphaBlockReadScans;
                    else if (path == AlphaScanPath::FullDecode) ++admit.alphaFullDecodeScans;
                    admit.alphaScanMs += (NowUs() - began) / 1000.0;
                }
                // Both scan paths have run the histogram and the shape guard; the last word
                // on a FORMAT-implied alpha channel is whether the pixels look like glass.
                if (_alphaForcedByFormat || FormatAlphaPolicyCoversTagged())
                {
                    ApplyFormatAlphaPolicy(_alphaStats, Name());
                }
                _alphaStatsScanned = true;
                if (timed)
                {
                    GTextureStreamStats.alphaScans.fetch_add(1, std::memory_order_relaxed);
                    if (path != AlphaScanPath::FullDecode)
                    {
                        GTextureStreamStats.alphaBlockScans.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (path == AlphaScanPath::Handoff)
                    {
                        GTextureStreamStats.alphaHandoffs.fetch_add(1, std::memory_order_relaxed);
                    }
                    GTextureStreamStats.alphaUs.fetch_add(NowUs() - began, std::memory_order_relaxed);
                }
            }
            decoded = _alphaStats;
            decoded.kind = LegacyAi88CoverageClass(_src->GetFormat() == PacAI88, decoded);
            decodedPtr = &decoded;
        }
        kind = ClassifyTextureAlpha(hasAlpha, chroma, oneBit, decodedPtr);
    }
    _alphaClass = static_cast<signed char>(kind);
    // One-shot per run, for the leaf-cutout investigation: the verdict AND the
    // numbers it came from. "The cards are solid" has never been the same question
    // as "the alpha is missing", and only this line separates them.
    {
        static std::atomic<uint32_t> logged{0};
        const char* n = Name();
        if (n != nullptr && std::strstr(n, "polyplane") != nullptr &&
            logged.fetch_add(1, std::memory_order_relaxed) < 4)
        {
            LOG_INFO(Graphics,
                     "Alpha class for {}: kind {} (hasAlpha {} chroma {} oneBit {}) -- aMin {} aMax {} clear {:.1f}% "
                     "partial {:.1f}%",
                     n, static_cast<int>(kind), _src != nullptr && _src->IsAlpha() ? 1 : 0,
                     _src != nullptr && _src->IsTransparent() ? 1 : 0,
                     _src != nullptr && _src->GetFormat() == PacDXT1 ? 1 : 0, _alphaStats.aMin, _alphaStats.aMax,
                     _alphaStats.pctClear, _alphaStats.pctPartial);
        }
    }
    RetireHotSourceProof();
    return kind;
}

void TextureWgpu::EvictGpu()
{
    if (_dynamic || _gpuHandle == 0 || !_bank)
    {
        return;
    }
    if (WgrRenderer* r = _bank->Renderer())
    {
        CheckProducerMayTouchRenderer("texture destroy");
        wgr_texture_destroy(r, _gpuHandle);
    }
    _gpuHandle = 0;
    _uploadTried = false;
    if (render::PreparedTextureStore::Enabled())
    {
        render::PreparedTextureStore::Instance().MarkEvicted(render::PreparedTextureStore::Key(SourceName()));
    }
}

bool TextureWgpu::HasAlphaHoles()
{
    // The cutout decision is reached only for a decoded multi-bit-alpha texture in the
    // TreeAdv path. A 2% floor matches ClassifyAlpha's clear-coverage threshold and keeps
    // harmless compression noise from turning a solid bark map into a masked draw.
    if (!_alphaStatsScanned)
    {
        (void)GetAlphaClass();
    }
    return _alphaStatsScanned && _alphaStats.pctClear >= 2.0;
}

bool TextureWgpu::NormalHasNativeCavity() const
{
    if (_nMipmaps <= 0)
        return false;
    const bool decodedDds = _mipmaps[0].DstFormat() == PacARGB8888 &&
        dynamic_cast<TextureSourceDDS*>(static_cast<ITextureSource*>(_src)) != nullptr;
    return NativeNtcCavityChannelAvailable(_mipmaps[0].DstFormat(), decodedDds,
                                            NormalIsRgPacked(), Name());
}

bool TextureWgpu::NormalHasNativePbrChannels() const
{
    if (_nMipmaps <= 0)
        return false;
    const bool decodedDds = _mipmaps[0].DstFormat() == PacARGB8888 &&
        dynamic_cast<TextureSourceDDS*>(static_cast<ITextureSource*>(_src)) != nullptr;
    return NativeNmoMaterialChannelsAvailable(_mipmaps[0].DstFormat(), decodedDds,
                                               NormalIsRgPacked(), Name());
}

bool TextureWgpu::NormalIsRgPacked() const
{
    if (_nMipmaps <= 0)
    {
        return false;
    }
    const PacFormat dst = _mipmaps[0].DstFormat();
    // BC5 is a two-channel format by definition: there is no alpha to read at all (it
    // samples as (r, g, 0, 1)), so this holds whatever the file is called.
    if (dst == PacBC5)
    {
        return true;
    }
    // BC7 keeps four channels and is also what every ordinary Enfusion ALBEDO is, so the
    // name has to decide. NMO and NTC store normal XY in RG; their alpha is
    // occlusion or cavity, respectively. Reading X from alpha flattens tree lighting.
    // Decoded Enfusion normals retain both R and the independent A data channel.
    // Keeping the RG declaration also prevents the direct shader from treating
    // R as POM height.
    const bool decodedDds =
        dst == PacARGB8888 && dynamic_cast<TextureSourceDDS*>(static_cast<ITextureSource*>(_src)) != nullptr;
    return (dst == PacBC7 || decodedDds) && IsEnfusionRgNormalName(Name());
}

void TextureWgpu::InitDynamic(int w, int h, const void* rgba, uint32_t size, bool genMips)
{
    _dynamic = true;
    _uploadTried = true;
    _w = w;
    _h = h;
    _nMipmaps = 1;
    if (WgrRenderer* r = _bank ? _bank->Renderer() : nullptr)
    {
        CheckProducerMayTouchRenderer("texture create");
        _gpuHandle = wgr_texture_create(r, static_cast<uint32_t>(w), static_cast<uint32_t>(h), WGR_TEXTURE_RGBA8, 1,
                                        genMips ? WGR_TEXTURE_GEN_MIPS : 0u,
                                        static_cast<const uint8_t*>(rgba), size);
    }
    if (!_gpuHandle)
    {
        LOG_WARN(Graphics, "Wgpu: failed to upload dynamic texture {}x{} size={}", w, h, size);
    }
}

void TextureWgpu::UpdateDynamic(const void* rgba, uint32_t size)
{
    if (!_gpuHandle)
    {
        return;
    }

    if (WgrRenderer* r = _bank ? _bank->Renderer() : nullptr)
    {
        CheckProducerMayTouchRenderer("texture update");
        wgr_texture_update(r, _gpuHandle, static_cast<const uint8_t*>(rgba), size);
    }
}

// CPU-side pixel read, mirroring TextureGL33::GetPixel (without interpolation).
// Used by Scene::SetSkyTexture to derive the fog / background colour from the sky
// texture — a stub here left the fog and horizon black. Decodes the requested mip
// to its dest format and samples it.
Color TextureWgpu::GetPixel(int level, float u, float v) const
{
    if (!_src || _nMipmaps <= 0)
    {
        return HWhite;
    }
    if (level < 0 || level >= _nMipmaps)
    {
        level = 0;
    }

    PacLevelMem mip = _mipmaps[level];
    std::vector<char> mem(static_cast<size_t>(mip._pitch) * mip._h);
    if (mem.empty() || !_src->GetMipmapData(mem.data(), mip, level))
    {
        return HWhite;
    }
    return mip.GetPixel(mem.data(), u, v);
}

// WGR_TEXTURE_MIP_BIAS=N drops the N most detailed mip levels of every block-compressed
// object texture at upload. 0 (default) uploads the full chain, exactly as before.
//
// This is the one lever here that BUYS PERFORMANCE WITH VISUAL QUALITY, so it is off by
// default and says so. Each level costs 4x the one below it, so a bias of 1 removes ~75% of
// a texture's bytes and halves its resolution.
//
// It exists for DayZ Chernarus, where the alternative is not "slightly softer textures", it
// is a world that does not render at all. Measured there: 3,552 object textures for 3.84 GB
// against a 2 GB budget, and the texture LRU starved to 1-31 eviction candidates per round
// because RetainGpuModel pins every texture of every registered model. Memory climbs
// unbounded until the driver refuses an allocation, and the world dies at ~22k of 199,917
// objects. Reforger Everon does not need this at all -- 199,881 objects but only 218 MB of
// textures, because they are heavily reused.
//
// The RIGHT fix is for a pin to stop meaning "registered, therefore untouchable" and for the
// LRU to age a retained texture on whether it was recently DRAWN. Both halves of that now
// exist; the split still matters when reading the code, so:
//
// SOLVED (REN-RES-001, slot leases -- wgr_texture_slot_acquire / _create_in_slot). The
// blocker used to be that a retained model's texture reference is a BAKED BINDLESS SLOT
// INDEX which nothing could re-bake: wgr_model_register resolves each material's handle to
// a dense slot ONCE (gfx3d/mod.rs register_model -> textures.texture_slot), stores the
// integer in the append-only SectionMaterialGpu table, and gpu_driven.wgsl indexes the
// bindless array with it. Destroying the texture then did two wrongs at once -- the freed
// slot went onto a LIFO free list the next upload popped first (so a live model sampled an
// unrelated texture, permanently), and the re-upload got a fresh slot the baked material
// never learned (so the object stayed white for the session).
//
// A lease removes both. The slot now belongs to the TextureWgpu, not to one upload: an
// eviction points it at the white fallback but leaves it reserved, and the re-upload
// reclaims the same index. Models never learn a new number because it never changes. There
// is no shader change and no per-fetch cost; the price is that a slot is held while
// evicted, bounded by distinct texture NAMES (3,552 on Chernarus against a cap of 8,192).
//
// ALSO SOLVED (REN-RES-001 "Liveness"), which is what finally let the pin check in
// TextureBankWgpu::TrimResidency be relaxed. `_lastUsedFrame` is refreshed inside
// EnsureUploaded(), which the DIRECT draw path calls every frame via UseMipmap -- so a
// direct-draw texture's eviction costs one white frame and self-heals -- but a retained
// model's textures were touched at registration and never again, because nothing on that path
// calls EnsureUploaded per frame. Unpinning them in that state made every retained texture an
// eviction candidate 120 frames after registration regardless of whether it was on screen, and
// nothing ever asked for it back: correct slots, white buildings. Measured exactly so at
// WGR_DYNAMIC_VRAM_MB=900 on the Everon traverse, 2026-08-31.
//
// The two missing halves now exist, both in EngineWgpu:
//
//   * MarkRetainedModelLiveness() sweeps the retained instance table against the camera each
//     frame (a slice of it, so the cost is bounded) and calls NoteTextureUse for the textures
//     of every registered model with an instance inside the object draw distance. It is a
//     PER-MODEL, distance-only signal -- a strict superset of what actually drew, because the
//     GPU's cull results come back only as aggregate counters and the CPU never sees a
//     Full-coverage object's visibility at all. Marking too much is the safe error.
//   * InvalidateStaleGpuModels() does the refill: a model back in range whose texture was
//     evicted gets an EnsureUploaded() rather than a teardown, and the lease above is what
//     makes that enough -- the upload reclaims the same bindless index, so the baked material
//     is correct again without re-registering anything.
//
// The pin exemption survives only as a failsafe, gated on the sweep having completed a pass
// recently; WGR_RETAINED_LIVENESS=0 restores the old unconditional exemption for an A/B.
//
// Separately, the `Texture with 'wgr_texture' label is invalid` failure seen on Chernarus is
// most likely NOT an eviction race at all: textures.rs calls device.create_texture with no
// error scope, so an allocation the driver refuses yields an *invalid* texture whose view
// still enters the bindless array and poisons it every frame afterwards. That is the same
// failure mode already documented for the geometry pool at lib.rs (`Buffer with
// 'wgr_geo_pool_vbuf' label is invalid`) -- a creation failure reported late, not a
// use-after-free.
//
// Until the Rust side moves, this bounds the demand instead of improving the supply.
int TextureMipBias()
{
    // RFG-085: a live lever (Streaming tab), seeded from WGR_TEXTURE_MIP_BIAS. Clamped: a
    // bias large enough to consume the whole chain would leave nothing to upload, and the
    // floor below keeps at least one level regardless.
    return std::clamp(Poseidon::Dev::GResidencyLevers().textureMipBias, 0, 4);
}

bool TextureWgpu::AdaptiveMipCandidate()
{
    static const bool enabled = [] { const char* v = std::getenv("WGR_ADAPTIVE_TEXTURE_DETAIL"); return v && v[0] == '1'; }();
    if (!enabled) return false;
    // Narrow prototype: immutable in-memory DDS BCR sources, opaque object colour only.
    // No archive reads, composites, cutouts, normals, dynamic/UI images or borrowed tint views.
    if (!_gpuHandle || !_slotLease || _dynamic || _tintDelegate || _cpuTintView || !_src ||
        !dynamic_cast<TextureSourceDDS*>(static_cast<ITextureSource*>(_src)) ||
        _nMipmaps < 2 || TextureMipBias() != 0)
        return false;
    const char* name = SourceName();
    if (!name) return false;
    const size_t length = std::strlen(name);
    const auto suffix = [&](const char* ending) {
        const size_t n = std::strlen(ending);
        return length >= n && strcmpi(name + length - n, ending) == 0;
    };
    return (suffix("_bcr.edds") || suffix("_bcr.dds")) &&
        BcFormatFor(_mipmaps[0].DstFormat()) >= 0 && GetAlphaClass() == AlphaStats::Opaque;
}

bool TextureWgpu::ReplaceMipTail(int firstLevel, uint64_t frame)
{
    if (!AdaptiveMipCandidate() || firstLevel == _uploadedFirstLevel || firstLevel < 0 || firstLevel >= _nMipmaps)
        return false;
    const PacFormat format = _mipmaps[0].DstFormat();
    const int count = _nMipmaps - firstLevel;
    std::vector<size_t> offsets(static_cast<size_t>(count));
    size_t bytes = 0;
    for (int i = 0; i < count; ++i) {
        offsets[i] = bytes;
        const auto& mip = _mipmaps[firstLevel + i];
        bytes += render::mipmap::ComputeLayout(format, mip._w, mip._h).dataSize;
    }
    // One bounded upload, retaining the old valid image. Include new image + staging
    // overlap; trackedBytes also includes submitted old textures awaiting completion.
    constexpr size_t maxUpload = 4 * 1024 * 1024;
    WgrMemoryStats memory{};
    if (bytes == 0 || bytes > maxUpload || !EngineWgpu::LastMemoryStats(memory) ||
        !memory.budget_bytes || memory.tracked_bytes >= memory.budget_bytes ||
        bytes * 2 > memory.budget_bytes - memory.tracked_bytes)
        return false;
    std::vector<uint8_t> blocks(bytes);
    std::vector<MipmapRead> reads(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        reads[i] = MipmapRead{blocks.data() + offsets[i], &_mipmaps[firstLevel + i], firstLevel + i};
    if (!_src->GetMipmapChain(reads.data(), count)) return false; // DDS copies resident CPU blocks
    CheckProducerMayTouchRenderer("adaptive mip replacement");
    auto* renderer = _bank->Renderer();
    const auto& top = _mipmaps[firstLevel];
    const uint64_t next = wgr_texture_create_in_slot(renderer, top._w, top._h, BcFormatFor(format),
        count, 0, blocks.data(), static_cast<uint32_t>(bytes), _slotLease);
    if (!next) return false; // old handle and leased descriptor remain valid
    const uint64_t previous = _gpuHandle;
    _gpuHandle = next;
    _w = top._w; _h = top._h;
    const int oldBias = _uploadedFirstLevel;
    _uploadedFirstLevel = firstLevel;
    _mipDemand.Replaced(frame);
    wgr_texture_destroy(renderer, previous);
    GTextureStreamStats.frameUploadBytes.fetch_add(bytes, std::memory_order_relaxed);
    LOG_INFO(Graphics, "Adaptive texture detail: mip {} -> {} bytes={} slot={} source='{}'",
        oldBias, firstLevel, bytes, _slotLease, SourceName());
    return true;
}

bool TextureWgpu::CaptureColdPaaRead(render::ColdPaaRead& out) const
{
    if (!render::ColdPaaHandoffEnabled() || !Foundation::IsMainThread() || _gpuHandle ||
        !_src || _dynamic || _tintDelegate || _cpuTintView || _nMipmaps <= 0 || _nMipmaps > MAX_MIPMAPS) return false;
    auto source = CurrentArchiveSourceBinding();
    if (!source) return false; // Actual Init-buffer provenance + current loaded mount; no fresh-name guess.
    const char* name = SourceName(); size_t length = 0;
    while (name && length < 8192 && name[length]) ++length;
    if (!name || length < 4 || length >= 8192 || strcmpi(name + length - 4, ".paa") != 0) return false;
    render::ColdPaaRead candidate;
    candidate.key = render::PreparedTextureStore::Key(name); candidate.source = std::move(source);
    const PacFormat format = _src->GetFormat();
    if (format < PacDXT1 || format > PacDXT5) return false;
    // PacFormat is not required to be numerically contiguous.
    switch (format) {
    case PacDXT1: candidate.magic = 0xff01; break; case PacDXT2: candidate.magic = 0xff02; break;
    case PacDXT3: candidate.magic = 0xff03; break; case PacDXT4: candidate.magic = 0xff04; break;
    case PacDXT5: candidate.magic = 0xff05; break; default: return false;
    }
    candidate.count = static_cast<size_t>(_nMipmaps);
    for (int i = 0; i < _nMipmaps; ++i) {
        const auto& mip = _mipmaps[i];
        if (mip.SrcFormat() != format || mip.DstFormat() != format || mip._start < 0) return false;
        candidate.levels[i] = {mip._w, mip._h,
            static_cast<size_t>(render::mipmap::ComputeLayout(format, mip._w, mip._h).dataSize), static_cast<size_t>(mip._start)};
    }
    if (!candidate.Valid()) return false;
    out = std::move(candidate); return true;
}

uint64_t TextureWgpu::EnsureUploaded()
{
    if (_bank)
    {
        _bank->NoteTextureUse(this);
    }
    if (auto* reuseOnly = WgpuTextureReuseOnlyBinding::Active())
    {
        // Final owner binding may only reuse a held, unchanged resident image.
        // Refusals leave deferred pixels, upload retry state and source proof intact.
        return reuseOnly->Check(this, {_gpuHandle, _slotLease, _dynamic,
                                       _deferredGenerated != nullptr});
    }
    if (_deferredGenerated)
    {
        // Move first so a reentrant consumer cannot upload the same payload twice.
        auto pending = std::move(_deferredGenerated);
        auto& ledger = *pending->ledger;
        if (!render::procedural::admission::SameBirthName(Name(), pending->birthName.c_str()))
        {
            ledger.NoteNameMismatch();
            ledger.NoteFailed();
            _uploadTried = true; // generated names have no file-backed retry path
            return 0;
        }
        InitDynamic(_w, _h, pending->rgba.data(), static_cast<uint32_t>(pending->rgba.size()));
        render::procedural::admission::NoteUploadAttempt(ledger, _gpuHandle != 0);
        return _gpuHandle;
    }
    // RFG-086: an `enft|r,g,b|inner` face texture draws through its INNER image.
    //
    // The tint used to be BAKED: DdsImport decoded the BC7 tile, multiplied it, and the
    // result went to the GPU as RGBA8 with generated mips -- 5.6 MB for a 1024^2 tile that
    // is 1.4 MB as BC7, and one such copy per (tint, tile) pair, because every wall colour
    // is its own name. Measured on native Everon at the owner's play configuration (800
    // models, 300 m) once RFG-075 sent the MatPBRMulti walls down the retained path:
    // 671 RGBA8 textures of the 1024 class, 3,480 MB, against 635 MB for the 514 BC7 ones
    // -- and a lost device at 7.5 GB tracked on an 8 GB card.
    //
    // The retained path already carries the same colour as `layer_colour[0]` (RFG-072) and
    // multiplies it in the shader, so for it the bake was not just 4x the bytes, it was the
    // tint applied TWICE on every `enft|enfa|...` composite. Delegating to the inner name
    // keeps the shared BC7 tile shared and lets the shader do the one multiply. The
    // direct (per-draw) path has no layer colour and draws these untinted now; RFG-075
    // leaves it almost nothing of this family, and POSEIDON_LAYER_TINT_BAKED=1 (or the
    // Streaming tab) restores the bake for an A/B.
    if (_tintDelegate && (!_cpuTintView || Poseidon::Dev::GResidencyLevers().layerTintInShader))
    {
        auto* inner = static_cast<TextureWgpu*>(_tintDelegate.GetRef());
        return inner ? inner->EnsureUploaded() : 0;
    }
    if (_bank && !_uploadTried && Poseidon::Dev::GResidencyLevers().layerTintInShader)
    {
        const char* innerName = nullptr;
        float       rgb[3];
        if (Poseidon::Enfusion::SplitLayerTintName(Name(), rgb, &innerName) && innerName != nullptr &&
            *innerName != '\0')
        {
            Ref<Texture> inner = _bank->Load(innerName);
            if (inner && inner.GetRef() != this)
            {
                _tintDelegate = inner;
                static std::atomic<uint32_t> delegated{0};
                if (delegated.fetch_add(1, std::memory_order_relaxed) < 4)
                    LOG_INFO(Graphics, "Wgpu texture: layer tint in shader -- '{}' draws through '{}'", Name(),
                             innerName);
                return static_cast<TextureWgpu*>(inner.GetRef())->EnsureUploaded();
            }
        }
    }
    if (_gpuHandle || _uploadTried)
    {
        RetireHotSourceProof();
        return _gpuHandle;
    }
    TerrainColdUploadTiming terrainTiming(TerrainPageUploadTimingsEnabled() ? SourceName() : nullptr);
    _uploadTried = true;

    WgrRenderer* r = _bank ? _bank->Renderer() : nullptr;
    if (!r || _w <= 0 || _h <= 0)
    {
        // The one silent way a NAMED texture becomes handle 0.
        //
        // Init() runs at Load() time and its return value is discarded (TextureBankWgpu::Load
        // calls `texture->Init();` and returns the object either way), so a texture whose file
        // is missing, whose factory refuses it, or whose header will not parse leaves _w == 0
        // and reaches here. Returning 0 hands the caller a zero handle, which
        // SharedTextures::texture_slot maps to bindless slot 0 -- the 1x1 WHITE texel -- and
        // the surface draws at albedo 1.0 with no log line anywhere. The warning below the
        // fallback path ("failed to upload texture") never fires for these, because this
        // return is above it: measured on three Stratis runs, zero occurrences of that warning
        // while pure-white geometry was on screen.
        //
        // Cannot spam: _uploadTried was set to true above, so a given texture reaches this at
        // most once per session.
        LOG_WARN(Graphics,
                 "Wgpu: texture {} has no uploadable image ({}, {}x{}, mips={}) -- draws on the white fallback",
                 Name() ? Name() : "<unnamed>", r ? "source failed to initialise" : "no renderer", _w, _h,
                 _nMipmaps);
        return 0;
    }

    const bool timed = TextureStreamStatsEnabled();
    // The per-admission split (ObjectAdmitProfile): active only while LandSave's admit loop
    // has a scope open around one ObjectCreate. One bool load on the resident fast path above
    // is not paid at all -- this is below the early return -- and here it costs a clock pair
    // per REAL upload, which is a file read away from mattering.
    render::ObjectAdmitProfile& admit = render::GObjectAdmitProfile;
    const bool profiled = admit.active;
    double diagnosticReadMs = 0.0;
    size_t diagnosticBytes = 0;
    bool diagnosticPrepared = false;
    if (profiled)
    {
        ++admit.textureUploads;
    }
    const PacFormat dst = _nMipmaps > 0 ? _mipmaps[0].DstFormat() : PacFormatN;
    const int bcFormat = BcFormatFor(dst);
    if (bcFormat >= 0 && _src)
    {
        // Full mip chain, tightly packed as wgr_texture_create expects: PAA mips
        // halve exactly, so level i is (_w>>i, _h>>i).
        //
        // Sized in one pass, then read in ONE call. The old shape — resize the vector
        // inside the loop, ask the source for one level per iteration — paid twice over
        // during the streaming transient: the growing resize recopies the chain up to
        // _nMipmaps times, and TextureSourcePac answers each per-level request by
        // re-opening the texture through the VFS, so a texture cost _nMipmaps opens
        // (up to MAX_MIPMAPS = 7) instead of one. GetMipmapChain's default implementation
        // is the old loop, so nothing changes for a source that has no per-call cost.
        // WGR_TEXTURE_MIP_BIAS drops the most detailed levels: the chain simply STARTS
        // lower. Always leaves at least one level, so a heavily-biased small texture
        // degrades to its coarsest level rather than to nothing.
        const int firstLevel = std::min(TextureMipBias(), _nMipmaps - 1);
        const int levelCount = _nMipmaps - firstLevel;
        std::vector<size_t> offsets(static_cast<size_t>(levelCount));
        size_t total = 0;
        for (int i = 0; i < levelCount; i++)
        {
            offsets[i] = total;
            const PacLevelMem& mip = _mipmaps[firstLevel + i];
            total += render::mipmap::ComputeLayout(dst, mip._w, mip._h).dataSize;
        }
        // Destinations are resolved only once the buffer is at its final size — a pointer
        // taken before the allocation would not survive it.
        std::vector<uint8_t> blocks;
        const uint64_t readBegan = (timed || profiled) ? NowUs() : 0;
        // TEXTURE STREAMING: a worker may already have read and inflated this chain
        // (ObjectStreamPreparer -> ReadPAABlockChain -> PreparedTextureStore) while the model
        // it belongs to was being parsed. Take it if so -- but trust nothing about it that
        // this side can check: the source format, the level count, and every level's
        // dimensions and byte size must agree with the header the bank parsed through the
        // file server at Load(). A chain that fails any of those is dropped and the file is
        // read exactly as before; the miss is counted so a silent fallback cannot masquerade
        // as a working prefetch. `blocks` ends up in the same layout either way: the levels
        // firstLevel.. tightly packed at `offsets`, so the alpha handoff below is unchanged.
        bool ok = false;
        bool fromPrepared = false;
        bool fromColdHandoff = false;
        std::shared_ptr<const ArchiveSourceBinding> warmPreparedSource;
        BankReadRequest physicalPreparedSource;
        bool physicalPrepared = false;
        if (render::ColdPaaHandoffEnabled() || render::PreparedTextureStore::Enabled())
        {
            PAABlockChain prepared;
            const std::string key = render::PreparedTextureStore::Key(SourceName());
            const uint64_t takeBegan = terrainTiming.enabled ? NowUs() : 0;
            render::ColdPaaRead currentHeader;
            if (render::ColdPaaHandoffEnabled() && CaptureColdPaaRead(currentHeader))
                fromColdHandoff = render::ColdPaaAdmissionScope::Claim(key, currentHeader.source, currentHeader, prepared);
            if (auto* proof = Streaming::WarmTextureProvenance::Active())
            {
                proof->FlushPromotions(); // bounded published-entry witness, outside queue/store mutex
                proof->FlushRetirements(); // emits outside PreparedTextureStore mutex
                const auto current = CurrentArchiveSourceBinding(); // Existing no-read loaded-mount check, opt-in only.
                const uint32_t token = current ? Streaming::WarmTextureProvenance::Match(key, current) : proof->KeyOnly(key);
                Streaming::WarmTextureProvenance::Emit(proof->Stamp(token, current ?
                    Streaming::WarmTextureProvenance::Event::OwnerExactAttempt :
                    Streaming::WarmTextureProvenance::Event::OwnerKeyOnlyAttempt));
            }
            static const bool physicalBuildingPilot = [] {
                const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_PILOT");
                return value && std::strcmp(value, "1") == 0;
            }();
            static const bool physicalMultistage = [] {
                const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_MULTISTAGE");
                return value && std::strcmp(value, "1") == 0;
            }();
            const bool pilotKey = physicalBuildingPilot &&
                (key == R"(dz\structures\data\plaster\plaster_flats01_nohq.paa)" ||
                 key == R"(dz\structures\data\concrete\concrete_bare4_nohq.paa)" ||
                 key == R"(dz\structures\data\plaster\police_station_wall_nohq.paa)");
            const bool multistageKey = physicalMultistage &&
                (key == R"(dz\structures\data\plaster\plaster_flats02_nohq.paa)" ||
                 key == R"(dz\structures\data\concrete\concrete_panels_dirty_nohq.paa)" ||
                 key == R"(dz\structures\data\concrete\concrete_panels_nohq.paa)" ||
                 key == R"(dz\structures\data\plaster\plaster_flats03_nohq.paa)");
            if (!fromColdHandoff && _src &&
                (pilotKey || multistageKey || TenementPhysicalPaaScope::Active()))
            {
                BankReadMemberIdentity initializedMember;
                if (_src->CopyArchiveMemberIdentity(initializedMember))
                    physicalPrepared = render::PreparedTextureStore::Instance().TakePhysical(
                        key, initializedMember, prepared, physicalPreparedSource);
            }
            const bool taken = fromColdHandoff || physicalPrepared ||
                (render::PreparedTextureStore::Enabled() &&
                 render::PreparedTextureStore::Instance().Take(key, prepared, &warmPreparedSource));
            if (terrainTiming.enabled)
            {
                terrainTiming.preparedTakeMs += (NowUs() - takeBegan) / 1000.0;
                terrainTiming.preparedTaken = taken;
            }
            if (taken)
            {
                const PacFormat preparedFormat = PacFormatFromMagic(prepared.magic);
                bool matches = preparedFormat == dst &&
                               prepared.levels.size() >= static_cast<size_t>(firstLevel + levelCount);
                bool sourceRefusedForTrace = false;
                if (matches && physicalPrepared)
                {
                    const char* name = SourceName();
                    QFBank* bank = name ? QIFStreamB::AutoBank(name) : nullptr;
                    const size_t length = name ? std::strlen(name) : 0;
                    const size_t prefix = bank ? static_cast<size_t>(bank->GetPrefix().GetLength()) : length;
                    matches = bank && prefix < length &&
                        bank->MatchesMountedMember(name + prefix, physicalPreparedSource);
                    sourceRefusedForTrace = !matches;
                }
                if (matches && warmPreparedSource)
                {
                    const auto currentSource = CurrentArchiveSourceBinding();
                    matches = currentSource && currentSource->Request().SameArchiveMember(warmPreparedSource->Request());
                    sourceRefusedForTrace = !matches;
                    if (auto* proof = Streaming::WarmTextureProvenance::Active())
                        Streaming::WarmTextureProvenance::Emit(proof->Stamp(
                            Streaming::WarmTextureProvenance::Match(key, warmPreparedSource), matches ?
                            Streaming::WarmTextureProvenance::Event::SourceValidated :
                            Streaming::WarmTextureProvenance::Event::SourceRefused));
                }
                for (int i = 0; matches && i < levelCount; i++)
                {
                    const PAABlockLevel& level = prepared.levels[static_cast<size_t>(firstLevel + i)];
                    const PacLevelMem& mip = _mipmaps[firstLevel + i];
                    const size_t want = static_cast<size_t>(render::mipmap::ComputeLayout(dst, mip._w, mip._h).dataSize);
                    matches = level.width == mip._w && level.height == mip._h && level.size == want &&
                              level.offset + level.size <= prepared.blocks.size() && offsets[i] + want <= total;
                }
                if (!matches && warmPreparedSource && !sourceRefusedForTrace)
                {
                    if (auto* proof = Streaming::WarmTextureProvenance::Active())
                        Streaming::WarmTextureProvenance::Emit(proof->Stamp(
                            Streaming::WarmTextureProvenance::Match(key, warmPreparedSource),
                            Streaming::WarmTextureProvenance::Event::LayoutRefused));
                }
                if (matches)
                {
                    if (firstLevel == 0 && prepared.levels[0].offset == 0)
                    {
                        // The common case: identical layout from byte 0. Steal the buffer and
                        // cut the tail levels the bank's header stopped short of (MIN_MIP_SIZE
                        // in TextureSourcePac::Init), so wgr_texture_create sees exactly
                        // `total` bytes for `levelCount` levels.
                        blocks = std::move(prepared.blocks);
                        blocks.resize(total);
                    }
                    else
                    {
                        blocks.resize(total);
                        for (int i = 0; i < levelCount; i++)
                        {
                            const PAABlockLevel& level = prepared.levels[static_cast<size_t>(firstLevel + i)];
                            std::memcpy(blocks.data() + offsets[i], prepared.blocks.data() + level.offset, level.size);
                        }
                    }
                    ok = true;
                    fromPrepared = true;
                }
                else
                {
                    static std::atomic<uint32_t> loggedMismatch{0};
                    if (loggedMismatch.fetch_add(1, std::memory_order_relaxed) < 8)
                    {
                        LOG_WARN(Graphics,
                                 "Wgpu texture prepare: prepared chain for {} did not match its header (prepared "
                                 "magic=0x{:x} {}x{} levels={} vs bank format={} {}x{} levels={} first={}) -- "
                                 "reading the file instead",
                                 Name(), prepared.magic, prepared.width, prepared.height, prepared.levels.size(),
                                 int(dst), _w, _h, levelCount, firstLevel);
                    }
                }
            }
        }
        if (!fromPrepared)
        {
            blocks.assign(total, 0);
            std::vector<MipmapRead> reads(static_cast<size_t>(levelCount));
            for (int i = 0; i < levelCount; i++)
            {
                reads[i] = MipmapRead{blocks.data() + offsets[i], &_mipmaps[firstLevel + i], firstLevel + i};
            }
            const uint64_t sourceReadBegan = terrainTiming.enabled ? NowUs() : 0;
            ok = _src->GetMipmapChain(reads.data(), levelCount);
            if (terrainTiming.enabled) terrainTiming.blockReadMs += (NowUs() - sourceReadBegan) / 1000.0;
        }
        if (terrainTiming.enabled) terrainTiming.preparedUsed = fromPrepared;
        if (warmPreparedSource && !fromPrepared)
            render::PreparedTextureStore::Instance().NoteWarmUpload(false, 0);
        // The frame BYTE accumulator is unconditional: the admission loop's per-frame upload
        // budget (WGR_OBJECT_STREAM_UPLOAD_MB) reads it, and a diagnostics opt-out
        // (WGR_TEXTURE_STREAM_STATS=0) must not silently disarm a throttle. One relaxed
        // fetch_add per real upload; only the CLOCK reads stay behind `timed`.
        GTextureStreamStats.frameUploadBytes.fetch_add(total, std::memory_order_relaxed);
        if (timed)
        {
            const uint64_t readDelta = NowUs() - readBegan;
            GTextureStreamStats.readUs.fetch_add(readDelta, std::memory_order_relaxed);
            GTextureStreamStats.blockBytes.fetch_add(total, std::memory_order_relaxed);
            GTextureStreamStats.frameUploadUs.fetch_add(readDelta, std::memory_order_relaxed);
            if (fromPrepared)
                GTextureStreamStats.preparedHits.fetch_add(1, std::memory_order_relaxed);
        }
        if (profiled)
        {
            diagnosticReadMs = (NowUs() - readBegan) / 1000.0;
            admit.textureReadMs += diagnosticReadMs;
            if (fromPrepared)
                ++admit.texturePrepared;
        }
        diagnosticBytes = total;
        diagnosticPrepared = fromPrepared;
        if (ok && !blocks.empty())
        {
            const uint64_t createBegan = (timed || profiled) ? NowUs() : 0;
            // Dimensions follow the level actually uploaded, not the file's level 0 —
            // handing wgpu the full-resolution size with a biased chain describes a
            // texture whose level 0 is missing, which is a validation error, not a
            // smaller texture. _w/_h are updated below so samplers and any later reader
            // see the size that really exists on the GPU.
            _uploadedFirstLevel = firstLevel;
            _w = _mipmaps[firstLevel]._w;
            _h = _mipmaps[firstLevel]._h;
            // Into this texture's LEASED slot, so a re-upload after an eviction reclaims
            // the bindless index registered models already baked (REN-RES-001).
            const uint64_t terrainCreateBegan = terrainTiming.enabled ? NowUs() : 0;
            CheckProducerMayTouchRenderer("texture create");
            _gpuHandle = wgr_texture_create_in_slot(r, static_cast<uint32_t>(_w), static_cast<uint32_t>(_h), bcFormat,
                                                    static_cast<uint32_t>(levelCount), 0, blocks.data(),
                                                    static_cast<uint32_t>(blocks.size()), AcquireSlotLease());
            if (terrainTiming.enabled) terrainTiming.compressedCreateOwnerMs += (NowUs() - terrainCreateBegan) / 1000.0;
            if (_gpuHandle)
                NoteUploadSize(_w, _h, blocks.size(), true);
            if (timed)
            {
                const uint64_t createDelta = NowUs() - createBegan;
                GTextureStreamStats.createUs.fetch_add(createDelta, std::memory_order_relaxed);
                GTextureStreamStats.frameUploadUs.fetch_add(createDelta, std::memory_order_relaxed);
            }
            if (profiled)
            {
                admit.textureCreateMs += (NowUs() - createBegan) / 1000.0;
            }
            // Both call sites classify this texture on the line after they upload it. Park
            // the level they will need so that call reads no file. The condition is exactly
            // GetAlphaClass's: a texture that will never reach a scan -- no alpha, a 1-bit
            // format, or a class already decided -- is not worth parking, and one that is
            // costs only a vector move, never a decode.
            const char* name = SourceName();
            // `firstLevel == 0` is load-bearing, not belt-and-braces.  Under
            // WGR_TEXTURE_MIP_BIAS the chain STARTS lower, so `blocks` does not contain the
            // top mip at all — and the classification needs precisely the top mip, because a
            // smaller level blurs a cutout's crisp 0/255 holes into false partial alpha and
            // mis-routes the section to the blend pass.  Parking a biased chain also made
            // `offsets[1]` a heap over-read whenever the bias left a single level
            // (`offsets` has `levelCount` entries, but the guard tested `_nMipmaps`), and the
            // garbage size then fed a bounds check.  With the park confined to the unbiased
            // case, `levelCount == _nMipmaps` and the index is in range by construction.
            // A biased run simply misses the handoff and re-reads level 0 from the file,
            // which is slower and correct.
            if (_gpuHandle && firstLevel == 0 && _alphaClass < 0 && !_alphaStatsScanned && _src->IsAlpha() &&
                IsBlockAlphaFormat(dst) && name && *name)
            {
                TopMipHandoff& handoff = GTopMipHandoff;
                // offsets[1] is where level 1 begins, i.e. exactly the size of level 0.
                handoff.topSize = levelCount > 1 ? offsets[1] : total;
                handoff.blocks = std::move(blocks);
                handoff.w = _mipmaps[0]._w;
                handoff.h = _mipmaps[0]._h;
                handoff.format = dst;
                handoff.name.assign(name);
            }
        }
        if (fromColdHandoff && fromPrepared && _gpuHandle && Foundation::IsMainThread())
        {
            static unsigned rows = 0;
            if (rows < 16) {
                ++rows;
                LOG_INFO(Graphics, "Cold PAA handoff upload: source={} bytes={} sourceValidated=true blockUploadSucceeded=true operationLocal=true row={} limit=16",
                    SourceName(), total, rows);
            } else if (rows == 16) { ++rows; LOG_INFO(Graphics, "Cold PAA handoff upload truncated: limit=16; prefix only, no all-texture/model performance proof"); }
        }
        if (auto* proof = Streaming::WarmTextureProvenance::Active())
        {
            if (fromPrepared && warmPreparedSource)
                Streaming::WarmTextureProvenance::Emit(proof->Stamp(
                    Streaming::WarmTextureProvenance::Match(render::PreparedTextureStore::Key(SourceName()), warmPreparedSource),
                    _gpuHandle ? Streaming::WarmTextureProvenance::Event::UploadSucceeded :
                    Streaming::WarmTextureProvenance::Event::UploadFailed, total));
            else if (_gpuHandle)
            {
                const auto current = CurrentArchiveSourceBinding();
                if (current)
                    Streaming::WarmTextureProvenance::Emit(proof->Stamp(
                        Streaming::WarmTextureProvenance::Match(render::PreparedTextureStore::Key(SourceName()), current),
                        Streaming::WarmTextureProvenance::Event::OrdinaryUpload, total));
            }
        }
        if (warmPreparedSource && fromPrepared)
        {
            render::PreparedTextureStore::Instance().NoteWarmUpload(_gpuHandle != 0, total);
            // Optional successful source-validated upload witness, not material-slot role.
            // First64 rows only; no reads, clocks or provenance recapture.
            static const bool warmUploadWitness = [] {
                const char* profile = std::getenv("WGR_OBJECT_STREAM_WARM_PROFILE");
                return profile && std::strcmp(profile, "1") == 0 &&
                    ArchiveSourceBinding::ModelReadScope::PurposeRequired();
            }();
            if (warmUploadWitness && _gpuHandle && Foundation::IsMainThread())
            {
                static unsigned rows = 0; // joined owner only
                if (rows < 64)
                {
                    ++rows;
                    const char* sourceName = SourceName();
                    LOG_INFO(Graphics, "Warm prepared upload witness: source={} bytes={} sourceMemberBytes={} sourceValidated=true blockUploadSucceeded=true row={} limit=64",
                        sourceName ? sourceName : "", total, warmPreparedSource->Request().bytes, rows);
                }
                else if (rows == 64)
                {
                    ++rows;
                    LOG_INFO(Graphics, "Warm prepared upload witness truncated: limit=64; observed prefix only, no material-slot or whole-model proof");
                }
            }
        }
        if (physicalPrepared && fromPrepared && _gpuHandle && Foundation::IsMainThread())
        {
            static unsigned rows = 0; // owner only; exact opt-in fixture
            if (rows < 7)
            {
                ++rows;
                LOG_INFO(Graphics, "RVMAT raP physical upload: source={} bytes={} memberBytes={} initMemberMatched=true currentMountMatched=true layoutMatched=true blockUploadSucceeded=true row={} limit=7",
                    SourceName(), total, physicalPreparedSource.bytes, rows);
            }
            if (TenementPhysicalPaaScope::Active())
            {
                static unsigned tenementRows = 0;
                static const unsigned tenementLimit = [] {
                    const char* proxy = std::getenv("WGR_OBJECT_STREAM_DAYZ_PROXY_PREFETCH");
                    return proxy && std::strcmp(proxy, "1") == 0 ? 96u : 64u;
                }();
                if (tenementRows < tenementLimit)
                {
                    ++tenementRows;
                    LOG_INFO(Graphics, "DayZ physical prefetch upload: source={} bytes={} memberBytes={} initMemberMatched=true currentMountMatched=true layoutMatched=true blockUploadSucceeded=true row={} limit={}",
                        SourceName(), total, physicalPreparedSource.bytes, tenementRows, tenementLimit);
                }
                else if (tenementRows == tenementLimit)
                {
                    ++tenementRows;
                    LOG_INFO(Graphics, "DayZ physical prefetch upload truncated: limit={}; observed prefix only", tenementLimit);
                }
            }
        }
    }

    // Fallback (non-DXT formats, or a failed block upload): decode the whole file
    // to RGBA8 via the shared PAA decoder -- unless a worker already did exactly that
    // (PreparedTextureStore::TakeDecoded), in which case only the upload remains.
    if (!_gpuHandle)
    {
        const uint64_t began = (timed || profiled) ? NowUs() : 0;
        const uint64_t terrainFallbackBegan = terrainTiming.enabled ? NowUs() : 0;
        // Pure owner-held pixel work can overlap the preceding render. Acquire
        // renderer ownership only after encoding, immediately before each GPU call.
        auto encodeBc3 = [&](const uint8_t* rgba, int width, int height,
                             std::vector<uint8_t>& blocks, std::vector<uint32_t>& offsets, int& levels)
        {
            const uint64_t encodeBegan = profiled ? NowUs() : 0;
            const bool withoutWindow = profiled && !EngineWgpu::ProducerMayTouchRenderer();
            const bool encoded = EncodeBc3ChainRGBA(rgba, width, height, blocks, offsets, levels);
            if (profiled)
            {
                const double encodeMs = (NowUs() - encodeBegan) / 1000.0;
                admit.textureEncodeMs += encodeMs;
                if (withoutWindow)
                {
                    admit.textureEncodeWithoutWindowMs += encodeMs;
                    ++admit.textureEncodesWithoutWindow;
                }
            }
            return encoded;
        };
        DecodedImage prepared;
        // Original M16 animation frames are ARGB4444, so each real upload reaches
        // this decoded path. Never alter alpha classification or phase selection.
        const auto warmMuzzle = [&](std::vector<uint8_t>& pixels, int width, int height) {
            const char* name = Name();
            if (!name || render::StockMuzzleSheetAt(name, width, height) == render::StockMuzzleSheet::None)
                return;
            const bool enabled = render::StockMuzzleAppearanceEnabled();
            const bool changed = render::WarmStockMuzzleSheet(name, width, height,
                pixels.data(), pixels.size(), enabled);
            static const bool trace = [] {
                const char* value = std::getenv("POSEIDON_MUZZLE_FLASH_TRACE");
                return value && value[0] == '1';
            }();
            if (trace)
                LOG_INFO(Graphics, "MUZZLE_FLASH_SHEET texture={} source={} width={} height={} enabled={} remapped={} alpha=authored-bounded",
                         name, SourceName(), width, height, enabled, changed);
        };
        if (render::PreparedTextureStore::Enabled() &&
            render::PreparedTextureStore::Instance().TakeDecoded(render::PreparedTextureStore::Key(SourceName()), prepared))
        {
            warmMuzzle(prepared.rgba, prepared.width, prepared.height);
            std::vector<uint8_t>  bc3;
            std::vector<uint32_t> bc3Offsets;
            int                   bc3Levels = 0;
            if (CompositeWantsBc3(Name()) &&
                encodeBc3(prepared.rgba.data(), prepared.width, prepared.height, bc3, bc3Offsets, bc3Levels))
            {
                const uint64_t terrainCreateBegan = terrainTiming.enabled ? NowUs() : 0;
                CheckProducerMayTouchRenderer("texture create");
                _gpuHandle = wgr_texture_create_in_slot(r, static_cast<uint32_t>(prepared.width),
                                                        static_cast<uint32_t>(prepared.height), WGR_TEXTURE_BC3,
                                                        static_cast<uint32_t>(bc3Levels), 0, bc3.data(),
                                                        static_cast<uint32_t>(bc3.size()), AcquireSlotLease());
                if (terrainTiming.enabled) terrainTiming.fallbackCreateOwnerMs += (NowUs() - terrainCreateBegan) / 1000.0;
                if (_gpuHandle)
                {
                    _w = prepared.width;
                    _h = prepared.height;
                    NoteUploadSize(_w, _h, bc3.size(), true);
                    GTextureStreamStats.frameUploadBytes.fetch_add(bc3.size(), std::memory_order_relaxed);
                }
            }
            if (!_gpuHandle)
            {
                const uint64_t terrainCreateBegan = terrainTiming.enabled ? NowUs() : 0;
                CheckProducerMayTouchRenderer("texture create");
                _gpuHandle = wgr_texture_create(r, static_cast<uint32_t>(prepared.width),
                                                static_cast<uint32_t>(prepared.height), WGR_TEXTURE_RGBA8, 1,
                                                WGR_TEXTURE_GEN_MIPS, prepared.rgba.data(),
                                                static_cast<uint32_t>(prepared.rgba.size()));
                if (terrainTiming.enabled) terrainTiming.fallbackCreateOwnerMs += (NowUs() - terrainCreateBegan) / 1000.0;
            }
            if (_gpuHandle && bc3.empty())
            {
                _w = prepared.width;
                _h = prepared.height;
                NoteUploadSize(_w, _h, prepared.rgba.size() * 4 / 3, false, Name()); // with generated mips
                GTextureStreamStats.frameUploadBytes.fetch_add(prepared.rgba.size(), std::memory_order_relaxed);
                if (timed)
                    GTextureStreamStats.preparedHits.fetch_add(1, std::memory_order_relaxed);
                if (profiled)
                    ++admit.texturePrepared;
            }
        }
        // RFG-018: an UNCOMPRESSED source that is already decoded in memory.
        //
        // Everything below this point re-opens the file and decodes it as a PAA,
        // which is the only route the non-block path ever had. That is fine for a
        // world whose textures are PAAs -- including the CONVERTED Reforger world,
        // whose exporter bakes them, which is exactly why that path works and this
        // one did not. A natively loaded `.edds` is neither: it is already parsed,
        // its pixels are in `_src`, and re-reading it to decode it as a format it
        // is not can only fail.
        //
        // So: if the source has pixels and they are 32-bit, upload them. This is
        // also the general case for any non-DXT source, not a Reforger special.
        if (!_gpuHandle && _src != nullptr && _nMipmaps > 0 &&
            dst == PacARGB8888 && _w > 0 && _h > 0)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(_w) * static_cast<size_t>(_h) * 4);
            const uint64_t pixelReadBegan = terrainTiming.enabled ? NowUs() : 0;
            const bool pixelsRead = _src->GetMipmapData(pixels.data(), _mipmaps[0], 0);
            if (terrainTiming.enabled) terrainTiming.fallbackReadDecodeMs += (NowUs() - pixelReadBegan) / 1000.0;
            if (pixelsRead)
            {
                // PacARGB8888 means B,G,R,A in memory here, and the renderer wants
                // R,G,B,A. Image::FromFile does the same swap at its own call site;
                // skipping it is a blue island.
                if (dst == PacARGB8888)
                    for (size_t i = 0; i + 2 < pixels.size(); i += 4)
                        std::swap(pixels[i], pixels[i + 2]);
                warmMuzzle(pixels, _w, _h);
                std::vector<uint8_t>  bc3;
                std::vector<uint32_t> bc3Offsets;
                int                   bc3Levels = 0;
                bool encoded = false;
                if (CompositeWantsBc3(Name()))
                {
                    Bc3MipChain preparedBc3;
                    auto* preparedSource = dynamic_cast<TextureSourceDDS*>(static_cast<ITextureSource*>(_src));
                    if (preparedSource && preparedSource->TakeCompositeBc3(SourceName(), _w, _h, preparedBc3))
                    {
                        bc3 = std::move(preparedBc3.blocks);
                        bc3Offsets = std::move(preparedBc3.offsets);
                        bc3Levels = preparedBc3.levels;
                        encoded = true;
                        if (profiled)
                        {
                            ++admit.texturePrepared;
                            ++admit.textureBc3PreparedClaims;
                            admit.textureBc3PreparedBytes += bc3.size();
                        }
                    }
                    else
                        encoded = encodeBc3(pixels.data(), _w, _h, bc3, bc3Offsets, bc3Levels);
                }
                if (encoded)
                {
                    const uint64_t terrainCreateBegan = terrainTiming.enabled ? NowUs() : 0;
                    CheckProducerMayTouchRenderer("texture create");
                    _gpuHandle = wgr_texture_create_in_slot(
                        r, static_cast<uint32_t>(_w), static_cast<uint32_t>(_h), WGR_TEXTURE_BC3,
                        static_cast<uint32_t>(bc3Levels), 0, bc3.data(), static_cast<uint32_t>(bc3.size()),
                        AcquireSlotLease());
                    if (terrainTiming.enabled) terrainTiming.fallbackCreateOwnerMs += (NowUs() - terrainCreateBegan) / 1000.0;
                    if (_gpuHandle)
                        NoteUploadSize(_w, _h, bc3.size(), true);
                }
                if (!_gpuHandle)
                {
                    const uint64_t terrainCreateBegan = terrainTiming.enabled ? NowUs() : 0;
                    CheckProducerMayTouchRenderer("texture create");
                    _gpuHandle = wgr_texture_create_in_slot(
                        r, static_cast<uint32_t>(_w), static_cast<uint32_t>(_h), WGR_TEXTURE_RGBA8, 1,
                        WGR_TEXTURE_GEN_MIPS, pixels.data(), static_cast<uint32_t>(pixels.size()), AcquireSlotLease());
                    if (terrainTiming.enabled) terrainTiming.fallbackCreateOwnerMs += (NowUs() - terrainCreateBegan) / 1000.0;
                    NoteUploadSize(_w, _h, pixels.size() * 4 / 3, false, Name());
                }
                GTextureStreamStats.frameUploadBytes.fetch_add(pixels.size(), std::memory_order_relaxed);
            }
        }
        const uint64_t fallbackReadBegan = (terrainTiming.enabled && !_gpuHandle) ? NowUs() : 0;
        QIFStreamB stream;
        if (!_gpuHandle)
            stream.AutoOpen(SourceName());
        const IFileBuffer* fb = _gpuHandle ? nullptr : stream.GetBuffer();
        if (fb && !fb->GetError() && fb->GetSize() > 0)
        {
            DecodedImage img = DecodePAABuffer(fb->GetData(), static_cast<size_t>(fb->GetSize()), IsPaaName(SourceName()));
            if (terrainTiming.enabled && fallbackReadBegan) terrainTiming.fallbackReadDecodeMs += (NowUs() - fallbackReadBegan) / 1000.0;
            if (img.valid())
            {
                warmMuzzle(img.rgba, img.width, img.height);
                const uint64_t terrainCreateBegan = terrainTiming.enabled ? NowUs() : 0;
                CheckProducerMayTouchRenderer("texture create");
                _gpuHandle = wgr_texture_create_in_slot(
                    r, static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), WGR_TEXTURE_RGBA8, 1,
                    WGR_TEXTURE_GEN_MIPS, img.rgba.data(), static_cast<uint32_t>(img.rgba.size()), AcquireSlotLease());
                if (terrainTiming.enabled) terrainTiming.fallbackCreateOwnerMs += (NowUs() - terrainCreateBegan) / 1000.0;
                _w = img.width;
                _h = img.height;
                NoteUploadSize(_w, _h, img.rgba.size() * 4 / 3, false, Name());
                GTextureStreamStats.frameUploadBytes.fetch_add(img.rgba.size(), std::memory_order_relaxed);
            }
        }
        else if (terrainTiming.enabled && fallbackReadBegan)
            terrainTiming.fallbackReadDecodeMs += (NowUs() - fallbackReadBegan) / 1000.0;
        if (terrainTiming.enabled) terrainTiming.fallbackOwnerMs = (NowUs() - terrainFallbackBegan) / 1000.0;
        if (timed)
        {
            const uint64_t fallbackDelta = NowUs() - began;
            GTextureStreamStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
            GTextureStreamStats.fallbackUs.fetch_add(fallbackDelta, std::memory_order_relaxed);
            GTextureStreamStats.frameUploadUs.fetch_add(fallbackDelta, std::memory_order_relaxed);
        }
        // The traverse capture counted 390 of these decodes costing 1.1 s and nothing said
        // WHICH textures or WHY the block path refused them (non-DXT format? no source? a
        // failed block upload?). Bounded: the first 24 name the culprits, which is enough to
        // classify the population without turning a fill-in into a log flood.
        {
            static std::atomic<uint32_t> loggedFallback{0};
            if (loggedFallback.fetch_add(1, std::memory_order_relaxed) < 24)
            {
                LOG_INFO(Graphics,
                         "Wgpu texture fallback decode: {} (format={} bc={} src={} {}x{} mips={} -> {})",
                         Name() ? Name() : "<unnamed>", int(dst), bcFormat, _src ? "yes" : "no", _w, _h, _nMipmaps,
                         _gpuHandle ? "uploaded" : "FAILED");
            }
        }
        if (profiled)
        {
            admit.textureFallbackMs += (NowUs() - began) / 1000.0;
        }
    }

    if (!_gpuHandle)
    {
        LOG_WARN(Graphics, "Wgpu: failed to upload texture {}", Name());
    }
    GTextureStreamStats.frameUploads.fetch_add(1, std::memory_order_relaxed);
    if (timed)
    {
        ReportTextureStreamStats(GTextureStreamStats.uploads.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    // Success or failure, this name has had its one upload attempt: a worker must not spend
    // a file read preparing it, and any chain already prepared for it is dead weight. (Reset
    // by EvictGpu, which is the one way this texture can want bytes again.)
    if (render::PreparedTextureStore::Enabled())
    {
        render::PreparedTextureStore::Instance().MarkUploaded(render::PreparedTextureStore::Key(SourceName()));
    }
    if (terrainTiming.enabled) terrainTiming.uploaded = _gpuHandle != 0;
    if (auto* trace = Streaming::GObjectTextureUploadTrace)
        trace->Record(SourceName(), diagnosticReadMs, diagnosticBytes,
                      diagnosticPrepared, _gpuHandle != 0);
    RetireHotSourceProof(); // After Claim/current-binding validation, upload and existing handoff bookkeeping.
    return _gpuHandle;
}

} // namespace Poseidon
