#pragma once
#include "EarthBuildingStream.hpp"
#include "EarthTreeMesh.hpp"
#include <array>

namespace Poseidon
{
// Independent cached OSM presentation prototype. No collision, AI,
// retail asset redistribution, terrain grading or planet-wide completeness.
class EarthFeatureStream
{
  public:
    using Ground = EarthBuildingStream::Ground;
    using Cell = EarthBuildingStream::Cell;
    using Vertex = EarthTrees::Vertex;
    enum Kind : size_t
    {
        PavedRoad,
        UnpavedRoad,
        Trees,
        Signs,
        InlandWater,
        KindCount
    };
    static constexpr size_t MaxBytes = 4 * 1024 * 1024, MaxElements = 6000, MaxPoints = 2048, MaxTriangles = 120000,
                            MaxTrees = 4000;
    struct Batch
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        size_t features = 0;
    };
    // Every tree the batch draws, as an instance for the near detail tier (EarthTreeDetail): its
    // base on the ground (frame of the batch), its height and its geographic seed.
    struct TreeInstance
    {
        float x, y, z, height;
        uint64_t seed;
        double latitude;
    };
    struct Mesh
    {
        Cell cell{};
        std::vector<TreeInstance> trees;
        uint64_t revision = 0, generation = 0;
        std::array<Batch, KindCount> batches;
        size_t skipped = 0, forestSamples = 0;
        std::string error;
    };
    static Mesh BuildCell(const std::string& json, const Ground& ground, Cell cell);
    static Mesh BuildNeighborhood(const std::filesystem::path& cache, const Ground& ground, Cell centre,
                                  const std::function<bool()>& cancelled = {});
    explicit EarthFeatureStream(std::filesystem::path cache);
    ~EarthFeatureStream();
    void Request(const EarthTerrainStream::Patch& ground, float cameraX, float cameraZ);
    void Relocate();
    std::unique_ptr<Mesh> TakeReady();

  private:
    void Run();
    std::filesystem::path _cache;
    std::thread _worker;
    std::mutex _mutex;
    std::condition_variable _wake;
    bool _stop = false, _pending = false, _requested = false;
    uint64_t _request = 0;
    Cell _cell{};
    Ground _ground{};
    std::unique_ptr<Mesh> _ready;
};
} // namespace Poseidon
