#pragma once
#include <chrono>

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Core/ITerrainRenderer.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Terrain/TerrainCdlod.hpp>
#include <Poseidon/World/Terrain/TerrainSurfaceCatalog.hpp>

#include <wgpu_renderer.hpp>
#include "TerrainPuddles.hpp"
#include "TerrainWgpuTidewaterGround.hpp"
#include "EarthTerrainStream.hpp"
#include "EarthBuildingStream.hpp"
#include "EarthSurfaceStream.hpp"
#include "EarthFeatureStream.hpp"
#include "EarthWorldgen.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <future>
#include <string>
#include <Poseidon/IO/Streams/QBStream.hpp>

class LODShapeWithShadow;

namespace Poseidon
{
class EngineWgpu;
class Landscape;
class Camera;
class TextureWgpu;

// The grass geography word and clutter atlas geometry, shared by the two bakes
// that write it (the Arma-generation LCA mask bake in TerrainWgpu.cpp and the
// Enfusion surface bake in TerrainWgpuEnfusionClutter.cpp). The full bit layout
// is documented at the top of TerrainWgpu.cpp; it is mirrored by GEO_* in
// grass/grass.wgsl, so all three change together.
namespace GrassGeo
{
constexpr uint32_t TextureCell = 0x80000000u;
constexpr uint32_t AuthoredCell = 0x00008000u;
constexpr uint32_t CoverageShift = 16u;
constexpr uint32_t CoverageMask = 0x007f0000u;
constexpr uint32_t SurfaceShift = 23u;
constexpr uint32_t SurfaceMask = 0x0f800000u;
constexpr uint32_t RunCountShift = 28u;
constexpr uint32_t RunCountMask = 0x70000000u;
constexpr uint32_t AtlasLayers = 32;
// RFG-115: the count field is 3 bits, so a run holds at most 7. This was 8, and an
// 8-plant run packed as (8 << 28) & RunCountMask == 0 -- which the shader reads as
// "no run of its own" and answers with the GLOBAL mix (layers 0..8). Everon's two
// conifer floors are 8-plant surfaces: they grew Grass_03's meadow cards instead of
// needles, ferns and bilberry, and the forest floor looked like a sunlit meadow.
constexpr uint32_t AtlasMaxRun = 7;
static_assert(AtlasMaxRun <= (RunCountMask >> RunCountShift), "a run must fit the count field");
constexpr int AtlasSize = 512;
constexpr uint32_t BakedMask = TextureCell | AuthoredCell | CoverageMask | SurfaceMask | RunCountMask;
} // namespace GrassGeo

// Draws the terrain via wgpu: uploads the heightmap once per map, builds a CDLOD
// quadtree, and each frame emits the selected grid nodes as GPU instances.
class TerrainWgpu : public ITerrainRenderer
{
  public:
    TerrainWgpu(EngineWgpu& engine, WgrRenderer* renderer);
    ~TerrainWgpu() override;
    bool IsEarthStreaming() const { return bool(_earthStream); }

