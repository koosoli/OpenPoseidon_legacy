#include "EarthTerrainStream.hpp"
#include "EarthLandCover.hpp"
#include "EarthImageryTiles.hpp"
#include "EarthIslands.hpp"
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>

#include <curl/curl.h>
#include <stb_image.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <future>
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
    ++_epoch;
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
    // A new window of the SAME origin does not cancel the patch under way, nor drop a finished one: in fast
    // flight a new window came every ~0.5 s and each one aborted the last, so no patch ever completed and
    // the ground (and its imagery, requested per patch) never arrived. The finished patch is still ground
    // of this origin; the newest window is built next. A new origin (epoch) does cancel.
    if (origin.revision != _requestOrigin.revision)
    {
        _ready.reset(); // a completed patch of the old origin must not be published after this request
        ++_epoch;
    }
    _requestOrigin = origin;
    ++_generation;
    _wake.notify_one();
}

std::unique_ptr<EarthTerrainStream::Patch> EarthTerrainStream::TakeReady()
{
    std::lock_guard lock(_mutex);
    if (_ready && !EarthStreaming::Locations().IsCurrent(_ready->originRevision)) _ready.reset();
    return std::move(_ready);
}

bool EarthTerrainStream::Download(const std::string& url, std::vector<unsigned char>& bytes)
{
    // Transient connection/TLS failures are common on real networks (Earth imagery measured 4 of 5
    // handshakes failing in a row on one machine); one failed tile used to stall the whole patch 10 s.
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        if (attempt)
        {
            for (int waited = 0; waited < 250 * attempt; waited += 50)
            {
                if (_stop.load() || _epoch.load() != _activeEpoch) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        bytes.clear();
        long status = 0;
        const bool aborted = !DownloadOnce(url, bytes, status);
        if (status == 200 && !bytes.empty()) return true;
        if (aborted || (status >= 400 && status < 500 && status != 408 && status != 429)) return false;
    }
    return false;
}

bool EarthTerrainStream::DownloadOnce(const std::string& url, std::vector<unsigned char>& bytes, long& status)
{
    CURL* curl = curl_easy_init();
    if (!curl) return true;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenPoseidon-EarthPrototype/1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(EarthDownloadShare()));
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    Cancellation token{_stop,_epoch,_activeEpoch};
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &token);
    const CURLcode result = curl_easy_perform(curl);
    status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK)
        status = 0;
    if ((result != CURLE_OK || status != 200) && result!=CURLE_ABORTED_BY_CALLBACK)
        std::fprintf(stderr, "Earth terrain HTTPS: %s, HTTP %ld (%s)\n", curl_easy_strerror(result), status,
                     EarthProviders::Redact(url, _elevation.key).c_str());
    return result != CURLE_ABORTED_BY_CALLBACK; // false: cancelled, do not retry
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
    if (auto failed = _failedTiles.find(key);
        failed != _failedTiles.end() && std::chrono::steady_clock::now() - failed->second < std::chrono::seconds(60))
        throw std::runtime_error("terrain tile recently unavailable");
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
        {
            if (!_stop.load() && _epoch.load() == _activeEpoch)
            {
                if (_failedTiles.size() >= 1024) _failedTiles.clear();
                _failedTiles[key] = std::chrono::steady_clock::now();
            }
            throw std::runtime_error("terrain tile unavailable: " + EarthProviders::Redact(url, _elevation.key));
        }
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
    if (_tiles.size() >= 384) // the distance-matched horizon touches several zooms at once
    {
        const auto oldest = std::min_element(_tiles.begin(), _tiles.end(),
            [](const auto& a, const auto& b) { return a.second.use < b.second.use; });
        _tiles.erase(oldest);
    }
    return _tiles.emplace(key, std::move(tile)).first->second;
}

float EarthTerrainStream::SampleFallback(const Earth::Coordinate& c, int zoom, int coarsest)
{
    for (int z = zoom;; --z)
    {
        try
        {
            const auto p = Earth::ToPixel(c, z);
            return Sample(p.x, p.y, z);
        }
        catch (const std::exception&)
        {
            if (z <= coarsest || _stop.load() || _epoch.load() != _activeEpoch) throw;
        }
    }
}

