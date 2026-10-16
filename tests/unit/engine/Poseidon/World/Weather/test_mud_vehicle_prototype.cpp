#include <Poseidon/World/Weather/MudVehiclePrototype.hpp>
#include <cassert>
#include <limits>

using namespace Poseidon;

int main()
{
    // Existing default is retained and no inherited/mod class silently opts in.
    assert(SelectMudWheelProfile("Jeep", false, true).enabled);
    assert(SelectMudWheelProfile("Jeep", true, false).maxDepth == .12f);
    assert(!SelectMudWheelProfile("Truck5t", false, true).enabled);
    assert(!SelectMudWheelProfile("Truck5t", true, false).enabled);
    assert(!SelectMudWheelProfile("Truck5tOpen", true, true).enabled);
    assert(!SelectMudWheelProfile("Ural", true, true).enabled);
    assert(!SelectMudWheelProfile(nullptr, true, true).enabled);
    const auto truck = SelectMudWheelProfile("Truck5t", true, true);
    assert(truck.enabled && truck.wheels == 6 && truck.maxDepth == .18f && truck.depthScale == .8f);

    // Families from the config inheritance chain: every truck and car, no name list.
    {
        const char* repair[] = {"Truck5tRepair", "Truck5t", "Truck", "Car", "LandVehicle", "Land", "AllVehicles", "All"};
        const char* ural[] = {"Ural", "Truck", "Car", "LandVehicle"};
        const char* jelcz[] = {"SW_Jelcz442", "truck", "car"};              // mod truck, any case
        const char* uaz[] = {"UAZ", "Car", "LandVehicle"};
        const char* tank[] = {"T72", "Tank", "LandVehicle"};
        const char* none[] = {"Thing"};
        assert(ClassifyMudVehicle(repair, 8) == MudVehicleFamily::Truck);
        assert(ClassifyMudVehicle(ural, 4) == MudVehicleFamily::Truck);
        assert(ClassifyMudVehicle(jelcz, 3) == MudVehicleFamily::Truck);
        assert(ClassifyMudVehicle(uaz, 3) == MudVehicleFamily::Car);
        assert(ClassifyMudVehicle(tank, 3) == MudVehicleFamily::Tracked);
        const char* bmp[] = {"BMP", "APC", "Tank", "LandVehicle"};             // APCs inherit from Tank
        assert(ClassifyMudVehicle(bmp, 4, 13000) == MudVehicleFamily::Tracked);
        assert(ClassifyMudVehicle(none, 1) == MudVehicleFamily::None);
        assert(ClassifyMudVehicle(nullptr, 3) == MudVehicleFamily::None);
        // Mod trucks that inherit straight from Car (seen in game: Star944) are trucks by mass.
        const char* star[] = {"Star944", "Car", "LandVehicle"};
        assert(ClassifyMudVehicle(star, 3, 9000) == MudVehicleFamily::Truck);
        assert(ClassifyMudVehicle(star, 3, 0) == MudVehicleFamily::Car);          // mass unknown
        assert(ClassifyMudVehicle(uaz, 3, 2400) == MudVehicleFamily::Car);        // light car stays a car
        assert(ClassifyMudVehicle(tank, 3, 40000) == MudVehicleFamily::Tracked);  // never a truck by mass
        assert(ClassifyMudVehicle(uaz, 3, std::numeric_limits<float>::quiet_NaN()) == MudVehicleFamily::Car);
        const char* odd[] = {nullptr, "Truck"};
        assert(ClassifyMudVehicle(odd, 2) == MudVehicleFamily::Truck);
        // Truck profile, same tuning as Truck5t; wheel count follows the model.
        const auto t6 = SelectMudWheelProfile(MudVehicleFamily::Truck, 6, true, true);
        const auto t4 = SelectMudWheelProfile(MudVehicleFamily::Truck, 4, true, true);
        assert(t6.enabled && t6.maxDepth == truck.maxDepth && t6.gripLoss == truck.gripLoss && t6.wheels == 6);
        assert(t4.enabled && t4.wheels == 4);
        assert(SelectMudWheelProfile(MudVehicleFamily::Truck, 0, true, true).wheels == 6);   // unknown: default
        assert(SelectMudWheelProfile(MudVehicleFamily::Truck, 40, true, true).wheels == 6);  // absurd: default
        assert(!SelectMudWheelProfile(MudVehicleFamily::Truck, 6, true, false).enabled);   // trucks: SP only
        assert(!SelectMudWheelProfile(MudVehicleFamily::Truck, 6, false, true).enabled);   // switch off
        const auto car = SelectMudWheelProfile(MudVehicleFamily::Car, 4, false, false);
        assert(car.enabled && car.maxDepth == SelectMudWheelProfile("Jeep", false, true).maxDepth);
        assert(!SelectMudWheelProfile(MudVehicleFamily::None, 4, true, true).enabled);
        assert(!SelectMudWheelProfile(MudVehicleFamily::Tracked, 2, true, true).enabled); // tracks: own profile
        // Tracks: two belts, shallower per pass than a truck tyre, much less grip lost;
        // a main battle tank sinks and digs more than a light APC. SP only.
        const auto apc = SelectMudTrackProfile(13000, true), mbt = SelectMudTrackProfile(46000, true);
        assert(apc.enabled && mbt.enabled && apc.wheels == 2 && mbt.wheels == 2);
        assert(apc.maxDepth < truck.maxDepth && mbt.maxDepth < truck.maxDepth);
        assert(apc.gripLoss < truck.gripLoss && mbt.gripLoss < truck.gripLoss);
        assert(mbt.maxDepth > apc.maxDepth && mbt.digMax > apc.digMax && mbt.gripLoss > apc.gripLoss);
        assert(!SelectMudTrackProfile(46000, false).enabled);
        assert(SelectMudTrackProfile(std::numeric_limits<float>::quiet_NaN(), true).maxDepth == apc.maxDepth);
    }

    assert(MudRollingDeceleration(10, .24f, .15f, .1f, 4, 4) == 0);
    assert(MudRollingDeceleration(10, .24f, 1, .1f, 0, 4) == 0);
    assert(MudRollingDeceleration(10, 0, 1, .1f, 4, 4) == 0);
    assert(MudRollingDeceleration(0, .24f, 1, .1f, 4, 4) == 0);
    assert(MudRollingDeceleration(10, .24f, 1, 0, 4, 4) == 0);
    assert(MudRollingDeceleration(std::numeric_limits<float>::quiet_NaN(), .24f, 1, .1f, 4, 4) == 0);
    const float whole = MudRollingDeceleration(10, .24f, 1, .1f, 4, 4);
    const float half = MudRollingDeceleration(10, .24f, 1, .1f, 2, 4);
    assert(std::abs(whole - 1.2f) < .000001f && std::abs(half * 2 - whole) < .000001f);
    assert(MudRollingDeceleration(-10, .24f, 1, .1f, 4, 4) == -whole);

    // Across speeds and time steps, resistance cannot add kinetic energy or
    // reverse a vehicle at rest/low speed; admitted-wheel count is bounded.
    for (float seconds : {.001f, .016f, .1f, 1.0f, 10.0f})
        for (float speed : {-25.0f, -1.0f, -.001f, 0.0f, .001f, 1.0f, 25.0f})
        {
            const float a = MudRollingDeceleration(speed, 1, 1, seconds, 50, 4);
            const float next = speed - seconds * a;
            assert(std::abs(a) <= 1.200001f);
            assert(std::abs(next) <= std::abs(speed) + .000001f);
            assert(speed * next >= -.000001f);
        }
}
