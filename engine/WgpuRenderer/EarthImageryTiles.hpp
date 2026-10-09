// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/World/Terrain/EarthProviders.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

namespace Poseidon
{
// Streams web-mercator imagery tiles from a configured provider (EarthProviders) into a bounded disk
// cache and reprojects them to latitude/longitude images the Earth surface atlas blends like any
// prepared regional source. Worker-thread only; never called from the render thread.
class EarthImageryTiles
{
  public:
    struct Mosaic
    {
        std::vector<uint8_t> pixels; // RGBA, row 0 = north; alpha 0 where a tile is missing
        int width = 0, height = 0;
        double south = 0, west = 0, north = 0, east = 0;
        int zoom = 0;
        unsigned tiles = 0, downloads = 0, failures = 0;
    };

    EarthImageryTiles(std::filesystem::path cache, EarthProviders::Source source);
    bool Enabled() const { return _source.Enabled(); }
    const EarthProviders::Source& Source() const { return _source; }

    // Bounds in degrees; `metresPerPixel` is the wanted ground resolution. The zoom is the finest that
    // keeps the tile count within `maxTiles` and the provider's max zoom; output is at most maxSize².
    bool Build(double south, double west, double north, double east, double metresPerPixel, int maxSize,
               unsigned maxTiles, Mosaic& out, const std::function<bool()>& cancelled = {});

    // A 4096x2048 equirectangular world image from zoom-3 tiles, written once as JPEG. Returns the
    // path, or empty when the provider could not supply it.
    std::filesystem::path Overview();

    static int ChooseZoom(double latitude, double metresPerPixel, int maxZoom);

  private:
    struct Tile
    {
        std::vector<uint8_t> rgba;
        int size = 0;
        uint64_t use = 0;
    };
    const Tile* Find(int z, int x, int y);
    void Fetch(const std::vector<std::tuple<int, int, int>>& keys, unsigned& downloads,
               const std::function<bool()>& cancelled);
    bool Download(const std::string& url, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled,
                  bool& permanent);
    int Perform(const std::string& url, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled,
                long& status);
    std::filesystem::path TilePath(int z, int x, int y) const;

    std::filesystem::path _dir;
    EarthProviders::Source _source;
    std::mutex _mutex;
    std::map<std::tuple<int, int, int>, Tile> _tiles;
    std::set<std::tuple<int, int, int>> _failed;
    uint64_t _use = 0;
};
} // namespace Poseidon
