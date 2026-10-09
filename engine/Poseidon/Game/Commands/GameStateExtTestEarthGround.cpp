#include <Evaluator/express.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace Poseidon;

// Read-only installed acceptance: unlike legacy scalar height queries, this
// distinguishes nonresident ground from a real zero-height surface. All height,
// slope, bounds and generation fields come from one immutable patch snapshot.
GameValue TriEarthGround(const GameState*, GameValuePar arg)
{
    if (!EarthStreaming::Enabled()) return GameValue("FAIL:earth_preview_disabled");
    const GameArrayType& values = arg;
    if (values.Size() != 2 || values[0].GetType() != GameScalar || values[1].GetType() != GameScalar)
        return GameValue("FAIL:expected_x_z");
    const float x = values[0], z = values[1];
    if (!std::isfinite(x) || !std::isfinite(z)) return GameValue("FAIL:nonfinite_position");
    const auto location = EarthStreaming::GetLocation();
    const auto ground = EarthStreaming::Locations().Ground();
    const bool resident = ground && ground->revision == location.revision;
    const auto sample = resident ? ground->Sample(x, z) : std::nullopt;
    Man* player = GWorld ? dyn_cast<Man>(GWorld->GetRealPlayer()) : nullptr;
    const Shape* contact = player && player->GetShape() ? player->GetShape()->LandContactLevel() : nullptr;
    const int contactPoints = contact ? contact->NPos() : 0;
    char result[768];
    char bounds[160] = "null";
    if (resident)
    {
        const double span = double(ground->samples - 1) * ground->gridMetres;
        snprintf(bounds, sizeof(bounds), "[%.9g,%.9g,%.9g,%.9g]", double(ground->x), double(ground->z),
            ground->x + span, ground->z + span);
    }
    snprintf(result, sizeof(result),
        "{\"covered\":%s,\"revision\":%llu,\"generation\":%llu,\"bounds\":%s,\"grid\":%.9g,"
        "\"height\":%.9g,\"dX\":%.9g,\"dZ\":%.9g,\"playerContactPoints\":%d,\"playerDown\":%s}",
        sample ? "true" : "false", static_cast<unsigned long long>(location.revision),
        static_cast<unsigned long long>(resident ? ground->generation : 0), bounds,
        resident ? double(ground->gridMetres) : 0, sample ? double(sample->height) : 0,
        sample ? double(sample->dX) : 0, sample ? double(sample->dZ) : 0,
        contactPoints, player && player->IsDown() ? "true" : "false");
    return GameValue(result);
}

// ---------------------------------------------------------------------------------------------------------------
// Whole-world script commands for mission makers. Positions are the usual script order [x, y(north), height];
// the map's geographic origin sits at EarthStreaming::CameraAnchor. Outside whole-world mode every command
// answers empty / false rather than inventing a location.

namespace
{
GameValue EmptyArray(const GameState* state)
{
    return state->CreateGameValue(GameArray);
}

bool ReadNumbers(GameValuePar arg, int minimum, double* out, int maximum)
{
    if (arg.GetType() != GameArray)
        return false;
    const GameArrayType& values = arg;
    if (values.Size() < minimum || values.Size() > maximum)
        return false;
    for (int i = 0; i < values.Size(); ++i)
    {
        if (values[i].GetType() != GameScalar)
            return false;
        out[i] = static_cast<float>(values[i]);
        if (!std::isfinite(out[i]))
            return false;
    }
    return true;
}
} // namespace

/// earthLocation -> [latitude, longitude, altitude, revision] of the geographic origin, or [].
GameValue EarthLocation(const GameState* state)
{
    GameValue value = EmptyArray(state);
    if (!EarthStreaming::Enabled())
        return value;
    const auto location = EarthStreaming::GetLocation();
    GameArrayType& array = value;
    array.Add(GameValue(float(location.latitude)));
    array.Add(GameValue(float(location.longitude)));
    array.Add(GameValue(location.altitude));
    array.Add(GameValue(float(location.revision)));
    return value;
}

/// earthPosToGeo [x, y(, z)] -> [latitude, longitude], or [] outside whole-world mode / off the globe.
GameValue EarthPosToGeo(const GameState* state, GameValuePar arg)
{
    GameValue value = EmptyArray(state);
    double p[3] = {};
    if (!EarthStreaming::Enabled() || !ReadNumbers(arg, 2, p, 3))
        return value;
    const auto origin = EarthStreaming::GetLocation();
    try
    {
        const auto c = Earth::FromLocal(origin.latitude, origin.longitude, p[0], p[1]);
        GameArrayType& array = value;
        array.Add(GameValue(float(c.latitude)));
        array.Add(GameValue(float(c.longitude)));
    }
    catch (const std::out_of_range&)
    {
    }
    return value;
}

/// earthGeoToPos [latitude, longitude] -> [x, y, 0], or [] when the point is on the far side of the globe.
GameValue EarthGeoToPos(const GameState* state, GameValuePar arg)
{
    GameValue value = EmptyArray(state);
    double g[2] = {};
    if (!EarthStreaming::Enabled() || !ReadNumbers(arg, 2, g, 2))
        return value;
    const auto origin = EarthStreaming::GetLocation();
    const auto local = Earth::ToLocal(origin.latitude, origin.longitude, g[0], g[1]);
    if (!local.visible)
        return value;
    GameArrayType& array = value;
    array.Add(GameValue(float(local.x)));
    array.Add(GameValue(float(local.z)));
    array.Add(GameValue(0.0f));
    return value;
}

/// earthGoTo [latitude, longitude, altitude] -> true when the geographic origin moved there. The world is
/// rebuilt around map position [6400, 6400]; move the player/camera there to stand at the new place.
GameValue EarthGoTo(const GameState*, GameValuePar arg)
{
    double g[3] = {};
    if (!EarthStreaming::Enabled() || !ReadNumbers(arg, 3, g, 3))
        return GameValue(false);
    return GameValue(EarthStreaming::Locations().Set(g[0], g[1], g[2]));
}

/// earthGroundHeight [x, y] -> [height] of the streamed ground there, or [] where none is resident yet.
GameValue EarthGroundHeight(const GameState* state, GameValuePar arg)
{
    GameValue value = EmptyArray(state);
    double p[2] = {};
    if (!EarthStreaming::Enabled() || !ReadNumbers(arg, 2, p, 2))
        return value;
    if (const auto sample = EarthStreaming::QueryGround(float(p[0]), float(p[1])))
    {
        GameArrayType& array = value;
        array.Add(GameValue(sample->height));
    }
    return value;
}
