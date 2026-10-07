#include "EarthTerrainStream.hpp"
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>

#include <curl/curl.h>
#include <stb_image.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace Poseidon
{
namespace
{
double Setting(const char* name, double fallback)
{
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const double result = std::strtod(value, &end);
    if (end == value || *end || !std::isfinite(result)) throw std::invalid_argument(name);
    return result;
}
constexpr size_t MaxTileBytes = 2 * 1024 * 1024;
size_t Receive(char* data, size_t size, size_t count, void* destination)
{
    auto& bytes = *static_cast<std::vector<unsigned char>*>(destination);
    if (count > MaxTileBytes / std::max<size_t>(1, size)) return 0;
    const size_t n = size * count;
    if (bytes.size() + n > MaxTileBytes) return 0;
    try { bytes.insert(bytes.end(), data, data + n); }
    catch (...) { return 0; } // Exceptions must never cross a C callback.
    return n;
}
int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<std::atomic<bool>*>(context)->load() ? 1 : 0;
}
}

EarthTerrainStream::EarthTerrainStream()
    : _latitude(Setting("POSEIDON_EARTH_LATITUDE", 46.6)),
      _longitude(Setting("POSEIDON_EARTH_LONGITUDE", 8.1))
{
    if (std::abs(_latitude) > 84.0 || std::abs(_longitude) > 180.0)
        throw std::invalid_argument("Earth origin outside supported latitude/longitude range");
    const char* path = std::getenv("POSEIDON_EARTH_CACHE");
    if (!path || !*path) throw std::invalid_argument("POSEIDON_EARTH_CACHE must name a writable cache directory");
    _cache = std::filesystem::u8path(path) / "terrarium-z10";
    std::filesystem::create_directories(_cache);
    static std::once_flag curlInit;
    std::call_once(curlInit, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("Earth terrain curl initialization failed");
    });
    _worker = std::thread(&EarthTerrainStream::Run, this);
}

EarthTerrainStream::~EarthTerrainStream()
{
    _stop.store(true);
    _wake.notify_all();
    if (_worker.joinable()) _worker.join();
}

void EarthTerrainStream::Request(float x, float z)
{
    // Bound this planar prototype before integer conversion; globe rebasing is
    // a subsequent stage. Bad camera positions must not reach indexing/math.
    if (!std::isfinite(x) || !std::isfinite(z) || std::abs(x) > EarthStreaming::CoordinateLimit ||
        std::abs(z) > EarthStreaming::CoordinateLimit) return;
    const int cx = Earth::CenterCell(x), cz = Earth::CenterCell(z);
    std::lock_guard lock(_mutex);
    if (_requested && cx == _requestX && cz == _requestZ) return;
    _requestX = cx; _requestZ = cz; _requested = true;
    ++_generation;
    _wake.notify_one();
}

std::unique_ptr<EarthTerrainStream::Patch> EarthTerrainStream::TakeReady()
{
    std::lock_guard lock(_mutex);
    return std::move(_ready);
}

bool EarthTerrainStream::Download(const std::string& url, std::vector<unsigned char>& bytes)
{
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenPoseidon-EarthPrototype/1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &_stop);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status != 200)
        std::fprintf(stderr, "Earth terrain HTTPS: %s, HTTP %ld (%s)\n", curl_easy_strerror(result), status, url.c_str());
    return result == CURLE_OK && status == 200 && !bytes.empty();
}

const EarthTerrainStream::Tile& EarthTerrainStream::LoadTile(int x, int y)
{
    const auto key = std::make_pair(x,y);
    if (auto it = _tiles.find(key); it != _tiles.end())
    {
        it->second.use = ++_use;
        return it->second;
    }
    const auto file = _cache / (std::to_string(x) + "-" + std::to_string(y) + ".png");
    std::vector<unsigned char> bytes;
    std::error_code ec;
    const auto length = std::filesystem::file_size(file, ec);
    if (!ec && length > 0 && length <= MaxTileBytes)
    {
        std::ifstream input(file, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), {});
    }
    auto decode = [&]() -> Tile
    {
        int width = 0, height = 0, channels = 0;
        if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels) ||
            width != Earth::TilePixels || height != Earth::TilePixels)
            throw std::runtime_error("invalid elevation tile dimensions");
        auto* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels, 3);
        if (!pixels) throw std::runtime_error("invalid elevation tile PNG");
        std::unique_ptr<unsigned char, decltype(&stbi_image_free)> owner(pixels, stbi_image_free);
        Tile tile;
        tile.heights.resize(Earth::TilePixels * Earth::TilePixels);
        tile.use = ++_use;
        for (size_t i = 0; i < tile.heights.size(); ++i)
        {
            const float h = Earth::Terrarium(pixels[3*i], pixels[3*i+1], pixels[3*i+2]);
            if (h < -12000 || h > 10000) throw std::runtime_error("invalid elevation sample");
            tile.heights[i] = h;
        }
        return tile;
    };
    Tile tile;
    bool cached = !bytes.empty();
    if (cached)
    {
        try { tile = decode(); ++_cacheHits; }
        catch (...) { cached = false; bytes.clear(); }
    }
    if (!cached)
    {
        const std::string url = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/" +
            std::to_string(Earth::Zoom) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
        if (!Download(url, bytes)) throw std::runtime_error("terrain tile unavailable: " + url);
        tile = decode();
        ++_downloads;
        // Only validated bytes enter the cache. Interrupted writes never become
        // visible as a final tile. The directory belongs to this prototype.
        const auto temporary = file.string() + ".part";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        output.close();
        if (output)
        {
            std::filesystem::remove(file, ec);
            std::filesystem::rename(temporary, file, ec);
            TrimDisk();
        }
        else std::filesystem::remove(temporary, ec);
    }
    if (_tiles.size() >= 64)
    {
        const auto oldest = std::min_element(_tiles.begin(), _tiles.end(),
            [](const auto& a, const auto& b) { return a.second.use < b.second.use; });
        _tiles.erase(oldest);
    }
    return _tiles.emplace(key, std::move(tile)).first->second;
}

