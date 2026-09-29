#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/Graphics/Textures/HotSourceProofRetirementPolicy.hpp>
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
#include <future>
#include <stdexcept>

using namespace Poseidon;
static_assert(!std::is_copy_constructible_v<ArchiveSourceBudget::Ticket>);
static_assert(!std::is_move_constructible_v<ArchiveSourceBudget::Ticket>);
static_assert(!std::is_copy_constructible_v<ArchiveSourceBinding::ModelReadScope>);
static_assert(!std::is_move_constructible_v<ArchiveSourceBinding::ModelReadScope>);

TEST_CASE("Selected warm-stage archive reasons accept only two exact bounded paths", "[archive-source-binding][stage-reason]")
{
    using namespace PatnikArchiveReason;
    CHECK(Select(nullptr) == Patnik);
    CHECK(Select("patnik") == Patnik);
    CHECK(Select("tree") == Tree);
    CHECK(Select("other").empty());
    CHECK(Matches(Patnik, R"(dz\structures\residential\misc\data\patnik_smdi.paa)"));
    CHECK(Matches(Tree, R"(DZ/PLANTS/TREE/DATA/T_PICEAABIES_TRUNK_NO.PAA)"));
    CHECK_FALSE(Matches(Patnik, R"(other\patnik_smdi.paa)"));
    CHECK_FALSE(Matches(Tree, R"(dz\plants\tree\data\t_piceaabies_trunk_no.paa.bak)"));
    CHECK(MatchesJoined(Tree, R"(dz\plants\tree\)", R"(data\t_piceaabies_trunk_no.paa)"));
    CHECK_FALSE(MatchesJoined(Tree, R"(other\plants\tree\)", R"(data\t_piceaabies_trunk_no.paa)"));
    CHECK_FALSE(Matches(Patnik, nullptr));
    CHECK_FALSE(Matches(Patnik, std::string(8192, 'a').c_str()));
}

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
const std::string& LongMember()
{
    static const std::string member = std::string(123, 'a') + ".paa"; // Fits exact127-byte legacy lookup.
    return member;
}
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
            entry("texture.paa", int(data.size())); entry("notes.bin", int(data.size()));
            entry("plaster_flats01_nohq.paa", int(data.size()));
            entry(LongMember().c_str(), int(data.size())); entry("bad.paa", 0); entry("malformed.paa", 8); entry("", 0);
            out.write(data.data(), data.size()); out.write(data.data(), data.size());
            out.write(data.data(), data.size()); out.write(data.data(), data.size());
            const char malformed[8] = {char(0x05), char(0xff), char(1), char(1), 0, 0, 0, 0};
            out.write(malformed, sizeof(malformed));
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

TEST_CASE("Building pilot copies exact mapped member identity through cache and Pac Init",
    "[archive-member-identity][archive-source-actual]")
{
#ifndef _WIN32
    SKIP("Physical PBO member identity is Windows-only.");
#else
    const char* pilot = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_PILOT");
    if (!pilot || std::strcmp(pilot, "1") != 0)
        SKIP("Requires fresh WGR_OBJECT_STREAM_RAP_BUILDING_PILOT=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Raw mapped bank read unavailable.");
    Archives files;
    constexpr const char* normal = "plaster_flats01_nohq.paa";
    constexpr const char* path = R"(dz\structures\data\plaster\plaster_flats01_nohq.paa)";
    files.first.SetPrefix(R"(dz\structures\data\plaster\)");
    files.second.SetPrefix(R"(dz\structures\data\plaster\)");
    ArchiveSourceBinding::ModelReadScope purpose;
    std::vector<Ref<IFileBuffer>> held;
    if (ArchiveSourceBinding::SnapshotStats().enabled)
    {
        // Saturate the ordinary source-binding budget. The numeric witness
        // must still describe the exact mapped buffer at the next read.
        while (ArchiveSourceBinding::SnapshotStats().liveBindings <
            ArchiveSourceBinding::ModelReadScope::BindingLimit())
        {
            auto read = files.first.Read("texture.paa"); REQUIRE(read);
            REQUIRE(read->GetArchiveSourceBinding());
            held.push_back(read);
        }
    }
    auto old = files.first.CaptureReadRequest(normal, true); REQUIRE(old);
    BankReadMemberIdentity expected;
    REQUIRE(old->CopyMemberIdentity(expected));
    auto raw = files.first.Read(normal); REQUIRE(raw);
    BankReadMemberIdentity actual;
    REQUIRE(raw->CopyArchiveMemberIdentity(actual));
    CHECK(actual == expected);
    if (ArchiveSourceBinding::SnapshotStats().enabled)
        CHECK_FALSE(raw->GetArchiveSourceBinding());
    ServerScope server;
    QIFStreamB cached; cached.open(files.first, normal); REQUIRE_FALSE(cached.fail());
    server.server->cache.Store(cached, path);
    auto source = CreateSource(path); REQUIRE(source);
    BankReadMemberIdentity parsed;
    REQUIRE(source->CopyArchiveMemberIdentity(parsed));
    CHECK(parsed == expected);
    auto newer = files.second.CaptureReadRequest(normal, true); REQUIRE(newer);
    BankReadMemberIdentity changed;
    REQUIRE(newer->CopyMemberIdentity(changed));
    CHECK_FALSE(parsed == changed);
    CHECK(files.first.MatchesMountedMember(normal, *old));
    CHECK_FALSE(files.second.MatchesMountedMember(normal, *old));
    // Reusing the old cache cannot relabel its parsed bytes as the new mount.
    auto stale = CreateSource(path); REQUIRE(stale);
    BankReadMemberIdentity staleIdentity;
    REQUIRE(stale->CopyArchiveMemberIdentity(staleIdentity));
    CHECK(staleIdentity == expected);
    FileBufferMemory loose(4);
    CHECK_FALSE(loose.CopyArchiveMemberIdentity(actual));
    held.clear();
#endif
}