    void DrawTerrain(Scene& scene, int xBeg, int zBeg, int xEnd, int zEnd) override;
    bool GetDrawCoverage(TerrainDrawCoverage& out) const override;
    int GrassSurfaceCount() const;
    // True only for the source receipt emitted after this real heightmap upload.
    bool RainWaterSourceMatches(const WgrRainWaterSourceKey& key) const;
    // GRS-E: upload the game's photographed grass tuft for the mid LOD.
    // `worldKey` identifies what the atlas content is derived from; the upload is
    // skipped while it is unchanged, and repeated when it is not. See the call
    // site in UploadGeography for why this is a key and not a bool.
    void UploadGrassTuft(const std::string& worldKey);
    // GRS-F: upload eight opaque photo layers for the near blade geometry.
    void UploadGrassBladeAtlas(const std::string& worldKey);
    // RFG-089: the same eight layers cut from a natively loaded Reforger world's OWN blade
    // atlas -- the `PlantMat` of its commonest grass surface -- so the near ring draws
    // Enfusion's blades rather than the stock meadow photos. Returns false (and uploads
    // nothing) on any world that is not Enfusion, so the stock atlas still applies there.
    bool UploadEnfusionBladeAtlas(const Landscape& land);
    // RFG-090: true while the near ring's blade atlas is the world's own PlantMat, which
    // is when the near ring must draw blades rather than clutter cards.
    bool GrassNearBladesFromEnfusion() const { return _grassBladeAtlasWorld.rfind("enfusion:", 0) == 0; }
    // RFG-090: the PlantMat's authored `Height` (m) behind that atlas, 0 when none.
    float GrassEnfusionBladeHeight() const { return GrassNearBladesFromEnfusion() ? _enfusionBladeHeight : 0.0f; }
    // RFG-091: the ground colour the native blades lean towards, and by how much (0 = none).
    float GrassEnfusionBladeTint(float out[3]) const
    {
        for (int c = 0; c < 3; ++c)
            out[c] = _enfusionBladeTint[c];
        return GrassNearBladesFromEnfusion() ? _enfusionBladeTintLerp : 0.0f;
    }
    void SetEnfusionBladesEnabled(bool enabled);
    // Reports whether the photo-card atlas actually reached the renderer, so an
    // absent atlas and a switched-off opt-in stop looking identical in the log.
    void ReportPhotoClumpState() const;
    // Measurement only: logs the engine-side pixels-per-radian once, so the
    // reverted near-ring clamp can be re-derived against a number rather than
    // against an assumption. Drives nothing.
    void ReportGrassScreenMetric(Scene& scene) const;
    const char* GrassLoadedMapName() const { return _uploadedName.c_str(); }
    const char* GrassSurfaceName(int index) const;
    bool GrassSurfaceEnabled(int index) const;
    void SetGrassSurfaceEnabled(int index, bool enabled);
    // Eden/Everon ships legacy geography flags that can classify all normal
    // terrain as excluded. Keep this map-specific compatibility decision next
    // to the uploaded terrain data, not in UI state.
    bool GrassNeedsCompatibilityOverride() const { return _grassNeedsCompatibilityOverride; }
    // True once this world's OWN clutter (CfgSurfaces -> CfgSurfaceCharacters -> clutter
    // models) built a multi-layer atlas. Layer 0 is the loose primary card the builder
    // always reserves, so >1 means the map actually contributed plants. False on every
    // OFP world and on any world whose clutter did not resolve -- both keep the
    // procedural blades. Read by EngineWgpu to auto-enable the photo-tuft path.
    bool GrassHasMapClutterAtlas() const { return !_clutterAtlas.empty() && _clutterAtlasLayers > 1; }
    // RFG-062: the native Enfusion clutter binding (TerrainWgpuEnfusionClutter.cpp).
    // `HasEnfusionSurfaces` is false on every world that is not a natively loaded
    // Reforger one, which is what the dev panel greys the checkbox on.
    bool HasEnfusionSurfaces() const { return _hasEnfusionSurfaces; }
    bool EnfusionClutterEnabled() const { return _enfusionClutterEnabled; }
    void SetEnfusionClutterEnabled(bool enabled);
    // In-process re-mount: the Landscape `_uploaded` points at has been freed and the config
    // replaced, so the next upload must take the new-map path and reload the detail layer.
    void ResetForRemount();
    // RFG-065: the native Enfusion GROUND material (TerrainWgpuEnfusionSurface.cpp) --
    // the `.emat`'s own ScaleUV tiling and its middle-distance map, instead of one
    // image stretched over each 12.5 m land cell. A per-frame uniform weight, so this
    // is a live A/B and 0 is bit-exact the legacy ground.
    bool EnfusionGroundEnabled() const { return _enfusionGroundEnabled; }
    void SetEnfusionGroundEnabled(bool enabled) { _enfusionGroundEnabled = enabled; }
    // Photographed clump families actually found on disk, in atlas-layer order
    // (index 0 = the primary clump). Discovered, never hardcoded, so dropping a
    // grass pack into assets/grass/more_grass is all it takes to get one.
    int GrassPhotoFamilyCount() const { return static_cast<int>(_grassPhotoFamilies.size()); }
    const char* GrassPhotoFamilyName(int index) const;

