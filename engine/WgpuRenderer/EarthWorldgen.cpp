#include "EarthWorldgen.hpp"
#include <cjson/cJSON.h>
#include <algorithm>
#include <cstring>
#include <fstream>
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
EarthWorldgen::Source EarthWorldgen::Load(const std::filesystem::path& manifest)
{
    Source source;
    try
    {
        const auto bytes = std::filesystem::file_size(manifest);
        if (bytes > 4 * 1024 * 1024)
            throw std::runtime_error("Worldgen manifest exceeds 4MiB");
        std::ifstream jsonFile(manifest, std::ios::binary);
        std::string json(size_t(bytes), '\0');
        if (!jsonFile.read(json.data(), std::streamsize(bytes)))
            throw std::runtime_error("Worldgen manifest read failed");
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
        const auto* bbox = cJSON_GetObjectItemCaseSensitive(world, "bbox");
        if (cJSON_GetArraySize(bbox) != 4)
            throw std::runtime_error("Worldgen geographic bbox required");
        double* bounds[] = {&source.south, &source.west, &source.north, &source.east};
        for (int i = 0; i < 4; ++i)
        {
            const auto* value = cJSON_GetArrayItem(bbox, i);
            if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble))
                throw std::runtime_error("Worldgen bbox invalid");
            *bounds[i] = value->valuedouble;
        }
        source.cellSize = Number(world, "cell_size");
        source.heightScale = Number(root.get(), "height_scale");
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
        input.read(reinterpret_cast<char*>(source.heights.data()), std::streamsize(count * 2));
        input.seekg(std::streamoff(offset));
        std::unordered_set<uint32_t> ids;
        const double extent = source.cells * source.cellSize;
        bool terminated = false;
        for (size_t record = 0; record < (wrpBytes - offset) / 128; ++record)
        {
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
            source.objects.push_back(std::move(object));
        }
        if (!terminated)
            throw std::runtime_error("Worldgen RVW4 terminator invalid");
    }
    catch (const std::exception& e)
    {
        source.error = e.what();
        source.objects.clear();
        source.heights.clear();
    }
    return source;
}
EarthWorldgen::Snapshot EarthWorldgen::Build(const Source& source, const EarthBuildingStream::Ground& g, float cameraX,
                                             float cameraZ, const std::function<bool()>& cancelled)
{
    Snapshot output;
    output.revision = g.revision;
    output.generation = g.generation;
    output.sourceObjects = source.objects.size();
    output.sourceValidation = source.validation;
    output.error = source.error;
    if (!output.error.empty())
        return output;
    if (g.heights.size() != 257 * 257 || !std::isfinite(g.x) || !std::isfinite(g.z) || !std::isfinite(g.latitude) ||
        !std::isfinite(g.longitude) || std::abs(g.latitude) > 84 || !std::isfinite(g.grid) || g.grid <= 0 ||
        !std::isfinite(cameraX) || !std::isfinite(cameraZ) || source.cells < 16 || source.cells > 2048 ||
        !std::isfinite(source.cellSize) || source.cellSize <= 0 || !std::isfinite(source.heightScale) ||
        source.heightScale <= 0 || source.objects.size() > MaxSourceObjects ||
        source.heights.size() != size_t(source.cells) * source.cells)
    {
        output.error = "Worldgen resident/source ground invalid";
        return output;
    }
    struct Candidate
    {
        const Object* object;
        Earth::Coordinate coordinate;
        Earth::LocalPoint local;
        double distance;
    };
    std::vector<Candidate> candidates;
    const double extent = source.cells * source.cellSize;
    for (const auto& object : source.objects)
    {
        if (cancelled && cancelled())
        {
            output.error = "request cancelled";
            return output;
        }
        const Earth::Coordinate coordinate{source.south + object.matrix[11] * (source.north - source.south) / extent,
                                           source.west + object.matrix[9] * (source.east - source.west) / extent};
        const auto p = Earth::ToLocal(g.latitude, g.longitude, coordinate.latitude, coordinate.longitude);
        if (!p.visible || p.x < g.x || p.z < g.z || p.x > double(g.x) + 256 * g.grid ||
            p.z > double(g.z) + 256 * g.grid)
            continue;
        candidates.push_back({&object, coordinate, p, std::hypot(p.x - cameraX, p.z - cameraZ)});
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
        if (!models.count(input.model) && models.size() == MaxModels)
        {
            ++output.modelLimitSkipped;
            continue;
        }
        const float ground = GroundHeight(g, candidate.local.x, candidate.local.z);
        if (!std::isfinite(ground))
            continue;
        const double lift = input.matrix[10] - SourceHeight(source, input.matrix[9], input.matrix[11]);
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
    const auto source = Load(_manifest);
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
        auto output = std::make_unique<Snapshot>(Build(source, ground, x, z, cancelled));
        {
            std::lock_guard lock(_mutex);
            if (!_stop && request == _request)
                _ready = std::move(output);
        }
    }
}
} // namespace Poseidon
