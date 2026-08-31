#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Audio/Core/VoiceHandle.hpp>

// Tests for the generation-checked voice handle table (roadmap A.2).
//
// These deliberately run against VoiceHandleTable directly and touch no audio
// device, no mixer and no OpenAL. REN-ID-001's finding was that the renderer's
// equivalent generation check had NO test at all, because the only way to reach
// it was through a type that needed a GPU, so every test of it opened with an
// early-return on "no adapter" and passed vacuously. A test that cannot fail is
// worthless; a test that can silently skip is the same thing wearing a hat.
//
// Two of these carry explicit anti-vacuity assertions (the slot really was
// recycled, the address really was reused). Without those, a change in free-list
// order would turn them green by never exercising the path they claim to cover.

using Poseidon::Audio::kInvalidVoiceHandle;
using Poseidon::Audio::MakeVoiceHandle;
using Poseidon::Audio::VoiceHandle;
using Poseidon::Audio::VoiceHandleBiasedSlot;
using Poseidon::Audio::VoiceHandleGeneration;
using Poseidon::Audio::VoiceHandleTable;

namespace
{

// Stand-in for WaveOAL. The table never dereferences what it stores, so an
// address is all a test needs -- and using a POD keeps the test independent of
// the audio backend entirely.
struct FakeVoice
{
    int tag = 0;
};

// The table's slots are biased by +1 on the way out; tests that want to prove a
// slot was reused have to unbias, exactly like cull.rs's `raw_slot` helper.
uint32_t RawSlot(VoiceHandle h)
{
    return VoiceHandleBiasedSlot(h) - 1u;
}

} // namespace

TEST_CASE("VoiceHandle: layout is (slot+1) | gen<<24, matching the renderer's InstanceTable",
          "[Audio][VoiceHandle]")
{
    // Pinned rather than assumed: the point of copying the renderer's layout is
    // that the two read the same, so a drift here should break a test.
    const VoiceHandle h = MakeVoiceHandle(5u, 3u);
    CHECK(h == ((5u + 1u) | (3u << 24)));
    CHECK(VoiceHandleBiasedSlot(h) == 6u);
    CHECK(RawSlot(h) == 5u);
    CHECK(VoiceHandleGeneration(h) == 3u);

    // Slot 0 with generation 0 must still be a non-zero handle, which is the
    // entire reason for the +1 bias.
    CHECK(MakeVoiceHandle(0u, 0u) != kInvalidVoiceHandle);
}

TEST_CASE("VoiceHandle: acquire/resolve roundtrip", "[Audio][VoiceHandle]")
{
    VoiceHandleTable<FakeVoice> table;
    FakeVoice a{1};
    FakeVoice b{2};

    const VoiceHandle ha = table.Acquire(&a);
    const VoiceHandle hb = table.Acquire(&b);

    REQUIRE(ha != kInvalidVoiceHandle);
    REQUIRE(hb != kInvalidVoiceHandle);
    CHECK(ha != hb);
    CHECK(table.Resolve(ha) == &a);
    CHECK(table.Resolve(hb) == &b);
    CHECK(table.LiveCount() == 2);
    CHECK(table.StaleOps() == 0);
}

TEST_CASE("VoiceHandle: a recycled slot refuses the dead voice's handle even at the same address",
          "[Audio][VoiceHandle]")
{
    // THE bug this table exists for. SoundSystemOAL::ResumeMusicForPreview used
    // to remember a WaveOAL* and prove liveness by finding it again in the live
    // registry -- which succeeds when the allocator hands the freed block to a
    // different wave. Here the second voice is deliberately placed at the SAME
    // address as the first to reproduce that exactly.
    VoiceHandleTable<FakeVoice> table;
    alignas(FakeVoice) unsigned char storage[sizeof(FakeVoice)];

    auto* first = new (storage) FakeVoice{1};
    const VoiceHandle dead = table.Acquire(first);
    REQUIRE(table.Resolve(dead) == first);
    first->~FakeVoice();
    REQUIRE(table.Release(dead));

    auto* second = new (storage) FakeVoice{2};
    const VoiceHandle live = table.Acquire(second);

    // Anti-vacuity, both halves: the address really was reused AND the slot
    // really was recycled. Without these the test could pass by never having
    // exercised recycling at all.
    REQUIRE(static_cast<void*>(second) == static_cast<void*>(first));
    REQUIRE(RawSlot(live) == RawSlot(dead));
    REQUIRE(live != dead);

    // The pointer comparison the old code did would say "still valid" here.
    CHECK(table.Resolve(dead) == nullptr);
    CHECK(table.StaleOps() == 1);
    CHECK(table.Resolve(live) == second);

    second->~FakeVoice();
}

TEST_CASE("VoiceHandle: a stale handle cannot release the voice that replaced it", "[Audio][VoiceHandle]")
{
    VoiceHandleTable<FakeVoice> table;
    FakeVoice a{1};
    FakeVoice b{2};

    const VoiceHandle dead = table.Acquire(&a);
    REQUIRE(table.Release(dead));
    const VoiceHandle live = table.Acquire(&b);
    REQUIRE(RawSlot(live) == RawSlot(dead)); // anti-vacuity: the slot was reused

    // A second release through the dead handle must not free the slot again --
    // that would push one slot onto the free list twice and hand the same slot
    // to two live voices.
    CHECK_FALSE(table.Release(dead));
    CHECK(table.StaleOps() == 1);
    CHECK(table.Resolve(live) == &b);
    CHECK(table.LiveCount() == 1);

    // And the free list is still sane: the next two acquires get distinct slots.
    FakeVoice c{3};
    FakeVoice d{4};
    const VoiceHandle hc = table.Acquire(&c);
    const VoiceHandle hd = table.Acquire(&d);
    CHECK(RawSlot(hc) != RawSlot(hd));
}