  private:
    void DrawEarthTerrain(Scene& scene, Camera& camera, const Landscape& land);
    std::unique_ptr<EarthTerrainStream> _earthStream;
    std::unique_ptr<EarthTerrainStream::Patch> _earthPatch;
    std::unique_ptr<EarthBuildingStream> _earthBuildings;
    std::unique_ptr<EarthSurfaceStream> _earthSurface;
    // A newer patch held back until its imagery is ready (or 12 s pass), so the textured ground stays up.
    std::unique_ptr<EarthTerrainStream::Patch> _earthPendingPatch;
    std::chrono::steady_clock::time_point _earthPendingAt{};
    std::string _earthImageryPath; // globe/overview image: POSEIDON_EARTH_IMAGERY or the provider overview
    std::unique_ptr<EarthFeatureStream> _earthFeatures;
    std::unique_ptr<EarthWorldgen> _earthWorldgen;
    std::unique_ptr<EarthWorldgen::Snapshot> _earthWorldgenSnapshot;
    std::unordered_set<std::string> _earthWorldgenAttemptedModels;
    std::unordered_set<std::string> _earthWorldgenFailedModels;
    size_t _earthWorldgenNext=0,_earthWorldgenCreated=0;
    uint64_t _earthWorldgenGeneration=0;
    std::array<WgrMesh,EarthFeatureStream::KindCount> _earthFeatureMeshes{};
    std::array<uint32_t,EarthFeatureStream::KindCount> _earthFeatureIndices{};
    std::array<WgrTexture,EarthFeatureStream::KindCount> _earthFeatureTextures{};
    uint64_t _earthFeatureGeneration=0;
    WgrMesh _earthNearMesh=0;
    uint32_t _earthNearIndices=0;
    WgrTexture _earthSurfaceTexture=0;
    WgrTexture _earthFarTexture=0;
    std::unique_ptr<class FishSchools> _fish;
    std::unique_ptr<class BirdFlocks> _birds;
    std::unique_ptr<class ReefLife> _reef;
    std::unique_ptr<class BeachCritters> _crabs;
    std::unique_ptr<class BeachDebris> _debris;
    std::unique_ptr<class ProceduralTrees> _ezTrees;
    std::unique_ptr<class EarthTreeDetail> _earthTreeDetail;
    float _surfaceGrid = 0.0f; // RFG-PBR: metres per native surface-map texel, 0 = none
    bool _earthTreeModels = false; // the game's own tree models carry the trees (PlaceEarthTreeModels)
    bool PlaceEarthTreeModels(const std::vector<EarthFeatureStream::TreeInstance>& trees);
    // the model tier's two sources: the OSM features' trees and the forest canopy the satellite atlas
    // shows (EarthCanopyTrees); RefreshEarthTreeModels places their union
    std::vector<EarthFeatureStream::TreeInstance> _earthFeatureTreeSet, _earthForestTreeSet, _earthCanopyTrees;
    void RefreshEarthTreeModels();
    static std::vector<EarthFeatureStream::TreeInstance> EarthCanopyTrees(const EarthTerrainStream::Patch& patch,
        const EarthSurfaceStream::Window& window, const std::vector<uint8_t>& pixels);
    // one mesh per EarthBuildingStream::Material, drawn with the game's house texture (when it loads) or
    // the palette colour
    std::array<WgrMesh,EarthBuildingStream::MaterialCount> _earthBuildingMeshes{};
    std::array<uint32_t,EarthBuildingStream::MaterialCount> _earthBuildingIndices{};
    std::array<Ref<Texture>,EarthBuildingStream::MaterialCount> _earthBuildingTextures{};
    bool _earthBuildingTexturesTried=false;
    // the game's own road surfaces for the paved and unpaved OSM ways (null: the plain colour)
    std::array<Ref<Texture>,2> _earthRoadTextures{};
    bool _earthRoadTexturesTried=false;
    WgrTexture _earthBuildingTexture=0; // wall/roof colour palette (EarthBuildingStream::Palette)
    uint64_t _earthBuildingGeneration=0;
    WgrMesh _earthHorizonMesh = 0;
    uint32_t _earthHorizonIndices = 0;
    WgrMesh _earthGlobeMesh = 0;
    WgrTexture _earthGlobeTexture = 0;
    uint32_t _earthGlobeIndices = 0;
    uint64_t _earthOriginRevision = 0;
    void UploadEarthGlobe(double latitude,double longitude);
    void UploadEarthHorizon(const EarthTerrainStream::Patch& patch,bool localUv=false);
    // Whole World grass: the near patch's heights and a grass mask read from its satellite atlas
    bool UploadEarthGrass(const EarthTerrainStream::Patch& patch,const struct EarthSurfaceStreamAtlasRef& atlas);
    bool _earthGrass=false;
    void UploadEarthNear(const EarthTerrainStream::Patch& patch,bool localUv);
    // (Re)uploads and rebuilds the quadtree when the map changes; returns true if it did.
    bool UploadIfNeeded(const Landscape& land);
    void BuildQuadtree(const Landscape& land);
    // `activeMaterials == nullptr` is the eager legacy path. Modern OPRW worlds
    // pass a near-to-far camera working set and keep the remaining materials on
    // a shared coarse surface fallback.
    void UploadGroundTextures(const Landscape& land, const std::vector<int>* activeMaterials = nullptr);
    Ref<Texture> BuildAutomaticSatmap(const Landscape& land);
    // Tidewater's sand and soil baked into OFP's sand / field layers (TerrainWgpuTidewaterGround.hpp),
    // once per layer per world
    TidewaterGround _twGround;
    std::string _twGroundWorld;
    std::vector<std::pair<Texture*, Ref<Texture>>> _twGroundBaked;
    Texture* TidewaterGroundLayer(const Landscape& land, Texture* layer);
    // Tidewater's detail heights (TerrainWgpuTidewaterDetail.hpp), built once, bound as a ground
    // layer for the shader's sand and rock (TerrainMaterial.middle on those legacy layers)
    Ref<Texture> _twDetail;
    Ref<Texture> _automaticSatmap;
    void UpdateGroundTexturePages(const Landscape& land, const Camera& camera, bool force);
    void UploadIndexMap(const Landscape& land);
    void UploadGeography(const Landscape& land, bool reuseClutterDefinitions = false);
    // RFG-101: re-upload the geography when the landscape folded streamed objects into it
    // (Landscape::GeographyRevision), at most every couple of seconds.
    void RefreshGeographyIfChanged(const Landscape& land);
    uint32_t _geographyRevisionUploaded = 0;
    float _snowUploadTime = -1.0f;
    double _snowUploadWallTime = -1.0;
    bool _snowWasEnabled = false;
    // PERF-FIELD-EMPTY-SKIP: the last upload of each field was an empty window (no chunks), whose
    // content does not depend on where the window sits.
    bool _snowUploadedEmpty = false, _mudUploadedEmpty = false, _mudFarUploadedEmpty = false,
         _sandUploadedEmpty = false;
    float _snowUploadX = 0.0f, _snowUploadZ = 0.0f; // camera at the last snow upload
    uint64_t _mudRevision = 0;
    int _mudWindowX = 0, _mudWindowZ = 0;
    bool _mudUploaded = false;
    bool _mudWasEnabled = false;
    // Far mud view (MudField::SnapshotFar): distant ruts, refreshed a few times a second.
    uint64_t _mudFarRevision = 0;
    int _mudFarWindowX = 0, _mudFarWindowZ = 0;
    double _mudFarUploadWallTime = 0.0;
    bool _mudFarUploaded = false;
    // BOOM-001: explosion crater views (CraterField), near (0.25 m) and far (1 m), and the
    // grass placement's nearest-crater list. Re-sent on a new crater or a window move.
    uint64_t _craterNearRevision = 0, _craterFarRevision = 0, _grassCraterRevision = 0;
    int _craterNearX = 0, _craterNearZ = 0, _craterFarX = 0, _craterFarZ = 0, _grassCraterX = 0, _grassCraterZ = 0;
    bool _craterNearUploaded = false, _craterFarUploaded = false, _grassCraterUploaded = false;
    // BOOM-004: sharp snow bullet holes, re-picked on a new mark or an 8 m camera move.
    uint64_t _snowMarksRevision = 0;
    int _snowMarksX = 0, _snowMarksZ = 0;
    bool _snowMarksUploaded = false;
    uint64_t _sandRevision = 0;
    int _sandWindowX = 0, _sandWindowZ = 0;
    bool _sandUploaded = false;
    bool _sandWasEnabled = false;
    TerrainPuddles::Wetness _puddleWetness;
    double _geographyRefreshLastMs = 0.0;
    // Reads the map's own CfgSurfaces / CfgSurfaceCharacters / clutter out of the
    // Arma-generation `config.bin` files of the archives that own THIS world's
    // terrain textures. Only those archives: merging a whole modern config tree is
    // what TerrainSurfaceCatalog exists to avoid, and an Arma 3 install has 487.
    void BuildSurfaceCatalog(const Landscape& land);
    // Decodes each authored LCA mask once and writes the per-geography-cell
    // clutter answer (grass bit + coverage + surface id) into `geography`.
    // Returns false when the world has no authored terrain materials, in which
    // case the caller's legacy per-layer classification stands untouched.
    // `refreshSelection` re-derives the dev panel's per-material selection from
    // the authored surfaces; it is set only on a world change, so a live toggle
    // is not undone by the next re-upload.
    bool BakeAuthoredGrassMask(const Landscape& land, std::vector<uint32_t>& geography, bool refreshSelection);
    // RFG-062: the same per-cell answer for a NATIVELY loaded Enfusion world, which
    // has no LCA mask to decode but does carry one authored surface index per land
    // cell and the surface's `.emat` path per palette entry. Walks the map's own
    // ClutterConfig -> set -> collection -> plant chain out of the mounted `.pak`s
    // and builds the same contiguous-run atlas the Arma path builds. Returns false
    // -- and leaves `geography` untouched -- on every other world. Implemented in
    // TerrainWgpuEnfusionClutter.cpp.
    bool BakeEnfusionClutter(const Landscape& land, std::vector<uint32_t>& geography, bool refreshSelection,
                            bool reuseDefinitions);
    // RFG-107/109: hands the grass placement the native road segments and hard-object
    // footprints per land cell, so blades stop at a ribbon's or a building's edge rather
    // than at the 12.5 m cell. Incremental: only footprints streamed in since the last call
    // are binned. Returns true when the grass has the roads (the bake then leaves road cells
    // growing). Implemented in TerrainWgpuEnfusionClutter.cpp.
    bool UploadGrassRoadCells(const Landscape& land);
  public:
    // RFG-153: Reforger's clutter plants near the camera as their own meshes (GPU-driven
    // render-only instances); the cards yield inside the radius. Once per frame on the producer
    // side (EngineWgpu::UpdateNativeClutter). TerrainWgpuNativeClutterMeshes.cpp.
    void UpdateNativeClutterMeshes(const Landscape& land, const Vector3& camera);
    // Drops every near-clutter instance and, with `forgetModels`, the cached plant models too --
    // a world unload must not keep references to shapes from content that is going away.
    void ResetNativeClutterMeshes(bool forgetModels = false);

