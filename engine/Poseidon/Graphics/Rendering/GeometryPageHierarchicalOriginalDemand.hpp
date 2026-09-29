#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalOriginalOutside.hpp>

namespace Poseidon::GeometryPages::HierarchicalOriginalDemand
{
namespace H=HierarchicalProjection;
namespace C=HierarchicalCombinedProjection;
namespace O=HierarchicalOriginalOutside;
struct Input
{
    O::Input views;
    double pixelIndicatorAllowance=1;
    // Owner-only invariant: every currently uploaded hierarchy page was checked
    // against admitted Fine IDs/serialized fields, and every future page must
    // pass the same gate before publication. A mere manifest/source hash is not
    // enough. Without this guarantee, outside views remain demand participants.
    bool noNewPagePositionsVerified=false;
};
struct Result
{
    O::Result outside;
    H::Demand demand;
    O::Token token;
    uint64_t requiredViewMask=0,outsideViewMask=0,forcedFineViewMask=0;
    std::array<float,64> groupThresholds{};
    std::array<double,64> projectedBakedErrorIndicators{};
    uint32_t groupCount=0;
    LocalizedHierarchicalDemand Localized() const
    {
        if(groupCount>groupThresholds.size())return {token.views.binding.identity,{}};
        return {token.views.binding.identity,std::span<const float>(groupThresholds).first(groupCount)};
    }
};
enum class Status { Ready,ForcedFine,Invalid,IdentityMismatch,Capacity };
inline Status Build(const HierarchicalPackage& package,const ShapeExport& original,
    const Input& input,const O::Token& expected,Result& destination)
{
    if(!ProjectedSurface::Detail::NormalOrZero(input.pixelIndicatorAllowance)||
       input.pixelIndicatorAllowance<=0||input.pixelIndicatorAllowance>10000)return Status::Invalid;
    O::Result outside;
    switch(O::Build(package,original,input.views,expected,outside)) {
        case O::Status::Invalid:return Status::Invalid;
        case O::Status::IdentityMismatch:return Status::IdentityMismatch;
        case O::Status::Capacity:return Status::Capacity;
        case O::Status::Ready:break;
    }
    Result result;result.token=outside.token;
    result.requiredViewMask=outside.requiredViewMask;
    result.outsideViewMask=outside.outsideViewMask;
    result.groupCount=uint32_t(package.groups.size());
    for(uint32_t g=0;g<result.groupCount;++g) {
        const float error=package.groups[g].simplified.error;
        result.groupThresholds[g]=error==FLT_MAX?0:error;
    }
    const auto publish=[&](Status status) {
        result.outside=outside;
        result.demand.authority=expected.views;
        result.demand.groupThresholds=result.groupThresholds;
        result.demand.projectedBakedErrorIndicators=result.projectedBakedErrorIndicators;
        result.demand.groupCount=result.groupCount;
        result.demand.forcedFineViewMask=result.forcedFineViewMask;
        destination=result;return status;
    };
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    result.forcedFineViewMask=result.requiredViewMask;
    result.groupThresholds.fill(0);return publish(Status::ForcedFine);
#else
    if constexpr(!std::numeric_limits<double>::is_iec559||!std::numeric_limits<float>::is_iec559||
        std::numeric_limits<double>::radix!=2||std::numeric_limits<float>::radix!=2||
        std::numeric_limits<double>::digits!=53||std::numeric_limits<float>::digits!=24) {
        result.forcedFineViewMask=result.requiredViewMask;
        result.groupThresholds.fill(0);return publish(Status::ForcedFine);
    }
#endif
    for(const auto& view:input.views.views)if(view.enabled) {
        const uint64_t bit=uint64_t(1)<<(view.id-1);
        if(!view.joinedNonJittered||view.kind==H::ViewKind::Unknown||
           uint32_t(view.kind)>uint32_t(H::ViewKind::Auxiliary)) {
            result.forcedFineViewMask|=bit;continue;
        }
        if(input.noNewPagePositionsVerified&&(outside.outsideViewMask&bit))continue;
        const bool separate=view.separateMainProjection&&
            (view.kind==H::ViewKind::Main||view.kind==H::ViewKind::Reflection);
        const bool combined=!view.separateMainProjection&&
            view.kind!=H::ViewKind::Main&&view.kind!=H::ViewKind::Reflection;
        if(!separate&&!combined) {result.forcedFineViewMask|=bit;continue;}
        bool supported=true;
        for(uint32_t g=0;g<result.groupCount;++g) {
            const auto& bounds=package.groups[g].simplified;
            if(bounds.error==FLT_MAX)continue;
            double indicator=0;
            if(separate) {
                H::View numeric;
                numeric.model=view.model;numeric.view=view.view;numeric.projection=view.projection;
                numeric.viewportWidth=view.viewportWidth;numeric.viewportHeight=view.viewportHeight;
                numeric.viewportOriginX=view.viewportOriginX;numeric.viewportOriginY=view.viewportOriginY;
                numeric.clipNear=view.clipNear;
                supported=H::Detail::Indicator(numeric,bounds,indicator);
            } else if(combined) {
                C::View numeric;
                numeric.model=view.model;numeric.combinedLightVP=view.combinedLightVP;
                numeric.viewportWidth=view.viewportWidth;numeric.viewportHeight=view.viewportHeight;
                numeric.viewportOriginX=view.viewportOriginX;numeric.viewportOriginY=view.viewportOriginY;
                supported=C::Detail::Indicator(numeric,bounds,indicator);
            } else supported=false;
            if(!supported)break;
            result.projectedBakedErrorIndicators[g]=
                std::max(result.projectedBakedErrorIndicators[g],indicator);
            if(indicator>input.pixelIndicatorAllowance)result.groupThresholds[g]=0;
        }
        if(!supported)result.forcedFineViewMask|=bit;
    }
    if(result.forcedFineViewMask)result.groupThresholds.fill(0);
    const auto status=result.forcedFineViewMask?Status::ForcedFine:Status::Ready;
    return publish(status);
}
}
