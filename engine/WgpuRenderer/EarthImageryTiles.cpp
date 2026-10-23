// SPDX-License-Identifier: GPL-3.0-or-later
#include "EarthImageryTiles.hpp"
#include "EarthParallel.hpp"
#include <Poseidon/World/Terrain/EarthPlannerMap.hpp>

#include <Poseidon/World/Terrain/EarthFrame.hpp>

#include <curl/curl.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <fstream>
#include <future>
#include <iterator>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace Poseidon
{
namespace
{
constexpr size_t MaxTileBytes = 4 * 1024 * 1024;
// Decoded tiles kept in memory. One window's mosaics take ~520 (rings ~190 + near 196 + centre ~121 +
// coarse preview 16), and prefetch holds the next window's beside them: at 384 every build pushed out
// the rings it would need again and re-read and re-decoded them from disk. 1024 256-px tiles are
// ~256 MB, whole world only.
constexpr size_t MaxResidentTiles = 1024;
// Few warm connections, not many cold ones. The tile server (or something on the way) resets bursts of
// new TLS handshakes: 40 at once gave 29 resets, even 4 at once lost one. A reset tile waits out the
// retry back-off, and after four it stays missing until the next window. Measured on new ground (Rio,
// fresh cache, 2026-10-23): 16 workers final imagery 10.5 s with 4 tiles lost, 32 workers 14.3 s with
// 30 lost, 6 workers 8.6 s with none. Kept alive and reused (DownloadShare), 6 carry ~80 tiles/s.
// POSEIDON_EARTH_DOWNLOADS overrides it (1..64), for A/B.
unsigned ParallelDownloads()
{
    static const unsigned n = []
    {
        const char* v = std::getenv("POSEIDON_EARTH_DOWNLOADS");
        const int parsed = v ? std::atoi(v) : 0;
        return parsed >= 1 ? unsigned(std::min(parsed, 64)) : 6u;
    }();
    return n;
}
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

// One connection cache, TLS session cache and DNS cache for every tile download. Each tile used a
// fresh easy handle, so every tile paid a TCP connect plus a full TLS handshake -- the slow part of
// fetching a 20 KB image, and the part this machine's network fails most often. Shared, the workers
// reuse warm keep-alive connections to the tile server and resume sessions instead.
std::mutex g_shareLocks[CURL_LOCK_DATA_LAST];
void ShareLock(CURL*, curl_lock_data data, curl_lock_access, void*) { g_shareLocks[data].lock(); }
void ShareUnlock(CURL*, curl_lock_data data, void*) { g_shareLocks[data].unlock(); }
CURLSH* DownloadShareImpl()
{
    static CURLSH* share = []
    {
        CURLSH* s = curl_share_init();
        if (s)
        {
            curl_share_setopt(s, CURLSHOPT_LOCKFUNC, ShareLock);
            curl_share_setopt(s, CURLSHOPT_UNLOCKFUNC, ShareUnlock);
            curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
            curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        }
        return s;
    }();
    return share;
}

} // namespace

void* EarthDownloadShare() { return DownloadShareImpl(); }

namespace
{
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
    if (CURLSH* share = DownloadShareImpl())
        curl_easy_setopt(curl, CURLOPT_SHARE, share);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
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
    // Where a provider has no imagery (Esri over open ocean from zoom 14 on) it answers 200 OK with a flat
    // grey "Map data not yet available" image. Taken as imagery it drew a light grey square around Desert
    // Island in the Bay of Biscay. Such a tile is missing ground: coarser real tiles and the globe show
    // through instead, and it is not fetched again.
    enum class Decoded { Ok, Bad, Placeholder };
    const auto decode = [](const std::vector<uint8_t>& bytes, Tile& tile)
    {
        int w = 0, h = 0, channels = 0;
        if (bytes.empty() || !stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels) || w != h ||
            w < 64 || w > 1024)
            return Decoded::Bad;
        stbi_uc* rgba = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
        if (!rgba)
            return Decoded::Bad;
        tile.rgba.assign(rgba, rgba + size_t(w) * h * 4);
        tile.size = w;
        stbi_image_free(rgba);
        size_t flatGrey = 0;
        const size_t texels = size_t(w) * h;
        for (size_t i = 0; i < texels; ++i)
        {
            const int r = tile.rgba[i * 4], g = tile.rgba[i * 4 + 1], b = tile.rgba[i * 4 + 2];
            flatGrey += std::abs(r - g) < 6 && std::abs(g - b) < 6 && r > 185 && r < 225;
        }
        return flatGrey * 10 >= texels * 9 ? Decoded::Placeholder : Decoded::Ok;
    };
    // A rolling pool: each worker takes the next tile as soon as its last one is done. The old lock-step
    // batches of 16 waited for the slowest tile of each batch (a retried handshake: seconds) before
    // starting any of the next sixteen.
    struct Job
    {
        bool ok = false, permanent = false;
        Tile tile;
        bool placeholder = false; // the provider has no imagery here: not a failure of the network
    };
    const auto fetchOne = [&](const std::tuple<int, int, int>& key) -> Job
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
            const auto cached = decode(bytes, tile);
            if (cached == Decoded::Ok)
                return {true, false, std::move(tile)};
            if (cached == Decoded::Placeholder)
                return {false, true, {}, true};
            bytes.clear();
        }
        if (!network)
            return {false, false, {}}; // cache-only pass: not on disk yet
        bool permanent = false;
        if (!Download(EarthProviders::ExpandUrl(_source, z, x, y), bytes, cancelled, permanent))
            return {false, permanent, {}};
        const auto fetched = decode(bytes, tile);
        if (fetched == Decoded::Bad)
            return {false, true, {}}; // not an image: an error page or an unsupported format
        // Only a decodable tile is cached (a placeholder too, so the next session knows without asking); a
        // partial write never becomes a final file.
        std::filesystem::create_directories(path.parent_path(), ec);
        const auto temporary = path.string() + ".part";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
        std::filesystem::rename(temporary, path, ec);
        if (ec)
            std::filesystem::remove(temporary, ec);
        if (fetched == Decoded::Placeholder)
            return {false, true, {}, true};
        tile.use = 1; // marks a network fetch for the caller's count
        return {true, false, std::move(tile)};
    };
    std::atomic<size_t> next{0};
    // Consecutive failures (after retries) across all workers. 32 in a row means the provider or the
    // network is down: stop rather than wait out every remaining timeout (the start-up overview would
    // otherwise stall). Same budget as the old "two whole batches of 16 failed".
    std::atomic<unsigned> failedInRow{0};
    const auto worker = [&]
    {
        for (;;)
        {
            if ((cancelled && cancelled()) || failedInRow.load() >= 2 * ParallelDownloads())
                return;
            const size_t i = next.fetch_add(1);
            if (i >= missing.size())
                return;
            auto [ok, permanent, tile, placeholder] = fetchOne(missing[i]);
            std::lock_guard lock(_mutex);
            if (!ok)
            {
                if (permanent) // transient failures are retried by the next window build
                    _failed.insert(missing[i]);
                if (placeholder)
                    failedInRow = 0; // the provider answered: open ocean, not a dead network
                else if (network)
                    ++failedInRow;
                continue;
            }
            failedInRow = 0;
            downloads += tile.use;
            _arrivals += tile.use;
            tile.use = ++_use;
            _tiles[missing[i]] = std::move(tile);
        }
    };
    const size_t workers = std::min<size_t>(ParallelDownloads(), missing.size());
    std::vector<std::future<void>> pool;
    for (size_t w = 1; w < workers; ++w)
        pool.push_back(std::async(std::launch::async, worker));
    if (workers > 0)
        worker();
    for (auto& f : pool)
        f.get();
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

