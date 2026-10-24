// SPDX-License-Identifier: GPL-3.0-or-later
#include "EarthHeightCache.hpp"
#include "EarthCoordinates.hpp"
#include "EarthIslands.hpp"
#include "EarthLandCover.hpp"

#include <Poseidon/World/Terrain/EarthProviders.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Poseidon
{
namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double MaxLatitude = 85.0511287798066;
constexpr int BathymetryZoom = 10;             // EarthTerrainStream: Terrarium's sea depths end here
constexpr float ShoreBand = 1.0f, PolderDepth = -12.0f; // EarthTerrainStream::WithSeaDepth

void Mercator(double latitude, double longitude, double& mx, double& my)
{
    const double lat = std::clamp(latitude, -MaxLatitude, MaxLatitude) * Pi / 180.0;
    double lon = std::fmod(longitude + 180.0, 360.0);
    if (lon < 0)
        lon += 360.0;
    mx = lon / 360.0;
    my = std::clamp((1.0 - std::asinh(std::tan(lat)) / Pi) * 0.5, 0.0, 1.0 - 1e-12);
}
double TexelLatitude(int z, int y, double py) // py: texel row + 0.5
{
    const double my = (y + py / EarthHeightCache::Tile) / double(1 << z);
    return std::atan(std::sinh(Pi * (1.0 - 2.0 * my))) * 180.0 / Pi;
}
double TexelLongitude(int z, int x, double px)
{
    return (x + px / EarthHeightCache::Tile) / double(1 << z) * 360.0 - 180.0;
}
void Encode(float h, uint8_t* p)
{
    const double v = std::clamp(double(h) + 32768.0, 0.0, 65535.996);
    const int whole = int(std::floor(v));
    p[0] = uint8_t(whole >> 8);
    p[1] = uint8_t(whole & 255);
    p[2] = uint8_t(std::clamp(int(std::lround((v - whole) * 256.0)), 0, 255));
    p[3] = 255;
}
} // namespace

EarthHeightCache::EarthHeightCache(WgrRenderer* renderer, const std::filesystem::path& cache) : _renderer(renderer)
{
    const auto source = EarthProviders::Load(cache).Elevation();
    _terrainRgb = source.terrainRgb;
    _tiles = std::make_unique<EarthImageryTiles>(cache, source);
    _tiles->NoFallback();
    if (!_tiles->Enabled())
    {
        _tiles.reset();
        return;
    }
    const std::vector<uint8_t> blank(size_t(AtlasSize) * AtlasSize * 4, 0);
    _atlas = wgr_texture_create(_renderer, AtlasSize, AtlasSize, WGR_TEXTURE_RGBA8, 1, 0, blank.data(), uint32_t(blank.size()));
    const std::vector<uint8_t> lookup(size_t(LookupWidth) * Levels * LookupRowsPerLevel * 4, 0);
    _lookup = wgr_texture_create(_renderer, LookupWidth, Levels * LookupRowsPerLevel, WGR_TEXTURE_RGBA8, 1, 0, lookup.data(),
                                 uint32_t(lookup.size()));
    if (!_atlas || !_lookup)
    {
        LOG_WARN(Graphics, "Earth heights: atlas allocation failed");
        return;
    }
    _slotKey.assign(Slots, Key{-1, 0, 0});
    _slotUse.assign(Slots, 0);
    _thread = std::thread([this] { Run(); });
    LOG_INFO(Graphics, "Earth heights: {} slots in a {}x{} atlas (Terrarium-encoded), zoom 0..{}", Slots, AtlasSize, AtlasSize, MaxZoom);
}

EarthHeightCache::~EarthHeightCache()
{
    _stop = true;
    _wake.notify_all();
    if (_thread.joinable())
        _thread.join();
    if (_renderer)
    {
        if (_atlas)
            wgr_texture_destroy(_renderer, _atlas);
        if (_lookup)
            wgr_texture_destroy(_renderer, _lookup);
    }
}

