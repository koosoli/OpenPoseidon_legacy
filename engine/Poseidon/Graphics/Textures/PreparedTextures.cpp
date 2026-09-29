#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/World/Terrain/WarmTextureProvenance.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <mutex>
#include <memory>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Poseidon::render
{

struct PreparedTextureStore::Impl
{
    using Clock = std::chrono::steady_clock;
    struct DdsDecode
    {
        std::vector<uint8_t> input;
        TextureSourceDDS decoded;
        size_t bytes = 0;
    };
    struct Entry
    {
        std::string key;
        PAABlockChain chain;    // DXT mip chain...
        DecodedImage image;     // ...OR a whole-file RGBA8 decode (image.valid() discriminates)
        Clock::time_point stored;
        std::shared_ptr<const DdsDecode> dds;
        std::unique_ptr<TextureSourceDDS> preparedDds;
        DdsPreparationOptions ddsOptions{};
        size_t preparedDdsBytes = 0;
        DdsPublicationToken publication;
        std::unique_ptr<PreparedAlphaFacts> alphaFacts;
        std::shared_ptr<const ArchiveSourceBinding> warmSource;
        std::optional<BankReadRequest> physicalSource;
    };
    static size_t AddCharge(size_t a, size_t b)
    { return b > std::numeric_limits<size_t>::max() - a ? std::numeric_limits<size_t>::max() : a + b; }
    static size_t ChainBytes(const PAABlockChain& chain)
    {
        if (chain.levels.capacity() > std::numeric_limits<size_t>::max() / sizeof(PAABlockLevel))
            return std::numeric_limits<size_t>::max();
        return AddCharge(chain.blocks.capacity(), chain.levels.capacity() * sizeof(PAABlockLevel));
    }
    static size_t EntryBytes(const Entry& entry)
    {
        size_t bytes = AddCharge(ChainBytes(entry.chain) + entry.image.rgba.capacity() +
            (entry.dds ? entry.dds->bytes : 0) + entry.preparedDdsBytes + AlphaFactsBytes(entry.alphaFacts.get()),
            entry.warmSource ? entry.warmSource->KnownCppBytes() : 0);
        if (entry.physicalSource)
            bytes = AddCharge(bytes, AddCharge(sizeof(BankReadRequest) + 1 + 64 +
                entry.physicalSource->archive.capacity(), entry.physicalSource->ArchiveIdentityBytes()));
        return bytes;
    }
    static size_t AlphaFactsBytes(const PreparedAlphaFacts* facts)
    {
        // Known C++ capacities plus a conservative shared_ptr control-block
        // guard. Shared archive leases are deliberately charged per entry.
        return facts ? AlphaFactsSourceBytes(facts->source) : 0;
    }
    static size_t AlphaFactsSourceBytes(const BankReadRequest& source)
    {
        return AddCharge(AddCharge(sizeof(PreparedAlphaFacts) + 1 + 64, source.archive.capacity()),
                         source.ArchiveIdentityBytes());
    }
    mutable std::mutex mutex;
    // Insertion order = production order = (through the preparer's queue) distance order, so
    // the front is the oldest and the TTL sweep walks from there and stops early.
    std::list<Entry> order;
    std::list<Entry>::iterator cancellationCursor = order.end();
    size_t tokenEntries = 0;
    std::unordered_map<std::string, std::list<Entry>::iterator> byKey;
    std::unordered_set<std::string> uploaded;
    size_t bytes = 0;
    uint64_t generation = 0;
    Stats stats;
    void CountCancelled(const Entry& entry)
    {
        if (entry.preparedDds) ++stats.ddsCancelled;
        else ++stats.paaCancelled;
    }

    void EraseLocked(std::unordered_map<std::string, std::list<Entry>::iterator>::iterator it, PAABlockChain* takeInto,
                     DecodedImage* takeImageInto = nullptr, std::list<Entry>* retired = nullptr,
                     Streaming::WarmTextureProvenance::RetireReason reason = Streaming::WarmTextureProvenance::RetireReason::Other,
                     BankReadRequest* takePhysicalInto = nullptr)
    {
        std::list<Entry>::iterator node = it->second;
        if (node->warmSource)
            if (auto* proof = Streaming::WarmTextureProvenance::Active())
            {
                BankReadMemberIdentity member;
                if (node->warmSource->Request().CopyMemberIdentity(member))
                    proof->NoteRetired(proof->Find(node->key, member), reason);
            }
        if (cancellationCursor == node) ++cancellationCursor;
        if (node->publication.inventory) --tokenEntries;
        bytes -= EntryBytes(*node);
        if (takePhysicalInto && node->physicalSource)
            *takePhysicalInto = std::move(*node->physicalSource);
        if (node->dds)
            stats.ddsReuseBytes -= node->dds->bytes;
        if (takeInto)
            *takeInto = std::move(node->chain);
        if (takeImageInto)
            *takeImageInto = std::move(node->image);
        if (retired)
            retired->splice(retired->end(), order, node);
        else
            order.erase(node);
        byKey.erase(it);
        stats.entries = byKey.size();
        stats.bytes = bytes;
    }

    // Drop entries past their TTL. Called under the lock at Put time only, so an idle store
    // costs nothing; a busy one sweeps a handful of stale heads per insert. Freed chains are
    // returned to the caller to release outside the lock.
    void SweepLocked(std::list<Entry>& retired)
    {
        const double ttl = TtlSeconds();
        if (ttl <= 0.0)
            return;
        const Clock::time_point now = Clock::now();
        while (!order.empty())
        {
            const Entry& head = order.front();
            const double age = std::chrono::duration<double>(now - head.stored).count();
            if (age < ttl)
                break;
            auto it = byKey.find(head.key);
            if (it != byKey.end())
                EraseLocked(it, nullptr, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::Expired);
            else
            {
                // Defensive unindexed TTL path: keep the exact warm-source reason honest too.
                if (head.warmSource)
                    if (auto* proof = Streaming::WarmTextureProvenance::Active())
                    {
                        BankReadMemberIdentity member;
                        if (head.warmSource->Request().CopyMemberIdentity(member))
                            proof->NoteRetired(proof->Find(head.key, member),
                                Streaming::WarmTextureProvenance::RetireReason::Expired);
                    }
                if (cancellationCursor == order.begin()) ++cancellationCursor;
                if (head.publication.inventory) --tokenEntries;
                bytes -= EntryBytes(head);
                if (head.dds) stats.ddsReuseBytes -= head.dds->bytes;
                retired.splice(retired.end(), order, order.begin());
            }
            ++stats.expired;
        }
        stats.entries = byKey.size();
        stats.bytes = bytes;
    }

    // Worker maintenance only: bounded visits, with destruction outside mutex.
    void SweepCancelledLocked(std::list<Entry>& retired)
    {
        if (!tokenEntries) return;
        const size_t visits = std::min<size_t>(32, order.size());
        for (size_t i = 0; i < visits && !order.empty(); ++i)
        {
            if (cancellationCursor == order.end()) cancellationCursor = order.begin();
            auto node = cancellationCursor++;
            if (node->publication.inventory && !node->publication.Valid())
            {
                auto indexed = byKey.find(node->key);
                if (indexed != byKey.end())
                {
                    CountCancelled(*node);
                    EraseLocked(indexed, nullptr, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::Cancelled);
                }
            }
        }
    }
};

PreparedTextureStore& PreparedTextureStore::Instance()
{
    static PreparedTextureStore store;
    return store;
}

PreparedTextureStore::PreparedTextureStore() : _impl(new Impl) {}
PreparedTextureStore::~PreparedTextureStore()
{
    delete _impl;
}

bool PreparedTextureStore::Enabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_TEXTURE_PREPARE");
        // Default ON: the worker side is a pure file read, and the main-thread side validates
        // every chain against the header it parsed itself before trusting a byte of it, so the
        // failure mode of a wrong chain is "ignored, counted, old path" -- not a wrong texture.
        // =0 is the A/B: workers never read a texture, EnsureUploaded never looks here.
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

bool PreparedTextureStore::NativeDdsEnabled()
{
    static const bool on = [] {
        const char* value = std::getenv("WGR_NATIVE_DDS_PREPARE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return Enabled() && on; // A/B experiment until native lifecycle/performance acceptance.
}

bool PreparedTextureStore::NativeDdsBc3Only()
{
    static const bool on = [] {
        const char* value = std::getenv("WGR_NATIVE_DDS_BC3_ONLY");
        return value && std::strcmp(value, "1") == 0;
    }();
    return NativeDdsEnabled() && on;
}

size_t PreparedTextureStore::ByteBudget()
{
    static const size_t bytes = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_TEXTURE_PREPARE_MB");
        const long mb = v ? std::strtol(v, nullptr, 10) : 512;
        return static_cast<size_t>(std::clamp<long>(mb, 1, 16384)) * 1024u * 1024u;
    }();
    return bytes;
}

double PreparedTextureStore::TtlSeconds()
{
    static const double s = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_TEXTURE_PREPARE_TTL_S");
        const double parsed = v ? std::strtod(v, nullptr) : 30.0;
        return parsed < 0.0 ? 0.0 : parsed;
    }();
    return s;
}

std::string PreparedTextureStore::Key(const char* name)
{
    std::string key;
    if (!name)
        return key;
    key.reserve(std::strlen(name));
    for (const char* p = name; *p; ++p)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        else if (c == '/')
            c = '\\';
        key.push_back(c);
    }
    return key;
}

