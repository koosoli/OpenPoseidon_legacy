// SPDX-License-Identifier: GPL-3.0-or-later
#include "EarthLandCover.hpp"
#include "EarthImageryTiles.hpp" // EarthDownloadShare

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <curl/curl.h>
#include <stb_image.h> // stbi_zlib_decode_buffer (implemented in Poseidon, Graphics/Textures/Image.cpp)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace Poseidon
{
namespace
{
constexpr size_t MaxBlocks = 48; // decoded 1 MB blocks kept in memory
constexpr uint64_t HeaderBytes = 64 * 1024;

size_t Receive(char* data, size_t size, size_t count, void* destination)
{
    auto& bytes = *static_cast<std::vector<uint8_t>*>(destination);
    const size_t n = size * count;
    if (bytes.size() + n > 8u * 1024 * 1024)
        return 0;
    bytes.insert(bytes.end(), data, data + n);
    return n;
}
int Progress(void* cancelled, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto& f = *static_cast<const std::function<bool()>*>(cancelled);
    return f && f() ? 1 : 0;
}
bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(input), {});
    return !bytes.empty();
}
void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temporary = path.string() + ".part";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec)
        std::filesystem::remove(temporary, ec);
}
std::string CellName(int lat0, int lon0)
{
    char name[96];
    std::snprintf(name, sizeof(name), "ESA_WorldCover_10m_2021_v200_%c%02d%c%03d_Map", lat0 < 0 ? 'S' : 'N', std::abs(lat0),
                  lon0 < 0 ? 'W' : 'E', std::abs(lon0));
    return name;
}

// The first IFD of a classic little-endian TIFF: what a tiled 8-bit deflate WorldCover file needs. False when the
// header bytes do not hold it all (the caller fetches more) or the file is not that layout.
struct Tiff
{
    int width = 0, height = 0, tileW = 0, tileH = 0, compression = 0, bits = 0, predictor = 1;
    double scale = 0, west = 0, north = 0;
    std::vector<uint64_t> offsets, sizes;
};
bool ParseTiff(const std::vector<uint8_t>& b, Tiff& t)
{
    const auto u16 = [&](uint64_t o) { return o + 2 <= b.size() ? uint32_t(b[o] | b[o + 1] << 8) : 0u; };
    const auto u32 = [&](uint64_t o) { return o + 4 <= b.size() ? uint32_t(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24) : 0u; };
    const auto f64 = [&](uint64_t o)
    {
        double d = 0;
        if (o + 8 <= b.size())
            std::memcpy(&d, &b[o], 8);
        return d;
    };
    if (b.size() < 8 || b[0] != 'I' || b[1] != 'I' || u16(2) != 42)
        return false;
    const uint64_t ifd = u32(4);
    const uint32_t count = u16(ifd);
    if (ifd + 2 + uint64_t(count) * 12 > b.size())
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t e = ifd + 2 + uint64_t(i) * 12;
        const uint32_t tag = u16(e), type = u16(e + 2), n = u32(e + 4);
        const uint32_t size = type == 3 ? 2 : type == 4 ? 4 : type == 12 ? 8 : 1;
        const uint64_t at = uint64_t(n) * size <= 4 ? e + 8 : u32(e + 8);
        if (at + uint64_t(n) * size > b.size())
            return false;
        const auto value = [&](uint32_t k) -> uint64_t { return type == 3 ? u16(at + k * 2) : u32(at + k * 4); };
        switch (tag)
        {
        case 256: t.width = int(value(0)); break;
        case 257: t.height = int(value(0)); break;
        case 258: t.bits = int(value(0)); break;
        case 259: t.compression = int(value(0)); break;
        case 317: t.predictor = int(value(0)); break;
        case 322: t.tileW = int(value(0)); break;
        case 323: t.tileH = int(value(0)); break;
        case 324: t.offsets.resize(n); for (uint32_t k = 0; k < n; ++k) t.offsets[k] = value(k); break;
        case 325: t.sizes.resize(n); for (uint32_t k = 0; k < n; ++k) t.sizes[k] = value(k); break;
        case 33550: t.scale = f64(at); break;
        case 33922: t.west = f64(at + 24); t.north = f64(at + 32); break;
        default: break;
        }
    }
    return t.width > 0 && t.tileW > 0 && t.tileH > 0 && t.bits == 8 && t.compression == 8 && t.predictor == 1 && t.scale > 0 &&
           !t.offsets.empty() && t.offsets.size() == t.sizes.size();
}
} // namespace

EarthLandCover* EarthLandCover::Get()
{
    static std::unique_ptr<EarthLandCover> instance = []() -> std::unique_ptr<EarthLandCover>
    {
        const char* off = std::getenv("POSEIDON_EARTH_LANDCOVER");
        const char* cache = std::getenv("POSEIDON_EARTH_CACHE");
        if ((off && off[0] == '0') || !cache || !*cache)
            return nullptr;
        return std::make_unique<EarthLandCover>(std::filesystem::u8path(cache));
    }();
    return instance.get();
}

EarthLandCover::EarthLandCover(std::filesystem::path cache) : _dir(std::move(cache) / "landcover") {}

