// Roadmap 8.4 -- the physics integration boundary. See
// design notes
//
// 8.4 says: "Do not assume the physics backend is safe or profitable to query
// concurrently merely because the engine has worker threads." Nothing in the tree
// queries it concurrently today, so this file does NOT test a live hazard. It
// tests the properties that a future move off the main thread would silently
// break, on the same principle SIM-806 used: exercise the path nothing calls,
// because that is the one nobody has evidence about.
//
// The method is SIM-806's, deliberately reused rather than reinvented -- FNV-1a
// over raw IEEE-754 BITS of the complete readback, strict equality, same build,
// same platform. Bits and not values: `-0.0f == 0.0f` and `NaN != NaN`, so a value
// comparison is simultaneously too loose and too tight to be a determinism check.
//
// Two anti-vacuity guards, also SIM-806's: a determinism test that agrees because
// nothing moved, or because everything hashed alike, passes for ever and means
// nothing.
//
// No game data required.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>

#include <cfenv>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define PHYS_DET_HAS_SSE 1
#include <xmmintrin.h>
#else
#define PHYS_DET_HAS_SSE 0
#endif

using namespace Poseidon::Physics;
namespace Fp = Poseidon::Foundation;

namespace
{

// ---------------------------------------------------------------------------
// Bit hasher. Copied from SIM-806's harness rather than shared with it: the two
// hash different things and a common base class would have to grow a type
// parameter for no gain. See SIM-806 "What is NOT covered" -- its pieces are
// file-local by design so the next author copies them deliberately.

struct Hasher
{
    std::uint64_t h = 1469598103934665603ull; // FNV-1a offset basis

    void Byte(std::uint8_t b)
    {
        h ^= b;
        h *= 1099511628211ull;
    }
    void Raw(const void* p, std::size_t n)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i)
        {
            Byte(bytes[i]);
        }
    }
    /// The float's BITS, never its value.
    void F(float v)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        Raw(&bits, sizeof(bits));
    }
    void U(std::uint32_t v) { Raw(&v, sizeof(v)); }
    void V(const Vector3& v)
    {
        F(v[0]);
        F(v[1]);
        F(v[2]);
    }
};

/// Everything the boundary will hand out about one probe. Hashed WHOLE, because
/// the gate asks for the full output and a spot check is how a divergence in the
/// half nobody sampled survives.
void HashSample(Hasher& hash, const ProbeSample& s)
{
    hash.U(s.id.index);
    hash.U(s.id.generation);
    hash.U(static_cast<std::uint32_t>(s.shape));
    hash.V(s.position);
    hash.V(s.axisX);
    hash.V(s.axisY);
    hash.V(s.axisZ);
    hash.V(s.halfExtents);
    hash.F(s.halfLength);
    hash.F(s.radius);
    hash.V(s.axis);
}

