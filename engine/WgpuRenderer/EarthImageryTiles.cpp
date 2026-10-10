// SPDX-License-Identifier: GPL-3.0-or-later
#include "EarthImageryTiles.hpp"
#include <Poseidon/World/Terrain/EarthPlannerMap.hpp>

#include <Poseidon/World/Terrain/EarthFrame.hpp>

#include <curl/curl.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <iterator>
#include <chrono>
#include <thread>

namespace Poseidon
{
namespace
{
constexpr size_t MaxTileBytes = 4 * 1024 * 1024;
constexpr size_t MaxResidentTiles = 384;
constexpr unsigned ParallelDownloads = 16;
constexpr double MercatorLimit = 85.0511287798066;

size_t Receive(char* data, size_t size, size_t count, void* destination)
{
    auto& bytes = *static_cast<std::vector<uint8_t>*>(destination);
    const size_t n = size * count;
    if (bytes.size() + n > MaxTileBytes)
        return 0;
    try
    {
        bytes.insert(bytes.end(), data, data + n);
    }
    catch (...)
    {
        return 0; // exceptions must never cross a C callback
    }
    return n;
}

int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto* cancelled = static_cast<const std::function<bool()>*>(context);
    return (*cancelled && (*cancelled)()) ? 1 : 0;
}

// Global web-mercator pixel position of a coordinate at `zoom` for `tile`-pixel tiles.
void ToMercator(double latitude, double longitude, int zoom, double& px, double& py)
{
    const double n = double(1 << zoom);
    const double lat = std::clamp(latitude, -MercatorLimit, MercatorLimit) * Earth::Pi / 180.0;
    px = (longitude + 180.0) / 360.0 * n;
    py = (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / Earth::Pi) * 0.5 * n;
}
} // namespace

EarthImageryTiles::EarthImageryTiles(std::filesystem::path cache, EarthProviders::Source source)
    : _source(std::move(source))
{
    _dir = cache / "imagery" / EarthProviders::CacheName(_source);
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

int EarthImageryTiles::ChooseZoom(double latitude, double metresPerPixel, int maxZoom)
{
    const double atEquator = 2.0 * Earth::Pi * Earth::Radius / 256.0; // metres per pixel at zoom 0
    const double c = std::max(0.05, std::cos(std::clamp(latitude, -MercatorLimit, MercatorLimit) * Earth::Pi / 180));
    int z = 0;
    while (z < maxZoom && atEquator * c / double(1 << z) > metresPerPixel)
        ++z;
    return z;
}

std::filesystem::path EarthImageryTiles::TilePath(int z, int x, int y) const
{
    return _dir / std::to_string(z) / (std::to_string(x) + "-" + std::to_string(y) + ".tile");
}

bool EarthImageryTiles::Download(const std::string& url, std::vector<uint8_t>& bytes,
                                 const std::function<bool()>& cancelled, bool& permanent)
{
    // Handshakes and connections fail transiently on real networks (measured on this machine: 4 of 5
    // TLS handshakes to one tile server failing in a row). Retry those; only a server refusal is final.
    permanent = false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        for (int waited = 0; waited < 250 * attempt; waited += 50)
        {
            if (cancelled && cancelled())
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        bytes.clear();
        long status = 0;
        const int result = Perform(url, bytes, cancelled, status);
        if (result == CURLE_OK && status == 200 && !bytes.empty())
            return true;
        if (result == CURLE_ABORTED_BY_CALLBACK)
            return false;
        permanent = result == CURLE_OK && status >= 400 && status < 500 && status != 408 && status != 429;
        if (permanent || attempt == 3)
        {
            std::fprintf(stderr, "Earth imagery HTTPS: %s, HTTP %ld (%s)\n", curl_easy_strerror(CURLcode(result)),
                         status, EarthProviders::Redact(url, _source.key).c_str());
            return false;
        }
    }
    return false;
}

int EarthImageryTiles::Perform(const std::string& url, std::vector<uint8_t>& bytes,
                               const std::function<bool()>& cancelled, long& status)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return CURLE_FAILED_INIT;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenPoseidon-Earth/1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return int(result);
}

