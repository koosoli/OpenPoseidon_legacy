#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageControlledMlodAsset.hpp>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace {
struct AdmissionOriginal {
    std::vector<uint8_t> bytes;
    void U32(uint32_t v){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(v>>(8*i)));}
    void F(float v){U32(std::bit_cast<uint32_t>(v));}
    void Text(const char* p){while(*p)bytes.push_back(uint8_t(*p++));}
    void Z(const char* p){Text(p);bytes.push_back(0);}
    void Fixed(const char* p){const size_t at=bytes.size();Z(p);bytes.resize(at+64,0);}
    AdmissionOriginal(){
        Text("MLOD");U32(0x101);U32(2);
        for(unsigned lod=0;lod<2;++lod){
            Text("P3DM");U32(28);U32(256);U32(4);U32(1);U32(1);U32(0);
            for(auto xyz:std::array<std::array<float,3>,4>{{{40,2,50},{41,2,50},{41,2,51},{40,2,51}}}){for(float v:xyz)F(v);U32(0);}
            F(0);F(1);F(0);U32(lod?4:3);
            for(unsigned v=0;v<4;++v){U32(v<(lod?4:3)?v:0);U32(0);F(v<(lod?4:3)?float(v&1):0);F(v<(lod?4:3)?float(v/2):0);}
            U32(0);Z("");Z("");Text("TAGG");bytes.push_back(1);Z("#Property#");U32(128);Fixed("autocenter");Fixed("0");
            bytes.push_back(1);Z("#EndOfFile#");U32(0);F(lod?1:10);
        }
    }
};
struct PrivateOriginalFile {
    std::filesystem::path root,dir,file;
    bool owned=false;
    PrivateOriginalFile(){
        static std::atomic<uint64_t> sequence{0};root=std::filesystem::canonical(std::filesystem::temp_directory_path());
        for(unsigned i=0;i<32;++i){dir=root/("original-admission-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++));
            std::error_code error;if(std::filesystem::create_directory(dir,error)){owned=true;break;}}
        REQUIRE(owned);file=dir/"original.p3d";
    }
    ~PrivateOriginalFile(){
        std::error_code e;if(!owned||dir.parent_path()!=root||dir.filename().string().find("original-admission-")!=0)return;
        if(std::filesystem::is_symlink(dir,e)||e)return;
        const auto resolved=std::filesystem::weakly_canonical(dir,e);if(e||resolved!=dir||resolved.parent_path()!=root)return;
        std::filesystem::remove(file,e);e.clear();std::filesystem::remove(dir,e); // Only owned file and empty directory.
    }
    void Write(const std::vector<uint8_t>& bytes){std::ofstream out(file,std::ios::binary|std::ios::trunc);REQUIRE(out);
        out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));out.close();REQUIRE(out);}
};
std::string Hash(const std::vector<uint8_t>& b){Foundation::Sha256 sha;sha.Update(b.data(),b.size());return sha.Hex();}
std::string Key(const SourceIdentity& s){return std::to_string(s.geometryOptions)+":"+std::to_string(s.materialOptions)+":"+
    std::to_string(s.producerVersion)+":"+std::to_string(s.coarseRepresentation)+":"+std::to_string(s.fineRepresentation)+":"+
    std::to_string(s.vertexLayout)+":"+std::to_string(s.materialMapping);}
}
TEST_CASE("Original startup captures actual trusted bytes and full export identity before path changes", "[geometry-page-original-admission]") {
    Foundation::CaptureMainThread();AdmissionOriginal original;ControlledMlodAsset independentlyExported;
    REQUIRE(ExportControlledMlodOwned(original.bytes,independentlyExported)==ControlledMlodStatus::Exported);
    PrivateOriginalFile file;file.Write(original.bytes);Engine::GeometryPageOriginalStartupInput input;
    REQUIRE(Engine::ParseGeometryPageOriginalStartupInput(file.file.string(),Hash(original.bytes),original.bytes.size(),Key(independentlyExported.original.source),input));
    std::vector<uint8_t> snapshot{0x5a};REQUIRE(Engine::ReadGeometryPageOriginalSnapshot(input,snapshot)==Engine::GeometryPageOriginalStatus::Admitted);
    REQUIRE(snapshot==original.bytes);
    // File contents can change after capture; owner export still observes exclusively captured original bytes.
    auto replacement=original.bytes;replacement[40]^=1;file.Write(replacement);
    ControlledMlodAsset actual;REQUIRE(ExportControlledMlodOwned(std::move(snapshot),actual)==ControlledMlodStatus::Exported);
    REQUIRE(Engine::GeometryPageOriginalIdentityMatches(input.expectedOriginal,actual.original.source));
    REQUIRE(actual.original.fine.positions.front().x>=40); // Original authored offset, no fixed-radius/synthetic fallback claim.
    auto expected=input.expectedOriginal;
    for(unsigned field=0;field<8;++field){expected=input.expectedOriginal;switch(field){
        case 0:expected.sha256[0]^=1;break;case 1:++expected.geometryOptions;break;case 2:++expected.materialOptions;break;
        case 3:++expected.producerVersion;break;case 4:++expected.coarseRepresentation;break;case 5:++expected.fineRepresentation;break;
        case 6:++expected.vertexLayout;break;case 7:++expected.materialMapping;break;}
        REQUIRE_FALSE(Engine::GeometryPageOriginalIdentityMatches(expected,actual.original.source));}
    const std::vector<uint8_t> sentinel{0x5a,0xa5};snapshot=sentinel;
    REQUIRE(Engine::ReadGeometryPageOriginalSnapshot(input,snapshot)==Engine::GeometryPageOriginalStatus::IdentityMismatch);REQUIRE(snapshot==sentinel);
}
TEST_CASE("Original startup rejects bad lengths hashes paths and owner without partial output", "[geometry-page-original-admission]") {
    Foundation::CaptureMainThread();AdmissionOriginal original;ControlledMlodAsset asset;
    REQUIRE(ExportControlledMlodOwned(original.bytes,asset)==ControlledMlodStatus::Exported);
    PrivateOriginalFile file;file.Write(original.bytes);Engine::GeometryPageOriginalStartupInput input;
    REQUIRE(Engine::ParseGeometryPageOriginalStartupInput(file.file.string(),Hash(original.bytes),original.bytes.size(),Key(asset.original.source),input));
    const auto valid=input;std::vector<uint8_t> output{7,8};const auto sentinel=output;
    Engine::GeometryPageOriginalStatus foreign=Engine::GeometryPageOriginalStatus::Admitted;
    std::thread wrong([&]{foreign=Engine::ReadGeometryPageOriginalSnapshot(input,output);});wrong.join();
    REQUIRE(foreign==Engine::GeometryPageOriginalStatus::WrongOwner);REQUIRE(output==sentinel);
    input.expectedRawBytes=128u*1024u+1;REQUIRE(Engine::ReadGeometryPageOriginalSnapshot(input,output)==Engine::GeometryPageOriginalStatus::Capacity);REQUIRE(output==sentinel);
    input=valid;--input.expectedRawBytes;REQUIRE(Engine::ReadGeometryPageOriginalSnapshot(input,output)==Engine::GeometryPageOriginalStatus::IdentityMismatch);REQUIRE(output==sentinel);
    input=valid;input.originalPath.fill(0);std::copy_n("relative.p3d",12,input.originalPath.data());
    REQUIRE(Engine::ReadGeometryPageOriginalSnapshot(input,output)==Engine::GeometryPageOriginalStatus::Invalid);REQUIRE(output==sentinel);
    input=valid;file.Write(std::vector<uint8_t>(128u*1024u+1,0));
    REQUIRE(Engine::ReadGeometryPageOriginalSnapshot(input,output)==Engine::GeometryPageOriginalStatus::Capacity);REQUIRE(output==sentinel);
    std::filesystem::remove(file.file);REQUIRE(Engine::ReadGeometryPageOriginalSnapshot(valid,output)==Engine::GeometryPageOriginalStatus::IoFailure);REQUIRE(output==sentinel);
}
TEST_CASE("Original startup argument admission is bounded strict and transactional", "[geometry-page-original-admission]") {
    Engine::GeometryPageOriginalStartupInput out;out.expectedRawBytes=17;const auto hash=std::string(64,'a');
    for(const auto& key:{"1:0:1:0:1:68:1:","1:0:1:0:1:68", "1:0:1:0:0:68:1", "1:0:4294967296:0:1:68:1", "1:0:+1:0:1:68:1", "1:0:1:0:1:68:1garbage"}){
        REQUIRE_FALSE(Engine::ParseGeometryPageOriginalStartupInput("/private/original.p3d",hash,16,key,out));REQUIRE(out.expectedRawBytes==17);}
    REQUIRE_FALSE(Engine::ParseGeometryPageOriginalStartupInput("/private/original.p3d",std::string(64,'0'),16,"1:0:1:0:1:68:1",out));
    REQUIRE_FALSE(Engine::ParseGeometryPageOriginalStartupInput(std::string(1024,'a'),hash,16,"1:0:1:0:1:68:1",out));
    REQUIRE_FALSE(Engine::ParseGeometryPageOriginalStartupInput("/private/original.p3d",hash,0,"1:0:1:0:1:68:1",out));
    REQUIRE_FALSE(Engine::ParseGeometryPageOriginalStartupInput("/private/original.p3d",hash,131073,"1:0:1:0:1:68:1",out));
    REQUIRE(out.expectedRawBytes==17);
}