TEST_CASE("Archive provenance never changes default bank bytes or adds OFF captures", "[archive-source-binding][archive-source-default]")
{
    if (ArchiveSourceBinding::SnapshotStats().enabled) SKIP("Run this default-arm case with WARM_TEXTURES unset.");
    Archives files;
    CHECK_FALSE(ArchiveSourceBinding::ModelReadScope::PurposeRequired());
    ArchiveSourceBinding::ModelReadScope optionalScope;
    CHECK_FALSE(ArchiveSourceBinding::ModelReadScope::IsActive());
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
    BankReadMemberIdentity oldMember, newMember;
    REQUIRE(oldBinding->Request().CopyMemberIdentity(oldMember));
    REQUIRE(newBinding->Request().CopyMemberIdentity(newMember));
    CHECK_FALSE(oldMember == newMember);
    BankReadMemberIdentity sameMember;
    REQUIRE(firstRequest->CopyMemberIdentity(sameMember));
    CHECK(oldMember == sameMember);
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
    // A zero-length member would fail native capture if reached. Saturation refuses
    // before CaptureReadRequest instead, preserving its original readable empty buffer.
    const auto preSkipped = ArchiveSourceBinding::SnapshotStats();
    auto empty = files.first.Read("bad.paa"); REQUIRE(empty); CHECK(empty->GetSize() == 0);
    const auto skipped = ArchiveSourceBinding::SnapshotStats();
    CHECK(skipped.capacityRefused == preSkipped.capacityRefused + 1);
    CHECK(skipped.captureRefused == preSkipped.captureRefused);
    auto refused = files.first.Read("texture.paa"); REQUIRE(refused);
    CHECK_FALSE(refused->GetArchiveSourceBinding());
    const auto expected = OriginalPaa(13);
    CHECK(std::memcmp(refused->GetData(), expected.data(), expected.size()) == 0);
    auto alias = held.front()->GetArchiveSourceBinding(); REQUIRE(alias);
    held.clear(); CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings + 1);
    alias.reset(); CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings);
#endif
}

