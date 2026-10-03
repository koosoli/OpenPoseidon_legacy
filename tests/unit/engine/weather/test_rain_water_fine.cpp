#include "../../../../engine/Poseidon/World/Weather/RainWaterFine.hpp"
#include "../../../../engine/Poseidon/World/Terrain/TerrainCrater.hpp"
#include <cassert>
#include <cstdio>
using Poseidon::RainWaterFine;
static constexpr double eps=1e-9;
static void checkBudget(const RainWaterFine& f,double seed=0) {
    const auto b=f.WaterBudget();
    if(std::abs(f.Volume()-(seed+b.rain-b.infiltration-b.evaporation-b.outlet))>=eps)
        std::fprintf(stderr,"Fine budget residual %.12g m3, volume %.12g, seed %.12g, rain %.12g, revision %llu\n",
            f.Volume()-(seed+b.rain-b.infiltration-b.evaporation-b.outlet),f.Volume(),seed,b.rain,
            static_cast<unsigned long long>(f.Revision()));
    assert(std::abs(f.Volume()-(seed+b.rain-b.infiltration-b.evaporation-b.outlet))<eps);
    for(const auto& o:f.Snapshot()) assert(o.volume>=0&&std::isfinite(o.Head())&&std::isfinite(o.flowX)&&std::isfinite(o.flowZ));
}
static void advance(RainWaterFine& f,double seconds,double rain=0) {
    for(int n=0;n<int(seconds*4);++n) f.Advance(.25,rain);
}
int main() {
    const std::vector<float> flat(16*8,10),fine(32*32,10);
    RainWaterFine f;
    assert(f.Configure(16,8,25,flat));
    assert(f.AddWater(100,100,625));const auto gen=f.Generation();
    const double vol=f.Volume();
    assert(f.RefineTile(0,0,fine));
    assert(f.Generation()==gen&&f.Volume()==vol&&f.OwnerCount()==128+64*15);
    assert(f.TileCount()==1&&std::abs(f.At(100,100).Depth()-1)<eps);
    assert(f.At(100,100).size==6.25&&f.At(201,100).size==25);
    double sumArea=0;for(const auto& o:f.Snapshot())sumArea+=o.size*o.size;
    assert(sumArea==16*8*625); // exclusive ownership: no overlapped coarse rain
    auto same=f;advance(f,1,1);checkBudget(f,625);
    advance(same,1,0);checkBudget(same,625);
    assert(std::abs(f.WaterBudget().rain-16*8*625*Poseidon::RainWaterMaximumRainMetresPerSecond)<eps);
    const double before=f.Volume();const auto rev=f.Revision();
    std::vector<float> changed=fine;changed[16*32+16]=8;
    assert(f.RefineTile(0,0,changed));assert(f.Volume()==before&&f.Revision()>rev);
    assert(!f.SetCoarseBed(0,0,3));
    assert(f.RefineTile(0,0,fine)&&f.Volume()==before); // actual terrain restoration
    const auto snap=f.Snapshot();f.At(10,10);f.At(390,10);f.Advance(0,1);
    assert(f.Volume()==before&&f.TileCount()==1); // neither read nor pause evicts
    assert(!f.RefineTile(-1,0,fine));assert(!f.RefineTile(INT32_MAX,0,fine));
    changed[0]=std::numeric_limits<float>::quiet_NaN();
    assert(!f.RefineTile(1,0,changed)&&f.TileCount()==1&&f.Volume()==before);
    assert(!f.AddWater(10,10,-1)&&!f.AddWater(10,10,std::numeric_limits<double>::infinity()));

    // Bidirectional exchange through a split coarse/fine face. A high flat bed
    // prevents sea outlets; actual wet donor, rather than head alone, is required.
    RainWaterFine cf,fc;
    assert(cf.Configure(16,8,4,std::vector<float>(128,10)));
    assert(cf.RefineTile(0,0,fine));assert(cf.AddWater(32.1,12.1,8));
    fc=cf;assert(fc.RefineTile(1,0,fine)); // same donor refined, fully fine control
    advance(cf,1);checkBudget(cf,8);checkBudget(fc,8);
    assert(cf.At(31.5,12.5).volume>0&&cf.At(32.1,12.1).volume<8);
    RainWaterFine fineToCoarse;
    assert(fineToCoarse.Configure(16,8,4,std::vector<float>(128,10)));
    assert(fineToCoarse.RefineTile(0,0,fine));assert(fineToCoarse.AddWater(31.5,12.5,2));
    advance(fineToCoarse,1);checkBudget(fineToCoarse,2);
    assert(fineToCoarse.At(32.1,12.1).volume>0);
    // Multiple outgoing faces cannot spend one donor more than once.
    RainWaterFine limited;
    assert(limited.Configure(16,8,4,std::vector<float>(128,1)));
    std::vector<float> peak=fine;std::fill(peak.begin(),peak.end(),1);peak[12*32+31]=100;
    assert(limited.RefineTile(0,0,peak));assert(limited.AddWater(31.5,12.5,.001));
    limited.Advance(.25,0);checkBudget(limited,.001);

    // Source-derived triangle crater at native6.25m, deliberately between the
    // old25m sample locations. Fine input evaluates a sampled real-style mesh;
    // it does not directly supply procedural cell-centre bowl elevations.
    constexpr int sourceSide=33;std::vector<float> source(sourceSide*sourceSide,10);
    for(int z=0;z<sourceSide;++z)for(int x=0;x<sourceSide;++x)
        source[z*sourceSide+x]-=Poseidon::TerrainCraterDepth(x*6.25f-112.5f,z*6.25f-112.5f,8,1);
    const auto actualTriangle=[&](double x,double z) {
        const int ix=int(x/6.25),iz=int(z/6.25);const double fx=x/6.25-ix,fz=z/6.25-iz;
        const double a=source[iz*sourceSide+ix],b=source[iz*sourceSide+ix+1];
        const double c=source[(iz+1)*sourceSide+ix],d=source[(iz+1)*sourceSide+ix+1];
        return float(fx+fz<=1?a+(b-a)*fx+(c-a)*fz:d+(c-d)*(1-fx)+(b-d)*(1-fz));
    };
    assert(source[18*sourceSide+18]==9);
    for(int z=0;z<=8;++z)for(int x=0;x<=8;++x)assert(source[z*4*sourceSide+x*4]==10);
    std::vector<float> craterFine(1024);
    for(int z=0;z<32;++z)for(int x=0;x<32;++x)craterFine[z*32+x]=actualTriangle((x+.5)*6.25,(z+.5)*6.25);
    RainWaterFine crater;assert(crater.Configure(8,8,25,std::vector<float>(64,10)));
    assert(crater.RefineTile(0,0,craterFine));advance(crater,120,1);checkBudget(crater);
    const auto deepest=std::min_element(craterFine.begin(),craterFine.end());
    const size_t centreIndex=size_t(deepest-craterFine.begin());
    const auto craterCentre=crater.At((centreIndex%32+.5)*6.25,(centreIndex/32+.5)*6.25);
    assert(craterCentre.bed<9.6f);
    assert(craterCentre.Depth()>crater.At(30,30).Depth()*2);
    assert(craterCentre.Depth()>.03);

    // Fine-to-coarse valley flow and spill: until the actual supplied rim is
    // lowered, no wet donor can escape a deep depression below that rim.
    RainWaterFine spill;assert(spill.Configure(16,8,4,std::vector<float>(128,3)));
    std::vector<float> bowl(1024,3);bowl[12*32+31]=1;
    assert(spill.RefineTile(0,0,bowl));assert(spill.AddWater(31.5,12.5,.5));
    advance(spill,1);assert(spill.At(32.1,12.1).volume==0);checkBudget(spill,.5);
    assert(spill.SetCoarseBed(8,3,1.1f));const double retained=spill.Volume();
    advance(spill,1);assert(spill.At(32.1,12.1).volume>0);checkBudget(spill,.5);
    assert(spill.Volume()<retained); // authored ordinary infiltration/evaporation

    // Adjacent tiles produce the same spatial result when terrain events arrive
    // in opposite order. The edge sweep order is spatial, not map/hash order.
    RainWaterFine first,second;
    assert(first.Configure(16,16,4,std::vector<float>(256,10)));second=first;
    assert(first.RefineTile(0,0,fine)&&first.RefineTile(1,0,fine));
    assert(second.RefineTile(1,0,fine)&&second.RefineTile(0,0,fine));
    assert(first.AddWater(31.5,31.5,1)&&second.AddWater(31.5,31.5,1));
    advance(first,2,1);advance(second,2,1);
    const auto firstOwners=first.Snapshot(),secondOwners=second.Snapshot();
    for(size_t i=0;i<firstOwners.size();++i) {
        assert(firstOwners[i].x==secondOwners[i].x&&firstOwners[i].z==secondOwners[i].z);
        assert(std::abs(firstOwners[i].volume-secondOwners[i].volume)<eps);
    }
    assert(first.At(31.5,32.1).volume>0); // north coarse/fine interface too
    checkBudget(first,1);checkBudget(second,1);

    // Same-footprint regrids conserve overlap volume and history at unequal
    // spacing; changed footprint is rejected transactionally.
    const double oldVolume=f.Volume(),pending=f.PendingSeconds();const auto oldGen=f.Generation();
    assert(f.Regrid(10,5,40,std::vector<float>(50,10)));
    assert(std::abs(f.Volume()-oldVolume)<eps&&f.Generation()>oldGen&&f.TileCount()==0);
    assert(f.PendingSeconds()==pending);checkBudget(f,625);
    assert(f.Regrid(16,8,25,flat));assert(std::abs(f.Volume()-oldVolume)<eps);
    assert(!f.Regrid(16,9,25,std::vector<float>(144,10))&&std::abs(f.Volume()-oldVolume)<eps);
    assert(!f.At(400,0).size&&!f.At(std::numeric_limits<double>::infinity(),0).size);
    RainWaterFine single,partitioned;
    assert(single.Configure(16,8,4,std::vector<float>(128,10)));
    assert(single.RefineTile(0,0,fine));partitioned=single;
    single.Advance(4,1,.7);for(int i=0;i<240;++i)partitioned.Advance(1.0/60,1,.7);
    assert(single.Revision()==partitioned.Revision());
    const auto a=single.Snapshot(),b=partitioned.Snapshot();
    for(size_t i=0;i<a.size();++i)assert(std::abs(a[i].volume-b[i].volume)<eps);
    checkBudget(single);checkBudget(partitioned);
    // Backlog remains bounded per call and is not lost; constant forcing only.
    single.Advance(8,1);assert(single.PendingSeconds()>3.9);
    single.Advance(.25,1);assert(single.PendingSeconds()<.26);
    checkBudget(single);
    RainWaterFine sea;assert(sea.Configure(16,8,4,std::vector<float>(128,1)));
    std::vector<float> coastal(1024,1);coastal[12*32+31]=-1;
    assert(sea.RefineTile(0,0,coastal));assert(sea.AddWater(32.1,12.1,2));
    advance(sea,1);checkBudget(sea,2);assert(sea.WaterBudget().outlet>0);
    RainWaterFine capped;assert(capped.Configure(65,64,4,std::vector<float>(65*64,10)));
    for(int z=0;z<8;++z)for(int x=0;x<8;++x)assert(capped.RefineTile(x,z,fine));
    const auto owners=capped.OwnerCount();
    assert(!capped.RefineTile(8,0,std::vector<float>(4*32,10))&&capped.TileCount()==64&&capped.OwnerCount()==owners);
    assert(capped.RefineTile(0,0,fine)); // full budget still admits existing edits
    assert(capped.AddWater(20,20,1));const double cappedWet=capped.Volume();
    assert(!capped.RefineTile(8,0,std::vector<float>(4*32,10))&&capped.Volume()==cappedWet);
    RainWaterFine capacity;assert(capacity.Configure(513,513,25,std::vector<float>(513*513,10)));
    const size_t baseBytes=capacity.StorageBytes();
    for(int z=0;z<8;++z)for(int x=0;x<8;++x)assert(capacity.RefineTile(x,z,fine));
    std::printf("513-square prototype vector capacities: coarse-only %.2f MiB, 64 fine tiles %.2f MiB (excluding map/snapshot/allocator).\n",
        double(baseBytes)/(1024*1024),double(capacity.StorageBytes())/(1024*1024));
    const auto cappedGen=capped.Generation();capped.Reset();
    assert(capped.Generation()>cappedGen&&capped.TileCount()==0&&!capped.At(0,0).size);
    std::puts("Fine ownership, conservative shared-face flow, real-triangle crater, spill, sea, regrid, budget, reset, pause and partition checks passed.");
}
