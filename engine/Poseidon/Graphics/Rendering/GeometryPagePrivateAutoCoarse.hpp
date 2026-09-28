#pragma once
#include <cmath>
#include <cstdint>

namespace Poseidon::GeometryPages {
// One private resident Fine -> Coarse selection, independent of the preparation
// policy. Caller authenticates the joined returned-frame camera/source/renderer
// facts immediately before enqueueing an update to the existing coarse model.
struct PrivateAutoCoarseCut {
    bool enabled=false,autoFineCommitted=false,alreadyReturned=false,active=false,readyReturned=false;
    bool sourceVerified=false,idealBound=false,fineSelected=false,fineResident=false,coarseResident=false;
    bool completeRows=false,noWork=false,withinWatchBudget=false;
    uint64_t fixtureEpoch=0,sourceEpoch=0,ownerEpoch=0,certificateEpoch=0;
    uint64_t cameraGeneration=0,autoFineGeneration=0,demandGeneration=0;
    uint64_t requestId=0,observedRequestId=0,autoFineSourceRequest=0;
    uint32_t currentModel=UINT32_MAX,fineModel=UINT32_MAX,coarseModel=UINT32_MAX;
    double idealUpper=0,leaveUpper=0;
    bool Eligible() const {
        return enabled&&autoFineCommitted&&!alreadyReturned&&active&&readyReturned&&
            sourceVerified&&idealBound&&fineSelected&&fineResident&&coarseResident&&
            completeRows&&noWork&&withinWatchBudget&&fixtureEpoch&&sourceEpoch&&sourceEpoch==ownerEpoch&&
            certificateEpoch==ownerEpoch&&cameraGeneration>autoFineGeneration&&
            cameraGeneration==demandGeneration&&requestId>autoFineSourceRequest&&
            requestId!=UINT64_MAX&&requestId==observedRequestId&&
            currentModel!=UINT32_MAX&&currentModel==fineModel&&
            coarseModel!=UINT32_MAX&&coarseModel!=fineModel&&
            std::isfinite(idealUpper)&&std::isfinite(leaveUpper)&&
            idealUpper>=0&&leaveUpper>=0&&idealUpper<=leaveUpper;
    }
};
}
