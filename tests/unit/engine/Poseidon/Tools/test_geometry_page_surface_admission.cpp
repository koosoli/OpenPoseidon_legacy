#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageSurfaceAdmission.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageDiskSource.hpp>
#include "../../../../../apps/tools/Tools/commands/ControlledMlodPageProducer.hpp"
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <atomic>
#include <chrono>
#include <sstream>
using namespace Poseidon::GeometryPages;
namespace SA=Poseidon::GeometryPages::SurfaceAdmission;
namespace MP=PoseidonTools::MlodPages;
namespace {
struct OriginalGrid {
    std::vector<uint8_t> bytes;
    void U32(uint32_t n){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(n>>(8*i)));}
    void F(float f){U32(std::bit_cast<uint32_t>(f));}
    void Text(const char* s){while(*s)bytes.push_back(uint8_t(*s++));}
    void Z(const char* s){Text(s);bytes.push_back(0);}
    void Fixed(const char* s){const auto at=bytes.size();Z(s);bytes.resize(at+64,0);}
    void Tag(const char* s,uint32_t n){bytes.push_back(1);Z(s);U32(n);}
    OriginalGrid(){Text("MLOD");U32(0x101);U32(2);
        for(unsigned level=0;level<2;++level){const unsigned n=level?16:1;
            Text("P3DM");U32(28);U32(256);U32((n+1)*(n+1));U32(1);U32(n*n);U32(0);
            for(unsigned z=0;z<=n;++z)for(unsigned x=0;x<=n;++x){const float px=float(x)/n*4-2,pz=float(z)/n*4-2;
                F(px);F(.9f*(1-px*px/4)*(1-pz*pz/4));F(pz);U32(0);}
            F(0);F(-1);F(0);
            for(unsigned z=0;z<n;++z)for(unsigned x=0;x<n;++x){U32(4);
                const std::array<std::array<unsigned,2>,4> corners{{{x,z},{x+1,z},{x+1,z+1},{x,z+1}}};
                for(auto corner:corners){U32(corner[1]*(n+1)+corner[0]);U32(0);F(float(corner[0])/n);F(float(corner[1])/n);}
                U32(0);Z("");Z("");}
            Text("TAGG");Tag("#Property#",128);Fixed("autocenter");Fixed("0");Tag("#EndOfFile#",0);F(level?1:10);
        }
    }
};

