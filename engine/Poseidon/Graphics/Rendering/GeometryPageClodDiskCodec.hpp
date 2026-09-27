#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageClodRamPackage.hpp>
#include <span>
#include <bit>
#include <string_view>

namespace Poseidon::GeometryPages
{
// Memory codec only, for already accepted immutable paired pilot cuts. This is NOT
// source-file freshness, retail eligibility, DAG-selection or GPU-residency proof.
// Explicit LE fields reproduce the pinned 68-byte SVertex layout; no byte buffer
// is copied into nontrivially-copyable Vector3P/SVertex objects.
// The 128KiB cap is encoded input/output length, not allocator/RSS or total scratch.
enum class ClodDiskStatus { Encoded, Decoded, Invalid, Unsupported, Capacity, AllocationFailed };
namespace ClodDiskDetail
{
inline constexpr size_t MaxBytes=128*1024, HeaderBytes=112, DigestAt=80;
inline constexpr uint32_t Magic=0x31444347, Schema=1;
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(SVertex)==68 && offsetof(SVertex,pos)==0 && offsetof(SVertex,norm)==12 &&
    offsetof(SVertex,t0)==24 && offsetof(SVertex,conform)==32 && offsetof(SVertex,tangent)==36 &&
    offsetof(SVertex,binormal)==48 && offsetof(SVertex,t1)==60);
static_assert(std::string_view(ClodBake::LibraryRevision).size()==40);
static_assert(ClodBake::AdapterVersion==1 && ClodRamPackage::AdapterVersion==1 && Package::FormatVersion==1);
inline uint64_t LayoutKey()
{
    uint64_t h=1469598103934665603ull;
    for(size_t n:{sizeof(SVertex),alignof(SVertex),offsetof(SVertex,pos),offsetof(SVertex,norm),
        offsetof(SVertex,t0),offsetof(SVertex,conform),offsetof(SVertex,tangent),offsetof(SVertex,binormal),offsetof(SVertex,t1)})
        {h^=uint64_t(n);h*=1099511628211ull;}
    return h;
}
inline std::array<uint8_t,32> HashBytes(Foundation::Sha256& hash)
{
    const auto hex=hash.Hex();std::array<uint8_t,32> out{};
    const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
    for(size_t i=0;i<32;++i) out[i]=uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1]));
    return out;
}
inline std::array<uint8_t,32> Checksum(std::span<const uint8_t> data)
{
    Foundation::Sha256 hash;
    hash.Update(data.data(),DigestAt);
    hash.Update(data.data()+HeaderBytes,data.size()-HeaderBytes);
    return HashBytes(hash);
}
struct Capacity {};
struct Writer
{
    std::vector<uint8_t> data;
    void Raw(const void* p,size_t n) {
        if(n>MaxBytes-data.size()) throw Capacity{};
        if(n) {const auto* bytes=static_cast<const uint8_t*>(p);data.insert(data.end(),bytes,bytes+n);}
    }
    void U32(uint32_t n) {uint8_t b[4];for(unsigned i=0;i<4;++i)b[i]=uint8_t(n>>(i*8));Raw(b,4);}
    void U64(uint64_t n) {uint8_t b[8];for(unsigned i=0;i<8;++i)b[i]=uint8_t(n>>(i*8));Raw(b,8);}
    void Float(float n) {U32(std::bit_cast<uint32_t>(n));}
    void Source(const SourceIdentity& v) {
        Raw(v.sourceSha256.data(),32);U64(v.geometryOptions);U64(v.materialOptions);
        U32(v.producerVersion);U32(v.coarseRepresentation);U32(v.fineRepresentation);U32(v.vertexLayout);U32(v.materialMapping);
    }
};
struct Reader
{
    std::span<const uint8_t> data;size_t at=0;bool valid=true;
    size_t Left() const {return at<=data.size()?data.size()-at:0;}
    bool Raw(void* p,size_t n) {
        if(!valid || n>Left()){valid=false;return false;}
        if(n)std::memcpy(p,data.data()+at,n);at+=n;return true;
    }
    uint32_t U32(){uint8_t b[4]{};Raw(b,4);return uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24;}
    uint64_t U64(){uint8_t b[8]{};Raw(b,8);uint64_t n=0;for(unsigned i=0;i<8;++i)n|=uint64_t(b[i])<<(8*i);return n;}
    float Float(){return std::bit_cast<float>(U32());}
    SourceIdentity Source() {
        SourceIdentity v;Raw(v.sourceSha256.data(),32);v.geometryOptions=U64();v.materialOptions=U64();
        v.producerVersion=U32();v.coarseRepresentation=U32();v.fineRepresentation=U32();v.vertexLayout=U32();v.materialMapping=U32();return v;
    }
};
inline void WriteVertex(Writer& w,const SVertex& v)
{
    for(const auto* vector:{&v.pos,&v.norm}) {w.Float(vector->X());w.Float(vector->Y());w.Float(vector->Z());}
    w.Float(v.t0.u);w.Float(v.t0.v);w.U32(v.conform);
    for(const auto* vector:{&v.tangent,&v.binormal}) {w.Float(vector->X());w.Float(vector->Y());w.Float(vector->Z());}
    w.Float(v.t1.u);w.Float(v.t1.v);
}
inline SVertex ReadVertex(Reader& r)
{
    // Each read is sequenced explicitly; constructor-argument evaluation order
    // must not reorder the wire fields.
    const auto vector=[&]() {
        const float x=r.Float();const float y=r.Float();const float z=r.Float();
        return Vector3P(x,y,z);
    };
    SVertex v;v.pos=vector();v.norm=vector();
    const float u0=r.Float();const float v0=r.Float();v.t0={u0,v0};v.conform=r.U32();
    v.tangent=vector();v.binormal=vector();
    const float u1=r.Float();const float v1=r.Float();v.t1={u1,v1};return v;
}
inline bool Ids(const std::vector<uint32_t>& ids)
{
    if(ids.empty() || ids.size()>64)return false;
    for(size_t i=0;i<ids.size();++i)if(ids[i]>=16384 || (i && ids[i]<=ids[i-1]))return false;
    return true;
}
inline bool Mesh(const ExportedMesh& m)
{
    if(m.vertices.empty() || m.vertices.size()>1024 || m.positions.size()!=m.vertices.size() ||
       m.indices.empty() || m.indices.size()>4096 || m.indices.size()%3 || m.materials.size()!=m.indices.size()/3)return false;
    const auto finite3=[](const auto& v){return std::isfinite(v.X()) && std::isfinite(v.Y()) && std::isfinite(v.Z());};
    for(size_t i=0;i<m.vertices.size();++i) {
        const auto& v=m.vertices[i];const auto& p=m.positions[i];
        if(v.conform || !finite3(v.pos) || !finite3(v.norm) || !finite3(v.tangent) || !finite3(v.binormal) ||
           !std::isfinite(v.t0.u) || !std::isfinite(v.t0.v) || !std::isfinite(v.t1.u) || !std::isfinite(v.t1.v) ||
           std::bit_cast<uint32_t>(p.x)!=std::bit_cast<uint32_t>(v.pos.X()) ||
           std::bit_cast<uint32_t>(p.y)!=std::bit_cast<uint32_t>(v.pos.Y()) ||
           std::bit_cast<uint32_t>(p.z)!=std::bit_cast<uint32_t>(v.pos.Z()))return false;
    }
    return Detail::Valid(m.Input(),Limits{});
}
// Exact derivation contract of ClodRamPackage adapter1. No DAG certificate is
// reconstructed: the expected derived identity must come from a trusted bake/index.
inline SourceIdentity Derived(const ClodRamPackage& v)
{
    Foundation::Sha256 hash;hash.Update(std::string("OpenPoseidon-selected-clod-ram-v1"));
    hash.Update(std::string(ClodBake::LibraryRevision));
    auto number=[&](uint64_t n,unsigned width){uint8_t b[8]{};for(unsigned i=0;i<width;++i)b[i]=uint8_t(n>>(8*i));hash.Update(b,width);};
    const auto& s=v.originalSource;hash.Update(s.sourceSha256.data(),32);
    number(s.geometryOptions,8);number(s.materialOptions,8);number(s.producerVersion,4);
    number(s.coarseRepresentation,4);number(s.fineRepresentation,4);number(s.vertexLayout,4);number(s.materialMapping,4);
    number(ClodBake::AdapterVersion,4);number(ClodRamPackage::AdapterVersion,4);
    number(std::bit_cast<uint32_t>(v.coarseThreshold),4);number(std::bit_cast<uint32_t>(v.fineThreshold),4);
    for(const auto* ids:{&v.coarseClusters,&v.fineClusters}){number(ids->size(),4);for(auto id:*ids)number(id,4);}
    for(const auto* m:{&v.selectedGeometry.coarse,&v.selectedGeometry.fine}) {
        number(m->vertices.size(),4);hash.Update(m->vertices.data(),m->vertices.size()*sizeof(SVertex));
        number(m->indices.size(),4);for(auto i:m->indices)number(i,4);for(auto material:m->materials)number(material,4);
    }
    auto derived=s;derived.coarseRepresentation=0;derived.fineRepresentation=1;derived.vertexLayout=sizeof(SVertex);
    derived.sourceSha256=HashBytes(hash);return derived;
}
inline bool Validate(const ClodRamPackage& v)
{
    bool source=false;for(auto b:v.originalSource.sourceSha256)source|=b!=0;
    if(!source || !v.originalSource.producerVersion || !v.originalSource.vertexLayout || !v.originalSource.materialMapping ||
       v.originalSource.coarseRepresentation==v.originalSource.fineRepresentation ||
       !std::isfinite(v.coarseThreshold) || !std::isfinite(v.fineThreshold) || v.coarseThreshold==FLT_MAX ||
       v.coarseThreshold<=v.fineThreshold || v.fineThreshold<0 || !Ids(v.coarseClusters) || !Ids(v.fineClusters) ||
       v.coarseClusters.size()+v.fineClusters.size()>64 || v.coarseClusters==v.fineClusters ||
       !Mesh(v.selectedGeometry.coarse) || !Mesh(v.selectedGeometry.fine) ||
       v.selectedGeometry.coarse.indices.size()>=v.selectedGeometry.fine.indices.size() ||
       v.package.pages.empty() || v.package.pages.size()>8 || v.package.clusters.empty() || v.package.clusters.size()>64 ||
       !(v.selectedGeometry.source==Derived(v)) || !(v.package.source==v.selectedGeometry.source))return false;
    const auto material=v.selectedGeometry.coarse.materials.front();
    for(const auto* m:{&v.selectedGeometry.coarse,&v.selectedGeometry.fine})for(auto id:m->materials)if(id!=material)return false;
    uint64_t payload=0;for(const auto& page:v.package.pages){if(page.bytes.size()>MaxBytes-payload)return false;payload+=page.bytes.size();}
    if(payload!=v.package.payloadBytes)return false;
    RepresentationDecodeLimits limits;limits.pages=8;limits.clusters=64;limits.vertexRecords=1024;limits.indices=4096;
    limits.decodedBytes=MaxBytes;limits.serializedBytes=MaxBytes;
    for(auto frontier:{Frontier::Coarse,Frontier::Fine}) {
        ResidentRepresentation decoded;
        if(DecodeResidentRepresentation(v.package,v.package.Identity(),frontier,v.selectedGeometry,decoded,limits)!=DecodeStatus::Decoded)return false;
    }
    return true;
}
inline void WriteMesh(Writer& w,const ExportedMesh& m)
{
    w.U32(uint32_t(m.vertices.size()));w.U32(uint32_t(m.indices.size()));w.U32(uint32_t(m.materials.size()));
    for(const auto& vertex:m.vertices)WriteVertex(w,vertex);
    for(auto i:m.indices)w.U32(i);for(auto material:m.materials)w.U32(material);
}
inline bool ReadMesh(Reader& r,ExportedMesh& m)
{
    const uint32_t vertices=r.U32(),indices=r.U32(),materials=r.U32();
    const uint64_t bytes=uint64_t(vertices)*sizeof(SVertex)+uint64_t(indices)*4+uint64_t(materials)*4;
    if(!r.valid || !vertices || vertices>1024 || !indices || indices>4096 || indices%3 || materials!=indices/3 || bytes>r.Left())return false;
    m.vertices.resize(vertices);m.positions.reserve(vertices);m.indices.resize(indices);m.materials.resize(materials);
    for(auto& vertex:m.vertices)vertex=ReadVertex(r);
    for(const auto& v:m.vertices)m.positions.push_back({v.pos.X(),v.pos.Y(),v.pos.Z()});
    for(auto& i:m.indices)i=r.U32();for(auto& material:m.materials)material=r.U32();return r.valid;
}
}

