#include <Poseidon/Dev/Diag/FramePaceTrace.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace Poseidon::Dev
{
namespace
{

// Capacity per side. 200k rows is ~55 minutes at 60 fps; a capture run is a few minutes.
// Recording stops silently at the cap (the dropped count is written into the CSV header),
// which is the honest failure: a trace that reallocates mid-run would be measuring itself.
constexpr std::size_t kCapacity = 200000;

struct Store
{
    bool                              enabled = false;
    std::string                       prefix;
    std::mutex                        mutex; // producer/worker rows come from two threads
    std::vector<FramePaceMainRow>     main;
    std::vector<FramePaceProducerRow> producer;
    std::vector<FramePaceWorkerRow>   worker;
    std::size_t                       mainFlushed = 0, producerFlushed = 0, workerFlushed = 0;
    std::size_t                       mainDropped = 0, producerDropped = 0, workerDropped = 0;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    Store()
    {
        const char* e = std::getenv("POSEIDON_FRAME_TRACE");
        if (e == nullptr || e[0] == 0)
            return;
        prefix = e;
        main.reserve(kCapacity);
        producer.reserve(kCapacity);
        worker.reserve(kCapacity);
        enabled = true;
    }
    ~Store() { FramePaceTraceFlush(); }
};

Store& G()
{
    static Store s;
    return s;
}

template <typename Row, typename Writer>
void FlushOne(const std::string& path, std::vector<Row>& rows, std::size_t& flushed, std::size_t dropped,
              const char* header, Writer write)
{
    if (rows.size() <= flushed && flushed > 0)
        return;
    FILE* f = std::fopen(path.c_str(), flushed == 0 ? "wb" : "ab");
    if (f == nullptr)
        return;
    if (flushed == 0)
        std::fprintf(f, "# dropped=%zu\n%s\n", dropped, header);
    for (std::size_t i = flushed; i < rows.size(); ++i)
        write(f, rows[i]);
    flushed = rows.size();
    std::fclose(f);
}

} // namespace

bool FramePaceTraceEnabled()
{
    return G().enabled;
}

double FramePaceNowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - G().epoch).count();
}

void FramePaceTraceMain(const FramePaceMainRow& row)
{
    Store& s = G();
    if (!s.enabled)
        return;
    if (s.main.size() < kCapacity)
        s.main.push_back(row);
    else
        ++s.mainDropped;
}

void FramePaceTraceProducer(const FramePaceProducerRow& row)
{
    Store& s = G();
    if (!s.enabled)
        return;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.producer.size() < kCapacity)
        s.producer.push_back(row);
    else
        ++s.producerDropped;
}

void FramePaceTraceWorker(const FramePaceWorkerRow& row)
{
    Store& s = G();
    if (!s.enabled)
        return;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.worker.size() < kCapacity)
        s.worker.push_back(row);
    else
        ++s.workerDropped;
}

void FramePaceTraceFlush()
{
    Store& s = G();
    if (!s.enabled)
        return;
    std::lock_guard<std::mutex> lock(s.mutex);
    FlushOne(s.prefix + ".main.csv", s.main, s.mainFlushed, s.mainDropped,
             "frame,t_start_ms,t_pace_ms,t_end_ms,deltaT_ms,steps,alpha,total_ticks,setup_ms,sim_ms,"
             "draw_init_ms,draw_ms,hud_ms,sound_ms,swap_ms,cap_fps,pace_period_ms,pace_elapsed_ms,"
             "pace_sleep_req_ms,pace_sleep_act_ms,cam_dx_m,interp_alpha,cam_dyaw_deg",
             [](FILE* f, const FramePaceMainRow& r)
             {
                 std::fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%u,%.4f,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f\n",
                              r.frame, r.tStartMs, r.tPaceMs, r.tEndMs, r.deltaTMs, r.steps, r.alpha,
                              static_cast<unsigned long long>(r.totalTicks), r.setupMs, r.simMs, r.drawInitMs,
                              r.drawMs, r.hudMs, r.soundMs, r.swapMs, r.capFps, r.pacePeriodMs, r.paceElapsedMs,
                              r.paceSleepReqMs, r.paceSleepActMs, r.camDxM, r.interpAlpha, r.camDyawDeg);
             });
    FlushOne(s.prefix + ".producer.csv", s.producer, s.producerFlushed, s.producerDropped,
             "frame,t_publish_ms,predraw_wait_ms,lazy_wait_ms,slot_wait_ms,lockstep_wait_ms,render_thread,overlap",
             [](FILE* f, const FramePaceProducerRow& r)
             {
                 std::fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u\n", r.frame, r.tPublishMs, r.preDrawWaitMs,
                              r.lazyWaitMs, r.slotWaitMs, r.lockstepWaitMs, r.renderThread, r.overlap);
             });
    FlushOne(s.prefix + ".worker.csv", s.worker, s.workerFlushed, s.workerDropped,
             "seq,t_start_ms,t_end_ms,idle_before_ms,acquire_ms,submit_ms,present_ms,render_ms,gpu_frame_ms",
             [](FILE* f, const FramePaceWorkerRow& r)
             {
                 std::fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", r.seq, r.tStartMs, r.tEndMs,
                              r.idleBeforeMs, r.acquireMs, r.submitMs, r.presentMs, r.renderMs, r.gpuFrameMs);
             });
}

} // namespace Poseidon::Dev
