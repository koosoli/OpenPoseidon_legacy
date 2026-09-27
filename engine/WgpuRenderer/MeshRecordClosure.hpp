#pragma once
#include <cstdint>
#include <limits>
namespace Poseidon::render
{
struct MeshRecordClosureInput
{
    bool sourceScopeValid=false,factsComplete=false;
    uint64_t requested=0,inspected=0,present=0,invalid=0,duplicates=0,flags=0;
    uint64_t vertexBytes=0,indexBytes=0,liveRecords=0,poolLiveBytes=0;
};
struct MeshRecordClosure
{
    bool scopeValid=false,recordsComplete=false,residualValid=false;
    uint64_t unattributedBytes=0;
};
// Exact logical mesh records/range payload only. Retired buffers, uploader,
// encoded work, bind groups and physical GPU memory are outside this scope.
inline MeshRecordClosure EvaluateMeshRecordClosure(const MeshRecordClosureInput& s)
{
    MeshRecordClosure r; r.scopeValid=s.sourceScopeValid;
    if(!s.sourceScopeValid || !s.factsComplete || s.requested!=s.inspected || s.present>s.inspected ||
        s.invalid || s.duplicates || s.flags || s.vertexBytes>std::numeric_limits<uint64_t>::max()-s.indexBytes) return r;
    const auto bytes=s.vertexBytes+s.indexBytes;
    if(s.present>s.liveRecords || bytes>s.poolLiveBytes) return r;
    r.residualValid=true; r.unattributedBytes=s.poolLiveBytes-bytes;
    r.recordsComplete=s.present==s.liveRecords && r.unattributedBytes==0;
    return r;
}
}
