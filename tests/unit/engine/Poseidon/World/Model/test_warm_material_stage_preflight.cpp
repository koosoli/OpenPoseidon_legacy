#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/WarmMaterialStagePreflightPolicy.hpp>

TEST_CASE("Warm material preflight shares two failure-charged source attempts across models", "[warm-material-preflight]")
{
    using Policy = Poseidon::Streaming::WarmMaterialStagePreflightPolicy;
    size_t attempts = 2, work = 256;
    REQUIRE(Policy::ReserveMissingSource(true, false, attempts, work));
    // First actual source load fails or throws: its reservation remains consumed.
    CHECK(attempts == 1); CHECK(work == 255);
    REQUIRE(Policy::ReserveMissingSource(true, false, attempts, work));
    CHECK(attempts == 0); CHECK(work == 254);
    CHECK_FALSE(Policy::ReserveMissingSource(true, false, attempts, work));
    CHECK(work == 254);
}
TEST_CASE("Warm material preflight cannot reload existing headers or bypass OFF and metadata limits", "[warm-material-preflight]")
{
    using Policy = Poseidon::Streaming::WarmMaterialStagePreflightPolicy;
    size_t attempts = 2, work = 256;
    CHECK_FALSE(Policy::ReserveMissingSource(false, false, attempts, work));
    CHECK_FALSE(Policy::ReserveMissingSource(true, true, attempts, work)); // Unknown existing Init binding stays Unknown.
    CHECK(attempts == 2); CHECK(work == 256);
    work = 0;
    CHECK_FALSE(Policy::ReserveMissingSource(true, false, attempts, work));
    CHECK(attempts == 2);
    work = 1; // Must retain the subsequent initialized-source inspection visit.
    CHECK_FALSE(Policy::ReserveMissingSource(true, false, attempts, work));
    CHECK(work == 1); CHECK(attempts == 2);
    work = 256; attempts = 3;
    CHECK_FALSE(Policy::ReserveMissingSource(true, false, attempts, work));
    CHECK(attempts == 3); CHECK(work == 256);
}

TEST_CASE("A registered no-retained model saves both missing specular attempts for stages with consumers",
    "[warm-material-preflight]")
{
    using Policy = Poseidon::Streaming::WarmMaterialStagePreflightPolicy;
    size_t attempts = 2, work = 256;
    // The caller supplies this negative ownership fact only for an absent
    // SpecularDetail bank entry on a completed NoOwnedSection registration.
    CHECK_FALSE(Policy::ReserveMissingSource(true, false, attempts, work, true));
    CHECK_FALSE(Policy::ReserveMissingSource(true, false, attempts, work, true));
    CHECK(attempts == 2); CHECK(work == 256);
    // The same direct-drawn model can still consume NormalMap; it must retain
    // the ordinary shared attempt/visit budget and byte-identical fallback.
    REQUIRE(Policy::ReserveMissingSource(true, false, attempts, work, false));
    CHECK(attempts == 1); CHECK(work == 255);
    REQUIRE(Policy::ReserveMissingSource(true, false, attempts, work, false));
    CHECK(attempts == 0); CHECK(work == 254);
    CHECK_FALSE(Policy::ReserveMissingSource(true, false, attempts, work, false));
    CHECK(work == 254);
}
