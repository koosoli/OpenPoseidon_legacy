#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalPackage.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace Poseidon::GeometryPages::HierarchicalBinaryDemand
{
// Private, fixed-source camera preference only. The owner has already joined the
// completed renderer packet, immutable source/page authority and complete view
// union. Baked CLOD error remains a selection heuristic, not a certified surface
// or raster-pixel error. A decision never authorizes upload, selection or release.
struct Binding
{
    HierarchicalIdentity identity;
    uint64_t privateOwnerEpoch=0,sourceAdmissionEpoch=0,pageEpoch=0,modelBirth=0;
    bool operator==(const Binding&) const = default;
    bool Valid() const
    {
        const auto nonzero=[](const auto& bytes) {
            return std::any_of(bytes.begin(),bytes.end(),[](uint8_t b){return b!=0;});
        };
        // modelBirth is the fixed admitted fallback birth, not the currently
        // selected root/fine model. A camera-driven switch cannot change it.
        const auto epoch=[](uint64_t value){return value&&value!=UINT64_MAX;};
        return epoch(privateOwnerEpoch)&&epoch(sourceAdmissionEpoch)&&epoch(pageEpoch)&&epoch(modelBirth)&&
            identity.adapterVersion&&identity.source.producerVersion&&
            identity.source.vertexLayout&&identity.source.materialMapping&&
            nonzero(identity.source.sourceSha256)&&nonzero(identity.packageSha256);
    }
};
struct Observation
{
    Binding binding;
    uint64_t frameGeneration=0,requiredViewMask=0;
    // wantsFine comes from comparing the complete localized requested cut with
    // the package's complete root key. maximumIndicator alone cannot do this:
    // a child group may be irrelevant once a coarser ancestor is selected.
    bool wantsFine=false,forcedFine=false,currentlyRoot=true,pending=false;
    double maximumIndicator=0,allowance=4;
};
enum class Decision { Hold,Refine,Coarsen,Refused };
struct Counters
{
    uint64_t observations=0,refused=0,forcedFine=0,refine=0,coarsen=0;
    bool operator==(const Counters&) const = default;
};
class Policy
{
    Binding binding_;
    uint64_t lastGeneration_=0,requiredViewMask_=0;
    uint32_t safeRootObservations_=0;
    bool lastWasRoot_=true,haveMode_=false;
    Counters counters_;
    static void Increment(uint64_t& value) {if(value!=UINT64_MAX)++value;}
    Decision Refuse()
    {
        safeRootObservations_=0;Increment(counters_.refused);return Decision::Refused;
    }
public:
    static constexpr double RefineAllowance=4,CoarsenAllowance=3;
    static constexpr uint32_t RequiredSafeObservations=3;
    explicit Policy(Binding binding):binding_(std::move(binding)) {}
    const Counters& Stats() const {return counters_;}
    uint64_t LastGeneration() const {return lastGeneration_;}
    uint32_t SafeRootObservations() const {return safeRootObservations_;}
    // Owner limits observations to <=10Hz and supplies only successful, complete
    // returned-frame associations. While page/model/worker work is pending, do
    // not consume a newer frame or change its frozen demand generation.
    Decision Observe(const Observation& observation)
    {
        if(observation.pending)return Decision::Hold;
        const bool fresh=observation.frameGeneration&&
            observation.frameGeneration!=UINT64_MAX&&observation.frameGeneration>lastGeneration_;
        // Fence a rejected *known* generation too. Replaying the same packet
        // after repairing its claimed identity must not restore coarsen progress.
        if(fresh)lastGeneration_=observation.frameGeneration;
        if(!fresh||!binding_.Valid()||!(observation.binding==binding_)||
           !observation.requiredViewMask||!(observation.requiredViewMask&1)||
           (observation.requiredViewMask>>40)||
           !std::isfinite(observation.maximumIndicator)||observation.maximumIndicator<0||
           !std::isfinite(observation.allowance)||
           observation.allowance!=(observation.currentlyRoot?RefineAllowance:CoarsenAllowance))
            return Refuse();
        Increment(counters_.observations);
        if(requiredViewMask_!=observation.requiredViewMask||
           (haveMode_&&lastWasRoot_!=observation.currentlyRoot))safeRootObservations_=0;
        requiredViewMask_=observation.requiredViewMask;
        lastWasRoot_=observation.currentlyRoot;haveMode_=true;
        if(observation.forcedFine)Increment(counters_.forcedFine);
        if(observation.wantsFine||observation.forcedFine) {
            safeRootObservations_=0;
            if(!observation.currentlyRoot)return Decision::Hold;
            Increment(counters_.refine);return Decision::Refine;
        }
        if(observation.currentlyRoot) {safeRootObservations_=0;return Decision::Hold;}
        // Only three consecutive fresh known unions at the lower allowance can
        // prefer roots. Unknown required views force Fine above; source or tuple
        // refusals never provide evidence for coarsening.
        if(safeRootObservations_<RequiredSafeObservations)++safeRootObservations_;
        if(safeRootObservations_<RequiredSafeObservations)return Decision::Hold;
        safeRootObservations_=0;Increment(counters_.coarsen);return Decision::Coarsen;
    }
};
} // namespace Poseidon::GeometryPages::HierarchicalBinaryDemand
