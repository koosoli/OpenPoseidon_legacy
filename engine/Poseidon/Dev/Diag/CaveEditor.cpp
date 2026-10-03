#include <Poseidon/Dev/Diag/CaveEditor.hpp>
#include <Poseidon/Dev/Diag/CaveMesh.hpp>
#include <Poseidon/Dev/Diag/TrenchMesh.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace Poseidon::Dev
{
namespace
{
CaveEditorSettings settings;
Landscape* owner = nullptr;
struct Record
{
    Ref<Object> object;
    EditorCaveInfo info;
};
std::vector<Record> caves;
std::uint32_t nextId = 1, selectedId = 0;
const char* status = "Choose a hillside and point into the mountain.";
constexpr float Pi = 3.14159265359f;
using Point = std::pair<float, float>;
using Polygon = std::vector<Point>;
float Cross(Point a, Point b, Point c)
{
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}
float Heading(float h)
{
    h = std::fmod(h, 360.0f);
    return h < 0 ? h + 360.0f : h;
}
Vector3 Forward(float h)
{
    const float a = Heading(h) * (Pi / 180);
    return Vector3(std::sin(a), 0, std::cos(a));
}
Polygon Footprint(const EditorCaveInfo& i)
{
    const Vector3 f = Forward(i.heading), a(f.Z(), 0, -f.X()), p(i.x, i.y, i.z);
    Polygon out;
    for (const auto& c : std::array<Point, 4>{
             {{-i.width * .5f, 0}, {i.width * .5f, 0}, {i.width * .5f, i.length}, {-i.width * .5f, i.length}}})
    {
        const Vector3 w = p + a * c.first + f * c.second;
        out.emplace_back(w.X(), w.Z());
    }
    return out;
}
Polygon Hull(Polygon points)
{
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3)
        return {};
    Polygon h(points.size() * 2);
    std::size_t n = 0;
    for (const auto& p : points)
    {
        while (n >= 2 && Cross(h[n - 2], h[n - 1], p) <= 0)
            --n;
        h[n++] = p;
    }
    const auto lower = n + 1;
    for (std::size_t k = points.size() - 1; k > 0; --k)
    {
        const auto p = points[k - 1];
        while (n >= lower && Cross(h[n - 2], h[n - 1], p) <= 0)
            --n;
        h[n++] = p;
    }
    h.resize(n - 1);
    return h;
}
bool Overlap(const Polygon& a, const Polygon& b)
{
    if (a.size() < 3 || b.size() < 3)
        return false;
    for (const auto* p : {&a, &b})
        for (std::size_t k = 0; k < p->size(); ++k)
        {
            const auto v = (*p)[k], u = (*p)[(k + 1) % p->size()];
            const float nx = -(u.second - v.second), nz = u.first - v.first;
            float amin = std::numeric_limits<float>::max(), amax = -amin, bmin = amin, bmax = amax;
            for (const auto& q : a)
            {
                const float d = nx * q.first + nz * q.second;
                amin = std::min(amin, d);
                amax = std::max(amax, d);
            }
            for (const auto& q : b)
            {
                const float d = nx * q.first + nz * q.second;
                bmin = std::min(bmin, d);
                bmax = std::max(bmax, d);
            }
            if (amax < bmin || bmax < amin)
                return false;
        }
    return true;
}
EditorCaveInfo Describe(const Record& r)
{
    auto i = r.info;
    const auto p = r.object->Position();
    i.x = p.X();
    i.y = p.Y();
    i.z = p.Z();
    i.heading = Heading(std::atan2(r.object->Direction().X(), r.object->Direction().Z()) * (180 / Pi));
    return i;
}
Record* Find(std::uint32_t id)
{
    if (owner == GLandscape)
        for (auto& r : caves)
            if (r.info.id == id)
                return &r;
    return nullptr;
}
// Read actual owners and skip only the selected Object pointer. Equal polygons
// or equal shape names never grant permission to ignore an authored/imported hole.
bool HasOtherHole(const EditorCaveInfo& i, const Object* ignore)
{
    const auto candidate = Footprint(i);
    const Vector3 centre = Vector3(i.x, i.y, i.z) + Forward(i.heading) * (i.length * .5f);
    int xmin, xmax, zmin, zmax;
    ObjRadiusRectangle(xmin, xmax, zmin, zmax, centre, centre, i.length + i.width + 1);
    std::size_t inspected = 0;
    const auto check = [&](const Object* obj)
    {
        if (!obj || obj == ignore || !Landscape::CutsTerrainHole(obj))
            return false;
        const auto* memory = obj->GetShape()->MemoryLevel();
        for (int k = 0; k < 18; ++k)
        {
            char name[64];
            const int suffix = k % 9;
            if (suffix)
                std::snprintf(name, sizeof(name), k < 9 ? "terrain_hole%d" : "terrain_hole_hidden%d", suffix);
            else
                std::snprintf(name, sizeof(name), "%s", k < 9 ? "terrain_hole" : "terrain_hole_hidden");
            const int index = memory->FindNamedSel(name);
            if (index < 0)
                continue;
            const auto& selection = memory->NamedSel(index);
            if (selection.Size() > 4096)
                return true;
            Polygon points;
            for (int v = 0; v < selection.Size(); ++v)
            {
                const auto local = memory->Pos(selection[v]);
                const auto p = obj->PositionModelToWorld(Vector3(local.X(), 0, local.Z()));
                if (!std::isfinite(p.X()) || !std::isfinite(p.Z()))
                    return true;
                points.emplace_back(p.X(), p.Z());
            }
            if (Overlap(candidate, Hull(std::move(points))))
                return true;
        }
        return false;
    };
    for (int z = std::max(0, zmin); z <= zmax && z < GLandscape->GetLandRange(); ++z)
        for (int x = std::max(0, xmin); x <= xmax && x < GLandscape->GetLandRange(); ++x)
        {
            const auto& objects = GLandscape->GetObjects(z, x);
            for (int j = 0; j < objects.Size(); ++j)
                if (++inspected > 16384 || check(objects[j]))
                    return true;
        }
    for (int j = 0; j < GWorld->NBuildings(); ++j)
        if (++inspected > 16384 || check(GWorld->GetBuilding(j)))
            return true;
    return false;
}
bool ValidSite(const EditorCaveInfo& i, const Object* ignore)
{
    if (!std::isfinite(i.x) || !std::isfinite(i.y) || !std::isfinite(i.z) || !std::isfinite(i.heading) ||
        !std::isfinite(i.width) || !std::isfinite(i.height) || !std::isfinite(i.length) || i.width <= 0 ||
        i.height <= 0 || i.length <= 0)
    {
        status = "Excavation refused: invalid live position or dimensions.";
        return false;
    }
    const bool cave = i.tool == ExcavationTool::Cave, tank = i.tool == ExcavationTool::TankTrench;
    const float extent = (GLandscape->GetTerrainRange() - 1) * GLandscape->GetTerrainGrid();
    const Vector3 f = Forward(i.heading), a(f.Z(), 0, -f.X()), p(i.x, i.y, i.z);
    for (int along = 0; along <= 24; ++along)
        for (int side = -1; side <= 1; ++side)
        {
            const auto q = p + f * ((i.length + .4f) * along / 24) + a * (side * (i.width * .5f + .4f));
            if (q.X() < 1 || q.Z() < 1 || q.X() >= extent - 1 || q.Z() >= extent - 1 ||
                i.y - (cave ? 0 : i.height) <= GLandscape->GetSeaLevel() + .1f)
            {
                status = "Excavation refused: outside land bounds or its floor is below water.";
                return false;
            }
            if (!cave && std::abs(GLandscape->SurfaceY(q.X(), q.Z()) - i.y) > .25f)
            {
                status = "Choose gentler ground: the trench rim must remain within 25 cm of level.";
                return false;
            }
        }
    if (cave)
    {
        const auto back = p + f * i.length;
        if (GLandscape->SurfaceY(back.X(), back.Z()) < i.y + i.height + .4f)
        {
            status = "Point into rising ground: the back needs terrain above its ceiling.";
            return false;
        }
        for (int s = 1; s <= 16; ++s)
        {
            const auto q = p + f * (i.length * s / 16);
            if (GLandscape->SurfaceY(q.X(), q.Z()) < i.y - .2f)
            {
                status = "Cave refused: ground drops below its floor.";
                return false;
            }
        }
    }
    if (HasOtherHole(i, ignore))
    {
        status = "Excavation refused: overlap or bounded owner scan exhausted.";
        return false;
    }
    return true;
}
bool Occupied(const EditorCaveInfo& i, bool newFootprint = false)
{
    auto shell = i;
    shell.width += .8f;
    shell.length += .4f;
    const auto footprint = Footprint(shell);
    const auto check = [&](const Object* object)
    {
        if (!object)
            return false;
        Polygon body;
        float minY = object->Position().Y(), maxTerrain = -std::numeric_limits<float>::max();
        const auto add = [&](Vector3Par p)
        {
            body.emplace_back(p.X(), p.Z());
            minY = std::min(minY, p.Y());
            const float extent = (GLandscape->GetTerrainRange() - 1) * GLandscape->GetTerrainGrid();
            if (p.X() >= 0 && p.Z() >= 0 && p.X() < extent && p.Z() < extent)
                maxTerrain = std::max(maxTerrain, GLandscape->SurfaceY(p.X(), p.Z()));
        };
        add(object->Position());
        if (const auto* shape = object->GetShape())
            for (int c = 0; c < 8; ++c)
                add(object->PositionModelToWorld(Vector3(c & 1 ? shape->Max().X() : shape->Min().X(),
                                                         c & 2 ? shape->Max().Y() : shape->Min().Y(),
                                                         c & 4 ? shape->Max().Z() : shape->Min().Z())));
        // Ground contact comes from transformed bounds, not Abrams' high origin.
        const auto hull = Hull(body);
        const bool overlap = hull.size() >= 3
                                 ? Overlap(footprint, hull)
                                 : EditorCaveFootprintContains(i, object->Position().X(), object->Position().Z());
        return overlap && EditorCaveSupportOccupied(i, minY, maxTerrain, newFootprint);
    };
    if (check(GWorld->PlayerOn()))
        return true;
    return GWorld->FocusOn() && check(GWorld->FocusOn()->GetVehicle());
}
void RefreshHoles(const EditorCaveInfo& i)
{
    Landscape::TerrainHoleArea holes[32];
    GLandscape->GatherTerrainHoles(Vector3(i.x, i.y, i.z), i.length + i.width + 1, holes, 32);
    GLandscape->FlushCache();
}
void Remember(Object* object, EditorCaveInfo i)
{
    i.id = nextId++;
    i.objectId = object->ID();
    i.sourceName = object->GetShape()->Name();
    i.heading = Heading(i.heading);
    caves.push_back({object, i});
    selectedId = i.id;
    GLandscape->AddToIDCache(object);
}
} // namespace
CaveEditorSettings& CaveEditor()
{
    return settings;
}
const char* EditorCaveStatus()
{
    return status;
}
const char* ExcavationToolName()
{
    return settings.tool == ExcavationTool::Cave         ? "CAVE"
           : settings.tool == ExcavationTool::TankTrench ? "TANK TRENCH"
                                                         : "INFANTRY TRENCH";
}
void SetExcavationTool(ExcavationTool tool)
{
    settings.tool = tool;
    settings.floorOffset = 0;
    if (tool == ExcavationTool::Cave)
    {
        settings.width = 3;
        settings.height = 2.5f;
        settings.length = 16;
    }
    if (tool == ExcavationTool::InfantryTrench)
    {
        settings.width = 1.5f;
        settings.height = 1.6f;
        settings.length = 12;
    }
    if (tool == ExcavationTool::TankTrench)
    {
        settings.width = 5;
        settings.height = 2;
        settings.length = 24;
    }
}
std::size_t EditorCaveCount()
{
    return owner == GLandscape ? caves.size() : 0;
}

