// SPDX-License-Identifier: GPL-3.0-or-later
#include "EarthTileCache.hpp"
#include "EarthIslands.hpp"

#include <Poseidon/World/Terrain/EarthProviders.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <stb_dxt.h> // implemented in Poseidon (Graphics/Textures/Bc3Encoder.cpp)
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

namespace Poseidon
{
namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double MaxLatitude = 85.0511287798066;
// The two web-mercator coordinates of a place, in [0, 1) (x from -180 degrees east, y from the north).
void Mercator(double latitude, double longitude, double& mx, double& my)
{
    const double lat = std::clamp(latitude, -MaxLatitude, MaxLatitude) * Pi / 180.0;
    double lon = std::fmod(longitude + 180.0, 360.0);
    if (lon < 0)
        lon += 360.0;
    mx = lon / 360.0;
    my = std::clamp((1.0 - std::asinh(std::tan(lat)) / Pi) * 0.5, 0.0, 1.0 - 1e-12);
}
// Metres one texel of a 256-texel tile of zoom `level` spans at `latitude`.
double TexelMetres(int level, double latitude)
{
    return 156543.03392 * std::cos(std::clamp(latitude, -MaxLatitude, MaxLatitude) * Pi / 180.0) / double(1 << level);
}
} // namespace

bool EarthTileCache::Enabled()
{
    // On by default since 2026-10-24 (close-up A/B over Lisbon: equal or sharper at 15-700 m, no BC1 blocking;
    // Desert Island unchanged). POSEIDON_EARTH_TILES=0: the window atlases.
    static const bool on = [] {
        const char* v = std::getenv("POSEIDON_EARTH_TILES");
        return !(v && v[0] == '0');
    }();
    return on;
}

EarthTileCache::EarthTileCache(WgrRenderer* renderer, const std::filesystem::path& cache) : _renderer(renderer)
{
    _tiles = std::make_unique<EarthImageryTiles>(cache, EarthProviders::Load(cache).Imagery());
    if (!_tiles->Enabled())
    {
        LOG_INFO(Graphics, "Earth tiles: no imagery provider configured; the window atlases stay in use");
        _tiles.reset();
        return;
    }
    size_t bytes = 0;
    for (int m = 0; m < Mips; ++m)
        bytes += size_t(AtlasSize >> m) * size_t(AtlasSize >> m) / 2; // BC1: 8 bytes per 4x4 block
    const std::vector<uint8_t> zeros(bytes, 0);
    _atlas = wgr_texture_create(_renderer, AtlasSize, AtlasSize, WGR_TEXTURE_BC1, Mips, 0, zeros.data(), uint32_t(zeros.size()));
    const std::vector<uint8_t> lookup(size_t(LookupWidth) * LookupRows * 4, 0);
    _lookup = wgr_texture_create(_renderer, LookupWidth, LookupRows, WGR_TEXTURE_RGBA8, 1, 0, lookup.data(),
                                 uint32_t(lookup.size()));
    if (!_atlas || !_lookup)
    {
        LOG_WARN(Graphics, "Earth tiles: atlas allocation failed; the window atlases stay in use");
        return;
    }
    // Night lights: NASA's Black Marble 2016 (VIIRS, public domain) through GIBS, zoom 3 only (~20 km a texel,
    // what a city's glow is from orbit). POSEIDON_EARTH_NIGHT_LIGHTS=0: none.
    if (const char* v = std::getenv("POSEIDON_EARTH_NIGHT_LIGHTS"); !(v && v[0] == '0'))
    {
        EarthProviders::Source night;
        night.id = "blackmarble";
        night.url = "https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/VIIRS_Black_Marble/default/2016-01-01/"
                    "GoogleMapsCompatible_Level8/{z}/{y}/{x}.png";
        night.attribution = "NASA Earth Observatory / GIBS: Black Marble 2016";
        night.maxZoom = 8;
        _night = std::make_unique<EarthImageryTiles>(cache, night);
    }
    for (int i = 0; i < NightTiles; ++i)
        _fineQueued[i][0] = _fineQueued[i][1] = _fineInstalled[i][0] = _fineInstalled[i][1] = -1;
    _slotKey.assign(Slots, Key{-1, 0, 0});
    _slotUse.assign(Slots, 0);
    _thread = std::thread([this] { Run(); });
    LOG_INFO(Graphics, "Earth tiles: {} slots in a {}x{} BC1 atlas ({} MB), zoom 0..{}", Slots, AtlasSize, AtlasSize,
             bytes / (1024 * 1024), MaxZoom);
}

