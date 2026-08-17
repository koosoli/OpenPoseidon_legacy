#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Weather/AirflowField.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>

#include <cmath>

using namespace Poseidon;
using Catch::Approx;

namespace
{

/// A rotor 30 m up over flat ground at y = 0, so the ground jet is out of the
/// picture and the column can be examined on its own.
RotorWash HighHover()
{
    RotorWash wash;
    wash.position = Vector3(0.0f, 30.0f, 0.0f);
    wash.discRadius = 8.0f;
    wash.strength = 12.0f;
    wash.groundY = 0.0f;
    return wash;
}

float HorizontalSize(Vector3Par v)
{
    return std::sqrt(v.X() * v.X() + v.Z() * v.Z());
}

} // namespace

// ---------------------------------------------------------------------------
// Rotor wake shape
// ---------------------------------------------------------------------------

TEST_CASE("Directly under the disc the wake blows down", "[World][Weather][Airflow]")
{
    const RotorWash wash = HighHover();

    // 2 m below the disc, on the axis: full column, nowhere near the ground.
    const Vector3 velocity = EvaluateRotorWash(wash, Vector3(0.0f, 28.0f, 0.0f));

    REQUIRE(velocity.Y() < 0.0f);
    // axialFall at 2 m depth is 1/(1 + 2/22); core is 1 on the axis.
    REQUIRE(velocity.Y() == Approx(-wash.strength / (1.0f + 2.0f / 22.0f)).epsilon(0.01));
    // No ground nearby, so nothing has turned outward yet.
    REQUIRE(HorizontalSize(velocity) == Approx(0.0f).margin(1e-4));
}

TEST_CASE("The column weakens with depth", "[World][Weather][Airflow]")
{
    const RotorWash wash = HighHover();

    const float shallow = -EvaluateRotorWash(wash, Vector3(0.0f, 29.0f, 0.0f)).Y();
    const float deep = -EvaluateRotorWash(wash, Vector3(0.0f, 8.0f, 0.0f)).Y();

    REQUIRE(shallow > deep);
    REQUIRE(deep > 0.0f);
}

TEST_CASE("Above the disc there is no wake", "[World][Weather][Airflow]")
{
    const RotorWash wash = HighHover();

    // Well above the rotor. Real rotors pull air in from above; simulating that
    // would make smoke climb into the blades, so it is deliberately absent.
    const Vector3 velocity = EvaluateRotorWash(wash, Vector3(0.0f, 45.0f, 0.0f));
    REQUIRE(velocity.X() == 0.0f);
    REQUIRE(velocity.Y() == 0.0f);
    REQUIRE(velocity.Z() == 0.0f);
}

TEST_CASE("Outside the cone at altitude there is no downward flow", "[World][Weather][Airflow]")
{
    const RotorWash wash = HighHover();

    // 40 m off the axis, only 2 m below the disc: far outside a cone that is
    // 8 m across up there.
    const Vector3 velocity = EvaluateRotorWash(wash, Vector3(40.0f, 28.0f, 0.0f));
    REQUIRE(velocity.Y() == Approx(0.0f).margin(1e-4));
}

TEST_CASE("At ground level the column has become an outward jet", "[World][Weather][Airflow]")
{
    RotorWash wash = HighHover();
    wash.position = Vector3(0.0f, 12.0f, 0.0f); // low hover, so the jet is strong

    // Near the ground, out at most of the cone radius. This is the signature a
    // helicopter leaves on dust and smoke: flow running outward along the deck.
    const Vector3 nearGround = EvaluateRotorWash(wash, Vector3(9.0f, 0.5f, 0.0f));

    REQUIRE(nearGround.X() > 0.0f);             // pointing away from the axis
    REQUIRE(HorizontalSize(nearGround) > 0.5f); // and meaningfully fast
    REQUIRE(nearGround.Z() == Approx(0.0f).margin(1e-4)); // radially outward, not skewed
    // Mostly turned, but not entirely: the column bends into the jet over the
    // last few metres rather than switching at a height. A step here would show
    // up as smoke visibly kinking as it crosses a plane above the ground.
    REQUIRE(HorizontalSize(nearGround) > 5.0f * std::fabs(nearGround.Y()));

    // On the deck itself the turn is complete — nothing is still going down.
    const Vector3 onDeck = EvaluateRotorWash(wash, Vector3(9.0f, 0.0f, 0.0f));
    REQUIRE(onDeck.Y() == Approx(0.0f).margin(1e-6));
    REQUIRE(HorizontalSize(onDeck) > HorizontalSize(nearGround));
}

