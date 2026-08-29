// Step() needs a live world, a landscape and a real entity, so what is unit
// testable here is the part that decides whether anything happens at all. That
// part matters more than it looks: this feature changes how a shipping object
// behaves, and "off unless asked" is the property that keeps it from doing so
// behind the owner's back.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Physics/LooseObjects.hpp>

using namespace Poseidon;

TEST_CASE("loose-object physics is off by default", "[physics][loose]")
{
    // The whole safety argument rests on this. Two questions are open -- who owns
    // a barrel in multiplayer, and what a moved one does to a saved game -- so a
    // default of ON would ship both of them unanswered.
    CHECK_FALSE(LooseObjects::Settings{}.enabled);
}

TEST_CASE("the defaults describe an object, not a placeholder", "[physics][loose]")
{
    const LooseObjects::Settings s;
    // CfgVehicles gives a Thing no mass, so these are chosen rather than read.
    // Pinning the ORDER of magnitude catches a zero or a stray factor, which is
    // what would actually go wrong, without pretending 30 kg is a measurement.
    CHECK(s.mass > 5.0f);
    CHECK(s.mass < 200.0f);
    CHECK(s.friction > 0.0f);
    // A barrel that bounces is a beach ball. This must stay near zero.
    CHECK(s.restitution < 0.3f);
}

TEST_CASE("reset restores every field", "[physics][loose]")
{
    LooseObjects::Settings& live = LooseObjects::Get();
    live.enabled = true;
    live.mass = 999.0f;
    live.friction = 0.0f;
    live.restitution = 1.0f;

    LooseObjects::Reset();

    const LooseObjects::Settings fresh;
    CHECK(live.enabled == fresh.enabled);
    CHECK(live.mass == fresh.mass);
    CHECK(live.friction == fresh.friction);
    CHECK(live.restitution == fresh.restitution);
}

TEST_CASE("releasing an invalid body is harmless", "[physics][loose]")
{
    // The usual case: almost every Thing never gets a body, and its destructor
    // still calls Release. If that needed a live physics world, every mission
    // teardown would be a crash waiting for the feature to be switched on once.
    Physics::BodyId none;
    LooseObjects::Release(none);
    CHECK_FALSE(none.IsValid());
}
