#pragma once

#include <Poseidon/Graphics/Rendering/GeometryPageRigidFinalHierarchyProduct.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageSurfaceCertificate.hpp>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <limits>
#include <memory>

namespace Poseidon::GeometryPages::RigidFinalSurface
{
// This proof describes only two complete, independently decoded GHP selected
// triangle unions in object space. Actual source/Shape/material/world admission
// and renderer publication remain owner checks. It is not a CLOD-error, pixel,
// normal/UV-fidelity, original-coarse-LOD, GPU, or all-view certificate.
enum class Status { Certified, Invalid, Unsupported, Capacity, AllocationFailed };
struct Limits
{
    uint64_t distanceVisits = 2'000'000;
    uint64_t selectedCapacityBytes = 512 * 1024;
    uint64_t knownObservedCapacityBytes = 4 * 1024 * 1024;
};
struct SelectedCutArtifact
{
    HierarchicalIdentity identity;
    uint32_t thresholdBits = 0;
    std::vector<uint32_t> clusters, requiredPages;
    std::vector<std::array<uint8_t,32>> requiredPageSha256;
    ExportedMesh packed; // decoded SVertex scalars, local indices rebased in cluster order
    std::array<uint8_t,32> scalarPackedSha256{};
    uint64_t knownCapacityBytes = 0;
};
struct Certificate
{
    static constexpr uint32_t AlgorithmVersion = 2;
    HierarchicalIdentity identity;
    SourceIdentity originalSource;
    // Descriptor of this authenticated GHP codec/packing, not a legacy GCD
    // selected package key. Full identity remains `identity` with package SHA.
    CacheIdentity selectedDescriptor;
    std::array<uint8_t,32> fileSha256{},metadataSha256{};
    std::array<uint8_t,32> rootScalarPackedSha256{},fineScalarPackedSha256{};
    uint64_t sourceAdmissionEpoch = 0,ownerEpoch = 0,modelBirth = 0,shapeQueryRevision = 0;
    uint32_t rootThresholdBits = 0,fineThresholdBits = 0;
    std::array<uint32_t,64> rootCut{},fineCut{};
    uint32_t rootCutCount = 0,fineCutCount = 0;
    uint32_t rootTriangles = 0,fineTriangles = 0;
    // Exact component extrema of both decoded selected unions. They enclose
    // their closed triangles and can feed a later owner-bound projection tuple.
    std::array<float,3> minimum{},maximum{};
    uint64_t distanceVisits = 0;
    double fineToRoot = 0,rootToFine = 0,hausdorffUpper = 0;
};
struct Proof
{
    std::shared_ptr<const Certificate> certificate;
    std::shared_ptr<const SelectedCutArtifact> root,fine;
    // Known owned C++ capacities at inspected phase boundaries, including the
    // supplied product/source. Upstream bake/codec/library scratch, allocator
    // overhead, RSS, GPU storage, and latency are not hard-bounded here.
    uint64_t knownObservedCapacityHighWater = 0;
};

namespace Detail
{
inline bool Platform()
{
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    return false;
#else
    return std::endian::native == std::endian::little &&
        std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559 &&
        std::numeric_limits<float>::radix == 2 && std::numeric_limits<double>::radix == 2 &&
        std::numeric_limits<float>::digits == 24 && std::numeric_limits<double>::digits == 53;
#endif
}
inline bool BoundsEqual(const clodBounds& a,const clodBounds& b)
{
    for(size_t i=0;i<3;++i)
        if(std::bit_cast<uint32_t>(a.center[i])!=std::bit_cast<uint32_t>(b.center[i]))return false;
    return std::bit_cast<uint32_t>(a.radius)==std::bit_cast<uint32_t>(b.radius) &&
        std::bit_cast<uint32_t>(a.error)==std::bit_cast<uint32_t>(b.error);
}
inline bool SameManifest(const HierarchicalDiskManifest& a,const HierarchicalDiskManifest& b)
{
    const auto& x=a.metadata;const auto& y=b.metadata;
    if(!(x.identity==y.identity)||x.pageByteLimit!=y.pageByteLimit||
       x.groups.size()!=y.groups.size()||x.clusters.size()!=y.clusters.size()||
       x.pages.size()!=y.pages.size()||x.rootClusters!=y.rootClusters||x.rootPages!=y.rootPages||
       a.pages.size()!=b.pages.size()||a.fileBytes!=b.fileBytes||a.metadataBytes!=b.metadataBytes||
       a.metadataSha256!=b.metadataSha256)return false;
    for(size_t i=0;i<x.groups.size();++i) {
        const auto& p=x.groups[i];const auto& q=y.groups[i];
        if(p.depth!=q.depth||p.first!=q.first||p.count!=q.count||!BoundsEqual(p.simplified,q.simplified))return false;
    }
    for(size_t i=0;i<x.clusters.size();++i) {
        const auto& p=x.clusters[i];const auto& q=y.clusters[i];
        if(p.group!=q.group||p.page!=q.page||p.pageSlot!=q.pageSlot||p.refined!=q.refined||
           !BoundsEqual(p.bounds,q.bounds))return false;
    }
    for(size_t i=0;i<x.pages.size();++i) {
        const auto& p=x.pages[i];const auto& q=y.pages[i];
        if(p.group!=q.group||p.logicalBytes!=q.logicalBytes||p.uploadBytes!=q.uploadBytes||
           p.clusters.size()!=q.clusters.size())return false;
        for(size_t j=0;j<p.clusters.size();++j)if(p.clusters[j].cluster!=q.clusters[j].cluster)return false;
        if(a.pages[i].offset!=b.pages[i].offset||a.pages[i].bytes!=b.pages[i].bytes||
           a.pages[i].sha256!=b.pages[i].sha256)return false;
    }
    return true;
}
inline uint64_t MeshCapacity(const ExportedMesh& mesh)
{
    return mesh.vertices.capacity()*uint64_t(sizeof(SVertex))+
        mesh.positions.capacity()*uint64_t(sizeof(Position))+
        (mesh.indices.capacity()+mesh.materials.capacity())*uint64_t(sizeof(uint32_t));
}
inline uint64_t ArtifactCapacity(const SelectedCutArtifact& artifact)
{
    return sizeof(artifact)+MeshCapacity(artifact.packed)+
        (artifact.clusters.capacity()+artifact.requiredPages.capacity())*uint64_t(sizeof(uint32_t))+
        artifact.requiredPageSha256.capacity()*uint64_t(sizeof(std::array<uint8_t,32>));
}
inline std::array<uint8_t,32> ScalarHash(const ExportedMesh& mesh)
{
    Foundation::Sha256 hash;hash.Update(std::string("retail-ghp-selected-scalar-packed-v1"));
    const auto number=[&](uint64_t value) {
        uint8_t bytes[4]{};for(unsigned i=0;i<4;++i)bytes[i]=uint8_t(value>>(i*8));
        hash.Update(bytes,sizeof(bytes));
    };
    number(sizeof(SVertex));number(HierarchicalDiskDetail::VertexScalarBytes);
    number(mesh.vertices.size());
    std::vector<uint8_t> scalar;scalar.reserve(HierarchicalDiskDetail::VertexScalarBytes);
    for(const auto& vertex:mesh.vertices) {
        scalar.clear();HierarchicalDiskDetail::Vertex(scalar,vertex);
        hash.Update(scalar.data(),scalar.size());
    }
    number(mesh.indices.size());for(auto index:mesh.indices)number(index);
    number(mesh.materials.size());for(auto material:mesh.materials)number(material);
    return HierarchicalDetail::Digest(hash);
}
inline bool ScalarEqual(const ExportedMesh& a,const ExportedMesh& b)
{
    if(a.vertices.size()!=b.vertices.size()||a.indices!=b.indices||a.materials!=b.materials||
       a.positions.size()!=b.positions.size())return false;
    std::vector<uint8_t> left,right;left.reserve(HierarchicalDiskDetail::VertexScalarBytes);
    right.reserve(HierarchicalDiskDetail::VertexScalarBytes);
    for(size_t i=0;i<a.vertices.size();++i) {
        left.clear();right.clear();HierarchicalDiskDetail::Vertex(left,a.vertices[i]);
        HierarchicalDiskDetail::Vertex(right,b.vertices[i]);
        if(left!=right||std::bit_cast<uint32_t>(a.positions[i].x)!=std::bit_cast<uint32_t>(b.positions[i].x)||
           std::bit_cast<uint32_t>(a.positions[i].y)!=std::bit_cast<uint32_t>(b.positions[i].y)||
           std::bit_cast<uint32_t>(a.positions[i].z)!=std::bit_cast<uint32_t>(b.positions[i].z))return false;
    }
    return true;
}
inline bool RoundTripPage(const HierarchicalPage& page,uint32_t id,std::span<const uint8_t> original)
{
    std::vector<uint8_t> encoded;encoded.reserve(original.size());
    HierarchicalDiskDetail::Number(encoded,0x31475048);
    HierarchicalDiskDetail::Number(encoded,HierarchicalDiskDetail::Schema);
    HierarchicalDiskDetail::Number(encoded,id);HierarchicalDiskDetail::Number(encoded,page.group);
    HierarchicalDiskDetail::Number(encoded,page.clusters.size());
    HierarchicalDiskDetail::Number(encoded,original.size());
    for(const auto& c:page.clusters) {
        HierarchicalDiskDetail::Number(encoded,c.cluster);
        HierarchicalDiskDetail::Number(encoded,c.material);
        HierarchicalDiskDetail::Number(encoded,c.vertices.size());
        HierarchicalDiskDetail::Number(encoded,c.indices.size());
        for(auto vertexId:c.originalVertexIds)HierarchicalDiskDetail::Number(encoded,vertexId);
        for(const auto& vertex:c.vertices)HierarchicalDiskDetail::Vertex(encoded,vertex);
        for(auto index:c.indices)HierarchicalDiskDetail::Number(encoded,index);
    }
    return encoded.size()==original.size()&&std::equal(encoded.begin(),encoded.end(),original.begin());
}
inline bool Append(const HierarchicalPayload& cluster,ExportedMesh& mesh)
{
    if(cluster.material||cluster.vertices.empty()||cluster.vertices.size()>64||
       mesh.vertices.size()>1024-cluster.vertices.size()||
       cluster.indices.size()>4096-mesh.indices.size())return false;
    const uint32_t base=uint32_t(mesh.vertices.size());
    for(const auto& v:cluster.vertices) {
        mesh.vertices.push_back(v);
        mesh.positions.push_back({v.pos.X(),v.pos.Y(),v.pos.Z()});
    }
    for(auto index:cluster.indices)mesh.indices.push_back(base+index);
    mesh.materials.insert(mesh.materials.end(),cluster.indices.size()/3,0);
    return true;
}
// For one source triangle, choose a SINGLE closed target triangle. Each source
// corner may choose a different vertex of that target triangle. Applying the
// source barycentric weights to those anchors gives a point inside that same
// target triangle, and convexity bounds every distance by the maximum corner
// distance. Taking a minimum over complete target triangles remains an upper
// bound. Independent nearest anchors across different target triangles would
// not have this property (their convex hull can span holes).
// This optional refinement consumes only remaining visits; the independently
// completed single-anchor baseline remains valid if the refinement budget ends.
inline Status RefineDirected(const ExportedMesh& from,const ExportedMesh& target,
    uint64_t cap,uint64_t& visits,double& bound)
{
    double refined=0;
    for(size_t triangle=0;triangle<from.indices.size();triangle+=3) {
        double best=bound;
        for(size_t candidate=0;candidate<target.indices.size();candidate+=3) {
            if(cap-visits<9)return Status::Certified; // Preserve the complete baseline.
            double maximum=0;
            for(unsigned corner=0;corner<3;++corner) {
                double nearest=std::numeric_limits<double>::infinity();
                for(unsigned anchor=0;anchor<3;++anchor) {
                    ++visits;double distance=0;
                    if(!SurfaceCertificate::Detail::DistanceUpper(
                        from.vertices[from.indices[triangle+corner]],
                        target.vertices[target.indices[candidate+anchor]],distance))
                        return Status::Unsupported;
                    nearest=std::min(nearest,distance);
                }
                maximum=std::max(maximum,nearest);
            }
            best=std::min(best,maximum);
            if(best==0)break;
        }
        refined=std::max(refined,best);
    }
    bound=refined;
    return Status::Certified;
}
inline Status Distance(const ExportedMesh& root,const ExportedMesh& fine,Limits limits,Certificate& result)
{
    if(!Platform())return Status::Unsupported;
    if(!limits.distanceVisits||limits.distanceVisits>2'000'000)return Status::Capacity;
    std::array<uint8_t,1024> rootReferenced{},fineReferenced{};
    if(!SurfaceCertificate::Detail::Mesh(root,rootReferenced)||
       !SurfaceCertificate::Detail::Mesh(fine,fineReferenced))return Status::Unsupported;
    const auto forward=SurfaceCertificate::Detail::Directed(fine,root,rootReferenced,
        limits.distanceVisits,result.distanceVisits,result.fineToRoot);
    if(forward!=SurfaceCertificate::Status::Certified)
        return forward==SurfaceCertificate::Status::Capacity?Status::Capacity:Status::Unsupported;
    const auto reverse=SurfaceCertificate::Detail::Directed(root,fine,fineReferenced,
        limits.distanceVisits,result.distanceVisits,result.rootToFine);
    if(reverse!=SurfaceCertificate::Status::Certified)
        return reverse==SurfaceCertificate::Status::Capacity?Status::Capacity:Status::Unsupported;
    const auto halfway=result.distanceVisits+(limits.distanceVisits-result.distanceVisits)/2;
    if(RefineDirected(fine,root,halfway,result.distanceVisits,result.fineToRoot)!=Status::Certified ||
       RefineDirected(root,fine,limits.distanceVisits,result.distanceVisits,result.rootToFine)!=Status::Certified)
        return Status::Unsupported;
    result.hausdorffUpper=std::max(result.fineToRoot,result.rootToFine);
    if(!std::isfinite(result.hausdorffUpper)||result.hausdorffUpper>10000)return Status::Unsupported;
    return Status::Certified;
}
inline void UnionBounds(const ExportedMesh& root,const ExportedMesh& fine,Certificate& result)
{
    result.minimum.fill(std::numeric_limits<float>::infinity());
    result.maximum.fill(-std::numeric_limits<float>::infinity());
    for(const auto* mesh:{&root,&fine})for(const auto& vertex:mesh->vertices) {
        const std::array<float,3> xyz{vertex.pos.X(),vertex.pos.Y(),vertex.pos.Z()};
        for(size_t i=0;i<3;++i) {
            result.minimum[i]=std::min(result.minimum[i],xyz[i]);
            result.maximum[i]=std::max(result.maximum[i],xyz[i]);
        }
    }
}
} // namespace Detail

// Product is an explicit successful producer result from an actual final Shape
// export. This re-authenticates its whole image/metadata and every independent
// decoded page before deriving Root/Fine packed geometry. Output is atomic.
inline Status Build(const RigidOdol7FinalExport& actual,const RigidFinalHierarchyProduct& product,
    Proof& destination,Limits limits={})
{
    if(!Detail::Platform())return Status::Unsupported;
    if(!limits.distanceVisits||limits.distanceVisits>2'000'000||
       !limits.selectedCapacityBytes||limits.selectedCapacityBytes>512*1024||
       !limits.knownObservedCapacityBytes||limits.knownObservedCapacityBytes>4*1024*1024)
        return Status::Capacity;
    if(!product.image||!product.manifest||!product.fineTriangleSetExact||
       !(product.expectedSource==actual.actual.source)||
       product.sourceAdmissionEpoch!=actual.sourceAdmissionEpoch||product.ownerEpoch!=actual.ownerEpoch||
       product.modelBirth!=actual.modelBirth||product.shapeQueryRevision!=actual.shapeQueryRevision||
       product.actualNoShadow!=actual.actualNoShadow||!product.requiresOriginalOtherPasses||
       !actual.requiresOriginalOtherPasses||!product.sourceAdmissionEpoch||!product.ownerEpoch||
       !product.modelBirth||!product.shapeQueryRevision||
       !(product.image->identity.source==actual.actual.source)||
       !(product.manifest->metadata.identity==product.image->identity))return Status::Invalid;
    const auto& image=*product.image;
    if(image.bytes.empty()||image.bytes.size()>HierarchicalDiskDetail::MaxFileBytes||
       product.fileSha256!=HierarchicalDiskDetail::Hash(image.bytes)||
       image.metadataSha256!=product.manifest->metadataSha256||
       product.manifest->metadataBytes>HierarchicalDiskDetail::MaxMetadataBytes||
       product.manifest->metadataBytes>image.bytes.size())return Status::Invalid;
    try {
        HierarchicalDiskManifest manifest;
        if(DecodeHierarchicalManifest(std::span<const uint8_t>(image.bytes).first(
                size_t(product.manifest->metadataBytes)),image.bytes.size(),image.identity,
                image.metadataSha256,manifest)!=HierarchicalDiskStatus::Decoded||
           !Detail::SameManifest(manifest,*product.manifest))return Status::Invalid;
        const auto& metadata=manifest.metadata;
        std::array<uint8_t,64> allResident{};allResident.fill(1);
        const float rootThreshold=std::nextafter(FLT_MAX,0.f);
        HierarchicalCutPlan rootPlan,finePlan;
        const auto resident=HierarchicalResidentPages{metadata.identity,
            std::span<const uint8_t>(allResident.data(),manifest.pages.size())};
        if(!PlanHierarchicalCut(metadata,rootThreshold,resident,rootPlan)||
           !PlanHierarchicalCut(metadata,0,resident,finePlan)||
           rootPlan.state!=HierarchicalCutState::RequestedCut||
           finePlan.state!=HierarchicalCutState::RequestedCut||
           rootPlan.selectedClusters!=metadata.rootClusters||
           rootPlan.selectedClusters.empty()||finePlan.selectedClusters.empty()||
           rootPlan.selectedClusters.size()>16||finePlan.selectedClusters.size()>16||
           !rootPlan.missingPages.empty()||!finePlan.missingPages.empty())return Status::Invalid;
        auto root=std::make_shared<SelectedCutArtifact>();
        auto fine=std::make_shared<SelectedCutArtifact>();
        for(auto pair:{std::pair{root.get(),&rootPlan},std::pair{fine.get(),&finePlan}}) {
            auto& artifact=*pair.first;const auto& plan=*pair.second;
            artifact.identity=metadata.identity;
            artifact.thresholdBits=std::bit_cast<uint32_t>(pair.first==root.get()?rootThreshold:0.f);
            artifact.clusters=plan.selectedClusters;artifact.requiredPages=plan.requiredPages;
            for(auto page:artifact.requiredPages)artifact.requiredPageSha256.push_back(manifest.pages[page].sha256);
        }
        RigidFinalHierarchyDetail::FineTriangleParity parity;
        if(!parity.Prepare(actual.actual.fine,fine->clusters))return Status::Invalid;
        size_t rootAt=0,fineAt=0;
        uint64_t highWater=std::max(product.knownObservedCapacityHighWater,
            product.knownSourceCapacityBytes+product.knownRetainedCapacityBytes);
        const auto observe=[&](uint64_t extra) {
            const uint64_t base=product.knownSourceCapacityBytes+product.knownRetainedCapacityBytes;
            if(base>limits.knownObservedCapacityBytes||extra>limits.knownObservedCapacityBytes-base)return false;
            highWater=std::max(highWater,base+extra);return true;
        };
        const uint64_t parityReservation=parity.ReservationBytes(actual.actual.fine.indices.size()/3);
        if(!observe(manifest.knownCapacityBytes+Detail::ArtifactCapacity(*root)+
                    Detail::ArtifactCapacity(*fine)+parityReservation+2*65536))return Status::Capacity;
        for(uint32_t pageId=0;pageId<manifest.pages.size();++pageId) {
            const auto& range=manifest.pages[pageId];
            if(range.offset>image.bytes.size()||range.bytes>image.bytes.size()-range.offset)return Status::Invalid;
            const auto encoded=std::span<const uint8_t>(image.bytes).subspan(size_t(range.offset),range.bytes);
            HierarchicalPage page;
            if(DecodeHierarchicalDiskPage(manifest,pageId,encoded,page)!=HierarchicalDiskStatus::Decoded||
               !Detail::RoundTripPage(page,pageId,encoded)||
               !HierarchicalOriginalOutside::ExactOriginalFinePageVertices(page,actual.actual.fine.vertices)||
               !parity.Add(page))return Status::Invalid;
            for(const auto& cluster:page.clusters) {
                if(rootAt<root->clusters.size()&&cluster.cluster==root->clusters[rootAt]) {
                    if(!Detail::Append(cluster,root->packed))return Status::Capacity;
                    ++rootAt;
                }
                if(fineAt<fine->clusters.size()&&cluster.cluster==fine->clusters[fineAt]) {
                    if(!Detail::Append(cluster,fine->packed))return Status::Capacity;
                    ++fineAt;
                }
            }
            if(!observe(manifest.knownCapacityBytes+Detail::ArtifactCapacity(*root)+
                        Detail::ArtifactCapacity(*fine)+parity.KnownBytes()+
                        HierarchicalDiskPageKnownBytes(page)+encoded.size()))return Status::Capacity;
        }
        if(rootAt!=root->clusters.size()||fineAt!=fine->clusters.size()||!parity.Finish())return Status::Invalid;
        root->knownCapacityBytes=Detail::ArtifactCapacity(*root);
        fine->knownCapacityBytes=Detail::ArtifactCapacity(*fine);
        if(root->knownCapacityBytes>limits.selectedCapacityBytes||
           fine->knownCapacityBytes>limits.selectedCapacityBytes-root->knownCapacityBytes)return Status::Capacity;
        auto certificate=std::make_shared<Certificate>();
        certificate->identity=metadata.identity;certificate->originalSource=actual.actual.source;
        certificate->selectedDescriptor.source=metadata.identity.source;
        certificate->selectedDescriptor.packing={64,128,metadata.pageByteLimit};
        certificate->selectedDescriptor.formatVersion=HierarchicalDiskDetail::Schema;
        certificate->selectedDescriptor.algorithmVersion=metadata.identity.adapterVersion;
        certificate->fileSha256=product.fileSha256;certificate->metadataSha256=image.metadataSha256;
        certificate->sourceAdmissionEpoch=actual.sourceAdmissionEpoch;certificate->ownerEpoch=actual.ownerEpoch;
        certificate->modelBirth=actual.modelBirth;certificate->shapeQueryRevision=actual.shapeQueryRevision;
        certificate->rootThresholdBits=root->thresholdBits;certificate->fineThresholdBits=fine->thresholdBits;
        certificate->rootCutCount=uint32_t(root->clusters.size());
        certificate->fineCutCount=uint32_t(fine->clusters.size());
        std::copy(root->clusters.begin(),root->clusters.end(),certificate->rootCut.begin());
        std::copy(fine->clusters.begin(),fine->clusters.end(),certificate->fineCut.begin());
        certificate->rootTriangles=uint32_t(root->packed.indices.size()/3);
        certificate->fineTriangles=uint32_t(fine->packed.indices.size()/3);
        const auto measured=Detail::Distance(root->packed,fine->packed,limits,*certificate);
        if(measured!=Status::Certified)return measured;
        Detail::UnionBounds(root->packed,fine->packed,*certificate);
        root->scalarPackedSha256=Detail::ScalarHash(root->packed);
        fine->scalarPackedSha256=Detail::ScalarHash(fine->packed);
        certificate->rootScalarPackedSha256=root->scalarPackedSha256;
        certificate->fineScalarPackedSha256=fine->scalarPackedSha256;
        Proof result;result.certificate=std::move(certificate);
        result.root=std::move(root);result.fine=std::move(fine);
        result.knownObservedCapacityHighWater=std::max(highWater,
            product.knownSourceCapacityBytes+product.knownRetainedCapacityBytes+
            manifest.knownCapacityBytes+result.root->knownCapacityBytes+result.fine->knownCapacityBytes+
            parity.KnownBytes()+sizeof(Certificate)+sizeof(Proof));
        if(result.knownObservedCapacityHighWater>limits.knownObservedCapacityBytes)return Status::Capacity;
        destination=std::move(result);return Status::Certified;
    }catch(const std::bad_alloc&){return Status::AllocationFailed;}
     catch(...){return Status::Invalid;}
}

inline bool MatchesSelected(const SelectedCutArtifact& artifact,const HierarchicalCutPlan& plan,
    float threshold,const ExportedMesh& decoded)
{
    if(!(artifact.identity==plan.identity)||plan.state!=HierarchicalCutState::RequestedCut||
       !std::isfinite(threshold)||artifact.thresholdBits!=std::bit_cast<uint32_t>(threshold)||
       artifact.clusters!=plan.selectedClusters||artifact.requiredPages!=plan.requiredPages||
       artifact.requiredPages.size()!=artifact.requiredPageSha256.size())return false;
    try {
        return Detail::ScalarEqual(artifact.packed,decoded)&&
            Detail::ScalarHash(decoded)==artifact.scalarPackedSha256;
    }catch(...){return false;}
}
inline bool MatchesRequiredPage(const SelectedCutArtifact& artifact,uint32_t pageId,
    std::span<const uint8_t> encoded)
{
    for(size_t i=0;i<artifact.requiredPages.size();++i)
        if(artifact.requiredPages[i]==pageId)
            return i<artifact.requiredPageSha256.size()&&
                HierarchicalDiskDetail::Hash(encoded)==artifact.requiredPageSha256[i];
    return false;
}
} // namespace Poseidon::GeometryPages::RigidFinalSurface
