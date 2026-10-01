#include "../../../../engine/WgpuRenderer/RainWaterPublication.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace Poseidon;
template<class T> static bool bytes(const std::vector<T>& a,const std::vector<T>& b) {
    return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(T))==0);
}
int main() {
    RainWaterField field;int actualOwner=0;
    assert(field.Configure(513,513,25,std::vector<float>(513*513,10)));
    field.RecordTerrainVertex(&actualOwner,2048,6.25f,82,82,2);
    assert(field.UpdateTerrainSource(&actualOwner,2048,6.25f,2,true,
        [](int x,int z){return x==82&&z==82?9.f:10.f;}).admitted==1);
    assert(field.AddWater(20,20,.04f));
    auto source=field.FineSnapshot();std::vector<WgrRainWaterFineCell> fine;
    assert(source.valid&&PackRainWaterFine(source,size_t(field.Width())*field.Height(),fine)&&fine.size()==1024);
    for(size_t i=0;i<fine.size();++i) {
        assert(fine[i].rect.x==source.surfaces[i].owner.x&&fine[i].rect.y==source.surfaces[i].owner.z&&fine[i].rect.z==6.25f);
        assert(fine[i].rect.w==float(source.surfaces[i].owner.Head()));
        assert(fine[i].corners.y==source.surfaces[i].geometry.Corner10()&&fine[i].corners.z==source.surfaces[i].geometry.Corner01());
        assert(fine[i].parent==source.surfaces[i].parent&&fine[i].reserved[0]==0&&fine[i].flow_depth.w==0);
    }
    WgrRainWaterPublication p{};
    p.coarse.domain={0,0,25,1};p.coarse.control={513,513,.8f,1};p.coarse.generation=field.Generation();
    p.source={uint64_t(reinterpret_cast<uintptr_t>(&actualOwner)),field.Generation(),2,2048,6.25f};
    p.revision=field.Revision();p.flags=WGR_RAIN_WATER_FINE_BACKEND|WGR_RAIN_WATER_SOURCE_READY|WGR_RAIN_WATER_FINE_READY;
    const auto raw=field.Snapshot();std::vector<WgrVec4> coarse(raw.size()/4);
    std::memcpy(coarse.data(),raw.data(),raw.size()*sizeof(float));
    WgrRainWaterPublication pending{};std::vector<WgrVec4> pc;std::vector<WgrRainWaterFineCell> pf;
    AccumulateRainWaterPublication(pending,pc,pf,p,coarse,fine);
    auto cameraOnly=p;cameraOnly.coarse.domain.w=2;cameraOnly.coarse.control.z=.1f;
    assert(SameRainWaterPublication(p,cameraOnly));
    AccumulateRainWaterPublication(pending,pc,pf,cameraOnly,{},{});
    assert(bytes(pc,coarse)&&bytes(pf,fine)&&pending.coarse.domain.w==2&&pending.coarse.control.z==.1f);
    auto next=p;++next.revision;
    AccumulateRainWaterPublication(pending,pc,pf,next,{},{});
    assert(pc.empty()&&pf.empty()); // changed key cannot retain either old slice
    AccumulateRainWaterPublication(pending,pc,pf,next,coarse,fine);
    AccumulateRainWaterPublication(pending,pc,pf,next,{},fine);
    assert(pc.empty()&&bytes(pf,fine)); // missing coarse is forwarded incomplete, never spliced with old bytes
    for(int kind=0;kind<6;++kind) {
        AccumulateRainWaterPublication(pending,pc,pf,p,coarse,fine);
        auto changed=p;
        if(kind==0)++changed.source.world_token;
        if(kind==1)++changed.source.generation;
        if(kind==2)++changed.source.height_revision;
        if(kind==3)++changed.source.terrain_range;
        if(kind==4)changed.source.terrain_spacing=12.5;
        if(kind==5)changed.flags&=~WGR_RAIN_WATER_FINE_READY;
        assert(!SameRainWaterPublication(p,changed));
        AccumulateRainWaterPublication(pending,pc,pf,changed,{},{});
        assert(pc.empty()&&pf.empty());
    }
    AccumulateRainWaterPublication(pending,pc,pf,p,coarse,fine);
    auto stale=p;stale.flags&=~WGR_RAIN_WATER_SOURCE_READY;stale.coarse.control.w=0;
    AccumulateRainWaterPublication(pending,pc,pf,stale,{},{});
    assert(pc.empty()&&pf.empty());
    auto invalid=source;invalid.surfaces.back().parent=513*513;
    assert(!PackRainWaterFine(invalid,513*513,fine)&&fine.empty());
    invalid=source;invalid.surfaces.back().owner.size=25;
    assert(!PackRainWaterFine(invalid,513*513,fine)&&fine.empty());
    invalid=source;invalid.surfaces.back().owner.head=std::numeric_limits<double>::max();
    assert(!PackRainWaterFine(invalid,513*513,fine)&&fine.empty());
    invalid=source;invalid.surfaces.back().geometry=RainWaterCellGeometry(std::numeric_limits<float>::quiet_NaN(),0,0,0);
    assert(!PackRainWaterFine(invalid,513*513,fine)&&fine.empty());
    invalid.surfaces.resize(WGR_RAIN_WATER_FINE_MAX_CELLS+1);
    assert(!PackRainWaterFine(invalid,513*513,fine)&&fine.empty());
    std::puts("Actual fine source packing, ABI layout, paired camera retention, changed/partial-key refusal and stale clears passed.");
}
