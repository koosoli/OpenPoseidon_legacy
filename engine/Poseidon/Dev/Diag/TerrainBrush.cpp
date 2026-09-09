#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/World.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Poseidon::Dev
{
namespace { TerrainBrushSettings settings; }
TerrainBrushSettings& TerrainBrush() { return settings; }
void ResetTerrainBrush() { settings = TerrainBrushSettings{}; }

bool PaintTerrain(float cx, float cz, float radius, float delta)
{
    if (!GLandscape || !GWorld || GWorld->GetMode() == GModeNetware ||
        !std::isfinite(cx) || !std::isfinite(cz) || !std::isfinite(radius) ||
        !std::isfinite(delta) || radius <= 0 || delta == 0 || std::abs(delta) > 16)
        return false;
    auto& land = *GLandscape;
    const int range = land.GetTerrainRange();
    const float spacing = land.GetTerrainGrid();
    if (range < 2 || range > 2049 || !(spacing > 0) || !std::isfinite(spacing) ||
        cx < 0 || cz < 0 || cx > (range-1)*spacing || cz > (range-1)*spacing)
        return false;
    radius = std::max(std::min(radius,250.0f),spacing*1.5f);
    const int left = std::max(0,static_cast<int>(std::floor((cx-radius)/spacing)));
    const int top = std::max(0,static_cast<int>(std::floor((cz-radius)/spacing)));
    const int right = std::min(range-1,static_cast<int>(std::ceil((cx+radius)/spacing)));
    const int bottom = std::min(range-1,static_cast<int>(std::ceil((cz+radius)/spacing)));
    const int width = right-left+1, height = bottom-top+1;
    if (width > 257 || height > 257) return false;
    std::vector<float> patch(static_cast<size_t>(width)*height);
    bool changed = false;
    for (int z=0; z<height; ++z)
        for (int x=0; x<width; ++x)
        {
            const float old = land.GetHeight(top+z,left+x);
            const float depth = TerrainCraterDepth((left+x)*spacing-cx,(top+z)*spacing-cz,radius,std::abs(delta));
            const float next = std::clamp(old+std::copysign(depth,delta),-10000.0f,10000.0f);
            patch[static_cast<size_t>(z)*width+x] = next;
            changed |= next != old;
        }
    if (!changed) return false;
    auto& world = Physics::EnsurePhysicsWorld();
    if (!world.Create()) return false;
    const auto stats = world.GetStats();
    if (!stats.terrainRegistered || stats.terrainWidth != range || stats.terrainHeight != range ||
        stats.terrainCellSize != spacing || stats.terrainOriginX != 0 || stats.terrainOriginZ != 0)
    {
        std::vector<float> all(static_cast<size_t>(range)*range);
        for (int z=0; z<range; ++z)
            for (int x=0; x<range; ++x)
                all[static_cast<size_t>(z)*range+x]=land.GetHeight(z,x);
        if (!world.SetTerrain(all.data(),range,range,spacing,0,0)) return false;
    }
    if (!world.PatchTerrain(left,top,width,height,patch.data())) return false;
    for (int z=0; z<height; ++z)
        for (int x=0; x<width; ++x)
        {
            const float next=patch[static_cast<size_t>(z)*width+x];
            if (next != land.GetHeight(top+z,left+x)) land.HeightChange(left+x,top+z,next);
        }
    land.FlushCache();
    return true;
}

bool PaintTerrainAtPixel(float x, float y, float seconds)
{
    if (!settings.enabled || !GScene || !GScene->GetCamera() || !GLandscape ||
        !std::isfinite(seconds) || seconds <= 0) return false;
    const Camera& camera = *GScene->GetCamera();
    const auto& projection = camera.Projection();
    if (std::abs(projection(0,0)) < 1e-6f || std::abs(projection(1,1)) < 1e-6f) return false;
    const float sx = (x-projection(0,2))/projection(0,0);
    const float sy = (y-projection(1,2))/projection(1,1);
    const Vector3 direction = (camera.Direction()+camera.DirectionAside()*sx*camera.Left()+
                              camera.DirectionUp()*sy*camera.Top()).Normalized();
    Vector3 hit;
    bool sea = false;
    if (GLandscape->IntersectWithGroundOrSea(&hit,sea,camera.Position(),direction,0,
                                            std::min(camera.Far(),50000.0f)) < 0 || sea)
        return false;
    const float delta = std::clamp(seconds,0.0f,0.25f)*std::clamp(settings.metresPerSecond,0.1f,32.0f);
    return PaintTerrain(hit.X(),hit.Z(),settings.radius,settings.raise ? delta : -delta);
}
}
