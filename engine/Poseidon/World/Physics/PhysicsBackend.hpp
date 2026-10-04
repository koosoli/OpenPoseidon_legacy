#pragma once

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/World/Physics/PhysicsTypes.hpp>

#include <memory>
#include <vector>

// The replaceable half. One implementation exists today (Box3D); PHY-001 names
// Jolt as the fallback and lists three points at which that choice is revisited.
// Everything the engine sees is above this line and does not change with it.

class LODShape;

namespace Poseidon::Physics
{

/// Geometry handed to the backend for one convex piece.
///
/// Points, not indices: OFP Geometry LODs are already authored as named convex
/// components (`Component01`, `Component02`, ...), so the expensive part of this
/// work -- convex decomposition -- is already done in the data and the backend
/// only has to hull each piece. PHY-001 §1.2 measured that; it is the reason the
/// binding is small.
struct ConvexPiece
{
    const Vector3* points = nullptr;
    int            count = 0;
    /// What this piece is for, read off the special LOD it came from. One body can
    /// carry pieces with different flags -- that is the point: a model that
    /// authored a separate Fire Geometry LOD contributes both, and a query then
    /// picks. Defaults to Solid so a caller that has no opinion (a probe, a test)
    /// gets the one undifferentiated collider it used to get.
    ColliderFlags flags = ColliderFlags::Solid;
};

/// A regular heightfield, row-major, `width * height` samples spaced `cellSize`
/// apart, with sample (0,0) at world `originX, originZ`.
struct TerrainField
{
    const float* heights = nullptr;
    int          width = 0;
    int          height = 0;
    float        cellSize = 0.0f;
    float        originX = 0.0f;
    float        originZ = 0.0f;
};

/// What a backend's OWN record/replay facility found. See SIM-808.
///
/// This is a second, independent witness and nothing else. The engine's own
/// divergence report (`Poseidon/Core/StateTimeline.hpp`) sees only what the three
/// scoped readbacks above hand out; a backend's internal replay sees the state
/// underneath them -- contacts, islands, sleep sets, the constraint graph -- and
/// can replay the same recorded call stream under a configuration the engine
/// cannot otherwise reach. It is strictly narrower in SUBJECT (physics only, and
/// only given the inputs the engine already chose) and strictly wider in DEPTH.
///
/// Deliberately not a getter of anything: nothing here flows into gameplay, and
/// `Begin` is a diagnostic switch a test throws, not something a frame does.
struct ReplayAudit
{
    /// False when the backend has no replay facility. Not a failure.
    bool supported = false;
    /// A recording existed and was replayed to completion (or to a divergence).
    bool ran = false;
    /// A recorded state hash did not match on replay.
    bool diverged = false;
    /// The FIRST frame whose state hash differed. -1 when none did.
    int divergentFrame = -1;
    /// Frames the replay reached.
    int frameCount = 0;
    /// Worker count the replay ran at, as the backend clamped it.
    int replayWorkerCount = 0;
    /// Recorded stream size, so the cost of this facility is a measured number
    /// rather than an impression.
    int recordedBytes = 0;
};

class PhysicsBackend
{
public:
    virtual ~PhysicsBackend() = default;

    /// Begin recording the backend's own call stream, if it can. Returns false
    /// when the backend has no such facility, which is a supported state.
    virtual bool BeginReplayRecording() { return false; }

    /// Stop recording, replay the stream into a fresh world at `replayWorkerCount`
    /// workers, and report the first state-hash mismatch. `supported == false`
    /// when the backend has no facility; `ran == false` when nothing was recorded.
    virtual ReplayAudit EndReplayRecordingAndValidate(int replayWorkerCount)
    {
        (void)replayWorkerCount;
        return {};
    }

    virtual bool Create() = 0;
    virtual void Destroy() = 0;

    /// Advance the simulation. Nothing reads results back into gameplay in
    /// PHY-010 -- Poseidon stays authoritative over every position. Stepping is
    /// here so the world is a real simulation that can be inspected, not so it
    /// can start driving anything.
    virtual void Step(float deltaSeconds) = 0;
    virtual void SetWaterEnvironment(const ProbeWaterEnvironment&) {}

