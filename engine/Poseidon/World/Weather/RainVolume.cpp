#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Weather/RainParticlePolicy.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/SnowShelter.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

namespace Poseidon
{
RainSystem GRain;
RainSystem GSnowFlakes;

void MaskSnowShelters(const Landscape& land, std::vector<float>& data)
{
    constexpr int n = SnowField::WindowSize;
    constexpr int step = 4; // 0.5m, matching the cached building shell voxels.
    if (data.size() != 4 + n*n) return;
    // Sheltered ground holds no snow at all: clear to the deepest base present,
    // snowfall deposit or altitude snowline cover (both read 0 through it).
    const float clearTo = std::max(data[3], GSnow().snowlineEnabled ? GSnow().snowlineDepth : 0.0f);
    if (clearTo <= 0.0f) return;
    static const bool disabled = [] {
        const char* value = std::getenv("POSEIDON_SNOW_SHELTER");
        return value && value[0] == '0';
    }();
    if (disabled) return;
    const auto begin = std::chrono::steady_clock::now();
    size_t probes = 0, masked = 0;
    const float span = n * data[2];
    const Vector3 centre(data[0] + span*0.5f, 0, data[1] + span*0.5f);
    std::vector<Building*> buildings;
    auto add = [&](Object* object)
    {
        Building* building = dyn_cast<Building>(object);
        if (!building || std::find(buildings.begin(), buildings.end(), building) != buildings.end()) return;
        const auto p = building->Position();
        const float radius = building->GetRadius();
        if (std::abs(p.X()-centre.X()) > span*0.5f+radius ||
            std::abs(p.Z()-centre.Z()) > span*0.5f+radius) return;
        buildings.push_back(building);
    };
    int xMin, xMax, zMin, zMax;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, centre, centre, span);
    for (int x = xMin; x <= xMax; ++x)
        for (int z = zMin; z <= zMax; ++z)
        {
            const auto& objects = land.GetObjects(z, x);
            for (int i = 0; i < objects.Size(); ++i) add(objects[i]);
        }
    if (GWorld)
    {
        for (int i = 0; i < GWorld->NVehicles(); ++i) add(GWorld->GetVehicle(i));
        for (int i = 0; i < GWorld->NFastVehicles(); ++i) add(GWorld->GetFastVehicle(i));
        for (int i = 0; i < GWorld->NOutVehicles(); ++i) add(GWorld->GetOutVehicle(i));
    }
    for (const Building* building : buildings)
    {
        const auto* interior = BuildingInterior::GetFor(building->GetBType());
        if (!interior || !interior->Valid()) continue;
        const auto p = building->Position();
        const float radius = building->GetRadius();
        const auto inv = building->GetInvTransform();
        const int x0 = std::max(0, static_cast<int>(std::floor((p.X()-radius-data[0]) / (data[2]*step))) * step);
        const int z0 = std::max(0, static_cast<int>(std::floor((p.Z()-radius-data[1]) / (data[2]*step))) * step);
        const int x1 = std::min(n, static_cast<int>(std::ceil((p.X()+radius-data[0]) / data[2])));
        const int z1 = std::min(n, static_cast<int>(std::ceil((p.Z()+radius-data[1]) / data[2])));
        for (int z = z0; z < z1; z += step)
            for (int x = x0; x < x1; x += step)
            {
                const float wx = data[0] + (x+step*0.5f)*data[2];
                const float wz = data[1] + (z+step*0.5f)*data[2];
                const Vector3 point(wx, land.SurfaceY(wx, wz)+0.05f, wz);
                ++probes;
                bool covered = interior->CoveredFromAboveModel(inv * point);
                if (building->DirectionUp().Y() < 0.999f)
                    covered = interior->ClassifySegmentModel(inv*point, inv*(point+Vector3(0, radius*2, 0))) != -1;
                if (!covered) continue;
                for (int dz = 0; dz < step && z+dz < n; ++dz)
                    for (int dx = 0; dx < step && x+dx < n; ++dx)
                    {
                        data[4+(z+dz)*n+x+dx] = clearTo;
                        ++masked;
                    }
            }
    }
    static const bool trace = std::getenv("POSEIDON_SNOW_TRACE") != nullptr;
    static float nextTrace = 0;
    if (trace && Glob.time.toFloat() >= nextTrace)
    {
        nextTrace = Glob.time.toFloat() + 2.0f;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-begin).count();
        LOG_INFO(World, "Snow shelter: buildings={} probes={} masked={} time={:.0f}us", buildings.size(), probes, masked, us);
    }
}

