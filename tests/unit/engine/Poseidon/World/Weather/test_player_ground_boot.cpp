#include <Poseidon/World/Entities/Infantry/GroundBootEvents.hpp>
#include <Poseidon/World/Scene/InsideViewPass.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/SandGroundAdmission.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/MudGroundAdmission.hpp>
#include <cassert>
#include <cmath>
#include <limits>
#include <cstdio>

using namespace Poseidon;

int main()
{
    // Actual queue policy: a normal fallback level0 is never a cockpit draw.
    // Preserve both an explicitly forced fallback0 and a distinct inside LOD,
    // including the same explicit queue used by camera effects.
    assert(!ExplicitInsideViewDraw(0,-1,0));
    assert(!ExplicitInsideViewDraw(0,-1,4));
    assert(!ExplicitInsideViewDraw(4,-1,4));
    assert(ExplicitInsideViewDraw(0,0,0));
    assert(ExplicitInsideViewDraw(4,4,4));
    assert(!ExplicitInsideViewDraw(3,4,4));
    assert(!ExplicitInsideViewDraw(-1,-1,-1));
    assert(!ExplicitInsideViewDraw(-2,-2,-2));

    // Sound-disabled real forward/strafe cycles still have physical sole edges.
    assert(GroundBootEvents(.125f,.25f,.25f,.75f,false,0,.65f)==1u);
    assert(GroundBootEvents(.625f,.25f,.25f,.75f,false,.4f,0)==2u);
    assert(GroundBootEvents(.125f,.75f,.25f,.75f,true,0,.65f)==3u);
    // Loop seam and both events within a long simulation tick.
    assert(GroundBootEvents(.875f,.5f,.25f,.75f,false,0,.65f)==1u);
    assert(GroundBootEvents(.875f,1.0f,.25f,.75f,false,0,.65f)==3u);
    assert(GroundBootEvents(.25f,.125f,.25f,.75f,true,0,.65f)==0u);
    assert(GroundBootEvents(.125f,.0f,.25f,.75f,false,0,.65f)==0u);
    assert(GroundBootEvents(.125f,.5f,.25f,.75f,false,0,0)==0u);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    const float inf=std::numeric_limits<float>::infinity();
    for (float v : {nan,inf,-.1f,1.1f})
        assert(GroundBootEvents(v,.5f,.25f,.75f,true,0,.65f)==0u);
    for (float v : {nan,inf,-.1f,0.0f})
        assert(GroundBootEvents(.125f,v,.25f,.75f,true,0,.65f)==0u);
    assert(GroundBootEvents(.125f,.5f,nan,2.0f,true,0,.65f)==0u);
    assert(GroundBootEvents(.125f,.5f,.25f,.75f,true,nan,.65f)==0u);

    // A player transition can move through its outgoing secondary animation.
    // Exactly one dominant producer prevents double stamping through blends.
    for (float weight : {0.0f,.1f,.49f,.5f,.9f,1.0f})
        assert(DominantGroundBootMove(weight,false) != DominantGroundBootMove(weight,true));
    assert(DominantGroundBootMove(.1f,true));
    assert(!DominantGroundBootMove(.1f,false));
    assert(!DominantGroundBootMove(nan,false) && !DominantGroundBootMove(nan,true));

    // Integrate the actual event helper with the actual signed geometry store.
    // This is CPU producer/kernel proof, not an installed actor/contact claim.
    SandField sand;
    const unsigned outgoing=GroundBootEvents(.625f,.25f,.25f,.75f,false,0,.65f);
    assert(DominantGroundBootMove(.1f,true) && outgoing==2u);
    if (outgoing & 2u)
        assert(sand.StampBoot(.0625f,.0625f,0,1,sand.ContactDepth(),sand.ContactRim()));
    assert(sand.HeightOffsetAt(.0625f,.0625f)==-.10f);
    const auto revision=sand.Revision();
    assert(GroundBootEvents(.625f,0,.25f,.75f,false,0,.65f)==0u && sand.Revision()==revision);
    assert(std::isfinite(sand.HeightGradientAt(.1f,.1f)[0]));

    MudField mud;
    mud.Advance(60,1);
    const unsigned incoming=GroundBootEvents(.125f,.25f,.25f,.75f,false,0,.65f);
    assert(DominantGroundBootMove(.9f,false) && incoming==1u);
    if (incoming & 1u) assert(mud.CompressBoot(.0625f,.0625f));
    assert(mud.HeightOffsetAt(.0625f,.0625f)<-.10f);
    assert(mud.HeightOffsetAt(10,10)==0); // no uniform lowering/body burial

    // Pure authored sand may continue through a tile seam. Mixed/hard surfaces
    // still fail every support tile's exact source admission, independent of
    // the UV validity predicate. A sand-looking building texture is excluded.
    assert(SandSourceInterior(.01f,.99f));
    assert(StockSandSurface("SandAbel","pi??????","sand",""));
    assert(StockSandSurface("SandDark","pt??????","sand",""));
    assert(!StockSandSurface("SandBuilding","pisek01*","sand",""));
    assert(!StockSandSurface("Grass","ps??????","sand",""));
    assert(!StockSandSurface("Sand","ps??????","sand","clutter"));
    assert(StockMudSoil("Field","pol","dirt",""));
    assert(StockCultivatedMudSoil("o/pole1.paa","Default","default","normalExt","",.01f,.99f));
    for (const auto world : {"worlds/eden.wrp","worlds/abel.wrp","worlds/cain.wrp","noe/custom.wrp"})
        assert(StockNogovaMudSoil(world,"o/pole2.paa","Default","default","normalExt","",.01f,.99f));
    assert(!StockCultivatedMudSoil("mod/o/pole1.paa","Default","default","normalExt","",.5f,.5f));
    assert(!StockCultivatedMudSoil("o/pole1.paa","Grass","default","normalExt","",.5f,.5f));
    assert(!StockCultivatedMudSoil("o/pole1.paa","Default","default","road","",.5f,.5f));
    assert(!StockCultivatedMudSoil("o/pole1.paa","Default","default","normalExt","grass",.5f,.5f));
    std::puts("Actual player ground event/inside queue policy and signed sand/mud integration PASS.");
}