void ClearEditorCaves()
{
    if (owner && owner == GLandscape)
        for (auto& cave : caves)
            owner->RemoveObject(cave.object);
    caves.clear();
    selectedId = 0;
    owner = nullptr;
    settings.enabled = false;
    status = "Caves cleared. Terrain heights are unchanged.";
}
void ResizeEditorCave(float wheelSteps)
{
    if (!std::isfinite(wheelSteps))
        return;
    const float scale = std::pow(1.15f, std::clamp(wheelSteps, -64.0f, 64.0f));
    settings.width = std::clamp(settings.width * scale, 0.1f, 12.0f);
    if (settings.tool == ExcavationTool::Cave)
        settings.height = std::clamp(settings.height * scale, 0.1f, 8.0f);
}
bool CreateEditorCave(float x, float y, float z, float heading, float width, float height, float length)
{
    if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(heading) || !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(length) || width < 0.1f || width > 12 || height < 0.1f || height > 8 || length < 1 ||
        length > 64)
    {
        status = "Cave refused: single-player only; invalid position or dimensions.";
        return false;
    }
    if (owner && owner != GLandscape)
        ClearEditorCaves();
    if (caves.size() >= 8 || !nextId || GLandscape->GetLastObjectID() == std::numeric_limits<int>::max())
    {
        status = "Eight cave limit reached. Undo a cave before placing another.";
        return false;
    }
    EditorCaveInfo info;
    info.x = x;
    info.y = y;
    info.z = z;
    info.heading = heading;
    info.width = width;
    info.height = height;
    info.length = length;
    if (!ValidSite(info, nullptr))
        return false;
    const auto forward = Forward(heading);
    CaveMeshParams params;
    params.width = width;
    params.height = height;
    params.length = length;
    Ref<LODShapeWithShadow> shape = BuildCaveMesh(params);
    if (!shape)
    {
        status = "Cave mesh creation failed; terrain unchanged.";
        return false;
    }
    Matrix4 transform(MIdentity);
    transform.SetDirectionAndUp(forward, VUp);
    transform.SetPosition(Vector3(x, y, z));
    Object* object = NewObject(shape, GLandscape->NewObjectID());
    if (!object)
    {
        status = "Cave registration failed; terrain unchanged.";
        return false;
    }
    object->SetType(Primary);
    object->SetTransform(transform);
    GLandscape->AddObject(object);
    owner = GLandscape;
    Remember(object, info);
    // Register immediately: movement queries must see the opening before the
    // first terrain draw discovers the model.
    Landscape::TerrainHoleArea holes[32];
    GLandscape->GatherTerrainHoles(Vector3(x, y, z), length + width, holes, 32);
    GLandscape->FlushCache();
    status = "Cave placed. Mission-local; reload removes it. Undo removes the last cave.";
    return true;
}
bool CreateEditorTrench(float x, float y, float z, float heading, float width, float depth, float length, bool tank)
{
    if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(heading) || !std::isfinite(width) || !std::isfinite(depth) ||
        !std::isfinite(length) || width < 0.1f || width > 12 || depth < 0.1f || depth > 4 || length > 64 ||
        length < depth * (tank ? 6.0f : 3.0f) + (tank ? 6.0f : 1.0f))
    {
        status = "Trench refused: invalid dimensions; allow room for its ramp and flat floor.";
        return false;
    }
    if (owner && owner != GLandscape)
        ClearEditorCaves();
    if (caves.size() >= 8 || !nextId || GLandscape->GetLastObjectID() == std::numeric_limits<int>::max())
    {
        status = "Eight excavation limit reached. Undo one first.";
        return false;
    }
    EditorCaveInfo info;
    info.tool = tank ? ExcavationTool::TankTrench : ExcavationTool::InfantryTrench;
    info.x = x;
    info.y = y;
    info.z = z;
    info.heading = heading;
    info.width = width;
    info.height = depth;
    info.length = length;
    if (!ValidSite(info, nullptr))
        return false;
    const auto forward = Forward(heading);
    TrenchMeshParams params;
    params.width = width;
    params.depth = depth;
    params.length = length;
    params.rampLength = depth * (tank ? 6.0f : 3.0f);
    Ref<LODShapeWithShadow> shape = BuildTrenchMesh(params);
    if (!shape)
    {
        status = "Trench mesh creation failed.";
        return false;
    }
    Matrix4 transform(MIdentity);
    transform.SetDirectionAndUp(forward, VUp);
    transform.SetPosition(Vector3(x, y, z));
    Object* object = NewObject(shape, GLandscape->NewObjectID());
    if (!object)
    {
        status = "Trench registration failed.";
        return false;
    }
    object->SetType(Primary);
    object->SetTransform(transform);
    GLandscape->AddObject(object);
    owner = GLandscape;
    Remember(object, info);
    Landscape::TerrainHoleArea holes[32];
    GLandscape->GatherTerrainHoles(Vector3(x, y, z), length + width, holes, 32);
    GLandscape->FlushCache();
    status = tank ? "Tank trench placed, with access ramp and parking floor. Mission-local."
                  : "Straight infantry trench placed, with access ramp. Mission-local.";
    return true;
}
bool CreateEditorCaveAtPixel(float x, float y)
{
    if (!settings.enabled || !GScene || !GScene->GetCamera() || !GLandscape)
        return false;
    const Camera& camera = *GScene->GetCamera();
    const auto& projection = camera.Projection();
    if (std::abs(projection(0, 0)) < 1e-6f || std::abs(projection(1, 1)) < 1e-6f)
        return false;
    const float sx = (x - projection(0, 2)) / projection(0, 0), sy = (y - projection(1, 2)) / projection(1, 1);
    Vector3 direction =
        (camera.Direction() + camera.DirectionAside() * sx * camera.Left() + camera.DirectionUp() * sy * camera.Top())
            .Normalized();
    Vector3 hit;
    bool sea = false;
    const float rayLimit = std::min(camera.Far(), 5000.0f);
    GLandscape->IntersectWithGroundOrSea(&hit, sea, camera.Position(), direction, 0, rayLimit);
    if (sea || (hit - camera.Position()).SquareSize() > rayLimit * rayLimit)
    {
        status = "No land hit. Point at the hillside where the entrance should start.";
        return false;
    }
    direction[1] = 0;
    if (direction.SquareSize() < (settings.tool == ExcavationTool::Cave ? 0.1f : 1e-6f))
    {
        status = "Look horizontally into a hillside, rather than straight down.";
        return false;
    }
    direction.Normalize();
    // Start just outside the hillside, on its surface, for a walk-in floor.
    const Vector3 entrance = hit - direction * 0.5f;
    const float floor = GLandscape->SurfaceY(entrance.X(), entrance.Z()) + settings.floorOffset;
    const float heading = std::atan2(direction.X(), direction.Z()) * (180 / Pi);
    if (settings.tool != ExcavationTool::Cave)
        return CreateEditorTrench(hit.X(), GLandscape->SurfaceY(hit.X(), hit.Z()), hit.Z(), heading, settings.width,
                                  settings.height, settings.length, settings.tool == ExcavationTool::TankTrench);
    return CreateEditorCave(entrance.X(), floor, entrance.Z(), heading, settings.width, settings.height,
                            settings.length);
}
std::vector<EditorCaveInfo> EditorCaves()
{
    std::vector<EditorCaveInfo> out;
    if (owner == GLandscape)
        for (const auto& r : caves)
            out.push_back(Describe(r));
    return out;
}
std::uint32_t SelectedEditorCaveId()
{
    return owner == GLandscape ? selectedId : 0;
}
bool SelectEditorCave(std::uint32_t id)
{
    if (!Find(id))
    {
        status = "Select a live generated excavation in this mission.";
        return false;
    }
    selectedId = id;
    return true;
}
bool EditorCaveFootprintContains(const EditorCaveInfo& i, float x, float z)
{
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(i.heading) || !std::isfinite(i.width) ||
        !std::isfinite(i.length) || i.width <= 0 || i.length <= 0)
        return false;
    const auto p = Footprint(i);
    for (std::size_t k = 0; k < p.size(); ++k)
        if (Cross(p[k], p[(k + 1) % p.size()], {x, z}) < 0)
            return false;
    return true;
}
bool EditorCaveFootprintsOverlap(const EditorCaveInfo& a, const EditorCaveInfo& b)
{
    return Overlap(Footprint(a), Footprint(b));
}
bool EditorCaveSupportOccupied(const EditorCaveInfo& i, float minY, float rawTerrainY, bool newFootprint)
{
    if (!std::isfinite(minY) || !std::isfinite(rawTerrainY))
        return true;
    if (i.tool == ExcavationTool::Cave && minY > i.y + i.height + .1f)
        return false;
    return minY < rawTerrainY + (newFootprint ? .1f : -.1f);
}
bool RotateEditorCave(std::uint32_t id, float heading)
{
    auto* r = Find(id);
    if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware || !r || !std::isfinite(heading))
    {
        status = "Select a live generated excavation and finite heading.";
        return false;
    }
    if (Occupied(Describe(*r)))
    {
        status = "Leave the excavation with the player and controlled vehicle before rotating it.";
        return false;
    }
    const auto old = Describe(*r);
    auto candidate = old;
    candidate.heading = Heading(heading);
    if (Occupied(candidate, true))
    {
        status = "Move the player and controlled vehicle away from the proposed rotated excavation.";
        return false;
    }
    if (!ValidSite(candidate, r->object))
        return false; // rejected site has no mutation to undo
    Matrix4 transform = r->object->Transform();
    transform.SetDirectionAndUp(Forward(candidate.heading), VUp);
    transform.SetPosition(Vector3(old.x, old.y, old.z));
    GLandscape->MoveObject(r->object, transform);
    r->info.heading = candidate.heading;
    RefreshHoles(old);
    RefreshHoles(candidate);
    status = "Selected excavation rotated; entrance and identity preserved.";
    return true;
}
bool DeleteEditorCave(std::uint32_t id)
{
    auto* r = Find(id);
    if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware || !r)
    {
        status = "Select a live generated excavation to delete.";
        return false;
    }
    if (Occupied(Describe(*r)))
    {
        status = "Leave the excavation with the player and controlled vehicle before deleting it.";
        return false;
    }
    const auto old = Describe(*r);
    owner->RemoveObject(r->object);
    caves.erase(std::find_if(caves.begin(), caves.end(), [&](const Record& v) { return v.info.id == id; }));
    if (selectedId == id)
        selectedId = caves.empty() ? 0 : caves.back().info.id;
    RefreshHoles(old);
    status = "Selected generated excavation removed; original terrain restored.";
    return true;
}
bool RemoveLastEditorCave()
{
    if (owner != GLandscape || caves.empty())
    {
        status = "No generated excavation to undo.";
        return false;
    }
    return DeleteEditorCave(caves.back().info.id);
}
} // namespace Poseidon::Dev
