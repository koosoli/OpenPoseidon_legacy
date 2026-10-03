#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>
#include "RainWaterGeometry.hpp"
#include "RainWaterForcing.hpp"

namespace Poseidon {
// Conservative CPU core, used by World's default-off fine support gate; the
// matched fine renderer publication is a separate integration. Corner geometry
// integrates actual terrain triangles; the old centre overload is flat-cell
// compatibility only. Disjoint rectangular finite-volume owners are deliberately
// distinct from RainWaterField's interpolated vertex queries.
class RainWaterFine {
public:
    static constexpr int Ratio=4, TileParents=8, MaxTiles=64;
    static constexpr size_t MaxCoarseCells=513*513;
    static constexpr double StepSeconds=.25;
    struct Budget { double rain=0,infiltration=0,evaporation=0,outlet=0; };
    struct Owner {
        double x=0,z=0,size=0,volume=0;
        float bed=0,flowX=0,flowZ=0;
        double head=0;
        double Depth() const { return size>0?volume/(size*size):0; }
        double Head() const { return head; }
    };
    struct Surface { Owner owner;RainWaterCellGeometry geometry;uint32_t parent=0; };
    bool HasTile(int x,int z) const {return _tiles.contains({x,z});}
    std::vector<std::pair<int,int>> Tiles() const {
        std::vector<std::pair<int,int>> result;for(const auto& entry:_tiles)result.push_back(entry.first);return result;
    }
    bool SetTileLimit(size_t limit) {
        if(limit==0||limit>MaxTiles||limit<_tiles.size())return false;
        _tileLimit=limit;return true;
    }
    void SetSeaLevel(double level) {if(std::isfinite(level))_sea=level;}
    bool IsFineAt(double x,double z) const {const auto i=Find(x,z);return i!=Absent&&_cells[i].child!=Coarse;}
    bool ImportLegacy(std::span<const float> depths,const Budget& budget,double pending) {
        if(_tiles.size()!=0||depths.size()!=_children.size()||!std::isfinite(pending)||pending<0||
           !std::isfinite(budget.rain)||!std::isfinite(budget.infiltration)||!std::isfinite(budget.evaporation)||!std::isfinite(budget.outlet)||
           budget.rain<0||budget.infiltration<0||budget.evaporation<0||budget.outlet<0||
           !std::all_of(depths.begin(),depths.end(),[](float d){return std::isfinite(d)&&d>=0;}))return false;
        const double area=_spacing*_spacing;
        for(size_t i=0;i<depths.size();++i)_cells[i].volume=double(depths[i])*area;
        _budget=budget;_pending=pending;RefreshHeads();return true;
    }
    void Defer(double seconds) {
        if(std::isfinite(seconds)&&seconds>0&&std::isfinite(_pending+seconds))_pending+=seconds;
    }
    // Existing vertex publication/query proxies remain available outside fine
    // ownership. Refined parents aggregate child volume, not a second owner.
    void CoarseState(std::span<float> depth,std::span<float> flowX,std::span<float> flowZ) const {
        if(depth.size()!=_children.size()||flowX.size()!=depth.size()||flowZ.size()!=depth.size())return;
        for(size_t p=0;p<_children.size();++p) {
            double volume=0,x=0,z=0;
            if(_children[p]==Absent){volume=_cells[p].volume;x=_cells[p].flowX;z=_cells[p].flowZ;}
            else for(int c=0;c<Ratio*Ratio;++c){const auto& cell=_cells[_children[p]+c];volume+=cell.volume;x+=cell.flowX/(Ratio*Ratio);z+=cell.flowZ/(Ratio*Ratio);}
            depth[p]=float(volume/(_spacing*_spacing));flowX[p]=float(x);flowZ[p]=float(z);
        }
    }
    std::vector<Surface> FineSnapshot() const {
        std::vector<Surface> result;result.reserve(_geometry.size());
        std::vector<uint32_t> parents;parents.reserve(_tiles.size()*TileParents*TileParents);
        for(const auto& tile:_tiles) {
            const int x0=tile.first.first*TileParents,z0=tile.first.second*TileParents;
            for(int z=z0;z<std::min(z0+TileParents,_height);++z)
                for(int x=x0;x<std::min(x0+TileParents,_width);++x)parents.push_back(uint32_t(z*_width+x));
        }
        std::sort(parents.begin(),parents.end()); // stable spatial order, no world scan
        for(const auto p:parents)
            for(int c=0;c<Ratio*Ratio;++c){const auto i=_children[p]+c;result.push_back({Describe(i,false),_geometry[i-_children.size()],p});}
        return result;
    }
    bool Configure(int width,int height,double spacing,std::span<const float> bed,
                   double originX=0,double originZ=0,double sea=0) {
        if(!ValidDomain(width,height,spacing,bed,originX,originZ,sea)) return false;
        Reset(); _width=width;_height=height;_spacing=spacing;_x=originX;_z=originZ;_sea=sea;
        _cells.reserve(bed.size()+MaxTiles*TileParents*TileParents*Ratio*Ratio);
        for(size_t i=0;i<bed.size();++i) _cells.push_back({0,bed[i],0,0,uint32_t(i),Coarse,true});
        _children.assign(bed.size(),Absent); BuildEdges(); return true;
    }
    void Reset() { const auto next=_generation+1; *this=RainWaterFine{};_generation=next; }
    // Tile bed is row-major fine cell-centre data, including clipped edge tiles.
    // Repeated edits update the same persistent owners and preserve their volume.
    bool RefineTile(int tx,int tz,std::span<const float> bed) {
        if(tx<0||tz<0||_width==0||tx>(_width-1)/TileParents||tz>(_height-1)/TileParents) return false;
        const int x0=tx*TileParents,z0=tz*TileParents;
        if(x0>=_width||z0>=_height) return false;
        const int w=std::min(TileParents,_width-x0),h=std::min(TileParents,_height-z0);
        if(bed.size()!=size_t(w*h*Ratio*Ratio)||!FiniteBed(bed)) return false;
        const auto key=std::pair{tx,tz}; const bool existing=_tiles.contains(key);
        if(!existing&&_tiles.size()>=_tileLimit) return false;
        uint32_t block=0;
        if(!existing) {
            const uint32_t count=uint32_t(w*h*Ratio*Ratio);
            const auto free=std::find_if(_freeBlocks.begin(),_freeBlocks.end(),[count](const auto& b){return b.second>=count;});
            if(free!=_freeBlocks.end()) {
                block=free->first;free->first+=count;free->second-=count;
                if(!free->second)_freeBlocks.erase(free);
            } else {
                block=uint32_t(_cells.size());_cells.resize(_cells.size()+count);
                _geometry.resize(_cells.size()-_children.size());
            }
        }
        for(int z=0;z<h;++z) for(int x=0;x<w;++x) {
            const auto p=uint32_t((z0+z)*_width+x0+x);
            if(!existing) {
                _children[p]=block+uint32_t((z*w+x)*Ratio*Ratio);
                const double share=_cells[p].volume/(Ratio*Ratio);
                const float fx=_cells[p].flowX,fz=_cells[p].flowZ;
                _cells[p].volume=0;_cells[p].active=false;
                for(int cz=0;cz<Ratio;++cz) for(int cx=0;cx<Ratio;++cx) {
                    const auto i=_children[p]+cz*Ratio+cx;
                    _cells[i]={share,0,fx,fz,p,uint16_t(cz*Ratio+cx),true};
                    _geometry[i-_children.size()]=RainWaterCellGeometry::Flat(0);
                }
            }
            for(int cz=0;cz<Ratio;++cz) for(int cx=0;cx<Ratio;++cx) {
                const auto i=_children[p]+cz*Ratio+cx;
                const float value=bed[(z*Ratio+cz)*w*Ratio+x*Ratio+cx];
                _cells[i].bed=value;_geometry[i-_children.size()]=RainWaterCellGeometry::Flat(value);
            }
        }
        _tiles.emplace(key,true); if(!existing) BuildEdges(); RefreshHeads();++_revision;return true;
    }
    // Runtime producers must use actual corner geometry, not the compatibility
    // centre-height overload above. Geometry updates preserve retained volumes.
    bool RefineTileGeometry(int tx,int tz,std::span<const RainWaterCellGeometry> geometry) {
        if(tx<0||tz<0||_width==0||tx>(_width-1)/TileParents||tz>(_height-1)/TileParents) return false;
        const int x0=tx*TileParents,z0=tz*TileParents;
        const int w=std::min(TileParents,_width-x0),h=std::min(TileParents,_height-z0);
        if(geometry.size()!=size_t(w*h*Ratio*Ratio)||
           !std::all_of(geometry.begin(),geometry.end(),[](const auto& g){return g.Finite();})) return false;
        const bool existing=HasTile(tx,tz);
        std::vector<double> reconstructed(geometry.size());
        if(!existing)for(int z=0;z<h;++z)for(int x=0;x<w;++x) {
            std::array<RainWaterCellGeometry,Ratio*Ratio> parent;
            std::array<double,Ratio*Ratio> volumes;
            for(int cz=0;cz<Ratio;++cz)for(int cx=0;cx<Ratio;++cx)
                parent[cz*Ratio+cx]=geometry[(z*Ratio+cz)*w*Ratio+x*Ratio+cx];
            const auto p=uint32_t((z0+z)*_width+x0+x);
            if(!RainWaterReconstruct(parent,_cells[p].volume,(_spacing/Ratio)*(_spacing/Ratio),volumes))return false;
            for(int cz=0;cz<Ratio;++cz)for(int cx=0;cx<Ratio;++cx)
                reconstructed[(z*Ratio+cz)*w*Ratio+x*Ratio+cx]=volumes[cz*Ratio+cx];
        }
        std::vector<float> minima;minima.reserve(geometry.size());
        for(const auto& g:geometry)minima.push_back(float(g.Minimum()));
        if(!RefineTile(tx,tz,minima))return false;
        for(int z=0;z<h;++z)for(int x=0;x<w;++x)for(int cz=0;cz<Ratio;++cz)for(int cx=0;cx<Ratio;++cx) {
            const auto i=Child(uint32_t((z0+z)*_width+x0+x),cx,cz);
            const auto source=(z*Ratio+cz)*w*Ratio+x*Ratio+cx;
            _geometry[i-_children.size()]=geometry[source];
            if(!existing)_cells[i].volume=reconstructed[source];
        }
        RefreshHeads();return true;
    }
    double TileVolume(int tx,int tz) const {
        if(!HasTile(tx,tz))return 0;
        const int x0=tx*TileParents,z0=tz*TileParents;
        double volume=0;
        for(int z=z0;z<std::min(z0+TileParents,_height);++z)
            for(int x=x0;x<std::min(x0+TileParents,_width);++x)
                for(int c=0;c<Ratio*Ratio;++c)volume+=_cells[_children[z*_width+x]+c].volume;
        return volume;
    }
    // Only genuinely empty owners can retire. Wet topology is never evicted by
    // interest/camera changes, and arbitrarily tiny nonzero volumes are retained.
    bool CoarsenDryTile(int tx,int tz,std::span<const float> bed) {
        const auto key=std::pair{tx,tz};
        if(!HasTile(tx,tz)||bed.size()!=_children.size()||TileVolume(tx,tz)!=0)return false;
        const int x0=tx*TileParents,z0=tz*TileParents;
        const int w=std::min(TileParents,_width-x0),h=std::min(TileParents,_height-z0);
        for(int z=0;z<h;++z)for(int x=0;x<w;++x)
            if(!std::isfinite(bed[(z0+z)*_width+x0+x]))return false;
        const uint32_t block=_children[z0*_width+x0];
        for(int z=0;z<h;++z)for(int x=0;x<w;++x) {
            const auto p=uint32_t((z0+z)*_width+x0+x);double volume=0;float fx=0,fz=0;
            for(int c=0;c<Ratio*Ratio;++c) {
                auto& child=_cells[_children[p]+c];volume+=child.volume;
                fx+=child.flowX/(Ratio*Ratio);fz+=child.flowZ/(Ratio*Ratio);child.active=false;
            }
            _cells[p]={volume,bed[p],fx,fz,p,Coarse,true};_children[p]=Absent;
        }
        _tiles.erase(key);_freeBlocks.push_back({block,uint32_t(w*h*Ratio*Ratio)});
        std::sort(_freeBlocks.begin(),_freeBlocks.end());
        // Joining adjacent empty blocks also permits a complete tile to reuse
        // storage formerly occupied by several partial compatibility tiles.
        for(size_t i=1;i<_freeBlocks.size();) {
            auto& previous=_freeBlocks[i-1];const auto next=_freeBlocks[i];
            if(previous.first+previous.second==next.first) {
                previous.second+=next.second;_freeBlocks.erase(_freeBlocks.begin()+i);
            }else ++i;
        }
        BuildEdges();++_revision;return true;
    }
    size_t StorageOwnerSlots() const {return _cells.size();}
    bool SetCoarseBed(int x,int z,float bed) {
        if(x<0||z<0||x>=_width||z>=_height||!std::isfinite(bed)) return false;
        const auto p=uint32_t(z*_width+x);
        if(_children[p]!=Absent) return false; // refined owners need actual fine beds
        _cells[p].bed=bed;++_revision;return true;
    }
    bool AddWater(double x,double z,double volume) {
        const auto i=Find(x,z);
        if(i==Absent||!std::isfinite(volume)||volume<0) return false;
        const auto owner=Describe(i);const double next=_cells[i].volume+volume;
        if(!std::isfinite(next)||!std::isfinite(Volume()+volume)||!std::isfinite(next/(owner.size*owner.size)+owner.bed)) return false;
        _cells[i].volume+=volume;_head[i]=CellHead(i);++_revision;return true;
    }
    Owner At(double x,double z) const {
        const auto i=Find(x,z);return i==Absent?Owner{}:Describe(i);
    }
    double LocalDepth(double x,double z) const {
        const auto i=Find(x,z);if(i==Absent)return 0;
        const auto owner=Describe(i);
        const double bed=_cells[i].child==Coarse?owner.bed:
            _geometry[i-_children.size()].Height((x-owner.x)/owner.size,(z-owner.z)/owner.size);
        return std::max(0.0,owner.Head()-bed);
    }
    std::vector<Owner> Snapshot() const {
        std::vector<Owner> result;result.reserve(_cells.size());
        // Stable spatial order independent of tile activation order.
        for(size_t p=0;p<_children.size();++p) {
            if(_children[p]==Absent) result.push_back(Describe(uint32_t(p)));
            else for(int c=0;c<Ratio*Ratio;++c) result.push_back(Describe(_children[p]+c));
        }
        return result;
    }
    // Explicit same-footprint terrain regrid: overlap remap into coarse owners.
    // Camera motion cannot call this. Changed world footprints are refused.
    bool Regrid(int width,int height,double spacing,std::span<const float> bed) {
        if(!ValidDomain(width,height,spacing,bed,_x,_z,_sea)||_width==0||
           !SameExtent(width*spacing,_width*_spacing)||!SameExtent(height*spacing,_height*_spacing)) return false;
        const auto old=Snapshot();const auto budget=_budget;const double pending=_pending;
        const double x0=_x,z0=_z,sea=_sea;
        if(!Configure(width,height,spacing,bed,x0,z0,sea)) return false;
        _budget=budget;_pending=pending;
        for(const auto& source:old) {
            if(source.volume==0) continue;
            const int ax=std::clamp(int(std::floor((source.x-_x)/spacing)),0,width-1);
            const int az=std::clamp(int(std::floor((source.z-_z)/spacing)),0,height-1);
            const int bx=std::clamp(int(std::ceil((source.x+source.size-_x)/spacing))-1,0,width-1);
            const int bz=std::clamp(int(std::ceil((source.z+source.size-_z)/spacing))-1,0,height-1);
            for(int z=az;z<=bz;++z) for(int x=ax;x<=bx;++x) {
                const double overlapX=std::max(0.0,std::min(source.x+source.size,_x+(x+1)*spacing)-std::max(source.x,_x+x*spacing));
                const double overlapZ=std::max(0.0,std::min(source.z+source.size,_z+(z+1)*spacing)-std::max(source.z,_z+z*spacing));
                _cells[size_t(z)*width+x].volume+=source.Depth()*overlapX*overlapZ;
            }
        }
        return true;
    }
    // Developer regrid may alter the legacy equal-area boundary footprint.
    // Preserve total mass through overlap; volume outside the new footprint is
    // explicitly redistributed to its nearest boundary owner. Shorelines are
    // not preserved, and exact curved subcell wet placement is not claimed.
    bool RegridDomain(int width,int height,double spacing,std::span<const float> bed,double x0,double z0) {
        if(!ValidDomain(width,height,spacing,bed,x0,z0,_sea)||_width==0||!std::isfinite(_x-x0)||!std::isfinite(_z-z0))return false;
        const auto old=Snapshot();const auto budget=_budget;const double pending=_pending,sea=_sea;
        const size_t limit=_tileLimit;
        if(!Configure(width,height,spacing,bed,x0,z0,sea))return false;
        _tileLimit=limit;_budget=budget;_pending=pending;
        const auto bound=[](double value,int maximum){return int(std::clamp(value,0.0,double(maximum)));};
        for(const auto& source:old) {
            double transferred=0;
            const int ax=bound(std::floor((source.x-_x)/spacing),width-1);
            const int az=bound(std::floor((source.z-_z)/spacing),height-1);
            const int bx=bound(std::ceil((source.x+source.size-_x)/spacing)-1,width-1);
            const int bz=bound(std::ceil((source.z+source.size-_z)/spacing)-1,height-1);
            for(int z=az;z<=bz;++z)for(int x=ax;x<=bx;++x) {
                const double dx=std::max(0.0,std::min(source.x+source.size,_x+(x+1)*spacing)-std::max(source.x,_x+x*spacing));
                const double dz=std::max(0.0,std::min(source.z+source.size,_z+(z+1)*spacing)-std::max(source.z,_z+z*spacing));
                const double amount=source.Depth()*dx*dz;
                _cells[size_t(z)*width+x].volume+=amount;transferred+=amount;
            }
            const int x=bound(std::floor((source.x+source.size*.5-_x)/spacing),width-1);
            const int z=bound(std::floor((source.z+source.size*.5-_z)/spacing),height-1);
            _cells[size_t(z)*width+x].volume+=std::max(0.0,source.volume-transferred);
        }
        RefreshHeads();return true;
    }
    void Advance(double seconds,double rain,double sun=0,double wind=0) {
        if(_width==0||!std::isfinite(seconds)||seconds<=0||!std::isfinite(rain)) return;
        rain=std::clamp(rain,0.0,1.0);
        sun=std::isfinite(sun)?std::clamp(sun,0.0,1.0):0;
        wind=std::isfinite(wind)?std::clamp(wind,0.0,1.0):0;
        if(!std::isfinite(_pending+seconds)) return;
        _pending+=seconds;
        for(int count=0;count<16&&_pending+1e-12>=StepSeconds;++count) {
            Step(rain,sun,wind);_pending-=StepSeconds;
        }
    }
    double Volume() const { double v=0;for(const auto& c:_cells) if(c.active) v+=c.volume;return v; }
    size_t TileCount() const {return _tiles.size();}
    size_t OwnerCount() const {return _children.size()+_tilesOwnerExtra;}
    size_t StorageBytes() const {
        return _cells.capacity()*sizeof(Cell)+_children.capacity()*sizeof(uint32_t)+
               _edges.capacity()*sizeof(Edge)+(_out.capacity()+_delta.capacity()+_flux.capacity()+_head.capacity()+_incidentLength.capacity())*sizeof(double)+
               _geometry.capacity()*sizeof(RainWaterCellGeometry);
    }
    uint64_t Generation() const {return _generation;}
    uint64_t Revision() const {return _revision;}
    double PendingSeconds() const {return _pending;}
    const Budget& WaterBudget() const {return _budget;}
private:
    static constexpr uint16_t Coarse=std::numeric_limits<uint16_t>::max();
    static constexpr uint32_t Absent=std::numeric_limits<uint32_t>::max();
    struct Cell { double volume;float bed,flowX,flowZ;uint32_t parent;uint16_t child;bool active; };
    struct Edge {uint32_t a,b;float length;bool alongX;};
    static bool FiniteBed(std::span<const float> bed) {
        return std::all_of(bed.begin(),bed.end(),[](float y){return std::isfinite(y);});
    }
    static bool ValidDomain(int w,int h,double s,std::span<const float> bed,double x,double z,double sea) {
        return w>0&&h>0&&size_t(w)*h<=MaxCoarseCells&&bed.size()==size_t(w)*h&&
               std::isfinite(s)&&s>0&&std::isfinite(float(s))&&std::isfinite(s*s)&&std::isfinite(1/(s*s))&&
               float(s/Ratio)>0&&std::isfinite(1/((s/Ratio)*(s/Ratio)))&&
               std::isfinite(x)&&std::isfinite(z)&&std::isfinite(sea)&&
               std::isfinite(x+w*s)&&std::isfinite(z+h*s)&&x+s>x&&z+s>z&&FiniteBed(bed);
    }
    static bool SameExtent(double a,double b) {return std::abs(a-b)<=1e-12*std::max(a,b);}
    double CellHead(uint32_t i) const {
        const auto& c=_cells[i];const double s=c.child==Coarse?_spacing:_spacing/Ratio;
        if(c.child==Coarse)return c.bed+c.volume/(s*s);
        double head=0;const double hint=i<_head.size()?_head[i]:std::numeric_limits<double>::quiet_NaN();
        if(!_geometry[i-_children.size()].Head(c.volume,s*s,head,hint))return std::numeric_limits<double>::quiet_NaN();
        return head;
    }
    double CellVolumeAtHead(uint32_t i,double head) const {
        const auto& c=_cells[i];
        if(c.child==Coarse)return std::max(0.0,head-c.bed)*_spacing*_spacing;
        const double size=_spacing/Ratio;
        return _geometry[i-_children.size()].Volume(head,size*size);
    }
    void RefreshHeads() {for(uint32_t i=0;i<_cells.size();++i)if(_cells[i].active)_head[i]=CellHead(i);}
    Owner Describe(uint32_t i,bool fresh=true) const {
        const auto& c=_cells[i];double x=_x+(c.parent%_width)*_spacing,z=_z+(c.parent/_width)*_spacing,s=_spacing;
        if(c.child!=Coarse) {s/=Ratio;x+=(c.child%Ratio)*s;z+=(c.child/Ratio)*s;}
        return {x,z,s,c.volume,c.bed,c.flowX,c.flowZ,fresh?CellHead(i):_head[i]};
    }
    uint32_t Find(double x,double z) const {
        if(_width==0||!std::isfinite(x)||!std::isfinite(z)||x<_x||z<_z||x>=_x+_width*_spacing||z>=_z+_height*_spacing) return Absent;
        const double gx=(x-_x)/_spacing,gz=(z-_z)/_spacing;
        const int ix=int(gx),iz=int(gz);
        if(ix>=_width||iz>=_height) return Absent;
        const auto p=uint32_t(iz*_width+ix);
        if(_children[p]==Absent) return p;
        return _children[p]+std::min(int((gz-iz)*Ratio),Ratio-1)*Ratio+std::min(int((gx-ix)*Ratio),Ratio-1);
    }
    uint32_t Child(uint32_t p,int x,int z) const {return _children[p]+z*Ratio+x;}
    void Boundary(uint32_t p,uint32_t q,bool alongX) {
        const bool fineA=_children[p]!=Absent,fineB=_children[q]!=Absent;
        if(!fineA&&!fineB) {_edges.push_back({p,q,float(_spacing),alongX});return;}
        for(int k=0;k<Ratio;++k) {
            const auto a=fineA?Child(p,alongX?Ratio-1:k,alongX?k:Ratio-1):p;
            const auto b=fineB?Child(q,alongX?0:k,alongX?k:0):q;
            _edges.push_back({a,b,float(_spacing/Ratio),alongX});
        }
    }
    void BuildEdges() {
        _edges.clear();_tilesOwnerExtra=0;
        for(int z=0;z<_height;++z) for(int x=0;x<_width;++x) {
            const auto p=uint32_t(z*_width+x);
            if(_children[p]!=Absent) {
                _tilesOwnerExtra+=Ratio*Ratio-1;
                for(int cz=0;cz<Ratio;++cz) for(int cx=0;cx<Ratio;++cx) {
                    const auto a=Child(p,cx,cz);
                    if(cx+1<Ratio) _edges.push_back({a,Child(p,cx+1,cz),float(_spacing/Ratio),true});
                    if(cz+1<Ratio) _edges.push_back({a,Child(p,cx,cz+1),float(_spacing/Ratio),false});
                }
            }
            if(x+1<_width) Boundary(p,p+1,true);
            if(z+1<_height) Boundary(p,p+_width,false);
        }
        _out.resize(_cells.size());_delta.resize(_cells.size());_flux.resize(_edges.size());
        _incidentLength.assign(_cells.size(),0);
        for(const auto& e:_edges) {
            _incidentLength[e.a]+=e.length;_incidentLength[e.b]+=e.length;
        }
        _head.resize(_cells.size());RefreshHeads();
    }
    std::pair<double,double> Face(uint32_t i,bool alongX,bool positive) const {
        const auto& c=_cells[i];
        if(c.child==Coarse)return {c.bed,c.bed};
        const auto& g=_geometry[i-_children.size()];
        if(alongX)return positive?std::pair<double,double>{g.Corner10(),g.Corner11()}:std::pair<double,double>{g.Corner00(),g.Corner01()};
        return positive?std::pair<double,double>{g.Corner01(),g.Corner11()}:std::pair<double,double>{g.Corner00(),g.Corner10()};
    }
    void Step(double rain,double sun,double wind) {
        for(uint32_t i=0;i<_cells.size();++i) if(_cells[i].active) {
            auto& c=_cells[i];const auto o=Describe(i,false);const double area=o.size*o.size;
            c.flowX=c.flowZ=0;
            if(c.bed<=_sea) {_budget.outlet+=c.volume;c.volume=0;continue;}
            const double input=rain*RainWaterMaximumRainMetresPerSecond*StepSeconds*area;
            c.volume+=input;_budget.rain+=input;
            const double infiltration=std::min(c.volume,.000004*StepSeconds*area);
            c.volume-=infiltration;_budget.infiltration+=infiltration;
            const double evaporation=std::min(c.volume,(.000002+sun*.000006+wind*.000001)*StepSeconds*area);
            c.volume-=evaporation;_budget.evaporation+=evaporation;
        }
        RefreshHeads();
        std::fill(_out.begin(),_out.end(),0);std::fill(_delta.begin(),_delta.end(),0);
        for(size_t i=0;i<_edges.size();++i) {
            const auto& e=_edges[i];const auto a=Describe(e.a,false),b=Describe(e.b,false);
            const double head=a.Head()-b.Head();const auto donor=head>0?a:b;
            if(head==0||donor.volume<=0){_flux[i]=0;continue;}
            const auto fa=Face(e.a,e.alongX,true),fb=Face(e.b,e.alongX,false);
            // Fine geometry is authoritative over a coarse centre sample. If
            // both sides are fine, inconsistent edits use the higher profile.
            const bool fineA=_cells[e.a].child!=Coarse,fineB=_cells[e.b].child!=Coarse;
            const auto face=fineA&&fineB?std::pair{std::max(fa.first,fb.first),std::max(fa.second,fb.second)}:fineA?fa:fb;
            double wet=RainWaterCellGeometry::FaceDepth(donor.Head(),face.first,face.second);
            if(!fineA&&!fineB)wet=donor.Depth(); // preserve the legacy coarse proposal
            else if((head>0?_cells[e.a]:_cells[e.b]).child==Coarse)wet=std::min(wet,donor.Depth());
            const double equilibrium=std::abs(head)/(1/(a.size*a.size)+1/(b.size*b.size));
            const auto donorIndex=head>0?e.a:e.b,receiverIndex=head>0?e.b:e.a;
            const auto receiver=head>0?b:a;
            const double middle=b.Head()+head*.5;
            // Bound the simultaneous sweep, not just each isolated edge.
            // Native V(H) accounts for partially wet triangles; face-length
            // shares sum to one even when a coarse face has four fine edges.
            // The outgoing/incoming sums therefore cannot carry a cell past
            // its midpoint with the lowest/highest incident neighbour head.
            const double outgoing=std::max(0.0,donor.volume-CellVolumeAtHead(donorIndex,middle))*
                e.length/_incidentLength[donorIndex];
            const double incoming=std::max(0.0,CellVolumeAtHead(receiverIndex,middle)-receiver.volume)*
                e.length/_incidentLength[receiverIndex];
            const double amount=std::min({equilibrium*.5,
                wet*std::min(5.0,std::sqrt(9.81*std::abs(head)))*StepSeconds*e.length,outgoing,incoming});
            _flux[i]=std::copysign(amount,head);_out[head>0?e.a:e.b]+=amount;
        }
        for(size_t i=0;i<_edges.size();++i) {
            const auto& e=_edges[i];double f=_flux[i];const auto donor=f>0?e.a:e.b;
            if(f==0)continue;
            if(_out[donor]>0) f*=std::min(1.0,_cells[donor].volume/_out[donor]);
            _delta[e.a]-=f;_delta[e.b]+=f;
            const double depth=std::max(Describe(donor,false).Depth(),.0001);
            for(const auto j:{e.a,e.b}) {
                auto& c=_cells[j];const float v=float(f/(2*depth*Describe(j,false).size*StepSeconds));
                if(e.alongX)c.flowX+=v;else c.flowZ+=v;
            }
        }
        for(size_t i=0;i<_cells.size();++i) if(_cells[i].active) {
            auto& c=_cells[i];c.volume=std::max(0.0,c.volume+_delta[i]);
            if(c.bed<=_sea) {_budget.outlet+=c.volume;c.volume=0;}
        }
        RefreshHeads();
        ++_revision;
    }
    int _width=0,_height=0;double _spacing=0,_x=0,_z=0,_sea=0,_pending=0;
    uint64_t _generation=0,_revision=0;size_t _tilesOwnerExtra=0;Budget _budget;
    size_t _tileLimit=MaxTiles;
    std::vector<Cell> _cells;std::vector<uint32_t> _children;std::vector<Edge> _edges;
    std::vector<double> _out,_delta,_flux,_head,_incidentLength;std::vector<RainWaterCellGeometry> _geometry;
    std::map<std::pair<int,int>,bool> _tiles;
    std::vector<std::pair<uint32_t,uint32_t>> _freeBlocks;
};
}