bool PreparedTextureStore::ShouldPrepare(const std::string& key)
{
    if (!Enabled() || key.empty())
        return false;
    std::list<Impl::Entry> retired;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->SweepCancelledLocked(retired);
    const auto old = _impl->byKey.find(key);
    if (old != _impl->byKey.end() && old->second->publication.inventory && !old->second->publication.Valid())
    {
        _impl->CountCancelled(*old->second);
        _impl->EraseLocked(old, nullptr, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::Cancelled);
    }
    if (_impl->uploaded.count(key))
    {
        ++_impl->stats.skippedUploaded;
        return false;
    }
    if (_impl->byKey.count(key))
        return false;
    return _impl->bytes < ByteBudget();
}

uint64_t PreparedTextureStore::Generation() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->generation;
}

bool PreparedTextureStore::CanRetainAlphaFacts(const std::string& key, const PAABlockChain& chain,
    const BankReadRequest& source, uint64_t generation, const DdsPublicationToken* publication)
{
    if (!Enabled() || key.empty() || !chain.valid() || !source.HasArchiveIdentity() ||
        chain.magic < 0xFF02 || chain.magic > 0xFF05 || !chain.levels[0].legacyBlockReadCompatible ||
        chain.levels[0].sourceHeaderOffset == SIZE_MAX) return false;
    // Same retained-charge arithmetic as Put, without copying strings or pixels.
    // A copied string may have a different capacity; final Put always checks its
    // actual charge. This snapshot is neither a reservation nor acceptance.
    const size_t factsBytes = Impl::AlphaFactsSourceBytes(source);
    const size_t chainBytes = Impl::ChainBytes(chain);
    std::list<Impl::Entry> retired;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if ((publication && !publication->Valid()) || generation != _impl->generation || _impl->uploaded.count(key)) return false;
    const auto old = _impl->byKey.find(key);
    if (old != _impl->byKey.end() && old->second->publication.inventory && !old->second->publication.Valid())
    {
        _impl->CountCancelled(*old->second);
        _impl->EraseLocked(old, nullptr, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::Cancelled);
    }
    if (_impl->byKey.count(key)) return false;
    const size_t available = ByteBudget() - std::min(_impl->bytes, ByteBudget());
    if (chainBytes > available || factsBytes > available - chainBytes)
    {
        ++_impl->stats.alphaFactsCapacitySkipped;
        return false;
    }
    return true;
}

