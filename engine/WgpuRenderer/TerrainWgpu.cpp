#include "TerrainWgpu.hpp"
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/SnowShelter.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/MudGroundAdmission.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/SunlightDrying.hpp>
#include <Poseidon/World/Entities/Infantry/UniformWetness.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/TerrainSatmap.hpp>

#include "CdlodDriver.hpp"
#include "RainWaterPublication.hpp"
#include "SandUploadReceipt.hpp"
#include "EngineWgpu.hpp"
#include "TextureWgpu.hpp"
#include "TextureBankWgpu.hpp"

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Rendering/Frame/PresentationSnapshot.hpp> // RenderSnapshot S1: the frame clock
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Core/TaskPool.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
// Grid-mesh resolution per axis; must match GRID_N in terrain/mod.rs.
constexpr int TerrainGridN = 32;

namespace
{
bool TerrainPageTimingsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_TERRAIN_PAGE_TIMINGS");
        return value && value[0] == '1';
    }();
    return enabled;
}
using TerrainPageClock = std::chrono::steady_clock;
double TerrainPageElapsedMs(TerrainPageClock::time_point start)
{
    return std::chrono::duration<double, std::milli>(TerrainPageClock::now() - start).count();
}
} // namespace


// ---------------------------------------------------------------------------
// The geography word uploaded to the grass system (one R32Uint texel per land
// cell). Mirrored by GEO_* in grass/grass.wgsl -- change both together.
//
//   bits  0-14  GeographyInfo::packed, verbatim from Landscape (AITypes.hpp):
//               0-1 waterDepth, 2 full, 3 forestInner, 4 forestOuter, 5 road,
//               6 track, 7 slow, 8-9 howManyObjects, 10-11 howManyHardObjects,
//               12-14 gradient.
//   bit  15     GrassAuthoredCell -- this cell's clutter answer came from the
//               map's OWN per-texel LCA surface mask, so the two fields below
//               are meaningful. Clear on every OFP world and on any cell whose
//               material carries no authored mask; there the legacy per-layer
//               texture classification set bit 31 and these fields are 0.
//   bits 16-22  GrassCoverage, 0..127. The cell's authored clutter density,
//               scaled so the densest clutter character on this world is 127.
//               This is what makes Takistan's grass (character total 0.94) and
//               its desert (0.12) different fields rather than the same lawn.
//   bits 23-27  GrassSurfaceId, 0..30, with 31 = none/unclassified. A dense id
//               for the surface that dominates the cell. NOTHING consumes it
//               yet: it is what per-surface clutter VARIETY selects on, and it
//               costs nothing to carry now that the mask is being decoded.
//   bits 28-30  reserved, always 0.
//   bit  31     GrassTextureCell -- this cell can grow grass at all.
// ---------------------------------------------------------------------------
// Aliases: the values themselves live in TerrainWgpu.hpp so the Enfusion clutter
// bake in its own translation unit writes exactly the same word.
constexpr uint32_t GrassTextureCell = GrassGeo::TextureCell;
constexpr uint32_t GrassAuthoredCell = GrassGeo::AuthoredCell;
constexpr uint32_t GrassCoverageShift = GrassGeo::CoverageShift;
constexpr uint32_t GrassCoverageMask = GrassGeo::CoverageMask;
constexpr uint32_t GrassSurfaceShift = GrassGeo::SurfaceShift;
constexpr uint32_t GrassSurfaceMask = GrassGeo::SurfaceMask;
constexpr uint32_t GrassRunCountShift = GrassGeo::RunCountShift;
constexpr uint32_t GrassRunCountMask = GrassGeo::RunCountMask;

// The world's clutter atlas. 32 layers because a surface's clutter classes must
// occupy a CONTIGUOUS run -- that is what lets the shader select one with
// `first + hash*count` instead of walking a bitmask -- so the budget is the SUM
// of the per-surface lists, not their distinct union: Stratis needs 28 slots
// over 21 distinct classes, Takistan 25 over 10. 32 is also exactly what the
// 5-bit `first` field can address, and 8 what the 3-bit `count` field can.
//
// 512 rather than 1024 square: this atlas feeds the MID ring only, the plates
// are 54-89% empty so a cropped sub-image carries what a whole 1024 plate did,
// and 32 layers at 512 is 42.7 MB against the 48 MB nine 1024 layers cost.
constexpr uint32_t GrassAtlasLayers = GrassGeo::AtlasLayers;
constexpr uint32_t GrassAtlasMaxRun = GrassGeo::AtlasMaxRun;
constexpr int GrassAtlasSize = GrassGeo::AtlasSize;
// Every bit this file writes on top of GeographyInfo, so a re-bake starts clean.
constexpr uint32_t GrassBakedMask =
    GrassTextureCell | GrassAuthoredCell | GrassCoverageMask | GrassSurfaceMask | GrassRunCountMask;

static bool ContainsNoCase(const char* text, const char* needle)
{
    if (text == nullptr || needle == nullptr || *needle == '\0')
    {
        return false;
    }
    const size_t needleLen = std::strlen(needle);
    for (const char* start = text; *start; ++start)
    {
        size_t i = 0;
        for (; i < needleLen && start[i]; ++i)
        {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(start[i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[i])));
            if (a != b)
            {
                break;
            }
        }
        if (i == needleLen)
        {
            return true;
        }
    }
    return false;
}

// CWR/OFP terrain assets are mostly named in English or Czech.  Keep this
// strict: an unclassified texture must not grow procedural grass over desert,
// dirt, concrete, or rock.
//
// This is the LAST resort. Where the map's own CfgSurfaces is readable it is the
// authority (TerrainSurfaceCatalog); this only answers for data that names no
// surface class at all, and for Arma archives that ship no config we can parse.
static bool IsGrassSurfaceName(const char* name)
{
    return ContainsNoCase(name, "grass") || ContainsNoCase(name, "trava") || ContainsNoCase(name, "louka") ||
           ContainsNoCase(name, "meadow") || ContainsNoCase(name, "pasture");
}

static bool IsGrassTexture(const Texture* texture)
{
    if (texture == nullptr)
    {
        return false;
    }
    return IsGrassSurfaceName(texture->Name());
}

static bool EqualsNoCase(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
    {
        return false;
    }
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b)))
        {
            return false;
        }
    }
    return *a == *b;
}

// Does this terrain texture resolve to a surface the GAME ITSELF calls grass?
//
// Operation Flashpoint's own CfgSurfaces answers this exactly, and names three
// grass classes and no others: Grass (`tn??????`), GrassAbel (`tt??????`) and
// GrassHigh (`tv??????`) -- measured over the retail CONFIG.BIN's 23 surface
// classes. Everon's terrain is pre-blended four-quadrant tiles of two-character
// codes, so `GetTextureQuadrants` gives a per-quadrant answer; a tile counts as
// grass-capable if any quadrant is one of the three.
//
// This exists because the name heuristic above matches NOTHING on Everon
// (`eden\tn`, `eden\pl`), which used to trip an "enable every layer" fallback and
// made sand, rock, forest floor and shore all grass-capable. `outResolved` reports
// whether the surface catalogue said anything at all about this texture, so that
// fallback can be kept for genuinely unclassifiable data instead of for data we
// simply were not reading.
static bool IsGrassSurfaceClass(const SurfaceInfo* const* quadrants, bool& outResolved)
{
    outResolved = false;
    if (quadrants == nullptr)
    {
        return false;
    }
    bool grass = false;
    for (int q = 0; q < 4; ++q)
    {
        const SurfaceInfo* surface = quadrants[q];
        if (surface == nullptr || surface->_class.GetLength() == 0)
        {
            continue;
        }
        outResolved = true;
        const char* cls = surface->_class.Data();
        if (EqualsNoCase(cls, "Grass") || EqualsNoCase(cls, "GrassAbel") || EqualsNoCase(cls, "GrassHigh"))
        {
            grass = true;
        }
    }
    return grass;
}

static float EnvFloat(const char* name, float fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return fallback;
    }
    return std::strtof(v, nullptr);
}

// The terrain's LCA mask is sampled once per land cell for grass placement. On
// Sahrani those cells are wide enough for a crisp 0-to-1 mask edge to read as a
// tiled lawn boundary. Keep the map's mask authoritative, but give the first
// ordinary-ground cell OUTSIDE an authored grass cell a sparse copy of its
// neighbour. The shader's stable coverage thinning turns that into a short,
// non-moving fringe rather than a new solid grass tile. Roads, water and hard
// object cells retain their exact exclusion, and we never fill a second cell.
//
// This is deliberately authored-mask-only: legacy worlds do not carry the
// surface/runs needed to preserve the correct local grass family.
static size_t FeatherAuthoredGrassEdges(std::vector<uint32_t>& geography, int width, float fraction)
{
    if (width < 3 || fraction <= 0.0f || geography.size() != static_cast<size_t>(width) * width)
    {
        return 0;
    }

    constexpr uint32_t HardExclusion = 0x00000c63u; // water, road/track and hard objects; mirrors grass.wgsl
    const std::vector<uint32_t> source = geography;
    size_t feathered = 0;
    for (int z = 1; z + 1 < width; ++z)
    {
        for (int x = 1; x + 1 < width; ++x)
        {
            const size_t index = static_cast<size_t>(z) * width + x;
            const uint32_t cell = source[index];
            if ((cell & GrassAuthoredCell) == 0 || (cell & HardExclusion) != 0)
            {
                continue;
            }

            // Fade the authored field's outermost full-density cell before
            // adding the sparse exterior fringe below. The two bands retain
            // the map's grass family but turn a 0/1 mask step into a visible
            // density ramp: dense field -> thinned edge -> sparse shoulder.
            if ((cell & GrassTextureCell) != 0)
            {
                bool atEdge = false;
                constexpr int dx[] = {-1, 1, 0, 0};
                constexpr int dz[] = {0, 0, -1, 1};
                for (int direction = 0; direction < 4; ++direction)
                {
                    const uint32_t neighbour = source[static_cast<size_t>(z + dz[direction]) * width + x + dx[direction]];
                    if ((neighbour & GrassTextureCell) == 0)
                    {
                        atEdge = true;
                        break;
                    }
                }
                if (atEdge)
                {
                    const uint32_t coverage = (cell & GrassCoverageMask) >> GrassCoverageShift;
                    const uint32_t softened = std::max(1u, static_cast<uint32_t>(std::lround(coverage * 0.68f)));
                    geography[index] = (cell & ~GrassCoverageMask) | ((softened << GrassCoverageShift) & GrassCoverageMask);
                    ++feathered;
                }
                continue;
            }

            uint32_t best = 0;
            uint32_t bestCoverage = 0;
            constexpr int dx[] = {-1, 1, 0, 0};
            constexpr int dz[] = {0, 0, -1, 1};
            for (int direction = 0; direction < 4; ++direction)
            {
                const uint32_t neighbour = source[static_cast<size_t>(z + dz[direction]) * width + x + dx[direction]];
                if ((neighbour & (GrassAuthoredCell | GrassTextureCell)) !=
                    (GrassAuthoredCell | GrassTextureCell))
                {
                    continue;
                }
                const uint32_t coverage = (neighbour & GrassCoverageMask) >> GrassCoverageShift;
                if (coverage > bestCoverage)
                {
                    best = neighbour;
                    bestCoverage = coverage;
                }
            }
            if (bestCoverage == 0)
            {
                continue;
            }

            const uint32_t coverage = std::max(1u, static_cast<uint32_t>(std::lround(bestCoverage * fraction)));
            uint32_t feather = cell & ~(GrassTextureCell | GrassCoverageMask | GrassSurfaceMask | GrassRunCountMask);
            feather |= GrassTextureCell;
            feather |= (coverage << GrassCoverageShift) & GrassCoverageMask;
            feather |= best & (GrassSurfaceMask | GrassRunCountMask);
            geography[index] = feather;
            ++feathered;
        }
    }
    return feathered;
}

// Modern worlds retain every authored placement even while their renderable
// Objects stream in around the camera. Roads therefore have a stable source
// before RoadNet/Object lists exist: the original placement rows and model
// table. Mark their hosting grass cells up front, so streamed `ca\\roads\\...`
// segments never receive blades. This is intentionally model-family-specific;
// it does not guess that ordinary hard scenery is a road.
static size_t ExcludeModernRoadPlacementsFromGrass(const Landscape& land, std::vector<uint32_t>& geography, int width,
                                                   size_t& roadPlacements)
{
    roadPlacements = 0;
    if (width < 1 || geography.size() != static_cast<size_t>(width) * width || land.GetLandGrid() <= 0.0f)
        return 0;

    const auto& models = land.ModernObjectModels();
    const auto& placements = land.ModernObjectPlacements();
    // Classify each model once, not the same path for every placement (over a
    // million on native Everon). Local to this upload, so no invalidation state.
    std::vector<bool> roadModels(models.size());
    for (size_t i = 0; i < models.size(); ++i)
        roadModels[i] = ContainsNoCase(models[i].Data(), "ca\\roads\\");
    if (std::none_of(roadModels.begin(), roadModels.end(), [](bool road) { return road; }))
        return 0;
    size_t excluded = 0;
    const float invGrid = land.GetInvLandGrid();
    for (const auto& placement : placements)
    {
        if (placement.modelIndex >= roadModels.size() || !roadModels[placement.modelIndex])
            continue;
        ++roadPlacements;
        const int x = std::clamp(static_cast<int>(std::floor(placement.rows[9] * invGrid)), 0, width - 1);
        const int z = std::clamp(static_cast<int>(std::floor(placement.rows[11] * invGrid)), 0, width - 1);
        uint32_t& cell = geography[static_cast<size_t>(z) * width + x];
        if ((cell & GrassTextureCell) != 0)
        {
            cell &= ~GrassTextureCell;
            ++excluded;
        }
    }
    return excluded;
}

static bool IsTerrainPageTextureName(const char* name)
{
    // These are world-tile assets, not general material textures. Restricting
    // eviction to this family avoids invalidating a retained object material
    // that happens to share a normal surface albedo with the ground.
    return ContainsNoCase(name, "\\layers\\") || ContainsNoCase(name, "/layers/") ||
           ContainsNoCase(name, "_lca.") || ContainsNoCase(name, "_lco.");
}

TerrainWgpu::TerrainWgpu(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer)
{
    _baseMult = EnvFloat("WGR_TERRAIN_LOD_BASE", 4.0f);
    _lodRatio = EnvFloat("WGR_TERRAIN_LOD_RATIO", 2.0f);
    _morphRegion = std::clamp(EnvFloat("WGR_TERRAIN_MORPH", 0.50f), 0.05f, 1.0f);
    // >1 extends terrain past the map edges (clamped edge heights) to complement the
    // infinite ocean; 1 = map only. Defaults to match the water extent (WGR_WATER_EXTENT)
    // so seabed underlies the whole transparent ocean with no seam at the old map edge.
    _extentFactor = std::max(1.0f, EnvFloat("WGR_TERRAIN_EXTENT", 3.0f));
    // Startup-only A/B control. Infinite-far wgpu water already covers this tree;
    // opaque terrain must remain beneath it and behind visible distant objects.
    _backgroundCoverage = EnvFloat("WGR_TERRAIN_BACKGROUND", 1.0f) != 0.0f;
    _terrainPageRadius = std::max(8, static_cast<int>(EnvFloat("WGR_TERRAIN_PAGE_RADIUS_CELLS", 64.0f)));
    _terrainPageLayerBudget = static_cast<uint32_t>(std::clamp(
        static_cast<int>(EnvFloat("WGR_TERRAIN_PAGE_LAYERS", 1024.0f)), 64,
        static_cast<int>(WGR_TERRAIN_MAX_GROUND_LAYERS)));
}

void TerrainWgpu::BuildQuadtree(const Landscape& land)
{
    const int range = land.GetTerrainRange();
    const float grid = land.GetTerrainGrid();

    // Optionally over-size + centre the root so land continues past the map edges. At
    // extent 1 this is exactly the map-only root at origin 0 (byte-identical selection).
    _extended = _extentFactor > 1.0f;
    const int coverage = _extended ? std::max(range, static_cast<int>(std::lround(_extentFactor * range))) : range;
    const int rootTexels = CdlodRootTexels(coverage, TerrainGridN);
    const int originTexel = _extended ? CdlodCenteredOrigin(rootTexels, range, TerrainGridN) : 0;

    // Each leaf's world-height extent is scanned from the heightmap (same per-texel
    // min/max the CDLOD selection frustum-tests against). Off-map texels clamp to the
    // nearest edge height — the vertex shader samples the heightmap the same clamped
    // way, so the extended border is a flat continuation of the boundary terrain.
    auto leafBounds = [&](int ox, int oz, int span, float& mn, float& mx)
    {
        CdlodHeightBounds(ox,oz,span,range,false,
            [&](int z,int x) { return land.GetHeight(z,x); },mn,mx);
    };
    BuildCdlodTree(rootTexels, originTexel, originTexel, grid, TerrainGridN, leafBounds, _tree, _rootIndex, _numLevels,
                   _leafSize);
    if (_rootIndex < 0)
    {
        return;
    }
    ComputeCdlodRanges(_leafSize * _baseMult, _lodRatio, _numLevels, _ranges);
    _treeMin = originTexel * grid;
    _treeMax = (originTexel + rootTexels) * grid;
}

bool TerrainWgpu::RainWaterSourceMatches(const WgrRainWaterSourceKey& key) const
{
    return _uploaded&&key.world_token==uint64_t(reinterpret_cast<uintptr_t>(_uploaded))&&
        _uploadedRange==int(key.terrain_range)&&_uploadedHeightRevision==key.height_revision&&
        _params.terrain_grid==key.terrain_spacing&&SameRainWaterSource(key,_rainWaterSource);
}

bool TerrainWgpu::UploadIfNeeded(const Landscape& land)
{
    const int range = land.GetTerrainRange();
    const char* name = land.GetName();
    const bool sameMap = _uploaded == &land && _uploadedRange == range && _uploadedName == name;
    const auto& rainWater=GRainWater();
    // New fine ownership/lifetime requires a REAL upload before its new source
    // receipt, even if a reused Landscape happened to retain the numeric key.
    const WgrRainWaterSourceKey requestedSource{uint64_t(reinterpret_cast<uintptr_t>(&land)),rainWater.Generation(),
        land.HeightRevision(),uint32_t(range),land.GetTerrainGrid()};
    // Coarse optics also require the actual world source, independently of fine ownership.
    const bool sourceEpochReady=SameRainWaterSource(_rainWaterSource,requestedSource);
    if (sameMap && _uploadedHeightRevision == land.HeightRevision()&&sourceEpochReady)
    {
        return false;
    }

    // Fill the static params (the coast wet-band fields are refreshed per frame in DrawTerrain).
    _params.world_origin = {0.0f, 0.0f};
    _params.land_grid = land.GetLandGrid();
    _params.terrain_grid = land.GetTerrainGrid();
    _params.hm_width = static_cast<uint32_t>(range);
    _params.hm_height = static_cast<uint32_t>(range);
    _params.land_range = static_cast<uint32_t>(land.GetLandRange());
    _params.data_scale = 1.0f;
    EngineWgpu::AcquireProducerWindow("terrain heightmap");
    // A proven new map invalidates source resources even if Landscape storage is reused.
    if (!sameMap) { const WgrRainWaterSourceKey emptySource{}; wgr_rain_water_set_source(_renderer,&emptySource); }
    // The FFI consumes/copies this slice synchronously. Borrowing the existing
    // heightfield avoids a 256 MiB temporary (plus STL alignment) on 8192 grids.
    wgr_terrain_set_heightmap(_renderer, land.HeightmapData(), &_params);
    LOG_INFO(Graphics, "Wgpu heightfield upload: {}x{} grid={}m data={}", range, range,
             land.GetTerrainGrid(), land.HeightmapData() != nullptr);
    _uploadedHeightRevision = land.HeightRevision();
    {
        _rainWaterSource={uint64_t(reinterpret_cast<uintptr_t>(&land)),rainWater.Generation(),
            _uploadedHeightRevision,uint32_t(range),land.GetTerrainGrid()};
        if(!land.HeightmapData())_rainWaterSource={};
        wgr_rain_water_set_source(_renderer,&_rainWaterSource);
    }
    if (sameMap)
    {
        // Experimental deformation changes geometry, not the material/grass atlas.
        BuildQuadtree(land);
        return false;
    }

    // Legacy worlds have small, reusable tile sets and keep the eager path.
    // Modern worlds name thousands of per-region LCA/LCO pages; their first
    // working set is selected below once DrawTerrain has the camera.
    _pagedGroundMaterials = false;
    for (int i = 0; i < land.GetNTextures(); ++i)
    {
        if (const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
            source != nullptr && source->surfaceCount != 0 && source->mask.GetLength() != 0)
        {
            _pagedGroundMaterials = true;
            break;
        }
    }
    _terrainPageCenterX = -0x3fffffff;
    _terrainPageCenterZ = -0x3fffffff;
    _terrainPageTestUpdate = 0;
    _terrainPageTestPhase = -1;
    if (!_pagedGroundMaterials)
    {
        UploadGroundTextures(land);
    }
    UploadIndexMap(land);
    UploadGeography(land);
    UploadJitterMap(land);
    UploadDetailNoise();

    BuildQuadtree(land);

    _uploaded = &land;
    _uploadedRange = range;
    _uploadedName = name;
    if (!sameMap) _puddleWetness.Reset();
    // Confirmed from the installed CWA world data: Eden is Everon. Its raw
    // GeographyInfo flags are not reliable grass exclusions, unlike the other
    // stock worlds, so the shader must rely on exact height/slope plus the
    // explicit surface controls for this map.
    _grassNeedsCompatibilityOverride = ContainsNoCase(name, "eden.wrp");
    if (_grassNeedsCompatibilityOverride)
    {
        LOG_WARN(Graphics, "Wgpu grass: enabling Everon/Eden legacy geography compatibility override");
    }
    return true;
}

