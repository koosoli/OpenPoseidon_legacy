#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Weather/RainParticlePolicy.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>
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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
RainSystem GRain;


RainSystem GSnowFlakes;

namespace
{
// PERF-031: true when no terrain vertex under the snow window (plus one terrain cell all round)
// reaches within a metre of `line`. Terrain is piecewise linear between those vertices, so no
// terrain fragment in the window gets altitude cover (terrain.wgsl snowline_depth: smoothstep from
// the line upwards, 0 at and below it). Any vertex off the map answers false: the off-map ring is
// not ours to bound here, so the mask runs as before.
bool SnowWindowBelowSnowline(const Landscape& land, float x0, float z0, float span, float line)
{
    const float grid = land.GetTerrainGrid();
    const int range = land.GetTerrainRange();
    if (!(grid > 0.0f) || range <= 0 || !std::isfinite(line))
        return false;
    const int xi0 = static_cast<int>(std::floor(x0 / grid)) - 1;
    const int zi0 = static_cast<int>(std::floor(z0 / grid)) - 1;
    const int xi1 = static_cast<int>(std::floor((x0 + span) / grid)) + 2;
    const int zi1 = static_cast<int>(std::floor((z0 + span) / grid)) + 2;
    if (xi0 < 0 || zi0 < 0 || xi1 >= range || zi1 >= range)
        return false;
    for (int z = zi0; z <= zi1; ++z)
        for (int x = xi0; x <= xi1; ++x)
            if (!(land.ClippedData(z, x) < line - 1.0f))
                return false;
    return true;
}
// PERF-031 (second half): the probe answers per building, kept between calls.
//
// With snow on, the mask still probed every 0.5 m lattice point under every roof in the window on
// every call -- the same points, with the same answers, every 0.1 s, because buildings and terrain
// do not move: 75 ms a call over the Big Shell. The probe points sit on one global lattice (the
// window origin is a whole multiple of 2 m, the probes 0.25 m + 0.5 m k off it), so each building
// keeps a byte per lattice point of its footprint: 0 not probed yet, 1 open, 2 covered. A call
// probes only the points it has not seen and reads the rest. The answer depends on the building's
// pose and interior and on the terrain height at the point, so a changed pose, radius or interior
// resets that building, and a terrain height revision resets them all. POSEIDON_SNOW_SHELTER_CACHE=0
// probes every point every call, as before.
struct ShelterProbeCache
{
    const void* interior = nullptr;
    Vector3 position, direction, up;
    float radius = 0.0f;
    int gx0 = 0, gz0 = 0, w = 0, h = 0; // footprint on the global 0.5 m lattice
    std::vector<uint8_t> state;
    uint64_t lastUse = 0;
};
} // namespace

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
    // PERF-031 -- the shelter mask writes a deficit under every roof in the 64 m window, probing a
    // 0.5 m grid against each building's interior voxels: 75 ms every 0.1 s on the main thread over
    // the MGS2 Big Shell (its modules are buildings whose radius spans the whole window), and that
    // on Desert Island, where no snow can lie at all -- the alpine snowline is on by default at
    // ~645 m and nothing there is above 20 m. A deficit only ever subtracts from snow: with no
    // snowfall deposit in the window (data[3]) the shaders read it only through terrain altitude
    // cover (terrain.wgsl snow_cover; conform/shader3d snow_depth return 0 without deposit), which
    // is 0 on every terrain fragment below the line. So when no deposit is present and the window's
    // terrain stays under the snowline, the mask cannot change a pixel and is skipped.
    // POSEIDON_SNOW_SHELTER_SKIP=0 masks anyway (for A/B).
    static const bool skipWhenBare = [] {
        const char* value = std::getenv("POSEIDON_SNOW_SHELTER_SKIP");
        return !(value && value[0] == '0');
    }();
    if (skipWhenBare && data[3] <= 0.0f &&
        (!GSnow().EffSnowlineEnabled() || SnowWindowBelowSnowline(land, data[0], data[1], span, GSnow().EffSnowlineHeight())))
        return;
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
    static const bool cacheEnabled = [] {
        const char* value = std::getenv("POSEIDON_SNOW_SHELTER_CACHE");
        return !(value && value[0] == '0');
    }();
    static std::unordered_map<const Building*, ShelterProbeCache> probeCache;
    static uint64_t cacheHeightRevision = ~0ull;
    static uint64_t cacheCall = 0;
    ++cacheCall;
    if (cacheHeightRevision != land.HeightRevision())
    {
        probeCache.clear();
        cacheHeightRevision = land.HeightRevision();
    }
    // Bounded: buildings that have not been in a window for a long while are dropped.
    if (probeCache.size() > 2048)
        for (auto it = probeCache.begin(); it != probeCache.end();)
            it = (cacheCall - it->second.lastUse > 600) ? probeCache.erase(it) : std::next(it);
    // The lattice mapping below holds only for the window SnowField hands over.
    const bool latticeOk = cacheEnabled && data[2] == 0.125f && std::fmod(data[0], 2.0f) == 0.0f &&
                           std::fmod(data[1], 2.0f) == 0.0f;
    size_t cachedReads = 0;
    for (const Building* building : buildings)
    {
        const auto* interior = BuildingInterior::GetFor(building->GetBType());
        if (!interior || !interior->Valid()) continue;
        const auto p = building->Position();
        const float radius = building->GetRadius();
        const auto inv = building->GetInvTransform();
        const bool tilted = building->DirectionUp().Y() < 0.999f;
        const auto probe = [&](float wx, float wz)
        {
            const Vector3 point(wx, land.SurfaceY(wx, wz)+0.05f, wz);
            ++probes;
            bool covered = interior->CoveredFromAboveModel(inv * point);
            if (tilted)
                covered = interior->ClassifySegmentModel(inv*point, inv*(point+Vector3(0, radius*2, 0))) != -1;
            return covered;
        };
        ShelterProbeCache* cache = nullptr;
        if (latticeOk)
        {
            ShelterProbeCache& c = probeCache[building];
            if (c.interior != interior || c.radius != radius || !(c.position == p) ||
                !(c.direction == building->Direction()) || !(c.up == building->DirectionUp()))
            {
                c.interior = interior;
                c.radius = radius;
                c.position = p;
                c.direction = building->Direction();
                c.up = building->DirectionUp();
                c.gx0 = static_cast<int>(std::floor((p.X() - radius - 0.25f) / 0.5f)) - 1;
                c.gz0 = static_cast<int>(std::floor((p.Z() - radius - 0.25f) / 0.5f)) - 1;
                c.w = static_cast<int>(std::ceil(2.0f * radius / 0.5f)) + 4;
                c.h = c.w;
                c.state.assign(static_cast<size_t>(c.w) * static_cast<size_t>(c.h), 0);
            }
            c.lastUse = cacheCall;
            cache = &c;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor((p.X()-radius-data[0]) / (data[2]*step))) * step);
        const int z0 = std::max(0, static_cast<int>(std::floor((p.Z()-radius-data[1]) / (data[2]*step))) * step);
        const int x1 = std::min(n, static_cast<int>(std::ceil((p.X()+radius-data[0]) / data[2])));
        const int z1 = std::min(n, static_cast<int>(std::ceil((p.Z()+radius-data[1]) / data[2])));
        for (int z = z0; z < z1; z += step)
            for (int x = x0; x < x1; x += step)
            {
                const float wx = data[0] + (x+step*0.5f)*data[2];
                const float wz = data[1] + (z+step*0.5f)*data[2];
                bool covered;
                uint8_t* slot = nullptr;
                if (cache)
                {
                    // wx = 0.25 + 0.5 * gx exactly: data[0] is a whole multiple of 2 m, x of 4 cells.
                    const int gx = static_cast<int>(data[0] * 2.0f) + x / step - cache->gx0;
                    const int gz = static_cast<int>(data[1] * 2.0f) + z / step - cache->gz0;
                    if (gx >= 0 && gz >= 0 && gx < cache->w && gz < cache->h)
                        slot = &cache->state[static_cast<size_t>(gz) * cache->w + gx];
                }
                if (slot && *slot != 0)
                {
                    covered = *slot == 2;
                    ++cachedReads;
                }
                else
                {
                    covered = probe(wx, wz);
                    if (slot) *slot = covered ? 2 : 1;
                }
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
        LOG_INFO(World, "Snow shelter: buildings={} probes={} cached={} masked={} time={:.0f}us", buildings.size(), probes, cachedReads, masked, us);
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
    if (const char* look = std::getenv("POSEIDON_RAIN_LOOK"); look != nullptr && look[0] == '1')
    {
        _params.modernLook = true;
    }
    // alpha, length scale, count scale of the new look (the Weather tab's sliders), for captures
    if (const char* tune = std::getenv("POSEIDON_RAIN_LOOK_TUNE"))
    {
        std::sscanf(tune, "%f,%f,%f", &_params.modernAlpha, &_params.modernLength, &_params.modernCount);
    }
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

// (OP, 2026-10-14) RAIN part 2, roof runoff (owner: "make our rain ... better inspired with the tarp
// runoff demo" of dgreenheck/threejs-particle-fluids). Rain on a roof does not vanish at the
// roofline: it runs down the roof and leaves it along the eaves, in drips that land in a line
// under them. The roof of a building type is mapped once, in model space, from its interior's
// cached shell columns: a covered column with an open neighbour is the roof's edge, and it is an
// EAVE where the roof falls toward that neighbour (a gable end, where it does not, sheds nothing).
const std::vector<Vector3>& RainSystem::EavesOf(const Occluder& occluder)
{
    const auto found = _eaves.find(occluder.interior);
    if (found != _eaves.end())
    {
        return found->second;
    }
    std::vector<Vector3> eaves;
    int nCovered = 0, nEdges = 0, nLow = 0, nFlat = 0;
    const LODShapeWithShadow* shape = occluder.object->GetShape();
    if (shape != nullptr)
    {
        const Vector3 lo = shape->Min(), hi = shape->Max();
        constexpr float step = 0.5f;
        const int nx = std::clamp(static_cast<int>((hi.X() - lo.X()) / step) + 1, 1, 128);
        const int nz = std::clamp(static_cast<int>((hi.Z() - lo.Z()) / step) + 1, 1, 128);
        const BuildingInterior& interior = *occluder.interior;
        std::vector<unsigned char> covered(static_cast<size_t>(nx) * nz);
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x)
                covered[static_cast<size_t>(z) * nx + x] =
                    interior.CoveredFromAboveModel(Vector3(lo.X() + x * step, lo.Y() + 0.1f, lo.Z() + z * step)) ? 1 : 0;
        for (unsigned char c : covered)
            nCovered += c;
        const auto isCovered = [&](int x, int z)
        { return x >= 0 && z >= 0 && x < nx && z < nz && covered[static_cast<size_t>(z) * nx + x] != 0; };
        // the roof's height over a column: the topmost shell voxel, scanned down from the top
        const auto roofY = [&](int x, int z)
        {
            const float px = lo.X() + x * step, pz = lo.Z() + z * step;
            for (float y = hi.Y(); y > lo.Y(); y -= 0.15f)
                if (interior.ClassifyModel(Vector3(px, y, pz)) == -2)
                    return y;
            return lo.Y();
        };
        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x)
            {
                if (!isCovered(x, z))
                    continue;
                for (const auto& d : dirs)
                {
                    if (isCovered(x + d[0], z + d[1]))
                        continue;
                    // the roof must fall toward this edge, over the cell behind it
                    if (!isCovered(x - d[0], z - d[1]))
                        continue;
                    ++nEdges;
                    // the overhang is thin: the outermost covered column often holds no shell voxel at
                    // the sample points (89 of 106 edges on zluty_statek_in), so the roof's height
                    // there is taken from the first column inward that has one, and its fall from
                    // the next one in
                    float edge = lo.Y();
                    int k0 = 0;
                    for (; k0 < 4; ++k0)
                    {
                        edge = roofY(x - d[0] * k0, z - d[1] * k0);
                        if (edge >= lo.Y() + 1.5f)
                            break;
                    }
                    if (k0 == 4)
                    {
                        ++nLow;
                        continue;
                    }
                    const float inner = roofY(x - d[0] * (k0 + 1), z - d[1] * (k0 + 1));
                    if (inner < edge + 0.03f)
                    {
                        ++nFlat;
                        continue;
                    }
                    // extrapolate the roof's fall out to the edge column
                    edge -= (inner - edge) * static_cast<float>(k0);
                    eaves.emplace_back(lo.X() + (x + d[0] * 0.6f) * step, edge - 0.05f, lo.Z() + (z + d[1] * 0.6f) * step);
                }
            }
    }
    LOG_INFO(World, "Rain runoff: {} eave points on '{}' ({} covered columns, {} edges, {} low, {} not falling; box {:.1f}..{:.1f} m)",
             eaves.size(), occluder.object->GetShape() ? occluder.object->GetShape()->Name() : "?", nCovered, nEdges, nLow,
             nFlat, shape ? shape->Min().Y() : 0.0f, shape ? shape->Max().Y() : 0.0f);
    return _eaves.emplace(occluder.interior, std::move(eaves)).first->second;
}

