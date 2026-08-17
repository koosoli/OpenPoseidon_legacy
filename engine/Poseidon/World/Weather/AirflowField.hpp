#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

#include <cstddef>

namespace Poseidon
{

// ---------------------------------------------------------------------------
// Local airflow: the prevailing wind plus whatever is stirring the air nearby.
//
// WindModel answers "what is the weather doing" and is deliberately a closed
// form with no position argument — a global vector. That is correct for the sea
// spectrum and the cloud deck, and useless for anything standing under a
// helicopter. This is the position-dependent layer on top.
//
// Before this existed, rotor downwash was a RENDERER effect: EngineWgpu scanned
// the vehicle list every frame, built four (x, z, radius, strength) discs and
// handed them to grass.wgsl. Nothing outside that shader could see them, which
// is why smoke drifting under a hovering Huey was completely unmoved by it.
//
// DESIGN CONSTRAINTS
//
// 1. Simulation-side, like WindModel. The renderer may sample it; it must not
//    be the only thing that can.
// 2. Rebuilt from world state once per simulation frame, never integrated. Like
//    WindModel it holds no history, so it cannot drift or desync: two machines
//    that agree on where the helicopters are agree on the air.
// 3. Analytic and cheap. Sample() is called per smoke particle per frame, so it
//    is a handful of flops times a small bounded emitter count — not a grid, not
//    a solve. Roadmap ATM-030 explicitly rules out world CFD; FX-130's volume
//    grids are a later, optional refinement that would sit above this, not
//    replace it.
// 4. The emitter list is bounded and nearest-first. A map with thirty
//    helicopters must cost the same as a map with four.
// ---------------------------------------------------------------------------

/// One rotor's wake. Position is the disc centre; the model below turns it into
/// a downward column that spreads radially when it reaches the ground.
struct RotorWash
{
    Vector3 position{VZero}; ///< rotor disc centre, world space
    float discRadius = 8.0f; ///< effective disc radius, m
    float strength = 0.0f;   ///< downwash speed at the disc, m/s
    float groundY = 0.0f;    ///< terrain height under the disc, for the ground jet
};

/// How many rotors can contribute at once. Four is what the grass shader has
/// always used and no fixture has ever needed more; the cost of a fifth is paid
/// by every particle in the world, so this stays small on purpose.
inline constexpr std::size_t kMaxRotorWash = 4;

class AirflowField
{
  public:
    /// Rebuild the emitter list from the world. Call once per simulation frame,
    /// after WindModel::Update. `focus` is the point emitters are ranked by
    /// distance from — the camera, since a wash nobody can see need not be
    /// simulated. Safe to call with no world; the field simply empties.
    void Update(Vector3Par focus);

    /// Empty the emitter list. Called on mission teardown so a stale rotor from
    /// the previous mission cannot blow the first frame of the next one.
    void Reset();

    /// Total air velocity at `pos`: prevailing wind plus every rotor wake.
    /// Prevailing wind is horizontal; the rotor terms are fully 3D.
    Vector3 Sample(Vector3Par pos) const;

    /// Just the local (rotor) part, with the prevailing wind removed. The smoke
    /// system needs these separated: being indoors should cut a particle off
    /// from the weather, but a helicopter hovering over a blown-open roof still
    /// pushes the air in the room.
    Vector3 SampleLocal(Vector3Par pos) const;

    std::size_t RotorCount() const { return _count; }
    const RotorWash& Rotor(std::size_t index) const { return _rotors[index]; }

    /// Master switch for the local terms, for the dev panel and for A/B tests.
    /// Prevailing wind is unaffected.
    bool LocalEnabled() const { return _localEnabled; }
    void SetLocalEnabled(bool enabled) { _localEnabled = enabled; }

    /// Scales every rotor's `strength`. 1 is the tuned default; the dev panel
    /// exposes it because "is the downwash doing anything at all" is much easier
    /// to answer at 3x than at 1x.
    float LocalScale() const { return _localScale; }
    void SetLocalScale(float scale) { _localScale = scale; }

  private:
    RotorWash _rotors[kMaxRotorWash]{};
    std::size_t _count = 0;
    bool _localEnabled = true;
    float _localScale = 1.0f;
};

AirflowField& GAirflowField();

/// Spelled as a pseudo-variable to match GWind / GLandscape at call sites.
#define GAirflow GAirflowField()

/// The rotor wake model, exposed so it can be unit-tested without a world.
/// Returns the air velocity `wash` contributes at `pos`.
Vector3 EvaluateRotorWash(const RotorWash& wash, Vector3Par pos);

} // namespace Poseidon
