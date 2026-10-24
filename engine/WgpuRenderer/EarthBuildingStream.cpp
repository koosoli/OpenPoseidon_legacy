#include "EarthBuildingStream.hpp"
#include "EarthContentSource.hpp"
#include "EarthOverture.hpp"
#include <cjson/cJSON.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace Poseidon
{
namespace
{
struct Point { double x,z; };

// Pitched roofs. OSM footprints are extruded to their real outline; a flat lid on every one read as a
// field of blocks. A rectangle gets the roof OSM tags it with (roof:shape), and an untagged small
// house-like rectangle the gabled roof most real houses have, ridge along its long side.
enum class RoofShape { Flat, Gabled, Hipped };
RoofShape ChooseRoof(const char* roofTag, const std::string& building, const std::vector<Point>& p, bool heightTagged,
                     double& halfWidth)
{
    if (p.size() != 4) return RoofShape::Flat;
    double longest = 0, shortest = 1e9;
    for (size_t i = 0; i < 4; ++i)
    {
        const auto a = p[i], b = p[(i + 1) % 4], c = p[(i + 2) % 4];
        const double ex = b.x - a.x, ez = b.z - a.z, fx = c.x - b.x, fz = c.z - b.z;
        const double le = std::hypot(ex, ez), lf = std::hypot(fx, fz);
        if (le < .5 || lf < .5 || std::abs((ex * fx + ez * fz) / (le * lf)) > .26) return RoofShape::Flat; // not ~90 deg
        longest = std::max(longest, le);
        shortest = std::min(shortest, le);
    }
    halfWidth = shortest * .5;
    const std::string tag = roofTag ? roofTag : "";
    if (tag == "gabled" || tag == "saltbox" || tag == "gambrel" || tag == "mansard") return RoofShape::Gabled;
    if (tag == "hipped" || tag == "pyramidal" || tag == "half-hipped") return RoofShape::Hipped;
    if (!tag.empty()) return RoofShape::Flat; // flat, skillion, dome, ...: keep the lid
    static const char* houses[] = {"house", "detached", "semidetached_house", "farm", "barn", "cabin", "bungalow",
                                   "hut", "shed", "farm_auxiliary", "chapel", "terrace", "stable", "cowshed"};
    const bool house = std::find_if(std::begin(houses), std::end(houses), [&](const char* h) { return building == h; }) !=
                       std::end(houses);
    // Untagged "yes" is most often a house too when it is house-sized and has no measured height.
    if ((house || building == "residential" || (building == "yes" && !heightTagged && longest * shortest < 220)) &&
        shortest <= 16)
        return RoofShape::Gabled;
    return RoofShape::Flat;
}

// OSM rectangles often carry extra nodes along a wall (a shared corner with the neighbour, a gate):
// drop points where the outline barely turns, so the rectangle is recognised as one.
std::vector<Point> DropStraightPoints(std::vector<Point> p)
{
    for (bool removed = true; removed && p.size() > 3;)
    {
        removed = false;
        for (size_t i = 0; i < p.size() && p.size() > 3; ++i)
        {
            const auto a = p[(i + p.size() - 1) % p.size()], b = p[i], c = p[(i + 1) % p.size()];
            const double ux = b.x - a.x, uz = b.z - a.z, vx = c.x - b.x, vz = c.z - b.z;
            const double lu = std::hypot(ux, uz), lv = std::hypot(vx, vz);
            if (lu < .3 || lv < .3 || std::abs(ux * vz - uz * vx) / (lu * lv) < .2) // < ~12 deg turn
            {
                p.erase(p.begin() + i);
                removed = true;
                break;
            }
        }
    }
    return p;
}

struct Corner { double x,y,z; };
// One planar face with its own normal; turned so its normal points along `outward` (the stream's winding:
// cross(p1-p0, p2-p0) is the outside).
template<class Vertices, class Indices>
void AddFace(Vertices& vertices, Indices& indices, std::initializer_list<Corner> corners, Corner outward, float u)
{
    std::vector<Corner> c(corners);
    const auto cross = [&](const Corner& a, const Corner& b, const Corner& d) {
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = d.x - a.x, vy = d.y - a.y, vz = d.z - a.z;
        return Corner{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
    };
    Corner n = cross(c[0], c[1], c[2]);
    if (n.x * outward.x + n.y * outward.y + n.z * outward.z < 0)
    {
        std::reverse(c.begin(), c.end());
        n = {-n.x, -n.y, -n.z};
    }
    const double length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length < 1e-9) return;
    const uint32_t base = uint32_t(vertices.size());
    for (const auto& v : c)
        vertices.push_back({float(v.x), float(v.y), float(v.z), float(n.x / length), float(n.y / length), float(n.z / length), u});
    for (uint32_t i = 1; i + 1 < c.size(); ++i)
        indices.insert(indices.end(), {base, base + i, base + i + 1});
}
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
// Index i with HorizonOffset(i) <= offset <= HorizonOffset(i+1) and the fraction between them.
bool HorizonCell(float offset,float radius,float halfWindow,int& i,double& f) {
    if(!(radius>halfWindow) || offset<Earth::HorizonOffset(0,radius,halfWindow) ||
       offset>Earth::HorizonOffset(Earth::HorizonSamples-1,radius,halfWindow))return false;
    int lo=0,hi=Earth::HorizonSamples-1;
    while(hi-lo>1){const int mid=(lo+hi)/2;if(Earth::HorizonOffset(mid,radius,halfWindow)<=offset)lo=mid;else hi=mid;}
    const float a=Earth::HorizonOffset(lo,radius,halfWindow),b=Earth::HorizonOffset(hi,radius,halfWindow);
    i=lo;f=b>a?(offset-a)/(b-a):0;return true;
}
float HorizonHeight(const EarthBuildingStream::Ground& g,Point p) {
    constexpr int n=Earth::HorizonSamples;
    const float half=Earth::NearHalfWindow(g.grid);
    int ix,iz;double fx,fz;
    if(g.horizonHeights.size()!=size_t(n)*n || g.horizonValid.size()!=g.horizonHeights.size() ||
       !HorizonCell(float(p.x-g.centreX),g.horizonRadius,half,ix,fx) ||
       !HorizonCell(float(p.z-g.centreZ),g.horizonRadius,half,iz,fz))
        throw std::out_of_range("outside resident terrain");
    const size_t i00=size_t(iz)*n+ix,i10=i00+1,i01=i00+n,i11=i01+1;
    if(!g.horizonValid[i00]||!g.horizonValid[i10]||!g.horizonValid[i01]||!g.horizonValid[i11])
        throw std::out_of_range("no horizon terrain");
    const auto& h=g.horizonHeights;
    const float y=float((h[i00]*(1-fx)+h[i10]*fx)*(1-fz)+(h[i01]*(1-fx)+h[i11]*fx)*fz);
    if(!std::isfinite(y))throw std::out_of_range("invalid terrain");return y;
}
float Height(const EarthBuildingStream::Ground& g,Point p) {
    const double x=(p.x-g.x)/g.grid,z=(p.z-g.z)/g.grid;
    if(x<0 || z<0 || x>256 || z>256) return HorizonHeight(g,p);
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
namespace {
using Buildings=EarthBuildingStream;
// The texture coordinates of one building's faces (vertices [first, end)), from each face's normal: walls
// run along the wall horizontally and stand on the ground (`ground`, the footprint's lowest terrain) so
// the facade's ground floor sits on it; roofs are tiled in their own plane, rows across the slope.
// Relative to the building's first corner, so the coordinates stay small in a frame tens of km wide.
void TextureFaces(Buildings::Mesh& result,size_t first,Point origin,float ground,uint32_t wall,uint32_t roof) {
    for(size_t i=first;i<result.vertices.size();++i) {
        auto& v=result.vertices[i];
        const double x=v.x-origin.x,z=v.z-origin.z;
        const bool isWall=std::abs(v.ny)<.3f;
        v.material=isWall?wall:roof;
        const float* tile=Buildings::MaterialTile[v.material];
        if(isWall) {
            const double h=std::hypot(double(v.nx),double(v.nz));
            const double ax=h>1e-6?v.nz/h:1,az=h>1e-6?-v.nx/h:0; // the viewer's right, facing the wall
            v.tu=float((x*ax+z*az)/tile[0]);
            v.tv=float(1-(v.y-ground)/tile[1]);
        } else if(v.ny>.99f) {
            v.tu=float(x/tile[0]);v.tv=float(z/tile[1]);
        } else {
            // downslope horizontal direction; the slope length is the horizontal one over the pitch's cosine
            const double h=std::hypot(double(v.nx),double(v.nz));
            const double dx=v.nx/h,dz=v.nz/h;
            v.tu=float((x*-dz+z*dx)/tile[0]);
            v.tv=float((x*dx+z*dz)/(tile[1]*v.ny));
        }
    }
}
void AppendCell(Buildings::Mesh& result,const std::string& json,const Buildings::Ground& g,Buildings::Cell cell,
                std::unordered_set<uint64_t>& ids) {
    constexpr auto MaxBytes=Buildings::MaxBytes,MaxBuildings=Buildings::MaxBuildings,
        MaxVertices=Buildings::MaxVertices,MaxTriangles=Buildings::MaxTriangles;
    constexpr auto CellDegrees=Buildings::CellDegrees;
    if(json.size()>MaxBytes || g.heights.size()!=257*257 || !std::isfinite(g.grid)||g.grid<=0 ||
        !std::isfinite(g.latitude)||!std::isfinite(g.longitude)||std::abs(g.latitude)>84) {result.error="invalid bounded cell/terrain";return;}
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(json.data(),json.size()),cJSON_Delete);
    if(!root){result.error="invalid cached JSON";return;}
    const auto* elements=cJSON_GetObjectItemCaseSensitive(root.get(),"elements");
    if(!cJSON_IsArray(elements)){result.error="missing elements";return;}
    size_t visited=0;
    const cJSON* element=nullptr;
    cJSON_ArrayForEach(element,elements) {
        if(++visited>MaxBuildings)break;
        const double sourceId=Number(element,"id");
        const uint64_t id=std::isfinite(sourceId)&&sourceId>0&&sourceId<=9007199254740991.0&&std::floor(sourceId)==sourceId?
            uint64_t(sourceId):0;
        if(id && ids.contains(id)){++result.skipped;continue;}
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
            const auto local=Earth::ToLocal(g.latitude,g.longitude,lat,lon);
            if(!local.visible){valid=false;break;}
            const Point point{local.x,local.z};
            if(!p.empty() && std::hypot(point.x-p.back().x,point.z-p.back().z)<.01){valid=false;break;}
            p.push_back(point);
        }
        std::vector<uint32_t> roof;
        if(!valid || p.size()<3 || !Triangulate(p,roof)){++result.skipped;continue;}
        double h=TagMetres(tags,"height");bool estimated=false;
        if(!std::isfinite(h)){h=TagMetres(tags,"building:levels")*3;estimated=true;}
        if(!std::isfinite(h)){h=6;estimated=true;}
        if(h<2 || h>900){++result.skipped;continue;}
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
        const float bottom=low-1;
        float top=high+float(h);
        // A stable per-building variation, so a street is not one colour.
        const uint64_t variant=id?id*2654435761ull:uint64_t(result.buildings)*40503ull;
        const float wallU=EarthBuildingStream::PaletteU(int((variant>>8)%4)),pitchedU=EarthBuildingStream::PaletteU(4+int((variant>>12)%3)),
            flatU=EarthBuildingStream::PaletteU(7);
        // House facades for a building up to 8 m tall, the doorless ones above. By the building's height,
        // not its wall: on a slope the downhill wall is metres longer and every house became a block.
        const size_t firstVertex=result.vertices.size();
        const uint32_t houseWall=uint32_t((variant>>8)%4),blockWall=Buildings::BlockA+uint32_t((variant>>20)%2),
            pitchedRoof=Buildings::TilesRed+uint32_t((variant>>12)%3);
        double halfWidth=0;
        const std::vector<Point> outline=DropStraightPoints(p);
        const RoofShape roofShape=h<=15?ChooseRoof(Text(tags,"roof:shape"),building,outline,!estimated,halfWidth):RoofShape::Flat;
        if(roofShape!=RoofShape::Flat) {
            // 35 degree pitch. A tagged height includes the roof; an estimated one is the walls.
            const double rise=std::clamp(halfWidth*.7,1.0,6.0);
            const double eaves=estimated?top:std::max(double(top)-rise,double(high)+2.5);
            const double ridgeY=eaves+rise;
            const uint32_t wallMaterial=h>8?blockWall:houseWall;
            // Long side first: q0->q1 is a long edge, q1->q2 a short (gable) edge.
            std::vector<Point> q=outline;
            if(std::hypot(q[1].x-q[0].x,q[1].z-q[0].z)<std::hypot(q[2].x-q[1].x,q[2].z-q[1].z))std::rotate(q.begin(),q.begin()+1,q.end());
            const Point m0{(q[3].x+q[0].x)*.5,(q[3].z+q[0].z)*.5},m1{(q[1].x+q[2].x)*.5,(q[1].z+q[2].z)*.5};
            Point r0=m0,r1=m1;
            if(roofShape==RoofShape::Hipped) {
                const double ax=m1.x-m0.x,az=m1.z-m0.z,len=std::hypot(ax,az);
                const double inset=std::min(halfWidth,len*.5);
                r0={m0.x+ax/len*inset,m0.z+az/len*inset};r1={m1.x-ax/len*inset,m1.z-az/len*inset};
            }
            const double cx=(q[0].x+q[1].x+q[2].x+q[3].x)*.25,cz=(q[0].z+q[1].z+q[2].z+q[3].z)*.25;
            const auto out=[&](double x,double z){return Corner{x-cx,0,z-cz};};
            const auto C=[&](Point v,double y){return Corner{v.x,y,v.z};};
            for(size_t i=0;i<4;++i) {
                const auto a=q[i],b=q[(i+1)%4];
                AddFace(result.vertices,result.indices,{C(a,bottom),C(b,bottom),C(b,eaves),C(a,eaves)},out((a.x+b.x)*.5,(a.z+b.z)*.5),wallU);
            }
            const Corner up{0,1,0};
            AddFace(result.vertices,result.indices,{C(q[0],eaves),C(q[1],eaves),C(r1,ridgeY),C(r0,ridgeY)},up,pitchedU);
            AddFace(result.vertices,result.indices,{C(q[2],eaves),C(q[3],eaves),C(r0,ridgeY),C(r1,ridgeY)},up,pitchedU);
            if(roofShape==RoofShape::Hipped) {
                AddFace(result.vertices,result.indices,{C(q[1],eaves),C(q[2],eaves),C(r1,ridgeY)},up,pitchedU);
                AddFace(result.vertices,result.indices,{C(q[3],eaves),C(q[0],eaves),C(r0,ridgeY)},up,pitchedU);
            } else {
                // Gable ends: upright triangles over the short walls.
                AddFace(result.vertices,result.indices,{C(q[1],eaves),C(q[2],eaves),C(r1,ridgeY)},out(m1.x,m1.z),wallU);
                AddFace(result.vertices,result.indices,{C(q[3],eaves),C(q[0],eaves),C(r0,ridgeY)},out(m0.x,m0.z),wallU);
            }
            TextureFaces(result,firstVertex,p[0],low,wallMaterial,pitchedRoof);
            ++result.buildings;if(estimated)++result.estimatedHeights;
            if(id)ids.insert(id);
            continue;
        }
        const uint32_t roofBase=uint32_t(result.vertices.size());
        for(auto v:p)result.vertices.push_back({float(v.x),top,float(v.z),0,1,0,flatU});
        for(auto index:roof)result.indices.push_back(roofBase+index);
        for(size_t i=0;i<p.size();++i) {
            const auto a=p[i],b=p[(i+1)%p.size()];const float dx=float(b.x-a.x),dz=float(b.z-a.z),length=std::hypot(dx,dz);
            const uint32_t base=uint32_t(result.vertices.size());
            for(auto v:std::array<Buildings::Vertex,4>{{{float(a.x),bottom,float(a.z),dz/length,0,-dx/length,wallU},
                {float(b.x),bottom,float(b.z),dz/length,0,-dx/length,wallU},{float(a.x),top,float(a.z),dz/length,0,-dx/length,wallU},
                {float(b.x),top,float(b.z),dz/length,0,-dx/length,wallU}}})result.vertices.push_back(v);
            result.indices.insert(result.indices.end(),{base,base+2,base+1,base+1,base+2,base+3});
        }
        TextureFaces(result,firstVertex,p[0],low,h>8?blockWall:houseWall,Buildings::FlatRoof);
        ++result.buildings;if(estimated)++result.estimatedHeights;
        if(id)ids.insert(id);
    }
}
}
void EarthBuildingStream::Split(Mesh& mesh,uint32_t textured) {
    // Faces never share vertices, so a triangle's material is its first vertex's.
    for(auto& part:mesh.parts)part.clear();
    for(auto& part:mesh.partIndices)part.clear();
    std::vector<uint32_t> remap(mesh.vertices.size(),UINT32_MAX);
    for(size_t t=0;t+2<mesh.indices.size();t+=3) {
        const uint32_t m=std::min<uint32_t>(mesh.vertices[mesh.indices[t]].material,MaterialCount-1);
        const bool isTextured=(textured>>m)&1u;
        for(int k=0;k<3;++k) {
            const uint32_t i=mesh.indices[t+k];
            if(remap[i]==UINT32_MAX) {
                const auto& v=mesh.vertices[i];
                WgrMeshVertex w{};
                w.pos={v.x,v.y,v.z};w.normal={v.nx,v.ny,v.nz};
                w.uv=isTextured?WgrVec2{v.tu,v.tv}:WgrVec2{PaletteU(MaterialPalette[m]),.5f};
                remap[i]=uint32_t(mesh.parts[m].size());mesh.parts[m].push_back(w);
            }
            mesh.partIndices[m].push_back(remap[i]);
        }
    }
    mesh.partsMask=textured;mesh.partsReady=true;
}
EarthBuildingStream::Mesh EarthBuildingStream::BuildCell(const std::string& json,const Ground& g,Cell cell) {
    Mesh result;result.cell=cell;result.revision=g.revision;result.generation=g.generation;
    std::unordered_set<uint64_t> ids;AppendCell(result,json,g,cell,ids);return result;
}
EarthBuildingStream::Mesh EarthBuildingStream::BuildNeighborhood(const std::filesystem::path& cache,const Ground& g,Cell centre,
                                                               const std::function<bool()>& cancelled) {
    Mesh result;result.cell=centre;result.revision=g.revision;result.generation=g.generation;
    std::unordered_set<uint64_t> ids;
    // Centre first, then edges and corners: nearby cells get the shared budget first.
    constexpr int offsets[9][2]={{0,0},{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
    bool cached=false;
    for(const auto& offset:offsets) {
        if(cancelled && cancelled()){result.error="request cancelled";return result;}
        Cell cell{centre.latitude+offset[0],((centre.longitude+offset[1]+27000)%18000)-9000};
        if(std::abs((cell.latitude+.5)*CellDegrees)>84)continue;
        try {
            auto path=cache/"buildings"/FileName(cell);std::error_code error;
            if(!std::filesystem::is_regular_file(path,error))path=EarthContentSource::StreamPath(cache,"buildings",cell.latitude,cell.longitude);
            const auto bytes=std::filesystem::file_size(path,error);
            if(error)continue; // An empty neighbouring cell cannot evict a cached one.
            cached=true;
            if(bytes>MaxBytes){result.error="cached cell exceeds 4MiB";continue;}
            std::string json(size_t(bytes),'\0');std::ifstream input(path,std::ios::binary);
            if(!input.read(json.data(),std::streamsize(bytes))){result.error="cell read failed";continue;}
            if(cancelled && cancelled()){result.error="request cancelled";return result;}
            AppendCell(result,json,g,cell,ids);
        }catch(const std::exception& e){result.error=e.what();}
        if(result.indices.size()/3>=MaxTriangles)break;
    }
    if(!cached)result.error="no cached cells in 3x3 window";
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
    const float half=Earth::NearHalfWindow(p.gridMetres);
    _ground.centreX=p.x+half;_ground.centreZ=p.z+half;_ground.horizonRadius=p.horizonRadius;
    _ground.horizonHeights=p.horizonHeights;_ground.horizonValid=p.horizonValid;
    ++_request;_requested=true;_pending=true;_ready.reset();_wake.notify_one();
}
std::unique_ptr<EarthBuildingStream::Mesh> EarthBuildingStream::TakeReady(){std::lock_guard lock(_mutex);return std::move(_ready);}
void EarthBuildingStream::Run(){
    EarthContentSource source;
    for(;;){
        Ground ground;Cell cell;uint64_t request;
        {std::unique_lock lock(_mutex);_wake.wait(lock,[&]{return _stop||_pending;});if(_stop)return;ground=_ground;cell=_cell;request=_request;_pending=false;}
        const auto cancelled=[&]{
            std::lock_guard lock(_mutex);return _stop || request!=_request;
        };
        // the draw split too (see Split), so the main thread only creates the meshes
        const auto split=[&](std::unique_ptr<Mesh>& m){if(const uint32_t mask=_textured.load();mask!=UINT32_MAX)Split(*m,mask);};
        auto mesh=std::make_unique<Mesh>(BuildNeighborhood(_cache,ground,cell,cancelled));
        split(mesh);
        // Overture first (EarthOverture: static tiles on a CDN, OSM and machine-traced footprints with heights);
        // Overpass only when Overture cannot be had.
        const bool overture=EarthOverture::Enabled();
        if(overture || source.Enabled()){
            {std::lock_guard lock(_mutex);if(!_stop && request==_request)_ready=std::move(mesh);}
            bool changed=false;
            const bool had=overture && EarthOverture::Instance().EnsureNeighborhood(_cache,cell.latitude,cell.longitude,cancelled,changed);
            if(!had && source.Enabled())changed=source.EnsureNeighborhood(_cache,"buildings",cell.latitude,cell.longitude,cancelled);
            if(!changed)continue;
            mesh=std::make_unique<Mesh>(BuildNeighborhood(_cache,ground,cell,cancelled));
            split(mesh);
        }
        {std::lock_guard lock(_mutex);if(!_stop && request==_request)_ready=std::move(mesh);}
    }
}
}