    /// One static body carrying `pieces` convex hulls, placed at `transform`.
    /// Returns an invalid id if every piece was refused.
    virtual BodyId AddStaticBody(const ConvexPiece* pieces, int pieceCount, const Matrix4& transform) = 0;

    virtual void RemoveBody(BodyId id) = 0;
    virtual BodyId SpawnArticulatedPiece(const ConvexPiece&, const Matrix4&, float,
        const ArticulatedInitialMotion& = {}) { return {}; }
    virtual bool SetArticulatedMotion(BodyId, Vector3Par, Vector3Par) { return false; }
    virtual JointId AddArticulatedJoint(const ArticulatedJointDef&) { return {}; }
    virtual void RemoveJoint(JointId) {}

    /// Transforms flow IN only (PHY-010). There is deliberately no getter: adding
    /// one is how "physics is a query service" quietly becomes "physics owns the
    /// position", which is the step PHY-001 defers.
    virtual void SetBodyTransform(BodyId id, const Matrix4& transform) = 0;

    virtual bool SetTerrain(const TerrainField& field) = 0;
    // Optional editor path. First patch may partition terrain; later patches are local.
    virtual bool PatchTerrain(int x, int z, int width, int height, const float* samples) { return false; }

    /// PHY-020. A dynamic sphere the physics world owns outright.
    virtual BodyId SpawnSphereProbe(const SphereProbeDef& def) = 0;
    /// A dynamic body built from convex pieces -- the same pieces a static body
    /// takes, so a model's authored Geometry LOD can simply be made to move.
    virtual BodyId SpawnDynamicPieces(const ConvexPiece* pieces, int pieceCount, const Matrix4& transform,
                                      float mass, float friction, float restitution, Vector3Par velocity) = 0;
    /// Push a body at a point. Gameplay -> physics, which is the direction
    /// PHY-010 already allows; nothing comes back.
    virtual void ApplyImpulse(BodyId id, Vector3Par point, Vector3Par impulse) = 0;
    /// Ray against the physics world; returns the body hit, invalid if none.
    ///
    /// The filter is a PARAMETER OF THE SEAM and not a post-filter above it. A
    /// caller cannot reach past this to a flag word on the hit: the closest hit
    /// under a filter is not the closest hit overall, so filtering after the fact
    /// would answer a different question and quietly return a miss whenever the
    /// nearest surface was one the caller did not ask about.
    virtual BodyId CastRay(Vector3Par from, Vector3Par to, Vector3& hitPoint, const QueryFilter& filter) const = 0;
    virtual bool   RayHitAnything(Vector3Par from, Vector3Par to, const QueryFilter& filter) const = 0;
    virtual void   ClearProbes() = 0;
    /// Probe positions for debug drawing only. Scoped to probe bodies on purpose:
    /// a general transform getter is what turns "physics is a query service" into
    /// "physics owns the position", and PHY-020 is not that step.
    virtual void GetProbeSamples(std::vector<ProbeSample>& out) const = 0;
    /// One body's pose and motion. False when the id is stale or the backend has
    /// no such body. See BodyMotion for why reading back is allowed at all.
    virtual bool GetBodyMotion(BodyId id, BodyMotion& out) const = 0;
    // Actual upward contact with a static solid; a nearby ray or wall contact
    // does not establish support. False also covers unavailable/stale bodies.
    virtual bool GetArticulatedGroundSupport(BodyId, Vector3&) const { return false; }
    /// Applied to every probe shape at the start of each step while enabled.
    virtual void SetWind(const WindSettings& wind) = 0;

    /// A KINEMATIC capsule the caller moves every tick. Kinematic, not dynamic:
    /// it pushes dynamic bodies and is pushed by nothing, so the player keeps
    /// full authority over their own position and physics gains none. Pass a
    /// zero height to remove it.
    virtual void SetKinematicProxy(Vector3Par position, float radius, float height) = 0;

    [[nodiscard]] virtual PhysicsStats GetStats() const = 0;

    /// Backend name for logs and the dev panel, e.g. "box3d 0.1.0".
    [[nodiscard]] virtual const char* Name() const = 0;
};

/// Builds the configured backend. Returns null when none is compiled in, which is
/// a supported state: the engine runs without a physics world.
std::unique_ptr<PhysicsBackend> CreatePhysicsBackend();

} // namespace Poseidon::Physics
