#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Poseidon
{

// Converts wall-clock frame durations into a bounded number of fixed simulation
// ticks. Rendering remains free to run once per outer frame using Alpha().
//
// The accumulator intentionally retains at most one unsimulated step after a
// catch-up cap. This prevents an expensive frame from creating a permanent
// backlog (the "spiral of death") while preserving an interpolation fraction.
class FixedStepAccumulator
{
public:
    static constexpr double DefaultStepSeconds = 1.0 / 60.0;
    static constexpr std::size_t DefaultMaxCatchUpSteps = 8;

    explicit FixedStepAccumulator(double stepSeconds = DefaultStepSeconds,
                                  std::size_t maxCatchUpSteps = DefaultMaxCatchUpSteps)
        : _stepSeconds(stepSeconds), _maxCatchUpSteps(maxCatchUpSteps)
    {
    }

    [[nodiscard]] std::size_t Advance(double frameSeconds)
    {
        _accumulatedSeconds += std::max(0.0, frameSeconds);

        constexpr double RoundoffTolerance = 1e-9;
        const std::size_t availableSteps =
            static_cast<std::size_t>(std::floor((_accumulatedSeconds + RoundoffTolerance) / _stepSeconds));
        const std::size_t steps = std::min(availableSteps, _maxCatchUpSteps);
        _accumulatedSeconds -= static_cast<double>(steps) * _stepSeconds;

        if (availableSteps > _maxCatchUpSteps)
        {
            // PERF-013: this discard is the anti-spiral guard doing its job, and it is also
            // the world silently losing time -- every step dropped here is simulation that
            // never happens. Nothing counted it, so "the world ran at half speed during that
            // fight" was not a statement anyone could make or refute. Counted now; the
            // behaviour is unchanged.
            _discardedSteps += availableSteps - _maxCatchUpSteps;
            ++_discardEvents;
            _accumulatedSeconds = std::fmod(_accumulatedSeconds, _stepSeconds);
        }

        if (_accumulatedSeconds < RoundoffTolerance || _stepSeconds - _accumulatedSeconds < RoundoffTolerance)
            _accumulatedSeconds = 0.0;

        return steps;
    }

    [[nodiscard]] double StepSeconds() const { return _stepSeconds; }
    /// Steps the catch-up cap discarded since construction, and how many frames discarded
    /// any. Diagnostic; never read by the simulation.
    [[nodiscard]] std::size_t DiscardedSteps() const { return _discardedSteps; }
    [[nodiscard]] std::size_t DiscardEvents() const { return _discardEvents; }
    [[nodiscard]] std::size_t MaxCatchUpSteps() const { return _maxCatchUpSteps; }
    [[nodiscard]] double Alpha() const { return std::clamp(_accumulatedSeconds / _stepSeconds, 0.0, 1.0); }

    void Reset() { _accumulatedSeconds = 0.0; }

private:
    double _stepSeconds;
    std::size_t _maxCatchUpSteps;
    double _accumulatedSeconds = 0.0;
    std::size_t _discardedSteps = 0;
    std::size_t _discardEvents = 0;
};

} // namespace Poseidon
