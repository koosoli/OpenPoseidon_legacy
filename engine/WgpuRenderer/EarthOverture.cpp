// SPDX-License-Identifier: GPL-3.0-or-later
#include "EarthOverture.hpp"
#include "EarthImageryTiles.hpp" // EarthDownloadShare

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <stb_image.h> // stbi_zlib_decode_noheader_malloc (implemented in Poseidon, Graphics/Textures/Image.cpp)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace Poseidon
{
namespace
{
constexpr double CellDegrees = 0.02; // EarthBuildingStream::CellDegrees
constexpr int TileZoom = 14;
constexpr size_t MaxCellBuildings = 2000; // EarthBuildingStream::MaxBuildings
constexpr size_t MaxRingPoints = 128;     // EarthBuildingStream::MaxVertices
constexpr double Pi = 3.14159265358979323846;

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(input), {});
    return true;
}
void WriteFile(const std::filesystem::path& path, const void* data, size_t size)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temporary = path.string() + ".part";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(static_cast<const char*>(data), std::streamsize(size));
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec)
        std::filesystem::remove(temporary, ec);
}
size_t Receive(char* data, size_t size, size_t count, void* destination)
{
    auto& bytes = *static_cast<std::vector<uint8_t>*>(destination);
    const size_t n = size * count;
    if (bytes.size() + n > 64u * 1024 * 1024)
        return 0;
    bytes.insert(bytes.end(), data, data + n);
    return n;
}
int Progress(void* cancelled, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto& f = *static_cast<const std::function<bool()>*>(cancelled);
    return f && f() ? 1 : 0;
}
// One HTTP GET (a byte range when `range` is set), retried: the CDN resets a share of connections.
bool Get(const std::string& url, const std::string& range, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled,
         long& status)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        bytes.clear();
        status = 0;
        CURL* curl = curl_easy_init();
        if (!curl)
            return false;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (!range.empty())
            curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenPoseidon-Earth/1");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        if (void* share = EarthDownloadShare())
            curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(share));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Receive);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
        const CURLcode result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        if (result == CURLE_OK && (status == 200 || status == 206))
            return true;
        if ((status >= 400 && status < 500 && status != 408 && status != 429) || (cancelled && cancelled()))
            return false;
    }
    return false;
}
// gzip (or bare) to bytes.
bool Gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    if (in.size() < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
    {
        out = in; // not gzip'd
        return true;
    }
    const uint8_t flags = in[3];
    size_t p = 10;
    if (flags & 4)
    {
        if (p + 2 > in.size())
            return false;
        p += 2 + size_t(in[p] | in[p + 1] << 8);
    }
    if (flags & 8)
        while (p < in.size() && in[p++] != 0) {}
    if (flags & 16)
        while (p < in.size() && in[p++] != 0) {}
    if (flags & 2)
        p += 2;
    if (p >= in.size())
        return false;
    int length = 0;
    char* raw = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char*>(in.data() + p), int(in.size() - p), &length);
    if (!raw)
        return false;
    out.assign(raw, raw + length);
    stbi_image_free(raw);
    return true;
}
struct Reader
{
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    uint64_t Varint()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7)
        {
            if (p >= end)
            {
                ok = false;
                return 0;
            }
            const uint8_t b = *p++;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
        }
        ok = false;
        return 0;
    }
    bool More() const { return ok && p < end; }
    // a protobuf field: its number and wire type, and for length-delimited the bytes
    bool Field(uint32_t& number, uint32_t& wire, Reader& sub, uint64_t& value)
    {
        const uint64_t key = Varint();
        number = uint32_t(key >> 3);
        wire = uint32_t(key & 7);
        switch (wire)
        {
        case 0: value = Varint(); break;
        case 1:
            if (end - p < 8) { ok = false; return false; }
            std::memcpy(&value, p, 8);
            p += 8;
            break;
        case 2:
        {
            const uint64_t n = Varint();
            if (!ok || uint64_t(end - p) < n) { ok = false; return false; }
            sub = {p, p + n};
            p += n;
            break;
        }
        case 5:
        {
            uint32_t v32 = 0;
            if (end - p < 4) { ok = false; return false; }
            std::memcpy(&v32, p, 4);
            value = v32;
            p += 4;
            break;
        }
        default: ok = false; return false;
        }
        return ok;
    }
};
int64_t Zigzag(uint64_t v) { return int64_t(v >> 1) ^ -int64_t(v & 1); }