std::unique_ptr<PreparedAlphaFacts> PreparedTextureStore::BuildAlphaFacts(const PAABlockChain& chain,
    const BankReadRequest& source, const std::function<bool()>& cancelled)
{
    constexpr size_t limit = 16 * 1024 * 1024;
    auto stop = [&] { return cancelled && cancelled(); };
    if (!source.HasArchiveIdentity() || source.bytes > limit || !chain.valid() ||
        chain.blocks.size() > limit || chain.blocks.capacity() > 2 * limit || chain.levels.size() > 32 ||
        chain.levels.capacity() > 8192 / sizeof(PAABlockLevel) ||
        chain.magic < 0xFF02 || chain.magic > 0xFF05 || stop()) return nullptr;
    const auto& top = chain.levels[0];
    if (!top.legacyBlockReadCompatible || top.sourceHeaderOffset == SIZE_MAX ||
        top.sourceHeaderOffset > source.bytes || source.bytes - top.sourceHeaderOffset < 7 ||
        top.offset != 0 || top.width != chain.width || top.height != chain.height ||
        top.width <= 0 || top.height <= 0 || top.offset > chain.blocks.size() ||
        top.size > chain.blocks.size() - top.offset ||
        static_cast<uint64_t>(top.width) * static_cast<uint64_t>(top.height) > limit) return nullptr;
    std::vector<uint8_t> alpha;
    if (!DecodeBlockAlpha(chain.blocks.data() + top.offset, top.size, top.width, top.height,
        chain.magic == 0xFF04 || chain.magic == 0xFF05, alpha) || alpha.capacity() > 2 * limit || stop()) return nullptr;
    const AlphaStats stats = ClassifyAlphaSamples(alpha.data(), alpha.size(), 1);
    if (stop()) return nullptr;
    AlphaShapeAnalysis shape;
    if ((stats.kind == AlphaStats::Cutout && stats.pctClear > 0.0) ||
        (stats.kind == AlphaStats::Blend && stats.pctClear >= 2.0))
        shape = MeasureAlphaShapeAnalysis(alpha.data(), top.width, top.height, 1, 0);
    if (stop()) return nullptr;
    auto facts = std::make_unique<PreparedAlphaFacts>();
    facts->source = source;
    facts->magic = chain.magic;
    facts->width = top.width; facts->height = top.height;
    facts->topOffset = top.offset; facts->topBytes = top.size;
    facts->sourceHeaderOffset = top.sourceHeaderOffset;
    facts->histogram = stats; facts->shape = shape;
    facts->pairedBlocks = chain.blocks.data();
    return facts;
}

bool PreparedTextureStore::HasAlphaFacts(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    const auto it = _impl->byKey.find(key);
    return it != _impl->byKey.end() && bool(it->second->alphaFacts) &&
        (!it->second->publication.inventory || it->second->publication.Valid());
}

