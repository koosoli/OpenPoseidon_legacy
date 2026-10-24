// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Whole world land cover: ESA WorldCover 2021 (10 m, CC-BY 4.0, (c) ESA WorldCover project / Contains modified
// Copernicus Sentinel data (2021) processed by ESA WorldCover consortium), read straight from the cloud-optimised
// GeoTIFFs ESA publishes on AWS (s3://esa-worldcover, 3 x 3 degree files, one per land cell; open sea has none).
// Only the 1024 x 1024 blocks a query touches are fetched, with HTTP range requests, and kept on disk under
// <cache>/landcover -- a 3-degree file is ~20 MB, a block ~100-500 KB. What the class is for: forest from the
// map rather than from the imagery's colour (EarthCanopyTrees), which read dark water, grass and shadow as forest.
// Worker threads only; a query may block on the network.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace Poseidon
{
class EarthLandCover
{
  public:
    // WorldCover's classes
    enum Class : uint8_t
    {
        Unknown = 0,
        Trees = 10,
        Shrubland = 20,
        Grassland = 30,
        Cropland = 40,
        BuiltUp = 50,
        Bare = 60,
        Snow = 70,
        Water = 80,
        Wetland = 90,
        Mangroves = 95,
        Moss = 100,
    };
    // The process-wide instance over POSEIDON_EARTH_CACHE, or null (no cache, or POSEIDON_EARTH_LANDCOVER=0).
    static EarthLandCover* Get();

    explicit EarthLandCover(std::filesystem::path cache);
    // The class at a place; Unknown when the data could not be had (network), Water over open sea.
    Class At(double latitude, double longitude, const std::function<bool()>& cancelled = {});

  private:
    struct File
    {
        bool present = false; // false: no file for this cell (all sea), or it could not be read
        bool failed = false;  // the network failed: tried again at retryAt
        std::chrono::steady_clock::time_point retryAt{};
        int width = 0, height = 0, tileW = 0, tileH = 0, across = 0;
        double west = 0, north = 0, step = 0; // degrees: the top-left corner and one pixel
        std::vector<uint64_t> offsets, sizes;
        std::string url;
    };
    using Block = std::vector<uint8_t>;
    File& Open(int lat0, int lon0, const std::function<bool()>& cancelled);
    bool Load(File& file, int lat0, int lon0, int tile, const std::function<bool()>& cancelled);
    bool Fetch(const std::string& url, uint64_t first, uint64_t last, std::vector<uint8_t>& bytes, long& status,
               const std::function<bool()>& cancelled);

    std::filesystem::path _dir;
    std::mutex _mutex;
    std::map<std::pair<int, int>, File> _files;
    std::map<std::tuple<int, int, int>, std::pair<Block, uint64_t>> _blocks; // (lat0, lon0, tile) -> pixels, last use
    std::map<std::tuple<int, int, int>, int> _failedBlocks;                  // a block's failures (tried again a few times)
    uint64_t _use = 0;
};
} // namespace Poseidon
