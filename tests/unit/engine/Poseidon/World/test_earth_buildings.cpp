#include "WgpuRenderer/EarthBuildingStream.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using Buildings=Poseidon::EarthBuildingStream;
using Point=std::pair<double,double>;
std::string Way(const std::vector<Point>& polygon,bool close=true,const char* tags="\"height\":\"9\"") {
    std::ostringstream out;out<<std::setprecision(9)<<"{\"type\":\"way\",\"tags\":{\"building\":\"yes\","<<tags<<"},\"geometry\":[";
    for(size_t i=0;i<polygon.size()+size_t(close);++i){auto p=polygon[i%polygon.size()];const auto c=Poseidon::Earth::FromLocal(0,0,p.first,p.second);if(i)out<<",";out<<"{\"lat\":"<<c.latitude<<",\"lon\":"<<c.longitude<<"}";}
    return out.str()+"]}";
}
std::string Json(const std::string& way){return "{\"elements\":["+way+"]}";}
int main(int argc,char** argv){
    Buildings::Ground g{6272,6272,1,0,0,5,7,{}};
    for(int z=0;z<257;++z)for(int x=0;x<257;++x)g.heights.push_back(10+.1f*(x-128)+.2f*(z-128));
    const auto cell=Buildings::CellAt(0,0);
    assert(Buildings::CellAt(-.001,-.001)==(Buildings::Cell{-1,-1}));
    assert(Buildings::CellAt(0,180)==Buildings::CellAt(0,-180));
    const std::vector<Point> square{{6400,6400},{6410,6400},{6410,6410},{6400,6410}};
    auto mesh=Buildings::BuildCell(Json(Way(square)),g,cell);
    assert(mesh.error.empty()&&mesh.buildings==1&&mesh.indices.size()/3==10);
    float low=INFINITY,high=-INFINITY;
    for(auto v:mesh.vertices){assert(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z));low=std::min(low,v.y);high=std::max(high,v.y);}
    assert(std::abs(low-9)<.001f&&std::abs(high-22)<.001f); // Foundation below lowest slope point, level roof above highest.
    for(size_t i=0;i<mesh.indices.size();i+=3){
        const auto a=mesh.vertices[mesh.indices[i]],b=mesh.vertices[mesh.indices[i+1]],c=mesh.vertices[mesh.indices[i+2]];
        const float ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z,vx=c.x-a.x,vy=c.y-a.y,vz=c.z-a.z;
        assert((uy*vz-uz*vy)*a.nx+(uz*vx-ux*vz)*a.ny+(ux*vy-uy*vx)*a.nz>0); // Winding agrees with roof and outward wall normals.
    }
    const std::vector<Point> concave{{6400,6400},{6410,6400},{6410,6404},{6404,6404},{6404,6410},{6400,6410}};
    mesh=Buildings::BuildCell(Json(Way(concave)),g,cell);assert(mesh.buildings==1&&mesh.indices.size()/3==16);
    double area=0;for(size_t i=0;i<(concave.size()-2)*3;i+=3){auto a=mesh.vertices[mesh.indices[i]],b=mesh.vertices[mesh.indices[i+1]],c=mesh.vertices[mesh.indices[i+2]];area+=std::abs((b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x))*.5;}
    assert(std::abs(area-64)<.001); // Ear clipping does not fill the missing concave corner.
    assert(Buildings::BuildCell(Json(Way(square,false)),g,cell).buildings==0);
    assert(Buildings::BuildCell(Json(Way({{6400,6400},{6410,6410},{6400,6410},{6410,6400}})),g,cell).buildings==0);
    assert(Buildings::BuildCell(Json(Way({{6400,6400},{6405,6400},{6410,6400}})),g,cell).buildings==0);
    assert(Buildings::BuildCell(Json(Way(square,true,"\"height\":\"900\"")),g,cell).buildings==0);
    assert(Buildings::BuildCell(Json(Way(square,true,"\"min_height\":\"2\"")),g,cell).buildings==0);
    assert(Buildings::BuildCell(Json(Way(square,true,"\"building:levels\":\"3\"")),g,cell).estimatedHeights==1);
    assert(!Buildings::BuildCell(std::string(Buildings::MaxBytes+1,' '),g,cell).error.empty());
    std::vector<Point> circle;for(int i=0;i<129;++i){double a=i*Poseidon::Earth::Pi*2/129;circle.emplace_back(6415+10*std::cos(a),6415+10*std::sin(a));}
    assert(Buildings::BuildCell(Json(Way(circle)),g,cell).buildings==0);
    circle.clear();for(int i=0;i<40;++i){double a=i*Poseidon::Earth::Pi*2/40;circle.emplace_back(6415+10*std::cos(a),6415+10*std::sin(a));}
    std::string many="{\"elements\":[";for(int i=0;i<2001;++i){if(i)many+=",";many+=Way(circle);}many+="]}";
    // Short JSON enough to exercise mesh budget rather than the file-size gate.
    assert(many.size()<=Buildings::MaxBytes);mesh=Buildings::BuildCell(many,g,cell);assert(mesh.indices.size()/3<=Buildings::MaxTriangles&&mesh.buildings<2000);
    const auto cache=std::filesystem::temp_directory_path()/("op-earth-buildings-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(cache/"buildings");const auto file=cache/"buildings"/Buildings::FileName(cell);{std::ofstream out(file);out<<Json(Way(square));}
    {
        Buildings stream(cache);Poseidon::EarthTerrainStream::Patch p{};p.x=g.x;p.z=g.z;p.gridMetres=g.grid;p.originLatitude=0;p.originLongitude=0;p.originRevision=5;p.generation=7;p.heights=g.heights;
        stream.Request(p,6400,6400);stream.Relocate();p.originRevision=6;p.generation=8;stream.Request(p,6400,6400);
        std::unique_ptr<Buildings::Mesh> ready;for(int i=0;i<200&&!ready;++i){std::this_thread::sleep_for(std::chrono::milliseconds(10));ready=stream.TakeReady();}
        assert(ready&&ready->revision==6&&ready->generation==8&&ready->buildings==1);
    }
    std::filesystem::remove(file);std::filesystem::remove(cache/"buildings");std::filesystem::remove(cache);
    if(argc>1){
        std::ifstream input(argv[1],std::ios::binary);std::string json((std::istreambuf_iterator<char>(input)),{});
        g={3200,3200,25,49.61,6.13,1,1,std::vector<float>(257*257,300)};
        mesh=Buildings::BuildCell(json,g,Buildings::CellAt(49.61,6.13));assert(mesh.error.empty()&&mesh.buildings>0);
        std::cout<<"Cached Luxembourg parser proof (synthetic 300m grounding, not runtime): "<<mesh.buildings<<" buildings, "<<mesh.skipped<<" skipped, "<<mesh.indices.size()/3<<" triangles\n";
    }
    std::cout<<"PASS Earth buildings: concave roofs, slope foundations, invalid polygons, limits, revision-safe worker\n";
}