TEST_CASE("Actual material preflight retains two existing archive slots after ordinary captures stop",
    "[archive-source-material-reserve][archive-source-actual]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    if (!ArchiveSourceBinding::ModelReadScope::MaterialPreflightReservationEnabled())
        SKIP("Requires fresh WARM_TEXTURES=1 WARM_TEXTURE_JOBS=1 WARM_MATERIAL_PREFLIGHT=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Exact raw mapped buffer capability unavailable.");
    Archives files;
    const auto before = ArchiveSourceBinding::SnapshotStats();
    REQUIRE(before.enabled);
    REQUIRE(before.liveBindings < 254);
    REQUIRE(ArchiveSourceBinding::ModelReadScope::BindingLimit() == 254);
    std::vector<Ref<IFileBuffer>> held;
    held.reserve(256 - before.liveBindings);
    {
        ArchiveSourceBinding::ModelReadScope normal;
        while (ArchiveSourceBinding::SnapshotStats().liveBindings < 254)
        {
            auto read = files.first.Read("texture.paa"); REQUIRE(read);
            REQUIRE(read->GetArchiveSourceBinding());
            held.push_back(read);
        }
        auto refused = files.first.Read("texture.paa"); REQUIRE(refused);
        CHECK_FALSE(refused->GetArchiveSourceBinding());
        const auto original = OriginalPaa(13);
        CHECK(std::memcmp(refused->GetData(), original.data(), original.size()) == 0);
        CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == 254);
        {
            ArchiveSourceBinding::ModelReadScope stage(
                ArchiveSourceBinding::ModelReadScope::Intent::MaterialPreflight);
            REQUIRE(ArchiveSourceBinding::ModelReadScope::IsMaterialPreflightActive());
            REQUIRE(ArchiveSourceBinding::ModelReadScope::BindingLimit() == 256);
            auto first = files.first.Read("texture.paa"); REQUIRE(first);
            auto firstBinding = first->GetArchiveSourceBinding(); REQUIRE(firstBinding);
            auto actualMember = files.first.CaptureReadRequest("texture.paa", true); REQUIRE(actualMember);
            CHECK(firstBinding->Request().SameArchiveMember(*actualMember));
            CHECK(std::memcmp(first->GetData(), original.data(), original.size()) == 0);
            held.push_back(first);
            {
                ArchiveSourceBinding::ModelReadScope nestedNormal;
                REQUIRE(ArchiveSourceBinding::ModelReadScope::IsMaterialPreflightActive());
                auto second = files.first.Read("texture.paa"); REQUIRE(second);
                REQUIRE(second->GetArchiveSourceBinding());
                held.push_back(second);
            }
            CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == 256);
            auto full = files.first.Read("texture.paa"); REQUIRE(full);
            CHECK_FALSE(full->GetArchiveSourceBinding());
        }
        CHECK_FALSE(ArchiveSourceBinding::ModelReadScope::IsMaterialPreflightActive());
        CHECK(ArchiveSourceBinding::ModelReadScope::BindingLimit() == 254);
        auto stillOrdinaryRefused = files.first.Read("texture.paa"); REQUIRE(stillOrdinaryRefused);
        CHECK_FALSE(stillOrdinaryRefused->GetArchiveSourceBinding());
    }
    CHECK_FALSE(ArchiveSourceBinding::ModelReadScope::IsActive());
    auto worker = std::async(std::launch::async, [] {
        ArchiveSourceBinding::ModelReadScope stage(
            ArchiveSourceBinding::ModelReadScope::Intent::MaterialPreflight);
        return ArchiveSourceBinding::ModelReadScope::IsActive() ||
            ArchiveSourceBinding::ModelReadScope::IsMaterialPreflightActive();
    });
    CHECK_FALSE(worker.get());
    held.clear();
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings);
    CHECK(ArchiveSourceBinding::SnapshotStats().knownCppBytes <= before.capBytes);
#endif
}

TEST_CASE("Material preflight reservation OFF leaves ordinary actual PBO reads eligible through 256",
    "[archive-source-material-reserve-off][archive-source-actual]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    if (ArchiveSourceBinding::ModelReadScope::MaterialPreflightReservationEnabled())
        SKIP("Requires fresh WARM_TEXTURES=1 WARM_TEXTURE_JOBS=1 WARM_MATERIAL_PREFLIGHT=0 process.");
    if (!ArchiveSourceBinding::ModelReadScope::PurposeRequired())
        SKIP("Requires fresh WARM_TEXTURES=1 WARM_TEXTURE_JOBS=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Exact raw mapped buffer capability unavailable.");
    Archives files;
    const auto before = ArchiveSourceBinding::SnapshotStats();
    REQUIRE(before.enabled);
    REQUIRE(before.liveBindings < before.capBindings);
    REQUIRE(ArchiveSourceBinding::ModelReadScope::BindingLimit() == before.capBindings);
    std::vector<Ref<IFileBuffer>> held;
    held.reserve(before.capBindings - before.liveBindings);
    ArchiveSourceBinding::ModelReadScope normal;
    while (ArchiveSourceBinding::SnapshotStats().liveBindings < before.capBindings)
    {
        auto read = files.first.Read("texture.paa"); REQUIRE(read);
        REQUIRE(read->GetArchiveSourceBinding());
        held.push_back(read);
    }
    ArchiveSourceBinding::ModelReadScope stage(
        ArchiveSourceBinding::ModelReadScope::Intent::MaterialPreflight);
    CHECK_FALSE(ArchiveSourceBinding::ModelReadScope::IsMaterialPreflightActive());
    auto full = files.first.Read("texture.paa"); REQUIRE(full);
    CHECK_FALSE(full->GetArchiveSourceBinding());
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.capBindings);
    held.clear();
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings);
#endif
}