float EarthTerrainStream::Sample(double x, double y)
{
    // Pixel centres are half a pixel from tile borders; fetching both adjacent
    // tiles before interpolation keeps tile edges continuous, including dateline.
    x -= 0.5; y -= 0.5;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = float(x - ix), fy = float(y - iy);
    auto at = [&](int px, int py)
    {
        px = Earth::WrapPixel(px);
        py = std::clamp(py, 0, Earth::TilePixels * (1 << Earth::Zoom) - 1);
        const auto& tile = LoadTile(px / Earth::TilePixels, py / Earth::TilePixels);
        return tile.heights[(py % Earth::TilePixels) * Earth::TilePixels + px % Earth::TilePixels];
    };
    const float a = at(ix,iy), b = at(ix+1,iy), c = at(ix,iy+1), d = at(ix+1,iy+1);
    return (a + (b-a)*fx)*(1-fy) + (c + (d-c)*fx)*fy;
}

void EarthTerrainStream::TrimDisk()
{
    std::vector<std::filesystem::directory_entry> files;
    for (const auto& item : std::filesystem::directory_iterator(_cache))
        if (item.is_regular_file() && item.path().extension() == ".png") files.push_back(item);
    if (files.size() <= 256) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.last_write_time() < b.last_write_time(); });
    std::error_code ec;
    for (size_t i = 0; i < files.size() - 256; ++i) std::filesystem::remove(files[i].path(), ec);
}

void EarthTerrainStream::Run()
{
    uint64_t handled = 0;
    while (!_stop.load())
    {
        int cx, cz;
        uint64_t version;
        {
            std::unique_lock lock(_mutex);
            _wake.wait(lock, [&] { return _stop.load() || _generation.load() != handled; });
            if (_stop.load()) return;
            cx = _requestX; cz = _requestZ; version = _generation.load();
        }
        try
        {
            const auto start = std::chrono::steady_clock::now();
            auto patch = std::make_unique<Patch>();
            patch->x = cx * Earth::RecenterMetres - 128 * Earth::GridMetres;
            patch->z = cz * Earth::RecenterMetres - 128 * Earth::GridMetres;
            const auto centre = Earth::FromLocal(_latitude, _longitude,
                cx * Earth::RecenterMetres, cz * Earth::RecenterMetres);
            patch->latitude = centre.latitude; patch->longitude = centre.longitude;
            patch->generation = version;
            patch->heights.resize(Earth::GridSamples * Earth::GridSamples);
            const unsigned downloads = _downloads, hits = _cacheHits;
            for (int z = 0; z < Earth::GridSamples; ++z)
            {
                if (_stop.load()) return;
                if (_generation.load() != version) break;
                for (int x = 0; x < Earth::GridSamples; ++x)
                {
                    const auto position = Earth::ToPixel(Earth::FromLocal(_latitude, _longitude,
                        patch->x + x * Earth::GridMetres, patch->z + z * Earth::GridMetres));
                    patch->heights[z * Earth::GridSamples + x] = Sample(position.x, position.y);
                }
            }
            patch->downloads = _downloads - downloads; patch->cacheHits = _cacheHits - hits;
            patch->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            TrimDisk();
            std::lock_guard lock(_mutex);
            if (_generation.load() == version) _ready = std::move(patch);
            handled = version;
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "Earth terrain: %s; retaining last completed patch, retry in 10s\n", e.what());
            std::unique_lock lock(_mutex);
            _wake.wait_for(lock, std::chrono::seconds(10), [&] { return _stop.load() || _generation.load() != version; });
            // Retry the same request after backoff; a newer request supersedes it.
        }
    }
}
}
