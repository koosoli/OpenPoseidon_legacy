#include <Poseidon/World/Weather/RotorWaterPolicy.hpp>
#include <cassert>
#include <initializer_list>
#include <limits>
using namespace Poseidon;
int main()
{
    // Nogova's measured native seabed interval: below local sea, no island gate.
    for (float datum : {0.0f, 3.6831796f, 123.0f})
    {
        assert(RotorSeaWashEligible(1,datum+8,datum-7.29f,datum));
        assert(RotorSeaWashEligible(1,datum+8,datum-1.79f,datum));
        assert(!RotorSeaWashEligible(1,datum+31,datum-7,datum));
        assert(!RotorSeaWashEligible(1,datum-3,datum-7,datum));
        assert(!RotorSeaWashEligible(1,datum+8,datum+.3f,datum));
        assert(!RotorSeaWashEligible(0,datum+8,datum-7,datum));
        assert(!RotorSeaWashEligible(.02f,datum+8,datum-7,datum));
    }
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
    {
        assert(!RotorSeaWashEligible(invalid,8,-7,0));
        assert(!RotorSeaWashEligible(1,invalid,-7,0));
        assert(!RotorSeaWashEligible(1,8,invalid,0));
        assert(!RotorSeaWashEligible(1,8,-7,invalid));
    }
}
