#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{

// ---------------------------------------------------------------------------
// The only way the smoke simulation is allowed to touch the world.
//
// This is the narrow query boundary roadmap PHY-000 asks for, introduced here
// because smoke is the first consumer that actually needs one. Four operations,
// no handles, no callback ordering, no backend types in the signature — so if
// PHY-GATE-0 ever concludes that an external backend is worth adopting, smoke
// moves onto it by writing one new implementation of this class, not by being
// rewritten.
//
// The shipping implementation is `PoseidonSmokeWorldQuery`, which is a thin
// wrapper over Landscape::ObjectCollision and Landscape::SurfaceY — the query
// path the engine already had. PHY-010 explicitly allows for the conclusion
// that the current path is sufficient for the query-only phase; for smoke it
// demonstrably is.
//
// COST IS PART OF THE CONTRACT. SweepSphere walks the object grid and does real
// geometry intersection, so it is the expensive call by an order of magnitude.
// SmokeVolume budgets it: each particle sweeps on a jittered interval and holds
// the resulting contact plane in between. Any implementation of this interface
// should assume the caller is already rationing it and need not ration itself.
// ---------------------------------------------------------------------------

struct SmokeSurfaceHit
{
    bool hit = false;
    Vector3 position{VZero}; ///< world-space contact point
    Vector3 normal{VZero};   ///< unit surface normal pointing OUT of the solid
};

class ISmokeWorldQuery
{
  public:
    virtual ~ISmokeWorldQuery() = default;

    /// Sweep a sphere of `radius` from `from` to `to`. Returns the first
    /// contact, or `hit == false`. Static geometry and vehicles both count;
    /// temporary objects (which includes all smoke) must not, or plumes push
    /// each other around.
    virtual SmokeSurfaceHit SweepSphere(Vector3Par from, Vector3Par to, float radius) const = 0;

    /// Terrain height. Cheap — a heightmap lookup, no geometry. Called every
    /// frame for every particle, unlike SweepSphere.
    virtual float GroundHeight(float x, float z) const = 0;

    /// Is there geometry directly overhead within `probeHeight` metres? This is
    /// the "am I indoors" test, and it is deliberately that crude: a roof is
    /// what stops the wind, and asking whether a point is inside a closed
    /// building volume is both much more expensive and no more useful. A
    /// particle under a bridge or a tree canopy reading as sheltered is a
    /// defensible answer, not a bug.
    virtual bool IsSheltered(Vector3Par pos, float probeHeight) const = 0;
};

/// The live query. Never null: with no world loaded it returns an
/// implementation that reports open sky and flat ground at y = 0, so the
/// simulation runs identically in a unit test with no engine behind it.
const ISmokeWorldQuery& GSmokeWorldQuery();

/// Substitute a query, for tests and for fixture replay. Pass nullptr to
/// restore the engine-backed one.
void SetSmokeWorldQuery(const ISmokeWorldQuery* query);

/// How many SweepSphere calls have been made since the counter was last reset.
/// The dev panel reports this: it is the single number that says whether the
/// collision budget is being respected, and it is much harder to fool than a
/// frame time.
long long SmokeSweepCount();
void ResetSmokeSweepCount();

} // namespace Poseidon
