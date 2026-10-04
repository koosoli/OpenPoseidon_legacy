#pragma once

#include <cstdint>
#include <Poseidon/Foundation/Math/Math3D.hpp>

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

struct JointId
{
    std::uint32_t index = 0, generation = 0;
    [[nodiscard]] bool IsValid() const { return generation != 0; }
};

enum class ArticulatedJointKind { Spherical, Hinge };

struct ArticulatedInitialMotion
{
    Vector3 linear = VZero, angular = VZero;
    // -1 preserves the explicit diagnostic. Production owns one distinct
    // negative family per active corpse: only its own limbs ignore each other.
    int collisionFamily = -1;
};

// Both local frames use their Z axes for the cone/twist or hinge axis. The caller fits
// these to measured model geometry; no RTM translation is a joint location.
struct ArticulatedJointDef
{
    BodyId first, second;
    Matrix4 firstFrame = MIdentity, secondFrame = MIdentity;
    float coneAngle = 1.2f, twistAngle = 0.6f;
    ArticulatedJointKind kind = ArticulatedJointKind::Spherical;
    // Signed rotation of secondFrame about firstFrame's Z; radians. These are
    // frame-relative limits, not a spring target or an anatomical zero angle.
    float lowerAngle = -0.1f, upperAngle = 2.5f;
};

/// What a collider piece is FOR.
///
/// NOT a taxonomy of our own. An OFP model already carries these as distinct
/// special LOD levels, and `LODShape` already names them: Geometry
/// (`FindGeometryLevel`) is the volume things bump into, Fire Geometry
/// (`FindFireGeometryLevel`) is what stops a shot, View Geometry
/// (`FindViewGeometryLevel`) is what breaks line of sight, Roadway
/// (`FindRoadwayLevel`) is the surface something walks on. The flags are read off
/// those levels; nothing here decides what a collider means.
///
/// A model that omits a level does not lose the meaning: `LODShape::ScanShapes`
/// collapses an absent View level onto Geometry and an absent Fire level onto
/// View, so most models carry all three flags on one piece and the distinction
/// only costs extra shapes for the models that actually authored a separate LOD.
/// That is the same resolution `Object::Intersect` uses, so a filtered query and
/// the 2001 intersect are asking about the same geometry.
enum class ColliderFlags : std::uint32_t
{
    None = 0,
    /// Geometry LOD: contact, and the general "did I hit this" question.
    Solid = 1u << 0,
    /// Fire Geometry LOD: what a projectile is stopped by.
    BulletCollision = 1u << 1,
    /// View Geometry LOD: what breaks line of sight.
    ViewBlocking = 1u << 2,
    /// Roadway LOD: the walkable surface.
    ///
    /// MEASURED, and the measurement is the reason this one is currently carried
    /// by the terrain and by nothing else. Over the 1327 models in O.pbo,
    /// Data3D.pbo and Noe.pbo: 236 (17.8%) carry a Roadway LOD, and 31 (2.3%) put
    /// any `ComponentNN` selection in it. A Roadway LOD is authored as a surface,
    /// so there is no convex set to hull and `LODShape` keeps no ConvexComponents
    /// for that level at all. The flag is real and filters correctly; what it
    /// cannot yet do is find a static collider, and that is a fact about the data
    /// rather than a gap in the plumbing. Compare Fire Geometry, where 509 (38.4%)
    /// authored their own level and 422 (31.8%) of those are hullable.
    Roadway = 1u << 3,

    All = Solid | BulletCollision | ViewBlocking | Roadway,
};

