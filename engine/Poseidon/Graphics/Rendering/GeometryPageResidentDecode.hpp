#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageShapeExport.hpp>

namespace Poseidon::GeometryPages
{
struct ResidentCluster
{
    uint32_t material=0, firstTriangle=0;
    std::vector<SVertex> vertices;
    std::vector<uint32_t> indices;
};
struct ResidentPage
{
    CacheIdentity identity;
    uint32_t page=0;
    std::vector<ResidentCluster> clusters;
};
enum class DecodeStatus { Decoded, Invalid, Capacity, AllocationFailed, Cancelled };
// Explicit resident pilot only: original is the immutable value DTO from the
// controlled owner export. Its source witness is a caller fact, not a retail-file
// freshness certificate. No GPU calls, disk parser, streaming or mesh ownership.
// Serialized controls are LE; opaque SVertex bytes are exact same-process ABI.
// No misaligned input is ever treated as a struct pointer.
inline DecodeStatus DecodeResidentPage(const Package& package, const CacheIdentity& identity,
    uint32_t pageIndex, const ShapeExport& original, ResidentPage& destination)
{
    if (!(package.Identity()==identity) || !(original.source==identity.source) ||
        identity.formatVersion!=1 || identity.algorithmVersion!=2 ||
        !identity.packing.clusterVertices || identity.packing.clusterVertices>256 ||
        !identity.packing.clusterTriangles || identity.packing.clusterTriangles>256 ||
        pageIndex>=package.pages.size() || !package.coarsePages ||
        package.coarsePages>=package.pages.size()) return DecodeStatus::Invalid;
    if (package.pages.size()>4096 || package.clusters.size()>65536) return DecodeStatus::Capacity;
    const auto& bytes=package.pages[pageIndex].bytes;
    if (bytes.size()>1024u*1024u || bytes.size()>identity.packing.pageBytes) return DecodeStatus::Capacity;
    if (bytes.size()<16) return DecodeStatus::Invalid;
    auto u32=[&](size_t at) { return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|
        uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24; };
    auto f32=[&](size_t at) { return std::bit_cast<float>(u32(at)); };
    if (u32(0)!=0x31504743 || u32(4)!=1 || u32(12)!=bytes.size()) return DecodeStatus::Invalid;
    const size_t count=u32(8);
    if (!count || count>256) return DecodeStatus::Capacity;
    const auto& source=pageIndex<package.coarsePages?original.coarse:original.fine;
    if (source.vertices.size()!=source.positions.size() || source.indices.size()%3 ||
        source.materials.size()!=source.indices.size()/3 || source.vertices.size()>65536 ||
        source.materials.size()>131072) return DecodeStatus::Invalid;
    try
    {
        ResidentPage result; result.identity=identity; result.page=pageIndex;
        result.clusters.reserve(count);
        size_t at=16, descriptorAt=0;
        for (size_t c=0; c<count; ++c)
        {
            while (descriptorAt<package.clusters.size() && package.clusters[descriptorAt].page!=pageIndex) ++descriptorAt;
            if (descriptorAt==package.clusters.size() || bytes.size()-at<48) return DecodeStatus::Invalid;
            const auto& descriptor=package.clusters[descriptorAt++];
            const uint32_t material=u32(at), first=u32(at+4), triangles=u32(at+8), stride=u32(at+12),
                vertices=u32(at+16), indices=u32(at+20);
            if (!vertices || vertices>identity.packing.clusterVertices || !triangles ||
                triangles>identity.packing.clusterTriangles || stride!=sizeof(SVertex) || indices!=triangles*3 ||
                first>source.materials.size() || triangles>source.materials.size()-first) return DecodeStatus::Invalid;
            const size_t length=48+size_t(vertices)*(4+sizeof(SVertex))+size_t(indices)*4;
            if (length>bytes.size()-at || descriptor.byteOffset!=at || descriptor.byteLength!=length ||
                descriptor.material!=material || descriptor.firstTriangle!=first || descriptor.triangles!=triangles)
                return DecodeStatus::Invalid;
            for (size_t t=first; t<first+triangles; ++t)
                if (source.materials[t]!=material) return DecodeStatus::Invalid;
            std::array<uint32_t,256> ids{};
            const size_t idAt=at+48, vertexAt=idAt+vertices*4, indexAt=vertexAt+vertices*sizeof(SVertex);
            ResidentCluster cluster; cluster.material=material; cluster.firstTriangle=first;
            cluster.vertices.resize(vertices); cluster.indices.resize(indices);
            std::array<float,3> minimum{},maximum{};
            for (uint32_t v=0; v<vertices; ++v)
            {
                ids[v]=u32(idAt+v*4);
                if (ids[v]>=source.vertices.size()) return DecodeStatus::Invalid;
                for (uint32_t previous=0; previous<v; ++previous) if (ids[previous]==ids[v]) return DecodeStatus::Invalid;
                if (std::memcmp(bytes.data()+vertexAt+v*sizeof(SVertex),&source.vertices[ids[v]],sizeof(SVertex)))
                    return DecodeStatus::Invalid;
                // The byte witness already matched this live typed source. Use its copy operators;
                // Vector3P makes SVertex non-trivially-copyable.
                cluster.vertices[v]=source.vertices[ids[v]];
                const auto& p=cluster.vertices[v].pos;
                const std::array<float,3> value{p.X(),p.Y(),p.Z()};
                const auto supplied=source.positions[ids[v]];
                if (value!=std::array<float,3>{supplied.x,supplied.y,supplied.z}) return DecodeStatus::Invalid;
                for (unsigned axis=0; axis<3; ++axis)
                {
                    if (!std::isfinite(value[axis])) return DecodeStatus::Invalid;
                    if (!v) minimum[axis]=maximum[axis]=value[axis];
                    else { minimum[axis]=std::min(minimum[axis],value[axis]); maximum[axis]=std::max(maximum[axis],value[axis]); }
                }
            }
            for (unsigned axis=0; axis<3; ++axis)
            {
                const float lo=std::nextafter(minimum[axis],-std::numeric_limits<float>::infinity()),
                    hi=std::nextafter(maximum[axis],std::numeric_limits<float>::infinity());
                if (!std::isfinite(lo)||!std::isfinite(hi)||f32(at+24+axis*4)!=lo||f32(at+36+axis*4)!=hi||
                    descriptor.minimum[axis]!=lo||descriptor.maximum[axis]!=hi) return DecodeStatus::Invalid;
            }
            for (uint32_t i=0; i<indices; ++i)
            {
                const uint32_t local=u32(indexAt+i*4);
                if (local>=vertices || ids[local]!=source.indices[size_t(first)*3+i]) return DecodeStatus::Invalid;
                cluster.indices[i]=local;
            }
            result.clusters.push_back(std::move(cluster)); at+=length;
        }
        while (descriptorAt<package.clusters.size())
            if (package.clusters[descriptorAt++].page==pageIndex) return DecodeStatus::Invalid;
        if (at!=bytes.size()) return DecodeStatus::Invalid;
        destination=std::move(result); return DecodeStatus::Decoded;
    }
    catch (const std::bad_alloc&) { return DecodeStatus::AllocationFailed; }
}
}
