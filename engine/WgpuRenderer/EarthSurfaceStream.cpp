#include "EarthSurfaceStream.hpp"
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <cjson/cJSON.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace Poseidon {
namespace {
constexpr double South=49.60,West=6.12,North=49.62,East=6.14;
double Fade(double d,double width) {const double t=std::clamp(d/width,0.0,1.0);return t*t*(3-2*t);}
std::vector<uint8_t> Read(const std::filesystem::path& path,size_t max) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("prepared surface cache missing");
    const auto n=file.tellg();if(n<=0 || uint64_t(n)>max)throw std::runtime_error("surface cache exceeds byte bound");
    std::vector<uint8_t> result(static_cast<size_t>(n));file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(result.data()),n))throw std::runtime_error("surface cache read failed");return result;
}
using Pixels=std::unique_ptr<stbi_uc,decltype(&stbi_image_free)>;
Pixels Decode(const std::vector<uint8_t>& bytes,int& width,int& height,bool ortho) {
    int channels=0;
    if(bytes.size()<3 || bytes[0]!=0xff || bytes[1]!=0xd8 ||
       !stbi_info_from_memory(bytes.data(),int(bytes.size()),&width,&height,&channels) ||
       width<=0 || height<=0 || (ortho ? (width!=2048 || height!=2048) : (width>5400 || height>2700)))
        throw std::runtime_error("invalid bounded surface JPEG dimensions");
    Pixels pixels(stbi_load_from_memory(bytes.data(),int(bytes.size()),&width,&height,&channels,4),stbi_image_free);
    if(!pixels)throw std::runtime_error("surface JPEG decode failed");return pixels;
}
const char* Text(const cJSON* root,const char* name) {
    auto* p=cJSON_GetObjectItemCaseSensitive(root,name);return cJSON_IsString(p)?p->valuestring:"";
}
double Number(const cJSON* root,const char* name) {
    auto* p=cJSON_GetObjectItemCaseSensitive(root,name);return cJSON_IsNumber(p)?p->valuedouble:NAN;
}
void Validate(const std::vector<uint8_t>& bytes,const std::vector<uint8_t>& receipt) {
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(reinterpret_cast<const char*>(receipt.data()),receipt.size()),cJSON_Delete);
    if(!root || std::string(Text(root.get(),"licence"))!="CC0-1.0" ||
       std::string(Text(root.get(),"layer"))!="ortho_2023" ||
       std::string(Text(root.get(),"provider"))!="Administration du cadastre et de la topographie (ACT)" ||
       Number(root.get(),"south")!=South || Number(root.get(),"west")!=West ||
       Number(root.get(),"north")!=North || Number(root.get(),"east")!=East ||
       Number(root.get(),"width")!=2048 || Number(root.get(),"height")!=2048 ||
       Number(root.get(),"bytes")!=double(bytes.size()) ||
       std::string(Text(root.get(),"sha256"))!=Foundation::Sha256::Of(bytes.data(),bytes.size()))
        throw std::runtime_error("surface receipt/hash/bounds mismatch");
}
}
Earth::Pixel EarthSurfaceStream::LocalUv(const Window& w,double x,double z) {return {(x-w.x)/w.span,1-(z-w.z)/w.span};}
double EarthSurfaceStream::OrthoWeight(const Window& w,double x,double z) {
    if(!std::isfinite(w.span)||w.span<=0)return 0;
    const auto c=Earth::FromLocal(w.latitude,w.longitude,x,z);
    if(c.latitude<South || c.latitude>North || c.longitude<West || c.longitude>East)return 0;
    const double metres=Earth::Radius*Earth::Pi/180.0;
    const double edge=std::min({(c.latitude-South)*metres,(North-c.latitude)*metres,
        (c.longitude-West)*metres*std::cos(c.latitude*Earth::Pi/180.0),
        (East-c.longitude)*metres*std::cos(c.latitude*Earth::Pi/180.0)});
    const double windowEdge=std::min({x-w.x,double(w.x+w.span)-x,z-w.z,double(w.z+w.span)-z});
    return Fade(edge,75)*Fade(windowEdge,200);
}
std::array<double,4> EarthSurfaceStream::Sample(ImageView im,double u,double v,bool repeatU) {
    std::array<double,4> out{};
    if(!im.pixels || im.width<=0 || im.height<=0)return out;
    if(repeatU)u-=std::floor(u);else u=std::clamp(u,0.0,1.0);
    v=std::clamp(v,0.0,1.0);
    const double fx=u*im.width-.5,fy=v*im.height-.5;
    const int ix=int(std::floor(fx)),iy=int(std::floor(fy));
    for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
        const int px=repeatU?((ix+x)%im.width+im.width)%im.width:std::clamp(ix+x,0,im.width-1);
        const int py=std::clamp(iy+y,0,im.height-1);
        const double weight=(x?fx-ix:1-(fx-ix))*(y?fy-iy:1-(fy-iy));
        for(int c=0;c<4;++c)out[c]+=im.pixels[(size_t(py)*im.width+px)*4+c]*weight;
    }
    return out;
}
EarthSurfaceStream::Atlas EarthSurfaceStream::Build(const Window& w,ImageView nasa,ImageView ortho,int size,const std::function<bool()>& cancelled) {
    Atlas result;result.window=w;
    if(!nasa.pixels || !ortho.pixels || size<2 || size>AtlasSize || !std::isfinite(w.latitude) ||
       std::abs(w.latitude)>84 || !std::isfinite(w.longitude) || !std::isfinite(w.span) || w.span<=0 ||
       !std::isfinite(w.x) || !std::isfinite(w.z)) {result.error="invalid surface source/window";return result;}
    // Outside this explicitly prepared cell, preserve the existing NASA draw
    // without allocating or filling a 16 MiB atlas on every rolling window.
    const double metres=Earth::Radius*Earth::Pi/180.0;
    const double west=Earth::Anchor+Earth::WrapLongitude(West-w.longitude)*metres*std::cos(w.latitude*Earth::Pi/180.0);
    const double east=west+(East-West)*metres*std::cos(w.latitude*Earth::Pi/180.0);
    const double south=Earth::Anchor+(South-w.latitude)*metres,north=south+(North-South)*metres;
    if(east<=w.x || west>=w.x+w.span || north<=w.z || south>=w.z+w.span)return result;
    result.pixels.resize(size_t(size)*size*4);
    for(int row=0;row<size;++row) {
        if(cancelled && cancelled()){result.pixels.clear();result.error="superseded";return result;}
        for(int col=0;col<size;++col) {
            const double x=w.x+(col+.5)*w.span/size,z=w.z+(size-row-.5)*w.span/size;
            const auto uv=Earth::OverviewUv(w.latitude,w.longitude,x,z);
            auto color=Sample(nasa,uv.x,uv.y,true);
            const double blend=OrthoWeight(w,x,z);
            if(blend>0) {
                result.cellPresent=true;const auto c=Earth::FromLocal(w.latitude,w.longitude,x,z);
                const auto local=Sample(ortho,(c.longitude-West)/(East-West),(North-c.latitude)/(North-South),false);
                for(int channel=0;channel<3;++channel)color[channel]=color[channel]*(1-blend)+local[channel]*blend;
            }
            for(int c=0;c<3;++c)result.pixels[(size_t(row)*size+col)*4+c]=uint8_t(std::clamp(std::lround(color[c]),0l,255l));
            result.pixels[(size_t(row)*size+col)*4+3]=255;
        }
    }
    return result;
}
EarthSurfaceStream::EarthSurfaceStream(std::filesystem::path cache,std::filesystem::path overview):
    _cache(std::move(cache)),_overview(std::move(overview)) {_worker=std::thread(&EarthSurfaceStream::Run,this);}