TEST_CASE("Mounted member observation never loads an unopened bank or guesses identities", "[archive-source-binding][archive-source-budget][archive-lease]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    Archives files;
    auto request = files.first.CaptureReadRequest("texture.paa", true); REQUIRE(request);
    CHECK(files.first.MatchesMountedMember("texture.paa", *request));
    CHECK(files.first.MatchesMountedMember("TEXTURE.PAA", *request));
    CHECK_FALSE(files.first.MatchesMountedMember("notes.bin", *request)); // Equal length, different offset.
    CHECK_FALSE(files.second.MatchesMountedMember("texture.paa", *request)); // Equal metadata, different file.
    CHECK_FALSE(files.first.MatchesMountedMember("absent.paa", *request));
    CHECK_FALSE(files.first.MatchesMountedMember(nullptr, *request));
    CHECK_FALSE(files.first.MatchesMountedMember("", *request));
    for (int mutation = 0; mutation < 3; ++mutation)
    {
        auto changed = *request;
        if (mutation == 0) ++changed.offset;
        else if (mutation == 1) ++changed.bytes;
        else changed.archive += ".wrong";
        CHECK_FALSE(files.first.MatchesMountedMember("texture.paa", changed));
    }
    auto unleased = *request; unleased.archiveLease.reset();
    CHECK_FALSE(files.first.MatchesMountedMember("texture.paa", unleased));
    auto longRequest = files.first.CaptureReadRequest(LongMember().c_str(), true); REQUIRE(longRequest);
    CHECK(files.first.MatchesMountedMember(LongMember().c_str(), *longRequest));
    // Legacy FindFileInfo would truncate this128-byte alias to the previous member.
    CHECK_FALSE(files.first.MatchesMountedMember((LongMember() + "x").c_str(), *longRequest));
    auto wrongThread = std::async(std::launch::async, [&] {
        return files.first.MatchesMountedMember("texture.paa", *request);
    });
    CHECK_FALSE(wrongThread.get());
    QFBank unopened;
    REQUIRE(unopened.open(RString((files.directory / "first").string().c_str())));
    CHECK_FALSE(unopened.MatchesMountedMember("texture.paa", *request));
    unopened.Lock();
    CHECK(unopened.MatchesMountedMember("texture.paa", *request)); // Explicit Load is required.
    unopened.Unlock(); unopened.close();
    files.first.Unlock(); files.first.close();
    CHECK_FALSE(files.first.MatchesMountedMember("texture.paa", *request));
#endif
}

