#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include <array>
#include <optional>
#include <span>
#include <map>
#include "RainWaterFine.hpp"
#include "RainWaterForcing.hpp"
#include "RainWaterCost.hpp"

namespace Poseidon
{
// Simulation-owned surface runoff. Equal-area cells store metres of water over
// the actual terrain bed, not a renderer noise mask. The conservative donor
// limiter moves water down the FREE SURFACE (bed + depth), so hollows retain it
// until their spill height is reached. Queries never advance the simulation.
class RainWaterField
{
public:
    static constexpr float StepSeconds = 0.25f;
    static constexpr size_t MaxCells = 1024 * 1024;
    struct Sample { float depth = 0, height = 0, flowX = 0, flowZ = 0; bool valid = false; };
    struct Budget { double rain = 0, infiltration = 0, evaporation = 0, outlet = 0; };
    struct SourceGrid { int side = 0, stride = 0; float spacing = 0; };
    static constexpr size_t FineTileLimit=8,FootprintLimit=32;
    struct EditFootprint {int minX=0,minZ=0,maxX=0,maxZ=0;uint64_t first=0,last=0;};
    struct FineUpdate {size_t admitted=0,updated=0,refused=0;bool changed=false,coverageLost=false,valid=true,native=false;};
    struct NaturalUpdate {size_t admitted=0,retired=0,refused=0,examined=0;bool changed=false,valid=true;};
    static constexpr double NaturalInterval=5.0,NaturalDrySeconds=10.0;
    // Scheduling only: not a physical wet/dry definition or renderer mask.
    static constexpr double NaturalPeakDepth=.002,NaturalExcessDepth=.001;
    struct FinePublication {
        uint64_t generation=0,revision=0,sourceRevision=0;
        bool valid=false;
        std::vector<RainWaterFine::Surface> surfaces;
    };
    static SourceGrid AlignedSourceGrid(int range, float spacing) {
        if (range < 2 || !std::isfinite(spacing) || spacing <= 0) return {};
        int64_t stride = 1;
        const int64_t last = static_cast<int64_t>(range)-1;
        while ((last+stride-1)/stride+1 > 513) stride *= 2;
        const float outputSpacing = spacing*static_cast<float>(stride);
        if (!std::isfinite(outputSpacing)) return {};
        return {static_cast<int>((last+stride-1)/stride+1),static_cast<int>(stride),outputSpacing};
    }

