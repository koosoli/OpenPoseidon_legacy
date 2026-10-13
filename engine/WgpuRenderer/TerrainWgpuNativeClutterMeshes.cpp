// RFG-153: Reforger's clutter plants near the camera as their own meshes.
//
// The grass system draws every clutter plant of a native Everon meadow as ONE rasterised
// impostor card (ComposeImpostor, TerrainWgpuEnfusionClutter.cpp). Past twenty-odd metres that
// is indistinguishable from the real thing; at the camera it is a flat photograph standing in
// the grass, which is the clearest difference between our meadow and Reforger's in a matched
// Workbench frame. Reforger itself has no impostor tier for clutter: a plant's .xob carries a
// coverage LOD ladder down to a few dozen triangles and that is what it draws.
//
// So, within `radius` of the camera, the plants become what they are: their .xob through the
// ShapeBank (ModelCache's external .xob loader, the same route every streamed native object
// takes), drawn by the GPU-driven object path as render-only instances
// (EngineWgpu::SceneClutterObjectCreated) -- their own MatPBRTreeCrown material, alpha, LODs and
// shadows, with no grass-specific shading in between. The cards hand over across the last
// kHandover metres (cs_place_mid, card_yields_to_mesh): a card survives with probability
// smoothstep(r - band, r, d), a mesh with the complement.
//
// Placement mirrors the card ring's statistics, not its exact positions (the cards are placed on
// the GPU): the same geography word per land cell -- grass cell, not blocked, authored, the
// cell's coverage, its run of atlas layers -- the same per-layer selection weights and
// MinScale..MaxScale, the same roads and hard-object footprints kept clear, on a grid of the
// card spacing. Cells are 4 m squares built once and torn down when they leave the ring; the
// ones in the handover band are rebuilt as the camera moves so the hand-off follows it.
//
// WGR_CLUTTER_MESH_RADIUS=<m> (default 24, 0 = off: the cards draw to the camera as before);
// WGR_CLUTTER_MESH_SPACING=<m> grid of candidate plants (default 0.56, the card ring's);
// WGR_CLUTTER_MESH_DENSITY=<0..4> fraction of it that grows (default 0.15, measured).

#include "TerrainWgpu.hpp"

#include "EngineWgpu.hpp"

