#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalCut.hpp>

namespace Poseidon::GeometryPages
{
struct HierarchicalDiskRange
{
    uint64_t offset=0;
    uint32_t bytes=0;
    std::array<uint8_t,32> sha256{};
};
struct HierarchicalDiskImage
{
    HierarchicalIdentity identity;
    std::array<uint8_t,32> metadataSha256{};
    std::vector<uint8_t> bytes;
};
struct HierarchicalDiskManifest
{
    // Only immutable metadata and page-local cluster IDs are retained. Payload
    // vertex/index vectors stay empty until separately requested page decoding.
    // This shape can feed the existing complete-cut/root-fallback reference.
    HierarchicalPackage metadata;
    std::vector<HierarchicalDiskRange> pages;
    uint64_t fileBytes=0,metadataBytes=0,knownCapacityBytes=0;
    std::array<uint8_t,32> metadataSha256{};
};
enum class HierarchicalDiskStatus { Encoded,Decoded,Invalid,Capacity,AllocationFailed };
namespace HierarchicalDiskDetail
{
inline constexpr uint32_t Schema=1,HeaderBytes=212,VertexScalarBytes=68;
inline constexpr uint32_t GroupBytes=32,ClusterBytes=36,PageRowBytes=72,PageHeaderBytes=24;
inline constexpr uint64_t MaxMetadataBytes=32768,MaxFileBytes=MaxMetadataBytes+64ull*65536;
static_assert(sizeof(SVertex)>=VertexScalarBytes);
inline void Number(std::vector<uint8_t>& bytes,uint64_t n,unsigned width=4)
{for(unsigned i=0;i<width;++i)bytes.push_back(uint8_t(n>>(8*i)));}
inline void Float(std::vector<uint8_t>& bytes,float value)
{Number(bytes,std::bit_cast<uint32_t>(value));}
inline void Bounds(std::vector<uint8_t>& bytes,const clodBounds& b)
{for(auto v:b.center)Float(bytes,v);Float(bytes,b.radius);Float(bytes,b.error);}
inline std::array<uint8_t,32> Hash(std::span<const uint8_t> bytes)
{Foundation::Sha256 hash;hash.Update(bytes.data(),bytes.size());return HierarchicalDetail::Digest(hash);}
class Reader
{
    std::span<const uint8_t> bytes_;size_t at_=0;bool valid_=true;
public:
    explicit Reader(std::span<const uint8_t> bytes):bytes_(bytes){}
    uint64_t Number(unsigned width=4) {
        if(!valid_||width>8||width>bytes_.size()-at_){valid_=false;return 0;}
        uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(bytes_[at_++])<<(8*i);return n;
    }
    float Float(){return std::bit_cast<float>(uint32_t(Number()));}
    clodBounds Bounds(){clodBounds b{};for(auto& v:b.center)v=Float();b.radius=Float();b.error=Float();return b;}
    template<size_t N> std::array<uint8_t,N> Array(){std::array<uint8_t,N> a{};for(auto& v:a)v=uint8_t(Number(1));return a;}
    bool Valid() const{return valid_;}
    bool Done() const{return valid_&&at_==bytes_.size();}
    size_t Position() const{return at_;}
};
inline void Identity(std::vector<uint8_t>& bytes,const HierarchicalIdentity& key)
{
    const auto& s=key.source;bytes.insert(bytes.end(),s.sourceSha256.begin(),s.sourceSha256.end());
    Number(bytes,s.geometryOptions,8);Number(bytes,s.materialOptions,8);Number(bytes,s.producerVersion);
    Number(bytes,s.coarseRepresentation);Number(bytes,s.fineRepresentation);Number(bytes,s.vertexLayout);Number(bytes,s.materialMapping);
    bytes.insert(bytes.end(),key.packageSha256.begin(),key.packageSha256.end());Number(bytes,key.adapterVersion);
}
inline HierarchicalIdentity Identity(Reader& r)
{
    HierarchicalIdentity key;auto& s=key.source;s.sourceSha256=r.Array<32>();
    s.geometryOptions=r.Number(8);s.materialOptions=r.Number(8);s.producerVersion=uint32_t(r.Number());
    s.coarseRepresentation=uint32_t(r.Number());s.fineRepresentation=uint32_t(r.Number());
    s.vertexLayout=uint32_t(r.Number());s.materialMapping=uint32_t(r.Number());
    key.packageSha256=r.Array<32>();key.adapterVersion=uint32_t(r.Number());return key;
}
inline bool Scalar(float v){return std::isfinite(v)&&std::abs(v)<=1000000.f;}
inline bool VertexValid(const SVertex& v)
{
    if(v.conform)return false;
    for(const auto* p:{&v.pos,&v.norm,&v.tangent,&v.binormal})
        if(!Scalar(p->X())||!Scalar(p->Y())||!Scalar(p->Z()))return false;
    return Scalar(v.t0.u)&&Scalar(v.t0.v)&&Scalar(v.t1.u)&&Scalar(v.t1.v);
}
inline void Vertex(std::vector<uint8_t>& bytes,const SVertex& v)
{
    for(const auto* p:{&v.pos,&v.norm}){Float(bytes,p->X());Float(bytes,p->Y());Float(bytes,p->Z());}
    Float(bytes,v.t0.u);Float(bytes,v.t0.v);Number(bytes,v.conform);
    for(const auto* p:{&v.tangent,&v.binormal}){Float(bytes,p->X());Float(bytes,p->Y());Float(bytes,p->Z());}
    Float(bytes,v.t1.u);Float(bytes,v.t1.v);
}
inline SVertex Vertex(Reader& r)
{
    SVertex v{};
    const auto vector=[&](){const float x=r.Float(),y=r.Float(),z=r.Float();return Vector3P(x,y,z);};
    v.pos=vector();v.norm=vector();v.t0.u=r.Float();v.t0.v=r.Float();v.conform=uint32_t(r.Number());
    v.tangent=vector();v.binormal=vector();v.t1.u=r.Float();v.t1.v=r.Float();return v;
}
inline bool PayloadValid(const HierarchicalPayload& c)
{
    if(c.vertices.empty()||c.vertices.size()>64||c.originalVertexIds.size()!=c.vertices.size()||
        c.indices.empty()||c.indices.size()%3||c.indices.size()>128*3)return false;
    std::array<uint8_t,64> used{};
    for(size_t v=0;v<c.vertices.size();++v) {
        if(c.originalVertexIds[v]>=4096||!VertexValid(c.vertices[v]))return false;
        for(size_t previous=0;previous<v;++previous)if(c.originalVertexIds[previous]==c.originalVertexIds[v])return false;
    }
    for(auto i:c.indices){if(i>=c.vertices.size())return false;used[i]=1;}
    return std::all_of(used.begin(),used.begin()+c.vertices.size(),[](auto bit){return bit==1;});
}
inline uint64_t ManifestKnownBytes(const HierarchicalDiskManifest& manifest)
{return sizeof(manifest)+HierarchicalKnownBytes(manifest.metadata)-sizeof(HierarchicalPackage)+
    manifest.pages.capacity()*sizeof(HierarchicalDiskRange);}
}
inline uint64_t HierarchicalDiskPageKnownBytes(const HierarchicalPage& page)
{
    uint64_t bytes=sizeof(page)+page.clusters.capacity()*sizeof(HierarchicalPayload);
    for(const auto& c:page.clusters)bytes+=c.vertices.capacity()*sizeof(SVertex)+
        (c.originalVertexIds.capacity()+c.indices.capacity())*sizeof(uint32_t);
    return bytes;
}
// Filesystem-neutral producer for immutable BuildHierarchicalPackage outputs.
// Complete graph/root metadata is one <=32KiB authenticated prefix. Page ranges
// follow contiguously and independently; pages have their own SHA256. Caller must
// retain/trust BOTH returned identity and metadata digest before accepting reads.
// Output limits are not a claim of allocator/RSS or bake-scratch peak bounds.
inline HierarchicalDiskStatus EncodeHierarchicalDisk(const HierarchicalPackage& package,HierarchicalDiskImage& destination)
{
    using namespace HierarchicalDiskDetail;
    if(!ValidHierarchicalMetadata(package))return HierarchicalDiskStatus::Invalid;
    try {
        const uint64_t metadataBytes=HeaderBytes+package.groups.size()*GroupBytes+package.clusters.size()*ClusterBytes+
            package.pages.size()*PageRowBytes+(package.rootClusters.size()+package.rootPages.size())*4;
        if(metadataBytes>MaxMetadataBytes)return HierarchicalDiskStatus::Capacity;
        std::vector<std::vector<uint8_t>> payloads;payloads.reserve(package.pages.size());
        std::vector<HierarchicalDiskRange> ranges;ranges.reserve(package.pages.size());
        uint64_t fileBytes=metadataBytes;
        for(uint32_t p=0;p<package.pages.size();++p) {
            const auto& page=package.pages[p];std::vector<uint8_t> bytes;bytes.reserve(size_t(page.logicalBytes));
            Number(bytes,0x31475048);Number(bytes,Schema);Number(bytes,p);Number(bytes,page.group);
            Number(bytes,page.clusters.size());Number(bytes,0);
            uint64_t logical=16,upload=0;
            for(const auto& c:page.clusters) {
                if(!PayloadValid(c))return HierarchicalDiskStatus::Invalid;
                logical+=48+c.vertices.size()*(sizeof(SVertex)+4)+c.indices.size()*4;
                upload+=c.vertices.size()*sizeof(SVertex)+c.indices.size()*4;
                Number(bytes,c.cluster);Number(bytes,c.material);Number(bytes,c.vertices.size());Number(bytes,c.indices.size());
                for(auto id:c.originalVertexIds)Number(bytes,id);
                for(const auto& v:c.vertices)Vertex(bytes,v);
                for(auto i:c.indices)Number(bytes,i);
            }
            if(logical!=page.logicalBytes||upload!=page.uploadBytes)return HierarchicalDiskStatus::Invalid;
            if(bytes.size()>package.pageByteLimit||bytes.size()>65536||fileBytes>MaxFileBytes-bytes.size())return HierarchicalDiskStatus::Capacity;
            Detail::Patch(bytes,20,uint32_t(bytes.size()));
            ranges.push_back({fileBytes,uint32_t(bytes.size()),Hash(bytes)});fileBytes+=bytes.size();payloads.push_back(std::move(bytes));
        }
        HierarchicalDiskImage result;result.identity=package.identity;auto& bytes=result.bytes;bytes.reserve(size_t(fileBytes));
        constexpr std::array<uint8_t,8> magic{'O','P','H','G','P','G','0','1'};bytes.insert(bytes.end(),magic.begin(),magic.end());
        Number(bytes,Schema);Number(bytes,HeaderBytes);Number(bytes,sizeof(SVertex));Number(bytes,VertexScalarBytes);Identity(bytes,package.identity);
        bytes.insert(bytes.end(),ClodBake::LibraryRevision,ClodBake::LibraryRevision+40);Number(bytes,ClodBake::AdapterVersion);
        Number(bytes,package.pageByteLimit);Number(bytes,package.groups.size());Number(bytes,package.clusters.size());Number(bytes,package.pages.size());
        Number(bytes,package.rootClusters.size());Number(bytes,package.rootPages.size());Number(bytes,fileBytes,8);Number(bytes,metadataBytes,8);
        if(bytes.size()!=HeaderBytes)return HierarchicalDiskStatus::Invalid;
        for(const auto& g:package.groups){Number(bytes,uint32_t(g.depth));Bounds(bytes,g.simplified);Number(bytes,g.first);Number(bytes,g.count);}
        for(const auto& c:package.clusters){Number(bytes,c.group);Number(bytes,c.page);Number(bytes,c.pageSlot);Number(bytes,uint32_t(c.refined));Bounds(bytes,c.bounds);}
        for(uint32_t p=0;p<package.pages.size();++p) {
            const auto& page=package.pages[p];const auto& range=ranges[p];
            Number(bytes,page.group);Number(bytes,page.clusters.size());Number(bytes,page.logicalBytes,8);Number(bytes,page.uploadBytes,8);
            Number(bytes,range.offset,8);Number(bytes,range.bytes);bytes.insert(bytes.end(),range.sha256.begin(),range.sha256.end());
            Number(bytes,page.clusters.front().cluster);
        }
        for(auto id:package.rootClusters)Number(bytes,id);for(auto p:package.rootPages)Number(bytes,p);
        if(bytes.size()!=metadataBytes)return HierarchicalDiskStatus::Invalid;
        result.metadataSha256=Hash(bytes);for(const auto& page:payloads)bytes.insert(bytes.end(),page.begin(),page.end());
        if(bytes.size()!=fileBytes)return HierarchicalDiskStatus::Invalid;
        destination=std::move(result);return HierarchicalDiskStatus::Encoded;
    }catch(const std::bad_alloc&){return HierarchicalDiskStatus::AllocationFailed;}
}
// Read ONLY the exact metadata prefix (no payload needed). totalFileBytes is the
// current file-size witness supplied by an independent owned reader. Its digest
// must come from the trusted producer/manifest, not this untrusted read's header.
// Refuse holes, aliasing, overlapping ranges, trailing file data and all overflow.
inline HierarchicalDiskStatus DecodeHierarchicalManifest(std::span<const uint8_t> bytes,uint64_t totalFileBytes,
    const HierarchicalIdentity& expected,const std::array<uint8_t,32>& expectedMetadataSha256,HierarchicalDiskManifest& destination)
{
    using namespace HierarchicalDiskDetail;
    if(bytes.size()<HeaderBytes||bytes.size()>MaxMetadataBytes||totalFileBytes>MaxFileBytes||totalFileBytes<bytes.size())
        return HierarchicalDiskStatus::Capacity;
    if(!HierarchicalDetail::Nonzero(expectedMetadataSha256))return HierarchicalDiskStatus::Invalid;
    try {
        if(Hash(bytes)!=expectedMetadataSha256)return HierarchicalDiskStatus::Invalid;
        Reader r(bytes);constexpr std::array<uint8_t,8> magic{'O','P','H','G','P','G','0','1'};
        if(r.Array<8>()!=magic||r.Number()!=Schema||r.Number()!=HeaderBytes||r.Number()!=sizeof(SVertex)||r.Number()!=VertexScalarBytes)
            return HierarchicalDiskStatus::Invalid;
        HierarchicalDiskManifest result;auto& package=result.metadata;package.identity=Identity(r);
        if(!(package.identity==expected))return HierarchicalDiskStatus::Invalid;
        const auto library=r.Array<40>();
        if(std::memcmp(library.data(),ClodBake::LibraryRevision,40)||r.Number()!=ClodBake::AdapterVersion)return HierarchicalDiskStatus::Invalid;
        package.pageByteLimit=uint32_t(r.Number());
        const uint32_t groups=uint32_t(r.Number()),clusters=uint32_t(r.Number()),pages=uint32_t(r.Number()),
            roots=uint32_t(r.Number()),rootPages=uint32_t(r.Number());
        result.fileBytes=r.Number(8);result.metadataBytes=r.Number(8);
        if(!r.Valid()||!groups||groups>64||!clusters||clusters>256||!pages||pages>64||!roots||roots>clusters||
            !rootPages||rootPages>pages||package.pageByteLimit<1024||package.pageByteLimit>65536)return HierarchicalDiskStatus::Invalid;
        const uint64_t exact=HeaderBytes+uint64_t(groups)*GroupBytes+uint64_t(clusters)*ClusterBytes+
            uint64_t(pages)*PageRowBytes+uint64_t(roots+rootPages)*4;
        if(exact!=bytes.size()||result.metadataBytes!=exact||result.fileBytes!=totalFileBytes)return HierarchicalDiskStatus::Invalid;
        package.groups.resize(groups);package.clusters.resize(clusters);package.pages.resize(pages);result.pages.resize(pages);
        for(auto& g:package.groups){g.depth=int32_t(r.Number());g.simplified=r.Bounds();g.first=uint32_t(r.Number());g.count=uint32_t(r.Number());}
        for(auto& c:package.clusters){c.group=uint32_t(r.Number());c.page=uint32_t(r.Number());c.pageSlot=uint32_t(r.Number());c.refined=int32_t(r.Number());c.bounds=r.Bounds();}
        uint64_t next=exact;uint32_t nextCluster=0;
        for(uint32_t p=0;p<pages;++p) {
            auto& page=package.pages[p];auto& range=result.pages[p];page.group=uint32_t(r.Number());const uint32_t count=uint32_t(r.Number());
            page.logicalBytes=r.Number(8);page.uploadBytes=r.Number(8);range.offset=r.Number(8);range.bytes=uint32_t(r.Number());range.sha256=r.Array<32>();
            const uint32_t first=uint32_t(r.Number());
            if(!r.Valid()||page.group>=groups||!count||count>clusters-nextCluster||first!=nextCluster||
                range.offset!=next||range.bytes<PageHeaderBytes||range.bytes>package.pageByteLimit||
                next>totalFileBytes||range.bytes>totalFileBytes-next||!HierarchicalDetail::Nonzero(range.sha256)||
                page.logicalBytes<16||page.logicalBytes>package.pageByteLimit||!page.uploadBytes||page.uploadBytes>page.logicalBytes)
                return HierarchicalDiskStatus::Invalid;
            page.clusters.resize(count);for(auto& c:page.clusters)c.cluster=nextCluster++;
            next+=range.bytes;
        }
        if(next!=totalFileBytes||nextCluster!=clusters)return HierarchicalDiskStatus::Invalid;
        package.rootClusters.resize(roots);package.rootPages.resize(rootPages);
        for(auto& id:package.rootClusters)id=uint32_t(r.Number());for(auto& id:package.rootPages)id=uint32_t(r.Number());
        if(!r.Done()||!ValidHierarchicalMetadata(package))return HierarchicalDiskStatus::Invalid;
        package.knownCapacityBytes=HierarchicalKnownBytes(package);result.knownCapacityBytes=ManifestKnownBytes(result);
        if(result.knownCapacityBytes>128*1024)return HierarchicalDiskStatus::Capacity;
        result.metadataSha256=expectedMetadataSha256;destination=std::move(result);return HierarchicalDiskStatus::Decoded;
    }catch(const std::bad_alloc&){return HierarchicalDiskStatus::AllocationFailed;}
}
// Independently decode one exact page range. The caller keeps a successfully
// decoded manifest immutable, verifies source/mount/generation separately and
// supplies only THIS range's bytes. A different page, prefix/suffix, corruption
// or malformed attributes/index mappings cannot replace the previous result.
inline HierarchicalDiskStatus DecodeHierarchicalDiskPage(const HierarchicalDiskManifest& manifest,uint32_t pageId,
    std::span<const uint8_t> bytes,HierarchicalPage& destination)
{
    using namespace HierarchicalDiskDetail;
    if(!ValidHierarchicalMetadata(manifest.metadata)||manifest.pages.size()!=manifest.metadata.pages.size()||
        pageId>=manifest.pages.size()||!HierarchicalDetail::Nonzero(manifest.metadataSha256))return HierarchicalDiskStatus::Invalid;
    const auto& range=manifest.pages[pageId];const auto& expected=manifest.metadata.pages[pageId];
    if(bytes.size()!=range.bytes||bytes.size()<PageHeaderBytes||bytes.size()>65536)return HierarchicalDiskStatus::Invalid;
    try {
        if(Hash(bytes)!=range.sha256)return HierarchicalDiskStatus::Invalid;
        Reader r(bytes);
        if(r.Number()!=0x31475048||r.Number()!=Schema||r.Number()!=pageId||r.Number()!=expected.group||
            r.Number()!=expected.clusters.size()||r.Number()!=bytes.size())return HierarchicalDiskStatus::Invalid;
        HierarchicalPage result;result.group=expected.group;result.clusters.reserve(expected.clusters.size());
        for(size_t slot=0;slot<expected.clusters.size();++slot) {
            HierarchicalPayload c;c.cluster=uint32_t(r.Number());c.material=uint32_t(r.Number());
            const uint32_t vertices=uint32_t(r.Number()),indices=uint32_t(r.Number());
            if(!r.Valid()||c.cluster!=expected.clusters[slot].cluster||!vertices||vertices>64||!indices||indices%3||indices>128*3||
                uint64_t(vertices)*(4+VertexScalarBytes)+uint64_t(indices)*4>bytes.size()-r.Position())return HierarchicalDiskStatus::Invalid;
            c.originalVertexIds.resize(vertices);c.vertices.resize(vertices);c.indices.resize(indices);
            for(auto& id:c.originalVertexIds)id=uint32_t(r.Number());for(auto& v:c.vertices)v=Vertex(r);for(auto& i:c.indices)i=uint32_t(r.Number());
            if(!r.Valid()||!PayloadValid(c))return HierarchicalDiskStatus::Invalid;
            result.logicalBytes+=48+uint64_t(vertices)*(sizeof(SVertex)+4)+uint64_t(indices)*4;
            result.uploadBytes+=uint64_t(vertices)*sizeof(SVertex)+uint64_t(indices)*4;result.clusters.push_back(std::move(c));
        }
        if(!r.Done()||result.logicalBytes!=expected.logicalBytes||result.uploadBytes!=expected.uploadBytes)return HierarchicalDiskStatus::Invalid;
        if(HierarchicalDiskPageKnownBytes(result)>128*1024)return HierarchicalDiskStatus::Capacity;
        destination=std::move(result);return HierarchicalDiskStatus::Decoded;
    }catch(const std::bad_alloc&){return HierarchicalDiskStatus::AllocationFailed;}
}
}
