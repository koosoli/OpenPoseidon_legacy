#pragma once

#include "EarthGroundPatch.hpp"
#include "EarthFrame.hpp"
#include "EarthMoonFrame.hpp"
#include "EarthMoonRelief.hpp"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <memory>
#include <stdexcept>

namespace Poseidon::EarthStreaming
{
// Shared opt-in gate. Only published resident near elevation supplies Earth ground.
inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("POSEIDON_WHOLE_WORLD");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
constexpr float CoordinateLimit = 500000.0f;
constexpr float OrbitAltitudeLimit = 30000000.0f;
// Free flight itself may leave orbit and travel to the moon (384,400 km); the explicit Go/menu
// altitude keeps the orbit limit above.
constexpr float FlightAltitudeLimit = 450000000.0f;
constexpr float FreeFlightAltitudeMinimum = -12000.0f;
constexpr float CameraAnchor = 6400.0f;
struct Location
{
    double latitude = 46.6, longitude = 8.1;
    float altitude = 4500.0f;
    uint64_t revision = 1;
};
inline bool ValidateLocation(double latitude, double longitude, double altitude)
{
    return std::isfinite(latitude) && std::abs(latitude)<=84.0 &&
        std::isfinite(longitude) && std::abs(longitude)<=180.0 &&
        std::isfinite(altitude) && altitude>=500.0 && altitude<=OrbitAltitudeLimit;
}
// Internal physical flight/rebase may approach low/negative resident terrain.
// Explicit geographic Go retains its separate safe starting-altitude contract.
inline bool ValidateFlightLocation(double latitude, double longitude, double altitude)
{
    return std::isfinite(latitude) && std::abs(latitude)<=84.0 &&
        std::isfinite(longitude) && std::abs(longitude)<=180.0 &&
        std::isfinite(altitude) && altitude>=FreeFlightAltitudeMinimum && altitude<=FlightAltitudeLimit;
}
// Geographic frame for explicit teleports and Earth free-flight rebasing.
// Finite OFP map objects/navigation remain local.
// Origin and revision are read together so a worker can never combine two Gos.
class LocationState
{
  public:
    explicit LocationState(Location initial = {}) : _location(initial)
    {
        if (!ValidateLocation(initial.latitude,initial.longitude,initial.altitude))
            throw std::invalid_argument("invalid Earth location");
        if (_location.longitude==180.0) _location.longitude=-180.0;
    }
    Location Get() const { std::lock_guard lock(_mutex); return _location; }
    bool Set(double latitude,double longitude,double altitude)
    {
        if (!ValidateLocation(latitude,longitude,altitude)) return false;
        std::lock_guard lock(_mutex);
        _location={latitude,longitude==180.0 ? -180.0 : longitude,float(altitude),_location.revision+1};
        _ground.reset();
        return true;
    }
    bool IsCurrent(uint64_t revision) const { return Get().revision==revision; }
    // A camera may only rebase the frame it actually read. A simultaneous
    // explicit Go wins; neither old worker results nor old ground survive.
    bool Rebase(uint64_t expectedRevision, double latitude, double longitude, double altitude)
    {
        if (!ValidateFlightLocation(latitude,longitude,altitude)) return false;
        std::lock_guard lock(_mutex);
        if (_location.revision != expectedRevision) return false;
        _location={latitude,longitude==180.0 ? -180.0 : longitude,float(altitude),_location.revision+1};
        _ground.reset();
        return true;
    }
    bool PublishGround(GroundPatch patch)
    {
        if (!patch.Valid()) return false;
        auto immutable = std::make_shared<const GroundPatch>(std::move(patch));
        std::lock_guard lock(_mutex);
        if (immutable->revision != _location.revision ||
            (_ground && immutable->generation <= _ground->generation)) return false;
        _ground = std::move(immutable);
        return true;
    }
    void ClearGround() { std::lock_guard lock(_mutex); _ground.reset(); }
    // Total local distance the free-flight origin has been rebased by. Anything laid out in local
    // map coordinates that must stay attached to the ground across rebases (the cloud deck) adds
    // this, otherwise it snaps back with the camera every 32 km and seems to follow it.
    void AddTravel(double x, double z) { std::lock_guard lock(_mutex); _travelX += x; _travelZ += z; }
    void Travel(double& x, double& z) const { std::lock_guard lock(_mutex); x = _travelX; z = _travelZ; }
    std::shared_ptr<const GroundPatch> Ground() const
    {
        std::lock_guard lock(_mutex);
        return _ground;
    }
    std::optional<GroundSample> QueryGround(float x, float z) const
    {
        // In the moon frame (EarthMoonFrame.hpp) the ground is the moon's surface: its sphere raised by the
        // LOLA topography (EarthMoonRelief.hpp) -- the heights the sky shader marches the moon against, so
        // the ground walked on is the ground drawn.
        if (MoonFrameActive())
        {
            const MoonFrame frame = Celestial().Frame();
            // The frame's axes in moon-fixed coordinates (w, e, p as x, y, z): MoonBasis without the sky.
            const MoonAxes m{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            const D3 U = Selenographic(m, frame.latitude, frame.longitude);
            D3 E = Cross(m.p, U);
            E = Length(E) > 1e-9 ? Normalize(E) : m.e;
            const D3 N = Cross(U, E);
            const auto ground = [&](double px, double pz) {
                const double dx = px - Earth::Anchor, dz = pz - Earth::Anchor;
                const double sphere = std::sqrt(std::max(MoonRadius * MoonRadius - dx * dx - dz * dz, 1.0)) - MoonRadius;
                double latitude = 0, longitude = 0;
                SelenographicOf(m, E * dx + U * (sphere + MoonRadius) + N * dz, latitude, longitude);
                const double r = MoonRadius + MoonReliefHeight(latitude, longitude);
                return std::sqrt(std::max(r * r - dx * dx - dz * dz, 1.0)) - MoonRadius;
            };
            const double h = ground(x, z);
            const double sx = 0.5 * (ground(double(x) + 1, z) - ground(double(x) - 1, z));
            const double sz = 0.5 * (ground(x, double(z) + 1) - ground(x, double(z) - 1));
            std::lock_guard lock(_mutex);
            return GroundSample{float(h), float(sx), float(sz), _location.revision, 1};
        }
        std::lock_guard lock(_mutex);
        return _ground ? _ground->Sample(x, z) : std::nullopt;
    }
  private:
    mutable std::mutex _mutex;
    Location _location;
    std::shared_ptr<const GroundPatch> _ground;
    double _travelX = 0, _travelZ = 0;
};
inline LocationState& Locations()
{
    static LocationState state([] {
        const auto setting=[](const char* key,double fallback) {
            const char* value=std::getenv(key);
            if (!value) return fallback;
            char* end=nullptr;
            const double result=std::strtod(value,&end);
            if (end==value || *end || !std::isfinite(result)) throw std::invalid_argument(key);
            return result;
        };
        Location initial;
        initial.latitude=setting("POSEIDON_EARTH_LATITUDE",initial.latitude);
        initial.longitude=setting("POSEIDON_EARTH_LONGITUDE",initial.longitude);
        if (!ValidateLocation(initial.latitude,initial.longitude,initial.altitude))
            throw std::invalid_argument("Earth origin outside supported latitude/longitude range");
        if (initial.longitude==180.0) initial.longitude=-180.0;
        return initial;
    }());
    return state;
}
inline Location GetLocation() { return Locations().Get(); }
inline std::optional<GroundSample> QueryGround(float x, float z) { return Locations().QueryGround(x, z); }
// Float-only legacy callers cannot express missing coverage. A finite datum is
// their fallback, never proof of ground. Teleport/collision require QueryGround.
inline float GroundHeight(float x, float z, float* dX = nullptr, float* dZ = nullptr)
{
    const auto sample = QueryGround(x, z);
    if (dX) *dX = sample ? sample->dX : 0;
    if (dZ) *dZ = sample ? sample->dZ : 0;
    return sample ? sample->height : 0;
}
inline float ZoomAltitude(float altitude, float wheelSteps)
{
    if (!std::isfinite(altitude) || !std::isfinite(wheelSteps)) return 4500.0f;
    return std::clamp((altitude+1000.0f)*std::pow(0.72f,std::clamp(wheelSteps,-20.0f,20.0f))-1000.0f,
        500.0f,FlightAltitudeLimit);
}
}