// PMTiles v3: tile (z, x, y) on the Hilbert curve
uint64_t TileId(int z, uint32_t x, uint32_t y)
{
    uint64_t acc = 0;
    for (int a = 0; a < z; ++a)
        acc += uint64_t(1) << (2 * a);
    uint64_t d = 0;
    for (uint32_t s = z > 0 ? 1u << (z - 1) : 0; s > 0; s >>= 1)
    {
        const uint32_t rx = (x & s) ? 1 : 0, ry = (y & s) ? 1 : 0;
        d += uint64_t(s) * s * ((3 * rx) ^ ry);
        if (ry == 0)
        {
            if (rx == 1)
            {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            std::swap(x, y);
        }
    }
    return acc + d;
}

struct Building
{
    std::string id;
    double area = 0;
    std::vector<std::pair<double, double>> points; // lat, lon, closed (first repeated)
    std::vector<std::pair<std::string, std::string>> tags;
};
std::string Clean(std::string s)
{
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '"' || c == '\\' || uint8_t(c) < 32; }), s.end());
    return s.substr(0, 64);
}
// The buildings of one zoom-14 MVT tile whose centroid lies in [south, north) x [west, east).
void DecodeTile(const std::vector<uint8_t>& mvt, uint32_t tx, uint32_t ty, double south, double west, double north, double east,
                std::map<std::string, Building>& out)
{
    Reader tile{mvt.data(), mvt.data() + mvt.size()};
    const double n = double(1u << TileZoom);
    while (tile.More())
    {
        uint32_t number, wire;
        Reader layer{nullptr, nullptr};
        uint64_t value = 0;
        if (!tile.Field(number, wire, layer, value))
            break;
        if (number != 3 || wire != 2)
            continue;
        // the layer: its name, keys and values first (they may follow the features), then the features
        std::string name;
        std::vector<std::string> keys, values;
        std::vector<Reader> features;
        double extent = 4096;
        while (layer.More())
        {
            Reader sub{nullptr, nullptr};
            if (!layer.Field(number, wire, sub, value))
                break;
            if (number == 1 && wire == 2)
                name.assign(reinterpret_cast<const char*>(sub.p), size_t(sub.end - sub.p));
            else if (number == 2 && wire == 2)
                features.push_back(sub);
            else if (number == 3 && wire == 2)
                keys.emplace_back(reinterpret_cast<const char*>(sub.p), size_t(sub.end - sub.p));
            else if (number == 4 && wire == 2)
            {
                std::string text;
                while (sub.More())
                {
                    uint32_t vn, vw;
                    Reader s{nullptr, nullptr};
                    uint64_t v = 0;
                    if (!sub.Field(vn, vw, s, v))
                        break;
                    char buffer[48];
                    switch (vn)
                    {
                    case 1: text.assign(reinterpret_cast<const char*>(s.p), size_t(s.end - s.p)); break;
                    case 2: { float f; uint32_t b = uint32_t(v); std::memcpy(&f, &b, 4); std::snprintf(buffer, sizeof(buffer), "%g", f); text = buffer; break; }
                    case 3: { double d; std::memcpy(&d, &v, 8); std::snprintf(buffer, sizeof(buffer), "%g", d); text = buffer; break; }
                    case 4: case 5: std::snprintf(buffer, sizeof(buffer), "%lld", (long long)v); text = buffer; break;
                    case 6: std::snprintf(buffer, sizeof(buffer), "%lld", (long long)Zigzag(v)); text = buffer; break;
                    case 7: text = v ? "true" : "false"; break;
                    default: break;
                    }
                }
                values.push_back(std::move(text));
            }
            else if (number == 5 && wire == 0)
                extent = double(std::max<uint64_t>(value, 1));
        }
        if (name != "building")
            continue;
        for (Reader feature : features)
        {
            std::vector<uint64_t> tags, geometry;
            uint64_t type = 0;
            while (feature.More())
            {
                Reader sub{nullptr, nullptr};
                if (!feature.Field(number, wire, sub, value))
                    break;
                if ((number == 2 || number == 4) && wire == 2)
                    while (sub.More())
                        (number == 2 ? tags : geometry).push_back(sub.Varint());
                else if (number == 3 && wire == 0)
                    type = value;
            }
            if (type != 3) // polygons only
                continue;
            std::map<std::string, std::string> attributes;
            for (size_t i = 0; i + 1 < tags.size(); i += 2)
                if (tags[i] < keys.size() && tags[i + 1] < values.size())
                    attributes[keys[tags[i]]] = values[tags[i + 1]];
            if (attributes["is_underground"] == "true")
                continue;
            // exterior rings (positive area in tile coordinates, y down); holes are left out, as for OSM outlines
            std::vector<std::vector<std::pair<int64_t, int64_t>>> rings;
            int64_t x = 0, y = 0;
            for (size_t i = 0; i < geometry.size();)
            {
                const uint32_t command = uint32_t(geometry[i] & 7), count = uint32_t(geometry[i] >> 3);
                ++i;
                if (command == 1 || command == 2)
                {
                    for (uint32_t k = 0; k < count && i + 1 < geometry.size(); ++k, i += 2)
                    {
                        x += Zigzag(geometry[i]);
                        y += Zigzag(geometry[i + 1]);
                        if (command == 1)
                            rings.emplace_back();
                        if (!rings.empty())
                            rings.back().push_back({x, y});
                    }
                }
                else if (command != 7)
                    break;
            }
            int part = 0;
            for (const auto& ring : rings)
            {
                if (ring.size() < 3 || ring.size() > MaxRingPoints)
                    continue;
                double area = 0, cx = 0, cy = 0;
                for (size_t i = 0; i < ring.size(); ++i)
                {
                    const auto& a = ring[i];
                    const auto& b = ring[(i + 1) % ring.size()];
                    const double cross = double(a.first) * double(b.second) - double(b.first) * double(a.second);
                    area += cross;
                    cx += double(a.first + b.first) * cross;
                    cy += double(a.second + b.second) * cross;
                }
                if (area <= 0)
                    continue; // a hole
                cx /= 3.0 * area;
                cy /= 3.0 * area;
                const auto toGeo = [&](double px, double py)
                {
                    const double mx = (tx + px / extent) / n, my = (ty + py / extent) / n;
                    return std::make_pair(std::atan(std::sinh(Pi * (1.0 - 2.0 * my))) * 180.0 / Pi, mx * 360.0 - 180.0);
                };
                const auto centre = toGeo(cx, cy);
                if (!(centre.first >= south && centre.first < north && centre.second >= west && centre.second < east))
                    continue;
                Building b;
                b.id = attributes["id"] + "#" + std::to_string(part++);
                b.area = area;
                for (const auto& point : ring)
                {
                    const auto geo = toGeo(double(point.first), double(point.second));
                    if (!b.points.empty() && std::abs(geo.first - b.points.back().first) < 2e-7 &&
                        std::abs(geo.second - b.points.back().second) < 2e-7)
                        continue; // the builder refuses repeated points
                    b.points.push_back(geo);
                }
                if (b.points.size() < 3)
                    continue;
                b.points.push_back(b.points.front());
                const std::string cls = attributes["class"];
                b.tags.push_back({"building", Clean(cls.empty() ? "yes" : cls)});
                for (const auto& [from, to] : {std::pair<const char*, const char*>{"height", "height"},
                                               {"num_floors", "building:levels"},
                                               {"roof_shape", "roof:shape"},
                                               {"min_height", "min_height"},
                                               {"roof_height", "roof:height"}})
                    if (auto it = attributes.find(from); it != attributes.end() && !it->second.empty())
                        b.tags.push_back({to, Clean(it->second)});
                // a building crossing a tile edge comes from both tiles, clipped: keep the larger piece
                auto existing = out.find(b.id);
                if (existing == out.end() || existing->second.area < b.area)
                    out[b.id] = std::move(b);
            }
        }
    }
}
uint64_t NumericId(const std::string& id)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for (const char c : id)
    {
        h ^= uint8_t(c);
        h *= 1099511628211ull;
    }
    h &= (uint64_t(1) << 53) - 1;
    return h ? h : 1;
}
} // namespace

