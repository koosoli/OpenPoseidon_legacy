#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/AuthoredGeometryPageWorker.hpp>
#include <chrono>
#include <cstring>
#include <thread>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
std::shared_ptr<AuthoredPageInput> OriginalInput()
{
    auto input=std::make_shared<AuthoredPageInput>();
    auto& dto=input->original;dto.source.sourceSha256[0]=21;
    dto.source.vertexLayout=sizeof(SVertex);dto.source.materialMapping=1;
    dto.source.coarseRepresentation=0;dto.source.fineRepresentation=1;
    for(auto* mesh:{&dto.coarse,&dto.fine}) {
        mesh->positions={{0,0,0},{2,0,0},{0,2,0},{2,2,0}};mesh->vertices.resize(4);
        for(size_t v=0;v<4;++v) {
            const auto p=mesh->positions[v];auto& vertex=mesh->vertices[v];
            vertex.pos=Vector3P(p.x,p.y,p.z);vertex.norm=Vector3P(0,0,1);
            vertex.t0={float(v),0};vertex.t1={0,float(v)};vertex.conform=0;
            vertex.tangent=Vector3P(1,0,0);vertex.binormal=Vector3P(0,1,0);
        }
    }
    dto.coarse.indices={0,1,2};dto.coarse.materials={7};
    dto.fine.indices={0,1,2,1,3,2};dto.fine.materials={7,8};
    Limits limits;limits.clusterTriangles=1;limits.pageBytes=uint32_t(16+48+3*(4+sizeof(SVertex))+3*4);
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,input->package,limits)==Status::Built);
    input->identity=input->package.Identity();input->knownCapacityBytes=AuthoredPageKnownBytes(*input);
    REQUIRE(input->knownCapacityBytes<=AuthoredPageInput::MaxSourceBytes);
    return input;
}
template<class Predicate> bool Until(Predicate predicate)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do {if(predicate()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    while(std::chrono::steady_clock::now()<end);
    return predicate();
}
void CheckOriginal(const ResidentRepresentation& decoded,const ExportedMesh& source)
{
    REQUIRE(decoded.representation==Frontier::Fine);REQUIRE(decoded.indices==source.indices.size());
    size_t triangle=0;
    for(const auto& page:decoded.pages) for(const auto& cluster:page.clusters) {
        REQUIRE(cluster.firstTriangle==triangle);
        for(size_t i=0;i<cluster.indices.size();++i) {
            REQUIRE(cluster.indices[i]<cluster.vertices.size());
            const auto& actual=cluster.vertices[cluster.indices[i]];
            const auto& original=source.vertices[source.indices[triangle*3+i]];
            REQUIRE(std::memcmp(&actual,&original,sizeof(SVertex))==0);
        }
        for(size_t t=0;t<cluster.indices.size()/3;++t) REQUIRE(cluster.material==source.materials[triangle+t]);
        triangle+=cluster.indices.size()/3;
    }
    REQUIRE(triangle==source.materials.size());
}
}
TEST_CASE("Authored page worker holds cancellation debt until exit and rejects stale publication", "[geometry-page-worker]")
{
    auto input=OriginalInput();AuthoredPageWorker worker;worker.HoldBeforePublish(true);
    REQUIRE(worker.Submit(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().active==1;}));
    REQUIRE(worker.Submit(1,11,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Duplicate);
    REQUIRE(worker.Submit(2,22,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(worker.Submit(3,33,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
    const auto held=worker.Snapshot();REQUIRE(held.liveJobs==2);REQUIRE(held.reservedBytes>input->knownCapacityBytes);
    worker.Cancel(1);REQUIRE_FALSE(worker.Take(1,11));
    REQUIRE(worker.Snapshot().active==1);REQUIRE(worker.Snapshot().liveJobs==2);
    worker.HoldBeforePublish(false);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));
    auto result=worker.Take(2,22);REQUIRE(result);REQUIRE(result->epoch==2);REQUIRE(result->request==22);
    REQUIRE(result->status==DecodeStatus::Decoded);REQUIRE(result->source==input);
    CheckOriginal(result->decoded,input->original.fine);
    REQUIRE(worker.Snapshot().cancelled>=1);REQUIRE(worker.Snapshot().liveJobs==1);
    result.reset();REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));
    REQUIRE(worker.Snapshot().reservedBytes==0);
}
TEST_CASE("Authored page result ownership bounds new jobs and a wrong epoch cannot take it", "[geometry-page-worker]")
{
    auto input=OriginalInput();AuthoredPageWorker worker;
    REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));
    REQUIRE_FALSE(worker.Take(2,2)); // discard rather than publish an earlier fixture result
    REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().stale==1);
    REQUIRE(worker.Submit(2,2,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));
    auto retained=worker.Take(2,2);REQUIRE(retained);worker.HoldBeforePublish(true);
    REQUIRE(worker.Submit(3,3,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker.Snapshot().active==1;}));
    REQUIRE(worker.Submit(4,4,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Busy);
    retained.reset();worker.Cancel(3);worker.HoldBeforePublish(false);
    REQUIRE(Until([&]{return worker.Snapshot().liveJobs==0;}));REQUIRE(worker.Snapshot().reservedBytes==0);
}
TEST_CASE("Authored worker refusal and page failure publish no partial fine representation", "[geometry-page-worker]")
{
    auto input=OriginalInput();AuthoredPageWorker worker;
    SECTION("mismatched immutable identity") {
        ++input->identity.source.geometryOptions;
        REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
        REQUIRE(worker.Snapshot().liveJobs==0);
    }
    SECTION("source or logical budget refusal") {
        REQUIRE(worker.Submit(1,1,input,1)==AuthoredPageWorker::SubmitStatus::Capacity);
        input->knownCapacityBytes=AuthoredPageInput::MaxSourceBytes+1;
        REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Invalid);
        REQUIRE(worker.Snapshot().reservedBytes==0);
    }
    SECTION("last page corruption after valid earlier page") {
        REQUIRE(input->package.pages.size()>=3);input->package.pages.back().bytes.back()^=1;
        REQUIRE(worker.Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
        REQUIRE(Until([&]{return worker.Snapshot().ready==1;}));auto result=worker.Take(1,1);REQUIRE(result);
        REQUIRE(result->status==DecodeStatus::Invalid);REQUIRE(result->decoded.pages.empty());REQUIRE(result->decoded.indices==0);
    }
    SECTION("cancelled decoder keeps previous destination") {
        std::atomic<bool> cancelled{true};ResidentRepresentation sentinel;sentinel.indices=991;
        REQUIRE(DecodeResidentRepresentation(input->package,input->identity,Frontier::Fine,input->original,sentinel,{},&cancelled)==DecodeStatus::Cancelled);
        REQUIRE(sentinel.indices==991);REQUIRE(sentinel.pages.empty());
    }
}
TEST_CASE("Authored worker shutdown releases an explicitly held active job", "[geometry-page-worker]")
{
    auto input=OriginalInput();auto worker=std::make_unique<AuthoredPageWorker>();worker->HoldBeforePublish(true);
    REQUIRE(worker->Submit(1,1,input,AuthoredPageWorker::MaxReserved)==AuthoredPageWorker::SubmitStatus::Queued);
    REQUIRE(Until([&]{return worker->Snapshot().active==1;}));
    worker.reset(); // destructor unholds, cancels and joins; no Engine or renderer dependency
}
