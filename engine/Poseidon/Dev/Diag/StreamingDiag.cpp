// Streaming/asset observability: snapshot assembly and the per-frame trace counters.
// See the header for why the storage lives on this side of the renderer boundary.

#include <Poseidon/Dev/Diag/StreamingDiag.hpp>

#include <Poseidon/Dev/Diag/PerfTrace.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>

#include <algorithm>
#include <chrono>

namespace Poseidon::Dev
{

TextureStreamCounters& GTextureStreamCounters()
{
    // Function-local so a static-init-order race between this TU and TextureWgpu.cpp
    // cannot hand out an unconstructed object.
    static TextureStreamCounters counters;
    return counters;
}

FileServerCounters& GFileServerCounters()
{
    static FileServerCounters counters;
    return counters;
}

StreamingSnapshot CollectStreamingSnapshot()
{
    StreamingSnapshot out;

    const FileServerCounters& f = GFileServerCounters();
    out.fileserver.hits = f.hits.load(std::memory_order_relaxed);
    out.fileserver.misses = f.misses.load(std::memory_order_relaxed);
    out.fileserver.missUs = f.missUs.load(std::memory_order_relaxed);
    out.fileserver.missUsMax = f.missUsMax.load(std::memory_order_relaxed);

    const TextureStreamCounters& c = GTextureStreamCounters();
    out.tex.uploads = c.uploads.load(std::memory_order_relaxed);
    out.tex.blockBytes = c.blockBytes.load(std::memory_order_relaxed);
    out.tex.readUs = c.readUs.load(std::memory_order_relaxed);
    out.tex.createUs = c.createUs.load(std::memory_order_relaxed);
    out.tex.fallbacks = c.fallbacks.load(std::memory_order_relaxed);
    out.tex.fallbackUs = c.fallbackUs.load(std::memory_order_relaxed);
    out.tex.alphaScans = c.alphaScans.load(std::memory_order_relaxed);
    out.tex.alphaUs = c.alphaUs.load(std::memory_order_relaxed);
    out.tex.alphaBlockScans = c.alphaBlockScans.load(std::memory_order_relaxed);
    out.tex.alphaHandoffs = c.alphaHandoffs.load(std::memory_order_relaxed);
    out.tex.coldAlphaPeeks = c.coldAlphaPeeks.load(std::memory_order_relaxed);
    out.tex.preparedHits = c.preparedHits.load(std::memory_order_relaxed);
    out.tex.lastFrameUploads = c.lastFrameUploads;
    out.tex.lastFrameUploadBytes = c.lastFrameUploadBytes;
    out.tex.lastFrameUploadUs = c.lastFrameUploadUs;
    out.tex.peakFrameUploads = c.peakFrameUploads;
    out.tex.peakFrameUploadBytes = c.peakFrameUploadBytes;
    out.tex.peakFrameUploadUs = c.peakFrameUploadUs;

    out.store = render::PreparedTextureStore::Instance().SnapshotStats();

    // GPU residency, so the item-count budget can finally be read in bytes (Phase 7).
    if (GEngine)
    {
        Engine::GpuMemoryStatsOut mem;
        if (GEngine->GetGpuMemoryStats(mem))
        {
            out.gpu.valid = true;
            out.gpu.trackedBytes = mem.trackedBytes;
            out.gpu.budgetBytes = mem.budgetBytes;
            out.gpu.objectTextureBytes = mem.objectTextureBytes;
            out.gpu.geometryLiveBytes = mem.geometryLiveBytes;
            out.gpu.geometryCapacityBytes = mem.geometryCapacityBytes;
            out.gpu.geometryRetiredBytes = mem.geometryRetiredBytes;
            out.gpu.objectTextureRetiredBytes = mem.objectTextureRetiredBytes;
            out.gpu.backendAllocationBytes = mem.backendAllocationBytes;
            out.gpu.objectTextureCount = mem.objectTextureCount;
            out.gpu.overBudget = mem.overBudget;
        }
    }

    if (GLandscape)
    {
        GLandscape->SnapshotObjectStreamDiag(out.residency);
    }
    // Derive AFTER residency is filled: the whole point is bytes-per-object, and the
    // object count comes from the landscape block below the GPU block above.
    if (out.gpu.valid && out.residency.valid && out.residency.residentObjects > 0)
        out.gpu.bytesPerResidentObject = out.gpu.trackedBytes / out.residency.residentObjects;

    return out;
}

void StreamingFrameRoll()
{
    // Frame boundary for the per-frame upload figures. exchange() rather than load+store:
    // an upload between the two would be lost, and a lost 4 MB spike is exactly the datum
    // this exists to keep.
    TextureStreamCounters& c = GTextureStreamCounters();
    c.lastFrameUploads = c.frameUploads.exchange(0, std::memory_order_relaxed);
    c.lastFrameUploadBytes = c.frameUploadBytes.exchange(0, std::memory_order_relaxed);
    c.lastFrameUploadUs = c.frameUploadUs.exchange(0, std::memory_order_relaxed);
    c.peakFrameUploads = std::max(c.peakFrameUploads, c.lastFrameUploads);
    c.peakFrameUploadBytes = std::max(c.peakFrameUploadBytes, c.lastFrameUploadBytes);
    c.peakFrameUploadUs = std::max(c.peakFrameUploadUs, c.lastFrameUploadUs);

    if (!Perf::Trace::IsEnabled())
    {
        return;
    }
    const StreamingSnapshot s = CollectStreamingSnapshot();
    const int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    // Cumulative counts render as staircases in Perfetto; the gauges (queue depth, ready
    // set, store bytes, resident objects) are the ones that show streaming pressure over
    // time, which is what a trace is for.
    Perf::Trace::PushCounter("Streaming", "texture uploads", ts, static_cast<int64_t>(s.tex.uploads));
    Perf::Trace::PushCounter("Streaming", "frame upload bytes", ts,
                             static_cast<int64_t>(s.tex.lastFrameUploadBytes));
    Perf::Trace::PushCounter("Streaming", "prepared store bytes", ts, static_cast<int64_t>(s.store.bytes));
    if (s.residency.prepValid)
    {
        Perf::Trace::PushCounter("Streaming", "preparer queued", ts, static_cast<int64_t>(s.residency.prep.queued));
        Perf::Trace::PushCounter("Streaming", "preparer ready", ts, static_cast<int64_t>(s.residency.prep.ready));
    }
    if (s.residency.valid)
    {
        Perf::Trace::PushCounter("Streaming", "resident objects", ts,
                                 static_cast<int64_t>(s.residency.residentObjects));
    }
}

} // namespace Poseidon::Dev
