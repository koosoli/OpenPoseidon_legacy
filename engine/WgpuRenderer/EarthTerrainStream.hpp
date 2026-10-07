#pragma once

#include "EarthCoordinates.hpp"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
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
        uint64_t generation;
        unsigned downloads, cacheHits;
        double seconds;
        std::vector<float> heights;
    };
    EarthTerrainStream();
    ~EarthTerrainStream();
    EarthTerrainStream(const EarthTerrainStream&) = delete;
    EarthTerrainStream& operator=(const EarthTerrainStream&) = delete;
    void Request(float x, float z);
    std::unique_ptr<Patch> TakeReady();

  private:
    struct Tile { std::vector<float> heights; uint64_t use; };
    void Run();
    const Tile& LoadTile(int x, int y);
    float Sample(double x, double y);
    bool Download(const std::string& url, std::vector<unsigned char>& bytes);
    void TrimDisk();
    double _latitude, _longitude;
    std::filesystem::path _cache;
    std::atomic<bool> _stop{false};
    std::atomic<uint64_t> _generation{0};
    std::thread _worker;
    std::mutex _mutex;
    std::condition_variable _wake;
    int _requestX = 0, _requestZ = 0;
    bool _requested = false;
    std::unique_ptr<Patch> _ready;
    std::map<std::pair<int,int>, Tile> _tiles;
    uint64_t _use = 0;
    unsigned _downloads = 0, _cacheHits = 0;
};
}
