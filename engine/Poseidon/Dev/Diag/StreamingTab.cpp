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

#include <algorithm>
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

    // RFG-085: the levers that trade quality or scope for VRAM, live. Each is seeded from the
    // environment variable named in its tooltip; the panel edits the running value.
    PanelHeading("Residency levers");
    PanelHelp("what to give up when the card is full; every one of these ships ON or 0 by default");
    ResidencyLevers& levers = GResidencyLevers();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("texture mip bias", &levers.textureMipBias, 0, 4);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WGR_TEXTURE_MIP_BIAS. Drops the N finest mip levels of every block-compressed\n"
                          "object texture at UPLOAD: 1 = quarter the bytes, half the resolution.\n"
                          "Applies to textures uploaded from now on; reload the world for all of them.");
    Checkbox("own opaque-class sections with an alpha channel (RFG-075)", &levers.ownOpaqueAlpha);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("POSEIDON_OWN_OPAQUE_ALPHA. Every Reforger BCR carries alpha (roughness); with this\n"
                          "off those sections fall back to the per-draw path, which has no layer blend --\n"
                          "church walls go flat -- but registers less geometry in the pool.");
    Checkbox("layer tint in the shader, not baked into RGBA8 copies (RFG-086)", &levers.layerTintInShader);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("POSEIDON_LAYER_TINT_BAKED=1 turns this off. An enft| face texture draws through its\n"
                          "shared BC7 tile and the shader multiplies the layer colour. Baked: one tinted RGBA8\n"
                          "copy per (colour, tile) pair -- measured 3,480 MB for 671 of them on Everon.\n"
                          "Applies to textures first drawn from now on; reload the world for all of them.");
    Checkbox("Native Reforger worlds stream their objects (RFG-097/099)", &levers.nativeStreaming);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("POSEIDON_REFORGER_STREAM=0 turns this off. Every placement of a native world goes to\n"
                          "the object stream (budget WGR_OBJECT_STREAM_MAX_OBJECTS, default 20000) instead of a\n"
                          "300 m radius placed at load. Measured at the church: 24 ms/frame streamed, 46 ms for\n"
                          "the radius path. Read at world load -- reload the world to apply.");
    Checkbox("CPU-path buffers share the retained model's mesh (RFG-088)", &levers.shareRetainedMeshes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WGR_SHARE_RETAINED_MESHES=0 turns this off. A Partial model (interior proxies, a glass\n"
                          "complement) draws its LODs on both paths; without sharing each LOD sits in the pool\n"
                          "twice -- measured 759 MB retained + 880 MB direct on Everon at 800 models.\n"
                          "Applies to buffers created from now on.");
    Checkbox("leaf composites (enfa|) encoded to BC3 instead of RGBA8 (RFG-092)", &levers.compressComposites);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("POSEIDON_COMPRESS_COMPOSITES=0 turns this off. A coverage-spliced leaf albedo went\n"
                          "up as RGBA8 with generated mips, 4x the bytes of a block format -- 693 MB on Everon\n"
                          "at play settings. Applies to textures uploaded from now on.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("sky bake: min enclosed", &levers.skyBakeMinEnclosed, 0.0f, 0.5f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WGR_SKY_BAKE_MIN_ENCLOSED. A model's sky-visibility volume (256 KB) is kept only\n"
                          "when this fraction of its voxels sees less than half the sky. 0 keeps every\n"
                          "volume, including a tree's, which reads as 'sky fully visible' anyway.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("sky bake: budget MB", &levers.skyVolumeBudgetMb, 0, 2048);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WGR_SKY_VOLUME_MB. Total bytes of kept volumes; bakes past it are dropped.\n"
                          "Measured: 1,024 MB before the gate on a 100k-model Everon, 256 MB after.");
    ImGui::Spacing();

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
        Row("native DDS puts / takes", "%llu / %llu", (unsigned long long)s.store.ddsPreparedPuts,
            (unsigned long long)s.store.ddsPreparedTakes);
        Row("DDS option mismatches", "%llu", (unsigned long long)s.store.ddsConfigurationMisses);
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
            // Request dedup, and the ready bound's back-pressure. The percentage denominator
            // is enqueued + coalesced, i.e. the asks that a working coalescer could have
            // saved; sticky refusals (NotLoose/Failed -> synchronous fallback) are shown
            // beside it rather than folded in, because they save no parse.
            {
                const unsigned long long asks = p.requested + p.coalesced;
                Row("coalesced", "%llu (%.0f%% of %llu asks), %llu sticky", (unsigned long long)p.coalesced,
                    asks ? 100.0 * (double)p.coalesced / (double)asks : 0.0, asks,
                    (unsigned long long)p.coalescedStuck);
            }
            Row("IR ready / reserved", "%.1f / %.1f MiB (budget %.1f)",
                p.readyPayloadBytes / 1048576.0, p.parseReservedBytes / 1048576.0, p.readyByteBudget / 1048576.0);
            ImGui::Text("Conversion reservation %.1f MiB (peak %.1f MiB)",
                p.conversionReservedBytes / 1048576.0, p.peakConversionReservedBytes / 1048576.0);
            Row("IR byte-bound parks", "%llu; oversize %llu", (unsigned long long)p.readyByteParks,
                (unsigned long long)p.oversizedPayloads);
            Row("ready-bound parks", "%llu (limit %zu)", (unsigned long long)p.readyFullParks,
                ObjectStreamPreparer::ReadyLimit());
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
            Row("backend buffers + textures", "%.0f MB (excludes allocator slack)",
                s.gpu.backendAllocationBytes / (1024.0 * 1024.0));
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
        // Which bound ended each admit update. A bound whose lever is off prints "n/a",
        // never 0: 0-because-disabled and 0-because-never-reached are opposite answers to
        // "is this lever earning its place".
        Row("admit updates", "%llu (%llu drained fully)", (unsigned long long)r.admitUpdates,
            (unsigned long long)(r.admitUpdates -
                                 std::min(r.admitUpdates, r.admitStopCap + r.admitStopUpload +
                                                              r.admitStopCeiling + r.admitStopBudget +
                                                              r.admitStopCold)));
        if (r.admitCounted)
        {
            // Counted mode never reads a clock, so the two clock stops are n/a here, not 0.
            Row("admit mode", "counted: %u cold models per update", (unsigned)r.admitColdBudget);
            Row("stopped by cap / cold budget", "%llu / %llu", (unsigned long long)r.admitStopCap,
                (unsigned long long)r.admitStopCold);
            Row("stopped by budget / wall ceiling", "%s", "n/a (counted; WGR_OBJECT_STREAM_ADMIT=timed)");
        }
        else
        {
            Row("admit mode", "%s", "timed (WGR_OBJECT_STREAM_MS_PER_UPDATE / _MAX_MS)");
            Row("stopped by cap / budget", "%llu / %llu", (unsigned long long)r.admitStopCap,
                (unsigned long long)r.admitStopBudget);
            if (r.admitWallCeilingArmed)
                Row("stopped by wall ceiling", "%llu", (unsigned long long)r.admitStopCeiling);
            else
                Row("stopped by wall ceiling", "%s", "n/a (WGR_OBJECT_STREAM_MAX_MS=0)");
        }
        if (r.admitUploadCeilingArmed)
            Row("stopped by upload ceiling", "%llu", (unsigned long long)r.admitStopUpload);
        else
            Row("stopped by upload ceiling", "%s", "n/a (WGR_OBJECT_STREAM_UPLOAD_MB unset)");
        ImGui::EndTable();
    }
}

} // namespace Poseidon::Dev
