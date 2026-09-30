#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRigidOdol7Candidate.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <limits>

using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
// Complete finite ODOL7 wire source, not fabricated audit/DAG rows. A curved
// four-vertex quad followed by a triangle exercises mixed original face order,
// x86 byte section ranges and the existing canonical ODOL7 loader.
struct Wire
{
    std::vector<uint8_t> bytes;
    void U(uint32_t value,unsigned n=4){for(unsigned i=0;i<n;++i)bytes.push_back(uint8_t(value>>(8*i)));}
    void F(float value){U(std::bit_cast<uint32_t>(value));}
    void Z(const char* value){while(*value)bytes.push_back(uint8_t(*value++));bytes.push_back(0);}
    Wire()
    {
        for(char c:std::string("ODOL"))bytes.push_back(uint8_t(c));U(7);U(3);
        for(unsigned lod=0;lod<3;++lod) {
            U(4);for(unsigned v=0;v<4;++v)U(63); // clip flags: no land/fog/deformation
            U(4);for(unsigned v=0;v<4;++v){F(float(v&1));F(float(v>>1));}
            U(4);for(auto p:std::array<Position,4>{{{0,0,0},{1,0,0},{1,.25f,1},{0,0,1}}}){F(p.x);F(p.y);F(p.z);}
            U(4);for(unsigned v=0;v<4;++v){F(0);F(1);F(0);}
            for(unsigned i=0;i<12;++i)U(0); // exact48-byte bounds/hints
            U(1);Z("data\\skala_piskovec2.pac");U(0);U(0); // edges
            U(2);U(0); // source faces / optional offset
            U(0x2000);U(0,2);U(4,1);for(unsigned v=0;v<4;++v)U(v,2);
            U(0x2000);U(0,2);U(3,1);for(unsigned v:{0u,2u,3u})U(v,2);
            U(1);U(0);U(34);U(0);U(0,2);U(0x2000); // quad18 + tri16 bytes
            U(0); // named selections
            U(lod==0?1:0);if(lod==0){Z("lodnoshadow");Z("1");}
            U(0); // keyframes
            U(0);U(0);U(0);U(0); // end colors/special, proxies
        }
        F(2.5f);F(12);F(1e13f);
        for(unsigned i=0;i<36;++i)U(0); // model fields144 bytes
        for(unsigned i=0;i<5;++i)U(0,1);U(11,1); // no animation, map rock
        U(0);for(unsigned i=0;i<4;++i)F(0); // mass table / mass, armor
        for(unsigned i=0;i<12;++i)U(i==1?2:255,1); // geometry2, others absent
    }
};
struct Source
{
    Wire wire;
    Model::Model model;
    RigidOdol7SourceBinding binding;
    Source():model(Asset::Formats::ODOLLoader::loadFromBuffer(
        reinterpret_cast<const char*>(wire.bytes.data()),int(wire.bytes.size()),"data3d\\skala_new.p3d"))
    {
        binding.canonicalPath=model.sourcePath;binding.decodedBytes=wire.bytes.size();
        Foundation::Sha256 hash;hash.Update(wire.bytes.data(),wire.bytes.size());const auto hex=hash.Hex();
        const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
        for(size_t i=0;i<32;++i)binding.decodedSha256[i]=uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1]));
    }
};
}
TEST_CASE("ODOL7 candidate uses complete source audit and preserves raw basis and nonvisual IR", "[geometry-page-odol7-candidate]")
{
    Source source;RigidOdol7Candidate candidate;
    REQUIRE(Model::HasStaticOdolSourceAudit(source.model));
    REQUIRE(InspectRigidOdol7Candidate(source.model,source.binding,candidate)==RigidOdol7Status::Candidate);
    REQUIRE(candidate.visual.size()==2);REQUIRE(candidate.lodEvidence.size()==3);
    REQUIRE(candidate.lodEvidence[2].purpose==Model::LodPurpose::Geometry);
    REQUIRE(candidate.visual[0].lodNoShadow);REQUIRE_FALSE(candidate.visual[1].lodNoShadow);
    REQUIRE(candidate.visual[0].sectionSpecial==0x2000);
    REQUIRE(candidate.visual[0].sourceBasis.windingConfidence==Model::BasisConfidence::Unknown);
    REQUIRE(candidate.visual[0].vertices[0].normal.y==1); // no SVertex negation assumption
    REQUIRE(candidate.visual[0].indices==std::vector<uint32_t>{0,1,2,0,2,3,0,2,3});
    REQUIRE(candidate.visual[0].triangleMaterials==std::vector<uint32_t>{0,0,0});
    const auto input=candidate.visual[0].Input();
    REQUIRE(input.stride==sizeof(RigidOdol7IrVertex));
    REQUIRE(input.positions[2].y==.25f);REQUIRE(candidate.visual[0].vertices[2].position.y==input.positions[2].y);
    REQUIRE(source.model.lodLevels[2].mesh.vertices.size()==4);
    REQUIRE(source.model.geometryIdx==2);REQUIRE(source.model.lodLevels[0].mesh.sections[0].triangleCount==34);
    // Nonvisual component names are retained in the caller IR, not interpreted as
    // visual deformation. No physics mesh or simulation-ready fact is exported.
    source.model.lodLevels[2].mesh.selections.emplace_back("component01");
    REQUIRE(InspectRigidOdol7Candidate(source.model,source.binding,candidate)==RigidOdol7Status::Candidate);
    REQUIRE(source.model.lodLevels[2].mesh.selections[0].name=="component01");
}
TEST_CASE("ODOL7 candidate refuses missing or animated source audit transactionally", "[geometry-page-odol7-candidate]")
{
    Source original;
    for(unsigned fault=0;fault<8;++fault) {
        auto model=original.model;RigidOdol7Candidate kept;kept.mapType=999;
        switch(fault) {
            case 0:model.sourceAudit.producerVersion=0;break;
            case 1:model.sourceAudit.observations=0;break;
            case 2:model.sourceAudit.keyframePayloadDiscarded=true;break;
            case 3:model.sourceAudit.vertexBoneReferenceCount=1;break;
            case 4:model.allowAnimation=1;break;
            case 5:model.lodLevels[0].mesh.selections.emplace_back("door");break;
            case 6:model.lodLevels[2].mesh.proxies.emplace_back("proxy");break;
            case 7:model.sourceVersion=73;break;
        }
        REQUIRE(InspectRigidOdol7Candidate(model,original.binding,kept)==RigidOdol7Status::Unsupported);
        REQUIRE(kept.mapType==999);REQUIRE(kept.visual.empty());
    }
}
TEST_CASE("ODOL7 raw export rejects changed bindings and source face corruption", "[geometry-page-odol7-candidate]")
{
    Source original;
    for(unsigned fault=0;fault<8;++fault) {
        auto model=original.model;auto binding=original.binding;
        RigidOdol7Candidate kept;kept.mapType=999;
        switch(fault) {
            case 0:binding.decodedSha256={};break;
            case 1:binding.canonicalPath="other.p3d";break;
            case 2:binding.decodedBytes=128*1024+1;break;
            case 3:model.lodLevels[0].mesh.sections[0].triangleCount=2;break; // bytes !=faces
            case 4:model.lodLevels[0].mesh.triangles[0].originalIndex=0;break; // duplicate withquad
            case 5:model.lodLevels[0].mesh.quads[0].indices[1]=4;break;
            case 6:model.lodLevels[0].mesh.vertices[0].normal.y=std::numeric_limits<float>::quiet_NaN();break;
            case 7:model.lodLevels[0].purpose=Model::LodPurpose::Memory;break;
        }
        REQUIRE(InspectRigidOdol7Candidate(model,binding,kept)==RigidOdol7Status::Invalid);
        REQUIRE(kept.mapType==999);
    }
}
TEST_CASE("ODOL7 candidate preserves unused zero normals but refuses referenced zero normals", "[geometry-page-odol7-candidate]")
{
    Source source;
    auto& mesh=source.model.lodLevels[0].mesh;
    mesh.vertices.push_back(mesh.vertices[0]);
    mesh.vertices.back().normal={0,0,0};
    RigidOdol7Candidate candidate;
    REQUIRE(InspectRigidOdol7Candidate(source.model,source.binding,candidate)==RigidOdol7Status::Candidate);
    REQUIRE(candidate.visual[0].vertices.size()==5);
    REQUIRE(candidate.visual[0].vertices.back().normal.x==0);
    REQUIRE(candidate.visual[0].vertices.back().normal.y==0);
    REQUIRE(candidate.visual[0].vertices.back().normal.z==0);
    REQUIRE(candidate.visual[0].indices==std::vector<uint32_t>{0,1,2,0,2,3,0,2,3});

    // The same literal zero becomes invalid as soon as an authored face uses it.
    mesh.triangles[0].indices[0]=4;
    RigidOdol7Candidate unchanged;unchanged.mapType=999;
    REQUIRE(InspectRigidOdol7Candidate(source.model,source.binding,unchanged)==RigidOdol7Status::Invalid);
    REQUIRE(unchanged.mapType==999);REQUIRE(unchanged.visual.empty());

    // An invalid face index must fail before it can address the bounded mask.
    mesh.triangles[0].indices[0]=4096;
    REQUIRE(InspectRigidOdol7Candidate(source.model,source.binding,unchanged)==RigidOdol7Status::Invalid);
    REQUIRE(unchanged.mapType==999);REQUIRE(unchanged.visual.empty());
}
TEST_CASE("ODOL7 candidate refuses special visual semantics and material mapping changes", "[geometry-page-odol7-candidate]")
{
    Source original;
    for(unsigned fault=0;fault<9;++fault) {
        auto model=original.model;auto& lod=model.lodLevels[0];RigidOdol7Candidate kept;kept.mapType=999;
        switch(fault) {
            case 0:lod.mesh.vertices[0].flags=Model::VertexFlags::LandKeep;break;
            case 1:lod.mesh.vertices[0].flags=Model::VertexFlags::FogSky;break;
            case 2:lod.mesh.sections[0].hints=Model::RenderHints(0x2100);break;
            case 3:lod.mesh.materials[0].materialPath="unknown.rvmat";break;
            case 4:lod.mesh.properties.emplace_back("unknown","1");break;
            case 5:lod.mesh.triangles[0].materialIndex=1;break;
            case 6:lod.mesh.vertices[0].hasTangentFrame=true;break;
            case 7:model.lodLevels[1].mesh.materials[0].texturePath="other.pac";break;
            case 8:lod.mesh.vertices[0].uv1.u=1;break;
        }
        REQUIRE(InspectRigidOdol7Candidate(model,original.binding,kept)==RigidOdol7Status::Unsupported);
        REQUIRE(kept.mapType==999);
    }
}
TEST_CASE("ODOL7 candidate enforces aggregate visual allocation caps before copying", "[geometry-page-odol7-candidate]")
{
    Source source;source.model.lodLevels[0].mesh.vertices.resize(4097);
    RigidOdol7Candidate kept;kept.mapType=999;
    REQUIRE(InspectRigidOdol7Candidate(source.model,source.binding,kept)==RigidOdol7Status::Capacity);
    REQUIRE(kept.mapType==999);
}
TEST_CASE("Real CWA leased ODOL7 exports bounded rock source candidates", "[.][geometry-page-odol7-candidate][retail]")
{
    // This hidden test requires the actual external archive when explicitly run.
    // It authenticates one LOCAL test-bank mount/member, not the game's active
    // world, finalized Object/config or GPU material/pass authority.
    const char* configured=std::getenv("WGR_TEST_COMPRESSED_MODEL_PBO");
    REQUIRE(configured!=nullptr);
    const std::filesystem::path archive(configured);
    REQUIRE(std::filesystem::is_regular_file(archive));
    Foundation::CaptureMainThread();QFBank bank;auto stem=archive;stem.replace_extension();
    REQUIRE(bank.open(RString(stem.string().c_str())));bank.Lock();
    struct Close {QFBank& bank;~Close(){if(bank.IsLocked())bank.Unlock();bank.close();}} close{bank};
    auto lease=bank.CaptureCompressedReadRequest("skala_new.p3d");REQUIRE(lease);
    REQUIRE(bank.MatchesMountedCompressedMember("skala_new.p3d",*lease));
    REQUIRE(lease->DecodedBytes()==53786);REQUIRE(lease->Encoded().bytes==33097);
    std::vector<char> decoded;std::string hash;REQUIRE(lease->ReadDecoded(decoded,hash));
    REQUIRE(hash=="1048fad6987e65533c17146ab8b9d4ef70cd475e3ed88b2f7c76670504ecb78b");
    REQUIRE(hash==Foundation::Sha256::Of(decoded.data(),decoded.size()));
    const auto model=Asset::Formats::ODOLLoader::loadFromBuffer(decoded.data(),int(decoded.size()),"data3d/skala_new.p3d");
    REQUIRE(Model::HasStaticOdolSourceAudit(model));REQUIRE(model.lodLevels.size()==7);
    RigidOdol7SourceBinding binding;binding.decodedBytes=decoded.size();binding.canonicalPath=model.sourcePath;
    const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
    for(size_t i=0;i<32;++i)binding.decodedSha256[i]=uint8_t(digit(hash[i*2])*16+digit(hash[i*2+1]));
    RigidOdol7Candidate candidate;
    REQUIRE(InspectRigidOdol7Candidate(model,binding,candidate)==RigidOdol7Status::Candidate);
    REQUIRE(bank.MatchesMountedCompressedMember("skala_new.p3d",*lease));
    REQUIRE(candidate.visual.size()==4);REQUIRE(candidate.lodEvidence.size()==7);
    const std::array<size_t,4> triangles{250,144,72,30},vertices{235,155,95,52};
    const std::array<float,4> resolutions{2.5f,5,8.5f,12};
    for(size_t i=0;i<4;++i) {
        const auto& visual=candidate.visual[i];
        REQUIRE(visual.vertices.size()==vertices[i]);REQUIRE(visual.indices.size()==triangles[i]*3);
        REQUIRE(visual.resolution==resolutions[i]);REQUIRE(visual.sourceLod==i);
        REQUIRE(visual.texturePath=="data\\skala_piskovec2.pac");
        REQUIRE(visual.sectionSpecial==0x2000);REQUIRE(visual.lodNoShadow==(i<2));
        REQUIRE(visual.sourceBasis.windingConfidence==Model::BasisConfidence::Unknown);
        REQUIRE(visual.Input().stride==sizeof(RigidOdol7IrVertex));
        for(size_t v=0;v<visual.vertices.size();++v) {
            const auto& actual=model.lodLevels[i].mesh.vertices[v];const auto& raw=visual.vertices[v];
            REQUIRE(raw.position.x==actual.position.x);REQUIRE(raw.position.y==actual.position.y);
            REQUIRE(raw.position.z==actual.position.z);REQUIRE(raw.normal.x==actual.normal.x);
            REQUIRE(raw.normal.y==actual.normal.y);REQUIRE(raw.normal.z==actual.normal.z);
            REQUIRE(raw.u==actual.uv.u);REQUIRE(raw.v==actual.uv.v);
        }
    }
    REQUIRE(candidate.lodEvidence[4].purpose==Model::LodPurpose::Geometry);
    REQUIRE(candidate.lodEvidence[5].purpose==Model::LodPurpose::Roadway);
    REQUIRE(candidate.lodEvidence[6].purpose==Model::LodPurpose::ViewGeometry);
    REQUIRE(model.lodLevels[4].mesh.selections.size()==9);
    REQUIRE(model.geometryIdx==4);REQUIRE(model.geometryFireIdx==6);
    REQUIRE(model.geometryViewIdx==6);REQUIRE(model.roadwayIdx==5);
    REQUIRE(candidate.sourceRoleLods[2]==6);REQUIRE(candidate.sourceRoleLods[9]==5);
}
TEST_CASE("Real CWA Skala2 retains only unreferenced zero normals", "[.][geometry-page-odol7-candidate][retail]")
{
    const char* configured=std::getenv("WGR_TEST_COMPRESSED_MODEL_PBO");
    REQUIRE(configured!=nullptr);
    const std::filesystem::path archive(configured);
    REQUIRE(std::filesystem::is_regular_file(archive));
    Foundation::CaptureMainThread();QFBank bank;auto stem=archive;stem.replace_extension();
    REQUIRE(bank.open(RString(stem.string().c_str())));bank.Lock();
    struct Close {QFBank& bank;~Close(){if(bank.IsLocked())bank.Unlock();bank.close();}} close{bank};
    auto lease=bank.CaptureCompressedReadRequest("skala2.p3d");REQUIRE(lease);
    REQUIRE(bank.MatchesMountedCompressedMember("skala2.p3d",*lease));
    REQUIRE(lease->DecodedBytes()==24442);
    std::vector<char> decoded;std::string hash;REQUIRE(lease->ReadDecoded(decoded,hash));
    REQUIRE(hash=="b92d0490860e98ffde6dc2969a248d19512f2db3de332793750cb4f6fcb2dad2");
    REQUIRE(hash==Foundation::Sha256::Of(decoded.data(),decoded.size()));
    std::string error;
    auto model=ModelCache::LoadOwnedBytes(decoded.data(),decoded.size(),"data3d\\skala2.p3d",error);
    REQUIRE(model);REQUIRE(Model::HasStaticOdolSourceAudit(*model));
    REQUIRE(model->lodLevels.size()==6);
    RigidOdol7SourceBinding binding;binding.decodedBytes=decoded.size();binding.canonicalPath=model->sourcePath;
    const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
    for(size_t i=0;i<32;++i)binding.decodedSha256[i]=uint8_t(digit(hash[i*2])*16+digit(hash[i*2+1]));
    RigidOdol7Candidate candidate;
    REQUIRE(InspectRigidOdol7Candidate(*model,binding,candidate)==RigidOdol7Status::Candidate);
    REQUIRE(candidate.visual.size()==3);
    const std::array<size_t,3> triangles{77,55,33},vertices{95,99,65};
    const std::array<std::vector<size_t>,3> unusedZero{{{2,8,54},{2,52,63},{2,41}}};
    for(size_t lod=0;lod<3;++lod) {
        const auto& visual=candidate.visual[lod];
        REQUIRE(visual.sourceLod==lod);
        REQUIRE(visual.vertices.size()==vertices[lod]);
        REQUIRE(visual.indices.size()==triangles[lod]*3);
        REQUIRE(visual.texturePath=="data\\piskovec.pac");
        std::vector<bool> referenced(vertices[lod]);
        for(auto index:visual.indices){REQUIRE(index<referenced.size());referenced[index]=true;}
        std::vector<size_t> zero;
        for(size_t vertex=0;vertex<visual.vertices.size();++vertex) {
            const auto& n=visual.vertices[vertex].normal;
            if(n.x==0 && n.y==0 && n.z==0) {
                REQUIRE_FALSE(referenced[vertex]);zero.push_back(vertex);
            }
        }
        REQUIRE(zero==unusedZero[lod]);
    }
    REQUIRE(bank.MatchesMountedCompressedMember("skala2.p3d",*lease));
}
