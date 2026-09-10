#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <mutex>
#include <memory>
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
    };
    static size_t EntryBytes(const Entry& entry)
    { return entry.chain.blocks.size() + entry.image.rgba.size() + (entry.dds ? entry.dds->bytes : 0) + entry.preparedDdsBytes; }
    mutable std::mutex mutex;
    // Insertion order = production order = (through the preparer's queue) distance order, so
    // the front is the oldest and the TTL sweep walks from there and stops early.
    std::list<Entry> order;
    std::unordered_map<std::string, std::list<Entry>::iterator> byKey;
    std::unordered_set<std::string> uploaded;
    size_t bytes = 0;
    uint64_t generation = 0;
    Stats stats;

    void EraseLocked(std::unordered_map<std::string, std::list<Entry>::iterator>::iterator it, PAABlockChain* takeInto,
                     DecodedImage* takeImageInto = nullptr, std::list<Entry>* retired = nullptr)
    {
        std::list<Entry>::iterator node = it->second;
        bytes -= EntryBytes(*node);
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
                EraseLocked(it, nullptr, nullptr, &retired);
            else
            {
                bytes -= EntryBytes(head);
                if (head.dds) stats.ddsReuseBytes -= head.dds->bytes;
                retired.splice(retired.end(), order, order.begin());
            }
            ++stats.expired;
        }
        stats.entries = byKey.size();
        stats.bytes = bytes;
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
    std::lock_guard<std::mutex> lock(_impl->mutex);
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

bool PreparedTextureStore::Put(const std::string& key, PAABlockChain&& chain,
                               std::optional<uint64_t> generation)
{
    if (!Enabled() || key.empty() || !chain.valid())
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
        else if (_impl->bytes + chain.blocks.size() > ByteBudget())
        {
            ++_impl->stats.rejectedFull;
        }
        else
        {
            const size_t size = chain.blocks.size();
            _impl->order.push_back(Impl::Entry{key, std::move(chain), DecodedImage{}, Impl::Clock::now()});
            _impl->byKey.emplace(key, std::prev(_impl->order.end()));
            _impl->bytes += size;
            ++_impl->stats.puts;
            _impl->stats.putBytes += size;
            _impl->stats.entries = _impl->byKey.size();
            _impl->stats.bytes = _impl->bytes;
            stored = true;
        }
    }
    freeLater.clear(); // outside the lock
    if (!stored)
        chain = PAABlockChain{};
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
        else if (_impl->bytes + image.rgba.size() > ByteBudget())
        {
            ++_impl->stats.rejectedFull;
        }
        else
        {
            const size_t size = image.rgba.size();
            _impl->order.push_back(Impl::Entry{key, PAABlockChain{}, std::move(image), Impl::Clock::now()});
            _impl->byKey.emplace(key, std::prev(_impl->order.end()));
            _impl->bytes += size;
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

bool PreparedTextureStore::Take(const std::string& key, PAABlockChain& out)
{
    out = PAABlockChain{};
    if (!Enabled() || key.empty())
        return false;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    auto it = _impl->byKey.find(key);
    if (it == _impl->byKey.end() || !it->second->chain.valid())
    {
        ++_impl->stats.misses;
        return false;
    }
    _impl->EraseLocked(it, &out);
    ++_impl->stats.takes;
    _impl->stats.takeBytes += out.blocks.size();
    return true;
}

bool PreparedTextureStore::PutDdsPrepared(const std::string& key, std::unique_ptr<TextureSourceDDS> source,
                                         const DdsPreparationOptions& options, uint64_t generation)
{
    if (!Enabled() || key.empty() || !source || source->_levels.empty() ||
        source->_mipmaps != static_cast<int>(source->_levels.size()) ||
        Key(static_cast<const char*>(source->_name)) != key ||
        options.decodedMaxEdge < 64 || options.decodedMaxEdge > 8192)
        return false;
    size_t bytes = sizeof(TextureSourceDDS) + source->_levels.capacity() * sizeof(DDSMipLevel);
    for (const auto& level : source->_levels)
        bytes += level.data.capacity();
    std::list<Impl::Entry> pending;
    pending.emplace_back(Impl::Entry{key, {}, {}, Impl::Clock::now()});
    pending.back().preparedDds = std::move(source);
    pending.back().ddsOptions = options;
    pending.back().preparedDdsBytes = bytes;
    std::list<Impl::Entry> retired;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->SweepLocked(retired);
    if (generation != _impl->generation) { ++_impl->stats.rejectedStale; return false; }
    if (_impl->uploaded.count(key)) { ++_impl->stats.skippedUploaded; return false; }
    if (_impl->byKey.count(key)) { ++_impl->stats.rejectedDup; return false; }
    if (bytes > ByteBudget() - std::min(ByteBudget(), _impl->bytes)) {
        ++_impl->stats.rejectedFull;
        return false;
    }
    // Allocate the index before the noexcept splice: an allocation failure must
    // not leave an unindexed payload resident or destroy it under this mutex.
    _impl->byKey.emplace(key, pending.begin());
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
    size_t estimate = sizeof(Impl::DdsDecode) + source.size() + decoded._levels.size() * sizeof(DDSMipLevel);
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
                      snapshot->decoded._levels.capacity() * sizeof(DDSMipLevel);
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
            _impl->EraseLocked(it, nullptr, nullptr, &retired);
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
    std::list<Impl::Entry> freeLater;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->generation;
        freeLater.swap(_impl->order);
        _impl->byKey.clear();
        _impl->uploaded.clear();
        _impl->bytes = 0;
        _impl->stats.entries = 0;
        _impl->stats.bytes = 0;
        _impl->stats.ddsReuseBytes = 0;
        _impl->stats.uploadedMarks = 0;
    }
    freeLater.clear();
}

PreparedTextureStore::Stats PreparedTextureStore::SnapshotStats() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->stats;
}

} // namespace Poseidon::render
