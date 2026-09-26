#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/World/Terrain/WorldObjectPlacementFrame.hpp>
#include <Poseidon/World/Terrain/SimulationCoverageIndex.hpp>

#include <cmath>
#include <limits>

using namespace Poseidon;

namespace
{
Matrix4 ShearedPlacement()
{
    Matrix4 raw = MIdentity;
    raw.SetDirectionAside(Vector3(2, 1, 0));
    raw.SetDirectionUp(Vector3(0, 3, 1));
    raw.SetDirection(Vector3(1, 0, 4));
    raw.SetPosition(Vector3(170, 25, -60));
    return raw;
}

void RequireSameVector(Vector3Par actual, Vector3Par expected)
{
    REQUIRE(actual.X() == expected.X());
    REQUIRE(actual.Y() == expected.Y());
    REQUIRE(actual.Z() == expected.Z());
}

void RequireScaledRotation(const Matrix4& frame, float scale)
{
    REQUIRE(frame.DirectionAside().Size() == Catch::Approx(scale).margin(0.0001f));
    REQUIRE(frame.DirectionUp().Size() == Catch::Approx(scale).margin(0.0001f));
    REQUIRE(frame.Direction().Size() == Catch::Approx(scale).margin(0.0001f));
    REQUIRE((frame.DirectionAside() * frame.DirectionUp()) == Catch::Approx(0).margin(0.0001f));
    REQUIRE((frame.DirectionAside() * frame.Direction()) == Catch::Approx(0).margin(0.0001f));
    REQUIRE((frame.DirectionUp() * frame.Direction()) == Catch::Approx(0).margin(0.0001f));
}
}

TEST_CASE("Placement forest exception preserves shear scale and translation verbatim", "[streaming-placement-frame]")
{
    const Matrix4 raw = ShearedPlacement();
    // Forest bypass precedes both Network scale and slope-following repair.
    const Matrix4 result = RepairWorldObjectPlacementFrame(raw, true, true, true);
    RequireSameVector(result.DirectionAside(), raw.DirectionAside());
    RequireSameVector(result.DirectionUp(), raw.DirectionUp());
    RequireSameVector(result.Direction(), raw.Direction());
    RequireSameVector(result.Position(), raw.Position());
}

TEST_CASE("Placement repair removes shear while preserving engine RMS scale", "[streaming-placement-frame]")
{
    const Matrix4 raw = ShearedPlacement();
    const float expectedScale = raw.Scale();
    // Engine Scale uses sqrt(mean squared column/row magnitudes), not one axis length.
    REQUIRE(expectedScale == Catch::Approx(std::sqrt(32.0f / 3.0f)).margin(0.0001f));
    const Matrix4 result = RepairWorldObjectPlacementFrame(raw, false, false, false);
    REQUIRE(result.Scale() == Catch::Approx(expectedScale).margin(0.0001f));
    RequireScaledRotation(result, expectedScale);
    RequireSameVector(result.Position(), raw.Position());
    REQUIRE(result.DirectionUp().Normalized().Distance(raw.DirectionUp().Normalized()) < 0.0001f);
    REQUIRE(std::abs(result.DirectionAside() * raw.DirectionUp()) < 0.0001f);
}

TEST_CASE("Placement Network repair selects scale one before rebuilding orientation", "[streaming-placement-frame]")
{
    const Matrix4 raw = ShearedPlacement();
    const Matrix4 result = RepairWorldObjectPlacementFrame(raw, false, true, false);
    REQUIRE(result.Scale() == Catch::Approx(1.0f).margin(0.0001f));
    RequireScaledRotation(result, 1.0f);
    RequireSameVector(result.Position(), raw.Position());
}

TEST_CASE("Placement ClipLandKeep uses world up and original aside with pre-repair scale", "[streaming-placement-frame]")
{
    const Matrix4 raw = ShearedPlacement();
    const Matrix4 result = RepairWorldObjectPlacementFrame(raw, false, false, true);
    RequireScaledRotation(result, raw.Scale());
    REQUIRE(result.DirectionUp().X() == 0);
    REQUIRE(result.DirectionUp().Z() == 0);
    REQUIRE(result.DirectionUp().Y() == Catch::Approx(raw.Scale()).margin(0.0001f));
    REQUIRE(result.DirectionAside().Y() == 0);
    REQUIRE(result.DirectionAside().X() > 0);
    RequireSameVector(result.Position(), raw.Position());
    const Matrix4 network = RepairWorldObjectPlacementFrame(raw, false, true, true);
    RequireScaledRotation(network, 1.0f);
    RequireSameVector(network.Position(), raw.Position());
}

TEST_CASE("Degenerate placement behavior is preserved and certification refuses it separately", "[streaming-placement-frame]")
{
    Matrix4 raw = MZero;
    raw.SetPosition(Vector3(5, 6, 7));
    const Matrix4 forest = RepairWorldObjectPlacementFrame(raw, true, false, false);
    RequireSameVector(forest.DirectionAside(), raw.DirectionAside());
    RequireSameVector(forest.DirectionUp(), raw.DirectionUp());
    RequireSameVector(forest.Direction(), raw.Direction());
    RequireSameVector(forest.Position(), raw.Position());
    // Finite zero orientation is not a valid positive-scale broadphase certificate.
    REQUIRE(std::isfinite(forest.DirectionUp().Y()));
    REQUIRE_FALSE(Streaming::BuildEngineBroadphaseEnvelope({5, 6, 7}, forest.Scale(), 1));
    // Repair is not responsible for sanitizing authored nonfinite position evidence.
    raw.SetPosition(Vector3(std::numeric_limits<float>::quiet_NaN(), 6, 7));
    const Matrix4 invalid = RepairWorldObjectPlacementFrame(raw, true, false, false);
    REQUIRE(std::isnan(invalid.Position().X()));
    REQUIRE_FALSE(Streaming::BuildEngineBroadphaseEnvelope(
        {invalid.Position().X(), invalid.Position().Y(), invalid.Position().Z()}, 1, 1));
}
