#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace Poseidon
{
class SmokeDensityGrid
{
    std::unordered_map<uint64_t, float> _cells;

  public:
    void Clear(std::size_t capacity) { _cells.clear(); _cells.reserve(capacity); }
    void Add(uint64_t key, float density) { _cells[key] += density; }
    float SampleExcluding(uint64_t key, uint64_t selfKey, float selfDensity) const
    {
        const auto it = _cells.find(key);
        return it == _cells.end() ? 0.0f : std::max(0.0f, it->second - (key == selfKey ? selfDensity : 0.0f));
    }
};
}