void EarthHeightCache::WindowOrigin(int level, double latitude, double longitude, int& ox, int& oy)
{
    const int n = 1 << level;
    if (n <= Window)
    {
        ox = oy = 0;
        return;
    }
    double mx, my;
    Mercator(latitude, longitude, mx, my);
    const int tx = int(std::floor(mx * n)), ty = int(std::floor(my * n));
    ox = ((tx - Window / 2) % n + n) % n;
    oy = std::clamp(ty - Window / 2, 0, n - Window);
}

void EarthHeightCache::Want(double latitude, double longitude, const std::set<Key>& tiles)
{
    if (!Ready())
        return;
    _latitude = latitude;
    _longitude = longitude;
    std::set<Key> all;
    for (auto [z, x, y] : tiles)
        for (z = std::min(z, MaxZoom); z >= 0; --z, x >>= 1, y >>= 1)
            if (!all.insert({z, x, y}).second)
                break; // its ancestors are in already
    static auto lastLog = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(5))
    {
        lastLog = std::chrono::steady_clock::now();
        LOG_INFO(Graphics, "Earth heights: {} tiles wanted ({} asked by nodes), {} in the atlas of {}", all.size(), tiles.size(),
                 _installed.size(), Slots);
    }
    std::lock_guard lock(_mutex);
    if (all == _wanted)
        return;
    _wanted = std::move(all);
    ++_wantedVersion;
    _wake.notify_all();
}

bool EarthHeightCache::Heights(const Key& key, std::vector<float>& heights, bool network)
{
    const auto [z, x, y] = key;
    _tiles->FetchTiles({key}, [this] { return _stop.load(); }, network);
    std::vector<uint8_t> rgba;
    int size = 0;
    if (!_tiles->TakeTile(z, x, y, rgba, size) || size <= 0)
        return false;
    heights.resize(size_t(Tile) * Tile);
    for (int j = 0; j < Tile; ++j)
        for (int i = 0; i < Tile; ++i)
        {
            const uint8_t* p = &rgba[(size_t(j * size / Tile) * size + size_t(i * size / Tile)) * 4];
            heights[size_t(j) * Tile + i] = _terrainRgb ? -10000.0f + 0.1f * float(p[0] * 65536 + p[1] * 256 + p[2])
                                                        : Earth::Terrarium(p[0], p[1], p[2]);
        }
    return true;
}

