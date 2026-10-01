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
static std::vector<Poseidon::RainWaterCellGeometry> actualCraterTile() {
    constexpr int n=33;std::vector<float> source(n*n,10);
    std::vector<Poseidon::RainWaterCellGeometry> out(1024);
    for(int z=0;z<n;++z)for(int x=0;x<n;++x)
        source[z*n+x]-=Poseidon::TerrainCraterDepth(x*6.25f-112.5f,z*6.25f-112.5f,8,1);
    for(int z=0;z<32;++z)for(int x=0;x<32;++x) {
        out[z*32+x]={source[z*n+x],source[z*n+x+1],source[(z+1)*n+x],source[(z+1)*n+x+1]};
    }
    return out;
}
int main() {
    std::puts("scenario,implementation,run,advance_ms_per_025simsec,snapshot_ms,snapshot_bytes,final_volume_m3");
    for(const auto scenario:{"closed-shallow","closed-deep","actual-crater","terrain-slope"}) {
        RainWaterField legacy;RainWaterFine coarse,fine;
        std::vector<float> bed(side*side,10);const std::vector<float> tile(1024,10);
        const bool slope=std::string_view(scenario)=="terrain-slope";
        if(slope)for(int z=0;z<side;++z)for(int x=0;x<side;++x)bed[z*side+x]=float(10+.01*x*25+.015*z*25);
        if(!legacy.Configure(side,side,25,bed)||!coarse.Configure(side,side,25,bed))return 1;
        if(std::string_view(scenario)=="closed-deep") {
            legacy.AddWater(4,4,2);coarse.AddWater(112.5,112.5,1250);
        }
        fine=coarse;
        for(int z=0;z<8;++z)for(int x=0;x<8;++x) {
            if(slope) {
                std::vector<Poseidon::RainWaterCellGeometry> geometry(1024);
                const auto height=[](double a,double b){return float(10+.01*a+.015*b);};
                for(int cz=0;cz<32;++cz)for(int cx=0;cx<32;++cx) {
                    const double a=x*200+cx*6.25,b=z*200+cz*6.25;
                    geometry[cz*32+cx]={height(a,b),height(a+6.25,b),height(a,b+6.25),height(a+6.25,b+6.25)};
                }
                if(!fine.RefineTileGeometry(x,z,geometry))return 2;
            } else if(!fine.RefineTile(x,z,tile))return 2;
        }
        if(std::string_view(scenario)=="actual-crater"&&!fine.RefineTileGeometry(0,0,actualCraterTile()))return 3;
        for(int run=1;run<=3;++run) {
            if(run%2) {measure(legacy,scenario,"legacy",run);measure(coarse,scenario,"fine-core-coarse",run);measure(fine,scenario,"fine-core-64tiles",run);}
            else {measure(fine,scenario,"fine-core-64tiles",run);measure(coarse,scenario,"fine-core-coarse",run);measure(legacy,scenario,"legacy",run);}
        }
    }
    return std::isfinite(digest)?0:4;
}
