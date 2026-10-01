#pragma once

#include <Poseidon/Graphics/Core/IWaterRenderer.hpp>
#include <Poseidon/World/Terrain/TerrainCdlod.hpp>

#include <wgpu_renderer.hpp>

#include <string>
#include <vector>

namespace Poseidon
{
class EngineWgpu;
class Landscape;

// Draws the sea via wgpu: a flat GPU CDLOD surface at the animated sea level,
// mirroring the terrain renderer. Builds a CDLOD quadtree once per map over the
// below-sea regions (pruned by terrain min-height), and each frame emits the
// selected grid nodes as GPU instances. Coastlines depth-cut the waterline; no
// per-tile shoreline geometry and no CPU regeneration on tide/wave change.
class WaterWgpu : public IWaterRenderer
{
  public:
    WaterWgpu(EngineWgpu& engine, WgrRenderer* renderer);

    void DrawWater(Scene& scene, int xBeg, int zBeg, int xEnd, int zEnd) override;

  private:
    void QueueRainWater(bool enabled);
    uint64_t _rainWaterRevision = UINT64_MAX;
    uint64_t _rainWaterGeneration = UINT64_MAX;
    std::string _rainWaterWorld;
    bool _rainWaterEnabled = false;
    bool _rainWaterHasSurface = false;
    bool _rainWaterFineBackend = false;
    uint64_t _rainWaterSourceRevision = UINT64_MAX;
    // (Re)builds the quadtree when the map changes; returns true if it did.
    bool RebuildIfNeeded(const Landscape& land);
    void BuildQuadtree(const Landscape& land);

    EngineWgpu& _engine;
    WgrRenderer* _renderer;

    // Identity of the last build. GLandscape is a reused singleton whose data is
    // swapped in place on map switch, so the loaded terrain name is the signal that
    // a rebuild is needed (mirrors TerrainWgpu).
    const Landscape* _built = nullptr;
    int _builtRange = 0;
    uint64_t _builtHeightRevision = 0;
    std::string _builtName;

    std::vector<CdlodNode> _tree;
    int _rootIndex = -1;
    int _numLevels = 0;
    float _leafSize = 0.0f;
    std::vector<float> _ranges;

    // World-xz bounds of the (map-centred, over-sized) water tree. The per-frame
    // selection clips to these instead of the map rect, so off-map ocean out to the
    // frustum far plane is eligible; frustum + CDLOD distance + fog bound what draws.
    float _treeMin = 0.0f;
    float _treeMax = 0.0f;

    // Static placement params (world_origin/terrain_grid/hm dims), captured on build;
    // sea_level is refreshed per frame.
    WgrWaterParams _params{};
    WgrWaterInteractionParams _interaction{};
    float _lastInteractionTime = 0.0f;
    // PERF-022: the cascade preset push is per frame by design (it carries the live seed
    // override), but the five wgr_water_set_cascade_config calls behind it are direct renderer
    // calls that opened the producer window every frame (a 5-7 ms wait for the worker, named
    // by the REN-THR-013 opener log). The push now happens only when its arguments change.
    struct CascadeApplied
    {
        bool valid = false;
        int preset = 0, fft = 0;
        uint32_t seed = 0;
        float wind = 1.0f, len = 1.0f;
    } _cascadeApplied;
    bool _haveInteractionDomain = false;
    bool _interactionDemo = false;
    int _lastInteractionDemoPulse = -1;
    bool _cameraSubmerged = false;
    // Throttling state for the submersion diagnostic in Simulate(); see the log site.
    bool _loggedCameraSubmerged = false;
    float _lastSubmersionLogTime = -1000.0f;

    // A node joins the water tree when its terrain min-height dips to or below this
    // world height (highest possible sea surface + a wave-crest margin).
    float _seaThreshold = 0.0f;

    // LOD tuning. The environment value remains an expert multiplier over the live
    // Water-tab quality preset rather than silently overriding that control.
    float _baseMult;
    float _baseMultScale;
    int _activeGeometryQuality = -1;
    float _lodRatio;
    float _morphRegion;
    // How far the ocean extends past the map, as a multiple of the map size (the tree
    // is built over `_extentFactor` x the map, centred). Frustum/fog hide the outer edge.
    float _extentFactor;

    std::vector<WgrWaterNode> _selected;

    // The mean weather wind is latched before it scales wave amplitude. The authored
    // spectrum, direction, fetch, foam and water material never take weather wind.
    // Coarse steps also avoid re-deriving h0 on every frame.
    float _seaWindSpeed = 12.0f;
    float _seaWindLatchTime = -1e9f;  // sim time of the last re-derive
    bool _seaWindInit = false;
    // Returns true when the latched mean speed moved this frame.
    bool UpdateSeaWind(float now);
};

} // namespace Poseidon
