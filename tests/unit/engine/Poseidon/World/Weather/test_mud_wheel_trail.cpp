#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/MudWheelTrail.hpp>
#include <cassert>
#include <limits>

using namespace Poseidon;

static MudField Drive(int divisions)
{
    MudField mud;
    mud.Advance(120, 1);
    MudWheelTrail wheel;
    auto load = [&](float x, float, float z) {
        mud.Stamp(x, z, 0.4f, 0.1f, 0.8f);
        return true;
    };
    for (int i = 0; i <= divisions; ++i)
        wheel.Update(0.0625f + 2.0f * i / divisions, 0, 0.0625f, true, load);
    return mud;
}

int main()
{
    // Independent dense and sparse simulation schedules load the same soil.
    const MudField dense = Drive(128), sparse = Drive(8);
    for (int z = -8; z <= 8; ++z)
        for (int x = -8; x <= 24; ++x)
            assert(std::abs(dense.HeightOffsetAt(x * .125f, z * .125f) -
                            sparse.HeightOffsetAt(x * .125f, z * .125f)) < .000001f);
    assert(dense.HeightOffsetAt(1, .0625f) < -.02f);

    MudWheelTrail wheel;
    int calls = 0;
    auto count = [&](float, float, float) { ++calls; return true; };
    wheel.Update(0, 0, 0, true, count);
    for (int i = 0; i < 1000; ++i) wheel.Update(0, 0, 0, true, count);
    assert(calls == 1); // Standing does not dig each frame.
    wheel.Update(0, 0, 0, false, count);
    wheel.Update(2, 0, 0, true, count);
    assert(calls == 2); // Airborne interval was not bridged.
    wheel.Update(100, 0, 0, true, count);
    assert(calls == 3); // Teleport deposits once, never a world-long strip.
    wheel.Update(std::numeric_limits<float>::quiet_NaN(), 0, 0, true, count);
    assert(calls == 3);

    // Road/dry/shelter rejection breaks continuity. Endpoints alone would miss it.
    wheel.Reset(); calls = 0;
    auto admit = [&](float x, float, float) { ++calls; return x < .4f || x > 1.0f; };
    assert(wheel.Update(0, 0, 0, true, admit));
    assert(!wheel.Update(1, 0, 0, true, admit));
    assert(calls == 3); // Start, .25 admitted, .5 refused.
    assert(wheel.Update(2, 0, 0, true, admit));
    assert(calls == 4);

    // Repeat driving can deepen a rut but cannot exceed its chosen capacity.
    MudField repeat = Drive(8);
    const float original = repeat.HeightOffsetAt(.0625f, .0625f);
    for (int i = 0; i < 50; ++i) repeat.Stamp(.0625f, .0625f, .4f, .1f, .8f);
    assert(repeat.HeightOffsetAt(.0625f, .0625f) < original);
    assert(repeat.HeightOffsetAt(.0625f, .0625f) >= -.100001f);
}