std::span<const uint8_t> Bytes(const std::string& s){return {reinterpret_cast<const uint8_t*>(s.data()),s.size()};}
struct Bundle {
    MP::Product product;std::string descriptor;SA::Arguments authority;uint64_t rawBytes=0;
    Bundle(){Poseidon::Foundation::CaptureMainThread();OriginalGrid original;rawBytes=original.bytes.size();
        REQUIRE(MP::BuildOwned(original.bytes,product,true)==MP::ProducerStatus::Produced);
        REQUIRE(MP::BuildSurfaceDescriptor(product,descriptor)==MP::ProducerStatus::Produced);RefreshDigests();}
    void RefreshDigests(){authority.manifestSha256=SA::Detail::Digest(Bytes(product.manifest));authority.certificateSha256=SA::Detail::Digest(Bytes(descriptor));}
    SA::Status VerifyCapturedCertificate(SA::Proof& out)const{return SA::VerifyCapturedCertificate(Bytes(product.manifest),Bytes(descriptor),product.encoded,
        authority,product.selected.originalSource,rawBytes,out);}
};
void Replace(std::string& s,std::string_view before,std::string_view after){const auto at=s.find(before);REQUIRE(at!=std::string::npos);s.replace(at,before.size(),after);}
struct Files {
    std::filesystem::path dir;static inline std::atomic<uint64_t> next{0};
    Files(){dir=std::filesystem::temp_directory_path()/("surface-admission-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++next));
        REQUIRE(std::filesystem::create_directory(dir));}
    ~Files(){std::error_code e;for(const char* name:{"manifest.json","surface-certificate.json","selected.gcd"})std::filesystem::remove(dir/name,e);std::filesystem::remove(dir,e);}
    void Write(const char* name,std::span<const uint8_t> bytes){std::ofstream f(dir/name,std::ios::binary|std::ios::trunc);REQUIRE(bool(f));
        f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));f.close();REQUIRE(bool(f));}
    void Save(Bundle& b){Write("manifest.json",Bytes(b.product.manifest));Write("surface-certificate.json",Bytes(b.descriptor));Write("selected.gcd",b.product.encoded);
        b.authority.manifestPath=dir/"manifest.json";b.authority.certificatePath=dir/"surface-certificate.json";}
};
}
TEST_CASE("Actual original producer certificate admits exact packed union and bit proof", "[geometry-page-surface-admission]") {
    Bundle b;SA::Proof p;REQUIRE(b.VerifyCapturedCertificate(p)==SA::Status::Verified);
    CHECK(p.certificate.originalSource==b.product.selected.originalSource);CHECK(p.certificate.selectedKey==b.product.selected.package.Identity());
    CHECK(p.encodedBytes==b.product.encoded.size());CHECK(p.encodedSha256==SA::Detail::Digest(b.product.encoded));
    CHECK(p.certificate.distanceVisits<=2'000'000);CHECK(p.certificate.coarseCutCount>0);CHECK(p.certificate.fineCutCount>0);
    CHECK(SA::MatchesDecoded(p,b.product.selected));CHECK(p.certificate.hausdorffUpper>0);
    CHECK(SA::Proof::KnownBytes()==sizeof(SA::Proof)+64);
    for(const auto* mesh:{&b.product.selected.selectedGeometry.coarse,&b.product.selected.selectedGeometry.fine})for(const auto& v:mesh->vertices){
        CHECK(v.pos.X()>=p.minimum[0]);CHECK(v.pos.X()<=p.maximum[0]);CHECK(v.pos.Y()>=p.minimum[1]);CHECK(v.pos.Y()<=p.maximum[1]);CHECK(v.pos.Z()>=p.minimum[2]);CHECK(v.pos.Z()<=p.maximum[2]);}
    auto changed=b.product.selected;changed.selectedGeometry.fine.vertices[0].norm[0]+=.5f;
    CHECK_FALSE(SA::MatchesDecoded(p,changed)); // All packed attributes, not position-only authority.
    SA::Proof unchanged=p;b.authority.manifestSha256[0]^=1;
    CHECK(b.VerifyCapturedCertificate(unchanged)==SA::Status::DigestMismatch);CHECK(unchanged.encodedSha256==p.encodedSha256);
}
TEST_CASE("Surface admission refuses self authority malformed JSON numeric mismatch and debt", "[geometry-page-surface-admission]") {
    Bundle b;SA::Proof p;REQUIRE(b.VerifyCapturedCertificate(p)==SA::Status::Verified);const auto original=b.descriptor;
    Replace(b.descriptor,"\"algorithmVersion\":1","\"algorithmVersion\":1,\"algorithmVersion\":1");b.RefreshDigests();
    CHECK(b.VerifyCapturedCertificate(p)==SA::Status::Invalid);CHECK(p.encodedBytes==b.product.encoded.size());b.descriptor=original;
    Replace(b.descriptor,"\"distanceVisitCap\":2000000","\"distanceVisitCap\":2000001");b.RefreshDigests();CHECK(b.VerifyCapturedCertificate(p)==SA::Status::Unsupported);b.descriptor=original;
    const auto hashAt=b.descriptor.find("\"coarsePackedSha256\":\"");REQUIRE(hashAt!=std::string::npos);
    const auto byte=hashAt+std::string_view("\"coarsePackedSha256\":\"").size();b.descriptor[byte]=b.descriptor[byte]=='0'?'1':'0';b.RefreshDigests();
    CHECK(b.VerifyCapturedCertificate(p)==SA::Status::CertificateMismatch);b.descriptor=original;
    const auto cutAt=b.descriptor.find("\"coarseCutIds\":[");REQUIRE(cutAt!=std::string::npos);
    const auto cutBegin=cutAt+std::string_view("\"coarseCutIds\":[").size();const auto cutEnd=b.descriptor.find(']',cutBegin);REQUIRE(cutEnd!=std::string::npos);
    b.descriptor.replace(cutBegin,cutEnd-cutBegin,"4294967295");b.RefreshDigests();CHECK(b.VerifyCapturedCertificate(p)==SA::Status::CertificateMismatch);b.descriptor=original;
    // A trusted digest is not mathematical authority: forged larger numeric
    // value + matching IEEE bits still needs the exact bounded recomputation.
    SA::Detail::Json parsed;REQUIRE(parsed.Parse(b.descriptor));const auto* number=parsed.Get(parsed.nodes[0],"hausdorffUpper");REQUIRE(number);
    std::ostringstream larger;larger.imbue(std::locale::classic());larger<<std::setprecision(std::numeric_limits<double>::max_digits10)<<p.certificate.hausdorffUpper*2;
    b.descriptor.replace(number->begin,number->end-number->begin,larger.str());
    Replace(b.descriptor,"\"hausdorffUpperBitsHex\":\""+MP::Hex64(std::bit_cast<uint64_t>(p.certificate.hausdorffUpper),true)+"\"",
        "\"hausdorffUpperBitsHex\":\""+MP::Hex64(std::bit_cast<uint64_t>(p.certificate.hausdorffUpper*2),true)+"\"");
    b.RefreshDigests();CHECK(b.VerifyCapturedCertificate(p)==SA::Status::CertificateMismatch);b.descriptor=original;
    b.descriptor+="{}";b.RefreshDigests();CHECK(b.VerifyCapturedCertificate(p)==SA::Status::Invalid);b.descriptor=original;b.RefreshDigests();
    const auto wrongOriginal=b.product.selected.originalSource;auto different=wrongOriginal;different.sourceSha256[0]^=1;
    CHECK(SA::VerifyCapturedCertificate(Bytes(b.product.manifest),Bytes(b.descriptor),b.product.encoded,b.authority,different,b.rawBytes,p)==SA::Status::IdentityMismatch);
    CHECK(SA::VerifyCapturedCertificate(Bytes(b.product.manifest),Bytes(b.descriptor),b.product.encoded,b.authority,wrongOriginal,b.rawBytes+1,p)==SA::Status::Unsupported);
    auto encoded=b.product.encoded;encoded.back()^=1;
    CHECK(SA::VerifyCapturedCertificate(Bytes(b.product.manifest),Bytes(b.descriptor),encoded,b.authority,wrongOriginal,b.rawBytes,p)==SA::Status::DigestMismatch);
    std::string huge(8193,' ');CHECK(SA::VerifyCapturedCertificate(Bytes(huge),Bytes(b.descriptor),encoded,b.authority,wrongOriginal,b.rawBytes,p)==SA::Status::Capacity);
    SA::Detail::Json parser;CHECK_FALSE(parser.Parse("{\"nested\":{\"x\":1,\"x\":2}}"));CHECK_FALSE(parser.Parse("{\"x\":01}"));CHECK_FALSE(parser.Parse("{\"x\":\"\\u0000\"}"));
    std::string deep="0";for(unsigned i=0;i<10;++i)deep="["+deep+"]";CHECK_FALSE(parser.Parse("{\"x\":"+deep+"}"));
    std::string many="{\"x\":[0";for(unsigned i=0;i<512;++i)many+=",0";many+="]}";CHECK_FALSE(parser.Parse(many));
}
TEST_CASE("Surface admission native snapshots and actual worker read bind the same encoded bytes", "[geometry-page-surface-admission]") {
    Bundle b;Files files;files.Save(b);auto p=std::make_shared<SA::Proof>();
    REQUIRE(SA::Prepare(b.authority,b.product.selected.originalSource,b.rawBytes,*p)==SA::Status::Verified);
    DiskPageInput input;input.path=files.dir/"selected.gcd";input.originalSource=b.product.selected.originalSource;input.selectedCut=b.product.selected.package.Identity();input.surfaceProof=p;
    std::atomic<bool> cancelled{false};ClodRamPackage decoded;
    REQUIRE(ReadDiskPageSource(input,decoded,cancelled)==DiskReadStatus::Read);CHECK(SA::MatchesDecoded(*p,decoded));
    const auto retained=decoded.package.Identity();cancelled=true;CHECK(ReadDiskPageSource(input,decoded,cancelled)==DiskReadStatus::Cancelled);CHECK(decoded.package.Identity()==retained);cancelled=false;
    // Correct old codec bytes still work unbound. A mismatched optional file witness
    // refuses, without altering the ordinary caller's decoder/eligibility policy.
    auto wrong=std::make_shared<SA::Proof>(*p);wrong->encodedSha256[0]^=1;input.surfaceProof=wrong;
    CHECK(ReadDiskPageSource(input,decoded,cancelled)==DiskReadStatus::Invalid);CHECK(decoded.package.Identity()==retained);
    input.surfaceProof.reset();CHECK(ReadDiskPageSource(input,decoded,cancelled)==DiskReadStatus::Read);
    files.Write("surface-certificate.json",Bytes(std::string("{}")));CHECK(SA::Prepare(b.authority,input.originalSource,b.rawBytes,*p)==SA::Status::DigestMismatch);
    CHECK(p->encodedBytes==b.product.encoded.size());files.Write("surface-certificate.json",Bytes(b.descriptor));
    std::filesystem::remove(files.dir/"surface-certificate.json");CHECK(SA::Prepare(b.authority,input.originalSource,b.rawBytes,*p)==SA::Status::IoFailure);
    CHECK(p->encodedBytes==b.product.encoded.size());files.Write("surface-certificate.json",Bytes(b.descriptor));
    auto growing=b.product.encoded;growing.push_back(0);files.Write("selected.gcd",growing);
    CHECK(SA::Prepare(b.authority,input.originalSource,b.rawBytes,*p)==SA::Status::Invalid);
}
TEST_CASE("Surface startup transport refuses partial unsafe and missing independent digests", "[geometry-page-surface-admission]") {
    SA::Arguments a;const std::string hash(64,'1');
    CHECK(SA::ParseArguments({"manifest.json",hash},{"surface-certificate.json",hash},a));
    CHECK_FALSE(SA::ParseArguments({"manifest.json"},{"surface-certificate.json",hash},a));
    CHECK_FALSE(SA::ParseArguments({std::string(1024,'x'),hash},{"surface-certificate.json",hash},a));
    CHECK_FALSE(SA::ParseArguments({std::string("bad\0path",8),hash},{"surface-certificate.json",hash},a));
    CHECK_FALSE(SA::ParseArguments({"manifest.json","self"},{"surface-certificate.json",hash},a));
}
