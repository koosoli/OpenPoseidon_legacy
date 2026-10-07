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
    assert(CurvatureDrop(Anchor,Anchor)==0);
    assert(std::abs(CurvatureDrop(Anchor+100000,Anchor)-784.0)<1.0);
    assert(CurvatureDrop(Anchor+100,Anchor)>0 && CurvatureDrop(Anchor+100,Anchor)<.001);
    assert(std::abs(CurvatureDrop(Anchor+100000,Anchor)-CurvatureDrop(Anchor,Anchor-100000))<1e-9);
    assert(HorizonRadius(NAN)==64000 && HorizonRadius(-100)==64000);
    assert(HorizonRadius(4500)>HorizonRadius(1000));
    assert(HorizonRadius(250000)==2048000 && HorizonRadius(1e20f)==2048000);
    for (float altitude : {500.0f,4500.0f,30000.0f,120000.0f,250000.0f})
    {
        const float radius=HorizonRadius(altitude);
        // Coverage reaches beyond the real geometric horizon, without growing
        // the per-frame mesh budget with altitude.
        assert(radius>std::sqrt(double(altitude)*(2*Radius+altitude)));
        assert(HorizonOffset(0,radius)==-radius);
        assert(HorizonOffset(HorizonSamples-1,radius)==radius);
        assert(HorizonOffset(HorizonInnerBegin,radius)==-HalfWindow);
        assert(HorizonOffset(HorizonInnerEnd,radius)==HalfWindow);
        for(int i=1;i<HorizonSamples;++i)
            assert(HorizonOffset(i,radius)>HorizonOffset(i-1,radius));
        assert(HorizonZoom(46.6,radius)>=3 && HorizonZoom(46.6,radius)<=Zoom);
        assert(std::isfinite(CurvatureDrop(Anchor+radius,Anchor+radius)));
    }
    assert(ToPixel({0,0},3).x==1024 && WrapPixel(-1,3)==2047);
    std::cout<<"Earth coordinates: origin, orientation, seams, elevation, polar guards, curved horizon coverage PASS\n";
}
