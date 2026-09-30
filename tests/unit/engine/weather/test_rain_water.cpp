#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include <cassert>
#include <cmath>
#include <limits>
using Poseidon::RainWaterField;
static void advance(RainWaterField& f,float duration,float rain,float sun=0) {
    for(int i=0;i<int(duration*4);++i) f.Advance(.25f,rain,sun);
}
int main() {
    RainWaterField bowl;
    assert(bowl.Configure(9,9,1,std::vector<float>(81,10),0,0,0));
    for(int z=1;z<8;++z) for(int x=1;x<8;++x) {
        float radius=std::hypot(float(x-4),float(z-4));
        assert(bowl.SetBed(x,z,9+std::min(radius/3,1.0f)));
    }
    advance(bowl,120,1);
    assert(bowl.At(4,4).depth>.08f); // catchment accumulates in the actual hollow
    assert(bowl.At(4,4).depth>bowl.At(0,0).depth*4);
    const auto budget=bowl.WaterBudget();
    assert(std::abs(bowl.Volume()-(budget.rain-budget.infiltration-budget.evaporation-budget.outlet))<.00002);
    const double retained=bowl.Volume(); const auto revision=bowl.Revision();
    bowl.Advance(0,0,1); bowl.At(2,3);
    assert(bowl.Volume()==retained && bowl.Revision()==revision);
    advance(bowl,120,0);
    assert(bowl.Volume()<retained && bowl.Volume()>retained*.8); // gradual drainage
    auto sunny=bowl,shady=bowl;
    advance(sunny,120,0,1); advance(shady,120,0,0);
    assert(sunny.Volume()<shady.Volume());

    RainWaterField ramp;
    std::vector<float> bed(45);
    for(int z=0;z<5;++z) for(int x=0;x<9;++x) bed[z*9+x]=float(8-x)+1;
    assert(ramp.Configure(9,5,1,bed));
    assert(ramp.AddWater(1,2,.2f));
    advance(ramp,1,0);
    assert(ramp.At(1,2).flowX>0 && ramp.At(2,2).depth>0);
    assert(ramp.At(0,2).depth==0); // no uphill transport
    advance(ramp,20,0);
    assert(ramp.At(8,2).depth>ramp.At(1,2).depth); // collects at valley bottom
    assert(ramp.At(1,2).depth>=0);

    RainWaterField spill;
    assert(spill.Configure(7,3,1,std::vector<float>(21,3)));
    assert(spill.SetBed(1,1,1)); assert(spill.AddWater(1,1,.5f));
    advance(spill,2,0);
    assert(spill.At(2,1).depth==0); // below rim cannot overflow
    assert(spill.SetBed(2,1,1.2f)); // excavation opens a physical outlet
    advance(spill,2,0);
    assert(spill.At(2,1).depth>0);

    RainWaterField sea;
    assert(sea.Configure(3,3,1,{1,1,1,1,1,-1,1,1,1}));
    assert(sea.AddWater(1,1,.1f)); advance(sea,2,0);
    assert(sea.WaterBudget().outlet>0 && sea.At(2,1).depth==0);
    assert(!sea.At(-1,0).valid && !sea.At(std::numeric_limits<float>::infinity(),0).valid);
    const double old=sea.Volume(); sea.Advance(std::numeric_limits<float>::quiet_NaN(),1);
    assert(sea.Volume()==old);

    RainWaterField single, partitioned;
    assert(single.Configure(5,5,2,std::vector<float>(25,10)));
    partitioned=single; single.Advance(4,1,.7f);
    for(int i=0;i<240;++i) partitioned.Advance(1.0f/60,1,.7f);
    assert(single.Revision()==partitioned.Revision());
    assert(std::abs(single.Volume()-partitioned.Volume())<1e-7);
    const auto generation=single.Generation();
    single.Reset(); assert(!single.At(0,0).valid && single.Volume()==0);
    assert(single.Generation()>generation);
    const auto resetGeneration=single.Generation();
    assert(single.Configure(5,5,2,std::vector<float>(25,10)));
    assert(single.Generation()>resetGeneration && single.Revision()==0);
}
