#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/CraterField.hpp>

using namespace Poseidon;
using Catch::Approx;

// BOOM-001: explosion craters as ground geometry.

TEST_CASE("A hand grenade scoops a shallow pit, not a shell crater", "[crater]")
{
    // HandGrenade: indirectHit 18, range 7 (owner, 2026-10-19: grenades made 1.6 m craters)
    const CraterShape g = CraterShapeFor(18.0f, 7.0f, 0.0f);
    REQUIRE(g.Valid());
    CHECK(g.radius < 0.4f);
    CHECK(g.depth < 0.15f);
    CHECK(BlastScale(18.0f) < 0.25f);
    // continuous at the shell: nothing at or above 150 changes
    CHECK(BlastScale(150.0f) == Approx(1.0f));
    CHECK(BlastScale(149.0f) == Approx(1.0f).margin(0.01));
}

TEST_CASE("Crater size follows the blast, anchored at the LAW / 120 mm shell", "[crater]")
{
    const CraterShape law = CraterShapeFor(150.0f, 4.0f, 0.0f);
    CHECK(law.radius == Approx(1.6f));
    CHECK(law.depth == Approx(1.0f));
    CHECK(law.lip > 0.1f);
    CHECK(law.ejecta == Approx(law.radius * 3.6f));
    // eight times the blast: twice the size (cube root)
    const CraterShape big = CraterShapeFor(1200.0f, 10.0f, 0.0f);
    CHECK(big.radius == Approx(law.radius * 2.0f));
    CHECK(big.depth == Approx(law.depth * 2.0f));
    CHECK(big.scorch >= law.scorch);
    // a burst above the ground digs less, an air burst nothing
    CHECK(CraterShapeFor(150.0f, 4.0f, 0.6f).depth < law.depth);
    CHECK_FALSE(CraterShapeFor(150.0f, 4.0f, 2.0f).Valid());
    CHECK_FALSE(CraterShapeFor(0.0f, 4.0f, 0.0f).Valid());
    CHECK_FALSE(CraterShapeFor(NAN, 4.0f, 0.0f).Valid());
}

TEST_CASE("A crater is a bowl with a raised lip, and nothing beyond its blanket changes", "[crater]")
{
    CraterField f;
    const CraterShape c = CraterShapeFor(150.0f, 4.0f, 0.0f);
    REQUIRE(f.Dig(100.0f, 200.0f, c, 7u));
    CHECK(f.Craters() == 1);
    // the centre is near the full depth, the rim is above the old ground, the far blanket is flat
    CHECK(f.HeightOffsetAt(100.0f, 200.0f) < -0.8f * c.depth);
    float rim = -1.0f;
    for (float r = 0.8f; r < 1.4f; r += 0.05f)
        rim = std::max(rim, f.HeightOffsetAt(100.0f + r * c.radius, 200.0f));
    CHECK(rim > 0.3f * c.lip);
    CHECK(f.HeightOffsetAt(100.0f + c.ejecta + 1.0f, 200.0f) == 0.0f);
    CHECK(f.HeightOffsetAt(150.0f, 250.0f) == 0.0f);
    // fresh soil over the bowl, none far away
    CHECK(f.SoilAt(100.0f, 200.0f) > 0.9f);
    CHECK(f.SoilAt(150.0f, 250.0f) == 0.0f);
    // the slope points down into the bowl
    const auto g = f.HeightGradientAt(100.0f + 0.5f * c.radius, 200.0f);
    CHECK(g[0] > 0.0f);
    // a second crater on top digs again instead of filling the first
    const float before = f.HeightOffsetAt(100.0f, 200.0f);
    REQUIRE(f.Dig(100.0f, 200.0f, c, 8u));
    CHECK(f.HeightOffsetAt(100.0f, 200.0f) <= before + 1e-5f);
    f.Reset();
    CHECK(f.Chunks() == 0);
    CHECK(f.HeightOffsetAt(100.0f, 200.0f) == 0.0f);
}

TEST_CASE("Crater snapshots pack heights and materials for the terrain shader", "[crater]")
{
    CraterField f;
    REQUIRE(f.Dig(64.0f, 64.0f, CraterShapeFor(150.0f, 4.0f, 0.0f), 3u));
    const int view = 4 + CraterField::WindowSize * CraterField::WindowSize;
    for (bool farView : {false, true})
    {
        const std::vector<float> data = f.Snapshot(64.0f, 64.0f, farView);
        REQUIRE(data.size() == size_t(view) * 2);
        CHECK(data[2] == (farView ? CraterField::FarCellSize : CraterField::CellSize));
        CHECK(data[3] == CraterField::MaxDepth);
        CHECK(data[view] == data[0]);
        CHECK(data[view + 1] == data[1]);
        // the cell holding the crater's centre is below zero; every material is a packed integer
        const float cell = data[2];
        const int ix = int((64.0f - data[0]) / cell), iz = int((64.0f - data[1]) / cell);
        // (the far view averages 4 x 4 fine cells: a 0.85 m bowl is shallower there)
        CHECK(data[4 + iz * CraterField::WindowSize + ix] < (farView ? -0.03f : -0.1f));
        bool packed = true;
        for (int i = 4; i < view; ++i)
        {
            const float m = data[view + i];
            packed &= m >= 0.0f && m <= 65535.0f && m == std::floor(m);
            packed &= data[i] >= -CraterField::MaxDepth && data[i] <= CraterField::MaxRise;
        }
        CHECK(packed);
    }
    // a window with no crater in it is switched off (limit 0)
    CHECK(f.Snapshot(5000.0f, 5000.0f, false)[3] == 0.0f);
    CHECK(CraterField::PackMaterial(1.0f, 1.0f) == 255.0f + 256.0f * 255.0f);
    CHECK(CraterField::PackMaterial(-1.0f, 2.0f) == 256.0f * 255.0f);
}

TEST_CASE("Grass gets the craters nearest the camera", "[crater]")
{
    CraterField f;
    for (int i = 0; i < 5; ++i)
        REQUIRE(f.Dig(100.0f * i, 0.0f, CraterShapeFor(150.0f, 4.0f, 0.0f), uint32_t(i)));
    const std::vector<float> list = f.NearestDiscs(390.0f, 0.0f, 2);
    REQUIRE(list.size() == 4 + 2 * 4);
    CHECK(list[0] == 2.0f);
    CHECK(list[4] == 400.0f); // nearest first
    CHECK(list[8] == 300.0f);
    CHECK(list[6] > 0.0f);    // a soil radius
    CHECK(f.NearestDiscs(0.0f, 0.0f, 0)[0] == 0.0f);
}
