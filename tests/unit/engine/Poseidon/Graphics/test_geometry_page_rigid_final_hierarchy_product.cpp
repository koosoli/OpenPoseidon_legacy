#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRigidFinalHierarchyProduct.hpp>
#include <limits>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
// Source-compatible private synthetic DTO only. This is NOT a successful live
// Shape/material/Init inspection and provides no installed-source authority.
RigidOdol7FinalExport SyntheticFinalExport()
{
    RigidOdol7FinalExport result;result.sourceAdmissionEpoch=7;result.ownerEpoch=9;
    result.modelBirth=11;result.shapeQueryRevision=13;result.actualNoShadow=true;
    result.finalLevels={1,0};result.sourceLods={1,0};result.resolutions={5,2.5f};
    auto& source=result.actual.source;source.sourceSha256[0]=91;
    source.geometryOptions=0x4f3746494e414c31ull;source.materialOptions=0x2000ull|(1ull<<32);
    source.vertexLayout=sizeof(SVertex);source.materialMapping=1;source.coarseRepresentation=1;source.fineRepresentation=0;
    constexpr uint32_t side=17;
    auto& fine=result.actual.fine;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const float a=float(x)/16,b=float(y)/16,z=.9f*(1-a*a)*(1-b*b);
        SVertex v{};v.pos=Vector3P(a*4,b*4,z);v.norm=Vector3P(0,0,1);
        v.tangent=Vector3P(1,0,0);v.binormal=Vector3P(0,1,0);v.t0={a*a,b*b};v.t1={-0.f,b};
        fine.vertices.push_back(v);fine.positions.push_back({a*4,b*4,z});
    }
    for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        fine.indices.insert(fine.indices.end(),{a,b,d,a,d,c});fine.materials.insert(fine.materials.end(),2,0);
    }
    // A coincident distinct vertex with an authored UV seam; preserve its ID.
    fine.vertices.push_back(fine.vertices.front());fine.positions.push_back(fine.positions.front());
    fine.vertices.back().t0={7,7};fine.indices.front()=uint32_t(fine.vertices.size()-1);
    result.actual.coarse=fine;result.actual.coarse.indices.resize(144*3);result.actual.coarse.materials.resize(144);
    return result;
}
RigidFinalHierarchyProduct Product(const RigidOdol7FinalExport& source)
{
    RigidFinalHierarchyProduct result;
    REQUIRE(BuildRigidFinalHierarchyProduct(source,result)==RigidFinalHierarchyStatus::Produced);
    return result;
}
std::span<const uint8_t> PageBytes(const RigidFinalHierarchyProduct& product,uint32_t page)
{
    const auto& range=product.manifest->pages.at(page);
    return std::span<const uint8_t>(product.image->bytes).subspan(size_t(range.offset),range.bytes);
}
}
TEST_CASE("Final Shape producer preserves actual export associations and independent scalar pages",
          "[geometry-page-rigid-final-hierarchy]")
{
    const auto source=SyntheticFinalExport();const auto product=Product(source);
    REQUIRE(product.image);REQUIRE(product.manifest);
    REQUIRE(product.expectedSource==source.actual.source);
    REQUIRE(product.image->identity.source==source.actual.source);
    REQUIRE(product.manifest->metadata.identity==product.image->identity);
    REQUIRE(product.sourceAdmissionEpoch==7);REQUIRE(product.ownerEpoch==9);
    REQUIRE(product.modelBirth==11);REQUIRE(product.shapeQueryRevision==13);
    REQUIRE(product.actualNoShadow);REQUIRE(product.requiresOriginalOtherPasses);
    REQUIRE(product.fineTriangleSetExact);
    REQUIRE(product.manifest->metadataBytes<=32768);REQUIRE(product.manifest->pages.size()<=64);
    REQUIRE(product.fileSha256==HierarchicalDiskDetail::Hash(product.image->bytes));
    REQUIRE(product.knownSourceCapacityBytes<=128*1024);
    REQUIRE(product.knownRetainedCapacityBytes<=product.knownObservedCapacityHighWater);
    REQUIRE(product.knownObservedCapacityHighWater<=2*1024*1024);
    bool seam=false;uint64_t next=product.manifest->metadataBytes;
    for(uint32_t p=0;p<product.manifest->pages.size();++p) {
        const auto& range=product.manifest->pages[p];REQUIRE(range.offset==next);
        REQUIRE(range.bytes<=65536);next+=range.bytes;
        HierarchicalPage decoded;
        REQUIRE(DecodeHierarchicalDiskPage(*product.manifest,p,PageBytes(product,p),decoded)==HierarchicalDiskStatus::Decoded);
        REQUIRE(HierarchicalOriginalOutside::ExactOriginalFinePageVertices(decoded,source.actual.fine.vertices));
        for(const auto& c:decoded.clusters) {
            REQUIRE(c.material==0);
            for(const auto& v:c.vertices)if(v.t0.u==7&&v.t0.v==7)seam=true;
        }
        for(const auto& stub:product.manifest->metadata.pages[p].clusters) {
            REQUIRE(stub.vertices.empty());REQUIRE(stub.indices.empty());
        }
    }
    REQUIRE(seam);REQUIRE(next==product.image->bytes.size());
}
TEST_CASE("Actual baked final-export graph retains roots while fine page reads are partial or corrupt",
          "[geometry-page-rigid-final-hierarchy]")
{
    const auto source=SyntheticFinalExport();const auto product=Product(source);
    const auto& manifest=*product.manifest;const auto& package=manifest.metadata;
    std::vector<uint8_t> resident(package.pages.size());HierarchicalCutPlan plan;
    REQUIRE(PlanHierarchicalCut(package,0,{product.image->identity,resident},plan));
    REQUIRE(plan.state==HierarchicalCutState::MissingRoots);
    for(auto root:package.rootPages) {
        HierarchicalPage decoded;
        REQUIRE(DecodeHierarchicalDiskPage(manifest,root,PageBytes(product,root),decoded)==HierarchicalDiskStatus::Decoded);
        REQUIRE(HierarchicalOriginalOutside::ExactOriginalFinePageVertices(decoded,source.actual.fine.vertices));
        resident[root]=1;
    }
    REQUIRE(PlanHierarchicalCut(package,0,{product.image->identity,resident},plan));
    REQUIRE(plan.state==HierarchicalCutState::RootFallback);
    REQUIRE(plan.selectedClusters==package.rootClusters);REQUIRE_FALSE(plan.missingPages.empty());
    const uint32_t missing=plan.missingPages.front();auto bytes=PageBytes(product,missing);
    std::vector<uint8_t> corrupt(bytes.begin(),bytes.end());corrupt.back()^=1;
    HierarchicalPage retained;retained.group=UINT32_MAX;
    REQUIRE(DecodeHierarchicalDiskPage(manifest,missing,corrupt,retained)==HierarchicalDiskStatus::Invalid);
    REQUIRE(retained.group==UINT32_MAX);
    REQUIRE(PlanHierarchicalCut(package,0,{product.image->identity,resident},plan));
    REQUIRE(plan.selectedClusters==package.rootClusters);
    for(auto missingPage:plan.missingPages) {
        HierarchicalPage decoded;
        REQUIRE(DecodeHierarchicalDiskPage(manifest,missingPage,PageBytes(product,missingPage),decoded)==HierarchicalDiskStatus::Decoded);
        resident[missingPage]=1;
    }
    REQUIRE(PlanHierarchicalCut(package,0,{product.image->identity,resident},plan));
    REQUIRE(plan.state==HierarchicalCutState::RequestedCut);
    REQUIRE_FALSE(plan.selectedClusters.empty());
}
TEST_CASE("Final-export producer refuses invalid associations and malformed payload transactionally",
          "[geometry-page-rigid-final-hierarchy]")
{
    const auto source=SyntheticFinalExport();auto retained=Product(source);
    const auto oldImage=retained.image;const auto oldHash=retained.fileSha256;
    for(int variant=0;variant<10;++variant) {
        auto changed=source;
        switch(variant) {
            case 0:changed.requiresOriginalOtherPasses=false;break;
            case 1:changed.sourceAdmissionEpoch=0;break;
            case 2:changed.modelBirth=UINT64_MAX;break;
            case 3:changed.shapeQueryRevision=0;break;
            case 4:changed.actual.source.geometryOptions=1;break;
            case 5:changed.actualNoShadow=false;break;
            case 6:changed.sourceLods[0]=0;break;
            case 7:changed.actual.fine.vertices[0].t0.u=std::numeric_limits<float>::quiet_NaN();break;
            case 8:changed.actual.fine.positions[0].x+=1;break;
            case 9:changed.actual.coarse.materials[0]=1;break;
        }
        REQUIRE(BuildRigidFinalHierarchyProduct(changed,retained)==RigidFinalHierarchyStatus::Invalid);
        REQUIRE(retained.image==oldImage);REQUIRE(retained.fileSha256==oldHash);
    }
}
TEST_CASE("Final-export producer checks source, output and observed phase capacities",
          "[geometry-page-rigid-final-hierarchy]")
{
    const auto source=SyntheticFinalExport();auto retained=Product(source);const auto oldImage=retained.image;
    for(int variant=0;variant<5;++variant) {
        RigidFinalHierarchyLimits limits;
        switch(variant) {
            case 0:limits.sourceCapacityBytes=1;break;
            case 1:limits.encodedCapacityBytes=1;break;
            case 2:limits.observedCapacityBytes=retained.knownSourceCapacityBytes+1;break;
            case 3:limits.clusters=1;break;
            case 4:limits.pageBytes=1024;break;
        }
        REQUIRE(BuildRigidFinalHierarchyProduct(source,retained,limits)==RigidFinalHierarchyStatus::Capacity);
        REQUIRE(retained.image==oldImage);
    }
    RigidFinalHierarchyLimits invalid;invalid.pageBytes=65537;
    REQUIRE(BuildRigidFinalHierarchyProduct(source,retained,invalid)==RigidFinalHierarchyStatus::Invalid);
    REQUIRE(retained.image==oldImage);
}
TEST_CASE("Repeated finalized payload baking is deterministic without certifying runtime provenance",
          "[geometry-page-rigid-final-hierarchy]")
{
    const auto source=SyntheticFinalExport();const auto a=Product(source),b=Product(source);
    REQUIRE(a.image->bytes==b.image->bytes);REQUIRE(a.fileSha256==b.fileSha256);
    REQUIRE(a.image->identity==b.image->identity);
    auto changed=source;changed.ownerEpoch=19;changed.modelBirth=21;
    const auto c=Product(changed);
    REQUIRE(c.image->bytes==a.image->bytes);REQUIRE(c.ownerEpoch==19);REQUIRE(c.modelBirth==21);
    // Resource births bind the caller transaction, not the reusable disk bytes.
    REQUIRE(c.expectedSource==a.expectedSource);
}
TEST_CASE("Terminal final-export pages remain useful residency when root and fine are the same cut",
          "[geometry-page-rigid-final-hierarchy]")
{
    auto source=SyntheticFinalExport();ExportedMesh triangle;
    for(auto id:{0u,1u,17u}) {
        triangle.vertices.push_back(source.actual.fine.vertices[id]);
        triangle.positions.push_back(source.actual.fine.positions[id]);
    }
    // Keep a signed-zero UV and an authored seam-like UV to exercise immutable
    // scalar records even when the baker has nothing useful to simplify.
    triangle.vertices[0].t0={7,7};triangle.vertices[1].t1.u=-0.f;
    triangle.indices={0,1,2};triangle.materials={0};
    source.actual.fine=triangle;source.actual.coarse=triangle;
    const auto fineBefore=source.actual.fine.vertices;
    const auto product=Product(source);const auto& package=product.manifest->metadata;
    REQUIRE(product.fineTriangleSetExact);
    REQUIRE(package.pages.size()==1);REQUIRE(package.clusters.size()==1);
    REQUIRE(package.rootPages==std::vector<uint32_t>{0});
    REQUIRE(package.rootClusters==std::vector<uint32_t>{0});
    HierarchicalPage decoded;
    REQUIRE(DecodeHierarchicalDiskPage(*product.manifest,0,PageBytes(product,0),decoded)==HierarchicalDiskStatus::Decoded);
    REQUIRE(HierarchicalOriginalOutside::ExactOriginalFinePageVertices(decoded,fineBefore));
    REQUIRE(HierarchicalOriginalOutside::ExactOriginalFinePageVertices(decoded,source.actual.fine.vertices));
    const auto& cluster=decoded.clusters[0];
    const auto signedZero=std::find(cluster.originalVertexIds.begin(),cluster.originalVertexIds.end(),1u);
    REQUIRE(signedZero!=cluster.originalVertexIds.end());
    REQUIRE(std::bit_cast<uint32_t>(cluster.vertices[size_t(signedZero-cluster.originalVertexIds.begin())].t1.u)==
        std::bit_cast<uint32_t>(-0.f));
    REQUIRE(source.actual.fine.indices==triangle.indices);
    std::array<uint8_t,1> resident{0};HierarchicalCutPlan missing;
    REQUIRE(PlanHierarchicalCut(package,0,{product.image->identity,resident},missing));
    REQUIRE(missing.state==HierarchicalCutState::MissingRoots);
    resident[0]=1;HierarchicalCutPlan fine,root;
    REQUIRE(PlanHierarchicalCut(package,0,{product.image->identity,resident},fine));
    REQUIRE(PlanHierarchicalCut(package,100,{product.image->identity,resident},root));
    REQUIRE(fine.state==HierarchicalCutState::RequestedCut);
    REQUIRE(root.state==HierarchicalCutState::RequestedCut);
    REQUIRE(fine.selectedClusters==root.selectedClusters);
    REQUIRE(fine.requiredPages==root.requiredPages);
    REQUIRE(fine.missingPages.empty());
}

