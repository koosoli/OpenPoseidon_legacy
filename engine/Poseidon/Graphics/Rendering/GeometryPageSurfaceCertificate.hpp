#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageClodDiskCodec.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Poseidon::GeometryPages::SurfaceCertificate
{
using namespace Poseidon::GeometryPages;
// Bounded geometric-set witness for immutable successful selected packages.
// Not original-asset fidelity, attribute/topology/pixel error, or GPU/pass proof.
struct Limits { uint64_t distanceVisits=2'000'000; };
enum class Status { Certified, Invalid, Unsupported, Capacity, AllocationFailed };
struct Certificate
{
    static constexpr uint32_t AlgorithmVersion=1;
    SourceIdentity originalSource;
    CacheIdentity selectedKey;
    std::array<uint8_t,32> coarsePackedSha256{},finePackedSha256{};
    std::array<uint32_t,64> coarseCut{},fineCut{};
    uint32_t coarseCutCount=0,fineCutCount=0,coarseThresholdBits=0,fineThresholdBits=0;
    uint64_t distanceVisits=0;
    // Object-space Euclidean surface-distance upper bounds, both directions.
    // Target anchors belong to referenced triangles, never unused vertex records.
    double fineToCoarse=0,coarseToFine=0,hausdorffUpper=0;
};
namespace Detail
{
// IEEE basic operations are individually materialized; no fused/reassociated sum.
// A nextafter step encloses any correctly-rounded IEEE rounding-mode result.
// Float-normal/zero coordinates in this domain keep every nonzero operation
// normal in binary64; exact zero cases avoid artificial subnormal intervals.
inline double Add(double a,double b){volatile double v=a+b;return v;}
inline double Sub(double a,double b){volatile double v=a-b;return v;}
inline double Mul(double a,double b){volatile double v=a*b;return v;}
inline double Up(double n){return std::nextafter(n,std::numeric_limits<double>::infinity());}
inline double Down(double n){return std::nextafter(n,-std::numeric_limits<double>::infinity());}
inline bool Coordinate(float f)
{
    const uint32_t bits=std::bit_cast<uint32_t>(f),magnitude=bits&0x7fffffffu;
    // Refuse subnormal source inputs (possible DAZ conversion), infinities/NaNs.
    if((magnitude && (magnitude&0x7f800000u)==0) || magnitude>=0x7f800000u)return false;
    return std::abs(double(f))<=10000;
}
struct Interval {double lo=0,hi=0;};
inline Interval Difference(double a,double b)
{if(a==b)return {};const auto d=Sub(a,b);return {Down(d),Up(d)};}
inline Interval Product(Interval a,Interval b)
{
    if((a.lo==0 && a.hi==0)||(b.lo==0 && b.hi==0))return {};
    const std::array<double,4> products{Mul(a.lo,b.lo),Mul(a.lo,b.hi),Mul(a.hi,b.lo),Mul(a.hi,b.hi)};
    const double lo=*std::min_element(products.begin(),products.end()),hi=*std::max_element(products.begin(),products.end());
    return {Down(lo),Up(hi)};
}
inline Interval Difference(Interval a,Interval b)
{return {a.lo==b.hi?0:Down(Sub(a.lo,b.hi)),a.hi==b.lo?0:Up(Sub(a.hi,b.lo))};}
inline bool Nondegenerate(const Poseidon::SVertex& a,const Poseidon::SVertex& b,const Poseidon::SVertex& c)
{
    const std::array<Interval,3> u{Difference(b.pos.X(),a.pos.X()),Difference(b.pos.Y(),a.pos.Y()),Difference(b.pos.Z(),a.pos.Z())};
    const std::array<Interval,3> v{Difference(c.pos.X(),a.pos.X()),Difference(c.pos.Y(),a.pos.Y()),Difference(c.pos.Z(),a.pos.Z())};
    for(size_t axis=0;axis<3;++axis){const size_t j=(axis+1)%3,k=(axis+2)%3;
        const auto cross=Difference(Product(u[j],v[k]),Product(u[k],v[j]));
        if(cross.lo>0 || cross.hi<0)return true;
    }
    return false; // Degenerate or too numerically uncertain to certify here.
}
inline bool DistanceUpper(const Poseidon::SVertex& a,const Poseidon::SVertex& b,double& out)
{
    const std::array<double,3> left{a.pos.X(),a.pos.Y(),a.pos.Z()},right{b.pos.X(),b.pos.Y(),b.pos.Z()};
    double sum=0;
    for(size_t i=0;i<3;++i){
        if(left[i]==right[i])continue;
        const double d=Up(std::abs(Sub(left[i],right[i])));
        sum=Up(Add(sum,Up(Mul(d,d))));
    }
    if(sum==0){out=0;return true;}
    if(!std::isfinite(sum)||sum<0)return false;
    volatile double root=std::sqrt(sum);double radius=root;
    if(!std::isfinite(radius)||radius<=0)return false;
    // Do not assume libm's sqrt accuracy: verify a LOWER bound of its square
    // dominates the accumulated UPPER squared distance. Refuse if no bounded repair.
    for(unsigned repair=0;repair<8;++repair){
        if(Down(Mul(radius,radius))>=sum){out=radius;return true;}
        radius=Up(radius);
    }
    return false;
}
inline bool Mesh(const ExportedMesh& mesh,std::array<uint8_t,1024>& referenced)
{
    if(!ClodDiskDetail::Mesh(mesh))return false;
    for(const auto& v:mesh.vertices)if(!Coordinate(v.pos.X())||!Coordinate(v.pos.Y())||!Coordinate(v.pos.Z()))return false;
    for(size_t i=0;i<mesh.indices.size();i+=3){
        const auto a=mesh.indices[i],b=mesh.indices[i+1],c=mesh.indices[i+2];
        if(a>=mesh.vertices.size()||b>=mesh.vertices.size()||c>=mesh.vertices.size())return false;
        if(!Nondegenerate(mesh.vertices[a],mesh.vertices[b],mesh.vertices[c]))return false;
        referenced[a]=referenced[b]=referenced[c]=1;
    }
    return true;
}
inline std::array<uint8_t,32> PackedHash(const ExportedMesh& mesh)
{
    Poseidon::Foundation::Sha256 hash;hash.Update(std::string("selected-surface-packed-v1"));
    auto number=[&](uint64_t v,unsigned width){uint8_t b[8]{};for(unsigned i=0;i<width;++i)b[i]=uint8_t(v>>(8*i));hash.Update(b,width);};
    number(sizeof(Poseidon::SVertex),4);number(mesh.vertices.size(),4);
    hash.Update(mesh.vertices.data(),mesh.vertices.size()*sizeof(Poseidon::SVertex));
    number(mesh.indices.size(),4);for(auto i:mesh.indices)number(i,4);
    number(mesh.materials.size(),4);for(auto m:mesh.materials)number(m,4);
    return ClodDiskDetail::HashBytes(hash);
}
inline Status Directed(const ExportedMesh& from,const ExportedMesh& target,const std::array<uint8_t,1024>& referenced,
    uint64_t cap,uint64_t& visits,double& bound)
{
    const auto charge=[&](){if(visits==cap)return false;++visits;return true;};
    for(size_t triangle=0;triangle<from.indices.size();triangle+=3){
        const auto& first=from.vertices[from.indices[triangle]];
        size_t anchor=SIZE_MAX;double closest=std::numeric_limits<double>::infinity();
        for(size_t i=0;i<target.vertices.size();++i){
            if(!charge())return Status::Capacity; // Count unused-record scans too.
            if(!referenced[i])continue;
            double distance=0;if(!DistanceUpper(first,target.vertices[i],distance))return Status::Unsupported;
            // Minimizing an UPPER estimate is only a heuristic; any surface anchor is valid.
            if(distance<closest){closest=distance;anchor=i;}
        }
        if(anchor==SIZE_MAX)return Status::Invalid;
        for(unsigned corner=0;corner<3;++corner){
            if(!charge())return Status::Capacity;
            double distance=0;if(!DistanceUpper(from.vertices[from.indices[triangle+corner]],target.vertices[anchor],distance))return Status::Unsupported;
            bound=std::max(bound,distance);
        }
    }
    return Status::Certified;
}
}
// Caller supplies authoritative expected keys; a modified package cannot authenticate
// its own source. The geometric certificate refers to actual packed selected triangles.
// No file read/bake, camera policy, or escaped payload owners.
inline Status Build(const ClodRamPackage& value,const SourceIdentity& expectedOriginal,
    const CacheIdentity& expectedSelected,Certificate& destination,Limits limits={})
{
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    (void)value;(void)expectedOriginal;(void)expectedSelected;(void)destination;(void)limits;
    return Status::Unsupported;
#else
    if constexpr(std::endian::native!=std::endian::little || !std::numeric_limits<float>::is_iec559 ||
        !std::numeric_limits<double>::is_iec559 || std::numeric_limits<float>::radix!=2 ||
        std::numeric_limits<double>::radix!=2 || std::numeric_limits<float>::digits!=24 || std::numeric_limits<double>::digits!=53)
        return Status::Unsupported;
    if(!limits.distanceVisits||limits.distanceVisits>2'000'000)return Status::Capacity;
    if(!(value.originalSource==expectedOriginal)||!(value.package.Identity()==expectedSelected))return Status::Invalid;
    try {
        if(!ClodDiskDetail::Validate(value))return Status::Invalid;
        std::array<uint8_t,1024> coarseReferenced{},fineReferenced{};
        if(!Detail::Mesh(value.selectedGeometry.coarse,coarseReferenced)||!Detail::Mesh(value.selectedGeometry.fine,fineReferenced))return Status::Unsupported;
        Certificate result;result.originalSource=expectedOriginal;result.selectedKey=expectedSelected;
        result.coarsePackedSha256=Detail::PackedHash(value.selectedGeometry.coarse);
        result.finePackedSha256=Detail::PackedHash(value.selectedGeometry.fine);
        result.coarseCutCount=uint32_t(value.coarseClusters.size());result.fineCutCount=uint32_t(value.fineClusters.size());
        std::copy(value.coarseClusters.begin(),value.coarseClusters.end(),result.coarseCut.begin());
        std::copy(value.fineClusters.begin(),value.fineClusters.end(),result.fineCut.begin());
        result.coarseThresholdBits=std::bit_cast<uint32_t>(value.coarseThreshold);result.fineThresholdBits=std::bit_cast<uint32_t>(value.fineThreshold);
        const auto forward=Detail::Directed(value.selectedGeometry.fine,value.selectedGeometry.coarse,coarseReferenced,
            limits.distanceVisits,result.distanceVisits,result.fineToCoarse);
        if(forward!=Status::Certified)return forward;
        const auto reverse=Detail::Directed(value.selectedGeometry.coarse,value.selectedGeometry.fine,fineReferenced,
            limits.distanceVisits,result.distanceVisits,result.coarseToFine);
        if(reverse!=Status::Certified)return reverse;
        result.hausdorffUpper=std::max(result.fineToCoarse,result.coarseToFine);
        destination=result;return Status::Certified;
    }catch(const std::bad_alloc&){return Status::AllocationFailed;}
#endif
}
}
