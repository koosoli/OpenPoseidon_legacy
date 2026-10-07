// Roadmap Phase 8.6 — "Scheduler and determinism validation".
//
// The gate asks that a representative deterministic fixture be run with serial
// task execution, 1 / 2 / 4 / a typical desktop worker count, and deliberately
// perturbed job scheduling, and that no discrete authoritative outcome differ.
//
// THE FIXTURE IS REAL, NOT SYNTHETIC.  `Landscape::GenerateSegmentInto` is the
// only production `TaskPool::ParallelFor` in the Poseidon library today
// (Landscape.cpp:1358, `LandCache::Fill` pass 3 — the two others are in
// WgpuRenderer, which this binary does not link).  It is also the only one whose
// call site already has a serial arm: `Fill` runs a plain `for` loop when the
// pool is null or there is a single miss, so "serial task execution" is an
// existing production path here, not something invented for the test.
//
// WHAT IS COMPARED.  Not a spot check: every generated segment is hashed in
// full — vertex positions, normals, UVs and clip flags as raw IEEE-754 bits,
// every face's arity, vertex indices, special flags and texture identity, the
// per-shape texture table, the bounding box, and the water sub-mesh — then the
// per-segment hashes are compared elementwise and as a sequence across arms.
// Float BITS, never float values: -0.0f compares equal to 0.0f and a NaN
// compares unequal to itself, so a value comparison would be both too loose and
// too tight to be the "strict comparison" the gate names.
//
// Related: SIM-801 (worker FP environment), SIM-803 (stable iteration and
// tie-breaking).  The decision record for this file is
// design notes,
// which also lists what this harness does NOT cover.

#include <catch2/catch_test_macros.hpp>

#include "../test_fixtures.hpp"

#include <Poseidon/Core/TaskPool.hpp>
#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/LandscapeShared.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Poseidon::LandBegEnd;
using Poseidon::Landscape;
using Poseidon::LandSegment;
using Poseidon::Ref;
using Poseidon::Shape;
using Poseidon::TaskPool;
using Poseidon::Texture;

namespace
{

// ---------------------------------------------------------------------------
// Bit-exact hashing
// ---------------------------------------------------------------------------

// FNV-1a 64.  Chosen for being three lines and order-sensitive; this is a
// comparison aid, not a security primitive.
class BitHasher
{
  public:
    void Bytes(const void* data, size_t n)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i)
        {
            _h ^= static_cast<uint64_t>(p[i]);
            _h *= 1099511628211ull;
        }
    }

    void U32(uint32_t v) { Bytes(&v, sizeof(v)); }
    void I32(int32_t v) { Bytes(&v, sizeof(v)); }

    // Hash the storage bits, not the arithmetic value.  See the file header.
    void F32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        U32(bits);
    }

    uint64_t Value() const { return _h; }

  private:
    uint64_t _h = 1469598103934665603ull;
};

// Hash a Shape completely.  Every field that survives GenerateSegmentInto and
// can be read back through the public API is folded in.
void HashShape(BitHasher& h, const Shape& shape)
{
    const int nVertex = shape.NVertex();
    h.I32(nVertex);
    for (int i = 0; i < nVertex; ++i)
    {
        const auto pos = shape.Pos(i);
        h.F32(pos.X());
        h.F32(pos.Y());
        h.F32(pos.Z());
        const auto norm = shape.Norm(i);
        h.F32(norm.X());
        h.F32(norm.Y());
        h.F32(norm.Z());
        h.F32(shape.U(i));
        h.F32(shape.V(i));
        h.I32(static_cast<int32_t>(shape.Clip(i)));
    }

    // Texture identity as an INDEX into this shape's own texture table, never as
    // a pointer: an address is allocator state, and SIM-803 is a whole decision
    // record about ranking by one.  The index also catches a divergence in
    // registration ORDER, which is what the shared GSegTextureRegMutex guards.
    const int nTextures = shape.NTextures();
    h.I32(nTextures);

    h.I32(shape.NFaces());
    for (Offset f = shape.BeginFaces(); f < shape.EndFaces(); shape.NextFace(f))
    {
        const Poly& face = shape.Face(f);
        h.I32(face.N());
        for (int v = 0; v < face.N(); ++v)
        {
            h.I32(static_cast<int32_t>(face.GetVertex(v)));
        }
        h.I32(face.Special());
        h.I32(shape.GetTextureIndex(face.GetTexture()));
    }

    const auto mn = shape.Min();
    const auto mx = shape.Max();
    h.F32(mn.X());
    h.F32(mn.Y());
    h.F32(mn.Z());
    h.F32(mx.X());
    h.F32(mx.Y());
    h.F32(mx.Z());
}

