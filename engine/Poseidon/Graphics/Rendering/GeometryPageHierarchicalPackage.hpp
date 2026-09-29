#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageClodBake.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <cstring>

namespace Poseidon::GeometryPages
{
// Explicit offline, rigid single-material bake consumer. Owned typed RAM pages;
// this is neither a disk codec nor a retail eligibility or GPU lifetime policy.
// Keep the successful source bake immutable throughout BuildHierarchicalPackage.
struct HierarchicalIdentity
{
    SourceIdentity source;
    std::array<uint8_t,32> packageSha256{};
    uint32_t adapterVersion=1;
    bool operator==(const HierarchicalIdentity&) const = default;
};
struct HierarchicalCluster
{
    uint32_t group=0,page=0,pageSlot=0;
    int refined=-1;
    clodBounds bounds{};
};
struct HierarchicalPayload
{
    uint32_t cluster=0,material=0;
    std::vector<uint32_t> originalVertexIds;
    std::vector<SVertex> vertices;
    std::vector<uint32_t> indices;
};
struct HierarchicalPage
{
    uint32_t group=0;
    std::vector<HierarchicalPayload> clusters;
    uint64_t logicalBytes=16,uploadBytes=0;
};
struct HierarchicalPackage
{
    HierarchicalIdentity identity;
    std::vector<ClodBakeGroup> groups;
    std::vector<HierarchicalCluster> clusters;
    std::vector<HierarchicalPage> pages;
    std::vector<uint32_t> rootClusters,rootPages;
    uint32_t pageByteLimit=65536;
    uint64_t knownCapacityBytes=0;
};
struct HierarchicalLimits
{
    uint32_t groups=64,clusters=256,pages=64,pageBytes=65536;
    uint64_t knownCapacityBytes=1024*1024;
};
enum class HierarchicalBuildStatus { Built,Invalid,Capacity,AllocationFailed };
inline uint64_t HierarchicalKnownBytes(const HierarchicalPackage& value)
{
    uint64_t bytes=sizeof(value)+value.groups.capacity()*sizeof(ClodBakeGroup)+
        value.clusters.capacity()*sizeof(HierarchicalCluster)+value.pages.capacity()*sizeof(HierarchicalPage)+
        (value.rootClusters.capacity()+value.rootPages.capacity())*sizeof(uint32_t);
    for(const auto& page:value.pages) {
        bytes+=page.clusters.capacity()*sizeof(HierarchicalPayload);
        for(const auto& c:page.clusters) bytes+=c.vertices.capacity()*sizeof(SVertex)+
            (c.originalVertexIds.capacity()+c.indices.capacity())*sizeof(uint32_t);
    }
    return bytes; // Known owned C++ capacity; excludes source bake/scratch/allocator/RSS.
}
namespace HierarchicalDetail
{
inline bool Nonzero(const std::array<uint8_t,32>& hash)
{return std::any_of(hash.begin(),hash.end(),[](uint8_t b){return b!=0;});}
inline std::array<uint8_t,32> Digest(Foundation::Sha256& hash)
{
    const auto hex=hash.Hex();std::array<uint8_t,32> value{};
    const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(size_t i=0;i<value.size();++i)value[i]=uint8_t(digit(hex[2*i])*16+digit(hex[2*i+1]));
    return value;
}
inline bool ValidTopology(std::span<const ClodBakeGroup> groups,std::span<const HierarchicalCluster> clusters,
    std::array<uint32_t,64>& parents)
{
    if(groups.empty()||groups.size()>64||clusters.empty()||clusters.size()>256)return false;
    size_t next=0;
    for(uint32_t g=0;g<groups.size();++g) {
        const auto& group=groups[g];
        if(group.depth<0||group.depth>32||!ClodDetail::BoundsValid(group.simplified)||group.first!=next||
            !group.count||group.count>clusters.size()-next)return false;
        for(size_t c=next;c<next+group.count;++c) {
            const auto& cluster=clusters[c];
            if(cluster.group!=g||cluster.refined < -1||cluster.refined>=int(g)||!ClodDetail::BoundsValid(cluster.bounds))return false;
            if(cluster.refined>=0) {
                if(group.simplified.error<groups[cluster.refined].simplified.error)return false;
                ++parents[cluster.refined];
            }
        }
        next+=group.count;
    }
    if(next!=clusters.size())return false;
    for(size_t g=0;g<groups.size();++g)
        if((!parents[g])!=(groups[g].simplified.error==FLT_MAX))return false;
    return true;
}
}
// Metadata-only validation for explicitly immutable Build-produced packages.
// Does not authenticate arbitrary/mutated payloads. Keep the package frozen after
// successful publication; workers borrow/copy a page without modifying its source.
inline bool ValidHierarchicalMetadata(const HierarchicalPackage& value)
{
    const auto& key=value.identity;
    if(key.adapterVersion!=1||!HierarchicalDetail::Nonzero(key.packageSha256)||
        !HierarchicalDetail::Nonzero(key.source.sourceSha256)||!key.source.producerVersion||
        !key.source.vertexLayout||!key.source.materialMapping||key.source.coarseRepresentation==key.source.fineRepresentation||
        value.pageByteLimit<1024||value.pageByteLimit>65536||value.pages.empty()||value.pages.size()>64)return false;
    std::array<uint32_t,64> parents{};
    if(!HierarchicalDetail::ValidTopology(value.groups,value.clusters,parents))return false;
    size_t next=0,rootAt=0,rootPageAt=0;
    for(uint32_t p=0;p<value.pages.size();++p) {
        const auto& page=value.pages[p];
        if(page.group>=value.groups.size()||page.clusters.empty()||page.clusters.size()>256||
            page.logicalBytes<16||page.logicalBytes>value.pageByteLimit||!page.uploadBytes)return false;
        if(!parents[page.group]) {
            if(rootPageAt>=value.rootPages.size()||value.rootPages[rootPageAt++]!=p)return false;
        }
        for(uint32_t slot=0;slot<page.clusters.size();++slot,++next) {
            if(next>=value.clusters.size())return false;
            const auto& c=value.clusters[next];
            if(c.page!=p||c.pageSlot!=slot||c.group!=page.group||page.clusters[slot].cluster!=next)return false;
            if(!parents[c.group]) {
                if(rootAt>=value.rootClusters.size()||value.rootClusters[rootAt++]!=next)return false;
            }
        }
    }
    return next==value.clusters.size()&&rootAt==value.rootClusters.size()&&rootPageAt==value.rootPages.size()&&rootAt;
}
inline HierarchicalBuildStatus BuildHierarchicalPackage(const ClodBake& bake,HierarchicalPackage& destination,
    HierarchicalLimits limits={})
{
    if(!limits.groups||limits.groups>64||!limits.clusters||limits.clusters>256||!limits.pages||limits.pages>64||
        limits.pageBytes<1024||limits.pageBytes>65536||!limits.knownCapacityBytes||limits.knownCapacityBytes>1024*1024||
        !HierarchicalDetail::Nonzero(bake.source.sourceSha256)||!bake.source.producerVersion||
        !bake.source.vertexLayout||!bake.source.materialMapping||bake.source.coarseRepresentation==bake.source.fineRepresentation)
        return HierarchicalBuildStatus::Invalid;
    if(bake.groups.size()>limits.groups||bake.clusters.size()>limits.clusters)return HierarchicalBuildStatus::Capacity;
    const auto& source=bake.original;
    Limits inputLimits;inputLimits.maxVertices=4096;inputLimits.maxTriangles=8192;
    if(!Detail::Valid(source.Input(),inputLimits))return HierarchicalBuildStatus::Invalid;
    const auto material=source.materials.front();for(auto m:source.materials)if(m!=material)return HierarchicalBuildStatus::Invalid;
    try {
        HierarchicalPackage result;result.identity.source=bake.source;result.pageByteLimit=limits.pageBytes;
        result.groups=bake.groups;result.clusters.resize(bake.clusters.size());
        for(size_t id=0;id<bake.clusters.size();++id) {
            const auto& c=bake.clusters[id];result.clusters[id]={c.group,0,0,c.refined,c.bounds};
        }
        std::array<uint32_t,64> parents{};
        if(!HierarchicalDetail::ValidTopology(result.groups,result.clusters,parents))
            return HierarchicalBuildStatus::Invalid;
        // Complete DAG payload, not two flattened threshold cuts. Same-group page
        // packing gives refinement dependencies an explicit independently charged
        // page set. Identical source IDs are never welded across seams/clusters.
        for(uint32_t id=0;id<bake.clusters.size();++id) {
            const auto& c=bake.clusters[id];
            if(c.indices.empty()||c.indices.size()%3||c.indices.size()>128*3)return HierarchicalBuildStatus::Invalid;
            HierarchicalPayload payload;payload.cluster=id;payload.material=material;
            payload.indices.reserve(c.indices.size());
            for(auto original:c.indices) {
                if(original>=source.vertices.size())return HierarchicalBuildStatus::Invalid;
                auto it=std::find(payload.originalVertexIds.begin(),payload.originalVertexIds.end(),original);
                const auto local=uint32_t(it-payload.originalVertexIds.begin());
                if(it==payload.originalVertexIds.end()) {
                    if(local==64)return HierarchicalBuildStatus::Invalid;
                    const auto& v=source.vertices[original];const auto p=source.positions[original];
                    if(v.pos.X()!=p.x||v.pos.Y()!=p.y||v.pos.Z()!=p.z||v.conform)return HierarchicalBuildStatus::Invalid;
                    payload.originalVertexIds.push_back(original);payload.vertices.push_back(v);
                }
                payload.indices.push_back(local);
            }
            const uint64_t logical=48+payload.vertices.size()*(sizeof(SVertex)+4)+payload.indices.size()*4;
            const uint64_t upload=payload.vertices.size()*sizeof(SVertex)+payload.indices.size()*4;
            if(logical+16>limits.pageBytes)return HierarchicalBuildStatus::Capacity;
            if(result.pages.empty()||result.pages.back().group!=c.group||result.pages.back().logicalBytes+logical>limits.pageBytes) {
                if(result.pages.size()==limits.pages)return HierarchicalBuildStatus::Capacity;
                HierarchicalPage page;page.group=c.group;result.pages.push_back(std::move(page));
                if(!parents[c.group])result.rootPages.push_back(uint32_t(result.pages.size()-1));
            }
            auto& page=result.pages.back();auto& descriptor=result.clusters[id];
            descriptor.page=uint32_t(result.pages.size()-1);descriptor.pageSlot=uint32_t(page.clusters.size());
            page.logicalBytes+=logical;page.uploadBytes+=upload;page.clusters.push_back(std::move(payload));
            if(!parents[c.group])result.rootClusters.push_back(id);
            if(HierarchicalKnownBytes(result)>limits.knownCapacityBytes)return HierarchicalBuildStatus::Capacity;
        }
        // All cluster index counts/IDs have been checked before this helper's
        // triangle loop. Its successful-bake precondition is not a parser bound.
        if(!ClodDetail::OriginalCoverage(bake))return HierarchicalBuildStatus::Invalid;
        Foundation::Sha256 hash;hash.Update(std::string("OpenPoseidon-hierarchical-ram-pages-v1"));
        hash.Update(std::string(ClodBake::LibraryRevision));
        const auto number=[&](uint64_t n,unsigned width=4){uint8_t bytes[8]{};for(unsigned i=0;i<width;++i)bytes[i]=uint8_t(n>>(8*i));hash.Update(bytes,width);};
        const auto bounds=[&](const clodBounds& b){for(float v:b.center)number(std::bit_cast<uint32_t>(v));number(std::bit_cast<uint32_t>(b.radius));number(std::bit_cast<uint32_t>(b.error));};
        const auto& s=bake.source;hash.Update(s.sourceSha256.data(),s.sourceSha256.size());
        number(s.geometryOptions,8);number(s.materialOptions,8);number(s.producerVersion);number(s.vertexLayout);number(s.materialMapping);
        number(s.coarseRepresentation);number(s.fineRepresentation);number(ClodBake::AdapterVersion);number(sizeof(SVertex));number(limits.pageBytes);
        number(result.groups.size());number(result.clusters.size());number(result.pages.size());
        for(const auto& g:result.groups){number(uint32_t(g.depth));bounds(g.simplified);number(g.first);number(g.count);}
        for(const auto& c:result.clusters){number(c.group);number(uint32_t(c.refined));bounds(c.bounds);number(c.page);number(c.pageSlot);}
        for(const auto& page:result.pages) {
            number(page.group);number(page.logicalBytes,8);number(page.uploadBytes,8);number(page.clusters.size());
            for(const auto& c:page.clusters){number(c.cluster);number(c.material);number(c.vertices.size());number(c.indices.size());
                for(auto id:c.originalVertexIds)number(id);
                hash.Update(c.vertices.data(),c.vertices.size()*sizeof(SVertex));for(auto i:c.indices)number(i);}
        }
        result.identity.packageSha256=HierarchicalDetail::Digest(hash);result.knownCapacityBytes=HierarchicalKnownBytes(result);
        if(result.knownCapacityBytes>limits.knownCapacityBytes)return HierarchicalBuildStatus::Capacity;
        if(!ValidHierarchicalMetadata(result))return HierarchicalBuildStatus::Invalid;
        destination=std::move(result);return HierarchicalBuildStatus::Built;
    }catch(const std::bad_alloc&){return HierarchicalBuildStatus::AllocationFailed;}
}
}
