#include "../../../../engine/Poseidon/World/Weather/RainWaterFine.hpp"
#include <cassert>
#include <cstdio>
using Poseidon::RainWaterFine;
using Poseidon::RainWaterCellGeometry;

// These are actual core updates, not a second implementation of its limiter.
// Balance the unchanged 4um/s infiltration + 2um/s evaporation independently
// of the shared rain calibration for the level-lake controls;
// their individual ledger entries must still be present.
static constexpr double balancedRain=.000006/Poseidon::RainWaterMaximumRainMetresPerSecond;
static void budget(const RainWaterFine& field,double initial) {
    const auto b=field.WaterBudget();
    const double expected=initial+b.rain-b.infiltration-b.evaporation-b.outlet;
    assert(std::abs(field.Volume()-expected)<1e-8*std::max(1.0,initial));
    for(const auto& o:field.Snapshot())assert(o.volume>=0&&std::isfinite(o.Head()));
}
static bool neighbours(const RainWaterFine::Owner& a,const RainWaterFine::Owner& b) {
    const bool x=(a.x+a.size==b.x||b.x+b.size==a.x)&&
        std::max(a.z,b.z)<std::min(a.z+a.size,b.z+b.size);
    const bool z=(a.z+a.size==b.z||b.z+b.size==a.z)&&
        std::max(a.x,b.x)<std::min(a.x+a.size,b.x+b.size);
    return x||z;
}
static void midpointBounds(RainWaterFine& field,double rain=balancedRain,double sun=0,double wind=0) {
    auto before=field.Snapshot();
    const auto geometry=field.FineSnapshot();
    // The limiter bounds flux against heads AFTER local rain and sinks.
    // Compute those preflux volumes in their actual order, including dry
    // exhaustion, then invert the actual native V(H). No solver tolerance or
    // production update is changed; nonzero net forcing is exercised below.
    for(auto& o:before) {
        const double area=o.size*o.size;
        o.volume+=rain*Poseidon::RainWaterMaximumRainMetresPerSecond*RainWaterFine::StepSeconds*area;
        o.volume-=std::min(o.volume,.000004*RainWaterFine::StepSeconds*area);
        o.volume-=std::min(o.volume,(.000002+sun*.000006+wind*.000001)*RainWaterFine::StepSeconds*area);
        o.head=o.bed+o.volume/area;
        for(const auto& s:geometry)if(s.owner.x==o.x&&s.owner.z==o.z&&s.owner.size==o.size) {
            assert(s.geometry.Head(o.volume,area,o.head,s.owner.Head()));
            break;
        }
    }
    field.Advance(RainWaterFine::StepSeconds,rain,sun,wind);
    for(const auto& o:before) {
        double lo=o.Head(),hi=o.Head();
        for(const auto& n:before)if(neighbours(o,n)) {
            lo=std::min(lo,n.Head());hi=std::max(hi,n.Head());
        }
        const double h=field.At(o.x+o.size*.5,o.z+o.size*.5).Head();
        assert(h>=(o.Head()+lo)*.5-1e-8);
        assert(h<=(o.Head()+hi)*.5+1e-8);
    }
}
static RainWaterFine small(std::span<const RainWaterCellGeometry> geometry) {
    RainWaterFine field;
    assert(field.Configure(2,2,25,std::vector<float>(4,10)));
    assert(field.RefineTileGeometry(0,0,geometry));
    return field;
}
int main() {
    constexpr double area=6.25*6.25;
    const std::vector<RainWaterCellGeometry> flat(64,RainWaterCellGeometry::Flat(10));
    auto checker=small(flat);
    for(int z=0;z<8;++z)for(int x=0;x<8;++x)
        assert(checker.AddWater((x+.5)*6.25,(z+.5)*6.25,area*(2+((x+z)%2?.04:-.04))));
    const double initial=checker.Volume();
    checker.Advance(.25,balancedRain);
    // Four fully wet incident faces used to flip 12.04 <-> 11.96 forever.
    // Interior endpoints all have four equal faces: the grid mode must damp.
    double residual=0;
    for(int z=2;z<6;++z)for(int x=2;x<6;++x)
        residual=std::max(residual,std::abs(checker.At((x+.5)*6.25,(z+.5)*6.25).Head()-12));
    std::printf("actual interior checker residual after one step: %.12g m\n",residual);
    std::fflush(stdout);
    assert(residual<.004); // at least 90% damping; cannot accept a sign flip
    budget(checker,initial);
    assert(checker.WaterBudget().rain>0&&checker.WaterBudget().infiltration>0&&checker.WaterBudget().evaporation>0);
    for(int step=0;step<12;++step)midpointBounds(checker);
    budget(checker,initial);

    // A level lake over exact sloping native triangles remains at rest.
    std::vector<RainWaterCellGeometry> slope;
    for(int z=0;z<8;++z)for(int x=0;x<8;++x) {
        const float b=10+.025f*x+.015f*z;
        slope.emplace_back(b,b+.025f,b+.015f,b+.04f);
    }
    auto lake=small(slope);
    for(const auto& s:lake.FineSnapshot())assert(lake.AddWater(s.owner.x+3.125,s.owner.z+3.125,s.geometry.Volume(12,area)));
    const double lakeInitial=lake.Volume();
    for(int step=0;step<8;++step)midpointBounds(lake);
    for(const auto& o:lake.Snapshot())assert(std::abs(o.Head()-12)<1e-9);
    budget(lake,lakeInitial);

    // Partially wet native cells have nonlinear V(H), not full-area capacity.
    std::vector<RainWaterCellGeometry> partial;
    for(int z=0;z<8;++z)for(int x=0;x<8;++x) {
        const float b=10+.02f*x+.03f*z;
        partial.emplace_back(b,b+.02f,b+.03f,b+.05f);
    }
    auto shallow=small(partial);
    for(const auto& s:shallow.FineSnapshot()) {
        const double h=10.25+(((int(s.owner.x/6.25)+int(s.owner.z/6.25))%2)? .012:-.012);
        const double v=s.geometry.Volume(h,area);
        if(v>0)assert(shallow.AddWater(s.owner.x+3.125,s.owner.z+3.125,v));
    }
    const double shallowInitial=shallow.Volume();
    for(int step=0;step<16;++step)midpointBounds(shallow);
    budget(shallow,shallowInitial);
    // The same native midpoint bounds hold when forcing is not balanced:
    // an evaporating shallow field and a raining mixed wet/dry field.
    auto draining=shallow,filling=shallow;
    const double forcingInitial=shallow.Volume();
    for(int step=0;step<8;++step) {
        midpointBounds(draining,.05,.5,.25);
        midpointBounds(filling,1,.5,.25);
    }
    budget(draining,forcingInitial);budget(filling,forcingInitial);
    auto shallowLake=small(partial);
    for(const auto& s:shallowLake.FineSnapshot()) {
        const double v=s.geometry.Volume(10.25,area);
        if(v>0)assert(shallowLake.AddWater(s.owner.x+3.125,s.owner.z+3.125,v));
    }
    const auto lakeBefore=shallowLake.Snapshot();const double shallowLakeInitial=shallowLake.Volume();
    for(int step=0;step<8;++step)midpointBounds(shallowLake);
    for(const auto& o:lakeBefore) {
        const auto after=shallowLake.At(o.x+3.125,o.z+3.125);
        if(o.volume>0)assert(std::abs(after.Head()-o.Head())<1e-9);
        // Balanced decimal rates can leave sub-ULP input in an empty triangle.
        // Its cubic inverse is poorly conditioned at zero; inspect retained
        // mass instead of demanding an exactly unchanged dry minimum head.
        else assert(after.volume<1e-12);
    }
    budget(shallowLake,shallowLakeInitial);

    // Actual shared face above both heads: a wet depression cannot tunnel
    // through its native rim into the next depression.
    auto blockedGeometry=flat;
    for(int z=0;z<8;++z) {
        blockedGeometry[z*8+3]=RainWaterCellGeometry(10,14,10,14);
        blockedGeometry[z*8+4]=RainWaterCellGeometry(14,10,14,10);
    }
    auto blocked=small(blockedGeometry);
    for(const auto& s:blocked.FineSnapshot())if(s.owner.x<25)
        assert(blocked.AddWater(s.owner.x+3.125,s.owner.z+3.125,s.geometry.Volume(12,area)));
    const double blockedInitial=blocked.Volume();
    for(int step=0;step<8;++step)midpointBounds(blocked);
    for(const auto& s:blocked.FineSnapshot())if(s.owner.x>=25)assert(s.owner.volume<1e-12);
    budget(blocked,blockedInitial);

    // Clipped owner topology: one coarse corner has EIGHT actual fine edges,
    // four per face, of unequal cell areas (625 versus 39.0625 m2).
    RainWaterFine mixed;
    assert(mixed.Configure(9,9,25,std::vector<float>(81,10)));
    assert(mixed.RefineTileGeometry(0,0,std::vector<RainWaterCellGeometry>(1024,RainWaterCellGeometry::Flat(10))));
    assert(mixed.RefineTileGeometry(1,0,std::vector<RainWaterCellGeometry>(128,RainWaterCellGeometry::Flat(10))));
    assert(mixed.RefineTileGeometry(0,1,std::vector<RainWaterCellGeometry>(128,RainWaterCellGeometry::Flat(10))));
    for(const auto& o:mixed.Snapshot())assert(mixed.AddWater(o.x+o.size*.5,o.z+o.size*.5,o.size*o.size*(o.size==25?2.08:2)));
    const auto coarseBefore=mixed.At(212.5,212.5);int incident=0;
    for(const auto& o:mixed.Snapshot())if(neighbours(coarseBefore,o))++incident;
    assert(incident==8);
    const double mixedInitial=mixed.Volume();auto reversed=mixed;
    midpointBounds(mixed);
    assert(mixed.At(212.5,212.5).Head()<coarseBefore.Head());
    for(int i=0;i<4;++i) {
        assert(mixed.At(196.875,203.125+i*6.25).Head()>12);
        assert(mixed.At(203.125+i*6.25,196.875).Head()>12);
    }
    // Reverse the physical direction; all four subfaces must exchange too.
    for(const auto& o:reversed.Snapshot())if(o.size==6.25)
        assert(reversed.AddWater(o.x+3.125,o.z+3.125,area*.16));
    const double reversedInitial=reversed.Volume();
    midpointBounds(reversed);budget(reversed,reversedInitial);
    assert(reversed.At(212.5,212.5).Head()>coarseBefore.Head());
    for(int i=0;i<4;++i) {
        assert(reversed.At(196.875,203.125+i*6.25).Head()<12.16);
        assert(reversed.At(203.125+i*6.25,196.875).Head()<12.16);
    }
    for(int step=0;step<4;++step)midpointBounds(mixed);
    budget(mixed,mixedInitial);

    // Partitions, paused/source-deferred work and source edits preserve history.
    auto whole=shallow,partition=shallow;
    whole.Advance(1,balancedRain);for(int n=0;n<60;++n)partition.Advance(1.0/60,balancedRain);
    const auto a=whole.Snapshot(),b=partition.Snapshot();
    assert(whole.Revision()==partition.Revision());
    for(size_t i=0;i<a.size();++i)assert(std::abs(a[i].volume-b[i].volume)<1e-10);
    const double retained=partition.Volume();const auto history=partition.WaterBudget();const auto revision=partition.Revision();
    partition.Advance(0,1);partition.Defer(.5);
    assert(partition.Volume()==retained&&partition.Revision()==revision&&partition.WaterBudget().rain==history.rain);
    assert(partition.RefineTileGeometry(0,0,partial)&&partition.Volume()==retained);
    auto invalid=partial;invalid[0]=RainWaterCellGeometry::Flat(std::numeric_limits<float>::quiet_NaN());
    assert(!partition.RefineTileGeometry(0,0,invalid)&&partition.Volume()==retained);

    // Sea remains an actual sink, counted exactly in the existing outlet ledger.
    auto seaGeometry=flat;seaGeometry[0]=RainWaterCellGeometry::Flat(-1);
    auto sea=small(seaGeometry);assert(sea.AddWater(3.125,3.125,1));
    sea.Advance(.25,0);assert(sea.WaterBudget().outlet>=1);budget(sea,1);
    std::puts("Actual checker damping, native midpoint bounds, level lake, partial wetting, dry face, eight-edge coarse/fine, source, sea, mass and partition checks passed.");
}
