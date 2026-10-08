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
    static constexpr size_t MaxSourceBytes = 32 * 1024 * 1024, MaxSourceObjects = 131072, MaxInstances = 2048,
                            MaxModels = 64, MaxRegions = 64, MaxResidentRegions = 2,
                            MaxResidentValueBytes = 64 * 1024 * 1024;
    struct Region
    {
        uint32_t id = 0;
        std::filesystem::path manifest;
        double south = 0, west = 0, north = 0, east = 0;
    };
    struct Catalog
    {
        bool indexed = false;
        std::vector<Region> regions;
        std::string error;
    };
    struct Object
    {
        uint32_t id;
        std::string model;
        std::array<float, 12> matrix;
    };
    struct Source
    {
        struct Stamp
        {
            std::filesystem::file_time_type manifestTime{}, wrpTime{};
            uintmax_t manifestBytes = 0, wrpBytes = 0;
            bool operator==(const Stamp&) const = default;
        } stamp;
        double south = 0, west = 0, north = 0, east = 0, cellSize = 0, heightScale = 0;
        int cells = 0;
        uint32_t regionId = 0;
        size_t sourceBytes = 0, valueBytes = 0;
        std::vector<int16_t> heights;
        std::vector<Object> objects;
        std::string validation = "unspecified";
        std::string error;
    };
    struct Snapshot
    {
        uint64_t revision = 0, generation = 0;
        size_t sourceObjects = 0, coveredObjects = 0, modelLimitSkipped = 0;
        size_t residentSourceBytes = 0, residentValueBytes = 0;
        std::vector<uint32_t> regionIds;
        std::vector<Object> objects;
        std::string sourceValidation;
        std::string error;
    };
    static Catalog LoadCatalog(const std::filesystem::path& manifestOrIndex);
    static std::vector<Region> SelectRegions(const Catalog&, const EarthBuildingStream::Ground&, float cameraX,
                                             float cameraZ);
    static Source Load(const std::filesystem::path& manifest, const std::function<bool()>& cancelled = {});
    static Source LoadRegion(const Region&, const std::function<bool()>& cancelled = {});
    static Snapshot Build(const Source&, const EarthBuildingStream::Ground&, float cameraX, float cameraZ,
                          const std::function<bool()>& cancelled = {});
    static Snapshot Build(const std::vector<const Source*>&, const EarthBuildingStream::Ground&, float cameraX,
                          float cameraZ, const std::function<bool()>& cancelled = {});
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