EarthSurfaceStream::~EarthSurfaceStream(){_stop=true;++_version;_wake.notify_one();_worker.join();}
void EarthSurfaceStream::Request(const EarthTerrainStream::Patch& p) {
    std::lock_guard lock(_mutex);
    if(_requested && _window.generation==p.generation && _window.revision==p.originRevision)return;
    _window={p.originLatitude,p.originLongitude,p.x,p.z,256*p.gridMetres,p.originRevision,p.generation};
    _requested=true;_pending=true;_ready.reset();++_version;_wake.notify_one();
}
void EarthSurfaceStream::Relocate(){std::lock_guard lock(_mutex);++_version;_pending=false;_requested=false;_ready.reset();}
std::unique_ptr<EarthSurfaceStream::Atlas> EarthSurfaceStream::TakeReady(){std::lock_guard lock(_mutex);return std::move(_ready);}
void EarthSurfaceStream::Run() {
    std::vector<uint8_t> nasa;Pixels ortho(nullptr,stbi_image_free);std::string sourceError;bool loaded=false;
    while(!_stop) {
        Window window;uint64_t version;
        {std::unique_lock lock(_mutex);_wake.wait(lock,[&]{return _stop || _pending;});if(_stop)return;
         window=_window;version=_version;_pending=false;}
        auto result=std::make_unique<Atlas>();result->window=window;
        try {
            if(!loaded) {
                loaded=true;
                // Match the globe's existing 4096x2048 resize exactly. Direct
                // stb ownership avoids a second 56 MiB original-image copy.
                {auto bytes=Read(_overview,MaxOverviewBytes);int w=0,h=0;auto pixels=Decode(bytes,w,h,false);
                 nasa.resize(4096*2048*4);stbir_resize_uint8_linear(pixels.get(),w,h,w*4,nasa.data(),4096,2048,4096*4,STBIR_RGBA);}
                auto bytes=Read(_cache/"surface/lux-ortho-2023.jpg",MaxOrthoBytes);
                Validate(bytes,Read(_cache/"surface/lux-ortho-2023.json",16384));
                int w=0,h=0;ortho=Decode(bytes,w,h,true);
            }
            if(!sourceError.empty())throw std::runtime_error(sourceError);
            *result=Build(window,{nasa.data(),4096,2048},{ortho.get(),2048,2048},AtlasSize,[&]{return _stop || _version!=version;});
        } catch(const std::exception& e) {sourceError=e.what();result->error=sourceError;}
        {std::lock_guard lock(_mutex);if(!_stop && _version==version)_ready=std::move(result);}
    }
}
}
