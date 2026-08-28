#pragma once

#include <cstdint>

// Engine-side vocabulary for the physics layer. NO BACKEND TYPE APPEARS HERE, and
// none may appear in any header the rest of the engine includes -- that is the
// whole point of this file existing separately from the backend.
//
// PHY-001 records why: only 250-300 lines of a physics integration are actually
// library-specific, and Box3D is at 0.1.0 with no API-stability guarantee. Keeping
// the boundary narrow is what makes a later move to Jolt a file rather than a
// project. If a Box3D type ever leaks past this boundary, that property is gone
// and nobody will notice until the day it matters.

namespace Poseidon::Physics
{

/// Handle to a body in the physics world.
///
/// A generation counter rides along with the index so a stale handle is
/// DETECTABLE rather than silently addressing whatever now occupies that slot.
/// Physics bodies outlive the code that created them by exactly as long as it
/// takes someone to forget a removal, and a raw index makes that a corruption
/// instead of an error.
struct BodyId
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] bool IsValid() const { return generation != 0; }
    friend bool operator==(const BodyId& a, const BodyId& b)
    {
        return a.index == b.index && a.generation == b.generation;
    }
    friend bool operator!=(const BodyId& a, const BodyId& b) { return !(a == b); }
};

/// What the physics world contains and what it refused to build.
///
/// The refusal counters are the load-bearing half. PHY-010's abort condition is
/// "stock CWA building Geometry LODs cannot be converted without per-model special
/// cases", and the only way to know that is to count what failed instead of
/// quietly skipping it. A physics world that silently contains half the map looks
/// exactly like one that works.
struct PhysicsStats
{
    std::uint32_t bodies = 0;         ///< live bodies
    std::uint32_t shapes = 0;         ///< collider shapes across all bodies
    std::uint32_t modelsRegistered = 0;

    /// A model whose Geometry LOD produced no usable component at all.
    std::uint32_t modelsWithoutGeometry = 0;
    /// Components rejected before reaching the backend: fewer than four distinct
    /// points, or a degenerate extent. A convex hull needs a volume.
    std::uint32_t degenerateComponents = 0;
    /// Components the backend itself refused to hull.
    std::uint32_t hullFailures = 0;

    /// Largest ConvexComponent point count seen. MaxHullVertices is currently an
    /// ASSUMPTION about the stock corpus; this is what turns it into a measurement.
    std::uint32_t maxComponentPoints = 0;

    /// PHY-020 dev probes currently alive.
    std::uint32_t probes = 0;

    bool terrainRegistered = false;
    /// Terrain grid actually handed to the backend, for comparison against the
    /// landscape's own dimensions.
    std::uint32_t terrainWidth = 0;
    std::uint32_t terrainHeight = 0;
};

/// PHY-020's dev probe: one dynamic sphere, owned entirely by physics.
///
/// Deliberately a PROBE and not an entity. It exists to answer one question --
/// can Box3D move a dynamic body at 60 Hz across the real terrain and the real
/// Geometry-LOD colliders -- and it is drawn from a probe-only readback rather
/// than by giving gameplay a transform getter. That distinction is the PHY-010
/// boundary and PHY-020 does not cross it.
struct SphereProbeDef
{
    Vector3 position;
    Vector3 velocity;
    float   radius = 0.2f;
    /// kg. Converted to Box3D's density (kg/m^3) from the sphere's own volume, so
    /// the dev UI can speak in a unit a person has intuition about.
    float mass = 0.5f;
    float friction = 0.5f;
    float restitution = 0.4f;
    /// [0,1], spheres and capsules only. Without it a sphere on any slope rolls
    /// for ever: sliding friction never opposes rolling, so the ball never
    /// settles no matter how high `friction` goes.
    float rollingResistance = 0.05f;
};

/// Enough to draw a probe. NOT a gameplay transform: it carries no orientation
/// and is only produced for bodies the probe spawner created.
struct ProbeSample
{
    Vector3 position;
    float   radius = 0.0f;
};

/// World wind pushed onto probe shapes each step. Box3D 0.1.0 computes relative
/// air speed itself (b3Shape_ApplyWind), so there is no second aerodynamic model
/// here -- writing one while the library already has one is how two models end up
/// disagreeing.
struct WindSettings
{
    Vector3 velocity;
    float   drag = 1.0f;
    float   lift = 0.0f;
    float   maxSpeed = 100.0f;
    bool    enabled = false;
};

} // namespace Poseidon::Physics
