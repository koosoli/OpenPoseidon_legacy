#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRigidFinalSurfaceCertificate.hpp>

using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace Cert=Poseidon::GeometryPages::RigidFinalSurface;

namespace
{
ExportedMesh Triangle(float height=0)
{
    ExportedMesh mesh;
    for(auto xyz:{Position{0,0,height},Position{100,0,height},Position{0,100,height}}) {
        SVertex vertex{};vertex.pos=Vector3P(xyz.x,xyz.y,xyz.z);
        mesh.vertices.push_back(vertex);mesh.positions.push_back(xyz);
    }
    mesh.indices={0,1,2};mesh.materials={0};return mesh;
}
ExportedMesh Square(bool oppositeDiagonal,float height=0)
{
    ExportedMesh mesh;
    for(auto xyz:{Position{0,0,height},Position{100,0,height},
                  Position{100,100,height},Position{0,100,height}}) {
        SVertex vertex{};vertex.pos=Vector3P(xyz.x,xyz.y,xyz.z);
        mesh.vertices.push_back(vertex);mesh.positions.push_back(xyz);
    }
    mesh.indices=oppositeDiagonal?std::vector<uint32_t>{0,1,3,1,2,3}:
        std::vector<uint32_t>{0,1,2,0,2,3};
    mesh.materials={0,0};return mesh;
}
// Actual-codec producer DTO, intentionally not a claim about a live Shape.
RigidOdol7FinalExport CurvedFinal()
{
    RigidOdol7FinalExport result;
    result.sourceAdmissionEpoch=7;result.ownerEpoch=9;result.modelBirth=11;result.shapeQueryRevision=13;
    result.actualNoShadow=true;result.finalLevels={1,0};result.sourceLods={1,0};result.resolutions={5,2.5f};
    auto& key=result.actual.source;key.sourceSha256[0]=91;
    key.geometryOptions=0x4f3746494e414c31ull;key.materialOptions=0x2000ull|(1ull<<32);
    key.vertexLayout=sizeof(SVertex);key.materialMapping=1;
    key.coarseRepresentation=1;key.fineRepresentation=0;
    constexpr uint32_t side=17;
    auto& fine=result.actual.fine;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const float a=float(x)/16,b=float(y)/16,z=.9f*(1-a*a)*(1-b*b);
        SVertex v{};v.pos=Vector3P(a*4,b*4,z);v.norm=Vector3P(0,0,1);
        v.tangent=Vector3P(1,0,0);v.binormal=Vector3P(0,1,0);
        v.t0={a*a,b*b};v.t1={-0.f,b};
        fine.vertices.push_back(v);fine.positions.push_back({a*4,b*4,z});
    }
    for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        fine.indices.insert(fine.indices.end(),{a,b,d,a,d,c});
        fine.materials.insert(fine.materials.end(),2,0);
    }
    fine.vertices.push_back(fine.vertices.front());fine.positions.push_back(fine.positions.front());
    fine.vertices.back().t0={7,7};fine.indices.front()=uint32_t(fine.vertices.size()-1);
    result.actual.coarse=fine;result.actual.coarse.indices.resize(144*3);
    result.actual.coarse.materials.resize(144);
    return result;
}
RigidFinalHierarchyProduct Product(const RigidOdol7FinalExport& source)
{
    RigidFinalHierarchyProduct result;
    REQUIRE(BuildRigidFinalHierarchyProduct(source,result)==RigidFinalHierarchyStatus::Produced);
    return result;
}
std::span<const uint8_t> PageBytes(const RigidFinalHierarchyProduct& product,uint32_t id)
{
    const auto& range=product.manifest->pages.at(id);
    return std::span<const uint8_t>(product.image->bytes).subspan(size_t(range.offset),range.bytes);
}
HierarchicalCutPlan Plan(const RigidFinalHierarchyProduct& product,float threshold)
{
    std::array<uint8_t,64> ready{};ready.fill(1);
    HierarchicalCutPlan plan;
    REQUIRE(PlanHierarchicalCut(product.manifest->metadata,threshold,
        {product.image->identity,std::span<const uint8_t>(ready.data(),product.manifest->pages.size())},plan));
    REQUIRE(plan.state==HierarchicalCutState::RequestedCut);
    return plan;
}
}

