// Presentation-snapshot ring (roadmap Phase 5 step 5, design notes).
//
// What these pin down, and why each matters beyond "the code does what it says":
//
//  * SLOT REUSE ORDER. The whole point of the ring is that a publish writes storage no
//    consumer is reading. A test that only asked "does the accessor return the newest
//    values" would pass just as well against the single global instance the ring
//    replaced -- so these assert on the raw slot index, and that publish N and publish
//    N + kRingSize land in the SAME storage while consecutive publishes do not.
//  * GENERATION STRICTLY INCREASES, and survives the wrap. Slot storage is recycled;
//    the generation must not be, or a render thread keying temporal history off it
//    would silently pair two different frames.
//  * THE ACCESSOR NEVER SHOWS A HALF-WRITTEN SLOT. Checked by holding a reference
//    across a later publish and asserting the referent did not change -- the invariant
//    the flip exists to provide, and the one the in-place overwrite did not have.
//  * PUBLISH-WITHOUT-CONSUME IS COUNTED. 5.3 forbids an unbounded queue; the ring
//    enforces the bound by dropping rather than queueing, and the drop must be visible
//    rather than silent.
//
// No live globals: PublishPresentationSnapshot takes a filled value, so the ring is
// exercised without a world, a scene or a renderer.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Dev/Diag/SnapshotDiag.hpp>
#include <Poseidon/Graphics/Rendering/Frame/PresentationSnapshot.hpp>

#include <cstdint>
#include <vector>

using Poseidon::render::frame::GPresentationSnapshot;
using Poseidon::render::frame::kPresentationSnapshotRingSize;
using Poseidon::render::frame::PresentationSnapshot;
using Poseidon::render::frame::PresentationSnapshotSlot;
using Poseidon::render::frame::PresentationSnapshotSlotIndex;
using Poseidon::render::frame::PublishPresentationSnapshot;
using Poseidon::render::frame::ResetPresentationSnapshotRingForTest;
using Poseidon::render::frame::RetirePresentationSnapshot;

namespace
{

// A snapshot whose payload is identifiable, so "the accessor returned the newest one"
// is a statement about VALUES and not only about the generation stamp.
PresentationSnapshot Marked(float mark)
{
    PresentationSnapshot snap;
    snap.valid = true;
    snap.timeSeconds = mark;
    return snap;
}

// One publish + one retire: the synchronous frame the engine actually runs.
void PublishAndConsume(float mark)
{
    PublishPresentationSnapshot(Marked(mark));
    RetirePresentationSnapshot();
}

} // namespace

TEST_CASE("presentation snapshot ring starts empty", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    // Generation 0 means "never published". The consuming sites already treat
    // valid == false as "no world up" and fall back to their own defaults, so a
    // pre-publish read is a defined state, not a hole.
    const PresentationSnapshot& initial = GPresentationSnapshot();
    CHECK(initial.generation == 0);
    CHECK_FALSE(initial.valid);
    CHECK(Poseidon::Dev::GSnapshotCounters().published == 0);
}

TEST_CASE("publishing N+1 times reuses slots in ring order", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    // Publish one MORE than the ring holds, so the last publish must land back in the
    // storage the first one used. That wrap is the assertion; anything less would pass
    // against a single instance too.
    const int publishes = kPresentationSnapshotRingSize + 1;
    std::vector<int> slots;
    for (int i = 0; i < publishes; ++i)
    {
        PublishAndConsume(static_cast<float>(i));
        slots.push_back(PresentationSnapshotSlotIndex());
    }

    REQUIRE(static_cast<int>(slots.size()) == publishes);

    // Consecutive publishes never share storage — that is what makes the reader's
    // reference safe across one publish.
    for (int i = 1; i < publishes; ++i)
        CHECK(slots[i] != slots[i - 1]);

    // ...and publish i and publish i + kRingSize DO share it, which is what makes this a
    // bounded ring rather than unbounded growth.
    for (int i = 0; i + kPresentationSnapshotRingSize < publishes; ++i)
        CHECK(slots[i] == slots[i + kPresentationSnapshotRingSize]);

    // Every slot index the ring hands out is in range.
    for (int slot : slots)
    {
        CHECK(slot >= 0);
        CHECK(slot < kPresentationSnapshotRingSize);
    }
}

TEST_CASE("the accessor always returns the newest complete snapshot", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    for (int i = 0; i < kPresentationSnapshotRingSize * 3; ++i)
    {
        const float mark = 100.0f + static_cast<float>(i);
        PublishAndConsume(mark);

        const PresentationSnapshot& current = GPresentationSnapshot();
        CHECK(current.valid);
        CHECK(current.timeSeconds == mark);
        CHECK(current.generation == static_cast<uint64_t>(i + 1));

        // The accessor and the raw slot must agree — i.e. the flip published the slot
        // that was just written, not the one before it.
        CHECK(&current == &PresentationSnapshotSlot(PresentationSnapshotSlotIndex()));
    }
}