TEST_CASE("VoiceHandle: handle zero is never valid", "[Audio][VoiceHandle]")
{
    VoiceHandleTable<FakeVoice> table;
    FakeVoice a{1};
    const VoiceHandle h = table.Acquire(&a);
    CHECK(h != kInvalidVoiceHandle);

    CHECK(table.Resolve(kInvalidVoiceHandle) == nullptr);
    CHECK_FALSE(table.IsLive(kInvalidVoiceHandle));
    CHECK_FALSE(table.Release(kInvalidVoiceHandle));
    CHECK(table.StaleOps() == 3);
}

TEST_CASE("VoiceHandle: a handle for a slot that was never allocated is refused", "[Audio][VoiceHandle]")
{
    VoiceHandleTable<FakeVoice> table;
    FakeVoice a{1};
    table.Acquire(&a); // one slot exists: slot 0

    CHECK(table.Resolve(MakeVoiceHandle(1u, 0u)) == nullptr);   // past the end
    CHECK(table.Resolve(MakeVoiceHandle(999u, 0u)) == nullptr); // far past the end
    CHECK(table.StaleOps() == 2);
}

TEST_CASE("VoiceHandle: a released slot is refused even by a generation-0 handle", "[Audio][VoiceHandle]")
{
    // Release bumps the generation, so this is normally caught by the generation
    // compare. The `live` flag is the belt to that braces: it is what refuses a
    // handle for a slot that is currently free, independent of any generation
    // arithmetic.
    VoiceHandleTable<FakeVoice> table;
    FakeVoice a{1};
    const VoiceHandle h = table.Acquire(&a);
    REQUIRE(table.Release(h));

    CHECK(table.Resolve(h) == nullptr);
    CHECK(table.LiveCount() == 0);

    // The post-bump generation is the case the generation compare CANNOT catch:
    // the slot's counter now reads 1, so a handle carrying generation 1 matches.
    // Nothing ever issues such a handle -- but a garbage or wrapped one can carry
    // it, and if it were accepted, Release would push an already-free slot onto
    // the free list a second time and two live voices would later share it.
    // Ablating the `live` flag while keeping the generation compare fails three
    // of the checks below (Resolve still answers nullptr, because a freed slot's
    // stored pointer was cleared -- which is precisely why Resolve alone is not
    // enough evidence and IsLive/Release have to be asserted too). Before these
    // were added, deleting the flag left all nine cases green.
    const VoiceHandle forged = MakeVoiceHandle(RawSlot(h), 1u);
    CHECK(table.Resolve(forged) == nullptr);
    CHECK_FALSE(table.IsLive(forged));
    CHECK_FALSE(table.Release(forged));

    // Free list still holds the slot exactly once: two acquires, two slots.
    FakeVoice b{2};
    FakeVoice c{3};
    const VoiceHandle hb = table.Acquire(&b);
    const VoiceHandle hc = table.Acquire(&c);
    CHECK(RawSlot(hb) != RawSlot(hc));
}

TEST_CASE("VoiceHandle: a stale handle aliases only after 256 generations", "[Audio][VoiceHandle]")
{
    // 8 generation bits. The bound is asserted, not assumed -- it is the honest
    // statement of what this scheme does and does not protect against, and a
    // future widening of the field should break this test rather than pass
    // silently.
    VoiceHandleTable<FakeVoice> table;
    FakeVoice a{1};

    const VoiceHandle original = table.Acquire(&a);
    VoiceHandle current = original;
    for (int cycle = 1; cycle <= 255; ++cycle)
    {
        REQUIRE(table.Release(current));
        current = table.Acquire(&a);
        REQUIRE(RawSlot(current) == RawSlot(original)); // anti-vacuity: same slot every time
        REQUIRE(current != original);                   // 255 distinct generations
    }
    CHECK(VoiceHandleGeneration(current) == 255u);

    // The 256th cycle wraps the generation back to 0 and the handle aliases the
    // original. That is the documented limit of the scheme, stated out loud.
    REQUIRE(table.Release(current));
    const VoiceHandle wrapped = table.Acquire(&a);
    CHECK(VoiceHandleGeneration(wrapped) == 0u);
    CHECK(wrapped == original);
    CHECK(table.SlotReuses() == 256u);
}

TEST_CASE("VoiceHandle: Clear resets slots and counters", "[Audio][VoiceHandle]")
{
    VoiceHandleTable<FakeVoice> table;
    FakeVoice a{1};
    const VoiceHandle h = table.Acquire(&a);
    table.Release(h);
    table.Resolve(h); // one stale op
    REQUIRE(table.StaleOps() == 1);

    table.Clear();
    CHECK(table.StaleOps() == 0);
    CHECK(table.LiveCount() == 0);
    CHECK(table.SlotCount() == 0);
    CHECK(table.SlotReuses() == 0);
}