bool EarthImageryTiles::Cover(double& south, double west, double& north, double east, double metresPerPixel,
                              unsigned maxTiles, int& zoom, int& x0, int& x1, int& y0, int& y1,
                              std::vector<std::tuple<int, int, int>>& keys) const
{
    if (!Enabled() || !(north > south) || !(east > west) || east - west > 180.0 || !std::isfinite(metresPerPixel) ||
        metresPerPixel <= 0)
        return false;
    south = std::max(south, -MercatorLimit);
    north = std::min(north, MercatorLimit);
    if (!(north > south))
        return false;
    const double mid = 0.5 * (south + north);
    zoom = ChooseZoom(mid, metresPerPixel, _source.maxZoom);
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
    keys.clear();
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            keys.emplace_back(zoom, x, y);
    const double cx = 0.5 * (x0 + x1), cy = 0.5 * (y0 + y1);
    std::stable_sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b)
    {
        const double da = std::hypot(std::get<1>(a) - cx, std::get<2>(a) - cy);
        const double db = std::hypot(std::get<1>(b) - cx, std::get<2>(b) - cy);
        return da < db;
    });
    return true;
}

unsigned EarthImageryTiles::Prefetch(double south, double west, double north, double east, double metresPerPixel,
                                     unsigned maxTiles, const std::function<bool()>& cancelled)
{
    int zoom = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    std::vector<std::tuple<int, int, int>> keys;
    if (!Cover(south, west, north, east, metresPerPixel, maxTiles, zoom, x0, x1, y0, y1, keys))
        return 0;
    unsigned downloads = 0;
    Fetch(keys, downloads, cancelled, true);
    return downloads;
}

bool EarthImageryTiles::Build(double south, double west, double north, double east, double metresPerPixel,
                              int maxSize, unsigned maxTiles, Mosaic& out, const std::function<bool()>& cancelled,
                              bool network)
{
    out = {};
    if (maxSize < 2)
        return false;
    int zoom = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    std::vector<std::tuple<int, int, int>> keys;
    if (!Cover(south, west, north, east, metresPerPixel, maxTiles, zoom, x0, x1, y0, y1, keys))
        return false;
    const double mid = 0.5 * (south + north);
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
    // The tiles once, not a map lookup per output pixel (Find also stamps the LRU, which no two threads
    // may do at once). The rows are then composed a band per core (EarthParallel.hpp); _mutex stays held,
    // so no fetch can add or evict a tile underneath them.
    const int gridW = x1 - x0 + 1, gridH = y1 - y0 + 1;
    std::vector<const Tile*> grid(size_t(gridW) * gridH, nullptr);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            grid[size_t(y - y0) * gridW + (x - x0)] = Find(zoom, x, y);
    const auto at = [&](int x, int y) -> const Tile*
    {
        return x < x0 || x > x1 || y < y0 || y > y1 ? nullptr : grid[size_t(y - y0) * gridW + (x - x0)];
    };
    return EarthParallel::Rows(out.height, [&](int row)
    {
        const double lat = north - (row + 0.5) * (north - south) / out.height;
        double unusedX, py;
        ToMercator(lat, west, zoom, unusedX, py);
        const int ty = int(std::floor(py));
        for (int col = 0; col < out.width; ++col)
        {
            const double lon = west + (col + 0.5) * (east - west) / out.width;
            const double px = (lon + 180.0) / 360.0 * double(1 << zoom);
            const Tile* tile = at(int(std::floor(px)), ty);
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
    }, cancelled);
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
