// test_derived_blob_store.cpp - AST-002: the on-disk half of the Derived Data Cache.
//
// DerivedAssetKey (AST-020) is tested next door and answers "what is this artefact's
// identity". These cases test the thing that had no test because it did not exist: a
// store that must hand back exactly the bytes that were put in, or nothing.
//
// Each case is a way a derived-data cache goes wrong in the field. The three that are
// not obvious, and that the existing sky-bake cache in the wgpu renderer can hit:
//
//   * a producer whose code changed keeps serving its old output (version invalidation);
//   * a file damaged in place at the same length reads back as a hit (payload digest);
//   * two processes writing one key share a temp file and publish the interleaving
//     (unique temp names).

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Cache/DerivedBlobStore.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Poseidon::Asset::VirtualPath;
using Poseidon::Asset::Cache::DerivedAssetKey;
using Poseidon::Asset::Cache::DerivedBlobStore;

namespace
{

// A unique scratch directory per test, removed on scope exit. Nothing here touches the
// user's real cache root -- a test that pruned %LOCALAPPDATA% would be a memorable bug.
class ScratchRoot
{
  public:
    ScratchRoot()
    {
        static std::atomic<uint64_t> n{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_            = std::filesystem::temp_directory_path() /
                ("pdc-test-" + std::to_string(stamp) + "-" + std::to_string(n.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~ScratchRoot()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& get() const { return path_; }

  private:
    std::filesystem::path path_;
};

DerivedAssetKey sample(int compilerVersion = DerivedAssetKey::kCompilerVersion)
{
    DerivedAssetKey key;
    key.SetCompilerVersion(compilerVersion);
    key.AddInput("model", VirtualPath::Parse("ca/air/ataka.p3d"), "aaaa");
    key.AddInput("texture", VirtualPath::Parse("ca/air/data/ataka_co.paa"), "cccc");
    return key;
}

std::vector<uint8_t> blob(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

// The single file the store wrote for this key. Tests that damage an entry need to find
// it without duplicating the sharding rule.
std::filesystem::path onlyEntry(const std::filesystem::path& root)
{
    std::filesystem::path found;
    size_t                n = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
    {
        if (!e.is_regular_file())
            continue;
        ++n;
        found = e.path();
    }
    REQUIRE(n == 1);
    return found;
}

} // namespace

TEST_CASE("Blob store: a miss on an empty store is a miss, not a failure", "[asset][cache][ast-002]")
{
    ScratchRoot       root;
    DerivedBlobStore  store(root.get());
    std::vector<uint8_t> out{1, 2, 3};

    REQUIRE_FALSE(store.Get(sample(), out));
    REQUIRE(store.GetStats().misses == 1);
    REQUIRE(store.GetStats().hits == 0);
}

TEST_CASE("Blob store: what went in comes back out, byte for byte", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());

    // Includes an embedded NUL and a high byte: a store that round-trips through a text
    // stream or a C string passes the ASCII case and loses these.
    std::vector<uint8_t> payload = {0x00, 0x41, 0xFF, 0x00, 0x7F, 0x80};
    REQUIRE(store.Put(sample(), payload));

    std::vector<uint8_t> out;
    REQUIRE(store.Get(sample(), out));
    REQUIRE(out == payload);
    REQUIRE(store.GetStats().hits == 1);
    REQUIRE(store.GetStats().writes == 1);
}

TEST_CASE("Blob store: an empty payload is a hit, not a miss", "[asset][cache][ast-002]")
{
    // "This model derives to nothing" is a real answer and must be cacheable, or the
    // producer pays the full conversion every launch to rediscover emptiness.
    ScratchRoot      root;
    DerivedBlobStore store(root.get());

    REQUIRE(store.Put(sample(), std::vector<uint8_t>{}));
    std::vector<uint8_t> out{9, 9, 9};
    REQUIRE(store.Get(sample(), out));
    REQUIRE(out.empty());
}

TEST_CASE("Blob store: a hit survives a new store object over the same root", "[asset][cache][ast-002]")
{
    // The point of an on-disk cache. A fresh DerivedBlobStore is what the next process
    // launch looks like from the store's side.
    ScratchRoot root;
    {
        DerivedBlobStore writer(root.get());
        REQUIRE(writer.Put(sample(), blob("converted-geometry")));
    }
    DerivedBlobStore     reader(root.get());
    std::vector<uint8_t> out;
    REQUIRE(reader.Get(sample(), out));
    REQUIRE(out == blob("converted-geometry"));
}

TEST_CASE("Blob store: a different key does not read another key's entry", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    REQUIRE(store.Put(sample(), blob("ataka")));

    DerivedAssetKey other;
    other.AddInput("model", VirtualPath::Parse("ca/air/ah1z.p3d"), "dddd");
    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(other, out));
}

TEST_CASE("Blob store: changed source content invalidates the entry", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    REQUIRE(store.Put(sample(), blob("derived-from-aaaa")));

    // Same path, different bytes -- an addon rebuilt in place. A cache keyed on the path
    // would hit here and serve output derived from content that no longer exists.
    DerivedAssetKey rebuilt;
    rebuilt.AddInput("model", VirtualPath::Parse("ca/air/ataka.p3d"), "aaab");
    rebuilt.AddInput("texture", VirtualPath::Parse("ca/air/data/ataka_co.paa"), "cccc");

    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(rebuilt, out));
}

// ---------------------------------------------------------------------------------
// Version invalidation -- the case that makes a converter bug fix actually reach users.
// ---------------------------------------------------------------------------------

TEST_CASE("Blob store: bumping the compiler version invalidates every entry it wrote",
          "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());

    const DerivedAssetKey v1 = sample(7);
    REQUIRE(store.Put(v1, blob("output-of-the-buggy-converter")));

    std::vector<uint8_t> out;
    REQUIRE(store.Get(v1, out)); // the machine that already cached it

    // The converter is fixed and its version bumped. Every entry the old one wrote must
    // become unreachable, or the fix ships and nothing changes on any machine that ran
    // the broken build -- which is the entire failure mode this check exists for.
    const DerivedAssetKey v2 = sample(8);
    REQUIRE_FALSE(store.Get(v2, out));

    // ...and the two versions coexist rather than overwrite, so a rollback to v7 reads
    // v7 output and not v8's.
    REQUIRE(store.Put(v2, blob("output-of-the-fixed-converter")));
    std::vector<uint8_t> a, b;
    REQUIRE(store.Get(v1, a));
    REQUIRE(store.Get(v2, b));
    REQUIRE(a == blob("output-of-the-buggy-converter"));
    REQUIRE(b == blob("output-of-the-fixed-converter"));
    REQUIRE(a != b);
}

TEST_CASE("Blob store: the version is in the file name, not only in the hash", "[asset][cache][ast-002]")
{
    // Belt and braces: even if two versions somehow computed the same digest, they must
    // not land on the same path and overwrite one another.
    REQUIRE(DerivedBlobStore::EntryId(sample(7)) != DerivedBlobStore::EntryId(sample(8)));
    REQUIRE(DerivedBlobStore::EntryId(sample(7)).find("v7-") == 0);
}

// ---------------------------------------------------------------------------------
// Damaged entries -- every one must be a miss, never a crash and never wrong bytes.
// ---------------------------------------------------------------------------------

TEST_CASE("Blob store: a truncated entry is rejected, not trusted", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    REQUIRE(store.Put(sample(), blob("a-long-enough-payload-to-cut-in-half")));

    const auto path = onlyEntry(root.get());
    const auto full = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, full / 2); // a crash mid-write, or a full disk

