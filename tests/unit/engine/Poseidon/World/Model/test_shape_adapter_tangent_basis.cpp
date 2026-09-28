#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>

#include <memory>

using namespace Poseidon::Model;

TEST_CASE("ShapeAdapter derives tangent basis from UV0", "[ShapeAdapter][tangent]")
{
    Model model;
    model.sourceFormat = "ODOL";
    LODLevel lodLevel(1.0f);
    Mesh& mesh = lodLevel.mesh;
    mesh.vertices = {
        Vertex({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f}),
        Vertex({1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f}),
        Vertex({1.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f}),
        Vertex({0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f}),
    };
    // Deliberately wrong source values: this test pins that rendering does not
    // rely on an unverified packed ODOL S/T interpretation.
    for (Vertex& vertex : mesh.vertices)
    {
        vertex.tangent = {0.f, 0.f, 1.f};
        vertex.binormal = {1.f, 0.f, 0.f};
        vertex.hasTangentFrame = true;
    }
    mesh.quads.emplace_back(0, 1, 2, 3);
    model.lodLevels.push_back(std::move(lodLevel));

    std::unique_ptr<LODShapeWithShadow> shape(ShapeAdapter::convertToLODShape(model, false));
    REQUIRE(shape);
    Shape* lod = shape->Level(0);
    REQUIRE(lod->HasTangentFrame());

    for (int vertex = 0; vertex < lod->NVertex(); ++vertex)
    {
        REQUIRE(lod->Tangent(vertex)[0] > 0.f);
        REQUIRE(lod->Tangent(vertex)[1] == Catch::Approx(0.f));
        REQUIRE(lod->Tangent(vertex)[2] == Catch::Approx(0.f));
        REQUIRE(lod->Binormal(vertex)[0] == Catch::Approx(0.f));
        REQUIRE(lod->Binormal(vertex)[1] > 0.f);
        REQUIRE(lod->Binormal(vertex)[2] == Catch::Approx(0.f));
    }
}

TEST_CASE("Reversed MLOD rotates geometry and its tangent frame exactly once", "[ShapeAdapter][mlod][reversed]")
{
    Model model;
    model.sourceFormat = "MLOD";
    LODLevel lodLevel(1.0f);
    Mesh& mesh = lodLevel.mesh;
    mesh.vertices = {
        Vertex({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f}),
        Vertex({2.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f}),
        Vertex({2.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f}),
        Vertex({0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f}),
    };
    mesh.quads.emplace_back(0, 1, 2, 3);
    model.lodLevels.push_back(std::move(lodLevel));

    std::unique_ptr<LODShapeWithShadow> normal(ShapeAdapter::convertToLODShape(model, false));
    std::unique_ptr<LODShapeWithShadow> reversed(ShapeAdapter::convertToLODShape(model, true));
    REQUIRE(normal);
    REQUIRE(reversed);
    REQUIRE((normal->Remarks() & REM_REVERSED) == 0);
    REQUIRE((reversed->Remarks() & REM_REVERSED) != 0);
    REQUIRE(normal->Level(0)->NVertex() == reversed->Level(0)->NVertex());
    for (int i = 0; i < normal->Level(0)->NVertex(); ++i)
    {
        const Shape* a = normal->Level(0);
        const Shape* b = reversed->Level(0);
        REQUIRE(b->Pos(i)[0] == Catch::Approx(-a->Pos(i)[0]));
        REQUIRE(b->Pos(i)[1] == Catch::Approx(a->Pos(i)[1]));
        REQUIRE(b->Pos(i)[2] == Catch::Approx(-a->Pos(i)[2]));
        REQUIRE(b->Norm(i)[2] == Catch::Approx(-a->Norm(i)[2]));
        REQUIRE(b->Tangent(i)[0] == Catch::Approx(-a->Tangent(i)[0]));
        REQUIRE(b->Binormal(i)[1] == Catch::Approx(a->Binormal(i)[1]));
    }
}

TEST_CASE("MLOD proxy-only sections stay hidden after proxy scanning", "[ShapeAdapter][mlod][proxy]")
{
    Model model;
    model.sourceFormat = "MLOD";
    model.sourcePath = "proxy-section-fixture.p3d";
    LODLevel lodLevel(1.0f);
    Mesh& mesh = lodLevel.mesh;
    mesh.vertices = {
        Vertex({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f}),
        Vertex({1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f}),
        Vertex({0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f}),
    };
    mesh.triangles.emplace_back(0, 1, 2);
    mesh.selections.emplace_back("proxy:missing_test_asset.01");
    mesh.selections.back().vertexIndices = {0, 1, 2};
    mesh.selections.back().triangleIndices = {0};
    model.lodLevels.push_back(std::move(lodLevel));

    std::unique_ptr<LODShapeWithShadow> shape(ShapeAdapter::convertToLODShape(model, false));
    REQUIRE(shape);
    REQUIRE(shape->Level(0)->NSections() == 1);
    REQUIRE((shape->Level(0)->GetSection(0).properties.Special() & IsHiddenProxy) != 0);
}