TEST_CASE("Model primary purpose captures actual mapped bytes and restores nested owner scope", "[archive-source-model-purpose]")
{
#ifndef _WIN32
    SKIP("Required immutable source identity is Windows-only.");
#else
    if (!ArchiveSourceBinding::ModelReadScope::PurposeRequired()) SKIP("Fresh WARM_TEXTURES=1 WARM_TEXTURE_JOBS=1 process required.");
    if (!QFileAccess::MappingSupported()) SKIP("Original mapped source capability unavailable.");
    Archives files;
    REQUIRE_FALSE(ArchiveSourceBinding::ModelReadScope::IsActive());
    const auto before = ArchiveSourceBinding::SnapshotStats();
    auto outside = files.first.Read("texture.paa"); REQUIRE(outside);
    CHECK_FALSE(outside->GetArchiveSourceBinding());
    CHECK(ArchiveSourceBinding::SnapshotStats().wrappedReads == before.wrappedReads);
    std::shared_ptr<const ArchiveSourceBinding> retained;
    {
        ArchiveSourceBinding::ModelReadScope outer;
        REQUIRE(ArchiveSourceBinding::ModelReadScope::IsActive());
        auto actual = files.first.Read("texture.paa"); REQUIRE(actual);
        retained = actual->GetArchiveSourceBinding(); REQUIRE(retained);
        auto mounted = files.first.CaptureReadRequest("texture.paa", true); REQUIRE(mounted);
        CHECK(retained->Request().SameArchiveMember(*mounted));
        const auto expected = OriginalPaa(13);
        CHECK(std::memcmp(actual->GetData(), expected.data(), expected.size()) == 0);
        try
        {
            ArchiveSourceBinding::ModelReadScope inner;
            REQUIRE(ArchiveSourceBinding::ModelReadScope::IsActive());
            throw std::runtime_error("purpose unwind");
        }
        catch (const std::runtime_error&) {}
        REQUIRE(ArchiveSourceBinding::ModelReadScope::IsActive());
        auto worker = std::async(std::launch::async, [] {
            ArchiveSourceBinding::ModelReadScope noPrivilege;
            return ArchiveSourceBinding::ModelReadScope::IsActive();
        });
        CHECK_FALSE(worker.get()); // No off-owner QFBank read is attempted.
        CHECK(ArchiveSourceBinding::ModelReadScope::IsActive());
    }
    REQUIRE_FALSE(ArchiveSourceBinding::ModelReadScope::IsActive());
    auto after = files.first.Read("texture.paa"); REQUIRE(after);
    CHECK_FALSE(after->GetArchiveSourceBinding());
    CHECK(ArchiveSourceBinding::SnapshotStats().wrappedReads == before.wrappedReads + 1);
    std::vector<char> bytes; REQUIRE(retained->Request().Read(bytes)); CHECK(bytes == OriginalPaa(13));
    // A scope that throws without a surviving outer scope must clear its purpose too.
    try { ArchiveSourceBinding::ModelReadScope scope; throw std::runtime_error("outer unwind"); }
    catch (const std::runtime_error&) {}
    CHECK_FALSE(ArchiveSourceBinding::ModelReadScope::IsActive());
#endif
}
TEST_CASE("Entering model purpose cannot certify a previously unwrapped cached header", "[archive-source-model-purpose]")
{
#ifndef _WIN32
    SKIP("Required immutable source identity is Windows-only.");
#else
    if (!ArchiveSourceBinding::ModelReadScope::PurposeRequired()) SKIP("Fresh WARM_TEXTURES=1 WARM_TEXTURE_JOBS=1 process required.");
    if (!QFileAccess::MappingSupported()) SKIP("Original mapped source capability unavailable.");
    Archives files; ServerScope server;
    QIFStreamB earlier; earlier.open(files.first, "texture.paa"); REQUIRE_FALSE(earlier.fail());
    CHECK_FALSE(earlier.GetBuffer()->GetArchiveSourceBinding());
    server.server->cache.Store(earlier, R"(model-purpose-fixture\texture.paa)");
    const auto before = ArchiveSourceBinding::SnapshotStats();
    std::unique_ptr<ITextureSource> unknown, witnessed;
    {
        ArchiveSourceBinding::ModelReadScope primary;
        unknown = CreateSource(R"(model-purpose-fixture\texture.paa)"); REQUIRE(unknown);
        CHECK_FALSE(unknown->GetArchiveSourceBinding());
        CHECK(ArchiveSourceBinding::SnapshotStats().wrappedReads == before.wrappedReads);
        CHECK(ArchiveSourceBinding::SnapshotStats().initRetains == before.initRetains);
        // Only an original subsequent raw read can carry new evidence. No fresh
        // capture is attached to unknown's already-parsed/cached header.
        QIFStreamB original; original.open(files.first, "texture.paa"); REQUIRE_FALSE(original.fail());
        REQUIRE(original.GetBuffer()->GetArchiveSourceBinding());
        server.server->cache.FlushBank(nullptr);
        server.server->cache.Store(original, R"(model-purpose-fixture\texture.paa)");
        witnessed = CreateSource(R"(model-purpose-fixture\texture.paa)"); REQUIRE(witnessed);
        CHECK(witnessed->GetArchiveSourceBinding() == original.GetBuffer()->GetArchiveSourceBinding());
        CHECK_FALSE(unknown->GetArchiveSourceBinding());
    }
    CHECK_FALSE(ArchiveSourceBinding::ModelReadScope::IsActive());
    CHECK_FALSE(unknown->GetArchiveSourceBinding());
    REQUIRE(witnessed->GetArchiveSourceBinding());
    CHECK(ArchiveSourceBinding::SnapshotStats().initRetains == before.initRetains + 1);
#endif
}


TEST_CASE("Hot proof policy requires actual upload and cached alpha without extending Init lifetime", "[archive-source-hot-retire]")
{
    using Policy = render::HotSourceProofRetirementPolicy;
    bool attempted = false;
    CHECK_FALSE(Policy::Reserve(false, true, 1, 0, true, false, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, false, 1, 0, true, false, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, true, 0, 0, true, false, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, true, 1, -1, true, false, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, true, 1, 0, false, false, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, true, 1, 0, true, true, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, true, 1, 0, true, false, true, attempted));
    CHECK_FALSE(attempted);
    REQUIRE(Policy::Reserve(true, true, 1, 0, true, false, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, true, 1, 0, true, false, false, attempted));
    // GPU eviction does not rearm; a new actual Init is the only explicit reset.
    CHECK_FALSE(Policy::Reserve(true, true, 0, 0, true, false, false, attempted));
    CHECK_FALSE(Policy::Reserve(true, true, 2, 0, true, false, false, attempted));
    attempted = false;
    CHECK(Policy::Reserve(true, true, 2, 1, true, false, false, attempted));
}

