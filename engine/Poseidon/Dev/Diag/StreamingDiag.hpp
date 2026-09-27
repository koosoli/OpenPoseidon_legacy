#pragma once

// Streaming/asset observability: one shared counter store and one snapshot that every
// consumer reads (dev-panel "Streaming" tab, the --capture-metrics sidecar, and the
// per-frame Perfetto counters).
//
// Three streaming counter systems existed before this header -- the texture-upload split
// (TextureWgpu.cpp), the cold-model preparer (ObjectStreamPreparer::Stats) and the
// worker->main mip-chain store (PreparedTextureStore::Stats) -- and all three were visible
// only as LOG_INFO rows behind environment variables. A capture or a person at the panel
// saw none of them, which is how "textures are the problem" stays a hypothesis. This file
// makes the three land in the same snapshot so the panel and the capture cannot disagree.
//
// The texture counter STORAGE lives here (Poseidon) rather than in the WgpuRenderer static
// lib because the dependency only points one way: WgpuRenderer already depends on Poseidon,
// so it can increment these, and Poseidon-side consumers can read them without a
// Poseidon -> WgpuRenderer link edge or a new Engine vtable slot.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>

namespace Poseidon::Dev
{

// Cumulative texture-upload counters, incremented by TextureWgpu::EnsureUploaded and the
// alpha-classification path. Cumulative on purpose: the streaming transient is ~81 s on
// Everon, and "of the whole fill-in, how much was texture work" is not answerable by a
// per-frame region once the frames stop being interesting. Atomics because the alpha
// counters can be touched from the prepared-texture worker handoff.
struct TextureStreamCounters
{
    std::atomic<uint64_t> uploads{0};    // EnsureUploaded calls that reached a real load
    std::atomic<uint64_t> blockBytes{0}; // mip-chain bytes read as compressed blocks
    std::atomic<uint64_t> readUs{0};     // ...and microseconds spent reading them
    std::atomic<uint64_t> createUs{0};   // microseconds inside wgr_texture_create
    std::atomic<uint64_t> fallbacks{0};  // whole-file RGBA8 decodes (block path unavailable)
    std::atomic<uint64_t> fallbackUs{0};
    std::atomic<uint64_t> alphaScans{0}; // top-mip decodes done only to classify alpha
    std::atomic<uint64_t> alphaUs{0};
    // Of those scans: how many read alpha off the compressed blocks instead of decoding a
    // full RGBA8 image, and how many of THOSE needed no file access at all because the
    // upload had just inflated the level.
    std::atomic<uint64_t> alphaBlockScans{0};
    std::atomic<uint64_t> alphaHandoffs{0};
    // Successful non-consuming ColdOwned classifications; counted only on the opt-in path.
    std::atomic<uint64_t> coldAlphaPeeks{0};
    // Uploads whose mip chain came out of the PreparedTextureStore (a worker read+inflated
    // it ahead of time) vs. read from the file on the main thread.
    std::atomic<uint64_t> preparedHits{0};

