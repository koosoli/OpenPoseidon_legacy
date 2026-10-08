#include "WgpuRenderer/EarthCoordinates.hpp"
#include "WgpuRenderer/EarthRenderBathymetry.hpp"
#include "Poseidon/World/Terrain/EarthStreamingMode.hpp"
#include <cassert>
#include <iostream>

using namespace Poseidon::Earth;

// Independent global ECEF reference (Y is the north pole), used to prove that
// an origin shift changes only coordinates, not the camera's physical pose.
Direction Ecef(Coordinate origin, Direction local)
{
    const double lat=origin.latitude*Pi/180,lon=origin.longitude*Pi/180;
    return {-std::sin(lon)*local.x+std::cos(lat)*std::cos(lon)*local.y-std::sin(lat)*std::cos(lon)*local.z,
        std::sin(lat)*local.y+std::cos(lat)*local.z,
        std::cos(lon)*local.x+std::cos(lat)*std::sin(lon)*local.y-std::sin(lat)*std::sin(lon)*local.z};
}
double Difference(Direction a,Direction b)
{
    return std::hypot(std::hypot(a.x-b.x,a.y-b.y),a.z-b.z);
}

int main()
{
    const auto origin = FromLocal(46.6,8.1,Anchor,Anchor);
    assert(std::abs(origin.latitude-46.6)<1e-12 && std::abs(origin.longitude-8.1)<1e-12);
    assert(FromLocal(46.6,8.1,Anchor,Anchor+10000).latitude>origin.latitude);
    assert(FromLocal(46.6,8.1,Anchor+10000,Anchor).longitude>origin.longitude);
    for(double lat : {-84.0,-49.45,0.0,49.45,84.0})
        for(double lon : {-179.99,8.1,179.99})
            for(double dx : {-2048000.0,-32001.0,0.0,32001.0,2048000.0})
                for(double dz : {-2048000.0,0.0,2048000.0})
                {
                    const auto c=FromLocal(lat,lon,Anchor+dx,Anchor+dz);
                    const auto local=ToLocal(lat,lon,c.latitude,c.longitude);
                    assert(local.visible);
                    assert(std::abs(local.x-Anchor-dx)<1e-7 && std::abs(local.z-Anchor-dz)<1e-7);
                    const auto n=GlobeNormal(lat,lon,c.latitude,c.longitude);
                    // Source addressing, OSM inverse and rendered curvature all
                    // identify exactly the same point on the spherical datum.
                    assert(std::abs(Radius*(n.y-1)+CurvatureDrop(Anchor+dx,Anchor+dz))<1e-7);
                }
    assert(!ToLocal(0,0,0,180).visible && !ToLocal(0,0,NAN,0).visible);
    bool outsideHemisphere=false;
    try { FromLocal(0,0,Anchor+Radius,Anchor); } catch(const std::out_of_range&) {outsideHemisphere=true;}
    assert(outsideHemisphere);
    // Bounding rejection must not lose a source corner when projected at high
    // latitude, across the dateline, or from a different geographic frame.
    for(Coordinate frame : {Coordinate{49.6,6.1},Coordinate{84,179.99},Coordinate{-40,-179.99}})
        for(double west : {-180.01,6.1,179.99})
            for(double lat : {-50.0,49.6,83.9})
                for(double cornerLat : {lat,lat+.02})
                    for(double cornerLon : {west,west+.02})
                    {
                        const auto corner=ToLocal(frame.latitude,frame.longitude,cornerLat,cornerLon);
                        assert(GeographicBoundsOverlap(frame,lat,west,lat+.02,west+.02,corner.x-.5,corner.z-.5,1));
                    }
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
    for (float x : {Anchor,8000.0f,25000.0f})
    {
        const float datum=-float(CurvatureDrop(x,4800));
        // Every ordinary wave trough now lies above opaque ocean background.
        const float floor=RenderTerrainHeight(x,4800,datum);
        assert(std::abs(floor-(datum-200))<.001f);
        for (float wave : {-4.0f,-1.0f,0.0f,4.0f}) assert(datum+wave>floor);
        for (float elevation : {1.0f,50.0f,-20.0f,-400.0f})
            assert(RenderTerrainHeight(x,4800,datum+elevation)==datum+elevation);
    }
    // Both triangles meet at the same continuous render-only shoreline floor.
    // Original source-backed gameplay support remains untouched, including
    // the near-zero source heights which cannot establish physical bathymetry.
    const std::vector<float> source={0,0,0,10};
    Poseidon::EarthStreaming::GroundPatch actual{Anchor,Anchor,25,2,1,1,source};
    auto visual=actual;
    for(int z=0;z<2;++z)for(int x=0;x<2;++x)
        visual.heights[z*2+x]=RenderTerrainHeight(Anchor+x*25,Anchor+z*25,source[z*2+x]);
    assert(actual.Sample(Anchor+5,Anchor+5)->height==0);
    assert(visual.Sample(Anchor+5,Anchor+5)->height<-199);
    assert(actual.Sample(Anchor+15,Anchor+15)->height>1.9f);
    assert(visual.Sample(Anchor+15,Anchor+15)->height<-157);
    assert(std::abs(visual.Sample(Anchor+12.5f-.001f,Anchor+12.5f)->height-
                    visual.Sample(Anchor+12.5f+.001f,Anchor+12.5f)->height)<.05f);
    assert(actual.heights==source);
    // The overview's real equator/prime-meridian grid vertex is exactly at the
    // tangent origin. Leaving it at 0 would still depth-clip a -4 m water trough
    // even after lowering the near/far zero-height meshes.
    const auto overviewUp=GlobeNormal(0,0,0,0);
    const double originalOverview=Radius*(overviewUp.y-1);
    const double submergedOverview=(Radius-RenderOceanDepth)*overviewUp.y-Radius;
    assert(originalOverview==0 && -4<originalOverview && -4>submergedOverview);
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
    using namespace Poseidon::EarthStreaming;
    assert(CameraAnchor==Anchor);
    LocationState locations;
    const auto initial=locations.Get();
    assert(locations.IsCurrent(initial.revision));
    for (const auto& invalid : {Location{85,0,4500},Location{0,181,4500},Location{0,0,499},
            Location{NAN,0,4500},Location{0,INFINITY,4500},Location{0,0,INFINITY}})
    {
        assert(!locations.Set(invalid.latitude,invalid.longitude,invalid.altitude));
        assert(locations.IsCurrent(initial.revision));
    }
    assert(locations.Set(-16.8,180,12000));
    const auto dateline=locations.Get();
    assert(dateline.longitude==-180 && dateline.latitude==-16.8 && dateline.altitude==12000);
    assert(!locations.IsCurrent(initial.revision) && locations.IsCurrent(dateline.revision));
    assert(ToPixel(FromLocal(dateline.latitude,dateline.longitude,Anchor+10000,Anchor),MaxZoom).x>=0);
    const auto leftUv=OverviewUv(dateline.latitude,dateline.longitude,Anchor-25,Anchor);
    const auto rightUv=OverviewUv(dateline.latitude,dateline.longitude,Anchor+25,Anchor);
    assert(leftUv.x<0 && rightUv.x>0 && rightUv.x-leftUv.x<.00001);
    const auto plus180=OverviewUv(0,180,Anchor+25,Anchor);
    const auto minus180=OverviewUv(0,-180,Anchor+25,Anchor);
    assert(std::abs(plus180.x-minus180.x-1.0)<1e-12);
    assert(locations.Set(36.1,-112.1,6000));
    const auto canyon=locations.Get();
    assert(canyon.revision==dateline.revision+1 && !locations.IsCurrent(dateline.revision));
    // Even a repeated Go invalidates a prior ready patch and resets the camera.
    assert(locations.Set(canyon.latitude,canyon.longitude,canyon.altitude));
    assert(!locations.IsCurrent(canyon.revision));
    // Camera-only rebase has a deadband. A 32 km rolling window never changes
    // geographic revision; neither normal patch recenter nor stationary flight
    // causes ground invalidation or a new provider request.
    assert(!CameraRebase({49.45,-2.55},Anchor+32000,1000,Anchor));
    assert(!CameraRebase({49.45,-2.55},NAN,1000,Anchor));
    assert(RecenterDistance(500)==32000 && RecenterDistance(100000)==32000);
    assert(RecenterDistance(1000000)==250000 && RecenterDistance(30000000)==400000);
    assert(!CameraRebase({0,0},Anchor+400000,18000000,Anchor));
    for(Coordinate frame : {Coordinate{0,179.99},Coordinate{49.45,-2.55},Coordinate{-70,150}})
        for(double altitude : {500.0,1000.0,9000.0,18000000.0})
        {
            const Direction point{RecenterDistance(altitude)+10000,Radius+altitude,-12000};
            const auto next=CameraRebase(frame,Anchor+point.x,altitude,Anchor+point.z);
            assert(next);
            assert(Difference(Ecef(frame,point),Ecef(next->origin,{0,Radius+next->altitude,0}))<1e-7);
            for(Direction v : {Direction{1,0,0},Direction{0,1,0},Direction{0,0,1},Direction{5,-3,8}})
            {
                const auto moved=RotateFrame(frame,next->origin,v);
                assert(Difference(Ecef(frame,v),Ecef(next->origin,moved))<1e-12);
                assert(Difference(v,RotateFrame(next->origin,frame,moved))<1e-12);
            }
        }
    LocationState travel({0,179.8,1000});
    // Synthetic constant-altitude waypoints make a complete equatorial circuit
    // and return to the starting origin, crossing the antimeridian twice. The
    // camera itself moves toward its view; it does not auto-level or auto-orbit.
    for(int step=1;step<=900;++step)
    {
        const auto before=travel.Get();
        const Coordinate waypoint{0,WrapLongitude(179.8+.4*step)};
        const auto n=GlobeNormal(before.latitude,before.longitude,waypoint.latitude,waypoint.longitude);
        const double r=Radius+1000;
        const auto next=CameraRebase({before.latitude,before.longitude},Anchor+n.x*r,n.y*r-Radius,Anchor+n.z*r);
        assert(next && std::abs(next->altitude-1000)<1e-7);
        assert(std::abs(WrapLongitude(next->origin.longitude-waypoint.longitude))<1e-10);
        GroundPatch patch{Anchor,Anchor,25,2,before.revision,1,{100,101,102,103}};
        assert(travel.PublishGround(patch));
        const auto immutable=travel.Ground();
        assert(travel.Rebase(before.revision,next->origin.latitude,next->origin.longitude,next->altitude));
        assert(!travel.QueryGround(Anchor,Anchor) && !travel.PublishGround(patch));
        assert(!travel.Rebase(before.revision,0,0,1000)); // stale free flight cannot overwrite a Go
        assert(immutable->Sample(Anchor,Anchor)->height==100);
        assert(travel.Get().revision==before.revision+1);
    }
    assert(std::abs(WrapLongitude(travel.Get().longitude-179.8))<1e-10);
    const auto polarRevision=travel.Get().revision;
    assert(!travel.Rebase(polarRevision,85,0,1000) && travel.IsCurrent(polarRevision));
    assert(travel.Set(49.45,-2.55,1000));
    const auto explicitGo=travel.Get();
    const GroundPatch newGround{Anchor,Anchor,25,2,explicitGo.revision,1,{300,300,300,300}};
    assert(travel.PublishGround(newGround));
    assert(!travel.Rebase(polarRevision,0,0,1000));
    assert(travel.Get().latitude==49.45 && travel.Get().longitude==-2.55);
    assert(travel.QueryGround(Anchor,Anchor)->height==300); // stale flight cannot clear new support
    rejected=false;
    try { LocationState invalid({90,0,4500}); } catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
    std::cout<<"Earth coordinates: spherical projection/inverse, ENU pose continuity, 900-rebase circumnavigation, stale ground/origin rejection, atlas bounds, seams, elevation, polar guards, curved horizon, render-only ocean proxy, adaptive detail PASS\n";
}
