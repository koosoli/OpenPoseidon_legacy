#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Poseidon::render
{

struct PreparedTextureStore::Impl
{
    using Clock = std::chrono::steady_clock;
    struct Entry
    {
        std::string key;
        PAABlockChain chain;    // DXT mip chain...
        DecodedImage image;     // ...OR a whole-file RGBA8 decode (image.valid() discriminates)
        Clock::time_point stored;
    };
    static size_t EntryBytes(const Entry& entry) { return entry.chain.blocks.size() + entry.image.rgba.size(); }
    mutable std::mutex mutex;
    // Insertion order = production order = (through the preparer's queue) distance order, so
    // the front is the oldest and the TTL sweep walks from there and stops early.
    std::list<Entry> order;
    std::unordered_map<std::string, std::list<Entry>::iterator> byKey;
    std::unordered_set<std::string> uploaded;
    size_t bytes = 0;
    Stats stats;

    void EraseLocked(std::unordered_map<std::string, std::list<Entry>::iterator>::iterator it, PAABlockChain* takeInto,
                     DecodedImage* takeImageInto = nullptr)
    {
        std::list<Entry>::iterator node = it->second;
        bytes -= EntryBytes(*node);
        if (takeInto)
            *takeInto = std::move(node->chain);
        if (takeImageInto)
            *takeImageInto = std::move(node->image);
        order.erase(node);
        byKey.erase(it);
        stats.entries = byKey.size();
        stats.bytes = bytes;
    }

    // Drop entries past their TTL. Called under the lock at Put time only, so an idle store
    // costs nothing; a busy one sweeps a handful of stale heads per insert. Freed chains are
    // returned to the caller to release outside the lock.
    void SweepLocked(std::vector<PAABlockChain>& freeLater)
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
            PAABlockChain gone;
            if (it != byKey.end())
                EraseLocked(it, &gone);
            else
            {
                bytes -= EntryBytes(head);
                gone = std::move(order.front().chain);
                order.pop_front();
            }
            freeLater.push_back(std::move(gone));
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

bool PreparedTextureStore::Put(const std::string& key, PAABlockChain&& chain)
{
    if (!Enabled() || key.empty() || !chain.valid())
        return false;
    std::vector<PAABlockChain> freeLater;
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->SweepLocked(freeLater);
        if (_impl->uploaded.count(key))
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

bool PreparedTextureStore::PutDecoded(const std::string& key, DecodedImage&& image)
{
    if (!Enabled() || key.empty() || !image.valid())
        return false;
    std::vector<PAABlockChain> freeLater;
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->SweepLocked(freeLater);
        if (_impl->uploaded.count(key))
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
    if (it == _impl->byKey.end())
    {
        ++_impl->stats.misses;
        return false;
    }
    _impl->EraseLocked(it, &out);
    ++_impl->stats.takes;
    _impl->stats.takeBytes += out.blocks.size();
    return true;
}

void PreparedTextureStore::MarkUploaded(const std::string& key)
{
    if (!Enabled() || key.empty())
        return;
    PAABlockChain gone;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->uploaded.insert(key);
        _impl->stats.uploadedMarks = _impl->uploaded.size();
        auto it = _impl->byKey.find(key);
        if (it != _impl->byKey.end())
            _impl->EraseLocked(it, &gone);
    }
    // `gone` frees here, outside the lock.
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
        freeLater.swap(_impl->order);
        _impl->byKey.clear();
        _impl->uploaded.clear();
        _impl->bytes = 0;
        _impl->stats.entries = 0;
        _impl->stats.bytes = 0;
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
