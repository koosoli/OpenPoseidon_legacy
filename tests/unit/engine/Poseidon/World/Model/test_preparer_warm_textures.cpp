#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Terrain/WarmTextureJobPolicy.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/FileAccessPolicy.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

using namespace Poseidon;
using Streaming::WarmTextureJobPolicy;
using render::PreparedTextureStore;
TEST_CASE("Warm texture debt bounds share the existing parse budget", "[warm-texture-policy]")
{
    CHECK(WarmTextureJobPolicy::Admit(7, 64, 0, WarmTextureJobPolicy::MetadataLimit));
    CHECK_FALSE(WarmTextureJobPolicy::Admit(8, 1, 0, 1));
    CHECK_FALSE(WarmTextureJobPolicy::Admit(0, 65, 0, 1));
    CHECK_FALSE(WarmTextureJobPolicy::Admit(0, 0, 0, 1));
    CHECK_FALSE(WarmTextureJobPolicy::Admit(0, 1, 1, WarmTextureJobPolicy::MetadataLimit));
    uint64_t charge = WarmTextureJobPolicy::MetadataLimit;
    CHECK_FALSE(WarmTextureJobPolicy::AddCharge(charge, 1));
    CHECK(charge == WarmTextureJobPolicy::MetadataLimit);
    const uint64_t scratch = WarmTextureJobPolicy::ScratchBytes;
    CHECK(WarmTextureJobPolicy::ScratchFits(scratch, 0, 0, 0, 0, 0));
    CHECK_FALSE(WarmTextureJobPolicy::ScratchFits(scratch, 0, 0, 0, 0, 0, 1));
    CHECK_FALSE(WarmTextureJobPolicy::ScratchFits(scratch * 2, 0, 1, 0, 0, scratch));
    CHECK(WarmTextureJobPolicy::ScratchFits(scratch * 2, 0, 0, 0, 0, scratch));
    CHECK(WarmTextureJobPolicy::ScratchFits(0, 0, 0, 0, 0, scratch * 8)); // soft budget0 alone is NOT a concurrency cap
    CHECK(WarmTextureJobPolicy::ActiveRoom(0)); CHECK(WarmTextureJobPolicy::ActiveRoom(1));
    CHECK_FALSE(WarmTextureJobPolicy::ActiveRoom(2)); CHECK_FALSE(WarmTextureJobPolicy::ActiveRoom(16));
}
TEST_CASE("Disabled warm jobs never change model state or allocate metadata", "[warm-texture-default]")
{
    if (ObjectStreamPreparer::WarmTextureJobsEnabled()) SKIP("Fresh process with warm jobs OFF required.");
    Foundation::CaptureMainThread();
    ObjectStreamPreparer preparer;
    const std::string path = "warm-only-inventory.p3d";
    preparer.Reset(&path, 1);
    auto token = preparer.QueryRadius(0);
    CHECK(preparer.SubmitWarmTextures(0, token, {}) == ObjectStreamPreparer::WarmTextureSubmit::Disabled);
    CHECK(preparer.Query(0) == ObjectStreamPreparer::State::Unknown);
    CHECK(preparer.SnapshotStats().warmTextureMetadataBytes == 0);
    CHECK(preparer.SnapshotStats().warmTextureScratchBytes == 0);
    CHECK(preparer.SnapshotStats().warmTextureQueueBytes == 0);
}
namespace
{
std::vector<char> OriginalPaa()
{
    std::vector<char> data;
    auto word = [&](unsigned x) { data.push_back(char(x)); data.push_back(char(x >> 8)); };
    word(0xff05); word(0);
    for (unsigned side : {16u, 8u, 4u})
    {
        word(side); word(side);
        unsigned bytes = ((side + 3) / 4) * ((side + 3) / 4) * 16;
        data.push_back(char(bytes)); data.push_back(char(bytes >> 8)); data.push_back(char(bytes >> 16));
        for (unsigned i = 0; i < bytes; ++i) data.push_back(char(i * 13 + side));
    }
    word(0); word(0); return data;
}
struct Fixture
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("warm-texture-worker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    QFBank bank;
    std::shared_ptr<const ArchiveSourceBinding> source;
    std::string key = dir.string() + "\\texture.paa";
    Fixture()
    {
        Foundation::CaptureMainThread();
        REQUIRE(std::filesystem::create_directory(dir));
        const auto paa = OriginalPaa();
        {
            std::ofstream out(dir / "original.pbo", std::ios::binary); REQUIRE(out.good());
            auto entry = [&](const char* name, int size) {
                out.write(name, std::strlen(name) + 1);
                for (int value : {0, 0, 0, 0, size}) out.write(reinterpret_cast<const char*>(&value), 4);
            };
            entry("texture.paa", int(paa.size())); entry("", 0); out.write(paa.data(), paa.size());
        }
        REQUIRE(bank.open(RString((dir / "original").string().c_str()))); bank.Lock();
        auto buffer = bank.Read("texture.paa"); REQUIRE(buffer); source = buffer->GetArchiveSourceBinding();
        REQUIRE(source); REQUIRE(bank.MatchesMountedMember("texture.paa", source->Request()));
    }
    ~Fixture()
    {
        source.reset(); if (bank.IsLocked()) bank.Unlock(); bank.close();
        std::error_code ec; std::filesystem::remove(dir / "original.pbo", ec); ec.clear();
        std::filesystem::remove(dir, ec);
    }
    std::vector<WarmTextureRead> Reads() const { return {{key, source}}; }
};
struct Gate
{
    std::mutex mutex; std::condition_variable cv; bool entered = false, blocked = true; size_t entries = 0;
    void Release() { std::lock_guard lock(mutex); blocked = false; cv.notify_all(); }
    static void BeforePut(void* context, bool)
    {
        auto& gate = *static_cast<Gate*>(context);
        std::unique_lock lock(gate.mutex); gate.entered = true; ++gate.entries; gate.cv.notify_all();
        gate.cv.wait(lock, [&] { return !gate.blocked; });
    }
    bool WaitEntries(size_t count, std::chrono::milliseconds duration = std::chrono::seconds(5))
    { std::unique_lock lock(mutex); return cv.wait_for(lock, duration, [&] { return entries >= count; }); }
    bool Wait() { std::unique_lock lock(mutex); return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }); }
};
// Must be destroyed BEFORE preparer (which joins workers), including assertion unwind.
struct ReleaseGuard { Gate& gate; ~ReleaseGuard() { gate.Release(); } };
bool WaitDone(ObjectStreamPreparer& preparer)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end)
    {
        const auto stats = preparer.SnapshotStats();
        if (!stats.warmTextureQueued && !stats.warmTextureActive) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
void RequireCapability()
{
#ifndef _WIN32
    SKIP("Required immutable archive lease is Windows-only.");
#else
    if (!ObjectStreamPreparer::WarmTextureJobsEnabled()) SKIP("Fresh WARM_TEXTURES=1 WARM_TEXTURE_JOBS=1 process required.");
    if (!QFileAccess::MappingSupported()) SKIP("Original mapped archive source capability unavailable.");
#endif
}
}
TEST_CASE("Warm-only worker publishes original leased mip bytes without a model request", "[warm-texture-actual]")
{
    RequireCapability(); Fixture fixture;
    auto& store = PreparedTextureStore::Instance(); store.Clear();
    ObjectStreamPreparer preparer;
    const std::string path = "never-read-warm-model.p3d"; preparer.Reset(&path, 1);
    const auto token = preparer.QueryRadius(0); CHECK(token.state == ObjectStreamPreparer::RadiusState::Unknown);
    auto wrong = token; ++wrong.generation;
    CHECK(preparer.SubmitWarmTextures(0, wrong, fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::InvalidInventory);
    wrong = token; wrong.modelIdentity += "x";
    CHECK(preparer.SubmitWarmTextures(0, wrong, fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::InvalidInventory);
    auto offthread = std::async(std::launch::async, [&] { return preparer.SubmitWarmTextures(0, token, fixture.Reads()); });
    CHECK(offthread.get() == ObjectStreamPreparer::WarmTextureSubmit::WrongThread);
    REQUIRE(preparer.SubmitWarmTextures(0, token, fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Submitted);
    REQUIRE(WaitDone(preparer));
    PAABlockChain chain, expected; auto original = OriginalPaa();
    REQUIRE(ReadPAABlockChainBuffer(original.data(), original.size(), expected));
    std::shared_ptr<const ArchiveSourceBinding> returned;
    REQUIRE(store.Take(fixture.key, chain, &returned)); REQUIRE(returned);
    CHECK(returned->Request().SameArchiveMember(fixture.source->Request()));
    CHECK(chain.blocks == expected.blocks); CHECK(chain.magic == expected.magic); CHECK(chain.levels.size() == expected.levels.size());
    CHECK(preparer.Query(0) == ObjectStreamPreparer::State::Unknown);
    const auto stats = preparer.SnapshotStats();
    CHECK(stats.prepared == 0); CHECK(stats.converted == 0); CHECK(stats.warmTextureJobsCompleted == 1); CHECK(stats.warmTextureQueueBytes > 0);
    CHECK(stats.warmTextureMembersPrepared == 1); CHECK(stats.warmTextureMetadataBytes == 0);
    CHECK(stats.warmTextureScratchBytes == 0); CHECK(stats.warmTextureScratchBytesPeak >= WarmTextureJobPolicy::ScratchBytes);
    store.Clear();
}
TEST_CASE("Warm active cancellation retains debt and cannot claim after same-world rescue or Reset", "[warm-texture-actual]")
{
    RequireCapability(); Fixture fixture;
    auto& store = PreparedTextureStore::Instance(); store.Clear();
    Gate gate; render::DdsPublicationObserver observer{}; observer.context = &gate; observer.beforePaaPut = &Gate::BeforePut;
    ObjectStreamPreparer preparer(256ull * 1024 * 1024, nullptr, &observer);
    ReleaseGuard release{gate};
    const std::string path = "warm-cancel-model.p3d"; preparer.Reset(&path, 1);
    auto token = preparer.QueryRadius(0);
    REQUIRE(preparer.SubmitWarmTextures(0, token, fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Submitted);
    REQUIRE(gate.Wait());
    const auto before = preparer.SnapshotStats(); CHECK(before.warmTextureMetadataBytes > 0); CHECK(before.warmTextureActive == 1);
    CHECK(preparer.SubmitWarmTextures(0, token, fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Coalesced);
    SECTION("same-world desired rescue cannot revalidate old publication token")
    {
        uint32_t stamp = 0; preparer.DropStale(&stamp, 1, 1);
        stamp = 1; preparer.DropStale(&stamp, 1, 1);
    }
    SECTION("new world inventory cannot revalidate old publication token")
    { preparer.Reset(&path, 1); token = preparer.QueryRadius(0); }
    CHECK(preparer.SnapshotStats().warmTextureMetadataBytes == before.warmTextureMetadataBytes);
    CHECK(preparer.SnapshotStats().warmTextureScratchBytes == before.warmTextureScratchBytes);
    gate.Release(); REQUIRE(WaitDone(preparer));
    PAABlockChain oldChain; std::shared_ptr<const ArchiveSourceBinding> oldSource;
    CHECK_FALSE(store.Take(fixture.key, oldChain, &oldSource));
    CHECK(preparer.SnapshotStats().warmTextureJobsCancelled == 1);
    CHECK(preparer.SnapshotStats().warmTextureMetadataBytes == 0);
    REQUIRE(preparer.SubmitWarmTextures(0, token, fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Submitted);
    REQUIRE(WaitDone(preparer)); REQUIRE(store.Take(fixture.key, oldChain, &oldSource)); REQUIRE(oldSource);
    CHECK(preparer.Query(0) == ObjectStreamPreparer::State::Unknown);
    store.Clear();
}

TEST_CASE("Warm queue is bounded even when shared scratch budget parks every optional job", "[warm-texture-actual]")
{
    RequireCapability(); Fixture fixture;
    ObjectStreamPreparer preparer(32ull * 1024 * 1024); // smaller than optional PAA scratch; no warm decode can start
    std::array<std::string, 9> paths;
    for (size_t i = 0; i < paths.size(); ++i) paths[i] = "warm-bound-" + std::to_string(i) + ".p3d";
    preparer.Reset(paths.data(), paths.size());
    auto overMembers = fixture.Reads(); overMembers.resize(65, overMembers.front());
    CHECK(preparer.SubmitWarmTextures(0, preparer.QueryRadius(0), std::move(overMembers)) == ObjectStreamPreparer::WarmTextureSubmit::Unsupported);
    auto missing = fixture.Reads(); missing.front().initializedSource.reset();
    CHECK(preparer.SubmitWarmTextures(0, preparer.QueryRadius(0), std::move(missing)) == ObjectStreamPreparer::WarmTextureSubmit::Unsupported);
    for (uint32_t i = 0; i < 8; ++i)
        REQUIRE(preparer.SubmitWarmTextures(i, preparer.QueryRadius(i), fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Submitted);
    CHECK(preparer.SubmitWarmTextures(8, preparer.QueryRadius(8), fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Capacity);
    CHECK(preparer.SubmitWarmTextures(0, preparer.QueryRadius(0), fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Coalesced);
    auto stats = preparer.SnapshotStats(); CHECK(stats.warmTextureQueued == 8); CHECK(stats.warmTextureActive == 0);
    CHECK(stats.warmTextureMetadataBytes <= WarmTextureJobPolicy::MetadataLimit); CHECK(stats.warmTextureScratchBytes == 0);
    std::array<uint32_t, 9> stamps{}; preparer.DropStale(stamps.data(), stamps.size(), 1);
    stats = preparer.SnapshotStats(); CHECK(stats.warmTextureQueued == 0); CHECK(stats.warmTextureMetadataBytes == 0);
    CHECK(stats.warmTextureJobsCancelled == 8); CHECK(stats.warmTextureMembersPrepared == 0);
    for (uint32_t i = 0; i < 9; ++i) CHECK(preparer.Query(i) == ObjectStreamPreparer::State::Unknown);
}

TEST_CASE("Warm slices retain a hard two-worker cap with unlimited soft budget and extra workers", "[warm-texture-many-workers]")
{
    RequireCapability();
    if (ObjectStreamPreparer::WorkerCount() < 3) SKIP("Fresh ASYNC_WORKERS=4 process required for extra-worker regression.");
    Fixture fixture; auto& store = PreparedTextureStore::Instance(); store.Clear();
    Gate gate; render::DdsPublicationObserver observer{}; observer.context = &gate; observer.beforePaaPut = &Gate::BeforePut;
    ObjectStreamPreparer preparer(0, nullptr, &observer); // zero means soft byte budget disabled
    ReleaseGuard release{gate};
    const std::array<std::string, 3> paths{"warm-cap-a.p3d", "warm-cap-b.p3d", "warm-cap-c.p3d"};
    preparer.Reset(paths.data(), paths.size());
    for (uint32_t i = 0; i < paths.size(); ++i)
        REQUIRE(preparer.SubmitWarmTextures(i, preparer.QueryRadius(i), fixture.Reads()) == ObjectStreamPreparer::WarmTextureSubmit::Submitted);
    REQUIRE(gate.WaitEntries(2));
    CHECK_FALSE(gate.WaitEntries(3, std::chrono::milliseconds(100))); // other workers must remain parked while two real Put calls are latched
    const auto active = preparer.SnapshotStats();
    CHECK(active.warmTextureActive == 2); CHECK(active.warmTextureQueued == 1);
    CHECK(active.warmTextureScratchBytes == 2 * WarmTextureJobPolicy::ScratchBytes);
    gate.Release(); REQUIRE(WaitDone(preparer));
    CHECK(preparer.SnapshotStats().warmTextureJobsCompleted == 3);
    CHECK(preparer.SnapshotStats().warmTextureScratchBytesPeak <= 2 * WarmTextureJobPolicy::ScratchBytes);
    store.Clear();
}