inline ClodDiskStatus EncodeClodDisk(const ClodRamPackage& value,std::vector<uint8_t>& destination)
{
    using namespace ClodDiskDetail;
    if constexpr(std::endian::native!=std::endian::little)return ClodDiskStatus::Unsupported;
    try {
        if(!Validate(value))return ClodDiskStatus::Invalid;
        Writer w;w.U32(Magic);w.U32(Schema);w.U32(0);w.U32(sizeof(SVertex));w.U64(LayoutKey());
        w.U32(Package::FormatVersion);w.U32(2);w.U32(ClodBake::AdapterVersion);w.U32(ClodRamPackage::AdapterVersion);
        w.Raw(ClodBake::LibraryRevision,40);const std::array<uint8_t,32> zero{};w.Raw(zero.data(),32);
        if(w.data.size()!=HeaderBytes)return ClodDiskStatus::Invalid;
        w.Source(value.originalSource);w.Source(value.selectedGeometry.source);
        w.U32(value.package.packing.clusterVertices);w.U32(value.package.packing.clusterTriangles);w.U32(value.package.packing.pageBytes);
        w.Float(value.coarseThreshold);w.Float(value.fineThreshold);
        for(const auto* ids:{&value.coarseClusters,&value.fineClusters}){w.U32(uint32_t(ids->size()));for(auto id:*ids)w.U32(id);}
        WriteMesh(w,value.selectedGeometry.coarse);WriteMesh(w,value.selectedGeometry.fine);
        w.U32(uint32_t(value.package.pages.size()));w.U32(uint32_t(value.package.clusters.size()));
        w.U32(value.package.coarsePages);w.U32(value.package.coarseClusters);w.U64(value.package.payloadBytes);
        for(const auto& c:value.package.clusters) {
            w.U32(c.page);w.U32(c.byteOffset);w.U32(c.byteLength);w.U32(c.material);w.U32(c.firstTriangle);w.U32(c.triangles);
            for(float n:c.minimum)w.Float(n);for(float n:c.maximum)w.Float(n);
        }
        for(const auto& page:value.package.pages){w.U32(uint32_t(page.bytes.size()));w.Raw(page.bytes.data(),page.bytes.size());}
        const auto total=uint32_t(w.data.size());for(unsigned i=0;i<4;++i)w.data[8+i]=uint8_t(total>>(i*8));
        const auto hash=Checksum(w.data);std::memcpy(w.data.data()+DigestAt,hash.data(),32);
        destination=std::move(w.data);return ClodDiskStatus::Encoded;
    }catch(const Capacity&){return ClodDiskStatus::Capacity;}catch(const std::bad_alloc&){return ClodDiskStatus::AllocationFailed;}
}

