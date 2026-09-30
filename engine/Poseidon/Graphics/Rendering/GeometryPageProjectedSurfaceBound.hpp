#pragma once
#include "GeometryPagePrototype.hpp"
#include <algorithm>
#include <utility>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Poseidon::GeometryPages::ProjectedSurface
{
// Pure prerequisite, NOT a camera capture, certificate parser, or runtime policy.
// Caller must provide the immutable successful surface certificate's full binding
// and an AABB enclosing BOTH actual packed closed-triangle unions. This helper
// cannot authenticate a descriptor or prove that the supplied envelope encloses
// geometry. One joined tuple must supply actual camera-relative model, translation-
// free view, projection and active viewport; never mix later live camera getters.
struct Binding {
    SourceIdentity originalSource;CacheIdentity selectedKey;
    std::array<uint8_t,32> coarsePackedSha256{},finePackedSha256{};
    std::array<uint32_t,64> coarseCut{},fineCut{};
    uint32_t coarseCutCount=0,fineCutCount=0,coarseThresholdBits=0,fineThresholdBits=0;
    uint32_t certificateAlgorithmVersion=0;
    uint64_t fineToCoarseUpperBits=0,coarseToFineUpperBits=0,hausdorffUpperBits=0;
    bool operator==(const Binding&) const=default;
};
struct Cut {uint64_t cameraGeneration=0,modelBirth=0,certificateGeneration=0;bool operator==(const Cut&) const=default;};
struct Input {
    Binding binding;Cut cut;
    // Exact GfxMatrix row-memory bytes: WGSL reads them as columns and multiplies
    // proj * view * world * column position. Model translation already subtracts
    // CameraEntry.pos; view translation is zero, as in PushSceneCamera.
    std::array<float,16> model{},view{},projection{};
    std::array<float,3> minimum{},maximum{};
    uint32_t viewportWidth=0,viewportHeight=0,viewportOriginX=0,viewportOriginY=0;
    float clipNear=0;double surfaceDistanceUpper=0;
};
struct Result {
    Input tuple;double depthMinimum=0,pixelXUpper=0,pixelYUpper=0,euclideanPixelUpper=0;
};
enum class Status {Bounded,Invalid,IdentityMismatch,Unsupported,NearIntersection,OutsideViewport};
namespace Detail {
struct Interval {double lo=0,hi=0;};
inline bool NormalOrZero(double v){const auto bits=std::bit_cast<uint64_t>(v)&0x7fffffffffffffffull;
    // Bit classification must precede FP comparisons: DAZ may compare an actual
    // subnormal operand as zero. Reject such inputs/endpoints, never certify them.
    return bits==0||(bits>=0x0010000000000000ull&&bits<0x7ff0000000000000ull);}
inline bool FloatWithin(float v,double limit){const auto bits=std::bit_cast<uint32_t>(v)&0x7fffffff;
    return bits<0x7f800000&&(bits==0||bits>=0x00800000)&&std::abs(double(v))<=limit;}
inline bool Float(float v){return FloatWithin(v,10000);}
inline bool Rounded(double v,bool upper,bool exactZero,double& out){
    if(!NormalOrZero(v)||(v==0&&!exactZero))return false;
    if(v==0){out=0;return true;}
    out=std::nextafter(v,upper?std::numeric_limits<double>::infinity():-std::numeric_limits<double>::infinity());
    // A flushed adjacent subnormal must not masquerade as an exact zero
    // endpoint. For nonzero starting values, conservatively refuse either.
    return NormalOrZero(out)&&(std::bit_cast<uint64_t>(out)&0x7fffffffffffffffull)!=0;
}
inline bool Add(double a,double b,bool upper,double& out){
    if(!NormalOrZero(a)||!NormalOrZero(b))return false;
    volatile double value=a+b;return Rounded(value,upper,a==-b,out);
}
inline bool Mul(double a,double b,bool upper,double& out){
    if(!NormalOrZero(a)||!NormalOrZero(b))return false;
    volatile double value=a*b;return Rounded(value,upper,a==0||b==0,out);
}
inline bool Div(double a,double b,bool upper,double& out){
    if(!NormalOrZero(a)||!NormalOrZero(b)||b<=0)return false;
    volatile double value=a/b;return Rounded(value,upper,a==0,out);
}
inline bool Add(Interval a,Interval b,Interval& out){return Add(a.lo,b.lo,false,out.lo)&&Add(a.hi,b.hi,true,out.hi);}
inline bool Mul(Interval a,Interval b,Interval& out){
    out.lo=std::numeric_limits<double>::infinity();out.hi=-out.lo;
    for(double x:{a.lo,a.hi})for(double y:{b.lo,b.hi}){double lo,hi;
        if(!Mul(x,y,false,lo)||!Mul(x,y,true,hi))return false;
        out.lo=std::min(out.lo,lo);out.hi=std::max(out.hi,hi);}
    return true;
}
inline bool NonzeroHash(const std::array<uint8_t,32>& bytes){for(auto b:bytes)if(b)return true;return false;}
inline bool BoundIdentity(const Binding& b){
    // Version1: single anchor. Version2: closed-triangle convex witness.
    // Version3: dyadic closed-source subdivision with interval target anchors.
    // All supply an authenticated conservative distance, with exact binding.
    if((b.certificateAlgorithmVersion!=1&&b.certificateAlgorithmVersion!=2&&
        b.certificateAlgorithmVersion!=3)||!NonzeroHash(b.originalSource.sourceSha256)||
       !NonzeroHash(b.selectedKey.source.sourceSha256)||!NonzeroHash(b.coarsePackedSha256)||!NonzeroHash(b.finePackedSha256)||
       !b.originalSource.producerVersion||!b.originalSource.vertexLayout||!b.originalSource.materialMapping||
       b.originalSource.coarseRepresentation==b.originalSource.fineRepresentation||
       !b.selectedKey.source.producerVersion||!b.selectedKey.source.vertexLayout||!b.selectedKey.source.materialMapping||
       b.selectedKey.source.coarseRepresentation==b.selectedKey.source.fineRepresentation||
       !b.selectedKey.formatVersion||!b.selectedKey.algorithmVersion||!b.selectedKey.packing.clusterVertices||
       !b.selectedKey.packing.clusterTriangles||!b.selectedKey.packing.pageBytes||
       !b.coarseCutCount||b.coarseCutCount>64||!b.fineCutCount||b.fineCutCount>64)return false;
    for(const auto pair:{std::pair{&b.coarseCut,b.coarseCutCount},std::pair{&b.fineCut,b.fineCutCount}})
        for(uint32_t i=1;i<pair.second;++i)if((*pair.first)[i-1]>=(*pair.first)[i])return false;
    const auto forward=std::bit_cast<double>(b.fineToCoarseUpperBits);
    const auto reverse=std::bit_cast<double>(b.coarseToFineUpperBits);
    const auto maximum=std::bit_cast<double>(b.hausdorffUpperBits);
    return NormalOrZero(forward)&&NormalOrZero(reverse)&&NormalOrZero(maximum)&&
        forward>=0&&reverse>=0&&maximum>=0&&maximum<=10000&&maximum==std::max(forward,reverse);
}
inline bool Affine(const std::array<float,16>& m){
    for(float v:m)if(!Float(v))return false;
    return m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1;
}
// Camera-relative MODEL translation is in world-coordinate units, unlike
// linear coefficients. Keep the existing1e4 domain for basis/view/geometry and
// projection; permit only model indices12..14 up to1e6. The same outward interval
// operations still refuse nonfinite/subnormal/overflow/underflow outcomes.
inline bool ModelAffine(const std::array<float,16>& m){
    for(size_t i=0;i<m.size();++i)
        if(!(i>=12&&i<=14?FloatWithin(m[i],1000000):Float(m[i])))return false;
    return m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1;
}
inline bool Projection(const Input& v){
    for(float n:v.projection)if(!Float(n))return false;
    for(unsigned i=0;i<16;++i)if(i!=0&&i!=5&&i!=10&&i!=11&&i!=14&&v.projection[i]!=0)return false;
    // ConvertProjectionMatrix + WGPU infinite-far depth override. Reversed-Z
    // reverse_z changes only depth; x/y homogeneous w is positive view z.
    return v.projection[0]>0&&v.projection[5]>0&&v.projection[10]==1&&
           v.projection[11]==1&&v.projection[14]==-v.clipNear;
}
inline bool Compose(const Input& v,std::array<std::array<Interval,4>,3>& rows){
    for(unsigned j=0;j<3;++j)for(unsigned k=0;k<4;++k){Interval value{};
        for(unsigned i=0;i<4;++i){Interval product,next;
            if(!Mul({v.model[k*4+i],v.model[k*4+i]},{v.view[i*4+j],v.view[i*4+j]},product)||
               !Add(value,product,next))return false;value=next;}
        rows[j][k]=value;}
    return true;
}
inline bool RangeAndSensitivity(const std::array<Interval,4>& row,const Input& v,Interval& range,double& sensitivity){
    range=row[3];sensitivity=0;
    for(unsigned i=0;i<3;++i){Interval term,next;
        if(!Mul(row[i],{v.minimum[i],v.maximum[i]},term)||!Add(range,term,next))return false;range=next;
        const auto magnitude=std::max(std::abs(row[i].lo),std::abs(row[i].hi));double sum;
        if(!Add(sensitivity,magnitude,true,sum))return false;sensitivity=sum;}
    return true;
}
inline bool Axis(double extent,double sensitivity,double depthSensitivity,double z,double error,double scale,uint32_t pixels,double& out){
    double dx,dz,zSquared,first,second,product,total,scaled;
    if(!Mul(error,sensitivity,true,dx)||!Mul(error,depthSensitivity,true,dz)||!Mul(z,z,false,zSquared)||
       !Div(dx,z,true,first)||!Mul(extent,dz,true,product)||!Div(product,zSquared,true,second)||
       !Add(first,second,true,total)||!Mul(total,scale,true,scaled)||!Mul(scaled,double(pixels)/2,true,out))return false;
    return out>=0;
}
}
// For p/q in the supplied union box with ||p-q|| <= E, camera row changes are
// bounded by E times the row L1 norm (>= Euclidean row norm). For each axis:
// |x(p)/z(p)-x(q)/z(q)| <= deltaX/zMin + maxAbsX*deltaZ/zMin^2.
// Outward interval composition bounds every row/range. BOTH depths and the whole
// connecting segment exceed ClipNear. Projection scale*viewport/2 gives pixels.
// The sum of axis bounds also upper-bounds Euclidean projected-set Hausdorff
// discrepancy. This is ideal mathematical projection of the selected geometry,
// NOT visibility/rasterization/occlusion/attributes/shader FP or all-pass quality.
// Full viewport containment is deliberately required; clipped-image set distance
// does not in general inherit the unclipped projection bound.
inline Status Build(const Input& input,const Binding& expectedBinding,const Cut& expectedCut,Result& destination)
{
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    (void)input;(void)expectedBinding;(void)expectedCut;(void)destination;return Status::Unsupported;
#else
    if(!(input.binding==expectedBinding)||!(input.cut==expectedCut)||
       std::bit_cast<uint64_t>(input.surfaceDistanceUpper)!=input.binding.hausdorffUpperBits)return Status::IdentityMismatch;
    if(!Detail::BoundIdentity(input.binding)||!input.cut.cameraGeneration||!input.cut.modelBirth||!input.cut.certificateGeneration||
       !input.viewportWidth||input.viewportWidth>16384||!input.viewportHeight||input.viewportHeight>16384||
       input.viewportOriginX>16384||input.viewportOriginY>16384)return Status::Invalid;
    if constexpr(!std::numeric_limits<double>::is_iec559||!std::numeric_limits<float>::is_iec559||
       std::numeric_limits<double>::radix!=2||std::numeric_limits<float>::radix!=2||
       std::numeric_limits<double>::digits!=53||std::numeric_limits<float>::digits!=24)
        return Status::Unsupported;
    if(!Detail::Float(input.clipNear)||input.clipNear<=0||!Detail::NormalOrZero(input.surfaceDistanceUpper)||
       input.surfaceDistanceUpper<0||input.surfaceDistanceUpper>10000||!Detail::ModelAffine(input.model)||!Detail::Affine(input.view)||
       input.view[12]!=0||input.view[13]!=0||input.view[14]!=0||!Detail::Projection(input))return Status::Unsupported;
    for(unsigned i=0;i<3;++i)if(!Detail::Float(input.minimum[i])||!Detail::Float(input.maximum[i])||input.minimum[i]>input.maximum[i])return Status::Invalid;
    std::array<std::array<Detail::Interval,4>,3> rows{};std::array<Detail::Interval,3> ranges{};std::array<double,3> norms{};
    if(!Detail::Compose(input,rows))return Status::Unsupported;
    for(unsigned i=0;i<3;++i)if(!Detail::RangeAndSensitivity(rows[i],input,ranges[i],norms[i]))return Status::Unsupported;
    if(ranges[2].lo<=input.clipNear)return Status::NearIntersection;
    const auto extentX=std::max(std::abs(ranges[0].lo),std::abs(ranges[0].hi));
    const auto extentY=std::max(std::abs(ranges[1].lo),std::abs(ranges[1].hi));double x,y,scaled;
    if(!Detail::Mul(extentX,input.projection[0],true,scaled)||!Detail::Div(scaled,ranges[2].lo,true,x)||
       !Detail::Mul(extentY,input.projection[5],true,scaled)||!Detail::Div(scaled,ranges[2].lo,true,y))return Status::Unsupported;
    if(x>=1||y>=1)return Status::OutsideViewport;
    Result result;result.tuple=input;result.depthMinimum=ranges[2].lo;
    if(!Detail::Axis(extentX,norms[0],norms[2],ranges[2].lo,input.surfaceDistanceUpper,input.projection[0],input.viewportWidth,result.pixelXUpper)||
       !Detail::Axis(extentY,norms[1],norms[2],ranges[2].lo,input.surfaceDistanceUpper,input.projection[5],input.viewportHeight,result.pixelYUpper)||
       !Detail::Add(result.pixelXUpper,result.pixelYUpper,true,result.euclideanPixelUpper))return Status::Unsupported;
    destination=result;return Status::Bounded;
#endif
}
}
