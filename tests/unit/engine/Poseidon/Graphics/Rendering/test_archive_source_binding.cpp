#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/FileAccessPolicy.hpp>
#include <Poseidon/IO/FileServerMT.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <type_traits>

using namespace Poseidon;
static_assert(!std::is_copy_constructible_v<ArchiveSourceBudget::Ticket>);
static_assert(!std::is_move_constructible_v<ArchiveSourceBudget::Ticket>);

TEST_CASE("Archive binding debt remains charged through copied aliases", "[archive-source-binding][archive-source-budget]")
{
    auto budget = std::make_shared<ArchiveSourceBudget>(2, 100);
    auto first = budget->Acquire(40); REQUIRE(first);
    auto alias = first; first.reset();
    CHECK(budget->Observe().bindings == 1); CHECK(budget->Observe().bytes == 40);
    bool capacity = false;
    CHECK_FALSE(budget->Acquire(61, &capacity)); CHECK(capacity);
    auto second = budget->Acquire(60); REQUIRE(second);
    CHECK_FALSE(budget->Acquire(1));
    alias.reset(); CHECK(budget->Observe().bytes == 60);
    auto replacement = budget->Acquire(40); REQUIRE(replacement);
    second.reset(); replacement.reset();
    CHECK(budget->Observe().bindings == 0); CHECK(budget->Observe().bytes == 0);
    CHECK_FALSE(budget->Acquire(0)); CHECK_FALSE(budget->Acquire(101));
}

