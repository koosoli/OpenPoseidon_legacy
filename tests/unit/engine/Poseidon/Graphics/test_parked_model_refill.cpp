#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Core/ParkedModelRefill.hpp>

using Poseidon::render::CanDeferParkedImage;
using Poseidon::render::CanRefillParkedImageActivation;
using Poseidon::render::ParkedImageBinding;
using Poseidon::render::ParkedRefillContext;
using Poseidon::render::RefilledParkedImageMatches;

namespace
{
constexpr ParkedRefillContext inactiveParked{true, true, false, 0};
constexpr ParkedImageBinding evictedImage{91, 7, 0, 7, true, false, true};
}

TEST_CASE("Only an inactive bounded parked owner may defer an evicted leased image", "[wgpu][parked-refill]")
{
    REQUIRE(CanDeferParkedImage(inactiveParked, evictedImage));
    SECTION("Default off retains the prior destruction path")
    {
        auto context = inactiveParked;
        context.enabled = false;
        REQUIRE_FALSE(CanDeferParkedImage(context, evictedImage));
    }
    SECTION("A zero-ref registration outside the park LRU is insufficient")
    {
        auto context = inactiveParked;
        context.parked = false;
        REQUIRE_FALSE(CanDeferParkedImage(context, evictedImage));
    }
    SECTION("Referenced or liveness-marked models cannot hide missing images")
    {
        auto context = inactiveParked;
        context.references = 1;
        REQUIRE_FALSE(CanDeferParkedImage(context, evictedImage));
        REQUIRE_FALSE(CanRefillParkedImageActivation(context, evictedImage));
        context.references = 0;
        context.live = true;
        REQUIRE_FALSE(CanDeferParkedImage(context, evictedImage));
        // The returning camera can mark a STILL parked zero-ref model live.
        // Activation repairs it before exposing any instance or unparking.
        REQUIRE(CanRefillParkedImageActivation(context, evictedImage));
    }
}

TEST_CASE("An image handle change is not a material-slot identity proof", "[wgpu][parked-refill]")
{
    auto image = evictedImage;
    image.capturedLease = 0;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
    image = evictedImage;
    image.currentLease = 8;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
    image.currentLease = 0;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
    image = evictedImage;
    image.capturedHandle = 0;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
    image = evictedImage;
    image.currentHandle = 102;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
}

TEST_CASE("Missing sources and dynamic images remain outside parked-image dormancy", "[wgpu][parked-refill]")
{
    auto image = evictedImage;
    image.texturePresent = false;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
    image = evictedImage;
    image.reloadableSource = false;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
    image = evictedImage;
    image.dynamic = true;
    REQUIRE_FALSE(CanDeferParkedImage(inactiveParked, image));
}

TEST_CASE("Activation proves the new image is published into the original baked slot", "[wgpu][parked-refill]")
{
    REQUIRE(CanDeferParkedImage(inactiveParked, evictedImage));
    // A replacement image has a different numeric handle but the same slot.
    REQUIRE(RefilledParkedImageMatches(evictedImage, 102, 102, 7));
    // Failed disk reads, lost slot leases, unpublished images and mismatched
    // return values must take the destroy/re-register fallback before visibility.
    REQUIRE_FALSE(RefilledParkedImageMatches(evictedImage, 0, 0, 7));
    REQUIRE_FALSE(RefilledParkedImageMatches(evictedImage, 102, 102, 8));
    REQUIRE_FALSE(RefilledParkedImageMatches(evictedImage, 102, 102, 0));
    REQUIRE_FALSE(RefilledParkedImageMatches(evictedImage, 102, 0, 7));
    REQUIRE_FALSE(RefilledParkedImageMatches(evictedImage, 102, 103, 7));
    auto neverBaked = evictedImage;
    neverBaked.capturedHandle = 0;
    REQUIRE_FALSE(RefilledParkedImageMatches(neverBaked, 102, 102, 7));
}
