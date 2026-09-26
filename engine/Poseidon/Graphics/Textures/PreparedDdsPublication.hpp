#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Poseidon::render
{
// Cancellation is logical visibility, independent of payload/store lifetime.
// Only optional native DDS or PBO texture preparation allocates this bounded inventory. Workers
// and cached sources may retain it after Reset; no Shape or pixel data lives here.
class DdsPublicationInventory
{
  public:
    static constexpr size_t MaxModels = 65536;
    static std::shared_ptr<DdsPublicationInventory> Create(size_t count)
    {
        if (count > MaxModels) return nullptr;
        return std::shared_ptr<DdsPublicationInventory>(new DdsPublicationInventory(count));
    }
    void InvalidateAll() { _alive.store(false, std::memory_order_release); }
    void CancelModel(size_t index)
    {
        if (index >= _epochs.size()) return;
        auto previous = _epochs[index].load(std::memory_order_relaxed);
        for (;;)
        {
            if (previous == std::numeric_limits<uint64_t>::max())
            {
                InvalidateAll(); // Never wrap into an older token's epoch.
                return;
            }
            if (_epochs[index].compare_exchange_weak(previous, previous + 1,
                    std::memory_order_release, std::memory_order_relaxed)) return;
        }
    }
    uint64_t Epoch(size_t index) const
    { return index < _epochs.size() ? _epochs[index].load(std::memory_order_acquire) : 0; }
    bool Valid(size_t index, uint64_t epoch) const
    {
        return epoch != 0 && _alive.load(std::memory_order_acquire) &&
               index < _epochs.size() && _epochs[index].load(std::memory_order_acquire) == epoch;
    }
    size_t MetadataBytes() const
    { return sizeof(*this) + _epochs.capacity() * sizeof(std::atomic<uint64_t>); }
  private:
    explicit DdsPublicationInventory(size_t count) : _epochs(count)
    { for (auto& epoch : _epochs) epoch.store(1, std::memory_order_relaxed); }
    std::atomic<bool> _alive{true};
    std::vector<std::atomic<uint64_t>> _epochs;
};

struct DdsPublicationToken
{
    std::shared_ptr<const DdsPublicationInventory> inventory;
    size_t index = 0;
    uint64_t epoch = 0;
    bool Valid() const { return inventory && inventory->Valid(index, epoch); }
};

// Copied into one preparer before workers start. The context must outlive its
// destructor (which joins workers). Runs outside all queue/store locks, after
// final job validation and before Put. Receives no borrowed source/pixel data.
// Exceptions use the same optional-preparation failure/debt-release path as I/O.
struct DdsPublicationObserver
{
    void (*beforePut)(void*, bool hasBc3Sidecar) = nullptr;
    void* context = nullptr;
    // PAA conversion-worker publication, same lock/lifetime rules. Appended to
    // preserve existing DDS observer aggregate initialisers and callback meaning.
    void (*beforePaaPut)(void*, bool hasAlphaFacts) = nullptr;
};
} // namespace Poseidon::render