bool EarthOverture::Enabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("POSEIDON_EARTH_BUILDINGS_SOURCE");
        return !(v && std::strcmp(v, "overpass") == 0);
    }();
    return on;
}

EarthOverture& EarthOverture::Instance()
{
    static EarthOverture instance;
    return instance;
}

bool EarthOverture::Range(uint64_t offset, uint64_t length, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled,
                          long& status)
{
    if (length == 0)
    {
        bytes.clear();
        return true;
    }
    return Get(_url, std::to_string(offset) + "-" + std::to_string(offset + length - 1), bytes, cancelled, status) &&
           bytes.size() == length;
}

// The latest release (Overture's STAC catalog, checked weekly; old releases are taken down) and its archive header.
bool EarthOverture::Open(const std::filesystem::path& cache, const std::function<bool()>& cancelled)
{
    if (_opened)
        return _header.valid;
    const auto root = cache / "overture";
    const auto releaseFile = root / "release.txt";
    std::error_code ec;
    std::vector<uint8_t> text;
    std::string release;
    bool fresh = false;
    if (ReadFile(releaseFile, text) && !text.empty())
    {
        release.assign(text.begin(), text.end());
        const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(releaseFile, ec);
        fresh = !ec && age < std::chrono::hours(24 * 7);
    }
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!fresh || release.empty())
        {
            std::vector<uint8_t> catalog;
            long status = 0;
            if (Get("https://stac.overturemaps.org/catalog.json", "", catalog, cancelled, status))
            {
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
                    cJSON_ParseWithLength(reinterpret_cast<const char*>(catalog.data()), catalog.size()), cJSON_Delete);
                std::string latest;
                const cJSON* link = nullptr;
                cJSON_ArrayForEach(link, cJSON_GetObjectItemCaseSensitive(json.get(), "links"))
                {
                    const cJSON* rel = cJSON_GetObjectItemCaseSensitive(link, "rel");
                    const cJSON* href = cJSON_GetObjectItemCaseSensitive(link, "href");
                    if (!cJSON_IsString(rel) || std::strcmp(rel->valuestring, "child") != 0 || !cJSON_IsString(href))
                        continue;
                    // https://stac.overturemaps.org/<release>/catalog.json
                    std::string h = href->valuestring;
                    const auto end = h.rfind('/');
                    const auto begin = end == std::string::npos ? std::string::npos : h.rfind('/', end - 1);
                    if (begin != std::string::npos)
                        latest = std::max(latest, h.substr(begin + 1, end - begin - 1));
                }
                if (!latest.empty() && latest.size() < 32)
                {
                    release = latest;
                    WriteFile(releaseFile, release.data(), release.size());
                }
            }
        }
        if (release.empty())
            break;
        _release = release;
        _dir = root / release;
        _url = "https://tiles.overturemaps.org/" + release + "/buildings.pmtiles";
        std::vector<uint8_t> header;
        if (!ReadFile(_dir / "header.bin", header) || header.size() < 16384)
        {
            long status = 0;
            if (!Range(0, 16384, header, cancelled, status))
            {
                if (status == 404 || status == 410) // taken down: look the release up again
                {
                    fresh = false;
                    std::filesystem::remove(releaseFile, ec);
                    continue;
                }
                return false; // the network: tried again on the next request
            }
            WriteFile(_dir / "header.bin", header.data(), header.size());
        }
        if (std::memcmp(header.data(), "PMTiles", 7) != 0 || header[7] != 3)
            break;
        const auto u64 = [&](size_t at)
        {
            uint64_t v;
            std::memcpy(&v, header.data() + at, 8);
            return v;
        };
        _header.rootOffset = u64(8);
        _header.rootLength = u64(16);
        _header.leafOffset = u64(40);
        _header.dataOffset = u64(56);
        _header.minZoom = header[100];
        _header.maxZoom = header[101];
        if (header[97] != 2 || header[99] != 1 || _header.rootOffset + _header.rootLength > header.size())
        {
            LOG_WARN(Graphics, "Earth buildings: Overture {} is not a gzip'd MVT PMTiles archive", release);
            break;
        }
        std::vector<uint8_t> rootBytes(header.begin() + long(_header.rootOffset), header.begin() + long(_header.rootOffset + _header.rootLength));
        // the root directory, from the header bytes
        std::vector<uint8_t> raw;
        if (!Gunzip(rootBytes, raw))
            break;
        Reader r{raw.data(), raw.data() + raw.size()};
        const uint64_t count = r.Varint();
        if (!r.ok || count > 1000000)
            break;
        _root.resize(size_t(count));
        uint64_t last = 0;
        for (auto& e : _root) { last += r.Varint(); e.tileId = last; }
        for (auto& e : _root) e.runLength = r.Varint();
        for (auto& e : _root) e.length = r.Varint();
        for (size_t i = 0; i < _root.size(); ++i)
        {
            const uint64_t v = r.Varint();
            _root[i].offset = (v == 0 && i > 0) ? _root[i - 1].offset + _root[i - 1].length : v - 1;
        }
        if (!r.ok)
            break;
        _header.valid = true;
        LOG_INFO(Graphics, "Earth buildings: Overture {} (zoom {}..{}, {} root entries)", release, _header.minZoom, _header.maxZoom,
                 _root.size());
        break;
    }
    _opened = _header.valid;
    return _header.valid;
}

