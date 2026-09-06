#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/WaterBodies.hpp>

using namespace Poseidon;

namespace
{
WaterBody Lake(float cx, float cz, float rx, float rz, float level)
{
    WaterBody b;
    b.kind = WaterBodyKind::Lake;
    b.centreX = cx;
    b.centreZ = cz;
    b.radiusX = rx;
    b.radiusZ = rz;
    b.level = level;
    return b;
}
} // namespace

TEST_CASE("Water body containment is the ellipse, not the box", "[World][Terrain][Water]")
{
    WaterBodyRegistry reg;
    reg.Add(Lake(1000.0f, 2000.0f, 100.0f, 50.0f, 45.0f));
    // Centre and the axis ends are in.
    REQUIRE(reg.Find(1000.0f, 2000.0f) != nullptr);
    REQUIRE(reg.Find(1099.0f, 2000.0f) != nullptr);
    REQUIRE(reg.Find(1000.0f, 2049.0f) != nullptr);
    // A box corner is inside the AABB but outside the ellipse: a camera below the lake's
    // elevation there must stay in air (plan WRL-003 step 3).
    REQUIRE(reg.Find(1095.0f, 2045.0f) == nullptr);
    // Outside the box entirely.
    REQUIRE(reg.Find(1200.0f, 2000.0f) == nullptr);
    REQUIRE(reg.Find(0.0f, 0.0f) == nullptr);
}

TEST_CASE("A sloping river reach interpolates its surface height", "[World][Terrain][Water]")
{
    WaterBody river = Lake(500.0f, 500.0f, 400.0f, 30.0f, 20.0f);
    river.kind = WaterBodyKind::River;
    river.gradientX = -0.01f; // falls 1 cm per metre downstream (+x)
    REQUIRE(river.SurfaceLevelAt(500.0f, 500.0f) == Catch::Approx(20.0f));
    REQUIRE(river.SurfaceLevelAt(600.0f, 500.0f) == Catch::Approx(19.0f));
    REQUIRE(river.SurfaceLevelAt(400.0f, 520.0f) == Catch::Approx(21.0f));
    // A lake's surface is flat everywhere inside it.
    const WaterBody lake = Lake(0.0f, 0.0f, 10.0f, 10.0f, 7.5f);
    REQUIRE(lake.SurfaceLevelAt(9.0f, -9.0f) == Catch::Approx(7.5f));
}

TEST_CASE("Overlapping bodies resolve to the smaller one, deterministically", "[World][Terrain][Water]")
{
    WaterBodyRegistry reg;
    const uint32_t big = reg.Add(Lake(0.0f, 0.0f, 200.0f, 200.0f, 10.0f));
    const uint32_t pond = reg.Add(Lake(50.0f, 50.0f, 20.0f, 20.0f, 12.0f));
    REQUIRE(reg.Find(50.0f, 50.0f)->id == pond);
    REQUIRE(reg.Find(-100.0f, 0.0f)->id == big);
    // Equal areas: the lower id wins, whichever order they were registered in.
    WaterBodyRegistry tie;
    const uint32_t first = tie.Add(Lake(0.0f, 0.0f, 30.0f, 30.0f, 1.0f));
    const uint32_t second = tie.Add(Lake(10.0f, 0.0f, 30.0f, 30.0f, 2.0f));
    REQUIRE(second > first);
    REQUIRE(tie.Find(5.0f, 0.0f)->id == first);
}

TEST_CASE("Registry lifecycle: ids are stable, generation moves on every change", "[World][Terrain][Water]")
{
    WaterBodyRegistry reg;
    REQUIRE(reg.Empty());
    const uint32_t g0 = reg.Generation();
    const uint32_t a = reg.Add(Lake(0.0f, 0.0f, 5.0f, 5.0f, 0.0f));
    const uint32_t b = reg.Add(Lake(100.0f, 0.0f, 5.0f, 5.0f, 0.0f));
    REQUIRE(a != b);
    REQUIRE(reg.Generation() > g0);
    const uint32_t g1 = reg.Generation();
    REQUIRE(reg.Remove(a));
    REQUIRE_FALSE(reg.Remove(a));
    REQUIRE(reg.Generation() > g1);
    REQUIRE(reg.FindById(a) == nullptr);
    REQUIRE(reg.FindById(b) != nullptr);
    // A new body never reuses a removed id.
    const uint32_t c = reg.Add(Lake(200.0f, 0.0f, 5.0f, 5.0f, 0.0f));
    REQUIRE(c != a);
    REQUIRE(c > b);
    const uint32_t g2 = reg.Generation();
    reg.Clear();
    REQUIRE(reg.Empty());
    REQUIRE(reg.Generation() > g2);
    // Clearing an empty registry is not a change.
    const uint32_t g3 = reg.Generation();
    reg.Clear();
    REQUIRE(reg.Generation() == g3);
}

TEST_CASE("A rotated reach contains along its heading, not the world axes", "[World][Terrain][Water]")
{
    WaterBody reach = Lake(0.0f, 0.0f, 200.0f, 20.0f, 30.0f);
    reach.kind = WaterBodyKind::River;
    reach.rotation = 3.14159265f * 0.5f; // heading +z
    REQUIRE(reach.Contains(0.0f, 150.0f));
    REQUIRE_FALSE(reach.Contains(150.0f, 0.0f));
    REQUIRE(reach.ExtentX() == Catch::Approx(20.0f).margin(1e-3f));
    REQUIRE(reach.ExtentZ() == Catch::Approx(200.0f).margin(1e-3f));
    WaterBodyRegistry reg;
    reg.Add(reach);
    REQUIRE(reg.Find(5.0f, -180.0f) != nullptr);
    REQUIRE(reg.Find(-60.0f, 0.0f) == nullptr);
}

TEST_CASE("Degenerate bodies contain nothing", "[World][Terrain][Water]")
{
    WaterBodyRegistry reg;
    reg.Add(Lake(0.0f, 0.0f, 0.0f, 10.0f, 0.0f));
    REQUIRE(reg.Find(0.0f, 0.0f) == nullptr);
}
