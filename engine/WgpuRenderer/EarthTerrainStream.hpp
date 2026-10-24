#pragma once

#include "EarthCoordinates.hpp"
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>
#include <Poseidon/World/Terrain/EarthProviders.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

namespace Poseidon
{
// Opt-in presentation prototype. The worker owns all network, cache, PNG decode
// and resampling work. Only completed, bounded height grids cross to the renderer.
class EarthTerrainStream
{
  public:
    struct Patch
    {
        float x, z;
        double latitude, longitude;
        double originLatitude, originLongitude;
        uint64_t generation;
        uint64_t originRevision;
        unsigned downloads, cacheHits;
        double seconds;
        std::vector<float> heights;
        float gridMetres;
        int sourceZoom;
        float horizonRadius;
        int horizonZoom;
        std::vector<float> horizonHeights;
        std::vector<uint8_t> horizonValid;
    };
    EarthTerrainStream();
    ~EarthTerrainStream();
    EarthTerrainStream(const EarthTerrainStream&) = delete;
    EarthTerrainStream& operator=(const EarthTerrainStream&) = delete;
    void Request(float x, float z, float altitude, const EarthStreaming::Location& origin);
    void Relocate(const EarthStreaming::Location& origin);
    std::unique_ptr<Patch> TakeReady();

  private:
    struct Tile { std::vector<float> heights; uint64_t use; };
    void Run();
    const Tile& LoadTile(int zoom, int x, int y);
    float Sample(double x, double y, int zoom = Earth::Zoom);
    // Sample at `zoom`, falling back one zoom at a time to `coarsest` where finer tiles are unavailable.
    float SampleFallback(const Earth::Coordinate& c, int zoom, int coarsest);
    // `height` sampled at `zoom`, with the sea's depth where it is the open sea's 0 m fill (below).
    float WithSeaDepth(const Earth::Coordinate& c, int zoom, float height);
    bool Download(const std::string& url, std::vector<unsigned char>& bytes);
    bool DownloadOnce(const std::string& url, std::vector<unsigned char>& bytes, long& status);
    void TrimDisk();
    // Downloads the listed tiles that are neither resident nor on disk, several at a time, onto disk.
    void Prefetch(const std::set<std::tuple<int,int,int>>& keys);
    std::filesystem::path TileDirectory(int zoom) const;
    EarthStreaming::Location _requestOrigin;
    uint64_t _activeGeneration = 0; // Worker-only download cancellation token.
    // Changes only with the origin (Relocate, a request in a new revision): what cancels a patch under way.
    std::atomic<uint64_t> _epoch{0};
    uint64_t _activeEpoch = 0;
    std::filesystem::path _cache;
    EarthProviders::Source _elevation; // where elevation tiles come from (providers.cfg)
    std::atomic<bool> _stop{false};
    std::atomic<uint64_t> _generation{0};
    std::thread _worker;
    std::mutex _mutex;
    std::condition_variable _wake;
    int _requestX = 0, _requestZ = 0;
    float _requestRadius = 64000.0f;
    Earth::NearDetail _requestDetail = Earth::NearDetail::Coarse;
    bool _requested = false;
    std::unique_ptr<Patch> _ready;
    std::map<std::tuple<int,int,int>, Tile> _tiles;
    // Tiles that failed to download recently: skipped for a minute instead of being retried per sample.
    std::map<std::tuple<int,int,int>, std::chrono::steady_clock::time_point> _failedTiles;
    uint64_t _use = 0;
    unsigned _downloads = 0, _cacheHits = 0;
};
}