TEST_CASE("Fine source triangle parity preserves winding, duplicate multiplicities and seam IDs",
          "[geometry-page-rigid-final-hierarchy]")
{
    const auto source=SyntheticFinalExport();ExportedMesh fine;
    for(auto id:{0u,1u,17u,18u})fine.vertices.push_back(source.actual.fine.vertices[id]);
    fine.indices={0,1,2,0,1,2,1,3,2}; // an authored duplicate must occur exactly twice
    HierarchicalPage page;HierarchicalPayload cluster;
    cluster.cluster=3;cluster.material=0;cluster.vertices=fine.vertices;
    cluster.originalVertexIds={0,1,2,3};
    // Same oriented triangles, different order and cyclic rotations.
    cluster.indices={3,2,1,1,2,0,2,0,1};page.clusters.push_back(cluster);
    const std::array<uint32_t,1> selected{3};
    RigidFinalHierarchyDetail::FineTriangleParity exact;
    REQUIRE(exact.Prepare(fine,selected));REQUIRE(exact.Add(page));REQUIRE(exact.Finish());
    REQUIRE(exact.KnownBytes()>=exact.ReservationBytes(3));
    for(int variant=0;variant<8;++variant) {
        auto changed=page;
        switch(variant) {
            case 0:std::swap(changed.clusters[0].indices[0],changed.clusters[0].indices[1]);break; // winding
            case 1:changed.clusters[0].indices.resize(6);break; // missing duplicate
            case 2:changed.clusters[0].indices={0,1,2,1,3,2,1,3,2};break; // wrong duplicate multiplicity
            case 3:changed.clusters[0].indices[0]=4;break; // invalid local ID
            case 4:changed.clusters[0].originalVertexIds[3]=4;break; // invalid original ID
            case 5:changed.clusters[0].originalVertexIds[3]=2;break; // aliased original IDs
            case 6:changed.clusters[0].cluster=4;break; // required cluster absent
            case 7:changed.clusters[0].indices.push_back(0);break; // partial triangle
        }
        RigidFinalHierarchyDetail::FineTriangleParity wrong;
        REQUIRE(wrong.Prepare(fine,selected));const bool appended=wrong.Add(changed);
        REQUIRE_FALSE((appended&&wrong.Finish()));
    }
    RigidFinalHierarchyDetail::FineTriangleParity repeated;
    REQUIRE(repeated.Prepare(fine,selected));REQUIRE(repeated.Add(page));
    REQUIRE_FALSE(repeated.Add(page));REQUIRE_FALSE(repeated.Finish());
}