// Caller keys are mandatory: a checksummed file cannot authenticate its own raw
// source or simplified cut. No filesystem, renderer, configuration or Shape access.
inline ClodDiskStatus DecodeClodDisk(std::span<const uint8_t> encoded,
    const SourceIdentity& expectedOriginal,const CacheIdentity& expectedCut,ClodRamPackage& destination)
{
    using namespace ClodDiskDetail;
    if constexpr(std::endian::native!=std::endian::little)return ClodDiskStatus::Unsupported;
    if(encoded.size()>MaxBytes)return ClodDiskStatus::Capacity;
    if(encoded.size()<HeaderBytes)return ClodDiskStatus::Invalid;
    try {
        Reader r{encoded};const auto magic=r.U32(),schema=r.U32(),total=r.U32(),stride=r.U32();const auto layout=r.U64();
        const auto format=r.U32(),algorithm=r.U32(),bake=r.U32(),ram=r.U32();
        char library[40]{};r.Raw(library,40);std::array<uint8_t,32> hash{};r.Raw(hash.data(),32);
        if(magic!=Magic || total!=encoded.size())return ClodDiskStatus::Invalid;
        if(schema!=Schema || stride!=sizeof(SVertex) || layout!=LayoutKey() || format!=1 || algorithm!=2 ||
           bake!=ClodBake::AdapterVersion || ram!=ClodRamPackage::AdapterVersion ||
           std::memcmp(library,ClodBake::LibraryRevision,40))return ClodDiskStatus::Unsupported;
        if(hash!=Checksum(encoded))return ClodDiskStatus::Invalid;
        ClodRamPackage result;result.originalSource=r.Source();result.selectedGeometry.source=r.Source();
        result.package.source=result.selectedGeometry.source;
        result.package.packing={r.U32(),r.U32(),r.U32()};
        if(!r.valid || !(result.originalSource==expectedOriginal) || !(result.package.Identity()==expectedCut))return ClodDiskStatus::Invalid;
        result.coarseThreshold=r.Float();result.fineThreshold=r.Float();
        for(auto* ids:{&result.coarseClusters,&result.fineClusters}) {
            const auto count=r.U32();if(!r.valid || !count || count>64 || uint64_t(count)*4>r.Left())return ClodDiskStatus::Invalid;
            ids->resize(count);for(auto& id:*ids)id=r.U32();
        }
        if(result.coarseClusters.size()+result.fineClusters.size()>64 ||
           !Ids(result.coarseClusters) || !Ids(result.fineClusters) || result.coarseClusters==result.fineClusters ||
           !std::isfinite(result.coarseThreshold) || !std::isfinite(result.fineThreshold) ||
           result.coarseThreshold==FLT_MAX || result.coarseThreshold<=result.fineThreshold || result.fineThreshold<0)
            return ClodDiskStatus::Invalid;
        if(!ReadMesh(r,result.selectedGeometry.coarse) || !ReadMesh(r,result.selectedGeometry.fine))return ClodDiskStatus::Invalid;
        const auto pages=r.U32(),clusters=r.U32();result.package.coarsePages=r.U32();result.package.coarseClusters=r.U32();
        result.package.payloadBytes=r.U64();
        if(!r.valid || !pages || pages>8 || !clusters || clusters>64 || result.package.payloadBytes>MaxBytes ||
           uint64_t(clusters)*48+uint64_t(pages)*4>r.Left())return ClodDiskStatus::Invalid;
        result.package.clusters.resize(clusters);result.package.pages.resize(pages);
        for(auto& c:result.package.clusters) {
            c.page=r.U32();c.byteOffset=r.U32();c.byteLength=r.U32();c.material=r.U32();c.firstTriangle=r.U32();c.triangles=r.U32();
            for(auto& n:c.minimum)n=r.Float();for(auto& n:c.maximum)n=r.Float();
        }
        uint64_t payload=0;
        for(auto& page:result.package.pages) {
            const auto size=r.U32();if(!r.valid || size<16 || size>MaxBytes-payload || size>r.Left())return ClodDiskStatus::Invalid;
            payload+=size;page.bytes.resize(size);r.Raw(page.bytes.data(),size);
        }
        if(!r.valid || r.Left() || payload!=result.package.payloadBytes || !Validate(result))return ClodDiskStatus::Invalid;
        result.knownCapacityBytes=ClodRamKnownBytes(result);
        if(result.knownCapacityBytes>MaxBytes)return ClodDiskStatus::Capacity;
        destination=std::move(result);return ClodDiskStatus::Decoded;
    }catch(const std::bad_alloc&){return ClodDiskStatus::AllocationFailed;}
}
}
