#pragma once
#include "EarthTerrainStream.hpp"
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace Poseidon
{
// OSM presentation only: optional pre-tiled HTTPS source, no game assets, collision or simulation.
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
        // The coarser horizon grid around the near patch (HorizonOffset spacing): buildings beyond the near
        // patch stand on it. At ground level the near patch is only 1.6 km across.
        float centreX=0,centreZ=0,horizonRadius=0;
        std::vector<float> horizonHeights;
        std::vector<uint8_t> horizonValid;
    };
    // u picks the building's colour from the Palette (walls 0-3, pitched roofs 4-6, flat roofs 7).
    struct Vertex { float x,y,z,nx,ny,nz,u=0,v=.5f; };
    static constexpr int PaletteSize=8;
    static constexpr uint8_t Palette[PaletteSize][4]={
        {214,204,184,255},{232,228,218,255},{188,180,168,255},{202,186,160,255}, // plaster, white, grey, sand
        {146,70,50,255},{104,74,58,255},{78,80,86,255},                          // terracotta, brown, slate
        {120,120,116,255}};                                                      // flat roof
    static constexpr float PaletteU(int index){return (index+.5f)/PaletteSize;}
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
