#include "EarthIslands.hpp"
#include "EarthCoordinates.hpp"
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <cjson/cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace Poseidon::EarthIslands
{
namespace
{
// Outer cells over which an island's own sea floor fades into the source elevation.
constexpr double BlendCells = 6; // the preparation script already deepens the sea floor away from the island

double Number(const cJSON* root, const char* name)
{
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(value) ? value->valuedouble : NAN;
}

std::vector<Island> Load()
{
    std::vector<Island> result;
    const char* cache = std::getenv("POSEIDON_EARTH_CACHE");
    if (!cache || !*cache) return result;
    std::error_code ec;
    const auto root = std::filesystem::u8path(cache) / "islands";
    if (!std::filesystem::is_directory(root, ec)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        const auto dir = entry.path();
        const auto json = dir / "island.json";
        try
        {
            if (!std::filesystem::is_regular_file(json) || std::filesystem::file_size(json) > 65536) continue;
            std::ifstream file(json, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), {});
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> doc(cJSON_Parse(text.c_str()), cJSON_Delete);
            if (!doc || Number(doc.get(), "earth_island") != 1) throw std::runtime_error("not an island.json");
            Island island;
            const auto* name = cJSON_GetObjectItemCaseSensitive(doc.get(), "name");
            island.name = cJSON_IsString(name) ? std::string(name->valuestring) : dir.filename().string();
            island.latitude = Number(doc.get(), "latitude");
            island.longitude = Number(doc.get(), "longitude");
            island.cellSize = Number(doc.get(), "cell_size");
            const double cells = Number(doc.get(), "cells");
            const auto* bounds = cJSON_GetObjectItemCaseSensitive(doc.get(), "image_bounds");
            if (!std::isfinite(island.latitude) || std::abs(island.latitude) > 80 || !std::isfinite(island.longitude) ||
                std::abs(island.longitude) > 180 || !(island.cellSize >= 1 && island.cellSize <= 1000) ||
                !(cells >= 2 && cells <= 4096) || !cJSON_IsArray(bounds) || cJSON_GetArraySize(bounds) != 4)
                throw std::runtime_error("bad island.json fields");
            island.cells = int(cells);
            double* b[] = {&island.south, &island.west, &island.north, &island.east};
            for (int i = 0; i < 4; ++i)
            {
                const auto* v = cJSON_GetArrayItem(bounds, i);
                if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble)) throw std::runtime_error("bad image_bounds");
                *b[i] = v->valuedouble;
            }
            if (!(island.north > island.south && island.east > island.west)) throw std::runtime_error("bad image_bounds");
            const auto heights = dir / "heights.f32";
            const size_t count = size_t(island.cells) * size_t(island.cells);
            if (std::filesystem::file_size(heights) != count * sizeof(float)) throw std::runtime_error("heights.f32 size");
            island.heights.resize(count);
            std::ifstream h(heights, std::ios::binary);
            h.read(reinterpret_cast<char*>(island.heights.data()), std::streamsize(count * sizeof(float)));
            for (const float v : island.heights)
                if (!std::isfinite(v) || std::abs(v) > 12000) throw std::runtime_error("heights.f32 values");
            island.image = dir / "surface.jpg";
            LOG_INFO(Graphics, "Earth island '{}' at [{},{}]: {}x{} cells of {} m", island.name, island.latitude,
                     island.longitude, island.cells, island.cells, island.cellSize);
            result.push_back(std::move(island));
        }
        catch (const std::exception& e)
        {
            LOG_WARN(Graphics, "Earth island {} skipped: {}", dir.string(), e.what());
        }
    }
    return result;
}
} // namespace

const std::vector<Island>& All()
{
    static const std::vector<Island> islands = Load();
    return islands;
}

float Height(double latitude, double longitude, float dem)
{
    for (const auto& island : All())
    {
        const double metres = Earth::Radius * Earth::Pi / 180.0;
        const double half = island.cells * island.cellSize * 0.5;
        const double x = (longitude - island.longitude) * metres * std::cos(island.latitude * Earth::Pi / 180.0) + half;
        const double z = (latitude - island.latitude) * metres + half;
        const double gx = x / island.cellSize, gz = z / island.cellSize;
        const int last = island.cells - 1;
        if (!(gx >= 0 && gz >= 0 && gx <= last && gz <= last)) continue;
        const int ix = std::min(int(gx), last - 1), iz = std::min(int(gz), last - 1);
        const double fx = gx - ix, fz = gz - iz;
        const auto at = [&](int cx, int cz) { return double(island.heights[size_t(cz) * island.cells + cx]); };
        const double h = (at(ix, iz) * (1 - fx) + at(ix + 1, iz) * fx) * (1 - fz) +
                         (at(ix, iz + 1) * (1 - fx) + at(ix + 1, iz + 1) * fx) * fz;
        const double edge = std::min({gx, gz, last - gx, last - gz});
        const double t = std::clamp(edge / BlendCells, 0.0, 1.0);
        const double w = t * t * (3 - 2 * t);
        return float(dem + (h - dem) * w);
    }
    return dem;
}
} // namespace Poseidon::EarthIslands
