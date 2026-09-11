#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace Poseidon
{
namespace Dev
{

// Per-frame phase timing for the main game loop (World::Simulate).  One
// Mark() per phase boundary accumulates the time since the previous mark;
// EndFrame() pushes the record into a fixed ring the dev panel Perf tab
// and triPerfStats read.  Seven steady_clock reads per frame — cheap
// enough to stay always-on.
class FrameProfiler
{
  public:
    enum Phase
    {
        // PERF-012: "setup" was input + network + camera + landscape sim AND THE ENTIRE
        // FIXED-STEP SIMULATION LOOP, which its own comment did not mention. On the combat
        // scene that bucket is 17-24 ms of a ~50 ms frame -- the single largest CPU cost in
        // the engine -- and it was reported as one number with a name that hid what was in
        // it. The loop now has its own phase so the two are separable.
        PhaseSetup = 0,    // input, network, scripts, camera, landscape sim (NOT the sim loop)
        PhaseSimStep,      // the fixed-step simulation loop: StepSimulation x steps
        PhaseDrawInit,     // clear, sky prep, camera setup
        PhaseDrawObjPrep,  // object pass preparation (EndObjects, shadow setup)
        PhaseDrawLandGround, // terrain mesh + water + horizon
        PhaseDrawLandObjects,// opaque objects + shadows (Pass1)
        PhaseDrawLandscape,  // grass, alpha objects (Pass2), segment tail
        PhaseDrawObjects,  // object + shadow passes, queue flush, frame observe
        PhaseDrawPost,     // cuts/effects, alternate draw paths, cleanup
        PhaseHud,          // UI, titles, FinishDraw
        PhaseSound,        // PerformSound + AdvanceAll + Commit
        PhaseSwap,         // NextFrame (resolve + present)
        PhaseCount,
        // Draw sub-phase range for aggregation (panel + triPerfStats draw=).
        PhaseDrawFirst = PhaseDrawInit,
        PhaseDrawLast = PhaseDrawPost,
    };

    static const char* PhaseName(int p)
    {
        // "ai+veh" was removed rather than left reading zero: PerformAI and
        // SimulateAllVehicles run inside the fixed-step loop and are `sim:step`, and the
        // phase that carried the name was measuring the SOUND work (PERF-013). A phase no
        // call site marks would report 0.000 forever, which is indistinguishable from work
        // that is genuinely free -- this repository has a documented history of diagnostics
        // that reported 0 where they meant "did not run".
        static const char* kNames[PhaseCount] = {"setup",    "sim:step", "drw:init", "drw:prep", "land:gnd",
                                                 "land:obj", "drw:land", "drw:obj",  "drw:post", "hud",
                                                 "sound",    "swap"};
        return (p >= 0 && p < PhaseCount) ? kNames[p] : "?";
    }


    struct FrameRecord
    {
        std::array<float, PhaseCount> ms{};
        float totalMs = 0.f;
        int drawCalls = 0;
    };

    struct PhaseStats
    {
        float avgMs = 0.f;
        float p95Ms = 0.f;
        float maxMs = 0.f;
    };

    static constexpr int kRingSize = 256;
    static constexpr size_t kMaxCaptureFrames = 16384;

    // Main-loop diagnostics only. Reserve once; recording never allocates or logs.
    bool StartCapture(size_t limit)
    {
        if (limit == 0 || limit > kMaxCaptureFrames)
            return false;
        _capture.reserve(limit);
        _capture.clear();
        _captureLimit = limit;
        _captureDropped = 0;
        _captureActive = false;
        _capturePending = true; // Exclude the frame already in progress.
        return true;
    }
    void StopCapture() { _captureActive = _capturePending = false; }
    const std::vector<FrameRecord>& CapturedFrames() const { return _capture; }
    size_t CaptureDropped() const { return _captureDropped; }

    void BeginFrame()
    {
        if (_capturePending)
        {
            _capturePending = false;
            _captureActive = true;
        }
        _frameStart = Clock::now();
        _lastMark = _frameStart;
        _current = FrameRecord{};
    }

    void Mark(Phase p)
    {
        const TimePoint now = Clock::now();
        _current.ms[p] += DurMs(_lastMark, now);
        _lastMark = now;
    }

    void EndFrame(int drawCalls)
    {
        const TimePoint now = Clock::now();
        _current.totalMs = DurMs(_frameStart, now);
        _current.drawCalls = drawCalls;
        _ring[_head] = _current;
        _head = (_head + 1) % kRingSize;
        if (_count < kRingSize)
            ++_count;
        if (_captureActive)
        {
            if (_capture.size() < _captureLimit)
                _capture.push_back(_current);
            else
                ++_captureDropped;
        }
    }

    int FrameCount() const { return _count; }

    // newest = Frame(0), oldest = Frame(FrameCount()-1)
    const FrameRecord& Frame(int back) const
    {
        int idx = (_head - 1 - back + 2 * kRingSize) % kRingSize;
        return _ring[idx];
    }

    PhaseStats Stats(Phase p) const { return ComputeStats([p](const FrameRecord& r) { return r.ms[p]; }); }
    PhaseStats TotalStats() const { return ComputeStats([](const FrameRecord& r) { return r.totalMs; }); }
    PhaseStats DrawStats() const
    {
        return ComputeStats(
            [](const FrameRecord& r)
            {
                float s = 0.f;
                for (int p = PhaseDrawFirst; p <= PhaseDrawLast; p++)
                    s += r.ms[p];
                return s;
            });
    }

    float AvgFps() const
    {
        const PhaseStats t = TotalStats();
        return t.avgMs > 0.001f ? 1000.f / t.avgMs : 0.f;
    }

    float AvgDrawCalls() const
    {
        if (_count == 0)
            return 0.f;
        long long sum = 0;
        for (int i = 0; i < _count; i++)
            sum += Frame(i).drawCalls;
        return static_cast<float>(sum) / _count;
    }

    void Reset()
    {
        _head = 0;
        _count = 0;
        StopCapture();
        _capture.clear();
        _captureDropped = 0;
    }

  private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static float DurMs(TimePoint a, TimePoint b)
    {
        return std::chrono::duration<float, std::milli>(b - a).count();
    }

    template <typename Get>
    PhaseStats ComputeStats(Get get) const
    {
        PhaseStats s;
        if (_count == 0)
            return s;
        std::array<float, kRingSize> vals;
        float sum = 0.f;
        for (int i = 0; i < _count; i++)
        {
            const float v = get(Frame(i));
            vals[i] = v;
            sum += v;
            if (v > s.maxMs)
                s.maxMs = v;
        }
        s.avgMs = sum / _count;
        // p95 via nth_element on the copied window
        const int nth = (_count * 95) / 100;
        std::nth_element(vals.begin(), vals.begin() + nth, vals.begin() + _count);
        s.p95Ms = vals[nth];
        return s;
    }

    std::array<FrameRecord, kRingSize> _ring{};
    FrameRecord _current{};
    TimePoint _frameStart{};
    TimePoint _lastMark{};
    int _head = 0;
    int _count = 0;
    std::vector<FrameRecord> _capture;
    size_t _captureLimit = 0;
    size_t _captureDropped = 0;
    bool _captureActive = false;
    bool _capturePending = false;
};

FrameProfiler& GFrameProfiler();

} // namespace Dev
} // namespace Poseidon
