#pragma once

#include <cstddef>
#include <cstdint>

namespace Poseidon::Streaming
{
// Owner-only decision for an optional last-holder Shape cache. The decision frees only
// cache references; it does not certify a physical GPU allocation was reclaimed.
class ShapeCachePressurePolicy
{
public:
    static constexpr uint8_t DwellUpdates = 8;

    void Reset() noexcept { _high = 0; _low = 0; _active = false; }
    bool Active() const noexcept { return _active; }

    bool Observe(bool available, uint64_t trackedBytes, uint64_t budgetBytes) noexcept
    {
        if (!available || budgetBytes == 0)
        {
            Reset(); // no stale memory sample may keep suppressing cache insertions
            return false;
        }
        // Compare strictly below 85% without an overflowing budget * 85 expression.
        const uint64_t lowWhole = (budgetBytes / 100) * 85;
        const uint64_t lowRemainder = (budgetBytes % 100) * 85;
        const uint64_t lowFloor = lowWhole + lowRemainder / 100;
        const bool belowLow = trackedBytes < lowFloor ||
            (trackedBytes == lowFloor && lowRemainder % 100 != 0);
        if (trackedBytes > budgetBytes)
        {
            _low = 0;
            if (_high < DwellUpdates) ++_high;
            if (_high == DwellUpdates) _active = true;
        }
        else if (belowLow)
        {
            _high = 0;
            if (_low < DwellUpdates) ++_low;
            if (_low == DwellUpdates) _active = false;
        }
        else
        {
            _high = 0;
            _low = 0;
        }
        return _active;
    }

    size_t DropCount(size_t cachedShapes) const noexcept
    {
        return _active && cachedShapes != 0 ? 1 : 0;
    }

private:
    uint8_t _high = 0, _low = 0;
    bool _active = false;
};
} // namespace Poseidon::Streaming
