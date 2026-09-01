// The penetration model's constants are estimates, and the header says so. What
// these tests pin down is the part that is NOT an estimate: the ordering, the
// physics, and the failure directions. A wall must always cost more than a plank
// whatever the numbers become, and an unknown surface must behave like a wall
// rather than like paper.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Poseidon/World/Entities/Weapons/Penetration.hpp>

using namespace Poseidon;

namespace
{
// 5.56x45 M855: 4.0 g at 940 m/s, 5.56 mm.
constexpr float Mass = 0.004f;
constexpr float Speed = 940.0f;
constexpr float Calibre = 0.00556f;
} // namespace

TEST_CASE("glass and foliage pass a round through untouched", "[penetration]")
{
    // Reproduces the legacy behaviour exactly rather than approximating it with a
    // very small resistance -- a token loss would be a number nobody could defend.
    const Penetration::Result r =
        Penetration::Compute(Penetration::Lookup("ca\\data\\sklo_okno.paa"), Speed, Mass, Calibre, 0.02f);
    CHECK(r.penetrated);
    CHECK(r.exitSpeed == Speed);
    CHECK(r.energyLost == 0.0f);
}

TEST_CASE("a thin plank is penetrated and a thick wall is not", "[penetration]")
{
    const Penetration::Material& wood = Penetration::Lookup("ca\\data\\drevo_prkna.paa");
    const Penetration::Material& concrete = Penetration::Lookup("ca\\data\\beton_zed.paa");

    const Penetration::Result plank = Penetration::Compute(wood, Speed, Mass, Calibre, 0.02f);
    CHECK(plank.penetrated);
    CHECK(plank.exitSpeed < Speed);   // it costs something
    CHECK(plank.exitSpeed > 0.0f);

    const Penetration::Result wall = Penetration::Compute(concrete, Speed, Mass, Calibre, 0.30f);
    CHECK_FALSE(wall.penetrated);
    CHECK(wall.exitSpeed == 0.0f);
}

TEST_CASE("thickness is what decides, not the material alone", "[penetration]")
{
    // The whole point of measuring a thickness: the SAME wall stops a shot that
    // crosses it at an angle while letting a square one through. A per-material
    // passthrough boolean cannot express this at all.
    const Penetration::Material& brick = Penetration::Lookup("ca\\data\\cihla_zed.paa");

    // 2 cm and 25 cm, well clear of the threshold on either side. The first
    // version used 5 cm, which is 2 mm from where these constants actually stop a
    // 5.56 -- so it was measuring my estimate rather than the principle, and
    // failed. A test of an ordering must not sit on the boundary of a number the
    // file itself calls an estimate.
    const Penetration::Result square = Penetration::Compute(brick, Speed, Mass, Calibre, 0.02f);
    const Penetration::Result glancing = Penetration::Compute(brick, Speed, Mass, Calibre, 0.25f);

    CHECK(square.penetrated);
    CHECK_FALSE(glancing.penetrated);
}

TEST_CASE("materials are ordered correctly whatever the constants become", "[penetration]")
{
    // The constants are estimates. Their ORDER is not, and this is the assertion
    // that survives someone retuning them: at one thickness, a round must keep
    // more speed through wood than brick, and more through brick than concrete.
    constexpr float T = 0.03f;
    const float     wood = Penetration::Compute(Penetration::Lookup("drevo"), Speed, Mass, Calibre, T).exitSpeed;
    const float     brick = Penetration::Compute(Penetration::Lookup("cihla"), Speed, Mass, Calibre, T).exitSpeed;
    const float     concrete = Penetration::Compute(Penetration::Lookup("beton"), Speed, Mass, Calibre, T).exitSpeed;

    CHECK(wood > brick);
    CHECK(brick > concrete);
}

TEST_CASE("an unrecognised surface behaves like a wall, not like paper", "[penetration]")
{
    // The failure direction matters more than the value. Most stock textures are
    // not in the table; if the default were soft, rounds would pass through most
    // of the map the moment this is switched on.
    const Penetration::Material& unknown = Penetration::Lookup("ca\\data\\something_nobody_listed.paa");
    CHECK_FALSE(Penetration::Compute(unknown, Speed, Mass, Calibre, 0.20f).penetrated);
}

TEST_CASE("energy is conserved, not invented", "[penetration]")
{
    const Penetration::Material& wood = Penetration::Lookup("drevo");
    const Penetration::Result    r = Penetration::Compute(wood, Speed, Mass, Calibre, 0.02f);

    const float before = 0.5f * Mass * Speed * Speed;
    const float after = 0.5f * Mass * r.exitSpeed * r.exitSpeed;
    // Whatever the constants are, what comes out plus what the material took must
    // equal what went in. A model that leaks energy would silently make rounds
    // faster or slower than either number says.
    CHECK_THAT(after + r.energyLost, Catch::Matchers::WithinRel(before, 1e-4f));
    CHECK(r.energyLost > 0.0f);
}

TEST_CASE("a slower round is stopped by what a fast one crosses", "[penetration]")
{
    const Penetration::Material& wood = Penetration::Lookup("drevo");
    constexpr float              T = 0.10f;
    CHECK(Penetration::Compute(wood, 940.0f, Mass, Calibre, T).penetrated);
    CHECK_FALSE(Penetration::Compute(wood, 120.0f, Mass, Calibre, T).penetrated);
}
