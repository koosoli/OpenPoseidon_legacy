// test_lod_purpose.cpp - AST-018: typed LOD purposes.
//
// The behaviour under test is not "the table has the right numbers" -- it is that
// a resolution outside the visual range never reads as a drawable LOD, whether or
// not this build happens to name it.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Model/LodPurpose.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <string>

using Poseidon::Model::ClassifyLodResolution;
using Poseidon::Model::IsGeometryOnlyLod;
using Poseidon::Model::LodPurpose;
using Poseidon::Model::LodPurposeName;

TEST_CASE("Ordinary viewing distances are visual LODs", "[model][lod][ast-018]")
{
    for (float resolution : {0.0f, 1.0f, 50.0f, 500.0f, 2000.0f, 10000.0f})
    {
        CAPTURE(resolution);
        REQUIRE(ClassifyLodResolution(resolution) == LodPurpose::Visual);
        REQUIRE_FALSE(IsGeometryOnlyLod(ClassifyLodResolution(resolution)));
    }
}

TEST_CASE("Special-LOD sentinels are classified by purpose", "[model][lod][ast-018]")
{
    REQUIRE(ClassifyLodResolution(1e13f) == LodPurpose::Geometry);
    REQUIRE(ClassifyLodResolution(1e15f) == LodPurpose::Memory);
    REQUIRE(ClassifyLodResolution(2e15f) == LodPurpose::LandContact);
    REQUIRE(ClassifyLodResolution(3e15f) == LodPurpose::Roadway);
    REQUIRE(ClassifyLodResolution(4e15f) == LodPurpose::Paths);
    REQUIRE(ClassifyLodResolution(5e15f) == LodPurpose::HitPoints);
    REQUIRE(ClassifyLodResolution(6e15f) == LodPurpose::ViewGeometry);
    REQUIRE(ClassifyLodResolution(7e15f) == LodPurpose::FireGeometry);
    REQUIRE(ClassifyLodResolution(8e15f) == LodPurpose::ViewCargoGeometry);
    REQUIRE(ClassifyLodResolution(10e15f) == LodPurpose::ViewCommander);
    REQUIRE(ClassifyLodResolution(11e15f) == LodPurpose::ViewCommanderGeometry);
    REQUIRE(ClassifyLodResolution(13e15f) == LodPurpose::ViewPilotGeometry);
    REQUIRE(ClassifyLodResolution(15e15f) == LodPurpose::ViewGunnerGeometry);
    REQUIRE(ClassifyLodResolution(16e15f) == LodPurpose::FireGunnerGeometry);
}

TEST_CASE("Sentinels survive the float they arrive in", "[model][lod][ast-018]")
{
    // The ODOL 73 fixture's geometry LOD stores 1e13 as 9999999827968.0f. An
    // equality check against the sentinel would miss it, and the LOD would be
    // drawn as ordinary geometry at a thirteen-digit draw distance.
    REQUIRE(ClassifyLodResolution(9999999827968.0f) == LodPurpose::Geometry);
}

TEST_CASE("Crewed view LODs are visual, not geometry-only", "[model][lod][ast-018]")
{
    // These are drawn: they are ordinary LODs at reserved distances, and folding
    // them in with the sentinels would make the crew's view disappear.
    REQUIRE(ClassifyLodResolution(1000.0f) == LodPurpose::ViewGunner);
    REQUIRE(ClassifyLodResolution(1100.0f) == LodPurpose::ViewPilot);
    REQUIRE(ClassifyLodResolution(1200.0f) == LodPurpose::ViewCargo);
    REQUIRE_FALSE(IsGeometryOnlyLod(LodPurpose::ViewGunner));
    REQUIRE_FALSE(IsGeometryOnlyLod(LodPurpose::ViewPilot));
    REQUIRE_FALSE(IsGeometryOnlyLod(LodPurpose::ViewCargo));
}

TEST_CASE("An unnamed sentinel is Unknown, never Visual", "[model][lod][ast-018]")
{
    // 9e15 and 12e15 are reserved in the source table but not defined by this
    // build. The point of the Unknown state is that they still cannot be drawn.
    for (float resolution : {9e15f, 12e15f, 14e15f, 1e20f})
    {
        CAPTURE(resolution);
        REQUIRE(ClassifyLodResolution(resolution) == LodPurpose::Unknown);
        REQUIRE(std::string(LodPurposeName(ClassifyLodResolution(resolution))) != "Visual");
    }
}

TEST_CASE("Every geometry-only purpose has a name that is not Visual", "[model][lod][ast-018]")
{
    for (uint8_t raw = 0; raw <= static_cast<uint8_t>(LodPurpose::Unknown); ++raw)
    {
        const auto purpose = static_cast<LodPurpose>(raw);
        CAPTURE(raw);
        REQUIRE(LodPurposeName(purpose) != nullptr);
        if (IsGeometryOnlyLod(purpose))
            REQUIRE(std::string(LodPurposeName(purpose)) != "Visual");
    }
}

TEST_CASE("A LOD built from a resolution carries its purpose", "[model][lod][ast-018]")
{
    // The convenience constructor is what most call sites use; leaving it to
    // default to Visual would put the burden back on every consumer.
    REQUIRE(Poseidon::Model::LODLevel(1e13f).purpose == LodPurpose::Geometry);
    REQUIRE(Poseidon::Model::LODLevel(100.0f).purpose == LodPurpose::Visual);
}
