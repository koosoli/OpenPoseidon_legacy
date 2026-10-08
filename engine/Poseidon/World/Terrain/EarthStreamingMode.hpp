#pragma once

#include "EarthGroundPatch.hpp"

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
// An explicit geographic teleport. Finite OFP map objects/navigation remain local.
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
    std::shared_ptr<const GroundPatch> Ground() const
    {
        std::lock_guard lock(_mutex);
        return _ground;
    }
    std::optional<GroundSample> QueryGround(float x, float z) const
    {
        std::lock_guard lock(_mutex);
        return _ground ? _ground->Sample(x, z) : std::nullopt;
    }
  private:
    mutable std::mutex _mutex;
    Location _location;
    std::shared_ptr<const GroundPatch> _ground;
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
        500.0f,OrbitAltitudeLimit);
}
}
