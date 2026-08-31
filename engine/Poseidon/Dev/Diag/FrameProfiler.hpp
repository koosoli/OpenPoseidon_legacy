#pragma once

#include <algorithm>
#include <array>
#include <chrono>

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
        PhaseAiVehicles,   // PerformAI + SimulateAllVehicles
        PhaseSound,        // PerformSound + AdvanceAll + Commit
        PhaseSwap,         // NextFrame (resolve + present)
        PhaseCount,
        // Draw sub-phase range for aggregation (panel + triPerfStats draw=).
        PhaseDrawFirst = PhaseDrawInit,
        PhaseDrawLast = PhaseDrawPost,
    };

    static const char* PhaseName(int p)
    {
        static const char* kNames[PhaseCount] = {"setup",    "sim:step", "drw:init", "drw:prep", "land:gnd", "land:obj",
                                                 "drw:land", "drw:obj",  "drw:post", "hud",      "ai+veh",   "sound",
                                                 "swap"};
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

    void BeginFrame()
    {
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
};

FrameProfiler& GFrameProfiler();

} // namespace Dev
} // namespace Poseidon