TEST_CASE("Actual PAC Init proof retirement preserves headers reads and strong alias debt", "[archive-source-hot-retire][archive-source-actual]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    if (!ArchiveSourceBinding::SnapshotStats().enabled) SKIP("Requires fresh WARM_TEXTURES=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Exact mapped buffer unavailable.");
    Archives files;
    ServerScope scope;
    const auto baseline = ArchiveSourceBinding::SnapshotStats();
    ArchiveSourceBinding::ModelReadScope purpose;
    QIFStreamB captured; captured.open(files.first, "texture.paa"); REQUIRE_FALSE(captured.fail());
    auto alias = captured.GetBuffer()->GetArchiveSourceBinding(); REQUIRE(alias);
    scope.server->cache.Store(captured, "hot-proof-fixture\\texture.paa");
    std::array<PacLevelMem, 7> mips;
    auto* factory = SelectTextureSourceFactory("hot-proof-fixture\\texture.paa"); REQUIRE(factory);
    std::unique_ptr<ITextureSource> source(factory->Create("hot-proof-fixture\\texture.paa", mips.data(), int(mips.size())));
    REQUIRE(source); REQUIRE(source->GetArchiveSourceBinding() == alias);
    const auto format = source->GetFormat();
    const auto average = source->GetAverageColor();
    const bool alpha = source->IsAlpha(), transparent = source->IsTransparent();
    const int count = source->GetMipmapCount(); REQUIRE(count > 0); REQUIRE(count <= 7);
    std::vector<std::vector<char>> before(static_cast<size_t>(count)), after(static_cast<size_t>(count));
    std::vector<MipmapRead> reads(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        mips[i].SetDestFormat(format, 8);
        before[i].resize(static_cast<size_t>(mips[i].Size())); after[i].resize(before[i].size());
        reads[i] = {before[i].data(), &mips[i], i};
    }
    REQUIRE(source->GetMipmapChain(reads.data(), count));
    const auto authored = OriginalPaa(13);
    size_t authoredAt = 4; // Magic/palette, then each actual seven-byte mip header.
    for (int i = 0; i < count; ++i)
    {
        REQUIRE(authoredAt + 7 <= authored.size()); authoredAt += 7;
        REQUIRE(before[i].size() <= authored.size() - authoredAt);
        CHECK(std::memcmp(before[i].data(), authored.data() + authoredAt, before[i].size()) == 0);
        authoredAt += before[i].size();
    }
    auto offOwner = std::async(std::launch::async, [&] { return source->ReleaseArchiveSourceBinding(); });
    CHECK_FALSE(offOwner.get()); REQUIRE(source->GetArchiveSourceBinding() == alias);
    REQUIRE(source->ReleaseArchiveSourceBinding()); CHECK_FALSE(source->ReleaseArchiveSourceBinding());
    CHECK_FALSE(source->GetArchiveSourceBinding());
    { ArchiveSourceBinding::ModelReadScope nested; CHECK_FALSE(source->GetArchiveSourceBinding()); }
    CHECK(source->GetFormat() == format); CHECK(source->GetAverageColor() == average);
    CHECK(source->IsAlpha() == alpha); CHECK(source->IsTransparent() == transparent);
    CHECK(source->GetMipmapCount() == count);
    for (int i = 0; i < count; ++i) reads[i].mem = after[i].data();
    REQUIRE(source->GetMipmapChain(reads.data(), count)); CHECK(before == after);
    const auto owned = ArchiveSourceBinding::SnapshotStats();
    CHECK(owned.liveBindings == baseline.liveBindings + 1); // Buffer/cache/local aliases retain same charged record.
    std::vector<char> exact;
    REQUIRE(alias->Request().Read(exact)); CHECK(exact == OriginalPaa(13));
    captured.DoDestruct(); scope.server->cache.FlushBank(nullptr);
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == baseline.liveBindings + 1);
    alias.reset();
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == baseline.liveBindings);
    CHECK(ArchiveSourceBinding::SnapshotStats().knownCppBytes == baseline.knownCppBytes);
    CHECK_FALSE(source->GetArchiveSourceBinding()); // No named recapture after aliases exit.
#endif
}


TEST_CASE("Weak cells keep separate bounded debt inside the original shared byte budget", "[archive-source-budget]")
{
    auto budget = std::make_shared<ArchiveSourceBudget>(1, 100, 1);
    auto lease = budget->Acquire(40); REQUIRE(lease);
    auto weak = budget->AcquireWeak(50); REQUIRE(weak);
    CHECK(budget->Observe().bindings == 1); CHECK(budget->Observe().weakCells == 1);
    CHECK(budget->Observe().bytes == 90); CHECK(budget->Observe().weakBytes == 50);
    bool capacity = false;
    CHECK_FALSE(budget->AcquireWeak(1, &capacity)); CHECK(capacity);
    CHECK_FALSE(budget->Acquire(1)); // Existing strong count cap is unchanged.
    auto alias = weak; weak.reset(); lease.reset();
    CHECK(budget->Observe().bindings == 0); CHECK(budget->Observe().bytes == 50);
    CHECK_FALSE(budget->Acquire(51)); // Weak/control debt survives the final lease.
    lease = budget->Acquire(50); REQUIRE(lease);
    alias.reset(); CHECK(budget->Observe().weakCells == 0); CHECK(budget->Observe().weakBytes == 0);
    lease.reset(); CHECK(budget->Observe().bytes == 0);
}

