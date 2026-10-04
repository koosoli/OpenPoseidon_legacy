#include <catch2/catch_test_macros.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/ObjectDrawRange.hpp>

TEST_CASE("Fog-limited object silhouettes stay within supporting terrain reach", "[scene][fog-range]")
{
    REQUIRE(Poseidon::ResolveObjectDrawRange(1333.0f, 515.0f, 0.0f) == 515.0f);
    REQUIRE(Poseidon::ResolveObjectDrawRange(1333.0f, 515.0f, 1.0f) == 515.0f);
    REQUIRE(Poseidon::ResolveObjectDrawRange(1333.0f, 515.0f, 3.0f) == 515.0f);
    REQUIRE(Poseidon::ResolveObjectDrawRange(1333.0f, 2000.0f, 0.0f) == 2000.0f);
    REQUIRE(Poseidon::ResolveObjectDrawRange(1333.0f, 2000.0f, 1.0f) == 1333.0f);
    REQUIRE(Poseidon::ResolveObjectDrawRange(1333.0f, 8000.0f, 0.0f) == 8000.0f);
    REQUIRE(Poseidon::ResolveObjectDrawRange(1333.0f, 0.0f, 0.0f) == 1333.0f);
}

TEST_CASE("scene.hpp compiles", "[scene]")
{
    SUCCEED("header included successfully");
}

TEST_CASE("Scene skips visible light volumes with missing shapes", "[scene][lights]")
{
    Poseidon::Scene scene;
    Poseidon::Frame frame;

    REQUIRE_NOTHROW(scene.DrawVolumeLight(nullptr, PackedWhite, frame, 1.0f));
}
