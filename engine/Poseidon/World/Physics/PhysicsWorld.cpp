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
    return true;
}

void PhysicsWorld::Destroy()
{
    if (_backend)
    {
        _backend->Destroy();
        _backend.reset();
    }
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
    if (!_backend || !shape)
    {
        return {};
    }

    // The Geometry LOD is the authored collision hull set. A model without one is
    // not broken -- plenty of props have no collision at all -- so this is counted
    // and not warned about.
    Shape* geometry = shape->GeometryLevel();
    if (!geometry || geometry->NVertex() <= 0)
    {
        ++_modelsWithoutGeometry;
        return {};
    }

    // Components are computed lazily and invalidated by animation, so ask for a
    // refresh rather than trusting whatever the last consumer left behind.
    shape->RecalculateGeomComponentsAsNeeded();
    const ConvexComponents& components = shape->GetGeomComponents();
    if (components.Size() <= 0)
    {
        ++_modelsWithoutGeometry;
        return {};
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
        const bool flat = (hi[0] - lo[0]) < MinComponentExtent && (hi[1] - lo[1]) < MinComponentExtent &&
                          (hi[2] - lo[2]) < MinComponentExtent;
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
    if (pieces.empty())
    {
        ++_modelsWithoutGeometry;
        return {};
    }

    const BodyId id = _backend->AddStaticBody(pieces.data(), static_cast<int>(pieces.size()), transform);
    if (id.IsValid())
    {
        ++_modelsRegistered;
    }
    return id;
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
