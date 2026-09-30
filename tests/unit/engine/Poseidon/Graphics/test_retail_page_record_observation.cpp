#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/RetailPageRecordObservation.hpp>
#include <array>
#include <memory>
#include <thread>

using Poseidon::render::RetailPageRecordObservation;
using Record = RetailPageRecordObservation;

namespace
{
constexpr std::array<Record::MeshExpectation, 2> liveMeshes{{
    {11, Record::ExpectedMesh::Present, 0},
    {12, Record::ExpectedMesh::Present, 0}}};
constexpr std::array<Record::ModelExpectation, 1> liveModel{{
    {3, Record::ExpectedModel::MappedValid}}};
Record::DrainEvidence PresentEvidence()
{
    Record::DrainEvidence e;
    e.getterSucceeded = true;
    e.summary = {2, 2, 2, 0, 0, 0, 1, 0, 1, 0};
    e.meshes[0] = {true, 101, 101, 48, 12, 1, 0};
    e.meshes[1] = {true, 102, 102, 72, 24, 1, 0};
    e.models[0] = {true, 31};
    return e;
}
}

TEST_CASE("Retail page record publishes one exact present cut without an instance claim",
    "[geometry-page][retail-record]")
{
    auto packet = std::make_shared<Record>(7, 8, 9, liveMeshes, liveModel);
    REQUIRE(packet->Valid());
    CHECK(packet->MeshAt(0).producer == 11);
    CHECK(packet->ModelAt(0).producer == 3);
    auto e = PresentEvidence();
    bool published = false;
    std::thread drain([packet, e, &published] { published = packet->Publish(e); });
    drain.join();
    CHECK(published);
    const auto observed = packet->Observe();
    CHECK(observed.state == Record::State::Observed);
    CHECK(observed.refusal == Record::Refusal::None);
    CHECK(observed.ownerEpoch == 7);
    CHECK(observed.sourceAdmissionEpoch == 8);
    CHECK(observed.requestId == 9);
    CHECK(observed.presentMeshes == 2);
    CHECK(observed.absentMeshes == 0);
    CHECK(observed.mappedModels == 1);
    CHECK(observed.rendererMeshes[0] == 101);
    CHECK(observed.rendererMeshes[1] == 102);
    CHECK(observed.rendererModels[0] == 31);
    CHECK_FALSE(packet->Publish(e)); // a second queue observation cannot replace the first
    packet->ConsumeRejected();
    CHECK(packet->Observe().state == Record::State::Observed);
}

TEST_CASE("Retail page retirement requires old renderer records absent and producer mappings erased",
    "[geometry-page][retail-record]")
{
    constexpr std::array<Record::MeshExpectation, 2> retired{{
        {11, Record::ExpectedMesh::Absent, 101},
        {12, Record::ExpectedMesh::Absent, 102}}};
    constexpr std::array<Record::ModelExpectation, 1> retiredModel{{
        {3, Record::ExpectedModel::Absent}}};
    Record::DrainEvidence gone;
    gone.getterSucceeded = true;
    gone.summary = {2, 2, 0, 2, 0, 0, 1, 0, 1, 0};
    gone.meshes[0] = {false, 0, 101, 0, 0, 2, 0};
    gone.meshes[1] = {false, 0, 102, 0, 0, 2, 0};
    gone.models[0] = {false, Record::InvalidModel};
    Record accepted(7, 8, 10, retired, retiredModel);
    REQUIRE(accepted.Publish(gone));
    const auto snapshot = accepted.Observe();
    CHECK(snapshot.absentMeshes == 2);
    CHECK(snapshot.absentModels == 1);
    CHECK(snapshot.rendererMeshes[0] == 101); // exact old generational ID, not an allocator-free claim

    auto stillMapped = gone;
    stillMapped.meshes[0].mappingExists = true;
    stillMapped.meshes[0].mappedRenderer = 101;
    Record mapped(7, 8, 11, retired, retiredModel);
    CHECK_FALSE(mapped.Publish(stillMapped));
    CHECK(mapped.Observe().refusal == Record::Refusal::Mesh);

    auto oldModelMapped = gone;
    oldModelMapped.models[0] = {true, 31};
    Record modelMapped(7, 8, 12, retired, retiredModel);
    CHECK_FALSE(modelMapped.Publish(oldModelMapped));
    CHECK(modelMapped.Observe().refusal == Record::Refusal::Model);

    auto wrongGeneration = gone;
    wrongGeneration.meshes[1].factRenderer = 999;
    Record wrong(7, 8, 13, retired, retiredModel);
    CHECK_FALSE(wrong.Publish(wrongGeneration));
    CHECK(wrong.Observe().refusal == Record::Refusal::Mesh);
}

TEST_CASE("Retail record packet and full getter summary fail closed", "[geometry-page][retail-record]")
{
    constexpr std::array<Record::MeshExpectation, 2> duplicate{{
        {11, Record::ExpectedMesh::Present, 0},
        {11, Record::ExpectedMesh::Present, 0}}};
    Record malformed(7, 8, 9, duplicate, liveModel);
    CHECK_FALSE(malformed.Valid());
    CHECK(malformed.Observe().state == Record::State::Rejected);
    CHECK(malformed.Observe().refusal == Record::Refusal::InvalidPacket);

    auto e = PresentEvidence();
    e.summary.duplicateHandles = 1;
    Record duplicateFact(7, 8, 9, liveMeshes, liveModel);
    CHECK_FALSE(duplicateFact.Publish(e));
    CHECK(duplicateFact.Observe().refusal == Record::Refusal::Summary);

    e = PresentEvidence();
    e.meshes[1].mappedRenderer = 0;
    Record missingMap(7, 8, 9, liveMeshes, liveModel);
    CHECK_FALSE(missingMap.Publish(e));
    CHECK(missingMap.Observe().refusal == Record::Refusal::Mesh);

    Record missingGetter(7, 8, 9, liveMeshes, liveModel);
    missingGetter.ConsumeRejected();
    CHECK(missingGetter.Observe().state == Record::State::Rejected);
    CHECK(missingGetter.Observe().refusal == Record::Refusal::Getter);
    CHECK_FALSE(missingGetter.Publish(PresentEvidence()));
}
