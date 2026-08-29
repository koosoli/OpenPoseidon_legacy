// The bounce coefficients are estimates and the header says so. What these
// tests pin down is what must hold WHATEVER they become: the direction a round
// leaves a surface, that a bounce never creates energy, and that lowering the
// tangential term is what makes a grenade sit down rather than run on -- which
// is the specific behaviour the review asked for.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Poseidon/World/Entities/Weapons/GrenadeFuse.hpp>

using namespace Poseidon;

namespace
{
GrenadeFuse::Settings Defaults() { return GrenadeFuse::Settings{}; }
const Vector3 Up(0.0f, 1.0f, 0.0f);
} // namespace

TEST_CASE("a round leaves the surface it struck", "[grenade]")
{
    GrenadeFuse::Settings s = Defaults();
    s.restSpeed = 0.0f; // never park it, so the direction is what is measured

    // Straight down onto level ground.
    const GrenadeFuse::BounceResult r = GrenadeFuse::Bounce(Vector3(0.0f, -20.0f, 0.0f), Up, s);
    CHECK_FALSE(r.atRest);
    // The one thing that must never happen: still heading into the ground.
    CHECK(r.speed.Y() > 0.0f);
}

TEST_CASE("a bounce cannot create speed", "[grenade]")
{
    GrenadeFuse::Settings s = Defaults();
    s.restSpeed = 0.0f;

    const Vector3 incoming(6.0f, -9.0f, 2.0f);
    const GrenadeFuse::BounceResult r = GrenadeFuse::Bounce(incoming, Up, s);
    // Both coefficients are below one, so this holds for every legal setting --
    // and it is the assertion that catches a sign or a factor going astray.
    CHECK(r.speed.Size() < incoming.Size());
}

TEST_CASE("the tangential term is what decides whether it runs on", "[grenade]")
{
    // Wetzer, 2026-08-29: a grenade is not a sphere and rolls far less. That
    // complaint is about this coefficient, so this is the test that would fail
    // if someone raised it back to a ball-like value.
    const Vector3 shallow(15.0f, -2.0f, 0.0f); // arriving almost flat

    GrenadeFuse::Settings sticky = Defaults();
    sticky.restSpeed = 0.0f;
    GrenadeFuse::Settings slippery = sticky;
    slippery.tangentialKeep = 0.9f;

    const float stuck = GrenadeFuse::Bounce(shallow, Up, sticky).speed.Size();
    const float slid = GrenadeFuse::Bounce(shallow, Up, slippery).speed.Size();
    CHECK(stuck < slid);
}

TEST_CASE("the shipped defaults settle a grenade rather than skate it", "[grenade]")
{
    // A hand grenade arriving flat at 15 m/s. With the value this shipped with
    // first (0.62) it kept about 9 m/s and slid metres; the point of the default
    // is that it does not. Deliberately loose -- this asserts the behaviour, not
    // a particular constant.
    const GrenadeFuse::BounceResult r = GrenadeFuse::Bounce(Vector3(15.0f, -2.0f, 0.0f), Up, Defaults());
    CHECK(r.speed.Size() < 6.0f);
}

TEST_CASE("a slow arrival is parked instead of simulated badly", "[grenade]")
{
    const GrenadeFuse::BounceResult r = GrenadeFuse::Bounce(Vector3(0.5f, -0.5f, 0.0f), Up, Defaults());
    CHECK(r.atRest);
    CHECK(r.speed.Size() == 0.0f);
}

TEST_CASE("reset restores the defaults", "[grenade]")
{
    GrenadeFuse::Get().tangentialKeep = 0.99f;
    GrenadeFuse::Get().enabled = false;
    GrenadeFuse::Reset();
    CHECK(GrenadeFuse::Get().tangentialKeep == GrenadeFuse::Settings{}.tangentialKeep);
    CHECK(GrenadeFuse::Get().enabled);
}
