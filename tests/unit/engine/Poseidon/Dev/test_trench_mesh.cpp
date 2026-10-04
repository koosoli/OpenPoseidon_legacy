#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Dev/Diag/TrenchMesh.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>
#include <Poseidon/World/Scene/Object.hpp>

#include <cmath>
#include <limits>
#include <memory>

namespace
{
std::unique_ptr<LODShapeWithShadow> Adapt(const Poseidon::Dev::TrenchMeshParams& p)
{
    const Poseidon::Model::ShapeAdapter::AdapterBankTables tables;
    return std::unique_ptr<LODShapeWithShadow>(
        Poseidon::Model::ShapeAdapter::convertToLODShape(Poseidon::Dev::BuildTrenchModel(p), false, &tables));
}

void RequireRoadHeight(Shape& roadway, float z, float expected, float expectedDz)
{
    roadway.InitPlanes();
    bool hit = false;
    int fi = 0;
    for (auto f = roadway.BeginFaces(); f < roadway.EndFaces(); roadway.NextFace(f), ++fi)
    {
        float y = -999, dx = 999, dz = 999;
        if (roadway.Face(f).InsideFromTop(roadway, roadway.GetPlane(fi), ::Vector3(0, 0, z), &y, &dx, &dz))
        {
            REQUIRE(y == Catch::Approx(expected).margin(1e-6f));
            REQUIRE(dx == Catch::Approx(0).margin(1e-6f));
            REQUIRE(dz == Catch::Approx(expectedDz).margin(1e-6f));
            hit = true;
        }
    }
    REQUIRE(hit);
}
} // namespace

TEST_CASE("Generated trenches validate dimensions and retain an open rectangular footprint", "[TrenchMesh][geometry]")
{
    using namespace Poseidon::Dev;
    for (const auto& p : {TrenchMeshParams{}, TankTrenchMeshParams()})
    {
        REQUIRE(ValidTrenchMeshParams(p));
        const auto model = BuildTrenchModel(p);
        REQUIRE(model.sourcePath != BuildTrenchModel(p).sourcePath);
        REQUIRE(model.lodLevels.size() == 6);
        const auto& memory = model.lodLevels[model.memoryIdx].mesh;
        REQUIRE(memory.selections.size() == 1);
        REQUIRE(memory.selections[0].name == "terrain_hole");
        REQUIRE(memory.selections[0].vertexIndices.size() == 4);
        for (const auto& v : memory.vertices)
        {
            REQUIRE(std::fabs(v.position.x) == Catch::Approx(p.width * 0.5f));
            REQUIRE(v.position.y == 0);
            REQUIRE(v.position.z >= 0);
            REQUIRE(v.position.z <= p.length);
        }
        REQUIRE(model.pathsIdx == -1);
        REQUIRE(model.lodLevels[model.geometryIdx].mesh.selections.size() == 5);
        for (const auto& v : model.lodLevels[0].mesh.vertices)
        {
            REQUIRE(std::isfinite(v.normal.x));
            REQUIRE(std::isfinite(v.normal.y));
            REQUIRE(std::isfinite(v.normal.z));
        }
    }
    auto p = TrenchMeshParams{};
    p.depth = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(ValidTrenchMeshParams(p));
    REQUIRE(BuildTrenchModel(p).IsEmpty());
    p = {};
    p.rampLength = p.depth;
    REQUIRE_FALSE(ValidTrenchMeshParams(p));
    p = {};
    p.length = p.rampLength + 0.001f;
    REQUIRE_FALSE(ValidTrenchMeshParams(p));
    p = {};
    p.width = 12.01f;
    REQUIRE_FALSE(ValidTrenchMeshParams(p));
}

