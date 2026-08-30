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

#include <atomic>
#include <cstdint>

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
    uint64_t residentObjects = 0;
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
    double coldTexReadMs = 0.0;
    double coldTexCreateMs = 0.0;
    double coldTexFallbackMs = 0.0;
    double coldAlphaMs = 0.0;
    uint64_t coldTexUploads = 0;
    uint64_t coldTexPrepared = 0;
    uint64_t parkHits = 0;
    uint64_t parkStale = 0;
    bool prepValid = false; // async preparer exists (WGR_OBJECT_STREAM_ASYNC on and a world loaded)
    ObjectStreamPreparer::Stats prep;
};

struct StreamingSnapshot
{
    TextureStreamSnapshot tex;
    render::PreparedTextureStore::Stats store;
    ObjectStreamResidencyCounters residency;
};

// Assembles the three sources. Cheap (two brief mutex hops); main thread.
StreamingSnapshot CollectStreamingSnapshot();

// Frame boundary: rolls the frame upload accumulators into lastFrame*/peakFrame* and, when
// --perf-trace is active, emits the headline gauges as Perfetto counter (C) events. Called
// once per frame from World::SimulateAndDraw next to FrameProfiler::EndFrame.
void StreamingFrameRoll();

} // namespace Poseidon::Dev
