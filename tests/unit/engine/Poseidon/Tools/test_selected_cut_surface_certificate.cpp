#include <catch2/catch_test_macros.hpp>
#include "../../../../../apps/tools/Tools/commands/SelectedCutSurfaceCertificate.hpp"
#include "../../../../../apps/tools/Tools/commands/ControlledMlodPageProducer.hpp"
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <bit>
#include <limits>
#include <atomic>
#include <chrono>
#include <iterator>
#include <locale>
namespace SC=PoseidonTools::SurfaceCertificate;
using namespace Poseidon::GeometryPages;
namespace {
// Actual owned original MLOD input, matching the accepted untextured two-LOD subset.
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
PoseidonTools::MlodPages::Product ActualProduct(bool sealForSurfaceCertificate=false){
    Poseidon::Foundation::CaptureMainThread();OriginalGrid input;
    Poseidon::Foundation::Sha256 raw;raw.Update(input.bytes.data(),input.bytes.size());
    PoseidonTools::MlodPages::Product product;
    REQUIRE(PoseidonTools::MlodPages::BuildOwned(input.bytes,product,sealForSurfaceCertificate)==PoseidonTools::MlodPages::ProducerStatus::Produced);
    REQUIRE(PoseidonTools::MlodPages::Hex(product.selected.originalSource.sourceSha256)==raw.Hex());
    return product;
}
Poseidon::SVertex CertVertex(float x,float y,float z){Poseidon::SVertex v{};v.pos=::Vector3P(x,y,z);return v;}
void SamplesWithin(const ExportedMesh& from,const ExportedMesh& target,double bound){
    std::array<uint8_t,1024> referenced{};for(auto i:target.indices)referenced[i]=1;
    for(size_t tri=0;tri<from.indices.size();tri+=3){
        for(unsigned a=0;a<=4;++a)for(unsigned b=0;b<=4-a;++b){
            const long double weights[3]{a/4.L,b/4.L,(4-a-b)/4.L};long double p[3]{};
            for(unsigned corner=0;corner<3;++corner){const auto& v=from.vertices[from.indices[tri+corner]].pos;
                p[0]+=weights[corner]*v.X();p[1]+=weights[corner]*v.Y();p[2]+=weights[corner]*v.Z();}
            long double nearest=std::numeric_limits<long double>::infinity();
            for(size_t i=0;i<target.vertices.size();++i)if(referenced[i]){const auto& v=target.vertices[i].pos;
                const auto dx=p[0]-v.X(),dy=p[1]-v.Y(),dz=p[2]-v.Z();nearest=std::min(nearest,std::sqrt(dx*dx+dy*dy+dz*dz));}
            REQUIRE(nearest<=bound);
        }
    }
}
}
TEST_CASE("Actual original producer cuts obtain a symmetric continuous-surface upper bound", "[selected-cut-surface-certificate]")
{
    const auto product=ActualProduct();const auto& package=product.selected;SC::Certificate certificate;
    REQUIRE(SC::Build(package,package.originalSource,package.package.Identity(),certificate)==SC::Status::Certified);
    CHECK(std::isfinite(certificate.hausdorffUpper));CHECK(certificate.hausdorffUpper>0);
    CHECK(certificate.hausdorffUpper==std::max(certificate.fineToCoarse,certificate.coarseToFine));
    CHECK(certificate.originalSource==package.originalSource);CHECK(certificate.selectedKey==package.package.Identity());
    CHECK(certificate.coarseCutCount==package.coarseClusters.size());CHECK(certificate.fineCutCount==package.fineClusters.size());
    CHECK(std::equal(package.coarseClusters.begin(),package.coarseClusters.end(),certificate.coarseCut.begin()));
    CHECK(std::equal(package.fineClusters.begin(),package.fineClusters.end(),certificate.fineCut.begin()));
    const uint64_t visits=uint64_t(package.selectedGeometry.fine.indices.size()/3)*(package.selectedGeometry.coarse.vertices.size()+3)+
        uint64_t(package.selectedGeometry.coarse.indices.size()/3)*(package.selectedGeometry.fine.vertices.size()+3);
    CHECK(certificate.distanceVisits==visits);CHECK(visits<=2'000'000);
    // Independent barycentric interior checks supplement (do not replace) the convex proof.
    SamplesWithin(package.selectedGeometry.fine,package.selectedGeometry.coarse,certificate.fineToCoarse);
    SamplesWithin(package.selectedGeometry.coarse,package.selectedGeometry.fine,certificate.coarseToFine);
    auto decoded=ClodRamPackage{};
    REQUIRE(DecodeClodDisk(product.encoded,package.originalSource,package.package.Identity(),decoded)==ClodDiskStatus::Decoded);
    SC::Certificate same;REQUIRE(SC::Build(decoded,package.originalSource,package.package.Identity(),same)==SC::Status::Certified);
    CHECK(same.coarsePackedSha256==certificate.coarsePackedSha256);CHECK(same.finePackedSha256==certificate.finePackedSha256);
    CHECK(same.hausdorffUpper==certificate.hausdorffUpper);
}
TEST_CASE("Surface certificate refuses changed authority geometry topology and visit debt transactionally", "[selected-cut-surface-certificate]")
{
    auto product=ActualProduct();auto& package=product.selected;const auto original=package.originalSource;const auto key=package.package.Identity();
    SC::Certificate kept;kept.distanceVisits=99;kept.hausdorffUpper=123;
    SECTION("raw source hash mismatch"){auto wrong=original;wrong.sourceSha256[0]^=1;CHECK(SC::Build(package,wrong,key,kept)==SC::Status::Invalid);}
    SECTION("full selected packing key mismatch"){auto wrong=key;++wrong.packing.pageBytes;CHECK(SC::Build(package,original,wrong,kept)==SC::Status::Invalid);}
    SECTION("same-length packed attribute mutation"){
        const auto before=SC::Detail::PackedHash(package.selectedGeometry.fine);
        package.selectedGeometry.fine.vertices[0].t0.u+=.125f;
        CHECK(SC::Detail::PackedHash(package.selectedGeometry.fine)!=before);
        CHECK(SC::Build(package,original,key,kept)==SC::Status::Invalid);
    }
    SECTION("cut ID mutation"){++package.coarseClusters[0];CHECK(SC::Build(package,original,key,kept)==SC::Status::Invalid);}
    SECTION("threshold mutation"){package.coarseThreshold=std::nextafter(package.coarseThreshold,INFINITY);CHECK(SC::Build(package,original,key,kept)==SC::Status::Invalid);}
    SECTION("malformed actual index"){package.selectedGeometry.fine.indices.back()=UINT32_MAX;CHECK(SC::Build(package,original,key,kept)==SC::Status::Invalid);}
    SECTION("nonfinite packed position"){package.selectedGeometry.fine.vertices[0].pos=::Vector3P(INFINITY,0,0);CHECK(SC::Build(package,original,key,kept)!=SC::Status::Certified);}
    SECTION("exact visit exhaustion"){
        SC::Certificate complete;REQUIRE(SC::Build(package,original,key,complete)==SC::Status::Certified);
        REQUIRE(complete.distanceVisits>1);
        CHECK(SC::Build(package,original,key,kept,{complete.distanceVisits-1})==SC::Status::Capacity);
    }
    SECTION("zero and above hard cap"){CHECK(SC::Build(package,original,key,kept,{0})==SC::Status::Capacity);CHECK(SC::Build(package,original,key,kept,{2'000'001})==SC::Status::Capacity);}
    CHECK(kept.distanceVisits==99);CHECK(kept.hausdorffUpper==123);
}
TEST_CASE("Surface anchors must belong to target triangles including when an unused vertex is nearer", "[selected-cut-surface-certificate]")
{
    ExportedMesh from,target;from.vertices={CertVertex(0,0,0),CertVertex(0,1,0),CertVertex(0,0,1)};from.indices={0,1,2};
    target.vertices={CertVertex(0,0,0),CertVertex(10,0,0),CertVertex(10,1,0),CertVertex(10,0,1)};target.indices={1,2,3};
    std::array<uint8_t,1024> used{};used[1]=used[2]=used[3]=1;
    uint64_t visits=0;double bound=0;
    REQUIRE(SC::Detail::Directed(from,target,used,100,visits,bound)==SC::Status::Certified);
    CHECK(visits==7);CHECK(bound>=std::sqrt(101.));CHECK(bound<11);
    SamplesWithin(from,target,bound);
    // Identical triangles still have a positive SAME-anchor covering radius.
    used={};used[0]=used[1]=used[2]=1;visits=0;bound=0;
    REQUIRE(SC::Detail::Directed(from,from,used,100,visits,bound)==SC::Status::Certified);
    CHECK(bound>=1);CHECK(bound<2);
}
TEST_CASE("Outward distance and interval area gates handle exact axes cancellation and unsupported arithmetic inputs", "[selected-cut-surface-certificate]")
{
    double bound=0;const auto origin=CertVertex(0,0,0);
    REQUIRE(SC::Detail::DistanceUpper(origin,CertVertex(3,4,0),bound));CHECK(bound>=5);CHECK(bound<5.0000000000001);
    REQUIRE(SC::Detail::DistanceUpper(origin,origin,bound));CHECK(bound==0);
    REQUIRE(SC::Detail::DistanceUpper(CertVertex(10000,-10000,1),CertVertex(-10000,10000,2),bound));CHECK(bound*bound>=800000001.);
    CHECK(SC::Detail::Nondegenerate(origin,CertVertex(1,0,0),CertVertex(0,1,0)));
    CHECK_FALSE(SC::Detail::Nondegenerate(origin,CertVertex(1,1,1),CertVertex(2,2,2)));
    CHECK_FALSE(SC::Detail::Nondegenerate(origin,origin,CertVertex(0,1,0)));
    CHECK_FALSE(SC::Detail::Coordinate(std::bit_cast<float>(uint32_t(1))));
    CHECK_FALSE(SC::Detail::Coordinate(INFINITY));CHECK_FALSE(SC::Detail::Coordinate(10001));
    CHECK(SC::Detail::Coordinate(std::numeric_limits<float>::min()));
    CHECK(SC::Detail::Coordinate(-0.f));
}