    std::vector<uint8_t> out{7, 7, 7};
    REQUIRE_FALSE(store.Get(sample(), out));
    REQUIRE(out.empty()); // and no partial fill the caller might use
    REQUIRE(store.GetStats().rejectedCorrupt == 1);

    // The poisoned file is gone, so the next run pays a plain miss rather than a read
    // and a hash before reaching the same conclusion every time forever.
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Blob store: a zero-length entry is rejected", "[asset][cache][ast-002]")
{
    // The classic aftermath of a power loss: the directory entry exists, the data does
    // not. It is shorter than the header, so it must fail the very first check.
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    REQUIRE(store.Put(sample(), blob("payload")));
    const auto path = onlyEntry(root.get());
    std::filesystem::resize_file(path, 0);

    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(sample(), out));
}

TEST_CASE("Blob store: a payload corrupted in place at the SAME length is rejected",
          "[asset][cache][ast-002]")
{
    // The case a magic-plus-length check cannot see, and the reason the payload digest
    // is in the header. Bit rot, a bad cable, a partially-overwritten sector: the file
    // is exactly the right size and every structural field still parses.
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    const auto       payload = blob("converted-geometry-vertices-and-indices");
    REQUIRE(store.Put(sample(), payload));

    const auto path   = onlyEntry(root.get());
    const auto before = std::filesystem::file_size(path);
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(f);
        f.seekp(static_cast<std::streamoff>(before) - 3);
        const char flipped = '\x01';
        f.write(&flipped, 1);
    }
    REQUIRE(std::filesystem::file_size(path) == before); // same length, damaged content

    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(sample(), out));
    REQUIRE(store.GetStats().rejectedCorrupt == 1);
}