TEST_CASE("Actual cached PAA hands off proof and preserves two initialized source aliases", "[archive-source-weak-cache]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    REQUIRE(ArchiveSourceBinding::CacheHandoffEnabled()); // Dedicated fresh four-flag process.
    REQUIRE(QFileAccess::MappingSupported());
    Archives files; ServerScope server; ArchiveSourceBinding::ModelReadScope purpose;
    const auto before = ArchiveSourceBinding::SnapshotStats();
    QIFStreamB cached; cached.open(files.first, "texture.paa"); REQUIRE_FALSE(cached.fail());
    auto exact = cached.GetBuffer()->GetArchiveSourceBinding(); REQUIRE(exact);
    auto wrongBuffer = files.second.Read("texture.paa"); REQUIRE(wrongBuffer);
    auto wrong = wrongBuffer->GetArchiveSourceBinding(); REQUIRE(wrong);
    CHECK_FALSE(cached.GetBuffer()->CompleteArchiveSourceInit(wrong));
    CHECK(cached.GetBuffer()->GetArchiveSourceBinding() == exact);
    auto offOwner = std::async(std::launch::async, [&] {
        return cached.GetBuffer()->CompleteArchiveSourceInit(exact);
    });
    CHECK_FALSE(offOwner.get()); CHECK(cached.GetBuffer()->GetArchiveSourceBinding() == exact);
    const auto uncompleted = ArchiveSourceBinding::SnapshotStats();
    CHECK(uncompleted.weakWrappersPublished == before.weakWrappersPublished + 2);
    CHECK(uncompleted.weakInitializedHandoffs == before.weakInitializedHandoffs);
    CHECK(uncompleted.weakFailedInitCompletions == before.weakFailedInitCompletions);
    wrong.reset(); wrongBuffer.Free();
    server.server->cache.Store(cached, R"(weak-proof-fixture\texture.paa)");
    auto first = CreateSource(R"(weak-proof-fixture\texture.paa)"); REQUIRE(first);
    auto second = CreateSource(R"(weak-proof-fixture\texture.paa)"); REQUIRE(second);
    REQUIRE(first->GetArchiveSourceBinding() == exact);
    REQUIRE(second->GetArchiveSourceBinding() == exact);
    CHECK_FALSE(files.second.MatchesMountedMember("texture.paa", exact->Request()));
    files.first.Unlock(); files.first.close();
    REQUIRE(files.first.open(RString((files.directory / "second").string().c_str())));
    files.first.Lock();
    CHECK_FALSE(files.first.MatchesMountedMember("texture.paa", exact->Request())); // Actual same-bank remount.
    CHECK(first->GetArchiveSourceBinding() == exact); // Stored birth evidence is never relabelled.
    const auto owned = ArchiveSourceBinding::SnapshotStats();
    CHECK(owned.liveBindings == before.liveBindings + 1);
    CHECK(owned.weakCells == before.weakCells + 1);
    CHECK(owned.weakInitializedHandoffs == before.weakInitializedHandoffs + 1);
    CHECK(owned.weakFailedInitCompletions == before.weakFailedInitCompletions);
    CHECK(owned.weakPeakReservationCells >= before.weakCells + 2);
    CHECK(owned.weakPeakReservationCells <= owned.capWeakCells);
    CHECK(owned.weakPeakReservationBytes <= owned.capBytes);
    REQUIRE(first->ReleaseArchiveSourceBinding());
    REQUIRE(second->GetArchiveSourceBinding() == exact);
    exact.reset();
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings + 1);
    REQUIRE(second->ReleaseArchiveSourceBinding());
    const auto released = ArchiveSourceBinding::SnapshotStats();
    CHECK(released.liveBindings == before.liveBindings);
    CHECK(released.weakCells == before.weakCells + 1);
    CHECK(released.weakKnownCppBytes > before.weakKnownCppBytes);
    CHECK(released.knownCppBytes == before.knownCppBytes + released.weakKnownCppBytes - before.weakKnownCppBytes);
    CHECK_FALSE(cached.GetBuffer()->GetArchiveSourceBinding());
    const auto bytes = OriginalPaa(13);
    REQUIRE(cached.GetBuffer()->GetSize() == int(bytes.size()));
    CHECK(std::memcmp(cached.GetBuffer()->GetData(), bytes.data(), bytes.size()) == 0);
    auto expired = CreateSource(R"(weak-proof-fixture\texture.paa)"); REQUIRE(expired);
    CHECK_FALSE(expired->GetArchiveSourceBinding()); // Existing bytes cannot recertify a new lease.
    CHECK(ArchiveSourceBinding::SnapshotStats().wrappedReads == before.wrappedReads + 2);
    CHECK(expired->GetMipmapCount() == first->GetMipmapCount());
    CHECK(ArchiveSourceBinding::SnapshotStats().weakInitializedHandoffs == owned.weakInitializedHandoffs);
    cached.DoDestruct(); server.server->cache.FlushBank(nullptr);
    const auto after = ArchiveSourceBinding::SnapshotStats();
    CHECK(after.weakCells == before.weakCells);
    CHECK(after.knownCppBytes == before.knownCppBytes);
    CHECK(after.weakWrappersPublished == before.weakWrappersPublished + 2);
    CHECK(after.weakInitializedHandoffs == before.weakInitializedHandoffs + 1);
    CHECK(after.weakPeakReservationCells == owned.weakPeakReservationCells);
    CHECK(after.weakPeakReservationBytes == owned.weakPeakReservationBytes);