void RainSystem::EmitEaveDrips(float deltaT)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_RAIN_EAVES");
        return !(v && v[0] == '0');
    }();
    if (!enabled || _params.snowflakes || _occluders.empty() || deltaT <= 0.0f)
    {
        return;
    }
    const float density = EffectiveDensity();
    static const bool trace = std::getenv("POSEIDON_RAIN_TRACE") != nullptr;
    static float nextTrace = 0.0f;
    if (trace && Glob.time.toFloat() > nextTrace)
    {
        nextTrace = Glob.time.toFloat() + 2.0f;
        size_t points = 0;
        for (const Occluder& occluder : _occluders)
        {
            const auto it = _eaves.find(occluder.interior);
            points += it == _eaves.end() ? 0 : it->second.size();
        }
        LOG_INFO(World, "Rain runoff: density {:.2f}, {} buildings, {} eave points", density, _occluders.size(), points);
    }
    if (density < 0.05f)
    {
        return;
    }
    // drips per second along each 0.5 m of eave at full rain: a steady patter, heavier than the rain
    const float perPoint = 2.5f * density;
    for (const Occluder& occluder : _occluders)
    {
        const std::vector<Vector3>& eaves = EavesOf(occluder);
        if (eaves.empty())
        {
            continue;
        }
        _dripAccumulator += perPoint * deltaT * static_cast<float>(eaves.size());
        while (_dripAccumulator >= 1.0f && static_cast<int>(_drops.size()) < _params.maxDrops)
        {
            _dripAccumulator -= 1.0f;
            const Vector3& at = eaves[static_cast<size_t>(RandUnit() * eaves.size()) % eaves.size()];
            Drop drop;
            drop.position = occluder.object->PositionModelToWorld(
                Vector3(at.X() + RandSymmetric() * 0.2f, at.Y(), at.Z() + RandSymmetric() * 0.2f));
            // a drip leaves the eave slowly and does not ride the wind like spray
            drop.speed = _params.fallSpeed * (0.85f + 0.1f * RandSymmetric());
            drop.size = 1.6f;
            drop.occlusionTimer = RandUnit() * _params.occlusionCadence;
            _drops.push_back(drop);
        }
        _dripAccumulator = std::min(_dripAccumulator, 1.0f);
    }
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
    // RAIN-LOOK: thin, mostly see-through streaks need more of them to read as heavy rain
    const float lookScale = (!_params.snowflakes && _params.modernLook) ? _params.modernCount : 1.0f;
    const int targetDrops = static_cast<int>(_params.targetDrops * lookScale);
    const int maxDrops = static_cast<int>(_params.maxDrops * lookScale);
    const float rate = static_cast<float>(targetDrops) * EffectiveDensity() / meanLifetime;
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
        const int target = RainParticlePopulationLimit(targetDrops, maxDrops, EffectiveDensity());
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
        if (static_cast<int>(_drops.size()) >= maxDrops)
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

        // the sea's surface is where a drop over it ends (the seabed was: rain fell through the water)
        float ground = std::max(query.GroundHeight(drop.position.X(), drop.position.Z()), GLandscape->GetSeaLevel());
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