TEST_CASE("Blob store: an entry whose magic is wrong is rejected", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    REQUIRE(store.Put(sample(), blob("payload")));
    const auto path = onlyEntry(root.get());
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0);
        f.write("XXXX", 4);
    }
    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(sample(), out));
}

TEST_CASE("Blob store: an entry written by another container version is rejected",
          "[asset][cache][ast-002]")
{
    // Independent of the producer's compiler version: this is the store's own on-disk
    // layout changing. An old file whose header parses under a new layout would be read
    // with the wrong field offsets, so the container version is checked before anything
    // else in the header is believed.
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    REQUIRE(store.Put(sample(), blob("payload")));
    const auto path = onlyEntry(root.get());
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(4);
        const uint32_t other = DerivedBlobStore::kContainerVersion + 1;
        f.write(reinterpret_cast<const char*>(&other), 4);
    }
    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(sample(), out));
    REQUIRE(store.GetStats().rejectedCorrupt == 1);
}

TEST_CASE("Blob store: an entry carrying another key's id is rejected", "[asset][cache][ast-002]")
{
    // What a hash collision looks like from the reader's side, and also what a file
    // hand-copied to the wrong name looks like. The entry echoes its own key, so a path
    // that says one thing and content that says another is a miss.
    ScratchRoot      root;
    DerivedBlobStore store(root.get());

    DerivedAssetKey other;
    other.AddInput("model", VirtualPath::Parse("ca/air/ah1z.p3d"), "dddd");
    REQUIRE(store.Put(other, blob("the-wrong-model")));
    const auto foreign = onlyEntry(root.get());

    // Move the other key's entry to where our key's entry would live.
    DerivedBlobStore probe(root.get());
    REQUIRE(probe.Put(sample(), blob("ours")));
    std::filesystem::path ours;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root.get()))
        if (e.is_regular_file() && e.path().filename() == DerivedBlobStore::EntryId(sample()))
            ours = e.path();
    REQUIRE(!ours.empty());
    std::filesystem::copy_file(foreign, ours, std::filesystem::copy_options::overwrite_existing);

    std::vector<uint8_t> out;
    REQUIRE_FALSE(probe.Get(sample(), out));
    REQUIRE(probe.GetStats().rejectedCorrupt == 1);
}

TEST_CASE("Blob store: a header claiming an absurd payload length does not allocate it",
          "[asset][cache][ast-002]")
{
    // A damaged length field must fail the size check, not be used to size a vector.
    // The check is written in 64-bit so idLen + payloadLen cannot wrap past it.
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    REQUIRE(store.Put(sample(), blob("payload")));
    const auto path = onlyEntry(root.get());
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(12); // payloadLen, u64 LE
        const uint64_t huge = 0xFFFFFFFFFFFFFFFFull;
        f.write(reinterpret_cast<const char*>(&huge), 8);
    }
    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(sample(), out));
}

// ---------------------------------------------------------------------------------
// Concurrency and bounds.
// ---------------------------------------------------------------------------------

TEST_CASE("Blob store: concurrent writers of one key never publish a torn entry",
          "[asset][cache][ast-002]")
{
    // The sky-bake cache derives its temp name from the key alone, so every writer of a
    // key shares one temp file. Here each writer gets its own; whichever rename lands
    // last wins, and because a key names one content the winner is right either way.
    ScratchRoot root;
    const auto  payload = blob(std::string(64 * 1024, 'Z')); // big enough to interleave

    std::vector<std::thread> threads;
    std::atomic<int>         published{0};
    for (int i = 0; i < 8; ++i)
    {
        threads.emplace_back([&] {
            DerivedBlobStore writer(root.get());
            for (int n = 0; n < 8; ++n)
                if (writer.Put(sample(), payload))
                    ++published;
        });
    }
    for (auto& t : threads)
        t.join();

    REQUIRE(published > 0);
    DerivedBlobStore     reader(root.get());
    std::vector<uint8_t> out;
    REQUIRE(reader.Get(sample(), out));
    REQUIRE(out == payload);
    REQUIRE(reader.GetStats().rejectedCorrupt == 0);
}

