// Offline copies of the exact captured solver: generated identifiers and one
// friend declaration only. No production state-restoration API is introduced.
#include "RainWaterReplayA.hpp"
#include "RainWaterReplayB.hpp"
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
struct Capture {
    uint32_t width=0,height=0;
    float spacing=0,x=0,z=0,sea=0;
    double pending=0;
    uint64_t generation=0,revision=0;
    std::array<double,4> budget{};
    std::vector<float> bed,depth;
};
static Capture parse(std::span<const unsigned char> bytes) {
    require(bytes.size()>=88,"truncated header");
    require(std::memcmp(bytes.data(),"RWCAP001",8)==0,"unknown magic");
    size_t offset=8;
    const auto u32=[&] {
        require(offset+4<=bytes.size(),"truncated u32");uint32_t n=0;
        for(unsigned i=0;i<4;++i)n|=uint32_t(bytes[offset++])<<(8*i);
        return n;
    };
    const auto u64=[&] {
        require(offset+8<=bytes.size(),"truncated u64");uint64_t n=0;
        for(unsigned i=0;i<8;++i)n|=uint64_t(bytes[offset++])<<(8*i);
        return n;
    };
    const auto f32=[&]{return std::bit_cast<float>(u32());};
    const auto f64=[&]{return std::bit_cast<double>(u64());};
    Capture c;c.width=u32();c.height=u32();
    require(c.width>=2&&c.height>=2&&c.width<=513&&c.height<=513,"unbounded dimensions");
    const size_t count=size_t(c.width)*c.height;
    require(bytes.size()==88+count*8,"nonexact byte count");
    c.spacing=f32();c.x=f32();c.z=f32();c.sea=f32();c.pending=f64();
    c.generation=u64();c.revision=u64();for(auto& n:c.budget)n=f64();
    require(std::isfinite(c.spacing)&&c.spacing>0&&std::isfinite(c.x)&&std::isfinite(c.z)&&
        std::isfinite(c.sea)&&std::isfinite(c.pending)&&c.pending>=0,"invalid header scalar");
    for(auto n:c.budget)require(std::isfinite(n)&&n>=0,"invalid budget");
    require(offset==88,"header ABI changed");
    c.bed.resize(count);c.depth.resize(count);
    for(auto& n:c.bed){n=f32();require(std::isfinite(n),"nonfinite bed");}
    for(auto& n:c.depth){n=f32();require(std::isfinite(n)&&n>=0,"invalid depth");}
    require(offset==bytes.size(),"trailing bytes");
    return c;
}
static std::vector<unsigned char> read(const char* path) {
    std::ifstream stream(path,std::ios::binary|std::ios::ate);require(bool(stream),"capture open failed");
    const auto size=stream.tellg();
    require(size>=88&&size<=2105440,"unbounded file");
    std::vector<unsigned char> bytes(static_cast<size_t>(size));stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()),size);require(bool(stream),"capture read failed");
    return bytes;
}
template<class T> static bool bits(T a,T b) {
    return std::memcmp(&a,&b,sizeof(T))==0;
}
static bool vectors(const std::vector<float>& a,const std::vector<float>& b) {
    return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0;
}
namespace Poseidon {
struct RainWaterReplayBridge {
    template<class F> static F restore(const Capture& c) {
        F f;require(f.Configure(int(c.width),int(c.height),c.spacing,c.bed,c.x,c.z,c.sea),"Configure failed");
        // Configure gives correctly sized scratch and zero flows. Neither was
        // captured. A genuine Step clears/recomputes flows before comparison.
        f._depth=c.depth;f._pending=c.pending;f._generation=c.generation;f._revision=c.revision;
        f._budget={c.budget[0],c.budget[1],c.budget[2],c.budget[3]};
        const auto v=f.CoarseCapture();require(bool(v),"restored capture unavailable");
        require(v->width==int(c.width)&&v->height==int(c.height)&&bits(v->spacing,c.spacing)&&
            bits(v->originX,c.x)&&bits(v->originZ,c.z)&&bits(v->seaLevel,c.sea),"restore changed domain bits");
        require(vectors(f._bed,c.bed)&&vectors(f._depth,c.depth)&&bits(f._pending,c.pending)&&
            f._generation==c.generation&&f._revision==c.revision,"restore changed captured state");
        require(bits(f._budget.rain,c.budget[0])&&bits(f._budget.infiltration,c.budget[1])&&
            bits(f._budget.evaporation,c.budget[2])&&bits(f._budget.outlet,c.budget[3]),"restore changed budgets");
        return f;
    }
    template<class A,class B> static void equal(const A& a,const B& b) {
        require(vectors(a._bed,b._bed)&&vectors(a._depth,b._depth)&&vectors(a._flowX,b._flowX)&&
            vectors(a._flowZ,b._flowZ),"post-Step bed/depth/flow differs bitwise");
        require(vectors(a._out,b._out)&&vectors(a._delta,b._delta)&&vectors(a._edgeX,b._edgeX)&&
            vectors(a._edgeZ,b._edgeZ),"post-Step scratch differs bitwise");
        require(bits(a._pending,b._pending)&&a._generation==b._generation&&a._revision==b._revision,
            "post-Advance pending/identity differs bitwise");
        require(bits(a._budget.rain,b._budget.rain)&&bits(a._budget.infiltration,b._budget.infiltration)&&
            bits(a._budget.evaporation,b._budget.evaporation)&&bits(a._budget.outlet,b._budget.outlet),
            "post-Advance budget differs bitwise");
    }
    template<class F> static uint64_t outputHash(const F& f) {
        uint64_t hash=14695981039346656037ull;
        const auto add=[&](const auto& value) {
            const auto* p=reinterpret_cast<const unsigned char*>(&value);
            for(size_t i=0;i<sizeof(value);++i){hash^=p[i];hash*=1099511628211ull;}
        };
        for(const auto* v:{&f._depth,&f._flowX,&f._flowZ})for(float n:*v)add(n);
        add(f._pending);add(f._generation);add(f._revision);
        add(f._budget.rain);add(f._budget.infiltration);add(f._budget.evaporation);add(f._budget.outlet);
        return hash;
    }
};
}
using A=Poseidon::RainWaterFieldReplayA;
using B=Poseidon::RainWaterFieldReplayB;
using Bridge=Poseidon::RainWaterReplayBridge;
static void negativeParserChecks(const std::vector<unsigned char>& bytes) {
    const auto reject=[](const auto& b) {
        bool refused=false;try{(void)parse(b);}catch(const std::runtime_error&){refused=true;}
        require(refused,"malformed native capture accepted");
    };
    auto bad=bytes;bad[0]='X';reject(bad);
    bad=bytes;bad.resize(87);reject(bad);
    bad=bytes;bad.push_back(0);reject(bad);
    bad=bytes;bad[8]=2;bad[9]=2;reject(bad); // width514
    bad=bytes;bad[88]=0;bad[89]=0;bad[90]=0xc0;bad[91]=0x7f;reject(bad); // NaN bed
    const auto c=parse(bytes);const size_t depth=88+c.bed.size()*4;
    bad=bytes;bad[depth]=0;bad[depth+1]=0;bad[depth+2]=0x80;bad[depth+3]=0xbf;reject(bad); // -1 depth
    bad=bytes;bad[38]=0xf8;bad[39]=0x7f;reject(bad); // NaN pending
}
static void proof(const Capture& c,float rain,float sun,float wind) {
    auto a=Bridge::restore<A>(c);auto b=Bridge::restore<B>(c);
    a.Advance(.25f,rain,sun,wind);b.Advance(.25f,rain,sun,wind);
    require(a.Revision()>c.revision,"first advance did not execute a genuine Step");
    Bridge::equal(a,b);
    // The same retained pending state exercises no-step and multi-step work;
    // paused/nonfinite calls remain inert. No historic flight forcing is inferred.
    constexpr std::array<float,12> dt={.125f,.125f,.5f,0,.0625f,.1875f,.25f,.5f,
        .01f,.03f,.11f,.13f};
    for(float seconds:dt){a.Advance(seconds,rain,sun,wind);b.Advance(seconds,rain,sun,wind);Bridge::equal(a,b);}
    a.Advance(std::numeric_limits<float>::quiet_NaN(),rain,sun,wind);
    b.Advance(std::numeric_limits<float>::quiet_NaN(),rain,sun,wind);Bridge::equal(a,b);
    std::cout<<"{\"kind\":\"bitwise-proof\",\"advancesCompared\":14,\"firstGenuineStep\":true,"
        "\"flowHistoryCaptured\":false,\"outputFnv64\":\""<<std::hex<<Bridge::outputHash(a)<<std::dec<<"\"}\n";
}
template<class F> static void measure(const Capture& c,float rain,float sun,float wind,
                                      unsigned round,const char* label,unsigned steps) {
    auto f=Bridge::restore<F>(c); // Configure/restore outside both clocks.
    f.Advance(.25f,rain,sun,wind); // first genuine step/warmup outside timing.
    const auto revision=f.Revision();
    const auto cpuStart=Poseidon::RainWaterCost::ThreadCpuTime();
    const auto begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<steps;++i)f.Advance(.25f,rain,sun,wind);
    const auto end=std::chrono::steady_clock::now();
    const auto cpuEnd=Poseidon::RainWaterCost::ThreadCpuTime();
    require(f.Revision()==revision+steps,"timed step count differs");
    const double ms=std::chrono::duration<double,std::milli>(end-begin).count();
    std::cout<<std::setprecision(17)<<"{\"kind\":\"timing\",\"round\":"<<round<<",\"label\":\""<<label
        <<"\",\"steps\":"<<steps<<",\"wallMs\":"<<ms<<",\"threadCpuMs\":";
    if(cpuStart.valid&&cpuEnd.valid&&cpuEnd.ns>=cpuStart.ns)std::cout<<double(cpuEnd.ns-cpuStart.ns)*1e-6;
    else std::cout<<"null";
    std::cout<<",\"startRevision\":"<<revision<<",\"endRevision\":"<<f.Revision()
        <<",\"outputFnv64\":\""<<std::hex<<Bridge::outputHash(f)<<std::dec<<"\"}\n";
}
int main(int argc,char** argv) try {
    require(argc==7,"expected capture/rainbits/sunbits/windbits/mode/rounds");
    const auto bytes=read(argv[1]);negativeParserChecks(bytes);const auto c=parse(bytes);
    const auto forcing=[](const char* s) {
        require(std::strlen(s)==8,"float forcing bits malformed");
        size_t consumed=0;const auto n=std::stoul(s,&consumed,16);
        require(consumed==8,"float forcing bits contain trailing characters");
        const auto v=std::bit_cast<float>(uint32_t(n));
        require(std::isfinite(v)&&v>=0&&v<=1,"forcing outside actual range");return v;
    };
    const float rain=forcing(argv[2]),sun=forcing(argv[3]),wind=forcing(argv[4]);
    require(!Poseidon::RainWaterCost::Enabled(),"internal diagnostic must be disabled for offline baseline timing");
    if(std::strcmp(argv[5],"proof")==0)proof(c,rain,sun,wind);
    else {
        require(std::strcmp(argv[5],"bench")==0,"unknown mode");
        const unsigned rounds=unsigned(std::stoul(argv[6]));
        require(rounds>=2&&rounds<=8,"benchmark rounds unbounded");
        for(unsigned r=0;r<rounds;++r) {
            measure<A>(c,rain,sun,wind,r,"A",16);measure<B>(c,rain,sun,wind,r,"B",16);
            measure<B>(c,rain,sun,wind,r,"B",16);measure<A>(c,rain,sun,wind,r,"A",16);
        }
    }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
