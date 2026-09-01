// Flag-tagged colliders: does one collider set actually answer more than one
// question?
//
// The mechanism these assert is that the filter runs INSIDE the cast. A filter
// applied to the result of an unfiltered cast would pass the easy test -- ask for
// the only thing there and get it -- and fail the only one that matters: when a
// collider the query did not ask about stands in front of one it did, the answer
// must be the far one, not a miss. Every test here is built so that a post-filter
// implementation gives a visibly wrong answer.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>

#include <memory>
#include <vector>

using namespace Poseidon::Physics;

namespace
{

/// An axis-aligned box from x0..x1, spanning 0..1 in y and z. A volume, so the
/// hull is never the thing under test.
std::vector<Vector3> BoxFromTo(float x0, float x1)
{
    return {
        {x0, 0, 0}, {x1, 0, 0}, {x1, 1, 0}, {x0, 1, 0},
        {x0, 0, 1}, {x1, 0, 1}, {x1, 1, 1}, {x0, 1, 1},
    };
}

/// Straight down the middle of everything BoxFromTo builds.
const Vector3 RayFrom(-5.0f, 0.5f, 0.5f);
const Vector3 RayTo(10.0f, 0.5f, 0.5f);

} // namespace

TEST_CASE("collider flags are the LOD kinds, and All covers each of them", "[physics]")
{
    // Cheap, but it is what stops a fifth kind being added to the enum and
    // silently left out of All -- at which case every default query would start
    // missing it and nothing else here would notice.
    CHECK(HasAny(ColliderFlags::All, ColliderFlags::Solid));
    CHECK(HasAny(ColliderFlags::All, ColliderFlags::BulletCollision));
    CHECK(HasAny(ColliderFlags::All, ColliderFlags::ViewBlocking));
    CHECK(HasAny(ColliderFlags::All, ColliderFlags::Roadway));
    CHECK_FALSE(HasAny(ColliderFlags::Solid, ColliderFlags::BulletCollision));
    CHECK_FALSE(HasAny(ColliderFlags::None, ColliderFlags::All));
    // A default piece is the undifferentiated collider the world had before flags,
    // so a caller that never heard of them keeps getting exactly that.
    CHECK(ConvexPiece{}.flags == ColliderFlags::Solid);
}

TEST_CASE("a filtered ray passes through the colliders it did not ask for", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    // NEAR body: view geometry only -- something that breaks line of sight and
    // stops nothing. FAR body: fire geometry. A shot fired along this line must
    // reach the far one.
    const std::vector<Vector3> near = BoxFromTo(0.0f, 1.0f);
    const std::vector<Vector3> far = BoxFromTo(3.0f, 4.0f);
    ConvexPiece                nearPiece{near.data(), static_cast<int>(near.size()), ColliderFlags::ViewBlocking};
    ConvexPiece                farPiece{far.data(), static_cast<int>(far.size()), ColliderFlags::BulletCollision};

    const BodyId nearBody = backend->AddStaticBody(&nearPiece, 1, Matrix4(MIdentity));
    const BodyId farBody = backend->AddStaticBody(&farPiece, 1, Matrix4(MIdentity));
    REQUIRE(nearBody.IsValid());
    REQUIRE(farBody.IsValid());
    REQUIRE(nearBody != farBody);

    Vector3 hit;
    // Unfiltered: the nearest surface, which is the view box.
    CHECK(backend->CastRay(RayFrom, RayTo, hit, QueryFilter{}) == nearBody);

    // The shot. If the filter were applied after an unfiltered cast this would be
    // an invalid id -- the closest hit was the view box, and discarding it is not
    // the same as never having seen it.
    CHECK(backend->CastRay(RayFrom, RayTo, hit, QueryFilter{ColliderFlags::BulletCollision}) == farBody);
    CHECK(hit[0] >= 3.0f);

    // And the sight line stops at the near one.
    CHECK(backend->CastRay(RayFrom, RayTo, hit, QueryFilter{ColliderFlags::ViewBlocking}) == nearBody);

    // A kind nothing here carries is a miss, not a fallback to "anything".
    CHECK_FALSE(backend->CastRay(RayFrom, RayTo, hit, QueryFilter{ColliderFlags::Roadway}).IsValid());
    CHECK_FALSE(backend->RayHitAnything(RayFrom, RayTo, QueryFilter{ColliderFlags::Roadway}));
    CHECK(backend->RayHitAnything(RayFrom, RayTo, QueryFilter{ColliderFlags::BulletCollision}));

    backend->Destroy();
}