bool PreparedTextureStore::CopyAlphaFacts(const std::string& key, const BankReadRequest& current,
    uint16_t magic, int width, int height, size_t topBytes, int sourceHeaderOffset, PreparedAlphaFacts& out)
{
    out = PreparedAlphaFacts{};
    std::lock_guard<std::mutex> lock(_impl->mutex);
    const auto it = _impl->byKey.find(key);
    const auto* entry = it != _impl->byKey.end() ? &*it->second : nullptr;
    const auto* facts = entry ? entry->alphaFacts.get() : nullptr;
    const double ttl = TtlSeconds();
    if (!facts || (entry->publication.inventory && !entry->publication.Valid()) ||
        (ttl > 0.0 && std::chrono::duration<double>(Impl::Clock::now() - entry->stored).count() >= ttl) ||
        !facts->source.SameArchiveMember(current) || facts->magic != magic || facts->width != width ||
        facts->height != height || facts->topOffset != 0 || facts->topBytes != topBytes ||
        sourceHeaderOffset < 0 || facts->sourceHeaderOffset != static_cast<size_t>(sourceHeaderOffset))
    {
        ++_impl->stats.alphaFactsMisses;
        return false;
    }
    out = *facts; // leaves both raw mip chain and its uploaded mark untouched
    ++_impl->stats.alphaFactsHits;
    return true;
}

void PreparedTextureStore::NoteAlphaFactsCancelled()
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    ++_impl->stats.alphaFactsCancelled;
}

bool PreparedTextureStore::Put(const std::string& key, PAABlockChain&& chain,
                               std::optional<uint64_t> generation, std::unique_ptr<PreparedAlphaFacts> alphaFacts,
                               const DdsPublicationToken* publication,
                               std::shared_ptr<const ArchiveSourceBinding> warmSource,
                               const BankReadRequest* physicalSource)
{
    if (!Enabled() || key.empty() || !chain.valid())
        return false;
    if (warmSource && (!generation || !publication || !publication->Valid() ||
        !warmSource->Request().HasArchiveIdentity() || warmSource->Request().bytes > 16 * 1024 * 1024))
        return false;
    if (physicalSource && (warmSource || !generation || !publication || !publication->Valid() ||
        !physicalSource->HasArchiveIdentity() || physicalSource->bytes > 16 * 1024 * 1024))
        return false;
    if (alphaFacts && (!generation || alphaFacts->pairedBlocks != chain.blocks.data() || !alphaFacts->source.HasArchiveIdentity() ||
        alphaFacts->magic != chain.magic || alphaFacts->width != chain.width || alphaFacts->height != chain.height ||
        alphaFacts->topOffset != chain.levels[0].offset || alphaFacts->topBytes != chain.levels[0].size ||
        !chain.levels[0].legacyBlockReadCompatible || alphaFacts->sourceHeaderOffset != chain.levels[0].sourceHeaderOffset))
        alphaFacts.reset(); // unsupported facts never prevent the ordinary chain fallback
    if (alphaFacts) alphaFacts->pairedBlocks = nullptr;
    std::list<Impl::Entry> pending;
    pending.emplace_back(Impl::Entry{key, std::move(chain), DecodedImage{}, Impl::Clock::now()});
    pending.back().alphaFacts = std::move(alphaFacts);
    pending.back().warmSource = std::move(warmSource);
    if (physicalSource) pending.back().physicalSource = *physicalSource;
    const bool boundWarm = bool(pending.back().warmSource);
    const bool physical = bool(pending.back().physicalSource);
    auto* trace = Streaming::WarmTextureProvenance::Active();
    const auto traceToken = trace && boundWarm ? Streaming::WarmTextureProvenance::Match(key, pending.back().warmSource) : 0;
    if (publication) pending.back().publication = *publication;
    // Conservatively charge a shared source witness per entry as well as the
    // source-capture process cap. Neither number measures physical memory.
    const size_t retained = Impl::EntryBytes(pending.back());
    const size_t size = pending.back().chain.blocks.size();
    std::list<Impl::Entry> freeLater;
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->SweepLocked(freeLater);
        _impl->SweepCancelledLocked(freeLater);
        const auto old = _impl->byKey.find(key);
        if (old != _impl->byKey.end() && old->second->publication.inventory && !old->second->publication.Valid())
        {
            _impl->CountCancelled(*old->second);
            _impl->EraseLocked(old, nullptr, nullptr, &freeLater, Streaming::WarmTextureProvenance::RetireReason::Cancelled);
        }
        if (publication && (!generation || !publication->Valid()))
        {
            ++_impl->stats.paaCancelled;
        }
        else if (generation && *generation != _impl->generation)
        {
            ++_impl->stats.rejectedStale;
        }
        else if (!pending.back().warmSource && !pending.back().physicalSource && _impl->uploaded.count(key))
        {
            ++_impl->stats.skippedUploaded;
        }
        else if (_impl->byKey.count(key))
        {
            ++_impl->stats.rejectedDup;
        }
        else if (retained > ByteBudget() - std::min(_impl->bytes, ByteBudget()))
        {
            ++_impl->stats.rejectedFull;
        }
        else
        {
            // Index allocation precedes noexcept splice; failures retire owned
            // payloads outside the mutex without creating an unindexed entry.
            _impl->byKey.emplace(key, pending.begin());
            if (pending.back().alphaFacts) ++_impl->stats.alphaFactsPuts;
            if (pending.back().publication.inventory) ++_impl->tokenEntries;
            _impl->order.splice(_impl->order.end(), pending);
            _impl->bytes += retained;
            ++_impl->stats.puts;
            if (boundWarm) ++_impl->stats.warmPuts;
            if (physical) ++_impl->stats.physicalPuts;
            _impl->stats.putBytes += size;
            _impl->stats.entries = _impl->byKey.size();
            _impl->stats.bytes = _impl->bytes;
            stored = true;
            if (boundWarm && trace) trace->NoteStored(traceToken); // exact published entry, under existing store lock
        }
    }
    freeLater.clear(); // outside the lock
    if (!stored)
        chain = PAABlockChain{};
    if (trace) Streaming::WarmTextureProvenance::Emit(trace->Stamp(traceToken, stored ?
        Streaming::WarmTextureProvenance::Event::StorePut :
        Streaming::WarmTextureProvenance::Event::StorePutRefused));
    return stored;
}

