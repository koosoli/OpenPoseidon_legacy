#pragma once

#include <atomic>
#include <algorithm>

// FOLIAGE-DUSK -- the exponent that decides WHEN the foliage-only night dimming starts.
//
// Two lines in shading.wgsl scale foliage ambient by `daylight` and nothing else in the
// renderer does: the near-field ambient boost, and the night floor `mix(0.35, 1.0, daylight)`.
// `daylight` is derived CPU-side from the atmospheric SUN RADIANCE through a hard
// (0.002 -> 0.04) ramp (EngineWgpu.cpp:2833-2837). At a low sun the air mass eats the
// transmittance, so the value collapses well before the geometric sunset -- and near-field
// leaves lost up to ~7x ambient in the golden hour while terrain, houses and rocks beside
// them, which neither line touches, kept theirs. Trees read as black cut-outs.
//
// The fix is a softer curve on that one input: pow(daylight, k). The night purpose survives
// exactly -- pow(0, k) == 0 for any k > 0 -- and full day is untouched, pow(1, k) == 1; only
// the middle (dusk / dawn) is lifted. k = 1 is the pre-2026-09 look bit for bit.
//
// It lives here rather than in Engine::FoliageSettings because that struct sits in
// Graphics/Core/Engine.hpp, which this change was not permitted to touch. Functionally it is
// a foliage setting: the Foliage dev tab owns it (slider "Dusk curve", next to "Ambient
// boost"), its reset buttons reset it, and EngineWgpu pushes it into the Frame UBO every
// frame alongside the rest of WgrFoliage. Fold it into FoliageSettings when that header is
// free; nothing outside these three call sites reads it.
//
// Written by the dev panel (UI thread), read by the renderer's frame publish -- hence atomic.

// Namespace `Poseidon`, NOT `Engine`: Engine is a CLASS here (Poseidon::Engine, the graphics
// interface), and several translation units say `using Poseidon::Engine;`.
namespace Poseidon
{

/// Default: 0.35. Chosen so `daylight` stays high through the golden hour (0.25 -> 0.61,
/// 0.5 -> 0.78) and still reaches 0 at real night. 1.0 = the legacy curve.
inline constexpr float kFoliageDuskCurveDefault = 0.35f;

inline std::atomic<float>& FoliageDuskCurveRef()
{
    static std::atomic<float> value{kFoliageDuskCurveDefault};
    return value;
}

/// Clamped to the shader's own accepted range, so a stray value can never publish a 0 lane
/// (which the shader reads as "legacy") or an exponent steep enough to invert the intent.
inline float FoliageDuskCurve()
{
    return std::clamp(FoliageDuskCurveRef().load(std::memory_order_relaxed), 0.05f, 1.0f);
}

inline void SetFoliageDuskCurve(float k)
{
    FoliageDuskCurveRef().store(std::clamp(k, 0.05f, 1.0f), std::memory_order_relaxed);
}

} // namespace Poseidon