// RAIN-LOOK: a tapered, soft-edged streak (u across, v along; the head at v = 1 is the brightest,
// the tail fades out), and a thin ring for the splashes.
static float RainStreakAlpha(float u, float v)
{
    const float across = std::exp(-Square((u - 0.5f) / 0.22f));
    const float along = std::pow(std::sin(3.14159265f * v), 0.6f) * (0.55f + 0.45f * v);
    return across * along;
}

static float RainRingAlpha(float x, float y)
{
    const float r = std::sqrt(x * x + y * y);
    return r < 1.0f ? std::exp(-Square((r - 0.78f) / 0.09f)) : 0.0f;
}

// a streak texture laid along x (the long axis of DrawDecalRotated's quad runs along u)
static float RainStreakAlongU(float u, float v)
{
    return RainStreakAlpha(v, u);
}

static Ref<Texture> MakeRainTexture(int w, int h, float (*alpha)(float, float), bool centred)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const float u = (x + 0.5f) / w;
            const float v = (y + 0.5f) / h;
            const float a = centred ? alpha(u * 2.0f - 1.0f, v * 2.0f - 1.0f) : alpha(u, v);
            pixels[(static_cast<size_t>(y) * w + x) * 4 + 3] = static_cast<uint8_t>(255.0f * std::clamp(a, 0.0f, 1.0f));
        }
    }
    return GEngine->TextBank()->CreateDynamic(w, h, pixels.data(), static_cast<uint32_t>(pixels.size()), true,
                                              AlphaStats::Blend);
}

