#include <Poseidon/World/Weather/RotorGroundPolicy.hpp>
#include <Poseidon/World/Weather/RotorLandDrawReceipt.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <cassert>
#include <limits>

using namespace Poseidon;
int main()
{
    RotorGroundBudget budget;
    for (int i=0; i<32; ++i) assert(budget.Take(.01f));
    assert(!budget.Take(.02f) && !budget.Take(.249f));
    for (int i=0; i<32; ++i) assert(budget.Take(.25f));
    assert(!budget.Take(.499f));
    assert(budget.Take(0)); // mission time reset, no old-world emitter state
    assert(!budget.Take(std::numeric_limits<float>::quiet_NaN()));
    assert(!budget.Take(-1));
    auto sample = [](float rpm = 1, float height = 5, float up = 1,
                     float dust = .5f, float snow = 0, float wet = 0, float rain = 0,
                     float water = 0, bool valid = false, bool land = true,
                     bool roof = false, bool road = false) {
        return RotorGroundPolicy(rpm,height,up,dust,snow,wet,rain,water,valid,land,roof,road);
    };
    const auto dust = sample();
    assert(dust.density > .6f && !dust.snow);
    assert(sample(.5f).density < dust.density * .26f);
    assert(sample(1,20).density < dust.density);
    assert(sample(0).density == 0 && sample(1,30).density == 0);
    assert(sample(1,-1).density == 0 && sample(1,5,.2f).density == 0);
    assert(sample(1,5,1,0).density == 0); // actual hard/no-dust metadata
    assert(sample(1,5,1,.5f,0,1).density == 0);
    assert(sample(1,5,1,.5f,0,0,1).density == 0);
    assert(sample(1,5,1,.5f,0,0,0,0,false,false).density == 0); // sea
    assert(sample(1,5,1,.5f,0,0,0,0,false,true,true).density == 0); // actual shelter
    assert(sample(1,5,1,.5f,0,0,0,0,false,true,false,true).density == 0); // roadway
    assert(sample(1,5,1,.5f,0,0,0,.004f,true).density == 0); // real standing water
    assert(sample(1,5,1,.5f,0,0,0,0,false).density > 0); // absent hydrology dry fallback
    const auto snow = sample(1,5,1,0,.1f,1,0);
    assert(snow.snow && snow.density > .6f); // actual deposited powder
    assert(sample(1,5,1,0,.1f,0,0,0,false,true,true).density == 0);
    assert(sample(std::numeric_limits<float>::quiet_NaN()).density == 0);
    assert(sample(1,5,1,.5f,0,0,0,std::numeric_limits<float>::infinity(),true).density == 0);
    // Exercise production hydrology query rather than invent a water sample.
    RainWaterField water;
    assert(water.Configure(3,3,1,std::vector<float>(9,1.0f)));
    assert(water.AddWater(1,1,.02f));
    const auto flooded = water.At(1,1);
    assert(flooded.valid && flooded.depth > .003f);
    assert(sample(1,5,1,.5f,0,0,0,flooded.depth,flooded.valid).density == 0);
    // Fixed-world annulus, deterministic private stream and no centre plume.
    for (unsigned phase=0; phase<32; ++phase) for (unsigned spoke=0; spoke<8; ++spoke) {
        const auto a = RotorGroundRing(phase,spoke,8);
        assert(a == RotorGroundRing(phase,spoke,8));
        const float radius = std::hypot(a[0],a[1]);
        assert(std::isfinite(radius) && radius >= 5.5f && radius <= 7.7f);
        assert(a != RotorGroundRing(phase+1,spoke,8));
    }
    assert((RotorGroundRing(0,0,std::numeric_limits<float>::infinity()) == std::array<float,2>{}));
    // A complete broad footprint costs five probes; no partial proof can be
    // published as an extra puff, and the global bound remains32/quarter second.
    RotorGroundBudget footprints;
    int complete = 0, probes = 0;
    for (int sector=0; sector<8; ++sector) {
        bool proven = true;
        for (int point=0; point<5; ++point) {
            if (!footprints.Take(.1f)) {proven=false;break;}
            ++probes;
        }
        if (proven) ++complete;
    }
    assert(complete == 6 && probes == 32);
    assert(RotorLandFootprint(0) == (std::array<float,2>{0,0}));
    for (unsigned i=1; i<5; ++i) {
        const auto edge=RotorLandFootprint(i);
        assert(std::abs(std::hypot(edge[0],edge[1])-RotorLandProofRadius)<1e-5f);
    }
    assert(RotorLandProofRadius >= std::sqrt(2.0f)*RotorLandMaxRadius+RotorLandMaxTravel);
    // The real cloudlet sheet renderer projects Scale directly. Its authored
    // sphere only controls culling; these two unit systems must stay separate.
    for (float sphere : {.125f,.7f,1.0f,2.0f,8.0f}) {
        const float scale=RotorLandCullScale(3.0f,sphere);
        assert(std::abs(scale*sphere-std::sqrt(2.0f)*3.0f)<1e-5f);
    }
    assert(RotorLandCullScale(3,0)==0 && RotorLandCullScale(3,-1)==0);
    assert(RotorLandCullScale(3,1e-38f)==0);
    // A small roof away from every center/perimeter ray still lies within the
    // broad puff corridor. Conservative bounds must refuse it, not miss it.
    const std::array<float,3> bottom{0,0,0}, top{0,10,0};
    assert(RotorLandBoundsBlock(bottom,top,{3,5,3},.5f));
    assert(RotorLandBoundsBlock(bottom,top,{0,-RotorLandProofRadius,0},0));
    assert(!RotorLandBoundsBlock(bottom,top,{20,5,0},1));
    assert(!RotorLandBoundsBlock(bottom,top,{0,30,0},1));
    assert(RotorLandBoundsBlock(bottom,bottom,{1,0,0},0));
    assert(RotorLandBoundsBlock(bottom,top,{0,5,0},-1));
    assert(RotorLandBoundsBlock(bottom,top,{std::numeric_limits<float>::infinity(),0,0},1));
    for (bool isSnow : {false,true}) {
        const float life=isSnow?1.7f:1.9f;
        assert(RotorLandProfile(0,.458f,isSnow).opacity==0);
        assert(RotorLandProfile(life,.458f,isSnow).radius==0);
        const auto root=RotorLandProfile(.12f,.458f,isSnow);
        const auto grown=RotorLandProfile(.9f,.458f,isSnow);
        assert(root.opacity>.18f && root.radius>1.6f);
        assert(grown.radius>root.radius && std::abs(grown.opacity-root.opacity)<1e-6f);
        // The already-soft stock alpha sheet must not suffer another area
        // attenuation. Before egress its actual density's bounded optical
        // mapping sets opacity, with the same near-hover caps.
        const float cap=isSnow?.28f:.35f;
        const float gain=isSnow?.60f:.70f;
        assert(std::abs(grown.opacity-std::min(cap,gain*std::sqrt(.458f)))<1e-6f);
        assert(RotorLandProfile(.9f,1,isSnow).opacity==cap);
        assert(RotorLandProfile(.9f,20,isSnow).opacity==cap);
        float previousOpacity=0;
        for (int strength=0;strength<=1000;++strength) {
            const float density=float(strength)*.001f;
            const auto profile=RotorLandProfile(.9f,density,isSnow);
            assert(profile.opacity>=previousOpacity && profile.opacity<=cap);
            assert(std::abs(profile.opacity-std::min(cap,gain*std::sqrt(density)))<1e-6f);
            previousOpacity=profile.opacity;
        }
        assert(RotorLandProfile(life-.1f,.458f,isSnow).opacity<grown.opacity);
        float previousRadius=0;
        for (int ms=0;ms<int(life*1000);++ms) {
            const auto p=RotorLandProfile(ms*.001f,1,isSnow);
            assert(std::isfinite(p.radius) && std::isfinite(p.opacity));
            assert(p.radius>=previousRadius && p.radius<=RotorLandMaxRadius);
            assert(p.opacity>=0 && p.opacity<=cap);
            previousRadius=p.radius;
        }
        assert(RotorLandProfile(-1,1,isSnow).opacity==0);
        assert(RotorLandProfile(.2f,0,isSnow).radius==0);
        assert(RotorLandProfile(.2f,std::numeric_limits<float>::infinity(),isSnow).opacity==0);
        assert(RotorLandProfile(std::numeric_limits<float>::quiet_NaN(),1,isSnow).radius==0);
        // Actual c55a65 hover was 19.45-19.82m. The full-strength policy's
        // squared height attenuation is ~.115-.124, not the old .458 fixture.
        for(const float density : {.115f,.124f}) {
            const auto low=RotorLandProfile(.9f,density,isSnow);
            const float old=gain*density;
            assert(low.opacity>2.8f*old && low.opacity<cap);
            assert(low.opacity*.32f>.06f); // stock-sheet mean cited in existing profile audit
            assert(low.radius==grown.radius);
        }
    }
    const auto earth=RotorLandTint(false),flakes=RotorLandTint(true);
    constexpr std::array<float,3> priorEarth{.62f,.53f,.39f};
    for(unsigned i=0;i<3;++i) {
        assert(std::isfinite(earth[i]) && earth[i]>0 && earth[i]<1);
        // Bounded material correction after an actual admitted-dust capture:
        // no channel turns into black ash or bright unlit material.
        assert(earth[i]/priorEarth[i]>.70f && earth[i]/priorEarth[i]<.80f);
        assert(flakes[i]>=.90f && flakes[i]<1);
    }
    assert(earth[0]>earth[1] && earth[1]>earth[2]);
    const float oldY=.2126f*priorEarth[0]+.7152f*priorEarth[1]+.0722f*priorEarth[2];
    const float newY=.2126f*earth[0]+.7152f*earth[1]+.0722f*earth[2];
    assert(newY/oldY>.70f && newY/oldY<.80f);
    float previousLitY=0;
    for(const float light : {0.f,.01f,.25f,1.f,4.f}) {
        const float litY=newY*light;
        assert(std::isfinite(litY) && litY>=previousLitY);
        previousLitY=litY;
        for(unsigned i=0;i<3;++i) {
            const float lit=earth[i]*light,prior=priorEarth[i]*light;
            assert(std::isfinite(lit) && lit>=0 && lit<=prior);
            if(light==0)assert(lit==0); // albedo cannot create an unlit glow
            else assert(lit/prior>.70f && lit/prior<.80f);
        }
    }
    RotorLandDrawReceipt receipt;
    assert(receipt.Calls()==0);
    for(auto outcome : {RotorLandDrawOutcome::Hidden,RotorLandDrawOutcome::Shape,RotorLandDrawOutcome::Near,RotorLandDrawOutcome::Mip})
        assert(receipt.Record(outcome));
    assert(receipt.Calls()==4 && receipt.counts[4]==0);
    assert(!receipt.Record(RotorLandDrawOutcome::Count));
    assert(!receipt.Record(RotorLandDrawOutcome::DecalCall,std::numeric_limits<float>::quiet_NaN(),20,10,3,50));
    assert(receipt.Record(RotorLandDrawOutcome::DecalCall,.24f,20,10,3,50,.9f));
    assert(receipt.Record(RotorLandDrawOutcome::DecalCall,.20f,30,15,2,40,.1f));
    assert(receipt.Calls()==7 && receipt.counts[4]==3 && receipt.invalid==1);
    assert(receipt.alphaMin==.20f && receipt.alphaMax==.24f && receipt.halfXMin==20 && receipt.halfXMax==30);
    assert(receipt.radiusMin==2 && receipt.radiusMax==3 && receipt.distanceMin==40 && receipt.distanceMax==50);
    assert(receipt.ageMin==.1f && receipt.ageMax==.9f);
    receipt={};assert(receipt.Calls()==0 && receipt.invalid==0 && receipt.alphaMax==0);
}
