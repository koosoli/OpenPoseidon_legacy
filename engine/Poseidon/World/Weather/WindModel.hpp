#pragma once

#include <cstdint>

namespace Poseidon
{

// ---------------------------------------------------------------------------
// The world's single wind authority.
//
// Before this existed, five subsystems each invented their own air motion:
//
//   * Weather::MoveClouds ran a 5-second random walk (Landscape.cpp) that
//     smoke, flags, parachutes and helicopters read via Landscape::GetWind().
//   * The volumetric cloud deck scrolled on a hardcoded 8/2 m/s constant
//     (Engine::SkySettings::cloudWind).
//   * The FFT ocean spectrum used a hardcoded unit vector and 12 m/s
//     (WaterWgpu.cpp fft_wind_sea), with a comment saying so.
//   * The CPU buoyancy predictor used its own hardcoded 0.82/0.57 at 6 m/s
//     (WaterSurfaceQuery.cpp).
//   * Ballistics used no wind at all.
//
// This model replaces the *source*; consumers keep their own response curves.
//
// DESIGN CONSTRAINTS
//
// 1. It lives on the simulation side, not the renderer. The renderer samples
//    it; it never invents wind.
// 2. It is a CLOSED FORM in (conditions, overcast, time). There is no
//    integrator and no per-frame RNG, so it cannot drift between machines,
//    cannot desynchronise after a pause, and needs no replication traffic:
//    every machine that agrees on the mission clock and the overcast computes
//    the same vector. Feeding it the network-corrected clock (Glob.NetTime)
//    is what makes that true in multiplayer.
// 3. It is C1-smooth and bounded — a weighted sum of sines whose weights sum
//    to one — so no consumer ever sees a step. The legacy random walk stepped
//    every 5 seconds, which is why smoke used to visibly jerk.
// 4. It costs one evaluation per simulation frame (a few dozen flops), cached
//    in the singleton. Consumers read the cache; nobody re-evaluates.
// ---------------------------------------------------------------------------

/// One evaluation of the wind field. Y is deliberately absent: the legacy model
/// only ever produced a horizontal vector plus a small vertical gust component
/// that nothing consumed meaningfully, and every consumer here is horizontal.
struct WindSample
{
    float velocityX = 0.0f; ///< m/s along world +X (instantaneous, gust included)
    float velocityZ = 0.0f; ///< m/s along world +Z (instantaneous, gust included)
    float speed = 0.0f;     ///< m/s, |(velocityX, velocityZ)|
    /// atan2(velocityZ, velocityX) — the direction the air is travelling TOWARD.
    float directionRad = 0.0f;

    /// The gust-free mean. This is the quantity a wave spectrum is parameterised
    /// by: an equilibrium sea answers to the mean wind over tens of minutes, not
    /// to a 10-second gust. Driving the FFT spectrum from `speed` instead would
    /// rebuild h0 constantly for no visual gain — see WaterWgpu.cpp.
    float meanSpeed = 0.0f;
    float meanDirectionRad = 0.0f;

    /// (speed - meanSpeed) / meanSpeed, roughly [-1, 1]. Grass and smoke want
    /// this; the ocean spectrum must not.
    float gustFraction = 0.0f;
};

/// Authored, replicable inputs. Everything here is stable mission state — it is
/// serialized with the world and is identical on every machine.
struct WindConditions
{
    /// Prevailing heading the wind blows TOWARD, radians, atan2(z, x).
    /// The default is atan2(2, 4) — the heading the legacy overcast wind bias
    /// (Vector3(4, 0, 2) * overcast) pointed at, so a classic mission's smoke
    /// still drifts the way it always did.
    float baseDirectionRad = 0.46364760f;

    /// Mean speed at overcast 0 (m/s). Legacy calm was a +/-1 m/s per-axis walk.
    float calmSpeed = 1.5f;
    /// Additional mean speed at overcast 1 (m/s). Legacy at full overcast peaked
    /// around 13 m/s including walk and gust; 1.5 + 7.5 with gust lands in the
    /// same envelope.
    float overcastSpeed = 7.5f;

