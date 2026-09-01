// SPDX-License-Identifier: AGPL-3.0-or-later
#include <Poseidon/Foundation/Threads/WatchDog.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <algorithm>
#include <cstring>

namespace Poseidon
{

std::atomic<WatchDog*> WatchDog::_instance{nullptr};

void WatchDogItem::Finish() noexcept
{
    if (!_slot)
    {
        return;
    }
    Slot* slot = _slot;
    _slot = nullptr;
    // Only clear the slot if it is still OURS. Once the ring wraps past a still-running scope
    // the slot belongs to a newer token, and clearing it here would silence a stall that is
    // genuinely happening, which is the trap here. A relaxed load is enough:
    // the generation is only ever compared for equality against a value we already hold.
    if (slot->generation.load(std::memory_order_relaxed) != _generation)
    {
        return;
    }
    slot->running.store(false, std::memory_order_release);
}

WatchDog::WatchDog(std::chrono::milliseconds timeLimit) : _timeLimit(timeLimit), _slots(RingSize)
{
    // Publish before the thread starts, so a scope entered from the sweep's own startup path
    // cannot see a half-built object.
    _instance.store(this, std::memory_order_release);
    _thread = std::thread(&WatchDog::Sweep, this);
    LOG_INFO(Core, "WatchDog armed: limit {} ms, {} slots", timeLimit.count(), RingSize);
}

WatchDog::~WatchDog() noexcept
{
    _instance.store(nullptr, std::memory_order_release);
    _running.store(false, std::memory_order_release);
    if (_thread.joinable())
    {
        _thread.join();
    }
}

WatchDogItem WatchDog::Add(std::string_view name) noexcept
{
    const size_t index = _nextIndex.fetch_add(1, std::memory_order_relaxed) & RingMask;
    Slot& slot = _slots[index];

    // Bump first: the generation is what the returned token is keyed on, and any token still
    // holding the previous value is invalidated by this store rather than by the ring wrapping
    // silently underneath it.
    const uint32_t generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;

    const size_t n = std::min(name.size(), slot.name.size() - 1);
    std::memcpy(slot.name.data(), name.data(), n);
    slot.name[n] = '\0';
    slot.deadline = std::chrono::steady_clock::now() + _timeLimit;
    // Release: the sweep must not see `running` before the name and deadline it will read.
    slot.running.store(true, std::memory_order_release);

    return WatchDogItem(slot, generation);
}

void WatchDog::Sweep() noexcept
{
    // This deliberately does NOT walk the ring in issue order and stop at the first live
    // scope. That ordering means one long-lived scope hides every stall behind it, which is the
    // opposite of what a stall detector is for. A full pass over 65,536 slots is a few
    // microseconds once per limit; there is nothing to optimise here.
    while (_running.load(std::memory_order_acquire))
    {
        const auto now = std::chrono::steady_clock::now();
        for (Slot& slot : _slots)
        {
            if (!slot.running.load(std::memory_order_acquire))
            {
                continue;
            }
            if (slot.deadline > now)
            {
                continue;
            }
            // Clear before reporting, so a scope that is stalled forever is named once rather
            // than once per sweep. If it later finishes, its token's generation still matches
            // and the (already false) store is harmless.
            slot.running.store(false, std::memory_order_release);
            _reports.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(Core, "WatchDog: scope '{}' exceeded {} ms and has not finished", slot.name.data(),
                     _timeLimit.count());
        }
        std::this_thread::sleep_for(_timeLimit);
    }
}

} // namespace Poseidon
