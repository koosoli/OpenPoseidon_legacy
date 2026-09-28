#pragma once
#include <cstdint>

namespace Poseidon::GeometryPages {
// Private, one-way fixture selection gate. The projected demand policy remains a
// preparation preference; the caller must independently supply an authenticated
// completed renderer cut and current immutable source/mesh ownership facts.
struct PrivateAutoFineCut {
    bool enabled=false,alreadyCommitted=false,active=false,readyReturned=false;
    bool sourceVerified=false,projectedBound=false,desiredFine=false;
    bool coarseSelected=false,finePrepared=false,fineResident=false,completeRows=false,noWork=false;
    uint64_t fixtureEpoch=0,sourceEpoch=0,ownerEpoch=0,certificateEpoch=0;
    uint64_t cameraGeneration=0,demandGeneration=0,policyGeneration=0;
    uint64_t requestId=0,observedRequestId=0;
    uint32_t currentModel=UINT32_MAX,coarseModel=UINT32_MAX;
    uint32_t preparedModel=UINT32_MAX,fineModel=UINT32_MAX;

    bool Eligible() const {
        return enabled&&!alreadyCommitted&&active&&readyReturned&&sourceVerified&&
            projectedBound&&desiredFine&&coarseSelected&&finePrepared&&fineResident&&
            completeRows&&noWork&&fixtureEpoch&&sourceEpoch&&ownerEpoch&&certificateEpoch&&
            sourceEpoch==ownerEpoch&&certificateEpoch==ownerEpoch&&
            cameraGeneration&&cameraGeneration==demandGeneration&&cameraGeneration==policyGeneration&&
            requestId&&requestId!=UINT64_MAX&&requestId==observedRequestId&&
            currentModel!=UINT32_MAX&&currentModel==coarseModel&&
            fineModel!=UINT32_MAX&&preparedModel==fineModel&&fineModel!=coarseModel;
    }
};
}
