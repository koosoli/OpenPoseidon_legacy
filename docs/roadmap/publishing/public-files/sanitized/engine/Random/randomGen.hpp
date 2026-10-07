#ifndef _RANDOMGEN_HPP
#define _RANDOMGEN_HPP

#include <Poseidon/Foundation/platform.hpp>

class RandomTable
{
  public:
    enum
    {
        Size = 32768
    };

  private:
    short int _table[Size];

  public:
    RandomTable(int seed = 1);
    // returned number is always in range 0..Size-1
    int operator()(int i) const { return _table[i & (Size - 1)]; }
    int Seed(int x, int z, int y) const;
    //! seed combiner - fast (unrolled) version
    int Seed(int x, int z) const;
    //! seed combiner - reference (slow, unoptimized C source) version
    int SeedRef(int x, int z) const;
};

// TWO KINDS OF RANDOM LIVE HERE, and roadmap Phase 8.2 turns on telling them apart
// (design notes).
//
//   POSITIONAL / SEEDED  — RandomValue(seed), RandomValue(x,z), RandomValue(x,z,y),
//     GetSeed(...). Pure functions of their arguments: same input, same output, no
//     shared state touched. Safe to call from any thread, in any order, and the
//     result cannot depend on who got there first. This is the pattern the roadmap
//     asks new parallel work to use, and it is already how terrain generation works
//     (Landscape::InitRandomization precomputes a positional table that
//     GenerateSegmentInto then reads on TaskPool workers -- no live RNG involved).
//
//   SEQUENTIAL / STREAM  — RandomValue(), Gauss(), PlusMinus(), SetSeed(). These
//     advance `_seed`, a single shared mutable stream. Calling them from two threads
//     at once is a data race on that int AND makes the values depend on scheduler
//     order, which is exactly what Phase 8 must not allow to appear by accident.
//
// AUDITED 2026-08-31 (276 call sites): no worker thread reaches the sequential path
// today. Terrain generation is positional as described; the streamed-object preparer
// workers (parse + ShapeAdapter conversion) contain no RNG call at all. The guard
// below exists so that stays true by construction rather than by luck: the first
// worker-thread call to a sequential entry point says so, loudly, once.
class RandomGenerator
{
    mutable int _seed;
    // SIM-815: cumulative count of sequential draws. The tick-hash recorder
    // (Core/TickStateHash) logs it per fixed step, so two runs whose consumers
    // took a DIFFERENT NUMBER of draws in a tick name that tick directly --
    // the draw-count coupling (smoke spawn cadence through the LOD governor,
    // radio pacing through audio completion) is the failure mode this counts.
    // One increment on a path that already runs an atomic CAS; not gated.
    mutable unsigned long long _sequentialDraws = 0;
    RandomTable _seedTable;
    RandomTable _valueTable;

    // Reports (once) when a sequential entry point is used off the thread that first
    // used it -- in practice the main thread, which is where the engine's simulation
    // and the legacy call sites live. Not a lock: making the stream thread-safe would
    // hide the ordering problem rather than surface it, and the fix Phase 8.2 wants is
    // a per-system stream or a positional seed at the CALL SITE, not a mutex here.
    static void NoteSequentialUse();

  public:
    RandomGenerator(int seed1, int seed2);
    RandomGenerator();
    // --- positional: pure, thread-safe, order-independent ---
    float RandomValue(int seed) const;
    float RandomValue(int x, int z) const { return RandomValue(GetSeed(x, z)); }
    float RandomValue(int x, int z, int y) const { return RandomValue(GetSeed(x, z, y)); }
    // --- sequential: shared mutable stream, MAIN THREAD ONLY (see the note above) ---
    void SetSeed(int seed)
    {
        NoteSequentialUse();
        _seed = seed;
    }
    float RandomValue() const;
    float Gauss(float min, float mid, float max) const;
    float PlusMinus(float a, float b) const;

    // SIM-815: how many sequential draws have been taken since process start.
    // Gauss and PlusMinus route through RandomValue(), so this counts all three
    // entry points. Read by the tick-hash recorder; safe to read anywhere.
    unsigned long long SequentialDraws() const { return _sequentialDraws; }

    __forceinline int GetSeed(int x, int z) const { return _seedTable.Seed(x, z); }
    __forceinline int GetSeedRef(int x, int z) const { return _seedTable.SeedRef(x, z); }
    __forceinline int GetSeed(int x, int z, int y) const { return _seedTable.Seed(x, z, y); }
};

// Meyers singleton accessor — constructed on first use, no static-init-order hazard.
inline RandomGenerator& GRandGen()
{
    static RandomGenerator instance;
    return instance;
}

// Let call sites spell the singleton as `GRandGen.Foo()`; expands to `GRandGen().Foo()`.
#define GRandGen GRandGen()

#endif
