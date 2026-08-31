#include <Poseidon/World/Model/ModelDerivedCache.hpp>

#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelBlob.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

namespace Poseidon
{
namespace
{

// The version of "what the P3D loaders plus Model::compile produce". Bump this
// -- by hand, deliberately -- whenever their output changes for input that did
// not. It is NOT a build stamp: a rebuild that changes nothing must not
// invalidate every cache in existence.
constexpr int kModelProducerVersion = 1;

// 512 MiB, sized against the measured corpus rather than picked round.
//
// The whole CWA retail model corpus is 1,400 `.p3d` members totalling 66.1 MiB,
// and the IR blob measures 2.14x the source (448 models of `AddOns\O.pbo`:
// 26.96 MiB in, 57.76 MiB of entries out -- AST-004 §4). Caching EVERY retail
// model therefore costs ~142 MiB, so 512 MiB holds the entire primary corpus
// three times over and only starts evicting on a foreign, Arma-scale one -- which
// is the case it must not be allowed to fill a disk with.
//
// The expansion is the reason there IS a budget rather than a "keep everything":
// the store is bigger than the assets it derives from, so an unbounded one grows
// past the game install. `POSEIDON_MODEL_DDC_BUDGET_MB` overrides it.
constexpr uint64_t kDefaultBudgetBytes = 512ull * 1024 * 1024;

bool EnvOn(const char* key, bool fallback)
{
    const char* v = std::getenv(key);
    if (!v || !*v)
        return fallback;
    return v[0] != '0';
}

} // namespace

ModelDerivedCache::ModelDerivedCache()
{
    // DEFAULT OFF, and the asymmetry is deliberate. This wiring is proved by unit
    // tests over real archives; it has NOT been validated in the running game,
    // because the game folder was owned by another session for the whole of the
    // work. Shipping a cache on-by-default that nobody has watched load a world
    // trades a measured 8x model-load win for the chance of serving one model's
    // geometry under another's name. The switch is one environment variable and
    // the tests exercise the enabled path, so turning it on is a decision someone
    // can make after a capture, not a rebuild.
    if (!EnvOn("POSEIDON_MODEL_DDC", false))
        return;

    if (const char* root = std::getenv("POSEIDON_MODEL_DDC_ROOT"); root && *root)
    {
        _root = std::filesystem::path(root);
    }
    else if (auto def = Asset::Cache::DerivedBlobStore::DefaultRoot())
    {
        _root = *def / "models";
    }
    else
    {
        return; // no writable location: stay off rather than guess one
    }

    uint64_t budget = kDefaultBudgetBytes;
    if (const char* mb = std::getenv("POSEIDON_MODEL_DDC_BUDGET_MB"); mb && *mb)
    {
        const long long parsed = std::atoll(mb);
        if (parsed > 0)
            budget = static_cast<uint64_t>(parsed) * 1024ull * 1024ull;
    }
    _store = std::make_unique<Asset::Cache::DerivedBlobStore>(_root, budget);
}

ModelDerivedCache& ModelDerivedCache::Instance()
{
    static ModelDerivedCache instance;
    return instance;
}

uint64_t ModelDerivedCache::BudgetBytes() const
{
    return _store ? _store->BudgetBytes() : 0;
}

void ModelDerivedCache::ResetForTesting(const std::filesystem::path& root, uint64_t budgetBytes)
{
    _root  = root;
    _store = std::make_unique<Asset::Cache::DerivedBlobStore>(root, budgetBytes);
    _stats = Stats{};
}

void ModelDerivedCache::DisableForTesting()
{
    _store.reset();
    _stats = Stats{};
}

std::string ModelDerivedCache::HashBytes(const void* data, size_t size)
{
    if (!data || size == 0)
        return std::string();
    return Foundation::Sha256::Of(data, size);
}

std::string ModelDerivedCache::HashBankMember(const char* virtualPath)
{
    if (!virtualPath || !*virtualPath)
        return std::string();
    QFBank* bank = QIFStreamB::AutoBank(virtualPath);
    if (!bank)
        return std::string();
    // AutoBank matches on the bank's mount prefix; the member name is the rest,
    // exactly as QIFStreamB::AutoOpen computes it. Getting this wrong would not
    // be a wrong answer -- GetContentHash returns "" for an absent member and the
    // caller falls back to hashing the bytes -- but it would silently cost the
    // memo, so it is worth doing the same way the opener does.
    const char* member = virtualPath + bank->GetPrefix().GetLength();
    RString     hash   = bank->GetContentHash(member);
    return hash.GetLength() > 0 ? std::string((const char*)hash) : std::string();
}

Asset::Cache::DerivedAssetKey ModelDerivedCache::MakeKey(const std::string& virtualPath,
                                                         const std::string& contentSha)
{
    Asset::Cache::DerivedAssetKey key;
    key.AddInput("model", Asset::VirtualPath::Parse(virtualPath), contentSha);
    key.AddOption("modelProducer", std::to_string(kModelProducerVersion));
    key.AddOption("blobFormat", std::to_string(ModelBlob::kFormatVersion));
    return key;
}

bool ModelDerivedCache::Load(const Asset::Cache::DerivedAssetKey& key, Poseidon::Model::Model& out)
{
    if (!_store)
        return false;
    std::vector<uint8_t> blob;
    if (!_store->Get(key, blob))
    {
        ++_stats.misses;
        return false;
    }
    if (!ModelBlob::Deserialize(blob.data(), blob.size(), out))
    {
        // The store verified the payload digest, so the bytes are the ones that
        // were written; if they still do not decode, the WRITER and this reader
        // disagree -- a format bump that was not accompanied by a version bump.
        // Counted separately from misses for exactly that reason: it is a bug
        // signal, not merely a cold cache.
        ++_stats.decodeFailed;
        return false;
    }
    ++_stats.hits;
    _stats.bytesLoaded += blob.size();
    return true;
}

bool ModelDerivedCache::Store(const Asset::Cache::DerivedAssetKey& key, const Poseidon::Model::Model& model)
{
    if (!_store)
        return false;
    const std::vector<uint8_t> blob = ModelBlob::Serialize(model);
    if (!_store->Put(key, blob))
    {
        ++_stats.storeFailures;
        return false;
    }
    ++_stats.stores;
    _stats.bytesStored += blob.size();
    return true;
}

size_t ModelDerivedCache::Prune()
{
    if (!_store)
        return 0;
    const size_t removed = _store->Prune();
    _stats.evictions += removed;
    if (removed > 0)
    {
        LOG_INFO(World, "Model derived cache: pruned {} entries to the {} MB budget ({} entries, {} MB left)",
                 removed, _store->BudgetBytes() / (1024 * 1024), _store->EntryCount(),
                 _store->TotalBytes() / (1024 * 1024));
    }
    return removed;
}

} // namespace Poseidon
