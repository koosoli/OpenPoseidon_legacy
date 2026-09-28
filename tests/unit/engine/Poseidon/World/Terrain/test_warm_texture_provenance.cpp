#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/WarmTextureProvenance.hpp>
#include <cstring>
#include <thread>
#include <array>
#include <string>
using Poseidon::BankReadMemberIdentity;
using Poseidon::Streaming::WarmTextureProvenance;

static BankReadMemberIdentity Identity(uint64_t volume, uint64_t offset)
{
    BankReadMemberIdentity id;
    id.volume = volume; id.archiveBytes = 8192; id.offset = offset; id.bytes = 512;
    id.fileId[0] = 19; id.fileId[15] = 73;
    return id;
}
TEST_CASE("Warm trace differentiates physical source incarnations without retaining leases", "[warm-provenance]")
{
    WarmTextureProvenance trace;
    auto a = Identity(1, 32), b = Identity(2, 32), c = Identity(1, 64);
    const auto old = trace.Admit("shared/path.paa", a);
    REQUIRE(old == 1);
    CHECK(trace.Admit("shared/path.paa", a) == old); // same source, not a new job
    CHECK(trace.KeyOnly("shared/path.paa") == old);
    const auto remounted = trace.Admit("shared/path.paa", b);
    REQUIRE(remounted == 2);
    CHECK(trace.Find("shared/path.paa", a) == old);
    CHECK(trace.Find("shared/path.paa", b) == remounted);
    CHECK(trace.Find("shared/path.paa", c) == 0);
    CHECK(trace.KeyOnly("shared/path.paa") == 0); // name is ambiguous, no fabricated attribution
    CHECK(trace.Admit("", a) == 0);
    CHECK(trace.Admit(std::string(1024, 'x'), a) == 0);
    CHECK(trace.Observe().sources == 2);
}
TEST_CASE("Warm trace atomics preserve one event across concurrent worker/owner stamps", "[warm-provenance]")
{
    WarmTextureProvenance trace;
    const auto token = trace.Admit("texture.paa", Identity(1, 32));
    REQUIRE(token == 1);
    std::array<WarmTextureProvenance::Row, 2> rows{};
    std::thread worker([&] { rows[0] = trace.Stamp(token, WarmTextureProvenance::Event::WorkerStarted, 8); });
    rows[1] = trace.Stamp(token, WarmTextureProvenance::Event::WorkerStarted, 9);
    worker.join();
    CHECK(bool(rows[0])); CHECK(bool(rows[1]));
    CHECK(rows[0].ordinal != rows[1].ordinal);
    CHECK(trace.Observe().rows == 2);
    CHECK(trace.Stamp(token, WarmTextureProvenance::Event::WorkerStarted));
    CHECK(trace.Stamp(token, WarmTextureProvenance::Event::WorkerStarted));
    CHECK_FALSE(trace.Stamp(token, WarmTextureProvenance::Event::WorkerStarted));
    CHECK(trace.Stamp(token, WarmTextureProvenance::Event::OwnerExactAttempt));
    CHECK(trace.Observe().rows == 5);
}
TEST_CASE("Warm trace caps keys and rows without reusing numeric tokens", "[warm-provenance]")
{
    WarmTextureProvenance trace;
    for (unsigned i=0;i<WarmTextureProvenance::MaxRecords;++i)
        REQUIRE(trace.Admit(std::to_string(i), Identity(1,i)) == i+1);
    CHECK(trace.Admit("ninth", Identity(1,9)) == 0);
    CHECK(trace.Observe().sources == 8);
    CHECK(trace.Observe().truncated);
    for (unsigned t=1;t<=8;++t)
        for (unsigned e=0;e<static_cast<unsigned>(WarmTextureProvenance::Event::Count);++e)
            trace.Stamp(t, static_cast<WarmTextureProvenance::Event>(e));
    CHECK(trace.Observe().rows == WarmTextureProvenance::MaxRows);
    CHECK(trace.Observe().truncated);
}
TEST_CASE("Warm trace reservation shares the unchanged archive metadata cap without consuming binding slots", "[warm-provenance]")
{
    auto budget = std::make_shared<Poseidon::ArchiveSourceBudget>(2, 128, 2);
    auto binding = budget->Acquire(32);
    auto weak = budget->AcquireWeak(32);
    auto trace = budget->AcquireTrace(64);
    REQUIRE(binding); REQUIRE(weak); REQUIRE(trace);
    const auto charged = budget->Observe();
    CHECK(charged.bytes == 128);
    CHECK(charged.bindings == 1);
    CHECK(charged.weakCells == 1);
    CHECK(charged.traceBytes == 64);
    CHECK_FALSE(budget->AcquireTrace(1));
    trace.reset();
    CHECK(budget->Observe().traceBytes == 0);
    CHECK(budget->Acquire(64));
}

TEST_CASE("Warm trace latches only actually stored exact-source retirement and drains each reason once", "[warm-provenance]")
{
    using P = WarmTextureProvenance;
    P trace;
    const auto old = trace.Admit("same.paa", Identity(1, 32));
    const auto remount = trace.Admit("same.paa", Identity(2, 32));
    REQUIRE(old == 1); REQUIRE(remount == 2);
    trace.NoteRetired(old, P::RetireReason::Cancelled); // no successful store yet
    CHECK(trace.DrainRetirementReasons(old) == 0);
    trace.NoteStored(old); trace.NoteRetired(old, P::RetireReason::Cancelled);
    CHECK(trace.DrainRetirementReasons(remount) == 0);
    CHECK(trace.DrainRetirementReasons(old) == (1u << unsigned(P::RetireReason::Cancelled)));
    CHECK(trace.DrainRetirementReasons(old) == 0);
    trace.NoteStored(old); trace.NoteRetired(old, P::RetireReason::Consumed);
    CHECK(trace.DrainRetirementReasons(old) == 0);
    trace.NoteStored(remount); trace.NoteClearLive();
    CHECK(trace.DrainRetirementReasons(old) == 0);
    CHECK(trace.DrainRetirementReasons(remount) == (1u << unsigned(P::RetireReason::Clear)));
}

TEST_CASE("Targeted warm trace leaves unrelated records unadmitted without changing event stamps", "[warm-provenance]")
{
    WarmTextureProvenance trace({}, true);
    const auto id = Identity(1, 32);
    CHECK(trace.Admit("unrelated.paa", id) == 0);
    CHECK(trace.Observe().sources == 0);
    const auto token = trace.Admit(WarmTextureProvenance::TargetKey(), id);
    REQUIRE(token == 1);
    CHECK(trace.Find(WarmTextureProvenance::TargetKey(), id) == token);
    CHECK(trace.Stamp(token, WarmTextureProvenance::Event::StorePut));
    CHECK(trace.EventOccurrences(token, WarmTextureProvenance::Event::StorePut) == 1);
    CHECK(trace.Observe().sources == 1);
}