    bool Configure(int width, int height, float spacing, std::vector<float> bed,
                   float originX = 0, float originZ = 0, float seaLevel = 0)
    {
        if (width < 2 || height < 2 || static_cast<size_t>(width) * height > MaxCells ||
            bed.size() != static_cast<size_t>(width) * height ||
            !std::isfinite(spacing) || spacing <= 0 || !std::isfinite(originX) ||
            !std::isfinite(originZ) || !std::isfinite(seaLevel) ||
            std::any_of(bed.begin(), bed.end(), [](float y) { return !std::isfinite(y); })) return false;
        // Explicit mission Reset retires edits; first rain Configure must not
        // erase genuine same-mission craters recorded while the field was dry.
        const auto journal=_journal;const auto source=_source;
        const size_t count=_editCount;const auto latest=_sourceLatest;
        const bool gap=_sourceGap,dirty=_sourceDirty;
        Reset();_journal=journal;_source=source;_editCount=count;_sourceLatest=latest;
        _sourceGap=gap;_sourceDirty=dirty;
        _width = width; _height = height; _spacing = spacing;
        _originX = originX; _originZ = originZ; _seaLevel = seaLevel;
        _bed = std::move(bed);
        _depth.resize(_bed.size()); _out.resize(_bed.size());
        _delta.resize(_bed.size()); _flowX.resize(_bed.size()); _flowZ.resize(_bed.size());
        _edgeX.resize(static_cast<size_t>(width-1)*height);
        _edgeZ.resize(static_cast<size_t>(width)*(height-1));
        return true;
    }
    void Reset() {
        const uint64_t nextGeneration = _generation + 1;
        *this = RainWaterField{};
        _generation = nextGeneration;
    }
    // Terrain editing changes bed elevation while preserving water volume.
    bool SetBed(int x, int z, float height)
    {
        if (x < 0 || z < 0 || x >= _width || z >= _height || !std::isfinite(height)) return false;
        _bed[Index(x,z)] = height;
        if(_fine)_fine->SetCoarseBed(x,z,height);
        ++_revision; return true;
    }
    void Advance(float seconds, float rain, float sunlight = 0, float wind = 0)
    {
        if(_fine) {
            if(!std::isfinite(seconds)||seconds<=0||!std::isfinite(rain))return;
            const auto before=_fine->Revision();
            if(_sourceDirty||_fineStale)_fine->Defer(seconds);
            else _fine->Advance(seconds,rain,sunlight,wind);
            _revision+=_fine->Revision()-before;
            if(_fine->Revision()!=before)SyncFine();return;
        }
        if (_bed.empty() || !std::isfinite(seconds) || seconds <= 0 || !std::isfinite(rain)) return;
        RainWaterCost::AdvanceScope cost;
        rain = std::clamp(rain, 0.0f, 1.0f);
        sunlight = std::isfinite(sunlight) ? std::clamp(sunlight, 0.0f, 1.0f) : 0;
        wind = std::isfinite(wind) ? std::clamp(wind, 0.0f, 1.0f) : 0;
        _pending += seconds;
        // Bounded per game tick; retain all pending time for subsequent ticks.
        // Fixed steps make pause and ordinary frame-rate partitions equivalent.
        int steps = 0;
        while (_pending + 1e-7 >= StepSeconds && steps++ < 16) {
            Step(rain, sunlight, wind, cost); _pending -= StepSeconds;
        }
    }
    Sample At(float x, float z) const
    {
        if (_bed.empty() || !std::isfinite(x) || !std::isfinite(z)) return {};
        if(_fine&&(_sourceDirty||_fineStale))return {};
        const float gx = (x - _originX) / _spacing, gz = (z - _originZ) / _spacing;
        if (gx < 0 || gz < 0 || gx > _width - 1 || gz > _height - 1) return {};
        if(_fine&&_fine->IsFineAt(x,z)) {
            const auto owner=_fine->At(x,z);
            return {float(_fine->LocalDepth(x,z)),float(owner.Head()),owner.flowX,owner.flowZ,true};
        }
        const int ix = std::min(static_cast<int>(gx), _width - 2);
        const int iz = std::min(static_cast<int>(gz), _height - 2);
        const float fx = gx - ix, fz = gz - iz;
        const auto interpolate = [&](const std::vector<float>& values) {
            const float a = values[Index(ix,iz)] * (1-fx) + values[Index(ix+1,iz)] * fx;
            const float b = values[Index(ix,iz+1)] * (1-fx) + values[Index(ix+1,iz+1)] * fx;
            return a * (1-fz) + b * fz;
        };
        const float depth = interpolate(_depth);
        // Match Landscape::SurfaceY's actual terrain triangle diagonal. A
        // bilinear bed creates an artificial saddle between four corner heights.
        const float y00=_bed[Index(ix,iz)], y01=_bed[Index(ix+1,iz)];
        const float y10=_bed[Index(ix,iz+1)], y11=_bed[Index(ix+1,iz+1)];
        const float bed = fx+fz <= 1 ? y00+(y01-y00)*fx+(y10-y00)*fz
            : y11+(y10-y11)*(1-fx)+(y01-y11)*(1-fz);
        return {depth, bed + depth, interpolate(_flowX), interpolate(_flowZ), true};
    }
    double Volume() const {
        if(_fine)return _fine->Volume();
        double sum = 0; for (float d : _depth) sum += d;
        return sum * _spacing * _spacing;
    }
    const Budget& WaterBudget() const { return _budget; }
    // Explicit paused diagnostic only. Spans remain borrowed from the producer;
    // no normal-step copy/allocation and no reconstruction from rounded heads.
    struct CoarseCaptureView {
        int width=0,height=0;
        float spacing=0,originX=0,originZ=0,seaLevel=0;
        double pending=0;
        uint64_t generation=0,revision=0;
        Budget budget;
        std::span<const float> bed,depth;
    };
    std::optional<CoarseCaptureView> CoarseCapture() const {
        constexpr size_t limit=size_t(513)*513;
        if(_fine||_width<2||_height<2||_width>513||_height>513||
           _bed.size()!=size_t(_width)*_height||_bed.size()>limit||_depth.size()!=_bed.size()||
           !std::isfinite(_spacing)||_spacing<=0||!std::isfinite(_originX)||!std::isfinite(_originZ)||
           !std::isfinite(_seaLevel)||!std::isfinite(_pending)||_pending<0||
           !std::isfinite(_budget.rain)||!std::isfinite(_budget.infiltration)||
           !std::isfinite(_budget.evaporation)||!std::isfinite(_budget.outlet)||
           std::any_of(_bed.begin(),_bed.end(),[](float v){return !std::isfinite(v);})||
           std::any_of(_depth.begin(),_depth.end(),[](float v){return !std::isfinite(v)||v<0;})) return {};
        return CoarseCaptureView{_width,_height,_spacing,_originX,_originZ,_seaLevel,_pending,
            _generation,_revision,_budget,_bed,_depth};
    }
    uint64_t Revision() const { return _revision; }
    uint64_t Generation() const { return _generation; }
    int Width() const { return _width; }
    int Height() const { return _height; }
    float Spacing() const { return _spacing; }
    float OriginX() const { return _originX; }
    float OriginZ() const { return _originZ; }
    // Height revisions can also change terrain-grid identity. Never apply a
    // differently spaced source bed to old water coordinates. Exact comparison
    // is intentional: these values are copied from the same source-grid key.
    bool MatchesDomain(int width, int height, float spacing,
                       float originX = 0, float originZ = 0) const {
        return !_bed.empty() && _width == width && _height == height &&
               _spacing == spacing && _originX == originX && _originZ == originZ;
    }
    void SetSeaLevel(float level) { if (std::isfinite(level)){_seaLevel = level;if(_fine)_fine->SetSeaLevel(level);} }
    // Immutable simulation cell data, copied by the main-thread presentation
    // producer. Camera movement does not seed, erase or recompute any water.
    std::vector<float> Snapshot() const {
        std::vector<float> cells(_bed.size() * 4);
        for (size_t i=0; i<_bed.size(); ++i) {
            cells[4*i] = _bed[i] + _depth[i]; cells[4*i+1] = _depth[i];
            cells[4*i+2] = _flowX[i]; cells[4*i+3] = _flowZ[i];
        }
        return cells;
    }
    double PendingSeconds() const { return _fine?_fine->PendingSeconds():_pending; }
    // Test/fixture seeding is explicit and finite; ordinary missions start dry.
    bool AddWater(int x, int z, float metres) {
        if (x < 0 || z < 0 || x >= _width || z >= _height ||
            !std::isfinite(metres) || metres < 0) return false;
        if(_fine) {
            const bool result=_fine->AddWater(_originX+x*_spacing,_originZ+z*_spacing,double(metres)*_spacing*_spacing);
            if(result){++_revision;SyncFine();}return result;
        }
        _depth[Index(x,z)] += metres; ++_revision; return true;
    }
    bool FineActive() const {return _fine.has_value();}
    bool SourceReady() const {return !_fine||(!_sourceDirty&&!_fineStale);}
    size_t FineTileCount() const {return _fine?_fine->TileCount():0;}
    FinePublication FineSnapshot() const {
        FinePublication result{_generation,_revision,_sourceApplied,false,{}};
        result.valid=_fine&&!_sourceDirty&&!_fineStale&&_fine->TileCount()>0;
        // Ownership rectangles remain available while invalid, so a renderer
        // can suppress their coarse proxies without drawing stale fine heads.
        if(_fine)result.surfaces=_fine->FineSnapshot();return result;
    }
    // Called only by the actual successful HeightChange publisher. No camera,
    // terrain noise or synthetically seeded fixture can promote an edit here.
    void RecordTerrainVertex(const void* owner,int range,float spacing,int x,int z,uint64_t revision) {
        if(!ObserveTerrainRevision(owner,range,spacing,x,z,revision))return;
        if(_editCount) {
            auto& previous=_journal[_editCount-1];
            if(revision==previous.last+1&&x>=previous.minX-2&&x<=previous.maxX+2&&z>=previous.minZ-2&&z<=previous.maxZ+2) {
                previous.minX=std::min(previous.minX,x);previous.maxX=std::max(previous.maxX,x);
                previous.minZ=std::min(previous.minZ,z);previous.maxZ=std::max(previous.maxZ,z);previous.last=revision;return;
            }
        }
        if(_editCount==FootprintLimit){_sourceGap=true;return;}
        _journal[_editCount++]={x,z,x,z,revision,revision};
    }
    // HeightChange also increments the revision for raw-height quantization
    // no-ops. Proven finite unchanged writes cover that revision, but NEVER
    // produce a footprint or admit a tile by themselves.
    void RecordTerrainUnchanged(const void* owner,int range,float spacing,int x,int z,uint64_t revision) {
        if(ObserveTerrainRevision(owner,range,spacing,x,z,revision)&&_editCount) {
            auto& previous=_journal[_editCount-1];
            if(previous.last+1==revision)previous.last=revision;
        }
    }
    // Sampler reads finite bare source vertices (never SurfaceY mud/sand or an
    // out-of-range sentinel). It is called only after source identity admission.
    template<class Sampler>
    FineUpdate UpdateTerrainSource(const void* owner,int range,float sourceSpacing,uint64_t revision,bool enabled,Sampler sample) {
        FineUpdate report;
        if(!enabled)return report; // default legacy path has no geometry work
        const SourceIdentity key{owner,range,sourceSpacing};
        if(!owner||range<2||!std::isfinite(sourceSpacing)||sourceSpacing<=0){
            InvalidateFineSource();report.valid=false;report.changed=_fine.has_value();return report;
        }
        if(!(key==_source)) {
            _source=key;_sourceLatest=revision;_sourceApplied=0;_editCount=0;_sourceGap=true;_sourceDirty=true;
        }
        if(_width==0)return report; // keep before-rain footprints queued
        if(_sourceApplied==revision&&!_sourceDirty)return report;
        report.changed=true;report.coverageLost=_sourceGap||_sourceLatest!=revision;
        // An unobserved edit might overlap a natural owner: keep that existing
        // owner pinned rather than retire a potentially authored crater later.
        if(report.coverageLost)_naturalTiles.clear();
        const auto grid=AlignedSourceGrid(range,sourceSpacing);
        report.native=sourceSpacing==6.25f&&_spacing==25&&grid.stride==4&&
            _width==grid.side&&_height==grid.side&&_originX==0&&_originZ==0;
        bool refreshed=true;
        if(_fine&&!report.native&&_fine->TileCount()) {
            refreshed=_fine->RegridDomain(_width,_height,_spacing,_bed,_originX-_spacing*.5,_originZ-_spacing*.5);
            if(refreshed){++_generation;_naturalTiles.clear();}
        }
        if(_fine&&report.native)for(const auto& tile:_fine->Tiles()) {
            const auto geometry=ReadTile(tile.first,tile.second,range,sample,false);
            if(geometry.empty()||!_fine->RefineTileGeometry(tile.first,tile.second,geometry))refreshed=false;
            else ++report.updated;
        }
        if(report.native&&!report.coverageLost) {
            size_t candidates=0;
            for(size_t n=0;n<_editCount;++n) {
                const auto& e=_journal[n];
                const int ax=(e.minX+1)/32,az=(e.minZ+1)/32,bx=(e.maxX+2)/32,bz=(e.maxZ+2)/32;
                const size_t count=size_t(bx-ax+1)*(bz-az+1);
                if(count>128-candidates){report.refused+=count;continue;}
                candidates+=count;
                for(int z=az;z<=bz;++z)for(int x=ax;x<=bx;++x) {
                    if(_fine&&_fine->HasTile(x,z)){_naturalTiles.erase({x,z});continue;}
                    if(FineTileCount()>=FineTileLimit){++report.refused;continue;}
                    const auto geometry=ReadTile(x,z,range,sample,true);
                    if(geometry.empty()||!ActivateFine()||!_fine->RefineTileGeometry(x,z,geometry)){++report.refused;continue;}
                    ++report.admitted;++_generation; // ownership topology changed
                }
            }
        }
        _fineStale=!refreshed;report.valid=refreshed;
        if(refreshed){_sourceApplied=revision;_sourceLatest=revision;_sourceDirty=false;_sourceGap=false;_editCount=0;}
        if(_fine){++_revision;SyncFine();}return report;
    }
    // Ordinary retained runoff can request native resolution without forging a
    // HeightChange footprint. Only real simulation time schedules this work;
    // camera/render queries never promote, retire, or move water.
    template<class Sampler>
    NaturalUpdate UpdateNaturalTerrain(const void* owner,int range,float sourceSpacing,uint64_t revision,
        bool enabled,float seconds,Sampler sample) {
        NaturalUpdate report;
        if(!enabled||!std::isfinite(seconds)||seconds<=0||_width==0)return report;
        const auto grid=AlignedSourceGrid(range,sourceSpacing);
        if(!owner||!(SourceIdentity{owner,range,sourceSpacing}==_source)||
           _sourceApplied!=revision||_sourceDirty||_fineStale) {report.valid=false;return report;}
        // Exact current paired ABI: do not invent an alternative layout for an
        // unsupported source resolution or partially covered outer tile.
        if(sourceSpacing!=6.25f||_spacing!=25||grid.stride!=4||
           _width!=grid.side||_height!=grid.side||_originX!=0||_originZ!=0)return report;
        for(auto& [tile,dry]:_naturalTiles) {
            if(_fine->TileVolume(tile.first,tile.second)!=0)dry=-1;
            // The first observed empty tick starts the quiet interval. Its
            // elapsed time cannot prove when a formerly wet owner drained.
            else dry=dry<0?0:dry+seconds;
        }
        _naturalPending+=seconds;
        if(_naturalPending<NaturalInterval)return report;
        // Coalesce missed scheduling slots; the solver's physical pending time
        // is separate and retained. A long tick cannot create a scan backlog.
        _naturalPending=std::fmod(_naturalPending,NaturalInterval);
        // Retire only exact emptiness after a quiet interval. No residual-volume
        // threshold, wet eviction, or loss of the global water budget is allowed.
        for(auto i=_naturalTiles.begin();i!=_naturalTiles.end();) {
            if(i->second>=NaturalDrySeconds&&_fine->CoarsenDryTile(i->first.first,i->first.second,_bed)) {
                i=_naturalTiles.erase(i);++report.retired;++_generation;
            }else ++i;
        }
        if(report.retired)SyncFine();
        const size_t columns=size_t((_width+7)/8),rows=size_t((_height+7)/8);
        if(_naturalRefusedRevision!=revision||_naturalRefused.size()!=columns*rows) {
            _naturalRefused.assign(columns*rows,false);_naturalRefusedRevision=revision;
        }
        // This bounded coarse scan reads at most 513^2 cached depths every five
        // sim seconds; native geometry is read only for at most two candidates.
        double sum=0;size_t inland=0;
        for(size_t i=0;i<_depth.size();++i)if(_bed[i]>_seaLevel){sum+=_depth[i];++inland;}
        const double background=inland?sum/inland:0;
        struct Candidate {int x,z;double excess;};
        std::vector<Candidate> candidates;
        if(FineTileCount()<FineTileLimit)for(size_t z=1;z<rows;++z)for(size_t x=1;x<columns;++x) {
            if((x+1)*8>size_t(_width)||(z+1)*8>size_t(_height)||
               int(x*32+30)>=range||int(z*32+30)>=range||_naturalRefused[z*columns+x]||
               (_fine&&_fine->HasTile(int(x),int(z))))continue;
            double peak=0,excess=0;bool inlandTile=true;
            for(size_t iz=z*8;iz<(z+1)*8;++iz)for(size_t ix=x*8;ix<(x+1)*8;++ix) {
                const size_t i=iz*_width+ix;
                inlandTile&=_bed[i]>_seaLevel;peak=std::max(peak,double(_depth[i]));
                excess+=std::max(0.0,double(_depth[i])-background)*_spacing*_spacing;
            }
            if(inlandTile&&peak>NaturalPeakDepth&&peak>background+NaturalExcessDepth&&excess>0)
                candidates.push_back({int(x),int(z),excess});
        }
        std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){
            if(a.excess!=b.excess)return a.excess>b.excess;
            return std::pair{a.z,a.x}<std::pair{b.z,b.x};
        });
        for(const auto& c:candidates) {
            if(report.examined==2)break;
            ++report.examined;
            const auto geometry=ReadTile(c.x,c.z,range,sample,true);
            if(geometry.empty()) {
                _naturalRefused[size_t(c.z)*columns+c.x]=true;++report.refused;continue;
            }
            if(!ActivateFine()||!_fine->RefineTileGeometry(c.x,c.z,geometry)){++report.refused;break;}
            _naturalTiles[{c.x,c.z}]=-1;++report.admitted;++_generation;break;
        }
        report.changed=report.admitted||report.retired;
        if(report.changed){++_revision;SyncFine();}
        return report;
    }
    // Default legacy domain change still uses Configure/reset. An already
    // enabled fine backend retains total mass through explicit regrid density
    // redistribution; exact shoreline placement is deliberately not promised.
    bool ReconfigureFine(int width,int height,float spacing,std::span<const float> bed) {
        if(!_fine)return false;
        if(width<2||height<2||size_t(width)*height>RainWaterFine::MaxCoarseCells||
           !_fine->RegridDomain(width,height,spacing,bed,_originX-spacing*.5,_originZ-spacing*.5)) {
            InvalidateFineSource();return false;
        }
        _width=width;_height=height;_spacing=spacing;_bed.assign(bed.begin(),bed.end());
        _depth.resize(bed.size());_flowX.resize(bed.size());_flowZ.resize(bed.size());
        _out.resize(bed.size());_delta.resize(bed.size());++_generation;++_revision;
        _sourceDirty=true;_fineStale=true;_sourceApplied=0;_naturalTiles.clear();
        _naturalPending=0;_naturalRefused.clear();SyncFine();return true;
    }
