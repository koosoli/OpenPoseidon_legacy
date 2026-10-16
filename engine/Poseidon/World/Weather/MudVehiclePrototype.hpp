#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Poseidon
{
struct MudWheelProfile
{
    float depthScale = 0.5f;
    float maxDepth = 0.12f;
    int wheels = 4;
    bool enabled = false;
    // Traction (MudTraction.hpp): largest share of grip lost in soaked ground,
    // and the deepest rut a spinning wheel can dig (metres).
    float gripLoss = 0.0f;
    float digMax = 0.0f;
    // How fast mud cakes on wheels and body (MudCoat), 1 = MudCoat's own rates.
    // Tested in game: trucks 0.6, light cars 0.33 (both slowed 40% for a gradual
    // build-up), tracked 0.4-0.45.
    float coatRate = 1.0f;
};

// Mud families, chosen by the vehicle's CfgVehicles inheritance rather than a
// list of names, so every truck (Truck5tOpen/Repair/Reammo/Refuel, Ural, mod
// trucks such as a Jelcz) and every light car gets mud without being listed.
// Tracked: everything under Tank in CfgVehicles (tanks, and APCs such as the
// M113 and BMP, which inherit from Tank).
enum class MudVehicleFamily { None, Car, Truck, Tracked };

inline bool MudClassNameIs(const char* a, const char* b)
{
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b)
    {
        const char ca = (*a >= 'A' && *a <= 'Z') ? char(*a - 'A' + 'a') : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? char(*b - 'A' + 'a') : *b;
        if (ca != cb) return false;
    }
    return *a == *b;
}

// A car this heavy is a truck whatever its config says: plenty of mod trucks
// (a Star 944, for one) inherit straight from Car, not from Truck.
constexpr float MudTruckMassKg = 4500.0f;

// chain: class names from the vehicle's own class up to the root
// (e.g. Truck5tRepair, Truck5t, Truck, Car, LandVehicle, ...); mass: kg, 0 = unknown.
inline MudVehicleFamily ClassifyMudVehicle(const char* const* chain, int count, float mass = 0.0f)
{
    if (!chain || count <= 0) return MudVehicleFamily::None;
    bool car = false;
    for (int i = 0; i < count; ++i)
    {
        if (MudClassNameIs(chain[i], "Tank")) return MudVehicleFamily::Tracked;
        if (MudClassNameIs(chain[i], "Truck")) return MudVehicleFamily::Truck;
        if (MudClassNameIs(chain[i], "Car")) car = true;
    }
    if (!car) return MudVehicleFamily::None;
    return std::isfinite(mass) && mass >= MudTruckMassKg ? MudVehicleFamily::Truck : MudVehicleFamily::Car;
}

// wheels: the model's wheel count (animated wheel selections); 0 = unknown.
inline MudWheelProfile SelectMudWheelProfile(MudVehicleFamily family, int wheels, bool truckOptIn, bool singlePlayer)
{
    MudWheelProfile profile;
    switch (family)
    {
    // Light car (jeep, UAZ, ...): floats over most of it, digs shallow ruts.
    // Mud cakes on it more slowly than on a truck (tested in game on a jeep: too quick).
    case MudVehicleFamily::Car: profile = {0.5f, 0.12f, 4, true, 0.38f, 0.18f, 0.33f}; break;
    // Loaded truck: heavy on narrow tyres, sinks and bogs down if driven carelessly,
    // but can still claw its way out of its own ruts with patience (tuned in game).
    case MudVehicleFamily::Truck:
        if (!truckOptIn || !singlePlayer) return {};
        profile = {0.8f, 0.18f, 6, true, 0.42f, 0.22f, 0.6f};
        break;
    default: return {};
    }
    if (wheels >= 2 && wheels <= 12) profile.wheels = wheels;
    return profile;
}

// Tracked vehicles. A track spreads the weight over a long belt, so per pass it
// presses shallower than a truck tyre but over a wide, flat-bottomed band, and it
// keeps far more grip; what bogs a tank down is churning (slipping tracks dig) and
// a heavy hull settling into deep ruts. A main battle tank (25 t+) sinks and digs
// more than a light APC. `wheels` is 2: the two tracks. Single player only, like trucks.
constexpr float MudHeavyTrackedMassKg = 25000.0f;
inline MudWheelProfile SelectMudTrackProfile(float mass, bool singlePlayer)
{
    if (!singlePlayer) return {};
    const bool heavy = std::isfinite(mass) && mass >= MudHeavyTrackedMassKg;
    // coatRate: tested in game, 1.1-1.2 caked tanks within seconds (belts churn and
    // slip far more than tyres roll); slowed to a gradual build-up like the jeep's.
    if (heavy) return {0.6f, 0.14f, 2, true, 0.28f, 0.24f, 0.45f};
    return {0.45f, 0.10f, 2, true, 0.22f, 0.16f, 0.4f};
}

// Name-only form (tests, callers without the class chain): only the two stock
// classes it was tuned on are recognised.
inline MudWheelProfile SelectMudWheelProfile(const char* name, bool truckOptIn, bool singlePlayer)
{
    if (!name) return {};
    if (std::strcmp(name, "Jeep") == 0) return SelectMudWheelProfile(MudVehicleFamily::Car, 4, truckOptIn, singlePlayer);
    if (std::strcmp(name, "Truck5t") == 0) return SelectMudWheelProfile(MudVehicleFamily::Truck, 6, truckOptIn, singlePlayer);
    return {};
}

// Signed longitudinal deceleration, no force at rest and no direction reversal
// from this contribution alone. This is rolling resistance, not a tyre solver.
inline float MudRollingDeceleration(float speed, float layerDepth, float wetness, float seconds,
                                    int admittedWheels, int totalWheels)
{
    if (!std::isfinite(speed) || !std::isfinite(layerDepth) || !std::isfinite(wetness) ||
        !std::isfinite(seconds) || seconds <= 0 || admittedWheels <= 0 || totalWheels <= 0) return 0;
    const float soft = std::clamp((wetness - 0.15f) / 0.85f, 0.0f, 1.0f);
    const float depth = std::clamp(layerDepth / 0.24f, 0.0f, 1.0f);
    const float support = std::clamp(float(admittedWheels) / totalWheels, 0.0f, 1.0f);
    const float magnitude = std::min(1.2f * soft * depth * support, std::abs(speed) / seconds);
    return std::copysign(magnitude, speed);
}
} // namespace Poseidon
