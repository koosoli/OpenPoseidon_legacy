#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace Poseidon::Streaming
{
// One owner operation only: entries may be borrowed numeric/shape pairs, never
// retained between recentres. Observe during the EXISTING unique warm-model walk.
// The two disjoint windows hold at most64 entries; no world rescan or allocation.
// This provides model capture opportunities, not readiness, complete texture/
// proxy coverage, binding-level fairness or fairness under changing registry order.
template<class Entry>
class WarmModelCaptureWindow
{
public:
    static constexpr size_t Limit = 32;
    struct Result
    {
        size_t total = 0, selected = 0, attempted = 0, nextCursor = 0;
        bool shrinkAdjusted = false;
    };
    explicit WarmModelCaptureWindow(size_t cursor) : _start(cursor) {}
    void Observe(const Entry& entry)
    {
        if (_seen == std::numeric_limits<size_t>::max()) { _overflow = true; return; }
        const size_t ordinal = _seen++;
        if (ordinal < _start)
        { if (_prefixCount < Limit) _prefix[_prefixCount++] = entry; }
        else if (_tailCount < Limit) _tail[_tailCount++] = entry;
    }
    template<class Budget, class Visit>
    Result CaptureWhile(Budget&& budget, Visit&& visit) const
    {
        Result result; result.total = _seen;
        if (!_seen || _overflow) return result;
        const bool shrink = _start >= _seen;
        const size_t start = shrink ? 0 : _start;
        result.shrinkAdjusted = shrink;
        const size_t tail = shrink ? 0 : _tailCount;
        const size_t prefix = std::min(_prefixCount, Limit - tail);
        result.selected = tail + prefix;
        for (size_t i = 0; i < result.selected && budget(); ++i)
        {
            // Count BEFORE capture: empty, refused and thrown captures must not
            // strand this ordinal. The caller's visit keeps ordinary fallback.
            ++result.attempted;
            try { visit(i < tail ? _tail[i] : _prefix[i - tail]); }
            catch (...) { }
        }
        // Advance only ACTUALLY attempted models: one heavy shape may use the
        // whole shared visit budget before the other31 selected entries are tried.
        const size_t toEnd = _seen - start;
        result.nextCursor = result.attempted >= toEnd ? result.attempted - toEnd : start + result.attempted;
        return result;
    }
private:
    size_t _start, _seen = 0, _prefixCount = 0, _tailCount = 0;
    bool _overflow = false;
    std::array<Entry, Limit> _prefix{}, _tail{};
};
}
