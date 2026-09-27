#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageClodCutDecode.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRepresentationDecode.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <cstring>

namespace Poseidon::GeometryPages
{
struct ClodRamLimits
{
    uint32_t pages=8, clusters=64, vertexRecordsPerCut=1024, indicesPerCut=4096;
    uint64_t knownSourceBytes=128*1024;
};
struct ClodRamPackage
{
    static constexpr uint32_t AdapterVersion=1;
    SourceIdentity originalSource;
    float coarseThreshold=0, fineThreshold=0;
    std::vector<uint32_t> coarseClusters, fineClusters;
    Package package;
    // Reference for page decoding is the complete SELECTED CUT geometry,
    // not an authored/original unsimplified fine representation.
    ShapeExport selectedGeometry;
    uint64_t knownCapacityBytes=0;
};
enum class ClodRamStatus { Built, Invalid, Capacity, AllocationFailed };
inline uint64_t ClodRamKnownBytes(const ClodRamPackage& value)
{
    uint64_t bytes=sizeof(value)+(value.coarseClusters.capacity()+value.fineClusters.capacity())*sizeof(uint32_t)+
        value.package.pages.capacity()*sizeof(Page)+value.package.clusters.capacity()*sizeof(Cluster);
    for(const auto& page:value.package.pages) bytes+=page.bytes.capacity();
    for(const auto* mesh:{&value.selectedGeometry.coarse,&value.selectedGeometry.fine})
        bytes+=mesh->positions.capacity()*sizeof(Position)+mesh->vertices.capacity()*sizeof(SVertex)+
            (mesh->indices.capacity()+mesh->materials.capacity())*sizeof(uint32_t);
    return bytes; // Known C++ capacity only, excluding allocator/library scratch/RSS.
}

// Controlled immutable successful BakeClodPilot + DecodeClodCut snapshots only.
// No bake, worker, renderer, source-file freshness or generalized DAG residency.
// Validate exact paired selections and their decoded bytes before publishing.
// The entire result/reference/package must remain immutable while borrowed.
inline ClodRamStatus BuildClodRamPackage(const ClodBake& bake,
    float coarseThreshold,float fineThreshold,const ClodDecodedCut& coarse,
    const ClodDecodedCut& fine,ClodRamPackage& destination,ClodRamLimits limits={})
{
    if(!limits.pages || limits.pages>8 || !limits.clusters || limits.clusters>64 ||
       !limits.vertexRecordsPerCut || limits.vertexRecordsPerCut>1024 ||
       !limits.indicesPerCut || limits.indicesPerCut>4096 ||
       !limits.knownSourceBytes || limits.knownSourceBytes>128*1024 ||
       !std::isfinite(coarseThreshold) || !std::isfinite(fineThreshold) ||
       coarseThreshold<=fineThreshold || fineThreshold<0 || coarseThreshold==FLT_MAX ||
       !(coarse.source==bake.source) || !(fine.source==bake.source) ||
       coarse.adapterVersion!=ClodBake::AdapterVersion || fine.adapterVersion!=ClodBake::AdapterVersion ||
       coarse.threshold!=coarseThreshold || fine.threshold!=fineThreshold ||
       bake.original.vertices.empty() || bake.original.vertices.size()>4096 ||
       bake.original.positions.size()!=bake.original.vertices.size() ||
       bake.original.indices.empty() || bake.original.indices.size()>8192*3 ||
       bake.original.indices.size()%3 || bake.original.materials.size()!=bake.original.indices.size()/3 ||
       bake.clusters.size()>16384 || bake.groups.size()>4096)
        return ClodRamStatus::Invalid;
    const auto material=bake.original.materials.front();
    for(auto m:bake.original.materials) if(m!=material) return ClodRamStatus::Invalid;
    if(coarse.clusters.empty() || fine.clusters.empty()) return ClodRamStatus::Invalid;
    if(coarse.clusters.size()>limits.clusters || fine.clusters.size()>limits.clusters-coarse.clusters.size())
        return ClodRamStatus::Capacity;
    try {
        ClodCut selectedCoarse,selectedFine;
        if(!SelectClodCut(bake,coarseThreshold,selectedCoarse) || !SelectClodCut(bake,fineThreshold,selectedFine) ||
           selectedCoarse.clusters==selectedFine.clusters) return ClodRamStatus::Invalid;
        ClodRamPackage result;result.originalSource=bake.source;
        result.coarseThreshold=coarseThreshold;result.fineThreshold=fineThreshold;
        auto flatten=[&](const ClodDecodedCut& cut,const ClodCut& expected,ExportedMesh& mesh)->ClodRamStatus {
            if(cut.clusters.size()!=expected.clusters.size()) return ClodRamStatus::Invalid;
            size_t vertices=0,indices=0;
            for(size_t k=0;k<cut.clusters.size();++k) {
                const auto& c=cut.clusters[k];
                if(c.bakedCluster!=expected.clusters[k] || c.bakedCluster>=bake.clusters.size() ||
                   c.mesh.material!=material ||
                   c.mesh.vertices.empty() || c.mesh.vertices.size()>64 ||
                   c.mesh.vertices.size()!=c.originalVertexIds.size() || c.mesh.indices.empty() ||
                   c.mesh.indices.size()%3 || c.mesh.indices.size()>128*3 ||
                   c.mesh.indices.size()!=bake.clusters[c.bakedCluster].indices.size()) return ClodRamStatus::Invalid;
                if(c.mesh.vertices.size()>limits.vertexRecordsPerCut-vertices ||
                   c.mesh.indices.size()>limits.indicesPerCut-indices) return ClodRamStatus::Capacity;
                vertices+=c.mesh.vertices.size();indices+=c.mesh.indices.size();
                for(size_t v=0;v<c.mesh.vertices.size();++v) {
                    const auto id=c.originalVertexIds[v];
                    if(id>=bake.original.vertices.size() ||
                       std::memcmp(&c.mesh.vertices[v],&bake.original.vertices[id],sizeof(SVertex)))
                        return ClodRamStatus::Invalid;
                    // Distinct seam IDs must remain distinct, even with identical positions.
                    for(size_t earlier=0;earlier<v;++earlier)
                        if(c.originalVertexIds[earlier]==id) return ClodRamStatus::Invalid;
                }
                for(size_t i=0;i<c.mesh.indices.size();++i) {
                    const auto local=c.mesh.indices[i];
                    if(local>=c.originalVertexIds.size() ||
                       c.originalVertexIds[local]!=bake.clusters[c.bakedCluster].indices[i])
                        return ClodRamStatus::Invalid;
                }
            }
            mesh.vertices.reserve(vertices);mesh.positions.reserve(vertices);
            mesh.indices.reserve(indices);mesh.materials.reserve(indices/3);
            for(const auto& c:cut.clusters) {
                const auto base=uint32_t(mesh.vertices.size());
                for(const auto& vertex:c.mesh.vertices) {
                    mesh.vertices.push_back(vertex);
                    mesh.positions.push_back({vertex.pos.X(),vertex.pos.Y(),vertex.pos.Z()});
                }
                for(uint32_t local:c.mesh.indices) mesh.indices.push_back(base+local);
                mesh.materials.insert(mesh.materials.end(),c.mesh.indices.size()/3,c.mesh.material);
            }
            return ClodRamStatus::Built;
        };
        auto status=flatten(coarse,selectedCoarse,result.selectedGeometry.coarse);
        if(status!=ClodRamStatus::Built) return status;
        status=flatten(fine,selectedFine,result.selectedGeometry.fine);
        if(status!=ClodRamStatus::Built) return status;
        if(result.selectedGeometry.coarse.indices.size()>=result.selectedGeometry.fine.indices.size()) return ClodRamStatus::Invalid;
        result.coarseClusters=std::move(selectedCoarse.clusters);result.fineClusters=std::move(selectedFine.clusters);
        Foundation::Sha256 hash;hash.Update(std::string("OpenPoseidon-selected-clod-ram-v1"));
        hash.Update(std::string(ClodBake::LibraryRevision));
        auto number=[&](uint64_t n,unsigned width) {uint8_t bytes[8]{};for(unsigned i=0;i<width;++i) bytes[i]=uint8_t(n>>(8*i));hash.Update(bytes,width);};
        const auto& source=bake.source;hash.Update(source.sourceSha256.data(),source.sourceSha256.size());
        number(source.geometryOptions,8);number(source.materialOptions,8);number(source.producerVersion,4);
        number(source.coarseRepresentation,4);number(source.fineRepresentation,4);
        number(source.vertexLayout,4);number(source.materialMapping,4);
        number(ClodBake::AdapterVersion,4);number(ClodRamPackage::AdapterVersion,4);
        number(std::bit_cast<uint32_t>(coarseThreshold),4);number(std::bit_cast<uint32_t>(fineThreshold),4);
        for(const auto* ids:{&result.coarseClusters,&result.fineClusters}) {number(ids->size(),4);for(auto id:*ids) number(id,4);}
        for(const auto* mesh:{&result.selectedGeometry.coarse,&result.selectedGeometry.fine}) {
            number(mesh->vertices.size(),4);hash.Update(mesh->vertices.data(),mesh->vertices.size()*sizeof(SVertex));
            number(mesh->indices.size(),4);for(auto index:mesh->indices) number(index,4);
            for(auto material:mesh->materials) number(material,4);
        }
        SourceIdentity derived=source;derived.coarseRepresentation=0;derived.fineRepresentation=1;derived.vertexLayout=sizeof(SVertex);
        const auto hex=hash.Hex();const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
        for(size_t i=0;i<32;++i) derived.sourceSha256[i]=uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1]));
        result.selectedGeometry.source=derived;
        Limits packing;packing.maxVertices=limits.vertexRecordsPerCut;packing.maxTriangles=limits.indicesPerCut/3;
        packing.maxPages=limits.pages;packing.maxClusters=limits.clusters;packing.payloadBytes=limits.knownSourceBytes;
        const auto built=Build(result.selectedGeometry.coarse.Input(),result.selectedGeometry.fine.Input(),derived,true,result.package,packing);
        if(built!=Status::Built) return built==Status::Capacity?ClodRamStatus::Capacity:
            built==Status::AllocationFailed?ClodRamStatus::AllocationFailed:ClodRamStatus::Invalid;
        result.knownCapacityBytes=ClodRamKnownBytes(result);
        if(result.knownCapacityBytes>limits.knownSourceBytes) return ClodRamStatus::Capacity;
        destination=std::move(result);return ClodRamStatus::Built;
    } catch(const std::bad_alloc&) {return ClodRamStatus::AllocationFailed;}
}
}