EarthTileCache::~EarthTileCache()
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

void EarthTileCache::View(double latitude, double longitude, double heightAboveGround, double pixelAngle)
{
    if (!std::isfinite(latitude) || !std::isfinite(longitude))
        return;
    _lastLatitude = latitude;
    _lastLongitude = longitude;
    std::lock_guard lock(_mutex);
    // Re-plan when the camera has moved by a few metres or changed height noticeably: the plan is cheap,
    // but the worker should not spin.
    const double moved = std::hypot((latitude - _view.latitude) * 111000.0,
                                    (longitude - _view.longitude) * 111000.0 * std::cos(latitude * Pi / 180.0));
    const double h = std::max(heightAboveGround, 1.0);
    if (moved < 0.05 * std::max(_view.height, 20.0) && std::abs(h - _view.height) < 0.1 * _view.height && _view.version)
        return;
    _view.latitude = latitude;
    _view.longitude = longitude;
    _view.height = h;
    _view.pixelAngle = std::clamp(pixelAngle, 1e-5, 0.01);
    ++_view.version;
    _wake.notify_all();
}

void EarthTileCache::WindowOrigin(int level, double latitude, double longitude, int& ox, int& oy)
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

// The tiles the view needs, coarse first: every tile of zoom 0..3 (85, the globe from anywhere), then per
// level the tiles near enough for one of their texels to span no more than a pixel -- the screen-space
// error rule -- within the level's 16x16 lookup window. The per-level radius is ~3 tiles at the ground and
// shrinks with height, so each level's set lies inside its parent's.
std::vector<EarthTileCache::Key> EarthTileCache::Plan(const ViewState& v) const
{
    std::vector<Key> keys;
    double mx, my;
    Mercator(v.latitude, v.longitude, mx, my);
    for (int level = 0; level <= MaxZoom; ++level)
    {
        const int n = 1 << level;
        if (level <= 3)
        {
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    keys.emplace_back(level, x, y);
            continue;
        }
        const double texel = TexelMetres(level, v.latitude);
        const double reach = texel / v.pixelAngle; // distance at which a texel spans one pixel
        if (reach <= v.height)
            break; // and every finer level too
        const double radiusTiles = std::sqrt(reach * reach - v.height * v.height) / (texel * TileTexels) + 0.75;
        int ox, oy;
        WindowOrigin(level, v.latitude, v.longitude, ox, oy);
        const double cx = mx * n, cy = my * n;
        std::vector<std::pair<double, Key>> level_keys;
        for (int dy = 0; dy < Window; ++dy)
            for (int dx = 0; dx < Window; ++dx)
            {
                const int x = (ox + dx) % n, y = oy + dy;
                if (y >= n)
                    continue;
                // distance from the camera to the tile's centre, in tiles (x wraps)
                double ddx = std::fmod(x + 0.5 - cx + 1.5 * n, double(n)) - 0.5 * n;
                const double ddy = y + 0.5 - cy;
                const double d = std::hypot(ddx, ddy);
                if (d <= radiusTiles)
                    level_keys.push_back({d, Key{level, x, y}});
            }
        std::sort(level_keys.begin(), level_keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& k : level_keys)
            keys.push_back(k.second);
    }
    if (keys.size() > size_t(TileSlots - 32))
        keys.resize(TileSlots - 32); // finest last: they are the ones dropped
    return keys;
}

namespace
{
// The geographic bounds of tile (z, x, y), degrees.
void TileBounds(int z, int x, int y, double& south, double& west, double& north, double& east)
{
    const double n = double(1 << z);
    west = x / n * 360.0 - 180.0;
    east = (x + 1) / n * 360.0 - 180.0;
    north = std::atan(std::sinh(Pi * (1.0 - 2.0 * y / n))) * 180.0 / Pi;
    south = std::atan(std::sinh(Pi * (1.0 - 2.0 * (y + 1) / n))) * 180.0 / Pi;
}
} // namespace