TEST_CASE("generation increases strictly across a ring wrap", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    uint64_t previous = 0;
    for (int i = 0; i < kPresentationSnapshotRingSize * 4 + 1; ++i)
    {
        PublishAndConsume(static_cast<float>(i));
        const uint64_t generation = GPresentationSnapshot().generation;
        // Strictly greater, not merely different: slot storage is recycled, identity
        // must not be.
        CHECK(generation > previous);
        previous = generation;
    }
}

TEST_CASE("a held reference survives the next publish unmodified", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    // This is the property the single global instance did NOT have: a consumer that
    // took a reference at the top of its frame was reading storage the next publish
    // would overwrite in place.
    PublishAndConsume(11.0f);
    const PresentationSnapshot& held = GPresentationSnapshot();
    const uint64_t heldGeneration = held.generation;

    PublishAndConsume(22.0f);

    CHECK(held.timeSeconds == 11.0f);
    CHECK(held.generation == heldGeneration);
    CHECK(GPresentationSnapshot().timeSeconds == 22.0f);
    CHECK(&held != &GPresentationSnapshot());
}

TEST_CASE("publish without consume is counted, not queued", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    // The steady state: one publish, one retire. Nothing is dropped.
    for (int i = 0; i < 4; ++i)
        PublishAndConsume(static_cast<float>(i));
    CHECK(Poseidon::Dev::GSnapshotCounters().publishedWithoutConsume == 0);
    CHECK(Poseidon::Dev::GSnapshotCounters().published == 4);
    CHECK(Poseidon::Dev::GSnapshotCounters().consumed == 4);

    // Now publish twice with no retire between. The ring is bounded, so the second
    // publish does not queue behind the first — it drops it. 5.3 asks for that to be
    // observable rather than silent.
    PublishPresentationSnapshot(Marked(50.0f));
    PublishPresentationSnapshot(Marked(51.0f));
    CHECK(Poseidon::Dev::GSnapshotCounters().publishedWithoutConsume == 1);
    CHECK(Poseidon::Dev::GSnapshotCounters().published == 6);
    CHECK(Poseidon::Dev::GSnapshotCounters().consumed == 4);
    // Queue depth stayed bounded: published - consumed can exceed the ring size as a
    // COUNT, but the storage did not grow, and the newest values are what a consumer
    // sees.
    CHECK(GPresentationSnapshot().timeSeconds == 51.0f);

    RetirePresentationSnapshot();
    CHECK(Poseidon::Dev::GSnapshotCounters().consumed == 5);
    // Age is 0 on the synchronous path: the retire consumed the newest publish.
    CHECK(Poseidon::Dev::GSnapshotCounters().consumedAge == 0);
}

TEST_CASE("retire arms the stale-capture fallback exactly once", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    // The fallback contract, stated in counters: between a publish and its retire the
    // snapshot is fresh, so a second retire must not double-count a consume, and the
    // published/consumed pair must stay balanced across a normal frame.
    PublishPresentationSnapshot(Marked(7.0f));
    RetirePresentationSnapshot();
    RetirePresentationSnapshot(); // idempotent: no world published in between

    CHECK(Poseidon::Dev::GSnapshotCounters().published == 1);
    CHECK(Poseidon::Dev::GSnapshotCounters().consumed == 1);

    // Retiring does NOT clear the values — the renderer's fallback decides whether to
    // re-capture, and until it does the last published frame is still readable. This
    // is what stops a retire from blanking the snapshot mid-teardown.
    CHECK(GPresentationSnapshot().timeSeconds == 7.0f);
    CHECK(GPresentationSnapshot().generation == 1);
}

TEST_CASE("the resource epoch is stamped on publish and is not yet fed", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();

    PublishAndConsume(1.0f);
    // 0 is the correct value today: nothing bumps the epoch, because the snapshot
    // carries no resource handles for an epoch to invalidate. This asserts the SEAM is
    // wired (stamped from the ring, echoed to diagnostics), not that a producer exists.
    CHECK(GPresentationSnapshot().resourceEpoch == 0);
    CHECK(Poseidon::Dev::GSnapshotCounters().resourceEpoch == 0);

    Poseidon::render::frame::BumpPresentationResourceEpoch();
    // The bump applies to snapshots published AFTER it; the one in flight keeps the
    // epoch it was stamped with, which is exactly what makes it rejectable.
    CHECK(GPresentationSnapshot().resourceEpoch == 0);
    PublishAndConsume(2.0f);
    CHECK(GPresentationSnapshot().resourceEpoch == 1);
    CHECK(Poseidon::Dev::GSnapshotCounters().resourceEpoch == 1);

    ResetPresentationSnapshotRingForTest();
}

TEST_CASE("the ring bound is recorded for the diagnostics", "[graphics][render][snapshot]")
{
    ResetPresentationSnapshotRingForTest();
    PublishAndConsume(1.0f);
    // 5.3's gate is "snapshot queue depth is bounded AND observable". The bound itself
    // has to reach the capture sidecar, not only the counts against it.
    CHECK(Poseidon::Dev::GSnapshotCounters().ringSize == static_cast<uint32_t>(kPresentationSnapshotRingSize));
    CHECK(kPresentationSnapshotRingSize >= 2);
}
