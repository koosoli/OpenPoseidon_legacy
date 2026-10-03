#include <Poseidon/Dev/Diag/CaveEditor.hpp>
#include <Poseidon/Dev/Diag/CaveMesh.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Poseidon::Dev
{
namespace
{
CaveEditorSettings settings;
Landscape* owner = nullptr;
std::vector<Ref<Object>> caves;
const char* status = "Choose a hillside and point into the mountain.";
constexpr float Pi = 3.14159265359f;
}
CaveEditorSettings& CaveEditor() { return settings; }
const char* EditorCaveStatus() { return status; }
std::size_t EditorCaveCount() { return owner == GLandscape ? caves.size() : 0; }

void ClearEditorCaves()
{
    if (owner && owner == GLandscape)
        for (auto& cave : caves) owner->RemoveObject(cave);
    caves.clear();
    owner = nullptr;
    settings.enabled = false;
    status = "Caves cleared. Terrain heights are unchanged.";
}
void ResizeEditorCave(float wheelSteps)
{
    if (!std::isfinite(wheelSteps)) return;
    const float scale = std::pow(1.15f, std::clamp(wheelSteps, -64.0f, 64.0f));
    settings.width = std::clamp(settings.width * scale, 0.1f, 12.0f);
    settings.height = std::clamp(settings.height * scale, 0.1f, 8.0f);
}
bool CreateEditorCave(float x, float y, float z, float heading, float width, float height, float length)
{
    if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(heading) ||
        !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(length) ||
        width < 0.1f || width > 12 || height < 0.1f || height > 8 || length < 1 || length > 64)
    {
        status = "Cave refused: single-player only; invalid position or dimensions.";
        return false;
    }
    if (owner && owner != GLandscape) ClearEditorCaves();
    if (caves.size() >= 8)
    {
        status = "Eight cave limit reached. Undo a cave before placing another.";
        return false;
    }
    const float extent = (GLandscape->GetTerrainRange()-1)*GLandscape->GetTerrainGrid();
    const float a = heading * (Pi/180);
    const Vector3 forward(std::sin(a), 0, std::cos(a));
    const Vector3 aside(std::cos(a), 0, -std::sin(a));
    // Validate the entire shell, not just its entrance: never index outside the
    // landscape, cut underwater terrain, or open a tunnel into the sky.
    for (int end = 0; end < 2; ++end)
        for (int side = -1; side <= 1; side += 2)
        {
            const Vector3 p = Vector3(x,y,z) + forward*(end*length) + aside*(side*(width*0.5f+0.4f));
            if (p.X() < 1 || p.Z() < 1 || p.X() >= extent-1 || p.Z() >= extent-1 || y <= GLandscape->GetSeaLevel()+0.1f)
            {
                status = "Cave refused: outside land bounds or below water.";
                return false;
            }
        }
    const Vector3 back = Vector3(x,y,z) + forward*length;
    if (GLandscape->SurfaceY(back.X(),back.Z()) < y+height+0.4f)
    {
        status = "Point farther into rising ground: the back needs terrain above its ceiling.";
        return false;
    }
    for (int sample=1; sample<=16; ++sample)
    {
        const Vector3 p=Vector3(x,y,z)+forward*(length*sample/16.0f);
        if (GLandscape->SurfaceY(p.X(),p.Z()) < y-0.2f || GLandscape->InTerrainHole(p.X(),p.Z()))
        {
            status = "Cave refused: ground drops below the floor or overlaps another cave.";
            return false;
        }
    }
    CaveMeshParams params;
    params.width=width; params.height=height; params.length=length;
    Ref<LODShapeWithShadow> shape=BuildCaveMesh(params);
    if (!shape)
    {
        status = "Cave mesh creation failed; terrain unchanged.";
        return false;
    }
    Matrix4 transform(MIdentity);
    transform.SetDirectionAndUp(forward,VUp);
    transform.SetPosition(Vector3(x,y,z));
    Object* object=NewObject(shape,-1);
    if (!object)
    {
        status = "Cave registration failed; terrain unchanged.";
        return false;
    }
    object->SetType(Primary);
    object->SetTransform(transform);
    GLandscape->AddObject(object);
    owner=GLandscape;
    caves.emplace_back(object);
    // Register immediately: movement queries must see the opening before the
    // first terrain draw discovers the model.
    Landscape::TerrainHoleArea holes[32];
    GLandscape->GatherTerrainHoles(Vector3(x,y,z),length+width,holes,32);
    GLandscape->FlushCache();
    status = "Cave placed. Mission-local; reload removes it. Undo removes the last cave.";
    return true;
}
bool CreateEditorCaveAtPixel(float x,float y)
{
    if (!settings.enabled || !GScene || !GScene->GetCamera() || !GLandscape) return false;
    const Camera& camera=*GScene->GetCamera();
    const auto& projection=camera.Projection();
    if (std::abs(projection(0,0))<1e-6f || std::abs(projection(1,1))<1e-6f) return false;
    const float sx=(x-projection(0,2))/projection(0,0), sy=(y-projection(1,2))/projection(1,1);
    Vector3 direction=(camera.Direction()+camera.DirectionAside()*sx*camera.Left()+
                       camera.DirectionUp()*sy*camera.Top()).Normalized();
    Vector3 hit;
    bool sea=false;
    if (GLandscape->IntersectWithGroundOrSea(&hit,sea,camera.Position(),direction,0,std::min(camera.Far(),5000.0f))<0 || sea)
    {
        status = "No land hit. Point at the hillside where the entrance should start.";
        return false;
    }
    direction[1]=0;
    if (direction.SquareSize()<0.1f)
    {
        status = "Look horizontally into a hillside, rather than straight down.";
        return false;
    }
    direction.Normalize();
    // Start just outside the hillside, on its surface, for a walk-in floor.
    const Vector3 entrance=hit-direction*0.5f;
    const float floor=GLandscape->SurfaceY(entrance.X(),entrance.Z())+settings.floorOffset;
    return CreateEditorCave(entrance.X(),floor,entrance.Z(),std::atan2(direction.X(),direction.Z())*(180/Pi),
                            settings.width,settings.height,settings.length);
}
bool RemoveLastEditorCave()
{
    if (!GWorld || GWorld->GetMode()==GModeNetware || owner!=GLandscape || caves.empty()) return false;
    // Refuse to close a hole on the owner while they are walking inside it.
    if (GWorld->PlayerOn() && GLandscape->InTerrainHoleBelow(GWorld->PlayerOn()->Position(),0.1f))
    {
        status = "Leave the cave before undoing it.";
        return false;
    }
    owner->RemoveObject(caves.back());
    caves.pop_back();
    owner->FlushCache();
    status = "Last cave removed; original terrain restored.";
    return true;
}
}
