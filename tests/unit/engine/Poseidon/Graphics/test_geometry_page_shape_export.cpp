#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageShapeExport.hpp>
#include <limits>
#include <thread>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
Shape* Triangle(bool quad=false)
{
    auto* s=new Shape;
    s->AddVertexFast(Vector3(0,0,0),V3Up,0,0,0);
    s->AddVertexFast(Vector3(1,0,0),V3Up,0,1,0);
    s->AddVertexFast(Vector3(1,1,0),V3Up,0,1,1);
    if (quad) s->AddVertexFast(Vector3(0,1,0),V3Up,0,0,1);
    Poly p; p.Init(); p.SetN(quad?4:3);
    for (int i=0; i<p.N(); ++i) p.Set(i,i);
    s->AddFace(p);
    ShapeSection section; section.properties.Init(); section.material=0;
    section.beg=s->BeginFaces(); section.end=s->EndFaces(); s->AddSection(section);
    return s;
}
ShapeExportSelection PilotSelection()
{
    ShapeExportSelection s; s.controlledAuthoredRigid=true; s.coarseLevel=0; s.fineLevel=1;
    s.source.sourceSha256[0]=1; s.source.vertexLayout=1; s.source.materialMapping=1;
    s.source.coarseRepresentation=0; s.source.fineRepresentation=1; return s;
}
}
TEST_CASE("Geometry page export uses actual engine packing and transactional refusal", "[geometry-page-export]")
{
    Foundation::CaptureMainThread();
    LODShape shape; shape.AddShape(Triangle(),10); shape.AddShape(Triangle(true),1);
    auto selected=PilotSelection(); ShapeExport dto;
    REQUIRE(ExportShapePair(shape,selected,dto)==ExportStatus::Exported);
    REQUIRE(dto.coarse.indices==std::vector<uint32_t>{0,1,2});
    REQUIRE(dto.fine.indices==std::vector<uint32_t>{0,1,2,0,2,3});
    REQUIRE(dto.fine.materials==std::vector<uint32_t>{0,0});
    std::vector<SVertex> expected(4); render::mesh::BuildVertices(*shape.Level(1),expected.data());
    REQUIRE(std::memcmp(expected.data(),dto.fine.vertices.data(),expected.size()*sizeof(SVertex))==0);
    for (size_t i=0; i<expected.size(); ++i)
    {
        REQUIRE(dto.fine.positions[i].x==expected[i].pos.X());
        REQUIRE(dto.fine.positions[i].y==expected[i].pos.Y());
        REQUIRE(dto.fine.positions[i].z==expected[i].pos.Z());
    }
    Package package; REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package)==Status::Built);
    SECTION("bad index") { shape.Level(1)->Face(shape.Level(1)->BeginFaces()).Set(0,99); }
    SECTION("nonfinite position") { shape.Level(1)->SetPos(0)=Vector3(std::numeric_limits<float>::quiet_NaN(),0,0); }
    SECTION("section gap") { shape.Level(1)->GetSection(0).end=shape.Level(1)->BeginFaces(); }
    REQUIRE(ExportShapePair(shape,selected,dto)==ExportStatus::Invalid);
    REQUIRE(dto.fine.indices==std::vector<uint32_t>{0,1,2,0,2,3});
}
TEST_CASE("Geometry page export refuses unsupported sources without loading", "[geometry-page-export]")
{
    Foundation::CaptureMainThread();
    LODShape shape; shape.AddShape(Triangle(),10); shape.AddShape(Triangle(),1);
    auto selected=PilotSelection(); ShapeExport dto;
    SECTION("caller certificate absent") { selected.controlledAuthoredRigid=false; }
    SECTION("animation") { shape.AllowAnimation(); }
    SECTION("conformance") { shape.Level(1)->SetClip(0,ClipLandKeep); }
    SECTION("special section") { shape.Level(1)->GetSection(0).properties.SetSpecial(1); }
    REQUIRE(ExportShapePair(shape,selected,dto)==ExportStatus::Unsupported);
    REQUIRE(dto.fine.vertices.empty());
}
TEST_CASE("Geometry page export refuses a worker before reading Shape", "[geometry-page-export]")
{
    Foundation::CaptureMainThread(); LODShape shape; ShapeExport dto;
    auto status=ExportStatus::Exported;
    std::thread worker([&] { status=ExportShapePair(shape,PilotSelection(),dto); }); worker.join();
    REQUIRE(status==ExportStatus::WrongOwner); REQUIRE(dto.coarse.vertices.empty());
}