TEST_CASE("Closed target triangle witnesses distinguish surface error from triangle diameter",
          "[geometry-page-rigid-final-surface]")
{
    const auto original=Triangle();Cert::Certificate certificate;
    REQUIRE(Cert::Detail::Distance(original,original,{},certificate)==Cert::Status::Certified);
    CHECK(certificate.hausdorffUpper==0);
    const auto raised=Triangle(2);certificate={};
    REQUIRE(Cert::Detail::Distance(original,raised,{},certificate)==Cert::Status::Certified);
    CHECK(certificate.hausdorffUpper>=2);
    CHECK(certificate.hausdorffUpper<2.000001);
    // A bounded optional refinement may not turn incomplete work into a lower bound.
    certificate={};Cert::Limits budget;budget.distanceVisits=12;
    REQUIRE(Cert::Detail::Distance(original,raised,budget,certificate)==Cert::Status::Certified);
    CHECK(certificate.distanceVisits<=12);
    CHECK(certificate.hausdorffUpper>100);
    // Disconnected target triangles cannot use anchors from different components
    // to certify their intervening empty space as a surface.
    auto disconnected=Triangle();
    for(auto& v:disconnected.vertices)v.pos=Vector3P(v.pos.X()*.001f,v.pos.Y()*.001f,0);
    for(unsigned i=0;i<3;++i) {
        auto vertex=disconnected.vertices[i];vertex.pos=Vector3P(vertex.pos.X()+100,vertex.pos.Y(),0);
        disconnected.vertices.push_back(vertex);
    }
    disconnected.positions.clear();
    for(const auto& v:disconnected.vertices)disconnected.positions.push_back({v.pos.X(),v.pos.Y(),v.pos.Z()});
    disconnected.indices={0,1,2,3,4,5};disconnected.materials={0,0};certificate={};
    REQUIRE(Cert::Detail::Distance(original,disconnected,{},certificate)==Cert::Status::Certified);
    CHECK(certificate.rootToFine>99);
}

TEST_CASE("Dyadic surface witness tightens identical differently triangulated planes and preserves shifts",
          "[geometry-page-rigid-final-surface]")
{
    const auto first=Square(false),second=Square(true);
    Cert::Certificate same;
    REQUIRE(Cert::Detail::Distance(first,second,{},same)==Cert::Status::Certified);
    CHECK(same.hausdorffUpper>=0);
    CHECK(same.hausdorffUpper<1); // v2's vertex-only closed-triangle bound was >100.
    CHECK(same.distanceVisits<=Cert::Limits{}.distanceVisits);
    const auto raised=Square(true,2);
    Cert::Certificate shifted;
    REQUIRE(Cert::Detail::Distance(first,raised,{},shifted)==Cert::Status::Certified);
    CHECK(shifted.hausdorffUpper>=2);
    CHECK(shifted.hausdorffUpper<3);
    Cert::Detail::DyadicPoint refused{};
    CHECK_FALSE(Cert::Detail::Dyadic(first,0,3,1,1,refused)); // non-dyadic weights lack exact division
    // A stopped candidate or subdivision level must leave the already valid
    // global baseline, never publish a partly evaluated lower estimate.
    uint64_t visits=0;double baseline=1000;
    REQUIRE(Cert::Detail::RefineDirected(first,second,1,visits,baseline)==Cert::Status::Certified);
    CHECK(visits==1);
    CHECK(baseline==1000);
}

