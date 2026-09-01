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

WatchDogItem WatchScopeFor(std::string_view label, std::string_view detail) noexcept
{
    WatchDog* dog = WatchDog::Instance();
    if (!dog)
    {
        return WatchDogItem();
    }

    // One slot's worth of name, built on the stack: 31 usable characters plus the
    // terminator Add appends.
    constexpr size_t kMax = 31;
    char buffer[kMax + 1];
    size_t used = std::min(label.size(), kMax);
    std::memcpy(buffer, label.data(), used);
    if (used < kMax)
    {
        buffer[used++] = ':';
    }
    const size_t room = kMax - used;
    // Keep the END of the detail (see the header): the file name is what identifies the
    // stall, the directory prefix is shared by everything.
    const size_t take = std::min(detail.size(), room);
    std::memcpy(buffer + used, detail.data() + (detail.size() - take), take);
    used += take;

    return dog->Add(std::string_view(buffer, used));
}

void WatchDog::Sweep() noexcept
{
    // This deliberately does NOT walk the ring in issue order and stop at the first live
    // scope. That ordering means one long-lived scope hides every stall behind it, which is the
    // opposite of what a stall detector is for. A full pass over 65,536 slots is a few tens of
    // microseconds; there is nothing to optimise here.
    //
    // The pass runs at a fixed short cadence rather than once per limit, and the cadence is
    // deliberately decoupled from the deadline. Each slot carries an absolute deadline, so
    // sweeping more often only makes a report land sooner -- and at the 30-second limit the
    // client arms, once-per-limit sweeping would name a stall anywhere from 30 to 60 seconds
    // after the fact, which is too vague to line up against anything else in the log. It also
    // bounds shutdown: the destructor joins this thread, and sleeping a whole limit would make
    // every quit pay the rare case's latency.
    constexpr auto kSweepInterval = std::chrono::milliseconds(200);
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
        // Never sleep past the limit itself: the unit tests arm limits of a few tens of
        // milliseconds, and a fixed 200 ms cadence would make a 40 ms deadline take five
        // sweeps' worth of wall clock to notice.
        std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(kSweepInterval, _timeLimit));
    }
}

} // namespace Poseidon
