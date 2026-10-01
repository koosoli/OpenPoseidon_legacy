#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include "../../../../engine/Poseidon/World/Terrain/TerrainCrater.hpp"
#include <cassert>
#include <cstdio>
using Poseidon::RainWaterField;
static constexpr int range=1028; // aligned stride4 /258 coarse nodes, native6.25
static constexpr int side=258;
struct Source {
    std::vector<float> heights=std::vector<float>(range*range,10);
    uint64_t revision=1;size_t reads=0;
    float Sample(int x,int z){assert(x>=0&&z>=0&&x<range&&z<range);++reads;return heights[z*range+x];}
    void Edit(RainWaterField& f,int x,int z,float h) {
        if(heights[z*range+x]==h)return;
        heights[z*range+x]=h;
        f.RecordTerrainVertex(this,range,6.25f,x,z,++revision);
    }
    void Crater(RainWaterField& f,int cx,int cz) {
        for(int z=cz-2;z<=cz+2;++z)for(int x=cx-2;x<=cx+2;++x) {
            const float delta=Poseidon::TerrainCraterDepth(float(x-cx)*6.25f,float(z-cz)*6.25f,8,1);
            if(delta>0)Edit(f,x,z,heights[z*range+x]-delta);
        }
    }
    std::vector<float> Coarse() const {
        std::vector<float> bed(side*side,10);
        for(int z=0;z<side;++z)for(int x=0;x<side;++x)
            bed[z*side+x]=(z*4<range&&x*4<range)?heights[z*4*range+x*4]:-100;
        return bed;
    }
    void RefreshCoarse(RainWaterField& f) const {
        const auto bed=Coarse();for(int z=0;z<side;++z)for(int x=0;x<side;++x)assert(f.SetBed(x,z,bed[z*side+x]));
    }
    RainWaterField::FineUpdate Update(RainWaterField& f,bool enabled=true) {
        return f.UpdateTerrainSource(this,range,6.25f,revision,enabled,[this](int x,int z){return Sample(x,z);});
    }
};
static void mass(const RainWaterField& f,double seed=0) {
    const auto b=f.WaterBudget();
    assert(std::abs(f.Volume()-(seed+b.rain-b.infiltration-b.evaporation-b.outlet))<1e-6*std::max(1.0,f.Volume()));
}
int main() {
    // OFF preserves the actual ordinary field, even if real terrain edited.
    RainWaterField off;Source offSource;
    assert(off.Configure(side,side,25,offSource.Coarse()));auto baseline=off;
    offSource.Crater(off,82,82);const auto beforeReads=offSource.reads;
    offSource.Update(off,false);assert(!off.FineActive()&&offSource.reads==beforeReads);
    off.Advance(1,1);baseline.Advance(1,1);
    assert(off.Snapshot()==baseline.Snapshot()&&off.Volume()==baseline.Volume());

    // Genuine before-rain edits survive the first water Configure.
    RainWaterField f;Source source;source.Crater(f,82,82);
    assert(!f.FineActive()&&f.Width()==0);
    assert(f.Configure(side,side,25,source.Coarse()));
    assert(f.AddWater(20,20,.05f));f.Advance(.125f,1);
    const double retained=f.Volume();const auto originalBudget=f.WaterBudget();
    const auto generation=f.Generation();
    const auto admission=source.Update(f);
    assert(admission.native&&!admission.coverageLost&&admission.admitted==1&&admission.valid);
    assert(f.FineTileCount()==1&&f.Generation()>generation&&std::abs(f.Volume()-retained)<1e-9);
    assert(f.PendingSeconds()==.125&&f.WaterBudget().rain==originalBudget.rain);
    const auto publication=f.FineSnapshot();
    assert(publication.valid&&publication.sourceRevision==source.revision&&publication.surfaces.size()==1024);
    // Tile key2 centre mapping: source corner62 *6.25 =387.5, no half-cell shift.
    assert(publication.surfaces.front().owner.x==387.5&&publication.surfaces.front().owner.z==387.5);
    for(const auto& s:publication.surfaces) {
        const int x=int(s.owner.x/6.25),z=int(s.owner.z/6.25);
        assert(s.geometry.Corner00()==source.heights[z*range+x]);
        assert(s.geometry.Corner11()==source.heights[(z+1)*range+x+1]);
    }
    const size_t reads=source.reads;
    for(int tick=0;tick<20;++tick)source.Update(f);
    assert(source.reads==reads); // unchanged revision: no terrain reads or rebuild
    for(int tick=0;tick<240;++tick)f.Advance(.25f,1);
    const float impact=82*6.25f;assert(f.At(impact,impact).valid&&f.At(impact,impact).depth>.03f);
    mass(f,retained);
    const auto frozen=f.Snapshot();const auto fineFrozen=f.FineSnapshot();
    f.At(impact,impact);f.At(5000,5000);f.Advance(0,1);
    assert(f.Snapshot()==frozen&&f.FineSnapshot().revision==fineFrozen.revision);

    // Immediate source invalidation, retained ownership for renderer exclusion,
    // queued simulation time, then successful actual source refresh/restoration.
    const double wetVolume=f.Volume();const auto rev=f.Revision();
    source.Edit(f,82,82,10);
    const auto unchangedTopology=f.Generation();
    assert(!f.SourceReady()&&!f.At(impact,impact).valid&&f.Revision()>rev);
    const auto stale=f.FineSnapshot();assert(!stale.valid&&stale.surfaces.size()==1024);
    f.Advance(.25f,1);assert(f.Volume()==wetVolume&&f.PendingSeconds()>=.25);
    source.RefreshCoarse(f);const auto restoration=source.Update(f);
    assert(restoration.updated==1&&restoration.valid&&f.SourceReady()&&f.Volume()==wetVolume);
    assert(f.Generation()==unchangedTopology);
    assert(f.At(impact,impact).valid);
    f.Advance(.25f,1);mass(f,retained);

    // Missing HeightRevision coverage does not turn a new unknown crater into
    // fine ownership. Existing genuine tiles are boundedly refreshed instead.
    source.heights[146*range+146]=9;++source.revision;
    source.RefreshCoarse(f);const auto gap=source.Update(f);
    assert(gap.coverageLost&&gap.admitted==0&&gap.updated==1&&f.FineTileCount()==1);

    // Eight interior tiles maximum, no camera eviction or wet budget loss.
    for(int tile=3;tile<=9;++tile) {
        source.Edit(f,tile*32+18,82,9);source.RefreshCoarse(f);
        assert(source.Update(f).admitted==1);
    }
    assert(f.FineTileCount()==8);
    const double capped=f.Volume();source.Edit(f,10*32+18,82,9);source.RefreshCoarse(f);
    assert(source.Update(f).refused>0&&f.FineTileCount()==8&&f.Volume()==capped);
    f.At(impact,impact);f.At(6000,6000);assert(f.FineTileCount()==8);

    // Real native corners only: complete edge tile, inland and finite gates.
    RainWaterField edge;Source edgeSource;assert(edge.Configure(side,side,25,edgeSource.Coarse()));
    edgeSource.Edit(edge,2,2,9);assert(edgeSource.Update(edge).refused>0&&!edge.FineActive());
    RainWaterField coast;Source coastSource;assert(coast.Configure(side,side,25,coastSource.Coarse()));
    coastSource.Edit(coast,30,30,-1);coastSource.Edit(coast,38,38,9);
    assert(coastSource.Update(coast).refused>0&&!coast.FineActive());
    RainWaterField overflow;Source overflowSource;assert(overflow.Configure(side,side,25,overflowSource.Coarse()));
    for(int n=0;n<40;++n)overflowSource.Edit(overflow,50+(n%10)*80,50+(n/10)*80,9);
    const auto lost=overflowSource.Update(overflow);assert(lost.coverageLost&&lost.admitted==0&&!overflow.FineActive());
    RainWaterField noJournal;assert(noJournal.Configure(side,side,25,source.Coarse()));
    assert(source.Update(noJournal).admitted==0&&!noJournal.FineActive());
    RainWaterField noOp;Source noOpSource;
    noOp.RecordTerrainUnchanged(&noOpSource,range,6.25f,82,82,++noOpSource.revision);
    assert(noOp.Configure(side,side,25,noOpSource.Coarse()));
    assert(noOpSource.Update(noOp).admitted==0&&!noOp.FineActive());
    noOpSource.Edit(noOp,82,82,9);
    noOp.RecordTerrainUnchanged(&noOpSource,range,6.25f,83,82,++noOpSource.revision);
    noOpSource.Edit(noOp,83,82,9);
    const auto continuous=noOpSource.Update(noOp);
    assert(!continuous.coverageLost&&continuous.admitted==1);
    RainWaterField boundary;Source boundarySource;
    assert(boundary.Configure(side,side,25,boundarySource.Coarse()));
    boundarySource.Edit(boundary,61,82,9);
    assert(boundarySource.Update(boundary).admitted==1&&boundary.FineTileCount()==1);
    const auto firstTopology=boundary.Generation();
    boundarySource.Edit(boundary,62,82,9); // true shared source vertex, both tiles
    assert(boundarySource.Update(boundary).admitted==1&&boundary.FineTileCount()==2);
    assert(boundary.Generation()>firstTopology);

    // Domain regrid retains total mass and budget but explicitly redistributes
    // shoreline and retires fine topology. Unsupported source becomes coarse.
    const auto oldGeneration=f.Generation();const double regridVolume=f.Volume();
    assert(!f.ReconfigureFine(129,129,50,std::vector<float>(129*129,std::numeric_limits<float>::quiet_NaN())));
    assert(!f.SourceReady()&&!f.At(impact,impact).valid&&f.Volume()==regridVolume&&f.FineTileCount()==8);
    const double failedPending=f.PendingSeconds();f.Advance(.125f,1);
    assert(f.Volume()==regridVolume&&f.PendingSeconds()==failedPending+.125);
    assert(source.Update(f).valid&&f.SourceReady()); // actual old-domain recovery
    auto invalidIdentity=f;
    assert(!invalidIdentity.UpdateTerrainSource(nullptr,range,6.25f,source.revision,true,[](int,int){return 10.f;}).valid);
    assert(!invalidIdentity.SourceReady()&&!invalidIdentity.FineSnapshot().valid&&invalidIdentity.Volume()==regridVolume);
    assert(f.ReconfigureFine(129,129,50,std::vector<float>(129*129,10)));
    assert(f.Generation()>oldGeneration&&f.FineTileCount()==0&&std::abs(f.Volume()-regridVolume)<1e-6);
    f.UpdateTerrainSource(&source,257,25,source.revision+1,true,[](int,int){return 10.f;});
    assert(f.SourceReady()&&!f.FineSnapshot().valid&&f.At(100,100).valid);mass(f,retained);
    f.Reset();assert(!f.FineActive()&&f.Width()==0&&f.FineTileCount()==0);
    assert(f.Configure(side,side,25,source.Coarse()));
    assert(source.Update(f).admitted==0); // mission Reset retired all prior edits
    RainWaterField nonfinite;Source bad;assert(nonfinite.Configure(side,side,25,bad.Coarse()));
    bad.Crater(nonfinite,82,82);assert(bad.Update(nonfinite).admitted==1);
    const double held=nonfinite.Volume();const auto goodRevision=bad.revision;
    // Actual publisher skips nonfinite committed vertices, but the revision
    // still reveals the missing source coverage when World next checks it.
    bad.heights[82*range+82]=std::numeric_limits<float>::quiet_NaN();++bad.revision;
    assert(!bad.Update(nonfinite).valid&&!nonfinite.SourceReady());
    nonfinite.Advance(.5f,1);assert(nonfinite.Volume()==held&&nonfinite.PendingSeconds()>=.5);
    bad.Edit(nonfinite,82,82,10);const auto recovered=bad.Update(nonfinite);
    assert(recovered.coverageLost&&recovered.valid&&nonfinite.SourceReady()&&bad.revision>goodRevision);
    auto whole=nonfinite,parts=nonfinite;
    whole.Advance(1,1);for(int tick=0;tick<60;++tick)parts.Advance(1.f/60,1);
    assert(whole.Revision()==parts.Revision()&&std::abs(whole.Volume()-parts.Volume())<1e-5);
    std::printf("Fine-only publication: %zu bytes/surface, %.3f MiB at eight tiles (excluding vector/allocator).\n",
        sizeof(Poseidon::RainWaterFine::Surface),sizeof(Poseidon::RainWaterFine::Surface)*8192.0/(1024*1024));
    std::puts("Default-off, genuine queued edits, exact corners/query, stale closure, budget, gap/overflow, source lifetime and mass-preserving regrid passed.");
}
