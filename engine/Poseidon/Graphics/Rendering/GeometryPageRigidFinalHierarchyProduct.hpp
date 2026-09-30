#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageRigidOdol7FinalShape.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalDiskCodec.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalOriginalOutside.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalCut.hpp>

namespace Poseidon::GeometryPages
{
struct RigidFinalHierarchyLimits
{
    uint32_t groups=64,clusters=63,pages=64,pageBytes=65536;
    uint64_t sourceCapacityBytes=128*1024,packageCapacityBytes=1024*1024;
    uint64_t encodedCapacityBytes=1024*1024,observedCapacityBytes=2*1024*1024;
};
struct RigidFinalHierarchyProduct
{
    // Caller keeps the successful export/source/material holds separately.
    // These immutable products are producer output, never a live admission.
    std::shared_ptr<const HierarchicalDiskImage> image;
    std::shared_ptr<const HierarchicalDiskManifest> manifest;
    SourceIdentity expectedSource;
    std::array<uint8_t,32> fileSha256{};
    uint64_t sourceAdmissionEpoch=0,ownerEpoch=0,modelBirth=0,shapeQueryRevision=0;
    bool actualNoShadow=false,requiresOriginalOtherPasses=true;
    // Exact oriented original-ID triangle multiplicities for the complete Fine
    // cut, plus separately checked scalar vertices. No raster/world claim.
    bool fineTriangleSetExact=false;
    uint64_t knownSourceCapacityBytes=0,knownRetainedCapacityBytes=0;
    // Maximum observed owned capacities at our phase boundaries, INCLUDING
    // caller source. NOT a hard peak: upstream bake/builder/codec temporary
    // vectors, allocator/control blocks and library scratch are uninstrumented.
    uint64_t knownObservedCapacityHighWater=0;
};
enum class RigidFinalHierarchyStatus { Produced,Invalid,Capacity,AllocationFailed };
namespace RigidFinalHierarchyDetail
{
inline uint64_t MeshCapacity(const ExportedMesh& mesh)
{
    return mesh.vertices.capacity()*uint64_t(sizeof(SVertex))+mesh.positions.capacity()*uint64_t(sizeof(Position))+
        (mesh.indices.capacity()+mesh.materials.capacity())*uint64_t(sizeof(uint32_t));
}
inline uint64_t BakeCapacity(const ClodBake& bake)
{
    uint64_t result=sizeof(bake)+MeshCapacity(bake.original)+bake.groups.capacity()*uint64_t(sizeof(ClodBakeGroup))+
        bake.clusters.capacity()*uint64_t(sizeof(ClodBakeCluster));
    for(const auto& c:bake.clusters)result+=c.indices.capacity()*uint64_t(sizeof(uint32_t));
    return result;
}
inline bool ValidMesh(const ExportedMesh& mesh)
{
    if(mesh.vertices.empty()||mesh.vertices.size()>4096||mesh.positions.size()!=mesh.vertices.size()||
       mesh.indices.empty()||mesh.indices.size()>8192*3||mesh.indices.size()%3||
       mesh.materials.size()!=mesh.indices.size()/3)return false;
    for(size_t i=0;i<mesh.vertices.size();++i) {
        const auto& v=mesh.vertices[i];const auto& p=mesh.positions[i];
        if(!HierarchicalDiskDetail::VertexValid(v)||v.pos.X()!=p.x||v.pos.Y()!=p.y||v.pos.Z()!=p.z)return false;
    }
    for(auto i:mesh.indices)if(i>=mesh.vertices.size())return false;
    for(auto m:mesh.materials)if(m!=0)return false;
    return true;
}
inline bool ValidLimits(const RigidFinalHierarchyLimits& l)
{
    return l.groups&&l.groups<=64&&l.clusters&&l.clusters<=63&&l.pages&&l.pages<=64&&
        l.pageBytes>=1024&&l.pageBytes<=65536&&l.sourceCapacityBytes&&l.sourceCapacityBytes<=128*1024&&
        l.packageCapacityBytes&&l.packageCapacityBytes<=1024*1024&&
        l.encodedCapacityBytes&&l.encodedCapacityBytes<=HierarchicalDiskDetail::MaxFileBytes&&
        l.observedCapacityBytes&&l.observedCapacityBytes<=4*1024*1024;
}
// Sequential decoded-page accumulator: no all-pages payload copy. Original IDs
// distinguish coincident authored seams; cyclic rotation preserves orientation.
class FineTriangleParity
{
    using Triangle=std::array<uint32_t,3>;
    std::vector<Triangle> source_,selected_;
    std::array<bool,64> wanted_{},seen_{};
    uint32_t vertexCount_=0;
    bool prepared_=false,failed_=false;
    bool Refuse(){failed_=true;return false;}
    static Triangle Oriented(uint32_t a,uint32_t b,uint32_t c)
    {return std::min({Triangle{a,b,c},Triangle{b,c,a},Triangle{c,a,b}});}
public:
    static uint64_t ReservationBytes(size_t triangles)
    {return sizeof(FineTriangleParity)+triangles*2*sizeof(Triangle);}
    uint64_t KnownBytes() const
    {return sizeof(*this)+(source_.capacity()+selected_.capacity())*uint64_t(sizeof(Triangle));}
    bool Prepare(const ExportedMesh& source,std::span<const uint32_t> clusters)
    {
        if(prepared_||failed_||source.vertices.empty()||source.vertices.size()>4096||source.indices.empty()||
           source.indices.size()%3||source.indices.size()/3>8192||clusters.empty()||clusters.size()>63)return Refuse();
        for(auto id:source.indices)if(id>=source.vertices.size())return Refuse();
        for(auto id:clusters){if(id>=wanted_.size()||wanted_[id])return Refuse();wanted_[id]=true;}
        vertexCount_=uint32_t(source.vertices.size());
        source_.reserve(source.indices.size()/3);selected_.reserve(source.indices.size()/3);
        for(size_t i=0;i<source.indices.size();i+=3)
            source_.push_back(Oriented(source.indices[i],source.indices[i+1],source.indices[i+2]));
        std::sort(source_.begin(),source_.end());prepared_=true;return true;
    }
    bool Add(const HierarchicalPage& page)
    {
        if(!prepared_||failed_)return Refuse();
        for(const auto& cluster:page.clusters) {
            if(cluster.cluster>=wanted_.size())return Refuse();
            if(!wanted_[cluster.cluster])continue;
            if(seen_[cluster.cluster]||cluster.vertices.empty()||cluster.vertices.size()>64||
               cluster.originalVertexIds.size()!=cluster.vertices.size()||cluster.indices.empty()||
               cluster.indices.size()%3||cluster.indices.size()/3>source_.size()-selected_.size())return Refuse();
            for(size_t i=0;i<cluster.originalVertexIds.size();++i) {
                if(cluster.originalVertexIds[i]>=vertexCount_)return Refuse();
                for(size_t j=0;j<i;++j)if(cluster.originalVertexIds[i]==cluster.originalVertexIds[j])return Refuse();
            }
            for(auto local:cluster.indices)if(local>=cluster.originalVertexIds.size())return Refuse();
            for(size_t i=0;i<cluster.indices.size();i+=3)
                selected_.push_back(Oriented(cluster.originalVertexIds[cluster.indices[i]],
                    cluster.originalVertexIds[cluster.indices[i+1]],cluster.originalVertexIds[cluster.indices[i+2]]));
            seen_[cluster.cluster]=true;
        }
        return true;
    }
    bool Finish()
    {
        if(!prepared_||failed_||source_.size()!=selected_.size())return false;
        for(size_t i=0;i<wanted_.size();++i)if(wanted_[i]!=seen_[i])return false;
        std::sort(selected_.begin(),selected_.end());return selected_==source_;
    }
};
}
// Explicit producer-only call, never a camera/frame demand action. The root
// passes its successful ACTUAL finalized export; this pure helper cannot prove
// that provenance from a manufactured DTO. No new MLOD/IR conversion, VFS, bank,
// texture work, file writing, GPU resources, world eligibility or pass coverage.
// Same pinned CLOD baker + full DAG/page builder + existing codec semantics.
inline RigidFinalHierarchyStatus BuildRigidFinalHierarchyProduct(const RigidOdol7FinalExport& actual,
    RigidFinalHierarchyProduct& destination,RigidFinalHierarchyLimits limits={})
{
    using Status=RigidFinalHierarchyStatus;
    const auto& source=actual.actual.source;
    if(!RigidFinalHierarchyDetail::ValidLimits(limits)||!actual.requiresOriginalOtherPasses||
       !actual.sourceAdmissionEpoch||actual.sourceAdmissionEpoch==UINT64_MAX||
       !actual.ownerEpoch||actual.ownerEpoch==UINT64_MAX||!actual.modelBirth||actual.modelBirth==UINT64_MAX||
       !actual.shapeQueryRevision||actual.shapeQueryRevision==UINT64_MAX||
       actual.finalLevels[0]<0||actual.finalLevels[1]<0||actual.finalLevels[0]==actual.finalLevels[1]||
       actual.sourceLods[0]!=source.coarseRepresentation||actual.sourceLods[1]!=source.fineRepresentation||
       !std::isfinite(actual.resolutions[0])||!std::isfinite(actual.resolutions[1])||
       actual.resolutions[1]<0||actual.resolutions[0]<=actual.resolutions[1]||
       !HierarchicalDetail::Nonzero(source.sourceSha256)||source.geometryOptions!=0x4f3746494e414c31ull||
       source.materialOptions!=(0x2000ull|(uint64_t(actual.actualNoShadow)<<32))||
       source.producerVersion!=1||source.vertexLayout!=sizeof(SVertex)||source.materialMapping!=1||
       source.coarseRepresentation==source.fineRepresentation||
       !RigidFinalHierarchyDetail::ValidMesh(actual.actual.coarse)||
       !RigidFinalHierarchyDetail::ValidMesh(actual.actual.fine))return Status::Invalid;
    const uint64_t sourceBytes=sizeof(actual)+RigidFinalHierarchyDetail::MeshCapacity(actual.actual.coarse)+
        RigidFinalHierarchyDetail::MeshCapacity(actual.actual.fine)+actual.material.sourceName.capacity();
    if(sourceBytes>limits.sourceCapacityBytes||sourceBytes>limits.observedCapacityBytes)return Status::Capacity;
    try {
        RigidFinalHierarchyProduct result;result.expectedSource=source;
        result.sourceAdmissionEpoch=actual.sourceAdmissionEpoch;result.ownerEpoch=actual.ownerEpoch;
        result.modelBirth=actual.modelBirth;result.shapeQueryRevision=actual.shapeQueryRevision;
        result.actualNoShadow=actual.actualNoShadow;result.requiresOriginalOtherPasses=true;
        result.knownSourceCapacityBytes=sourceBytes;result.knownObservedCapacityHighWater=sourceBytes;
        const auto observe=[&](uint64_t bytes) {
            if(bytes>limits.observedCapacityBytes-sourceBytes)return false;
            result.knownObservedCapacityHighWater=std::max(result.knownObservedCapacityHighWater,sourceBytes+bytes);
            return true;
        };
        ClodBake bake;ClodBakeLimits bakeLimits;
        bakeLimits.groups=limits.groups;bakeLimits.clusters=limits.clusters;
        bakeLimits.indexEntries=32768;bakeLimits.storedBytes=limits.packageCapacityBytes;
        const auto baked=BakeClodPilot(actual.actual,true,bake,bakeLimits);
        if(baked!=ClodBakeStatus::Baked)return baked==ClodBakeStatus::Capacity?Status::Capacity:
            baked==ClodBakeStatus::AllocationFailed?Status::AllocationFailed:Status::Invalid;
        uint64_t bakeBytes=RigidFinalHierarchyDetail::BakeCapacity(bake);
        if(!observe(bakeBytes))return Status::Capacity;
        HierarchicalPackage package;HierarchicalLimits pageLimits;
        pageLimits.groups=limits.groups;pageLimits.clusters=limits.clusters;pageLimits.pages=limits.pages;
        pageLimits.pageBytes=limits.pageBytes;pageLimits.knownCapacityBytes=limits.packageCapacityBytes;
        const auto built=BuildHierarchicalPackage(bake,package,pageLimits);
        if(built!=HierarchicalBuildStatus::Built)return built==HierarchicalBuildStatus::Capacity?Status::Capacity:
            built==HierarchicalBuildStatus::AllocationFailed?Status::AllocationFailed:Status::Invalid;
        const uint64_t packageBytes=HierarchicalKnownBytes(package);
        if(!observe(bakeBytes+packageBytes))return Status::Capacity;
        bake=ClodBake{}; // release copied source/bake debt before codec/roundtrip
        auto image=std::make_shared<HierarchicalDiskImage>();
        const auto encoded=EncodeHierarchicalDisk(package,*image);
        if(encoded!=HierarchicalDiskStatus::Encoded)return encoded==HierarchicalDiskStatus::Capacity?Status::Capacity:
            encoded==HierarchicalDiskStatus::AllocationFailed?Status::AllocationFailed:Status::Invalid;
        const uint64_t imageBytes=sizeof(*image)+image->bytes.capacity();
        if(image->bytes.capacity()>limits.encodedCapacityBytes||!observe(packageBytes+imageBytes))return Status::Capacity;
        const uint64_t prefix=HierarchicalDiskDetail::HeaderBytes+
            package.groups.size()*HierarchicalDiskDetail::GroupBytes+package.clusters.size()*HierarchicalDiskDetail::ClusterBytes+
            package.pages.size()*HierarchicalDiskDetail::PageRowBytes+(package.rootClusters.size()+package.rootPages.size())*4;
        if(prefix>HierarchicalDiskDetail::MaxMetadataBytes||prefix>image->bytes.size())return Status::Capacity;
        auto manifest=std::make_shared<HierarchicalDiskManifest>();
        if(DecodeHierarchicalManifest(std::span<const uint8_t>(image->bytes).first(size_t(prefix)),image->bytes.size(),
            image->identity,image->metadataSha256,*manifest)!=HierarchicalDiskStatus::Decoded)return Status::Invalid;
        if(!(manifest->metadata.identity.source==source)||manifest->metadata.rootPages!=package.rootPages||
           manifest->metadata.rootClusters!=package.rootClusters)return Status::Invalid;
        result.knownRetainedCapacityBytes=sizeof(result)+imageBytes+manifest->knownCapacityBytes;
        if(!observe(packageBytes+result.knownRetainedCapacityBytes))return Status::Capacity;
        std::array<uint8_t,64> allResident{};allResident.fill(1);
        HierarchicalCutPlan fineCut;
        if(!PlanHierarchicalCut(manifest->metadata,0,{manifest->metadata.identity,
            std::span<const uint8_t>(allResident.data(),manifest->pages.size())},fineCut)||
           fineCut.state!=HierarchicalCutState::RequestedCut)return Status::Invalid;
        const uint64_t planBytes=sizeof(fineCut)+(fineCut.requestedClusters.capacity()+fineCut.requiredPages.capacity()+
            fineCut.missingPages.capacity()+fineCut.selectedClusters.capacity())*uint64_t(sizeof(uint32_t));
        RigidFinalHierarchyDetail::FineTriangleParity parity;
        if(!observe(packageBytes+result.knownRetainedCapacityBytes+planBytes+
            parity.ReservationBytes(actual.actual.fine.indices.size()/3)))return Status::Capacity;
        if(!parity.Prepare(actual.actual.fine,fineCut.selectedClusters))return Status::Invalid;
        const uint64_t scratchBytes=planBytes+parity.KnownBytes();
        if(!observe(packageBytes+result.knownRetainedCapacityBytes+scratchBytes))return Status::Capacity;
        for(uint32_t p=0;p<manifest->pages.size();++p) {
            const auto& range=manifest->pages[p];HierarchicalPage page;
            if(range.bytes>65536||range.offset>image->bytes.size()||range.bytes>image->bytes.size()-range.offset||
               DecodeHierarchicalDiskPage(*manifest,p,std::span<const uint8_t>(image->bytes).subspan(size_t(range.offset),range.bytes),
                   page)!=HierarchicalDiskStatus::Decoded||
               !HierarchicalOriginalOutside::ExactOriginalFinePageVertices(page,actual.actual.fine.vertices))return Status::Invalid;
            if(!observe(packageBytes+result.knownRetainedCapacityBytes+scratchBytes+
                HierarchicalDiskPageKnownBytes(page)))return Status::Capacity;
            if(!parity.Add(page))return Status::Invalid;
        }
        if(!parity.Finish())return Status::Invalid;
        result.fineTriangleSetExact=true;
        result.fileSha256=HierarchicalDiskDetail::Hash(image->bytes);
        result.image=std::move(image);result.manifest=std::move(manifest);
        destination=std::move(result);return Status::Produced;
    }catch(const std::bad_alloc&){return Status::AllocationFailed;}
     catch(...){return Status::Invalid;}
}
}
