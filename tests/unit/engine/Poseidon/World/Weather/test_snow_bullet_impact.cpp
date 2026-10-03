#include <Poseidon/World/Weather/SnowBulletImpact.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <cassert>
#include <limits>
#include <iostream>

namespace Poseidon { Landscape* GLandscape = nullptr; }
using namespace Poseidon;

int main()
{
    const auto valid = [](float speed = 850, float remaining = .35f, bool local = true,
                          bool explosive = false, bool water = false, bool road = false, bool roof = false) {
        return SnowBulletImpactPolicy(speed,8,remaining,1,-1,0,local,explosive,water,road,roof);
    };
    const auto cut = valid();
    assert(cut.radius >= .2f && cut.radius <= .35f && cut.depth > .05f && cut.depth <= .16f);
    assert(valid(850,0).depth == 0 && valid(850,.008f).depth == 0);
    assert(valid(850,.02f).depth <= .02f);
    assert(valid(29).depth == 0 && valid(5001).depth == 0);
    assert(valid(850,.35f,false).depth == 0);
    assert(valid(850,.35f,true,true).depth == 0);
    assert(valid(850,.35f,true,false,true).depth == 0);
    assert(valid(850,.35f,true,false,false,true).depth == 0);
    assert(valid(850,.35f,true,false,false,false,true).depth == 0);
    assert(SnowBulletImpactPolicy(850,8,.35f,.49f,-1,0,true,false,false,false,false).depth == 0);
    assert(SnowBulletImpactPolicy(850,8,.35f,1,-.03f,0,true,false,false,false,false).depth == 0);
    assert(SnowBulletImpactPolicy(850,8,.35f,1,-1,.081f,true,false,false,false,false).depth == 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (int lane = 0; lane < 6; ++lane) {
        float p[] = {850,8,.35f,1,-1,0}; p[lane] = nan;
        assert(SnowBulletImpactPolicy(p[0],p[1],p[2],p[3],p[4],p[5],true,false,false,false,false).depth == 0);
    }
    const auto vertical = SnowBulletGroovePolicy(.35f,1,-1,0,0);
    assert(vertical.length == 0);
    const auto oblique = SnowBulletGroovePolicy(.18f,1,-.274f,0,std::sqrt(1-.274f*.274f));
    assert(oblique.directionX == 0 && oblique.directionZ == 1 && oblique.length > .6f && oblique.length < .64f);
    assert(SnowBulletGroovePolicy(.35f,1,-.04f,1,0).length == .8f);
    assert(SnowBulletGroovePolicy(.35f,.49f,-.5f,.8f,0).length == 0);
    assert(SnowBulletGroovePolicy(.35f,1,-.03f,.8f,0).length == 0);
    for (int lane=0;lane<5;++lane) {
        float p[]={.18f,1,-.274f,0,.96f};p[lane]=nan;
        assert(SnowBulletGroovePolicy(p[0],p[1],p[2],p[3],p[4]).length == 0);
    }
    SnowBulletBudget budget;
    for (int i=0;i<8;++i) assert(budget.Take(10));
    assert(!budget.Take(10.249f)); assert(budget.Take(10.25f));
    assert(budget.Take(0)); assert(!budget.Take(nan) && !budget.Take(-1));

    // This is the actual SnowField, not an independent crater approximation.
    SnowField snow; snow.snowlineEnabled = false;
    assert(!snow.BulletImpact(.0625f,.0625f,cut.radius,cut.depth));
    snow.enabled = true; snow.falling = false; snow.Deposit(.35f);
    assert(snow.BulletImpact(.0625f,.0625f,cut.radius,cut.depth));
    const float centre = snow.DeficitAt(0,0), ring = snow.DeficitAt(1,0);
    assert(centre > .05f && centre <= .16f && ring > 0 && ring < centre);
    assert(snow.DeficitAt(3,0) == 0); // Retained powder rim/outside stays uncut.
    const size_t chunks = snow.Chunks();
    for (int i=0;i<30;++i) snow.BulletImpact(.0625f,.0625f,cut.radius,cut.depth);
    assert(snow.DeficitAt(0,0) <= snow.Depth() && snow.Chunks() == chunks);
    const float saturated = snow.DeficitAt(0,0);
    assert(!snow.BulletImpact(nan,0,cut.radius,cut.depth));
    assert(!snow.BulletImpact(0,0,nan,cut.depth) && !snow.BulletImpact(0,0,cut.radius,nan));
    assert(!snow.BulletImpact(0,0,-1,cut.depth) && !snow.BulletImpact(0,0,cut.radius,-1));
    snow.Advance(1); assert(snow.DeficitAt(0,0) == saturated); // No falling snow.
    auto away = snow.Snapshot(1000,1000); (void)away;
    auto returned = snow.Snapshot(.0625f,.0625f);
    const int sx = int(std::floor((.0625f-returned[0])/SnowField::CellSize));
    const int sz = int(std::floor((.0625f-returned[1])/SnowField::CellSize));
    assert(returned[4+sz*SnowField::WindowSize+sx] == saturated);
    snow.enabled = false; assert(!snow.BulletImpact(.0625f,.0625f,cut.radius,cut.depth));
    snow.enabled = true; assert(snow.DeficitAt(0,0) == saturated);
    snow.Deposit(.10f); assert(std::abs(snow.DeficitAt(0,0)-(saturated-.10f)) < .00001f);
    snow.Deposit(.5f); assert(snow.DeficitAt(0,0) == 0);
    snow.Reset(); assert(snow.Chunks() == 0 && snow.DeficitAt(0,0) == 0);
    // Oblique impacts follow the actual incoming horizontal axis upstream.
    // Rotation/reflection must rotate/refelect stored cells, not the camera.
    SnowField groove;groove.snowlineEnabled=false;groove.enabled=true;groove.falling=false;groove.Deposit(.18f);
    assert(groove.BulletImpact(.0625f,.0625f,cut.radius,cut.depth,0,1,oblique.length));
    assert(groove.DeficitAt(0,-4) == groove.DeficitAt(0,0));
    assert(groove.DeficitAt(0,-6)>0 && groove.DeficitAt(0,3)==0);
    assert(groove.DeficitAt(3,-3)==0);
    SnowField rotated;rotated.snowlineEnabled=false;rotated.enabled=true;rotated.falling=false;rotated.Deposit(.18f);
    assert(rotated.BulletImpact(.0625f,.0625f,cut.radius,cut.depth,-1,0,oblique.length));
    float volume=0;
    for(int iz=-10;iz<=10;++iz)for(int ix=-10;ix<=10;++ix) {
        assert(std::abs(groove.DeficitAt(ix,iz)-rotated.DeficitAt(-iz,ix))<.000001f);
        volume+=groove.DeficitAt(ix,iz)*SnowField::CellSize*SnowField::CellSize;
    }
    // Positive, bounded removed volume; at maximum depth/width/length the
    // continuous capsule is under 0.152m3. This representative rifle is <0.03m3.
    assert(volume>0 && volume<.03f);
    const float intact=groove.DeficitAt(0,-4);
    assert(!groove.BulletImpact(0,0,cut.radius,cut.depth,1,0,.801f));
    assert(!groove.BulletImpact(0,0,cut.radius,cut.depth,2,0,.5f));
    assert(!groove.BulletImpact(0,0,cut.radius,cut.depth,nan,0,.5f));
    groove.Advance(1);assert(groove.DeficitAt(0,-4)==intact);
    auto awayGroove=groove.Snapshot(1000,1000);(void)awayGroove;
    groove.Snapshot(.0625f,.0625f);assert(groove.DeficitAt(0,-4)==intact);
    groove.Deposit(.03f);assert(std::abs(groove.DeficitAt(0,-4)-(intact-.03f))<.000001f);
    std::cout << "Actual SnowField vertical bowls/directional entry grooves, persistence/refill, support admission and query budget PASS\n";
}
