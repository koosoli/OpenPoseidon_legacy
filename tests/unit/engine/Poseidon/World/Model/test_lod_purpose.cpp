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
using Poseidon::Model::IsDrawableLod;
using Poseidon::Model::IsGeometryOnlyLod;
using Poseidon::Model::LodPurpose;
using Poseidon::Model::LodPurposeName;

TEST_CASE("Ordinary viewing distances are visual LODs", "[model][lod][ast-018]")
{
    // 10000 used to be in this list. It is Arma 3's shadow-volume band, not a
    // viewing distance -- see the shadow-LOD case below for what that cost.
    for (float resolution : {0.0f, 1.0f, 50.0f, 500.0f, 2000.0f, 9999.0f})
    {
        CAPTURE(resolution);
        REQUIRE(ClassifyLodResolution(resolution) == LodPurpose::Visual);
        REQUIRE_FALSE(IsGeometryOnlyLod(ClassifyLodResolution(resolution)));
    }
}

TEST_CASE("Arma 3's shadow LODs are not viewing distances", "[model][lod][ast-018][shadow]")
{
    // The bands, not single values: shadow volumes are 10000, 10010, 10020... and
    // shadow buffers 11000, 11010... A model carries several.
    for (float resolution : {10000.0f, 10010.0f, 10020.0f, 10990.0f})
    {
        CAPTURE(resolution);
        REQUIRE(ClassifyLodResolution(resolution) == LodPurpose::ShadowVolume);
    }
    for (float resolution : {11000.0f, 11010.0f, 11990.0f})
    {
        CAPTURE(resolution);
        REQUIRE(ClassifyLodResolution(resolution) == LodPurpose::ShadowBuffer);
    }

    // This is the property that matters. These sit BELOW the 1e5 cut-off that
    // catches every other sentinel, so they read as ordinary LODs at a 10 km
    // viewing distance -- which is exactly what a distance-based LOD chooser
    // reaches for when an object is far away, and it drew the shadow silhouette.
    REQUIRE(IsGeometryOnlyLod(LodPurpose::ShadowVolume));
    REQUIRE(IsGeometryOnlyLod(LodPurpose::ShadowBuffer));
    REQUIRE_FALSE(IsDrawableLod(LodPurpose::ShadowVolume));
    REQUIRE_FALSE(IsDrawableLod(LodPurpose::ShadowBuffer));
}

