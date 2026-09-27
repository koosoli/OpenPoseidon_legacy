#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageControlledMlodAsset.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Core/Application.hpp>
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <iterator>

using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
struct OriginalFile
{
    std::vector<uint8_t> bytes;
    void U32(uint32_t v) {for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(v>>(i*8)));}
    void Float(float v) {U32(std::bit_cast<uint32_t>(v));}
    void Text(const char* s) {while(*s)bytes.push_back(uint8_t(*s++));}
    void Z(const char* s) {Text(s);bytes.push_back(0);}
    void Fixed(const char* s) {const size_t at=bytes.size();Z(s);bytes.resize(at+64,0);}
    void Tag(const char* s,uint32_t n) {bytes.push_back(1);Z(s);U32(n);}
    OriginalFile(bool quads=true,const char* texture="",const char* extra=nullptr)
    {
        Text("MLOD");U32(0x101);U32(2);
        for(unsigned lod=0;lod<2;++lod) {
            Text("P3DM");U32(28);U32(256);U32(4);U32(1);U32(1);U32(0);
            for(auto point:std::array<std::array<float,3>,4>{{{4,2,5},{5,2,5},{5,2,6},{4,2,6}}})
                {for(float f:point)Float(f);U32(0);}
            Float(0);Float(1);Float(0);
            const unsigned n=lod==1&&quads?4:3;U32(n);
            for(unsigned v=0;v<4;++v) {U32(v<n?v:0);U32(0);Float(v<n?float(v&1):0);Float(v<n?float(v/2):0);}
            U32(0);Z(texture);Z("");Text("TAGG");
            Tag("#Property#",128);Fixed("autocenter");Fixed("0");
            if(extra)Tag(extra,0);
            Tag("#EndOfFile#",0);Float(lod==0?10:1);
        }
    }
};
Model::Model Parsed(const std::vector<uint8_t>& bytes)
{
    auto model=Asset::Formats::MLODLoader::loadFromBuffer(reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),
        "offline-neutral-mesh-packing.p3d");
    REQUIRE(model.compile());return model;
}
void EqualMesh(const ExportedMesh& a,const ExportedMesh& b)
{
    REQUIRE(a.indices==b.indices);REQUIRE(a.materials==b.materials);
    REQUIRE(a.vertices.size()==b.vertices.size());
    REQUIRE(std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(SVertex))==0);
}
}
TEST_CASE("Original owned MLOD export retains exact existing MeshBuild packing", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();
    // The test runner installs TestApplication. Reproduce the ordinary offline
    // tool context only for the legacy comparison, restoring it even on failure.
    // Production controlled export must work with the live test application.
    REQUIRE((Pars >> "CfgModels").FindEntry("offline_neutral_mesh_packing")==nullptr);
    for(bool quads:{false,true}) {
        OriginalFile source(quads);ControlledMlodAsset result;
        REQUIRE(ExportControlledMlodOwned(source.bytes,result)==ControlledMlodStatus::Exported);
        REQUIRE(result.originalSourceBytes==source.bytes.size());
        REQUIRE(result.sourceResolutions==std::array<float,2>{10,1});
        REQUIRE(result.original.fine.indices.size()==(quads?6:3));
        REQUIRE(result.original.coarse.indices.size()==3);
        auto model=Parsed(source.bytes);Model::ShapeAdapter::AdapterBankTables empty;
        std::unique_ptr<LODShapeWithShadow> legacy;
        {
            struct RestoreApplication {
                decltype(GApp) saved=GApp;
                ~RestoreApplication() { GApp=saved; }
            } restore;
            GApp=nullptr;
            legacy.reset(Model::ShapeAdapter::convertToLODShape(model,false,&empty));
        }
        ShapeExportSelection select;select.controlledAuthoredRigid=true;select.coarseLevel=0;select.fineLevel=1;
        select.source=result.original.source;ShapeExport baseline;
        REQUIRE(ExportShapePair(*legacy,select,baseline)==ExportStatus::Exported);
        EqualMesh(result.original.coarse,baseline.coarse);EqualMesh(result.original.fine,baseline.fine);
        // autocenter=0 leaves the original authored offset, rather than recentering.
        REQUIRE(result.original.fine.positions[0].x>=4);
        Foundation::Sha256 hash;hash.Update(source.bytes.data(),source.bytes.size());
        const auto hex=hash.Hex();const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
        for(size_t i=0;i<32;++i)REQUIRE(result.original.source.sourceSha256[i]==digit(hex[i*2])*16+digit(hex[i*2+1]));
    }
}
TEST_CASE("Original source mutation changes whole-byte identity without synthetic key", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile a;auto b=a.bytes;
    // First raw source position x; same byte length, same filename/domain context.
    const uint32_t changed=std::bit_cast<uint32_t>(4.5f);
    for(unsigned i=0;i<4;++i)b[40+i]=uint8_t(changed>>(i*8));
    ControlledMlodAsset first,second;
    REQUIRE(ExportControlledMlodOwned(a.bytes,first)==ControlledMlodStatus::Exported);
    REQUIRE(ExportControlledMlodOwned(b,second)==ControlledMlodStatus::Exported);
    REQUIRE(first.original.source.sourceSha256!=second.original.source.sourceSha256);
    REQUIRE(first.originalSourceBytes==second.originalSourceBytes);
    REQUIRE(std::memcmp(first.original.coarse.vertices.data(),second.original.coarse.vertices.data(),
        first.original.coarse.vertices.size()*sizeof(SVertex))!=0);
}
TEST_CASE("Raw unsupported MLOD data is refused before parser loss transactionally", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile source;ControlledMlodAsset kept;
    REQUIRE(ExportControlledMlodOwned(source.bytes,kept)==ControlledMlodStatus::Exported);
    const auto oldHash=kept.original.source.sourceSha256;auto bytes=source.bytes;
    SECTION("unknown tag even though original loader skips it") {
        bytes=OriginalFile(true,"","#UnknownPolicy#").bytes;
        REQUIRE(Parsed(bytes).lodLevels[0].mesh.properties.size()==1);
    }
    SECTION("named selection even when zero sized") {bytes=OriginalFile(true,"","proxy:hidden").bytes;}
    SECTION("material dependency") {bytes=OriginalFile(true,"texture.paa").bytes;}
    SECTION("tail") {bytes.push_back(0);}
    SECTION("truncated") {bytes.pop_back();}
    SECTION("point flag masked by original loader") {bytes[52]=0x80;}
    SECTION("face invalid source index") {bytes[120]=0xff;}
    SECTION("nonfinite position") {const uint32_t n=0x7fc00000;for(unsigned i=0;i<4;++i)bytes[40+i]=uint8_t(n>>(i*8));}
    SECTION("count cap before table allocation") {bytes[24]=0xff;bytes[25]=0xff;}
    SECTION("oversize input") {bytes.resize(16*1024*1024+1);}
    const auto status=ExportControlledMlodOwned(std::move(bytes),kept);
    REQUIRE(status!=ControlledMlodStatus::Exported);REQUIRE(kept.original.source.sourceSha256==oldHash);
    REQUIRE(kept.original.fine.indices.size()==6);
}
TEST_CASE("Controlled conversion independently rejects unsupported mutated IR", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile source;auto model=Parsed(source.bytes);
    SECTION("material") {model.lodLevels[0].mesh.materials[0].texturePath="unsupported.paa";}
    SECTION("proxy") {model.lodLevels[0].mesh.proxies.emplace_back("proxy:unknown");}
    SECTION("animation") {model.allowAnimation=1;}
    SECTION("property") {model.lodLevels[0].mesh.properties.emplace_back("class","house");}
    SECTION("large original face index") {model.lodLevels[0].mesh.triangles[0].originalIndex=UINT32_MAX;}
    SECTION("conformance") {model.lodLevels[0].mesh.vertices[0].flags=Model::VertexFlags(ClipLandOn);}
    SECTION("path with embedded nul") {model.sourcePath=std::string("neutral\0tree",12);}
    REQUIRE(Model::ShapeAdapter::ConvertControlledMlod(model)==nullptr);
    // The friend implementation is forward-declared in Shape.hpp. Calling it
    // directly must not bypass the controlled IR refusal before any allocation.
    const Model::ShapeAdapter::AdapterBankTables emptyBanks;
    REQUIRE(Model::ShapeAdapter::ConvertWithContext(model,false,&emptyBanks,true,true)==nullptr);
}
TEST_CASE("Controlled source export refuses worker before source walk", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile source;ControlledMlodAsset result;
    auto status=ControlledMlodStatus::Exported;
    std::thread worker([&]{status=ExportControlledMlodOwned(std::move(source.bytes),result);});worker.join();
    REQUIRE(status==ControlledMlodStatus::WrongOwner);REQUIRE(result.original.fine.vertices.empty());
}

