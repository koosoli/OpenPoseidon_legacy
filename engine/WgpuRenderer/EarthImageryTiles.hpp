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
// One libcurl share (connection cache, TLS sessions, DNS) for every Earth download: imagery and
// elevation reuse warm connections instead of a TCP connect + TLS handshake per tile. A CURLSH*.
void* EarthDownloadShare();
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
               unsigned maxTiles, Mosaic& out, const std::function<bool()>& cancelled = {}, bool network = true);
    // The tiles Build would use for these bounds, fetched (disk, then network) into memory and the disk
    // cache without composing anything: getting ground ready before it is asked for. Returns how many
    // were downloaded.
    unsigned Prefetch(double south, double west, double north, double east, double metresPerPixel, unsigned maxTiles,
                      const std::function<bool()>& cancelled = {});

    // A 4096x2048 equirectangular world image from zoom-3 tiles, written once as JPEG -- only when every
    // tile arrived (a partial one would bake black holes into the globe for good). Returns the path, or
    // empty when the provider could not supply all of it this time.
    std::filesystem::path Overview();

    // Single tiles, for the tile pyramid (EarthTileCache): fetch several (disk, then the network when asked) into
    // memory, then take one out -- its decoded RGBA, which leaves this cache's memory with it.
    void FetchTiles(const std::vector<std::tuple<int, int, int>>& keys, const std::function<bool()>& cancelled, bool network);
    bool TakeTile(int z, int x, int y, std::vector<uint8_t>& rgba, int& size);
    // The provider has no imagery there (an ocean placeholder, an error page): not asked again.
    bool Failed(int z, int x, int y);

    static int ChooseZoom(double latitude, double metresPerPixel, int maxZoom);
    // Tiles downloaded so far (all time): a cheap "has anything new arrived" for progressive publishing.
    uint64_t Arrivals() const { return _arrivals.load(); }

  private:
    struct Tile
    {
        std::vector<uint8_t> rgba;
        int size = 0;
        uint64_t use = 0;
    };
    const Tile* Find(int z, int x, int y);
    // The zoom and tile range Build covers these bounds with, and its keys, nearest the centre first (the
    // download pool takes them in that order, so the ground under the camera arrives first).
    bool Cover(double& south, double west, double& north, double east, double metresPerPixel, unsigned maxTiles,
               int& zoom, int& x0, int& x1, int& y0, int& y1, std::vector<std::tuple<int, int, int>>& keys) const;
    void Fetch(const std::vector<std::tuple<int, int, int>>& keys, unsigned& downloads,
               const std::function<bool()>& cancelled, bool network);
    bool Download(const std::string& url, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled,
                  bool& permanent);
    int Perform(const std::string& url, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled,
                long& status);
    std::filesystem::path TilePath(int z, int x, int y) const;
    // The disk cache kept under its budget (POSEIDON_EARTH_IMAGERY_CACHE_MB, default 2048): the tiles least
    // recently used go first (a tile read from disk is touched).
    void TrimDisk();

    std::filesystem::path _dir;
    EarthProviders::Source _source;
    std::mutex _mutex;
    std::map<std::tuple<int, int, int>, Tile> _tiles;
    std::set<std::tuple<int, int, int>> _failed;
    uint64_t _use = 0;
    std::atomic<uint64_t> _arrivals{0};
    std::atomic<uint64_t> _trimmedAt{0}; // _arrivals at the last TrimDisk
};
} // namespace Poseidon
