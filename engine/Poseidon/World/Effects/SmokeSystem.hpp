#pragma once

#include <Poseidon/World/Effects/SmokeVolume.hpp>

#include <vector>

namespace Poseidon
{

// ---------------------------------------------------------------------------
// The registry of live smoke plumes, and the A/B harness against the legacy
// system.
//
// The comparison is the reason this is a named subsystem rather than a few
// helpers. "Is the new smoke better?" is not answerable by looking at the new
// smoke; it is answerable by putting the two side by side under one wind, one
// camera and one frame, and reading the same counters off both. So this owns:
//
//   * spawning a new-system plume (SmokeVolume),
//   * spawning a legacy plume (SmokeSourceVehicle, the closest thing the 2001
//     engine has to a standalone emitter),
//   * the per-frame cost counters for both.
//
// The new system is OFF as the game's smoke: nothing in gameplay routes through
// it yet. Both spawn paths are dev-driven, which is what keeps a regression
// here out of a classic mission.
// ---------------------------------------------------------------------------

/// Named colours for smoke-grenade-style plumes. Values are the tint applied to
/// the (white) cloudlet texture, not the perceived colour of the plume — smoke
/// is lit and fogged on top of this, so the constants are more saturated than
/// the result.
enum class SmokeColorPreset
{
    White,
    Black,
    Red,
    Green,
    Blue,
    Yellow,
    Purple,
    Orange
};

const char* SmokeColorPresetName(SmokeColorPreset preset);
void ApplySmokeColorPreset(SmokeParams& params, SmokeColorPreset preset);

/// Per-frame cost, for the dev panel's benchmark section. Sampled rather than
/// accumulated: these are the numbers from the most recent frame in which the
/// volumes actually ticked.
struct SmokeStats
{
    int volumes = 0;
    int particles = 0;
    double simulateMicroseconds = 0.0;
    double drawMicroseconds = 0.0;
    long long sweeps = 0; ///< world queries issued last frame

    /// Legacy cloudlets alive in the world. This counts EVERY legacy cloudlet,
    /// including vehicle dust and weapon clouds, not only the ones spawned from
    /// the dev panel — there is no per-source tagging in the legacy system to
    /// filter on. Read it on an otherwise-static scene.
    int legacyCloudlets = 0;
};

class SmokeSystem
{
  public:
    /// Spawn a new-system plume. Returns an id, or -1 if the world or the
    /// cloudlet shape is unavailable. `duration` < 0 emits forever.
    int Spawn(Vector3Par position, const SmokeParams& params, float duration = -1.0f);

    /// Spawn a legacy SmokeSourceVehicle at `position` for comparison. Returns
    /// an id in the same space as Spawn; despawning works on both.
    int SpawnLegacy(Vector3Par position, float density, float size, float duration);

    /// Stop the emitter and let its particles thin out. Returns false if the id
    /// is unknown. Not an instant delete: a plume that blinks out is a worse
    /// artefact than one that disperses.
    bool Extinguish(int id);

    /// Stop every emitter this system owns, of both kinds.
    void ExtinguishAll();

    /// Drop dead entries. Called once per frame from the dev panel; the volumes
    /// themselves are owned by the world's cloudlet list, so this only prunes
    /// the registry's weak bookkeeping.
    void Prune();

    struct Entry
    {
        int id = -1;
        bool legacy = false;
        OLink<Entity> entity;
    };

    const std::vector<Entry>& Entries() const { return _entries; }

    /// Recompute the counters. Call once per frame while the panel is open;
    /// it walks the live volumes, so it is not free.
    SmokeStats Sample() const;

    /// Reset the world-query counter. Call immediately after Sample() so the
    /// next frame's sweep count is a per-frame number rather than a running
    /// total.
    void BeginFrame();

    /// One particle for the renderer's ground-shadow pass. Kept renderer-
    /// agnostic here (plain floats); EngineWgpu copies it into its own struct.
    struct ShadowBlob
    {
        float x, y, z, radius;
        float density, heightAboveGround;
    };

    /// Gather this frame's shadow-casting particles from every live volume, densest
    /// first, capped at `maxBlobs`. `strength` is the global multiplier the panel
    /// exposes. Cost is a walk over the live particles; called once per frame by
    /// the renderer, not by the panel.
    //! `force` ignores the ground-shadow strength/enable gates. SMK-038 needs the same
    //! particle list for froxel injection, and that has nothing to do with whether the
    //! ground-shadow pass is switched on.
    void CollectShadowBlobs(std::vector<ShadowBlob>& out, std::size_t maxBlobs, bool force = false) const;

    float GroundShadowStrength() const { return _groundShadowStrength; }
    void SetGroundShadowStrength(float strength) { _groundShadowStrength = strength; }

  private:
    std::vector<Entry> _entries;
    int _nextId = 1;
    float _groundShadowStrength = 2.5f;
};

SmokeSystem& GSmokeSystemInstance();

#define GSmokeSystem GSmokeSystemInstance()

} // namespace Poseidon