TEST_CASE("one body carries pieces of different kinds", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    // The case the flags exist for: a model that authored a Fire Geometry LOD
    // separate from its Geometry LOD registers both, on ONE body, and a query
    // picks. A per-body flag word could not express this.
    const std::vector<Vector3> solid = BoxFromTo(0.0f, 1.0f);
    const std::vector<Vector3> fire = BoxFromTo(3.0f, 4.0f);
    const ConvexPiece          pieces[] = {
        {solid.data(), static_cast<int>(solid.size()), ColliderFlags::Solid},
        {fire.data(), static_cast<int>(fire.size()), ColliderFlags::BulletCollision},
    };

    const BodyId body = backend->AddStaticBody(pieces, 2, Matrix4(MIdentity));
    REQUIRE(body.IsValid());

    Vector3 hit;
    CHECK(backend->CastRay(RayFrom, RayTo, hit, QueryFilter{ColliderFlags::Solid}) == body);
    CHECK(hit[0] <= 1.001f);
    CHECK(backend->CastRay(RayFrom, RayTo, hit, QueryFilter{ColliderFlags::BulletCollision}) == body);
    // Same body, different surface. Without a per-piece filter both rays would
    // return the same point and the distinction would be decorative.
    CHECK(hit[0] >= 3.0f);

    backend->Destroy();
}

TEST_CASE("query-only shapes are counted and given back", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> box = BoxFromTo(0.0f, 1.0f);
    ConvexPiece                solid{box.data(), static_cast<int>(box.size()), ColliderFlags::Solid};
    ConvexPiece                viewOnly{box.data(), static_cast<int>(box.size()), ColliderFlags::ViewBlocking};

    const BodyId solidBody = backend->AddStaticBody(&solid, 1, Matrix4(MIdentity));
    REQUIRE(solidBody.IsValid());
    // A Geometry-LOD collider is not query-only: it takes part in contact exactly
    // as it did before flags existed.
    CHECK(backend->GetStats().queryOnlyShapes == 0);

    const BodyId viewBody = backend->AddStaticBody(&viewOnly, 1, Matrix4(MIdentity));
    REQUIRE(viewBody.IsValid());
    CHECK(backend->GetStats().shapes == 2);
    CHECK(backend->GetStats().queryOnlyShapes == 1);

    // Given back on removal. A running total that only counts up stops being a
    // measurement the first time anything is streamed out -- the same trap the
    // shape count already had.
    backend->RemoveBody(viewBody);
    CHECK(backend->GetStats().shapes == 1);
    CHECK(backend->GetStats().queryOnlyShapes == 0);

    backend->Destroy();
}

TEST_CASE("a query-only collider takes part in no contact", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    // A view-geometry box with a sphere dropped onto it. It answers a sight ray,
    // so it is really there -- and the sphere must fall straight through it. If
    // query-only shapes collided, every model with a coarse View Geometry LOD
    // would gain an invisible wall around it.
    const std::vector<Vector3> box = {
        {-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2},
        {-2, 1, -2}, {2, 1, -2}, {2, 1, 2}, {-2, 1, 2},
    };
    ConvexPiece viewOnly{box.data(), static_cast<int>(box.size()), ColliderFlags::ViewBlocking};
    REQUIRE(backend->AddStaticBody(&viewOnly, 1, Matrix4(MIdentity)).IsValid());

    // It is there as far as a query is concerned.
    Vector3 hit;
    CHECK(backend->RayHitAnything(Vector3(0, 10, 0), Vector3(0, -10, 0), QueryFilter{ColliderFlags::ViewBlocking}));
    (void)hit;

    SphereProbeDef def;
    def.position = Vector3(0.0f, 5.0f, 0.0f);
    def.radius = 0.2f;
    const BodyId probe = backend->SpawnSphereProbe(def);
    REQUIRE(probe.IsValid());

    // Long enough to fall well past the box, at a step small enough that nothing
    // tunnels for reasons unrelated to filtering. Fixed count and fixed step: no
    // wall clock reaches this, so the outcome is the same on every machine.
    for (int i = 0; i < 240; ++i)
    {
        backend->Step(1.0f / 60.0f);
    }

    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(probe, motion));
    CHECK(motion.position[1] < -1.0f);

    backend->Destroy();
}