constexpr ColliderFlags operator|(ColliderFlags a, ColliderFlags b)
{
    return static_cast<ColliderFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr ColliderFlags operator&(ColliderFlags a, ColliderFlags b)
{
    return static_cast<ColliderFlags>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr ColliderFlags& operator|=(ColliderFlags& a, ColliderFlags b) { return a = a | b; }
/// Do `value` and `mask` share a bit? The question every filter asks, spelled once
/// so no caller writes the `!= None` half of it wrong.
constexpr bool HasAny(ColliderFlags value, ColliderFlags mask) { return (value & mask) != ColliderFlags::None; }

/// Which colliders a query is allowed to see.
///
/// A struct rather than a bare mask so the seam can grow an exclusion or a layer
/// without changing every signature that carries it. Default is everything, which
/// is what an unfiltered query meant before flags existed.
struct QueryFilter
{
    /// A piece answers the query when it carries ANY of these.
    ColliderFlags any = ColliderFlags::All;
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
    /// Of `shapes`, those attached for QUERIES ONLY -- a Fire or View Geometry LOD
    /// the model authored separately from its Geometry LOD. They answer rays and
    /// take part in no contact, so this is the price of the flag set: zero for a
    /// corpus where every model reuses one LOD, and the number to look at first if
    /// a physics step gets slower after tagging.
    std::uint32_t queryOnlyShapes = 0;
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
    std::uint32_t articulatedBodies = 0;
    std::uint32_t articulatedJoints = 0;
    // Actual solver totals include terrain and the separately owned kinematic
    // proxy; tracked slots alone cannot prove an otherwise empty backend.
    std::uint32_t solverBodies = 0, solverJoints = 0;
    bool kinematicProxyRegistered = false;

    /// Mean wall-clock cost of ONE b3World_Step, in milliseconds, since the last
    /// reset. Multiply by the Fixed Step tab's ticks-per-frame for the per-frame
    /// cost -- the two numbers are deliberately separate because a step is a
    /// fixed unit of work and a frame is not.
    float meanStepMs = 0.0f;
    /// Steps behind that mean, so a figure from three samples is not read as a
    /// measurement.
    std::uint64_t stepsTimed = 0;
    /// Worst single step seen. A mean hides the hitch that a player feels.
    float worstStepMs = 0.0f;

    /// How many threads the BACKEND uses inside one step. 1 means the solver runs
    /// entirely on the calling thread.
    ///
    /// This is a measurement, not a setting, and it is on the boundary rather than
    /// buried in the backend because roadmap 8.4 turns on it: "parallel query
    /// access is enabled only for operations and backend versions whose
    /// thread-safety contract has been verified". A backend that quietly spawns its
    /// own workers changes the answer to that question, is invisible from
    /// PhysicsWorld, and would not appear in SIM-801's thread table either -- so
    /// its FP environment would be nobody's. Reporting it makes the assumption
    /// falsifiable by a test instead of by reading a third-party default.
    std::uint32_t workerCount = 0;

    bool terrainRegistered = false;
    /// Terrain grid actually handed to the backend, for comparison against the
    /// landscape's own dimensions.
    std::uint32_t terrainWidth = 0;
    float terrainCellSize = 0, terrainOriginX = 0, terrainOriginZ = 0;
    std::uint32_t terrainHeight = 0;
};

/// PHY-020's dev probe: one dynamic sphere, owned entirely by physics.
///
/// Deliberately a PROBE and not an entity. It exists to answer one question --
/// can Box3D move a dynamic body at 60 Hz across the real terrain and the real
/// Geometry-LOD colliders -- and it is drawn from a probe-only readback rather
/// than by giving gameplay a transform getter. That distinction is the PHY-010
/// boundary and PHY-020 does not cross it.
/// What the probe is. A capsule is NOT a ragdoll and is not called one: it is a
/// single rigid body that happens to be body-shaped, so it topples and slides
/// down a slope in a way a sphere cannot. A ragdoll needs a joint per bone pair
/// and a skeleton hierarchy the engine does not carry (PHY-040).
enum class ProbeShape
{
    Sphere,
    Capsule,
    /// Box and cylinder exist to be STACKED. A sphere proves the physics runs; a
    /// tower of boxes that stays standing until something hits it proves the
    /// contact solver and the friction actually behave.
    Box,
    Cylinder,
    /// A real model's Geometry LOD, made dynamic. The physics owns this body
    /// outright and NOTHING in the game knows about it -- it is a separate
    /// instance that exists only in the physics world and the draw list, so no
    /// gameplay entity is ever moved by the simulation and nothing is written
    /// back into the mission. That is what makes it safe to throw a jeep around
    /// while PHY-050 is still unplanned.
    Model,
};

struct SphereProbeDef
{
    ProbeShape shape = ProbeShape::Sphere;
    /// Capsule only: distance from the centre to each hemisphere centre, so the
    /// total length is 2 * halfLength + 2 * radius. 0.6 m with a 0.25 m radius is
    /// roughly a person lying down.
    float halfLength = 0.6f;
    /// Box only: half extents. A domino is thin on one axis and tall on another,
    /// which is the shape that makes a stack interesting.
    Vector3 halfExtents{0.1f, 0.4f, 0.25f};
    /// Yaw in radians. A domino has to FACE somewhere: a row of world-aligned
    /// bricks cannot be laid along a path, and the first one to fall would miss
    /// the next. Placement uses the camera's heading so a row builds itself as
    /// you walk it.
    float yaw = 0.0f;

    Vector3 position;
    Vector3 velocity{0, 0, 0};
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
/// One body's pose and motion, read back OUT of the physics world.
///
/// PHY-010 held that data only ever flows gameplay -> physics, so that nothing
/// in the game could depend on the solver. Owning a loose object breaks that by
/// necessity -- a barrel the solver moves is only useful if the game can see
/// where it went -- so the exception is named here rather than smuggled in as a
/// second meaning for something else. It applies to bodies a caller spawned as
/// dynamic and to nothing else.
struct BodyMotion
{
    Vector3 position;
    /// Orientation columns, in the same form Object keeps its own.
    Vector3 axisX{1, 0, 0};
    Vector3 axisY{0, 1, 0};
    Vector3 axisZ{0, 0, 1};
    Vector3 linearVelocity;
    Vector3 angularVelocity;
    /// False when the solver has put the body to sleep. A sleeping body needs no
    /// further reading and, in multiplayer, is the moment its owner can hand it
    /// back.
    bool awake = true;
};

struct ProbeSample
{
    /// So a caller holding its own per-probe data (a render object, say) can find
    /// the sample that belongs to it.
    BodyId     id;
    ProbeShape shape = ProbeShape::Sphere;
    Vector3    position;
    /// Rotation columns, so the debug view can draw a box or cylinder as it lies.
    Vector3 axisX{1, 0, 0};
    Vector3 axisY{0, 1, 0};
    Vector3 axisZ{0, 0, 1};
    Vector3 halfExtents;
    float   halfLength = 0.0f;
    float   radius = 0.0f;
    /// Capsule axis in world space, scaled by the half length. Zero for a sphere.
    /// Orientation is here only so the debug view can draw a capsule the way it
    /// actually lies; it is still probe-only and still not a gameplay transform.
    Vector3 axis;
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

struct ProbeWaterSample
{
    float height = 0.0f;
    Vector3 velocity{0, 0, 0};
};

// Injected at the engine boundary; standalone physics tests need no landscape.
struct ProbeWaterEnvironment
{
    bool (*sample)(Vector3Par position, ProbeWaterSample& out) = nullptr;
    void (*impact)(Vector3Par position, Vector3Par velocity, float radius, float mass, bool entering) = nullptr;
};

} // namespace Poseidon::Physics
