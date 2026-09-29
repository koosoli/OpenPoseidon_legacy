#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalPackage.hpp>
#include <algorithm>
#include <limits>

namespace Poseidon::GeometryPages
{
// Private, one-cycle record-cut preflight. This only plans ownership changes;
// the owner must queue model retirement before mesh destruction, observe every
// historical handle Absent, and then use fresh producer IDs for refill. It is
// not an all-view, image, or physical GPU allocator release certificate.
struct HierarchicalReleaseToken
{
    HierarchicalIdentity identity;
    uint64_t sourceAdmissionEpoch=0,pageEpoch=0,demandGeneration=0,requestId=0;
    bool operator==(const HierarchicalReleaseToken&) const = default;
};
struct HierarchicalReleaseCutSlot
{
    uint32_t producerModel=UINT32_MAX;
    bool everUsed=false,live=false;
    std::span<const uint32_t> clusters; // empty for fallback and unused slots
};
struct HierarchicalReleaseMeshHistory
{
    uint64_t producerMesh=0,rendererMesh=0;
    uint32_t cluster=UINT32_MAX; // authored fallback has no hierarchy cluster
    uint8_t expectedState=0,observedState=0; // 1 Present, 2 Absent
    bool current=false;
};
struct HierarchicalReleaseInput
{
    HierarchicalReleaseToken expected,observed;
    SourceIdentity admittedSource;
    std::span<const uint8_t> residentPages;
    std::span<const uint32_t> selectedClusters;
    std::span<const HierarchicalReleaseCutSlot> cutSlots; // fallback + at most three cuts
    std::span<const HierarchicalReleaseMeshHistory> meshHistory;
    uint32_t selectedModel=UINT32_MAX,consumedModel=UINT32_MAX;
    bool returnedFrameReady=false,instanceExact=false;
    bool pendingPage=false,pendingModel=false,pendingWorker=false;
    uint32_t workerLiveJobs=0;
    uint64_t workerReservedBytes=0,knownBytes=0,retirementStagingBytes=0,refillReservationBytes=0;
};
struct HierarchicalReleasePlan
{
    HierarchicalReleaseToken token;
    uint64_t pinnedRootPages=0,retiredPages=0;
    std::array<uint32_t,4> retireModelSlots{};
    uint32_t retireModelCount=0,freshModelSlot=UINT32_MAX;
    std::array<uint32_t,64> retireHistoryIndices{};
    std::array<uint8_t,64> expectedHistoryStates{};
    uint32_t retireMeshCount=0,refillMeshCount=0;
    uint64_t refillUploadBytes=0;
};

inline bool PlanHierarchicalRelease(const HierarchicalPackage& package,
    const HierarchicalReleaseInput& input,HierarchicalReleasePlan& destination)
{
    constexpr uint64_t MaxKnown=1024*1024;
    if(!ValidHierarchicalMetadata(package)||!(input.expected==input.observed)||
        !(package.identity==input.expected.identity)||!(package.identity.source==input.admittedSource)||
        !input.expected.sourceAdmissionEpoch||!input.expected.pageEpoch||
        !input.expected.demandGeneration||!input.expected.requestId||
        !input.returnedFrameReady||!input.instanceExact||
        input.selectedModel==UINT32_MAX||input.selectedModel!=input.consumedModel||
        input.pendingPage||input.pendingModel||input.pendingWorker||
        input.workerLiveJobs||input.workerReservedBytes||
        input.residentPages.size()!=package.pages.size()||
        input.selectedClusters.size()!=package.rootClusters.size()||
        !std::equal(input.selectedClusters.begin(),input.selectedClusters.end(),package.rootClusters.begin())||
        input.cutSlots.size()!=4||input.meshHistory.empty()||input.meshHistory.size()>64||
        input.knownBytes>MaxKnown||input.retirementStagingBytes>MaxKnown-input.knownBytes||
        input.refillReservationBytes>MaxKnown-input.knownBytes-input.retirementStagingBytes)return false;
    for(auto bit:input.residentPages)if(bit>1)return false;

    HierarchicalReleasePlan result;result.token=input.expected;
    std::array<int,256> currentClusterIndex{};currentClusterIndex.fill(-1);
    uint32_t rootSlot=UINT32_MAX,fallbackCount=0;
    for(uint32_t i=0;i<input.cutSlots.size();++i) {
        const auto& slot=input.cutSlots[i];
        if(slot.producerModel==UINT32_MAX||(!slot.everUsed&&slot.live)||
            (slot.live&&i!=0&&slot.clusters.empty())||slot.clusters.size()>64)return false;
        for(uint32_t j=0;j<i;++j)
            if(input.cutSlots[j].producerModel==slot.producerModel)return false;
        if(i==0) {
            if(!slot.everUsed||!slot.live||!slot.clusters.empty())return false;
            continue;
        }
        if(!slot.everUsed) {
            if(!slot.clusters.empty())return false;
            if(result.freshModelSlot==UINT32_MAX)result.freshModelSlot=i;
        }
        if(slot.live&&slot.producerModel==input.selectedModel)rootSlot=i;
    }
    if(rootSlot==UINT32_MAX||result.freshModelSlot==UINT32_MAX)return false;
    const auto& rootCut=input.cutSlots[rootSlot].clusters;
    if(rootCut.size()!=package.rootClusters.size()||
        !std::equal(rootCut.begin(),rootCut.end(),package.rootClusters.begin()))return false;
    for(uint32_t i=1;i<input.cutSlots.size();++i)if(input.cutSlots[i].live&&i!=rootSlot)
        result.retireModelSlots[result.retireModelCount++]=i;

    for(uint32_t p:package.rootPages) {
        if(!input.residentPages[p])return false;
        result.pinnedRootPages|=uint64_t(1)<<p;
    }
    for(uint32_t p=0;p<package.pages.size();++p)if(input.residentPages[p]&&
        !(result.pinnedRootPages&(uint64_t(1)<<p))) {
        result.retiredPages|=uint64_t(1)<<p;
        result.refillMeshCount+=uint32_t(package.pages[p].clusters.size());
        result.refillUploadBytes+=package.pages[p].uploadBytes;
    }
    if(!result.retiredPages||input.meshHistory.size()+result.refillMeshCount>64)return false;

    for(uint32_t i=0;i<input.meshHistory.size();++i) {
        const auto& mesh=input.meshHistory[i];
        if(!mesh.producerMesh||!mesh.rendererMesh||
            (mesh.expectedState!=1&&mesh.expectedState!=2)||
            mesh.observedState!=mesh.expectedState||mesh.current!=(mesh.expectedState==1))return false;
        for(uint32_t j=0;j<i;++j)
            if(input.meshHistory[j].producerMesh==mesh.producerMesh||
               input.meshHistory[j].rendererMesh==mesh.rendererMesh)return false;
        result.expectedHistoryStates[i]=mesh.expectedState;
        if(mesh.cluster==UINT32_MAX) {
            if(mesh.current)++fallbackCount;
            continue;
        }
        if(mesh.cluster>=package.clusters.size())return false;
        if(!mesh.current)continue;
        if(currentClusterIndex[mesh.cluster]>=0)return false;
        if(!input.residentPages[package.clusters[mesh.cluster].page])return false;
        currentClusterIndex[mesh.cluster]=int(i);
    }
    if(fallbackCount!=1)return false;
    for(uint32_t c=0;c<package.clusters.size();++c)
        if((currentClusterIndex[c]>=0)!=(input.residentPages[package.clusters[c].page]!=0))return false;
    for(uint32_t i=1;i<input.cutSlots.size();++i)if(input.cutSlots[i].live) {
        std::array<uint8_t,256> seen{};
        for(auto c:input.cutSlots[i].clusters) {
            if(c>=package.clusters.size()||seen[c]++||currentClusterIndex[c]<0)return false;
        }
    }
    for(uint32_t c=0;c<package.clusters.size();++c)
        if(result.retiredPages&(uint64_t(1)<<package.clusters[c].page)) {
            const int index=currentClusterIndex[c];
            if(index<0||result.retireMeshCount>=64)return false;
            result.retireHistoryIndices[result.retireMeshCount++]=uint32_t(index);
            result.expectedHistoryStates[index]=2;
        }
    if(result.retireMeshCount!=result.refillMeshCount)return false;
    destination=result;return true;
}
}