void EarthImageryTiles::Fetch(const std::vector<std::tuple<int, int, int>>& keys, unsigned& downloads,
                              const std::function<bool()>& cancelled, bool network)
{
    // Disk first, then the network in small parallel batches. Decoding happens off the lock.
    std::vector<std::tuple<int, int, int>> missing;
    for (const auto& key : keys)
    {
        {
            std::lock_guard lock(_mutex);
            if (_tiles.count(key) || _failed.count(key))
                continue;
        }
        missing.push_back(key);
    }
    const auto decode = [](const std::vector<uint8_t>& bytes, Tile& tile)
    {
        int w = 0, h = 0, channels = 0;
        if (bytes.empty() || !stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels) || w != h ||
            w < 64 || w > 1024)
            return false;
        stbi_uc* rgba = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
        if (!rgba)
            return false;
        tile.rgba.assign(rgba, rgba + size_t(w) * h * 4);
        tile.size = w;
        stbi_image_free(rgba);
        return true;
    };
    int deadBatches = 0;
    for (size_t begin = 0; begin < missing.size(); begin += ParallelDownloads)
    {
        if (cancelled && cancelled())
            return;
        const size_t end = std::min(missing.size(), begin + ParallelDownloads);
        struct Job
        {
            bool ok = false, permanent = false;
            Tile tile;
        };
        std::vector<std::future<Job>> jobs;
        for (size_t i = begin; i < end; ++i)
        {
            jobs.push_back(std::async(std::launch::async, [&, key = missing[i]]() -> Job
            {
                const auto [z, x, y] = key;
                const auto path = TilePath(z, x, y);
                std::vector<uint8_t> bytes;
                std::error_code ec;
                const auto length = std::filesystem::file_size(path, ec);
                Tile tile;
                if (!ec && length > 0 && length <= MaxTileBytes)
                {
                    std::ifstream input(path, std::ios::binary);
                    bytes.assign(std::istreambuf_iterator<char>(input), {});
                    if (decode(bytes, tile))
                        return {true, false, std::move(tile)};
                    bytes.clear();
                }
                if (!network)
                    return {false, false, {}}; // cache-only pass: not on disk yet
                bool permanent = false;
                if (!Download(EarthProviders::ExpandUrl(_source, z, x, y), bytes, cancelled, permanent))
                    return {false, permanent, {}};
                if (!decode(bytes, tile))
                    return {false, true, {}}; // not an image: an error page or an unsupported format
                // Only a decodable tile is cached; a partial write never becomes a final file.
                std::filesystem::create_directories(path.parent_path(), ec);
                const auto temporary = path.string() + ".part";
                {
                    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                }
                std::filesystem::rename(temporary, path, ec);
                if (ec)
                    std::filesystem::remove(temporary, ec);
                tile.use = 1; // marks a network fetch for the caller's count
                return {true, false, std::move(tile)};
            }));
        }
        bool any = false;
        for (size_t i = begin; i < end; ++i)
        {
            auto [ok, permanent, tile] = jobs[i - begin].get();
            std::lock_guard lock(_mutex);
            if (!ok)
            {
                if (permanent) // transient failures are retried by the next window build
                    _failed.insert(missing[i]);
                continue;
            }
            any = true;
            downloads += tile.use;
            tile.use = ++_use;
            _tiles[missing[i]] = std::move(tile);
        }
        // Two whole batches failing (after retries) means the provider or the network is down: stop rather
        // than wait out every remaining timeout (the start-up overview would otherwise stall).
        deadBatches = any ? 0 : deadBatches + 1;
        if (deadBatches >= 2)
            break;
    }
    std::lock_guard lock(_mutex);
    while (_tiles.size() > MaxResidentTiles)
        _tiles.erase(std::min_element(_tiles.begin(), _tiles.end(),
                                      [](const auto& a, const auto& b) { return a.second.use < b.second.use; }));
}

const EarthImageryTiles::Tile* EarthImageryTiles::Find(int z, int x, int y)
{
    auto it = _tiles.find({z, x, y});
    if (it == _tiles.end())
        return nullptr;
    it->second.use = ++_use;
    return &it->second;
}