namespace {
struct CertificateFiles {
    std::filesystem::path parent,root,input,off,on,failed;
    CertificateFiles(){namespace fs=std::filesystem;parent=fs::canonical(fs::temp_directory_path());
        static std::atomic<uint64_t> serial=0;
        for(unsigned attempt=0;attempt<16;++attempt){root=parent/("poseidon-surface-cli-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
            if(fs::create_directory(root)){input=root/"original.p3d";off=root/"off";on=root/"on";failed=root/"failed";return;}}
        throw std::runtime_error("unable to create private certificate fixture");}
    ~CertificateFiles(){namespace fs=std::filesystem;std::error_code error;
        if(root.empty()||fs::is_symlink(root,error)||fs::canonical(root,error).parent_path()!=parent||error)return;
        for(const auto& directory:{off,on,failed}) {
            if(fs::is_symlink(directory,error))continue;
            fs::remove(directory/"surface-certificate.json",error);fs::remove(directory/"selected.gcd",error);
            fs::remove(directory/"manifest.json",error);fs::remove(directory,error);
        }
        fs::remove(input,error);fs::remove(root,error);}
    void Write(){OriginalGrid source;std::ofstream file(input,std::ios::binary);
        REQUIRE(file.good());file.write(reinterpret_cast<const char*>(source.bytes.data()),std::streamsize(source.bytes.size()));REQUIRE(file.good());}
};
std::string CertificateRead(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);
    REQUIRE(file.good());return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
std::string CertificateField(const std::string& text,const char* key,bool quoted=false){
    const auto marker=std::string("\"")+key+"\":"+(quoted?"\"":"");
    const auto found=text.find(marker);REQUIRE(found!=std::string::npos);const auto start=found+marker.size();
    const auto finish=text.find_first_of(quoted?"\"":",}\n",start);REQUIRE(finish!=std::string::npos);
    return text.substr(start,finish-start);
}
double CertificateNumber(const std::string& text,const char* key){
    std::istringstream input(CertificateField(text,key));input.imbue(std::locale::classic());double value=0;input>>value;
    REQUIRE_FALSE(input.fail());REQUIRE(input.peek()==std::char_traits<char>::eof());return value;
}
std::string CertificateIds(const std::array<uint32_t,64>& ids,uint32_t count){
    std::ostringstream out;out.imbue(std::locale::classic());out<<'[';
    for(uint32_t i=0;i<count;++i){if(i)out<<',';out<<ids[i];}out<<']';return out.str();
}
}
TEST_CASE("Optional original producer certificate is separately published with exact bindings and unchanged artifacts", "[selected-cut-surface-certificate][selected-cut-surface-cli]")
{
    namespace MP=PoseidonTools::MlodPages;namespace fs=std::filesystem;
    Poseidon::Foundation::CaptureMainThread();CertificateFiles files;files.Write();MP::Product product;
    std::ostringstream published;
    REQUIRE(MP::ProduceFile(files.input,files.on,product,&published,true)==MP::ProducerStatus::Produced);
    REQUIRE(published.str()==product.manifest);REQUIRE(CertificateRead(files.on/"manifest.json")==product.manifest);
    const auto bytes=CertificateRead(files.on/"selected.gcd");
    REQUIRE(bytes.size()==product.encoded.size());REQUIRE(std::memcmp(bytes.data(),product.encoded.data(),bytes.size())==0);
    const auto descriptor=CertificateRead(files.on/"surface-certificate.json");REQUIRE(descriptor.size()<=8192);
    Poseidon::Foundation::Sha256 fileHash;fileHash.Update(product.encoded.data(),product.encoded.size());
    CHECK(CertificateField(descriptor,"fileSha256",true)==fileHash.Hex());
    CHECK(CertificateNumber(descriptor,"fileBytes")==product.encoded.size());
    SC::Certificate expected;
    REQUIRE(SC::Build(product.selected,product.selected.originalSource,product.selected.package.Identity(),expected)==SC::Status::Certified);
    REQUIRE(descriptor.find("\"originalSource\":"+MP::Identity(expected.originalSource,true))!=std::string::npos);
    REQUIRE(descriptor.find("\"source\":"+MP::Identity(expected.selectedKey.source,true))!=std::string::npos);
    CHECK(CertificateNumber(descriptor,"formatVersion")==expected.selectedKey.formatVersion);
    CHECK(CertificateNumber(descriptor,"algorithmVersion")==SC::Certificate::AlgorithmVersion);
    CHECK(descriptor.find("\"algorithmVersion\":"+std::to_string(expected.selectedKey.algorithmVersion)+",\"packing\"")!=std::string::npos);
    CHECK(CertificateNumber(descriptor,"clusterVertices")==expected.selectedKey.packing.clusterVertices);
    CHECK(CertificateNumber(descriptor,"clusterTriangles")==expected.selectedKey.packing.clusterTriangles);
    CHECK(CertificateNumber(descriptor,"pageBytes")==expected.selectedKey.packing.pageBytes);
    CHECK(CertificateField(descriptor,"coarsePackedSha256",true)==MP::Hex(expected.coarsePackedSha256));
    CHECK(CertificateField(descriptor,"finePackedSha256",true)==MP::Hex(expected.finePackedSha256));
    CHECK(descriptor.find("\"coarseCutIds\":"+CertificateIds(expected.coarseCut,expected.coarseCutCount))!=std::string::npos);
    CHECK(descriptor.find("\"fineCutIds\":"+CertificateIds(expected.fineCut,expected.fineCutCount))!=std::string::npos);
    CHECK(CertificateNumber(descriptor,"coarseThresholdBits")==expected.coarseThresholdBits);
    CHECK(CertificateNumber(descriptor,"fineThresholdBits")==expected.fineThresholdBits);
    CHECK(CertificateNumber(descriptor,"distanceVisits")==expected.distanceVisits);
    CHECK(CertificateNumber(descriptor,"distanceVisitCap")==2'000'000);
    for(const auto pair:{std::pair{"fineToCoarseUpper",expected.fineToCoarse},
                        std::pair{"coarseToFineUpper",expected.coarseToFine},std::pair{"hausdorffUpper",expected.hausdorffUpper}}) {
        CHECK(std::bit_cast<uint64_t>(CertificateNumber(descriptor,pair.first))==std::bit_cast<uint64_t>(pair.second));
        const auto bitsKey=std::string(pair.first)+"BitsHex";
        CHECK(CertificateField(descriptor,bitsKey.c_str(),true)==MP::Hex64(std::bit_cast<uint64_t>(pair.second),true));
    }
    CHECK(descriptor.find("not-original-source-fidelity/pixels/attributes")!=std::string::npos);
    // Ordinary publication still produces EXACTLY the same pair, with no descriptor
    // and no surface computation. The default argument exercises the old call site.
    REQUIRE(MP::Publish(product,files.off)==MP::ProducerStatus::Produced);
    CHECK_FALSE(fs::exists(files.off/"surface-certificate.json"));
    CHECK(CertificateRead(files.off/"manifest.json")==product.manifest);
    CHECK(CertificateRead(files.off/"selected.gcd")==bytes);
    size_t offFiles=0;for(const auto& unused:fs::directory_iterator(files.off)){(void)unused;++offFiles;}CHECK(offFiles==2);
    // Re-publication never overwrites a previously complete descriptor or pair.
    CHECK(MP::Publish(product,files.on,nullptr,true)==MP::ProducerStatus::IoFailure);
    CHECK(CertificateRead(files.on/"surface-certificate.json")==descriptor);
}
TEST_CASE("Certificate refusal and post-write output failure publish no partial private artifact", "[selected-cut-surface-certificate][selected-cut-surface-cli]")
{
    namespace MP=PoseidonTools::MlodPages;CertificateFiles files;files.Write();auto product=ActualProduct(true);
    SECTION("same-length packed mutation is refused before create directory or stdout") {
        product.selected.selectedGeometry.fine.vertices[0].t0.u+=.125f;std::ostringstream sink;
        CHECK(MP::Publish(product,files.failed,&sink,true)==MP::ProducerStatus::Invalid);
        CHECK(sink.str().empty());CHECK_FALSE(std::filesystem::exists(files.failed));
    }
    SECTION("encoded-only mutation refuses before publication") {
        REQUIRE(product.HasProducedPublicationBinding());product.encoded.back()^=1;std::ostringstream sink;
        CHECK_FALSE(product.HasProducedPublicationBinding());
        CHECK(MP::Publish(product,files.failed,&sink,true)==MP::ProducerStatus::Unsupported);
        CHECK(sink.str().empty());CHECK_FALSE(std::filesystem::exists(files.failed));
    }
    SECTION("manifest-only mutation refuses before publication") {
        REQUIRE(product.HasProducedPublicationBinding());product.manifest.back()=' ';std::ostringstream sink;
        CHECK_FALSE(product.HasProducedPublicationBinding());
        CHECK(MP::Publish(product,files.failed,&sink,true)==MP::ProducerStatus::Unsupported);
        CHECK(sink.str().empty());CHECK_FALSE(std::filesystem::exists(files.failed));
    }
    SECTION("changed selected key and canonical re-encode cannot recreate original producer seal") {
        // Threshold bits belong to the selected key even when the paired cut IDs
        // stay the same. Recompute the actual canonical derived key/package.
        auto& selected=product.selected;
        selected.coarseThreshold=std::nextafter(selected.coarseThreshold,INFINITY);
        const auto derived=ClodDiskDetail::Derived(selected);
        selected.package.source=derived;selected.selectedGeometry.source=derived;
        REQUIRE(EncodeClodDisk(selected,product.encoded)==ClodDiskStatus::Encoded);
        std::ostringstream sink;
        CHECK_FALSE(product.HasProducedPublicationBinding());
        CHECK(MP::Publish(product,files.failed,&sink,true)==MP::ProducerStatus::Unsupported);
        CHECK(sink.str().empty());CHECK_FALSE(std::filesystem::exists(files.failed));
    }
    SECTION("unsealed ordinary Product refuses optional publication without changing default mode") {
        auto unsealed=ActualProduct();std::ostringstream sink;CHECK_FALSE(unsealed.HasProducedPublicationBinding());
        CHECK(MP::Publish(unsealed,files.failed,&sink,true)==MP::ProducerStatus::Unsupported);
        CHECK(sink.str().empty());CHECK_FALSE(std::filesystem::exists(files.failed));
        CHECK(MP::Publish(unsealed,files.off)==MP::ProducerStatus::Produced);
        CHECK_FALSE(std::filesystem::exists(files.off/"surface-certificate.json"));
    }
    SECTION("failed stdout rolls back all three already-written owned files") {
        std::ostringstream sink;sink.setstate(std::ios::badbit);MP::Product kept;kept.manifest="sentinel";
        CHECK(MP::ProduceFile(files.input,files.failed,kept,&sink,true)==MP::ProducerStatus::IoFailure);
        CHECK(kept.manifest=="sentinel");CHECK_FALSE(std::filesystem::exists(files.failed));
    }
    CHECK(std::filesystem::exists(files.input));
}
TEST_CASE("Surface descriptor numbers and full identity remain classic under a decimal-comma global locale", "[selected-cut-surface-certificate][selected-cut-surface-cli]")
{
    namespace MP=PoseidonTools::MlodPages;const auto product=ActualProduct(true);std::string ordinary,localized;
    REQUIRE(MP::BuildSurfaceDescriptor(product,ordinary)==MP::ProducerStatus::Produced);
    struct Comma : std::numpunct<char> {char do_decimal_point() const override{return ',';}
        char do_thousands_sep() const override{return '.';}std::string do_grouping() const override{return "\3";}};
    {
        struct Restore {std::locale previous=std::locale();~Restore(){std::locale::global(previous);}} restore;
        std::locale::global(std::locale(std::locale::classic(),new Comma));
        REQUIRE(MP::BuildSurfaceDescriptor(product,localized)==MP::ProducerStatus::Produced);
    }
    CHECK(localized==ordinary);CHECK(CertificateNumber(localized,"hausdorffUpper")>0);
}
