#include "../../../../engine/Poseidon/World/Weather/RainWaterFine.hpp"
#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include "../../../../engine/Poseidon/World/Terrain/TerrainCrater.hpp"
#include <chrono>
#include <cstdio>
#include <string_view>
using Clock=std::chrono::steady_clock;
using Poseidon::RainWaterFine;
using Poseidon::RainWaterField;
static volatile double digest=0;
static constexpr int side=513,steps=32,copies=16;
// The complete payload escapes optimisation without timing a second checksum
// sweep. This benchmark script explicitly targets Clang.
[[gnu::noinline]] static void observe(const void* data,size_t bytes) {
    __asm__ __volatile__("" : : "r"(data),"r"(bytes) : "memory");
}
template<class F> static void measure(F field,const char* scenario,const char* implementation,int run) {
    for(int i=0;i<5;++i)field.Advance(.25,1,.5,.1);
    const auto begin=Clock::now();
    for(int i=0;i<steps;++i)field.Advance(.25,1,.5,.1);
    const double stepMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count()/steps;
    const auto snapshotStart=Clock::now();
    size_t bytes=0;
    for(int i=0;i<copies;++i) {auto snapshot=field.Snapshot();bytes=snapshot.size()*sizeof(snapshot[0]);observe(snapshot.data(),bytes);digest=double(snapshot.size());}
    const double copyMs=std::chrono::duration<double,std::milli>(Clock::now()-snapshotStart).count()/copies;
    digest=field.Volume();
    std::printf("%s,%s,%d,%.6f,%.6f,%zu,%.9f\n",scenario,implementation,run,stepMs,copyMs,bytes,field.Volume());
}
static std::vector<float> actualCraterTile() {
    constexpr int n=33;std::vector<float> source(n*n,10),out(1024);
    for(int z=0;z<n;++z)for(int x=0;x<n;++x)
        source[z*n+x]-=Poseidon::TerrainCraterDepth(x*6.25f-112.5f,z*6.25f-112.5f,8,1);
    for(int z=0;z<32;++z)for(int x=0;x<32;++x) {
        const double gx=x+.5,gz=z+.5;const int ix=int(gx),iz=int(gz);
        const double fx=gx-ix,fz=gz-iz;
        const double a=source[iz*n+ix],b=source[iz*n+ix+1],c=source[(iz+1)*n+ix],d=source[(iz+1)*n+ix+1];
        out[z*32+x]=float(fx+fz<=1?a+(b-a)*fx+(c-a)*fz:d+(c-d)*(1-fx)+(b-d)*(1-fz));
    }
    return out;
}
int main() {
    std::puts("scenario,implementation,run,advance_ms_per_025simsec,snapshot_ms,snapshot_bytes,final_volume_m3");
    for(const auto scenario:{"closed-shallow","closed-deep","actual-crater"}) {
        RainWaterField legacy;RainWaterFine coarse,fine;
        const std::vector<float> bed(side*side,10),tile(1024,10);
        if(!legacy.Configure(side,side,25,bed)||!coarse.Configure(side,side,25,bed))return 1;
        if(std::string_view(scenario)=="closed-deep") {
            legacy.AddWater(4,4,2);coarse.AddWater(112.5,112.5,1250);
        }
        fine=coarse;
        for(int z=0;z<8;++z)for(int x=0;x<8;++x)if(!fine.RefineTile(x,z,tile))return 2;
        if(std::string_view(scenario)=="actual-crater"&&!fine.RefineTile(0,0,actualCraterTile()))return 3;
        for(int run=1;run<=3;++run) {
            if(run%2) {measure(legacy,scenario,"legacy",run);measure(coarse,scenario,"fine-core-coarse",run);measure(fine,scenario,"fine-core-64tiles",run);}
            else {measure(fine,scenario,"fine-core-64tiles",run);measure(coarse,scenario,"fine-core-coarse",run);measure(legacy,scenario,"legacy",run);}
        }
    }
    return std::isfinite(digest)?0:4;
}
