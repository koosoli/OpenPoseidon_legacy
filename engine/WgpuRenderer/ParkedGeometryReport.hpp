#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <limits>
namespace Poseidon::render {
// Copied producer-request selection only: no Shape/Texture owners or reclaim policy.
struct ParkedGeometryCapture {
    static constexpr size_t MaxModels=256,MaxVisits=2048;
    bool complete=true,truncated=false;
    uint64_t modelsVisited=0,metadataVisits=0,invalidEntries=0;
    std::vector<uint32_t> models;
    std::vector<uint64_t> allocations;
};
struct ParkedGeometryObservation {
    uint64_t allocation=0,epoch=0,handle=0,cpu=0,modelLinks=0,selectedLinks=0;
    uint64_t vertexBytes=0,indexBytes=0;
    bool producer=false,retired=false,factValid=false;
    uint32_t state=0; // Existing Rust facts:1 Present,2 Absent. No new FFI.
};
struct ParkedGeometryClasses {
    bool complete=false;
    uint64_t uniqueAllocations=0,knownCandidateBytes=0;
    uint64_t cpuBorrowed=0,cpuBorrowedBytes=0,unborrowed=0,unborrowedBytes=0;
    uint64_t outsideLinked=0,outsideLinkedBytes=0,absentRetired=0,unknown=0,unknownKnownBytes=0;
};
// Disjoint logical candidates, never physical/exclusive ownership or reclaimability.
// Selection uncertainty sends every observed byte to Unknown, not reassuring partial proof.
// UnknownKnownBytes covers only successfully joined Present facts, not missing/uninspected bytes.
inline ParkedGeometryClasses ClassifyParkedGeometry(const ParkedGeometryCapture& capture,
    uint64_t epoch,bool joinedCutComplete,const std::vector<ParkedGeometryObservation>& observations)
{
    ParkedGeometryClasses out;
    if(capture.models.size()>ParkedGeometryCapture::MaxModels||capture.metadataVisits>ParkedGeometryCapture::MaxVisits||
       capture.allocations.size()>ParkedGeometryCapture::MaxVisits||observations.size()>8192)return out;
    std::unordered_map<uint64_t,const ParkedGeometryObservation*> byId;std::unordered_set<uint64_t> seen;
    bool duplicate=false;for(const auto& item:observations)if(!byId.emplace(item.allocation,&item).second)duplicate=true;
    const bool certain=capture.complete&&!capture.truncated&&!capture.invalidEntries&&epoch&&joinedCutComplete&&!duplicate;
    bool arithmetic=true;
    const auto add=[&](uint64_t& field,uint64_t n){if(n>UINT64_MAX-field){arithmetic=false;return;}field+=n;};
    for(uint64_t id:capture.allocations) {
        if(!seen.insert(id).second)continue;++out.uniqueAllocations;
        const auto at=byId.find(id);const auto* v=at==byId.end()?nullptr:at->second;
        const bool valid=v&&id&&v->epoch==epoch&&v->handle&&v->factValid&&v->state>=1&&v->state<=2;
        uint64_t bytes=0;
        if(valid&&v->state==1){add(bytes,v->vertexBytes);add(bytes,v->indexBytes);add(out.knownCandidateBytes,bytes);}
        if(!certain||!valid||!arithmetic||v->selectedLinks>v->modelLinks){++out.unknown;add(out.unknownKnownBytes,bytes);continue;}
        if(v->state==2){if(v->retired&&!v->producer&&!v->cpu&&!v->modelLinks)++out.absentRetired;else ++out.unknown;}
        else if(v->retired||!v->producer||!v->selectedLinks){++out.unknown;add(out.unknownKnownBytes,bytes);}
        else if(v->selectedLinks<v->modelLinks){++out.outsideLinked;add(out.outsideLinkedBytes,bytes);}
        else if(v->cpu){++out.cpuBorrowed;add(out.cpuBorrowedBytes,bytes);}
        else {++out.unborrowed;add(out.unborrowedBytes,bytes);}
    }
    if(!arithmetic){const auto count=out.uniqueAllocations;out={};out.uniqueAllocations=count;out.unknown=count;return out;}
    out.complete=certain&&!out.unknown;return out;
}
}
