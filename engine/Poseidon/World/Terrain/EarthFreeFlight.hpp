#pragma once
#include "EarthStreamingMode.hpp"

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
inline float FlightSpeedScale(FlightPoint point, const GroundPatch* ground, uint64_t revision)
{
    const auto support = FlightGround(ground, revision, point);
    const double clearance = std::max(support ? double(point.y) - support->height : FlightAltitude(point), 0.0);
    // Same orbital curve as before, evaluated above the actual resident ground.
    // No discontinuous high-mountain sea-level speed boost near its surface.
    return float(std::max(std::min(1.0 + clearance*.03 + clearance*clearance*.00002, 64.0),
        std::min(clearance/80.0, 250000.0)));
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
        FlightYAtAltitude(next.x, next.z, OrbitAltitudeLimit));
    return next;
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