  private:
    const Landscape* _grassRoadLand = nullptr;
    size_t _grassRoadSegments = 0;
    bool _grassRoadUploaded = false;
    std::vector<std::vector<uint32_t>> _grassShapeCells; // RFG-109: shape indices per land cell
    std::vector<float> _grassShapes;                     // 6 floats per shape (see the .cpp)
    size_t _grassFootprintsBinned = 0;
    bool _grassRoadsBinned = false;
    // PERF-GRASS-CELLS-INCREMENTAL: the flattened table kept between uploads -- new shapes go into
    // the shape table's spare capacity, a changed cell's list is appended at the tail and its
    // header word repointed, and only those ranges are sent.
    std::vector<uint32_t> _grassWords;
    size_t _grassShapeTable = 0;    // word offset of the shape table
    size_t _grassShapeCapacity = 0; // shapes it has room for
    size_t _grassTail = 0;          // first free word after the appended lists
    size_t _grassGarbage = 0;       // words of lists that were superseded by an append
    // RFG-065: reads each palette entry's `.emat` and builds the mipped detail/middle
    // ground textures it names. Cached per world; returns false and leaves the table
    // empty on every world that is not a natively loaded Reforger one. Implemented in
    // TerrainWgpuEnfusionSurface.cpp.
    bool BuildEnfusionGroundMaterials(const Landscape& land);
    // Builds the world's own clutter atlas: one layer per clutter class, the
    // classes of a surface consecutive so the shader selects within a run.
    // `surfaces` is (surface texture key, character) in the order the runs
    // should be laid down; `outRuns` maps the same key to (first, count).
    bool BuildClutterAtlas(const std::vector<std::pair<std::string, RStringB>>& surfaces,
                           std::unordered_map<std::string, std::pair<uint8_t, uint8_t>>& outRuns,
                           std::vector<uint8_t>& outLayers, uint32_t& outLayerCount);
    void UploadJitterMap(const Landscape& land);
    // Loads the global high-frequency detail noise texture (config-driven, once).
    void UploadDetailNoise();

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    // Identity of the last upload. GLandscape is a reused singleton whose data is
    // swapped in place on map switch, so the loaded terrain name is the signal
    // that a re-upload is needed.
    const Landscape* _uploaded = nullptr;
    int _uploadedRange = 0;
    uint64_t _uploadedHeightRevision = 0;
    WgrRainWaterSourceKey _rainWaterSource{};
    std::string _uploadedName;
    bool _detailNoiseTried = false;
    // Keeps the detail texture's GPU handle registered for the renderer's
    // lifetime (releasing the last Ref destroys the registry entry).
    Ref<Texture> _detailNoise;
    // OPRW25 TerrainSNX materials name extra LCA/base/overlay images outside
    // Landscape::_texture.  Keep references alive while their bindless handles
    // are resident in the renderer.
    std::vector<Ref<Texture>> _a3TerrainTextures;
    // Only terrain-page images (LCA/LCO and map layer plates) are eligible for
    // GPU eviction. Shared surface albedos stay resident because object models
    // may legitimately reference the same Texture object.
    std::vector<TextureWgpu*> _terrainPageTextures;
    bool _pagedGroundMaterials = false;
    // PERF-TERRAIN-TILE-PREP: page tiles (s_*_lco and friends) a re-centre needs that are not on the
    // GPU yet are read + LZO-decoded in the background instead of in the update; their layers use the
    // fallback until the batch is done, then the page is rebound once and takes the prepared chains.
    bool _terrainPageDeferUploads = false;
    bool _terrainPageRebindPending = false;
    std::unordered_set<std::string> _terrainTilePendingKeys;
    std::vector<std::pair<std::string, BankReadRequest>> _terrainTileBatch;
    std::vector<std::future<void>> _terrainTileTasks;
    // The deferred textures themselves: uploaded a few per update once decoded (each GPU texture
    // create is ~5-8 ms of the main thread), and the page rebound when they are all resident.
    std::vector<Ref<Texture>> _terrainTileTextures;
    // PERF-TERRAIN-QUEUED-BIND: an ordinary re-centre's bind runs on the render thread (queued), so
    // the pages it unbinds are evicted a few updates later, when the window is free.
    std::vector<Ref<Texture>> _terrainPendingEvict;
    uint32_t _terrainPendingEvictAge = 0;
    bool _terrainPageQueuedBindAllowed = false;
    bool _terrainPageCenterSet = false;
    bool QueueTerrainTilePrep(TextureWgpu* texture);
    int _terrainPageCenterX = -0x3fffffff;
    int _terrainPageCenterZ = -0x3fffffff;
    int _terrainPageRadius = 64;
    uint32_t _terrainPageLayerBudget = 1024;
    uint32_t _terrainPageTestUpdate = 0;
    int _terrainPageTestPhase = -1;
    // Cadence counter for the tracked-residency memory row emitted beside the object
    // residency rows, so the working set's coverage and its cost read from one log.
    uint32_t _residencyMemoryLogTick = 0;