TEST_CASE("An unnamed sentinel is not drawable either", "[model][lod][ast-018]")
{
    // `Unknown` is neither drawable nor geometry-only, so a consumer asking only
    // "is this geometry?" would draw it. Measured case: Arma 3 models carry a
    // 4e13 LOD this table does not name, and the penetration materials on it
    // reached the colour pass.
    REQUIRE(ClassifyLodResolution(4e13f) == LodPurpose::Unknown);
    REQUIRE_FALSE(IsGeometryOnlyLod(LodPurpose::Unknown));
    REQUIRE_FALSE(IsDrawableLod(LodPurpose::Unknown));
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

// The shape path's ResolGeometryOnly now delegates to the IR table. That is a
// change to the draw path, so the claim "same answers" is pinned here rather
// than asserted in a commit message.
TEST_CASE("Geometry-only classification matches the shape path's old table", "[model][lod][ast-018]")
{
    const float kOldTable[] = {1e13f,        // GEOMETRY_SPEC
                               6e15f, 7e15f, // VIEW_GEOM_SPEC, FIRE_GEOM_SPEC
                               13e15f,       // VIEW_PILOT_GEOM_SPEC
                               8e15f,        // VIEW_CARGO_GEOM_SPEC
                               11e15f,       // VIEW_COMMANDER_GEOM_SPEC
                               15e15f,       // VIEW_GUNNER_GEOM_SPEC
                               1e15f,        // MEMORY_SPEC
                               2e15f, 3e15f, // LANDCONTACT_SPEC, ROADWAY_SPEC
                               4e15f, 5e15f};// PATHS_SPEC, HITPOINTS_SPEC
    for (float resolution : kOldTable)
    {
        CAPTURE(resolution);
        REQUIRE(IsGeometryOnlyLod(ClassifyLodResolution(resolution)));
    }
    for (float resolution : {1.0f, 100.0f, 1000.0f, 1100.0f, 1200.0f, 5000.0f})
    {
        CAPTURE(resolution);
        REQUIRE_FALSE(IsGeometryOnlyLod(ClassifyLodResolution(resolution)));
    }
    // The one deliberate difference. The old table listed the fire-geometry
    // sentinel but not its gunner variant, so a LOD that is never drawn was
    // loaded with normals as if it were. It is geometry-only now.
    REQUIRE(IsGeometryOnlyLod(ClassifyLodResolution(16e15f)));
    // Unknown stays outside the geometry-only set, exactly as an unlisted
    // sentinel did before: this change adds no new load-path behaviour for LODs
    // the table cannot name.
    REQUIRE(ClassifyLodResolution(9e15f) == LodPurpose::Unknown);
    REQUIRE_FALSE(IsGeometryOnlyLod(LodPurpose::Unknown));
}

TEST_CASE("A LOD built from a resolution carries its purpose", "[model][lod][ast-018]")
{
    // The convenience constructor is what most call sites use; leaving it to
    // default to Visual would put the burden back on every consumer.
    REQUIRE(Poseidon::Model::LODLevel(1e13f).purpose == LodPurpose::Geometry);
    REQUIRE(Poseidon::Model::LODLevel(100.0f).purpose == LodPurpose::Visual);
}

// The special-LOD slot assignment in LODShape decides which LOD index lands in
// _geometry, _memory, _roadway and friends -- and the code immediately after it
// does _lods[_geometry]->OrSpecial(IsAlpha | IsAlphaFog | IsColored). So a
// visual LOD wrongly assigned a geometry slot would be made alpha-transparent:
// the model would render translucent. This reproduces the old macro chain and
// the new switch side by side and requires them to agree everywhere.
TEST_CASE("Special-LOD slot assignment is unchanged by the typed table", "[model][lod][ast-018]")
{
    enum Slot { None, Geometry, Memory, LandContact, Roadway, Paths, HitPoints,
                ViewGeom, FireGeom, PilotGeom, GunnerGeom, CommanderGeom, CargoGeom };

    auto isSpec = [](float resolution, double spec) {
        const double difference = static_cast<double>(resolution) - spec;
        return (difference < 0 ? -difference : difference) < spec * 1e-3;
    };
    // The chain exactly as it was written, in its original order.
    auto oldSlot = [&](float r) {
        if (!(r > 900)) return None;
        if (isSpec(r, 1e13)) return Geometry;
        if (isSpec(r, 1e15 * 1)) return Memory;
        if (isSpec(r, 1e15 * 2)) return LandContact;
        if (isSpec(r, 1e15 * 3)) return Roadway;
        if (isSpec(r, 1e15 * 4)) return Paths;
        if (isSpec(r, 1e15 * 5)) return HitPoints;
        if (isSpec(r, 1e15 * 6)) return ViewGeom;
        if (isSpec(r, 1e15 * 7)) return FireGeom;
        if (isSpec(r, 1e15 * 13)) return PilotGeom;
        if (isSpec(r, 1e15 * 15)) return GunnerGeom;
        if (isSpec(r, 1e15 * 11)) return CommanderGeom;
        if (isSpec(r, 1e15 * 8)) return CargoGeom;
        return None;
    };
    auto newSlot = [](float r) {
        if (!(r > 900)) return None;
        switch (ClassifyLodResolution(r))
        {
            case LodPurpose::Geometry:              return Geometry;
            case LodPurpose::Memory:                return Memory;
            case LodPurpose::LandContact:           return LandContact;
            case LodPurpose::Roadway:               return Roadway;
            case LodPurpose::Paths:                 return Paths;
            case LodPurpose::HitPoints:             return HitPoints;
            case LodPurpose::ViewGeometry:          return ViewGeom;
            case LodPurpose::FireGeometry:          return FireGeom;
            case LodPurpose::ViewPilotGeometry:     return PilotGeom;
            case LodPurpose::ViewGunnerGeometry:    return GunnerGeom;
            case LodPurpose::ViewCommanderGeometry: return CommanderGeom;
            case LodPurpose::ViewCargoGeometry:     return CargoGeom;
            default:                                return None;
        }
    };

    // Every sentinel, including the reserved-but-unnamed ones.
    for (int multiple = 1; multiple <= 20; ++multiple)
    {
        const float resolution = static_cast<float>(1e15 * multiple);
        CAPTURE(multiple, resolution);
        REQUIRE(oldSlot(resolution) == newSlot(resolution));
    }
    REQUIRE(oldSlot(1e13f) == newSlot(1e13f));
    REQUIRE(oldSlot(9999999827968.0f) == newSlot(9999999827968.0f));

    // And a sweep of ordinary and crewed-view distances: none of these may be
    // assigned a slot, because a slot means "made alpha transparent" for
    // _geometry, _geometryView and _geometryFire.
    for (float resolution : {0.5f, 1.0f, 100.0f, 901.0f, 999.0f, 1000.0f, 1100.0f, 1200.0f,
                             1999.0f, 2000.0f, 5000.0f, 9999.0f, 10000.0f, 11000.0f, 50000.0f,
                             99999.0f})
    {
        CAPTURE(resolution);
        REQUIRE(oldSlot(resolution) == newSlot(resolution));
        REQUIRE(newSlot(resolution) == None);
    }
}