TEST_CASE("Generated infantry and tank trenches have a supported descent and empty open top", "[TrenchMesh][collision]")
{
    using namespace Poseidon::Dev;
    for (const auto& p : {TrenchMeshParams{}, TankTrenchMeshParams()})
    {
        auto shape = Adapt(p);
        REQUIRE(shape);
        REQUIRE(shape->Mass() >= 10);
        REQUIRE(shape->GeometryLevel());
        REQUIRE(shape->RoadwayLevel());
        REQUIRE(shape->MemoryLevel());
        REQUIRE(shape->GetGeomComponents().Size() == 5);
        REQUIRE(shape->GetFireComponents().Size() == 5);
        REQUIRE(shape->GetViewComponents().Size() == 5);
        const auto& c = shape->GetGeomComponents();
        for (const auto* set : {&shape->GetGeomComponents(), &shape->GetFireComponents(), &shape->GetViewComponents()})
        {
            for (int i = 0; i < set->Size(); ++i)
            {
                REQUIRE((*set)[i]->NPlanes() == 6);
                REQUIRE_FALSE((*set)[i]->IsInside(::Vector3(0, 1, p.length * 0.5f))); // no ceiling
                REQUIRE_FALSE((*set)[i]->IsInside(::Vector3(0, 0.05f, -0.1f)));       // open ground-level entrance
                REQUIRE_FALSE((*set)[i]->IsInside(::Vector3(0, -p.depth * 0.5f + 0.1f, p.rampLength * 0.5f)));
                REQUIRE_FALSE((*set)[i]->IsInside(::Vector3(0, -p.depth + 0.1f, (p.rampLength + p.length) * 0.5f)));
            }
        }
        REQUIRE(c[0]->IsInside(::Vector3(-p.width * 0.5f - p.wallThickness * 0.5f, -p.depth * 0.5f, 1)));
        REQUIRE(c[1]->IsInside(::Vector3(p.width * 0.5f + p.wallThickness * 0.5f, -p.depth * 0.5f, 1)));
        REQUIRE(c[2]->IsInside(::Vector3(0, -p.depth * 0.5f - p.wallThickness * 0.5f, p.rampLength * 0.5f)));
        REQUIRE(c[3]->IsInside(::Vector3(0, -p.depth - p.wallThickness * 0.5f, (p.rampLength + p.length) * 0.5f)));
        REQUIRE(c[4]->IsInside(::Vector3(0, -p.depth * 0.5f, p.length + p.wallThickness * 0.5f)));
        RequireRoadHeight(*shape->RoadwayLevel(), 0, 0, -p.depth / p.rampLength);
        RequireRoadHeight(*shape->RoadwayLevel(), p.rampLength * 0.5f, -p.depth * 0.5f, -p.depth / p.rampLength);
        RequireRoadHeight(*shape->RoadwayLevel(), p.rampLength + 0.1f, -p.depth, 0);
        RequireRoadHeight(*shape->RoadwayLevel(), p.length, -p.depth, 0);
    }
}

TEST_CASE("Ramp-only trenches omit degenerate bed slabs and retain a supported endpoint", "[TrenchMesh][collision]")
{
    Poseidon::Dev::TrenchMeshParams p;
    p.length = p.rampLength;
    REQUIRE(Poseidon::Dev::ValidTrenchMeshParams(p));
    auto shape = Adapt(p);
    REQUIRE(shape);
    REQUIRE(shape->GetGeomComponents().Size() == 4);
    REQUIRE(shape->RoadwayLevel()->NFaces() == 1);
    RequireRoadHeight(*shape->RoadwayLevel(), p.length, -p.depth, -p.depth / p.rampLength);
}

TEST_CASE("Generated earth does not inherit low-mass tree destruction", "[TrenchMesh][collision]")
{
    for (const auto& p : {Poseidon::Dev::TrenchMeshParams{}, Poseidon::Dev::TankTrenchMeshParams()})
    {
        Ref<LODShapeWithShadow> shape = Adapt(p).release();
        REQUIRE(shape);
        Poseidon::ObjectPlain earth(shape, -1);
        REQUIRE(earth.GetDestructType() == Poseidon::DestructNo);
    }
}