void TerrainWgpu::UploadGroundTextures(const Landscape& land, const std::vector<int>* activeMaterials)
{
    const bool timings = TerrainPageTimingsEnabled();
    const auto timingStart = timings ? TerrainPageClock::now() : TerrainPageClock::time_point{};
    double loadMs = 0.0, ensureMs = 0.0;
    size_t loadCalls = 0, ensureCalls = 0;

    // A terrain material and a ground layer are different things. Modern worlds
    // can declare thousands of per-region material descriptors and page images;
    // only the camera working set receives its full authored composition. Every
    // other descriptor keeps a cheap shared surface albedo, so distant terrain
    // degrades to a plausible coarse material instead of white.
    const int materialCount = std::max(1, land.GetNTextures());
    const bool paged = activeMaterials != nullptr;
    const size_t layerLimit = paged ? _terrainPageLayerBudget : WGR_TERRAIN_MAX_GROUND_LAYERS;
    std::vector<TextureWgpu*> oldPageTextures = std::move(_terrainPageTextures);
    _terrainPageTextures.clear();

    std::vector<uint64_t> handles{0};
    std::vector<Texture*> boundTextures{nullptr};
    size_t dropped = 0;
    const auto bindLayer = [&](Texture* raw, bool terrainPage) -> uint32_t
    {
        if (raw == nullptr)
            return 0;
        const auto found = std::find(boundTextures.begin(), boundTextures.end(), raw);
        if (found != boundTextures.end())
        {
            if (terrainPage)
            {
                auto* page = static_cast<TextureWgpu*>(raw);
                if (std::find(_terrainPageTextures.begin(), _terrainPageTextures.end(), page) ==
                    _terrainPageTextures.end())
                    _terrainPageTextures.push_back(page);
            }
            return static_cast<uint32_t>(found - boundTextures.begin());
        }
        if (handles.size() >= layerLimit)
        {
            ++dropped;
            return 0;
        }
        auto* texture = static_cast<TextureWgpu*>(raw);
        const auto ensureStart = timings ? TerrainPageClock::now() : TerrainPageClock::time_point{};
        const uint64_t handle = texture->EnsureUploaded();
        if (timings) { ensureMs += TerrainPageElapsedMs(ensureStart); ++ensureCalls; }
        if (handle == 0)
            return 0;
        boundTextures.push_back(raw);
        handles.push_back(handle);
        if (terrainPage && std::find(_terrainPageTextures.begin(), _terrainPageTextures.end(), texture) ==
                               _terrainPageTextures.end())
            _terrainPageTextures.push_back(texture);
        return static_cast<uint32_t>(handles.size() - 1);
    };

    std::vector<WgrTerrainMaterial> materials(static_cast<size_t>(materialCount));
    _a3TerrainTextures.clear();
    const auto bindTexture = [&](const RStringB& name, uint32_t fallback, bool& resolved,
                                 bool terrainPage) -> uint32_t
    {
        resolved = false;
        if (name.GetLength() == 0)
            return fallback;
        const auto loadStart = timings ? TerrainPageClock::now() : TerrainPageClock::time_point{};
        Ref<Texture> texture = GlobLoadTexture(name);
        if (timings) { loadMs += TerrainPageElapsedMs(loadStart); ++loadCalls; }
        Texture* raw = texture;
        if (raw == nullptr)
            return fallback;
        _a3TerrainTextures.push_back(texture);
        const uint32_t layer = bindLayer(raw, terrainPage);
        if (layer == 0)
            return fallback;
        resolved = true;
        return layer;
    };

    // The eager compatibility path remains byte-for-byte in spirit: every
    // legacy tile enters the layer array. On modern worlds, bind only a small
    // set of distinct surface albedos as the far-field fallback.
    // RFG-065: on a natively loaded Enfusion world the palette entry's own `.emat` says
    // how its ground is tiled and what its middle-distance map is. Built once per world;
    // the table is empty everywhere else and the loop below is then unchanged.
    BuildEnfusionGroundMaterials(land);
    // Every actual terrain material receives ground puddles, independent of
    // source names or residency. Preserve semantic soft-soil metadata separately
    // for contact deformation; unknown soil is not promoted to soft mud.
    size_t puddleMaterials = 0;
    for (int i = 0; i < materialCount; ++i)
    {
        WgrTerrainMaterial& material = materials[static_cast<size_t>(i)];
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
        if (source != nullptr && source->surfaceCount != 0)
        {
            for (uint32_t slot = 0; slot < std::min<uint32_t>(source->surfaceCount, WGR_TERRAIN_SURFACE_SLOTS); ++slot)
                if (TerrainPuddles::EligibleSurface(source->surfaces[slot].Data()))
                    material.puddle_flags |= 1u << (8u + slot);
        }
        else if (static_cast<size_t>(i) < _enfusionGround.size())
        {
            // Native palette .emat semantic identity was classified while loading;
            // a generic inherited BCRMap must never promote grass/unknown to mud.
            if (_enfusionGround[static_cast<size_t>(i)].puddleEligible)
                material.puddle_flags = TerrainPuddles::SoftSoil;
        }
        else
        {
            const SurfaceInfo* const* quadrants = land.GetTextureQuadrants(i);
            bool allMud = quadrants != nullptr;
            for (int q = 0; allMud && q < 4; ++q)
                allMud = quadrants[q] != nullptr &&
                    TerrainPuddles::EligibleLegacySurface(quadrants[q]->_class.Data(), quadrants[q]->_name.Data(),
                                                         quadrants[q]->_soundEnv.Data(), quadrants[q]->_character.Data());
            if (allMud) material.puddle_flags = TerrainPuddles::SoftSoil;
            const Texture* tile = land.GetTexture(i);
            bool cultivated = quadrants != nullptr && tile != nullptr;
            for (int q = 0; cultivated && q < 4; ++q)
                cultivated = quadrants[q] != nullptr && StockNogovaMudSoil(land.GetName(), tile->GetName().Data(),
                    quadrants[q]->_class.Data(), quadrants[q]->_name.Data(), quadrants[q]->_soundEnv.Data(),
                    quadrants[q]->_character.Data(), 0.5f, 0.5f);
            if (cultivated) material.puddle_flags |= 4u; // shader enforces the actual world-cell interior
        }
        material.puddle_flags |= TerrainPuddles::GroundReceiver;
        ++puddleMaterials;
    }
    LOG_INFO(Graphics, "Wgpu terrain puddles: {} of {} source materials eligible (all ground; soft-soil metadata remains separate)",
             puddleMaterials, materialCount);
    // An opt-in installed-test fixture comes from this world's real material
    // grid. Pick a 3x3 ground patch, away from native road ribbons and the coast,
    // with the shader's exact central-difference flatness criterion. Only log a
    // pose; do not teleport the owner or change terrain/weather/profile state.
    if (!paged && puddleMaterials > 0 && EnvFloat("WGR_TERRAIN_PUDDLE_FIXTURE", 0.0f) > 0.5f)
    {
        const int range = land.GetLandRange();
        bool found = false;
        for (int z = 1; z + 1 < range && !found; ++z)
            for (int x = 1; x + 1 < range && !found; ++x)
            {
                bool patch = true;
                for (int dz = -1; patch && dz <= 1; ++dz)
                    for (int dx = -1; patch && dx <= 1; ++dx)
                    {
                        const int id = land.GetTexture(z + dz, x + dx);
                        patch = id >= 0 && id < materialCount &&
                            (materials[static_cast<size_t>(id)].puddle_flags & 1u) != 0u &&
                            !land.GetEnfusionRoadCell(z + dz, x + dx);
                    }
                if (!patch) continue;
                const float east = (x + 0.5f) * land.GetLandGrid();
                const float north = (z + 0.5f) * land.GetLandGrid();
                const float h = land.SurfaceY(east, north);
                const float step = land.GetTerrainGrid();
                const float dx = (land.SurfaceY(east + step, north) - land.SurfaceY(east - step, north)) / (2.0f * step);
                const float dz = (land.SurfaceY(east, north + step) - land.SurfaceY(east, north - step)) / (2.0f * step);
                if (h <= land.GetSeaLevel() + 4.5f || 1.0f / std::sqrt(1.0f + dx * dx + dz * dz) < 0.9995f)
                    continue;
                const int id = land.GetTexture(z, x);
                LOG_INFO(Graphics, "Wgpu terrain puddle fixture: freefly {:.2f} {:.2f} {:.2f} 0 -18; material={} "
                         "source={} (CPU candidate, visual acceptance pending)",
                         east, north - 12.0f, h + 5.0f, id, land.GetEnfusionSurface(id));
                found = true;
            }
        if (!found) LOG_INFO(Graphics, "Wgpu terrain puddle fixture: no fully eligible flat 3x3 patch found");
    }
    if (!paged)
    {
        for (int i = 0; i < materialCount; ++i)
        {
            WgrTerrainMaterial& material = materials[static_cast<size_t>(i)];
            const EnfusionGroundMaterial* enf = static_cast<size_t>(i) < _enfusionGround.size()
                                                    ? &_enfusionGround[static_cast<size_t>(i)]
                                                    : nullptr;
            // The Enfusion detail image REPLACES the layer the loader bound, because that
            // one has a single mip level (CreateDynamic) and is unusable once the tiling
            // goes from the 12.5 m land cell to the authored few metres. It is still the
            // `legacy` slot, so with the dev switch off the ground is the same image at
            // the same UV as before -- only mipped.
            Texture* colour = enf != nullptr && enf->detail != nullptr ? enf->detail : land.GetTexture(i);
            material.legacy = bindLayer(colour, false);
            material.satellite = material.legacy;
            if (colour != nullptr && enf == nullptr && land.GetA3TerrainMaterial(i) == nullptr)
            {
                auto* bank = static_cast<TextureBankWgpu*>(_engine.TextBank());
                Ref<Texture> normal = bank->LoadLegacyEnhancementNormal(colour->Name());
                if (normal.GetRef() != nullptr)
                {
                    material.tile_normal = bindLayer(normal.GetRef(), false);
                    if (material.tile_normal != 0)
                    {
                        _a3TerrainTextures.push_back(normal);
                        LOG_INFO(Graphics, "Wgpu legacy terrain: {} enhanced by {}", (const char*)colour->Name(),
                                 (const char*)normal->Name());
                        Ref<Texture> detail = bank->LoadLegacyTerrainDetailNormal(colour->Name(), material.legacy_detail_scale);
                        if (detail.GetRef() != nullptr)
                        {
                            material.legacy_detail_normal = bindLayer(detail.GetRef(), false);
                            if (material.legacy_detail_normal != 0)
                            {
                                _a3TerrainTextures.push_back(detail);
                                LOG_INFO(Graphics, "Wgpu legacy terrain detail: {} -> {} repeats/metre={}",
                                         (const char*)colour->Name(), (const char*)detail->Name(), material.legacy_detail_scale);
                            }
                        }
                    }
                }
            }
            if (enf != nullptr && enf->detail != nullptr)
            {
                material.enfusion = 1;
                material.detail_scale = enf->detailScale;
                material.middle_scale = enf->middleScale;
                material.middle_blend = enf->middleBlend;
                material.detail_max = enf->detailMax;
                material.detail_fade = enf->detailFade;
                for (int c = 0; c < 3; ++c)
                    material.middle_color[c] = enf->middleColor[c];
                if (enf->middle != nullptr)
                    material.middle = bindLayer(enf->middle, false);
                if (enf->normal != nullptr)
                    material.tile_normal = bindLayer(enf->normal, false);
            }
        }
    }
    else
    {
        for (int i = 0; i < materialCount; ++i)
        {
            const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
            if (source == nullptr)
                continue;
            for (uint32_t slot = 0; slot < source->surfaceCount; ++slot)
            {
                if (source->surfaces[slot].GetLength() == 0)
                    continue;
                bool resolved = false;
                const uint32_t fallback = bindTexture(source->surfaces[slot], 0, resolved, false);
                if (resolved)
                {
                    materials[static_cast<size_t>(i)].legacy = fallback;
                    materials[static_cast<size_t>(i)].satellite = fallback;
                    break;
                }
            }
        }
    }

    std::vector<int> eagerIds;
    if (!paged)
    {
        eagerIds.resize(static_cast<size_t>(materialCount));
        for (int i = 0; i < materialCount; ++i)
            eagerIds[static_cast<size_t>(i)] = i;
    }
    const std::vector<int>& ids = paged ? *activeMaterials : eagerIds;
    size_t authored = 0;
    size_t complete = 0;
    for (const int id : ids)
    {
        if (id < 0 || id >= materialCount)
            continue;
        WgrTerrainMaterial& material = materials[static_cast<size_t>(id)];
        if (paged)
        {
            const uint32_t detailedFallback = bindLayer(land.GetTexture(id), true);
            if (detailedFallback != 0)
            {
                material.legacy = detailedFallback;
                material.satellite = detailedFallback;
            }
        }
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(id);
        if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
            continue;
        ++authored;
        const uint32_t fallback = material.legacy;
        bool satelliteResolved = false;
        bool maskResolved = false;
        material.satellite = bindTexture(source->satellite, fallback, satelliteResolved, paged);
        material.mask = bindTexture(source->mask, material.satellite, maskResolved, paged);
        bool normalResolved = false;
        material.tile_normal = bindTexture(source->tileNormal, 0, normalResolved, false);
        const auto setUvSource = [&](uint32_t slot, const A3TerrainUv& uv)
        {
            if (uv.worldPos)
                material.uv_source_mask |= 1u << slot;
        };
        setUvSource(0, source->satelliteUv);
        setUvSource(1, source->maskUv);
        std::copy(source->satelliteUv.u.begin(), source->satelliteUv.u.end(), material.satellite_uv.u);
        std::copy(source->satelliteUv.v.begin(), source->satelliteUv.v.end(), material.satellite_uv.v);
        std::copy(source->maskUv.u.begin(), source->maskUv.u.end(), material.mask_uv.u);
        std::copy(source->maskUv.v.begin(), source->maskUv.v.end(), material.mask_uv.v);
        material.surface_count = source->surfaceCount;
        bool anySurface = false;
        for (uint32_t slot = 0; slot < material.surface_count; ++slot)
        {
            bool surfaceResolved = false;
            material.surfaces[slot] = bindTexture(source->surfaces[slot], 0, surfaceResolved, false);
            material.surface_normals[slot] =
                bindTexture(source->surfaceNormals[slot], 0, normalResolved, false);
            anySurface = anySurface || surfaceResolved;
            setUvSource(2 + slot, source->surfaceUvs[slot]);
            std::copy(source->surfaceUvs[slot].u.begin(), source->surfaceUvs[slot].u.end(),
                      material.surface_uvs[slot].u);
            std::copy(source->surfaceUvs[slot].v.begin(), source->surfaceUvs[slot].v.end(),
                      material.surface_uvs[slot].v);
        }
        (void)satelliteResolved;
        if (maskResolved && anySurface)
            ++complete;
    }

    Ref<Texture> previousSatmap = _automaticSatmap;
    if (!paged && authored == 0 && _enfusionGround.empty())
    {
        _automaticSatmap = BuildAutomaticSatmap(land);
        if (_automaticSatmap)
        {
            const uint32_t slot = bindLayer(_automaticSatmap, false);
            if (slot != 0)
                for (auto& material : materials)
                {
                    material.satellite = slot;
                    material._pad0 = 1; // Generated legacy far albedo, not an authored LCA.
                }
        }
    }
    else _automaticSatmap = nullptr;

    // Replace the terrain bind group before destroying pages referenced by the
    // previous group. Shared texture views then release cleanly on the Rust side.
    const double descriptorTotalMs = timings ? TerrainPageElapsedMs(timingStart) : 0.0;
    const auto acquireStart = timings ? TerrainPageClock::now() : TerrainPageClock::time_point{};
    EngineWgpu::AcquireProducerWindow("terrain ground layers");
    const double acquireMs = timings ? TerrainPageElapsedMs(acquireStart) : 0.0;
    const auto bindStart = timings ? TerrainPageClock::now() : TerrainPageClock::time_point{};
    wgr_terrain_set_ground_layers(_renderer, handles.data(), static_cast<uint32_t>(handles.size()));
    wgr_terrain_set_materials(_renderer, materials.data(), static_cast<uint32_t>(materials.size()));
    const double bindMs = timings ? TerrainPageElapsedMs(bindStart) : 0.0;
    const auto evictionStart = timings ? TerrainPageClock::now() : TerrainPageClock::time_point{};
    size_t evicted = 0;
    for (TextureWgpu* old : oldPageTextures)
    {
        if (old != nullptr && std::find(boundTextures.begin(), boundTextures.end(), old) == boundTextures.end() &&
            IsTerrainPageTextureName(old->Name()))
        {
            old->EvictGpu();
            ++evicted;
        }
    }

    if (timings)
    {
        const double evictionMs = TerrainPageElapsedMs(evictionStart);
        // Disjoint synchronous CPU phases; EnsureUploaded contains its own
        // reads/decodes/uploads and internal producer windows. acquireMs only
        // measures the explicit final terrain acquire, never GPU elapsed time.
        LOG_INFO(Graphics,
            "Wgpu terrain page timings: mode={} descriptors={} candidates={} layers={} evictions={} "
            "loadCalls={} ensureCalls={} descriptorMs={:.3f} loadOwnerMs={:.3f} ensureOwnerMs={:.3f} "
            "finalAcquireMs={:.3f} bindMs={:.3f} evictionMs={:.3f} uploadTotalMs={:.3f}",
            paged ? "paged" : "eager", materialCount, ids.size(), handles.size(), evicted,
            loadCalls, ensureCalls, std::max(0.0, descriptorTotalMs - loadMs - ensureMs), loadMs, ensureMs,
            acquireMs, bindMs, evictionMs, TerrainPageElapsedMs(timingStart));
    }
    LOG_INFO(Graphics,
             "Wgpu terrain material pages: mode={} candidates={} authored={} complete={} layers={}/{} dropped={} "
             "evicted={}",
             paged ? "paged" : "eager", ids.size(), authored, complete, handles.size(), layerLimit, dropped,
             evicted);
}

