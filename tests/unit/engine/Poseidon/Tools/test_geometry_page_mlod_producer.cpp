#include <catch2/catch_test_macros.hpp>
#include "../../../../../apps/tools/Tools/commands/ControlledMlodPageProducer.hpp"
#include <atomic>
#include <chrono>
#include <iterator>

using namespace PoseidonTools::MlodPages;
namespace
{
struct OriginalGrid
{
    std::vector<uint8_t> bytes;
    void U32(uint32_t v){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(v>>(i*8)));}
    void F(float v){U32(std::bit_cast<uint32_t>(v));}
    void Text(const char* s){while(*s)bytes.push_back(uint8_t(*s++));}
    void Z(const char* s){Text(s);bytes.push_back(0);}
    void Fixed(const char* s){const auto at=bytes.size();Z(s);bytes.resize(at+64,0);}
    void Tag(const char* s,uint32_t n){bytes.push_back(1);Z(s);U32(n);}
    explicit OriginalGrid(unsigned fine=16,const char* texture="")
    {
        Text("MLOD");U32(0x101);U32(2);
        for(unsigned level=0;level<2;++level){const unsigned n=level?fine:1;
            Text("P3DM");U32(28);U32(256);U32((n+1)*(n+1));U32(1);U32(n*n);U32(0);
            for(unsigned z=0;z<=n;++z)for(unsigned x=0;x<=n;++x){const float px=float(x)/n*4-2,pz=float(z)/n*4-2;
                F(px);F(.9f*(1-px*px/4)*(1-pz*pz/4));F(pz);U32(0);}
            F(0);F(1);F(0);
            for(unsigned z=0;z<n;++z)for(unsigned x=0;x<n;++x){U32(4);
                const std::array<std::array<unsigned,2>,4> corners{{{x,z},{x,z+1},{x+1,z+1},{x+1,z}}};
                for(auto p:corners){U32(p[1]*(n+1)+p[0]);U32(0);F(float(p[0])/n);F(float(p[1])/n);}
                U32(0);Z(texture);Z("");}
            Text("TAGG");Tag("#Property#",128);Fixed("autocenter");Fixed("0");Tag("#EndOfFile#",0);F(level?1:10);
        }
    }
};
struct PrivateFiles
{
    std::filesystem::path root,parent,input,output;
    PrivateFiles(){namespace fs=std::filesystem;parent=fs::canonical(fs::temp_directory_path());
        static std::atomic<uint64_t> count=0;
        for(unsigned attempt=0;attempt<16;++attempt){root=parent/("poseidon-mlod-producer-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++count));
            if(fs::create_directory(root)){input=root/"original.p3d";output=root/"package";return;}}
        throw std::runtime_error("unable to create private test directory");}
    ~PrivateFiles(){namespace fs=std::filesystem;std::error_code e;
        if(root.empty()||fs::is_symlink(root,e)||fs::canonical(root,e).parent_path()!=parent||e)return;
        // Only exact owned paths; never recursive or arbitrary directory deletion.
        fs::remove(output/"selected.gcd",e);fs::remove(output/"manifest.json",e);fs::remove(output,e);
        fs::remove(input,e);fs::remove(root,e);}
    void Write(const std::vector<uint8_t>& bytes){std::ofstream out(input,std::ios::binary);
        REQUIRE(out.good());out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));REQUIRE(out.good());}
};
}
TEST_CASE("Actual original curved MLOD file produces independently keyed selected disk cuts", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;PrivateFiles files;files.Write(source.bytes);
    Product result;REQUIRE(ProduceFile(files.input,files.output,result)==ProducerStatus::Produced);
    REQUIRE(result.selected.originalSource.sourceSha256!=result.selected.package.Identity().source.sourceSha256);
    Poseidon::Foundation::Sha256 raw;raw.Update(source.bytes.data(),source.bytes.size());
    REQUIRE(Hex(result.selected.originalSource.sourceSha256)==raw.Hex());
    REQUIRE(result.selected.selectedGeometry.fine.indices.size()/3==512);
    REQUIRE(result.selected.selectedGeometry.coarse.indices.size()<result.selected.selectedGeometry.fine.indices.size());
    REQUIRE(result.encoded.size()<=128*1024);REQUIRE(result.selected.knownCapacityBytes<=128*1024);
    std::ifstream input(files.output/"selected.gcd",std::ios::binary);
    std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};REQUIRE(actual==result.encoded);
    ClodRamPackage decoded;REQUIRE(DecodeClodDisk(actual,result.selected.originalSource,result.selected.package.Identity(),decoded)==ClodDiskStatus::Decoded);
    REQUIRE(decoded.selectedGeometry.fine.indices==result.selected.selectedGeometry.fine.indices);
    REQUIRE(result.manifest.find("\"schemaVersion\":2")!=std::string::npos);
    REQUIRE(result.manifest.find(raw.Hex())!=std::string::npos);
    // Reuse of an existing output must fail without changing its valid original artifacts.
    Product untouched;REQUIRE(ProduceFile(files.input,files.output,untouched)==ProducerStatus::IoFailure);
    REQUIRE(untouched.encoded.empty());REQUIRE(std::filesystem::file_size(files.output/"selected.gcd")==actual.size());
}
TEST_CASE("Bounded original-file producer refuses before publication transactionally", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();PrivateFiles files;OriginalGrid source;auto bytes=source.bytes;
    ProducerStatus expected=ProducerStatus::Invalid;
    SECTION("too small for pinned CLOD") {bytes=OriginalGrid(4).bytes;expected=ProducerStatus::Unsupported;}
    SECTION("unsupported texture") {bytes=OriginalGrid(16,"unapproved.paa").bytes;expected=ProducerStatus::Unsupported;}
    SECTION("truncated") {bytes.pop_back();}
    SECTION("trailing") {bytes.push_back(0);}
    SECTION("oversize before snapshot allocation") {bytes.resize(16*1024*1024+1);expected=ProducerStatus::Capacity;}
    files.Write(bytes);Product kept;kept.manifest="sentinel";
    REQUIRE(ProduceFile(files.input,files.output,kept)==expected);
    REQUIRE(kept.manifest=="sentinel");REQUIRE_FALSE(std::filesystem::exists(files.output));
}
TEST_CASE("Actual source-byte mutation changes producer keys without filename freshness inference", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;auto changed=source.bytes;
    const uint32_t value=std::bit_cast<uint32_t>(-1.75f);for(unsigned i=0;i<4;++i)changed[40+i]=uint8_t(value>>(i*8));
    Product first,second;REQUIRE(BuildOwned(source.bytes,first)==ProducerStatus::Produced);REQUIRE(BuildOwned(changed,second)==ProducerStatus::Produced);
    REQUIRE(first.selected.originalSource.sourceSha256!=second.selected.originalSource.sourceSha256);
    REQUIRE(first.selected.package.Identity()!=second.selected.package.Identity());
    ClodRamPackage refused;REQUIRE(DecodeClodDisk(second.encoded,first.selected.originalSource,first.selected.package.Identity(),refused)!=ClodDiskStatus::Decoded);
}

TEST_CASE("Original producer output failure removes only its private artifact pair", "[geometry-page-mlod-producer]")
{
    Poseidon::Foundation::CaptureMainThread();OriginalGrid source;PrivateFiles files;files.Write(source.bytes);
    std::ostringstream refused;refused.setstate(std::ios::badbit);Product kept;kept.manifest="sentinel";
    REQUIRE(ProduceFile(files.input,files.output,kept,&refused)==ProducerStatus::IoFailure);
    REQUIRE(kept.manifest=="sentinel");REQUIRE_FALSE(std::filesystem::exists(files.output));
    REQUIRE(std::filesystem::exists(files.input));
}
