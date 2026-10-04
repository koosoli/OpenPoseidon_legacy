#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Dev/Diag/CaveMesh.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>

#include <cmath>
#include <limits>
#include <memory>

TEST_CASE("Generated cave rejects invalid dimensions and admits tiny apertures", "[CaveMesh][geometry]")
{
    Poseidon::Dev::CaveMeshParams p;
    REQUIRE(Poseidon::Dev::ValidCaveMeshParams(p));
    p.width = p.height = p.length = 0.1f;
    REQUIRE(Poseidon::Dev::ValidCaveMeshParams(p));
    REQUIRE(Poseidon::Dev::BuildCaveModel(p).lodLevels.size() == 6);
    p.width = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(Poseidon::Dev::ValidCaveMeshParams(p));
    REQUIRE(Poseidon::Dev::BuildCaveModel(p).IsEmpty());
    p.width = 12.01f;
    REQUIRE_FALSE(Poseidon::Dev::ValidCaveMeshParams(p));
    p.width = 4.0f;
    p.wallThickness = 0.0f;
    REQUIRE_FALSE(Poseidon::Dev::ValidCaveMeshParams(p));
}

TEST_CASE("Generated cave footprint, ceiling and roadway retain the authored origin", "[CaveMesh][geometry]")
{
    Poseidon::Dev::CaveMeshParams p;
    auto model = Poseidon::Dev::BuildCaveModel(p);
    REQUIRE(model.sourcePath != Poseidon::Dev::BuildCaveModel(p).sourcePath);
    const auto& memory = model.lodLevels[model.memoryIdx].mesh;
    REQUIRE(memory.selections.size() == 2);
    REQUIRE(memory.selections[0].name == "terrain_hole");
    REQUIRE(memory.selections[0].vertexIndices.size() == 4);
    for (auto i : memory.selections[0].vertexIndices)
    {
        REQUIRE(std::fabs(memory.vertices[i].position.x) == Catch::Approx(p.width * 0.5f));
        REQUIRE(memory.vertices[i].position.y == 0.0f);
        REQUIRE(memory.vertices[i].position.z >= 0.0f);
        REQUIRE(memory.vertices[i].position.z <= p.length);
    }
    REQUIRE(memory.selections[1].name == "terrain_hole_ceiling");
    REQUIRE(memory.selections[1].vertexIndices.size() == 1);
    REQUIRE(memory.vertices[memory.selections[1].vertexIndices[0]].position.y == p.height);
    const auto& roadway = model.lodLevels[model.roadwayIdx].mesh;
    REQUIRE(roadway.quads.size() == 1);
    for (const auto& vertex : roadway.vertices)
        REQUIRE(vertex.position.y == 0.0f);
    p.includeCeilingSelection = false;
    REQUIRE(Poseidon::Dev::BuildCaveModel(p).lodLevels[2].mesh.selections.size() == 1);
    REQUIRE(model.pathsIdx == -1); // no invented AI navigation graph
}

TEST_CASE("Generated cave adapter creates solid slabs around an empty walkable corridor", "[CaveMesh][collision]")
{
    const Poseidon::Dev::CaveMeshParams p;
    auto model = Poseidon::Dev::BuildCaveModel(p);
    // No asset/bank/device dependency: testing collision and selections only.
    const Poseidon::Model::ShapeAdapter::AdapterBankTables tables;
    std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(model, false, &tables));
    REQUIRE(shape);
    REQUIRE(shape->Mass() >= 10.0f);
    REQUIRE(shape->GeometryLevel());
    REQUIRE(shape->FireGeometryLevel());
    REQUIRE(shape->ViewGeometryLevel());
    REQUIRE(shape->RoadwayLevel());
    REQUIRE(shape->MemoryLevel());
    REQUIRE(shape->GetPropertyDammage() == RStringB("no"));
    const auto& components = shape->GetGeomComponents();
    REQUIRE(components.Size() == 5);
    for (int i = 0; i < components.Size(); ++i)
    {
        REQUIRE(components[i]->NPlanes() == 6);
        REQUIRE_FALSE(components[i]->IsInside(::Vector3(0, p.height * 0.5f, p.length * 0.5f)));
        REQUIRE_FALSE(components[i]->IsInside(::Vector3(0, p.height * 0.5f, -0.1f)));
    }
    REQUIRE(components[0]->IsInside(::Vector3(-p.width * 0.5f - p.wallThickness * 0.5f, 1, 1)));
    REQUIRE(components[1]->IsInside(::Vector3(p.width * 0.5f + p.wallThickness * 0.5f, 1, 1)));
    REQUIRE(components[2]->IsInside(::Vector3(0, p.height + p.wallThickness * 0.5f, 1)));
    REQUIRE(components[3]->IsInside(::Vector3(0, -p.wallThickness * 0.5f, 1)));
    REQUIRE(components[4]->IsInside(::Vector3(0, 1, p.length + p.wallThickness * 0.5f)));
    Shape* roadway = shape->RoadwayLevel();
    roadway->InitPlanes();
    float y = -999, dx = 999, dz = 999;
    REQUIRE(roadway->Face(roadway->BeginFaces())
                .InsideFromTop(*roadway, roadway->GetPlane(0), ::Vector3(0, 0, p.length * 0.5f), &y, &dx, &dz));
    REQUIRE(y == 0.0f);
    REQUIRE(dx == 0.0f);
    REQUIRE(dz == 0.0f);
    REQUIRE(roadway->Pos(0).Y() == 0.0f); // autocenter=0 survived adapter
}