TEST_CASE("Actual decoded GHP Root and Fine have an immutable selected-surface witness",
          "[geometry-page-rigid-final-surface]")
{
    const auto actual=CurvedFinal();
    const auto product=Product(actual);
    Cert::Proof proof;
    REQUIRE(Cert::Build(actual,product,proof)==Cert::Status::Certified);
    REQUIRE(proof.certificate);REQUIRE(proof.root);REQUIRE(proof.fine);
    REQUIRE(proof.certificate->identity==product.image->identity);
    REQUIRE(proof.certificate->originalSource==actual.actual.source);
    REQUIRE(proof.certificate->selectedDescriptor.source==actual.actual.source);
    REQUIRE(proof.certificate->selectedDescriptor.packing.pageBytes==product.manifest->metadata.pageByteLimit);
    REQUIRE(proof.certificate->fileSha256==product.fileSha256);
    REQUIRE(proof.certificate->metadataSha256==product.image->metadataSha256);
    REQUIRE(proof.certificate->sourceAdmissionEpoch==7);
    REQUIRE(proof.certificate->ownerEpoch==9);
    REQUIRE(proof.certificate->modelBirth==11);
    REQUIRE(proof.certificate->shapeQueryRevision==13);
    REQUIRE(proof.certificate->fineTriangles==512);
    REQUIRE(proof.certificate->rootTriangles==proof.root->packed.indices.size()/3);
    REQUIRE(proof.certificate->fineTriangles==proof.fine->packed.indices.size()/3);
    REQUIRE(proof.certificate->fineToRoot>=0);
    REQUIRE(proof.certificate->rootToFine>=0);
    REQUIRE(proof.certificate->hausdorffUpper==
        std::max(proof.certificate->fineToRoot,proof.certificate->rootToFine));
    for(const auto* artifact:{proof.root.get(),proof.fine.get()})
        for(const auto& v:artifact->packed.vertices) {
            REQUIRE(v.pos.X()>=proof.certificate->minimum[0]);
            REQUIRE(v.pos.X()<=proof.certificate->maximum[0]);
            REQUIRE(v.pos.Y()>=proof.certificate->minimum[1]);
            REQUIRE(v.pos.Y()<=proof.certificate->maximum[1]);
            REQUIRE(v.pos.Z()>=proof.certificate->minimum[2]);
            REQUIRE(v.pos.Z()<=proof.certificate->maximum[2]);
        }
    REQUIRE(proof.certificate->distanceVisits>0);
    REQUIRE(proof.knownObservedCapacityHighWater<=4*1024*1024);
    const float rootThreshold=std::nextafter(FLT_MAX,0.f);
    const auto root=Plan(product,rootThreshold),fine=Plan(product,0);
    REQUIRE(Cert::MatchesSelected(*proof.root,root,rootThreshold,proof.root->packed));
    REQUIRE(Cert::MatchesSelected(*proof.fine,fine,0,proof.fine->packed));
    REQUIRE(proof.root->clusters==root.selectedClusters);
    REQUIRE(proof.fine->clusters==fine.selectedClusters);
    REQUIRE(proof.certificate->rootCutCount==root.selectedClusters.size());
    REQUIRE(proof.certificate->fineCutCount==fine.selectedClusters.size());
    REQUIRE(std::equal(root.selectedClusters.begin(),root.selectedClusters.end(),proof.certificate->rootCut.begin()));
    REQUIRE(std::equal(fine.selectedClusters.begin(),fine.selectedClusters.end(),proof.certificate->fineCut.begin()));
    for(const auto* artifact:{proof.root.get(),proof.fine.get()})
        for(auto id:artifact->requiredPages)
            REQUIRE(Cert::MatchesRequiredPage(*artifact,id,PageBytes(product,id)));
}