bool EarthOverture::Directory(uint64_t offset, uint64_t length, std::vector<Entry>& entries, const std::function<bool()>& cancelled)
{
    if (length == 0)
        return false;
    if (auto it = _leaves.find(offset); it != _leaves.end())
    {
        entries = it->second;
        return true;
    }
    const auto path = _dir / "leaves" / (std::to_string(offset) + ".gz");
    std::vector<uint8_t> packed;
    if (!ReadFile(path, packed) || packed.size() != length)
    {
        long status = 0;
        if (!Range(_header.leafOffset + offset, length, packed, cancelled, status))
            return false;
        WriteFile(path, packed.data(), packed.size());
    }
    std::vector<uint8_t> raw;
    if (!Gunzip(packed, raw))
        return false;
    Reader r{raw.data(), raw.data() + raw.size()};
    const uint64_t count = r.Varint();
    if (!r.ok || count > 10000000)
        return false;
    entries.assign(size_t(count), {});
    uint64_t last = 0;
    for (auto& e : entries) { last += r.Varint(); e.tileId = last; }
    for (auto& e : entries) e.runLength = r.Varint();
    for (auto& e : entries) e.length = r.Varint();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const uint64_t v = r.Varint();
        entries[i].offset = (v == 0 && i > 0) ? entries[i - 1].offset + entries[i - 1].length : v - 1;
    }
    if (!r.ok)
        return false;
    if (_leaves.size() > 64)
        _leaves.clear();
    _leaves[offset] = entries;
    return true;
}

