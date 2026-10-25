// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Whole world elevation as a tile pyramid on the GPU (docs/earth-elevation-pyramid.md): the Terrarium tiles the drawn
// ground needs, with the patch builder's rules baked in -- the sea's depth from zoom 10 under the open sea's 0 m fill
// (EarthTerrainStream::WithSeaDepth), the WorldCover shore near sea level, the OFP islands -- in an RGBA8 atlas of
// 256 x 256 slots, Terrarium-encoded (exact; read with textureLoad in the vertex stage), and a lookup texture like
// the imagery's (EarthTileCache): per zoom a 16 x 16 window round the camera, slot or empty. Which tiles are wanted
// comes from the node selection (Want); a node falls back to the nearest resident ancestor.
#include "EarthImageryTiles.hpp"
#include "include/wgpu_renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace Poseidon
{
class EarthHeightCache
{
  public:
    // Atlas and lookup layout, shared with shader3d.wgsl (EH_* constants).
    static constexpr int AtlasSize = 4096, Tile = 256, SlotsPerRow = AtlasSize / Tile, Slots = SlotsPerRow * SlotsPerRow;
    static constexpr int Levels = 16, MaxZoom = 15, Window = 16, LookupWidth = Window, LookupRowsPerLevel = Window + 1;
    using Key = std::tuple<int, int, int>; // z, x, y

    explicit EarthHeightCache(WgrRenderer* renderer, const std::filesystem::path& cache);
    ~EarthHeightCache();
    EarthHeightCache(const EarthHeightCache&) = delete;
    EarthHeightCache& operator=(const EarthHeightCache&) = delete;

    bool Ready() const { return _atlas != 0 && _lookup != 0; }
    WgrTexture Atlas() const { return _atlas; }
    WgrTexture Lookup() const { return _lookup; }

    // Main thread: the camera (degrees, for the lookup windows) and the data tiles the drawn nodes want (their
    // ancestors are added). The worker fetches what is missing, coarse first.
    void Want(double latitude, double longitude, const std::vector<Key>& tiles);
    // Main thread, every frame: finished tiles into the atlas (a budget per frame) and the lookup table.
    void Install();
    // Is this tile in the atlas (installed)? Main thread.
    bool Resident(const Key& key) const { return _installed.count(key) != 0; }
    // The heights under tile (z, x, y) from it or its nearest installed ancestor: false when none is. Main thread.
    bool HeightRange(int z, int x, int y, float& lo, float& hi) const;

    static void WindowOrigin(int level, double latitude, double longitude, int& ox, int& oy);

  private:
    struct Op
    {
        int slot = -1;
        Key key{};
        std::vector<uint8_t> rgba; // Tile x Tile Terrarium-encoded
        float lo = 0, hi = 0;      // its lowest and highest height
    };
    void Run();
    bool Bake(const Key& key, std::vector<uint8_t>& rgba, float& lo, float& hi);
    bool Heights(const Key& key, std::vector<float>& heights, bool network); // decoded source heights, Tile x Tile

    WgrRenderer* _renderer = nullptr;
    WgrTexture _atlas = 0, _lookup = 0;
    std::unique_ptr<EarthImageryTiles> _tiles;
    bool _terrainRgb = false;

    std::thread _thread;
    std::atomic<bool> _stop{false};
    std::mutex _mutex;
    std::condition_variable _wake;
    std::set<Key> _wanted;     // guarded by _mutex
    uint64_t _wantedVersion = 0;
    std::deque<Op> _ops;       // guarded by _mutex

    // worker
    std::map<Key, int> _slotOf;
    std::vector<Key> _slotKey;
    std::vector<uint64_t> _slotUse;
    uint64_t _useClock = 0;
    std::map<Key, std::vector<float>> _decoded; // small: zoom-10 tiles for the sea's depth
    std::set<Key> _failed;

    // main
    std::map<Key, int> _installed;
    std::map<Key, std::pair<float, float>> _ranges; // installed tiles' lowest and highest heights
    double _latitude = 0, _longitude = 0;
    int _origins[Levels][2] = {};
    bool _lookupDirty = true;
};
} // namespace Poseidon
