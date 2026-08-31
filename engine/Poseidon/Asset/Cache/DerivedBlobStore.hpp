#pragma once

#include <Poseidon/Asset/Cache/DerivedAssetKey.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Poseidon::Asset::Cache
{

// AST-002 -- the on-disk half of the Derived Data Cache.
//
// DerivedAssetKey (AST-020) already answers "what is this derived artefact's identity".
// It had no store: nothing wrote a blob under that key, so a correct key invalidated
// nothing. This is the store, and it is deliberately a dumb keyed blob store -- it
// knows about bytes, not about meshes or textures. A producer serialises its own
// artefact and hands it over; the store's only job is to give back exactly those bytes
// or nothing at all.
//
// THE THREE THINGS THAT MAKE IT MORE THAN A FILE WRITE, each of which is a failure the
// existing sky-bake cache (engine/WgpuRenderer/rust/src/gfx3d/mod.rs) can hit:
//
//   1. THE KEY IS ECHOED INSIDE THE ENTRY. Sky-bake names a file by a 64-bit FNV-1a and
//      then trusts whatever is at that path. A collision, a hand-copied file, or a
//      stale entry left by a key scheme that has since changed all read back as a HIT
//      of the wrong data, silently and content-dependently. Here the entry carries its
//      own full key id and a read that does not match the requested one is a miss.
//
//   2. THE PAYLOAD IS DIGESTED. Sky-bake validates a magic and an exact length. A file
//      truncated to a shorter length is caught; a file whose bytes were corrupted in
//      place is not, and neither is one torn at exactly the right size. The payload
//      SHA-256 is in the header, so any corruption is a miss.
//
//   3. THE TEMPORARY FILE IS UNIQUE PER WRITER. Sky-bake writes to
//      `path.with_extension("tmp")` -- a name derived only from the key, therefore the
//      SAME name for every process and thread writing that key. Two writers interleave
//      into one temp and the rename publishes the interleaving. Here the temp carries
//      the process id and a per-process counter, so concurrent writers of one key never
//      share a file and the rename is the only publication point.
//
// A corrupt entry is never trusted and never fatal: it is counted, unlinked so it does
// not cost every later run a read, and reported as a miss so the caller regenerates.
//
// SIZE BOUND. Sky-bake grows without limit under %LOCALAPPDATA%. Prune() evicts
// oldest-mtime-first until the tree is under the byte budget. It is explicit rather
// than automatic on every Put because eviction is a whole-directory walk and the right
// moment to pay for it is a level change, not a model load.
//
// THREADING. There is no lock. Correctness across threads and across processes rests on
// rename being atomic and on entries being immutable once published -- a key names one
// content forever, so two writers racing on a key are writing identical bytes and
// whichever rename lands last is right either way. A rename that fails (on Windows a
// reader may hold the target open) leaves the existing valid entry in place and is
// counted, not raised.
class DerivedBlobStore
{
  public:
    // Container format, independent of the producer's compiler version. Bump when the
    // header layout below changes; older files then fail the check and are misses.
    static constexpr uint32_t kContainerVersion = 1;
    static constexpr char     kMagic[4]         = {'P', 'D', 'C', '1'};

    struct Stats
    {
        uint64_t hits            = 0; // entry found, verified, returned
        uint64_t misses          = 0; // no file at the path
        uint64_t rejectedCorrupt = 0; // file present but unreadable/inconsistent -> miss + unlink
        uint64_t writes          = 0; // Put published an entry
        uint64_t writeFailures   = 0; // Put could not publish (disk full, rename lost a race)
        uint64_t evictions       = 0; // entries removed by Prune
        uint64_t bytesRead       = 0;
        uint64_t bytesWritten    = 0;
    };

    explicit DerivedBlobStore(std::filesystem::path root, uint64_t budgetBytes = 512ull * 1024 * 1024)
        : root_(std::move(root)), budgetBytes_(budgetBytes)
    {
    }

    // %LOCALAPPDATA%/OpenPoseidon/derived on Windows, $XDG_CACHE_HOME (or ~/.cache)
    // /OpenPoseidon/derived elsewhere. Nothing is created here; Put creates on demand.
    static std::optional<std::filesystem::path> DefaultRoot()
    {
#if defined(_WIN32)
        if (const char* base = std::getenv("LOCALAPPDATA"); base && *base)
            return std::filesystem::path(base) / "OpenPoseidon" / "derived";
        return std::nullopt;
#else
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "OpenPoseidon" / "derived";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".cache" / "OpenPoseidon" / "derived";
        return std::nullopt;
#endif
    }

    const std::filesystem::path& Root() const { return root_; }
    uint64_t                     BudgetBytes() const { return budgetBytes_; }
    void                         SetBudgetBytes(uint64_t bytes) { budgetBytes_ = bytes; }
    const Stats&                 GetStats() const { return stats_; }
    void                         ResetStats() { stats_ = Stats{}; }

    // The identity string stored inside the entry and used as its file name. Carries the
    // compiler version explicitly so artefacts from two versions coexist rather than
    // overwrite -- a rollback must not read forward-version output.
    static std::string EntryId(const DerivedAssetKey& key) { return key.FileName(); }

    bool Contains(const DerivedAssetKey& key) const
    {
        std::vector<uint8_t> ignored;
        return Get(key, ignored);
    }

    // True and `out` filled iff a verified entry exists. False for every other reason --
    // absent, truncated, wrong container version, wrong key, corrupt payload -- and the
    // caller's response to all of them is the same: regenerate.
    bool Get(const DerivedAssetKey& key, std::vector<uint8_t>& out) const
    {
        const std::string id   = EntryId(key);
        const std::string sha  = key.Compute();
        const auto        path = PathFor(sha, id);

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            ++stats_.misses;
            return false;
        }
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        if (!Decode(raw, id, out))
        {
            ++stats_.rejectedCorrupt;
            // Unlink rather than leave it: a poisoned entry that stays costs every
            // subsequent run a read and a hash before it reaches the same conclusion.
            std::error_code ec;
            std::filesystem::remove(path, ec);
            out.clear();
            return false;
        }
        ++stats_.hits;
        stats_.bytesRead += out.size();
        return true;
    }

    bool Put(const DerivedAssetKey& key, const void* data, size_t size)
    {
        const std::string id  = EntryId(key);
        const std::string sha = key.Compute();
        const auto        dir = ShardDir(sha);

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            ++stats_.writeFailures;
            return false;
        }

        const std::vector<uint8_t> encoded = Encode(id, data, size);

        // Unique per process and per call: two writers of one key must not share a temp.
        static std::atomic<uint64_t> counter{0};
        const auto                   tmp =
            dir / (".tmp-" + std::to_string(ProcessId()) + "-" +
                   std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + "-" + sha.substr(0, 16));
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                ++stats_.writeFailures;
                return false;
            }
            file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
            file.flush();
            if (!file)
            {
                file.close();
                std::filesystem::remove(tmp, ec);
                ++stats_.writeFailures;
                return false;
            }
        }

        std::filesystem::rename(tmp, PathFor(sha, id), ec);
        if (ec)
        {
            // The existing entry (if any) is still valid; losing this race costs nothing
            // but the regeneration we already paid for.
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            ++stats_.writeFailures;
            return false;
        }
        ++stats_.writes;
        stats_.bytesWritten += size;
        return true;
    }

    bool Put(const DerivedAssetKey& key, const std::vector<uint8_t>& blob)
    {
        return Put(key, blob.data(), blob.size());
    }

    uint64_t TotalBytes() const
    {
        uint64_t total = 0;
        ForEachEntry([&](const std::filesystem::path&, uint64_t size, std::filesystem::file_time_type) {
            total += size;
        });
        return total;
    }

    size_t EntryCount() const
    {
        size_t n = 0;
        ForEachEntry([&](const std::filesystem::path&, uint64_t, std::filesystem::file_time_type) { ++n; });
        return n;
    }

    // Evict oldest-mtime-first until the tree fits the budget. Returns entries removed.
    // mtime is an approximation of last use: entries are immutable, so it is really
    // "least recently written". Refining it to true LRU means touching a file on every
    // hit, which is a write per read -- not obviously worth it, and measurable later.
    size_t Prune()
    {
        struct Entry
        {
            std::filesystem::path         path;
            uint64_t                      size;
            std::filesystem::file_time_type mtime;
        };
        std::vector<Entry> entries;
        uint64_t           total = 0;
        ForEachEntry([&](const std::filesystem::path& p, uint64_t size, std::filesystem::file_time_type mtime) {
            entries.push_back({p, size, mtime});
            total += size;
        });
        if (total <= budgetBytes_)
            return 0;

        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.mtime != b.mtime)
                return a.mtime < b.mtime;
            return a.path < b.path; // deterministic tie-break; mtime resolution is coarse
        });

        size_t removed = 0;
        for (const Entry& e : entries)
        {
            if (total <= budgetBytes_)
                break;
            std::error_code ec;
            if (std::filesystem::remove(e.path, ec) && !ec)
            {
                total -= e.size;
                ++removed;
                ++stats_.evictions;
            }
        }
        return removed;
    }

  private:
    // Layout on disk:
    //   <root>/<first two hex of sha>/v<compilerVersion>-<sha>.pdc
    // Sharded because a flat directory of a hundred thousand entries is slow to
    // enumerate on NTFS, and Prune enumerates.
    std::filesystem::path ShardDir(const std::string& sha) const
    {
        return root_ / (sha.size() >= 2 ? sha.substr(0, 2) : std::string("00"));
    }

    std::filesystem::path PathFor(const std::string& sha, const std::string& id) const
    {
        return ShardDir(sha) / id;
    }

    // Header: magic(4) container(4) idLen(4) payloadLen(8) payloadSha256Hex(64) id payload
    static constexpr size_t kFixedHeader = 4 + 4 + 4 + 8 + 64;

    static std::vector<uint8_t> Encode(const std::string& id, const void* data, size_t size)
    {
        const std::string digest = Foundation::Sha256::Of(data, size);
        std::vector<uint8_t> out;
        out.reserve(kFixedHeader + id.size() + size);
        Append(out, kMagic, 4);
        AppendU32(out, kContainerVersion);
        AppendU32(out, static_cast<uint32_t>(id.size()));
        AppendU64(out, static_cast<uint64_t>(size));
        Append(out, digest.data(), 64);
        Append(out, id.data(), id.size());
        Append(out, data, size);
        return out;
    }

    // Every rejection path here is a MISS, never an exception and never a partial fill.
    static bool Decode(const std::vector<uint8_t>& raw, const std::string& expectedId, std::vector<uint8_t>& out)
    {
        if (raw.size() < kFixedHeader)
            return false;
        if (std::memcmp(raw.data(), kMagic, 4) != 0)
            return false;
        if (ReadU32(raw, 4) != kContainerVersion)
            return false;
        const uint32_t idLen      = ReadU32(raw, 8);
        const uint64_t payloadLen = ReadU64(raw, 12);
        const std::string digest(reinterpret_cast<const char*>(raw.data()) + 20, 64);

        // Overflow-safe: idLen and payloadLen come from a file that may be hostile or
        // merely damaged, so compute the expected size in a width that cannot wrap.
        const uint64_t expected = static_cast<uint64_t>(kFixedHeader) + idLen + payloadLen;
        if (expected != raw.size())
            return false;

        const std::string id(reinterpret_cast<const char*>(raw.data()) + kFixedHeader, idLen);
        if (id != expectedId)
            return false; // collision, stale scheme, or a file copied to the wrong name

        const uint8_t* payload = raw.data() + kFixedHeader + idLen;
        if (Foundation::Sha256::Of(payload, static_cast<size_t>(payloadLen)) != digest)
            return false;

        out.assign(payload, payload + payloadLen);
        return true;
    }

    template <typename F>
    void ForEachEntry(F&& fn) const
    {
        std::error_code ec;
        if (!std::filesystem::exists(root_, ec))
            return;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root_, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            if (!it->is_regular_file(ec) || ec)
                continue;
            const auto name = it->path().filename().string();
            if (name.rfind(".tmp-", 0) == 0)
                continue; // an in-flight write is not an entry
            const auto size  = static_cast<uint64_t>(it->file_size(ec));
            if (ec)
                continue;
            const auto mtime = std::filesystem::last_write_time(it->path(), ec);
            if (ec)
                continue;
            fn(it->path(), size, mtime);
        }
    }

    static void Append(std::vector<uint8_t>& out, const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    static void AppendU32(std::vector<uint8_t>& out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    static void AppendU64(std::vector<uint8_t>& out, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    static uint32_t ReadU32(const std::vector<uint8_t>& raw, size_t off)
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(raw[off + i]) << (8 * i);
        return v;
    }
    static uint64_t ReadU64(const std::vector<uint8_t>& raw, size_t off)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(raw[off + i]) << (8 * i);
        return v;
    }

    static unsigned long ProcessId()
    {
#if defined(_WIN32)
        return static_cast<unsigned long>(_getpid());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }

    std::filesystem::path root_;
    uint64_t              budgetBytes_;
    mutable Stats         stats_;
};

} // namespace Poseidon::Asset::Cache
