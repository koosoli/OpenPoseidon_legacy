#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
namespace enf = Poseidon::Asset::Formats::Enfusion;
using Poseidon::ObjectStreamPreparer;
using Poseidon::render::PreparedTextureStore;
constexpr const char* Composite = "enfa|coverage.edds|colour.edds";
void Le32(std::vector<uint8_t>& out, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (i * 8))); }
void Be32(std::vector<uint8_t>& out, uint32_t v)
{ for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (i * 8))); }
void Tag(std::vector<uint8_t>& out, const char* v) { out.insert(out.end(), v, v + 4); }
void Word(std::vector<uint8_t>& out, size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); }
std::vector<uint8_t> Edds(bool coverage)
{
    std::vector<uint8_t> out(128, 0);
    std::memcpy(out.data(), "DDS ", 4);
    Word(out, 4, 124); Word(out, 8, 0x20000); Word(out, 12, 16); Word(out, 16, 16); Word(out, 28, 1);
    std::memcpy(out.data() + 36, "ENF1", 4);
    Word(out, 76, 32); Word(out, 80, 0x41); Word(out, 88, 32);
    Word(out, 92, 0xff0000); Word(out, 96, 0xff00); Word(out, 100, 0xff); Word(out, 104, 0xff000000);
    Le32(out, 0x59504f43); Le32(out, 16 * 16 * 4);
    for (unsigned i = 0; i < 256; ++i)
    {
        out.push_back(coverage ? (i % 3 ? 255 : 0) : static_cast<uint8_t>(i * 13));
        out.push_back(static_cast<uint8_t>(i * 7)); out.push_back(static_cast<uint8_t>(i * 23)); out.push_back(255);
    }
    return out;
}
struct LoaderGate
{
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked = false, entered = false;
    void Release() { std::lock_guard lock(mutex); blocked = false; cv.notify_all(); }
};
LoaderGate* gate = nullptr;
std::shared_ptr<Poseidon::Model::Model> NativeModel(const std::string& path, std::string&)
{
    if (gate)
    {
        std::unique_lock lock(gate->mutex);
        gate->entered = true; gate->cv.notify_all();
        gate->cv.wait(lock, [] { return !gate->blocked; });
    }
    auto model = std::make_shared<Poseidon::Model::Model>();
    model->sourcePath = path; model->sourceFormat = "MLOD";
    model->lodLevels.emplace_back(1.0f);
    model->lodLevels[0].mesh.materials.emplace_back("synthetic-native", Composite);
    return model;
}
struct Fixture
{
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("poseidon-bc3-worker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string oldRoot = enf::EnfusionMount::Instance().Root();
    Poseidon::ModelCache::ExternalLoader oldLoader = Poseidon::ModelCache::GetExternalLoader();
    bool oldCompress = Poseidon::Dev::GResidencyLevers().compressComposites;
    Fixture()
    {
        REQUIRE(std::filesystem::create_directory(directory));
        std::vector<uint8_t> data, index{0, 0};
        Le32(index, 2);
        for (const auto& item : {std::pair{"coverage.edds", true}, std::pair{"colour.edds", false}})
        {
            const auto pixels = Edds(item.second);
            const std::string name = item.first;
            index.push_back(1); index.push_back(static_cast<uint8_t>(name.size()));
            index.insert(index.end(), name.begin(), name.end());
            for (uint32_t v : {20u + static_cast<uint32_t>(data.size()), static_cast<uint32_t>(pixels.size()),
                               static_cast<uint32_t>(pixels.size()), 0u, 0u, 0u}) Le32(index, v);
            data.insert(data.end(), pixels.begin(), pixels.end());
        }
        std::vector<uint8_t> chunks;
        Tag(chunks, "DATA"); Be32(chunks, static_cast<uint32_t>(data.size())); chunks.insert(chunks.end(), data.begin(), data.end());
        Tag(chunks, "FILE"); Be32(chunks, static_cast<uint32_t>(index.size())); chunks.insert(chunks.end(), index.begin(), index.end());
        std::vector<uint8_t> pak; Tag(pak, "FORM"); Be32(pak, static_cast<uint32_t>(4 + chunks.size())); Tag(pak, "PAC1");
        pak.insert(pak.end(), chunks.begin(), chunks.end());
        std::ofstream file(directory / "test.pak", std::ios::binary);
        file.write(reinterpret_cast<const char*>(pak.data()), pak.size()); REQUIRE(file.good()); file.close();
        REQUIRE(enf::EnfusionMount::Instance().Open(directory.string()));
        PreparedTextureStore::Instance().Clear();
        Poseidon::Dev::GResidencyLevers().compressComposites = true;
        Poseidon::ModelCache::SetExternalLoader(NativeModel);
    }
    ~Fixture()
    {
        gate = nullptr;
        Poseidon::ModelCache::SetExternalLoader(oldLoader);
        Poseidon::Dev::GResidencyLevers().compressComposites = oldCompress;
        PreparedTextureStore::Instance().Clear();
        enf::EnfusionMount::Instance().Close();
        if (!oldRoot.empty()) enf::EnfusionMount::Instance().Open(oldRoot);
        std::error_code error;
        std::filesystem::remove(directory / "test.pak", error); std::filesystem::remove(directory, error);
    }
};
template<class Predicate> bool Wait(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while (std::chrono::steady_clock::now() < end);
    return predicate();
}
void RequireEnabled()
{
    if (!ObjectStreamPreparer::AsyncEnabled() || !PreparedTextureStore::NativeDdsEnabled())
        SKIP("Fresh process requires WGR_NATIVE_DDS_PREPARE=1 and asynchronous preparation");
    Poseidon::Foundation::CaptureMainThread();
}
}