TEST_CASE("File-backed original MLOD snapshot exports its actual complete file hash", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile original;
    const auto file=std::filesystem::temp_directory_path()/
        ("poseidon-controlled-mlod-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".p3d");
    // Only this authored file is removed, never recursive/shared directory cleanup.
    struct Remove {std::filesystem::path path;~Remove(){std::error_code e;std::filesystem::remove(path,e);}} cleanup{file};
    {
        std::ofstream out(file,std::ios::binary);REQUIRE(out.good());
        out.write(reinterpret_cast<const char*>(original.bytes.data()),std::streamsize(original.bytes.size()));
        REQUIRE(out.good());
    }
    std::ifstream in(file,std::ios::binary);
    REQUIRE(in.good());std::vector<uint8_t> owned{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    REQUIRE(owned==original.bytes);ControlledMlodAsset exported;
    REQUIRE(ExportControlledMlodOwned(std::move(owned),exported)==ControlledMlodStatus::Exported);
    REQUIRE(exported.originalSourceBytes==original.bytes.size());
    ControlledMlodAsset memory;
    REQUIRE(ExportControlledMlodOwned(original.bytes,memory)==ControlledMlodStatus::Exported);
    REQUIRE(exported.original.source.sourceSha256==memory.original.source.sourceSha256);
    EqualMesh(exported.original.coarse,memory.original.coarse);EqualMesh(exported.original.fine,memory.original.fine);
}

TEST_CASE("Controlled friend implementation refuses a non-isolated caller context", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile source;auto model=Parsed(source.bytes);
    Model::ShapeAdapter::AdapterBankTables empty;
    SECTION("reversed") {REQUIRE(Model::ShapeAdapter::ConvertWithContext(model,true,&empty,true,true)==nullptr);}
    SECTION("incomplete") {REQUIRE(Model::ShapeAdapter::ConvertWithContext(model,false,&empty,false,true)==nullptr);}
    SECTION("ordinary bank path") {REQUIRE(Model::ShapeAdapter::ConvertWithContext(model,false,nullptr,true,true)==nullptr);}
    SECTION("injected bank row") {
        empty.textures.resize(1);
        REQUIRE(Model::ShapeAdapter::ConvertWithContext(model,false,&empty,true,true)==nullptr);
    }
}

TEST_CASE("Original MLOD DTO copies actual neutral material while source Shape is alive", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile raw;ControlledMlodAsset exported;
    REQUIRE(ExportControlledMlodOwned(raw.bytes,exported)==ControlledMlodStatus::Exported);
    REQUIRE(exported.coarseMaterial==exported.fineMaterial);
    const auto& material=exported.fineMaterial;
    REQUIRE(material.materialType==0);REQUIRE(material.specFlags==0);REQUIRE(material.specularPower==0);
    REQUIRE(material.ambient==std::array<float,4>{1,1,1,1});
    REQUIRE(material.diffuse==std::array<float,4>{1,1,1,1});
    REQUIRE(material.emissive==std::array<float,4>{0,0,0,1});
    REQUIRE(material.specular==std::array<float,4>{0,0,0,1});
    REQUIRE(material.forcedDiffuse==std::array<float,4>{0,0,0,1});
    auto model=Parsed(raw.bytes);std::unique_ptr<LODShapeWithShadow> shape(Model::ShapeAdapter::ConvertControlledMlod(model));
    REQUIRE(shape!=nullptr);const auto& actualSection=shape->Level(1)->GetSection(0);
    TLMaterial actual;CreateMaterial(actual,HWhite,actualSection.material);
    REQUIRE(material.ambient[0]==actual.ambient.R());REQUIRE(material.emissive[3]==actual.emmisive.A());
    REQUIRE(material.specular[3]==actual.specular.A());
    ControlledMlodPlainMaterial kept;kept.ambient[0]=37;
    SECTION("unsupported shading type") {shape->Level(1)->GetSection(0).material=MSShining;}
    SECTION("special policy") {shape->Level(1)->GetSection(0).properties.SetSpecial(NoShadow);}
    REQUIRE(ControlledMlodAssetDetail::CapturePlainMaterial(shape->Level(1)->GetSection(0),kept)==ControlledMlodStatus::Unsupported);
    REQUIRE(kept.ambient[0]==37);
}
TEST_CASE("Original MLOD radius encloses actual offset geometry without centering", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile raw;ControlledMlodAsset exported;
    REQUIRE(ExportControlledMlodOwned(raw.bytes,exported)==ControlledMlodStatus::Exported);
    REQUIRE(std::isfinite(exported.originRadius));REQUIRE(exported.originRadius>5);
    double maximum=0;
    for(const auto* mesh:{&exported.original.coarse,&exported.original.fine})for(const auto& vertex:mesh->vertices) {
        const long double x=vertex.pos.X(),y=vertex.pos.Y(),z=vertex.pos.Z();
        const long double distance=x*x+y*y+z*z;
        REQUIRE(static_cast<long double>(exported.originRadius)*exported.originRadius>=distance);
        maximum=std::max(maximum,double(distance));
    }
    REQUIRE(exported.originRadius==std::nextafter(float(std::sqrt(maximum)),std::numeric_limits<float>::infinity()));
    // Put the farthest point in the authored fallback only: union must not bound
    // merely the fine stream. This value helper does not mint source identity.
    auto source=exported.original;
    source.coarse.vertices[0].pos=Vector3P(10000,10000,10000);source.coarse.positions[0]={10000,10000,10000};
    float radius=0;REQUIRE(ControlledMlodAssetDetail::OriginRadius(source,radius)==ControlledMlodStatus::Eligible);
    REQUIRE(static_cast<long double>(radius)*radius>=300000000.L);
    REQUIRE(source.coarse.vertices[0].pos.X()==10000);
}
TEST_CASE("Copied radius refusal is bounded and transactional", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile raw;ControlledMlodAsset exported;
    REQUIRE(ExportControlledMlodOwned(raw.bytes,exported)==ControlledMlodStatus::Exported);
    auto source=exported.original;float kept=73;auto expected=ControlledMlodStatus::Invalid;
    SECTION("position table mismatch") {source.coarse.positions[0].x+=1;}
    SECTION("nonfinite") {source.fine.vertices[0].pos=Vector3P(std::numeric_limits<float>::quiet_NaN(),0,0);}
    SECTION("stream cap") {source.coarse.vertices.resize(4097);source.coarse.positions.resize(4097);expected=ControlledMlodStatus::Capacity;}
    SECTION("outside original supported domain") {
        source.coarse.vertices[0].pos=Vector3P(10001,0,0);source.coarse.positions[0]={10001,0,0};expected=ControlledMlodStatus::Unsupported;
    }
    SECTION("empty") {source.fine.vertices.clear();source.fine.positions.clear();}
    REQUIRE(ControlledMlodAssetDetail::OriginRadius(source,kept)==expected);REQUIRE(kept==73);
}

TEST_CASE("Origin sphere rounds outward at exact representable and zero radii", "[controlled-mlod-asset]")
{
    Foundation::CaptureMainThread();OriginalFile raw;ControlledMlodAsset exported;
    REQUIRE(ExportControlledMlodOwned(raw.bytes,exported)==ControlledMlodStatus::Exported);
    for(float distance:{0.f,5.f}) {
        auto source=exported.original;
        for(auto* mesh:{&source.coarse,&source.fine})for(size_t i=0;i<mesh->vertices.size();++i) {
            mesh->vertices[i].pos=Vector3P(distance,0,0);mesh->positions[i]={distance,0,0};
        }
        float radius=0;
        REQUIRE(ControlledMlodAssetDetail::OriginRadius(source,radius)==ControlledMlodStatus::Eligible);
        REQUIRE(radius==std::nextafter(distance,std::numeric_limits<float>::infinity()));
        REQUIRE(radius>distance);
    }
}