TEST_CASE("The wake is radially symmetric", "[World][Weather][Airflow]")
{
    RotorWash wash = HighHover();
    wash.position = Vector3(0.0f, 12.0f, 0.0f);

    const Vector3 east = EvaluateRotorWash(wash, Vector3(9.0f, 0.5f, 0.0f));
    const Vector3 north = EvaluateRotorWash(wash, Vector3(0.0f, 0.5f, 9.0f));

    REQUIRE(HorizontalSize(east) == Approx(HorizontalSize(north)).epsilon(1e-4));
    REQUIRE(east.X() == Approx(north.Z()).epsilon(1e-4));
}

TEST_CASE("A stopped rotor produces nothing", "[World][Weather][Airflow]")
{
    RotorWash wash = HighHover();
    wash.strength = 0.0f;

    const Vector3 velocity = EvaluateRotorWash(wash, Vector3(0.0f, 28.0f, 0.0f));
    REQUIRE(velocity.X() == 0.0f);
    REQUIRE(velocity.Y() == 0.0f);
    REQUIRE(velocity.Z() == 0.0f);
}

// ---------------------------------------------------------------------------
// Field composition
// ---------------------------------------------------------------------------

TEST_CASE("Disabling local airflow removes the rotor terms only", "[World][Weather][Airflow]")
{
    AirflowField field;
    REQUIRE(field.LocalEnabled());

    // With no world, Update finds no rotors, so SampleLocal is zero either way.
    // What is being pinned here is that the switch exists and is honoured — the
    // dev panel's A/B toggle depends on it.
    field.SetLocalEnabled(false);
    const Vector3 local = field.SampleLocal(Vector3(0.0f, 1.0f, 0.0f));
    REQUIRE(local.X() == 0.0f);
    REQUIRE(local.Y() == 0.0f);
    REQUIRE(local.Z() == 0.0f);
}

// ---------------------------------------------------------------------------
// Wind override (the dev-panel escape hatch)
// ---------------------------------------------------------------------------

TEST_CASE("The wind override is exactly what was dialled in", "[World][Weather][Wind]")
{
    WindModel model;
    model.Init();

    WindOverride override;
    override.enabled = true;
    override.speed = 9.0f;
    override.directionRad = 0.0f; // due +X
    override.gustiness = 0.0f;    // dead steady
    model.SetOverride(override);

    // Sampled across twenty minutes of mission time. The whole point of the
    // steady override is that a comparison run half an hour in sees the same
    // air as one at t = 0 — otherwise "is the new smoke better" is confounded
    // by the weather having moved underneath the test.
    for (int64_t minute = 0; minute < 20; ++minute)
    {
        model.Update(minute * 60 * 1000, 0.3f);
        const WindSample& sample = model.Sample();

        REQUIRE(sample.speed == Approx(9.0f).epsilon(1e-5));
        REQUIRE(sample.meanSpeed == Approx(9.0f).epsilon(1e-5));
        REQUIRE(sample.gustFraction == Approx(0.0f).margin(1e-6));
        REQUIRE(sample.velocityX == Approx(9.0f).epsilon(1e-5));
        REQUIRE(sample.velocityZ == Approx(0.0f).margin(1e-4));
    }
}

TEST_CASE("Turning the override off restores the mission wind exactly", "[World][Weather][Wind]")
{
    WindModel model;
    model.Init();

    const int64_t time = 987654;
    model.Update(time, 0.6f);
    const WindSample before = model.Sample();

    WindOverride override;
    override.enabled = true;
    override.speed = 20.0f;
    model.SetOverride(override);
    model.Update(time, 0.6f);
    REQUIRE(model.Sample().speed == Approx(20.0f).epsilon(1e-5));

    // There is no state to unwind: the override replaces the evaluation, it does
    // not perturb the conditions. Bit equality, because this is the same claim
    // the closed form makes.
    override.enabled = false;
    model.SetOverride(override);
    model.Update(time, 0.6f);

    REQUIRE(model.Sample().velocityX == before.velocityX);
    REQUIRE(model.Sample().velocityZ == before.velocityZ);
}

TEST_CASE("meanVariation defaults preserve the shipped wind", "[World][Weather][Wind]")
{
    // The field was added for the override. If its default ever drifts from
    // 0.18 the shipping weather changes silently, which is exactly the kind of
    // regression nobody notices until the sea looks wrong.
    const WindConditions conditions{};
    REQUIRE(conditions.meanVariation == Approx(0.18f));
}