bool EarthOverture::Tile(int z, int x, int y, std::vector<uint8_t>& bytes, const std::function<bool()>& cancelled)
{
    bytes.clear();
    const auto path = _dir / std::to_string(z) / (std::to_string(x) + "_" + std::to_string(y) + ".mvt");
    std::error_code ec;
    if (std::filesystem::exists(path.string() + ".none", ec))
        return true;
    if (ReadFile(path, bytes) && !bytes.empty())
        return true;
    const uint64_t id = TileId(z, uint32_t(x), uint32_t(y));
    std::vector<Entry> current = _root;
    for (int depth = 0; depth < 4; ++depth)
    {
        auto it = std::upper_bound(current.begin(), current.end(), id, [](uint64_t v, const Entry& e) { return v < e.tileId; });
        if (it == current.begin())
            break;
        --it;
        if (it->runLength == 0) // a leaf directory
        {
            std::vector<Entry> leaf;
            if (!Directory(it->offset, it->length, leaf, cancelled))
                return false;
            current = std::move(leaf);
            continue;
        }
        if (id >= it->tileId + it->runLength)
            break; // not in the archive
        long status = 0;
        if (!Range(_header.dataOffset + it->offset, it->length, bytes, cancelled, status))
            return false;
        WriteFile(path, bytes.data(), bytes.size());
        return true;
    }
    WriteFile(path.string() + ".none", "0", 1); // no buildings there
    return true;
}