// Terrarium's bathymetry (ETOPO1 / GEBCO-derived, joerd) is in its zooms 0..10 only: from zoom 11 on the open sea
// is filled with exactly 0 m, the surface itself (measured in the cache 2026-10-24: z10 tiles go down to -4653 m,
// the Bay of Biscay's z12/z14 tiles are 0 m throughout). There the depth comes from zoom 10, the finest that has
// it -- ~150 m a sample, plenty for a sea floor; a sea sample zoom 10 blurs onto the shore is kept just under the
// surface rather than raised onto it.
//
// But exact 0 m is not only sea: the coarse sources are whole metres, and low coast -- marsh and polder behind the
// dikes, the Wadden coast -- reads 0 m (or a little either side) too. Sunk to the sea's depth it went under water
// (owner, Cuxhaven, 2026-10-24: "lots is under water what should not be"). Near the camera ESA WorldCover decides:
// water is sea (the depth), land stays a metre above the sea surface, as the dikes we do not model keep it. The land
// cover is read from disk only (EarthLandCover::Peek); a block not there yet is fetched in the background and the
// next patch has it.
namespace
{
constexpr int BathymetryZoom = 10;
constexpr float ShoreBand = 1.0f, PolderDepth = -12.0f;
}

float EarthTerrainStream::WithSeaDepth(const Earth::Coordinate& c, int zoom, float height, bool shore)
{
    const bool fill = zoom > BathymetryZoom && height == 0.0f;
    if (!fill && (!shore || height > ShoreBand || height < PolderDepth))
        return height;
    if (shore)
        if (EarthLandCover* cover = EarthLandCover::Get())
        {
            const auto landClass = cover->Peek(c.latitude, c.longitude);
            if (landClass != EarthLandCover::Unknown && landClass != EarthLandCover::Water)
                return std::max(height, ShoreBand);
            if (!fill)
                return height;
        }
    if (!fill)
        return height;
    try
    {
        return std::min(SampleFallback(c, BathymetryZoom, 3), -0.5f);
    }
    catch (const std::exception&)
    {
        if (_stop.load() || _epoch.load() != _activeEpoch)
            throw;
        return -200.0f; // no depth anywhere: deep enough for every keel, as the old stand-in floor
    }
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

void EarthTerrainStream::Prefetch(const std::set<std::tuple<int,int,int>>& keys)
{
    std::vector<std::tuple<int,int,int>> missing;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& key : keys)
    {
        if (_tiles.count(key)) continue;
        if (auto failed = _failedTiles.find(key); failed != _failedTiles.end() && now - failed->second < std::chrono::seconds(60))
            continue;
        const auto [zoom, x, y] = key;
        std::error_code ec;
        const auto size = std::filesystem::file_size(TileDirectory(zoom) / (std::to_string(x) + "-" + std::to_string(y) + ".png"), ec);
        if (!ec && size > 0)
            continue;
        missing.push_back(key);
    }
    if (missing.empty()) return;
    std::atomic<size_t> next{0};
    std::mutex failedMutex;
    std::vector<std::tuple<int,int,int>> failed;
    const auto worker = [&]
    {
        for (size_t i; (i = next.fetch_add(1)) < missing.size();)
        {
            if (_stop.load() || _epoch.load() != _activeEpoch) return;
            const auto [zoom, x, y] = missing[i];
            std::vector<unsigned char> bytes;
            if (!Download(EarthProviders::ExpandUrl(_elevation, zoom, x, y), bytes))
            {
                std::lock_guard lock(failedMutex);
                failed.push_back(missing[i]);
                continue;
            }
            // Decoded and validated later by LoadTile, which also drops a bad file and refetches it.
            const auto directory = TileDirectory(zoom);
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            const auto file = directory / (std::to_string(x) + "-" + std::to_string(y) + ".png");
            const auto temporary = file.string() + ".part" + std::to_string(i);
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            output.close();
            if (output) std::filesystem::rename(temporary, file, ec);
            if (!output || ec) std::filesystem::remove(temporary, ec);
            ++_downloads;
        }
    };
    constexpr size_t Workers = 8;
    std::vector<std::future<void>> pool;
    for (size_t w = 1; w < std::min(Workers, missing.size()); ++w) pool.push_back(std::async(std::launch::async, worker));
    worker();
    for (auto& f : pool) f.get();
    if (!_stop.load() && _epoch.load() == _activeEpoch)
        for (const auto& key : failed) _failedTiles[key] = now;
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
    // Room for an offline region pack (scripts/Prepare-EarthRegion.py: z10-z14 around a place) plus normal
    // streaming; terrarium PNGs are ~100 KB, so this caps the elevation cache near 400 MB.
    constexpr size_t MaxFiles = 4096;
    if (files.size() <= MaxFiles) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.last_write_time() < b.last_write_time(); });
    std::error_code ec;
    for (size_t i = 0; i < files.size() - MaxFiles; ++i) std::filesystem::remove(files[i].path(), ec);
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
            _activeEpoch = _epoch.load();
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
            {
                // Every tile this patch will sample, fetched eight at a time before the (serial) sampling
                // below: one at a time, each paying its own handshake, a fresh place took ~10 s of
                // elevation before any satellite imagery could even be requested for it.
                std::set<std::tuple<int,int,int>> keys;
                const auto addTiles = [&](const Earth::Coordinate& c, int zoom)
                {
                    if (zoom > BathymetryZoom) // the sea's depth (WithSeaDepth)
                    {
                        const auto b = Earth::ToPixel(c, BathymetryZoom);
                        keys.insert({BathymetryZoom, Earth::WrapPixel(int(std::floor(b.x)), BathymetryZoom) / Earth::TilePixels,
                                     std::clamp(int(std::floor(b.y)), 0, Earth::TilePixels * (1 << BathymetryZoom) - 1) / Earth::TilePixels});
                    }
                    const auto p = Earth::ToPixel(c, zoom);
                    for (int dy = -1; dy <= 1; dy += 2)
                        for (int dx = -1; dx <= 1; dx += 2)
                        {
                            const int px = Earth::WrapPixel(int(std::floor(p.x + 0.5 * dx)), zoom);
                            const int py = std::clamp(int(std::floor(p.y + 0.5 * dy)), 0, Earth::TilePixels * (1 << zoom) - 1);
                            keys.insert({zoom, px / Earth::TilePixels, py / Earth::TilePixels});
                        }
                };
                for (int z = 0; z < Earth::GridSamples; z += 16)
                    for (int x = 0; x < Earth::GridSamples; x += 16)
                        addTiles(Earth::FromLocal(origin.latitude, origin.longitude, patch->x + x * patch->gridMetres,
                                                  patch->z + z * patch->gridMetres), patch->sourceZoom);
                const int horizonZoom = Earth::HorizonZoom(centre.latitude, radius);
                for (int z = 0; z < Earth::HorizonSamples; z += 2)
                    for (int x = 0; x < Earth::HorizonSamples; x += 2)
                    {
                        const double wx = cx * stride + Earth::HorizonOffset(x,radius,halfWindow);
                        const double wz = cz * stride + Earth::HorizonOffset(z,radius,halfWindow);
                        const auto g = Earth::FromLocal(origin.latitude,origin.longitude,wx,wz);
                        if (std::abs(g.latitude) > Earth::MaxLatitude) continue;
                        const float spacing = std::max(
                            Earth::HorizonOffset(std::min(x+1,Earth::HorizonSamples-1),radius,halfWindow) -
                                Earth::HorizonOffset(std::max(x-1,0),radius,halfWindow),
                            Earth::HorizonOffset(std::min(z+1,Earth::HorizonSamples-1),radius,halfWindow) -
                                Earth::HorizonOffset(std::max(z-1,0),radius,halfWindow)) * 0.5f;
                        addTiles(g, Earth::SpacingZoom(g.latitude, spacing, horizonZoom, std::min(patch->sourceZoom, 12)));
                    }
                if (keys.size() <= 256) Prefetch(keys);
            }
            for (int z = 0; z < Earth::GridSamples; ++z)
            {
                if (_stop.load()) return;
                if (_epoch.load() != _activeEpoch) break;
                for (int x = 0; x < Earth::GridSamples; ++x)
                {
                    const auto geographic = Earth::FromLocal(origin.latitude, origin.longitude,
                        patch->x + x * patch->gridMetres, patch->z + z * patch->gridMetres);
                    const auto position = Earth::ToPixel(geographic,patch->sourceZoom);
                    const double wx = patch->x + x * patch->gridMetres;
                    const double wz = patch->z + z * patch->gridMetres;
                    patch->heights[z * Earth::GridSamples + x] = EarthIslands::Height(geographic.latitude, geographic.longitude,
                        WithSeaDepth(geographic, patch->sourceZoom, Sample(position.x, position.y,patch->sourceZoom), true))
                        - float(Earth::CurvatureDrop(wx,wz));
                }
            }
            if (_epoch.load() != _activeEpoch) { handled = version; continue; }
            patch->horizonRadius = radius;
            patch->horizonZoom = Earth::HorizonZoom(centre.latitude, radius);
            patch->horizonHeights.resize(Earth::HorizonSamples * Earth::HorizonSamples);
            patch->horizonValid.resize(patch->horizonHeights.size(), 1);
            for (int z = 0; z < Earth::HorizonSamples; ++z)
            {
                if (_stop.load()) return;
                if (_epoch.load() != _activeEpoch) break;
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
                    // Terrain LOD: each vertex samples the zoom matching its own spacing, finest near
                    // the near window and coarsest at the horizon, falling back where tiles are missing.
                    const float spacing = std::max(
                        Earth::HorizonOffset(std::min(x+1,Earth::HorizonSamples-1),radius,halfWindow) -
                            Earth::HorizonOffset(std::max(x-1,0),radius,halfWindow),
                        Earth::HorizonOffset(std::min(z+1,Earth::HorizonSamples-1),radius,halfWindow) -
                            Earth::HorizonOffset(std::max(z-1,0),radius,halfWindow)) * 0.5f;
                    const int zoom = Earth::SpacingZoom(geographic.latitude, spacing, patch->horizonZoom,
                                                        std::min(patch->sourceZoom, 12));
                    const bool shore = std::max(std::abs(ox), std::abs(oz)) < 30000.0f; // land cover: the near horizon only
                    float height = WithSeaDepth(geographic, zoom, SampleFallback(geographic, zoom, patch->horizonZoom), shore);
                    // The inner near square is not drawn by this mesh. Its edge
                    // shares the selected near source; blend resolutions outside
                    // it instead of making an elevation cliff at the join.
                    const float edge = std::max(std::abs(ox),std::abs(oz)) - halfWindow;
                    const float blendWidth = Earth::NearBlendWidth(patch->gridMetres);
                    if (edge < blendWidth)
                    {
                        const auto fine = Earth::ToPixel(geographic,patch->sourceZoom);
                        const float weight = std::clamp(edge / blendWidth,0.0f,1.0f);
                        height = WithSeaDepth(geographic, patch->sourceZoom, Sample(fine.x,fine.y,patch->sourceZoom), true) * (1-weight)
                               + height * weight;
                    }
                    height = EarthIslands::Height(geographic.latitude, geographic.longitude, height);
                    patch->horizonHeights[index] = height - float(Earth::CurvatureDrop(wx,wz));
                }
            }
            patch->downloads = _downloads - downloads; patch->cacheHits = _cacheHits - hits;
            patch->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            TrimDisk();
            std::lock_guard lock(_mutex);
            if (_epoch.load() == _activeEpoch) _ready = std::move(patch);
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
