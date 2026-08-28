#include <Poseidon/World/Physics/PhysicsWorld.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>

#include <algorithm>
#include <chrono>
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
    return true;
}

void PhysicsWorld::Destroy()
{
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
// behaved differently as a probe than it did as scenery.
bool PhysicsWorld::ExtractPieces(LODShape* shape, std::vector<Vector3>& points, std::vector<ConvexPiece>& pieces,
                                 int& degenerate, int& maxPoints)
{
    points.clear();
    pieces.clear();
    degenerate = 0;
    maxPoints = 0;

    Shape* geometry = shape ? shape->GeometryLevel() : nullptr;
    if (!geometry || geometry->NVertex() <= 0)
    {
        return false;
    }
    shape->RecalculateGeomComponentsAsNeeded();
    const ConvexComponents& components = shape->GetGeomComponents();
    if (components.Size() <= 0)
    {
        return false;
    }

    pieces.reserve(components.Size());
    std::vector<int> offsets;
    offsets.reserve(components.Size());

    for (int i = 0; i < components.Size(); ++i)
    {
        const ConvexComponent* component = components[i];
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
            if (vertex < 0 || vertex >= geometry->NVertex())
            {
                continue;
            }
            const Vector3 position = geometry->Pos(vertex);
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
        pieces.push_back(ConvexPiece{nullptr, count});
    }

    for (std::size_t i = 0; i < pieces.size(); ++i)
    {
        pieces[i].points = points.data() + offsets[i];
    }
    return !pieces.empty();
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
    if (!ExtractPieces(shape, points, pieces, degenerate, maxPoints))
    {
        return {};
    }
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

BodyId PhysicsWorld::CastRay(Vector3Par from, Vector3Par to, Vector3& hitPoint) const
{
    return _backend ? _backend->CastRay(from, to, hitPoint) : BodyId{};
}

bool PhysicsWorld::RayHitAnything(Vector3Par from, Vector3Par to) const
{
    return _backend && _backend->RayHitAnything(from, to);
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
    const bool               usable = ExtractPieces(shape, points, pieces, degenerate, maxPoints);

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
    if (!_backend || !heights || width <= 1 || height <= 1 || cellSize <= 0.0f)
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
