#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/PaaPreparationInflight.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using Poseidon::BankReadRequest;
using Poseidon::Streaming::PaaPreparationInflight;
using Poseidon::render::DdsPublicationInventory;
using Poseidon::render::DdsPublicationToken;
using Outcome = PaaPreparationInflight::Outcome;

TEST_CASE("PAA inflight never suppresses unleased or uncertified inputs", "[paa-inflight]")
{
    PaaPreparationInflight attempts;
    auto inventory = DdsPublicationInventory::Create(2);
    DdsPublicationToken token{inventory, 0, inventory->Epoch(0)};
    BankReadRequest unleased{"not-opened.pbo", 4, 16};
    auto result = attempts.Acquire("texture.paa", 1, unleased, token);
    CHECK(result.outcome == Outcome::UnleasedOrInvalid);
    CHECK_FALSE(result.ticket.OwnsSlot());
    CHECK(attempts.Snapshot().active == 0);
    CHECK(attempts.Snapshot().suppressed == 0);
}

#ifdef _WIN32
namespace
{
struct Archives
{
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("paa-inflight-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Poseidon::QFBank first, other;
    Archives()
    {
        REQUIRE(std::filesystem::create_directory(directory));
        for (const char* name : {"first", "other"})
        {
            std::ofstream out(directory / (std::string(name) + ".pbo"), std::ios::binary);
            REQUIRE(out.good());
            auto entry = [&](const char* member, int size) {
                out.write(member, std::char_traits<char>::length(member) + 1);
                for (int value : {0, 0, 0, 0, size}) out.write(reinterpret_cast<const char*>(&value), 4);
            };
            entry("a.paa", 4); entry("b.paa", 4); entry("", 0);
            out.write("abcdefgh", 8);
        }
        REQUIRE(first.open(Poseidon::RString((directory / "first").string().c_str())));
        REQUIRE(other.open(Poseidon::RString((directory / "other").string().c_str())));
        first.Lock(); other.Lock();
    }
    ~Archives()
    {
        first.Unlock(); other.Unlock();
        first.close(); other.close();
        std::error_code error;
        std::filesystem::remove(directory / "first.pbo", error);
        error.clear();
        std::filesystem::remove(directory / "other.pbo", error);
        error.clear();
        std::filesystem::remove(directory, error); // nonrecursive: preserve unexpected files
    }
    BankReadRequest Capture(Poseidon::QFBank& bank, const char* member)
    {
        auto request = bank.CaptureReadRequest(member, true);
        REQUIRE(request);
        REQUIRE(request->HasArchiveIdentity());
        return *request;
    }
};
}

TEST_CASE("PAA inflight matches complete member identity and store generation", "[paa-inflight][archive-lease]")
{
    Archives files;
    auto source = files.Capture(files.first, "a.paa");
    auto same = files.Capture(files.first, "a.paa");
    auto otherMember = files.Capture(files.first, "b.paa");
    auto otherFile = files.Capture(files.other, "a.paa");
    auto inventory = DdsPublicationInventory::Create(2);
    DdsPublicationToken token{inventory, 0, inventory->Epoch(0)};
    PaaPreparationInflight attempts;
    auto owner = attempts.Acquire("texture.paa", 1, source, token);
    REQUIRE(owner.outcome == Outcome::Tracked);
    CHECK(owner.ticket.OwnsSlot());
    auto duplicate = attempts.Acquire("texture.paa", 1, same, token);
    CHECK(duplicate.outcome == Outcome::Suppressed);
    CHECK_FALSE(duplicate.ticket.OwnsSlot());
    auto member = attempts.Acquire("texture.paa", 1, otherMember, token);
    auto physicalFile = attempts.Acquire("texture.paa", 1, otherFile, token);
    auto generation = attempts.Acquire("texture.paa", 2, source, token);
    auto key = attempts.Acquire("another.paa", 1, source, token);
    CHECK(member.outcome == Outcome::Tracked);
    CHECK(physicalFile.outcome == Outcome::Tracked);
    CHECK(generation.outcome == Outcome::Tracked);
    CHECK(key.outcome == Outcome::Tracked);
    auto mutated = source; ++mutated.offset;
    CHECK(attempts.Acquire("texture.paa", 1, mutated, token).outcome == Outcome::UnleasedOrInvalid);
    CHECK(attempts.Acquire("texture.paa", 0, source, token).outcome == Outcome::UnleasedOrInvalid);
    CHECK(attempts.Acquire("texture.paa", 1, source, {}).outcome == Outcome::UnleasedOrInvalid);
    CHECK(attempts.Snapshot().active == 5);
    owner.ticket.Release();
    auto afterRelease = attempts.Acquire("texture.paa", 1, source, token);
    CHECK(afterRelease.outcome == Outcome::Tracked);
}

TEST_CASE("PAA inflight cancellation replaces bookkeeping without borrowing debt or tokens", "[paa-inflight][archive-lease]")
{
    Archives files;
    auto source = files.Capture(files.first, "a.paa");
    auto inventory = DdsPublicationInventory::Create(2);
    auto replacementInventory = DdsPublicationInventory::Create(2);
    DdsPublicationToken old{inventory, 0, inventory->Epoch(0)};
    PaaPreparationInflight attempts;
    auto original = attempts.Acquire("texture.paa", 1, source, old);
    REQUIRE(original.outcome == Outcome::Tracked);
    const auto oldBytes = attempts.Snapshot().metadataBytes;
    DdsPublicationToken fresh;
    SECTION("model cancellation")
    {
        inventory->CancelModel(0);
        fresh = {inventory, 1, inventory->Epoch(1)};
    }
    SECTION("world reset invalidates the original inventory")
    {
        inventory->InvalidateAll();
        fresh = {replacementInventory, 0, replacementInventory->Epoch(0)};
    }
    auto replacement = attempts.Acquire("texture.paa", 1, source, fresh);
    REQUIRE(replacement.outcome == Outcome::Tracked);
    CHECK(attempts.Snapshot().active == 1);
    CHECK(attempts.Snapshot().metadataBytes > oldBytes); // cancelled ticket still owns its lease/debt
    original.ticket.Release(); // cannot erase the new slot
    CHECK(attempts.Snapshot().active == 1);
    CHECK(attempts.Snapshot().metadataBytes == oldBytes);
    CHECK(attempts.Acquire("texture.paa", 1, source, fresh).outcome == Outcome::Suppressed);
    replacement.ticket.Release();
    CHECK(attempts.Snapshot().active == 0);
    CHECK(attempts.Acquire("texture.paa", 1, source, old).outcome == Outcome::UnleasedOrInvalid);
}

TEST_CASE("PAA inflight slot and metadata limits pass through without blocking work", "[paa-inflight][archive-lease]")
{
    Archives files;
    auto source = files.Capture(files.first, "a.paa");
    auto inventory = DdsPublicationInventory::Create(1);
    DdsPublicationToken token{inventory, 0, inventory->Epoch(0)};
    PaaPreparationInflight attempts;
    std::vector<PaaPreparationInflight::Ticket> held;
    for (size_t i = 0; i < PaaPreparationInflight::MaxSlots; ++i)
    {
        auto acquired = attempts.Acquire("stage" + std::to_string(i) + ".paa", 1, source, token);
        REQUIRE(acquired.outcome == Outcome::Tracked);
        held.push_back(std::move(acquired.ticket));
    }
    CHECK(attempts.Snapshot().active == 128);
    CHECK(attempts.Acquire("overflow.paa", 1, source, token).outcome == Outcome::CapacityPassThrough);
    CHECK(attempts.Acquire(std::string(PaaPreparationInflight::MaxKeyBytes + 1, 'x'), 1, source, token).outcome == Outcome::CapacityPassThrough);
    CHECK(attempts.Snapshot().overflow == 2);
    CHECK(attempts.Snapshot().metadataBytes <= PaaPreparationInflight::MetadataBudget);
    held.clear();
    CHECK(attempts.Snapshot().active == 0);
    // Independently measured structural charge permits exactly one owned binding.
    PaaPreparationInflight measured;
    const auto baseBytes = measured.Snapshot().metadataBytes;
    auto single = measured.Acquire("texture.paa", 1, source, token);
    REQUIRE(single.outcome == Outcome::Tracked);
    const auto oneBindingBytes = measured.Snapshot().metadataBytes - baseBytes;
    PaaPreparationInflight exact(baseBytes + oneBindingBytes);
    auto one = exact.Acquire("texture.paa", 1, source, token);
    REQUIRE(one.outcome == Outcome::Tracked);
    CHECK(exact.Snapshot().metadataBytes == baseBytes + oneBindingBytes);
    CHECK(exact.Acquire("texture.paa", 2, source, token).outcome == Outcome::CapacityPassThrough);
    one.ticket.Release();
    CHECK(exact.Snapshot().metadataBytes == baseBytes);
    CHECK(exact.Acquire("texture.paa", 2, source, token).outcome == Outcome::Tracked);
    PaaPreparationInflight noRoom(0);
    CHECK(noRoom.Acquire("texture.paa", 1, source, token).outcome == Outcome::CapacityPassThrough);
    CHECK(noRoom.Snapshot().active == 0);
}

TEST_CASE("PAA inflight actual threads release tracked attempts on normal and error exits", "[paa-inflight][archive-lease]")
{
    Archives files;
    auto source = files.Capture(files.first, "a.paa");
    auto inventory = DdsPublicationInventory::Create(2);
    DdsPublicationToken token{inventory, 0, inventory->Epoch(0)};
    PaaPreparationInflight attempts;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future().share();
    auto worker = std::async(std::launch::async, [&] {
        auto ticket = attempts.Acquire("texture.paa", 1, source, token);
        entered.set_value();
        releaseFuture.wait();
        return ticket.outcome;
    });
    struct ReleaseGuard
    {
        std::promise<void>& release;
        bool released = false;
        void Release() { if (!released) { released = true; release.set_value(); } }
        ~ReleaseGuard() { Release(); }
    } releaseGuard{release}; // releases even if a Catch assertion throws
    REQUIRE(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto peer = std::async(std::launch::async, [&] { return attempts.Acquire("texture.paa", 1, source, token).outcome; });
    CHECK(peer.get() == Outcome::Suppressed);
    releaseGuard.Release();
    CHECK(worker.get() == Outcome::Tracked);
    CHECK(attempts.Snapshot().active == 0);
    auto failed = std::async(std::launch::async, [&] {
        try { auto ticket = attempts.Acquire("texture.paa", 1, source, token); throw std::runtime_error("synthetic read failure"); }
        catch (const std::runtime_error&) { return true; }
    });
    CHECK(failed.get());
    CHECK(attempts.Snapshot().active == 0);
    CHECK(attempts.Acquire("texture.paa", 1, source, token).outcome == Outcome::Tracked);
}
#else
TEST_CASE("PAA inflight immutable archive matching requires Windows lease support", "[paa-inflight][archive-lease]")
{ SKIP("Native immutable archive leases are not supported on this platform"); }
#endif
