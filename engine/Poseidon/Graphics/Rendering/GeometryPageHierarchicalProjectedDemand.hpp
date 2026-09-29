#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalLocalCut.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageProjectedSurfaceBound.hpp>

namespace Poseidon::GeometryPages::HierarchicalProjection
{
// Pure bounded preparation preference. CLOD simplified.error is its accumulated
// position/attribute selection metric, NOT certified symmetric surface distance.
// Even when the numeric projection calculation is bounded, this indicator does
// not establish pixel fidelity, visibility, cracks, raster quality or all passes.
inline constexpr uint32_t MaxViews=40;
struct Binding {
    HierarchicalIdentity identity;
    uint64_t worldEpoch=0,mountEpoch=0,sourceAdmissionEpoch=0,modelBirth=0,viewSetEpoch=0;
    uint64_t requiredViewMask=0;
    bool operator==(const Binding&) const=default;
};
struct Authority {
    Binding binding;
    uint64_t frameGeneration=0;
    std::array<uint64_t,MaxViews> viewGenerations{};
    bool operator==(const Authority&) const=default;
};
enum class ViewKind { Unknown,Main,SolarShadow,Reflection,Interior,LocalLight,GI,Auxiliary };
struct View {
    Binding binding;
    uint32_t id=0; // 1..40 owner-assigned IDs, no hardwired renderer universe.
    uint64_t frameGeneration=0,generation=0;
    ViewKind kind=ViewKind::Unknown;
    bool enabled=true,joinedNonJittered=false;
    // Same exact row-memory convention as ProjectedSurface; camera-relative
    // model, translation-free view and matching viewport from ONE joined cut.
    // Orthographic combined camera-relative light VP is accepted as projection
    // with identity view, matching the actual solar/interior renderer matrices.
    std::array<float,16> model{},view{},projection{};
    uint32_t viewportWidth=0,viewportHeight=0,viewportOriginX=0,viewportOriginY=0;
    float clipNear=0;
};
struct Input {
    Binding binding;
    std::span<const View> views;
    // Owner must supply the complete enabled set for this operation. The mask
    // proves agreement with supplied authority, not discovery of omitted passes.
    bool completeEnabledSet=false;
    double pixelIndicatorAllowance=1;
};
struct Demand {
    Authority authority;
    std::array<float,64> groupThresholds{};
    std::array<double,64> projectedBakedErrorIndicators{};
    uint32_t groupCount=0;
    uint64_t forcedFineViewMask=0;
    LocalizedHierarchicalDemand Localized() const {
        if(groupCount>groupThresholds.size())return {authority.binding.identity,{}};
        return {authority.binding.identity,std::span<const float>(groupThresholds).first(groupCount)};
    }
};
enum class Status { Ready,ForcedFine,Invalid,IdentityMismatch,Capacity,Unsupported };
namespace Detail {
namespace P=ProjectedSurface::Detail;
inline bool Epoch(uint64_t value){return value&&value!=UINT64_MAX;}
inline bool Bound(const Binding& b) {
    return Epoch(b.worldEpoch)&&Epoch(b.mountEpoch)&&Epoch(b.sourceAdmissionEpoch)&&Epoch(b.modelBirth)&&Epoch(b.viewSetEpoch)&&
        b.requiredViewMask&&(b.requiredViewMask>>MaxViews)==0;
}
// Admission rail for ordinary float similarity transforms. Residual tolerance
// accepts rounded rotations; it is NOT an exact uniform-scale proof. Projection
// sensitivity below uses every ACTUAL affine coefficient, retaining that residue.
inline bool Similarity(const std::array<float,16>& m,bool model) {
    if(!(model?P::ModelAffine(m):P::Affine(m)))return false;
    std::array<double,3> lengths{};
    for(unsigned i=0;i<3;++i)for(unsigned k=0;k<3;++k)lengths[i]+=double(m[i*4+k])*m[i*4+k];
    const double maximum=*std::max_element(lengths.begin(),lengths.end());
    const double minimum=*std::min_element(lengths.begin(),lengths.end());
    if(!std::isfinite(maximum)||minimum<1e-8||maximum>1e8||maximum-minimum>maximum*1e-5)return false;
    if(!model&&(std::abs(maximum-1)>1e-5||std::abs(minimum-1)>1e-5))return false;
    for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j) {
        double dot=0;for(unsigned k=0;k<3;++k)dot+=double(m[i*4+k])*m[j*4+k];
        if(std::abs(dot)>maximum*1e-5)return false;
    }
    return model||(m[12]==0&&m[13]==0&&m[14]==0);
}
inline bool FloatEndpoint(double value,bool upper,float& out) {
    if(!P::NormalOrZero(value)||std::abs(value)>10000)return false;
    out=float(value);
    if(upper?double(out)<value:double(out)>value)
        out=std::nextafter(out,upper?std::numeric_limits<float>::infinity():-std::numeric_limits<float>::infinity());
    return P::Float(out);
}
inline bool Envelope(const clodBounds& bounds,ProjectedSurface::Input& input) {
    if(!P::Float(bounds.radius)||bounds.radius<0||!P::Float(bounds.error)||bounds.error<0)return false;
    double extent;
    if(!P::Add(double(bounds.radius),double(bounds.error),true,extent))return false;
    // Error-expanded sphere box is conservative for this numeric indicator's
    // assumed displacement domain. It is not a certificate of actual mesh error.
    for(unsigned i=0;i<3;++i) {
        if(!P::Float(bounds.center[i]))return false;double lo,hi;
        if(!P::Add(double(bounds.center[i]),-extent,false,lo)||!P::Add(double(bounds.center[i]),extent,true,hi)||
            !FloatEndpoint(lo,false,input.minimum[i])||!FloatEndpoint(hi,true,input.maximum[i]))return false;
    }
    return true;
}
inline bool Orthographic(const std::array<float,16>& matrix) {
    for(float value:matrix)if(!P::Float(value))return false;
    // WGPU shadow/sky affine clip transform: x/y in[-1,1], depth in[0,1].
    // Signed scales and rotations/translation in a combined VP are intentional.
    return matrix[3]==0&&matrix[7]==0&&matrix[11]==0&&matrix[15]==1;
}
inline bool OrthographicIndicator(const ProjectedSurface::Input& input,double error,double& destination) {
    std::array<std::array<P::Interval,4>,3> transformed{},clip{};
    if(!P::Compose(input,transformed))return false;
    for(unsigned row=0;row<3;++row)for(unsigned column=0;column<4;++column) {
        P::Interval value{column==3?input.projection[12+row]:0,column==3?input.projection[12+row]:0};
        for(unsigned k=0;k<3;++k) {
            P::Interval term,next;
            if(!P::Mul(transformed[k][column],{input.projection[k*4+row],input.projection[k*4+row]},term)||
                !P::Add(value,term,next))return false;value=next;
        }
        clip[row][column]=value;
    }
    std::array<P::Interval,3> ranges{};std::array<double,3> sensitivity{};
    for(unsigned row=0;row<3;++row)
        if(!P::RangeAndSensitivity(clip[row],input,ranges[row],sensitivity[row])||sensitivity[row]<=0)return false;
    if(ranges[0].lo<=-1||ranges[0].hi>=1||ranges[1].lo<=-1||ranges[1].hi>=1||
        ranges[2].lo<=0||ranges[2].hi>=1)return false;
    double dx,dy,px,py,total;
    if(!P::Mul(error,sensitivity[0],true,dx)||!P::Mul(dx,double(input.viewportWidth)/2,true,px)||
        !P::Mul(error,sensitivity[1],true,dy)||!P::Mul(dy,double(input.viewportHeight)/2,true,py)||
        !P::Add(px,py,true,total))return false;
    destination=total;return true;
}
inline bool Indicator(const View& view,const clodBounds& bounds,double& destination) {
    ProjectedSurface::Input input;input.model=view.model;input.view=view.view;input.projection=view.projection;
    input.viewportWidth=view.viewportWidth;input.viewportHeight=view.viewportHeight;
    input.viewportOriginX=view.viewportOriginX;input.viewportOriginY=view.viewportOriginY;input.clipNear=view.clipNear;
    if(!Envelope(bounds,input)||!Similarity(view.model,true)||!Similarity(view.view,false)||
        !view.viewportWidth||view.viewportWidth>16384||!view.viewportHeight||view.viewportHeight>16384||
        view.viewportOriginX>16384||view.viewportOriginY>16384)return false;
    if(Orthographic(view.projection))return OrthographicIndicator(input,bounds.error,destination);
    if(!P::Float(view.clipNear)||view.clipNear<=0||!P::Projection(input))return false;
    std::array<std::array<P::Interval,4>,3> rows{};std::array<P::Interval,3> ranges{};std::array<double,3> norms{};
    if(!P::Compose(input,rows))return false;
    for(unsigned i=0;i<3;++i)if(!P::RangeAndSensitivity(rows[i],input,ranges[i],norms[i]))return false;
    if(ranges[2].lo<=view.clipNear)return false;
    const double x=std::max(std::abs(ranges[0].lo),std::abs(ranges[0].hi));
    const double y=std::max(std::abs(ranges[1].lo),std::abs(ranges[1].hi));double extent,ratio;
    if(!P::Mul(x,view.projection[0],true,extent)||!P::Div(extent,ranges[2].lo,true,ratio)||ratio>=1||
        !P::Mul(y,view.projection[5],true,extent)||!P::Div(extent,ranges[2].lo,true,ratio)||ratio>=1)return false;
    double px,py,total;
    if(!P::Axis(x,norms[0],norms[2],ranges[2].lo,bounds.error,view.projection[0],view.viewportWidth,px)||
        !P::Axis(y,norms[1],norms[2],ranges[2].lo,bounds.error,view.projection[5],view.viewportHeight,py)||
        !P::Add(px,py,true,total))return false;
    destination=total;return true;
}
}
// Invalid/stale/incomplete authority never changes destination. Unknown/near/
// clipped/unsupported active views publish conservative threshold-zero demand;
// the existing page planner still retains authored/root fallback until complete.
// The owner must obtain expected generations, matrices and joinedNonJittered from
// its captured immutable renderer tuples; counters/flags alone cannot prove this.
inline Status Build(const HierarchicalPackage& package,const Input& input,const Authority& expected,Demand& destination)
{
    if(!ValidHierarchicalMetadata(package)||!Detail::Bound(expected.binding)||!Detail::Epoch(expected.frameGeneration)||
        !input.completeEnabledSet||input.views.empty()||!ProjectedSurface::Detail::NormalOrZero(input.pixelIndicatorAllowance)||
        input.pixelIndicatorAllowance<=0||input.pixelIndicatorAllowance>10000)return Status::Invalid;
    if(input.views.size()>MaxViews)return Status::Capacity;
    if(!(package.identity==expected.binding.identity)||!(input.binding==expected.binding))return Status::IdentityMismatch;
    uint64_t seen=0,enabled=0;
    for(const auto& view:input.views) {
        if(!view.id||view.id>MaxViews)return Status::Invalid;
        const uint64_t bit=uint64_t(1)<<(view.id-1);if(seen&bit)return Status::Invalid;seen|=bit;
        if(!view.enabled)continue;enabled|=bit;
        if(!(view.binding==expected.binding)||view.frameGeneration!=expected.frameGeneration||
            !Detail::Epoch(view.generation)||view.generation!=expected.viewGenerations[view.id-1])return Status::IdentityMismatch;
    }
    if(enabled!=expected.binding.requiredViewMask)return Status::Invalid;
    for(uint32_t id=0;id<MaxViews;++id)
        if((enabled&(uint64_t(1)<<id))? !Detail::Epoch(expected.viewGenerations[id]):expected.viewGenerations[id]!=0)return Status::Invalid;
    Demand result;result.authority=expected;result.groupCount=uint32_t(package.groups.size());
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    result.forcedFineViewMask=enabled;destination=std::move(result);return Status::ForcedFine;
#else
    if constexpr(!std::numeric_limits<double>::is_iec559||!std::numeric_limits<float>::is_iec559||
        std::numeric_limits<double>::radix!=2||std::numeric_limits<float>::radix!=2||
        std::numeric_limits<double>::digits!=53||std::numeric_limits<float>::digits!=24) {
        result.forcedFineViewMask=enabled;destination=std::move(result);return Status::ForcedFine;
    }
#endif
    for(uint32_t g=0;g<result.groupCount;++g) {
        const float error=package.groups[g].simplified.error;
        result.groupThresholds[g]=error==FLT_MAX?0:error;
    }
    for(const auto& view:input.views)if(view.enabled) {
        if(!view.joinedNonJittered||view.kind==ViewKind::Unknown||uint32_t(view.kind)>uint32_t(ViewKind::Auxiliary)) {
            result.forcedFineViewMask|=uint64_t(1)<<(view.id-1);continue;
        }
        for(uint32_t g=0;g<result.groupCount;++g) {
            const auto& bounds=package.groups[g].simplified;if(bounds.error==FLT_MAX)continue;
            double indicator;
            if(!Detail::Indicator(view,bounds,indicator)) {result.forcedFineViewMask|=uint64_t(1)<<(view.id-1);break;}
            result.projectedBakedErrorIndicators[g]=std::max(result.projectedBakedErrorIndicators[g],indicator);
            if(indicator>input.pixelIndicatorAllowance)result.groupThresholds[g]=0;
        }
    }
    if(result.forcedFineViewMask)result.groupThresholds.fill(0);
    const auto status=result.forcedFineViewMask?Status::ForcedFine:Status::Ready;
    destination=std::move(result);return status;
}
}
