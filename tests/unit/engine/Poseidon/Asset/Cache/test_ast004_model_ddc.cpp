#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Cache/DerivedAssetKey.hpp>
#include <Poseidon/Asset/Cache/DerivedBlobStore.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelBlob.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ModelDerivedCache.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include "../../Support/test_fixtures.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// AST-004 -- the P3D -> Model IR derived data cache, end to end.
//
// The property under test is NOT "the cache is fast". It is that a warm run and
// a cold run produce THE SAME MODEL, and that every way of making them differ is
// a miss rather than a wrong answer. So the assertions compare BYTES: a model is
// re-serialised after a cache hit and the blob is required to equal the one the
// cold derivation produced. Comparing a handful of fields would pass with any
// number of fields silently dropped by the serialiser.
//
// The invalidation cases each break exactly one input and require a MISS. If any
// of them stops failing when its input is dropped from the key, the test is not
// testing what it says -- see the ablation section of AST-004.

using namespace TestFixtures;
using namespace Poseidon;
namespace fs = std::filesystem;

namespace
{

// A private root per test, removed on the way out, so nothing here can read or
// write the developer's real %LOCALAPPDATA% cache, and two tests cannot see each
// other's entries.
struct ScopedCache
{
    fs::path root;

    explicit ScopedCache(const char* label, uint64_t budget = 64ull * 1024 * 1024)
    {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("ast004-" + std::string(label) + "-" +
                std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::remove_all(root, ec);
        ModelDerivedCache::Instance().ResetForTesting(root, budget);
    }
    ~ScopedCache()
    {
        // Disable BEFORE removing, so a later test in this process cannot find a
        // cache pointed at a directory that no longer exists.
        ModelDerivedCache::Instance().DisableForTesting();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// A working copy of a fixture, so a test may corrupt its bytes without touching
// the checked-in file.
struct ScopedCopy
{
    fs::path path;

    ScopedCopy(const char* fixture, const char* label)
    {
        static std::atomic<int> counter{0};
        const fs::path src = ResolveFixturePath(fixture);
        path               = fs::temp_directory_path() /
               ("ast004-" + std::string(label) + "-" + std::to_string(counter.fetch_add(1)) + ".p3d");
        std::error_code ec;
        fs::remove(path, ec);
        fs::copy_file(src, path, fs::copy_options::overwrite_existing, ec);
    }
    ~ScopedCopy()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
    bool valid() const { return fs::exists(path); }
};

std::vector<uint8_t> ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Every .pdc entry under a root, so a test can damage one without knowing the key.
std::vector<fs::path> EntryFiles(const fs::path& root)
{
    std::vector<fs::path> out;
    std::error_code       ec;
    if (!fs::exists(root, ec))
        return out;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
            break;
        if (it->is_regular_file(ec) && !ec && it->path().extension() == ".pdc")
            out.push_back(it->path());
    }
    return out;
}

// The two witnesses: an MLOD with several LODs, and an ODOL. Both are checked in,
// so these cases run on any machine -- the retail-corpus cases below do not.
constexpr const char* kMlod = "p3d/multi_lod_vehicle.p3d";
constexpr const char* kOdol = "p3d/animated_morph_odol.p3d";

} // namespace

// ---------------------------------------------------------------------------
// The serialisation itself
// ---------------------------------------------------------------------------

TEST_CASE("ModelBlob: serialise, deserialise, re-serialise is byte-identical", "[ast004]")
{
    for (const char* fixture : {kMlod, kOdol})
    {
        const fs::path path = ResolveFixturePath(fixture);
        if (!fs::exists(path))
            continue;
        std::string error;
        bool        opened = false;
        auto        model  = ModelCache::LoadLooseFile(path.string(), &error, &opened);
        REQUIRE(opened);
        REQUIRE(model);

        const std::vector<uint8_t> a = ModelBlob::Serialize(*model);
        REQUIRE(a.size() > 8);

        Poseidon::Model::Model back;
        REQUIRE(ModelBlob::Deserialize(a.data(), a.size(), back));

        const std::vector<uint8_t> b = ModelBlob::Serialize(back);
        // The whole point: a round trip is a FIXED POINT. A field the writer
        // emits and the reader drops shows up here as a shorter second blob.
        REQUIRE(a == b);

        // And a few of the things a byte comparison would also catch, named so a
        // failure says which half is wrong.
        REQUIRE(back.lodLevels.size() == model->lodLevels.size());
        REQUIRE(back.sourcePath == model->sourcePath);
        REQUIRE(back.sourceFormat == model->sourceFormat);
        for (size_t i = 0; i < back.lodLevels.size(); ++i)
        {
            REQUIRE(back.lodLevels[i].mesh.vertices.size() == model->lodLevels[i].mesh.vertices.size());
            REQUIRE(back.lodLevels[i].mesh.triangles.size() == model->lodLevels[i].mesh.triangles.size());
            REQUIRE(back.lodLevels[i].mesh.materials.size() == model->lodLevels[i].mesh.materials.size());
        }
    }
}

// The round-trip test above is a FIXED-POINT test, and a fixed point is exactly
// what a field dropped from BOTH the writer and the reader still is. That is a
// real hole and it is what this case closes: the serialised bytes of two known
// fixtures are pinned by digest, so removing, adding, reordering or rewidening
// any field fails here and names the format as the thing that moved.
//
// It is deliberately brittle for the BASE layout: an intentional base change
// must bump kFormatVersion before updating these digests. The optional source
// audit has its own tagged version and is tested separately. Pinning the base
// retains evidence that old DDC entries still mean the same geometry, rather
// than replacing the old SHA merely because an audit footer was appended.
//
// `sourcePath` is normalised away first: it is a real input to the IR, but it is
// the absolute path of the fixture on this machine, so pinning it would pin the
// checkout directory.
TEST_CASE("ModelBlob: the stable base form of two fixtures is pinned by digest", "[ast004]")
{
    struct Case
    {
        const char* fixture;
        const char* digest;
    };
    // Regenerate by running this case and reading the failure's expansion.
    const Case cases[] = {
        {kMlod, "bd19e9e0d812713eb8ef7ac76e5bea960f8900ae4649fd9c0238d0c155664a98"},
        {kOdol, "129cab0041e0a81fef2698ece68a8a1d3fa5b822a2835510ab64d7ad8cf61f26"},
    };

    for (const Case& c : cases)
    {
        const fs::path path = ResolveFixturePath(c.fixture);
        if (!fs::exists(path))
            continue;
        std::string error;
        bool        opened = false;
        auto        model  = ModelCache::LoadLooseFile(path.string(), &error, &opened);
        REQUIRE(model);
        model->sourcePath = "<fixture>";
        const std::vector<uint8_t> blob = ModelBlob::Serialize(*model);
        INFO("fixture " << c.fixture);
        REQUIRE(blob.size() > ModelBlob::kSourceAuditFooterBytes);
        const size_t baseBytes = blob.size() - ModelBlob::kSourceAuditFooterBytes;
        REQUIRE(Foundation::Sha256::Of(blob.data(), baseBytes) == std::string(c.digest));

        Poseidon::Model::Model current;
        REQUIRE(ModelBlob::Deserialize(blob.data(), blob.size(), current));
        REQUIRE(ModelBlob::Serialize(current) == blob); // Current audit must survive, too.
        Poseidon::Model::Model legacy;
        REQUIRE(ModelBlob::Deserialize(blob.data(), baseBytes, legacy));
        REQUIRE(legacy.sourceAudit.producerVersion == 0);
        REQUIRE(legacy.sourceAudit.observations == 0);
        REQUIRE(legacy.sourceAudit.geometryCoverage == Model::SourceGeometryCoverage::Unknown);
        const auto reencoded = ModelBlob::Serialize(legacy);
        REQUIRE(reencoded.size() == blob.size());
        REQUIRE(std::equal(blob.begin(), blob.begin() + baseBytes, reencoded.begin()));
    }
}

TEST_CASE("ModelBlob: only the exact legacy base boundary is accepted among short prefixes", "[ast004]")
{
    const fs::path path = ResolveFixturePath(kMlod);
    if (!fs::exists(path))
        return;
    std::string error;
    bool        opened = false;
    auto        model  = ModelCache::LoadLooseFile(path.string(), &error, &opened);
    REQUIRE(model);
    const std::vector<uint8_t> blob = ModelBlob::Serialize(*model);
    REQUIRE(blob.size() > ModelBlob::kSourceAuditFooterBytes);
    const size_t baseBytes = blob.size() - ModelBlob::kSourceAuditFooterBytes;

    // Exact base-end is a complete old v1 blob and intentionally remains
    // readable with Unknown audit. Every other prefix must refuse, including
    // partial audit tags/lengths/payloads. This still catches trusting a count
    // before its bytes are available, without treating old entries as corrupt.
    for (size_t n = 0; n < blob.size(); ++n)
    {
        Poseidon::Model::Model out;
        if (n == baseBytes)
        {
            REQUIRE(ModelBlob::Deserialize(blob.data(), n, out));
            REQUIRE(out.sourceAudit.producerVersion == 0);
            REQUIRE(out.sourceAudit.observations == 0);
            REQUIRE(out.sourceAudit.geometryCoverage == Model::SourceGeometryCoverage::Unknown);
            REQUIRE(out.lodLevels.size() == model->lodLevels.size());
            const auto reencoded = ModelBlob::Serialize(out);
            REQUIRE(reencoded.size() == blob.size());
            REQUIRE(std::equal(blob.begin(), blob.begin() + baseBytes, reencoded.begin()));
        }
        else REQUIRE_FALSE(ModelBlob::Deserialize(blob.data(), n, out));
    }
    Poseidon::Model::Model whole;
    REQUIRE(ModelBlob::Deserialize(blob.data(), blob.size(), whole));
}

TEST_CASE("ModelBlob: wrong magic and wrong format version are refused", "[ast004]")
{
    const fs::path path = ResolveFixturePath(kMlod);
    if (!fs::exists(path))
        return;
    std::string error;
    bool        opened = false;
    auto        model  = ModelCache::LoadLooseFile(path.string(), &error, &opened);
    REQUIRE(model);
    std::vector<uint8_t> blob = ModelBlob::Serialize(*model);

    {
        std::vector<uint8_t>   bad = blob;
        bad[0]                     = 'X';
        Poseidon::Model::Model out;
        REQUIRE_FALSE(ModelBlob::Deserialize(bad.data(), bad.size(), out));
    }
    {
        std::vector<uint8_t> bad = blob;
        bad[4]                   = static_cast<uint8_t>(bad[4] + 1); // format version
        Poseidon::Model::Model out;
        REQUIRE_FALSE(ModelBlob::Deserialize(bad.data(), bad.size(), out));
    }
    {
        // Trailing junk is as much a wrong reading as missing bytes.
        std::vector<uint8_t> bad = blob;
        bad.push_back(0);
        Poseidon::Model::Model out;
        REQUIRE_FALSE(ModelBlob::Deserialize(bad.data(), bad.size(), out));
    }
}

// ---------------------------------------------------------------------------
// The cache, through the real ModelCache seam
// ---------------------------------------------------------------------------

TEST_CASE("Model DDC: a cold run and a warm run produce byte-identical output", "[ast004]")
{
    ScopedCache cache("warmcold");
    ModelDerivedCache& ddc = ModelDerivedCache::Instance();
    REQUIRE(ddc.Enabled());

    const fs::path path = ResolveFixturePath(kMlod);
    if (!fs::exists(path))
        return;

    std::string error;
    bool        opened = false;

    auto cold = ModelCache::LoadLooseFile(path.string(), &error, &opened);
    REQUIRE(cold);
    const ModelDerivedCache::Stats afterCold = ddc.GetStats();
    REQUIRE(afterCold.misses == 1);
    REQUIRE(afterCold.hits == 0);
    REQUIRE(afterCold.stores == 1);

    auto warm = ModelCache::LoadLooseFile(path.string(), &error, &opened);
    REQUIRE(warm);
    const ModelDerivedCache::Stats afterWarm = ddc.GetStats();
    REQUIRE(afterWarm.hits == 1);
    REQUIRE(afterWarm.stores == 1); // a hit must not rewrite the entry

    // Bytes, not "looks the same".
    REQUIRE(ModelBlob::Serialize(*cold) == ModelBlob::Serialize(*warm));
}

TEST_CASE("Model DDC: changing the input content changes the key and misses", "[ast004]")
{
    ScopedCache cache("content");
    ModelDerivedCache& ddc = ModelDerivedCache::Instance();

    ScopedCopy copy(kMlod, "content");
    if (!copy.valid())
        return;

    std::string error;
    bool        opened = false;
    auto        first  = ModelCache::LoadLooseFile(copy.path.string(), &error, &opened);
    REQUIRE(first);
    REQUIRE(ddc.GetStats().stores == 1);

    // Same path, one byte different. Nothing but the content has moved, so a
    // path- or mtime-keyed cache would serve the previous model here.
    std::vector<uint8_t> bytes = ReadAll(copy.path);
    REQUIRE(bytes.size() > 64);
    // Late in the file, inside the payload rather than the header, so the file
    // still parses and the difference is a real difference in the IR.
    const size_t at = bytes.size() - 8;
    bytes[at]       = static_cast<uint8_t>(bytes[at] ^ 0xFF);
    WriteAll(copy.path, bytes);

    ddc.ResetStats();
    auto second = ModelCache::LoadLooseFile(copy.path.string(), &error, &opened);
    REQUIRE(ddc.GetStats().hits == 0);
    REQUIRE(ddc.GetStats().misses == 1);
}

TEST_CASE("Model DDC: FileServer bytes, not a different mounted member, determine the key", "[ast004]")
{
    const fs::path bankModel = ResolveFixturePath(kMlod);
    // complex_vehicle_mlod.p3d is a byte-for-byte copy of kMlod. This smaller
    // checked-in MLOD has distinct geometry and is parsed by the same loader.
    const fs::path selectedModel = ResolveFixturePath("p3d/sky_plane.p3d");
    if (!fs::exists(bankModel) || !fs::exists(selectedModel)) return;
    const auto bankBytes = ReadAll(bankModel);
    const auto selectedBytes = ReadAll(selectedModel);
    REQUIRE(!bankBytes.empty());
    REQUIRE(!selectedBytes.empty());
    REQUIRE(bankBytes != selectedBytes);

    ScopedCache cache("file-server-source");
    // AutoBank deliberately names A while FileServer's actual selected stream
    // first contains B. This can happen with an override or a changed mount.
    // HashBankMember used to key the B derivation by A's independently-read hash.
    static std::atomic<int> serial{0};
    const fs::path archive = fs::temp_directory_path() /
        ("ast004-source-" + std::to_string(serial.fetch_add(1)) + ".pbo");
    struct MountedMember
    {
        fs::path archive;
        int index = -1;
        bool oldUseBanks = GUseFileBanks;
        MountedMember(fs::path path, const std::vector<uint8_t>& bytes) : archive(std::move(path))
        {
            std::ofstream out(archive, std::ios::binary);
            REQUIRE(out.good());
            const char member[] = "model.p3d";
            out.write(member, sizeof(member));
            for (int value : {0, 0, 0, 0, int(bytes.size())})
                out.write(reinterpret_cast<const char*>(&value), sizeof(value));
            const char terminator = 0;
            out.write(&terminator, 1);
            for (int value : {0, 0, 0, 0, 0})
                out.write(reinterpret_cast<const char*>(&value), sizeof(value));
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();
            REQUIRE(out.good());
            GUseFileBanks = true;
            index = GFileBanks.Add();
            auto basename = archive;basename.replace_extension();
            REQUIRE(GFileBanks[index].open(RString(basename.string().c_str())));
            GFileBanks[index].SetPrefix(RString(R"(ast004_source_probe\)"));
            GFileBanks[index].Lock();
        }
        ~MountedMember()
        {
            if (index >= 0) {
                GFileBanks[index].Unlock();GFileBanks[index].close();GFileBanks.Delete(index);
            }
            GUseFileBanks = oldUseBanks;
            std::error_code ec;fs::remove(archive, ec);
        }
    } mounted(archive, bankBytes);
    constexpr const char* virtualPath = R"(ast004_source_probe\model.p3d)";
    REQUIRE(ModelDerivedCache::HashBankMember(virtualPath) ==
            ModelDerivedCache::HashBytes(bankBytes.data(), bankBytes.size()));
    class RedirectServer final : public FileServer
    {
    public:
        fs::path selected;
        void Request(const char*, float, int, int) override {}
        void CancelRequest(const char*, int, int) override {}
        void Open(QIFStream& stream, const char*) override { stream.open(selected.string().c_str()); }
        void Start() override {} void Stop() override {}
        void FlushBank(QFBank*) override {}
    };
    const Ref<FileServer> previous = GFileServer;
    GFileServer = new RedirectServer;
    auto* redirect = static_cast<RedirectServer*>(GFileServer.GetRef());
    struct RestoreServer
    {
        Ref<FileServer> previous;
        ~RestoreServer() { GFileServer = previous; }
    } restore{previous};

    redirect->selected = selectedModel;
    ModelCache firstCache;
    std::shared_ptr<const ModelCompressedSourceBirth> parserBirth;
    auto first = firstCache.load(virtualPath, &parserBirth);
    REQUIRE(first);
    REQUIRE_FALSE(parserBirth); // a same-named bank is not the selected parser buffer's birth
    REQUIRE(ModelDerivedCache::Instance().GetStats().misses == 1);
    redirect->selected = bankModel;
    ModelCache secondCache;
    auto second = secondCache.load(virtualPath, &parserBirth);
    REQUIRE(second);
    REQUIRE_FALSE(parserBirth);
    REQUIRE(secondCache.load(virtualPath, &parserBirth) == second);
    REQUIRE_FALSE(parserBirth); // in-memory hits never mint source authority
    REQUIRE(ModelDerivedCache::Instance().GetStats().hits == 0);
    REQUIRE(ModelDerivedCache::Instance().GetStats().misses == 2);
    REQUIRE(ModelBlob::Serialize(*first) != ModelBlob::Serialize(*second));
}

TEST_CASE("Model DDC: the same bytes under two names are two entries", "[ast004]")
{
    ScopedCache cache("twonames");
    ModelDerivedCache& ddc = ModelDerivedCache::Instance();

    ScopedCopy a(kMlod, "namea");
    ScopedCopy b(kMlod, "nameb");
    if (!a.valid() || !b.valid())
        return;
    REQUIRE(ReadAll(a.path) == ReadAll(b.path)); // identical content, different names

    std::string error;
    bool        opened = false;
    auto        first  = ModelCache::LoadLooseFile(a.path.string(), &error, &opened);
    REQUIRE(first);
    ddc.ResetStats();
    auto second = ModelCache::LoadLooseFile(b.path.string(), &error, &opened);
    REQUIRE(second);

    // The path is an INPUT: the loaders write it into Model::sourcePath. If it
    // were left out of the key this would be a hit, and `second` would carry
    // `a`'s sourcePath -- a wrong answer, not a stale one.
    REQUIRE(ddc.GetStats().hits == 0);
    REQUIRE(ddc.GetStats().misses == 1);
    REQUIRE(first->sourcePath != second->sourcePath);
    REQUIRE(second->sourcePath == b.path.string());
}

TEST_CASE("Model DDC: changing an option changes the key and misses", "[ast004]")
{
    ScopedCache cache("option");
    Asset::Cache::DerivedBlobStore store(cache.root);

    const std::string sha = Foundation::Sha256::Of(std::string("some model bytes"));

    Asset::Cache::DerivedAssetKey base = ModelDerivedCache::MakeKey("ca\\air\\x.p3d", sha);

    // FIRST: pin the option SET that MakeKey actually builds.
    //
    // This assertion exists because of an ablation that did not fail. The three
    // "a bumped option is a different key" checks below were originally the whole
    // test, and deleting `AddOption("blobFormat", ...)` from MakeKey left all of
    // them passing -- of course it did, because they compare two hand-built keys
    // to each other and never ask what MakeKey put in. They proved DerivedAssetKey
    // honours options, which AST-020 already proves, and proved nothing about this
    // producer. An option silently missing from the real key is precisely the
    // wrong-data hit the design is for, so the real key is compared against a
    // written-out expectation of it.
    //
    // "1" is `kModelProducerVersion`, which is file-local to the cache. Bumping it
    // must fail here: that bump means every cached model is stale, and it should
    // not be possible to make it without this test saying so.
    {
        Asset::Cache::DerivedAssetKey expected;
        expected.AddInput("model", Asset::VirtualPath::Parse("ca\\air\\x.p3d"), sha);
        expected.AddOption("modelProducer", "1");
        expected.AddOption("blobFormat", std::to_string(ModelBlob::kFormatVersion));
        REQUIRE(base.Compute() == expected.Compute());
        REQUIRE(base.compilerVersion() == Asset::Cache::DerivedAssetKey::kCompilerVersion);
        REQUIRE(base.inputs().size() == 1);
        REQUIRE(base.inputs()[0].role == "model");
        REQUIRE(base.inputs()[0].sha256 == sha);
        REQUIRE(base.inputs()[0].path.canonical() == Asset::VirtualPath::Parse("ca\\air\\x.p3d").canonical());
    }

    // The same inputs with one option moved. Built by hand rather than by moving
    // a constant, so the test states the property ("an option participates")
    // instead of restating the constant's current value.
    Asset::Cache::DerivedAssetKey bumpedBlobFormat;
    bumpedBlobFormat.AddInput("model", Asset::VirtualPath::Parse("ca\\air\\x.p3d"), sha);
    bumpedBlobFormat.AddOption("modelProducer", "1");
    bumpedBlobFormat.AddOption("blobFormat", std::to_string(ModelBlob::kFormatVersion + 1));

    Asset::Cache::DerivedAssetKey bumpedProducer;
    bumpedProducer.AddInput("model", Asset::VirtualPath::Parse("ca\\air\\x.p3d"), sha);
    bumpedProducer.AddOption("modelProducer", "2");
    bumpedProducer.AddOption("blobFormat", std::to_string(ModelBlob::kFormatVersion));

    REQUIRE(base.Compute() != bumpedBlobFormat.Compute());
    REQUIRE(base.Compute() != bumpedProducer.Compute());
    REQUIRE(bumpedBlobFormat.Compute() != bumpedProducer.Compute());

    const std::vector<uint8_t> payload{1, 2, 3, 4};
    REQUIRE(store.Put(base, payload));

    std::vector<uint8_t> out;
    REQUIRE(store.Get(base, out));
    REQUIRE(out == payload);
    REQUIRE_FALSE(store.Get(bumpedBlobFormat, out));
    REQUIRE_FALSE(store.Get(bumpedProducer, out));
}

TEST_CASE("Model DDC: a corrupted cache entry is a miss, not a wrong answer", "[ast004]")
{
    ScopedCache cache("corrupt");
    ModelDerivedCache& ddc = ModelDerivedCache::Instance();

    const fs::path path = ResolveFixturePath(kMlod);
    if (!fs::exists(path))
        return;

    std::string error;
    bool        opened = false;
    auto        cold   = ModelCache::LoadLooseFile(path.string(), &error, &opened);
    REQUIRE(cold);
    const std::vector<uint8_t> expected = ModelBlob::Serialize(*cold);

    std::vector<fs::path> entries = EntryFiles(cache.root);
    REQUIRE(entries.size() == 1);

    // Damage the payload IN PLACE, at the same length. The magic and the length
    // still check out; only the payload digest can see this.
    std::vector<uint8_t> raw = ReadAll(entries[0]);
    REQUIRE(raw.size() > 16);
    raw[raw.size() - 1] = static_cast<uint8_t>(raw[raw.size() - 1] ^ 0xFF);
    WriteAll(entries[0], raw);

    ddc.ResetStats();
    auto after = ModelCache::LoadLooseFile(path.string(), &error, &opened);
    REQUIRE(after);
    // Not a hit, and not the damaged data: the load re-derived and got the same
    // model it would have got with no cache at all.
    REQUIRE(ddc.GetStats().hits == 0);
    REQUIRE(ModelBlob::Serialize(*after) == expected);
    // And the poisoned entry was replaced rather than left to be re-read forever.
    REQUIRE(ddc.GetStats().stores == 1);
}

TEST_CASE("Model DDC: it is off unless it is switched on", "[ast004]")
{
    // The default state of the singleton in this build is disabled, and a
    // disabled cache must not create its root or count anything.
    ModelDerivedCache::Instance().DisableForTesting();
    REQUIRE_FALSE(ModelDerivedCache::Instance().Enabled());

    const fs::path path = ResolveFixturePath(kMlod);
    if (!fs::exists(path))
        return;
    std::string error;
    bool        opened = false;
    auto        model  = ModelCache::LoadLooseFile(path.string(), &error, &opened);
    REQUIRE(model);
    const ModelDerivedCache::Stats s = ModelDerivedCache::Instance().GetStats();
    REQUIRE(s.hits == 0);
    REQUIRE(s.misses == 0);
    REQUIRE(s.stores == 0);
}

TEST_CASE("Model DDC: Prune evicts to the budget and keeps the newest", "[ast004]")
{
    // A budget below one entry, so the very first Prune has work to do. The store
    // owns the eviction policy (AST-002 tests it); what is checked here is that
    // the model cache's Prune reaches it, counts it, and survives a store that is
    // not there.
    ScopedCache cache("prune", /*budget=*/1);
    ModelDerivedCache& ddc = ModelDerivedCache::Instance();

    const fs::path path = ResolveFixturePath(kMlod);
    if (!fs::exists(path))
        return;
    std::string error;
    bool        opened = false;
    REQUIRE(ModelCache::LoadLooseFile(path.string(), &error, &opened));
    REQUIRE(EntryFiles(cache.root).size() == 1);

    REQUIRE(ddc.Prune() == 1);
    REQUIRE(ddc.GetStats().evictions == 1);
    REQUIRE(EntryFiles(cache.root).empty());

    ddc.DisableForTesting();
    REQUIRE(ddc.Prune() == 0); // a disabled cache prunes nothing and does not throw
}

// ---------------------------------------------------------------------------
// The win, measured. Hidden: needs the retail corpus.
//   PoseidonCoreTests.exe "[ast004bench]"  with AST004_BENCH_DIR pointing at a
//   directory of loose .p3d files (spilled out of a PBO by the caller).
// ---------------------------------------------------------------------------
TEST_CASE("Model DDC cold vs warm on a real model corpus", "[.][ast004bench]")
{
    const char* dir = std::getenv("AST004_BENCH_DIR");
    if (!dir || !*dir)
    {
        WARN("AST004_BENCH_DIR not set; skipping");
        return;
    }
    std::vector<fs::path> models;
    std::error_code       ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
            break;
        if (it->is_regular_file(ec) && !ec && it->path().extension() == ".p3d")
            models.push_back(it->path());
    }
    REQUIRE_FALSE(models.empty());

