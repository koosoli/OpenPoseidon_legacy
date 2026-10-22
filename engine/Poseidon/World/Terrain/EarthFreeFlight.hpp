#pragma once
#include "EarthStreamingMode.hpp"
#include "EarthMoonFrame.hpp"

namespace Poseidon::EarthStreaming
{
constexpr float FreeFlightClearance = 0.30f;
constexpr float UnknownFlightAltitude = 500.0f;
struct FlightPoint { float x, y, z; };

inline std::optional<GroundSample> FlightGround(const GroundPatch* ground, uint64_t revision, FlightPoint point)
{
    return ground && ground->revision == revision ? ground->Sample(point.x, point.z) : std::nullopt;
}
inline double FlightAltitude(FlightPoint point)
{ return Earth::CameraAltitude(point.x, point.y, point.z); }
inline float FlightYAtAltitude(float x, float z, double altitude)
{
    const double dx = double(x) - CameraAnchor, dz = double(z) - CameraAnchor;
    const double r = Earth::Radius + altitude, horizontal = dx * dx + dz * dz;
    // Stable near sea level; local Y includes spherical curvature, altitude does not.
    return float(altitude - horizontal / (std::sqrt(r * r - horizontal) + r));
}
inline Earth::Direction FlightUp(FlightPoint point)
{
    const double dx = double(point.x) - CameraAnchor, dz = double(point.z) - CameraAnchor;
    const double up = Earth::Radius + point.y, length = std::hypot(std::hypot(dx, dz), up);
    return std::isfinite(length) && length > 0 ? Earth::Direction{dx/length, up/length, dz/length} : Earth::Direction{0,1,0};
}
// Strictly rising with clearance: the old curve sat flat at 64x from ~1.6 km to ~5 km, so climbing
// bought nothing there. Precise below 1 km (51x at 1 km), then a smooth power law: ~200x at 5 km,
// ~2,600x at 100 km, ~18,000x at 1,000 km, held near 250,000x in orbit; past geostationary distance
// the cap opens with the same law, so the moon is a minute away. Free flight takes the smaller of the
// Earth's and the moon's value, so it slows down approaching either surface.
inline double ClearanceSpeedScale(double clearance)
{
    clearance = std::max(clearance, 0.0);
    if (clearance <= 1000.0)
        return 1.0 + clearance * .03 + clearance * clearance * .00002;
    return std::min(51.0 * std::pow(clearance / 1000.0, 0.85),
                    250000.0 * std::max(1.0, std::pow(clearance / 2.5e7, 0.85)));
}
inline float FlightSpeedScale(FlightPoint point, const GroundPatch* ground, uint64_t revision)
{
    // Evaluated above the actual resident ground (no sea-level boost beside a mountain).
    const auto support = FlightGround(ground, revision, point);
    return float(ClearanceSpeedScale(support ? double(point.y) - support->height : FlightAltitude(point)));
}
inline FlightPoint ConstrainFlight(FlightPoint previous, FlightPoint next, const GroundPatch* ground, uint64_t revision)
{
    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z)) return previous;
    next.x = std::clamp(next.x, -CoordinateLimit, CoordinateLimit);
    next.z = std::clamp(next.z, -CoordinateLimit, CoordinateLimit);
    const auto support = FlightGround(ground, revision, next);
    if (support) next.y = std::max(next.y, support->height + FreeFlightClearance);
    else
    {
        const auto before = FlightGround(ground, revision, previous);
        if (before && next.y < before->height + UnknownFlightAltitude)
        {
            // Do not step off a certified patch into unknown ground near its
            // surface. Hold its covered XZ, allow ascent, and retain its floor.
            next.x = previous.x; next.z = previous.z;
            next.y = std::max(next.y, before->height + FreeFlightClearance);
        }
        else if (FlightAltitude(previous) < UnknownFlightAltitude && FlightAltitude(next) < UnknownFlightAltitude)
        {
            // Rebase/coverage retirement at low altitude must not teleport a
            // grounded camera back to 500m. Await fresh support; ascent is allowed.
            next.x = previous.x; next.z = previous.z; next.y = std::max(next.y, previous.y);
        }
        else next.y = std::max(next.y, FlightYAtAltitude(next.x, next.z, UnknownFlightAltitude));
    }
    next.y = std::clamp(next.y, FlightYAtAltitude(next.x, next.z, FreeFlightAltitudeMinimum),
        FlightYAtAltitude(next.x, next.z, FlightAltitudeLimit));
    return next;
}
// The moon as a body free flight can reach (EarthMoonFrame.hpp): its centre in world coordinates of the
// active frame -- from the ephemeris in the Earth frame, the fixed point (A, -R_moon, A) in the moon
// frame. Free flight slows approaching it, levels against it once it is the nearer surface, and keeps
// a little clearance above it.
inline bool MoonCentreWorld(D3& centre)
{
    if (MoonFrameActive())
    {
        centre = {double(Earth::Anchor), -MoonRadius, double(Earth::Anchor)};
        return true;
    }
    D3 moon;
    if (!MoonCentreEcef(Celestial().Sky(), moon)) return false;
    const auto location = GetLocation();
    centre = ToWorld(EarthBasis(location.latitude, location.longitude), moon);
    return true;
}
inline double MoonClearance(FlightPoint p)
{
    D3 c;
    if (!MoonCentreWorld(c)) return 1e12;
    return Length(D3{p.x, p.y, p.z} - c) - MoonRadius;
}
// Height above the Earth's sea level, in either frame.
inline double EarthAltitude(FlightPoint p)
{
    if (!MoonFrameActive()) return FlightAltitude(p);
    const auto location = GetLocation();
    Basis world;
    if (!WorldBasis(location.latitude, location.longitude, world)) return 1e12;
    return Length(ToEcef(world, p.x, p.y, p.z)) - Earth::Radius;
}
// "Up" for free flight: the Earth's radial up, or the moon's once the moon is the nearer surface, so the
// camera levels against the ground it is over and stands upright on the moon.
inline Earth::Direction FlightUpNear(FlightPoint p)
{
    D3 c;
    if (MoonCentreWorld(c))
    {
        const D3 v = D3{p.x, p.y, p.z} - c;
        const double d = Length(v);
        if (std::isfinite(d) && d > 0 && (MoonFrameActive() || d - MoonRadius < FlightAltitude(p)))
            return {v.x / d, v.y / d, v.z / d};
    }
    return FlightUp(p);
}
inline FlightPoint KeepOutsideMoon(FlightPoint p, double clearance)
{
    D3 c;
    if (!MoonCentreWorld(c)) return p;
    const double keepOut = MoonRadius + clearance;
    const D3 v = D3{p.x, p.y, p.z} - c;
    const double d = Length(v);
    if (!(d < keepOut) || d <= 0) return p;
    const D3 out = c + v * (keepOut / d);
    return {float(out.x), float(out.y), float(out.z)};
}
inline FlightPoint ZoomFlight(FlightPoint previous, float wheelSteps, const GroundPatch* ground, uint64_t revision)
{
    if (!std::isfinite(wheelSteps)) return previous;
    auto next = previous;
    if (const auto support = FlightGround(ground, revision, previous))
    {
        const double clearance = std::max(double(previous.y) - support->height - FreeFlightClearance, 0.0);
        // A 2m offset gives small steps at ground level and the same exponential
        // factor in orbit; the old 1000m offset made every near-ground step huge.
        next.y = float(support->height + FreeFlightClearance + std::max(0.0,
            (clearance + 2.0) * std::pow(.72, std::clamp(double(wheelSteps), -20.0, 20.0)) - 2.0));
    }
    else if (FlightAltitude(previous) < UnknownFlightAltitude)
        next.y += float(2.0 * (std::pow(.72, std::clamp(double(wheelSteps), -20.0, 20.0)) - 1.0));
    else next.y = FlightYAtAltitude(previous.x, previous.z, ZoomAltitude(float(FlightAltitude(previous)), wheelSteps));
    return ConstrainFlight(previous, next, ground, revision);
}
}
