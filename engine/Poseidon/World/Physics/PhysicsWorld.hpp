#pragma once

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/World/Physics/PhysicsTypes.hpp>

#include <memory>
#include <vector>

// What the rest of the engine talks to. Deliberately smaller than the backend
// interface: gameplay code registers a model, moves it, and forgets it.
//
// PHY-010 SCOPE, and it is a boundary rather than a milestone: transforms flow
// INTO physics only. Nothing here reads a position back, no entity is driven by
// the simulation, and no existing collision or ray-cast path is replaced. The
// fork this work draws from never crossed that line either -- its physics is a
// read-only service and the one class that would have owned motion was written
// and never instantiated. See PHY-001 §1.3.

class LODShape;

namespace Poseidon::Physics
{

class PhysicsBackend;

/// Why one model did or did not become a body.
///
/// A plain BodyId cannot answer PHY-010's abort condition, which is not "how many
/// failed" but "WHICH DISTINCT MODELS failed". Five hundred failures can be five
/// hundred copies of one fence, and that is a very different finding from five
/// hundred different buildings.
struct RegisterResult
{
    enum class Outcome
    {
        Registered,
        NoGeometryLod,          ///< no Geometry LOD, or it is empty -- normal for many props
        AllComponentsDegenerate,///< had components, none of them a volume
        BackendRefused,         ///< the library would not hull anything we offered
    };

    BodyId  id;
    Outcome outcome = Outcome::NoGeometryLod;
    /// Largest point count among this model's components, for sizing the hull cap.
    int maxComponentPoints = 0;
};

class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    /// Brings up the configured backend. Returns false when none is available;
    /// that is not an error and the caller carries on without a physics world.
    bool Create();
    void Destroy();
    [[nodiscard]] bool IsCreated() const { return _backend != nullptr; }

    void Step(float deltaSeconds);

    /// Registers a model's Geometry LOD as one static body.
    ///
    /// The convex decomposition is READ, not computed: OFP models carry their
    /// Geometry LOD as named convex components and `Shape::GetGeomComponents()`
    /// already exposes them. Anything this cannot use is COUNTED in PhysicsStats
    /// rather than skipped quietly -- PHY-010's abort condition is about how much
    /// of the stock corpus converts, and a silent skip makes a half-built world
    /// indistinguishable from a working one.
    ///
    /// Returns an invalid id when the model has no usable geometry.
    BodyId RegisterStatic(LODShape* shape, const Matrix4& transform);

    /// Same, but says WHY. Used by the corpus census; ordinary callers want the
    /// short form.
    RegisterResult RegisterStaticDetailed(LODShape* shape, const Matrix4& transform);

    void SetTransform(BodyId id, const Matrix4& transform);
    void Remove(BodyId id);

    /// Row-major heightfield, `width * height` samples `cellSize` apart.
    bool SetTerrain(const float* heights, int width, int height, float cellSize, float originX, float originZ);

    /// PHY-020 dev probe. See SphereProbeDef.
    BodyId SpawnSphereProbe(const SphereProbeDef& def);

    /// A DYNAMIC copy of a model's Geometry LOD. Deliberately a separate instance:
    /// the physics world owns it, nothing in the game knows it exists, and no
    /// mission state is touched. That is what lets a jeep be thrown around while
    /// PHY-050 -- physics owning a real gameplay vehicle -- stays unplanned.
    BodyId SpawnModelProbe(LODShape* shape, const Matrix4& transform, float mass, float friction,
                           float restitution, Vector3Par velocity = VZero);

    /// Gameplay -> physics. Nothing comes back, so PHY-010's rule holds.
    void ApplyImpulse(BodyId id, Vector3Par point, Vector3Par impulse);
    /// Ray against the physics world. Returns the body hit, invalid if none.
    BodyId CastRay(Vector3Par from, Vector3Par to, Vector3& hitPoint) const;
    /// Did the ray hit ANYTHING, including the terrain height field, which has no
    /// body handle? CastRay alone cannot answer that: it returns an invalid id
    /// both for "missed" and for "hit something untracked", and conflating those
    /// would make the terrain look like a miss in every audit.
    bool RayHitAnything(Vector3Par from, Vector3Par to) const;

    /// How thick is the thing a shot just entered?
    ///
    /// This is the measurement the fork's material system does NOT have -- its
    /// bulletPassthrough is a boolean and its own code carries a TODO where the
    /// depth should be. With colliders in a physics world the answer is two rays:
    /// the entry is known, and casting BACKWARDS from beyond the object finds the
    /// far face. Forwards from just inside would find the next object instead
    /// whenever the shot is already past the mid-plane.
    ///
    /// Returns false when nothing is found within `maxDepth`, which means either a
    /// very thick object or one the collider set does not have -- the caller must
    /// not treat that as "thin".
    bool MeasureThickness(Vector3Par entry, Vector3Par direction, float maxDepth, float& thickness) const;
    void   ClearProbes();
    void   GetProbeSamples(std::vector<ProbeSample>& out) const;
    void   SetWind(const WindSettings& wind);
    /// Follows the player so they can shove probes. Physics never moves them back.
    void SetKinematicProxy(Vector3Par position, float radius, float height);
    [[nodiscard]] const WindSettings& GetWind() const { return _wind; }

    [[nodiscard]] PhysicsStats GetStats() const;
    /// Backend name, or "none" when no world is up.
    [[nodiscard]] const char* BackendName() const;

private:
    void ResetCounters();
    bool ExtractPieces(LODShape* shape, std::vector<Vector3>& points, std::vector<struct ConvexPiece>& pieces,
                       int& degenerate, int& maxPoints);

    std::unique_ptr<PhysicsBackend> _backend;

    // Counted HERE and not in the backend, because these are facts about our
    // data rather than about the library: a model with no Geometry LOD and a
    // component too thin to hull are both decided before the backend is asked.
    // Keeping them out of PhysicsBackend also keeps that interface to what a
    // replacement actually has to implement.
    std::uint32_t _modelsRegistered = 0;
    std::uint32_t _modelsWithoutGeometry = 0;
    std::uint32_t _degenerateComponents = 0;
    std::uint32_t _maxComponentPoints = 0;
    WindSettings  _wind;

    // Timing lives in the facade, not the backend: it measures OUR cost of asking
    // for a step, which is what a person cares about, and it stays identical if
    // the backend is replaced.
    double        _stepMsTotal = 0.0;
    double        _stepMsWorst = 0.0;
    std::uint64_t _stepsTimed = 0;
};

/// The process-wide physics world. Null until something creates one; PHY-010
/// leaves that to the dev panel rather than wiring it into world load, so a build
/// with physics compiled in behaves exactly as before until asked.
PhysicsWorld* GetPhysicsWorld();
PhysicsWorld& EnsurePhysicsWorld();

} // namespace Poseidon::Physics