void TerrainWgpu::UploadIndexMap(const Landscape& land)
{
    const int n = land.GetLandRange();
    if (n <= 0)
    {
        return;
    }
    // A cell names a terrain material, not a ground layer. The material array is a
    // storage buffer sized to the world's material count; the ground-layer cap
    // applies to the layer each material points at, which UploadGroundTextures
    // keeps in range. Clamping here to the layer cap silently collapsed every
    // material past it onto one texture -- on Takistan, 2,269 of 3,293.
    // CELL_LAYER_MASK reserves bit 15, so the material index must still fit 15 bits.
    const int maxMaterial = std::min(std::max(1, land.GetNTextures()), 0x8000) - 1;
    std::vector<uint16_t> indices(static_cast<size_t>(n) * n);
    for (int z = 0; z < n; z++)
    {
        for (int x = 0; x < n; x++)
        {
            // Same (col=x, row=z) orientation as the heightmap upload;
            // GetTexture(z, x) == GetTex(x, z) is the land cell's layer index.
            // Bit 15 marks non-simple (transition) textures, which map exactly
            // once onto their cell instead of tiling — the GL33 path's
            // ClampU|ClampV (see Landscape::ClampFlags).
            const int layer = std::clamp(land.GetTexture(z, x), 0, maxMaterial);
            uint16_t entry = static_cast<uint16_t>(layer);
            if (!land.TextureIsSimple(layer))
            {
                entry |= 0x8000;
            }
            indices[static_cast<size_t>(z) * n + x] = entry;
        }
    }
    EngineWgpu::AcquireProducerWindow("terrain index map");
    wgr_terrain_set_index_map(_renderer, static_cast<uint32_t>(n), static_cast<uint32_t>(n), indices.data());
}

static bool StartsWithNoCase(const char* text, const char* prefix)
{
    if (text == nullptr || prefix == nullptr || *prefix == '\0')
    {
        return false;
    }
    for (; *prefix != '\0'; ++text, ++prefix)
    {
        if (*text == '\0')
        {
            return false;
        }
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(*text)));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(*prefix)));
        // Archive prefixes and texture paths disagree about the separator often
        // enough that treating them as different characters loses whole banks.
        a = (a == '/') ? '\\' : a;
        b = (b == '/') ? '\\' : b;
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

// Read one file out of the virtual filesystem (PBO or loose). Empty on failure;
// every caller here treats "absent" as a fallback, never as an error.
static std::vector<uint8_t> ReadVfsFile(const char* path)
{
    std::vector<uint8_t> bytes;
    QIFStreamB file;
    file.AutoOpen(path);
    if (file.fail() || file.rest() <= 0)
    {
        return bytes;
    }
    bytes.resize(static_cast<size_t>(file.rest()));
    file.read(bytes.data(), static_cast<int>(bytes.size()));
    return bytes;
}

namespace
{
constexpr int ObjectSatSize = 64;
struct ObjectSatView
{
    Vector3 lo = VZero, hi = VZero;
    std::vector<uint8_t> rgba;
    std::vector<float> height;
    std::array<float,4> average{};
};

// Temporary per-bake caches only. No extra GPU resources or per-frame object scan.
void BakeSatmapObjects(const Landscape& land, int size, std::vector<uint8_t>& pixels)
{
    const char* setting = std::getenv("WGR_OFP_SATMAP_OBJECTS");
    if (setting && std::strcmp(setting,"0") == 0) return;
    const auto started = std::chrono::steady_clock::now();
    std::unordered_map<const LODShapeWithShadow*,ObjectSatView> views;
    std::unordered_map<const Texture*,DecodedImage> textures;
    std::vector<float> top(size*size, -1e30f);
    constexpr size_t MaxModels = 2048, MaxTextures = 2048;
    const float metresPerPixel = land.GetLandGrid()*land.GetLandRange()/size;
    size_t stamped = 0, missing = 0, samples = 0;
    const auto textureImage = [&](Texture* texture) -> const DecodedImage* {
        if (!texture) return nullptr;
        if (const auto it = textures.find(texture); it != textures.end()) return &it->second;
        if (textures.size() >= MaxTextures) return nullptr;
        auto& dst = textures[texture];
        const char* source = static_cast<TextureWgpu*>(texture)->SourceName();
        const auto bytes = ReadVfsFile(source);
        if (bytes.empty()) return &dst;
        const auto src = DecodePAABuffer(bytes.data(),bytes.size(),!ContainsNoCase(source,".pac"));
        if (!src.valid()) return &dst;
        dst.width = dst.height = 64;
        dst.rgba.resize(64*64*4);
        if (!stbir_resize_uint8_linear(src.rgba.data(),src.width,src.height,src.width*4,
                                      dst.rgba.data(),64,64,64*4,STBIR_RGBA)) dst = {};
        return &dst;
    };
    for (int z=0; z<land.GetLandRange(); ++z) for (int x=0; x<land.GetLandRange(); ++x)
    {
        const auto& objects = land.GetObjects(z,x);
        for (int i=0; i<objects.Size(); ++i)
        {
            Object* object = objects[i];
            if (!object || (object->GetType() != Primary && object->GetType() != Network)) continue;
            auto* lod = object->GetShape();
            if (!lod) continue;
            auto found = views.find(lod);
            if (found == views.end())
            {
                if (views.size() >= MaxModels) {++missing; continue;}
                auto& view = views[lod];
                Shape* shape = nullptr;
                for (int l=0; l<lod->NLevels(); ++l)
                    if (lod->IsNormalLevel(l)) {shape=lod->Level(l); break;}
                if (shape)
                {
                    view.lo=shape->Min(); view.hi=shape->Max();
                    const float sx=view.hi.X()-view.lo.X(), sz=view.hi.Z()-view.lo.Z();
                    if (sx>0.05f && sz>0.05f && sx<1000 && sz<1000 && shape->NFaces()<100000)
                    {
                        view.rgba.assign(ObjectSatSize*ObjectSatSize*4,0);
                        view.height.assign(ObjectSatSize*ObjectSatSize,-1e30f);
                        for (Offset f=shape->BeginFaces(); f<shape->EndFaces(); shape->NextFace(f))
                        {
                            const Poly& face=shape->Face(f);
                            const auto* image=textureImage(face.GetTexture());
                            if (!image || !image->valid()) continue;
                            for (int t=1; t+1<face.N(); ++t)
                            {
                                const int ids[3]={face.GetVertex(0),face.GetVertex(t),face.GetVertex(t+1)};
                                std::array<float,2> p[3]; float h[3]; UVPair uv[3];
                                for (int k=0;k<3;++k) {
                                    const auto& pos=shape->Pos(ids[k]);
                                    p[k]={(pos.X()-view.lo.X())/sx*ObjectSatSize,(pos.Z()-view.lo.Z())/sz*ObjectSatSize};
                                    h[k]=pos.Y(); uv[k]=shape->UV(ids[k]);
                                }
                                const int left=std::clamp(int(std::floor(std::min({p[0][0],p[1][0],p[2][0]}))),0,ObjectSatSize-1);
                                const int right=std::clamp(int(std::ceil(std::max({p[0][0],p[1][0],p[2][0]}))),0,ObjectSatSize-1);
                                const int low=std::clamp(int(std::floor(std::min({p[0][1],p[1][1],p[2][1]}))),0,ObjectSatSize-1);
                                const int high=std::clamp(int(std::ceil(std::max({p[0][1],p[1][1],p[2][1]}))),0,ObjectSatSize-1);
                                for(int v=low;v<=high;++v) for(int u=left;u<=right;++u) {
                                    std::array<float,3> w;
                                    if(!SatmapTriangleWeights(p[0],p[1],p[2],u+0.5f,v+0.5f,w)) continue;
                                    const int at=v*ObjectSatSize+u;
                                    const float height=w[0]*h[0]+w[1]*h[1]+w[2]*h[2];
                                    if(height<=view.height[at]) continue;
                                    float tu=0,tv=0;
                                    for(int k=0;k<3;++k){tu+=w[k]*uv[k].u;tv+=w[k]*uv[k].v;}
                                    if(!std::isfinite(tu)||!std::isfinite(tv)) continue;
                                    const int tx=std::clamp(int((tu-std::floor(tu))*64),0,63);
                                    const int ty=std::clamp(int((tv-std::floor(tv))*64),0,63);
                                    const auto* texel=&image->rgba[(ty*64+tx)*4];
                                    if(texel[3]<128) continue;
                                    std::copy_n(texel,3,&view.rgba[at*4]);
                                    view.rgba[at*4+3]=255; view.height[at]=height;
                                }
                            }
                        }
                        float covered=0;
                        for(size_t p=0;p<view.height.size();++p) if(view.rgba[p*4+3]) {
                            ++covered;
                            for(int c=0;c<3;++c)view.average[c]+=view.rgba[p*4+c];
                        }
                        if(covered>0)for(int c=0;c<3;++c)view.average[c]/=covered;
                        view.average[3]=covered/(ObjectSatSize*ObjectSatSize);
                    }
                }
                found=views.find(lod);
            }
            const auto& view=found->second;
            if(view.rgba.empty() || view.average[3]<=0){++missing;continue;}
            const Vector3 origin=object->PositionModelToWorld(VZero);
            const Vector3 ax=object->PositionModelToWorld(Vector3(1,0,0))-origin;
            const Vector3 az=object->PositionModelToWorld(Vector3(0,0,1))-origin;
            const float det=ax.X()*az.Z()-az.X()*ax.Z();
            if(std::abs(det)<0.01f)continue;
            float lx=1e30f,lz=1e30f,hx=-1e30f,hz=-1e30f;
            for(int c=0;c<4;++c){
                const auto p=object->PositionModelToWorld(Vector3(c&1?view.hi.X():view.lo.X(),0,c&2?view.hi.Z():view.lo.Z()));
                lx=std::min(lx,p.X()/metresPerPixel);hx=std::max(hx,p.X()/metresPerPixel);
                lz=std::min(lz,p.Z()/metresPerPixel);hz=std::max(hz,p.Z()/metresPerPixel);
            }
            if(hx<0||hz<0||lx>=size||lz>=size)continue;
            const bool tiny=(hx-lx<1.0f || hz-lz<1.0f);
            const int left=std::clamp(int(std::floor(lx)),0,size-1),right=std::clamp(int(std::floor(hx)),0,size-1);
            const int low=std::clamp(int(std::floor(lz)),0,size-1),high=std::clamp(int(std::floor(hz)),0,size-1);
            ++stamped;
            for(int py=low;py<=high;++py)for(int px=left;px<=right;++px){
                float alpha=1; std::array<float,3> rgb; float height=origin.Y()+view.hi.Y();
                if(tiny){
                    alpha=std::max(0.0f,std::min(float(px+1),hx)-std::max(float(px),lx))
                        *std::max(0.0f,std::min(float(py+1),hz)-std::max(float(py),lz))*view.average[3];
                    std::copy_n(view.average.begin(),3,rgb.begin());
                }else{
                    const float dx=(px+0.5f)*metresPerPixel-origin.X(),dz=(py+0.5f)*metresPerPixel-origin.Z();
                    const float u=((dx*az.Z()-dz*az.X())/det-view.lo.X())/(view.hi.X()-view.lo.X());
                    const float v=((dz*ax.X()-dx*ax.Z())/det-view.lo.Z())/(view.hi.Z()-view.lo.Z());
                    if(u<0||u>=1||v<0||v>=1)continue;
                    const int at=int(v*ObjectSatSize)*ObjectSatSize+int(u*ObjectSatSize);
                    if(!view.rgba[at*4+3])continue;
                    for(int c=0;c<3;++c)rgb[c]=view.rgba[at*4+c];
                    height=origin.Y()+view.height[at];
                }
                const int at=py*size+px;
                if(alpha<=0 || (!tiny && height<top[at]))continue;
                if(!tiny)top[at]=height;
                for(int c=0;c<3;++c)pixels[at*4+c]=uint8_t(std::clamp(pixels[at*4+c]*(1-alpha)+rgb[c]*alpha,0.0f,255.0f));
                ++samples;
            }
        }
    }
    LOG_INFO(Graphics,"OFP satmap objects: {} models, {} textures, {} instances, {} samples, {} skipped; {:.1f} ms",
             views.size(),textures.size(),stamped,samples,missing,
             std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
}
}

Ref<Texture> TerrainWgpu::BuildAutomaticSatmap(const Landscape& land)
{
    const char* enabled = std::getenv("WGR_OFP_SATMAP");
    const int count = land.GetNTextures(), range = land.GetLandRange();
    const int size = TerrainSatmapResolution(count, range);
    if ((enabled && std::strcmp(enabled,"0") == 0) || size == 0)
        return nullptr;
    const auto started = std::chrono::steady_clock::now();
    constexpr int tileSize = 16;
    using Tile = std::array<uint8_t,tileSize*tileSize*4>;
    std::vector<Tile> tiles(static_cast<size_t>(count));
    int decoded = 0;
    for (int i=0; i<count; ++i)
    {
        Texture* texture = land.GetTexture(i);
        if (!texture) return nullptr;
        const auto* wgpuTexture = static_cast<const TextureWgpu*>(texture);
        const char* sourceName = wgpuTexture->SourceName();
        const auto bytes = ReadVfsFile(sourceName);
        if (bytes.empty())
        {
            LOG_WARN(Graphics,"OFP automatic satmap skipped: missing source {}",texture->Name());
            return nullptr;
        }
        auto image = DecodePAABuffer(bytes.data(),bytes.size(),!ContainsNoCase(sourceName,".pac"));
        // Sinkhole W5: a world whose ground textures are loose PNG/JPG (the converted Forest island) decodes them
        // here too. Only reached when the PAA/PAC decode failed AND the source is a .png/.jpg, so the stock
        // worlds (all PAA/PAC) bake exactly as before.
        if (!image.valid() && (ContainsNoCase(sourceName,".png") || ContainsNoCase(sourceName,".jpg")))
        {
            int w=0,h=0,channels=0;
            if (stbi_uc* rgba = stbi_load_from_memory(bytes.data(),static_cast<int>(bytes.size()),&w,&h,&channels,4))
            {
                image.width=w; image.height=h;
                image.rgba.assign(rgba,rgba+static_cast<size_t>(w)*h*4);
                stbi_image_free(rgba);
            }
        }
        if (!image.valid())
        {
            LOG_WARN(Graphics,"OFP automatic satmap skipped: decode failed {}",texture->Name());
            return nullptr; // Never bake white missing tiles into a whole island.
        }
        if (!stbir_resize_uint8_linear(image.rgba.data(),image.width,image.height,image.width*4,
                                      tiles[i].data(),tileSize,tileSize,tileSize*4,STBIR_RGBA)) return nullptr;
        ++decoded;
    }
    const auto sample = [&](int cx,int cz,float x,float z)
    {
        const int id = std::clamp(land.GetTexture(std::clamp(cz,0,range-1),std::clamp(cx,0,range-1)),0,count-1);
        const bool clampTile = land.ClampFlags(id) != NoClamp;
        float u = clampTile ? std::clamp(x-cx,0.0f,1.0f) : x-std::floor(x);
        float v = clampTile ? std::clamp(z-cz,0.0f,1.0f) : z-std::floor(z);
        const float px = u*tileSize-0.5f, pz = v*tileSize-0.5f;
        const int ix = static_cast<int>(std::floor(px)), iz = static_cast<int>(std::floor(pz));
        const float fx = px-ix, fz = pz-iz;
        std::array<float,3> color{};
        for (int j=0; j<2; ++j) for (int i=0; i<2; ++i)
        {
            const int tx = clampTile ? std::clamp(ix+i,0,tileSize-1) : (ix+i+tileSize)%tileSize;
            const int tz = clampTile ? std::clamp(iz+j,0,tileSize-1) : (iz+j+tileSize)%tileSize;
            const float weight = (i ? fx : 1-fx)*(j ? fz : 1-fz);
            for (int c=0; c<3; ++c) color[c] += tiles[id][(tz*tileSize+tx)*4+c]*weight;
        }
        return color;
    };
    std::vector<uint8_t> pixels(size*size*4);
    for (int z=0; z<size; ++z) for (int x=0; x<size; ++x)
    {
        const auto color = SampleTerrainSatmap((x+0.5f)*range/size,(z+0.5f)*range/size,sample);
        for (int c=0; c<3; ++c) pixels[(z*size+x)*4+c] = static_cast<uint8_t>(std::clamp(color[c]+0.5f,0.0f,255.0f));
        pixels[(z*size+x)*4+3] = 255;
    }
    BakeSatmapObjects(land,size,pixels);
    // Explicit diagnostic only: inspect the baked albedo without live 3D objects.
    if (const char* dump = std::getenv("WGR_OFP_SATMAP_DUMP"); dump && *dump)
    {
        const bool written = PNGWriter::WriteRGBA(dump,size,size,pixels.data());
        LOG_INFO(Graphics,"OFP satmap diagnostic {}: {}",written ? "written" : "failed",dump);
    }
    EngineWgpu::AcquireProducerWindow("automatic satellite albedo");
    Ref<Texture> result = _engine.TextBank()->CreateDynamic(size,size,pixels.data(),static_cast<uint32_t>(pixels.size()),true);
    LOG_INFO(Graphics,"OFP automatic satmap: {}x{} from {} tiles, {:.1f} ms, mipmapped ~{:.1f} MiB",size,size,decoded,
             std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),
             double(size)*size*4*4/3/(1024*1024));
    return result;
}

