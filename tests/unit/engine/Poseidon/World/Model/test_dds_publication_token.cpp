#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/PreparedDdsPublication.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>

using Poseidon::render::DdsPublicationInventory;
using Poseidon::render::DdsPublicationToken;

TEST_CASE("DDS publication epochs invalidate cancelled models independently", "[preparer][dds-publication]")
{
    auto inventory = DdsPublicationInventory::Create(2);
    REQUIRE(inventory);
    DdsPublicationToken old{inventory, 0, inventory->Epoch(0)};
    DdsPublicationToken other{inventory, 1, inventory->Epoch(1)};
    REQUIRE(old.Valid()); REQUIRE(other.Valid());
    inventory->CancelModel(0);
    CHECK_FALSE(old.Valid()); CHECK(other.Valid());
    DdsPublicationToken replacement{inventory, 0, inventory->Epoch(0)};
    CHECK(replacement.Valid());
    inventory->CancelModel(99);
    CHECK(replacement.Valid()); CHECK(other.Valid());
    inventory->InvalidateAll();
    CHECK_FALSE(replacement.Valid()); CHECK_FALSE(other.Valid());
    auto reset = DdsPublicationInventory::Create(2);
    DdsPublicationToken newWorld{reset, 0, reset->Epoch(0)};
    CHECK(newWorld.Valid()); CHECK_FALSE(old.Valid());
}

TEST_CASE("DDS publication metadata is bounded and unknown tokens fail closed", "[preparer][dds-publication]")
{
    CHECK_FALSE(DdsPublicationInventory::Create(DdsPublicationInventory::MaxModels + 1));
    auto inventory = DdsPublicationInventory::Create(3);
    REQUIRE(inventory);
    CHECK(inventory->MetadataBytes() >= 3 * sizeof(std::atomic<uint64_t>));
    CHECK_FALSE(DdsPublicationToken{}.Valid());
    CHECK_FALSE((DdsPublicationToken{inventory, 3, 1}).Valid());
    CHECK_FALSE((DdsPublicationToken{inventory, 0, 0}).Valid());
}

TEST_CASE("Prepared DDS claims reject cancelled epochs and permit same-key replacements", "[preparer][dds-publication]")
{
    using Poseidon::render::PreparedTextureStore;
    if (!PreparedTextureStore::Enabled()) SKIP("Texture preparation disabled");
    auto& store = PreparedTextureStore::Instance();
    store.Clear();
    struct ClearOnExit { ~ClearOnExit() { PreparedTextureStore::Instance().Clear(); } } clear;
    const char* key = "enfc|0.2,0.3,0.4";
    const Poseidon::DdsPreparationOptions options{true, true, 1024};
    auto source = [&] {
        auto result = std::make_unique<Poseidon::TextureSourceDDS>();
        Poseidon::PacLevelMem mips[16];
        REQUIRE(result->InitFromReader(key, mips, 16,
            [](const char*, std::vector<uint8_t>&) { return false; }, options));
        return result;
    };
    auto inventory = DdsPublicationInventory::Create(1);
    DdsPublicationToken old{inventory, 0, inventory->Epoch(0)};
    REQUIRE(store.PutDdsPrepared(key, source(), options, store.Generation(), &old));
    inventory->CancelModel(0);
    CHECK_FALSE(store.TakeDdsPrepared(key, options));
    CHECK(store.SnapshotStats().entries == 0);
    DdsPublicationToken replacement{inventory, 0, inventory->Epoch(0)};
    REQUIRE(store.PutDdsPrepared(key, source(), options, store.Generation(), &replacement));
    inventory->CancelModel(0);
    REQUIRE(store.ShouldPrepare(key));
    DdsPublicationToken next{inventory, 0, inventory->Epoch(0)};
    REQUIRE(store.PutDdsPrepared(key, source(), options, store.Generation(), &next));
    auto claimed = store.TakeDdsPrepared(key, options);
    REQUIRE(claimed);
    // An already claimed source remains the owner's responsibility after Reset.
    inventory->InvalidateAll();
    CHECK(claimed->GetMipmapCount() > 0);
    CHECK_FALSE(store.PutDdsPrepared(key, source(), options, store.Generation(), &next));
    REQUIRE(store.ShouldPrepare(key));
    auto fresh = DdsPublicationInventory::Create(1);
    DdsPublicationToken newWorld{fresh, 0, fresh->Epoch(0)};
    REQUIRE(store.PutDdsPrepared(key, source(), options, store.Generation(), &newWorld));
    store.Clear();
    CHECK_FALSE(store.TakeDdsPrepared(key, options));
}

TEST_CASE("Worker cancellation retirement visits a bounded slice and progresses across calls", "[preparer][dds-publication]")
{
    using Poseidon::render::PreparedTextureStore;
    if (!PreparedTextureStore::Enabled()) SKIP("Texture preparation disabled");
    auto& store = PreparedTextureStore::Instance(); store.Clear();
    struct ClearOnExit { ~ClearOnExit() { PreparedTextureStore::Instance().Clear(); } } clear;
    const Poseidon::DdsPreparationOptions options{true, true, 1024};
    auto inventory = DdsPublicationInventory::Create(1);
    DdsPublicationToken token{inventory, 0, inventory->Epoch(0)};
    for (unsigned i = 0; i < 40; ++i)
    {
        const std::string key = "enfc|0.2,0.3," + std::to_string(i / 100.0);
        auto source = std::make_unique<Poseidon::TextureSourceDDS>();
        Poseidon::PacLevelMem mips[16];
        REQUIRE(source->InitFromReader(key.c_str(), mips, 16,
            [](const char*, std::vector<uint8_t>&) { return false; }, options));
        REQUIRE(store.PutDdsPrepared(key, std::move(source), options, store.Generation(), &token));
    }
    REQUIRE(store.SnapshotStats().entries == 40);
    inventory->InvalidateAll();
    REQUIRE(store.ShouldPrepare("new-worker-name.edds"));
    CHECK(store.SnapshotStats().entries == 8);
    REQUIRE(store.ShouldPrepare("new-worker-name.edds"));
    CHECK(store.SnapshotStats().entries == 0);
    CHECK(store.SnapshotStats().bytes == 0);
}
