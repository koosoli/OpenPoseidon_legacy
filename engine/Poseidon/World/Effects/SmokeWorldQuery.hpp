#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{

class BuildingInterior;
class Object;

// A nearby cached building is usable only while this point belongs to its room grid.
bool SmokeCachedBuildingContains(const BuildingInterior* interior, const Object* building, Vector3Par position);

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

/// One proactive in-room movement step (rooms and portals). When `allowed` is
/// false the move would have left the room by any route but a portal:
/// `position` is the last point provably inside it and `normal` the outward
/// normal of the boundary about to be crossed -- a contact without a contact.
struct SmokeContainStep
{
    bool allowed = true;
    Vector3 position{VZero};
    Vector3 normal{VZero};
    int room = -1; ///< room the mover ends this step in, -1 = outdoors / unknown
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

    /// The surface a thing standing here would stand ON: the topmost of the
    /// terrain and any building floor / roadway over it. This is what an
    /// emitter placed inside a house must sit on. GroundHeight put a plume
    /// spawned in a house UNDER its floor slab — in the crawl space between the
    /// terrain and the roadway LOD — where every particle rose 20 cm, hit the
    /// underside of the floor, was pushed back down, and leaked out the sides.
    /// Costs a roadway lookup; fine for spawn and for a per-particle clamp.
    virtual float FloorHeight(float x, float z) const = 0;

    /// The nearest surface UNDER `pos` — the floor of whatever storey the point
    /// is on. FloorHeight(x, z) is the TOPMOST surface, which inside a building
    /// is the roof: spawning on it put every indoor plume on top of the house.
    /// This is what a spawn at the camera must use.
    virtual float FloorHeightBelow(Vector3Par pos) const = 0;

    /// Is there geometry directly overhead within `probeHeight` metres? This is
    /// the "am I indoors" test, and it is deliberately that crude: a roof is
    /// what stops the wind, and asking whether a point is inside a closed
    /// building volume is both much more expensive and no more useful. A
    /// particle under a bridge or a tree canopy reading as sheltered is a
    /// defensible answer, not a bug.
    virtual bool IsSheltered(Vector3Par pos, float probeHeight) const = 0;

    /// Is there geometry ANYWHERE along the segment `from` -> `to`, and where?
    /// This is IsSheltered's look-ahead sibling: instead of asking "has a roof
    /// already closed over me", it asks "will this path cross a surface", which
    /// is the only form of the question that can stop a fast mover BEFORE it is
    /// through. Rain probes its own next few metres of fall with it and dies at
    /// the roofline rather than a probe-interval below it. `hit` is the contact
    /// point when the answer is true. Non-pure (default: nothing in the way) so
    /// test implementations without geometry keep compiling and keep raining.
    virtual bool ProbeSegment(Vector3Par from, Vector3Par to, Vector3& hit) const
    {
        return false;
    }

    /// Which room of a building is this point in, false when outdoors or no
    /// data. Non-pure (default: no data) so existing and test implementations
    /// keep compiling; only the engine-backed query answers it.
    virtual bool IndoorRoomAt(Vector3Par pos, int& room) const { return false; }

    /// Conservative distance to the nearest boundary of `room`, capped by
    /// `maxDistance`. Implementations without room geometry leave the visual
    /// radius unchanged.
    virtual float IndoorClearanceAt(Vector3Par, int, float maxDistance) const { return maxDistance; }

    /// Proactive in-room step from -> to while in `room`. Default allows the
    /// move uncontained -- exactly what an implementation without interior
    /// data should say.
    virtual SmokeContainStep IndoorContainStep(Vector3Par from, Vector3Par to, int room) const
    {
        SmokeContainStep step;
        step.position = to;
        step.room = room;
        return step;
    }
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