    std::vector<CdlodNode> _tree;
    int _rootIndex = -1;
    int _numLevels = 0;
    float _leafSize = 0.0f;
    std::vector<float> _ranges;

    // When extended (WGR_TERRAIN_EXTENT > 1), the tree is over-sized and map-centred so
    // land continues past the map edges (clamped edge heights) to complement the
    // infinite ocean; below-sea off-map areas are pruned (the water covers them). These
    // are the tree's world-xz bounds; _extended selects the extended vs. map-only paths.
    bool _extended = false;
    float _treeMin = 0.0f;
    float _treeMax = 0.0f;

    // LOD tuning, read from the environment once at construction.
    float _baseMult;
    float _lodRatio;
    float _morphRegion;
    // Land extent past the map as a multiple of the map size (1 = map only).
    float _extentFactor;
    bool _backgroundCoverage = true;
    TerrainDrawCoverage _drawCoverage;

    std::vector<WgrTerrainNode> _selected;

    // Terrain layers are map-specific.  The explicit selection is deliberately
    // kept outside GeographyInfo so the dev panel can reclassify a surface live.
    std::vector<std::string> _grassSurfaceNames;
    // What the currently resident atlases were built from. Empty = never built.
    // A world-scoped key rather than a "done" flag, so a second world in one
    // session cannot inherit the first world's clutter.
    std::string _grassTuftWorld;
    // Display names of the uploaded photo layers, index-aligned with the atlas.
    std::vector<std::string> _grassPhotoFamilies;
    std::string _grassBladeAtlasWorld;
    std::vector<bool> _grassSurfaceEnabled;
    bool _grassNeedsCompatibilityOverride = false;
    // RFG-062: set at upload from Landscape::GetEnfusionSurfaceCount(), so the dev
    // panel can say "this world has no Enfusion surfaces" rather than offering a
    // switch that does nothing. Default ON where the world IS Enfusion.
    bool _hasEnfusionSurfaces = false;
    bool _enfusionClutterEnabled = true;
    struct EnfusionClutterRun
    {
        float weight = 0.0f;
        uint8_t layerFirst = 0;
        uint8_t layerCount = 0;
        //! RFG-138: a PlantMat surface with no ClutterConfig (crop fields) -- one transparent layer
        //! so the cell carries a run for its blades; no cards. Weight follows the bake's reference.
        bool bladesOnly = false;
    };
    // Only geography-only refreshes reuse these; world/material/UI uploads rebuild.
    std::vector<EnfusionClutterRun> _enfusionClutterRuns;
    float _enfusionClutterMaxWeight = 0.0f;
    bool _enfusionBladesEnabled = true; // RFG-089
    float _enfusionBladeHeight = 0.0f;  // RFG-090
    float _enfusionBladeTint[3] = {0.0f, 0.0f, 0.0f}; // RFG-091: mean linear albedo of the surface
    float _enfusionBladeTintLerp = 0.0f;              // RFG-091: the PlantMat's SatMapLerp
    // The Landscape::EnfusionRoadClutterClear() value the last bake honoured, so the
    // per-frame SetEnfusionClutterEnabled call can notice the dev switch moved and
    // re-bake -- the switch lives on Landscape (a world fact), not in GrassSettings.
    bool _enfusionRoadClearBaked = true;
    // RFG-065. One record per palette entry; `detail == nullptr` means the entry
    // resolved no `.emat` colour and keeps the legacy route untouched.
    struct EnfusionGroundMaterial
    {
        Texture* detail = nullptr;  //!< BCRMap, mipped, tiled at `detailScale`
        Texture* middle = nullptr;  //!< BCRMiddleMap, or null
        Texture* normal = nullptr;  //!< NHOMap, raw RG normal channels, or null
        bool puddleEligible = false; //!< Semantic palette .emat leaf, never inherited BCR/dynamic texture.
        float detailScale = 0.0f;   //!< 1 / ScaleUV, repeats per world metre
        float middleScale = 0.0f;   //!< 1 / MiddleScaleUV
        float middleBlend = 0.0f;   //!< MiddleBCRBlend, clamped to 0..1
        float detailMax = 0.0f;     //!< DetailMaxDistance, m
        float detailFade = 0.0f;    //!< DetailBlendDistance, m (the band's WIDTH)
        //! MiddleColor: a LINEAR multiplier on the middle map (RFG-071's rule), 1 when
        //! the material declares none. Applied per fragment, not baked into the tile,
        //! because the tile is shared: Dirt_01_Middle_BCR serves 12 materials under 9
        //! different MiddleColors.
        float middleColor[3] = {1.0f, 1.0f, 1.0f};
        float satBlend = 0.0f; //!< SatMapBlend, 0..1: the supertexture's share past DetailMaxDistance
        float parallax = 0.0f; //!< RFG-110: ParallaxScale over the NHO height (B), detail-UV units
    };
    std::vector<EnfusionGroundMaterial> _enfusionGround;
    //! RFG-PBR: the world's supertexture pages composited into one image (see
    //! BuildEnfusionSatmap); null on every other world or when the pages are absent.
    Ref<Texture> _enfusionSatmap;
    float _enfusionSatmapExtent = 0.0f; //!< metres the image covers
    Ref<Texture> BuildEnfusionSatmap(const Landscape& land);
    //! Native worlds use their own HBAO radius/power (see TerrainWgpuEnfusionSurface.cpp).
    void ApplyEnfusionAmbientOcclusion(bool native);
    Engine::AoSettings _enfusionAoSaved{};
    bool _enfusionAoApplied = false;
    //! Keeps the built textures alive for as long as the world is loaded.
    std::vector<Ref<Texture>> _enfusionGroundTextures;
    std::string _enfusionGroundWorld;
    bool _enfusionGroundEnabled = true;

