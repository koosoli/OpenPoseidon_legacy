#pragma once
#include "EarthTerrainStream.hpp"
#include "EarthImageryTiles.hpp"
#include <any>
#include <array>
#include <functional>
#include <memory>
#include <string>

namespace Poseidon {
// Prepared regional caches, plus streamed provider imagery when one is configured (providers.cfg).
// No gameplay collision.
class EarthSurfaceStream {
public:
    static constexpr int AtlasSize=2048;
    // The far atlas spans the whole horizon (up to ~1000 km) and is warped toward its centre (FarUv).
    static constexpr int FarAtlasSize=4096;
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
        // Level 0 first, then the rest of the box-filtered mip chain (mipLevels levels in all), built on the
        // worker: the main thread only hands them to the GPU. Readers of the image use the first
        // AtlasSize^2 (farSize^2) texels.
        std::vector<uint8_t> pixels;
        std::vector<uint8_t> farPixels;
        int farSize=0;
        int mipLevels=1,farMipLevels=1;
        std::string sources; // what fed it, for the log
        std::string error;
        bool cellPresent=false;
        // The window's last atlas (not a coarse preview or a streaming update): what is derived from the
        // imagery (SetDerive) comes with it, computed on the worker.
        bool final=false;
        std::any derived;
    };
    // Work derived from a window's final imagery and its elevation patch (the canopy trees), run on the
    // surface worker instead of the main thread; its result travels in Atlas::derived.
    using Derive=std::function<std::any(const EarthTerrainStream::Patch&,const Window&,const std::vector<uint8_t>&)>;
    void SetDerive(Derive derive){std::lock_guard lock(_mutex);_derive=std::move(derive);}
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
    // The far chart: per-axis log warp around the near window, so texel density is highest just beyond
    // the near atlas and falls off toward the horizon (the near atlas is warped the same way, gentler).
    static double FarWarpK(const Window& w);
    static Earth::Pixel FarUv(const Window& w,double x,double z);
    static Earth::Pixel FarPoint(const Window& w,double u,double v);
    static Atlas BuildRegional(const Window& window,ImageView overview,const std::vector<SourceView>& sources,
                               int size=AtlasSize,const std::function<bool()>& cancelled={},int farSize=0);
    EarthSurfaceStream(std::filesystem::path cache,std::filesystem::path overview);
    ~EarthSurfaceStream();
    void Request(const EarthTerrainStream::Patch& patch);
    // Windows the camera is about to need -- the one under it before its elevation patch has arrived (at
    // start-up and on a jump), and the one ahead along its flight. Their tiles are fetched to memory and
    // disk whenever the worker has no atlas to build; a Request always comes first. Only the window
    // fields that place it are read (latitude, longitude, x, z, span, revision, horizonRadius).
    void Prefetch(const std::vector<Window>& windows);
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
    bool _prefetchPending=false;
    Derive _derive;
    std::shared_ptr<const EarthTerrainStream::Patch> _patch; // the requested window's elevation (heights only)
    std::vector<Window> _prefetch;
    std::atomic<uint64_t> _prefetchSerial{0};
    Window _window;
    std::unique_ptr<Atlas> _ready;
};
}