TEST_CASE("Non-owner native requests retain ordinary preparation without consulting owner BC3 policy", "[preparer][bc3-worker][native-fixture]")
{
    RequireEnabled(); Fixture fixture;
    ObjectStreamPreparer prep(128ull * 1024 * 1024);
    const std::string path = "synthetic_bc3_nonowner.xob";
    prep.Reset(&path, 1);
    REQUIRE(Poseidon::Foundation::IsMainThread());
    bool accepted = false, owner = true;
    std::thread caller([&] {
        owner = Poseidon::Foundation::IsMainThread();
        accepted = prep.Request(0);
    });
    caller.join();
    CHECK_FALSE(owner); REQUIRE(accepted);
    REQUIRE(Wait([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    const auto stats = prep.SnapshotStats();
    CHECK(stats.workerBc3Prepared == 0); CHECK(stats.workerBc3BudgetSkipped == 0);
    CHECK(stats.textureEncodeReservedBytes == 0); CHECK(stats.peakTextureEncodeReservedBytes == 0);
    auto source = PreparedTextureStore::Instance().TakeDdsPrepared(Composite, Poseidon::CaptureDdsPreparationOptions());
    REQUIRE(source);
    Poseidon::Bc3MipChain blocks;
    CHECK_FALSE(source->TakeCompositeBc3(Composite, 16, 16, blocks));
    CHECK(source->GetFormat() == Poseidon::PacARGB8888);
    REQUIRE(prep.Take(0));
}

TEST_CASE("Native worker BC3 budget refusal retains ordinary DDS preparation and progresses", "[preparer][bc3-worker][native-fixture]")
{
    RequireEnabled(); Fixture fixture;
    ObjectStreamPreparer prep(1024);
    const std::string path = "synthetic_bc3_budget.xob";
    prep.Reset(&path, 1); REQUIRE(prep.Request(0));
    REQUIRE(Wait([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    const auto stats = prep.SnapshotStats();
    CHECK(stats.workerBc3BudgetSkipped == 1); CHECK(stats.workerBc3Prepared == 0);
    CHECK(stats.textureEncodeReservedBytes == 0); CHECK(stats.parseReservedBytes == 0);
    auto source = PreparedTextureStore::Instance().TakeDdsPrepared(Composite, Poseidon::CaptureDdsPreparationOptions());
    REQUIRE(source);
    Poseidon::Bc3MipChain blocks;
    CHECK_FALSE(source->TakeCompositeBc3(Composite, 16, 16, blocks));
    CHECK(source->GetFormat() == Poseidon::PacARGB8888);
    CHECK(source->CompositeBc3Reservation() > 1024);
    REQUIRE(prep.Take(0));
    CHECK(prep.SnapshotStats().readyPayloadBytes == 0);
}

TEST_CASE("Native worker publishes a claimable BC3 sidecar and releases its bounded reservation", "[preparer][bc3-worker][native-fixture]")
{
    RequireEnabled(); Fixture fixture;
    constexpr uint64_t budget = 128ull * 1024 * 1024;
    ObjectStreamPreparer prep(budget);
    const std::string path = "synthetic_bc3_success.xob";
    prep.Reset(&path, 1); REQUIRE(prep.Request(0));
    REQUIRE(Wait([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    const auto stats = prep.SnapshotStats();
    CHECK(stats.workerBc3Prepared == 1); CHECK(stats.workerBc3BudgetSkipped == 0);
    CHECK(stats.textureEncodeReservedBytes == 0); CHECK(stats.parseReservedBytes == 0);
    CHECK(stats.peakTextureEncodeReservedBytes > 0);
    CHECK(stats.peakTextureEncodeReservedBytes <= budget);
    auto source = PreparedTextureStore::Instance().TakeDdsPrepared(Composite, Poseidon::CaptureDdsPreparationOptions());
    REQUIRE(source);
    REQUIRE(source->GetMipmapCount() == 1);
    // Dimensions and pixel format are fixed by the synthetic EDDS fixture;
    // read through the public source API without exposing factory internals.
    Poseidon::PacLevelMem mip;
    mip._w = 16; mip._h = 16;
    mip._dFormat = Poseidon::PacARGB8888; mip._pitch = 16 * 4;
    std::vector<uint8_t> pixels(16 * 16 * 4);
    REQUIRE(source->GetMipmapData(pixels.data(), mip, 0));
    for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    Poseidon::Bc3MipChain expected;
    REQUIRE(Poseidon::EncodeBc3ChainRGBA(pixels.data(), 16, 16, expected.blocks, expected.offsets, expected.levels));
    const auto retained = source->PreparedByteSize();
    Poseidon::Bc3MipChain prepared;
    REQUIRE(source->TakeCompositeBc3(Composite, 16, 16, prepared));
    CHECK(prepared.blocks == expected.blocks); CHECK(prepared.offsets == expected.offsets);
    CHECK(prepared.levels == expected.levels);
    CHECK(retained - source->PreparedByteSize() == prepared.RetainedBytes());
    CHECK(source->CompositeBc3Reservation() == stats.peakTextureEncodeReservedBytes);
    CHECK_FALSE(source->TakeCompositeBc3(Composite, 16, 16, prepared));
    REQUIRE(prep.Take(0)); CHECK(prep.SnapshotStats().readyPayloadBytes == 0);
}

TEST_CASE("Native worker cancellation preserves active parse debt and publishes no stale DDS", "[preparer][bc3-worker][native-fixture]")
{
    RequireEnabled(); Fixture fixture;
    LoaderGate barrier; barrier.blocked = true; gate = &barrier;
    // Release before preparer destruction even if an assertion throws.
    ObjectStreamPreparer prep(128ull * 1024 * 1024);
    struct ReleaseOnExit { LoaderGate& value; ~ReleaseOnExit() { value.Release(); } } release{barrier};
    const std::string path = "synthetic_bc3_cancel.xob";
    prep.Reset(&path, 1); REQUIRE(prep.Request(0));
    {
        std::unique_lock lock(barrier.mutex);
        REQUIRE(barrier.cv.wait_for(lock, std::chrono::seconds(10), [&] { return barrier.entered; }));
    }
    const auto debt = prep.SnapshotStats().parseReservedBytes; REQUIRE(debt > 0);
    SECTION("Reset replaces inventory while old worker owns its reservation")
    {
        const std::string replacement = "synthetic_bc3_replacement.xob";
        prep.Reset(&replacement, 1);
    }
    SECTION("DropStale cancels current inventory without publishing textures")
    {
        const uint32_t epoch = 0; prep.DropStale(&epoch, 1, 1);
    }
    CHECK(prep.SnapshotStats().parseReservedBytes == debt);
    barrier.Release();
    REQUIRE(Wait([&] { return prep.SnapshotStats().parseReservedBytes == 0; }));
    CHECK(prep.Query(0) == ObjectStreamPreparer::State::Unknown);
    CHECK(prep.SnapshotStats().textureEncodeReservedBytes == 0);
    CHECK(prep.SnapshotStats().workerBc3Prepared == 0);
    CHECK_FALSE(PreparedTextureStore::Instance().TakeDdsPrepared(Composite, Poseidon::CaptureDdsPreparationOptions()));
}
