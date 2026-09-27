#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageClodRamPackage.hpp>
#include <cstring>
#include <Poseidon/Graphics/Rendering/AuthoredGeometryPageWorker.hpp>
#include <chrono>
#include <thread>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
struct OriginalCuts
{
    ClodBake bake;ClodDecodedCut coarse,fine;float coarseThreshold=0,fineThreshold=0;
};
OriginalCuts CurvedCuts(bool seam=false)
{
    ShapeExport source;source.source.sourceSha256[0]=39;source.source.vertexLayout=sizeof(SVertex);
    source.source.materialMapping=1;source.source.coarseRepresentation=0;source.source.fineRepresentation=1;
    constexpr uint32_t side=9;
    for(uint32_t y=0;y<side;++y) for(uint32_t x=0;x<side;++x) {
        const float z=.08f*x*x+.035f*y*y;SVertex vertex{};
        vertex.pos=Vector3P(float(x),float(y),z);vertex.norm=Vector3P(0,0,1);
        vertex.tangent=Vector3P(1,0,0);vertex.binormal=Vector3P(0,1,0);
        vertex.t0={float(x*x)/64,float(y*y)/64};vertex.t1=vertex.t0;
        source.fine.vertices.push_back(vertex);source.fine.positions.push_back({float(x),float(y),z});
    }
    for(uint32_t y=0;y<side-1;++y) for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        source.fine.indices.insert(source.fine.indices.end(),{a,b,d,a,d,c});
        source.fine.materials.insert(source.fine.materials.end(),2,4);
    }
    if(seam) {
        source.fine.vertices.push_back(source.fine.vertices[0]);source.fine.positions.push_back(source.fine.positions[0]);
        source.fine.vertices.back().t0={7,7};source.fine.indices[0]=uint32_t(source.fine.vertices.size()-1);
    }
    OriginalCuts result;REQUIRE(BakeClodPilot(source,true,result.bake)==ClodBakeStatus::Baked);
    float least=FLT_MAX,most=0;
    for(const auto& group:result.bake.groups) if(group.simplified.error>0 && group.simplified.error<FLT_MAX && std::isfinite(group.simplified.error)) {
        least=std::min(least,group.simplified.error);most=std::max(most,group.simplified.error);
    }
    REQUIRE(least<FLT_MAX);REQUIRE(most>0);
    result.fineThreshold=std::nextafter(least,0.f);result.coarseThreshold=std::nextafter(most,std::numeric_limits<float>::infinity());
    ClodCut coarse,fine;REQUIRE(SelectClodCut(result.bake,result.coarseThreshold,coarse));
    REQUIRE(SelectClodCut(result.bake,result.fineThreshold,fine));REQUIRE(coarse.clusters!=fine.clusters);
    REQUIRE(DecodeClodCut(result.bake,result.coarseThreshold,coarse,result.coarse)==ClodCutDecodeStatus::Decoded);
    REQUIRE(DecodeClodCut(result.bake,result.fineThreshold,fine,result.fine)==ClodCutDecodeStatus::Decoded);
    return result;
}
ClodRamStatus BuildCuts(const OriginalCuts& cuts,ClodRamPackage& result,ClodRamLimits limits={})
{return BuildClodRamPackage(cuts.bake,cuts.coarseThreshold,cuts.fineThreshold,cuts.coarse,cuts.fine,result,limits);}
void CheckFlat(const ClodDecodedCut& input,const ExportedMesh& flat)
{
    size_t v=0,i=0,t=0;
    for(const auto& cluster:input.clusters) {
        const auto base=v;
        for(const auto& vertex:cluster.mesh.vertices) {
            REQUIRE(v<flat.vertices.size());REQUIRE(std::memcmp(&flat.vertices[v],&vertex,sizeof(SVertex))==0);++v;
        }
        for(auto index:cluster.mesh.indices) {REQUIRE(flat.indices[i]==base+index);++i;}
        for(size_t k=0;k<cluster.mesh.indices.size()/3;++k) {REQUIRE(flat.materials[t]==cluster.mesh.material);++t;}
    }
    REQUIRE(v==flat.vertices.size());REQUIRE(i==flat.indices.size());REQUIRE(t==flat.materials.size());
}
void CheckPages(const ResidentRepresentation& decoded,const ExportedMesh& flat)
{
    size_t index=0;
    for(const auto& page:decoded.pages) for(const auto& cluster:page.clusters) for(auto local:cluster.indices) {
        REQUIRE(local<cluster.vertices.size());REQUIRE(index<flat.indices.size());
        REQUIRE(std::memcmp(&cluster.vertices[local],&flat.vertices[flat.indices[index]],sizeof(SVertex))==0);++index;
    }
    REQUIRE(index==flat.indices.size());REQUIRE(decoded.indices==index);
}
}
TEST_CASE("CLOD RAM adapter pages nontrivial paired cuts with exact selected geometry", "[geometry-clod-ram]")
{
    const auto cuts=CurvedCuts();ClodRamPackage result;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Built);
    REQUIRE(result.originalSource==cuts.bake.source);REQUIRE(result.selectedGeometry.source!=cuts.bake.source);
    REQUIRE(result.package.Identity().source==result.selectedGeometry.source);
    REQUIRE(result.knownCapacityBytes==ClodRamKnownBytes(result));REQUIRE(result.knownCapacityBytes<=128*1024);
    REQUIRE(result.package.pages.size()<=8);REQUIRE(result.package.clusters.size()<=64);
    REQUIRE(result.coarseClusters!=result.fineClusters);
    REQUIRE(result.selectedGeometry.coarse.indices.size()<result.selectedGeometry.fine.indices.size());
    CheckFlat(cuts.coarse,result.selectedGeometry.coarse);CheckFlat(cuts.fine,result.selectedGeometry.fine);
    for(auto frontier:{Frontier::Coarse,Frontier::Fine}) {
        ResidentRepresentation decoded;RepresentationDecodeLimits limits;limits.pages=8;limits.clusters=64;
        limits.vertexRecords=1024;limits.indices=4096;limits.decodedBytes=128*1024;limits.serializedBytes=128*1024;
        REQUIRE(DecodeResidentRepresentation(result.package,result.package.Identity(),frontier,result.selectedGeometry,decoded,limits)==DecodeStatus::Decoded);
        CheckPages(decoded,frontier==Frontier::Coarse?result.selectedGeometry.coarse:result.selectedGeometry.fine);
    }
    ClodRamPackage repeated;REQUIRE(BuildCuts(cuts,repeated)==ClodRamStatus::Built);
    REQUIRE(repeated.package.Identity()==result.package.Identity());
    auto shifted=cuts;shifted.coarseThreshold=std::nextafter(shifted.coarseThreshold,std::numeric_limits<float>::infinity());
    shifted.coarse.threshold=shifted.coarseThreshold;REQUIRE(BuildCuts(shifted,repeated)==ClodRamStatus::Built);
    REQUIRE(repeated.coarseClusters==result.coarseClusters);REQUIRE(repeated.package.Identity()!=result.package.Identity());
    auto changed=cuts;changed.bake.source.geometryOptions^=1;changed.coarse.source=changed.fine.source=changed.bake.source;
    REQUIRE(BuildCuts(changed,repeated)==ClodRamStatus::Built);REQUIRE(repeated.package.Identity()!=result.package.Identity());
}
TEST_CASE("CLOD RAM adapter keeps seam records and refuses mismatched snapshots transactionally", "[geometry-clod-ram]")
{
    auto cuts=CurvedCuts(true);ClodRamPackage result;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Built);
    bool seam=false;for(const auto& vertex:result.selectedGeometry.fine.vertices) if(vertex.t0.u==7 && vertex.t0.v==7) seam=true;
    REQUIRE(seam);CheckFlat(cuts.fine,result.selectedGeometry.fine);
    const auto identity=result.package.Identity();const auto known=result.knownCapacityBytes;
    SECTION("wrong original source") {cuts.fine.source.sourceSha256[0]^=1;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Invalid);}
    SECTION("wrong explicit threshold") {cuts.fine.threshold=cuts.coarseThreshold;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Invalid);}
    SECTION("wrong selected cluster") {cuts.fine.clusters.front().bakedCluster=UINT32_MAX;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Invalid);}
    SECTION("modified attributes") {cuts.fine.clusters.front().mesh.vertices.front().t1.u+=1;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Invalid);}
    SECTION("modified topology") {cuts.fine.clusters.front().mesh.indices.back()=UINT32_MAX;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Invalid);}
    SECTION("wrong material") {++cuts.fine.clusters.front().mesh.material;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Invalid);}
    SECTION("bounded vertices") {ClodRamLimits limits;limits.vertexRecordsPerCut=1;REQUIRE(BuildCuts(cuts,result,limits)==ClodRamStatus::Capacity);}
    SECTION("bounded indices") {ClodRamLimits limits;limits.indicesPerCut=1;REQUIRE(BuildCuts(cuts,result,limits)==ClodRamStatus::Capacity);}
    SECTION("bounded pages") {ClodRamLimits limits;limits.pages=1;REQUIRE(BuildCuts(cuts,result,limits)==ClodRamStatus::Capacity);}
    SECTION("bounded clusters") {ClodRamLimits limits;limits.clusters=1;REQUIRE(BuildCuts(cuts,result,limits)==ClodRamStatus::Capacity);}
    SECTION("bounded bytes") {ClodRamLimits limits;limits.knownSourceBytes=1;REQUIRE(BuildCuts(cuts,result,limits)==ClodRamStatus::Capacity);}
    SECTION("caps cannot be raised") {ClodRamLimits limits;limits.pages=9;REQUIRE(BuildCuts(cuts,result,limits)==ClodRamStatus::Invalid);}
    REQUIRE(result.package.Identity()==identity);REQUIRE(result.knownCapacityBytes==known);
}
TEST_CASE("CLOD RAM page decoding refuses malformed final pages and foreign identities without publication", "[geometry-clod-ram]")
{
    const auto cuts=CurvedCuts();ClodRamPackage result;REQUIRE(BuildCuts(cuts,result)==ClodRamStatus::Built);
    ResidentRepresentation decoded;REQUIRE(DecodeResidentRepresentation(result.package,result.package.Identity(),Frontier::Fine,result.selectedGeometry,decoded)==DecodeStatus::Decoded);
    const auto indices=decoded.indices;const auto pages=decoded.pages.size();const auto identity=decoded.identity;
    SECTION("truncated tail") {result.package.pages.back().bytes.pop_back();REQUIRE(DecodeResidentRepresentation(result.package,result.package.Identity(),Frontier::Fine,result.selectedGeometry,decoded)==DecodeStatus::Invalid);}
    SECTION("corrupt final payload") {result.package.pages.back().bytes.back()^=0xff;REQUIRE(DecodeResidentRepresentation(result.package,result.package.Identity(),Frontier::Fine,result.selectedGeometry,decoded)==DecodeStatus::Invalid);}
    SECTION("foreign identity") {auto wrong=result.package.Identity();wrong.source.sourceSha256[0]^=1;REQUIRE(DecodeResidentRepresentation(result.package,wrong,Frontier::Fine,result.selectedGeometry,decoded)==DecodeStatus::Invalid);}
    REQUIRE(decoded.indices==indices);REQUIRE(decoded.pages.size()==pages);REQUIRE(decoded.identity==identity);
}

