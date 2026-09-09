// PHY-010: does a physics world actually come up, take our geometry, and step?
//
// These assert the BOUNDARY, not Box3D. Box3D has its own tests; what nobody else
// checks is that our convex pieces survive the trip, that a refusal is counted
// rather than swallowed, and that a stale handle stays dead. Those are the three
// ways this layer can be wrong while looking fine.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/Dev/Diag/ShowcaseCastle.hpp>

#include <memory>
#include <cmath>
#include <vector>

using namespace Poseidon::Physics;

TEST_CASE("Showcase castle leaves a gate and a bounded projectile budget", "[physics][showcase]")
{
    const auto blocks = Poseidon::Dev::ShowcaseCastleLayout();
    CHECK(blocks.size() == 488);
    CHECK(blocks.size() + 16 < 512);
    float maxHeight = 0;
    for (const auto& b : blocks)
    {
        CHECK_FALSE((b.z == -9 && b.height < 3.2f && b.x >= -1 && b.x <= 1));
        maxHeight = std::max(maxHeight, b.height + 0.4f);
    }
    CHECK(maxHeight > 11);
    CHECK(maxHeight < 12);
}

TEST_CASE("a newly defined probe has no launch velocity", "[physics][showcase]")
{
    SphereProbeDef def;
    REQUIRE(def.velocity[0] == 0.0f);
    REQUIRE(def.velocity[1] == 0.0f);
    REQUIRE(def.velocity[2] == 0.0f);
}

namespace
{
int waterEntries = 0;
bool FlatProbeWater(Vector3Par p, ProbeWaterSample& water)
{
    water.height = 0;
    water.velocity = Vector3(0, 0, 0);
    return p[0] >= 0;
}
void CountProbeImpact(Vector3Par, Vector3Par, float, float, bool entering)
{
    if (entering) ++waterEntries;
}
}

TEST_CASE("probe water supports displaced mass and damps fast entries", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    waterEntries = 0;
    SphereProbeDef def;
    def.velocity = Vector3(0, 0, 0);
    def.radius = 0.5f; // 524 kg of displaced fresh water when fully submerged.
    def.position = Vector3(5, 2, 0);
    def.mass = 100;
    const auto floating = backend->SpawnSphereProbe(def);
    def.position = Vector3(10, 2, 0);
    def.mass = 1000;
    const auto sinking = backend->SpawnSphereProbe(def);
    def.position = Vector3(-5, 2, 0);
    const auto dry = backend->SpawnSphereProbe(def);
    def.position = Vector3(15, 2, 0);
    def.velocity = Vector3(0, -100, 0);
    def.mass = 0.01f;
    const auto fast = backend->SpawnSphereProbe(def);
    for (int i = 0; i < 1200; ++i) backend->Step(1.0f / 60.0f);
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(floating, motion));
    CHECK(motion.position[1] > 0.1f);
    CHECK(motion.position[1] < 0.5f);
    CHECK(std::abs(motion.linearVelocity[1]) < 0.3f);
    REQUIRE(backend->GetBodyMotion(sinking, motion));
    CHECK(motion.position[1] < -5);
    REQUIRE(backend->GetBodyMotion(dry, motion));
    CHECK(motion.position[1] < -100);
    REQUIRE(backend->GetBodyMotion(fast, motion));
    CHECK(std::isfinite(motion.position[1]));
    CHECK(std::abs(motion.linearVelocity[1]) < 1);
    CHECK(waterEntries >= 3);
    CHECK(waterEntries < 20);
}

TEST_CASE("box water balance uses box volume and the requested kilograms", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    SphereProbeDef def;
    def.shape = ProbeShape::Box;
    def.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
    def.radius = 0.1f; // Unrelated sphere slider must not change the box mass.
    def.mass = 600;
    def.position = Vector3(5, 1, 0);
    def.velocity = Vector3(0, 0, 0);
    const auto box = backend->SpawnSphereProbe(def);
    for (int i = 0; i < 900; ++i) backend->Step(1.0f / 60.0f);
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(box, motion));
    CHECK(motion.position[1] > -0.15f);
    CHECK(motion.position[1] < -0.05f); // 60% of a 1 m cube submerged.
}

TEST_CASE("sideways shore entry emits without a downward throw", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    waterEntries = 0;
    SphereProbeDef def;
    def.radius = 0.5f;
    def.mass = 100;
    def.position = Vector3(-0.5f, 0.4f, 0);
    def.velocity = Vector3(5, 0, 0);
    REQUIRE(backend->SpawnSphereProbe(def).IsValid());
    for (int i = 0; i < 60; ++i) backend->Step(1.0f / 60.0f);
    CHECK(waterEntries == 1);
}

