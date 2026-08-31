#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include "../Support/test_fixtures.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace TestFixtures;
using namespace Poseidon;
namespace fs = std::filesystem;

// AST-003 -- QFBank::GetContentHash.
//
// The property under test is the one DerivedAssetKey depends on and the one a
// path- or mtime-derived "hash" would fail: the answer is a function of the
// member's BYTES and of nothing else. So the archives built here deliberately
// store the same bytes twice under different names, and different bytes under
// names chosen so that a path-derived hash would still look plausible.

namespace
{

struct PboMember
{
    std::string name;
    std::vector<uint8_t> data;
    int32_t time = 0;
};

void PutInt(std::ofstream& out, int32_t v)
{
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void PutEntry(std::ofstream& out, const std::string& name, int32_t magic, int32_t uncompressed, int32_t offset,
              int32_t time, int32_t length)
{
    out.write(name.c_str(), static_cast<std::streamsize>(name.size()) + 1); // asciiz
    PutInt(out, magic);
    PutInt(out, uncompressed);
    PutInt(out, offset);
    PutInt(out, time);
    PutInt(out, length);
}

// An uncompressed PBO in the shape all 44 retail CWA archives use: header
// entries, a zero-name terminator, then the members' bytes back to back. No
// property block and no trailer, because those archives have neither.
void WritePbo(const fs::path& path, const std::vector<PboMember>& members)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    for (const PboMember& m : members)
    {
        PutEntry(out, m.name, 0, 0, 0, m.time, static_cast<int32_t>(m.data.size()));
    }
    PutEntry(out, "", 0, 0, 0, 0, 0); // terminator
    for (const PboMember& m : members)
    {
        if (!m.data.empty())
        {
            out.write(reinterpret_cast<const char*>(m.data.data()), static_cast<std::streamsize>(m.data.size()));
        }
    }
    out.close();
    REQUIRE(fs::exists(path));
}

std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string ReadMember(const QFBank& bank, const char* name)
{
    Ref<IFileBuffer> buffer = bank.Read(name);
    if (!buffer || !buffer->GetData())
    {
        return std::string();
    }
    return std::string(buffer->GetData(), static_cast<size_t>(buffer->GetSize()));
}

// Bank name is the path without ".pbo" -- QFBank::open appends it.
std::string BankNameOf(const fs::path& pbo)
{
    std::string s = pbo.string();
    return s.substr(0, s.size() - 4);
}

struct TempPbo
{
    fs::path path;
    explicit TempPbo(const char* stem, const std::vector<PboMember>& members)
        : path(fs::temp_directory_path() / (std::string("ast003_") + stem + ".pbo"))
    {
        WritePbo(path, members);
    }
    ~TempPbo()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

} // namespace

TEST_CASE("QFBank::GetContentHash is the same for identical bytes under different names",
          "[qstream][pbo][hash][ast003]")
{
    // Same content, two names, and the names differ in length and in directory
    // depth so that anything derived from the path could not coincide.
    const std::vector<uint8_t> shared = Bytes("the same forty-two bytes of content, twice");
    TempPbo pbo("same_bytes", {
                                  {"alpha.txt", shared, 1000},
                                  {"deep\\nested\\a_much_longer_name.bin", shared, 2000},
                                  {"different.txt", Bytes("the same forty-two bytes of content, twicE"), 1000},
                              });

    QFBank bank;
    REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
    bank.Lock();
    REQUIRE(bank.error() == false);

    const RString a = bank.GetContentHash("alpha.txt");
    const RString b = bank.GetContentHash("deep\\nested\\a_much_longer_name.bin");
    const RString c = bank.GetContentHash("different.txt");

    REQUIRE(a.GetLength() == 64);
    REQUIRE(b.GetLength() == 64);
    REQUIRE(c.GetLength() == 64);

    // Same bytes -> same hash, whatever they are called.
    REQUIRE(strcmp((const char*)a, (const char*)b) == 0);
    // One byte apart -> different hash. (A path-derived hash also separates these,
    // which is why the first assertion above is the load-bearing one.)
    REQUIRE(strcmp((const char*)a, (const char*)c) != 0);

    // And it really is SHA-256 of the content, not of something that merely
    // behaves like a hash.
    REQUIRE(std::string((const char*)a) == Foundation::Sha256::Of(shared.data(), shared.size()));

    bank.Unlock();
}

TEST_CASE("QFBank::GetContentHash ignores the member timestamp", "[qstream][pbo][hash][ast003]")
{
    // Two archives, byte-identical members, different per-member mtimes. The
    // header timestamp is the other thing DerivedAssetKey refuses to key on:
    // an addon rebuilt identically must not invalidate anything.
    const std::vector<uint8_t> content = Bytes("content that did not change when the addon was rebuilt");
    TempPbo older("mtime_old", {{"model.p3d", content, 1}});
    TempPbo newer("mtime_new", {{"model.p3d", content, 2000000000}});

    QFBank bankA;
    QFBank bankB;
    REQUIRE(bankA.open(RString(BankNameOf(older.path).c_str())));
    REQUIRE(bankB.open(RString(BankNameOf(newer.path).c_str())));
    bankA.Lock();
    bankB.Lock();

    const RString a = bankA.GetContentHash("model.p3d");
    const RString b = bankB.GetContentHash("model.p3d");
    REQUIRE(a.GetLength() == 64);
    REQUIRE(strcmp((const char*)a, (const char*)b) == 0);

    bankA.Unlock();
    bankB.Unlock();
}

TEST_CASE("QFBank::GetContentHash is stable across two independent opens", "[qstream][pbo][hash][ast003]")
{
    TempPbo pbo("stable", {
                              {"one.bin", Bytes("first member"), 10},
                              {"two.bin", Bytes("second member, longer"), 20},
                          });

    std::string firstOne;
    std::string firstTwo;
    {
        QFBank bank;
        REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
        bank.Lock();
        firstOne = (const char*)bank.GetContentHash("one.bin");
        firstTwo = (const char*)bank.GetContentHash("two.bin");
        bank.Unlock();
    }
    REQUIRE(firstOne.size() == 64);
    REQUIRE(firstTwo.size() == 64);
    REQUIRE(firstOne != firstTwo);

    // A fresh QFBank over the same file, nothing shared with the first.
    QFBank again;
    REQUIRE(again.open(RString(BankNameOf(pbo.path).c_str())));
    again.Lock();
    REQUIRE(std::string((const char*)again.GetContentHash("one.bin")) == firstOne);
    REQUIRE(std::string((const char*)again.GetContentHash("two.bin")) == firstTwo);
    again.Unlock();
}

TEST_CASE("QFBank::GetContentHash memoises without changing the answer", "[qstream][pbo][hash][ast003]")
{
    TempPbo pbo("memo", {
                            {"a.bin", Bytes("aaaa"), 1},
                            {"b.bin", Bytes("bbbbbb"), 2},
                        });

    QFBank bank;
    REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
    bank.Lock();

    REQUIRE(bank.ContentHashCacheSize() == 0);
    const std::string first = (const char*)bank.GetContentHash("a.bin");
    REQUIRE(bank.ContentHashCacheSize() == 1);
    const std::string second = (const char*)bank.GetContentHash("a.bin");
    REQUIRE(bank.ContentHashCacheSize() == 1); // second call did not recompute
    REQUIRE(first == second);

    bank.GetContentHash("b.bin");
    REQUIRE(bank.ContentHashCacheSize() == 2);

    // An absent member is not cached and is reported as "no answer", which a
    // caller must be able to tell apart from the hash of an empty member.
    REQUIRE(bank.GetContentHash("nosuch.bin").GetLength() == 0);
    REQUIRE(bank.ContentHashCacheSize() == 2);

    bank.Unlock();
}

TEST_CASE("QFBank::GetContentHash leaves member reads correct", "[qstream][pbo][hash][ast003]")
{
    // Reading a member seeks inside the archive. Hashing does a read of its own,
    // so the regression to guard against is a later read coming back short or
    // shifted. Read every member before hashing, hash them, read them again.
    const std::string one = "member one payload";
    const std::string two = "member two payload, deliberately a different length";
    const std::string three(4096, 'z');
    TempPbo pbo("reads_ok", {
                                {"one.bin", Bytes(one), 1},
                                {"two.bin", Bytes(two), 2},
                                {"three.bin", Bytes(three), 3},
                            });

    QFBank bank;
    REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
    bank.Lock();

    REQUIRE(ReadMember(bank, "one.bin") == one);
    REQUIRE(ReadMember(bank, "two.bin") == two);
    REQUIRE(ReadMember(bank, "three.bin") == three);

    bank.GetContentHash("two.bin");
    bank.GetContentHash("one.bin");
    bank.GetContentHash("three.bin");

    REQUIRE(ReadMember(bank, "one.bin") == one);
    REQUIRE(ReadMember(bank, "two.bin") == two);
    REQUIRE(ReadMember(bank, "three.bin") == three);

    // And the hashes agree with the bytes the reads produced.
    REQUIRE(std::string((const char*)bank.GetContentHash("one.bin")) == Foundation::Sha256::Of(one));
    REQUIRE(std::string((const char*)bank.GetContentHash("three.bin")) == Foundation::Sha256::Of(three));

    bank.Unlock();
}

TEST_CASE("QFBank::GetContentHash on the checked-in PBO fixture", "[qstream][pbo][hash][ast003]")
{
    // A real archive rather than one this test wrote, so the reader's own index
    // handling is in the loop.
    REQUIRE_FIXTURE("pbo/addon_fixture.pbo");
    const std::string fixture = GetTestFixturePath("pbo/addon_fixture.pbo");

    QFBank bank;
    REQUIRE(bank.open(RString(BankNameOf(fs::path(fixture)).c_str())));
    bank.Lock();
    REQUIRE(bank.error() == false);

    std::vector<std::string> names;
    struct Collect
    {
        std::vector<std::string>* names;
    } ctx{&names};
    bank.ForEach([](const FileInfoO& fi, const FileBankType*, void* c)
                 { static_cast<Collect*>(c)->names->push_back((const char*)fi.name); }, &ctx);
    REQUIRE(!names.empty());

    for (const std::string& name : names)
    {
        const RString hash = bank.GetContentHash(name.c_str());
        REQUIRE(hash.GetLength() == 64);
        Ref<IFileBuffer> buffer = bank.Read(name.c_str());
        REQUIRE(buffer);
        REQUIRE(std::string((const char*)hash) ==
                Foundation::Sha256::Of(buffer->GetData(), static_cast<size_t>(buffer->GetSize())));
    }

    bank.Unlock();
}

// Hidden (leading-dot tag): never runs in the default suite, so it costs the
// normal run nothing and does not move the case count. Run it explicitly with
//   PoseidonTests "[ast003bench]" --  and point AST003_BENCH_PBO at a real
// archive (without the .pbo extension), e.g. a retail
//   ".../ARMA Cold War Assault/AddOns/Data3D".
// Reports index time, cold whole-archive read time, and the added cost of
// hashing every member -- the numbers the decision doc quotes.
TEST_CASE("QFBank::GetContentHash cost on a real archive", "[.][ast003bench]")
{
    const char* env = std::getenv("AST003_BENCH_PBO");
    if (!env || !*env)
    {
        WARN("AST003_BENCH_PBO not set; skipping");
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto Ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    const int kRuns = 5;
    for (int run = 0; run < kRuns; ++run)
    {
        QFBank bank;
        const auto t0 = Clock::now();
        REQUIRE(bank.open(RString(env)));
        bank.Lock();
        const auto t1 = Clock::now(); // index (open + Load: header parse)

        std::vector<std::string> names;
        struct Collect
        {
            std::vector<std::string>* names;
        } ctx{&names};
        bank.ForEach([](const FileInfoO& fi, const FileBankType*, void* c)
                     { static_cast<Collect*>(c)->names->push_back((const char*)fi.name); }, &ctx);

        uint64_t bytes = 0;
        uint64_t sink = 0;
        const auto t2 = Clock::now();
        for (const std::string& n : names)
        {
            Ref<IFileBuffer> b = bank.Read(n.c_str());
            if (b && b->GetData() && b->GetSize() > 0)
            {
                bytes += static_cast<uint64_t>(b->GetSize());
                // Touch the bytes: a memory-mapped read is otherwise free and the
                // comparison against hashing would be against nothing at all.
                sink += static_cast<unsigned char>(b->GetData()[b->GetSize() - 1]);
            }
        }
        const auto t3 = Clock::now(); // read every member

        for (const std::string& n : names)
        {
            bank.GetContentHash(n.c_str());
        }
        const auto t4 = Clock::now(); // hash every member (first, uncached)

        for (const std::string& n : names)
        {
            bank.GetContentHash(n.c_str());
        }
        const auto t5 = Clock::now(); // hash every member again (memoised)

        std::printf("[ast003bench] run %d  members=%zu bytes=%llu  index=%.2fms read=%.2fms "
                    "hash-first=%.2fms hash-memo=%.3fms  (sink=%llu)\n",
                    run, names.size(), (unsigned long long)bytes, Ms(t1 - t0), Ms(t3 - t2), Ms(t4 - t3), Ms(t5 - t4),
                    (unsigned long long)sink);
        std::fflush(stdout);
        bank.Unlock();
    }
}
