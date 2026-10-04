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
// the simulation, and no existing collision or ray-cast path is replaced. See
// PHY-001 §1.3 for why the line sits here and what crossing it would cost.

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

    /// Which special LODs this model resolves to, INCLUDING the engine's own
    /// fallbacks -- an absent View level reads as Geometry, an absent Fire level as
    /// View. This is what a query will actually be answered from.
    ColliderFlags lodKindsResolved = ColliderFlags::None;
    /// Of those, the ones the model AUTHORED as a level of its own rather than
    /// inheriting from Geometry. The number that says whether flagging colliders
    /// buys anything on a given corpus: where this is empty, every flag lands on
    /// one shared set of hulls and the filter is free but also inert.
    ColliderFlags lodKindsDedicated = ColliderFlags::None;
    /// Flags that reached at least one registered piece. A kind present in
    /// `lodKindsResolved` and missing here had a LOD that yielded no hullable
    /// convex component -- which is the Roadway answer on this corpus, and a fact
    /// about the data rather than a failure to look.
    ColliderFlags flagsRegistered = ColliderFlags::None;
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
    bool PatchTerrain(int x, int z, int width, int height, const float* samples);

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
    ///
    /// `filter` decides WHICH colliders the ray is allowed to see, so "what stops
    /// this shot" and "what breaks this sight line" are two different questions of
    /// the same collider set rather than one question with a shared answer. The
    /// default sees everything, which is what an unfiltered ray meant before.
    BodyId CastRay(Vector3Par from, Vector3Par to, Vector3& hitPoint,
                   const QueryFilter& filter = QueryFilter{}) const;
    /// Did the ray hit ANYTHING, including the terrain height field, which has no
    /// body handle? CastRay alone cannot answer that: it returns an invalid id
    /// both for "missed" and for "hit something untracked", and conflating those
    /// would make the terrain look like a miss in every audit.
    bool RayHitAnything(Vector3Par from, Vector3Par to, const QueryFilter& filter = QueryFilter{}) const;

    /// How thick is the thing a shot just entered?
    ///
    /// The question a per-material passthrough boolean cannot answer, and the
    /// reason penetration is not one. With colliders in a physics world the
    /// answer is two rays:
    /// the entry is known, and casting BACKWARDS from beyond the object finds the
    /// far face. Forwards from just inside would find the next object instead
    /// whenever the shot is already past the mid-plane.
    ///
    /// Returns false when nothing is found within `maxDepth`, which means either a
    /// very thick object or one the collider set does not have -- the caller must
    /// not treat that as "thin".
    ///
    /// Asks the FIRE GEOMETRY. A View Geometry LOD exists to break line of sight
    /// and is routinely a coarse box around a building that has windows; measuring
    /// a shot's path through that would report a wall where the model says there is
    /// air. `filter` defaults accordingly rather than to everything.
    bool MeasureThickness(Vector3Par entry, Vector3Par direction, float maxDepth, float& thickness,
                          const QueryFilter& filter = QueryFilter{ColliderFlags::BulletCollision}) const;
    void   ClearProbes();
    void   GetProbeSamples(std::vector<ProbeSample>& out) const;
    /// Physics -> gameplay, for a body the caller spawned as dynamic. See
    /// BodyMotion for why this exception to PHY-010's one-way rule exists.
    bool   GetBodyMotion(BodyId id, BodyMotion& out) const;
    bool GetArticulatedGroundSupport(BodyId, Vector3&) const;
    BodyId SpawnArticulatedPiece(const struct ConvexPiece&, const Matrix4&, float mass);
    BodyId SpawnArticulatedPiece(const struct ConvexPiece&, const Matrix4&, float mass,
        const ArticulatedInitialMotion&);
    bool SetArticulatedMotion(BodyId, Vector3Par linear, Vector3Par angular);
    JointId AddArticulatedJoint(const ArticulatedJointDef&);
    void RemoveJoint(JointId);
    [[nodiscard]] std::uint64_t Generation() const { return _generation; }
    // Monotonic mutation attempts; a failed backend operation cannot establish
    // that a previously prepared diagnostic terrain still has its ownership.
    [[nodiscard]] std::uint64_t TerrainMutationSerial() const { return _terrainMutationSerial; }
    void   SetWind(const WindSettings& wind);
    /// Follows the player so they can shove probes. Physics never moves them back.
    void SetKinematicProxy(Vector3Par position, float radius, float height);
    [[nodiscard]] const WindSettings& GetWind() const { return _wind; }

    [[nodiscard]] PhysicsStats GetStats() const;
    /// Backend name, or "none" when no world is up.
    [[nodiscard]] const char* BackendName() const;

private:
    void ResetCounters();
    /// Pulls the convex components of every special LOD in `wanted` into flat
    /// spans, tagging each piece with the kind it came from. `result` collects what
    /// the model turned out to carry; pass the same one the caller will return.
    bool ExtractPieces(LODShape* shape, ColliderFlags wanted, std::vector<Vector3>& points,
                       std::vector<struct ConvexPiece>& pieces, int& degenerate, int& maxPoints,
                       RegisterResult& result);

    std::unique_ptr<PhysicsBackend> _backend;
    std::uint64_t _generation = 0;
    std::uint64_t _terrainMutationSerial = 0;

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
