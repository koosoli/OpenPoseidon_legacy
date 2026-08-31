#include <Poseidon/Core/TickStateHash.hpp>

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace Poseidon::Determinism
{
namespace
{
constexpr std::uint64_t kBasis = 14695981039346656037ull;
constexpr std::uint64_t kPrime = 1099511628211ull;

// FNV-1a over raw bytes -- the same fold as EngineWgpu::HashPublishedFrame,
// and for the same reason: the property needed is "two runs whose state
// differs must disagree", not cryptographic strength.
inline std::uint64_t HashBytes(std::uint64_t h, const void* p, std::size_t n)
{
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= kPrime;
    }
    return h;
}
} // namespace

void TickStateHash::BeginTick()
{
    for (std::size_t i = 0; i < kTickFieldClassCount; i++)
    {
        _acc[i] = kBasis;
        _items[i] = 0;
    }
    for (std::size_t i = 0; i < kTickCounterCount; i++)
    {
        _counters[i] = 0;
    }
    _open = true;
}

void TickStateHash::FoldBytes(TickFieldClass cls, const void* p, std::size_t n)
{
    const auto i = static_cast<std::size_t>(cls);
    // Length first -- HashPublishedFrame's HashSlice discipline. Two adjacent
    // items cannot be confused for one longer one, and an empty item is
    // distinguishable from a missing one.
    const std::uint32_t len = static_cast<std::uint32_t>(n);
    _acc[i] = HashBytes(_acc[i], &len, sizeof(len));
    if (p && n)
    {
        _acc[i] = HashBytes(_acc[i], p, n);
    }
    _items[i]++;
}

void TickStateHash::FoldVec3(TickFieldClass cls, float x, float y, float z)
{
    // As BITS, in one item. memcpy rather than a cast so the fold never reads
    // a float as a value (-0.0f == 0.0f would then hash alike -- exactly the
    // ambiguity a bit fold exists to remove).
    unsigned char bytes[sizeof(float) * 3];
    std::memcpy(bytes + 0 * sizeof(float), &x, sizeof(float));
    std::memcpy(bytes + 1 * sizeof(float), &y, sizeof(float));
    std::memcpy(bytes + 2 * sizeof(float), &z, sizeof(float));
    FoldBytes(cls, bytes, sizeof(bytes));
}

void TickStateHash::SetCounter(TickCounter c, std::uint64_t v)
{
    _counters[static_cast<std::size_t>(c)] = v;
}

TickStateRow TickStateHash::EndTick()
{
    TickStateRow row;
    row.tick = _tick;

    std::uint64_t all = kBasis;
    for (std::size_t i = 0; i < kTickFieldClassCount; i++)
    {
        // Count-suffix: a class that folded 3 items of state A must not hash
        // equal to one that folded 2 items whose concatenation matches.
        std::uint64_t h = HashBytes(_acc[i], &_items[i], sizeof(_items[i]));
        row.classHash[i] = h;
        all = HashBytes(all, &h, sizeof(h));
    }
    for (std::size_t i = 0; i < kTickCounterCount; i++)
    {
        row.counter[i] = _counters[i];
        // TimeMs is the ALIGNMENT key, not state: it advances every tick by
        // construction, so folding it into `all` would give a frozen world a
        // fresh hash per tick and mask exactly the vacuity the distinct count
        // exists to expose. (The first version folded it; the frozen-world
        // test reported distinct == 96 where it meant "nothing moved".)
        if (i == static_cast<std::size_t>(TickCounter::TimeMs))
        {
            continue;
        }
        all = HashBytes(all, &_counters[i], sizeof(_counters[i]));
    }
    row.all = all;

    _distinct.insert(all);
    row.distinct = _distinct.size();

    _open = false;
    _tick++;
    return row;
}

std::string TickStateHash::FormatRow(const TickStateRow& row)
{
    // Fixed column order, stable across builds that are to be diffed. The
    // distinct count rides on every line so a log whose ticks stopped moving
    // announces its own vacuity (SIM-808's rule) without a second pass.
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "TickHash tick=%" PRIu64 " tms=%" PRIu64 " pos=%016" PRIx64 " vel=%016" PRIx64
                  " cam=%016" PRIx64 " rng=%" PRIu64 " shapes=%" PRIu64 " ents=%" PRIu64
                  " clouds=%" PRIu64 " all=%016" PRIx64 " distinct=%" PRIu64,
                  row.tick, row.counter[static_cast<std::size_t>(TickCounter::TimeMs)],
                  row.classHash[static_cast<std::size_t>(TickFieldClass::Positions)],
                  row.classHash[static_cast<std::size_t>(TickFieldClass::Velocities)],
                  row.classHash[static_cast<std::size_t>(TickFieldClass::Camera)],
                  row.counter[static_cast<std::size_t>(TickCounter::RngDraws)],
                  row.counter[static_cast<std::size_t>(TickCounter::ShapeCount)],
                  row.counter[static_cast<std::size_t>(TickCounter::EntityCount)],
                  row.counter[static_cast<std::size_t>(TickCounter::Cloudlets)], row.all, row.distinct);
    return std::string(buf);
}

} // namespace Poseidon::Determinism
