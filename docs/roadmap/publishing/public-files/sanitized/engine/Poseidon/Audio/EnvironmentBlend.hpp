#pragma once

// The reverb crossfade, as plain arithmetic on an array of scalars.
//
// This is the blender SoundSystemOAL::TickEnvironmentBlend actually runs -- the backend
// gathers the EAX reverb parameter set into a float array, ticks this, and scatters the
// result back. It is kept free of OpenAL types on purpose: EFXEAXREVERBPROPERTIES is an
// anonymous-struct typedef that cannot even be forward declared, and a blender that
// needs a sound device to test is a blender nobody tests.
//
// See design notes

#include <algorithm>
#include <cmath>

namespace Poseidon::Audio
{

// Capacity, not a requirement -- the blender carries whatever count it was given. The
// EAX reverb set needs 26 (20 scalars + two 3-component pan vectors).
inline constexpr int kEnvironmentBlendMaxParams = 32;

// Time constant of the reverb crossfade, in SECONDS.
//
// 0.6 s sits between two audible failures. Much shorter and crossing a doorway is still
// heard as a step -- the hard cut the roadmap asks us to remove. Much longer and the
// reverb lags the picture: you are well inside the building before it sounds like one,
// and a listener who steps in and straight out again hears a slow filter sweep rather
// than a room. An exponential ease is ~95% converged after three time constants, so
// 0.6 s means a transition completes in about two seconds -- roughly the time it takes
// to walk through a door and keep going, which is the transition this exists for.
//
// It is one number for every parameter, which is a simplification and not a claim:
// decay time and high-frequency gain do not have to move at the same rate to sound
// right, and if the owner's ears say the transition sweeps, splitting them is the first
// thing to try.
inline constexpr float kEnvironmentBlendTauSeconds = 0.6f;

// Longest frame delta the blend will honour, in SECONDS. A stall, a pause, or a loading
// hitch must advance the ease by at most this much: without the cap, one long frame
// teleports the reverb onto its target and reintroduces the very hard cut being removed.
inline constexpr float kEnvironmentBlendMaxStepSeconds = 0.25f;

// Fraction of the remaining distance to cover this frame. In [0, 1), so the ease can
// approach the target but never overshoot it -- which is what makes each parameter's
// trajectory monotonic.
inline float EnvironmentBlendFactor(float dtSeconds)
{
    const float dt = std::clamp(dtSeconds, 0.0f, kEnvironmentBlendMaxStepSeconds);
    return 1.0f - std::exp(-dt / kEnvironmentBlendTauSeconds);
}

// Near enough that further easing is inaudible. Relative term for parameters that live
// at large magnitudes (decay time, HF reference in Hz), absolute floor for the many
// that live in [0, 1].
inline bool EnvironmentBlendClose(float cur, float tgt)
{
    return std::fabs(tgt - cur) <= 0.001f + 0.005f * std::fabs(tgt);
}

// Eases a fixed-size parameter vector toward a target.
//
// The property that matters: SetTarget moves the TARGET and leaves CURRENT alone, so an
// environment change part-way through a transition continues from where the ear already
// is. It does not restart from the environment being left, and it does not jump.
class EnvironmentBlender
{
    float _current[kEnvironmentBlendMaxParams]{};
    float _target[kEnvironmentBlendMaxParams]{};
    int _count = 0;
    bool _valid = false;
    bool _settled = true;

  public:
    // False until the first Snap/SetTarget. There is nothing plausible to blend FROM
    // before that, so the first application must snap.
    bool Valid() const { return _valid; }
    // True when current == target and Tick has no work left. A settled environment
    // costs one bool load per frame and no device writes.
    bool Settled() const { return _settled; }
    int Count() const { return _count; }
    const float* Current() const { return _current; }
    const float* Target() const { return _target; }

    // Forget everything. Used when the effect object goes away: blending FROM a
    // parameter set the new device never heard would fade in from nowhere.
    void Invalidate()
    {
        _valid = false;
        _settled = true;
        _count = 0;
    }

    // current AND target := p, settled immediately. The first application after the
    // effect comes up, and every application when blending is switched off.
    void Snap(const float* p, int count)
    {
        _count = std::clamp(count, 0, kEnvironmentBlendMaxParams);
        for (int i = 0; i < _count; ++i)
        {
            _current[i] = p[i];
            _target[i] = p[i];
        }
        _valid = true;
        _settled = true;
    }

    // Retarget. Returns true if it blended, false if it had to snap (not yet valid, or
    // a differently shaped vector -- neither has a meaningful "from").
    bool SetTarget(const float* p, int count)
    {
        if (!_valid || count != _count)
        {
            Snap(p, count);
            return false;
        }
        for (int i = 0; i < _count; ++i)
        {
            _target[i] = p[i];
        }
        _settled = false;
        return true;
    }

    // Advances the ease. Returns true when Current() moved and must be pushed to the
    // device; false when there was nothing to do.
    bool Tick(float dtSeconds)
    {
        if (!_valid || _settled)
        {
            return false;
        }
        const float k = EnvironmentBlendFactor(dtSeconds);
        bool close = true;
        for (int i = 0; i < _count; ++i)
        {
            _current[i] += (_target[i] - _current[i]) * k;
            if (!EnvironmentBlendClose(_current[i], _target[i]))
            {
                close = false;
            }
        }
        if (close)
        {
            // Land exactly on the target rather than leaving a permanent epsilon.
            for (int i = 0; i < _count; ++i)
            {
                _current[i] = _target[i];
            }
            _settled = true;
        }
        return true;
    }
};

} // namespace Poseidon::Audio