TEST_CASE("Fine source parity cannot substitute a coincident authored seam ID or invent source indices",
          "[geometry-page-rigid-final-hierarchy]")
{
    auto source=SyntheticFinalExport();auto& fine=source.actual.fine;
    fine.indices={uint32_t(fine.vertices.size()-1),1,17};
    HierarchicalPage page;HierarchicalPayload cluster;
    cluster.cluster=0;cluster.originalVertexIds={0,1,17};
    for(auto id:cluster.originalVertexIds)cluster.vertices.push_back(fine.vertices[id]);
    cluster.indices={0,1,2};page.clusters.push_back(cluster);
    const std::array<uint32_t,1> selected{0};
    RigidFinalHierarchyDetail::FineTriangleParity seam;
    REQUIRE(seam.Prepare(fine,selected));REQUIRE(seam.Add(page));REQUIRE_FALSE(seam.Finish());
    fine.indices[0]=uint32_t(fine.vertices.size());
    RigidFinalHierarchyDetail::FineTriangleParity outside;
    REQUIRE_FALSE(outside.Prepare(fine,selected));REQUIRE_FALSE(outside.Finish());
    fine.indices={0,1,17};const std::array<uint32_t,2> repeated{0,0};
    RigidFinalHierarchyDetail::FineTriangleParity duplicateSelection;
    REQUIRE_FALSE(duplicateSelection.Prepare(fine,repeated));
}
