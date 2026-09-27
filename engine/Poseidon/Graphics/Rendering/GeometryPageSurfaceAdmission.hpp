#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageSurfaceArguments.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageSurfaceCertificate.hpp>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

namespace Poseidon::GeometryPages::SurfaceAdmission
{
enum class Status { Verified, Invalid, Unsupported, Capacity, IoFailure, DigestMismatch, IdentityMismatch, CertificateMismatch, AllocationFailed };
// Immutable after successful Prepare. Numeric proof only: no encoded/source payload,
// filename, Shape or renderer owner. Selected coarse/fine unions exclude the authored
// fallback. KnownBytes is logical object/control-block guard, not allocator or RSS.
struct Proof {
    SurfaceCertificate::Certificate certificate;
    std::array<uint8_t,32> encodedSha256{};
    uint64_t encodedBytes=0;
    std::array<float,3> minimum{},maximum{};
    static constexpr uint64_t KnownBytes(){return sizeof(Proof)+64;}
};
namespace Detail {
inline std::array<uint8_t,32> Digest(std::span<const uint8_t> bytes) {
    Foundation::Sha256 hash;hash.Update(bytes.data(),bytes.size());return ClodDiskDetail::HashBytes(hash);
}
inline bool Hex(std::string_view s,std::span<uint8_t> output) {
    if(s.size()!=output.size()*2)return false;
    auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    for(size_t i=0;i<output.size();++i){const int a=digit(s[i*2]),b=digit(s[i*2+1]);if(a<0||b<0)return false;output[i]=uint8_t(a*16+b);}return true;
}
// Restricted actual producer JSON. Fixed 512 tokens, depth <=8 and <=8192 bytes;
// ASCII strings with no escapes (all current producer values are canonical ASCII).
// Unknown syntax, duplicate object keys at ANY depth, trailing data and token debt
// refuse. No third-party parser heap or permissive number/string coercion.
class Json {
public:
    enum Type { Object,Array,String,Number };
    struct Node { Type type=Object;uint16_t begin=0,end=0,keyBegin=0,keyEnd=0,parent=UINT16_MAX; };
    std::array<Node,512> nodes{};size_t count=0;std::string_view text;
    bool Parse(std::string_view s){text=s;size_t p=0;if(s.empty()||s.size()>8192)return false;
        if(!Value(p,UINT16_MAX,0,0,0))return false;Space(p);return p==s.size()&&nodes[0].type==Object;}
    std::string_view Slice(const Node& n)const{return text.substr(n.begin,n.end-n.begin);}
    const Node* Get(const Node& object,std::string_view key)const {
        if(object.type!=Object)return nullptr;const auto parent=uint16_t(&object-nodes.data());
        for(size_t i=0;i<count;++i)if(nodes[i].parent==parent&&text.substr(nodes[i].keyBegin,nodes[i].keyEnd-nodes[i].keyBegin)==key)return &nodes[i];
        return nullptr;
    }
    bool U64(const Node* n,uint64_t& value)const {if(!n||n->type!=Number)return false;auto s=Slice(*n);
        const auto result=std::from_chars(s.data(),s.data()+s.size(),value);return result.ec==std::errc{}&&result.ptr==s.data()+s.size();}
    bool U32(const Node* n,uint32_t& value)const {uint64_t v;if(!U64(n,v)||v>UINT32_MAX)return false;value=uint32_t(v);return true;}
    bool StringIs(const Node* n,std::string_view s)const{return n&&n->type==String&&Slice(*n)==s;}
    bool Hash(const Node* n,std::array<uint8_t,32>& h)const{return n&&n->type==String&&Hex(Slice(*n),h);}
    bool Bits(const Node* n,uint64_t& v)const {if(!n||n->type!=String||Slice(*n).size()!=16)return false;
        const auto s=Slice(*n);auto r=std::from_chars(s.data(),s.data()+s.size(),v,16);return r.ec==std::errc{}&&r.ptr==s.data()+s.size();}
    bool DoubleBits(const Node* decimal,const Node* hex,double& value)const {uint64_t bits=0;
        if(!decimal||decimal->type!=Number||!Bits(hex,bits))return false;const auto s=Slice(*decimal);
        auto r=std::from_chars(s.data(),s.data()+s.size(),value,std::chars_format::general);
        return r.ec==std::errc{}&&r.ptr==s.data()+s.size()&&std::isfinite(value)&&value>=0&&std::bit_cast<uint64_t>(value)==bits;}
    bool Cut(const Node* n,std::array<uint32_t,64>& ids,uint32_t& size)const {
        if(!n||n->type!=Array)return false;size=0;const auto parent=uint16_t(n-nodes.data());
        for(size_t i=0;i<count;++i)if(nodes[i].parent==parent){uint32_t v;if(size==64||!U32(&nodes[i],v)||
            (size&&v<=ids[size-1]))return false;ids[size++]=v;}return size!=0;
    }
private:
    void Space(size_t& p)const {while(p<text.size()&&(text[p]==' '||text[p]=='\n'||text[p]=='\r'||text[p]=='\t'))++p;}
    bool StringToken(size_t& p,uint16_t& begin,uint16_t& end)const {
        if(p==text.size()||text[p++]!='"')return false;begin=uint16_t(p);
        while(p<text.size()&&text[p]!='"'){const unsigned char c=text[p];if(c<32||c>126||c=='\\')return false;++p;}
        if(p==text.size())return false;end=uint16_t(p++);return true;
    }
    bool Value(size_t& p,uint16_t parent,uint16_t keyBegin,uint16_t keyEnd,unsigned depth) {
        Space(p);if(p==text.size()||count==nodes.size()||depth>8)return false;
        const auto index=uint16_t(count++);nodes[index].parent=parent;nodes[index].keyBegin=keyBegin;nodes[index].keyEnd=keyEnd;
        if(text[p]=='{'||text[p]=='['){const char start=text[p++],end=start=='{'?'}':']';nodes[index].type=start=='{'?Object:Array;
            Space(p);if(p<text.size()&&text[p]==end){++p;return true;}
            for(;;){uint16_t b=0,e=0;if(start=='{'){Space(p);if(!StringToken(p,b,e))return false;
                for(size_t i=index+1;i<count;++i)if(nodes[i].parent==index&&text.substr(nodes[i].keyBegin,nodes[i].keyEnd-nodes[i].keyBegin)==text.substr(b,e-b))return false;
                Space(p);if(p==text.size()||text[p++]!=':')return false;}
                if(!Value(p,index,b,e,depth+1))return false;Space(p);if(p==text.size())return false;
                if(text[p]==end){++p;return true;}if(text[p++]!=',')return false;}
        }
        if(text[p]=='"'){nodes[index].type=String;return StringToken(p,nodes[index].begin,nodes[index].end);}
        nodes[index].type=Number;nodes[index].begin=uint16_t(p);
        if(text[p]=='-')++p;if(p==text.size()||text[p]<'0'||text[p]>'9')return false;
        if(text[p]=='0')++p;else while(p<text.size()&&text[p]>='0'&&text[p]<='9')++p;
        if(p<text.size()&&text[p]=='.'){++p;const auto at=p;while(p<text.size()&&text[p]>='0'&&text[p]<='9')++p;if(at==p)return false;}
        if(p<text.size()&&(text[p]=='e'||text[p]=='E')){++p;if(p<text.size()&&(text[p]=='+'||text[p]=='-'))++p;
            const auto at=p;while(p<text.size()&&text[p]>='0'&&text[p]<='9')++p;if(at==p)return false;}
        nodes[index].end=uint16_t(p);return true;
    }
};
inline bool Source(const Json& j,const Json::Node* n,SourceIdentity& key) {
    if(!n||n->type!=Json::Object)return false;
    return j.Hash(j.Get(*n,"sourceSha256"),key.sourceSha256)&&j.Bits(j.Get(*n,"geometryOptionsHex"),key.geometryOptions)&&
        j.Bits(j.Get(*n,"materialOptionsHex"),key.materialOptions)&&j.U32(j.Get(*n,"producerVersion"),key.producerVersion)&&
        j.U32(j.Get(*n,"coarseRepresentation"),key.coarseRepresentation)&&j.U32(j.Get(*n,"fineRepresentation"),key.fineRepresentation)&&
        j.U32(j.Get(*n,"vertexLayout"),key.vertexLayout)&&j.U32(j.Get(*n,"materialMapping"),key.materialMapping);
}
inline bool Key(const Json& j,const Json::Node* n,CacheIdentity& key) {
    if(!n||n->type!=Json::Object)return false;const auto* p=j.Get(*n,"packing");if(!p)return false;
    return Source(j,j.Get(*n,"source"),key.source)&&j.U32(j.Get(*n,"formatVersion"),key.formatVersion)&&
        j.U32(j.Get(*n,"algorithmVersion"),key.algorithmVersion)&&j.U32(j.Get(*p,"clusterVertices"),key.packing.clusterVertices)&&
        j.U32(j.Get(*p,"clusterTriangles"),key.packing.clusterTriangles)&&j.U32(j.Get(*p,"pageBytes"),key.packing.pageBytes);
}
inline bool Exact(const Json& j,const Json::Node& root,const char* name,uint64_t expected) {
    uint64_t value=0;return j.U64(j.Get(root,name),value)&&value==expected;
}
inline Status Read(const std::filesystem::path& path,size_t cap,std::vector<uint8_t>& out) {
    try {if(path.empty()||path.native().size()>1024)return Status::Invalid;
        std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return Status::IoFailure;
        const auto size=f.tellg();if(size<=0)return Status::Invalid;if(uint64_t(std::streamoff(size))>cap)return Status::Capacity;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));f.seekg(0);if(!f.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))return Status::IoFailure;
        if(f.peek()!=std::char_traits<char>::eof()||!f.eof()||f.bad())return Status::IoFailure;
        out=std::move(bytes);return Status::Verified;
    }catch(const std::bad_alloc&){return Status::AllocationFailed;}catch(...){return Status::IoFailure;}
}
inline bool SameCertificate(const SurfaceCertificate::Certificate& a,const SurfaceCertificate::Certificate& b) {
    return a.originalSource==b.originalSource&&a.selectedKey==b.selectedKey&&a.coarsePackedSha256==b.coarsePackedSha256&&
        a.finePackedSha256==b.finePackedSha256&&a.coarseCut==b.coarseCut&&a.fineCut==b.fineCut&&
        a.coarseCutCount==b.coarseCutCount&&a.fineCutCount==b.fineCutCount&&a.coarseThresholdBits==b.coarseThresholdBits&&
        a.fineThresholdBits==b.fineThresholdBits&&a.distanceVisits==b.distanceVisits&&
        std::bit_cast<uint64_t>(a.fineToCoarse)==std::bit_cast<uint64_t>(b.fineToCoarse)&&
        std::bit_cast<uint64_t>(a.coarseToFine)==std::bit_cast<uint64_t>(b.coarseToFine)&&
        std::bit_cast<uint64_t>(a.hausdorffUpper)==std::bit_cast<uint64_t>(b.hausdorffUpper);
}
inline bool Envelope(const ClodRamPackage& value,std::array<float,3>& low,std::array<float,3>& high) {
    low.fill(std::numeric_limits<float>::infinity());high.fill(-std::numeric_limits<float>::infinity());
    for(const auto* mesh:{&value.selectedGeometry.coarse,&value.selectedGeometry.fine}) {
        if(mesh->vertices.empty()||mesh->vertices.size()>1024)return false;
        for(const auto& v:mesh->vertices){const std::array<float,3> p{v.pos.X(),v.pos.Y(),v.pos.Z()};
            for(unsigned k=0;k<3;++k){if(!SurfaceCertificate::Detail::Coordinate(p[k]))return false;low[k]=std::min(low[k],p[k]);high[k]=std::max(high[k],p[k]);}}
    }return true; // All actual packed vertices, including unused records: conservative.
}
}
// Production-used core accepts the exact captured buffers. Digests are supplied by
// an independent caller, not read from editable JSON. One bounded recomputation:
// <=2M anchor visits, no bake/texture/file/VFS work here. Output is transactional.
inline Status VerifyCapturedCertificate(std::span<const uint8_t> manifest,std::span<const uint8_t> descriptor,std::span<const uint8_t> encoded,
    const Arguments& expected,const SourceIdentity& original,uint64_t originalBytes,Proof& destination) {
    if(manifest.empty()||descriptor.empty()||manifest.size()>8192||descriptor.size()>8192||encoded.size()>ClodDiskDetail::MaxBytes)return Status::Capacity;
    if(Detail::Digest(manifest)!=expected.manifestSha256||Detail::Digest(descriptor)!=expected.certificateSha256)return Status::DigestMismatch;
    try {Detail::Json m,d;
        if(!m.Parse({reinterpret_cast<const char*>(manifest.data()),manifest.size()})||!d.Parse({reinterpret_cast<const char*>(descriptor.data()),descriptor.size()}))return Status::Invalid;
        const auto& mr=m.nodes[0];const auto& dr=d.nodes[0];Proof result;SourceIdentity manifestOriginal,descriptorOriginal;CacheIdentity manifestKey,descriptorKey;
        uint64_t manifestBytes=0,descriptorBytes=0;std::array<uint8_t,32> manifestFile{},descriptorFile{};
        if(!Detail::Exact(m,mr,"schemaVersion",2)||!m.StringIs(m.Get(mr,"producer"),"controlled-original-mlod")||
            !Detail::Exact(m,mr,"originalSubsetVersion",1)||!Detail::Exact(m,mr,"sourceBytes",originalBytes)||!Detail::Exact(m,mr,"diskCodecSchema",ClodDiskDetail::Schema)||
            !Detail::Exact(m,mr,"sVertexBytes",sizeof(SVertex))||!Detail::Exact(m,mr,"clodAdapterVersion",ClodBake::AdapterVersion)||
            !Detail::Exact(m,mr,"ramAdapterVersion",ClodRamPackage::AdapterVersion)||
            !m.StringIs(m.Get(mr,"clodLibraryRevision"),ClodBake::LibraryRevision)||
            !m.StringIs(m.Get(mr,"file"),"selected.gcd")||!d.StringIs(d.Get(dr,"file"),"selected.gcd"))return Status::Unsupported;
        uint64_t layout=0;if(!m.Bits(m.Get(mr,"sVertexLayoutKeyHex"),layout)||layout!=ClodDiskDetail::LayoutKey())return Status::Unsupported;
        if(!Detail::Source(m,m.Get(mr,"originalSource"),manifestOriginal)||!Detail::Source(d,d.Get(dr,"originalSource"),descriptorOriginal)||
            !Detail::Key(m,m.Get(mr,"selectedCut"),manifestKey)||!Detail::Key(d,d.Get(dr,"selectedCut"),descriptorKey))return Status::Invalid;
        if(!(manifestOriginal==original)||!(descriptorOriginal==original)||!(manifestKey==descriptorKey))return Status::IdentityMismatch;
        if(!m.U64(m.Get(mr,"fileBytes"),manifestBytes)||!d.U64(d.Get(dr,"fileBytes"),descriptorBytes)||manifestBytes!=encoded.size()||descriptorBytes!=manifestBytes||
            !m.Hash(m.Get(mr,"fileSha256"),manifestFile)||!d.Hash(d.Get(dr,"fileSha256"),descriptorFile))return Status::Invalid;
        result.encodedSha256=Detail::Digest(encoded);result.encodedBytes=encoded.size();
        if(manifestFile!=result.encodedSha256||descriptorFile!=manifestFile)return Status::DigestMismatch;
        auto& c=result.certificate;c.originalSource=original;c.selectedKey=manifestKey;
        if(!Detail::Exact(d,dr,"algorithmVersion",SurfaceCertificate::Certificate::AlgorithmVersion)||!Detail::Exact(d,dr,"distanceVisitCap",2'000'000)||
            !d.StringIs(d.Get(dr,"scope"),"object-space-selected-packed-triangle-unions-only; conservative-symmetric-surface-distance-upper-bound; not-original-source-fidelity/pixels/attributes/UV/normals/topology/all-passes-or-runtime-selection"))return Status::Unsupported;
        if(!d.Hash(d.Get(dr,"coarsePackedSha256"),c.coarsePackedSha256)||!d.Hash(d.Get(dr,"finePackedSha256"),c.finePackedSha256)||
            !d.Cut(d.Get(dr,"coarseCutIds"),c.coarseCut,c.coarseCutCount)||!d.Cut(d.Get(dr,"fineCutIds"),c.fineCut,c.fineCutCount)||
            !d.U32(d.Get(dr,"coarseThresholdBits"),c.coarseThresholdBits)||!d.U32(d.Get(dr,"fineThresholdBits"),c.fineThresholdBits)||
            !d.U64(d.Get(dr,"distanceVisits"),c.distanceVisits)||!c.distanceVisits||c.distanceVisits>2'000'000||
            !d.DoubleBits(d.Get(dr,"fineToCoarseUpper"),d.Get(dr,"fineToCoarseUpperBitsHex"),c.fineToCoarse)||
            !d.DoubleBits(d.Get(dr,"coarseToFineUpper"),d.Get(dr,"coarseToFineUpperBitsHex"),c.coarseToFine)||
            !d.DoubleBits(d.Get(dr,"hausdorffUpper"),d.Get(dr,"hausdorffUpperBitsHex"),c.hausdorffUpper))return Status::Invalid;
        uint32_t coarseBits=0,fineBits=0;
        if(!m.U32(m.Get(mr,"coarseThresholdBits"),coarseBits)||!m.U32(m.Get(mr,"fineThresholdBits"),fineBits)||coarseBits!=c.coarseThresholdBits||fineBits!=c.fineThresholdBits)return Status::IdentityMismatch;
        ClodRamPackage decoded;if(DecodeClodDisk(encoded,original,manifestKey,decoded)!=ClodDiskStatus::Decoded)return Status::Invalid;
        SurfaceCertificate::Certificate recomputed;const auto status=SurfaceCertificate::Build(decoded,original,manifestKey,recomputed);
        if(status!=SurfaceCertificate::Status::Certified)return status==SurfaceCertificate::Status::Capacity?Status::Capacity:
            status==SurfaceCertificate::Status::AllocationFailed?Status::AllocationFailed:Status::Unsupported;
        // Exact bit agreement, stricter than accepting an arbitrary larger bound.
        if(!Detail::SameCertificate(c,recomputed))return Status::CertificateMismatch;
        if(!Detail::Envelope(decoded,result.minimum,result.maximum))return Status::Unsupported;
        destination=std::move(result);return Status::Verified;
    }catch(const std::bad_alloc&){return Status::AllocationFailed;}catch(...){return Status::Invalid;}
}
inline Status Prepare(const Arguments& arguments,const SourceIdentity& original,uint64_t originalBytes,Proof& destination) {
    std::vector<uint8_t> manifest,descriptor,encoded;auto s=Detail::Read(arguments.manifestPath,8192,manifest);if(s!=Status::Verified)return s;
    if(Detail::Digest(manifest)!=arguments.manifestSha256)return Status::DigestMismatch;
    s=Detail::Read(arguments.certificatePath,8192,descriptor);if(s!=Status::Verified)return s;
    if(Detail::Digest(descriptor)!=arguments.certificateSha256)return Status::DigestMismatch;
    // The trusted actual manifest fixes the filename. Path supplies a snapshot,
    // not a native lease/path freshness theorem. Same captured encoded bytes hash/decode.
    s=Detail::Read(arguments.manifestPath.parent_path()/"selected.gcd",ClodDiskDetail::MaxBytes,encoded);if(s!=Status::Verified)return s;
    return VerifyCapturedCertificate(manifest,descriptor,encoded,arguments,original,originalBytes,destination);
}
// Worker post-decode binding only: no 2M recomputation. The admitted numeric
// certificate plus exact encoded digest binds all bytes; these actual decoded
// checks keep the publication contract explicit. No claim for authored fallback.
inline bool MatchesDecoded(const Proof& p,const ClodRamPackage& value) {
    const auto& c=p.certificate;
    if(!(value.originalSource==c.originalSource)||!(value.package.Identity()==c.selectedKey)||
        value.coarseClusters.size()!=c.coarseCutCount||value.fineClusters.size()!=c.fineCutCount||
        !std::equal(value.coarseClusters.begin(),value.coarseClusters.end(),c.coarseCut.begin())||
        !std::equal(value.fineClusters.begin(),value.fineClusters.end(),c.fineCut.begin())||
        std::bit_cast<uint32_t>(value.coarseThreshold)!=c.coarseThresholdBits||std::bit_cast<uint32_t>(value.fineThreshold)!=c.fineThresholdBits||
        SurfaceCertificate::Detail::PackedHash(value.selectedGeometry.coarse)!=c.coarsePackedSha256||
        SurfaceCertificate::Detail::PackedHash(value.selectedGeometry.fine)!=c.finePackedSha256)return false;
    std::array<float,3> low{},high{};return Detail::Envelope(value,low,high)&&low==p.minimum&&high==p.maximum;
}
}
