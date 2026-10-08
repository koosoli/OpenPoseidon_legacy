#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace Poseidon::Earth
{
constexpr double Pi = 3.14159265358979323846;
constexpr double Radius = 6378137.0;
constexpr float Anchor = 6400.0f;
constexpr double FreeFlightRecenter = 32000.0;

struct Coordinate { double latitude, longitude; };
struct Direction { double x, y, z; };
struct LocalPoint { double x, z; bool visible; };

inline double WrapLongitude(double degrees)
{
    return degrees - 360.0 * std::floor((degrees + 180.0) / 360.0);
}

// ECEF unit sphere expressed in an origin's east/up/north basis. This is the
// same orthographic projection used by the curved near/far terrain and globe.
inline Direction GlobeNormal(double originLat, double originLon, double lat, double lon)
{
    const double p0 = originLat * Pi / 180.0;
    const double p = lat * Pi / 180.0, dl = (lon - originLon) * Pi / 180.0;
    return {std::cos(p) * std::sin(dl),
        std::sin(p0) * std::sin(p) + std::cos(p0) * std::cos(p) * std::cos(dl),
        std::cos(p0) * std::sin(p) - std::sin(p0) * std::cos(p) * std::cos(dl)};
}

inline LocalPoint ToLocal(double originLat, double originLon, double lat, double lon)
{
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat) > 90.0)
        return {0, 0, false};
    const auto n = GlobeNormal(originLat, originLon, lat, lon);
    return {Anchor + Radius * n.x, Anchor + Radius * n.z, n.y >= 0.0};
}

// Convert any nonzero direction in local ENU into its spherical footpoint.
// Camera positions include Radius in Y; terrain positions use the curved datum.
inline Coordinate FromDirection(double originLat, double originLon, Direction n)
{
    const double p0 = originLat * Pi / 180.0;
    const double north = std::sin(p0) * n.y + std::cos(p0) * n.z;
    const double equator = std::cos(p0) * n.y - std::sin(p0) * n.z;
    return {std::atan2(north, std::hypot(n.x, equator)) * 180.0 / Pi,
        WrapLongitude(originLon + std::atan2(n.x, equator) * 180.0 / Pi)};
}

inline Coordinate FromLocal(double latitude, double longitude, double x, double z)
{
    const double dx = x - Anchor, dz = z - Anchor;
    const double r2 = dx * dx + dz * dz;
    if (!std::isfinite(r2) || r2 >= Radius * Radius)
        throw std::out_of_range("Earth local coordinates outside the front hemisphere");
    return FromDirection(latitude, longitude, {dx, std::sqrt(Radius * Radius - r2), dz});
}

// Rotate a vector, never translate it. The columns are the new ENU axes in the
// old frame, so this preserves ECEF view/up/velocity across an origin shift.
inline Direction RotateFrame(Coordinate oldOrigin, Coordinate newOrigin, Direction v)
{
    const double a = oldOrigin.latitude * Pi / 180.0;
    const double b = newOrigin.latitude * Pi / 180.0;
    const double dl = (newOrigin.longitude - oldOrigin.longitude) * Pi / 180.0;
    const Direction east{std::cos(dl), -std::cos(a) * std::sin(dl), std::sin(a) * std::sin(dl)};
    const auto up = GlobeNormal(oldOrigin.latitude, oldOrigin.longitude, newOrigin.latitude, newOrigin.longitude);
    const Direction north{-std::sin(b) * std::sin(dl),
        std::sin(a) * std::cos(b) - std::cos(a) * std::sin(b) * std::cos(dl),
        std::cos(a) * std::cos(b) + std::sin(a) * std::sin(b) * std::cos(dl)};
    const auto dot = [v](Direction axis) { return v.x * axis.x + v.y * axis.y + v.z * axis.z; };
    return {dot(east), dot(up), dot(north)};
}

struct FrameRebase { Coordinate origin; double altitude; };
inline double RecenterDistance(double altitude)
{
    // Orbital input can travel kilometres per frame. Reuse its local frame for
    // longer before re-uploading the fixed globe; no provider/draw radius grows.
    // The maximum remains below the legacy +/-500 km coordinate safety guard.
    return std::clamp(altitude * .25, FreeFlightRecenter, 400000.0);
}
inline std::optional<FrameRebase> CameraRebase(Coordinate origin, double x, double y, double z)
{
    const double dx = x - Anchor, dz = z - Anchor;
    const double threshold = RecenterDistance(y);
    if (!std::isfinite(dx) || !std::isfinite(y) || !std::isfinite(dz) ||
        dx * dx + dz * dz <= threshold * threshold) return std::nullopt;
    const double up = Radius + y;
    if (up <= 0.0) return std::nullopt;
    const double radius = std::hypot(std::hypot(dx, dz), up);
    return FrameRebase{FromDirection(origin.latitude, origin.longitude, {dx, up, dz}), radius - Radius};
}

// Conservative source-rectangle overlap in the spherical projection. Arc
// length bounds the difference of every projected coordinate, including at a
// dateline or near a pole. It may admit extra atlas work, never omit coverage.
inline bool GeographicBoundsOverlap(Coordinate origin, double south, double west, double north, double east,
    double x, double z, double span)
{
    const auto centre = ToLocal(origin.latitude, origin.longitude, .5 * (south + north), .5 * (west + east));
    const double reach = Radius * Pi / 360.0 * ((north - south) + (east - west));
    return centre.x + reach > x && centre.x - reach < x + span &&
        centre.z + reach > z && centre.z - reach < z + span;
}
}