TEST_CASE("already submerged probes do not fabricate a surface impact", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    waterEntries = 0;
    SphereProbeDef def;
    def.position = Vector3(5, -10, 0);
    def.velocity = Vector3(0, -2, 0);
    REQUIRE(backend->SpawnSphereProbe(def).IsValid());
    for (int i = 0; i < 60; ++i) backend->Step(1.0f / 60.0f);
    CHECK(waterEntries == 0);
}

namespace
{

/// Unit cube corners: the smallest input that is unambiguously a volume.
std::vector<Vector3> UnitCube()
{
    return {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
}

} // namespace

TEST_CASE("physics backend creates and destroys a world", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend != nullptr);
    REQUIRE(backend->Create());
    // Idempotent: a second Create must not leak a second world.
    REQUIRE(backend->Create());
    backend->Destroy();
    // And destroying twice must not fault -- shutdown paths run more than once.
    backend->Destroy();
}

TEST_CASE("a convex piece becomes a body with a shape", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};

    const BodyId id = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(id.IsValid());

    const PhysicsStats stats = backend->GetStats();
    CHECK(stats.bodies == 1);
    CHECK(stats.shapes == 1);
    CHECK(stats.hullFailures == 0);

    backend->Destroy();
}

TEST_CASE("a piece with too few points is refused and counted", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    // A triangle is a sheet, not a hull. The point of the assertion is that the
    // refusal is VISIBLE: PHY-010's abort condition is about how much of the stock
    // corpus converts, and a silently skipped component makes a half-built world
    // indistinguishable from a working one.
    const std::vector<Vector3> triangle = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    ConvexPiece                piece{triangle.data(), static_cast<int>(triangle.size())};

    const BodyId id = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    CHECK_FALSE(id.IsValid());
    CHECK(backend->GetStats().hullFailures == 1);
    // A body with no usable shape must not be left behind as an empty one.
    CHECK(backend->GetStats().bodies == 0);

    backend->Destroy();
}

TEST_CASE("a removed body's handle stops resolving", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};

    const BodyId first = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(first.IsValid());
    backend->RemoveBody(first);
    CHECK(backend->GetStats().bodies == 0);

    // The slot is recycled, so `second` may share an index with `first`. The
    // generation counter is what keeps them distinct -- without it, moving the
    // stale handle would move the NEW body, and nothing would report an error.
    const BodyId second = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(second.IsValid());
    CHECK(second != first);

    backend->SetBodyTransform(first, Matrix4(MIdentity)); // must be a no-op, not a crash
    backend->RemoveBody(first);                           // likewise
    CHECK(backend->GetStats().bodies == 1);

    backend->Destroy();
}

TEST_CASE("a heightfield is accepted and a degenerate one is not", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    constexpr int      N = 8;
    std::vector<float> heights(N * N, 0.0f);
    for (int z = 0; z < N; ++z)
    {
        for (int x = 0; x < N; ++x)
        {
            heights[z * N + x] = static_cast<float>(x) * 0.5f; // a ramp, so it is not a plane
        }
    }

    TerrainField field;
    field.heights = heights.data();
    field.width = N;
    field.height = N;
    field.cellSize = 50.0f; // OFP's landscape grid
    field.originX = 0.0f;
    field.originZ = 0.0f;
    REQUIRE(backend->SetTerrain(field));

    const PhysicsStats stats = backend->GetStats();
    CHECK(stats.terrainRegistered);
    CHECK(stats.terrainWidth == N);
    CHECK(stats.terrainHeight == N);

    // A single row is not a surface.
    TerrainField tooSmall = field;
    tooSmall.height = 1;
    CHECK_FALSE(backend->SetTerrain(tooSmall));

    backend->Destroy();
}

TEST_CASE("removing a body gives its shapes back to the count", "[physics]")
{
    // Regression: _shapeCount only ever counted up. b3DestroyBody takes the
    // body's shapes with it silently, so after any removal -- streaming, a
    // mission reload -- the diagnostic reported more shapes than existed. A
    // number that can only grow is not a diagnostic.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    // Two pieces on one body, so the test would still fail if removal subtracted
    // a flat 1 per body instead of what the body actually owned.
    const ConvexPiece pieces[2] = {
        {cube.data(), static_cast<int>(cube.size())},
        {cube.data(), static_cast<int>(cube.size())},
    };

    const BodyId id = backend->AddStaticBody(pieces, 2, Matrix4(MIdentity));
    REQUIRE(id.IsValid());
    CHECK(backend->GetStats().shapes == 2);

    backend->RemoveBody(id);
    CHECK(backend->GetStats().bodies == 0);
    CHECK(backend->GetStats().shapes == 0);

    backend->Destroy();
}

