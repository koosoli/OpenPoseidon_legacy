#include <Poseidon/World/Weather/RotorGroundPolicy.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <cassert>
#include <limits>

using namespace Poseidon;
int main()
{
    RotorGroundBudget budget;
    for (int i=0; i<32; ++i) assert(budget.Take(.01f));
    assert(!budget.Take(.02f) && !budget.Take(.249f));
    for (int i=0; i<32; ++i) assert(budget.Take(.25f));
    assert(!budget.Take(.499f));
    assert(budget.Take(0)); // mission time reset, no old-world emitter state
    assert(!budget.Take(std::numeric_limits<float>::quiet_NaN()));
    assert(!budget.Take(-1));
    auto sample = [](float rpm = 1, float height = 5, float up = 1,
                     float dust = .5f, float snow = 0, float wet = 0, float rain = 0,
                     float water = 0, bool valid = false, bool land = true,
                     bool roof = false, bool road = false) {
        return RotorGroundPolicy(rpm,height,up,dust,snow,wet,rain,water,valid,land,roof,road);
    };
    const auto dust = sample();
    assert(dust.density > .6f && !dust.snow);
    assert(sample(.5f).density < dust.density * .26f);
    assert(sample(1,20).density < dust.density);
    assert(sample(0).density == 0 && sample(1,30).density == 0);
    assert(sample(1,-1).density == 0 && sample(1,5,.2f).density == 0);
    assert(sample(1,5,1,0).density == 0); // actual hard/no-dust metadata
    assert(sample(1,5,1,.5f,0,1).density == 0);
    assert(sample(1,5,1,.5f,0,0,1).density == 0);
    assert(sample(1,5,1,.5f,0,0,0,0,false,false).density == 0); // sea
    assert(sample(1,5,1,.5f,0,0,0,0,false,true,true).density == 0); // actual shelter
    assert(sample(1,5,1,.5f,0,0,0,0,false,true,false,true).density == 0); // roadway
    assert(sample(1,5,1,.5f,0,0,0,.004f,true).density == 0); // real standing water
    assert(sample(1,5,1,.5f,0,0,0,0,false).density > 0); // absent hydrology dry fallback
    const auto snow = sample(1,5,1,0,.1f,1,0);
    assert(snow.snow && snow.density > .6f); // actual deposited powder
    assert(sample(1,5,1,0,.1f,0,0,0,false,true,true).density == 0);
    assert(sample(std::numeric_limits<float>::quiet_NaN()).density == 0);
    assert(sample(1,5,1,.5f,0,0,0,std::numeric_limits<float>::infinity(),true).density == 0);
    // Exercise production hydrology query rather than invent a water sample.
    RainWaterField water;
    assert(water.Configure(3,3,1,std::vector<float>(9,1.0f)));
    assert(water.AddWater(1,1,.02f));
    const auto flooded = water.At(1,1);
    assert(flooded.valid && flooded.depth > .003f);
    assert(sample(1,5,1,.5f,0,0,0,flooded.depth,flooded.valid).density == 0);
    // Fixed-world annulus, deterministic private stream and no centre plume.
    for (unsigned phase=0; phase<32; ++phase) for (unsigned spoke=0; spoke<8; ++spoke) {
        const auto a = RotorGroundRing(phase,spoke,8);
        assert(a == RotorGroundRing(phase,spoke,8));
        const float radius = std::hypot(a[0],a[1]);
        assert(std::isfinite(radius) && radius >= 5.5f && radius <= 7.7f);
        assert(a != RotorGroundRing(phase+1,spoke,8));
    }
    assert((RotorGroundRing(0,0,std::numeric_limits<float>::infinity()) == std::array<float,2>{}));
}
