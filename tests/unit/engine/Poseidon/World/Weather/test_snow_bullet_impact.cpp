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
    std::cout << "Actual SnowField bullet bowl, persistence/refill, support admission and query budget PASS\n";
}
