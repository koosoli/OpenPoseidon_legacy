#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace Poseidon::GeometryPages {
// One private asset only. Distances predict prefetch, not screen error/visibility
// or all-view need. Time is injected; caller observes the joined world-camera cut.
struct CameraDemandSettings {
    double nearDistance=30,farDistance=50;
    uint32_t dwellMs=200,retryMs=250;
    bool Valid() const {return std::isfinite(nearDistance)&&std::isfinite(farDistance)&&
        nearDistance>0&&nearDistance<farDistance&&farDistance<=100000&&dwellMs<=2000&&retryMs>=50&&retryMs<=2000;}
};
class CameraPageDemand {
    CameraDemandSettings settings_;
    uint64_t last_=0,since_=0,retryAt_=0,revision_=1;
    uint32_t attempts_=0;
    bool seen_=false,candidate_=false,candidateValid_=false,desired_=false;
    void Desired(bool value) {
        if(desired_==value)return;
        if(revision_==UINT64_MAX){desired_=false;attempts_=MaxAttempts;return;}
        desired_=value;++revision_;
    }
public:
    static constexpr uint32_t MaxAttempts=8;
    explicit CameraPageDemand(CameraDemandSettings settings):settings_(settings){}
    bool DesiredFine() const {return desired_;}
    uint64_t Revision() const {return revision_;}
    uint32_t Attempts() const {return attempts_;}
    // Returns departure/revocation, never a geometric clearance claim.
    bool Observe(uint64_t now,bool hasCamera,double distance) {
        const bool before=desired_;
        if(!settings_.Valid()||now==UINT64_MAX||(seen_&&now<last_)||!hasCamera||!std::isfinite(distance)||distance<0){
            Desired(false);candidateValid_=false;return before;}
        seen_=true;last_=now;
        bool next=desired_;
        if(distance<=settings_.nearDistance)next=true;
        else if(distance>=settings_.farDistance)next=false;
        else {candidateValid_=false;return false;}
        if(next==desired_){candidateValid_=false;return false;}
        if(!candidateValid_||candidate_!=next){candidate_=next;candidateValid_=true;since_=now;}
        if(now-since_>=settings_.dwellMs){Desired(next);candidateValid_=false;}
        return before&&!desired_;
    }
    bool CanAttempt(uint64_t now,bool prepared,bool activeRequest,bool uploading) const {
        return settings_.Valid()&&seen_&&now>=last_&&now!=UINT64_MAX&&desired_&&!prepared&&!activeRequest&&!uploading&&
            attempts_<MaxAttempts&&now>=retryAt_;
    }
    void Attempted(uint64_t now) {
        if(attempts_<MaxAttempts)++attempts_;
        retryAt_=now>UINT64_MAX-settings_.retryMs?UINT64_MAX:now+settings_.retryMs;
    }
};
// Same full record cut used by the live fixture; presence alone isn't an instance
// selection/draw/pixel/physical-GPU-free or all-pass certificate.
inline bool PreparedFineRecordCut(bool active,bool pending,bool returnedFrame,bool validRows,bool expectedRows,
    uint32_t expectedModel,uint32_t observedModel) {
    return active&&pending&&returnedFrame&&validRows&&expectedRows&&expectedModel!=UINT32_MAX&&observedModel!=UINT32_MAX;
}
}
