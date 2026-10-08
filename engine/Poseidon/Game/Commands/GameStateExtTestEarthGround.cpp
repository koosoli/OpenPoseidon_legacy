#include <Evaluator/express.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <cmath>
#include <cstdio>

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
