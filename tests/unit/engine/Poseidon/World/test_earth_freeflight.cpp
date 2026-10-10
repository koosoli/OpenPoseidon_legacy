#include "Poseidon/World/Terrain/EarthFreeFlight.hpp"
#include <cassert>
#include <iostream>
using namespace Poseidon::EarthStreaming;

bool Close(double a, double b, double tolerance=.002) { return std::abs(a-b)<tolerance; }
GroundPatch Flat(float height, float x=6400, float z=6400)
{ return {x,z,10,2,1,1,{height,height,height,height}}; }
int main()
{
    auto lowland=Flat(300);
    FlightPoint previous{6405,600,6405};
    auto near=ConstrainFlight(previous,{6405,200,6405},&lowland,1);
    assert(Close(near.y,300.3) && near.y<500); // actual user regression
    for(int i=0;i<120;++i)
    {
        near=ConstrainFlight(near,{near.x,near.y-100,near.z},&lowland,1);
        assert(Close(near.y,300.3)); // held descent/large dt cannot tunnel
    }
    auto mountain=Flat(8000), sea=Flat(0), below=Flat(-100);
    assert(Close(ConstrainFlight({6405,8100,6405},{6405,7000,6405},&mountain,1).y,8000.3));
    assert(Close(ConstrainFlight({6405,100,6405},{6405,-100,6405},&sea,1).y,.3));
    assert(Close(ConstrainFlight({6405,100,6405},{6405,-500,6405},&below,1).y,-99.7));
    assert(Close(FlightSpeedScale({6405,8001,6405},&mountain,1),FlightSpeedScale({6405,1,6405},&sea,1)));
    assert(FlightSpeedScale({6405,8001,6405},&mountain,1)<1.04); // no mountain speed boost
    assert(FlightSpeedScale({6405,18000000,6405},nullptr,1)>200000); // retain orbital travel
    assert(std::abs(FlightSpeedScale({6405,18000000,6405},&lowland,1)-FlightSpeedScale({6405,18000000,6405},nullptr,1))<5);
    // Higher is always faster: no plateau anywhere between the ground and orbit.
    float lastScale = 0;
    for (float clearance = 1; clearance < 5.0e6f; clearance *= 1.25f)
    {
        const float scale = FlightSpeedScale({6405,clearance,6405},&sea,1);
        assert(scale > lastScale);
        lastScale = scale;
    }

    GroundPatch triangles{0,0,10,2,1,1,{100,110,120,250}};
    assert(Close(ConstrainFlight({2.5,200,2.5},{2.5,0,2.5},&triangles,1).y,107.8));
    assert(Close(ConstrainFlight({7.5,200,7.5},{7.5,0,7.5},&triangles,1).y,182.8));
    auto edge=ConstrainFlight({6409,301,6405},{6411,299,6405},&lowland,1);
    assert(edge.x==6409 && Close(edge.y,300.3)); // hold covered side, no500m snap
    auto wait=ConstrainFlight(near,{near.x+1,near.y-1,near.z},nullptr,2);
    assert(wait.x==near.x && wait.y==near.y); // retired support, descent waits
    auto ascent=ConstrainFlight(near,{near.x+1,near.y+1,near.z},nullptr,2);
    assert(ascent.x==near.x && ascent.y==near.y+1); // still escape upward
    assert(ConstrainFlight({6405,1000,6405},{6405,0,6405},nullptr,1).y==500);
    assert(ConstrainFlight({6405,1000,6405},{6405,0,6405},&lowland,2).y==500); // stale patch cannot establish ground

    const float x=100000,z=6400,datum=FlightYAtAltitude(x,z,0);
    auto curved=Flat(datum,x,z);
    auto curvedNear=ConstrainFlight({x,1000,z},{x,datum-50,z},&curved,1);
    assert(Close(FlightAltitude(curvedNear),.3)); // local negative Y is not radial altitude
    const auto fallback=ConstrainFlight({x,1000,z},{x,datum,z},nullptr,1);
    assert(Close(FlightAltitude(fallback),500) && fallback.y<500);
    const auto up=FlightUp(curvedNear);
    assert(Close(std::hypot(std::hypot(up.x,up.z),up.y),1,1e-12) && up.x>0 && up.y>0);

    const auto wheelIn=ZoomFlight(near,1,&lowland,1), wheelOut=ZoomFlight(near,-1,&lowland,1);
    assert(Close(wheelIn.y,300.3));
    assert(wheelOut.y>near.y && wheelOut.y-near.y<1); // precise at ground
    assert(ZoomFlight(near,-1,nullptr,2).y-near.y<1); // retired patch cannot jump to500
    auto above=FlightPoint{6405,330,6405};
    assert(Close(ZoomFlight(ZoomFlight(above,1,&lowland,1),-1,&lowland,1).y,above.y));
    assert(Close(ZoomFlight({6405,18000000,6405},1,&lowland,1).y/18000000.0,.72,.00001));
    assert(ZoomFlight(near,NAN,&lowland,1).y==near.y);
    assert(ConstrainFlight(near,{NAN,0,0},&lowland,1).y==near.y);

    assert(!ValidateLocation(49.61,6.13,499) && ValidateLocation(49.61,6.13,500));
    assert(ValidateFlightLocation(49.61,6.13,-100) && !ValidateFlightLocation(49.61,6.13,-12001));
    LocationState state;
    assert(state.Rebase(1,49.61,6.13,300.3));
    assert(Close(state.Get().altitude,300.3));
    assert(!state.Rebase(1,49.61,6.13,500)); // explicit Go/rebase revision still wins
    assert(!state.Set(49.61,6.13,300.3));
    for(double altitude : {-100.0,123.7,300.3})
    {
        const float rebaseX=CameraAnchor+32001;
        const float oldY=FlightYAtAltitude(rebaseX,CameraAnchor,altitude);
        const auto frame=Poseidon::Earth::CameraRebase({49.61,6.13},rebaseX,oldY,CameraAnchor);
        assert(frame && Close(frame->altitude,altitude));
        const auto revision=state.Get().revision;
        assert(state.Rebase(revision,frame->origin.latitude,frame->origin.longitude,frame->altitude));
        assert(Close(state.Get().altitude,altitude)); // physical rebase never promotes it to500
    }
    std::cout<<"Earth freeflight: resident triangle+.30m, high/negative/curved ground, clearance speed, radial up, coverage guards, precise wheel, Go/rebase separation PASS\n";
}
