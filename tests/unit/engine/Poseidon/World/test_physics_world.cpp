// PHY-010: does a physics world actually come up, take our geometry, and step?
//
// These assert the BOUNDARY, not Box3D. Box3D has its own tests; what nobody else
// checks is that our convex pieces survive the trip, that a refusal is counted
// rather than swallowed, and that a stale handle stays dead. Those are the three
// ways this layer can be wrong while looking fine.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>

#include <memory>
#include <vector>

using namespace Poseidon::Physics;

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