namespace
{
std::shared_ptr<const AuthoredPageInput> SelectedClodInput(const OriginalCuts& cuts)
{
    ClodRamPackage ram;REQUIRE(BuildCuts(cuts,ram)==ClodRamStatus::Built);
    auto input=std::make_shared<AuthoredPageInput>();
    input->identity=ram.package.Identity();input->package=std::move(ram.package);
    input->original=std::move(ram.selectedGeometry); // Complete selected-cut reference, not raw fine triangles.
    input->knownCapacityBytes=AuthoredPageKnownBytes(*input);
    REQUIRE(input->knownCapacityBytes<=AuthoredPageInput::MaxSourceBytes);
    return input;
}
template<class Predicate> bool ClodWorkerUntil(Predicate predicate)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do {if(predicate()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    while(std::chrono::steady_clock::now()<deadline);
    return predicate();
}
struct ClodWorkerUnhold
{
    AuthoredPageWorker& worker;
    ~ClodWorkerUnhold() {worker.HoldBeforePublish(false);}
};
}
TEST_CASE("Actual authored worker decodes selected CLOD pages and retains cancelled source debt", "[geometry-clod-ram][geometry-clod-ram-worker]")
{
    const auto cuts=CurvedCuts(true);const auto input=SelectedClodInput(cuts);
    CheckFlat(cuts.fine,input->original.fine);
    AuthoredPageWorker worker;ClodWorkerUnhold release{worker};
    const auto reservation=input->knownCapacityBytes+AuthoredPageWorker::ResultReservation+AuthoredPageWorker::MetadataReservation;
    SECTION("successful real worker exact selected-cut bytes and topology") {
        REQUIRE(worker.Submit(31,301,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(ClodWorkerUntil([&]{return worker.Snapshot().ready==1;}));
        auto result=worker.Take(31,301);REQUIRE(result);
        REQUIRE(result->epoch==31);REQUIRE(result->request==301);REQUIRE(result->source==input);
        REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->decoded.identity==input->identity);
        CheckPages(result->decoded,input->original.fine);
        REQUIRE(worker.Snapshot().reservedBytes==reservation);
        result.reset();REQUIRE(ClodWorkerUntil([&]{return worker.Snapshot().liveJobs==0;}));
        REQUIRE(worker.Snapshot().reservedBytes==0);
    }
    SECTION("cancelled publication cannot lend old identity to replacement epoch") {
        worker.HoldBeforePublish(true);
        REQUIRE(worker.Submit(41,401,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(ClodWorkerUntil([&]{return worker.Snapshot().active==1;}));
        worker.Cancel(41);
        // The latch prevents publication/job exit, not a claimed exact per-page decode phase.
        REQUIRE_FALSE(worker.Take(41,401));REQUIRE(worker.Snapshot().liveJobs==1);
        REQUIRE(worker.Snapshot().reservedBytes==reservation);
        auto changed=cuts;changed.bake.source.materialOptions^=1;
        changed.coarse.source=changed.fine.source=changed.bake.source;
        const auto replacement=SelectedClodInput(changed);REQUIRE(replacement->identity!=input->identity);
        const auto replacementReservation=replacement->knownCapacityBytes+AuthoredPageWorker::ResultReservation+AuthoredPageWorker::MetadataReservation;
        REQUIRE(worker.Submit(42,402,replacement,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(worker.Snapshot().liveJobs==2);
        REQUIRE(worker.Snapshot().reservedBytes==reservation+replacementReservation);
        worker.HoldBeforePublish(false);
        REQUIRE(ClodWorkerUntil([&]{return worker.Snapshot().ready==1;}));
        auto result=worker.Take(42,402);REQUIRE(result);REQUIRE(result->status==DecodeStatus::Decoded);
        REQUIRE(result->epoch==42);REQUIRE(result->request==402);REQUIRE(result->source==replacement);
        REQUIRE(result->decoded.identity==replacement->identity);REQUIRE(result->decoded.identity!=input->identity);
        CheckPages(result->decoded,replacement->original.fine);
        REQUIRE(ClodWorkerUntil([&]{return worker.Snapshot().liveJobs==1;}));
        REQUIRE(worker.Snapshot().cancelled==1);REQUIRE(worker.Snapshot().reservedBytes==replacementReservation);
        result.reset();REQUIRE(ClodWorkerUntil([&]{return worker.Snapshot().liveJobs==0;}));
        REQUIRE(worker.Snapshot().reservedBytes==0);
    }
}