bool EarthImageryTiles::Build(double south, double west, double north, double east, double metresPerPixel,
                              int maxSize, unsigned maxTiles, Mosaic& out, const std::function<bool()>& cancelled,
                              bool network)
{
    out = {};
    if (!Enabled() || !(north > south) || !(east > west) || east - west > 180.0 || !std::isfinite(metresPerPixel) ||
        metresPerPixel <= 0 || maxSize < 2)
        return false;
    south = std::max(south, -MercatorLimit);
    north = std::min(north, MercatorLimit);
    if (!(north > south))
        return false;
    const double mid = 0.5 * (south + north);
    int zoom = ChooseZoom(mid, metresPerPixel, _source.maxZoom);
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (;; --zoom)
    {
        double ax, ay, bx, by;
        ToMercator(north, west, zoom, ax, ay);
        ToMercator(south, east, zoom, bx, by);
        const int n = 1 << zoom;
        x0 = std::clamp(int(std::floor(ax)), 0, n - 1), x1 = std::clamp(int(std::floor(bx)), 0, n - 1);
        y0 = std::clamp(int(std::floor(ay)), 0, n - 1), y1 = std::clamp(int(std::floor(by)), 0, n - 1);
        if (unsigned(x1 - x0 + 1) * unsigned(y1 - y0 + 1) <= maxTiles || zoom == 0)
            break;
    }
    std::vector<std::tuple<int, int, int>> keys;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            keys.emplace_back(zoom, x, y);
    Fetch(keys, out.downloads, cancelled, network);
    if (cancelled && cancelled())
        return false;

    const double metres = Earth::Radius * Earth::Pi / 180.0;
    const double wideM = (east - west) * metres * std::cos(mid * Earth::Pi / 180.0), tallM = (north - south) * metres;
    out.width = std::clamp(int(std::ceil(wideM / metresPerPixel)), 2, maxSize);
    out.height = std::clamp(int(std::ceil(tallM / metresPerPixel)), 2, maxSize);
    out.south = south, out.west = west, out.north = north, out.east = east, out.zoom = zoom;
    out.tiles = unsigned(keys.size());
    out.pixels.assign(size_t(out.width) * out.height * 4, 0);

    std::lock_guard lock(_mutex);
    for (const auto& key : keys)
        out.failures += _tiles.count(key) ? 0u : 1u;
    if (out.failures == keys.size())
        return false;
    for (int row = 0; row < out.height; ++row)
    {
        if (cancelled && cancelled())
            return false;
        const double lat = north - (row + 0.5) * (north - south) / out.height;
        double unusedX, py;
        ToMercator(lat, west, zoom, unusedX, py);
        const int ty = int(std::floor(py));
        for (int col = 0; col < out.width; ++col)
        {
            const double lon = west + (col + 0.5) * (east - west) / out.width;
            const double px = (lon + 180.0) / 360.0 * double(1 << zoom);
            const Tile* tile = Find(zoom, int(std::floor(px)), ty);
            if (!tile)
                continue;
            // Bilinear inside the tile (edges clamp; adjacent tiles meet within half a texel).
            const double fx = (px - std::floor(px)) * tile->size - 0.5, fy = (py - ty) * tile->size - 0.5;
            const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
            const double ax = fx - ix, ay = fy - iy;
            uint8_t* dst = &out.pixels[(size_t(row) * out.width + col) * 4];
            for (int c = 0; c < 3; ++c)
            {
                double v = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sx = std::clamp(ix + dx, 0, tile->size - 1), sy = std::clamp(iy + dy, 0, tile->size - 1);
                        v += tile->rgba[(size_t(sy) * tile->size + sx) * 4 + c] * (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
                    }
                dst[c] = uint8_t(std::clamp(std::lround(v), 0l, 255l));
            }
            dst[3] = 255;
        }
    }
    return true;
}