namespace
{

// Same blend family the smoke cloudlets use: alpha-blended, no depth write,
// fogged, tintable, sorted. Rain streaks must not z-write -- they are thin
// sprites crossing everything at high speed.
constexpr int kRainSpecials = ClampU | ClampV | NoZWrite | IsAlpha | IsAlphaFog | NoShadow | IsColored;

// Same env-hook discipline as POSEIDON_TEST_SMOKE: a self-run smoke must be
// able to turn particle rain on over a clear day and get a log trail, without
// anyone opening the dev panel.
bool RainTestHook()
{
    const char* value = std::getenv("POSEIDON_TEST_RAIN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

// Which layers the hook turns on. `1` stays RainBoth so every existing capture
// recipe keeps meaning what it meant.
//
// `particle` is what an INDOOR check needs, and the distinction is not cosmetic.
// The legacy overlay is a full-screen scroll with no notion of shelter: under
// RainBoth it draws streaks over an interior wall no matter what the volumetric
// layer decided, so a capture taken inside a building cannot tell "rain comes
// through the roof" from "the A/B layer is on". Only the shipping default,
// RainParticle, answers that question, and before this the hook could not select
// it.
RainMode RainTestMode()
{
    const char* value = std::getenv("POSEIDON_TEST_RAIN");
    if (value == nullptr)
        return RainBoth;
    const std::string text(value);
    if (text == "particle")
        return RainParticle;
    if (text == "legacy")
        return RainLegacy;
    return RainBoth;
}

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

RainSystem::RainSystem()
{
    if (RainTestHook())
    {
        // Forced-max density over whichever layers were asked for: RainBoth by
        // default, which is the A/B pair a rain smoke wants, independent of the
        // mission's weather state. The weather rain density itself is driven up
        // as well, because consumers beyond the two render layers -- the ocean's
        // rain-ripple term, wet-surface effects -- read GetRainDensity, not GRain.
        _mode = RainTestMode();
        _params.densityOverride = 1.0f;
        _legacyDensityOverride = 1.0f;
        _forceWeatherRain = true;
    }
}

Vector3 RainProbeTarget(Vector3Par position, Vector3Par velocity, float interval, float lookahead)
{
    return position + velocity * (std::max(interval, 0.0f) * std::max(lookahead, 1.0f));
}

float PrecipitationSpriteLength(bool snowflake, float projectedMotion, float projectedWidth)
{
    // Snow is a billboard, not a velocity streak: looking along its motion must
    // not collapse it to zero area (and discard every flake when looking down).
    return snowflake ? projectedWidth : projectedMotion;
}

float SnowflakeAlpha(float x, float y)
{
    const float r = std::sqrt(x*x + y*y);
    if (r >= 1.0f) return 0;
    // Six feathered arms, not a shaded disc. Small flakes resolve to sparse
    // coverage; close flakes retain gaps instead of looking like snowballs.
    const float angle = std::atan2(y, x);
    const float arm = std::abs(std::sin(angle * 3.0f)) * r;
    const float branch = std::abs(arm - std::max(0.0f, 0.22f - std::abs(r-0.52f)));
    const float shape = std::max(std::clamp((0.095f-arm)/0.07f, 0.0f, 1.0f),
                                 std::clamp((0.055f-branch)/0.045f, 0.0f, 1.0f));
    return shape * std::clamp((1.0f-r)*5.0f, 0.0f, 1.0f);
}

bool SnowParticleInViewVolume(Vector3Par particle, Vector3Par camera, float radius, float height)
{
    const Vector3 offset = particle-camera;
    return offset.X()*offset.X()+offset.Z()*offset.Z() <= radius*radius && std::abs(offset.Y()) <= height;
}

float RainSystem::EffectiveDensity() const
{
    if (_mode != RainParticle && _mode != RainBoth)
    {
        return 0.0f;
    }
    if (_params.densityOverride >= 0.0f)
    {
        return std::clamp(_params.densityOverride, 0.0f, 1.0f);
    }
    return GLandscape != nullptr ? std::clamp(GLandscape->GetRainDensity(), 0.0f, 1.0f) : 0.0f;
}

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
    const float rate = static_cast<float>(_params.targetDrops) * EffectiveDensity() / meanLifetime;
    const ISmokeWorldQuery& query = GSmokeWorldQuery();
    const bool probeSpawn = _params.probeAtSpawn && _params.occlusion != RainOcclusionNone;
    if (_params.snowflakes)
    {
        // Camera-following population, but retained flakes remain world-space.
        // No camera velocity is added to them and a teleport does not leave the
        // pool full of invisible particles on the other side of the island.
        const auto culled = std::erase_if(_drops, [&](const Drop& drop) {
            return !SnowParticleInViewVolume(drop.position, camPos, _params.areaRadius, _params.spawnHeight);
        });
        const int target = std::clamp(static_cast<int>(_params.targetDrops * EffectiveDensity()), 0, _params.maxDrops);
        if (static_cast<int>(_drops.size()) > target) _drops.resize(target);
        const int missing = target - static_cast<int>(_drops.size());
        if (!_snowVolumeReady || !SnowParticleInViewVolume(camPos, _snowVolumeCentre,
                _params.areaRadius*0.5f, _params.spawnHeight*0.5f))
        {
            _spawnAccumulator = static_cast<float>(missing);
            _snowVolumeReady = true;
            _snowVolumeCentre = camPos;
        }
        else
        {
            // Failed shelter spawns must NOT be retried as a full pool every
            // frame indoors. Only camera losses/density increases fill at once.
            _spawnAccumulator += rate*deltaT + static_cast<float>(culled) + std::max(0, target-_snowTarget);
            _spawnAccumulator = std::min(_spawnAccumulator, static_cast<float>(missing));
        }
        _snowTarget = target;
    }
    else
    {
        // At aircraft height the terrain can be hundreds of metres below the
        // camera. Keeping every streak until that distant ground hit filled
        // maxDrops with invisible particles and kept probing their roofs.
        // Retire camera-exited visuals before any spawn or per-drop world query.
        _stats.volumeCulls += static_cast<int>(std::erase_if(_drops, [&](const Drop& drop) {
            return !RainParticleInViewVolume(
                {drop.position.X(), drop.position.Y(), drop.position.Z()},
                {camPos.X(), camPos.Y(), camPos.Z()}, _params.areaRadius, _params.spawnHeight);
        }));
        const int target = RainParticlePopulationLimit(_params.targetDrops, _params.maxDrops, EffectiveDensity());
        _stats.populationLimit = target;
        if (static_cast<int>(_drops.size()) > target)
        {
            _stats.populationCulls += static_cast<int>(_drops.size()) - target;
            _drops.resize(target);
        }
        // Do not store spawn debt while the target is full or weather dries.
        // Retained drops still run the unchanged birth/forward/room/ground tests.
        _spawnAccumulator = std::min(_spawnAccumulator + rate * deltaT,
                                     static_cast<float>(target - static_cast<int>(_drops.size())));
    }
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
        if (_params.snowflakes)
        {
            // Populate the whole visible column, not only a ceiling that takes
            // seconds to reach the eye after freefly moved somewhere else.
            const float bottom = std::max(camPos.Y()-_params.spawnHeight,
                query.GroundHeight(drop.position.X(), drop.position.Z()) + 0.05f);
            drop.position[1] = bottom + RandUnit()*std::max(0.0f, camPos.Y()+_params.spawnHeight-bottom);
            drop.phase = RandUnit()*6.2831853f;
            drop.size = 0.6f + RandUnit()*0.9f;
        }
        drop.speed = _params.fallSpeed * (1.0f + _params.speedJitter * RandSymmetric());
        if (GWind.IsActive())
        {
            const WindSample& wind = GWind.Sample();
            drop.velocityX = wind.velocityX * _params.windResponse;
            drop.velocityZ = wind.velocityZ * _params.windResponse;
        }
        drop.occlusionTimer = RandUnit() * _params.occlusionCadence;
        drop.indoor = false;

        // A drop BORN under cover -- under a bridge deck, a balcony, a canopy,
        // or on an upper storey of a building with no usable interior -- never
        // crosses a roof on its way down, so no amount of look-ahead can see
        // one. This upward probe is the only thing that catches it, and it has
        // to happen here, once, at birth. Costs one ray per spawn (roughly
        // targetDrops / mean lifetime per second); the Weather tab can switch
        // it off and the spawn timing row reports what it costs.
        if (probeSpawn && query.IsSheltered(drop.position, std::max(_params.spawnProbeHeight, 0.0f)))
        {
            _stats.spawnKills++;
            continue;
        }

        _drops.push_back(drop);
        _stats.spawned++;
    }
}

void RainSystem::Simulate(float deltaT)
{
    const ISmokeWorldQuery& query = GSmokeWorldQuery();
    _stats.occlusionTests = 0;

    // The portal tolerance, resolved once: which openings forgive a crossing at
    // all, and how close to one a crossing has to pass. See RainParams.
    InteriorPortalFilter filter;
    filter.radiusScale = std::max(_params.portalRadiusScale, 0.0f);
    filter.doors = true;
    filter.windows = _params.portalWindows;
    filter.breaches = _params.portalBreaches;

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
        Vector3 targetVelocity = RainTargetVelocity(drop.speed, windX, windZ, _params.windResponse);
        if (_params.snowflakes)
        {
            drop.phase += deltaT * (0.8f + drop.size*0.4f);
            targetVelocity += Vector3(0.22f*std::sin(drop.phase), 0, 0.17f*std::cos(drop.phase*1.3f));
        }
        const Vector3 velocity = RainRelaxVelocity(currentVelocity, targetVelocity, _params.windAcceleration, deltaT);
        drop.velocityX = velocity.X();
        drop.velocityZ = velocity.Z();
        const Vector3 nextPosition = drop.position + velocity * deltaT;

        // -- occlusion ----------------------------------------------------
        bool nearShell = false;     // some building footprint overlaps this step
        bool legalIndoor = drop.legalIndoor; // granted only by a portal passage
        if (_params.occlusion == RainOcclusionRooms)
        {
            bool killed = false;
            // Where the drop should be seen to break, when we know. A roof that
            // swallows drops silently reads as rain that stops for no reason;
            // the same splash the ground gets says "it landed on something".
            bool splashOnKill = false;
            Vector3 killPosition = nextPosition;
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
                nearShell = true;
                ++_stats.occlusionTests;
                const Vector3 fromModel = occluder.inv.FastTransform(drop.position);
                const Vector3 toModel = occluder.inv.FastTransform(nextPosition);

                // A drop that entered through an opening on an earlier step is
                // indoors legally: its fate is the floor below, not the wall it
                // is no longer crossing, and the sky probe must not read its
                // ceiling as cover to kill it with.
                if (legalIndoor && occluder.interior->ClassifyModel(fromModel) >= 0)
                {
                    continue;
                }

                const int label = occluder.interior->ClassifySegmentModel(fromModel, toModel);
                if (label == -1)
                {
                    continue; // stayed outside this shell
                }

                // The segment crossed shell or a room boundary. An opening
                // forgives it -- doors, windows and breaches are exactly where
                // smoke leaves and rain comes in -- but only the exact
                // point-segment portal test decides, never a voxel label.
                if (!_params.strictIndoor && occluder.interior->SegmentPassesPortalModel(fromModel, toModel, filter))
                {
                    legalIndoor = true;
                    continue;
                }
                if (label >= 0)
                {
                    _stats.indoorKills++;
                    killed = true;
                }
                else
                {
                    _stats.roofKills++;
                    killed = true;
                    splashOnKill = true;
                }
                break;
            }

            // Companion shelter probe. The voxel sweep above only knows
            // buildings whose type yielded a usable interior; RefreshOcclusion
            // drops the rest, so without this pass a house with no fire
            // geometry, a bridge deck, a rock overhang or a tree canopy would
            // rain through. One staggered fire-geometry ray per drop -- the same
            // query Sheltered mode is built on -- answers for every such roof at
            // once, and the jittered reset keeps the drops from probing in
            // lockstep bursts. A drop that is legally indoors is skipped: its
            // ceiling is the roof it came through, and the probe would kill it
            // mid-room.
            //
            // The ray now runs FORWARD ALONG THE DROP'S OWN PATH, not upward
            // from where it already is. The old upward form could only report a
            // roof the drop had ALREADY gone through, so at the default 0.4 s
            // cadence and 9 m/s a drop fell up to 3.6 m into the room before
            // anything killed it -- which is exactly the rain the owner still
            // saw coming through roofs. Probing the next interval instead costs
            // the same one ray and kills the drop at the surface, with a splash
            // on it.
            if (!killed && !legalIndoor)
            {
                drop.occlusionTimer -= deltaT;
                if (drop.occlusionTimer <= 0.0f)
                {
                    drop.occlusionTimer = std::max(_params.occlusionCadence, 0.01f) * (0.75f + 0.5f * RandUnit());
                    ++_stats.occlusionTests;
                    const Vector3 ahead =
                        RainProbeTarget(nextPosition, velocity, drop.occlusionTimer, _params.probeLookahead);
                    Vector3 hit;
                    if (query.ProbeSegment(drop.position, ahead, hit))
                    {
                        ++_stats.probeKills;
                        killed = true;
                        splashOnKill = true;
                        killPosition = hit;
                    }
                }
            }
            if (killed)
            {
                if (splashOnKill && _params.splashes && static_cast<int>(_splashes.size()) < 256)
                {
                    Splash splash;
                    splash.position = killPosition;
                    splash.age = 0.0f;
                    _splashes.push_back(splash);
                }
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
                if (query.IsSheltered(nextPosition, std::max(_params.shelterProbeHeight, 0.0f)))
                {
                    _stats.indoorKills++;
                    _drops[i] = _drops.back();
                    _drops.pop_back();
                    continue;
                }
            }
        }

