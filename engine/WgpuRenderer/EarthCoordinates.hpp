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
constexpr int MaxZoom = 12;
constexpr int TilePixels = 256;
constexpr int GridSamples = 257;
constexpr float GridMetres = 100.0f;
constexpr float RecenterMetres = 3200.0f;
constexpr float Anchor = 6400.0f;
constexpr float HalfWindow = 128 * GridMetres;
constexpr int HorizonSamples = 257;
constexpr int HorizonInnerBegin = 96;
constexpr int HorizonInnerEnd = 160;

enum class NearDetail { Coarse, Fine };
// Hysteresis keeps small altitude changes from rebuilding/downloading a patch.
// Both bands retain 257 samples: detail spends coverage, not more vertices.
inline NearDetail SelectNearDetail(float altitude, NearDetail current)
{
    if (!std::isfinite(altitude)) return current;
    if (altitude <= 8000.0f) return NearDetail::Fine;
    if (altitude >= 12000.0f) return NearDetail::Coarse;
    return current;
}
constexpr float NearGrid(NearDetail detail) { return detail == NearDetail::Fine ? 25.0f : GridMetres; }
constexpr int NearZoom(NearDetail detail) { return detail == NearDetail::Fine ? MaxZoom : Zoom; }
constexpr float NearHalfWindow(float grid) { return 128.0f * grid; }
constexpr float NearRecenter(float grid) { return 32.0f * grid; }
constexpr float NearBlendWidth(float grid) { return 40.0f * grid; }

// Stable spherical sagitta, in the fixed origin's tangent frame. The rational
// form avoids subtracting two Earth-sized numbers near the origin. This is a
// presentation projection, not a change to the finite simulation landscape.
inline double CurvatureDrop(double x, double z)
{
    const double dx = x - Anchor, dz = z - Anchor;
    const double r2 = dx * dx + dz * dz;
    return r2 / (Radius + std::sqrt(std::max(0.0, Radius * Radius - r2)));
}

inline float HorizonRadius(float altitude)
{
    if (!std::isfinite(altitude)) return 64000.0f;
    const double h = std::clamp(double(altitude), 0.0, 250000.0);
    const double reach = 1.25 * std::sqrt(h * (2.0 * Radius + h));
    float radius = 64000.0f;
    while (radius < reach && radius < 2048000.0f) radius *= 2.0f;
    return radius;
}

// A dense inner boundary exactly matches the near window. Quadratically spaced
// outer samples cover the horizon with a fixed vertex/triangle budget.
inline float HorizonOffset(int i, float radius, float halfWindow = HalfWindow)
{
    if (i >= HorizonInnerBegin && i <= HorizonInnerEnd)
        return (i - 128) * (halfWindow / 32.0f);
    const bool left = i < HorizonInnerBegin;
    const float t = float(left ? HorizonInnerBegin - i : i - HorizonInnerEnd) / 96.0f;
    const float distance = halfWindow + (radius - halfWindow) * t * t;
    return left ? -distance : distance;
}

inline int HorizonZoom(double latitude, float radius)
{
    // Roughly four source tiles span the whole far window. The inner seam uses
    // the selected near source separately; distant terrain never needs zoom 12.
    const double circumference = 2.0 * Pi * Radius * std::cos(latitude * Pi / 180.0);
    return std::clamp(int(std::floor(std::log2(circumference * 2.0 / radius))), 3, Zoom);
}

struct Coordinate { double latitude, longitude; };
struct Pixel { double x, y; };

struct Direction { double x, y, z; };
// ECEF unit sphere expressed in the fixed origin's east/up/north basis.
// Unlike the streaming window's local map this covers the poles and far side.
inline Direction GlobeNormal(double originLat, double originLon, double lat, double lon)
{
    const double p0=originLat*Pi/180.0;
    const double p=lat*Pi/180.0, dl=(lon-originLon)*Pi/180.0;
    return {std::cos(p)*std::sin(dl),
        std::sin(p0)*std::sin(p)+std::cos(p0)*std::cos(p)*std::cos(dl),
        std::cos(p0)*std::sin(p)-std::sin(p0)*std::cos(p)*std::cos(dl)};
}

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

inline Pixel ToPixel(Coordinate c, int zoom = Zoom)
{
    if (!std::isfinite(c.latitude) || !std::isfinite(c.longitude) || std::abs(c.latitude) > MaxLatitude)
        throw std::out_of_range("Terrain Tiles polar coverage limit (85.051 degrees)");
    const double size = double(TilePixels * (1 << zoom));
    const double lat = c.latitude * Pi / 180.0;
    return {(WrapLongitude(c.longitude) + 180.0) / 360.0 * size,
        std::clamp((1.0 - std::asinh(std::tan(lat)) / Pi) * 0.5 * size, 0.0, size - 1.0)};
}

inline int WrapPixel(int pixel, int zoom = Zoom)
{
    const int size = TilePixels * (1 << zoom);
    return (pixel % size + size) % size;
}

inline float Terrarium(unsigned char r, unsigned char g, unsigned char b)
{
    return float(int(r) * 256 + int(g) - 32768) + float(b) / 256.0f;
}

inline int CenterCell(float coordinate, float stride = RecenterMetres) { return int(std::floor(coordinate / stride)); }
}
