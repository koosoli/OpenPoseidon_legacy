#include <Poseidon/World/Entities/Infantry/FootstepWater.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <cassert>
#include <cstdio>
#include <limits>

using namespace Poseidon;
int main()
{
    SoundStepSole sole;
    assert(!sole.Fresh(1000));
    sole.Capture(5, 1.04f, 7, 1000);
    assert(sole.Fresh(1000) && sole.Fresh(1250));
    assert(!sole.Fresh(999) && !sole.Fresh(1251));
    const auto emitted = sole;
    sole.Clear();
    assert(!sole.Fresh(1000) && emitted.Fresh(1000)); // consume exactly once
    sole.Capture(9, 2, 11, 1200); // a later leg replaces, never mixes positions
    assert(sole.position[0] == 9 && sole.position[1] == 2 && sole.position[2] == 11);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis)
    {
        float p[] = {1, 2, 3}; p[axis] = nan;
        sole.Capture(p[0], p[1], p[2], 1000);
        assert(!sole.Fresh(1000));
    }
    sole.Capture(1, 2, 3, std::numeric_limits<std::int64_t>::min());
    assert(!sole.Fresh(std::numeric_limits<std::int64_t>::max()));

    RainWaterField water;
    assert(water.Configure(3, 3, 1, std::vector<float>(9, 1.0f)));
    const auto test = [](const RainWaterField::Sample& s, float foot = 1.04f,
                         float support = 1, float sea = 0, bool contact = true,
                         bool attached = false)
    {
        return StandingWaterFootstep(s.valid, s.depth, s.height, foot, support,
                                    sea, contact, attached);
    };
    assert(!test(water.At(1, 1))); // valid but actually dry
    assert(water.AddWater(1, 1, .02f));
    const auto puddle = water.At(1, 1);
    assert(test(puddle)); // actual production field, no invented wetness mask
    const auto revision = water.Revision();
    const auto volume = water.Volume();
    const auto pending = water.PendingSeconds();
    for (int i = 0; i < 500; ++i) assert(test(water.At(1, 1)));
    assert(water.Revision() == revision && water.Volume() == volume &&
           water.PendingSeconds() == pending); // sound queries never advance water
    assert(!test(water.At(-1, 1)) && !test(water.At(nan, 1)));
    assert(!test(puddle, 1.22f, 1.2f)); // dry raised pavement/room floor
    assert(!test(puddle, 1.27f)); // unsupported sole in the air
    assert(!test(puddle, .5f)); // not the supporting boot
    assert(!test(puddle, 1.04f, 1, 0, false)); // no solid support
    assert(!test(puddle, 1.04f, 1, 0, true, true)); // attached/cargo foot
    assert(!test(puddle, 1.04f, 1, 1)); // sea outlet; existing GroundWater route owns it
    assert(!test(puddle, nan) && !test(puddle, 1.04f, inf) && !test(puddle, 1.04f, 1, nan));
    for (float v : {nan, inf, -1.0f, 0.0f, .001f})
        assert(!StandingWaterFootstep(true, v, 1.02f, 1.04f, 1, 0, true, false));
    assert(!StandingWaterFootstep(true, .02f, inf, 1.04f, 1, 0, true, false));
    // A fully flooded low pavement really reaches the boot; do not exclude all
    // roads by object class. Only actual supporting elevation matters.
    assert(water.AddWater(1, 1, .30f));
    assert(test(water.At(1, 1), 1.22f, 1.2f));
    water.Reset();
    assert(!test(water.At(1, 1))); // new world cannot inherit a prior cell
    std::puts("Actual standing-water foot admission/event lifetime/read-only field checks PASS.");
}
