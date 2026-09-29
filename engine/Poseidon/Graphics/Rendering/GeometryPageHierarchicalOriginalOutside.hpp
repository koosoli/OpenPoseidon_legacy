#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalCombinedProjection.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageShapeExport.hpp>
#include <bit>
#include <tuple>

namespace Poseidon::GeometryPages::HierarchicalOriginalOutside
{
namespace H=HierarchicalProjection;
namespace C=HierarchicalCombinedProjection;
namespace P=ProjectedSurface::Detail;

struct View
{
    H::Binding binding;
    uint32_t id=0;
    uint64_t frameGeneration=0,generation=0;
    H::ViewKind kind=H::ViewKind::Unknown;
    bool enabled=true,joinedNonJittered=false;
    // Main/reflection use the actual joined camera-relative MODEL, zero-
    // translation VIEW and positive-Z PROJECTION as separate matrices. Light
    // views use the actual combined RH VP; neither path fabricates a float
    // flattened replacement for a renderer tuple.
    bool separateMainProjection=false;
    std::array<float,16> model{},view{},projection{},combinedLightVP{};
    uint32_t viewportWidth=0,viewportHeight=0,viewportOriginX=0,viewportOriginY=0;
    float clipNear=0;
};

// This checks only the fields serialized by the hierarchy disk codec. SVertex
// padding and unused vector lanes are not encoded and must not be memcmp'd.
inline bool ExactOriginalFinePageVertices(const HierarchicalPage& page,
    std::span<const SVertex> originalFine)
{
    if(page.clusters.empty()||page.clusters.size()>64||originalFine.empty()||originalFine.size()>4096)return false;
    const auto equalFloat=[](float a,float b){return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);};
    const auto equalVector=[&](const Vector3P& a,const Vector3P& b) {
        return equalFloat(a.X(),b.X())&&equalFloat(a.Y(),b.Y())&&equalFloat(a.Z(),b.Z());
    };
    for(const auto& cluster:page.clusters) {
        if(cluster.vertices.empty()||cluster.vertices.size()>64||
            cluster.originalVertexIds.size()!=cluster.vertices.size())return false;
        for(size_t i=0;i<cluster.vertices.size();++i) {
            const auto id=cluster.originalVertexIds[i];
            if(id>=originalFine.size())return false;
            const auto& a=cluster.vertices[i];const auto& b=originalFine[id];
            if(!equalVector(a.pos,b.pos)||!equalVector(a.norm,b.norm)||
               !equalVector(a.tangent,b.tangent)||!equalVector(a.binormal,b.binormal)||
               !equalFloat(a.t0.u,b.t0.u)||!equalFloat(a.t0.v,b.t0.v)||
               !equalFloat(a.t1.u,b.t1.u)||!equalFloat(a.t1.v,b.t1.v)||
               a.conform!=b.conform)return false;
        }
    }
    return true;
}

