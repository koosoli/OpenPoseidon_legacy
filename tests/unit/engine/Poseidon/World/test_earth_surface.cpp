#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize2.h>
#include "WgpuRenderer/EarthSurfaceStream.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <fstream>

using Surface=Poseidon::EarthSurfaceStream;
using namespace Poseidon;
std::unique_ptr<Surface::Atlas> Wait(Surface& worker) {
    for(int i=0;i<1000;++i){if(auto p=worker.TakeReady())return p;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
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
    const double west=Earth::Anchor+(6.12-6.13)*Earth::Pi/180*Earth::Radius*std::cos(49.61*Earth::Pi/180);
    auto wider=window;wider.x=3200;wider.z=3200;wider.span=6400;
    assert(Surface::OrthoWeight(wider,west,6400)<1e-20);
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
    EarthTerrainStream::Patch patch{};patch.originLatitude=49.61;patch.originLongitude=6.13;
    patch.x=patch.z=3200;patch.gridMetres=25;patch.originRevision=3;patch.generation=9;
    {
        Surface missing("absent-surface-cache","absent-overview.jpg");
        missing.Request(patch);patch.originRevision=4;patch.generation=10;missing.Request(patch);
        const auto error=Wait(missing);
        assert(error->window.revision==4 && error->window.generation==10 && !error->error.empty());
        missing.Relocate();assert(!missing.TakeReady());
    }
    if(argc==2) {
        const std::filesystem::path cache=std::filesystem::u8path(argv[1]);
        Surface worker(cache,cache/"blue-marble-june-5400.jpg");
        worker.Request(patch);worker.Relocate();
        patch.originRevision=5;patch.generation=11;worker.Request(patch);
        const auto ready=Wait(worker);
        assert(ready->error.empty() && ready->cellPresent && ready->window.revision==5 && ready->window.generation==11);
        assert(ready->pixels.size()==2048*2048*4);
        std::cout<<"Actual cached ACT/NASA decode/hash/composition PASS; generation 11 revision 5\n";
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
