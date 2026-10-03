#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <Poseidon/World/Terrain/TerrainEditJournal.hpp>
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
namespace
{
    TerrainEditJournal terrainBaseline;
    Landscape* baselineOwner = nullptr;
    int baselineRange = 0;
    float baselineSpacing = 0;
}
void ClearTerrainEditBaseline()
{
    terrainBaseline.Clear();
    baselineOwner = nullptr;
    baselineRange = 0;
    baselineSpacing = 0;
}
bool HasTerrainEditBaseline()
{
    return GLandscape && baselineOwner == GLandscape && !terrainBaseline.Empty() &&
        baselineRange == GLandscape->GetTerrainRange() && baselineSpacing == GLandscape->GetTerrainGrid();
}
bool CaptureTerrainEditBaseline(int x, int z, int width, int height)
{
    if (!GLandscape) return false;
    // Never apply an old grid's sample indices to a newly subdivided landscape.
    if (!terrainBaseline.Empty() && !HasTerrainEditBaseline()) return false;
    const int range = GLandscape->GetTerrainRange();
    if (range < 2 || range > 2049) return false;
    if (!terrainBaseline.Remember(range,x,z,width,height,
            [](int ix,int iz) { return GLandscape->GetHeight(iz,ix); })) return false;
    baselineOwner = GLandscape;
    baselineRange = range;
    baselineSpacing = GLandscape->GetTerrainGrid();
    return true;
}
bool RestoreEditedTerrain()
{
    if (!HasTerrainEditBaseline() || !GWorld || GWorld->GetMode() == GModeNetware) return false;
    auto& world = Physics::EnsurePhysicsWorld();
    // Collision replacement remains whole-grid until its regional publication
    // gate is implemented. The persistent undo state itself is now sparse.
    std::vector<float> restored(static_cast<size_t>(baselineRange)*baselineRange);
    for (int z=0; z<baselineRange; ++z)
        for (int x=0; x<baselineRange; ++x)
            restored[static_cast<size_t>(z)*baselineRange+x] = GLandscape->GetHeight(z,x);
    terrainBaseline.ForEach([&](int x,int z,float before) {
        restored[static_cast<size_t>(z)*baselineRange+x] = before;
    });
    // SetTerrain publishes transactionally. Keep both the visible edit and its backup on failure.
    if (!world.Create() || !world.SetTerrain(restored.data(),baselineRange,baselineRange,
                                           baselineSpacing,0,0)) return false;
    terrainBaseline.ForEach([](int x,int z,float before) {
        if (before != GLandscape->GetHeight(z,x)) GLandscape->HeightChange(x,z,before);
    });
    GLandscape->FlushCache();
    ForgetShowcaseTerrainCrater();
    ClearTerrainEditBaseline();
    return true;
}
namespace { RocketCraterSettings rocketSettings; }
RocketCraterSettings& RocketCraters() { return rocketSettings; }
void ResetRocketCraters() { rocketSettings = RocketCraterSettings{}; }
bool RocketTerrainImpact(float x, float y, float z, float indirectHit)
{
    if (!rocketSettings.enabled || !GLandscape || !GWorld || GWorld->GetMode() == GModeNetware ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const float ground = GLandscape->SurfaceY(x,z);
    if (!RocketCraterGroundContact(y,ground,GLandscape->GetSeaLevel())) return false;
    const float scale = BallisticCraterScale(indirectHit);
    if (scale <= 0) return false;
    return PaintTerrain(x,z,std::clamp(rocketSettings.radius*scale,0.5f,100.0f),
                        -std::clamp(rocketSettings.depth*scale,0.025f,8.0f));
}
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
    radius = TerrainBrushRadius(radius, spacing);
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
    if (!CaptureTerrainEditBaseline(left,top,width,height)) return false;
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

bool TerrainBrushTargetAtPixel(float x, float y, TerrainBrushTarget& target)
{
    target = {};
    if (!settings.enabled || !GScene || !GScene->GetCamera() || !GLandscape ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(settings.radius) || settings.radius <= 0)
        return false;
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
    const int range = GLandscape->GetTerrainRange();
    const float spacing = GLandscape->GetTerrainGrid();
    if (!(spacing > 0) || !std::isfinite(spacing)) return false;
    target.x = hit.X(); target.y = hit.Y(); target.z = hit.Z();
    target.radius = TerrainBrushRadius(settings.radius, spacing);
    target.editable = GWorld && GWorld->GetMode() != GModeNetware && range >= 2 && range <= 2049 &&
        hit.X() >= 0 && hit.Z() >= 0 && hit.X() <= (range-1)*spacing && hit.Z() <= (range-1)*spacing;
    if (target.editable)
    {
        const int left = std::max(0,static_cast<int>(std::floor((target.x-target.radius)/spacing)));
        const int top = std::max(0,static_cast<int>(std::floor((target.z-target.radius)/spacing)));
        const int right = std::min(range-1,static_cast<int>(std::ceil((target.x+target.radius)/spacing)));
        const int bottom = std::min(range-1,static_cast<int>(std::ceil((target.z+target.radius)/spacing)));
        target.editable = right-left+1 <= 257 && bottom-top+1 <= 257;
    }
    return true;
}

bool PaintTerrainAtPixel(float x, float y, float seconds, bool invert, bool fast)
{
    if (!std::isfinite(seconds) || seconds <= 0) return false;
    TerrainBrushTarget target;
    if (!TerrainBrushTargetAtPixel(x,y,target) || !target.editable) return false;
    const float delta = std::min(16.0f, std::clamp(seconds,0.0f,0.25f)*std::clamp(settings.metresPerSecond,0.1f,32.0f)*(fast ? 4.0f : 1.0f));
    return PaintTerrain(target.x,target.z,target.radius,(settings.raise != invert) ? delta : -delta);
}
}
