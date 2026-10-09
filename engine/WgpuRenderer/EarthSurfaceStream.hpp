#pragma once
#include "EarthTerrainStream.hpp"
#include "EarthImageryTiles.hpp"
#include <array>
#include <functional>
#include <string>

namespace Poseidon {
// Prepared regional caches, plus streamed provider imagery when one is configured (providers.cfg).
// No gameplay collision.
class EarthSurfaceStream {
public:
    static constexpr int AtlasSize=2048;
    static constexpr size_t MaxOrthoBytes=8*1024*1024,MaxOverviewBytes=16*1024*1024;
    struct Window {
        double latitude=0,longitude=0;
        float x=0,z=0,span=0;
        uint64_t revision=0,generation=0;
        float horizonRadius=0;
    };
    struct ImageView { const uint8_t* pixels=nullptr; int width=0,height=0; };
    struct SourceView {
        ImageView image;
        double south=0,west=0,north=0,east=0;
        double featherMetres=100;
    };
    struct Atlas {
        Window window;
        std::vector<uint8_t> pixels;
        std::vector<uint8_t> farPixels;
        std::string error;
        bool cellPresent=false;
    };
    static Earth::Pixel LocalUv(const Window& w,double x,double z);
    // A fixed patch-centred chart; density increases near the resident camera
    // without changing atlas bytes, geometry or the exact far-atlas boundary.
    static Earth::Pixel NearUv(Earth::Pixel linearUv);
    static Earth::Pixel NearPoint(const Window& w,double u,double v);
    static double OrthoWeight(const Window& w,double x,double z);
    static std::array<double,4> Sample(ImageView image,double u,double v,bool repeatU);
    static Atlas Build(const Window& w,ImageView overview,ImageView ortho,int size=AtlasSize,
                       const std::function<bool()>& cancelled={});
    static Window FarWindow(const Window& window);
    static Atlas BuildRegional(const Window& window,ImageView overview,const std::vector<SourceView>& sources,
                               int size=AtlasSize,const std::function<bool()>& cancelled={});
    EarthSurfaceStream(std::filesystem::path cache,std::filesystem::path overview);
    ~EarthSurfaceStream();
    void Request(const EarthTerrainStream::Patch& patch);
    void Relocate();
    std::unique_ptr<Atlas> TakeReady();
private:
    void Run();
    std::filesystem::path _cache,_overview;
    std::unique_ptr<EarthImageryTiles> _tiles; // null: no imagery provider
    std::thread _worker;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::atomic<bool> _stop{false};
    std::atomic<uint64_t> _version{0};
    bool _pending=false,_requested=false;
    Window _window;
    std::unique_ptr<Atlas> _ready;
};
}
