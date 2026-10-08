#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize2.h>
#include "WgpuRenderer/EarthSurfaceStream.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <fstream>
#include <iomanip>

using Surface=Poseidon::EarthSurfaceStream;
using namespace Poseidon;
std::unique_ptr<Surface::Atlas> Wait(Surface& worker) {
    for(int i=0;i<3000;++i){if(auto p=worker.TakeReady())return p;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    assert(false && "bounded worker timeout");return {};
}
int main(int argc,char** argv) {
    const uint8_t black[]{0,0,0,255};
    const uint8_t northSouth[]{255,0,0,255,255,0,0,255,0,0,255,255,0,0,255,255};
    Surface::Window window{49.61,6.13,5900,5900,1000,3,9};
    const auto uv=Surface::LocalUv(window,5900,6900);
    assert(uv.x==0 && uv.y==0); // North is the image's top row.
    const auto atlas=Surface::Build(window,{black,1,1},{northSouth,2,2},64);
    assert(atlas.error.empty() && atlas.cellPresent && atlas.pixels.size()==64*64*4);
    assert(atlas.pixels[(10*64+32)*4]>atlas.pixels[(10*64+32)*4+2]);
    assert(atlas.pixels[(53*64+32)*4]<atlas.pixels[(53*64+32)*4+2]);
    assert(Surface::OrthoWeight(window,6400,6400)==1);
    const double edge=Surface::OrthoWeight(window,5900,6400),middle=Surface::OrthoWeight(window,6000,6400);
    assert(edge==0 && middle>0 && middle<1);
    const auto west=Earth::ToLocal(49.61,6.13,49.61,6.12);
    auto wider=window;wider.x=3200;wider.z=3200;wider.span=6400;
    assert(Surface::OrthoWeight(wider,west.x,west.z)<1e-20);
    // The same prepared cell remains aligned after an origin shift. Its new
    // projected centre may be far from Anchor; overlap culling must retain it.
    auto shifted=window;shifted.longitude=6.5;
    const auto cellCentre=Earth::ToLocal(shifted.latitude,shifted.longitude,49.61,6.13);
    shifted.x=float(cellCentre.x-500);shifted.z=float(cellCentre.z-500);
    assert(Surface::OrthoWeight(shifted,cellCentre.x,cellCentre.z)>.999999);
    const auto shiftedAtlas=Surface::Build(shifted,{black,1,1},{northSouth,2,2},64);
    assert(shiftedAtlas.cellPresent && shiftedAtlas.error.empty());
    for(double latitude : {49.607,49.613}) {
        const auto oldPoint=Earth::ToLocal(window.latitude,window.longitude,latitude,6.13);
        const auto newPoint=Earth::ToLocal(shifted.latitude,shifted.longitude,latitude,6.13);
        const auto oldUv=Surface::LocalUv(window,oldPoint.x,oldPoint.z);
        const auto newUv=Surface::LocalUv(shifted,newPoint.x,newPoint.z);
        const auto oldColor=Surface::Sample({atlas.pixels.data(),64,64},oldUv.x,oldUv.y,false);
        const auto newColor=Surface::Sample({shiftedAtlas.pixels.data(),64,64},newUv.x,newUv.y,false);
        for(int channel=0;channel<3;++channel)assert(std::abs(oldColor[channel]-newColor[channel])<2);
    }
    // Both altitude bands retain the identical near/far top-surface seam. UVs
    // change only on the split near draw; its boundary is entirely NASA.
    for(float grid:{25.f,100.f}) {
        auto w=window;w.span=grid*256;w.x=w.z=Earth::Anchor-w.span/2;
        for(int i=96;i<=160;++i) {
            const double offset=Earth::HorizonOffset(i,64000,w.span/2);
            const double near=Earth::Anchor-w.span/2+(i-96)*4*grid;
            assert(std::abs(near-(Earth::Anchor+offset))<.001);
            for(auto point:std::array<Earth::Pixel,4>{{{near,w.z},{near,w.z+w.span},{w.x,near},{w.x+w.span,near}}})
                assert(Surface::OrthoWeight(w,point.x,point.y)==0);
        }
    }
    const uint8_t repeat[]{255,0,0,255,0,0,255,255};
    assert(Surface::Sample({repeat,2,1},0,.5,true)==Surface::Sample({repeat,2,1},1,.5,true));
    const auto top=Surface::Sample({northSouth,2,2},.5,0,false);
    assert(top[0]==255 && top[2]==0);
    int rows=0;auto cancelled=Surface::Build(window,{black,1,1},{northSouth,2,2},64,[&]{return ++rows==3;});
    assert(cancelled.pixels.empty() && cancelled.error=="superseded");
    auto invalid=window;invalid.latitude=NAN;
    assert(!Surface::Build(invalid,{black,1,1},{northSouth,2,2},64).error.empty());
    auto remote=window;remote.latitude=0;remote.longitude=179.995;
    assert(Surface::Build(remote,{black,1,1},{northSouth,2,2},64).pixels.empty());
    // A prepared regional pyramid covers the far mesh too; near-window edge
    // pixels converge to its exact sampled colour rather than raw NASA.
    auto regionalWindow=window;regionalWindow.horizonRadius=2000;
    const std::vector<Surface::SourceView> sources{{{northSouth,2,2},49.58,6.08,49.64,6.18,100}};
    const auto regional=Surface::BuildRegional(regionalWindow,{black,1,1},sources,64);
    assert(regional.cellPresent && regional.error.empty() && regional.farPixels.size()==64*64*4);
    const auto farWindow=Surface::FarWindow(regionalWindow);
    for(int row:{0,12,32,63})for(int col:{0,63}) {
        const double x=window.x+(col+.5)*window.span/64,z=window.z+(64-row-.5)*window.span/64;
        const auto fuv=Surface::LocalUv(farWindow,x,z);
        const auto farColor=Surface::Sample({regional.farPixels.data(),64,64},fuv.x,fuv.y,false);
        for(int k=0;k<3;++k)assert(std::abs(regional.pixels[(row*64+col)*4+k]-farColor[k])<=2);
    }
    auto outsideNear=regionalWindow;outsideNear.x=outsideNear.z=9500;outsideNear.horizonRadius=64000;
    assert(Surface::BuildRegional(outsideNear,{black,1,1},sources,64).cellPresent);
    assert(Surface::BuildRegional(remote,{black,1,1},sources,64).pixels.empty());
    int cancelRows=0;const auto regionalCancelled=Surface::BuildRegional(regionalWindow,{black,1,1},sources,64,[&]{return ++cancelRows==68;});
    assert(regionalCancelled.error=="superseded" && regionalCancelled.pixels.empty() && regionalCancelled.farPixels.empty());
    auto badSource=sources;badSource[0].featherMetres=0;
    assert(!Surface::BuildRegional(regionalWindow,{black,1,1},badSource,64).error.empty());
    EarthTerrainStream::Patch patch{};patch.originLatitude=49.61;patch.originLongitude=6.13;
    patch.x=patch.z=3200;patch.gridMetres=25;patch.originRevision=3;patch.generation=9;patch.horizonRadius=64000;
    {
        Surface missing("absent-surface-cache","absent-overview.jpg");
        missing.Request(patch);patch.originRevision=4;patch.generation=10;missing.Request(patch);
        const auto error=Wait(missing);
        assert(error->window.revision==4 && error->window.generation==10 && !error->error.empty());
        missing.Relocate();assert(!missing.TakeReady());
    }
    if(argc==2) {
        const std::filesystem::path cache=std::filesystem::u8path(argv[1]);
        const auto coldStart=std::chrono::steady_clock::now();
        Surface worker(cache,cache/"blue-marble-june-5400.jpg");
        worker.Request(patch);worker.Relocate();
        patch.originRevision=5;patch.generation=11;worker.Request(patch);
        const auto ready=Wait(worker);
        const double coldSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-coldStart).count();
        assert(ready->error.empty() && ready->cellPresent && ready->window.revision==5 && ready->window.generation==11);
        assert(ready->pixels.size()==2048*2048*4);
        assert(ready->farPixels.size()==2048*2048*4);
        // The regional JPEG's white exterior is missing national coverage, not
        // white terrain. It must yield the NASA fallback beyond that boundary.
        const auto outsidePoint=Earth::ToLocal(49.61,6.13,49.95,6.4);
        const auto fuv=Surface::LocalUv(Surface::FarWindow(ready->window),outsidePoint.x,outsidePoint.z);
        const auto outsideColor=Surface::Sample({ready->farPixels.data(),2048,2048},fuv.x,fuv.y,false);
        assert(outsideColor[0]<220 && outsideColor[1]<220 && outsideColor[2]<220);
        std::cout<<"Actual cached ACT/NASA decode/hash/composition PASS; generation 11 revision 5\n";
        const auto warmStart=std::chrono::steady_clock::now();
        patch.x+=800;patch.generation=12;worker.Request(patch);
        const auto warmReady=Wait(worker);
        const double warmSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-warmStart).count();
        assert(warmReady->error.empty() && warmReady->cellPresent && warmReady->window.generation==12);
        assert(warmReady->pixels.size()==2048*2048*4 && warmReady->farPixels.size()==2048*2048*4);
        std::cout<<std::fixed<<std::setprecision(3)<<"EARTH_SURFACE_TIMING {\"coldWorkerSeconds\":"<<coldSeconds
            <<",\"warmRecenterSeconds\":"<<warmSeconds<<",\"atlasSize\":2048,\"pairedPixels\":8388608}\n";
        // An invalid receipt never publishes pixels. Reusing the real cached
        // source tests the production validation path without another request.
        const auto bad=std::filesystem::temp_directory_path()/"openposeidon-surface-invalid-receipt";
        std::filesystem::create_directories(bad/"surface");
        std::filesystem::copy_file(cache/"surface/lux-ortho-2023.jpg",bad/"surface/lux-ortho-2023.jpg",std::filesystem::copy_options::overwrite_existing);
        {std::ofstream receipt(bad/"surface/lux-ortho-2023.json");receipt<<"{\"licence\":\"CC0-1.0\",\"sha256\":\"wrong\"}";}
        {
            Surface invalidReceipt(bad,cache/"blue-marble-june-5400.jpg");invalidReceipt.Request(patch);
            const auto rejected=Wait(invalidReceipt);
            assert(rejected->pixels.empty() && rejected->error=="surface receipt/hash/bounds mismatch");
        }
        std::filesystem::remove(bad/"surface/lux-ortho-2023.jpg");std::filesystem::remove(bad/"surface/lux-ortho-2023.json");
        std::filesystem::remove(bad/"surface");std::filesystem::remove(bad);
    }
    std::cout<<"Earth surface orientation, feather, geometry seam, dateline, cancellation and latest-publication PASS\n";
}
