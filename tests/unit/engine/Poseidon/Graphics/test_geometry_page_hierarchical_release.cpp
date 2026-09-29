#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalRelease.hpp>
#include <vector>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
HierarchicalPackage TwoPageHierarchy()
{
    HierarchicalPackage package;
    auto& source=package.identity.source;
    source.sourceSha256[0]=7;source.producerVersion=1;
    source.vertexLayout=sizeof(SVertex);source.materialMapping=1;
    source.coarseRepresentation=0;source.fineRepresentation=1;
    package.identity.packageSha256[0]=11;
    clodBounds leaf{},root{};leaf.radius=root.radius=1;
    leaf.error=.25f;root.error=FLT_MAX;
    package.groups={{0,leaf,0,1},{1,root,1,1}};
    package.clusters={{0,0,0,-1,leaf},{1,1,0,0,root}};
    for(uint32_t i=0;i<2;++i) {
        HierarchicalPage page;page.group=i;
        HierarchicalPayload payload;payload.cluster=i;payload.originalVertexIds={0,1,2};
        payload.vertices.resize(3);payload.indices={0,1,2};
        page.clusters.push_back(std::move(payload));
        page.logicalBytes=256;page.uploadBytes=3*sizeof(SVertex)+3*sizeof(uint32_t);
        package.pages.push_back(std::move(page));
    }
    package.rootClusters={1};package.rootPages={1};
    package.knownCapacityBytes=HierarchicalKnownBytes(package);
    REQUIRE(ValidHierarchicalMetadata(package));return package;
}
struct Fixture
{
    HierarchicalPackage package=TwoPageHierarchy();
    std::array<uint8_t,2> resident{1,1};
    std::vector<uint32_t> selected{1},rootKey{1},fineKey{0};
    std::array<HierarchicalReleaseCutSlot,4> slots{};
    std::vector<HierarchicalReleaseMeshHistory> history{
        {100,1000,UINT32_MAX,1,1,true},
        {101,1001,0,1,1,true},
        {102,1002,1,1,1,true}};
    HierarchicalReleaseToken token;
    Fixture()
    {
        token.identity=package.identity;
        token.sourceAdmissionEpoch=3;token.pageEpoch=5;token.demandGeneration=7;token.requestId=9;
        slots[0]={10,true,true,{}};
        slots[1]={11,true,true,rootKey};
        slots[2]={12,true,true,fineKey};
        slots[3]={13,false,false,{}};
    }
    HierarchicalReleaseInput Input() const
    {
        HierarchicalReleaseInput input;
        input.expected=input.observed=token;input.admittedSource=package.identity.source;
        input.residentPages=resident;input.selectedClusters=selected;
        input.cutSlots=slots;input.meshHistory=history;
        input.selectedModel=input.consumedModel=11;
        input.returnedFrameReady=input.instanceExact=true;
        input.knownBytes=128*1024;input.retirementStagingBytes=4096;
        input.refillReservationBytes=256*1024;
        return input;
    }
};
void RefusedWithoutPublication(const Fixture& fixture,HierarchicalReleaseInput input)
{
    HierarchicalReleasePlan plan;plan.pinnedRootPages=0xdead;
    plan.retireMeshCount=77;plan.freshModelSlot=88;
    REQUIRE_FALSE(PlanHierarchicalRelease(fixture.package,input,plan));
    REQUIRE(plan.pinnedRootPages==0xdead);
    REQUIRE(plan.retireMeshCount==77);
    REQUIRE(plan.freshModelSlot==88);
}
}
TEST_CASE("Hierarchy release pins the complete root and plans one fresh-ID refill", "[geometry-page-hierarchical-release]")
{
    const Fixture fixture;HierarchicalReleasePlan plan;
    REQUIRE(PlanHierarchicalRelease(fixture.package,fixture.Input(),plan));
    REQUIRE(plan.token==fixture.token);
    REQUIRE(plan.pinnedRootPages==(uint64_t(1)<<1));
    REQUIRE(plan.retiredPages==1);
    REQUIRE(plan.retireModelCount==1);
    REQUIRE(plan.retireModelSlots[0]==2);
    REQUIRE(plan.freshModelSlot==3);
    REQUIRE(plan.retireMeshCount==1);
    REQUIRE(plan.retireHistoryIndices[0]==1);
    REQUIRE(plan.refillMeshCount==1);
    REQUIRE(plan.refillUploadBytes==fixture.package.pages[0].uploadBytes);
    REQUIRE(plan.expectedHistoryStates[0]==1);
    REQUIRE(plan.expectedHistoryStates[1]==2);
    REQUIRE(plan.expectedHistoryStates[2]==1);
}
TEST_CASE("Hierarchy release refuses stale authority, incomplete root and worker debt transactionally", "[geometry-page-hierarchical-release]")
{
    Fixture fixture;
    auto input=fixture.Input();input.observed.demandGeneration++;
    RefusedWithoutPublication(fixture,input);
    input=fixture.Input();input.observed.requestId++;
    RefusedWithoutPublication(fixture,input);
    input=fixture.Input();input.admittedSource.sourceSha256[0]++;
    RefusedWithoutPublication(fixture,input);
    input=fixture.Input();input.pendingWorker=true;
    RefusedWithoutPublication(fixture,input);
    input=fixture.Input();input.workerLiveJobs=1;input.workerReservedBytes=4096;
    RefusedWithoutPublication(fixture,input);
    input=fixture.Input();input.returnedFrameReady=false;
    RefusedWithoutPublication(fixture,input);
    fixture.selected={0};RefusedWithoutPublication(fixture,fixture.Input());
    fixture.selected={1};fixture.resident[1]=0;
    RefusedWithoutPublication(fixture,fixture.Input());
}
TEST_CASE("Hierarchy release refuses duplicate or missing handles and exhausted history", "[geometry-page-hierarchical-release]")
{
    Fixture fixture;
    fixture.history[1].rendererMesh=fixture.history[2].rendererMesh;
    RefusedWithoutPublication(fixture,fixture.Input());
    fixture.history[1].rendererMesh=1001;fixture.history[2].producerMesh=0;
    RefusedWithoutPublication(fixture,fixture.Input());
    fixture.history[2].producerMesh=102;fixture.history[2].expectedState=2;
    fixture.history[2].observedState=2;fixture.history[2].current=false;
    RefusedWithoutPublication(fixture,fixture.Input());
    fixture.history[2]={102,1002,1,1,1,true};
    for(uint32_t i=0;i<61;++i)
        fixture.history.push_back({200+i,2000+i,UINT32_MAX,2,2,false});
    REQUIRE(fixture.history.size()==64);
    RefusedWithoutPublication(fixture,fixture.Input());
}
TEST_CASE("Hierarchy release requires an unused model slot and charged staging headroom", "[geometry-page-hierarchical-release]")
{
    Fixture fixture;
    fixture.slots[3]={13,true,true,fixture.fineKey};
    RefusedWithoutPublication(fixture,fixture.Input());
    fixture.slots[3]={13,false,false,{}};
    auto input=fixture.Input();input.knownBytes=1024*1024;
    RefusedWithoutPublication(fixture,input);
    input=fixture.Input();input.pendingPage=true;
    RefusedWithoutPublication(fixture,input);
    input=fixture.Input();input.pendingModel=true;
    RefusedWithoutPublication(fixture,input);
}