namespace
{
std::vector<char> OriginalPaa(unsigned char seed)
{
    std::vector<char> data;
    auto word = [&](unsigned value) { data.push_back(char(value)); data.push_back(char(value >> 8)); };
    word(0xff05); word(0); // BC3 magic, no palette/taggs.
    for (unsigned side : {16u, 8u, 4u})
    {
        word(side); word(side);
        const unsigned bytes = ((side + 3) / 4) * ((side + 3) / 4) * 16;
        data.push_back(char(bytes)); data.push_back(char(bytes >> 8)); data.push_back(char(bytes >> 16));
        for (unsigned i = 0; i < bytes; ++i) data.push_back(char(seed + i * 17));
    }
    word(0); word(0);
    return data;
}
struct Archives
{
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("archive-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    QFBank first, second;
    Archives()
    {
        Foundation::CaptureMainThread();
        REQUIRE(std::filesystem::create_directory(directory));
        for (int index = 0; index < 2; ++index)
        {
            const auto data = OriginalPaa(index ? 87 : 13);
            std::ofstream out(directory / (index ? "second.pbo" : "first.pbo"), std::ios::binary);
            REQUIRE(out.good());
            auto entry = [&](const char* member, int size)
            {
                out.write(member, std::strlen(member) + 1);
                for (int value : {0, 0, 0, 0, size}) out.write(reinterpret_cast<const char*>(&value), 4);
            };
            entry("texture.paa", int(data.size())); entry("notes.bin", int(data.size())); entry("bad.paa", 0); entry("", 0);
            out.write(data.data(), data.size()); out.write(data.data(), data.size());
        }
        REQUIRE(first.open(RString((directory / "first").string().c_str())));
        REQUIRE(second.open(RString((directory / "second").string().c_str())));
        first.Lock(); second.Lock();
    }
    ~Archives()
    {
        if (first.IsLocked()) first.Unlock();
        if (second.IsLocked()) second.Unlock();
        first.close(); second.close();
        std::error_code error;
        std::filesystem::remove(directory / "first.pbo", error); error.clear();
        std::filesystem::remove(directory / "second.pbo", error); error.clear();
        std::filesystem::remove(directory, error); // No recursive deletion.
    }
};
class CachedServer final : public FileServer
{
public:
    FileCache cache;
    void Request(const char*, float, int, int) override {}
    void CancelRequest(const char*, int, int) override {}
    void Open(QIFStream& stream, const char* name) override { cache.Open(stream, name); }
    void Start() override {} void Stop() override {}
    void FlushBank(QFBank* bank) override { cache.FlushBank(bank); }
};
struct ServerScope
{
    Ref<FileServer> original = GFileServer;
    bool caching = GEnableCaching;
    CachedServer* server = new CachedServer;
    ServerScope() { GEnableCaching = true; GFileServer = server; }
    ~ServerScope() { GFileServer = original; GEnableCaching = caching; }
};
std::unique_ptr<ITextureSource> CreateSource(const char* name)
{
    std::array<PacLevelMem, 7> mips;
    auto* factory = SelectTextureSourceFactory(name); REQUIRE(factory);
    return std::unique_ptr<ITextureSource>(factory->Create(name, mips.data(), int(mips.size())));
}
}

TEST_CASE("Archive provenance never changes default bank bytes or adds OFF captures", "[archive-source-binding][archive-source-default]")
{
    if (ArchiveSourceBinding::SnapshotStats().enabled) SKIP("Run this default-arm case with WARM_TEXTURES unset.");
    Archives files;
    auto buffer = files.first.Read("texture.paa"); REQUIRE(buffer);
    const auto expected = OriginalPaa(13);
    REQUIRE(buffer->GetSize() == int(expected.size())); CHECK_FALSE(buffer->GetError());
    CHECK(std::memcmp(buffer->GetData(), expected.data(), expected.size()) == 0);
    CHECK_FALSE(buffer->GetArchiveSourceBinding());
    ServerScope scope;
    QIFStreamB cached; cached.open(files.first, "texture.paa");
    scope.server->cache.Store(cached, "binding-fixture\\texture.paa");
    auto source = CreateSource("binding-fixture\\texture.paa"); REQUIRE(source);
    CHECK_FALSE(source->GetArchiveSourceBinding());
    const auto stats = ArchiveSourceBinding::SnapshotStats();
    CHECK(stats.liveBindings == 0); CHECK(stats.knownCppBytes == 0);
    CHECK(stats.wrappedReads == 0); CHECK(stats.initRetains == 0);
}

TEST_CASE("PAA source retains exact cached physical member across mount selection changes", "[archive-source-binding][archive-source-actual][archive-lease]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    if (!ArchiveSourceBinding::SnapshotStats().enabled) SKIP("Requires fresh WGR_OBJECT_STREAM_WARM_TEXTURES=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Exact raw mapped buffer capability unavailable.");
    Archives files;
    ServerScope scope;
    QIFStreamB oldRead; oldRead.open(files.first, "texture.paa");
    REQUIRE_FALSE(oldRead.fail());
    auto oldBinding = oldRead.GetBuffer()->GetArchiveSourceBinding(); REQUIRE(oldBinding);
    auto firstRequest = files.first.CaptureReadRequest("texture.paa", true); REQUIRE(firstRequest);
    CHECK(oldBinding->Request().SameArchiveMember(*firstRequest));
    const auto expected = OriginalPaa(13);
    CHECK(std::memcmp(oldRead.GetBuffer()->GetData(), expected.data(), expected.size()) == 0);
    scope.server->cache.Store(oldRead, "binding-fixture\\texture.paa");
    auto source = CreateSource("binding-fixture\\texture.paa"); REQUIRE(source);
    REQUIRE(source->GetArchiveSourceBinding());
    CHECK(source->GetArchiveSourceBinding() == oldBinding);

    QIFStreamB replacement; replacement.open(files.second, "texture.paa");
    REQUIRE_FALSE(replacement.fail());
    auto newBinding = replacement.GetBuffer()->GetArchiveSourceBinding(); REQUIRE(newBinding);
    CHECK_FALSE(oldBinding->Request().SameArchiveMember(newBinding->Request()));
    // FileCache deliberately keeps the earlier buffer even if selection now names another bank.
    scope.server->cache.Store(replacement, "binding-fixture\\texture.paa");
    auto cachedAgain = CreateSource("binding-fixture\\texture.paa"); REQUIRE(cachedAgain);
    CHECK(cachedAgain->GetArchiveSourceBinding() == oldBinding);
    files.first.Unlock(); files.first.close(); // Native lease persists independently of QFBank.
    std::vector<char> reread;
    REQUIRE(oldBinding->Request().Read(reread)); CHECK(reread == expected);

    scope.server->cache.FlushBank(nullptr);
    scope.server->cache.Store(replacement, "binding-fixture\\texture.paa");
    auto fresh = CreateSource("binding-fixture\\texture.paa"); REQUIRE(fresh);
    CHECK(fresh->GetArchiveSourceBinding() == newBinding);
    CHECK(source->GetArchiveSourceBinding() == oldBinding); // Source aliases do not borrow the cache buffer.
    oldRead.DoDestruct();
    firstRequest.reset(); oldBinding.reset();
    REQUIRE(source->GetArchiveSourceBinding()->Request().Read(reread));
    CHECK(reread == expected); // Only source metadata remains; no Init buffer needed.
    CHECK(ArchiveSourceBinding::SnapshotStats().wrappedReads > 0);
    CHECK(ArchiveSourceBinding::SnapshotStats().initRetains > 0);
#endif
}

TEST_CASE("Unwrapped cached PAA and failed headers never gain guessed provenance", "[archive-source-binding][archive-source-actual]")
{
    if (!ArchiveSourceBinding::SnapshotStats().enabled) SKIP("Requires fresh WGR_OBJECT_STREAM_WARM_TEXTURES=1 process.");
    Archives files;
    ServerScope scope;
    const auto bytes = OriginalPaa(13);
    Ref<FileBufferMemory> plain = new FileBufferMemory(int(bytes.size()));
    std::memcpy(plain->GetWritableData(), bytes.data(), bytes.size());
    QIFStreamB cached; cached.OpenBuffer(plain.GetRef());
    scope.server->cache.Store(cached, "binding-fixture\\texture.paa");
    auto source = CreateSource("binding-fixture\\texture.paa"); REQUIRE(source);
    CHECK_FALSE(source->GetArchiveSourceBinding());
    auto ordinary = files.first.Read("notes.bin"); REQUIRE(ordinary);
    CHECK_FALSE(ordinary->GetArchiveSourceBinding());
    scope.server->cache.FlushBank(nullptr);
    QIFStreamB bad; bad.open(files.first, "bad.paa");
    scope.server->cache.Store(bad, "binding-fixture\\bad.paa");
    auto failed = CreateSource("binding-fixture\\bad.paa");
    CHECK_FALSE(failed);
}

TEST_CASE("Actual archive binding cap refuses metadata while preserving original bytes", "[archive-source-binding][archive-source-actual][archive-lease]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    auto before = ArchiveSourceBinding::SnapshotStats();
    if (!before.enabled) SKIP("Requires fresh WGR_OBJECT_STREAM_WARM_TEXTURES=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Exact raw mapped buffer capability unavailable.");
    Archives files;
    std::vector<Ref<IFileBuffer>> held;
    for (size_t i = 0; i < before.capBindings + 1; ++i)
    {
        auto read = files.first.Read("texture.paa"); REQUIRE(read);
        held.push_back(read);
    }
    const auto full = ArchiveSourceBinding::SnapshotStats();
    CHECK(full.liveBindings <= full.capBindings); CHECK(full.knownCppBytes <= full.capBytes);
    CHECK(full.capacityRefused > before.capacityRefused);
    auto refused = files.first.Read("texture.paa"); REQUIRE(refused);
    CHECK_FALSE(refused->GetArchiveSourceBinding());
    const auto expected = OriginalPaa(13);
    CHECK(std::memcmp(refused->GetData(), expected.data(), expected.size()) == 0);
    auto alias = held.front()->GetArchiveSourceBinding(); REQUIRE(alias);
    held.clear(); CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings + 1);
    alias.reset(); CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings);
#endif
}
