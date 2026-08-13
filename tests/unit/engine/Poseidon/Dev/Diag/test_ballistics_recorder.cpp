// Unit tests for the ballistics diagnostic's data model.
//
// The store is deliberately engine-free (no World, no Vector3, no RString), so
// these tests exercise the parts with real logic — ring eviction, sample
// decimation, and the drop / lateral solve — without standing up a mission.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Poseidon/Dev/Diag/BallisticsTrackStore.hpp>
#include <Poseidon/Dev/Diag/DiagPause.hpp>

#include <cmath>

using Poseidon::Dev::BallisticsTrackStore;
using Poseidon::Dev::BallisticTerminus;
using Poseidon::Dev::BallisticTrack;

namespace
{
/// Begin a flat shot heading down +Z at `v0` m/s from the origin.
uint32_t BeginFlatShot(BallisticsTrackStore& store, float v0 = 900.0f, bool byPlayer = true)
{
    return store.Begin("B_762x51_Ball", "Alpha 1-1", byPlayer, 0.0f, 1.5f, 0.0f, 0.0f, 0.0f, v0, false, 0.0f, 0.0f);
}
} // namespace

TEST_CASE("Ballistics store retains muzzle state measured from the initial velocity", "[dev][ballistics]")
{
    BallisticsTrackStore store;
    const uint32_t id = BeginFlatShot(store, 850.0f);

    REQUIRE(id != 0);
    REQUIRE(store.Size() == 1);

    const BallisticTrack* track = store.Find(id);
    REQUIRE(track != nullptr);
    CHECK_THAT(track->muzzleSpeed, Catch::Matchers::WithinAbs(850.0, 1e-3));
    CHECK_THAT(track->dirZ, Catch::Matchers::WithinAbs(1.0, 1e-6));
    CHECK_THAT(track->dirX, Catch::Matchers::WithinAbs(0.0, 1e-6));
    CHECK(track->byPlayer);
    CHECK(track->terminus == BallisticTerminus::InFlight);
}

TEST_CASE("Ballistics store evicts the oldest shot and never exceeds capacity", "[dev][ballistics]")
{
    BallisticsTrackStore store;
    store.SetCapacity(4);

    uint32_t first = 0;
    uint32_t last = 0;
    for (int i = 0; i < 20; ++i)
    {
        const uint32_t id = BeginFlatShot(store);
        if (i == 0)
        {
            first = id;
        }
        last = id;
        CHECK(store.Size() <= 4);
    }

    CHECK(store.Size() == 4);
    // The first shot is long gone; the most recent one is still there.
    CHECK(store.Find(first) == nullptr);
    CHECK(store.Find(last) != nullptr);

    // Shrinking capacity evicts immediately rather than waiting for new shots.
    store.SetCapacity(2);
    CHECK(store.Size() == 2);
    CHECK(store.Find(last) != nullptr);

    store.Clear();
    CHECK(store.Size() == 0);
    CHECK(store.TotalSamples() == 0);
}

TEST_CASE("Ballistics sample budget is bounded by halving, not truncation", "[dev][ballistics]")
{
    BallisticsTrackStore store;
    store.SetMaxSamples(16);
    const uint32_t id = BeginFlatShot(store);

    // Feed far more samples than the budget along a straight line.
    for (int i = 0; i < 500; ++i)
    {
        const float z = static_cast<float>(i);
        store.AddSample(id, 0.0f, 1.5f, z, z * 0.001f);
    }

    const BallisticTrack* track = store.Find(id);
    REQUIRE(track != nullptr);
    CHECK(static_cast<int>(track->samples.size()) <= 16);
    CHECK(track->decimation > 1);

    // Truncation would have thrown the tail away.  Halving keeps it: the last
    // sample must still be near the end of the flight, not near the muzzle.
    REQUIRE(track->samples.size() >= 2);
    CHECK(track->samples.back().z > 400.0f);
    CHECK(track->samples.front().z == 0.0f);

    // Samples must remain in flight order after every halving.
    for (std::size_t i = 1; i < track->samples.size(); ++i)
    {
        CHECK(track->samples[i].z > track->samples[i - 1].z);
    }
}

