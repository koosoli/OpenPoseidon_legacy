#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/Graphics/Rendering/Effects/UnderwaterBubbles.hpp>
#include <Poseidon/World/Entities/Weapons/UnderwaterBallistics.hpp>
#include <cmath>
#include <vector>

using namespace Poseidon;

namespace
{
// Integrates a round under water at 60 Hz with the production step; returns the
// distance at which it is spent and the energy share left after `probe` metres.
float SpentDistance(float mass, float calibre, float v0, float probe, float& energyAtProbe)
{
    const float k = Underwater::DragK(mass, calibre, v0);
    float v = v0, x = 0.0f;
    energyAtProbe = -1.0f;
    for (int i = 0; i < 600 && v >= Underwater::RetireSpeed; ++i)
    {
        float d = 0.0f, v1 = 0.0f;
        Underwater::Advance(v, k, 1.0f / 60.0f, d, v1);
        if (energyAtProbe < 0.0f && x + d >= probe)
            energyAtProbe = Underwater::EnergyScale(v * std::exp(-k * (probe - x)), v0);
        x += d;
        v = v1;
    }
    return x;
}
}

TEST_CASE("Rifle rounds are spent within about a metre of water, pistol rounds go further", "[underwater]")
{
    float rifleAtHalf = 0, rifleAtMetre = 0, pistolAtMetre = 0, heavyAtMetre = 0;
    const float rifle = SpentDistance(0.004f, 0.00556f, 900.0f, 0.5f, rifleAtHalf);
    SpentDistance(0.004f, 0.00556f, 900.0f, 1.0f, rifleAtMetre);
    const float heavy = SpentDistance(0.0095f, 0.00782f, 840.0f, 1.0f, heavyAtMetre);
    const float pistol = SpentDistance(0.0075f, 0.009f, 360.0f, 1.0f, pistolAtMetre);
    REQUIRE(rifle > 0.8f);
    REQUIRE(rifle < 1.6f);
    REQUIRE(heavy < 2.0f);
    REQUIRE(pistol > rifle);
    REQUIRE(pistol < 3.0f);
    REQUIRE(rifleAtHalf < 0.1f);     // half a metre down: under a tenth of the energy
    REQUIRE(rifleAtMetre < 0.01f);   // a metre down: harmless
    REQUIRE(pistolAtMetre > rifleAtMetre);
}

TEST_CASE("The exact drag step agrees with the closed form for any tick", "[underwater]")
{
    const float k = Underwater::DragK(0.004f, 0.00556f, 900.0f);
    float d1 = 0, v1 = 0;
    Underwater::Advance(900.0f, k, 0.02f, d1, v1);
    // Two half steps land in the same place as one whole step.
    float a = 0, va = 0, b = 0, vb = 0;
    Underwater::Advance(900.0f, k, 0.01f, a, va);
    Underwater::Advance(va, k, 0.01f, b, vb);
    REQUIRE(a + b == Catch::Approx(d1).epsilon(1e-4));
    REQUIRE(vb == Catch::Approx(v1).epsilon(1e-4));
    // v = v0 exp(-k x) along the path.
    REQUIRE(v1 == Catch::Approx(900.0f * std::exp(-k * d1)).epsilon(1e-3));
    float d = 0, v = 0;
    Underwater::Advance(0.0f, k, 0.02f, d, v);
    REQUIRE(d == 0.0f);
    REQUIRE(v == 0.0f);
}

TEST_CASE("A grenade sinks at a few metres per second", "[underwater]")
{
    float vx = 6.0f, vy = -4.0f, vz = 0.0f;
    for (int i = 0; i < 300; ++i)
        Underwater::SinkStep(vx, vy, vz, Underwater::GrenadeDragK, Underwater::GrenadeSinkAccel, 1.0f / 60.0f);
    REQUIRE(std::fabs(vx) < 0.05f);
    REQUIRE(vy < -1.5f);
    REQUIRE(vy > -4.0f); // terminal sink speed, not free fall
}

TEST_CASE("An underwater blast reaches swimmers further and spares what is above", "[underwater]")
{
    Underwater::BlastShaping shaping;
    REQUIRE_FALSE(Underwater::ShapeUnderwaterBlast(0.0f, true, shaping));
    REQUIRE_FALSE(Underwater::ShapeUnderwaterBlast(-5.0f, true, shaping));
    REQUIRE(Underwater::ShapeUnderwaterBlast(2.0f, true, shaping));
    REQUIRE(shaping.rangeScale > 1.5f);
    REQUIRE(shaping.hitScale == 1.0f);
    REQUIRE(Underwater::ShapeUnderwaterBlast(2.0f, false, shaping));
    REQUIRE(shaping.hitScale < 0.5f);
}

TEST_CASE("Bullet bubble trails: streak first, then rising bubbles that fade", "[underwater]")
{
    render::BubbleTrail trail;
    trail.from = {10, -0.1f, 20};
    trail.to = {10, -0.8f, 21};
    trail.speed = 800.0f;
    trail.surfaceY = 0.0f;
    trail.birthMs = 1000;
    trail.seed = 9;
    std::vector<render::BubbleSprite> early, later;
    render::BubbleTrailSprites(trail, 1010.0, [&](const render::BubbleSprite& s) { early.push_back(s); });
    render::BubbleTrailSprites(trail, 2000.0, [&](const render::BubbleSprite& s) { later.push_back(s); });
    REQUIRE(!early.empty());
    REQUIRE(early.front().kind == render::BubbleKind::Cavity);
    REQUIRE(!later.empty());
    float lowest = 1e9f;
    for (const auto& s : later)
    {
        REQUIRE(s.kind == render::BubbleKind::Bubble);
        REQUIRE(s.radius < 0.02f);
        lowest = std::min(lowest, s.head.y);
    }
    REQUIRE(lowest > -0.8f - 0.05f); // they rose
    for (const auto& s : later) REQUIRE(s.head.y <= trail.surfaceY);
    std::size_t gone = 0;
    render::BubbleTrailSprites(trail, 1000.0 + render::BubbleTrailLifetimeMs, [&](const render::BubbleSprite&) { ++gone; });
    REQUIRE(gone == 0);
}

TEST_CASE("Blast bubbles never pass the surface and include flash and gas bubble", "[underwater]")
{
    render::BubbleBlast blast;
    blast.centre = {0, -3, 0};
    blast.radius = 1.5f;
    blast.surfaceY = 0.0f;
    blast.birthMs = 0;
    blast.seed = 4;
    bool flash = false, gas = false;
    render::BubbleBlastSprites(blast, 10.0, [&](const render::BubbleSprite& s) {
        flash |= s.kind == render::BubbleKind::Flash;
        gas |= s.kind == render::BubbleKind::GasBubble;
    });
    REQUIRE(flash);
    REQUIRE(gas);
    for (double t = 100.0; t < render::BubbleBlastLifetimeMs; t += 250.0)
        render::BubbleBlastSprites(blast, t, [&](const render::BubbleSprite& s) { REQUIRE(s.head.y <= 0.0f); });
}
