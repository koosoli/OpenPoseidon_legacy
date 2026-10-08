#pragma once
#include "EarthTerrainStream.hpp"
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace Poseidon
{
// Cached OSM presentation only: no network, game assets, collision or simulation.
class EarthBuildingStream
{
public:
    static constexpr size_t MaxBytes=4*1024*1024, MaxBuildings=2000, MaxVertices=128, MaxTriangles=200000;
    static constexpr double CellDegrees=.02;
    struct Cell { int latitude,longitude; bool operator==(const Cell&) const=default; };
    struct Ground {
        float x,z,grid;
        double latitude,longitude;
        uint64_t revision,generation;
        std::vector<float> heights;
    };
    struct Vertex { float x,y,z,nx,ny,nz; };
    struct Mesh {
        Cell cell{};
        uint64_t revision=0,generation=0;
        size_t buildings=0,skipped=0,estimatedHeights=0;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::string error;
    };
    static Cell CellAt(double latitude,double longitude);
    static std::string FileName(Cell cell);
    static Mesh BuildCell(const std::string& json,const Ground& ground,Cell cell);
    // Cached 3x3 cell window, one shared geometry budget, no network. Checks
    // cancellation between files; cancelled output is never published by Run.
    static Mesh BuildNeighborhood(const std::filesystem::path& cache,const Ground& ground,Cell centre,
                                  const std::function<bool()>& cancelled={});
    explicit EarthBuildingStream(std::filesystem::path cache);
    ~EarthBuildingStream();
    void Request(const EarthTerrainStream::Patch& ground,float cameraX,float cameraZ);
    void Relocate();
    std::unique_ptr<Mesh> TakeReady();
private:
    void Run();
    std::filesystem::path _cache;
    std::thread _worker;
    std::mutex _mutex;
    std::condition_variable _wake;
    bool _stop=false,_pending=false,_requested=false;
    uint64_t _request=0;
    Cell _cell{};
    Ground _ground{};
    std::unique_ptr<Mesh> _ready;
};
}
