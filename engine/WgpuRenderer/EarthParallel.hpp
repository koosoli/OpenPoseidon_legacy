#pragma once
// Whole world: the imagery atlases are built texel by texel on the CPU (EarthSurfaceStream::BuildRegional,
// EarthImageryTiles::Build). On one thread a window's near + far atlases and their source mosaics took
// seconds while the downloaded tiles sat ready, so they are built a band of rows per core.
//
// POSEIDON_EARTH_ATLAS_THREADS: how many threads (default: all but two cores, at most 16; 1 = the old
// single-threaded build, for A/B).
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

namespace Poseidon::EarthParallel
{
inline unsigned Threads()
{
    static const unsigned threads = [] {
        if (const char* v = std::getenv("POSEIDON_EARTH_ATLAS_THREADS"))
        {
            const int n = std::atoi(v);
            if (n >= 1)
                return unsigned(std::min(n, 64));
        }
        const unsigned cores = std::max(std::thread::hardware_concurrency(), 1u);
        return std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 16u);
    }();
    return threads;
}

// Runs row(r) for r in [0, rows), rows handed out one at a time to Threads() workers (the calling
// thread is one of them). Returns false if `cancelled` (checked before each row, so it must be safe to
// call from several threads) said stop; rows already started finish. Below `serialBelow` rows, or with
// one thread, it runs inline in row order, exactly as a plain loop.
inline bool Rows(int rows, const std::function<void(int)>& row, const std::function<bool()>& cancelled = {},
                 int serialBelow = 256)
{
    const unsigned threads = Threads();
    if (threads <= 1 || rows < serialBelow)
    {
        for (int r = 0; r < rows; ++r)
        {
            if (cancelled && cancelled())
                return false;
            row(r);
        }
        return true;
    }
    std::atomic<int> next{0};
    std::atomic<bool> stopped{false};
    const auto work = [&] {
        for (;;)
        {
            if (stopped.load(std::memory_order_relaxed))
                return;
            const int r = next.fetch_add(1, std::memory_order_relaxed);
            if (r >= rows)
                return;
            if (cancelled && cancelled())
            {
                stopped = true;
                return;
            }
            row(r);
        }
    };
    std::vector<std::thread> pool;
    const unsigned extra = std::min<unsigned>(threads - 1, unsigned(rows) - 1);
    pool.reserve(extra);
    for (unsigned i = 0; i < extra; ++i)
        pool.emplace_back(work);
    work();
    for (auto& t : pool)
        t.join();
    return !stopped.load();
}
} // namespace Poseidon::EarthParallel
