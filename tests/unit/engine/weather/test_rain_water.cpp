#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include <cassert>
#include <cmath>
#include <limits>
using Poseidon::RainWaterField;
static void advance(RainWaterField& f,float duration,float rain,float sun=0) {
    for(int i=0;i<int(duration*4);++i) f.Advance(.25f,rain,sun);
}
int main() {
    // Independently specified slider calibration and unchanged sinks. Exercise
    // actual coarse and fully refined finite-volume forcing over equal areas,
    // rather than only checking a shared constant against itself.
    assert(std::abs(Poseidon::RainWaterMaximumRainMetresPerSecond*3600*1000-90)<1e-12);
    RainWaterField calibrated;
    Poseidon::RainWaterFine nativeCalibrated;
    constexpr double calibrationArea=8*8*25*25;
    assert(calibrated.Configure(8,8,25,std::vector<float>(64,10)));
    assert(nativeCalibrated.Configure(8,8,25,std::vector<float>(64,10)));
    assert(nativeCalibrated.RefineTile(0,0,std::vector<float>(1024,10)));
    for(int i=0;i<4;++i) {
        calibrated.Advance(.25f,1,.5f,.25f);
        nativeCalibrated.Advance(.25,1,.5,.25);
    }
    const auto cb=calibrated.WaterBudget();const auto nb=nativeCalibrated.WaterBudget();
    for(const double rain:{cb.rain,nb.rain})assert(std::abs(rain-calibrationArea*.000025)<1e-6);
    for(const double infiltration:{cb.infiltration,nb.infiltration})assert(std::abs(infiltration-calibrationArea*.000004)<1e-6);
    for(const double evaporation:{cb.evaporation,nb.evaporation})assert(std::abs(evaporation-calibrationArea*.00000525)<1e-6);
    assert(std::abs(calibrated.Volume()-nativeCalibrated.Volume())<1e-6);
    assert(std::abs(calibrated.Volume()-(cb.rain-cb.infiltration-cb.evaporation-cb.outlet))<1e-6);
    assert(std::abs(nativeCalibrated.Volume()-(nb.rain-nb.infiltration-nb.evaporation-nb.outlet))<1e-9);
    const auto cRev=calibrated.Revision(),nRev=nativeCalibrated.Revision();
    const double cVolume=calibrated.Volume(),nVolume=nativeCalibrated.Volume();
    calibrated.Advance(0,1,1);nativeCalibrated.Advance(0,1,1);
    assert(calibrated.Revision()==cRev&&nativeCalibrated.Revision()==nRev);
    assert(calibrated.Volume()==cVolume&&nativeCalibrated.Volume()==nVolume);
    calibrated.Advance(.25f,0,1);nativeCalibrated.Advance(.25,0,1);
    assert(calibrated.WaterBudget().rain==cb.rain&&nativeCalibrated.WaterBudget().rain==nb.rain);
    assert(calibrated.Volume()<cVolume&&nativeCalibrated.Volume()<nVolume);
    RainWaterField dry;Poseidon::RainWaterFine dryNative;
    assert(dry.Configure(8,8,25,std::vector<float>(64,10)));
    assert(dryNative.Configure(8,8,25,std::vector<float>(64,10)));
    dry.Advance(.25f,0,1,1);dryNative.Advance(.25,0,1,1);
    assert(dry.Volume()==0&&dryNative.Volume()==0);
    assert(dry.WaterBudget().rain==0&&dry.WaterBudget().infiltration==0&&dry.WaterBudget().evaporation==0);
    assert(dryNative.WaterBudget().rain==0&&dryNative.WaterBudget().infiltration==0&&dryNative.WaterBudget().evaporation==0);
    const auto native=RainWaterField::AlignedSourceGrid(2048,6.25f);
    assert(native.side==513 && native.stride==4 && native.spacing==25);
    const auto stock=RainWaterField::AlignedSourceGrid(256,50);
    assert(stock.side==256 && stock.stride==1 && stock.spacing==50);
    assert(!RainWaterField::AlignedSourceGrid(1,50).side);
    assert(!RainWaterField::AlignedSourceGrid(256,std::numeric_limits<float>::infinity()).side);
    RainWaterField triangle;
    assert(triangle.Configure(2,2,1,{10,10,10,14}));
    assert(triangle.At(.25f,.25f).height==10); // no artificial bilinear saddle
    assert(triangle.At(.75f,.75f).height==12);
    assert(triangle.AddWater(1,1,.4f));
    assert(std::abs(triangle.At(.25f,.25f).depth-.025f)<1e-6f);
    RainWaterField bowl;
    assert(bowl.Configure(9,9,1,std::vector<float>(81,10),0,0,0));
    for(int z=1;z<8;++z) for(int x=1;x<8;++x) {
        float radius=std::hypot(float(x-4),float(z-4));
        assert(bowl.SetBed(x,z,9+std::min(radius/3,1.0f)));
    }
    advance(bowl,600,1); // sustained calibrated rain still fills an actual hollow
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

    // The runtime producer uses this exact predicate before height revision
    // updates. Grid changes must never reuse stale world coordinates/storage.
    RainWaterField domain;
    assert(!domain.MatchesDomain(0,0,1));
    assert(domain.Configure(5,5,2,std::vector<float>(25,10),10,20));
    assert(domain.MatchesDomain(5,5,2,10,20));
    assert(!domain.MatchesDomain(9,9,1,10,20)); // same world extent, new cell area
    assert(!domain.MatchesDomain(5,6,2,10,20));
    assert(!domain.MatchesDomain(6,5,2,10,20));
    assert(!domain.MatchesDomain(5,5,1,10,20));
    assert(!domain.MatchesDomain(5,5,2,11,20));
    assert(!domain.MatchesDomain(5,5,2,10,21));
    assert(!domain.MatchesDomain(5,5,std::numeric_limits<float>::quiet_NaN(),10,20));
    assert(domain.AddWater(2,2,.3f)); domain.Advance(.125f,1);
    const double domainVolume=domain.Volume();
    const auto domainGeneration=domain.Generation();
    // Same-domain actual excavation preserves volume, generation and backlog.
    assert(domain.SetBed(2,2,9));
    assert(domain.MatchesDomain(5,5,2,10,20));
    assert(domain.Volume()==domainVolume && domain.Generation()==domainGeneration);
    assert(domain.PendingSeconds()==.125 && std::abs(domain.At(14,24).height-9.3f)<1e-6f);
    // Explicit runtime regrid policy: retire the old domain before attempting
    // the new configuration, including a failed finite-bed admission.
    domain.Reset();
    assert(!domain.MatchesDomain(5,5,2,10,20) && !domain.At(14,24).valid);
    assert(domain.Generation()>domainGeneration && domain.PendingSeconds()==0);
    std::vector<float> invalidBed(81,10); invalidBed[40]=std::numeric_limits<float>::infinity();
    assert(!domain.Configure(9,9,1,std::move(invalidBed),10,20));
    assert(!domain.At(14,24).valid && domain.Volume()==0);
    assert(domain.Configure(9,9,1,std::vector<float>(81,10),10,20));
    assert(domain.MatchesDomain(9,9,1,10,20) && !domain.MatchesDomain(5,5,2,10,20));
    assert(domain.Volume()==0 && domain.PendingSeconds()==0 && domain.Revision()==0);
    assert(domain.WaterBudget().rain==0 && domain.WaterBudget().outlet==0);
    assert(domain.At(14,24).valid && domain.At(14,24).height==10);
    // The inverse regrid is equally explicit; coordinates outside the newly
    // smaller domain become invalid rather than reading old larger storage.
    assert(domain.AddWater(8,8,.3f));
    domain.Reset();
    assert(domain.Configure(3,3,2,std::vector<float>(9,10),10,20));
    assert(domain.MatchesDomain(3,3,2,10,20) && !domain.At(18,28).valid);
}
