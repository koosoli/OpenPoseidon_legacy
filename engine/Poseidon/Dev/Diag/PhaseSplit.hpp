#pragma once

// PERF-030: a named CPU breakdown of ONE frame phase, for the frames that are slow.
//
// The frame profiler says which phase a frame spent its time in (`land:gnd`, `land:obj`, ...),
// and nothing about what inside it. On the MGS2 Big Shell flight (2026-10-21) `land:gnd` went
// from ~2 ms to 40-60 ms a frame on WGPU for seconds at a time while the GPU (17 ms) and the
// render worker (11 ms) were idle half the frame and the producer never waited for the worker --
// main-thread work inside the phase, in code nobody had timed. This splits the phase into the
// calls it is made of, accumulates them per frame, and hands back one line when the frame was
// slow (and a mean line now and then), so the next measurement names the call instead of the
// phase. Main thread only; the cost is two steady_clock reads per slot per frame.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Poseidon::Dev
{

template <size_t N> class PhaseSplit
{
  public:
    using Clock = std::chrono::steady_clock;

    PhaseSplit(const char* phase, const std::array<const char*, N>& names, const char* thresholdEnv,
               double defaultThresholdMs)
        : _phase(phase), _names(names)
    {
        const char* v = thresholdEnv ? std::getenv(thresholdEnv) : nullptr;
        _thresholdMs = v ? std::atof(v) : defaultThresholdMs;
    }

    // Times one slot for the lifetime of the returned object. Re-entering a slot adds up.
    class Scope
    {
      public:
        Scope(PhaseSplit& owner, size_t slot) : _owner(owner), _slot(slot), _t0(Clock::now()) {}
        ~Scope() { _owner.Add(_slot, std::chrono::duration<double, std::milli>(Clock::now() - _t0).count()); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

      private:
        PhaseSplit& _owner;
        size_t _slot;
        Clock::time_point _t0;
    };

    Scope Time(size_t slot) { return Scope(*this, slot); }
    // Lap timing for a long function: Start() once, then Lap(slot) after each piece bills the
    // time since the previous Start/Lap to `slot`, without re-indenting the pieces into scopes.
    void Start() { _lap = Clock::now(); }
    void Lap(size_t slot)
    {
        const auto now = Clock::now();
        Add(slot, std::chrono::duration<double, std::milli>(now - _lap).count());
        _lap = now;
    }
    void Add(size_t slot, double ms)
    {
        if (slot < N)
            _frame[slot] += ms;
    }
    bool Enabled() const { return _thresholdMs > 0.0; }

    // Closes the frame. Returns true and fills `line` when this frame's total reached the
    // threshold (the first 40 such frames, then one in 25), or when a mean line is due (every
    // 600 frames). The caller logs it; this header owns no logger.
    bool EndFrame(std::string& line)
    {
        double total = 0.0;
        for (double v : _frame)
            total += v;
        for (size_t i = 0; i < N; ++i)
            _sum[i] += _frame[i];
        _sumTotal += total;
        ++_frames;
        bool emit = false;
        if (Enabled() && total >= _thresholdMs)
        {
            ++_slowFrames;
            if (_slowFrames <= 40 || (_slowFrames % 25) == 0)
            {
                line = Format("slow", total, _frame, 1.0);
                emit = true;
            }
        }
        if (!emit && Enabled() && (_frames % 600) == 0)
        {
            line = Format("mean", _sumTotal / static_cast<double>(_frames), _sum, 1.0 / static_cast<double>(_frames));
            emit = true;
        }
        _frame.fill(0.0);
        return emit;
    }

  private:
    std::string Format(const char* kind, double total, const std::array<double, N>& v, double scale) const
    {
        char buf[1024];
        int off = std::snprintf(buf, sizeof(buf), "PERF-030 %s %s: %.2f ms |", _phase, kind, total);
        for (size_t i = 0; i < N && off > 0 && off < static_cast<int>(sizeof(buf)); ++i)
            off += std::snprintf(buf + off, sizeof(buf) - off, " %s=%.2f", _names[i], v[i] * scale);
        if (off > 0 && off < static_cast<int>(sizeof(buf)))
            std::snprintf(buf + off, sizeof(buf) - off, " | frame %llu, slow %llu",
                          static_cast<unsigned long long>(_frames), static_cast<unsigned long long>(_slowFrames));
        return buf;
    }

    const char* _phase;
    std::array<const char*, N> _names;
    double _thresholdMs = 0.0;
    Clock::time_point _lap{};
    std::array<double, N> _frame{};
    std::array<double, N> _sum{};
    double _sumTotal = 0.0;
    unsigned long long _frames = 0;
    unsigned long long _slowFrames = 0;
};

} // namespace Poseidon::Dev