bool PreparedTextureStore::PutDecoded(const std::string& key, DecodedImage&& image,
                                      std::optional<uint64_t> generation)
{
    if (!Enabled() || key.empty() || !image.valid())
        return false;
    std::list<Impl::Entry> freeLater;
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->SweepLocked(freeLater);
        if (generation && *generation != _impl->generation)
        {
            ++_impl->stats.rejectedStale;
        }
        else if (_impl->uploaded.count(key))
        {
            ++_impl->stats.skippedUploaded;
        }
        else if (_impl->byKey.count(key))
        {
            ++_impl->stats.rejectedDup;
        }
        else if (image.rgba.capacity() > ByteBudget() - std::min(_impl->bytes, ByteBudget()))
        {
            ++_impl->stats.rejectedFull;
        }
        else
        {
            const size_t size = image.rgba.size();
            const size_t retained = image.rgba.capacity();
            _impl->order.push_back(Impl::Entry{key, PAABlockChain{}, std::move(image), Impl::Clock::now()});
            _impl->byKey.emplace(key, std::prev(_impl->order.end()));
            _impl->bytes += retained;
            ++_impl->stats.decodedPuts;
            _impl->stats.putBytes += size;
            _impl->stats.entries = _impl->byKey.size();
            _impl->stats.bytes = _impl->bytes;
            stored = true;
        }
    }
    freeLater.clear(); // outside the lock
    if (!stored)
        image = DecodedImage{};
    return stored;
}

bool PreparedTextureStore::TakeDecoded(const std::string& key, DecodedImage& out)
{
    out = DecodedImage{};
    if (!Enabled() || key.empty())
        return false;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    auto it = _impl->byKey.find(key);
    if (it == _impl->byKey.end() || !it->second->image.valid())
        return false; // no miss counter: the fallback path asks opportunistically
    _impl->EraseLocked(it, nullptr, &out);
    ++_impl->stats.decodedTakes;
    _impl->stats.takeBytes += out.rgba.size();
    return out.valid();
}

bool PreparedTextureStore::Take(const std::string& key, PAABlockChain& out,
                               std::shared_ptr<const ArchiveSourceBinding>* warmSource)
{
    out = PAABlockChain{};
    if (warmSource) warmSource->reset();
    if (!Enabled() || key.empty())
        return false;
    std::list<Impl::Entry> retired;
    using Proof = Streaming::WarmTextureProvenance;
    struct Exit
    {
        Proof* proof = nullptr; uint32_t token = 0;
        Proof::Event event = Proof::Event::StoreTakeMissKeyOnly;
        explicit Exit(const std::string& key) : proof(Proof::Active()), token(proof ? proof->KeyOnly(key) : 0) {}
        ~Exit() { if (proof) Proof::Emit(proof->Stamp(token, event)); }
    } trace(key);
    std::lock_guard<std::mutex> lock(_impl->mutex);
    auto it = _impl->byKey.find(key);
    if (it == _impl->byKey.end() || !it->second->chain.valid())
    {
        ++_impl->stats.misses;
        return false;
    }
    if (it->second->publication.inventory && !it->second->publication.Valid())
    {
        if (it->second->warmSource)
        {
            trace.event = Proof::Event::StoreTakeCancelled;
            trace.token = Proof::Match(key, it->second->warmSource);
        }
        else trace.event = Proof::Event::Count;
        _impl->CountCancelled(*it->second);
        _impl->EraseLocked(it, nullptr, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::Cancelled);
        ++_impl->stats.misses;
        return false;
    }
    if (it->second->warmSource)
    {
        trace.token = Proof::Match(key, it->second->warmSource);
        if (!warmSource)
        {
            trace.event = Proof::Event::StoreTakeRefused;
            ++_impl->stats.warmSourceRefused;
            _impl->EraseLocked(it, nullptr, nullptr, &retired);
            ++_impl->stats.misses;
            return false;
        }
        *warmSource = it->second->warmSource;
        trace.event = Proof::Event::StoreTakeWarm;
        ++_impl->stats.warmTakes;
    }
    if (it->second->physicalSource)
    {
        // A key-only or old warm-source consumer has no access to the physical
        // identity checked against the initialized PAA header. Retire rather
        // than leave a same-key chain available for a later unsafe claim.
        trace.event = Proof::Event::Count;
        ++_impl->stats.physicalSourceRefused;
        _impl->EraseLocked(it, nullptr, nullptr, &retired);
        ++_impl->stats.misses;
        return false;
    }
    if (!it->second->warmSource) trace.event = Proof::Event::Count; // successful non-warm take is not a warm miss
    _impl->EraseLocked(it, &out, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::Consumed);
    ++_impl->stats.takes;
    _impl->stats.takeBytes += out.blocks.size();
    return true;
}

