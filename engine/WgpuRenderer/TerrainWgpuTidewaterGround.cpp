// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainWgpuTidewaterGround.hpp"
#include "TextureWgpu.hpp"
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace Poseidon
{
namespace
{
std::vector<uint8_t> ReadSource(const char* path)
{
    std::vector<uint8_t> bytes;
    QIFStreamB file;
    file.AutoOpen(path);
    if (file.fail() || file.rest() <= 0)
        return bytes;
    bytes.resize(static_cast<size_t>(file.rest()));
    file.read(bytes.data(), static_cast<int>(bytes.size()));
    return bytes;
}

bool Contains(const char* s, const char* sub)
{
    std::string a = s ? s : "", b = sub;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return a.find(b) != std::string::npos;
}

DecodedImage DecodeLayer(const Texture* layer)
{
    const auto* t = static_cast<const TextureWgpu*>(layer);
    const char* source = t ? t->SourceName() : nullptr;
    if (!source)
        return {};
    const auto bytes = ReadSource(source);
    if (bytes.empty())
        return {};
    return DecodePAABuffer(bytes.data(), bytes.size(), !Contains(source, ".pac"));
}

// the corner surfaces of a layer: a pure image is its own code, a transition its four 2-letter corners
std::vector<std::string> Corners(const std::string& base)
{
    if (base.size() == 8)
        return {base.substr(0, 2), base.substr(2, 2), base.substr(4, 2), base.substr(6, 2)};
    return {base};
}

float Smooth(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// periodic value noise over the image (it tiles like the layer it is baked into), 0..1
float Hash(int x, int y, int seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffff) / float(0xffffff);
}
float ValueNoise(float u, float v, int period, int seed)
{
    const float x = u * period, y = v * period;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    auto at = [&](int i, int j) { return Hash(((i % period) + period) % period, ((j % period) + period) % period, seed); };
    const float a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}
float Fbm(float u, float v, int period, int seed)
{
    return ValueNoise(u, v, period, seed) * 0.55f + ValueNoise(u, v, period * 2, seed + 7) * 0.3f +
           ValueNoise(u, v, period * 4, seed + 13) * 0.15f;
}

using Rgb = std::array<float, 3>;
Rgb Mix(const Rgb& a, const Rgb& b, float t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }
float Lum(const Rgb& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
} // namespace

bool TidewaterGround::Enabled()
{
    static const bool on = []
    {
        // opt-in (WGR_TW_GROUND=1) until the owner has seen it: the pure sand layer is not found yet
        const char* v = std::getenv("WGR_TW_GROUND");
        return v && v[0] == '1';
    }();
    return on;
}

std::string TidewaterGround::BaseName(const char* path)
{
    std::string s = path ? path : "";
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos)
        s = s.substr(slash + 1);
    const size_t dot = s.find('.');
    if (dot != std::string::npos)
        s = s.substr(0, dot);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool TidewaterGround::Applies(const std::string& base)
{
    if (base == "pol")
        return true;
    for (const std::string& c : Corners(base))
        if (c == "ps")
            return true;
    return false;
}

void TidewaterGround::Prepare(const Landscape& land)
{
    const std::string world = land.GetName() ? land.GetName() : "";
    if (world == _world && !_means.empty())
        return;
    _world = world;
    _means.clear();
    const auto measure = [&](const std::string& base, const DecodedImage& image)
    {
        if (!image.valid())
            return;
        double sum[3] = {};
        const size_t n = size_t(image.width) * image.height;
        for (size_t p = 0; p < n; ++p)
            for (int c = 0; c < 3; ++c)
                sum[c] += image.rgba[p * 4 + c];
        _means[base] = {float(sum[0] / n / 255.0), float(sum[1] / n / 255.0), float(sum[2] / n / 255.0)};
    };
    // corner surfaces the transitions name, and the folder they live in
    std::map<std::string, std::string> cornerDirs;
    for (int i = 0; i < land.GetNTextures(); ++i)
    {
        Texture* t = land.GetTexture(i);
        if (!t)
            continue;
        const char* source = static_cast<const TextureWgpu*>(t)->SourceName();
        const std::string base = BaseName(source);
        if (base.size() == 8 && source)
        {
            const std::string path = source;
            const size_t slash = path.find_last_of("\\/");
            const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
            for (const std::string& c : Corners(base))
                cornerDirs.emplace(c, dir);
        }
        if (base.size() > 3 || _means.count(base))
            continue; // the pure surfaces only ("ps", "sz", "pol", ...)
        measure(base, DecodeLayer(t));
    }
    // A world need not place a pure surface on any cell (Eden never lays plain "ps": its beaches are
    // all transitions), yet the pure image ships beside the transitions; read it from there.
    for (const auto& [code, dir] : cornerDirs)
    {
        if (_means.count(code))
            continue;
        for (const char* ext : {".paa", ".pac"})
        {
            const std::string path = dir + code + ext;
            const auto bytes = ReadSource(path.c_str());
            if (bytes.empty())
                continue;
            measure(code, DecodePAABuffer(bytes.data(), bytes.size(), std::strcmp(ext, ".pac") != 0));
            break;
        }
    }
    LOG_INFO(Graphics, "Tidewater ground: {} pure OFP surfaces measured on '{}' (sand {}, field {})", _means.size(), world,
             _means.count("ps") ? "yes" : "no", _means.count("pol") ? "yes" : "no");
}

bool TidewaterGround::Recolour(const std::string& base, DecodedImage& image) const
{
    if (!Enabled() || !image.valid() || !Applies(base))
        return false;
    const bool field = base == "pol";
    const std::vector<std::string> corners = Corners(base);
    std::set<std::string> codes(corners.begin(), corners.end());
    const auto mean = [&](const std::string& code) -> const Rgb*
    {
        const auto it = _means.find(code);
        return it == _means.end() ? nullptr : &it->second;
    };
    const Rgb* own = mean(field ? "pol" : "ps");
    if (!own)
        return false;
    const float ownLum = std::max(Lum(*own), 0.02f);
    // Terrain.js: sand = mix(pale, paler, macro); drifts of warmer, coarser sand; soil = trodden bare
    // earth with sandy and dark humus patches. sRGB 0..1, as Tidewater's S() constants.
    const Rgb sandA{0.83f, 0.75f, 0.6f}, sandB{0.9f, 0.84f, 0.72f}, sandDrift{0.84f, 0.72f, 0.55f};
    const Rgb soilA{0.40f, 0.33f, 0.23f}, soilSandy{0.60f, 0.52f, 0.38f}, soilDark{0.17f, 0.13f, 0.08f};
    const int w = image.width, h = image.height;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint8_t* px = &image.rgba[(size_t(y) * w + x) * 4];
            const Rgb c{px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
            // this pixel's share of the recoloured surface: 1 on a pure layer; on a transition, the
            // corner surfaces' likelihoods by colour distance to their pure images
            float share = 1;
            if (!field && codes.size() > 1)
            {
                float total = 0, mine = 0;
                for (const std::string& code : codes)
                {
                    const Rgb* m = mean(code);
                    if (!m)
                        continue;
                    const float d = (c[0] - (*m)[0]) * (c[0] - (*m)[0]) + (c[1] - (*m)[1]) * (c[1] - (*m)[1]) +
                                    (c[2] - (*m)[2]) * (c[2] - (*m)[2]);
                    const float l = std::exp(-d / (2 * 0.06f * 0.06f));
                    total += l;
                    if (code == "ps")
                        mine += l;
                }
                share = total > 1e-6f ? mine / total : 0.0f;
            }
            if (share <= 0.002f)
                continue;
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            const float m1 = Fbm(u, v, 4, field ? 31 : 11), m2 = Fbm(u + 0.37f, v + 0.61f, 3, field ? 47 : 23);
            Rgb tw;
            if (field)
            {
                tw = Mix(soilA, soilSandy, Smooth(0.45f, 0.72f, m1) * 0.55f);
                tw = Mix(tw, soilDark, Smooth(0.6f, 0.85f, m2) * 0.4f);
            }
            else
            {
                tw = Mix(sandA, sandB, Smooth(0.3f, 0.72f, m1));
                tw = Mix(tw, sandDrift, Smooth(0.55f, 0.8f, m2) * 0.45f);
            }
            // keep OFP's own detail (grain, furrows, footprints of the old image) as a luminance ratio
            const float ratio = std::clamp(Lum(c) / ownLum, 0.72f, 1.28f);
            const Rgb out = Mix(c, Rgb{tw[0] * ratio, tw[1] * ratio, tw[2] * ratio}, share);
            for (int k = 0; k < 3; ++k)
                px[k] = uint8_t(std::clamp(out[k] * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    return true;
}

Ref<Texture> TidewaterGround::Bake(Texture* layer, AbstractTextBank* bank) const
{
    if (!layer || !bank || !Enabled())
        return nullptr;
    const std::string base = BaseName(static_cast<const TextureWgpu*>(layer)->SourceName());
    if (!Applies(base))
        return nullptr;
    DecodedImage image = DecodeLayer(layer);
    if (!Recolour(base, image))
        return nullptr;
    return bank->CreateDynamic(image.width, image.height, image.rgba.data(), uint32_t(image.rgba.size()), true);
}
} // namespace Poseidon
