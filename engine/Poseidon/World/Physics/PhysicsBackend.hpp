#pragma once

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/World/Physics/PhysicsTypes.hpp>

#include <memory>
#include <vector>

// The replaceable half. One implementation exists today (Box3D); PHY-001 names
// Jolt as the fallback and lists three points at which that choice is revisited.
// Everything the engine sees is above this line and does not change with it.

class LODShape;

namespace Poseidon::Physics
{

/// Geometry handed to the backend for one convex piece.
///
/// Points, not indices: OFP Geometry LODs are already authored as named convex
/// components (`Component01`, `Component02`, ...), so the expensive part of this
/// work -- convex decomposition -- is already done in the data and the backend
/// only has to hull each piece. PHY-001 §1.2 measured that; it is the reason the
/// binding is small.
struct ConvexPiece
{
    const Vector3* points = nullptr;
    int            count = 0;
};

/// A regular heightfield, row-major, `width * height` samples spaced `cellSize`
/// apart, with sample (0,0) at world `originX, originZ`.
struct TerrainField
{
    const float* heights = nullptr;
    int          width = 0;
    int          height = 0;
    float        cellSize = 0.0f;
    float        originX = 0.0f;
    float        originZ = 0.0f;
};

class PhysicsBackend
{
public:
    virtual ~PhysicsBackend() = default;

    virtual bool Create() = 0;
    virtual void Destroy() = 0;

    /// Advance the simulation. Nothing reads results back into gameplay in
    /// PHY-010 -- Poseidon stays authoritative over every position. Stepping is
    /// here so the world is a real simulation that can be inspected, not so it
    /// can start driving anything.
    virtual void Step(float deltaSeconds) = 0;

    /// One static body carrying `pieces` convex hulls, placed at `transform`.
    /// Returns an invalid id if every piece was refused.
    virtual BodyId AddStaticBody(const ConvexPiece* pieces, int pieceCount, const Matrix4& transform) = 0;

    virtual void RemoveBody(BodyId id) = 0;

    /// Transforms flow IN only (PHY-010). There is deliberately no getter: adding
    /// one is how "physics is a query service" quietly becomes "physics owns the
    /// position", which is the step PHY-001 defers and the fork never took.
    virtual void SetBodyTransform(BodyId id, const Matrix4& transform) = 0;

    virtual bool SetTerrain(const TerrainField& field) = 0;

    /// PHY-020. A dynamic sphere the physics world owns outright.
    virtual BodyId SpawnSphereProbe(const SphereProbeDef& def) = 0;
    /// A dynamic body built from convex pieces -- the same pieces a static body
    /// takes, so a model's authored Geometry LOD can simply be made to move.
    virtual BodyId SpawnDynamicPieces(const ConvexPiece* pieces, int pieceCount, const Matrix4& transform,
                                      float mass, float friction, float restitution) = 0;
    /// Push a body at a point. Gameplay -> physics, which is the direction
    /// PHY-010 already allows; nothing comes back.
    virtual void ApplyImpulse(BodyId id, Vector3Par point, Vector3Par impulse) = 0;
    /// Ray against the physics world; returns the body hit, invalid if none.
    virtual BodyId CastRay(Vector3Par from, Vector3Par to, Vector3& hitPoint) const = 0;
    virtual void   ClearProbes() = 0;
    /// Probe positions for debug drawing only. Scoped to probe bodies on purpose:
    /// a general transform getter is what turns "physics is a query service" into
    /// "physics owns the position", and PHY-020 is not that step.
    virtual void GetProbeSamples(std::vector<ProbeSample>& out) const = 0;
    /// Applied to every probe shape at the start of each step while enabled.
    virtual void SetWind(const WindSettings& wind) = 0;

    /// A KINEMATIC capsule the caller moves every tick. Kinematic, not dynamic:
    /// it pushes dynamic bodies and is pushed by nothing, so the player keeps
    /// full authority over their own position and physics gains none. Pass a
    /// zero height to remove it.
    virtual void SetKinematicProxy(Vector3Par position, float radius, float height) = 0;

    [[nodiscard]] virtual PhysicsStats GetStats() const = 0;

    /// Backend name for logs and the dev panel, e.g. "box3d 0.1.0".
    [[nodiscard]] virtual const char* Name() const = 0;
};

/// Builds the configured backend. Returns null when none is compiled in, which is
/// a supported state: the engine runs without a physics world.
std::unique_ptr<PhysicsBackend> CreatePhysicsBackend();

} // namespace Poseidon::Physics