TEST_CASE("a destroy/create cycle does not reuse stale slots", "[physics]")
{
    // Regression: Destroy() cleared _slots but not _free, so the next Store()
    // popped a stale index and indexed an empty vector. Silent memory corruption
    // one mission reload away, and nothing in the earlier tests touched it because
    // they never destroyed and recreated the same backend.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};

    const BodyId first = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(first.IsValid());
    backend->RemoveBody(first); // leaves a free slot behind

    backend->Destroy();
    REQUIRE(backend->Create());

    // Counters must start from zero too, or two worlds accumulate into one report.
    CHECK(backend->GetStats().bodies == 0);
    CHECK(backend->GetStats().shapes == 0);
    CHECK(backend->GetStats().hullFailures == 0);
    CHECK_FALSE(backend->GetStats().terrainRegistered);

    const BodyId afterCycle = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(afterCycle.IsValid());
    CHECK(backend->GetStats().bodies == 1);

    backend->Destroy();
}

TEST_CASE("terrain survives a replacement and a destroy", "[physics]")
{
    // b3CreateHeightFieldShape holds a REFERENCE to the height field for the
    // lifetime of the shape, so the world must be destroyed before the field.
    // This exercises both orders: replace-while-live, then destroy.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    constexpr int      N = 8;
    std::vector<float> heights(N * N);
    for (int i = 0; i < N * N; ++i)
    {
        heights[static_cast<std::size_t>(i)] = static_cast<float>(i % N) * 0.25f;
    }

    TerrainField field;
    field.heights = heights.data();
    field.width = N;
    field.height = N;
    field.cellSize = 50.0f;
    field.originX = 0.0f;
    field.originZ = 0.0f;

    REQUIRE(backend->SetTerrain(field));
    REQUIRE(backend->SetTerrain(field)); // replace the live one
    backend->Step(1.0f / 60.0f);
    CHECK(backend->GetStats().terrainRegistered);

    backend->Destroy();
    CHECK_FALSE(backend->GetStats().terrainRegistered);
}

TEST_CASE("a world with no backend answers safely", "[physics]")
{
    // PhysicsWorld must be usable before Create() and after Destroy(): the engine
    // is allowed to run with no physics at all, and every call has to be inert
    // rather than a null dereference waiting for the first caller who forgets.
    PhysicsWorld world;
    CHECK_FALSE(world.IsCreated());
    CHECK(std::string(world.BackendName()) == "none");
    world.Step(1.0f / 60.0f);
    CHECK_FALSE(world.RegisterStatic(nullptr, Matrix4(MIdentity)).IsValid());
    CHECK_FALSE(world.SetTerrain(nullptr, 0, 0, 0.0f, 0.0f, 0.0f));
    CHECK(world.GetStats().bodies == 0);
}

