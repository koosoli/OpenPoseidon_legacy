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