bool PreparedTextureStore::TakePhysical(const std::string& key,
    const BankReadMemberIdentity& initializedMember, PAABlockChain& out, BankReadRequest& source)
{
    out = PAABlockChain{};
    source = BankReadRequest{};
    if (!Enabled() || key.empty()) return false;
    std::list<Impl::Entry> retired;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    const auto it = _impl->byKey.find(key);
    if (it == _impl->byKey.end() || !it->second->chain.valid() || !it->second->physicalSource)
    {
        ++_impl->stats.misses;
        return false;
    }
    if (it->second->publication.inventory && !it->second->publication.Valid())
    {
        _impl->CountCancelled(*it->second);
        _impl->EraseLocked(it, nullptr, nullptr, &retired,
            Streaming::WarmTextureProvenance::RetireReason::Cancelled);
        ++_impl->stats.misses;
        return false;
    }
    BankReadMemberIdentity retainedMember;
    if (!it->second->physicalSource->CopyMemberIdentity(retainedMember) ||
        !(retainedMember == initializedMember))
    {
        ++_impl->stats.physicalSourceRefused;
        _impl->EraseLocked(it, nullptr, nullptr, &retired);
        ++_impl->stats.misses;
        return false;
    }
    _impl->EraseLocked(it, &out, nullptr, &retired,
        Streaming::WarmTextureProvenance::RetireReason::Consumed, &source);
    ++_impl->stats.takes;
    ++_impl->stats.physicalTakes;
    _impl->stats.takeBytes += out.blocks.size();
    return true;
}

PreparedTextureStore::WarmEntryProbe PreparedTextureStore::ProbeWarmEntry(
    const std::string& key, const BankReadRequest* expected) const
{
    WarmEntryProbe result;
    if (!Enabled() || key.empty()) return result;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    result.storeGeneration = _impl->generation;
    result.uploadedMarkPresent = _impl->uploaded.count(key) != 0;
    const auto found = _impl->byKey.find(key);
    if (found == _impl->byKey.end()) return result;
    const auto& entry = *found->second;
    result.entryPresent = true;
    result.validChain = entry.chain.valid();
    result.warmBound = bool(entry.warmSource);
    result.publicationValid = !entry.publication.inventory || entry.publication.Valid();
    const double ttl = TtlSeconds();
    result.withinTtl = ttl <= 0.0 ||
        std::chrono::duration<double>(Impl::Clock::now() - entry.stored).count() < ttl;
    if (expected && entry.warmSource)
    {
        BankReadMemberIdentity a, b;
        result.sameMember = expected->CopyMemberIdentity(a) &&
            entry.warmSource->Request().CopyMemberIdentity(b) && a == b;
    }
    return result;
}

bool PreparedTextureStore::ShouldPrepareWarm(const std::string& key)
{
    if (!Enabled() || key.empty()) return false;
    std::list<Impl::Entry> retired;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->SweepCancelledLocked(retired);
    const auto old = _impl->byKey.find(key);
    if (old != _impl->byKey.end() && old->second->publication.inventory && !old->second->publication.Valid())
    {
        _impl->CountCancelled(*old->second);
        _impl->EraseLocked(old, nullptr, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::Cancelled);
    }
    return _impl->byKey.count(key) == 0;
}

size_t PreparedTextureStore::PromotePublishedWarmBeforeCancel(const DdsPublicationInventory& inventory,
    const uint32_t* wantedEpochs, const std::vector<bool>& priorStale,
    size_t count, uint32_t currentEpoch)
{
    if (!Foundation::IsMainThread() || !Enabled() || !wantedEpochs || !currentEpoch || !count) return 0;
    using Proof = Streaming::WarmTextureProvenance;
    auto* proof = Proof::Active();
    size_t visited = 0, promoted = 0;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    // The existing store capacity remains authoritative. This is a bounded
    // opportunity, not a completeness scan; an unvisited entry keeps its token.
    for (auto it = _impl->order.begin(); it != _impl->order.end() && visited < 32; ++it, ++visited)
    {
        if (!it->chain.valid() || !it->warmSource || !it->warmSource->Request().HasArchiveIdentity()) continue;
        const auto& publication = it->publication;
        if (publication.inventory.get() != &inventory || publication.index >= count ||
            publication.index >= priorStale.size() || priorStale[publication.index] ||
            wantedEpochs[publication.index] == currentEpoch || !publication.Valid()) continue;
        if (proof)
            proof->NotePublishedPromotion(Proof::Match(it->key, it->warmSource));
        // Put finished before this lock cut. An in-flight Put cannot enter the
        // store until this lock is released; it keeps its model token. Clear
        // still destroys this entry and increments the world-store generation.
        it->publication = {};
        --_impl->tokenEntries;
        ++promoted;
    }
    return promoted;
}