        drop.legalIndoor = legalIndoor;

        // -- fall ---------------------------------------------------------
        drop.position = nextPosition;

        float ground = query.GroundHeight(drop.position.X(), drop.position.Z());
        if (nearShell)
        {
            // Under a shell the terrain is not what the drop lands on: the
            // storey floor is (and outdoors this also catches bridge decks and
            // other roadway surfaces). Roadway faces exist only inside the
            // footprint, so drops beside the walls are unaffected.
            const float floorY = query.FloorHeightBelow(drop.position);
            if (floorY > ground)
            {
                ground = floorY;
            }
        }
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
    if (_params.snowflakes && !_snowTexture)
    {
        constexpr int side = 64;
        std::vector<uint8_t> pixels(side * side * 4, 255);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                pixels[(y*side+x)*4+3] = static_cast<uint8_t>(255.0f * SnowflakeAlpha(
                    (x+0.5f)*2.0f/side-1.0f, (y+0.5f)*2.0f/side-1.0f));
        _snowTexture = GEngine->TextBank()->CreateDynamic(side, side, pixels.data(),
            static_cast<uint32_t>(pixels.size()), true, AlphaStats::Blend);
    }
    Texture* texture = _params.snowflakes ? _snowTexture.GetRef() : GScene->Preloaded(TextureRain);
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
    float radianceScale = 1.0f;
    if (_params.snowflakes && GScene->MainLight())
    {
        const auto sky = GEngine->GetSkySettings();
        const float daylight = 1.0f - std::clamp(GScene->MainLight()->NightEffect(), 0.0f, 1.0f);
        // Small tumbling flakes see an averaged hemisphere, not a fixed normal.
        // Keep scene radiance separate from exposure so photo mode cannot erase them.
        radianceScale = std::max(0.02f, sky.sunIntensity * sky.exposure * 0.35f * daylight);
    }