    // The map's own clutter definitions, rebuilt per world. Empty for every OFP
    // world by construction (that generation defines none of these classes) and
    // empty whenever the owning archive ships no readable config, which is why
    // every consumer below has a texture-name fallback.
    TerrainSurfaceCatalog _surfaceCatalog;
    std::string _surfaceCatalogWorld;
    // The baked per-cell answer (bit 31 / bit 15 / coverage / surface id only),
    // kept so the dev panel's per-material toggle re-applies the selection
    // without decoding every mask on the map again.
    std::vector<uint32_t> _grassAuthoredCells;
    std::string _grassAuthoredWorld;
    // The world's clutter atlas, built during the mask bake and uploaded by
    // UploadGrassTuft. Empty on every OFP world, which keeps the loose cards.
    std::vector<uint8_t> _clutterAtlas;
    uint32_t _clutterAtlasLayers = 0;
    //! Per clutter-atlas layer: the map's selection weight and max draw distance fraction (see
    //! wgr_grass_set_layer_pick). All 0 = uniform pick, no limit -- every map that gives none.
    float _clutterLayerWeight[32] = {};
    float _clutterLayerFar[32] = {};
    //! RFG-108: world size (m) of a layer's full square when it holds a rasterised plant
    //! (0 = a texture-sheet card, stock sizing), and the plant's MinScale..MaxScale as
    //! {mean, half spread / mean}. Sent with wgr_grass_set_layer_size.
    float _clutterLayerSize[32] = {};
    float _clutterLayerScale[32][2] = {};
    //! RFG-116: bit per atlas layer whose surface names no PlantMat (wgr_grass_set_blade_skip).
    uint32_t _clutterBladeSkip = 0;
    //! RFG-138: bit per atlas layer whose run grows blades only (wgr_grass_set_blade_sets).
    uint32_t _clutterCardSkip = 0;
    //! RFG-143: bit per atlas layer whose cards are a crop field's distance card (full coverage).
    uint32_t _clutterCardFull = 0;
    //! RFG-153: the .xob each clutter-atlas layer was rasterised from ("" = a texture-sheet card
    //! or a crop distance card: no mesh), and the near-mesh ring built from them.
    std::string _clutterLayerModel[32];
    std::vector<uint32_t> _clutterGeoCopy; //!< the geography last sent to the grass (land cells)
    uint32_t _clutterMeshBakeEpoch = 0, _clutterMeshSeenBake = 0, _clutterMeshShapeEpoch = 0;
    struct ClutterMeshCell
    {
        std::vector<uint32_t> ids;
        float builtX = 0.0f, builtZ = 0.0f;
        uint32_t shapeEpoch = 0;
        bool partial = false;
    };
    std::unordered_map<int64_t, ClutterMeshCell> _clutterMeshCells;
    std::unordered_map<std::string, Ref<LODShapeWithShadow>> _clutterMeshShapes;
    std::unordered_set<std::string> _clutterMeshFailed;
    std::vector<uint32_t> _clutterMeshFreeIds;
    uint32_t _clutterMeshNextId = 0;
    float _clutterMeshRadiusSent = -1.0f;
    float _clutterMeshExclX = 1e30f, _clutterMeshExclZ = 1e30f;
    size_t _clutterMeshFootprintsSeen = 0;
    std::vector<float> _clutterMeshRoads, _clutterMeshBoxes;
    uint32_t _clutterMeshLogTick = 0;
    //! RFG-138: per-surface PlantMat blade sets. Set index per palette surface (-1 = names no
    //! PlantMat), the PlantMat per set (commonest ground first, at most 8), whether a set's
    //! surface is blades-only (crop: its authored Height in metres), and the per-set tuft
    //! parameters UploadEnfusionBladeAtlas measured, sent by SendEnfusionBladeSets.
    std::string _enfusionBladeSetKey; //!< the palette the sets below were resolved for
    std::vector<int> _enfusionBladeSetOfSurface;
    std::vector<std::string> _enfusionBladeSetPlantMats;
    std::vector<std::string> _enfusionBladeSetSurfaces;
    std::vector<bool> _enfusionBladeSetBladesOnly;
    bool _enfusionBladeSetParamsReady = false;
    float _enfusionBladeSetParams[64] = {};
    float _enfusionBladeSetAspect[64] = {};
    float _enfusionBladeSetScale[64] = {};
    void ResolveEnfusionBladeSets(const Landscape& land);
    void SendEnfusionBladeSets();

    // Terrain params UBO mirror. The static fields (grid/dims/range) are set at upload; the
    // coast wet-band fields (sea_level/time/swash/wet_*) are refreshed and pushed every frame.
    WgrTerrainParams _params{};
};

} // namespace Poseidon
