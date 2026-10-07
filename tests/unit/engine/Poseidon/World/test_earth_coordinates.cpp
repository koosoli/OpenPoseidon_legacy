#include "WgpuRenderer/EarthCoordinates.hpp"
#include <cassert>
#include <iostream>

using namespace Poseidon::Earth;
int main()
{
    const auto origin = FromLocal(46.6,8.1,Anchor,Anchor);
    assert(std::abs(origin.latitude-46.6)<1e-12 && std::abs(origin.longitude-8.1)<1e-12);
    assert(FromLocal(46.6,8.1,Anchor,Anchor+10000).latitude>origin.latitude);
    assert(FromLocal(46.6,8.1,Anchor+10000,Anchor).longitude>origin.longitude);
    const auto equator=ToPixel({0,0});
    assert(equator.x==131072 && std::abs(equator.y-131072)<1e-7);
    assert(std::abs(ToPixel({0,180}).x-ToPixel({0,-180}).x)<1e-9);
    assert(WrapPixel(-1)==262143 && WrapPixel(262144)==0);
    assert(Terrarium(128,0,0)==0 && Terrarium(127,255,128)==-.5f);
    assert(Terrarium(129,44,0)==300 && Terrarium(128,0,255)==255.0f/256);
    assert(CenterCell(-.01f)==-1 && CenterCell(3199)==0 && CenterCell(3200)==1);
    // Recentring a render window must not move the Earth sample underneath it.
    const float world=12800;
    const auto a=ToPixel(FromLocal(46.6,8.1,world,6400));
    const auto b=ToPixel(FromLocal(46.6,8.1,(world-3200)+3200,6400));
    assert(a.x==b.x && a.y==b.y);
    bool rejected=false;
    try { ToPixel({90,0}); } catch(const std::out_of_range&) { rejected=true; }
    assert(rejected);
    rejected=false;
    try { ToPixel({NAN,0}); } catch(const std::out_of_range&) { rejected=true; }
    assert(rejected);
    std::cout<<"Earth coordinates: origin, orientation, dateline, seams, elevation, polar guards PASS\n";
}
