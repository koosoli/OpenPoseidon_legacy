#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace Poseidon;
static constexpr int range=1028,side=258;
// Deliberately synthetic unit source. Actual retail inputs are tested separately
// below and must never be labelled an installed subdivided heightfield.
struct Source {
    uint64_t revision=1;size_t reads=0;
    float Sample(int x,int z) {assert(x>=0&&z>=0&&x<range&&z<range);++reads;return 10;}
    void Refresh(RainWaterField& f) {
        f.UpdateTerrainSource(this,range,6.25f,revision,true,[this](int x,int z){return Sample(x,z);});
    }
    RainWaterField::NaturalUpdate Natural(RainWaterField& f,float seconds,bool enabled=true) {
        return f.UpdateNaturalTerrain(this,range,6.25f,revision,enabled,seconds,
            [this](int x,int z){return Sample(x,z);});
    }
};
static void mass(const RainWaterField& f,double seed) {
    const auto b=f.WaterBudget();
    assert(std::abs(f.Volume()-(seed+b.rain-b.infiltration-b.evaporation-b.outlet))<1e-7*std::max(1.0,seed));
}
static RainWaterField configure() {
    RainWaterField f;assert(f.Configure(side,side,25,std::vector<float>(side*side,10)));return f;
}
int main(int argc,char** argv) {
    // A parent's high children stay dry; equal-share refinement would invent
    // sixteen independent pools, including perched water on those high beds.
    std::array<RainWaterCellGeometry,16> g;
    for(size_t i=0;i<g.size();++i)g[i]=RainWaterCellGeometry::Flat(i<8?10:20);
    std::array<double,16> v;
    assert(RainWaterReconstruct(g,100,39.0625,v));
    double total=0;for(size_t i=0;i<v.size();++i){total+=v[i];assert(std::abs(v[i]-(i<8?12.5:0))<1e-9);}
    assert(total==100);
    assert(RainWaterReconstruct(g,1e-30,39.0625,v));
    total=0;for(double value:v)total+=value;assert(total==1e-30);
    assert(RainWaterReconstruct(g,0,39.0625,v));for(double value:v)assert(value==0);
    g[0]=RainWaterCellGeometry::Flat(std::numeric_limits<float>::quiet_NaN());
    assert(!RainWaterReconstruct(g,1,39.0625,v));

    RainWaterFine perched;std::vector<float> parentBed(8*8,10);
    assert(perched.Configure(8,8,25,parentBed));assert(perched.AddWater(10,10,100));
    std::vector<RainWaterCellGeometry> ridge;
    const auto height=[](int x){return x<2?10.f:20.f;};
    for(int z=0;z<32;++z)for(int x=0;x<32;++x)
        ridge.emplace_back(height(x),height(x+1),height(x),height(x+1));
    assert(perched.RefineTileGeometry(0,0,ridge));
    const auto native=perched.FineSnapshot();double parentVolume=0,wetHead=-1;
    for(size_t i=0;i<16;++i) {
        const auto& cell=native[i];parentVolume+=cell.owner.volume;
        if(i%4>=2)assert(cell.owner.volume==0);
        if(cell.owner.volume>1e-8) {
            if(wetHead<0)wetHead=cell.owner.Head();
            assert(std::abs(cell.owner.Head()-wetHead)<1e-7);
            assert(std::abs(cell.geometry.Volume(cell.owner.Head(),39.0625)-cell.owner.volume)<1e-7);
        }
    }
    assert(std::abs(parentVolume-100)<1e-10&&std::abs(perched.Volume()-100)<1e-10);
    auto shifted=ridge;for(auto& cell:shifted)cell=RainWaterCellGeometry::Flat(15);
    assert(perched.RefineTileGeometry(0,0,shifted));
    const auto retained=perched.FineSnapshot();
    for(size_t i=0;i<native.size();++i)assert(native[i].owner.volume==retained[i].owner.volume);

    // True dryness and storage reuse, including a tiny residual that must NOT
    // count as drained. Repeated cycles do not append abandoned child arrays.
    RainWaterFine core;std::vector<float> bed(16*8,10);
    assert(core.Configure(16,8,25,bed));assert(core.SetTileLimit(1));
    std::vector<RainWaterCellGeometry> flat(1024,RainWaterCellGeometry::Flat(10));
    size_t slots=0;
    for(int cycle=0;cycle<100;++cycle) {
        assert(core.RefineTileGeometry(cycle%2,0,flat));
        if(!cycle)slots=core.StorageOwnerSlots();
        assert(core.StorageOwnerSlots()==slots);
        assert(core.CoarsenDryTile(cycle%2,0,bed));assert(core.TileCount()==0);
    }
    assert(core.RefineTileGeometry(0,0,flat));assert(core.AddWater(10,10,1e-30));
    assert(!core.CoarsenDryTile(0,0,bed)&&core.Volume()==1e-30);

    auto f=configure();Source source;source.Refresh(f);
    assert(!f.FineActive());assert(f.AddWater(20,20,.1f));const double seed=f.Volume();
    const auto before=f.Snapshot();const auto generation=f.Generation();
    assert(source.Natural(f,0).admitted==0&&f.Snapshot()==before&&f.Generation()==generation);
    assert(source.Natural(f,5,false).admitted==0&&source.reads==0);
    const auto promoted=source.Natural(f,5);
    assert(promoted.admitted==1&&promoted.examined==1&&promoted.valid&&source.revision==1);
    assert(source.reads==33*33&&f.FineTileCount()==1&&f.FineSnapshot().valid);
    assert(f.FineSnapshot().sourceRevision==1&&f.Volume()==seed);mass(f,seed);
    const auto frozen=f.Snapshot();const auto frozenRevision=f.Revision();
    f.At(500,500);f.At(5000,5000);assert(source.Natural(f,0).admitted==0);
    assert(f.Snapshot()==frozen&&f.Revision()==frozenRevision);
    // An unknown actual revision cannot be admitted by a scheduler alone.
    ++source.revision;const auto reads=source.reads;
    assert(!source.Natural(f,5).valid&&source.reads==reads&&f.Volume()==seed);
    source.Refresh(f);mass(f,seed);

    auto divided=configure();Source partitions;partitions.Refresh(divided);
    assert(divided.AddWater(20,20,.1f));
    for(int tick=0;tick<19;++tick)assert(partitions.Natural(divided,.25f).admitted==0);
    assert(partitions.reads==0&&partitions.Natural(divided,.25f).admitted==1);
    assert(divided.Snapshot()==f.Snapshot());
    const auto matching=divided.FineSnapshot();
    assert(matching.surfaces.size()==f.FineSnapshot().surfaces.size());
    int wrongOwner=0;
    assert(!divided.UpdateNaturalTerrain(&wrongOwner,range,6.25f,1,true,5,
        [](int,int){assert(false);return 0.f;}).valid);

    for(const float rejectedHeight: {0.f,std::numeric_limits<float>::quiet_NaN()}) {
        auto closed=configure();Source missing;missing.Refresh(closed);
        assert(closed.AddWater(20,20,.1f));const auto held=closed.Volume();
        size_t badReads=0;
        const auto refuse=[&](int,int){++badReads;return rejectedHeight;};
        const auto invalid=closed.UpdateNaturalTerrain(&missing,range,6.25f,1,true,5,refuse);
        assert(invalid.refused==1&&invalid.admitted==0&&badReads==1&&closed.Volume()==held);
        const auto cached=closed.UpdateNaturalTerrain(&missing,range,6.25f,1,true,5,refuse);
        assert(cached.examined==0&&badReads==1&&!closed.FineActive());
        ++missing.revision;missing.Refresh(closed);
        assert(missing.Natural(closed,5).admitted==1&&closed.Volume()==held);
    }
    auto border=configure();Source edge;edge.Refresh(border);
    assert(border.AddWater(side-1,side-1,.1f));
    assert(edge.Natural(border,5).admitted==0&&edge.reads==0);

    auto uniform=configure();Source film;film.Refresh(uniform);
    for(int tick=0;tick<140;++tick)uniform.Advance(.25f,1);
    const auto filmVolume=uniform.Volume();assert(filmVolume>0);
    assert(film.Natural(uniform,5).admitted==0&&film.reads==0&&uniform.Volume()==filmVolume);

    // The first eight stable wet candidates saturate ownership; a ninth cannot
    // evict them, even when queries move to it. Ordering is deterministic.
    auto capped=configure();Source many;many.Refresh(capped);
    for(int tile=2;tile<=10;++tile)assert(capped.AddWater(tile*8+3,20,.1f));
    const double cappedSeed=capped.Volume();
    for(int n=0;n<8;++n)assert(many.Natural(capped,5).admitted==1);
    assert(capped.FineTileCount()==8&&many.Natural(capped,100).admitted==0);
    capped.At(2100,500);assert(capped.FineTileCount()==8);mass(capped,cappedSeed);

    // A low-volume natural owner drains via real solver sinks, then only its
    // exact emptiness + simulation-clock quiet interval can release the slot.
    auto drained=configure();Source dry;dry.Refresh(drained);
    assert(drained.AddWater(20,20,.0021f));const double drySeed=drained.Volume();
    assert(dry.Natural(drained,5).admitted==1);
    for(int n=0;n<4000&&drained.Volume()!=0;++n) {
        drained.Advance(.25f,0,1,1);
        if(drained.Volume()!=0)dry.Natural(drained,.25f);
    }
    assert(drained.Volume()==0&&drained.FineTileCount()==1);
    // A long first empty observation is not proof that it was empty for the
    // whole tick; retain it until a separately observed quiet interval passes.
    assert(dry.Natural(drained,100).retired==0);
    assert(dry.Natural(drained,9).retired==0);
    // Retirement executes on the next five-second scheduling boundary.
    const auto quiet=dry.Natural(drained,1);
    if(!quiet.retired)assert(dry.Natural(drained,5).retired==1);
    assert(drained.FineTileCount()==0&&drained.FineActive()&&drained.SourceReady());
    mass(drained,drySeed);
    assert(drained.AddWater(28,20,.05f));
    assert(dry.Natural(drained,5).admitted==1);mass(drained,drySeed+.05*625);
    drained.Reset();assert(drained.FineTileCount()==0&&!drained.FineActive());

    // Cached retail FMD1 inputs are authentic ORIGINAL 256-square 50m geometry.
    // They exercise actual authored triangles and no-edit refusal of the native
    // 6.25m publication layout; no interpolation is called a native game dump.
    for(int arg=1;arg<argc;++arg) {
        std::ifstream input(argv[arg],std::ios::binary);assert(input.good());
        char magic[4];input.read(magic,4);
        if(std::string(magic,4)=="RWNT") {
            uint32_t nativeRange=0;uint64_t revision=0;float spacing=0,x=0,z=0,sea=0;
            input.read(reinterpret_cast<char*>(&nativeRange),4);input.read(reinterpret_cast<char*>(&revision),8);
            input.read(reinterpret_cast<char*>(&spacing),4);input.read(reinterpret_cast<char*>(&x),4);
            input.read(reinterpret_cast<char*>(&z),4);input.read(reinterpret_cast<char*>(&sea),4);
            assert(nativeRange==2048&&spacing==6.25f&&revision>0);
            std::array<float,33*33> heights;
            input.read(reinterpret_cast<char*>(heights.data()),33*33*4);assert(input.gcount()==33*33*4);
            std::vector<RainWaterCellGeometry> actual;std::vector<float> centres;
            for(int iz=0;iz<32;++iz)for(int ix=0;ix<32;++ix) {
                const size_t i=iz*33+ix;
                actual.emplace_back(heights[i],heights[i+1],heights[i+33],heights[i+34]);
                assert(actual.back().Finite());
            }
            for(int iz=0;iz<8;++iz)for(int ix=0;ix<8;++ix)centres.push_back(heights[(iz*4+2)*33+ix*4+2]);
            RainWaterFine replay;assert(replay.Configure(8,8,25,centres,x,z,sea));
            for(int tick=0;tick<80;++tick)replay.Advance(.25,1);
            const double retained=replay.Volume();const auto budget=replay.WaterBudget();
            assert(retained>0&&replay.RefineTileGeometry(0,0,actual));
            assert(std::abs(replay.Volume()-retained)<1e-8&&replay.WaterBudget().rain==budget.rain);
            const auto cells=replay.FineSnapshot();assert(cells.size()==1024);
            for(const auto& cell:cells) {
                const int ix=int((cell.owner.x-x)/spacing),iz=int((cell.owner.z-z)/spacing);
                assert(cell.geometry.Corner00()==heights[iz*33+ix]);
                assert(cell.geometry.Corner11()==heights[(iz+1)*33+ix+1]);
                assert(std::isfinite(cell.owner.Head())&&cell.owner.volume>=0);
            }
            std::printf("Actual unedited native33x33/6.25m geometry, CPU solver rain and conservative local refinement: %s origin %.2f/%.2f revision %llu volume %.9f\n",
                argv[arg],x,z,static_cast<unsigned long long>(revision),retained);
            if(std::floor((x+12.5)/200)==(x+12.5)/200&&std::floor((z+12.5)/200)==(z+12.5)/200) {
                // Complete production-aligned native tile, with uncaptured
                // surroundings explicitly EXCLUDED by a synthetic sea mask.
                // This is bounded source/selector replay, not world hydrology.
                const int width=RainWaterField::AlignedSourceGrid(int(nativeRange),spacing).side;
                const int tx=int((x+12.5)/200),tz=int((z+12.5)/200);
                std::vector<float> masked(width*width,sea);
                for(int iz=0;iz<8;++iz)for(int ix=0;ix<8;++ix)
                    masked[(tz*8+iz)*width+tx*8+ix]=centres[iz*8+ix];
                RainWaterField bounded;assert(bounded.Configure(width,width,25,masked,0,0,sea));
                size_t nativeReads=0;
                const auto exact=[&](int ix,int iz) {
                    const int cx=ix-int(x/spacing),cz=iz-int(z/spacing);
                    assert(cx>=0&&cx<33&&cz>=0&&cz<33);++nativeReads;
                    return heights[cz*33+cx];
                };
                bounded.UpdateTerrainSource(&input,int(nativeRange),spacing,revision,true,exact);
                size_t promoted=0;
                for(int tick=0;tick<80;++tick) {
                    const auto stepVolume=bounded.Volume();
                    const auto stepBudget=bounded.WaterBudget();
                    const bool doubleOwners=bounded.FineActive();
                    bounded.Advance(.25f,1);
                    const auto preVolume=bounded.Volume();
                    const auto preBudget=bounded.WaterBudget();
                    // The legacy solver stores float depths before promotion.
                    // Test double-owner conservation independently, so inherited
                    // pre-promotion roundoff cannot mask a new transfer loss.
                    if(doubleOwners) {
                        const double net=(preBudget.rain-stepBudget.rain)-
                            (preBudget.infiltration-stepBudget.infiltration)-
                            (preBudget.evaporation-stepBudget.evaporation)-
                            (preBudget.outlet-stepBudget.outlet);
                        assert(std::abs(preVolume-stepVolume-net)<1e-8);
                    }
                    const auto admission=bounded.UpdateNaturalTerrain(&input,int(nativeRange),spacing,revision,true,.25f,exact);
                    assert(std::abs(bounded.Volume()-preVolume)<1e-9);
                    assert(admission.valid);promoted+=admission.admitted;
                }
                const auto b=bounded.WaterBudget();
                const double residual=bounded.Volume()-(b.rain-b.infiltration-b.evaporation-b.outlet);
                assert(std::abs(residual)<8*std::numeric_limits<float>::epsilon()*b.rain);
                assert(promoted==1&&nativeReads==1089);
                std::printf("Aligned actual tile bounded no-edit selector replay (synthetic excluded surround): tile%d/%d admitted%zu volume %.9f inherited coarse-float residual %.9g\n",tx,tz,promoted,bounded.Volume(),residual);
            }
            continue;
        }
        assert(std::string(magic,4)=="FMD1");
        input.seekg(4+65536*9);std::vector<float> heights(65536);
        input.read(reinterpret_cast<char*>(heights.data()),65536*4);assert(input.gcount()==65536*4);
        std::array<RainWaterCellGeometry,16> authored;
        for(int z=0;z<4;++z)for(int x=0;x<4;++x) {
            const size_t i=size_t(92+z)*256+98+x;
            authored[z*4+x]={heights[i],heights[i+1],heights[i+256],heights[i+257]};
        }
        assert(RainWaterReconstruct(authored,123.456,50*50,v));
        total=0;for(double value:v){assert(value>=0&&std::isfinite(value));total+=value;}
        assert(std::abs(total-123.456)<1e-10);
        RainWaterField retail;assert(retail.Configure(256,256,50,heights));
        retail.UpdateTerrainSource(&input,256,50,1,true,[&](int x,int z){return heights[z*256+x];});
        retail.Advance(1,1);
        assert(retail.UpdateNaturalTerrain(&input,256,50,1,true,5,[&](int,int){assert(false);return 0.f;}).admitted==0);
        std::printf("Actual original no-edit source triangle/reconstruction and non-native layout refusal: %s\n",argv[arg]);
    }
    std::puts("Natural unchanged-revision selection, conservative reconstruction, dry-only retirement/reuse, saturation, stale source, pause and mass tests passed.");
}