    ScopedCache        cache("bench", 8ull * 1024 * 1024 * 1024);
    ModelDerivedCache& ddc = ModelDerivedCache::Instance();

    using Clock = std::chrono::steady_clock;
    const auto Ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    uint64_t sourceBytes = 0;
    for (const fs::path& p : models)
        sourceBytes += static_cast<uint64_t>(fs::file_size(p, ec));

    // Three passes: cold (derive + store), then two warm ones, so a single warm
    // number cannot be a fluke of the page cache warming during it.
    double coldMs = 0, warm1Ms = 0, warm2Ms = 0;
    for (int pass = 0; pass < 3; ++pass)
    {
        ddc.ResetStats();
        const auto t0 = Clock::now();
        for (const fs::path& p : models)
        {
            std::string error;
            bool        opened = false;
            auto        m      = ModelCache::LoadLooseFile(p.string(), &error, &opened);
            (void)m;
        }
        const double ms = Ms(Clock::now() - t0);
        const auto   s  = ddc.GetStats();
        std::printf("[ast004bench] pass %d  %.1f ms  hits=%llu misses=%llu stores=%llu "
                    "loaded=%llu B stored=%llu B\n",
                    pass, ms, (unsigned long long)s.hits, (unsigned long long)s.misses,
                    (unsigned long long)s.stores, (unsigned long long)s.bytesLoaded,
                    (unsigned long long)s.bytesStored);
        if (pass == 0)
            coldMs = ms;
        else if (pass == 1)
            warm1Ms = ms;
        else
            warm2Ms = ms;
    }

    uint64_t cacheBytes = 0;
    for (const fs::path& p : EntryFiles(cache.root))
        cacheBytes += static_cast<uint64_t>(fs::file_size(p, ec));

    std::printf("[ast004bench] models=%zu source=%.2f MiB cache=%.2f MiB (%.2fx)  "
                "cold=%.1f ms warm=%.1f/%.1f ms  speedup=%.2fx\n",
                models.size(), sourceBytes / 1048576.0, cacheBytes / 1048576.0,
                sourceBytes ? (double)cacheBytes / (double)sourceBytes : 0.0, coldMs, warm1Ms, warm2Ms,
                warm1Ms > 0 ? coldMs / warm1Ms : 0.0);
    std::fflush(stdout);
    SUCCEED();
}
