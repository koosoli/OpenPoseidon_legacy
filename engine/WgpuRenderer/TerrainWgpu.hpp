#pragma once

#include <Poseidon/Graphics/Core/ITerrainRenderer.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Terrain/TerrainCdlod.hpp>
#include <Poseidon/World/Terrain/TerrainSurfaceCatalog.hpp>

#include <wgpu_renderer.hpp>

#include <string>
#include <vector>

namespace Poseidon
{
class EngineWgpu;
class Landscape;

// Draws the terrain via wgpu: uploads the heightmap once per map, builds a CDLOD
// quadtree, and each frame emits the selected grid nodes as GPU instances.
class TerrainWgpu : public ITerrainRenderer
{
  public:
    TerrainWgpu(EngineWgpu& engine, WgrRenderer* renderer);

    void DrawTerrain(Scene& scene, int xBeg, int zBeg, int xEnd, int zEnd) override;
    int GrassSurfaceCount() const;
    // GRS-E: upload the game's photographed grass tuft for the mid LOD.
    // `worldKey` identifies what the atlas content is derived from; the upload is
    // skipped while it is unchanged, and repeated when it is not. See the call
    // site in UploadGeography for why this is a key and not a bool.
    void UploadGrassTuft(const std::string& worldKey);
    // GRS-F: upload eight opaque photo layers for the near blade geometry.
    void UploadGrassBladeAtlas(const std::string& worldKey);
    const char* GrassLoadedMapName() const { return _uploadedName.c_str(); }
    const char* GrassSurfaceName(int index) const;
    bool GrassSurfaceEnabled(int index) const;
    void SetGrassSurfaceEnabled(int index, bool enabled);
    // Eden/Everon ships legacy geography flags that can classify all normal
    // terrain as excluded. Keep this map-specific compatibility decision next
    // to the uploaded terrain data, not in UI state.
    bool GrassNeedsCompatibilityOverride() const { return _grassNeedsCompatibilityOverride; }
    // Photographed clump families actually found on disk, in atlas-layer order
    // (index 0 = the primary clump). Discovered, never hardcoded, so dropping a
    // grass pack into assets/grass/more_grass is all it takes to get one.
    int GrassPhotoFamilyCount() const { return static_cast<int>(_grassPhotoFamilies.size()); }
    const char* GrassPhotoFamilyName(int index) const;

  private:
    // (Re)uploads and rebuilds the quadtree when the map changes; returns true if it did.
    bool UploadIfNeeded(const Landscape& land);
    void BuildQuadtree(const Landscape& land);
    void UploadGroundTextures(const Landscape& land);
    void UploadIndexMap(const Landscape& land);
    void UploadGeography(const Landscape& land);
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
    std::string _uploadedName;
    bool _detailNoiseTried = false;
    // Keeps the detail texture's GPU handle registered for the renderer's
    // lifetime (releasing the last Ref destroys the registry entry).
    Ref<Texture> _detailNoise;
    // OPRW25 TerrainSNX materials name extra LCA/base/overlay images outside
    // Landscape::_texture.  Keep references alive while their bindless handles
    // are resident in the renderer.
    std::vector<Ref<Texture>> _a3TerrainTextures;

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