TEST_CASE("Ballistics drop is measured against the line of departure", "[dev][ballistics]")
{
    BallisticsTrackStore store;
    const uint32_t id = BeginFlatShot(store, 900.0f);

    // A flat shot down +Z at 900 m/s under 9.81 m/s^2, no drag: y = -0.5*g*t^2.
    constexpr float g = 9.81f;
    for (int i = 0; i <= 100; ++i)
    {
        const float t = static_cast<float>(i) * 0.01f;
        const float z = 900.0f * t;
        const float y = 1.5f - 0.5f * g * t * t;
        store.AddSample(id, 0.0f, y, z, t);
    }

    const BallisticTrack* track = store.Find(id);
    REQUIRE(track != nullptr);

    // At t = 1 s the analytic drop is 0.5 * 9.81 = 4.905 m below the muzzle ray.
    CHECK_THAT(track->MaxDrop(), Catch::Matchers::WithinAbs(4.905, 0.01));
    CHECK_THAT(track->TimeOfFlight(), Catch::Matchers::WithinAbs(1.0, 1e-3));
    CHECK_THAT(track->StraightDistance(), Catch::Matchers::WithinAbs(900.013, 0.1));
    // Straight-line firing: no horizontal deviation at all.  This is the value
    // that stays ~0 while ballistics wind is off, and grows when it is on.
    CHECK_THAT(track->MaxLateral(), Catch::Matchers::WithinAbs(0.0, 1e-3));
    CHECK_THAT(track->TerminalSpeed(), Catch::Matchers::WithinAbs(900.1, 1.0));
}

TEST_CASE("Ballistics lateral deviation detects a crosswind push", "[dev][ballistics]")
{
    BallisticsTrackStore store;
    const uint32_t id = store.Begin("B_762x51_Ball", "Alpha 1-1", false, 0.0f, 1.5f, 0.0f, 0.0f, 0.0f, 900.0f,
                                    /*windActive*/ true, 9.0f, 0.0f);

    // Same flat shot, plus a steadily accumulating +X push.
    for (int i = 0; i <= 100; ++i)
    {
        const float t = static_cast<float>(i) * 0.01f;
        store.AddSample(id, 2.0f * t * t, 1.5f, 900.0f * t, t);
    }

    const BallisticTrack* track = store.Find(id);
    REQUIRE(track != nullptr);
    CHECK(track->windActive);
    // right = normalize(cross(up, dir)) = (dirZ, 0, -dirX) = (+1, 0, 0), so a
    // +X push reads as a positive lateral.
    CHECK_THAT(track->MaxLateral(), Catch::Matchers::WithinAbs(2.0, 0.01));
    CHECK(!track->byPlayer);
}

TEST_CASE("Ballistics terminus is recorded and names what was hit", "[dev][ballistics]")
{
    BallisticsTrackStore store;
    const uint32_t id = BeginFlatShot(store);
    store.AddSample(id, 0.0f, 1.5f, 0.0f, 0.0f);
    store.AddSample(id, 0.0f, 1.4f, 100.0f, 0.11f);

    store.Finish(id, BallisticTerminus::HitObject, "T72");

    const BallisticTrack* track = store.Find(id);
    REQUIRE(track != nullptr);
    CHECK(track->terminus == BallisticTerminus::HitObject);
    CHECK(std::string(track->hit.Get()) == "T72");

    // Finishing an id the store never issued must be a no-op, not a crash:
    // the sweep in BallisticsRecorder::Sample can reach an evicted track.
    store.Finish(id + 12345u, BallisticTerminus::Lost, nullptr);
    store.AddSample(id + 12345u, 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(store.Size() == 1);
}

TEST_CASE("Ballistics names are truncated rather than overflowing", "[dev][ballistics]")
{
    BallisticsTrackStore store;
    const char* longName = "an_extremely_long_ammunition_class_name_that_will_not_fit_in_the_fixed_field";
    const uint32_t id = store.Begin(longName, longName, false, 0, 0, 0, 0, 0, 1, false, 0, 0);

    const BallisticTrack* track = store.Find(id);
    REQUIRE(track != nullptr);
    CHECK(std::strlen(track->ammo.Get()) == Poseidon::Dev::BallisticName::Capacity - 1);
    CHECK(track->hit.Empty());

    // A null name is legal and yields an empty field.
    store.Finish(id, BallisticTerminus::Expired, nullptr);
    CHECK(track->hit.Empty());
}

TEST_CASE("Diagnostic pause is off by default and refuses to engage without a world", "[dev][ballistics][pause]")
{
    // The whole "inert when unused" claim rests on this default.  A unit test
    // runs with GWorld == nullptr, which is also the DiagPauseAvailable() false
    // case, so this doubles as the check that the gate refuses to latch.
    CHECK(Poseidon::Dev::DiagPauseActive() == false);
    CHECK(Poseidon::Dev::DiagPauseAvailable() == false);

    CHECK(Poseidon::Dev::SetDiagPause(true) == false);
    CHECK(Poseidon::Dev::DiagPauseActive() == false);

    CHECK(Poseidon::Dev::ToggleDiagPause() == false);
    CHECK(Poseidon::Dev::DiagPauseActive() == false);
}