TEST_CASE("Selected page and scalar mutations cannot reuse a final-surface witness",
          "[geometry-page-rigid-final-surface]")
{
    const auto actual=CurvedFinal();
    const auto product=Product(actual);
    Cert::Proof proof;REQUIRE(Cert::Build(actual,product,proof)==Cert::Status::Certified);
    const auto finePlan=Plan(product,0);
    auto changed=proof.fine->packed;
    changed.vertices[0].t0.u+=1;
    REQUIRE_FALSE(Cert::MatchesSelected(*proof.fine,finePlan,0,changed));
    changed=proof.fine->packed;changed.indices[0]=changed.indices[1];
    REQUIRE_FALSE(Cert::MatchesSelected(*proof.fine,finePlan,0,changed));
    REQUIRE_FALSE(Cert::MatchesSelected(*proof.fine,finePlan,1,proof.fine->packed));
    REQUIRE_FALSE(Cert::MatchesSelected(*proof.root,finePlan,0,proof.root->packed));
    auto encoded=PageBytes(product,proof.fine->requiredPages.back());
    std::vector<uint8_t> corrupt(encoded.begin(),encoded.end());corrupt.back()^=1;
    REQUIRE_FALSE(Cert::MatchesRequiredPage(*proof.fine,proof.fine->requiredPages.back(),corrupt));
    REQUIRE_FALSE(Cert::MatchesRequiredPage(*proof.fine,UINT32_MAX,encoded));
}

TEST_CASE("Final-surface build rejects altered authority and bounded work transactionally",
          "[geometry-page-rigid-final-surface]")
{
    const auto actual=CurvedFinal();
    const auto product=Product(actual);
    Cert::Proof retained;REQUIRE(Cert::Build(actual,product,retained)==Cert::Status::Certified);
    const auto prior=retained.certificate;
    auto wrongProduct=product;wrongProduct.sourceAdmissionEpoch++;
    REQUIRE(Cert::Build(actual,wrongProduct,retained)==Cert::Status::Invalid);
    REQUIRE(retained.certificate==prior);
    wrongProduct=product;wrongProduct.fileSha256[0]^=1;
    REQUIRE(Cert::Build(actual,wrongProduct,retained)==Cert::Status::Invalid);
    REQUIRE(retained.certificate==prior);
    auto wrongActual=actual;wrongActual.actual.fine.vertices[0].t0.u+=1;
    REQUIRE(Cert::Build(wrongActual,product,retained)==Cert::Status::Invalid);
    REQUIRE(retained.certificate==prior);
    auto image=std::make_shared<HierarchicalDiskImage>(*product.image);
    image->bytes.back()^=1;wrongProduct=product;wrongProduct.image=image;
    REQUIRE(Cert::Build(actual,wrongProduct,retained)==Cert::Status::Invalid);
    REQUIRE(retained.certificate==prior);
    Cert::Limits tiny;tiny.distanceVisits=1;
    REQUIRE(Cert::Build(actual,product,retained,tiny)==Cert::Status::Capacity);
    REQUIRE(retained.certificate==prior);
    tiny=Cert::Limits{};tiny.selectedCapacityBytes=1;
    REQUIRE(Cert::Build(actual,product,retained,tiny)==Cert::Status::Capacity);
    REQUIRE(retained.certificate==prior);
}

TEST_CASE("Selected-surface numeric witness refuses degenerate and unsupported geometry",
          "[geometry-page-rigid-final-surface]")
{
    const auto actual=CurvedFinal();
    const auto product=Product(actual);
    Cert::Proof proof;REQUIRE(Cert::Build(actual,product,proof)==Cert::Status::Certified);
    auto collapsed=proof.fine->packed;
    for(size_t i=0;i<collapsed.vertices.size();++i) {
        collapsed.vertices[i].pos=Vector3P(0,0,0);
        collapsed.positions[i]={0,0,0};
    }
    Cert::Certificate result;
    REQUIRE(Cert::Detail::Distance(collapsed,collapsed,{},result)==Cert::Status::Unsupported);
    auto unbounded=proof.fine->packed;
    unbounded.vertices[unbounded.indices[0]].pos=Vector3P(10001,0,0);
    unbounded.positions[unbounded.indices[0]]={10001,0,0};
    REQUIRE(Cert::Detail::Distance(unbounded,unbounded,{},result)==Cert::Status::Unsupported);
}
