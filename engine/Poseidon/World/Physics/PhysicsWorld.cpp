#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/Core/Global.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/WaterBodies.hpp>
#include <Poseidon/World/Terrain/WaterSurfaceQuery.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <cstdint>
#include <memory>
#include <vector>

namespace Poseidon::Physics
{
namespace
{

// A convex hull needs a volume. Below this extent on every axis a "component" is
// a point, a line or a sheet, and hulling it either fails in the backend or
// produces a degenerate shape that behaves unpredictably at contact. Counting
// these separately from backend refusals matters: one says our data is thin, the
// other says the library disagreed with us, and they lead to different work.
constexpr float MinComponentExtent = 1e-3f;
// Four points is the minimum for a tetrahedron.
constexpr int MinComponentPoints = 4;

std::unique_ptr<PhysicsWorld> g_world;

bool SampleProbeWater(Vector3Par p, ProbeWaterSample& out)
{
    if (!GLandscape) return false;
    float level = GLandscape->GetSeaLevel();
    float waveScale = 1.0f;
    if (const WaterBody* body = GetWaterBodies().Find(p[0], p[2]))
    {
        level = body->SurfaceLevelAt(p[0], p[2]);
        waveScale = body->waveScale;
    }
    if (GLandscape->SurfaceY(p[0], p[2]) >= level) return false;
    const auto surface = QueryWaterSurfaceScaled(p[0], p[2], Glob.time.toFloat(), level, waveScale);
    out.height = surface.height;
    out.velocity = Vector3(surface.velocityX, 0, surface.velocityZ);
    return true;
}

void ProbeWaterImpact(Vector3Par p, Vector3Par velocity, float radius, float mass, bool entering)
{
    const float speed = velocity.Size();
    // Displaced water bounds the coupled mass: a dense projectile does not
    // impart all of its kinetic energy to the surface instantaneously.
    const float coupledMass = std::min(mass, 1000.0f * 4.1887902f * radius * radius * radius);
    const float strength = std::clamp(std::sqrt(std::max(0.0f, coupledMass)) * speed * 0.035f, 0.02f, 4.5f);
    HydroWaterInteractionEvent event{};
    event.positionRadius[0] = p[0];
    event.positionRadius[1] = p[2];
    event.positionRadius[2] = std::clamp(radius * 1.5f, 0.15f, 8.0f);
    event.positionRadius[3] = entering ? strength : strength * 0.15f;
    event.velocityKind[0] = velocity[0];
    event.velocityKind[1] = velocity[2];
    event.velocityKind[2] = velocity[1];
    event.velocityKind[3] = entering ? HydroWaterInteractionObject : HydroWaterInteractionContinuous;
    event.timeLifeFoamMass[1] = 0.8f;
    event.timeLifeFoamMass[2] = std::min(strength * 0.2f, 0.8f);
    event.timeLifeFoamMass[3] = mass;
    event.directionDepthFlags[3] = HydroWaterInteractionPendingImpulse;
    SubmitWaterInteraction(event);
    static const bool diagnostic = std::getenv("POSEIDON_PHYSICS_PROBE") != nullptr;
    static int diagnosticSamples = 0;
    if (diagnostic && diagnosticSamples < 32)
    {
        ++diagnosticSamples;
        LOG_INFO(World, "PROBE-WATER: {} mass {:.3f} radius {:.3f} speed {:.3f} strength {:.3f} at ({:.2f},{:.2f},{:.2f})",
                 entering ? "entry" : "wake", mass, radius, speed, event.positionRadius[3], p[0], p[1], p[2]);
    }
    if (!entering || !GWorld || speed < 0.5f) return;
    WaterSource spray;
    spray.SetSize(0.035f, std::clamp(radius * 0.1f, 0.05f, 0.25f));
    spray.SetFades(0.06f, 0.03f, 0.35f);
    spray.SetTimes(0.06f, 0.6f);
    const int count = std::clamp(static_cast<int>(strength * 4.0f), 2, 12);
    for (int i = 0; i < count; ++i)
    {
        const float angle = i * (6.2831853f / count);
        const float spread = std::clamp(speed * 0.12f, 0.2f, 3.0f);
        Vector3 v(std::cos(angle) * spread + velocity[0] * 0.1f,
                  std::clamp(speed * 0.35f, 0.3f, 7.0f),
                  std::sin(angle) * spread + velocity[2] * 0.1f);
        if (Cloudlet* drop = spray.Drop(p, v)) GWorld->AddCloudlet(drop);
    }
}

} // namespace

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;

bool PhysicsWorld::Create()
{
    if (_backend)
    {
        return true;
    }
    _backend = CreatePhysicsBackend();
    if (!_backend)
    {
        return false;
    }
    if (!_backend->Create())
    {
        _backend.reset();
        return false;
    }
    ResetCounters();
    ++_generation;
    _backend->SetWaterEnvironment({SampleProbeWater, ProbeWaterImpact});
    return true;
}

void PhysicsWorld::Destroy()
{
    ++_generation;
    if (_backend)
    {
        _backend->Destroy();
        _backend.reset();
    }
    ResetCounters();
}

void PhysicsWorld::ResetCounters()
{
    // The world is process-wide and long-lived, so without this two missions
    // accumulate into one report and the corpus numbers stop meaning anything.
    // Exactly the trap the fixed-step accumulator had, one layer up.
    _modelsRegistered = 0;
    _modelsWithoutGeometry = 0;
    _degenerateComponents = 0;
    _maxComponentPoints = 0;
    _stepMsTotal = 0.0;
    _stepMsWorst = 0.0;
    _stepsTimed = 0;
}

void PhysicsWorld::Step(float deltaSeconds)
{
    if (!_backend)
    {
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    _backend->Step(deltaSeconds);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    _stepMsTotal += ms;
    _stepMsWorst = std::max(_stepMsWorst, ms);
    ++_stepsTimed;
}

BodyId PhysicsWorld::RegisterStatic(LODShape* shape, const Matrix4& transform)
{
    return RegisterStaticDetailed(shape, transform).id;
}

// Extracts a model's authored convex components into flat spans the backend can
// take. Shared by the static registration and the dynamic model probe, because
// "make this model collide" and "make this model fall over" want exactly the same
// geometry and any drift between the two would be invisible until something
// behaved differently as a probe than it did as scenery. `wanted` is what keeps
// them apart where they must differ: a probe asks for Solid alone, so a falling
// jeep is its Geometry hull and not also the sight-blocking box around it.
//
// The LOD kinds are MERGED BY LEVEL INDEX before anything is read. LODShape
// already collapses an absent View level onto Geometry and an absent Fire level
// onto View, so on most models all three resolve to the same level -- and reading
// them one at a time would register that one level's hulls three times over.
bool PhysicsWorld::ExtractPieces(LODShape* shape, ColliderFlags wanted, std::vector<Vector3>& points,
                                 std::vector<ConvexPiece>& pieces, int& degenerate, int& maxPoints,
                                 RegisterResult& result)
{
    points.clear();
    pieces.clear();
    degenerate = 0;
    maxPoints = 0;
    if (!shape)
    {
        return false;
    }

    const int geometryLevel = shape->FindGeometryLevel();

    // One entry per LOD kind, in the order the flags are declared. The level index
    // is what identifies a source, not the kind.
    struct Source
    {
        int           level;
        ColliderFlags kind;
    };
    const Source sources[] = {
        {geometryLevel, ColliderFlags::Solid},
        {shape->FindFireGeometryLevel(), ColliderFlags::BulletCollision},
        {shape->FindViewGeometryLevel(), ColliderFlags::ViewBlocking},
        {shape->FindRoadwayLevel(), ColliderFlags::Roadway},
    };

    // Merged: level index -> the union of kinds resolving to it.
    int           mergedLevels[std::size(sources)];
    ColliderFlags mergedKinds[std::size(sources)];
    int           mergedCount = 0;
    for (const Source& source : sources)
    {
        if (source.level < 0 || !HasAny(wanted, source.kind))
        {
            continue;
        }
        result.lodKindsResolved |= source.kind;
        if (source.level != geometryLevel)
        {
            result.lodKindsDedicated |= source.kind;
        }
        int found = -1;
        for (int i = 0; i < mergedCount; ++i)
        {
            if (mergedLevels[i] == source.level)
            {
                found = i;
                break;
            }
        }
        if (found >= 0)
        {
            mergedKinds[found] |= source.kind;
            continue;
        }
        mergedLevels[mergedCount] = source.level;
        mergedKinds[mergedCount] = source.kind;
        ++mergedCount;
    }
    if (mergedCount == 0)
    {
        return false;
    }

    std::vector<int> offsets;

    for (int s = 0; s < mergedCount; ++s)
    {
        Shape* level = shape->Level(mergedLevels[s]);
        if (!level || level->NVertex() <= 0)
        {
            continue;
        }
        // The Roadway and Land Contact levels have no ConvexComponents set at all
        // -- `LODShape::GetConvexComponents` returns null for them, because they are
        // authored as surfaces rather than as named `ComponentNN` volumes. Nothing
        // is invented to fill that in: the kind stays out of `flagsRegistered`, and
        // the census reports how often the corpus asks for it.
        ConvexComponents* components = shape->GetConvexComponents(mergedLevels[s]);
        if (!components)
        {
            continue;
        }
        shape->RecalculateConvexComponentsAsNeeded(mergedLevels[s]);
        if (components->Size() <= 0)
        {
            continue;
        }

        for (int i = 0; i < components->Size(); ++i)
        {
            const ConvexComponent* component = (*components)[i];
            if (!component || component->Size() < MinComponentPoints)
            {
                ++degenerate;
                continue;
            }
            maxPoints = std::max(maxPoints, component->Size());

            const int begin = static_cast<int>(points.size());
            Vector3   lo(0, 0, 0);
            Vector3   hi(0, 0, 0);
            bool      first = true;
            for (int p = 0; p < component->Size(); ++p)
            {
                const int vertex = (*component)[p];
                if (vertex < 0 || vertex >= level->NVertex())
                {
                    continue;
                }
                const Vector3 position = level->Pos(vertex);
                points.push_back(position);
                if (first)
                {
                    lo = hi = position;
                    first = false;
                }
                else
                {
                    for (int a = 0; a < 3; ++a)
                    {
                        lo[a] = std::min(lo[a], position[a]);
                        hi[a] = std::max(hi[a], position[a]);
                    }
                }
            }

            const int   count = static_cast<int>(points.size()) - begin;
            const float smallestExtent = std::min({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
            if (count < MinComponentPoints || smallestExtent < MinComponentExtent)
            {
                points.resize(static_cast<std::size_t>(begin));
                ++degenerate;
                continue;
            }
            offsets.push_back(begin);
            pieces.push_back(ConvexPiece{nullptr, count, mergedKinds[s]});
            result.flagsRegistered |= mergedKinds[s];
        }
    }

    // Pointers last: `points` reallocates as it grows, so anything taken during the
    // loop above would dangle.
    for (std::size_t i = 0; i < pieces.size(); ++i)
    {
        pieces[i].points = points.data() + offsets[i];
    }
    return !pieces.empty();
}

static void TracePhysicsPieces(LODShape* shape, const Matrix4& transform,
                               const std::vector<ConvexPiece>& pieces, bool dynamic)
{
    const char* filter = std::getenv("POSEIDON_PHYSICS_COMPONENT_TRACE");
    if (!filter || !*filter || !shape || !std::strstr(shape->Name(), filter)) return;
    for (std::size_t i = 0; i < pieces.size(); ++i)
    {
        const auto& piece = pieces[i];
        if (!HasAny(piece.flags, ColliderFlags::Solid) || piece.count < 1) continue;
        Vector3 lo = transform.FastTransform(piece.points[0]), hi = lo;
        for (int j = 1; j < piece.count; ++j)
        {
            const Vector3 p = transform.FastTransform(piece.points[j]);
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a],p[a]); hi[a] = std::max(hi[a],p[a]); }
        }
        LOG_INFO(World, "PHYSCOMP: {} {} piece {} scale {} bounds [{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f}]",
                 dynamic ? "dynamic" : "static", shape->Name(), i, transform.Scale(),
                 lo.X(),lo.Y(),lo.Z(),hi.X(),hi.Y(),hi.Z());
    }
}

BodyId PhysicsWorld::SpawnModelProbe(LODShape* shape, const Matrix4& transform, float mass, float friction,
                                     float restitution, Vector3Par velocity)
{
    if (!_backend)
    {
        return {};
    }
    std::vector<Vector3>     points;
    std::vector<ConvexPiece> pieces;
    int                      degenerate = 0;
    int                      maxPoints = 0;
    RegisterResult           census;
    // Solid only. A model probe is one falling object, and giving it its View
    // Geometry box as well would make it collide with the world at the size of its
    // occluder rather than of its hull.
    if (!ExtractPieces(shape, ColliderFlags::Solid, points, pieces, degenerate, maxPoints, census))
    {
        return {};
    }
    TracePhysicsPieces(shape, transform, pieces, true);
    return _backend->SpawnDynamicPieces(pieces.data(), static_cast<int>(pieces.size()), transform, mass, friction,
                                        restitution, velocity);
}

void PhysicsWorld::ApplyImpulse(BodyId id, Vector3Par point, Vector3Par impulse)
{
    if (_backend && id.IsValid())
    {
        _backend->ApplyImpulse(id, point, impulse);
    }
}

BodyId PhysicsWorld::CastRay(Vector3Par from, Vector3Par to, Vector3& hitPoint, const QueryFilter& filter) const
{
    return _backend ? _backend->CastRay(from, to, hitPoint, filter) : BodyId{};
}

bool PhysicsWorld::RayHitAnything(Vector3Par from, Vector3Par to, const QueryFilter& filter) const
{
    return _backend && _backend->RayHitAnything(from, to, filter);
}

bool PhysicsWorld::MeasureThickness(Vector3Par entry, Vector3Par direction, float maxDepth, float& thickness,
                                    const QueryFilter& filter) const
{
    thickness = 0.0f;
    if (!_backend || maxDepth <= 0.0f)
    {
        return false;
    }
    const float len = direction.Size();
    if (len < 1e-6f)
    {
        return false;
    }
    const Vector3 dir = direction / len;

    // Backwards from beyond the object. The far face is the first thing this ray
    // meets, and the distance from `entry` to it is the thickness along the shot's
    // own line -- which is what matters, not the wall's nominal thickness: a
    // glancing shot through the same wall travels further inside it.
    const Vector3 beyond = entry + dir * maxDepth;
    Vector3       exit;
    if (!_backend->CastRay(beyond, entry, exit, filter).IsValid() &&
        !_backend->RayHitAnything(beyond, entry, filter))
    {
        return false;
    }
    // CastRay reports the point even when the body is untracked (terrain), but only
    // fills it on a hit, so re-cast to read the point rather than trust a stale one.
    Vector3 point;
    _backend->CastRay(beyond, entry, point, filter);

    const float depth = (point - entry) * dir;
    if (depth <= 0.0f || depth > maxDepth)
    {
        return false;
    }
    thickness = depth;
    return true;
}

RegisterResult PhysicsWorld::RegisterStaticDetailed(LODShape* shape, const Matrix4& transform)
{
    RegisterResult result;
    if (!_backend || !shape)
    {
        return result;
    }

    std::vector<Vector3>     points;
    std::vector<ConvexPiece> pieces;
    int                      degenerate = 0;
    int                      maxPoints = 0;
    const bool               usable =
        ExtractPieces(shape, ColliderFlags::All, points, pieces, degenerate, maxPoints, result);

    _degenerateComponents += static_cast<std::uint32_t>(degenerate);
    result.maxComponentPoints = maxPoints;
    _maxComponentPoints = std::max(_maxComponentPoints, static_cast<std::uint32_t>(maxPoints));

    if (!usable)
    {
        // No Geometry LOD at all, versus had components and none of them a volume.
        // Different findings: one says the model carries no collision by design
        // (roads, grass), the other says its data is thin.
        if (degenerate > 0)
        {
            result.outcome = RegisterResult::Outcome::AllComponentsDegenerate;
        }
        else
        {
            ++_modelsWithoutGeometry;
            result.outcome = RegisterResult::Outcome::NoGeometryLod;
        }
        return result;
    }

    TracePhysicsPieces(shape, transform, pieces, false);
    result.id = _backend->AddStaticBody(pieces.data(), static_cast<int>(pieces.size()), transform);
    if (result.id.IsValid())
    {
        ++_modelsRegistered;
        result.outcome = RegisterResult::Outcome::Registered;
    }
    else
    {
        result.outcome = RegisterResult::Outcome::BackendRefused;
    }
    return result;
}

void PhysicsWorld::SetTransform(BodyId id, const Matrix4& transform)
{
    if (_backend && id.IsValid())
    {
        _backend->SetBodyTransform(id, transform);
    }
}

void PhysicsWorld::Remove(BodyId id)
{
    if (_backend && id.IsValid())
    {
        _backend->RemoveBody(id);
    }
}

bool PhysicsWorld::SetTerrain(const float* heights, int width, int height, float cellSize, float originX,
                              float originZ)
{
    ++_terrainMutationSerial;
    if (!_backend || !heights || width <= 1 || height <= 1 || cellSize <= 0.0f ||
        !std::isfinite(cellSize) || !std::isfinite(originX) || !std::isfinite(originZ))
    {
        return false;
    }
    TerrainField field;
    field.heights = heights;
    field.width = width;
    field.height = height;
    field.cellSize = cellSize;
    field.originX = originX;
    field.originZ = originZ;
    return _backend->SetTerrain(field);
}

bool PhysicsWorld::PatchTerrain(int x, int z, int width, int height, const float* samples)
{
    if (!_backend) return false;
    ++_terrainMutationSerial;
    return _backend->PatchTerrain(x, z, width, height, samples);
}

BodyId PhysicsWorld::SpawnSphereProbe(const SphereProbeDef& def)
{
    return _backend ? _backend->SpawnSphereProbe(def) : BodyId{};
}

void PhysicsWorld::ClearProbes()
{
    if (_backend)
    {
        _backend->ClearProbes();
    }
}

bool PhysicsWorld::GetBodyMotion(BodyId id, BodyMotion& out) const
{
    return _backend && _backend->GetBodyMotion(id, out);
}

bool PhysicsWorld::GetArticulatedGroundSupport(BodyId id, Vector3& point) const
{ return _backend && _backend->GetArticulatedGroundSupport(id,point); }

BodyId PhysicsWorld::SpawnArticulatedPiece(const ConvexPiece& piece, const Matrix4& frame, float mass)
{ return SpawnArticulatedPiece(piece,frame,mass,ArticulatedInitialMotion{}); }
BodyId PhysicsWorld::SpawnArticulatedPiece(const ConvexPiece& piece, const Matrix4& frame, float mass,
    const ArticulatedInitialMotion& motion)
{
    return _backend ? _backend->SpawnArticulatedPiece(piece, frame, mass, motion) : BodyId{};
}
bool PhysicsWorld::SetArticulatedMotion(BodyId body, Vector3Par linear, Vector3Par angular)
{ return _backend && _backend->SetArticulatedMotion(body,linear,angular); }
JointId PhysicsWorld::AddArticulatedJoint(const ArticulatedJointDef& definition)
{
    return _backend ? _backend->AddArticulatedJoint(definition) : JointId{};
}
void PhysicsWorld::RemoveJoint(JointId id)
{
    if (_backend && id.IsValid()) _backend->RemoveJoint(id);
}

void PhysicsWorld::GetProbeSamples(std::vector<ProbeSample>& out) const
{
    out.clear();
    if (_backend)
    {
        _backend->GetProbeSamples(out);
    }
}

void PhysicsWorld::SetWind(const WindSettings& wind)
{
    _wind = wind;
    if (_backend)
    {
        _backend->SetWind(wind);
    }
}

void PhysicsWorld::SetKinematicProxy(Vector3Par position, float radius, float height)
{
    if (_backend)
    {
        _backend->SetKinematicProxy(position, radius, height);
    }
}

PhysicsStats PhysicsWorld::GetStats() const
{
    // Backend owns what it built; the facade owns what it refused to hand over.
    PhysicsStats stats = _backend ? _backend->GetStats() : PhysicsStats{};
    stats.modelsRegistered = _modelsRegistered;
    stats.modelsWithoutGeometry = _modelsWithoutGeometry;
    stats.degenerateComponents = _degenerateComponents;
    stats.maxComponentPoints = _maxComponentPoints;
    stats.stepsTimed = _stepsTimed;
    stats.meanStepMs = _stepsTimed ? static_cast<float>(_stepMsTotal / static_cast<double>(_stepsTimed)) : 0.0f;
    stats.worstStepMs = static_cast<float>(_stepMsWorst);
    return stats;
}

const char* PhysicsWorld::BackendName() const { return _backend ? _backend->Name() : "none"; }

PhysicsWorld* GetPhysicsWorld() { return g_world.get(); }

PhysicsWorld& EnsurePhysicsWorld()
{
    if (!g_world)
    {
        g_world = std::make_unique<PhysicsWorld>();
    }
    return *g_world;
}

} // namespace Poseidon::Physics