#endif
}

TEST_CASE("Failed actual cached PAA Init clears only its bootstrap lease and keeps bytes", "[archive-source-weak-cache]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    REQUIRE(ArchiveSourceBinding::CacheHandoffEnabled()); REQUIRE(QFileAccess::MappingSupported());
    Archives files; ServerScope server; ArchiveSourceBinding::ModelReadScope purpose;
    const auto before = ArchiveSourceBinding::SnapshotStats();
    QIFStreamB cached; cached.open(files.first, "malformed.paa"); REQUIRE_FALSE(cached.fail());
    REQUIRE(cached.GetBuffer()->GetArchiveSourceBinding());
    server.server->cache.Store(cached, R"(weak-proof-fixture\malformed.paa)");
    auto failed = CreateSource(R"(weak-proof-fixture\malformed.paa)"); CHECK_FALSE(failed);
    const auto completed = ArchiveSourceBinding::SnapshotStats();
    CHECK(completed.weakWrappersPublished == before.weakWrappersPublished + 1);
    CHECK(completed.weakInitializedHandoffs == before.weakInitializedHandoffs);
    CHECK(completed.weakFailedInitCompletions == before.weakFailedInitCompletions + 1);
    CHECK(cached.GetBuffer()->CompleteArchiveSourceInit({}));
    CHECK(ArchiveSourceBinding::SnapshotStats().weakFailedInitCompletions == completed.weakFailedInitCompletions);
    CHECK_FALSE(cached.GetBuffer()->GetArchiveSourceBinding());
    CHECK(cached.GetBuffer()->GetSize() == 8); CHECK_FALSE(cached.GetBuffer()->GetError());
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings);
    CHECK(ArchiveSourceBinding::SnapshotStats().weakCells == before.weakCells + 1);
    cached.DoDestruct(); server.server->cache.FlushBank(nullptr);
    CHECK(ArchiveSourceBinding::SnapshotStats().knownCppBytes == before.knownCppBytes);
#endif
}

TEST_CASE("Cache handoff OFF preserves the original strong mapped wrapper", "[archive-source-weak-cache-off]")
{
#ifndef _WIN32
    SKIP("Strong archive identity is Windows-only.");
#else
    REQUIRE_FALSE(ArchiveSourceBinding::CacheHandoffEnabled());
    const auto off = ArchiveSourceBinding::SnapshotStats();
    CHECK(off.weakWrappersPublished == 0); CHECK(off.weakInitializedHandoffs == 0);
    CHECK(off.weakFailedInitCompletions == 0); CHECK(off.weakPeakReservationCells == 0);
    CHECK(off.weakPeakReservationBytes == 0);
    REQUIRE(ArchiveSourceBinding::SnapshotStats().enabled); REQUIRE(QFileAccess::MappingSupported());
    Archives files; ServerScope server; ArchiveSourceBinding::ModelReadScope purpose;
    const auto before = ArchiveSourceBinding::SnapshotStats();
    QIFStreamB cached; cached.open(files.first, "texture.paa"); REQUIRE_FALSE(cached.fail());
    REQUIRE(cached.GetBuffer()->GetArchiveSourceBinding());
    CHECK_FALSE(cached.GetBuffer()->CompleteArchiveSourceInit({}));
    server.server->cache.Store(cached, R"(weak-proof-fixture\texture.paa)");
    auto source = CreateSource(R"(weak-proof-fixture\texture.paa)"); REQUIRE(source);
    REQUIRE(source->ReleaseArchiveSourceBinding());
    REQUIRE(cached.GetBuffer()->GetArchiveSourceBinding());
    CHECK(ArchiveSourceBinding::SnapshotStats().liveBindings == before.liveBindings + 1);
    CHECK(ArchiveSourceBinding::SnapshotStats().weakCells == before.weakCells);
    cached.DoDestruct(); server.server->cache.FlushBank(nullptr);
    CHECK(ArchiveSourceBinding::SnapshotStats().knownCppBytes == before.knownCppBytes);
#endif
}
