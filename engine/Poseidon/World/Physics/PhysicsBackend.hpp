#pragma once

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/World/Physics/PhysicsTypes.hpp>

#include <memory>

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

    [[nodiscard]] virtual PhysicsStats GetStats() const = 0;

    /// Backend name for logs and the dev panel, e.g. "box3d 0.1.0".
    [[nodiscard]] virtual const char* Name() const = 0;
};

/// Builds the configured backend. Returns null when none is compiled in, which is
/// a supported state: the engine runs without a physics world.
std::unique_ptr<PhysicsBackend> CreatePhysicsBackend();

} // namespace Poseidon::Physics
