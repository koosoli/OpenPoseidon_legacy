#pragma once

#include <Poseidon/World/Terrain/EarthFrame.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Poseidon::EarthTrees
{
struct Vertex
{
    float x, y, z, nx, ny, nz;
    float u = .5f, v = .5f;
};
constexpr size_t TriangleCount = 28;
constexpr size_t VertexCount = TriangleCount * 3;
constexpr int PaletteWidth = 16;
constexpr auto PalettePixels = []
{
    constexpr uint8_t colors[4][4] = {{83, 61, 40, 255}, {43, 67, 31, 255}, {53, 77, 35, 255}, {63, 86, 40, 255}};
    std::array<uint8_t, PaletteWidth * 4> pixels{};
    for (int x = 0; x < PaletteWidth; ++x)
        for (int channel = 0; channel < 4; ++channel)
            pixels[x * 4 + channel] = colors[x / 4][channel];
    return pixels;
}();

inline uint64_t Mix(uint64_t value)
{
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}
inline uint64_t GeographicSeed(double latitude, double longitude)
{
    // Sub-metre geographic quantization makes appearance independent of the
    // rolling patch, camera and ENU origin; it is synthetic, not species data.
    const auto lat = uint64_t(std::llround((latitude + 90.0) * 1000000.0));
    const auto lon = uint64_t(std::llround((Earth::WrapLongitude(longitude) + 180.0) * 1000000.0));
    return Mix((lat << 32) | lon);
}

// One grounded trunk and a closed, two-ring broadleaf crown. Expanded vertices
// retain the existing single-batch mesh path; no models, alpha or wind assets.
inline std::array<Vertex, VertexCount> Build(double x, float ground, double z, double height, uint64_t seed,
                                             bool varyHeight = false)
{
    const auto unit = [seed](uint64_t lane) { return double(Mix(seed + lane) >> 40) / 16777216.0; };
    const double h = height * (varyHeight ? .82 + .36 * unit(1) : 1.0);
    const double rx = h * (.22 + .045 * unit(2)), rz = rx * (.88 + .24 * unit(3));
    const double angle = 2 * Earth::Pi * unit(4), leanX = h * (.025 * unit(5) - .0125),
                 leanZ = h * (.025 * unit(6) - .0125);
    const float leafU = .375f + .25f * float(Mix(seed + 7) % 3);
    const auto crown = [&](double dx, double dy, double dz)
    {
        const double nx = (dx - leanX * dy / h) / (rx * rx), ny = (dy - .65 * h) / (.35 * h * .35 * h),
                     nz = (dz - leanZ * dy / h) / (rz * rz);
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        return Vertex{float(x + dx),      ground + float(dy), float(z + dz), float(nx / length),
                      float(ny / length), float(nz / length), leafU,         .5f};
    };
    std::array<Vertex, VertexCount> vertices{};
    size_t next = 0;
    const auto triangle = [&](Vertex a, Vertex b, Vertex c, bool flat)
    {
        if (flat)
        {
            const double ux = double(b.x) - a.x, uy = double(b.y) - a.y, uz = double(b.z) - a.z, vx = double(c.x) - a.x,
                         vy = double(c.y) - a.y, vz = double(c.z) - a.z;
            const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx,
                         length = std::sqrt(nx * nx + ny * ny + nz * nz);
            a.nx = b.nx = c.nx = float(nx / length);
            a.ny = b.ny = c.ny = float(ny / length);
            a.nz = b.nz = c.nz = float(nz / length);
        }
        vertices[next++] = a;
        vertices[next++] = b;
        vertices[next++] = c;
    };
    std::array<Vertex, 4> base{}, trunkTop{};
    const double trunkRadius = std::clamp(.018 * h, .08, .6);
    for (int i = 0; i < 4; ++i)
    {
        const double theta = angle + i * Earth::Pi / 2;
        const double dx = std::sin(theta) * trunkRadius, dz = std::cos(theta) * trunkRadius;
        base[i] = {float(x + dx), ground, float(z + dz), 0, 0, 0, .125f, .5f};
        trunkTop[i] = {float(x + dx * .6 + leanX * .45),
                       ground + float(h * .45),
                       float(z + dz * .6 + leanZ * .45),
                       0,
                       0,
                       0,
                       .125f,
                       .5f};
    }
    for (int i = 0; i < 4; ++i)
    {
        const int j = (i + 1) % 4;
        triangle(base[i], base[j], trunkTop[i], true);
        triangle(base[j], trunkTop[j], trunkTop[i], true);
    }
    std::array<Vertex, 5> lower{}, upper{};
    for (int i = 0; i < 5; ++i)
    {
        const double theta = angle + i * 2 * Earth::Pi / 5;
        const double asymmetry = .95 + .1 * unit(8 + i);
        lower[i] = crown(std::sin(theta) * rx * asymmetry + leanX * .53, .53 * h,
                         std::cos(theta) * rz * asymmetry + leanZ * .53);
        upper[i] = crown(std::sin(theta) * rx * .78 * asymmetry + leanX * .77, .77 * h,
                         std::cos(theta) * rz * .78 * asymmetry + leanZ * .77);
    }
    const auto bottom = crown(leanX * .30, h * .30, leanZ * .30), top = crown(leanX, h, leanZ);
    for (int i = 0; i < 5; ++i)
    {
        const int j = (i + 1) % 5;
        triangle(bottom, lower[j], lower[i], false);
        triangle(lower[i], lower[j], upper[i], false);
        triangle(lower[j], upper[j], upper[i], false);
        triangle(top, upper[i], upper[j], false);
    }
    return vertices;
}
} // namespace Poseidon::EarthTrees
