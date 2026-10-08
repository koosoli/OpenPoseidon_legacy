#pragma once
#include "EarthBuildingStream.hpp"
#include <array>

namespace Poseidon
{
// Local CWR-Worldgen output adapter. Values only on its worker; installed P3D
// loading and renderer ownership remain on the main thread. No network/assets.
class EarthWorldgen
{
  public:
    static constexpr size_t MaxSourceBytes = 32 * 1024 * 1024, MaxSourceObjects = 65536, MaxInstances = 2048,
                            MaxModels = 64;
    struct Object
    {
        uint32_t id;
        std::string model;
        std::array<float, 12> matrix;
    };
    struct Source
    {
        double south = 0, west = 0, north = 0, east = 0, cellSize = 0, heightScale = 0;
        int cells = 0;
        std::vector<int16_t> heights;
        std::vector<Object> objects;
        std::string error;
    };
    struct Snapshot
    {
        uint64_t revision = 0, generation = 0;
        size_t sourceObjects = 0, coveredObjects = 0, modelLimitSkipped = 0;
        std::vector<Object> objects;
        std::string error;
    };
    static Source Load(const std::filesystem::path& manifest);
    static Snapshot Build(const Source&, const EarthBuildingStream::Ground&, float cameraX, float cameraZ,
                          const std::function<bool()>& cancelled = {});
    explicit EarthWorldgen(std::filesystem::path manifest);
    ~EarthWorldgen();
    void Request(const EarthTerrainStream::Patch&, float cameraX, float cameraZ);
    void Relocate();
    std::unique_ptr<Snapshot> TakeReady();

  private:
    void Run();
    std::filesystem::path _manifest;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _worker;
    bool _stop = false, _pending = false, _requested = false;
    uint64_t _request = 0;
    EarthBuildingStream::Ground _ground{};
    float _cameraX = 0, _cameraZ = 0;
    int _cameraCellX = 0, _cameraCellZ = 0;
    std::unique_ptr<Snapshot> _ready;
};
} // namespace Poseidon