TEST_CASE("Blob store: a reader racing writers sees a whole entry or none", "[asset][cache][ast-002]")
{
    ScratchRoot         root;
    const auto          payload = blob(std::string(32 * 1024, 'Q'));
    std::atomic<bool>   stop{false};
    std::atomic<int>    corrupt{0};
    std::atomic<int>    hits{0};

    std::thread writer([&] {
        DerivedBlobStore w(root.get());
        while (!stop.load())
            w.Put(sample(), payload);
    });
    {
        DerivedBlobStore r(root.get());
        for (int i = 0; i < 400; ++i)
        {
            std::vector<uint8_t> out;
            if (r.Get(sample(), out))
            {
                ++hits;
                if (out != payload)
                    ++corrupt; // a torn read would land here
            }
        }
        stop = true;
        writer.join();
        REQUIRE(r.GetStats().rejectedCorrupt == 0);
    }
    REQUIRE(corrupt == 0);
}

TEST_CASE("Blob store: no temp files are left behind after a clean write", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    for (int i = 0; i < 5; ++i)
    {
        DerivedAssetKey k;
        k.AddInput("model", VirtualPath::Parse("ca/m" + std::to_string(i) + ".p3d"), "hash");
        REQUIRE(store.Put(k, blob("x")));
    }
    for (const auto& e : std::filesystem::recursive_directory_iterator(root.get()))
        if (e.is_regular_file())
            REQUIRE(e.path().filename().string().rfind(".tmp-", 0) != 0);
}

TEST_CASE("Blob store: Prune evicts down to the budget and stops", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get(), 512ull * 1024 * 1024);

    const auto payload = blob(std::string(4096, 'p'));
    for (int i = 0; i < 20; ++i)
    {
        DerivedAssetKey k;
        k.AddInput("model", VirtualPath::Parse("ca/m" + std::to_string(i) + ".p3d"), "hash");
        REQUIRE(store.Put(k, payload));
        // mtime is the eviction order and its filesystem resolution is coarse; without a
        // gap the sort falls back to the path tie-break and "oldest first" means nothing.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(store.EntryCount() == 20);
    const uint64_t total = store.TotalBytes();
    REQUIRE(total > 20 * 4096); // payload plus headers

    REQUIRE(store.Prune() == 0); // under budget: nothing to do

    store.SetBudgetBytes(total / 2);
    const size_t removed = store.Prune();
    REQUIRE(removed > 0);
    REQUIRE(store.TotalBytes() <= total / 2);
    REQUIRE(store.EntryCount() == 20 - removed);
    REQUIRE(store.GetStats().evictions == removed);

    // An evicted entry is a miss, not a corrupt read -- eviction must be indistinguishable
    // from never having cached it.
    DerivedBlobStore after(root.get());
    REQUIRE(after.GetStats().rejectedCorrupt == 0);
}

TEST_CASE("Blob store: Prune keeps the newest entries", "[asset][cache][ast-002]")
{
    ScratchRoot      root;
    DerivedBlobStore store(root.get());
    const auto       payload = blob(std::string(2048, 'p'));

    std::vector<DerivedAssetKey> keys;
    for (int i = 0; i < 8; ++i)
    {
        DerivedAssetKey k;
        k.AddInput("model", VirtualPath::Parse("ca/n" + std::to_string(i) + ".p3d"), "hash");
        REQUIRE(store.Put(k, payload));
        keys.push_back(k);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    store.SetBudgetBytes(store.TotalBytes() / 4);
    REQUIRE(store.Prune() > 0);

    std::vector<uint8_t> out;
    REQUIRE(store.Get(keys.back(), out));    // newest survived
    REQUIRE_FALSE(store.Get(keys.front(), out)); // oldest went first
}

TEST_CASE("Blob store: a root that cannot be created fails the write instead of throwing",
          "[asset][cache][ast-002]")
{
    // Read-only media, a full disk, a path the user cannot write. A cache is an
    // optimisation; failing to have one must never be fatal.
    ScratchRoot root;
    const auto  blocked = root.get() / "not-a-directory";
    {
        std::ofstream f(blocked); // a FILE where the store wants a directory tree
        f << "x";
    }
    DerivedBlobStore store(blocked);
    REQUIRE_FALSE(store.Put(sample(), blob("payload")));
    REQUIRE(store.GetStats().writeFailures == 1);

    std::vector<uint8_t> out;
    REQUIRE_FALSE(store.Get(sample(), out));
}
