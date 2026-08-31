#include <Random/randomGen.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>

#include <Random/isaac.hpp>

// `DWORD` for the `GlobalTickCount` extern below: on Linux it's
// typedef'd in `platform.hpp`'s Linux branch; on Windows it comes
// from `<windows.h>` via `win.h` (which also undefs GDI macros
// like `DrawText` / `GetObject` that would otherwise pollute
// engine method names).
#ifdef _WIN32
#include <Poseidon/Foundation/Common/Win.h>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <atomic>
#include <thread>
#endif

// Phase 8.2 guard: the sequential entry points share one mutable stream, so using
// them from two threads is both a data race and a scheduler-order dependency. The
// first thread to arrive is recorded as the owner (the main thread in every path
// audited on 2026-08-31); any later thread reports itself once. Deliberately a
// report and not a lock -- see the header for why serialising would hide the bug.
void RandomGenerator::NoteSequentialUse()
{
    static std::atomic<std::thread::id> owner{std::thread::id()};
    static std::atomic<bool> reported{false};
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id expected{};
    if (owner.compare_exchange_strong(expected, self, std::memory_order_relaxed))
        return; // first caller: this is the owning thread
    if (expected == self || reported.load(std::memory_order_relaxed))
        return;
    if (!reported.exchange(true, std::memory_order_relaxed))
    {
        LOG_WARN(Core,
                 "RandomGenerator: the SEQUENTIAL random stream was used from a second thread. "
                 "It is one shared mutable seed, so this is a data race and makes results depend "
                 "on scheduler order. Use the positional form (RandomValue(x,z[,y])) or give the "
                 "system its own stream -- see roadmap 8.2 / SIM-802.");
    }
}

float RandomGenerator::RandomValue(int seed) const
{
    return _valueTable(_seedTable(seed)) * (1.0 / static_cast<double>(RandomTable::Size));
}

float RandomGenerator::RandomValue() const
{
    return _valueTable(_seed++) * (1.0 / static_cast<double>(RandomTable::Size));
}

int RandomTable::Seed(int x, int z, int y) const
{
    // make x, z out of order
    const int mask = Size - 1;
    x = _table[x & mask];
    z = _table[z & mask];
    y = _table[y & mask];
    // bitwise interleave x, z, and y
    int xz = 0;
    // only 12 bits is significant
    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;
    xz <<= 1, xz |= y & 1, y >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;
    xz <<= 1, xz |= y & 1, y >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;
    xz <<= 1, xz |= y & 1, y >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;
    xz <<= 1, xz |= y & 1, y >>= 1;
    return xz;
}

int RandomTable::Seed(int x, int z) const
{
    // make x, z out of order
    enum
    {
        mask = Size - 1
    };
    // bitwise interleave x and z
    int xz = 0;
    // only 12 bits is significant
    x = _table[x & mask];
    z = _table[z & mask];

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;

    xz <<= 1, xz |= x & 1, x >>= 1;
    xz <<= 1, xz |= z & 1, z >>= 1;

    return xz;
}

int RandomTable::SeedRef(int x, int z) const
{
    // slow reference implementation
    // make x, z out of order
    enum
    {
        mask = Size - 1
    };
    // bitwise interleave x and z
    int xz = 0;
    // only 12 bits is significant
    x = _table[x & mask];
    z = _table[z & mask];

    for (int i = 0; i < 6; i++)
    {
        xz <<= 1, xz |= x & 1, x >>= 1;
        xz <<= 1, xz |= z & 1, z >>= 1;
    }

    return xz;
}

const int A = 48271;
const int M = 0x7fffffff;
const int Q = M / A;
const int R = M % A;

[[maybe_unused]] static int RandomInt(int& seed)
{
    seed &= M; // make seed positive
    int seedDivQ = seed / Q;
    int seedModQ = seed - seedDivQ * Q;

    // seed = A * ( seed % Q ) - R * ( seed / Q );
    seed = A * seedDivQ - R * seedModQ;

    seed &= M; // make seed positive
    return seed;
}

struct RandomOrder
{
    int index;
    int value;
};

[[maybe_unused]] static int CmpRandomOrder(const void* o0, const void* o1)
{
    const RandomOrder* r0 = static_cast<const RandomOrder*>(o0);
    const RandomOrder* r1 = static_cast<const RandomOrder*>(o1);
    return r0->value - r1->value;
}

RandomTable::RandomTable(int seed)
{
    QTIsaac<> gen;
    gen.srand(seed);
    for (int i = 0; i < Size; i++)
    {
        _table[i] = gen.rand() & (Size - 1);
    }
}

// Seeded construction yields a reproducible generator, e.g. for landscape bumps.
RandomGenerator::RandomGenerator(int seed1, int seed2) : _seed(seed2), _seedTable(seed1), _valueTable(120) {}

RandomGenerator::RandomGenerator() : _seed(Poseidon::Foundation::GlobalTickCount() + 3256), _seedTable(Poseidon::Foundation::GlobalTickCount()), _valueTable(120) {}

float RandomGenerator::Gauss(float min, float mid, float max) const
{
    float gauss = (RandomValue() + RandomValue() + RandomValue() + RandomValue()) * 0.25;
    float delay = 0;
    if (gauss < 0.5)
    {
        float coef = gauss * 2;
        delay = min + (mid - min) * coef;
    }
    else
    {
        float coef = gauss * 2 - 1;
        delay = mid + (max - mid) * coef;
    }
    return delay;
}

float RandomGenerator::PlusMinus(float a, float b) const
{
    return a - b + 2.0f * b * RandomValue();
}
