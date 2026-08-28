#include <Poseidon/World/Physics/PhysicsWorld.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>

#include <algorithm>
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
}

void PhysicsWorld::Step(float deltaSeconds)
{
    if (_backend)
    {
        _backend->Step(deltaSeconds);
    }
}

BodyId PhysicsWorld::RegisterStatic(LODShape* shape, const Matrix4& transform)
{
    return RegisterStaticDetailed(shape, transform).id;
}

RegisterResult PhysicsWorld::RegisterStaticDetailed(LODShape* shape, const Matrix4& transform)
{
    RegisterResult result;
    if (!_backend || !shape)
    {
        return result;
    }

    // The Geometry LOD is the authored collision hull set. A model without one is
    // not broken -- plenty of props have no collision at all -- so this is counted
    // and not warned about.
    Shape* geometry = shape->GeometryLevel();
    if (!geometry || geometry->NVertex() <= 0)
    {
        ++_modelsWithoutGeometry;
        result.outcome = RegisterResult::Outcome::NoGeometryLod;
        return result;
    }

    // Components are computed lazily and invalidated by animation, so ask for a
    // refresh rather than trusting whatever the last consumer left behind.
    shape->RecalculateGeomComponentsAsNeeded();
    const ConvexComponents& components = shape->GetGeomComponents();
    if (components.Size() <= 0)
    {
        ++_modelsWithoutGeometry;
        result.outcome = RegisterResult::Outcome::NoGeometryLod;
        return result;
    }

    // Flattened point soup plus a piece table: the backend gets contiguous spans
    // and never walks our Selection type. One allocation pair per model, freed on
    // return -- the backend copies what it keeps.
    std::vector<Vector3>     points;
    std::vector<ConvexPiece> pieces;
    pieces.reserve(components.Size());
    // Offsets first, pointers after: `points` reallocates while filling, and a
    // pointer taken before a reallocation is the classic way this loop breaks.
    std::vector<int> offsets;
    offsets.reserve(components.Size());

    int degenerate = 0;
    for (int i = 0; i < components.Size(); ++i)
    {
        const ConvexComponent* component = components[i];
        if (!component || component->Size() < MinComponentPoints)
        {
            ++degenerate;
            continue;
        }

        result.maxComponentPoints = std::max(result.maxComponentPoints, component->Size());

        const int   begin = static_cast<int>(points.size());
        Vector3     lo(0, 0, 0);
        Vector3     hi(0, 0, 0);
        bool        first = true;
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
                lo[0] = std::min(lo[0], position[0]);
                lo[1] = std::min(lo[1], position[1]);
                lo[2] = std::min(lo[2], position[2]);
                hi[0] = std::max(hi[0], position[0]);
                hi[1] = std::max(hi[1], position[1]);
                hi[2] = std::max(hi[2], position[2]);
            }
        }

        const int count = static_cast<int>(points.size()) - begin;
        // SMALLEST extent, not all three. The previous form required every axis to
        // be tiny, which only catches a point -- a wall-sized sheet lying in a
        // plane sailed through it and then failed in the backend, so a data problem
        // was reported as a library refusal.
        //
        // This catches a point, an axis-aligned line and an axis-aligned sheet,
        // which is what authored Geometry components actually degenerate into.
        // A set of points coplanar in an OBLIQUE plane still has extent on all
        // three AABB axes and passes; Box3D remains the final validator for those
        // and they land in hullFailures. Detecting them properly needs a rank or
        // PCA pass over every component of every model, which is not worth paying
        // for a diagnostic label.
        const float smallestExtent =
            std::min({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        const bool flat = smallestExtent < MinComponentExtent;
        if (count < MinComponentPoints || flat)
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

    _degenerateComponents += static_cast<std::uint32_t>(degenerate);
    _maxComponentPoints =
        std::max(_maxComponentPoints, static_cast<std::uint32_t>(result.maxComponentPoints));

    if (pieces.empty())
    {
        // It HAD components; every one of them was a point, a line or a sheet.
        // Distinct from having no Geometry LOD at all, because it points at the
        // data rather than at the model simply not carrying collision.
        result.outcome = RegisterResult::Outcome::AllComponentsDegenerate;
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

PhysicsStats PhysicsWorld::GetStats() const
{
    // Backend owns what it built; the facade owns what it refused to hand over.
    PhysicsStats stats = _backend ? _backend->GetStats() : PhysicsStats{};
    stats.modelsRegistered = _modelsRegistered;
    stats.modelsWithoutGeometry = _modelsWithoutGeometry;
    stats.degenerateComponents = _degenerateComponents;
    stats.maxComponentPoints = _maxComponentPoints;
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