    /// 0 = perfectly steady, 1 = the gust swings the speed by its full envelope.
    float gustiness = 0.55f;
    /// How far the mean heading veers either side of `baseDirectionRad` (rad).
    float veerAmplitudeRad = 0.55f;

    /// Fractional slow drift of the mean speed, minutes-long. 0.18 is the value
    /// this model shipped with and is the default so nothing changes; the field
    /// exists so a *steady* field can be requested (the dev override sets it to
    /// zero, otherwise the speed you dial in is not the speed you get).
    float meanVariation = 0.18f;

    /// Decorrelates the sine phases per mission. Any two machines with the same
    /// seed produce the same wind; it is NOT consumed from a shared RNG stream.
    uint32_t seed = 1337u;
};

/// A developer's hand on the wind. LOCAL AND UNREPLICATED BY DESIGN: it is not
/// serialized with the world and it is not sent to anyone, so switching it on in
/// a network game makes this machine disagree with the server about the air. It
/// exists so a dev can dial in "9 m/s, due east, no gusts" and watch what
/// answers — smoke, grass, sea, tracer drift — which is impossible against a
/// closed form in mission time. Do not wire gameplay to it.
struct WindOverride
{
    bool enabled = false;
    float speed = 6.0f;        ///< mean speed, m/s
    float directionRad = 0.0f; ///< heading the air travels TOWARD, atan2(z, x)
    /// 0 = dead steady (the mean *is* the instantaneous value), 1 = full gust
    /// envelope. Defaults to steady: an override exists to remove variables.
    float gustiness = 0.0f;
};

/// Pure, side-effect-free evaluation. `timeMilliseconds` should be the
/// network-corrected mission clock (Glob.NetTime().toInt()); milliseconds are
/// taken as an integer so the time base itself cannot drift in float.
/// `overcast` is clamped to [0, 1].
WindSample EvaluateWind(const WindConditions& conditions, float overcast, int64_t timeMilliseconds);

/// The world-owned instance. World::SimulateLandscape ticks it once per frame;
/// everything else reads Sample().
class WindModel
{
  public:
    /// Mission start. Resets to authored conditions and marks the model active.
    void Init(const WindConditions& conditions);
    void Init() { Init(WindConditions{}); }

    /// One tick. Cheap enough to call unconditionally; it is a closed form, so
    /// calling it twice with the same arguments is idempotent.
    void Update(int64_t netTimeMilliseconds, float overcast);

    /// False until the first Update. Consumers that have a legacy fallback
    /// (Landscape::GetWind) use this to decide whether the authority is live,
    /// so a Landscape used outside a World keeps its old behaviour exactly.
    bool IsActive() const { return _active; }
    void Deactivate() { _active = false; }

    const WindSample& Sample() const { return _sample; }
    const WindConditions& Conditions() const { return _conditions; }
    void SetConditions(const WindConditions& conditions) { _conditions = conditions; }

    float Overcast() const { return _overcast; }

    // -- Dev override -----------------------------------------------------
    // While enabled, Sample() reports the override instead of the closed form.
    // Conditions() and Overcast() keep reporting the real mission state, so
    // turning the override off restores the exact vector the mission would have
    // had at that instant — there is no state to unwind.
    const WindOverride& Override() const { return _override; }
    void SetOverride(const WindOverride& value) { _override = value; }
    bool IsOverridden() const { return _override.enabled; }

    // -- Ballistics opt-in ------------------------------------------------
    // OFF BY DEFAULT AND DELIBERATELY SO. Classic OFP missions were authored
    // and tested against a windless ballistic solution; letting a 9 m/s
    // crosswind push a sniper round would change how they play. With this
    // false, ShotShell::Simulate computes the byte-identical expression it
    // always did. Enable with POSEIDON_WIND_BALLISTICS=1 or the setter.
    static bool BallisticsEnabled();
    static void SetBallisticsEnabled(bool enabled);

  private:
    WindConditions _conditions{};
    WindOverride _override{};
    WindSample _sample{};
    float _overcast = 0.0f;
    bool _active = false;
};

WindModel& GWindModel();

/// Spelled as a pseudo-variable to match GRandGen / GLandscape at call sites.
#define GWind GWindModel()

} // namespace Poseidon
