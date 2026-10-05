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
    assert(truck.enabled && truck.wheels == 6 && truck.maxDepth == .16f && truck.depthScale == .65f);

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