// One tile as the ground is drawn: the source heights with the patch builder's rules (EarthTerrainStream::WithSeaDepth,
// EarthIslands::Height), Terrarium-encoded.
bool EarthHeightCache::Bake(const Key& key, std::vector<uint8_t>& rgba)
{
    const auto [z, x, y] = key;
    std::vector<float> h;
    if (!Heights(key, h, true))
        return false;
    // the sea's depth under the open sea's 0 m fill, from zoom 10
    std::vector<uint8_t> fill(h.size(), 0);
    if (z > BathymetryZoom)
    {
        bool any = false;
        for (size_t i = 0; i < h.size(); ++i)
        {
            fill[i] = h[i] == 0.0f;
            any = any || fill[i];
        }
        if (any)
        {
            const int shift = z - BathymetryZoom;
            const Key parent{BathymetryZoom, x >> shift, y >> shift};
            auto it = _decoded.find(parent);
            if (it == _decoded.end())
            {
                std::vector<float> ph;
                if (Heights(parent, ph, true))
                {
                    if (_decoded.size() >= 16)
                        _decoded.erase(_decoded.begin());
                    it = _decoded.emplace(parent, std::move(ph)).first;
                }
            }
            const double scale = 1.0 / double(1 << shift);
            for (int j = 0; j < Tile; ++j)
                for (int i = 0; i < Tile; ++i)
                {
                    const size_t k = size_t(j) * Tile + i;
                    if (!fill[k])
                        continue;
                    if (it == _decoded.end())
                    {
                        h[k] = -200.0f; // no depth anywhere: deep enough for every keel (as WithSeaDepth)
                        continue;
                    }
                    // this texel's centre in the zoom-10 tile, bilinear (clamped at its edge)
                    const double px = ((x & ((1 << shift) - 1)) * Tile + i + 0.5) * scale - 0.5;
                    const double py = ((y & ((1 << shift) - 1)) * Tile + j + 0.5) * scale - 0.5;
                    const int ix = std::clamp(int(std::floor(px)), 0, Tile - 2), iy = std::clamp(int(std::floor(py)), 0, Tile - 2);
                    const float fx = float(std::clamp(px - ix, 0.0, 1.0)), fy = float(std::clamp(py - iy, 0.0, 1.0));
                    const auto& p = it->second;
                    const auto at = [&](int a, int b) { return p[size_t(b) * Tile + a]; };
                    const float d = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                                    (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
                    h[k] = std::min(d, -0.5f);
                }
        }
    }
    // the shore: near sea level, WorldCover's land stays a metre above the sea (zoom 11 on: the ground near the camera)
    if (z > BathymetryZoom)
        if (EarthLandCover* cover = EarthLandCover::Get())
        {
            std::vector<size_t> index;
            std::vector<std::pair<double, double>> points;
            for (int j = 0; j < Tile; ++j)
                for (int i = 0; i < Tile; ++i)
                {
                    const size_t k = size_t(j) * Tile + i;
                    const float source = fill[k] ? 0.0f : h[k];
                    if (!fill[k] && (source > ShoreBand || source < PolderDepth))
                        continue;
                    index.push_back(k);
                    points.push_back({TexelLatitude(z, y, j + 0.5), TexelLongitude(z, x, i + 0.5)});
                }
            if (!points.empty())
            {
                std::vector<EarthLandCover::Class> classes;
                cover->PeekMany(points, classes);
                for (size_t n = 0; n < index.size(); ++n)
                    if (classes[n] != EarthLandCover::Unknown && classes[n] != EarthLandCover::Water)
                        h[index[n]] = std::max(fill[index[n]] ? 0.0f : h[index[n]], ShoreBand);
            }
        }
    // the OFP islands, where their grids reach
    {
        const double n = double(1 << z);
        const double west = x / n * 360.0 - 180.0, east = (x + 1) / n * 360.0 - 180.0;
        const double north = TexelLatitude(z, y, 0.0), south = TexelLatitude(z, y, Tile);
        for (const auto& island : EarthIslands::All())
        {
            const double half = island.cells * island.cellSize * 0.5 / 111320.0;
            const double halfLon = half / std::max(0.05, std::cos(island.latitude * Pi / 180.0));
            if (island.latitude + half < south || island.latitude - half > north || island.longitude + halfLon < west ||
                island.longitude - halfLon > east)
                continue;
            for (int j = 0; j < Tile; ++j)
            {
                const double lat = TexelLatitude(z, y, j + 0.5);
                for (int i = 0; i < Tile; ++i)
                    h[size_t(j) * Tile + i] = EarthIslands::Height(lat, TexelLongitude(z, x, i + 0.5), h[size_t(j) * Tile + i]);
            }
        }
    }
    rgba.resize(size_t(Tile) * Tile * 4);
    for (size_t k = 0; k < h.size(); ++k)
        Encode(h[k], &rgba[k * 4]);
    return true;
}

void EarthHeightCache::Run()
{
    uint64_t seen = 0;
    while (!_stop)
    {
        std::set<Key> wanted;
        {
            std::unique_lock lock(_mutex);
            _wake.wait_for(lock, std::chrono::milliseconds(500), [&] { return _stop.load() || _wantedVersion != seen; });
            if (_stop)
                return;
            seen = _wantedVersion;
            wanted = _wanted;
        }
        ++_useClock;
        for (const auto& key : wanted)
            if (auto it = _slotOf.find(key); it != _slotOf.end())
                _slotUse[size_t(it->second)] = _useClock;
        // missing tiles, coarse first (a parent is in before its children are asked for)
        std::vector<Key> missing;
        for (const auto& key : wanted)
            if (!_slotOf.count(key) && !_failed.count(key))
                missing.push_back(key);
        std::stable_sort(missing.begin(), missing.end(), [](const Key& a, const Key& b) { return std::get<0>(a) < std::get<0>(b); });
        for (const auto& key : missing)
        {
            {
                std::lock_guard lock(_mutex);
                if (_stop || _wantedVersion != seen)
                    break; // re-plan with the newer wish list
            }
            Op op;
            if (!Bake(key, op.rgba))
            {
                if (_tiles->Failed(std::get<0>(key), std::get<1>(key), std::get<2>(key)))
                    _failed.insert(key);
                continue;
            }
            int slot = -1;
            for (int s = 0; s < Slots && slot < 0; ++s)
                if (std::get<0>(_slotKey[size_t(s)]) < 0)
                    slot = s;
            if (slot < 0)
            {
                uint64_t oldest = ~uint64_t(0);
                for (int s = 0; s < Slots; ++s)
                    if (!wanted.count(_slotKey[size_t(s)]) && _slotUse[size_t(s)] < oldest)
                        oldest = _slotUse[size_t(s)], slot = s;
                if (slot < 0)
                    break; // every slot is wanted: the finest wishes wait
                _slotOf.erase(_slotKey[size_t(slot)]);
                Op evict;
                evict.slot = slot;
                evict.key = _slotKey[size_t(slot)];
                std::lock_guard lock(_mutex);
                _ops.push_back(std::move(evict)); // empty rgba: an eviction
            }
            op.slot = slot;
            op.key = key;
            _slotKey[size_t(slot)] = key;
            _slotUse[size_t(slot)] = _useClock;
            _slotOf[key] = slot;
            std::lock_guard lock(_mutex);
            _ops.push_back(std::move(op));
        }
    }
}

void EarthHeightCache::Install()
{
    if (!Ready())
        return;
    constexpr int Budget = 6; // 256 KB a slot
    int uploaded = 0;
    {
        std::unique_lock lock(_mutex);
        while (!_ops.empty() && uploaded < Budget)
        {
            Op op = std::move(_ops.front());
            _ops.pop_front();
            lock.unlock();
            if (op.rgba.empty())
            {
                if (auto it = _installed.find(op.key); it != _installed.end() && it->second == op.slot)
                    _installed.erase(it);
            }
            else
            {
                const uint32_t sx = uint32_t(op.slot % SlotsPerRow) * Tile, sy = uint32_t(op.slot / SlotsPerRow) * Tile;
                wgr_texture_update_region(_renderer, _atlas, 0, sx, sy, Tile, Tile, op.rgba.data(), uint32_t(op.rgba.size()));
                _installed[op.key] = op.slot;
                ++uploaded;
            }
            _lookupDirty = true;
            lock.lock();
        }
    }
    int origins[Levels][2];
    for (int level = 0; level < Levels; ++level)
        WindowOrigin(level, _latitude, _longitude, origins[level][0], origins[level][1]);
    if (std::memcmp(origins, _origins, sizeof(origins)) != 0)
    {
        std::memcpy(_origins, origins, sizeof(origins));
        _lookupDirty = true;
    }
    if (!_lookupDirty)
        return;
    _lookupDirty = false;
    std::vector<uint8_t> table(size_t(LookupWidth) * Levels * LookupRowsPerLevel * 4, 0);
    const auto put = [&](int x, int row, uint32_t value)
    {
        uint8_t* p = &table[(size_t(row) * LookupWidth + x) * 4];
        p[0] = uint8_t(value & 255);
        p[1] = uint8_t((value >> 8) & 255);
        p[2] = uint8_t((value >> 16) & 255);
        p[3] = 255;
    };
    for (int level = 0; level < Levels; ++level)
    {
        put(0, level * LookupRowsPerLevel, uint32_t(_origins[level][0]));
        put(1, level * LookupRowsPerLevel, uint32_t(_origins[level][1]));
    }
    for (const auto& [key, slot] : _installed)
    {
        const auto [z, x, y] = key;
        const int n = 1 << z;
        const int dx = ((x - _origins[z][0]) % n + n) % n, dy = y - _origins[z][1];
        if (dx < Window && dy >= 0 && dy < Window)
            put(dx, z * LookupRowsPerLevel + 1 + dy, uint32_t(slot));
    }
    wgr_texture_update(_renderer, _lookup, table.data(), uint32_t(table.size()));
}
} // namespace Poseidon