struct Token
{
    H::Authority views;
    uint64_t pageEpoch=0,requestId=0,originalSourceBytes=0;
    bool operator==(const Token&) const=default;
};
struct Input
{
    Token token;
    std::span<const View> views;
    bool completeEnabledSet=false;
};
struct Result
{
    Token token;
    std::array<float,3> originalMinimum{},originalMaximum{};
    uint64_t requiredViewMask=0,outsideViewMask=0,unknownViewMask=0;
};
enum class Status { Ready,Invalid,IdentityMismatch,Capacity };
namespace Detail
{
inline bool OriginalBox(const ShapeExport& original,std::array<float,3>& minimum,
    std::array<float,3>& maximum)
{
    bool first=true;
    for(const auto* mesh:{&original.coarse,&original.fine}) {
        if(mesh->vertices.empty()||mesh->vertices.size()>4096||
           mesh->positions.size()!=mesh->vertices.size()||
           mesh->indices.empty()||mesh->indices.size()%3||mesh->indices.size()>8192*3||
           mesh->materials.size()!=mesh->indices.size()/3)return false;
        for(auto index:mesh->indices)if(index>=mesh->vertices.size())return false;
        for(size_t i=0;i<mesh->vertices.size();++i) {
            const auto& v=mesh->vertices[i].pos;const auto& p=mesh->positions[i];
            const std::array<float,3> coordinates{v.X(),v.Y(),v.Z()};
            if(coordinates!=std::array<float,3>{p.x,p.y,p.z})return false;
            for(unsigned axis=0;axis<3;++axis) {
                if(!P::FloatWithin(coordinates[axis],1000000))return false;
                if(first)minimum[axis]=maximum[axis]=coordinates[axis];
                else {minimum[axis]=std::min(minimum[axis],coordinates[axis]);
                    maximum[axis]=std::max(maximum[axis],coordinates[axis]);}
            }
            first=false;
        }
    }
    return !first;
}
inline bool UpperNegative(const std::array<P::Interval,4>& first,
    const std::array<P::Interval,4>* second,bool subtract,
    const ProjectedSurface::Input& box,bool& outside)
{
    std::array<P::Interval,4> row{};
    for(unsigned i=0;i<4;++i) {
        if(!second)row[i]=first[i];
        else {
            const P::Interval term=subtract?P::Interval{-(*second)[i].hi,-(*second)[i].lo}:(*second)[i];
            if(!P::Add(first[i],term,row[i]))return false;
        }
    }
    P::Interval range{};double sensitivity=0;
    if(!P::RangeAndSensitivity(row,box,range,sensitivity))return false;
    outside=range.hi<0;return true;
}
inline bool Outside(const View& view,const std::array<float,3>& minimum,
    const std::array<float,3>& maximum,bool& outside)
{
    outside=false;
    if(!view.joinedNonJittered||view.kind==H::ViewKind::Unknown||
       uint32_t(view.kind)>uint32_t(H::ViewKind::Auxiliary)||
       !H::Detail::Similarity(view.model,true)||
       !view.viewportWidth||view.viewportWidth>16384||!view.viewportHeight||view.viewportHeight>16384||
       view.viewportOriginX>16384||view.viewportOriginY>16384||
       uint64_t(view.viewportOriginX)+view.viewportWidth>32768||
       uint64_t(view.viewportOriginY)+view.viewportHeight>32768)return false;
    ProjectedSurface::Input box;box.minimum=minimum;box.maximum=maximum;
    std::array<std::array<P::Interval,4>,4> rows{};
    if(view.separateMainProjection) {
        ProjectedSurface::Input projectionCheck;
        projectionCheck.projection=view.projection;projectionCheck.clipNear=view.clipNear;
        if((view.kind!=H::ViewKind::Main&&view.kind!=H::ViewKind::Reflection)||
           !H::Detail::Similarity(view.view,false)||!P::Float(view.clipNear)||
           view.clipNear<=0||!P::Projection(projectionCheck))return false;
        std::array<std::array<P::Interval,4>,4> modelView{};
        for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k) {
            P::Interval value{};
            for(unsigned i=0;i<4;++i) {
                P::Interval product,next;
                if(!P::Mul({view.model[k*4+i],view.model[k*4+i]},
                        {view.view[i*4+j],view.view[i*4+j]},product)||
                   !P::Add(value,product,next))return false;
                value=next;
            }
            modelView[j][k]=value;
        }
        for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k) {
            P::Interval value{};
            for(unsigned i=0;i<4;++i) {
                P::Interval product,next;
                if(!P::Mul(modelView[i][k],
                        {view.projection[i*4+j],view.projection[i*4+j]},product)||
                   !P::Add(value,product,next))return false;
                value=next;
            }
            rows[j][k]=value;
        }
    } else {
        if(!C::Detail::Shape(view.combinedLightVP))return false;
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
    }
    // One strict negative homogeneous half-space over the entire original
    // AABB excludes every captured original triangle. Partial clipping and
    // overlapping plane ranges remain Unknown. No raster/count claim follows.
    const auto plane=[&](unsigned a,const std::array<P::Interval,4>* b,bool subtract) {
        bool proved=false;
        if(!UpperNegative(rows[a],b,subtract,box,proved))return -1;
        return proved?1:0;
    };
    for(const auto spec:{std::tuple{3u,0u,false},std::tuple{3u,0u,true},
                         std::tuple{3u,1u,false},std::tuple{3u,1u,true},
                         std::tuple{3u,2u,true}}) {
        const auto [a,b,subtract]=spec;
        const int result=plane(a,&rows[b],subtract);
        if(result<0)return false;
        if(result>0){outside=true;return true;}
    }
    for(unsigned row:{2u,3u}) {
        const int result=plane(row,nullptr,false);
        if(result<0)return false;
        if(result>0){outside=true;return true;}
    }
    return true;
}
}
// Original-only conservative outside result. It does not prove that arbitrary
// decoded page vertices retain original positions; the owner must separately
// establish ExactOriginalFinePageVertices before using this to filter a page
// demand. Required mask is preserved even for outside views, and neither COUNT
// absence nor page retirement is inferred from this result.
inline Status Build(const HierarchicalPackage& package,const ShapeExport& original,
    const Input& input,const Token& expected,Result& destination)
{
    if(!ValidHierarchicalMetadata(package)||!H::Detail::Bound(expected.views.binding)||
       !H::Detail::Epoch(expected.views.frameGeneration)||
       !H::Detail::Epoch(expected.pageEpoch)||!H::Detail::Epoch(expected.requestId)||
       !expected.originalSourceBytes||expected.originalSourceBytes>128*1024||
       !input.completeEnabledSet||input.views.empty())return Status::Invalid;
    if(input.views.size()>H::MaxViews)return Status::Capacity;
    if(!(input.token==expected)||!(package.identity==expected.views.binding.identity)||
       !(original.source==package.identity.source))return Status::IdentityMismatch;
    uint64_t seen=0,enabled=0;
    for(const auto& view:input.views) {
        if(!view.id||view.id>H::MaxViews)return Status::Invalid;
        const uint64_t bit=uint64_t(1)<<(view.id-1);
        if(seen&bit)return Status::Invalid;
        seen|=bit;
        if(!view.enabled)continue;
        enabled|=bit;
        if(!(view.binding==expected.views.binding)||
           view.frameGeneration!=expected.views.frameGeneration||
           !H::Detail::Epoch(view.generation)||
           view.generation!=expected.views.viewGenerations[view.id-1])return Status::IdentityMismatch;
    }
    if(enabled!=expected.views.binding.requiredViewMask)return Status::Invalid;
    for(uint32_t id=0;id<H::MaxViews;++id)
        if((enabled&(uint64_t(1)<<id))?!H::Detail::Epoch(expected.views.viewGenerations[id]):
            expected.views.viewGenerations[id]!=0)return Status::Invalid;
    Result result;result.token=expected;result.requiredViewMask=enabled;
    if(!Detail::OriginalBox(original,result.originalMinimum,result.originalMaximum))return Status::Invalid;
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    result.unknownViewMask=enabled;destination=result;return Status::Ready;
#else
    if constexpr(!std::numeric_limits<double>::is_iec559||!std::numeric_limits<float>::is_iec559||
        std::numeric_limits<double>::radix!=2||std::numeric_limits<float>::radix!=2||
        std::numeric_limits<double>::digits!=53||std::numeric_limits<float>::digits!=24) {
        result.unknownViewMask=enabled;destination=result;return Status::Ready;
    }
#endif
    for(const auto& view:input.views)if(view.enabled) {
        const uint64_t bit=uint64_t(1)<<(view.id-1);bool outside=false;
        if(Detail::Outside(view,result.originalMinimum,result.originalMaximum,outside)&&outside)
            result.outsideViewMask|=bit;
        else result.unknownViewMask|=bit;
    }
    destination=result;return Status::Ready;
}
}