void TerrainWgpu::BuildSurfaceCatalog(const Landscape& land)
{
    _surfaceCatalog = TerrainSurfaceCatalog{};

    // Which archives can possibly define THIS world's surfaces? The ones that own
    // its terrain images, and every mounted archive they nest inside. Offering
    // every mounted archive instead would parse 487 configs on an Arma 3 install.
    //
    // Measured against the retail installs, this is exactly the right net, and it
    // is the IMAGE PATHS that carry it -- follow the references, do not scan:
    //
    //   Stratis   surfaces  a3\map_stratis\data\layers\p_*.rvmat ->
    //                       a3\map_data\gdt_strdrygrass_co.paa
    //             mask      a3\map_stratis\data\layers\m_000_000_lca.paa
    //             CfgSurfaces + CfgSurfaceCharacters are in a3\map_data\config.bin
    //             the `class clutter` bodies are in a3\map_stratis\config.bin,
    //             which nothing but the MASK path reaches -- no surface texture
    //             lives under a3\map_stratis\.
    //   Takistan  surfaces  ca\takistan\data\tk_trava_co.paa, and that one
    //             archive's config.bin holds all three classes.
    //
    // Bank prefixes are stored with a trailing separator and nest, so a single
    // reference collects every ancestor that is actually mounted: `a3\`,
    // `a3\map_stratis\`, `a3\map_stratis\data\`, `a3\map_stratis\data\layers\`.
    // That is at most a handful of configs -- about 150 KB of raP on Stratis --
    // rather than the whole addon set.
    std::vector<std::string> owners;
    const auto note = [&](const RStringB& reference)
    {
        if (reference.GetLength() == 0)
        {
            return;
        }
        for (int b = 0; b < GFileBanks.Size(); ++b)
        {
            const RString prefix = GFileBanks[b].GetPrefix();
            if (prefix.GetLength() == 0 || !StartsWithNoCase(reference.Data(), prefix.Data()))
            {
                continue;
            }
            std::string owner(prefix.Data());
            if (std::find(owners.begin(), owners.end(), owner) == owners.end())
            {
                owners.push_back(std::move(owner));
            }
        }
    };
    const int materialCount = std::max(0, land.GetNTextures());
    for (int i = 0; i < materialCount; ++i)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
        if (source == nullptr)
        {
            continue;
        }
        for (const RStringB& surface : source->surfaces)
        {
            note(surface);
        }
        // The mask, satellite and tile normal are the world's OWN images, so
        // they are what reaches the world's own addon -- which on Arma 3 is the
        // only archive carrying the `class clutter` bodies. Noting only the
        // surfaces found CfgSurfaces and missed every clutter model.
        note(source->mask);
        note(source->satellite);
        note(source->tileNormal);
    }
    // And the world by name, for a world mounted under its own addon rather than
    // loaded from a loose path.
    note(RStringB(land.GetName() ? land.GetName() : ""));

    // Escape hatch: point at extra configs without a rebuild when a world turns
    // out to declare its surfaces somewhere this rule does not reach.
    if (const char* extra = std::getenv("WGR_GRASS_SURFACE_CONFIGS"))
    {
        std::string list(extra);
        size_t start = 0;
        while (start <= list.size())
        {
            const size_t end = list.find(';', start);
            std::string one = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!one.empty())
            {
                const std::vector<uint8_t> bytes = ReadVfsFile(one.c_str());
                if (!bytes.empty() && _surfaceCatalog.AddConfig(bytes))
                {
                    LOG_INFO(Graphics, "Wgpu grass surfaces: added config '{}' (WGR_GRASS_SURFACE_CONFIGS)", one);
                }
                else
                {
                    LOG_WARN(Graphics, "Wgpu grass surfaces: could not read config '{}'", one);
                }
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
    }

    // A world referencing an unbounded number of addons must not turn world load
    // into a config parse. Nothing observed comes close to this.
    constexpr size_t MaxSurfaceConfigs = 16;
    int offered = 0, accepted = 0;
    std::string parsedNames;
    for (const std::string& owner : owners)
    {
        if (static_cast<size_t>(offered) >= MaxSurfaceConfigs)
        {
            LOG_WARN(Graphics, "Wgpu grass surfaces: stopping at {} configs; {} archives were candidates",
                     MaxSurfaceConfigs, owners.size());
            break;
        }
        // `QFBank::GetPrefix()` already ends with the separator -- BankList::Load
        // mounts with `prefix + NATIVE_DIR_STR`, and `AutoBank` strips exactly
        // that length before looking the remainder up inside the archive. Adding
        // another one produced `a3\map_data\\config.bin`, whose remainder was
        // `\config.bin`, which is not an entry in any PBO. That is why every
        // Arma world reported "0 offered a config" and ran on the name heuristic.
        std::string path = owner;
        if (!path.empty() && path.back() != '\\' && path.back() != '/')
        {
            path += '\\';
        }
        path += "config.bin";
        const std::vector<uint8_t> bytes = ReadVfsFile(path.c_str());
        if (bytes.empty())
        {
            continue;
        }
        ++offered;
        if (_surfaceCatalog.AddConfig(bytes))
        {
            ++accepted;
            if (!parsedNames.empty())
            {
                parsedNames += ", ";
            }
            parsedNames += path;
        }
    }
    LOG_INFO(Graphics,
             "Wgpu grass surfaces: {} archive(s) own or contain this world's terrain images, {} offered a config, "
             "{} parsed -> {} surfaces, {} characters, {} clutter classes [{}]",
             owners.size(), offered, accepted, _surfaceCatalog.SurfaceCount(), _surfaceCatalog.CharacterCount(),
             _surfaceCatalog.ClutterCount(), parsedNames.empty() ? "none" : parsedNames);
}

namespace
{
// One clutter class's card: the plate it draws from, and the sub-rect of that
// plate its own geometry uses.
struct ClutterCard
{
    std::string plate;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    bool valid = false;
};

// Read a clutter p3d and take the plate it names plus the UV rect its faces
// occupy.
//
// The UVs are the AUTHORED answer and the reason this reads the model at all.
// A shared plate carries several classes -- `c_papaver_ca.paa` is one 2048
// square image serving three poppy models -- and each model uses only its own
// corner. Connected-component labelling on alpha was the cheap alternative and
// it is wrong here for a measurable reason: that plate is only 54.7% clear, so
// its sub-images sit close enough to merge, and a merged card draws two plants
// on one quad, which reads as a rendering bug nobody can attribute.
//
// Both generations load: Operation Arrowhead clutter is ODOL 49 and Arma 3's is
// ODOL 73, and the narrow readers cover both.
ClutterCard ReadClutterCard(const char* modelPath, const std::vector<uint8_t>& bytes)
{
    ClutterCard card;
    if (bytes.empty())
    {
        return card;
    }
    Poseidon::Model::Model model;
    try
    {
        model = Poseidon::Asset::Formats::ODOLLoader::loadFromBuffer(reinterpret_cast<const char*>(bytes.data()),
                                                                     static_cast<int>(bytes.size()), modelPath);
    }
    catch (const std::exception&)
    {
        return card;
    }

    // The most detailed VISUAL lod that carries faces. Geometry/memory/roadway
    // lods have no usable UVs and a clutter p3d ships several of them.
    const Poseidon::Model::LODLevel* best = nullptr;
    for (const Poseidon::Model::LODLevel& lod : model.lodLevels)
    {
        if (lod.purpose != Poseidon::Model::LodPurpose::Visual)
        {
            continue;
        }
        if (lod.mesh.triangles.empty() && lod.mesh.quads.empty())
        {
            continue;
        }
        if (best == nullptr || lod.resolution < best->resolution)
        {
            best = &lod;
        }
    }
    if (best == nullptr)
    {
        return card;
    }

    int material = -1;
    for (size_t i = 0; i < best->mesh.materials.size(); ++i)
    {
        const std::string& texture = best->mesh.materials[i].texturePath;
        if (texture.empty())
        {
            continue;
        }
        // `_ca` is the cut-out plate, which is the one that carries the plant.
        if (ContainsNoCase(texture.c_str(), "_ca."))
        {
            material = static_cast<int>(i);
            break;
        }
        if (material < 0)
        {
            material = static_cast<int>(i);
        }
    }
    if (material < 0)
    {
        return card;
    }
    card.plate = best->mesh.materials[static_cast<size_t>(material)].texturePath;

    float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
    const auto accumulate = [&](uint32_t index)
    {
        if (index >= best->mesh.vertices.size())
        {
            return;
        }
        const Poseidon::Model::Vector2& uv = best->mesh.vertices[index].uv;
        u0 = std::min(u0, uv.u);
        u1 = std::max(u1, uv.u);
        v0 = std::min(v0, uv.v);
        v1 = std::max(v1, uv.v);
    };
    for (const Poseidon::Model::Triangle& tri : best->mesh.triangles)
    {
        if (static_cast<int>(tri.materialIndex) != material)
            continue;
        accumulate(tri.indices[0]);
        accumulate(tri.indices[1]);
        accumulate(tri.indices[2]);
    }
    for (const Poseidon::Model::Quad& quad : best->mesh.quads)
    {
        if (static_cast<int>(quad.materialIndex) != material)
            continue;
        for (int k = 0; k < 4; ++k)
            accumulate(quad.indices[k]);
    }
    // A degenerate or out-of-range box means the model tiles or mirrors its
    // plate rather than taking a sub-rect; the whole plate is then the honest
    // answer, and it is what the previous whole-plate behaviour did anyway.
    if (u1 - u0 < 0.02f || v1 - v0 < 0.02f || u0 < -0.01f || v0 < -0.01f || u1 > 1.01f || v1 > 1.01f)
    {
        u0 = 0.0f;
        v0 = 0.0f;
        u1 = 1.0f;
        v1 = 1.0f;
    }
    card.u0 = std::clamp(u0, 0.0f, 1.0f);
    card.v0 = std::clamp(v0, 0.0f, 1.0f);
    card.u1 = std::clamp(u1, 0.0f, 1.0f);
    card.v1 = std::clamp(v1, 0.0f, 1.0f);
    card.valid = true;
    return card;
}

// One authored surface slot's contribution, resolved once per material.
struct SlotClutter
{
    float coverage = 0.0f;  // authored, unnormalised (see Coverage())
    uint8_t layerFirst = 0; // first atlas layer this surface's clutter occupies
    uint8_t layerCount = 0; // how many follow it; 0 = no clutter atlas run
    bool present = false;   // false for a hole: the slot has no surface
};

// The WLD-019 decode, on the CPU. The mask is an INDEXED selector, not a weight
// field: RGB is strictly one-hot and alpha takes three authored levels. The three
// levels are read as a partition of unity so a filtered texel between two of them
// crossfades instead of decoding as a level nobody painted.
inline void DecodeMaskWeights(const uint8_t* texel, float out[A3TerrainMaterial::MaxSurfaces])
{
    const float r = texel[0] * (1.0f / 255.0f);
    const float g = texel[1] * (1.0f / 255.0f);
    const float b = texel[2] * (1.0f / 255.0f);
    const float a = texel[3] * (1.0f / 255.0f);
    const float group = std::clamp((a - 0.5f) * 2.0f, 0.0f, 1.0f);   // alpha 1.0 -> RGB slots
    const float levelFour = 1.0f - std::abs(a - 0.5f) * 2.0f;        // alpha 0.5 -> slot 4
    const float levelFive = std::clamp(1.0f - a * 2.0f, 0.0f, 1.0f); // alpha 0.0 -> slot 5
    const float painted = std::min(r + g + b, 1.0f);
    out[0] = group * (1.0f - painted);
    out[1] = group * r;
    out[2] = group * g;
    out[3] = group * b;
    out[4] = levelFour;
    out[5] = levelFive;
}

// terrain.wgsl's `terrain_uv`, on the CPU: the transform is a basis, so u is the
// first component of aside/up/dir and v the second (WLD-019 bug 4).
inline void MaskUv(const A3TerrainUv& uv, float wx, float wy, float wz, float tileU, float tileV, float& outU,
                   float& outV)
{
    const float sx = uv.worldPos ? wx : tileU;
    const float sy = uv.worldPos ? wy : tileV;
    const float sz = uv.worldPos ? wz : 0.0f;
    outU = sx * uv.u[0] + sy * uv.u[1] + sz * uv.u[2] + uv.u[3];
    outV = sx * uv.v[0] + sy * uv.v[1] + sz * uv.v[2] + uv.v[3];
}

inline int WrapTexel(float coord, int dim)
{
    // The mask is sampled through a repeating sampler on the GPU.
    float f = coord - std::floor(coord);
    int t = static_cast<int>(f * static_cast<float>(dim));
    return std::clamp(t, 0, dim - 1);
}
} // namespace