bool EarthTileCache::OverIsland(int z, int x, int y) const
{
    double s, w, n, e;
    TileBounds(z, x, y, s, w, n, e);
    for (const auto& island : _islands)
        if (s < island.north && n > island.south && w < island.east && e > island.west)
            return true;
    return false;
}

void EarthTileCache::ApplyIslands(int z, int x, int y, std::vector<uint8_t>& rgba, int size) const
{
    const double scale = double(1 << z);
    for (const auto& island : _islands)
    {
        double s, w, n, e;
        TileBounds(z, x, y, s, w, n, e);
        if (!(s < island.north && n > island.south && w < island.east && e > island.west))
            continue;
        const double metresPerDegree = 111320.0;
        for (int py = 0; py < size; ++py)
        {
            const double my = (y + (py + 0.5) / size) / scale;
            const double lat = std::atan(std::sinh(Pi * (1.0 - 2.0 * my))) * 180.0 / Pi;
            const double v = (island.north - lat) / (island.north - island.south);
            if (v < 0 || v >= 1)
                continue;
            const double cosLat = std::cos(lat * Pi / 180.0);
            for (int px = 0; px < size; ++px)
            {
                const double lon = (x + (px + 0.5) / size) / scale * 360.0 - 180.0;
                const double u = (lon - island.west) / (island.east - island.west);
                if (u < 0 || u >= 1)
                    continue;
                // feathered over 100 m at the image's edge (which is open water)
                const double edge = std::min({(lon - island.west) * cosLat, (island.east - lon) * cosLat, lat - island.south,
                                              island.north - lat}) * metresPerDegree;
                const double blend = std::clamp(edge / 100.0, 0.0, 1.0);
                const int ix = std::min(int(u * island.width), island.width - 1), iy = std::min(int(v * island.height), island.height - 1);
                const uint8_t* src = &island.rgba[(size_t(iy) * island.width + ix) * 4];
                uint8_t* dst = &rgba[(size_t(py) * size + px) * 4];
                for (int c = 0; c < 3; ++c)
                    dst[c] = uint8_t(std::lround(dst[c] + (src[c] - dst[c]) * blend));
                dst[3] = 255;
            }
        }
    }
}

// One mip level of a slot (size x size RGBA) as BC1 blocks.
std::vector<uint8_t> EarthTileCache::EncodeMip(const std::vector<uint8_t>& rgba, int size)
{
    const int blocks = size / 4;
    std::vector<uint8_t> out(size_t(blocks) * blocks * 8);
    uint8_t block[64];
    for (int by = 0; by < blocks; ++by)
        for (int bx = 0; bx < blocks; ++bx)
        {
            for (int y = 0; y < 4; ++y)
                std::memcpy(block + y * 16, &rgba[(size_t(by * 4 + y) * size + bx * 4) * 4], 16);
            stb_compress_dxt_block(&out[(size_t(by) * blocks + bx) * 8], block, 0, STB_DXT_NORMAL);
        }
    return out;
}

// 256 texels with an 8-texel border of their own edge, then 2 box-filtered levels, as BC1.
void EarthTileCache::EncodeSlot(const std::vector<uint8_t>& tile, int size, Op& op)
{
    std::vector<uint8_t> level0(size_t(SlotTexels) * SlotTexels * 4);
    for (int ty = 0; ty < SlotTexels; ++ty)
        for (int tx = 0; tx < SlotTexels; ++tx)
        {
            const int sx = std::clamp((tx - Border) * size / TileTexels, 0, size - 1);
            const int sy = std::clamp((ty - Border) * size / TileTexels, 0, size - 1);
            std::memcpy(&level0[(size_t(ty) * SlotTexels + tx) * 4], &tile[(size_t(sy) * size + sx) * 4], 4);
            level0[(size_t(ty) * SlotTexels + tx) * 4 + 3] = 255;
        }
    std::vector<uint8_t> current = std::move(level0);
    for (int m = 0; m < Mips; ++m)
    {
        const int s = SlotTexels >> m;
        op.mips[m] = EncodeMip(current, s);
        if (m + 1 < Mips)
        {
            const int h = s / 2;
            std::vector<uint8_t> next(size_t(h) * h * 4);
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < h; ++xx)
                    for (int c = 0; c < 4; ++c)
                    {
                        const auto at = [&](int px, int py) { return int(current[(size_t(py) * s + px) * 4 + c]); };
                        next[(size_t(yy) * h + xx) * 4 + c] =
                            uint8_t((at(2 * xx, 2 * yy) + at(2 * xx + 1, 2 * yy) + at(2 * xx, 2 * yy + 1) + at(2 * xx + 1, 2 * yy + 1) + 2) / 4);
                    }
            current = std::move(next);
        }
    }
}