uint64_t HashSegment(const LandSegment& seg)
{
    BitHasher h;
    h.I32(seg.GetLodLevel());
    h.I32(seg.SomeWater() ? 1 : 0);
    h.I32(seg.OnlyWater() ? 1 : 0);
    const auto off = seg.Offset();
    h.F32(off.X());
    h.F32(off.Y());
    h.F32(off.Z());
    HashShape(h, seg.GetTable());
    HashShape(h, seg.GetWaterTable());
    return h.Value();
}

// ---------------------------------------------------------------------------
// The fixture landscape
// ---------------------------------------------------------------------------

// `Dim`, `SetData` and `SetTex` are protected on Landscape, and the terrain and
// texture tables are normally filled by the .wrp loader.  This subclass builds
// the same state from synthetic values so the harness needs no game data — the
// GEOMETRY KERNEL under test is the unmodified production one.
class HarnessLandscape : public Landscape
{
  public:
    HarnessLandscape() : Landscape(Poseidon::GEngine, nullptr, false) {}

    using Landscape::_random;
    using Landscape::_texture;
    using Landscape::Dim;
    using Landscape::SetData;
    using Landscape::SetTex;
};

// Land 64x64 over terrain 128x128, i.e. subdivision level 1 — the segment mesh
// is refined 2x per land cell, which is what exercises the inner subdivision
// loops, the shared-vertex candidate lists and the seam quads.  A flat
// land==terrain configuration would collapse all of that.
constexpr int kLandRange = 64;
constexpr int kTerrainRange = 128;
constexpr float kLandGrid = 50.0f;

// 64 land cells / 8 per segment = 8x8 = 64 segments.  Enough work items that a
// 2-, 4- and 8-worker split all produce different partition boundaries.
constexpr int kSegmentsPerAxis = kLandRange / Poseidon::LandSegmentSize;
constexpr int kSegmentCount = kSegmentsPerAxis * kSegmentsPerAxis;

// A height field with real relief and a genuine coastline.  The point is not
// realism, it is that the arithmetic in GenerateSegmentInto (cross products,
// Normalized(), bilinear UV offsets) sees varied inputs rather than a constant,
// and that both the land branch and the water branch are taken.
float FixtureHeight(int x, int z)
{
    const float fx = static_cast<float>(x);
    const float fz = static_cast<float>(z);
    // Deliberately not std::sin: keep the fixture's own arithmetic simple and
    // exactly reproducible, and leave the interesting FP to the engine.
    const float ridge = 0.05f * ((x * 7 + z * 13) % 97);
    const float bowl = 0.002f * ((fx - 64.0f) * (fx - 64.0f) + (fz - 64.0f) * (fz - 64.0f));
    return ridge + bowl - 22.0f; // negative in the middle => sea, positive at the rim
}

// Cache the fixture: `Landscape`'s constructor is not cheap and every arm must
// read byte-identical input anyway.  Reusing one instance is also the stronger
// test — it removes "the two arms were given different landscapes" as an
// explanation for agreement or disagreement.
struct FixtureState
{
    HarnessLandscape land;
    std::vector<LandBegEnd> rects;
};