// Build the world's clutter atlas: one 512-square layer per clutter class, the
// classes of a surface laid down consecutively so the shader can pick one with
// `first + hash*count`.
//
// Layer 0 is reserved for the loose primary photo card, so a surface that
// resolves nothing degrades to exactly today's look rather than to a hole.
bool TerrainWgpu::BuildClutterAtlas(const std::vector<std::pair<std::string, RStringB>>& surfaces,
                                    std::unordered_map<std::string, std::pair<uint8_t, uint8_t>>& outRuns,
                                    std::vector<uint8_t>& outLayers, uint32_t& outLayerCount)
{
    constexpr size_t LayerBytes = static_cast<size_t>(GrassAtlasSize) * GrassAtlasSize * 4u;
    outLayers.assign(LayerBytes * GrassAtlasLayers, 0u);
    outRuns.clear();
    outLayerCount = 1; // layer 0 is the loose primary, filled by the caller

    // Per generation, the alpha-weighted mean colour of the opaque texels we
    // actually put in the atlas. Arma 3's plates are authored a generation apart
    // from Operation Arrowhead's, and if one reads darker this is the number that
    // says so -- measured from the pixels in hand rather than assumed, because
    // both generations ship the same DXT5 `_ca` container and the format cannot
    // tell them apart.
    double toneSum[2][3] = {{0, 0, 0}, {0, 0, 0}};
    double toneWeight[2] = {0, 0};
    int cards = 0, missingModel = 0, missingPlate = 0, dropped = 0;
    std::string report;

    // ---- Phase 1: plan the atlas, serially ----------------------------------
    //
    // Model reads stay on the calling thread: QFBank is not MT-safe (MT_SAFE is 0
    // in QBStream.hpp), the same constraint the mask bake works under. Only the
    // decode, crop and rescale are parallel, and those are where the 715 ms
    // measured on Stratis actually goes.
    struct CardJob
    {
        uint32_t layer = 0;
        size_t plate = 0;
        ClutterCard card;
    };
    std::vector<CardJob> jobs;
    std::vector<std::string> platePaths;
    std::unordered_map<std::string, size_t> plateIds;

    for (const auto& [surfaceKey, character] : surfaces)
    {
        if (character.GetLength() == 0)
        {
            continue;
        }
        std::vector<TerrainSurfaceCatalog::Clutter> classes = _surfaceCatalog.ClutterFor(character.Data());
        // Highest probability first, so a run truncated by the 8-layer cap keeps
        // the plants that actually cover the ground.
        std::stable_sort(classes.begin(), classes.end(),
                         [](const TerrainSurfaceCatalog::Clutter& a, const TerrainSurfaceCatalog::Clutter& b)
                         { return a.probability > b.probability; });

        const uint32_t first = outLayerCount;
        uint32_t count = 0;
        for (const TerrainSurfaceCatalog::Clutter& entry : classes)
        {
            if (count >= GrassAtlasMaxRun || outLayerCount >= GrassAtlasLayers)
            {
                ++dropped;
                continue;
            }
            if (entry.model.GetLength() == 0)
            {
                // A class the config names but whose `class clutter` body we never
                // saw -- Stratis's StrGrassDryMediumgroup, at p<=0.35 not a rare
                // one. Skipped rather than substituted: putting a different plant
                // in its slot would be a silent wrong answer, and the surface
                // still has its other classes. A surface that loses ALL of them
                // falls back to layer 0 below.
                ++missingModel;
                continue;
            }
            const std::vector<uint8_t> modelBytes = ReadVfsFile(entry.model.Data());
            const ClutterCard card = ReadClutterCard(entry.model.Data(), modelBytes);
            if (!card.valid || card.plate.empty())
            {
                ++missingModel;
                continue;
            }
            std::string plateKey = card.plate;
            std::transform(plateKey.begin(), plateKey.end(), plateKey.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            // One decode per PLATE, not per class: `c_papaver_ca.paa` serves three
            // classes and is 2048 square.
            auto plate = plateIds.find(plateKey);
            if (plate == plateIds.end())
            {
                plate = plateIds.emplace(plateKey, platePaths.size()).first;
                platePaths.push_back(card.plate);
            }
            jobs.push_back(CardJob{outLayerCount, plate->second, card});

            if (report.size() < 900)
            {
                char line[192];
                snprintf(line, sizeof(line), "%s%s@%u", report.empty() ? "" : ", ", entry.name.Data(), outLayerCount);
                report += line;
            }
            ++outLayerCount;
            ++count;
            ++cards;
        }
        // No usable class: point the surface at the primary card rather than
        // leaving a run the shader would read as "no clutter" and then thin.
        outRuns[surfaceKey] = count > 0 ? std::make_pair(static_cast<uint8_t>(first), static_cast<uint8_t>(count))
                                        : std::make_pair(static_cast<uint8_t>(0), static_cast<uint8_t>(1));
    }

    // ---- Phase 2: read every distinct plate, serially -----------------------
    std::vector<std::vector<uint8_t>> plateBytes(platePaths.size());
    std::vector<std::vector<size_t>> jobsByPlate(platePaths.size());
    size_t plateTotal = 0;
    for (size_t i = 0; i < platePaths.size(); ++i)
    {
        plateBytes[i] = ReadVfsFile(platePaths[i].c_str());
        plateTotal += plateBytes[i].size();
    }
    for (size_t j = 0; j < jobs.size(); ++j)
    {
        jobsByPlate[jobs[j].plate].push_back(j);
    }

    // ---- Phase 3: decode, crop and rescale, in parallel over PLATES ---------
    //
    // Over plates rather than cards, because the decode is the cost and cards
    // sharing a plate must not decode it twice. Every job writes only its own
    // layer and the layers are disjoint, so no job can race another. Tone is
    // accumulated per plate and reduced afterwards rather than through an atomic.
    struct PlateTone
    {
        double sum[3] = {0.0, 0.0, 0.0};
        double weight = 0.0;
        int generation = 0;
        bool decoded = false;
    };
    std::vector<PlateTone> plateTone(platePaths.size());
    const auto runPlates = [&](uint32_t begin, uint32_t end)
    {
        for (uint32_t i = begin; i < end; ++i)
        {
            if (plateBytes[i].empty() || jobsByPlate[i].empty())
            {
                continue;
            }
            const DecodedImage image = DecodePAABuffer(plateBytes[i].data(), plateBytes[i].size(), /*isPaa=*/true);
            if (!image.valid())
            {
                continue;
            }
            plateTone[i].decoded = true;
            plateTone[i].generation = StartsWithNoCase(platePaths[i].c_str(), "a3\\") ? 1 : 0;
            for (size_t j : jobsByPlate[i])
            {
                const ClutterCard& card = jobs[j].card;
                const int x0 =
                    std::clamp(static_cast<int>(card.u0 * static_cast<float>(image.width)), 0, image.width - 1);
                const int x1 = std::clamp(static_cast<int>(std::ceil(card.u1 * static_cast<float>(image.width))),
                                          x0 + 1, image.width);
                const int y0 =
                    std::clamp(static_cast<int>(card.v0 * static_cast<float>(image.height)), 0, image.height - 1);
                const int y1 = std::clamp(static_cast<int>(std::ceil(card.v1 * static_cast<float>(image.height))),
                                          y0 + 1, image.height);
                const uint8_t* src = image.rgba.data() + (static_cast<size_t>(y0) * image.width + x0) * 4u;
                uint8_t* dst = outLayers.data() + static_cast<size_t>(jobs[j].layer) * LayerBytes;
                stbir_resize_uint8_linear(src, x1 - x0, y1 - y0, image.width * 4, dst, GrassAtlasSize, GrassAtlasSize,
                                          GrassAtlasSize * 4, STBIR_RGBA);
                for (size_t t = 0; t < LayerBytes; t += 4)
                {
                    const double a = dst[t + 3] / 255.0;
                    if (a <= 0.5)
                        continue;
                    plateTone[i].sum[0] += dst[t + 0] * a;
                    plateTone[i].sum[1] += dst[t + 1] * a;
                    plateTone[i].sum[2] += dst[t + 2] * a;
                    plateTone[i].weight += a;
                }
            }
        }
    };
    TaskPool* pool = GetGlobalTaskPool();
    if (pool != nullptr && platePaths.size() > 1)
    {
        pool->ParallelFor(static_cast<uint32_t>(platePaths.size()), runPlates);
    }
    else
    {
        runPlates(0, static_cast<uint32_t>(platePaths.size()));
    }

    // ---- Phase 4: reduce ----------------------------------------------------
    for (size_t i = 0; i < plateTone.size(); ++i)
    {
        if (!plateTone[i].decoded)
        {
            missingPlate += static_cast<int>(jobsByPlate[i].size());
            cards -= static_cast<int>(jobsByPlate[i].size());
            continue;
        }
        const int g = plateTone[i].generation;
        toneSum[g][0] += plateTone[i].sum[0];
        toneSum[g][1] += plateTone[i].sum[1];
        toneSum[g][2] += plateTone[i].sum[2];
        toneWeight[g] += plateTone[i].weight;
    }
    LOG_INFO(Graphics, "Wgpu grass clutter plates: {} distinct, {:.1f} MB read, decoded on {} thread(s)",
             platePaths.size(), plateTotal / (1024.0 * 1024.0), pool ? pool->ThreadCount() : 1u);

    for (int generation = 0; generation < 2; ++generation)
    {
        if (toneWeight[generation] <= 0.0)
            continue;
        LOG_INFO(Graphics,
                 "Wgpu grass clutter tone ({}): alpha-weighted mean of opaque texels = ({:.3f}, {:.3f}, {:.3f})",
                 generation == 1 ? "Arma 3" : "Arma 2/OA", toneSum[generation][0] / toneWeight[generation] / 255.0,
                 toneSum[generation][1] / toneWeight[generation] / 255.0,
                 toneSum[generation][2] / toneWeight[generation] / 255.0);
    }
    LOG_INFO(Graphics,
             "Wgpu grass clutter atlas: {} cards over {} layers of {} ({} classes had no model, {} no readable plate, "
             "{} dropped at the {}-per-surface / {}-layer caps) [{}]",
             cards, outLayerCount, GrassAtlasLayers, missingModel, missingPlate, dropped, GrassAtlasMaxRun,
             GrassAtlasLayers, report.empty() ? "none" : report);
    return cards > 0;
}

// Taps per land-cell axis. 4x4 is the smallest grid that resolves a coverage
// FRACTION rather than a yes/no from one texel, which is what turns a mask edge
// into a thinning field instead of a new hard boundary one step finer than the
// old one. On Stratis a 4 m cell spans 4 mask texels, so this is a full cover.
constexpr int GrassMaskTapsPerAxis = 4;

bool TerrainWgpu::BakeAuthoredGrassMask(const Landscape& land, std::vector<uint32_t>& geography, bool refreshSelection)
{
    const int n = land.GetLandRange();
    const int materialCount = std::max(0, land.GetNTextures());
    if (n <= 0 || materialCount <= 0)
    {
        return false;
    }
    const size_t cells = static_cast<size_t>(n) * static_cast<size_t>(n);
    const std::string world = land.GetName() ? land.GetName() : "";

    // Applies the baked answer, filtered by the dev panel's per-material
    // selection. Separated from the bake itself so a live toggle costs a pass
    // over the cells rather than a decode of every mask on the map.
    const auto apply = [&]()
    {
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const size_t cell = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
                const uint32_t baked = _grassAuthoredCells[cell];
                if ((baked & GrassAuthoredCell) == 0)
                {
                    continue; // no authored mask here: the legacy answer stands
                }
                const int material = land.GetTexture(z, x);
                const bool selected = material >= 0 && material < static_cast<int>(_grassSurfaceEnabled.size()) &&
                                      _grassSurfaceEnabled[static_cast<size_t>(material)];
                uint32_t word = (geography[cell] & ~GrassBakedMask) | baked;
                if (!selected)
                {
                    word &= ~GrassTextureCell;
                }
                geography[cell] = word;
            }
        }
    };

    // ---- Resolve every material's slots to a clutter coverage ---------------
    //
    // The catalogue is the authority where it answers. Where it does not -- an
    // OFP world, or an Arma archive whose config we could not read -- the slot's
    // own texture NAME is the fallback, which is still a per-texel answer and so
    // still strictly better than the per-material name match it replaces.
    std::vector<std::array<SlotClutter, A3TerrainMaterial::MaxSurfaces>> slots(static_cast<size_t>(materialCount));
    std::unordered_map<std::string, float> coverageCache;
    // Distinct clutter-bearing surfaces, in first-seen order, with the character
    // each resolved to. This is what the atlas runs are laid down from, so the
    // order is deterministic for a given world.
    std::vector<std::pair<std::string, RStringB>> clutterSurfaces;
    float maxCoverage = 0.0f;
    int authoredMaterials = 0;
    int byCatalogue = 0, byName = 0;
    // Every distinct surface, with the character and coverage it resolved to.
    // This is the line to read when the question is "is the map's own config
    // reaching us": a world running on the name fallback reports every surface
    // at exactly 1.00 with no character, and one reading its config reports the
    // authored spread. `models` is how many of the character's clutter classes
    // found a `model=`, which is zero when the world's own addon is not mounted
    // even though CfgSurfaceCharacters was.
    std::string surfaceReport;
    for (int m = 0; m < materialCount; ++m)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(m);
        if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
        {
            continue;
        }
        ++authoredMaterials;
        for (unsigned char s = 0; s < source->surfaceCount && s < A3TerrainMaterial::MaxSurfaces; ++s)
        {
            const RStringB& surface = source->surfaces[s];
            if (surface.GetLength() == 0)
            {
                continue; // a hole keeps its slot but grows nothing
            }
            SlotClutter& slot = slots[static_cast<size_t>(m)][s];
            slot.present = true;
            std::string key(surface.Data());
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const auto cached = coverageCache.find(key);
            if (cached != coverageCache.end())
            {
                slot.coverage = cached->second;
            }
            else
            {
                float coverage = 0.0f;
                size_t models = 0, clutterCount = 0;
                const RStringB character = _surfaceCatalog.Character(surface.Data());
                if (character.GetLength() != 0)
                {
                    coverage = _surfaceCatalog.Coverage(character.Data());
                    for (const TerrainSurfaceCatalog::Clutter& entry : _surfaceCatalog.ClutterFor(character.Data()))
                    {
                        ++clutterCount;
                        models += entry.model.GetLength() != 0 ? 1 : 0;
                    }
                    if (coverage > 0.0f)
                    {
                        ++byCatalogue;
                    }
                }
                const bool authoredAnswer = coverage > 0.0f;
                if (coverage <= 0.0f && !_surfaceCatalog.Declares(surface.Data()) && IsGrassSurfaceName(surface.Data()))
                {
                    // No authored probability to scale by; treat it as a full
                    // field so a config-less install still gets its meadows.
                    // Only when the catalogue said NOTHING about the surface: one
                    // the map itself declares as `character = "Empty"` grows
                    // nothing, and guessing from its name would put the grass
                    // straight back where the map says there is none.
                    coverage = 1.0f;
                    ++byName;
                }
                if (coverage > 0.0f)
                {
                    clutterSurfaces.emplace_back(key, character);
                    char line[192];
                    snprintf(line, sizeof(line), "%s%s -> %s %.2f (%zu/%zu models)", surfaceReport.empty() ? "" : ", ",
                             key.c_str(), authoredAnswer ? character.Data() : "<by name>", coverage, models,
                             clutterCount);
                    surfaceReport += line;
                }
                coverageCache.emplace(key, coverage);
                slot.coverage = coverage;
            }
            if (slot.coverage > 0.0f)
            {
                maxCoverage = std::max(maxCoverage, slot.coverage);
            }
        }
    }
    if (authoredMaterials == 0)
    {
        return false; // every OFP world takes this exit before touching anything
    }
    if (maxCoverage <= 0.0f)
    {
        LOG_WARN(Graphics,
                 "Wgpu grass: {} authored materials but no surface on this world grows clutter; keeping the legacy "
                 "per-layer classification",
                 authoredMaterials);
        return false;
    }

    // The map's own plants. Built once per world, from the same surface list the
    // coverage came from, so a surface's atlas run and its density are two
    // statements about one authored character rather than two guesses.
    std::unordered_map<std::string, std::pair<uint8_t, uint8_t>> atlasRuns;
    const auto atlasStarted = std::chrono::steady_clock::now();
    const bool haveAtlas = BuildClutterAtlas(clutterSurfaces, atlasRuns, _clutterAtlas, _clutterAtlasLayers);
    const double atlasMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - atlasStarted).count();
    if (!haveAtlas)
    {
        // Nothing usable: drop the buffer so UploadGrassTuft keeps the loose
        // photo cards rather than uploading 32 blank layers.
        _clutterAtlas.clear();
        _clutterAtlasLayers = 0;
    }
    LOG_INFO(Graphics, "Wgpu grass clutter atlas: {:.0f} ms", atlasMs);
    // Attach each slot to its surface's run.
    for (int m = 0; m < materialCount; ++m)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(m);
        if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
        {
            continue;
        }
        for (unsigned char s = 0; s < source->surfaceCount && s < A3TerrainMaterial::MaxSurfaces; ++s)
        {
            SlotClutter& slot = slots[static_cast<size_t>(m)][s];
            if (!slot.present || slot.coverage <= 0.0f)
            {
                continue;
            }
            std::string key(source->surfaces[s].Data());
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const auto run = atlasRuns.find(key);
            if (run != atlasRuns.end())
            {
                slot.layerFirst = run->second.first;
                slot.layerCount = run->second.second;
            }
        }
    }

    // Re-derive the dev panel's per-material selection from the authored
    // surfaces. Before this it came from matching the material's slot-0 texture
    // NAME, which is the defect being fixed: 271 of Stratis's 479 materials read
    // as `gdt_strbeach` and grew nothing at all, whatever their mask painted.
    if (refreshSelection && static_cast<int>(_grassSurfaceEnabled.size()) == materialCount)
    {
        int selectable = 0;
        for (int m = 0; m < materialCount; ++m)
        {
            const A3TerrainMaterial* source = land.GetA3TerrainMaterial(m);
            if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
            {
                continue; // no authored mask: the legacy name/class answer stands
            }
            bool hasClutter = false;
            for (const SlotClutter& slot : slots[static_cast<size_t>(m)])
            {
                hasClutter = hasClutter || slot.coverage > 0.0f;
            }
            _grassSurfaceEnabled[static_cast<size_t>(m)] = hasClutter;
            selectable += hasClutter ? 1 : 0;
        }
        LOG_INFO(Graphics, "Wgpu grass surfaces: {} of {} authored materials carry a clutter-bearing surface",
                 selectable, materialCount);
    }

    // A live dev-panel toggle re-enters here with the same world; re-applying the
    // cached bake costs a pass over the cells instead of decoding every mask.
    if (_grassAuthoredWorld == world && _grassAuthoredCells.size() == cells)
    {
        apply();
        return true;
    }

    // ---- Group cells by the mask they resolve through -----------------------
    //
    // A mask serves every variant of its satellite tile -- Stratis has 479
    // materials over 64 masks, Takistan 3,293 over 220 -- so decoding per
    // material would decode each image seven to fifteen times over.
    std::unordered_map<std::string, int> maskIds;
    std::vector<std::string> maskPaths;
    std::vector<int> materialMask(static_cast<size_t>(materialCount), -1);
    for (int m = 0; m < materialCount; ++m)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(m);
        if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
        {
            continue;
        }
        std::string key(source->mask.Data());
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const auto found = maskIds.find(key);
        if (found != maskIds.end())
        {
            materialMask[static_cast<size_t>(m)] = found->second;
            continue;
        }
        const int id = static_cast<int>(maskPaths.size());
        maskIds.emplace(key, id);
        maskPaths.emplace_back(source->mask.Data());
        materialMask[static_cast<size_t>(m)] = id;
    }
    if (maskPaths.empty())
    {
        return false;
    }

    const size_t cellCount = cells;
    std::vector<uint32_t> cellsByMask(cellCount);
    std::vector<uint32_t> maskOffset(maskPaths.size() + 2, 0); // +1 for the "no mask" bucket
    {
        // Counting sort: one pass to count, one to place. 16 MB of indices on a
        // 2048-cell world, against 268 MB if every decoded mask were kept live.
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const int material = land.GetTexture(z, x);
                const int mask =
                    (material >= 0 && material < materialCount) ? materialMask[static_cast<size_t>(material)] : -1;
                ++maskOffset[static_cast<size_t>(mask + 1) + 1];
            }
        }
        for (size_t i = 1; i < maskOffset.size(); ++i)
        {
            maskOffset[i] += maskOffset[i - 1];
        }
        std::vector<uint32_t> cursor(maskOffset.begin(), maskOffset.end() - 1);
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const int material = land.GetTexture(z, x);
                const int mask =
                    (material >= 0 && material < materialCount) ? materialMask[static_cast<size_t>(material)] : -1;
                cellsByMask[cursor[static_cast<size_t>(mask + 1)]++] =
                    static_cast<uint32_t>(z) * static_cast<uint32_t>(n) + static_cast<uint32_t>(x);
            }
        }
    }

    // ---- Decode each mask once and answer its own cells ---------------------
    const float landGrid = land.GetLandGrid();
    const float originX = _params.world_origin.x;
    const float originZ = _params.world_origin.y;
    const float invMax = 1.0f / maxCoverage;
    std::atomic<uint32_t> decoded{0};
    std::atomic<uint32_t> missing{0};
    std::atomic<uint64_t> maskBytes{0};

    const auto processMask = [&](size_t maskIndex, const std::vector<uint8_t>& bytes)
    {
        const uint32_t begin = maskOffset[maskIndex + 1];
        const uint32_t end = maskOffset[maskIndex + 2];
        if (begin >= end)
        {
            return; // a mask nobody's cells use
        }
        const DecodedImage image = DecodePAABuffer(bytes.data(), bytes.size(), /*isPaa=*/true);
        if (!image.valid())
        {
            missing.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        decoded.fetch_add(1, std::memory_order_relaxed);
        const int mw = image.width;
        const int mh = image.height;
        const uint8_t* pixels = image.rgba.data();
        constexpr float tapStep = 1.0f / static_cast<float>(GrassMaskTapsPerAxis);
        for (uint32_t c = begin; c < end; ++c)
        {
            const uint32_t cell = cellsByMask[c];
            const int x = static_cast<int>(cell % static_cast<uint32_t>(n));
            const int z = static_cast<int>(cell / static_cast<uint32_t>(n));
            const int material = land.GetTexture(z, x);
            if (material < 0 || material >= materialCount)
            {
                continue;
            }
            const A3TerrainMaterial* source = land.GetA3TerrainMaterial(material);
            if (source == nullptr)
            {
                continue;
            }
            const auto& slotTable = slots[static_cast<size_t>(material)];
            const float height = land.GetHeight(z, x);
            float coverageSum = 0.0f;
            int histogram[A3TerrainMaterial::MaxSurfaces] = {0, 0, 0, 0, 0, 0};
            for (int tz = 0; tz < GrassMaskTapsPerAxis; ++tz)
            {
                for (int tx = 0; tx < GrassMaskTapsPerAxis; ++tx)
                {
                    const float fx = static_cast<float>(x) + (static_cast<float>(tx) + 0.5f) * tapStep;
                    const float fz = static_cast<float>(z) + (static_cast<float>(tz) + 0.5f) * tapStep;
                    const float wx = originX + fx * landGrid;
                    const float wz = originZ + fz * landGrid;
                    float u = 0.0f, v = 0.0f;
                    MaskUv(source->maskUv, wx, height, wz, fx, fz, u, v);
                    const uint8_t* texel = pixels + (static_cast<size_t>(WrapTexel(v, mh)) * static_cast<size_t>(mw) +
                                                     static_cast<size_t>(WrapTexel(u, mw))) *
                                                        4u;
                    float weights[A3TerrainMaterial::MaxSurfaces];
                    DecodeMaskWeights(texel, weights);
                    // Argmax over the slots this material actually filled. A hole
                    // must not win: the source says there is no surface there, and
                    // handing its weight to a neighbour is exactly the renumbering
                    // bug WLD-019 records.
                    int best = -1;
                    float bestWeight = 0.0f;
                    for (unsigned char s = 0; s < source->surfaceCount && s < A3TerrainMaterial::MaxSurfaces; ++s)
                    {
                        if (!slotTable[s].present)
                        {
                            continue;
                        }
                        if (best < 0 || weights[s] > bestWeight)
                        {
                            best = s;
                            bestWeight = weights[s];
                        }
                    }
                    if (best < 0)
                    {
                        continue;
                    }
                    coverageSum += slotTable[static_cast<size_t>(best)].coverage;
                    ++histogram[best];
                }
            }
            constexpr float taps = static_cast<float>(GrassMaskTapsPerAxis * GrassMaskTapsPerAxis);
            const float normalised = std::clamp((coverageSum / taps) * invMax, 0.0f, 1.0f);
            const uint32_t quantised = static_cast<uint32_t>(std::lround(normalised * 127.0f));
            // The dominant CLUTTER-bearing surface, not the dominant surface. A
            // cell that is mostly beach with a corner of dry grass grows dry
            // grass, so that is the variety its id has to name; the beach's id
            // would say nothing about the blades actually standing there.
            int dominant = -1;
            for (int s = 0; s < static_cast<int>(A3TerrainMaterial::MaxSurfaces); ++s)
            {
                if (histogram[s] <= 0 || slotTable[static_cast<size_t>(s)].layerCount == 0)
                {
                    continue;
                }
                if (dominant < 0 || histogram[s] > histogram[dominant])
                {
                    dominant = s;
                }
            }
            // The cell grows the plants its dominant clutter-bearing surface
            // declares. The run is contiguous, so this is two small fields and
            // the shader needs no table: see the geography word layout above.
            const uint32_t runFirst = dominant >= 0 ? slotTable[static_cast<size_t>(dominant)].layerFirst : 0u;
            const uint32_t runCount = dominant >= 0 ? slotTable[static_cast<size_t>(dominant)].layerCount : 0u;
            uint32_t word = GrassAuthoredCell;
            word |= (quantised << GrassCoverageShift) & GrassCoverageMask;
            word |= (runFirst << GrassSurfaceShift) & GrassSurfaceMask;
            word |= (runCount << GrassRunCountShift) & GrassRunCountMask;
            if (quantised > 0)
            {
                word |= GrassTextureCell;
            }
            _grassAuthoredCells[cell] = word;
        }
    };

    // I/O stays on the calling thread (QFBank is not MT-safe -- MT_SAFE is 0 in
    // QBStream.hpp); only the decode and the gather are parallel. Batching keeps
    // the compressed masks resident for one batch instead of all at once.
    const auto started = std::chrono::steady_clock::now();
    _grassAuthoredCells.assign(cellCount, 0u);
    TaskPool* pool = GetGlobalTaskPool();
    const size_t batchSize = std::max<size_t>(1, (pool ? pool->ThreadCount() : 1) * 2u);
    std::vector<std::vector<uint8_t>> batch;
    batch.reserve(batchSize);
    for (size_t first = 0; first < maskPaths.size(); first += batchSize)
    {
        const size_t last = std::min(first + batchSize, maskPaths.size());
        batch.clear();
        for (size_t i = first; i < last; ++i)
        {
            batch.push_back(ReadVfsFile(maskPaths[i].c_str()));
            maskBytes.fetch_add(batch.back().size(), std::memory_order_relaxed);
            if (batch.back().empty())
            {
                missing.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const auto run = [&](uint32_t begin, uint32_t end)
        {
            for (uint32_t i = begin; i < end; ++i)
            {
                if (!batch[i].empty())
                {
                    processMask(first + i, batch[i]);
                }
            }
        };
        if (pool != nullptr)
        {
            pool->ParallelFor(static_cast<uint32_t>(last - first), run);
        }
        else
        {
            run(0, static_cast<uint32_t>(last - first));
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    // Cells whose material carries no authored mask keep whatever the legacy
    // per-layer pass decided; they are reported so a partly-authored world is
    // visible as a number rather than as a look nobody can attribute.
    const uint32_t legacyCells = maskOffset[1];
    LOG_INFO(Graphics,
             "Wgpu grass mask bake: {} masks decoded ({} unreadable, {:.1f} MB read) for {} authored materials; "
             "{} surfaces grow clutter ({} from the map's config, {} by name), peak coverage {:.2f}; "
             "{} cells left on the legacy path; {:.0f} ms on {} thread(s)",
             decoded.load(), missing.load(), maskBytes.load() / (1024.0 * 1024.0), authoredMaterials,
             clutterSurfaces.size(), byCatalogue, byName, maxCoverage, legacyCells, ms,
             pool ? pool->ThreadCount() : 1u);
    LOG_INFO(Graphics, "Wgpu grass surface coverage: [{}]", surfaceReport.empty() ? "none" : surfaceReport);
    if (decoded.load() == 0)
    {
        // Nothing decoded: leave the legacy classification alone rather than
        // blanking a world's grass because its archives were not mounted.
        _grassAuthoredCells.clear();
        _grassAuthoredWorld.clear();
        return false;
    }
    _grassAuthoredWorld = world;
    apply();
    return true;
}

void TerrainWgpu::RefreshGeographyIfChanged(const Landscape& land)
{
    if (_uploaded != &land || land.GeographyRevision() == _geographyRevisionUploaded)
        return;
    const double nowMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - _geographyRefreshLastMs < 2000.0)
        return;
    _geographyRefreshLastMs = nowMs;
    LOG_INFO(Graphics, "RFG-101 geography re-upload: revision {} -> {}", _geographyRevisionUploaded, land.GeographyRevision());
    UploadGeography(land, true);
}

void TerrainWgpu::UploadGeography(const Landscape& land, bool reuseClutterDefinitions)
{
    const int n = land.GetLandRange();
    if (n <= 0)
    {
        return;
    }
    const int layerCount = std::max(0, land.GetNTextures());
    // RFG-062: a natively loaded Enfusion world names its palette entries by `.emat`
    // path. Its Textures are CreateDynamic handles with no name at all, so both the
    // legacy signals below classify every one of Everon's 22 surfaces as not-grass
    // ("0 of 22 layers grass-capable ... 1 layers the surface catalogue recognised"),
    // and the whole-world fallback does not fire because that stray 1 keeps
    // resolvedLayers non-zero. The native bake answers this world instead.
    _hasEnfusionSurfaces = land.GetEnfusionSurfaceCount() == layerCount && layerCount > 0;
    const auto layerName = [&](int i) -> std::string
    {
        if (_hasEnfusionSurfaces)
        {
            return land.GetEnfusionSurface(i);
        }
        return land.GetTexture(i) ? land.GetTexture(i)->Name() : "<null>";
    };
    bool layersChanged = static_cast<int>(_grassSurfaceNames.size()) != layerCount;
    if (!layersChanged)
    {
        for (int i = 0; i < layerCount; ++i)
        {
            if (_grassSurfaceNames[static_cast<size_t>(i)] != layerName(i))
            {
                layersChanged = true;
                break;
            }
        }
    }
    if (layersChanged && _hasEnfusionSurfaces)
    {
        // Every surface starts selectable; BakeEnfusionClutter narrows it to the ones
        // whose ClutterConfig actually resolved a plant.
        _grassSurfaceNames.clear();
        _grassSurfaceEnabled.assign(static_cast<size_t>(layerCount), true);
        for (int i = 0; i < layerCount; ++i)
        {
            _grassSurfaceNames.emplace_back(layerName(i));
        }
        LOG_INFO(Graphics, "Wgpu grass surfaces: {} native Enfusion surfaces; the legacy name/class classification is "
                           "skipped on this world",
                 layerCount);
    }
    else if (layersChanged)
    {
        _grassSurfaceNames.clear();
        _grassSurfaceEnabled.clear();
        _grassSurfaceNames.reserve(static_cast<size_t>(layerCount));
        _grassSurfaceEnabled.reserve(static_cast<size_t>(layerCount));
        // Two independent signals, because the generations classify differently.
        // The authored CfgSurfaces class is the authority wherever it resolves --
        // that is what covers OFP, whose texture names carry no semantics at all.
        // The name heuristic covers the Arma generations, whose surfaces resolve to
        // `Default` here (their own CfgSurfaces lives in a config.bin ParamFile
        // cannot parse) but whose texture names ARE semantic: Takistan's
        // `tk_trava_co` is matched by "trava".
        //
        // On an Arma-generation world both signals are only a SEED. They classify
        // a whole terrain material by the name of its slot-0 surface, which is
        // the cell's material for a whole land cell -- 32 m on Stratis. The
        // authored mask bake below replaces that answer per geography cell, and
        // re-derives this selection from the surfaces that actually grow clutter.
        int classGrass = 0, nameGrass = 0, resolvedLayers = 0;
        for (int i = 0; i < layerCount; ++i)
        {
            const Texture* texture = land.GetTexture(i);
            _grassSurfaceNames.emplace_back(texture ? texture->Name() : "<null>");
            bool resolved = false;
            const bool byClass = IsGrassSurfaceClass(land.GetTextureQuadrants(i), resolved);
            const bool byName = IsGrassTexture(texture);
            classGrass += byClass;
            nameGrass += byName;
            resolvedLayers += resolved;
            _grassSurfaceEnabled.push_back(byClass || byName);
        }

        // Fall back to every terrain layer ONLY when neither signal said anything
        // and the surface catalogue did not recognise a single layer -- i.e. data
        // we genuinely cannot classify. It used to fire on Everon, where the name
        // heuristic matches nothing, and made sand, rock, forest floor and shore
        // all grass-capable; the class signal now answers that world from its own
        // config instead. Water, roads, forests, cliffs and shore are still
        // rejected by the authoritative geography/slope checks in the grass
        // shader, and the Grass tab can still narrow the selection.
        const bool noneSelected = std::none_of(_grassSurfaceEnabled.begin(), _grassSurfaceEnabled.end(),
                                               [](bool enabled) { return enabled; });
        // ...and ALSO when the whole world is ONE terrain material. A converted Enfusion world
        // (Reforger's Everon) arrives as a single catch-all layer, which the catalogue happily
        // "recognises" as not-grass, so the guard above did not fire and the map grew nothing
        // at all (owner: "there is also no grass"; log: `0 of 1 layers grass-capable ... 1
        // layers the surface catalogue recognised`, `0 / 65536 surface cells`). One material
        // cannot say where the grass is; geography (water/road/forest/buildings) and slope in
        // the shader are the only per-cell signal left, and they still apply.
        const bool singleCatchAll = layerCount == 1;
        if (noneSelected && (resolvedLayers == 0 || singleCatchAll) && layerCount > 0)
        {
            std::fill(_grassSurfaceEnabled.begin(), _grassSurfaceEnabled.end(), true);
            LOG_INFO(Graphics,
                     "Wgpu grass: {} on this world; enabling all {} terrain layers",
                     singleCatchAll ? "a single catch-all terrain material" : "no surface class and no named grass",
                     layerCount);
        }
        LOG_INFO(Graphics,
                 "Wgpu grass surfaces: {} of {} layers grass-capable ({} by CfgSurfaces class, {} by name, {} layers "
                 "the surface catalogue recognised)",
                 std::count(_grassSurfaceEnabled.begin(), _grassSurfaceEnabled.end(), true), layerCount, classGrass,
                 nameGrass, resolvedLayers);
    }

    std::vector<bool> grassLayer(static_cast<size_t>(layerCount), false);
    std::string grassLayerNames;
    std::string allLayerNames;
    int grassLayerCount = 0;
    for (int i = 0; i < land.GetNTextures(); ++i)
    {
        if (!allLayerNames.empty())
        {
            allLayerNames += ", ";
        }
        allLayerNames += std::to_string(i);
        allLayerNames += ":";
        allLayerNames += land.GetTexture(i) ? land.GetTexture(i)->Name() : "<null>";
        grassLayer[static_cast<size_t>(i)] = _grassSurfaceEnabled[static_cast<size_t>(i)];
        if (grassLayer[static_cast<size_t>(i)])
        {
            if (!grassLayerNames.empty())
            {
                grassLayerNames += ", ";
            }
            // Null-guarded, unlike the original: a layer slot can carry no
            // texture at all, and the "no named grass" fallback above enables
            // every slot including those. On a world whose terrain materials do
            // not resolve, that made this line dereference null for layer 0 and
            // took the renderer down inside DrawGround. The same call is already
            // guarded where the names are first collected.
            grassLayerNames += land.GetTexture(i) ? land.GetTexture(i)->Name() : "<null>";
            ++grassLayerCount;
        }
    }

    _geographyRevisionUploaded = land.GeographyRevision(); // RFG-101
    std::vector<uint32_t> geography(static_cast<size_t>(n) * n);
    for (int z = 0; z < n; ++z)
    {
        for (int x = 0; x < n; ++x)
        {
            uint32_t cell = land.GetGeography(x, z).packed;
            const int layer = land.GetTexture(z, x);
            if (layer >= 0 && layer < static_cast<int>(grassLayer.size()) && grassLayer[static_cast<size_t>(layer)])
            {
                cell |= GrassTextureCell;
            }
            geography[static_cast<size_t>(z) * n + x] = cell;
        }
    }

    // The map's own answer, where the map has one. OFP worlds have no authored
    // terrain materials at all, so `BakeAuthoredGrassMask` returns false before
    // touching a byte and everything below runs on the array just built.
    //
    // The catalogue is skipped outright on those worlds rather than merely going
    // unused. An OFP `config.bin` is raP version 2 with a different layout from
    // the version-0 container `ArmaRapReader` reads, so offering one is at best a
    // wasted read and at worst a misparse; and this keeps the OFP path free of
    // any Arma config handling by construction instead of by luck.
    bool anyAuthored = false;
    for (int i = 0; i < layerCount && !anyAuthored; ++i)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
        anyAuthored = source != nullptr && source->surfaceCount != 0 && source->mask.GetLength() != 0;
    }
    const std::string world = land.GetName() ? land.GetName() : "";
    if (anyAuthored && _surfaceCatalogWorld != world)
    {
        BuildSurfaceCatalog(land);
        _surfaceCatalogWorld = world;
    }
    // RFG-062: the native Enfusion answer first, because a natively loaded world has
    // no authored A3 materials at all and BakeAuthoredGrassMask would return false
    // for it -- which is exactly the state that left Everon bare.
    const bool nativeClutter = BakeEnfusionClutter(land, geography, layersChanged,
                                                  reuseClutterDefinitions && !layersChanged);
    const bool authored = nativeClutter || BakeAuthoredGrassMask(land, geography, layersChanged);
    if (!authored)
    {
        // THE hazard, and it lives here rather than in the upload: the atlas is a
        // member, so a session that loads Takistan and then Everon would still be
        // holding Takistan's clutter when UploadGrassTuft next runs, and OFP
        // terrain would grow Arma plants. A world that builds no atlas must drop
        // the previous world's.
        _clutterAtlas.clear();
        _clutterAtlasLayers = 0;
    }
    else
    {
        const float edgeFraction = std::clamp(EnvFloat("WGR_GRASS_EDGE_FEATHER", 0.22f), 0.0f, 0.50f);
        const size_t feathered = FeatherAuthoredGrassEdges(geography, n, edgeFraction);
        LOG_INFO(Graphics,
                 "Wgpu grass edge feather: {} sparse fringe cells at {:.0f}% of adjacent authored coverage "
                 "(WGR_GRASS_EDGE_FEATHER)",
                 feathered, edgeFraction * 100.0f);
    }

    size_t roadPlacements = 0;
    const size_t roadExcluded = ExcludeModernRoadPlacementsFromGrass(land, geography, n, roadPlacements);
    if (roadPlacements != 0)
    {
        LOG_INFO(Graphics, "Wgpu grass road placements: {} authored road segment(s), {} grass cell(s) excluded",
                 roadPlacements, roadExcluded);
    }

    // Match grass.wgsl: water, forests, roads/tracks and hard buildings are
    // excluded, but the legacy "full" flag is valid ordinary ground on Everon
    // and must remain grass-capable.
    size_t grassCellCount = 0;
    size_t grassRenderableCellCount = 0;
    for (uint32_t cell : geography)
    {
        if ((cell & GrassTextureCell) == 0)
        {
            continue;
        }
        ++grassCellCount;
        if ((cell & 0x00000c7bu) == 0)
        {
            ++grassRenderableCellCount;
        }
    }
    // Some legacy Everon WRP revisions mark a broad part of ordinary ground
    // as forest. If that leaves no possible grass anywhere, treat only that
    // blanket forest flag as invalid. Water, roads and tracks stay protected.
    // Real forest/builder exclusions remain intact whenever the source data
    // provides even one valid grass cell.
    if (grassCellCount > 0 && grassRenderableCellCount == 0)
    {
        for (uint32_t& cell : geography)
            cell &= ~0x00000018u; // forestInner | forestOuter
        for (uint32_t cell : geography)
        {
            if ((cell & GrassTextureCell) != 0 && (cell & 0x00000c7bu) == 0)
                ++grassRenderableCellCount;
        }
        LOG_WARN(Graphics,
                 "Wgpu grass: legacy geography had no usable grass cells; relaxed blanket forest flags -> {} cells",
                 grassRenderableCellCount);
    }
    // A handful of old WRP files also store hard-object density across entire
    // ground regions. Only as a final zero-candidate fallback do we relax it;
    // this is preferable to silently rendering no grass at all on the map.
    if (grassCellCount > 0 && grassRenderableCellCount == 0)
    {
        for (uint32_t& cell : geography)
            cell &= ~0x00000c00u; // howManyHardObjects
        for (uint32_t cell : geography)
        {
            if ((cell & GrassTextureCell) != 0 && (cell & 0x00000c7bu) == 0)
                ++grassRenderableCellCount;
        }
        LOG_WARN(
            Graphics,
            "Wgpu grass: legacy geography still had no usable cells; relaxed blanket hard-object flags -> {} cells",
            grassRenderableCellCount);
    }
    // What the shader will ACTUALLY place. grass.wgsl splits the mask: hard
    // exclusions (water/road/track/hard objects) always apply, forest only when
    // the compatibility override is off. `grassRenderableCellCount` above uses
    // the combined mask, so it under-reports whenever the override is active.
    size_t hardOnlyCells = 0;
    for (uint32_t cell : geography)
    {
        if ((cell & GrassTextureCell) != 0 && (cell & 0x00000c63u) == 0)
            ++hardOnlyCells;
    }
    LOG_INFO(Graphics,
             "Wgpu grass texture mask ({}): {} layer(s), {} / {} surface cells, {} renderable after geography, "
             "{} with forest relaxed (road/water/building always excluded) [{}]",
             authored ? "authored LCA surfaces" : "legacy per-layer", grassLayerCount, grassCellCount, geography.size(),
             grassRenderableCellCount, hardOnlyCells, grassLayerNames.empty() ? "none matched" : grassLayerNames);
    LOG_INFO(Graphics, "Wgpu terrain texture layers: [{}]", allLayerNames);
    EngineWgpu::AcquireProducerWindow("grass geography");
    wgr_grass_set_geography(_renderer, static_cast<uint32_t>(n), static_cast<uint32_t>(n), geography.data());

    // The grass atlases are WORLD-scoped, not process-scoped.
    //
    // They used to be guarded by a plain `uploaded` bool on the single
    // EngineWgpu::_terrain, so the first world to load owned the atlas for the
    // rest of the session. That is harmless while the only content is our own
    // loose photo cards, and becomes a real defect the moment the atlas is built
    // from the MAP's clutter: load Takistan, then Everon, and OFP terrain grows
    // Arma clutter with no way to notice. Closing it now, before that content
    // exists, is the cheap ordering.
    //
    // The key is what the atlas content is derived FROM. An OFP world derives
    // nothing from the world, so every OFP world shares one key and rebuilds
    // never fire -- Everon keeps exactly the cards it has today at exactly
    // today's cost. An Arma world is keyed by name, so any Arma <-> OFP or
    // Arma <-> Arma transition rebuilds.
    const std::string atlasKey = anyAuthored ? ("arma:" + world) : std::string("loose");
    UploadGrassTuft(atlasKey);
    // RFG-089: a native Reforger world brings its own blades; everything else keeps the
    // stock meadow atlas.
    if (!(_enfusionBladesEnabled && UploadEnfusionBladeAtlas(land)))
    {
        UploadGrassBladeAtlas(atlasKey);
    }
}

// GRS-E — upload an authored photographed grass clump for the mid LOD's crossed
// cards. Entirely OPTIONAL: its absence must change nothing except how the mid
// ring is drawn.
//
// If no texture is found, `have_tuft` stays false on the renderer side and the
// mid ring keeps its procedural crossed ribbons -- the long-standing look. That
// is the only fallback, deliberately. The game also ships a 2001 grass PAA
// (data/trava1_pmp2.pac), but its opaque texels average (0.322, 0.375, 0.334):
// blue level with red, i.e. grey-teal rather than green. Falling back to that
// would hand anyone without this asset a WORSE picture than no photo cards at
// all, so it is reachable only by pointing WGR_GRASS_TUFT at a converted copy.
namespace
{
// One discovered photographed grass family: a colour plate and the opacity plate
// that carries its cut-out.
struct PhotoGrassSource
{
    std::string name;
    std::string colour;
    std::string opacity;
};

// The shader indexes a fixed nine-layer array (primary + eight families), so
// this is the number of families the atlas can carry, not a list of which ones.
constexpr uint32_t MaxPhotoGrassFamilies = 8;

bool HasSuffixToken(const std::string& stem, std::string_view token)
{
    return stem.find(token) != std::string::npos;
}

// Scan assets/grass/more_grass for colour/opacity plate pairs.
//
// The convention is only that the two files sit in the same directory and that
// their names differ by a role token -- the shipped packs use albedo/opacity
// (Poliigon-style) and diff/alpha (Poly Haven-style), and both are matched by
// substituting the token. Anything following either convention works with no
// code change, which is the point: the families are whatever is installed.
std::vector<PhotoGrassSource> DiscoverPhotoGrassFamilies(uint32_t limit)
{
    namespace fs = std::filesystem;
    static constexpr std::string_view kColourTokens[] = {"albedo", "diff", "color", "colour", "basecolor"};
    static constexpr std::string_view kOpacityTokens[] = {"opacity", "alpha", "mask"};

    std::vector<PhotoGrassSource> found;
    std::error_code ec;
    const fs::path root{"assets/grass/more_grass"};
    if (!fs::is_directory(root, ec))
    {
        return found;
    }
    for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec}, end;
         it != end && found.size() < limit; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        if (!it->is_regular_file(ec))
        {
            continue;
        }
        const fs::path& colour = it->path();
        std::string extension = colour.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".jpg" && extension != ".jpeg" && extension != ".png" && extension != ".tga")
        {
            continue;
        }
        std::string stem = colour.stem().string();
        std::string lower = stem;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        // Never treat an opacity plate as a colour plate: "..._alpha_4k" also
        // contains no colour token, but "..._diff_alpha" style names could.
        bool isOpacity = false;
        for (std::string_view token : kOpacityTokens)
        {
            isOpacity = isOpacity || HasSuffixToken(lower, token);
        }
        if (isOpacity)
        {
            continue;
        }
        std::string_view matched;
        for (std::string_view token : kColourTokens)
        {
            if (HasSuffixToken(lower, token))
            {
                matched = token;
                break;
            }
        }
        if (matched.empty())
        {
            continue;
        }
        // Find the sibling opacity plate by swapping the role token, trying each
        // opacity spelling and each supported extension.
        const size_t at = lower.find(matched);
        PhotoGrassSource source;
        for (std::string_view replacement : kOpacityTokens)
        {
            std::string candidateStem = stem;
            candidateStem.replace(at, matched.size(), replacement);
            for (const char* candidateExt : {".png", ".jpg", ".jpeg", ".tga"})
            {
                fs::path candidate = colour.parent_path() / (candidateStem + candidateExt);
                if (fs::is_regular_file(candidate, ec))
                {
                    source.opacity = candidate.string();
                    break;
                }
            }
            if (!source.opacity.empty())
            {
                break;
            }
        }
        if (source.opacity.empty())
        {
            continue;
        }
        source.colour = colour.string();
        // Name the family after its own directory when that is descriptive (the
        // packs each ship in one), otherwise after the plate itself.
        std::string folder = colour.parent_path().filename().string();
        source.name = (folder == "textures" || folder.empty()) ? stem : folder;
        found.push_back(std::move(source));
    }
    // Directory iteration order is filesystem-defined; sorting keeps atlas layer
    // indices stable between runs, so a weight slider means the same family
    // every time and a saved setting is not silently reassigned.
    std::sort(found.begin(), found.end(),
              [](const PhotoGrassSource& a, const PhotoGrassSource& b) { return a.colour < b.colour; });
    return found;
}
} // namespace

