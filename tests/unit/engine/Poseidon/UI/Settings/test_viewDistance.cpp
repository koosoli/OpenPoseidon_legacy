#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Poseidon/UI/Settings/GameSettingsConfig.hpp"
#include "Poseidon/UI/Settings/ViewDistance.hpp"

#include <optional>

using Poseidon::GameSettingsConfig;
using Poseidon::ViewDistanceLimits;
using Poseidon::ViewDistanceResolver;
using Poseidon::ViewDistances;

TEST_CASE("ViewDistanceResolver EffectiveView picks mission vs preferred", "[Settings][ViewDistance]")
{
    // respect off: always the preferred (engine/user) value, mission ignored
    CHECK(ViewDistanceResolver::EffectiveView(1500.0f, false, 600.0f) == Catch::Approx(1500.0f));
    // respect on + mission present: the mission value
    CHECK(ViewDistanceResolver::EffectiveView(1500.0f, true, 600.0f) == Catch::Approx(600.0f));
    // respect on + no mission value: the preferred value
    CHECK(ViewDistanceResolver::EffectiveView(900.0f, true, std::nullopt) == Catch::Approx(900.0f));
    // clamped to [100, 50000] (both the preferred and the mission value)
    CHECK(ViewDistanceResolver::EffectiveView(50.0f, false, std::nullopt) == Catch::Approx(100.0f));
    CHECK(ViewDistanceResolver::EffectiveView(99999.0f, false, std::nullopt) == Catch::Approx(50000.0f));
    CHECK(ViewDistanceResolver::EffectiveView(900.0f, true, 99999.0f) == Catch::Approx(50000.0f));
    // The default limits must agree with the settings clamp, or a value the Options page
    // accepts gets silently cut down by the resolver a moment later (VD-001).
    CHECK(ViewDistanceLimits{}.maxView == Catch::Approx(GameSettingsConfig::kMaxViewDistance));
    CHECK(ViewDistanceLimits{}.minView == Catch::Approx(GameSettingsConfig::kMinViewDistance));
}

TEST_CASE("ViewDistanceResolver Derive scales objects and shadows with the view distance", "[Settings][ViewDistance]")
{
    // OFP ratios: objects = 2/3 VD, shadows = 5/18 VD (capped 500), shadow <= objects.
    {
        const ViewDistances d = ViewDistanceResolver::Derive(2000.0f);
        CHECK(d.view == Catch::Approx(2000.0f));
        CHECK(d.objects == Catch::Approx(1333.333f).margin(0.5f)); // 2000 * 2/3
        CHECK(d.shadows == Catch::Approx(500.0f));                 // 2000*5/18=556 -> cap 500
    }
    {
        const ViewDistances d = ViewDistanceResolver::Derive(900.0f);
        CHECK(d.objects == Catch::Approx(600.0f));
        CHECK(d.shadows == Catch::Approx(250.0f));
    }
    {
        const ViewDistances d = ViewDistanceResolver::Derive(600.0f);
        CHECK(d.objects == Catch::Approx(400.0f));
        CHECK(d.shadows == Catch::Approx(166.667f).margin(0.5f)); // 600*5/18
    }
    {
        const ViewDistances d = ViewDistanceResolver::Derive(4800.0f);
        CHECK(d.objects == Catch::Approx(3200.0f)); // 4800*2/3, under the 5000 cap
    }
    {
        // The object cap binds above 7500 m of view (7500*2/3 = 5000), so a 10 km view --
        // draws terrain to 10 km and simulation objects to 5 km, not 6.7 km.
        const ViewDistances d = ViewDistanceResolver::Derive(10000.0f);
        CHECK(d.view == Catch::Approx(10000.0f));
        CHECK(d.objects == Catch::Approx(5000.0f));
        CHECK(d.shadows == Catch::Approx(500.0f)); // shadowCap, deliberately NOT raised
    }
}

TEST_CASE("ViewDistanceResolver Derive floors tiny distances and keeps shadow within objects",
          "[Settings][ViewDistance]")
{
    const ViewDistances d = ViewDistanceResolver::Derive(100.0f);
    CHECK(d.view == Catch::Approx(100.0f));
    CHECK(d.objects == Catch::Approx(100.0f)); // 100*2/3=67 -> floor 100
    CHECK(d.shadows == Catch::Approx(50.0f));  // 100*5/18=28 -> floor 50; clamped to objects
    CHECK(d.shadows <= d.objects);
}

TEST_CASE("ViewDistanceResolver Resolve is EffectiveView then Derive", "[Settings][ViewDistance]")
{
    // mission 2000 respected -> view 2000, objects 1333 (binocular case)
    const ViewDistances d = ViewDistanceResolver::Resolve(900.0f, true, 2000.0f);
    CHECK(d.view == Catch::Approx(2000.0f));
    CHECK(d.objects == Catch::Approx(1333.333f).margin(0.5f));
    // respect off -> preferred 900 -> objects 600 (mission 2000 ignored)
    const ViewDistances d2 = ViewDistanceResolver::Resolve(900.0f, false, 2000.0f);
    CHECK(d2.view == Catch::Approx(900.0f));
    CHECK(d2.objects == Catch::Approx(600.0f));
}

TEST_CASE("ViewDistanceResolver honours custom limits", "[Settings][ViewDistance]")
{
    ViewDistanceLimits lim;
    lim.objectRatio = 1.0f; // objects = full view distance
    lim.objectCap = 5000.0f;
    const ViewDistances d = ViewDistanceResolver::Derive(2000.0f, lim);
    CHECK(d.objects == Catch::Approx(2000.0f));
}