TEST_CASE("removing a probe body individually leaves no dangling readback", "[physics]")
{
    // The path nothing called. `ClearProbes()` removes probes wholesale and is
    // what the dev panel uses, so `RemoveBody()` on a body that is ALSO a probe
    // had never run -- yet it is exactly what `LooseObjects::Release` does, via
    // `PhysicsWorld::Remove`, from `Thing::~Thing`.
    //
    // Before the fix this crashed: `RemoveBody` freed the slot and destroyed the
    // body but left the entry in `_probes`, and `GetProbeSamples` calls
    // b3Body_GetTransform on every entry without a validity check. Three probes,
    // remove the middle one, next readback = SIGSEGV.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    std::vector<BodyId> ids;
    for (int i = 0; i < 3; ++i)
    {
        SphereProbeDef def;
        def.position = Vector3(0.0f, 5.0f + static_cast<float>(i), 0.0f);
        def.radius = 0.2f;
        const BodyId id = backend->SpawnSphereProbe(def);
        REQUIRE(id.IsValid());
        ids.push_back(id);
    }
    REQUIRE(backend->GetStats().probes == 3);
    REQUIRE(backend->GetStats().bodies == 3);

    backend->RemoveBody(ids[1]);

    // The count is the cheap tell, and it was wrong before anything crashed.
    CHECK(backend->GetStats().bodies == 2);
    CHECK(backend->GetStats().probes == 2);

    std::vector<ProbeSample> samples;
    backend->GetProbeSamples(samples); // used to fault here
    REQUIRE(samples.size() == 2);
    // Every surviving sample still resolves to a live slot. A sample whose id
    // came back invalid would mean the entry outlived its slot -- the same defect
    // wearing a quieter symptom.
    for (const ProbeSample& s : samples)
    {
        CHECK(s.id.IsValid());
        BodyMotion motion;
        CHECK(backend->GetBodyMotion(s.id, motion));
    }
    CHECK_FALSE(samples[0].id == ids[1]);
    CHECK_FALSE(samples[1].id == ids[1]);

    // Stepping must not touch the removed body either -- `Step` walks `_probes`
    // to apply wind.
    WindSettings wind;
    wind.enabled = true;
    wind.velocity = Vector3(5.0f, 0.0f, 0.0f);
    backend->SetWind(wind);
    backend->Step(1.0f / 60.0f);
    CHECK(backend->GetStats().probes == 2);

    // The quieter half: Box3D reuses body ids, so a stale `_probes` entry can
    // come to ALIAS a body created later. `ClearProbes` destroys everything in
    // `_probes`, so the alias would delete a collider nobody asked it to touch.
    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};
    Matrix4                    at(MIdentity);
    at.SetPosition(Vector3(20.0f, 1.0f, 20.0f));
    const BodyId building = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(building.IsValid());

    // A step before the ray: Box3D holds a newly created proxy in a move buffer
    // until the next broadphase update, so a cast straight after AddStaticBody
    // misses a body that is genuinely there.
    backend->Step(1.0f / 60.0f);

    Vector3       hit;
    const Vector3 from(20.5f, 10.0f, 20.5f);
    const Vector3 to(20.5f, -5.0f, 20.5f);
    REQUIRE(backend->CastRay(from, to, hit, QueryFilter{}) == building);

    backend->ClearProbes();
    CHECK(backend->GetStats().probes == 0);
    // Still there. This is the assertion the whole test exists for.
    CHECK(backend->CastRay(from, to, hit, QueryFilter{}) == building);

    backend->Destroy();
}

TEST_CASE("a ray identifies a body that moved into a recycled slot", "[physics]")
{
    // `CastRay` is the only place that read the generation counter as a live/dead
    // PARITY flag -- slots start at 1, `RemoveBody` increments, so a freed slot is
    // even. `Store` then reuses that slot WITHOUT incrementing again, so the next
    // occupant keeps the even generation and this refused to name it. The ray hit;
    // the answer came back "no body".
    //
    // It matters because the answer feeds `ShotShell::TryPenetrate` through
    // `MeasureThickness`, and because a slot is freed by every `ClearProbes()` and
    // every `LooseObjects::Release`. Nothing in the suite removed a body and then
    // cast a ray at its replacement, which is why an inconsistency between `Find()`
    // (exact compare, always right) and this (parity, wrong on every reuse) sat
    // there unnoticed.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};
    Matrix4                    at(MIdentity);
    at.SetPosition(Vector3(20.0f, 1.0f, 20.0f));

    // First occupant of slot 0. A ray names it -- this always worked.
    const BodyId first = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(first.IsValid());
    backend->Step(1.0f / 60.0f); // let the broadphase take the new proxy

    Vector3       hit;
    const Vector3 from(20.5f, 10.0f, 20.5f);
    const Vector3 to(20.5f, -5.0f, 20.5f);
    REQUIRE(backend->RayHitAnything(from, to, QueryFilter{}));
    REQUIRE(backend->CastRay(from, to, hit, QueryFilter{}) == first);

    // Free the slot and put a different body in it.
    backend->RemoveBody(first);
    CHECK_FALSE(backend->RayHitAnything(from, to, QueryFilter{}));

    const BodyId second = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(second.IsValid());
    CHECK(second != first);          // the generation moved on
    CHECK(second.index == first.index); // and it is the same slot, so this is the reuse case
    backend->Step(1.0f / 60.0f);

    // The ray still hits -- that half was never broken, which is what made the
    // other half hard to see.
    REQUIRE(backend->RayHitAnything(from, to, QueryFilter{}));
    // And it names the CURRENT occupant. This is the assertion that used to fail.
    CHECK(backend->CastRay(from, to, hit, QueryFilter{}) == second);
    // Never the stale handle.
    CHECK_FALSE(backend->CastRay(from, to, hit, QueryFilter{}) == first);

    // Third occupant, so the fix is not merely "even generations also work".
    backend->RemoveBody(second);
    const BodyId third = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(third.IsValid());
    backend->Step(1.0f / 60.0f);
    CHECK(backend->CastRay(from, to, hit, QueryFilter{}) == third);

    backend->Destroy();
}