TEST_CASE("Model sections preserve a quad-only MLOD material run", "[Model][sections][mlod]")
{
    Model model;
    model.sourceFormat = "MLOD";
    LODLevel lodLevel(1.0f);
    Mesh& mesh = lodLevel.mesh;
    mesh.vertices = {
        Vertex({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f}),
        Vertex({1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f}),
        Vertex({1.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f}),
        Vertex({0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f}),
    };
    Material material("lamp", "lamp.paa");
    material.materialPath = "lamp.rvmat";
    mesh.materials.push_back(std::move(material));
    mesh.quads.emplace_back(0, 1, 2, 3);
    mesh.quads.back().materialIndex = 0;
    mesh.quads.back().originalIndex = 0;
    model.lodLevels.push_back(std::move(lodLevel));

    REQUIRE(model.compile());
    const auto& sections = model.lodLevels.front().mesh.sections;
    REQUIRE(sections.size() == 1);
    REQUIRE(sections.front().materialIndex == 0);
    REQUIRE(sections.front().startTriangle == 0);
    REQUIRE(sections.front().triangleCount == 1);
}

TEST_CASE("ShapeAdapter keeps faces past the old signed 16-bit vertex range", "[ShapeAdapter][odol][vertex-range]")
{
    // This asserted the opposite until VertexIndex was widened: a vertex table of
    // 32769 was truncated to 32768 and the face touching index 32768 was dropped.
    // On Reforger that rule cost church_01 140,041 of its faces and left
    // house_village_e_1l03t a roof with no walls.
    Model model;
    model.sourceFormat = "ODOL";
    model.sourcePath = "oversized-test.p3d";
    LODLevel lodLevel(1.0f);
    Mesh& mesh = lodLevel.mesh;
    mesh.vertices.resize(32769);
    mesh.vertices[0].position = {0.f, 0.f, 0.f};
    mesh.vertices[1].position = {1.f, 0.f, 0.f};
    mesh.vertices[2].position = {0.f, 1.f, 0.f};
    mesh.vertices[32768].position = {1.f, 1.f, 0.f};
    mesh.triangles.emplace_back(0, 1, 2);
    mesh.triangles.emplace_back(0, 1, 32768);
    mesh.triangles[0].originalIndex = 0;
    mesh.triangles[1].originalIndex = 1;
    model.lodLevels.push_back(std::move(lodLevel));

    std::unique_ptr<LODShapeWithShadow> shape;
    REQUIRE_NOTHROW(shape.reset(ShapeAdapter::convertToLODShape(model, false)));
    REQUIRE(shape);
    REQUIRE(shape->Level(0)->NVertex() == 32769);
    REQUIRE(shape->Level(0)->NFaces() == 2);
}

TEST_CASE("ShapeAdapter still rejects a face indexing past the vertex table", "[ShapeAdapter][odol][vertex-range]")
{
    // The guard is not gone, it moved. A face referencing a vertex the table does
    // not contain is malformed at any index width, and admitting it would let
    // RecalculateAreas dereference out of bounds -- which is what the clamp existed
    // to prevent in the first place. Widening the type must not turn a bounds check
    // into a no-op, so the negative case is asserted rather than assumed.
    Model model;
    model.sourceFormat = "ODOL";
    model.sourcePath = "malformed-test.p3d";
    LODLevel lodLevel(1.0f);
    Mesh& mesh = lodLevel.mesh;
    mesh.vertices.resize(64);
    mesh.vertices[0].position = {0.f, 0.f, 0.f};
    mesh.vertices[1].position = {1.f, 0.f, 0.f};
    mesh.vertices[2].position = {0.f, 1.f, 0.f};
    mesh.triangles.emplace_back(0, 1, 2);
    mesh.triangles.emplace_back(0, 1, 64); // one past the end
    mesh.triangles[0].originalIndex = 0;
    mesh.triangles[1].originalIndex = 1;
    model.lodLevels.push_back(std::move(lodLevel));

    std::unique_ptr<LODShapeWithShadow> shape;
    REQUIRE_NOTHROW(shape.reset(ShapeAdapter::convertToLODShape(model, false)));
    REQUIRE(shape);
    REQUIRE(shape->Level(0)->NVertex() == 64);
    REQUIRE(shape->Level(0)->NFaces() == 1);
}
