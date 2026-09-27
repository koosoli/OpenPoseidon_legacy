#pragma once
#include <cstdint>
#include <limits>
namespace Poseidon::GeometryPages
{
// Private authored pilot only. Historical handles remain in their original
// order; retirement changes expected record state, never erases history.
struct PageHistoryScope
{
    uint32_t count=0,retiredBegin=0,retiredEnd=0;
    bool active=false;
    bool Valid() const { return count>0 && count<=64 && retiredBegin<=retiredEnd && retiredEnd<=count; }
    uint32_t Expected(uint32_t index) const {
        if(!Valid() || index>=count) return 0;
        return !active || (index>=retiredBegin && index<retiredEnd)?2:1;
    }
};
enum class FineUpdateDecision { Drop, Resident, Commit };
// Each generation has its own immutable producer ID and is staged once.
// Thus an old queued update cannot commit a replacement request/model.
inline FineUpdateDecision DecideFineUpdate(bool active,bool resident,bool uploading,
    uint32_t operation,uint32_t current,uint32_t pending,uint64_t request)
{
    constexpr auto Invalid=std::numeric_limits<uint32_t>::max();
    if(!active || operation==Invalid || current==Invalid || operation!=current) return FineUpdateDecision::Drop;
    if(resident) return FineUpdateDecision::Resident;
    return uploading && request && pending!=Invalid && operation==pending?FineUpdateDecision::Commit:FineUpdateDecision::Drop;
}
}