FixtureState& Fixture()
{
    static FixtureState* state = []
    {
        auto* s = new FixtureState();
        s->land.Dim(kLandRange, kLandRange, kTerrainRange, kTerrainRange, kLandGrid);

        for (int z = 0; z < kTerrainRange; ++z)
        {
            for (int x = 0; x < kTerrainRange; ++x)
            {
                s->land.SetData(x, z, FixtureHeight(x, z));
            }
        }

        // Two real textures so that the `if (texture)` branch — RegisterTexture
        // under the shared GSegTextureRegMutex, and the UToPhysical/VToPhysical
        // UV mapping — is actually executed.  Index 0 is the water slot.
        // NoTextures is the engine's global "skip all texture I/O" switch; the
        // dummy engine honours it, so it has to be off while we load.
        const bool prevNoTextures = Poseidon::NoTextures;
        Poseidon::NoTextures = false;
        Ref<Texture> texA = Poseidon::GlobLoadTexture(GET_FIXTURE("jpg/checker_32x32.jpg"));
        Ref<Texture> texB = Poseidon::GlobLoadTexture(GET_FIXTURE("jpg/gradient_64x64.jpg"));
        Poseidon::NoTextures = prevNoTextures;

        s->land._texture.Resize(3);
        s->land._texture[0].texture = texA; // water slot
        s->land._texture[0].offsetUV = false;
        s->land._texture[1].texture = texA;
        s->land._texture[1].offsetUV = false; // => ClampU|ClampV
        s->land._texture[2].texture = texB;
        s->land._texture[2].offsetUV = true; // => NoClamp, and the _random UV offsets apply

        for (int z = 0; z < kLandRange; ++z)
        {
            for (int x = 0; x < kLandRange; ++x)
            {
                // Index 0 means "emit no land polygon here"; keep a few so the
                // skip branch is covered too, but keep them rare.
                const int t = ((x * 5 + z * 3) % 11 == 0) ? 0 : (((x + z) & 1) ? 1 : 2);
                s->land.SetTex(x, z, t);

                auto& r = s->land._random(x, z);
                r.color = static_cast<unsigned>((x * 31 + z * 17) & 0xFF);
                r.uOff = static_cast<int>((x * 3 + z) % 15) - 7;
                r.vOff = static_cast<int>((z * 5 + x) % 15) - 7;
            }
        }

        for (int sz = 0; sz < kSegmentsPerAxis; ++sz)
        {
            for (int sx = 0; sx < kSegmentsPerAxis; ++sx)
            {
                LandBegEnd rect;
                rect.xBeg = sx * Poseidon::LandSegmentSize;
                rect.zBeg = sz * Poseidon::LandSegmentSize;
                rect.xEnd = rect.xBeg + Poseidon::LandSegmentSize;
                rect.zEnd = rect.zBeg + Poseidon::LandSegmentSize;
                s->rects.push_back(rect);
            }
        }
        return s;
    }();
    return *state;
}

// ---------------------------------------------------------------------------
// The arms
// ---------------------------------------------------------------------------

// workerCount == 0 selects the SERIAL arm: the plain `for` loop that
// LandCache::Fill runs when there is no pool.  Otherwise a TaskPool with
// exactly `workerCount` total threads (enkiTS counts the calling thread), which
// is what --max-threads resolves to in the shipping game.
std::vector<uint64_t> RunAtWorkerCount(uint32_t workerCount)
{
    FixtureState& fx = Fixture();
    const uint32_t n = static_cast<uint32_t>(fx.rects.size());

    // Segments are allocated on the calling thread in production too — the
    // FastAlloc behind USE_FAST_ALLOCATOR is not thread-safe, which is exactly
    // why LandCache::Fill has a separate "pass 2" for this.
    std::vector<Ref<LandSegment>> segs(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        segs[i] = new LandSegment;
    }

    Landscape* land = &fx.land;
    auto generate = [land, &fx, &segs](uint32_t start, uint32_t end)
    {
        for (uint32_t i = start; i < end; ++i)
        {
            land->GenerateSegmentInto(segs[i], fx.rects[i], /*deferGPU=*/true, /*lodLevel=*/0);
        }
    };

    if (workerCount == 0)
    {
        generate(0, n);
    }
    else
    {
        TaskPool pool(workerCount);
        pool.ParallelFor(n, generate);
    }

    std::vector<uint64_t> hashes(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        hashes[i] = HashSegment(*segs[i]);
    }
    return hashes;
}

