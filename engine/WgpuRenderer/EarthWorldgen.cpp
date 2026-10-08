#include "EarthWorldgen.hpp"
#include <cjson/cJSON.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_set>

namespace Poseidon
{
namespace
{
double Number(const cJSON* object, const char* key)
{
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) ? value->valuedouble : NAN;
}
void Cancel(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        throw std::runtime_error("request cancelled");
}
std::string ReadJson(const std::filesystem::path& path)
{
    const auto bytes = std::filesystem::file_size(path);
    if (bytes > 4 * 1024 * 1024)
        throw std::runtime_error("Worldgen manifest/index exceeds 4MiB");
    std::ifstream file(path, std::ios::binary);
    std::string json(size_t(bytes), '\0');
    if (!file.read(json.data(), std::streamsize(bytes)))
        throw std::runtime_error("Worldgen manifest/index read failed");
    return json;
}
void ReadBounds(const cJSON* bbox, double& south, double& west, double& north, double& east)
{
    if (!cJSON_IsArray(bbox) || cJSON_GetArraySize(bbox) != 4)
        throw std::runtime_error("Worldgen geographic bbox required");
    double* bounds[] = {&south, &west, &north, &east};
    for (int i = 0; i < 4; ++i)
    {
        const auto* value = cJSON_GetArrayItem(bbox, i);
        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble))
            throw std::runtime_error("Worldgen bbox invalid");
        *bounds[i] = value->valuedouble;
    }
    if (south < -84 || north > 84 || south >= north || west < -180 || east > 180 || west >= east)
        throw std::runtime_error("Worldgen bbox must be non-crossing within supported latitude");
}
bool Contains(double south, double west, double north, double east, Earth::Coordinate p)
{
    const double longitude = (west + east) * .5 + Earth::WrapLongitude(p.longitude - (west + east) * .5);
    return p.latitude >= south && p.latitude <= north && longitude >= west && longitude <= east;
}
bool ValidGround(const EarthBuildingStream::Ground& g, float x, float z)
{
    return g.heights.size() == 257 * 257 && std::isfinite(g.x) && std::isfinite(g.z) && std::isfinite(g.latitude) &&
           std::isfinite(g.longitude) && std::abs(g.latitude) <= 84 && std::isfinite(g.grid) && g.grid > 0 &&
           std::isfinite(x) && std::isfinite(z);
}
bool ModelPath(const std::string& path)
{
    if (path.empty() || path.size() > 75 || path.front() == '/' || path.front() == '\\' || path.find("..") != path.npos)
        return false;
    for (unsigned char c : path)
        if (c < 32 || c > 126 || c == ':' || c == '"')
            return false;
    auto suffix = path.substr(path.size() > 4 ? path.size() - 4 : 0);
    for (char& c : suffix)
        if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
    return suffix == ".p3d";
}
bool MatrixValid(const std::array<float, 12>& m)
{
    for (float v : m)
        if (!std::isfinite(v))
            return false;
    for (int axis = 0; axis < 3; ++axis)
    {
        const double length = std::hypot(std::hypot(m[axis * 3], m[axis * 3 + 1]), m[axis * 3 + 2]);
        if (length < .01 || length > 20)
            return false;
    }
    const double det = m[0] * (double(m[4]) * m[8] - double(m[5]) * m[7]) -
                       m[3] * (double(m[1]) * m[8] - double(m[2]) * m[7]) +
                       m[6] * (double(m[1]) * m[5] - double(m[2]) * m[4]);
    return det > .000001 && det < 8000;
}
float GroundHeight(const EarthBuildingStream::Ground& g, double x, double z)
{
    const double fx = (x - g.x) / g.grid, fz = (z - g.z) / g.grid;
    const int ix = std::min(255, int(fx)), iz = std::min(255, int(fz));
    const double tx = fx - ix, tz = fz - iz;
    const float a = g.heights[iz * 257 + ix], b = g.heights[iz * 257 + ix + 1], c = g.heights[(iz + 1) * 257 + ix],
                d = g.heights[(iz + 1) * 257 + ix + 1];
    return float(tx + tz <= 1 ? a + (b - a) * tx + (c - a) * tz : d + (c - d) * (1 - tx) + (b - d) * (1 - tz));
}
double GroundRoadLength(const EarthWorldgen::Object& object)
{
    std::string model = object.model;
    for (char& c : model)
    {
        if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
        if (c == '\\')
            c = '/';
    }
    double length = 0;
    for (const char* family : {"sil", "ces"})
        for (const auto& piece : {std::pair{"25", 25.0}, std::pair{"12", 12.5}, std::pair{"6", 6.25}})
            if (model == std::string("o/road/") + family + piece.first + ".p3d")
                length = piece.second;
    const auto& m = object.matrix;
    // Only the flat unit-scale stock-road records emitted by the terrain-road
    // fitter are supported. A marked bridge, curve, pitched/scaled road is refused.
    if (!length || std::abs(m[1]) > 1e-4 || std::abs(m[7]) > 1e-4 || std::abs(m[3]) > 1e-4 ||
        std::abs(m[4] - 1) > 1e-4 || std::abs(m[5]) > 1e-4 || std::abs(std::hypot(m[0], m[2]) - 1) > 1e-4 ||
        std::abs(std::hypot(m[6], m[8]) - 1) > 1e-4 || std::abs(m[0] * m[6] + m[2] * m[8]) > 1e-4)
        return 0;
    return length;
}
bool FitGroundRoad(EarthWorldgen::Object& object, const EarthBuildingStream::Ground& g, double length, double centre)
{
    auto& m = object.matrix;
    const auto sample = [&](double x, double z) -> double
    {
        if (x < g.x || z < g.z || x > g.x + 256 * g.grid || z > g.z + 256 * g.grid)
            return NAN;
        return double(GroundHeight(g, x, z));
    };
    const double half = length * .5;
    const double forward = sample(m[9] + half * m[6], m[11] + half * m[8]);
    const double back = sample(m[9] - half * m[6], m[11] - half * m[8]);
    const double right = sample(m[9] + 5 * m[0], m[11] + 5 * m[2]);
    const double left = sample(m[9] - 5 * m[0], m[11] - 5 * m[2]);
    if (!std::isfinite(centre) || !std::isfinite(forward) || !std::isfinite(back) || !std::isfinite(right) ||
        !std::isfinite(left))
        return false;
    // An affine plane preserves the complete horizontal footprint and stable
    // instance identity. It approximates the resident triangles; it does not
    // grade terrain, deform each road vertex, or fill junctions.
    m[10] = float(centre + .02);
    m[1] = float((right - left) / 10);
    m[7] = float((forward - back) / length);
    return MatrixValid(m);
}
double SourceHeight(const EarthWorldgen::Source& source, double x, double z)
{
    // Worldgen placement heights use its quantized source grid. Preserve their
    // relative lift/burial while replacing that grid with resident Earth ground.
    const double fx = std::clamp(x / source.cellSize, 0.0, double(source.cells - 1)),
                 fz = std::clamp(z / source.cellSize, 0.0, double(source.cells - 1));
    const int ix = int(fx), iz = int(fz), jx = std::min(ix + 1, source.cells - 1),
              jz = std::min(iz + 1, source.cells - 1);
    const double tx = fx - ix, tz = fz - iz;
    return source.heightScale *
           ((1 - tz) *
                ((1 - tx) * source.heights[iz * source.cells + ix] + tx * source.heights[iz * source.cells + jx]) +
            tz * ((1 - tx) * source.heights[jz * source.cells + ix] + tx * source.heights[jz * source.cells + jx]));
}
} // namespace
EarthWorldgen::Catalog EarthWorldgen::LoadCatalog(const std::filesystem::path& path)
{
    Catalog catalog;
    try
    {
        const auto json = ReadJson(path);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(json.data(), json.size()),
                                                             cJSON_Delete);
        const auto* version = cJSON_GetObjectItemCaseSensitive(root.get(), "earth_worldgen_regions");
        if (!version)
        {
            Region region;
            region.manifest = path;
            ReadBounds(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root.get(), "world"), "bbox"),
                       region.south, region.west, region.north, region.east);
            catalog.regions.push_back(std::move(region));
            return catalog;
        }
        catalog.indexed = true;
        const auto* regions = cJSON_GetObjectItemCaseSensitive(root.get(), "regions");
        if (json.size() > 64 * 1024 || !cJSON_IsNumber(version) || version->valuedouble != 1 ||
            !cJSON_IsArray(regions) || cJSON_GetArraySize(regions) < 1 || cJSON_GetArraySize(regions) > MaxRegions)
            throw std::runtime_error("Worldgen region index version/count/64KiB budget invalid");
        const auto base = std::filesystem::weakly_canonical(path.parent_path());
        std::unordered_set<uint32_t> ids;
        std::unordered_set<std::string> paths;
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, regions)
        {
            const double id = Number(entry, "id");
            const auto* manifest = cJSON_GetObjectItemCaseSensitive(entry, "manifest");
            if (!std::isfinite(id) || id < 1 || id > 255 || std::floor(id) != id || !ids.insert(uint32_t(id)).second ||
                !cJSON_IsString(manifest))
                throw std::runtime_error("Worldgen region ID/path invalid or duplicate");
            const std::string name = manifest->valuestring;
            if (name.empty() || name.size() > 512 || name.front() == '/' || name.front() == '\\' ||
                name.find(':') != name.npos || name.find("..") != name.npos)
                throw std::runtime_error("Worldgen region manifest must be a contained relative path");
            for (unsigned char c : name)
                if (c < 32)
                    throw std::runtime_error("Worldgen region manifest path contains control characters");
            Region region;
            region.id = uint32_t(id);
            region.manifest = std::filesystem::weakly_canonical(
                base / std::filesystem::path(std::u8string(name.begin(), name.end())));
            const auto relative = region.manifest.lexically_relative(base);
            if (relative.empty() || *relative.begin() == ".." || !paths.insert(region.manifest.generic_string()).second)
                throw std::runtime_error("Worldgen region manifest escapes index directory or duplicates a path");
            ReadBounds(cJSON_GetObjectItemCaseSensitive(entry, "bbox"), region.south, region.west, region.north,
                       region.east);
            catalog.regions.push_back(std::move(region));
        }
        std::sort(catalog.regions.begin(), catalog.regions.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
    }
    catch (const std::exception& e)
    {
        catalog.error = e.what();
        catalog.regions.clear();
    }
    return catalog;
}
std::vector<EarthWorldgen::Region>
EarthWorldgen::SelectRegions(const Catalog& catalog, const EarthBuildingStream::Ground& g, float cameraX, float cameraZ)
{
    if (!catalog.error.empty() || !ValidGround(g, cameraX, cameraZ))
        return {};
    const auto camera = Earth::FromLocal(g.latitude, g.longitude, cameraX, cameraZ);
    double reach = 0;
    for (double x : {double(g.x), double(g.x) + 256 * g.grid})
        for (double z : {double(g.z), double(g.z) + 256 * g.grid})
            reach = std::max(reach, std::hypot(x - cameraX, z - cameraZ));
    reach = std::min(20000.0, reach + 512); // Fixed neighborhood bound, including request-cell movement.
    std::vector<std::pair<double, const Region*>> candidates;
    for (const auto& region : catalog.regions)
    {
        const double middle = (region.west + region.east) * .5;
        const double longitude = middle + Earth::WrapLongitude(camera.longitude - middle);
        const auto p =
            Earth::ToLocal(camera.latitude, camera.longitude, std::clamp(camera.latitude, region.south, region.north),
                           Earth::WrapLongitude(std::clamp(longitude, region.west, region.east)));
        const double distance = std::hypot(p.x - Earth::Anchor, p.z - Earth::Anchor);
        if (p.visible && distance <= reach)
            candidates.push_back({distance, &region});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b)
              { return a.first != b.first ? a.first < b.first : a.second->id < b.second->id; });
    std::vector<Region> selected;
    for (const auto& candidate : candidates)
    {
        if (selected.size() == MaxResidentRegions)
            break;
        selected.push_back(*candidate.second);
    }
    return selected;
}
EarthWorldgen::Source EarthWorldgen::Load(const std::filesystem::path& manifest, const std::function<bool()>& cancelled)
{
    Source source;
    try
    {
        Cancel(cancelled);
        source.stamp.manifestTime = std::filesystem::last_write_time(manifest);
        source.stamp.manifestBytes = std::filesystem::file_size(manifest);
        const auto json = ReadJson(manifest);
        Cancel(cancelled);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(json.data(), json.size()),
                                                             cJSON_Delete);
        const auto* world = cJSON_GetObjectItemCaseSensitive(root.get(), "world");
        const auto* validation = cJSON_GetObjectItemCaseSensitive(root.get(), "final_validation");
        const auto* status = cJSON_GetObjectItemCaseSensitive(validation, "status");
        if (cJSON_IsString(status) &&
            (std::strcmp(status->valuestring, "failed") == 0 || std::strcmp(status->valuestring, "succeeded") == 0))
            source.validation = status->valuestring;
        const auto* name = cJSON_GetObjectItemCaseSensitive(world, "name");
        if (!cJSON_IsString(name))
            throw std::runtime_error("Worldgen world name missing");
        const std::string worldName = name->valuestring;
        if (worldName.empty() || worldName.size() > 20)
            throw std::runtime_error("Worldgen world name invalid");
        for (char c : worldName)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                throw std::runtime_error("Worldgen world name unsafe");
        ReadBounds(cJSON_GetObjectItemCaseSensitive(world, "bbox"), source.south, source.west, source.north,
                   source.east);
        source.cellSize = Number(world, "cell_size");
        source.heightScale = Number(root.get(), "height_scale");
        std::unordered_set<uint32_t> groundRoadIds;
        if (const auto* listed = cJSON_GetObjectItemCaseSensitive(root.get(), "earth_ground_road_ids"))
        {
            if (!cJSON_IsArray(listed) || cJSON_GetArraySize(listed) > MaxSourceObjects)
                throw std::runtime_error("Worldgen ground-road classification budget/array invalid");
            const cJSON* id = nullptr;
            cJSON_ArrayForEach(id, listed)
            {
                if (!cJSON_IsNumber(id) || !std::isfinite(id->valuedouble) || id->valuedouble < 0 ||
                    id->valuedouble > INT32_MAX || std::floor(id->valuedouble) != id->valuedouble ||
                    !groundRoadIds.insert(uint32_t(id->valuedouble)).second)
                    throw std::runtime_error("Worldgen ground-road classification ID invalid/duplicate");
            }
        }
        const double cells = Number(world, "cells");
        if (!std::isfinite(cells) || cells < 16 || cells > 2048 || std::floor(cells) != cells ||
            !std::isfinite(source.cellSize) || source.cellSize <= 0 || source.cellSize > 1000 ||
            !std::isfinite(source.heightScale) || source.heightScale <= 0 || source.heightScale > 10 ||
            source.south < -84 || source.north > 84 || source.south >= source.north || source.west < -180 ||
            source.east > 180 || source.west >= source.east)
            throw std::runtime_error("Worldgen geographic/grid contract invalid");
        source.cells = int(cells);
        // The current Worldgen single-world-PBO policy nests the original
        // source under its addon prefix. Older outputs keep the flat layout.
        auto wrp = manifest.parent_path() / "source" / worldName / (worldName + ".wrp");
        if (!std::filesystem::is_regular_file(wrp))
            wrp = manifest.parent_path() / "source" / (worldName + ".wrp");
        const auto wrpBytes = std::filesystem::file_size(wrp);
        source.stamp.wrpTime = std::filesystem::last_write_time(wrp);
        source.stamp.wrpBytes = wrpBytes;
        source.sourceBytes = size_t(wrpBytes);
        const size_t count = size_t(source.cells) * source.cells, offset = 12 + count * 4 + 512 * 32;
        if (wrpBytes > MaxSourceBytes || wrpBytes < offset + 128 || (wrpBytes - offset) % 128 ||
            (wrpBytes - offset) / 128 > MaxSourceObjects + 1)
            throw std::runtime_error("Worldgen RVW4 source budget/length invalid");
        std::ifstream input(wrp, std::ios::binary);
        char magic[4]{};
        int32_t width = 0, height = 0;
        input.read(magic, 4);
        input.read(reinterpret_cast<char*>(&width), 4);
        input.read(reinterpret_cast<char*>(&height), 4);
        if (std::memcmp(magic, "4WVR", 4) || width != source.cells || height != source.cells)
            throw std::runtime_error("Worldgen RVW4 header mismatch");
        source.heights.resize(count);
        for (size_t first = 0; first < count; first += 262144)
        {
            Cancel(cancelled);
            input.read(reinterpret_cast<char*>(source.heights.data() + first),
                       std::streamsize(std::min(size_t(262144), count - first) * 2));
            if (!input)
                throw std::runtime_error("Worldgen RVW4 heights truncated");
        }
        input.seekg(std::streamoff(offset));
        std::unordered_set<uint32_t> ids;
        const double extent = source.cells * source.cellSize;
        bool terminated = false;
        source.objects.reserve((wrpBytes - offset) / 128 - 1);
        for (size_t record = 0; record < (wrpBytes - offset) / 128; ++record)
        {
            if (record % 256 == 0)
                Cancel(cancelled);
            Object object{};
            char model[76]{};
            input.read(reinterpret_cast<char*>(object.matrix.data()), 48);
            input.read(reinterpret_cast<char*>(&object.id), 4);
            input.read(model, 76);
            if (!input)
                throw std::runtime_error("Worldgen RVW4 truncated");
            if (!model[0])
            {
                terminated = record + 1 == (wrpBytes - offset) / 128;
                break;
            }
            const auto* end = static_cast<const char*>(std::memchr(model, 0, 76));
            if (!end)
                throw std::runtime_error("Worldgen RVW4 model unterminated");
            object.model.assign(model, size_t(end - model));
            if (!ModelPath(object.model) || !MatrixValid(object.matrix) || !ids.insert(object.id).second ||
                object.id > INT32_MAX || object.matrix[9] < 0 || object.matrix[9] > extent || object.matrix[11] < 0 ||
                object.matrix[11] > extent || std::abs(object.matrix[10]) > 100000)
                throw std::runtime_error("Worldgen RVW4 object invalid");
            object.groundRoad = groundRoadIds.erase(object.id) != 0;
            if (object.groundRoad && !GroundRoadLength(object))
                throw std::runtime_error("Worldgen ground-road ID is not a flat unit stock straight road");
            source.objects.push_back(std::move(object));
        }
        if (!terminated)
            throw std::runtime_error("Worldgen RVW4 terminator invalid");
        if (!groundRoadIds.empty())
            throw std::runtime_error("Worldgen ground-road classification ID missing from RVW4");
        if (std::filesystem::last_write_time(manifest) != source.stamp.manifestTime ||
            std::filesystem::file_size(manifest) != source.stamp.manifestBytes ||
            std::filesystem::last_write_time(wrp) != source.stamp.wrpTime ||
            std::filesystem::file_size(wrp) != source.stamp.wrpBytes)
            throw std::runtime_error("Worldgen source changed during read; restart with immutable prepared data");
        source.valueBytes =
            sizeof(Source) + source.heights.capacity() * sizeof(int16_t) + source.objects.capacity() * sizeof(Object);
        for (const auto& object : source.objects)
            source.valueBytes += object.model.capacity() + 1; // Conservative: also counts inline strings.
        if (source.valueBytes > MaxResidentValueBytes / MaxResidentRegions)
            throw std::runtime_error("Worldgen source value-memory budget exceeded");
    }
    catch (const std::exception& e)
    {
        source.error = e.what();
        std::vector<Object>().swap(source.objects);
        std::vector<int16_t>().swap(source.heights);
        source.valueBytes = 0;
    }
    return source;
}
EarthWorldgen::Source EarthWorldgen::LoadRegion(const Region& region, const std::function<bool()>& cancelled)
{
    auto source = Load(region.manifest, cancelled);
    if (!source.error.empty())
        return source;
    if (region.id > 255 || !std::isfinite(region.south) || !std::isfinite(region.west) ||
        !std::isfinite(region.north) || !std::isfinite(region.east) || std::abs(source.south - region.south) > 1e-9 ||
        std::abs(source.west - region.west) > 1e-9 || std::abs(source.north - region.north) > 1e-9 ||
        std::abs(source.east - region.east) > 1e-9)
    {
        source.error = "Worldgen index/manifest geographic bounds mismatch";
        std::vector<Object>().swap(source.objects);
        std::vector<int16_t>().swap(source.heights);
        source.valueBytes = 0;
        return source;
    }
    source.regionId = region.id;
    if (region.id)
    {
        std::sort(source.objects.begin(), source.objects.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        for (size_t rank = 0; rank < source.objects.size(); ++rank)
            source.objects[rank].id = (region.id << 24) | uint32_t(rank);
    }
    return source;
}
EarthWorldgen::Snapshot EarthWorldgen::Build(const Source& source, const EarthBuildingStream::Ground& g, float cameraX,
                                             float cameraZ, const std::function<bool()>& cancelled)
{
    return Build(std::vector<const Source*>{&source}, g, cameraX, cameraZ, cancelled);
}
EarthWorldgen::Snapshot EarthWorldgen::Build(const std::vector<const Source*>& sources,
                                             const EarthBuildingStream::Ground& g, float cameraX, float cameraZ,
                                             const std::function<bool()>& cancelled)
{
    Snapshot output;
    output.revision = g.revision;
    output.generation = g.generation;
    if (!ValidGround(g, cameraX, cameraZ) || sources.size() > MaxResidentRegions)
    {
        output.error = "Worldgen resident/source ground invalid";
        return output;
    }
    std::vector<const Source*> valid;
    std::unordered_set<uint32_t> regionIds;
    output.sourceValidation = "succeeded";
    for (const auto* source : sources)
    {
        if (!source || !source->error.empty())
        {
            output.error += (output.error.empty() ? "" : "; ") +
                            (source ? source->error.substr(0, 1024) : "Worldgen source missing");
            continue;
        }
        if (source->cells < 16 || source->cells > 2048 || !std::isfinite(source->cellSize) || source->cellSize <= 0 ||
            !std::isfinite(source->heightScale) || source->heightScale <= 0 ||
            source->objects.size() > MaxSourceObjects ||
            source->heights.size() != size_t(source->cells) * source->cells || source->sourceBytes > MaxSourceBytes ||
            source->valueBytes > MaxResidentValueBytes / MaxResidentRegions ||
            !regionIds.insert(source->regionId).second)
        {
            output.error = "Worldgen resident/source ground invalid";
            return output;
        }
        valid.push_back(source);
        output.sourceObjects += source->objects.size();
        output.residentSourceBytes += source->sourceBytes;
        output.residentValueBytes += source->valueBytes;
        output.regionIds.push_back(source->regionId);
        if (source->validation == "failed")
            output.sourceValidation = "failed";
        else if (source->validation != "succeeded" && output.sourceValidation != "failed")
            output.sourceValidation = "unspecified";
    }
    if (valid.empty())
        output.sourceValidation = "unspecified";
    if (output.residentSourceBytes > MaxSourceBytes * MaxResidentRegions ||
        output.residentValueBytes > MaxResidentValueBytes)
    {
        output.error = "Worldgen aggregate source budget exceeded";
        return output;
    }
    struct Candidate
    {
        const Object* object;
        const Source* source;
        Earth::Coordinate coordinate;
        Earth::LocalPoint local;
        double distance;
    };
    std::vector<Candidate> candidates;
    for (const auto* source : valid)
    {
        const double extent = source->cells * source->cellSize;
        for (const auto& object : source->objects)
        {
            if (cancelled && cancelled())
            {
                output.error = "request cancelled";
                return output;
            }
            const Earth::Coordinate coordinate{
                source->south + object.matrix[11] * (source->north - source->south) / extent,
                source->west + object.matrix[9] * (source->east - source->west) / extent};
            const bool shadowed =
                std::any_of(valid.begin(), valid.end(),
                            [&](const auto* other)
                            {
                                return other->regionId < source->regionId &&
                                       Contains(other->south, other->west, other->north, other->east, coordinate);
                            });
            if (shadowed)
                continue;
            const auto p = Earth::ToLocal(g.latitude, g.longitude, coordinate.latitude, coordinate.longitude);
            if (!p.visible || p.x < g.x || p.z < g.z || p.x > double(g.x) + 256 * g.grid ||
                p.z > double(g.z) + 256 * g.grid)
                continue;
            candidates.push_back({&object, source, coordinate, p, std::hypot(p.x - cameraX, p.z - cameraZ)});
        }
    }
    output.coveredObjects = candidates.size();
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b)
              { return a.distance != b.distance ? a.distance < b.distance : a.object->id < b.object->id; });
    std::unordered_set<std::string> models;
    for (const auto& candidate : candidates)
    {
        if (output.objects.size() == MaxInstances)
            break;
        const auto& input = *candidate.object;
        const auto& source = *candidate.source;
        const double extent = source.cells * source.cellSize;
        if (!models.count(input.model) && models.size() == MaxModels)
        {
            ++output.modelLimitSkipped;
            continue;
        }
        const float ground = GroundHeight(g, candidate.local.x, candidate.local.z);
        if (!std::isfinite(ground))
            continue;
        const double lift =
            input.groundRoad ? 0 : input.matrix[10] - SourceHeight(source, input.matrix[9], input.matrix[11]);
        const auto normal =
            Earth::GlobeNormal(g.latitude, g.longitude, candidate.coordinate.latitude, candidate.coordinate.longitude);
        Object object = input;
        object.matrix[9] = float(candidate.local.x + normal.x * lift);
        object.matrix[10] = float(ground + normal.y * lift);
        object.matrix[11] = float(candidate.local.z + normal.z * lift);
        const double eastScale = Earth::Radius * Earth::Pi / 180 *
                                 std::cos(candidate.coordinate.latitude * Earth::Pi / 180) *
                                 (source.east - source.west) / extent,
                     northScale = Earth::Radius * Earth::Pi / 180 * (source.north - source.south) / extent;
        for (int axis = 0; axis < 3; ++axis)
        {
            const auto direction = Earth::RotateFrame(candidate.coordinate, {g.latitude, g.longitude},
                                                      {input.matrix[axis * 3] * eastScale, input.matrix[axis * 3 + 1],
                                                       input.matrix[axis * 3 + 2] * northScale});
            object.matrix[axis * 3] = float(direction.x);
            object.matrix[axis * 3 + 1] = float(direction.y);
            object.matrix[axis * 3 + 2] = float(direction.z);
        }
        if (input.groundRoad)
        {
            const double length = GroundRoadLength(input);
            if (!length || !FitGroundRoad(object, g, length, ground))
            {
                ++output.groundRoadCoverageSkipped;
                continue;
            }
            ++output.groundRoadsFitted;
        }
        if (!MatrixValid(object.matrix))
            continue;
        models.insert(input.model);
        output.objects.push_back(std::move(object));
    }
    return output;
}
EarthWorldgen::EarthWorldgen(std::filesystem::path manifest) : _manifest(std::move(manifest))
{
    _worker = std::thread([this] { Run(); });
}
EarthWorldgen::~EarthWorldgen()
{
    {
        std::lock_guard lock(_mutex);
        _stop = true;
        ++_request;
    }
    _wake.notify_one();
    _worker.join();
}
void EarthWorldgen::Request(const EarthTerrainStream::Patch& p, float x, float z)
{
    if (!std::isfinite(x) || !std::isfinite(z) || std::abs(x) > 1000000 || std::abs(z) > 1000000)
        return;
    std::lock_guard lock(_mutex);
    const int cx = int(std::floor(x / 256)), cz = int(std::floor(z / 256));
    if (_requested && _ground.revision == p.originRevision && _ground.generation == p.generation &&
        _cameraCellX == cx && _cameraCellZ == cz)
        return;
    _ground = {p.x, p.z, p.gridMetres, p.originLatitude, p.originLongitude, p.originRevision, p.generation, p.heights};
    _cameraX = x;
    _cameraZ = z;
    _cameraCellX = cx;
    _cameraCellZ = cz;
    _requested = true;
    _pending = true;
    _ready.reset();
    ++_request;
    _wake.notify_one();
}
void EarthWorldgen::Relocate()
{
    std::lock_guard lock(_mutex);
    ++_request;
    _ready.reset();
    _pending = false;
    _requested = false;
}
std::unique_ptr<EarthWorldgen::Snapshot> EarthWorldgen::TakeReady()
{
    std::lock_guard lock(_mutex);
    return std::move(_ready);
}
void EarthWorldgen::Run()
{
    const auto catalog = LoadCatalog(_manifest);
    // Worker-only ownership: eviction precedes loads, so old regions do not
    // accumulate while travelling through a large prepared index.
    std::map<uint32_t, Source> resident;
    std::map<uint32_t, Source::Stamp> pinned;
    for (;;)
    {
        EarthBuildingStream::Ground ground;
        float x, z;
        uint64_t request;
        {
            std::unique_lock lock(_mutex);
            _wake.wait(lock, [this] { return _stop || _pending; });
            if (_stop)
                return;
            ground = _ground;
            x = _cameraX;
            z = _cameraZ;
            request = _request;
            _pending = false;
        }
        const auto cancelled = [this, request]
        {
            std::lock_guard lock(_mutex);
            return _stop || _request != request;
        };
        const auto selected = SelectRegions(catalog, ground, x, z);
        for (auto it = resident.begin(); it != resident.end();)
        {
            if (std::none_of(selected.begin(), selected.end(),
                             [&](const auto& region) { return region.id == it->first; }))
                it = resident.erase(it);
            else
                ++it;
        }
        for (const auto& region : selected)
        {
            if (cancelled())
                break;
            if (!resident.count(region.id))
            {
                auto source = LoadRegion(region, cancelled);
                if (cancelled())
                    break;
                if (source.error.empty())
                {
                    const auto [it, inserted] = pinned.emplace(region.id, source.stamp);
                    if (!inserted && !(it->second == source.stamp))
                    {
                        source = Source{};
                        source.error = "prepared source changed during run; restart to reload";
                    }
                }
                if (!source.error.empty())
                    source.error = "region " + std::to_string(region.id) + ": " + source.error;
                resident.emplace(region.id, std::move(source));
            }
        }
        if (cancelled())
            continue;
        std::vector<const Source*> sources;
        for (const auto& region : selected)
            sources.push_back(&resident.at(region.id));
        auto output = std::make_unique<Snapshot>(Build(sources, ground, x, z, cancelled));
        if (!catalog.error.empty())
            output->error = catalog.error;
        {
            std::lock_guard lock(_mutex);
            if (!_stop && request == _request)
                _ready = std::move(output);
        }
    }
}
} // namespace Poseidon
