#include "../../../../engine/Poseidon/World/Weather/RainWaterFine.hpp"
#include "../../../../engine/Poseidon/World/Terrain/TerrainCrater.hpp"
#include <cassert>
#include <cstdio>
using Poseidon::RainWaterCellGeometry;
using Poseidon::RainWaterTriangle;
using Poseidon::RainWaterFine;
static bool close(double a,double b,double relative=1e-10) {
    return std::abs(a-b)<=relative*std::max({std::abs(a),std::abs(b),1e-30});
}
static void roundtrip(const RainWaterCellGeometry& g,double area) {
    double prev=0;
    std::array<double,5> heads{g.Minimum(),g.Minimum()+1e-5,(g.Minimum()+g.Maximum())*.5,g.Maximum(),g.Maximum()+1};
    std::sort(heads.begin(),heads.end());
    for(const double head:heads) {
        const double v=g.Volume(head,area);assert(v>=prev&&v>=0);prev=v;
        double actual=0;assert(g.Head(v,area,actual));
        assert(close(g.Volume(actual,area),v,1e-7));
        if(v>0)assert(std::abs(actual-head)<1e-8*std::max(1.0,std::abs(head)));
        assert(g.WetArea(head,area)>=0&&g.WetArea(head,area)<=area);
    }
}
int main() {
    const auto flat=RainWaterCellGeometry::Flat(10);
    assert(flat.Volume(9,4)==0&&flat.Volume(11,4)==4);
    double h=0;assert(flat.Head(8,4,h)&&h==12);
    assert(flat.Head(0,4,h)&&h==10);
    // Plane y=x on unit square: exact volume below height h is h²/2.
    const RainWaterCellGeometry slope(0,1,0,1);
    assert(close(slope.Volume(.5,1),.125));assert(close(slope.WetArea(.5,1),.5));
    assert(slope.Head(.125,1,h)&&close(h,.5));
    assert(close(slope.Volume(2,1),1.5));
    // Anti-diagonal ridge: both halves rise from one low corner to height1.
    const RainWaterCellGeometry ridge(0,1,1,0);
    assert(close(ridge.Volume(.5,1),1.0/24));assert(close(ridge.WetArea(.5,1),.25));
    const RainWaterCellGeometry hollow(1,0,0,1);
    assert(close(hollow.Volume(.5,1),5.0/24));assert(close(hollow.WetArea(.5,1),.75));
    assert(hollow.Height(.5,.5)==0); // no invented bilinear saddle
    assert(hollow.Height(0,0)==1&&hollow.Height(1,1)==1);
    for(const auto g:{flat,slope,ridge,hollow,RainWaterCellGeometry(-10000,-9999,-9999,-10000),
        RainWaterCellGeometry(2,2,3,2),RainWaterCellGeometry(2,3,3,3),RainWaterCellGeometry(2,2,2,3),
        RainWaterCellGeometry(0,1,3,2)}) {
        roundtrip(g,1);roundtrip(g,39.0625);
    }
    // Equal-height branches and triangle vertex permutation preserve integration.
    for(const auto t:{RainWaterTriangle(0,0,1),RainWaterTriangle(1,0,0)}) {
        assert(close(t.Volume(.5,1),5.0/24));assert(close(t.WetArea(.5,1),.75));
    }
    for(const auto t:{RainWaterTriangle(0,1,1),RainWaterTriangle(1,1,0)})
        assert(close(t.Volume(.5,1),1.0/24));
    assert(RainWaterTriangle(1,1,1).Volume(2,3)==3);
    // Independent numerical area integral validates both cubic branches for
    // unequal triangle heights. Sampling is test-only, never in the solver.
    const RainWaterTriangle general(0,1,3);constexpr int n=800;
    for(const double head:{.5,1.0,2.0,3.0,4.0}) {
        double integral=0;
        for(int y=0;y<n;++y)for(int x=0;x<n-y;++x) {
            const double a=(x+1.0/3)/n,b=(y+1.0/3)/n;
            integral+=std::max(0.0,head-a-3*b)/(n*n);
        }
        assert(std::abs(general.Volume(head,.5)-integral)<.003);
    }
    assert(RainWaterCellGeometry::FaceDepth(1,2,3)==0);
    assert(close(RainWaterCellGeometry::FaceDepth(.5,0,1),.125));
    assert(RainWaterCellGeometry::FaceDepth(2,0,1)==1.5);
    assert(!flat.Head(-1,1,h)&&!flat.Head(1,0,h)&&!flat.Head(std::numeric_limits<double>::infinity(),1,h));
    const RainWaterCellGeometry invalid(std::numeric_limits<float>::quiet_NaN(),0,0,0);
    assert(!invalid.Finite()&&!invalid.Head(1,1,h));

    // Curved fine owners preserve exact integrated water under source edits.
    RainWaterFine field;assert(field.Configure(16,8,4,std::vector<float>(128,10)));
    RainWaterFine invalidDomain;assert(!invalidDomain.Configure(2,2,1e-200,std::vector<float>(4,10)));
    std::vector<RainWaterCellGeometry> tile(1024,RainWaterCellGeometry::Flat(10));
    tile[12*32+31]=RainWaterCellGeometry(11,10,11,10); // actual inclined face, min10
    assert(field.RefineTileGeometry(0,0,tile));assert(field.AddWater(31.5,12.5,.125));
    const auto wet=field.At(31.5,12.5);assert(close(wet.Head(),10.5));
    assert(close(tile[12*32+31].Volume(wet.Head(),1),wet.volume));
    assert(field.LocalDepth(31.1,12.5)==0&&close(field.LocalDepth(31.9,12.5),.4));
    const auto volume=field.Volume();
    tile[12*32+31]=RainWaterCellGeometry(12,11,12,11);
    assert(field.RefineTileGeometry(0,0,tile)&&field.Volume()==volume);
    assert(close(field.At(31.5,12.5).Head(),11.5));
    tile[0]=invalid;assert(!field.RefineTileGeometry(0,0,tile)&&field.Volume()==volume);

    // A lower coarse centre cannot drain a fine depression through a high
    // physical shared edge. This differs from the old centre-only proposal.
    RainWaterFine spill;assert(spill.Configure(16,8,4,std::vector<float>(128,11)));
    std::fill(tile.begin(),tile.end(),RainWaterCellGeometry::Flat(11));
    tile[12*32+31]=RainWaterCellGeometry(10,11,10,11);
    assert(spill.RefineTileGeometry(0,0,tile));assert(spill.AddWater(31.5,12.5,.125));
    assert(spill.SetCoarseBed(8,3,10));
    spill.Advance(.25,0);assert(spill.At(32.1,12.1).volume==0);
    assert(spill.At(31.5,12.5).Head()<11);
    tile[12*32+31]=RainWaterCellGeometry(10,10,10,10);
    const double retained=spill.Volume();assert(spill.RefineTileGeometry(0,0,tile));
    spill.Advance(.25,0);assert(spill.At(32.1,12.1).volume>0);
    const auto b=spill.WaterBudget();
    assert(std::abs(spill.Volume()-(.125+b.rain-b.infiltration-b.evaporation-b.outlet))<1e-10);
    assert(spill.Volume()<retained);

    // Real source-corner crater now uses integrated triangles, not the old
    // sampled-centre approximation. Every old25m vertex still misses it.
    constexpr int nSource=33;std::vector<float> source(nSource*nSource,10);
    for(int z=0;z<nSource;++z)for(int x=0;x<nSource;++x)
        source[z*nSource+x]-=Poseidon::TerrainCraterDepth(x*6.25f-112.5f,z*6.25f-112.5f,8,1);
    for(int z=0;z<=8;++z)for(int x=0;x<=8;++x)assert(source[z*4*nSource+x*4]==10);
    for(int z=0;z<32;++z)for(int x=0;x<32;++x)
        tile[z*32+x]={source[z*nSource+x],source[z*nSource+x+1],source[(z+1)*nSource+x],source[(z+1)*nSource+x+1]};
    RainWaterFine crater;assert(crater.Configure(8,8,25,std::vector<float>(64,10)));
    assert(crater.RefineTileGeometry(0,0,tile));
    for(int step=0;step<480;++step)crater.Advance(.25,1);
    assert(crater.LocalDepth(112.5,112.5)>.03);
    assert(crater.LocalDepth(112.5,112.5)>crater.LocalDepth(30,30)*2);
    for(const auto& owner:crater.Snapshot()) {
        const size_t ix=size_t(owner.x/6.25),iz=size_t(owner.z/6.25);
        assert(close(tile[iz*32+ix].Volume(owner.Head(),owner.size*owner.size),owner.volume,1e-7));
    }
    const auto budget=crater.WaterBudget();
    assert(std::abs(crater.Volume()-(budget.rain-budget.infiltration-budget.evaporation-budget.outlet))<1e-8);
    auto paused=crater.Snapshot();const auto revision=crater.Revision();
    crater.Advance(0,1);crater.LocalDepth(112.5,112.5);
    assert(crater.Revision()==revision&&crater.Snapshot()[0].volume==paused[0].volume);
    auto single=crater,partitioned=crater;
    single.Advance(4,1,.7);for(int i=0;i<240;++i)partitioned.Advance(1.0/60,1,.7);
    const auto s=single.Snapshot(),p=partitioned.Snapshot();
    assert(single.Revision()==partitioned.Revision());
    for(size_t i=0;i<s.size();++i) {
        assert(close(s[i].volume,p[i].volume,1e-9));
        assert(std::abs(s[i].Head()-p[i].Head())<1e-9);
    }
    // Regrid is total-volume conservative but explicitly assumes uniform
    // source-owner density; it does not preserve curved subcell wet placement.
    const double beforeRegrid=crater.Volume();
    assert(crater.Regrid(4,4,50,std::vector<float>(16,10)));
    assert(std::abs(crater.Volume()-beforeRegrid)<1e-8);
    std::puts("Actual triangle storage, wet-area, inverse head, face spill and retained-volume geometry tests passed.");
}
