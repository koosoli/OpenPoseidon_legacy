#include "WgpuRenderer/EarthCoordinates.hpp"
#include "Poseidon/World/Terrain/EarthStreamingMode.hpp"
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
    for (double lat : {-90.0,-46.6,0.0,46.6,90.0})
        for (double lon : {-180.0,8.1,180.0})
        {
            const auto n=GlobeNormal(46.6,8.1,lat,lon);
            assert(std::abs(n.x*n.x+n.y*n.y+n.z*n.z-1.0)<1e-12);
            const auto wrapped=GlobeNormal(46.6,8.1,lat,lon+360);
            assert(std::abs(n.x-wrapped.x)+std::abs(n.y-wrapped.y)+std::abs(n.z-wrapped.z)<1e-12);
        }
    const auto up=GlobeNormal(46.6,8.1,46.6,8.1);
    assert(std::abs(up.x)<1e-12 && std::abs(up.y-1)<1e-12 && std::abs(up.z)<1e-12);
    const auto opposite=GlobeNormal(46.6,8.1,-46.6,188.1);
    assert(std::abs(opposite.y+1)<1e-12);
    using Poseidon::EarthStreaming::ZoomAltitude;
    assert(ZoomAltitude(18000000,1)<18000000 && ZoomAltitude(4500,-1)>4500);
    assert(ZoomAltitude(500,1)==500 && ZoomAltitude(30000000,-1)==30000000);
    assert(ZoomAltitude(NAN,1)==4500 && ZoomAltitude(4500,NAN)==4500);
    assert(std::abs(ZoomAltitude(ZoomAltitude(4500,1),-1)-4500)<.01f);
    assert(SelectNearDetail(8000,NearDetail::Coarse)==NearDetail::Fine);
    assert(SelectNearDetail(12000,NearDetail::Fine)==NearDetail::Coarse);
    assert(SelectNearDetail(10000,NearDetail::Fine)==NearDetail::Fine);
    assert(SelectNearDetail(10000,NearDetail::Coarse)==NearDetail::Coarse);
    assert(SelectNearDetail(NAN,NearDetail::Fine)==NearDetail::Fine);
    assert(SelectNearDetail(-100,NearDetail::Coarse)==NearDetail::Fine);
    // A noisy flight inside the hysteresis band cannot change its source/grid.
    auto detail=NearDetail::Fine;
    for(float altitude : {9990.0f,10010.0f,11000.0f,9000.0f,11999.0f})
    {
        detail=SelectNearDetail(altitude,detail);
        assert(detail==NearDetail::Fine);
    }
    for (auto band : {NearDetail::Coarse,NearDetail::Fine})
    {
        const float grid=NearGrid(band), half=NearHalfWindow(grid), stride=NearRecenter(grid);
        const int zoom=NearZoom(band);
        assert(grid==(band==NearDetail::Fine ? 25.0f : 100.0f));
        assert(zoom==(band==NearDetail::Fine ? 12 : 10));
        assert(2*half==(GridSamples-1)*grid && half==4*stride);
        assert(NearBlendWidth(grid)==40*grid);
        assert(CenterCell(-.01f,stride)==-1 && CenterCell(stride-.01f,stride)==0);
        assert(CenterCell(stride,stride)==1);
        // Same absolute world point is the same geographic sample across a
        // recenter and across the fine/coarse source zooms. The zoom only scales
        // the tile address; neither the map origin nor curvature follows it.
        for(float worldX : {-499999.0f,Anchor,123456.0f,499999.0f})
        {
            const int centreCell=CenterCell(worldX,stride);
            const float centreX=centreCell*stride;
            const float originX=centreX-half;
            assert(worldX>=originX && worldX<=originX+2*half);
            assert(worldX-originX>=half && worldX-originX<half+stride);
            const float localX=worldX-originX;
            const auto fixed=ToPixel(FromLocal(46.6,8.1,worldX,Anchor),zoom);
            const auto recentered=ToPixel(FromLocal(46.6,8.1,originX+localX,Anchor),zoom);
            assert(fixed.x==recentered.x && fixed.y==recentered.y);
            const auto coarse=ToPixel(FromLocal(46.6,8.1,worldX,Anchor),Zoom);
            const float scale=float(1<<(zoom-Zoom));
            assert(std::abs(fixed.x-scale*coarse.x)<1e-7 && std::abs(fixed.y-scale*coarse.y)<1e-7);
        }
        for(float radius : {64000.0f,512000.0f,2048000.0f})
        {
            assert(HorizonOffset(0,radius,half)==-radius);
            assert(HorizonOffset(HorizonSamples-1,radius,half)==radius);
            for(int i=1;i<HorizonSamples;++i)
                assert(HorizonOffset(i,radius,half)>HorizonOffset(i-1,radius,half));
            // Every far boundary vertex lands on exactly every fourth near
            // edge vertex. Both samplers receive identical world coordinates,
            // source zoom and curvature, so their surface heights cannot split.
            for(int i=HorizonInnerBegin;i<=HorizonInnerEnd;++i)
            {
                const int nearIndex=(i-HorizonInnerBegin)*4;
                const float offset=HorizonOffset(i,radius,half);
                assert(offset==-half+nearIndex*grid);
                for(float edge : {-half,half})
                {
                    const double farX=Anchor+offset, farZ=Anchor+edge;
                    const double nearX=Anchor-half+nearIndex*grid, nearZ=Anchor+edge;
                    const auto fp=ToPixel(FromLocal(46.6,8.1,farX,farZ),zoom);
                    const auto np=ToPixel(FromLocal(46.6,8.1,nearX,nearZ),zoom);
                    assert(fp.x==np.x && fp.y==np.y);
                    assert(CurvatureDrop(farX,farZ)==CurvatureDrop(nearX,nearZ));
                    // The transposed edges have the same seam guarantee.
                    const auto ft=ToPixel(FromLocal(46.6,8.1,farZ,farX),zoom);
                    const auto nt=ToPixel(FromLocal(46.6,8.1,nearZ,nearX),zoom);
                    assert(ft.x==nt.x && ft.y==nt.y);
                }
            }
            // Skirt endpoints use precisely the same grid as the native mesh.
            for(int i=0;i<GridSamples;++i)
            {
                const float skirtX=Anchor-half+i*grid;
                assert(skirtX>=Anchor-half && skirtX<=Anchor+half);
            }
        }
    }
    assert(WrapPixel(-1,MaxZoom)==1048575 && WrapPixel(1048576,MaxZoom)==0);
    std::cout<<"Earth coordinates: origin, orientation, seams, elevation, polar guards, curved horizon, adaptive detail/hysteresis PASS\n";
}