void HashMotion(Hasher& hash, const BodyMotion& m)
{
    hash.V(m.position);
    hash.V(m.axisX);
    hash.V(m.axisY);
    hash.V(m.axisZ);
    hash.V(m.linearVelocity);
    hash.V(m.angularVelocity);
    hash.Byte(m.awake ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// The fixture.
//
// A cluster, not a scatter. Probes that never touch each other measure only
// integration of gravity, which is deterministic in any solver; the interesting
// state is the CONTACT GRAPH, because that is what a worker count re-partitions
// (box3d's own header says so of its replay player: "Replaying at a different
// count re-partitions the constraint graph") and what an allocator- or
// pointer-ordered iteration would reorder.

constexpr int   kTerrainN = 16;
constexpr float kCellSize = 10.0f;
constexpr float kStepSeconds = 1.0f / 60.0f;
constexpr int   kSteps = 240; // 4 s: long enough to fall, collide, and settle

struct Fixture
{
    std::unique_ptr<PhysicsBackend> backend;
    std::vector<float>              heights;
    std::vector<Vector3>            cubePoints;
    std::vector<BodyId>             probes;
};

std::vector<Vector3> CubeAt(float cx, float cy, float cz, float half)
{
    return {
        {cx - half, cy - half, cz - half}, {cx + half, cy - half, cz - half},
        {cx + half, cy + half, cz - half}, {cx - half, cy + half, cz - half},
        {cx - half, cy - half, cz + half}, {cx + half, cy - half, cz + half},
        {cx + half, cy + half, cz + half}, {cx - half, cy + half, cz + half},
    };
}

/// `nudge` perturbs ONE probe's start, for the has-teeth control. 0 is the fixture.
std::unique_ptr<Fixture> BuildFixture(float nudge = 0.0f)
{
    auto fix = std::make_unique<Fixture>();
    fix->backend = CreatePhysicsBackend();
    REQUIRE(fix->backend != nullptr);
    REQUIRE(fix->backend->Create());

    // A shallow bowl, so bodies that slide have somewhere to slide to and the
    // resting state is not the trivial one every body reaches on a plane.
    fix->heights.assign(kTerrainN * kTerrainN, 0.0f);
    for (int z = 0; z < kTerrainN; ++z)
    {
        for (int x = 0; x < kTerrainN; ++x)
        {
            const float dx = static_cast<float>(x) - 7.5f;
            const float dz = static_cast<float>(z) - 7.5f;
            fix->heights[static_cast<std::size_t>(z) * kTerrainN + x] = 0.02f * (dx * dx + dz * dz);
        }
    }
    TerrainField field;
    field.heights = fix->heights.data();
    field.width = kTerrainN;
    field.height = kTerrainN;
    field.cellSize = kCellSize;
    field.originX = 0.0f;
    field.originZ = 0.0f;
    REQUIRE(fix->backend->SetTerrain(field));

    // A static obstacle to land against, from the same convex-piece path the
    // engine registers real Geometry LODs through.
    fix->cubePoints = CubeAt(0.0f, 0.5f, 0.0f, 0.5f);
    ConvexPiece piece{fix->cubePoints.data(), static_cast<int>(fix->cubePoints.size())};
    Matrix4     at(MIdentity);
    at.SetPosition(Vector3(75.0f, 1.13f, 75.0f));
    REQUIRE(fix->backend->AddStaticBody(&piece, 1, at).IsValid());

    // 24 probes in a tight cluster over the obstacle, four shapes, so they
    // collide with the terrain, the static body and each other.
    const ProbeShape kinds[] = {ProbeShape::Sphere, ProbeShape::Box, ProbeShape::Capsule, ProbeShape::Cylinder};
    for (int i = 0; i < 24; ++i)
    {
        SphereProbeDef def;
        def.shape = kinds[i % 4];
        const float col = static_cast<float>(i % 6) * 0.35f;
        const float row = static_cast<float>(i / 6) * 0.35f;
        def.position = Vector3(74.4f + col + (i == 0 ? nudge : 0.0f), 3.0f + row * 1.7f, 74.4f + row);
        def.velocity = Vector3(0.1f * static_cast<float>(i % 3 - 1), -0.5f, 0.05f * static_cast<float>(i % 5 - 2));
        def.radius = 0.16f;
        def.halfLength = 0.18f;
        def.halfExtents = Vector3(0.12f, 0.2f, 0.15f);
        def.yaw = 0.21f * static_cast<float>(i);
        def.mass = 0.4f + 0.05f * static_cast<float>(i % 4);
        def.friction = 0.5f;
        def.restitution = 0.25f;
        def.rollingResistance = 0.04f;
        const BodyId id = fix->backend->SpawnSphereProbe(def);
        REQUIRE(id.IsValid());
        fix->probes.push_back(id);
    }
    return fix;
}

/// Steps the fixture and hashes the COMPLETE readback at every step -- not only
/// the final pose. A divergence that appears at step 40 and is absorbed by
/// friction before step 240 is still a divergence, and a final-state-only hash
/// would call that run identical.
std::uint64_t RunAndHash(Fixture& fix, int steps = kSteps)
{
    Hasher                   hash;
    std::vector<ProbeSample> samples;
    BodyMotion               motion;
    for (int step = 0; step < steps; ++step)
    {
        fix.backend->Step(kStepSeconds);

        samples.clear();
        fix.backend->GetProbeSamples(samples);
        hash.U(static_cast<std::uint32_t>(samples.size()));
        for (const ProbeSample& s : samples)
        {
            HashSample(hash, s);
        }
        // GetBodyMotion is the OTHER readback -- the one LooseObjects uses to
        // drive a gameplay entity. It carries velocity and the sleep flag, which
        // GetProbeSamples does not, so hashing only samples would miss a
        // divergence that has not reached the pose yet.
        for (const BodyId& id : fix.probes)
        {
            const bool ok = fix.backend->GetBodyMotion(id, motion);
            hash.Byte(ok ? 1u : 0u);
            if (ok)
            {
                HashMotion(hash, motion);
            }
        }
    }
    return hash.h;
}

std::uint64_t BuildAndRun(float nudge = 0.0f, int steps = kSteps)
{
    auto fix = BuildFixture(nudge);
    const std::uint64_t h = RunAndHash(*fix, steps);
    fix->backend->Destroy();
    return h;
}

#if PHYS_DET_HAS_SSE
constexpr std::uint32_t kFtzBit = 0x8000u;
constexpr std::uint32_t kDazBit = 0x0040u;
#endif

/// Restores this thread's FP state and the process-wide capture on scope exit, so
/// a test that perturbs the environment cannot strand a later one. Same shape as
/// SIM-801's guard, for the same reason.
struct FpGuard
{
#if PHYS_DET_HAS_SSE
    std::uint32_t mxcsr = _mm_getcsr();
#endif
    int  rounding = std::fegetround();
    bool hadCapture = Fp::MainFpEnvironmentCaptured();

    ~FpGuard()
    {
        std::fesetround(rounding);
#if PHYS_DET_HAS_SSE
        _mm_setcsr(mxcsr);
#endif
        if (hadCapture)
        {
            Fp::CaptureMainFpEnvironment();
        }
    }
};

/// A computation whose result depends on flush-to-zero: the product lands inside
/// the binary32 subnormal window, so FTZ turns it into an exact zero.
/// SIM-801 records the two ways an earlier draft of this got it wrong.
std::uint32_t DenormalSentinel()
{
    volatile float a = 1e-20f;
    volatile float b = 1e-20f;
    const float    product = a * b;
    std::uint32_t  bits = 0;
    std::memcpy(&bits, &product, sizeof(bits));
    return bits;
}

} // namespace

// ---------------------------------------------------------------------------
// The measurement 8.4 actually turns on.

TEST_CASE("the physics backend steps on the calling thread only", "[physics][determinism]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    // 8.4's rule is that parallel access is enabled only where the thread-safety
    // contract is VERIFIED. The first fact that decision needs is whether the
    // backend is already running threads of its own -- box3d creates them and
    // uses an internal scheduler whenever workerCount > 1, and those threads
    // would be outside SIM-801's table, so their FP environment would be nobody's.
    //
    // 1 is the answer today and it is what makes the rest of this file's
    // single-threaded reasoning sound. If this ever fails, the SIM-804 write-up
    // is stale before anything else in it is.
    const PhysicsStats stats = backend->GetStats();
    CHECK(stats.workerCount == 1);

    backend->Destroy();
    // A destroyed world reports no workers rather than a stale count.
    CHECK(backend->GetStats().workerCount == 0);
}

// ---------------------------------------------------------------------------
// Anti-vacuity. These two run FIRST in the file's logic: everything below is
// worthless if the fixture does not move or the hash does not discriminate.

TEST_CASE("physics fixture is non-degenerate", "[physics][determinism]")
{
    auto fix = BuildFixture();

    std::vector<ProbeSample> before;
    fix->backend->GetProbeSamples(before);
    REQUIRE(before.size() == 24);

    RunAndHash(*fix);

    std::vector<ProbeSample> after;
    fix->backend->GetProbeSamples(after);
    REQUIRE(after.size() == 24);

    // Every body moved. A body that did not is a body whose contact was never
    // solved, and a determinism test over stationary bodies proves nothing.
    int moved = 0;
    int rotated = 0;
    for (std::size_t i = 0; i < after.size(); ++i)
    {
        if ((after[i].position - before[i].position).SquareSize() > 1e-4f)
        {
            ++moved;
        }
        if ((after[i].axisX - before[i].axisX).SquareSize() > 1e-6f)
        {
            ++rotated;
        }
    }
    CHECK(moved == 24);
    // Rotation means the SOLVER acted, not just gravity: a body only spins here
    // because something torqued it in a contact.
    CHECK(rotated > 12);

    // And they did not all land in one heap indistinguishable from each other.
    std::set<std::uint64_t> distinct;
    for (const ProbeSample& s : after)
    {
        Hasher one;
        HashSample(one, s);
        distinct.insert(one.h);
    }
    CHECK(distinct.size() == 24);

    fix->backend->Destroy();
}

TEST_CASE("physics run hash has teeth", "[physics][determinism]")
{
    const std::uint64_t reference = BuildAndRun();

    // A millimetre on one of twenty-four bodies must change the whole-run hash.
    // If it does not, the hash is not reading the state that matters.
    CHECK(BuildAndRun(0.001f) != reference);
    // And a different amount of simulation must differ from the full run.
    CHECK(BuildAndRun(0.0f, kSteps / 2) != reference);
    // Sanity in the other direction: the perturbations are not just "any two
    // runs differ".
    CHECK(BuildAndRun() == reference);
}

// ---------------------------------------------------------------------------
// Same thread, same process, repeated. These are the properties an in-process
// replay or a save/reload comparison would rest on.

TEST_CASE("a physics run repeats bit for bit in the same process", "[physics][determinism]")
{
    const std::uint64_t first = BuildAndRun();
    const std::uint64_t second = BuildAndRun();
    const std::uint64_t third = BuildAndRun();
    CHECK(first == second);
    CHECK(first == third);
}

TEST_CASE("a physics run does not depend on heap layout", "[physics][determinism]")
{
    // SIM-803 is an entire decision record about an ordering that ranked by heap
    // address. The equivalent hazard here is a solver or a readback that iterates
    // a pointer-keyed structure: it would be perfectly stable within one process
    // image and would diverge the moment allocation history changed. Churning the
    // allocator between runs is what makes that visible.
    const std::uint64_t clean = BuildAndRun();

    std::vector<std::unique_ptr<char[]>> churn;
    churn.reserve(512);
    for (int i = 0; i < 512; ++i)
    {
        churn.push_back(std::make_unique<char[]>(static_cast<std::size_t>(64 + (i * 37) % 4096)));
    }
    for (std::size_t i = 0; i < churn.size(); i += 2)
    {
        churn[i].reset();
    }

    CHECK(BuildAndRun() == clean);
}

TEST_CASE("two physics worlds do not disturb each other", "[physics][determinism]")
{
    // Nothing in the engine holds two physics worlds at once today -- there is one
    // global (`GetPhysicsWorld`). This is the negative control for the state a
    // backend might keep OUTSIDE its world handle: a file-scope scratch buffer, a
    // shared hull database, a global length scale. Interleaving two worlds is the
    // only way such a thing shows up, and the corpus census plus a live world is
    // exactly the shape that would hit it.
    const std::uint64_t alone = BuildAndRun();

    auto a = BuildFixture();
    auto b = BuildFixture();

    Hasher                   hash;
    std::vector<ProbeSample> samples;
    BodyMotion               motion;
    for (int step = 0; step < kSteps; ++step)
    {
        a->backend->Step(kStepSeconds);
        b->backend->Step(kStepSeconds); // the interference, if there is any

        samples.clear();
        a->backend->GetProbeSamples(samples);
        hash.U(static_cast<std::uint32_t>(samples.size()));
        for (const ProbeSample& s : samples)
        {
            HashSample(hash, s);
        }
        for (const BodyId& id : a->probes)
        {
            const bool ok = a->backend->GetBodyMotion(id, motion);
            hash.Byte(ok ? 1u : 0u);
            if (ok)
            {
                HashMotion(hash, motion);
            }
        }
    }
    CHECK(hash.h == alone);

    a->backend->Destroy();
    b->backend->Destroy();
}

// ---------------------------------------------------------------------------
// The path nothing calls: stepping physics off the main thread.
//
// SIM-806's finding came from writing a negative control for a signature that
// existed for a use nobody had yet. This is that, for 8.4: physics is stepped
// from `World::StepSimulation` on the main thread and from nowhere else, so the
// question "would moving it cost determinism" has no evidence behind it.

TEST_CASE("physics stepped on a worker matches the main thread", "[physics][determinism][fp]")
{
    FpGuard guard;

    // Put the MAIN thread somewhere the platform default is not, and make that the
    // reference. This is the shape SIM-801 found in the wild: `enablePIII` leaves
    // the main thread flushing denormals while every freshly created thread does
    // not, and nothing propagated it.
#if PHYS_DET_HAS_SSE
    _mm_setcsr(_mm_getcsr() | kFtzBit | kDazBit);
#endif
    Fp::CaptureMainFpEnvironment();

    const std::uint32_t mainSentinel = DenormalSentinel();
    const std::uint64_t mainHash = BuildAndRun();

    std::uint32_t conformedSentinel = 0;
    std::uint64_t conformedHash = 0;
    std::thread   conformed(
        [&]
        {
            // What every engine-owned worker does since SIM-801.
            Fp::ApplyMainFpEnvironment();
            conformedSentinel = DenormalSentinel();
            conformedHash = BuildAndRun();
        });
    conformed.join();

    std::uint32_t rawSentinel = 0;
    std::uint64_t rawHash = 0;
    std::thread   raw(
        [&]
        {
            // A thread that does NOT conform -- a bare std::thread, or a
            // middleware callback. `poThreadCreate` still hands control straight
            // to a caller-supplied proc with no wrapper to inject into
            // (SIM-801, "What is NOT covered"), so this is reachable.
            rawSentinel = DenormalSentinel();
            rawHash = BuildAndRun();
        });
    raw.join();

    // THE LOAD-BEARING ASSERTION. A conformed worker reproduces the main thread
    // exactly. This is the property that has to hold for physics to be movable
    // off the main thread at all, and it is the one that would break silently.
    CHECK(conformedHash == mainHash);
    CHECK(conformedSentinel == mainSentinel);

#if PHYS_DET_HAS_SSE
    // Not vacuous: the perturbation IS live on the unconformed thread. Without
    // this, a build where the main thread happened to match the default would
    // pass the assertion above having tested nothing.
    CHECK(rawSentinel != mainSentinel);
    CHECK(rawSentinel != 0u);      // the unconformed thread keeps the subnormal
    CHECK(mainSentinel == 0u);     // the flush-to-zero main thread does not
#endif

    // Whether the SOLVER itself is FP-environment-sensitive is a measurement, not
    // a requirement, and it is recorded in SIM-804 rather than asserted in either
    // direction -- pinning it would turn a fact about box3d 0.1.0's internals into
    // a test the next version has to keep passing. What matters is that a
    // conformed worker agrees, which is checked above unconditionally.
    (void)rawHash;
}

TEST_CASE("physics readback from a worker matches the stepping thread", "[physics][determinism]")
{
    // The other half of 8.4's sentence: not "can it step elsewhere" but "can it
    // be QUERIED elsewhere". Stepped on the main thread, read on a worker after
    // the step has returned. This is the access pattern a render or audio thread
    // would want, and the safe-looking one -- there is no concurrency here at all,
    // only a hand-off -- so it is worth pinning that the readback carries no
    // thread-local state.
    auto fix = BuildFixture();
    for (int step = 0; step < kSteps; ++step)
    {
        fix->backend->Step(kStepSeconds);
    }

    Hasher                   onMain;
    std::vector<ProbeSample> samples;
    BodyMotion               motion;
    fix->backend->GetProbeSamples(samples);
    for (const ProbeSample& s : samples)
    {
        HashSample(onMain, s);
    }
    for (const BodyId& id : fix->probes)
    {
        REQUIRE(fix->backend->GetBodyMotion(id, motion));
        HashMotion(onMain, motion);
    }

    std::uint64_t onWorker = 0;
    std::thread   reader(
        [&]
        {
            Hasher                   h;
            std::vector<ProbeSample> s2;
            BodyMotion               m2;
            fix->backend->GetProbeSamples(s2);
            for (const ProbeSample& s : s2)
            {
                HashSample(h, s);
            }
            for (const BodyId& id : fix->probes)
            {
                if (fix->backend->GetBodyMotion(id, m2))
                {
                    HashMotion(h, m2);
                }
            }
            onWorker = h.h;
        });
    reader.join();

    CHECK(onWorker == onMain.h);
    fix->backend->Destroy();
}