void PreparedTextureStore::NoteWarmUpload(bool successful, size_t bytes)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (successful) { ++_impl->stats.warmUploads; _impl->stats.warmUploadBytes += bytes; }
    else ++_impl->stats.warmUploadFailed;
}

bool PreparedTextureStore::PutDdsPrepared(const std::string& key, std::unique_ptr<TextureSourceDDS> source,
                                         const DdsPreparationOptions& options, uint64_t generation,
                                         const DdsPublicationToken* publication)
{
    if (!Enabled() || key.empty() || !source || source->_levels.empty() ||
        source->_mipmaps != static_cast<int>(source->_levels.size()) ||
        Key(static_cast<const char*>(source->_name)) != key ||
        options.decodedMaxEdge < 64 || options.decodedMaxEdge > 8192)
        return false;
    const size_t bytes = source->PreparedByteSize();
    std::list<Impl::Entry> pending;
    pending.emplace_back(Impl::Entry{key, {}, {}, Impl::Clock::now()});
    pending.back().preparedDds = std::move(source);
    pending.back().ddsOptions = options;
    pending.back().preparedDdsBytes = bytes;
    if (publication) pending.back().publication = *publication;
    std::list<Impl::Entry> retired;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->SweepLocked(retired);
    _impl->SweepCancelledLocked(retired);
    if (publication && !publication->Valid()) { ++_impl->stats.ddsCancelled; return false; }
    if (generation != _impl->generation) { ++_impl->stats.rejectedStale; return false; }
    if (_impl->uploaded.count(key)) { ++_impl->stats.skippedUploaded; return false; }
    const auto previous = _impl->byKey.find(key);
    if (previous != _impl->byKey.end() && previous->second->publication.inventory && !previous->second->publication.Valid())
    {
        _impl->CountCancelled(*previous->second);
        _impl->EraseLocked(previous, nullptr, nullptr, &retired);
    }
    if (_impl->byKey.count(key)) { ++_impl->stats.rejectedDup; return false; }
    if (bytes > ByteBudget() - std::min(ByteBudget(), _impl->bytes)) {
        ++_impl->stats.rejectedFull;
        return false;
    }
    // Allocate the index before the noexcept splice: an allocation failure must
    // not leave an unindexed payload resident or destroy it under this mutex.
    _impl->byKey.emplace(key, pending.begin());
    if (pending.back().publication.inventory) ++_impl->tokenEntries;
    pending.back().stored = Impl::Clock::now();
    _impl->order.splice(_impl->order.end(), pending);
    _impl->bytes += bytes;
    ++_impl->stats.ddsPreparedPuts;
    _impl->stats.putBytes += bytes;
    _impl->stats.bytes = _impl->bytes;
    _impl->stats.entries = _impl->byKey.size();
    return true;
}

std::unique_ptr<TextureSourceDDS> PreparedTextureStore::TakeDdsPrepared(
    const std::string& key, const DdsPreparationOptions& options)
{
    if (!Enabled() || key.empty()) return nullptr;
    std::list<Impl::Entry> retired;
    std::unique_ptr<TextureSourceDDS> result;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->SweepLocked(retired);
        const auto it = _impl->byKey.find(key);
        if (it == _impl->byKey.end() || !it->second->preparedDds) return nullptr;
        // This is the claim linearization point. Cancellation after a valid
        // claim belongs to the owner reset pipeline; cancelled sources never
        // escape even if publication raced a prior Reset/DropStale.
        if (it->second->publication.inventory && !it->second->publication.Valid())
        {
            _impl->EraseLocked(it, nullptr, nullptr, &retired);
            ++_impl->stats.ddsCancelled;
            return nullptr;
        }
        const auto& stored = it->second->ddsOptions;
        if (stored.compressedPassthrough == options.compressedPassthrough &&
            stored.linearTintMultiply == options.linearTintMultiply &&
            stored.decodedMaxEdge == options.decodedMaxEdge) {
            result = std::move(it->second->preparedDds);
            ++_impl->stats.ddsPreparedTakes;
            _impl->stats.takeBytes += it->second->preparedDdsBytes;
        } else {
            ++_impl->stats.ddsConfigurationMisses;
        }
        // Even a mismatched result retires outside the lock. Future requests may
        // prepare under the new settings; the current consumer uses its fallback.
        _impl->EraseLocked(it, nullptr, nullptr, &retired);
    }
    return result;
}