bool EarthTileCache::LoadNight()
{
    if (!_night)
        return true;
    std::vector<std::tuple<int, int, int>> missing;
    constexpr int n = 1 << NightLevel;
    for (int i = 0; i < NightTiles; ++i)
        if (!_nightLoaded[i] && !_night->Failed(NightLevel, i % n, i / n))
            missing.emplace_back(NightLevel, i % n, i / n);
    if (missing.empty())
        return true;
    _night->FetchTiles(missing, [this] { return _stop.load(); }, true);
    unsigned loaded = 0;
    for (const auto& [z, x, y] : missing)
    {
        std::vector<uint8_t> tile;
        int size = 0;
        if (!_night->TakeTile(z, x, y, tile, size))
            continue;
        Op op;
        EncodeSlot(tile, size, op);
        op.slot = NightBase + y * n + x;
        op.key = Key{-2, x, y}; // not a pyramid tile: no lookup entry
        _nightLoaded[y * n + x] = true;
        ++loaded;
        std::lock_guard lock(_mutex);
        _ops.push_back(std::move(op));
    }
    LOG_INFO(Graphics, "Earth tiles: {} of {} night-light tiles loaded", loaded, missing.size());
    return loaded == missing.size();
}

bool EarthTileCache::LoadNightFine(double latitude, double longitude)
{
    if (!_night)
        return true;
    constexpr int n = 1 << NightFineLevel, w = 8;
    double mx, my;
    Mercator(latitude, longitude, mx, my);
    const int cx = int(std::floor(mx * n)), cy = int(std::floor(my * n));
    const int y0 = std::clamp(cy - w / 2, 0, n - w);
    std::vector<std::tuple<int, int, int>> missing;
    for (int dy = 0; dy < w; ++dy)
        for (int dx = -w / 2; dx < w / 2; ++dx)
        {
            const int x = ((cx + dx) % n + n) % n, y = y0 + dy;
            const int s = (y % w) * w + x % w;
            if ((_fineQueued[s][0] != x || _fineQueued[s][1] != y) && !_night->Failed(NightFineLevel, x, y))
                missing.emplace_back(NightFineLevel, x, y);
        }
    if (missing.empty())
        return true;
    _night->FetchTiles(missing, [this] { return _stop.load(); }, true);
    size_t loaded = 0;
    for (const auto& [z, x, y] : missing)
    {
        std::vector<uint8_t> tile;
        int size = 0;
        if (!_night->TakeTile(z, x, y, tile, size))
            continue;
        const int s = (y % w) * w + x % w;
        Op op;
        EncodeSlot(tile, size, op);
        op.slot = NightFineBase + s;
        op.key = Key{-3, x, y};
        _fineQueued[s][0] = x;
        _fineQueued[s][1] = y;
        ++loaded;
        std::lock_guard lock(_mutex);
        _ops.push_back(std::move(op));
    }
    return loaded == missing.size();
}

