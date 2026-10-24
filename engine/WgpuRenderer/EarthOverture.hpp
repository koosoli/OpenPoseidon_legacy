// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Whole world buildings from Overture Maps (buildings theme: OpenStreetMap merged with Microsoft's and Google's
// machine-traced footprints, with heights, floor counts and roof shapes; ODbL / CDLA-Permissive-2.0, credit:
// "(c) OpenStreetMap contributors, Overture Maps Foundation"). Read from the release's PMTiles archive on Overture's
// CDN with HTTP range requests -- its zoom-14 vector tiles, each ~2.4 km -- and written as the same building cells
// (content-stream JSON, Overpass's shape) EarthBuildingStream already extrudes, roofs and textures. A static file
// on a CDN: no query server to time out or throttle (2026-10-24 overpass-api.de stopped answering this machine
// and the buildings with it). POSEIDON_EARTH_BUILDINGS_SOURCE=overpass goes back to Overpass.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace Poseidon
{
class EarthOverture
{
  public:
    static bool Enabled();
    static EarthOverture& Instance();
    // The building cell (EarthBuildingStream's 0.02-degree cell) on disk, from Overture's tiles: <cache>/buildings/<cell>.json,
    // which EarthBuildingStream reads before the Overpass cells. True when the cell is there (made now or before).
    bool EnsureCell(const std::filesystem::path& cache, int lat, int lon, const std::function<bool()>& cancelled);
    // The 3 x 3 cells round (lat, lon), centre first. False when Overture could not be had (the network); `changed`
    // when any cell was written.
    bool EnsureNeighborhood(const std::filesystem::path& cache, int lat, int lon, const std::function<bool()>& cancelled,
                            bool& changed);

  private:
    struct Entry
    {
        uint64_t tileId = 0, offset = 0, length = 0, runLength = 0;
    };
    struct Header
    {
        uint64_t rootOffset = 0, rootLength = 0, leafOffset = 0, dataOffset = 0;
        int minZoom = 0, maxZoom = 0;
        bool valid = false;
    };
    bool Open(const std::filesystem::path& cache, const std::function<bool()>& cancelled);
    bool Range(uint64_t offset, uint64_t length, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled, long& status);
    bool Directory(uint64_t offset, uint64_t length, std::vector<Entry>& entries, const std::function<bool()>& cancelled);
    // The gzip'd MVT bytes of tile (z, x, y), from disk or the archive; empty with true when the archive has none.
    bool Tile(int z, int x, int y, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled);

    std::mutex _mutex;
    std::filesystem::path _dir; // <cache>/overture/<release>
    std::string _release, _url;
    Header _header;
    std::vector<Entry> _root;
    std::map<uint64_t, std::vector<Entry>> _leaves; // leaf directories by offset
    bool _opened = false;
};
} // namespace Poseidon
