#pragma once

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/World/Physics/PhysicsTypes.hpp>

#include <memory>

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

    void SetTransform(BodyId id, const Matrix4& transform);
    void Remove(BodyId id);

    /// Row-major heightfield, `width * height` samples `cellSize` apart.
    bool SetTerrain(const float* heights, int width, int height, float cellSize, float originX, float originZ);

    [[nodiscard]] PhysicsStats GetStats() const;
    /// Backend name, or "none" when no world is up.
    [[nodiscard]] const char* BackendName() const;

private:
    void ResetCounters();

    std::unique_ptr<PhysicsBackend> _backend;

    // Counted HERE and not in the backend, because these are facts about our
    // data rather than about the library: a model with no Geometry LOD and a
    // component too thin to hull are both decided before the backend is asked.
    // Keeping them out of PhysicsBackend also keeps that interface to what a
    // replacement actually has to implement.
    std::uint32_t _modelsRegistered = 0;
    std::uint32_t _modelsWithoutGeometry = 0;
    std::uint32_t _degenerateComponents = 0;
};

/// The process-wide physics world. Null until something creates one; PHY-010
/// leaves that to the dev panel rather than wiring it into world load, so a build
/// with physics compiled in behaves exactly as before until asked.
PhysicsWorld* GetPhysicsWorld();
PhysicsWorld& EnsurePhysicsWorld();

} // namespace Poseidon::Physics