std::filesystem::path EarthImageryTiles::Overview()
{
    if (!Enabled())
        return {};
    const auto path = _dir / "overview-v2-4096.jpg"; // v2: v1 could be cached with missing tiles
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 0 && !ec)
        return path;
    // Zoom 3 (64 tiles) covers the globe at ~10 km/px; the equirectangular rows past the mercator
    // limit repeat its last row (the poles are rarely seen). Two half-worlds keep each build within
    // Build's 180-degree bound.
    constexpr int W = 4096, H = 2048;
    std::vector<uint8_t> world(size_t(W) * H * 3);
    Mosaic halves[2];
    if (!Build(-MercatorLimit, -180.0, MercatorLimit, 0.0, 9800.0, 2048, 64, halves[0]) ||
        !Build(-MercatorLimit, 0.0, MercatorLimit, 180.0, 9800.0, 2048, 64, halves[1]) ||
        halves[0].failures || halves[1].failures)
        return {};
    for (int row = 0; row < H; ++row)
    {
        const double lat = 90.0 - (row + 0.5) * 180.0 / H;
        for (int col = 0; col < W; ++col)
        {
            const Mosaic& h = halves[col < W / 2 ? 0 : 1];
            const double lon = -180.0 + (col + 0.5) * 360.0 / W;
            const int sx = std::clamp(int((lon - h.west) / (h.east - h.west) * h.width), 0, h.width - 1);
            const int sy = std::clamp(int((h.north - std::clamp(lat, h.south, h.north)) / (h.north - h.south) * h.height),
                                      0, h.height - 1);
            for (int c = 0; c < 3; ++c)
                world[(size_t(row) * W + col) * 3 + c] = h.pixels[(size_t(sy) * h.width + sx) * 4 + c];
        }
    }
    std::filesystem::create_directories(_dir, ec);
    const auto temporary = path.string() + ".part";
    if (!stbi_write_jpg(temporary.c_str(), W, H, 3, world.data(), 90))
        return {};
    std::filesystem::rename(temporary, path, ec);
    return ec ? std::filesystem::path{} : path;
}
} // namespace Poseidon

namespace Poseidon
{
namespace
{
// The Whole World mission planner's map (EarthPlannerMap): the configured provider's imagery for the
// planned area, resampled to a square PNG the menu loads as a texture.
bool BuildPlannerMap(const std::filesystem::path& cache, double south, double west, double north, double east,
                     int size, const std::filesystem::path& out, std::string& message,
                     const std::function<bool()>& cancelled)
{
    EarthImageryTiles tiles(cache, EarthProviders::Load(cache).Imagery());
    if (!tiles.Enabled())
    {
        message = "The selected imagery provider is not configured.";
        return false;
    }
    const double metres = (north - south) * Earth::Pi / 180.0 * Earth::Radius;
    EarthImageryTiles::Mosaic mosaic;
    if (!tiles.Build(south, west, north, east, metres / size, size, 96, mosaic, cancelled) || mosaic.width < 2 ||
        mosaic.height < 2)
    {
        message = "No imagery for this area (offline, or the provider refused).";
        return false;
    }
    // RGBA: a tile that did not arrive stays transparent, so the coarser image under it shows through.
    std::vector<uint8_t> rgb(size_t(size) * size * 4);
    unsigned missing = 0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const int sx = std::min(mosaic.width - 1, x * mosaic.width / size);
            const int sy = std::min(mosaic.height - 1, y * mosaic.height / size);
            const uint8_t* p = &mosaic.pixels[(size_t(sy) * mosaic.width + sx) * 4];
            uint8_t* q = &rgb[(size_t(y) * size + x) * 4];
            const bool present = p[3] != 0;
            missing += present ? 0 : 1;
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = present ? 255 : 0;
        }
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (!stbi_write_png(out.string().c_str(), size, size, 4, rgb.data(), size * 4))
    {
        message = "Cannot write the planner map to " + out.string();
        return false;
    }
    message = "Imagery zoom " + std::to_string(mosaic.zoom) + ", " + std::to_string(mosaic.tiles) + " tiles (" +
              std::to_string(mosaic.downloads) + " downloaded)" +
              (missing ? ", some tiles missing" : std::string());
    return true;
}
const bool PlannerMapRegistered = (EarthPlannerMap::Hook() = &BuildPlannerMap, true);
} // namespace
} // namespace Poseidon
