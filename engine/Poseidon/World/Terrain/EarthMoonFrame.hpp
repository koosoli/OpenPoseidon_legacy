#pragma once
// Whole world: the world frame is either the Earth tangent frame at the geographic origin (as always) or,
// near the moon, a MOON tangent frame at a selenographic point. Near the moon the Earth frame's
// coordinates are ~3.8e8 m and a float steps 32 m, which shook the camera and made standing on the moon
// impossible; in the moon frame the camera and the moon's surface are small numbers again.
//
// Everything here is double precision. World coordinates of either frame: x = Anchor + east,
// y = up (0 = the surface point the frame is anchored at), z = Anchor + north.
// ECEF: Earth-centred, Earth-fixed (x through 0N 0E, z through the north pole).
#include "EarthFrame.hpp"
#include "EarthGravity.hpp"
#include <cmath>
#include <cstdint>
#include <mutex>

namespace Poseidon::EarthStreaming
{
constexpr double MoonRadius = 1.7374e6;
constexpr double MoonDistance = 3.844e8; // from the Earth's centre
constexpr double MoonGravity = 1.62;      // m/s^2 at the surface

struct D3
{
    double x = 0, y = 0, z = 0;
};
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 Cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Length(D3 a) { return std::sqrt(Dot(a, a)); }
inline D3 Normalize(D3 a)
{
    const double l = Length(a);
    return l > 0 && std::isfinite(l) ? a * (1.0 / l) : D3{0, 1, 0};
}

// A tangent frame in ECEF: its east/up/north axes and the surface point it is anchored at.
struct Basis
{
    D3 E, U, N, O;
};
inline D3 ToEcef(const Basis& b, double x, double y, double z)
{
    return b.O + b.E * (x - Earth::Anchor) + b.U * y + b.N * (z - Earth::Anchor);
}
inline D3 ToWorld(const Basis& b, D3 X)
{
    const D3 d = X - b.O;
    return {Earth::Anchor + Dot(d, b.E), Dot(d, b.U), Earth::Anchor + Dot(d, b.N)};
}
inline D3 DirToEcef(const Basis& b, D3 v) { return b.E * v.x + b.U * v.y + b.N * v.z; }
inline D3 DirToWorld(const Basis& b, D3 v) { return {Dot(v, b.E), Dot(v, b.U), Dot(v, b.N)}; }

inline Basis EarthBasis(double latitude, double longitude)
{
    const double p = latitude * Earth::Pi / 180.0, l = longitude * Earth::Pi / 180.0;
    const D3 U{std::cos(p) * std::cos(l), std::cos(p) * std::sin(l), std::sin(p)};
    const D3 E{-std::sin(l), std::cos(l), 0};
    const D3 N{-std::sin(p) * std::cos(l), -std::sin(p) * std::sin(l), std::cos(p)};
    return {E, U, N, U * Earth::Radius};
}

// Moon-fixed axes. The moon is tidally locked: w points from its centre at the Earth (selenographic
// 0/0 is the near side's centre), p is the Earth's spin axis made perpendicular to it (the moon's pole is
// within ~7 degrees of that), e completes the right-handed set.
struct MoonAxes
{
    D3 w, e, p;
};
inline MoonAxes MoonFixedAxes(D3 moonCentre)
{
    const D3 w = Normalize(moonCentre * -1.0);
    const D3 z{0, 0, 1};
    const D3 p = Normalize(z - w * Dot(z, w));
    return {w, Cross(p, w), p};
}
inline D3 Selenographic(const MoonAxes& a, double latitude, double longitude)
{
    const double p = latitude * Earth::Pi / 180.0, l = longitude * Earth::Pi / 180.0;
    return a.w * (std::cos(p) * std::cos(l)) + a.e * (std::cos(p) * std::sin(l)) + a.p * std::sin(p);
}
inline void SelenographicOf(const MoonAxes& a, D3 u, double& latitude, double& longitude)
{
    u = Normalize(u);
    latitude = std::asin(std::clamp(Dot(u, a.p), -1.0, 1.0)) * 180.0 / Earth::Pi;
    longitude = std::atan2(Dot(u, a.e), Dot(u, a.w)) * 180.0 / Earth::Pi;
}
inline Basis MoonBasis(D3 moonCentre, double latitude, double longitude)
{
    const MoonAxes a = MoonFixedAxes(moonCentre);
    const D3 U = Selenographic(a, latitude, longitude);
    D3 E = Cross(a.p, U);
    E = Length(E) > 1e-9 ? Normalize(E) : a.e;
    return {E, U, Cross(U, E), moonCentre + U * MoonRadius};
}

// The sky as LightSun last computed it, in the EARTH origin's tangent frame (Lights.cpp), and the
// moon frame when it is active. Main thread writes; the renderer's snapshot reads.
struct SkyGeometry
{
    bool valid = false;
    double originLatitude = 0, originLongitude = 0;
    D3 toSun{0, 1, 0}, toMoon{0, 1, 0};
};
struct MoonFrame
{
    bool active = false;
    double latitude = 0, longitude = 0;
    uint64_t revision = 0;
};
class CelestialState
{
  public:
    SkyGeometry Sky() const { std::lock_guard lock(_mutex); return _sky; }
    void SetSky(const SkyGeometry& sky) { std::lock_guard lock(_mutex); _sky = sky; }
    MoonFrame Frame() const { std::lock_guard lock(_mutex); return _frame; }
    void EnterMoon(double latitude, double longitude)
    {
        std::lock_guard lock(_mutex);
        _frame = {true, latitude, longitude, _frame.revision + 1};
        Gravity = float(MoonGravity);
        OnMoon = true;
    }
    void LeaveMoon()
    {
        std::lock_guard lock(_mutex);
        _frame = {false, 0, 0, _frame.revision + 1};
        Gravity = EarthGravity;
        OnMoon = false;
    }

  private:
    mutable std::mutex _mutex;
    SkyGeometry _sky;
    MoonFrame _frame;
};
inline CelestialState& Celestial()
{
    static CelestialState state;
    return state;
}
inline bool MoonFrameActive() { return Celestial().Frame().active; }

// The moon's centre in ECEF: the published topocentric direction from the Earth origin, at the point
// MoonDistance from the Earth's centre along it.
inline bool MoonCentreEcef(const SkyGeometry& sky, D3& out)
{
    if (!sky.valid) return false;
    const Basis e = EarthBasis(sky.originLatitude, sky.originLongitude);
    const D3 m = Normalize(DirToEcef(e, sky.toMoon));
    const double b = Dot(e.O, m);
    const double t = -b + std::sqrt(std::max(b * b - Earth::Radius * Earth::Radius + MoonDistance * MoonDistance, 0.0));
    out = e.O + m * t;
    return true;
}
// The current world frame: the moon frame when active, otherwise the Earth origin's.
inline bool WorldBasis(double earthLatitude, double earthLongitude, Basis& out)
{
    const MoonFrame frame = Celestial().Frame();
    if (!frame.active)
    {
        out = EarthBasis(earthLatitude, earthLongitude);
        return true;
    }
    D3 c;
    if (!MoonCentreEcef(Celestial().Sky(), c)) return false;
    out = MoonBasis(c, frame.latitude, frame.longitude);
    return true;
}
} // namespace Poseidon::EarthStreaming
