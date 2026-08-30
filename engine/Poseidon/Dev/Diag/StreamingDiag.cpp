// Streaming/asset observability: snapshot assembly and the per-frame trace counters.
// See the header for why the storage lives on this side of the renderer boundary.

#include <Poseidon/Dev/Diag/StreamingDiag.hpp>

#include <Poseidon/Dev/Diag/PerfTrace.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

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

StreamingSnapshot CollectStreamingSnapshot()
{
    StreamingSnapshot out;

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
    out.tex.preparedHits = c.preparedHits.load(std::memory_order_relaxed);

    out.store = render::PreparedTextureStore::Instance().SnapshotStats();

    if (GLandscape)
    {
        GLandscape->SnapshotObjectStreamDiag(out.residency);
    }
    return out;
}

void PushStreamingTraceCounters()
{
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
