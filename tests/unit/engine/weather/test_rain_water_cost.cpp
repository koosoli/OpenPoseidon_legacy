#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include <cassert>
#include <iostream>
#ifdef GetObject
#error The shared runoff diagnostic must not import the GDI GetObject macro
#endif
struct LandscapeLookupFixture { int GetObject(int id) const { return id; } };
using namespace Poseidon;
int main() {
    static_assert(static_cast<unsigned>(RainWaterCost::Phase::FinalCells)==4);
    static_assert(static_cast<unsigned>(RainWaterCost::Phase::FusedFlux)==5&&RainWaterCost::PhaseCount==6);
    assert(LandscapeLookupFixture{}.GetObject(7)==7);
    // Aggregate wall time never substitutes unavailable/reversed thread time.
    RainWaterCost::Accum a;
    RainWaterCost::RecordAdvance(a,1000000,{10,true},{1000010,true},0);
    RainWaterCost::RecordAdvance(a,75000000,{}, {},2);
    RainWaterCost::RecordAdvance(a,2000000,{90,true},{89,true},1);
    assert(a.advances==3 && a.steps==3 && a.noStepAdvances==1 && a.multiStepAdvances==1);
    assert(a.slowAdvances==1 && a.cpuSamples==1 && a.cpuMs==1 && a.wallMs==78);
    assert(a.maxWallMs==75 && a.cpuAtMaxWallMs==-1 && a.stepsAtMaxWall==2);
    RainWaterCost::RecordPhase(a,RainWaterCost::Phase::RawTransfer,1000000);
    RainWaterCost::RecordPhase(a,RainWaterCost::Phase::RawTransfer,3000000);
    RainWaterCost::RecordPhase(a,RainWaterCost::Phase::Count,9999999);
    assert(a.phaseCalls[2]==2 && a.phaseMs[2]==4 && a.phaseMaxMs[2]==3);

    RainWaterCost::Reset();
    RainWaterField field;
    assert(field.Configure(3,3,2,std::vector<float>(9,10)));
    field.Advance(.125f,0);field.Advance(.125f,0); // Actual one empty solver step.
    assert(field.AddWater(1,1,.1f));
    field.Advance(.5f,.7f); // Actual two wet steps, same call.
    field.Advance(0,1); // Rejected input has no coarse timing scope.
    const auto& c=RainWaterCost::Costs();
    if(RainWaterCost::Enabled()) {
        assert(c.advances==3 && c.steps==3 && c.emptySteps==1);
        assert(c.noStepAdvances==1 && c.multiStepAdvances==1);
        assert(c.phaseCalls[0]==3 && c.phaseCalls[1]==3 && c.phaseCalls[4]==3);
        assert(c.phaseCalls[2]==2 && c.phaseCalls[3]==2); // Empty shortcut preserved.
        assert(c.phaseCalls[5]==0); // spacing2 retains the original limiter path.
        assert(c.wallMs>=0 && c.maxWallMs>=0);
#ifdef _WIN32
        assert(c.cpuSamples==3); // Real calling-thread API, no emulation.
#endif
    } else {
        assert(c.advances==0 && c.steps==0 && c.wallMs==0 && c.cpuSamples==0);
        for(auto calls:c.phaseCalls)assert(calls==0);
    }
    RainWaterCost::Reset();
    RainWaterField native;assert(native.Configure(3,3,25,std::vector<float>(9,10)));
    assert(native.AddWater(1,1,.08f));native.Advance(.25f,.8f);
    const auto& fused=RainWaterCost::Costs();
    if(RainWaterCost::Enabled()) {
        assert(fused.steps==1&&fused.emptySteps==0);
        assert(fused.phaseCalls[0]==1&&fused.phaseCalls[1]==1&&fused.phaseCalls[4]==1);
        assert(fused.phaseCalls[2]==0&&fused.phaseCalls[3]==0&&fused.phaseCalls[5]==1);
        assert(std::strcmp(RainWaterCost::Name(RainWaterCost::Phase::FusedFlux),"fused-flux")==0);
    } else for(auto calls:fused.phaseCalls)assert(calls==0);
    RainWaterCost::Reset();assert(RainWaterCost::Costs().advances==0);
    std::cout << "rain water phase accounting passed; diagnostic=" << RainWaterCost::Enabled() << '\n';
}