bool EarthLandCover::Fetch(const std::string& url, uint64_t first, uint64_t last, std::vector<uint8_t>& bytes, long& status,
                           const std::function<bool()>& cancelled)
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        bytes.clear();
        status = 0;
        CURL* curl = curl_easy_init();
        if (!curl)
            return false;
        const std::string range = std::to_string(first) + "-" + std::to_string(last);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenPoseidon-Earth/1");
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
        if (result == CURLE_OK && (status == 206 || status == 200))
            return true;
        if (status == 403 || status == 404 || (cancelled && cancelled()))
            return false;
    }
    return false;
}

EarthLandCover::File& EarthLandCover::Open(int lat0, int lon0, const std::function<bool()>& cancelled)
{
    File& file = _files[{lat0, lon0}];
    if (file.present || (!file.url.empty() && (!file.failed || std::chrono::steady_clock::now() < file.retryAt)))
        return file;
    const std::string name = CellName(lat0, lon0);
    file.url = "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/" + name + ".tif";
    file.failed = false;
    std::error_code ec;
    if (std::filesystem::exists(_dir / (name + ".none"), ec))
        return file; // no land in this cell
    std::vector<uint8_t> header;
    Tiff tiff;
    if (!ReadFile(_dir / (name + ".header"), header) || !ParseTiff(header, tiff))
    {
        long status = 0;
        if (!Fetch(file.url, 0, HeaderBytes - 1, header, status, cancelled))
        {
            if (status == 403 || status == 404) // S3 answers 403 for a missing key in a bucket that lists
                WriteFile(_dir / (name + ".none"), {uint8_t('0')});
            else
            {
                file.failed = true;
                file.retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            }
            return file;
        }
        if (!ParseTiff(header, tiff))
        {
            LOG_WARN(Graphics, "Earth land cover: {} is not the expected tiled 8-bit deflate GeoTIFF", name);
            return file;
        }
        WriteFile(_dir / (name + ".header"), header);
    }
    file.present = true;
    file.width = tiff.width;
    file.height = tiff.height;
    file.tileW = tiff.tileW;
    file.tileH = tiff.tileH;
    file.across = (tiff.width + tiff.tileW - 1) / tiff.tileW;
    file.west = tiff.west;
    file.north = tiff.north;
    file.step = tiff.scale;
    file.offsets = std::move(tiff.offsets);
    file.sizes = std::move(tiff.sizes);
    return file;
}

bool EarthLandCover::Load(File& file, int lat0, int lon0, int tile, const std::function<bool()>& cancelled)
{
    if (tile < 0 || size_t(tile) >= file.offsets.size())
        return false;
    const auto path = _dir / CellName(lat0, lon0) / (std::to_string(tile) + ".z");
    std::vector<uint8_t> packed;
    if (!ReadFile(path, packed) || packed.size() != file.sizes[size_t(tile)])
    {
        if (file.sizes[size_t(tile)] == 0)
            return false;
        long status = 0;
        if (!Fetch(file.url, file.offsets[size_t(tile)], file.offsets[size_t(tile)] + file.sizes[size_t(tile)] - 1, packed, status,
                   cancelled) ||
            packed.size() != file.sizes[size_t(tile)])
            return false;
        WriteFile(path, packed);
    }
    Block block(size_t(file.tileW) * file.tileH);
    const int n = stbi_zlib_decode_buffer(reinterpret_cast<char*>(block.data()), int(block.size()),
                                          reinterpret_cast<const char*>(packed.data()), int(packed.size()));
    if (n != int(block.size()))
        return false;
    if (_blocks.size() >= MaxBlocks)
        _blocks.erase(std::min_element(_blocks.begin(), _blocks.end(),
                                       [](const auto& a, const auto& b) { return a.second.second < b.second.second; }));
    _blocks[{lat0, lon0, tile}] = {std::move(block), ++_use};
    return true;
}

EarthLandCover::Class EarthLandCover::At(double latitude, double longitude, const std::function<bool()>& cancelled)
{
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude >= 84.0 || latitude < -60.0)
        return Unknown; // WorldCover spans 84 N .. 60 S
    longitude = std::fmod(longitude + 540.0, 360.0) - 180.0;
    const int lat0 = int(std::floor(latitude / 3.0)) * 3, lon0 = int(std::floor(longitude / 3.0)) * 3;
    std::lock_guard lock(_mutex);
    File& file = Open(lat0, lon0, cancelled);
    if (!file.present)
        return file.failed ? Unknown : Water; // no file: sea (or a cell with no land)
    const int px = std::clamp(int((longitude - file.west) / file.step), 0, file.width - 1);
    const int py = std::clamp(int((file.north - latitude) / file.step), 0, file.height - 1);
    const int tile = (py / file.tileH) * file.across + px / file.tileW;
    const auto key = std::make_tuple(lat0, lon0, tile);
    auto it = _blocks.find(key);
    if (it == _blocks.end())
    {
        if (auto f = _failedBlocks.find(key); f != _failedBlocks.end() && f->second >= 3)
            return Unknown;
        if (!Load(file, lat0, lon0, tile, cancelled))
        {
            ++_failedBlocks[key];
            return Unknown;
        }
        it = _blocks.find(key);
    }
    it->second.second = ++_use;
    const uint8_t c = it->second.first[size_t(py % file.tileH) * file.tileW + px % file.tileW];
    return c == 0 ? Water : Class(c); // 0 is no-data: outside the land mask, i.e. the sea
}
} // namespace Poseidon
