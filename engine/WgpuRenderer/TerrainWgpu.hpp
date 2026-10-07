#pragma once

#include <Poseidon/Graphics/Core/ITerrainRenderer.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Terrain/TerrainCdlod.hpp>
#include <Poseidon/World/Terrain/TerrainSurfaceCatalog.hpp>

#include <wgpu_renderer.hpp>
#include "TerrainPuddles.hpp"
#include "EarthTerrainStream.hpp"

#include <cstdint>
#include <string>
#include <vector>

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
constexpr uint32_t AtlasMaxRun = 8;
constexpr int AtlasSize = 512;
constexpr uint32_t BakedMask = TextureCell | AuthoredCell | CoverageMask | SurfaceMask | RunCountMask;
} // namespace GrassGeo

// Draws the terrain via wgpu: uploads the heightmap once per map, builds a CDLOD
// quadtree, and each frame emits the selected grid nodes as GPU instances.
class TerrainWgpu : public ITerrainRenderer
{
  public:
    TerrainWgpu(EngineWgpu& engine, WgrRenderer* renderer);

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
    // (Re)uploads and rebuilds the quadtree when the map changes; returns true if it did.
    bool UploadIfNeeded(const Landscape& land);
    void BuildQuadtree(const Landscape& land);
    // `activeMaterials == nullptr` is the eager legacy path. Modern OPRW worlds
    // pass a near-to-far camera working set and keep the remaining materials on
    // a shared coarse surface fallback.
    void UploadGroundTextures(const Landscape& land, const std::vector<int>* activeMaterials = nullptr);
    Ref<Texture> BuildAutomaticSatmap(const Landscape& land);
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
    uint64_t _mudRevision = 0;
    int _mudWindowX = 0, _mudWindowZ = 0;
    bool _mudUploaded = false;
    bool _mudWasEnabled = false;
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
    };
    std::vector<EnfusionGroundMaterial> _enfusionGround;
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

    // Terrain params UBO mirror. The static fields (grid/dims/range) are set at upload; the
    // coast wet-band fields (sea_level/time/swash/wet_*) are refreshed and pushed every frame.
    WgrTerrainParams _params{};
};

} // namespace Poseidon
