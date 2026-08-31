#pragma once

#include <Poseidon/Asset/Cache/DerivedAssetKey.hpp>
#include <Poseidon/Asset/Cache/DerivedBlobStore.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace Poseidon
{
namespace Model
{
struct Model;
}

// AST-004 -- the first producer wired to the derived data cache.
//
// AST-002 shipped `DerivedBlobStore` with no customer and AST-003 shipped
// `QFBank::GetContentHash` with no caller. This joins them for exactly one
// producer: the P3D -> `Model::Model` IR conversion, at the point in
// `ModelCache` where the source bytes are in hand and the loader has not run.
//
// WHY THIS PRODUCER, in the numbers that chose it (retail `AddOns\O.pbo`,
// 1,703 members, RelWithDebInfo, the [ast004cost] case):
//
//   sha256 of the member (the key's own cost)     0.4497 ms   <- the floor
//   PAA block chain (streaming prepare)           0.0185 ms   24x BELOW the floor
//   PAA full RGBA decode                          0.2786 ms   below the floor
//   PAA decode + alpha classification             0.4679 ms   at the floor
//   P3D -> Model IR                               9.6164 ms   21x above it
//
// A cache can only pay when the derivation costs more than the key that names
// it. Three of the four candidates cost less, so caching them is a guaranteed
// loss no matter how well the store is written; the model path is the only one
// where the arithmetic works, and it was the one with no serialisation. Writing
// `ModelBlob` was the price of picking the right producer instead of the
// convenient one.
//
// WHAT IS IN THE KEY, and why each part has to be:
//
//   role "model" + the canonical virtual path
//        The path is an INPUT, not just a label: `ODOLLoader::loadFromBuffer`
//        writes it into `Model::sourcePath`, so the same bytes under two names
//        are two different IRs. Dropping it makes the second name serve the
//        first's `sourcePath` -- a wrong-data hit, not a miss.
//   the member's SHA-256 (AST-003)
//        Content, so a rebuilt addon invalidates and an identically-rebuilt one
//        does not.
//   option "modelProducer"
//        The version of the P3D loaders and `Model::compile`. Bump it when their
//        OUTPUT changes for unchanged input. Without it, fixing a loader bug
//        leaves every machine that already cached the broken IR serving it, and
//        the bug reproduces only where it never happened.
//   option "blobFormat"
//        `ModelBlob::kFormatVersion`. A separate axis on purpose: the loaders can
//        change without the envelope changing and vice versa, and collapsing them
//        would either wipe every entry for a field reordering or reinterpret old
//        field offsets as new ones.
//
// The store's own `kContainerVersion` is a third, independent axis and lives
// inside the entry, not in this key.
//
// NOT IN THE KEY, deliberately and checked: the P3D loaders read no environment
// variable and no config -- `grep getenv` over `Asset/Formats/P3D/` and
// `World/Model/` finds nothing outside `ShapeAdapter`, which runs LATER and is
// not cached here. If that ever stops being true, the new switch is an option or
// the cache is wrong.
//
// FAILURE IS ALWAYS A MISS. Nothing here throws and nothing here is fatal; a
// disabled, unwritable or corrupt cache costs a derivation that would have
// happened anyway.
class ModelDerivedCache
{
  public:
    struct Stats
    {
        uint64_t hits          = 0;
        uint64_t misses        = 0;
        uint64_t decodeFailed  = 0; // entry read and verified, but ModelBlob rejected it
        uint64_t stores        = 0;
        uint64_t storeFailures = 0;
        uint64_t evictions     = 0;
        uint64_t bytesLoaded   = 0;
        uint64_t bytesStored   = 0;
    };

    static ModelDerivedCache& Instance();

    // Off unless the store has a usable root. `POSEIDON_MODEL_DDC=0` disables
    // read AND write, so a bad cache can be taken out of the picture without a
    // rebuild; `POSEIDON_MODEL_DDC_ROOT` overrides the location, which is what
    // the tests use so they never touch the user's real cache.
    bool Enabled() const { return _store != nullptr; }

    // Same value `QFBank::GetContentHash` produces for a member: SHA-256 of the
    // logical bytes. Used where no bank is in play (a loose file on disk).
    static std::string HashBytes(const void* data, size_t size);

    // "" when the path names nothing in a bank, or banks are off. Goes through
    // AST-003's memo, so a member hashed once is not hashed again.
    static std::string HashBankMember(const char* virtualPath);

    static Asset::Cache::DerivedAssetKey MakeKey(const std::string& virtualPath, const std::string& contentSha);

    // False for every reason -- disabled, absent, corrupt, undecodable -- and the
    // caller's response to all of them is the same: derive.
    bool Load(const Asset::Cache::DerivedAssetKey& key, Poseidon::Model::Model& out);

    bool Store(const Asset::Cache::DerivedAssetKey& key, const Poseidon::Model::Model& model);

    // Evict oldest-first to the byte budget. Explicit, because it walks the whole
    // tree; call it at a world change, not per model. Returns entries removed.
    size_t Prune();

    const Stats&                 GetStats() const { return _stats; }
    void                         ResetStats() { _stats = Stats{}; }
    const std::filesystem::path& Root() const { return _root; }
    uint64_t                     BudgetBytes() const;

    // Point the singleton at a different root/budget. For tests: production code
    // has no reason to move the cache mid-run.
    void ResetForTesting(const std::filesystem::path& root, uint64_t budgetBytes);

    // Put the singleton back to the state a build with the switch off starts in.
    // A test that leaves the cache armed changes what every LATER test in the same
    // process measures, which is a far more confusing failure than the one it was
    // written to catch.
    void DisableForTesting();

  private:
    ModelDerivedCache();

    std::filesystem::path                         _root;
    std::unique_ptr<Asset::Cache::DerivedBlobStore> _store;
    Stats                                         _stats;
};

} // namespace Poseidon
