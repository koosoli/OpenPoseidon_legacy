#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageClodBake.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageResidentDecode.hpp>

namespace Poseidon::GeometryPages
{
struct ClodCutDecodeLimits
{
    uint32_t clusters=4096, vertexRecords=65536, indexEntries=262144;
    uint64_t knownPayloadBytes=8ull*1024*1024;
};
struct ClodDecodedCluster
{
    uint32_t bakedCluster=0;
    ResidentCluster mesh;
    std::vector<uint32_t> originalVertexIds;
    std::array<float,3> minimum{},maximum{};
};
struct ClodDecodedCut
{
    SourceIdentity source;
    uint32_t adapterVersion=ClodBake::AdapterVersion;
    float threshold=0;
    std::vector<ClodDecodedCluster> clusters;
    uint64_t knownPayloadBytes=0;
};
enum class ClodCutDecodeStatus { Decoded, Invalid, Capacity, AllocationFailed };
// Explicit resident-only controlled pilot. Bake must be an immutable successful
// BakeClodPilot result. The supplied cut must exactly match a fresh full paired
// selection at threshold. No disk parsing, GPU calls, frame jobs or coarse/root
// residency management; the caller retains a complete fallback externally.
// Simplified triangles are intentionally NOT required to equal original triangles.
inline ClodCutDecodeStatus DecodeClodCut(const ClodBake& bake, float threshold,
    const ClodCut& selected, ClodDecodedCut& destination, ClodCutDecodeLimits limits={})
{
    if (!limits.clusters || limits.clusters>4096 || !limits.vertexRecords || limits.vertexRecords>65536 ||
        !limits.indexEntries || limits.indexEntries>262144 || !limits.knownPayloadBytes ||
        limits.knownPayloadBytes>8ull*1024*1024) return ClodCutDecodeStatus::Invalid;
    const auto& source=bake.original;
    if (source.vertices.empty() || source.vertices.size()>4096 || source.positions.size()!=source.vertices.size() ||
        source.indices.empty() || source.indices.size()>8192*3 || source.indices.size()%3 ||
        source.materials.size()!=source.indices.size()/3 || selected.clusters.size()>16384)
        return ClodCutDecodeStatus::Invalid;
    const uint32_t material=source.materials.front();
    for (auto m:source.materials) if (m!=material) return ClodCutDecodeStatus::Invalid;
    try
    {
        ClodCut expected;
        if (!SelectClodCut(bake,threshold,expected) || expected.clusters!=selected.clusters)
            return ClodCutDecodeStatus::Invalid;
        if (selected.clusters.size()>limits.clusters) return ClodCutDecodeStatus::Capacity;
        uint64_t payload=sizeof(ClodDecodedCut); uint32_t records=0,entries=0;
        // Bounded preflight before output allocations. Stack remap preserves seam
        // IDs: coincident positions/attributes never cause IDs to be welded.
        for (uint32_t id:selected.clusters)
        {
            if (id>=bake.clusters.size()) return ClodCutDecodeStatus::Invalid;
            const auto& indices=bake.clusters[id].indices;
            if (indices.empty() || indices.size()%3 || indices.size()>128*3) return ClodCutDecodeStatus::Invalid;
            if (indices.size()>limits.indexEntries-entries) return ClodCutDecodeStatus::Capacity;
            entries+=uint32_t(indices.size());
            std::array<uint32_t,64> ids{}; uint32_t count=0;
            for (uint32_t original:indices)
            {
                if (original>=source.vertices.size()) return ClodCutDecodeStatus::Invalid;
                if (std::find(ids.begin(),ids.begin()+count,original)==ids.begin()+count)
                {
                    if (count==ids.size()) return ClodCutDecodeStatus::Invalid;
                    ids[count++]=original;
                    const auto& p=source.vertices[original].pos;
                    const auto supplied=source.positions[original];
                    if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z()) ||
                        p.X()!=supplied.x || p.Y()!=supplied.y || p.Z()!=supplied.z) return ClodCutDecodeStatus::Invalid;
                }
            }
            if (count>limits.vertexRecords-records) return ClodCutDecodeStatus::Capacity;
            records+=count;
            const uint64_t bytes=sizeof(ClodDecodedCluster)+uint64_t(count)*(sizeof(SVertex)+sizeof(uint32_t))+
                indices.size()*sizeof(uint32_t);
            if (payload>limits.knownPayloadBytes || bytes>limits.knownPayloadBytes-payload)
                return ClodCutDecodeStatus::Capacity;
            payload+=bytes;
        }
        ClodDecodedCut result; result.source=bake.source; result.threshold=threshold;
        result.knownPayloadBytes=payload; result.clusters.reserve(selected.clusters.size());
        for (uint32_t id:selected.clusters)
        {
            ClodDecodedCluster cluster; cluster.bakedCluster=id; cluster.mesh.material=material;
            // firstTriangle has no original contiguous-range meaning for CLOD.
            cluster.mesh.firstTriangle=0;
            const auto& indices=bake.clusters[id].indices;
            cluster.mesh.indices.reserve(indices.size());
            for (uint32_t original:indices)
            {
                auto found=std::find(cluster.originalVertexIds.begin(),cluster.originalVertexIds.end(),original);
                uint32_t local=uint32_t(found-cluster.originalVertexIds.begin());
                if (found==cluster.originalVertexIds.end())
                {
                    cluster.originalVertexIds.push_back(original); cluster.mesh.vertices.push_back(source.vertices[original]);
                }
                cluster.mesh.indices.push_back(local);
            }
            for (size_t v=0;v<cluster.mesh.vertices.size();++v)
            {
                const auto& p=cluster.mesh.vertices[v].pos;
                const std::array<float,3> value{p.X(),p.Y(),p.Z()};
                for (unsigned axis=0;axis<3;++axis)
                    if (!v) cluster.minimum[axis]=cluster.maximum[axis]=value[axis];
                    else { cluster.minimum[axis]=std::min(cluster.minimum[axis],value[axis]);
                           cluster.maximum[axis]=std::max(cluster.maximum[axis],value[axis]); }
            }
            for (unsigned axis=0;axis<3;++axis)
            {
                cluster.minimum[axis]=std::nextafter(cluster.minimum[axis],-std::numeric_limits<float>::infinity());
                cluster.maximum[axis]=std::nextafter(cluster.maximum[axis],std::numeric_limits<float>::infinity());
                if (!std::isfinite(cluster.minimum[axis]) || !std::isfinite(cluster.maximum[axis]))
                    return ClodCutDecodeStatus::Invalid;
            }
            result.clusters.push_back(std::move(cluster));
        }
        // Logical payload includes output value metadata and element contents,
        // not vector capacity/allocator metadata or temporary paired-cut scratch.
        destination=std::move(result); return ClodCutDecodeStatus::Decoded;
    }
    catch (const std::bad_alloc&) { return ClodCutDecodeStatus::AllocationFailed; }
}
}
