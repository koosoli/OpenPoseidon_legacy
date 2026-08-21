#include <Poseidon/World/Weather/RainVolume.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Poseidon
{
RainSystem GRain;

namespace
{

// Same blend family the smoke cloudlets use: alpha-blended, no depth write,
// fogged, tintable, sorted. Rain streaks must not z-write -- they are thin
// sprites crossing everything at high speed.
constexpr int kRainSpecials = ClampU | ClampV | NoZWrite | IsAlpha | IsAlphaFog | NoShadow | IsColored;

} // namespace

Vector3 RainTargetVelocity(float fallSpeed, float windX, float windZ, float windResponse)
{
    const float response = std::max(windResponse, 0.0f);
    return Vector3(windX * response, -std::max(fallSpeed, 0.0f), windZ * response);
}

Vector3 RainRelaxVelocity(Vector3Par current, Vector3Par target, float responsePerSecond, float deltaT)
{
    const float alpha = 1.0f - std::exp(-std::max(responsePerSecond, 0.0f) * std::max(deltaT, 0.0f));
    return current + (target - current) * alpha;
}

RainSystem::RainSystem() = default;

float RainSystem::RandUnit()
{
    // Private xorshift stream, same discipline as SmokeVolume: pulling from
    // GRandGen would make the drop count change every downstream random draw.
    _rand ^= _rand << 13;
    _rand ^= _rand >> 17;
    _rand ^= _rand << 5;
    return static_cast<float>(_rand & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

float RainSystem::RandSymmetric()
{
    return RandUnit() * 2.0f - 1.0f;
}

void RainSystem::RefreshOcclusion()
{
    _occluders.clear();
    _stats.nearbyBuildings = 0;
    if (_params.occlusion != RainOcclusionRooms || GLandscape == nullptr)
    {
        return;
    }
    if (GScene == nullptr || GScene->GetCamera() == nullptr)
    {
        return;
    }

    const Vector3 camPos = GScene->GetCamera()->Position();
    int xMin, xMax, zMin, zMax;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, camPos, camPos, _params.areaRadius);
    for (int x = xMin; x <= xMax; ++x)
    {
        for (int z = zMin; z <= zMax; ++z)
        {
            const ObjectList& objects = GLandscape->GetObjects(z, x);
            for (int i = 0; i < objects.Size(); ++i)
            {
                Building* building = dyn_cast<Building>(objects[i]);
                if (building == nullptr)
                {
                    continue;
                }
                bool duplicate = false;
                for (const Occluder& existing : _occluders)
                {
                    if (existing.object == building)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                {
                    continue;
                }
                const float radius = building->GetRadius();
                const Vector3 centre = building->Position();
                const float dx = centre.X() - camPos.X();
                const float dz = centre.Z() - camPos.Z();
                if (dx * dx + dz * dz > Square(_params.areaRadius + radius))
                {
                    continue;
                }
                BuildingInterior const* interior = BuildingInterior::GetFor(building->GetBType());
                if (interior == nullptr || !interior->Valid())
                {
                    continue;
                }
                Occluder occluder;
                occluder.object = building;
                occluder.interior = interior;
                occluder.inv = building->GetInvTransform();
                occluder.centre = centre;
                occluder.radius = radius;
                _occluders.push_back(occluder);
            }
        }
    }
    _stats.nearbyBuildings = static_cast<int>(_occluders.size());
}

void RainSystem::Spawn(float deltaT)
{
    if (GScene == nullptr || GScene->GetCamera() == nullptr)
    {
        return;
    }
    const Vector3 camPos = GScene->GetCamera()->Position();

    // Steady-state spawner: rate = target / mean lifetime, lifetime being
    // roughly the fall from spawn height. The cap is a hard stop so lowering
    // the target in the panel cannot build a spawn debt that fires as a burst.
    const float meanLifetime = std::max(_params.spawnHeight / std::max(_params.fallSpeed, 0.5f), 0.1f);
    const float weatherDensity = _params.densityOverride >= 0.0f
                                     ? std::clamp(_params.densityOverride, 0.0f, 1.0f)
                                     : (GLandscape != nullptr ? std::clamp(GLandscape->GetRainDensity(), 0.0f, 1.0f) : 0.0f);
    const float rate = static_cast<float>(_params.targetDrops) * weatherDensity / meanLifetime;
    _spawnAccumulator += rate * deltaT;
    while (_spawnAccumulator >= 1.0f)
    {
        _spawnAccumulator -= 1.0f;
        if (static_cast<int>(_drops.size()) >= _params.maxDrops)
        {
            _spawnAccumulator = 0.0f;
            break;
        }

        const float r = _params.areaRadius * std::sqrt(RandUnit());
        const float theta = RandUnit() * 6.2831853f;
        Drop drop;
        drop.position = camPos + Vector3(r * std::cos(theta), _params.spawnHeight * (0.35f + 0.65f * RandUnit()),
                                         r * std::sin(theta));
        drop.speed = _params.fallSpeed * (1.0f + _params.speedJitter * RandSymmetric());
        if (GWind.IsActive())
        {
            const WindSample& wind = GWind.Sample();
            drop.velocityX = wind.velocityX * _params.windResponse;
            drop.velocityZ = wind.velocityZ * _params.windResponse;
        }
        drop.occlusionTimer = RandUnit() * _params.occlusionCadence;
        drop.indoor = false;
        _drops.push_back(drop);
        _stats.spawned++;
    }
}

void RainSystem::Simulate(float deltaT)
{
    const ISmokeWorldQuery& query = GSmokeWorldQuery();
    _stats.occlusionTests = 0;

    float windX = 0.0f, windZ = 0.0f;
    if (GWind.IsActive())
    {
        const WindSample& wind = GWind.Sample();
        windX = wind.velocityX;
        windZ = wind.velocityZ;
    }

    for (std::size_t i = 0; i < _drops.size();)
    {
        Drop& drop = _drops[i];

        const Vector3 currentVelocity(drop.velocityX, -drop.speed, drop.velocityZ);
        const Vector3 targetVelocity = RainTargetVelocity(drop.speed, windX, windZ, _params.windResponse);
        const Vector3 velocity = RainRelaxVelocity(currentVelocity, targetVelocity, _params.windAcceleration, deltaT);
        drop.velocityX = velocity.X();
        drop.velocityZ = velocity.Z();
        const Vector3 nextPosition = drop.position + velocity * deltaT;

        // -- occlusion ----------------------------------------------------
        if (_params.occlusion == RainOcclusionRooms)
        {
            bool killed = false;
            const Vector3 midpoint = (drop.position + nextPosition) * 0.5f;
            const float halfStep = drop.position.Distance(nextPosition) * 0.5f;
            for (const Occluder& occluder : _occluders)
            {
                const float dx = midpoint.X() - occluder.centre.X();
                const float dz = midpoint.Z() - occluder.centre.Z();
                if (dx * dx + dz * dz > Square(occluder.radius + halfStep))
                {
                    continue;
                }
                ++_stats.occlusionTests;
                const int label = occluder.interior->ClassifySegmentModel(
                    occluder.inv.FastTransform(drop.position), occluder.inv.FastTransform(nextPosition));
                if (label >= 0)
                {
                    _stats.indoorKills++;
                    killed = true;
                }
                else if (label == -2)
                {
                    _stats.roofKills++;
                    killed = true;
                }
                if (killed)
                {
                    break;
                }
            }
            if (killed)
            {
                _drops[i] = _drops.back();
                _drops.pop_back();
                continue;
            }
        }
        else if (_params.occlusion == RainOcclusionSheltered)
        {
            // Cheap fallback for buildings with no Paths data. Staggered
            // cadence; coarser than Rooms and it cannot stop drops at the
            // roofline, only under it.
            drop.occlusionTimer -= deltaT;
            if (drop.occlusionTimer <= 0.0f)
            {
                drop.occlusionTimer = _params.occlusionCadence;
                if (query.IsSheltered(nextPosition, 8.0f))
                {
                    _stats.indoorKills++;
                    _drops[i] = _drops.back();
                    _drops.pop_back();
                    continue;
                }
            }
        }

        // -- fall ---------------------------------------------------------
        drop.position = nextPosition;

        const float ground = query.GroundHeight(drop.position.X(), drop.position.Z());
        if (drop.position.Y() <= ground)
        {
            if (_params.splashes && !drop.indoor && static_cast<int>(_splashes.size()) < 256)
            {
                Splash splash;
                splash.position = Vector3(drop.position.X(), ground + 0.05f, drop.position.Z());
                splash.age = 0.0f;
                _splashes.push_back(splash);
            }
            if (!drop.indoor)
            {
                _stats.groundHits++;
            }
            // swap-remove; index not advanced
            _drops[i] = _drops.back();
            _drops.pop_back();
            continue;
        }
        ++i;
    }

    for (std::size_t i = 0; i < _splashes.size();)
    {
        _splashes[i].age += deltaT;
        if (_splashes[i].age >= _params.splashLifetime)
        {
            _splashes[i] = _splashes.back();
            _splashes.pop_back();
            continue;
        }
        ++i;
    }
}

void RainSystem::Draw()
{
    _stats.drawnDrops = 0;
    _stats.streakDecals = 0;
    if (GScene == nullptr || GEngine == nullptr || GScene->GetCamera() == nullptr)
    {
        return;
    }
    Texture* texture = GScene->Preloaded(TextureRain);
    if (texture == nullptr)
    {
        return;
    }
    MipInfo mip = GEngine->TextBank()->UseMipmap(texture, 0, 0);
    if (!mip.IsOK())
    {
        return;
    }

    const Camera& camera = *GScene->GetCamera();
    Matrix4Val project = camera.Projection();
    Matrix4Val toView = GScene->ScaledInvTransform();
    const float nearest = camera.Near() * 10.0f;
    const float invLeft = camera.InvLeft();
    const float invTop = camera.InvTop();
    const Color tint(_params.red, _params.green, _params.blue, 1.0f);

    // Drops: a short chain of overlapping sprites along the true velocity.
    // DrawDecal itself is axis-aligned, which made the old streaks look vertical
    // even while their centres moved downwind. Three beads make direction and
    // wind speed legible without adding a rotated-decal API to every backend.
    // Calm rain retains the original single submission; only visibly slanted
    // drops pay for a second bead.
    for (const Drop& drop : _drops)
    {
        if (drop.indoor)
        {
            continue; // the point of the feature: nothing falls indoors
        }
        const float distance2 = (drop.position - camera.Position()).SquareSize();
        const float fog = GScene->Fog8(distance2) * (1.0f / 255.0f);
        Color colour = tint;
        colour.SetA(_params.opacity * (1.0f - fog));
        const Vector3 velocity(drop.velocityX, -drop.speed, drop.velocityZ);
        const float speed = std::max(velocity.Size(), 0.001f);
        const float horizontalSpeed = std::sqrt(Square(drop.velocityX) + Square(drop.velocityZ));
        const int trailBeads = horizontalSpeed > 0.25f ? 2 : 1;
        const Vector3 trailStep = trailBeads > 1 ? velocity * (-_params.streakLength / speed) : VZero;
        bool submitted = false;
        for (int bead = 0; bead < trailBeads; ++bead)
        {
            const Vector3 beadPosition = drop.position + trailStep * static_cast<float>(bead);
            Vector3 posp = toView * beadPosition;
            if (posp.Z() < nearest)
            {
                continue;
            }
            const float invW = 1.0f / posp[2];
            posp[0] = project(0, 2) + project(0, 0) * posp[0] * invW;
            posp[1] = project(1, 2) + project(1, 1) * posp[1] * invW;
            const float beadLength = trailBeads > 1 ? _params.streakLength * 0.65f : _params.streakLength;
            const float nearPos = floatMax(posp[2] - std::min(beadLength * 0.10f, 0.10f), nearest);
            const float invNearPos = 1.0f / nearPos;
            posp[2] = project(2, 2) + project.Position()[2] * invNearPos;
            const float sizeX = +project(0, 0) * _params.streakWidth * invW * invLeft * 0.5f;
            const float sizeY = -project(1, 1) * beadLength * invW * invTop * 0.5f;
            if (sizeX * sizeY < 0.5f)
            {
                continue;
            }
            GEngine->DrawDecal(posp, invNearPos, sizeX, sizeY, PackedColor(colour), mip, kRainSpecials);
            ++_stats.streakDecals;
            submitted = true;
        }
        if (submitted)
        {
            ++_stats.drawnDrops;
        }
    }

    // Splashes: brief expanding faint discs at the ground.
    for (const Splash& splash : _splashes)
    {
        const float t = splash.age / std::max(_params.splashLifetime, 1e-3f);
        const float size = 0.1f + _params.splashSize * t;
        Vector3 posp = toView * splash.position;
        if (posp.Z() < nearest)
        {
            continue;
        }
        const float invW = 1.0f / posp[2];
        posp[0] = project(0, 2) + project(0, 0) * posp[0] * invW;
        posp[1] = project(1, 2) + project(1, 1) * posp[1] * invW;
        const float nearPos = floatMax(posp[2] - 0.05f, nearest);
        const float invNearPos = 1.0f / nearPos;
        posp[2] = project(2, 2) + project.Position()[2] * invNearPos;

        const float sizeX = +project(0, 0) * size * invW * invLeft * 0.5f;
        const float sizeY = -project(1, 1) * size * invW * invTop * 0.5f;
        if (sizeX * sizeY < 0.5f)
        {
            continue;
        }
        const float distance2 = (splash.position - camera.Position()).SquareSize();
        const float fog = GScene->Fog8(distance2) * (1.0f / 255.0f);
        Color colour = tint;
        // Fading fast and never opaque: a splash is a highlight on the ground,
        // not an object. The rain texture stretched round reads as a ripple.
        colour.SetA(_params.opacity * 0.5f * (1.0f - t) * (1.0f - fog));
        GEngine->DrawDecal(posp, invNearPos, sizeX, sizeY, PackedColor(colour), mip, kRainSpecials);
    }
}

void RainSystem::UpdateAndDraw()
{
    if (_mode != RainParticle && _mode != RainBoth)
    {
        return;
    }
    if (GScene == nullptr || GLandscape == nullptr)
    {
        return;
    }

    const auto begin = std::chrono::steady_clock::now();

    float deltaT = Glob.time - _lastUpdate;
    _lastUpdate = Glob.time;
    // Same hitch rule as smoke: a long frame integrated in one step teleports
    // every drop through the ground; clamp rather than substep.
    deltaT = std::clamp(deltaT, 0.0f, 0.10f);
    if (deltaT <= 0.0f)
    {
        return;
    }

    auto phase = std::chrono::steady_clock::now();
    Spawn(deltaT);
    auto now = std::chrono::steady_clock::now();
    _stats.spawnMicroseconds = std::chrono::duration<double, std::micro>(now - phase).count();
    phase = now;
    RefreshOcclusion();
    now = std::chrono::steady_clock::now();
    _stats.occlusionMicroseconds = std::chrono::duration<double, std::micro>(now - phase).count();
    phase = now;
    Simulate(deltaT);
    now = std::chrono::steady_clock::now();
    _stats.simulateMicroseconds = std::chrono::duration<double, std::micro>(now - phase).count();
    phase = now;
    Draw();
    now = std::chrono::steady_clock::now();
    _stats.drawMicroseconds = std::chrono::duration<double, std::micro>(now - phase).count();

    _stats.liveDrops = static_cast<int>(_drops.size());
    _stats.liveSplashes = static_cast<int>(_splashes.size());
    _stats.lastUpdateUs = std::chrono::duration<double, std::micro>(now - begin).count();
}

} // namespace Poseidon