const char* TerrainWgpu::GrassPhotoFamilyName(int index) const
{
    if (index < 0 || index >= static_cast<int>(_grassPhotoFamilies.size()))
    {
        return "";
    }
    return _grassPhotoFamilies[static_cast<size_t>(index)].c_str();
}

// Which of the two conditions behind the photo-card path is actually holding it
// off. They are indistinguishable from every other signal -- both end with the
// expensive procedural clump drawn -- and the vertex count is the only tell,
// which needs the GPU stats overlay to read. So it is reported here, permanently,
// every time the atlas is (re)uploaded.
//
//   have_photo_clumps = 0  the atlas never reached the renderer: missing asset,
//                          failed decode, or a world whose clutter resolved
//                          nothing. A renderer-side fact.
//   have_photo_clumps = 1  the atlas is resident, so anything still drawing the
//                          procedural clump is the `use_photo_tuft` opt-in that
//                          the game pushes with GrassSettings, which defaults off.
void TerrainWgpu::ReportPhotoClumpState() const
{
    if (_renderer == nullptr)
    {
        return;
    }
    EngineWgpu::AcquireProducerWindow("grass photo clumps");
    const bool resident = wgr_grass_have_photo_clumps(_renderer) != 0;
    LOG_INFO(Graphics,
             "Wgpu grass photo cards: atlas resident={}; when resident, the near/mid rings still draw the procedural "
             "clump unless the game's use_photo_tuft opt-in is set (360/108 vertices per instance against 12)",
             resident ? 1 : 0);
}