bool PreparedTextureStore::CopyDdsDecode(const std::string& key, const std::vector<uint8_t>& source,
                                        TextureSourceDDS& out)
{
    if (!Enabled() || key.empty() || source.empty()) return false;
    std::shared_ptr<const Impl::DdsDecode> snapshot;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        const auto it = _impl->byKey.find(key);
        if (it == _impl->byKey.end() || !it->second->dds) return false;
        const double ttl = TtlSeconds();
        if (ttl > 0 && std::chrono::duration<double>(Impl::Clock::now() - it->second->stored).count() >= ttl)
            return false;
        snapshot = it->second->dds;
    }
    // A remount or an edited loose file must never reuse another source's pixels.
    if (snapshot->input != source) return false;
    const RStringB requestedName = out._name;
    out = snapshot->decoded;
    out._name = requestedName;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->stats.ddsReuseHits;
    }
    return true;
}

bool PreparedTextureStore::PutDdsDecode(const std::string& key, const std::vector<uint8_t>& source,
                                       const TextureSourceDDS& decoded)
{
    if (!Enabled() || key.empty() || source.empty() || decoded._levels.empty()) return false;
    const size_t cap = std::min<size_t>(64u * 1024u * 1024u, ByteBudget() / 4);
    size_t estimate = sizeof(Impl::DdsDecode) + source.size() + decoded._levels.size() * sizeof(DDSMipLevel) +
                      decoded._tintAverageLevel.data.size();
    for (const auto& level : decoded._levels) estimate += level.data.size();
    std::list<Impl::Entry> retired;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->SweepLocked(retired);
        if (_impl->byKey.count(key) || estimate > cap - std::min(cap, _impl->stats.ddsReuseBytes) ||
            estimate > ByteBudget() - std::min(ByteBudget(), _impl->bytes)) return false;
    }
    retired.clear();
    auto snapshot = std::make_shared<Impl::DdsDecode>();
    snapshot->input = source;
    snapshot->decoded = decoded;
    snapshot->bytes = sizeof(Impl::DdsDecode) + snapshot->input.capacity() +
                      snapshot->decoded._levels.capacity() * sizeof(DDSMipLevel) +
                      snapshot->decoded._tintAverageLevel.data.capacity();
    for (const auto& level : snapshot->decoded._levels) snapshot->bytes += level.data.capacity();
    // Preparation and large copies never hold the worker hand-off mutex. Recheck
    // the shared budget because a worker may have inserted a payload meanwhile.
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (_impl->byKey.count(key) || snapshot->bytes > cap - std::min(cap, _impl->stats.ddsReuseBytes) ||
        snapshot->bytes > ByteBudget() - std::min(ByteBudget(), _impl->bytes)) return false;
    const size_t bytes = snapshot->bytes;
    _impl->order.push_back(Impl::Entry{key, {}, {}, Impl::Clock::now(), std::move(snapshot)});
    _impl->byKey.emplace(key, std::prev(_impl->order.end()));
    _impl->bytes += bytes;
    _impl->stats.ddsReuseBytes += bytes;
    ++_impl->stats.ddsReusePuts;
    _impl->stats.entries = _impl->byKey.size();
    _impl->stats.bytes = _impl->bytes;
    return true;
}

void PreparedTextureStore::MarkUploaded(const std::string& key)
{
    if (!Enabled() || key.empty())
        return;
    std::list<Impl::Entry> retired;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->uploaded.insert(key);
        _impl->stats.uploadedMarks = _impl->uploaded.size();
        auto it = _impl->byKey.find(key);
        if (it != _impl->byKey.end())
            _impl->EraseLocked(it, nullptr, nullptr, &retired, Streaming::WarmTextureProvenance::RetireReason::UploadedMark);
    }
    // Both payload kinds free here, outside the lock.
}

void PreparedTextureStore::MarkEvicted(const std::string& key)
{
    if (!Enabled() || key.empty())
        return;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->uploaded.erase(key);
    _impl->stats.uploadedMarks = _impl->uploaded.size();
}

void PreparedTextureStore::Clear()
{
    auto* proof = Streaming::WarmTextureProvenance::Active();
    std::list<Impl::Entry> freeLater;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        if (proof) proof->NoteClearLive(); // at most eight exact registered sources; no store scan
        ++_impl->generation;
        freeLater.swap(_impl->order);
        _impl->cancellationCursor = _impl->order.end();
        _impl->tokenEntries = 0;
        _impl->byKey.clear();
        _impl->uploaded.clear();
        _impl->bytes = 0;
        _impl->stats.entries = 0;
        _impl->stats.bytes = 0;
        _impl->stats.ddsReuseBytes = 0;
        _impl->stats.uploadedMarks = 0;
    }
    freeLater.clear();
    if (proof) proof->FlushRetirements(); // Clear records only under the lock; logs afterward
}

PreparedTextureStore::Stats PreparedTextureStore::SnapshotStats() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->stats;
}

} // namespace Poseidon::render
