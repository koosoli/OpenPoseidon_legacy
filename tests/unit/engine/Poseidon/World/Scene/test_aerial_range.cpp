// FAR-001: the far-plane / fog-range resolver.
//
// The interesting cases here are not the arithmetic -- they are the INERTNESS
// guarantees. The whole change is defensible only if a ground-level frame is
// provably untouched, and "provably" has to mean something stronger than a
// screenshot that looks the same: the resolver must hand back the caller's own
// number, exactly, so that nothing downstream can round differently.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Poseidon/World/Scene/Camera/AerialRange.hpp"

using Poseidon::Aerial::AerialRange;
using Poseidon::Aerial::AerialRangeSettings;
using Poseidon::Aerial::Resolve;

namespace
{
// An explicit settings block rather than the live one: the live one reads
// environment overrides on first use, and a test that silently depends on the
// developer's shell is not a test.
AerialRangeSettings Defaults()
{
    return AerialRangeSettings{};
}
} // namespace

TEST_CASE("AerialRange is exactly inert at and below the start altitude", "[World][AerialRange]")
{
    const AerialRangeSettings s = Defaults();
    const float fogBase = 880.0f; // the default 900 m view distance, less MAX_FOG's 20

    for (const float alt : {-50.0f, 0.0f, 1.7f, 100.0f, s.startAlt})
    {
        const AerialRange r = Resolve(fogBase, alt, true, s);
        INFO("altitude " << alt);
        CHECK_FALSE(r.active);
        // Bit-for-bit, not approximately: this is the ground-play guarantee.
        CHECK(r.farPlane == fogBase);
        CHECK(r.fogMaxRange == fogBase);
    }
}

TEST_CASE("AerialRange stays inert while the view distance is still the longer reach",
          "[World][AerialRange]")
{
    const AerialRangeSettings s = Defaults();
    // A player who has deliberately set a 10 km view distance must not LOSE reach
    // by climbing. At 1 km up the altitude law asks for 3 km, which is shorter, so
    // the policy must stand down rather than shorten anything.
    const AerialRange r = Resolve(9980.0f, 1000.0f, true, s);
    CHECK_FALSE(r.active);
    CHECK(r.farPlane == 9980.0f);
}

TEST_CASE("AerialRange opens with altitude and keeps the far plane outside the fog",
          "[World][AerialRange]")
{
    const AerialRangeSettings s = Defaults();
    const AerialRange r = Resolve(880.0f, 5000.0f, true, s);

    REQUIRE(r.active);
    // Well past fullAlt, so the ramp is saturated and the reach is the plain
    // altitude * reachFactor.
    CHECK(r.fogMaxRange == Catch::Approx(5000.0f * s.reachFactor));
    // The far plane must be strictly beyond the haze, or terrain is clipped at a
    // visible circular edge instead of dissolving into it.
    CHECK(r.farPlane > r.fogMaxRange);
    CHECK(r.farPlane == Catch::Approx(r.fogMaxRange * s.clipMargin));
}

TEST_CASE("AerialRange ramps in monotonically with no step", "[World][AerialRange]")
{
    const AerialRangeSettings s = Defaults();
    float previous = 0.0f;
    // Walk the transition band. Every sample must be at least as long as the last
    // -- a non-monotonic reach would read in flight as the haze breathing.
    for (float alt = s.startAlt; alt <= s.fullAlt + 2000.0f; alt += 25.0f)
    {
        const AerialRange r = Resolve(880.0f, alt, true, s);
        const float reach = r.active ? r.fogMaxRange : 880.0f;
        INFO("altitude " << alt << " reach " << reach);
        CHECK(reach >= previous - 0.001f);
        previous = reach;
    }
}

TEST_CASE("AerialRange respects the two reach ceilings", "[World][AerialRange]")
{
    const AerialRangeSettings s = Defaults();

    // Infinite-far reversed-Z (wgpu): depth precision does not depend on the far
    // plane, so the generous ceiling applies.
    const AerialRange wgpu = Resolve(880.0f, 14000.0f, true, s);
    REQUIRE(wgpu.active);
    CHECK(wgpu.fogMaxRange == Catch::Approx(s.maxReach));

    // Finite forward-Z (GL33): precision IS a far/near ratio, so the far smaller
    // ceiling applies and the two backends must not come out the same.
    const AerialRange gl33 = Resolve(880.0f, 14000.0f, false, s);
    REQUIRE(gl33.active);
    CHECK(gl33.fogMaxRange == Catch::Approx(s.finiteFarReach));
    CHECK(gl33.fogMaxRange < wgpu.fogMaxRange);
}

TEST_CASE("AerialRange master switch restores the single-number behaviour", "[World][AerialRange]")
{
    AerialRangeSettings s = Defaults();
    s.enabled = false;

    const AerialRange r = Resolve(880.0f, 14000.0f, true, s);
    CHECK_FALSE(r.active);
    CHECK(r.farPlane == 880.0f);
    CHECK(r.fogMaxRange == 880.0f);
}
