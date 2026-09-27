#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageResidentDecode.hpp>
#include <atomic>

namespace Poseidon::GeometryPages
{
struct RepresentationDecodeLimits
{
    static constexpr uint32_t HardPages=4096, HardClusters=65536;
    static constexpr uint32_t HardVertexRecords=393216, HardIndices=393216;
    static constexpr uint64_t HardDecodedBytes=64ull*1024*1024;
    static constexpr uint64_t HardSerializedBytes=128ull*1024*1024;
    uint32_t pages=HardPages, clusters=HardClusters;
    uint32_t vertexRecords=HardVertexRecords, indices=HardIndices;
    uint64_t decodedBytes=HardDecodedBytes, serializedBytes=HardSerializedBytes;
};
struct ResidentRepresentation
{
    CacheIdentity identity;
    Frontier representation=Frontier::Unavailable;
    std::vector<ResidentPage> pages;
    uint64_t vertexRecords=0, indices=0, clusters=0;
    // Exact value payload only; vector capacities/metadata/allocator/RSS excluded.
    uint64_t decodedBytes=0, serializedBytes=0;
};

// Explicit synchronous value-only bridge. Package and original export remain
// immutable for this entire operation; no engine pointers, owner privileges,
// disk/renderer calls or residency change. Success certifies all original
// triangles of ONE authored representation, not a simplification/error bound.
inline DecodeStatus DecodeResidentRepresentation(const Package& package,
    const CacheIdentity& identity, Frontier representation, const ShapeExport& original,
    ResidentRepresentation& destination, RepresentationDecodeLimits limits={}, const std::atomic<bool>* cancelled=nullptr)
{
    if(cancelled && cancelled->load()) return DecodeStatus::Cancelled;
    using L=RepresentationDecodeLimits;
    if (!(package.Identity()==identity) || !(original.source==identity.source) ||
        (representation!=Frontier::Coarse && representation!=Frontier::Fine) ||
        identity.formatVersion!=1 || identity.algorithmVersion!=2 ||
        identity.packing.clusterVertices<3 || identity.packing.clusterVertices>256 ||
        !identity.packing.clusterTriangles || identity.packing.clusterTriangles>256 ||
        identity.packing.pageBytes<64 || identity.packing.pageBytes>1024*1024 ||
        !package.coarsePages || package.coarsePages>=package.pages.size() ||
        !package.coarseClusters || package.coarseClusters>=package.clusters.size())
        return DecodeStatus::Invalid;
    if (limits.pages>L::HardPages || limits.clusters>L::HardClusters ||
        limits.vertexRecords>L::HardVertexRecords || limits.indices>L::HardIndices ||
        limits.decodedBytes>L::HardDecodedBytes || limits.serializedBytes>L::HardSerializedBytes ||
        package.pages.size()>L::HardPages || package.clusters.size()>L::HardClusters)
        return DecodeStatus::Capacity;
    const size_t pageBegin=representation==Frontier::Coarse?0:package.coarsePages;
    const size_t pageEnd=representation==Frontier::Coarse?package.coarsePages:package.pages.size();
    const size_t clusterBegin=representation==Frontier::Coarse?0:package.coarseClusters;
    const size_t clusterEnd=representation==Frontier::Coarse?package.coarseClusters:package.clusters.size();
    if (pageEnd-pageBegin>limits.pages || clusterEnd-clusterBegin>limits.clusters)
        return DecodeStatus::Capacity;
    const auto& source=representation==Frontier::Coarse?original.coarse:original.fine;
    if (source.vertices.empty() || source.vertices.size()>65536 ||
        source.vertices.size()!=source.positions.size() || source.indices.empty() ||
        source.indices.size()%3 || source.materials.size()!=source.indices.size()/3 ||
        source.materials.size()>131072) return DecodeStatus::Invalid;
    // Partition metadata itself must not move a coarse descriptor into fine,
    // hide an extra descriptor, or reorder pages across the chosen boundary.
    for (size_t c=0;c<package.clusters.size();++c)
    {
        const auto page=package.clusters[c].page;
        if (page>=package.pages.size() ||
            (c<package.coarseClusters)!=(page<package.coarsePages) ||
            (c && page<package.clusters[c-1].page)) return DecodeStatus::Invalid;
    }
    uint64_t vertices=0, indices=0, serialized=0, decoded=0;
    size_t descriptor=clusterBegin, triangle=0;
    const auto add=[](uint64_t& total,uint64_t amount,uint64_t cap)
    { if (total>cap || amount>cap-total) return false; total+=amount; return true; };
    // Preflight every selected page before allocating any output. The existing
    // page decoder then checks every original attribute, index and bound again.
    for (size_t page=pageBegin;page<pageEnd;++page)
    {
        if(cancelled && cancelled->load()) return DecodeStatus::Cancelled;
        const auto& bytes=package.pages[page].bytes;
        if (bytes.size()>identity.packing.pageBytes || bytes.size()>1024u*1024u ||
            !add(serialized,bytes.size(),limits.serializedBytes)) return DecodeStatus::Capacity;
        if (bytes.size()<16) return DecodeStatus::Invalid;
        const auto u32=[&](size_t at) { return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|
            uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24; };
        if (u32(0)!=0x31504743 || u32(4)!=1 || u32(12)!=bytes.size()) return DecodeStatus::Invalid;
        const uint32_t count=u32(8);
        if (!count || count>256) return DecodeStatus::Invalid;
        size_t at=16;
        for (uint32_t c=0;c<count;++c)
        {
            if (descriptor>=clusterEnd || package.clusters[descriptor].page!=page ||
                bytes.size()-at<48) return DecodeStatus::Invalid;
            const auto& d=package.clusters[descriptor++];
            const uint32_t material=u32(at), first=u32(at+4), triangles=u32(at+8),
                stride=u32(at+12), v=u32(at+16), i=u32(at+20);
            if (!v || v>identity.packing.clusterVertices || !triangles ||
                triangles>identity.packing.clusterTriangles || stride!=sizeof(SVertex) ||
                i!=triangles*3 || first!=triangle || triangle>source.materials.size() ||
                triangles>source.materials.size()-triangle) return DecodeStatus::Invalid;
            const uint64_t length=48ull+uint64_t(v)*(4+sizeof(SVertex))+uint64_t(i)*4;
            if (length>bytes.size()-at || d.byteOffset!=at || d.byteLength!=length ||
                d.material!=material || d.firstTriangle!=first || d.triangles!=triangles)
                return DecodeStatus::Invalid;
            for (size_t t=triangle;t<triangle+triangles;++t)
                if (source.materials[t]!=material) return DecodeStatus::Invalid;
            if (!add(vertices,v,limits.vertexRecords) || !add(indices,i,limits.indices) ||
                !add(decoded,uint64_t(v)*sizeof(SVertex)+uint64_t(i)*4,limits.decodedBytes))
                return DecodeStatus::Capacity;
            triangle+=triangles; at+=size_t(length);
        }
        if (at!=bytes.size()) return DecodeStatus::Invalid;
    }
    if (descriptor!=clusterEnd || triangle!=source.materials.size() || indices!=source.indices.size())
        return DecodeStatus::Invalid;
    try
    {
        ResidentRepresentation result; result.identity=identity; result.representation=representation;
        result.vertexRecords=vertices; result.indices=indices; result.clusters=clusterEnd-clusterBegin;
        result.decodedBytes=decoded; result.serializedBytes=serialized;
        result.pages.reserve(pageEnd-pageBegin);
        for (size_t page=pageBegin;page<pageEnd;++page)
        {
            if(cancelled && cancelled->load()) return DecodeStatus::Cancelled;
            ResidentPage value;
            const auto status=DecodeResidentPage(package,identity,uint32_t(page),original,value);
            if (status!=DecodeStatus::Decoded) return status;
            if(cancelled && cancelled->load()) return DecodeStatus::Cancelled;
            result.pages.push_back(std::move(value));
        }
        if(cancelled && cancelled->load()) return DecodeStatus::Cancelled;
        destination=std::move(result); return DecodeStatus::Decoded;
    }
    catch (const std::bad_alloc&) { return DecodeStatus::AllocationFailed; }
}
}
