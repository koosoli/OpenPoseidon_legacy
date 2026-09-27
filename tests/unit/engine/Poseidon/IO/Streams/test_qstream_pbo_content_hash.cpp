#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/IO/FileServerMT.hpp>
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
#include <future>
#include <string>
#include <vector>
#ifdef _WIN32
#include <Poseidon/Foundation/Common/Win.h>
#endif

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

TEST_CASE("FileCache selective bank flush removes exact debt without evicting unrelated bytes",
          "[qstream][pbo][file-cache]")
{
    const std::string suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    TempPbo first(("filecache_first_" + suffix).c_str(),
                  {{"one.bin", Bytes(std::string(16, 'a')), 0},
                   {"two.bin", Bytes(std::string(32, 'b')), 0}});
    TempPbo second(("filecache_second_" + suffix).c_str(),
                   {{"keep.bin", Bytes(std::string(24, 'k')), 0}});
    // The cache also reads loose bytes. This private path shares the helper's
    // cleanup ownership, but intentionally holds a raw blob rather than an archive.
    TempPbo loose(("filecache_loose_" + suffix).c_str(), {});
    {
        std::ofstream out(loose.path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << std::string(32, 'c');
    }
    QFBank firstBank, secondBank;
    REQUIRE(firstBank.open(RString(BankNameOf(first.path).c_str())));
    REQUIRE(secondBank.open(RString(BankNameOf(second.path).c_str())));
    QIFStreamB one, two, keep;
    one.open(firstBank, "one.bin");
    two.open(firstBank, "two.bin");
    keep.open(secondBank, "keep.bin");
    REQUIRE_FALSE(one.fail());
    REQUIRE_FALSE(two.fail());
    REQUIRE_FALSE(keep.fail());
    REQUIRE(one.rest() == 16);
    REQUIRE(two.rest() == 32);
    REQUIRE(keep.rest() == 24);
    struct CachingGuard
    {
        bool previous = GEnableCaching;
        CachingGuard() { GEnableCaching = true; }
        ~CachingGuard() { GEnableCaching = previous; }
    } caching;
    FileCache cache(80, 16);
    cache.Store(one, "first/one.bin");
    cache.Store(keep, "second/keep.bin");
    cache.Store(two, "first/two.bin");
    REQUIRE(cache.HeldBytes() == 72);
    cache.FlushBank(&firstBank);
    CHECK_FALSE(cache.IsLoaded("first/one.bin"));
    CHECK_FALSE(cache.IsLoaded("first/two.bin"));
    REQUIRE(cache.IsLoaded("second/keep.bin"));
    REQUIRE(cache.HeldBytes() == 24);
    cache.FlushBank(&firstBank); // An already flushed bank cannot subtract twice.
    REQUIRE(cache.HeldBytes() == 24);

    QIFStream newFile;
    cache.Open(newFile, loose.path.string().c_str());
    REQUIRE_FALSE(newFile.fail());
    CHECK(newFile.rest() == 32);
    // 24+32 fits the real 80-byte budget. Phantom pre-flush debt previously
    // exceeded it and discarded keep.bin despite ample actual cache capacity.
    REQUIRE(cache.HeldBytes() == 56);
    REQUIRE(cache.IsLoaded("second/keep.bin"));
    QIFStream unrelated;
    cache.Open(unrelated, "second/keep.bin");
    char data[24]{};
    unrelated.read(data, sizeof(data));
    REQUIRE_FALSE(unrelated.fail());
    CHECK(std::string(data, sizeof(data)) == std::string(24, 'k'));
    REQUIRE(cache.HeldBytes() == 56); // Reader cursors do not change held debt.

    cache.FlushBank(&secondBank);
    CHECK_FALSE(cache.IsLoaded("second/keep.bin"));
    REQUIRE(cache.HeldBytes() == 32);
    cache.FlushBank(nullptr); // Existing all-bank semantics also include loose entries.
    REQUIRE(cache.HeldBytes() == 0);
    cache.FlushBank(nullptr);
    REQUIRE(cache.HeldBytes() == 0);
}

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


TEST_CASE("Owned bank reads preserve member ranges without sharing the bank cursor", "[pbo][owned-read]")
{
    const std::string first(131071, 'a'), second(65537, 'z');
    TempPbo pbo("owned_read", {{"first.bin", Bytes(first), 0}, {"second.bin", Bytes(second), 0}});
    QFBank bank;
    REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
    bank.Lock();
    auto a = bank.CaptureReadRequest("first.bin");
    auto b = bank.CaptureReadRequest("second.bin");
    REQUIRE(a); REQUIRE(b);
    CHECK_FALSE(bank.CaptureReadRequest("absent.bin"));
    auto readMany = [](BankReadRequest request, const std::string& expected) {
        for (int i = 0; i < 20; ++i) {
            std::vector<char> bytes;
            if (!request.Read(bytes) || std::string(bytes.begin(), bytes.end()) != expected) return false;
        }
        return true;
    };
    auto workerA = std::async(std::launch::async, readMany, *a, first);
    auto workerB = std::async(std::launch::async, readMany, *b, second);
    CHECK(ReadMember(bank, "second.bin") == second);
    CHECK(ReadMember(bank, "first.bin") == first);
    CHECK(workerA.get()); CHECK(workerB.get());
    std::vector<char> bytes{'x'};
    auto truncated = *b;
    ++truncated.bytes; // range extends one byte beyond the archive
    CHECK_FALSE(truncated.Read(bytes));
    a->offset = UINT64_MAX;
    CHECK_FALSE(a->Read(bytes));
    CHECK(bytes.empty());
    b->bytes = 65ull * 1024 * 1024;
    CHECK_FALSE(b->Read(bytes));
}

TEST_CASE("Archive identity is optional and unleased aggregate reads remain available", "[pbo][owned-read][archive-lease]")
{
    TempPbo pbo("lease_optional", {{"member.bin", Bytes("member"), 0}});
    QFBank bank;
    REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
    bank.Lock();
    auto ordinary = bank.CaptureReadRequest("member.bin");
    REQUIRE(ordinary);
    CHECK_FALSE(ordinary->HasArchiveIdentity());
    BankReadRequest aggregate{ordinary->archive, ordinary->offset, ordinary->bytes};
    CHECK_FALSE(aggregate.HasArchiveIdentity());
    CHECK_FALSE(aggregate.SameArchiveMember(*ordinary));
    std::vector<char> data;
    REQUIRE(aggregate.Read(data));
    CHECK(std::string(data.begin(), data.end()) == "member");
#ifndef _WIN32
    CHECK_FALSE(bank.CaptureReadRequest("member.bin", true));
#endif
}

#ifdef _WIN32
TEST_CASE("Windows archive leases bind physical members and independent reader cursors", "[pbo][owned-read][archive-lease]")
{
    const std::string first(131071, 'a'), second(65537, 'z');
    TempPbo pbo("lease_identity", {{"first.bin", Bytes(first), 0}, {"second.bin", Bytes(second), 0}});
    TempPbo other("lease_identity_other", {{"first.bin", Bytes(first), 0}, {"second.bin", Bytes(second), 0}});
    QFBank bank, otherBank;
    REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
    REQUIRE(otherBank.open(RString(BankNameOf(other.path).c_str())));
    bank.Lock(); otherBank.Lock();
    auto a = bank.CaptureReadRequest("first.bin", true);
    auto recaptured = bank.CaptureReadRequest("first.bin", true);
    auto b = bank.CaptureReadRequest("second.bin", true);
    auto differentFile = otherBank.CaptureReadRequest("first.bin", true);
    REQUIRE(a); REQUIRE(recaptured); REQUIRE(b); REQUIRE(differentFile);
    CHECK(a->HasArchiveIdentity());
    CHECK(a->SameArchiveMember(*recaptured));
    CHECK_FALSE(a->SameArchiveMember(*b));
    CHECK_FALSE(a->SameArchiveMember(*differentFile));
    std::vector<char> data{'x'};
    for (int change = 0; change < 3; ++change)
    {
        auto borrowed = *a;
        if (change == 0) borrowed.archive = differentFile->archive;
        if (change == 1) ++borrowed.offset;
        if (change == 2) ++borrowed.bytes;
        CHECK_FALSE(borrowed.HasArchiveIdentity());
        CHECK_FALSE(borrowed.SameArchiveMember(*a));
        CHECK_FALSE(borrowed.Read(data));
        CHECK(data.empty());
    }
    auto readMany = [](BankReadRequest request, const std::string& expected) {
        for (int i = 0; i < 20; ++i) {
            std::vector<char> bytes;
            if (!request.Read(bytes) || std::string(bytes.begin(), bytes.end()) != expected) return false;
        }
        return true;
    };
    auto workerA = std::async(std::launch::async, readMany, *a, first);
    auto workerB = std::async(std::launch::async, readMany, *b, second);
    CHECK(ReadMember(bank, "second.bin") == second);
    CHECK(ReadMember(bank, "first.bin") == first);
    CHECK(workerA.get()); CHECK(workerB.get());
    bank.close();
    REQUIRE(a->Read(data));
    CHECK(std::string(data.begin(), data.end()) == first);
}

TEST_CASE("Windows request copies retain write and delete denial after unused bank unload", "[pbo][owned-read][archive-lease]")
{
    TempPbo pbo("lease_unload", {{"member.bin", Bytes("held source"), 0}});
    const fs::path renamed = pbo.path.string() + ".renamed";
    struct BankSlot
    {
        int index = GFileBanks.Add();
        ~BankSlot() { GFileBanks.Delete(index); }
    } slot;
    auto& bank = GFileBanks[slot.index];
    REQUIRE(bank.open(RString(BankNameOf(pbo.path).c_str())));
    bank.Lock();
    auto request = bank.CaptureReadRequest("member.bin", true);
    REQUIRE(request);
    auto copy = *request;
    REQUIRE(bank.CanBeUnloaded());
    // UnloadUnused may first retire other mounted banks; bound visits to the
    // inventory and check that this bank was actually unloaded, without Clear.
    for (int i = 0; i <= GFileBanks.Size() && bank.CanBeUnloaded(); ++i)
        GFileBanks.UnloadUnused();
    REQUIRE_FALSE(bank.CanBeUnloaded());
    bank.close();
    const auto wide = pbo.path.wstring();
    const auto moved = renamed.wstring();
    auto denied = [&] {
        HANDLE writer = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD error = GetLastError();
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        CHECK(writer == INVALID_HANDLE_VALUE);
        CHECK(error == ERROR_SHARING_VIOLATION);
        CHECK_FALSE(DeleteFileW(wide.c_str()));
        CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
        CHECK_FALSE(MoveFileExW(wide.c_str(), moved.c_str(), 0));
        CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
    };
    denied();
    request.reset();
    denied(); // the last request copy, rather than the bank, still owns denial
    std::vector<char> data;
    REQUIRE(copy.Read(data));
    CHECK(std::string(data.begin(), data.end()) == "held source");
    copy.archiveLease.reset();
    REQUIRE(MoveFileExW(wide.c_str(), moved.c_str(), 0));
    REQUIRE(MoveFileExW(moved.c_str(), wide.c_str(), 0));
    HANDLE writer = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(writer != INVALID_HANDLE_VALUE);
    CloseHandle(writer);
    REQUIRE(DeleteFileW(wide.c_str()));
    WritePbo(pbo.path, {{"member.bin", Bytes("replacement"), 0}});
    QFBank replacement;
    REQUIRE(replacement.open(RString(BankNameOf(pbo.path).c_str())));
    replacement.Lock();
    auto next = replacement.CaptureReadRequest("member.bin", true);
    REQUIRE(next);
    REQUIRE(next->Read(data));
    CHECK(std::string(data.begin(), data.end()) == "replacement");
}
#endif