void TerrainWgpu::UploadGrassTuft(const std::string& worldKey)
{
    if (!_renderer || _grassTuftWorld == worldKey)
    {
        return;
    }
    // Read straight off disk rather than the VFS: the file sits beside the
    // binaries, not in a PBO. stb_image is already vendored for the JPEG loader.
    // The upload is a fixed nine-layer local texture array: primary A3 clump
    // plus every available user-local grass family.
    const char* overridePath = std::getenv("WGR_GRASS_TUFT");

    // The map's own plants, where the map has them. Layer 0 is still the loose
    // primary card -- the atlas builder reserved it -- so a surface whose clutter
    // did not resolve, and every OFP world, keep exactly the look they have now.
    if (!_clutterAtlas.empty() && _clutterAtlasLayers > 1)
    {
        constexpr size_t LayerBytes = static_cast<size_t>(GrassAtlasSize) * GrassAtlasSize * 4u;
        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = nullptr;
        for (const char* candidate : {overridePath, "assets/grass/meadow-grass-clump-alpha-1024.png"})
        {
            if (candidate != nullptr && *candidate != '\0')
            {
                pixels = stbi_load(candidate, &w, &h, &channels, 4);
                if (pixels != nullptr)
                    break;
            }
        }
        if (pixels != nullptr)
        {
            stbir_resize_uint8_linear(pixels, w, h, w * 4, _clutterAtlas.data(), GrassAtlasSize, GrassAtlasSize,
                                      GrassAtlasSize * 4, STBIR_RGBA);
            stbi_image_free(pixels);
        }
        // Unused slots repeat layer 0 rather than sampling uninitialised memory.
        for (uint32_t layer = _clutterAtlasLayers; layer < GrassAtlasLayers; ++layer)
        {
            std::memcpy(_clutterAtlas.data() + static_cast<size_t>(layer) * LayerBytes, _clutterAtlas.data(),
                        LayerBytes);
        }
        _grassPhotoFamilies.clear();
        _grassPhotoFamilies.emplace_back("map clutter (per surface)");
        EngineWgpu::AcquireProducerWindow("grass tufts");
        wgr_grass_set_tufts(_renderer, GrassAtlasSize, GrassAtlasSize, GrassAtlasLayers, _clutterAtlas.data());
        _grassTuftWorld = worldKey;
        LOG_INFO(Graphics, "Wgpu grass: uploaded the map's clutter atlas ({} of {} layers used, {}x{}, {:.1f} MB)",
                 _clutterAtlasLayers, GrassAtlasLayers, GrassAtlasSize, GrassAtlasSize,
                 static_cast<double>(LayerBytes * GrassAtlasLayers) / (1024.0 * 1024.0));
        ReportPhotoClumpState();
        return;
    }

    const char* primary[] = {overridePath, "assets/grass/meadow-grass-clump-alpha-1024.png"};
    constexpr int TargetSize = 1024;
    std::vector<uint8_t> layers;
    const auto appendLayer = [&](const char* colourPath, const char* opacityPath) -> bool
    {
        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = stbi_load(colourPath, &w, &h, &channels, 4);
        if (pixels == nullptr)
        {
            return false;
        }
        std::vector<uint8_t> resized(static_cast<size_t>(TargetSize) * TargetSize * 4u);
        stbir_resize_uint8_linear(pixels, w, h, w * 4, resized.data(), TargetSize, TargetSize, TargetSize * 4,
                                  STBIR_RGBA);
        stbi_image_free(pixels);
        if (opacityPath != nullptr)
        {
            int aw = 0, ah = 0, ac = 0;
            uint8_t* opacity = stbi_load(opacityPath, &aw, &ah, &ac, 4);
            if (opacity == nullptr)
                return false;
            std::vector<uint8_t> alpha(static_cast<size_t>(TargetSize) * TargetSize * 4u);
            stbir_resize_uint8_linear(opacity, aw, ah, aw * 4, alpha.data(), TargetSize, TargetSize, TargetSize * 4,
                                      STBIR_RGBA);
            stbi_image_free(opacity);
            for (size_t i = 0; i < resized.size() / 4u; ++i)
                resized[i * 4u + 3u] = alpha[i * 4u];
        }
        layers.insert(layers.end(), resized.begin(), resized.end());
        return true;
    };
    for (const char* authored : primary)
    {
        if (authored != nullptr && *authored != '\0' && appendLayer(authored, nullptr))
            break;
    }
    if (!layers.empty())
    {
        _grassPhotoFamilies.clear();
        _grassPhotoFamilies.emplace_back("meadow-grass-clump (primary)");
        // Local families are DISCOVERED, not listed: anything dropped into
        // assets/grass/more_grass with a colour plate and a matching opacity
        // plate becomes a family. Hardcoding the eight that happened to be on one
        // machine meant the feature only worked on that machine.
        const size_t primaryBytes = static_cast<size_t>(TargetSize) * TargetSize * 4u;
        // Copy the primary layer out FIRST. Inserting a range that aliases the
        // same vector is undefined behaviour the moment the insert reallocates --
        // and it reallocates on every one of these appends. That fired precisely
        // when the optional `more_grass` sources are absent, i.e. for anyone who
        // has not installed the extra grass assets: the fallback path crashed.
        const std::vector<uint8_t> primaryLayer(layers.begin(), layers.begin() + primaryBytes);
        for (const PhotoGrassSource& variety : DiscoverPhotoGrassFamilies(MaxPhotoGrassFamilies))
        {
            if (appendLayer(variety.colour.c_str(), variety.opacity.c_str()))
            {
                _grassPhotoFamilies.push_back(variety.name);
            }
            else
            {
                LOG_WARN(Graphics, "Wgpu grass: photo family '{}' failed to decode; skipped", variety.name);
            }
        }
        // The shader's layer indices are fixed, so unfilled slots repeat the
        // primary clump rather than sampling uninitialised memory. They are not
        // named, so the Grass tab only ever offers families that really exist.
        const size_t named = _grassPhotoFamilies.size();
        while (layers.size() < (MaxPhotoGrassFamilies + 1u) * primaryBytes)
        {
            layers.insert(layers.end(), primaryLayer.begin(), primaryLayer.end());
        }
        EngineWgpu::AcquireProducerWindow("grass tufts");
        wgr_grass_set_tufts(_renderer, TargetSize, TargetSize, MaxPhotoGrassFamilies + 1u, layers.data());
        _grassTuftWorld = worldKey;
        LOG_INFO(Graphics, "Wgpu grass: {} photographed clump families discovered ({} atlas layers)", named,
                 MaxPhotoGrassFamilies + 1u);
        ReportPhotoClumpState();
        return;
    }
    // Expected for anyone without the optional asset -- INFO, not a warning.
    LOG_INFO(Graphics, "Wgpu grass: no mid-LOD clump texture; mid ring uses procedural ribbons");
    ReportPhotoClumpState();
}