    // Drops: one rotated textured quad per drop, stretched along the drop's
    // true on-screen velocity. DrawDecal is axis-aligned -- the original
    // streaks looked vertical even while their centres moved downwind, and
    // the bead chains that followed were a workaround for exactly that. The
    // streak lengthens with the lean so hard wind reads as hard wind.
    for (const Drop& drop : _drops)
    {
        if (drop.indoor)
        {
            continue; // the point of the feature: nothing falls indoors
        }
        const Vector3 velocity(drop.velocityX, -drop.speed, drop.velocityZ);
        const float speed = std::max(velocity.Size(), 0.001f);
        const float horizontalSpeed = std::sqrt(Square(drop.velocityX) + Square(drop.velocityZ));
        // The streak runs from this drop BACK along its true velocity, and its
        // length grows with the lean so hard wind reads as hard wind. One
        // rotated textured quad per drop -- direction on screen is the whole
        // point, which an axis-aligned decal cannot carry (the bead chains
        // this replaced were a workaround for exactly that).
        const float leanRatio = horizontalSpeed / speed;
        const float streakLength = _params.streakLength * (_params.snowflakes ? 1.0f : 1.0f + leanRatio);
        const Vector3 tailWorld = drop.position - velocity * (streakLength / speed);

        Vector3 headView = toView * drop.position;
        Vector3 tailView = toView * tailWorld;
        if (headView.Z() < nearest || tailView.Z() < nearest)
        {
            continue;
        }
        const Vector3 midWorld = (drop.position + tailWorld) * 0.5f;
        Vector3 midView = toView * midWorld;

        const float invWHead = 1.0f / headView.Z();
        const float invWTail = 1.0f / tailView.Z();
        const float invWMid = 1.0f / midView.Z();

        Vector3 headP = toView * drop.position;
        headP[0] = project(0, 2) + project(0, 0) * headP[0] * invWHead;
        headP[1] = project(1, 2) + project(1, 1) * headP[1] * invWHead;
        Vector3 tailP = toView * tailWorld;
        tailP[0] = project(0, 2) + project(0, 0) * tailP[0] * invWTail;
        tailP[1] = project(1, 2) + project(1, 1) * tailP[1] * invWTail;

        const float dirX = _params.snowflakes ? std::sin(drop.phase*0.7f) : headP[0] - tailP[0];
        const float dirY = _params.snowflakes ? std::cos(drop.phase*0.7f) : headP[1] - tailP[1];
        const float sizeScale = _params.snowflakes ? drop.size : 1.0f;
        const float flakeWidth = std::fabs(project(0, 0)) * _params.streakWidth * sizeScale * invWMid * invLeft;
        const float lengthPx = PrecipitationSpriteLength(_params.snowflakes,
            std::sqrt(dirX * dirX + dirY * dirY), flakeWidth);
        if (lengthPx < 0.5f)
        {
            continue;
        }

        // Depth from the midpoint, softened by the same fraction of the
        // streak DrawDecal's callers use, so the quad still depth-tests
        // against the world without z-fighting along its own length.
        const float nearPos = floatMax(midView.Z() - std::min(streakLength * 0.10f, 0.10f), nearest);
        const float invNearPos = 1.0f / nearPos;
        Vector3 posp = toView * midWorld;
        posp[0] = project(0, 2) + project(0, 0) * posp[0] * invWMid;
        posp[1] = project(1, 2) + project(1, 1) * posp[1] * invWMid;
        posp[2] = project(2, 2) + project.Position()[2] * invNearPos;

        const float tumble = _params.snowflakes ? 0.65f + 0.35f*std::abs(std::sin(drop.phase)) : 1.0f;
        const float halfWidthPx = flakeWidth * 0.5f * tumble;
        if (halfWidthPx <= 0.0f)
        {
            continue;
        }

        const float distance2 = (drop.position - camera.Position()).SquareSize();
        const float fog = GScene->Fog8(distance2) * (1.0f / 255.0f);
        Color colour = tint;
        colour.SetA(_params.opacity * (1.0f - fog));

        // Snow is an occluding particle, not an additive light flare. White RGB
        // plus a soft alpha mask stays legible without black corners or bloom.
        const int specials = kRainSpecials;
        GEngine->DrawDecalRotated(posp, invNearPos, halfWidthPx, lengthPx * 0.5f, PackedColor(colour), mip,
                                  specials, dirX, dirY, radianceScale);
        ++_stats.streakDecals;
        ++_stats.drawnDrops;
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

    if (_forceWeatherRain)
    {
        // Once per second is plenty: SetRain ramps density over `time`.
        static Foundation::Time nextForce;
        if (Glob.time > nextForce)
        {
            nextForce = Glob.time + 1.0f;
            GLandscape->SetRain(1.0f, 1.0f);
            // Rain implies cloud. Without this the forced density produced a
            // downpour out of a clear blue sky, which is not a frame anything
            // downstream of the weather can be judged against -- cloud shadows,
            // the sky's own transmittance, sun and moon occlusion all read
            // overcast, not rain density.
            //
            // Through GWorld, NOT Landscape::SetOvercast: World re-applies its
            // own weather state to the landscape every frame, so a direct
            // landscape write is silently reverted before the next draw (it
            // measured as cloudCover=0.23 with the overcast pinned to 1).
            if (GWorld != nullptr && GWorld->GetOvercast() < 0.99f)
            {
                GWorld->SetWeather(1.0f, GWorld->GetFog(), 2.0f);
            }
        }
    }

    const auto begin = std::chrono::steady_clock::now();

    float deltaT = Glob.time - _lastUpdate;
    _lastUpdate = Glob.time;
    // Same hitch rule as smoke: a long frame integrated in one step teleports
    // every drop through the ground; clamp rather than substep.
    deltaT = std::clamp(deltaT, 0.0f, 0.10f);
    // Freefly can move while simulation time is paused. Keep drawing and
    // replenishing its local snow volume, but integrate no fall/tumble motion.
    if (deltaT <= 0.0f && !_params.snowflakes)
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

    // Opt-in once-per-second trail for POSEIDON_TEST_RAIN, same shape as the
    // SmokeVolume one: alive versus drawn catches projection rejection, the
    // kill split says which occlusion path did the work.
    // Trace ordinary mission weather without the test hook forcing a downpour.
    static const bool verbose = RainTestHook() || [] {
        const char* value = std::getenv("POSEIDON_RAIN_TRACE");
        return value && std::string(value) == "1";
    }();
    if (verbose && Glob.time > _nextDiagTime)
    {
        _nextDiagTime = Glob.time + 1.0f;
        const WindSample* wind = GWind.IsActive() ? &GWind.Sample() : nullptr;
        const float windSpeed = wind ? wind->speed : 0.0f;
        // The lean the CURRENT drops should be showing, so a smoke log can be
        // compared against what is on screen without guessing.
        const float leanDeg =
            std::atan2(windSpeed * _params.windResponse, std::max(_params.fallSpeed, 0.01f)) * (180.0f / 3.14159265f);
        LOG_INFO(World,
                 "RainVolume: drops={} drawn={} decals={} spawned={} covered={} roof={} indoor={} probe={} ground={} "
                 "buildings={} sweeps={} wind={:.1f}m/s@{:.0f}deg lean={:.0f}deg spawn={:.0f}us gather={:.0f}us sim={:.0f}us draw={:.0f}us volumeCulls={} capCulls={} limit={}",
                 _stats.liveDrops, _stats.drawnDrops, _stats.streakDecals, _stats.spawned, _stats.spawnKills,
                 _stats.roofKills, _stats.indoorKills, _stats.probeKills, _stats.groundHits, _stats.nearbyBuildings,
                 _stats.occlusionTests, windSpeed,
                 wind ? wind->directionRad * (180.0f / 3.14159265f) : 0.0f, leanDeg, _stats.spawnMicroseconds,
                 _stats.occlusionMicroseconds, _stats.simulateMicroseconds, _stats.drawMicroseconds,
                 _stats.volumeCulls, _stats.populationCulls, _stats.populationLimit);
        ResetStats();
    }
}

} // namespace Poseidon
