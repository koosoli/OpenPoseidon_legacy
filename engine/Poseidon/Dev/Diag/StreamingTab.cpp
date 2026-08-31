// The dev-panel "Streaming" tab: the asset/streaming counters that previously reached only
// LOG_INFO rows behind environment variables. Everything shown here is cumulative since the
// world loaded (or process start, for the texture rows) -- the point is "of the whole
// fill-in, where did the time go", not a per-frame region.

#include <Poseidon/Dev/Diag/StreamingTab.hpp>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp>

// The PCH pulls in Logging.hpp, which #defines DebugLog() as a macro and collides with
// the ImGui::DebugLog() method.  Same dance as DebugOverlay.cpp / BallisticsTab.cpp.
#ifdef DebugLog
#undef DebugLog
#endif
#include <imgui.h>

#include <cstdarg>

namespace Poseidon::Dev
{
namespace
{

constexpr double kMb = 1024.0 * 1024.0;

void Row(const char* label, const char* fmt, ...)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

} // namespace

void DrawStreamingTab()
{
    const StreamingSnapshot s = CollectStreamingSnapshot();

    PanelHeading("Texture uploads");
    PanelHelp("cumulative; WGR_TEXTURE_STREAM_STATS=0 disables the timing half");
    if (ImGui::BeginTable("tex", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        Row("uploads", "%llu (%llu prepared by workers, %llu fallback decodes)",
            (unsigned long long)s.tex.uploads, (unsigned long long)s.tex.preparedHits,
            (unsigned long long)s.tex.fallbacks);
        Row("block bytes", "%.1f MB", s.tex.blockBytes / kMb);
        Row("read / create", "%.1f / %.1f ms", s.tex.readUs / 1000.0, s.tex.createUs / 1000.0);
        Row("fallback", "%.1f ms", s.tex.fallbackUs / 1000.0);
        Row("alpha scans", "%llu (%llu block, %llu handoff), %.1f ms", (unsigned long long)s.tex.alphaScans,
            (unsigned long long)s.tex.alphaBlockScans, (unsigned long long)s.tex.alphaHandoffs,
            s.tex.alphaUs / 1000.0);
        Row("texture total", "%.1f ms",
            (s.tex.readUs + s.tex.createUs + s.tex.fallbackUs + s.tex.alphaUs) / 1000.0);
        // The per-frame pair is the GPU-upload-budget evidence (roadmap 1.3): a large peak
        // says one frame paid for many megabytes of upload at once; a small one says the
        // admit ceiling already bounds this and a byte budget would be a lever without a
        // measured problem.
        Row("last frame", "%llu uploads, %.2f MB, %.2f ms", (unsigned long long)s.tex.lastFrameUploads,
            s.tex.lastFrameUploadBytes / kMb, s.tex.lastFrameUploadUs / 1000.0);
        Row("peak frame", "%llu uploads, %.2f MB, %.2f ms", (unsigned long long)s.tex.peakFrameUploads,
            s.tex.peakFrameUploadBytes / kMb, s.tex.peakFrameUploadUs / 1000.0);
        ImGui::EndTable();
    }

    PanelHeading("File cache");
    PanelHelp("synchronous server: every miss stalls the main thread; queue depth is 0 by construction");
    if (ImGui::BeginTable("fs", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        const uint64_t total = s.fileserver.hits + s.fileserver.misses;
        Row("opens", "%llu (%.0f%% cache hits)", (unsigned long long)total,
            total ? 100.0 * s.fileserver.hits / (double)total : 0.0);
        Row("misses", "%llu, %.1f ms total, %.2f ms worst", (unsigned long long)s.fileserver.misses,
            s.fileserver.missUs / 1000.0, s.fileserver.missUsMax / 1000.0);
        ImGui::EndTable();
    }

    PanelHeading("Prepared-texture store");
    PanelHelp("worker-read mip chains waiting for the main thread's upload");
    if (ImGui::BeginTable("store", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        Row("held", "%zu entries, %.1f MB", s.store.entries, s.store.bytes / kMb);
        Row("puts / takes", "%llu (%.1f MB) / %llu (%.1f MB)", (unsigned long long)s.store.puts,
            s.store.putBytes / kMb, (unsigned long long)s.store.takes, s.store.takeBytes / kMb);
        Row("misses", "%llu", (unsigned long long)s.store.misses);
        // rejectedDup is the number the roadmap's concurrent-request-dedup decision reads:
        // it counts worker reads thrown away because two producers raced on the same name.
        Row("rejected dup / full", "%llu / %llu", (unsigned long long)s.store.rejectedDup,
            (unsigned long long)s.store.rejectedFull);
        Row("expired (TTL) / skipped", "%llu / %llu", (unsigned long long)s.store.expired,
            (unsigned long long)s.store.skippedUploaded);
        Row("decoded (RGBA8) puts / takes", "%llu / %llu", (unsigned long long)s.store.decodedPuts,
            (unsigned long long)s.store.decodedTakes);
        ImGui::EndTable();
    }

    PanelHeading("Cold-model preparer");
    if (!s.residency.prepValid)
    {
        PanelHelp("not running (no streamed world, or WGR_OBJECT_STREAM_ASYNC=0)");
    }
    else
    {
        const ObjectStreamPreparer::Stats& p = s.residency.prep;
        if (ImGui::BeginTable("prep", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            Row("workers", "%u", p.workers);
            Row("queued / ready now", "%zu / %zu", p.queued, p.ready);
            Row("requested", "%llu (%llu refused: queue full)", (unsigned long long)p.requested,
                (unsigned long long)p.rejectedFull);
            Row("prepared / taken", "%llu / %llu", (unsigned long long)p.prepared, (unsigned long long)p.taken);
            Row("not loose / failed / dropped", "%llu / %llu / %llu", (unsigned long long)p.notLoose,
                (unsigned long long)p.failed, (unsigned long long)p.dropped);
            Row("worker parse", "%.0f ms total, %.0f ms slowest", p.workerMs, p.workerMaxMs);
            // The one asset path that is asynchronous today, so the one place "time from
            // request to residency" is a real measurement rather than a synchronous call.
            if (p.prepared > 0)
                Row("request -> ready", "%.1f ms avg, %.1f ms max",
                    p.readyLatencyUsTotal / 1000.0 / (double)p.prepared, p.readyLatencyUsMax / 1000.0);
            if (p.taken > 0)
                Row("request -> installed", "%.1f ms avg, %.1f ms max",
                    p.takeLatencyUsTotal / 1000.0 / (double)p.taken, p.takeLatencyUsMax / 1000.0);
            Row("tex prepared", "%llu chains, %.1f MB, %.0f ms", (unsigned long long)p.texPrepared,
                p.texPreparedBytes / kMb, p.texMs);
            Row("tex skipped / unreadable", "%llu / %llu", (unsigned long long)p.texSkipped,
                (unsigned long long)p.texUnreadable);
            ImGui::EndTable();
        }
    }

    PanelHeading("Object residency");
    if (!s.residency.valid)
    {
        PanelHelp("no streamed world");
        return;
    }
    const ObjectStreamResidencyCounters& r = s.residency;
    if (ImGui::BeginTable("res", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        Row("resident / budget", "%llu / %llu", (unsigned long long)r.residentObjects, (unsigned long long)r.budget);
        // Phase 7: the item budget, in bytes. `per obj` is what a byte budget would be
        // divided by; `budget` here is WGR_DYNAMIC_VRAM_MB, a fixed default, NOT the
        // device's VRAM -- read "over" with that in mind.
        if (s.gpu.valid)
        {
            Row("gpu tracked / budget", "%.0f / %.0f MB%s", s.gpu.trackedBytes / (1024.0 * 1024.0),
                s.gpu.budgetBytes / (1024.0 * 1024.0), s.gpu.overBudget ? "  OVER" : "");
            Row("gpu tex / geom live / geom cap", "%.0f / %.0f / %.0f MB",
                s.gpu.objectTextureBytes / (1024.0 * 1024.0), s.gpu.geometryLiveBytes / (1024.0 * 1024.0),
                s.gpu.geometryCapacityBytes / (1024.0 * 1024.0));
            // Shown only when derived -- a stock CWA mission does not use the modern
            // object-streaming path, so there is no object count to divide by and a flat
            // "0 KB" would read as a measurement.
            if (s.gpu.bytesPerResidentObject > 0)
                Row("gpu per resident object", "%.0f KB", s.gpu.bytesPerResidentObject / 1024.0);
            else
                Row("gpu per resident object", "%s", "n/a (no modern object residency)");
        }
        Row("shape cache", "%llu/%llu held, hits %llu, ins %llu, drops %llu", (unsigned long long)r.shapeCacheSize,
            (unsigned long long)r.shapeCacheLimit, (unsigned long long)r.shapeCacheHits,
            (unsigned long long)r.shapeCacheInserts, (unsigned long long)r.shapeCacheDrops);
        Row("admits async / waited / sync", "%llu / %llu / %llu", (unsigned long long)r.asyncInstalled,
            (unsigned long long)r.asyncWaited, (unsigned long long)r.syncCold);
        Row("cold models", "%llu", (unsigned long long)r.coldModels);
        Row("cold parse/adapt/opt/create", "%.0f / %.0f / %.0f / %.0f ms", r.coldParseMs, r.coldAdaptMs,
            r.coldOptimizeMs, r.coldCreateMs);
        Row("cold tex read/create/fb/alpha", "%.0f / %.0f / %.0f / %.0f ms", r.coldTexReadMs, r.coldTexCreateMs,
            r.coldTexFallbackMs, r.coldAlphaMs);
        Row("cold tex header opens", "%llu, %.0f ms", (unsigned long long)r.coldTexHeaderLoads, r.coldTexHeaderMs);
        Row("cold tex uploads", "%llu (%llu from workers)", (unsigned long long)r.coldTexUploads,
            (unsigned long long)r.coldTexPrepared);
        Row("model park hits / stale", "%llu / %llu", (unsigned long long)r.parkHits,
            (unsigned long long)r.parkStale);
        ImGui::EndTable();
    }
}

} // namespace Poseidon::Dev
