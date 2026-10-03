// Reuse the audited loader/restore bridge. Baseline entry stays independent.
#define main replay_baseline_main
#include "rain_water_coarse_replay.cpp"
#undef main
static_assert(sizeof(A)==sizeof(B),"test-only dispatch must preserve class layout size");
static void publicEqual(const A& a,const B& b) {
    require(vectors(a.Snapshot(),b.Snapshot()),"candidate public bed/depth/flow differs bitwise");
    require(bits(a.PendingSeconds(),b.PendingSeconds())&&a.Generation()==b.Generation()&&
        a.Revision()==b.Revision(),"candidate public pending/identity differs");
    const auto x=a.WaterBudget();const auto y=b.WaterBudget();
    require(bits(x.rain,y.rain)&&bits(x.infiltration,y.infiltration)&&
        bits(x.evaporation,y.evaporation)&&bits(x.outlet,y.outlet),"candidate public budgets differ");
    // _out/_edge caches are deliberately unused on fused steps. They are not
    // claimed equal; their next fallback use must overwrite all entries first.
}
static void actual(const Capture& c,float rain,float sun,float wind) {
    auto a=Bridge::restore<A>(c);auto b=Bridge::restore<B>(c);
    a.Advance(.25f,rain,sun,wind);b.Advance(.25f,rain,sun,wind);publicEqual(a,b);
    require(a.Revision()>c.revision,"no genuine initial Step");
    constexpr std::array<float,12> dt={.125f,.125f,.5f,0,.0625f,.1875f,.25f,.5f,.01f,.03f,.11f,.13f};
    for(float seconds:dt){a.Advance(seconds,rain,sun,wind);b.Advance(seconds,rain,sun,wind);publicEqual(a,b);}
    a.Advance(std::numeric_limits<float>::quiet_NaN(),rain,sun,wind);
    b.Advance(std::numeric_limits<float>::quiet_NaN(),rain,sun,wind);publicEqual(a,b);
    std::cout<<"{\"kind\":\"actual-candidate-proof\",\"advancesCompared\":14,"
        "\"observableStateBitwiseEqual\":true,\"unusedScratchClaimedEqual\":false,"
        "\"fusedSteps\":"<<Poseidon::ReplayFusionSteps<<",\"fallbackSteps\":"<<Poseidon::ReplayFallbackSteps
        <<",\"outputFnv64\":\""<<std::hex<<Bridge::outputHash(a)<<std::dec<<"\"}\n";
}
static void boundaries() {
    for(float spacing:{2.f,9.999f,10.f,25.f,1000000.f,1000001.f}) {
        A a;B b;require(a.Configure(3,3,spacing,std::vector<float>(9,10))&&
            b.Configure(3,3,spacing,std::vector<float>(9,10)),"boundary configure failed");
        require(a.AddWater(1,1,.08f)&&b.AddWater(1,1,.08f),"boundary water failed");
        const auto before=Poseidon::ReplayFusionSteps;
        for(int i=0;i<4;++i){a.Advance(.25f,.8f);b.Advance(.25f,.8f);publicEqual(a,b);}
        require((Poseidon::ReplayFusionSteps>before)==(spacing>=10&&spacing<=1000000),
            "spacing boundary admitted wrong branch");
    }
    // Fused -> fallback -> fused: cached raw transfers must be freshly written,
    // never reused from the deliberately untouched fused-step scratch.
    A a;B b;require(a.Configure(3,3,25,std::vector<float>(9,10))&&
        b.Configure(3,3,25,std::vector<float>(9,10)),"transition configure failed");
    a.AddWater(1,1,.08f);b.AddWater(1,1,.08f);
    a.Advance(.25f,.8f);b.Advance(.25f,.8f);publicEqual(a,b);
    const auto fused=Poseidon::ReplayFusionSteps,fallback=Poseidon::ReplayFallbackSteps;
    require(a.SetBed(0,0,1000001)&&b.SetBed(0,0,1000001),"transition bed failed");
    a.Advance(.25f,.8f);b.Advance(.25f,.8f);publicEqual(a,b);
    require(Poseidon::ReplayFusionSteps==fused&&Poseidon::ReplayFallbackSteps==fallback+1,
        "unsafe bed did not take exact fallback");
    a.SetBed(0,0,10);b.SetBed(0,0,10);
    a.Advance(.25f,.8f);b.Advance(.25f,.8f);publicEqual(a,b);
    require(Poseidon::ReplayFusionSteps==fused+1,"safe source did not return to fused branch");
    for(float depth:{1024.f,1025.f,std::numeric_limits<float>::max()}) {
        A x;B y;x.Configure(3,3,25,std::vector<float>(9,10));y.Configure(3,3,25,std::vector<float>(9,10));
        x.AddWater(1,1,depth);y.AddWater(1,1,depth);
        const auto f=Poseidon::ReplayFallbackSteps;
        x.Advance(.25f,.8f);y.Advance(.25f,.8f);publicEqual(x,y);
        require((Poseidon::ReplayFallbackSteps>f)==(depth>1024),"depth cap branch differs");
        // Existing AddWater may overflow; the prototype must retain the
        // original invalid-state path rather than fusing nonfinite depths.
        if(depth==std::numeric_limits<float>::max()) {
            x.AddWater(1,1,depth);y.AddWater(1,1,depth);
            x.AddWater(1,1,depth);y.AddWater(1,1,depth);
            const auto previous=Poseidon::ReplayFusionSteps;
            x.Advance(.25f,0);y.Advance(.25f,0);publicEqual(x,y);
            require(Poseidon::ReplayFusionSteps==previous,"overflow entered fused kernel");
        }
    }
    // Four simultaneous speed-capped donor faces at the minimum admitted
    // spacing, plus dry neighbours and actual sea sinks. These exercise the
    // conservative margin, not merely low-amplitude shallow transfers.
    std::vector<float> bed(25,10);
    bed[0]=-1000000;bed[12]=1000000;bed[24]=1000000;
    A high;B highCandidate;high.Configure(5,5,10,bed);highCandidate.Configure(5,5,10,bed);
    for(const auto& point:std::array<std::pair<int,int>,3>{{{0,0},{2,2},{4,4}}}) {
        high.AddWater(point.first,point.second,1024);highCandidate.AddWater(point.first,point.second,1024);
    }
    for(int i=0;i<12;++i){high.Advance(.25f,1,1,1);highCandidate.Advance(.25f,1,1,1);publicEqual(high,highCandidate);}
}
int main(int argc,char** argv) try {
    require(argc==5,"expected native capture and three float bit strings");
    const auto bytes=read(argv[1]);negativeParserChecks(bytes);const auto c=parse(bytes);
    const auto number=[](const char* s){return std::bit_cast<float>(uint32_t(std::stoul(s,nullptr,16)));};
    const float rain=number(argv[2]),sun=number(argv[3]),wind=number(argv[4]);
    require(!Poseidon::RainWaterCost::Enabled(),"offline cost instrumentation enabled");
    Poseidon::ReplayFusionSteps=Poseidon::ReplayFallbackSteps=0;
    actual(c,rain,sun,wind);
    boundaries();
    std::cout<<"{\"kind\":\"synthetic-boundaries\",\"exactFallbackAndTransitions\":true}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
