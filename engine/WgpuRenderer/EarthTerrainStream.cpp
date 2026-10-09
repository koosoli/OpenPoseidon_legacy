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
struct Cancellation
{
    const std::atomic<bool>& stop;
    const std::atomic<uint64_t>& generation;
    uint64_t expected;
};
int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto& token=*static_cast<Cancellation*>(context);
    return token.stop.load() || token.generation.load()!=token.expected ? 1 : 0;
}
}

EarthTerrainStream::EarthTerrainStream()
{
    _requestOrigin=EarthStreaming::GetLocation();
    const char* path = std::getenv("POSEIDON_EARTH_CACHE");
    if (!path || !*path) throw std::invalid_argument("POSEIDON_EARTH_CACHE must name a writable cache directory");
    _cache = std::filesystem::u8path(path);
    std::filesystem::create_directories(_cache);
    _elevation = EarthProviders::Load(_cache).Elevation();
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

void EarthTerrainStream::Relocate(const EarthStreaming::Location& origin)
{
    if (!EarthStreaming::ValidateLocation(origin.latitude,origin.longitude,origin.altitude)) return;
    std::lock_guard lock(_mutex);
    _requestOrigin=origin;
    _requested=false;
    _ready.reset();
    ++_generation; // Cancel even at orbital altitude, where no new DEM is needed.
    _wake.notify_one();
}

void EarthTerrainStream::Request(float x, float z, float altitude, const EarthStreaming::Location& origin)
{
    // Bound local travel before integer conversion. Geographic relocation is
    // explicit; continuous globe circumnavigation is not supported here.
    if (!std::isfinite(x) || !std::isfinite(z) || std::abs(x) > EarthStreaming::CoordinateLimit ||
        std::abs(z) > EarthStreaming::CoordinateLimit ||
        !EarthStreaming::ValidateLocation(origin.latitude,origin.longitude,origin.altitude)) return;
    const float radius = Earth::HorizonRadius(altitude);
    std::lock_guard lock(_mutex);
    const auto detail = Earth::SelectNearDetail(altitude, _requestDetail);
    const float stride = Earth::NearRecenter(Earth::NearGrid(detail));
    const int cx = Earth::CenterCell(x, stride), cz = Earth::CenterCell(z, stride);
    if (_requested && cx == _requestX && cz == _requestZ && radius == _requestRadius && detail == _requestDetail &&
        origin.revision==_requestOrigin.revision) return;
    _requestX = cx; _requestZ = cz; _requestRadius = radius; _requested = true;
    _requestDetail = detail;
    _requestOrigin = origin;
    _ready.reset(); // A completed old band/cell must not be published after this request.
    ++_generation;
    _wake.notify_one();
}

std::unique_ptr<EarthTerrainStream::Patch> EarthTerrainStream::TakeReady()
{
    std::lock_guard lock(_mutex);
    if (_ready && (_ready->generation != _generation.load() ||
        !EarthStreaming::Locations().IsCurrent(_ready->originRevision))) _ready.reset();
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
    Cancellation token{_stop,_generation,_activeGeneration};
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &token);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if ((result != CURLE_OK || status != 200) && result!=CURLE_ABORTED_BY_CALLBACK)
        std::fprintf(stderr, "Earth terrain HTTPS: %s, HTTP %ld (%s)\n", curl_easy_strerror(result), status,
                     EarthProviders::Redact(url, _elevation.key).c_str());
    return result == CURLE_OK && status == 200 && !bytes.empty();
}

std::filesystem::path EarthTerrainStream::TileDirectory(int zoom) const
{
    // The free default keeps its historical cache directory; other providers get their own.
    if (_elevation.id == "terrarium")
        return _cache / ("terrarium-z" + std::to_string(zoom));
    return _cache / ("elev-" + EarthProviders::CacheName(_elevation) + "-z" + std::to_string(zoom));
}

