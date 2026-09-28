#pragma once
#include <cmath>
#include <cstdint>

namespace Poseidon::GeometryPages {
// Private experimental preparation preference, NOT absolute source error, raster
// pixels, visibility, all-pass consistency, or permission to select a cut.
struct ProjectedDemandSettings {
    double enterUpper=2,leaveUpper=1;
    uint32_t dwellMs=200,retryMs=250;
    bool Valid() const {return std::isfinite(enterUpper)&&std::isfinite(leaveUpper)&&
        leaveUpper>=0&&leaveUpper<enterUpper&&enterUpper<=10000&&dwellMs<=2000&&retryMs>=50&&retryMs<=2000;}
};
struct ProjectedDemandBinding {
    uint64_t fixtureEpoch=0,sourceAdmissionEpoch=0,certificateGeneration=0,coarseModelBirth=0;
    bool Valid() const {return fixtureEpoch&&sourceAdmissionEpoch&&certificateGeneration&&coarseModelBirth;}
    bool operator==(const ProjectedDemandBinding&) const = default;
};
struct ProjectedDemandObservation {
    ProjectedDemandBinding binding;
    uint64_t cameraGeneration=0;
    // Caller validates full immutable source/package/certificate, placement,
    // successful main-frame tuple, exact 1:1 viewport and ideal bound first.
    // A numeric epoch alone cannot authenticate those facts.
    bool validatedIdealPairBound=false;
    double pairDiscrepancyUpper=0;
};
class ProjectedPageDemand {
    ProjectedDemandSettings settings_;
    ProjectedDemandBinding binding_;
    uint64_t lastTime_=0,lastSeenTime_=0,lastGeneration_=0,attemptedGeneration_=0,since_=0,retryAt_=0,revision_=1;
    uint32_t attempts_=0;
    bool seenTime_=false,authorized_=false,candidate_=false,candidateValid_=false,desired_=false;
    void Desired(bool value) {
        if(desired_==value)return;
        if(revision_==UINT64_MAX){desired_=false;attempts_=MaxAttempts;authorized_=false;return;}
        desired_=value;++revision_;
    }
    bool Refuse() {const bool before=desired_;Desired(false);authorized_=false;candidateValid_=false;return before;}
public:
    static constexpr uint32_t MaxAttempts=8;
    explicit ProjectedPageDemand(ProjectedDemandBinding binding,ProjectedDemandSettings settings={}):settings_(settings),binding_(binding){}
    bool DesiredFinePreparation() const {return desired_;}
    uint64_t Revision() const {return revision_;}
    uint64_t LastGeneration() const {return lastGeneration_;}
    double LeaveUpper() const {return settings_.leaveUpper;} // Read-only rail for a separate private return gate.
    uint32_t Attempts() const {return attempts_;}
    // Returns interest revocation. Cancel an unstaged owned request, retaining
    // its real worker debt until exit. No geometry/renderer ownership is changed.
    bool Observe(uint64_t now,const ProjectedDemandObservation& observation) {
        const bool fresh=observation.cameraGeneration>lastGeneration_;
        // Camera serials share one renderer namespace. Fence a rejected known
        // generation too, even when its source binding or math was refused.
        if(fresh)lastGeneration_=observation.cameraGeneration;
        const bool backward=seenTime_&&now<lastSeenTime_;
        if(!backward){seenTime_=true;lastSeenTime_=now;}
        if(!settings_.Valid()||!binding_.Valid()||now==UINT64_MAX||backward)return Refuse();
        if(!observation.validatedIdealPairBound||!(observation.binding==binding_)||
            !observation.cameraGeneration||observation.cameraGeneration==UINT64_MAX||
            !std::isfinite(observation.pairDiscrepancyUpper)||observation.pairDiscrepancyUpper<0)return Refuse();
        if(observation.cameraGeneration<lastGeneration_)return Refuse();
        // Re-reading a frame cannot advance time/freshness, restore refused
        // authorization, or provide a second attempt on the same generation.
        if(!fresh)return false;
        lastTime_=now;authorized_=true;
        bool next=desired_;
        if(observation.pairDiscrepancyUpper>=settings_.enterUpper)next=true;
        else if(observation.pairDiscrepancyUpper<=settings_.leaveUpper)next=false;
        else {candidateValid_=false;return false;}
        if(next==desired_){candidateValid_=false;return false;}
        if(!candidateValid_||candidate_!=next){candidate_=next;candidateValid_=true;since_=now;}
        const bool before=desired_;
        if(now-since_>=settings_.dwellMs){Desired(next);candidateValid_=false;}
        return before&&!desired_;
    }
    bool CanAttempt(uint64_t now,uint64_t currentGeneration,bool prepared,bool activeRequest,bool uploading) const {
        return settings_.Valid()&&binding_.Valid()&&authorized_&&seenTime_&&now==lastTime_&&now!=UINT64_MAX&&
            currentGeneration==lastGeneration_&&lastGeneration_>attemptedGeneration_&&
            desired_&&!prepared&&!activeRequest&&!uploading&&attempts_<MaxAttempts&&now>=retryAt_;
    }
    // Reserve only after CanAttempt returns true, before entering existing
    // request code; failures consume an attempt and cannot create a tight loop.
    void Attempted(uint64_t now) {
        if(attempts_<MaxAttempts)++attempts_;
        attemptedGeneration_=lastGeneration_;
        retryAt_=now>UINT64_MAX-settings_.retryMs?UINT64_MAX:now+settings_.retryMs;
    }
};
}
