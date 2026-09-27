#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

namespace Poseidon::render
{
// Pure byte memoization, never file/header/source freshness. No payload pointer
// escapes: comparison, output copy and admission copies hold one process-cache
// mutex; decompression itself is the caller's operation outside this mutex.
class PaaLzoReplayCache
{
public:
    using Hash = uint64_t (*)(const uint8_t*,size_t);
    struct Stats {
        uint64_t hits=0,misses=0,hitOutputBytes=0,evictions=0,oversize=0,admissionFailures=0;
        size_t storeBytes=0,peakStoreBytes=0,entries=0;
    };
    static constexpr size_t MaxEntries=128;
    // Charged known C++ fixed storage + exact requested payload bytes; excludes
    // allocator headers, native memory and all caller decode/output allocations.
    static constexpr size_t KnownMetadataBytes() { return sizeof(PaaLzoReplayCache); }
    explicit PaaLzoReplayCache(size_t budget=32u*1024u*1024u,size_t maxEntries=MaxEntries,Hash hash=nullptr)
        : _budget(budget),_maxEntries(maxEntries<MaxEntries?maxEntries:MaxEntries),_hash(hash?hash:&HashBytes)
    { _stats.storeBytes=KnownMetadataBytes(); _stats.peakStoreBytes=_stats.storeBytes; }
    bool TryCopy(const uint8_t* input,size_t inputBytes,size_t expected,uint8_t* output)
    {
        if(!input || !inputBytes || !expected || !output) return false;
        if(!CanRetain(inputBytes,expected)) { std::lock_guard<std::mutex> lock(_mutex); Add(_stats.misses,1); return false; }
        const uint64_t hash=_hash(input,inputBytes);
        std::lock_guard<std::mutex> lock(_mutex);
        for(auto& e:_entries) if(Matches(e,hash,input,inputBytes,expected)) {
            std::memcpy(output,e.bytes.get()+inputBytes,expected); e.age=NextAge();
            Add(_stats.hits,1); Add(_stats.hitOutputBytes,expected); return true;
        }
        Add(_stats.misses,1); return false;
    }
    // Called only after the unchanged decoder. Failure/partial output never enters
    // cache; admission refusal must not change the caller's successful result.
    bool StoreSuccessful(const uint8_t* input,size_t inputBytes,size_t expected,const uint8_t* output,size_t got)
    {
        if(!input || !inputBytes || !output || !expected || got!=expected) return false;
        if(!CanRetain(inputBytes,expected)) { std::lock_guard<std::mutex> lock(_mutex); Add(_stats.oversize,1); return false; }
        const uint64_t hash=_hash(input,inputBytes);
        std::lock_guard<std::mutex> lock(_mutex);
        for(auto& e:_entries) if(Matches(e,hash,input,inputBytes,expected)) { e.age=NextAge(); return true; }
        const size_t charge=inputBytes+expected;
        // Evict before allocation. No outside pins can retain an evicted block.
        while(_stats.entries>=_maxEntries || charge>_budget-_stats.storeBytes) EvictOldest();
        auto block=std::unique_ptr<uint8_t[]>(new(std::nothrow) uint8_t[charge]);
        if(!block) { Add(_stats.admissionFailures,1); return false; }
        std::memcpy(block.get(),input,inputBytes); std::memcpy(block.get()+inputBytes,output,expected);
        for(auto& e:_entries) if(!e.bytes) {
            e.bytes=std::move(block); e.hash=hash; e.input=inputBytes; e.output=expected; e.age=NextAge();
            ++_stats.entries; _stats.storeBytes+=charge;
            if(_stats.storeBytes>_stats.peakStoreBytes) _stats.peakStoreBytes=_stats.storeBytes;
            return true;
        }
        Add(_stats.admissionFailures,1); return false;
    }
    Stats Snapshot() const { std::lock_guard<std::mutex> lock(_mutex); return _stats; }
private:
    bool CanRetain(size_t inputBytes,size_t expected) const {
        return _maxEntries && KnownMetadataBytes()<=_budget && inputBytes<=std::numeric_limits<size_t>::max()-expected &&
            inputBytes+expected<=_budget-KnownMetadataBytes();
    }
    struct Entry { std::unique_ptr<uint8_t[]> bytes; uint64_t hash=0,age=0; size_t input=0,output=0; };
    // A bounded shortlist fingerprint, never an exact-content certificate.
    // Matches still compares EVERY input byte before replay or deduplication.
    static uint64_t HashBytes(const uint8_t* input,size_t count) {
        uint64_t hash=14695981039346656037ull;
        const auto feed=[&](uint8_t byte) { hash^=byte; hash*=1099511628211ull; };
        uint64_t length=static_cast<uint64_t>(count);
        for(unsigned i=0;i<8;++i) { feed(static_cast<uint8_t>(length)); length>>=8; }
        if(count<=192) {
            for(size_t i=0;i<count;++i) feed(input[i]);
        } else {
            // Subtraction before division/addition avoids offset overflow.
            const size_t middle=(count-64)/2, last=count-64;
            for(size_t i=0;i<64;++i) feed(input[i]);
            for(size_t i=0;i<64;++i) feed(input[middle+i]);
            for(size_t i=0;i<64;++i) feed(input[last+i]);
        }
        return hash;
    }
    static bool Matches(const Entry& e,uint64_t hash,const uint8_t* input,size_t count,size_t expected) {
        return e.bytes && e.hash==hash && e.input==count && e.output==expected && std::memcmp(e.bytes.get(),input,count)==0;
    }
    static void Add(uint64_t& value,uint64_t amount) {
        value=amount>std::numeric_limits<uint64_t>::max()-value?std::numeric_limits<uint64_t>::max():value+amount;
    }
    uint64_t NextAge() {
        if(_age==std::numeric_limits<uint64_t>::max()) { for(auto& e:_entries) e.age=0; _age=0; }
        return ++_age;
    }
    void EvictOldest() {
        Entry* oldest=nullptr;
        for(auto& e:_entries) if(e.bytes && (!oldest || e.age<oldest->age)) oldest=&e;
        if(!oldest) return;
        _stats.storeBytes-=oldest->input+oldest->output; --_stats.entries;
        oldest->bytes.reset(); Add(_stats.evictions,1);
    }
    const size_t _budget,_maxEntries;
    const Hash _hash;
    mutable std::mutex _mutex;
    std::array<Entry,MaxEntries> _entries{};
    Stats _stats;
    uint64_t _age=0;
};
}
