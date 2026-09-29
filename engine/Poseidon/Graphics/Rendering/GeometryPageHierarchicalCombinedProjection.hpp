#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalProjectedDemand.hpp>

namespace Poseidon::GeometryPages::HierarchicalCombinedProjection
{
// Combined camera-relative light VP, in the exact row-memory bytes uploaded to
// the renderer. The light may look down RH -Z; view translation is already in VP.
// This is a bounded indicator for CLOD's baked error metric, never a certified
// geometry-error or all-view image bound. An outside/clipped group forces Fine.
namespace H=HierarchicalProjection;
namespace P=ProjectedSurface::Detail;
struct View
{
    H::Binding binding;
    uint32_t id=0;
    uint64_t frameGeneration=0,generation=0;
    H::ViewKind kind=H::ViewKind::Unknown;
    bool enabled=true,joinedNonJittered=false;
    std::array<float,16> model{},combinedLightVP{};
    uint32_t viewportWidth=0,viewportHeight=0,viewportOriginX=0,viewportOriginY=0;
};
struct Input
{
    H::Binding binding;
    std::span<const View> views;
    bool completeEnabledSet=false;
    double pixelIndicatorAllowance=1;
};
namespace Detail
{
inline double NormSquared(const std::array<double,3>& v)
{return v[0]*v[0]+v[1]*v[1]+v[2]*v[2];}
inline double Dot(const std::array<double,3>& a,const std::array<double,3>& b)
{return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline bool Shape(const std::array<float,16>& m)
{
    for(float value:m)if(!P::Float(value))return false;
    std::array<std::array<double,3>,4> row{};
    for(unsigned j=0;j<4;++j)for(unsigned k=0;k<3;++k)row[j][k]=m[k*4+j];
    const double x=NormSquared(row[0]),y=NormSquared(row[1]),z=NormSquared(row[2]),w=NormSquared(row[3]);
    if(!std::isfinite(x+y+z+w)||x<1e-8||y<1e-8||z<1e-8)return false;
    const auto orthogonal=[&](unsigned a,unsigned b) {
        return std::abs(Dot(row[a],row[b]))<=1e-5*std::sqrt(NormSquared(row[a])*NormSquared(row[b]));
    };
    if(!orthogonal(0,1))return false;
    if(w==0) {
        // Signed, rotated and translated affine light VP (including Ortho).
        return m[15]==1&&orthogonal(0,2)&&orthogonal(1,2);
    }
    // Symmetric RH perspective * rigid light view. The W row is a unit view
    // direction, XY are perpendicular to it, and depth is parallel to W.
    // This deliberately refuses skew/off-axis projection and nonrigid view.
    if(std::abs(w-1)>1e-4||!orthogonal(0,3)||!orthogonal(1,3))return false;
    const double factor=Dot(row[2],row[3])/w;
    if(!std::isfinite(factor)||factor<=0)return false;
    for(unsigned k=0;k<3;++k)
        if(std::abs(row[2][k]-factor*row[3][k])>1e-5*std::sqrt(z))return false;
    return true;
}
inline bool PositiveMargin(const std::array<P::Interval,4>& first,
    const std::array<P::Interval,4>& second,bool subtract,
    const ProjectedSurface::Input& envelope)
{
    std::array<P::Interval,4> row{};
    for(unsigned i=0;i<4;++i) {
        const P::Interval term=subtract?P::Interval{-second[i].hi,-second[i].lo}:second[i];
        if(!P::Add(first[i],term,row[i]))return false;
    }
    P::Interval range{};double sensitivity=0;
    return P::RangeAndSensitivity(row,envelope,range,sensitivity)&&range.lo>0;
}
inline bool Indicator(const View& view,const clodBounds& bounds,double& destination)
{
    if(!H::Detail::Similarity(view.model,true)||!Shape(view.combinedLightVP)||
       !view.viewportWidth||view.viewportWidth>16384||!view.viewportHeight||view.viewportHeight>16384||
       view.viewportOriginX>16384||view.viewportOriginY>16384||
       uint64_t(view.viewportOriginX)+view.viewportWidth>32768||
       uint64_t(view.viewportOriginY)+view.viewportHeight>32768)return false;
    ProjectedSurface::Input envelope;
    if(!H::Detail::Envelope(bounds,envelope))return false;
    envelope.model=view.model;envelope.viewportWidth=view.viewportWidth;
    envelope.viewportHeight=view.viewportHeight;
    envelope.viewportOriginX=view.viewportOriginX;
    envelope.viewportOriginY=view.viewportOriginY;
    std::array<std::array<P::Interval,4>,4> rows{};
    for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k) {
        P::Interval value{};
        for(unsigned i=0;i<4;++i) {
            P::Interval product,next;
            if(!P::Mul({view.model[k*4+i],view.model[k*4+i]},
                    {view.combinedLightVP[i*4+j],view.combinedLightVP[i*4+j]},product)||
               !P::Add(value,product,next))return false;
            value=next;
        }
        rows[j][k]=value;
    }
    std::array<P::Interval,4> ranges{};std::array<double,4> sensitivity{};
    for(unsigned j=0;j<4;++j)
        if(!P::RangeAndSensitivity(rows[j],envelope,ranges[j],sensitivity[j]))return false;
    if(ranges[3].lo<=0||sensitivity[0]<=0||sensitivity[1]<=0)return false;
    // For positive W, zero-to-one homogeneous containment is equivalent to
    // these six positive affine half-spaces. Combining coefficient intervals
    // before evaluating the box preserves Z/W correlation at the far plane.
    if(ranges[2].lo<=0||
       !PositiveMargin(rows[3],rows[0],false,envelope)||
       !PositiveMargin(rows[3],rows[0],true,envelope)||
       !PositiveMargin(rows[3],rows[1],false,envelope)||
       !PositiveMargin(rows[3],rows[1],true,envelope)||
       !PositiveMargin(rows[3],rows[2],true,envelope))return false;
    const double extentX=std::max(std::abs(ranges[0].lo),std::abs(ranges[0].hi));
    const double extentY=std::max(std::abs(ranges[1].lo),std::abs(ranges[1].hi));
    double px,py,total;
    if(!P::Axis(extentX,sensitivity[0],sensitivity[3],ranges[3].lo,bounds.error,1,view.viewportWidth,px)||
       !P::Axis(extentY,sensitivity[1],sensitivity[3],ranges[3].lo,bounds.error,1,view.viewportHeight,py)||
       !P::Add(px,py,true,total))return false;
    destination=total;return true;
}
}
inline H::Status Build(const HierarchicalPackage& package,const Input& input,
    const H::Authority& expected,H::Demand& destination)
{
    if(!ValidHierarchicalMetadata(package)||!H::Detail::Bound(expected.binding)||
       !H::Detail::Epoch(expected.frameGeneration)||!input.completeEnabledSet||input.views.empty()||
       !P::NormalOrZero(input.pixelIndicatorAllowance)||input.pixelIndicatorAllowance<=0||
       input.pixelIndicatorAllowance>10000)return H::Status::Invalid;
    if(input.views.size()>H::MaxViews)return H::Status::Capacity;
    if(!(package.identity==expected.binding.identity)||!(input.binding==expected.binding))return H::Status::IdentityMismatch;
    uint64_t seen=0,enabled=0;
    for(const auto& view:input.views) {
        if(!view.id||view.id>H::MaxViews)return H::Status::Invalid;
        const uint64_t bit=uint64_t(1)<<(view.id-1);
        if(seen&bit)return H::Status::Invalid;
        seen|=bit;
        if(!view.enabled)continue;
        enabled|=bit;
        if(!(view.binding==expected.binding)||view.frameGeneration!=expected.frameGeneration||
           !H::Detail::Epoch(view.generation)||view.generation!=expected.viewGenerations[view.id-1])
            return H::Status::IdentityMismatch;
    }
    if(enabled!=expected.binding.requiredViewMask)return H::Status::Invalid;
    for(uint32_t id=0;id<H::MaxViews;++id)
        if((enabled&(uint64_t(1)<<id))?!H::Detail::Epoch(expected.viewGenerations[id]):
            expected.viewGenerations[id]!=0)return H::Status::Invalid;
    H::Demand result;result.authority=expected;result.groupCount=uint32_t(package.groups.size());
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    result.forcedFineViewMask=enabled;destination=std::move(result);return H::Status::ForcedFine;
#else
    if constexpr(!std::numeric_limits<double>::is_iec559||!std::numeric_limits<float>::is_iec559||
        std::numeric_limits<double>::radix!=2||std::numeric_limits<float>::radix!=2||
        std::numeric_limits<double>::digits!=53||std::numeric_limits<float>::digits!=24) {
        result.forcedFineViewMask=enabled;destination=std::move(result);return H::Status::ForcedFine;
    }
#endif
    for(uint32_t g=0;g<result.groupCount;++g) {
        const float error=package.groups[g].simplified.error;
        result.groupThresholds[g]=error==FLT_MAX?0:error;
    }
    for(const auto& view:input.views)if(view.enabled) {
        if(!view.joinedNonJittered||view.kind==H::ViewKind::Unknown||
           uint32_t(view.kind)>uint32_t(H::ViewKind::Auxiliary)) {
            result.forcedFineViewMask|=uint64_t(1)<<(view.id-1);continue;
        }
        for(uint32_t g=0;g<result.groupCount;++g) {
            const auto& bounds=package.groups[g].simplified;
            if(bounds.error==FLT_MAX)continue;
            double indicator=0;
            if(!Detail::Indicator(view,bounds,indicator)) {
                result.forcedFineViewMask|=uint64_t(1)<<(view.id-1);break;
            }
            result.projectedBakedErrorIndicators[g]=std::max(result.projectedBakedErrorIndicators[g],indicator);
            if(indicator>input.pixelIndicatorAllowance)result.groupThresholds[g]=0;
        }
    }
    if(result.forcedFineViewMask)result.groupThresholds.fill(0);
    const auto status=result.forcedFineViewMask?H::Status::ForcedFine:H::Status::Ready;
    destination=std::move(result);return status;
}
}
