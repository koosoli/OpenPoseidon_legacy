#pragma once

// ---------------------------------------------------------------------------
// Ballistics diagnostic — the pure data model.
//
// Deliberately free of every engine type (no Vector3, no RString, no Entity).
// Two reasons:
//
//   1. It is the part with real logic — ring eviction, sample decimation, the
//      drop / lateral-deviation solve — so it is the part worth unit-testing,
//      and a test that has to stand up a World is a test nobody runs.
//   2. It makes the "costs nothing when closed" claim checkable by reading one
//      file: nothing in here allocates or computes unless a track exists, and
//      tracks only exist while the tab is open.
//
// The engine-facing half is BallisticsRecorder.{hpp,cpp}, which samples
// GWorld's fast-vehicle list into this store and draws it.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>

namespace Poseidon::Dev
{

/// One position sample along a trajectory. `t` is seconds since the shot was
/// first seen, NOT mission time — so a track is readable after the fact even
/// if the mission clock was accelerated or paused mid-flight.
struct BallisticSample
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float t = 0.0f;
};

/// How a tracked projectile stopped existing. `Lost` is the honest answer when
/// the projectile left the fast-vehicle list without the impact hook firing
/// (deleted by script, ownership migration, mission end) — the tab must not
/// claim an impact it did not observe.
enum class BallisticTerminus : uint8_t
{
    InFlight = 0,
    HitObject,
    HitGround,
    HitWater,
    Expired, ///< time-to-live ran out (ShotShell::Simulate `_timeToLive < 0`)
    Lost
};

const char* BallisticTerminusName(BallisticTerminus terminus);

/// Fixed-size name field. char[] rather than std::string so a track is a POD-ish
/// blob with no per-shot heap traffic beyond its sample vector.
struct BallisticName
{
    static constexpr int Capacity = 48;
    char text[Capacity] = {};

    void Set(const char* value);
    const char* Get() const { return text; }
    bool Empty() const { return text[0] == '\0'; }
};

/// One recorded shot.
struct BallisticTrack
{
    uint32_t id = 0;
    bool byPlayer = false;

    BallisticName ammo;    ///< AmmoType class name (e.g. "B_762x51_Ball")
    BallisticName shooter; ///< firing entity debug name
    BallisticName hit;     ///< what the terminus struck, when known

    /// Muzzle state, measured from the first sample rather than read from
    /// config: config `initSpeed` is the magazine's nominal figure, while this
    /// is what the projectile actually carries, including the shooter's own
    /// velocity (`_speed + dir * initSpeed` in EntityAI::FireShell).
    float originX = 0.0f, originY = 0.0f, originZ = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 1.0f; ///< unit initial velocity direction
    float muzzleSpeed = 0.0f;                    ///< m/s

    /// Wind as sampled at the moment of fire, plus whether ballistics wind was
    /// actually switched on. Recording `windActive` is the point: without it a
    /// zero lateral deviation is ambiguous between "no wind" and "wind ignored".
    bool windActive = false;
    float windSpeed = 0.0f;
    float windDirRad = 0.0f;

    BallisticTerminus terminus = BallisticTerminus::InFlight;

    /// Sample stride doubles each time the sample budget is exhausted, so a
    /// long-flight shell keeps whole-trajectory shape instead of a truncated head.
    int decimation = 1;
    /// Samples offered since the last one kept (0 .. decimation-1).
    int skipCounter = 0;

    std::vector<BallisticSample> samples;

    // -- derived facts, computed on demand (the tab lists a handful of rows) --

    float TimeOfFlight() const;
    /// Sum of segment lengths — the distance the round actually travelled.
    float PathLength() const;
    /// Straight-line muzzle-to-terminus distance.
    float StraightDistance() const;
    /// Greatest distance the round fell below the un-dropped muzzle ray (m).
    /// Positive means below the line of departure, which is the normal case.
    float MaxDrop() const;
    /// Greatest horizontal deviation from the muzzle ray, signed by the sample
    /// that achieved it (+ = to the shooter's right). With ballistics wind off
    /// this is ~0 by construction, which is exactly what makes it a wind probe.
    float MaxLateral() const;
    /// Terminal speed estimated from the last two samples (m/s), 0 if unknown.
    float TerminalSpeed() const;
};

/// Bounded store of recent shots. Oldest evicted first; nothing here grows
/// without a bound the caller set.
class BallisticsTrackStore
{
  public:
    static constexpr int DefaultCapacity = 16;
    static constexpr int DefaultMaxSamples = 192;

    void SetCapacity(int shots);
    int Capacity() const { return _capacity; }

    void SetMaxSamples(int samples);
    int MaxSamples() const { return _maxSamples; }

    /// Start a track. Returns its id (never 0). Evicts the oldest track if the
    /// store is at capacity.
    uint32_t Begin(const char* ammo, const char* shooter, bool byPlayer, float x, float y, float z, float velX,
                   float velY, float velZ, bool windActive, float windSpeed, float windDirRad);

    /// Append a sample. Ignored when `id` is not a live track. Respects the
    /// current decimation, and halves the sample set when the budget is hit.
    void AddSample(uint32_t id, float x, float y, float z, float t);

    /// Close a track. `hit` may be null.
    void Finish(uint32_t id, BallisticTerminus terminus, const char* hit);

    void Clear();

    int Size() const { return static_cast<int>(_tracks.size()); }
    const BallisticTrack& At(int index) const { return _tracks[static_cast<std::size_t>(index)]; }
    BallisticTrack* Find(uint32_t id);
    const BallisticTrack* Find(uint32_t id) const;

    /// Total samples held across every track — the store's whole memory story.
    int TotalSamples() const;

  private:
    void EvictToCapacity();

    std::deque<BallisticTrack> _tracks;
    int _capacity = DefaultCapacity;
    int _maxSamples = DefaultMaxSamples;
    uint32_t _nextId = 1;
};

} // namespace Poseidon::Dev