    // Per-frame upload accounting, the evidence a GPU-upload budget decision needs
    // (roadmap 1.3): accumulated during the frame by EnsureUploaded, rolled by
    // StreamingFrameRoll into lastFrame*/peakFrame* and zeroed. The cumulative totals
    // above answer "how much of the fill-in was texture work"; these answer "did one
    // frame pay for too much of it at once" -- which only a per-frame figure can.
    std::atomic<uint64_t> frameUploads{0};
    std::atomic<uint64_t> frameUploadBytes{0};
    std::atomic<uint64_t> frameUploadUs{0}; // read + create + fallback time this frame
    uint64_t lastFrameUploads = 0;          // written only by StreamingFrameRoll (main thread)
    uint64_t lastFrameUploadBytes = 0;
    uint64_t lastFrameUploadUs = 0;
    uint64_t peakFrameUploads = 0;
    uint64_t peakFrameUploadBytes = 0;
    uint64_t peakFrameUploadUs = 0;
};

TextureStreamCounters& GTextureStreamCounters();

// RFG-085: the residency LEVERS -- the knobs that trade visual quality or feature scope for
// VRAM, in one place the Streaming tab can edit and the consumers read every time they
// decide. Each is seeded from the environment variable that used to be its only control, so
// a launch script keeps working; the panel changes the live value.
//
//   textureMipBias      WGR_TEXTURE_MIP_BIAS      drops the N finest mips of block-compressed
//                                                 object textures at upload (next uploads only)
//   ownOpaqueAlpha      POSEIDON_OWN_OPAQUE_ALPHA  RFG-075: a section whose texture carries an
//                                                 alpha channel but is opaque-class is still
//                                                 GPU-owned (the MatPBRMulti layer blend)
//   skyBakeMinEnclosed  WGR_SKY_BAKE_MIN_ENCLOSED  RFG-084: a model's sky-visibility volume is
//                                                 kept only if this fraction of voxels is enclosed
//   skyVolumeBudgetMb   WGR_SKY_VOLUME_MB          ... and only up to this many MB in total
struct ResidencyLevers
{
    int   textureMipBias = 0;
    bool  ownOpaqueAlpha = true;
    float skyBakeMinEnclosed = 0.02f;
    int   skyVolumeBudgetMb = 256;
    //   layerTintInShader   POSEIDON_LAYER_TINT_BAKED=1 turns it OFF: RFG-086, an `enft|`
    //                                                 face texture draws through its untinted
    //                                                 BC7 tile and the shader multiplies the
    //                                                 layer colour, instead of a tinted RGBA8 copy
    bool  layerTintInShader = true;
    //   shareRetainedMeshes WGR_SHARE_RETAINED_MESHES=0 turns it OFF: RFG-088, a CPU-path
    //                                                 vertex buffer for a level the retained
    //                                                 path holds draws through that mesh instead
    //                                                 of a second copy in the geometry pool
    bool  shareRetainedMeshes = true;
    //   nativeStreaming POSEIDON_REFORGER_STREAM=0 turns it OFF: RFG-097/099, a native
    //   Reforger world hands every placement to the object stream instead of placing a
    //   radius at load. Measured at the church: 24 ms/frame streamed against 46 ms for
    //   the radius path (419k registered objects), 0 wgpu errors at the 20k budget, and
    //   the world exists beyond 300 m. Read at world load.
    bool  nativeStreaming = true;
    //   compressComposites  POSEIDON_COMPRESS_COMPOSITES=0 turns it OFF: RFG-092, an `enfa|`
    //                                                 leaf composite is encoded to BC3 on the
    //                                                 CPU instead of uploaded as RGBA8 (4x)
    bool  compressComposites = true;
};

inline ResidencyLevers& GResidencyLevers()
{
    static ResidencyLevers levers = []
    {
        ResidencyLevers l;
        if (const char* v = std::getenv("WGR_TEXTURE_MIP_BIAS"))
            l.textureMipBias = std::clamp(std::atoi(v), 0, 4);
        if (const char* v = std::getenv("POSEIDON_OWN_OPAQUE_ALPHA"))
            l.ownOpaqueAlpha = !(*v == '0');
        if (const char* v = std::getenv("WGR_SKY_BAKE_MIN_ENCLOSED"))
            l.skyBakeMinEnclosed = std::clamp((float)std::atof(v), 0.0f, 1.0f);
        if (const char* v = std::getenv("WGR_SKY_VOLUME_MB"))
            l.skyVolumeBudgetMb = std::clamp(std::atoi(v), 0, 4096);
        if (const char* v = std::getenv("POSEIDON_LAYER_TINT_BAKED"))
            l.layerTintInShader = (*v == '0');
        if (const char* v = std::getenv("WGR_SHARE_RETAINED_MESHES"))
            l.shareRetainedMeshes = !(*v == '0');
        if (const char* v = std::getenv("POSEIDON_REFORGER_STREAM"))
            l.nativeStreaming = !(*v == '0');
        if (const char* v = std::getenv("POSEIDON_COMPRESS_COMPOSITES"))
            l.compressComposites = !(*v == '0');
        return l;
    }();
    return levers;
}

// FileCache (the synchronous file server's MRU of whole decompressed files): hits
// share cached bytes, misses pay the open+decompress on the caller's thread -- the
// main thread, always. missUs is therefore main-thread stall time by definition.
// There is no queue-depth counter because there is no queue (ENABLE_OVERLAPPED_IO
// compiles out); Phase 3's "file-read latency and queue depth" is answered by these
// plus "depth = 0 by construction".
struct FileServerCounters
{
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> missUs{0};
    std::atomic<uint64_t> missUsMax{0};
};

FileServerCounters& GFileServerCounters();

// Plain-value copy of the above for consumers.
struct TextureStreamSnapshot
{
    uint64_t uploads = 0;
    uint64_t blockBytes = 0;
    uint64_t readUs = 0;
    uint64_t createUs = 0;
    uint64_t fallbacks = 0;
    uint64_t fallbackUs = 0;
    uint64_t alphaScans = 0;
    uint64_t alphaUs = 0;
    uint64_t alphaBlockScans = 0;
    uint64_t alphaHandoffs = 0;
    uint64_t coldAlphaPeeks = 0;
    uint64_t preparedHits = 0;
    uint64_t lastFrameUploads = 0;
    uint64_t lastFrameUploadBytes = 0;
    uint64_t lastFrameUploadUs = 0;
    uint64_t peakFrameUploads = 0;
    uint64_t peakFrameUploadBytes = 0;
    uint64_t peakFrameUploadUs = 0;
};

// The Landscape residency system's cumulative counters (the `_modernObject*` family that
// today only reaches the "Modern object residency" log rows). Filled by
// Landscape::SnapshotObjectStreamDiag; `valid` stays false when no streamed world is up.
struct ObjectStreamResidencyCounters
{
    bool valid = false;
    uint64_t residentObjects = 0; // visual residency
    uint64_t requiredObjects = 0;
    uint64_t logicalOnlyObjects = 0;
    uint64_t budget = 0;
    uint64_t shapeCacheSize = 0;
    uint64_t shapeCacheLimit = 0;
    uint64_t shapeCacheHits = 0;
    uint64_t shapeCacheInserts = 0;
    uint64_t shapeCacheDrops = 0;
    uint64_t asyncInstalled = 0; // objects admitted on a worker-prepared IR
    uint64_t asyncWaited = 0;    // admission attempts skipped because the IR was not ready
    uint64_t syncCold = 0;       // cold models loaded on the main thread anyway
    uint64_t coldModels = 0;
    double coldParseMs = 0.0;
    double coldAdaptMs = 0.0;
    double coldOptimizeMs = 0.0;
    double coldCreateMs = 0.0;
    double coldTexHeaderMs = 0.0; // header opens (TextureBankWgpu::Load misses) during admission
    uint64_t coldTexHeaderLoads = 0;
    double coldTexReadMs = 0.0;
    double coldTexCreateMs = 0.0;
    double coldTexFallbackMs = 0.0;
    double coldAlphaMs = 0.0;
    uint64_t coldTexUploads = 0;
    uint64_t coldTexPrepared = 0;
    uint64_t parkHits = 0;
    uint64_t parkStale = 0;
    // WHICH OF THE ADMIT LOOP'S BOUNDS ACTUALLY BIT (roadmap 1.3 "cap GPU uploads by bytes
    // and/or milliseconds per frame"). `admitUpdates` is the denominator; the four stop
    // counters are disjoint (first bound to fire ends the update) and their sum is at most
    // `admitUpdates` -- the remainder are updates that drained every candidate.
    //
    // THE TWO `*Armed` FLAGS ARE NOT DECORATION. Both the upload ceiling
    // (WGR_OBJECT_STREAM_UPLOAD_MB, default OFF) and the wall-clock ceiling
    // (WGR_OBJECT_STREAM_MAX_MS) can be switched off, and a disabled bound reports the same
    // 0 as a bound that was armed and never reached. Those are opposite conclusions -- "this
    // lever is not needed" vs "this lever is not on" -- so consumers MUST render an
    // unarmed counter as n/a (panel) or omit the key entirely (JSON) rather than print 0.
    uint64_t admitUpdates = 0;
    uint64_t admitStopCap = 0;
    uint64_t admitStopUpload = 0;
    uint64_t admitStopCeiling = 0;
    uint64_t admitStopBudget = 0;
    // Counted admission (WGR_OBJECT_STREAM_ADMIT, default counted): the two clock stops
    // above are never consulted, so they read 0-because-disabled -- consumers must render
    // them n/a whenever `admitCounted` is set, exactly as for the armed flags. `admitStopCold`
    // is the counted mode's own stop, and `admitColdBudget` the count it stops at.
    uint64_t admitStopCold = 0;
    bool admitCounted = false;
    uint32_t admitColdBudget = 0;
    bool admitUploadCeilingArmed = false;
    bool admitWallCeilingArmed = false;
    bool prepValid = false; // async preparer exists (WGR_OBJECT_STREAM_ASYNC on and a world loaded)
    ObjectStreamPreparer::Stats prep;
};

struct FileServerSnapshot
{
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t missUs = 0;
    uint64_t missUsMax = 0;
};

// What the resident set actually COSTS on the GPU, next to how many objects it is
// (roadmap Phase 7: "use CPU and GPU memory budgets rather than arbitrary item-count
// limits"). The object residency budget is an item count today -- 20,000 placements --
// and nothing reported what that count is worth in bytes, so nobody could set a byte
// budget or say whether the count is generous or ruinous on a given world. These
// numbers are the missing denominator; `bytesPerResidentObject` is the one to watch
// across corpora, because it is what a byte budget would have to be divided by.
//
// Source: Engine::GetGpuMemoryStats (the renderer's own tracked totals; portable
// payload lower bounds, no driver metadata). `valid` is false on backends that do not
// report them (GL33) or with no renderer up.
struct GpuResidencySnapshot
{
    bool valid = false;
    uint64_t trackedBytes = 0;          // textures + geometry the renderer tracks
    uint64_t budgetBytes = 0;           // WGR_DYNAMIC_VRAM_MB (a FIXED default today, not the device's VRAM)
    uint64_t objectTextureBytes = 0;
    uint64_t geometryLiveBytes = 0;
    uint64_t geometryCapacityBytes = 0; // pool capacity, i.e. what is actually allocated
    uint64_t geometryRetiredBytes = 0;
    uint64_t objectTextureRetiredBytes = 0;
    uint64_t backendAllocationBytes = 0;
    unsigned objectTextureCount = 0;
    bool overBudget = false;
    // trackedBytes / resident objects, 0 when nothing is resident. The headline figure:
    // multiply by the item budget to see what that budget promises to cost.
    uint64_t bytesPerResidentObject = 0;
};

struct StreamingSnapshot
{
    TextureStreamSnapshot tex;
    FileServerSnapshot fileserver;
    render::PreparedTextureStore::Stats store;
    ObjectStreamResidencyCounters residency;
    GpuResidencySnapshot gpu;
};

// Assembles the three sources. Cheap (two brief mutex hops); main thread.
StreamingSnapshot CollectStreamingSnapshot();

// Frame boundary: rolls the frame upload accumulators into lastFrame*/peakFrame* and, when
// --perf-trace is active, emits the headline gauges as Perfetto counter (C) events. Called
// once per frame from World::SimulateAndDraw next to FrameProfiler::EndFrame.
void StreamingFrameRoll();

} // namespace Poseidon::Dev