#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace Poseidon
{
namespace
{
constexpr float kCell = 4.0f;      // ring cell edge, metres
constexpr float kHandover = 4.0f;  // width of the card <-> mesh band at the ring's edge
constexpr uint32_t kIdBase = 0x40000000u;

float EnvFloat(const char* name, float fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
        return fallback;
    const float f = std::strtof(v, nullptr);
    return std::isfinite(f) ? f : fallback;
}

uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Hash01(int cx, int cz, uint32_t k, uint32_t salt)
{
    const uint32_t h = Hash(static_cast<uint32_t>(cx) * 0x8da6b343u ^ static_cast<uint32_t>(cz) * 0xd8163841u ^
                            k * 0xcb1ab31fu ^ salt);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

float Smoothstep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / std::max(b - a, 1e-4f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// grass.wgsl GEO_* (one word per land cell, TerrainWgpuEnfusionClutter.cpp writes them).
constexpr uint32_t kGeoGrass = 0x80000000u;
constexpr uint32_t kGeoAuthored = 0x00008000u;
constexpr uint32_t kGeoExcludeHard = 0x00000c63u;
constexpr uint32_t kGeoExcludeSoft = 0x00000018u;
} // namespace

void TerrainWgpu::ResetNativeClutterMeshes()
{
    for (auto& entry : _clutterMeshCells)
        for (uint32_t id : entry.second.ids)
        {
            _engine.SceneClutterObjectRemoved(id);
            _clutterMeshFreeIds.push_back(id);
        }
    _clutterMeshCells.clear();
}

void TerrainWgpu::UpdateNativeClutterMeshes(const Landscape& land, const Vector3& camera)
{
    static const float radius = std::clamp(EnvFloat("WGR_CLUTTER_MESH_RADIUS", 24.0f), 0.0f, 80.0f);
    static const float spacing = std::clamp(EnvFloat("WGR_CLUTTER_MESH_SPACING", 0.56f), 0.2f, 4.0f);
    // Fraction of the candidate grid that grows a plant, on top of the cell's authored coverage.
    // 0.15 measured against Workbench at three meadow poses: at 1.0 (the card ring's own grid)
    // the near meadow carried 10x Reforger's yellow and 30x its white flowers; at 0.15 with the
    // CMASK weighting (RFG-152) the weeds are the sparse accents they are in Reforger.
    static const float density = std::clamp(EnvFloat("WGR_CLUTTER_MESH_DENSITY", 0.15f), 0.0f, 4.0f);
    const bool active = radius > kHandover && land.GetEnfusionSurfaceCount() > 0 && !_clutterGeoCopy.empty() &&
                        std::any_of(std::begin(_clutterLayerModel), std::end(_clutterLayerModel),
                                    [](const std::string& m) { return !m.empty(); });
    const float sendRadius = active ? radius : 0.0f;
    if (sendRadius != _clutterMeshRadiusSent)
    {
        _clutterMeshRadiusSent = sendRadius;
        EngineWgpu::AcquireProducerWindow("clutter mesh radius");
        wgr_grass_set_clutter_mesh_radius(_renderer, sendRadius);
        LOG_INFO(Graphics, "Wgpu clutter meshes (RFG-153): {} (radius {:.0f} m, handover {:.0f} m, spacing {:.2f} m, "
                 "density {:.2f})", active ? "on" : "off", radius, kHandover, spacing, density);
    }
    if (_clutterMeshBakeEpoch != _clutterMeshSeenBake)
    {
        _clutterMeshSeenBake = _clutterMeshBakeEpoch;
        ResetNativeClutterMeshes();
    }
    if (!active || !std::isfinite(camera.X()) || !std::isfinite(camera.Z()))
    {
        if (!_clutterMeshCells.empty())
            ResetNativeClutterMeshes();
        return;
    }

    // One model per frame at most: a cold .xob costs tens of milliseconds through the ShapeBank.
    bool loadedThisFrame = false;
    const auto shapeFor = [&](int layer) -> LODShapeWithShadow*
    {
        const std::string& path = _clutterLayerModel[layer];
        if (path.empty() || _clutterMeshFailed.count(path))
            return nullptr;
        if (const auto hit = _clutterMeshShapes.find(path); hit != _clutterMeshShapes.end())
            return hit->second.GetRef();
        if (loadedThisFrame)
            return nullptr;
        loadedThisFrame = true;
        Ref<LODShapeWithShadow> shape = Shapes.New(RStringB(path.c_str()), false, true);
        if (!shape)
        {
            _clutterMeshFailed.insert(path);
            LOG_WARN(Graphics, "Wgpu clutter meshes: '{}' did not load; its plants stay cards", path);
            return nullptr;
        }
        _clutterMeshShapes.emplace(path, shape);
        ++_clutterMeshShapeEpoch; // cells built without it are rebuilt below
        return shape.GetRef();
    };

    const int landRange = land.GetLandRange();
    const float landGrid = land.GetLandGrid();
    const float seaLevel = land.GetSeaLevel();
    const float camX = camera.X(), camZ = camera.Z();

    // Roads and hard-object footprints near the camera, refreshed when the camera has moved a
    // cell's worth: the full lists run to tens of thousands of entries.
    if (std::abs(camX - _clutterMeshExclX) > kCell || std::abs(camZ - _clutterMeshExclZ) > kCell ||
        land.GetGrassFootprints().size() != _clutterMeshFootprintsSeen)
    {
        _clutterMeshExclX = camX;
        _clutterMeshExclZ = camZ;
        _clutterMeshFootprintsSeen = land.GetGrassFootprints().size();
        _clutterMeshRoads.clear();
        _clutterMeshBoxes.clear();
        const float reach = radius + 2.0f * kCell + 30.0f;
        const std::vector<float>& roads = land.GetEnfusionRoadSegments();
        for (size_t i = 0; i + 4 < roads.size(); i += 5)
        {
            const float mx = 0.5f * (roads[i] + roads[i + 2]) - camX, mz = 0.5f * (roads[i + 1] + roads[i + 3]) - camZ;
            const float half = 0.5f * std::hypot(roads[i + 2] - roads[i], roads[i + 3] - roads[i + 1]);
            if (std::hypot(mx, mz) - half - roads[i + 4] < reach)
                _clutterMeshRoads.insert(_clutterMeshRoads.end(), roads.begin() + static_cast<ptrdiff_t>(i),
                                         roads.begin() + static_cast<ptrdiff_t>(i + 5));
        }
        const std::vector<float>& boxes = land.GetGrassFootprints();
        for (size_t i = 0; i + 5 < boxes.size(); i += 6)
            if (std::hypot(boxes[i] - camX, boxes[i + 1] - camZ) - std::max(boxes[i + 4], boxes[i + 5]) < reach)
                _clutterMeshBoxes.insert(_clutterMeshBoxes.end(), boxes.begin() + static_cast<ptrdiff_t>(i),
                                         boxes.begin() + static_cast<ptrdiff_t>(i + 6));
    }
    const auto excluded = [&](float x, float z)
    {
        for (size_t i = 0; i + 4 < _clutterMeshRoads.size(); i += 5)
        {
            const float ax = _clutterMeshRoads[i], az = _clutterMeshRoads[i + 1];
            const float bx = _clutterMeshRoads[i + 2], bz = _clutterMeshRoads[i + 3];
            const float dx = bx - ax, dz = bz - az;
            const float len2 = dx * dx + dz * dz;
            const float t = len2 > 0.0f ? std::clamp(((x - ax) * dx + (z - az) * dz) / len2, 0.0f, 1.0f) : 0.0f;
            const float px = ax + dx * t - x, pz = az + dz * t - z;
            if (px * px + pz * pz < _clutterMeshRoads[i + 4] * _clutterMeshRoads[i + 4])
                return true;
        }
        for (size_t i = 0; i + 5 < _clutterMeshBoxes.size(); i += 6)
        {
            const float rx = x - _clutterMeshBoxes[i], rz = z - _clutterMeshBoxes[i + 1];
            const float ux = _clutterMeshBoxes[i + 2], uz = _clutterMeshBoxes[i + 3];
            if (std::abs(rx * ux + rz * uz) < _clutterMeshBoxes[i + 4] &&
                std::abs(-rx * uz + rz * ux) < _clutterMeshBoxes[i + 5])
                return true;
        }
        return false;
    };

    const auto buildCell = [&](int cx, int cz, ClutterMeshCell& cell)
    {
        for (uint32_t id : cell.ids)
        {
            _engine.SceneClutterObjectRemoved(id);
            _clutterMeshFreeIds.push_back(id);
        }
        cell.ids.clear();
        cell.partial = false;
        cell.builtX = camX;
        cell.builtZ = camZ;
        cell.shapeEpoch = _clutterMeshShapeEpoch;
        const int perSide = std::max(1, static_cast<int>(std::lround(kCell / spacing)));
        const float step = kCell / static_cast<float>(perSide);
        for (int j = 0; j < perSide; ++j)
            for (int i = 0; i < perSide; ++i)
            {
                const uint32_t k = static_cast<uint32_t>(j * perSide + i);
                const float x = (static_cast<float>(cx) + (static_cast<float>(i) + Hash01(cx, cz, k, 11u)) / perSide) * kCell;
                const float z = (static_cast<float>(cz) + (static_cast<float>(j) + Hash01(cx, cz, k, 23u)) / perSide) * kCell;
                (void)step;
                const float d = std::hypot(x - camX, z - camZ);
                if (d > radius)
                    continue;
                // The mesh's share of the handover band; complement of card_yields_to_mesh.
                if (Hash01(cx, cz, k, 37u) >= 1.0f - Smoothstep(radius - kHandover, radius, d))
                    continue;
                const int gx = static_cast<int>(std::floor(x / landGrid));
                const int gz = static_cast<int>(std::floor(z / landGrid));
                if (gx < 0 || gz < 0 || gx >= landRange || gz >= landRange)
                    continue;
                const uint32_t geo = _clutterGeoCopy[static_cast<size_t>(gz) * landRange + gx];
                if ((geo & kGeoGrass) == 0 || (geo & kGeoAuthored) == 0 ||
                    (geo & (kGeoExcludeHard & ~0x00000c00u)) != 0 || (geo & kGeoExcludeSoft) != 0)
                    continue;
                const uint32_t first = (geo >> 23) & 31u;
                const uint32_t count = (geo >> 28) & 7u;
                if (count == 0 || ((_clutterCardSkip >> first) & 1u) || ((_clutterCardFull >> first) & 1u))
                    continue;
                const float coverage = static_cast<float>((geo >> 16) & 127u) * (1.0f / 127.0f);
                if (Hash01(cx, cz, k, 41u) >= coverage * density)
                    continue;
                // The run's plants at the map's own per-plant frequency (photo layer pick).
                float total = 0.0f;
                for (uint32_t r = 0; r < count; ++r)
                    total += std::max(_clutterLayerWeight[std::min<uint32_t>(first + r, 31u)], 0.0f);
                uint32_t layer = first;
                const float h = Hash01(cx, cz, k, 53u);
                if (total > 0.0f)
                {
                    float acc = 0.0f;
                    for (uint32_t r = 0; r < count; ++r)
                    {
                        acc += std::max(_clutterLayerWeight[std::min<uint32_t>(first + r, 31u)], 0.0f) / total;
                        layer = std::min<uint32_t>(first + r, 31u);
                        if (h < acc)
                            break;
                    }
                }
                else
                    layer = std::min<uint32_t>(first + std::min<uint32_t>(static_cast<uint32_t>(h * count), count - 1), 31u);
                if (_clutterLayerModel[layer].empty())
                    continue;
                LODShapeWithShadow* shape = shapeFor(static_cast<int>(layer));
                if (shape == nullptr)
                {
                    if (!_clutterMeshFailed.count(_clutterLayerModel[layer]))
                        cell.partial = true;
                    continue;
                }
                if (excluded(x, z))
                    continue;
                float dX = 0.0f, dZ = 0.0f;
                const float y = land.SurfaceY(x, z, &dX, &dZ);
                if (y <= seaLevel + 0.35f || 1.0f / std::sqrt(1.0f + dX * dX + dZ * dZ) < 0.70f)
                    continue;
                const float mid = _clutterLayerScale[layer][0] > 0.0f ? _clutterLayerScale[layer][0] : 1.0f;
                const float spread = _clutterLayerScale[layer][1];
                const float scale = mid * (1.0f + spread * (Hash01(cx, cz, k, 67u) * 2.0f - 1.0f));
                Matrix4 transform;
                transform.SetRotationY(Hash01(cx, cz, k, 79u) * 6.2831853f);
                transform.SetScale(scale);
                transform.SetPosition(Vector3(x, y, z));
                uint32_t id;
                if (!_clutterMeshFreeIds.empty())
                {
                    id = _clutterMeshFreeIds.back();
                    _clutterMeshFreeIds.pop_back();
                }
                else
                    id = kIdBase + _clutterMeshNextId++;
                if (_engine.SceneClutterObjectCreated(id, shape, transform))
                    cell.ids.push_back(id);
                else
                    _clutterMeshFreeIds.push_back(id);
            }
    };

    const int ccx = static_cast<int>(std::floor(camX / kCell));
    const int ccz = static_cast<int>(std::floor(camZ / kCell));
    const int reachCells = static_cast<int>(std::ceil(radius / kCell)) + 1;
    const float cellHalfDiag = kCell * 0.7072f;
    // Drop cells that left the ring.
    for (auto it = _clutterMeshCells.begin(); it != _clutterMeshCells.end();)
    {
        const int cx = static_cast<int>(it->first >> 32);
        const int cz = static_cast<int>(static_cast<int32_t>(it->first & 0xffffffffu));
        const float d = std::hypot((cx + 0.5f) * kCell - camX, (cz + 0.5f) * kCell - camZ);
        if (d - cellHalfDiag > radius)
        {
            for (uint32_t id : it->second.ids)
            {
                _engine.SceneClutterObjectRemoved(id);
                _clutterMeshFreeIds.push_back(id);
            }
            it = _clutterMeshCells.erase(it);
        }
        else
            ++it;
    }
    // Build new cells; rebuild handover-band cells once the camera has moved a metre past their
    // build, and partial cells once a model they wanted has loaded. Bounded per frame.
    int built = 0;
    for (int dz = -reachCells; dz <= reachCells && built < 12; ++dz)
        for (int dx = -reachCells; dx <= reachCells && built < 12; ++dx)
        {
            const int cx = ccx + dx, cz = ccz + dz;
            const float d = std::hypot((cx + 0.5f) * kCell - camX, (cz + 0.5f) * kCell - camZ);
            if (d - cellHalfDiag > radius)
                continue;
            const int64_t key = (static_cast<int64_t>(cx) << 32) | static_cast<uint32_t>(cz);
            auto found = _clutterMeshCells.find(key);
            if (found == _clutterMeshCells.end())
            {
                ClutterMeshCell cell;
                buildCell(cx, cz, cell);
                _clutterMeshCells.emplace(key, std::move(cell));
                ++built;
                continue;
            }
            ClutterMeshCell& cell = found->second;
            const bool inBand = d + cellHalfDiag > radius - kHandover;
            const bool moved = std::hypot(cell.builtX - camX, cell.builtZ - camZ) > 1.0f;
            if ((inBand && moved) || (cell.partial && cell.shapeEpoch != _clutterMeshShapeEpoch))
            {
                buildCell(cx, cz, cell);
                ++built;
            }
        }

    if (++_clutterMeshLogTick % 600 == 0)
    {
        size_t instances = 0;
        for (const auto& entry : _clutterMeshCells)
            instances += entry.second.ids.size();
        LOG_INFO(Graphics, "Wgpu clutter meshes: {} cells, {} plant instances, {} models loaded, {} failed",
                 _clutterMeshCells.size(), instances, _clutterMeshShapes.size(), _clutterMeshFailed.size());
    }
}

} // namespace Poseidon
