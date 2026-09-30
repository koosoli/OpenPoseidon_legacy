#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRigidOdol7FinalShape.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Graphics/Dummy/TextureDummy.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
struct Wire
{
    std::vector<char> bytes;
    void U(uint32_t value,unsigned n=4){for(unsigned i=0;i<n;++i)bytes.push_back(char(value>>(8*i)));}
    void F(float value){U(std::bit_cast<uint32_t>(value));}
    void Z(const char* value){while(*value)bytes.push_back(*value++);bytes.push_back(0);}
    Wire()
    {
        for(char c:std::string("ODOL"))bytes.push_back(c);U(7);U(4);
        for(unsigned lod=0;lod<4;++lod) {
            U(4);for(unsigned v=0;v<4;++v)U(63);
            U(4);for(unsigned v=0;v<4;++v){F(float(v&1));F(float(v>>1));}
            U(4);for(auto p:std::array<Position,4>{{{0,0,0},{1,0,0},{1,.25f,1},{0,0,1}}}){F(p.x);F(p.y);F(p.z);}
            U(4);for(unsigned v=0;v<4;++v){F(0);F(1);F(0);}
            for(unsigned i=0;i<12;++i)U(0);
            U(1);Z("data\\skala_piskovec2.pac");U(0);U(0);
            U(1);U(0);U(0x2000);U(0,2);U(4,1);for(unsigned v=0;v<4;++v)U(v,2);
            U(1);U(0);U(18);U(0);U(0,2);U(0x2000);U(0);
            if(lod<2){U(1);Z("lodnoshadow");Z("1");}
            else if(lod==3){U(2);Z("map");Z("rock");Z("dammage");Z("no");}
            else U(0);
            U(0);U(0);U(0);U(0);U(0);
        }
        F(2.5f);F(5);F(12);F(1e13f);
        for(unsigned i=0;i<36;++i)U(0);
        for(unsigned i=0;i<5;++i)U(0,1);U(11,1);
        U(0);for(unsigned i=0;i<4;++i)F(0);
        for(unsigned i=0;i<12;++i)U(i==1?3:255,1);
    }
};
struct Fixture
{
    Wire wire;
    Model::Model model;
    RigidOdol7Candidate candidate;
    Ref<Texture> texture;
    std::unique_ptr<LODShapeWithShadow> shape;
    std::filesystem::path directory;
    QFBank bank;
    RigidOdol7FinalSourceHold source;
    Fixture()
    {
        Foundation::CaptureMainThread();
        model=Asset::Formats::ODOLLoader::loadFromBuffer(wire.bytes.data(),int(wire.bytes.size()),"data3d\\skala_new.p3d");
        RigidOdol7SourceBinding binding;binding.canonicalPath=model.sourcePath;binding.decodedBytes=wire.bytes.size();
        const auto hash=Foundation::Sha256::Of(wire.bytes.data(),wire.bytes.size());
        const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
        for(size_t i=0;i<32;++i)binding.decodedSha256[i]=uint8_t(digit(hash[i*2])*16+digit(hash[i*2+1]));
        REQUIRE(InspectRigidOdol7Candidate(model,binding,candidate)==RigidOdol7Status::Candidate);
        texture=new TextureDummy();texture->SetName("data\\skala_piskovec2.pac");
        Model::ShapeAdapter::AdapterBankTables tables;
        tables.textures.resize(4);tables.surfMats.resize(4);
        for(unsigned i=0;i<4;++i){tables.textures[i]={texture};tables.surfMats[i].resize(1);}
        {
            // Real adapter final tail in offline tool context; no installed game,
            // global texture/config lookup or GPU resolver is invoked.
            struct Restore {decltype(GApp) old=GApp;~Restore(){GApp=old;}} restore;
            GApp=nullptr;shape.reset(Model::ShapeAdapter::convertToLODShape(model,false,&tables));
        }
        REQUIRE(shape);REQUIRE(shape->GetMapType()==MapRock);
        directory=std::filesystem::temp_directory_path()/(
            "odol7-final-shape-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(std::filesystem::create_directory(directory));
        QOStream encoded;SSCompress codec;codec.Encode(encoded,wire.bytes.data(),long(wire.bytes.size()));
        {
            std::ofstream out(directory/"models.pbo",std::ios::binary);REQUIRE(out.good());
            const char member[]="skala_new.p3d";out.write(member,sizeof(member));
            for(int field:{CompMagic,int(wire.bytes.size()),0,0,encoded.pcount()})out.write(reinterpret_cast<const char*>(&field),4);
            const char zero=0;out.write(&zero,1);const int field=0;
            for(unsigned i=0;i<5;++i)out.write(reinterpret_cast<const char*>(&field),4);
            out.write(encoded.str(),encoded.pcount());REQUIRE(out.good());
        }
        REQUIRE(bank.open(RString((directory/"models").string().c_str())));bank.Lock();
#ifdef _WIN32
        auto captured=bank.CaptureCompressedReadRequest("skala_new.p3d");REQUIRE(captured);
        std::vector<char> decoded;std::string decodedHash;REQUIRE(captured->ReadDecoded(decoded,decodedHash));
        REQUIRE(decoded==wire.bytes);REQUIRE(decodedHash==hash);
        source.lease=std::make_shared<const BankCompressedReadRequest>(*captured);
#endif
        source.bank=&bank;source.decodedSha256=binding.decodedSha256;
        source.sourceAdmissionEpoch=1;source.ownerEpoch=2;source.modelBirth=3;
    }
    ~Fixture()
    {
        source.lease.reset();if(bank.IsLocked())bank.Unlock();bank.close();
        std::error_code error;std::filesystem::remove(directory/"models.pbo",error);
        error.clear();std::filesystem::remove(directory,error);
    }
    RigidOdol7FinalMaterialHold Inspect(int level,const ShapeSection& section) const
    {
        RigidOdol7FinalMaterialHold hold;hold.texture=texture;hold.surface=section.surfMat;
        auto& facts=hold.facts;facts.complete=true;facts.capturedGroups=render::PreparedGpuSectionFacts::All;
        facts.gpuOwned=SectionGpuOwned(section);facts.indexCount=render::mesh::CountIndices(*shape->Level(level));
        facts.texturePresent=true;facts.alphaClass=AlphaStats::Opaque;facts.materialFullyOpaque=true;
        facts.descriptor=render::BuildRenderPassDescriptor(render::SplitLegacy(section.properties.Special()));
        // Deliberately injected material inspector facts: these unit tests validate
        // the geometry/helper contract, not real TextureWgpu Init/opaque-PAC or
        // renderer-slot authority. Runtime collector must supply real provenance.
        hold.current.textureToken=7;hold.current.sourceName=texture->Name();
        hold.current.member={1,1000,40,10,{}};hold.current.handle=8;hold.current.slotLease=9;
        hold.current.birthLeaseHeld=true;hold.current.mountedCurrent=true;hold.materialBindingBirth=10;
        return hold;
    }
};
}
TEST_CASE("Final ODOL7 Shape capture matches actual MeshBuild and retains original shadow coverage", "[geometry-page-odol7-final]")
{
    Fixture fixture;RigidOdol7FinalExport result;
    auto inspect=[&](int level,const ShapeSection& section){return fixture.Inspect(level,section);};
#ifdef _WIN32
    REQUIRE(ExportRigidOdol7FinalShape(*fixture.shape,fixture.candidate,1,0,fixture.source,inspect,result)==RigidOdol7FinalStatus::Exported);
    REQUIRE(result.actualNoShadow);REQUIRE(result.requiresOriginalOtherPasses);
    REQUIRE_FALSE(fixture.shape->Level(2)->HasNoShadowProperty()); // conventional shadow12m survives
    REQUIRE(result.finalLevels==std::array<int,2>{1,0});REQUIRE(result.sourceLods==std::array<uint32_t,2>{1,0});
    REQUIRE(result.sourceMember.bytes==fixture.source.lease->Encoded().bytes);
    REQUIRE(result.material.slotLease==9);REQUIRE(result.material.materialBindingBirth==10);
    std::vector<SVertex> original(fixture.shape->Level(0)->NVertex());
    render::mesh::BuildVertices(*fixture.shape->Level(0),original.data());
    REQUIRE(std::memcmp(original.data(),result.actual.fine.vertices.data(),original.size()*sizeof(SVertex))==0);
    REQUIRE(result.actual.fine.vertices[0].norm.Y()==-1);
    REQUIRE(result.actual.fine.vertices[2].t0.u==fixture.candidate.visual[0].vertices[2].u);
#else
    REQUIRE(ExportRigidOdol7FinalShape(*fixture.shape,fixture.candidate,1,0,fixture.source,inspect,result)==RigidOdol7FinalStatus::StaleSource);
#endif
}
TEST_CASE("Final ODOL7 strict geometry matcher refuses deform recenter UV and topology changes", "[geometry-page-odol7-final]")
{
    for(unsigned fault=0;fault<5;++fault) {
        Fixture fixture;auto& actual=*fixture.shape->Level(0);ExportedMesh kept;kept.materials={99};
        switch(fault) {
            case 0:actual.SetPos(0)[0]+=.1f;break;
            case 1:actual.SetNorm(0)[1]=.5f;break;
            case 2:actual.SetU(0,.25f);break;
            case 3:{auto offset=actual.BeginFaces();actual.Face(offset).Set(0,1);break;}
            case 4:actual.SetClip(0,ClipLandKeep);break;
        }
        REQUIRE(RigidOdol7FinalDetail::Geometry(actual,fixture.candidate.visual[0],kept)==RigidOdol7FinalStatus::Unsupported);
        REQUIRE(kept.materials==std::vector<uint32_t>{99});
    }
}
TEST_CASE("Final ODOL7 export refuses changed source or material incarnation transactionally", "[geometry-page-odol7-final]")
{
#ifdef _WIN32
    for(unsigned fault=0;fault<10;++fault) {
        Fixture fixture;RigidOdol7FinalExport kept;kept.modelBirth=999;unsigned inspections=0;
        if(fault==0)fixture.source.decodedSha256[0]^=1;
        if(fault==1)fixture.source.modelBirth=0;
        auto inspect=[&](int level,const ShapeSection& section) {
            auto result=fixture.Inspect(level,section);++inspections;
            if(fault==2)result.facts.capturedGroups=0;
            if(fault==3)result.current.mountedCurrent=false;
            if(fault==4 && inspections>=3)++result.materialBindingBirth;
            if(fault==5)result.facts.alphaClass=AlphaStats::Blend;
            if(fault==6)result.current.slotLease=0;
            if(fault==7){result.texture=new TextureDummy();result.texture->SetName("data\\skala_piskovec2.pac");}
            if(fault==8)result.facts.foldedEmissive[0]=std::numeric_limits<float>::quiet_NaN();
            if(fault==9)result.facts.descriptor.blend=render::BlendMode::AlphaBlend;
            return result;
        };
        const auto status=ExportRigidOdol7FinalShape(*fixture.shape,fixture.candidate,1,0,fixture.source,inspect,kept);
        REQUIRE(status==(fault<2?RigidOdol7FinalStatus::StaleSource:RigidOdol7FinalStatus::StaleMaterial));
        REQUIRE(kept.modelBirth==999);REQUIRE(kept.actual.fine.vertices.empty());
        if(fault<2)REQUIRE(inspections==0);
    }
#endif
}
TEST_CASE("Final ODOL7 export refuses incompatible shadow levels and departed query revision", "[geometry-page-odol7-final]")
{
#ifdef _WIN32
    Fixture fixture;RigidOdol7FinalExport kept;kept.modelBirth=999;
    auto inspect=[&](int level,const ShapeSection& section){return fixture.Inspect(level,section);};
    REQUIRE(ExportRigidOdol7FinalShape(*fixture.shape,fixture.candidate,2,0,fixture.source,inspect,kept)==RigidOdol7FinalStatus::Unsupported);
    unsigned calls=0;
    auto changed=[&](int level,const ShapeSection& section) {
        auto result=fixture.Inspect(level,section);
        if(++calls==3)fixture.shape->AllowAnimation();
        return result;
    };
    REQUIRE(ExportRigidOdol7FinalShape(*fixture.shape,fixture.candidate,1,0,fixture.source,changed,kept)==RigidOdol7FinalStatus::StaleSource);
    REQUIRE(kept.modelBirth==999);
#endif
}