void EarthTileCache::Run()
{
    for (const auto& island : EarthIslands::All())
    {
        int w = 0, h = 0, channels = 0;
        const auto path = island.image.u8string();
        stbi_uc* pixels = stbi_load(reinterpret_cast<const char*>(path.c_str()), &w, &h, &channels, 4);
        if (!pixels)
            continue;
        IslandImage image;
        image.rgba.assign(pixels, pixels + size_t(w) * h * 4);
        stbi_image_free(pixels);
        image.width = w;
        image.height = h;
        image.south = island.south;
        image.west = island.west;
        image.north = island.north;
        image.east = island.east;
        // the sea's colour: the image's top-left corner, which is open water
        std::memcpy(image.sea, image.rgba.data(), 4);
        _islands.push_back(std::move(image));
    }
    uint64_t planned = 0;
    std::vector<Key> plan;
    std::set<Key> wanted;
    bool night = false, fine = false;
    auto nightRetry = std::chrono::steady_clock::now(), fineRetry = nightRetry;
    int fineTile[2] = {-1, -1};
    while (!_stop)
    {
        // After the first plan's coarse tiles (the globe), and again every half minute while any are missing.
        if (!night && planned && std::chrono::steady_clock::now() >= nightRetry)
        {
            night = LoadNight();
            nightRetry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
        // The zoom-6 window: when the camera's tile changes, else retried while some are missing.
        if (planned)
        {
            ViewState v;
            {
                std::lock_guard lock(_mutex);
                v = _view;
            }
            double mx, my;
            Mercator(v.latitude, v.longitude, mx, my);
            const int at[2] = {int(mx * (1 << NightFineLevel)), int(my * (1 << NightFineLevel))};
            const bool moved = at[0] != fineTile[0] || at[1] != fineTile[1];
            if (moved || (!fine && std::chrono::steady_clock::now() >= fineRetry))
            {
                fineTile[0] = at[0];
                fineTile[1] = at[1];
                fine = LoadNightFine(v.latitude, v.longitude);
                fineRetry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            }
        }
        ViewState view;
        {
            std::unique_lock lock(_mutex);
            _wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return _stop.load() || _view.version != planned; });
            if (_stop)
                return;
            view = _view;
        }
        if (!view.version)
            continue;
        const bool replanned = view.version != planned;
        planned = view.version;
        if (replanned || plan.empty())
        {
            plan = Plan(view);
            wanted = std::set<Key>(plan.begin(), plan.end());
            ++_useClock;
            for (const auto& key : plan)
                if (auto it = _slotOf.find(key); it != _slotOf.end())
                    _slotUse[size_t(it->second)] = _useClock;
        }
        const auto superseded = [&] {
            std::lock_guard lock(_mutex);
            return _stop.load() || _view.version != planned;
        };
        // Level by level, coarse first, so a parent is in the atlas before its children are asked for.
        bool full = false;
        for (size_t begin = 0; begin < plan.size() && !full && !superseded();)
        {
            const int level = std::get<0>(plan[begin]);
            size_t end = begin;
            std::vector<std::tuple<int, int, int>> missing;
            while (end < plan.size() && std::get<0>(plan[end]) == level)
            {
                const auto [kz, kx, ky] = plan[end];
                if (!_slotOf.count(plan[end]) && (!_tiles->Failed(kz, kx, ky) || OverIsland(kz, kx, ky)))
                    missing.push_back(plan[end]);
                ++end;
            }
            if (!missing.empty())
                _tiles->FetchTiles(missing, superseded, true);
            for (const auto& key : missing)
            {
                const auto [z, x, y] = key;
                std::vector<uint8_t> tile;
                int size = 0;
                const bool island = OverIsland(z, x, y);
                if (!_tiles->TakeTile(z, x, y, tile, size))
                {
                    if (!island || !_tiles->Failed(z, x, y))
                        continue;
                    // no provider tile over the island's sea: the island image over the sea's colour
                    size = TileTexels;
                    tile.assign(size_t(size) * size * 4, 0);
                    for (size_t i = 0; i < tile.size(); i += 4)
                        std::memcpy(&tile[i], _islands.front().sea, 4);
                }
                if (island)
                    ApplyIslands(z, x, y, tile, size);
                // a free slot, else the least recently wanted one that is not wanted now
                int slot = -1;
                for (int s = 0; s < TileSlots && slot < 0; ++s)
                    if (std::get<0>(_slotKey[size_t(s)]) < 0)
                        slot = s;
                Op op;
                if (slot < 0)
                {
                    uint64_t oldest = ~uint64_t(0);
                    for (int s = 0; s < TileSlots; ++s)
                        if (!wanted.count(_slotKey[size_t(s)]) && _slotUse[size_t(s)] < oldest)
                            oldest = _slotUse[size_t(s)], slot = s;
                    if (slot < 0)
                    {
                        full = true;
                        break;
                    }
                    Op evict;
                    evict.evict = true;
                    evict.slot = slot;
                    evict.key = _slotKey[size_t(slot)];
                    _slotOf.erase(evict.key);
                    std::lock_guard lock(_mutex);
                    _ops.push_back(std::move(evict));
                }
                EncodeSlot(tile, size, op);
                op.slot = slot;
                op.key = key;
                _slotKey[size_t(slot)] = key;
                _slotUse[size_t(slot)] = _useClock;
                _slotOf[key] = slot;
                std::lock_guard lock(_mutex);
                _ops.push_back(std::move(op));
            }
            begin = end;
        }
    }
}