void RainSystem::DrawModern()
{
    if (!_streakTexture)
        _streakTexture = MakeRainTexture(128, 16, RainStreakAlongU, false);
    if (!_ringTexture)
        _ringTexture = MakeRainTexture(64, 64, RainRingAlpha, true);
    if (!_streakTexture || !_ringTexture)
        return;
    MipInfo streakMip = GEngine->TextBank()->UseMipmap(_streakTexture.GetRef(), 0, 0);
    MipInfo ringMip = GEngine->TextBank()->UseMipmap(_ringTexture.GetRef(), 0, 0);
    if (!streakMip.IsOK() || !ringMip.IsOK())
        return;

    const Camera& camera = *GScene->GetCamera();
    Matrix4Val project = camera.Projection();
    Matrix4Val toView = GScene->ScaledInvTransform();
    const float nearest = camera.Near() * 10.0f;
    const float invLeft = camera.InvLeft();
    const float invTop = camera.InvTop();
    const Vector3 camPos = camera.Position();

    // The light a drop shows: the sky's, as the snow's (an averaged hemisphere), a little under it
    // since a drop transmits most of what is behind it. Not a tint: at noon the streaks are bright
    // against houses and trees and almost gone against the sky; at night they need a light.
    float radiance = 0.02f;
    Vector3 toSun(0, 1, 0);
    float sunGlint = 0.0f;
    if (const LightSun* sun = GScene->MainLight())
    {
        const auto sky = GEngine->GetSkySettings();
        const float daylight = 1.0f - std::clamp(sun->NightEffect(), 0.0f, 1.0f);
        radiance = std::max(0.02f, sky.sunIntensity * sky.exposure * 0.14f * daylight);
        toSun = -sun->SunDirection(); // SunDirection points FROM the sun
        toSun.Normalize();
        // drops toward the sun glint (forward scattering through the drop), less so under a thick deck
        const float overcast = GLandscape != nullptr ? GLandscape->GetOvercast() : 0.0f;
        sunGlint = daylight * (1.0f - 0.75f * overcast) * std::clamp(toSun.Y() * 4.0f + 0.2f, 0.0f, 1.0f);
    }
    // grey-white, a little cool: the sky seen through and off a drop
    const Color white(0.86f, 0.89f, 0.93f, 1.0f);
    // a streak is a few millimetres of water smeared by its fall: mostly see-through. The first pass
    // (0.34, sky x 0.28) drew bright white lines (owner, 2026-10-26: "looks like white lines"); rain
    // reads as a faint grey veil, so each streak is barely there and the count does the work.
    const float streakAlpha = _params.modernAlpha * _params.opacity;
    const float widthM = 0.012f;

    for (const Drop& drop : _drops)
    {
        if (drop.indoor)
            continue;
        const Vector3 velocity(drop.velocityX, -drop.speed, drop.velocityZ);
        const float speed = std::max(velocity.Size(), 0.001f);
        const float horizontalSpeed = std::sqrt(Square(drop.velocityX) + Square(drop.velocityZ));
        const float streakLength = _params.streakLength * _params.modernLength * (1.0f + horizontalSpeed / speed);
        const Vector3 tailWorld = drop.position - velocity * (streakLength / speed);

        const Vector3 headView = toView * drop.position;
        const Vector3 tailView = toView * tailWorld;
        if (headView.Z() < nearest || tailView.Z() < nearest)
            continue;
        const Vector3 midWorld = (drop.position + tailWorld) * 0.5f;
        const Vector3 midView = toView * midWorld;
        const float invWHead = 1.0f / headView.Z();
        const float invWTail = 1.0f / tailView.Z();
        const float invWMid = 1.0f / midView.Z();
        const float hx = project(0, 2) + project(0, 0) * headView.X() * invWHead;
        const float hy = project(1, 2) + project(1, 1) * headView.Y() * invWHead;
        const float tx = project(0, 2) + project(0, 0) * tailView.X() * invWTail;
        const float ty = project(1, 2) + project(1, 1) * tailView.Y() * invWTail;
        const float dirX = hx - tx;
        const float dirY = hy - ty;
        const float lengthPx = std::sqrt(dirX * dirX + dirY * dirY);
        if (lengthPx < 0.5f)
            continue;

        // thin streaks are drawn two pixels wide and that much fainter: narrower, a rotated quad
        // flickers along its length into dotted lines; far rain is a fine veil, not a field of dashes
        float widthPx = std::fabs(project(0, 0)) * widthM * invWMid * invLeft;
        float coverage = 1.0f;
        if (widthPx < 2.0f)
        {
            coverage = std::max(widthPx / 2.0f, 0.12f);
            widthPx = 2.0f;
        }

        const float nearPos = floatMax(midView.Z() - std::min(streakLength * 0.10f, 0.10f), nearest);
        const float invNearPos = 1.0f / nearPos;
        Vector3 posp(project(0, 2) + project(0, 0) * midView.X() * invWMid,
                     project(1, 2) + project(1, 1) * midView.Y() * invWMid,
                     project(2, 2) + project.Position()[2] * invNearPos);

        const Vector3 toDrop = drop.position - camPos;
        const float distance2 = toDrop.SquareSize();
        const float fog = GScene->Fog8(distance2) * (1.0f / 255.0f);
        const float dist = std::sqrt(distance2);
        // the nearest drops would draw long bright lines across the lens: fade them in over 3 m
        const float nearFade = std::clamp((dist - 0.6f) / 2.4f, 0.0f, 1.0f);
        // Against the sun a drop is lit from behind and scatters the sun forward into the eye: a broad
        // brightening (forward lobe) and a sharp glint round the sun. With the sun at your back it
        // sends almost nothing back: the rain thins toward see-through (owner, 2026-10-26). Under a
        // thick deck there is no sun to do either (sunGlint falls with the overcast).
        const float cosSun = (toDrop * toSun) / std::max(dist, 1e-3f);
        const float toward = std::max(cosSun, 0.0f);
        const float away = std::max(-cosSun, 0.0f);
        const float glint = std::max(1.0f + sunGlint * (1.2f * toward * toward + 3.0f * std::pow(toward, 10.0f)
                                                        - 0.65f * away),
                                     0.2f);

        Color colour = white;
        colour.SetA(std::clamp(streakAlpha * coverage * nearFade * glint, 0.0f, 1.0f) * (1.0f - fog));
        GEngine->DrawDecalRotated(posp, invNearPos, widthPx * 0.5f, lengthPx * 0.5f, PackedColor(colour), streakMip,
                                  kRainSpecials, dirX, dirY, radiance * std::min(glint, 2.0f));
        ++_stats.streakDecals;
        ++_stats.drawnDrops;
    }

    // Splashes: a thin ring that widens and fades where a drop lands, laid flat on the ground (from
    // eye height a narrow ellipse, not a disc facing the camera).
    for (const Splash& splash : _splashes)
    {
        const float t = std::clamp(splash.age / std::max(_params.splashLifetime, 1e-3f), 0.0f, 1.0f);
        const float radius = 0.03f + 0.6f * _params.splashSize * std::sqrt(t);
        const Vector3 view = toView * splash.position;
        if (view.Z() < nearest)
            continue;
        const float invW = 1.0f / view.Z();
        const float nearPos = floatMax(view.Z() - 0.05f, nearest);
        const float invNearPos = 1.0f / nearPos;
        Vector3 posp(project(0, 2) + project(0, 0) * view.X() * invW, project(1, 2) + project(1, 1) * view.Y() * invW,
                     project(2, 2) + project.Position()[2] * invNearPos);
        const float halfW = std::fabs(project(0, 0)) * radius * invW * invLeft;
        const Vector3 toSplash = splash.position - camPos;
        const float dist = std::max(toSplash.Size(), 1e-3f);
        const float flat = std::clamp(-toSplash.Y() / dist, 0.12f, 1.0f);
        const float halfH = std::fabs(project(1, 1)) * radius * invW * invTop * flat;
        if (halfW < 0.6f)
            continue;
        const float fog = GScene->Fog8(toSplash.SquareSize()) * (1.0f / 255.0f);
        Color colour = white;
        colour.SetA(0.55f * _params.opacity * std::pow(1.0f - t, 1.5f) * (1.0f - fog));
        // the quad's long axis horizontal on screen: the ring's width across, its foreshortened depth up
        GEngine->DrawDecalRotated(posp, invNearPos, halfH, halfW, PackedColor(colour), ringMip, kRainSpecials, 1.0f,
                                  0.0f, radiance * 1.2f);
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
    if (!_params.snowflakes && _params.modernLook)
    {
        DrawModern();
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
    // No rain under water (owner, 2026-10-13: "it is raining under water which is a bug"): a camera
    // below the surface draws none; above it, drops end on the water (Simulate), not on the seabed.
    if (const Camera* camera = GScene->GetCamera())
    {
        const Vector3 eye = camera->Position();
        if (eye.Y() < SwimWaterSurfaceY(eye.X(), eye.Z()))
        {
            _drops.clear();
            _splashes.clear();
            return;
        }
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
    EmitEaveDrips(deltaT);
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
