// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Whole world imagery as a tile pyramid (POSEIDON_EARTH_TILES=1), in the manner of CesiumJS's
// QuadtreePrimitive: every web-mercator tile the view needs, coarse to fine, kept in one BC1 tile atlas
// on the GPU, and a small lookup table that says, per zoom level, which atlas slot holds which tile in a
// 16x16 window around the camera. The ground shader (shader3d.wgsl earth_tile_albedo) picks the zoom for
// each pixel from its ground footprint and falls back to the nearest loaded ancestor. A loaded tile only
// leaves the atlas when it is no longer wanted, so ground on screen is never dropped for a coarser
// picture while its replacement loads -- the reloads, flicker and near/far/globe seams of the window
// atlases (EarthSurfaceStream) cannot happen by construction. docs/earth-whole-world.md has the design.
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
#include <thread>
#include <tuple>
#include <vector>

namespace Poseidon
{
class EarthTileCache
{
  public:
    static bool Enabled(); // POSEIDON_EARTH_TILES=1

    // Atlas layout, shared with shader3d.wgsl (ET_* constants): 30 x 30 slots of 272 texels (a 256 tile
    // and an 8-texel border of its own edge, so the 3 mip levels sampled never reach a neighbour).
    static constexpr int AtlasSize = 8192, SlotTexels = 272, Border = 8, TileTexels = 256, SlotsPerRow = 30;
    static constexpr int Slots = SlotsPerRow * SlotsPerRow, Mips = 3;
    // Lookup table: per zoom level 0..19, one row with the window's origin tile and 16 rows of 16 entries.
    static constexpr int Levels = 20, Window = 16, LookupWidth = Window, LookupRowsPerLevel = Window + 1;
    static constexpr int MaxZoom = 19;

    EarthTileCache(WgrRenderer* renderer, const std::filesystem::path& cache);
    ~EarthTileCache();
    EarthTileCache(const EarthTileCache&) = delete;
    EarthTileCache& operator=(const EarthTileCache&) = delete;

    bool Ready() const { return _atlas != 0 && _lookup != 0; }
    WgrTexture Atlas() const { return _atlas; }
    WgrTexture Lookup() const { return _lookup; }

    // Main thread, every frame: where the camera is (degrees), its height over the ground (m) and the
    // angle one pixel spans (radians). The worker re-plans from it.
    void View(double latitude, double longitude, double heightAboveGround, double pixelAngle);
    // Main thread, every frame: finished tiles into the atlas (a budget per frame), and the lookup table.
    void Install();

    struct Stats
    {
        unsigned resident = 0, wanted = 0, uploadedThisFrame = 0, queued = 0;
    };
    Stats LastStats() const { return _stats; }

  private:
    using Key = std::tuple<int, int, int>; // z, x, y
    struct Op
    {
        int slot = -1;
        Key key{};
        bool evict = false;
        std::vector<uint8_t> mips[Mips]; // BC1 blocks per mip level
    };
    struct ViewState
    {
        double latitude = 0, longitude = 0, height = 1000, pixelAngle = 0.0013;
        uint64_t version = 0;
    };
    void Run();
    std::vector<Key> Plan(const ViewState& v) const;
    static void WindowOrigin(int level, double latitude, double longitude, int& ox, int& oy);
    static std::vector<uint8_t> EncodeMip(const std::vector<uint8_t>& rgba, int size);
    // OFP islands placed in the sea (EarthIslands): their ground image laid into the tiles they overlap, as the
    // window atlases do (feathered 100 m at the edge). Where the provider has no tile (open sea), a tile is made
    // from the island image over the sea's colour.
    struct IslandImage
    {
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        double south = 0, west = 0, north = 0, east = 0;
        uint8_t sea[4] = {20, 40, 70, 255};
    };
    std::vector<IslandImage> _islands; // worker
    bool OverIsland(int z, int x, int y) const;
    void ApplyIslands(int z, int x, int y, std::vector<uint8_t>& rgba, int size) const;

    WgrRenderer* _renderer = nullptr;
    WgrTexture _atlas = 0, _lookup = 0;
    std::unique_ptr<EarthImageryTiles> _tiles;

    // worker side
    std::thread _thread;
    std::atomic<bool> _stop{false};
    std::mutex _mutex;
    std::condition_variable _wake;
    ViewState _view;              // guarded by _mutex
    std::deque<Op> _ops;          // guarded by _mutex: evictions and uploads, in order
    std::map<Key, int> _slotOf;   // worker: what each slot holds (planned, maybe not uploaded yet)
    std::vector<Key> _slotKey;    // worker
    std::vector<uint64_t> _slotUse;
    uint64_t _useClock = 0;

    // main side
    std::map<Key, int> _installed; // uploaded into the atlas
    double _lastLatitude = 0, _lastLongitude = 0;
    bool _lookupDirty = true;
    int _origins[Levels][2] = {};
    Stats _stats;
};
} // namespace Poseidon