bool EarthOverture::EnsureCell(const std::filesystem::path& cache, int lat, int lon, const std::function<bool()>& cancelled)
{
    const auto path = cache / "buildings" / (std::to_string(lat) + "_" + std::to_string(lon) + ".json");
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec))
        return true;
    std::lock_guard lock(_mutex);
    if (!Open(cache, cancelled))
        return false;
    const double south = lat * CellDegrees, north = south + CellDegrees, west = lon * CellDegrees, east = west + CellDegrees;
    if (std::abs(south) > 84.0)
        return false;
    const double n = double(1 << TileZoom);
    const auto tileX = [&](double longitude) { return int(std::floor((longitude + 180.0) / 360.0 * n)); };
    const auto tileY = [&](double latitude)
    {
        const double r = latitude * Pi / 180.0;
        return int(std::floor((1.0 - std::asinh(std::tan(r)) / Pi) * 0.5 * n));
    };
    std::map<std::string, Building> buildings;
    for (int ty = tileY(north); ty <= tileY(south); ++ty)
        for (int tx = tileX(west); tx <= tileX(east - 1e-9); ++tx)
        {
            const int wx = ((tx % int(n)) + int(n)) % int(n);
            std::vector<uint8_t> packed, mvt;
            if (!Tile(TileZoom, wx, ty, packed, cancelled))
                return false; // the network: tried again on the next request
            if (packed.empty())
                continue;
            if (!Gunzip(packed, mvt))
                continue;
            DecodeTile(mvt, uint32_t(wx), uint32_t(ty), south, west, north, east, buildings);
        }
    std::vector<Building*> order;
    order.reserve(buildings.size());
    for (auto& [id, b] : buildings)
        order.push_back(&b);
    // the cell budget is EarthBuildingStream's: the largest first, so a city's landmarks are never the ones cut
    std::sort(order.begin(), order.end(), [](const Building* a, const Building* b) { return a->area > b->area; });
    if (order.size() > MaxCellBuildings)
        order.resize(MaxCellBuildings);
    std::string json = "{\"source\":\"overture " + _release + "\",\"elements\":[";
    char buffer[64];
    for (size_t i = 0; i < order.size(); ++i)
    {
        const Building& b = *order[i];
        json += i ? ",{" : "{";
        json += "\"type\":\"way\",\"id\":" + std::to_string(NumericId(b.id)) + ",\"tags\":{";
        for (size_t t = 0; t < b.tags.size(); ++t)
            json += (t ? ",\"" : "\"") + b.tags[t].first + "\":\"" + b.tags[t].second + "\"";
        json += "},\"geometry\":[";
        for (size_t k = 0; k < b.points.size(); ++k)
        {
            std::snprintf(buffer, sizeof(buffer), "%s{\"lat\":%.7f,\"lon\":%.7f}", k ? "," : "", b.points[k].first, b.points[k].second);
            json += buffer;
        }
        json += "]}";
    }
    json += "]}";
    WriteFile(path, json.data(), json.size());
    LOG_INFO(Graphics, "Earth buildings: Overture cell [{},{}] {} buildings ({} in its tiles)", lat, lon, order.size(), buildings.size());
    return true;
}

bool EarthOverture::EnsureNeighborhood(const std::filesystem::path& cache, int lat, int lon, const std::function<bool()>& cancelled,
                                       bool& changed)
{
    constexpr int offsets[9][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    changed = false;
    for (const auto& offset : offsets)
    {
        if (cancelled && cancelled())
            break;
        const int cellLat = lat + offset[0], cellLon = ((lon + offset[1] + 27000) % 18000) - 9000;
        std::error_code ec;
        const bool had = std::filesystem::is_regular_file(cache / "buildings" / (std::to_string(cellLat) + "_" + std::to_string(cellLon) + ".json"), ec);
        if (!EnsureCell(cache, cellLat, cellLon, cancelled))
            return false;
        changed = changed || !had;
    }
    return true;
}
} // namespace Poseidon
