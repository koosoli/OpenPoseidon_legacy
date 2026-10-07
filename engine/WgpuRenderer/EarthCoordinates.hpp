#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Poseidon::Earth
{
constexpr double Pi = 3.14159265358979323846;
constexpr double Radius = 6378137.0;
constexpr double MaxLatitude = 85.0511287798066;
constexpr int Zoom = 10;
constexpr int TilePixels = 256;
constexpr int GridSamples = 257;
constexpr float GridMetres = 100.0f;
constexpr float RecenterMetres = 3200.0f;
constexpr float Anchor = 6400.0f;

struct Coordinate { double latitude, longitude; };
struct Pixel { double x, y; };

inline double WrapLongitude(double degrees)
{
    return degrees - 360.0 * std::floor((degrees + 180.0) / 360.0);
}

// A fixed geographic origin, independent of the moving render window. Double
// precision prevents texture/tile jitter when a window changes. This prototype
// uses a local tangent map; it does not claim globe curvature or polar coverage.
inline Coordinate FromLocal(double latitude, double longitude, double x, double z)
{
    const double lat = latitude + (z - Anchor) / Radius * 180.0 / Pi;
    return {lat, WrapLongitude(longitude + (x - Anchor) /
        (Radius * std::cos(latitude * Pi / 180.0)) * 180.0 / Pi)};
}

inline Pixel ToPixel(Coordinate c)
{
    if (!std::isfinite(c.latitude) || !std::isfinite(c.longitude) || std::abs(c.latitude) > MaxLatitude)
        throw std::out_of_range("Terrain Tiles polar coverage limit (85.051 degrees)");
    const double size = double(TilePixels * (1 << Zoom));
    const double lat = c.latitude * Pi / 180.0;
    return {(WrapLongitude(c.longitude) + 180.0) / 360.0 * size,
        std::clamp((1.0 - std::asinh(std::tan(lat)) / Pi) * 0.5 * size, 0.0, size - 1.0)};
}

inline int WrapPixel(int pixel)
{
    constexpr int size = TilePixels * (1 << Zoom);
    return (pixel % size + size) % size;
}

inline float Terrarium(unsigned char r, unsigned char g, unsigned char b)
{
    return float(int(r) * 256 + int(g) - 32768) + float(b) / 256.0f;
}

inline int CenterCell(float coordinate) { return int(std::floor(coordinate / RecenterMetres)); }
}