void EarthTileCache::Install()
{
    if (!Ready())
        return;
    // Finished tiles into the atlas: a BC1 slot is ~50 KB over its 3 levels, a budget of 24 a frame.
    constexpr int Budget = 24;
    int uploaded = 0;
    size_t queued = 0;
    {
        std::unique_lock lock(_mutex);
        while (!_ops.empty() && uploaded < Budget)
        {
            Op op = std::move(_ops.front());
            _ops.pop_front();
            lock.unlock();
            if (op.evict)
            {
                if (auto it = _installed.find(op.key); it != _installed.end() && it->second == op.slot)
                    _installed.erase(it);
            }
            else
            {
                const int sx = op.slot % SlotsPerRow, sy = op.slot / SlotsPerRow;
                for (int m = 0; m < Mips; ++m)
                {
                    const uint32_t s = uint32_t(SlotTexels >> m);
                    wgr_texture_update_region(_renderer, _atlas, uint32_t(m), uint32_t(sx) * s, uint32_t(sy) * s, s, s,
                                              op.mips[m].data(), uint32_t(op.mips[m].size()));
                }
                if (std::get<0>(op.key) >= 0)
                    _installed[op.key] = op.slot;
                else if (std::get<0>(op.key) == -3)
                {
                    _fineInstalled[op.slot - NightFineBase][0] = std::get<1>(op.key);
                    _fineInstalled[op.slot - NightFineBase][1] = std::get<2>(op.key);
                }
                ++uploaded;
            }
            _lookupDirty = true;
            lock.lock();
        }
        queued = _ops.size();
    }
    // The lookup table: each level's window round the camera and the slots of the tiles in it.
    int origins[Levels][2];
    for (int level = 0; level < Levels; ++level)
        WindowOrigin(level, _lastLatitude, _lastLongitude, origins[level][0], origins[level][1]);
    if (std::memcmp(origins, _origins, sizeof(origins)) != 0)
    {
        std::memcpy(_origins, origins, sizeof(origins));
        _lookupDirty = true;
    }
    if (_lookupDirty)
    {
        _lookupDirty = false;
        std::vector<uint8_t> table(size_t(LookupWidth) * LookupRows * 4, 0);
        const auto put = [&](int x, int row, uint32_t value, uint8_t alpha) {
            uint8_t* p = &table[(size_t(row) * LookupWidth + x) * 4];
            p[0] = uint8_t(value & 255);
            p[1] = uint8_t((value >> 8) & 255);
            p[2] = uint8_t((value >> 16) & 255);
            p[3] = alpha;
        };
        for (int level = 0; level < Levels; ++level)
        {
            put(0, level * LookupRowsPerLevel, uint32_t(_origins[level][0]), 255);
            put(1, level * LookupRowsPerLevel, uint32_t(_origins[level][1]), 255);
        }
        for (const auto& [key, slot] : _installed)
        {
            const auto [z, x, y] = key;
            if (z >= Levels)
                continue;
            const int n = 1 << z;
            const int dx = ((x - _origins[z][0]) % n + n) % n, dy = y - _origins[z][1];
            if (dx < Window && dy >= 0 && dy < Window)
                put(dx, z * LookupRowsPerLevel + 1 + dy, uint32_t(slot), 255);
        }
        for (int i = 0; i < NightTiles; ++i)
            if (_fineInstalled[i][0] >= 0)
            {
                uint8_t* p = &table[(size_t(NightFineRow + i / LookupWidth) * LookupWidth + i % LookupWidth) * 4];
                p[0] = uint8_t(_fineInstalled[i][0]);
                p[1] = uint8_t(_fineInstalled[i][1]);
                p[3] = 255;
            }
        wgr_texture_update(_renderer, _lookup, table.data(), uint32_t(table.size()));
    }
    _stats.resident = unsigned(_installed.size());
    _stats.uploadedThisFrame = unsigned(uploaded);
    _stats.queued = unsigned(queued);
}
} // namespace Poseidon