const EarthTerrainStream::Tile& EarthTerrainStream::LoadTile(int zoom, int x, int y)
{
    const auto key = std::make_tuple(zoom,x,y);
    if (auto it = _tiles.find(key); it != _tiles.end())
    {
        it->second.use = ++_use;
        return it->second;
    }
    const auto directory = TileDirectory(zoom);
    std::filesystem::create_directories(directory);
    const auto file = directory / (std::to_string(x) + "-" + std::to_string(y) + ".png");
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
            const float h = _elevation.terrainRgb
                ? -10000.0f + 0.1f * float(pixels[3*i] * 65536 + pixels[3*i+1] * 256 + pixels[3*i+2])
                : Earth::Terrarium(pixels[3*i], pixels[3*i+1], pixels[3*i+2]);
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
        const std::string url = EarthProviders::ExpandUrl(_elevation, zoom, x, y);
        if (!Download(url, bytes))
            throw std::runtime_error("terrain tile unavailable: " + EarthProviders::Redact(url, _elevation.key));
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

float EarthTerrainStream::Sample(double x, double y, int zoom)
{
    // Pixel centres are half a pixel from tile borders; fetching both adjacent
    // tiles before interpolation keeps tile edges continuous, including dateline.
    x -= 0.5; y -= 0.5;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = float(x - ix), fy = float(y - iy);
    auto at = [&](int px, int py)
    {
        px = Earth::WrapPixel(px, zoom);
        py = std::clamp(py, 0, Earth::TilePixels * (1 << zoom) - 1);
        const auto& tile = LoadTile(zoom, px / Earth::TilePixels, py / Earth::TilePixels);
        return tile.heights[(py % Earth::TilePixels) * Earth::TilePixels + px % Earth::TilePixels];
    };
    const float a = at(ix,iy), b = at(ix+1,iy), c = at(ix,iy+1), d = at(ix+1,iy+1);
    return (a + (b-a)*fx)*(1-fy) + (c + (d-c)*fx)*fy;
}

void EarthTerrainStream::TrimDisk()
{
    std::vector<std::filesystem::directory_entry> files;
    for (int zoom = 3; zoom <= Earth::MaxZoom; ++zoom)
    {
        const auto directory = TileDirectory(zoom);
        if (!std::filesystem::exists(directory)) continue;
        for (const auto& item : std::filesystem::directory_iterator(directory))
            if (item.is_regular_file() && item.path().extension() == ".png") files.push_back(item);
    }
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
        float radius;
        Earth::NearDetail detail;
        EarthStreaming::Location origin;
        uint64_t version;
        {
            std::unique_lock lock(_mutex);
            _wake.wait(lock, [&] { return _stop.load() || (_requested && _generation.load() != handled); });
            if (_stop.load()) return;
            cx = _requestX; cz = _requestZ; version = _generation.load();
            radius = _requestRadius;
            detail = _requestDetail;
            origin = _requestOrigin;
            _activeGeneration = version;
        }
        try
        {
            const auto start = std::chrono::steady_clock::now();
            auto patch = std::make_unique<Patch>();
            patch->gridMetres = Earth::NearGrid(detail);
            patch->sourceZoom = Earth::NearZoom(detail);
            const float halfWindow = Earth::NearHalfWindow(patch->gridMetres);
            const float stride = Earth::NearRecenter(patch->gridMetres);
            patch->x = cx * stride - halfWindow;
            patch->z = cz * stride - halfWindow;
            const auto centre = Earth::FromLocal(origin.latitude, origin.longitude,
                cx * stride, cz * stride);
            patch->latitude = centre.latitude; patch->longitude = centre.longitude;
            patch->originLatitude = origin.latitude; patch->originLongitude = origin.longitude;
            patch->originRevision = origin.revision;
            patch->generation = version;
            patch->heights.resize(Earth::GridSamples * Earth::GridSamples);
            const unsigned downloads = _downloads, hits = _cacheHits;
            for (int z = 0; z < Earth::GridSamples; ++z)
            {
                if (_stop.load()) return;
                if (_generation.load() != version) break;
                for (int x = 0; x < Earth::GridSamples; ++x)
                {
                    const auto position = Earth::ToPixel(Earth::FromLocal(origin.latitude, origin.longitude,
                        patch->x + x * patch->gridMetres, patch->z + z * patch->gridMetres),patch->sourceZoom);
                    const double wx = patch->x + x * patch->gridMetres;
                    const double wz = patch->z + z * patch->gridMetres;
                    patch->heights[z * Earth::GridSamples + x] =
                        Sample(position.x, position.y,patch->sourceZoom) - float(Earth::CurvatureDrop(wx,wz));
                }
            }
            if (_generation.load() != version) { handled = version; continue; }
            patch->horizonRadius = radius;
            patch->horizonZoom = Earth::HorizonZoom(centre.latitude, radius);
            patch->horizonHeights.resize(Earth::HorizonSamples * Earth::HorizonSamples);
            patch->horizonValid.resize(patch->horizonHeights.size(), 1);
            for (int z = 0; z < Earth::HorizonSamples; ++z)
            {
                if (_stop.load()) return;
                if (_generation.load() != version) break;
                for (int x = 0; x < Earth::HorizonSamples; ++x)
                {
                    const float ox = Earth::HorizonOffset(x,radius,halfWindow), oz = Earth::HorizonOffset(z,radius,halfWindow);
                    const double wx = cx * stride + ox;
                    const double wz = cz * stride + oz;
                    const auto geographic = Earth::FromLocal(origin.latitude,origin.longitude,wx,wz);
                    const size_t index = z * Earth::HorizonSamples + x;
                    if (std::abs(geographic.latitude) > Earth::MaxLatitude)
                    {
                        patch->horizonValid[index] = 0; // Never invent polar source data.
                        continue;
                    }
                    const auto coarse = Earth::ToPixel(geographic,patch->horizonZoom);
                    float height = Sample(coarse.x,coarse.y,patch->horizonZoom);
                    // The inner near square is not drawn by this mesh. Its edge
                    // shares the selected near source; blend resolutions outside
                    // it instead of making an elevation cliff at the join.
                    const float edge = std::max(std::abs(ox),std::abs(oz)) - halfWindow;
                    const float blendWidth = Earth::NearBlendWidth(patch->gridMetres);
                    if (edge < blendWidth)
                    {
                        const auto fine = Earth::ToPixel(geographic,patch->sourceZoom);
                        const float weight = std::clamp(edge / blendWidth,0.0f,1.0f);
                        height = Sample(fine.x,fine.y,patch->sourceZoom) * (1-weight) + height * weight;
                    }
                    patch->horizonHeights[index] = height - float(Earth::CurvatureDrop(wx,wz));
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
            if (_generation.load()==version && !_stop.load())
                std::fprintf(stderr, "Earth terrain: %s; retaining last completed patch, retry in 10s\n", e.what());
            std::unique_lock lock(_mutex);
            _wake.wait_for(lock, std::chrono::seconds(10), [&] { return _stop.load() || _generation.load() != version; });
            // Retry the same request after backoff; a newer request supersedes it.
        }
    }
}
}
