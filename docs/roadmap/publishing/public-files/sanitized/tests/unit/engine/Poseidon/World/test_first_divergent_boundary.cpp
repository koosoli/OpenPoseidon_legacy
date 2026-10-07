// Roadmap Phase 8.6 and Phase 13 -- "record/replay should report the FIRST
// divergent authoritative state boundary". See
// design notes
//
// SIM-806 (terrain, worker counts) and SIM-804 (physics, threading) both compare
// two runs by hashing each WHOLE run into one 64-bit value. That is the right
// oracle for "did they diverge" and it is useless for "where". When SIM-804's
// comparison fails, what a reader learns is that two 240-tick runs over 24 bodies
// disagree somewhere among roughly 14,000 numbers.
//
// This file is the same discipline -- bits not values, strict equality, same build,
// same platform, no tolerance, anti-vacuity guards, a live ablation -- applied to a
// TIMELINE instead of a scalar, so the answer is a tick, a record and a field.
//
// The engine side is Poseidon/Core/StateTimeline.hpp. It is deliberately not a
// physics facility: physics is merely the only authoritative simulation job that
// exists today.
//
// No game data required.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Core/StateTimeline.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace Poseidon::Physics;
namespace Det = Poseidon::Determinism;

namespace
{

// ---------------------------------------------------------------------------
// The fixture. Same parameters as SIM-804's, deliberately, so the two harnesses
// are describing the same simulation and a reader can put the two write-ups side
// by side. A cluster and not a scatter: bodies that never touch measure only the
// integration of gravity, and the contact graph is the interesting state.

constexpr int   kTerrainN = 16;
constexpr float kCellSize = 10.0f;
constexpr float kStepSeconds = 1.0f / 60.0f;
constexpr int   kSteps = 240;
constexpr int   kProbes = 24;

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

std::unique_ptr<Fixture> BuildFixture()
{
    auto fix = std::make_unique<Fixture>();
    fix->backend = CreatePhysicsBackend();
    REQUIRE(fix->backend != nullptr);
    REQUIRE(fix->backend->Create());

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

    fix->cubePoints = CubeAt(0.0f, 0.5f, 0.0f, 0.5f);
    ConvexPiece piece{fix->cubePoints.data(), static_cast<int>(fix->cubePoints.size())};
    Matrix4     at(MIdentity);
    at.SetPosition(Vector3(75.0f, 1.13f, 75.0f));
    REQUIRE(fix->backend->AddStaticBody(&piece, 1, at).IsValid());

    const ProbeShape kinds[] = {ProbeShape::Sphere, ProbeShape::Box, ProbeShape::Capsule, ProbeShape::Cylinder};
    for (int i = 0; i < kProbes; ++i)
    {
        SphereProbeDef def;
        def.shape = kinds[i % 4];
        const float col = static_cast<float>(i % 6) * 0.35f;
        const float row = static_cast<float>(i / 6) * 0.35f;
        def.position = Vector3(74.4f + col, 3.0f + row * 1.7f, 74.4f + row);
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

// ---------------------------------------------------------------------------
// Labels, interned once. `key` is the probe's index in the fixture, never its
// BodyId and never an address: SIM-803 is an entire decision record about what
// happens when an ordering is derived from allocator state.

struct Labels
{
    Det::NameId motion, sample;
    Det::NameId posX, posY, posZ;
    Det::NameId axX, axY, axZ;   // one component each of the three basis vectors
    Det::NameId linX, linY, linZ;
    Det::NameId angX, angY, angZ;
    Det::NameId awake, known;
    Det::NameId count, shape, radius;

    explicit Labels(Det::StateTimeline& t)
        : motion(t.Intern("probe"))
        , sample(t.Intern("sample"))
        , posX(t.Intern("pos.x"))
        , posY(t.Intern("pos.y"))
        , posZ(t.Intern("pos.z"))
        , axX(t.Intern("axisX.x"))
        , axY(t.Intern("axisY.y"))
        , axZ(t.Intern("axisZ.z"))
        , linX(t.Intern("linvel.x"))
        , linY(t.Intern("linvel.y"))
        , linZ(t.Intern("linvel.z"))
        , angX(t.Intern("angvel.x"))
        , angY(t.Intern("angvel.y"))
        , angZ(t.Intern("angvel.z"))
        , awake(t.Intern("awake"))
        , known(t.Intern("known"))
        , count(t.Intern("count"))
        , shape(t.Intern("shape"))
        , radius(t.Intern("radius"))
    {
    }
};

/// Called before each Step with the tick index. This is how an ablation gets in --
/// a perturbation applied to the live world, not a doctored recording.
using Perturb = std::function<void(Fixture&, int)>;

/// Steps the fixture and records the complete readback into a timeline.
///
/// Both readbacks the boundary offers are recorded, for SIM-804's reason:
/// `GetProbeSamples` carries the shape and the resolved pose, `GetBodyMotion`
/// carries velocity and the sleep flag, and a divergence that has reached velocity
/// but not yet position is invisible to either one alone.
void Record(Fixture& fix, Det::StateTimeline& out, const Perturb& perturb = {}, int steps = kSteps)
{
    const Labels L(out);
    std::vector<ProbeSample> samples;
    BodyMotion               motion;

    for (int tick = 0; tick < steps; ++tick)
    {
        if (perturb)
        {
            perturb(fix, tick);
        }
        fix.backend->Step(kStepSeconds);

        out.BeginTick();

        samples.clear();
        fix.backend->GetProbeSamples(samples);
        // Discrete: a body appearing or vanishing is not a rounding artefact.
        out.U32(L.sample, 0u, L.count, static_cast<std::uint32_t>(samples.size()));
        for (std::size_t s = 0; s < samples.size(); ++s)
        {
            const ProbeSample& ps = samples[s];
            const auto         key = static_cast<std::uint32_t>(s);
            out.U32(L.sample, key, L.shape, static_cast<std::uint32_t>(ps.shape));
            out.F32(L.sample, key, L.radius, ps.radius);
            out.F32(L.sample, key, L.posX, ps.position[0]);
            out.F32(L.sample, key, L.posY, ps.position[1]);
            out.F32(L.sample, key, L.posZ, ps.position[2]);
        }

        for (std::size_t p = 0; p < fix.probes.size(); ++p)
        {
            const auto key = static_cast<std::uint32_t>(p);
            const bool ok = fix.backend->GetBodyMotion(fix.probes[p], motion);
            out.Bool(L.motion, key, L.known, ok);
            if (!ok)
            {
                continue;
            }
            out.F32(L.motion, key, L.posX, motion.position[0]);
            out.F32(L.motion, key, L.posY, motion.position[1]);
            out.F32(L.motion, key, L.posZ, motion.position[2]);
            out.F32(L.motion, key, L.axX, motion.axisX[0]);
            out.F32(L.motion, key, L.axY, motion.axisY[1]);
            out.F32(L.motion, key, L.axZ, motion.axisZ[2]);
            out.F32(L.motion, key, L.linX, motion.linearVelocity[0]);
            out.F32(L.motion, key, L.linY, motion.linearVelocity[1]);
            out.F32(L.motion, key, L.linZ, motion.linearVelocity[2]);
            out.F32(L.motion, key, L.angX, motion.angularVelocity[0]);
            out.F32(L.motion, key, L.angY, motion.angularVelocity[1]);
            out.F32(L.motion, key, L.angZ, motion.angularVelocity[2]);
            // Discrete, and the one that matters most on this fixture: a body that
            // fell asleep a tick earlier in one run than the other has diverged in a
            // way no epsilon on a position could ever be allowed to absorb.
            out.Bool(L.motion, key, L.awake, motion.awake);
        }

        out.EndTick();
    }
}

void RunInto(Det::StateTimeline& out, const Perturb& perturb = {}, int steps = kSteps)
{
    auto fix = BuildFixture();
    Record(*fix, out, perturb, steps);
    fix->backend->Destroy();
}

/// A perturbation with an obvious physical meaning: shove one body, once, hard
/// enough that the very next readback cannot look the same.
Perturb ImpulseAt(int atTick, int probeIndex)
{
    return [atTick, probeIndex](Fixture& fix, int tick) {
        if (tick != atTick)
        {
            return;
        }
        BodyMotion m;
        REQUIRE(fix.backend->GetBodyMotion(fix.probes[static_cast<std::size_t>(probeIndex)], m));
        fix.backend->ApplyImpulse(fix.probes[static_cast<std::size_t>(probeIndex)], m.position,
                                  Vector3(0.0f, 2.0f, 0.0f));
    };
}

float FloatFromBits(std::uint64_t bits)
{
    const auto    narrow = static_cast<std::uint32_t>(bits);
    float         v = 0.0f;
    std::memcpy(&v, &narrow, sizeof(v));
    return v;
}

} // namespace

// ===========================================================================
// Section 1 -- the comparison itself, on timelines built by hand.
//
// These run first because everything below is worthless if the comparison cannot
// tell the four kinds of divergence apart, or if it agrees with itself for the
// wrong reason.

TEST_CASE("identical timelines report no divergent boundary", "[determinism][replay]")
{
    Det::StateTimeline a;
    Det::StateTimeline b;
    for (int t = 0; t < 8; ++t)
    {
        for (auto* tl : {&a, &b})
        {
            tl->BeginTick();
            tl->F32("body", 0u, "pos.x", 1.0f * static_cast<float>(t));
            tl->Bool("body", 0u, "alive", true);
            tl->EndTick();
        }
    }
    const Det::Divergence d = Det::CompareTimelines(a, b);
    CHECK_FALSE(d.Diverged());
    CHECK(d.kind == Det::DivergenceKind::None);
    CHECK(d.tick == -1);
    CHECK(d.Describe() == "no divergence");
    // Not vacuous: the two runs really did record something.
    CHECK(a.TickCount() == 8);
    CHECK(a.EntryCount() == 16);
    CHECK(a.RunHash() == b.RunHash());
    CHECK(a.DistinctTickHashes() == 8);
}

TEST_CASE("a continuous field divergence names the tick, the record and the field",
          "[determinism][replay]")
{
    Det::StateTimeline a;
    Det::StateTimeline b;
    for (int t = 0; t < 8; ++t)
    {
        a.BeginTick();
        a.F32("body", 3u, "pos.x", static_cast<float>(t));
        a.EndTick();
        b.BeginTick();
        // One ulp, at exactly one tick, on exactly one field.
        const float base = static_cast<float>(t);
        b.F32("body", 3u, "pos.x", t == 5 ? std::nextafter(base, 1e30f) : base);
        b.EndTick();
    }

    const Det::Divergence d = Det::CompareTimelines(a, b);
    REQUIRE(d.Diverged());
    CHECK(d.kind == Det::DivergenceKind::FieldValue);
    CHECK(d.tick == 5);
    CHECK(d.entryIndex == 0);
    CHECK(d.stream == "body");
    CHECK(d.key == 3u);
    CHECK(d.field == "pos.x");
    CHECK(d.fieldKind == Det::FieldKind::Continuous);
    CHECK(d.lhsBits != d.rhsBits);
    // The ulp distance is a DIAGNOSTIC. One ulp is as divergent as a mile: the
    // verdict above is already `Diverged`, and this number did not soften it.
    CHECK(d.ulpDistance == 1);
    CHECK(d.Describe().find("tick 5") != std::string::npos);
    CHECK(d.Describe().find("pos.x") != std::string::npos);
}

TEST_CASE("a discrete field divergence is named as discrete and gets no distance",
          "[determinism][replay]")
{
    Det::StateTimeline a;
    Det::StateTimeline b;
    for (int t = 0; t < 6; ++t)
    {
        a.BeginTick();
        a.F32("unit", 2u, "pos.x", 4.0f);
        a.U32("unit", 2u, "targetId", 17u);
        a.EndTick();
        b.BeginTick();
        b.F32("unit", 2u, "pos.x", 4.0f);
        b.U32("unit", 2u, "targetId", t == 3 ? 18u : 17u);
        b.EndTick();
    }

    const Det::Divergence d = Det::CompareTimelines(a, b);
    REQUIRE(d.Diverged());
    CHECK(d.kind == Det::DivergenceKind::FieldValue);
    CHECK(d.tick == 3);
    CHECK(d.field == "targetId");
    CHECK(d.fieldKind == Det::FieldKind::Discrete);
    CHECK(d.lhsBits == 17u);
    CHECK(d.rhsBits == 18u);
    // No ulp distance is offered for a discrete outcome. There is no such thing as
    // nearly the same target, and a number here is an invitation to threshold it.
    CHECK(d.ulpDistance == -1);
    CHECK(d.Describe().find("discrete") != std::string::npos);
    CHECK(d.Describe().find("no tolerance applies") != std::string::npos);
}

TEST_CASE("a reordered emission is reported as an ordering divergence, not a value one",
          "[determinism][replay]")
{
    Det::StateTimeline a;
    Det::StateTimeline b;
    for (int t = 0; t < 5; ++t)
    {
        a.BeginTick();
        a.U32("event", 0u, "kind", 7u);
        a.U32("event", 1u, "kind", 9u);
        a.EndTick();
        b.BeginTick();
        // Same two facts, opposite order, from tick 2 on. Nothing's VALUE changed;
        // the sequence did, which is exactly the failure the gate's "event order"
        // clause is about.
        if (t >= 2)
        {
            b.U32("event", 1u, "kind", 9u);
            b.U32("event", 0u, "kind", 7u);
        }
        else
        {
            b.U32("event", 0u, "kind", 7u);
            b.U32("event", 1u, "kind", 9u);
        }
        b.EndTick();
    }

    const Det::Divergence d = Det::CompareTimelines(a, b);
    REQUIRE(d.Diverged());
    CHECK(d.kind == Det::DivergenceKind::FieldIdentity);
    CHECK(d.tick == 2);
    CHECK(d.entryIndex == 0);
    CHECK(d.key == 0u);
    CHECK(d.rhsKey == 1u);
    CHECK(d.Describe().find("ORDERING") != std::string::npos);
}

TEST_CASE("a record that appears or vanishes is reported as a shape divergence",
          "[determinism][replay]")
{
    Det::StateTimeline a;
    Det::StateTimeline b;
    for (int t = 0; t < 5; ++t)
    {
        a.BeginTick();
        a.F32("body", 0u, "pos.x", 1.0f);
        if (t == 4)
        {
            a.F32("body", 1u, "pos.x", 2.0f);
        }
        a.EndTick();
        b.BeginTick();
        b.F32("body", 0u, "pos.x", 1.0f);
        b.EndTick();
    }

    const Det::Divergence d = Det::CompareTimelines(a, b);
    REQUIRE(d.Diverged());
    CHECK(d.kind == Det::DivergenceKind::RecordShape);
    CHECK(d.tick == 4);
    CHECK(d.lhsCount == 2);
    CHECK(d.rhsCount == 1);
}

TEST_CASE("a run that ends early is a divergence, not a match", "[determinism][replay]")
{
    Det::StateTimeline a;
    Det::StateTimeline b;
    for (int t = 0; t < 7; ++t)
    {
        a.BeginTick();
        a.F32("body", 0u, "pos.x", static_cast<float>(t));
        a.EndTick();
        if (t < 5)
        {
            b.BeginTick();
            b.F32("body", 0u, "pos.x", static_cast<float>(t));
            b.EndTick();
        }
    }

    const Det::Divergence d = Det::CompareTimelines(a, b);
    REQUIRE(d.Diverged());
    CHECK(d.kind == Det::DivergenceKind::TickCount);
    CHECK(d.tick == 5);
    CHECK(d.lhsCount == 7);
    CHECK(d.rhsCount == 5);
}

TEST_CASE("the comparison is over bits, so signed zero diverges and identical NaNs do not",
          "[determinism][replay]")
{
    // The reason this class stores bits. A value comparison would call +0.0f and
    // -0.0f equal -- and they are not the same state, they are two different
    // trajectories that a solver will separate again -- while calling two identical
    // NaNs different, which would make every run with a NaN in it diverge from
    // itself.
    {
        Det::StateTimeline a;
        Det::StateTimeline b;
        a.BeginTick();
        a.F32("body", 0u, "pos.x", 0.0f);
        a.EndTick();
        b.BeginTick();
        b.F32("body", 0u, "pos.x", -0.0f);
        b.EndTick();
        const Det::Divergence d = Det::CompareTimelines(a, b);
        CHECK(d.Diverged());
        CHECK(d.tick == 0);
    }
    {
        const float nan = std::nanf("");
        Det::StateTimeline a;
        Det::StateTimeline b;
        a.BeginTick();
        a.F32("body", 0u, "pos.x", nan);
        a.EndTick();
        b.BeginTick();
        b.F32("body", 0u, "pos.x", nan);
        b.EndTick();
        CHECK_FALSE(Det::CompareTimelines(a, b).Diverged());
    }
}

// ===========================================================================
// Section 2 -- the real fixture.

TEST_CASE("the recorded physics timeline is non-degenerate", "[physics][determinism][replay]")
{
    // A record/replay harness that agrees because nothing was recorded, or because
    // nothing moved, passes for ever and means nothing. SIM-806's guard, restated
    // for a timeline: the shape of the recording, and evidence the solver acted.
    Det::StateTimeline t;
    RunInto(t);

    REQUIRE(t.TickCount() == kSteps);

    const int perTick = t.EntryCountInTick(0);
    CHECK(perTick > 0);
    for (int i = 1; i < t.TickCount(); ++i)
    {
        // Constant shape: every probe reported every tick. A body that quietly
        // stopped being readable would show up here before anything else.
        REQUIRE(t.EntryCountInTick(i) == perTick);
    }

    // Every probe key present, and both streams.
    std::set<std::uint32_t> motionKeys;
    bool                    sawDiscrete = false;
    bool                    sawContinuous = false;
    for (int i = 0; i < perTick; ++i)
    {
        const auto& e = t.EntryAt(0, i);
        if (t.Name(e.stream) == "probe")
        {
            motionKeys.insert(e.key);
        }
        sawDiscrete |= (e.kind == Det::FieldKind::Discrete);
        sawContinuous |= (e.kind == Det::FieldKind::Continuous);
    }
    CHECK(motionKeys.size() == static_cast<std::size_t>(kProbes));
    // The discrete machinery is exercised on the real fixture, not only by hand.
    CHECK(sawDiscrete);
    CHECK(sawContinuous);

    // The solver acted: nearly every tick is a distinct world, and bodies both
    // translated and rotated between the first tick and the last.
    CHECK(t.DistinctTickHashes() > static_cast<std::size_t>(kSteps * 3 / 4));

    int moved = 0;
    int rotated = 0;
    for (int i = 0; i < perTick; ++i)
    {
        const auto& first = t.EntryAt(0, i);
        const auto& last = t.EntryAt(kSteps - 1, i);
        if (first.kind != Det::FieldKind::Continuous || first.bits == last.bits)
        {
            continue;
        }
        const std::string_view f = t.Name(first.field);
        if (f == "pos.y")
        {
            ++moved;
        }
        if (f == "axisX.x" || f == "axisY.y" || f == "axisZ.z")
        {
            ++rotated;
        }
    }
    CHECK(moved >= kProbes);      // both streams report pos.y, so >= is the honest bound
    CHECK(rotated > kProbes / 2); // rotation means the SOLVER acted, not just gravity
}

TEST_CASE("two identical physics runs have no divergent boundary", "[physics][determinism][replay]")
{
    Det::StateTimeline a;
    Det::StateTimeline b;
    RunInto(a);
    RunInto(b);

    const Det::Divergence d = Det::CompareTimelines(a, b);
    INFO(d.Describe());
    CHECK_FALSE(d.Diverged());
    CHECK(a.RunHash() == b.RunHash());
    CHECK(a.EntryCount() == b.EntryCount());
}

TEST_CASE("ablating one body at one tick names that tick and that body",
          "[physics][determinism][replay]")
{
    // The whole point. A harness that cannot fail is worthless, so: perturb the
    // LIVE world -- one impulse, one body, one tick -- and require the report to
    // name it. Nothing about the recording is doctored; the simulation really did
    // do something different.
    constexpr int kAblationTick = 120;
    constexpr int kAblationProbe = 7;

    Det::StateTimeline reference;
    Det::StateTimeline perturbed;
    RunInto(reference);
    RunInto(perturbed, ImpulseAt(kAblationTick, kAblationProbe));

    const Det::Divergence d = Det::CompareTimelines(reference, perturbed);
    INFO(d.Describe());
    REQUIRE(d.Diverged());

    // The FIRST divergent boundary is the tick the impulse was applied on -- the
    // readback that follows that Step is already different.
    CHECK(d.tick == kAblationTick);
    // Not merely "they differ": the record is named. The sample stream is keyed by
    // readback order rather than by probe index, so the assertion that ties the
    // report to the ablated BODY is made against the motion stream below.
    CHECK(d.kind == Det::DivergenceKind::FieldValue);
    CHECK(d.lhsBits != d.rhsBits);
    CHECK_FALSE(d.field.empty());
    CHECK(d.Describe().find("tick " + std::to_string(kAblationTick)) != std::string::npos);

    // And the value it names is real: the two runs disagree about that exact field
    // at that exact tick, and agreed about everything before it.
    const auto& lhs = reference.EntryAt(d.tick, d.entryIndex);
    const auto& rhs = perturbed.EntryAt(d.tick, d.entryIndex);
    CHECK(lhs.bits == d.lhsBits);
    CHECK(rhs.bits == d.rhsBits);
    if (d.tick > 0)
    {
        CHECK(reference.TickHash(d.tick - 1) == perturbed.TickHash(d.tick - 1));
    }
    CHECK(reference.TickHash(d.tick) != perturbed.TickHash(d.tick));

    // Now pin it to the ablated body. Scan the motion stream at that tick for the
    // first probe key whose fields disagree; it must be probe 7 and no other.
    int firstDivergentProbe = -1;
    for (int i = 0; i < reference.EntryCountInTick(d.tick); ++i)
    {
        const auto& le = reference.EntryAt(d.tick, i);
        const auto& re = perturbed.EntryAt(d.tick, i);
        if (reference.Name(le.stream) != "probe" || le.bits == re.bits)
        {
            continue;
        }
        firstDivergentProbe = static_cast<int>(le.key);
        break;
    }
    CHECK(firstDivergentProbe == kAblationProbe);
}

TEST_CASE("an ablation at the last tick is reported at the last tick",
          "[physics][determinism][replay]")
{
    // The negative control on the negative control. The case above would also pass
    // if the comparison always reported the tick it was given, or always reported
    // an early one. Move the perturbation to the end of the run and the answer must
    // move with it -- and a divergence in the final tick must not be missed
    // because the run "mostly agreed".
    constexpr int kAblationTick = kSteps - 1;

    Det::StateTimeline reference;
    Det::StateTimeline perturbed;
    RunInto(reference);
    RunInto(perturbed, ImpulseAt(kAblationTick, 3));

    const Det::Divergence d = Det::CompareTimelines(reference, perturbed);
    INFO(d.Describe());
    REQUIRE(d.Diverged());
    CHECK(d.tick == kAblationTick);
    CHECK(d.kind == Det::DivergenceKind::FieldValue);

    // Every earlier tick genuinely agreed, so "first" means first and not "only".
    for (int i = 0; i < kAblationTick; ++i)
    {
        REQUIRE(reference.TickHash(i) == perturbed.TickHash(i));
    }

    // And the ablation was live: the whole-run hash that SIM-804 would have used
    // also changed. This harness is strictly more informative than that one, not
    // differently sensitive.
    CHECK(reference.RunHash() != perturbed.RunHash());

    // The named value is a real float, not a tag.
    if (d.fieldKind == Det::FieldKind::Continuous)
    {
        const float lv = FloatFromBits(d.lhsBits);
        const float rv = FloatFromBits(d.rhsBits);
        CHECK_FALSE(std::isnan(lv));
        CHECK_FALSE(std::isnan(rv));
    }
}

// ===========================================================================
// Section 3 -- the backend's OWN replay facility, as a second witness.
//
// box3d ships record/replay and nothing here used it (SIM-804 names it as
// unwired). It records every API call against a world, seeded by a full snapshot,
// and stamps a state hash after every step -- FNV-1a over the transform and the
// linear/angular velocity of every LIVE body, bitwise. Replay re-issues the calls
// into a fresh world at a chosen worker count and recomputes the hash per frame,
// so a mismatch names the frame.
//
// It is wired because it reaches somewhere the timeline above cannot: the replay
// world can run at a worker count the live engine never uses. SIM-804 measured our
// worker count as 1, so box3d's multi-worker solver -- which re-partitions the
// constraint graph -- has no coverage from anything that steps the live world.
//
// It is NOT the oracle, and these cases are written so nobody can mistake it for
// one. The engine's calls are baked into the recording, so replay reproduces box3d
// given inputs the engine already chose. A divergence in AI, in scripting or in an
// event ORDER is invisible here by construction.

TEST_CASE("box3d's own replay agrees with the recorded run at the same worker count",
          "[physics][determinism][replay]")
{
    auto fix = BuildFixture();
    REQUIRE(fix->backend->BeginReplayRecording());

    Det::StateTimeline live;
    Record(*fix, live, {}, 60);

    const ReplayAudit audit = fix->backend->EndReplayRecordingAndValidate(1);
    INFO("replay: frames=" << audit.frameCount << " bytes=" << audit.recordedBytes
                           << " workers=" << audit.replayWorkerCount
                           << " divergentFrame=" << audit.divergentFrame);

    CHECK(audit.supported);
    // Anti-vacuity, and the one that matters most here: a replay that validated
    // nothing -- an empty recording, a player that refused the stream, a run of
    // zero frames -- would report `diverged == false` and look like a pass.
    REQUIRE(audit.ran);
    REQUIRE(audit.frameCount >= 60);
    REQUIRE(audit.recordedBytes > 0);
    CHECK(audit.replayWorkerCount == 1);

    CHECK_FALSE(audit.diverged);
    CHECK(audit.divergentFrame == -1);

    fix->backend->Destroy();
}

TEST_CASE("box3d's replay is bit-identical at worker counts the engine never uses",
          "[physics][determinism][replay]")
{
    // The reason this arm exists. Everything else in this file, and everything in
    // SIM-804, steps a world whose worker count is 1. Replaying the SAME recorded
    // stream at 2 and 4 exercises the partitioned solver against a hash taken from
    // the single-threaded run -- which is the roadmap's "same build, same platform,
    // strict comparison" applied to the one axis nothing else can vary.
    for (const int workers : {2, 4})
    {
        auto fix = BuildFixture();
        REQUIRE(fix->backend->BeginReplayRecording());
        Det::StateTimeline live;
        Record(*fix, live, {}, 60);

        const ReplayAudit audit = fix->backend->EndReplayRecordingAndValidate(workers);
        INFO("workers=" << workers << " frames=" << audit.frameCount
                        << " divergentFrame=" << audit.divergentFrame);
        REQUIRE(audit.ran);
        REQUIRE(audit.frameCount >= 60);
        CHECK(audit.replayWorkerCount == workers);
        // Recorded at 1, replayed at N. If this ever fails, the divergentFrame it
        // reports is the first step at which the constraint-graph partition changed
        // the answer, and raising the live worker count is off the table until it
        // is explained.
        CHECK_FALSE(audit.diverged);
        CHECK(audit.divergentFrame == -1);

        fix->backend->Destroy();
    }
}

TEST_CASE("the replay audit reports honestly when nothing was recorded",
          "[physics][determinism][replay]")
{
    // A facility that returns a cheerful `diverged == false` when it did no work is
    // worse than no facility. `ran` is the field that separates "validated and
    // agreed" from "validated nothing", and the cases above REQUIRE it.
    auto fix = BuildFixture();

    const ReplayAudit audit = fix->backend->EndReplayRecordingAndValidate(1);
    CHECK(audit.supported);
    CHECK_FALSE(audit.ran);
    CHECK(audit.frameCount == 0);
    CHECK(audit.recordedBytes == 0);
    CHECK(audit.divergentFrame == -1);

    // Starting twice is refused rather than leaking the first buffer.
    REQUIRE(fix->backend->BeginReplayRecording());
    CHECK_FALSE(fix->backend->BeginReplayRecording());

    // And a Destroy while a recording is live must not leak or crash -- the
    // recording holds a pointer into the world, so the stop has to happen while
    // the world still exists.
    fix->backend->Destroy();
}