// GRS-F — replace only the near ribbon's procedural surface detail. The near
// grass keeps its existing per-blade geometry, density and wind; these are
// opaque texture layers, not alpha-cutout cards.
void TerrainWgpu::UploadGrassBladeAtlas(const std::string& worldKey)
{
    if (!_renderer || _grassBladeAtlasWorld == worldKey)
    {
        return;
    }
    constexpr std::array<const char*, 8> BladePaths = {
        "assets/grass/meadow-grass-blade-0.png", "assets/grass/meadow-grass-blade-1.png",
        "assets/grass/meadow-grass-blade-2.png", "assets/grass/meadow-grass-blade-3.png",
        "assets/grass/meadow-grass-blade-4.png", "assets/grass/meadow-grass-blade-5.png",
        "assets/grass/meadow-grass-blade-6.png", "assets/grass/meadow-grass-blade-7.png",
    };

    int width = 0;
    int height = 0;
    std::vector<uint8_t> layers;
    for (const char* path : BladePaths)
    {
        int w = 0;
        int h = 0;
        int channels = 0;
        uint8_t* pixels = stbi_load(path, &w, &h, &channels, 4); // force opaque RGBA upload
        if (pixels == nullptr || (width != 0 && (w != width || h != height)))
        {
            stbi_image_free(pixels);
            LOG_INFO(Graphics,
                     "Wgpu grass: no complete near-blade photo atlas; near ring uses procedural surface detail");
            return;
        }
        if (width == 0)
        {
            width = w;
            height = h;
            layers.reserve(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * BladePaths.size());
        }
        layers.insert(layers.end(), pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        stbi_image_free(pixels);
    }

    EngineWgpu::AcquireProducerWindow("grass blade atlas");
    wgr_grass_set_blade_atlas(_renderer, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                              static_cast<uint32_t>(BladePaths.size()), layers.data());
    _grassBladeAtlasWorld = worldKey;
    LOG_INFO(Graphics, "Wgpu grass: near-LOD photo blade atlas uploaded ({} layers, {}x{})", BladePaths.size(), width,
             height);
}

int TerrainWgpu::GrassSurfaceCount() const
{
    return static_cast<int>(_grassSurfaceNames.size());
}

const char* TerrainWgpu::GrassSurfaceName(int index) const
{
    return index >= 0 && index < GrassSurfaceCount() ? _grassSurfaceNames[static_cast<size_t>(index)].c_str() : "";
}

bool TerrainWgpu::GrassSurfaceEnabled(int index) const
{
    return index >= 0 && index < static_cast<int>(_grassSurfaceEnabled.size()) &&
           _grassSurfaceEnabled[static_cast<size_t>(index)];
}

void TerrainWgpu::SetGrassSurfaceEnabled(int index, bool enabled)
{
    if (index < 0 || index >= static_cast<int>(_grassSurfaceEnabled.size()) ||
        _grassSurfaceEnabled[static_cast<size_t>(index)] == enabled)
    {
        return;
    }
    _grassSurfaceEnabled[static_cast<size_t>(index)] = enabled;
    if (_uploaded != nullptr)
    {
        UploadGeography(*_uploaded);
    }
}

void TerrainWgpu::UploadJitterMap(const Landscape& land)
{
    const int n = land.GetLandRange();
    if (n <= 0)
    {
        return;
    }
    // Per-grid-point random UV offsets (Landscape::_random). GetRandomColor
    // yields at most +-0.7 UV, so the snorm quantisation loses nothing next to
    // the source's own 0.1 granularity.
    std::vector<int8_t> offsets(2 * static_cast<size_t>(n) * n);
    for (int z = 0; z < n; z++)
    {
        for (int x = 0; x < n; x++)
        {
            float u = 0.0f;
            float v = 0.0f;
            land.GetRandomColor(x, z, u, v);
            const size_t at = 2 * (static_cast<size_t>(z) * n + x);
            offsets[at + 0] = static_cast<int8_t>(std::lround(u * 127.0f));
            offsets[at + 1] = static_cast<int8_t>(std::lround(v * 127.0f));
        }
    }
    EngineWgpu::AcquireProducerWindow("terrain jitter map");
    wgr_terrain_set_jitter_map(_renderer, static_cast<uint32_t>(n), static_cast<uint32_t>(n), offsets.data());
}

void TerrainWgpu::UploadDetailNoise()
{
    // Global config-driven texture; load once for the renderer's lifetime,
    // through the shared texture bank like any other texture.
    if (_detailNoiseTried)
    {
        return;
    }
    _detailNoiseTried = true;

    const ParamEntry& names = Remaster >> "CfgDetailTextures";
    RStringB detailName = names >> "detail";
    _detailNoise = GlobLoadTexture(detailName);
    Texture* tex = _detailNoise;
    if (tex == nullptr)
    {
        return;
    }
    const uint64_t handle = static_cast<TextureWgpu*>(tex)->EnsureUploaded();
    if (handle != 0)
    {
        EngineWgpu::AcquireProducerWindow("terrain detail layer");
        wgr_terrain_set_detail_layer(_renderer, handle);
    }
}

// MEASUREMENT ONLY -- changes nothing, drives nothing.
//
// The projected-size clamp on the grass near ring was reverted because the
// metric it was handed did not scale with the viewport. Reading the source
// settles half of that: `WgrCamera::proj` element 5 IS 1/tan(fov_y/2), by
// construction and not by assumption --
//
//   Camera.cpp:41            _projectionNormal(1,1) = _invCTop = 1 / cTop
//   MatrixConversion.cpp:63  mat._22 = src(1,1)            (flat index 5)
//   EngineWgpu.cpp:1705      ConvertProjectionMatrix(entry.proj, ProjectionNormal())
//   EngineWgpu.cpp:1716-17   only _33/_43 are rewritten for reversed-Z infinite far
//
// so the reversed-Z rebuild leaves the vertical-FOV term alone and the formula
// was structurally right. What that leaves is the HEIGHT term, and the engine's
// own viewport is the way to check it: if this line and the renderer-side value
// disagree, the disagreement is `config.height`, not the projection.
//
// cTop is the half-height of the view at unit distance, so pixels per radian is
// (render_height / 2) / cTop, and a blade of width w is one pixel wide at
// w * px_per_radian metres.
void TerrainWgpu::ReportGrassScreenMetric(Scene& scene) const
{
    static bool reported = false;
    if (reported || GEngine == nullptr)
    {
        return;
    }
    const Camera* camera = scene.GetCamera();
    if (camera == nullptr)
    {
        return;
    }
    const float cTop = static_cast<float>(camera->Top());
    const int height = GEngine->Height();
    const int width = GEngine->Width();
    if (cTop <= 0.0f || height <= 0)
    {
        return;
    }
    reported = true;
    const float pxPerRadian = (static_cast<float>(height) * 0.5f) / cTop;
    // The mean authored near-blade width (grass.wgsl: mix(0.016, 0.043) half-width).
    const float bladeWidth = 0.059f;
    LOG_INFO(Graphics,
             "Wgpu grass screen metric: engine viewport {}x{}, cTop={:.4f} (1/cTop={:.4f}), px_per_radian={:.1f}, "
             "one-pixel blade distance={:.1f} m",
             width, height, cTop, 1.0f / cTop, pxPerRadian, bladeWidth * pxPerRadian);
}

void TerrainWgpu::UpdateGroundTexturePages(const Landscape& land, const Camera& camera, bool force)
{
    if (!_pagedGroundMaterials)
        return;
    const float grid = std::max(land.GetLandGrid(), 0.001f);
    Vector3 pos = camera.Position();
    // Deterministic out-and-back page-cache diagnostic. The camera and final
    // screenshot remain fixed; only the material working-set centre moves.
    // Normal gameplay does not enter this path unless explicitly requested.
    if (const char* sweep = std::getenv("WGR_TERRAIN_PAGE_TEST_SWEEP_CELLS"))
    {
        const int range = std::max(1, land.GetLandRange());
        const int offsetCells = std::clamp(static_cast<int>(std::strtol(sweep, nullptr, 10)), 1, range);
        const int phase = _terrainPageTestUpdate < 180 ? 0 : (_terrainPageTestUpdate < 360 ? 1 : 2);
        ++_terrainPageTestUpdate;
        if (phase == 1)
        {
            pos[0] += offsetCells * grid;
            pos[2] += offsetCells * grid;
        }
        if (phase != _terrainPageTestPhase)
        {
            _terrainPageTestPhase = phase;
            LOG_INFO(Graphics, "Wgpu terrain page test sweep: phase={} offsetCells={}", phase,
                     phase == 1 ? offsetCells : 0);
        }
    }
    int centerX = static_cast<int>(std::floor(pos.X() / grid));
    int centerZ = static_cast<int>(std::floor(pos.Z() / grid));
    // An eight-cell hysteresis block avoids rebuilding descriptor arrays for
    // ordinary camera motion while still refreshing well before the player can
    // reach the edge of the 64-cell default working set.
    constexpr int QuantizeCells = 8;
    centerX = (centerX / QuantizeCells) * QuantizeCells;
    centerZ = (centerZ / QuantizeCells) * QuantizeCells;
    const int range = land.GetLandRange();
    // Sinkhole W5: a 4WVR world naming RVMATs (the converted Forest island: 1.7 m cells, 256 materials) keeps the
    // whole map resident -- a 64-cell window there is ~110 m and every tile past it shows its slot-0 surface. One
    // fixed window, built once per world, so the camera never re-pages. Every other world: unchanged.
    const bool wholeMap = land.HasRvmatCellMaterials();
    const int radius = wholeMap ? range : _terrainPageRadius;
    if (wholeMap)
        centerX = centerZ = 0;
    if (!force && centerX == _terrainPageCenterX && centerZ == _terrainPageCenterZ)
        return;
    _terrainPageCenterX = centerX;
    _terrainPageCenterZ = centerZ;
    const bool timings = TerrainPageTimingsEnabled();
    const auto timingStart = timings ? TerrainPageClock::now() : TerrainPageClock::time_point{};

    const int x0 = std::clamp(centerX - radius, 0, range);
    const int z0 = std::clamp(centerZ - radius, 0, range);
    const int x1 = std::clamp(centerX + radius + 1, 0, range);
    const int z1 = std::clamp(centerZ + radius + 1, 0, range);
    std::unordered_map<int, uint32_t> nearest;
    for (int z = z0; z < z1; ++z)
    {
        for (int x = x0; x < x1; ++x)
        {
            const int id = land.GetTexture(z, x);
            const int dx = x - centerX;
            const int dz = z - centerZ;
            const uint32_t d2 = static_cast<uint32_t>(dx * dx + dz * dz);
            const auto [it, inserted] = nearest.emplace(id, d2);
            if (!inserted)
                it->second = std::min(it->second, d2);
        }
    }
    std::vector<std::pair<int, uint32_t>> ordered(nearest.begin(), nearest.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
              { return a.second < b.second || (a.second == b.second && a.first < b.first); });
    std::vector<int> materials;
    materials.reserve(ordered.size());
    for (const auto& [id, distance] : ordered)
    {
        (void)distance;
        materials.push_back(id);
    }
    const double candidateScanMs = timings ? TerrainPageElapsedMs(timingStart) : 0.0;
    UploadGroundTextures(land, &materials);
    if (timings)
        LOG_INFO(Graphics,
            "Wgpu terrain page update timings: centre=({}, {}) candidateCells={} candidates={} "
            "candidateScanSortMs={:.3f} updateTotalMs={:.3f}",
            centerX, centerZ, static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(z1 - z0),
            materials.size(), candidateScanMs, TerrainPageElapsedMs(timingStart));
    LOG_INFO(Graphics, "Wgpu terrain page window: centre=({}, {}) radius={} cells candidateMaterials={}", centerX,
             centerZ, radius, materials.size());
}

bool TerrainWgpu::GetDrawCoverage(TerrainDrawCoverage& out) const
{
    if (!_uploaded || _uploaded != GLandscape) return false;
    out = _drawCoverage;
    return out.ready;
}

void TerrainWgpu::DrawTerrain(Scene& scene, int xBeg, int zBeg, int xEnd, int zEnd)
{
    _drawCoverage = {};
    if (_renderer == nullptr || GLandscape == nullptr)
    {
        return;
    }
    const Landscape& land = *GLandscape;
    Camera* camera = scene.GetCamera();
    const bool uploaded = UploadIfNeeded(land);
    RefreshGeographyIfChanged(land); // RFG-101: streamed objects densify the geography
    if (camera != nullptr)
    {
        // RenderSnapshot S4: the residency update -- a WORLD-STATE MUTATION -- no longer
        // runs from inside the terrain draw. World::SimulateAndDraw calls it on the
        // simulation side of the frame, which also stops object streaming from billing
        // its CPU cost to the land:gnd phase.
        UpdateGroundTexturePages(land, *camera, uploaded);
        // The object working set is a memory policy, so its cost has to be readable in the
        // same log as its counts -- otherwise "the world loads more now" is an unpriced claim.
        // Rate-limited to once every 128 terrain draws; the residency rows above are on a
        // similar cadence, so the two interleave without either drowning the other.
        if ((++_residencyMemoryLogTick % 128) == 0)
        {
            WgrMemoryStats memory{};
            if (EngineWgpu::LastMemoryStats(memory))
                LOG_INFO(Graphics,
                         "Wgpu tracked residency: {:.1f} MB tracked of {:.1f} MB budget; object textures={} "
                         "retired geometry={:.1f} MB",
                         memory.tracked_bytes / (1024.0 * 1024.0), memory.budget_bytes / (1024.0 * 1024.0),
                         memory.object_texture_count, memory.geometry_retired_bytes / (1024.0 * 1024.0));
        }
    }
    if (_rootIndex < 0)
    {
        return;
    }
    ReportGrassScreenMetric(scene);

    // Refresh the coast wet-band params every frame: the animated sea level + clock (+ the
    // live-tuned coast look) drive the damp intertidal band, keyed on the SAME sea level and
    // swash the water uses so the two register at one moving waterline.
    const Engine::WaterSettings& coast = _engine.WaterLook();
    _params.sea_level = land.GetSeaLevel();
    _params.time = render::frame::GPresentationSnapshot().timeSeconds;
    _params.swash_speed = coast.swashSpeed;
    _params.swash_amp = coast.swashAmp;
    _params.wet_height = coast.wetHeight;
    _params.wet_darken = coast.wetDarken;
    // RFG-065: the native Enfusion ground is a per-frame weight, so the dev panel's
    // checkbox is a live A/B with no re-upload. It reaches the shader as 0 on every
    // world whose materials do not set `enfusion`, so it is inert there by data.
    _params.enfusion_ground = _enfusionGroundEnabled ? 1.0f : 0.0f;
    // Alpine snowline (dev weather tab): permanent cover above a height, pushed
    // per frame like the wet band so the slider is live. Off = -1.
    _params.snowline_height =
        GSnow().EffSnowlineEnabled() && GSnow().snowlineRange > 0.0f ? GSnow().EffSnowlineHeight() : -1.0f;
    _params.snowline_range = std::max(GSnow().snowlineRange, 0.0f);
    _params.snowline_depth = std::clamp(GSnow().snowlineDepth, 0.0f, 1.0f);
    const auto& weather = render::frame::GPresentationSnapshot();
    _params._pad2 = UniformWettingDensity(weather.landscapeRainDensity, weather.rainEffectiveDensity,
                                        weather.rainParticleSnowflakes);
    const float solarDrying = SunlightDryingExposure(weather.sunDirection.Y(),
        weather.overcast, weather.nightEffect);
    _params._pad1 = _puddleWetness.Update(weather.timeSeconds, _params._pad2,
        solarDrying, weather.windSpeed / 20.0f);
    // Process-local diagnostic controls: isolate appearance without editing a
    // persistent weather/water profile. The wetness override still obeys ground,
    // slope, shore, roof and snow eligibility in the shader.
    static const float wetnessOverride = EnvFloat("WGR_TERRAIN_PUDDLE_WETNESS", -1.0f);
    static const bool puddlesEnabled = EnvFloat("WGR_TERRAIN_PUDDLES", 1.0f) > 0.5f;
    if (std::isfinite(wetnessOverride) && wetnessOverride >= 0.0f)
        _params._pad1 = std::clamp(wetnessOverride, 0.0f, 1.0f);
    if (!puddlesEnabled) { _params._pad1 = 0.0f; _params._pad2 = 0.0f; }
    // Diagnostic-only telemetry establishes that the installed weather snapshot
    // reaches accumulation. Duplicate/paused draws do not produce new samples.
    static const bool puddleDiagnostic = EnvFloat("WGR_TERRAIN_PUDDLE_FIXTURE", 0.0f) > 0.5f;
    static float lastPuddleDiagnosticTime = -1.0f;
    if (puddleDiagnostic && std::isfinite(weather.timeSeconds) &&
        (lastPuddleDiagnosticTime < 0.0f || weather.timeSeconds < lastPuddleDiagnosticTime ||
         weather.timeSeconds - lastPuddleDiagnosticTime >= 5.0f))
    {
        LOG_INFO(Graphics, "Wgpu terrain puddle weather: time={:.3f} rain={:.3f} accumulated={:.4f} uploaded={:.4f}",
                 weather.timeSeconds, _params._pad2,
                 _puddleWetness.value, _params._pad1);
        lastPuddleDiagnosticTime = weather.timeSeconds;
    }
    _engine.QueueTerrainParams(_params); // REN-THR-013: queued, applied before wgr_render_frame
    const auto& snow = GSnow();
    // The GPU window is presentation state. Paused freefly, deposit commands
    // and roof edits must still refresh it without advancing retained snow.
    const double snowWallTime = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (camera && (snow.CoverActive() || _snowWasEnabled || uploaded) &&
        (snow.CoverActive() != _snowWasEnabled || uploaded || _params.time < _snowUploadTime ||
         _params.time - _snowUploadTime >= 0.1f || snowWallTime - _snowUploadWallTime >= 0.1))
    {
        auto snapshot = snow.Snapshot(camera->Position().X(), camera->Position().Z());
        MaskSnowShelters(land, snapshot);
        _engine.QueueSnow(snapshot);
        _snowUploadTime = _params.time;
        _snowUploadWallTime = snowWallTime;
        _snowWasEnabled = snow.CoverActive();
    }

    const auto& mud = GMud();
    if (camera && std::isfinite(camera->Position().X()) && std::isfinite(camera->Position().Z()))
    {
        // Chunk-aligned camera residency changes only the displayed window, never the store.
        const int mx = int(std::floor(camera->Position().X() / 2.0f));
        const int mz = int(std::floor(camera->Position().Z() / 2.0f));
        if (!_mudUploaded || uploaded || _mudWasEnabled != mud.enabled || _mudRevision != mud.Revision() || mx != _mudWindowX || mz != _mudWindowZ)
        {
            auto snapshot = mud.Snapshot(camera->Position().X(), camera->Position().Z());
            if (mud.Chunks() == 0) snapshot[3] = 0.0f;
            _engine.QueueMud(std::move(snapshot));
            _mudRevision = mud.Revision(); _mudWindowX = mx; _mudWindowZ = mz; _mudUploaded = true; _mudWasEnabled = mud.enabled;
        }
    }
    const auto& sand = GSand();
    if (camera && std::isfinite(camera->Position().X()) && std::isfinite(camera->Position().Z()))
    {
        const int sx = int(std::floor(camera->Position().X() / 2.0f));
        const int sz = int(std::floor(camera->Position().Z() / 2.0f));
        if (!_sandUploaded || uploaded || _sandWasEnabled != sand.enabled || _sandRevision != sand.Revision() ||
            sx != _sandWindowX || sz != _sandWindowZ)
        {
            auto snapshot = sand.Snapshot(camera->Position().X(), camera->Position().Z());
            if (sand.Chunks() == 0) snapshot[3] = 0.0f;
            static const bool sandUploadTrace=[] {
#ifdef _WIN32
                char* value=nullptr;size_t length=0;
                _dupenv_s(&value,&length,"WGR_SAND_UPLOAD_TRACE");
                const bool enabled=SandUploadTraceRequested(value);std::free(value);return enabled;
#else
                return SandUploadTraceRequested(std::getenv("WGR_SAND_UPLOAD_TRACE"));
#endif
            }();
            if (sandUploadTrace)
            {
                const auto receipt=InspectSandUpload(snapshot);
                const float minX=snapshot[0]+(float(receipt.minimumIndex%SandField::WindowSize)+.5f)*snapshot[2];
                const float minZ=snapshot[1]+(float(receipt.minimumIndex/SandField::WindowSize)+.5f)*snapshot[2];
                LOG_INFO(Graphics,"SAND_UPLOAD_QUEUED revision={} enabled={} chunks={} cameraX={:.5f} cameraZ={:.5f} originX={:.5f} originZ={:.5f} cell={:.5f} limit={:.5f} count={} finite={} negative={} positive={} min={:.8f} max={:.8f} minX={:.5f} minZ={:.5f} hash={:016x}",
                    sand.Revision(),sand.enabled,sand.Chunks(),camera->Position().X(),camera->Position().Z(),
                    snapshot[0],snapshot[1],snapshot[2],snapshot[3],snapshot.size(),receipt.finite,
                    receipt.negative,receipt.positive,receipt.minimum,receipt.maximum,minX,minZ,receipt.hash);
                static const auto sandSnapshotPaths=[] {
#ifdef _WIN32
                    char* fileValue=nullptr;char* rootValue=nullptr;size_t length=0;
                    _dupenv_s(&fileValue,&length,"WGR_SAND_SNAPSHOT_FILE");
                    _dupenv_s(&rootValue,&length,"WGR_SAND_SNAPSHOT_ROOT");
                    const std::array<std::filesystem::path,2> paths{fileValue?fileValue:"",rootValue?rootValue:""};
                    std::free(fileValue);std::free(rootValue);return paths;
#else
                    const char* fileValue=std::getenv("WGR_SAND_SNAPSHOT_FILE");
                    const char* rootValue=std::getenv("WGR_SAND_SNAPSHOT_ROOT");
                    return std::array<std::filesystem::path,2>{fileValue?fileValue:"",rootValue?rootValue:""};
#endif
                }();
                const auto& sandSnapshotFile=sandSnapshotPaths[0];
                if (!sandSnapshotFile.empty())
                {
                    const bool written=DumpSandUpload(snapshot,sandSnapshotFile,sandSnapshotPaths[1]);
                    LOG_INFO(Graphics,"SAND_UPLOAD_SNAPSHOT written={} time={:.5f} revision={} count={} hash={:016x} file={}",
                        written,_params.time,sand.Revision(),snapshot.size(),receipt.hash,sandSnapshotFile.string());
                }
            }
            _engine.QueueSand(std::move(snapshot));
            _sandRevision = sand.Revision(); _sandWindowX = sx; _sandWindowZ = sz;
            _sandUploaded = true; _sandWasEnabled = sand.enabled;
        }
    }
    if (camera == nullptr)
    {
        return;
    }
    // Preserve the old fog/view-distance rectangle for the startup A/B and witness.
    // It is not the infinite-far GPU frustum.
    const float landGrid = land.GetLandGrid();
    const int landRange = land.GetLandRange();
    float rx0, rz0, rx1, rz1;
    if (_extended)
    {
        rx0 = std::max(xBeg * landGrid, _treeMin);
        rz0 = std::max(zBeg * landGrid, _treeMin);
        rx1 = std::min(xEnd * landGrid, _treeMax);
        rz1 = std::min(zEnd * landGrid, _treeMax);
    }
    else
    {
        rx0 = std::max(xBeg, 0) * landGrid;
        rz0 = std::max(zBeg, 0) * landGrid;
        rx1 = std::min(xEnd, landRange) * landGrid;
        rz1 = std::min(zEnd, landRange) * landGrid;
    }
    const std::array<float, 4> legacyRect{rx0, rz0, rx1, rz1};
    if (_backgroundCoverage)
    {
        // One selection partitions the existing tree: no duplicate near/far draw.
        // Distance still chooses detail; texture residency/fallback stays unchanged.
        // Water covers this same tree with an infinite-far projection. Clipping
        // opaque ground to the old rectangle exposes sky through its dry-land mask,
        // with a hard edge whenever a whole CDLOD patch disappears.
        rx0 = rz0 = _extended ? _treeMin : std::max(_treeMin, 0.0f);
        rx1 = rz1 = _extended ? _treeMax : std::min(_treeMax, landRange * landGrid);
    }

    auto emitNode = [&](const CdlodSelection& s)
    {
        WgrTerrainNode node{};
        node.origin = {s.originX, s.originZ};
        node.size = s.size;
        node.lod = static_cast<uint32_t>(s.level);
        node.morph_start = s.morphStart;
        node.morph_end = s.morphEnd;
        _selected.push_back(node);
    };

    const Vector3 snowCamera = camera->Position();
    // Effective snow depth at the camera's ground: snowfall deposit or altitude
    // snowline cover, so the close-up relief also refines on permanent snowfields.
    const float snowEffDepth = std::max(snow.Depth(), snow.AltitudeDepthAt(snowCamera.X(), snowCamera.Z()));
    const bool refineSnow = snow.CoverActive() && snow.detailedGeometry && snowEffDepth > 0.002f &&
        std::abs(snowCamera.Y() - land.SurfaceY(snowCamera.X(), snowCamera.Z())) < 15.0f;
    const bool refineMud = mud.enabled && mud.Chunks() > 0 &&
        std::abs(snowCamera.Y() - land.SurfaceY(snowCamera.X(), snowCamera.Z())) < 15.0f;
    const bool refineSand = sand.enabled && sand.Chunks() > 0 &&
        std::abs(snowCamera.Y() - land.SurfaceY(snowCamera.X(), snowCamera.Z())) < 15.0f;
    std::array<CdlodContactFocus,2> reliefFocus{};
    size_t reliefFocusCount=0;
    if (refineSnow || refineMud || refineSand)
        reliefFocus[reliefFocusCount++]={snowCamera.X(),snowCamera.Z(),8.0f};
    // Main-thread Landscape::Draw: CameraOn is the actual controlled/spectated
    // soldier, not the offset external render camera. Keep the view's existing
    // focus and add only a near on-foot target with local stored contact relief.
    // Freefly/cutscene effects keep CameraOn on the player: exclude those.
    if (GWorld && !GWorld->GetCameraEffect() &&
        (GWorld->GetCameraType()==CamExternal || GWorld->GetCameraType()==CamGroup))
    {
        const Man* target=dyn_cast<Man>(GWorld->CameraOn());
        const bool onFoot=target && !target->IsDead() &&
            (!target->Brain() || !target->Brain()->GetVehicleIn());
        if (onFoot)
        {
            const Vector3 pos=target->Position();
            const bool finiteTarget=std::isfinite(pos.X()) && std::isfinite(pos.Y()) && std::isfinite(pos.Z()) &&
                std::abs(pos.X())<=1000000.0f && std::abs(pos.Y())<=1000000.0f && std::abs(pos.Z())<=1000000.0f;
            const float ground=finiteTarget ? land.SurfaceY(pos.X(),pos.Z()) : 0.0f;
            // First apply cheap range/height guards. Persistent stored relief
            // is the current local-contact proof, including pause/dry-down;
            // no new producer timestamp, synthetic stamp or field mutation.
            if (finiteTarget && CdlodContactFocusEligible(snowCamera.X(),snowCamera.Y(),snowCamera.Z(),
                    pos.X(),pos.Y(),pos.Z(),ground,true,true,true))
            {
                bool localRelief=false;
                for (int z=-4;z<=4 && !localRelief;++z)
                    for (int x=-4;x<=4 && !localRelief;++x)
                    {
                        const float fx=pos.X()+x*0.25f, fz=pos.Z()+z*0.25f;
                        localRelief=(mud.enabled && mud.Chunks()>0 && mud.HeightOffsetAt(fx,fz)<-0.001f) ||
                            (sand.enabled && sand.Chunks()>0 && sand.HeightOffsetAt(fx,fz)<-0.001f) ||
                            (snow.CoverActive() && snow.detailedGeometry && snow.BaseDepthAt(fx,fz)>0.002f &&
                                snow.DeficitAt(int(std::floor(fx/SnowField::CellSize)),
                                               int(std::floor(fz/SnowField::CellSize)))>0.001f);
                    }
                if (localRelief)
                    reliefFocus[reliefFocusCount++]={pos.X(),pos.Z(),8.0f};
            }
        }
    }
    auto emit = [&](const CdlodSelection& s)
    {
        if (reliefFocusCount>0)
            RefineCdlodContactUnion(s,reliefFocus,reliefFocusCount,emitNode);
        else
            emitNode(s);
    };

    _selected.clear();
    // Off-map terrain draws in full, seabed included — NOT just above-water land. The
    // water is transparent, so the seabed shows through it; pruning below-sea off-map
    // would leave a visible seam at the map edge where in-map water sits over seabed
    // but off-map water sits over the empty sky background. (Once the water look plan
    // makes deep water opaque, an occlusion prune of fully-submerged off-map terrain
    // can come back as a pure optimization — keep the terrain extent >= the water
    // extent so seabed always underlies the ocean.)
    SelectVisibleCdlod(
        _tree, _rootIndex, _numLevels, _ranges, _morphRegion, *camera, rx0, rz0, rx1, rz1,
        [](const CdlodNode&) { return true; }, emit,
        (snow.CoverActive() ? snowEffDepth : 0.0f) + (mud.enabled && mud.Chunks() > 0 ? MudField::MaxDepth : 0.0f)
            + (sand.enabled && sand.Chunks() > 0 ? SandField::MaxDepth : 0.0f),
        _backgroundCoverage);

    _drawCoverage.frame = GEngine ? static_cast<uint64_t>(GEngine->GetFrameCounter()) : 0;
    _drawCoverage.heightRevision = _uploadedHeightRevision;
    _drawCoverage.terrainRange = _uploadedRange;
    _drawCoverage.terrainGrid = land.GetTerrainGrid();
    _drawCoverage.ready = _uploaded == &land && _uploadedHeightRevision == land.HeightRevision() &&
        _rootIndex >= 0 && !_selected.empty();
    _drawCoverage.background = _backgroundCoverage;
    _drawCoverage.pagedMaterials = _pagedGroundMaterials;
    _drawCoverage.patches = static_cast<uint32_t>(_selected.size());
    const auto& cameraPos = camera->Position();
    _drawCoverage.camera = {cameraPos.X(), cameraPos.Y(), cameraPos.Z()};
    _drawCoverage.cameraFar = camera->Far();
    _drawCoverage.legacyRect = legacyRect;
    _drawCoverage.treeBounds = {_treeMin, _treeMax};
    for (const auto& patch : _selected)
    {
        ++_drawCoverage.levels[std::min<size_t>(patch.lod, _drawCoverage.levels.size() - 1)];
        CdlodNode node{};
        node.originX = patch.origin.x; node.originZ = patch.origin.y; node.size = patch.size;
        if (!CdlodIntersectsRect(node, legacyRect[0], legacyRect[1], legacyRect[2], legacyRect[3]))
            ++_drawCoverage.outsideLegacyRect;
        const float dx = std::max(std::abs(node.originX - cameraPos.X()),
            std::abs(node.originX + node.size - cameraPos.X()));
        const float dz = std::max(std::abs(node.originZ - cameraPos.Z()),
            std::abs(node.originZ + node.size - cameraPos.Z()));
        _drawCoverage.farthestPatch = std::max(_drawCoverage.farthestPatch, std::sqrt(dx * dx + dz * dz));
    }

    _engine.SubmitTerrain(_selected);
    // The procedural path must remain available on maps that use the modern GPU
    // terrain path (which can skip the legacy alpha overlay callback entirely).
    // SubmitGrass is frame-deduplicated by EngineWgpu; SetGrassParams below is an
    // additional legacy signal, not the only activation path.
    _engine.SubmitGrass();
}

} // namespace Poseidon
