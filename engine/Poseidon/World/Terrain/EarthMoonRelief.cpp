#include "EarthMoonRelief.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Poseidon::EarthStreaming
{
namespace
{
struct Relief
{
    int width = 0, height = 0;
    float lo = 0, hi = 0;
    std::vector<uint16_t> level0;
};

std::filesystem::path ExeDirectory()
{
#ifdef _WIN32
    wchar_t path[32768]{};
    if (GetModuleFileNameW(nullptr, path, 32768))
        return std::filesystem::path(path).parent_path();
    return {};
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

// Level 0 of the file the renderer also loads (sky/moonmap.rs); the format is in make_moon_relief.py.
const Relief& Load()
{
    static Relief relief;
    static std::once_flag once;
    std::call_once(once, [] {
        std::ifstream in(ExeDirectory() / "assets" / "earth" / "moon" / "moon_height.pmoh", std::ios::binary);
        if (!in)
            return;
        char magic[4]{};
        uint32_t header[4]{};
        float range[2]{};
        in.read(magic, 4);
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        in.read(reinterpret_cast<char*>(range), sizeof(range));
        if (!in || std::memcmp(magic, "PMOH", 4) != 0 || header[0] != 1 || header[1] == 0 || header[2] == 0 ||
            header[1] > 16384 || header[2] > 8192 || !(range[1] > range[0]))
            return;
        std::vector<uint16_t> texels(size_t(header[1]) * header[2]);
        in.read(reinterpret_cast<char*>(texels.data()), std::streamsize(texels.size() * 2));
        if (!in)
            return;
        relief.width = int(header[1]);
        relief.height = int(header[2]);
        relief.lo = range[0];
        relief.hi = range[1];
        relief.level0 = std::move(texels);
    });
    return relief;
}
} // namespace

double MoonReliefHeight(double latitude, double longitude)
{
    const Relief& r = Load();
    if (r.level0.empty() || !std::isfinite(latitude) || !std::isfinite(longitude))
        return 0.0;
    // Texel centres, as the shader's bilinear sample: longitude wraps, latitude clamps.
    const double u = (longitude / 360.0 + 0.5) * r.width - 0.5;
    const double v = (0.5 - latitude / 180.0) * r.height - 0.5;
    const double fu = std::floor(u), fv = std::floor(v);
    const double au = u - fu, av = v - fv;
    const auto at = [&](long x, long y) {
        x = ((x % r.width) + r.width) % r.width;
        y = std::clamp<long>(y, 0, r.height - 1);
        return double(r.level0[size_t(y) * r.width + x]);
    };
    const long x0 = long(fu), y0 = long(fv);
    const double q = (at(x0, y0) * (1 - au) + at(x0 + 1, y0) * au) * (1 - av) +
                     (at(x0, y0 + 1) * (1 - au) + at(x0 + 1, y0 + 1) * au) * av;
    return r.lo + (r.hi - r.lo) * q / 65535.0;
}
} // namespace Poseidon::EarthStreaming
