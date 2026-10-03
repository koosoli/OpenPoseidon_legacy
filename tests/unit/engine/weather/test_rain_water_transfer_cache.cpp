#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include <RainWaterFieldReference.hpp>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

// Observe the actual float-vector allocations, including absence of per-step
// allocation. No private access, production test switch or replacement kernel.
static bool watch=false;
static size_t allocations=0,bytes=0;
void* operator new(size_t size) {
    if (watch) {++allocations;bytes+=size;}
    if (void* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
using Actual=Poseidon::RainWaterField;
using Reference=Poseidon::RainWaterFieldReference;
static bool exact(double a,double b) {return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);}
static void same(const Actual& a,const Reference& r) {
    assert(a.Width()==r.Width() && a.Height()==r.Height() && a.Spacing()==r.Spacing());
    assert(a.Generation()==r.Generation() && a.Revision()==r.Revision());
    assert(!a.FineActive() && !r.FineActive() && a.SourceReady()==r.SourceReady());
    assert(exact(a.PendingSeconds(),r.PendingSeconds()) && exact(a.Volume(),r.Volume()));
    const auto x=a.Snapshot(),y=r.Snapshot();
    assert(x.size()==y.size() && (x.empty() || std::memcmp(x.data(),y.data(),x.size()*sizeof(float))==0));
    const auto ab=a.WaterBudget();const auto rb=r.WaterBudget();
    assert(exact(ab.rain,rb.rain) && exact(ab.infiltration,rb.infiltration));
    assert(exact(ab.evaporation,rb.evaporation) && exact(ab.outlet,rb.outlet));
}
static void advance(Actual& a,Reference& r,float dt,float rain,float sun=0,float wind=0) {
    allocations=0;bytes=0;watch=true;a.Advance(dt,rain,sun,wind);watch=false;
    assert(allocations==0 && bytes==0);
    r.Advance(dt,rain,sun,wind);same(a,r);
}
static void configure(Actual& a,Reference& r,int w,int h,float spacing,const std::vector<float>& bed) {
    auto actualBed=bed,referenceBed=bed;
    allocations=0;bytes=0;watch=true;
    const bool ac=a.Configure(w,h,spacing,std::move(actualBed),7,-3,0);
    watch=false;const size_t acBytes=bytes,acCount=allocations;
    allocations=0;bytes=0;watch=true;
    const bool rc=r.Configure(w,h,spacing,std::move(referenceBed),7,-3,0);
    watch=false;
    assert(ac && rc && acCount==allocations+2);
    const size_t cacheBytes=(size_t(w-1)*h+size_t(w)*(h-1))*sizeof(float);
    // Large MSVC vector allocations request alignment overhead too; include
    // that actual overhead in the unchanged strict 2*MaxCells-float bound.
    assert(acBytes>=bytes+cacheBytes && acBytes-bytes<2*Actual::MaxCells*sizeof(float));
    same(a,r);
}
int main() {
    constexpr int w=13,h=9;
    for(int terrain=0;terrain<6;++terrain) {
        std::vector<float> bed(w*h,10);
        for(int z=0;z<h;++z) for(int x=0;x<w;++x) {
            const auto i=size_t(z)*w+x;
            if(terrain==1) bed[i]=10+.07f*x-.1f*z;
            if(terrain==2) bed[i]=10+((x+z)%2 ? .5f : -.5f);
            if(terrain==3) bed[i]=z==h-1 ? -1 : 10-.2f*z;
            if(terrain==4) bed[i]=10+std::hypot(float(x-w/2),float(z-h/2))*.1f;
            if(terrain==5) bed[i]=(x%2 ? 1 : -1)*std::numeric_limits<float>::max();
        }
        Actual a;Reference r;configure(a,r,w,h,terrain%2 ? 25.f : .5f,bed);
        // Real dry and tiny bounded-transition controls precede seeded fixture
        // water; fixtures never imply runtime map/source admission.
        advance(a,r,.25f,0);advance(a,r,.25f,6e-16f);
        for(int z=0;z<h;++z) for(int x=0;x<w;++x) if((x+3*z)%7==0) {
            const float water=.02f*float(1+(x+z)%9);
            assert(a.AddWater(x,z,water)==r.AddWater(x,z,water));
        }
        same(a,r);
        for(int n=0;n<80;++n) {
            if(n==19 || n==51) {assert(a.SetBed(6,4,9.2f)==r.SetBed(6,4,9.2f));same(a,r);}
            if(n==42) {a.SetSeaLevel(9.9f);r.SetSeaLevel(9.9f);}
            advance(a,r,n%3 ? .25f : .125f,n<30 ? .8f : 0,n%2 ? .9f : 0,n%5 ? .7f : 0);
        }
        const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
        advance(a,r,0,1);advance(a,r,-1,1);advance(a,r,nan,1);advance(a,r,inf,1);
        advance(a,r,.25f,nan);advance(a,r,.25f,inf);
        advance(a,r,.25f,2,nan,inf);advance(a,r,.25f,-1,2,-1);
        assert(!a.SetBed(-1,0,2) && !r.SetBed(-1,0,2));
        assert(!a.SetBed(1,1,nan) && !r.SetBed(1,1,nan));
        assert(!a.AddWater(1,1,-1) && !r.AddWater(1,1,-1));same(a,r);
        const auto invalidBed=std::vector<float>(w*h,nan);
        assert(!a.Configure(w,h,1,invalidBed) && !r.Configure(w,h,1,invalidBed));same(a,r);
        a.Reset();r.Reset();same(a,r);
        configure(a,r,3,7,2,std::vector<float>(21,8));advance(a,r,4,.6f,.8f,.9f);
        configure(a,r,17,2,3,std::vector<float>(34,6));advance(a,r,8,.6f);
        // Bounded 16-step backlog drains using each next call's real parameters.
        advance(a,r,.001f,0);advance(a,r,.001f,0);
    }
    Actual a;Reference r;configure(a,r,5,5,2,std::vector<float>(25,10));
    // An exact empty step must ignore stale transfers from an earlier wet
    // step, retain all real sink budgets/revisions, and overwrite the whole
    // cache before later water is admitted. Tiny actual rain is fully absorbed.
    assert(a.AddWater(2,2,.000002f) && r.AddWater(2,2,.000002f));
    advance(a,r,.25f,0);
    for(int n=0;n<12;++n) advance(a,r,.25f,6e-16f);
    assert(a.Volume()==0 && r.Volume()==0);
    advance(a,r,.25f,.8f);
    assert(a.Volume()>0);
    // Both local clamp branches must remain physical: small spacing needs
    // partial donor limiting; default coarse spacing retains most donor water.
    for(const float spacing : {.125f,2.f,25.f}) {
        auto raised=std::vector<float>(9,10);raised[4]=11;
        configure(a,r,3,3,spacing,raised);
        assert(a.AddWater(1,1,.1f) && r.AddWater(1,1,.1f));
        advance(a,r,.25f,0);
        const auto cells=a.Snapshot();
        assert(cells[4*4+1]>=0 && cells[4*4+1]<.1f);
        if(spacing==.125f)assert(cells[4*4+1]<=1e-7f);
        if(spacing==25.f)assert(cells[4*4+1]>.08f);
    }
    configure(a,r,5,5,2,std::vector<float>(25,10));
    for(int n=0;n<240;++n) advance(a,r,1.f/60,.7f,.3f,.8f);
    // Validate the actual MaxCells bound and cache size without changing it.
    configure(a,r,1024,1024,25,std::vector<float>(Actual::MaxCells,10));
    assert(!a.Configure(1025,1024,25,{}) && !r.Configure(1025,1024,25,{}));
    assert(a.AddWater(512,511,.2f) && r.AddWater(512,511,.2f));
    advance(a,r,.25f,.85f,.4f,.1f);
    std::cout << "PASS bitwise actual coarse reference: flat/slope/checker/hollow/sea/extreme, "
                 "dry/rain/drying/wind, edit/regrid/backlog/partitions/finite, bounded cache and zero step allocations\n";
}