// Report the FIRST divergent element, in the roadmap's own terms ("the first
// divergent authoritative state boundary"), rather than just "vectors differ".
void RequireIdentical(const std::vector<uint64_t>& reference, const std::string& referenceName,
                      const std::vector<uint64_t>& actual, const std::string& actualName)
{
    REQUIRE(actual.size() == reference.size());
    for (size_t i = 0; i < reference.size(); ++i)
    {
        if (actual[i] != reference[i])
        {
            FAIL("Segment " << i << " diverged between " << referenceName << " and " << actualName << ": 0x" << std::hex
                            << reference[i] << " vs 0x" << actual[i]);
        }
    }
    SUCCEED("all " << reference.size() << " segments identical: " << referenceName << " vs " << actualName);
}

uint32_t TypicalWorkerCount()
{
    const unsigned hw = std::thread::hardware_concurrency();
    return Poseidon::ResolveTaskPoolThreadCount(/*cliOverride=*/-1, hw);
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("Terrain fixture is non-degenerate", "[determinism][terrain]")
{
    // A determinism harness that agrees because it generated nothing is the
    // classic vacuous pass.  Pin that the fixture really produces geometry, that
    // both the land and the water branch are taken, and that textures are bound
    // — otherwise the arms below could all agree on 64 empty meshes.
    FixtureState& fx = Fixture();
    REQUIRE(fx.rects.size() == kSegmentCount);
    REQUIRE(fx.land._texture[1].texture.NotNull());
    REQUIRE(fx.land._texture[2].texture.NotNull());

    int withLand = 0, withWater = 0, texturedShapes = 0;
    long long totalFaces = 0, totalVertices = 0;
    for (size_t i = 0; i < fx.rects.size(); ++i)
    {
        Ref<LandSegment> seg = new LandSegment;
        fx.land.GenerateSegmentInto(seg, fx.rects[i], true, 0);
        totalFaces += seg->GetTable().NFaces();
        totalVertices += seg->GetTable().NVertex();
        if (seg->GetTable().NFaces() > 0)
        {
            ++withLand;
        }
        if (seg->GetWaterTable().NFaces() > 0)
        {
            ++withWater;
        }
        if (seg->GetTable().NTextures() > 0)
        {
            ++texturedShapes;
        }
    }

    CHECK(withLand > 0);
    CHECK(withWater > 0);
    CHECK(texturedShapes > 0);
    CHECK(totalFaces > 1000);
    CHECK(totalVertices > 1000);
    INFO("segments=" << kSegmentCount << " land=" << withLand << " water=" << withWater
                     << " textured=" << texturedShapes << " faces=" << totalFaces);
}

TEST_CASE("Segment hash has teeth", "[determinism][terrain]")
{
    // The negative control.  Every arm below reports "identical"; that is only
    // evidence if the comparator would have said "different" had the geometry
    // differed.  A hash that read nothing, or that folded in only constants,
    // would make the whole harness pass vacuously and for ever.
    FixtureState& fx = Fixture();
    const LandBegEnd rect = fx.rects[27]; // arbitrary interior segment

    Ref<LandSegment> a = new LandSegment;
    fx.land.GenerateSegmentInto(a, rect, true, 0);
    const uint64_t hashA = HashSegment(*a);

    // Same input, same code path, second object: must agree, or the hash is
    // reading address-dependent or uninitialised state.
    Ref<LandSegment> b = new LandSegment;
    fx.land.GenerateSegmentInto(b, rect, true, 0);
    CHECK(HashSegment(*b) == hashA);

    // Different input => different hash.  LOD 1 halves the mesh stride, so the
    // vertex count, positions, normals and faces all change.
    Ref<LandSegment> coarse = new LandSegment;
    fx.land.GenerateSegmentInto(coarse, rect, true, 1);
    CHECK(HashSegment(*coarse) != hashA);
    CHECK(coarse->GetTable().NVertex() != a->GetTable().NVertex());

    // A different rectangle of the same size must also hash differently — this
    // is what makes the ELEMENTWISE comparison below meaningful rather than a
    // comparison of 64 copies of one value.
    Ref<LandSegment> elsewhere = new LandSegment;
    fx.land.GenerateSegmentInto(elsewhere, fx.rects[28], true, 0);
    CHECK(HashSegment(*elsewhere) != hashA);
}

TEST_CASE("Regenerating into a used segment leaves no residue", "[determinism][terrain]")
{
    // SIM-806's actual finding.  `GenerateSegmentInto` takes a caller-owned
    // segment for exactly one reason — so it can be REgenerated instead of
    // reallocated — and nothing in the tree does that yet, so the reset had
    // never been exercised.  It cleared `_table`'s FACES and not its VERTICES,
    // so a second pass appended a whole second copy of the mesh: correct to
    // look at, and a vertex buffer that doubles every time, for ever.  See the
    // comment at the top of Landscape::GenerateSegmentInto.
    //
    // The bit-for-bit hash is what caught this; no count assertion was looking.
    // The explicit counts below are here so a regression says WHICH property
    // broke rather than only "the hash moved".
    FixtureState& fx = Fixture();

    // A wet-and-dry rect (both sub-meshes populated) and, separately, whichever
    // rects are dry and wet, so the wet->dry transition is covered too.
    const LandBegEnd rect = fx.rects[27];

    Ref<LandSegment> fresh = new LandSegment;
    fx.land.GenerateSegmentInto(fresh, rect, true, 0);
    const uint64_t expected = HashSegment(*fresh);
    const int expectedVertices = fresh->GetTable().NVertex();
    const int expectedFaces = fresh->GetTable().NFaces();
    const int expectedTextures = fresh->GetTable().NTextures();
    const int expectedWaterVertices = fresh->GetWaterTable().NVertex();
    REQUIRE(expectedVertices > 0);
    REQUIRE(expectedFaces > 0);

    Ref<LandSegment> reused = new LandSegment;
    for (int pass = 0; pass < 4; ++pass)
    {
        fx.land.GenerateSegmentInto(reused, rect, true, 0);
        INFO("pass " << pass);
        CHECK(reused->GetTable().NVertex() == expectedVertices);
        CHECK(reused->GetTable().NFaces() == expectedFaces);
        CHECK(reused->GetTable().NTextures() == expectedTextures);
        CHECK(reused->GetWaterTable().NVertex() == expectedWaterVertices);
        CHECK(HashSegment(*reused) == expected);
    }

    // Regenerating a segment as a DIFFERENT rect must not leave the old rect's
    // geometry behind either — the pooled-slot case, which is the one that
    // would actually ship stale terrain rather than merely waste memory.
    for (size_t i = 0; i < fx.rects.size(); ++i)
    {
        Ref<LandSegment> pooled = new LandSegment;
        fx.land.GenerateSegmentInto(pooled, fx.rects[(i + 7) % fx.rects.size()], true, 0);
        fx.land.GenerateSegmentInto(pooled, fx.rects[i], true, 0);

        Ref<LandSegment> reference = new LandSegment;
        fx.land.GenerateSegmentInto(reference, fx.rects[i], true, 0);

        if (HashSegment(*pooled) != HashSegment(*reference))
        {
            FAIL("segment " << i << " differs when generated into a recycled object");
        }
    }
    SUCCEED("all " << fx.rects.size() << " rects survive generation into a recycled segment");
}

TEST_CASE("Terrain generation is bit-identical at every worker count", "[determinism][terrain][task_pool]")
{
    // The roadmap's list, verbatim: serial, 1, 2, 4, typical desktop.
    const std::vector<uint64_t> serial = RunAtWorkerCount(0);

    // Guard against the other vacuous pass: if every segment hashed to the same
    // value, an elementwise comparison would succeed no matter how the work was
    // shuffled between partitions.
    {
        std::vector<uint64_t> distinct = serial;
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        REQUIRE(distinct.size() > kSegmentCount / 2);
    }

    RequireIdentical(serial, "serial", RunAtWorkerCount(1), "1 worker");
    RequireIdentical(serial, "serial", RunAtWorkerCount(2), "2 workers");
    RequireIdentical(serial, "serial", RunAtWorkerCount(4), "4 workers");

    const uint32_t typical = TypicalWorkerCount();
    RequireIdentical(serial, "serial", RunAtWorkerCount(typical), "typical (" + std::to_string(typical) + ")");
}

TEST_CASE("Terrain generation is bit-identical under perturbed scheduling", "[determinism][terrain][task_pool]")
{
    // "Deliberately perturbed job scheduling where practical."  enkiTS gives no
    // hook to force a particular partitioning, so perturb the two things that
    // are reachable: an OVER-SUBSCRIBED pool (more threads than the machine has
    // cores, so partitions are preempted mid-flight and interleave differently
    // every run), and REPEATED runs on the SAME pool, where enki's work-stealing
    // hands different partitions to different threads run to run.
    const std::vector<uint64_t> serial = RunAtWorkerCount(0);

    const uint32_t oversubscribed = std::max<uint32_t>(16, std::thread::hardware_concurrency() * 2);
    RequireIdentical(serial, "serial", RunAtWorkerCount(oversubscribed), "oversubscribed");

    // Three passes at a mid worker count: work stealing makes the partition-to-
    // thread mapping vary between them even though the request is identical.
    for (int pass = 0; pass < 3; ++pass)
    {
        RequireIdentical(serial, "serial", RunAtWorkerCount(4), "4 workers pass " + std::to_string(pass));
    }
}

TEST_CASE("Terrain generation is bit-identical with the main FP environment captured",
          "[determinism][terrain][task_pool][fp]")
{
    // SIM-801 gave TaskPool workers the main thread's MXCSR, but only once
    // `CaptureMainFpEnvironment()` has run — and it has NOT run in this test
    // binary, so every arm above executes with the capture EMPTY and
    // `EnsureFpEnvironmentMatchesMain()` a no-op.  That means the arms above
    // prove worker-count determinism, and prove nothing about the SIM-801 path.
    // Capture here, and re-run, so both halves are covered.
    namespace FP = Poseidon::Foundation;

    const bool wasCaptured = FP::MainFpEnvironmentCaptured();
    FP::CaptureMainFpEnvironment();
    REQUIRE(FP::MainFpEnvironmentCaptured());

    const std::vector<uint64_t> serial = RunAtWorkerCount(0);
    RequireIdentical(serial, "serial", RunAtWorkerCount(1), "1 worker (FP captured)");
    RequireIdentical(serial, "serial", RunAtWorkerCount(4), "4 workers (FP captured)");
    RequireIdentical(serial, "serial", RunAtWorkerCount(TypicalWorkerCount()), "typical (FP captured)");

    INFO("main FP environment was " << (wasCaptured ? "already" : "not previously") << " captured");
}

TEST_CASE("TaskPool honours the requested worker count", "[determinism][task_pool]")
{
    // The harness above is only meaningful if asking for N threads actually
    // gives N.  enkiTS counts the calling thread, and TaskPool subtracts one
    // before handing `numTaskThreadsToCreate` over — so a request of 1 means
    // "the calling thread only", which is the serial-equivalent arm.
    for (uint32_t requested : {1u, 2u, 4u, 8u})
    {
        TaskPool pool(requested);
        INFO("requested " << requested);
        CHECK(pool.ThreadCount() == requested);
    }
}