private:
    void InvalidateFineSource() {
        if(!_fine)return;
        if(!_fineStale||!_sourceDirty)++_revision;
        _fineStale=true;_sourceDirty=true;
    }
    struct SourceIdentity {
        const void* owner=nullptr;int range=0;float spacing=0;
        bool operator==(const SourceIdentity&) const=default;
    };
    bool ObserveTerrainRevision(const void* owner,int range,float spacing,int x,int z,uint64_t revision) {
        if(!owner||range<2||!std::isfinite(spacing)||spacing<=0||x<0||z<0||x>=range||z>=range||revision==0)return false;
        const SourceIdentity key{owner,range,spacing};
        if(!(key==_source)) {
            _source=key;_editCount=0;_sourceLatest=revision-1;_sourceApplied=0;_sourceGap=false;
        }
        if(revision!=_sourceLatest+1)_sourceGap=true;
        _sourceLatest=revision;_sourceDirty=true;if(_fine){_fineStale=true;++_revision;}
        return true;
    }
    template<class Sampler>
    std::vector<RainWaterCellGeometry> ReadTile(int x,int z,int range,Sampler sample,bool inland) const {
        const int sx=x*32-2,sz=z*32-2;
        if(x<0||z<0||sx<0||sz<0||sx+32>=range||sz+32>=range||
           x*8+8>_width||z*8+8>_height)return {};
        std::array<float,33*33> heights;
        for(int iz=0;iz<33;++iz)for(int ix=0;ix<33;++ix) {
            const float h=sample(sx+ix,sz+iz);
            if(!std::isfinite(h)||(inland&&h<=_seaLevel))return {};
            heights[iz*33+ix]=h;
        }
        std::vector<RainWaterCellGeometry> result;result.reserve(1024);
        for(int iz=0;iz<32;++iz)for(int ix=0;ix<32;++ix)
            result.emplace_back(heights[iz*33+ix],heights[iz*33+ix+1],heights[(iz+1)*33+ix],heights[(iz+1)*33+ix+1]);
        return result;
    }
    bool ActivateFine() {
        if(_fine)return true;
        RainWaterFine candidate;
        const RainWaterFine::Budget budget{_budget.rain,_budget.infiltration,_budget.evaporation,_budget.outlet};
        if(!candidate.Configure(_width,_height,_spacing,_bed,_originX-_spacing*.5,_originZ-_spacing*.5,_seaLevel)||
           !candidate.SetTileLimit(FineTileLimit)||!candidate.ImportLegacy(_depth,budget,std::max(0.0,_pending)))return false;
        _fine=std::move(candidate);++_generation;++_revision;return true;
    }
    void SyncFine() {
        _fine->CoarseState(_depth,_flowX,_flowZ);const auto b=_fine->WaterBudget();
        _budget={b.rain,b.infiltration,b.evaporation,b.outlet};_pending=_fine->PendingSeconds();
    }
    size_t Index(int x, int z) const { return static_cast<size_t>(z) * _width + x; }
    float Transfer(size_t a, size_t b) const {
        const float head = (_bed[a] + _depth[a]) - (_bed[b] + _depth[b]);
        const size_t donor = head > 0 ? a : b;
        if (head == 0 || _depth[donor] <= 0) return 0;
        // Bounded shallow runoff speed; the head-based cap cannot overshoot
        // level equilibrium even when one cell has a steep bare bed.
        const float speed = std::min(5.0f, std::sqrt(9.81f * std::abs(head)));
        const float amount = std::min(std::abs(head) * 0.25f,
            _depth[donor] * speed * StepSeconds / _spacing);
        return std::copysign(amount, head);
    }
    template<class F> void Edges(F visit) const {
        for (int z=0; z<_height; ++z) for (int x=0; x<_width; ++x) {
            const size_t a = Index(x,z);
            if (x+1<_width) visit(a,Index(x+1,z),true);
            if (z+1<_height) visit(a,Index(x,z+1),false);
        }
    }
    void Step(float rain, float sunlight, float wind, RainWaterCost::AdvanceScope& cost) {
        RainWaterCost::PhaseTimer phase(cost.Active());
        const double area = static_cast<double>(_spacing) * _spacing;
        const float input = rain * static_cast<float>(RainWaterMaximumRainMetresPerSecond) * StepSeconds;
        const float evaporation = (0.000002f + sunlight * 0.000006f + wind * 0.000001f) * StepSeconds;
        const float infiltration = 0.000004f * StepSeconds;
        bool hasWater = false;
        // Dispatch only: finite caps exclude unsafe head/flux arithmetic.
        // Four incident faces at speed<=5 and dt=.25 donate at most half a
        // normal donor depth at spacing>=10, with ample IEEE rounding margin.
        bool canFuse = _spacing >= 10.0f && _spacing <= 1000000.0f;
        for (size_t i=0; i<_depth.size(); ++i) {
            // Check sea beds too: their zero depth still enters inland heads.
            canFuse &= std::abs(_bed[i]) <= 1000000.0f;
            // Sea is a real outlet, not a source of rainwater above dry terrain.
            if (_bed[i] <= _seaLevel) {
                _budget.outlet += _depth[i] * area; _depth[i] = 0; continue;
            }
            _depth[i] += input; _budget.rain += input * area;
            const float absorbed = std::min(_depth[i], infiltration);
            _depth[i] -= absorbed; _budget.infiltration += absorbed * area;
            const float evaporated = std::min(_depth[i], evaporation);
            _depth[i] -= evaporated; _budget.evaporation += evaporated * area;
            hasWater |= _depth[i] > 0;
            canFuse &= _depth[i] <= 1024.0f &&
                (_depth[i] == 0.0f || _depth[i] >= std::numeric_limits<float>::min());
        }
        phase.End(RainWaterCost::Phase::SourceSink);
        cost.Step(!hasWater);
        std::fill(_out.begin(),_out.end(),0); std::fill(_delta.begin(),_delta.end(),0);
        std::fill(_flowX.begin(),_flowX.end(),0); std::fill(_flowZ.begin(),_flowZ.end(),0);
        phase.End(RainWaterCost::Phase::Clear);
        // Every donor is exactly dry after the unchanged source/sink pass.
        // Transfer then returns +0 on every edge. Preserve the clears, final
        // cell pass, budgets and revision, but avoid two empty edge traversals.
        // The next cached step overwrites every transfer before reading it.
        if (hasWater) {
            if (canFuse) {
                // The original donor multiplier is exactly one. Keep the
                // original interleaved edge, delta and flow update order.
                Edges([&](size_t a,size_t b,bool alongX) {
                    const float f=Transfer(a,b);const size_t donor=f>0?a:b;
                    _delta[a]-=f;_delta[b]+=f;
                    auto& flow=alongX ? _flowX : _flowZ;
                    const float velocity=f*_spacing/StepSeconds/std::max(_depth[donor],0.0001f);
                    flow[a]+=velocity*0.5f;flow[b]+=velocity*0.5f;
                });
                phase.End(RainWaterCost::Phase::FusedFlux);
            } else {
                size_t edgeX=0,edgeZ=0;
                Edges([&](size_t a,size_t b,bool alongX) {
                    const float f = Transfer(a,b);
                    (alongX ? _edgeX[edgeX++] : _edgeZ[edgeZ++]) = f;
                    _out[f>0?a:b] += std::abs(f);
                });
                phase.End(RainWaterCost::Phase::RawTransfer);
                // Bed/depth do not change between these passes. Reuse only the raw
                // transfer from this step, in the same interleaved row/axis edge order;
                // donor normalization, delta and flow writes retain their exact order.
                edgeX=0;edgeZ=0;
                Edges([&](size_t a,size_t b,bool alongX) {
                    float f = alongX ? _edgeX[edgeX++] : _edgeZ[edgeZ++]; const size_t donor = f>0?a:b;
                    if (_out[donor]>0)
                        // The clamp is exactly one when outgoing<=depth. Keep the
                        // multiply and full ratio fallback for limited/nonfinite cases.
                        f *= _out[donor] <= _depth[donor] ? 1.0f : std::min(1.0f,_depth[donor]/_out[donor]);
                    _delta[a] -= f; _delta[b] += f;
                    auto& flow = alongX ? _flowX : _flowZ;
                    // Signed volume flux converted to a surface-flow speed where wet.
                    const float velocity = f * _spacing / StepSeconds / std::max(_depth[donor],0.0001f);
                    flow[a] += velocity * 0.5f; flow[b] += velocity * 0.5f;
                });
                phase.End(RainWaterCost::Phase::LimitedFlux);
            }
        }
        for (size_t i=0; i<_depth.size(); ++i) {
            _depth[i] = std::max(0.0f,_depth[i]+_delta[i]);
            if (_bed[i] <= _seaLevel) {
                _budget.outlet += _depth[i] * area; _depth[i]=0;
            }
        }
        ++_revision;
        phase.End(RainWaterCost::Phase::FinalCells);
    }
    int _width=0,_height=0;
    float _spacing=1,_originX=0,_originZ=0,_seaLevel=0;
    double _pending=0;
    uint64_t _revision=0, _generation=0;
    Budget _budget;
    std::vector<float> _bed,_depth,_out,_delta,_flowX,_flowZ;
    // Configure-sized scratch: every actual edge is overwritten before use.
    // Each vector has fewer than MaxCells floats; no allocation in Step.
    std::vector<float> _edgeX,_edgeZ;
    std::optional<RainWaterFine> _fine;
    SourceIdentity _source;
    std::array<EditFootprint,FootprintLimit> _journal;
    size_t _editCount=0;
    uint64_t _sourceLatest=0,_sourceApplied=0;
    bool _sourceGap=false,_sourceDirty=false,_fineStale=false;
    std::map<std::pair<int,int>,double> _naturalTiles;
    std::vector<bool> _naturalRefused;
    uint64_t _naturalRefusedRevision=0;
    double _naturalPending=0;
};
inline RainWaterField& GRainWater() { static RainWaterField field; return field; }
}
