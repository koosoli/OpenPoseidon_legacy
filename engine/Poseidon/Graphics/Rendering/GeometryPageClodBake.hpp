#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageShapeExport.hpp>
#include <meshoptimizer.h>
#include <clusterlod.h>
#include <cfloat>

namespace Poseidon::GeometryPages
{
// Offline controlled, immutable, rigid, single-material pilot only. One upstream
// call owns uninstrumented std::vector/library scratch: these input/output caps
// are NOT a hard peak-memory/time bound, cancellation API, or live worker policy.
struct ClodBakeLimits
{
    uint32_t vertices=4096, triangles=8192, groups=4096, clusters=16384;
    uint32_t indexEntries=1024*1024;
    uint64_t storedBytes=8ull*1024*1024;
};
struct ClodBakeGroup { int depth=0; clodBounds simplified{}; uint32_t first=0,count=0; };
struct ClodBakeCluster { uint32_t group=0; int refined=-1; clodBounds bounds{}; std::vector<uint32_t> indices; };
struct ClodBake
{
    SourceIdentity source;
    // This immutable value copy is the render vertex buffer; indices below refer
    // to original IDs. Attributes are never updated or re-quantized by the bake.
    ExportedMesh original;
    std::vector<ClodBakeGroup> groups;
    std::vector<ClodBakeCluster> clusters;
    uint64_t storedBytes=0; // logical contents, excludes capacity/allocator scratch
    static constexpr const char* LibraryRevision="9e1f07b159d3cb777f1c67ed31fc11fd117986f4";
    // AdapterVersion includes ALL fixed bake config, attribute weights/lock policy,
    // numeric domain and validation rules below. Any such change bumps this
    // version before derived-data reuse. No disk cache format is defined here.
    static constexpr uint32_t AdapterVersion=1;
};
enum class ClodBakeStatus { Baked, Unsupported, Invalid, Capacity, AllocationFailed };
namespace ClodDetail
{
inline bool BoundsValid(const clodBounds& b)
{
    return std::isfinite(b.center[0]) && std::isfinite(b.center[1]) && std::isfinite(b.center[2]) &&
        std::isfinite(b.radius) && b.radius>=0 && std::isfinite(b.error) && b.error>=0;
}
struct Capture
{
    ClodBake* output;
    const ClodBakeLimits* limits;
    uint32_t calls=0; uint64_t indexCount=0;
    ClodBakeStatus failure=ClodBakeStatus::Baked;
    static int Emit(void* context, clodGroup group, const clodCluster* clusters, size_t count)
    {
        auto& c=*static_cast<Capture*>(context); const uint32_t id=c.calls++;
        // A callback return is an ID, NOT a cancellation signal. After refusal,
        // stop copying output, let the small bounded-input upstream call unwind
        // normally, then refuse the entire transaction. No partial bake escapes.
        if (c.failure!=ClodBakeStatus::Baked) return int(id);
        if (id>=c.limits->groups || count>c.limits->clusters-c.output->clusters.size())
        { c.failure=ClodBakeStatus::Capacity; return int(id); }
        if (!count || !clusters || group.depth<0 || group.depth>32 || !BoundsValid(group.simplified))
        { c.failure=ClodBakeStatus::Invalid; return int(id); }
        try
        {
            uint64_t additional=sizeof(ClodBakeGroup)+count*sizeof(ClodBakeCluster);
            for (size_t i=0;i<count;++i)
            {
                const auto& cluster=clusters[i];
                if (!cluster.indices || !cluster.index_count || cluster.index_count%3 || cluster.index_count>128*3 ||
                    !cluster.vertex_count || cluster.vertex_count>64 || cluster.refined < -1 || cluster.refined>=int(id) || !BoundsValid(cluster.bounds))
                { c.failure=ClodBakeStatus::Invalid; return int(id); }
                if (cluster.index_count>c.limits->indexEntries-c.indexCount)
                { c.failure=ClodBakeStatus::Capacity; return int(id); }
                c.indexCount+=cluster.index_count; additional+=cluster.index_count*sizeof(uint32_t);
                for (size_t k=0;k<cluster.index_count;++k)
                    if (cluster.indices[k]>=c.output->original.vertices.size())
                    { c.failure=ClodBakeStatus::Invalid; return int(id); }
                std::array<uint32_t,64> unique{}; size_t uniqueCount=0;
                for (size_t k=0;k<cluster.index_count;++k)
                    if (std::find(unique.begin(),unique.begin()+uniqueCount,cluster.indices[k])==unique.begin()+uniqueCount)
                    {
                        if (uniqueCount==unique.size()) { c.failure=ClodBakeStatus::Invalid; return int(id); }
                        unique[uniqueCount++]=cluster.indices[k];
                    }
                if (uniqueCount!=cluster.vertex_count) { c.failure=ClodBakeStatus::Invalid; return int(id); }
            }
            if (additional>c.limits->storedBytes-c.output->storedBytes)
            { c.failure=ClodBakeStatus::Capacity; return int(id); }
            ClodBakeGroup stored{group.depth,group.simplified,uint32_t(c.output->clusters.size()),uint32_t(count)};
            for (size_t i=0;i<count;++i)
            {
                const auto& cluster=clusters[i]; ClodBakeCluster copy;
                copy.group=id; copy.refined=cluster.refined; copy.bounds=cluster.bounds;
                copy.indices.assign(cluster.indices,cluster.indices+cluster.index_count);
                c.output->clusters.push_back(std::move(copy));
            }
            c.output->groups.push_back(stored); c.output->storedBytes+=additional;
        }
        catch (const std::bad_alloc&) { c.failure=ClodBakeStatus::AllocationFailed; }
        return int(id);
    }
};
inline std::array<uint32_t,3> TriangleKey(uint32_t a,uint32_t b,uint32_t c)
{
    std::array<uint32_t,3> first{a,b,c},second{b,c,a},third{c,a,b};
    return std::min(first,std::min(second,third)); // preserve winding, allow cyclic rotation
}
inline bool OriginalCoverage(const ClodBake& bake)
{
    std::vector<std::array<uint32_t,3>> source,leaves;
    for (size_t i=0;i<bake.original.indices.size();i+=3)
        source.push_back(TriangleKey(bake.original.indices[i],bake.original.indices[i+1],bake.original.indices[i+2]));
    for (const auto& cluster:bake.clusters) if (cluster.refined==-1)
        for (size_t i=0;i<cluster.indices.size();i+=3)
            leaves.push_back(TriangleKey(cluster.indices[i],cluster.indices[i+1],cluster.indices[i+2]));
    std::sort(source.begin(),source.end()); std::sort(leaves.begin(),leaves.end()); return source==leaves;
}
}
inline ClodBakeStatus BakeClodPilot(const ShapeExport& dto, bool controlledRigidSingleMaterial,
                                  ClodBake& destination, ClodBakeLimits limits={})
{
    if (!controlledRigidSingleMaterial) return ClodBakeStatus::Unsupported;
    if (!limits.vertices || limits.vertices>4096 || !limits.triangles || limits.triangles>8192 ||
        !limits.groups || limits.groups>4096 || !limits.clusters || limits.clusters>16384 ||
        !limits.indexEntries || limits.indexEntries>1024*1024 || !limits.storedBytes || limits.storedBytes>8ull*1024*1024)
        return ClodBakeStatus::Invalid;
    const auto& original=dto.fine; const auto input=original.Input();
    if (original.vertices.size()>limits.vertices || original.materials.size()>limits.triangles) return ClodBakeStatus::Capacity;
    Limits validation; validation.maxVertices=limits.vertices; validation.maxTriangles=limits.triangles;
    if (!Detail::Valid(input,validation) || original.vertices.size()!=original.positions.size()) return ClodBakeStatus::Invalid;
    const uint32_t material=original.materials.front();
    for (uint32_t m:original.materials) if (m!=material) return ClodBakeStatus::Unsupported;
    bool hash=false; for (uint8_t b:dto.source.sourceSha256) hash|=b!=0;
    if (!hash || !dto.source.vertexLayout || !dto.source.materialMapping || !dto.source.producerVersion ||
        dto.source.coarseRepresentation==dto.source.fineRepresentation)
        return ClodBakeStatus::Invalid;
    try
    {
        ClodBake result; result.source=dto.source;
        const uint64_t bytes=original.vertices.size()*(sizeof(SVertex)+sizeof(Position))+
            original.indices.size()*4+original.materials.size()*4;
        if (bytes>limits.storedBytes) return ClodBakeStatus::Capacity;
        std::vector<std::array<float,3>> positions(original.vertices.size());
        std::vector<std::array<float,7>> attributes(original.vertices.size());
        std::vector<unsigned char> locks(original.vertices.size(),0);
        for (size_t i=0;i<original.vertices.size();++i)
        {
            const auto& v=original.vertices[i]; const auto supplied=original.positions[i];
            positions[i]={v.pos.X(),v.pos.Y(),v.pos.Z()};
            if (positions[i]!=std::array<float,3>{supplied.x,supplied.y,supplied.z} || v.conform)
                return ClodBakeStatus::Invalid;
            // Keep the controlled pilot away from float-overflow extremes inside
            // the external simplifier; this is an explicit supported domain cap.
            for (float p:positions[i]) if (std::abs(p)>1000000.f) return ClodBakeStatus::Invalid;
            attributes[i]={v.norm.X(),v.norm.Y(),v.norm.Z(),v.t0.u,v.t0.v,v.t1.u,v.t1.v};
            for (float a:attributes[i]) if (!std::isfinite(a) || std::abs(a)>1000000.f) return ClodBakeStatus::Invalid;
        }
        // Preserve every coincident original vertex ID (including UV/normal seams).
        // Bounded4096^2 offline scan; no owner-frame or world inventory integration.
        for (size_t i=0;i<positions.size();++i) for (size_t j=0;j<i;++j)
            if (positions[i]==positions[j]) locks[i]=locks[j]=meshopt_SimplifyVertex_Lock;
        result.original=original; result.storedBytes=bytes;
        clodConfig config=clodDefaultConfig(128);
        config.max_vertices=64; config.min_triangles=32; config.max_triangles=128;
        config.simplify_permissive=false; config.simplify_fallback_permissive=false;
        config.simplify_fallback_sloppy=false; config.simplify_dilate_borders=false;
        config.simplify_regularize=false; config.simplify_error_edge_limit=0;
        config.simplify_error_clamped=false;
        config.simplify_error_merge_previous=1; config.simplify_error_merge_additive=1;
        const std::array<float,7> weights{1,1,1,1,1,1,1};
        clodMesh mesh{}; mesh.indices=original.indices.data(); mesh.index_count=original.indices.size();
        mesh.vertex_count=positions.size(); mesh.vertex_positions=positions.front().data();
        mesh.vertex_positions_stride=sizeof(positions.front()); mesh.vertex_attributes=attributes.front().data();
        mesh.vertex_attributes_stride=sizeof(attributes.front()); mesh.vertex_lock=locks.data();
        mesh.attribute_weights=weights.data(); mesh.attribute_count=weights.size(); mesh.attribute_protect_mask=0;
        ClodDetail::Capture capture{&result,&limits};
        clodBuild(config,mesh,&capture,&ClodDetail::Capture::Emit);
        if (capture.failure!=ClodBakeStatus::Baked) return capture.failure;
        if (result.groups.empty() || !ClodDetail::OriginalCoverage(result)) return ClodBakeStatus::Invalid;
        // Every nonterminal group must have a later replacement cluster; terminal
        // groups must remain roots at every finite threshold. Refined IDs are DAG
        // references, not a tree parent or cluster.bounds.error selection rule.
        std::vector<uint32_t> parents(result.groups.size(),0);
        for (const auto& c:result.clusters) if (c.refined>=0)
        {
            if (result.groups[c.group].simplified.error < result.groups[c.refined].simplified.error)
                return ClodBakeStatus::Invalid;
            ++parents[c.refined];
        }
        for (size_t i=0;i<parents.size();++i)
            if (!parents[i] && result.groups[i].simplified.error!=FLT_MAX) return ClodBakeStatus::Invalid;
        destination=std::move(result); return ClodBakeStatus::Baked;
    }
    catch (const std::bad_alloc&) { return ClodBakeStatus::AllocationFailed; }
}
struct ClodCut { std::vector<uint32_t> clusters; };
// Full resident reference scan. Object-space threshold; projection/per-pass demand
// is NOT implemented. Pair the containing group's simplified error with refined
// group error, exactly; cluster.bounds.error is deliberately not used for cuts.
// Precondition: an immutable successful BakeClodPilot result. The checks below
// validate selection topology, not mutated index payloads or original coverage;
// this is not a parser/validator for arbitrary externally constructed geometry.
inline bool SelectClodCut(const ClodBake& bake, float threshold, ClodCut& destination)
{
    if (!std::isfinite(threshold) || threshold<0 || threshold==FLT_MAX || bake.groups.empty() ||
        bake.groups.size()>4096 || bake.clusters.size()>16384) return false;
    try
    {
        ClodCut result;
        std::vector<uint32_t> parents(bake.groups.size(),0);
        size_t next=0;
        uint32_t groupIndex=0;
        for (const auto& group:bake.groups)
        {
            if (!ClodDetail::BoundsValid(group.simplified) || group.first!=next || !group.count ||
                group.count>bake.clusters.size()-next) return false;
            for (size_t i=next;i<next+group.count;++i)
                if (bake.clusters[i].group!=groupIndex) return false;
            next+=group.count; ++groupIndex;
        }
        if (next!=bake.clusters.size()) return false;
        for (uint32_t i=0;i<bake.clusters.size();++i)
        {
            const auto& c=bake.clusters[i];
            if (c.group>=bake.groups.size() || c.refined < -1 || c.refined>=int(c.group)) return false;
            if (c.refined>=0 && bake.groups[c.group].simplified.error<bake.groups[c.refined].simplified.error) return false;
            if (c.refined>=0) ++parents[c.refined];
            if (bake.groups[c.group].simplified.error>threshold &&
                (c.refined==-1 || bake.groups[c.refined].simplified.error<=threshold)) result.clusters.push_back(i);
        }
        for (size_t i=0;i<parents.size();++i)
            if (!parents[i] && bake.groups[i].simplified.error!=FLT_MAX) return false;
        if (result.clusters.empty()) return false;
        destination=std::move(result); return true;
    }
    catch (const std::bad_alloc&) { return false; }
}
}
