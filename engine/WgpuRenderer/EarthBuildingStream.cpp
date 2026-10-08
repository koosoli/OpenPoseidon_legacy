#include "EarthBuildingStream.hpp"
#include <cjson/cJSON.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace Poseidon
{
namespace
{
struct Point { double x,z; };
double Cross(Point a,Point b,Point c) { return (b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x); }
bool On(Point a,Point b,Point p) {
    return std::abs(Cross(a,b,p))<1e-7 && p.x>=std::min(a.x,b.x)-1e-7 && p.x<=std::max(a.x,b.x)+1e-7 &&
        p.z>=std::min(a.z,b.z)-1e-7 && p.z<=std::max(a.z,b.z)+1e-7;
}
bool Intersects(Point a,Point b,Point c,Point d) {
    return (Cross(a,b,c)*Cross(a,b,d)<0 && Cross(c,d,a)*Cross(c,d,b)<0) ||
        On(a,b,c)||On(a,b,d)||On(c,d,a)||On(c,d,b);
}
bool Triangulate(std::vector<Point>& p,std::vector<uint32_t>& triangles) {
    const size_t n=p.size(); double area=0;
    for(size_t i=0;i<n;++i) area+=p[i].x*p[(i+1)%n].z-p[(i+1)%n].x*p[i].z;
    if(std::abs(area)<2 || std::abs(area)>180000) return false;
    if(area<0) std::reverse(p.begin(),p.end());
    for(size_t i=0;i<n;++i) for(size_t j=i+1;j<n;++j) {
        if(j==i+1 || (i==0 && j==n-1)) continue;
        if(Intersects(p[i],p[(i+1)%n],p[j],p[(j+1)%n])) return false;
    }
    std::vector<uint32_t> active(n);std::iota(active.begin(),active.end(),0);
    while(active.size()>3) {
        bool cut=false;
        for(size_t i=0;i<active.size();++i) {
            const auto a=active[(i+active.size()-1)%active.size()],b=active[i],c=active[(i+1)%active.size()];
            if(Cross(p[a],p[b],p[c])<=1e-7) continue;
            bool contains=false;
            for(auto v:active) if(v!=a && v!=b && v!=c &&
                Cross(p[a],p[b],p[v])>=-1e-7 && Cross(p[b],p[c],p[v])>=-1e-7 && Cross(p[c],p[a],p[v])>=-1e-7) {contains=true;break;}
            if(contains) continue;
            triangles.insert(triangles.end(),{a,c,b}); // X/Z CCW has upward Y after reversal.
            active.erase(active.begin()+i);cut=true;break;
        }
        if(!cut) return false;
    }
    triangles.insert(triangles.end(),{active[0],active[2],active[1]});return true;
}
const char* Text(const cJSON* object,const char* key) {
    const auto* value=cJSON_GetObjectItemCaseSensitive(object,key);
    return cJSON_IsString(value)?value->valuestring:nullptr;
}
double Number(const cJSON* object,const char* key) {
    const auto* value=cJSON_GetObjectItemCaseSensitive(object,key);
    return cJSON_IsNumber(value)?value->valuedouble:NAN;
}
double TagMetres(const cJSON* object,const char* key) {
    const char* value=Text(object,key);if(!value) return NAN;
    char* end=nullptr;double number=std::strtod(value,&end);
    while(end && *end==' ')++end;
    if(!end || end==value || (*end && std::string(end)!="m") || !std::isfinite(number)) return NAN;
    return number;
}
float Height(const EarthBuildingStream::Ground& g,Point p) {
    const double x=(p.x-g.x)/g.grid,z=(p.z-g.z)/g.grid;
    if(x<0 || z<0 || x>256 || z>256) throw std::out_of_range("outside resident terrain");
    const int ix=std::min(255,int(x)),iz=std::min(255,int(z));
    const float a=g.heights[iz*257+ix],b=g.heights[iz*257+ix+1],c=g.heights[(iz+1)*257+ix],d=g.heights[(iz+1)*257+ix+1];
    const float y=float((a*(1-(x-ix))+b*(x-ix))*(1-(z-iz))+(c*(1-(x-ix))+d*(x-ix))*(z-iz));
    if(!std::isfinite(y))throw std::out_of_range("invalid terrain");return y;
}
}
EarthBuildingStream::Cell EarthBuildingStream::CellAt(double latitude,double longitude) {
    if(!std::isfinite(latitude)||!std::isfinite(longitude)||std::abs(latitude)>84)throw std::out_of_range("invalid cell");
    return {int(std::floor(latitude/CellDegrees)),int(std::floor(Earth::WrapLongitude(longitude)/CellDegrees))};
}
std::string EarthBuildingStream::FileName(Cell cell) {
    return std::to_string(cell.latitude)+"_"+std::to_string(cell.longitude)+".json";
}
EarthBuildingStream::Mesh EarthBuildingStream::BuildCell(const std::string& json,const Ground& g,Cell cell) {
    Mesh result;result.cell=cell;result.revision=g.revision;result.generation=g.generation;
    if(json.size()>MaxBytes || g.heights.size()!=257*257 || !std::isfinite(g.grid)||g.grid<=0 ||
        !std::isfinite(g.latitude)||!std::isfinite(g.longitude)||std::abs(g.latitude)>84) {result.error="invalid bounded cell/terrain";return result;}
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(json.data(),json.size()),cJSON_Delete);
    if(!root){result.error="invalid cached JSON";return result;}
    const auto* elements=cJSON_GetObjectItemCaseSensitive(root.get(),"elements");
    if(!cJSON_IsArray(elements)){result.error="missing elements";return result;}
    size_t visited=0;
    const cJSON* element=nullptr;
    cJSON_ArrayForEach(element,elements) {
        if(++visited>MaxBuildings)break;
        const auto* tags=cJSON_GetObjectItemCaseSensitive(element,"tags");
        const char* kind=Text(element,"type"),*building=Text(tags,"building");
        const auto* geometry=cJSON_GetObjectItemCaseSensitive(element,"geometry");
        const int count=cJSON_GetArraySize(geometry);
        if(!kind || std::string(kind)!="way" || !building || std::string(building)=="no" ||
            !cJSON_IsArray(geometry)||count<4||count>int(MaxVertices+1)||TagMetres(tags,"min_height")>0) {++result.skipped;continue;}
        std::vector<Point> p;bool valid=true;
        double firstLat=0,firstLon=0;
        for(int i=0;i<count;++i) {
            const auto* node=cJSON_GetArrayItem(geometry,i);const double lat=Number(node,"lat"),lon=Number(node,"lon");
            if(!std::isfinite(lat)||!std::isfinite(lon)||std::abs(lat-(cell.latitude+.5)*CellDegrees)>.04 ||
                std::abs(Earth::WrapLongitude(lon-(cell.longitude+.5)*CellDegrees))>.04){valid=false;break;}
            if(i==0){firstLat=lat;firstLon=lon;}
            if(i==count-1){if(std::abs(firstLat-lat)>1e-9||std::abs(Earth::WrapLongitude(firstLon-lon))>1e-9)valid=false;break;}
            const Point point{Earth::Anchor+Earth::WrapLongitude(lon-g.longitude)*Earth::Pi/180*Earth::Radius*std::cos(g.latitude*Earth::Pi/180),
                Earth::Anchor+(lat-g.latitude)*Earth::Pi/180*Earth::Radius};
            if(!p.empty() && std::hypot(point.x-p.back().x,point.z-p.back().z)<.01){valid=false;break;}
            p.push_back(point);
        }
        std::vector<uint32_t> roof;
        if(!valid || p.size()<3 || !Triangulate(p,roof)){++result.skipped;continue;}
        double h=TagMetres(tags,"height");bool estimated=false;
        if(!std::isfinite(h)){h=TagMetres(tags,"building:levels")*3;estimated=true;}
        if(!std::isfinite(h)){h=6;estimated=true;}
        if(h<2 || h>150){++result.skipped;continue;}
        float low=INFINITY,high=-INFINITY;
        try {
            for(size_t i=0;i<p.size();++i) {
                const auto a=p[i],b=p[(i+1)%p.size()];const double length=std::hypot(b.x-a.x,b.z-a.z);
                if(length>300)throw std::out_of_range("large footprint");
                const int steps=std::max(1,int(std::ceil(length/std::max(1.0f,g.grid*.5f))));
                for(int j=0;j<=steps;++j) {const double t=double(j)/steps;const float y=Height(g,{a.x+(b.x-a.x)*t,a.z+(b.z-a.z)*t});low=std::min(low,y);high=std::max(high,y);}
            }
        }catch(...){++result.skipped;continue;}
        if(high-low>30){++result.skipped;continue;}
        const size_t triangles=roof.size()/3+p.size()*2;
        if(result.indices.size()/3+triangles>MaxTriangles)break;
        const float bottom=low-1,top=high+float(h);
        const uint32_t roofBase=uint32_t(result.vertices.size());
        for(auto v:p)result.vertices.push_back({float(v.x),top,float(v.z),0,1,0});
        for(auto index:roof)result.indices.push_back(roofBase+index);
        for(size_t i=0;i<p.size();++i) {
            const auto a=p[i],b=p[(i+1)%p.size()];const float dx=float(b.x-a.x),dz=float(b.z-a.z),length=std::hypot(dx,dz);
            const uint32_t base=uint32_t(result.vertices.size());
            for(auto v:std::array<Vertex,4>{{{float(a.x),bottom,float(a.z),dz/length,0,-dx/length},
                {float(b.x),bottom,float(b.z),dz/length,0,-dx/length},{float(a.x),top,float(a.z),dz/length,0,-dx/length},
                {float(b.x),top,float(b.z),dz/length,0,-dx/length}}})result.vertices.push_back(v);
            result.indices.insert(result.indices.end(),{base,base+2,base+1,base+1,base+2,base+3});
        }
        ++result.buildings;if(estimated)++result.estimatedHeights;
    }
    return result;
}
EarthBuildingStream::EarthBuildingStream(std::filesystem::path cache):_cache(std::move(cache)){_worker=std::thread(&EarthBuildingStream::Run,this);}
EarthBuildingStream::~EarthBuildingStream(){ {std::lock_guard lock(_mutex);_stop=true;++_request;} _wake.notify_one();if(_worker.joinable())_worker.join(); }
void EarthBuildingStream::Relocate(){std::lock_guard lock(_mutex);++_request;_pending=false;_requested=false;_ready.reset();}
void EarthBuildingStream::Request(const EarthTerrainStream::Patch& p,float x,float z) {
    if(!std::isfinite(x)||!std::isfinite(z))return;
    const auto coordinate=Earth::FromLocal(p.originLatitude,p.originLongitude,x,z);
    if(!std::isfinite(coordinate.latitude)||std::abs(coordinate.latitude)>84)return;
    const auto cell=CellAt(coordinate.latitude,coordinate.longitude);
    std::lock_guard lock(_mutex);
    if(_requested && cell==_cell && p.generation==_ground.generation && p.originRevision==_ground.revision)return;
    _cell=cell;_ground={p.x,p.z,p.gridMetres,p.originLatitude,p.originLongitude,p.originRevision,p.generation,p.heights};
    ++_request;_requested=true;_pending=true;_ready.reset();_wake.notify_one();
}
std::unique_ptr<EarthBuildingStream::Mesh> EarthBuildingStream::TakeReady(){std::lock_guard lock(_mutex);return std::move(_ready);}
void EarthBuildingStream::Run(){
    for(;;){
        Ground ground;Cell cell;uint64_t request;
        {std::unique_lock lock(_mutex);_wake.wait(lock,[&]{return _stop||_pending;});if(_stop)return;ground=_ground;cell=_cell;request=_request;_pending=false;}
        auto mesh=std::make_unique<Mesh>();mesh->cell=cell;mesh->revision=ground.revision;mesh->generation=ground.generation;
        try{
            const auto path=_cache/"buildings"/FileName(cell);std::error_code error;const auto bytes=std::filesystem::file_size(path,error);
            if(error)mesh->error="cell not cached";
            else if(bytes>MaxBytes)mesh->error="cached cell exceeds 4MiB";
            else {std::string json(size_t(bytes),'\0');std::ifstream input(path,std::ios::binary);if(!input.read(json.data(),std::streamsize(bytes)))mesh->error="cell read failed";else *mesh=BuildCell(json,ground,cell);}
        }catch(const std::exception& e){mesh->error=e.what();}
        {std::lock_guard lock(_mutex);if(!_stop && request==_request)_ready=std::move(mesh);}
    }
}
}
